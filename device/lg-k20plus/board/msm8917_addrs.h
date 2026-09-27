/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * MSM8917 (Snapdragon 425) base addresses and IRQ lines.
 * From: reference/.../arch/arm/boot/dts/qcom/msm8917.dtsi
 * Parsed by generate_dts_lg.py; kept numeric so plain dtc compiles the output.
 */
#ifndef LG_MSM8917_ADDRS_H
#define LG_MSM8917_ADDRS_H

/* qcom,msm-qgic2 (same binding mainline uses for msm8916). */
#define MSM8917_GIC_BASE	0x0b000000UL
#define MSM8917_GIC_SIZE	0x1000UL
#define MSM8917_GIC_CPU_BASE	0x0b002000UL
#define MSM8917_GIC_CPU_SIZE	0x2000UL

/* BLSP1 UART2: LK/kernel console (qcom,msm-lsuart-v14). */
#define MSM8917_UART_BASE	0x078b0000UL
#define MSM8917_UART_SIZE	0x200UL
#define MSM8917_IRQ_UART	108

/* SDHCI: sdhc_1 (eMMC, HS400) and sdhc_2 (SD slot). */
#define MSM8917_SDC1_HC_BASE	0x07824900UL
#define MSM8917_SDC1_CORE_BASE	0x07824000UL
#define MSM8917_SDC1_HC_SIZE	0x500UL
#define MSM8917_SDC1_CORE_SIZE	0x800UL
#define MSM8917_IRQ_SDC1	123
#define MSM8917_IRQ_SDC1_PWR	138
#define MSM8917_SDC2_HC_BASE	0x07864900UL
#define MSM8917_SDC2_CORE_BASE	0x07864000UL
#define MSM8917_SDC2_HC_SIZE	0x500UL
#define MSM8917_SDC2_CORE_SIZE	0x800UL
#define MSM8917_IRQ_SDC2	125
#define MSM8917_IRQ_SDC2_PWR	221

/* BLSP1 QUP3: i2c_3, the touch bus (400 kHz). */
#define MSM8917_I2C3_BASE	0x078b7000UL
#define MSM8917_I2C_SIZE	0x600UL
#define MSM8917_IRQ_I2C3	97

/* TLMM: pin N: ctl N*0x1000, in/out N*0x1000+0x4, IN = bit 0
 * (pinctrl-msm8917.c REG_BASE/REG_SIZE + in_bit). */
#define MSM8917_TLMM_BASE	0x01000000UL
#define MSM8917_TLMM_SIZE	0x1000UL
#define MSM8917_TLMM_STRIDE	0x1000UL
#define MSM8917_TLMM_IO_OFF	0x4UL

/* Touch wiring (msm8917-lv517_gsm_us-touch.dtsi): reset 64, IRQ 65. */
#define MSM8917_TOUCH_RESET_GPIO 64
#define MSM8917_TOUCH_IRQ_GPIO	65
#define MSM8917_TOUCH_LG4894_ADDR 0x28
#define MSM8917_TOUCH_TD4100_ADDR 0x20

/* SPMI arbiter + PMI8950 (WLED backlight, pwrkey, charger). */
#define MSM8917_SPMI_BASE	0x0200f000UL
#define MSM8917_SPMI_SIZE	0x1000UL

/* MDSS (display): documented for the panel driver; the controller itself
 * is NOT in the v1 DTS (no mainline msm8917 mdss) -- the LK framebuffer
 * node below is the v1 display. */
#define MSM8917_MDSS_BASE	0x01800000UL

/* Pronto (WCN3660 Wi-Fi): PIL region for wcnss_ctrl. */
#define MSM8917_PRONTO_BASE	0x0a21b000UL
#define MSM8917_PRONTO_SIZE	0x3000UL
#define MSM8917_IRQ_PRONTO	149

/* LK splash framebuffer: NOT in the reference DTS. Measure from the LK
 * log (or `fastboot getvar`) on first hardware contact and pass
 * --lk-fb-base to the generator; until then the framebuffer node is
 * emitted disabled and the console is serial-only. */
#define MSM8917_LK_FB_BASE	0x0UL

#endif /* LG_MSM8917_ADDRS_H */
