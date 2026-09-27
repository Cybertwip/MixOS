/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Novatek NT36672 touch geometry + wiring (20181-class boards).
 * The report protocol lives in linux/oppo_mt6877_touch_nt36672.c.
 *
 * From: reference/.../arch/arm64/boot/dts/mediatek/cust_mt6877_touch_nt36672.dtsi
 *       reference/.../drivers/input/touchscreen/mediatek/NT36xxx/nt36xxx.c
 */
#ifndef OPPO_TOUCH_NT36672_H
#define OPPO_TOUCH_NT36672_H

/* On SPI4, chip-select 0, falling-edge IRQ on GPIO 14. */
#define NT36672_SPI_BUS		4
#define NT36672_SPI_CS		0
#define NT36672_SPI_MAX_HZ	4800000
#define NT36672_IRQ_GPIO	14

/* 10 fingers over a 1080x2400 active area (touch glass is taller than the
 * 1080x2280 display window; the driver scales to the panel). */
#define NT36672_MAX_FINGERS	10
#define NT36672_TX_NUM		16
#define NT36672_RX_NUM		36
#define NT36672_COORD_X		1080
#define NT36672_COORD_Y		2400

/* Novatek report protocol (nt36xxx.c touch_event_handler): after the IRQ the
 * host reads a 66-byte frame. Byte 0 is a dummy; each of the 10 fingers owns
 * 6 bytes at 1+6*i: status+id, X-high, Y-high, nibble halves, W, P. */
#define NT36672_FINGER_DATA_LEN	6
#define NT36672_REPORT_SIZE	66

#endif /* OPPO_TOUCH_NT36672_H */
