#ifndef MT6592_MSDC_SD_H
#define MT6592_MSDC_SD_H

/*
 * MT6592 MSDC1 removable microSD driver (J36 Ultra).
 *
 * Independent of the MSDC0 eMMC driver in mt6592_msdc.c: separate controller
 * base (0x11240000, confirmed by the stock LK msdc base table
 * {0x11230000, 0x11240000, 0x11250000, 0x11260000}), separate clock gate and
 * pinmux, and the SD init sequence (CMD0/CMD8/ACMD41/CMD2/CMD3, 4-bit bus)
 * instead of the eMMC CMD1 path.
 *
 * Hot-plug contract: mt6592_sd_probe() is cheap when no card is inserted
 * (bounded, hardware CMD-timeout driven) so mvii_storage_rescan() can call it
 * every rescan tick. A card that stops responding is forgotten and re-probed
 * on the next tick.
 *
 * Error codes are shared with mt6592_msdc.h (MT6592_MSDC_*).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe / (re)initialise the card behind MSDC1. Returns MT6592_MSDC_OK when a
 * card is ready. Fast-fails (no card / dead card) so it can poll hot-plug. */
int mt6592_sd_probe(void);

/* Non-probing state queries. */
int mt6592_sd_present(void);
uint64_t mt6592_sd_capacity_sectors(void);

/* 512-byte sector I/O. On transport failure the card is marked absent so the
 * next probe re-enumerates it (covers surprise removal). */
int mt6592_sd_read(uint64_t lba, uint32_t nsectors, void* buffer);
int mt6592_sd_write(uint64_t lba, uint32_t nsectors, const void* buffer);

/* Drop cached card state; next probe runs the full init again. */
void mt6592_sd_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* MT6592_MSDC_SD_H */
