/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
#include "mt67xx_wdt.h"

#include "mt67xx_facts.h"

void mt67xx_wdt_disable(void) {
    *(volatile uint32_t*)(uintptr_t)MT67XX_TOPRGU_BASE = MT67XX_WDT_DISABLE_KEY;
}
