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
    (root / "dev/.watch-status").write_text("mediatek.ko\n")
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
        (root / "mark").write_text("run_usb:mediatek.ko\n")
        result = subprocess.run(["sh", str(script)], check=True, text=True,
                                capture_output=True, env=dict(os.environ,
                                    TEST_ROOT=str(root), power_diag=enabled,
                                    bootfs_mounted=existing, FAIL_MOUNT=fail,
                                    watch_markfile=str(root / "mark")))
        ops = (root / "ops").read_text()
        if not enabled:
            assert not ops and not list((root / "bootfs").glob("*.txt"))
            continue
        if fail == "1":
            assert not list((root / "bootfs").glob("*.txt"))
            continue
        assert "sleep " not in ops
        assert result.stdout.strip() == "mounted=" + existing
        logs = [f.read_text() for f in (root / "bootfs").glob("*.txt")]
        assert len(logs) == 2
        assert any("stage=final" in log and "checkpoint_complete=2" in log for log in logs)
        assert all("boot_id=test-boot" in log and "test kernel message" in log for log in logs)
        assert all("unavailable" in log for log in logs)
        assert all("watch_step=mediatek.ko" in log for log in logs)
        assert all("wedge_mark=run_usb:mediatek.ko" in log for log in logs)
        assert ("umount " in ops) == (existing == "0")
        assert ops.count("remount,ro") == 2
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
            marker.write_text("v5\n")
        elif marker.exists():
            marker.unlink()
        cmdline.write_text(argument + "\n")
        result = subprocess.run(["sh", "-c", 'stage() { :; }; detail() { :; }; sleep() { :; }; power_diag_checkpoint() { [ \"$want_expand\" = 0 ] && [ \"$power_diag_ready\" = 1 ] || exit 73; };\n'
                                 + mode + '\nwant_expand=retry\n' + bypass
                                 + '\necho "$want_expand"'], text=True,
                                capture_output=True, check=True)
        assert result.stdout.strip() == expected
assert builder.index('stage "J36 DIAG v5: resize skipped"') < builder.index('\nexpand_root\n')

def function(name):
    start = builder.index(name + "() {\n")
    return builder[start:builder.index("\n}\n", start) + 3]

with tempfile.TemporaryDirectory(prefix="j36-recall-test-") as tmp:
    root = Path(tmp)
    script = root / "recall.sh"
    script.write_text("""
watch_markfile="$TEST_ROOT/boot-stage"
watch_wedge=""
watch_recalled=0
say() { echo "say: $*"; }
detail() { :; }
sync() { :; }
""" + function("watch_mark") + function("watch_recall") + """
watch_recall
echo "wedge=$watch_wedge"
echo "mark=$(cat "$watch_markfile" 2>/dev/null)"
echo "prev=$(cat "$watch_markfile.prev" 2>/dev/null)"
""")
    for mode, mark, wedge, prev, words in [
            ("", "run_usb:mediatek.ko", "run_usb", "",
             ["inside run_usb at mediatek.ko", "skipped this time"]),
            ("power", "run_usb:mediatek.ko", "", "run_usb:mediatek.ko",
             ["diag retry", "mediatek.ko"]),
            ("", "run_wifi", "run_wifi", "",
             ["inside run_wifi", "skipped this time"]),
            ("power", None, "", "", [])]:
        for f in ("boot-stage", "boot-stage.prev"):
            if (root / f).exists():
                (root / f).unlink()
        if mark is not None:
            (root / "boot-stage").write_text(mark + "\n")
        result = subprocess.run(["sh", str(script)], check=True, text=True,
                                capture_output=True, env=dict(os.environ,
                                    TEST_ROOT=str(root), power_diag=mode))
        out = result.stdout
        assert f"wedge={wedge}\n" in out
        assert "mark=\n" in out
        assert f"prev={prev}\n" in out
        for word in words:
            assert word in out, (mode, mark, word, out)
        if not words:
            assert "stopped dead" not in out

with tempfile.TemporaryDirectory(prefix="j36-earlytrace-test-") as tmp:
    root = Path(tmp)
    script = root / "stage.sh"
    staged = (function("ensure_run_tmpfs") +
              function("setup_earlytrace")).replace("/newroot/", str(root) + "/")
    script.write_text("""
run_tmpfs=0
rootdev=/dev/mmcblk0p2
say() { echo "say: $*"; }
detail() { :; }
mount() { :; }
""" + staged + "\nsetup_earlytrace\n")
    for mode, want in [("power", True), ("", False)]:
        run = root / "run"
        if run.exists():
            for f in sorted(run.rglob("*"), reverse=True):
                if f.is_symlink() or f.is_file():
                    f.unlink()
                else:
                    f.rmdir()
        result = subprocess.run(["sh", str(script)], check=True, text=True,
                                capture_output=True, env=dict(os.environ,
                                    TEST_ROOT=str(root), power_diag=mode))
        unit = run / "systemd/system/j36-early-trace.service"
        prog = run / "j36/bin/j36-early-trace"
        link = run / "systemd/system/sysinit.target.wants/j36-early-trace.service"
        assert unit.exists() == want
        assert prog.exists() == want
        assert link.is_symlink() == want
        if not want:
            continue
        assert "j36-early-0.txt" in result.stdout
        assert "Before=sysinit.target" in unit.read_text()
        assert "ExecStart=/bin/sh /run/j36/bin/j36-early-trace" in unit.read_text()
        text = prog.read_text()
        for needle in ("j36-early-", "is-system-running", "is-active mixdash.service",
                       "sleep 2", "trace_complete="):
            assert needle in text, needle
        assert os.access(prog, os.X_OK)
        subprocess.run(["sh", "-n", str(prog)], check=True)

with tempfile.TemporaryDirectory(prefix="j36-switchroot-test-") as tmp:
    root = Path(tmp)
    fake_switch = root / "switch_root"
    fake_switch.write_text('#!/bin/sh\necho "switch_root $*"\n')
    fake_switch.chmod(0o755)
    script = root / "handover.sh"
    script.write_text("""
PATH="$TEST_ROOT:$PATH"
rootdev="$TEST_ROOTDEV"
want_switchroot="$TEST_WANT"
splash_on=0
splash_chan=/dev/null
say() { echo "say: $*"; }
stage() { echo "stage: $*"; }
detail() { :; }
progress() { :; }
mount() { :; }
sync() { :; }
""" + function("do_switchroot") + '\ndo_switchroot\necho "returned=$?"\n')
    for dev, want, present, absent in [
            ("/dev/mmcblk0p2", "1",
             ["stage: Starting MixOS", "switch_root /newroot /sbin/init"], ["returned="]),
            ("/dev/mmcblk0p2", "0",
             ["staying in the initramfs", "stage: Shell without systemd", "returned=0"],
             ["switch_root /newroot"]),
            ("", "1", ["returned=1"],
             ["switch_root", "switching root", "Shell without systemd"])]:
        result = subprocess.run(["sh", str(script)], check=True, text=True,
                                capture_output=True, env=dict(os.environ,
                                    TEST_ROOT=str(root), TEST_ROOTDEV=dev,
                                    TEST_WANT=want))
        for word in present:
            assert word in result.stdout, (dev, want, word, result.stdout)
        for word in absent:
            assert word not in result.stdout, (dev, want, word, result.stdout)
    branch_start = builder.index("j36.switchroot=0|noswitchroot)")
    branch = builder[branch_start:builder.index(";;", branch_start) + 2]
    for word, want in [("j36.switchroot=0", "0"), ("noswitchroot", "0"),
                       ("j36.audio", "1")]:
        result = subprocess.run(
            ["sh", "-c", 'want_switchroot=1; arg="' + word + '"\ncase "$arg" in\n'
             + branch + '\nesac\necho "want=$want_switchroot"'],
            check=True, text=True, capture_output=True)
        assert f"want={want}\n" in result.stdout, (word, result.stdout)
print("Power diagnostic: opt-in, embedded mode, resize bypass, no idle delay, rotation, mount cleanup, step marks, diag retry, early trace and switchroot gate passed")
