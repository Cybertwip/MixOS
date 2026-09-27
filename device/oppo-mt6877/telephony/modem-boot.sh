#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Boot the MT6877 modem and wait for READY. Exits nonzero with the reason on
# stdout when the modem cannot boot (missing firmware is the common one).
set -u

SYS=/sys/bus/platform/devices
MDM=""

for d in "$SYS"/oppo-mt6877-modem* "$SYS"/*modem*; do
    [ -f "$d/state" ] && MDM="$d" && break
done
if [ -z "$MDM" ]; then
    echo "no modem device (is oppo_mt6877_modem loaded? try oppo.modem=1)"
    exit 1
fi
if command -v rfkill >/dev/null 2>&1; then
    rfkill unblock wwan 2>/dev/null || true
fi
echo 1 > "$MDM/boot" 2>/dev/null || {
    echo "modem refused boot; see dmesg (firmware $MDM usually)"
    exit 1
}
for _ in $(seq 1 60); do
    [ "$(cat "$MDM/state")" = "ready" ] && { echo "modem ready"; exit 0; }
    [ "$(cat "$MDM/state")" = "failed" ] && { echo "modem failed; see dmesg"; exit 1; }
    sleep 1
done
echo "modem timed out waiting for ready"
exit 1
