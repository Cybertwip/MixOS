/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MVII_DEBUG_CONSOLE_H
#define MVII_DEBUG_CONSOLE_H

#include <stdint.h>

/*
 * Live debug console, served from the LK over the MUSB peripheral port.
 *
 * WHY IT LIVES HERE AND NOT IN THE BROM PAYLOAD
 * ---------------------------------------------
 * The BROM payload can be trusted to write eMMC and to reset the SoC; past that
 * it is on borrowed hardware (its USB transport is the BROM's own, and it dies
 * the moment a clock it depends on is reprogrammed). Every bug still open on
 * this board -- the battery reading, the LED channels, the panel bring-up
 * timing, the missing left click -- is a bug in a *booted* machine, so the
 * instrument has to be a booted machine too.
 *
 * So: the BROM menu's "debug mode" only drops a one-shot flag
 * (mt6592_dbgflag.h) and reboots. The board then takes its ordinary path
 * (BROM -> preloader -> this LK), the LK finds the flag, and instead of jumping
 * to the kernel it runs this: a plain-text, line-oriented console on a USB
 * device we own (mt6592_usb_gadget.h), with the SoC and PMIC fully alive.
 *
 * PROTOCOL
 * --------
 * Deliberately the dumbest thing that works, because the transport is new and a
 * framing bug would be indistinguishable from a driver bug. Device to host:
 * ASCII, newline-terminated lines, no framing, no length prefix, no escaping.
 * Host to device: one command per line, terminated by \n or \r. The device
 * echoes nothing and never blocks the host. Every line the console emits is also
 * appended to the eMMC console ring, so even a session where the gadget never
 * enumerates leaves a full transcript behind for
 * `flash -mtk-read-boot-status`.
 *
 * HOW IT IS SUMMONED
 * ------------------
 * Primarily by holding POWER + VOL_UP through the LK's early init; the eMMC flag
 * the BROM payload writes is the fallback. See lk_debug_combo_held() in
 * mvii_lk_main.c for why that order is the right way round.
 *
 * SAFETY
 * ------
 * The console is bounded, in two independent ways. Neither trigger persists: the
 * flag is cleared before the console starts, and the buttons are sampled once,
 * so a wedge costs one power cycle. And if no host claims the device within
 * MVII_DEBUG_CONSOLE_WAIT_MS the console gives up and returns, letting the LK
 * boot normally -- being unable to enumerate must look like a slow boot, never
 * like a dead board.
 */

enum {
    /* How long to wait for a host to enumerate before giving up and booting.
     *
     * 60 s, sized to the human on the other end: they hold POWER + VOL_UP, wait
     * for the board to come up, then switch to a terminal and run `flash -dbg`.
     * Thirty seconds only covered someone already poised over the keyboard, and
     * a console that times out two seconds early looks exactly like one that is
     * broken -- a full reboot to find out otherwise. Being wrong the other way
     * costs a minute of black screen on a boot the operator asked to be
     * different. */
    MVII_DEBUG_CONSOLE_WAIT_MS = 60000u,

    /* How long to wait for a host that has ALREADY attached once to come back.
     *
     * A lost link no longer ends the console (see the outer loop in
     * mvii_debug_console_run), so this is the other side of that: the bridge
     * stays up across a reseated cable, a restarted host tool or a stray BROM
     * probe, but it does not stay up forever on a board nobody is using.
     *
     * Five minutes, not sixty seconds, because the two waits are answering
     * different questions. The first is "did anyone ever want this console",
     * where a minute is generous. This one is "is the operator still there",
     * asked of someone who demonstrably was a moment ago -- and the things that
     * drop a link in the middle of a session (unplug the board, flash something
     * else, re-run the host tool) routinely take longer than a minute. Timing
     * out on them would throw away a live session for someone who never left.
     *
     * It is bounded at all because VBAT is VSYS on this board: an unattended
     * console is a lit panel draining a cell. And it is cheap to get wrong now
     * -- a double-tap on the charge screen re-enters the console with no
     * reboot, so a timeout costs two taps rather than the whole button dance. */
    MVII_DEBUG_CONSOLE_REATTACH_MS = 300000u,
};

/* Optional hooks for the things the console can drive but does not own. Any
 * member may be NULL; the matching command then reports that it is unavailable
 * rather than misbehaving. */
typedef struct {
    /* Bring the panel up (the LK's own display path). Returns 0 on success. */
    int (*display_on)(void);
    /* Fill the framebuffer with an ARGB colour, for eyeballing scanout. */
    void (*fb_fill)(uint32_t argb);
} mvii_debug_console_hooks_t;

/* Run the console until the operator types `boot` (or the wait for a host times
 * out). Returns so the caller can carry on booting; `reboot` does not return. */
void mvii_debug_console_run(const mvii_debug_console_hooks_t* hooks);

/*
 * Read back the battery-presence verdict an operator's console probe left on the
 * card, and LOG it. Nothing branches on the result.
 *
 * Presence on this board is not readable from any live sensor -- seven were tried
 * and every one measured to fail, most recently the voltage window (a full pack
 * reads 4177 mV and an empty holder 4178 mV, because there is no power-path FET
 * and VBAT *is* VSYS) -- and the one test that works costs the board a power cycle
 * when the holder is empty. So mt6592_pmic.h has no presence property at all and
 * minos_platform_battery_present() answers -1, which the renderer treats as
 * "assume fitted".
 *
 * What is left is this: `bat cal'/`bat probe'/`bat auto' write a verdict to the
 * breadcrumb sector, and every boot picks it up here so the operator can see it in
 * the boot log. It writes no PMIC register, NEVER runs the destructive probe
 * itself, and -- unlike the version this replaced -- publishes nothing.
 *
 * Returns 1 fitted, 0 absent, -1 nothing on the card yet, which is the answer on
 * any board nobody has probed by hand. Safe to call with no SD card (returns -1).
 */
int mvii_battery_boot_policy(void);

#endif /* MVII_DEBUG_CONSOLE_H */
