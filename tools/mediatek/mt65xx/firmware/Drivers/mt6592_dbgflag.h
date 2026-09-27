/* Copyright (c) 2026 SynChanCyberSecurity LLC. All Rights Reserved. */
#ifndef MT6592_DBGFLAG_H
#define MT6592_DBGFLAG_H

#include <stdint.h>

/*
 * One-shot "come up in debug mode" flag, in eMMC.
 *
 * The BROM payload can only really be trusted to do two things on this board:
 * write eMMC and reset the SoC. So that is all "debug mode" in the BROM menu
 * does -- it drops this flag in a sector and kicks the watchdog. The board then
 * takes its normal path (BROM -> preloader -> our LK), and the LK picks the flag
 * up and starts the live console instead of booting on.
 *
 * WHY eMMC AND NOT A SCRATCH REGISTER
 * MediaTek's own boot-mode channel is WDT_NONRST_REG (0x10007020), which is
 * designed to survive exactly this reset. It is not used here because the stock
 * preloader runs between the reset and our LK, and the stock preloader is a
 * consumer of that register -- it reads the boot mode out of it and is free to
 * clear it. A sector we own cannot be taken away from us by code we did not
 * write.
 *
 * WHY ONE-SHOT
 * mt6592_dbgflag_take() clears the flag before the console it requested has run
 * a single line. That ordering is deliberate and it is the safety property that
 * makes this feature safe to ship: if the console wedges, faults, or simply
 * never enumerates, the next power cycle boots normally. A sticky flag would
 * turn any console bug into an unbootable device that only a BROM reflash could
 * recover, which is precisely the position this whole effort exists to get out
 * of.
 */

enum {
    MT6592_DBG_MODE_NORMAL = 0u,
    MT6592_DBG_MODE_CONSOLE = 1u,

    /* The sector immediately below the 64 KiB console ring, inside the LK
     * slot's padding. The slot is 2 MiB (0x01d40000..0x01f40000), the LK payload
     * is ~31 KiB of it, and the ring owns the top 64 KiB -- so everything from
     * the payload's end up to the ring is unused, and this is the sector that
     * sits right under the ring. */
    MT6592_DBGFLAG_OFFSET = 0x01f2fe00u,
    MT6592_DBGFLAG_MAGIC = 0x47443744u, /* D7DG */
};

/* Write the flag. Returns 0 if the sector landed. */
int mt6592_dbgflag_arm(uint32_t mode);

/* Read the flag and clear it in the same call. Returns the mode that was
 * armed, or MT6592_DBG_MODE_NORMAL if none was (including every failure path:
 * an unreadable sector must never mean "enter the console"). */
uint32_t mt6592_dbgflag_take(void);

#endif /* MT6592_DBGFLAG_H */
