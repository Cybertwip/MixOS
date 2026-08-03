#!/usr/bin/env bash
# Incrementally build the separate ARMv7/MT6592 J36 Ultra bring-up layer.
# Reuses the persistent Multipass VM created by build-r36-ultra.sh, but never
# reruns or patches the incompatible AArch64/RK3326 image.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
VM_NAME="${DARKOS_VM_NAME:-darkos-r36}"
VM_CPUS="${DARKOS_VM_CPUS:-8}"
VM_MEMORY="${DARKOS_VM_MEMORY:-16G}"
VM_DISK="${DARKOS_VM_DISK:-160G}"
UBUNTU_IMAGE="${DARKOS_UBUNTU_IMAGE:-24.04}"
POWERENGINE_ROOT="${POWERENGINE_ROOT:-$(dirname "$ROOT")/PowerEngineV3/PowerEngine}"
DRIVERS_HOST="${J36_DRIVERS_DIR:-$POWERENGINE_ROOT/OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers}"
ARTIFACT_DIR="${J36_ARTIFACT_DIR:-${ROOT}-artifacts/j36-ultra}"
VM_SOURCE_MOUNT="/mnt/darkos-host"
VM_DRIVERS_MOUNT="/mnt/j36-drivers-host"
VM_ARTIFACT_MOUNT="/mnt/j36-artifacts"
VM_BUILD_DIR="/home/ubuntu/dArkOS"
VM_WORK_DIR="/home/ubuntu/j36-ultra-work"

log() { printf '\n[build-j36-ultra] %s\n' "$*"; }
die() { printf '\n[build-j36-ultra] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<USAGE
Usage: ./build-j36-ultra.sh

Reuses Multipass VM: $VM_NAME
Writes artifacts to: $ARTIFACT_DIR

The first run clones/builds the small J36 ARMv7 kernel workspace. Later runs
reuse that kernel build and only rebuild changed DTB/module/boot artifacts.
It does not rerun 'make rg351mp'.

Overrides:
  J36_UPDATE_KERNEL=1 ./build-j36-ultra.sh
  J36_KERNEL_BRANCH=linux-6.12.y ./build-j36-ultra.sh
  J36_ARTIFACT_DIR=/path/to/output ./build-j36-ultra.sh
USAGE
}

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
    usage
    exit 0
elif [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi

[[ "$(uname -s)" == Darwin ]] || die "run this wrapper on macOS"
[[ -d "$DRIVERS_HOST" ]] || die "PowerEngine J36 Drivers not found: $DRIVERS_HOST"
command -v multipass >/dev/null 2>&1 || die "Multipass is required"

if ! multipass list >/dev/null 2>&1; then
    log "Starting Multipass"
    open -a Multipass >/dev/null 2>&1 || true
    for _ in {1..10}; do
        multipass list >/dev/null 2>&1 && break
        sleep 2
    done
    if ! multipass list >/dev/null 2>&1 && \
       ! launchctl print system/com.canonical.multipassd >/dev/null 2>&1 && \
       [[ -f /Library/LaunchDaemons/com.canonical.multipassd.plist ]]; then
        log "Reloading the installed Multipass daemon"
        osascript -e 'do shell script "mkdir -p /Library/Logs/Multipass; launchctl bootout system/com.canonical.multipassd >/dev/null 2>&1 || true; launchctl bootstrap system /Library/LaunchDaemons/com.canonical.multipassd.plist; launchctl enable system/com.canonical.multipassd; launchctl kickstart -k system/com.canonical.multipassd" with administrator privileges'
    fi
    for _ in {1..20}; do
        multipass list >/dev/null 2>&1 && break
        sleep 2
    done
    multipass list >/dev/null 2>&1 || die "could not connect to Multipass"
fi

if multipass info "$VM_NAME" >/dev/null 2>&1; then
    multipass start "$VM_NAME" >/dev/null 2>&1 || true
else
    log "Creating the persistent Ubuntu VM $VM_NAME"
    multipass launch "$UBUNTU_IMAGE" --name "$VM_NAME" \
        --cpus "$VM_CPUS" --memory "$VM_MEMORY" --disk "$VM_DISK"
fi

# Do not unmount or rewrite the VM checkout while the baseline image is active.
if multipass exec "$VM_NAME" -- bash -lc \
    "pgrep -f '[b]uild_rg351mp.sh|[m]ake[[:space:]].*rg351mp' >/dev/null"; then
    die "the R36/RG351MP baseline build is still running. Re-run this command after it finishes."
fi

log "Regenerating the DTB locally from the current MVII Drivers"
J36_DRIVERS_DIR="$DRIVERS_HOST" "$ROOT/build-j36-ultra-dtb.sh"

mkdir -p "$ARTIFACT_DIR"
multipass umount "$VM_NAME:$VM_SOURCE_MOUNT" >/dev/null 2>&1 || true
multipass umount "$VM_NAME:$VM_DRIVERS_MOUNT" >/dev/null 2>&1 || true
multipass umount "$VM_NAME:$VM_ARTIFACT_MOUNT" >/dev/null 2>&1 || true
multipass mount "$ROOT" "$VM_NAME:$VM_SOURCE_MOUNT"
multipass mount "$DRIVERS_HOST" "$VM_NAME:$VM_DRIVERS_MOUNT"
multipass mount "$ARTIFACT_DIR" "$VM_NAME:$VM_ARTIFACT_MOUNT"

log "Synchronizing only the changed dArkOS/J36 sources into the persistent VM"
multipass exec "$VM_NAME" -- bash -lc "
set -Eeuo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y rsync
mkdir -p '$VM_BUILD_DIR' '$VM_WORK_DIR/powerengine-drivers'
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
    --exclude='build.log*' \\
    --exclude='*.img' \\
    --exclude='*.img.7z*' \\
    '$VM_SOURCE_MOUNT/' '$VM_BUILD_DIR/'
rsync -a --delete '$VM_DRIVERS_MOUNT/' '$VM_WORK_DIR/powerengine-drivers/'
"

log "Running the incremental J36-only build"
multipass exec "$VM_NAME" -- env \
    J36_WORK_DIR="$VM_WORK_DIR" \
    J36_DRIVERS_DIR="$VM_WORK_DIR/powerengine-drivers" \
    J36_EXPORT_DIR="$VM_ARTIFACT_MOUNT" \
    J36_KERNEL_BRANCH="${J36_KERNEL_BRANCH:-linux-6.12.y}" \
    J36_KERNEL_URL="${J36_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}" \
    J36_UPDATE_KERNEL="${J36_UPDATE_KERNEL:-0}" \
    bash "$VM_BUILD_DIR/device/j36-ultra/build-in-vm.sh"

log "J36 Ultra artifacts are ready: $ARTIFACT_DIR"
printf '  %s\n' \
    "$ARTIFACT_DIR/boot.img" \
    "$ARTIFACT_DIR/mt6592-j36-ultra.dtb" \
    "$ARTIFACT_DIR/j36_mt6592_input.ko" \
    "$ARTIFACT_DIR/manifest.txt"
