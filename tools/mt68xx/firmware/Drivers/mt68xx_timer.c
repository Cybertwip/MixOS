/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx timer implementation: arch counter first, GPT4 free-run second,
 * software counter last. Detection, never assumption: each source has to
 * prove it ticks before it is used, because a dead counter that returns a
 * constant would hang every delay in the tree.
 */
#include "mt68xx_timer.h"

#include "mt68xx_facts.h"

enum {
    TIMER_SRC_NONE = 0,
    TIMER_SRC_ARCH = 1,
    TIMER_SRC_GPT4 = 2,
    TIMER_SRC_SOFT = 3,
};

static int g_src = TIMER_SRC_NONE;
static uint32_t g_arch_mhz = 0u;
static uint32_t g_start = 0u;
static uint32_t g_soft = 0u;

#if MT68XX_HAS_GPT
enum {
    GPT_CON_EN = 0x0001u,
    GPT_CON_FREERUN = 0x0030u,
    GPT_CON_CLEAR = 0x0002u,
    GPT_CLK_SYS = 0x0000u,
};

static uint32_t reg_read(uint32_t addr) {
    return *(volatile uint32_t*)(uintptr_t)addr;
}

static void reg_write(uint32_t addr, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)addr = value;
}
#endif

static uint32_t arch_cntfrq(void) {
    uint32_t v = 0u;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

static uint32_t arch_cntpct_lo(void) {
    uint32_t lo = 0u, hi = 0u;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    (void)hi;
    return lo;
}

static int arch_try(void) {
    const uint32_t frq = arch_cntfrq();
    uint32_t a, b;

    /* CNTFRQ of 0 means the preloader never set it up: unusable. Anything
     * below 1 MHz cannot resolve microseconds: unusable. */
    if (frq < 1000000u) return 0;
    a = arch_cntpct_lo();
    b = arch_cntpct_lo();
    if (a == b) {
        /* One more chance: back-to-back reads can straddle nothing at low
         * frequencies. A bounded spin that still sees no movement is dead. */
        for (uint32_t i = 0u; i < 1000u; ++i) {
            __asm__ volatile("" ::: "memory");
        }
        b = arch_cntpct_lo();
        if (a == b) return 0;
    }
    g_arch_mhz = (frq + 500000u) / 1000000u;
    if (g_arch_mhz == 0u) return 0;
    g_start = b;
    g_src = TIMER_SRC_ARCH;
    return 1;
}

#if MT68XX_HAS_GPT
static void gpt4_power(int on) {
    const uint32_t addr = MT68XX_PERICFG_BASE + 0x10u;
    uint32_t v = reg_read(addr);

    if (on) {
        v &= ~(1u << MT68XX_GPT_PDN_BIT);
    } else {
        v |= 1u << MT68XX_GPT_PDN_BIT;
    }
    reg_write(addr, v);
}

static int gpt4_try(void) {
    uint32_t a, b;

    /* [mt6735] mt_gpt.c: power on, stop, clear, sys clock, free-run. */
    gpt4_power(1);
    reg_write(MT68XX_GPT4_CON, 0u);
    reg_write(MT68XX_GPT4_CON, GPT_CON_CLEAR);
    reg_write(MT68XX_GPT4_CLK, GPT_CLK_SYS);
    reg_write(MT68XX_GPT4_CON, GPT_CON_EN | GPT_CON_FREERUN);
    a = reg_read(MT68XX_GPT4_DAT);
    for (uint32_t i = 0u; i < 1000u; ++i) {
        __asm__ volatile("" ::: "memory");
    }
    b = reg_read(MT68XX_GPT4_DAT);
    if (a == b) {
        reg_write(MT68XX_GPT4_CON, 0u);
        return 0;
    }
    g_start = b;
    g_src = TIMER_SRC_GPT4;
    return 1;
}
#endif

void mt68xx_timer_init(void) {
    if (g_src != TIMER_SRC_NONE) return;
    if (arch_try()) return;
#if MT68XX_HAS_GPT
    if (gpt4_try()) return;
#endif
    g_src = TIMER_SRC_SOFT;
    g_start = 0u;
    g_soft = 0u;
}

uint32_t mt68xx_timer_microseconds(void) {
    uint32_t now;

    if (g_src == TIMER_SRC_NONE) mt68xx_timer_init();
    switch (g_src) {
    case TIMER_SRC_ARCH:
        now = arch_cntpct_lo();
        return (now - g_start) / g_arch_mhz;
#if MT68XX_HAS_GPT
    case TIMER_SRC_GPT4:
        now = reg_read(MT68XX_GPT4_DAT);
        return (now - g_start + MT68XX_GPT_TICKS_PER_US / 2u) /
               MT68XX_GPT_TICKS_PER_US;
#endif
    default:
        /* Monotonic and wrong-rate: each call is one more tick, and the
         * banner said "soft" so nobody mistakes it for time. */
        g_soft += 1000u;
        return g_soft;
    }
}

void mt68xx_timer_mdelay(uint32_t ms) {
    if (g_src == TIMER_SRC_SOFT) {
        /* Uncalibrated by definition: spin a bounded pile of iterations
         * per millisecond so the delay terminates near the right order of
         * magnitude on an A55/A76 at any sane clock. */
        for (uint32_t i = 0u; i < ms * 1000u; ++i) {
            __asm__ volatile("" ::: "memory");
        }
        return;
    }
    {
        const uint32_t end = mt68xx_timer_microseconds() + ms * 1000u;
        while ((int32_t)(mt68xx_timer_microseconds() - end) < 0) {
        }
    }
}

int mt68xx_timer_hw_ok(void) {
    if (g_src == TIMER_SRC_NONE) mt68xx_timer_init();
    return g_src == TIMER_SRC_ARCH || g_src == TIMER_SRC_GPT4;
}

const char* mt68xx_timer_source_name(void) {
    if (g_src == TIMER_SRC_NONE) mt68xx_timer_init();
    switch (g_src) {
    case TIMER_SRC_ARCH:
        return "arch";
    case TIMER_SRC_GPT4:
        return "gpt4";
    default:
        return "soft";
    }
}
