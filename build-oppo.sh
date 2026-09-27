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
#   ./build-oppo.sh              one MixOS_<arch>_<debian>_<commit>.img
#                                per device (boot.img + rootfs folded in),
#                                into MixOS-Artifacts/oppo/<device>/
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
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" oppo "$DEVICE")"
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

    ./build-oppo.sh              one MixOS_<arch>_<debian>_<commit>.img into $ARTIFACT_DIR
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
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" oppo "$DEVICE")"

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

# Layout migration, same rule as darkos_artifact_dir: this device's old
# oppo-<device>/ dir follows the build to oppo/<device>/ instead of being
# left behind to look like a current output.
if [[ ! -d "$ARTIFACT_DIR" && -d "$BASE_ARTIFACT_DIR/oppo-$DEVICE" ]]; then
    darkos_log "Moving $BASE_ARTIFACT_DIR/oppo-$DEVICE to $ARTIFACT_DIR"
    mkdir -p "$BASE_ARTIFACT_DIR/oppo"
    mv -- "$BASE_ARTIFACT_DIR/oppo-$DEVICE" "$ARTIFACT_DIR"
fi
mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"
VM_EXPORT_DIR="$VM_ARTIFACT_MOUNT"
FIRMWARE_MOUNT=""
if [[ -n "${OPPO_FIRMWARE_DIR:-}" && -d "${OPPO_FIRMWARE_DIR:-}" ]]; then
    darkos_vm_remount "$VM_NAME" "$OPPO_FIRMWARE_DIR:/mnt/oppo-firmware"
    FIRMWARE_MOUNT="/mnt/oppo-firmware"
fi

darkos_log "Building the OPPO $DEVICE layer"
# Computed on the host: the VM's checkout has no .git to read the commit from.
FULL_IMAGE_NAME="$(darkos_image_name "$ROOT" arm64 "${DEBIAN_CODE_NAME:-trixie}")"
# Cleared before the build, not after (J36 pattern): the handover lives in
# the work dir, which survives runs, so a stale one would otherwise pass as
# this run's.
multipass exec "$VM_NAME" -- rm -f "$VM_WORK_DIR-$DEVICE/artifacts/full-image.txt"
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    OPPO_BUILD_DIR="$VM_BUILD_DIR" \
    OPPO_WORK_DIR="$VM_WORK_DIR-$DEVICE" \
    OPPO_EXPORT_DIR="$VM_EXPORT_DIR" \
    OPPO_DEVICE="$DEVICE" \
    OPPO_FULL_IMAGE_NAME="$FULL_IMAGE_NAME" \
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
darkos_warn_layout_strays "$BASE_ARTIFACT_DIR"

# ── Handing over: the image crossed the mount, so verify from this side ────
# (same lesson as the J36 wrapper: a copy that succeeded in the VM and left
# nothing here is a real failure mode). The handover is read out of the VM
# work dir, never out of the artifact dir: the model dir holds the image.
FULLIMG=""
BOOT_SKIP=""
BOOT_COUNT=""
ROOTFS_SKIP=""
ROOTFS_COUNT=""
if [[ "$MIX_ONLY" != 1 ]]; then
    # Through a temporary file and not `done < <(multipass ...)' (J36 pattern):
    # macOS /bin/sh rejects process substitution at parse time.
    HANDOVER="$(mktemp -t oppo-full-image)"
    multipass exec "$VM_NAME" -- \
        cat "$VM_WORK_DIR-$DEVICE/artifacts/full-image.txt" > "$HANDOVER" 2>/dev/null || true
    while IFS='=' read -r key value; do
        case "$key" in
            image) FULLIMG="$value" ;;
            boot_skip) BOOT_SKIP="$value" ;;
            boot_count) BOOT_COUNT="$value" ;;
            rootfs_skip) ROOTFS_SKIP="$value" ;;
            rootfs_count) ROOTFS_COUNT="$value" ;;
        esac
    done < "$HANDOVER"
    rm -f "$HANDOVER"
fi

if [[ "$MIX_ONLY" == 1 ]]; then
    [[ -f "$ARTIFACT_DIR/boot/oppo-$DEVICE-boot.img" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/oppo-$DEVICE-boot.img"
    [[ -f "$ARTIFACT_DIR/boot/oppo-$DEVICE.dtb" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/oppo-$DEVICE.dtb"
    darkos_log "OPPO $DEVICE board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> boot.img + DTB (fastboot flash boot)" \
        "$ARTIFACT_DIR/root/   -> /opt/mixos payload for the ROOTFS partition"
    darkos_log "No image was built. Run ./build-oppo.sh with no flag for that."
else
    if [[ -d "$ARTIFACT_DIR/boot" ]]; then
        darkos_warn "$ARTIFACT_DIR/boot is from an earlier --mix-only run and is NOT being refreshed."
    fi
    [[ -n "$FULLIMG" && -n "$BOOT_SKIP" && -n "$BOOT_COUNT" \
        && -n "$ROOTFS_SKIP" && -n "$ROOTFS_COUNT" ]] \
        || darkos_die "the build reported success but the handover is missing or incomplete"
    VM_IMAGE_SIZE="$(multipass exec "$VM_NAME" -- stat -c %s "$VM_WORK_DIR-$DEVICE/artifacts/$FULLIMG")"
    HOST_IMAGE_SIZE=0
    if [[ -f "$ARTIFACT_DIR/$FULLIMG" ]]; then
        HOST_IMAGE_SIZE="$(stat -f %z "$ARTIFACT_DIR/$FULLIMG" 2>/dev/null \
            || stat -c %s "$ARTIFACT_DIR/$FULLIMG")"
    fi
    if [[ "$HOST_IMAGE_SIZE" == 0 || "$HOST_IMAGE_SIZE" != "$VM_IMAGE_SIZE" ]]; then
        darkos_die "the image was built but did not reach $ARTIFACT_DIR ($HOST_IMAGE_SIZE of $VM_IMAGE_SIZE bytes)"
    fi
    printf '%s\n' "$FULLIMG" > "$ARTIFACT_DIR/latest-image.txt"
    # The part files from before the single-image layout are intermediates now;
    # leaving them would look like parallel deliverables. Exact names only.
    for obsolete in "oppo-$DEVICE-boot.img" "oppo-$DEVICE-trixie.img" \
            "oppo-$DEVICE-rootfs.tar.gz" "oppo-$DEVICE-manifest.txt" "oppo-$DEVICE.zip"; do
        if [[ -e "$ARTIFACT_DIR/$obsolete" ]]; then
            darkos_log "Removing superseded $obsolete (its bytes are inside $FULLIMG)"
            rm -f "$ARTIFACT_DIR/$obsolete"
        fi
    done
    if [[ "$COMPRESS" == 1 ]]; then
        (cd -- "$ARTIFACT_DIR" && rm -f "$FULLIMG.part.zip" \
            && zip -9 -q "$FULLIMG.part.zip" "$FULLIMG" \
            && unzip -tqq "$FULLIMG.part.zip" \
            && mv -f "$FULLIMG.part.zip" "$FULLIMG.zip") \
            || darkos_die "compression failed"
        darkos_log "Compressed copy: $ARTIFACT_DIR/$FULLIMG.zip (unzip it before flashing)"
    fi
    darkos_report_stale_images "$ARTIFACT_DIR" "$FULLIMG"
    SIZE="$(ls -lh "$ARTIFACT_DIR/$FULLIMG" | awk '{print $5}')"
    darkos_log "Full OS image ($SIZE): $ARTIFACT_DIR/$FULLIMG"
    darkos_log "  p1 BOOT holds the Android boot.img bytes; p2 ROOTFS the ext4 rootfs"
    darkos_log "  Split it with (sectors, bs=512):"
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=oppo-$DEVICE-boot.img bs=512 skip=$BOOT_SKIP count=$BOOT_COUNT"
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=oppo-$DEVICE-rootfs.img bs=512 skip=$ROOTFS_SKIP count=$ROOTFS_COUNT"
    darkos_log "  Then: fastboot flash boot oppo-$DEVICE-boot.img; dd the rootfs onto a PARTLABEL=ROOTFS partition"
fi
