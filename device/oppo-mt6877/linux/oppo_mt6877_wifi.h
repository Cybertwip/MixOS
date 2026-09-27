/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPPO MT6877 Wi-Fi: shared state for the CONSYS_6877 fullmac driver.
 *
 * ARCHITECTURE. This is the same four-stage WMT architecture as the J36 wifi
 * driver (see j36_mt6592_wifi.h), moved one generation forward:
 *
 *   1. consys  -- oppo_mt6877_consys.ko owns the power/reset (separate module).
 *   2. WMT     -- oppo_mt6877_wifi_wmt.c downloads WIFI_RAM_CODE, runs the WMT
 *                 handshake and asserts the function-ready bit.
 *   3. HIF     -- SDIO command/response + TX/RX queues (in wifi_main.c).
 *   4. net     -- oppo_mt6877_wifi_net.c: cfg80211 wiphy + wlan0, fullmac.
 *
 * The WMT wire protocol is stable across MTK combo generations, which is why
 * the port is mostly new base addresses (board/wifi_consys.h) and new
 * firmware names rather than a new driver. What is NOT carried over blindly
 * is the MT6592 chip-id / strap sequence: CONSYS_6877 enumerates its SDIO
 * function only after the RGU power bit is set, so probe defers until the
 * consys module has run (load.order enforces the order; -EPROBE_DEFER is the
 * backstop).
 */
#ifndef OPPO_MT6877_WIFI_H
#define OPPO_MT6877_WIFI_H

#include <linux/device.h>
#include <linux/types.h>

struct oppo_wifi;
struct wiphy;

/* oppo_mt6877_consys.c -- the shared block owner. */
int oppo_consys_power_up(struct device *dev);
void oppo_consys_power_down(struct device *dev);

/* oppo_mt6877_wifi_wmt.c -- firmware + handshake. */
int oppo_wifi_wmt_boot(struct oppo_wifi *w);
void oppo_wifi_wmt_shutdown(struct oppo_wifi *w);

/* oppo_mt6877_wifi_main.c -- SDIO HIF + driver glue. */
struct oppo_wifi {
	struct device *dev;
	void __iomem *consys_cfg;
	const struct firmware *fw;
	struct wiphy *wiphy;
	bool wmt_ready;
	/* Statistics the net stage reports through cfg80211. */
	u64 tx_packets;
	u64 rx_packets;
};

/* oppo_mt6877_wifi_net.c -- cfg80211 wiphy + wlan0. */
int oppo_wifi_net_attach(struct oppo_wifi *w);
void oppo_wifi_net_detach(struct oppo_wifi *w);

#endif /* OPPO_MT6877_WIFI_H */
