#!/usr/bin/env bash
# Checkpointed RG351MP/R36 base builder for Ubuntu.  The default profile builds
# a native armhf Debian userspace and EmulationStation on the existing arm64
# RK3326 kernel/boot chain, while preserving completed stages across retries.

set -o pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$ROOT" || exit 1

DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"
USERSPACE_ARCH="${USERSPACE_ARCH:-armhf}"
# R36 builds are single-userspace-architecture images.  Keep the legacy
# multiarch switch off; USERSPACE_ARCH selects the native rootfs instead.
BUILD_ARMHF=n
BUILD_JOBS="${BUILD_JOBS:-4}"
BUILD_BUNDLED_APPS="${BUILD_BUNDLED_APPS:-n}"
ENABLE_CACHE="${ENABLE_CACHE:-y}"
CHIPSET=rk3326
UNIT=rg351mp
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
    BUILD_PROFILE=full
    IMAGE_PREFIX="dArkOS_RG351MP_FULL_${USERSPACE_ARCH}"
else
    BUILD_PROFILE=gui
    IMAGE_PREFIX="dArkOS_R36_ULTRA_GUI_BASE_${USERSPACE_ARCH}"
fi
FILESYSTEM="ArkOS_R36_${DEBIAN_CODE_NAME}_${USERSPACE_ARCH}_${BUILD_PROFILE}_File_System.img"
export DEBIAN_CODE_NAME USERSPACE_ARCH BUILD_ARMHF BUILD_JOBS BUILD_BUNDLED_APPS
export ENABLE_CACHE CHIPSET UNIT BUILD_PROFILE FILESYSTEM

# Keep direct make, CMake, Meson/Ninja, and helper scripts on the same
# configurable parallelism limit.  Exporting nproc also covers child bash
# scripts that still derive their job count with $(nproc).
export MAKEFLAGS="-j${BUILD_JOBS}"
export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
export NINJAFLAGS="-j${BUILD_JOBS}"
nproc() { printf '%s\n' "$BUILD_JOBS"; }
export -f nproc

STATE_ROOT="${DARKOS_R36_STATE_DIR:-$HOME/darkos-r36-state}"
STATE_DIR="$STATE_ROOT/${DEBIAN_CODE_NAME}-userspace-${USERSPACE_ARCH}-profile-${BUILD_PROFILE}-v3"
mkdir -p "$STATE_DIR"
LOG="$STATE_DIR/resume.log"
CURRENT_STAGE="$STATE_DIR/current-stage"
exec > >(tee -a "$LOG") 2>&1

log() { printf '\n[r36-resume] %s\n' "$*"; }
fail() { printf '\n[r36-resume] ERROR: %s\n' "$*" >&2; exit 1; }
marked() { [[ -f "$STATE_DIR/$1.done" ]]; }
mark() { touch "$STATE_DIR/$1.done"; }
maybe_stop() {
    if [[ "${DARKOS_R36_STOP_AFTER:-}" == "$1" ]]; then
        log "Stopping after requested checkpoint: $1"
        rm -f "$CURRENT_STAGE"
        exit 0
    fi
}

[[ "$USERSPACE_ARCH" == armhf || "$USERSPACE_ARCH" == arm64 ]] || \
    fail "USERSPACE_ARCH must be armhf or arm64"
[[ "$BUILD_BUNDLED_APPS" == y || "$BUILD_BUNDLED_APPS" == n ]] || \
    fail "BUILD_BUNDLED_APPS must be y or n"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || \
    fail "BUILD_JOBS must be a positive integer"

if [[ "$USERSPACE_ARCH" == armhf && "$BUILD_BUNDLED_APPS" == y ]]; then
    fail "The armhf-only R36 profile supports Debian + EmulationStation only; set BUILD_BUNDLED_APPS=n"
fi

log "Configuration: Debian=${DEBIAN_CODE_NAME}, userspace=${USERSPACE_ARCH}, profile=${BUILD_PROFILE}, jobs=${BUILD_JOBS}, cache=${ENABLE_CACHE}"

if [[ "${DARKOS_R36_RESET:-0}" == 1 ]]; then
    fail "DARKOS_R36_RESET requires a fresh build; use 'make clean' manually before retrying"
fi

# Load shared helpers, then put the Linaro bin directory on PATH as an ABSOLUTE
# path. utils.sh's relative entry stops working after build_kernel.sh cd's into
# rg351, which caused the original aarch64-linux-gnu-gcc not found failure.
source ./utils.sh
TOOLCHAIN="$ROOT/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu"
[[ -x "$TOOLCHAIN/bin/aarch64-linux-gnu-gcc" ]] || \
    fail "Linaro AArch64 compiler is missing from $TOOLCHAIN"
export PATH="$TOOLCHAIN/bin:$PATH"
"$TOOLCHAIN/bin/aarch64-linux-gnu-gcc" --version | head -n 1

# Recover the date of an existing partial image, even if a retry happens after
# midnight. A new build records today's date once.
if [[ -s "$STATE_DIR/build-date" ]]; then
    BUILD_DATE="$(cat "$STATE_DIR/build-date")"
else
    existing_image="$(ls -1t "${IMAGE_PREFIX}_${DEBIAN_CODE_NAME}_"*.img 2>/dev/null | head -n 1 || true)"
    if [[ -n "$existing_image" && -f "$FILESYSTEM" ]]; then
        BUILD_DATE="${existing_image##*_}"
        BUILD_DATE="${BUILD_DATE%.img}"
    else
        BUILD_DATE="$(date +%m%d%Y)"
    fi
    printf '%s\n' "$BUILD_DATE" > "$STATE_DIR/build-date"
fi
export BUILD_DATE

# setup_partition.sh values, reproduced without recreating its 52 GB filesystem.
ROOT_FILESYSTEM_FORMAT=btrfs
ROOT_FILESYSTEM_FORMAT_PARAMETERS="-O ^free-space-tree -f -L ROOTFS"
ROOT_FILESYSTEM_MOUNT_OPTIONS="defaults,noatime,compress=zlib:1"
SYSTEM_SIZE=100
STORAGE_SIZE=7500
ROM_PART_SIZE=300
BUILD_SIZE=52000
SYSTEM_PART_START=32768
SYSTEM_PART_END=$(( SYSTEM_PART_START + (SYSTEM_SIZE * 1024 * 1024 / 512) - 1 ))
STORAGE_PART_START=$(( SYSTEM_PART_END + 1 ))
STORAGE_PART_END=$(( STORAGE_PART_START + (STORAGE_SIZE * 1024 * 1024 / 512) - 1 ))
ROM_PART_START=$(( STORAGE_PART_END + 1 ))
ROM_PART_END=$(( ROM_PART_START + (ROM_PART_SIZE * 1024 * 1024 / 512) - 1 ))
DISK_START_PADDING=$(( (SYSTEM_PART_START + 2048 - 1) / 2048 ))
DISK_SIZE=$(( DISK_START_PADDING + SYSTEM_SIZE + STORAGE_SIZE + ROM_PART_SIZE + 1 ))
DISK="${IMAGE_PREFIX}_${DEBIAN_CODE_NAME}_${BUILD_DATE}.img"
export ROOT_FILESYSTEM_FORMAT ROOT_FILESYSTEM_FORMAT_PARAMETERS
export ROOT_FILESYSTEM_MOUNT_OPTIONS SYSTEM_PART_START SYSTEM_PART_END
export STORAGE_PART_START STORAGE_PART_END ROM_PART_START ROM_PART_END
export DISK_SIZE FILESYSTEM DISK

KERNEL_SRC=rg351
DEF_CONFIG=rg351p_tweaked_defconfig
SCREEN_ROTATION=0
KERNEL_DTB=rk3326-rg351mp-linux.dtb
mountpoint=mnt/boot
export KERNEL_SRC DEF_CONFIG SCREEN_ROTATION KERNEL_DTB mountpoint

ensure_rootfs_mounts() {
    [[ -f "$FILESYSTEM" ]] || return 0
    mkdir -p Arkbuild
    if ! mountpoint -q Arkbuild; then
        log "Remounting the preserved Btrfs build root"
        sudo mount -t btrfs -o "$ROOT_FILESYSTEM_MOUNT_OPTIONS",loop \
            "$FILESYSTEM" Arkbuild
    fi
    sudo mkdir -p Arkbuild/{dev/pts,proc,sys}
    mountpoint -q Arkbuild/dev || sudo mount --bind /dev Arkbuild/dev
    mountpoint -q Arkbuild/dev/pts || \
        sudo mount -t devpts none Arkbuild/dev/pts -o newinstance,ptmxmode=0666
    mountpoint -q Arkbuild/proc || sudo mount --bind /proc Arkbuild/proc
    mountpoint -q Arkbuild/sys || sudo mount --bind /sys Arkbuild/sys
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
        sudo tee Arkbuild/etc/resolv.conf >/dev/null
}

ensure_ccache_mount() {
    sudo mkdir -p Arkbuild/home/ark/Arkbuild_ccache
    if ! mountpoint -q Arkbuild/home/ark/Arkbuild_ccache; then
        sudo mount --bind "$ROOT/Arkbuild_ccache" Arkbuild/home/ark/Arkbuild_ccache
    fi
}

ensure_boot_mount() {
    mkdir -p "$mountpoint"
    if ! mountpoint -q "$mountpoint"; then
        local offset size loop
        offset=$((SYSTEM_PART_START * 512))
        size=$(( (SYSTEM_PART_END - SYSTEM_PART_START + 1) * 512 ))
        loop="$(sudo losetup --find --show --offset "$offset" --sizelimit "$size" "$DISK")"
        sudo mount "$loop" "$mountpoint"
    fi
}

clear_stale_boot_mount() {
    if mountpoint -q "$mountpoint"; then
        local loop
        loop="$(findmnt -n -o SOURCE "$mountpoint" || true)"
        sudo umount -l "$mountpoint"
        [[ "$loop" == /dev/loop* ]] && sudo losetup -d "$loop" || true
    fi
}

clear_foreign_rootfs_mount() {
    local mounted_source
    mountpoint -q Arkbuild || return 0
    mounted_source="$(findmnt -n -o SOURCE Arkbuild || true)"
    if ! sudo losetup -j "$ROOT/$FILESYSTEM" 2>/dev/null | \
        cut -d: -f1 | grep -Fxq "$mounted_source"; then
        log "Unmounting root filesystem left by a different R36 build profile"
        remove_arkbuild
    fi
}

clear_legacy_multiarch_root() {
    if [[ -d Arkbuild32 ]]; then
        log "Removing legacy secondary armhf build root; this profile is single-architecture"
        remove_arkbuild32
    fi
}

run_stage() {
    local name="$1" script="$2" rc
    if marked "$name"; then
        log "Skipping completed stage: $name"
        return 0
    fi
    printf '%s\n' "$name" > "$CURRENT_STAGE"
    log "Running stage: $name ($script)"
    source "./$script"
    rc=$?
    if (( rc != 0 )); then
        fail "stage $name returned $rc"
    fi
    mark "$name"
}

verify_native_userspace() {
    local actual foreign
    actual="$(sudo chroot Arkbuild dpkg --print-architecture)" || \
        fail "Could not query the Debian userspace architecture"
    [[ "$actual" == "$USERSPACE_ARCH" ]] || \
        fail "Expected ${USERSPACE_ARCH} userspace, found ${actual}"
    foreign="$(sudo chroot Arkbuild dpkg --print-foreign-architectures)" || \
        fail "Could not query foreign Debian architectures"
    [[ -z "$foreign" ]] || \
        fail "Single-architecture build unexpectedly contains foreign architectures: ${foreign}"
    log "Verified native ${actual} Debian userspace with no foreign architecture"
}

verify_gui_architecture() {
    local binary header
    binary="Arkbuild/usr/bin/emulationstation/emulationstation"
    [[ -x "$binary" ]] || fail "EmulationStation GUI binary is missing"
    header="$(readelf -h "$binary")" || fail "Could not inspect the EmulationStation GUI binary"
    if [[ "$USERSPACE_ARCH" == armhf ]]; then
        grep -q 'Class:.*ELF32' <<<"$header" && grep -q 'Machine:.*ARM' <<<"$header" || \
            fail "EmulationStation is not a 32-bit ARM binary"
    else
        grep -q 'Class:.*ELF64' <<<"$header" && grep -q 'Machine:.*AArch64' <<<"$header" || \
            fail "EmulationStation is not a 64-bit AArch64 binary"
    fi
    log "Verified ${USERSPACE_ARCH} EmulationStation GUI binary"
}

# A GUI-only and a full-app build use different filesystem/image names.  Make
# sure a mount left by the other profile cannot contaminate this build.
clear_foreign_rootfs_mount
clear_legacy_multiarch_root

# Infer checkpoints made by the original monolithic build that failed before
# this resume runner existed.
if [[ -f "$FILESYSTEM" ]]; then
    ensure_rootfs_mounts
    mark partition
fi
if [[ -f Arkbuild/etc/debian_version && -d Arkbuild/home/ark ]]; then
    mark bootstrap
fi
if [[ -s "$DISK" && $(stat -c %s "$DISK") -ge $((DISK_SIZE * 1024 * 1024)) ]]; then
    mark image
fi
if [[ -s "$mountpoint/Image" && -s "$mountpoint/$KERNEL_DTB" && -s "$mountpoint/uInitrd" ]]; then
    mark kernel
fi

# If a complete split archive already exists, verify and stop immediately.
if [[ -f "${DISK}.7z.001" ]]; then
    log "Existing completed archive found; verifying it"
    7z t "${DISK}.7z.001"
    mark complete
    printf '%s\n' "$DISK" > "$STATE_DIR/latest-image"
    exit 0
fi

run_stage prepare prepare.sh

if ! marked partition; then
    run_stage partition setup_partition.sh
else
    ensure_rootfs_mounts
fi
run_stage bootstrap bootstrap_rootfs.sh
verify_native_userspace
run_stage image image_setup.sh

if ! marked kernel; then
    clear_stale_boot_mount
    run_stage kernel build_kernel.sh
else
    ensure_boot_mount
fi
maybe_stop kernel

# A failed userspace phase can be safely retried.  The default GUI profile only
# builds the native graphics/runtime prerequisites and EmulationStation.  The
# original complete emulator/application list remains available only for the
# arm64 userspace profile with BUILD_BUNDLED_APPS=y.
if ! marked userspace; then
    if marked component-build_deps; then
        ensure_ccache_mount
    fi
    if ! marked component-build_deps && mountpoint -q Arkbuild/home/ark/Arkbuild_ccache; then
        sudo umount -l Arkbuild/home/ark/Arkbuild_ccache || true
    fi
    if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
        userspace_scripts=(
            build_deps.sh
            build_sdl2.sh
            build_ppssppsa.sh
            build_ppsspp-2021sa.sh
            build_duckstationsa.sh
            build_mupen64plussa.sh
            build_gzdoom.sh
            build_lzdoom.sh
            build_retroarch.sh
            build_retrorun.sh
            build_yabasanshirosa.sh
            build_mednafen.sh
            build_ecwolfsa.sh
            build_hypseus-singe.sh
            build_openbor.sh
            build_solarus.sh
            build_scummvmsa.sh
            build_fake08.sh
            build_xroar.sh
            build_mvem.sh
            build_bigpemu.sh
            build_ogage.sh
            build_ogacontrols.sh
            build_351files.sh
            build_filemanager.sh
            build_filebrowser.sh
            build_gptokeyb.sh
            build_image-viewer.sh
            build_emulationstation.sh
            build_linapple.sh
            build_applewinsa.sh
            build_piemu.sh
            build_ti99sim.sh
            build_gametank.sh
            build_openmsxsa.sh
            build_flycastsa.sh
            build_sdljoytest.sh
            build_controllertester.sh
            build_batteryplus.sh
            build_drastic.sh
        )
    else
        userspace_scripts=(
            build_deps.sh
            build_sdl2.sh
            build_emulationstation.sh
        )
    fi
    if [[ -s "$STATE_DIR/sdl2-extension" ]]; then
        extension="$(cat "$STATE_DIR/sdl2-extension")"
    fi
    for script in "${userspace_scripts[@]}"; do
        component="component-${script%.sh}"
        if marked "$component"; then
            log "Skipping completed userspace component: $script"
            continue
        fi
        printf '%s\n' "userspace:$script" > "$CURRENT_STAGE"
        log "Running userspace component: $script"
        source "./$script" || fail "$script failed"
        if [[ "$script" == build_sdl2.sh ]]; then
            printf '%s\n' "${extension:-}" > "$STATE_DIR/sdl2-extension"
        fi
        mark "$component"
    done

    mark userspace
else
    ensure_rootfs_mounts
    ensure_boot_mount
    if [[ -s "$STATE_DIR/sdl2-extension" ]]; then
        extension="$(cat "$STATE_DIR/sdl2-extension")"
    else
        extension="$(grep -oP '(?<=extension=").*?(?=")' \
            Arkbuild/home/ark/${CHIPSET}_core_builds/scripts/sdl2.sh 2>/dev/null || true)"
    fi
fi
verify_gui_architecture

# Final image assembly is kept as one checkpoint because these scripts shrink
# and unmount the root filesystem. If it fails, the log identifies the exact
# final script rather than silently starting over.
if ! marked finalization; then
    final_scripts=(
        finishing_touches.sh
        cleanup_filesystem.sh
        write_rootfs.sh
        clean_mounts.sh
        create_image.sh
    )
    for script in "${final_scripts[@]}"; do
        printf '%s\n' "final:$script" > "$CURRENT_STAGE"
        log "Running final image stage: $script"
        source "./$script" || fail "$script failed"
    done
    mark finalization
fi

[[ -f "${DISK}.7z.001" ]] || fail "split archive was not produced: ${DISK}.7z.001"
7z t "${DISK}.7z.001"
mark complete
printf '%s\n' "$DISK" > "$STATE_DIR/latest-image"
rm -f "$CURRENT_STAGE"
log "R36/RG351MP build completed successfully: $DISK"
