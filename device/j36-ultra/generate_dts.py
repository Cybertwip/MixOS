#!/usr/bin/env python3
"""Generate the J36 Ultra MT6592 bring-up DTS from the MVII driver sources.

The panel timing, power GPIOs, compact 155-record JD9365 command table, keypad
matrix, direct GPIO keys, framebuffer and AUXADC channels are parsed from:
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


def parse_int(text: str, name: str) -> int:
    patterns = (
        rf"\b{name}\s*=\s*(-?(?:0x[0-9a-fA-F]+|[0-9]+))u?\b",
        rf"#define\s+{name}\s+(-?(?:0x[0-9a-fA-F]+|[0-9]+))u?\b",
    )
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return int(match.group(1), 0)
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

\t\ttimer: timer@10008000 {{
\t\t\tcompatible = \"mediatek,mt6577-timer\";
\t\t\treg = <0x10008000 0x80>;
\t\t\tinterrupts = <0 144 8>; /* GIC_SPI, IRQ_TYPE_LEVEL_LOW */
\t\t\tclocks = <&system_clk>, <&rtc_clk>;
\t\t\tclock-names = \"system-clk\", \"rtc-clk\";
\t\t}};

\t\tuart0: serial@11002000 {{
\t\t\tcompatible = \"mediatek,mt6577-uart\";
\t\t\treg = <0x11002000 0x400>;
\t\t\tinterrupts = <0 51 8>; /* GIC_SPI, IRQ_TYPE_LEVEL_LOW */
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
\t\t\tstatus = \"okay\";
\t\t}};

\t\tmmsys: syscon@14000000 {{
\t\t\tcompatible = \"j36,mt6592-mmsys\", \"syscon\";
\t\t\treg = <0x14000000 0x1000>;
\t\t}};

\t\tdsi_phy: phy@10010000 {{
\t\t\tcompatible = \"j36,mt6592-mipi-tx\";
\t\t\treg = <0x10010000 0x1000>;
\t\t\t#phy-cells = <0>;
\t\t\tmediatek,efuse-trim-reg = <0x10206180>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tdisp_pwm: pwm@1400a000 {{
\t\t\tcompatible = \"j36,mt6592-disp-pwm\";
\t\t\treg = <0x1400a000 0x1000>;
\t\t\t#pwm-cells = <3>;
\t\t\tmediatek,pwm-pin = <{backlight_gpio}>;
\t\t\tstatus = \"okay\";
\t\t}};

\t\tdsi: dsi@1400c000 {{
\t\t\tcompatible = \"j36,mt6592-dsi\";
\t\t\treg = <0x1400c000 0x1000>;
\t\t\tphys = <&dsi_phy>;
\t\t\tphy-names = \"dphy\";
\t\t\t#address-cells = <1>;
\t\t\t#size-cells = <0>;
\t\t\tstatus = \"okay\";

\t\t\tpanel: panel@0 {{
\t\t\t\tcompatible = \"j36,jd9365-qc-190227\";
\t\t\t\treg = <0>;
\t\t\t\tlabel = \"J36 Ultra JD9365 QC 190227\";
\t\t\t\treset-gpios = <&gpio {reset_gpio} {GPIO_ACTIVE_LOW}>;
\t\t\t\tmediatek,power-gpios = <&gpio {power0_gpio} 0>, <&gpio {power1_gpio} 0>;
\t\t\t\tbacklight = <&backlight>;
\t\t\t\tdsi,lanes = <{lanes}>;
\t\t\t\tdsi,format = <0>; /* MIPI_DSI_FMT_RGB888 */
\t\t\t\tdsi,flags = <5>;  /* VIDEO | VIDEO_SYNC_PULSE */
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
\t\t\t}};
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

\tbacklight: backlight {{
\t\tcompatible = \"pwm-backlight\";
\t\tpwms = <&disp_pwm 0 30000 0>;
\t\tbrightness-levels = <0 64 128 256 384 512 640 768 896 1023>;
\t\tdefault-brightness-level = <7>;
\t\tenable-gpios = <&gpio {backlight_gpio} 0>;
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

\tgamepad-gpio-keys {{
\t\tcompatible = \"gpio-keys-polled\";
\t\tpoll-interval = <5>;
{chr(10).join(gpio_children)}
\t}};

\tgamepad-analog {{
\t\tcompatible = \"adc-joystick\";
\t\tio-channels = <&auxadc {joy_x}>, <&auxadc {joy_y}>,
\t\t              <&auxadc {joy_z}>, <&auxadc {joy_rz}>;
\t\t#address-cells = <1>;
\t\t#size-cells = <0>;
\t\tmediatek,fallback-center = <{joy_center}>;

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
