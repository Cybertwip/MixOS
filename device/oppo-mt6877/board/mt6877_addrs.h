/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * MT6877 (Dimensity 900) base addresses and IRQ lines.
 * From: reference/android_kernel_oppo_mt6877/arch/arm64/boot/dts/mediatek/mt6877.dts
 * Parsed by generate_dts_oppo.py; kept numeric so plain dtc compiles the output.
 */
#ifndef OPPO_MT6877_ADDRS_H
#define OPPO_MT6877_ADDRS_H

/* GICv3: distributor + one redistributor region (8 CPUs: 2xA78 + 6xA55, PSCI). */
#define MT6877_GICD_BASE	0x0c000000UL
#define MT6877_GICD_SIZE	0x40000UL
#define MT6877_GICR_BASE	0x0c040000UL
#define MT6877_GICR_SIZE	0x200000UL

/* UARTs: mediatek,mt6577-uart. LK/kernel console is uart0. */
#define MT6877_UART0_BASE	0x11002000UL
#define MT6877_UART1_BASE	0x11003000UL
#define MT6877_UART_SIZE	0x1000UL

/* MSDC: msdc0 is the eMMC (HS400, 8-bit); msdc1 is the SD slot. */
#define MT6877_MSDC0_BASE	0x11230000UL
#define MT6877_MSDC0_SIZE	0x10000UL
#define MT6877_MSDC1_BASE	0x11240000UL
#define MT6877_MSDC1_SIZE	0x1000UL

/* SPI: spi4 carries the NT36672 touch controller. */
#define MT6877_SPI0_BASE	0x1100a000UL
#define MT6877_SPI4_BASE	0x11018000UL
#define MT6877_SPI_SIZE		0x100UL

/* CONSYS (Wi-Fi/BT/GPS combo): conn_infra block. */
#define MT6877_CONSYS_BASE	0x18000000UL
#define MT6877_CONSYS_SIZE	0x470UL
#define MT6877_CONSYS_CFG	0x18001000UL
#define MT6877_CONSYS_CFG_SIZE	0x658UL

/* Audio: AFE + audiosys syscon. */
#define MT6877_AFE_BASE		0x11210000UL
#define MT6877_AFE_SIZE		0x2000UL

/* PMIC wrapper + infra config (clocks/resets live here). */
#define MT6877_PWRAP_BASE	0x10026000UL
#define MT6877_PWRAP_SIZE	0x1000UL
#define MT6877_INFRACFG_AO_BASE	0x10001000UL
#define MT6877_INFRACFG_AO_SIZE	0x1000UL

/* I2C used for backlight/bias + PMIC-adjacent parts. */
#define MT6877_I2C1_BASE	0x11d20000UL
#define MT6877_I2C_SIZE		0x1000UL

/* GPIO block (Paris layout): DIR 0x000, DOUT 0x100, DIN 0x200, 32 pins
 * per register, 0x10 stride. Volume keys are EINT-capable GPIOs:
 * VOL_UP = GPIO120, VOL_DOWN = GPIO114 (oplus6877_20181_v1.dts).
 * Power is the MT6359 PMIC key. */
#define MT6877_GPIO_BASE	0x10005000UL
#define MT6877_GPIO_SIZE	0x1000UL
#define MT6877_GPIO_DIN_OFF	0x200UL
#define MT6877_KEY_VOL_UP_GPIO	120
#define MT6877_KEY_VOL_DOWN_GPIO 114

/* Keypad controller (mediatek,kp), SPI 106. */
#define MT6877_KP_BASE		0x10010000UL
#define MT6877_KP_SIZE		0x1000UL
#define MT6877_IRQ_KP		106

/* Touch wiring on 20181-class boards (cust_mt6877_touch_nt36672.dtsi). */
#define MT6877_TOUCH_IRQ_GPIO	14
#define MT6877_TOUCH_SPI_HZ	4800000

/* GIC SPI lines (mt6877.dts). GIC_SPI = 0, LEVEL_HIGH = 4 in DTS cells. */
#define MT6877_IRQ_UART0	141
#define MT6877_IRQ_MSDC0	131
#define MT6877_IRQ_MSDC1	135
#define MT6877_IRQ_SPI4		207
#define MT6877_IRQ_AFE		246

/* LK framebuffer handoff (oplus6877_20181.dts chosen/atag): TD4330 FHD+. */
#define MT6877_LK_FB_BASE	0x7e605000UL
#define MT6877_LK_FB_SIZE	0x1be0000UL
#define MT6877_PANEL_WIDTH	1080
#define MT6877_PANEL_HEIGHT	2280

#endif /* OPPO_MT6877_ADDRS_H */
