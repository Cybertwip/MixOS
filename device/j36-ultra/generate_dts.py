#!/usr/bin/env python3
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
"""Generate the J36 Ultra MT6592 bring-up DTS from the MVII driver sources.

The panel timing, power GPIOs, compact 155-record JD9365 command table, keypad
matrix, keypad pad mux, direct GPIO keys, framebuffer and AUXADC channels are
parsed from:
  PowerEngine/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers

No data is taken from PowerEngine/Reference/J36-ULTRA.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


DRIVER_FILES = {
    "board": "mt6592_board_j36.h",
    "display": "mt6592_disp_hw.h",
    "panel": "panel_bringup.h",
    "dsi": "dsi_drv.c",
    "keys": "mt6592_keys.c",
}

# Linux input-event codes. Keep numeric values in the generated DTS so plain
# dtc can compile it without a kernel dt-bindings include path.
KEY_UP = 103
KEY_DOWN = 108
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_F12 = 88
KEY_VOLUMEDOWN = 114
KEY_VOLUMEUP = 115
BTN_A = 0x130
BTN_B = 0x131
BTN_X = 0x133
BTN_Y = 0x134
BTN_TL = 0x136
BTN_TR = 0x137
BTN_TL2 = 0x138
BTN_TR2 = 0x139
BTN_SELECT = 0x13A
BTN_START = 0x13B
BTN_MODE = 0x13C
BTN_THUMBL = 0x13D
BTN_THUMBR = 0x13E
ABS_X = 0x00
ABS_Y = 0x01
ABS_Z = 0x02
ABS_RZ = 0x05
GPIO_ACTIVE_LOW = 1


def read_sources(drivers: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for key, name in DRIVER_FILES.items():
        path = drivers / name
        if not path.is_file():
            raise SystemExit(f"missing MVII J36 driver source: {path}")
        result[key] = path.read_text(encoding="utf-8")
    return result


def parse_int(text: str, name: str, _depth: int = 0) -> int:
    patterns = (
        rf"\b{name}\s*=\s*(-?(?:0x[0-9a-fA-F]+|[0-9]+))u?\b",
        rf"#define\s+{name}\s+(-?(?:0x[0-9a-fA-F]+|[0-9]+))u?\b",
    )
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return int(match.group(1), 0)
    # One constant may be defined as another's name. The board header writes
    # MT6592_J36_KPD_STROBE2_GPIO = MT6592_J36_KPD_PAD_UNMAPPED, because this
    # board spent KPROW2's pad on the D-pad UP EINT, so an alias is a fact about
    # the wiring and not a parse failure. Followed, with a depth cap so a typo
    # that makes a constant refer to itself still fails loudly.
    alias = re.search(rf"\b{name}\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\b", text)
    if alias and _depth < 4:
        return parse_int(text, alias.group(1), _depth + 1)
    raise SystemExit(f"could not parse {name} from MVII J36 drivers")


def parse_jd9365_records(text: str) -> list[tuple[int, int, int, int]]:
    table = re.search(
        r"static const jd9365_v3_entry_t g_jd9365_stock_init\[\]\s*=\s*\{(.*?)\n\};",
        text,
        re.S,
    )
    if not table:
        raise SystemExit("could not find g_jd9365_stock_init in dsi_drv.c")
    records = [
        tuple(int(value, 16) for value in groups)
        for groups in re.findall(
            r"JD9365_V3\(0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+),\s*"
            r"0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+)\)",
            table.group(1),
        )
    ]
    if len(records) != 155:
        raise SystemExit(f"expected 155 JD9365 records, found {len(records)}")
    return records


def matrix_key(row: int, column: int, code: int) -> int:
    return (row << 24) | (column << 16) | code


def check_kpd_pads(
    strobes: list[tuple[int, int]],
    senses: list[tuple[int, int]],
    reserved: list[tuple[int, int]],
    direct_keys: list[tuple[str, int, int]],
    gpio_max: int,
) -> None:
    """Refuse to emit a pad description that the board header calls a regression.

    Every rule here is one that hardware has already broken once, so the
    generator is the place to catch it rather than the console:

      * a keypad pad claimed twice, or claimed by a button as well -- the wire
        scan once claimed pads 11, 12 and 2, and row 3 plus columns 3 and 4
        stopped scanning;
      * the reserved pad appearing among the strobes -- pad 93 is D-pad UP's
        EINT here, and muxing it as KPROW2 buys a matrix row whose keys this
        board does not have and costs UP;
      * a pad above MT6592_J36_GPIO_MAX, which the preloader's own
        mt_set_gpio_mode rejects before it touches a register.
    """
    owned: dict[int, str] = {}
    for label, pads in (("strobe", strobes), ("sense", senses)):
        for pad, mode in pads:
            if pad in owned:
                raise SystemExit(
                    f"KPD pad {pad} is claimed as both {owned[pad]} and {label}"
                )
            if pad > gpio_max:
                raise SystemExit(f"KPD {label} pad {pad} is above GPIO_MAX {gpio_max}")
            if mode == 0:
                raise SystemExit(f"KPD {label} pad {pad} wants mode 0, which is plain GPIO")
            owned[pad] = label
    for pad, mode in reserved:
        if pad in owned:
            raise SystemExit(f"pad {pad} is reserved for a button but muxed as {owned[pad]}")
        if mode != 0:
            raise SystemExit(f"reserved pad {pad} must stay mode 0, not {mode}")
    for name, gpio, _code in direct_keys:
        if gpio in owned:
            raise SystemExit(f"{name} is on pad {gpio}, which the keypad block owns as {owned[gpio]}")
        if gpio > gpio_max:
            raise SystemExit(f"{name} is on pad {gpio}, above GPIO_MAX {gpio_max}")


def format_pad_pairs(pads: list[tuple[int, int]], indent: str = "\t\t\t") -> str:
    return ",\n".join(f"{indent}<{pad} {mode}>" for pad, mode in pads)


def format_cells(values: list[int], per_line: int = 4, indent: str = "\t\t\t") -> str:
    lines = []
    for offset in range(0, len(values), per_line):
        chunk = values[offset : offset + per_line]
        lines.append(indent + " ".join(f"0x{value:08x}" for value in chunk))
    return "\n".join(lines)


def format_bytes(records: list[tuple[int, int, int, int]]) -> str:
    flat = [byte for record in records for byte in record]
    lines = []
    for offset in range(0, len(flat), 16):
        chunk = flat[offset : offset + 16]
        lines.append("\t\t\t\t" + " ".join(f"{byte:02x}" for byte in chunk))
    return "\n".join(lines)


def generate(sources: dict[str, str]) -> str:
    board = sources["board"]
    display = sources["display"]
    panel = sources["panel"]
    keys = sources["keys"]
    records = parse_jd9365_records(sources["dsi"])

    width = parse_int(board, "MT6592_J36_PANEL_WIDTH")
    height = parse_int(board, "MT6592_J36_PANEL_HEIGHT")
    pixel_clock = parse_int(board, "MT6592_J36_PANEL_PIXEL_CLOCK_HZ")
    pll_mhz = parse_int(board, "MT6592_J36_PANEL_DSI_PLL_CLOCK_MHZ")
    lanes = parse_int(board, "MT6592_J36_PANEL_DSI_LANES")
    hfp = parse_int(board, "MT6592_J36_PANEL_HFP")
    hsync = parse_int(board, "MT6592_J36_PANEL_HSYNC")
    hbp = parse_int(board, "MT6592_J36_PANEL_HBP")
    vfp = parse_int(board, "MT6592_J36_PANEL_VFP")
    vsync = parse_int(board, "MT6592_J36_PANEL_VSYNC")
    vbp = parse_int(board, "MT6592_J36_PANEL_VBP")
    fb_addr = parse_int(board, "MT6592_J36_LK_HANDOFF_FB_ADDR")
    fb_pitch = parse_int(board, "MT6592_J36_LK_HANDOFF_FB_PITCH")

    reset_gpio = parse_int(panel, "MT6592_PANEL_GPIO_RESET")
    power0_gpio = parse_int(panel, "MT6592_PANEL_GPIO_PWR0")
    power1_gpio = parse_int(panel, "MT6592_PANEL_GPIO_PWR1")
    backlight_gpio = parse_int(display, "MTK_BACKLIGHT_GPIO")

    direct_keys = [
        ("dpad-up", parse_int(board, "MT6592_J36_KEY_DPAD_UP_GPIO"), KEY_UP),
        ("dpad-down", parse_int(board, "MT6592_J36_KEY_DPAD_DOWN_GPIO"), KEY_DOWN),
        ("dpad-left", parse_int(board, "MT6592_J36_KEY_DPAD_LEFT_GPIO"), KEY_LEFT),
        ("dpad-right", parse_int(board, "MT6592_J36_KEY_DPAD_RIGHT_GPIO"), KEY_RIGHT),
        # These three direct GPIO/EINT keys are documented beside the D-pad map
        # in mt6592_board_j36.h and are part of the same stock input device.
        ("left-stick-click", 7, BTN_THUMBL),
        ("right-stick-click", 46, BTN_THUMBR),
        ("special-home", 0, KEY_F12),
    ]

    matrix = {
        "A": (parse_int(board, "MT6592_J36_KEY_A_MATRIX"), BTN_A),
        "B": (parse_int(board, "MT6592_J36_KEY_B_MATRIX"), BTN_B),
        "X": (parse_int(board, "MT6592_J36_KEY_X_MATRIX"), BTN_X),
        "Y": (parse_int(board, "MT6592_J36_KEY_Y_MATRIX"), BTN_Y),
        "L1": (parse_int(board, "MT6592_J36_KEY_L1_MATRIX"), BTN_TL),
        "R1": (parse_int(board, "MT6592_J36_KEY_R1_MATRIX"), BTN_TR),
        "L2": (parse_int(board, "MT6592_J36_KEY_L2_MATRIX"), BTN_TL2),
        "R2": (parse_int(board, "MT6592_J36_KEY_R2_MATRIX"), BTN_TR2),
        "START": (parse_int(board, "MT6592_J36_KEY_START_MATRIX"), BTN_START),
        "SELECT": (parse_int(board, "MT6592_J36_KEY_SELECT_MATRIX"), BTN_SELECT),
        "MENU": (parse_int(board, "MT6592_J36_KEY_MENU_MATRIX"), BTN_MODE),
        "VOLUP": (parse_int(board, "MT6592_J36_KEY_VOL_UP_MATRIX"), KEY_VOLUMEUP),
        "VOLDOWN": (parse_int(board, "MT6592_J36_KEY_VOL_DOWN_MATRIX"), KEY_VOLUMEDOWN),
    }
    keymap = []
    matrix_bit_map = []
    for _name, (bit, code) in matrix.items():
        row, column = divmod(bit, 9)
        keymap.append(matrix_key(row, column, code))
        matrix_bit_map.extend((bit, code))

    # The nine pads the MT6592 keypad block owns on this board, each paired with
    # the mux mode it needs. The mode is a per-pad fact: mode 1 is the keypad
    # function on the five pads the preloader already muxes, but pad 11 (KPROW3)
    # and pad 12 (KPCOL3) land there at mode 3 and pad 2 (KPCOL4) at mode 6, all
    # three measured with `kpdmode <pad>` in the LK console. KPROW2 has no pad on
    # this board, so its entry is the header's UNMAPPED alias and drops out.
    kpd_unmapped = parse_int(board, "MT6592_J36_KPD_PAD_UNMAPPED")
    kpd_mode = parse_int(board, "MT6592_J36_KPD_MUX_MODE")
    gpio_max = parse_int(board, "MT6592_J36_GPIO_MAX")
    strobe_pads = [
        (parse_int(board, "MT6592_J36_KPD_STROBE0_GPIO"), kpd_mode),
        (parse_int(board, "MT6592_J36_KPD_STROBE1_GPIO"), kpd_mode),
        (parse_int(board, "MT6592_J36_KPD_STROBE2_GPIO"), kpd_mode),
        (
            parse_int(board, "MT6592_J36_KPD_STROBE3_GPIO"),
            parse_int(board, "MT6592_J36_KPD_STROBE3_MUX_MODE"),
        ),
    ]
    sense_pads = [
        (parse_int(board, "MT6592_J36_KPD_SENSE0_GPIO"), kpd_mode),
        (parse_int(board, "MT6592_J36_KPD_SENSE1_GPIO"), kpd_mode),
        (parse_int(board, "MT6592_J36_KPD_SENSE2_GPIO"), kpd_mode),
        (
            parse_int(board, "MT6592_J36_KPD_SENSE3_GPIO"),
            parse_int(board, "MT6592_J36_KPD_SENSE3_MUX_MODE"),
        ),
        (
            parse_int(board, "MT6592_J36_KPD_SENSE4_GPIO"),
            parse_int(board, "MT6592_J36_KPD_SENSE4_MUX_MODE"),
        ),
    ]
    strobe_pads = [pair for pair in strobe_pads if pair[0] != kpd_unmapped]
    sense_pads = [pair for pair in sense_pads if pair[0] != kpd_unmapped]
    reserved_pads = [(parse_int(board, "MT6592_J36_KPD_ROW2_PAD_TAKEN_BY_DPAD_UP"), 0)]
    check_kpd_pads(strobe_pads, sense_pads, reserved_pads, direct_keys, gpio_max)
    strobe_pad_cells = format_pad_pairs(strobe_pads)
    sense_pad_cells = format_pad_pairs(sense_pads)
    reserved_pad_cells = format_pad_pairs(reserved_pads)

    joy_x = parse_int(keys, "AUXADC_JOY_X")
    joy_y = parse_int(keys, "AUXADC_JOY_Y")
    joy_z = parse_int(keys, "AUXADC_JOY_Z")
    joy_rz = parse_int(keys, "AUXADC_JOY_RZ")
    joy_center = parse_int(keys, "AUXADC_JOY_FALLBACK_CENTER")
    joy_deadzone = parse_int(keys, "AUXADC_JOY_DEADZONE")

    gpio_children = []
    for name, gpio, code in direct_keys:
        gpio_children.append(
            f"""\t\t{name} {{
\t\t\tlabel = \"J36 Ultra {name}\";
\t\t\tlinux,code = <{code}>;
\t\t\tgpios = <&gpio {gpio} {GPIO_ACTIVE_LOW}>;
\t\t\tdebounce-interval = <5>;
\t\t}};"""
        )

    # Compact V3 records are exactly (data_id, command, count, parameter0),
    # including the two delay markers (00 ff c8 00 and 00 ff 64 00).
    init_bytes = format_bytes(records)
    keymap_cells = format_cells(keymap, per_line=3)
    matrix_map_cells = format_cells(matrix_bit_map, per_line=4)
    direct_map = []
    for _name, gpio, code in direct_keys:
        direct_map.extend((gpio, code))
    direct_map_cells = format_cells(direct_map, per_line=4, indent="\t\t")
    axis_map = [
        joy_x, ABS_X, 1,
        joy_y, ABS_Y, 1,
        joy_z, ABS_Z, 0,
        joy_rz, ABS_RZ, 0,
    ]
    axis_map_cells = format_cells(axis_map, per_line=6, indent="\t\t")

    return f"""/dts-v1/;

/*
 * J36 Ultra MT6592 bring-up DTB.
 *
 * GENERATED from PowerEngine OS/MVII J36Ultra/Drivers. Do not hand-edit the
 * generated file; update the MVII driver source or generate_dts.py instead.
 *
 * This describes the hardware and exact panel/input wiring. The compatible
 * strings marked j36,* intentionally require the small Linux adapter drivers;
 * a DTB cannot execute the DSI program or read the MT6592 KPD/AUXADC by itself.
 */

/ {{
\t#address-cells = <1>;
\t#size-cells = <1>;
\tmodel = \"J36 Ultra (MediaTek MT6592)\";
\tcompatible = \"j36,j36-ultra\", \"mediatek,mt6592\";
\tinterrupt-parent = <&sysirq>;

\tchosen {{
\t\tstdout-path = \"serial0:115200n8\";
\t}};

\taliases {{
\t\tserial0 = &uart0;
\t}};

\tmemory@80000000 {{
\t\tdevice_type = \"memory\";
\t\treg = <0x80000000 0x40000000>;
\t}};

\treserved-memory {{
\t\t#address-cells = <1>;
\t\t#size-cells = <1>;
\t\tranges;

\t\tlk_framebuffer: framebuffer@{fb_addr:08x} {{
\t\t\treg = <0x{fb_addr:08x} 0x{width * height * 4:08x}>;
\t\t\tno-map;
\t\t}};
\t}};

\tcpus {{
\t\t#address-cells = <1>;
\t\t#size-cells = <0>;
\t\tcpu@0 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <0>; }};
\t\tcpu@1 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <1>; }};
\t\tcpu@2 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <2>; }};
\t\tcpu@3 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <3>; }};
\t\tcpu@4 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <4>; }};
\t\tcpu@5 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <5>; }};
\t\tcpu@6 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <6>; }};
\t\tcpu@7 {{ device_type = \"cpu\"; compatible = \"arm,cortex-a7\"; reg = <7>; }};
\t}};

\tsoc {{
\t\t#address-cells = <1>;
\t\t#size-cells = <1>;
\t\tcompatible = \"simple-bus\";
\t\tranges;

\t\tgic: interrupt-controller@10211000 {{
\t\t\tcompatible = \"arm,cortex-a7-gic\";
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <3>;
\t\t\tinterrupt-parent = <&gic>;
\t\t\treg = <0x10211000 0x1000>, <0x10212000 0x1000>;
\t\t}};

\t\tsysirq: interrupt-controller@10200220 {{
\t\t\tcompatible = \"mediatek,mt6592-sysirq\", \"mediatek,mt6577-sysirq\";
\t\t\tinterrupt-controller;
\t\t\t#interrupt-cells = <3>;
\t\t\tinterrupt-parent = <&gic>;
\t\t\treg = <0x10200220 0x1c>;
\t\t}};

\t\t/*
\t\t * This node is where the SPI numbering in this file is anchored, so it
\t\t * is worth writing down once here rather than at every consumer.
\t\t *
\t\t * MediaTek's own kernel numbers interrupts by GIC interrupt ID, not by
\t\t * SPI index. Three independent register computations in the stock
\t\t * MT6592 kernel say so, all applying the raw number with no offset:
\t\t *
\t\t *   mt_irq_mask      0xc0324d68  GICD_ICENABLER 0xf0211180 + (irq>>5)*4,
\t\t *                                bit irq&31
\t\t *   mt_irq_set_sens  0xc03250b8  GICD_ICFGR     0xf0211c00 + (irq>>4)*4,
\t\t *                                bit pair 2*(irq&15)
\t\t *   mt_irq_set_polarity
\t\t *                    0xc0325148  INT_POL_CTL0   0xf0200220 + ((irq-32)>>5)*4,
\t\t *                                bit irq&31
\t\t *
\t\t * The GIC layout fixes ICENABLER's index at INTID/32 and ICFGR's at
\t\t * INTID/16, so the argument is an INTID and SPI = INTID - 32. The
\t\t * polarity register agrees: its linear bit is INTID-32, which is
\t\t * exactly the bit mainline's mtk-sysirq computes from hwirq, and its
\t\t * seven words cover SPI 0..223 for INTIDs 32..255 with nothing over.
\t\t * INT_POL_CTL0 is at MCUCFG+0x220 on this SoC, not the +0x620 later
\t\t * MediaTek parts use -- hence the reg on the sysirq node above.
\t\t *
\t\t * And this timer is the hardware proof, not just the derivation: the
\t\t * stock kernel's own irqaction for gpt_handler, at 0xc0b33380, reads
\t\t * {{handler=gpt_handler, irq=176, flags=0x15228 (IRQF_TRIGGER_LOW |
\t\t * __IRQF_TIMER | ...), name=\"mt6592-gpt\"}}. 176 - 32 = 144, the value
\t\t * below, and it is the clockevent for this profile -- there is no
\t\t * arch-timer node, so if 144 were wrong nothing would schedule at all.
\t\t * IRQF_TRIGGER_LOW is the type cell's 8.
\t\t *
\t\t * Do not derive these from mainline mt6592.dtsi. It puts UART0..UART3
\t\t * at GIC_SPI 51..54 where the stock kernel has INTIDs 115..118, i.e.
\t\t * -64, and that cost a boot: see the MSDC1 node.
\t\t */
\t\ttimer: timer@10008000 {{
\t\t\tcompatible = \"mediatek,mt6577-timer\";
\t\t\treg = <0x10008000 0x80>;
\t\t\tinterrupts = <0 144 8>; /* GIC_SPI 144 = INTID 176, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&system_clk>, <&rtc_clk>;
\t\t\tclock-names = \"system-clk\", \"rtc-clk\";
\t\t}};

\t\t/*
\t\t * Without this node the kernel cannot restart the board. Userspace gets
\t\t * all the way through a clean shutdown -- filesystems unmounted, swaps
\t\t * deactivated, \"systemd-shutdown[1]: Rebooting.\" -- and then:
\t\t *
\t\t *   reboot: Restarting system
\t\t *   Reboot failed -- System halted
\t\t *
\t\t * machine_restart() had no handler to call. On MediaTek the reset is the
\t\t * TOPRGU watchdog: watchdog_core.c registers a restart handler for any
\t\t * watchdog whose ops provide .restart, and mtk_wdt provides it.
\t\t * CONFIG_MEDIATEK_WATCHDOG was already y -- multi_v7_defconfig keeps it
\t\t * under ARCH_MEDIATEK -- so the driver was built in the whole time and
\t\t * simply had nothing to probe. That matters for the first-boot scripts as
\t\t * much as for a user reboot: MixOS expands its partitions in two stages
\t\t * with a reboot between them, and a reboot that halts turns that into a
\t\t * power-cycle by hand.
\t\t *
\t\t * 0x10007000 is the RGU here, from three places that agree: the mtkclient
\t\t * chip table (watchdog=0x10007000 for MT6592), its stage1 payload for this
\t\t * family, which reboots by writing RESTART 0x1971, MODE 0x22000014 and
\t\t * SWRST 0x1209, and PowerEngine's own bare-metal Standalone/src/hello.c.
\t\t * mtk_wdt_restart() writes that same SWRST key 0x1209 at offset 0x14,
\t\t * after probe has already put the 0x22000000 key into MODE.
\t\t *
\t\t * mediatek,mt6589-wdt alone, with no invented mt6592 string in front of
\t\t * it: mt6589-wdt is the plain variant of this block in the driver's OF
\t\t * table -- no reset controller, no clock -- and a leading compatible that
\t\t * matches nothing only risks the node not binding at all. No interrupt is
\t\t * needed (platform_get_irq_optional), and probe calls mtk_wdt_stop(), so
\t\t * the hardware timer stays disarmed; nothing in the MixOS rootfs opens
\t\t * /dev/watchdog, and the board already runs for minutes with no node at
\t\t * all, so the LK is not leaving it armed either.
\t\t */
\t\twatchdog: watchdog@10007000 {{
\t\t\tcompatible = \"mediatek,mt6589-wdt\";
\t\t\treg = <0x10007000 0x100>;
\t\t}};

\t\t/*
\t\t * 83, not mainline's 51: the stock kernel's resource array gives this
\t\t * base INTID 115, and 115 - 32 = 83. Console output does not prove
\t\t * either number, because 8250 console writes poll the THRE bit and
\t\t * never touch the interrupt -- which is precisely why a wrong SPI here
\t\t * survived unnoticed and then got copied to MSDC1, where it mattered.
\t\t * Receiving is what needs the IRQ, so a shell on ttyS0 is the test.
\t\t */
\t\tuart0: serial@11002000 {{
\t\t\tcompatible = \"mediatek,mt6577-uart\";
\t\t\treg = <0x11002000 0x400>;
\t\t\tinterrupts = <0 83 8>; /* GIC_SPI 83 = INTID 115, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&uart_clk>;
\t\t\tstatus = \"okay\";
\t\t}};
\t\tgpio: gpio@10005000 {{
\t\t\tcompatible = \"j36,mt6592-gpio\";
\t\t\treg = <0x10005000 0x1000>;
\t\t\tgpio-controller;
\t\t\t#gpio-cells = <2>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tpwrap: pwrap@1000d000 {{
\t\t\tcompatible = \"j36,mt6592-pwrap\";
\t\t\treg = <0x1000d000 0x1000>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tpericfg: syscon@10003000 {{
\t\t\tcompatible = \"j36,mt6592-pericfg\", \"syscon\";
\t\t\treg = <0x10003000 0x1000>;
\t\t}};

\t\t/*
\t\t * The two clock blocks, as syscons rather than clock providers. There is
\t\t * no MT6592 entry in drivers/clk/mediatek, so nothing here can hand out a
\t\t * struct clk; the consumers that need a gate released reach the register
\t\t * through a phandle and clear one documented bit. Same shape and same
\t\t * reason as pericfg above, which is how the input adapter reaches the
\t\t * keypad's peripheral gate.
\t\t */
\t\ttopckgen: syscon@10000000 {{
\t\t\tcompatible = \"j36,mt6592-topckgen\", \"syscon\";
\t\t\treg = <0x10000000 0x1000>;
\t\t}};

\t\tinfracfg: syscon@10001000 {{
\t\t\tcompatible = \"j36,mt6592-infracfg\", \"syscon\";
\t\t\treg = <0x10001000 0x1000>;
\t\t}};

\t\t/*
\t\t * The AFE. Not mediatek,mt6592-audio and not an ASoC card: nothing
\t\t * upstream binds either name -- sound/soc/mediatek starts at MT2701 --
\t\t * so this is our own compatible for our own adapter, and it stays a
\t\t * j36, prefix precisely so it can never collide with a real binding.
\t\t *
\t\t * reg is the AFE window alone. The three phandles are the blocks the
\t\t * driver has to reach but does not own: two clock gates it clears, and
\t\t * the PMIC wrapper it programs the MT6323 analog downlink through.
\t\t *
\t\t * status is \"okay\" and this costs nothing on a boot that does not ask
\t\t * for audio: the driver is a module on the boot partition, so with no
\t\t * j36.audio word nothing is ever loaded to bind to this node.
\t\t */
\t\taudio: audio@11220000 {{
\t\t\tcompatible = \"j36,j36-ultra-audio\";
\t\t\treg = <0x11220000 0x1000>;
\t\t\tj36,topckgen-controller = <&topckgen>;
\t\t\tj36,infracfg-controller = <&infracfg>;
\t\t\tj36,pwrap-controller = <&pwrap>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tauxadc: adc@11001000 {{
\t\t\tcompatible = \"j36,mt6592-auxadc\";
\t\t\treg = <0x11001000 0x1000>;
\t\t\t#io-channel-cells = <1>;
\t\t\tmediatek,channel-count = <16>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tkeypad: keypad@10011000 {{
\t\t\tcompatible = \"j36,j36-ultra-keypad\", \"j36,mt6592-keypad\";
\t\t\treg = <0x10011000 0x100>;
\t\t\tkeypad,num-rows = <8>;
\t\t\tkeypad,num-columns = <9>;
\t\t\tdebounce-delay-ms = <8>;
\t\t\tpoll-interval-ms = <5>;
\t\t\tmediatek,active-low;
\t\t\tlinux,keymap = <
{keymap_cells}
\t\t\t>;
\t\t\t/* Pairs of raw KPD_MEM bit index and Linux input code. */
\t\t\tmediatek,matrix-bit-map = <
{matrix_map_cells}
\t\t\t>;

\t\t\t/*
\t\t\t * THE PADS THIS BLOCK OWNS, as <pad mux-mode> pairs, and they are not
\t\t\t * optional. The boot chain hands over only five of the eight muxed:
\t\t\t * KPROW3 (11), KPCOL3 (12) and KPCOL4 (2) arrive parked as plain GPIO,
\t\t\t * and with them parked the block only ever scans matrix bits
\t\t\t * {{0,1,2,9,10,11}} -- so VOL-, VOL+, SELECT, START, MENU, R2 and A read
\t\t\t * as never pressed no matter how correct the keymap above is.
\t\t\t *
\t\t\t * The mode differs per pad and every value here was measured, not
\t\t\t * assumed (`kpdmode <pad>` in the MVII LK console sweeps one pad through
\t\t\t * all eight modes with a button held). Mode 1 is the keypad function on
\t\t\t * 74/92/75/167/168; pad 11 and pad 12 need mode 3 and pad 2 needs mode
\t\t\t * 6. Writing mode 1 to pad 11 turns it into a STATIC LOW rather than a
\t\t\t * strobed row, which drags its column low in all eight rows at once --
\t\t\t * SELECT then lights bits 2, 11, 20, 29, 38, 47, 56 and 65 together.
\t\t\t *
\t\t\t * Strobes are outputs and keep whatever pull they have; senses are
\t\t\t * inputs with a pull-up. A pad already in its wanted mode must be left
\t\t\t * exactly as found, floating sense lines included: 167 and 168 sit in
\t\t\t * mode 1 with no pull at all and Y and B read fine through them.
\t\t\t */
\t\t\tj36,kpd-strobe-pads =
{strobe_pad_cells};
\t\t\tj36,kpd-sense-pads =
{sense_pad_cells};

\t\t\t/*
\t\t\t * KPROW2's pad, which this board spent on the D-pad UP EINT instead --
\t\t\t * which is the whole reason matrix row 2 is dead here. It must stay a
\t\t\t * mode-0 GPIO input; muxing it buys a matrix row whose keys this board
\t\t\t * does not have and costs UP. Listed so the driver can prove on every
\t\t\t * boot that it is still mode 0 and put it back if something took it.
\t\t\t *
\t\t\t * It also will not idle high: it reads 0 with the internal pull-up armed
\t\t\t * and verified in the register readback, held or released, because
\t\t\t * something on the board loads it harder than that resistor can fight.
\t\t\t * Driven high instead it behaves perfectly. The driver measures this at
\t\t\t * probe rather than trusting the list -- see the driven-read path in
\t\t\t * j36_mt6592_input.c -- so a board revision that populates the missing
\t\t\t * pull-up simply never takes it.
\t\t\t */
\t\t\tj36,kpd-reserved-pads =
{reserved_pad_cells};
\t\t\tstatus = \"okay\";
\t\t}};

\t\t/*
\t\t * The display pipe, as mainline mtk_drm sees it.
\t\t *
\t\t * MT6592's DDP is the MT2701/MT8173 generation. Every base and offset
\t\t * below was checked against the MVII LK that lights this panel today, so
\t\t * almost nothing here is new: each node names
\t\t * \"mediatek,mt6592-<block>\" first and \"mediatek,mt2701-<block>\"
\t\t * second, and __of_match_node() scores the first higher where the kernel
\t\t * patch adds an mt6592 entry (MMSYS and OVL, which are the only two that
\t\t * differ) and falls through to the second everywhere else, because the
\t\t * mt2701 driver data is already exact -- mutex MOD 0x488 / SOF 1, RDMA
\t\t * FIFO 4 KiB, the COLOR start offset, the DSI CMDQ and VM_CMD offsets,
\t\t * and MPPLL_PRESERVE 3. See linux/0002-drm-mediatek-mt6592.patch, which
\t\t * records how each of those was measured.
\t\t *
\t\t * They must all be siblings of mmsys. mtk_mmsys_probe() registers the
\t\t * \"mediatek-drm\" platform device as its own child, and mtk_drm_probe()
\t\t * then collects components by walking
\t\t * for_each_child_of_node(mmsys_node->parent) -- a node one level deeper
\t\t * is never found, and a node with status = \"disabled\" is skipped.
\t\t *
\t\t * All five drivers are modules, and nothing insmods them unless the
\t\t * card's BOOT partition carries a j36/mtkdrm directory and the command
\t\t * line says j36.mtkdrm=1. With the modules absent these nodes bind
\t\t * nothing at all, so the default boot stays byte-identical and they can
\t\t * safely say okay. Even when they do load, no register is touched until
\t\t * userspace opens /dev/dri/card0 and sets a mode: DRM_FBDEV_EMULATION is
\t\t * off, so mtk_drm registers a card and then waits, and the LK's image on
\t\t * the simple-framebuffer below stays exactly where it is.
\t\t */
\t\tdsi_phy: phy@10010000 {{
\t\t\tcompatible = \"mediatek,mt6592-mipi-tx\", \"mediatek,mt2701-mipi-tx\";
\t\t\treg = <0x10010000 0x1000>;
\t\t\t#phy-cells = <0>;
\t\t\t/*
\t\t\t * This PHY is also the DSI host's \"hs\" clock provider, which is
\t\t\t * how mtk_dsi asks for a data rate: clk_set_rate(hs_clk, ...)
\t\t\t * lands in mtk_mipi_tx_pll_set_rate and the PLL is programmed
\t\t\t * from it at phy_power_on. clock-output-names is mandatory --
\t\t\t * mtk_mipi_tx_probe fails the probe without it.
\t\t\t *
\t\t\t * The unnamed reference clock is taken for its NAME only, as the
\t\t\t * PLL's clk parent; mt8173_mipi_tx_pll_prepare hardcodes the
\t\t\t * 26 MHz reference in its PCW arithmetic and never reads this
\t\t\t * rate. 26 MHz is nonetheless the true reference: mainline's
\t\t\t * pcw = data_rate * 2 * txdiv << 24 / 26000000 is the same
\t\t\t * number as the LK's pcw = data_Rate * txdiv / 13, and the two
\t\t\t * txdiv ladders (500/250/125/62) are identical.
\t\t\t *
\t\t\t * mediatek,efuse-trim-reg is the LK's, not mainline's: the LK
\t\t\t * reads DSI_EFUSE_RES3 here and programs each lane's RT_CODE
\t\t\t * from it, defaulting to 0x8. Mainline wants an nvmem
\t\t\t * \"calibration-data\" cell instead, does not find one, logs
\t\t\t * \"can't get nvmem_cell_get, ignore it\" and carries on -- and
\t\t\t * the mt2701 signal path never writes RT_CODE, so the LK's
\t\t\t * calibration survives untouched underneath it.
\t\t\t */
\t\t\t#clock-cells = <0>;
\t\t\tclocks = <&dsi_ref_clk>;
\t\t\tclock-output-names = \"mipi_tx0_pll\";
\t\t\tmediatek,efuse-trim-reg = <0x10206180>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tmmsys: syscon@14000000 {{
\t\t\tcompatible = \"mediatek,mt6592-mmsys\", \"mediatek,mt2701-mmsys\", \"syscon\";
\t\t\treg = <0x14000000 0x1000>;
\t\t}};

\t\tovl0: ovl@14007000 {{
\t\t\tcompatible = \"mediatek,mt6592-disp-ovl\", \"mediatek,mt2701-disp-ovl\";
\t\t\treg = <0x14007000 0x1000>;
\t\t\tinterrupts = <0 185 8>; /* GIC_SPI 185 = INTID 217, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&disp_clk>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\trdma0: rdma@14008000 {{
\t\t\tcompatible = \"mediatek,mt6592-disp-rdma\", \"mediatek,mt2701-disp-rdma\";
\t\t\treg = <0x14008000 0x1000>;
\t\t\tinterrupts = <0 184 8>; /* GIC_SPI 184 = INTID 216, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&disp_clk>;
\t\t\t/*
\t\t\t * No mediatek,rdma-fifo-size: mt2701_rdma_driver_data already
\t\t\t * says SZ_4K and that is the measured value. The LK never writes
\t\t\t * RDMA_FIFO_CON at all, so this came out of the stock 3.4
\t\t\t * kernel's display probe, which writes 0x81000010 to 0x14008030
\t\t\t * + 0x10 -- FIFO_UNDERFLOW_EN | ((4096 / 16) << 16) | (256 / 16).
\t\t\t */
\t\t\tstatus = \"okay\";
\t\t}};

\t\t/*
\t\t * The BLS -- the panel backlight, and the only brightness control this
\t\t * board has. j36_mt6592_backlight.ko binds this node and registers
\t\t * /sys/class/backlight/j36-backlight, which is what the dashboard's
\t\t * Display page writes to.
\t\t *
\t\t * It is NOT a pwm_chip and the #pwm-cells below is a leftover of the
\t\t * disabled pwm-backlight node further down still naming it. Registering
\t\t * a real PWM provider here would be the tidier shape and it would buy
\t\t * nothing: the one consumer on this board is the backlight itself, and
\t\t * routing it through the PWM core would put pwm-backlight's GPIO
\t\t * requirement back in the way -- see the comment on that node.
\t\t *
\t\t * The two phandles are how the driver reaches the pieces of this block
\t\t * that are not inside its own register window: the pad, because the PWM
\t\t * output only leaves the SoC when pad 90 is muxed to mode 1 and there is
\t\t * no gpiochip driver for MT6592 anywhere to ask, and MMSYS, because the
\t\t * BLS counter runs behind an MM clock gate. Both are the same
\t\t * phandle-plus-ioremap idiom j36_mt6592_input and j36_mt6592_usb_phy
\t\t * already use, and for the same reason: gpiod and the clock framework
\t\t * both defer forever against providers that bind nothing.
\t\t */
\t\tdisp_pwm: pwm@1400a000 {{
\t\t\tcompatible = \"j36,mt6592-disp-pwm\";
\t\t\treg = <0x1400a000 0x1000>;
\t\t\t#pwm-cells = <3>;
\t\t\tmediatek,pwm-pin = <{backlight_gpio}>;
\t\t\tj36,gpio-controller = <&gpio>;
\t\t\tj36,mmsys-controller = <&mmsys>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tcolor0: color@1400b000 {{
\t\t\tcompatible = \"mediatek,mt6592-disp-color\", \"mediatek,mt2701-disp-color\";
\t\t\treg = <0x1400b000 0x1000>;
\t\t\tclocks = <&disp_clk>;
\t\t\t/*
\t\t\t * No interrupts, deliberately. mtk_disp_color_probe asks for
\t\t\t * none, so declaring one would be decoration. It exists --
\t\t\t * INTID 220, GIC_SPI 188 -- and this is where to put it if
\t\t\t * something ever wants it.
\t\t\t */
\t\t\tstatus = \"okay\";
\t\t}};

\t\tdsi: dsi@1400c000 {{
\t\t\tcompatible = \"mediatek,mt6592-dsi\", \"mediatek,mt2701-dsi\";
\t\t\treg = <0x1400c000 0x1000>;
\t\t\tinterrupts = <0 189 8>; /* GIC_SPI 189 = INTID 221, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&disp_clk>, <&disp_clk>, <&dsi_phy>;
\t\t\tclock-names = \"engine\", \"digital\", \"hs\";
\t\t\tphys = <&dsi_phy>;
\t\t\tphy-names = \"dphy\";
\t\t\t#address-cells = <1>;
\t\t\t#size-cells = <0>;
\t\t\tj36,preserve-lk-state;
\t\t\tstatus = \"okay\";

\t\t\t/*
\t\t\t * The OF graph is not optional here and it is not documentation:
\t\t\t * mtk_dsi_host_attach calls devm_drm_of_get_bridge(dev,
\t\t\t * dev->of_node, 0, 0), which resolves port 0 / endpoint 0 of THIS
\t\t\t * node to the panel. A port with no reg counts as port 0, so the
\t\t\t * bare port/endpoint pair below is enough.
\t\t\t *
\t\t\t * And component_add for the DSI happens inside that same
\t\t\t * host_attach, so the DRM master never completes until the panel
\t\t\t * driver calls mipi_dsi_attach(). No panel module, no
\t\t\t * /dev/dri/card0.
\t\t\t */
\t\t\tport {{
\t\t\t\tdsi_out: endpoint {{
\t\t\t\t\tremote-endpoint = <&panel_in>;
\t\t\t\t}};
\t\t\t}};

\t\t\tpanel: panel@0 {{
\t\t\t\tcompatible = \"j36,jd9365-qc-190227\";
\t\t\t\treg = <0>;
\t\t\t\tlabel = \"J36 Ultra JD9365 QC 190227\";
\t\t\t\treset-gpios = <&gpio {reset_gpio} {GPIO_ACTIVE_LOW}>;
\t\t\t\tmediatek,power-gpios = <&gpio {power0_gpio} 0>, <&gpio {power1_gpio} 0>;
\t\t\t\t/*
\t\t\t\t * No backlight phandle, and that is a fix, not an omission.
\t\t\t\t * drm_panel_of_backlight -> of_find_backlight returns
\t\t\t\t * -EPROBE_DEFER for as long as the referenced node has not
\t\t\t\t * registered a backlight device. There is one now --
\t\t\t\t * j36_mt6592_backlight.ko on &disp_pwm -- and that is exactly
\t\t\t\t * why the phandle is still not here: that module ships in the
\t\t\t\t * power payload, so a boot without j36.power, or a card with
\t\t\t\t * j36/power/ deleted, legitimately has no backlight device. A
\t\t\t\t * phandle would make the panel -- and with it the whole DRM
\t\t\t\t * master, and /dev/dri/card0 -- conditional on a module that is
\t\t\t\t * allowed to be absent. The LK switched this backlight on before
\t\t\t\t * Linux started and nothing here turns it off, so the cost of
\t\t\t\t * leaving it unlinked is that DPMS does not dim the panel, which
\t\t\t\t * is a shell's job on this board anyway.
\t\t\t\t *
\t\t\t\t * reset-gpios and mediatek,power-gpios are the same story in
\t\t\t\t * reverse: they record what the LK does, and the panel driver
\t\t\t\t * must NOT resolve them with gpiod, because &gpio is
\t\t\t\t * j36,mt6592-gpio and has no driver either.
\t\t\t\t */
\t\t\t\tdsi,lanes = <{lanes}>;
\t\t\t\tdsi,format = <0>; /* MIPI_DSI_FMT_RGB888 */
\t\t\t\t/*
\t\t\t\t * VIDEO only -- 1, not the 5 this used to say.
\t\t\t\t *
\t\t\t\t * The LK runs this panel in SYNC_EVENT video mode: dsi_drv.c's
\t\t\t\t * g_lcm.mode is DSI_SYNC_EVENT_VDO_MODE and the measured
\t\t\t\t * DSI_MODE_CTRL handoff value is 0x2. mtk_dsi_set_mode picks
\t\t\t\t * SYNC_EVENT (2) only when MIPI_DSI_MODE_VIDEO is set and
\t\t\t\t * MIPI_DSI_MODE_VIDEO_SYNC_PULSE is clear, so asking for
\t\t\t\t * SYNC_PULSE here would have programmed mode 3 and disagreed
\t\t\t\t * with the bootloader on a panel that demonstrably works.
\t\t\t\t */
\t\t\t\tdsi,flags = <1>;  /* MIPI_DSI_MODE_VIDEO */
\t\t\t\tmediatek,dsi-pll-clock-mhz = <{pll_mhz}>;
\t\t\t\tclock-frequency = <{pixel_clock}>;
\t\t\t\thactive = <{width}>;
\t\t\t\tvactive = <{height}>;
\t\t\t\thfront-porch = <{hfp}>;
\t\t\t\thsync-len = <{hsync}>;
\t\t\t\thback-porch = <{hbp}>;
\t\t\t\tvfront-porch = <{vfp}>;
\t\t\t\tvsync-len = <{vsync}>;
\t\t\t\tvback-porch = <{vbp}>;
\t\t\t\treset-delay-ms = <10>;
\t\t\t\tinit-delay-ms = <10>;
\t\t\t\tprepare-delay-ms = <150>;
\t\t\t\tenable-delay-ms = <100>;

\t\t\t\t/* PMIC tuples are <register value mask shift>. */
\t\t\t\tmediatek,pmic-power-off-sequence = <
\t\t\t\t\t0x050c 0 1 15
\t\t\t\t\t0x0532 0 7 5
\t\t\t\t\t0x050a 0 1 15
\t\t\t\t\t0x0530 0 7 5
\t\t\t\t>;
\t\t\t\tmediatek,pmic-power-on-sequence = <
\t\t\t\t\t0x0532 3 7 5
\t\t\t\t\t0x050c 1 1 15
\t\t\t\t\t0x0530 7 7 5
\t\t\t\t\t0x050a 1 1 15
\t\t\t\t\t0x0538 6 7 5
\t\t\t\t\t0x0536 1 1 15
\t\t\t\t>;
\t\t\t\t/* GPIO tuples are <gpio level delay-after-ms>. */
\t\t\t\tmediatek,power-gpio-sequence = <
\t\t\t\t\t{reset_gpio} 0 10
\t\t\t\t\t{reset_gpio} 1 10
\t\t\t\t\t{power0_gpio} 1 10
\t\t\t\t\t{power0_gpio} 0 150
\t\t\t\t\t{power0_gpio} 1 100
\t\t\t\t\t{power1_gpio} 1 0
\t\t\t\t>;

\t\t\t\tmediatek,panel-init-record-size = <4>;
\t\t\t\tmediatek,panel-init-record-count = <{len(records)}>;
\t\t\t\tmediatek,panel-init-format = \"mtk-lcm-setting-table-v3-compact\";
\t\t\t\tpanel-init-sequence = [
{init_bytes}
\t\t\t\t];

\t\t\t\tpanel-timing {{
\t\t\t\t\tclock-frequency = <{pixel_clock}>;
\t\t\t\t\thactive = <{width}>;
\t\t\t\t\tvactive = <{height}>;
\t\t\t\t\thfront-porch = <{hfp}>;
\t\t\t\t\thsync-len = <{hsync}>;
\t\t\t\t\thback-porch = <{hbp}>;
\t\t\t\t\tvfront-porch = <{vfp}>;
\t\t\t\t\tvsync-len = <{vsync}>;
\t\t\t\t\tvback-porch = <{vbp}>;
\t\t\t\t\thsync-active = <0>;
\t\t\t\t\tvsync-active = <0>;
\t\t\t\t\tde-active = <1>;
\t\t\t\t\tpixelclk-active = <0>;
\t\t\t\t}};

\t\t\t\tport {{
\t\t\t\t\tpanel_in: endpoint {{
\t\t\t\t\t\tremote-endpoint = <&dsi_out>;
\t\t\t\t\t}};
\t\t\t\t}};
\t\t\t}};
\t\t}};

\t\tmutex: mutex@1400e000 {{
\t\t\tcompatible = \"mediatek,mt6592-disp-mutex\", \"mediatek,mt2701-disp-mutex\";
\t\t\treg = <0x1400e000 0x1000>;
\t\t\tclocks = <&disp_clk>;
\t\t\t/*
\t\t\t * No interrupts: mtk_mutex_probe asks for none. INTID 225,
\t\t\t * GIC_SPI 193, if one is ever wanted.
\t\t\t *
\t\t\t * This node is what makes the handoff exact. mt2701_mutex_mod[]
\t\t\t * numbers OVL0 = 3, COLOR0 = 7 and RDMA0 = 10, so the MOD word
\t\t\t * this driver builds for the OVL0 -> RDMA0 -> COLOR0 -> DSI0 path
\t\t\t * is 0x488, and mt2712_mutex_sof[MUTEX_SOF_DSI0] is 1 -- both the
\t\t\t * LK's measured MUTEX0_MOD and MUTEX0_SOF.
\t\t\t */
\t\t\tstatus = \"okay\";
\t\t}};
\t}};

\tsystem_clk: clock-13m {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <13000000>;
\t}};

\trtc_clk: clock-32k {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <32000>;
\t}};

\tuart_clk: clock-26m {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <26000000>;
\t}};

\t/*
\t * MSDC1 source and bus clocks.
\t *
\t * MT6592 has no clock driver in mainline, so these are fixed-clocks that
\t * describe what the bootloader already left running rather than anything
\t * this kernel can change. 200 MHz is the rate the MVII LK's own MSDC
\t * driver divides from (MSDC_SRC_CLK_HZ in mt6592_msdc_sd.c), and that
\t * driver reads this card on this board, so the number is measured and not
\t * a datasheet guess. The LK also ungates the gate this kernel cannot
\t * touch: PERI_PDN0 bit 13 for MSDC30_1, plus the CLK_CFG_3 mux and the
\t * six pad mode/drive settings. Nothing here re-does any of that.
\t *
\t * mtk-sd only takes the rate of \"source\" into its divider arithmetic;
\t * \"hclk\" it merely enables, so its declared frequency is inert.
\t */
\tmsdc1_src_clk: clock-msdc1-source {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <200000000>;
\t}};

\tmsdc1_h_clk: clock-msdc1-hclk {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <200000000>;
\t}};

\t/*
\t * One clock for the whole display pipe, and its rate is honestly zero.
\t *
\t * OVL0, RDMA0, COLOR0, MUTEX and the DSI host's \"engine\" and \"digital\"
\t * clocks are all mandatory -- each of those probes returns the error from
\t * devm_clk_get -- and every one of them is used for exactly one thing:
\t * clk_prepare_enable, which on a fixed-clock is a no-op. Not one of them is
\t * ever rate-queried. The only clk_get_rate calls in the whole mediatek DRM
\t * directory are in mtk_dpi, mtk_disp_merge and mtk_hdmi, and none of those
\t * three is on this path; the only clk_set_rate is on the DSI's \"hs\" clock,
\t * which is the MIPI-TX PHY and not this node.
\t *
\t * So there is nothing here to get right, exactly as with the Mali clocks
\t * below, and for the same reason: MT6592 has no clock driver in mainline,
\t * the LK ungated MMSYS before it drew the boot logo, and no rate in this
\t * pipe is programmable from here. A plausible megahertz number would be an
\t * invention. Six phandles into one node rather than six identical nodes,
\t * because six copies of nothing is still nothing.
\t */
\tdisp_clk: clock-disp {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <0>;
\t}};

\t/*
\t * The MIPI-TX PLL's reference, and this one is a real number.
\t *
\t * mtk_mipi_tx_probe takes it with devm_clk_get(dev, NULL) and uses only
\t * __clk_get_name() on it, to name the PLL's parent; the PCW arithmetic in
\t * mt8173_mipi_tx_pll_prepare hardcodes 26000000 rather than reading it. It
\t * is still 26 MHz and not zero, because that hardcoded reference is the one
\t * this SoC actually has: mainline computes
\t * pcw = ((data_rate * 2 * txdiv) << 24) / 26000000 and the LK computes
\t * pcw = data_Rate * txdiv / 13 with data_Rate in MHz, which is the same
\t * quotient, and the two txdiv ladders agree at every step.
\t */
\tdsi_ref_clk: clock-dsi-ref {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <26000000>;
\t}};

\t/*
\t * The Mali-450's two clocks, and their rate is honestly zero.
\t *
\t * lima requires \"bus\" and \"core\": lima_clk_init() returns the error from
\t * devm_clk_get() for either, so a node without them does not probe. What it
\t * does with them is clk_prepare_enable -- a no-op on a fixed-clock -- and
\t * two dev_info lines, \"bus rate\" and \"mod rate\". Nothing else reads the
\t * rate, because devfreq is skipped outright when operating-points-v2 is
\t * absent (lima_devfreq_init returns 0 on !device_property_present), and this
\t * node has no OPP table.
\t *
\t * So there is nothing here to get right and nothing to measure: MT6592 has
\t * no clock driver in mainline, the MFG clock is ungated by writing bit 0 of
\t * MFG_CG_CLR at 0x13000008 and no rate is ever programmed by anything in
\t * this boot path, and the frequency of that gate's parent mux is not known.
\t * A plausible-looking megahertz number would be an invention, so these say
\t * 0 -- which lima prints and never uses.
\t */
\tmali_bus_clk: clock-mali-bus {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <0>;
\t}};

\tmali_core_clk: clock-mali-core {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <0>;
\t}};

\t/*
\t * The card's 3.3 V rail, as a fixed regulator.
\t *
\t * This is not decoration. mmc_regulator_get_supply() is the ONLY thing in
\t * the MMC core that ever assigns mmc->ocr_avail (drivers/mmc/core/
\t * regulator.c), and mtk-sd never sets it itself. With no vmmc-supply the
\t * host comes up with ocr_avail == 0, mmc_power_up() offers the card no
\t * voltage at all, and the SD attach fails with nothing in the log that
\t * points at the cause.
\t *
\t * always-on/boot-on because the rail is genuinely already up: MT6323's
\t * VMC/VMCH were switched on by the LK before it read /mvii/boot.conf off
\t * this same card. There is no MT6323 regulator driver in this profile to
\t * turn it off, and describing it as switchable would be a lie.
\t */
\tmsdc1_vmmc: regulator-msdc1-vmmc {{
\t\tcompatible = \"regulator-fixed\";
\t\tregulator-name = \"vmc-3v3\";
\t\tregulator-min-microvolt = <3300000>;
\t\tregulator-max-microvolt = <3300000>;
\t\tregulator-always-on;
\t\tregulator-boot-on;
\t}};

\t/*
\t * MSDC1 -- the microSD slot, and the only path to the shared rootfs.
\t *
\t * Every number here was measured, not assumed:
\t *
\t *   reg 0x11240000  The MVII LK's SD driver drives this base and reads
\t *                   this card (mt6592_msdc_sd.c). MSDC0 at 0x11230000 is
\t *                   the eMMC and is deliberately left out of this tree.
\t *
\t *   interrupts 72   The stock MT6592 kernel carries a platform resource
\t *                   array in .data at 0xc0b33050..0xc0b331c4 holding
\t *                   {{start, end, name, IORESOURCE_MEM}} followed by
\t *                   {{irq, 0, name, IORESOURCE_IRQ}}. It gives MSDC0 IRQ
\t *                   103, MSDC1 104, MSDC3 105.
\t *
\t *                   Those are GIC interrupt IDs, so the SPI number is 32
\t *                   lower -- see the note on the timer node above, which
\t *                   proves the -32 on an interrupt this board is already
\t *                   fielding. 104 - 32 = 72.
\t *
\t *                   This said 40 for one boot, from trusting mainline
\t *                   mt6592.dtsi's UART SPIs (51..54 against the stock
\t *                   kernel's 115..118, implying -64). Every request then
\t *                   died on mtk-sd's 5-second software watchdog with
\t *                   host->error=0x2 REQ_CMD_TMO and no "CMD bus busy"
\t *                   line -- the command reached the controller and no
\t *                   interrupt ever came back. A card that simply does not
\t *                   answer raises MSDC_INT_CMDTMO in milliseconds; only a
\t *                   misrouted IRQ is silent for a full five seconds.
\t *
\t *   compatible      mt6592-mmc, added by 0001-mtk-sd-mt6592.patch. MT6592
\t *                   is a 12-bit-divider part: the LK writes CKDIV in
\t *                   MSDC_CFG[19:8] and CKMOD in [21:20], which is mtk-sd's
\t *                   clk_div_bits == 12 layout, not the 8-bit mt8135 one.
\t *                   No existing compatible pairs a 12-bit divider with
\t *                   this generation's pad-tune register, hence the patch.
\t *
\t * 25 MHz to start with: default speed, no high-speed timing to get wrong
\t * while the pad tuning is unproven. The LK runs the same card at 13 MHz.
\t * Raise to 50 MHz with cap-sd-highspeed once a filesystem has survived
\t * real traffic here.
\t *
\t * non-removable is a statement about this kernel, not about the slot: card
\t * detect is a GPIO, MT6592 has no pinctrl/GPIO driver in mainline, and
\t * root lives on this card anyway. There is nothing useful to do if it goes
\t * away mid-run.
\t */
\tmmc1: mmc@11240000 {{
\t\tcompatible = \"mediatek,mt6592-mmc\";
\t\treg = <0x11240000 0x1000>;
\t\tinterrupts = <0 72 8>; /* GIC_SPI 72 = INTID 104, IRQ_TYPE_LEVEL_LOW */
\t\tclocks = <&msdc1_src_clk>, <&msdc1_h_clk>;
\t\tclock-names = \"source\", \"hclk\";
\t\tvmmc-supply = <&msdc1_vmmc>;
\t\tmax-frequency = <25000000>;
\t\tbus-width = <4>;
\t\tno-1-8-v;
\t\tno-mmc;
\t\tno-sdio;
\t\tnon-removable;
\t\tdisable-wp;
\t\tstatus = \"okay\";
\t}};

\t/*
\t * The USB controller, and there is exactly one on this SoC.
\t *
\t * MT6592 carries a Mentor MUSBMHDRC dual-role core at 0x11200000 and no
\t * EHCI, OHCI or XHCI anywhere. That is measured, not assumed: MVII's own
\t * mt6592_musb.c drives this window in device mode on this board, and the
\t * stock Android kernel's mt_usb driver hardcodes the same base -- its
\t * inlined musb_init_controller at 0xc053cf94 builds it with
\t * `mov r3,#0 / movt r3,#0xf120' and stores it to both musb->mregs and
\t * musb->ctrl_base, 0xf1200000 being the stock kernel's static mapping of
\t * 0x11200000.
\t *
\t * mediatek,mtk-musb is the only compatible drivers/usb/musb/mediatek.c
\t * matches; the mt6592 string in front of it is documentation. The glue is
\t * the MT2701/MT8173 generation and is reused unmodified -- the same
\t * argument as mtk_drm, and unlike mtk_drm it needed no patch at all.
\t *
\t * dr_mode = \"host\" is a decision about power, not about the port. The core
\t * is dual-role and the socket is OTG, but sourcing VBUS means the MT6322
\t * boost, which nothing in this runtime enables. So the board never drives
\t * VBUS and THE HUB HAS TO BE POWERED. mediatek.c reads this property with
\t * usb_get_dr_mode() and refuses to probe if it is missing.
\t *
\t * interrupt-names = \"mc\" IS MANDATORY and is the one line here that will
\t * not announce itself if it goes missing. mediatek.c copies this node's
\t * resources into a child platform device called musb-hdrc, and musb_core's
\t * musb_probe() opens with
\t *
\t *     int irq = platform_get_irq_byname(pdev, \"mc\");
\t *
\t * -- by name, not by index. of_irq_to_resource() names an IRQ resource from
\t * interrupt-names when the property is there and from the node's full name
\t * when it is not, so without this line the lookup returns -ENXIO and the
\t * controller never probes, with a message that says nothing about names.
\t *
\t * ── The interrupt number, which is the one thing here that is a guess ─────
\t *
\t * Every other SPI in this file was read out of the stock kernel. This one
\t * could not be, and the search was run to exhaustion: mt_usb's
\t * platform_device at 0xc0b31048 has num_resources == 0 and resource ==
\t * NULL -- MediaTek's usb20 driver passes no resources at all -- the base is
\t * hardcoded as above rather than coming from a resource array, nIrq is
\t * initialised to -ENODEV and never assigned a constant in that function,
\t * and the literal 0x11200000 does not occur once as a 32-bit word anywhere
\t * in the 12 MB image. There is no MEM+IRQ resource pair for USB to read.
\t *
\t * 64 is therefore an extrapolation, and a thin one. The only other
\t * MediaTek interrupt table in this source tree is the MT6735 LK's
\t * mt_irq.h, which has USB0 = 104 and MSDC0 = 111; MT6592's MSDC0 is INTID
\t * 103, eight below MT6735's, so shifting USB0 by the same eight gives INTID
\t * 96 and SPI 64. One anchor, across two SoC generations. Treat it as a
\t * placeholder.
\t *
\t * Being wrong here is safe, which is why shipping a guess is acceptable:
\t * musb requests an interrupt that never fires, enumeration times out, and
\t * nothing crashes. It is also self-diagnosing. j36_mt6592_usb_phy scans
\t * GICD_ISPENDR after power-on and prints every SPI that became pending, and
\t * a wrong number here is exactly what makes the measurement work -- MUSB's
\t * real line is level-sensitive, so with no handler registered on it the GIC
\t * latches it pending and leaves it there. One boot log names the number.
\t */
\tusb0: usb@11200000 {{
\t\tcompatible = \"mediatek,mt6592-musb\", \"mediatek,mtk-musb\";
\t\treg = <0x11200000 0x1000>;
\t\tinterrupts = <0 64 8>; /* GUESS: SPI 64 = INTID 96, IRQ_TYPE_LEVEL_LOW */
\t\tinterrupt-names = \"mc\";
\t\tclocks = <&usb_clk>, <&usb_clk>, <&usb_clk>;
\t\tclock-names = \"main\", \"mcu\", \"univpll\";
\t\tphys = <&usb_phy>;
\t\tdr_mode = \"host\";
\t\tstatus = \"okay\";
\t}};

\t/*
\t * The U2 PHY, as a generic-PHY provider, because mediatek.c takes it with
\t * devm_of_phy_get_by_index(dev, np, 0) and will not probe without one.
\t *
\t * 0x11210800 is the USB PHY block inside the SIFSLV window at 0x11210000,
\t * and the register sequence j36_mt6592_usb_phy runs on it is transcribed
\t * byte for byte out of the stock LK's usb_phy_recover() at 0x81e09520 by
\t * way of MVII's mt6592_musb.c, which uses it to enumerate on this board.
\t *
\t * j36,pericfg-controller is not optional and not a convenience. The PHY and
\t * the controller are both behind the PERI clock gate at 0x10003010
\t * (PDN_CLR) / 0x10003018 (PDN_STA), and an APB read of a gated MediaTek
\t * peripheral does not fault -- it stalls the bus until the watchdog fires.
\t * The gate is cleared in this PHY's .init, which musb_platform_init calls
\t * before any MUSB register is touched, and that ordering is the whole
\t * reason the PHY is a separate driver rather than three writes inside the
\t * glue.
\t *
\t * j36,gic-controller is read-only and exists for the measurement described
\t * on the usb0 node above: the module maps the distributor to sample
\t * GICD_ISPENDR, never to write it. GICD_ISPENDR reports the pending state
\t * of a level-sensitive line whether or not it is enabled, so an unclaimed
\t * MUSB interrupt shows up in it.
\t *
\t * j36,drvvbus-pad is the 5 V. It is not a PMIC boost on this board: the
\t * stock Android kernel's mt_usb_set_vbus() -- 0xc052e938, line 60, found
\t * through the __func__ pointer in its own printk -- is four instructions
\t * long on the `on' path and they are
\t *
\t *   mt_set_gpio_mode(0x8000000f, 0);   // ops slot 0x3c, writes 0x10005600
\t *   mt_set_gpio_out (0x8000000f, 1);   // ops slot 0x30, writes 0x10005400
\t *
\t * 0x80000000 is MTK's GPIO_..._PIN marker, which the wrappers strip before
\t * bounds-checking the pin against 0xa8, so the pad is 15. The two callees
\t * were identified from the ops table rather than from their names: slot
\t * 0x3c divides by five and writes a 3-bit field at base+0x600, which is
\t * MODE, and slot 0x30 writes the SET/RST alias of base+0x400, which is
\t * DOUT. Both bases match the map mt6592_led.c already drives on this board.
\t * The pad is active high. Only two functions in the whole image mention pin
\t * 15, mt_usb_set_vbus and the drvvbus pad setup next to it, so nothing else
\t * on this board wants it.
\t *
\t * There is no gpiochip driver for MT6592 in mainline, which is why this is
\t * a plain pad number and a phandle to the register block rather than a
\t * gpios = <&pio 15 ...> the way a board with pinctrl would write it -- the
\t * same reason j36_mt6592_input takes j36,gpio-controller and does its own
\t * bank arithmetic. It is called -pad and not -gpio for two reasons: it
\t * matches j36,kpd-strobe-pads on the keypad node, which is the same kind of
\t * number, and dtc's gpios_property check reads any *-gpio property as a
\t * phandle-and-cells specifier, so <15> under that name warns about a bad
\t * phandle into whatever node happens to hold phandle 15.
\t *
\t * READ THIS BEFORE TURNING IT ON WITH NO CELL FITTED. The 5 V comes from a
\t * boost off VBAT, and VBAT on this PMIC is the system node, not a
\t * battery-only rail. A bus-powered hub is the same class of load as the
\t * class-D amp, which MVII measured pulling VBAT under the undervoltage
\t * lockout on a cell-less board. Delete the property, or pass vbus=0 to the
\t * module, and the port is back to the state it shipped in here.
\t */
\tusb_phy: usb-phy@11210800 {{
\t\tcompatible = \"j36,mt6592-usb-phy\";
\t\treg = <0x11210800 0x100>;
\t\tj36,pericfg-controller = <&pericfg>;
\t\tj36,gic-controller = <&gic>;
\t\tj36,gpio-controller = <&gpio>;
\t\tj36,drvvbus-pad = <15>;
\t\t#phy-cells = <0>;
\t\tstatus = \"okay\";
\t}};

\t/*
\t * MUSB's three clocks, and their rate is honestly zero -- the same case as
\t * disp_clk and the two Mali clocks above, for the same reason.
\t *
\t * mediatek.c's mtk_musb_clks_get() names them \"main\", \"mcu\" and
\t * \"univpll\" and takes all three with devm_clk_bulk_get(), so a node
\t * missing any one of them does not probe. What it does with them is
\t * clk_bulk_prepare_enable(), which on a fixed-clock is a no-op; nothing in
\t * the glue or in musb_core ever asks for a rate. MT6592 has no clock driver
\t * in mainline, so there is nothing here that could hand out a real one, and
\t * a plausible megahertz number would be an invention.
\t *
\t * What actually ungates USB is the PHY driver clearing the PERI gate, which
\t * is why this node can be inert without USB being dead.
\t *
\t * One node, three phandles, following disp_clk: three copies of nothing is
\t * still nothing. clk_bulk_prepare_enable on the same clk three times is
\t * three increments of one enable count on a clock that is always on.
\t */
\tusb_clk: clock-usb {{
\t\tcompatible = \"fixed-clock\";
\t\t#clock-cells = <0>;
\t\tclock-frequency = <0>;
\t}};

\t/*
\t * The MT6323 PMIC: battery gauge, charger and power-off.
\t *
\t * No reg, for the same reason gamepad-input below has none. This driver owns
\t * no window at all -- the PMIC is not on any bus Linux can see, and every one
\t * of its registers is reached by handing a 16-bit address to the PMIC wrapper
\t * and waiting for the bridge to retire it. The four phandles are the four
\t * blocks it borrows, and j36,pwrap-controller is the only one it cannot work
\t * without: the AUXADC channels the gauge samples, the CHR_CON charger bank it
\t * arms, the BC1.2 comparator that decides how many milliamps the wall will
\t * give us, and the RTC latch that cuts the rail on poweroff are all on the
\t * far side of it.
\t *
\t * j36,usb-phy-controller and j36,pericfg-controller are a pair, and a node
\t * that names one without the other gets neither. BC1.2 needs exactly one bit
\t * on the SoC side -- rg_usb20_gpio_ctl in the PHY's 0x1a, which hands D+/D-
\t * to the PMIC's comparator -- and that register is behind the PERI clock gate
\t * at 0x10003010. Reading a gated MediaTek peripheral does not fault; it
\t * stalls the APB until the watchdog resets the board, so the driver clears
\t * the gate itself before the first access and skips charger detection
\t * entirely if it was not given the means to. The cost of skipping it is one
\t * conservative input limit, not a dead port.
\t *
\t * j36,gpio-controller with j36,drvvbus-pad is the host-mode interlock, and it
\t * is the same pad 15 as on usb_phy above, deliberately duplicated rather than
\t * shared through a driver-to-driver call. When the port is a host, that pad
\t * drives our own 5 V onto VBUS, and CHRDET inside the PMIC cannot tell our
\t * boost from a charger -- it would arm the charger against a rail we are
\t * sourcing. So the PMIC reads the pad's mode, direction and output value
\t * directly, three register reads a second, and while it is asserted the
\t * charger stays disarmed and the supply reports offline. Reading a pad costs
\t * nothing and creates no ordering between the two modules; a symbol
\t * dependency would.
\t *
\t * poll-interval-ms is the gauge cadence. A second is slow enough that the
\t * AUXADC work is invisible and fast enough that the coulomb integrator, which
\t * multiplies the measured current by the time since the previous sample,
\t * never has to trust a long gap. The driver clamps whatever it is given to
\t * 200..10000 ms, and a poll_ms= module argument overrides this.
\t *
\t * status is \"okay\" and costs nothing on a boot without the j36.power word,
\t * the same as audio and the panel: with no module loaded there is nothing to
\t * bind to the node.
\t *
\t * ONE FACT ABOUT THIS BOARD EXPLAINS MOST OF THE DRIVER. There is no
\t * power-path FET, so VBAT is the system node, not a battery-only rail. Every
\t * live ADC channel measures what the whole board is doing, the terminal
\t * voltage moves with the amplifier and the backlight rather than with charge
\t * state, and a cell-less board sitting on a charger reads a full battery.
\t * That is why the gauge is seeded from the hardware OCV latch the PMIC
\t * captured at wakeup, why current comes from a differential across the sense
\t * resistor rather than from the rail, why the charge voltage is only ever
\t * raised, and why the battery supply has no PRESENT property to lie with.
\t */
\tpmic: pmic {{
\t\tcompatible = \"j36,j36-ultra-pmic\";
\t\tj36,pwrap-controller = <&pwrap>;
\t\tj36,usb-phy-controller = <&usb_phy>;
\t\tj36,pericfg-controller = <&pericfg>;
\t\tj36,gpio-controller = <&gpio>;
\t\tj36,drvvbus-pad = <15>;
\t\tpoll-interval-ms = <1000>;
\t\tstatus = \"okay\";
\t}};

\t/*
\t * The Mali-450 MP4, for DRM lima.
\t *
\t * Every number here was read out of hardware descriptions this board
\t * already carries, and the base address happens to be attested twice by
\t * sources that never saw each other:
\t *
\t *   The stock MT6592 kernel carries the same kind of platform resource array
\t *   in .data that MSDC1's interrupt above came from, at
\t *   0xc0b71de4..0xc0b720f3 (28-byte {{start, end, name, flags}} entries,
\t *   IORESOURCE_MEM 0x200 / IORESOURCE_IRQ 0x400). Named, it reads:
\t *
\t *     Mali_L2           0x13050000  Mali_GP            0x13040000  IRQ 234
\t *     Mali_GP_MMU       0x13043000  IRQ 235             Mali_L2     0x13041000
\t *     Mali_PP0          0x13048000  IRQ 236             Mali_PP0_MMU 0x13044000  IRQ 237
\t *     Mali_PP1          0x1304a000  IRQ 238             Mali_PP1_MMU 0x13045000  IRQ 239
\t *     Mali_PP2          0x1304c000  IRQ 240             Mali_PP2_MMU 0x13046000  IRQ 241
\t *     Mali_PP3          0x1304e000  IRQ 242             Mali_PP3_MMU 0x13047000  IRQ 243
\t *     Mali_Broadcast    0x13053000  Mali_DLBU          0x13054000
\t *     Mali_PP_Broadcast 0x13056000  IRQ 244             Mali_PP_MMU_Broadcast 0x13055000
\t *     Mali_DMA          0x13052000
\t *
\t *   The MVII LK's own bare-metal Utgard driver -- mt6592_gpu_offload.c,
\t *   which powers this GPU, submits a PLBU job and renders a self-test on
\t *   PP0 -- declares MALI_BASE = 0x13040000 with GP at +0x00000, the GP MMU
\t *   at +0x03000, PP0 at +0x08000, PP0's MMU at +0x04000 and the two L2s at
\t *   +0x10000 and +0x01000. Silicon, not a datasheet reading.
\t *
\t * Both agree with lima's own mali450 offset column in lima_device.c, block
\t * for block: gp 0x00000, l2_cache1 0x01000, pmu 0x02000, gpmmu 0x03000,
\t * ppmmu0..3 0x04000..0x07000, pp0..3 0x08000/0x0a000/0x0c000/0x0e000,
\t * l2_cache0 0x10000, bcast 0x13000, dlbu 0x14000, ppmmu_bcast 0x15000,
\t * pp_bcast 0x16000. That is the whole of what this driver maps for a 450,
\t * and 0x30000 covers it with room past the vendor map's last register at
\t * +0x17100.
\t *
\t * The interrupt numbers are those INTIDs minus 32, the same conversion the
\t * MSDC1 node above documents and this board is already fielding: GP 234-32
\t * = 202 through PP_Broadcast 244-32 = 212. IRQ_TYPE_LEVEL_LOW for the same
\t * reason every other node here uses it.
\t *
\t * lima insists on five of these by name -- gp, gpmmu, pp0, ppmmu0 and pp
\t * (the PP broadcast) are must_have for a 450 and take the whole probe down
\t * if platform_get_irq_byname fails; the rest go through
\t * platform_get_irq_byname_optional. All eleven the hardware has are here.
\t *
\t * status is okay and yet nothing probes at boot: CONFIG_DRM_LIMA is =m on
\t * purpose. The MFG power domain is gated when Linux starts -- nothing on
\t * this boot path calls the LK's mfg_power_on() -- and the first register
\t * read into an unpowered MTK subsystem stalls the bus rather than returning
\t * anything, which is a silent watchdog reboot with no log. So the module is
\t * loaded from /init only after j36/mfgpower has powered the domain and read
\t * back the GP and PP version registers. See the lima section of
\t * build-in-vm.sh.
\t */
\tgpu: gpu@13040000 {{
\t\tcompatible = \"arm,mali-450\";
\t\treg = <0x13040000 0x30000>;
\t\tinterrupts = <0 202 8>, /* GIC_SPI 202 = INTID 234, Mali_GP */
\t\t\t     <0 203 8>, /* 235, Mali_GP_MMU */
\t\t\t     <0 204 8>, /* 236, Mali_PP0 */
\t\t\t     <0 205 8>, /* 237, Mali_PP0_MMU */
\t\t\t     <0 206 8>, /* 238, Mali_PP1 */
\t\t\t     <0 207 8>, /* 239, Mali_PP1_MMU */
\t\t\t     <0 208 8>, /* 240, Mali_PP2 */
\t\t\t     <0 209 8>, /* 241, Mali_PP2_MMU */
\t\t\t     <0 210 8>, /* 242, Mali_PP3 */
\t\t\t     <0 211 8>, /* 243, Mali_PP3_MMU */
\t\t\t     <0 212 8>; /* 244, Mali_PP_Broadcast */
\t\tinterrupt-names = \"gp\", \"gpmmu\",
\t\t\t\t  \"pp0\", \"ppmmu0\",
\t\t\t\t  \"pp1\", \"ppmmu1\",
\t\t\t\t  \"pp2\", \"ppmmu2\",
\t\t\t\t  \"pp3\", \"ppmmu3\",
\t\t\t\t  \"pp\";
\t\tclocks = <&mali_bus_clk>, <&mali_core_clk>;
\t\tclock-names = \"bus\", \"core\";
\t\tstatus = \"okay\";
\t}};

\t/*
\t * Disabled, superseded, and kept only as the record of what the hardware
\t * is wired like.
\t *
\t * There IS a backlight device on this board now -- j36_mt6592_backlight.ko
\t * binds &disp_pwm directly and registers it -- but it is emphatically not
\t * this node. pwm-backlight consumes two providers that still do not exist
\t * here: a pwm_chip on &disp_pwm, which that driver does not register, and
\t * a gpiochip on &gpio, which is j36,mt6592-gpio and binds nothing. An
\t * enabled consumer with an unresolvable provider does not fail -- it
\t * defers, and keeps deferring until driver_deferred_probe_timeout expires.
\t * That is the ten-second gap between the last input message at 0.87 s and
\t *
\t *   [ 11.381612] platform backlight: deferred probe pending:
\t *                pwm-backlight: failed to acquire enable GPIO
\t *
\t * on a board whose backlight the LK already switched on before Linux
\t * started. The node bought nothing and cost ten seconds of every boot.
\t *
\t * brightness-levels stays because it is the measured scale: ten steps of a
\t * 1023-count duty, which is exactly the range the driver exposes as
\t * max_brightness.
\t */
\tbacklight: backlight {{
\t\tcompatible = \"pwm-backlight\";
\t\tpwms = <&disp_pwm 0 30000 0>;
\t\tbrightness-levels = <0 64 128 256 384 512 640 768 896 1023>;
\t\tdefault-brightness-level = <7>;
\t\tenable-gpios = <&gpio {backlight_gpio} 0>;
\t\tstatus = \"disabled\";
\t}};

\tframebuffer@{fb_addr:08x} {{
\t\tcompatible = \"simple-framebuffer\";
\t\treg = <0x{fb_addr:08x} 0x{width * height * 4:08x}>;
\t\twidth = <{width}>;
\t\theight = <{height}>;
\t\tstride = <{fb_pitch}>;
\t\tformat = \"x8r8g8b8\";
\t\tstatus = \"okay\";
\t}};

\tj36_input: gamepad-input {{
\t\tcompatible = \"j36,j36-ultra-input\";
\t\tj36,gpio-controller = <&gpio>;
\t\tj36,keypad-controller = <&keypad>;
\t\tj36,auxadc-controller = <&auxadc>;
\t\tj36,pericfg-controller = <&pericfg>;
\t\t/*
\t\t * The keypad's 32 kHz clock is not on the SoC side at all: it is one bit
\t\t * of MT6323 register 0x40, reached over PWRAP. This is the step whose
\t\t * absence is invisible from here -- with the clock gated the scan engine
\t\t * sits with KP_EN set, KP_DEBOUNCE loaded and every scan memory reading
\t\t * its idle all-ones pattern, which is indistinguishable from a correctly
\t\t * configured matrix that nobody is pressing. A live capture showed exactly
\t\t * that: en=1, deb=0x400, mem ffff ffff ffff ffff 00ff, before and after a
\t\t * verified-correct pad mux.
\t\t */
\t\tj36,pwrap-controller = <&pwrap>;
\t\tpoll-interval-ms = <5>;
\t\tj36,direct-key-map = <
{direct_map_cells}
\t\t>;
\t\tj36,matrix-key-map = <
{matrix_map_cells}
\t\t>;
\t\t/* Triples are <AUXADC channel Linux ABS code inverted>. */
\t\tj36,axis-map = <
{axis_map_cells}
\t\t>;
\t\tj36,raw-min = <800>;
\t\tj36,raw-max = <3900>;
\t\tj36,fallback-center = <{joy_center}>;
\t\tj36,deadzone = <{joy_deadzone}>;
\t\tstatus = \"okay\";
\t}};

\tgamepad-gpio-keys {{
\t\tcompatible = \"gpio-keys-polled\";
\t\tpoll-interval = <5>;
\t\tstatus = \"disabled\";
{chr(10).join(gpio_children)}
\t}};

\tgamepad-analog {{
\t\tcompatible = \"adc-joystick\";
\t\tio-channels = <&auxadc {joy_x}>, <&auxadc {joy_y}>,
\t\t              <&auxadc {joy_z}>, <&auxadc {joy_rz}>;
\t\t#address-cells = <1>;
\t\t#size-cells = <0>;
\t\tmediatek,fallback-center = <{joy_center}>;
\t\tstatus = \"disabled\";

\t\taxis@0 {{
\t\t\treg = <0>;
\t\t\tlinux,code = <{ABS_X}>;
\t\t\tabs-range = <3900 800>;
\t\t\tabs-flat = <{joy_deadzone}>;
\t\t}};
\t\taxis@1 {{
\t\t\treg = <1>;
\t\t\tlinux,code = <{ABS_Y}>;
\t\t\tabs-range = <3900 800>;
\t\t\tabs-flat = <{joy_deadzone}>;
\t\t}};
\t\taxis@2 {{
\t\t\treg = <2>;
\t\t\tlinux,code = <{ABS_Z}>;
\t\t\tabs-range = <800 3900>;
\t\t\tabs-flat = <{joy_deadzone}>;
\t\t}};
\t\taxis@3 {{
\t\t\treg = <3>;
\t\t\tlinux,code = <{ABS_RZ}>;
\t\t\tabs-range = <800 3900>;
\t\t\tabs-flat = <{joy_deadzone}>;
\t\t}};
\t}};
}};
"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--drivers", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    sources = read_sources(args.drivers)
    output = generate(sources)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()
