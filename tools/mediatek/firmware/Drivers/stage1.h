#ifndef MVII_STAGE1_H
#define MVII_STAGE1_H

#include <stdint.h>

/*
 * stage1 builds one 512-byte status record per bring-up step and hands it to a
 * sink; the sink decides where it goes. The standalone LK loader uses the
 * built-in eMMC sink (writes the boot-status sector). The resident MVIIFlash
 * payload installs a USB sink instead, so the host watches stage1 progress live
 * over the same VCOM bridge -- no reboot, and no dependence on eMMC writes
 * actually landing.
 *
 * For plain "flash run -address ..." jumps (no feed protocol) we also blast
 * ASCII progress via the BROM USB put vector + a VCOM poke so the raw bridge
 * shows step results ("log the result ... within jumps") instead of only
 * READYREADYREADY.
 *
 * The sink returns 0 on success; nonzero lets stage1 count persistent failures
 * and eventually stop hammering a dead channel.
 */
typedef int (*stage1_sink_fn)(void* ctx, const uint8_t* sector, uint32_t size, uint32_t sequence);

/* Install the breadcrumb sink. Pass (0, 0) to select the default eMMC sink.
 * Call before mvii_arm_stage1_run(). */
void mvii_arm_stage1_set_sink(stage1_sink_fn sink, void* ctx);

/* Run the full display bring-up, emitting a breadcrumb per step through the
 * active sink, and return when done. Safe to call repeatedly (it resets the
 * per-run breadcrumb state), which is what lets the resident payload re-run it
 * without a reboot. */
void mvii_arm_stage1_run(uint32_t entry_r0, uint32_t entry_r1, uint32_t entry_r2, uint32_t entry_r3);

/* Standalone LK entry point: select the default eMMC sink, run once, then park
 * in wfi (the loader has nothing else to do). */
void mvii_arm_stage1_main(uint32_t entry_r0, uint32_t entry_r1, uint32_t entry_r2, uint32_t entry_r3);

#endif
