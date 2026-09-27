/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * LGD LG4894 720x1280 DSI video-mode panel: geometry + init summary.
 * The full init table lives in linux/lg_msm8917_panel.c (variant lg4894).
 *
 * From: reference/.../arch/arm64/boot/dts/lge/
 *       dsi-panel-lv5-lgd-lg4894-hd-video.dtsi
 */
#ifndef LG_PANEL_LG4894_H
#define LG_PANEL_LG4894_H

#define LG4894_WIDTH	720
#define LG4894_HEIGHT	1280
#define LG4894_HFP	24
#define LG4894_HBP	72
#define LG4894_HSA	4
#define LG4894_VFP	500
#define LG4894_VBP	15
#define LG4894_VSA	1
#define LG4894_BPP	24

/* Backlight: WLED through the PMI8950 (mainline qpnp-wled), 1..4095. */
#define LG4894_BL_MIN	1
#define LG4894_BL_MAX	4095

#endif /* LG_PANEL_LG4894_H */
