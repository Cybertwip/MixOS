/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * SiW LG4894 touch protocol (lv517 default) + Synaptics TD4100 note.
 *
 * The LG4894 speaks I2C at 0x28 with a 2-byte register header and reports
 * through the lg4894_touch_info struct at 0x200: status words, then ten
 * 12-byte finger records {tool:4, event:4, track_id, x, y, pressure,
 * angle, width_major, width_minor}. Events: IDLE 0, DOWN 1, MOVE 2, UP 3.
 * Implemented in linux/lg_msm8917_touch.c.
 *
 * The TD4100 at 0x20 is an RMI4 device and is served by mainline rmi4-i2c
 * ("synaptics,rmi4-i2c") -- no custom code. Both nodes stay enabled like
 * the vendor DTS; only the populated IC probes.
 *
 * From: drivers/input/touchscreen/lge/lgsic/touch_lg4894.{c,h},
 *       msm8917-lv517_gsm_us-touch.dtsi (720x1280, 10 fingers, 400 kHz).
 */
#ifndef LG_TOUCH_LG4894_H
#define LG_TOUCH_LG4894_H

#define LG4894_I2C_ADDR		0x28
#define LG4894_TD4100_ADDR	0x20
#define LG4894_REG_REPORT	0x200
#define LG4894_MAX_FINGERS	10
#define LG4894_COORD_X		720
#define LG4894_COORD_Y		1280

#define LG4894_EVT_IDLE		0
#define LG4894_EVT_DOWN		1
#define LG4894_EVT_MOVE		2
#define LG4894_EVT_UP		3

#endif /* LG_TOUCH_LG4894_H */
