#!/usr/bin/env python3
"""Exercise the real init functions with module insertion replaced by a recorder."""
from pathlib import Path
import os
import subprocess
import tempfile

builder = (Path(__file__).resolve().parents[1] / "build-in-vm.sh").read_text()

def function(name):
    start = builder.index(name + "() {\n")
    return builder[start:builder.index("\n}\n", start) + 3]

with tempfile.TemporaryDirectory(prefix="j36-policy-test-") as tmp:
    root = Path(tmp)
    for group, module in [("wifi", "j36_mt6592_pmic"), ("audio", "j36_mt6592_audio")]:
        (root / group).mkdir()
        (root / group / "load.order").write_text("j36_pwrap.ko\n" + module + ".ko\n")
    # The shared dependency was inserted before the input driver.
    (root / "sys/module/j36_pwrap").mkdir(parents=True)
    recorder = root / "calls"
    script = """
find_payload() { payload="$TEST_ROOT"; }
say() { :; }
show() { :; }
watch_say() { :; }
wlan_iface() { echo wlan0; }
dmesg() { :; }
insmod() { echo "$*" >> "$TEST_ROOT/calls"; }
""" + function("run_wifi") + function("run_audio") + "\nrun_wifi\nrun_audio\n"
    script = script.replace("/sys/", str(root / "sys") + "/")
    script = script.replace("/tmp/insmod.log", str(root / "insmod.log"))
    for external, charge, pmic_args, audio_args in [
        ("1", "1", "external_power=1", "speaker=0 external_power=1"),
        ("0", "0", "charge=0", "speaker=1"),
        ("0", "1", "", "speaker=1"),
    ]:
        recorder.write_text("")
        env = dict(os.environ, TEST_ROOT=str(root), power_external=external,
                   power_charge=charge, audio_speaker="1")
        subprocess.run(["sh", "-c", script], env=env, check=True, timeout=10)
        assert recorder.read_text().splitlines() == [
            f"{root}/wifi/j36_mt6592_pmic.ko" + (" " + pmic_args if pmic_args else ""),
            f"{root}/audio/j36_mt6592_audio.ko {audio_args}",
        ]
print("Power policy: Wi-Fi fallback, batteryless audio and shared-module skip passed")
