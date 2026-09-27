#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/oppo-a77-4g/LICENSE for the texts and for what they do not cover.
# Build the OPPO A77 (MT6765) phone layer in the Multipass VM.
#
# BRING-UP SCAFFOLD: this builds an image that will not boot until
# device/oppo-a77-4g/BRINGUP.md lands hardware facts. It refuses to run without
# OPPO_A77_4G_BRINGUP_ACK=1. Same two modes as the proven phone wrappers; no
# root/ payload in --mix-only yet (no drivers), and no layout migration
# (nothing predates this tree).
#
#   ./build-oppo-a77-4g.sh              one MixOS_<arch>_<debian>_<commit>.img
#                                  per device, into MixOS-Artifacts/oppo/<device>/
#   ./build-oppo-a77-4g.sh --mix-only   boot specifics only (boot/ dir)
#   A77_4G_DEVICE=cph2385 ./build-oppo-a77-4g.sh
#   ./build-oppo-a77-4g.sh --list-devices
#
# More A77 devices arrive as rows in device/oppo-a77-4g/devices.sh; this
# script needs no edits for them.

# ── Why this re-execs itself, and why from a copy ─────────────────────────────
# Same reason as build-j36-ultra.sh: `sh' on macOS is bash 3.2 in POSIX mode
# and will not run this file, and bash reads a running script block by block,
# so an edit mid-run (a checkout, a rebase) corrupts the run AFTER the long
# VM build has finished. A run executes a snapshot taken at startup.
if [ -z "${BASH_VERSION:-}" ] || [ -z "${A77_4G_SNAPSHOT:-}" ]; then
    A77_4G_ROOT_DIR="$(cd -- "$(dirname -- "$0")" && pwd -P)" || exit 1
    A77_4G_SNAPSHOT="$(mktemp "${TMPDIR:-/tmp}/build-oppo-a77-4g.XXXXXX")" || exit 1
    cat -- "$0" > "$A77_4G_SNAPSHOT" || { rm -f -- "$A77_4G_SNAPSHOT"; exit 1; }
    export A77_4G_ROOT_DIR A77_4G_SNAPSHOT
    exec bash "$A77_4G_SNAPSHOT" "$@"
fi

set -Eeuo pipefail

trap 'case "${A77_4G_SNAPSHOT:-}" in */build-oppo-a77-4g.??????) rm -f -- "$A77_4G_SNAPSHOT" ;; esac' EXIT

ROOT="${A77_4G_ROOT_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)}"
DARKOS_LOG_TAG="build-a77"
# shellcheck source=device/common/multipass.sh
. "$ROOT/device/common/multipass.sh"
# shellcheck source=device/oppo-a77-4g/devices.sh
. "$ROOT/device/oppo-a77-4g/devices.sh"

VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
BASE_ARTIFACT_DIR="${MIXOS_ARTIFACT_DIR:-${DARKOS_ARTIFACT_DIR:-$(darkos_artifact_dir "$ROOT")}}"
DEVICE="${A77_4G_DEVICE:-$(a77_4g_default_device)}"
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" oppo "$DEVICE")"
MIX_ONLY=0
COMPRESS=0
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/a77-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/a77-work"

usage() {
    cat <<USAGE
Usage: ./build-oppo-a77-4g.sh [--mix-only | --compress] [--device CODENAME] [--list-devices]

Builds the OPPO A77 layer for one device (default: $DEVICE) in the $VM_NAME VM.
BRING-UP SCAFFOLD: needs OPPO_A77_4G_BRINGUP_ACK=1 (see device/oppo-a77-4g/BRINGUP.md).

    ./build-oppo-a77-4g.sh              one MixOS_<arch>_<debian>_<commit>.img into $ARTIFACT_DIR
    ./build-oppo-a77-4g.sh --mix-only   boot specifics only, into $ARTIFACT_DIR:
                                     boot/   boot.img + DTB + DTS (fastboot + inspection)
    ./build-oppo-a77-4g.sh --compress   also archive the deliverables as .zip
    ./build-oppo-a77-4g.sh --list-devices
                                 supported A77 codenames, one per line

Overrides:
  A77_4G_DEVICE=cph2385 ./build-oppo-a77-4g.sh
  A77_4G_FIRMWARE_DIR=/path/to/blobs ./build-oppo-a77-4g.sh   (staged once drivers request it)
  A77_4G_FB_BASE=0x40000000 ./build-oppo-a77-4g.sh            (enables simplefb; 0 omits it)
  A77_4G_KERNEL_BRANCH=linux-6.12.y ./build-oppo-a77-4g.sh
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --mix-only) MIX_ONLY=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        --device) DEVICE="${2:?--device needs a codename}"; shift 2 ;;
        --list-devices) a77_4g_devices; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

if [[ "${OPPO_A77_4G_BRINGUP_ACK:-}" != 1 ]]; then
    darkos_die "device/oppo-a77-4g is bring-up scaffolding (see BRINGUP.md): it builds but will not boot. Set OPPO_A77_4G_BRINGUP_ACK=1 to build anyway."
fi

if [[ "$MIX_ONLY" == 1 && "$COMPRESS" == 1 ]]; then
    darkos_die "--compress cannot be combined with --mix-only."
fi
DEVICE_INFO="$(a77_4g_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
ARTIFACT_DIR="$(darkos_model_artifact_dir "$BASE_ARTIFACT_DIR" oppo "$DEVICE")"

[[ "$(uname -s)" == "Darwin" ]] || darkos_die "run this wrapper on macOS"
if [[ "$COMPRESS" == 1 ]]; then
    command -v zip >/dev/null 2>&1 || darkos_die "--compress needs 'zip'."
    command -v unzip >/dev/null 2>&1 || darkos_die "--compress needs 'unzip'."
fi

darkos_log "A77 $A77_4G_DEVICE ($A77_4G_NOTES)"
darkos_multipass_ready
darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"
darkos_vm_remount "$VM_NAME" "$ROOT:$VM_SOURCE_MOUNT"
darkos_vm_refuse_concurrent_build "$VM_NAME"
darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"

mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"
VM_EXPORT_DIR="$VM_ARTIFACT_MOUNT"
FIRMWARE_MOUNT=""
if [[ -n "${A77_4G_FIRMWARE_DIR:-}" && -d "${A77_4G_FIRMWARE_DIR:-}" ]]; then
    darkos_vm_remount "$VM_NAME" "$A77_4G_FIRMWARE_DIR:/mnt/a77-firmware"
    FIRMWARE_MOUNT="/mnt/a77-firmware"
fi

darkos_log "Building the A77 $DEVICE layer"
# Computed on the host: the VM's checkout has no .git to read the commit from.
FULL_IMAGE_NAME="$(darkos_image_name "$ROOT" arm64 "${DEBIAN_CODE_NAME:-trixie}")"
# Cleared before the build, not after (J36 pattern): the handover lives in
# the work dir, which survives runs, so a stale one would otherwise pass as
# this run's.
multipass exec "$VM_NAME" -- rm -f "$VM_WORK_DIR-$DEVICE/artifacts/full-image.txt"
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    A77_4G_BUILD_DIR="$VM_BUILD_DIR" \
    A77_4G_WORK_DIR="$VM_WORK_DIR-$DEVICE" \
    A77_4G_EXPORT_DIR="$VM_EXPORT_DIR" \
    A77_4G_DEVICE="$DEVICE" \
    A77_4G_FULL_IMAGE_NAME="$FULL_IMAGE_NAME" \
    A77_4G_MIX_ONLY="$MIX_ONLY" \
    A77_4G_JOBS="${A77_4G_JOBS:-}" \
    A77_4G_KERNEL_BRANCH="${A77_4G_KERNEL_BRANCH:-linux-6.12.y}" \
    A77_4G_KERNEL_URL="${A77_4G_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}" \
    A77_4G_FIRMWARE_DIR="$FIRMWARE_MOUNT" \
    A77_4G_FB_BASE="${A77_4G_FB_BASE:-0}" \
    DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}" \
    bash "$VM_BUILD_DIR/device/oppo-a77-4g/build-in-vm.sh" || BUILD_RC=$?

if [[ "$BUILD_RC" != 0 ]]; then
    darkos_warn "The A77 $DEVICE layer FAILED (exit $BUILD_RC). NOTHING WAS HANDED OVER."
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
    HANDOVER="$(mktemp -t a77-full-image)"
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
    [[ -f "$ARTIFACT_DIR/boot/a77-$DEVICE-boot.img" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/a77-$DEVICE-boot.img"
    [[ -f "$ARTIFACT_DIR/boot/a77-$DEVICE.dtb" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/a77-$DEVICE.dtb"
    darkos_log "A77 $DEVICE board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> boot.img + DTB + DTS (fastboot boot trials + inspection)"
    darkos_log "No image was built. Run ./build-oppo-a77-4g.sh with no flag for that."
else
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
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=a77-$DEVICE-boot.img bs=512 skip=$BOOT_SKIP count=$BOOT_COUNT"
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=a77-$DEVICE-rootfs.img bs=512 skip=$ROOTFS_SKIP count=$ROOTFS_COUNT"
    darkos_log "  Then: fastboot boot a77-$DEVICE-boot.img (BRINGUP step 5; never flash first)"
fi
