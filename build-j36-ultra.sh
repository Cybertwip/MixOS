#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Add the ARMv7/MT6592 J36 Ultra layer on top of the R36 Ultra build.
#
# THIS IS AN EXTENSION OF build-r36-ultra.sh, NOT A SECOND BUILD SYSTEM.  It used
# to carry its own copy of the Multipass daemon repair, the VM create-or-start and
# the checkout rsync with its own exclude list, and it had to refuse to run while
# an R36 build was in flight because the two would fight over the VM checkout.
# Two entry points that can disagree is one exclude list that goes stale.  Now the
# R36 wrapper owns the VM and the base image, this script resumes it, and what is
# left here is only what is J36-specific:
#
#   - the ARMv7 kernel workspace, DTB, input module and boot.img built in the VM.
#
# Nothing is generated on the workstation.  This script reads the checkout, warns
# about drift, drives Multipass and copies artifacts out; every file the build makes
# is made in the VM, under its own work directory, and arrives here only as an
# artifact under ${ROOT}-artifacts.
#
# It is self-contained: no PowerEngine checkout is needed.  The five MVII board
# files the DTB generator parses are committed at device/j36-ultra/mvii-board and
# refreshed by device/j36-ultra/sync-mvii-board.sh, which is run by hand.
#
# The R36 base build is checkpointed, so resuming a finished one costs seconds.
# Set J36_RESUME_R36=0 to skip it and build the J36 layer against whatever base
# is already in the VM.

# `sh ./build-j36-ultra.sh' is the natural thing to type and it is not what this
# script is.  It uses arrays, [[ ]] and set -E, and it sources a helper that does
# too; macOS /bin/sh is bash 3.2 in POSIX mode and will not run all of it.  Rather
# than document that, re-exec under bash and let either invocation work.
if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
DARKOS_LOG_TAG="build-j36-ultra"
# shellcheck source=device/common/multipass.sh
. "$ROOT/device/common/multipass.sh"

VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
# The MVII board sources are vendored at device/j36-ultra/mvii-board and ride
# into the VM with the checkout, so this build needs no PowerEngine tree.
# POWERENGINE_ROOT is still honoured, but only to notice that the vendored copies
# have drifted -- see the check below -- never as a build input.
BOARD_SRC="$ROOT/device/j36-ultra/mvii-board"
POWERENGINE_ROOT="${POWERENGINE_ROOT:-$(dirname "$ROOT")/PowerEngineV3/PowerEngine}"
DRIVERS_HOST="${J36_DRIVERS_DIR:-$POWERENGINE_ROOT/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers}"
ARTIFACT_DIR="${J36_ARTIFACT_DIR:-${ROOT}-artifacts/j36-ultra}"
# Where build-r36-ultra.sh puts the image archive and latest-image.txt.  Spelt the same
# way it spells it, and honouring the same override, because the payload-carrying archive
# has to land on top of the pre-payload one it wrote -- see the hand-over at the bottom.
BASE_ARTIFACT_DIR="${DARKOS_ARTIFACT_DIR:-${ROOT}-artifacts}"
RESUME_R36="${J36_RESUME_R36:-1}"
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/j36-artifacts"
VM_BASE_ARTIFACT_MOUNT="/mnt/darkos-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/j36-ultra-work"

usage() {
    cat <<USAGE
Usage: ./build-j36-ultra.sh

Resumes the R36 Ultra build (build-r36-ultra.sh, checkpointed) and then adds the
J36 Ultra layer on top of it in the same Multipass VM: $VM_NAME

Writes J36 artifacts to: $ARTIFACT_DIR

The first J36 run creates the persistent ARMv7 Linux 6.12 LTS workspace.  Later
runs reuse it and rebuild only changed kernel, DTB, input-module, initramfs and
boot.img files.

Overrides:
  J36_RESUME_R36=0 ./build-j36-ultra.sh      # J36 layer only, leave the base as is
  J36_UPDATE_KERNEL=1 ./build-j36-ultra.sh
  J36_REBUILD_BUSYBOX=1 ./build-j36-ultra.sh
  J36_KERNEL_BRANCH=linux-6.12.y ./build-j36-ultra.sh
  J36_ARTIFACT_DIR=/path/to/output ./build-j36-ultra.sh
  J36_IMAGE_EXPORT=0 ./build-j36-ultra.sh    # still fold the payload into the image,
                                             # but skip the minutes-long re-split of
                                             # its .7z volumes.  Use when the card is
                                             # already flashed and sd-boot/ -- which
                                             # carries sd-root.tar.gz -- is how it is
                                             # being updated.

Anything build-r36-ultra.sh honours (BUILD_JOBS, USERSPACE_ARCH,
DARKOS_R36_BOOT_PAYLOAD, ...) is inherited by the resumed base build.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
elif [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi

[[ "$(uname -s)" == "Darwin" ]] || darkos_die "run this wrapper on macOS"
[[ "$RESUME_R36" == "0" || "$RESUME_R36" == "1" ]] || darkos_die "J36_RESUME_R36 must be 0 or 1."
[[ -d "$BOARD_SRC" ]] || darkos_die "vendored MVII board sources are missing: $BOARD_SRC
run ./device/j36-ultra/sync-mvii-board.sh to restore them from a PowerEngine checkout"

# A warning, not a failure.  This build is self-contained by design, so a
# PowerEngine tree that happens to be newer than the vendored copies must not stop
# it -- but silently building last week's keymap is worse than being told.
#
# Written as a pipe into a subshell rather than `done < <(...)': macOS /bin/sh is
# bash 3.2 in POSIX mode, which rejects process substitution outright, and it
# rejects it at PARSE time -- so `sh ./build-j36-ultra.sh' died on this block
# before the script had run a single line.
if [[ -d "$DRIVERS_HOST" && -f "$BOARD_SRC/PROVENANCE.txt" ]]; then
    drifted="$(
        grep -E '^[0-9a-f]{64}  ' "$BOARD_SRC/PROVENANCE.txt" |
        while read -r want file; do
            [ -n "${file:-}" ] || continue
            [ -f "$DRIVERS_HOST/$file" ] || continue
            have="$(shasum -a 256 "$DRIVERS_HOST/$file" | awk '{print $1}')"
            [ "$have" = "$want" ] || printf '%s ' "$file"
        done
    )"
    if [[ -n "$drifted" ]]; then
        darkos_warn "MVII drivers have moved since mvii-board/ was vendored: $drifted"
        darkos_warn "run ./device/j36-ultra/sync-mvii-board.sh to pick the changes up"
    fi
fi

# THE DTB IS NOT BUILT HERE.  It used to be, and the reason given was a good one --
# its generator asserts on the JD9365 record table and the keypad pad mux, and those
# assertions should fail in a second rather than after a base image has been rebuilt.
# But building it here wrote three files into device/j36-ultra/generated/, inside the
# checkout, on the one machine in this build that does nothing but edit the checkout;
# it also asked macOS for dtc, fdtget and python3.  Nothing ever read them: the DTB
# that ends up in the image is the one build-in-vm.sh generates into its own work
# directory inside the VM.  So the generation moved there, and moved to the TOP of
# that script -- before the kernel is even cloned -- which keeps the fast failure and
# leaves this tree clean.

if [[ "$RESUME_R36" == "1" ]]; then
    darkos_log "Resuming the R36 Ultra base build; completed stages are skipped"
    "$ROOT/build-r36-ultra.sh"
else
    darkos_log "Skipping the R36 base build (J36_RESUME_R36=0)"
    darkos_multipass_ready
    darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
    darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
    darkos_vm_refuse_concurrent_build "$VM_NAME"
    darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"
fi

# The checkout is already in the VM either way, and device/j36-ultra rode along
# with it -- board sources, kernel patch, input module and all.  The only mount
# still needed is somewhere to put the results.  There used to be a second one,
# /mnt/j36-drivers-host, plus an rsync into the VM to stage the PowerEngine
# drivers; vendoring them removed both.
mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"

darkos_log "Building the J36 Ultra layer"
multipass exec "$VM_NAME" -- env \
    J36_WORK_DIR="$VM_WORK_DIR" \
    J36_EXPORT_DIR="$VM_ARTIFACT_MOUNT" \
    J36_KERNEL_BRANCH="${J36_KERNEL_BRANCH:-linux-6.12.y}" \
    J36_KERNEL_URL="${J36_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}" \
    J36_UPDATE_KERNEL="${J36_UPDATE_KERNEL:-0}" \
    J36_BUSYBOX_URL="${J36_BUSYBOX_URL:-https://git.busybox.net/busybox}" \
    J36_BUSYBOX_BRANCH="${J36_BUSYBOX_BRANCH:-1_36_stable}" \
    J36_REBUILD_BUSYBOX="${J36_REBUILD_BUSYBOX:-0}" \
    J36_DOOM="${J36_DOOM:-1}" \
    J36_DOOM_URL="${J36_DOOM_URL:-https://github.com/ozkl/doomgeneric}" \
    J36_DOOM_COMMIT="${J36_DOOM_COMMIT:-dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284}" \
    J36_LIMA="${J36_LIMA:-1}" \
    J36_MTKDRM="${J36_MTKDRM:-1}" \
    J36_AUDIO="${J36_AUDIO:-1}" \
    J36_GL="${J36_GL:-${J36_ES:-1}}" \
    J36_MIXDASH="${J36_MIXDASH:-1}" \
    J36_PAYLOAD_ON="${J36_PAYLOAD_ON:-root}" \
    J36_IMAGE_EXPORT="${J36_IMAGE_EXPORT:-archive}" \
    bash "$VM_BUILD_DIR/device/j36-ultra/build-in-vm.sh"

# ── Handing over the image that actually carries the payload ──────────────────
#
# WHY THIS STEP EXISTS.  build-r36-ultra.sh, which ran above, copies ${IMAGE}.7z.* to
# the workstation as its last act -- and that archive was written by create_image.sh
# during the base build's finalization, BEFORE this layer injected anything.  So every
# card flashed from it came up with no /opt/mixos and no sd-root.tar.gz on BOOT, no
# matter what the injection reported: the operator was unpacking a snapshot of the image
# taken before the payload went in.
#
# The in-VM script now rewrites the archive from the injected image under the same name.
# This copies that rewrite out, on top of the volumes the base wrapper already put there.
# Into $BASE_ARTIFACT_DIR and not $ARTIFACT_DIR on purpose: latest-image.txt, the
# volumes and the per-file copy stamps are all there, and one image in one place is the
# whole point -- two images of which only one boots is the mistake being fixed.  Sharing
# copy_artifacts.sh's stamp directory with the base wrapper is also what keeps this to
# one transfer per run: next run the wrapper finds the stamp written here and skips.
FLASH_IMAGE=""
FLASH_PAYLOAD=""
if [[ -f "$ARTIFACT_DIR/flashable-image.txt" ]]; then
    while IFS='=' read -r key value; do
        case "$key" in
            image)   FLASH_IMAGE="$value" ;;
            payload) FLASH_PAYLOAD="$value" ;;
        esac
    done < "$ARTIFACT_DIR/flashable-image.txt"
fi

if [[ "$FLASH_PAYLOAD" == "in-image" && -n "$FLASH_IMAGE" && "$FLASH_IMAGE" != none ]]; then
    mkdir -p "$BASE_ARTIFACT_DIR"
    darkos_vm_remount "$VM_NAME" "$BASE_ARTIFACT_DIR:$VM_BASE_ARTIFACT_MOUNT"
    darkos_log "Copying ${FLASH_IMAGE}.7z.* out: the archive with both payloads folded in"
    multipass exec "$VM_NAME" -- bash -lc '
set -Eeuo pipefail
BUILD_DIR=$1
DEST=$2
IMAGE=$3
cd "$BUILD_DIR"
[[ -f "${IMAGE}.7z.001" ]] || { echo "missing archive: ${IMAGE}.7z.001" >&2; exit 1; }
bash device/r36-ultra/copy_artifacts.sh "$DEST" "${IMAGE}.7z."*
printf "%s\n" "$IMAGE" > "$DEST/latest-image.txt"
sync "$DEST"
' j36-image-copy "$VM_BUILD_DIR" "$VM_BASE_ARTIFACT_MOUNT" "$FLASH_IMAGE"
elif [[ -n "$FLASH_IMAGE" && "$FLASH_IMAGE" != none ]]; then
    darkos_warn "${FLASH_IMAGE}.7z.* in $BASE_ARTIFACT_DIR predates the payload and was NOT refreshed."
    darkos_warn "Read the 'image:' lines in the build log; do not expect /opt/mixos on a card flashed from it."
else
    darkos_warn "No base image was found in the VM, so there is nothing flashable to hand over."
    darkos_warn "Run without J36_RESUME_R36=0 so the base build produces one."
fi

darkos_log "J36 Ultra artifacts are ready: $ARTIFACT_DIR"
printf '  %s\n' \
    "$ARTIFACT_DIR/boot.img" \
    "$ARTIFACT_DIR/mt6592-j36-ultra.dtb" \
    "$ARTIFACT_DIR/j36_mt6592_input.ko" \
    "$ARTIFACT_DIR/manifest.txt"
# FLASH AND GO.  The build folds both halves into the image inside the VM -- the
# launcher into the vfat BOOT partition and /opt/mixos into the ext2 OS partition --
# so the normal path is one dd and nothing else.  It has to be that way: the
# workstation here is macOS, which mounts FAT and exFAT and neither ext2 nor btrfs, so
# "untar this onto the OS partition" is a step that cannot be performed at all from the
# machine doing the flashing.
#
# The two artifacts below are still emitted, for updating a card in place from a Linux
# box.  Unpacking sd-root.tar.gz onto BOOT instead of the OS partition is the one wrong
# way to do it: FAT holds neither the ~30 Qt SONAME symlinks nor the execute bits, and
# mixdash would fail before main().
if [[ "$FLASH_PAYLOAD" == "in-image" ]]; then
    darkos_log "Flash this, and nothing else has to be copied: $BASE_ARTIFACT_DIR/${FLASH_IMAGE}.7z.001"
    darkos_log "  7z x ${FLASH_IMAGE}.7z.001   then dd the .img -- the launcher is already on BOOT and /opt/mixos on the OS partition"
fi
darkos_log "The card's three partitions: p1 BOOT vfat (launcher only), p2 ROOTFS ext2 (Debian + /opt/mixos), p3 DATA ext2 (your home, mounted at /home/virtua)"
# The in-place update channel, and it works FROM MACOS -- which the previous wording
# denied.  sd-boot/ now contains sd-root.tar.gz, and /init unpacks it onto the ext2 OS
# partition on the next boot, once per tarball.  Dragging the whole of sd-boot/ onto the
# one partition macOS can mount is therefore a complete update, dashboard included; what
# does not work is unpacking that tarball onto BOOT, because FAT holds neither the ~30 Qt
# SONAME symlinks nor the execute bits and mixdash then dies before main() with
# "invalid ELF header".
darkos_log "To update an already-flashed card without reflashing: copy all of $ARTIFACT_DIR/sd-boot/ onto its BOOT partition"
if [[ -f "$ARTIFACT_DIR/sd-boot/sd-root.tar.gz" ]]; then
    darkos_log "  sd-root.tar.gz rides along on BOOT and /init unpacks it onto the OS partition on the next boot -- so this works from macOS, and only BOOT is written"
    darkos_log "  Do NOT untar it onto BOOT itself: FAT loses the Qt symlinks and the execute bits, which is the 'invalid ELF header' from libQt5Widgets"
elif [[ -f "$ARTIFACT_DIR/sd-root.tar.gz" ]]; then
    darkos_warn "sd-root.tar.gz did not fit on BOOT alongside the launcher, so a card can only be updated by reflashing or from a Linux box:"
    darkos_log "  as root: sudo tar -C /path/to/the/mounted/ROOTFS -xzpf $ARTIFACT_DIR/sd-root.tar.gz --numeric-owner   (the ext2 OS partition -- gives /opt/mixos)"
else
    darkos_warn "No sd-root.tar.gz was produced, so nothing will be on the OS partition: no dashboard, no lima, no Mesa. Check the sd-root lines in the build log."
fi
darkos_log "Check the build log for the 'image:' lines -- they say whether the fold into the image succeeded, and on which partitions"
darkos_warn "The R36 base image kernel is arm64 and this SoC is ARMv7; only the armhf rootfs is shared. sd-boot/mvii/boot.conf is what points the MVII LK at the 32-bit kernel."
