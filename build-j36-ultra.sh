#!/usr/bin/env bash
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
#   - the DTB, generated on the host from the vendored MVII board sources;
#   - the ARMv7 kernel workspace, input module and boot.img built in the VM.
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
RESUME_R36="${J36_RESUME_R36:-1}"
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/j36-artifacts"
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

# The DTB is generated on the host from those board sources, and its generator
# asserts on the panel record table and on the keypad pad mux.  Run it before
# anything slow: a keymap or pad-mux regression should fail in a second, not after
# the base image has been rebuilt.
darkos_log "Regenerating the DTB from the vendored MVII board sources"
"$ROOT/build-j36-ultra-dtb.sh"

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
    bash "$VM_BUILD_DIR/device/j36-ultra/build-in-vm.sh"

darkos_log "J36 Ultra artifacts are ready: $ARTIFACT_DIR"
printf '  %s\n' \
    "$ARTIFACT_DIR/boot.img" \
    "$ARTIFACT_DIR/mt6592-j36-ultra.dtb" \
    "$ARTIFACT_DIR/j36_mt6592_input.ko" \
    "$ARTIFACT_DIR/manifest.txt"
darkos_log "Copy $ARTIFACT_DIR/sd-boot/ onto the card's BOOT partition"
darkos_warn "The R36 base image kernel is arm64 and this SoC is ARMv7; only the armhf rootfs is shared. sd-boot/mvii/boot.conf is what points the MVII LK at the 32-bit kernel."
