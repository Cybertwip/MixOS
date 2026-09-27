#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/oppo-mt6877/LICENSE for the texts and for what they do not cover.
# Build the OPPO MT6877 phone layer in the Multipass VM.
#
# THIS IS THE J36 WRAPPER'S SIBLING, NOT A FORK OF IT. It reuses the same VM
# machinery (device/common/multipass.sh) and the same two modes, but its
# deliverable is a phone boot image, not an SD card image, so there is no R36
# base to resume:
#
#   ./build-oppo.sh              boot.img + Debian rootfs tarball, one per
#                                device, into MixOS-Artifacts/
#   ./build-oppo.sh --mix-only   board specifics only (boot/ + root/ dirs)
#   OPPO_DEVICE=20183 ./build-oppo.sh
#   ./build-oppo.sh --list-devices
#
# More OPPO devices arrive as rows in device/oppo-mt6877/devices.sh; this
# script needs no edits for them.

# ── Why this re-execs itself, and why from a copy ─────────────────────────────
# Same reason as build-j36-ultra.sh: `sh' on macOS is bash 3.2 in POSIX mode
# and will not run this file, and bash reads a running script block by block,
# so an edit mid-run (a checkout, a rebase) corrupts the run AFTER the long
# VM build has finished. A run executes a snapshot taken at startup.
if [ -z "${BASH_VERSION:-}" ] || [ -z "${OPPO_SNAPSHOT:-}" ]; then
    OPPO_ROOT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)" || exit 1
    OPPO_SNAPSHOT="$(mktemp "${TMPDIR:-/tmp}/build-oppo.XXXXXX")" || exit 1
    cat -- "$0" > "$OPPO_SNAPSHOT" || { rm -f -- "$OPPO_SNAPSHOT"; exit 1; }
    export OPPO_ROOT_DIR OPPO_SNAPSHOT
    exec bash "$OPPO_SNAPSHOT" "$@"
fi

set -Eeuo pipefail

trap 'case "${OPPO_SNAPSHOT:-}" in */build-oppo.??????) rm -f -- "$OPPO_SNAPSHOT" ;; esac' EXIT

ROOT="${OPPO_ROOT_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)}"
DARKOS_LOG_TAG="build-oppo"
# shellcheck source=device/common/multipass.sh
. "$ROOT/device/common/multipass.sh"
# shellcheck source=device/oppo-mt6877/devices.sh
. "$ROOT/device/oppo-mt6877/devices.sh"

VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
BASE_ARTIFACT_DIR="${MIXOS_ARTIFACT_DIR:-${DARKOS_ARTIFACT_DIR:-$(darkos_artifact_dir "$ROOT")}}"
DEVICE="${OPPO_DEVICE:-$(oppo_default_device)}"
ARTIFACT_DIR="$BASE_ARTIFACT_DIR/oppo-$DEVICE"
MIX_ONLY=0
COMPRESS=0
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/oppo-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/oppo-work"

usage() {
    cat <<USAGE
Usage: ./build-oppo.sh [--mix-only | --compress] [--device CODENAME] [--list-devices]

Builds the OPPO phone layer for one device (default: $DEVICE) in the $VM_NAME VM.

    ./build-oppo.sh              boot.img + rootfs tarball into $BASE_ARTIFACT_DIR
    ./build-oppo.sh --mix-only   board specifics only, into $ARTIFACT_DIR:
                                     boot/   boot.img + DTB (fastboot + inspection)
                                     root/   /opt/mixos payload + manifest
    ./build-oppo.sh --compress   also archive the deliverables as .zip
    ./build-oppo.sh --list-devices
                                 supported OPPO codenames, one per line

Overrides:
  OPPO_DEVICE=20183 ./build-oppo.sh
  OPPO_FIRMWARE_DIR=/path/to/blobs ./build-oppo.sh   (modem.img, WIFI_RAM_CODE, ...)
  OPPO_KERNEL_BRANCH=linux-6.12.y ./build-oppo.sh
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mix-only) MIX_ONLY=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        --device) DEVICE="${2:?--device needs a codename}"; shift 2 ;;
        --list-devices) oppo_devices; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "$MIX_ONLY" == 1 && "$COMPRESS" == 1 ]]; then
    darkos_die "--compress cannot be combined with --mix-only."
fi
DEVICE_INFO="$(oppo_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
ARTIFACT_DIR="$BASE_ARTIFACT_DIR/oppo-$DEVICE"

[[ "$(uname -s)" == "Darwin" ]] || darkos_die "run this wrapper on macOS"
if [[ "$COMPRESS" == 1 ]]; then
    command -v zip >/dev/null 2>&1 || darkos_die "--compress needs 'zip'."
    command -v unzip >/dev/null 2>&1 || darkos_die "--compress needs 'unzip'."
fi

darkos_log "OPPO $OPPO_DEVICE ($OPPO_NOTES)"
darkos_multipass_ready
darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
darkos_vm_refuse_concurrent_build "$VM_NAME"
darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"

mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"
VM_EXPORT_DIR="$VM_ARTIFACT_MOUNT"
FIRMWARE_MOUNT=""
if [[ -n "${OPPO_FIRMWARE_DIR:-}" && -d "${OPPO_FIRMWARE_DIR:-}" ]]; then
    darkos_vm_remount "$VM_NAME" "$OPPO_FIRMWARE_DIR:/mnt/oppo-firmware"
    FIRMWARE_MOUNT="/mnt/oppo-firmware"
fi

darkos_log "Building the OPPO $DEVICE layer"
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    OPPO_BUILD_DIR="$VM_BUILD_DIR" \
    OPPO_WORK_DIR="$VM_WORK_DIR-$DEVICE" \
    OPPO_EXPORT_DIR="$VM_EXPORT_DIR" \
    OPPO_DEVICE="$DEVICE" \
    OPPO_MIX_ONLY="$MIX_ONLY" \
    OPPO_JOBS="${OPPO_JOBS:-}" \
    OPPO_KERNEL_BRANCH="${OPPO_KERNEL_BRANCH:-linux-6.12.y}" \
    OPPO_KERNEL_URL="${OPPO_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}" \
    OPPO_FIRMWARE_DIR="$FIRMWARE_MOUNT" \
    DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}" \
    bash "$VM_BUILD_DIR/device/oppo-mt6877/build-in-vm.sh" || BUILD_RC=$?

if [[ "$BUILD_RC" != 0 ]]; then
    darkos_warn "The OPPO $DEVICE layer FAILED (exit $BUILD_RC). NOTHING WAS HANDED OVER."
    exit "$BUILD_RC"
fi

# ── Handing over: the VM wrote into the mounted artifact dir, so verify ─────
# from this side of the mount (same lesson as the J36 wrapper: a copy that
# succeeded in the VM and left nothing here is a real failure mode).
MANIFEST="$ARTIFACT_DIR/oppo-$DEVICE-manifest.txt"
[[ -f "$MANIFEST" ]] || darkos_die "the build reported success but wrote no manifest"
BOOTIMG=""
ROOTFS=""
while IFS='=' read -r key value; do
    case "$key" in
        bootimg) BOOTIMG="$value" ;;
        rootfs) ROOTFS="$value" ;;
    esac
done < "$MANIFEST"

if [[ "$MIX_ONLY" == 1 ]]; then
    [[ -f "$ARTIFACT_DIR/boot/$BOOTIMG" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/$BOOTIMG"
    darkos_log "OPPO $DEVICE board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> boot.img + DTB (fastboot flash boot)" \
        "$ARTIFACT_DIR/root/   -> /opt/mixos payload for the ROOTFS partition"
    darkos_log "No rootfs was built. Run ./build-oppo.sh with no flag for that."
else
    [[ -f "$ARTIFACT_DIR/$BOOTIMG" ]] || darkos_die "missing $ARTIFACT_DIR/$BOOTIMG"
    [[ "$ROOTFS" == none || -f "$ARTIFACT_DIR/$ROOTFS" ]] \
        || darkos_die "missing $ARTIFACT_DIR/$ROOTFS"
    if [[ "$COMPRESS" == 1 ]]; then
        (cd -- "$ARTIFACT_DIR" && rm -f "oppo-$DEVICE.part.zip" \
            && zip -9 -q "oppo-$DEVICE.part.zip" "$BOOTIMG" "$ROOTFS" \
            && unzip -tqq "oppo-$DEVICE.part.zip" \
            && mv -f "oppo-$DEVICE.part.zip" "oppo-$DEVICE.zip") \
            || darkos_die "compression failed"
        darkos_log "Compressed copy: $ARTIFACT_DIR/oppo-$DEVICE.zip"
    fi
    darkos_log "Flash this: fastboot flash boot $ARTIFACT_DIR/$BOOTIMG"
    if [[ "$ROOTFS" != none ]]; then
        darkos_log "Unpack once onto the phone's ROOTFS partition: $ARTIFACT_DIR/$ROOTFS"
    fi
fi
