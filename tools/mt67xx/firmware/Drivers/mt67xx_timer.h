/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt67xx menu clock. Same contract as the j36 timer: callers always get a
 * monotonically increasing microsecond count. Three sources, tried in
 * order at init, first one that ticks wins:
 *
 *   arch   The ARM generic timer (CNTPCT/CNTFRQ). No SoC facts at all --
 *          the preloader programs CNTFRQ and the counter runs. Preferred
 *          everywhere, and the only source the mt68xx tree trusts blind.
 *   gpt4   APXGPT GPT4 free-run at 13 MHz ([mt6735] mt_gpt.c pattern).
 *          Needs the prior bases, so it is second, not first.
 *   soft   A per-call counter plus an uncalibrated spin. Monotonic, wrong
 *          rate, loudly reported -- delays still terminate, timestamps
 *          still order, and the banner says not to trust either.
 *
 * All math is 32-bit deltas (cortex-a7 carries hardware UDIV, so no
 * compiler-rt is linked). The horizon is a 32-bit counter wrap -- ~165 s
 * at 26 MHz -- and the menu window is 5 s, so the wrap is documented, not
 * handled. A caller that needs absolute time past the wrap is a caller
 * that has outgrown this file.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mt67xx_timer_init(void);

/* Microseconds since init. */
uint32_t mt67xx_timer_microseconds(void);

/* Spin approximately ms milliseconds. Approximate under "soft" (see
 * above); exact everywhere else. */
void mt67xx_timer_mdelay(uint32_t ms);

/* 1 if a hardware counter is ticking, 0 if the software fallback won. */
int mt67xx_timer_hw_ok(void);

/* The winning source: "arch", "gpt4" or "soft". The banner prints it. */
const char* mt67xx_timer_source_name(void);

#ifdef __cplusplus
}
#endif
