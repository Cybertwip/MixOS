#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/oppo-mt6877/LICENSE for the texts and for what they do not cover.
"""Generate the OPPO MT6877 bring-up DTS from the board extracts.

The SoC addresses, IRQ lines, panel geometry, touch wiring and eMMC caps are
parsed from the committed headers under:
  device/oppo-mt6877/board/

Those are the whole input; --board points at that directory. See
board/PROVENANCE.txt for where each fact came from.

The output is standalone: numeric cells only, no #include, so plain dtc
compiles it without a kernel dt-bindings include path -- the same rule the
J36 generator follows.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


HEADER_FILES = {
    "addrs": "mt6877_addrs.h",
    "panel": "panel_td4330.h",
    "touch": "touch_nt36672.h",
    "msdc": "msdc_mt6877.h",
}

# GIC cells, kept numeric (no arm-gic.h include).
GIC_SPI = 0
IRQ_LEVEL_HIGH = 4
IRQ_EDGE_FALLING = 2

# MT6877 CPU topology, from mt6877.dts: six A55 then two big cores. The vendor
# DTS calls the big cores cortex-a76 (the SoC is sold as A78); the string is
# copied verbatim because that is what the reference boots with.
CPUS = [
    ("arm,cortex-a55", 0x000),
    ("arm,cortex-a55", 0x100),
    ("arm,cortex-a55", 0x200),
    ("arm,cortex-a55", 0x300),
    ("arm,cortex-a55", 0x400),
    ("arm,cortex-a55", 0x500),
    ("arm,cortex-a76", 0x600),
    ("arm,cortex-a76", 0x700),
]


def read_sources(board: Path) -> dict[str, str]:
    sources = {}
    for key, name in HEADER_FILES.items():
        path = board / name
        if not path.is_file():
            raise SystemExit(f"board extract missing: {path}")
        sources[key] = path.read_text()
    return sources


def parse_int(text: str, name: str) -> int:
    m = re.search(r"#define\s+" + re.escape(name) + r"\s+(0x[0-9a-fA-F]+|\d+)", text)
    if not m:
        raise SystemExit(f"board extract has no #{name}")
    return int(m.group(1), 0)


def generate(sources: dict[str, str], device: str, cmdline_extra: str = "") -> str:
    addrs = sources["addrs"]
    panel = sources["panel"]
    touch = sources["touch"]

    gicd = parse_int(addrs, "MT6877_GICD_BASE")
    gicd_size = parse_int(addrs, "MT6877_GICD_SIZE")
    gicr = parse_int(addrs, "MT6877_GICR_BASE")
    gicr_size = parse_int(addrs, "MT6877_GICR_SIZE")
    uart0 = parse_int(addrs, "MT6877_UART0_BASE")
    uart_size = parse_int(addrs, "MT6877_UART_SIZE")
    msdc0 = parse_int(addrs, "MT6877_MSDC0_BASE")
    msdc0_size = parse_int(addrs, "MT6877_MSDC0_SIZE")
    msdc1 = parse_int(addrs, "MT6877_MSDC1_BASE")
    msdc1_size = parse_int(addrs, "MT6877_MSDC1_SIZE")
    spi4 = parse_int(addrs, "MT6877_SPI4_BASE")
    spi_size = parse_int(addrs, "MT6877_SPI_SIZE")
    consys = parse_int(addrs, "MT6877_CONSYS_BASE")
    consys_size = parse_int(addrs, "MT6877_CONSYS_SIZE")
    afe = parse_int(addrs, "MT6877_AFE_BASE")
    afe_size = parse_int(addrs, "MT6877_AFE_SIZE")
    pwrap = parse_int(addrs, "MT6877_PWRAP_BASE")
    pwrap_size = parse_int(addrs, "MT6877_PWRAP_SIZE")
    irq_uart0 = parse_int(addrs, "MT6877_IRQ_UART0")
    irq_msdc0 = parse_int(addrs, "MT6877_IRQ_MSDC0")
    irq_msdc1 = parse_int(addrs, "MT6877_IRQ_MSDC1")
    irq_spi4 = parse_int(addrs, "MT6877_IRQ_SPI4")
    irq_afe = parse_int(addrs, "MT6877_IRQ_AFE")
    fb_base = parse_int(addrs, "MT6877_LK_FB_BASE")
    width = parse_int(panel, "TD4330_WIDTH")
    height = parse_int(panel, "TD4330_HEIGHT")
    touch_irq_gpio = parse_int(touch, "NT36672_IRQ_GPIO")
    touch_hz = parse_int(touch, "NT36672_SPI_MAX_HZ")

    if width != 1080 or height != 2280:
        raise SystemExit(f"unexpected TD4330 geometry {width}x{height}")
    if fb_base == 0:
        raise SystemExit("LK framebuffer base is zero; the handoff node would be garbage")

    stride = width * 4
    cpu_nodes = []
    for idx, (compat, reg) in enumerate(CPUS):
        cpu_nodes.append(
            f"\t\tcpu{idx}: cpu@{reg:x} {{\n"
            f'\t\t\tdevice_type = "cpu";\n'
            f'\t\t\tcompatible = "{compat}";\n'
            f"\t\t\treg = <{reg:#x}>;\n"
            f'\t\t\tenable-method = "psci";\n'
            f"\t\t}};"
        )

    bootargs = (
        "earlycon console=ttyS0,921600n8 root=PARTLABEL=ROOTFS rw rootwait "
        "oppo.audio=1 oppo.wifi=1 oppo.modem=1 oppo.power=1 "
        f"oppo.device={device}"
    )
    if cmdline_extra:
        bootargs += " " + cmdline_extra

    return f"""/dts-v1/;

/ {{
\tmodel = "OPPO {device} (MT6877, MixOS bring-up)";
\tcompatible = "oppo,{device}", "mediatek,mt6877";
\t#address-cells = <2>;
\t#size-cells = <2>;
\tinterrupt-parent = <&gic>;

\tchosen {{
\t\tstdout-path = "serial0:921600n8";
\t\tbootargs = "{bootargs}";
\t}};

\tmemory@40000000 {{
\t\tdevice_type = "memory";
\t\treg = <0x0 0x40000000 0x1 0x0>;
\t}};

\treserved-memory {{
\t\t#address-cells = <2>;
\t\t#size-cells = <2>;
\t\tranges;

\t\tlk_framebuffer: framebuffer@{fb_base:x} {{
\t\t\treg = <0x0 {fb_base:#x} 0x0 0x1000000>;
\t\t\tno-map;
\t\t}};
\t}};

\tcpus {{
\t\t#address-cells = <1>;
\t\t#size-cells = <0>;

{chr(10).join(cpu_nodes)}
\t}};

\tpsci {{
\t\tcompatible = "arm,psci-1.0";
\t\tmethod = "smc";
\t}};

\ttimer {{
\t\tcompatible = "arm,armv8-timer";
\t\tinterrupts = <1 13 0xf08>,
\t\t\t     <1 14 0xf08>,
\t\t\t     <1 11 0xf08>,
\t\t\t     <1 10 0xf08>;
\t}};

\tsoc {{
\t\tcompatible = "simple-bus";
\t\t#address-cells = <2>;
\t\t#size-cells = <2>;
\t\tranges;

\t\tgic: interrupt-controller@{gicd:x} {{
\t\t\tcompatible = "arm,gic-v3";
\t\t\t#interrupt-cells = <3>;
\t\t\t#address-cells = <2>;
\t\t\t#size-cells = <2>;
\t\t\t#redistributor-regions = <1>;
\t\t\tinterrupt-controller;
\t\t\treg = <0x0 {gicd:#x} 0x0 {gicd_size:#x}>,
\t\t\t      <0x0 {gicr:#x} 0x0 {gicr_size:#x}>;
\t\t}};

\t\tuart0: serial@{uart0:x} {{
\t\t\tcompatible = "mediatek,mt6577-uart";
\t\t\treg = <0x0 {uart0:#x} 0x0 {uart_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_uart0} {IRQ_LEVEL_HIGH}>;
\t\t\tstatus = "okay";
\t\t}};

\t\tmsdc0: mmc@{msdc0:x} {{
\t\t\tcompatible = "mediatek,mt6877-mmc", "mediatek,mt6779-mmc";
\t\t\treg = <0x0 {msdc0:#x} 0x0 {msdc0_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_msdc0} {IRQ_LEVEL_HIGH}>;
\t\t\tbus-width = <8>;
\t\t\tmax-frequency = <200000000>;
\t\t\tcap-mmc-highspeed;
\t\t\tmmc-ddr-1_8v;
\t\t\tmmc-hs200-1_8v;
\t\t\tmmc-hs400-1_8v;
\t\t\tno-sd;
\t\t\tno-sdio;
\t\t\tnon-removable;
\t\t\tstatus = "okay";
\t\t}};

\t\tmsdc1: mmc@{msdc1:x} {{
\t\t\tcompatible = "mediatek,mt6877-mmc", "mediatek,mt6779-mmc";
\t\t\treg = <0x0 {msdc1:#x} 0x0 {msdc1_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_msdc1} {IRQ_LEVEL_HIGH}>;
\t\t\tbus-width = <4>;
\t\t\tmax-frequency = <200000000>;
\t\t\tcap-sd-highspeed;
\t\t\tsd-uhs-sdr50;
\t\t\tstatus = "okay";
\t\t}};

\t\tspi4: spi@{spi4:x} {{
\t\t\tcompatible = "mediatek,mt6765-spi";
\t\t\treg = <0x0 {spi4:#x} 0x0 {spi_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_spi4} {IRQ_LEVEL_HIGH}>;
\t\t\t#address-cells = <1>;
\t\t\t#size-cells = <0>;
\t\t\tstatus = "okay";

\t\t\ttouchscreen@0 {{
\t\t\t\tcompatible = "oppo,nt36672";
\t\t\t\treg = <0>;
\t\t\t\tspi-max-frequency = <{touch_hz}>;
\t\t\t\tinterrupt-parent = <&pio>;
\t\t\t\tinterrupts = <{touch_irq_gpio} {IRQ_EDGE_FALLING}>;
\t\t\t\tstatus = "okay";
\t\t\t}};
\t\t}};

\t\tpio: pinctrl {{
\t\t\tcompatible = "mediatek,mt6877-pinctrl";
\t\t\tgpio-controller;
\t\t\t#gpio-cells = <2>;
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <2>;
\t\t\tstatus = "okay";
\t\t}};

\t\tpwrap: pwrap@{pwrap:x} {{
\t\t\tcompatible = "mediatek,mt6877-pwrap";
\t\t\treg = <0x0 {pwrap:#x} 0x0 {pwrap_size:#x}>;
\t\t\tstatus = "okay";
\t\t}};

\t\tafe: audio@{afe:x} {{
\t\t\tcompatible = "oppo,mt6877-afe";
\t\t\treg = <0x0 {afe:#x} 0x0 {afe_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_afe} {IRQ_LEVEL_HIGH}>;
\t\t\tstatus = "okay";
\t\t}};

\t\tconsys: consys@{consys:x} {{
\t\t\tcompatible = "oppo,mt6877-consys";
\t\t\treg = <0x0 {consys:#x} 0x0 {consys_size:#x}>;
\t\t\tstatus = "okay";
\t\t}};

\t\tmdm: modem {{
\t\t\tcompatible = "oppo,mt6877-modem";
\t\t\tstatus = "okay";
\t\t}};

\t\tkeys: gpio-keys {{
\t\t\tcompatible = "gpio-keys";
\t\t\tstatus = "okay";

\t\t\tvol-up {{
\t\t\t\tlabel = "Volume Up";
\t\t\t\tlinux,code = <115>;
\t\t\t\tgpios = <&pio 0 1>;
\t\t\t}};
\t\t\tvol-down {{
\t\t\t\tlabel = "Volume Down";
\t\t\t\tlinux,code = <114>;
\t\t\t\tgpios = <&pio 1 1>;
\t\t\t}};
\t\t}};
\t}};

\tframebuffer0: framebuffer {{
\t\tcompatible = "simple-framebuffer";
\t\treg = <0x0 {fb_base:#x} 0x0 0x1000000>;
\t\twidth = <{width}>;
\t\theight = <{height}>;
\t\tstride = <({stride})>;
\t\tformat = "a8b8g8r8";
\t\tstatus = "okay";
\t}};

\taliases {{
\t\tserial0 = &uart0;
\t}};
}};
"""


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the OPPO MT6877 bring-up DTS")
    parser.add_argument("--board", required=True, help="board/ extract directory")
    parser.add_argument("--device", default="20181", help="OPPO codename (devices.sh)")
    parser.add_argument("--cmdline-extra", default="", help="extra bootargs words")
    parser.add_argument("--out", required=True, help="output .dts path")
    args = parser.parse_args()

    sources = read_sources(Path(args.board))
    dts = generate(sources, args.device, args.cmdline_extra)
    Path(args.out).write_text(dts)
    print(f"wrote {args.out} for OPPO {args.device}")


if __name__ == "__main__":
    main()
