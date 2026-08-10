#!/bin/bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Build the FAT32 BOOT partition of the R36 image, and own it end to end.
#
# Before this stage existed, the boot partition was a side effect: build_kernel.sh
# was the only script that ran `mkfs.vfat -F 32 -n BOOT`, and everything that
# came later wrote into mnt/boot on the assumption that stage had left a
# filesystem mounted there.  Nothing checked.  The 08042026 GUI image is what
# that costs: image_setup.sh recreates $DISK with a plain `dd` (no notrunc, so
# the file is truncated and the partition contents vanish) while the kernel
# checkpoint -- a stamp in the state directory -- survived, so build_kernel.sh
# was skipped, mnt/boot was an ordinary empty directory, and finishing_touches.sh
# happily wrote boot.ini into the VM's own filesystem.  The shipped image has an
# all-zero BPB in partition 1: u-boot finds nothing to load.
#
# So this stage formats the partition itself, installs the kernel payload from
# the stash the runner keeps outside the disk image, adds the files the pipeline
# never wrote at all (the R36S device tree, the u-boot panel variants, the
# charge-animation bitmaps and the dtb selector), and leaves the partition
# mounted with LOOP_BOOT exported so finishing_touches.sh can add boot.ini and
# detach it the way it always has.
#
# Reference for what belongs here: the BOOT partition of a working R36
# image, which the caller points at with DARKOS_R36_BOOT_PAYLOAD.

boot_log() { printf '\n[r36-boot] %s\n' "$*"; }
boot_fail() { printf '\n[r36-boot] ERROR: %s\n' "$*" >&2; exit 1; }

mountpoint="${mountpoint:-mnt/boot}"
BOOT_STASH="${BOOT_STASH:-}"
BOOT_PAYLOAD="${BOOT_PAYLOAD:-}"

[[ -n "$DISK" && -f "$DISK" ]] || boot_fail "the disk image is missing: ${DISK:-<unset>}"
[[ -n "$SYSTEM_PART_START" && -n "$SYSTEM_PART_END" ]] || \
    boot_fail "the boot partition geometry is not set"

# The kernel payload has to exist before anything is formatted: a reformat is
# only safe because everything that belongs in the partition can be put back.
[[ -n "$BOOT_STASH" ]] || boot_fail "BOOT_STASH is not set; the runner owns the kernel stash"
for required in Image uInitrd; do
    [[ -s "$BOOT_STASH/$required" ]] || \
        boot_fail "the kernel stash has no $required; the kernel stage must run before this one"
done

# Which device tree u-boot should load.  An R36S is not an RG351MP: the panel
# and the button matrix differ, and booting one with the other's device tree is
# how you get a dark screen on hardware that is otherwise running.  Prefer a
# tree the kernel build produced, fall back to the reference blob, and only
# settle for the RG351MP tree if there is nothing else -- and say which.
BOOT_KERNEL_DTB=""
if [[ -s "$BOOT_STASH/rk3326-r36s-linux.dtb" ]]; then
    BOOT_KERNEL_DTB="rk3326-r36s-linux.dtb"
    boot_log "Using the R36S device tree built from the kernel source"
elif [[ -n "$BOOT_PAYLOAD" && -s "$BOOT_PAYLOAD/rk3326-r36s-linux.dtb" ]]; then
    BOOT_KERNEL_DTB="rk3326-r36s-linux.dtb"
    boot_log "The kernel source has no R36S device tree; using the reference blob from $BOOT_PAYLOAD"
elif [[ -s "$BOOT_STASH/$KERNEL_DTB" ]]; then
    BOOT_KERNEL_DTB="$KERNEL_DTB"
    boot_log "WARNING: no R36S device tree is available; booting with ${KERNEL_DTB}, which targets an RG351MP panel"
else
    boot_fail "no kernel device tree is available to install"
fi

# Start from a known state.  A loop device left attached by an earlier attempt
# would otherwise be mounted here instead of the partition we are about to make.
if mountpoint -q "$mountpoint"; then
    stale_loop="$(findmnt -n -o SOURCE "$mountpoint" 2>/dev/null)"
    sync "$mountpoint" 2>/dev/null
    sudo umount -l "$mountpoint"
    if [[ "$stale_loop" == /dev/loop* ]]; then
        sudo losetup -d "$stale_loop" 2>/dev/null || true
    fi
fi

BOOT_PART_OFFSET=$((SYSTEM_PART_START * 512))
BOOT_PART_SIZE=$(( (SYSTEM_PART_END - SYSTEM_PART_START + 1) * 512 ))
LOOP_BOOT="$(sudo losetup --find --show --offset "$BOOT_PART_OFFSET" \
    --sizelimit "$BOOT_PART_SIZE" "$DISK")"
[[ -n "$LOOP_BOOT" ]] || boot_fail "could not attach a loop device to the boot partition"

boot_log "Formatting the boot partition (${LOOP_BOOT}, $((BOOT_PART_SIZE / 1048576)) MiB)"
sudo mkfs.vfat -F 32 -n BOOT "$LOOP_BOOT" || boot_fail "mkfs.vfat failed on $LOOP_BOOT"
mkdir -p "$mountpoint"
sudo mount "$LOOP_BOOT" "$mountpoint" || boot_fail "could not mount the boot partition"
mountpoint -q "$mountpoint" || boot_fail "$mountpoint is not a mount point after mounting it"

# The reference payload first, so the freshly built kernel files below always
# win over the blobs that shipped in the reference image.  Image and uInitrd are
# ours by definition -- the reference kernel would not match the modules in this
# rootfs -- and boot.ini, logo.bmp and the three first-boot helpers are written
# later by finishing_touches.sh from this checkout, which is where they belong.
if [[ -n "$BOOT_PAYLOAD" ]]; then
    if [[ -d "$BOOT_PAYLOAD" ]]; then
        command -v rsync >/dev/null 2>&1 || boot_fail "rsync is needed to install the boot payload"
        boot_log "Installing the reference boot payload from $BOOT_PAYLOAD"
        # No -p/-o/-g and no symlinks: FAT has nowhere to put any of that, and
        # rsync fails the transfer rather than skipping it.
        sudo rsync -rt --omit-dir-times --no-perms --no-owner --no-group \
            --exclude='Image' \
            --exclude='uInitrd' \
            --exclude='initrd.img' \
            --exclude='boot.ini' \
            --exclude='logo.bmp' \
            --exclude='expandtoexfat.sh' \
            --exclude='firstboot.sh' \
            --exclude='fstab.exfat' \
            --exclude='.DS_Store' \
            --exclude='.fseventsd' \
            --exclude='.Spotlight-V100' \
            --exclude='.Trashes' \
            --exclude='System Volume Information' \
            "$BOOT_PAYLOAD"/ "$mountpoint"/ || boot_fail "could not install the boot payload"
    else
        boot_log "WARNING: DARKOS_R36_BOOT_PAYLOAD points at $BOOT_PAYLOAD, which does not exist"
    fi
else
    boot_log "No reference boot payload configured; installing the built kernel payload only"
fi

boot_log "Installing the kernel payload from $BOOT_STASH"
for artifact in "$BOOT_STASH"/*; do
    [[ -f "$artifact" ]] || continue
    sudo cp -f "$artifact" "$mountpoint/$(basename "$artifact")" || \
        boot_fail "could not copy $(basename "$artifact") into the boot partition"
done

sync "$mountpoint"

# u-boot loads exactly three files, and the build has now put all three in
# place; boot.ini, which names them, is written by finishing_touches.sh from
# BOOT_KERNEL_DTB and BOOT_ROOT_SPEC.
for required in Image uInitrd "$BOOT_KERNEL_DTB"; do
    [[ -s "$mountpoint/$required" ]] || \
        boot_fail "$required did not make it into the boot partition"
done

boot_log "Boot partition contents:"
ls -l "$mountpoint"
df -h "$mountpoint" | tail -n 1

export LOOP_BOOT BOOT_KERNEL_DTB
