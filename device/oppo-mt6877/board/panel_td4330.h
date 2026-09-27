/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * TD4330 FHD+ DSI command-mode panel (AUO glass, Richtek backlight/bias).
 * Geometry + timings for generate_dts_oppo.py. The full init table lives in
 * linux/oppo_mt6877_panel_td4330.c, transcribed from the same source.
 *
 * From: reference/.../drivers/misc/mediatek/lcm/td4330_fhdp_dsi_cmd_auo_rt5081/
 *       td4330_fhdp_dsi_cmd_auo_rt5081.c (init_setting_cmd[], get_params())
 *       reference/.../arch/arm64/boot/dts/mediatek/oplus6877_20181.dts
 *       (lcmname "td4330_fhdp_dsi_cmd_auo_rt4831_drv")
 */
#ifndef OPPO_PANEL_TD4330_H
#define OPPO_PANEL_TD4330_H

/* 1080x2400 command mode, RGB888, 4 lanes. */
#define TD4330_WIDTH		1080
#define TD4330_HEIGHT		2280
#define TD4330_VSA		2
#define TD4330_VBP		16
#define TD4330_VFP		10
#define TD4330_HSA		20
#define TD4330_HBP		40
#define TD4330_HFP		40
#define TD4330_PLL_CLOCK_MHZ	525

/* Column/page window the vendor init programs (2A/2B): full frame. */
#define TD4330_COL_START	0
#define TD4330_COL_END		1079
#define TD4330_PAGE_START	0
#define TD4330_PAGE_END		2279

/* Sleep in/out + display on/off + backlight level (standard MIPI DCS). */
#define TD4330_CMD_SLEEP_OUT	0x11
#define TD4330_CMD_SLEEP_IN	0x10
#define TD4330_CMD_DISPLAY_ON	0x29
#define TD4330_CMD_DISPLAY_OFF	0x28
#define TD4330_CMD_WRDISBV	0x51
#define TD4330_CMD_WRCTRLD	0x53
#define TD4330_CMD_WRCABC	0x55

#endif /* OPPO_PANEL_TD4330_H */
