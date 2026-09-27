/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* mt68xx watchdog: one write. [mt6735] platform.c:70-71 disables the WDT by
 * writing 0x22000000 to TOPRGU_BASE; this tree parks with a heartbeat and
 * never kicks, so that write runs before anything else that could stall.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mt68xx_wdt_disable(void);

#ifdef __cplusplus
}
#endif
