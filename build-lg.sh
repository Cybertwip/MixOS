#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/lg-k20plus/LICENSE for the texts and for what they do not cover.
# Build the LG K20 Plus (MSM8917) phone layer in the Multipass VM.
#
# THIS IS THE J36 WRAPPER'S SIBLING, NOT A FORK OF IT. It reuses the same VM
# machinery (device/common/multipass.sh) and the same two modes, but its
# deliverable is a phone boot image, not an SD card image, so there is no R36
# base to resume:
#
#   ./build-lg.sh              boot.img + trixie.img + rootfs tarball,
#                                one model dir per device, into
#                                MixOS-Artifacts/lg/<device>/
#   ./build-lg.sh --mix-only   board specifics only (boot/ + root/ dirs)
#   LG_DEVICE=lv517-rev0 ./build-lg.sh
#   ./build-lg.sh --list-devices
#
# More LG devices arrive as rows in device/lg-k20plus/devices.sh; this
# script needs no edits for them.

# ── Why this re-execs itself, and why from a copy ─────────────────────────────
# Same reason as build-j36-ultra.sh: `sh' on macOS is bash 3.2 in POSIX mode
# and will not run this file, and bash reads a running script block by block,
# so an edit mid-run (a checkout, a rebase) corrupts the run AFTER the long
# VM build has finished. A run executes a snapshot taken at startup.
if [ -z "${BASH_VERSION:-}" ] || [ -z "${LG_SNAPSHOT:-}" ]; then
    LG_ROOT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)" || exit 1
    LG_SNAPSHOT="$(mktemp "${TMPDIR:-/tmp}/build-lg.XXXXXX")" || exit 1
    cat -- "$0" > "$LG_SNAPSHOT" || { rm -f -- "$LG_SNAPSHOT"; exit 1; }
    export LG_ROOT_DIR LG_SNAPSHOT
    exec bash "$LG_SNAPSHOT" "$@"
fi

set -Eeuo pipefail

trap 'case "${LG_SNAPSHOT:-}" in */build-lg.??????) rm -f -- "$LG_SNAPSHOT" ;; esac' EXIT

ROOT="${LG_ROOT_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)}"
DARKOS_LOG_TAG="build-lg"
# shellcheck source=device/common/multipass.sh
. "$ROOT/device/common/multipass.sh"
# shellcheck source=device/lg-k20plus/devices.sh
. "$ROOT/device/lg-k20plus/devices.sh"

VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
BASE_ARTIFACT_DIR="${MIXOS_ARTIFACT_DIR:-${DARKOS_ARTIFACT_DIR:-$(darkos_artifact_dir "$ROOT")}}"
DEVICE="${LG_DEVICE:-$(lg_default_device)}"
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" lg "$DEVICE")"
MIX_ONLY=0
COMPRESS=0
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/lg-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/lg-work"

usage() {
    cat <<USAGE
Usage: ./build-lg.sh [--mix-only | --compress] [--device CODENAME] [--list-devices]

Builds the LG phone layer for one device (default: $DEVICE) in the $VM_NAME VM.

    ./build-lg.sh              boot.img + trixie.img + rootfs tarball into $ARTIFACT_DIR
    ./build-lg.sh --mix-only   board specifics only, into $ARTIFACT_DIR:
                                     boot/   boot.img + DTB (fastboot + inspection)
                                     root/   /opt/mixos payload + manifest
    ./build-lg.sh --compress   also archive the deliverables as .zip
    ./build-lg.sh --list-devices
                                 supported LG codenames, one per line

Overrides:
  LG_DEVICE=lv517-rev0 ./build-lg.sh
  LG_FIRMWARE_DIR=/path/to/blobs ./build-lg.sh   (modem.mdt, wcnss.mdt, ...)
  LG_KERNEL_BRANCH=linux-6.12.y ./build-lg.sh
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mix-only) MIX_ONLY=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        --device) DEVICE="${2:?--device needs a codename}"; shift 2 ;;
        --list-devices) lg_devices; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "$MIX_ONLY" == 1 && "$COMPRESS" == 1 ]]; then
    darkos_die "--compress cannot be combined with --mix-only."
fi
DEVICE_INFO="$(lg_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" lg "$DEVICE")"

[[ "$(uname -s)" == "Darwin" ]] || darkos_die "run this wrapper on macOS"
if [[ "$COMPRESS" == 1 ]]; then
    command -v zip >/dev/null 2>&1 || darkos_die "--compress needs 'zip'."
    command -v unzip >/dev/null 2>&1 || darkos_die "--compress needs 'unzip'."
fi

darkos_log "LG $LG_DEVICE ($LG_NOTES)"
darkos_multipass_ready
darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
darkos_vm_refuse_concurrent_build "$VM_NAME"
darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"

# Layout migration, same rule as darkos_artifact_dir: this device's old
# lg-<device>/ dir follows the build to lg/<device>/ instead of being
# left behind to look like a current output.
if [[ ! -d "$ARTIFACT_DIR" && -d "$BASE_ARTIFACT_DIR/lg-$DEVICE" ]]; then
    darkos_log "Moving $BASE_ARTIFACT_DIR/lg-$DEVICE to $ARTIFACT_DIR"
    mkdir -p "$BASE_ARTIFACT_DIR/lg"
    mv -- "$BASE_ARTIFACT_DIR/lg-$DEVICE" "$ARTIFACT_DIR"
fi
mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"
VM_EXPORT_DIR="$VM_ARTIFACT_MOUNT"
FIRMWARE_MOUNT=""
if [[ -n "${LG_FIRMWARE_DIR:-}" && -d "${LG_FIRMWARE_DIR:-}" ]]; then
    darkos_vm_remount "$VM_NAME" "$LG_FIRMWARE_DIR:/mnt/lg-firmware"
    FIRMWARE_MOUNT="/mnt/lg-firmware"
fi

darkos_log "Building the LG $DEVICE layer"
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    LG_BUILD_DIR="$VM_BUILD_DIR" \
    LG_WORK_DIR="$VM_WORK_DIR-$DEVICE" \
    LG_EXPORT_DIR="$VM_EXPORT_DIR" \
    LG_DEVICE="$DEVICE" \
    LG_MIX_ONLY="$MIX_ONLY" \
    LG_JOBS="${LG_JOBS:-}" \
    LG_KERNEL_BRANCH="${LG_KERNEL_BRANCH:-linux-6.12.y}" \
    LG_KERNEL_URL="${LG_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}" \
    LG_FIRMWARE_DIR="$FIRMWARE_MOUNT" \
    DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}" \
    bash "$VM_BUILD_DIR/device/lg-k20plus/build-in-vm.sh" || BUILD_RC=$?

if [[ "$BUILD_RC" != 0 ]]; then
    darkos_warn "The LG $DEVICE layer FAILED (exit $BUILD_RC). NOTHING WAS HANDED OVER."
    exit "$BUILD_RC"
fi
darkos_warn_layout_strays "$BASE_ARTIFACT_DIR"

# ── Handing over: the VM wrote into the mounted artifact dir, so verify ─────
# from this side of the mount (same lesson as the J36 wrapper: a copy that
# succeeded in the VM and left nothing here is a real failure mode).
MANIFEST="$ARTIFACT_DIR/lg-$DEVICE-manifest.txt"
[[ -f "$MANIFEST" ]] || darkos_die "the build reported success but wrote no manifest"
BOOTIMG=""
TRIXIEIMG=""
ROOTFS=""
while IFS='=' read -r key value; do
    case "$key" in
        bootimg) BOOTIMG="$value" ;;
        trixieimg) TRIXIEIMG="$value" ;;
        rootfs) ROOTFS="$value" ;;
    esac
done < "$MANIFEST"

if [[ "$MIX_ONLY" == 1 ]]; then
    [[ -f "$ARTIFACT_DIR/boot/$BOOTIMG" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/$BOOTIMG"
    darkos_log "LG $DEVICE board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> boot.img + DTB (fastboot flash boot)" \
        "$ARTIFACT_DIR/root/   -> /opt/mixos payload for the ROOTFS partition"
    darkos_log "No rootfs was built. Run ./build-lg.sh with no flag for that."
else
    [[ -f "$ARTIFACT_DIR/$BOOTIMG" ]] || darkos_die "missing $ARTIFACT_DIR/$BOOTIMG"
    [[ -f "$ARTIFACT_DIR/$TRIXIEIMG" ]] || darkos_die "missing $ARTIFACT_DIR/$TRIXIEIMG"
    [[ "$ROOTFS" == none || -f "$ARTIFACT_DIR/$ROOTFS" ]] \
        || darkos_die "missing $ARTIFACT_DIR/$ROOTFS"
    if [[ "$COMPRESS" == 1 ]]; then
        (cd -- "$ARTIFACT_DIR" && rm -f "lg-$DEVICE.part.zip" \
            && zip -9 -q "lg-$DEVICE.part.zip" "$BOOTIMG" "$TRIXIEIMG" "$ROOTFS" \
            && unzip -tqq "lg-$DEVICE.part.zip" \
            && mv -f "lg-$DEVICE.part.zip" "lg-$DEVICE.zip") \
            || darkos_die "compression failed"
        darkos_log "Compressed copy: $ARTIFACT_DIR/lg-$DEVICE.zip"
    fi
    darkos_log "Flash this: fastboot flash boot $ARTIFACT_DIR/$BOOTIMG"
    darkos_log "Write this onto the phone's ROOTFS partition (PARTLABEL=ROOTFS, must hold $(du -h "$ARTIFACT_DIR/$TRIXIEIMG" | awk '{print $1}')): $ARTIFACT_DIR/$TRIXIEIMG"
    if [[ "$ROOTFS" != none ]]; then
        darkos_log "Unpack-once alternative for a rooted shell or recovery: $ARTIFACT_DIR/$ROOTFS"
    fi
fi
