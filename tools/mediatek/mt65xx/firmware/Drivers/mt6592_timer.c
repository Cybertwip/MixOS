/*
 * mt6592_timer.c — monotonic clock, with the tick rate discovered at runtime.
 *
 * The full-OS build used to feed minos_qemu_monotonic_microseconds() from a
 * counter that advanced 16.6 ms per *call*, which made wall-clock time, USB
 * hot-plug rescan pacing and engine frame pacing meaningless on hardware.
 *
 * Nothing here is allowed to assume a tick rate, because a wrong one is worse
 * than no clock at all: every frame budget, scheduler deadline and profiling
 * number in the system is denominated in these microseconds, so a rate that is
 * off by a factor of N makes the whole OS look N times slower or faster than it
 * is and sends the next person tuning the wrong thing. In particular the tick
 * rate has nothing to do with the CPU frequency -- the core can be at 2 GHz
 * while these counters run at 13 MHz off the system clock, because they are fed
 * from the peripheral clock tree, not the CPU PLL.
 *
 * Sources, in preference order:
 *
 *   1. The ARMv7 generic timer. Cortex-A7 implements it, it is 64-bit so it
 *      never wraps, and it states its own frequency in CNTFRQ -- which is
 *      exactly what is wanted here. Trusted only when CNTFRQ is plausible and
 *      CNTPCT is observably moving, since CNTFRQ is firmware-programmed and
 *      reads as garbage if the preloader skipped it.
 *
 *   2. GPT4, the classic MTK free-running counter; the stock preloader/LK
 *      already start it on this family, and if it is not ticking we start it
 *      ourselves (CON=EN|FREERUN, CLK=13 MHz src, div 1 — the same programming
 *      LK's mt_gpt driver uses). Its rate is then read back out of GPT4_CLK:
 *      the source bit picks 13 MHz system clock or 32.768 kHz RTC, and the
 *      divider field divides it. APXGPT_BASE 0x10008000 is the MT6592
 *      generation address (mt6735+ moved it to 0x10004000; both are probed,
 *      ticking wins). GPT4 is 32-bit, so mt6592_timer_microseconds() extends it
 *      in software and must be called at least once per wrap window -- 330 s at
 *      13 MHz, and the render loop calls it hundreds of times per second.
 *
 *   3. A coarse software counter, so callers always get something monotonic.
 *
 * This is one of the hottest functions in the system: the Virtua scheduler asks
 * for the time on every step of its burst, and each step also pays for it inside
 * hasRunnableProcess() and wakeSleepingProcesses(). It therefore must not
 * divide. The obvious "elapsed_ticks * 1000000 / hz" costs a call to
 * __aeabi_uldivmod -- a bit-serial 64-bit division, ~150 cycles -- on top of the
 * Strongly-Ordered APB read, which is what made a 4096-step scheduler burst
 * spend most of its budget reading the clock instead of running the guest.
 * Instead the microseconds-per-tick reciprocal is computed once, at detection
 * time, and each delta is converted with a single multiply; the sub-microsecond
 * remainder is carried between calls so nothing is rounded away.
 */

#include "mt6592_timer.h"

enum {
    APXGPT_BASE_MT6592 = 0x10008000u,
    APXGPT_BASE_ALT = 0x10004000u,

    GPT4_CON = 0x40u,
    GPT4_CLK = 0x44u,
    GPT4_DAT = 0x48u,

    GPT_CON_EN = 1u << 0,
    GPT_CON_CLR = 1u << 1,
    GPT_CON_FREERUN = 3u << 4,

    GPT_CLK_13M_DIV1 = 0u, /* clk src = system 13 MHz, divide by 1 */

    /* GPT4_CLK: bit 4 selects the source, bits 3:0 the divider. */
    GPT_CLK_SRC_RTC = 1u << 4,
    GPT_CLK_DIV_MASK = 0x0fu,

    GPT_SRC_SYS_HZ = 13000000u, 
    GPT_SRC_RTC_HZ = 32768u,

    /* What the generic timer is allowed to claim before we stop believing it.
     * Real parts sit between 1 and 100 MHz; 0 (never programmed) and wild
     * values are the failure modes actually seen in the field. */
    ARCH_TIMER_MIN_HZ = 1000000u,
    ARCH_TIMER_MAX_HZ = 100000000u,
};

/* MTK's divider field is an index, not the divisor itself. */
static const uint8_t k_gpt_clk_dividers[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 32, 64};

typedef enum {
    TIMER_SOURCE_NONE = 0,
    TIMER_SOURCE_ARCH,
    TIMER_SOURCE_GPT,
    TIMER_SOURCE_SOFTWARE,
} timer_source_t;

static timer_source_t g_source;
static uint32_t g_base;          /* GPT block address; 0 = not yet probed */
static uint32_t g_hz;            /* detected tick rate of whichever source won */
static uint64_t g_us_per_tick_q24; /* 1e6 * 2^24 / g_hz, so no divide per call */
static uint64_t g_frac_q24;      /* sub-microsecond carry */
static uint64_t g_last_ticks;
static uint32_t g_stagnant_reads;
static uint64_t g_acc_us;        /* microseconds accumulated since the first read */
static uint64_t g_fallback;      /* software fallback when nothing ticks */
static uint64_t g_last_us;
/* Every call, counted. The clock is read from the engine loop, the scheduler,
 * the rasterizer's work accounting and every yield path, and nothing has ever
 * said how many of those add up per frame. A GPT read is an APB access plus a
 * 64-bit multiply, so it is cheap but not free, and at a few thousand a frame
 * "cheap" stops being the point. This is the denominator that turns an
 * unexplained span into microseconds per read. */
static uint64_t g_reads;

static uint32_t rd(uint32_t base, uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(base + off);
}

static void wr(uint32_t base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)(base + off) = value;
}

/* ---- ARMv7 generic timer ------------------------------------------------- */

/* ID_PFR1[19:16] is 1 when the core implements the Generic Timer. Asked before
 * anything in the c14 space is touched: ID registers are always readable at
 * PL1, whereas reading CNTPCT on a part that has no timer is an undefined
 * instruction, and taking that trap during early boot would look like a dead
 * board rather than a missing feature. */
static int arch_timer_present(void) {
    uint32_t pfr1;
    __asm__ volatile("mrc p15, 0, %0, c0, c1, 1" : "=r"(pfr1));
    return ((pfr1 >> 16) & 0xfu) != 0u;
}

static uint32_t arch_timer_frequency(void) {
    uint32_t hz;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(hz));
    return hz;
}

static uint64_t arch_timer_count(void) {
    uint32_t lo, hi;
    /* CNTPCT is read as a pair; the architecture guarantees the two halves come
     * from one atomic sample, so no hi/lo/hi retry loop is needed. */
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int arch_timer_usable(void) {
    uint32_t hz;
    uint64_t a, b;
    if (!arch_timer_present()) return 0;
    hz = arch_timer_frequency();
    if (hz < ARCH_TIMER_MIN_HZ || hz > ARCH_TIMER_MAX_HZ) return 0;
    /* A plausible CNTFRQ still does not mean the counter was ungated. */
    a = arch_timer_count();
    for (volatile uint32_t spin = 0; spin < 2000u; ++spin) {
    }
    b = arch_timer_count();
    return b != a;
}

/* ---- MTK APXGPT --------------------------------------------------------- */

static int gpt_ticks(uint32_t base) {
    uint32_t a = rd(base, GPT4_DAT);
    for (volatile uint32_t spin = 0; spin < 2000u; ++spin) {
    }
    return rd(base, GPT4_DAT) != a;
}

static int gpt_bring_up(uint32_t base) {
    if (gpt_ticks(base)) return 1;
    /* Not ticking: program it the way LK's mt_gpt does. */
    wr(base, GPT4_CON, 0u);
    wr(base, GPT4_CON, GPT_CON_CLR);
    wr(base, GPT4_CLK, GPT_CLK_13M_DIV1);
    wr(base, GPT4_CON, GPT_CON_EN | GPT_CON_FREERUN);
    return gpt_ticks(base);
}

/* The rate LK left the block programmed at, read back rather than assumed --
 * the preloader is free to have picked the RTC source or a divider, and a boot
 * that did would otherwise report time off by a factor of 400. */
static uint32_t gpt_detected_hz(uint32_t base) {
    const uint32_t clk = rd(base, GPT4_CLK);
    const uint32_t src_hz = (clk & GPT_CLK_SRC_RTC) ? GPT_SRC_RTC_HZ : GPT_SRC_SYS_HZ;
    const uint32_t divider = k_gpt_clk_dividers[clk & GPT_CLK_DIV_MASK];
    return src_hz / divider;
}

/* ---- rate plumbing ------------------------------------------------------- */

static void set_rate(uint32_t hz) {
    g_hz = hz;
    /* One 64-bit divide, once. Q24 keeps the per-call product inside 64 bits for
     * any 32-bit delta even at the 32 kHz RTC rate, and the carry below means
     * the truncation here costs well under a millisecond an hour. */
    g_us_per_tick_q24 = ((uint64_t)1000000u << 24) / hz;
}

static void advance(uint64_t delta_ticks) {
    while (delta_ticks != 0u) {
        /* Chunked so a long gap between calls cannot overflow the multiply;
         * in the steady state this runs exactly once. */
        const uint64_t chunk = delta_ticks > 0xffffffffull ? 0xffffffffull : delta_ticks;
        const uint64_t scaled = chunk * g_us_per_tick_q24 + g_frac_q24;
        g_acc_us += scaled >> 24;
        g_frac_q24 = scaled & 0xffffffull;
        delta_ticks -= chunk;
    }
    if (g_acc_us > g_last_us) g_last_us = g_acc_us;
}

static void timer_init(void) {
    if (g_source != TIMER_SOURCE_NONE) return;

    if (arch_timer_usable()) {
        g_source = TIMER_SOURCE_ARCH;
        set_rate(arch_timer_frequency());
        g_last_ticks = arch_timer_count();
        return;
    }

    if (gpt_bring_up(APXGPT_BASE_MT6592)) {
        g_base = APXGPT_BASE_MT6592;
    } else if (gpt_bring_up(APXGPT_BASE_ALT)) {
        g_base = APXGPT_BASE_ALT;
    } else {
        g_source = TIMER_SOURCE_SOFTWARE;
        return;
    }
    g_source = TIMER_SOURCE_GPT;
    set_rate(gpt_detected_hz(g_base));
    g_last_ticks = rd(g_base, GPT4_DAT);
}

uint64_t mt6592_timer_microseconds(void) {
    ++g_reads;
    if (g_source == TIMER_SOURCE_NONE) timer_init();

    if (g_source == TIMER_SOURCE_SOFTWARE) {
        /* Last resort: at least strictly monotonic. */
        g_fallback += 1000ull;
        if (g_fallback <= g_last_us) g_fallback = g_last_us + 1000ull;
        g_last_us = g_fallback;
        return g_fallback;
    }

    if (g_source == TIMER_SOURCE_ARCH) {
        const uint64_t now = arch_timer_count();
        const uint64_t delta = now - g_last_ticks;
        if (delta == 0u) return g_last_us;
        g_last_ticks = now;
        advance(delta);
        return g_last_us;
    }

    {
        const uint32_t lo = rd(g_base, GPT4_DAT);
        /* Unsigned wraparound makes this the true elapsed tick count across the
         * counter's 32-bit rollover, so no separate wrap accumulator is needed. */
        const uint32_t delta = lo - (uint32_t)g_last_ticks;
        if (delta == 0u) {
            if (++g_stagnant_reads > 4096u) {
                g_source = TIMER_SOURCE_SOFTWARE;
                g_fallback = g_last_us;
                return mt6592_timer_microseconds();
            }
            return g_last_us;
        }
        g_stagnant_reads = 0;
        g_last_ticks = lo;
        advance(delta);
    }
    return g_last_us;
}

int mt6592_timer_hw_ok(void) {
    if (g_source == TIMER_SOURCE_NONE) timer_init();
    return g_source == TIMER_SOURCE_ARCH || g_source == TIMER_SOURCE_GPT;
}

uint32_t mt6592_timer_hz(void) {
    if (g_source == TIMER_SOURCE_NONE) timer_init();
    return g_hz;
}

uint64_t mt6592_timer_read_count(void) {
    return g_reads;
}

const char* mt6592_timer_source_name(void) {
    if (g_source == TIMER_SOURCE_NONE) timer_init();
    switch (g_source) {
    case TIMER_SOURCE_ARCH:
        return "armv7-generic";
    case TIMER_SOURCE_GPT:
        return "apxgpt4";
    default:
        return "software";
    }
}
