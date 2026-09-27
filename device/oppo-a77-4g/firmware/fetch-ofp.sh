#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Offline route for the OPPO blobs: decrypt a stock .ofp with oppo_decrypt
# (MTK OFP decrypter, MIT, github.com/oneseeker279/oppo_decrypt) and lift
# modem.img, dsp.img, WIFI_RAM_CODE and WMT_SOC.cfg out of the vendor image
# into ./stock/ (git-ignored). Usage:
#   ./fetch-ofp.sh /path/to/CPH2385.ofp
#   A77_4G_FIRMWARE_DIR=device/oppo-a77-4g/firmware/stock OPPO_A77_4G_4G_BRINGUP_ACK=1 sh ./build-oppo-a77-4g.sh
#
# The .ofp is OPPO's property (several GB, from the official firmware
# downloads or a community mirror), so the download itself is a browser
# job and this script starts from the file. What lands in stock/ has the
# same status as an adb extraction: device firmware for local builds,
# never committed.
set -Eeuo pipefail

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
OFP="${1:?usage: fetch-ofp.sh STOCK.ofp}"
[[ -f "$OFP" ]] || { echo "error: no such file: $OFP" >&2; exit 1; }
OUT="$HERE/stock"
TOOLS="$HERE/.tools/oppo_decrypt"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/fetch-ofp.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

command -v python3 >/dev/null || { echo "error: python3 missing" >&2; exit 1; }
command -v git >/dev/null || { echo "error: git missing" >&2; exit 1; }
if command -v 7z >/dev/null; then P7Z=7z
elif command -v 7zz >/dev/null; then P7Z=7zz
else echo "error: 7z missing (brew install p7zip)" >&2; exit 1; fi
python3 -c "import Cryptodome" 2>/dev/null || {
    echo "-- installing pycryptodomex (OFP decryption needs Cryptodome)"
    python3 -m pip install --user -q pycryptodomex \
        || { echo "error: pip install failed; install pycryptodomex by hand" >&2; exit 1; }
}

if [[ ! -f "$TOOLS/ofp_mtk_decrypt.py" ]]; then
    echo "-- cloning oppo_decrypt (cached under firmware/.tools/)"
    mkdir -p "$(dirname "$TOOLS")"
    git clone -q https://github.com/oneseeker279/oppo_decrypt.git "$TOOLS"
fi

echo "-- OFP -> partition images"
python3 "$TOOLS/ofp_mtk_decrypt.py" "$OFP" "$WORK/ofp" >/dev/null
# The decrypter exits 0 even on "Unknown key", leaving nothing behind --
# an empty outdir is the real failure signal, so check it, not the exit.
if [[ -z "$(ls "$WORK/ofp" 2>/dev/null)" ]]; then
    echo "error: nothing decrypted (this OFP's keys are unknown to the tool;" >&2
    echo "  newer builds rotate them -- extract from the phone instead)" >&2
    exit 1
fi
VENDOR="$(ls "$WORK/ofp" | grep -i '^vendor.*\.img$' | head -n 1)"
if [[ -z "$VENDOR" ]]; then
    echo "error: no vendor image among:" >&2; ls "$WORK/ofp" >&2; exit 1
fi
echo "-- vendor image: $VENDOR"
"$P7Z" x -o"$WORK/vendor" "$WORK/ofp/$VENDOR" firmware/ >/dev/null 2>&1 \
    || "$P7Z" x -o"$WORK/vendor" "$WORK/ofp/$VENDOR" >/dev/null \
    || { echo "error: 7z could not open $VENDOR" >&2; exit 1; }

mkdir -p "$OUT"
found=0
for f in modem.img dsp.img WMT_SOC.cfg; do
    hit="$(find "$WORK/vendor" -name "$f" 2>/dev/null | head -n 1)"
    [[ -n "$hit" ]] || { echo "MISSING in vendor image: $f" >&2; continue; }
    cp "$hit" "$OUT/$f"
    found=$((found + 1))
done
hit="$(find "$WORK/vendor" -name 'WIFI_RAM_CODE*' 2>/dev/null | head -n 1)"
if [[ -n "$hit" ]]; then
    cp "$hit" "$OUT/WIFI_RAM_CODE"
    found=$((found + 1))
else
    echo "MISSING in vendor image: WIFI_RAM_CODE*" >&2
fi
if [[ "$found" -eq 0 ]]; then
    echo "error: none of the four blobs are in $VENDOR" >&2
    echo "  (some builds keep modem.img in the md1img partition instead;" >&2
    echo "   extract from the phone with extract-stock.sh)" >&2
    exit 1
fi
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(basename "$OFP")"
    for f in modem.img dsp.img WIFI_RAM_CODE WMT_SOC.cfg; do
        [[ -f "$OUT/$f" ]] || continue
        printf '%s  %s  %s  (from %s)\n' \
            "$(shasum -a 256 "$OUT/$f" | cut -d' ' -f1)" \
            "$(stat -f%z "$OUT/$f" 2>/dev/null || stat -c%s "$OUT/$f")" \
            "$f" "$VENDOR"
    done
} > "$OUT/MANIFEST.txt"
echo "-- $found files into $OUT"
echo "Build with: A77_4G_FIRMWARE_DIR=$OUT OPPO_A77_4G_4G_BRINGUP_ACK=1 sh ./build-oppo-a77-4g.sh"
