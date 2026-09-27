#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# On-device Wi-Fi bring-up check. Run after booting with lg.wifi=1.
set -u

say() { printf '%-5s %s\n' "$1" "$2"; }

if lsmod | grep -q lg_msm8917_wcnss; then say PASS "pronto boot loaded"; else say FAIL "pronto boot missing"; fi
if lsmod | grep -q wcn36xx; then say PASS "wcn36xx loaded"; else say FAIL "wcn36xx missing"; fi
if [ -f /lib/firmware/lg/lv517/wcnss.mdt ]; then say PASS "wcnss.mdt present"; else say FAIL "wcnss.mdt missing"; fi
if ip link show wlan0 >/dev/null 2>&1; then say PASS "wlan0 present"; else say FAIL "no wlan0"; fi
if iw dev wlan0 scan >/dev/null 2>&1; then say PASS "scan works"; else say FAIL "scan failed"; fi
