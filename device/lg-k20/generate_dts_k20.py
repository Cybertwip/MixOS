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
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parent
sys.path.insert(0, str(TREE.parent / "common"))
from mtk_bringup_dts import generate_bringup_dts  # noqa: E402

# Exact-SoC compatibles claimed per device (bring-up prosthetic; the real
# mt6739.dtsi does not exist in mainline 6.12 yet).
COMPATIBLES = [
    "lg,lm-x120",
    "mediatek,mt6739",
]

REQUIRED_BOOTARGS = ["earlycon", "lg.device="]


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
    body = generate_bringup_dts(
        model=f"LG {device.upper()} (MT6739 bring-up)",
        board_compat="lg,lm-x120",
        soc_compat="mediatek,mt6739",
        mem_mb=spec["mem_mb"],
        bootargs=f"earlycon console=ttyS0,115200n8 lg.device={device}",
        fb_base=int(fb_base, 0) if fb_base else 0,
        width=spec["width"], height=spec["height"],
    )
    # The helper emits a root-children fragment (plus its own header lines);
    # the root node is this tree's to own.
    fragment = body.splitlines()[2:]
    lines = ["/dts-v1;/", "",
             f"/ {{ /* {device}: {spec['soc']}, {spec['mem_mb']} MiB */"]
    lines += fragment
    lines += ["};", ""]
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
