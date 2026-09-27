#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Boot the MSM8917 modem and wait for its QRTR services.
set -u

MDM_SYS="$(ls -d /sys/bus/platform/devices/lg-lv517-modem* 2>/dev/null | head -n1)"
RPROC="$(ls -d /sys/class/remoteproc/remoteproc* 2>/dev/null | head -n1)"

if [ ! -e /dev/disk/by-partlabel/modemst1 ]; then
    echo "missing modemst1 (rmtfs); the modem will not register. Flash rmtfs first."
    exit 1
fi
if command -v rfkill >/dev/null 2>&1; then
    rfkill unblock wwan 2>/dev/null || true
fi
if [ -n "$RPROC" ] && [ "$(cat "$RPROC/state" 2>/dev/null)" = "offline" ]; then
    echo start > "$RPROC/state" || { echo "remoteproc refused start; see dmesg"; exit 1; }
fi
[ -n "$MDM_SYS" ] && echo boot > "$MDM_SYS/state" 2>/dev/null
for _ in $(seq 1 60); do
    if command -v qrtr-lookup >/dev/null 2>&1 && qrtr-lookup 2>/dev/null | grep -q .; then
        [ -n "$MDM_SYS" ] && echo ready > "$MDM_SYS/state" 2>/dev/null
        echo "modem ready ($(qrtr-lookup 2>/dev/null | wc -l | tr -d ' ') qrtr services)"
        exit 0
    fi
    sleep 1
done
[ -n "$MDM_SYS" ] && echo failed > "$MDM_SYS/state" 2>/dev/null
echo "modem timed out waiting for qrtr services"
exit 1
