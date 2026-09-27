#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""OPPO DTS test: generate for every known device and assert the bring-up set.

Checks each generated DTS contains the nodes the drivers bind to, then
compiles it with dtc when dtc is available (it is on the build VM; on a
workstation without dtc the text assertions still run).
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEVICE_DIR = HERE.parent
sys.path.insert(0, str(DEVICE_DIR))

from generate_dts_oppo import generate, read_sources  # noqa: E402

REQUIRED_COMPATIBLES = [
    "mediatek,mt6877",       # root
    "arm,gic-v3",
    "mediatek,mt6577-uart",
    "mediatek,mt6877-mmc",
    "mediatek,mt6765-spi",
    "oppo,nt36672",
    "oppo,mt6877-consys",
    "oppo,mt6877-wifi",
    "oppo,mt6877-afe",
    "oppo,mt6877-sound",
    "oppo,mt6877-modem",
    "oppo,mt6877-keys-polled",
    "oppo,mt6877-power",
    "simple-framebuffer",
]

REQUIRED_BOOTARGS = ["oppo.audio=1", "oppo.wifi=1", "oppo.modem=1", "oppo.power=1"]


def devices() -> list[str]:
    out = subprocess.run(
        ["bash", "-c", f"source {DEVICE_DIR}/devices.sh && oppo_devices"],
        capture_output=True, text=True, check=True)
    return [d for d in out.stdout.split() if d]


def main() -> None:
    sources = read_sources(DEVICE_DIR / "board")
    devs = devices()
    assert devs, "devices.sh lists no OPPO devices"
    dtc = shutil.which("dtc")
    for dev in devs:
        dts = generate(sources, dev)
        for compat in REQUIRED_COMPATIBLES:
            assert compat in dts, f"{dev}: missing {compat}"
        for word in REQUIRED_BOOTARGS:
            assert word in dts, f"{dev}: bootargs missing {word}"
        assert f"oppo.device={dev}" in dts, f"{dev}: bootargs missing device word"
        if dtc:
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / "dev.dts"
                src.write_text(dts)
                subprocess.run([dtc, "-I", "dts", "-O", "dtb",
                                "-o", str(Path(tmp) / "dev.dtb"), str(src)],
                               capture_output=True, text=True, check=True)
        print(f"  {dev}: {len(REQUIRED_COMPATIBLES)} nodes ok" +
              (" + dtc" if dtc else " (no dtc)"))
    print(f"PASS: {len(devs)} OPPO device(s)")


if __name__ == "__main__":
    main()
