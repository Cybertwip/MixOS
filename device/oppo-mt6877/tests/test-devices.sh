#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# OPPO devices.sh test: every row must survive the eval the wrappers run
# (values carry parens and spaces), and unknown codenames must fail.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=device/oppo-mt6877/devices.sh
. "$HERE/../devices.sh"

fail=0
for d in $(oppo_devices); do
    info="$(oppo_device_info "$d")" || { echo "FAIL: $d rejected"; fail=1; continue; }
    eval "$info"
    want="$(printf '%s\n' "$OPPO_DEVICES" | grep -m1 "^${d}|" | cut -d'|' -f7)"
    if [ "${OPPO_DEVICE:-}" != "$d" ] || [ "${OPPO_NOTES:-}" != "$want" ]; then
        echo "FAIL: $d mangled through eval (got: $OPPO_NOTES)"
        fail=1
        continue
    fi
    echo "  $d: eval ok ($OPPO_DEVICE/$OPPO_SOC/$OPPO_PANEL)"
done
if oppo_device_info bogus >/dev/null 2>&1; then
    echo "FAIL: unknown device accepted"
    fail=1
else
    echo "  bogus: rejected ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: OPPO devices.sh"
exit "$fail"
