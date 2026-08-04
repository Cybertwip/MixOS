#!/usr/bin/env bash
# Checkpointed RG351MP/R36 base builder for Ubuntu. This sources the existing
# dArkOS build stages but preserves completed partition/bootstrap/U-Boot/kernel
# work across retries.

set -o pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$ROOT" || exit 1

DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"
BUILD_ARMHF="${BUILD_ARMHF:-y}"
BUILD_JOBS="${BUILD_JOBS:-4}"
BUILD_BUNDLED_APPS="${BUILD_BUNDLED_APPS:-n}"
ENABLE_CACHE="${ENABLE_CACHE:-y}"
CHIPSET=rk3326
UNIT=rg351mp
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
    BUILD_PROFILE=full
    IMAGE_PREFIX=dArkOS_RG351MP
    FILESYSTEM=ArkOS_File_System.img
else
    BUILD_PROFILE=gui
    IMAGE_PREFIX=dArkOS_R36_ULTRA_GUI_BASE
    FILESYSTEM=ArkOS_R36_GUI_File_System.img
fi
export DEBIAN_CODE_NAME BUILD_ARMHF BUILD_JOBS BUILD_BUNDLED_APPS
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
STATE_DIR="$STATE_ROOT/${DEBIAN_CODE_NAME}-armhf-${BUILD_ARMHF}-profile-${BUILD_PROFILE}"
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

[[ "$BUILD_ARMHF" == y || "$BUILD_ARMHF" == n ]] || \
    fail "BUILD_ARMHF must be y or n"
[[ "$BUILD_BUNDLED_APPS" == y || "$BUILD_BUNDLED_APPS" == n ]] || \
    fail "BUILD_BUNDLED_APPS must be y or n"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || \
    fail "BUILD_JOBS must be a positive integer"

log "Configuration: Debian=${DEBIAN_CODE_NAME}, profile=${BUILD_PROFILE}, ARMHF=${BUILD_ARMHF}, jobs=${BUILD_JOBS}, cache=${ENABLE_CACHE}"

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

ensure_armhf_mounts() {
    [[ "$BUILD_ARMHF" == y && -d Arkbuild32 ]] || return 0
    sudo mkdir -p Arkbuild32/{dev/pts,proc,sys}
    mountpoint -q Arkbuild32/dev || sudo mount --bind /dev Arkbuild32/dev
    mountpoint -q Arkbuild32/dev/pts || \
        sudo mount -t devpts none Arkbuild32/dev/pts -o newinstance,ptmxmode=0666
    mountpoint -q Arkbuild32/proc || sudo mount --bind /proc Arkbuild32/proc
    mountpoint -q Arkbuild32/sys || sudo mount --bind /sys Arkbuild32/sys
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
        sudo tee Arkbuild32/etc/resolv.conf >/dev/null
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

# A GUI-only and a full-app build use different filesystem/image names.  Make
# sure a mount left by the other profile cannot contaminate this build.
clear_foreign_rootfs_mount

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
run_stage image image_setup.sh

if ! marked kernel; then
    clear_stale_boot_mount
    run_stage kernel build_kernel.sh
else
    ensure_boot_mount
fi
maybe_stop kernel

# A failed userspace phase can be safely retried.  The default GUI profile only
# builds the graphics/runtime prerequisites and EmulationStation.  The original
# complete emulator/application list remains available with
# BUILD_BUNDLED_APPS=y.
if ! marked userspace; then
    ensure_armhf_mounts
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

    # Full builds create the armhf build root while compiling RetroArch.  The
    # GUI-only profile has no 32-bit applications to trigger that path, so make
    # the requested compatibility runtime explicit without adding any apps.
    if [[ "$BUILD_BUNDLED_APPS" == n && "$BUILD_ARMHF" == y ]]; then
        component=component-armhf-support
        if marked "$component"; then
            log "Skipping completed 32-bit compatibility support"
        else
            printf '%s\n' "userspace:armhf-support" > "$CURRENT_STAGE"
            log "Building 32-bit compatibility libraries (no bundled apps)"
            setup_arkbuild32 || fail "32-bit compatibility setup failed"
            [[ -n "${extension:-}" ]] || fail "SDL2 extension was not detected for armhf support"
            [[ -e "Arkbuild/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension}" ]] || \
                fail "32-bit SDL2 compatibility library was not installed"
            printf '%s\n' "$extension" > "$STATE_DIR/sdl2-extension"
            mark "$component"
        fi
    fi
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
