# shellcheck shell=bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/j36-ultra/LICENSE for the texts and for what they do not cover.
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
#
# ONE RULE FOR EVERY `multipass exec' ADDED HERE OR IN A WRAPPER: never send its
# stdout to /dev/null on this side. Multipass 1.16.1 spins at 100% CPU forever
# when it does, whatever the remote command is -- `multipass exec vm -- echo hello
# >/dev/null' never returns. Redirecting to a file, to a pipe or to a terminal is
# fine, and so is `2>/dev/null', so a command whose chatter is not wanted gets the
# redirect inside the quoted remote command instead, where it costs nothing.

DARKOS_LOG_TAG="${DARKOS_LOG_TAG:-darkos}"

darkos_log() { printf '\n[%s] %s\n' "$DARKOS_LOG_TAG" "$*"; }
darkos_warn() { printf '\n[%s] WARNING: %s\n' "$DARKOS_LOG_TAG" "$*" >&2; }
darkos_die() { printf '\n[%s] ERROR: %s\n' "$DARKOS_LOG_TAG" "$*" >&2; exit 1; }

# darkos_image_name ROOT ARCH CODENAME
#
# The one name every image this project ships has: MixOS_<arch>_<debian>_<commit>.img,
# for instance MixOS_armhf_trixie_cd52cee.img.
#
# WHY A COMMIT AND NOT A DATE.  The name used to end in %m%d%Y, which answers the wrong
# question: two cards flashed a fortnight apart from identical sources got different
# names, and two cards flashed on the same afternoon from different commits got the same
# one.  "Which build is this?" could then only be answered from the operator's memory.
# A commit id answers it exactly, and it is the same identity the boot image and the
# dashboard already stamp themselves with.
#
# COMPUTED ON THE HOST and passed into the VM as DARKOS_IMAGE_NAME, because the checkout
# that is rsynced into the build VM excludes .git/ -- see DARKOS_SYNC_EXCLUDES below --
# so `git rev-parse' in there has nothing to read.
#
# A dirty tree keeps the committed id rather than growing a "-dirty" suffix: the name has
# one shape, and every glob and stamp that looks for it would have to learn a second.
# It says so loudly instead, because the id then describes the build only approximately.
darkos_image_name() {
    local root=$1 arch=$2 codename=$3 commit
    commit="$(git -C "$root" rev-parse --short=7 HEAD 2>/dev/null || true)"
    if [[ -z "$commit" ]]; then
        commit=nogit
        darkos_warn "$root is not a git checkout, so this image is named ..._nogit.img"
    elif [[ -n "$(git -C "$root" status --porcelain 2>/dev/null)" ]]; then
        darkos_warn "the checkout has uncommitted changes, so $commit names this image only approximately"
    fi
    printf 'MixOS_%s_%s_%s.img\n' "$arch" "$codename" "$commit"
}

# darkos_base_image_name ARCH CODENAME
#
# The name of the image the BASE build works on and keeps: MixOS_<arch>_<debian>_base.img.
# One per arch and Debian release, and it never carries a commit.
#
# ── WHY THE BASE MUST NOT BE NAMED AFTER A COMMIT ────────────────────────────
#
# The base build checkpoints its stages in a state directory and decides what it can
# skip by asking whether its outputs are still on disk.  With the image named after the
# commit, a commit was enough to make it look for a file no run had ever written -- so
# every commit invalidated the image checkpoint, and with it finalization, and the whole
# final stage ran again: a six-minute restore of the cached root, an hour or more of
# package churn, and a fresh 8 GB image, to produce bytes byte-identical to the ones
# beside it under a different name.  Four such images were found in one build VM.
#
# The base is a function of the Debian release and the architecture, and of nothing this
# repository commits, so that is what its name says.  It is built once and then reused,
# and the commit belongs on the thing that actually varies with the commit: the flashable
# image the J36 layer produces FROM the base, which darkos_image_name above still names.
# That one is a copy, so the base is never patched, never iterated on, and never has to
# be trusted to be pristine -- it is.
darkos_base_image_name() {
    printf 'MixOS_%s_%s_base.img\n' "$1" "$2"
}

# darkos_artifact_dir ROOT
#
# The host directory the finished images land in: MixOS-Artifacts, a sibling of the
# checkout.
#
# It used to be "${ROOT}-artifacts", so a checkout directory called dArkOS produced
# dArkOS-artifacts -- the output was named after whatever the working copy happened to
# be called rather than after the project, and a second clone under another name grew a
# second artifact tree beside it.  The name is a literal now.
#
# THE OLD DIRECTORY IS MOVED ACROSS, not left behind, and that is why this is a function
# and not just a changed default.  DARKOS_R36_BOOT_PAYLOAD points inside it, at
# Reference/BOOT: the R36S device tree, the u-boot panel variants, the dtb selector and
# the off-charging bitmaps -- a payload this pipeline has never produced and does not
# download.  A rename that orphaned it would still build, and the images would simply
# not boot; the only sign would be one warning in a very long log.  Moving is free on
# one filesystem and keeps every image already built alongside it.
#
# Drop the migration once no workstation has a *-artifacts directory left.
darkos_artifact_dir() {
    local root=$1 current legacy
    current="$(dirname -- "$root")/MixOS-Artifacts"
    legacy="${root}-artifacts"
    if [[ ! -d "$current" && -d "$legacy" ]]; then
        darkos_log "Moving ${legacy} to ${current}"
        mv -- "$legacy" "$current"
    fi
    printf '%s\n' "$current"
}

# darkos_report_stale_images DIR KEEP_NAME
#
# One image per commit means the artifact directory grows by another 8 GB every time a
# new commit is built, and the images are no longer compressed -- so it is worth saying
# out loud rather than discovering when the workstation runs out of disk.
#
# Nothing is deleted here.  Which older card the operator still needs to be able to
# flash is not this script's call to make, and an artifact directory that silently
# removes builds is worse than one that grows.
darkos_report_stale_images() {
    local dir=$1 keep=$2 img name n=0
    for img in "$dir"/MixOS_*.img; do
        [[ -f "$img" ]] || continue
        name="$(basename "$img")"
        if [[ "$name" == "$keep" ]]; then
            continue
        fi
        n=$((n + 1))
        darkos_log "  older image still here: $name ($(du -h "$img" | awk '{print $1}'))"
    done
    if (( n )); then
        darkos_warn "$n older image(s) in $dir; delete the ones you no longer flash"
    fi
    return 0
}

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

# darkos_vm_ensure_sshfs NAME
#
# `multipass mount' is sshfs, and the daemon finds sshfs by running
#
#     snap run multipass-sshfs.env
#
# over SSH AS THE DEFAULT USER.  When that probe fails for any reason at all,
# Multipass reports one thing:
#
#     Error enabling mount support in '<vm>'
#     Please install the 'multipass-sshfs' snap manually inside the instance.
#
# which is a misdiagnosis in the case that actually happens.  The snap is
# installed.  What breaks is the per-user systemd manager: an apt upgrade of
# systemd or a snapd refresh re-executes it, it comes back having lost the cgroup
# delegation for its own app.slice, and every later `snap run' as that user dies
# in the journal with
#
#     Couldn't move process N to requested cgroup
#       .../user@1000.service/app.slice/snap....scope: Input/output error
#     Failed to add PIDs to scope's control group: Permission denied
#
# `sudo snap run' keeps working the whole time, because as root the scope is
# created by the SYSTEM manager instead -- which is why every by-hand check an
# operator is likely to try comes back clean while the daemon still cannot mount.
# Restarting the user manager fixes it, and nothing in this build owns a process
# under it, so the restart is free.
#
# Probe the way the daemon does: as the default user, without sudo, and test what
# the daemon actually needs.  `multipass-sshfs.env' prints an environment block,
# out of which the daemon takes SNAP= and runs $SNAP/bin/sshfs; a zero exit alone
# does not promise that line is there, so match it.  The `timeout' is a backstop
# against a wedged snapd rather than a fix for anything seen here, and it sits
# inside the VM because a signal to `multipass exec' on the host does not reach
# the remote process.
#
# Note the redirect that is NOT here: see the /dev/null rule at the top of this
# file.  An earlier version of this probe ended in `>/dev/null 2>&1' on the host
# side and hung the build for as long as it was left running.
darkos_vm_sshfs_works() {
    multipass exec "$1" -- bash -lc \
        'timeout 15 snap run multipass-sshfs.env 2>/dev/null | grep -q "^SNAP="'
}

# Repair the two things that can genuinely be wrong, in that order: the second is
# a network round trip to the store and the first is not.  A VM still broken
# after both gets a warning naming the command to run by hand, and the mount
# below is left to fail on its own terms rather than being pre-empted here.
darkos_vm_ensure_sshfs() {
    local name=$1

    if darkos_vm_sshfs_works "$name"; then
        return 0
    fi

    darkos_log "Repairing sshfs mount support in $name"
    multipass exec "$name" -- bash -lc \
        'timeout 60 sudo systemctl restart "user@$(id -u).service" >/dev/null 2>&1 || true
         sleep 2' || true
    if darkos_vm_sshfs_works "$name"; then
        return 0
    fi

    multipass exec "$name" -- bash -lc \
        'timeout 300 sudo snap install multipass-sshfs >/dev/null 2>&1 || true' || true
    if darkos_vm_sshfs_works "$name"; then
        return 0
    fi

    darkos_warn "multipass-sshfs still will not run in $name, so the mounts below will fail"
    darkos_warn "see it for yourself with: multipass exec $name -- snap run multipass-sshfs.env"
    return 0
}

# darkos_vm_remount NAME HOST_PATH:VM_PATH [HOST_PATH:VM_PATH ...]
#
# Unmount first so that moving the checkout or the artifact directory on the host
# is handled rather than silently serving the old path.  Unmounting also clears a
# mount Multipass has recorded but failed to activate: it refuses a second
# `mount' at a path it believes is already mounted, so a single failed attempt
# would otherwise answer "already mounted" for the rest of the instance's life
# while the directory in the VM does not exist.
darkos_vm_remount() {
    local name=$1
    shift
    local pair host vm

    darkos_vm_ensure_sshfs "$name"

    for pair in "$@"; do
        host="${pair%%:*}"
        vm="${pair#*:}"
        multipass umount "$name:$vm" >/dev/null 2>&1 || true
    done
    # And the paths this project mounted before a rename.  Multipass remembers a mount
    # for the life of the instance, so one whose host directory has moved fails at every
    # VM start and leaves behind an empty directory in the VM that looks exactly like the
    # real one -- a build that wrote its images there would report success and hand over
    # nothing.  Unmounting what is not mounted is a no-op, so this costs one failed
    # command per refresh.  Drop it once no build VM predates the rename.
    for vm in /mnt/darkos-artifacts; do
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
    'MixOSBuild/'
    'MixOSBuild32/'
    'MixOSBuild_ccache/'
    'MixOSBuild_package_cache/'
    # The names these four had before the rename.  They are listed because this rsync
    # is --delete and the host has no such directories: an existing VM's ccache and
    # package cache would be deleted on the first sync after the rename, which costs a
    # re-debootstrap and a full rebuild.  The build migrates them across on its next
    # run (utils.sh, prepare.sh); these entries are what keep them alive until it does.
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
    # Nothing a workstation happens to have made is a build input.  build/ is where
    # a by-hand run of build-j36-ultra-dtb.sh puts its outputs, .DS_Store is Finder's
    # and __pycache__ is CPython's -- and this rsync is --delete, so carrying a macOS
    # .pyc into a Linux VM is worth nobody's second.
    'build/'
    '.DS_Store'
    '__pycache__/'
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

# darkos_vm_refuse_concurrent_build NAME
#
# The checkout sync is `rsync --delete` over the directory a build reads its own
# scripts from, so two wrappers running at once is not a race that produces a bad
# image -- it is one that rewrites the running build's source tree underneath it.
# The J36 wrapper used to guard this by grepping for `build_rg351mp.sh` and
# `make ... rg351mp`, neither of which is how anything is invoked any more, so the
# guard had quietly stopped guarding. Match the script that actually runs.
darkos_vm_refuse_concurrent_build() {
    local name=$1 running

    running="$(multipass exec "$name" -- bash -lc \
        "pgrep -af '[b]uild-in-vm\.sh' || true" 2>/dev/null)"
    [[ -z "$running" ]] && return 0

    darkos_warn "a build is already running in $name:"
    printf '%s\n' "$running" >&2
    darkos_die "refusing to rewrite the VM checkout underneath it; re-run when it finishes"
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
