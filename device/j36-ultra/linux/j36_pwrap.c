// SPDX-License-Identifier: GPL-2.0
/* One WACS2 owner for all J36 drivers. Recovery, command submission and result
 * collection must share a lock: separate driver locks can clear another
 * driver's valid flag or mistake its register data for their own. */
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include "j36_pwrap.h"

#define J36_PWRAP_WACS2_CMD       0x009c
#define J36_PWRAP_WACS2_RDATA     0x00a0
#define J36_PWRAP_WACS2_VLDCLR    0x00a4
#define J36_PWRAP_FSM_IDLE        0
#define J36_PWRAP_FSM_WFVLDCLR    6
#define J36_PWRAP_POLL_LIMIT      10000

/* The J36 has one physical wrapper, even when clients map it separately. */
static DEFINE_SPINLOCK(j36_wacs2_lock);

static int j36_wacs2_xfer_locked(void __iomem *base, bool write, u32 adr,
                               u32 wdata, u32 *rdata)
{
	unsigned int i;
	u32 value;

	if ((adr & ~0xfffeu) || (wdata & ~0xffffu))
		return -EINVAL;
	if (!write && !rdata)
		return -EINVAL;

	value = readl(base + J36_PWRAP_WACS2_RDATA);
	if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR)
		writel(1, base + J36_PWRAP_WACS2_VLDCLR);

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(base + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_IDLE)
			break;
		cpu_relax();
	}
	if (i == J36_PWRAP_POLL_LIMIT)
		return -ETIMEDOUT;

	writel(((u32)write << 31) | ((adr >> 1) << 16) | wdata,
	       base + J36_PWRAP_WACS2_CMD);
	if (write)
		return 0;

	for (i = 0; i < J36_PWRAP_POLL_LIMIT; ++i) {
		value = readl(base + J36_PWRAP_WACS2_RDATA);
		if (((value >> 16) & 0x7) == J36_PWRAP_FSM_WFVLDCLR) {
			*rdata = value & 0xffff;
			writel(1, base + J36_PWRAP_WACS2_VLDCLR);
			return 0;
		}
		cpu_relax();
	}
	return -ETIMEDOUT;
}

int j36_pwrap_transfer(void __iomem *base, bool write, u32 adr,
                       u32 wdata, u32 *rdata)
{
    unsigned long flags;
    int ret;

    spin_lock_irqsave(&j36_wacs2_lock, flags);
    ret = j36_wacs2_xfer_locked(base, write, adr, wdata, rdata);
    spin_unlock_irqrestore(&j36_wacs2_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(j36_pwrap_transfer);

int j36_pwrap_update_bits(void __iomem *base, u32 adr, u32 clr, u32 set, u32 ro)
{
    unsigned long flags;
    u32 old, next;
    int ret;

    spin_lock_irqsave(&j36_wacs2_lock, flags);
    ret = j36_wacs2_xfer_locked(base, false, adr, 0, &old);
    if (ret)
        goto out;
    next = ((old & ~clr) | set) & ~ro;
    if (next == (old & ~ro)) {
        ret = 0;
        goto out;
    }
    ret = j36_wacs2_xfer_locked(base, true, adr, next, NULL);
    if (!ret)
        ret = 1;
out:
    spin_unlock_irqrestore(&j36_wacs2_lock, flags);
    return ret;
}
EXPORT_SYMBOL_GPL(j36_pwrap_update_bits);

MODULE_DESCRIPTION("J36 shared PMIC wrapper transaction transport");
MODULE_LICENSE("GPL");
