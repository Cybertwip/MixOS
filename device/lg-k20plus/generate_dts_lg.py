#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/lg-k20plus/LICENSE for the texts and for what they do not cover.
"""Generate the LG K20 Plus (MSM8917) bring-up DTS from the board extracts.

Inputs, all committed under device/lg-k20plus/board/ (--board):
  msm8917_addrs.h  SoC addresses, IRQs, carveouts
  panel_lg4894.h   LGD panel geometry (asserted)
  panel_td4100.h   Tovis panel geometry (asserted when --panel td4100)
  touch_lg4894.h   touch geometry (asserted)
  keys_lv517.h     key wiring (--rev picks the GPIO91 code)

The output is standalone (numeric cells, no includes) like the OPPO
generator. Two deliberate v1 choices are baked in and commented at the
nodes: clocks are fixed-clock stand-ins for the ones the LK leaves running
(no mainline gcc-msm8917 yet), and the LPASS/modem/wcnss controller nodes
are present but disabled until their power/clock dependencies land.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


HEADER_FILES = {
    "addrs": "msm8917_addrs.h",
    "lg4894": "panel_lg4894.h",
    "td4100": "panel_td4100.h",
    "touch": "touch_lg4894.h",
    "keys": "keys_lv517.h",
}

GIC_SPI = 0
IRQ_LEVEL_HIGH = 4
IRQ_EDGE_RISING = 1
IRQ_EDGE_FALLING = 2

# MSM8917: 4x Cortex-A53, PSCI (msm8917-cpu.dtsi).
CPUS = [
    ("arm,cortex-a53", 0x100),
    ("arm,cortex-a53", 0x101),
    ("arm,cortex-a53", 0x102),
    ("arm,cortex-a53", 0x103),
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


def generate(sources: dict[str, str], device: str, panel: str, rev: str,
             lk_fb_base: int) -> str:
    addrs = sources["addrs"]
    p = sources["lg4894"] if panel == "lg4894" else sources["td4100"]
    touch = sources["touch"]
    keys = sources["keys"]

    gic = parse_int(addrs, "MSM8917_GIC_BASE")
    gic_cpu = parse_int(addrs, "MSM8917_GIC_CPU_BASE")
    uart = parse_int(addrs, "MSM8917_UART_BASE")
    uart_size = parse_int(addrs, "MSM8917_UART_SIZE")
    irq_uart = parse_int(addrs, "MSM8917_IRQ_UART")
    sdc1_hc = parse_int(addrs, "MSM8917_SDC1_HC_BASE")
    sdc1_core = parse_int(addrs, "MSM8917_SDC1_CORE_BASE")
    irq_sdc1 = parse_int(addrs, "MSM8917_IRQ_SDC1")
    irq_sdc1_pwr = parse_int(addrs, "MSM8917_IRQ_SDC1_PWR")
    sdc2_hc = parse_int(addrs, "MSM8917_SDC2_HC_BASE")
    sdc2_core = parse_int(addrs, "MSM8917_SDC2_CORE_BASE")
    irq_sdc2 = parse_int(addrs, "MSM8917_IRQ_SDC2")
    irq_sdc2_pwr = parse_int(addrs, "MSM8917_IRQ_SDC2_PWR")
    i2c3 = parse_int(addrs, "MSM8917_I2C3_BASE")
    i2c_size = parse_int(addrs, "MSM8917_I2C_SIZE")
    irq_i2c3 = parse_int(addrs, "MSM8917_IRQ_I2C3")
    tlmm = parse_int(addrs, "MSM8917_TLMM_BASE")
    tlmm_size = parse_int(addrs, "MSM8917_TLMM_SIZE")
    spmi = parse_int(addrs, "MSM8917_SPMI_BASE")
    spmi_size = parse_int(addrs, "MSM8917_SPMI_SIZE")
    pronto = parse_int(addrs, "MSM8917_PRONTO_BASE")
    pronto_size = parse_int(addrs, "MSM8917_PRONTO_SIZE")
    touch_rst = parse_int(addrs, "MSM8917_TOUCH_RESET_GPIO")
    touch_irq = parse_int(addrs, "MSM8917_TOUCH_IRQ_GPIO")
    addr_4894 = parse_int(addrs, "MSM8917_TOUCH_LG4894_ADDR")
    addr_td4100 = parse_int(addrs, "MSM8917_TOUCH_TD4100_ADDR")

    if panel == "lg4894":
        width = parse_int(p, "LG4894_WIDTH")
        height = parse_int(p, "LG4894_HEIGHT")
    else:
        width = parse_int(p, "TD4100_WIDTH")
        height = parse_int(p, "TD4100_HEIGHT")
    if (width, height) != (720, 1280):
        raise SystemExit(f"unexpected {panel} geometry {width}x{height}")

    key_gpio = parse_int(keys, "LV517_KEY_GPIO")
    hall_gpio = parse_int(keys, "LV517_HALL_GPIO")
    hall_code = parse_int(keys, "LV517_HALL_CODE")
    # rev-0 remaps the GPIO rocker to VOL_DOWN (PMIC takes VOL_UP).
    # The PMIC RESIN key always takes the other slot of the rocker.
    if rev == "0":
        key_code = parse_int(keys, "LV517_KEY_VOL_DOWN")
        pmic_code = parse_int(keys, "LV517_KEY_VOL_UP")
    else:
        key_code = parse_int(keys, "LV517_KEY_VOL_UP")
        pmic_code = parse_int(keys, "LV517_KEY_VOL_DOWN")

    touch_addr = addr_td4100 if panel == "td4100" else addr_4894
    touch_compat = "synaptics,rmi4-i2c" if panel == "td4100" else "lge,lg4894"

    fb_enabled = "okay" if lk_fb_base else "disabled"
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
        "earlycon console=ttyMSM0,115200n8 root=PARTLABEL=ROOTFS rw rootwait "
        "lg.audio=1 lg.wifi=1 lg.modem=1 lg.power=1 "
        f"lg.device={device} lg.panel={panel}"
    )

    return f"""/dts-v1/;

/ {{
\tmodel = "LG K20 Plus {device} (MSM8917, MixOS bring-up)";
\tcompatible = "lge,{device}", "qcom,msm8917";
\t#address-cells = <2>;
\t#size-cells = <2>;
\tinterrupt-parent = <&intc>;

\tchosen {{
\t\tstdout-path = "serial0:115200n8";
\t\tbootargs = "{bootargs}";
\t}};

\tmemory@80000000 {{
\t\tdevice_type = "memory";
\t\treg = <0x0 0x80000000 0x0 0x80000000>;
\t}};

\treserved-memory {{
\t\t#address-cells = <2>;
\t\t#size-cells = <2>;
\t\tranges;

\t\t/* MBA = first 2 MB of the modem carveout, MPSS the rest. */
\t\tmba_region: mba@86800000 {{
\t\t\treg = <0x0 0x86800000 0x0 0x200000>;
\t\t\tno-map;
\t\t}};
\t\tmpss_region: mpss@86a00000 {{
\t\t\treg = <0x0 0x86a00000 0x0 0x4e00000>;
\t\t\tno-map;
\t\t}};
\t\twcnss_region: wcnss@8c900000 {{
\t\t\treg = <0x0 0x8c900000 0x0 0x700000>;
\t\t\tno-map;
\t\t}};
\t\tsmem_region: smem@86300000 {{
\t\t\treg = <0x0 0x86300000 0x0 0x100000>;
\t\t\tno-map;
\t\t}};
\t\tsplash_region: splash@90000000 {{
\t\t\treg = <0x0 0x90000000 0x0 0x1400000>;
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

\t/* Fixed clocks: no gcc-msm8917 in mainline, so these stand in for
\t * the clocks the LK leaves running (UART console, eMMC boot,
\t * QUP3). A driver that needs rate changes (LPASS, MDSS) cannot
\t * use them -- those nodes stay disabled. */
\txo: xo {{
\t\tcompatible = "fixed-clock";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <19200000>;
\t}};

\tsmem {{
\t\tcompatible = "qcom,smem";
\t\tmemory-region = <&smem_region>;
\t\thwlocks = <&tcsr_mutex 3>;
\t}};

\tsoc: soc@0 {{
\t\tcompatible = "simple-bus";
\t\t#address-cells = <1>;
\t\t#size-cells = <1>;
\t\tranges = <0 0 0 0xffffffff>;

\t\tintc: interrupt-controller@{gic:x} {{
\t\t\tcompatible = "qcom,msm-qgic2";
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <3>;
\t\t\treg = <{gic:#x} 0x1000>,
\t\t\t      <{gic_cpu:#x} 0x2000>;
\t\t}};

\t\trestart: restart@4ab000 {{
\t\t\tcompatible = "qcom,pshold";
\t\t\treg = <0x004ab000 0x4>;
\t\t}};

\t\ttcsr_mutex: hwlock@1905000 {{
\t\t\tcompatible = "qcom,tcsr-mutex";
\t\t\treg = <0x01905000 0x20000>;
\t\t\t#hwlock-cells = <1>;
\t\t}};

\t\tapcs: mailbox@b011000 {{
\t\t\tcompatible = "qcom,msm8917-apcs-kpss-global",
\t\t\t\t     "qcom,msm8916-apcs-kpss-global",
\t\t\t\t     "syscon";
\t\t\treg = <0x0b011000 0x1000>;
\t\t\t#mbox-cells = <1>;
\t\t}};

\t\tuart0: serial@{uart:x} {{
\t\t\tcompatible = "qcom,msm-uartdm-v1.4", "qcom,msm-uartdm";
\t\t\treg = <{uart:#x} {uart_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_uart} {IRQ_LEVEL_HIGH}>;
\t\t\tclocks = <&xo>, <&xo>;
\t\t\tclock-names = "core", "iface";
\t\t\tstatus = "okay";
\t\t}};

\t\tsdhc_1: mmc@{sdc1_hc:x} {{
\t\t\tcompatible = "qcom,sdhci-msm-v4";
\t\t\treg = <{sdc1_hc:#x} 0x500>,
\t\t\t      <{sdc1_core:#x} 0x800>;
\t\t\treg-names = "hc_mem", "core_mem";
\t\t\tinterrupts = <{GIC_SPI} {irq_sdc1} {IRQ_LEVEL_HIGH}>,
\t\t\t\t     <{GIC_SPI} {irq_sdc1_pwr} {IRQ_LEVEL_HIGH}>;
\t\t\tinterrupt-names = "hc_irq", "pwr_irq";
\t\t\tclocks = <&xo>, <&xo>, <&xo>;
\t\t\tclock-names = "iface", "core", "xo";
\t\t\tbus-width = <8>;
\t\t\tmax-frequency = <384000000>;
\t\t\tmmc-ddr-1_8v;
\t\t\tmmc-hs200-1_8v;
\t\t\tmmc-hs400-1_8v;
\t\t\tno-sd;
\t\t\tno-sdio;
\t\t\tnon-removable;
\t\t\tstatus = "okay";
\t\t}};

\t\tsdhc_2: mmc@{sdc2_hc:x} {{
\t\t\tcompatible = "qcom,sdhci-msm-v4";
\t\t\treg = <{sdc2_hc:#x} 0x500>,
\t\t\t      <{sdc2_core:#x} 0x800>;
\t\t\treg-names = "hc_mem", "core_mem";
\t\t\tinterrupts = <{GIC_SPI} {irq_sdc2} {IRQ_LEVEL_HIGH}>,
\t\t\t\t     <{GIC_SPI} {irq_sdc2_pwr} {IRQ_LEVEL_HIGH}>;
\t\t\tinterrupt-names = "hc_irq", "pwr_irq";
\t\t\tclocks = <&xo>, <&xo>, <&xo>;
\t\t\tclock-names = "iface", "core", "xo";
\t\t\tbus-width = <4>;
\t\t\tmax-frequency = <192000000>;
\t\t\tcap-sd-highspeed;
\t\t\tstatus = "okay";
\t\t}};

\t\ti2c_3: i2c@{i2c3:x} {{
\t\t\tcompatible = "qcom,i2c-qup-v2.2.1";
\t\t\treg = <{i2c3:#x} {i2c_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} {irq_i2c3} {IRQ_LEVEL_HIGH}>;
\t\t\tclocks = <&xo>, <&xo>;
\t\t\tclock-names = "core", "iface";
\t\t\t#address-cells = <1>;
\t\t\t#size-cells = <0>;
\t\t\tstatus = "okay";

\t\t\ttouchscreen@{touch_addr:x} {{
\t\t\t\tcompatible = "{touch_compat}";
\t\t\t\treg = <{touch_addr:#x}>;
\t\t\t\tinterrupt-parent = <&tlmm>;
\t\t\t\tinterrupts = <{touch_irq} {IRQ_EDGE_FALLING}>;
\t\t\t\treset-gpios = <&tlmm {touch_rst} 0>;
\t\t\t\tstatus = "okay";
\t\t\t}};
\t\t}};

\t\tspmi_bus: spmi@{spmi:x} {{
\t\t\tcompatible = "qcom,spmi-pmic-arb";
\t\t\treg = <0x200f000 0x1000>,
\t\t\t      <0x2400000 0x800000>,
\t\t\t      <0x2c00000 0x800000>,
\t\t\t      <0x3800000 0x200000>,
\t\t\t      <0x200a000 0x2100>;
\t\t\treg-names = "core", "chnls", "obsrvr", "intr", "cnfg";
\t\t\tinterrupts = <{GIC_SPI} 190 {IRQ_LEVEL_HIGH}>;
\t\t\t#address-cells = <2>;
\t\t\t#size-cells = <0>;
\t\t\tstatus = "okay";

\t\t\tpmi8950_0: pmic@0 {{
\t\t\t\tcompatible = "qcom,pmi8950", "qcom,spmi-pmic";
\t\t\t\treg = <0 0>;
\t\t\t\t#address-cells = <1>;
\t\t\t\t#size-cells = <0>;

\t\t\t\tpmi8950_wled: wled@d800 {{
\t\t\t\t\tcompatible = "qcom,pmi8950-wled";
\t\t\t\t\treg = <0xd800>;
\t\t\t\t\tqcom,enabled-strings = <0 1 2 3>;
\t\t\t\t\tqcom,current-limit-microamp = <20000>;
\t\t\t\t\tstatus = "okay";
\t\t\t\t}};

\t\t\t\tpmi8950_pwrkey: pon@800 {{
\t\t\t\t\tcompatible = "qcom,pm8941-pwrkey";
\t\t\t\t\treg = <0x800>;
\t\t\t\t\tstatus = "okay";
\t\t\t\t}};

\t\t\t\tpmi8950_resin: resin@810 {{
\t\t\t\t\tcompatible = "qcom,pm8941-resin";
\t\t\t\t\treg = <0x810>;
\t\t\t\t\tlinux,code = <{pmic_code}>;
\t\t\t\t\tstatus = "okay";
\t\t\t\t}};
\t\t\t}};
\t\t}};

\t\tpronto: pronto@{pronto:x} {{
\t\t\tcompatible = "lge,pronto-wcnss";
\t\t\treg = <{pronto:#x} {pronto_size:#x}>;
\t\t\tinterrupts = <{GIC_SPI} 149 {IRQ_EDGE_RISING}>;
\t\t\tmemory-region = <&wcnss_region>;
\t\t\tstatus = "okay";
\t\t}};

\t\tkeys: keys@{tlmm:x} {{
\t\t\tcompatible = "lge,lv517-keys-polled";
\t\t\treg = <{tlmm:#x} {tlmm_size:#x}>;
\t\t\tlge,key-pins = <{key_gpio} {hall_gpio}>;
\t\t\tlge,key-codes = <{key_code} {hall_code}>;
\t\t\tstatus = "okay";
\t\t}};
\t}};

\tsmp2p_modem: smp2p-modem {{
\t\tcompatible = "qcom,smp2p";
\t\tqcom,smem = <451>, <431>;
\t\tinterrupts = <{GIC_SPI} 27 {IRQ_EDGE_RISING}>;
\t\tmboxes = <&apcs 14>;
\t\tqcom,local-pid = <0>;
\t\tqcom,remote-pid = <1>;
\t\tmodem_smp2p_out: master-kernel {{
\t\t\tqcom,entry-name = "master-kernel";
\t\t\t#qcom,smem-state-cells = <1>;
\t\t}};
\t\tmodem_smp2p_in: slave-kernel {{
\t\t\tqcom,entry-name = "slave-kernel";
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <2>;
\t\t}};
\t}};

\tsmp2p_wcnss: smp2p-wcnss {{
\t\tcompatible = "qcom,smp2p";
\t\tqcom,smem = <451>, <431>;
\t\tinterrupts = <{GIC_SPI} 143 {IRQ_EDGE_RISING}>;
\t\tmboxes = <&apcs 18>;
\t\tqcom,local-pid = <0>;
\t\tqcom,remote-pid = <4>;
\t\twcnss_smp2p_out: master-kernel {{
\t\t\tqcom,entry-name = "master-kernel";
\t\t\t#qcom,smem-state-cells = <1>;
\t\t\t}};
\t\twcnss_smp2p_in: slave-kernel {{
\t\t\tqcom,entry-name = "slave-kernel";
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <2>;
\t\t}};
\t}};

\tsmd {{
\t\tcompatible = "qcom,smd";
\t\tmodem_edge: modem-edge {{
\t\t\tinterrupts = <{GIC_SPI} 25 {IRQ_EDGE_RISING}>;
\t\t\tqcom,ipc = <&apcs 8 12>;
\t\t\tqcom,smd-edge = <0>;
\t\t\tlabel = "modem";
\t\t}};
\t\twcnss_edge: wcnss-edge {{
\t\t\tinterrupts = <{GIC_SPI} 142 {IRQ_EDGE_RISING}>;
\t\t\tqcom,ipc = <&apcs 8 17>;
\t\t\tqcom,smd-edge = <6>;
\t\t\tlabel = "wcnss";
\t\t\twcnss_ctrl {{
\t\t\t\tcompatible = "qcom,wcnss";
\t\t\t\tqcom,smd-channel = "WCNSS_CTRL";
\t\t\t}};
\t\t}};
\t}};

\tmodem: modem-mss {{
\t\tcompatible = "qcom,msm8917-mss-pil", "qcom,msm8916-mss-pil";
\t\tmba-region = <&mba_region>;
\t\tmpss-region = <&mpss_region>;
\t\t/* Disabled: q6v5_mss needs CX/MX power domains and
\t\t * GCC clocks (no rpmpd/gcc-msm8917 yet). Flip when
\t\t * those land, or when a bootloader-on run proves
\t\t * the domains stay up without them. */
\t\tstatus = "disabled";
\t}};

\t/* Unclaimed until the TLMM port lands; present so phandle
\t * references (&tlmm) resolve. */
\ttlmm: pinctrl {{
\t\tcompatible = "qcom,msm8917-pinctrl";
\t\tgpio-controller;
\t\t#gpio-cells = <2>;
\t\tinterrupt-controller;
\t\t#interrupt-cells = <2>;
\t\tstatus = "disabled";
\t}};

\tlge_modem: lge-modem {{
\t\tcompatible = "lge,lv517-modem";
\t\tstatus = "okay";
\t}};

\tsound {{
\t\tcompatible = "lge,lv517-sound";
\t\t/* Disabled: the machine binds mainline LPASS, whose MI2S
\t\t * clocks need the LPASSCC/GCC driver. USB audio covers v1. */
\t\tstatus = "disabled";
\t}};

\tpower {{
\t\tcompatible = "lge,lv517-power";
\t\tstatus = "okay";
\t}};

\tframebuffer0: framebuffer@{lk_fb_base:x} {{
\t\tcompatible = "simple-framebuffer";
\t\treg = <0x0 {lk_fb_base:#x} 0x0 0x1400000>;
\t\twidth = <{width}>;
\t\theight = <{height}>;
\t\tstride = <({stride})>;
\t\tformat = "a8b8g8r8";
\t\tstatus = "{fb_enabled}";
\t}};

\taliases {{
\t\tserial0 = &uart0;
\t}};
}};
"""


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the LG K20 Plus bring-up DTS")
    parser.add_argument("--board", required=True, help="board/ extract directory")
    parser.add_argument("--device", default="lv517", help="LG codename (devices.sh)")
    parser.add_argument("--panel", default="lg4894", choices=["lg4894", "td4100"])
    parser.add_argument("--rev", default="b", help="board revision (keys map)")
    parser.add_argument("--lk-fb-base", default="0x90000000",
                        help="LK splash framebuffer base (0 disables the node)")
    parser.add_argument("--out", required=True, help="output .dts path")
    args = parser.parse_args()

    sources = read_sources(Path(args.board))
    dts = generate(sources, args.device, args.panel, args.rev,
                   int(args.lk_fb_base, 0))
    Path(args.out).write_text(dts)
    print(f"wrote {args.out} for LG {args.device} ({args.panel})")


if __name__ == "__main__":
    main()
