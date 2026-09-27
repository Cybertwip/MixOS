#!/usr/bin/env python3
"""Generate the OPPO A77 4G (MT6765) bring-up DTS.

Sibling-shaped to device/lg-k20/generate_dts_k20.py: the tests import
``read_sources`` / ``generate``; the CLI writes a file. The skeleton is
the shared device/common/mtk_bringup_dts.py emission; this file owns
only the root node, the per-device facts from devices.sh, and the
exact-SoC compatibles (per-device, not family wildcards). Anything the
phone has not told us yet is a comment, not a node: no invented
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
# mt6765.dtsi does not exist in mainline 6.12 yet). Both RAM rows are the
# same phone, so both claim the same pair.
COMPATIBLES = [
    "oppo,cph2385",
    "mediatek,mt6765",
]

REQUIRED_BOOTARGS = ["earlycon", "oppo.device="]


def read_sources(tree: Path = TREE) -> dict:
    """Load per-device geometry from devices.sh via the shell."""
    devices = tree / "devices.sh"
    names = subprocess.run(["sh", "-c", f". {devices} && a77_4g_devices"],
                           capture_output=True, text=True, check=True)
    sources = {}
    for name in names.stdout.split():
        probe = subprocess.run(
            ["sh", "-c", f". {devices} && eval \"$(a77_4g_device_info {name})\" && "
                         "echo \"$A77_4G_SOC $A77_4G_MEM_MB $A77_4G_WIDTH $A77_4G_HEIGHT\""],
            capture_output=True, text=True, check=True)
        soc, mem_mb, width, height = probe.stdout.split()
        sources[name] = {"soc": soc, "mem_mb": int(mem_mb),
                         "width": int(width), "height": int(height)}
    return sources


def generate(sources: dict, device: str, fb_base: str = "") -> str:
    spec = sources[device]  # KeyError: unknown device, callers load devices.sh
    body = generate_bringup_dts(
        model=f"OPPO {device.upper()} (MT6765 bring-up)",
        board_compat="oppo,cph2385",
        soc_compat="mediatek,mt6765",
        mem_mb=spec["mem_mb"],
        bootargs=f"earlycon console=ttyS0,115200n8 oppo.device={device}",
        fb_base=int(fb_base, 0) if fb_base else 0,
        width=spec["width"], height=spec["height"],
    )
    # The helper emits a root-children fragment (plus its own header lines);
    # the root node is this tree's to own.
    fragment = body.splitlines()[2:]
    lines = ["/dts-v1/;", "",
             f"/ {{ /* {device}: {spec['soc']}, {spec['mem_mb']} MiB */"]
    lines += fragment
    lines += ["};", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the OPPO A77 4G bring-up DTS")
    parser.add_argument("--device", default="cph2385", help="OPPO codename (devices.sh)")
    parser.add_argument("--fb-base", default="",
                        help="LK framebuffer phys base, e.g. 0x5c000000 (BRINGUP step 2)")
    parser.add_argument("--out", required=True, help="output .dts path")
    args = parser.parse_args()

    if args.fb_base and not re.fullmatch(r"0[xX][0-9a-fA-F]+", args.fb_base):
        parser.error("--fb-base must look like 0x5c000000")
    dts = generate(read_sources(), args.device, args.fb_base)
    Path(args.out).write_text(dts)
    print(f"wrote {args.out} for OPPO {args.device}")


if __name__ == "__main__":
    main()
