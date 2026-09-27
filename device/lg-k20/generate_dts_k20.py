#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""LG K20 (MT6739) DTS generator: thin per-device wrapper over the shared
family helper (device/common/mtk_bringup_dts.py). Emits the honest
skeleton -- model, memory from the row, one CPU, optional simplefb --
and nothing it cannot source. Subsystem nodes land with stock-DTB facts
(BRINGUP.md step 2), never before."""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "common"))
from mtk_bringup_dts import generate_bringup_dts  # noqa: E402


def generate(dev: str, mem_mb: int, width: int, height: int,
             fb_base: int = 0) -> str:
    bootargs = f"rw rootwait root=PARTLABEL=ROOTFS k20.device={dev}"
    return generate_bringup_dts(
        model=f"LG K20 2019 ({dev})",
        board_compat=f"lge,{dev}",
        soc_compat="mediatek,mt6739",
        mem_mb=mem_mb, bootargs=bootargs,
        fb_base=fb_base, width=width, height=height)


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate LG K20 DTS")
    ap.add_argument("--device", required=True)
    ap.add_argument("--mem-mb", required=True, type=int)
    ap.add_argument("--width", required=True, type=int)
    ap.add_argument("--height", required=True, type=int)
    ap.add_argument("--fb-base", default="0")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    dts = generate(args.device, args.mem_mb, args.width, args.height,
                   int(args.fb_base, 0))
    with open(args.out, "w") as f:
        f.write(dts)
    print(f"wrote {args.out} for K20 {args.device}")


if __name__ == "__main__":
    main()
