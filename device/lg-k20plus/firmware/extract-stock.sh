#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Pull the LG K20 Plus coprocessor blobs off a stock phone over adb into
# ./stock/ (git-ignored), then point the build at them with:
#   LG_FIRMWARE_DIR=device/lg-k20plus/firmware/stock sh ./build-lg.sh
#
# The blobs are Qualcomm/LG proprietary binaries -- there is no source and
# no redistributable copy, so the phone you are building for is the source.
# /firmware/image is usually world-readable; only the /persist NV file
# needs root (adb root), and the script says so when it cannot read it.
set -Eeuo pipefail

OUT="$(cd -- "$(dirname -- "$0")" && pwd)/stock"
mkdir -p "$OUT"
command -v adb >/dev/null || { echo "error: adb is not installed" >&2; exit 1; }

# One -s flag for the whole script when several devices are attached.
SERIAL="${ANDROID_SERIAL:-}"
if [[ -z "$SERIAL" ]]; then
    mapfile -t DEVS < <(adb devices | awk '$2=="device"{print $1}')
    if [[ "${#DEVS[@]}" -eq 0 ]]; then
        echo "error: no adb device (enable USB debugging, plug in, accept)" >&2
        exit 1
    elif [[ "${#DEVS[@]}" -gt 1 ]]; then
        echo "error: several devices; rerun with ANDROID_SERIAL=<one of: ${DEVS[*]}>" >&2
        exit 1
    fi
    SERIAL="${DEVS[0]}"
fi
adb() { command adb -s "$SERIAL" "$@"; }
adb wait-for-device

# /firmware/image is the classic QC mount (usually a symlink to the
# vendor firmware mount); the rest are where it moved across releases.
SEARCH_DIRS=(
    /firmware/image
    /vendor/firmware_mnt/image
    /vendor/firmware
    /vendor/etc/firmware
    /etc/firmware
)

MISSING=()
remote_ls() { adb shell "ls '$1' 2>/dev/null" | tr -d '\r'; }

# pull_set $base: the .mdt header plus EVERY segment the phone holds for
# it, found by listing -- never a hardcoded b00..bNN range, because the
# count varies between builds and one missing segment fails PIL auth with
# an error that looks exactly like a driver bug.
pull_set() {
    local base=$1 dir f found_mdt="" nseg=0
    for dir in "${SEARCH_DIRS[@]}"; do
        while IFS= read -r f; do
            case "$f" in
                "$base.mdt")
                    echo "-- $f  (at $dir/$f)"
                    adb pull "$dir/$f" "$OUT/$f"
                    found_mdt="$dir/$f" ;;
                "$base".b*)
                    adb pull "$dir/$f" "$OUT/$f" >/dev/null
                    nseg=$((nseg + 1)) ;;
            esac
        done < <(remote_ls "$dir")
        [[ -n "$found_mdt" ]] && break
    done
    if [[ -z "$found_mdt" ]]; then
        echo "MISSING on device: $base.mdt (searched ${SEARCH_DIRS[*]})" >&2
        MISSING+=( "$base.mdt" )
    elif [[ "$nseg" -eq 0 ]]; then
        echo "MISSING on device: no $base.bXX segments next to $found_mdt" >&2
        MISSING+=( "$base.bXX" )
    else
        echo "   $base: header + $nseg segments"
    fi
}

pull_set modem
pull_set wcnss

# Per-unit WLAN calibration. /persist is root-only on stock, so this is
# best-effort: PIL boots without it, but the radio keeps its defaults.
if adb pull /persist/WCNSS_qcom_wlan_nv.bin \
        "$OUT/WCNSS_qcom_wlan_nv.bin" >/dev/null 2>&1; then
    echo "-- WCNSS_qcom_wlan_nv.bin  (at /persist/WCNSS_qcom_wlan_nv.bin)"
else
    echo "WARNING: cannot read /persist/WCNSS_qcom_wlan_nv.bin" >&2
    echo "  (needs 'adb root'; WLAN calibration stays at defaults without it)" >&2
fi

echo
echo "== stock/ =="
ls -la "$OUT"
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) from $SERIAL"
    for f in "$OUT"/modem.* "$OUT"/wcnss.* "$OUT"/WCNSS_qcom_wlan_nv.bin; do
        [[ -f "$f" ]] || continue
        printf '%s  %s  %s\n' \
            "$(shasum -a 256 "$f" | cut -d' ' -f1)" \
            "$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")" \
            "$(basename "$f")"
    done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"

if [[ "${#MISSING[@]}" -gt 0 ]]; then
    echo >&2
    echo "error: incomplete PIL sets: ${MISSING[*]}" >&2
    echo "Check 'adb shell ls /firmware/image' -- if the files are there but" >&2
    echo "unreadable, the phone needs root (adb root); if they are absent," >&2
    echo "unpack them from a stock .kdz with an offline kdz extractor instead." >&2
    exit 1
fi
echo
echo "Complete modem + wcnss sets extracted. Build with:"
echo "  LG_FIRMWARE_DIR=$OUT sh ./build-lg.sh"
