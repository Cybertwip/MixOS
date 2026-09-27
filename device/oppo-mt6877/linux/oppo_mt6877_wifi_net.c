// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPPO MT6877 Wi-Fi net stage: a fullmac cfg80211 wiphy + wlan0.
 *
 * Fullmac, not mac80211: the firmware owns the 802.11 state machine (scan,
 * auth, assoc, 4-way handshake) and Linux owns Ethernet frames and cfg80211
 * plumbing -- the same split as the J36 driver. Scan/connect/key ops forward
 * to the firmware over the SDIO command queue; the queue itself is stage 3.
 *
 * BRING-UP STATUS. The wiphy registers and the interface appears. Scan and
 * connect forward to firmware commands whose opcodes are carried over from
 * the MT6592 driver and are marked CHECK below: they are the first thing to
 * verify against the CONSYS_6877 firmware on hardware, with
 * device/oppo-mt6877/tests/wifi-bringup.sh. Until that run passes, wlan0
 * exists but does not associate -- and says so in the log.
 */

#include <linux/bits.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <net/cfg80211.h>

#include "oppo_mt6877_wifi.h"

/* CHECK: firmware command opcodes, carried from the MT6592 driver. */
#define OPPO_FW_CMD_SCAN	0x01
#define OPPO_FW_CMD_CONNECT	0x02
#define OPPO_FW_CMD_ADD_KEY	0x03

struct oppo_wiphy_priv {
	struct oppo_wifi *w;
	struct wireless_dev *wdev;
};

static int oppo_cfg_scan(struct wiphy *wiphy, struct cfg80211_scan_request *req)
{
	struct oppo_wiphy_priv *priv = wiphy_priv(wiphy);

	dev_info(priv->w->dev, "scan requested (%u ssids); firmware cmd %#x\n",
		 req->n_ssids, OPPO_FW_CMD_SCAN);
	/* The HIF queue hands the request to the firmware and completes it
	 * from the firmware's scan-done event. */
	return -EOPNOTSUPP; /* until the on-hardware opcode check passes */
}

static int oppo_cfg_connect(struct wiphy *wiphy, struct net_device *dev,
			    struct cfg80211_connect_params *sme)
{
	struct oppo_wiphy_priv *priv = wiphy_priv(wiphy);

	dev_info(priv->w->dev, "connect requested; firmware cmd %#x\n",
		 OPPO_FW_CMD_CONNECT);
	return -EOPNOTSUPP; /* until the on-hardware opcode check passes */
}

static const struct cfg80211_ops oppo_cfg_ops = {
	.scan = oppo_cfg_scan,
	.connect = oppo_cfg_connect,
};

int oppo_wifi_net_attach(struct oppo_wifi *w)
{
	struct wiphy *wiphy;
	struct oppo_wiphy_priv *priv;
	struct net_device *ndev;
	struct wireless_dev *wdev;
	int ret;

	wiphy = wiphy_new(&oppo_cfg_ops, sizeof(*priv));
	if (!wiphy)
		return -ENOMEM;
	priv = wiphy_priv(wiphy);
	priv->w = w;
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	/* Bands are filled from the firmware capability reply on bring-up;
	 * an empty wiphy still registers so the stack can be inspected. */
	ret = wiphy_register(wiphy);
	if (ret)
		goto out_free_wiphy;

	ndev = alloc_netdev(0, "wlan%d", NET_NAME_UNKNOWN, ether_setup);
	if (!ndev) {
		ret = -ENOMEM;
		goto out_unreg_wiphy;
	}
	wdev = kzalloc(sizeof(*wdev), GFP_KERNEL);
	if (!wdev) {
		ret = -ENOMEM;
		goto out_free_netdev;
	}
	wdev->wiphy = wiphy;
	wdev->netdev = ndev;
	wdev->iftype = NL80211_IFTYPE_STATION;
	ndev->ieee80211_ptr = wdev;
	priv->wdev = wdev;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	ret = register_netdev(ndev);
	if (ret)
		goto out_free_wdev;
	w->wiphy = wiphy; /* drvdata stays the platform_data; see wifi_main.c */
	dev_info(w->dev, "wlan0 registered (scan/connect pending opcode check)\n");
	return 0;

out_free_wdev:
	kfree(wdev);
out_free_netdev:
	free_netdev(ndev);
out_unreg_wiphy:
	wiphy_unregister(wiphy);
out_free_wiphy:
	wiphy_free(wiphy);
	return ret;
}

void oppo_wifi_net_detach(struct oppo_wifi *w)
{
	struct wiphy *wiphy = w->wiphy;
	struct oppo_wiphy_priv *priv;
	struct net_device *ndev;

	if (!wiphy)
		return;
	priv = wiphy_priv(wiphy);
	if (priv->wdev) {
		ndev = priv->wdev->netdev;
		unregister_netdev(ndev);
		kfree(priv->wdev);
		free_netdev(ndev);
	}
	wiphy_unregister(wiphy);
	wiphy_free(wiphy);
	w->wiphy = NULL;
}

MODULE_LICENSE("GPL");
