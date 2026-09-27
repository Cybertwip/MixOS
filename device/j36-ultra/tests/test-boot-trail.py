#!/usr/bin/env python3
"""Exercise the real splash-tick script with mounting replaced by a recorder.

The script is extracted from device/j36-ultra/build-in-vm.sh (the SPLASHTICK
heredoc), so this test fails if the builder and the test drift apart.  Mount,
sleep and the /proc readers are stubbed; the trail file, the splash channel
and the done flag are redirected into a temporary directory.
"""
from pathlib import Path
import os
import subprocess
import tempfile

builder = (Path(__file__).resolve().parents[1] / "build-in-vm.sh").read_text()
start = builder.index("cat > /newroot/run/j36/bin/mixos-splash-tick <<'SPLASHTICK'\n")
start += len("cat > /newroot/run/j36/bin/mixos-splash-tick <<'SPLASHTICK'\n")
splash_tick = builder[start:builder.index("\nSPLASHTICK", start)]


def run_case(cmdline, chan_present, done_present, background_done=False):
    with tempfile.TemporaryDirectory(prefix="j36-trail-test-") as tmp:
        root = Path(tmp)
        script = splash_tick.replace("/dev/.mixsplash-done", str(root / "done"))
        script = script.replace("/dev/.mixsplash", str(root / "chan"))
        script = script.replace("/run/j36/trailmnt", str(root / "mnt"))
        (root / "fakedev").write_text("stand-in block device\n")
        (root / "mnt").mkdir()
        (root / "mnt" / "j36-trail.txt").write_text("stale trail from an older boot\n")
        if chan_present:
            (root / "chan").write_text("")
        if done_present:
            (root / "done").write_text("")
        harness = """
sleep() { :; }
sync() { :; }
mount() { mkdir -p "$6/mvii"; }
umount() { :; }
dmesg() { echo "fake kernel line"; }
cat() {
    if [ "$1" = /proc/cmdline ]; then printf '%s\\n' "$FAKE_CMDLINE";
    else command cat "$@"; fi;
}
cut() {
    if [ "$3" = /proc/uptime ]; then echo "42.5";
    else command cut "$@"; fi;
}
""" + ("(command sleep 0.3; touch \"" + str(root / "done") + "\") &\n"
            if background_done else "") + script
        env = dict(os.environ, FAKE_CMDLINE=cmdline,
                   J36_TRAIL_DEVS=str(root / "fakedev"))
        subprocess.run(["sh", "-c", harness], env=env, check=True,
                       timeout=30)
        trail = root / "mnt" / "j36-trail.txt"
        return (trail.read_text() if trail.exists() else None,
                (root / "chan").read_text() if (root / "chan").exists() else None)


# Trail on, splash present, dashboard already done: tick 0 only, stale gone.
trail, chan = run_case("console=tty0 j36.trail=1 j36.splash=1", True, True)
assert "stale trail" not in trail, trail
assert "--- trail tick 0: 42.5s up ---" in trail, trail
assert "fake kernel line" in trail, trail
assert "stage:Starting system services" in chan, chan

# Trail off: the old file is untouched, the splash still ticks.
trail, chan = run_case("console=tty0 j36.splash=1", True, True)
assert trail == "stale trail from an older boot\n", trail
assert "stage:Starting system services" in chan, chan

# No splash channel (j36.splash=0): the loop still runs and the trail lands.
trail, chan = run_case("console=tty0 j36.trail=1 j36.splash=0", False, True)
assert chan is None
assert "--- trail tick 0: 42.5s up ---" in trail, trail

# Dashboard arrives mid-loop: later ticks append after tick 0.
trail, chan = run_case("console=tty0 j36.trail=1", True, False,
                       background_done=True)
assert "--- trail tick 0: 42.5s up ---" in trail, trail
assert "--- trail tick 1: 42.5s up ---" in trail, trail
assert "detail:systemd -- " in chan, chan

print("Boot trail: truncate, gate, headless run and tick appends passed")
