/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* Run: cc -std=c99 -Wall -Wextra -Werror external-power.c -o /tmp/j36-power-test
 *      /tmp/j36-power-test
 * Models the charger watchdog fields, including self-clearing strobes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../linux/j36_external_power.h"

struct pmic {
    unsigned int reg[0x30];
    unsigned int calls, fail_at, ignore_reg, flag_writes;
};

static int read_reg(void *ctx, unsigned int reg, unsigned int *value)
{
    struct pmic *p = ctx;
    assert(reg < 0x30);
    if (++p->calls == p->fail_at)
        return -7;
    *value = p->reg[reg];
    return 0;
}

static int write_reg(void *ctx, unsigned int reg, unsigned int value)
{
    struct pmic *p = ctx;
    /* No writes to charge enable, current, voltage or any other bank. */
    assert(reg == 0x1a || reg == 0x1e || reg == 0x20);
    if (++p->calls == p->fail_at)
        return -7;
    if (reg == p->ignore_reg)
        return 0; /* Wrapper ACK does not guarantee the PMIC took the write. */
    if (reg == 0x1a) {
        assert(!(value & 0x110)); /* Never enable or kick the timer. */
        assert((value & ~0x110u) == (p->reg[reg] & ~0x110u));
    } else if (reg == 0x1e) {
        assert(!(value & 5)); /* IRQ off; no writeback of read-only OUT. */
        assert((value & ~7u) == (p->reg[reg] & ~7u));
        value |= p->reg[reg] & 4; /* OUT is read-only. */
        if (value & 2) {
            ++p->flag_writes;
            value &= ~6u; /* FLAG_WR self clears and clears OUT. */
        }
    } else {
        assert((value & ~3u) == (p->reg[reg] & ~3u));
    }
    p->reg[reg] = value;
    return 0;
}

static struct pmic inherited(unsigned int mode)
{
    struct pmic p;
    unsigned int i;
    memset(&p, 0, sizeof(p));
    for (i = 0; i < 0x30; ++i)
        p.reg[i] = 0xa500 | i;
    p.reg[0] |= 0x18; /* CHR_EN and CSDAC_EN already supplied by preloader. */
    p.reg[0x1a] = 0xa11b; /* Armed, pending WR, nonzero TD and reserved bits. */
    p.reg[0x1e] = 0xa005; /* IRQ enabled, expired. */
    p.reg[0x20] = 0xa003 | mode; /* All USBDL combinations must survive. */
    p.ignore_reg = ~0u;
    return p;
}

static int hold(struct pmic *p)
{
    return j36_external_power_hold(p, read_reg, write_reg);
}

int main(void)
{
    unsigned int mode, i, total;
    struct pmic p = inherited(8);
    assert(hold(&p) == 0);
    total = p.calls;

    for (mode = 0; mode <= 12; mode += 4) {
        struct pmic before = inherited(mode);
        p = before;
        assert(hold(&p) == 0);
        assert(!(p.reg[0x1a] & 0x110));
        assert(!(p.reg[0x1e] & 5));
        assert(p.reg[0x20] == (before.reg[0x20] & ~3u));
        for (i = 0; i < 0x30; ++i)
            if (i != 0x1a && i != 0x1e && i != 0x20)
                assert(p.reg[i] == before.reg[i]);

        /* A long handoff must not let an armed timer clear charger enable. */
        for (i = 0; i < 60; ++i)
            if (i >= 4 && (p.reg[0x1a] & 0x10))
                p.reg[0] &= ~0x10u;
        assert(p.reg[0] == before.reg[0]);
        i = p.flag_writes;
        assert(hold(&p) == 0);
        assert(p.flag_writes > i); /* Repeated service must still clear OUT. */
    }

    /* Propagate every transport error, and recover on a later service call. */
    for (i = 1; i <= total; ++i) {
        p = inherited(8);
        p.fail_at = i;
        assert(hold(&p) == -7);
        assert(p.calls == i);
        p.fail_at = 0;
        assert(hold(&p) == 0);
    }
    for (i = 0x1a; i <= 0x20; i += 2) {
        if (i == 0x1c)
            continue;
        p = inherited(8);
        p.ignore_reg = i;
        assert(hold(&p) != 0);
        p.ignore_reg = ~0u;
        assert(hold(&p) == 0);
    }
    puts("external power: preservation, handoff, strobes, failures and retry passed");
    return 0;
}
