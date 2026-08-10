#!/usr/bin/env bash
# Build the dArkOS RG351MP base used for R36 Ultra bring-up.  The default
# profile is a native armhf Debian userspace + EmulationStation with bundled
# applications disabled.  The RK3326 kernel/boot chain remains arm64.
#
# macOS: builds inside a persistent Ubuntu 24.04 Multipass VM.
# Linux: runs the same checkpointed build directly on the host.
#
# Optional environment overrides:
#   DARKOS_VM_NAME=darkos-r36
#   DARKOS_VM_CPUS=8
#   DARKOS_VM_MEMORY=16G
#   DARKOS_VM_DISK=160G
#   DARKOS_UBUNTU_IMAGE=24.04
#   DARKOS_ARTIFACT_DIR=/path/to/output
#   DARKOS_COPY_RAW_IMAGE=0       # Set to 1 to copy the raw .img to macOS.
#   DARKOS_R36_BOOT_PAYLOAD=<artifacts>/Reference/BOOT
#                                 # The BOOT partition of a working dArkOS R36
#                                 # image.  Supplies what this pipeline never
#                                 # produced: the R36S device tree, the u-boot
#                                 # panel variants, the off-charging bitmaps and
#                                 # the dtb selector.  On macOS it has to live
#                                 # under the artifact directory, which is the
#                                 # only host path the build VM can read.
#   DEBIAN_CODE_NAME=trixie
#   USERSPACE_ARCH=armhf          # armhf (default) or arm64; never multiarch.
#   BUILD_JOBS=4
#   BUILD_BUNDLED_APPS=n         # Debian + EmulationStation only (default).
#   ENABLE_CACHE=y

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
DARKOS_LOG_TAG="build-r36-ultra"
# The Multipass daemon repair, the create-or-start, the mount refresh and the
# checkout rsync live here because build-j36-ultra.sh needs the same four.
# shellcheck source=device/common/multipass.sh
. "$SCRIPT_DIR/device/common/multipass.sh"

VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
ARTIFACT_DIR="${DARKOS_ARTIFACT_DIR:-${SCRIPT_DIR}-artifacts}"
COPY_RAW_IMAGE="${DARKOS_COPY_RAW_IMAGE:-0}"
BOOT_PAYLOAD_DIR="${DARKOS_R36_BOOT_PAYLOAD:-${ARTIFACT_DIR}/Reference/BOOT}"
DEBIAN_RELEASE="${DEBIAN_CODE_NAME:-trixie}"
USERSPACE_ARCH="${USERSPACE_ARCH:-armhf}"
BUILD_JOBS="${BUILD_JOBS:-4}"
BUNDLED_APPS="${BUILD_BUNDLED_APPS:-n}"
CACHE="${ENABLE_CACHE:-y}"
if [[ "$BUNDLED_APPS" == "y" ]]; then
    BUILD_PROFILE="full"
else
    BUILD_PROFILE="gui"
fi
STATE_KEY="${DEBIAN_RELEASE}-userspace-${USERSPACE_ARCH}-profile-${BUILD_PROFILE}-v3"
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/darkos-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"

usage() {
    cat <<USAGE
Usage: ./build-r36-ultra.sh

Builds the RG351MP/RK3326 base image used for R36 Ultra bring-up.  By default
this produces one native armhf (32-bit) Debian userspace with the
EmulationStation GUI.  It does not add an arm64 userspace or build the bundled
emulators and standalone applications.  The existing arm64 RK3326 kernel/boot
chain is retained, and the R36 Ultra-specific DTB is not yet injected.

On macOS the script automatically creates or reuses a Multipass VM. Failed
builds resume from the last completed infrastructure checkpoint instead of
recreating the partition image, Debian rootfs, U-Boot, or kernel.
Artifacts are copied to:
  ${ARTIFACT_DIR}

Common overrides:
  DARKOS_VM_CPUS=4 DARKOS_VM_MEMORY=8G ./build-r36-ultra.sh
  BUILD_JOBS=8 ./build-r36-ultra.sh
  DARKOS_COPY_RAW_IMAGE=1 ./build-r36-ultra.sh
  USERSPACE_ARCH=arm64 ./build-r36-ultra.sh
  USERSPACE_ARCH=arm64 BUILD_BUNDLED_APPS=y ./build-r36-ultra.sh

Defaults:
  BUILD_JOBS=4 USERSPACE_ARCH=armhf BUILD_BUNDLED_APPS=n
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
elif [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi

[[ "$USERSPACE_ARCH" == "armhf" || "$USERSPACE_ARCH" == "arm64" ]] || darkos_die "USERSPACE_ARCH must be armhf or arm64."
[[ "$BUNDLED_APPS" == "y" || "$BUNDLED_APPS" == "n" ]] || darkos_die "BUILD_BUNDLED_APPS must be y or n."
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || darkos_die "BUILD_JOBS must be a positive integer."
[[ "$CACHE" == "y" || "$CACHE" == "n" ]] || darkos_die "ENABLE_CACHE must be y or n."
[[ "$COPY_RAW_IMAGE" == "0" || "$COPY_RAW_IMAGE" == "1" ]] || darkos_die "DARKOS_COPY_RAW_IMAGE must be 0 or 1."

run_make() {
    cd "$SCRIPT_DIR"
    darkos_log "Building or resuming RG351MP (Debian ${DEBIAN_RELEASE}, userspace=${USERSPACE_ARCH}, profile=${BUILD_PROFILE}, jobs=${BUILD_JOBS}, cache=${CACHE})"
    env DEBIAN_CODE_NAME="$DEBIAN_RELEASE" \
        USERSPACE_ARCH="$USERSPACE_ARCH" \
        BUILD_JOBS="$BUILD_JOBS" \
        BUILD_BUNDLED_APPS="$BUNDLED_APPS" \
        ENABLE_CACHE="$CACHE" \
        DARKOS_R36_STATE_DIR="${DARKOS_R36_STATE_DIR:-$HOME/darkos-r36-state}" \
        DARKOS_R36_BOOT_PAYLOAD="$BOOT_PAYLOAD_DIR" \
        bash device/r36-ultra/build-in-vm.sh
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    command -v make >/dev/null 2>&1 || darkos_die "make is required."
    run_make
    exit $?
fi

available_kib="$(df -Pk "$SCRIPT_DIR" | awk 'NR == 2 { print $4 }')"
available_gib=$((available_kib / 1024 / 1024))
if (( available_gib < 140 )); then
    darkos_warn "Only about ${available_gib} GiB is free. The initial build may exhaust the disk; 160-180 GiB free is recommended."
fi

darkos_multipass_ready
darkos_vm_ensure "$VM_NAME" "$VM_CPUS" "$VM_MEMORY" "$VM_DISK" "$UBUNTU_IMAGE"

mkdir -p "$ARTIFACT_DIR"
darkos_vm_remount "$VM_NAME" \
    "$SCRIPT_DIR:$VM_SOURCE_MOUNT" \
    "$ARTIFACT_DIR:$VM_ARTIFACT_MOUNT"

darkos_log "Preparing the Ubuntu build environment"
multipass exec "$VM_NAME" -- bash -lc "
set -Eeuo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y git make rsync tmux
printf '%s ALL=(ALL:ALL) NOPASSWD: ALL\\n' \"\$(id -un)\" | sudo tee /etc/sudoers.d/darkos-build >/dev/null
sudo chmod 0440 /etc/sudoers.d/darkos-build
"
darkos_vm_sync_checkout "$VM_NAME" "$VM_SOURCE_MOUNT" "$VM_BUILD_DIR"

# The reference BOOT payload is read with sudo by install_boot.sh, and a
# Multipass host mount belongs to the ubuntu user rather than to root.  Copy it
# into the build directory as that user first; from there root can read it.
# Image and uInitrd are deliberately left behind: the reference kernel would not
# match the modules this build installs into the rootfs.
VM_BOOT_PAYLOAD=""
if [[ -d "$BOOT_PAYLOAD_DIR" ]]; then
    if [[ "$BOOT_PAYLOAD_DIR" == "$ARTIFACT_DIR"/* ]]; then
        payload_relative="${BOOT_PAYLOAD_DIR#"$ARTIFACT_DIR"/}"
        VM_BOOT_PAYLOAD="$VM_BUILD_DIR/device/r36-ultra/boot-payload"
        darkos_log "Staging the reference boot payload from ${BOOT_PAYLOAD_DIR}"
        multipass exec "$VM_NAME" -- bash -lc "
set -Eeuo pipefail
mkdir -p '$VM_BOOT_PAYLOAD'
rsync -rt --delete \\
    --exclude='Image' \\
    --exclude='uInitrd' \\
    --exclude='initrd.img' \\
    --exclude='.DS_Store' \\
    --exclude='.fseventsd' \\
    --exclude='.Spotlight-V100' \\
    --exclude='.Trashes' \\
    '$VM_ARTIFACT_MOUNT/$payload_relative/' '$VM_BOOT_PAYLOAD/'
du -sh '$VM_BOOT_PAYLOAD'
"
    else
        darkos_warn "DARKOS_R36_BOOT_PAYLOAD is outside ${ARTIFACT_DIR}, which the build VM cannot read; the R36S device tree and the off-charging bitmaps will not be installed."
    fi
else
    darkos_warn "No reference boot payload at ${BOOT_PAYLOAD_DIR}; the boot partition will get only the files this build produces."
fi

darkos_log "Starting or resuming the checkpointed R36/RG351MP build"
darkos_log "Completed partition, Debian bootstrap, U-Boot and kernel stages are preserved across retries."

multipass exec "$VM_NAME" -- env \
    DEBIAN_CODE_NAME="$DEBIAN_RELEASE" \
    USERSPACE_ARCH="$USERSPACE_ARCH" \
    BUILD_JOBS="$BUILD_JOBS" \
    BUILD_BUNDLED_APPS="$BUNDLED_APPS" \
    ENABLE_CACHE="$CACHE" \
    DARKOS_R36_STATE_DIR="/home/ubuntu/darkos-r36-state" \
    DARKOS_R36_BOOT_PAYLOAD="$VM_BOOT_PAYLOAD" \
    bash "$VM_BUILD_DIR/device/r36-ultra/build-in-vm.sh"

multipass exec "$VM_NAME" -- bash -lc '
set -Eeuo pipefail
STATE_DIR=$1
BUILD_DIR=$2
ARTIFACT_DIR=$3
COPY_RAW=$4
read -r IMAGE < "$STATE_DIR/latest-image"
cd "$BUILD_DIR"
[[ -s "$IMAGE" ]] || { echo "missing image: $IMAGE" >&2; exit 1; }
[[ -f "${IMAGE}.7z.001" ]] || { echo "missing archive: ${IMAGE}.7z.001" >&2; exit 1; }
7z t "${IMAGE}.7z.001"
sudo parted -s "$IMAGE" print
cp -f -- "${IMAGE}.7z."* "$ARTIFACT_DIR/"
cp -f -- "$STATE_DIR/resume.log" "$ARTIFACT_DIR/build-r36-ultra-resume.log"
[[ -f build.log ]] && cp -f -- build.log "$ARTIFACT_DIR/build.log" || true
if [[ "$COPY_RAW" == 1 ]]; then
    cp -f -- "$IMAGE" "$ARTIFACT_DIR/"
fi
printf "%s\n" "$IMAGE" > "$ARTIFACT_DIR/latest-image.txt"
sync "$ARTIFACT_DIR"
' artifact-copy \
    "/home/ubuntu/darkos-r36-state/${STATE_KEY}" \
    "$VM_BUILD_DIR" \
    "$VM_ARTIFACT_MOUNT" \
    "$COPY_RAW_IMAGE"

darkos_log "Build completed and verified."
darkos_log "Artifacts: ${ARTIFACT_DIR}"
darkos_warn "This is the RG351MP/RK3326 base image. It does not yet contain the R36 Ultra-specific DTB layer."
