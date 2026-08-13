#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Checkpointed RG351MP/R36 base builder for Ubuntu.  It builds a native armhf
# Debian userspace on the existing arm64 RK3326 kernel/boot chain, preserving
# completed stages across retries.
#
# WHAT IT NO LONGER BUILDS: EmulationStation, and the thirty-odd emulators and
# helper applications that used to hang off BUILD_BUNDLED_APPS=y.  Every one of
# those was a third-party source tree cloned at build time, and between them they
# carried licences MixOS cannot ship under.  The Debian packages stay exactly as
# they were -- needed_packages*.txt is untouched -- so this is still the same full
# gaming/PC/development userspace; what left is the vendored source, not the
# software.  The dashboard is mixdash, which the J36 layer stages into /opt/mixos
# on this same rootfs.

set -o pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$ROOT" || exit 1

DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"
USERSPACE_ARCH="${USERSPACE_ARCH:-armhf}"
# R36 builds are single-userspace-architecture images.  Keep the legacy
# multiarch switch off; USERSPACE_ARCH selects the native rootfs instead.
BUILD_ARMHF=n
BUILD_JOBS="${BUILD_JOBS:-4}"
ENABLE_CACHE="${ENABLE_CACHE:-y}"
CHIPSET=rk3326
UNIT=rg351mp
# One profile now, because BUILD_BUNDLED_APPS is gone with the source trees it selected,
# so this no longer selects anything.  It survives for exactly one reason: STATE_DIR
# below is built from it, and build-r36-ultra.sh mirrors that expression with `gui' spelt
# as a literal.  The word names the checkpoint directory every build VM already has on
# disk, and dropping it would rename that directory -- a full debootstrap to say nothing
# new.  It is no longer part of any filename.
BUILD_PROFILE=gui
# The BUILD ROOT: the loopback ext2 the rootfs is assembled in, not the image that ships
# (that is DISK, below).  The profile is out of the name because there is one profile and
# nothing left to collide with, and the ArkOS_ prefix is out of it because the project is
# not called that.  Nothing in flight is orphaned by the rename: the STATE_KEY bump to v4
# already discards every checkpoint that could have referred to a build root under the
# old name.  One may still be sitting in the build directory using disk; `make clean'
# sweeps both spellings.
FILESYSTEM="MixOS_R36_${DEBIAN_CODE_NAME}_${USERSPACE_ARCH}_File_System.img"
export DEBIAN_CODE_NAME USERSPACE_ARCH BUILD_ARMHF BUILD_JOBS
export ENABLE_CACHE CHIPSET UNIT BUILD_PROFILE FILESYSTEM

# Keep direct make, CMake, Meson/Ninja, and helper scripts on the same
# configurable parallelism limit.  Exporting nproc also covers child bash
# scripts that still derive their job count with $(nproc).
export MAKEFLAGS="-j${BUILD_JOBS}"
export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
export NINJAFLAGS="-j${BUILD_JOBS}"
nproc() { printf '%s\n' "$BUILD_JOBS"; }
export -f nproc

STATE_ROOT="${DARKOS_R36_STATE_DIR:-$HOME/darkos-r36-state}"
# THE SAME STRING IS BUILT IN build-r36-ultra.sh (STATE_KEY), which needs it on the host
# to find `latest-image' after this script exits.  The two are separate expressions and
# have to be changed together -- including the -vN suffix.  See the note there for why
# it is at v4.
STATE_DIR="$STATE_ROOT/${DEBIAN_CODE_NAME}-userspace-${USERSPACE_ARCH}-profile-${BUILD_PROFILE}-v4"
mkdir -p "$STATE_DIR"
LOG="$STATE_DIR/resume.log"
CURRENT_STAGE="$STATE_DIR/current-stage"
exec > >(tee -a "$LOG") 2>&1

log() { printf '\n[r36-resume] %s\n' "$*"; }
fail() { printf '\n[r36-resume] ERROR: %s\n' "$*" >&2; exit 1; }
marked() { [[ -f "$STATE_DIR/$1.done" ]]; }
mark() { touch "$STATE_DIR/$1.done"; }
maybe_stop() {
    if [[ "${DARKOS_R36_STOP_AFTER:-}" == "$1" ]]; then
        log "Stopping after requested checkpoint: $1"
        rm -f "$CURRENT_STAGE"
        exit 0
    fi
}

[[ "$USERSPACE_ARCH" == armhf || "$USERSPACE_ARCH" == arm64 ]] || \
    fail "USERSPACE_ARCH must be armhf or arm64"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || \
    fail "BUILD_JOBS must be a positive integer"

# Set by habit, by a stale Makefile, or by a shell that still has it exported from
# before: say what happened rather than building something other than what was
# asked for and never mentioning it.
if [[ -n "${BUILD_BUNDLED_APPS:-}" ]]; then
    log "BUILD_BUNDLED_APPS=${BUILD_BUNDLED_APPS} is ignored: the bundled emulator and application source builds are gone"
fi

log "Configuration: Debian=${DEBIAN_CODE_NAME}, userspace=${USERSPACE_ARCH}, profile=${BUILD_PROFILE}, jobs=${BUILD_JOBS}, cache=${ENABLE_CACHE}"

if [[ "${DARKOS_R36_RESET:-0}" == 1 ]]; then
    fail "DARKOS_R36_RESET requires a fresh build; use 'make clean' manually before retrying"
fi

# Load shared helpers, then put the Linaro bin directory on PATH as an ABSOLUTE
# path. utils.sh's relative entry stops working after build_kernel.sh cd's into
# rg351, which caused the original aarch64-linux-gnu-gcc not found failure.
source ./utils.sh
TOOLCHAIN="$ROOT/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu"
[[ -x "$TOOLCHAIN/bin/aarch64-linux-gnu-gcc" ]] || \
    fail "Linaro AArch64 compiler is missing from $TOOLCHAIN"
export PATH="$TOOLCHAIN/bin:$PATH"
"$TOOLCHAIN/bin/aarch64-linux-gnu-gcc" --version | head -n 1

# The on-device version string, and nothing else any more: finishing_touches.sh writes
# it into /etc/os-release's title and ~/.config/.VERSION.  It used to name the image as
# well, which is why it went to the trouble of recovering the date out of an existing
# partial image's filename -- so that a retry after midnight did not produce a second
# image under a second name.  The image is named after the commit now (see DISK below),
# so all that is left is to keep one date per build across retries.
if [[ -s "$STATE_DIR/build-date" ]]; then
    BUILD_DATE="$(cat "$STATE_DIR/build-date")"
else
    BUILD_DATE="$(date +%m%d%Y)"
    printf '%s\n' "$BUILD_DATE" > "$STATE_DIR/build-date"
fi
export BUILD_DATE

# setup_partition.sh values, reproduced without recreating its 52 GB filesystem.
#
# WHY ext2 AND NOT btrfs.  The J36's MVII LK reads FAT32 and nothing else, so the card
# needs a FAT32 BOOT partition no matter what -- and that is all BOOT is for: the
# kernel, the device tree and boot.conf, which the loader hands to Linux.  Nothing
# after that moment is the loader's business, so the OS partition is free to be the
# simplest filesystem both sides can read, and ext2 is it.  btrfs bought compression
# and cost a filesystem whose on-disk format only Linux implements.
#
# What this is NOT: a fast filesystem, or a crash-safe one.  ext2 has no journal, so a
# board that loses power with the rootfs mounted rw comes back needing an fsck -- which
# is the trade this layout accepts, because the alternative is a rootfs the MVII side
# cannot open at all.  It is also the reason /init mounts everything it does not own
# read-only.
#
# The build root and the shipped filesystem are the same object: write_rootfs.sh dds
# $FILESYSTEM straight into the image at STORAGE_PART_START.  So this choice is made
# once, here, and the 52 GB build image is ext2 too.
ROOT_FILESYSTEM_FORMAT=ext2
# -F because mkfs is being pointed at a file rather than a block device, and -b 4096 so
# the block size does not depend on how big the build image happens to be: the shrink in
# write_rootfs.sh and resize2fs on the device both work in these blocks, and a
# 1 KiB-block filesystem would cap the rootfs at 16 GB after expansion.
ROOT_FILESYSTEM_FORMAT_PARAMETERS="-F -b 4096 -L ROOTFS"
ROOT_FILESYSTEM_MOUNT_OPTIONS="defaults,noatime"
# p3, the data partition: ext2 and labelled DATA rather than vfat and EASYROMS.  The
# long form of why is in setup_partition.sh, which sets the same four values; the short
# form is that it holds Linux content, and the old flow formatted it vfat here only for
# firstboot to reformat it to exfat and untar /roms.tar back onto it on the device.
DATA_LABEL="DATA"
DATA_FILESYSTEM_FORMAT="ext2"
DATA_FILESYSTEM_FORMAT_PARAMETERS="-F -b 1024 -L ${DATA_LABEL}"
# Deliberately no umask/uid/gid: mount refuses them on ext2, and an fstab line that
# carries them is a partition that does not mount at all.  Ownership is set once, in
# finishing_touches.sh, while the partition is still a loop device.  nofail because this
# partition is the home directory now, and a card without it must still reach a shell.
DATA_MOUNT_OPTIONS="defaults,auto,noatime,nofail"
# The mount point is a home directory, not a rom library -- see setup_partition.sh.
DATA_MOUNT_POINT="/home/virtua"
SYSTEM_SIZE=100
# 4000 MB for the OS partition, not 7500.  write_rootfs.sh reports what the rootfs
# actually weighs -- "Root filesystem holds 3384 MB" -- and 4000 is that rounded up
# to the next whole 1000 MB: compact base system, one round step of headroom.  The old
# 7500 put 4 GB of zeros in the image and 4 GB nothing would ever use on the card, which
# was free only while btrfs compressed the image and a .7z shipped it.  Both are gone, so
# the image is now 4417 MB instead of 7917 MB and every copy of it is that much shorter.
#
# THE RULE IF THE BASE SYSTEM OUTGROWS IT: round up to the next 1000 MB step and change
# it here.  Nothing has to guess -- write_rootfs.sh refuses to dd a rootfs that would run
# over the DATA partition and prints the exact value to set.  It cannot be derived
# automatically, because image_setup.sh writes the partition table in the `image' stage
# and the rootfs is not measurable until finalization, thousands of packages later.
#
# setup_partition.sh reads this rather than setting its own copy (`${STORAGE_SIZE:-4000}'),
# so the sourced partition stage and the resumed build that skips it cannot disagree.
STORAGE_SIZE=4000
ROM_PART_SIZE=300
BUILD_SIZE=52000
SYSTEM_PART_START=32768
SYSTEM_PART_END=$(( SYSTEM_PART_START + (SYSTEM_SIZE * 1024 * 1024 / 512) - 1 ))
STORAGE_PART_START=$(( SYSTEM_PART_END + 1 ))
STORAGE_PART_END=$(( STORAGE_PART_START + (STORAGE_SIZE * 1024 * 1024 / 512) - 1 ))
ROM_PART_START=$(( STORAGE_PART_END + 1 ))
ROM_PART_END=$(( ROM_PART_START + (ROM_PART_SIZE * 1024 * 1024 / 512) - 1 ))
DISK_START_PADDING=$(( (SYSTEM_PART_START + 2048 - 1) / 2048 ))
DISK_SIZE=$(( DISK_START_PADDING + SYSTEM_SIZE + STORAGE_SIZE + ROM_PART_SIZE + 1 ))
# THE ONE ARTIFACT THIS BUILD SHIPS, and the only one: MixOS_<arch>_<debian>_<commit>.img,
# uncompressed.  There is no .7z beside it any more -- see the finalization stage for why
# that went, and darkos_image_name() in device/common/multipass.sh for where this name
# comes from and why the host computes it rather than this script.  The fallback is for
# running this file by hand on a Linux box with no wrapper; .git is absent in the build VM,
# so there the wrapper's value is the only one there is.
DISK="${DARKOS_IMAGE_NAME:-MixOS_${USERSPACE_ARCH}_${DEBIAN_CODE_NAME}_$(git -C "$ROOT" rev-parse --short=7 HEAD 2>/dev/null || echo nogit).img}"
export ROOT_FILESYSTEM_FORMAT ROOT_FILESYSTEM_FORMAT_PARAMETERS
export DATA_LABEL DATA_FILESYSTEM_FORMAT DATA_FILESYSTEM_FORMAT_PARAMETERS
export DATA_MOUNT_OPTIONS DATA_MOUNT_POINT
export ROOT_FILESYSTEM_MOUNT_OPTIONS SYSTEM_PART_START SYSTEM_PART_END
export STORAGE_PART_START STORAGE_PART_END ROM_PART_START ROM_PART_END
export DISK_SIZE FILESYSTEM DISK

KERNEL_SRC=rg351
DEF_CONFIG=rg351p_tweaked_defconfig
SCREEN_ROTATION=0
KERNEL_DTB=rk3326-rg351mp-linux.dtb
mountpoint=mnt/boot
export KERNEL_SRC DEF_CONFIG SCREEN_ROTATION KERNEL_DTB mountpoint

# The three files u-boot loads live in the FAT boot partition, and that
# partition does not survive the image stage: image_setup.sh recreates $DISK
# with a plain `dd`, which truncates the file, while a kernel checkpoint -- a
# stamp in this state directory -- survives.  Keeping a copy of the kernel
# payload outside the image is what lets the boot partition be rebuilt without
# a two-hour kernel rebuild, and its absence is how we know a kernel checkpoint
# is describing something that no longer exists.
BOOT_STASH="$STATE_DIR/boot"
# What the pipeline never wrote at all: the R36S device tree, the u-boot panel
# variants, the off-charging bitmaps and the dtb selector.  Taken from the BOOT
# partition of a working R36 image.
BOOT_PAYLOAD="${DARKOS_R36_BOOT_PAYLOAD:-}"
# LABEL=ROOTFS rather than /dev/mmcblk0p2: an R36S has two card slots and no
# eMMC, so the mmcblk index depends on which slot enumerates first, while the
# label is written by mkfs and matches the fstab this build installs.
BOOT_ROOT_SPEC="LABEL=ROOTFS"
export BOOT_STASH BOOT_PAYLOAD BOOT_ROOT_SPEC

# Image and uInitrd come out of the boot partition; the device trees are worth
# stashing too so a rebuild does not depend on the kernel source tree, which
# clean_mounts.sh deletes.
stash_boot_payload() {
    local artifact
    mkdir -p "$BOOT_STASH"
    for artifact in Image uInitrd "$KERNEL_DTB" rk3326-r36s-linux.dtb \
        rg351mp-uboot.dtb rg351v-uboot.dtb; do
        if [[ -s "$mountpoint/$artifact" ]]; then
            sudo cp -f "$mountpoint/$artifact" "$BOOT_STASH/$artifact"
        fi
    done
    # An R36S is not an RG351MP.  If the vendor tree carries its device tree,
    # prefer that over any reference blob; the build_kernel.sh copy step only
    # knows about $KERNEL_DTB.
    if [[ -s "$KERNEL_SRC/arch/arm64/boot/dts/rockchip/rk3326-r36s-linux.dtb" ]]; then
        sudo cp -f "$KERNEL_SRC/arch/arm64/boot/dts/rockchip/rk3326-r36s-linux.dtb" \
            "$BOOT_STASH/rk3326-r36s-linux.dtb"
    fi
    sudo chown -R "$(id -u):$(id -g)" "$BOOT_STASH"
    log "Boot payload stashed in $BOOT_STASH:"
    ls -l "$BOOT_STASH"
}

# ── Not unpacking Debian twice ────────────────────────────────────────────────
#
# clean_mounts.sh deletes $FILESYSTEM at the end of a successful run, to give back
# the working copy's disk.  The consequence was that the *next* run -- a changed
# profile, a deleted image, a bumped STATE_KEY -- had to debootstrap Debian and
# reinstall every chroot build dependency from scratch, which is the bulk of the
# build and the "it sets up the whole system again" the log makes it look like.
#
# So the rootfs is copied once, immediately before finalization, which is the last
# moment it is still a usable build root: cleanup_filesystem.sh strips it and
# write_rootfs.sh shrinks it to fit the image.  A later run whose $FILESYSTEM is
# gone restores that copy and keeps its bootstrap/userspace checkpoints.
#
# The copy is crash-consistent rather than quiesced -- MixOSBuild is still mounted, so it
# is synced and then copied.  On btrfs that was what log replay was for; on ext2 there
# is no log, and what a torn copy needs is the e2fsck that write_rootfs.sh runs before
# it ships the image.  A build root is not a database either way.  --sparse=always
# keeps the 52 GiB image's holes.
ROOTFS_SNAPSHOT="$STATE_DIR/rootfs-prefinal.img"
# ── The second snapshot, and the two hours it is worth ────────────────────────
#
# cleanup_filesystem.sh is the single most expensive thing in this build after
# Debian itself: it apt-removes a hundred -dev packages, autoremoves whatever that
# orphans, apt-cleans the archive, and then REINSTALLS the hundred and fifty
# runtime names from needed_packages.txt that the autoremove just took out --
# every one of those a dpkg pass in an armhf chroot.  Up to two hours.
#
# It ran on every build, and not because anything had changed: the image is named
# after the commit, so a new commit means a $DISK that does not exist, which
# clears the finalization checkpoint, which re-runs the whole final stage on a
# rootfs restored from the PRE-cleanup snapshot -- a rootfs that still has all
# hundred -dev packages on it, so all of that work is genuinely there to do again.
#
# So there is a second snapshot, taken the moment cleanup_filesystem.sh returns.
# Restore that instead and cleanup_filesystem.sh costs nothing at all, by its own
# logic rather than by being skipped: install_package dpkg-queries the whole list
# in one go and returns "all already installed", and `apt remove' on packages that
# are already gone is a no-op.  Nothing is bypassed, so nothing can go stale --
# the script still runs, still checks, and would still fix anything that had
# drifted.  That is the whole trick, and it is why this needed no signature file
# and no "is the cache still valid" logic.
#
# What made it possible is the reorder in the finalization stage below:
# finishing_touches.sh now runs AFTER cleanup_filesystem.sh.  It has to, because
# some of what it does is not idempotent -- it appends @reboot lines to root's
# crontab -- so the state that gets cached has to be one where it has not run yet.
#
# WHAT A RESTORED RUN LOOKS LIKE IN THE LOG, so it is not read as a fault: cleanup
# also deletes /var/lib/apt/lists, so its own `apt remove' on the next run cannot
# resolve those hundred names and prints a screen of "Unable to locate package".
# There is nothing there to remove -- the snapshot is the state after they were
# removed -- and every step that matters afterwards asks dpkg directly, which
# needs no lists.  That noise is what a cleanup with nothing to do looks like.
ROOTFS_STRIPPED_SNAPSHOT="$STATE_DIR/rootfs-stripped.img"
SNAPSHOT_ROOTFS="${DARKOS_R36_SNAPSHOT_ROOTFS:-1}"

# snapshot_rootfs TARGET DESCRIPTION
snapshot_rootfs() {
    local target="$1" what="$2" need avail
    [[ "$SNAPSHOT_ROOTFS" == 1 ]] || return 0
    [[ -f "$FILESYSTEM" ]] || return 0
    [[ ! -s "$target" ]] || return 0

    # Allocated bytes, not the 52 GiB apparent size: du's default is disk usage.
    need="$(du -B1 "$FILESYSTEM" 2>/dev/null | cut -f1)"
    need="${need:-0}"
    avail=$(( $(df -Pk "$STATE_DIR" | awk 'NR == 2 { print $4 }') * 1024 ))
    if (( need == 0 || avail < need + 4294967296 )); then
        log "Not snapshotting the $what root: it needs $need bytes and $avail are free"
        log "The next build from a deleted filesystem will do that work again"
        return 0
    fi

    log "Snapshotting the $what root so a later run need not build it again"
    sync
    # btrfs has a sync of its own that flushes its log trees; ext2 has no log to flush
    # and no such command, so the plain sync above is the whole of it.
    if [[ "$ROOT_FILESYSTEM_FORMAT" == btrfs ]]; then
        mountpoint -q MixOSBuild && sudo btrfs filesystem sync MixOSBuild 2>/dev/null || true
    fi
    if ! sudo cp --reflink=auto --sparse=always "$FILESYSTEM" "$target.part"; then
        log "Snapshot failed; continuing without one"
        sudo rm -f "$target.part"
        return 0
    fi
    sudo chown "$(id -u):$(id -g)" "$target.part"
    mv -f "$target.part" "$target"
    log "$what root snapshot: $(du -h "$target" | cut -f1)"
}

# Only ever called when $FILESYSTEM is missing, which is the one case where a
# snapshot is not a stale copy of something that already exists.
#
# WHICH OF THE TWO, and the rule is `marked userspace': the stripped root has no
# compiler and no -dev package on it, so it is only the right answer when there is
# nothing left to compile.  With a userspace component still to build -- an
# operator who deleted a component-*.done, a new script in the list -- the
# pre-final root is the only one that can build it, and paying cleanup's two hours
# afterwards is the price of that.  Getting this backwards would be silent: the
# checkpoint says build_deps.sh ran, so nothing would reinstall the compiler, and
# the component build would fail hundreds of packages later.
# Whether the build root this run is working on came from the stripped snapshot --
# read once, at the top of the finalization stage, so a run that started from a
# rootfs with no compiler on it cannot go on to save that rootfs as the copy a
# later run compiles from.
RESTORED_STRIPPED=0

restore_rootfs_snapshot() {
    local from what
    [[ ! -f "$FILESYSTEM" ]] || return 1
    # A finished image is about to be verified and returned; copying ten gigabytes to
    # then exit immediately would be pure waste.  `marked finalization' and not merely
    # the image's existence: image_setup.sh creates $DISK long before it is finished.
    if marked finalization && [[ -s "$DISK" ]]; then return 1; fi

    if marked userspace && [[ -s "$ROOTFS_STRIPPED_SNAPSHOT" ]]; then
        from="$ROOTFS_STRIPPED_SNAPSHOT"
        what="stripped root; neither Debian nor the package cleanup runs again"
    elif [[ -s "$ROOTFS_SNAPSHOT" ]]; then
        from="$ROOTFS_SNAPSHOT"
        what="pre-finalization build root; Debian will not be unpacked again"
    else
        return 1
    fi

    log "Restoring the $what"
    if ! cp --reflink=auto --sparse=always "$from" "$FILESYSTEM.part"; then
        rm -f "$FILESYSTEM.part"
        return 1
    fi
    mv -f "$FILESYSTEM.part" "$FILESYSTEM"
    [[ "$from" == "$ROOTFS_STRIPPED_SNAPSHOT" ]] && RESTORED_STRIPPED=1
    # Finalization is destructive and its checkpoint describes a rootfs that no
    # longer exists in that state.  Everything before it is exactly what the
    # snapshot holds.
    rm -f "$STATE_DIR/finalization.done" "$STATE_DIR/complete.done"
    return 0
}

boot_stash_ready() {
    [[ -s "$BOOT_STASH/Image" && -s "$BOOT_STASH/uInitrd" ]] || return 1
    compgen -G "$BOOT_STASH/*.dtb" >/dev/null
}

ensure_rootfs_mounts() {
    [[ -f "$FILESYSTEM" ]] || return 0
    mkdir -p MixOSBuild
    if ! mountpoint -q MixOSBuild; then
        log "Remounting the preserved $ROOT_FILESYSTEM_FORMAT build root"
        # Fatal, and it was not before.  An unchecked failure here leaves MixOSBuild as an
        # ordinary directory on the VM's own disk, and everything below -- the bind
        # mounts, debootstrap, every chroot stage -- then builds Debian into the build
        # machine while `mark partition' records the stage as done.  The image that comes
        # out has an empty OS partition, which is the shape of the bug that shipped.
        sudo mount -t "$ROOT_FILESYSTEM_FORMAT" -o "$ROOT_FILESYSTEM_MOUNT_OPTIONS",loop \
            "$FILESYSTEM" MixOSBuild || \
            fail "Could not mount $FILESYSTEM as $ROOT_FILESYSTEM_FORMAT -- if this build root predates the ext2 layout, delete it and $ROOTFS_SNAPSHOT and run again"
    fi
    # --rbind + --make-rslave, and no separate devpts.  A plain bind of /dev joins the
    # host's peer group on a shared-/ machine, so the `newinstance' devpts that used to
    # go on MixOSBuild/dev/pts propagated onto the BUILD VM's own /dev/pts and left every
    # sudo on it failing with "unable to allocate pty".  This ran once per stage, so it
    # stacked a fresh instance each time.  See bootstrap_rootfs.sh for the long version.
    sudo mkdir -p MixOSBuild/{dev,proc,sys}
    if ! mountpoint -q MixOSBuild/dev; then
        sudo mount --rbind /dev MixOSBuild/dev
        sudo mount --make-rslave MixOSBuild/dev
    fi
    if ! mountpoint -q MixOSBuild/proc; then
        sudo mount --rbind /proc MixOSBuild/proc
        sudo mount --make-rslave MixOSBuild/proc
    fi
    if ! mountpoint -q MixOSBuild/sys; then
        sudo mount --rbind /sys MixOSBuild/sys
        sudo mount --make-rslave MixOSBuild/sys
    fi
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
        sudo tee MixOSBuild/etc/resolv.conf >/dev/null
}

ensure_ccache_mount() {
    sudo mkdir -p MixOSBuild/home/virtua/MixOSBuild_ccache
    if ! mountpoint -q MixOSBuild/home/virtua/MixOSBuild_ccache; then
        sudo mount --bind "$ROOT/MixOSBuild_ccache" MixOSBuild/home/virtua/MixOSBuild_ccache
    fi
}

ensure_boot_mount() {
    mkdir -p "$mountpoint"
    if ! mountpoint -q "$mountpoint"; then
        local offset size loop
        # There is no image to find a partition in, so say that instead of what
        # this function used to say.  losetup writes its own "No such file or
        # directory" to stderr, leaves $loop empty, and `mount ""' then fails --
        # which the tolerant branch below reported as an unformatted boot
        # partition.  That is a different fault with a different fix, and dressing
        # one as the other is what let a run walk two more stages before stopping
        # in install_boot.sh with "the disk image is missing".
        [[ -s "$DISK" ]] || fail "the disk image $DISK is missing, so its boot partition cannot be mounted"
        offset=$((SYSTEM_PART_START * 512))
        size=$(( (SYSTEM_PART_END - SYSTEM_PART_START + 1) * 512 ))
        loop="$(sudo losetup --find --show --offset "$offset" --sizelimit "$size" "$DISK")" || \
            fail "could not attach a loop device to $DISK at offset $offset"
        [[ -n "$loop" ]] || fail "losetup attached no device to $DISK"
        # A disk the image stage has just recreated has no filesystem here yet.
        # Mounting it is not possible and not needed: install_boot.sh formats and
        # fills this partition during the finalization stage.  Failing quietly
        # here, and detaching the loop, is what keeps a stale /dev/loop from
        # being mounted over the real partition later.
        if ! sudo mount "$loop" "$mountpoint" 2>/dev/null; then
            log "The boot partition carries no filesystem yet; the finalization stage builds it"
            sudo losetup -d "$loop" 2>/dev/null || true
            return 0
        fi
    fi
}

clear_stale_boot_mount() {
    if mountpoint -q "$mountpoint"; then
        local loop
        loop="$(findmnt -n -o SOURCE "$mountpoint" || true)"
        sudo umount -l "$mountpoint"
        [[ "$loop" == /dev/loop* ]] && sudo losetup -d "$loop" || true
    fi
}

clear_foreign_rootfs_mount() {
    local mounted_source
    mountpoint -q MixOSBuild || return 0
    mounted_source="$(findmnt -n -o SOURCE MixOSBuild || true)"
    if ! sudo losetup -j "$ROOT/$FILESYSTEM" 2>/dev/null | \
        cut -d: -f1 | grep -Fxq "$mounted_source"; then
        log "Unmounting root filesystem left by a different R36 build profile"
        remove_mixosbuild
    fi
}

clear_legacy_multiarch_root() {
    if [[ -d MixOSBuild32 ]]; then
        log "Removing legacy secondary armhf build root; this profile is single-architecture"
        remove_mixosbuild32
    fi
}

run_stage() {
    local name="$1" script="$2" rc
    if marked "$name"; then
        log "Skipping completed stage: $name"
        return 0
    fi
    printf '%s\n' "$name" > "$CURRENT_STAGE"
    log "Running stage: $name ($script)"
    source "./$script"
    rc=$?
    if (( rc != 0 )); then
        fail "stage $name returned $rc"
    fi
    mark "$name"
}

# Which architecture is which, asserted rather than remembered.
#
# This image is deliberately mixed: an arm64 kernel, because an RK3326 is arm64,
# on a native armhf userspace, because 32-bit is what the R36 profile ships and
# what makes the same rootfs usable on the ARMv7 J36 Ultra.  The consequence is
# worth stating where it is checked: the MVII LK's SD hand-off reads the arm64
# boot magic at offset 0x38 and refuses this kernel by design.  A J36 card gets
# its own 32-bit zImage from device/j36-ultra; it shares the rootfs, not the
# kernel.  If this assertion ever fails, one of those two facts has moved.
verify_boot_kernel_arch() {
    local image="$BOOT_STASH/Image" magic
    [[ -s "$image" ]] || fail "no kernel image to check in $BOOT_STASH"
    magic="$(od -An -t x4 -j 0x38 -N 4 "$image" | tr -d ' \n')"
    [[ "$magic" == 644d5241 ]] || \
        fail "the RK3326 kernel does not carry the arm64 boot magic (0x38 reads $magic)"
    log "Verified an arm64 RK3326 kernel on an ${USERSPACE_ARCH} userspace"
    log "The MVII LK will refuse this kernel on a J36 Ultra; that card boots device/j36-ultra/sd-boot"
}

verify_native_userspace() {
    local actual foreign
    actual="$(sudo chroot MixOSBuild dpkg --print-architecture)" || \
        fail "Could not query the Debian userspace architecture"
    [[ "$actual" == "$USERSPACE_ARCH" ]] || \
        fail "Expected ${USERSPACE_ARCH} userspace, found ${actual}"
    foreign="$(sudo chroot MixOSBuild dpkg --print-foreign-architectures)" || \
        fail "Could not query foreign Debian architectures"
    [[ -z "$foreign" ]] || \
        fail "Single-architecture build unexpectedly contains foreign architectures: ${foreign}"
    log "Verified native ${actual} Debian userspace with no foreign architecture"
}

# verify_gui_architecture IS GONE, and nothing replaced it.  It read the ELF header
# of MixOSBuild/usr/bin/emulationstation/emulationstation and asserted ELF32/ARM or
# ELF64/AArch64 to catch a GUI built for the wrong architecture -- a real risk when
# one build script cross-compiled and the next did not.  There is no GUI in this
# image any more: mixdash is built by the J36 layer, from its own toolchain, and
# checked there.  What that assertion was really protecting -- "the userspace is
# the architecture it claims to be" -- verify_native_userspace above already states
# from dpkg, for the whole rootfs rather than for one binary in it.

# ── Anything left over from the previous filesystem layout ────────────────────
#
# What the artifacts of this build are made of, in one string, so a later run can tell
# whether the checkpoints beside it describe the layout it is being asked for.  A
# finished build writes it; a build from before this existed has no file, which reads
# as "unknown" and is treated as foreign.
# The geometry is in here as well as the two formats, because "the layout changed" has
# to include "the partitions moved".  STORAGE_SIZE went from 7500 to 4000, which shifts
# where p3 begins; an image built before that is not a smaller version of this one, it is
# a different card.  Adding the sizes also invalidates every layout file written by an
# earlier build, which is correct: those all describe the 7500 MB layout.
layout_signature() {
    printf 'rootfs=%s data=%s boot=%sMB os=%sMB data_part=%sMB\n' \
        "$ROOT_FILESYSTEM_FORMAT" "$DATA_FILESYSTEM_FORMAT" \
        "$SYSTEM_SIZE" "$STORAGE_SIZE" "$ROM_PART_SIZE"
}

# What is actually IN the finished image, which is the only evidence that survives the
# build root being deleted -- and the evidence the last run had in front of it and did
# not look at: parted printed "2 ... btrfs" and "3 ... fat32" while the resume decided
# from a checkpoint file that everything was done.
#
# parted -m emits one line per partition: number:begin:end:size:filesystem:name:flags;
# Field 5 is a probe of the partition's contents, not the MBR type byte, so it says
# ext2 for an ext2 partition even when the table calls it fat32.  An empty field is a
# partition with no filesystem yet -- an image mid-build -- and is not a mismatch.
#
# `unit s' so the boundaries can be compared as well as the filesystems, exactly: the
# table was written by image_setup.sh with `parted -a min unit s mkpart' from these same
# numbers, so a match is a match to the sector.  WHY THAT MATTERS: the recorded-signature
# path below only gets a say once the image has been deleted, and a STORAGE_SIZE change on
# an UNCOMMITTED tree does not change the image's name -- the name carries the committed
# id.  So the old 7500 MB image would sit there under exactly the name this build wants,
# with an ext2 p2 and an ext2 p3, pass the format check, get verified as "finished" and be
# shipped as though it were the new layout.  Two ext2 partitions in the wrong places is
# still the wrong card.
# To within 1 MiB, not exactly: parted is free to nudge a requested boundary to satisfy
# alignment, and an exact comparison that it ever failed would put this build in a rebuild
# loop it could not get out of.  The difference being looked for is 7,168,000 sectors --
# 7500 MB against 4000 -- so 2048 sectors of slack costs nothing and removes the whole
# class of false positives.  A non-numeric or empty value means parted said something this
# does not understand, and that is not evidence of anything.
sectors_agree() {
    local have="$1" want="$2"
    [[ "$have" =~ ^[0-9]+$ ]] || return 0
    (( (have > want ? have - want : want - have) <= 2048 ))
}

image_layout_is_foreign() {
    local img="$1" num begin end size fs rest
    [[ -s "$img" ]] || return 1
    while IFS=: read -r num begin end size fs rest; do
        begin="${begin%s}"
        end="${end%s}"
        case "$num" in
            2)  [[ -n "$fs" && "$fs" != "$ROOT_FILESYSTEM_FORMAT" ]] && {
                    log "$img: the OS partition holds $fs, not $ROOT_FILESYSTEM_FORMAT"
                    return 0
                }
                if ! sectors_agree "$begin" "$STORAGE_PART_START" ||
                   ! sectors_agree "$end" "$STORAGE_PART_END"; then
                    log "$img: the OS partition is sectors $begin..$end, and this layout"
                    log "$img: puts it at $STORAGE_PART_START..$STORAGE_PART_END (${STORAGE_SIZE} MB)"
                    return 0
                fi ;;
            3)  [[ -n "$fs" && "$fs" != "$DATA_FILESYSTEM_FORMAT" ]] && {
                    log "$img: the DATA partition holds $fs, not $DATA_FILESYSTEM_FORMAT"
                    return 0
                }
                if ! sectors_agree "$begin" "$ROM_PART_START" ||
                   ! sectors_agree "$end" "$ROM_PART_END"; then
                    log "$img: the DATA partition is sectors $begin..$end, and this layout"
                    log "$img: puts it at $ROM_PART_START..$ROM_PART_END"
                    return 0
                fi ;;
        esac
    done < <(parted -m -s "$img" unit s print 2>/dev/null || true)
    return 1
}

# ROOT_FILESYSTEM_FORMAT went from btrfs to ext2, because the MVII LK on the J36 Ultra
# reads FAT32 only and the OS partition has to be the simplest filesystem both kernels
# on the card can mount.  A build root made under the old value cannot be resumed from,
# and every way that goes wrong is silent:
#
#   * ensure_rootfs_mounts mounts it with -t $ROOT_FILESYSTEM_FORMAT, so the mount
#     fails outright while `mark partition' just above records the stage as done.  The
#     build then walks into an unmounted MixOSBuild and packages an image with no
#     userspace in it.
#   * write_rootfs.sh dds this very file into the image, so the image's p2 keeps the
#     old format no matter what these variables say.
#   * finishing_touches.sh writes /etc/fstab from $ROOT_FILESYSTEM_FORMAT, so a
#     half-migrated card ends up claiming ext2 for a btrfs partition.
#
# There is no shortcut: Debian is unpacked again, 30-60 minutes for the GUI profile.
# The snapshot goes too -- it is a copy of the same wrong filesystem, and leaving it
# would have restore_rootfs_snapshot put it straight back a few lines below.
#
# The finished image and archive of the old layout are LEFT ALONE, and the build date
# is reset so the rebuild takes a new name beside them.  They are the only flashable
# card the operator has until this finishes, and deleting that to save a few gigabytes
# would be the wrong trade.
discard_foreign_layout() {
    local target have keep part stale_loop recorded
    local rootfs_foreign=0 image_foreign=0 snapshot_ok=0

    for target in "$FILESYSTEM" "$ROOTFS_SNAPSHOT" "$ROOTFS_STRIPPED_SNAPSHOT"; do
        [[ -s "$target" ]] || continue
        have="$(sudo blkid -o value -s TYPE "$target" 2>/dev/null || true)"
        # No answer at all is not a mismatch: an image mid-write has no superblock yet,
        # and the stages below already handle a build root they cannot mount.
        [[ -n "$have" ]] || continue
        if [[ "$have" == "$ROOT_FILESYSTEM_FORMAT" ]]; then
            # Either snapshot will do for the "old image, current build root" path
            # below: both are states restore_rootfs_snapshot knows how to hand the
            # finalization stage, and it picks between them itself.
            [[ "$target" == "$ROOTFS_SNAPSHOT" || \
               "$target" == "$ROOTFS_STRIPPED_SNAPSHOT" ]] && snapshot_ok=1
            continue
        fi
        rootfs_foreign=1
        log "$(basename "$target") is $have and this build makes $ROOT_FILESYSTEM_FORMAT"
    done

    if image_layout_is_foreign "$DISK"; then
        image_foreign=1
    elif marked finalization && [[ ! -f "$DISK" ]]; then
        # Finished and then deleted -- the operator flashed it and reclaimed the eight
        # gigabytes.  There is nothing left to probe, so the recorded signature is the
        # only remaining witness, and its absence is itself the answer on a state
        # directory written before this check existed.
        recorded="$(cat "$STATE_DIR/layout" 2>/dev/null || true)"
        if [[ "$recorded" != "$(layout_signature)" ]]; then
            image_foreign=1
            log "The finished build records layout '${recorded:-unknown}', not '$(layout_signature)'"
        fi
    fi

    (( rootfs_foreign || image_foreign )) || return 0

    # An old image with a current build root: the rootfs itself is fine, but
    # finalization has already shrunk it to fit and re-running the stage on a
    # minimum-sized filesystem is how a build runs out of space at the very end.  The
    # pre-finalization snapshot is exactly the state that stage expects, so hand back
    # to it; with no snapshot there is no honest option but to bootstrap again.
    if (( image_foreign && ! rootfs_foreign )); then
        if (( snapshot_ok )); then
            log "The image predates this layout but the build root does not"
            log "Reverting to the pre-finalization snapshot rather than unpacking Debian again"
            mountpoint -q MixOSBuild && remove_mixosbuild
            sudo rm -f "$FILESYSTEM" "$FILESYSTEM.part"
        else
            log "The image predates this layout and there is no pre-finalization snapshot"
            log "to fall back on, so the build root goes with it"
            rootfs_foreign=1
        fi
    fi

    if (( rootfs_foreign )); then
        log "Discarding the old build root; Debian will be unpacked again, which is the"
        log "only way the image's OS partition changes format"
        mountpoint -q MixOSBuild && remove_mixosbuild
        # remove_mixosbuild unmounts lazily, so a loop device can still be holding the
        # image after the umount returns.  Detach it before the file goes: otherwise
        # losetup keeps the unlinked inode alive and clear_foreign_rootfs_mount below
        # compares the new build root against a loop that points at a deleted one.
        while read -r stale_loop; do
            [[ -n "$stale_loop" ]] && sudo losetup -d "$stale_loop" 2>/dev/null || true
        done < <(sudo losetup -j "$ROOT/$FILESYSTEM" 2>/dev/null | cut -d: -f1)
        sudo rm -f "$FILESYSTEM" "$FILESYSTEM.part" \
                   "$ROOTFS_SNAPSHOT" "$ROOTFS_SNAPSHOT.part" \
                   "$ROOTFS_STRIPPED_SNAPSHOT" "$ROOTFS_STRIPPED_SNAPSHOT.part"
        rm -f "$STATE_DIR"/partition.done "$STATE_DIR"/bootstrap.done \
              "$STATE_DIR"/userspace.done "$STATE_DIR"/component-*.done
    fi

    # Both cases end here: whatever else was kept, the image and everything that
    # packaged it describe the old layout and have to be made again.
    rm -f "$STATE_DIR"/finalization.done "$STATE_DIR"/complete.done \
          "$STATE_DIR"/image.done "$STATE_DIR"/layout

    # The name is the commit's, so the image being replaced is under exactly the name
    # this build is about to write -- and the "existing finished image" check further
    # down would otherwise find it, verify it and exit 0 without building anything,
    # which is the failure this function exists to stop.  Move it aside rather than
    # delete it: it is still the only card the operator can flash until this build
    # finishes, and now it says what it is.
    if [[ -f "$DISK" ]]; then
        keep="${DISK%.img}-old-layout.img"
        log "Keeping the old-layout image as $keep; the rebuilt one takes its name"
        mv -f "$DISK" "$keep"
    fi
    log "The rebuilt image will be $DISK; the old one is still there to flash meanwhile"
    return 0
}
discard_foreign_layout

# A GUI-only and a full-app build use different filesystem/image names.  Make
# sure a mount left by the other profile cannot contaminate this build.
clear_foreign_rootfs_mount
clear_legacy_multiarch_root

# Before any checkpoint is inferred: a missing build root may be recoverable.
restore_rootfs_snapshot || true

# Infer checkpoints made by the original monolithic build that failed before
# this resume runner existed.
if [[ -f "$FILESYSTEM" ]]; then
    ensure_rootfs_mounts
    mark partition
fi
if [[ -f MixOSBuild/etc/debian_version && -d MixOSBuild/home/virtua ]]; then
    mark bootstrap
fi
if [[ -s "$DISK" && $(stat -c %s "$DISK") -ge $((DISK_SIZE * 1024 * 1024)) ]]; then
    mark image
fi
if [[ -s "$mountpoint/Image" && -s "$mountpoint/$KERNEL_DTB" && -s "$mountpoint/uInitrd" ]]; then
    mark kernel
fi

# The same reasoning as the kernel checkpoint below, for the one stage that never
# had it: an image checkpoint means image_setup.sh once ran, not that its output
# is still on disk.  $DISK is a plain file in the build directory and it goes
# whenever the operator clears space, whenever a run is interrupted between the
# `dd` and the partition table, and whenever the image was built under a
# different commit -- the name carries the short SHA, so a commit is enough to
# make this build look for a file no run ever wrote.
#
# Leaving the stamp in place is worse than useless: every later stage takes $DISK
# for granted, so the run walks past `Skipping completed stage: image', reports
# the boot partition as merely unformatted, and only stops in install_boot.sh at
# the very end with "the disk image is missing".  Clearing it here costs one `dd`
# and puts the failure where it belongs.
if marked image && [[ ! -s "$DISK" ]]; then
    log "The image checkpoint describes $DISK, which is not here; it will be rebuilt"
    # finalization and complete both describe the contents of that same file, so
    # they cannot outlive it either.  layout is the signature of the image that
    # is gone; leaving it would let discard_foreign_layout believe the rebuilt
    # image already matches a layout it has never been written with.
    rm -f "$STATE_DIR"/image.done "$STATE_DIR"/finalization.done \
          "$STATE_DIR"/complete.done "$STATE_DIR"/layout
fi

# clean_mounts.sh deletes the build filesystem at the end of a successful run,
# so a state directory outlives the rootfs its checkpoints describe.  Every
# rootfs stage has to run again in that case; leaving the stamps in place would
# skip straight past an empty Debian tree and package an image with no userspace.
if [[ ! -f "$FILESYSTEM" ]] && marked bootstrap; then
    log "The build filesystem is gone; clearing the rootfs checkpoints that described it"
    rm -f "$STATE_DIR"/partition.done "$STATE_DIR"/bootstrap.done \
        "$STATE_DIR"/userspace.done "$STATE_DIR"/finalization.done \
        "$STATE_DIR"/component-*.done
fi

# A kernel checkpoint means the kernel was compiled, not that its output is
# still anywhere.  Recover the payload from a boot partition that happens to be
# mounted; otherwise the checkpoint is stale and the kernel has to be rebuilt.
if marked kernel && ! boot_stash_ready; then
    if [[ -s "$mountpoint/Image" && -s "$mountpoint/uInitrd" ]]; then
        log "Recovering the boot payload from the mounted boot partition"
        stash_boot_payload
    fi
fi
if marked kernel && ! boot_stash_ready; then
    log "The kernel checkpoint has no boot payload left to install; the kernel will be rebuilt"
    rm -f "$STATE_DIR/kernel.done"
fi

# If a finished image already exists, verify and stop immediately -- but only if it can
# actually boot.  The 08042026 GUI image finalized cleanly with an unformatted BOOT
# partition, and accepting that here is what turned a build bug into a released artifact.
#
# `marked finalization' is what makes "finished" mean finished: image_setup.sh creates
# $DISK at full size long before anything is written into it, so the file's existence on
# its own would let a half-built image be handed over as a completed build.  That
# distinction used to be free, because the archive only existed after create_image.sh ran
# at the very end -- and the archive is what has gone.
if marked finalization && [[ -s "$DISK" ]]; then
    if ! python3 device/r36-ultra/verify_boot.py "$DISK" \
            --require Image --require uInitrd --require boot.ini; then
        log "The finished image has no bootable BOOT partition; discarding it and rebuilding"
        rm -f "$DISK"
        rm -f "$STATE_DIR"/complete.done "$STATE_DIR"/finalization.done \
            "$STATE_DIR"/image.done
    else
        log "Existing finished image found and its BOOT partition is bootable: $DISK"
        mark complete
        # Reached only because discard_foreign_layout let this image stand, so it is this
        # layout's image by elimination.  Recording that is what lets the next run tell
        # the two apart once the image itself has been deleted.
        layout_signature > "$STATE_DIR/layout"
        printf '%s\n' "$DISK" > "$STATE_DIR/latest-image"
        exit 0
    fi
fi

run_stage prepare prepare.sh

if ! marked partition; then
    run_stage partition setup_partition.sh
else
    ensure_rootfs_mounts
fi
run_stage bootstrap bootstrap_rootfs.sh
verify_native_userspace
run_stage image image_setup.sh

if ! marked kernel; then
    clear_stale_boot_mount
    run_stage kernel build_kernel.sh
    stash_boot_payload
else
    ensure_boot_mount
fi
verify_boot_kernel_arch
maybe_stop kernel

# A failed userspace phase can be safely retried.  Two components are left of what
# used to be a forty-entry list: the native build dependencies, and SDL2.
#
# WHY THE OTHER THIRTY-EIGHT WENT.  Every one was `git clone' plus `make' against a
# third-party tree -- RetroArch and the libretro cores, PPSSPP, DuckStation,
# Mupen64Plus, Drastic, BigPEmu, the two Dooms, and the small ArkOS utilities that
# only ever existed to be launched from EmulationStation's menus.  Vendoring that
# source put licences into this repository that MixOS cannot ship under, and the
# whole layer had exactly one consumer, which was EmulationStation.
#
# The DEBIAN PACKAGES are not affected and were not touched: needed_packages*.txt
# still installs the compilers, the libraries, the media stack and the desktop
# tooling, so the image is the same full gaming/PC/development userspace it was.
# What is gone is source this project was copying, not software the device runs.
if ! marked userspace; then
    if marked component-build_deps; then
        ensure_ccache_mount
    fi
    if ! marked component-build_deps && mountpoint -q MixOSBuild/home/virtua/MixOSBuild_ccache; then
        sudo umount -l MixOSBuild/home/virtua/MixOSBuild_ccache || true
    fi
    userspace_scripts=(
        build_deps.sh
        build_sdl2.sh
    )
    if [[ -s "$STATE_DIR/sdl2-extension" ]]; then
        extension="$(cat "$STATE_DIR/sdl2-extension")"
    fi
    for script in "${userspace_scripts[@]}"; do
        component="component-${script%.sh}"
        if marked "$component"; then
            log "Skipping completed userspace component: $script"
            continue
        fi
        printf '%s\n' "userspace:$script" > "$CURRENT_STAGE"
        log "Running userspace component: $script"
        source "./$script" || fail "$script failed"
        if [[ "$script" == build_sdl2.sh ]]; then
            printf '%s\n' "${extension:-}" > "$STATE_DIR/sdl2-extension"
        fi
        mark "$component"
    done

    mark userspace
else
    ensure_rootfs_mounts
    ensure_boot_mount
    if [[ -s "$STATE_DIR/sdl2-extension" ]]; then
        extension="$(cat "$STATE_DIR/sdl2-extension")"
    else
        extension="$(grep -oP '(?<=extension=").*?(?=")' \
            MixOSBuild/home/virtua/${CHIPSET}_core_builds/scripts/sdl2.sh 2>/dev/null || true)"
    fi
fi
# Again, and not by oversight: it ran once after the bootstrap, and build_deps.sh
# has since run `apt install' inside the chroot.  A package that pulls in a foreign
# architecture does it there, which is after the first check and before this one.
verify_native_userspace

# Final image assembly is kept as one checkpoint because these scripts shrink
# and unmount the root filesystem. If it fails, the log identifies the exact
# final script rather than silently starting over.
if ! marked finalization; then
    # Not when this run started from the stripped root: that copy has no compiler
    # and no -dev package on it, and saving it under the name the userspace stage
    # restores from would turn one cached cleanup into a build root that cannot
    # build anything.  The pre-final snapshot is written by a run that unpacked
    # Debian, or it is not written at all.
    if (( RESTORED_STRIPPED )); then
        log "Started from the stripped root, so it is not saved as a build root"
    else
        snapshot_rootfs "$ROOTFS_SNAPSHOT" "pre-finalization build"
    fi
    # ── cleanup_filesystem.sh BEFORE finishing_touches.sh ─────────────────────
    #
    # It used to be the other way round, and the swap is what makes the stripped
    # snapshot above possible: the cached state has to be one where nothing
    # non-idempotent has run yet, and finishing_touches.sh appends to root's
    # crontab, so re-running it on its own output would add a second @reboot line
    # per build.  cleanup_filesystem.sh, by contrast, is safe to run any number of
    # times -- it asks dpkg what is installed before it does anything.
    #
    # It is also the better order on its own merits.  cleanup's `apt remove' and
    # `apt autoremove' pull out packages and run their maintainer scripts, which
    # is exactly the kind of thing that can undo a `systemctl enable'; doing that
    # first and configuring afterwards means the configuration is the last word.
    #
    # Neither script needs the other's output.  finishing_touches.sh makes its own
    # loop device for the DATA partition (LOOP_ROM, at its line 461) and does not
    # read anything cleanup writes; cleanup wants $extension from build_sdl2.sh,
    # which the userspace stage above has already resolved.
    final_scripts=(
        device/r36-ultra/install_boot.sh
        cleanup_filesystem.sh
        finishing_touches.sh
        write_rootfs.sh
        clean_mounts.sh
        device/r36-ultra/verify_boot.sh
    )
    # create_image.sh IS NOT IN THAT LIST ANY MORE.  It 7z'd the finished image into
    # 1950 MB volumes, and every consumer of this build then had to know about them: the
    # wrapper copied ${DISK}.7z.* to the workstation, verify_archive.sh spent twenty
    # minutes decompressing 8 GiB to test bytes 7z had just CRC'd, the J36 layer had to
    # re-split the whole archive after injecting its payload -- and the operator had to
    # 7z x before dd'ing anything.  The deliverable is one uncompressed .img; the only
    # thing the compression ever bought was a smaller upload, and nothing here uploads.
    # create_image.sh itself stays: the dormant per-device build_*.sh scripts source it.
    for script in "${final_scripts[@]}"; do
        printf '%s\n' "final:$script" > "$CURRENT_STAGE"
        log "Running final image stage: $script"
        source "./$script" || fail "$script failed"
        # The one moment the rootfs holds exactly what ships and nothing has been
        # written into it that a second run would write again.  After this,
        # finishing_touches.sh configures it and write_rootfs.sh shrinks it to
        # the size of the OS partition; neither state is one a later build can
        # start from.
        if [[ "$script" == cleanup_filesystem.sh ]]; then
            snapshot_rootfs "$ROOTFS_STRIPPED_SNAPSHOT" "stripped"
            # cleanup_filesystem.sh ends by deleting the chroot's resolv.conf,
            # which was right while it was the last thing to touch the chroot and
            # is not any more: finishing_touches.sh pip-installs `inputs' inside
            # it, and that needs DNS.  Put it back for the rest of the stage --
            # after the snapshot, so the cached copy is exactly what cleanup
            # produced -- and take it away again below, before the rootfs is
            # written into the image.
            printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
                sudo tee MixOSBuild/etc/resolv.conf >/dev/null
        elif [[ "$script" == finishing_touches.sh ]]; then
            sudo rm -f MixOSBuild/etc/resolv.conf
        fi
    done
    mark finalization
fi

[[ -s "$DISK" ]] || fail "the image was not produced: $DISK"
mark complete
# What this build's artifacts are, so a run made after the layout changes again can
# tell that they are not what it was asked for -- even after the image is deleted.
layout_signature > "$STATE_DIR/layout"
printf '%s\n' "$DISK" > "$STATE_DIR/latest-image"
rm -f "$CURRENT_STAGE"
log "R36/RG351MP build completed successfully: $DISK"
