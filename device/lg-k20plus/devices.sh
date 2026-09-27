# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# The LG phone family: one row per device. Unlike the OPPO family this one is
# Qualcomm (MSM8917 here); the columns are the same shape on purpose so
# build-mixos.sh treats both families alike.
#
# Sourced by build-lg.sh (host) and device/lg-k20plus/build-in-vm.sh (VM).
# Usage:  lg_device_info <codename>     # prints KEY=VALUE lines to stdout
#         lg_devices                    # prints supported codenames, one per line
#
# Columns: codename|soc|arch|panel|touch|modem|rev|notes
#   panel: lg4894 (LGD) or td4100 (Tovis second source); the driver carries
#   both and the DTS picks by LG_PANEL. touch follows the panel.

LG_DEVICES='
lv517|msm8917|arm64|lg4894|lg4894|q6v5-mss|b|LG K20 Plus (MP260), rev-b, LGD panel
lv517-rev0|msm8917|arm64|lg4894|lg4894|q6v5-mss|0|LG K20 Plus rev-0 (GPIO91 reads VOL_DOWN, PMIC key is VOL_UP)
lv517-tovis|msm8917|arm64|td4100|td4100|q6v5-mss|b|LG K20 Plus with Tovis TD4100 panel + Synaptics touch
'

# Single-quote one value for eval consumption. Works in POSIX sh and bash.
_lg_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

lg_devices() {
    printf '%s\n' "$LG_DEVICES" | grep '|' | cut -d'|' -f1
}

lg_default_device() {
    printf 'lv517\n'
}

lg_device_info() {
    local want="$1" row
    row="$(printf '%s\n' "$LG_DEVICES" | grep -m1 "^${want}|")"
    if [ -z "$row" ]; then
        echo "unknown LG device '$want' (supported: $(lg_devices | tr '\n' ' '))" >&2
        return 1
    fi
    printf 'LG_DEVICE=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f1)")"
    printf 'LG_SOC=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f2)")"
    printf 'LG_ARCH=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f3)")"
    printf 'LG_PANEL=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f4)")"
    printf 'LG_TOUCH=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f5)")"
    printf 'LG_MODEM=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f6)")"
    printf 'LG_REV=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f7)")"
    printf 'LG_NOTES=%s\n' "$(_lg_quote "$(printf '%s' "$row" | cut -d'|' -f8)")"
}
