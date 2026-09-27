# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# The LG K20 (2019) family: MediaTek MT6739 (mt67xx), one row per device.
# Sourced by build-lg-k20.sh (host) and device/lg-k20/build-in-vm.sh (VM).
# Usage:  k20_device_info <codename>     # prints KEY=VALUE lines to stdout
#         k20_devices                    # prints supported codenames, one per line
#
# Columns: codename|soc|pmic|arch|mem_mb|width|height|notes
#   mem_mb/width/height come from retail specs (GSMArena, Icecat LMX120EMW
#   datasheet), NOT from hardware: panel/touch models are still unknown
#   (BRINGUP.md step 2 resolves them from the stock DTB).

LG_K20_DEVICES='
lm-x120|mt6739|mt6357|arm64|1024|480|960|LG K20 2019 (LM-X120EMW): MT6739, 5.45in 480x960, 1GB/16GB, Android 9 Go (PMIC is the standard MT6739 pairing, confirm from stock DTB)
'

# Single-quote one value for eval consumption. Works in POSIX sh and bash.
_k20_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

k20_devices() {
    printf '%s\n' "$LG_K20_DEVICES" | grep '|' | cut -d'|' -f1
}

k20_default_device() {
    printf 'lm-x120\n'
}

k20_device_info() {
    local want="$1" row
    row="$(printf '%s\n' "$LG_K20_DEVICES" | grep -m1 "^${want}|")"
    if [ -z "$row" ]; then
        echo "unknown K20 device '$want' (supported: $(k20_devices | tr '\n' ' '))" >&2
        return 1
    fi
    printf 'K20_DEVICE=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f1)")"
    printf 'K20_SOC=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f2)")"
    printf 'K20_PMIC=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f3)")"
    printf 'K20_ARCH=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f4)")"
    printf 'K20_MEM_MB=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f5)")"
    printf 'K20_WIDTH=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f6)")"
    printf 'K20_HEIGHT=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f7)")"
    printf 'K20_NOTES=%s\n' "$(_k20_quote "$(printf '%s' "$row" | cut -d'|' -f8)")"
}
