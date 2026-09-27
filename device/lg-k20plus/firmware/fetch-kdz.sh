#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Offline route for the LG PIL sets: unpack a stock .kdz with kdztools
# (LG KDZ/DZ utilities, GPL, github.com/ehem/kdztools) and lift modem/wcnss
# out of the modem partition image into ./stock/ (git-ignored). Usage:
#   ./fetch-kdz.sh /path/to/MP26011K_00.kdz
#   LG_FIRMWARE_DIR=device/lg-k20plus/firmware/stock sh ./build-lg.sh
#
# The .kdz is LG's property (~2 GB, wait-walled on the ROM mirrors), so the
# download itself is a browser job and this script starts from the file.
# What lands in stock/ has the same status as an adb extraction: device
# firmware for local builds, never committed.
set -Eeuo pipefail

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
KDZ="${1:?usage: fetch-kdz.sh STOCK.kdz}"
[[ -f "$KDZ" ]] || { echo "error: no such file: $KDZ" >&2; exit 1; }
OUT="$HERE/stock"
TOOLS="$HERE/.tools/kdztools"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fetch-kdz.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

command -v python3 >/dev/null || { echo "error: python3 missing" >&2; exit 1; }
command -v git >/dev/null || { echo "error: git missing" >&2; exit 1; }
if command -v 7z >/dev/null; then P7Z=7z
elif command -v 7zz >/dev/null; then P7Z=7zz
else echo "error: 7z missing (brew install p7zip)" >&2; exit 1; fi

if [[ ! -f "$TOOLS/unkdz.py" ]]; then
    echo "-- cloning kdztools (cached under firmware/.tools/)"
    mkdir -p "$(dirname "$TOOLS")"
    git clone -q https://github.com/ehem/kdztools.git "$TOOLS"
fi

echo "-- KDZ -> DZ"
mkdir -p "$WORK/kdz" && cd "$WORK/kdz"
python3 "$TOOLS/unkdz.py" -f "$KDZ" -x >/dev/null
DZ_NAME="$(ls kdzextracted/*.dz ./*.dz 2>/dev/null | head -n 1)"
[[ -n "$DZ_NAME" ]] || { echo "error: no .dz came out of the .kdz" >&2; exit 1; }
DZ="$WORK/kdz/$DZ_NAME"

echo "-- DZ -> partitions"
mkdir -p "$WORK/dz" && cd "$WORK/dz"
python3 "$TOOLS/undz.py" -f "$DZ" -x >/dev/null
PARTDIR="$WORK/dz/dzextracted"
[[ -d "$PARTDIR" ]] || PARTDIR="$WORK/dz"
MODEM_BIN="$(ls "$PARTDIR" | grep -i modem | head -n 1)"
if [[ -z "$MODEM_BIN" ]]; then
    echo "error: no modem partition among:" >&2; ls "$PARTDIR" >&2; exit 1
fi
echo "-- modem partition: $MODEM_BIN"
"$P7Z" x -o"$WORK/fw" "$PARTDIR/$MODEM_BIN" >/dev/null \
    || { echo "error: 7z could not open $MODEM_BIN" >&2; exit 1; }

mkdir -p "$OUT"
found=0
for f in "$WORK/fw"/modem.mdt "$WORK/fw"/modem.b* \
         "$WORK/fw"/wcnss.mdt "$WORK/fw"/wcnss.b*; do
    [[ -f "$f" ]] || continue
    cp "$f" "$OUT/"
    found=$((found + 1))
done
if [[ "$found" -eq 0 ]]; then
    echo "error: no modem/wcnss files inside $MODEM_BIN:" >&2
    ls "$WORK/fw" | head >&2; exit 1
fi
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(basename "$KDZ")"
    for f in "$OUT"/modem.* "$OUT"/wcnss.*; do
        [[ -f "$f" ]] || continue
        printf '%s  %s  %s\n' \
            "$(shasum -a 256 "$f" | cut -d' ' -f1)" \
            "$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")" \
            "$(basename "$f")"
    done
} > "$OUT/MANIFEST.txt"
echo "-- $found files into $OUT"
echo "Build with: LG_FIRMWARE_DIR=$OUT sh ./build-lg.sh"
