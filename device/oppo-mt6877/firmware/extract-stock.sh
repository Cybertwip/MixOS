#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Pull the OPPO MT6877 coprocessor blobs off a rooted stock phone over adb
# into ./stock/ (git-ignored). Point the build at them with:
#   OPPO_FIRMWARE_DIR=device/oppo-mt6877/firmware/stock ./build-oppo.sh
set -Eeuo pipefail

OUT="$(cd -- "$(dirname -- "$0")" && pwd)/stock"
mkdir -p "$OUT"
command -v adb >/dev/null || { echo "adb is not installed" >&2; exit 1; }
adb wait-for-device
adb root >/dev/null 2>&1 || true
for f in modem.img dsp.img WIFI_RAM_CODE WMT_SOC.cfg; do
    if adb pull "/vendor/firmware/$f" "$OUT/$f" >/dev/null 2>&1; then
        echo "got $f"
    else
        echo "MISSING on device: /vendor/firmware/$f" >&2
    fi
done
ls -la "$OUT"
