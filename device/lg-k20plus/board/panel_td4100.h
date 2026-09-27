/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Tovis TD4100 720x1280 DSI video-mode panel (second source): geometry.
 * No vendor init table -- stock sends only sleep-out (+120 ms) and
 * display-on. Carried in linux/lg_msm8917_panel.c (variant td4100).
 *
 * From: reference/.../arch/arm64/boot/dts/lge/
 *       dsi-panel-lv5-tovis-td4100-hd-video.dtsi
 */
#ifndef LG_PANEL_TD4100_H
#define LG_PANEL_TD4100_H

#define TD4100_WIDTH	720
#define TD4100_HEIGHT	1280
#define TD4100_HFP	78
#define TD4100_HBP	32
#define TD4100_HSA	4
#define TD4100_VFP	127
#define TD4100_VBP	127
#define TD4100_VSA	127
#define TD4100_BPP	24

#endif /* LG_PANEL_TD4100_H */
