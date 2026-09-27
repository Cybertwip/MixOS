#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# A77 devices.sh test: every row must survive the eval the wrappers run,
# and unknown codenames must fail.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=device/oppo-a77/devices.sh
. "$HERE/../devices.sh"

fail=0
for d in $(a77_devices); do
    info="$(a77_device_info "$d")" || { echo "FAIL: $d rejected"; fail=1; continue; }
    eval "$info"
    want="$(printf '%s\n' "$OPPO_A77_DEVICES" | grep -m1 "^${d}|" | cut -d'|' -f8)"
    if [ "${A77_DEVICE:-}" != "$d" ] || [ "${A77_NOTES:-}" != "$want" ]; then
        echo "FAIL: $d mangled through eval (got: $A77_NOTES)"
        fail=1
        continue
    fi
    echo "  $d: eval ok ($A77_DEVICE/$A77_SOC/${A77_MEM_MB}MB)"
done
if a77_device_info bogus >/dev/null 2>&1; then
    echo "FAIL: unknown device accepted"
    fail=1
else
    echo "  bogus: rejected ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: A77 devices.sh"
exit "$fail"
