#!/usr/bin/env python3
"""Generate the LG K20 (MT6739) bring-up DTS.

Sibling-shaped to device/oppo-mt6877/generate_dts_oppo.py: the tests
import ``read_sources`` / ``generate``; the CLI writes a file.

Geometry comes from devices.sh (single source of truth). Anything the
phone has not told us yet is a comment, not a node: exact-SoC
compatibles only (per-device, not family wildcards), no invented
register addresses, no framebuffer until BRINGUP step 2 measures one.
"""
import argparse
import re
import subprocess
from pathlib import Path

TREE = Path(__file__).resolve().parent

# Exact-SoC compatibles claimed per device (bring-up prosthetic; the real
# mt6739.dtsi does not exist in mainline 6.12 yet).
COMPATIBLES = [
    "lg,lm-x120",
    "mediatek,mt6739",
]

REQUIRED_BOOTARGS = ["earlycon", "lg.device="]

FB_NODE_TMPL = """\
\tchosen_fb: framebuffer@{fb_base:x} {{
\t\tcompatible = "simple-framebuffer";
\t\treg = <0x0 0x{fb_base:x} 0x0 0x{fb_size_bytes:x}>;
\t\twidth = <{width}>;
\t\theight = <{height}>;
\t\tstride = <({width} * 4)>;
\t\tformat = "a8r8g8b8";
\t}};
"""


def read_sources(tree: Path = TREE) -> dict:
    """Load per-device geometry from devices.sh via the shell."""
    devices = tree / "devices.sh"
    probe = subprocess.run(
        ["sh", "-c", f". {devices} && eval \"$(k20_device_info lm-x120)\" && "
                     "echo \"$K20_SOC $K20_MEM_MB $K20_WIDTH $K20_HEIGHT\""],
        capture_output=True, text=True, check=True)
    soc, mem_mb, width, height = probe.stdout.split()
    return {"lm-x120": {"soc": soc, "mem_mb": int(mem_mb),
                        "width": int(width), "height": int(height)}}


def generate(sources: dict, device: str, fb_base: str = "") -> str:
    spec = sources[device]  # KeyError: unknown device, callers load devices.sh
    width, height, mem_mb = spec["width"], spec["height"], spec["mem_mb"]
    mem_bytes = mem_mb * 1024 * 1024

    if fb_base:
        fb = int(fb_base, 0)
        fb_size = width * height * 4
        fb_node = FB_NODE_TMPL.format(fb_base=fb, fb_size_bytes=fb_size,
                                      width=width, height=height)
    else:
        fb_node = ("\t/* No simple-framebuffer node: the LK framebuffer base\n"
                   "\t * is unknown (BRINGUP step 2). Re-run with --fb-base.\n"
                   "\t */\n")

    lines = [
        "/dts-v1/;",
        "",
        f"/ {{ /* {device}: {spec['soc']}, {mem_mb} MiB */",
        f'\tmodel = "LG {device.upper()} (MT6739 bring-up)";',
        '\tcompatible = "lg,lm-x120", "mediatek,mt6739";',
        "\t#address-cells = <2>;",
        "\t#size-cells = <2>;",
        "",
        "\tchosen {",
        f'\t\tbootargs = "earlycon console=ttyS0,115200n8 lg.device={device}";',
        "\t};",
        "",
        f"\tmemory@40000000 {{ /* prior {hex(0x40000000)}: replace with LK value */",
        '\t\tdevice_type = "memory";',
        f"\t\treg = <0x0 0x40000000 0x0 {hex(mem_bytes)}>;",
        "\t};",
        "",
        fb_node.rstrip("\n"),
        "",
        "\t/* UART/MMC/PMIC/panel/touch/modem nodes land here once the",
        "\t * phone's own LK/DTB gives us addresses (BRINGUP step 1). */",
        "};",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the LG K20 bring-up DTS")
    parser.add_argument("--device", default="lm-x120", help="LG codename (devices.sh)")
    parser.add_argument("--fb-base", default="",
                        help="LK framebuffer phys base, e.g. 0x5c000000 (BRINGUP step 2)")
    parser.add_argument("--out", required=True, help="output .dts path")
    args = parser.parse_args()

    if args.fb_base and not re.fullmatch(r"0[xX][0-9a-fA-F]+", args.fb_base):
        parser.error("--fb-base must look like 0x5c000000")
    dts = generate(read_sources(), args.device, args.fb_base)
    Path(args.out).write_text(dts)
    print(f"wrote {args.out} for LG {args.device}")


if __name__ == "__main__":
    main()
