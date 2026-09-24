#!/usr/bin/env python3
"""Compile the real shared transport against a concurrent fake WACS2 device."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "linux"
stubs = {
    "types.h": "#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n"
               "#include <stddef.h>\ntypedef uint32_t u32;\n#define __iomem\n",
    "errno.h": "#include <errno.h>\n",
    "module.h": "#define MODULE_DESCRIPTION(x)\n#define MODULE_LICENSE(x)\n"
                "#define EXPORT_SYMBOL_GPL(x)\n",
    "io.h": "#pragma once\n#include <linux/types.h>\n#include <sched.h>\n"
            "u32 readl(const void *p);\nvoid writel(u32 v, void *p);\n"
            "#define cpu_relax() sched_yield()\n",
    "spinlock.h": "#include <pthread.h>\n"
                  "#define DEFINE_SPINLOCK(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER\n"
                  "#define spin_lock_irqsave(l,f) do { (f)=0; pthread_mutex_lock(l); } while (0)\n"
                  "#define spin_unlock_irqrestore(l,f) do { (void)(f); pthread_mutex_unlock(l); } while (0)\n",
}
harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "j36_pwrap.c"

static unsigned char mmio[0x100];
static u32 regs[32768], state, result;
static unsigned int commands, clears;
static bool wedged;

u32 readl(const void *p)
{
    assert((const unsigned char *)p - mmio == 0xa0);
    sched_yield(); /* Invite a competing driver between every bus operation. */
    return (wedged ? 2u : state) << 16 | result;
}

void writel(u32 v, void *p)
{
    ptrdiff_t off = (unsigned char *)p - mmio;
    if (off == 0xa4) {
        assert(v == 1);
        state = 0;
        result = 0;
        ++clears;
    } else {
        u32 addr = (v >> 16) & 0x7fff;
        assert(off == 0x9c);
        assert(state == 0);
        ++commands;
        if (v >> 31) {
            regs[addr] = v & 0xffff;
        } else {
            result = regs[addr];
            state = 6;
        }
    }
    sched_yield();
}

static void *client(void *arg)
{
    unsigned int id = (unsigned int)(uintptr_t)arg, i;
    u32 value, mask = 1u << id;
    for (i = 0; i < 2000; ++i) {
        assert(j36_pwrap_transfer(mmio, false, 0x100 + id * 2, 0, &value) == 0);
        assert(value == 0xa100 + id); /* Never collect a different client's read. */
        assert(j36_pwrap_update_bits(mmio, 0x200, mask, (i & 1) ? mask : 0, 0) >= 0);
        assert(j36_pwrap_transfer(mmio, false, 0x200, 0, &value) == 0);
        assert((value & mask) == ((i & 1) ? mask : 0));
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    unsigned int i, before;
    u32 value;
    regs[0x80] = 0xa100;
    state = 6; result = 0xdead; /* Orphaned bootloader read. */
    assert(j36_pwrap_transfer(mmio, false, 0x100, 0, &value) == 0);
    assert(value == 0xa100 && clears == 2);
    before = commands;
    assert(j36_pwrap_transfer(mmio, false, 1, 0, &value) == -EINVAL);
    assert(j36_pwrap_transfer(mmio, false, 0, 0, NULL) == -EINVAL);
    assert(j36_pwrap_transfer(mmio, true, 0, 0x10000, NULL) == -EINVAL);
    assert(commands == before);
    wedged = true;
    assert(j36_pwrap_transfer(mmio, false, 0x100, 0, &value) == -ETIMEDOUT);
    assert(commands == before);
    wedged = false;
    assert(j36_pwrap_transfer(mmio, false, 0x100, 0, &value) == 0);
    regs[0x100] = 0xff00;
    assert(j36_pwrap_update_bits(mmio, 0x200, 0xf, 5, 0x8000) == 1);
    assert(regs[0x100] == 0x7f05); /* Preserve unrelated bits, mask RO writeback. */
    assert(j36_pwrap_update_bits(mmio, 0x200, 0xf, 5, 0x8000) == 0);
    regs[0x100] = 0;
    for (i = 0; i < 4; ++i) {
        regs[0x80 + i] = 0xa100 + i;
        assert(pthread_create(&threads[i], NULL, client, (void *)(uintptr_t)i) == 0);
    }
    for (i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(regs[0x100] == 15); /* No lost read-modify-write from other clients. */
    puts("PWRAP: concurrent reads/updates, stale recovery, timeout and validation passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="j36-pwrap-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "linux").mkdir()
    for name, contents in stubs.items():
        (tmp / "linux" / name).write_text(contents)
    (tmp / "test.c").write_text(harness)
    binary = tmp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Wextra",
                    "-Werror", "-pthread", f"-I{tmp}", f"-I{source}",
                    str(tmp / "test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
