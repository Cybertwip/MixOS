#!/usr/bin/env bash
# Build the dArkOS RG351MP base used for R36 Ultra bring-up.  The default
# profile is Debian + EmulationStation with bundled applications disabled.
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
#   DEBIAN_CODE_NAME=trixie
#   BUILD_ARMHF=y
#   BUILD_JOBS=4
#   BUILD_BUNDLED_APPS=n         # Debian + EmulationStation only (default).
#   ENABLE_CACHE=y

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
ARTIFACT_DIR="${DARKOS_ARTIFACT_DIR:-${SCRIPT_DIR}-artifacts}"
COPY_RAW_IMAGE="${DARKOS_COPY_RAW_IMAGE:-0}"
DEBIAN_RELEASE="${DEBIAN_CODE_NAME:-trixie}"
ARMHF="${BUILD_ARMHF:-y}"
BUILD_JOBS="${BUILD_JOBS:-4}"
BUNDLED_APPS="${BUILD_BUNDLED_APPS:-n}"
CACHE="${ENABLE_CACHE:-y}"
if [[ "$BUNDLED_APPS" == "y" ]]; then
    BUILD_PROFILE="full"
else
    BUILD_PROFILE="gui"
fi
STATE_KEY="${DEBIAN_RELEASE}-armhf-${ARMHF}-profile-${BUILD_PROFILE}-v2"
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_ARTIFACT_MOUNT="/mnt/darkos-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"

log() {
    printf '\n[build-r36-ultra] %s\n' "$*"
}

warn() {
    printf '\n[build-r36-ultra] WARNING: %s\n' "$*" >&2
}

die() {
    printf '\n[build-r36-ultra] ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<USAGE
Usage: ./build-r36-ultra.sh

Builds the RG351MP/RK3326 base image used for R36 Ultra bring-up.  By
default this produces Debian with the EmulationStation GUI and does not build
the bundled emulators or standalone applications.  It does not yet inject an
R36 Ultra-specific DTB.

On macOS the script automatically creates or reuses a Multipass VM. Failed
builds resume from the last completed infrastructure checkpoint instead of
recreating the partition image, Debian rootfs, U-Boot, or kernel.
Artifacts are copied to:
  ${ARTIFACT_DIR}

Common overrides:
  DARKOS_VM_CPUS=4 DARKOS_VM_MEMORY=8G ./build-r36-ultra.sh
  BUILD_JOBS=8 ./build-r36-ultra.sh
  DARKOS_COPY_RAW_IMAGE=1 ./build-r36-ultra.sh
  BUILD_ARMHF=n ./build-r36-ultra.sh
  BUILD_BUNDLED_APPS=y ./build-r36-ultra.sh

Defaults:
  BUILD_JOBS=4 BUILD_ARMHF=y BUILD_BUNDLED_APPS=n
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
elif [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi

[[ "$ARMHF" == "y" || "$ARMHF" == "n" ]] || die "BUILD_ARMHF must be y or n."
[[ "$BUNDLED_APPS" == "y" || "$BUNDLED_APPS" == "n" ]] || die "BUILD_BUNDLED_APPS must be y or n."
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || die "BUILD_JOBS must be a positive integer."
[[ "$CACHE" == "y" || "$CACHE" == "n" ]] || die "ENABLE_CACHE must be y or n."
[[ "$COPY_RAW_IMAGE" == "0" || "$COPY_RAW_IMAGE" == "1" ]] || die "DARKOS_COPY_RAW_IMAGE must be 0 or 1."

run_make() {
    cd "$SCRIPT_DIR"
    log "Building or resuming RG351MP (Debian ${DEBIAN_RELEASE}, profile=${BUILD_PROFILE}, ARMHF=${ARMHF}, jobs=${BUILD_JOBS}, cache=${CACHE})"
    env DEBIAN_CODE_NAME="$DEBIAN_RELEASE" \
        BUILD_ARMHF="$ARMHF" \
        BUILD_JOBS="$BUILD_JOBS" \
        BUILD_BUNDLED_APPS="$BUNDLED_APPS" \
        ENABLE_CACHE="$CACHE" \
        DARKOS_R36_STATE_DIR="${DARKOS_R36_STATE_DIR:-$HOME/darkos-r36-state}" \
        bash device/r36-ultra/build-in-vm.sh
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    command -v make >/dev/null 2>&1 || die "make is required."
    run_make
    exit $?
fi

command -v multipass >/dev/null 2>&1 || die "Multipass is required on macOS. Install and launch Multipass first."

available_kib="$(df -Pk "$SCRIPT_DIR" | awk 'NR == 2 { print $4 }')"
available_gib=$((available_kib / 1024 / 1024))
if (( available_gib < 140 )); then
    warn "Only about ${available_gib} GiB is free. The initial build may exhaust the disk; 160-180 GiB free is recommended."
fi

if ! multipass list >/dev/null 2>&1; then
    log "Starting Multipass"
    open -a Multipass >/dev/null 2>&1 || true
    for _ in {1..10}; do
        multipass list >/dev/null 2>&1 && break
        sleep 2
    done

    # Some macOS updates leave Multipass installed but unload its launch daemon,
    # which makes the GUI loop forever at "Waiting for daemon". Repair only that
    # existing service; macOS will show one administrator-authentication dialog.
    if ! multipass list >/dev/null 2>&1 &&
       ! launchctl print system/com.canonical.multipassd >/dev/null 2>&1 &&
       [[ -f /Library/LaunchDaemons/com.canonical.multipassd.plist ]]; then
        log "Reloading the installed Multipass daemon"
        osascript -e 'do shell script "mkdir -p /Library/Logs/Multipass; launchctl bootout system/com.canonical.multipassd >/dev/null 2>&1 || true; launchctl bootstrap system /Library/LaunchDaemons/com.canonical.multipassd.plist; launchctl enable system/com.canonical.multipassd; launchctl kickstart -k system/com.canonical.multipassd" with administrator privileges'
    fi

    for _ in {1..20}; do
        multipass list >/dev/null 2>&1 && break
        sleep 2
    done
    multipass list >/dev/null 2>&1 || die "Could not connect to the Multipass daemon."
fi

if multipass info "$VM_NAME" >/dev/null 2>&1; then
    log "Reusing Multipass VM ${VM_NAME}"
    multipass start "$VM_NAME" >/dev/null 2>&1 || true
else
    log "Creating Ubuntu ${UBUNTU_IMAGE} VM ${VM_NAME}"
    multipass launch "$UBUNTU_IMAGE" \
        --name "$VM_NAME" \
        --cpus "$VM_CPUS" \
        --memory "$VM_MEMORY" \
        --disk "$VM_DISK"
fi

mkdir -p "$ARTIFACT_DIR"

# Refresh the mounts so moving the checkout or artifact directory is handled.
multipass umount "$VM_NAME:$VM_SOURCE_MOUNT" >/dev/null 2>&1 || true
multipass umount "$VM_NAME:$VM_ARTIFACT_MOUNT" >/dev/null 2>&1 || true
multipass mount "$SCRIPT_DIR" "$VM_NAME:$VM_SOURCE_MOUNT"
multipass mount "$ARTIFACT_DIR" "$VM_NAME:$VM_ARTIFACT_MOUNT"

log "Preparing the Ubuntu build environment"
multipass exec "$VM_NAME" -- bash -lc "
set -Eeuo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y git make rsync tmux
printf '%s ALL=(ALL:ALL) NOPASSWD: ALL\\n' \"\$(id -un)\" | sudo tee /etc/sudoers.d/darkos-build >/dev/null
sudo chmod 0440 /etc/sudoers.d/darkos-build
mkdir -p '$VM_BUILD_DIR'
rsync -a --delete \\
    --exclude='.git/' \\
    --exclude='Arkbuild/' \\
    --exclude='Arkbuild32/' \\
    --exclude='Arkbuild_ccache/' \\
    --exclude='Arkbuild_package_cache/' \\
    --exclude='prebuilts/' \\
    --exclude='mnt/' \\
    --exclude='main/' \\
    --exclude='initrd/' \\
    --exclude='rg351/' \\
    --exclude='odroidgoA-4.4.y/' \\
    --exclude='u-boot-rk3326/' \\
    --exclude='build.log*' \\
    --exclude='*.img' \\
    --exclude='*.img.7z*' \\
    '$VM_SOURCE_MOUNT/' '$VM_BUILD_DIR/'
"

log "Starting or resuming the checkpointed R36/RG351MP build"
log "Completed partition, Debian bootstrap, U-Boot and kernel stages are preserved across retries."

multipass exec "$VM_NAME" -- env \
    DEBIAN_CODE_NAME="$DEBIAN_RELEASE" \
    BUILD_ARMHF="$ARMHF" \
    BUILD_JOBS="$BUILD_JOBS" \
    BUILD_BUNDLED_APPS="$BUNDLED_APPS" \
    ENABLE_CACHE="$CACHE" \
    DARKOS_R36_STATE_DIR="/home/ubuntu/darkos-r36-state" \
    bash "$VM_BUILD_DIR/device/r36-ultra/build-in-vm.sh"

multipass exec "$VM_NAME" -- bash -lc "
set -Eeuo pipefail
STATE_DIR='/home/ubuntu/darkos-r36-state/${STATE_KEY}'
read -r IMAGE < \$STATE_DIR/latest-image
cd '$VM_BUILD_DIR'
[[ -s "\$IMAGE" ]] || { echo "missing image: \$IMAGE" >&2; exit 1; }
[[ -f "\${IMAGE}.7z.001" ]] || { echo "missing archive: \${IMAGE}.7z.001" >&2; exit 1; }
7z t "\${IMAGE}.7z.001"
sudo parted -s "\$IMAGE" print
cp -f "\${IMAGE}.7z."* '$VM_ARTIFACT_MOUNT/'
cp -f "\$STATE_DIR/resume.log" '$VM_ARTIFACT_MOUNT/build-r36-ultra-resume.log'
[[ -f build.log ]] && cp -f build.log '$VM_ARTIFACT_MOUNT/build.log' || true
if [[ '$COPY_RAW_IMAGE' == '1' ]]; then
    cp -f "\$IMAGE" '$VM_ARTIFACT_MOUNT/'
fi
printf '%s\n' "\$IMAGE" > '$VM_ARTIFACT_MOUNT/latest-image.txt'
"

log "Build completed and verified."
log "Artifacts: ${ARTIFACT_DIR}"
warn "This is the RG351MP/RK3326 base image. It does not yet contain the R36 Ultra-specific DTB layer."
