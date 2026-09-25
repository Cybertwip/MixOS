#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Sourced by /init. Opt-in with j36.diag=power; no register writes here.
# mount_bootfs must be defined, and expansion finished, before start is called.
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
        echo "J36 power diagnostic v1"
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
    # Expansion needs all sibling partitions unmounted for its table reread.
    # This is called only AFTER expand_root has returned.
    power_diag_ready=1
    stage "Power diagnostic: 60-second idle check"
    say "Expansion has returned. Peripheral startup begins after this check."
    power_diag_waited=0
    while [ "$power_diag_waited" -lt 60 ]; do
        power_diag_checkpoint "post-expansion idle $power_diag_waited/60s"
        detail "Idle $power_diag_waited/60s; saving BOOT:/j36-power-*.txt"
        sleep 5
        power_diag_waited=$((power_diag_waited + 5))
    done
    say "Power diagnostic: idle check complete; continuing startup"
    power_diag_checkpoint "idle complete"
}
