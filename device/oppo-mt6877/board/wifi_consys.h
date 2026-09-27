/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * CONSYS_6877 (Wi-Fi/BT/GPS combo) outline for linux/oppo_mt6877_wifi*.c.
 *
 * MT6877 integrates the connectivity subsystem on-chip (CONFIG_MTK_COMBO_CHIP_
 * CONSYS_6877 in oplus6877_defconfig). The bring-up order is the same WMT
 * architecture the J36 wifi driver already implements for MT6592: power the
 * conn_infra block, take the subsystem out of reset, download the firmware,
 * run the WMT handshake, then talk to Wi-Fi over SDIO. Only the register
 * block and the firmware names are generation-specific; both are here.
 *
 * From: consys@18000000 node of mt6877.dts,
 *       reference/.../drivers/misc/mediatek/connectivity/ (WMT protocol),
 *       arch/arm64/configs/oplus6877_defconfig (CONSYS_6877 selection).
 */
#ifndef OPPO_WIFI_CONSYS_H
#define OPPO_WIFI_CONSYS_H

/* Register windows inside the conn_infra block (offsets from 0x18000000). */
#define CONSYS_INFRA_RGU	0x0000
#define CONSYS_INFRA_RGU_SIZE	0x470
#define CONSYS_INFRA_CFG	0x1000
#define CONSYS_INFRA_CFG_SIZE	0x658

/* Firmware the driver requests from /lib/firmware (taken from stock). */
#define CONSYS_WIFI_FW		"oppo/mt6877/WIFI_RAM_CODE"
#define CONSYS_WMT_CFG		"oppo/mt6877/WMT_SOC.cfg"

/* WMT operation codes the driver uses (stable across MTK generations). */
#define CONSYS_WMT_OP_SET	0x1
#define CONSYS_WMT_OP_QUERY	0x2

#endif /* OPPO_WIFI_CONSYS_H */
