# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# The OPPO phone family: one row per device, all MediaTek.
#
# Sourced by build-oppo.sh (host) and device/oppo-mt6877/build-in-vm.sh (VM).
# Usage:  oppo_device_info <codename>   # prints KEY=VALUE lines to stdout
#         oppo_devices                  # prints supported codenames, one per line
#
# Adding a device later means appending one row to OPPO_DEVICES below plus its
# panel/touch notes to device/oppo-mt6877/board/.  Nothing else moves: the VM
# build reads this file through the synced checkout.
#
# Columns: codename|soc|arch|panel|touch|modem|notes
#   panel/touch name the linux/ driver + board extract; "sfb" means the device
#   boots on the bootloader's framebuffer until its panel driver lands.

OPPO_DEVICES='
20181|mt6877|arm64|td4330|nt36672|eccci|OPPO Reno6 5G class (oplus6877_20181), primary bring-up target
20183|mt6877|arm64|td4330|nt36672|eccci|oplus6877_20183, same SoC/panel family as 20181
20355|mt6877|arm64|sfb|nt36672|eccci|oplus6877_20355, panel TBD from its dts; boots on simple-framebuffer
'

# Single-quote one value for eval consumption. Works in POSIX sh and bash.
_oppo_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

oppo_devices() {
    printf '%s\n' "$OPPO_DEVICES" | grep '|' | cut -d'|' -f1
}

oppo_default_device() {
    printf '20181\n'
}

# Prints shell-assignable facts for one codename; exits nonzero when unknown.
oppo_device_info() {
    local want="$1" row
    row="$(printf '%s\n' "$OPPO_DEVICES" | grep -m1 "^${want}|")"
    if [ -z "$row" ]; then
        echo "unknown OPPO device '$want' (supported: $(oppo_devices | tr '\n' ' '))" >&2
        return 1
    fi
    printf 'OPPO_DEVICE=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f1)")"
    printf 'OPPO_SOC=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f2)")"
    printf 'OPPO_ARCH=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f3)")"
    printf 'OPPO_PANEL=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f4)")"
    printf 'OPPO_TOUCH=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f5)")"
    printf 'OPPO_MODEM=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f6)")"
    printf 'OPPO_NOTES=%s\n' "$(_oppo_quote "$(printf '%s' "$row" | cut -d'|' -f7)")"
}
