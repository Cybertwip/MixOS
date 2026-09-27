#!/usr/bin/env python3
"""DTS honesty tests for the LG K20 tree (sibling of the mt6877 test)."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from generate_dts_k20 import (  # noqa: E402
    COMPATIBLES, REQUIRED_BOOTARGS, generate, read_sources)


def compile_check(dts, dtc):
    if dtc is None:
        print("  dtc: not installed, compile check skipped")
        return
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "dev.dts"
        src.write_text(dts)
        subprocess.run([dtc, "-I", "dts", "-O", "dtb",
                        "-o", str(Path(tmp) / "dev.dtb"), str(src)],
                       capture_output=True, text=True, check=True)
        print("  dtc: compiles ok")


def main():
    dtc = shutil.which("dtc")
    sources = read_sources()
    assert list(sources) == ["lm-x120"], f"unexpected devices: {list(sources)}"

    for dev in sources:
        dts = generate(sources, dev)
        for compat in COMPATIBLES:
            assert compat in dts, f"{dev}: missing {compat}"
        for word in REQUIRED_BOOTARGS:
            assert word in dts, f"{dev}: bootargs missing {word}"
        assert f"lg.device={dev}" in dts, f"{dev}: bootargs missing device word"
        assert 'compatible = "simple-framebuffer"' not in dts, \
            f"{dev}: invented a framebuffer"
        assert "0x40000000" in dts and "prior" in dts, \
            f"{dev}: DRAM prior not marked as a prior"
        # With a measured base the framebuffer appears, honestly sized.
        fb = generate(sources, dev, fb_base="0x5c000000")
        assert 'compatible = "simple-framebuffer"' in fb, f"{dev}: --fb-base ignored"
        assert "0x5c000000" in fb and "0x1c2000" in fb, f"{dev}: fb reg wrong"
        compile_check(dts, dtc)
        print(f"  {dev}: DTS ok")
    print("PASS: K20 DTS")


main()
