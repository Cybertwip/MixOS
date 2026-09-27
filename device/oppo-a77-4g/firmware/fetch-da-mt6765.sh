#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# MT6765 Download Agent for BROM work on the OPPO A77 4G (CPH2385).
# A Download Agent is MediaTek's property, so like the .ofp route this
# script verifies bit for bit and lifts just this SoC's entry out of the
# AllInOne bundle into ./stock/ (git-ignored):
#   ./fetch-da-mt6765.sh [--download | MT6765_DA_File.zip]
#   ./flash -da-loader device/oppo-a77-4g/firmware/stock/MTK_DA_mt6765.bin ...
# With no argument it expects the zip beside it (a browser job -- the host
# discourages hotlinking); --download fetches the pinned copy instead.
# What lands in stock/ has the same status as an adb extraction: device
# firmware for local builds, never committed.
set -Eeuo pipefail

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
OUT="$HERE/stock"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fetch-da-mt6765.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

ZIP_URL="https://androiddatahost.com/wp-content/uploads/MT6765_DA_File.zip"
ZIP_SIZE=8992904
ZIP_SHA256="47026f10322cd1801730c29f15e5dad668d0ec9abc98b909c2291c375870dcd5"
INNER="MT6765_DA_File/MT6765.bin"
INNER_SIZE=15226924
INNER_SHA256="a908397e129c27ddc0fc45f7550ac3085e8060c504288f25ddc7b35ab5be37b9"

command -v python3 >/dev/null || { echo "error: python3 missing" >&2; exit 1; }
if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
fi
check() { # $1 = file, $2 = size, $3 = sha256, $4 = label
    size="$(wc -c < "$1" | tr -d ' ')"
    [[ "$size" == "$2" ]] || { echo "error: $4 is $size bytes, want $2" >&2; exit 1; }
    got="$(sha256 "$1")"
    [[ "$got" == "$3" ]] || { echo "error: $4 hash mismatch: $got" >&2; exit 1; }
}

ZIP=""
if [[ "${1:-}" == "--download" ]]; then
    command -v curl >/dev/null || { echo "error: curl missing" >&2; exit 1; }
    echo "-- downloading $ZIP_URL"
    curl -sSL -o "$WORK/da.zip" "$ZIP_URL" \
        || { echo "error: download failed (fetch the zip in a browser and pass its path)" >&2; exit 1; }
    ZIP="$WORK/da.zip"
elif [[ -n "${1:-}" ]]; then
    [[ -f "$1" ]] || { echo "error: no such file: $1" >&2; exit 1; }
    ZIP="$1"
else
    echo "usage: fetch-da-mt6765.sh [--download | MT6765_DA_File.zip]" >&2
    exit 2
fi

echo "-- verifying $ZIP"
check "$ZIP" "$ZIP_SIZE" "$ZIP_SHA256" "DA zip"
echo "-- unpacking $INNER"
python3 - "$ZIP" "$WORK/MT6765.bin" "$INNER" <<'PY'
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    with z.open(sys.argv[3]) as src, open(sys.argv[2], "wb") as dst:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            dst.write(chunk)
PY
check "$WORK/MT6765.bin" "$INNER_SIZE" "$INNER_SHA256" "AllInOne DA"
echo "-- lifting the MT6765 entry"
mkdir -p "$OUT"
python3 "$HERE/extract-mtk-da.py" "$WORK/MT6765.bin" 0x6765 "$OUT/MTK_DA_mt6765.bin"
echo "-- DA ready: $OUT/MTK_DA_mt6765.bin"
echo "   use: ./flash -da-loader $OUT/MTK_DA_mt6765.bin ..."
