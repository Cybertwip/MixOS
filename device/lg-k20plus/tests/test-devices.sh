#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# LG devices.sh test: every row must survive the eval the wrappers run
# (values carry parens and spaces), and unknown codenames must fail.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=device/lg-k20plus/devices.sh
. "$HERE/../devices.sh"

fail=0
for d in $(lg_devices); do
    info="$(lg_device_info "$d")" || { echo "FAIL: $d rejected"; fail=1; continue; }
    eval "$info"
    want="$(printf '%s\n' "$LG_DEVICES" | grep -m1 "^${d}|" | cut -d'|' -f8)"
    if [ "${LG_DEVICE:-}" != "$d" ] || [ "${LG_NOTES:-}" != "$want" ]; then
        echo "FAIL: $d mangled through eval (got: $LG_NOTES)"
        fail=1
        continue
    fi
    echo "  $d: eval ok ($LG_DEVICE/$LG_SOC/$LG_PANEL/$LG_REV)"
done
if lg_device_info bogus >/dev/null 2>&1; then
    echo "FAIL: unknown device accepted"
    fail=1
else
    echo "  bogus: rejected ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: LG devices.sh"
exit "$fail"
