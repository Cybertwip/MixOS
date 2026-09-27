/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * lv517 key wiring. One GPIO rocker + hall sensor on TLMM, one PMIC key:
 * the base DTS maps GPIO91 to VOL_UP (115) and rev-0 remaps it to VOL_DOWN
 * (114), with the PMIC PON key taking the other slot. The generator emits
 * the mapping for LG_REV; the driver polls TLMM MMIO (no pinctrl yet).
 *
 * From: msm8917-lv517_gsm_us-misc.dtsi + pmi8950-lv517_gsm_us_rev-0.dts.
 */
#ifndef LG_KEYS_LV517_H
#define LG_KEYS_LV517_H

#define LV517_KEY_GPIO		91
#define LV517_KEY_VOL_UP	115
#define LV517_KEY_VOL_DOWN	114
#define LV517_HALL_GPIO		12
#define LV517_HALL_TYPE		5
#define LV517_HALL_CODE		222

#endif /* LG_KEYS_LV517_H */
