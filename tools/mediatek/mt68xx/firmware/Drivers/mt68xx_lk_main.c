/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx minimal LK: bring-up instrument, step 2 of LK-BRINGUP.md.
 *
 * What this is: serial hello, a clock that proves it ticks, a banner that
 * prints every fact in force (so the log names its own assumptions), then
 * a heartbeat park. What it is not: a bootloader -- LK-BRINGUP steps 2b-5
 * (WDT, GPT, eMMC read, display + menu, AArch64 handoff) land where the
 * markers below say NEXT, each with its own grounding, and not before.
 *
 * NOTE: the watchdog write is compiled OUT on this family (TOPRGU base
 * ungrounded on Dimensity -- see the facts header), so hello-then-reset
 * is the EXPECTED first result, not a failure: it proves UART + MEMBASE
 * right and queues LK-BRINGUP step 2b.
 *
 * How to read a test boot:
 *   hello + heartbeat  UART right, MEMBASE right, and the WDT was already
 *                      off (or step 2b landed). Proceed.
 *   hello then reset   Expected until step 2b: the preloader armed the WDT
 *                      and this image cannot turn it off yet.
 *   silence            UART base wrong (rebuild -DMT68XX_DEBUG_UART=N, N=1..3),
 *                      or MEMBASE wrong (LK-BRINGUP step 1 re-reads it from
 *                      the stock LK). Both are priors; the log cannot tell
 *                      them apart because there is no log. That is what the
 *                      boot dir's FACTS.md is for -- it lists the priors in
 *                      force before anything is flashed.
 *   exception report   A vector fired: the report names the PC and fault
 *                      registers. During bring-up this is telemetry, not
 *                      failure -- it says exactly which prior lied.
 */
#include <stdint.h>

#include "mt68xx_facts.h"
#include "mt68xx_timer.h"
#include "mt68xx_uart.h"
#include "mt68xx_wdt.h"

/* The data-abort resume pair the entry trampoline expects. No probe claims
 * it yet; display bring-up (step 4) will. */
volatile uint32_t mt68xx_stage1_abort_flag = 0u;
volatile uint32_t mt68xx_stage1_abort_resume = 0u;

#ifndef MT68XX_DEVICE
#define MT68XX_DEVICE "unknown-device"
#endif

#ifndef MVII_BUILD_COMMIT
#define MVII_BUILD_COMMIT "nogit"
#endif

#ifndef MT68XX_LK_SLOT_SIZE
#define MT68XX_LK_SLOT_SIZE 0x200000u
#endif

static const char* const k_vec_names[8] = {
    "reset", "undef", "svc", "pabt", "dabt", "reserved", "irq", "fiq",
};

void mt68xx_lk_report_exception(uint32_t vec, uint32_t pc, uint32_t spsr,
                                uint32_t dfsr, uint32_t dfar, uint32_t ifsr,
                                uint32_t ifar) {
    mt68xx_uart_puts("\n[mt68xx-lk] EXCEPTION vec=");
    if (vec < 8u) {
        mt68xx_uart_puts(k_vec_names[vec]);
    } else {
        mt68xx_uart_put_dec(vec);
    }
    mt68xx_uart_puts(" pc=");
    mt68xx_uart_put_hex32(pc);
    mt68xx_uart_puts(" spsr=");
    mt68xx_uart_put_hex32(spsr);
    mt68xx_uart_puts("\n[mt68xx-lk] dfsr=");
    mt68xx_uart_put_hex32(dfsr);
    mt68xx_uart_puts(" dfar=");
    mt68xx_uart_put_hex32(dfar);
    mt68xx_uart_puts(" ifsr=");
    mt68xx_uart_put_hex32(ifsr);
    mt68xx_uart_puts(" ifar=");
    mt68xx_uart_put_hex32(ifar);
    mt68xx_uart_puts("\n[mt68xx-lk] halted; the PC above names the lying prior\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void put_label_hex(const char* label, uint32_t value) {
    mt68xx_uart_puts(label);
    mt68xx_uart_put_hex32(value);
}

void mt68xx_lk_main(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    uint32_t beat = 0u;

    /* Order matters: the port first (everything below reports through it),
     * the watchdog call second (a no-op until step 2b grounds TOPRGU --
     * the call stays so the step lands without restructuring main),
     * the clock third. */
    mt68xx_uart_init();
    mt68xx_wdt_disable();
    mt68xx_timer_init();

    mt68xx_uart_puts("[mt68xx-lk] MixOS minimal LK (bring-up) for ");
    mt68xx_uart_puts(MT68XX_DEVICE);
    mt68xx_uart_puts(" (mt");
    mt68xx_uart_put_dec(MT68XX_SOC);
    mt68xx_uart_puts("), commit ");
    mt68xx_uart_puts(MVII_BUILD_COMMIT);
    mt68xx_uart_puts("\n[mt68xx-lk] facts in force:");
    put_label_hex(" uart=", MT68XX_DEBUG_UART_BASE);
    mt68xx_uart_puts("/");
    mt68xx_uart_put_dec(MT68XX_UART_BAUD);
    put_label_hex(" membase=", MT68XX_MEMBASE);
    put_label_hex(" slot=", MT68XX_LK_SLOT_SIZE);
    mt68xx_uart_puts(" timer=");
    mt68xx_uart_puts(mt68xx_timer_source_name());
    mt68xx_uart_puts(mt68xx_timer_hw_ok() ? "(hw)" : "(SOFT: delays uncalibrated)");
#if MT68XX_HAS_WDT
    mt68xx_uart_puts(" wdt=off");
#else
    mt68xx_uart_puts(" wdt=UNTOUCHED(expect reset loop: LK-BRINGUP step 2b)");
#endif
    mt68xx_uart_puts("\n[mt68xx-lk] preloader args:");
    put_label_hex(" r0=", r0);
    put_label_hex(" r1=", r1);
    put_label_hex(" r2=", r2);
    put_label_hex(" r3=", r3);
    mt68xx_uart_puts("\n[mt68xx-lk] NEXT: LK-BRINGUP step 2b (WDT base). "
                     "Parking with heartbeat.\n");

    /* NEXT(step 2b/2c): flip HAS_WDT/HAS_GPT when the bases land; nothing
     * here changes shape.
     * NEXT(step 3): read boot.img from eMMC here; failure parks exactly
     * like this, with the breadcrumb naming the stage.
     * NEXT(step 4): light the panel, run the lk_bootmenu window
     * (lk_bootmenu.h / lk_menu_ui.h are built and host-tested already).
     * NEXT(step 5): AArch32->AArch64 switch and DTB handoff to the kernel.
     */
    for (;;) {
        mt68xx_timer_mdelay(5000u);
        ++beat;
        mt68xx_uart_puts("[mt68xx-lk] alive ");
        mt68xx_uart_put_dec(beat * 5u);
        mt68xx_uart_puts("s\n");
    }
}
