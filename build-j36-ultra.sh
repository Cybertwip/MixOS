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
# artifact under MixOS-Artifacts, next to the checkout.
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
# Where build-r36-ultra.sh puts the image and latest-image.txt.  Derived the same way it
# derives it -- same helper, same two overrides -- because the payload-carrying image has
# to land there and nowhere else; see the hand-over at the bottom.
BASE_ARTIFACT_DIR="${MIXOS_ARTIFACT_DIR:-${DARKOS_ARTIFACT_DIR:-$(darkos_artifact_dir "$ROOT")}}"
# Only --mix-only writes here, and it holds exactly two directories: boot/ and root/.
# See the MIX_ONLY block below for why a full build leaves this untouched.  Hung off the
# base directory rather than computed again, so an operator who moves one moves both.
ARTIFACT_DIR="${J36_ARTIFACT_DIR:-${BASE_ARTIFACT_DIR}/j36-ultra}"
RESUME_R36="${J36_RESUME_R36:-1}"
MIX_ONLY=0
# --no-splash.  Passed to the VM as J36_SPLASH and applied to the bootargs line in
# device/j36-ultra/build-in-vm.sh, which is the only place that line exists.
SPLASH=1
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/j36-artifacts"
VM_BASE_ARTIFACT_MOUNT="/mnt/mixos-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/j36-ultra-work"
# The same path build-r36-ultra.sh hands its in-VM half as DARKOS_R36_STATE_DIR.
# Only the image prune at the bottom reads it, and only to find out which images the
# base build's checkpoints still expect to be there.  Spelt out rather than exported
# from that script because the two wrappers share nothing but the VM.
VM_R36_STATE_DIR="/home/ubuntu/darkos-r36-state"

usage() {
    cat <<USAGE
Usage: ./build-j36-ultra.sh [--mix-only] [--no-splash]

Resumes the R36 Ultra build (build-r36-ultra.sh, checkpointed) and then adds the
J36 Ultra layer on top of it in the same Multipass VM: $VM_NAME

    ./build-j36-ultra.sh              the finished article: one flashable image,
                                      both payloads folded into it, written to
                                      $BASE_ARTIFACT_DIR

    ./build-j36-ultra.sh --mix-only   the board specifics only -- kernel, DTB,
                                      modules, drivers, the mixdash dashboard,
                                      eglprobe.  No base image is built, resumed
                                      or touched, and the result is two
                                      directories in $ARTIFACT_DIR:

                                          boot/   copy onto the card's BOOT
                                                  partition; sd-root.tar.gz rides
                                                  along and /init unpacks it on the
                                                  next boot, so this alone updates
                                                  an already-flashed card, from
                                                  macOS, without reflashing
                                          root/   the same payload unpacked, for
                                                  writing onto the ext2 OS
                                                  partition from a Linux box

                                      This is the iteration loop.  Use it while
                                      debugging; use the full build when done.

    ./build-j36-ultra.sh --no-splash  the same build, with the boot picture off:
                                      writes j36.splash=0 loglevel=7 into
                                      mvii/boot.conf instead of j36.splash=1
                                      loglevel=4.  The panel then shows the
                                      kernel console for the whole boot, which is
                                      what to use when something goes wrong
                                      behind the splash -- a stall in the card
                                      scan, a module that does not probe, a
                                      driver that says why only at loglevel 7.
                                      Combines with --mix-only, and turns off
                                      with a text editor on the card: boot.conf
                                      is on the FAT partition, so the same switch
                                      is one edit away from any machine that can
                                      read it.

The first J36 run creates the persistent ARMv7 Linux 6.12 LTS workspace.  Later
runs reuse it and rebuild only changed kernel, DTB, input-module, initramfs and
boot.img files.

Overrides:
  J36_RESUME_R36=0 ./build-j36-ultra.sh      # build the layer and fold it into
                                             # whatever base image the VM already
                                             # has, without resuming the base build
  J36_UPDATE_KERNEL=1 ./build-j36-ultra.sh
  J36_REBUILD_BUSYBOX=1 ./build-j36-ultra.sh
  J36_KERNEL_BRANCH=linux-6.12.y ./build-j36-ultra.sh
  J36_ARTIFACT_DIR=/path/to/output ./build-j36-ultra.sh --mix-only

Anything build-r36-ultra.sh honours (BUILD_JOBS, USERSPACE_ARCH,
DARKOS_R36_BOOT_PAYLOAD, ...) is inherited by the resumed base build.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mix-only) MIX_ONLY=1; shift ;;
        --no-splash) SPLASH=0; shift ;;
        *) usage >&2; exit 2 ;;
    esac
done

# --mix-only means the base image is not this run's business at all, so resuming the
# base build would be a contradiction: it is the long step, and skipping it is the
# point.  Said out loud rather than silently overridden, because J36_RESUME_R36=1 is
# the default and someone will have it in their shell history.
if [[ "$MIX_ONLY" == 1 && "$RESUME_R36" == "1" ]]; then
    RESUME_R36=0
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
    # DARKOS_DEFER_IMAGE_COPY=1: the base wrapper's last act is to copy its finished
    # image out to the workstation, and this layer is about to inject two payloads into
    # that very image.  Copying it twice moves 8 GB across the host share for nothing,
    # and worse, between the two copies there is a window in which the artifact
    # directory holds an image that boots without /opt/mixos.  So the base build leaves
    # it in the VM and the hand-over at the bottom of this script does the one copy.
    DARKOS_DEFER_IMAGE_COPY=1 "$ROOT/build-r36-ultra.sh"
elif [[ "$MIX_ONLY" == 1 ]]; then
    darkos_log "Board specifics only (--mix-only); the base image is not built or touched"
    darkos_multipass_ready
    darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
    darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
    darkos_vm_refuse_concurrent_build "$VM_NAME"
    darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"
else
    darkos_log "Skipping the R36 base build (J36_RESUME_R36=0)"
    darkos_multipass_ready
    darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
    darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
    darkos_vm_refuse_concurrent_build "$VM_NAME"
    darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"
fi

# The checkout is already in the VM either way, and device/j36-ultra rode along
# with it -- board sources, kernel patch, input module and all.  There used to be a
# second mount, /mnt/j36-drivers-host, plus an rsync into the VM to stage the
# PowerEngine drivers; vendoring them removed both.
#
# WHY THE OUTPUT MOUNT IS --mix-only's ALONE.  A full build ships one file, the image,
# and it goes to $BASE_ARTIFACT_DIR next to latest-image.txt.  It used to also rsync
# twenty-odd intermediates into a j36-ultra/ directory -- boot.img, the bare zImage,
# the cpio, the checksums -- all of which are already inside the image and none of
# which anyone copies anywhere.  Having them sit next to a flashable image is how a
# stale one gets picked up.  So the directory exists only when it is the deliverable,
# and the mount only then too.
if [[ "$MIX_ONLY" == 1 ]]; then
    mkdir -p "$ARTIFACT_DIR"
    darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"
    VM_EXPORT_DIR="$VM_ARTIFACT_MOUNT"
else
    # Left over from a previous --mix-only run; unmounted so that nothing in this run
    # can write into a directory it is not exporting to.
    multipass umount "$VM_NAME:$VM_ARTIFACT_MOUNT" >/dev/null 2>&1 || true
    VM_EXPORT_DIR="$VM_WORK_DIR/export"
    if [[ -d "$ARTIFACT_DIR" ]]; then
        darkos_warn "$ARTIFACT_DIR is from an earlier --mix-only run and is NOT being refreshed."
        darkos_warn "The image this build writes carries the payload; that directory does not."
    fi
fi

# The name of what this run delivers, computed here for the same reason the base build's
# is: .git does not ride into the VM, so `git rev-parse' in there has nothing to read.
#
# The base image the layer goes into carries no commit -- see darkos_base_image_name --
# because nothing in it varies with one.  This does: it is the base plus this checkout's
# kernel, device tree, drivers and dashboard, so it is named after the checkout.  The
# defaults match build-r36-ultra.sh's, which is what decides the base's own name.
J36_IMAGE_NAME="$(darkos_image_name "$ROOT" \
    "${USERSPACE_ARCH:-armhf}" "${DEBIAN_CODE_NAME:-trixie}")"

# THE MANIFEST IS CLEARED BEFORE THE BUILD, NOT AFTER.  flashable-image.txt lives in the
# work directory, which survives between runs, and build-in-vm.sh writes it at the END of
# its image stage.  So a run that dies before that stage leaves the PREVIOUS run's manifest
# in place, still saying payload=in-image about an image built from another commit -- and
# the hand-over below trusts that file.  Clearing it first makes its absence mean exactly
# one thing: this build did not get as far as an image.
multipass exec "$VM_NAME" -- \
    rm -f "$VM_WORK_DIR/artifacts/flashable-image.txt"

darkos_log "Building the J36 Ultra layer"
# The status is caught rather than left to `set -e'.  An abort here stops the script on
# this line, which says nothing about the thing the operator asks first -- was anything
# handed over? -- and leaves that to be inferred from a "Flash this:" line that never
# appears, hundreds of lines below wherever the log actually ends.  Catching it puts the
# answer at the bottom of the run, in the same place a successful run reports its image.
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    J36_IMAGE_NAME="$J36_IMAGE_NAME" \
    J36_WORK_DIR="$VM_WORK_DIR" \
    J36_EXPORT_DIR="$VM_EXPORT_DIR" \
    J36_MIX_ONLY="$MIX_ONLY" \
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
    J36_SPLASH="$SPLASH" \
    J36_PAYLOAD_ON="${J36_PAYLOAD_ON:-root}" \
    bash "$VM_BUILD_DIR/device/j36-ultra/build-in-vm.sh" || BUILD_RC=$?

if [[ "$BUILD_RC" != 0 ]]; then
    darkos_warn "The J36 Ultra layer FAILED (exit $BUILD_RC).  NOTHING WAS HANDED OVER."
    darkos_warn "$BASE_ARTIFACT_DIR has not been written to by this run.  If an image is"
    darkos_warn "sitting there it is from an earlier build and carries none of this checkout."
    darkos_warn "The failure itself is above; the build stops at the first command that fails."
    exit "$BUILD_RC"
fi

# ── Handing over the image that actually carries the payload ──────────────────
#
# WHY THIS STEP EXISTS.  build-r36-ultra.sh finishes the image, and this layer then
# injects two payloads into that very image inside the VM.  Whatever reaches the
# workstation therefore has to be copied out AFTER the injection, not before -- which is
# why the base wrapper was run with DARKOS_DEFER_IMAGE_COPY=1 above.  Getting this the
# wrong way round is not a cosmetic bug: it is a card that boots to a Debian with no
# /opt/mixos on it, and nothing in the log to say why.
#
# Into $BASE_ARTIFACT_DIR and not $ARTIFACT_DIR on purpose: latest-image.txt and the
# per-file copy stamps are there, and one image in one place is the whole point -- two
# images of which only one boots is the mistake being fixed.  Sharing copy_artifacts.sh's
# stamp directory with the base wrapper is also what keeps this to one transfer per run.
#
# The manifest is read out of the VM rather than the artifact directory because in this
# mode there is no artifact directory: nothing is exported except the image itself.
FLASH_IMAGE=""
FLASH_PAYLOAD=""
if [[ "$MIX_ONLY" == 1 ]]; then
    FLASH_PAYLOAD=exported
else
    # Through a temporary file and not `done < <(multipass ...)': macOS /bin/sh is bash
    # 3.2 in POSIX mode, which rejects process substitution at PARSE time, so one such
    # construct anywhere in this file kills `sh ./build-j36-ultra.sh' before line 1 runs
    # -- branch never taken or not.  This file has been bitten by that once already.
    FLASH_MANIFEST="$(mktemp -t j36-flashable)"
    multipass exec "$VM_NAME" -- \
        cat "$VM_WORK_DIR/artifacts/flashable-image.txt" > "$FLASH_MANIFEST" 2>/dev/null || true
    while IFS='=' read -r key value; do
        case "$key" in
            image)   FLASH_IMAGE="$value" ;;
            payload) FLASH_PAYLOAD="$value" ;;
        esac
    done < "$FLASH_MANIFEST"
    rm -f "$FLASH_MANIFEST"
fi

if [[ "$MIX_ONLY" == 1 ]]; then
    :
elif [[ "$FLASH_PAYLOAD" == "in-image" && -n "$FLASH_IMAGE" && "$FLASH_IMAGE" != none ]]; then
    mkdir -p "$BASE_ARTIFACT_DIR"
    darkos_vm_remount "$VM_NAME" "$BASE_ARTIFACT_DIR:$VM_BASE_ARTIFACT_MOUNT"
    darkos_log "Copying $FLASH_IMAGE out: the image with both payloads folded in"
    multipass exec "$VM_NAME" -- bash -lc '
set -Eeuo pipefail
BUILD_DIR=$1
DEST=$2
IMAGE=$3
cd "$BUILD_DIR"
[[ -s "$IMAGE" ]] || { echo "missing image: $IMAGE" >&2; exit 1; }
bash device/r36-ultra/copy_artifacts.sh "$DEST" "$IMAGE"
printf "%s\n" "$IMAGE" > "$DEST/latest-image.txt"
sync "$DEST"
' j36-image-copy "$VM_BUILD_DIR" "$VM_BASE_ARTIFACT_MOUNT" "$FLASH_IMAGE"

    # AND THEN CHECKED FROM THIS SIDE OF THE MOUNT.  Everything above runs in the VM and
    # reports success there, against /mnt/mixos-artifacts -- but what gets flashed is a
    # path on the workstation, and the two are the same directory only for as long as an
    # sshfs mount says so.  A copy that succeeded in the VM and left nothing here is not a
    # hypothetical: it is the failure that sent this build round again with an empty
    # MixOS-Artifacts and a log full of green.  Comparing sizes across the boundary costs
    # two stat calls and turns that into an error at the moment it happens.
    #
    # stat is spelt twice because this half runs on the workstation: BSD stat on macOS,
    # GNU stat on a Linux one.  The VM is always Ubuntu, so its side is GNU only.
    VM_IMAGE_SIZE="$(multipass exec "$VM_NAME" -- stat -c %s "$VM_BUILD_DIR/$FLASH_IMAGE")"
    HOST_IMAGE_SIZE=0
    if [[ -f "$BASE_ARTIFACT_DIR/$FLASH_IMAGE" ]]; then
        HOST_IMAGE_SIZE="$(stat -f %z "$BASE_ARTIFACT_DIR/$FLASH_IMAGE" 2>/dev/null \
            || stat -c %s "$BASE_ARTIFACT_DIR/$FLASH_IMAGE")"
    fi
    if [[ "$HOST_IMAGE_SIZE" != "$VM_IMAGE_SIZE" ]]; then
        darkos_warn "The copy reported success in the VM but $BASE_ARTIFACT_DIR/$FLASH_IMAGE"
        darkos_warn "is $HOST_IMAGE_SIZE bytes and the image in the VM is $VM_IMAGE_SIZE."
        darkos_warn "The image itself is fine and is still at $VM_BUILD_DIR/$FLASH_IMAGE in"
        darkos_warn "the VM; what failed is the hand-over across the Multipass mount."
        darkos_warn "Check that $BASE_ARTIFACT_DIR is mounted at $VM_BASE_ARTIFACT_MOUNT:"
        darkos_warn "  multipass info $VM_NAME"
        darkos_warn "and that the workstation has room for another $VM_IMAGE_SIZE bytes."
        darkos_die "the image was built but did not reach $BASE_ARTIFACT_DIR"
    fi

    # ── and only now, the ones this run superseded ────────────────────────────
    #
    # ONE OUTPUT IMAGE IN THE VM, AND IT IS THE ONE ON THE WORKSTATION.  Every run
    # copies the base to a name carrying the commit it was built from, and nothing
    # ever removed the last one: five had piled up in /home/ubuntu/dArkOS before this
    # block existed -- 23 GB of a disk that also has to hold a chroot, a kernel tree
    # and the base image.  A VM that runs out of room does not fail here; it fails in
    # the middle of a debootstrap or a dd, hours in, with an error about the thing it
    # happened to be writing.
    #
    # AFTER the hand-over AND after the size check, never before.  The only moment it
    # is safe to delete this run's predecessors is when this run's article has been
    # verified to exist on the workstation at the right size.  Pruning first would turn
    # one failed copy into no image anywhere.
    #
    # FOUR THINGS ARE KEPT and each for a reason the next run depends on:
    #   - $FLASH_IMAGE, obviously: it is what was just built.
    #   - *_base.img, because find_base_image() in device/j36-ultra/build-in-vm.sh
    #     falls back to the newest of those when the state directory cannot name one,
    #     and the checkpointed base build treats its own output as a finished stage.
    #   - MixOS_R36_*_File_System.img, which is not an output at all: it is the base
    #     build's rootfs intermediate ($FILESYSTEM in device/r36-ultra/build-in-vm.sh)
    #     and its checkpoint says it exists.
    #   - whatever a latest-image marker names.  Those live one directory down, in
    #     "$STATE_ROOT/<codename>-userspace-<arch>-profile-<profile>-v4", so the loop
    #     reads every one it finds rather than guessing the profile this run used.
    # Removing any of them turns the next run into a full rebuild, which is precisely
    # what the resume exists to avoid.
    multipass exec "$VM_NAME" -- bash -lc '
set -Eeuo pipefail
BUILD_DIR=$1
KEEP=$2
STATE_ROOT=$3
cd "$BUILD_DIR"

# Every checkpoint directory under the state root gets a say, not just this run.
# A delimited string and not an array: this is a here-string handed to a remote
# shell, and one that needs no bash-4 feature is one fewer thing to be wrong about
# an Ubuntu image nobody pinned.
ADOPTED="|"
for marker in "$STATE_ROOT"/latest-image "$STATE_ROOT"/*/latest-image; do
    [[ -f "$marker" ]] || continue
    name=""
    read -r name < "$marker" || true
    [[ -n "$name" ]] && ADOPTED="$ADOPTED$name|"
done

freed=0
for img in MixOS_*.img; do
    [[ -f "$img" ]] || continue
    [[ "$img" == "$KEEP" ]] && continue
    [[ "$ADOPTED" == *"|$img|"* ]] && continue
    case "$img" in
        *_base.img|MixOS_R36_*_File_System.img) continue ;;
    esac
    size="$(stat -c %s "$img")"
    printf "  prune: %s (%s bytes), superseded by %s\n" "$img" "$size" "$KEEP"
    rm -f "$img"
    freed=$(( freed + size ))
done
if (( freed > 0 )); then
    printf "  prune: %s GB reclaimed in the VM\n" "$(( freed / 1024 / 1024 / 1024 ))"
else
    printf "  prune: nothing to remove; %s is the only output image here\n" "$KEEP"
fi
df -h --output=avail "$BUILD_DIR" | tail -1 | xargs printf "  prune: %s free in the VM now\n"
' j36-image-prune "$VM_BUILD_DIR" "$FLASH_IMAGE" "$VM_R36_STATE_DIR"
elif [[ -n "$FLASH_IMAGE" && "$FLASH_IMAGE" != none ]]; then
    darkos_warn "$FLASH_IMAGE was NOT handed over: the payload did not reach it."
    darkos_warn "Read the 'image:' lines in the build log; do not expect /opt/mixos on a card flashed from it."
else
    darkos_warn "No base image was found in the VM, so there is nothing flashable to hand over."
    darkos_warn "Run without J36_RESUME_R36=0 so the base build produces one."
fi

# ── What was produced, and what to do with it ─────────────────────────────────
#
# FLASH AND GO.  A full build folds both halves into the image inside the VM -- the
# launcher into the vfat BOOT partition and /opt/mixos into the ext2 OS partition -- so
# the normal path is one dd and nothing else.  It has to be that way: the workstation
# here is macOS, which mounts FAT and exFAT and neither ext2 nor btrfs, so "untar this
# onto the OS partition" is a step that cannot be performed at all from the machine
# doing the flashing.
#
# --mix-only produces the in-place update channel instead, and that one works from
# macOS too: boot/ contains sd-root.tar.gz, and /init unpacks it onto the ext2 OS
# partition on the next boot, once per tarball.  Dragging the whole of boot/ onto the
# one partition macOS can mount is therefore a complete update, dashboard included.
# What does not work is unpacking that tarball onto BOOT itself, because FAT holds
# neither the ~30 Qt SONAME symlinks nor the execute bits, and mixdash then dies before
# main() with "invalid ELF header".
if [[ "$MIX_ONLY" == 1 ]]; then
    darkos_log "J36 Ultra board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> copy ALL of it onto the card's BOOT partition (vfat; macOS can do this)" \
        "$ARTIFACT_DIR/root/   -> the OS partition's contents, for a Linux box (ext2)"
    if [[ -f "$ARTIFACT_DIR/boot/sd-root.tar.gz" ]]; then
        darkos_log "  boot/sd-root.tar.gz rides along and /init unpacks it onto the OS partition on the next boot, so copying boot/ is a complete update"
        darkos_log "  Do NOT untar it onto BOOT itself: FAT loses the Qt symlinks and the execute bits, which is the 'invalid ELF header' from libQt5Widgets"
    else
        darkos_warn "boot/ has no sd-root.tar.gz, so copying it updates the kernel and launcher only -- /opt/mixos on the card stays as it is."
        darkos_log "  From a Linux box: sudo rsync -a $ARTIFACT_DIR/root/ /path/to/the/mounted/ROOTFS/"
    fi
    darkos_log "No image was built or modified.  Run ./build-j36-ultra.sh with no flag for that."
elif [[ "$FLASH_PAYLOAD" == "in-image" ]]; then
    darkos_log "Flash this, and nothing else has to be copied: $BASE_ARTIFACT_DIR/$FLASH_IMAGE"
    darkos_log "  sudo dd if=$BASE_ARTIFACT_DIR/$FLASH_IMAGE of=/dev/rdiskN bs=4m status=progress"
    darkos_log "  The launcher is already on BOOT and /opt/mixos on the OS partition; no copying afterwards"
    darkos_log "  Iterating on the board specifics after this?  ./build-j36-ultra.sh --mix-only, then copy boot/ onto BOOT"
    darkos_report_stale_images "$BASE_ARTIFACT_DIR" "$FLASH_IMAGE"
fi
darkos_log "The card's three partitions: p1 BOOT vfat (launcher only), p2 ROOTFS ext2 (Debian + /opt/mixos), p3 DATA ext2 (your home, mounted at /home/virtua)"
darkos_log "Check the build log for the 'image:' lines -- they say whether the fold into the image succeeded, and on which partitions"
darkos_warn "The R36 base image kernel is arm64 and this SoC is ARMv7; only the armhf rootfs is shared. sd-boot/mvii/boot.conf is what points the MVII LK at the 32-bit kernel."
