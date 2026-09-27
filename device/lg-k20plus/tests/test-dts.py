#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""LG DTS test: generate for every known device and assert the bring-up set.

Covers both panel variants and both revisions, then compiles each with dtc
when available.
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

from generate_dts_lg import generate, read_sources  # noqa: E402

REQUIRED_COMPATIBLES = [
    "qcom,msm8917",
    "qcom,msm-qgic2",
    "qcom,msm-uartdm-v1.4",
    "qcom,sdhci-msm-v4",
    "qcom,i2c-qup-v2.2.1",
    "qcom,spmi-pmic-arb",
    "qcom,pmi8950-wled",
    "qcom,pm8941-pwrkey",
    "lge,pronto-wcnss",
    "lge,lv517-modem",
    "lge,lv517-keys-polled",
    "lge,lv517-power",
    "lge,lv517-sound",
    "simple-framebuffer",
]

REQUIRED_BOOTARGS = ["lg.audio=1", "lg.wifi=1", "lg.modem=1", "lg.power=1"]


def main() -> None:
    sources = read_sources(DEVICE_DIR / "board")
    dtc = shutil.which("dtc")
    cases = [
        ("lv517", "lg4894", "b", "lge,lg4894"),
        ("lv517-rev0", "lg4894", "0", "lge,lg4894"),
        ("lv517-tovis", "td4100", "b", "synaptics,rmi4-i2c"),
    ]
    for dev, panel, rev, touch_compat in cases:
        dts = generate(sources, dev, panel, rev, 0x90000000)
        for compat in REQUIRED_COMPATIBLES + [touch_compat]:
            assert compat in dts, f"{dev}: missing {compat}"
        for word in REQUIRED_BOOTARGS:
            assert word in dts, f"{dev}: bootargs missing {word}"
        assert f"lg.device={dev}" in dts, f"{dev}: bootargs missing device"
        assert f"lg.panel={panel}" in dts, f"{dev}: bootargs missing panel"
        if rev == "0":
            assert "lge,key-codes = <114 222>" in dts, "rev-0 rocker remap missing"
        else:
            assert "lge,key-codes = <115 222>" in dts, "rocker map missing"
        if dtc:
            with tempfile.TemporaryDirectory() as tmp:
                src = Path(tmp) / "dev.dts"
                src.write_text(dts)
                subprocess.run([dtc, "-I", "dts", "-O", "dtb",
                                "-o", str(Path(tmp) / "dev.dtb"), str(src)],
                               capture_output=True, text=True, check=True)
        print(f"  {dev}/{panel}/rev-{rev}: ok" + (" + dtc" if dtc else ""))
    # Disabled framebuffer: base 0 must emit status disabled, still compiling.
    dts = generate(sources, "lv517", "lg4894", "b", 0)
    assert 'status = "disabled"' in dts
    print(f"PASS: {len(cases)} LG device(s)")


if __name__ == "__main__":
    main()
