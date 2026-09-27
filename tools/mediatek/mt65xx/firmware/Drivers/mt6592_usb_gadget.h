/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MT6592_USB_GADGET_H
#define MT6592_USB_GADGET_H

#include <stdint.h>

/*
 * MT6592 MUSB peripheral ("gadget") transport: two bulk pipes on the OTG port,
 * enough for a live debug console and nothing more.
 *
 * WHY THIS EXISTS
 * ---------------
 * The BROM download agent talks to the host through the BROM's own usbdl
 * callbacks (see flash_stage.c usb_init(): a vector table at SRAM 0x0000a564).
 * That transport has two properties that make it useless for debugging this
 * board:
 *
 *   1. It only exists in BROM. Any reboot takes it away, so nothing that
 *      happens on a real boot -- which is where every remaining bug lives --
 *      can be observed through it.
 *   2. It dies when the display or audio clocks are reprogrammed. That is not a
 *      guess: the live `panel` command returned LIBUSB_ERROR_NO_DEVICE, and an
 *      earlier AFE_DAC_CON0 write did the same thing.
 *
 * So the console needs a USB device that belongs to *us* and comes up on a
 * normally booted board. This is that device.
 *
 * WHAT IS BORROWED AND WHAT IS INVENTED
 * -------------------------------------
 * The PHY sequence is not invented. Reference/j36-lk-reverse/lk.full-decompile.c
 * FUN_81e09520 (line 9866) is the stock LK's usb_phy_recover() for this exact
 * SoC and board, and phy_recover() below is a transcription of it, register for
 * register and delay for delay. FUN_81e093b8 (line 9825) is the matching
 * savecurrent()/power-down and is transcribed too, so the console can hand a
 * quiet PHY back before jumping to the kernel.
 *
 * The MUSB peripheral state machine is generic Mentor MUSBMHDRC and is written
 * from the register semantics; the register offsets are shared with
 * the MUSB host driver, which is the same core in the other role. Two details
 * were taken from the decompile rather than assumed, because getting them wrong
 * is silent: the dynamic-FIFO address unit is 8 bytes (FUN_81e09a1c computes
 * TXFIFOADD as addr/8 via `(addr << 13) >> 16`) and FIFOSZ is log2(bytes) - 3.
 *
 * DESIGN CHOICES THAT BUY RELIABILITY
 * -----------------------------------
 * Full speed only. HSENAB is deliberately left clear: high speed adds a chirp
 * negotiation and a 512-byte packet size that this console has no use for, and
 * a console that enumerates is worth more than a fast one. Every packet is
 * therefore 64 bytes and every control transfer fits in one packet.
 *
 * Separate endpoints per direction (EP1 IN, EP2 OUT) rather than one
 * bidirectional EP1. MUSB's TXCSR.MODE bit only matters when an endpoint's TX
 * and RX share a FIFO; giving each direction its own endpoint and its own FIFO
 * window removes the question entirely.
 *
 * Fully polled, no interrupts, no allocation -- the same rules the rest of this
 * bring-up plays by.
 */

enum {
    /* Vendor-specific class with two bulk pipes, so no host OS claims it and
     * libusb can. The PID is ours, not a MediaTek one: the BROM device is
     * 0x0e8d/0x2000-ish and the host tool must be able to tell "board is in
     * BROM" from "board is booted and running the console" by VID:PID alone. */
    MT6592_GADGET_VID = 0x0e8du,
    MT6592_GADGET_PID = 0x4d56u, /* 'MV' */

    MT6592_GADGET_PACKET = 64u,
};

/* Bring up the PHY and the MAC and attach the D+ pull-up. Returns 0 if the
 * register windows answered; it does not wait for a host. Idempotent. */
int mt6592_usb_gadget_init(void);

/* Service bus reset and EP0 control traffic. Must be called often enough that
 * the host's enumeration does not time out -- every send/recv below calls it,
 * and an idle console should call it in its wait loop. */
void mt6592_usb_gadget_poll(void);

/* Non-zero once the host has issued SET_CONFIGURATION, i.e. the bulk pipes are
 * usable. Goes back to zero on unplug. */
int mt6592_usb_gadget_configured(void);

/* Log POWER, DEVCTL and the bus-reset / SETUP counters to UART and the eMMC
 * boot ring. Meant for the enumeration wait: when no host ever attaches, this
 * line is the only evidence of why, and it survives on storage whether or not
 * the console itself ever gets a byte out. */
void mt6592_usb_gadget_log_state(void);

/* Queue up to `len` bytes to the host on EP1 IN. Blocks until the packets are
 * handed to the controller (bounded), servicing EP0 while it waits. Returns the
 * number of bytes accepted, or -1 if the link went away. */
int mt6592_usb_gadget_send(const void* data, uint32_t len);

/* Collect one packet from EP2 OUT if one has arrived. Returns the byte count
 * (0 when nothing is pending, which is not an error), or -1 if the link went
 * away. Never blocks. */
int mt6592_usb_gadget_recv(void* data, uint32_t max);

/* Detach and power the PHY down, for the hand-off to the kernel: leaving a
 * pull-up asserted on a controller nobody is servicing makes the host retry
 * enumeration forever. */
void mt6592_usb_gadget_shutdown(void);

#endif /* MT6592_USB_GADGET_H */
