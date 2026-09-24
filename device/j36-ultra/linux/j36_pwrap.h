/* SPDX-License-Identifier: GPL-2.0 */
#ifndef J36_PWRAP_H
#define J36_PWRAP_H
#include <linux/io.h>
#include <linux/types.h>

/* Serialized against every J36 client, including stale-result recovery.
 * No charger policy or probe dependency; usable by the early input driver.
 * transfer returns 0/error; update_bits returns 1/0 for changed/unchanged.
 * ro names read-only bits that must be written as zero. */
int j36_pwrap_transfer(void __iomem *base, bool write, u32 adr,
                       u32 wdata, u32 *rdata);
int j36_pwrap_update_bits(void __iomem *base, u32 adr, u32 clr, u32 set, u32 ro);
#endif
