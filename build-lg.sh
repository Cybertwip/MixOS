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
#   ./build-lg.sh              one MixOS_<arch>_<debian>_<commit>.img
#                                per device (boot.img + rootfs folded in),
#                                into MixOS-Artifacts/lg/<device>/
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

    ./build-lg.sh              one MixOS_<arch>_<debian>_<commit>.img into $ARTIFACT_DIR
    ./build-lg.sh --mix-only   board specifics only, into $ARTIFACT_DIR:
                                     boot/   boot.img + DTB (fastboot + inspection)
                                     root/   /opt/mixos payload + manifest
    ./build-lg.sh --compress   also archive the deliverables as .zip
    ./build-lg.sh --list-devices
                                 supported LG codenames, one per line

Overrides:
  LG_DEVICE=lv517-rev0 ./build-lg.sh
  LG_FIRMWARE_DIR=/path/to/blobs ./build-lg.sh   (modem.mdt, wcnss.mdt, ...)
  LG_KDZ=/path/to/stock.kdz ./build-lg.sh        (unpack offline into firmware/stock/; ignored when LG_FIRMWARE_DIR is set)
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

# Offline firmware, opt-in and preflighted: with LG_KDZ pointing at a
# stock .kdz, the PIL sets are unpacked into firmware/stock/ (reused when
# a previous fetch left modem.mdt there) and used exactly as if
# LG_FIRMWARE_DIR had been set. Before the VM work, so a missing 7z or
# a bad path fails in seconds, not after the kernel build.
if [[ -z "${LG_FIRMWARE_DIR:-}" && -n "${LG_KDZ:-}" ]]; then
    LG_STOCK_DIR="$ROOT/device/lg-k20plus/firmware/stock"
    if [[ -f "$LG_STOCK_DIR/modem.mdt" ]]; then
        darkos_log "Reusing firmware in $LG_STOCK_DIR"
    else
        bash "$ROOT/device/lg-k20plus/firmware/fetch-kdz.sh" "$LG_KDZ" \
            || darkos_die "firmware fetch from $LG_KDZ failed"
    fi
    LG_FIRMWARE_DIR="$LG_STOCK_DIR"
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
# Computed on the host: the VM's checkout has no .git to read the commit from.
FULL_IMAGE_NAME="$(darkos_image_name "$ROOT" arm64 "${DEBIAN_CODE_NAME:-trixie}")"
# Cleared before the build, not after (J36 pattern): the handover lives in
# the work dir, which survives runs, so a stale one would otherwise pass as
# this run's.
multipass exec "$VM_NAME" -- rm -f "$VM_WORK_DIR-$DEVICE/artifacts/full-image.txt"
BUILD_RC=0
multipass exec "$VM_NAME" -- env \
    LG_BUILD_DIR="$VM_BUILD_DIR" \
    LG_WORK_DIR="$VM_WORK_DIR-$DEVICE" \
    LG_EXPORT_DIR="$VM_EXPORT_DIR" \
    LG_DEVICE="$DEVICE" \
    LG_FULL_IMAGE_NAME="$FULL_IMAGE_NAME" \
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
    HANDOVER="$(mktemp -t lg-full-image)"
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
    [[ -f "$ARTIFACT_DIR/boot/lg-$DEVICE-boot.img" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/lg-$DEVICE-boot.img"
    [[ -f "$ARTIFACT_DIR/boot/lg-$DEVICE.dtb" ]] \
        || darkos_die "missing $ARTIFACT_DIR/boot/lg-$DEVICE.dtb"
    darkos_log "LG $DEVICE board artifacts are ready: $ARTIFACT_DIR"
    printf '  %s\n' \
        "$ARTIFACT_DIR/boot/   -> boot.img + DTB (fastboot flash boot)" \
        "$ARTIFACT_DIR/root/   -> /opt/mixos payload for the ROOTFS partition"
    darkos_log "No image was built. Run ./build-lg.sh with no flag for that."
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
    for obsolete in "lg-$DEVICE-boot.img" "lg-$DEVICE-trixie.img" \
            "lg-$DEVICE-rootfs.tar.gz" "lg-$DEVICE-manifest.txt" "lg-$DEVICE.zip"; do
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
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=lg-$DEVICE-boot.img bs=512 skip=$BOOT_SKIP count=$BOOT_COUNT"
    darkos_log "    dd if=$ARTIFACT_DIR/$FULLIMG of=lg-$DEVICE-rootfs.img bs=512 skip=$ROOTFS_SKIP count=$ROOTFS_COUNT"
    darkos_log "  Then: fastboot flash boot lg-$DEVICE-boot.img; dd the rootfs onto a PARTLABEL=ROOTFS partition"
fi
