#!/usr/bin/env bash
# Incremental J36 Ultra ARMv7 bring-up builder. Run inside Ubuntu, normally via
# ./build-j36-ultra.sh on macOS. It never invokes the RG351MP/R36 build target.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
WORK="${J36_WORK_DIR:-$HOME/j36-ultra-work}"
DRIVERS="${J36_DRIVERS_DIR:-$WORK/powerengine-drivers}"
EXPORT_DIR="${J36_EXPORT_DIR:-$WORK/export}"
KERNEL_URL="${J36_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
KERNEL_BRANCH="${J36_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_SRC="$WORK/linux"
KERNEL_OUT="$WORK/kernel-build"
BUSYBOX_URL="${J36_BUSYBOX_URL:-https://git.busybox.net/busybox}"
BUSYBOX_BRANCH="${J36_BUSYBOX_BRANCH:-1_36_stable}"
BUSYBOX_SRC="$WORK/busybox"
MODULE_SRC="$WORK/module-src"
DTB_OUT="$WORK/dtb"
INITROOT="$WORK/initramfs-root"
ARTIFACTS="$WORK/artifacts"
CACHE="$WORK/cache"

log() { printf '\n[build-j36-ultra] %s\n' "$*"; }
die() { printf '\n[build-j36-ultra] ERROR: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == Linux ]] || die "build-in-vm.sh must run on Linux"
[[ -d "$DRIVERS" ]] || die "MVII J36 Drivers not found: $DRIVERS"

mkdir -p "$WORK" "$CACHE" "$ARTIFACTS" "$EXPORT_DIR"

if [[ ! -f "$WORK/.deps-installed" ]]; then
    log "Installing the one-time ARMv7 kernel build dependencies"
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
        bc bison build-essential ccache cpio device-tree-compiler \
        flex gcc-arm-linux-gnueabihf git gzip libelf-dev libssl-dev \
        python3 rsync xz-utils
    touch "$WORK/.deps-installed"
fi

if [[ ! -d "$KERNEL_SRC/.git" ]]; then
    log "Cloning Linux $KERNEL_BRANCH once; later runs reuse this checkout"
    git clone --depth=1 --branch "$KERNEL_BRANCH" "$KERNEL_URL" "$KERNEL_SRC"
elif [[ "${J36_UPDATE_KERNEL:-0}" == 1 ]]; then
    log "Updating the persistent Linux checkout"
    git -C "$KERNEL_SRC" fetch --depth=1 origin "$KERNEL_BRANCH"
    git -C "$KERNEL_SRC" reset --hard FETCH_HEAD
fi

mkdir -p "$KERNEL_OUT"
if [[ ! -f "$KERNEL_OUT/.config" ]]; then
    log "Creating the initial MT6592 ARMv7 kernel configuration"
    make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
        CROSS_COMPILE=arm-linux-gnueabihf- multi_v7_defconfig
fi

CONFIG="$KERNEL_OUT/.config"
SC="$KERNEL_SRC/scripts/config"
config_y() { "$SC" --file "$CONFIG" -e "$1"; }
config_n() { "$SC" --file "$CONFIG" -d "$1" || true; }
config_v() { "$SC" --file "$CONFIG" --set-val "$1" "$2"; }
config_s() { "$SC" --file "$CONFIG" --set-str "$1" "$2"; }

# Core ARMv7/MT6592 boot and stock-LK ATAG + appended-DTB handoff.
for symbol in \
    ARCH_MULTI_V7 ARCH_MEDIATEK MACH_MT6592 OF ATAGS ARM_APPENDED_DTB \
    ARM_ATAG_DTB_COMPAT ARM_ATAG_DTB_COMPAT_CMDLINE_FROM_BOOTLOADER \
    AEABI VFP NEON SMP HIGHMEM AUTO_ZRELADDR BLK_DEV_INITRD RD_GZIP \
    DEVTMPFS DEVTMPFS_MOUNT TMPFS PROC_FS SYSFS UNIX MODULES MODULE_UNLOAD \
    INPUT INPUT_EVDEV INPUT_JOYDEV VT VT_CONSOLE FRAMEBUFFER_CONSOLE \
    FB FB_SIMPLE SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_8250_MT6577 \
    SERIAL_OF_PLATFORM EARLY_PRINTK; do
    config_y "$symbol"
done
config_v SERIAL_8250_NR_UARTS 4
config_v SERIAL_8250_RUNTIME_UARTS 4
config_s LOCALVERSION "-j36"

# multi_v7_defconfig enables dozens of unrelated boards. Prune user-selectable
# ARCH/MACH/SOC targets so the zImage plus initramfs fits the fixed 9 MiB slot.
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        ARCH_MULTIPLATFORM|ARCH_MULTI_V7|ARCH_MULTI_V6_V7|ARCH_MEDIATEK|MACH_MT6592)
            ;;
        ARCH_*|MACH_*|SOC_*)
            config_n "$symbol"
            ;;
    esac
done < <(grep -E '^CONFIG_(ARCH|MACH|SOC)_[A-Z0-9_]+=y$' "$CONFIG")
for symbol in ARCH_MULTIPLATFORM ARCH_MULTI_V7 ARCH_MULTI_V6_V7 ARCH_MEDIATEK MACH_MT6592; do
    config_y "$symbol"
done

# Keep this first-stage image below the fixed 9 MiB BOOTIMG partition. Storage,
# networking, audio and native DRM/DSI are added only after the serial/fb/input
# bring-up is proven.
for symbol in \
    DRM SOUND SND MEDIA_SUPPORT NET WIRELESS WLAN BT USB_SUPPORT SCSI ATA \
    DEBUG_INFO DEBUG_KERNEL KALLSYMS LOGO; do
    config_n "$symbol"
done

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig

for required in MACH_MT6592 ARM_APPENDED_DTB ARM_ATAG_DTB_COMPAT \
                FB_SIMPLE SERIAL_8250_MT6577 BLK_DEV_INITRD MODULES; do
    grep -q "^CONFIG_${required}=y$" "$CONFIG" || \
        die "required kernel option CONFIG_${required}=y was not selected"
done

log "Building the incremental ARMv7 kernel and its symbol table"
export CCACHE_DIR="${CCACHE_DIR:-$ROOT/Arkbuild_ccache}"
mkdir -p "$CCACHE_DIR"
if [[ -d /usr/lib/ccache ]]; then export PATH="/usr/lib/ccache:$PATH"; fi
# `modules' is not optional here even though this configuration selects almost no
# in-tree modules.  It is the target that runs modpost over vmlinux.o and writes
# $KERNEL_OUT/Module.symvers, and the out-of-tree J36 input adapter below is
# resolved against that file.  Building only zImage leaves it absent, and modpost
# then reports every core symbol the adapter uses -- __platform_driver_register,
# devm_kmalloc, memset, __aeabi_unwind_cpp_pr0 -- as "undefined!".
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)" zImage modules

# Kbuild runs modpost in two passes and the first one writes vmlinux.symvers: the
# vmlinux exports alone, which is exactly what an external module needs.  If the
# second pass produced nothing because this tree has no modules of its own, that
# first-pass output is still the right symbol table to link against.
if [[ ! -s "$KERNEL_OUT/Module.symvers" && -s "$KERNEL_OUT/vmlinux.symvers" ]]; then
    cp "$KERNEL_OUT/vmlinux.symvers" "$KERNEL_OUT/Module.symvers"
fi
[[ -s "$KERNEL_OUT/Module.symvers" ]] || \
    die "the kernel build produced no Module.symvers; an out-of-tree module cannot be resolved against it"

# ── The one mistake this layer exists to prevent ──────────────────────────────
#
# The other half of this repository builds an aarch64 kernel for the RK3326, the
# two boot chains now share an SD card, and an MT6592 is ARMv7.  The MVII LK
# refuses an arm64 payload at the last moment by reading the arm64 boot magic at
# offset 0x38 -- see sd_kernel_is_armv7() in mvii_lk_main.c.  Run that same test
# here, on the build machine, where "no" is a build failure rather than a board
# that silently fell back to the eMMC.
blob_le32() { od -An -t x4 -j "$1" -N 4 "$2" | tr -d ' \n'; }

verify_arm_elf() {
    local file="$1" what="$2" header
    header="$(readelf -h "$file")" || die "could not read the ELF header of $what"
    grep -q 'Class:.*ELF32' <<<"$header" || die "$what is not a 32-bit ELF object"
    grep -q 'Machine:.*ARM' <<<"$header" || die "$what is not an ARM ELF object"
}

# The LK's byte tests, on the raw binary the LK will actually read.
verify_armv7_kernel() {
    local file="$1" what="$2" size
    size="$(stat -c %s "$file")"
    (( size >= 64 )) || die "$what is too short to be a kernel"
    [[ "$(blob_le32 0x38 "$file")" != 644d5241 ]] || \
        die "$what carries the arm64 boot magic at 0x38; this SoC is ARMv7"
    [[ "$(blob_le32 0x24 "$file")" == 016f2818 ]] || \
        die "$what carries no zImage magic at 0x24"
}

# The LK's own load windows: kernel 24 MiB at 0x80008000, device tree 256 KiB at
# 0x83000000, initramfs 96 MiB at 0x84000000.  A payload that does not fit is
# rejected by sd_read_into() at boot, so reject it here instead.
fits_in() {
    local file="$1" limit="$2" what="$3" size
    size="$(stat -c %s "$file")"
    (( size <= limit )) || \
        die "$what is $size bytes and the LK load window is $limit bytes"
}

ZIMAGE="$KERNEL_OUT/arch/arm/boot/zImage"
[[ -s "$ZIMAGE" ]] || die "zImage was not produced"
verify_arm_elf "$KERNEL_OUT/vmlinux" "the kernel"
verify_armv7_kernel "$ZIMAGE" "the zImage"
fits_in "$ZIMAGE" $((0x01800000)) "the zImage"
log "Verified a 32-bit ARMv7 zImage: $(stat -c %s "$ZIMAGE") bytes"

log "Regenerating the J36 DTB from the current PowerEngine Drivers"
J36_DRIVERS_DIR="$DRIVERS" J36_DTB_OUT_DIR="$DTB_OUT" \
    "$ROOT/build-j36-ultra-dtb.sh"

log "Building only the J36 input adapter module"
mkdir -p "$MODULE_SRC"
rsync -a --delete "$ROOT/device/j36-ultra/linux/" "$MODULE_SRC/"
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- M="$MODULE_SRC" -j"$(nproc)" modules
KERNEL_RELEASE="$(make -s -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- kernelrelease)"
MODULE="$MODULE_SRC/j36_mt6592_input.ko"
[[ -s "$MODULE" ]] || die "input module was not produced"
verify_arm_elf "$MODULE" "the input module"
fits_in "$DTB_OUT/mt6592-j36-ultra.dtb" $((0x00040000)) "the device tree"

if [[ ! -d "$BUSYBOX_SRC/.git" ]]; then
    log "Cloning BusyBox $BUSYBOX_BRANCH once for the ARM bring-up initramfs"
    git clone --depth=1 --branch "$BUSYBOX_BRANCH" "$BUSYBOX_URL" "$BUSYBOX_SRC"
fi
if [[ ! -x "$BUSYBOX_SRC/busybox" || "${J36_REBUILD_BUSYBOX:-0}" == 1 ]]; then
    log "Building the static ARMv7 BusyBox once"
    make -C "$BUSYBOX_SRC" distclean
    make -C "$BUSYBOX_SRC" defconfig
    sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' "$BUSYBOX_SRC/.config"
    yes '' | make -C "$BUSYBOX_SRC" oldconfig >/dev/null || true
    make -C "$BUSYBOX_SRC" CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)"
fi
BUSYBOX="$BUSYBOX_SRC/busybox"
[[ -x "$BUSYBOX" ]] || die "static ARM BusyBox was not produced"

rm -rf "$INITROOT"
mkdir -p "$INITROOT"/{bin,dev,etc,lib/modules/$KERNEL_RELEASE/extra,proc,root,sbin,sys,tmp}
cp "$BUSYBOX" "$INITROOT/bin/busybox"
chmod 0755 "$INITROOT/bin/busybox"
for applet in sh mount mkdir mknod cat echo sleep dmesg insmod ls hexdump \
              setsid cttyhack poweroff reboot uname; do
    ln -sf busybox "$INITROOT/bin/$applet"
done
cp "$MODULE" "$INITROOT/lib/modules/$KERNEL_RELEASE/extra/"

cat > "$INITROOT/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

echo
echo "J36 Ultra ARMv7 bring-up initramfs"
echo "Display must remain on the stock-LK framebuffer; native DSI is disabled."
insmod /lib/modules/*/extra/j36_mt6592_input.ko || echo "input module load failed"
echo "Framebuffer devices:"
ls -l /dev/fb* 2>/dev/null || true
echo "Input devices:"
ls -l /dev/input 2>/dev/null || true
echo "Use: hexdump -C /dev/input/event0"
echo
exec setsid cttyhack sh
INIT
chmod 0755 "$INITROOT/init"

log "Packing the bring-up initramfs"
(
    cd "$INITROOT"
    find . -print0 | cpio --null -o --format=newc --owner=0:0 2>/dev/null | gzip -9
) > "$ARTIFACTS/initramfs-j36-ultra.cpio.gz"

fits_in "$ARTIFACTS/initramfs-j36-ultra.cpio.gz" $((0x06000000)) "the initramfs"

# TWO KERNEL PAYLOADS, AND THEY ARE NOT INTERCHANGEABLE.
#
# The eMMC BOOTIMG path enters the kernel the way the stock LK does, with an ATAG
# list in r2 and no device tree anywhere, so that payload carries its tree
# appended to the zImage and CONFIG_ARM_ATAG_DTB_COMPAT folds the ATAGs into it.
#
# The SD hand-off path passes the tree itself in r2 and patches /chosen first --
# bootargs, linux,initrd-start and linux,initrd-end.  An appended tree would
# silently win that argument: arch/arm/boot/compressed/head.S takes the appended
# DTB whenever its magic is present and only consults r2 to fold ATAGs in, so a
# DTB in r2 is discarded, and with it the initramfs range.  The kernel would boot
# and then fail to find /init.  So the SD payload is the PLAIN zImage with the
# tree beside it as a separate file, which is also what the LK's built-in
# defaults and the boot.conf below name.
cat "$ZIMAGE" "$DTB_OUT/mt6592-j36-ultra.dtb" > "$ARTIFACTS/zImage-j36-ultra"
cp "$ZIMAGE" "$ARTIFACTS/zImage"
cp "$DTB_OUT/mt6592-j36-ultra.dtb" "$ARTIFACTS/"
cp "$MODULE" "$ARTIFACTS/"
cp "$CONFIG" "$ARTIFACTS/kernel.config"

log "Creating the stock-LK-compatible BOOTIMG payload"
python3 "$ROOT/device/j36-ultra/create_boot_image.py" \
    --kernel "$ARTIFACTS/zImage-j36-ultra" \
    --ramdisk "$ARTIFACTS/initramfs-j36-ultra.cpio.gz" \
    --output "$ARTIFACTS/boot.img"

# ── The SD BOOT payload ───────────────────────────────────────────────────────
#
# Copy this tree onto the FAT partition labelled BOOT and the MVII LK boots the
# card instead of the eMMC.  /mvii/boot.conf is written because a dArkOS card
# already carries a boot.ini, and that boot.ini names the RK3326's arm64 `Image`
# and an rk3326 device tree.  The LK parses boot.ini first and boot.conf second
# precisely so this file gets the last word; without it the LK would load the
# arm64 kernel, refuse it at the magic check, and fall back to the eMMC.
#
# Load addresses are deliberately absent.  They are the LK's business -- it knows
# this SoC's DRAM map and the address of the framebuffer the DTB hands to
# simple-framebuffer -- and boot.conf can only get them wrong.
log "Staging the SD card BOOT payload"
SDBOOT="$ARTIFACTS/sd-boot"
rm -rf "$SDBOOT"
mkdir -p "$SDBOOT/mvii"
cp "$ZIMAGE" "$SDBOOT/zImage"
cp "$DTB_OUT/mt6592-j36-ultra.dtb" "$SDBOOT/"
cp "$ARTIFACTS/initramfs-j36-ultra.cpio.gz" "$SDBOOT/initrd.img"

# This kernel has no MMC, block, SCSI or network support: the bring-up profile
# turns them off to fit the 9 MiB BOOTIMG slot.  There is therefore no root=
# here, and there must not be -- a root= this kernel cannot honour is a panic
# instead of a shell.  It boots to the initramfs.  When native MSDC lands, add
# `root=LABEL=ROOTFS rootwait rw` and drop rdinit=.
cat > "$SDBOOT/mvii/boot.conf" <<'CONF'
# MVII LK SD hand-off, J36 Ultra (MT6592, ARMv7).
#
# Read after the card's own boot.ini, so these override it.  A dArkOS boot.ini
# names the RK3326 arm64 kernel, which this SoC cannot execute.
kernel=zImage
dtb=mt6592-j36-ultra.dtb
initrd=initrd.img

# No root= : this bring-up kernel has no storage drivers and boots its initramfs.
bootargs=console=tty0 console=ttyS0,115200n8 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init
CONF

# The LK reads boot.conf into a fixed 2 KiB buffer and a longer file is silently
# truncated mid-line.
(( $(stat -c %s "$SDBOOT/mvii/boot.conf") <= 2048 )) || \
    die "boot.conf exceeds the LK's 2048-byte read buffer"
verify_armv7_kernel "$SDBOOT/zImage" "the SD payload kernel"

cat > "$SDBOOT/README.txt" <<'README'
J36 Ultra (MT6592, ARMv7) SD card BOOT payload.

Copy the contents of this directory into the root of the FAT partition labelled
BOOT.  Existing files are not disturbed: an R36S dArkOS card keeps its Image,
uInitrd, rk3326 device trees and boot.ini, and mvii/boot.conf points the MVII LK
at the ARMv7 payload instead.

  zImage                    plain 32-bit ARM kernel, no appended device tree
  mt6592-j36-ultra.dtb      the tree the LK loads separately and patches
  initrd.img                bring-up initramfs (busybox + the input module)
  mvii/boot.conf            filenames and command line for the MVII LK

The R36S kernel on the same card is arm64 and stays there for the R36S.  The
armhf Debian rootfs is shared; this kernel cannot mount it yet, because storage
drivers are not in the bring-up profile.
README

(
    cd "$ARTIFACTS"
    sha256sum boot.img zImage zImage-j36-ultra mt6592-j36-ultra.dtb \
        j36_mt6592_input.ko initramfs-j36-ultra.cpio.gz \
        sd-boot/zImage sd-boot/mvii/boot.conf > SHA256SUMS
    {
        echo "kernel_branch=$KERNEL_BRANCH"
        echo "kernel_release=$KERNEL_RELEASE"
        echo "kernel_arch=arm (ARMv7, 32-bit)"
        echo "zimage_size=$(stat -c %s zImage)"
        echo "dtb_sha256=$(sha256sum mt6592-j36-ultra.dtb | awk '{print $1}')"
        echo "bootimg_size=$(stat -c %s boot.img)"
        echo "bootimg_kernel=zImage-j36-ultra (device tree appended, ATAG path)"
        echo "sd_kernel=sd-boot/zImage (plain, LK passes the tree in r2)"
        echo "display=stock-lk-simple-framebuffer"
        echo "native_dsi=disabled"
        echo "input_adapter=j36_mt6592_input.ko"
    } > manifest.txt
)

mkdir -p "$EXPORT_DIR"
rsync -a --delete "$ARTIFACTS/" "$EXPORT_DIR/"

log "J36 Ultra incremental bring-up artifacts are ready"
printf '  %s\n' \
    "$EXPORT_DIR/boot.img" \
    "$EXPORT_DIR/mt6592-j36-ultra.dtb" \
    "$EXPORT_DIR/j36_mt6592_input.ko" \
    "$EXPORT_DIR/sd-boot/ (copy onto the BOOT partition)" \
    "$EXPORT_DIR/manifest.txt"
