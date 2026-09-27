#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# K20 devices.sh test: every row must survive the eval the wrappers run,
# and unknown codenames must fail.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=device/lg-k20/devices.sh
. "$HERE/../devices.sh"

fail=0
for d in $(k20_devices); do
    info="$(k20_device_info "$d")" || { echo "FAIL: $d rejected"; fail=1; continue; }
    eval "$info"
    want="$(printf '%s\n' "$LG_K20_DEVICES" | grep -m1 "^${d}|" | cut -d'|' -f8)"
    if [ "${K20_DEVICE:-}" != "$d" ] || [ "${K20_NOTES:-}" != "$want" ]; then
        echo "FAIL: $d mangled through eval (got: $K20_NOTES)"
        fail=1
        continue
    fi
    echo "  $d: eval ok ($K20_DEVICE/$K20_SOC/${K20_MEM_MB}MB)"
done
if k20_device_info bogus >/dev/null 2>&1; then
    echo "FAIL: unknown device accepted"
    fail=1
else
    echo "  bogus: rejected ok"
fi
[ "$fail" -eq 0 ] && echo "PASS: K20 devices.sh"
exit "$fail"
