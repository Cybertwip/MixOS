/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * MSDC (eMMC/SD) capability summary for generate_dts_oppo.py.
 *
 * From: reference/.../arch/arm64/boot/dts/mediatek/cust_mt6877_msdc.dtsi
 */
#ifndef OPPO_MSDC_MT6877_H
#define OPPO_MSDC_MT6877_H

/* msdc0: soldered eMMC, 8-bit, HS400 @ 200 MHz, non-removable, bootable. */
#define MSDC0_BUS_WIDTH		8
#define MSDC0_MAX_HZ		200000000
#define MSDC0_HS400		1
#define MSDC0_HS200		1
#define MSDC0_DDR_1_8V		1

/* msdc1: SD slot, 4-bit, UHS SDR50 @ 200 MHz. */
#define MSDC1_BUS_WIDTH		4
#define MSDC1_MAX_HZ		200000000
#define MSDC1_SDR50		1

#endif /* OPPO_MSDC_MT6877_H */
