#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""Shared bring-up DTS emission for the MediaTek phone families (mt67xx,
mt68xx). One function both trees' generators call: the skeleton shape is
family-shared, the facts come per device from devices.sh, and anything not
known is OMITTED -- a missing node fails visibly at bring-up, while an
invented address fails as a mystery brick.

What is emitted (all of it either ARM-standard or spec-sourced):
  model + board/soc compatibles, /chosen bootargs, /memory from the row's
  spec RAM size, one cpu@0 (SMP is a stock-DTB TODO) + PSCI, and a
  simple-framebuffer ONLY when an LK framebuffer base is supplied
  (fb_base 0 disables the node; same rule as the lg-k20plus generator).

MTK_DRAM_BASE_PRIOR is the one prior in here: 0x40000000 is MediaTek's
conventional DRAM base across generations, but it is UNVERIFIED for these
SoCs -- BRINGUP step 2 confirms it from the stock DTB, one line to fix.
"""
from __future__ import annotations

MTK_DRAM_BASE_PRIOR = 0x40000000


def generate_bringup_dts(model: str, board_compat: str, soc_compat: str,
                         mem_mb: int, bootargs: str, fb_base: int = 0,
                         width: int = 0, height: int = 0) -> str:
    if mem_mb <= 0:
        raise SystemExit("memory size must come from the devices.sh row")
    if fb_base and (width <= 0 or height <= 0):
        raise SystemExit("simple-framebuffer needs width+height with fb_base")
    mem_bytes = mem_mb * 1024 * 1024
    out = []
    out.append("/dts-v1/;")
    out.append("")
    out.append(f"\tmodel = \"{model}\";")
    out.append(f"\tcompatible = \"{board_compat}\", \"{soc_compat}\";")
    out.append("\t#address-cells = <2>;")
    out.append("\t#size-cells = <2>;")
    out.append("")
    out.append("\tchosen {")
    out.append(f"\t\tbootargs = \"{bootargs}\";")
    out.append("\t\t/* stdout-path omitted: console UART unknown (BRINGUP step 2). */")
    out.append("\t};")
    out.append("")
    out.append("\tmemory@%x {" % MTK_DRAM_BASE_PRIOR)
    out.append("\t\tdevice_type = \"memory\";")
    out.append("\t\t/* Size from retail specs, base is MTK_DRAM_BASE_PRIOR (BRINGUP step 2). */")
    out.append("\t\treg = <0x0 0x%x 0x%x 0x%x>;" % (
        MTK_DRAM_BASE_PRIOR, (mem_bytes >> 32) & 0xFFFFFFFF, mem_bytes & 0xFFFFFFFF))
    out.append("\t};")
    out.append("")
    out.append("\tpsci {")
    out.append("\t\tcompatible = \"arm,psci-1.0\", \"arm,psci\";")
    out.append("\t\tmethod = \"smc\";")
    out.append("\t};")
    out.append("")
    out.append("\tcpus {")
    out.append("\t\t#address-cells = <1>;")
    out.append("\t\t#size-cells = <0>;")
    out.append("\t\t/* One core: SMP MPIDRs come from the stock DTB (BRINGUP step 2). */")
    out.append("\t\tcpu@0 {")
    out.append("\t\t\tdevice_type = \"cpu\";")
    out.append("\t\t\tcompatible = \"arm,armv8\";")
    out.append("\t\t\treg = <0x0>;")
    out.append("\t\t\tenable-method = \"psci\";")
    out.append("\t\t};")
    out.append("\t};")
    if fb_base:
        out.append("")
        out.append("\tframebuffer@%x {" % fb_base)
        out.append("\t\tcompatible = \"simple-framebuffer\";")
        out.append("\t\treg = <0x0 0x%x 0x0 0x%x>;" % (fb_base, width * height * 4))
        out.append("\t\twidth = <%d>;" % width)
        out.append("\theight = <%d>;" % height)
        out.append("\t\tstride = <(%d * 4)>;" % width)
        out.append("\t\tformat = \"a8r8g8b8\";")
        out.append("\t};")
    else:
        out.append("")
        out.append("\t/* No simple-framebuffer: LK framebuffer base unknown (BRINGUP step 2).")
        out.append("\t * Pass --fb-base once the stock DTB gives it. No soc bus either:")
        out.append("\t * uart/mmc/pmic/panel/touch nodes land with measured addresses. */")
    return "\n".join(out) + "\n"
