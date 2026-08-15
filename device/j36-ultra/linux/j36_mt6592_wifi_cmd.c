// SPDX-License-Identifier: GPL-2.0
/*
 * J36 Ultra MT6592 CONSYS Wi-Fi, stage 4a: the running firmware's own command
 * and event protocol.
 *
 * Ported from PowerEngine/OS/MVII's mt6592_wifi_hif.c, whose command builders
 * were established against this board and this firmware one refused command at a
 * time.  Every byte offset below is traced in the comments to MediaTek's own
 * struct it came from -- CMD_UPDATE_STA_RECORD_T, CMD_SET_BSS_INFO_T and the
 * rest -- because a field written to the wrong offset here is not a compile
 * error and not a runtime error either: it is a firmware that accepts the
 * command, does nothing with it, and never says why.
 *
 *
 * ── WHAT THIS FILE IS ──
 *
 * Stage 3 ends at WLAN_READY: a firmware that is running and a FIFO pair that
 * carries whole packets.  This file is the vocabulary spoken over that pair.  It
 * is two halves and they meet nowhere except in struct j36_wifi:
 *
 *   downwards	one function per command the join needs, each one building
 *		MediaTek's own byte layout and handing it to the transport.
 *   upwards	one pump that drains the receive port, sorts what comes out
 *		into events, management frames and data, and calls
 *		j36_mt6592_wifi_net.c for each.
 *
 * There is no state machine here.  Which command comes next, what a grant means,
 * when a join has failed -- all of that is stage 4b's, because it is cfg80211's
 * question and this file has never heard of cfg80211.  What this file owns is
 * four counters that belong to the wire rather than to the policy: the frame tag,
 * the scan sequence, the channel token, and which EAPOL frame is still in the
 * firmware's transmit queue.
 *
 *
 * ── WHAT THE PORT CHANGED ──
 *
 * MVII computes its own PMK and runs its own four-way handshake, because it has
 * no supplicant.  This kernel has one, so all of that is gone: the EAPOL frames
 * are ordinary data on the transmit and receive paths here and wpa_supplicant
 * does the cryptography.  What survives from that code is the one thing the
 * supplicant cannot do for us, which is the ORDERING -- see the wait at the top
 * of j36_wlan_cmd_install_key().
 *
 * The scan-result bookkeeping is gone too.  MVII keeps its own table because
 * nothing else would; cfg80211 keeps one, so a beacon here is parsed straight
 * into it and forgotten.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "j36_mt6592_wifi.h"

/* ── little-endian fields, a byte at a time ──────────────────────────────────
 *
 * Same reasoning as the transport's copies: these are wire layouts at whatever
 * offset the peer chose, in u8 arrays nothing has declared an alignment for.
 */
static void j36_put_le16(u8 *p, u16 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
}

static void j36_put_le32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

static u16 j36_get_le16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 j36_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

/* memcmp rather than ether_addr_equal(): that one requires both addresses to be
 * u16-aligned and half of these are at packet + 12 + (packet[4] & 3), which is
 * whatever alignment the firmware felt like. */
static bool j36_same_mac(const u8 *a, const u8 *b)
{
	return !memcmp(a, b, ETH_ALEN);
}

/* ── one command out ─────────────────────────────────────────────────────────
 *
 * The 8-byte header is WIFI_CMD_T's: [0..1] the transfer length, [3] the traffic
 * class and packet type, [4] the command id, [5] set-or-query, [6] the sequence
 * the answering event echoes back.
 *
 * The length written is the ALIGNED one, unlike a data frame's, which is the
 * real one.  That asymmetry is stock's and it is not a rounding convenience: a
 * command has no payload the firmware measures, so the padding is part of the
 * command; a frame's trailing bytes are not part of the frame.
 */
static int j36_wlan_command(struct j36_wifi *w, u8 cid, bool set,
			    const u8 *payload, u32 payload_len)
{
	const u32 packet_size = J36_HIF_CMD_HEADER_SIZE + payload_len;
	const u32 transfer = ALIGN(packet_size, 4);
	u8 *packet = w->hif_tx;
	int ret;

	if (transfer > J36_HIF_TX_BUFFER_SIZE || transfer > 0x0fff)
		return -EMSGSIZE;
	if (!w->firmware_alive)
		return -ENODEV;

	memset(packet, 0, transfer);
	j36_put_le16(packet, (u16)transfer);
	packet[3] = (J36_HIF_TC_COMMAND << J36_HIF_TX_RESOURCE_SHIFT) |
		    (J36_HIF_PACKET_TYPE_CMD << J36_HIF_TX_PACKET_TYPE_SHIFT);
	packet[4] = cid;
	packet[5] = set ? 1 : 0;
	packet[6] = j36_wifi_hif_sequence(w);
	if (payload && payload_len)
		memcpy(packet + J36_HIF_CMD_HEADER_SIZE, payload, payload_len);

	/* Every command is a TC4 packet and TC4 owns four pages; a join sends
	 * seven of them close together.  The acquire is not optional -- see
	 * j36_wifi_hif_submit(). */
	ret = j36_wifi_hif_tx_acquire(w, J36_HIF_TC_COMMAND);
	if (ret)
		return ret;
	j36_wifi_hif_submit(w, J36_HIF_TC_COMMAND, packet, transfer);
	w->hif_stats.tx_commands++;
	return 0;
}

/* ── one frame out ───────────────────────────────────────────────────────────
 *
 * The 16-byte transmit descriptor, and the derivation that is not in it.
 *
 * The traffic class follows the station record index and not the other way
 * round, in that order, because that is the order qmEnqueueTxPackets does it in:
 * qmDetermineStaRecIndex reads the ETHERNET destination first and a group
 * address there is STA_REC_INDEX_BMCAST before anything else is looked at, and
 * only then does the switch on the record pick the class -- TC5 for BMCAST or
 * NOT_FOUND, TC1 for a valid record with no QoS.  Management frames are forced
 * to TC4 from elsewhere.  802.1X is TC4 too, because stock never sends it down
 * this path at all -- wlanHardStartXmit turns an EAPOL frame into a security
 * command on the command queue, which is TC4's port.
 *
 * The port follows the class and nothing else: WTDR1 for TC4, WTDR0 for the
 * rest.  A management frame written to WTDR0 is accepted and never transmitted.
 *
 * ── THE BROADCAST ARM: WHY IT LEFT, AND WHY IT IS BACK ───────────────────────
 *
 * It was taken out on the argument that a station never puts a group-addressed
 * frame onto the medium -- true; it goes out as a ToDS unicast to the AP under
 * the pairwise key, Addr1 the BSSID, Addr2 us, ff:ff:ff:ff:ff:ff carried in
 * Addr3 -- and that TC5's single page would therefore never be credited back.
 * The 802.11 half of that is right.  The conclusion drawn from it is not:
 *
 *   The record is not this driver's decision to make.  qmDetermineStaRecIndex
 *   says so in its own comment, on the arm in question: "For intrastructure mode
 *   and P2P (playing as a GC), BMCAST frames shall be sent to the AP.  FW SHALL
 *   TAKE CARE OF THIS.  The host driver is not able to distinguish these cases."
 *   Stock takes that arm for an AIS station in infrastructure mode with no
 *   exception anywhere.  The firmware is what turns the BMCAST record into a
 *   frame for the AP under the pairwise key -- that is the whole reason the
 *   comment is there.  Naming the AP's record instead tells the firmware the
 *   destination is a peer that the Ethernet header in the same packet says it is
 *   not, and what it does with that contradiction is undocumented and silent.
 *
 *   TC5 is credited back exactly like every other class.  nicTxReleaseResource
 *   adds aucTxRlsCnt[i] for i = 0..5 out of the same two registers -- WTSR0
 *   carries TC0..TC3, WTSR1 carries TC4 and TC5 -- and j36_hif_tx_release()
 *   already decodes all six of them.  There is no arm anywhere, in stock or
 *   here, that skips TC5.
 *
 * What actually wedged in the version that was reverted is visible in its own
 * diff.  It sent BOTH the BMCAST record AND STA_REC_INDEX_NOT_FOUND to TC5:
 *
 *     tc = (record == J36_STA_INDEX_BMCAST ||
 *           record == J36_STA_INDEX_NOT_FOUND) ? TC5 : TC1;
 *
 * and wlan->sta_index sits at NOT_FOUND until the firmware confirms the station
 * record, so every UNICAST frame sent before that confirmation went onto a class
 * with one page too.  The confirmation works now -- "peer record 0 (firmware
 * confirmed)" in this driver's own status line -- so unicast stays on TC1's
 * twenty pages and only the genuinely group-addressed frames take TC5's one,
 * which is all stock ever needs at a time.  A starve on TC5 is no longer fatal
 * either: j36_wlan_drain_tx() retries at the head of the queue for
 * J36_WLAN_TX_STALL_MS and then drops that one frame and moves on.
 *
 * And what does a freshly associated station send?  A DHCPDISCOVER, to
 * ff:ff:ff:ff:ff:ff, because it has no address yet to be spoken to at.  Then
 * ARP, broadcast.  Then IPv6 multicast.  For the whole of a failed DHCP there is
 * not one unicast data frame in the trace -- eighteen frames out, none deferred,
 * none starved, nothing back, forty-five seconds, twice -- so the group-
 * addressed path is the ONLY path that was ever exercised after the handshake.
 * "Joined, but the router never sent an address" was never about the router.
 */
static int j36_wlan_frame(struct j36_wifi *w, const u8 *frame, u32 frame_len,
			  u8 packet_type, u8 sta_index, bool is_80211,
			  bool is_1x, bool basic_rate, bool may_wait, u8 *tag_out)
{
	const u32 packet_size = J36_HIF_DATA_HEADER_SIZE + frame_len;
	const u32 transfer = ALIGN(packet_size, 4);
	/*
	 * qmDetermineStaRecIndex' first test, and it is on the ETHERNET
	 * destination, so only an 802.3 data frame can reach it: an 802.11 frame
	 * begins with a frame control field and its first octet's low bit means
	 * nothing here.  802.1X is excluded as well -- it is never group-addressed
	 * and its whole point on this path is TC4 with the 1X flag set, which is
	 * what tells the firmware to leave it in the clear.
	 */
	const bool group = !is_80211 && !is_1x &&
			   packet_type == J36_HIF_PACKET_TYPE_DATA &&
			   frame_len >= ETH_ALEN &&
			   is_multicast_ether_addr(frame);
	const u8 record = group ? J36_STA_INDEX_BMCAST : sta_index;
	const u8 tc = (packet_type == J36_HIF_PACKET_TYPE_MGMT || is_1x) ?
			J36_HIF_TC_COMMAND :
			(group ? J36_HIF_TC_BROADCAST : J36_HIF_TC_DATA);
	/*
	 * ucPacketSeqNo and the NEED_ACK bit move together, because the sequence
	 * only means anything as the tag on the TX-status event NEED_ACK asks
	 * for.  A nonzero tag without the bit is a number nothing will report.
	 */
	const bool need_ack = packet_type == J36_HIF_PACKET_TYPE_MGMT || is_1x;
	u8 *packet = w->hif_tx;
	u8 tag = 0;
	int ret;

	if (tag_out)
		*tag_out = 0;
	if (!frame || !frame_len || transfer > J36_HIF_TX_BUFFER_SIZE ||
	    packet_size > 0x0fff)
		return -EMSGSIZE;
	if (!w->firmware_alive)
		return -ENODEV;

	memset(packet, 0, transfer);
	j36_put_le16(packet, (u16)packet_size);
	packet[2] = (u8)((J36_HIF_DATA_HEADER_SIZE + (is_80211 ? 24 : 12)) >> 1);
	packet[3] = (tc << J36_HIF_TX_RESOURCE_SHIFT) |
		    ((packet_type & 0x3) << J36_HIF_TX_PACKET_TYPE_SHIFT);
	packet[4] = is_80211 ? 24 : 14;
	packet[5] = (J36_NETWORK_TYPE_AIS << 4) | (is_1x ? BIT(6) : 0) |
		    (is_80211 ? BIT(7) : 0);
	packet[10] = record;
	packet[11] = BIT(5);	/* burst end */
	if (need_ack) {
		if (++w->frame_sequence == 0)
			w->frame_sequence = 1;
		tag = w->frame_sequence;
		packet[12] = tag;
		packet[13] = BIT(0) | (basic_rate ? BIT(2) : 0);
	} else {
		packet[13] = basic_rate ? BIT(2) : 0;
	}
	memcpy(packet + J36_HIF_DATA_HEADER_SIZE, frame, frame_len);

	ret = may_wait ? j36_wifi_hif_tx_acquire(w, tc) :
			 j36_wifi_hif_tx_try(w, tc);
	if (ret)
		return ret;
	j36_wifi_hif_submit(w, tc, packet, transfer);
	w->hif_stats.tx_frames++;
	if (tag_out)
		*tag_out = tag;
	return 0;
}

/* ── the commands ────────────────────────────────────────────────────────────*/

/*
 * The rate bitmaps.  MediaTek numbers the fourteen 802.11b/g rates in the order
 * they appear in a supported-rates element, which is neither ascending nor
 * grouped by modulation, so this is a table and not an expression.  The high bit
 * of a rate byte is the "basic" flag and is masked off before the lookup.
 */
static u16 j36_rate_bit(u8 rate)
{
	switch (rate & 0x7f) {
	case 2:		return BIT(0);	/* 1 Mb/s   */
	case 4:		return BIT(1);	/* 2        */
	case 11:	return BIT(2);	/* 5.5      */
	case 22:	return BIT(3);	/* 11       */
	case 44:	return BIT(4);	/* 22       */
	case 66:	return BIT(5);	/* 33       */
	case 12:	return BIT(6);	/* 6        */
	case 18:	return BIT(7);	/* 9        */
	case 24:	return BIT(8);	/* 12       */
	case 36:	return BIT(9);	/* 18       */
	case 48:	return BIT(10);	/* 24       */
	case 72:	return BIT(11);	/* 36       */
	case 96:	return BIT(12);	/* 48       */
	case 108:	return BIT(13);	/* 54       */
	default:	return 0;
	}
}

/* The two fallbacks below are stock's own 802.11bg sets, used when a beacon
 * carried no rates element at all: every b and g rate operational, the four b
 * rates plus 6/12/24 basic. */
#define J36_RATES_DEFAULT_OPERATIONAL	0x3fcf
#define J36_RATES_DEFAULT_BASIC		0x044f

static u16 j36_bss_operational(const struct j36_wlan_bss *bss)
{
	return bss->operational_rates ? bss->operational_rates :
				        J36_RATES_DEFAULT_OPERATIONAL;
}

static u16 j36_bss_basic(const struct j36_wlan_bss *bss)
{
	return bss->basic_rates ? bss->basic_rates : J36_RATES_DEFAULT_BASIC;
}

/*
 * assoc.c:793-805, one number written to two places: the listen interval in the
 * station record IS the interval the association request advertised.  Telling
 * the AP one wake-up schedule and the firmware another is how a station ends up
 * being buffered for on a cadence it does not keep.
 */
static u16 j36_listen_interval(const struct j36_wlan_bss *bss)
{
	return bss->dtim_period ? (u16)bss->dtim_period * 2 : 10;
}

/*
 * CMD_UPDATE_STA_RECORD_T, 40 bytes.  The fields that are not obvious:
 *
 *   1  ucStaType		STA_TYPE_LEGACY_AP = LEGACY | AP = 0x41
 *   13 ucDesiredPhyTypeSet	PHY_TYPE_SET_802_11BGN = 0x13
 *   18 ucIsQoS			0, and consistently 0: this driver sends no WMM
 *				IE, so it declares a non-QoS peer here, a
 *				non-QBSS in SET_BSS_INFO, and takes the
 *				`!fgIsQoS -> TC1' arm for its data.  Claiming
 *				QoS in one of the three is what would be wrong.
 *   21 ucMcsSet		0xff, MCS 0..7
 *   34 ucNeedResp		THE GATE ON THE WHOLE DATA PATH.  Without it the
 *				firmware sends no ACTIVATE_STA_REC event, without
 *				that event the record never becomes valid, and
 *				without a valid record every unicast data frame
 *				goes out as STA_INDEX_NOT_FOUND.  Stock sets it
 *				exactly on a transition into STA_STATE_3.
 */
int j36_wlan_cmd_sta_record(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			    u8 state, u16 aid)
{
	u8 command[40] = {};

	command[0] = J36_STA_RECORD_INDEX;
	command[1] = 0x41;
	memcpy(command + 2, bss->bssid, ETH_ALEN);
	j36_put_le16(command + 8, aid);
	j36_put_le16(command + 10, j36_listen_interval(bss));
	command[12] = J36_NETWORK_TYPE_AIS;
	command[13] = 0x13;
	j36_put_le16(command + 14, j36_bss_operational(bss));
	j36_put_le16(command + 16, j36_bss_basic(bss));
	command[20] = state;
	command[21] = 0xff;
	command[33] = bss->rcpi;
	command[34] = state == J36_STA_STATE_3 ? 1 : 0;
	return j36_wlan_command(w, J36_CMD_UPDATE_STA_RECORD, true, command,
				sizeof(command));
}

/*
 * CID 0x18, and the one this port must not confuse with 0x17 above: 0x18 forgets
 * the peer.  Sending a station record under it removes a record that was never
 * added, and the firmware answers by going silent rather than by refusing.
 */
int j36_wlan_cmd_sta_remove(struct j36_wifi *w, const struct j36_wlan_bss *bss)
{
	u8 command[8] = {};

	command[0] = J36_NETWORK_TYPE_AIS;
	memcpy(command + 1, bss->bssid, ETH_ALEN);
	return j36_wlan_command(w, J36_CMD_REMOVE_STA_RECORD, true, command,
				sizeof(command));
}

/*
 * CMD_SET_BSS_INFO, 80 bytes: a 64-byte body with a 16-byte RLM parameter block
 * welded onto the end.  The fields worth naming:
 *
 *   1  ucConnectionState	CONNECTED 1, DISCONNECTED 0
 *   51 ucNonHTBasicPhyType	PHY_TYPE_ERP_INDEX = 1, which is every 2.4 GHz
 *				g/n AP; the OFDM and HR_DSSS arms are 5 GHz and
 *				b-only
 *   52 ucAuthMode		OPEN 0, WPA2_PSK 7
 *   53 ucEncStatus		DISABLED 1, ENCRYPTION3_ENABLED 6,
 *				ENCRYPTION3_KEY_ABSENT 7.  Note the aliasing in
 *				MediaTek's enum: WEP_ENABLED is 0, so "disabled"
 *				is 1 and not 0.
 *   77 ucUseShortPreamble	the AP's own capability BIT(5)
 *   78 ucUseShortSlotTime	the AP's own capability BIT(10)
 *   79 ucCheckId		0x72, the firmware's guard that the 16-byte tail
 *				arrived intact.  The last byte of an 80-byte body
 *				is the one that must not be padding.
 *
 * The protection-mode bytes at 68..71 and the HT operation words at 72..76 stay
 * zero deliberately: this driver associates as a 20 MHz non-QoS station and the
 * settings that pair with that are all-zero.  They cost throughput near legacy
 * traffic; they cannot cost the association.
 *
 * ── ucEncStatus IS THE CIPHER, NOT THE STATE OF THE HANDSHAKE ────────────────
 *
 * It used to be 7 (ENCRYPTION3_KEY_ABSENT) at join and this command was then
 * SENT A SECOND TIME after the four-way handshake, purely to move it to 6.  That
 * is not what the field means and stock does neither half of it:
 * mtk_cfg80211_connect calls wlanoidSetEncryptionStatus with
 * ENUM_ENCRYPTION3_ENABLED -- 6 -- from the cipher suite in the connect request,
 * before a single key exists, and nicUpdateBss copies that value straight out of
 * prConnSettings->eEncStatus.  KEY_ABSENT is the arm stock takes for BOW and for
 * P2P, never for AIS.
 *
 * And the second send has no equivalent anywhere: every nicUpdateBss() call site
 * in the tree is a join, a disconnect, an IBSS transition or a roam, and not one
 * of them is on the key path.  Re-sending a command that describes the whole BSS
 * -- BSSID, rates, PHY, security -- at the instant the pairwise and group keys
 * have just been written is asking a firmware that may reinitialise its BSS
 * security context on receipt to discard both of them, and the symptom of that
 * would be precisely what was seen: a handshake that completes and then not one
 * encrypted frame in either direction.
 */
int j36_wlan_cmd_bss_info(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			  bool secure)
{
	u8 command[80] = {};

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = 1;			/* CONNECTED		*/
	command[2] = 0;			/* INFRASTRUCTURE	*/
	command[3] = bss->ssid_len;
	memcpy(command + 4, bss->ssid, min_t(u8, bss->ssid_len,
					     IEEE80211_MAX_SSID_LEN));
	memcpy(command + 36, bss->bssid, ETH_ALEN);
	j36_put_le16(command + 44, j36_bss_operational(bss));
	j36_put_le16(command + 46, j36_bss_basic(bss));
	command[48] = J36_STA_RECORD_INDEX;
	command[51] = 1;		/* PHY_TYPE_ERP_INDEX	*/
	command[52] = secure ? 7 : 0;
	command[53] = secure ? 6 : 1;
	command[54] = 0x13;		/* PHY_TYPE_SET_802_11BGN */
	memcpy(command + 55, w->mac, ETH_ALEN);
	command[64] = J36_NETWORK_TYPE_AIS;
	command[65] = 1;		/* BAND_2G4		*/
	command[66] = bss->channel ? bss->channel : 1;
	command[77] = (bss->capability & BIT(5)) ? 1 : 0;
	command[78] = (bss->capability & BIT(10)) ? 1 : 0;
	command[79] = 0x72;
	return j36_wlan_command(w, J36_CMD_SET_BSS_INFO, true, command,
				sizeof(command));
}

/*
 * The same command with the one field that matters inverted.  Everything else is
 * still the live BSS on purpose: the firmware matches this against the BSS it is
 * holding, and a disconnect for a BSS described differently from the one it was
 * given is a disconnect for nothing.  So the caller sends this BEFORE it forgets
 * which network it was on.
 *
 * ucStaRecIdxOfAP is NOT_FOUND rather than 0, which is nicUpdateBss's else-arm --
 * reached exactly when there is no AP record to name.  A disconnect that still
 * claims record 0 is the AP leaves the firmware holding the association it was
 * told to drop, and the mistake is invisible because it is a zero nobody wrote.
 */
int j36_wlan_cmd_bss_disconnect(struct j36_wifi *w,
				const struct j36_wlan_bss *bss)
{
	u8 command[80] = {};

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = 0;			/* DISCONNECTED		*/
	command[2] = 0;			/* INFRASTRUCTURE	*/
	memcpy(command + 36, bss->bssid, ETH_ALEN);
	command[48] = J36_STA_INDEX_NOT_FOUND;
	memcpy(command + 55, w->mac, ETH_ALEN);
	command[64] = J36_NETWORK_TYPE_AIS;
	command[65] = 1;		/* BAND_2G4		*/
	command[66] = bss->channel ? bss->channel : 1;
	command[79] = 0x72;		/* written every time, connect or not */
	return j36_wlan_command(w, J36_CMD_SET_BSS_INFO, true, command,
				sizeof(command));
}

/*
 * CMD_BSS_ACTIVATE_CTRL, 4 bytes, sent twice: off, then on.
 *
 * SET_BSS_INFO(DISCONNECTED) alone does not clear the firmware's per-BSS
 * context, so its receive filter goes on claiming that BSSID's beacons for a BSS
 * nothing is servicing and they never reach a scan.  Measured on this board: an
 * AP at -46 dBm absent from all three scans taken after its deauth, and present
 * at -47 in a scan taken while associated to a different one.  Deactivating and
 * immediately reactivating makes the firmware rebuild the context, and the AIS
 * network stays live so nothing here has to track an inactive BSS.
 *
 * THE SECOND HALF IS THE ONE THAT MATTERS AND IT IS THE ONE MOST LIKELY TO FAIL.
 * It is the fourth or fifth command of a teardown burst on a class with four
 * pages, so it is exactly where the accounting runs out -- and a refused "on"
 * leaves the AIS network deactivated, which is a radio that accepts every later
 * command and answers none of them: no channel grants, no scan results, nothing
 * to log.  So the off half may fail and be reported, but the on half is retried
 * until a page comes free or the transport is gone.
 */
int j36_wlan_cmd_bss_reactivate(struct j36_wifi *w)
{
	unsigned int attempt;
	u8 command[4] = {};
	int ret;

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = 0;
	ret = j36_wlan_command(w, J36_CMD_BSS_ACTIVATE_CTRL, true, command,
			       sizeof(command));
	if (ret)
		return ret;

	command[1] = 1;
	for (attempt = 0; attempt < J36_WLAN_ACTIVATE_ATTEMPTS; attempt++) {
		ret = j36_wlan_command(w, J36_CMD_BSS_ACTIVATE_CTRL, true,
				       command, sizeof(command));
		if (ret != -EBUSY)
			return ret;
	}
	dev_err(w->dev,
		"wlan: the AIS network could not be reactivated -- the radio is deactivated and will answer nothing\n");
	return ret;
}

/*
 * CMD_INDICATE_PM_BSS_CONNECTED, 12 bytes -- and WHEN, not just what.
 *
 * There is no DTIM period in an association response, and stock does not invent
 * one: aisUpdateBssInfoForJOIN sets ucDTIMPeriod to 0 and leaves a comment saying
 * the indication is deferred to the first beacon after connection, where
 * scanProcessBeaconAndProbeResp sends it -- guarded on not having sent it yet, on
 * the BSSID matching, on being an infrastructure station, and on the frame being
 * a BEACON rather than a probe response, because only a beacon carries the TIM
 * element the DTIM count lives in.
 *
 * So the beacon is waited for rather than guessed at -- but only for a while, and
 * this is where MVII's port and stock part company for a reason worth writing
 * down.  Stock waits forever because stock always gets its beacon: its receive
 * path hands every beacon of the connected BSS to scanProcessBeaconAndProbeResp
 * whether it is scanning or not.  This driver, on this firmware, has been
 * observed to get none at all while associated -- 47 management frames in a whole
 * session, all of them accounted for by four scans -- so "wait for a beacon" here
 * means "never send it", and never sending it is not the safe option:
 *
 *   SET_BSS_INFO HAS NO BEACON INTERVAL FIELD.  This command is the only one in
 *   the protocol that carries u2BeaconInterval, so a link brought up without it
 *   leaves the firmware supervising an AP it has been told beacons every zero
 *   time units.  The result is EVENT_BSS_BEACON_TIMEOUT about a minute after a
 *   perfectly good association, with the AP a metre away at -46 dBm.
 *
 * Hence dtim_fallback.  Fabricating a DTIM period of 1 is safe HERE and only
 * here, because j36_wlan_cmd_configure() puts this radio in Param_PowerModeCAM
 * and never takes it out: the receiver is on continuously, so the DTIM period
 * cannot cost a buffered frame.  It sets the cadence the firmware's supervision
 * expects and nothing else.  The day this driver learns to power-save, this
 * argument stops being true and the fallback has to go with it.
 *
 * The beacon interval falls back too, to the 100 TU every consumer AP on this
 * band ships with, and for a narrower reason: it is normally known -- the fixed
 * fields of a PROBE RESPONSE carry it just as a beacon's do, so an active scan
 * gets it -- but a BSS learned only from EVENT_SCAN_RESULT has no value for it at
 * all, and 100 TU is a far better guess than not sending the command.
 */
int j36_wlan_cmd_pm_connected(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			      u16 aid, bool dtim_fallback)
{
	u8 command[12] = {};

	if (!dtim_fallback && (!bss->dtim_period || !bss->beacon_interval))
		return -EAGAIN;

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = bss->dtim_period ? bss->dtim_period : 1;
	j36_put_le16(command + 2, aid);
	j36_put_le16(command + 4, bss->beacon_interval ? bss->beacon_interval :
							 100);
	return j36_wlan_command(w, J36_CMD_INDICATE_PM_CONNECTED, true, command,
				sizeof(command));
}

/*
 * CMD_INDICATE_PM_BSS_ABORT, 4 bytes: the network index and three reserved.
 *
 * The counterpart of the command above and, like it, easy to leave out and hard
 * to miss the absence of.  aisFsmDisconnect() sends it FIRST -- before
 * rlmBssAborted(), before the media state changes, before nicUpdateBss() -- on
 * every path that ends a link, including the one that ends it to start another.
 * Skipping it leaves the firmware's power-management module holding a BSS
 * context while the host pulls SET_BSS_INFO(DISCONNECTED), REMOVE_STA_RECORD and
 * BSS_ACTIVATE_CTRL out from under it, and what that costs is not a message: it
 * is a radio that takes the next join's commands and never answers one.
 */
int j36_wlan_cmd_pm_abort(struct j36_wifi *w)
{
	u8 command[4] = {};

	command[0] = J36_NETWORK_TYPE_AIS;
	return j36_wlan_command(w, J36_CMD_INDICATE_PM_ABORT, true, command,
				sizeof(command));
}

/*
 * CMD_802_11_KEY, 64 bytes.  ucTxKey, ucKeyType and the peer address are not
 * three independent fields -- they are three readings of the one thing cfg80211
 * passes, and mtk_cfg80211_add_key derives all of them from whether there is a
 * mac_addr: a pairwise key names the AP and sets both flags, a group key names
 * the broadcast address and sets neither.  wlanoidSetAddKey then rejects a
 * pairwise key whose id is not 0, which is why the supplicant's PTK always
 * arrives as key 0.
 *
 * aucKeyRsc AT 48 IS LEFT ZERO.  The only writer of that field anywhere in
 * MediaTek's tree is the WAPI arm, so its format for CCMP is unverifiable from
 * here, and the failure mode of guessing is not weaker replay protection -- it is
 * a silent broadcast blackhole, because a byte-swapped starting packet number
 * makes every group frame the AP sends look like a replay.
 *
 * ── THE WAIT AT THE TOP ──
 *
 * The pairwise key must not go in while the last EAPOL frame is still in the
 * firmware's transmit queue.  wpa_supplicant writes message 4 to wlan0 and then
 * immediately asks for the key; if the key lands first the firmware CCMP-encrypts
 * a frame the AP can only read in the clear, and the handshake dies one message
 * from the end.  The supplicant cannot order this -- it has no way to know when
 * the frame left the radio -- so the driver does, against the TX-status event the
 * 1X path asked for.  Ignoring the status is deliberate: a failed transmission
 * still means the frame is out of the queue, which is the only thing being
 * ordered against, and stalling on it would hang the join in the one case where
 * the AP is about to retransmit message 3 anyway.
 */
int j36_wlan_cmd_install_key(struct j36_wifi *w, const u8 *peer, u8 key_id,
			     bool pairwise, const u8 *key)
{
	u8 command[64] = {};
	unsigned int round;

	for (round = 0; pairwise && w->pending_1x &&
			round < J36_WLAN_1X_DONE_TIMEOUT_MS; round++) {
		j36_wlan_cmd_pump(w);
		if (!w->pending_1x)
			break;
		usleep_range(800, 1600);
	}
	/* Not fatal.  A TX-status event that never came is a firmware that is
	 * about to have a key installed anyway, and refusing here would turn a
	 * missed event into a network that can never be joined. */
	w->pending_1x = 0;

	command[0] = 1;			/* add			*/
	command[1] = pairwise ? 1 : 0;	/* ucTxKey		*/
	command[2] = pairwise ? 1 : 0;	/* ucKeyType		*/
	command[3] = 0;			/* we are the supplicant */
	memcpy(command + 4, peer, ETH_ALEN);
	command[10] = J36_NETWORK_TYPE_AIS;
	command[11] = 4;		/* CIPHER_SUITE_CCMP	*/
	command[12] = key_id;
	command[13] = 16;
	memcpy(command + 16, key, 16);
	return j36_wlan_command(w, J36_CMD_ADD_REMOVE_KEY, true, command,
				sizeof(command));
}

/*
 * The same command with ucAddRemove cleared, which is the only field the remove
 * path of wlanoidSetRemoveKey fills differently -- the peer, the index and the
 * pairwise flag still have to name the key being taken out.
 *
 * Worth having even though a reconnect rebuilds the station record anyway: a
 * GROUP key is not attached to the station record, so an AP that rotates its GTK
 * while we are away would otherwise be answered with the key from the previous
 * association until the next handshake replaces it.
 */
int j36_wlan_cmd_remove_key(struct j36_wifi *w, const u8 *peer, u8 key_id,
			    bool pairwise)
{
	u8 command[64] = {};

	command[0] = 0;			/* remove		*/
	command[1] = pairwise ? 1 : 0;
	command[2] = pairwise ? 1 : 0;
	command[3] = 0;
	memcpy(command + 4, peer, ETH_ALEN);
	command[10] = J36_NETWORK_TYPE_AIS;
	command[11] = 4;		/* CIPHER_SUITE_CCMP	*/
	command[12] = key_id;
	return j36_wlan_command(w, J36_CMD_ADD_REMOVE_KEY, true, command,
				sizeof(command));
}

/*
 * CMD_CH_PRIVILEGE, 20 bytes, and it is a LEASE rather than a request: byte 2
 * selects request or abort and the token in byte 1 says which outstanding lease
 * is being talked about.
 *
 * u4MaxInterval is 2000 because that is AIS_JOIN_CH_REQUEST_INTERVAL, the named
 * constant stock's own join passes.  It is how long the grant is held before it
 * lapses, not whether it is issued, but a number stock does not send has no place
 * in the one command whose silence is being investigated.
 *
 * The token starts at 1 and never returns to 0, because 0 is what an unwritten
 * field looks like and the grant event is matched on it.
 */
int j36_wlan_cmd_channel_request(struct j36_wifi *w,
				 const struct j36_wlan_bss *bss, u8 *token_out)
{
	u8 command[20] = {};
	int ret;

	if (++w->channel_token == 0)
		w->channel_token = 1;

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = w->channel_token;
	command[2] = 0;			/* CMD_CH_ACTION_REQ	*/
	command[3] = bss->channel ? bss->channel : 1;
	command[4] = 0;			/* CHNL_EXT_SCN		*/
	command[5] = 1;			/* BAND_2G4		*/
	command[6] = 0;			/* CH_REQ_TYPE_JOIN	*/
	j36_put_le32(command + 8, 2000);
	memcpy(command + 12, bss->bssid, ETH_ALEN);

	ret = j36_wlan_command(w, J36_CMD_CH_PRIVILEGE, true, command,
			       sizeof(command));
	if (!ret && token_out)
		*token_out = w->channel_token;
	return ret;
}

/*
 * THREE FIELDS, NOT NINE.  cnmChMngrAbortPrivilege writes the network index, the
 * token and the action into a body it never zeroed, then sends the whole struct,
 * so bytes 3..19 of a real abort are heap residue -- the firmware cannot be
 * reading them, and the token is what identifies the lease being given back.
 * Filling in the channel and BSSID "to match the request" sounds careful and is
 * the same class of invention as a made-up interval.
 *
 * Releasing matters more than it looks.  A scan has to leave the operating
 * channel, and a firmware still holding a CH_REQ_TYPE_JOIN grant will not: every
 * later scan is submitted cleanly and answered by nothing at all, TC4 never gets
 * a page back, and the eventual report is a transmit starvation whose cause is
 * four commands upstream.
 */
int j36_wlan_cmd_channel_abort(struct j36_wifi *w, u8 token)
{
	u8 command[20] = {};

	command[0] = J36_NETWORK_TYPE_AIS;
	command[1] = token;
	command[2] = 1;			/* CMD_CH_ACTION_ABORT	*/
	return j36_wlan_command(w, J36_CMD_CH_PRIVILEGE, true, command,
				sizeof(command));
}

/*
 * CMD_SCAN_REQ_T, 110 bytes, which is OFFSET_OF(CMD_SCAN_REQ, aucIE) -- stock's
 * own length for a request carrying no IEs.  The two offsets a plausible guess
 * gets wrong are both here: the channel-type byte is 42, not 40, and the IE
 * length is 108, not 110.  Leaving byte 43 and the 64-byte channel list zero
 * while asking for SCAN_CHANNEL_2G4 is byte-for-byte what stock emits for a
 * 2.4 GHz wildcard sweep.
 */
int j36_wlan_cmd_scan(struct j36_wifi *w, bool active, u8 *sequence_out)
{
	u8 command[J36_SCAN_COMMAND_SIZE] = {};
	int ret;

	if (++w->scan_sequence == 0)
		w->scan_sequence = 1;

	command[0] = w->scan_sequence;
	command[1] = J36_NETWORK_TYPE_AIS;
	command[2] = active ? J36_SCAN_TYPE_ACTIVE : J36_SCAN_TYPE_PASSIVE;
	command[3] = J36_SCAN_SSID_WILDCARD;
	command[42] = J36_SCAN_CHANNEL_2G4;
	j36_put_le16(command + 108, 0);

	ret = j36_wlan_command(w, J36_CMD_SCAN_REQ, true, command,
			       sizeof(command));
	if (!ret && sequence_out)
		*sequence_out = w->scan_sequence;
	return ret;
}

/* ── the two management frames the driver builds itself ──────────────────────
 *
 * A fullmac firmware answers most management frames on its own; these two it
 * expects the host to compose, because they are the ones that carry the host's
 * choices -- which algorithm, which rates, which security IEs.
 */
int j36_wlan_cmd_auth(struct j36_wifi *w, const struct j36_wlan_bss *bss)
{
	u8 frame[30] = {};

	j36_put_le16(frame + 0, 0x00b0);	/* mgmt, authentication	*/
	memcpy(frame + 4, bss->bssid, ETH_ALEN);
	memcpy(frame + 10, w->mac, ETH_ALEN);
	memcpy(frame + 16, bss->bssid, ETH_ALEN);
	j36_put_le16(frame + 24, 0);		/* open system		*/
	j36_put_le16(frame + 26, 1);		/* transaction 1	*/
	j36_put_le16(frame + 28, 0);		/* status		*/
	return j36_wlan_frame(w, frame, sizeof(frame), J36_HIF_PACKET_TYPE_MGMT,
			      J36_STA_RECORD_INDEX, true, false, true, true, NULL);
}

/*
 * The deauthentication frame.  26 bytes: the same 24-byte header as the
 * authentication request with a two-byte reason code where its body would start.
 *
 * Sent BEFORE the BSS is torn down, which is the whole reason it is a separate
 * call rather than part of the teardown: SET_BSS_INFO(DISCONNECTED) removes the
 * firmware's context for this AP, and a management frame addressed to a BSS the
 * firmware no longer has is dropped on the way out rather than transmitted.
 */
int j36_wlan_cmd_deauth(struct j36_wifi *w, const struct j36_wlan_bss *bss,
			u16 reason)
{
	u8 frame[26] = {};

	j36_put_le16(frame + 0, 0x00c0);	/* mgmt, deauthentication */
	memcpy(frame + 4, bss->bssid, ETH_ALEN);
	memcpy(frame + 10, w->mac, ETH_ALEN);
	memcpy(frame + 16, bss->bssid, ETH_ALEN);
	j36_put_le16(frame + 24, reason);
	return j36_wlan_frame(w, frame, sizeof(frame), J36_HIF_PACKET_TYPE_MGMT,
			      J36_STA_RECORD_INDEX, true, false, true, true, NULL);
}

/*
 * The association request, and the one place this port deliberately differs from
 * MVII: the security IEs are the SUPPLICANT'S, copied verbatim, not a fixed RSN
 * element built here.
 *
 * That is not tidiness.  The AP checks the RSN IE it is sent in message 3 of the
 * handshake against the one in the association request, and wpa_supplicant
 * computes the message-2 MIC over the IE it believes it sent.  A driver that
 * substitutes its own -- however correct in isolation -- makes both of those
 * comparisons fail, and the failure surfaces as a four-way handshake that times
 * out with no reason given.  So whatever cfg80211 hands down goes on the wire
 * unaltered, and the same bytes come back to the supplicant as req_ie.
 */
int j36_wlan_cmd_assoc(struct j36_wifi *w, const struct j36_wlan_bss *bss,
		       const u8 *ies, u32 ies_len)
{
	static const u8 default_rates[] = {
		0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
		0x30, 0x48, 0x60, 0x6c,
	};
	u8 frame[J36_WLAN_MAX_ASSOC_FRAME] = {};
	const u8 *rates = bss->rate_count ? bss->rates : default_rates;
	const u32 rate_count = bss->rate_count ? bss->rate_count :
						 sizeof(default_rates);
	const u32 first = min_t(u32, rate_count, 8);
	const u8 ssid_len = min_t(u8, bss->ssid_len, IEEE80211_MAX_SSID_LEN);
	u16 capability = BIT(0);	/* ESS */
	u32 length = 28;

	if (ies_len > J36_WLAN_MAX_IES)
		return -EMSGSIZE;

	j36_put_le16(frame + 0, 0x0000);	/* mgmt, assoc request	*/
	memcpy(frame + 4, bss->bssid, ETH_ALEN);
	memcpy(frame + 10, w->mac, ETH_ALEN);
	memcpy(frame + 16, bss->bssid, ETH_ALEN);

	if (bss->capability & BIT(5))
		capability |= BIT(5);		/* short preamble	*/
	if (bss->capability & BIT(10))
		capability |= BIT(10);		/* short slot time	*/
	if (bss->privacy)
		capability |= BIT(4);
	j36_put_le16(frame + 24, capability);
	j36_put_le16(frame + 26, j36_listen_interval(bss));

	frame[length++] = WLAN_EID_SSID;
	frame[length++] = ssid_len;
	memcpy(frame + length, bss->ssid, ssid_len);
	length += ssid_len;

	/* The first eight rates go in the supported-rates element and the rest
	 * in the extended one, because that element's length field is capped at
	 * eight by 802.11 and an AP is entitled to reject a longer one. */
	frame[length++] = WLAN_EID_SUPP_RATES;
	frame[length++] = (u8)first;
	memcpy(frame + length, rates, first);
	length += first;
	if (rate_count > first) {
		frame[length++] = WLAN_EID_EXT_SUPP_RATES;
		frame[length++] = (u8)(rate_count - first);
		memcpy(frame + length, rates + first, rate_count - first);
		length += rate_count - first;
	}

	if (ies && ies_len) {
		if (length + ies_len > sizeof(frame))
			return -EMSGSIZE;
		memcpy(frame + length, ies, ies_len);
		length += ies_len;
	}

	return j36_wlan_frame(w, frame, length, J36_HIF_PACKET_TYPE_MGMT,
			      J36_STA_RECORD_INDEX, true, false, true, true, NULL);
}

/*
 * One Ethernet frame down.  EAPOL is the only kind that is treated differently,
 * and it is treated differently twice: it goes out on TC4 with an acknowledgement
 * requested, and its tag is remembered so the pairwise key install can be ordered
 * behind it.
 *
 * may_wait is false from the poll worker and true from nowhere at present, but it
 * stays a parameter rather than a constant because the distinction is the caller's
 * lock state and not this function's business.  -EBUSY here is not an error: it
 * means the class is full, and the caller is expected to hand the same frame back
 * on the next tick rather than to drop it.
 */
int j36_wlan_cmd_tx_ethernet(struct j36_wifi *w, const u8 *frame, u32 len,
			     u8 sta_index, bool is_1x, bool may_wait)
{
	u8 tag = 0;
	int ret;

	if (len < ETH_HLEN)
		return -EINVAL;

	ret = j36_wlan_frame(w, frame, len, J36_HIF_PACKET_TYPE_DATA, sta_index,
			     false, is_1x, false, may_wait, &tag);
	if (!ret && is_1x && tag)
		w->pending_1x = tag;
	return ret;
}

/* ── bringing the radio's runtime configuration up ───────────────────────────*/

/*
 * The station address, asked of the firmware rather than assumed.
 *
 * CMD_BASIC_CONFIG under QUERY comes back as event 0x09 -- not as an event
 * numbered after the command, which is why the match is on the event id AND the
 * sequence and not on the command id.  A firmware that will not answer is not
 * fatal: the board's own address is used and the fact that it was not the
 * firmware's is recorded, because "the MAC is wrong" and "the MAC came from us"
 * are the same symptom from the outside and different faults.
 */
static const u8 j36_wlan_board_mac[ETH_ALEN] = {
	0x68, 0x52, 0xd6, 0x05, 0x7c, 0x28,
};

static bool j36_valid_station_mac(const u8 *mac)
{
	u8 any = 0;
	u8 all = 0xff;
	int i;

	for (i = 0; i < ETH_ALEN; i++) {
		any |= mac[i];
		all &= mac[i];
	}
	return any && all != 0xff && !(mac[0] & 0x01);
}

static void j36_wlan_query_mac(struct j36_wifi *w)
{
	const u32 packet_size = J36_HIF_CMD_HEADER_SIZE + 12;
	const u32 transfer = ALIGN(packet_size, 4);
	u8 *packet = w->hif_tx;
	ktime_t deadline;
	u8 sequence;

	memcpy(w->mac, j36_wlan_board_mac, ETH_ALEN);
	w->mac_from_firmware = false;

	memset(packet, 0, transfer);
	j36_put_le16(packet, (u16)transfer);
	packet[3] = (J36_HIF_TC_COMMAND << J36_HIF_TX_RESOURCE_SHIFT) |
		    (J36_HIF_PACKET_TYPE_CMD << J36_HIF_TX_PACKET_TYPE_SHIFT);
	packet[4] = J36_CMD_BASIC_CONFIG;
	packet[5] = 0;			/* query, not set	*/
	sequence = j36_wifi_hif_sequence(w);
	packet[6] = sequence;

	if (j36_wifi_hif_tx_acquire(w, J36_HIF_TC_COMMAND))
		return;
	j36_wifi_hif_submit(w, J36_HIF_TC_COMMAND, packet, transfer);
	w->hif_stats.tx_commands++;

	deadline = ktime_add_ms(ktime_get(), 1000);
	while (ktime_before(ktime_get(), deadline)) {
		u8 port;
		u32 length;

		if (!j36_wifi_hif_pending(w, &port, &length)) {
			usleep_range(100, 400);
			continue;
		}
		if (j36_wifi_hif_collect(w, port, length) || length < 20) {
			w->hif_stats.dropped_packets++;
			continue;
		}
		if ((j36_get_le16(w->hif_rx + 2) & J36_HIF_RX_PACKET_TYPE_MASK) !=
			    J36_HIF_PACKET_TYPE_EVENT ||
		    w->hif_rx[4] != J36_EVENT_BASIC_CONFIG ||
		    w->hif_rx[5] != sequence) {
			w->hif_stats.dropped_packets++;
			continue;
		}
		if (j36_valid_station_mac(w->hif_rx + 8)) {
			memcpy(w->mac, w->hif_rx + 8, ETH_ALEN);
			w->mac_from_firmware = true;
		}
		return;
	}
}

/*
 * CMD_SET_DOMAIN_INFO -- the channel list the firmware is allowed to use, sent as
 * two commands under one id: the channels that are legal at all, then the subset
 * of those that must be listened to passively.
 *
 * The 56-byte body is a country code, a table selector, six 8-byte subband slots
 * and two bandwidth overrides.  A subband whose band is neither 1 nor 2 has only
 * its first two bytes copied and the rest left zero, which is also how a short
 * list terminates -- a zero channel count ends it.
 */
enum {
	J36_DOMAIN_PAYLOAD_SIZE	= 56,
	J36_DOMAIN_TABLE_ALLOWED = 0,
	J36_DOMAIN_TABLE_PASSIVE = 1,
	J36_DOMAIN_COUNTRY_EU	= 0x4555,	/* 'E' << 8 | 'U'	*/
	J36_DOMAIN_SUBBANDS	= 6,
};

/*
 * The domain stock falls back to when the NVRAM country code matches none of its
 * groups: channels 1..13 on 2.4 GHz plus the four 5 GHz subbands.  Sent whole
 * rather than trimmed to the band we scan, because the point of this command is
 * to stop differing from stock.
 */
static const u8 j36_domain_allowed[J36_DOMAIN_SUBBANDS][5] = {
	{  81, 1, 1,   1, 13 },		/* 2.4 GHz, channels 1..13	*/
	{ 115, 2, 4,  36,  4 },		/* 5 GHz, 36..48		*/
	{ 118, 2, 4,  52,  4 },		/* 5 GHz, 52..64		*/
	{ 121, 2, 4, 100, 12 },		/* 5 GHz, 100..144		*/
	{ 125, 2, 4, 149,  7 },		/* 5 GHz, 149..173		*/
	{   0, 0, 0,   0,  0 },
};

/* Empty, and not by omission: stock's passive table has two entries and the
 * default one is all zeros, so the second command really does say "no channel is
 * passive-only". */
static const u8 j36_domain_passive[J36_DOMAIN_SUBBANDS][5] = {
	{ 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0 },
};

static int j36_wlan_domain_table(struct j36_wifi *w, u16 table,
				 const u8 subbands[J36_DOMAIN_SUBBANDS][5])
{
	u8 payload[J36_DOMAIN_PAYLOAD_SIZE] = {};
	unsigned int i;

	j36_put_le16(payload + 0, J36_DOMAIN_COUNTRY_EU);
	j36_put_le16(payload + 2, table);
	for (i = 0; i < J36_DOMAIN_SUBBANDS; i++) {
		u8 *slot = payload + 4 + (i * 8);

		slot[0] = subbands[i][0];
		slot[1] = subbands[i][1];
		if (subbands[i][1] != 1 && subbands[i][1] != 2)
			continue;
		slot[2] = subbands[i][2];
		slot[3] = subbands[i][3];
		slot[4] = subbands[i][4];
	}
	/* [52] and [53] are the fixed-bandwidth overrides.  Stock forwards two
	 * adapter bytes that are zero unless someone has pinned a channel width,
	 * and nothing here pins one, so zero is the faithful value rather than a
	 * placeholder for one we could not work out. */
	return j36_wlan_command(w, J36_CMD_SET_DOMAIN_INFO, true, payload,
				sizeof(payload));
}

/*
 * CMD_SET_RX_FILTER -- which classes of received frame the firmware is allowed
 * to hand to the host at all, as one 32-bit PARAM_PACKET_FILTER_* mask.
 *
 * Stock sends this from ndo_set_rx_mode, and Linux calls ndo_set_rx_mode out of
 * dev_open() before anything can be received, so stock is never running without
 * having sent it and the firmware's power-on default is whatever it happens to
 * be.  This driver had no set_rx_mode at all and so never sent the command, and
 * the symptom was exact: the only data frames that ever arrived were the two
 * unencrypted EAPOL frames of the four-way handshake, which the firmware punts
 * to the host whatever the filter says because otherwise no supplicant could
 * ever run.  Everything else -- the DHCP offer, ARP, router advertisements --
 * was dropped inside the chip, so the link associated, the handshake completed,
 * the keys went in, and then the address never came.
 *
 * ALL_MULTICAST rather than MULTICAST alone, because the group-address list is
 * a separate command (CMD_ID_MAC_MCAST_ADDR) that this driver does not send:
 * asking for MULTICAST with no list installed is asking for the empty set,
 * which is how IPv6 loses both its router advertisements and its own duplicate
 * address detection.  Letting every group address up and filtering in the host
 * costs a handful of frames a second on a home network, and the radio is in
 * constantly-awake mode anyway.
 *
 * DIRECTED is set even though stock's OS layer never sets it -- stock leans on
 * the firmware always passing frames addressed to the station -- because the
 * adapter's own shadow copy of this mask starts life as
 * PARAM_PACKET_FILTER_SUPPORTED, which does include it, and unicast is the one
 * class where guessing wrong costs the whole link rather than a corner of it.
 */
enum {
	J36_RX_FILTER_DIRECTED		= 0x00000001,
	J36_RX_FILTER_MULTICAST		= 0x00000002,
	J36_RX_FILTER_ALL_MULTICAST	= 0x00000004,
	J36_RX_FILTER_BROADCAST		= 0x00000008,
};

int j36_wlan_cmd_rx_filter(struct j36_wifi *w)
{
	u8 payload[4] = {};

	j36_put_le32(payload, J36_RX_FILTER_DIRECTED |
			      J36_RX_FILTER_MULTICAST |
			      J36_RX_FILTER_ALL_MULTICAST |
			      J36_RX_FILTER_BROADCAST);
	return j36_wlan_command(w, J36_CMD_SET_RX_FILTER, true, payload,
				sizeof(payload));
}

/*
 * The narrowest useful thing that can be done to a firmware that has stopped
 * answering.
 *
 * There is no reset command on this part.  The only true reset is the whole
 * four-stage bring-up, which means downloading the firmware again, and that is
 * not something to reach for while an interface is up.  What CAN be rebuilt from
 * here is the per-network state -- and the per-network state is what actually
 * goes missing: a teardown whose BSS_ACTIVATE_CTRL "on" was refused leaves the
 * AIS network deactivated, and a deactivated network accepts every command that
 * follows without complaint and answers none of them.  From outside, that is
 * indistinguishable from a dead radio.
 *
 * So: the power-management context released, the power-save profile and the
 * receive filter restated, and the network itself cycled -- the same commands
 * j36_wlan_cmd_configure() establishes them with, in the same order.  The station
 * address and the domain table are deliberately NOT re-sent: they are
 * adapter-wide rather than per-network, they were accepted at start-up, and the
 * channel list alone is several more pages on a class that has four.
 */
int j36_wlan_cmd_rearm(struct j36_wifi *w)
{
	u8 ps_profile[4] = {};
	int ret;

	if (!w->firmware_alive)
		return -ENODEV;

	ret = j36_wifi_hif_own(w);
	if (ret)
		return ret;

	j36_wlan_cmd_pm_abort(w);

	ps_profile[0] = J36_NETWORK_TYPE_AIS;
	ps_profile[1] = 0;		/* Param_PowerModeCAM	*/
	j36_wlan_command(w, J36_CMD_POWER_SAVE_MODE, true, ps_profile,
			 sizeof(ps_profile));

	ret = j36_wlan_cmd_rx_filter(w);
	if (ret)
		return ret;
	return j36_wlan_cmd_bss_reactivate(w);
}

/*
 * Everything wlanAdapterStart does between the firmware download and the first
 * scan that actually talks to the chip, in stock's own order.
 *
 * The power-save profile is a deliberate divergence and so it is spelled out:
 * stock sends 2 (fast switch) and this sends 0 (constantly awake), which is what
 * stock itself sends from the arm taken when power management is compiled out.
 * Off is what we are -- fast switch hands the firmware the right to sleep between
 * DTIMs, and there is no power-save state machine on this side to keep in step
 * with it.  A radio that sleeps on a schedule nobody is tracking loses packets
 * silently, which is the failure this whole layer exists to avoid.
 *
 * The channel list is sent HERE because this is where stock sends it, during
 * adapter start and long before anything can scan or associate.  Carrying it as
 * an on-demand command was a bug in MVII and the hardware named it.
 */
int j36_wlan_cmd_configure(struct j36_wifi *w)
{
	u8 basic_config[12] = {};
	u8 activate[4] = {};
	u8 ps_profile[4] = {};
	int ret;

	if (!w->firmware_alive)
		return -ENODEV;

	ret = j36_wifi_hif_own(w);
	if (ret)
		return ret;

	j36_wlan_query_mac(w);

	/* Non-fatal, exactly as in stock, which does not check this one either:
	 * a firmware that refuses the profile still scans. */
	ps_profile[0] = J36_NETWORK_TYPE_AIS;
	ps_profile[1] = 0;		/* Param_PowerModeCAM	*/
	j36_wlan_command(w, J36_CMD_POWER_SAVE_MODE, true, ps_profile,
			 sizeof(ps_profile));

	/*
	 * CMD_BASIC_CONFIG, 12 bytes: the MAC, then ucNative80211 and two
	 * checksum-offload halfwords.  All three stay zero -- nothing here asks
	 * the firmware to compute a checksum, so the host stack keeps doing what
	 * it already does.
	 */
	memcpy(basic_config, w->mac, ETH_ALEN);
	ret = j36_wlan_command(w, J36_CMD_BASIC_CONFIG, true, basic_config,
			       sizeof(basic_config));
	if (ret) {
		j36_wifi_fail(w, "wlan-basic-config-refused",
			      "the WLAN firmware would not take the station address");
		return ret;
	}

	/* After the station address and not before it: the DIRECTED class in the
	 * mask means "addressed to this station", and this is the command that
	 * tells the firmware which station that is. */
	ret = j36_wlan_cmd_rx_filter(w);
	if (ret) {
		j36_wifi_fail(w, "wlan-rx-filter-refused",
			      "the WLAN firmware would not take the receive filter; nothing but the 802.1X handshake would ever be received");
		return ret;
	}

	activate[0] = J36_NETWORK_TYPE_AIS;
	activate[1] = 1;
	ret = j36_wlan_command(w, J36_CMD_BSS_ACTIVATE_CTRL, true, activate,
			       sizeof(activate));
	if (ret) {
		j36_wifi_fail(w, "wlan-ais-activate-refused",
			      "the WLAN firmware would not activate the station network");
		return ret;
	}

	ret = j36_wlan_domain_table(w, J36_DOMAIN_TABLE_ALLOWED,
				    j36_domain_allowed);
	if (ret) {
		/* Not fatal: the scan path provably works without this command,
		 * so a refused channel list should cost association rather than
		 * the radio.  Say so rather than reporting a transport that is
		 * only three quarters up. */
		dev_warn(w->dev,
			 "the WLAN firmware refused the allowed-channel list; association may fail\n");
		return 0;
	}
	usleep_range(2000, 4000);
	if (j36_wlan_domain_table(w, J36_DOMAIN_TABLE_PASSIVE,
				  j36_domain_passive))
		dev_warn(w->dev,
			 "the WLAN firmware refused the passive-channel list\n");
	return 0;
}

/* ── and everything the firmware says back ───────────────────────────────────*/

static void j36_wlan_rx_event(struct j36_wifi *w, const u8 *packet, u32 length)
{
	const u8 *payload = packet + J36_HIF_CMD_HEADER_SIZE;
	u32 payload_len;
	u8 event_id;

	if (length < J36_HIF_CMD_HEADER_SIZE) {
		w->hif_stats.dropped_packets++;
		return;
	}
	event_id = packet[4];
	payload_len = length - J36_HIF_CMD_HEADER_SIZE;
	w->hif_stats.rx_events++;

	switch (event_id) {
	case J36_EVENT_TX_DONE:
		/* EVENT_TX_DONE_T: ucPacketSeq at 0, ucStatus at 1.  The tag
		 * names one frame exactly -- it is the ucPacketSeqNo this driver
		 * wrote into that frame's HIF descriptor. */
		if (payload_len >= 1 && w->pending_1x &&
		    payload[0] == w->pending_1x)
			w->pending_1x = 0;
		break;

	case J36_EVENT_SCAN_DONE:
		if (payload_len >= 1)
			j36_wlan_on_scan_done(w, payload[0]);
		break;

	case J36_EVENT_SCAN_RESULT:
		/*
		 * The digested form of a beacon: RSSI, frequency, BSSID and
		 * capability at fixed offsets with the IEs from 56 onwards.
		 * Only 2.4 GHz frequencies are converted, because that is the
		 * only band the scan asks for and a channel number computed
		 * from a 5 GHz frequency by this formula would be nonsense
		 * rather than an error.
		 */
		if (payload_len >= 58) {
			const u32 khz = j36_get_le32(payload + 8);
			u8 channel = 0;

			if (khz == 2484000)
				channel = 14;
			else if (khz >= 2412000 && khz <= 2472000)
				channel = (u8)((khz - 2407000) / 5000);

			j36_wlan_on_scan_result(w, payload + 22,
						j36_get_le16(payload + 54),
						channel,
						(s32)j36_get_le32(payload),
						payload + 56, payload_len - 56);
		}
		break;

	case J36_EVENT_CH_PRIVILEGE:
		/* [0] network, [1] the token we asked under, [2] the action --
		 * 0 being the grant of a request and not a release of one. */
		if (payload_len >= 12 && payload[0] == J36_NETWORK_TYPE_AIS &&
		    payload[2] == 0)
			j36_wlan_on_channel_grant(w, payload[1]);
		break;

	case J36_EVENT_ACTIVATE_STA_REC:
		/*
		 * cnmStaRecHandleEventPkt does not take the firmware's word for
		 * which record this is: it checks the index, the state and the
		 * MAC before activating.  The index and the address are checked
		 * here; the state is stage 4b's, which is why the peer goes up
		 * rather than a bare "the record is valid now".
		 */
		if (payload_len >= 8 && payload[6] == J36_STA_RECORD_INDEX)
			j36_wlan_on_sta_active(w, payload);
		break;

	case J36_EVENT_BSS_BEACON_TIMEOUT:
		j36_wlan_on_beacon_timeout(w);
		break;

	case J36_EVENT_CMD_RESULT:
		/* Set commands are fire-and-forget; consume the result. */
		break;

	default:
		break;
	}
}

static void j36_wlan_rx_management(struct j36_wifi *w, const u8 *packet,
				   u32 length)
{
	const u8 *peer = j36_wlan_peer_bssid(w);
	const u8 *frame;
	u32 usable;
	u32 offset;
	u32 frame_len;
	u16 subtype;

	if (length < J36_HIF_RX_HEADER_SIZE)
		return;
	w->hif_stats.rx_management++;

	/* The packet's own length is authoritative up to what was actually read,
	 * and the two low bits of byte 4 are how far past the 12-byte receive
	 * header the frame really starts. */
	usable = min_t(u32, j36_get_le16(packet), length);
	offset = packet[4] & 0x3;
	if (J36_HIF_RX_HEADER_SIZE + offset + 24 > usable) {
		w->hif_stats.dropped_packets++;
		return;
	}
	frame = packet + J36_HIF_RX_HEADER_SIZE + offset;
	frame_len = usable - J36_HIF_RX_HEADER_SIZE - offset;
	subtype = j36_get_le16(frame) & 0x00f0;

	/* Addressed to us: the three frames a join can be answered with. */
	if (j36_same_mac(frame + 4, w->mac)) {
		if (subtype == 0x00b0)
			j36_wlan_on_auth_response(w, frame, frame_len);
		else if (subtype == 0x0010)
			j36_wlan_on_assoc_response(w, frame, frame_len);
		else if ((subtype == 0x00a0 || subtype == 0x00c0) && peer &&
			 j36_same_mac(frame + 10, peer))
			j36_wlan_on_ap_disconnect(w, frame, frame_len,
						  subtype == 0x00c0);
	}

	/*
	 * Beacons and probe responses go up whenever they arrive, not only while
	 * a sweep is running.  While associated and idle the radio sits on the
	 * AP's channel and hears its beacon every hundred milliseconds or so, and
	 * gating on a scan throws away precisely those -- so the one network
	 * guaranteed to be in range becomes the one most likely to be missing
	 * from the next scan's results.  cfg80211's BSS table ages its own
	 * entries; it does not need us to decide when it may remember one.
	 */
	if ((subtype == 0x0080 || subtype == 0x0050) && frame_len >= 36)
		j36_wlan_on_beacon(w, frame, frame_len, packet[10], packet[9]);
}

/*
 * A data packet, which arrives in one of two shapes depending on whether the
 * firmware has already stripped the 802.11 header for us.  Byte 5 bit 0 says it
 * has not, and then this converts in place: an 802.11 header plus LLC/SNAP is at
 * least 32 bytes and an Ethernet header is 14, so the shorter one is written over
 * the tail of the longer and the payload never moves.  The EtherType is already
 * where it needs to be -- SNAP's own type field lands exactly on the Ethernet
 * header's last two bytes -- so only the addresses are copied, and they are taken
 * to locals first because they live inside the bytes about to be overwritten.
 */
static void j36_wlan_rx_data(struct j36_wifi *w, u8 *packet, u32 length)
{
	u8 dest[ETH_ALEN];
	u8 src[ETH_ALEN];
	u8 *payload;
	u8 *ethernet;
	u8 *llc;
	u32 packet_len;
	u32 payload_len;
	u32 header_len;
	u32 body_len;
	u32 offset;
	u16 fc;
	bool to_ds;
	bool from_ds;

	if (length < J36_HIF_RX_HEADER_SIZE)
		return;
	packet_len = min_t(u32, j36_get_le16(packet), length);
	offset = packet[4] & 0x3;
	if (J36_HIF_RX_HEADER_SIZE + offset >= packet_len)
		return;
	payload = packet + J36_HIF_RX_HEADER_SIZE + offset;
	payload_len = packet_len - J36_HIF_RX_HEADER_SIZE - offset;
	w->hif_stats.rx_data++;

	if (!(packet[5] & BIT(0))) {
		j36_wlan_on_ethernet(w, payload, payload_len);
		return;
	}

	header_len = (packet[4] >> 2) & 0x3f;
	if (header_len < 24)
		header_len = 24;
	if (payload_len < header_len + 8)
		return;

	fc = j36_get_le16(payload);
	to_ds = fc & 0x0100;
	from_ds = fc & 0x0200;
	if (to_ds && from_ds)		/* four-address WDS: not ours	*/
		return;
	llc = payload + header_len;
	if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03)
		return;
	body_len = payload_len - header_len - 8;

	memcpy(dest, from_ds ? payload + 4 : (to_ds ? payload + 16 : payload + 4),
	       ETH_ALEN);
	memcpy(src, from_ds ? payload + 16 : payload + 10, ETH_ALEN);
	ethernet = llc + 8 - ETH_HLEN;
	memcpy(ethernet, dest, ETH_ALEN);
	memcpy(ethernet + ETH_ALEN, src, ETH_ALEN);
	j36_wlan_on_ethernet(w, ethernet, body_len + ETH_HLEN);
}

/*
 * Drain the receive port and sort what comes out.
 *
 * Returns how many packets were handled, which is what the poll worker in
 * j36_mt6592_wifi_net.c uses to decide whether to come straight back or to back
 * off -- a productive poll is evidence there is more waiting.
 *
 * An abnormal FIFO condition clears firmware_alive rather than trying to
 * recover.  There is no reset short of the whole four-stage bring-up, and a
 * transport that has faulted will otherwise answer every future command with
 * silence, which reads as a radio that is present and ignoring us.
 */
unsigned int j36_wlan_cmd_pump(struct j36_wifi *w)
{
	unsigned int processed = 0;
	u32 whisr;

	if (!w->firmware_alive)
		return 0;
	if (j36_wifi_hif_own(w))
		return 0;

	whisr = j36_wifi_hif_status(w, J36_HIF_WHISR);
	w->hif_stats.last_whisr = whisr;
	if (whisr & J36_WHISR_ABNORMAL_INT) {
		w->hif_stats.last_wasr = j36_wifi_hif_status(w, J36_HIF_WASR);
		w->firmware_alive = false;
		j36_wifi_fail(w, "hif-abnormal-interrupt",
			      "the WLAN HIF reported an abnormal FIFO condition (WASR 0x%08x)",
			      w->hif_stats.last_wasr);
		return 0;
	}
	if (whisr & J36_WHISR_TX_DONE_INT)
		j36_wifi_hif_tx_credit(w);

	while (processed < J36_WLAN_MAX_RX_PER_POLL) {
		u8 port;
		u32 length;

		if (!j36_wifi_hif_pending(w, &port, &length))
			break;
		processed++;
		if (j36_wifi_hif_collect(w, port, length) || length < 4) {
			w->hif_stats.dropped_packets++;
			continue;
		}

		switch (j36_get_le16(w->hif_rx + 2) &
			J36_HIF_RX_PACKET_TYPE_MASK) {
		case J36_HIF_PACKET_TYPE_EVENT:
			j36_wlan_rx_event(w, w->hif_rx, length);
			break;
		case J36_HIF_PACKET_TYPE_MGMT:
			j36_wlan_rx_management(w, w->hif_rx, length);
			break;
		case J36_HIF_PACKET_TYPE_DATA:
			j36_wlan_rx_data(w, w->hif_rx, length);
			break;
		default:
			w->hif_stats.dropped_packets++;
			break;
		}
	}
	return processed;
}
