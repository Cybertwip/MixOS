#ifndef MT6592_DELAY_H
#define MT6592_DELAY_H

#include <stdint.h>

/*
 * Wall-clock-stable replacement for the old "volatile counter" spin delays.
 *
 * The MT6592 driver delay constants in this tree were tuned with the MMU and
 * caches off, where the CPU retires roughly 33 spin iterations per
 * microsecond (the delay_panel_ms convention: 33000 cycles == 1 ms). Stage2
 * now runs with caches enabled (the OS MMU setup), which would shrink a pure
 * spin ~30-50x and violate hardware settle times (eMMC power/clock switches,
 * MUSB resets, SD card ramp). Anchor the delay to the free-running 13 MHz
 * APXGPT GPT4 counter instead — the preloader/LK leave it ticking on this
 * family — and fall back to the legacy spin if it is not (very early
 * bring-up), where execution is uncached anyway and the old calibration
 * still holds.
 *
 * Header-inline on purpose: the callers (mt6592_msdc.c, mt6592_msdc_sd.c,
 * the MUSB host driver) are linked into stage1/flash payloads that do not link
 * mt6592_timer.c, so this must not add an object-file dependency.
 */

enum {
    MT6592_DELAY_GPT_BASE = 0x10008000u, /* APXGPT on the MT6592 generation */
    MT6592_DELAY_GPT4_DAT = 0x48u,
    MT6592_DELAY_TICKS_PER_US = 13u,
    MT6592_DELAY_LEGACY_CYCLES_PER_US = 33u,
    MT6592_DELAY_TICK_PROBE_READS = 64u,
};

/* Raw 13 MHz GPT4 tick counter for elapsed-time measurement (wrap-safe via
 * unsigned subtraction; 32 bits at 13 MHz wraps every ~330 s, far longer than
 * any window measured with it). Returns whatever the counter reads — callers
 * that must survive a dead GPT should pair tick deltas with a sample-count
 * fallback (see the OS keypad driver's joy calibration). */
static inline uint32_t mt6592_delay_gpt_ticks(void) {
    return *(volatile uint32_t*)(uintptr_t)(MT6592_DELAY_GPT_BASE + MT6592_DELAY_GPT4_DAT);
}

static inline uint32_t mt6592_delay_ticks_to_us(uint32_t ticks) {
    return ticks / (uint32_t)MT6592_DELAY_TICKS_PER_US;
}

static inline void mt6592_delay_cycles(uint32_t cycles) {
    volatile uint32_t* dat =
        (volatile uint32_t*)(uintptr_t)(MT6592_DELAY_GPT_BASE + MT6592_DELAY_GPT4_DAT);
    uint32_t us = cycles / MT6592_DELAY_LEGACY_CYCLES_PER_US + 1u;
    uint32_t ticks = us * MT6592_DELAY_TICKS_PER_US;
    uint32_t start = *dat;
    uint32_t ticking = 0u;

    /* 64 Strongly-Ordered APB reads take multiple microseconds — far longer
     * than one 13 MHz tick — so an unchanged counter means GPT4 is not
     * running yet. */
    for (uint32_t i = 0; i < MT6592_DELAY_TICK_PROBE_READS; ++i) {
        if (*dat != start) {
            ticking = 1u;
            break;
        }
    }

    if (!ticking) {
        for (volatile uint32_t i = 0; i < cycles; ++i) {
        }
        return;
    }

    while ((uint32_t)(*dat - start) < ticks) {
    }
}

#endif
