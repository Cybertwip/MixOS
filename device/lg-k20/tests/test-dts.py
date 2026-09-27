#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""K20 DTS test: generate for every known device and assert the honest
skeleton -- model, memory from the row, one CPU, bootargs -- plus the
two no-fabrication rules: no simple-framebuffer without an fb base, and
a simple-framebuffer with one. Compiles with dtc when available."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEVICE_DIR = HERE.parent
sys.path.insert(0, str(DEVICE_DIR))

from generate_dts_k20 import generate  # noqa: E402


def devices() -> list[str]:
    out = subprocess.run(
        ["bash", "-c", f"source {DEVICE_DIR}/devices.sh && k20_devices"],
        capture_output=True, text=True, check=True)
    return [d for d in out.stdout.split() if d]


def row(dev: str) -> dict[str, str]:
    out = subprocess.run(
        ["bash", "-c", f"source {DEVICE_DIR}/devices.sh && k20_device_info {dev}"],
        capture_output=True, text=True, check=True)
    vals: dict[str, str] = {}
    for line in out.stdout.splitlines():
        k, _, v = line.partition("=")
        vals[k] = v.strip("'")
    return vals


def compile_check(dts: str, dtc: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "dev.dts"
        src.write_text(dts)
        subprocess.run([dtc, "-I", "dts", "-O", "dtb",
                        "-o", str(Path(tmp) / "dev.dtb"), str(src)],
                       capture_output=True, text=True, check=True)


def main() -> None:
    devs = devices()
    assert devs, "devices.sh lists no K20 devices"
    dtc = shutil.which("dtc")
    for dev in devs:
        r = row(dev)
        mem = int(r["K20_MEM_MB"])
        dts = generate(dev, mem, int(r["K20_WIDTH"]), int(r["K20_HEIGHT"]))
        assert f"k20.device={dev}" in dts, f"{dev}: bootargs missing device word"
        assert "root=PARTLABEL=ROOTFS" in dts, f"{dev}: bootargs missing root"
        assert "mediatek,mt6739" in dts, f"{dev}: missing soc compatible"
        assert "simple-framebuffer" not in dts, f"{dev}: invented a framebuffer"
        assert dts.count("cpu@") == 1, f"{dev}: expected one cpu node"
        expect = mem * 1024 * 1024
        assert "%x" % (expect & 0xFFFFFFFF) in dts, f"{dev}: memory size missing"
        if dtc:
            compile_check(dts, dtc)
        with_fb = generate(dev, mem, int(r["K20_WIDTH"]), int(r["K20_HEIGHT"]),
                            fb_base=0x40000000 + 0x1000000)
        assert "simple-framebuffer" in with_fb, f"{dev}: fb base ignored"
        if dtc:
            compile_check(with_fb, dtc)
        print(f"  {dev}: skeleton ok" + (" + dtc" if dtc else " (no dtc)"))
    print(f"PASS: {len(devs)} K20 device(s)")


if __name__ == "__main__":
    main()
