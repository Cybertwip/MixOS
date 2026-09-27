/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
#include "mt68xx_wdt.h"

#include "mt68xx_facts.h"

void mt68xx_wdt_disable(void) {
#if MT68XX_HAS_WDT
    *(volatile uint32_t*)(uintptr_t)MT68XX_TOPRGU_BASE = MT68XX_WDT_DISABLE_KEY;
#else
    /* Compiled out: the TOPRGU base is ungrounded on this family, and a
     * blind write to a reset-adjacent register is worse than the
     * diagnosable reset loop it avoids (see the facts header). */
#endif
}
