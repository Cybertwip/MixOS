/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt67xx minimal LK: bring-up instrument, step 2 of LK-BRINGUP.md.
 *
 * What this is: serial hello, watchdog off, a clock that proves it ticks,
 * a banner that prints every fact in force (so the log names its own
 * assumptions), then a heartbeat park. What it is not: a bootloader --
 * LK-BRINGUP steps 3-5 (eMMC read, display + menu, AArch64 handoff) land
 * where the markers below say NEXT, each with its own grounding, and not
 * before.
 *
 * How to read a test boot:
 *   hello + heartbeat  UART prior right, MEMBASE right, WDT off. Proceed.
 *   hello then reset   WDT survived: the TOPRGU prior is wrong (LK-BRINGUP 2).
 *   silence            UART base wrong (rebuild -DMT67XX_DEBUG_UART=N, N=1..3),
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

#include "mt67xx_facts.h"
#include "mt67xx_timer.h"
#include "mt67xx_uart.h"
#include "mt67xx_wdt.h"

/* The data-abort resume pair the entry trampoline expects. No probe claims
 * it yet; display bring-up (step 4) will. */
volatile uint32_t mt67xx_stage1_abort_flag = 0u;
volatile uint32_t mt67xx_stage1_abort_resume = 0u;

#ifndef MT67XX_DEVICE
#define MT67XX_DEVICE "unknown-device"
#endif

#ifndef MVII_BUILD_COMMIT
#define MVII_BUILD_COMMIT "nogit"
#endif

#ifndef MT67XX_LK_SLOT_SIZE
#define MT67XX_LK_SLOT_SIZE 0x200000u
#endif

static const char* const k_vec_names[8] = {
    "reset", "undef", "svc", "pabt", "dabt", "reserved", "irq", "fiq",
};

void mt67xx_lk_report_exception(uint32_t vec, uint32_t pc, uint32_t spsr,
                                uint32_t dfsr, uint32_t dfar, uint32_t ifsr,
                                uint32_t ifar) {
    mt67xx_uart_puts("\n[mt67xx-lk] EXCEPTION vec=");
    if (vec < 8u) {
        mt67xx_uart_puts(k_vec_names[vec]);
    } else {
        mt67xx_uart_put_dec(vec);
    }
    mt67xx_uart_puts(" pc=");
    mt67xx_uart_put_hex32(pc);
    mt67xx_uart_puts(" spsr=");
    mt67xx_uart_put_hex32(spsr);
    mt67xx_uart_puts("\n[mt67xx-lk] dfsr=");
    mt67xx_uart_put_hex32(dfsr);
    mt67xx_uart_puts(" dfar=");
    mt67xx_uart_put_hex32(dfar);
    mt67xx_uart_puts(" ifsr=");
    mt67xx_uart_put_hex32(ifsr);
    mt67xx_uart_puts(" ifar=");
    mt67xx_uart_put_hex32(ifar);
    mt67xx_uart_puts("\n[mt67xx-lk] halted; the PC above names the lying prior\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void put_label_hex(const char* label, uint32_t value) {
    mt67xx_uart_puts(label);
    mt67xx_uart_put_hex32(value);
}

void mt67xx_lk_main(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    uint32_t beat = 0u;

    /* Order matters: the port first (everything below reports through it),
     * the watchdog off second (a live one punishes every stall below),
     * the clock third. */
    mt67xx_uart_init();
    mt67xx_wdt_disable();
    mt67xx_timer_init();

    mt67xx_uart_puts("[mt67xx-lk] MixOS minimal LK (bring-up) for ");
    mt67xx_uart_puts(MT67XX_DEVICE);
    mt67xx_uart_puts(" (mt");
    mt67xx_uart_put_dec(MT67XX_SOC);
    mt67xx_uart_puts("), commit ");
    mt67xx_uart_puts(MVII_BUILD_COMMIT);
    mt67xx_uart_puts("\n[mt67xx-lk] facts in force:");
    put_label_hex(" uart=", MT67XX_DEBUG_UART_BASE);
    mt67xx_uart_puts("/");
    mt67xx_uart_put_dec(MT67XX_UART_BAUD);
    put_label_hex(" membase=", MT67XX_MEMBASE);
    put_label_hex(" slot=", MT67XX_LK_SLOT_SIZE);
    mt67xx_uart_puts(" timer=");
    mt67xx_uart_puts(mt67xx_timer_source_name());
    mt67xx_uart_puts(mt67xx_timer_hw_ok() ? "(hw)" : "(SOFT: delays uncalibrated)");
#if MT67XX_HAS_WDT
    mt67xx_uart_puts(" wdt=off");
#else
    mt67xx_uart_puts(" wdt=UNTOUCHED(expect reset loop: LK-BRINGUP step 2b)");
#endif
    mt67xx_uart_puts("\n[mt67xx-lk] preloader args:");
    put_label_hex(" r0=", r0);
    put_label_hex(" r1=", r1);
    put_label_hex(" r2=", r2);
    put_label_hex(" r3=", r3);
    mt67xx_uart_puts("\n[mt67xx-lk] NEXT: LK-BRINGUP step 3 (eMMC read). "
                     "Parking with heartbeat.\n");

    /* NEXT(step 3): read boot.img from eMMC here; failure parks exactly
     * like this, with the breadcrumb naming the stage.
     * NEXT(step 4): light the panel, run the lk_bootmenu window
     * (lk_bootmenu.h / lk_menu_ui.h are built and host-tested already).
     * NEXT(step 5): AArch32->AArch64 switch and DTB handoff to the kernel.
     */
    for (;;) {
        mt67xx_timer_mdelay(5000u);
        ++beat;
        mt67xx_uart_puts("[mt67xx-lk] alive ");
        mt67xx_uart_put_dec(beat * 5u);
        mt67xx_uart_puts("s\n");
    }
}
