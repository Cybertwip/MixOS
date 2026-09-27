#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# On-device telephony self-test. Each line is PASS/FAIL/SKIP so the result
# can be read over a serial console without scrolling back.
set -u

say() { printf '%-5s %s\n' "$1" "$2"; }

if lsmod | grep -q oppo_mt6877_modem; then say PASS "modem module loaded"; else say FAIL "modem module missing"; fi
MDM="$(ls -d /sys/bus/platform/devices/*modem* 2>/dev/null | head -n1)"
if [ -n "$MDM" ] && [ "$(cat "$MDM/state" 2>/dev/null)" = "ready" ]; then
    say PASS "modem ready"
else
    say FAIL "modem not ready (${MDM:-no device})"
fi
if command -v rfkill >/dev/null 2>&1 && rfkill list wwan 2>/dev/null | grep -q "Soft blocked: no"; then
    say PASS "wwan rfkill unblocked"
else
    say FAIL "wwan rfkill blocked or missing"
fi
if [ -e /dev/ccmni0 ] || ip link show wwan0 >/dev/null 2>&1; then
    say PASS "data interface present"
else
    say SKIP "no data interface (stage-2 channel port)"
fi
