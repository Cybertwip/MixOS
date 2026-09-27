#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Pull the LG K20 Plus coprocessor blobs off a rooted stock phone over adb
# into ./stock/ (git-ignored). Point the build at them with:
#   LG_FIRMWARE_DIR=device/lg-k20plus/firmware/stock ./build-lg.sh
set -Eeuo pipefail

OUT="$(cd -- "$(dirname -- "$0")" && pwd)/stock"
mkdir -p "$OUT"
command -v adb >/dev/null || { echo "adb is not installed" >&2; exit 1; }
adb wait-for-device
adb root >/dev/null 2>&1 || true
for f in modem.mdt modem.b00 modem.b01 modem.b02 modem.b03 modem.b04 modem.b05 \
         modem.b06 modem.b07 modem.b08 modem.b09 modem.b10 modem.b11 modem.b12 \
         wcnss.mdt wcnss.b00 wcnss.b01 wcnss.b02 wcnss.b03 wcnss.b04 wcnss.b05; do
    if adb pull "/firmware/image/$f" "$OUT/$f" >/dev/null 2>&1; then
        echo "got $f"
    else
        echo "missing on device: /firmware/image/$f" >&2
    fi
done
adb pull /persist/WCNSS_qcom_wlan_nv.bin "$OUT/WCNSS_qcom_wlan_nv.bin" >/dev/null 2>&1 \
    && echo "got WCNSS_qcom_wlan_nv.bin" || echo "missing: WCNSS_qcom_wlan_nv.bin" >&2
ls "$OUT"
