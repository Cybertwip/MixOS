# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# OPPO A77 4G (MT6765) firmware: unconfirmed on this model. The names
# below are the proven oppo-mt6877 set (same vendor, same MTK CONSYS/modem
# architecture) -- expected, not measured. The script reports misses
# loudly and asks for a /vendor/firmware listing so it learns.
set -Eeuo pipefail

OUT="$(cd -- "$(dirname -- "$0")" && pwd)/stock"
mkdir -p "$OUT"
command -v adb >/dev/null || { echo "error: adb is not installed" >&2; exit 1; }

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

SEARCH_DIRS=(
    /vendor/firmware
    /vendor/etc/firmware
    /odm/etc/firmware
    /system/etc/firmware
    /vendor/firmware_mnt
)
WANT=( modem.img dsp.img WMT_SOC.cfg )
MISSING=()
remote_ls() { adb shell "ls '$1' 2>/dev/null" | tr -d '\r'; }

find_remote() {
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

for f in "${WANT[@]}"; do
    if remote="$(find_remote "$f")"; then
        echo "-- $f  (at $remote)"
        adb pull "$remote" "$OUT/$f"
    else
        echo "MISSING on device: $f (searched ${SEARCH_DIRS[*]})" >&2
        MISSING+=( "$f" )
    fi
done

WIFI_FROM=""
for dir in "${SEARCH_DIRS[@]}"; do
    while IFS= read -r f; do
        [[ "$f" == WIFI_RAM_CODE* ]] || continue
        WIFI_FROM="$dir/$f"
        break 2
    done < <(remote_ls "$dir")
done
if [[ -n "$WIFI_FROM" ]]; then
    echo "-- $WIFI_FROM  (saving as WIFI_RAM_CODE)"
    adb pull "$WIFI_FROM" "$OUT/WIFI_RAM_CODE"
else
    echo "MISSING on device: WIFI_RAM_CODE* (searched ${SEARCH_DIRS[*]})" >&2
    MISSING+=( "WIFI_RAM_CODE" )
fi

echo
ls -la "$OUT"
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) from $SERIAL"
    for f in modem.img dsp.img WIFI_RAM_CODE WMT_SOC.cfg; do
        [[ -f "$OUT/$f" ]] || continue
        printf '%s  %s  %s\n' \
            "$(shasum -a 256 "$OUT/$f" | cut -d' ' -f1)" \
            "$(stat -f%z "$OUT/$f" 2>/dev/null || stat -c%s "$OUT/$f")" \
            "$f"
    done
} > "$OUT/MANIFEST.txt"

if [[ "${#MISSING[@]}" -gt 0 ]]; then
    echo >&2
    echo "error: ${#MISSING[@]} file(s) not found: ${MISSING[*]}" >&2
    echo "MT6765 firmware names are priors, not facts -- send" >&2
    echo "'adb shell ls /vendor/firmware' and the script gets taught." >&2
    exit 1
fi
echo
echo "All blobs extracted. Build with:"
echo "  A77_4G_FIRMWARE_DIR=$OUT OPPO_A77_4G_4G_BRINGUP_ACK=1 sh ./build-oppo-a77-4g.sh"
