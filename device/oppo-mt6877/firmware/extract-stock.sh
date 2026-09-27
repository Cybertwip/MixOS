#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Pull the OPPO MT6877 coprocessor blobs off a stock phone over adb into
# ./stock/ (git-ignored), then point the build at them with:
#   OPPO_FIRMWARE_DIR=device/oppo-mt6877/firmware/stock sh ./build-oppo.sh
#
# The blobs are OPPO/MediaTek proprietary binaries -- there is no source and
# no redistributable copy, so the phone you are building for is the source,
# exactly like the J36 blobs vendored under device/j36-ultra/firmware/.
# Stock firmware directories are usually world-readable, so this often works
# on an unrooted phone; root is only needed if your build hid them.
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

# Where stock images keep coprocessor firmware. /vendor/firmware is the
# usual home on this phone; the rest are where OEMs move things between
# Android releases, so every file is searched in each before giving up.
SEARCH_DIRS=(
    /vendor/firmware
    /vendor/etc/firmware
    /odm/etc/firmware
    /system/etc/firmware
    /vendor/firmware_mnt
)

# file on stock -> name the MixOS drivers ask request_firmware() for
WANT=( modem.img dsp.img WMT_SOC.cfg )
declare -A FOUND=()
MISSING=()

remote_ls() { adb shell "ls '$1' 2>/dev/null" | tr -d '\r'; }

find_remote() { # $1 = basename -> prints dir/file or nothing
    local want=$1 dir f
    for dir in "${SEARCH_DIRS[@]}"; do
        while IFS= read -r f; do
            [[ "$f" == "$want" ]] || continue
            if adb shell "test -f '$dir/$f'" >/dev/null 2>&1; then
                printf '%s/%s\n' "$dir" "$f"
                return 0
            fi
        done < <(remote_ls "$dir")
    done
    return 1
}

pull_one() { # $1 = stock basename, $2 = local name
    local want=$1 local=$2 remote
    if remote="$(find_remote "$want")"; then
        echo "-- $want  (at $remote)"
        adb pull "$remote" "$OUT/$local"
        FOUND["$local"]="$remote"
    else
        echo "MISSING on device: $want (searched ${SEARCH_DIRS[*]})" >&2
        MISSING+=( "$want" )
    fi
}

for f in "${WANT[@]}"; do
    pull_one "$f" "$f"
done

# The Wi-Fi RAM code ships with a chip suffix (WIFI_RAM_CODE_SOC on this
# phone); the driver asks for the bare name, so match by prefix and save
# the one hit under it.
WIFI_HIT=""
WIFI_FROM=""
for dir in "${SEARCH_DIRS[@]}"; do
    while IFS= read -r f; do
        [[ "$f" == WIFI_RAM_CODE* ]] || continue
        WIFI_FROM="$dir/$f"
        WIFI_HIT="$f"
        break 2
    done < <(remote_ls "$dir")
done
if [[ -n "$WIFI_HIT" ]]; then
    echo "-- $WIFI_HIT  (at $WIFI_FROM, saving as WIFI_RAM_CODE)"
    adb pull "$WIFI_FROM" "$OUT/WIFI_RAM_CODE"
    FOUND["WIFI_RAM_CODE"]="$WIFI_FROM"
else
    echo "MISSING on device: WIFI_RAM_CODE* (searched ${SEARCH_DIRS[*]})" >&2
    MISSING+=( "WIFI_RAM_CODE" )
fi

echo
echo "== stock/ =="
ls -la "$OUT"
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) from $SERIAL"
    for f in modem.img dsp.img WIFI_RAM_CODE WMT_SOC.cfg; do
        [[ -f "$OUT/$f" ]] || continue
        printf '%s  %s  %s  (from %s)\n' \
            "$(shasum -a 256 "$OUT/$f" | cut -d' ' -f1)" \
            "$(stat -f%z "$OUT/$f" 2>/dev/null || stat -c%s "$OUT/$f")" \
            "$f" "${FOUND[$f]}"
    done
} > "$OUT/MANIFEST.txt"
cat "$OUT/MANIFEST.txt"

if [[ "${#MISSING[@]}" -gt 0 ]]; then
    echo >&2
    echo "error: ${#MISSING[@]} file(s) not found: ${MISSING[*]}" >&2
    echo "If 'adb shell ls /vendor/firmware' shows them, the phone needs root" >&2
    echo "(adb root) so the pull can read them; otherwise they live somewhere" >&2
    echo "this script does not know -- send the listing and it gets taught." >&2
    exit 1
fi
echo
echo "All four blobs extracted. Build with:"
echo "  OPPO_FIRMWARE_DIR=$OUT sh ./build-oppo.sh"
