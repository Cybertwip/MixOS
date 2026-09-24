/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
/* See mt6592_dbgflag.h for the design and the one-shot safety argument. */

#include "mt6592_dbgflag.h"

#include "mt6592_msdc.h"

enum { SECTOR = 512u };

/* Layout: magic, mode, ~magic. The third word is a cheap integrity check --
 * a sector of 0xff (erased) or 0x00 (zeroed) satisfies neither, so an
 * uninitialised sector reads as "no flag" rather than as garbage that happens
 * to match. */
static uint8_t g_sector[SECTOR] __attribute__((aligned(64)));

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int mt6592_dbgflag_arm(uint32_t mode) {
    for (uint32_t i = 0; i < SECTOR; ++i) g_sector[i] = 0u;
    put32(g_sector + 0, MT6592_DBGFLAG_MAGIC);
    put32(g_sector + 4, mode);
    put32(g_sector + 8, ~MT6592_DBGFLAG_MAGIC);
    return mt6592_emmc_write_user(MT6592_DBGFLAG_OFFSET, g_sector, SECTOR) == MT6592_MSDC_OK
               ? 0
               : -1;
}

uint32_t mt6592_dbgflag_take(void) {
    uint32_t mode;

    if (mt6592_emmc_read_user(MT6592_DBGFLAG_OFFSET, g_sector, SECTOR) != MT6592_MSDC_OK) {
        return MT6592_DBG_MODE_NORMAL;
    }
    if (get32(g_sector + 0) != MT6592_DBGFLAG_MAGIC) return MT6592_DBG_MODE_NORMAL;
    if (get32(g_sector + 8) != (uint32_t)~MT6592_DBGFLAG_MAGIC) return MT6592_DBG_MODE_NORMAL;

    mode = get32(g_sector + 4);
    if (mode == MT6592_DBG_MODE_NORMAL) return MT6592_DBG_MODE_NORMAL;

    /* Clear before returning, never after. The caller is about to run code that
     * may not survive; the flag must already be gone by then. If this write
     * fails we still refuse the request, because a flag we cannot clear is a
     * boot loop. */
    for (uint32_t i = 0; i < SECTOR; ++i) g_sector[i] = 0u;
    if (mt6592_emmc_write_user(MT6592_DBGFLAG_OFFSET, g_sector, SECTOR) != MT6592_MSDC_OK) {
        return MT6592_DBG_MODE_NORMAL;
    }
    return mode;
}
