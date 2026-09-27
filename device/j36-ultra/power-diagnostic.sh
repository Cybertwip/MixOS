#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Sourced by /init. Opt-in with j36.diag=power; no register writes here.
# mount_bootfs must be defined. Enable writes only after expansion is disabled
# or has returned; every borrowed BOOT mount is released after its checkpoint.
power_diag_ready=0
power_diag_seq=0

power_diag_checkpoint() {
    [ "$power_diag_ready" = 1 ] || return 0
    power_diag_had_mount=$bootfs_mounted
    if ! mount_bootfs || ! mount -o remount,rw /bootfs; then
        say "power diagnostic: cannot write BOOT"
        return 0
    fi
    power_diag_seq=$((power_diag_seq + 1))
    power_diag_slot=$((power_diag_seq % 2))
    # Alternate files so a cut during this write does not truncate the last
    # checkpoint. Sequence and boot ID distinguish old boots and partial files.
    {
        echo "J36 power diagnostic v6 (resize bypassed)"
        echo "sequence=$power_diag_seq stage=$*"
        echo "boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
        echo "uptime=$(cat /proc/uptime)"
        uname -a
        cat /proc/cmdline
        echo "root=$rootdev filesystem=$rootfs_type"
        echo "external_power=$(cat /sys/module/j36_mt6592_pmic/parameters/external_power 2>/dev/null)"
        for power_diag_file in /sys/class/power_supply/usb/power_snapshot \
            /sys/class/power_supply/usb/online \
            /sys/class/power_supply/usb/voltage_now \
            /sys/class/power_supply/battery/voltage_now; do
            echo "$power_diag_file:"
            cat "$power_diag_file" 2>/dev/null || echo unavailable
        done
        # The in-flight module and the wedge mark, so a board that dies
        # between two checkpoints still left its step behind. Both are empty
        # before the first stage runs, which reads the same as missing.
        for power_diag_state in "watch_step:${watch_status:-/dev/.watch-status}" \
                "wedge_mark:${watch_markfile:-/newroot/opt/mixos/boot-stage}"; do
            power_diag_key="${power_diag_state%%:*}"
            power_diag_path="${power_diag_state#*:}"
            if [ -s "$power_diag_path" ]; then
                echo "$power_diag_key=$(cat "$power_diag_path")"
            else
                echo "$power_diag_key=unavailable"
            fi
        done
        echo "--- init trace ---"
        cat /dev/j36-init-trace 2>/dev/null
        echo "--- kernel log ---"
        dmesg
        echo "checkpoint_complete=$power_diag_seq"
    } > "/bootfs/j36-power-$power_diag_slot.txt"
    power_diag_rc=$?
    sync
    mount -o remount,ro /bootfs || say "power diagnostic: BOOT remount read-only failed"
    if [ "$power_diag_had_mount" != 1 ]; then
        if umount /bootfs; then
            bootfs_mounted=0
        else
            say "power diagnostic: BOOT unmount failed"
        fi
    fi
    [ "$power_diag_rc" = 0 ] || say "power diagnostic: checkpoint write failed"
    return 0
}

power_diag_start() {
    [ "$power_diag" = power ] || return 0
    # Called after expand_root returns; continue directly to peripheral startup.
    power_diag_ready=1
    power_diag_checkpoint "root ready; continuing startup"
}
