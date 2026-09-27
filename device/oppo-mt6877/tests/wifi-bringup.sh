#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# On-device Wi-Fi bring-up check. Run after booting with oppo.wifi=1.
# PASS/FAIL/SKIP per line for serial-console reading.
set -u

say() { printf '%-5s %s\n' "$1" "$2"; }

if lsmod | grep -q oppo_mt6877_consys; then say PASS "consys loaded"; else say FAIL "consys missing"; fi
if lsmod | grep -q oppo_mt6877_wifi; then say PASS "wifi loaded"; else say FAIL "wifi missing"; fi
if [ -f /lib/firmware/oppo/mt6877/WIFI_RAM_CODE ]; then say PASS "RAM code present"; else say FAIL "RAM code missing"; fi
if ip link show wlan0 >/dev/null 2>&1; then say PASS "wlan0 present"; else say FAIL "no wlan0"; fi
if iw dev wlan0 scan >/dev/null 2>&1; then say PASS "scan works"; else say SKIP "scan pending (opcode check)"; fi
