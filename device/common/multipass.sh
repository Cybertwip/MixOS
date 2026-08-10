# shellcheck shell=bash
#
# The macOS build-VM plumbing that build-r36-ultra.sh and build-j36-ultra.sh both
# need, in one place.
#
# WHY this file exists: the two wrappers had the same seventy lines twice -- the
# Multipass daemon repair dance, the create-or-start, the umount/mount refresh and
# the checkout rsync with its exclude list. Two copies of an exclude list is one
# copy that goes stale, and the J36 wrapper is supposed to be an extension of the
# R36 one rather than a fork of it. Nothing here is J36- or R36-specific; the
# device-specific work stays in the wrapper that calls these.
#
# Source it, do not execute it. The caller sets DARKOS_LOG_TAG first if it wants
# its own prefix on these messages.

DARKOS_LOG_TAG="${DARKOS_LOG_TAG:-darkos}"

darkos_log() { printf '\n[%s] %s\n' "$DARKOS_LOG_TAG" "$*"; }
darkos_warn() { printf '\n[%s] WARNING: %s\n' "$DARKOS_LOG_TAG" "$*" >&2; }
darkos_die() { printf '\n[%s] ERROR: %s\n' "$DARKOS_LOG_TAG" "$*" >&2; exit 1; }

# Bring the Multipass daemon up, repairing it if macOS has left it installed but
# unloaded -- which is the state that makes the GUI loop forever at "Waiting for
# daemon". Only the already-installed service is touched, and macOS shows one
# administrator-authentication dialog when it is.
darkos_multipass_ready() {
    command -v multipass >/dev/null 2>&1 ||
        darkos_die "Multipass is required on macOS. Install and launch Multipass first."
    multipass list >/dev/null 2>&1 && return 0

    darkos_log "Starting Multipass"
    open -a Multipass >/dev/null 2>&1 || true
    for _ in {1..10}; do
        multipass list >/dev/null 2>&1 && return 0
        sleep 2
    done

    if ! launchctl print system/com.canonical.multipassd >/dev/null 2>&1 &&
       [[ -f /Library/LaunchDaemons/com.canonical.multipassd.plist ]]; then
        darkos_log "Reloading the installed Multipass daemon"
        osascript -e 'do shell script "mkdir -p /Library/Logs/Multipass; launchctl bootout system/com.canonical.multipassd >/dev/null 2>&1 || true; launchctl bootstrap system /Library/LaunchDaemons/com.canonical.multipassd.plist; launchctl enable system/com.canonical.multipassd; launchctl kickstart -k system/com.canonical.multipassd" with administrator privileges'
    fi

    for _ in {1..20}; do
        multipass list >/dev/null 2>&1 && return 0
        sleep 2
    done
    darkos_die "Could not connect to the Multipass daemon."
}

# darkos_vm_ensure NAME CPUS MEMORY DISK UBUNTU_IMAGE
darkos_vm_ensure() {
    local name=$1 cpus=$2 memory=$3 disk=$4 image=$5

    if multipass info "$name" >/dev/null 2>&1; then
        darkos_log "Reusing Multipass VM $name"
        multipass start "$name" >/dev/null 2>&1 || true
    else
        darkos_log "Creating Ubuntu $image VM $name"
        multipass launch "$image" --name "$name" \
            --cpus "$cpus" --memory "$memory" --disk "$disk"
    fi
}

# darkos_vm_remount NAME HOST_PATH:VM_PATH [HOST_PATH:VM_PATH ...]
#
# Unmount first so that moving the checkout or the artifact directory on the host
# is handled rather than silently serving the old path.
darkos_vm_remount() {
    local name=$1
    shift
    local pair host vm

    for pair in "$@"; do
        host="${pair%%:*}"
        vm="${pair#*:}"
        multipass umount "$name:$vm" >/dev/null 2>&1 || true
    done
    for pair in "$@"; do
        host="${pair%%:*}"
        vm="${pair#*:}"
        multipass mount "$host" "$name:$vm"
    done
}

# The build directories and downloaded trees that must never be copied from the
# host: they are either enormous, machine-generated inside the VM, or both.
DARKOS_SYNC_EXCLUDES=(
    '.git/'
    'Arkbuild/'
    'Arkbuild32/'
    'Arkbuild_ccache/'
    'Arkbuild_package_cache/'
    'prebuilts/'
    'mnt/'
    'main/'
    'initrd/'
    'rg351/'
    'odroidgoA-4.4.y/'
    'u-boot-rk3326/'
    'build.log*'
    '*.img'
    '*.img.7z*'
    'device/r36-ultra/boot-payload/'
)

# darkos_vm_prepare_once NAME STAMP PACKAGE...
#
# Install the VM's own build tools once and remember it.  This used to run
# `apt-get update` plus an install on every single invocation of either wrapper,
# which is most of what "it sets up the system every time" was: minutes of apt
# before any build work, on a VM where nothing had changed.  The stamp lives in
# the VM rather than on the host so that recreating the VM redoes it.
# DARKOS_FORCE_HOST_PREP=1 runs it anyway.
darkos_vm_prepare_once() {
    local name=$1 stamp=$2
    shift 2

    if [[ "${DARKOS_FORCE_HOST_PREP:-0}" != 1 ]] &&
        multipass exec "$name" -- test -f "$stamp" 2>/dev/null; then
        darkos_log "Build tools already installed in $name; skipping apt"
        return 0
    fi

    darkos_log "Installing the VM build tools once"
    multipass exec "$name" -- bash -lc "
set -Eeuo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y $*
printf '%s ALL=(ALL:ALL) NOPASSWD: ALL\\n' \"\$(id -un)\" | sudo tee /etc/sudoers.d/darkos-build >/dev/null
sudo chmod 0440 /etc/sudoers.d/darkos-build
mkdir -p \"\$(dirname '$stamp')\"
touch '$stamp'
"
}

# darkos_vm_sync_checkout NAME SOURCE_MOUNT BUILD_DIR
darkos_vm_sync_checkout() {
    local name=$1 source_mount=$2 build_dir=$3
    local excludes=""
    local pattern

    for pattern in "${DARKOS_SYNC_EXCLUDES[@]}"; do
        excludes+=" --exclude='$pattern'"
    done

    multipass exec "$name" -- bash -lc "
set -Eeuo pipefail
if ! command -v rsync >/dev/null 2>&1; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y rsync
fi
mkdir -p '$build_dir'
rsync -a --delete$excludes '$source_mount/' '$build_dir/'
"
}
