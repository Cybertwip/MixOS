# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# The OPPO A77 4G family: MediaTek Helio G35 (MT6765), one row per device.
# Sourced by build-oppo-a77-4g.sh (host) and device/oppo-a77-4g/build-in-vm.sh (VM).
# Usage:  a77_4g_device_info <codename>     # prints KEY=VALUE lines to stdout
#         a77_4g_devices                    # prints supported codenames, one per line
#
# Columns: codename|soc|pmic|arch|mem_mb|width|height|notes
#   mem_mb/width/height come from retail specs (GSMArena + launch coverage:
#   6.56in 720x1612 60Hz, Helio G35, 4 or 6GB RAM, Android 12), NOT from
#   hardware. Two rows because RAM ships in two sizes: the 4GB row is the
#   default (safe direction -- describing less RAM than exists only wastes
#   it; describing more crashes). Confirm your unit in Settings > About
#   (BRINGUP.md step 0) and pick the matching row. Panel/touch models are
#   still unknown (BRINGUP.md step 2 resolves them from the stock DTB).

OPPO_A77_4G_4G_DEVICES='
cph2385|mt6765|unknown|arm64|4096|720|1612|OPPO A77 4G (CPH2385) 4GB: Helio G35 (MT6765), 6.56in 720x1612, 4GB/64GB, Android 12 (default row: safe on 6GB units too, wastes 2GB)
cph2385-4gb|mt6765|unknown|arm64|6144|720|1612|OPPO A77 4G (CPH2385) 6GB: same phone, 6GB/128GB (use only if Settings > About says 6GB)
'

# Single-quote one value for eval consumption. Works in POSIX sh and bash.
_a77_4g_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

a77_4g_devices() {
    printf '%s\n' "$OPPO_A77_4G_4G_DEVICES" | grep '|' | cut -d'|' -f1
}

a77_4g_default_device() {
    printf 'cph2385\n'
}

a77_4g_device_info() {
    local want="$1" row
    row="$(printf '%s\n' "$OPPO_A77_4G_4G_DEVICES" | grep -m1 "^${want}|")"
    if [ -z "$row" ]; then
        echo "unknown A77 device '$want' (supported: $(a77_4g_devices | tr '\n' ' '))" >&2
        return 1
    fi
    printf 'A77_4G_DEVICE=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f1)")"
    printf 'A77_4G_SOC=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f2)")"
    printf 'A77_4G_PMIC=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f3)")"
    printf 'A77_4G_ARCH=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f4)")"
    printf 'A77_4G_MEM_MB=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f5)")"
    printf 'A77_4G_WIDTH=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f6)")"
    printf 'A77_4G_HEIGHT=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f7)")"
    printf 'A77_4G_NOTES=%s\n' "$(_a77_4g_quote "$(printf '%s' "$row" | cut -d'|' -f8)")"
}
