#!/usr/bin/env python3
"""Exercise checkpoint rotation, opt-in behavior and mount cleanup without a card."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

helper = Path(__file__).resolve().parents[1] / "power-diagnostic.sh"
with tempfile.TemporaryDirectory(prefix="j36-diagnostic-test-") as tmp:
    root = Path(tmp)
    for name in ("bootfs", "dev", "proc/sys/kernel/random", "sys"):
        (root / name).mkdir(parents=True)
    (root / "proc/uptime").write_text("123.00 0.00\n")
    (root / "proc/cmdline").write_text("j36.power=external j36.diag=power\n")
    (root / "proc/sys/kernel/random/boot_id").write_text("test-boot\n")
    (root / "dev/j36-init-trace").write_text("expansion returned\n")
    source = re.sub(r"/bootfs|/dev/|/proc/|/sys/",
                    lambda match: str(root) + match[0], helper.read_text())
    script = root / "test.sh"
    script.write_text("""
say() { :; }
stage() { power_diag_checkpoint "$*"; }
detail() { :; }
sleep() { echo "sleep $*" >> "$TEST_ROOT/ops"; }
sync() { echo sync >> "$TEST_ROOT/ops"; }
mount() { echo "mount $*" >> "$TEST_ROOT/ops"; [ "$FAIL_MOUNT" != 1 ]; }
umount() { echo "umount $*" >> "$TEST_ROOT/ops"; }
mount_bootfs() { bootfs_mounted=1; }
dmesg() { echo 'test kernel message'; }
rootdev=/dev/mmcblk0p2
rootfs_type=ext2
""" + source + "\npower_diag_start\npower_diag_checkpoint final\n"
                      + 'echo "mounted=$bootfs_mounted"\n')
    for enabled, existing, fail in [("", "0", "0"), ("power", "0", "0"),
                                     ("power", "1", "0"), ("power", "0", "1")]:
        (root / "ops").write_text("")
        for f in (root / "bootfs").glob("*.txt"):
            f.unlink()
        result = subprocess.run(["sh", str(script)], check=True, text=True,
                                capture_output=True, env=dict(os.environ,
                                    TEST_ROOT=str(root), power_diag=enabled,
                                    bootfs_mounted=existing, FAIL_MOUNT=fail))
        ops = (root / "ops").read_text()
        if not enabled:
            assert not ops and not list((root / "bootfs").glob("*.txt"))
            continue
        if fail == "1":
            assert not list((root / "bootfs").glob("*.txt"))
            continue
        assert ops.count("sleep 5") == 12
        assert result.stdout.strip() == "mounted=" + existing
        logs = [f.read_text() for f in (root / "bootfs").glob("*.txt")]
        assert len(logs) == 2
        assert any("stage=final" in log and "checkpoint_complete=15" in log for log in logs)
        assert all("boot_id=test-boot" in log and "test kernel message" in log for log in logs)
        assert all("unavailable" in log for log in logs)
        assert ("umount " in ops) == (existing == "0")
        assert ops.count("remount,ro") == 15
builder = (helper.parent / "build-in-vm.sh").read_text()
start = builder.index('power_diag=""\n')
mode = builder[start:builder.index('say() {', start)]
start = builder.index('if [ "$power_diag" = power ]; then\n    # Do not wait')
bypass = builder[start:builder.index('\nfi\n', start) + 4]
with tempfile.TemporaryDirectory(prefix="j36-diagnostic-mode-") as tmp:
    root = Path(tmp)
    marker = root / "marker"
    cmdline = root / "cmdline"
    empty = root / "helper"
    empty.write_text("")
    mode = mode.replace('/proc/cmdline', str(cmdline))
    mode = mode.replace('/etc/j36-power-diagnostic', str(marker))
    mode = mode.replace('/power-diagnostic.sh', str(empty))
    for embedded, argument, expected in [(False, "", "retry"),
                                         (False, "j36.diag=power", "0"),
                                         (True, "", "0")]:
        if embedded:
            marker.write_text("v2\n")
        elif marker.exists():
            marker.unlink()
        cmdline.write_text(argument + "\n")
        result = subprocess.run(["sh", "-c", 'stage() { :; }; detail() { :; }; sleep() { :; };\n'
                                 + mode + '\nwant_expand=retry\n' + bypass
                                 + '\necho "$want_expand"'], text=True,
                                capture_output=True, check=True)
        assert result.stdout.strip() == expected
assert builder.index('stage "J36 DIAG v2: resize skipped"') < builder.index('\nexpand_root\n')
print("Power diagnostic: opt-in, embedded mode, resize bypass, idle interval, rotation and mount cleanup passed")
