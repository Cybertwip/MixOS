/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * MT6877 modem (MD) power/boot outline for linux/oppo_mt6877_modem.c.
 *
 * The MT6877 modem is an on-SoC baseband driven by MediaTek's ECCCI stack in
 * the reference kernel (drivers/misc/mediatek/eccci/, modem_sys3.c for this
 * generation: MD power domain on, MD SRAM ungated, boot handshake over the
 * MD<->AP sync registers, then CCIF channels for control and CLDMA for data).
 * Mainline 6.12 has no ECCCI driver, so the MixOS modem driver replays the
 * power/boot half of that sequence and exposes control to userspace; the
 * CCIF/CLDMA data path is stage 2 and its register map is reserved below.
 *
 * From: reference/.../drivers/misc/mediatek/eccci/modem_sys3.c
 *       reference/.../drivers/misc/mediatek/eccci/hif/ (CCIF/CLDMA layout)
 */
#ifndef OPPO_MODEM_MD_H
#define OPPO_MODEM_MD_H

/* MD boot states the driver walks through, in order. */
#define OPPO_MD_STATE_OFF	0
#define OPPO_MD_STATE_POWER	1
#define OPPO_MD_STATE_SRAM	2
#define OPPO_MD_STATE_BOOT	3
#define OPPO_MD_STATE_READY	4
#define OPPO_MD_STATE_FAILED	5

/* Firmware the driver requests from /lib/firmware (taken from stock). */
#define OPPO_MD_FW_IMAGE	"oppo/mt6877/modem.img"
#define OPPO_MD_DSP_IMAGE	"oppo/mt6877/dsp.img"

/* CCIF channel plan (stage 2 data path; control channel is 0). */
#define OPPO_MD_CCIF_CTRL_CH	0
#define OPPO_MD_CCIF_DATA_CH	1
#define OPPO_MD_CCIF_MAX_CH	8

#endif /* OPPO_MODEM_MD_H */
