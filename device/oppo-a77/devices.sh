# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# The OPPO A77 5G family: MediaTek Dimensity 810 (MT6833), one row per device.
# Sourced by build-oppo-a77.sh (host) and device/oppo-a77/build-in-vm.sh (VM).
# Usage:  a77_device_info <codename>     # prints KEY=VALUE lines to stdout
#         a77_devices                    # prints supported codenames, one per line
#
# Columns: codename|soc|pmic|arch|mem_mb|width|height|notes
#   mem_mb/width/height come from retail specs (GSMArena + launch coverage:
#   6.56in 720x1612 90Hz, Dimensity 810, 4 or 6GB RAM, Android 12), NOT from
#   hardware. Two rows because RAM ships in two sizes: the 4GB row is the
#   default (safe direction -- describing less RAM than exists only wastes
#   it; describing more crashes). Confirm your unit in Settings > About
#   (BRINGUP.md step 0) and pick the matching row. Panel/touch models are
#   still unknown (BRINGUP.md step 2 resolves them from the stock DTB).

OPPO_A77_DEVICES='
cph2381|mt6833|unknown|arm64|4096|720|1612|OPPO A77 5G (CPH2381) 4GB: Dimensity 810 (MT6833), 6.56in 720x1612, 4GB/64GB, Android 12 (default row: safe on 6GB units too, wastes 2GB)
cph2381-6gb|mt6833|unknown|arm64|6144|720|1612|OPPO A77 5G (CPH2381) 6GB: same phone, 6GB/128GB (use only if Settings > About says 6GB)
'

# Single-quote one value for eval consumption. Works in POSIX sh and bash.
_a77_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

a77_devices() {
    printf '%s\n' "$OPPO_A77_DEVICES" | grep '|' | cut -d'|' -f1
}

a77_default_device() {
    printf 'cph2381\n'
}

a77_device_info() {
    local want="$1" row
    row="$(printf '%s\n' "$OPPO_A77_DEVICES" | grep -m1 "^${want}|")"
    if [ -z "$row" ]; then
        echo "unknown A77 device '$want' (supported: $(a77_devices | tr '\n' ' '))" >&2
        return 1
    fi
    printf 'A77_DEVICE=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f1)")"
    printf 'A77_SOC=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f2)")"
    printf 'A77_PMIC=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f3)")"
    printf 'A77_ARCH=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f4)")"
    printf 'A77_MEM_MB=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f5)")"
    printf 'A77_WIDTH=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f6)")"
    printf 'A77_HEIGHT=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f7)")"
    printf 'A77_NOTES=%s\n' "$(_a77_quote "$(printf '%s' "$row" | cut -d'|' -f8)")"
}
