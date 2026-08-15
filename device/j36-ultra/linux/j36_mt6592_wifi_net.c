// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi, stage 4b: cfg80211 and wlan0.
 *
 * The last of the five, and the only one that knows what a network interface is.
 * Below it j36_mt6592_wifi_cmd.c speaks the firmware's protocol and has never
 * heard of cfg80211; above it wpa_supplicant and NetworkManager are already
 * running on this rootfs and are waiting for an interface to appear.
 *
 *
 * ── FULLMAC, AND WHAT THAT MOVES WHERE ──
 *
 * The firmware owns the MAC.  There is no mac80211 in this build and there is
 * nothing for it to do: the radio answers probe requests, keeps the station
 * record, tracks the BSS and does the CCMP once it has a key.  So the division is
 * the ordinary fullmac one, in three parts rather than the usual two:
 *
 *   the firmware	beacons, ACKs, retries, rate control, encryption
 *   this driver	the scan, open-system auth, association, key installs,
 *			and an Ethernet frame in each direction
 *   wpa_supplicant	the PSK, the four-way handshake, the saved networks
 *
 * The middle row is a REAL state machine and not a pass-through, because the
 * firmware's join is a sequence of commands with events between them: a channel
 * privilege has to be granted before the authentication frame goes out, the
 * station record has to exist before the association response is answered, and
 * the record is not valid until the firmware says so.  None of that is visible
 * from cfg80211, whose .connect is one call.
 *
 *
 * ── WHY THIS POLLS ──
 *
 * The wifi node in this board's device tree has no interrupts property, because
 * nothing has established which GIC line the CONSYS HIF raises -- stage 3's whole
 * bring-up polls for the same reason.  So there is one work item that reads the
 * receive port, drains the transmit queue and checks the join's deadlines, and it
 * reschedules itself at a delay that depends on what is going on: immediately
 * after a productive poll, one jiffy while anything is in flight, and fiftyish
 * milliseconds when the interface is up and idle.
 *
 * Which makes the transmit queue below load-bearing rather than decoration.
 * ndo_start_xmit is called with a spinlock held and may not sleep; w->lock is a
 * mutex and the page accounting under it sleeps for up to 200 ms when the
 * firmware's own queue is full.  So the two cannot meet, and a transmitted frame
 * goes on an sk_buff_head that the poll worker drains.
 *
 *
 * ── THE ONE ORDERING THE SUPPLICANT CANNOT DO ITSELF ──
 *
 * wpa_supplicant writes message 4 of the handshake to wlan0 and then immediately
 * asks for the pairwise key.  It has no way to know when that frame actually left
 * the radio, and if the key is installed first the firmware encrypts a frame the
 * AP can only read in the clear.  So .add_key drains this queue before it sends
 * the key command, and the key command itself waits on the firmware's own
 * transmit status -- see j36_wlan_cmd_install_key().
 */

#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <net/cfg80211.h>

#include "j36_mt6592_wifi.h"

/*
 * How deep the transmit queue is allowed to get before the stack is told to stop,
 * and where it is let go again.  128 frames is about 190 KB and roughly half a
 * second of this radio, which is enough to ride out the 200 ms the page
 * accounting can spend waiting without dropping a TCP window, and short enough
 * that a stalled radio is felt as backpressure rather than as latency.
 */
#define J36_WLAN_TX_QUEUE_STOP		128
#define J36_WLAN_TX_QUEUE_WAKE		64
/* Bounded because the drain holds w->lock, which .connect and .scan want. */
#define J36_WLAN_MAX_TX_PER_POLL	16
#define J36_WLAN_IDLE_POLL_MS		50

/*
 * How many polls in a row the frame at the head of the queue may be refused a
 * transmit page before it is dropped instead of retried.
 *
 * Putting a refused frame back at the head and stopping is the right answer to a
 * SHORTAGE: the page comes back a moment later and the queue keeps its order,
 * which is what TCP and an EAPOL exchange both want.  It is the wrong answer to a
 * class that will never be credited at all, and this driver spent three boots
 * proving it -- a single broadcast frame sent to a traffic class the firmware
 * does not service in station mode took that class's only page, and every other
 * frame on the interface queued behind it for ever.  Nothing above could tell:
 * ndo_start_xmit kept returning NETDEV_TX_OK, the carrier stayed up, and the
 * counters showed a queue that never drained and no errors at all.
 *
 * This used to be a count of polls, and it could be: each acquire spent up to
 * 200 ms of its own inside w->lock, so eight of them was a page that had not come
 * back in a second and a half.  The data path no longer sleeps for a page at all
 * -- see J36_HIF_TX_TRY_ROUNDS, and the reason there is the lock -- so a poll is
 * now a jiffy and eight of them is eighty milliseconds, which is a shortage and
 * not a stall.  The wall clock says what was always meant.
 */
#define J36_WLAN_TX_STALL_MS		1500u

/*
 * How many firmware operations may go unanswered in a row before the AIS network
 * is re-armed, and how many re-arms before the radio is declared lost.
 *
 * "Unanswered" is narrow on purpose: a channel privilege that was never granted
 * and a scan that never reported SCAN_DONE.  Both are commands the firmware
 * accepted -- the page accounting says so -- and then did nothing about, and an
 * authentication or association that times out is NOT counted, because that is
 * the AP not answering and has nothing to do with the state of the chip.
 *
 * Two, because one of either is ordinary: an AP can vanish between the scan that
 * found it and the connect that names it, and a sweep can collide with something
 * else on the channel.  Two in a row is a firmware whose AIS network is no longer
 * live, which this driver has now watched happen -- eighteen consecutive scans
 * submitted, credited and abandoned on their own deadlines over three minutes,
 * with not one message from the driver to say so.
 */
#define J36_WLAN_SILENCE_RECOVER	2
#define J36_WLAN_RECOVER_LIMIT		2

/* 1..13.  Channel 14 is 802.11b-only and Japan-only; the firmware's own domain
 * table stops at 13 and so does this. */
#define J36_WLAN_CHANNELS		13
#define J36_WLAN_RATES			12

enum j36_wlan_state {
	J36_WLAN_IDLE = 0,
	J36_WLAN_CHANNEL,	/* CH_PRIVILEGE sent, waiting for the grant	*/
	J36_WLAN_AUTH,		/* authentication frame out			*/
	J36_WLAN_ASSOC,		/* association request out			*/
	J36_WLAN_CONNECTED,	/* associated; keys may still be arriving	*/
};

struct j36_wlan {
	struct j36_wifi *w;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *ndev;

	/*
	 * The band is per-device and not a shared static on purpose: applying a
	 * regulatory domain WRITES the channel flags, so a static table would be
	 * one radio's rules in another radio's wiphy.  There is one radio on this
	 * board, which is exactly the sort of reasoning that stops being true
	 * quietly.
	 */
	struct ieee80211_supported_band band;
	struct ieee80211_channel channels[J36_WLAN_CHANNELS];
	struct ieee80211_rate rates[J36_WLAN_RATES];

	struct workqueue_struct *wq;
	struct delayed_work poll;
	struct sk_buff_head tx_queue;

	bool running;
	enum j36_wlan_state state;
	ktime_t deadline;

	/* The join. */
	struct j36_wlan_bss bss;
	bool secure;
	bool reported;		/* cfg80211 has been told the connection took */
	bool ptk_ready;
	bool gtk_ready;
	bool key_ready;
	bool pm_sent;
	/* When INDICATE_PM_CONNECTED stops waiting for a beacon to supply the real
	 * DTIM period and goes out with an assumed one.  Zero until the indication
	 * first becomes sendable, so a slow handshake spends its time waiting for
	 * keys rather than spending the beacon window.  See j36_wlan_try_pm(). */
	ktime_t pm_deadline;
	bool channel_held;
	u8 channel_token;
	u8 sta_index;
	/* The firmware confirmed the record with ACTIVATE_STA_REC. Reporting only:
	 * the data path does NOT wait for it -- see j36_wlan_on_sta_active(). */
	bool sta_confirmed;
	u16 aid;
	ktime_t gtk_deadline;
	u8 assoc_ies[J36_WLAN_MAX_IES];
	u16 assoc_ies_len;
	u8 resp_ies[J36_WLAN_MAX_IES];
	u16 resp_ies_len;

	/* The scan. */
	struct cfg80211_scan_request *scan_request;
	u8 scan_sequence;
	ktime_t scan_deadline;

	/*
	 * What we have heard, in the firmware's terms.  Not a second copy of
	 * cfg80211's BSS table for its own sake: SET_BSS_INFO and
	 * UPDATE_STA_RECORD want rate BITMAPS and a DTIM period, and
	 * cfg80211_get_bss() hands back IEs and a signal and expects the driver
	 * to have kept whatever else it needs.
	 */
	struct j36_wlan_bss results[J36_WLAN_MAX_SCAN_RESULTS];
	unsigned int result_count;

	u32 scans;
	u32 joins;
	u32 join_failures;
	u32 link_losses;
	/* Beacons of the BSS this interface is associated to, counted separately
	 * from every other management frame because the difference between the two
	 * is a whole class of fault.  A link that is up and hearing its own AP has
	 * a number here that climbs by ten a second; a link that is up and hearing
	 * nothing has a zero, and that zero is why INDICATE_PM_CONNECTED has to be
	 * willing to assume a DTIM period rather than wait for one. */
	u32 beacons;
	/* Consecutive firmware operations that were accepted and never answered,
	 * and how many times that count has been acted on.  See j36_wlan_recover(). */
	u32 silence;
	u32 recoveries;
	u32 tx_backpressure;
	u32 tx_deferred;
	/* Consecutive polls the head of the queue has been refused a page, and when
	 * the run started.  Both reset by anything at all going out, so they count
	 * a stall and not traffic. */
	u32 tx_stalls;
	ktime_t tx_stall_since;
	/* And how many times that ran out and a frame was dropped for it, which
	 * is cumulative and is the one of the two worth printing. */
	u32 tx_starved;
};

/* ── the wiphy's fixed tables ────────────────────────────────────────────────*/

static const u16 j36_wlan_channel_freq[J36_WLAN_CHANNELS] = {
	2412, 2417, 2422, 2427, 2432, 2437, 2442,
	2447, 2452, 2457, 2462, 2467, 2472,
};

/*
 * hw_value is MediaTek's own rate encoding -- the 500 kb/s units an 802.11
 * supported-rates element carries -- so that anything reading a rate back out of
 * this table gets the number the firmware would have used.
 */
static const struct ieee80211_rate j36_wlan_rate_table[J36_WLAN_RATES] = {
	{ .bitrate =  10, .hw_value =   2 },
	{ .bitrate =  20, .hw_value =   4,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate =  55, .hw_value =  11,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value =  22,
	  .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate =  60, .hw_value =  12 },
	{ .bitrate =  90, .hw_value =  18 },
	{ .bitrate = 120, .hw_value =  24 },
	{ .bitrate = 180, .hw_value =  36 },
	{ .bitrate = 240, .hw_value =  48 },
	{ .bitrate = 360, .hw_value =  72 },
	{ .bitrate = 480, .hw_value =  96 },
	{ .bitrate = 540, .hw_value = 108 },
};

static const u32 j36_wlan_cipher_suites[] = {
	WLAN_CIPHER_SUITE_CCMP,
};

/*
 * The address a GROUP key is filed under in CMD_802_11_KEY.
 *
 * It is the broadcast address and not the AP's, because what the firmware matches
 * it against is the destination of a group-addressed frame.  cfg80211 says the
 * same thing by omission -- .add_key gets a mac_addr for a pairwise key and NULL
 * for a group one -- and reading that NULL as "no peer, use the BSSID" is what
 * used to make every broadcast the AP sent undecryptable.  See .add_key.
 */
static const u8 j36_wlan_group_addr[ETH_ALEN] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/*
 * WPA2-PSK and nothing else, said out loud rather than left for a connect to
 * discover.  The key command this driver sends carries CIPHER_SUITE_CCMP and a
 * sixteen-byte key, so TKIP and WEP are not near-misses that might work -- and
 * advertising an AKM here is what lets wpa_supplicant pick something else before
 * it has committed a user to a network that cannot come up.
 */
static const u32 j36_wlan_akm_suites[] = {
	WLAN_AKM_SUITE_PSK,
};

/*
 * A custom regulatory domain rather than a hint, because the alternative is
 * CONFIG_CFG80211_INTERNAL_REGDB or a regulatory.db in the initramfs, and this
 * boot partition has about two and a half megabytes of slack.  The rule is the
 * 2.4 GHz band at 20 dBm, which is the intersection of every domain the firmware
 * itself was given in j36_wlan_cmd_configure() -- the radio is the thing actually
 * enforcing this, and it was told first.
 */
static const struct ieee80211_regdomain j36_wlan_regdom = {
	.n_reg_rules = 1,
	.alpha2 = "99",
	.reg_rules = {
		REG_RULE(2402, 2482, 40, 0, 20, 0),
	},
};

/* ── small shared helpers ────────────────────────────────────────────────────*/

static u16 j36_get_le16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

/* memcmp and not ether_addr_equal(): the frame pointers here come out of the
 * receive buffer at an offset the firmware chose, so nothing guarantees the
 * u16 alignment that one needs. */
static bool j36_same_mac(const u8 *a, const u8 *b)
{
	return !memcmp(a, b, ETH_ALEN);
}

static struct j36_wlan *j36_wlan_from_netdev(struct net_device *ndev)
{
	return *(struct j36_wlan **)netdev_priv(ndev);
}

static struct ieee80211_channel *j36_wlan_channel(struct j36_wlan *wlan,
						  u8 channel)
{
	int freq = ieee80211_channel_to_frequency(channel, NL80211_BAND_2GHZ);

	if (freq <= 0)
		return NULL;
	return ieee80211_get_channel(wlan->wiphy, freq);
}

/* The firmware reports RCPI, which is twice the power in dBm above -110. */
static s16 j36_wlan_rcpi_dbm(u8 rcpi)
{
	return (s16)(rcpi / 2) - 110;
}

static void j36_wlan_kick(struct j36_wlan *wlan)
{
	if (wlan->running)
		mod_delayed_work(wlan->wq, &wlan->poll, 0);
}

/* ── what a beacon says ──────────────────────────────────────────────────────
 *
 * Every element this driver needs and not one more.  The rate bytes are kept raw
 * as well as reduced to bitmaps, because the association request has to echo them
 * back in a supported-rates element and a bitmap cannot be turned back into the
 * bytes it came from.
 */
static void j36_wlan_parse_ies(struct j36_wlan_bss *bss, const u8 *ies,
			       u32 ies_len)
{
	u32 offset = 0;

	while (offset + 2 <= ies_len) {
		const u8 id = ies[offset];
		const u8 body_len = ies[offset + 1];
		const u8 *body = ies + offset + 2;
		u8 i;

		if (offset + 2 + body_len > ies_len)
			break;

		switch (id) {
		case WLAN_EID_SSID:
			bss->ssid_len = min_t(u8, body_len,
					      IEEE80211_MAX_SSID_LEN);
			memcpy(bss->ssid, body, bss->ssid_len);
			break;
		case WLAN_EID_SUPP_RATES:
		case WLAN_EID_EXT_SUPP_RATES:
			for (i = 0; i < body_len; i++) {
				if (bss->rate_count < ARRAY_SIZE(bss->rates))
					bss->rates[bss->rate_count++] = body[i];
			}
			break;
		case WLAN_EID_DS_PARAMS:
			if (body_len >= 1)
				bss->channel = body[0];
			break;
		case WLAN_EID_TIM:
			/* Element 5 is [DTIM count][DTIM period][...]: the
			 * PERIOD is what INDICATE_PM_BSS_CONNECTED wants, and
			 * the count is where in the cycle this beacon fell. */
			if (body_len >= 2)
				bss->dtim_period = body[1];
			break;
		default:
			break;
		}
		offset += 2 + body_len;
	}
}

/*
 * The rate bitmaps the firmware's commands take, derived from the raw bytes.
 * Kept here rather than in the command layer because it is the beacon's units
 * being converted and not the command's: bit 7 of a rate byte is 802.11's own
 * "this rate is basic" flag, which is why a basic rate is also an operational
 * one and the two masks overlap.
 */
static u16 j36_wlan_rate_bit(u8 rate)
{
	switch (rate & 0x7f) {
	case 2:		return BIT(0);
	case 4:		return BIT(1);
	case 11:	return BIT(2);
	case 22:	return BIT(3);
	case 44:	return BIT(4);
	case 66:	return BIT(5);
	case 12:	return BIT(6);
	case 18:	return BIT(7);
	case 24:	return BIT(8);
	case 36:	return BIT(9);
	case 48:	return BIT(10);
	case 72:	return BIT(11);
	case 96:	return BIT(12);
	case 108:	return BIT(13);
	default:	return 0;
	}
}

static void j36_wlan_rate_masks(struct j36_wlan_bss *bss)
{
	u8 i;

	for (i = 0; i < bss->rate_count; i++) {
		const u16 bit = j36_wlan_rate_bit(bss->rates[i]);

		bss->operational_rates |= bit;
		if (bss->rates[i] & 0x80)
			bss->basic_rates |= bit;
	}
}

/*
 * File the BSS away, replacing the weakest entry once the table is full.
 *
 * A beacon that updates an entry we already hold must NOT clobber a DTIM period
 * with a zero, because a probe response carries no TIM element and would
 * otherwise erase what a beacon told us -- and the DTIM period is what decides
 * how long the firmware may keep its receiver off.
 */
static void j36_wlan_record(struct j36_wlan *wlan,
			    const struct j36_wlan_bss *entry)
{
	struct j36_wlan_bss *slot = NULL;
	unsigned int i;

	for (i = 0; i < wlan->result_count; i++) {
		if (j36_same_mac(wlan->results[i].bssid, entry->bssid)) {
			slot = &wlan->results[i];
			break;
		}
	}
	if (!slot && wlan->result_count < ARRAY_SIZE(wlan->results)) {
		slot = &wlan->results[wlan->result_count++];
	} else if (!slot) {
		slot = &wlan->results[0];
		for (i = 1; i < wlan->result_count; i++)
			if (wlan->results[i].signal < slot->signal)
				slot = &wlan->results[i];
	}

	if (j36_same_mac(slot->bssid, entry->bssid) && slot->valid) {
		const u8 dtim = entry->dtim_period ? entry->dtim_period :
						     slot->dtim_period;
		const u16 interval = entry->beacon_interval ?
					     entry->beacon_interval :
					     slot->beacon_interval;

		*slot = *entry;
		slot->dtim_period = dtim;
		slot->beacon_interval = interval;
	} else {
		*slot = *entry;
	}
	slot->valid = true;
}

static const struct j36_wlan_bss *j36_wlan_lookup(struct j36_wlan *wlan,
						  const u8 *bssid,
						  const u8 *ssid, u8 ssid_len)
{
	const struct j36_wlan_bss *best = NULL;
	unsigned int i;

	for (i = 0; i < wlan->result_count; i++) {
		const struct j36_wlan_bss *entry = &wlan->results[i];

		if (!entry->valid)
			continue;
		if (bssid) {
			if (j36_same_mac(entry->bssid, bssid))
				return entry;
			continue;
		}
		if (!ssid_len || entry->ssid_len != ssid_len ||
		    memcmp(entry->ssid, ssid, ssid_len))
			continue;
		if (!best || entry->signal > best->signal)
			best = entry;
	}
	return best;
}

/* ── telling cfg80211 what we heard ──────────────────────────────────────────*/

static void j36_wlan_inform(struct j36_wlan *wlan,
			    const struct j36_wlan_bss *entry,
			    enum cfg80211_bss_frame_type ftype,
			    const u8 *ies, u32 ies_len)
{
	struct ieee80211_channel *channel = j36_wlan_channel(wlan,
							     entry->channel);
	struct cfg80211_bss *bss;

	if (!channel)
		return;
	/* The timestamp is passed as zero: this firmware does not report the
	 * beacon's TSF, and cfg80211 uses it only to order two sightings of the
	 * same BSS -- which our own table does by arrival instead. */
	bss = cfg80211_inform_bss(wlan->wiphy, channel, ftype, entry->bssid, 0,
				  entry->capability, entry->beacon_interval,
				  ies, ies_len, entry->signal * 100,
				  GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(wlan->wiphy, bss);
}

/* ── the join, one step at a time ────────────────────────────────────────────*/

static void j36_wlan_release_channel(struct j36_wlan *wlan)
{
	if (!wlan->channel_held)
		return;
	j36_wlan_cmd_channel_abort(wlan->w, wlan->channel_token);
	wlan->channel_held = false;
}

static void j36_wlan_flush_tx(struct j36_wlan *wlan)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wlan->tx_queue))) {
		wlan->ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
	}
	netif_wake_queue(wlan->ndev);
}

/*
 * Give the firmware back everything this join took from it, in the order stock's
 * own disconnect uses.
 *
 * The deauthentication frame goes FIRST, while the BSS is still live: SET_BSS_INFO
 * with the connection state cleared removes the firmware's context for this AP,
 * and a management frame addressed to a BSS it no longer holds is dropped on the
 * way out rather than transmitted.
 *
 * INDICATE_PM_ABORT goes next and before anything is dismantled, which is where
 * aisFsmDisconnect() puts it: the frame on the air first, then the firmware's
 * power-management module told the BSS is finished, and only then the BSS itself
 * taken apart.  Leaving it out -- which this driver did -- means the PM module
 * goes on holding a context for an AP whose BSS info, station record and network
 * activation are all being pulled out from under it, and what that costs is not a
 * refused command.  It is the next join's commands accepted and never answered.
 *
 * BSS_ACTIVATE_CTRL last, off and straight back on, is the one step that looks
 * superfluous and is not -- see j36_wlan_cmd_bss_reactivate(), where the AP that
 * went missing from three consecutive scans is described.
 */
static void j36_wlan_drop_link(struct j36_wlan *wlan, bool send_deauth,
			       u16 reason)
{
	struct j36_wifi *w = wlan->w;

	if (wlan->state == J36_WLAN_IDLE)
		return;

	/* Before anything is cleared, because the counters are the only account of
	 * why a link that came up did nothing, and the only moment they are worth
	 * printing is the moment it goes away. */
	j36_wlan_net_trace(w);

	netif_carrier_off(wlan->ndev);

	if (wlan->state == J36_WLAN_CONNECTED) {
		if (send_deauth)
			j36_wlan_cmd_deauth(w, &wlan->bss, reason);
		j36_wlan_cmd_pm_abort(w);
		j36_wlan_cmd_bss_disconnect(w, &wlan->bss);
		j36_wlan_cmd_sta_remove(w, &wlan->bss);
		j36_wlan_cmd_bss_reactivate(w);
	}
	j36_wlan_release_channel(wlan);

	wlan->state = J36_WLAN_IDLE;
	wlan->sta_index = J36_STA_INDEX_NOT_FOUND;
	wlan->sta_confirmed = false;
	wlan->ptk_ready = false;
	wlan->gtk_ready = false;
	wlan->key_ready = false;
	wlan->pm_sent = false;
	wlan->pm_deadline = 0;
	wlan->aid = 0;
	wlan->resp_ies_len = 0;
	wlan->tx_stalls = 0;
	w->pending_1x = 0;
	j36_wlan_flush_tx(wlan);
}

/*
 * A join that never completed.  cfg80211 is owed exactly one answer to its
 * .connect and this is the failing one; wpa_supplicant reads the status code, so
 * an association refusal is reported with the AP's own reason rather than
 * flattened to "failed".
 */
static void j36_wlan_join_failed(struct j36_wlan *wlan, u16 status)
{
	u8 bssid[ETH_ALEN];

	memcpy(bssid, wlan->bss.bssid, ETH_ALEN);
	j36_wlan_drop_link(wlan, false, 0);
	wlan->join_failures++;
	wlan->reported = false;
	cfg80211_connect_result(wlan->ndev, bssid, NULL, 0, NULL, 0,
				status ? status : WLAN_STATUS_UNSPECIFIED_FAILURE,
				GFP_KERNEL);
}

/* A join that completed and then ended.  The distinction from the above is not
 * cosmetic: cfg80211 tracks whether it has an outstanding connect, and answering
 * the wrong one leaves wpa_supplicant waiting for an event that will not come. */
static void j36_wlan_link_lost(struct j36_wlan *wlan, u16 reason,
			       bool locally_generated)
{
	const bool reported = wlan->reported;

	j36_wlan_drop_link(wlan, locally_generated, reason);
	wlan->reported = false;
	wlan->link_losses++;
	if (reported)
		cfg80211_disconnected(wlan->ndev, reason, NULL, 0,
				      locally_generated, GFP_KERNEL);
}

static void j36_wlan_end_link(struct j36_wlan *wlan, u16 reason,
			      bool locally_generated)
{
	switch (wlan->state) {
	case J36_WLAN_CONNECTED:
		j36_wlan_link_lost(wlan, reason, locally_generated);
		break;
	case J36_WLAN_CHANNEL:
	case J36_WLAN_AUTH:
	case J36_WLAN_ASSOC:
		j36_wlan_join_failed(wlan, WLAN_STATUS_UNSPECIFIED_FAILURE);
		break;
	default:
		break;
	}
}

/*
 * INDICATE_PM_BSS_CONNECTED, once, as soon as there is a real DTIM period to put
 * in it -- AND, FAILING THAT, ONCE ANYWAY.
 *
 * An association response carries no DTIM period, so this is called from
 * everywhere one might turn up: the key settlement, the association itself for an
 * open network, the first beacon after connecting, and the deadline check.
 * Whichever of them finds the link ready is the one that sends it.
 *
 * The deadline is the part that is not stock, and it is here because the reason
 * stock can wait forever does not hold on this firmware.  Stock is handed every
 * beacon of the connected BSS and sends the indication from the first one; this
 * driver, on this chip, has been observed to receive NONE while associated -- the
 * whole session's management-frame count was accounted for by its scans.  So
 * "wait for a beacon" here reads as "never send it", and never sending it is the
 * expensive option: this is the only command that carries the beacon interval, so
 * without it the firmware supervises the link against zero and reports a beacon
 * timeout about a minute into a perfectly good association.
 *
 * The clock therefore starts when the indication first becomes sendable rather
 * than at association, so a slow four-way handshake spends its own time and not
 * the beacon window; and the DTIM period that gets assumed is 1, which is safe
 * only because this radio is left in CAM and never sleeps.
 */
static void j36_wlan_try_pm(struct j36_wlan *wlan)
{
	bool fallback;

	if (wlan->pm_sent || wlan->state != J36_WLAN_CONNECTED)
		return;
	if (wlan->secure && !wlan->key_ready)
		return;

	if (!wlan->pm_deadline)
		wlan->pm_deadline = ktime_add_ms(ktime_get(),
						 J36_WLAN_PM_BEACON_WAIT_MS);
	fallback = ktime_after(ktime_get(), wlan->pm_deadline);

	if (j36_wlan_cmd_pm_connected(wlan->w, &wlan->bss, wlan->aid, fallback))
		return;
	wlan->pm_sent = true;
	if (!wlan->bss.dtim_period || !wlan->bss.beacon_interval)
		dev_info(wlan->w->dev,
			 "%s: nothing from %pM supplied a %s%s%s in %u ms; the power-save indication went out with the standard value assumed\n",
			 wlan->ndev->name, wlan->bss.bssid,
			 wlan->bss.dtim_period ? "" : "DTIM period",
			 (!wlan->bss.dtim_period && !wlan->bss.beacon_interval) ?
				 " or a " : "",
			 wlan->bss.beacon_interval ? "" : "beacon interval",
			 J36_WLAN_PM_BEACON_WAIT_MS);
}

/*
 * The moment the 802.1X port really opens, which is later than the pairwise key
 * and deliberately so.
 *
 * Message 3 of the handshake carries the group key and the supplicant installs it
 * immediately after the pairwise one, so the usual wait here is microseconds.
 * What the grace period buys is the case where it does not arrive at all: opening
 * the port while the firmware has no group key makes every broadcast the AP sends
 * undecryptable, and the first thing an AP broadcasts at a freshly joined station
 * is ARP.
 *
 * Only now is the channel privilege given back, too.  Holding it through the
 * handshake keeps the radio on the AP's channel for the four frames that matter
 * most and cannot be retried by anything above us.  Stock holds it for the same
 * span by a different clock -- rJoinTimeoutTimer runs for the granted interval
 * less AIS_JOIN_CH_GRANT_THRESHOLD and aisFsmRunEventJoinTimeout releases it from
 * AIS_STATE_NORMAL_TR when it fires.
 *
 * NO COMMAND IS SENT FROM HERE ANY MORE.  This used to re-send CMD_SET_BSS_INFO
 * to move ucEncStatus from KEY_ABSENT to ENABLED, which stock does not do and no
 * longer needs doing: j36_wlan_cmd_bss_info() writes ENCRYPTION3_ENABLED at join,
 * from the cipher, exactly as nicUpdateBss does.  key_ready is now purely this
 * driver's own gate -- the transmit queue and the PM indication wait on it.
 */
static void j36_wlan_settle_keys(struct j36_wlan *wlan)
{
	if (wlan->state != J36_WLAN_CONNECTED)
		return;
	if (!wlan->secure || wlan->key_ready || !wlan->ptk_ready)
		return;
	if (!wlan->gtk_ready && ktime_before(ktime_get(), wlan->gtk_deadline))
		return;

	wlan->key_ready = true;
	j36_wlan_release_channel(wlan);
	j36_wlan_try_pm(wlan);
}

/* ── what the pump hands up ──────────────────────────────────────────────────*/

static struct j36_wlan *j36_wlan_of(struct j36_wifi *w)
{
	struct j36_wlan *wlan = w->wlan;

	if (!wlan || !wlan->running)
		return NULL;
	return wlan;
}

const u8 *j36_wlan_peer_bssid(struct j36_wifi *w)
{
	struct j36_wlan *wlan = j36_wlan_of(w);

	if (!wlan || wlan->state == J36_WLAN_IDLE)
		return NULL;
	return wlan->bss.bssid;
}

void j36_wlan_on_beacon(struct j36_wifi *w, const u8 *frame, u32 frame_len,
			u8 channel, u8 rcpi)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	struct j36_wlan_bss entry = {};
	const u8 *ies;
	u32 ies_len;
	bool is_beacon;

	/* 24 bytes of 802.11 header, then the fixed fields a beacon and a probe
	 * response share: timestamp, beacon interval, capability. */
	if (!wlan || frame_len < 36)
		return;
	/* Anything at all off the air is proof the radio is still working, which
	 * is what the silence count is counting the absence of. */
	wlan->silence = 0;
	is_beacon = (j36_get_le16(frame) & 0x00f0) == 0x0080;
	ies = frame + 36;
	ies_len = frame_len - 36;

	memcpy(entry.bssid, frame + 16, ETH_ALEN);
	entry.beacon_interval = j36_get_le16(frame + 32);
	entry.capability = j36_get_le16(frame + 34);
	entry.privacy = !!(entry.capability & BIT(4));
	entry.channel = channel;
	entry.rcpi = rcpi;
	entry.signal = j36_wlan_rcpi_dbm(rcpi);
	j36_wlan_parse_ies(&entry, ies, ies_len);
	j36_wlan_rate_masks(&entry);
	if (!entry.channel)
		entry.channel = channel;

	j36_wlan_record(wlan, &entry);
	j36_wlan_inform(wlan, &entry,
			is_beacon ? CFG80211_BSS_FTYPE_BEACON :
				    CFG80211_BSS_FTYPE_PRESP,
			ies, ies_len);

	/*
	 * While connected, the AP's own beacon is the live signal reading the
	 * dashboard shows and the only source of a DTIM period.  It is also why
	 * the pump hands beacons up whether or not a scan is running: a station
	 * sitting on its operating channel hears this one every hundred
	 * milliseconds, and gating on a scan would throw away the readings for
	 * the one network guaranteed to be in range.
	 */
	if (wlan->state != J36_WLAN_CONNECTED ||
	    !j36_same_mac(entry.bssid, wlan->bss.bssid))
		return;
	wlan->bss.signal = entry.signal;
	wlan->bss.rcpi = entry.rcpi;
	if (!is_beacon)
		return;
	/* Counted apart from every other management frame because the trace is the
	 * only place the difference shows, and the difference is the whole reason
	 * the power-save indication has a deadline on it. */
	wlan->beacons++;
	if (entry.beacon_interval)
		wlan->bss.beacon_interval = entry.beacon_interval;
	if (entry.dtim_period)
		wlan->bss.dtim_period = entry.dtim_period;
	j36_wlan_try_pm(wlan);
}

void j36_wlan_on_scan_result(struct j36_wifi *w, const u8 *bssid, u16 capability,
			     u8 channel, s32 signal, const u8 *ies, u32 ies_len)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	struct j36_wlan_bss entry = {};

	if (!wlan || !channel)
		return;
	wlan->silence = 0;

	memcpy(entry.bssid, bssid, ETH_ALEN);
	entry.capability = capability;
	entry.privacy = !!(capability & BIT(4));
	entry.channel = channel;
	entry.signal = clamp_t(s32, signal, -128, 0);
	/* The descriptor reports a level and not an RCPI; converting back is
	 * what UPDATE_STA_RECORD's own field wants. */
	entry.rcpi = (u8)clamp_t(s32, (entry.signal + 110) * 2, 0, 255);
	j36_wlan_parse_ies(&entry, ies, ies_len);
	j36_wlan_rate_masks(&entry);
	if (!entry.channel)
		entry.channel = channel;

	j36_wlan_record(wlan, &entry);
	j36_wlan_inform(wlan, &entry, CFG80211_BSS_FTYPE_UNKNOWN, ies, ies_len);
}

static void j36_wlan_finish_scan(struct j36_wlan *wlan, bool aborted)
{
	struct cfg80211_scan_request *request = wlan->scan_request;
	struct cfg80211_scan_info info = { .aborted = aborted };

	if (!request)
		return;
	wlan->scan_request = NULL;
	wlan->scan_sequence = 0;
	cfg80211_scan_done(request, &info);
}

void j36_wlan_on_scan_done(struct j36_wifi *w, u8 sequence)
{
	struct j36_wlan *wlan = j36_wlan_of(w);

	if (!wlan || !wlan->scan_request || sequence != wlan->scan_sequence)
		return;
	/* A sweep that ran to its own SCAN_DONE is the strongest evidence this
	 * driver ever gets that the chip is working, so it clears the recovery
	 * budget too -- the limit is meant to bound one episode, not a lifetime. */
	wlan->silence = 0;
	wlan->recoveries = 0;
	j36_wlan_finish_scan(wlan, false);
}

/*
 * The channel is ours.  Two commands go out back to back and their order is
 * stock's: the station record has to exist in STA_STATE_1 before the
 * authentication frame, because the firmware matches the incoming response
 * against a record and drops one it cannot place.
 */
void j36_wlan_on_channel_grant(struct j36_wifi *w, u8 token)
{
	struct j36_wlan *wlan = j36_wlan_of(w);

	if (!wlan || wlan->state != J36_WLAN_CHANNEL ||
	    token != wlan->channel_token)
		return;

	wlan->silence = 0;
	wlan->channel_held = true;
	j36_wlan_cmd_sta_record(w, &wlan->bss, J36_STA_STATE_1, 0);
	if (j36_wlan_cmd_auth(w, &wlan->bss)) {
		j36_wlan_join_failed(wlan, WLAN_STATUS_UNSPECIFIED_FAILURE);
		return;
	}
	wlan->state = J36_WLAN_AUTH;
	wlan->deadline = ktime_add_ms(ktime_get(), J36_WLAN_AUTH_TIMEOUT_MS);
}

/*
 * The authentication response: algorithm at 24, transaction sequence at 26,
 * status at 28.  Only transaction 2 is ours -- 1 is the request we sent, echoed
 * back by an AP in a mode this driver does not use.
 */
void j36_wlan_on_auth_response(struct j36_wifi *w, const u8 *frame, u32 frame_len)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	u16 status;

	if (!wlan || wlan->state != J36_WLAN_AUTH || frame_len < 30)
		return;
	if (!j36_same_mac(frame + 10, wlan->bss.bssid))
		return;
	if (j36_get_le16(frame + 26) != 2)
		return;

	status = j36_get_le16(frame + 28);
	if (status) {
		j36_wlan_join_failed(wlan, status);
		return;
	}
	if (j36_wlan_cmd_assoc(w, &wlan->bss, wlan->assoc_ies,
			       wlan->assoc_ies_len)) {
		j36_wlan_join_failed(wlan, WLAN_STATUS_UNSPECIFIED_FAILURE);
		return;
	}
	wlan->state = J36_WLAN_ASSOC;
	wlan->deadline = ktime_add_ms(ktime_get(), J36_WLAN_ASSOC_TIMEOUT_MS);
}

/*
 * The association response: capability at 24, status at 26, AID at 28, then the
 * AP's own IEs.
 *
 * SET_BSS_INFO goes out BEFORE the station record, which is the order stock uses
 * and not an arbitrary one -- the record names a BSS index the firmware has to
 * already hold, and a record for a BSS it does not have is accepted and then
 * never activated.
 *
 * cfg80211 is told the connection succeeded HERE, at association, not after the
 * keys.  It has to be: wpa_supplicant does not begin the four-way handshake until
 * it sees the connect event, and the handshake is what produces the keys.
 */
void j36_wlan_on_assoc_response(struct j36_wifi *w, const u8 *frame,
				u32 frame_len)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	u16 status;

	if (!wlan || wlan->state != J36_WLAN_ASSOC || frame_len < 30)
		return;
	if (!j36_same_mac(frame + 10, wlan->bss.bssid))
		return;

	status = j36_get_le16(frame + 26);
	if (status) {
		j36_wlan_join_failed(wlan, status);
		return;
	}

	wlan->aid = j36_get_le16(frame + 28) & 0x3fff;
	wlan->resp_ies_len = (u16)min_t(u32, frame_len - 30, J36_WLAN_MAX_IES);
	memcpy(wlan->resp_ies, frame + 30, wlan->resp_ies_len);

	j36_wlan_cmd_bss_info(w, &wlan->bss, wlan->secure);
	j36_wlan_cmd_sta_record(w, &wlan->bss, J36_STA_STATE_3, wlan->aid);

	/*
	 * ── THE RECORD INDEX IS OURS, NOT THE FIRMWARE'S ─────────────────────
	 *
	 * J36_STA_RECORD_INDEX is a constant this driver chose and just sent in
	 * the command above; the firmware does not allocate it and does not hand
	 * it back.  ACTIVATE_STA_REC is a CONFIRMATION that the record it names
	 * went valid, and it used to be the only thing that set this field -- so
	 * an event that did not arrive left every data frame carrying
	 * STA_INDEX_NOT_FOUND, which is a descriptor the firmware cannot place.
	 *
	 * There is exactly one peer in an infrastructure BSS and it is the one
	 * the association just completed with, so waiting for permission to name
	 * it buys nothing and costs the whole data path.  The event still arrives
	 * or does not; it sets sta_confirmed and the trace prints it.
	 */
	wlan->sta_index = J36_STA_RECORD_INDEX;
	wlan->sta_confirmed = false;

	wlan->state = J36_WLAN_CONNECTED;
	wlan->joins++;
	wlan->reported = true;
	netif_carrier_on(wlan->ndev);
	netif_wake_queue(wlan->ndev);

	/*
	 * req_ie is the supplicant's own bytes, unchanged, because it computes
	 * the message-2 MIC over the RSN element it believes it sent and the AP
	 * checks the same element against message 3.  A driver that substituted
	 * its own -- however correct in isolation -- would break both.
	 */
	cfg80211_connect_result(wlan->ndev, wlan->bss.bssid, wlan->assoc_ies,
				wlan->assoc_ies_len, wlan->resp_ies,
				wlan->resp_ies_len, WLAN_STATUS_SUCCESS,
				GFP_KERNEL);

	/* An open network has no handshake to wait for, so nothing is holding
	 * the channel and the power-save indication can go as soon as a beacon
	 * supplies a DTIM period. */
	if (!wlan->secure) {
		j36_wlan_release_channel(wlan);
		j36_wlan_try_pm(wlan);
	}
}

void j36_wlan_on_sta_active(struct j36_wifi *w, const u8 *peer)
{
	struct j36_wlan *wlan = j36_wlan_of(w);

	if (!wlan || wlan->state != J36_WLAN_CONNECTED)
		return;
	if (!j36_same_mac(peer, wlan->bss.bssid))
		return;
	/* The index was set when the record was sent -- see
	 * j36_wlan_on_assoc_response().  All this adds is that the firmware agrees. */
	wlan->sta_index = J36_STA_RECORD_INDEX;
	wlan->sta_confirmed = true;
}

void j36_wlan_on_ap_disconnect(struct j36_wifi *w, const u8 *frame,
			       u32 frame_len, bool deauth)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	u16 reason = WLAN_REASON_UNSPECIFIED;

	if (!wlan)
		return;
	if (frame_len >= 26)
		reason = j36_get_le16(frame + 24);
	dev_dbg(w->dev, "%s from the AP, reason %u\n",
		deauth ? "deauthentication" : "disassociation", reason);
	j36_wlan_end_link(wlan, reason, false);
}

/*
 * EVENT_BSS_BEACON_TIMEOUT: the firmware's own supervision gave up on the AP.
 *
 * Reported rather than merely acted on, because the two ways of reaching it look
 * identical from userspace and are not the same fault at all.  One is a station
 * that walked out of range, and then the beacon count below is large and the
 * signal is poor.  The other is a firmware that was never told how often to
 * expect a beacon -- see j36_wlan_try_pm() -- and then the count is zero and the
 * signal is whatever the scan last read, which is how this was found.
 */
void j36_wlan_on_beacon_timeout(struct j36_wifi *w)
{
	struct j36_wlan *wlan = j36_wlan_of(w);

	if (!wlan)
		return;
	if (wlan->state == J36_WLAN_CONNECTED)
		dev_info(w->dev,
			 "%s: the firmware reported a beacon timeout for %pM after %u of its beacons reached us, last signal %d dBm, power-save indication %s\n",
			 wlan->ndev->name, wlan->bss.bssid, wlan->beacons,
			 wlan->bss.signal, wlan->pm_sent ? "sent" : "NEVER SENT");
	j36_wlan_end_link(wlan, WLAN_REASON_DISASSOC_DUE_TO_INACTIVITY, false);
}

/* ── the data path ───────────────────────────────────────────────────────────*/

void j36_wlan_on_ethernet(struct j36_wifi *w, const u8 *frame, u32 frame_len)
{
	struct j36_wlan *wlan = j36_wlan_of(w);
	struct sk_buff *skb;

	/*
	 * Gated on being associated and NOT on the keys being in, which is the
	 * whole reason a fullmac station can run an unmodified supplicant: the
	 * EAPOL frames of the handshake arrive before any key exists, and a
	 * driver that held them until the port opened would be waiting for the
	 * thing they produce.
	 */
	if (!wlan || wlan->state != J36_WLAN_CONNECTED)
		return;
	if (frame_len < ETH_HLEN || frame_len > wlan->ndev->mtu + ETH_HLEN) {
		wlan->ndev->stats.rx_length_errors++;
		return;
	}

	/* Two bytes of headroom so the IP header lands on a four-byte boundary
	 * once eth_type_trans() has pulled the fourteen-byte Ethernet one. */
	skb = netdev_alloc_skb(wlan->ndev, frame_len + NET_IP_ALIGN);
	if (!skb) {
		wlan->ndev->stats.rx_dropped++;
		return;
	}
	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, frame, frame_len);
	skb->protocol = eth_type_trans(skb, wlan->ndev);
	wlan->ndev->stats.rx_packets++;
	wlan->ndev->stats.rx_bytes += frame_len;
	netif_rx(skb);
}

static bool j36_wlan_is_eapol(const struct sk_buff *skb)
{
	if (skb->len < ETH_HLEN)
		return false;
	return (((u16)skb->data[12] << 8) | skb->data[13]) == ETH_P_PAE;
}

/*
 * Drain what ndo_start_xmit queued.
 *
 * Bounded per call because this runs with w->lock held and .connect and .scan
 * are waiting on it; a starved traffic class puts the frame back at the HEAD of
 * the queue rather than dropping it, because the frame that could not get a page
 * is very often the one an EAPOL exchange is waiting on and TCP is not the only
 * thing that would notice it going missing.
 *
 * AND IT DOES NOT WAIT FOR A PAGE, which is the whole point of the may_wait
 * argument below.  Waiting here is waiting with w->lock held, and the lock is
 * what the event pump needs -- so the sixteen frames of a busy poll could spend
 * six seconds between them not sending, during which no TX_DONE came back to
 * credit the pages being waited on, no beacon or scan result was read out of a
 * receive FIFO that was filling, and .scan and .connect sat on the mutex.  That
 * is the shape of "the link drops when I run apt update": nothing was lost on
 * the air, the driver simply stopped servicing the chip under its own load.
 * Refused now means requeued now, and the next tick is a jiffy away.
 */
static unsigned int j36_wlan_drain_tx(struct j36_wlan *wlan)
{
	struct j36_wifi *w = wlan->w;
	unsigned int sent = 0;
	struct sk_buff *skb;

	if (wlan->state != J36_WLAN_CONNECTED)
		return 0;

	while (sent < J36_WLAN_MAX_TX_PER_POLL &&
	       (skb = skb_dequeue(&wlan->tx_queue))) {
		const bool is_1x = j36_wlan_is_eapol(skb);
		const unsigned int len = skb->len;
		int ret;

		ret = j36_wlan_cmd_tx_ethernet(w, skb->data, len,
					       wlan->sta_index, is_1x, false);
		if (ret == -EBUSY) {
			const ktime_t now = ktime_get();

			if (!wlan->tx_stalls++)
				wlan->tx_stall_since = now;
			if (ktime_before(now,
					 ktime_add_ms(wlan->tx_stall_since,
						      J36_WLAN_TX_STALL_MS))) {
				skb_queue_head(&wlan->tx_queue, skb);
				wlan->tx_deferred++;
				break;
			}
			/* Rate limited because the point of getting here is that
			 * there are a lot of frames behind this one and every one
			 * of them is about to say the same thing. */
			dev_warn_ratelimited(w->dev,
					     "%s: no transmit page in %u ms across %u polls; dropping the frame at the head of the queue rather than stalling the interface behind it\n",
					     wlan->ndev->name,
					     J36_WLAN_TX_STALL_MS,
					     wlan->tx_stalls);
			wlan->tx_starved++;
		}

		/* Past the retry, so the head of the queue is moving again --
		 * whether it moved onto the air or into the bin. */
		wlan->tx_stalls = 0;

		if (ret) {
			wlan->ndev->stats.tx_errors++;
			wlan->ndev->stats.tx_dropped++;
		} else {
			wlan->ndev->stats.tx_packets++;
			wlan->ndev->stats.tx_bytes += len;
			sent++;
		}
		dev_kfree_skb_any(skb);
	}

	if (skb_queue_len(&wlan->tx_queue) < J36_WLAN_TX_QUEUE_WAKE)
		netif_wake_queue(wlan->ndev);
	return sent;
}

static netdev_tx_t j36_wlan_start_xmit(struct sk_buff *skb,
				       struct net_device *ndev)
{
	struct j36_wlan *wlan = j36_wlan_from_netdev(ndev);

	/*
	 * ATOMIC.  Nothing here may take w->lock, touch the HIF or sleep -- the
	 * frame goes on the queue and the poll worker sends it.  That is not a
	 * design preference: the page accounting one layer down sleeps for up to
	 * 200 ms when the firmware's transmit queue is full.
	 */
	if (wlan->state != J36_WLAN_CONNECTED || skb_linearize(skb)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	skb_queue_tail(&wlan->tx_queue, skb);
	if (skb_queue_len(&wlan->tx_queue) >= J36_WLAN_TX_QUEUE_STOP) {
		netif_stop_queue(ndev);
		wlan->tx_backpressure++;
	}
	mod_delayed_work(wlan->wq, &wlan->poll, 0);
	return NETDEV_TX_OK;
}

/* ── the poll worker ─────────────────────────────────────────────────────────*/

static void j36_wlan_check_deadlines(struct j36_wlan *wlan)
{
	const ktime_t now = ktime_get();

	switch (wlan->state) {
	case J36_WLAN_CHANNEL:
		/* CMD_CH_PRIVILEGE was accepted -- the page accounting says so --
		 * and EVENT_CH_PRIVILEGE never came back.  Nothing on the air was
		 * involved, so this one is the chip and it counts. */
		if (ktime_after(now, wlan->deadline)) {
			wlan->silence++;
			j36_wlan_join_failed(wlan,
					     WLAN_STATUS_UNSPECIFIED_FAILURE);
		}
		break;
	case J36_WLAN_AUTH:
	case J36_WLAN_ASSOC:
		/* Deliberately NOT counted.  An authentication or association
		 * that goes unanswered is an AP not answering, and re-arming the
		 * radio over it would be treating a crowded channel as a broken
		 * chip. */
		if (ktime_after(now, wlan->deadline))
			j36_wlan_join_failed(wlan,
					     WLAN_STATUS_UNSPECIFIED_FAILURE);
		break;
	case J36_WLAN_CONNECTED:
		j36_wlan_settle_keys(wlan);
		/* Here and not only from the beacon handler, because the case
		 * this exists for is the one where no beacon ever arrives. */
		j36_wlan_try_pm(wlan);
		break;
	default:
		break;
	}

	if (wlan->scan_request && ktime_after(now, wlan->scan_deadline)) {
		wlan->silence++;
		j36_wlan_finish_scan(wlan, true);
	}
}

/*
 * Zero after a productive poll, because a packet that arrived is evidence there
 * is another behind it; one jiffy whenever anything is in flight, which is the
 * whole of the data path's added latency; and fifty milliseconds when the
 * interface is up with nothing to do, which is the price of having no interrupt.
 */
static unsigned long j36_wlan_poll_delay(struct j36_wlan *wlan,
					 unsigned int handled)
{
	if (handled)
		return 0;
	if (wlan->scan_request || wlan->state != J36_WLAN_IDLE)
		return 1;
	return msecs_to_jiffies(J36_WLAN_IDLE_POLL_MS);
}

/* The transport faulted or the firmware stopped answering.  There is no reset
 * short of the whole four-stage bring-up, so the honest thing is to take the
 * carrier down, tell whoever was connected, and stop polling a dead radio. */
static void j36_wlan_radio_lost(struct j36_wlan *wlan)
{
	dev_err(wlan->w->dev,
		"the WLAN firmware stopped answering; %s is going down [%s]\n",
		wlan->ndev->name,
		wlan->w->blocked ? wlan->w->blocked : "unknown");
	j36_wlan_end_link(wlan, WLAN_REASON_UNSPECIFIED, false);
	j36_wlan_finish_scan(wlan, true);
	netif_carrier_off(wlan->ndev);
	wlan->running = false;
}

/*
 * The step between "one operation went unanswered" and "the radio is gone".
 *
 * There is a failure mode on this part that has no message of its own and used
 * to have no end either: the firmware keeps taking commands -- they are credited,
 * the page accounting is happy, nothing returns an error -- and answers none of
 * them.  Every scan is submitted and then abandoned on its own deadline, every
 * channel request times out, and from userspace it reads as an SSID that has
 * stopped existing.  MVII's own log has three minutes and eighteen scans of it
 * with not one line from this driver.
 *
 * The likeliest cause is now fixed at the source -- a teardown whose
 * BSS_ACTIVATE_CTRL "on" was refused leaves the AIS network deactivated, and a
 * deactivated network behaves in exactly this way -- but "likeliest" is not
 * "only", and an interface that wedges until reboot is the worst outcome
 * available.  So the network is re-armed, which is the largest thing that can be
 * done to this chip short of downloading the firmware again, and if that does not
 * take then the radio is declared lost with a message that says which of the two
 * it was.
 *
 * Bounded, and the bound resets on the first scan that completes normally: the
 * budget is meant to stop a re-arm loop inside one episode, not to ration a
 * device's lifetime.
 */
static void j36_wlan_recover(struct j36_wlan *wlan)
{
	struct j36_wifi *w = wlan->w;

	wlan->silence = 0;
	wlan->recoveries++;

	if (wlan->recoveries > J36_WLAN_RECOVER_LIMIT) {
		dev_err(w->dev,
			"%s: the firmware is still not answering after %u re-arms of the station network, and there is no reset short of the whole bring-up\n",
			wlan->ndev->name, J36_WLAN_RECOVER_LIMIT);
		j36_wlan_radio_lost(wlan);
		return;
	}

	dev_warn(w->dev,
		 "%s: %u firmware operations in a row were accepted and never answered; re-arming the station network (attempt %u of %u)\n",
		 wlan->ndev->name, J36_WLAN_SILENCE_RECOVER, wlan->recoveries,
		 J36_WLAN_RECOVER_LIMIT);

	/*
	 * What is in flight is finished first, and finished honestly: cfg80211 is
	 * owed exactly one answer to any outstanding .connect and .scan, and
	 * tearing the network down underneath one leaves wpa_supplicant waiting
	 * for an event that is never coming -- which is the same wedge by another
	 * road.
	 */
	j36_wlan_end_link(wlan, WLAN_REASON_UNSPECIFIED, false);
	j36_wlan_finish_scan(wlan, true);
	j36_wlan_release_channel(wlan);
	j36_wlan_flush_tx(wlan);

	if (j36_wlan_cmd_rearm(w))
		dev_err(w->dev, "%s: the station network would not re-arm\n",
			wlan->ndev->name);
}

static void j36_wlan_poll_work(struct work_struct *work)
{
	struct j36_wlan *wlan = container_of(to_delayed_work(work),
					     struct j36_wlan, poll);
	struct j36_wifi *w = wlan->w;
	unsigned int handled;

	mutex_lock(&w->lock);
	if (!wlan->running)
		goto out;
	if (!w->firmware_alive) {
		j36_wlan_radio_lost(wlan);
		goto out;
	}

	handled = j36_wlan_cmd_pump(w);
	handled += j36_wlan_drain_tx(wlan);
	j36_wlan_check_deadlines(wlan);

	/* The pump clears firmware_alive when the FIFO pair reports abnormal,
	 * which is the one fault that cannot be waited out. */
	if (!w->firmware_alive) {
		j36_wlan_radio_lost(wlan);
		goto out;
	}
	/* And this is the other one, which reports nothing at all: a firmware
	 * that takes every command and answers none.  Checked here rather than
	 * inside the deadline pass so the recovery is not run from underneath the
	 * state machine it is about to reset. */
	if (wlan->silence >= J36_WLAN_SILENCE_RECOVER) {
		j36_wlan_recover(wlan);
		if (!wlan->running)
			goto out;
	}
	queue_delayed_work(wlan->wq, &wlan->poll,
			   j36_wlan_poll_delay(wlan, handled));
out:
	mutex_unlock(&w->lock);
}

/* ── the netdev ──────────────────────────────────────────────────────────────*/

static int j36_wlan_open(struct net_device *ndev)
{
	struct j36_wlan *wlan = j36_wlan_from_netdev(ndev);
	struct j36_wifi *w = wlan->w;

	mutex_lock(&w->lock);
	if (!w->firmware_alive) {
		mutex_unlock(&w->lock);
		return -ENODEV;
	}
	/*
	 * Where stock sends the receive filter: Linux calls ndo_set_rx_mode out
	 * of dev_open(), and stock's set_rx_mode is the only caller of
	 * CMD_SET_RX_FILTER on the station path.  It is sent from the adapter
	 * start as well, so the first open is a harmless repeat -- this one is
	 * for the down-and-up cycle, after which the adapter start does not run
	 * again and a firmware that had dropped the mask would go deaf without
	 * anything saying so.  The command is fire-and-forget and the interface
	 * still comes up if it fails; the failure is already reported by the
	 * caller of the same command at attach.
	 */
	j36_wlan_cmd_rx_filter(w);
	/* A fresh interface starts with a clean account of the firmware, so a
	 * count left over from before it went down cannot re-arm a radio that was
	 * never asked to do anything yet. */
	wlan->silence = 0;
	wlan->recoveries = 0;
	wlan->running = true;
	mutex_unlock(&w->lock);

	netif_carrier_off(ndev);
	netif_start_queue(ndev);
	queue_delayed_work(wlan->wq, &wlan->poll, 0);
	return 0;
}

static int j36_wlan_stop(struct net_device *ndev)
{
	struct j36_wlan *wlan = j36_wlan_from_netdev(ndev);
	struct j36_wifi *w = wlan->w;

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);

	mutex_lock(&w->lock);
	j36_wlan_end_link(wlan, WLAN_REASON_DEAUTH_LEAVING, true);
	/*
	 * Aborted HERE and not left to cfg80211's own NETDEV_DOWN handler, which
	 * reaches the same place with a WARN_ON for a driver that never answered
	 * the scan it was given.
	 */
	j36_wlan_finish_scan(wlan, true);
	wlan->running = false;
	mutex_unlock(&w->lock);

	/* Outside the lock: the worker takes it, and this waits for the worker. */
	cancel_delayed_work_sync(&wlan->poll);
	skb_queue_purge(&wlan->tx_queue);
	return 0;
}

static const struct net_device_ops j36_wlan_netdev_ops = {
	.ndo_open		= j36_wlan_open,
	.ndo_stop		= j36_wlan_stop,
	.ndo_start_xmit		= j36_wlan_start_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/* ── cfg80211 ────────────────────────────────────────────────────────────────*/

static int j36_wlan_cfg_scan(struct wiphy *wiphy,
			     struct cfg80211_scan_request *request)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;
	int ret;

	mutex_lock(&w->lock);
	if (!wlan->running || !w->firmware_alive) {
		ret = -ENETDOWN;
		goto out;
	}
	if (wlan->scan_request) {
		ret = -EBUSY;
		goto out;
	}
	/* A join is a sequence of commands with a channel lease under it, and a
	 * sweep in the middle of one takes the radio off the AP's channel. */
	if (wlan->state != J36_WLAN_IDLE && wlan->state != J36_WLAN_CONNECTED) {
		ret = -EBUSY;
		goto out;
	}

	/*
	 * The SSID list, the channel list and request->ie are all ignored:
	 * CMD_SCAN_REQ goes out as the wildcard 2.4 GHz sweep stock sends, and
	 * the firmware builds the probe request body itself, so there is nowhere
	 * to put a caller's IEs.  What comes back is a superset of what was
	 * asked for.  The one thing it cannot do is find a hidden SSID, which is
	 * why max_scan_ssids is 1 rather than a number that would imply directed
	 * probes this does not send.
	 */
	ret = j36_wlan_cmd_scan(w, true, &wlan->scan_sequence);
	if (ret)
		goto out;

	wlan->scan_request = request;
	wlan->scan_deadline = ktime_add_ms(ktime_get(),
					   J36_WLAN_SCAN_TIMEOUT_MS);
	wlan->scans++;
	j36_wlan_kick(wlan);
out:
	mutex_unlock(&w->lock);
	return ret;
}

static bool j36_wlan_crypto_supported(const struct cfg80211_crypto_settings *c)
{
	int i;

	if (!c->cipher_group && !c->n_ciphers_pairwise)
		return true;			/* open */
	if (c->cipher_group != WLAN_CIPHER_SUITE_CCMP)
		return false;
	for (i = 0; i < c->n_ciphers_pairwise; i++)
		if (c->ciphers_pairwise[i] != WLAN_CIPHER_SUITE_CCMP)
			return false;
	for (i = 0; i < c->n_akm_suites; i++)
		if (c->akm_suites[i] != WLAN_AKM_SUITE_PSK)
			return false;
	return true;
}

static int j36_wlan_cfg_connect(struct wiphy *wiphy, struct net_device *ndev,
				struct cfg80211_connect_params *sme)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;
	const struct j36_wlan_bss *found;
	const u8 *bssid = sme->bssid ? sme->bssid : sme->bssid_hint;
	int ret;

	if (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	    sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC)
		return -EOPNOTSUPP;
	if (!j36_wlan_crypto_supported(&sme->crypto))
		return -EOPNOTSUPP;
	if (sme->ie_len > J36_WLAN_MAX_IES)
		return -E2BIG;

	mutex_lock(&w->lock);
	if (!wlan->running || !w->firmware_alive) {
		ret = -ENETDOWN;
		goto out;
	}

	/*
	 * Everything the firmware's commands need about this AP comes out of our
	 * own table, because a scan is the only thing that ever carried it: the
	 * rate bitmaps, the DTIM period and the channel are not in a connect
	 * request and are not reconstructible from one.  A miss is reported
	 * rather than papered over -- wpa_supplicant scans before it connects,
	 * so a miss means the AP went away between the two and retrying after a
	 * fresh scan is the correct answer, not guessing at defaults.
	 */
	found = j36_wlan_lookup(wlan, bssid, sme->ssid, sme->ssid_len);
	if (!found) {
		dev_info(w->dev,
			 "no scanned BSS matches this connect request; a fresh scan is needed\n");
		ret = -ENOENT;
		goto out;
	}

	if (wlan->state != J36_WLAN_IDLE)
		j36_wlan_drop_link(wlan, true, WLAN_REASON_DEAUTH_LEAVING);
	if (wlan->scan_request)
		j36_wlan_finish_scan(wlan, true);

	wlan->bss = *found;
	wlan->secure = sme->crypto.cipher_group == WLAN_CIPHER_SUITE_CCMP;
	wlan->assoc_ies_len = (u16)sme->ie_len;
	if (sme->ie_len)
		memcpy(wlan->assoc_ies, sme->ie, sme->ie_len);
	wlan->reported = false;
	wlan->resp_ies_len = 0;
	wlan->sta_index = J36_STA_INDEX_NOT_FOUND;
	wlan->ptk_ready = false;
	wlan->gtk_ready = false;
	wlan->key_ready = false;
	wlan->pm_sent = false;
	wlan->pm_deadline = 0;
	wlan->beacons = 0;
	wlan->aid = 0;
	wlan->tx_stalls = 0;

	/*
	 * ONE command starts the join, and it is not the station record.  The
	 * firmware will not accept an authentication frame for a channel it has
	 * not granted, so everything else waits for EVENT_CH_PRIVILEGE.
	 */
	ret = j36_wlan_cmd_channel_request(w, &wlan->bss, &wlan->channel_token);
	if (ret)
		goto out;

	wlan->state = J36_WLAN_CHANNEL;
	wlan->deadline = ktime_add_ms(ktime_get(), J36_WLAN_CHANNEL_TIMEOUT_MS);
	j36_wlan_kick(wlan);
out:
	mutex_unlock(&w->lock);
	return ret;
}

static int j36_wlan_cfg_disconnect(struct wiphy *wiphy, struct net_device *ndev,
				   u16 reason)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;

	mutex_lock(&w->lock);
	j36_wlan_end_link(wlan, reason, true);
	mutex_unlock(&w->lock);
	return 0;
}

/*
 * .add_key, and the two orderings in it.
 *
 * The queue is drained first so that the EAPOL frames this driver is still
 * holding are handed to the firmware BEFORE the key command, and
 * j36_wlan_cmd_install_key() then waits on the firmware's own transmit status for
 * the last of them.  Between those two, message 4 of the handshake cannot end up
 * encrypted with the key it is the acknowledgement for.
 */
static int j36_wlan_cfg_add_key(struct wiphy *wiphy, struct net_device *ndev,
				int link_id, u8 key_index, bool pairwise,
				const u8 *mac_addr, struct key_params *params)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;
	const u8 *peer;
	int ret;

	if (params->cipher != WLAN_CIPHER_SUITE_CCMP)
		return -EOPNOTSUPP;
	if (params->key_len != 16)
		return -EINVAL;
	/* wlanoidSetAddKey refuses a pairwise key at any index but 0, and the
	 * supplicant has never asked for one. */
	if (pairwise && key_index != 0)
		return -EINVAL;

	mutex_lock(&w->lock);
	if (wlan->state != J36_WLAN_CONNECTED) {
		ret = -ENOLINK;
		goto out;
	}

	/*
	 * ── THE ADDRESS A GROUP KEY IS FILED UNDER, AND THE BUG THAT WAS HERE ──
	 *
	 * A group key names the BROADCAST address.  Not the AP's.  The comment over
	 * j36_wlan_cmd_install_key() has said so since it was written -- "a pairwise
	 * key names the AP and sets both flags, a group key names the broadcast
	 * address and sets neither" -- and this line used to fall back to the BSSID
	 * for both, because cfg80211 passes mac_addr for a pairwise key and NULL for
	 * a group one and NULL was read as "no peer given" rather than as the peer it
	 * actually is.
	 *
	 * Filed under the BSSID the GTK is a key the firmware never matches, because
	 * what it matches a group-addressed frame against is ff:ff:ff:ff:ff:ff.  So
	 * every broadcast and multicast frame the AP sent was undecryptable and
	 * silently dropped, and NOTHING ABOVE COULD SEE IT: association completed,
	 * the four-way handshake completed, unicast worked, the link showed as up.
	 *
	 * What died was DHCP.  A DHCP server answers a station that has no address
	 * yet by broadcasting, and so does every ARP request on the segment, so the
	 * board sat in ip-config until whatever was asking gave up -- dhcpcd falling
	 * back to 169.254.x.x when this page ran its own client, and
	 * "Timeout expired" once NetworkManager's did.  One wrong address in one
	 * command, and it looked like a radio that could not reach the internet.
	 */
	peer = pairwise ? (mac_addr ? mac_addr : wlan->bss.bssid)
			: j36_wlan_group_addr;
	j36_wlan_drain_tx(wlan);
	ret = j36_wlan_cmd_install_key(w, peer, key_index, pairwise,
				       params->key);
	if (ret)
		goto out;

	if (pairwise) {
		wlan->ptk_ready = true;
		wlan->gtk_deadline = ktime_add_ms(ktime_get(),
						  J36_WLAN_GTK_GRACE_MS);
	} else {
		wlan->gtk_ready = true;
	}
	j36_wlan_settle_keys(wlan);
	j36_wlan_kick(wlan);
out:
	mutex_unlock(&w->lock);
	return ret;
}

static int j36_wlan_cfg_del_key(struct wiphy *wiphy, struct net_device *ndev,
				int link_id, u8 key_index, bool pairwise,
				const u8 *mac_addr)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;
	int ret;

	mutex_lock(&w->lock);
	if (wlan->state != J36_WLAN_CONNECTED) {
		/* The station record is already gone and its keys with it. */
		ret = 0;
		goto out;
	}
	/* Same address rule as .add_key, and it has to be the same or the remove
	 * names a key that was never installed: the firmware finds the entry by the
	 * address it went in under. */
	ret = j36_wlan_cmd_remove_key(w,
				      pairwise ? (mac_addr ? mac_addr
							   : wlan->bss.bssid)
					       : j36_wlan_group_addr,
				      key_index, pairwise);
out:
	mutex_unlock(&w->lock);
	return ret;
}

/*
 * Accepted and acted on by doing nothing, which for a station is the whole of
 * the correct behaviour: "the default key" selects which group key a frame is
 * TRANSMITTED under, and a station never transmits a group-addressed frame.  The
 * receive side picks its key from the index in each frame's own header, and that
 * index went in with the key.
 */
static int j36_wlan_cfg_set_default_key(struct wiphy *wiphy,
					struct net_device *ndev, int link_id,
					u8 key_index, bool unicast,
					bool multicast)
{
	return 0;
}

static int j36_wlan_cfg_get_station(struct wiphy *wiphy,
				    struct net_device *ndev, const u8 *mac,
				    struct station_info *sinfo)
{
	struct j36_wlan *wlan = wiphy_priv(wiphy);
	struct j36_wifi *w = wlan->w;
	int ret = 0;

	mutex_lock(&w->lock);
	if (wlan->state != J36_WLAN_CONNECTED ||
	    !j36_same_mac(mac, wlan->bss.bssid)) {
		ret = -ENOENT;
		goto out;
	}

	/* This is what wpa_supplicant's SIGNAL_POLL reads and therefore what the
	 * dashboard's signal bar is showing; the number is the last beacon this
	 * radio reported an RCPI for. */
	sinfo->filled = BIT_ULL(NL80211_STA_INFO_SIGNAL) |
			BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
			BIT_ULL(NL80211_STA_INFO_RX_BYTES64);
	sinfo->signal = (s8)clamp_t(s16, wlan->bss.signal, -128, 0);
	sinfo->tx_packets = (u32)ndev->stats.tx_packets;
	sinfo->rx_packets = (u32)ndev->stats.rx_packets;
	sinfo->tx_bytes = ndev->stats.tx_bytes;
	sinfo->rx_bytes = ndev->stats.rx_bytes;
out:
	mutex_unlock(&w->lock);
	return ret;
}

/* One interface, one type.  wpa_supplicant reads the mode back before it sets
 * it, so this is only ever reached by something asking for a mode this radio
 * does not have -- and saying no is the answer that lets it move on. */
static int j36_wlan_cfg_change_iface(struct wiphy *wiphy,
				     struct net_device *ndev,
				     enum nl80211_iftype type,
				     struct vif_params *params)
{
	if (type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	ndev->ieee80211_ptr->iftype = type;
	return 0;
}

static const struct cfg80211_ops j36_wlan_cfg80211_ops = {
	.scan			= j36_wlan_cfg_scan,
	.connect		= j36_wlan_cfg_connect,
	.disconnect		= j36_wlan_cfg_disconnect,
	.add_key		= j36_wlan_cfg_add_key,
	.del_key		= j36_wlan_cfg_del_key,
	.set_default_key	= j36_wlan_cfg_set_default_key,
	.get_station		= j36_wlan_cfg_get_station,
	.change_virtual_intf	= j36_wlan_cfg_change_iface,
};

/* ── attach and detach ───────────────────────────────────────────────────────*/

static void j36_wlan_build_band(struct j36_wlan *wlan)
{
	unsigned int i;

	for (i = 0; i < J36_WLAN_CHANNELS; i++) {
		wlan->channels[i].band = NL80211_BAND_2GHZ;
		wlan->channels[i].center_freq = j36_wlan_channel_freq[i];
		wlan->channels[i].hw_value = i + 1;
		wlan->channels[i].max_power = 20;
	}
	memcpy(wlan->rates, j36_wlan_rate_table, sizeof(wlan->rates));

	wlan->band.band = NL80211_BAND_2GHZ;
	wlan->band.channels = wlan->channels;
	wlan->band.n_channels = J36_WLAN_CHANNELS;
	wlan->band.bitrates = wlan->rates;
	wlan->band.n_bitrates = J36_WLAN_RATES;
	/*
	 * No HT capability is advertised, and the association request does not
	 * carry one either.  That is a throughput ceiling of 54 Mb/s and it is
	 * deliberate: this driver declares a non-QoS peer in the station record
	 * and takes the non-QoS arm for its data, and claiming 802.11n in the
	 * one place it is not claimed in the other two is how a link comes up
	 * and then carries nothing.
	 */
}

int j36_wlan_net_attach(struct j36_wifi *w)
{
	struct j36_wlan *wlan;
	struct wiphy *wiphy;
	struct net_device *ndev;
	int ret;

	if (w->wlan)
		return 0;

	/*
	 * The radio's own runtime configuration first, under the lock, because
	 * it is a conversation with the firmware: the station address comes back
	 * from it and everything below needs that address.
	 */
	mutex_lock(&w->lock);
	ret = j36_wlan_cmd_configure(w);
	mutex_unlock(&w->lock);
	if (ret)
		return ret;

	wiphy = wiphy_new(&j36_wlan_cfg80211_ops, sizeof(*wlan));
	if (!wiphy) {
		j36_wifi_fail(w, "wlan-wiphy-alloc",
			      "there was no memory for a wiphy");
		return -ENOMEM;
	}
	wlan = wiphy_priv(wiphy);
	wlan->w = w;
	wlan->wiphy = wiphy;
	wlan->sta_index = J36_STA_INDEX_NOT_FOUND;
	skb_queue_head_init(&wlan->tx_queue);
	INIT_DELAYED_WORK(&wlan->poll, j36_wlan_poll_work);

	/*
	 * Its own single-threaded queue rather than system_wq: this worker holds
	 * a mutex and sleeps in the page accounting for up to 200 ms, and one
	 * poll at a time is not just acceptable but required -- two would be two
	 * readers of one FIFO.
	 */
	wlan->wq = alloc_ordered_workqueue("j36-wlan", 0);
	if (!wlan->wq) {
		ret = -ENOMEM;
		goto free_wiphy;
	}

	j36_wlan_build_band(wlan);
	set_wiphy_dev(wiphy, w->dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->bands[NL80211_BAND_2GHZ] = &wlan->band;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 1;
	/*
	 * nl80211 rejects a scan whose IEs are longer than this before the
	 * driver ever sees it, so a zero here would turn every supplicant that
	 * appends anything to a probe request -- WPS, MBO, plain extended
	 * capabilities -- into "scan trigger failed" and no networks at all.
	 * The firmware builds its own probe request, so the room is advertised
	 * and the IEs are dropped in j36_wlan_cfg_scan().
	 */
	wiphy->max_scan_ie_len = 256;
	wiphy->cipher_suites = j36_wlan_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(j36_wlan_cipher_suites);
	wiphy->akm_suites = j36_wlan_akm_suites;
	wiphy->n_akm_suites = ARRAY_SIZE(j36_wlan_akm_suites);
	wiphy->regulatory_flags = REGULATORY_CUSTOM_REG;
	wiphy_apply_custom_regulatory(wiphy, &j36_wlan_regdom);

	ret = wiphy_register(wiphy);
	if (ret) {
		j36_wifi_fail(w, "wlan-wiphy-register",
			      "cfg80211 refused the wiphy (%d)", ret);
		goto free_wq;
	}

	ndev = alloc_netdev(sizeof(struct j36_wlan *), "wlan%d", NET_NAME_ENUM,
			    ether_setup);
	if (!ndev) {
		ret = -ENOMEM;
		goto unregister_wiphy;
	}
	*(struct j36_wlan **)netdev_priv(ndev) = wlan;
	wlan->ndev = ndev;
	wlan->wdev.wiphy = wiphy;
	wlan->wdev.iftype = NL80211_IFTYPE_STATION;
	wlan->wdev.netdev = ndev;
	ndev->ieee80211_ptr = &wlan->wdev;
	ndev->netdev_ops = &j36_wlan_netdev_ops;
	ndev->needs_free_netdev = true;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	eth_hw_addr_set(ndev, w->mac);

	/* Published before the interface exists, so nothing can arrive through
	 * cfg80211 or the pump and find a half-built stage 4. */
	mutex_lock(&w->lock);
	w->wlan = wlan;
	mutex_unlock(&w->lock);

	/* Not under w->lock: this takes the RTNL, and ndo_open takes w->lock
	 * under it. */
	ret = register_netdev(ndev);
	if (ret) {
		mutex_lock(&w->lock);
		w->wlan = NULL;
		mutex_unlock(&w->lock);
		j36_wifi_fail(w, "wlan-netdev-register",
			      "the network interface would not register (%d)",
			      ret);
		goto free_netdev;
	}
	return 0;

free_netdev:
	wlan->ndev = NULL;
	free_netdev(ndev);
unregister_wiphy:
	wiphy_unregister(wiphy);
free_wq:
	destroy_workqueue(wlan->wq);
free_wiphy:
	wiphy_free(wiphy);
	return ret;
}

void j36_wlan_net_detach(struct j36_wifi *w)
{
	struct j36_wlan *wlan = w->wlan;

	if (!wlan)
		return;

	/*
	 * Withdrawn first, so that anything still in the pump stops finding a
	 * stage 4 to hand packets to.  The netdev keeps its own pointer, which
	 * is what ndo_stop below uses.
	 */
	mutex_lock(&w->lock);
	w->wlan = NULL;
	mutex_unlock(&w->lock);

	/* Calls ndo_stop, which takes w->lock -- so this cannot be under it. */
	unregister_netdev(wlan->ndev);
	wlan->ndev = NULL;

	cancel_delayed_work_sync(&wlan->poll);
	destroy_workqueue(wlan->wq);
	skb_queue_purge(&wlan->tx_queue);

	wiphy_unregister(wlan->wiphy);
	wiphy_free(wlan->wiphy);
}

const char *j36_wlan_net_interface(struct j36_wifi *w)
{
	struct j36_wlan *wlan = w->wlan;

	if (!wlan || !wlan->ndev)
		return NULL;
	return wlan->ndev->name;
}

/*
 * One line, and every number in it separates two failures that look identical
 * from userspace: an interface that scans but never joins from one that joins
 * and is thrown off, a transmit queue backing up from one whose frames the
 * firmware will not take a page for, and a radio hearing nothing from one whose
 * results never reached cfg80211.
 */
void j36_wlan_net_trace(struct j36_wifi *w)
{
	struct j36_wlan *wlan = w->wlan;

	if (!wlan)
		return;
	dev_info(w->dev,
		 "wlan: %u scans, %u BSS known, %u joins, %u refused, %u links lost, %u re-arms; peer record %u%s, keys %s/%s%s, power-save %s; tx %u frames %u deferred %u backpressure %u starved (%u deep now); rx %u events %u mgmt (%u of them this AP's beacons) %u data\n",
		 wlan->scans, wlan->result_count, wlan->joins,
		 wlan->join_failures, wlan->link_losses, wlan->recoveries,
		 wlan->sta_index,
		 wlan->sta_confirmed ? " (firmware confirmed)"
				     : " (no ACTIVATE_STA_REC)",
		 wlan->ptk_ready ? "PTK" : "-",
		 wlan->gtk_ready ? "GTK" : "-",
		 wlan->key_ready ? ", encryption enabled" : "",
		 wlan->pm_sent ? "indicated" : "NOT indicated",
		 w->hif_stats.tx_frames, wlan->tx_deferred,
		 wlan->tx_backpressure, wlan->tx_starved,
		 skb_queue_len(&wlan->tx_queue), w->hif_stats.rx_events,
		 w->hif_stats.rx_management, wlan->beacons,
		 w->hif_stats.rx_data);
}
