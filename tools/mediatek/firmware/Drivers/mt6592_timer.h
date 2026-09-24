#ifndef MT6592_TIMER_H
#define MT6592_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Free-running monotonic time, in microseconds, from whichever counter this
 * board actually has: the ARMv7 generic timer where firmware programmed it, the
 * MTK APXGPT otherwise, and a coarse software counter if neither ticks, so
 * callers always get a monotonically increasing value. The tick rate is
 * detected, never assumed -- and it is a peripheral clock, unrelated to the CPU
 * frequency. First call initialises the timer. */
uint64_t mt6592_timer_microseconds(void);

/* 1 if a hardware counter is ticking, 0 if the fallback counter is in use. */
int mt6592_timer_hw_ok(void);

/* The detected tick rate in Hz, and a short name for the counter it came from
 * ("armv7-generic", "apxgpt4" or "software"). Reporting these is the only way
 * to tell a genuinely slow system from one whose clock is lying about it. */
uint32_t mt6592_timer_hz(void);
const char* mt6592_timer_source_name(void);

/* Calls to mt6592_timer_microseconds() since boot. Divided into a span measured
 * with that same clock, it gives the per-read cost -- which is the difference
 * between "this code is slow" and "asking what time it is is slow". */
uint64_t mt6592_timer_read_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_TIMER_H */
