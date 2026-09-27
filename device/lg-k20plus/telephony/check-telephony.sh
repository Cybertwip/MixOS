#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# On-device telephony self-test. PASS/FAIL/SKIP per line.
set -u

say() { printf '%-5s %s\n' "$1" "$2"; }

if lsmod | grep -q qcom_q6v5_mss; then say PASS "mss remoteproc loaded"; else say FAIL "mss remoteproc missing"; fi
if [ -e /dev/disk/by-partlabel/modemst1 ]; then say PASS "rmtfs present"; else say FAIL "rmtfs missing"; fi
if command -v qrtr-lookup >/dev/null 2>&1 && qrtr-lookup 2>/dev/null | grep -q .; then
    say PASS "qrtr services visible"
else
    say FAIL "no qrtr services"
fi
if command -v mmcli >/dev/null 2>&1 && mmcli -L 2>/dev/null | grep -q "Modem/"; then
    say PASS "ModemManager sees a modem"
else
    say SKIP "ModemManager has no modem (yet)"
fi
