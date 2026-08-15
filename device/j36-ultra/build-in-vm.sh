#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/j36-ultra/LICENSE for the full text and for what it does not
# cover.
# Incremental J36 Ultra ARMv7 bring-up builder. Run inside Ubuntu, normally via
# ./build-j36-ultra.sh on macOS. It never invokes the RG351MP/R36 build target.

set -Eeuo pipefail

# Every python helper here is run as a script, which writes no bytecode -- but this
# script is also runnable straight out of a checkout on a Linux box, and one stray
# `import' would then leave a __pycache__ in device/j36-ultra.  Build products belong
# in $WORK, not in the tree.
export PYTHONDONTWRITEBYTECODE=1

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
WORK="${J36_WORK_DIR:-$HOME/j36-ultra-work}"
# The MVII board sources are vendored in this checkout, so nothing here reaches
# outside it. They used to be rsynced into the VM from a PowerEngine tree on the
# host, which made a MixOS build depend on a sibling repository being present.
DRIVERS="${J36_DRIVERS_DIR:-$ROOT/device/j36-ultra/mvii-board}"
EXPORT_DIR="${J36_EXPORT_DIR:-$WORK/export}"
# `build-j36-ultra.sh --mix-only': build the board specifics and hand back boot/ and
# root/, without touching the base image.
#
# WHY IT IS A MODE AND NOT A SEPARATE SCRIPT.  Everything a debug iteration changes --
# a module, a driver, the dashboard, the probe -- is built by this script and then
# folded into an 8 GB image, and folding it in costs minutes of losetup, e2fsck,
# resize2fs and tar for a payload that is tens of megabytes.  On the workstation the
# operator then writes the same two directories onto the card by hand anyway, because
# BOOT is the one partition macOS can mount.  So the injection is skipped, not
# reimplemented: same build, same artifacts, one step fewer.  The full build (no flag)
# is what produces the flashable image, and it is the only thing that should.
MIX_ONLY="${J36_MIX_ONLY:-0}"
KERNEL_URL="${J36_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
KERNEL_BRANCH="${J36_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_SRC="$WORK/linux"
KERNEL_OUT="$WORK/kernel-build"
BUSYBOX_URL="${J36_BUSYBOX_URL:-https://git.busybox.net/busybox}"
BUSYBOX_BRANCH="${J36_BUSYBOX_BRANCH:-1_36_stable}"
BUSYBOX_SRC="$WORK/busybox"
# fbdoom: the first thing that drew a moving picture on this panel, and off by
# default now that the dashboard draws on it -- J36_DOOM=1 stages it again.
# Pinned to a commit rather than a branch because the build recipe below derives
# its source list from the layout of that tree -- see the fbdoom section for why.
DOOM_URL="${J36_DOOM_URL:-https://github.com/ozkl/doomgeneric}"
DOOM_COMMIT="${J36_DOOM_COMMIT:-dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284}"
DOOM_SRC="$WORK/doomgeneric"
MODULE_SRC="$WORK/module-src"
DTB_OUT="$WORK/dtb"
INITROOT="$WORK/initramfs-root"
ARTIFACTS="$WORK/artifacts"
CACHE="$WORK/cache"

log() { printf '\n[build-j36-ultra] %s\n' "$*"; }
die() { printf '\n[build-j36-ultra] ERROR: %s\n' "$*" >&2; exit 1; }

# ── how many compiles run at once ──────────────────────────────────────────────
#
# One number, decided here and used by every build below -- kernel, out-of-tree
# modules, busybox and the emulated armhf dashboard -- so `-j' is something this
# file settles once instead of something each make call rediscovers.  J36_JOBS
# overrides it, which is what a machine that has to stay usable while this runs
# wants:  J36_JOBS=4 ./build-j36-ultra.sh.  Unset, it is the VM's own core count,
# which is DARKOS_VM_CPUS -- 8 -- unless the operator narrowed that too.
#
# It is expanded HERE, natively, and the literal number is what crosses into the
# armhf chroot.  nproc does work under qemu-arm -- /proc is bound for it, see
# ensure_armhf_chroot -- but a `$(nproc)' written inside a string that is going to
# be re-expanded by a second shell is one escaping mistake away from collapsing to
# a bare `-j', and a bare `-j' is make's "unlimited": it forks a compile for every
# source file at once, which on an emulated toolchain is how a build machine runs
# out of memory rather than how it goes faster.  A literal number cannot do that.
JOBS="${J36_JOBS:-$(nproc 2>/dev/null || echo 4)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || JOBS=4

[[ "$(uname -s)" == Linux ]] || die "build-in-vm.sh must run on Linux"
[[ -d "$DRIVERS" ]] || die "MVII J36 Drivers not found: $DRIVERS"

# The two caches in the checkout root were called Arkbuild_ccache and
# Arkbuild_package_cache until the rename, and a VM that last built before it still has
# them under those names.  Neither is a build input this script can do without cheaply:
# the ccache is every object the ARMv7 kernel build has compiled, and the package cache
# holds the armhf debootstrap tarball ensure_armhf_chroot prefers over debootstrapping
# again.  Missing, both are silent -- a cold ccache and a fallback path -- so move them
# rather than let a rename cost an hour.  The R36 build does the same in utils.sh and
# prepare.sh; this script sources neither.  Drop this once no VM predates the rename.
for legacy_cache in ccache package_cache; do
    if [[ -d "$ROOT/Arkbuild_$legacy_cache" && ! -d "$ROOT/MixOSBuild_$legacy_cache" ]]; then
        log "Moving Arkbuild_$legacy_cache to MixOSBuild_$legacy_cache"
        mv "$ROOT/Arkbuild_$legacy_cache" "$ROOT/MixOSBuild_$legacy_cache"
    fi
done

mkdir -p "$WORK" "$CACHE" "$ARTIFACTS"
# Only --mix-only exports anything, so only --mix-only gets a directory.  A full build
# ships one image and an empty export/ next to it would read as "the artifacts are
# missing" rather than "they went into the image".
if [[ "$MIX_ONLY" == 1 ]]; then
    mkdir -p "$EXPORT_DIR"
fi

if [[ ! -f "$WORK/.deps-installed" ]]; then
    log "Installing the one-time ARMv7 kernel build dependencies"
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
        bc bison build-essential ccache cpio device-tree-compiler \
        flex gcc-arm-linux-gnueabihf git gzip libelf-dev libssl-dev \
        python3 rsync xz-utils
    touch "$WORK/.deps-installed"
fi

# ── the device tree, first, and in here rather than on the workstation ─────────
#
# FIRST, because the generator asserts on the JD9365 record table and on the keypad
# pad mux, and those assertions are the whole reason it parses real MVII driver
# source instead of a frozen JSON.  A pad-mux change that would leave seven keys
# dead on the device should cost a second, not a kernel build -- so this runs before
# the kernel is even cloned.  Nothing here depends on the kernel; only dtc, fdtget
# and python3, which the block above just installed.
#
# IN HERE, because it used to run on the workstation as well, in build-j36-ultra.sh,
# and that copy wrote its three outputs into device/j36-ultra/generated/ -- inside
# the source tree, on the machine that only edits it.  Nothing consumed them: this
# is the run whose output is used, into $WORK/dtb in the VM.  The macOS run only
# dirtied the checkout and demanded dtc and fdtget on a Mac.
log "Regenerating the J36 DTB from the vendored MVII board sources"
J36_DRIVERS_DIR="$DRIVERS" J36_DTB_OUT_DIR="$DTB_OUT" \
    "$ROOT/build-j36-ultra-dtb.sh"

if [[ ! -d "$KERNEL_SRC/.git" ]]; then
    log "Cloning Linux $KERNEL_BRANCH once; later runs reuse this checkout"
    git clone --depth=1 --branch "$KERNEL_BRANCH" "$KERNEL_URL" "$KERNEL_SRC"
elif [[ "${J36_UPDATE_KERNEL:-0}" == 1 ]]; then
    log "Updating the persistent Linux checkout"
    git -C "$KERNEL_SRC" fetch --depth=1 origin "$KERNEL_BRANCH"
    git -C "$KERNEL_SRC" reset --hard FETCH_HEAD
fi

# ── The four changes this build makes to the kernel itself ────────────────────
#
# mtk-sd carries no compatible for MT6592 and refuses to probe without pinctrl;
# linux/0001-mtk-sd-mt6592.patch fixes both, and its header records why neither
# can be worked around from the device tree instead.
#
# linux/0002-drm-mediatek-mt6592.patch is the display half.  The SoC part of it is
# six hunks in two files, because MT6592's DDP is the MT2701/MT8173 generation:
# only the pipeline order and the OVL colour-format numbering are genuinely this
# SoC's own.  Its header records, register by register, what was measured against
# the MVII LK and against the stock 3.4 kernel to prove the rest of the mt2701
# driver data exact.  Everything else that port needs is device tree, not code.
#
# The remaining ten hunks are not the SoC but the board: preserve_lk_state, which
# stops mtk_crtc_atomic_disable() tearing the pipe down, and the OVL boot-layer
# snapshot that hands the panel back afterwards.  The LK lights this panel and no
# cold start exists here, so that teardown was permanent -- one GL client exiting
# cost the display until reboot -- and the "vblank wait timed out on crtc 0" WARN
# was the same event seen from inside the wait.  Sixteen hunks in seven files
# altogether, all of it behind one flag that only mt6592_mmsys_driver_data sets.
#
# linux/0003-musb-mediatek-mt6592.patch is USB, and it is now one hunk: it moves
# the two TXTOGEN/RXTOGEN writes in mtk_musb_init() from above phy_init() to
# below phy_set_mode().  On this SoC the PHY driver's .init is what ungates the
# controller, so a MUSB register access issued before it is an APB access to a
# gated peripheral, which on MediaTek stalls the bus until the watchdog fires.
# That has never actually fired here -- the LK hands over with the gate already
# open, which the PHY driver prints on every boot -- but an open gate at
# hand-over is one bootloader's habit and not a contract.
#
# It used to be much larger, and the board deleted the rest.  It also gave
# MT6592 its own musb_platform_ops without .busctl_offset, on the reasoning that
# MediaTek added the TXTOG/RXTOG block at 0x80..0x87 in the MT2701/MT7623
# generation and moved the multipoint BUSCTL block to 0x480 to make room, so a
# part two generations older should still have BUSCTL at the stock 0x80.  The
# J36 PHY driver settles that by writing to the hardware rather than reasoning
# about it -- a pattern into ep0's TXFUNCADDR at both candidate bases, read
# back, restored -- and this board answers 0x480, with 0x080 reading back 00
# exactly as a gated MUSB_RXTOG would.  It is the MT2701 layout after all, so
# mediatek.c was right unmodified and the variant, the second ops table and the
# "mediatek,mt6592-musb" of_match entry are gone.  The device tree still names
# that compatible first and "mediatek,mtk-musb" second; the second is what binds.
#
# linux/0004-usb-phy-generic-no-node-no-vbus.patch is one line of USB cosmetics
# with one board-specific reason to care.  mediatek.c registers a nop xceiv from
# code for every MUSB glue, so that device has no of_node and its "vbus" supply
# can never resolve; the regulator core then refuses to give its dummy to an
# exclusive request and warns about it at KERN_WARNING, which on this board is
# the panel, mid-splash.  The patch asks for the supply only when there is a
# node that could have named one.
#
# linux/0005-arm-mediatek-mt6592-smp.patch is the other seven cores.  Mainline
# platsmp.c knows three MediaTek release protocols and MT6592 is none of them:
# the three SoCs there each hold three secondary keys, which would light four
# cores of eight even if the base address matched, and it does not.  The
# constants the patch adds are transcribed from MediaTek's own MT6592 platform
# sources -- mediatek/platform/mt6592/kernel/core/mt-smp.c in the 3.4.67 ALPS
# trees -- and not extrapolated from the neighbouring SoCs.  The release
# protocol is SRAMROM at 0x10202000: the jump address goes to +0x34, then cores
# 1..3 are woken by writing 0x534c4131, 0x4c415332, 0x41534c33 to +0x38 and
# cores 4..7 by writing 0x534c4134, 0x4c415335, 0x41534c36, 0x534c4137 to +0x3c.
#
# Seven keys is only half of it, because MT6592 is two Cortex-A7 clusters and
# not one eight-core cluster -- an A7 MPCore tops out at four.  Cores 0..3 are
# MP0 and are already powered when the LK hands over; cores 4..7 are MP1 and are
# cold.  So the patch also does the MP1 power-up the vendor's mtcmos code does
# before it writes those last four keys: the cluster rail and its L2 retention
# release, then each of the four core rails with its L1 retention release, then
# the debug block, all through SPM at 0x10006000 behind the 0xb16 project-code
# write that unlocks it.  Coherency is the last step on each side -- clear
# ACINACTM in MCUSYS MP0/MP1_AXI_CONFIG at 0x10200000 and raise the matching
# CCI-400 snoop and DVM bits at 0x10390000, SI4 for MP0 and SI3 for MP1.
#
# Every vendor spin-until-ready loop became a bounded poll that logs and gives
# up, so a wrong bit costs four cores and a KERN_ERR line rather than a hang at
# an unlit screen; maxcpus=4 on the command line skips the MP1 half outright.
# The device tree side is the enable-method in generate_dts.py; neither half
# does anything without the other, so they move together.
#
# Applied idempotently rather than from a stamp file, because this checkout
# persists across runs and a stamp can outlive the tree it describes -- a
# J36_UPDATE_KERNEL reset above throws the patch away and would leave the stamp
# claiming otherwise.  `apply --reverse --check' succeeds only when the change is
# already present, so the tree is asked instead of a bookkeeping file.
#
# The same persistence is why there is a revert-and-retry below.  A patch that is
# EDITED during bring-up -- 0003 has now been rewritten twice -- leaves a tree
# holding the previous version, which the new one neither reverse-applies to nor
# applies over, and the build would stop dead on a checkout the developer has no
# reason to suspect.  So the files the patch touches are checked back out and it
# is tried once more.  That is safe here only because these five patches touch
# disjoint files -- mtk-sd.c, two DRM files, mediatek.c, phy-generic.c,
# platsmp.c -- so restoring one patch's files cannot undo another's; keep it that
# way, or this has to become a full reset-and-reapply of all of them.
apply_kernel_patch() {
    local patch="$ROOT/device/j36-ultra/linux/$1" what="$2"
    [[ -f "$patch" ]] || die "missing kernel patch: $patch"

    if git -C "$KERNEL_SRC" apply --reverse --check "$patch" 2>/dev/null; then
        log "The $what patch is already applied"
        return
    fi

    if ! git -C "$KERNEL_SRC" apply --check "$patch" 2>/dev/null; then
        local files=()
        mapfile -t files < <(sed -n 's|^+++ b/||p' "$patch")
        [[ ${#files[@]} -gt 0 ]] || die "$1 has no +++ b/ lines: it is not a patch"
        log "Restoring ${files[*]} to make room for a revised $what patch"
        git -C "$KERNEL_SRC" checkout -- "${files[@]}" 2>/dev/null \
            || die "cannot restore ${files[*]} in $KERNEL_SRC"
        git -C "$KERNEL_SRC" apply --check "$patch" 2>/dev/null \
            || die "$1 neither applies to nor is applied in $KERNEL_SRC; refresh it against $KERNEL_BRANCH"
    fi

    log "Applying the $what patch"
    git -C "$KERNEL_SRC" apply "$patch"
}
apply_kernel_patch 0001-mtk-sd-mt6592.patch "mtk-sd MT6592"
apply_kernel_patch 0002-drm-mediatek-mt6592.patch "mtk_drm MT6592 display"
apply_kernel_patch 0003-musb-mediatek-mt6592.patch "musb MT6592 USB host"
apply_kernel_patch 0004-usb-phy-generic-no-node-no-vbus.patch "usb_phy_generic VBUS"
apply_kernel_patch 0005-arm-mediatek-mt6592-smp.patch "MT6592 SMP"

mkdir -p "$KERNEL_OUT"
if [[ ! -f "$KERNEL_OUT/.config" ]]; then
    log "Creating the initial MT6592 ARMv7 kernel configuration"
    make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
        CROSS_COMPILE=arm-linux-gnueabihf- multi_v7_defconfig
fi

CONFIG="$KERNEL_OUT/.config"
SC="$KERNEL_SRC/scripts/config"
config_y() { "$SC" --file "$CONFIG" -e "$1"; }
config_m() { "$SC" --file "$CONFIG" -m "$1"; }
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

# CRC32 is `default y' and would very probably be on anyway.  It is asked for
# explicitly because j36_mt6592_wifi's firmware download puts a CRC-32 in every
# 2 KiB chunk header and the boot ROM checks it, so crc32_le() has to resolve at
# insmod time -- and an unresolved symbol there is a module that never loads,
# reported as a Wi-Fi failure with nothing about a CRC in it.
config_y CRC32

# ── The black screen between the LK's logo and the MixOS splash ───────────────
#
# simplefb hands this kernel the framebuffer the LK was already drawing into,
# logo and all, and fbcon then binds to it and clears it to a text console:
#
#   [3.338545] simple-framebuffer 82700000.framebuffer: framebuffer at 0x82700000
#   [3.339220] Console: switching to colour frame buffer device 80x30
#
# /init does not start mixsplash until 4.2 s, so that is the better part of a
# second of black panel in the middle of a boot that is meant to show a picture
# and then another picture. Deferred takeover leaves the firmware's pixels alone
# until something is actually printed to the console, which with loglevel=4 is
# nothing at all on a boot that goes right -- so the LK logo stays up until
# mixsplash paints over it, and the seam closes.
#
# It stays honest on a boot that goes wrong: the first KERN_ERR takes the console
# over exactly as before, clears the logo and starts drawing, so a failure still
# reaches the panel. That is also why loglevel is not lowered past 4 to help this
# along -- the point is to print nothing when there is nothing to say, not to
# stop being able to say it.
config_y FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER

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

# Keep this first-stage image below the fixed 9 MiB BOOTIMG partition.
# Each thing that leaves this list leaves it because its bring-up is proven and
# it earned the bytes: storage went first, then NET -- see below -- then DRM, for
# the GPU section further down, then SOUND and SND, for the audio section after
# it, and now USB_SUPPORT, for the USB section after that. A native DSI/display
# driver is still on the outside.
#
# USB_SUPPORT left this list as a menuconfig gate only. It costs nothing on its
# own -- it is the `menuconfig' symbol that makes the whole drivers/usb menu
# visible -- and everything behind it that this board actually loads is =m, on
# the BOOT partition, insmodded from /init only when the command line asks. The
# USB section below turns off the four host controllers and the gadget stack by
# name, because none of them is on this SoC and all four are `default y' under
# some dependency multi_v7_defconfig satisfies.
#
# WLAN and BT stay off, and they are disabled here rather than left out: both
# live inside `if NET' in net/Kconfig and would come back by default once NET is
# on. Both the =y for NET and the "is not set" lines for these are in .config
# before the single olddefconfig below, which is what makes the explicit n stick.
#
# WIRELESS IS NO LONGER IN THIS LIST, and WLAN still is, which looks like a
# contradiction and is not. WIRELESS is the cfg80211/mac80211 menu; WLAN is the
# in-tree driver menu under drivers/net/wireless. This board's radio driver is
# out of tree, built from device/j36-ultra/linux, so it needs the first and none
# of the second -- and the second is a large menu of drivers for hardware that is
# not on this board, every one of which would be built and shipped for nothing.
# The wireless section further down turns CFG80211 on as a module and refuses
# MAC80211 outright: the MT6592 CONSYS part is FULLMAC, its firmware owns the MAC,
# and mac80211 would be some 700 KiB of code with nothing to do.
#
# SCSI USED TO BE IN THIS LIST and is not any more.  It left for one reason: a USB
# disk is a SCSI device.  usb-storage is a SCSI host adapter that speaks Bulk-Only
# Transport, sd_mod is what turns the LUN behind it into /dev/sda, and there is no
# arrangement of the USB menu that reaches a mountable partition without both. So
# the storage section below turns SCSI on as a MODULE and prunes the rest of that
# menu by name; ATA stays refused, because libata is the other thing under SCSI
# and there is no SATA or PATA anywhere on this SoC.
for symbol in \
    MEDIA_SUPPORT WLAN BT ATA \
    DEBUG_INFO DEBUG_KERNEL KALLSYMS LOGO; do
    config_n "$symbol"
done

# ── Storage: the microSD card this kernel is already being loaded from ────────
#
# MSDC1 at 0x11240000 is the microSD slot. That is not a guess: it is the base
# the MVII LK's own mt6592_msdc_sd.c programs to read the card this kernel comes
# off, alongside MSDC0 at 0x11230000 for the eMMC. mmc@11240000 in the generated
# device tree claims it with `mediatek,mt6592-mmc', which the patch above adds to
# mtk-sd. No eMMC node is described, so the card is the only host and its
# partitions land on mmcblk0.
#
# REGULATOR_FIXED_VOLTAGE is not padding. mmc->ocr_avail is assigned in exactly
# one place in the whole MMC core -- drivers/mmc/core/regulator.c, from the vmmc
# regulator -- and mtk-sd never sets it itself. With no driver behind
# vmmc-supply the host advertises no voltage at all, and card init then fails
# with nothing in the log pointing at the reason.
#
# EXT2 because the rootfs this card carries is ext2: setup_partition.sh and
# device/r36-ultra/build-in-vm.sh both set ROOT_FILESYSTEM_FORMAT=ext2, precisely
# so this kernel and the MVII LK can be asked for the same card.  It is its own
# driver, not a mode of ext4 -- `mount -t ext4' does NOT mount an ext2 filesystem
# when EXT2_FS is built separately, which is why /init names ext2 first and why
# leaving this symbol out would produce a card that mounts on a PC and not here.
# EXT4 and BTRFS stay for the cards already in the field, written by the builds
# that came before this one; /init tries all three.
#
# EXFAT and VFAT are not for /init -- they are for BOOT, which the rootfs mounts
# itself, and for whatever the operator plugs into the USB port.  The card is two
# partitions now, and bootstrap_rootfs.sh writes the whole of its fstab as
#
#   LABEL=ROOTFS / ext2 defaults,noatime 0 1
#   LABEL=BOOT /boot vfat defaults 0 0
#
# There is no third line and no nofail on either of these, because there is no longer
# an optional partition to be missing: the login user's home is /home/virtua, a
# directory on the root filesystem, so it is present whenever the root is.  A vfat
# driver this kernel did not have would fail local-fs and take a machine with no
# keyboard driver into emergency mode, which is why VFAT_FS is not optional here.
# EXFAT stays built in for the cards already in the field, whose p3 was made vfat and
# converted to exfat by firstboot, and for USB sticks formatted on a PC.
for symbol in \
    BLOCK BLK_DEV MMC MMC_BLOCK MMC_MTK REGULATOR REGULATOR_FIXED_VOLTAGE \
    EXT2_FS EXT4_FS BTRFS_FS EXFAT_FS VFAT_FS; do
    config_y "$symbol"
done

# ── The other disks: SCSI, as a module, for USB mass storage only ─────────────
#
# A USB stick is a SCSI target.  That is not an analogy: usb-storage registers as
# a SCSI host adapter, wraps SCSI command blocks in Bulk-Only Transport, and
# sd_mod claims the LUN and creates /dev/sda.  Drop any one of the three and the
# port enumerates the device, prints its VID:PID, and produces no block device --
# which reads in a log exactly like a broken cable.
#
# =m FOR ALL THREE, for a reason that has nothing to do with the clock-gate
# argument the rest of this file keeps making.  Here it is simply the 9 MiB
# BOOTIMG budget and the containment that goes with it: scsi_mod, sd_mod and
# usb-storage are ~250 KB together, they are useless without the USB stack that
# is already =m behind j36.usb=1, and they are staged into j36/usb/ and insmodded
# by the same run_usb that loads musb.  A boot without the word loads none of it.
#
# THE PRUNE IS THE POINT OF THE LOOP.  drivers/scsi is one of the largest menus
# in the tree and multi_v7_defconfig turns on a dozen entries in it -- HBA drivers
# for hardware that is not here, the tape and CD-ROM upper layers, the transport
# attribute helpers.  With SCSI refused outright those all fell out for free; with
# SCSI back they would return at their defconfig values, so the allowlist is the
# core, its two hidden helpers, procfs support and sd_mod, and everything else
# that starts with SCSI_ is turned off by name.  Same shape as the USB and HID
# prunes further down, and for the same reason: an allowlist is the only kind of
# list that stays correct across a kernel bump.
#
#   BLK_DEV_SR / CHR_DEV_ST / CHR_DEV_SG are the CD, tape and passthrough upper
#   layers.  None of them is a disk, all three are `default y' under some
#   dependency defconfig satisfies, and sg in particular hands a raw command
#   channel to anything that can open the node.
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        SCSI|SCSI_MOD|SCSI_COMMON|SCSI_DMA|SCSI_PROC_FS) ;;
        SCSI_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_SCSI[A-Z0-9_]*=(y|m)$' "$CONFIG")
for symbol in BLK_DEV_SR CHR_DEV_ST CHR_DEV_SG; do
    config_n "$symbol"
done
config_m SCSI
config_m BLK_DEV_SD

# NTFS3, and it is the one filesystem here that is NOT for this card.  Every
# partition MixOS writes is ext2, vfat or exfat and all three are =y above; this
# is for the disk somebody plugs in, which on any desk with a Windows machine on
# it is NTFS more often than not.  ntfs3 is the in-tree read-write driver (the old
# read-only fs/ntfs was deleted in 6.9, so there is no second option), it selects
# NLS for the UTF-16 name conversion -- nls_base, already =y here, so ntfs3.ko
# carries no modular dependency of its own -- and =m puts it in the same j36/usb/
# payload as the stack that will need it.  The card itself never does.
#
# CONFIG_NTFS_FS HAS TO GO FIRST, and it is not the driver its name suggests.
# 6.10 brought the symbol back after the classic driver was deleted, as a
# compatibility shim for configs carrying it: a tristate whose entire body is
# `select NTFS3_FS'.  multi_v7_defconfig still carries CONFIG_NTFS_FS=y, a select
# forces its target to at least the selecting symbol's value, and =y outranks =m,
# so olddefconfig quietly promoted NTFS3_FS back to built-in and the assertion
# below caught it.  Turning the shim off costs nothing -- it is a name, not code --
# and it is the only way `config_m NTFS3_FS' survives to the end of the file.
config_n NTFS_FS
config_m NTFS3_FS
#
# A dirty volume -- one Windows fast-booted out of rather than shut down -- is
# refused read-write by design.  mixos-automount handles that by falling back to
# a read-only mount rather than forcing it, which is the difference between a
# disk you can read and a filesystem with two owners.

# ── What the shared rootfs's PID 1 needs to exist at all ──────────────────────
#
# The card mounts, switch_root succeeds, and then systemd 257 aborts:
#
#   systemd[1]: Failed to find module 'unix'
#   systemd[1]: Failed to open netlink, ignoring: Function not implemented
#   systemd[1]: Failed to allocate device monitor: Function not implemented
#   systemd[1]: Failed to allocate notification socket: Function not implemented
#   systemd[1]: Assertion '...' failed at src/core/device.c:64, function
#               device_unset_sysfs(). Aborting.
#   systemd[1]: Freezing execution.
#
# ENOSYS from socket(2) three times over. NET was in the disable list above and
# UNIX was in the enable list at the top -- CONFIG_UNIX lives under `if NET', so
# olddefconfig dropped it, along with INET, PACKET, POSIX_MQUEUE and
# SECCOMP_FILTER (which depends on NET as well as HAVE_ARCH_SECCOMP_FILTER).
# systemd cannot open an AF_UNIX notification socket or an AF_NETLINK uevent
# monitor, so its device objects never get a sysfs path, and the assertion that
# every .device unit has one takes PID 1 down with it. Nothing was wrong with the
# storage or the switch_root; the kernel simply had no sockets.
#
# NAMESPACES is here for the same class of reason, found by auditing the shipped
# kernel.config rather than by another boot: it was =n, which also removes
# NET_NS/PID_NS/IPC_NS/UTS_NS, and Debian's own units use PrivateMounts,
# PrivateTmp and ProtectSystem. Those services fail to start without it.
#
# The POSIX_ACL symbols matter because systemd sets ACLs on the journal.
# FS_POSIX_ACL was already y; the per-filesystem ones were not, so the generic
# support was there with nothing able to use it.  EXT2_FS_POSIX_ACL is the one
# that now matters -- the rootfs is ext2 -- and EXT2_FS_XATTR comes with it
# because ext2's ACL support is gated on it in fs/ext2/Kconfig, unlike ext4's,
# which is unconditional.  The btrfs and ext4 entries stay for the cards written
# by earlier builds.
for symbol in \
    NET UNIX INET NAMESPACES EXT2_FS_XATTR EXT2_FS_POSIX_ACL \
    BTRFS_FS_POSIX_ACL EXT4_FS_POSIX_ACL; do
    config_y "$symbol"
done

# ── The GPU: DRM core built in, lima built as a module on purpose ──────────────
#
# The MT6592 carries a Mali-450 MP4 and DRM lima drives that generation.  The
# register map is not a guess: the stock kernel's own `struct resource' array,
# read out of its .data at VA 0xc0b71de4, names all eighteen blocks -- Mali_GP at
# 0x13040000 IRQ 234, GP_MMU 0x13043000/235, PP0..PP3 at 0x13048000, 0x1304a000,
# 0x1304c000, 0x1304e000 with 236/238/240/242, their MMUs 0x13044000..0x13047000
# with 237/239/241/243, both L2s at 0x13041000 and 0x13050000, Broadcast 0x13053000,
# DLBU 0x13054000, PP_Broadcast 0x13056000/244 -- and every offset from that base
# matches lima's own mali450 column in lima_device.c block for block.  The device
# tree node generate_dts.py emits carries all of it; that comment has the table.
#
# THE MODULE IS THE WHOLE POINT.  MT6592 power-gates the MFG domain through the
# SPM, and nothing on this boot path un-gates it: the MVII LK has a working
# mfg_power_on(), but its only callers are in the MVII kernel, not in the LK's
# hand-off to Linux.  Reading an unpowered MTK subsystem stalls the AXI bus, so a
# built-in lima would probe during boot and take the board into a watchdog reset
# with nothing in any log to say why -- and it would do that on every boot,
# destroying the serial console, the input bring-up and the fbdoom test with it.
# As a module it is loaded from /init only after tools/mfgpower.c has powered the
# domain and read back the GP and PP product IDs, and only when the command line
# asks for it.  A default boot is byte-identical to the one before this section.
#
# DRM itself is =y rather than =m because lima's helper symbols (DRM_SCHED,
# DRM_GEM_SHMEM_HELPER) are tristate and follow lima to =m, so the core is the
# only piece that has to be resident, and a modular DRM core would mean loading
# four more .ko in a fixed order from a shell that has no modprobe.
#
# ── The display: mtk_drm, modular for the same reason and one more ─────────────
#
# mediatek-drm is the KMS half of the pair: lima renders, mtk_drm scans out, and
# Mesa's kmsro glue needs both card nodes to exist before EGL can present
# anything.  MT6592's DDP is the MT2701/MT8173 generation, so the driver is almost
# entirely reused -- see linux/0002-drm-mediatek-mt6592.patch and the display
# section of generate_dts.py, which between them record every register that was
# measured to prove it.
#
# Four symbols, and every one of them =m:
#
#   DRM_MEDIATEK        mediatek-drm.ko, which contains mtk_dsi
#   MTK_MMSYS           two modules from one symbol, mtk-mmsys.ko and mtk-mutex.ko
#   PHY_MTK_MIPI_DSI    phy-mtk-mipi-dsi-drv.ko, note the -drv
#
# MTK_MMSYS is the one that has to be forced.  It is `default ARCH_MEDIATEK', so
# it is =y in this configuration today and mtk-mmsys is already inside vmlinux --
# invisible only because no node in the old device tree carried a compatible it
# matched.  The moment the display nodes land it would bind at boot, register the
# "mediatek-drm" and "clk-mt2701-mm" platform devices as its children, and the
# claim that a default boot is byte-identical would stop being true.  As a module
# it binds nothing until /init insmods it.
#
# DRM_MEDIATEK_DP and DRM_MEDIATEK_HDMI stay off: neither block exists on this
# SoC, and the HDMI one `select's SND_SOC_HDMI_CODEC.  The prune below takes them
# because the allowlist names DRM_MEDIATEK exactly, not a prefix.
#
# ── DRM_UDL: the third card, and it is not a SoC block at all ──────────────────
#
# The two refusals above are the reason this one needs a paragraph.  MT6592 has
# no HDMI encoder and no DisplayPort, so "HDMI on this board" cannot come from
# the display subsystem -- but it does not have to.  A DisplayLink adapter is a
# USB device that receives compressed framebuffer updates over bulk transfers and
# drives the monitor itself, so the entire path is drivers/usb plus udl.ko, and
# the DDP, the MIPI-TX PHY and mtk_drm are not involved in it at any point.  That
# makes it the one external-display route this SoC can take, and it is =m and
# staged with the USB set below rather than with the display set above.
#
# What it will and will not bind, because this decides whether a given adapter
# works: mainline udl speaks the USB 2.0 DisplayLink protocol -- DL-1x0/DL-1x5,
# the "DisplayLink Graphics Adapter" generation, matched by
# USB_CLASS_VENDOR_SPEC interface class 0xff/0x00/0x00 with a vendor-specific
# descriptor.  DL-3xxx and later are USB 3.0 parts speaking a different protocol
# with no in-tree driver; those need out-of-tree evdi.  This board is USB 2.0
# high-speed and nothing else, so a USB 3.0 adapter has nothing to gain here
# anyway.  At 640x480 the bandwidth question does not arise.
#
# The prune matters more here than in the ARCH loop above: multi_v7_defconfig
# turns on a dozen other DRM drivers, and one of them, DRM_SIMPLEDRM, matches the
# very `simple-framebuffer' node FB_SIMPLE is driving.  Two drivers bidding for
# one panel is a lottery, and simpledrm wins it by calling
# drm_aperture_acquire_from_firmware() and evicting simplefb -- which is the
# working display.  So every DRM_* symbol that is not lima or mediatek is turned
# off, by enumeration rather than by name, and DRM_SIMPLEDRM is then refused
# outright after olddefconfig.  The helper symbols those two `select' are swept up
# by this too; that is harmless, because a selected symbol's value is computed
# from the select and olddefconfig puts them back, and DRM_BRIDGE and
# DRM_PANEL_BRIDGE are `def_bool y' and cannot be turned off at all.
config_y DRM
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        DRM|DRM_LIMA|DRM_MEDIATEK|DRM_UDL) ;;
        DRM_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_DRM_[A-Z0-9_]+=(y|m)$' "$CONFIG")
config_m DRM_LIMA
config_m MTK_MMSYS
config_m PHY_MTK_MIPI_DSI
config_m DRM_MEDIATEK
# Asked for down in the USB section, where its dependency lives; named in the
# allowlist here so this prune does not take it back out again.
# Off, and it is doing real work switched off.
#
# lima registers no CRTC, so for lima there is nothing to emulate; the symbol is
# `default FB' and would otherwise arrive on the back of FB_SIMPLE and drag
# DRM_KMS_HELPER in for nothing.  mediatek-drm does register a CRTC, and leaving
# this off is what keeps the two display paths out of each other's way: with no
# fbdev emulation mtk_drm creates no second /dev/fb, so /dev/fb0 stays simplefb's
# window onto the LK's framebuffer and the console keeps working, and mtk_drm
# programs no register at all until userspace opens /dev/dri/card0 and sets a
# mode.  Loading the modules is therefore visually a no-op.
config_n DRM_FBDEV_EMULATION
# mfgpower reaches the SPM through /dev/mem.  STRICT_DEVMEM may stay on: it
# blocks RAM, not MMIO, and mem.c maps a non-RAM pfn opened O_SYNC as
# pgprot_noncached, which is exactly what a register window needs.
config_y DEVMEM

# ── Audio: the ALSA core, modular, and one driver for the MT6592 AFE ───────────
#
# Until now SOUND and SND were in the disable list above, so this kernel had no
# ALSA core, no /dev/snd and no controlC0 -- which is worth writing down because
# the missing card was mistaken for the `90-alsa-restore.rules:1 GOTO has no
# matching label' warning in the boot log.  That warning is unrelated: it comes
# out of the shared Debian rootfs, whose first two rule lines are early-exit
# GOTOs whose LABEL is in a file udev is not reading here.  It is still there
# after this section; it was never the reason there was no sound.
#
# SOUND=y, everything else =m.  SOUND alone puts soundcore in vmlinux, which is
# ten kilobytes of char-device registration -- the BOOTIMG budget does not
# notice it -- and leaves snd, snd-timer, snd-pcm and our own driver on the vfat
# BOOT partition, loaded from /init only when the command line asks.  Same
# containment as lima and mtk_drm: a boot with no j36.audio word loads nothing,
# binds nothing and is byte-identical to the one before this section.
#
# SND_DUMMY is the load-bearing oddity.  CONFIG_SND_PCM has no prompt of its own
# -- it exists only to be `select'ed by a card driver -- so writing SND_PCM=m
# into .config is silently dropped by olddefconfig, exactly the way CONFIG_UNIX
# was dropped under `if NET' further up, and the assertion below would then fail
# with nothing to point at.  An in-tree card that selects it is the way to ask
# for it, and snd-dummy is the smallest.  It is built and then deliberately not
# staged: nothing in j36/audio/ carries it, so it never loads, never registers
# and never takes card0 from the AFE.
#
# The prune is not optional.  multi_v7_defconfig carries SND_SOC=y and dozens of
# SND_SOC_* codecs behind it, all invisible today only because SND is off; the
# moment SND comes back they would come back with it, every one of them building
# into a kernel whose only sound hardware has no ASoC driver anywhere upstream.
# So the same enumerate-and-disable idiom as DRM above, with an allowlist of
# exactly what this board uses, and an outright refusal of SND_SOC after
# olddefconfig -- because a stray `select SND_SOC_*' from some codec that
# survives would be a silent 300 KiB and a second sound card.
#
# SND_PCM_TIMER is in the allowlist and is not a feature request: SND_PCM's line
# in sound/core/Kconfig is `select SND_TIMER if SND_PCM_TIMER', so pruning this
# one bool -- which is `default y' and matches SND_* like everything else --
# quietly takes snd-timer.ko out of the build, and the assertion below that says
# it must be =m would then fail on a configuration that was otherwise right.
config_y SOUND
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        SOUND|SND|SND_TIMER|SND_PCM|SND_PCM_TIMER|SND_PROC_FS|SND_DRIVERS|SND_DUMMY) ;;
        SOUND_*|SND_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_(SOUND|SND)[A-Z0-9_]*=(y|m)$' "$CONFIG")
config_m SND
config_y SND_DRIVERS
config_m SND_DUMMY
# SND_PROC_FS is kept for one reason: `cat /proc/asound/cards' is the first thing
# anyone reaches for on a board with no aplay, and this initramfs has no aplay.
# It is a few kilobytes inside snd.ko, which is on the BOOT partition.
#
# The rest are dead weight, and named explicitly rather than left to the prune so
# that the intent survives a kernel bump that renames one of them.  Nothing in the
# MixOS rootfs opens /dev/dsp or /dev/sequencer.
for symbol in SND_SUPPORT_OLD_API SND_PCM_OSS SND_MIXER_OSS SND_SEQUENCER \
    SND_VERBOSE_PROCFS SND_DEBUG; do
    config_n "$symbol"
done

# ── USB: one controller, host mode, and everything else refused ────────────────
#
# What is on this SoC, measured rather than assumed.  MT6592 has exactly ONE USB
# controller: a Mentor MUSBMHDRC dual-role core at 0x11200000, with the MediaTek
# U2 PHY at 0x11210800 inside the SIFSLV window at 0x11210000, gated by the PERI
# clock block at 0x10003010 (PDN_CLR) / 0x10003018 (PDN_STA).  There is no EHCI,
# no OHCI and no XHCI anywhere on the die -- which is why all three are refused
# below rather than merely left alone, and why every "add USB" instinct that
# starts with USB_EHCI_HCD would produce a kernel that binds nothing.
#
# The evidence that host mode works on this hardware is the stock Android kernel:
# it carries drivers/misc/mediatek/usb20/ driving the same core, with usbhid and
# hid-generic built alongside.  Mainline's equivalent is USB_MUSB_MEDIATEK, which
# has been in-tree since 5.4 and is the MT2701/MT8173-generation glue -- the same
# reuse argument as mtk_drm above.
#
# HOST ONLY, and that is a deliberate narrowing.  The core is dual-role and the
# board's port is an OTG port, but dual-role means a role switch driven by an ID
# pin this bring-up has never read, and the PHY has no sequence that releases the
# role override -- both of MTK's are overrides that pin it.  So USB_MUSB_HOST is
# the mode and dr_mode = "host" is in the device tree, and the role is decided at
# power-on rather than by how the ID pin floats.
#
# VBUS is sourced, which is new.  It is a GPIO on this board and not the MT6322
# boost the MVII note assumed: the stock Android kernel's mt_usb_set_vbus() sets
# pad 15 to mode 0 and drives it high, and j36_mt6592_usb_phy.ko now does the
# same off j36,drvvbus-pad, and holds it there -- charging arrives on a separate
# DC inlet with no data lines in it, so the port has nothing to arbitrate.
# So a bus-powered hub enumerates.  It is NOT free, and the cost landed on the
# gauge rather than on the port: CHRIN is one pin and it is on this net, so a pad
# held up for the whole uptime holds the PMIC's charger comparator up with it and
# an unplug is invisible.  j36_mt6592_pmic.ko answers that by measuring the pin
# instead of trusting the bit -- see the vbus_sourcing note in the help text, and
# j36.usb=automeasure for the board where measuring is not enough.  The other cost
# is that
# the 5 V is a boost off VBAT, which on this PMIC is the system node -- with no
# cell fitted that is the rail the class-D amp already proved can be pulled under
# the undervoltage lockout, so j36.usb=novbus exists and a self-powered hub is
# still the safer arrangement.  DEVCTL bits 3|4 report whether VBUS is present,
# which is how to tell which of the two is actually carrying the port.
#
# MUSB_PIO_ONLY is the first-bring-up choice.  MUSB's DMA on this generation goes
# through MediaTek's own controller glue and none of it has been measured here;
# PIO is slower and cannot be wrong.  A mouse, a keyboard and a 640x480
# DisplayLink surface are not a bandwidth problem.
#
# =m for the whole set, for the third time in this file and for the same reason:
# an APB access to a clock-gated MediaTek peripheral does not fault, it HANGS THE
# BUS.  A built-in musb would probe at boot, before anything has ungated PERI,
# and take the board into a watchdog reset with nothing in any log.  As modules
# they are on the BOOT partition and /init insmods them only for j36.usb=1.  The
# same containment as lima, mtk_drm and the AFE: a default boot loads none of it.
#
# GENERIC_PHY is the one =y here, because it has to be: it is bool, phy-core is
# what j36_mt6592_usb_phy.ko registers into, and mediatek.c reaches the PHY with
# devm_of_phy_get_by_index().  It is a few kilobytes of class registration.
config_y USB_SUPPORT
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        USB|USB_SUPPORT|USB_COMMON|USB_PHY|USB_HID|USB_ROLE_SWITCH) ;;
        USB_MUSB_HDRC|USB_MUSB_MEDIATEK|USB_MUSB_HOST) ;;
        USB_ANNOUNCE_NEW_DEVICES|USB_DEFAULT_PERSIST) ;;
        USB_STORAGE) ;;
        USB_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_USB[A-Z0-9_]*=(y|m)$' "$CONFIG")
# The HID menu gets the same treatment and for a plainer reason: multi_v7_defconfig
# turns on around eighty vendor HID drivers, every one of them a module this build
# would compile and this card would never stage.  hid-generic is what binds a mouse
# and a keyboard -- it claims any HID device no specific driver wanted -- so the
# allowlist is the core, the gate and the generic driver, and nothing else.
while IFS='=' read -r option _value; do
    symbol="${option#CONFIG_}"
    case "$symbol" in
        HID|HID_SUPPORT|HID_GENERIC) ;;
        HID_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_HID[A-Z0-9_]*=(y|m)$' "$CONFIG")
config_y HID_SUPPORT
config_m USB
config_y USB_ANNOUNCE_NEW_DEVICES
config_y USB_DEFAULT_PERSIST
config_y USB_PHY
config_m NOP_USB_XCEIV
config_y GENERIC_PHY
config_m USB_MUSB_HDRC
config_y USB_MUSB_HOST
config_y MUSB_PIO_ONLY
config_m USB_MUSB_MEDIATEK
config_m USB_ROLE_SWITCH
config_m HID
config_m USB_HID
config_m HID_GENERIC
# The mass-storage class driver.  Its two dependencies are settled elsewhere --
# USB is =m two lines up, SCSI is =m in the storage section -- and =m is the only
# value it could take anyway with both of them modular.  See that section for why
# SCSI came back at all.
config_m USB_STORAGE
# The USB->HDMI half.  DRM_UDL is asked for here rather than in the DRM section
# because its dependency is USB, not the display block -- `depends on DRM && USB
# && MMU'.  DRM is =y, USB is =m, so =m is the only value it can take, which is
# also the value that keeps it off the 9 MiB image and out of a default boot.
config_m DRM_UDL
# Named refusals, one line of reasoning each:
#
#   USB_GADGET      the other half of dual-role; nothing here is a peripheral,
#                   and it would pull in a UDC, a composite framework and a
#                   function set for a port that is being driven as a host.
#   USB_EHCI/OHCI/  no such block on MT6592.  Left to the prune they would be
#   XHCI_HCD        off anyway; refused by name so that a kernel bump which makes
#                   one of them `default y' fails the build instead of adding a
#                   host controller driver for hardware that does not exist.
#   USB_DWC2/DWC3/  the same, for the three other IP cores an ARM defconfig
#   CHIPIDEA        commonly carries.  This SoC has MUSB and only MUSB.
#   USB_UAS         the OTHER mass-storage protocol, and the one entry here that
#                   is a judgement rather than absent hardware.  UAS is worth
#                   having when the controller can queue and stream -- this one
#                   is MUSB with MUSB_PIO_ONLY, where every byte crosses on the
#                   CPU and there is nothing for command queueing to overlap.
#                   What it would add is uas's quirk table and a second protocol
#                   that has to be blacklisted per enclosure when it misbehaves.
#                   usb-storage drives the same devices with BOT.
for symbol in USB_GADGET USB_EHCI_HCD USB_OHCI_HCD USB_XHCI_HCD \
    USB_DWC2 USB_DWC3 USB_CHIPIDEA USB_UAS; do
    config_n "$symbol"
done

# ── The power supply class, for the PMIC ──────────────────────────────────────
#
# =y and not =m, which is the opposite of the rule everything else in this file
# follows, and the reason is that this is a class rather than a driver.  It
# registers no hardware, touches nothing at boot and costs a few kilobytes; what
# it provides is /sys/class/power_supply and the registration entry points that
# j36_mt6592_pmic.ko -- which IS =m, and staged behind j36.power -- links
# against.  A modular class would mean a second file in load.order for no gain
# and one more thing to get out of order.
#
# It is asked for by name rather than left to arrive by dependency because
# nothing else in this configuration selects it.  multi_v7_defconfig gets it
# from battery and charger drivers for boards that are not this one, so a future
# ARCH prune taking those out would take the class with them, and the failure
# would surface as a PMIC module that builds fine and fails to insmod with
# unresolved symbols on the board.
config_y POWER_SUPPLY

# ── The backlight class, for the BLS driver ───────────────────────────────────
#
# Same argument as POWER_SUPPLY above, one word at a time: a class, not a driver,
# =y so that j36_mt6592_backlight.ko has one fewer line in load.order, and named
# explicitly because nothing in this configuration selects it on purpose.  It
# does usually arrive anyway -- half the DRM panel drivers in multi_v7_defconfig
# select it for boards that are not this one -- and that is precisely the kind of
# accidental dependency that disappears under a prune, taking
# /sys/class/backlight and the dashboard's brightness slider with it.
config_y BACKLIGHT_CLASS_DEVICE

# ── Wireless: cfg80211 and nothing above it ───────────────────────────────────
#
# The MT6592's connectivity subsystem is on the die, not on an SDIO bus, and it
# is FULLMAC: the firmware owns the MAC. It beacons, ACKs, retries, does its own
# rate control and does the CCMP once j36_mt6592_wifi.ko hands it a key. What
# crosses the AHB HIF is a command, an event, a whole 802.11 management frame or
# an Ethernet frame -- so the driver is a cfg80211 driver, and there is nothing
# for mac80211 to do.
#
# That is worth being explicit about, because "wifi needs CFG80211 and MAC80211"
# is the shape of the answer for a softmac part and it is wrong here by about
# 700 KiB of code in a boot partition with roughly two and a half megabytes of
# slack. MAC80211 is refused below rather than merely not enabled, so a future
# defconfig bump cannot bring it back by dependency.
#
# =m and not =y for both, because they are only needed once the radio module
# loads and both are staged behind j36.wifi in load.order. RFKILL is asked for
# rather than inherited: cfg80211 does not select it, it says `depends on RFKILL
# || !RFKILL', which is Kconfig for "either build me modular too, or leave rfkill
# out entirely". Naming it =m alongside CFG80211=m satisfies that in the
# direction that keeps a real rfkill switch instead of the no-op stubs, which is
# what NetworkManager reads before it will bring a radio up.
config_y WIRELESS
config_m CFG80211
config_m RFKILL
# The regulatory domain this driver uses is a custom one compiled into the
# module -- 2.4 GHz at 20 dBm, the intersection of every domain it hands the
# firmware itself -- so no regulatory.db is loaded and none needs signing. This
# is off for size and not for taste: it is `default y' and it selects
# SYSTEM_DATA_VERIFICATION, which is the X.509 parser, PKCS#7 and the asymmetric
# key infrastructure, all built IN rather than modular, for a database this image
# does not ship.
config_n CFG80211_REQUIRE_SIGNED_REGDB
# Off deliberately: power save is a firmware-side decision on this part, made
# through INDICATE_PM_BSS_CONNECTED once the DTIM period is known, and a default
# from cfg80211 would be a second opinion about the same radio.
config_n CFG80211_DEFAULT_PS
# Wireless Extensions is the pre-nl80211 ioctl interface. wpa_supplicant on this
# rootfs is built with the nl80211 backend and NetworkManager talks to
# wpa_supplicant, so nothing here uses it.
config_n CFG80211_WEXT
config_n CFG80211_DEBUGFS

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig

# olddefconfig re-derives every symbol from its dependencies, so it has the last
# word on all of the above; the checks come after it, not before. MMC_MTK is
# asserted =y and not =m on purpose: a module for the driver that mounts the
# filesystem the modules live on cannot be loaded.
#
# MEDIATEK_WATCHDOG is in the list as the reboot path, not as a watchdog
# feature: it is the only thing that registers a restart handler on this SoC, and
# without it `reboot' ends in "Reboot failed -- System halted". It arrives from
# multi_v7_defconfig rather than from a config_y here, which is exactly the kind
# of symbol a future ARCH prune would take out silently. The two NLS charsets go
# with EXFAT_FS and VFAT_FS: exfat's default iocharset is utf8 and vfat's
# codepage is 437, and a mount whose charset is missing fails as surely as one
# whose filesystem is.
#
# This list is the real fix for the systemd freeze, not the NET=y above. UNIX was
# already in the enable list and had been silently dropped for being under `if
# NET' while NET was in the disable list -- a config_y whose result nothing
# checked. So everything PID 1 cannot start without is asserted here, including
# the symbols that come along by dependency: if a future prune takes NET out
# again, or turns SECCOMP off, the build fails here instead of the board
# freezing at 15 s with the reason four screens up.
#
# DEVMEM is what mfgpower reaches the SPM through.
for required in MACH_MT6592 ARM_APPENDED_DTB ARM_ATAG_DTB_COMPAT \
                FB_SIMPLE SERIAL_8250_MT6577 BLK_DEV_INITRD MODULES \
                MMC MMC_BLOCK MMC_MTK REGULATOR_FIXED_VOLTAGE \
                EXT2_FS EXT4_FS BTRFS_FS EXFAT_FS VFAT_FS \
                NLS_UTF8 NLS_CODEPAGE_437 \
                WATCHDOG WATCHDOG_CORE MEDIATEK_WATCHDOG \
                NET UNIX INET SECCOMP_FILTER NAMESPACES NET_NS PID_NS \
                CGROUPS FHANDLE INOTIFY_USER SIGNALFD TIMERFD EPOLL \
                DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_XATTR TMPFS_POSIX_ACL \
                PROC_FS PROC_SYSCTL SYSFS \
                EXT2_FS_XATTR EXT2_FS_POSIX_ACL BTRFS_FS_POSIX_ACL \
                DRM DEVMEM SOUND POWER_SUPPLY BACKLIGHT_CLASS_DEVICE WIRELESS \
                USB_SUPPORT USB_PHY GENERIC_PHY HID_SUPPORT \
                FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER \
                USB_MUSB_HOST MUSB_PIO_ONLY USB_ANNOUNCE_NEW_DEVICES; do
    grep -q "^CONFIG_${required}=y$" "$CONFIG" || \
        die "required kernel option CONFIG_${required}=y was not selected"
done

# Module compression has to be OFF, and it is asserted as an absence rather than as
# a symbol because of a Kconfig change this build tripped over. Up to 6.11 the
# choice had a MODULE_COMPRESS_NONE member and asserting =y on it worked; 6.12's
# kernel/module/Kconfig replaced that with a plain `config MODULE_COMPRESS' bool
# that the GZIP/XZ/ZSTD choice now depends on, so MODULE_COMPRESS_NONE no longer
# exists as a symbol at all and the old assertion could never pass -- it failed on
# a kernel that was configured exactly right.
#
# Why it matters either way: /init loads modules by filename with busybox insmod,
# which decompresses nothing, and modules_install with compression on renames every
# .ko to .ko.xz -- leaving both load.order files naming modules that are not there.
if grep -q "^CONFIG_MODULE_COMPRESS=y$" "$CONFIG"; then
    die "CONFIG_MODULE_COMPRESS=y came back after olddefconfig; busybox insmod cannot read a compressed .ko and load.order names uncompressed filenames"
fi

# =m and not =y, and the difference is the whole design: see the GPU section.
# lima's two tristate helpers are asserted alongside it because they are what
# `select' produces at module scope, and a lima.ko whose dependencies were built
# into vmlinux instead would still load -- it is the modules-not-built case that
# is silent, leaving /init with a load.order naming files that do not exist.
for wanted_module in DRM_LIMA DRM_SCHED DRM_GEM_SHMEM_HELPER; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; lima must be a module because the MFG power domain is gated until /init runs mfgpower"
done

# The display set, and =m matters here for a different reason: these bind real
# device tree nodes, so a =y among them takes the panel on every boot whether the
# card carries the payload or not. MTK_MMSYS is the one to watch -- it is
# `default ARCH_MEDIATEK' and comes back as =y from any olddefconfig that has
# forgotten the config_m above.
for wanted_module in DRM_MEDIATEK MTK_MMSYS PHY_MTK_MIPI_DSI; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; the mtk_drm display path must be modular so the default boot binds none of it"
done

# The ALSA core, and the reason this list exists at all is SND_PCM: it has no
# prompt, so it cannot be asked for directly, and if the SND_DUMMY select above
# ever stops reaching it the build has to fail here rather than produce an
# initramfs whose load.order names a snd-pcm.ko that was never built. All three
# =m, because that is what puts them on the BOOT partition instead of in the
# 9 MiB image.
for wanted_module in SND SND_TIMER SND_PCM SND_DUMMY; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; the ALSA core must be modular and SND_PCM has no prompt of its own, so it only arrives by select (SND_DUMMY is what selects it here)"
done

# SND_SOC is a refusal and not a preference. multi_v7_defconfig turns it on with
# dozens of SND_SOC_* codecs behind it, none of which is this board -- there is no
# MT6592 ASoC driver anywhere upstream, which is why j36_mt6592_audio.ko is a
# native ALSA card instead. =m is refused along with =y: the ASoC core registers
# nothing by itself, but a codec that came back with it can bind a node.
for refused in SND_SOC; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; MT6592 has no ASoC driver upstream and the SND_SOC_* set behind this symbol is hundreds of kilobytes of codecs for other boards"
    fi
done

# cfg80211 and rfkill, both modular, both staged behind j36.wifi. Asserted
# because CFG80211 is `default n' with no prompt-free path to it from anything
# else in this configuration -- nothing selects it here -- so a lost config_m is
# a wifi module that builds and then fails to insmod with unresolved symbols on
# the board, which is the same symptom as three other faults.
for wanted_module in CFG80211 RFKILL; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; j36_mt6592_wifi.ko links against cfg80211 and both must be modules so they land on BOOT rather than in the 9 MiB image"
done

# MAC80211 is a refusal, and this is the one that is easy to get wrong in the
# helpful direction. The MT6592 CONSYS radio is FULLMAC -- its firmware owns the
# MAC, and j36_mt6592_wifi.ko is a cfg80211 driver with no ieee80211_hw anywhere
# in it -- so mac80211 has nothing to do here and would be some 700 KiB of it.
# =m is refused along with =y for the same reason DRM_SIMPLEDRM is: /init loads
# modules by filename from a text file, and a stray one is a loadable one.
for refused in MAC80211; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; the MT6592 CONSYS part is fullmac and its driver never registers an ieee80211_hw"
    fi
done

# Off on purpose, and worth failing over. WLAN is the in-tree driver menu, and
# this board's radio driver is out of tree -- everything behind that symbol is
# hardware that is not here. BT is the Bluetooth stack, which nothing on this
# image talks to yet.
for refused in WLAN BT; do
    if grep -q "^CONFIG_${refused}=y$" "$CONFIG"; then
        die "CONFIG_${refused}=y came back after olddefconfig; the wifi driver is out of tree and nothing here needs the in-tree menus"
    fi
done

# DRM_SIMPLEDRM binds `simple-framebuffer' -- the same node FB_SIMPLE is driving
# and the only working display on this board -- and evicts the incumbent when it
# does. Either value is a refusal, including =m, because /init loads modules by
# filename from a text file on a FAT partition and a stray one would be loadable.
for refused in DRM_SIMPLEDRM; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; it claims the same simple-framebuffer node as FB_SIMPLE and would take the panel"
    fi
done

# Neither block is on this SoC. DRM_MEDIATEK_HDMI additionally `select's
# SND_SOC_HDMI_CODEC, which is how a display driver quietly turns into an audio
# subsystem in an image with a fixed partition and 2.5 MiB of slack.
for refused in DRM_MEDIATEK_HDMI DRM_MEDIATEK_DP; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; MT6592 has neither block"
    fi
done

# The USB set, and the two entries that are not obvious are the load-bearing ones.
#
# NOP_USB_XCEIV has no relationship to this board's PHY and is required anyway:
# USB_MUSB_MEDIATEK's Kconfig line is `depends on NOP_USB_XCEIV', and mediatek.c
# calls usb_phy_generic_register() unconditionally in probe. Without it the glue
# does not build at all, and the failure reads as "USB_MUSB_MEDIATEK was not
# selected" with nothing to say why -- so it is asserted next to the thing that
# needs it.
#
# USB_ROLE_SWITCH is the same shape: `select USB_ROLE_SWITCH' in the same Kconfig
# entry, roles.ko at module scope, and mtk_musb_probe registers a switch even in
# host mode. It is in the load order for that reason and not because anything
# here changes role.
#
# DRM_UDL is asserted here rather than with the display set because the reason it
# can exist is USB=m. If USB ever goes back to =y this assertion still passes; if
# USB goes away, this is the line that says the external display went with it.
for wanted_module in USB USB_COMMON USB_MUSB_HDRC USB_MUSB_MEDIATEK \
                     NOP_USB_XCEIV USB_ROLE_SWITCH HID USB_HID HID_GENERIC \
                     DRM_UDL; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; the USB stack must be modular because an APB access to the clock-gated MUSB window hangs the bus, so nothing may probe before /init has ungated PERI"
done

# The mass-storage half, asserted as its own list because it fails as a set and in
# a way that is hard to read on the board: SCSI without BLK_DEV_SD enumerates a
# target and creates no /dev/sda, BLK_DEV_SD without USB_STORAGE creates the upper
# layer with no host adapter under it, and either arrangement produces a port that
# lights the stick's LED and mounts nothing.  =m for all four for the reason in the
# storage section -- the 9 MiB budget, and the same containment as the rest of the
# USB payload.
#
# NTFS3_FS is in this list rather than with the filesystems above because it is
# part of the same payload and shares its fate: it is staged into j36/usb/ and
# insmodded by run_usb, so a build that produced the disk stack without it would
# mount every plugged-in disk except the ones people actually carry.
for wanted_module in SCSI BLK_DEV_SD USB_STORAGE NTFS3_FS; do
    grep -q "^CONFIG_${wanted_module}=m$" "$CONFIG" || \
        die "CONFIG_${wanted_module}=m was not selected; external USB disks need scsi_mod, sd_mod, usb-storage and ntfs3 together, all modular, staged into j36/usb/"
done

# No host controller driver may come back, because there is no host controller on
# this SoC to drive -- MT6592 has one MUSB core and nothing else. =y and =m are
# both refusals: /init loads modules by filename from a text file, so a stray one
# is loadable, and a built-in one binds at boot.
#
# USB_GADGET is refused for a different reason: it is not absent hardware, it is
# the other half of the same core. Turning it on changes MUSB's driver mode out
# of host-only, and the board would then wait for a role switch that nothing
# performs while the powered hub sits there unenumerated.
for refused in USB_EHCI_HCD USB_OHCI_HCD USB_XHCI_HCD USB_DWC2 USB_DWC3 \
               USB_CHIPIDEA USB_GADGET; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; MT6592 has exactly one USB core (MUSB at 0x11200000) and it is driven host-only"
    fi
done

# The storage refusals, kept apart from the list above because the reasons are
# different in kind.  ATA is absent hardware.  USB_UAS is a choice -- see the USB
# section for why BOT is the protocol on a PIO-only controller.  The three upper
# layers are drivers for device classes nobody is going to plug into a handheld,
# and CHR_DEV_SG additionally exports a raw SCSI command channel.
for refused in ATA USB_UAS BLK_DEV_SR CHR_DEV_ST CHR_DEV_SG; do
    if grep -qE "^CONFIG_${refused}=(y|m)$" "$CONFIG"; then
        die "CONFIG_${refused} came back after olddefconfig; SCSI is on for USB mass storage only and the rest of that menu stays off"
    fi
done

# And the NTFS compatibility shim on its own, because it fails in a way none of
# the lists above would name.  It is not a driver and it is not absent hardware:
# it is a bare `select NTFS3_FS' left behind for configs that still spell the old
# symbol, and multi_v7_defconfig is one of those.  At =y it drags ntfs3 into
# vmlinux -- past the 9 MiB BOOTIMG budget's back, and out of the j36/usb/ payload
# where run_usb expects to find it.  The assertion is here so that a kernel bump
# putting it back cannot silently undo the config_n in the storage section.
if grep -qE '^CONFIG_NTFS_FS=(y|m)$' "$CONFIG"; then
    die "CONFIG_NTFS_FS came back after olddefconfig; it is a compatibility alias that selects NTFS3_FS and its =y from multi_v7_defconfig is what forces ntfs3 built-in"
fi

# Print the whole DRM set rather than trusting the assertions above to have named
# everything that matters. This is the line to read when a kernel bump changes
# what `select' pulls in. MTK_ and PHY_MTK_ are in the same line because the
# display path spans three directories and its Kconfig symbols do not share a
# prefix -- mediatek-drm is useless without mtk-mmsys, mtk-mutex and the MIPI-TX
# PHY, and none of those three is a DRM_ symbol.
log "DRM configuration: $(grep -E '^CONFIG_(DRM|MTK_|PHY_MTK_).*=(y|m)$' "$CONFIG" | tr '\n' ' ')"

# The same for sound, and for the same reason: the prune above is an allowlist, so
# this line is where anything a kernel bump adds behind SND shows up. A short line
# -- SOUND=y and four =m -- is the expected shape; a long one means a codec came
# back and the allowlist needs a look.
log "Sound configuration: $(grep -E '^CONFIG_(SOUND|SND)[A-Z0-9_]*=(y|m)$' "$CONFIG" | tr '\n' ' ')"

# And for the SCSI menu, which is the newest allowlist in this file and therefore
# the one most likely to be wrong.  The expected shape is short: SCSI=m, its two
# hidden helpers, SCSI_PROC_FS and BLK_DEV_SD=m.  Anything else on this line is a
# driver for hardware that is not in this handheld.
log "Storage configuration: $(grep -E '^CONFIG_(SCSI|BLK_DEV_SD|BLK_DEV_SR|CHR_DEV_|USB_STORAGE|NTFS)[A-Z0-9_]*=(y|m)$' "$CONFIG" | tr '\n' ' ')"

log "Building the incremental ARMv7 kernel and its symbol table"
export CCACHE_DIR="${CCACHE_DIR:-$ROOT/MixOSBuild_ccache}"
mkdir -p "$CCACHE_DIR"
if [[ -d /usr/lib/ccache ]]; then export PATH="/usr/lib/ccache:$PATH"; fi
# `modules' is not optional here even though this configuration selects almost no
# in-tree modules.  It is the target that runs modpost over vmlinux.o and writes
# $KERNEL_OUT/Module.symvers, and the out-of-tree J36 input adapter below is
# resolved against that file.  Building only zImage leaves it absent, and modpost
# then reports every core symbol the adapter uses -- __platform_driver_register,
# devm_kmalloc, memset, __aeabi_unwind_cpp_pr0 -- as "undefined!".
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- -j"$JOBS" zImage modules

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

log "Building the out-of-tree J36 modules: the input adapter, the panel, the AFE, the USB PHY, the PMIC, the backlight, the framebuffer exporter and the radio"
mkdir -p "$MODULE_SRC"
rsync -a --delete "$ROOT/device/j36-ultra/linux/" "$MODULE_SRC/"
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- M="$MODULE_SRC" -j"$JOBS" modules
KERNEL_RELEASE="$(make -s -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- kernelrelease)"
MODULE="$MODULE_SRC/j36_mt6592_input.ko"
[[ -s "$MODULE" ]] || die "input module was not produced"
verify_arm_elf "$MODULE" "the input module"
# The panel module is not part of the initramfs and not part of a default boot; it
# is staged with the rest of the mtkdrm payload further down.  It is built here
# because it is in the same M= directory, and checked here because a missing .ko
# would otherwise surface as a load.order line that names a file that is not there.
PANEL_MODULE="$MODULE_SRC/j36_jd9365_panel.ko"
[[ -s "$PANEL_MODULE" ]] || die "panel module was not produced"
verify_arm_elf "$PANEL_MODULE" "the panel module"
# Likewise the AFE adapter: staged with the audio payload, never in the initramfs.
# It is the one out-of-tree module that links against symbols the kernel only
# exports when the ALSA core is configured, so a missing .ko here usually means
# the sound section above lost SND rather than that this file failed to compile.
AUDIO_MODULE="$MODULE_SRC/j36_mt6592_audio.ko"
[[ -s "$AUDIO_MODULE" ]] || die "audio module was not produced"
verify_arm_elf "$AUDIO_MODULE" "the audio module"
# And the USB PHY, staged with the usb payload.  It loads before musb_hdrc,
# because it is the only hook that runs before musb touches 0x11200000 -- and an
# APB read of that window while PERI still gates it hangs the bus rather than
# faulting.  A missing .ko here would put musb_hdrc first in load.order, so check
# it at build time.  usbcore now sorts ahead of both (the PHY calls
# usb_for_each_dev to tell a charger from a device), which changes nothing: the
# window belongs to musb and usbcore does not touch it.
USB_PHY_MODULE="$MODULE_SRC/j36_mt6592_usb_phy.ko"
[[ -s "$USB_PHY_MODULE" ]] || die "USB PHY module was not produced"
verify_arm_elf "$USB_PHY_MODULE" "the USB PHY module"
# And the PMIC, staged as its own power payload.  It is the only module here that
# writes registers which outlive a reboot -- the MT6323 charger bank keeps its
# settings across a warm reset -- which is why it lives behind its own j36.power
# word rather than in the initramfs, and why it is worth failing the build over a
# missing .ko rather than discovering it as a silent absence of
# /sys/class/power_supply/battery on a board that then never reports a charge.
PMIC_MODULE="$MODULE_SRC/j36_mt6592_pmic.ko"
[[ -s "$PMIC_MODULE" ]] || die "PMIC module was not produced"
verify_arm_elf "$PMIC_MODULE" "the PMIC module"
# And the backlight, which rides along in the same power payload.  It is the only
# module here that the user can see working without reading a log: it is what puts
# /sys/class/backlight/j36-backlight on the board, and therefore what the
# dashboard's Display page writes to.  Worth failing the build over for the same
# reason as the PMIC -- a missing .ko is a load.order line naming a file that is
# not there, discovered as a brightness slider that does nothing.
BACKLIGHT_MODULE="$MODULE_SRC/j36_mt6592_backlight.ko"
[[ -s "$BACKLIGHT_MODULE" ]] || die "backlight module was not produced"
verify_arm_elf "$BACKLIGHT_MODULE" "the backlight module"
# And the dma-buf exporter over the LK's framebuffer carveout, staged with the
# mtkdrm payload.  It is the only module here that imports a symbol namespace --
# dma_buf_export and friends are EXPORT_SYMBOL_NS_GPL into DMA_BUF -- so if this
# line ever fires, look above it for a modpost "uses symbol ... but does not
# import it" rather than for a compile error.  A missing .ko is discovered on the
# board as `eglprobe -z' saying /dev/j36fb does not exist, which reads like the
# device tree is wrong when it is not.
FBMEM_MODULE="$MODULE_SRC/j36_fbmem.ko"
[[ -s "$FBMEM_MODULE" ]] || die "framebuffer dma-buf module was not produced"
verify_arm_elf "$FBMEM_MODULE" "the framebuffer dma-buf module"
# And the radio, staged as its own wifi payload.  It is the only module here built
# from more than one translation unit -- main, consys, wmt, hif, cmd, net -- and
# the only one that links against another of these modules rather than against
# the kernel alone: the PMIC exports the two wrapper accessors it needs, because
# the WACS2 bridge has one owner.  Both facts are why a missing .ko is worth
# failing over and not worth guessing about.  If this line ever fires, read the
# modpost output above it: an undefined j36_pmic_pwrap_* is the PMIC having been
# dropped from this same M= directory, not the radio failing to compile.
WIFI_MODULE="$MODULE_SRC/j36_mt6592_wifi.ko"
[[ -s "$WIFI_MODULE" ]] || die "Wi-Fi module was not produced"
verify_arm_elf "$WIFI_MODULE" "the Wi-Fi module"
fits_in "$DTB_OUT/mt6592-j36-ultra.dtb" $((0x00040000)) "the device tree"

if [[ ! -d "$BUSYBOX_SRC/.git" ]]; then
    log "Cloning BusyBox $BUSYBOX_BRANCH once for the ARM bring-up initramfs"
    git clone --depth=1 --branch "$BUSYBOX_BRANCH" "$BUSYBOX_URL" "$BUSYBOX_SRC"
fi
# Two helpers instead of a bare sed, because a symbol can appear in .config in
# either of two forms -- `CONFIG_X=y' or `# CONFIG_X is not set' -- and a sed that
# only knows one of them silently does nothing when defconfig flips a default.
bb_enable() {
    local sym="$1" cfg="$BUSYBOX_SRC/.config"
    if grep -q "^# $sym is not set\$" "$cfg"; then
        sed -i "s|^# $sym is not set\$|$sym=y|" "$cfg"
    elif ! grep -q "^$sym=y\$" "$cfg"; then
        printf '%s=y\n' "$sym" >>"$cfg"
    fi
}
bb_disable() {
    local sym="$1" cfg="$BUSYBOX_SRC/.config"
    sed -i "s|^$sym=y\$|# $sym is not set|" "$cfg"
}

# ── EVERY EXTERNAL COMMAND /init RUNS, IN EXACTLY ONE LIST ───────────────────
#
# This used to be two lists -- a set of CONFIG_ symbols asserted during the
# BusyBox build, and a set of names symlinked to busybox in the initramfs -- and
# they drifted, which is invisible until the device says so. What it said was
#
#   /init: line 346: ln: not found
#
# and the consequence was not a missing symlink. /init's GL staging runs `cp' and
# `ln'; neither was in either list, so the payload directory was created, the
# LD_LIBRARY_PATH pointing at it was written, and the directory stayed empty. A GL
# client's DT_NEEDED is the bare name `libEGL.so', so the loader missed the empty
# path and resolved it in /usr/lib, where the shared rootfs has pointed that name at
# the RK3326's libMali.so. That blob is Tag_CPU_arch v8 -- ARMv8-A -- and this SoC is
# a Cortex-A7. The client died on SIGILL, status 132, before main(), six times, until
# systemd gave up on the restart counter.
#
# `grep' was missing too, and had been all along: /init asks /proc/consoles which
# console the kernel chose, under a 2>/dev/null that hid the failure, so
# panel_is_console stayed 0 and every say() line went to stdout AND /dev/tty1 --
# the doubled output on the panel was this, not a driver.
#
# So: one list, asserted and symlinked from the same array. Adding a command to
# /init without adding it here now fails the build instead of the boot.
# Every name /init calls has to be in here.  This busybox is built without
# FEATURE_SH_STANDALONE, so ash does not look inside the binary for an applet it
# cannot find on PATH -- the applet being compiled in is not enough, the symlink
# is what makes it callable.  chmod was the one that was missing, and it failed
# the only way a missing applet can: "/init: line NNN: chmod: not found", twice,
# which is why the probe log stayed unwritable.
# rmdir is in this list because /init runs it, and it was missing: mount_card clears
# /run/j36/card out of the way before replacing it with a symlink to /home/virtua.
# A missing applet is not a no-op here -- `ln -s target dir' with dir still present
# creates the link INSIDE it,
# so the dashboard's Files page would open on a directory containing one dangling
# symlink instead of on the card.  The assertion loop below is what would have caught
# this, and it can only catch what is named here.
# tar and gunzip are here for stage_from_boot(): the only channel that can update an
# already-flashed card from a macOS workstation is the FAT BOOT partition, and a tree
# copied there by hand loses every SONAME symlink and every execute bit.  A tarball
# does not, so the tarball is what crosses, and this initramfs unpacks it onto the OS
# partition -- on Linux, as root, where symlinks and modes still mean something.
# sed and printf were the next two to be missing, and they were missing in the quiet
# way: both are inside `$( )', so a "not found" goes to stderr and the substitution
# yields the empty string instead of failing anything.  What that cost was the
# build-identity check -- setup_dash reads /etc/j36-build with sed to learn which
# mixdash this boot image expects, got nothing, and skipped passing MIXDASH_EXPECT,
# so a stale dashboard on the OS partition looked exactly like a current one.  ash
# has a printf builtin and probably always resolved it; the symlink costs nothing
# and takes the question away.
INIT_APPLETS=(sh mount umount mkdir mknod cat cp ln ls tr grep echo sleep dmesg
              insmod hexdump setsid cttyhack switch_root sync poweroff reboot
              uname chmod rmdir tar gunzip sed printf)

# Most applets are CONFIG_<applet in caps>; three are not, and guessing would
# assert a symbol that does not exist, which greps false and dies on a correct
# config. sh is provided by ash, and halt/poweroff/reboot are one applet.
bb_applet_symbol() {
    case "$1" in
        sh)              printf 'CONFIG_ASH\n' ;;
        poweroff|reboot) printf 'CONFIG_HALT\n' ;;
        *)               printf 'CONFIG_%s\n' "$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')" ;;
    esac
}

if [[ ! -x "$BUSYBOX_SRC/busybox" || "${J36_REBUILD_BUSYBOX:-0}" == 1 ]]; then
    log "Building the static ARMv7 BusyBox once"
    make -C "$BUSYBOX_SRC" distclean
    make -C "$BUSYBOX_SRC" defconfig
    bb_enable CONFIG_STATIC

    # ── APPLETS WHOSE KERNEL INTERFACE NO LONGER EXISTS ──────────────────────
    #
    # `tc' is compiled against the target libc's copy of <linux/pkt_sched.h>, and
    # Linux 6.3 retired the CBQ qdisc: TCA_CBQ_MAX, TCA_CBQ_RATE, TCA_CBQ_LSSOPT,
    # struct tc_cbq_lssopt, struct tc_cbq_wrropt, TCF_CBQ_LSS_BOUNDED and the rest
    # were deleted from the uapi header outright. BusyBox 1.36 still prints CBQ
    # options, so networking/tc.c cannot compile at all on any modern header set,
    # which is why the build stopped at `TCA_CBQ_MAX undeclared'. Not a toolchain
    # fault and not fixable by a flag: the declarations are gone.
    #
    # Disabled rather than patched. Traffic control has no part in a bring-up
    # initramfs whose whole job is to insmod one input driver and hand over a
    # shell, so carrying a local patch against upstream's copy of a utility we do
    # not run would be maintaining a fork for nothing.
    #
    # If a later busybox breaks this way on another applet, add it here with the
    # same note -- what was removed, and which kernel removed it.
    bb_disable CONFIG_TC
    bb_disable CONFIG_FEATURE_TC_INGRESS

    # /init now hands the machine over to the rootfs on the card, which needs the
    # applet that does it and the one that backs out of a candidate partition
    # that turned out not to be a root filesystem.
    bb_enable CONFIG_SWITCH_ROOT
    bb_enable CONFIG_UMOUNT
    bb_enable CONFIG_SYNC

    # And the pair that unpacks sd-root.tar.gz from the BOOT partition onto the OS
    # partition.  gunzip rather than tar -z: seamless decompression is a separate
    # busybox feature symbol, and a pipe needs no feature at all.
    bb_enable CONFIG_TAR
    bb_enable CONFIG_GUNZIP

    yes '' | make -C "$BUSYBOX_SRC" oldconfig >/dev/null || true

    # oldconfig has the last word on all of the above -- it re-derives every
    # symbol from its dependencies -- so the checks come after it, not before.
    grep -q '^CONFIG_STATIC=y$' "$BUSYBOX_SRC/.config" || \
        die "busybox CONFIG_STATIC did not survive oldconfig; the initramfs needs a static binary"
    ! grep -q '^CONFIG_TC=y$' "$BUSYBOX_SRC/.config" || \
        die "busybox CONFIG_TC is still on and it cannot build against Linux 6.3+ headers"

    # The applets /init invokes are asserted after this block rather than inside
    # it, because a cached BusyBox skips this block entirely and the assertion has
    # to hold on every run.

    make -C "$BUSYBOX_SRC" CROSS_COMPILE=arm-linux-gnueabihf- -j"$JOBS"
fi
BUSYBOX="$BUSYBOX_SRC/busybox"
[[ -x "$BUSYBOX" ]] || die "static ARM BusyBox was not produced"
# The kernel and the module are checked for their machine type; this was not, and
# it is the one binary in the initramfs that the SoC executes first. A busybox
# built for the host is a perfectly valid ELF that this board cannot run, and the
# symptom on the device would be an unhelpful `/init: not found' at hand-over.
verify_arm_elf "$BUSYBOX" "the initramfs BusyBox"

# And every applet /init runs has to be in that binary. This is outside the build
# block on purpose: J36_REBUILD_BUSYBOX defaults to 0, so on all but the first run
# the block above does not execute, and an assertion that only fires when BusyBox
# is compiled is an assertion that never fires. The .config survives in the cached
# source tree, which is what makes checking a cached build possible at all.
#
# It cannot be checked by running the binary: that busybox is static ARMv7 and this
# builder is not ARM.
for applet in "${INIT_APPLETS[@]}"; do
    sym="$(bb_applet_symbol "$applet")"
    grep -q "^$sym=y\$" "$BUSYBOX_SRC/.config" || \
        die "busybox $sym is off but /init runs \`$applet'; rebuild with J36_REBUILD_BUSYBOX=1"
done
# Not an applet, so not in the list: it decides which shell CONFIG_ASH installs as
# /bin/sh, and /init is #!/bin/sh.
grep -q '^CONFIG_SH_IS_ASH=y$' "$BUSYBOX_SRC/.config" || \
    die "busybox CONFIG_SH_IS_ASH is off; /init is #!/bin/sh"
# Nor is this one, and without it `mount -o ro' silently becomes a mount attempt with
# a filesystem type of "ro": busybox parses -o flag words only when
# FEATURE_MOUNT_FLAGS is on.  Every probe /init makes -- the rootfs scan, BOOT, the
# payload search -- mounts read-only first and remounts rw after, so this is not one
# corner of the boot but the whole of how it looks at a card.
grep -q '^CONFIG_FEATURE_MOUNT_FLAGS=y$' "$BUSYBOX_SRC/.config" || \
    die "busybox CONFIG_FEATURE_MOUNT_FLAGS is off; /init needs \`mount -o ro'"

rm -rf "$INITROOT"
mkdir -p "$INITROOT"/{bin,dev,etc,lib/modules/$KERNEL_RELEASE/extra,proc,root,sbin,sys,tmp}
cp "$BUSYBOX" "$INITROOT/bin/busybox"
chmod 0755 "$INITROOT/bin/busybox"
for applet in "${INIT_APPLETS[@]}"; do
    ln -sf busybox "$INITROOT/bin/$applet"
done
cp "$MODULE" "$INITROOT/lib/modules/$KERNEL_RELEASE/extra/"

# ── the boot splash ───────────────────────────────────────────────────────────
#
# Two artifacts go into the initramfs: a static ARM binary that draws, and the
# wallpaper it draws, already decoded.
#
# DECODED HERE AND NOT THERE, because /splash.mixspl is read before switch_root,
# where there is no ld.so, no /lib and therefore no libjpeg -- the splash is one
# of the few things on this board that genuinely cannot link against anything.
# The decoder is tools/jpeg2raw.py: pure Python, no imports beyond the standard
# library, vendored rather than `pip install pillow' for a reason that is easy to
# miss.  The one-time apt install in this script is guarded by $WORK/.deps-installed,
# so a dependency added to that list is a dependency that never reaches a VM which
# has already built once -- the build would then fail on exactly the machines that
# have been working, which is the worst possible failure mode.  Forty kilobytes of
# baseline JPEG decoder costs nothing and cannot do that.
#
# Non-fatal, like fbdoom and mfgpower: a splash is decoration, and the boot has
# to survive its absence.  /init tests for both files before running anything.
SPLASH_SRC="$ROOT/device/j36-ultra/tools/mixsplash.c"
SPLASH_JPEG="$ROOT/device/j36-ultra/resources/MixOS.jpg"
SPLASH_TOOL="$ROOT/device/j36-ultra/tools/jpeg2raw.py"
SPLASH_OK=0

build_mixsplash() {
    local out="$WORK/mixsplash" header

    [[ -f "$SPLASH_SRC"  ]] || { log "splash: $SPLASH_SRC is missing";  return 1; }
    [[ -f "$SPLASH_JPEG" ]] || { log "splash: $SPLASH_JPEG is missing"; return 1; }
    [[ -f "$SPLASH_TOOL" ]] || { log "splash: $SPLASH_TOOL is missing"; return 1; }

    # -static for the same reason Doom and mfgpower are.  No -lm: the two
    # transcendentals it needs are polynomials in the source, so that the boot
    # path carries no link dependency for two calls a frame.
    arm-linux-gnueabihf-gcc -O2 -std=gnu11 -Wall -Wextra -static \
        -o "$out" "$SPLASH_SRC" || return 1

    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "splash: mixsplash is not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "splash: mixsplash is not an ARM ELF"; return 1; }
    if grep -q 'NEEDED' <<<"$header"; then
        log "splash: mixsplash wants shared libraries and the initramfs has none"
        return 1
    fi

    # 640x480 x8r8g8b8 is what the LK leaves lit and what the panel is, and the
    # picture is already exactly that -- so this is a decode and not a resample.
    # The size is passed anyway: a later panel revision changes one number here
    # rather than shipping a wallpaper that is silently letterboxed.
    python3 "$SPLASH_TOOL" "$SPLASH_JPEG" "$INITROOT/splash.mixspl" 640 480 || return 1

    install -m 0755 "$out" "$INITROOT/bin/mixsplash"
    log "splash: mixsplash is $(stat -c %s "$out") bytes static ARM," \
        "wallpaper $(stat -c %s "$INITROOT/splash.mixspl") bytes"
    return 0
}

if build_mixsplash; then
    SPLASH_OK=1
else
    log "splash: not built; the boot will narrate itself in text as it used to"
    rm -f "$INITROOT/bin/mixsplash" "$INITROOT/splash.mixspl"
fi

# ── the build identity, and why there is one ──────────────────────────────────
#
# The two halves of this build are updated by different means and can be a version
# apart.  The boot image -- kernel, this initramfs, /init -- goes onto the vfat BOOT
# partition, which is the one partition a Mac can write; /opt/mixos goes onto the ext2
# OS partition, which a Mac cannot even mount, and reaches it either by being injected
# into the image at build time or by /init unpacking sd-root.tar.gz from BOOT.
#
# So "did the tarball actually replace the binaries" is a real question with no way to
# ask it, and a boot that answers with the OLD dashboard looks exactly like a boot that
# answers with the new one and fails.  Both halves therefore carry the same twelve
# characters -- the hash of the dashboard's sources -- /init states what it expects,
# mixdash states what it is, and one line of the trace says whether they agree.
#
# Computed here rather than in build_mixdash because the initramfs is packed long
# before the dashboard is compiled, and a hash of source files needs no compiler.
MIXDASH_SRC="$ROOT/device/j36-ultra/tools/mixdash"
mixdash_source_id() {
    cat "$MIXDASH_SRC"/*.cpp "$MIXDASH_SRC"/*.h "$MIXDASH_SRC"/mixdash.pro 2>/dev/null \
        | sha256sum | awk '{print $1}'
}
MIXDASH_SOURCE_ID="$(mixdash_source_id)"
MIXDASH_BUILD_ID="${MIXDASH_SOURCE_ID:0:12}"
log "mixdash: build id $MIXDASH_BUILD_ID (sha256 of its sources)"

cat > "$INITROOT/init" <<'INIT'
#!/bin/busybox sh
export PATH=/bin:/sbin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

# ── Say it on the panel, not only on the cable ────────────────────────────────
#
# /dev/console is whichever console= came LAST on the kernel command line, so
# with `console=tty0 console=ttyS0,115200n8' every word of this script goes to
# the UART and the panel shows a bare blinking cursor.  say() writes to both, and
# the interactive shell at the bottom is exec'd on /dev/tty1 explicitly for the
# same reason -- `exec setsid cttyhack sh' alone inherits /dev/console and puts
# the prompt on a serial port that may have nothing plugged into it.
#
# boot.conf now puts console=tty0 last, so the panel usually IS /dev/console and
# writing to both would double every line on a 30-line screen.  Ask the kernel
# which console it chose rather than re-parsing the command line: /proc/consoles
# flags the one /dev/console maps to with a C.
panel_is_console=0
if grep -q '^tty0 .*C' /proc/consoles 2>/dev/null; then panel_is_console=1; fi
# Declared before say() reads it, and that ordering is the whole reason it is up
# here rather than in the splash block below: ash evaluates a function body when
# it is called, so an unset variable would be a silent empty string in the first
# comparison rather than an error anyone would notice.
splash_on=0
say() {
    echo "$@"
    if [ "$panel_is_console" = 0 ] && [ -c /dev/tty1 ]; then echo "$@" >/dev/tty1; fi
    # With console=tty0 last, the line above went to the panel and nowhere else --
    # and while the splash owns the panel, KD_GRAPHICS means it went nowhere at
    # all.  It is still in the VT's scrollback and reappears the moment the splash
    # gives the console back, which covers every failure; what it does not cover is
    # somebody watching a working boot on the cable.  So while the picture is up,
    # the cable gets a copy.
    if [ "$splash_on" = 1 ] && [ "$panel_is_console" = 1 ] && [ -c /dev/ttyS0 ]; then
        echo "$@" >/dev/ttyS0
    fi
    return 0
}

# And the same for a file's contents.  `cat /proc/interrupts' goes to stdout
# only, which is the console, which is the serial port -- exactly the output that
# is invisible on a board being debugged from the panel.
show() {
    while IFS= read -r line; do say "  $line"; done < "$1"
    return 0
}

# ── the splash, and the four helpers that talk to it ──────────────────────────
#
# Started here, at the top, because "where is the boot" is only worth answering
# while the boot is still going on.  Everything below still calls say(), so the
# serial console gets the same story in the same order it always did; stage()
# just also puts the headline on the panel.
#
# THE CHANNEL IS A FILE THAT IS APPENDED TO, and that is a safety property rather
# than a style: `echo x > fifo' blocks until something reads it, so a splash that
# died -- no /dev/fb0, killed, panel-less board -- would hang the boot at the
# next stage message, on a device whose only recovery is taking the card out.
# `>>' on a regular file cannot block, cannot fail for want of a reader, and
# cannot wedge anything.  It lives in /dev because /dev is `mount --move'd across
# switch_root, so the same path keeps working in the rootfs.
#
# The command line is read here rather than in the option loop below, because
# that loop runs after a module load and two mounts, and the whole point of this
# is to be on the screen before any of that.
splash_chan=/dev/.mixsplash
case " $(cat /proc/cmdline 2>/dev/null) " in
    *" j36.splash=0 "*|*" nosplash "*) want_splash=0 ;;
    *)                                 want_splash=1 ;;
esac
if [ "$want_splash" = 1 ] && [ -x /bin/mixsplash ] && \
   [ -f /splash.mixspl ] && [ -e /dev/fb0 ]; then
    : > "$splash_chan"
    /bin/mixsplash -i /splash.mixspl -f "$splash_chan" -s "Starting MixOS" &
    splash_on=1
fi

# stage() is say() plus the headline; the panel gets the short version and the
# cable gets everything, which is the right split for a 640x480 screen.
stage() {
    say "$1"
    if [ "$splash_on" = 1 ]; then echo "stage:$1" >> "$splash_chan"; fi
    return 0
}
detail() {
    if [ "$splash_on" = 1 ]; then echo "detail:$1" >> "$splash_chan"; fi
    return 0
}
progress() {
    if [ "$splash_on" = 1 ]; then echo "progress:$1" >> "$splash_chan"; fi
    return 0
}
# Told rather than killed: `kill' is a job-control builtin in BusyBox ash and
# this initramfs does not carry the applet, and the splash puts the text console
# back by itself on the way out -- which is the entire reason to stop it here.
# The VT redraws from its own scrollback when KD_TEXT is restored, so everything
# said while the picture was up is still on the panel afterwards.
# `abort' and not `quit', and the difference is the whole reason both exist: the
# hand-over below is sent before switch_root, and switch_root can fail.  A `quit'
# would honour that hand-over and leave the panel showing a picture with the
# post-mortem invisible behind it; `abort' gives the text console back whatever
# it was told earlier.
splash_off() {
    if [ "$splash_on" = 1 ]; then echo "abort" >> "$splash_chan"; splash_on=0; fi
    return 0
}

say ""
say "J36 Ultra ARMv7 bring-up initramfs"
say "Display: the LK's framebuffer on /dev/fb0 until something opens /dev/dri/card0."
progress 4
insmod /lib/modules/*/extra/j36_mt6592_input.ko || say "input module load failed"

# ── Hand over to the rootfs on the card, if there is one ─────────────────────
#
# root= is treated as a hint and nothing more.  Partition numbering follows
# whichever MMC host attached first, and a card can be repartitioned between
# boots, so every candidate is proved by mounting it and looking for /sbin/init
# before the machine is handed to it.  Failing that we stay here with a shell,
# which is strictly better than the kernel panicking on a root= it cannot honour.
root_hint=""
want_lima=0
want_mtkdrm=0
want_gl=0
gl_debug=0
want_dash=0
want_audio=0
audio_speaker=0
want_usb=0
# 1, 0 or auto, passed straight through to the PHY's vbus= parameter as 1, 0 or
# -1.  1 is the default because this handheld has two connectors: the DC inlet
# charges and the OTG port carries data, so the port never has to give up its 5 V
# for a charger and there is nothing to measure.  See run_usb.
usb_vbus=1
want_power=0
power_charge=1
want_wifi=0
# The only j36 word that defaults to ON, and the reason is that it is the word you
# cannot ask for after the fact: it writes the file that says why the boot went
# wrong, and by the time somebody wants that file the boot has already gone wrong
# and the card is in another machine.  It loads nothing and it holds the FAT open
# for about a second per pass -- see setup_logdump -- so the cost of it being on
# by default is smaller than the cost of one boot where it was not.
want_log=1
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        j36.audio|j36.audio=1)
            want_audio=1
            ;;
        # The class-D amp, and it is a separate word because it is the only thing
        # in this payload that can switch the board off.  The amp hangs off VBAT,
        # which on this PMIC is the system node: with no cell fitted VBAT is held
        # up only by the charger's current source, and the amp at output pulls it
        # under the undervoltage lockout.
        #
        # It is in the shipped bootargs because a handheld whose sound only comes
        # out of a jack is half a handheld, and because the word is no longer the
        # only way to reach the amp: it seeds a "Speaker Amp" mixer control, so a
        # board that cannot hold the rail says `amixer -c0 set "Speaker Amp" off'
        # and keeps its card, and one that can is not asking anybody to edit
        # bootargs on a vfat partition first.  Drop back to j36.audio=1 to boot
        # with the amp off -- that is not silence, it is the headphone jack, which
        # comes up either way.
        j36.audio=speaker)
            want_audio=1
            audio_speaker=1
            ;;
        j36.lima|j36.lima=1)
            want_lima=1
            ;;
        j36.mtkdrm|j36.mtkdrm=1)
            want_mtkdrm=1
            ;;
        # USB host: keyboards, mice, hubs, and the DisplayLink adapter, which is
        # one word and not two because they are one stack -- a USB->HDMI adapter
        # is a USB device, so udl loads off the same port and the same core as
        # the mouse.  Behind a word like the rest because the first APB access to
        # the MUSB window is the thing that could hang this board, and a card
        # that ends up wedged by it is fixed by editing boot.conf on any machine
        # that reads SD cards.
        j36.usb|j36.usb=1)
            want_usb=1
            ;;
        # The same stack with the 5 V left alone.  VBUS on this board is a GPIO
        # -- pad 15, active high, read out of the stock Android kernel's
        # mt_usb_set_vbus() -- and it is a boost off VBAT, which on this PMIC is
        # the system node, the same rail the class-D amp above can pull under
        # the undervoltage lockout.  So: use this word with a self-powered hub,
        # which brings its own 5 V and does not want the port's, and use it on a
        # board with no cell fitted, where a bus-powered device is a load VBAT
        # cannot carry.  The pad is never driven, so nothing bus-powered will come
        # up; charging is unaffected either way, because the charger comes in on
        # the DC inlet and not on this port.
        j36.usb=novbus)
            want_usb=1
            usb_vbus=0
            ;;
        # The default, spelled out.  Always a host, DRVVBUS high for the whole
        # uptime, and that is right here: the DC inlet charges and this port
        # carries the data, so the port has nothing to arbitrate and every reason
        # to hold still.  The word is kept so a boot.conf can say what it means
        # rather than rely on a default staying put.
        j36.usb=vbus)
            want_usb=1
            usb_vbus=1
            ;;
        # The old behaviour, for a board where the charger and the port really are
        # ONE socket -- which is what this bring-up believed for a long time and
        # what a J36 Ultra is not.  The PHY drops DRVVBUS, reads DEVCTL's VBUS
        # comparator and follows it: fed means be a device and let the charger in,
        # unfed means be a host and source 5 V.  What it costs is the drop: about a
        # tenth of a second with no 5 V on the port, every fifteen seconds, for the
        # life of the board.  Pair it with chrin_shared=1 on the PMIC, or the two
        # halves disagree about which net is which.
        j36.usb=automeasure)
            want_usb=1
            usb_vbus=auto
            ;;
        # The PMIC: the battery gauge, the charger and a poweroff that actually
        # cuts the rail.  Behind a word like the rest, and for the sharpest
        # version of the usual reason -- this is the only payload that writes
        # registers a reboot does not clear.  The MT6323 charger bank keeps its
        # constant voltage, its current-sense threshold and its enable bits
        # across a warm reset, so if the arming sequence here is wrong, the next
        # boot starts from the wrong state whether or not the module loads.
        # Dropping the word from boot.conf is the recovery, and it works from
        # any machine that reads SD cards.
        j36.power|j36.power=1)
            want_power=1
            ;;
        # The read-only half.  The gauge samples, the plug edge is reported, the
        # supplies appear in /sys and poweroff still cuts the rail -- and nothing
        # in Linux touches CHR_CON, so the charger keeps exactly the settings the
        # LK left it with.  This is the word to reach for on a board with no cell
        # fitted, and the one to compare against when something about charging
        # behaves differently after this driver landed.
        j36.power=nocharge)
            want_power=1
            power_charge=0
            ;;
        # The radio: MT6323 rails, the CONSYS power domain, the BTIF link, the
        # two ROM patches, the WLAN firmware and wlan0.  Behind its own word for
        # the reason every payload here has one, and for one more specific to it:
        # the MTCMOS sequence brings up a domain the AP shares a bus with, and
        # a bus protection released in the wrong order on MediaTek does not fault
        # -- it stalls until the watchdog resets the board.  Dropping the word, or
        # deleting j36/wifi/, is the recovery, from any machine that reads SD
        # cards.
        #
        # It implies j36.power and does not merely want it: the rails are on the
        # MT6323 behind a PMIC wrapper with exactly one owner, so without that
        # module the wifi module's symbols do not resolve and insmod refuses.  The
        # implication is applied after this loop, where the ordering is visible.
        j36.wifi|j36.wifi=1)
            want_wifi=1
            ;;
        # Mesa, staged where the loader will find it ahead of the RK3326 blob.
        #
        # j36.es and j36.es=debug were accepted here too, because this word was
        # named after the thing that first used the payload.  They are gone with
        # the rest of it, and nothing in the field is stranded by that: /init and
        # boot.conf are written to the boot partition by the same build, so a card
        # old enough to say j36.es carries an /init old enough to understand it.
        j36.gl|j36.gl=1)
            want_gl=1
            ;;
        # Same payload, plus the things that make a failed GL bring-up say why:
        # Mesa's EGL loader trace, and eglprobe run before the shell starts.  It
        # is a separate word rather than a build option because boot.conf is on
        # the vfat partition, so it can be turned off from any machine that can
        # read the card.
        j36.gl=debug)
            want_gl=1
            gl_debug=1
            ;;
        # The dashboard as the shell.  Still a word rather than the unconditional
        # default because /init has to be able to hand a board to systemd without
        # staging anything at all -- that is the boot that says what is missing.
        j36.dash|j36.dash=1)
            want_dash=1
            ;;
        # Off, for the one board that is being watched on the serial cable while
        # somebody edits the card between boots: the log is redundant then, and the
        # two mounts it makes are two chances to leave a FAT dirty under a reader
        # that already has the answer.  Nothing else turns it off.
        j36.log=0|nolog)
            want_log=0
            ;;
        root=/dev/*)
            root_hint="${arg#root=}"
            ;;
        root=*)
            # LABEL=/UUID= needs blkid, which this initramfs does not carry; the
            # scan below finds the same partition by looking inside it.
            say "root ${arg#root=} is not a device path; scanning instead"
            ;;
    esac
done

# j36.wifi implies j36.power, and it is applied here rather than inside the case
# so that it holds whichever order the two words appear in.  It is an implication
# and not a warning because the alternative is a boot that says "FAILED to load
# j36_mt6592_wifi.ko" with an unresolved-symbol dump in place of a reason: the
# MT6323 connectivity rails are reached through j36_mt6592_pmic's two exported
# calls, so the PMIC module is a hard link-time dependency rather than a
# preference.  Saying so out loud matters because it means j36.wifi silently turns
# the charger on, which is the one thing on this board that is worth being asked
# about -- j36.power=nocharge beside it keeps the gauge and leaves CHR_CON alone.
if [ "$want_wifi" = 1 ] && [ "$want_power" != 1 ]; then
    want_power=1
    say "wifi: j36.wifi needs the PMIC for the MT6323 rails, so j36.power is implied"
    say "      (add j36.power=nocharge as well to leave the charger as the LK set it)"
fi

mkdir -p /newroot

# ext2 first because that is what MixOS formats ROOTFS as now, and it has to be
# named explicitly: ext2 and ext4 are separate drivers in this kernel, and
# `mount -t ext4' will not touch an ext2 filesystem.  ext4 and btrfs follow for
# the cards written by earlier builds and for hand-made ones.  Mounted read-only
# to test, so a candidate that is not the root filesystem is never written to.
#
# A CARD THAT IS NOT THERE YET IS NOT A CARD TO MOUNT.
#
# This is what made the panel look frozen on "Looking for the MixOS card".  A
# mount() against a block device whose card has not finished identification does
# not fail quickly -- it goes down into MSDC1 and waits out a command timeout,
# seconds at a time, in the kernel, with nothing for the splash to be scheduled
# against.  Three filesystem types across every mmcblk node turned that into tens
# of seconds of a boot that was, from the outside, a still picture.
#
# The block layer already knows the answer and it costs nothing to ask: a device
# whose card has not been read has a size of 0 sectors.  Checking that first turns
# the whole not-ready case into a sysfs read, which is what leaves the wait loop
# below free to tick once a second and the animation free to run.
# Only an ANSWER of zero disqualifies a device.  No sysfs entry at all is not an
# answer, and a scan that treated it as one would refuse to mount a card this
# check simply could not see -- trading a slow boot for no boot, which is not the
# trade being made here.
#
# AND A DEVICE THAT CANNOT HOLD A ROOTFS IS NOT WORTH A MOUNT EITHER.  The glob
# below matches everything the mmc block driver publishes, and two kinds of that
# are traps rather than candidates:
#
#   mmcblk0rpmb   the eMMC's Replay Protected Memory Block.  Every read of it has
#                 to be an authenticated RPMB request, so a plain block read is
#                 answered with a command error -- and the controller's answer to
#                 a command error is a timeout and a reset, seconds at a time, in
#                 exactly the place this whole block is about.  It has a non-zero
#                 size in sysfs, so the check above waves it straight through.
#   mmcblk0boot0  the eMMC boot areas.  Readable, small, and never a rootfs.
#   mmcblk0boot1
#
# The size gate covers the rest of it.  The OS partition is four thousand
# megabytes and the smallest thing that has ever been asked to hold this rootfs
# is a good deal more than 256 MB, so anything under that is the BOOT partition,
# a vendor partition off the internal eMMC, or one of the two dozen small MediaTek
# partitions on it -- none of which is a candidate, and all of which used to cost
# three mount() calls apiece to prove it.
dev_ready() {
    case "${1##*/}" in
        *rpmb|*boot0|*boot1) return 1 ;;
    esac
    sz=""
    if [ -r "/sys/class/block/${1##*/}/size" ]; then
        read -r sz < "/sys/class/block/${1##*/}/size"
    else
        return 0
    fi
    case "$sz" in
        ''|*[!0-9]*) return 0 ;;
    esac
    # 524288 sectors of 512 bytes is 256 MB.  Zero is caught by the same compare.
    if [ "$sz" -lt 524288 ]; then return 1; fi
    return 0
}

# Where the child below says what it is doing, so the loop in the parent can put
# it on the panel, and where it leaves its answer.
#
# TWO FILES AND NO SIGNALS, and the reason is the applet list.  This initramfs
# carries no `rm', no `mv' and no `kill' -- INIT_APPLETS in build-in-vm.sh is the
# whole of what /bin holds, and ash here is built without the standalone-shell
# lookup, so an applet that is compiled in but not symlinked is still "not found".
# So the child is not waited on and not signalled: it ANSWERS, by making
# $scan_result non-empty, and `[ -s ]' is the whole protocol.  `:>' creates and
# empties without needing rm, which is why the files are reset that way rather
# than deleted.  They live in /dev beside /dev/.mixsplash, which is mount --move'd
# across switch_root, so a zero-byte leftover there is the same kind of thing that
# channel already is.
scan_status=/dev/.scan-status
scan_result=/dev/.scan-result
scan_say() {
    echo "$1" > "$scan_status" 2>/dev/null
    return 0
}

try_root() {
    dev="$1"
    if [ ! -b "$dev" ]; then return 1; fi
    if ! dev_ready "$dev"; then return 1; fi
    for fs in ext2 ext4 btrfs; do
        # Recorded BEFORE the call and not after it, because the call is the part
        # that can take a while: if a mount really does stall, the panel is
        # already showing which device and which driver it stalled in.
        scan_say "trying $dev as $fs"
        if ! mount -t "$fs" -o ro "$dev" /newroot 2>/dev/null; then continue; fi
        if [ -x /newroot/sbin/init ] || [ -L /newroot/sbin/init ]; then
            # ── REMOUNTED, NOT MOUNTED ALL OVER AGAIN ─────────────────────────
            #
            # This used to umount and mount a second time, and that second mount
            # re-reads the superblock and the group descriptors the first one has
            # just read.  On a card slow enough to be worth watching -- which is
            # the whole subject of this block -- it is the expensive part paid
            # twice, for a filesystem that is already up and already correct.
            # `remount,rw' leaves it where it is and flips the one flag.
            #
            # The read-only probe stays exactly as it was: a candidate that turns
            # out not to be a root filesystem must never have been mounted
            # writable, and that is what the probe is for.  What goes is only the
            # round trip AFTER it has proved that this one is.
            scan_say "remounting $dev read-write"
            if mount -o remount,rw "$dev" /newroot 2>/dev/null ||
               mount -o remount,rw /newroot 2>/dev/null; then
                # Written after the mount call has returned, so a parent reading
                # this file is reading it about a filesystem that is already up.
                printf '%s %s\n' "$dev" "$fs" > "$scan_result"
                return 0
            fi
            # A filesystem that will not remount, rather than one that will not
            # mount.  Fall back to what this always did instead of refusing a
            # card over the way it was asked.
            umount /newroot
            if mount -t "$fs" "$dev" /newroot; then
                printf '%s %s\n' "$dev" "$fs" > "$scan_result"
                return 0
            fi
            return 1
        fi
        umount /newroot
    done
    return 1
}

find_root() {
    if [ -n "$root_hint" ] && try_root "$root_hint"; then return 0; fi
    for dev in /dev/mmcblk*p* /dev/mmcblk*; do
        if try_root "$dev"; then return 0; fi
    done
    return 1
}

# ── the rest of the card, given to the OS partition ───────────────────────────
#
# WHAT IS WRONG.  The image is a fixed 4.2 GB and the card is whatever the operator
# bought.  ROOTFS ends where the image ended, so a 64 GB card carries 60 GB that
# nothing can reach and no page can show.  On this board that is not an inconvenience:
# there is no keyboard, the dashboard is the only shell, and gparted on a PC means
# taking the card out, which is the exact thing a share and a Files page exist to stop
# being necessary.
#
# THIS USED TO GROW A DIFFERENT PARTITION.  There was a p3 -- ext2, labelled DATA,
# mounted at /home/virtua -- and it was the last one on the disk, so it was the one
# that could be grown.  It is gone; /home/virtua is a directory on the rootfs now, the
# OS partition is last, and the space at the end of the card belongs to it.
#
# WHY IT IS INITRAMFS CODE AND NOT A UNIT, WHICH IS THE OPPOSITE OF WHAT IT WAS.  ext2
# has no online resize -- that is an ext4 feature and this filesystem is deliberately
# ext2 -- so the partition has to be unmounted while it grows.  For p3 the one moment
# it was present and unmounted was between udev finding it and systemd mounting it,
# which is a moment a unit can be ordered into.  For the ROOT filesystem there is no
# such moment after switch_root: it is mounted for as long as the system is up.  The
# only window in the whole boot is here, before /newroot is handed over, and it is a
# better window than the old one besides -- nothing else on the disk is mounted either,
# so sfdisk's own BLKRRPART is accepted and no partx dance is needed.
#
# AND THE TOOLS COME OFF THE CARD.  BusyBox has no ext2 resize applet and no sfdisk,
# which is precisely why this was a unit before.  So the three binaries are copied out
# of the rootfs into this initramfs, with the loader and the handful of libraries they
# name, WHILE IT IS STILL MOUNTED -- and then it is unmounted and they are run against
# the bare partition.  They are checked by running them first: a missing library makes
# the loader exit 127, which is the difference between "cannot grow the card" and
# "unmounted the root filesystem and then could not put it back".
#
# NOTHING IS WRITTEN TO THE CARD TO REMEMBER THIS.  No stamp, no /etc/mixos/expanded.
# "Does the partition already reach the end of the disk" is answerable from three files
# in sysfs on every boot, so there is no state to be wrong, nothing to clear when a card
# is re-flashed, and no way for this to run twice or refuse to run once.

# The loader's path is compiled into every binary copied below, so the copies have to
# land at the paths their PT_INTERP and DT_NEEDED name.  /lib is that path on Debian
# armhf and it does not exist in this initramfs until here.
EXPAND_BIN=/lib/mixexpand
EXPAND_LIBS="ld-linux-armhf.so.3 ld-linux.so.3 libc.so.6 libm.so.6 libdl.so.2
             libpthread.so.0 librt.so.1 libgcc_s.so.1 libcrypt.so.1
             libext2fs.so.2 libcom_err.so.2 libe2p.so.2 libss.so.2
             libblkid.so.1 libuuid.so.1 libmount.so.1
             libfdisk.so.1 libsmartcols.so.1
             libselinux.so.1 libpcre2-8.so.0 libz.so.1 libtinfo.so.6"

# Copy one file out of the mounted rootfs, trying each place Debian might keep it.
# `cp' without -a follows the symlink, which is what is wanted: /lib/ld-linux-armhf.so.3
# points into the multiarch directory and this initramfs has no such directory.
expand_take() {
    take_name="$1"
    take_to="$2"
    for take_dir in /newroot/lib /newroot/usr/lib /newroot/sbin /newroot/usr/sbin \
                    /newroot/bin /newroot/usr/bin \
                    /newroot/lib/arm-linux-gnueabihf /newroot/usr/lib/arm-linux-gnueabihf; do
        if [ -e "$take_dir/$take_name" ]; then
            if cp "$take_dir/$take_name" "$take_to/$take_name" 2>/dev/null; then
                chmod 0755 "$take_to/$take_name" 2>/dev/null
                return 0
            fi
        fi
    done
    return 1
}

# Runs a copied tool with a harmless argument.  127 is the shell's "not found" and the
# loader's "a library it needs is not here"; either way the tool cannot be used, and
# finding that out now is what keeps the umount below from being a one-way trip.
expand_works() {
    "$EXPAND_BIN/$1" "$2" >/dev/null 2>&1
    [ "$?" != 127 ]
}

expand_root() {
    if [ -z "$rootdev" ] || [ -z "$rootfs_type" ]; then return 0; fi
    case "$rootfs_type" in
        ext2|ext3|ext4) : ;;
        *)  say "expand: $rootfs_type is not an ext filesystem, so it is left alone"
            return 0 ;;
    esac

    # ── WHICH DISK, AND WHICH NUMBER ON IT ───────────────────────────────────
    #
    # Out of sysfs and not out of the device name.  Stripping a trailing number works
    # for sda2 and is wrong for mmcblk0p2 and for nvme0n1p2.  readlink is not in this
    # busybox either, so the parent is found by looking for the one /sys/block entry
    # that has this partition inside it -- which is the same answer readlink would
    # have given, spelt with a glob.
    ex_name=${rootdev#/dev/}
    ex_sys=/sys/class/block/$ex_name
    if [ ! -r "$ex_sys/partition" ]; then
        say "expand: $rootdev is a whole disk and not a partition; nothing to grow"
        return 0
    fi
    read -r ex_pno < "$ex_sys/partition"
    ex_disk=""
    for ex_cand in /sys/block/*/"$ex_name"; do
        if [ ! -d "$ex_cand" ]; then continue; fi
        ex_cand=${ex_cand%/$ex_name}
        ex_disk=${ex_cand#/sys/block/}
    done
    if [ -z "$ex_disk" ] || [ ! -b "/dev/$ex_disk" ]; then
        say "expand: cannot find the disk $rootdev is a partition of; nothing to grow"
        return 0
    fi

    # ── IS THERE ANYTHING TO DO?  ────────────────────────────────────────────
    #
    # sysfs sizes are in 512-byte sectors regardless of the device's own block size, so
    # these three numbers are directly comparable and no unit conversion is needed.  The
    # margin is 8 MiB: an MBR keeps its last sector to itself, an SD controller may round
    # the reported capacity, and rewriting the partition table to recover four megabytes
    # is a write to the one structure on the card worth not writing to.
    read -r ex_start < "$ex_sys/start"
    read -r ex_size  < "$ex_sys/size"
    read -r ex_whole < "/sys/block/$ex_disk/size"
    ex_slack=$((ex_whole - ex_start - ex_size))
    if [ "$ex_slack" -le 16384 ]; then
        say "expand: $rootdev already reaches the end of /dev/$ex_disk"
        return 0
    fi

    # ── THE TOOLS, TAKEN BEFORE THE FILESYSTEM GOES AWAY ─────────────────────
    say "expand: $((ex_slack / 2048)) MiB of /dev/$ex_disk is unused; taking the tools"
    stage "Growing the MixOS partition"
    detail "$((ex_slack / 2048)) MiB of the card was unused"
    mkdir -p /lib "$EXPAND_BIN"
    for ex_lib in $EXPAND_LIBS; do
        expand_take "$ex_lib" /lib
    done
    ex_missing=""
    for ex_tool in sfdisk e2fsck resize2fs; do
        if ! expand_take "$ex_tool" "$EXPAND_BIN"; then ex_missing="$ex_missing $ex_tool"; fi
    done
    if [ -n "$ex_missing" ]; then
        say "expand: the rootfs has no$ex_missing, so the card keeps the size it has"
        return 0
    fi
    # LD_LIBRARY_PATH as well as the copies being in /lib: the default search path is a
    # property of how glibc was configured, and this costs one environment variable.
    export LD_LIBRARY_PATH=/lib
    for ex_tool in sfdisk:--version e2fsck:-V resize2fs:-h; do
        if ! expand_works "${ex_tool%%:*}" "${ex_tool#*:}"; then
            say "expand: ${ex_tool%%:*} will not run in the initramfs -- a library it"
            say "        needs is not in EXPAND_LIBS.  The card keeps the size it has,"
            say "        and the root filesystem was never unmounted."
            return 0
        fi
    done

    # ── THE WORK, IN A CHILD, WITH THE PANEL STILL TALKING ───────────────────
    #
    # e2fsck and resize2fs on a big slow card are minutes, and mixsplash gives the text
    # console back when nothing has spoken to it for ninety seconds.  So this is forked
    # for the same reason the card scan above is, and by the same means: the child says
    # what it is doing in a file and answers in another one, and the loop below is what
    # keeps the picture moving.  fork does not unshare the mount namespace, so the
    # child's umount and mount are the parent's as well.
    : > /dev/.expand-status
    : > /dev/.expand-result
    (
        ex_step() { echo "$1" > /dev/.expand-status 2>/dev/null; }
        ex_done() { echo "$1" > /dev/.expand-result 2>/dev/null; }

        ex_step "unmounting the OS partition"
        sync
        if ! umount /newroot 2>/dev/null; then
            ex_done "the root filesystem would not unmount, so it was left as it is"
            exit 0
        fi

        # ", +" is sfdisk's whole vocabulary for this: keep the start, take everything
        # left.  -N names the one partition to touch, so the other entry is rewritten
        # byte for byte as it was.  Nothing in the filesystem is read or written by it --
        # it is four bytes of a partition entry -- and because no partition of this disk
        # is mounted at this point in the boot, the kernel accepts the re-read that
        # follows instead of leaving the new table for the next power-on.
        ex_step "rewriting the partition table"
        if echo ", +" | "$EXPAND_BIN/sfdisk" -N "$ex_pno" --force "/dev/$ex_disk" >/dev/null 2>&1; then
            sync
        else
            ex_done "sfdisk would not extend partition $ex_pno; the filesystem is unchanged"
            mount -t "$rootfs_type" "$rootdev" /newroot 2>/dev/null
            exit 0
        fi

        # The node goes away and comes back when the kernel re-reads the table, so wait
        # for it rather than racing it.  Twenty seconds is far past anything real; a card
        # that has not come back by then has a problem the resize is not going to fix.
        # Whole seconds because fractional sleep is a BusyBox build option and this
        # build's applet list is the one at the top of the script, not a guess.
        ex_step "waiting for the partition to come back"
        ex_n=0
        while [ ! -b "$rootdev" ] && [ "$ex_n" -lt 20 ]; do
            ex_n=$((ex_n + 1))
            sleep 1
        done
        if [ ! -b "$rootdev" ]; then
            ex_done "$rootdev did not come back after the table was rewritten"
            exit 0
        fi

        # resize2fs refuses a filesystem it has not seen checked, so this is not
        # optional.  -p fixes what can be fixed without asking, because there is nobody
        # to ask; status 1 means it corrected something and the filesystem is now good,
        # which is a success here.  Anything above that is a filesystem to leave alone.
        ex_step "checking the filesystem"
        "$EXPAND_BIN/e2fsck" -fp "$rootdev" >/dev/null 2>&1
        ex_rc=$?
        if [ "$ex_rc" -gt 1 ]; then
            ex_done "e2fsck says $rootdev needs attention (status $ex_rc), so it was not grown"
            mount -t "$rootfs_type" "$rootdev" /newroot 2>/dev/null
            exit 0
        fi

        # No size argument: resize2fs with none grows the filesystem to fill whatever
        # the partition now is, which is exactly the question that was just answered.
        ex_step "growing the filesystem"
        if "$EXPAND_BIN/resize2fs" "$rootdev" >/dev/null 2>&1; then
            ex_result="$rootdev now fills /dev/$ex_disk"
        else
            ex_result="resize2fs could not grow $rootdev; the partition is bigger than the filesystem"
        fi

        # ── AND PUT IT BACK, WHICH IS THE PART THAT CANNOT BE ALLOWED TO FAIL ──
        #
        # Everything above this point is optional and every failure returns the card
        # unchanged.  This is not optional: the parent is about to switch_root into
        # /newroot.  So it is tried the way try_root does it and then again read-only,
        # because a root that is mounted read-only boots to a shell somebody can fix it
        # from, and a root that is not mounted at all boots to the initramfs.
        ex_step "remounting the OS partition"
        if mount -t "$rootfs_type" "$rootdev" /newroot 2>/dev/null; then
            ex_done "$ex_result"
        elif mount -t "$rootfs_type" -o ro "$rootdev" /newroot 2>/dev/null; then
            ex_done "$ex_result, but it would only remount READ-ONLY"
        else
            ex_done "$rootdev WOULD NOT REMOUNT after the resize"
        fi
    ) &

    # 1800 seconds.  e2fsck and resize2fs on a 512 GB card in a slow reader are the one
    # boot in the life of this device where half an hour is the right thing to wait for,
    # and the bound exists because the alternative to a bound is a board that counts
    # upwards forever.
    ex_waited=0
    while [ ! -s /dev/.expand-result ]; do
        if [ "$ex_waited" -ge 1800 ]; then
            say "expand: the resize has not answered in ${ex_waited}s; carrying on"
            break
        fi
        ex_what=""
        if [ -s /dev/.expand-status ]; then read -r ex_what < /dev/.expand-status; fi
        if [ -z "$ex_what" ]; then ex_what="growing the OS partition"; fi
        detail "$ex_what -- ${ex_waited}s"
        ex_waited=$((ex_waited + 1))
        sleep 1
    done
    if [ -s /dev/.expand-result ]; then
        read -r ex_said < /dev/.expand-result
        say "expand: $ex_said"
    fi
    : > /dev/.expand-status
    : > /dev/.expand-result

    # THE ONE THING THAT IS CHECKED TWICE.  Every failure above puts /newroot back and
    # says so, but "the child was killed", "the wait timed out" and "the node never came
    # back" are between them a path on which nobody did.  switch_root into an unmounted
    # /newroot is an empty directory and a kernel panic, so this asks the kernel rather
    # than trusting the story, and tries once more if the answer is no.  Read-only on
    # the second attempt for the same reason try_root does it: a root that comes up ro
    # boots to something a person can look at.
    if ! grep -q " /newroot " /proc/mounts 2>/dev/null; then
        say "expand: /newroot is not mounted after the resize; remounting it"
        mount -t "$rootfs_type" "$rootdev" /newroot 2>/dev/null ||
        mount -t "$rootfs_type" -o ro "$rootdev" /newroot 2>/dev/null ||
        say "expand: AND IT WOULD NOT MOUNT.  The hand-over below is going to fail."
    fi

    # Whatever happened, the panel goes back to saying what the boot is doing.
    stage "Mounting the MixOS partition"
    detail "$rootdev  $rootfs_type"
    return 0
}

# Waiting is this script's job.  `rootwait' on the command line is honoured by
# the kernel's own root mount, and rdinit= runs instead of that -- so a single
# scan here races the card: MSDC1 runs card identification on a workqueue, and
# an mmc host that is still deferred when /init starts has no block device yet.
#
# ── THE SCAN RUNS IN A CHILD, AND THIS SHELL KEEPS TALKING ────────────────────
#
# The scan used to run right here, and that is what made the panel look dead.
# Not because anything crashed: a single mount() of a card the controller is
# still bringing up can sit in the kernel for tens of seconds, and while this
# shell is inside that call it writes nothing.  mixsplash, which is a separate
# process animating from its own clock, keeps drawing -- but its LAST-MESSAGE
# fuse is ninety seconds, and when nothing has spoken to it for that long it
# concludes /init has stopped somewhere it did not expect to and gives the text
# console back.  A slow card was therefore indistinguishable, to the splash, from
# a dead /init, and the way that reads on the panel is a picture that stops.
#
# So the scan is forked and the ticker is what stays in the foreground.  The
# child mounts -- fork does not unshare the mount namespace, so /newroot really
# is mounted for the parent too -- and reports what it is trying through
# $scan_status; this loop reads that once a second, adds the elapsed time and
# pushes it at the splash.  The splash is now fed every second no matter how long
# any one mount takes, so the fuse cannot fire during a slow card.
#
# IT IS ALSO THE DIAGNOSTIC.  If the panel now counts the seconds up while the
# card is found, userspace was never the problem and the wait is the controller.
# If the count itself stops, the CPU is being held below userspace -- in the MMC
# path, with preemption off -- and no change on this side of the kernel can fix
# that.  One is a boot that says what it is waiting for; the other is a kernel
# question.  Before this, both looked exactly the same.
rootdev=""
rootfs_type=""
stage "Looking for the MixOS card"
detail "waiting for the card"
progress 8
say "waiting for the microSD card"

: > "$scan_result"
scan_say "waiting for the card"
(
    tries=0
    while : ; do
        if find_root; then break; fi
        if [ "$tries" -ge 10 ]; then
            # The give-up answer, so the parent stops on an answer rather than on
            # its own timeout.  Two fields, because that is what the parent reads.
            printf 'none -\n' > "$scan_result"
            break
        fi
        tries=$((tries + 1))
        scan_say "waiting for the card"
        sleep 1
    done
) &

# 300 seconds, and the number is chosen to be one nobody reaches by being slow.
# Ten seconds of retries was the old patience and it is still the child's; this is
# the outer bound on a scan that has WEDGED, and it exists because the alternative
# to a bound is a board that counts upwards forever.  Five minutes in, the
# emergency shell is more use than another tick -- and it says how it got there.
waited=0
while [ ! -s "$scan_result" ]; do
    if [ "$waited" -ge 300 ]; then
        say "the card scan has not answered in ${waited}s; carrying on without it"
        break
    fi
    # An empty read is a status file caught between its truncate and its write,
    # which is a tick of a generic word and not a blank line on the panel.
    scan_what=""
    if [ -s "$scan_status" ]; then read -r scan_what < "$scan_status"; fi
    if [ -z "$scan_what" ]; then scan_what="scanning the card"; fi
    detail "$scan_what -- ${waited}s"
    waited=$((waited + 1))
    sleep 1
done

# Said by the parent and not inside try_root, so that the ticker above -- which
# can run for up to a second after the child has answered -- cannot overwrite it.
if [ -s "$scan_result" ]; then
    read -r rootdev rootfs_type < "$scan_result"
fi
if [ "$rootdev" = none ]; then rootdev=""; rootfs_type=""; fi
if [ -n "$rootdev" ]; then
    say "root: $rootdev ($rootfs_type) after ${waited}s"
    stage "Mounting the MixOS partition"
    detail "$rootdev  $rootfs_type  read-write"
    progress 22
fi
: > "$scan_status"
: > "$scan_result"

# Here and not one line lower, and the line is load-bearing in two directions.
# Everything BELOW reads or writes /newroot -- modules, the GL payload, the dashboard
# staging -- and the resize needs /newroot unmounted; everything below also mounts BOOT,
# and a mounted p1 is what makes the kernel refuse to re-read the partition table it was
# just handed.  This is the last instant in the boot at which the OS partition is known,
# mounted (so its tools can be copied out of it) and the only thing on the disk that is.
# It returns with /newroot mounted either way.
expand_root

# ── Optional payloads: modules, mfgpower, Mesa, the probe ────────────────────
#
# All of them are run here, after the wait loop and before the hand-over, for the
# same two reasons: the card is known to be up by now, and nothing of the rootfs
# has started yet, so systemd, journald and the RK3326 units are not competing for
# the panel.  None of them lives in this initramfs; see build-in-vm.sh for why.
#
# WHERE THEY COME FROM.  The OS partition, at /opt/mixos/j36 -- and it is already
# mounted, because find_root above mounted it as /newroot to switch into.  So the
# usual answer needs no mount, no scan and no filesystem guess at all, and it comes
# off a filesystem that holds symlinks and execute bits.
#
# The FAT BOOT partition is the fallback and only the fallback: a card written by a
# build from before this layout has its payload there, and the two-line search below
# is the whole cost of not breaking it.  BOOT itself is still found the way
# try_root() finds the rootfs -- by mounting candidates and looking inside, because
# partition numbering follows whichever MMC host attached first and this initramfs
# has no blkid.  Read-only, since nothing here writes to the card.
bootfs_mounted=0
# Remembered so that the data-partition automount below can leave the BOOT partition
# alone: it is already reachable, it is the one partition the operator edits from a
# PC, and it is not what "show me the card" means.
bootdev=""
mount_bootfs() {
    if [ "$bootfs_mounted" = 1 ]; then return 0; fi
    mkdir -p /bootfs
    for dev in /dev/mmcblk*p*; do
        if [ ! -b "$dev" ]; then continue; fi
        if ! mount -t vfat -o ro "$dev" /bootfs 2>/dev/null; then continue; fi
        # mvii/ identifies it now, not j36/: boot.conf is the file the LK itself
        # reads and the one thing BOOT always carries, whereas j36/ is exactly what
        # moved off this partition.  j36/ is still accepted, because on a card from
        # an older build that is what is there.
        if [ -d /bootfs/mvii ] || [ -d /bootfs/j36 ]; then
            bootfs_mounted=1
            bootdev="$dev"
            say "boot partition: $dev"
            return 0
        fi
        umount /bootfs
    done
    say "no FAT partition on this card carries mvii/ or j36/"
    return 1
}

# Set once, read by run_lima, run_mtkdrm, run_audio and setup_gl.  Empty means
# "not looked for yet"; find_payload is called by each of them and is idempotent.
payload=""
find_payload() {
    if [ -n "$payload" ]; then return 0; fi
    if [ -d /newroot/opt/mixos/j36 ]; then
        payload=/newroot/opt/mixos/j36
        say "payload: /opt/mixos/j36 in the rootfs"
        return 0
    fi
    # Second, and said out loud, because a payload found on BOOT means the card was
    # written by an older build: it still works, and the person holding it should know
    # which half of the card their next sd-root.tar.gz has to go onto.
    if mount_bootfs && [ -d /bootfs/j36 ]; then
        payload=/bootfs/j36
        say "payload: j36/ on the BOOT partition (an older layout; it still works)"
        return 0
    fi
    say "payload: no j36/ in the rootfs /opt/mixos and none on the BOOT partition"
    return 1
}

# ── Updating a flashed card from a machine that can only write FAT ────────────
#
# The OS partition is ext2 -- and it is the only partition on this card that is not
# FAT, the user's home being a directory inside it -- and macOS mounts ext2 not at all.
# So the documented update -- untar sd-root.tar.gz onto ROOTFS -- needs a Linux
# machine, and the only other way to change an already-flashed card is to write the
# whole image again.  This is the third way: copy ONE FILE, sd-root.tar.gz, onto the
# FAT BOOT partition, which macOS does mount, and let this initramfs unpack it.
#
# WHY A TARBALL AND NOT THE TREE.  Copying opt/ onto a FAT partition by hand is the
# thing that looks like it should work and does not.  vfat and exfat have no symlinks
# and no execute bit, and the Qt payload is some thirty SONAME symlinks -- libQt5Core.so.5
# and friends.  Depending on which tool does the copying they arrive flattened into
# full copies, dereferenced into nothing, or written as short text files holding the
# target's path, and the loader's verdict on that last one is "invalid ELF header"
# when it opens libQt5Widgets.so.  A tar archive stores the link, the mode and the
# name; unpacking it here happens on Linux, as root, on ext2, where all three survive.
#
# Once per tarball, not once per boot: the size and timestamp are recorded in the tree
# and compared on the next boot.  Nothing is deleted first -- tar overwrites in place,
# and `rm -rf' inside an initramfs aimed at somebody's OS partition is not a trade
# worth making for a few stale files.
#
# Same two files and the same protocol as the card scan above, for the same reason and
# with a second one on top.  The reason: unpacking tens of megabytes of Qt through
# gunzip on a Cortex-A7 takes minutes, and for every one of them this shell was inside
# a single pipeline saying nothing -- past mixsplash's ninety-second last-message fuse,
# which then decided /init had died and gave the text console back.  That is exactly
# what "the splash stops after Installing the update and the kernel log comes back"
# was, and it was not a crash: the boot underneath carried on and finished, invisibly,
# behind a console nobody had asked for.
#
# The second reason is that this step has something worth showing.  `tar -v' names
# every entry as it writes it, so the child pipes that into a read loop that keeps the
# LAST name in $unpack_status -- one builtin write per file, no fork -- and the parent
# puts it on the panel once a second.  So the minute is a minute of libQt5Widgets.so.5
# and bin/mixdash going past rather than a still picture, and a hang has an obvious
# shape: the name stops changing while the seconds keep counting.
unpack_status=/dev/.unpack-status
unpack_result=/dev/.unpack-result
stage_from_boot() {
    if [ -z "$rootdev" ]; then return 1; fi
    if ! mount_bootfs; then return 1; fi
    if [ ! -f /bootfs/sd-root.tar.gz ]; then return 1; fi

    # Size, month, day and time out of `ls -l': this initramfs has no stat, no wc and
    # no md5sum, and those four fields change whenever the build writes a new tarball.
    set -- $(ls -l /bootfs/sd-root.tar.gz)
    boot_stamp="$5 $6 $7 $8"
    if [ -r /newroot/opt/mixos/.staged-from-boot ]; then
        read -r staged_stamp < /newroot/opt/mixos/.staged-from-boot
        if [ "$staged_stamp" = "$boot_stamp" ]; then
            say "stage: /opt/mixos already matches sd-root.tar.gz on BOOT"
            return 0
        fi
        say "stage: sd-root.tar.gz on BOOT is not the one that made /opt/mixos"
    fi

    say "stage: unpacking sd-root.tar.gz from BOOT onto the OS partition"
    say "       $boot_stamp -- once per tarball, not once per boot"
    # The longest single step in this script by a wide margin -- tens of megabytes
    # of Qt through gunzip on a Cortex-A7 -- and it happens on exactly the boot
    # where the user has just changed something and is watching.  It gets its own
    # headline for that reason.
    stage "Installing the update"
    detail "unpacking sd-root.tar.gz"
    mkdir -p /newroot/opt
    : > "$unpack_status"
    : > "$unpack_result"
    (
        gunzip -c /bootfs/sd-root.tar.gz | tar -xv -C /newroot 2>/dev/null | \
        while IFS= read -r unpacked; do
            printf '%s\n' "$unpacked" > "$unpack_status" 2>/dev/null
        done
        # In here, and not after the loop below, because this is the second place the
        # fuse used to fire.  tar returns when the last write is in the page cache;
        # pushing tens of megabytes of that out to a class-4 microSD is its own long
        # silence, and it landed immediately after the one this whole fork exists to
        # cover.  Inside the child it is ticked like everything else, and the parent's
        # own sync a few lines down then has only the stamp file left to flush.
        sync
        # The answer, and it is only ever this one word: what the unpack produced is
        # checked by the parent below, on the tree, not on an exit status.  All this
        # has to carry is "the child is no longer running".
        printf 'done\n' > "$unpack_result"
    ) &

    # 900 seconds.  A tarball this size has never taken five, and the number is not a
    # patience setting -- it is the bound that stops a wedged gunzip from counting
    # upwards until the battery goes, on a board with no key to press.  Past it the
    # boot carries on and the check below reports what did or did not arrive, which is
    # a great deal more use than a panel stuck on a number.
    unpack_waited=0
    while [ ! -s "$unpack_result" ]; do
        if [ "$unpack_waited" -ge 900 ]; then
            say "stage: the unpack has not finished in ${unpack_waited}s; carrying on"
            break
        fi
        # Empty means the status file was caught between its truncate and its write,
        # which is a tick of the headline word rather than a blank line on the panel.
        unpack_what=""
        if [ -s "$unpack_status" ]; then read -r unpack_what < "$unpack_status"; fi
        if [ -z "$unpack_what" ]; then unpack_what="unpacking"; fi
        detail "$unpack_what -- ${unpack_waited}s"
        unpack_waited=$((unpack_waited + 1))
        sleep 1
    done
    : > "$unpack_status"
    : > "$unpack_result"
    say "stage: unpacked in ${unpack_waited}s"

    # tar's exit status is not the test, and now it is not even reachable -- the
    # unpack ran in a child that is answered for by a file.  It was never the right
    # test anyway: that was a pipeline in ash, so what came back was tar's, and a
    # gunzip that dies half way through a truncated file leaves tar perfectly happy
    # with the part it did get.  What matters is whether the thing this exists to
    # install is there and executable.
    if [ ! -x /newroot/opt/mixos/bin/mixdash ]; then
        say "stage: FAILED -- no executable opt/mixos/bin/mixdash came out of it."
        say "       Either the copy of the tarball on BOOT is truncated (check that"
        say "       it is the same size as the one in the artifacts) or the OS"
        say "       partition is full."
        return 1
    fi
    printf '%s\n' "$boot_stamp" > /newroot/opt/mixos/.staged-from-boot
    sync
    say "stage: /opt/mixos is now what the tarball holds"
    return 0
}

# Doom used to run from here, off the BOOT partition, before the dashboard existed.
# It does not any more and there is no j36.doom word.  A 26 MiB IWAD was the first
# thing that obviously did not belong on a 100 MB vfat launcher partition shared with
# an R36S card's own boot files, and moving it is what started the layout the rest of
# the payload now follows.  A J36_DOOM=1 build puts doomgeneric and its IWAD in
# /opt/mixos on the ext2 OS partition, where the dashboard's Doom card launches them
# -- after the boot, from a shell, rather than in the middle of an initramfs.

# ── The Mali-450, if the command line asks ───────────────────────────────────
#
# CONFIG_DRM_LIMA is =m and this is the only thing that loads it, because the MFG
# power domain is gated when Linux starts and a read into an unpowered MTK
# subsystem stalls the AXI bus -- a built-in lima would probe during boot and
# take the board into a silent watchdog reset.  So mfgpower runs first: it powers
# the domain through the SPM and reads back the GP and PP product IDs, and its
# exit status is the gate.  Nothing is insmod'ed unless a Mali-450 answered.
#
# Its output goes through a file and show(), not straight to stdout, because
# stdout is /dev/console and that is one of the two places this needs to be
# readable -- the register values it prints are the whole diagnostic when the
# domain does not come up.
#
# load.order is written by the build from `modinfo -F depends', so the order here
# is the dependency order and not a guess.  insmod failures are reported and not
# fatal: a module that is already built in returns EEXIST-ish noise, and the boot
# should continue either way.
run_lima() {
    if ! find_payload; then return 1; fi
    if [ ! -x "$payload/mfgpower" ]; then
        say "lima: j36.lima was asked for but j36/mfgpower is not on the card"
        return 1
    fi
    if [ ! -f "$payload/modules/load.order" ]; then
        say "lima: j36/modules/load.order is missing; nothing to load"
        return 1
    fi
    "$payload/mfgpower" >/tmp/mfgpower.log 2>&1
    rc=$?
    show /tmp/mfgpower.log
    if [ "$rc" != 0 ]; then
        say "lima: mfgpower exited $rc; leaving the GPU alone"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        if insmod "$payload/modules/$ko" >/tmp/insmod.log 2>&1; then
            say "lima: loaded $ko"
        else
            say "lima: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < "$payload/modules/load.order"
    say "DRM devices:"
    ls -l /dev/dri 2>/dev/null || say "  none"
    return 0
}

# ── The display pipe, if the command line asks ────────────────────────────────
#
# lima renders and this scans out; Mesa needs both card nodes before EGL can
# present anything.  Everything here is a module for the same containment reason
# as lima, plus one of its own: these bind real device tree nodes, so a built-in
# set would take the panel on every boot whether the card carried the payload or
# not.  As modules, deleting j36/mtkdrm from the card restores the previous boot
# exactly, with no reflash.
#
# There is no gate program here and there does not need to be one.  MMSYS is
# already powered and clocked -- the LK drew the boot logo through it -- so
# nothing in this set reads an unpowered block, which is the failure mode
# mfgpower exists to prevent for the MFG domain.  What replaces the gate is
# CONFIG_DRM_FBDEV_EMULATION=n: with no fbdev emulation these modules program no
# register at all until userspace opens /dev/dri/card0 and sets a mode, so
# loading them is visually a no-op and /dev/fb0 stays simplefb's window onto the
# LK's framebuffer.
#
# The last line of load.order is the panel, and it is the one to watch.  mtk_dsi
# calls component_add from inside mtk_dsi_host_attach, which only runs when a
# mipi_dsi_driver has probed on the panel node and called mipi_dsi_attach() -- so
# no panel module means no DRM master and no card node, however well the other
# four loaded.  If /dev/dri stays empty after this, that is the first thing dmesg
# will say.
#
# The FIRST line is j36_fbmem, and it is independent of everything after it: it
# binds the device tree's j36,lk-framebuffer node and puts /dev/j36fb on the
# board, which is the LK's framebuffer carveout as a dma-buf.  Nothing in the boot
# uses it -- it is what lets a GL client render into the scanout instead of
# copying into it, and `eglprobe -z' is what measures that -- so its absence costs
# nothing at boot and is reported here rather than being fatal.
run_mtkdrm() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/mtkdrm/load.order" ]; then
        say "mtkdrm: j36.mtkdrm was asked for but j36/mtkdrm/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        if insmod "$payload/mtkdrm/$ko" >/tmp/insmod.log 2>&1; then
            say "mtkdrm: loaded $ko"
        else
            say "mtkdrm: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < "$payload/mtkdrm/load.order"
    say "DRM devices:"
    ls -l /dev/dri 2>/dev/null || say "  none"
    if [ -c /dev/j36fb ]; then
        say "the LK framebuffer as a dma-buf: $(cat /sys/class/misc/j36fb/info 2>/dev/null)"
    else
        say "no /dev/j36fb: GL clients can only reach the panel by copying into /dev/fb0"
    fi
    return 0
}

# ── The AFE, if the command line asks ─────────────────────────────────────────
#
# Same containment as the two above, for one reason of its own: this is the first
# thing on this board that ungates the AFE's functional clocks, so nobody has yet
# seen the DL1 DMA cursor advance.  Taking that measurement is what the payload is
# for, and putting it behind a word means a card that ends up wedged by it is
# fixed by deleting a directory rather than by a reflash.
#
# Loaded before the hand-over, like the others, so the card is known to be up and
# the modules are in place before systemd starts looking for a controlC0.  The
# module's own dmesg lines are the output that matters and they are not repeated
# here -- CLK_CFG_AUD before and after, AUDIO_TOP_CON0, and then, on the first
# stream, either "AFE DL1 DMA is live" or the cursor warning.
#
# The speaker parameter is passed as a module parameter and not compiled in, so
# the same .ko serves both words.
run_audio() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/audio/load.order" ]; then
        say "audio: j36.audio was asked for but j36/audio/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        params=""
        if [ "$audio_speaker" = 1 ]; then
            case "$ko" in j36_mt6592_audio.ko) params="speaker=1" ;; esac
        fi
        if insmod "$payload/audio/$ko" $params >/tmp/insmod.log 2>&1; then
            say "audio: loaded $ko $params"
        else
            say "audio: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < "$payload/audio/load.order"
    if [ -d /dev/snd ]; then
        say "sound devices:"
        ls -l /dev/snd 2>/dev/null
    else
        say "audio: no /dev/snd; the card did not register"
    fi
    if [ "$audio_speaker" = 1 ]; then
        say "audio: speaker amp armed; it powers up when the DL1 cursor first moves"
        say "audio: if the board cuts out in playback: amixer -c0 set \"Speaker Amp\" off"
    else
        say "audio: speaker amp off -- amixer -c0 set \"Speaker Amp\" on to try it"
    fi
    # The jack is on unless somebody turns it off, and it is not behind a word:
    # the headphone buffers run off the reference the DAC already needs, so unlike
    # the class-D there is no rail to take down.  Nothing on this board detects a
    # plug, so this is the only thing that decides it.
    say "audio: headphone jack on -- amixer -c0 set Headphone off, or Settings > Sound"
    return 0
}

# ── USB host, if the command line asks ────────────────────────────────────────
#
# Same containment as the three above, and here the gate is the sharpest one in
# this file: MUSB and its PHY are behind the PERI clock gate, and on MediaTek an
# APB access to a gated peripheral does not fault, it stalls the bus until the
# watchdog resets the board.  j36_mt6592_usb_phy clears the gate in phy_init,
# which musb calls before it reads a single controller register, so the ordering
# is correct -- but it is correct by construction rather than by measurement, and
# that is exactly the kind of thing that belongs behind a word.
#
# The load order comes out of the build's dependency walk, and the first entry is
# the PHY: nothing that touches the MUSB window may be loaded before the driver
# that ungates it.
#
# SKIP WHAT IS ALREADY LOADED, which none of the other payloads has to do.  udl
# is a DRM driver, so it pulls in drm_kms_helper -- and so does mtk_drm, which
# means the mtkdrm payload stages its own copy of the same module.  (Only that
# one: udl's other helper, drm_shmem_helper, is udl's alone.  mtk_drm implements
# its GEM object itself and asks for the DMA helper only when fbdev emulation is
# on, and this build turns fbdev emulation off.)  A boot that asks for both words
# would otherwise reach the second copy and insmod would fail with EEXIST, which
# reads in the log exactly like a broken module.  /sys/module names have
# underscores where filenames have hyphens, hence the tr.
run_usb() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/usb/load.order" ]; then
        say "usb: j36.usb was asked for but j36/usb/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        mod=$(printf '%s' "${ko%.ko}" | tr '-' '_')
        if [ -d "/sys/module/$mod" ]; then
            say "usb: $ko is already loaded"
            continue
        fi
        # The one module here that takes an argument, and it has to be passed on
        # the insmod line rather than in bootargs: a kernel-cmdline
        # `modname.param=' only reaches modules built into the image, and every
        # one of these is loadable.  So the three j36.usb words that pick a VBUS
        # policy are translated here.
        #
        # Always passed, even when it matches the module's own default: the insmod
        # line is echoed by the say below, so a boot log states the policy outright
        # instead of leaving a reader to know what the default was on that build.
        # That default has changed once already, which is why this is worth a line.
        args=""
        case "$ko" in
            j36_mt6592_usb_phy.ko)
                case "$usb_vbus" in
                    0) args="vbus=0" ;;
                    auto) args="vbus=-1" ;;
                    *) args="vbus=1" ;;
                esac
                ;;
        esac
        if insmod "$payload/usb/$ko" $args >/tmp/insmod.log 2>&1; then
            say "usb: loaded $ko${args:+ $args}"
        else
            say "usb: FAILED to load $ko${args:+ $args}"
            show /tmp/insmod.log
        fi
    done < "$payload/usb/load.order"

    # What to look at, in the order it answers the questions.  The bus directory
    # is the controller itself: usb1 appearing means musb bound and the root hub
    # registered, which is already most of the bring-up.  Anything past usb1 is a
    # device that enumerated.
    if [ -d /sys/bus/usb/devices ]; then
        say "usb devices:"
        ls /sys/bus/usb/devices 2>/dev/null
    else
        say "usb: no /sys/bus/usb/devices -- usbcore did not register"
    fi
    if [ -e /dev/input/event0 ]; then
        say "input devices:"
        ls /dev/input 2>/dev/null
    fi
    if [ -d /sys/class/drm ]; then
        say "drm nodes:"
        ls /sys/class/drm 2>/dev/null
    fi

    # The disk half, reported separately because it fails separately.  A stick that
    # enumerated shows up in the bus listing above whether or not usb-storage bound
    # it; what says the mass-storage stack is working is a /dev/sd* node, and what
    # says a partition table was read is a numbered one beside it.  Nothing is
    # mounted here -- that is udev's and mixos-automount's job after switch_root,
    # and a read-only mount from the initramfs would only be in their way.
    for blk in /dev/sd[a-z]; do
        if [ ! -b "$blk" ]; then continue; fi
        say "usb: mass storage is up --"
        ls -l /dev/sd[a-z] /dev/sd[a-z][0-9]* 2>/dev/null
        say "     left unmounted on purpose; /media is filled in after switch_root"
        break
    done

    # Said every time, because VBUS is the single most likely reason for a port
    # that looks dead, and because on this board it is also the one thing in the
    # payload that draws real current off the system rail.  The PHY driver logs
    # the pad it drove; this says which of the three arrangements the card asked
    # for, in the words the operator would have to change.
    #
    # These lines used to describe an either/or, and it was the wrong picture of
    # the board.  A J36 Ultra has TWO connectors: a DC inlet, which charges and has
    # no data lines, and this OTG port, which carries the data.  CHRDET is the
    # inlet, DRVVBUS is the port, and neither can be mistaken for the other -- so
    # sourcing 5 V here costs nothing at the charger, the port never has to stand
    # down, and the measurement that used to arbitrate between them had nothing to
    # arbitrate.  It is still in the driver for boards that do share one socket:
    # j36.usb=automeasure, and set chrin_shared=1 on the PMIC to match.
    case "$usb_vbus" in
        0)
            say "usb: VBUS held off by j36.usb=novbus -- a bus-powered device will not come up"
            say "usb: charging is unaffected: it comes in on the DC inlet, not this port"
            ;;
        auto)
            say "usb: VBUS follows the port (j36.usb=automeasure) -- for a board with ONE socket"
            say "usb: this drops 5 V on the port for ~0.1 s every 15 s; drop the word to stop that"
            ;;
        *)
            say "usb: VBUS held high -- always a host, so a stick, a mouse or a hub comes up"
            say "usb: fit a cell: the 5 V is a switch off VBAT, which on this PMIC is the system rail"
            ;;
    esac
    return 0
}

# ── The PMIC, if the command line asks ───────────────────────────────────────
#
# One module, and the smallest payload on the card by a wide margin.  What it
# buys is not small: without it nothing in Linux knows this board has a battery,
# so the dashboard's meter has nothing to read, batt_led.service has no
# /sys/class/power_supply/battery/capacity to poll, and `poweroff' halts the CPU
# with the rail still up -- the board sits there warm until the cell runs out or
# somebody holds the power button.
#
# LOAD IT AFTER USB, not before.  The ordering is worth less than it used to be
# and is kept anyway.  The PMIC reads GPIO pad 15 every poll to find out whether
# the OTG port is sourcing 5 V, and it used to hold the charger off while that
# read high -- on the belief that this handheld had one connector doing both
# jobs.  It has two: a DC inlet, which is what CHRDET senses, and the OTG port,
# which is what the pad switches.  So the read is an observation now and decides
# nothing unless chrin_shared says otherwise, and the worst a bad order can cost
# is one misleading log line rather than a boot that never charges.  Loading the
# PHY first still means the pad is settled before the first poll looks at it.
#
# j36.power=nocharge becomes charge=0 on the insmod line, for the same reason
# j36.usb=novbus becomes vbus=0: a kernel-cmdline `modname.param=' only reaches
# modules built into the image, and this one is loadable.
run_power() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/power/load.order" ]; then
        say "power: j36.power was asked for but j36/power/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        args=""
        case "$ko" in
            j36_mt6592_pmic.ko)
                [ "$power_charge" = 1 ] || args="charge=0"
                ;;
        esac
        if insmod "$payload/power/$ko" $args >/tmp/insmod.log 2>&1; then
            say "power: loaded $ko${args:+ $args}"
        else
            say "power: FAILED to load $ko${args:+ $args}"
            show /tmp/insmod.log
        fi
    done < "$payload/power/load.order"

    # The two supplies are the whole test, and they are worth listing by name:
    # `battery' and `usb' are not arbitrary.  Both readers -- mixdash and
    # batt_led.service -- walk /sys/class/power_supply/* and take the first
    # entry whose type file says Battery, which is what makes them work without
    # either one being told this driver exists.  The name is kept anyway,
    # because /sys/class/power_supply/battery/capacity is the path every RK3326
    # script and every forum answer for this family reaches for by hand.
    if [ -d /sys/class/power_supply ]; then
        say "power supplies:"
        ls /sys/class/power_supply 2>/dev/null
    else
        say "power: no /sys/class/power_supply -- the driver did not register"
    fi

    # The first capacity reading, said out loud.  It comes from the PMIC's own
    # wakeup OCV latch rather than from a curve fit against the live rail, which
    # is the one number on this board that means what it says: there is no
    # power-path FET here, so VBAT is the system node and every live sample
    # moves with the backlight and the amp rather than with charge state.
    if [ -r /sys/class/power_supply/battery/capacity ]; then
        say "power: battery reads $(cat /sys/class/power_supply/battery/capacity 2>/dev/null)%"
    fi

    if [ "$power_charge" != 1 ]; then
        say "power: charger left as the LK set it by j36.power=nocharge"
    fi

    # The backlight, by the same test and for the same reason: the dashboard's
    # Display page walks /sys/class/backlight/* and drives the first entry it
    # finds, so the directory existing at all is the whole difference between a
    # brightness slider and a row that says there is nothing to move.  The
    # adopted level is printed because it is the LK's, not ours -- the driver
    # reads the duty out of the PWM block rather than writing one -- so this line
    # is also the answer to "what did the loader hand over".
    if [ -d /sys/class/backlight ] && [ -n "$(ls /sys/class/backlight 2>/dev/null)" ]; then
        for bl in /sys/class/backlight/*; do
            [ -r "$bl/brightness" ] || continue
            say "power: backlight ${bl##*/} at $(cat "$bl/brightness" 2>/dev/null) of $(cat "$bl/max_brightness" 2>/dev/null)"
        done
    else
        say "power: no /sys/class/backlight -- brightness cannot be changed from Linux"
    fi
    return 0
}

# ── The connectivity subsystem, if the command line asks ─────────────────────
#
# AFTER run_power, AND THE ORDER IS A LINKER FACT RATHER THAN A PREFERENCE.  The
# MT6323 rails the radio needs are behind the PMIC wrapper, which is one state
# machine with one result register and no arbitration, so there is exactly one
# owner of it in this kernel and this module reaches it through two exported
# symbols.  insmod of the wifi module before the PMIC module fails outright,
# naming the symbol.  Loaded in this order it resolves, and the driver's probe
# additionally defers until the PMIC's DEVICE has bound, which is the part a
# symbol cannot express.
#
# THE FIRMWARE PATH IS SET BEFORE THE INSMOD, and that line is the whole reason
# this works from an initramfs.  request_firmware() searches the filesystem that
# is mounted now, and /init switch_roots a few seconds later; pointing the loader
# at the payload's own directory means the images are read while that directory
# is still there.  The driver reads both patches during probe rather than during
# its bring-up work precisely so this insmod is the synchronisation point -- when
# it returns, the images are in kernel memory and the card can be pulled out from
# under it.
#
# Nothing is written to the rootfs and no /lib/firmware is populated: the blobs
# ship inside j36/wifi/firmware/, so deleting j36/wifi/ takes the radio off the
# card in one step, like every other payload here.

# The wireless interface, by the one property that identifies one: phy80211 is
# the symlink cfg80211 puts in a wireless netdev's sysfs directory.  The name is
# not assumed, because the kernel numbers these and a USB Ethernet adapter that
# was plugged in at boot can be there first.
wlan_iface() {
    for n in /sys/class/net/*; do
        [ -e "$n/phy80211" ] || continue
        printf '%s' "${n##*/}"
        return 0
    done
    return 1
}

run_wifi() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/wifi/load.order" ]; then
        say "wifi: j36.wifi was asked for but j36/wifi/load.order is not on the card"
        return 1
    fi

    if [ -w /sys/module/firmware_class/parameters/path ]; then
        echo "$payload/wifi/firmware" > /sys/module/firmware_class/parameters/path
        say "wifi: firmware search path is $payload/wifi/firmware"
    else
        say "wifi: no writable firmware_class path -- the ROM patches and the WLAN firmware will not be found"
    fi

    # Same skip as run_usb, for the same reason and a different overlap: the
    # dependency walk that built this load order followed j36_mt6592_wifi's link
    # to j36_mt6592_pmic, so the PMIC is in here too -- and run_power has already
    # loaded it, with its charge= argument, which this must not undo.
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        mod=$(printf '%s' "${ko%.ko}" | tr '-' '_')
        if [ -d "/sys/module/$mod" ]; then
            say "wifi: $ko is already loaded"
            continue
        fi
        if insmod "$payload/wifi/$ko" >/tmp/insmod.log 2>&1; then
            say "wifi: loaded $ko"
        else
            say "wifi: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < "$payload/wifi/load.order"

    # The bring-up is a work item, not part of insmod, because it is seconds long
    # -- a hundred patch fragments with a two-second ceiling on each answer -- and
    # a radio that does not come up must not be able to hold up the panel.
    #
    # So this waits, and it waits with a bound that is deliberately shorter than
    # the worst case.  A healthy bring-up lands in two or three seconds and the
    # verdict is worth having in the boot log; an unhealthy one can spend ten
    # seconds inside the RF calibration timeout alone, and standing here for that
    # buys a message that dmesg already has.  Eight seconds covers the first and
    # refuses the second.
    #
    # Nothing is lost by walking away: the work item holds no reference to any
    # filesystem by this point, so it runs to completion across switch_root.
    #
    # THE SUCCESS TEST IS /sys AND NOT dmesg, because an interface either exists
    # or it does not and that is exactly the question the dashboard's Wi-Fi page
    # asks.
    #
    # The three failure tests are dmesg, and they are plain greps rather than one
    # alternation: `\|' is a GNU extension to basic regular expressions, and which
    # regex engine busybox grep ends up using is a property of how busybox was
    # configured.  A boot-time test that silently never matches would turn every
    # failed bring-up into the "still running" verdict.
    i=0
    while [ "$i" -lt 8 ]; do
        if wlan_iface >/dev/null; then break; fi
        if dmesg | grep -q 'j36-mt6592-wifi.*no interface was registered'; then break; fi
        if dmesg | grep -q 'j36-mt6592-wifi.*connectivity MCU up'; then break; fi
        if dmesg | grep -q 'j36-mt6592-wifi.*bring-up stopped'; then break; fi
        sleep 1
        i=$((i + 1))
    done

    iface=$(wlan_iface) || iface=""
    if [ -n "$iface" ]; then
        dmesg | grep 'j36-mt6592-wifi' | tail -n 2
        say "wifi: $iface is up.  NetworkManager and wpa_supplicant take it from here"
    elif dmesg | grep -q 'j36-mt6592-wifi.*no interface was registered'; then
        dmesg | grep 'j36-mt6592-wifi' | tail -n 4
        say "wifi: the WLAN firmware is running but no interface was registered;"
        say "      the line above names where cfg80211 refused it"
    elif dmesg | grep -q 'j36-mt6592-wifi.*connectivity MCU up'; then
        dmesg | grep 'j36-mt6592-wifi' | tail -n 4
        say "wifi: the connectivity MCU is up and patched, but the WLAN firmware"
        say "      did not start, so there is no interface"
    elif dmesg | grep -q 'j36-mt6592-wifi.*bring-up stopped'; then
        dmesg | grep 'j36-mt6592-wifi' | tail -n 6
        say "wifi: bring-up did not finish; the lines above say where it stopped"
    else
        say "wifi: bring-up is still running -- dmesg has the verdict after boot"
    fi
    return 0
}

# ── Mesa, staged where the loader will find it, without touching the rootfs ───
#
# This is the last link in the chain: card0 from mtkdrm, a render node from lima,
# and now a libEGL/libgbm/libGLESv2 that are Mesa's rather than the RK3326 blob's.
#
# NOTHING ON THE SHARED ROOTFS IS WRITTEN, and that is the whole design of this
# function.  The card carries one Debian rootfs for two machines, and the R36S needs
# its libEGL.so -> libMali.so symlinks to stay exactly where they are: that blob is
# the only thing that drives its Mali-G31.  So the libraries go into a tmpfs and the
# environment that points at them goes into mixdash.service, in the same tmpfs.  Pull
# this card into an R36S and there is no trace of any of it.
#
# The mechanism is the initrd's half of a contract systemd documents: /run must be a
# tmpfs, and if the initrd has already mounted one, PID 1 adopts it with its contents
# rather than mounting its own over the top.
#
# WHAT THIS FUNCTION USED TO ALSO BE.  It was setup_es_gl, and better than half of it
# was EmulationStation: a second GLES 2.0 build of it copied out of the payload and
# bind-mounted over the rootfs's binary, and four drop-ins under
# emulationstation.service.d that set SDL_VIDEODRIVER, an LD_PRELOAD, a debug ExecStart
# and a pair of probe ExecStartPres.  All of it existed to get one program that this
# board cannot run -- Renderer_GLES10.cpp aborts with status 134 here -- as far as its
# first frame.  The dashboard replaced that program, so the reason went with it; what
# is left is the part that was never about ES at all, which is Mesa on a search path.
# The three names checked at the bottom are the ones mixdash's 3D cube card needs
# through eglprobe, and they are the same three the GLES 2.0 renderer needed, because
# they are simply what EGL on this GPU is made of.
#
# One tmpfs, two callers.  The GL payload and the dashboard both put their files and
# their units under /newroot/run, and both are optional, so whichever runs first
# mounts it -- and mounting a second one over the top would hide the first one's work.
# It has to be a tmpfs and not the rootfs's own /run because of the invariant this
# whole card is built on: nothing on the shared rootfs is written.
run_tmpfs=0
ensure_run_tmpfs() {
    if [ "$run_tmpfs" = 1 ]; then return 0; fi
    if mount -t tmpfs -o mode=0755 tmpfs /newroot/run 2>/dev/null; then
        run_tmpfs=1
        return 0
    fi
    return 1
}

# Set by setup_gl for setup_dash to read: whether the Mesa payload is complete enough
# to point a child at, and whether the probe was staged.  They are separate because
# the payload can be there without the probe and the dashboard's 3D cube card needs
# the first while the boot-time diagnostics need the second.
gl_ready=0
probe_ready=0

setup_gl() {
    if ! find_payload; then return 1; fi
    if [ ! -f "$payload/gl/links" ]; then
        say "gl: j36.gl was asked for but j36/gl/ is not on the card"
        return 1
    fi
    if [ -z "$rootdev" ]; then
        say "gl: no rootfs was found, so there is no systemd to configure"
        return 1
    fi

    if ! ensure_run_tmpfs; then
        say "gl: could not mount a tmpfs on the rootfs /run"
        return 1
    fi

    mkdir -p /newroot/run/j36/gl
    staged=0
    # cp and not a bind mount, even now that the payload is on a filesystem that could
    # be bind-mounted: /run/j36/gl is a tmpfs the rootfs adopts, and the point of it is
    # that nothing here depends on the payload partition still being reachable -- or
    # writable, or unmounted cleanly -- once systemd is running.
    for so in "$payload"/gl/*.so*; do
        [ -f "$so" ] || continue
        if cp "$so" /newroot/run/j36/gl/; then
            staged=$((staged + 1))
        else
            say "gl: could not copy $so"
        fi
    done
    # The links file stands in for the symlinks vfat cannot store: "name target", one
    # pair per line, targets relative to this directory.  Read whichever partition the
    # payload came off, because a card written before the payload moved has it on the
    # vfat one.
    while read -r name target; do
        case "$name" in ''|'#'*) continue ;; esac
        ln -sf "$target" "/newroot/run/j36/gl/$name"
    done < "$payload/gl/links"

    # The EGL probe rides in beside the libraries, not among them: /run/j36/gl is
    # a loader search path and a binary in it would be a name ld.so has to skip.
    # mixdash-probe.service is what runs it, once, under j36.gl=debug.
    if [ -f "$payload/eglprobe" ]; then
        if cp "$payload/eglprobe" /newroot/run/j36/eglprobe; then
            chmod 0755 /newroot/run/j36/eglprobe
            probe_ready=1
            # The unit tees the probe's output to a file so it can be printed again
            # after the shell has exited and its own spew has scrolled past.  That
            # unit does not run as root and this directory is root-owned 0755, so
            # tee could not create the file -- it said "permission denied" and the
            # repeat had nothing to print.  Create it here, where we are root, and
            # let anyone write it.  A log in a tmpfs that is thrown away on reboot
            # does not need to be guarded.
            : > /newroot/run/j36/eglprobe.log
            chmod 0666 /newroot/run/j36/eglprobe.log
        else
            say "gl: could not copy the EGL probe"
        fi
    fi

    # Nothing above is allowed to fail quietly, because of what the fallback is.
    # LD_LIBRARY_PATH tells the loader to look in this directory; if the directory is
    # incomplete the loader simply misses and finds the same names in /usr/lib, where
    # the shared rootfs has pointed them at the RK3326's libMali.so -- an ARMv8-A
    # object on a Cortex-A7.  What that produces is SIGILL before main() in whatever
    # dlopened it, and the message names neither this directory nor the blob.  So it
    # has to be caught here.
    #
    # These three and no others.  libEGL.so.1 creates the context and libGLESv2.so.2
    # supplies the entry points eglGetProcAddress hands back.  libgbm.so.1 is
    # load-bearing twice over: the KMSDRM path dlopens it and libEGL_mesa.so.0 carries
    # it in its own DT_NEEDED, so without it Mesa's EGL pulls the RK3326 blob in as
    # its gbm and cannot initialise.
    #
    # mixdash itself needs none of them: it is Qt on linuxfb and it draws with the
    # CPU.  Its 3D cube card is what needs them, through eglprobe -- so an incomplete
    # payload costs one card in the dashboard, not the boot.
    missing=""
    for need in libEGL.so.1 libgbm.so.1 libGLESv2.so.2; do
        [ -e "/newroot/run/j36/gl/$need" ] || missing="$missing $need"
    done
    if [ -n "$missing" ]; then
        say "gl: the GL payload is incomplete, missing:$missing"
        say "    ($staged of the libraries copied.)  LD_LIBRARY_PATH is deliberately"
        say "    NOT set: pointing it at a directory that cannot satisfy those names"
        say "    sends the loader to /usr/lib and the RK3326 Mali blob, which is"
        say "    ARMv8-A on this Cortex-A7.  Leaving the environment alone fails in"
        say "    the same place but without this initramfs having claimed to fix it."
        return 1
    fi
    gl_ready=1

    say "gl: payload in /run/j36/gl, $staged libraries"
    say "    $(ls /newroot/run/j36/gl | tr '\n' ' ')"
    return 0
}

# ── The dashboard, and the unit that starts it ────────────────────────────────
#
# A UNIT OF OUR OWN, WRITTEN INTO A tmpfs.  mixdash.service goes in
# /run/systemd/system, wanted by multi-user.target through a symlink in
# /run/systemd/system/multi-user.target.wants/.  Unit FILES do not merge across the
# search trees but dependency DIRECTORIES do -- .wants is additive -- so a want
# written in /run is honoured for a target whose unit is in /usr/lib.  This is the
# only thing that starts the dashboard.
#
# Nothing here writes to the card, and the whole arrangement is gone on the next
# boot: /run/systemd/system is the tmpfs mounted a few lines up.  That is the
# invariant the whole card is built on -- one Debian rootfs, two machines, and the
# R36S has to find it exactly as it left it.
#
# THIS USED TO BE TWICE THE SIZE, and half of it was a mask.  EmulationStation was
# installed as /etc/systemd/system/emulationstation.service and enabled, so taking it
# out of the boot meant a symlink to /dev/null in /run/systemd/system.control -- the
# one runtime directory in systemd.unit(5)'s precedence list that outranks /etc --
# plus a drop-in resetting its ExecStart to an echo for the case where the mask was
# ignored.  There is no such unit on this rootfs any more, so there is nothing to
# mask; the precedence detail is written down once more above where neuter_es was,
# because it is the part somebody would get wrong if it ever comes back.
#
#
# WHERE THE PAYLOAD IS, asked rather than assumed.  find_mixos() looks for it in the
# rootfs first, because extracting sd-root.tar.gz there is what the artifact README
# says to do, and then on every other partition of the card -- a tree extracted onto
# any partition works, read-only mounted, without a keyboard and without a shell.
#
# WHY User=root.  The obvious unit runs as the login user, and three things the dashboard
# does are not that user's: it puts /dev/tty0 into KD_GRAPHICS at its first paint so the
# kernel's console stops drawing over the dashboard -- and back into KD_TEXT if it
# fails or is stopped, which is the only reason a failure on this board is readable at
# all -- and that is an ioctl on a device an unprivileged login cannot open; the Restart and Power off
# cards call reboot and poweroff; and the 3D cube card needs DRM master for its
# modeset.  None of them is worth a polkit rule on a card that cannot be edited.
#
# WHY LD_LIBRARY_PATH NAMES BOTH DIRECTORIES.  mixdash finds its own Qt through
# RPATH -- built with --disable-new-dtags, so it is DT_RPATH and it covers the
# platform plugin's dependencies too -- but RPATH is not inherited by children, and
# the cube it launches resolves libEGL and libgbm by dlopen at runtime.  This is
# what points that child at Mesa in /run/j36/gl instead of at the RK3326 Mali blob
# /usr/lib's symlinks name.  It is also what makes a payload found somewhere other
# than /opt/mixos work at all: ld.so searches DT_RPATH first, misses, and falls
# through to this.
mixos_root=""
# One word per partition considered -- mmcblk0p1:vfat:none, mmcblk0p3:unreadable --
# collected so that the notice unit can repeat it at the end of the boot, after the
# kernel's own output has scrolled these lines off a 640x480 panel.
dash_seen=""
find_mixos() {
    # Extracted into the rootfs: the documented way, and the only one that needs no
    # mount of its own, since /init has already mounted that partition as /newroot.
    if [ -x /newroot/opt/mixos/bin/mixdash ]; then
        mixos_root=/opt/mixos
        say "dash: /opt/mixos is in the rootfs"
        return 0
    fi
    dash_seen="${rootdev##*/}:rootfs:no-opt-mixos"

    # Anywhere else on the card.  Read-only, because this is somebody's data
    # partition and nothing here has any business writing to it, and under /run so
    # that the mount lives in a tmpfs directory rather than in a directory this
    # script would have to create on the shared rootfs.
    #
    # Every partition it tries is named on the console whether or not the payload is
    # there, and that is the point: with no keyboard on this board, "nothing loaded"
    # and "the tarball was never unpacked onto the card" look identical, and this is
    # the line that tells them apart.
    mkdir -p /newroot/run/j36/mixos
    for dev in /dev/mmcblk*p* /dev/sd*; do
        if [ ! -b "$dev" ]; then continue; fi
        if [ "$dev" = "$rootdev" ]; then continue; fi
        dash_mounted=0
        # exfat is in the list because that is what firstboot used to convert p3 to on
        # a card written before this layout, and because a USB stick formatted on a PC
        # is exfat as often as not; a payload unpacked on one has to be reported as
        # "found but crippled" rather than as "no such partition" -- the libQt5Core
        # check below is what says which.
        #
        # Whatever is found here is left mounted read-only for the rest of the boot,
        # which is the price of a card whose rootfs has no /opt/mixos at all.  It is
        # announced on the console by the line below, and the fix is the documented
        # one -- put the tarball in the rootfs.
        for fs in ext2 ext4 btrfs exfat vfat; do
            if ! mount -t "$fs" -o ro "$dev" /newroot/run/j36/mixos 2>/dev/null; then continue; fi
            dash_mounted=1
            # opt/mixos is what the tarball unpacks to at a partition root; mixos/ is
            # what unpacking it one level down produces, and it is a mistake worth
            # tolerating rather than reporting.
            for sub in opt/mixos mixos; do
                if [ -x "/newroot/run/j36/mixos/$sub/bin/mixdash" ]; then
                    mixos_root="/run/j36/mixos/$sub"
                    say "dash: found $sub on $dev ($fs), mounted read-only at /run/j36/mixos"
                    # vfat holds no symlinks, and the Qt payload is thirty-odd SONAME
                    # symlinks -- libQt5Core.so.5 and friends.  Without them the loader
                    # cannot resolve mixdash's own DT_NEEDED and it dies before main().
                    if [ ! -e "/newroot$mixos_root/qt/lib/libQt5Core.so.5" ]; then
                        say "dash: that copy has no qt/lib/libQt5Core.so.5 -- if it was"
                        say "      unpacked onto the vfat BOOT partition the SONAME"
                        say "      symlinks are gone, and mixdash will not start."
                        say "      Unpack it on the ext2 ROOTFS partition instead;"
                        say "      BOOT only carries the kernel and the device tree."
                    fi
                    return 0
                fi
            done
            say "dash: $dev ($fs) carries no opt/mixos"
            dash_seen="$dash_seen ${dev##*/}:$fs:none"
            umount /newroot/run/j36/mixos
            break
        done
        # Not a footnote: a partition none of these drivers will mount is the one
        # shape of this failure that no amount of looking in the right directory
        # fixes.  All four are built into this kernel, so what lands here is an
        # unformatted partition, or f2fs, or one whose superblock is damaged.
        if [ "$dash_mounted" = 0 ]; then
            say "dash: $dev would not mount as ext2, ext4, btrfs, exfat or vfat"
            dash_seen="$dash_seen ${dev##*/}:unreadable"
        fi
    done
    return 1
}

# ── where the Files page opens ────────────────────────────────────────────────
#
# There is no keyboard on this board and the dashboard is the only shell, so
# /run/j36/card is the one path the Files page opens on and it has to point at
# something before the dashboard's first paint.
#
# THIS USED TO MOUNT A PARTITION AND NOW IT DOES NOT, and the reason is that there
# is no longer a partition for it to mount.  The card is two partitions -- BOOT and
# ROOTFS -- and the user's space is /home/virtua, an ordinary directory on the root
# filesystem that is mounted, by definition, before anything here runs.  So the whole
# of the old job -- glob the mmcblk partitions, try five filesystems on each, skip the
# ones carrying j36/ or mvii/, look for a .mixos-home stamp, unmount again and leave
# the real mount to systemd -- collapses into naming a path.
#
# AND THE SYMLINK IS STILL RIGHT ON A CARD WRITTEN BEFORE THIS LAYOUT, which is why
# the scan could be deleted rather than kept behind a condition.  Such a card has an
# ext2 p3 labelled DATA and an fstab line mounting it at /home/virtua; this link names
# that mount point, not that device, so systemd mounts p3 there a few seconds later
# and the Files page follows it without knowing which of the two layouts it is on.
# That was the reason for not mounting it here in the first place: a block device
# cannot be held ro by the initramfs and mounted rw by systemd at the same time -- the
# second mount comes back EBUSY, silently, that fstab line carrying nofail.
mount_card() {
    # Where the rootfs intends the user's space to be, read out of its own fstab if
    # that card still mounts something there, and otherwise the path this build uses.
    # Pure shell rather than awk: this initramfs has neither awk nor cut, and `read'
    # splits a line into fields for free -- see INIT_APPLETS.
    home_mp=""
    if [ -r /newroot/etc/fstab ]; then
        while read -r fs_spec fs_mp fs_rest; do
            case "$fs_spec" in
                LABEL=DATA) home_mp="$fs_mp"; break ;;
            esac
        done < /newroot/etc/fstab
    fi
    [ -n "$home_mp" ] || home_mp=/home/virtua

    # Made if it is missing, because a dangling symlink and an empty directory look
    # the same from the Files page and neither says which one went wrong.  On any card
    # this build writes it is already there, owned by virtua; this covers the case
    # where it is not, and the ownership is fixed by the login user's own home anyway.
    mkdir -p "/newroot$home_mp" 2>/dev/null

    # -n as well as -f: without it, `ln -s target dir' with the directory still there
    # puts the link INSIDE it instead of replacing it, and the Files page would open on
    # a directory holding one dangling symlink.  /newroot and not /run, because this
    # runs before switch_root.
    mkdir -p /newroot/run/j36
    rmdir /newroot/run/j36/card 2>/dev/null
    if ln -sfn "$home_mp" /newroot/run/j36/card; then
        say "dash: /run/j36/card -> $home_mp"
        return 0
    fi
    say "dash: could not point /run/j36/card at $home_mp; the Files page will be empty"
    return 1
}

setup_dash() {
    if [ -z "$rootdev" ]; then
        say "dash: no rootfs was found, so there is no systemd to configure"
        return 1
    fi
    if ! ensure_run_tmpfs; then
        say "dash: could not mount a tmpfs on the rootfs /run"
        return 1
    fi
    if ! find_mixos; then
        say "dash: no opt/mixos/bin/mixdash on any partition of this card."
        say "      Unpack sd-root.tar.gz at the root of the ext2 ROOTFS partition,"
        say "      not on BOOT -- BOOT is FAT and would lose Qt's symlinks."
        say "      Nothing is started as a fallback: the dashboard is the only shell"
        say "      in this build, so a readable console is the whole of what is left"
        say "      and it is the better failure."
        dash_notice
        return 1
    fi

    # The card, before the unit, so that the dashboard's Files page has it from its
    # first paint rather than after a rescan the operator has no way to trigger.
    mount_card

    # ── the dashboard's own unit ─────────────────────────────────────────────────
    #
    # The directory is made here and not assumed: setup_gl is the only other thing
    # that creates it, and on a card with no j36/ directory at all, or a boot with no
    # j36.gl word, it never runs.  A `cat >' into a directory that does not exist
    # would take the dashboard out of the boot for the sake of one mkdir.
    mkdir -p /newroot/run/systemd/system
    # ── UNQUOTED, AND THAT MAKES THE COMMENTS BELOW CODE ──────────────────────
    #
    # The word is bare and has to stay bare: $mixos_root is resolved at boot and is
    # the whole point of writing this unit from the initramfs rather than shipping
    # it.  The cost is that everything between here and UNITDASH is still read by
    # the shell for $, ` and \ -- a `#' at the start of a line means nothing to it,
    # so these are systemd comments and not shell comments.
    #
    # This is not hypothetical.  Two of them said `handover' and `-', the way every
    # other comment in this file does, and the backticks paired up into a command
    # substitution spanning six lines with unbalanced apostrophes inside it.
    # BusyBox ash gave up parsing setup_dash, /init exited on the spot, and because
    # nothing else ever writes to /dev/.mixsplash the splash held its last line --
    # "trying /dev/mmcblk0p2" -- for as long as the board was on.  A quoting mistake
    # in prose that reads as a frozen boot.
    #
    # So: no backticks, no bare $, no trailing backslash in this body or in UNITFC,
    # UNITID and UNITNOTICE below, which are unquoted for the same reason.  Use "..."
    # when a word needs setting off.  The check after the heredoc parses the finished
    # /init with a real POSIX shell so the next one fails the build instead.
    cat > /newroot/run/systemd/system/mixdash.service <<UNITDASH
# Written by the J36 Ultra initramfs, into a tmpfs.  Not on the card, not in the
# rootfs: it exists only for as long as this boot does.
[Unit]
Description=MixOS dashboard (J36 Ultra)
Documentation=file:///opt/mixos/README.txt
# Ordering only, and kept for the cards already in the field: firstboot repartitioned
# them, and a dashboard listing a partition that is being resized should not race it.
# It is disabled in this rootfs -- see finishing_touches.sh -- and a unit that is not
# installed is simply not ordered against, so naming it costs nothing.
After=firstboot.service systemd-user-sessions.service
# Three tries a minute and then it stops, and the number is not caution -- it is the
# lesson from the shell this one replaced, whose Restart=on-failure ran a binary that
# aborted in 200 ms and put the same stack trace on a 640x480 panel six times, with
# the first line -- the only one that said anything -- already scrolled away.  A
# dashboard that cannot start should leave its last error on the glass.
StartLimitIntervalSec=60
StartLimitBurst=3

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=$mixos_root/bin
# ── the last word to the splash, which is no longer a goodbye ────────────────
#
# The splash has been running since the initramfs and is still on the panel: it
# survived switch_root, /dev came with it, so /dev/.mixsplash is the same file it
# has had open all along.  This names the stage and fills the bar, and that is now
# all it does -- it does not stop the splash and it does not set the done flag the
# ticker watches for.
#
# IT USED TO SEND "quit" HERE, a whole second before mixdash was even exec'd, and
# the comment that stood in this space defended it: mixsplash exits leaving the
# console in KD_GRAPHICS, so its last frame stays on the glass while Qt links
# itself, and a held picture beats a text console reappearing.  Both halves of
# that were wrong on this board.  The held picture is precisely what "MixSplash
# freezes during the dashboard load" is a report of -- it is a still frame for as
# long as the dynamic linker takes.  And the console reappears regardless, because
# KD_GRAPHICS is a mode and not a lock: systemd, agetty and vconsole-setup each
# reset the VT during that same stretch of the boot, and the one process that was
# taking it back every second was the splash this line had just killed.  What
# fbcon drew into the gap included this unit's own journal+console output, which
# is the "quick console text info" that shows up just before the dashboard.
#
# So the splash now runs until mixdash has a frame on the panel, and mixdash puts
# it down itself: dismissSplash() in main.cpp, called from the first paintEvent --
# and from every failure path as well, so a dashboard that never paints still
# hands the console back in time to say why.
#
# Appending to a file, never a pipe, so this cannot block even if nothing is
# reading; and "-" in front so that a boot with no splash at all does not fail
# its ExecStartPre.  Re-running on every restart attempt is harmless by design.
ExecStartPre=-/bin/sh -c '{ echo "stage:Starting the dashboard"; echo "progress:100"; } >> /dev/.mixsplash'
ExecStart=$mixos_root/bin/mixdash
# ── the backstop for the dashboard that never gets as far as its own code ────
#
# dismissSplash() covers every way mixdash can fail once it is running, because it
# hangs off textMode() and every fatal path goes through that.  What it cannot
# cover is the dashboard that never runs at all: a missing binary, a missing
# library, an exec that fails before main.  With the splash no longer stopped
# ahead of ExecStart, that boot would sit under a splash still animating and still
# re-taking the console every second, with systemd's explanation on a VT that
# nothing is drawing -- a picture of a working boot on a board that has none.
#
# So the unit dismisses it too, on the way out.  It runs on the failure that would
# otherwise be silent, and on the ordinary stop, and on the restart in between;
# every one of those wants the console back.  On the normal path it is a duplicate
# of what mixdash already did minutes ago, appended to a regular file with nothing
# reading it, which costs six bytes and does nothing else.
#
# The word is abort and not quit -- and NO BACKTICKS AROUND EITHER OF THEM, because
# this heredoc is the unquoted one and a backtick in it is a command substitution that
# ends up inside /init.  The quoting habit the rest of this file uses for words like
# this was applied here once and the apostrophes caught inside the substitution parsed
# as "Unterminated quoted string"; bash -n on this script sees none of it, because out
# there the whole of /init is inside a single-quoted heredoc.  What caught it is the
# dash -n at the bottom of this file, and that is the only reason it cost a build
# rather than a card.
#
# As for the choice itself: /init says handover before switch_root, and quit honours it
# by exiting without touching the console mode.  That is right when the dashboard is
# painting and wrong here, where the whole point is to get the text console back so the
# reason there is no dashboard can be read off the panel.  abort is the word that means
# exactly that.
ExecStopPost=-/bin/sh -c ': > /dev/.mixsplash-done; echo abort >> /dev/.mixsplash'
Restart=on-failure
RestartSec=2
# 3 is the dashboard's own startup watchdog and 4 is an exception caught at the top of
# main -- an out-of-memory report with the requested size, the step and a backtrace in
# it.  Both mean the dashboard has already put the console back into text mode and
# written its own verdict there.  Restarting produces the identical verdict twice more
# and scrolls the first one off a 480-pixel panel, which is exactly how the last
# bad_alloc was read as three unrelated failures.
RestartPreventExitStatus=3 4
# /run/mixdash, 0700, for XDG_RUNTIME_DIR -- Qt keeps sockets and lock files there and
# warns about it in its first line of output when it is not set.
RuntimeDirectory=mixdash
RuntimeDirectoryMode=0700
Environment="XDG_RUNTIME_DIR=/run/mixdash"
Environment="LD_LIBRARY_PATH=/run/j36/gl:$mixos_root/qt/lib"
# nographicsmodeswitch, AND IT IS NOT OPTIONAL ON THIS BOARD.  The bootargs put
# /dev/console on tty0, so the panel IS the console; Qt's linuxfb plugin otherwise
# sets KD_GRAPHICS from inside the QApplication constructor, fbcon stops drawing, and
# every message after that -- kernel printk, this unit's own stdout, the dashboard's
# reason for not starting -- goes only to a journal on an ext2 partition that the
# machine which flashed this card cannot mount.  What that looks like is a panel
# frozen mid-boot with no dashboard on it and nothing to say why.  mixdash takes
# KD_GRAPHICS itself from its first paint instead.  The binary appends this option if
# it is missing, because it is on the OS partition and this unit is on BOOT.
Environment="QT_QPA_PLATFORM=linuxfb:nographicsmodeswitch"
Environment="QT_QPA_PLATFORM_PLUGIN_PATH=$mixos_root/qt/plugins/platforms"
Environment="QT_QPA_FB_DISABLE_INPUT=1"
# Read only by QBasicFontDatabase, which is not the one this Qt uses -- Debian's
# QtGui links fontconfig -- so it is a fallback, not the font path.  main.cpp loads
# the payload's faces by name through QFontDatabase::addApplicationFont.
Environment="QT_QPA_FONTDIR=$mixos_root/qt/fonts"
# journal AND console, and it is startup-only in practice.  Until the first frame the
# panel is the best place these two can go: a dashboard that dies before it paints has
# nothing else to say why, and the journal is on an ext2 partition the machine that
# flashed this card cannot read.  From the first frame the dashboard takes both
# descriptors off the console itself and points them at /run/j36/mixdash.log -- see
# Console::toLog in tools/mixdash/console.cpp -- because after that a Qt warning here
# is a line of text drawn straight across the grid, and it arrives through a channel
# that holding the console mode cannot close, the text being the dashboard own output.
# j36-logdump copies the tail of that file into BOOT:/mixos-log.txt, so nothing said
# after the first frame is lost by the move.
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
UNITDASH

    # FONTCONFIG_FILE, and only if the file is actually on this card.
    #
    # It is what decides what the font phase costs: without it fontconfig reads the
    # rootfs's /etc/fonts/fonts.conf, walks every font directory on the card and builds
    # a cache, and the payload's own fonts.conf names one directory with two faces in
    # it instead.  But it is NOT safe to name unconditionally -- fontconfig fails
    # closed.  Point FONTCONFIG_FILE at a file that is not there and FcInitLoadConfig
    # gives back a configuration with no font directories at all, which is a dashboard
    # with no glyphs rather than a slow one.  This unit is generated onto a tmpfs from
    # an initramfs on the BOOT partition and the payload it names lives on the OS
    # partition; the two are updated by different means and can be a version apart, so
    # the test is done here rather than assumed.
    if [ -f "/newroot$mixos_root/qt/fonts/fonts.conf" ]; then
        cat >> /newroot/run/systemd/system/mixdash.service <<UNITFC

[Service]
Environment="FONTCONFIG_FILE=$mixos_root/qt/fonts/fonts.conf"
UNITFC
        say "dash: fontconfig confined to $mixos_root/qt/fonts, two faces, no rootfs scan"
    else
        say "dash: no $mixos_root/qt/fonts/fonts.conf, so fontconfig reads the rootfs's"
        say "      own config and may build a cache -- watch for a long \"fonts\" phase"
    fi

    # ── the middle of systemd, narrated once a second ────────────────────────────
    #
    # Between switch_root and mixdash there is a stretch of systemd -- fsck, udev,
    # the journal, whatever the rootfs enables -- that on this SoC is the longest
    # quiet part of the boot.  /init cannot narrate it (it is gone) and mixdash
    # cannot (it has not started).
    #
    # This was one oneshot unit that echoed twice at sysinit.target, and that was
    # not enough for the same reason a single message is never enough here: the
    # splash gives the console back when nothing has spoken to it for long enough,
    # and after the hand-over that fuse is three minutes.  A first boot -- fsck of
    # a freshly written OS partition, a cold udev, ldconfig, a journal with no
    # /var/log yet -- can spend longer than that between two echoes, and what the
    # panel then does is exactly what it does when /init dies: picture off, kernel
    # log back.  Nothing has gone wrong at that point; the boot below carries on
    # and finishes, unwatched, behind a console nobody asked for.
    #
    # So it ticks.  Every five seconds it says how long systemd has been at it,
    # which resets the fuse and, more usefully, turns "did it stop?" into a
    # question the panel answers by itself: a number that keeps climbing is a slow
    # boot and a number that stops is a wedged one.
    #
    # WHY IT IS A FILE AND NOT A LINE IN THE UNIT.  systemd expands ${...} in Exec
    # lines before /bin/sh ever sees them, so a shell loop with a counter written
    # inline is at the mercy of a substitution rule that has nothing to do with the
    # shell.  A script in /run has no such reader in the middle; the unit names it
    # and that is all.
    #
    # It starts before sysinit.target rather than after it, because the stretch
    # this exists to cover starts at switch_root -- waiting for sysinit is waiting
    # through the very part of the boot that is slowest on a first run.
    # DefaultDependencies=no is what makes that legal, and Type=simple means
    # systemd considers it started the moment it forks, so nothing waits on it.
    mkdir -p /newroot/run/j36/bin
    cat > /newroot/run/j36/bin/mixos-splash-tick <<'SPLASHTICK'
#!/bin/sh
# Written by the J36 Ultra initramfs into a tmpfs.  Feeds the boot splash while
# systemd starts, and stops the moment mixdash.service touches the done file.
chan=/dev/.mixsplash
done_flag=/dev/.mixsplash-done

# A boot with j36.splash=0, or one where mixsplash could not open /dev/fb0, has no
# channel and nothing to say anything to.  Appending would create the file and tick
# into it forever.
[ -e "$chan" ] || exit 0

{ echo "stage:Starting system services"
  echo "detail:systemd"
  echo "progress:94"
} >> "$chan"

# 2400 ticks of five seconds is two hours, and it is a backstop rather than a
# patience setting: every boot that reaches a dashboard stops this in well under a
# minute by way of the done file.  What it bounds is the boot that does not, where
# a shell counting into a file nobody reads should eventually stop counting.
n=0
while [ "$n" -lt 2400 ]; do
    if [ -e "$done_flag" ]; then exit 0; fi
    sleep 5
    n=$((n + 1))
    echo "detail:systemd -- $((n * 5))s" >> "$chan"
done
exit 0
SPLASHTICK
    chmod 0755 /newroot/run/j36/bin/mixos-splash-tick

    cat > /newroot/run/systemd/system/j36-splash.service <<'UNITSPLASH'
# Written by the J36 Ultra initramfs, into a tmpfs.
[Unit]
Description=Keep the boot splash fed until the dashboard takes the panel
DefaultDependencies=no
After=systemd-remount-fs.service
Before=sysinit.target

[Service]
Type=simple
ExecStart=-/bin/sh /run/j36/bin/mixos-splash-tick

[Install]
WantedBy=sysinit.target
UNITSPLASH
    mkdir -p /newroot/run/systemd/system/sysinit.target.wants
    ln -sf ../j36-splash.service \
           /newroot/run/systemd/system/sysinit.target.wants/j36-splash.service

    # ── which build is this, on both sides of the card ───────────────────────────
    #
    # /etc/j36-build was written into this initramfs when the boot image was built, and
    # says what the dashboard binary of that same build hashes to.  The binary on the OS
    # partition says what IT was compiled from.  They are updated by different means --
    # a boot image is dragged onto the vfat BOOT partition, /opt/mixos arrives either
    # baked into the image or unpacked from sd-root.tar.gz -- so they can be a version
    # apart, and a stale dashboard failing looks exactly like a new one failing.
    # Passing the expectation in lets mixdash print both and say whether they agree,
    # which is the difference between "my change did not work" and "my change did not
    # ship".
    if [ -f /etc/j36-build ]; then
        say "dash: this boot image is $(sed -n 's/^init=//p' /etc/j36-build)"
        dash_expect="$(sed -n 's/^mixdash=//p' /etc/j36-build)"
        if [ -n "$dash_expect" ]; then
            cat >> /newroot/run/systemd/system/mixdash.service <<UNITID

[Service]
Environment="MIXDASH_EXPECT=$dash_expect"
UNITID
            say "dash: expects mixdash $dash_expect"
        fi
    fi

    # The [Install] section above is inert -- nothing runs systemctl enable on a
    # read-only rootfs -- so the want is written by hand.  .wants directories are
    # merged across /etc, /run and /usr/lib, which is why this works for a target
    # whose own unit file is in /usr/lib and cannot be touched.
    mkdir -p /newroot/run/systemd/system/multi-user.target.wants
    ln -sf ../mixdash.service \
           /newroot/run/systemd/system/multi-user.target.wants/mixdash.service

    # ── the dock: mirror /dev/fb0 onto a USB-HDMI adapter ────────────────────────
    #
    # Only written when the binary is on the card, so deleting one file from
    # /opt/mixos/bin takes the whole feature out of the boot with nothing left behind
    # to fail -- the same removal contract every payload in this build has.
    #
    # WHY IT RUNS FROM BOOT AND NOT FROM A RULE.  It watches for the adapter itself:
    # a readdir of /dev/dri every two seconds, and it mirrors while a "udl" node is
    # there.  A udev rule would be less code here and more places to be wrong -- the
    # rootfs is shared with an R36S, udev is not running yet in the initramfs, and a
    # rule that fires once cannot notice the TV being switched on behind a hub that
    # was already plugged in.  Two seconds of readdir is not measurable on this SoC.
    #
    # It is ordered after the dashboard only so that the first thing it mirrors is a
    # dashboard rather than a console; it does not require it, and mirrors whatever is
    # in /dev/fb0.  Restart=always because the one thing it must not do is give up:
    # the whole point is to be ready whenever the dock arrives, which may be hours in.
    #
    # THE COMMENTS BELOW ARE INSIDE AN UNQUOTED HEREDOC, like UNITDASH above.  No
    # backticks, no bare apostrophes, no trailing backslash -- $mixos_root is the one
    # expansion wanted here and everything else that looks like shell would be shell.
    if [ -x "/newroot$mixos_root/bin/j36-mixmirror" ]; then
        cat > /newroot/run/systemd/system/j36-mixmirror.service <<UNITMIRROR
# Written by the J36 Ultra initramfs, into a tmpfs.
[Unit]
Description=Mirror the panel onto a USB-HDMI adapter when one is plugged in
Documentation=file:///opt/mixos/README.txt
After=mixdash.service

[Service]
Type=simple
User=root
Group=root
# Nice, because this loses every argument against the dashboard: a diff of the panel
# is worth exactly nothing if it costs the thing being mirrored its frame rate.
Nice=5
ExecStart=$mixos_root/bin/j36-mixmirror
Restart=always
RestartSec=5
# The journal only.  Its steady state is silence, but a dock that is being plugged and
# unplugged says a line each way, and console output would put those on top of the
# dashboard.
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNITMIRROR
        ln -sf ../j36-mixmirror.service \
               /newroot/run/systemd/system/multi-user.target.wants/j36-mixmirror.service
        say "dash: USB-HDMI mirror armed -- plug a DisplayLink dock in at any time"
    else
        say "dash: no bin/j36-mixmirror on this card, so a USB-HDMI dock will"
        say "      enumerate but will not show the dashboard"
    fi

    # ── the probe, and WHY IT NO LONGER RUNS ON AN ORDINARY BOOT ─────────────────
    #
    # This unit was the whole of "mixsplash shows the eglprobe colours and then a
    # console text screen flashes before the dashboard", and it produced both halves
    # of that from the two lines in `eglprobe -f' that were written to be helpful.
    #
    # The colours are literal: fb_report(repair=1, paint_it=1) fills /dev/fb0 with
    # eight bars, a white border and a diagonal, then sleeps.  There is no compositor
    # on this board and no DRM in that path -- the bars land directly on top of the
    # splash's own pixels, in the same buffer, a second before the dashboard.
    #
    # The text screen is tty_report(repair=1), and it is subtler.  The splash holds
    # /dev/tty0 in KD_GRAPHICS, which is exactly the state that function is written to
    # diagnose: it reads the mode, decides that "something put it there and did not put
    # it back", and sets KD_TEXT.  fbcon then repaints the entire console over the
    # splash.  It lasts about a second because console_regrab() in mixsplash.c re-takes
    # KD_GRAPHICS once a second and paints the next frame over it -- which is precisely
    # a flash, and precisely where the user sees one.  Neither of those is a bug in
    # eglprobe.  A probe whose job is to undo a console left in graphics mode cannot
    # tell a splash that is using the mode from a dead client that abandoned it, so the
    # answer is not to make it cleverer: it is not to run it while a splash is up.
    #
    # So it runs under j36.gl=debug and not otherwise.  That is the flag whose whole
    # meaning is "I am debugging the display and I want to see what it says", and the
    # boot it costs its seamlessness on is a boot the operator asked to look inside.
    # The binary is staged either way, so `/run/j36/eglprobe -f' from a shell answers
    # the same question after the fact without a rebuild.
    #
    # It is NOT -p or -c even then: those two modeset, so either one would own the
    # screen for as long as it ran and the boot would be watching a cube instead of
    # coming up.  That is a smaller objection than it was -- preserve_lk_state now
    # gives the panel back when the client exits, where before either one hid the
    # dashboard for the rest of the boot -- but a boot is still not where they go.
    #
    # A UNIT OF ITS OWN, AND NOT ExecStartPre, AND THIS IS THE FIX FOR A HANG.
    # ExecStartPre runs on every start ATTEMPT, restarts included.  mixdash.service is
    # Restart=on-failure with StartLimitBurst=3, so a dashboard that fails to start ran
    # these probes three times -- and under j36.gl=debug two of them are the full probe,
    # which loads Mesa and creates EGL contexts on lima.  What that looked like on the
    # glass was colour bars, then hundreds of libEGL debug lines, then the bars again,
    # repeating until the kernel stopped answering: each cycle re-initialised the GPU
    # while the previous attempt's error was still the only thing worth reading, and
    # each cycle repainted the bars over it.
    #
    # Type=oneshot with RemainAfterExit=yes runs once per boot.  mixdash Wants= and
    # After= it, so a mixdash restart finds it already active and does not re-run it:
    # one EGL init per boot instead of three, and attempts two and three put their own
    # stderr on a panel nothing has repainted.
    if [ "$probe_ready" = 1 ] && [ "$gl_debug" = 1 ]; then
        cat >> /newroot/run/systemd/system/mixdash.service <<'UNITPROBEDEP'

[Unit]
Wants=mixdash-probe.service
After=mixdash-probe.service
UNITPROBEDEP
        cat > /newroot/run/systemd/system/mixdash-probe.service <<'UNITPROBE'
# Written by the J36 Ultra initramfs, and ONLY under j36.gl=debug.  Once per boot,
# before the dashboard.  It paints over the splash and hands the console back to
# fbcon on purpose; that is the point of asking for it.
[Unit]
Description=MixOS panel probe (J36 Ultra, j36.gl=debug only)
Before=mixdash.service

[Service]
Type=oneshot
RemainAfterExit=yes
StandardOutput=journal+console
StandardError=journal+console
# - because a probe is not a precondition: whatever it says, the dashboard still
# gets its attempt.
ExecStart=-/bin/sh -c '/run/j36/eglprobe -f 1 2>&1 | tee /run/j36/eglprobe.log'
# And the two library questions as well.  The replay stays on mixdash.service, where
# it belongs: the dashboard covers the panel with its own drawing, so the only time
# the probe's verdict can be read is after the shell has exited.
ExecStart=-/bin/sh -c '/run/j36/eglprobe 2>&1 | tee -a /run/j36/eglprobe.log'
ExecStart=-/bin/sh -c 'LIBGL_ALWAYS_SOFTWARE=1 /run/j36/eglprobe -s 2>&1 | tee -a /run/j36/eglprobe.log'
Environment="EGL_LOG_LEVEL=debug"
Environment="LIBGL_DEBUG=verbose"
UNITPROBE
        cat >> /newroot/run/systemd/system/mixdash.service <<'UNITDBGREPLAY'

[Service]
ExecStopPost=-/bin/sh -c 'echo "--- eglprobe, repeated now that the shell has exited ---"; cat /run/j36/eglprobe.log'
UNITDBGREPLAY
        say "dash: j36.gl=debug, so mixdash-probe.service runs the probe once per boot"
        say "      -- it paints bars over the splash and gives fbcon the console back"
    elif [ "$probe_ready" = 1 ]; then
        say "dash: eglprobe is staged but NOT run at boot: -f paints colour bars into"
        say "      /dev/fb0 and puts tty0 back into KD_TEXT, which is the flash the"
        say "      splash used to show.  Add j36.gl=debug for it, or run"
        say "      /run/j36/eglprobe -f from a shell afterwards"
    fi

    # ── WARMING EGL, WHICH IS THE SAME BINARY WITH NO ARGUMENTS ─────────────────
    #
    # WHAT IS SLOW.  The first thing on this board to want graphics pays for the
    # whole stack coming up: the loader maps libEGL, libgbm, libGLESv2 and the
    # DRI driver behind them, every one of their DT_NEEDEDs comes off the card,
    # lima's first open of the render node sets up its context, and Mesa builds
    # the config table.  On a Cortex-A7 reading a microSD that is most of a
    # second, and it is paid by whatever the operator just launched -- so it
    # reads as "the emulator takes a moment to start" rather than as boot time.
    #
    # WHAT THIS DOES.  Runs it once, in the background, after the dashboard is
    # already on the panel, where nobody is waiting.  Everything above is then
    # in the page cache and in the kernel's lima state, and the app that follows
    # finds it warm.  Nothing is left running: the probe exits, and what remains
    # is cache.
    #
    # WHY THE PROBE AND NOT A PROGRAM WRITTEN FOR THIS.  Because with no
    # arguments it is exactly this and nothing else.  main() with argc == 1 does
    # a read-only fb_report -- two ioctls, no writes -- then load(), then probe()
    # on the display node and on renderD128: eglInitialize, the config table,
    # and one context per API.  It is the -f, -p, -c, -o and -z modes that paint
    # or modeset, and none of them is passed here.  See the paragraph above for
    # what happened the last time this binary ran at boot with -f.
    #
    # AND ITS OUTPUT GOES TO THE JOURNAL AND NOWHERE ELSE.  journal+console here
    # would be forty lines of Mesa drawn over a dashboard that has just finished
    # painting, which is the failure console.h in mixdash exists to prevent; the
    # same argument holds for anything else this board starts behind it.
    if [ "$probe_ready" = 1 ] && [ "$gl_ready" = 1 ] && [ "$gl_debug" != 1 ]; then
        cat > /newroot/run/systemd/system/j36-glwarm.service <<UNITGLWARM
# Written by the J36 Ultra initramfs, into a tmpfs.  Not on the card.
[Unit]
Description=Warm the EGL stack so the first graphics app does not pay for it
# After, and not Requires: a dashboard that failed is a board somebody is about
# to run something on by hand, and the warm-up is worth just as much there.
After=mixdash.service
[Service]
Type=oneshot
RemainAfterExit=yes
Environment="LD_LIBRARY_PATH=/run/j36/gl:$mixos_root/qt/lib"
# Mesa keeps its compiled shaders under XDG_CACHE_HOME and disables the cache
# outright when it has nowhere to put them -- which, for a unit running as root
# with no HOME, is what would happen.  A tmpfs is the right home for it on a
# machine whose only disk is the card it boots from: the cache is worth having
# within one boot, which is exactly what this unit is filling it for, and it is
# not worth a single write to the card.  The ceiling is there because Mesa's
# own default is measured in gigabytes and this board has neither the RAM nor
# the shaders to need it.
Environment="XDG_CACHE_HOME=/run/j36/glcache"
Environment="MESA_SHADER_CACHE_MAX_SIZE=32M"
ExecStartPre=-/bin/mkdir -p /run/j36/glcache
# Leading dash: a probe that could not make a context is a board with no GL, and
# that is a thing to read in the journal, not a failed unit to restart.
ExecStart=-/run/j36/eglprobe
StandardOutput=journal
StandardError=journal
# Behind the dashboard in both queues.  The whole point is that this costs
# nobody anything; a warm-up that made the first frame late would be a loss.
Nice=10
IOSchedulingClass=idle
TimeoutStartSec=120
[Install]
WantedBy=multi-user.target
UNITGLWARM
        mkdir -p /newroot/run/systemd/system/multi-user.target.wants
        ln -sf ../j36-glwarm.service \
               /newroot/run/systemd/system/multi-user.target.wants/j36-glwarm.service
        say "dash: j36-glwarm.service brings EGL up once after the dashboard, so the"
        say "      first app that wants graphics finds the stack already warm"
    fi

    say "dash: mixdash.service is the shell"
    say "      $mixos_root/bin/mixdash, Qt on linuxfb over /dev/fb0"
    if [ "$gl_ready" != 1 ]; then
        say "dash: /run/j36/gl has no usable Mesa payload, so the 3D cube card will"
        say "      find no libEGL.  The dashboard itself does not need one."
    fi
    return 0
}

# ── External USB disks, mounted the way every other Linux does it ─────────────
#
# WHAT THIS BOARD DID BEFORE.  Nothing.  A stick went in, musb enumerated it, and
# usb-storage was not in the kernel at all -- SCSI was refused at the top of the
# build script and the class driver with it -- so the port printed a VID:PID and
# produced no block device.  That half is fixed in the kernel configuration; this
# is the other half, and without it a /dev/sda would appear and sit there.
#
# THREE FILES AND NOT A DAEMON.  What a desktop distribution runs here is udisks2
# behind a session bus, a policy engine and a file manager asking it for things.
# None of those three exists on this image and none of them is needed: udev already
# runs blkid on every block device that appears, so the filesystem type and the
# label are known before anything of ours runs, and systemd already tracks device
# units.  So the whole mechanism is a rule that says "want this unit", a template
# unit bound to the device, and a shell script that mounts.  Unplug is handled by
# BindsTo=: when the device unit goes away systemd stops the instance, which runs
# ExecStop, which unmounts.  Nothing polls.
#
# WRITTEN FROM THE INITRAMFS INTO /run, like every other unit this file generates,
# and here that placement is more than habit.  The kernel modules that make a USB
# disk possible are on the BOOT partition, in j36/usb/, staged by the same build
# that produced this initramfs.  Putting the userspace half in the rootfs would let
# the two drift: a card whose /opt/mixos came from an older sd-root.tar.gz would
# have the modules and not the rules.  Here they ship and update together, and a
# card pulled into an R36S carries none of it.
#
# WHY IT IS SAFE TO MATCH sd*.  On this SoC there is no SATA, no PATA and no other
# SCSI host of any kind -- ATA is refused by name in the kernel configuration -- so
# a /dev/sd* node can only have arrived through usb-storage.  The card itself is
# mmcblk0 and is never matched.
setup_automount() {
    if ! ensure_run_tmpfs; then
        say "automount: no tmpfs on the rootfs /run, so no rules can be written"
        return 1
    fi
    mkdir -p /newroot/run/j36/bin /newroot/run/udev/rules.d /newroot/run/systemd/system

    # ── the rule ─────────────────────────────────────────────────────────────
    #
    # /run/udev/rules.d is read alongside /etc/udev/rules.d and
    # /usr/lib/udev/rules.d, and this is written before switch_root, so udevd has
    # not started yet and there is nothing to reload.
    #
    # ID_FS_USAGE is what blkid decided the thing is, imported by udev's own
    # 60-persistent-storage.rules long before a 99- file runs.  "filesystem" is the
    # one value worth acting on: a partitioned whole disk has no usage at all, a
    # LUKS container reads "crypto", and a RAID member reads "raid".  Testing it is
    # what makes one rule handle both a partitioned stick (three events, one per
    # partition, the disk itself ignored) and a superfloppy-formatted one (one
    # event, on the disk).
    #
    # SYSTEMD_WANTS is the documented hand-off from udev to PID 1 and the reason
    # there is no RUN+= here.  A RUN+= command runs inside udev's own event worker:
    # it is killed after 180 s, it cannot survive the event, and mounting from
    # inside it is a known way to deadlock udev against the mount it just made.
    # SYSTEMD_WANTS starts a real unit, in its own cgroup, with its own lifetime.
    cat > /newroot/run/udev/rules.d/99-mixos-automount.rules <<'RULES'
# Written by the J36 Ultra initramfs, into a tmpfs.  See setup_automount in /init.
ACTION!="add", GOTO="mixos_automount_end"
SUBSYSTEM!="block", GOTO="mixos_automount_end"
# The only SCSI host on this board is usb-storage; there is no ATA and no other.
KERNEL!="sd[a-z]*", GOTO="mixos_automount_end"
ENV{ID_FS_USAGE}!="filesystem", GOTO="mixos_automount_end"
TAG+="systemd"
ENV{SYSTEMD_WANTS}+="mixos-automount@%k.service"
LABEL="mixos_automount_end"
RULES

    # ── the unit ─────────────────────────────────────────────────────────────
    #
    # BindsTo AND After on the device unit, which is the whole unplug story.  BindsTo
    # is what makes the instance stop when the device unit goes away -- and it goes
    # away the moment udev sees the remove event, whether the disk was pulled or
    # powered off.  After orders the start so the node exists before ExecStart runs.
    #
    # Conflicts=umount.target and Before=umount.target put the unmount in the right
    # place at shutdown as well, ahead of the point where systemd starts taking
    # filesystems down itself.
    #
    # /bin/sh with the script as an argument, rather than the script as ExecStart:
    # it lives in /run, and while systemd does not mount /run noexec, a distribution
    # that decided to would turn this into a permission error at unplug time -- the
    # one moment there is nobody watching a console.  Running it through sh cannot
    # care.
    #
    # Type=oneshot with RemainAfterExit: the mount outlives the process that made
    # it, so the unit has to be considered active after ExecStart returns or systemd
    # would never run ExecStop.
    cat > /newroot/run/systemd/system/mixos-automount@.service <<'UNITMOUNT'
# Written by the J36 Ultra initramfs, into a tmpfs.  One instance per volume.
[Unit]
Description=Automount the USB volume on %I
BindsTo=dev-%i.device
After=dev-%i.device
DefaultDependencies=no
Conflicts=umount.target
Before=umount.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh /run/j36/bin/mixos-automount add %I
ExecStop=/bin/sh /run/j36/bin/mixos-automount remove %I
# A disk that has been yanked mid-write can take a while to give up its writeback.
# The script falls back to a lazy unmount, so this is the ceiling on the tidy path
# rather than on the whole thing.
TimeoutStopSec=30
StandardOutput=journal
StandardError=journal
UNITMOUNT

    # ── the script ───────────────────────────────────────────────────────────
    #
    # This one runs in the ROOTFS, not in the initramfs, so it is ordinary POSIX
    # shell with Debian's userland under it -- the applet list in the build script
    # has nothing to say about it.
    cat > /newroot/run/j36/bin/mixos-automount <<'AUTOMOUNT'
#!/bin/sh
# mixos-automount -- mount and unmount external USB volumes on the J36 Ultra.
#
# Written by the initramfs into /run.  Called only by mixos-automount@.service,
# which udev instantiates for a block device carrying a filesystem:
#
#     mixos-automount add    sda1
#     mixos-automount remove sda1
#
# The argument is a KERNEL NAME, never a path.  Everything this script decides is
# derived from it: /dev/<name> is the node, and the udev database is asked what is
# on it.  Nothing is passed in from the rule, because a rule's environment does not
# reach a unit started through SYSTEMD_WANTS.
#
# set -f, and it is not decoration.  Splitting a mountinfo line into fields is done
# with `set -- $line', which is field splitting AND pathname expansion -- and a
# mount option or a volume label holding a `*' would then be replaced by whatever
# happens to be in the current directory.  Nothing here wants a glob.
set -u
set -f
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

STATEDIR=/run/mixos/volumes

say() { echo "mixos-automount: $*"; }

action="${1:-}"
name="${2:-}"
if [ -z "$action" ] || [ -z "$name" ]; then
    say "usage: mixos-automount add|remove <kernel name>"
    exit 64
fi
# A kernel name and not a path, asserted rather than assumed: everything below
# builds paths out of it, and this is the only place it can be checked once.
case "$name" in
    */*|.|..|"") say "refusing \"$name\": that is not a kernel device name"; exit 64 ;;
esac
dev="/dev/$name"
state="$STATEDIR/$name"

# Every mount point currently carrying $1 as its source, one per line.
#
# Parsed out of /proc/self/mountinfo rather than asked of findmnt, for one reason:
# this runs at unplug, when the device node is already gone.  mountinfo keeps the
# source as the string it was mounted with, so it still answers; a tool that stats
# the device does not.  The line splits on " - ", with the mount point as field 5
# of the front half and the source as field 2 of the back half.
mounts_of() {
    _want="$1"
    while IFS= read -r _line; do
        case "$_line" in *" - "*) ;; *) continue ;; esac
        _pre="${_line%% - *}"
        _post="${_line#* - }"
        # shellcheck disable=SC2086
        set -- $_post
        [ "${2:-}" = "$_want" ] || continue
        # shellcheck disable=SC2086
        set -- $_pre
        [ -n "${5:-}" ] && printf '%s\n' "$5"
    done < /proc/self/mountinfo
}

case "$action" in
add)
    # What is on it, from the udev database first.  udev has already run blkid for
    # this device -- that is what set ID_FS_USAGE, which is what made the rule fire
    # -- so this is a lookup and not a second probe of the disk.  blkid is the
    # fallback for the case where this was invoked by hand.
    props="$(udevadm info --query=property --name="$dev" 2>/dev/null || true)"
    fstype="$(printf '%s\n' "$props" | sed -n 's/^ID_FS_TYPE=//p' | head -n 1)"
    label="$(printf '%s\n' "$props" | sed -n 's/^ID_FS_LABEL=//p' | head -n 1)"
    if [ -z "$fstype" ]; then
        fstype="$(blkid -o value -s TYPE "$dev" 2>/dev/null || true)"
        label="$(blkid -o value -s LABEL "$dev" 2>/dev/null || true)"
    fi
    if [ -z "$fstype" ]; then
        say "$dev carries no filesystem udev could name; leaving it alone"
        exit 0
    fi

    # Already mounted somewhere -- by fstab, by hand, or by a previous run of this
    # script that systemd has restarted.  Mounting a second time would give the
    # same filesystem two mount points and two chances to be the one nobody
    # unmounts.
    existing="$(mounts_of "$dev" | head -n 1)"
    if [ -n "$existing" ]; then
        say "$dev is already mounted at $existing"
        exit 0
    fi

    # The name on the glass.  A label is what the operator wrote on the disk, so it
    # is the first choice, but it arrives from a filesystem somebody else formatted
    # and may hold anything at all -- slashes, spaces, control characters, four
    # hundred bytes of it.  tr keeps the characters that can be a directory name and
    # nothing else; cut bounds the length; the kernel name is the fallback for a
    # disk with no label or a label that sanitised away to nothing.
    safe="$(printf '%s' "$label" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-32)"
    # Trim what the substitution left at the ends, so " Backup " does not become
    # "_Backup_" -- and so a label written entirely in characters this cannot keep
    # collapses to nothing and falls through to the kernel name below.
    while :; do
        case "$safe" in
            _*) safe="${safe#_}" ;;
            *_) safe="${safe%_}" ;;
            *)  break ;;
        esac
    done
    case "$safe" in
        ''|.|..) safe="" ;;
    esac
    [ -n "$safe" ] || safe="$name"

    # /media if the rootfs will take it, /run/media if it will not.  The second is
    # where systemd's own tooling puts removable media and is always writable,
    # being a tmpfs; the first is where anyone looking for it would look first.
    base=/media
    mkdir -p "$base" 2>/dev/null || true
    if [ ! -d "$base" ] || [ ! -w "$base" ]; then
        base=/run/media
        mkdir -p "$base" || { say "no writable place to mount $dev"; exit 1; }
    fi

    # Two disks both labelled BACKUP is not a hypothetical.  Anything already there
    # and in use gets a suffix rather than a mount over the top of it.
    target="$base/$safe"
    i=1
    while [ -e "$target" ] && [ -n "$(ls -A "$target" 2>/dev/null || true)" ]; do
        i=$((i + 1))
        if [ "$i" -gt 16 ]; then
            say "$base has too many volumes called $safe"
            exit 1
        fi
        target="$base/${safe}_$i"
    done
    mkdir -p "$target" || { say "cannot create $target"; exit 1; }

    # Who owns the files.  vfat, exfat and ntfs3 have no on-disk ownership, so
    # whatever uid is passed at mount time is the uid of every file on the volume;
    # pass none and that uid is root, which makes a stick read-only to the person
    # holding the handheld.  The login user is looked up rather than assumed:
    # `virtua' is what this tree builds, and `ark' is what it was called before the
    # rename -- which matters because --mix-only drops this payload onto a card that
    # is already out there rather than onto a rootfs this run made.
    ouid=""; ogid=""
    for candidate in virtua ark; do
        while IFS=: read -r u _p uid gid _rest; do
            [ "$u" = "$candidate" ] || continue
            ouid="$uid"; ogid="$gid"
            break
        done < /etc/passwd
        [ -n "$ouid" ] && break
    done
    [ -n "$ouid" ] || { ouid=1000; ogid=1000; }

    # nosuid and nodev on everything, because this is somebody else's disk and a
    # setuid root binary or a device node on it would be theirs to choose.  exec is
    # NOT taken away: this is meant to be a console PC station, and a program run
    # off a stick is a thing people do on one.
    common="nosuid,nodev,noatime"
    case "$fstype" in
        vfat)
            opts="rw,$common,uid=$ouid,gid=$ogid,umask=0002,flush,iocharset=utf8,shortname=mixed" ;;
        exfat)
            # No flush option in this driver, and passing one it does not know is a
            # failed mount rather than an ignored word.
            opts="rw,$common,uid=$ouid,gid=$ogid,umask=0002,iocharset=utf8" ;;
        ntfs|ntfs3)
            # ntfs3 is the only NTFS driver in this kernel, and it spells the
            # charset option `nls' where the FAT drivers spell it `iocharset'.
            fstype=ntfs3
            opts="rw,$common,uid=$ouid,gid=$ogid,umask=0002,nls=utf8" ;;
        iso9660|udf)
            opts="ro,$common,uid=$ouid,gid=$ogid" ;;
        *)
            # ext2/3/4, btrfs, f2fs, xfs and anything else: real on-disk ownership,
            # so uid= would be rejected and is not wanted.
            opts="rw,$common" ;;
    esac

    # The ladder, and each rung is a different thing having gone wrong.  Tuned
    # options first.  Then the same type with nothing but the safety options, which
    # is the answer to a driver that did not recognise one of them.  Then read-only,
    # which is the answer to a dirty NTFS volume -- Windows fast-booted out of
    # rather than shut down -- and to a filesystem with errors.  Then read-only with
    # no type at all, letting the kernel try each one it has.
    #
    # err is kept so the message says what the LAST attempt said, which is the one
    # that matters.
    mounted=0
    err=""
    for attempt in "$fstype|$opts" "$fstype|rw,$common" "$fstype|ro,$common" "|ro,$common"; do
        t="${attempt%%|*}"
        o="${attempt#*|}"
        if [ -n "$t" ]; then
            err="$(mount -t "$t" -o "$o" "$dev" "$target" 2>&1)" && { mounted=1; break; }
        else
            err="$(mount -o "$o" "$dev" "$target" 2>&1)" && { mounted=1; break; }
        fi
    done
    if [ "$mounted" != 1 ]; then
        rmdir "$target" 2>/dev/null || true
        say "could not mount $dev ($fstype): $err"
        exit 1
    fi

    # Read-only or not, asked of the kernel rather than inferred from which rung of
    # the ladder succeeded: a driver is free to downgrade a mount by itself, and
    # ntfs3 does exactly that.  Field 6 of a mountinfo line is the per-mount option
    # list, and `ro' or `rw' is always the first word in it.
    ro=0
    while IFS= read -r line; do
        case "$line" in *" - "*) ;; *) continue ;; esac
        set -- ${line%% - *}
        [ "${5:-}" = "$target" ] || continue
        case "${6:-}" in ro|ro,*) ro=1 ;; esac
        break
    done < /proc/self/mountinfo

    # A state file, so that remove does not have to re-derive any of this and the
    # dashboard can list what is mounted without parsing mountinfo itself.  In /run,
    # so it cannot outlive the boot that made it.
    mkdir -p "$STATEDIR" 2>/dev/null || true
    {
        echo "device=$dev"
        echo "mountpoint=$target"
        echo "fstype=$fstype"
        echo "label=$label"
        echo "readonly=$ro"
    } > "$state" 2>/dev/null || true

    if [ "$ro" = 1 ]; then
        say "mounted $dev ($fstype) read-only at $target"
    else
        say "mounted $dev ($fstype) at $target"
    fi
    exit 0
    ;;

remove)
    # The state file first, because it is exact.  mountinfo is the fallback for a
    # mount this script did not make, or one whose state file went with a /run that
    # was remounted.
    target=""
    if [ -r "$state" ]; then
        target="$(sed -n 's/^mountpoint=//p' "$state" | head -n 1)"
    fi
    [ -n "$target" ] || target="$(mounts_of "$dev" | head -n 1)"
    if [ -z "$target" ]; then
        rm -f "$state" 2>/dev/null || true
        exit 0
    fi

    # Plain unmount first: it flushes, and on a disk that is still physically there
    # -- a "safely remove" from the dashboard, a shutdown -- that is the one that
    # matters.  Lazy second, because after a yank the writeback has nowhere to go
    # and a plain unmount would block until the timeout with the mount still in the
    # tree afterwards.  Nothing is forced on the first try.
    if ! umount "$target" 2>/dev/null; then
        sync
        umount -l "$target" 2>/dev/null || true
    fi

    # Only ever the directory this script made, only when it is empty, and never
    # the mount root itself.
    case "$target" in
        /media/?*|/run/media/?*) rmdir "$target" 2>/dev/null || true ;;
    esac
    rm -f "$state" 2>/dev/null || true
    say "unmounted $target"
    exit 0
    ;;

*)
    say "unknown action \"$action\""
    exit 64
    ;;
esac
AUTOMOUNT
    chmod 0755 /newroot/run/j36/bin/mixos-automount

    say "automount: USB disks land under /media, one directory per volume label"
    return 0
}

# ── `default', so that sound is a property of the machine ─────────────────────
#
# WHAT WAS WRONG.  Everything on this image that plays a sound and does not name a
# device names `default', and `default' on this card was the RG351MP's:
#
#     pcm.!default { type plug  slave.pcm "dmixer" }
#     pcm.dmixer   { type dmix  ipc_key 1024
#                    slave { pcm "hw:0,0" period_size 1024 buffer_size 4096
#                            rate 44100 } }
#
# finishing_touches.sh links /etc/asound.conf to /home/virtua/.asoundrc, and that
# is the file.  Three things are wrong with it here and only the first is fatal:
# the RK3326-era geometry is hard-coded, the rate is hard-coded, and dmix puts a
# shared-memory software mixer between every player and a DAC that has exactly one
# consumer.  The dashboard sidestepped all of it by naming `plughw:C,D' itself --
# which is why films had sound and nothing else on the machine did.  Sidestepping
# is not the same as fixing: aplay, mpv, SDL, an emulator, anything installed from
# the Packages page, all of them ask for `default'.
#
# WHAT THIS DOES.  Writes the two-stanza file this board actually wants into the
# tmpfs and BIND-MOUNTS it over the one on the card.  A bind mount is not a write:
# the bytes on the card are untouched, the same card in an R36S gets its own file
# back, and there is nothing to undo.  That matters more than usual here -- .asoundrc
# lives in /home/virtua, which is a directory on the rootfs and therefore shared with
# the other launcher, and the invariant this whole initramfs is built on is that
# nothing on the shared rootfs is written.
#
# WHY IT CANNOT BE A DROP-IN INSTEAD.  alsa-lib's hook list loads, in order,
# /usr/share/alsa/alsa.conf.d/, /etc/alsa/conf.d/, /etc/asound.conf and finally
# ~/.asoundrc.  Later wins, and pcm.!default is an override in both files -- so a
# conf.d file, which is the polite way to do this, loses to the very file it is
# trying to correct.  The only place that beats ~/.asoundrc is ~/.asoundrc.
#
# WHY BOTH PATHS ARE TRIED.  /etc/asound.conf is a symlink to ~/.asoundrc on this
# image, so one bind normally covers both; readlink -f collapses them and the loop
# skips the duplicate.  A card where somebody replaced the symlink with a real file
# gets both bound, which is the only way to be right about a rootfs this old.
#
# GATED ON A CARD HAVING REGISTERED, and deliberately so.  Without j36.audio there
# is no /dev/snd, and pointing `default' at a card that does not exist would turn a
# silent machine into a machine where every player fails to open a device.  No
# card, no change, and the original file stands.
#
# NO dmix, AND THAT IS A DECISION.  It buys one thing -- several processes sharing
# the DAC -- and this AFE has no playback interrupt: the driver polls the DL1
# cursor from a work item and calls snd_pcm_period_elapsed() from there.  A
# software mixer on top of that is latency and CPU on eight A7s for a case that
# does not arise on a handheld running one thing at a time.  The cost is that the
# second opener gets EBUSY while the first is playing.  Anyone who wants the trade
# the other way replaces the slave in the file below with a dmix block; the
# boot.conf notes carry the stanza.
setup_asound() {
    if [ -z "$rootdev" ]; then
        say "asound: no rootfs was found, so there is no default to correct"
        return 1
    fi
    if [ ! -d /dev/snd ]; then
        say "asound: no card registered, so \`default' is left as the rootfs has it"
        return 1
    fi
    if ! ensure_run_tmpfs; then
        say "asound: no tmpfs on the rootfs /run, so nothing can be staged"
        return 1
    fi
    mkdir -p /newroot/run/j36/bin /newroot/run/systemd/system

    # hw:CARD=j36 and not hw:0.  The id is set by the driver itself -- the third
    # argument to snd_devm_card_new in j36_mt6592_audio.c is the literal "j36" --
    # so it is a promise rather than a guess, and it survives a USB headset or an
    # HDMI adapter enumerating first and taking card 0.
    #
    # plug over hw, which is what `default' has always meant: rate, format and
    # channel conversion in alsa-lib for anything that does not match the AFE's one
    # PCM -- s16, stereo, 8 to 48 kHz.  A mono 22 kHz wav plays; the card is never
    # asked for something it does not do.
    cat > /newroot/run/j36/asound.conf <<'ASOUNDRC'
# The J36 Ultra's default sound device.
#
# Written by the MixOS initramfs into a tmpfs and bind-mounted over the file this
# card shipped with, which was the RG351MP's and named an RK3326 dmix.  The bytes
# on the card are untouched: boot another launcher and you get its file back.
#
# hw:CARD=j36 is the MT6592 AFE's DL1 playback path.  The id comes from the driver
# and does not move when something else enumerates as card 0.
pcm.!default {
	type plug
	slave.pcm "hw:CARD=j36,DEV=0"
}

# So that amixer and alsamixer with no -c land on the same card.  The controls
# here are "Master Playback Volume", "Master Playback Switch", "Speaker Amp" and
# "Headphone" -- the last two are which output the sound comes out of, and both
# on at once is a real setting.  Master moves whichever of them is up.
ctl.!default {
	type hw
	card j36
}
ASOUNDRC

    cat > /newroot/run/j36/bin/j36-asound <<'ASOUNDSH'
#!/bin/sh
# j36-asound -- put this board's default PCM in front of the card's own.
#
# Written by the J36 Ultra initramfs into /run and started once by
# j36-asound.service, after local-fs.target.  The ordering is kept for the cards
# written before this layout, where /home/virtua was a separate partition carrying
# the file being covered: binding before it was mounted would have covered the wrong
# file and the mount would have hidden the work.  On a current card /home/virtua is a
# directory on the root filesystem and the ordering is simply free.
#
# Nothing here is a write.  Every path is bind-mounted over, so the card keeps
# whatever it had and a reboot into anything else sees it.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin

src=/run/j36/asound.conf
[ -f "$src" ] || { echo "j36-asound: $src is missing"; exit 0; }

done_targets=""
bound=0
for p in /etc/asound.conf /home/virtua/.asoundrc /root/.asoundrc; do
	# -f after the resolve, so a dangling symlink is skipped rather than being
	# a mount target that does not exist.
	t=$(readlink -f "$p" 2>/dev/null) || continue
	[ -n "$t" ] && [ -f "$t" ] || continue
	case " $done_targets " in *" $t "*) continue ;; esac
	done_targets="$done_targets $t"
	if mount --bind "$src" "$t"; then
		echo "j36-asound: default now plays through hw:CARD=j36 (over $t)"
		bound=$((bound + 1))
	else
		echo "j36-asound: could not bind over $t"
	fi
done

if [ "$bound" = 0 ]; then
	# Not a failure.  A rootfs with no asound.conf and no .asoundrc has no
	# wrong default to correct -- alsa-lib's own is already plug over the one
	# card -- so there is nothing to do and nothing to report as broken.
	echo "j36-asound: nothing to cover; alsa-lib's own default stands"
fi

# ── the second job: the libasound that aplay cannot link against ──────────────
#
#	aplay: symbol lookup error: undefined symbol: snd_pcm_subformat_value
#
# snd_pcm_subformat_value arrived in alsa-lib 1.2.10.  aplay is from the
# alsa-utils apt installed and expects it; the copy of libasound.so.2 that the
# loader actually hands it predates it.  Both are soname 2 so ld.so is content,
# and the program dies at the first call instead.
#
# That is a rootfs the R36S image shipped with, not something this build made,
# and it is not confined to aplay: every alsa-lib caller on the machine resolves
# through the same shadowing copy.  So it is fixed the same way `default' is --
# by covering the wrong file rather than replacing it, which keeps the card
# bootable by anything else.
#
# THE TEST IS THE SYMBOL AND NOTHING ELSE.  Not a version string, not a file
# date, not a size: the exported name lives in .dynstr as plain bytes, so a
# library that has it can satisfy aplay and one that does not cannot, which is
# exactly the question being asked.  grep -q rather than -a, because -q alone
# gives the right exit status on a binary and busybox grep has no -a.
#
# THE ORDER OF THE DIRECTORIES IS THE POLICY.  The multiarch directories come
# first because that is where apt keeps the copy it updates; /usr/local and /opt
# come after because that is where a handheld image drops the build it froze
# years ago.  The first complete library found wins and is bound over every
# incomplete one, so the frozen copy stops being what the loader picks.
good=""
for d in /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf \
         /usr/lib /lib /usr/local/lib/arm-linux-gnueabihf /usr/local/lib \
         /opt/lib /opt/system/lib; do
	[ -d "$d" ] || continue
	for f in "$d"/libasound.so.2 "$d"/libasound.so.2.*; do
		[ -f "$f" ] || continue
		grep -q snd_pcm_subformat_value "$f" 2>/dev/null || continue
		good=$(readlink -f "$f" 2>/dev/null)
		[ -n "$good" ] && break
		good=""
	done
	[ -n "$good" ] && break
done

if [ -z "$good" ]; then
	# Nothing on the card exports it.  Say so plainly and stop: there is no
	# file here to put in front, and a build that wants aplay working from
	# this state has to ship a newer alsa-lib, which is not a decision an
	# initramfs gets to make at boot.
	echo "j36-asound: no libasound.so.2 here exports snd_pcm_subformat_value"
	exit 0
fi

seen_libs=" $good "
covered=0
for d in /usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf \
         /usr/lib /lib /usr/local/lib/arm-linux-gnueabihf /usr/local/lib \
         /opt/lib /opt/system/lib; do
	[ -d "$d" ] || continue
	for f in "$d"/libasound.so.2 "$d"/libasound.so.2.*; do
		[ -f "$f" ] || continue
		t=$(readlink -f "$f" 2>/dev/null) || continue
		[ -n "$t" ] || continue
		case " $seen_libs " in *" $t "*) continue ;; esac
		seen_libs="$seen_libs $t"
		grep -q snd_pcm_subformat_value "$t" 2>/dev/null && continue
		if mount --bind "$good" "$t"; then
			echo "j36-asound: $t was too old for aplay; $good is in front of it now"
			covered=$((covered + 1))
		else
			echo "j36-asound: could not bind $good over $t"
		fi
	done
done

if [ "$covered" = 0 ]; then
	echo "j36-asound: libasound is $good, and it has what aplay asks for"
fi
exit 0
ASOUNDSH
    chmod 0755 /newroot/run/j36/bin/j36-asound

    # THE THREE ORDERING LINES ARE ONE DECISION EACH.
    #
    # After=local-fs.target, for the cards written before this layout, where the file
    # being covered was on a separate home partition.  Bind before that partition was
    # mounted and two things went wrong at once: readlink -f resolved /etc/asound.conf
    # into the empty /home/virtua on the rootfs and found nothing to cover, and then
    # the real mount arrived and brought the RG351MP's file back uncovered.
    # RequiresMountsFor names a path and not a device, so it is satisfied by the root
    # mount itself on a current card and by the p3 mount on an old one -- which is why
    # both lines could stay as they were when the partition went away.
    #
    # Before=sysinit.target, which is as early as a unit that needs a mounted
    # filesystem can be.  `default' has to be right before the first process that
    # asks for it, and alsa-lib resolves the name once per open and keeps the
    # handle -- so a player that starts a second too early plays out of the old file
    # for its whole run.
    #
    # DefaultDependencies=no is what makes that legal rather than a cycle: the
    # default set puts After=sysinit.target on every service, and a unit both after
    # and before the same target is a loop systemd breaks by dropping one of them at
    # random.  j36-splash.service above does the same thing for the same reason.
    #
    # RemainAfterExit because the mounts outlive the process that made them, and no
    # ExecStop: they are meant to last as long as the boot does, and unmounting them
    # at shutdown would only uncover a file nothing is going to read again.  The
    # leading `-' on ExecStart is the belt: this runs before sysinit.target, and a
    # unit that can fail there is a unit that can hold up a boot over a sound file.
    cat > /newroot/run/systemd/system/j36-asound.service <<'UNITASOUND'
# Written by the J36 Ultra initramfs, into a tmpfs.  See setup_asound in /init.
[Unit]
Description=Point ALSA at the J36 Ultra's card, and at a libasound aplay can use
DefaultDependencies=no
RequiresMountsFor=/home/virtua
After=local-fs.target
Before=sysinit.target shutdown.target
Conflicts=shutdown.target
ConditionPathExists=/run/j36/asound.conf

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=-/bin/sh /run/j36/bin/j36-asound

[Install]
WantedBy=sysinit.target
UNITASOUND

    mkdir -p /newroot/run/systemd/system/sysinit.target.wants
    ln -sf ../j36-asound.service \
           /newroot/run/systemd/system/sysinit.target.wants/j36-asound.service

    say "asound: default is plug over hw:CARD=j36 for the whole system"
    return 0
}

# ── the name the machine calls itself ─────────────────────────────────────────
#
# WHAT IS WRONG.  Every line of the journal on this board opens with `rg351mp'.  So
# does the netbios name samba advertises, the prompt on a serial console, and the
# name an mDNS browser shows.  It is not a cosmetic leftover of an old build: it is
# the correct hostname for the image this rootfs was made as, and the J36 boots that
# same rootfs.  /etc/hostname on p2 says rg351mp because device/r36-ultra's
# finishing_touches.sh wrote it there with UNIT=rg351mp, and that is the R36's answer.
#
# WHY IT IS NOT FIXED WHERE IT IS WRITTEN.  Because p2 is SHARED.  Changing UNIT, or
# the file it produces, renames the R36 image too -- and the whole arrangement here is
# that one rootfs boots as either board and neither one edits it.  The J36 needs a
# different answer to the same question, which is exactly the shape every other
# correction in this file has: cover the file, do not rewrite it.
#
# WHY THERE IS NO UNIT FOR THIS ONE, unlike j36-asound.service.  systemd reads
# /etc/hostname in PID 1 before it looks at a single unit -- there is no unit early
# enough to beat it, and hostnamed setting the name a second later still leaves the
# first hundred journal lines saying rg351mp.  But the initramfs runs before systemd
# exists at all, /newroot/etc is already mounted here, and a bind put down now moves
# with the root across switch_root.  So this is a plain bind and there is nothing to
# schedule.
#
# AND /etc/hosts WITH IT, WHICH IS NOT OPTIONAL.  A hostname with no 127.0.1.1 line
# to match is what makes sudo pause for ten seconds on `unable to resolve host', and
# on a console with no keyboard that is a boot that looks hung.  The file is derived
# from the rootfs's own rather than written from scratch, so anything an operator put
# in it survives: drop whatever 127.0.1.1 claimed, rename any other mention, append
# the new mapping.
J36_HOSTNAME=virtua

setup_hostname() {
    if [ -z "$rootdev" ]; then
        say "hostname: no rootfs was found, so there is no name to correct"
        return 1
    fi
    if ! ensure_run_tmpfs; then
        say "hostname: no tmpfs on the rootfs /run, so nothing can be staged"
        return 1
    fi
    mkdir -p /newroot/run/j36

    # tr rather than a shell trim, because the applet list has tr and the file
    # ends in a newline that would otherwise become part of the comparison.
    old=""
    if [ -r /newroot/etc/hostname ]; then
        old=$(cat /newroot/etc/hostname 2>/dev/null | tr -d ' \011\015\012')
    fi

    if [ "$old" = "$J36_HOSTNAME" ]; then
        say "hostname: the rootfs already calls this board $J36_HOSTNAME"
        return 0
    fi

    printf '%s\n' "$J36_HOSTNAME" > /newroot/run/j36/hostname
    if ! mount --bind /newroot/run/j36/hostname /newroot/etc/hostname; then
        say "hostname: could not cover /etc/hostname; staying ${old:-unnamed}"
        return 1
    fi

    if [ -r /newroot/etc/hosts ]; then
        # Two passes and they do different jobs.  The delete takes out the
        # 127.0.1.1 line whatever it names, which is the one Debian writes and the
        # one that has to end up pointing at us.  The substitution catches the
        # other places the old name turns up -- some images put it on the
        # 127.0.0.1 localhost line as well -- and is skipped entirely when the
        # rootfs never had a name, since s//virtua/g on an empty pattern would
        # rewrite every line in the file.
        if [ -n "$old" ]; then
            sed -e '/^127\.0\.1\.1[ \011]/d' -e "s/$old/$J36_HOSTNAME/g" \
                /newroot/etc/hosts > /newroot/run/j36/hosts 2>/dev/null
        else
            sed -e '/^127\.0\.1\.1[ \011]/d' \
                /newroot/etc/hosts > /newroot/run/j36/hosts 2>/dev/null
        fi
        printf '127.0.1.1\t%s\n' "$J36_HOSTNAME" >> /newroot/run/j36/hosts
        if mount --bind /newroot/run/j36/hosts /newroot/etc/hosts; then
            say "hostname: $J36_HOSTNAME, and 127.0.1.1 resolves to it"
        else
            # The name is already covered and that is the half that matters; say
            # which half did not take rather than pretending the whole thing failed.
            say "hostname: $J36_HOSTNAME, but /etc/hosts still says ${old:-nothing}"
        fi
    else
        say "hostname: $J36_HOSTNAME (this rootfs has no /etc/hosts to match)"
    fi
    return 0
}

# ── the log somebody can actually read ────────────────────────────────────────
#
# WHY THIS EXISTS.  "Send me the log" has had no answer on this board.  The panel
# is the only output it has, the dashboard covers the panel a few seconds in, and
# the two partitions that hold anything are ext2 -- the one filesystem the machine
# that flashed this card will not mount.  The journal does not survive either:
# cleanup_filesystem.sh deletes /var/log/journal from the shared rootfs, so
# journald falls back to /run and the whole boot's log dies with the power.  What
# was left was photographing a 640x480 screen, which is how the last four bugs
# were reported.
#
# BOOT IS THE ANSWER BECAUSE BOOT IS FAT.  It is the one partition every machine
# that can write this card can also read -- 100 MB, of which the launcher uses
# about twelve, so a megabyte of text is free and the file lands at the top level
# where it is the first thing in the window.
#
# IT IS MOUNTED FOR AS LONG AS IT TAKES TO WRITE ONE FILE AND NOT A SECOND LONGER.
# This is a handheld with a power switch, and a FAT left mounted read-write when
# somebody flicks it is a dirty FAT on the partition the bootloader reads.  So a
# pass mounts, writes under a temporary name, syncs, renames over the previous
# log, syncs again and unmounts: a power cut during the write loses the new log
# and keeps the old one, and a power cut at any other moment finds BOOT not
# mounted at all.
#
# It is staged from here, in a tmpfs, for the same reason everything else is:
# nothing on the shared rootfs is written, so this lands on a card that was
# flashed months ago without rebuilding anything on it.
setup_logdump() {
    if [ -z "$rootdev" ]; then
        say "logdump: no rootfs was found, so there is no systemd to run the dump"
        return 1
    fi
    if ! ensure_run_tmpfs; then
        say "logdump: no tmpfs on the rootfs /run, so nothing can be staged"
        return 1
    fi
    mkdir -p /newroot/run/j36/bin /newroot/run/systemd/system

    # The one thing the rootfs cannot work out as cheaply as we can: this partition
    # has already been mounted and looked inside for mvii/, so hand the answer over
    # rather than making the script repeat the search.  It still knows how to search
    # -- bootdev is empty on a boot where mount_bootfs never ran -- and an empty
    # file is a legitimate value that means exactly that.
    printf '%s\n' "$bootdev" > /newroot/run/j36/bootdev

    cat > /newroot/run/j36/bin/j36-logdump <<'LOGDUMP'
#!/bin/sh
# j36-logdump -- write one diagnostic file onto the FAT BOOT partition.
#
# Written by the J36 Ultra initramfs into /run, and started by j36-logdump.service:
#
#     j36-logdump boot     wait, write, wait longer, write again
#     j36-logdump now      write once, immediately -- the shutdown pass
#
# The output is /mixos-log.txt at the root of BOOT, which is the partition a Mac or
# a Windows machine mounts when the card goes into a reader.
#
# This one runs in the ROOTFS, not in the initramfs, so it is ordinary POSIX shell
# with Debian's userland under it -- but it is a DIAGNOSTIC, which changes what it
# may assume.  A general-purpose Debian need not carry iw, rfkill or usbutils, and
# the whole value of this file is that a missing tool leaves a line saying so
# instead of truncating everything after it.  Hence run() below, and hence no
# `set -e': the one thing this script must not do is stop early.
#
# No `set -f' either, unlike its sibling mixos-automount, because the search for
# the boot partition is a glob over /dev/mmcblk*p* and needs one.  Nothing else
# here expands: every value that comes from outside is a device path and is quoted.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# Both, because journalctl and systemctl each check their own and a pager here
# would be a process waiting forever for a terminal that does not exist.
SYSTEMD_PAGER=
SYSTEMD_COLORS=0
export SYSTEMD_PAGER SYSTEMD_COLORS
# And English, for everything below.  nmcli, systemctl and ip all translate their
# own words, and a log is read by whoever is debugging the board rather than by
# whoever is holding it -- a Portuguese handheld should still produce a log that
# the same grep finds something in.
LC_ALL=C
export LC_ALL

MNT=/run/j36/bootmnt
NAME=mixos-log.txt
TMP=mixos-log.new
BOOTDEV=""

# ── finding and mounting BOOT ─────────────────────────────────────────────────
#
# Identified by looking inside it, not by partition number: numbering here follows
# whichever MMC host attached first.  mvii/ is what the LK reads and the one
# directory BOOT always carries; j36/ is accepted because that is what a card from
# an older layout has.
#
# Mounting a block device that is already mounted elsewhere is safe on Linux -- the
# second mount finds the existing superblock and shares it rather than making a
# second one -- so no check for that is needed, and a card whose fstab does mount
# BOOT still gets its log.
mount_boot() {
    _saved=""
    if [ -r /run/j36/bootdev ]; then
        _saved="$(cat /run/j36/bootdev 2>/dev/null)"
    fi
    mkdir -p "$MNT"
    for _d in $_saved /dev/mmcblk*p*; do
        [ -b "$_d" ] || continue
        mount -t vfat -o rw,noatime "$_d" "$MNT" 2>/dev/null || continue
        if [ -d "$MNT/mvii" ] || [ -d "$MNT/j36" ]; then
            BOOTDEV="$_d"
            return 0
        fi
        umount "$MNT" 2>/dev/null || true
    done
    return 1
}

# ── the four emitters everything below is built from ──────────────────────────
sec() { printf '\n\n===== %s =====\n' "$*"; }

# A command, its output, and what happened to it.  The exit status is printed
# rather than swallowed: `ip link' exiting 1 and `ip link' printing nothing are
# different findings, and on a board with no network interface it is the second.
run() {
    sec "$*"
    if command -v "$1" >/dev/null 2>&1; then
        "$@" 2>&1 || printf '(exited %s)\n' "$?"
    else
        printf '(%s: not installed on this rootfs)\n' "$1"
    fi
}

show() {
    sec "$1"
    if [ -r "$1" ]; then cat "$1" 2>&1; else printf '(absent)\n'; fi
}

# /sys is full of one-value files and the value means nothing without the path it
# came from, so every one is named as it goes.  The label is separate because the
# caller passes an expanded glob and a header made of twenty paths is not a header.
showall() {
    _lbl="$1"; shift
    sec "$_lbl"
    _any=0
    for _f in "$@"; do
        [ -e "$_f" ] || continue
        _any=1
        printf -- '--- %s\n' "$_f"
        cat "$_f" 2>/dev/null || printf '(unreadable)\n'
    done
    [ "$_any" = 1 ] || printf '(nothing matched)\n'
}

# The whole ring buffer is at the end of this file.  These are the same lines
# pulled forward under the name of the bug they belong to, because the difference
# between a log that gets read and one that does not is whether the answer is in
# the first screen.
kgrep() {
    _lbl="$1"; _pat="$2"
    sec "kernel messages about $_lbl"
    dmesg 2>/dev/null | grep -iE "$_pat" || printf '(no line matched)\n'
}

dump() {
    printf 'MixOS -- J36 Ultra boot diagnostic\n'
    printf 'pass: %s\n' "$1"
    printf 'Read the four numbered sections first; the raw dmesg and journal are\n'
    printf 'at the end.  Nothing here is secret except any Wi-Fi passphrase you\n'
    printf 'typed, and none is included.\n'
    run date
    run uptime
    run uname -a
    show /proc/cmdline
    show /etc/j36-build
    show /etc/os-release

    printf '\n\n########## 0.  WHAT FAILED ##########\n'
    run systemctl --no-pager --no-legend --failed
    run systemctl --no-pager --no-legend list-units --state=activating
    sec "the last 400 lines at warning or worse"
    journalctl -b --no-pager -p warning -n 400 2>&1 || printf '(journalctl failed)\n'

    printf '\n\n########## 1.  WI-FI ##########\n'
    run ls -l /sys/class/net
    run ip -d link
    run ip addr
    run iw dev
    run rfkill list

    # ── who owns wlan0, and what it did with it ──────────────────────────────
    #
    # NetworkManager manages this radio in full -- "managed-type: 'full'" in its
    # own log -- which means it is the thing that associates, runs DHCP, sets the
    # route, writes the resolver and remembers the network for next time.  All
    # five of those jobs are dumped here, because a log that had only `ip addr'
    # could not tell "the AP never answered" from "the AP answered and nothing
    # ever asked it for an address", and those are different bugs with the same
    # symptom.
    #
    # THE DHCP4.OPTION LINES IN `device show' ARE THE LEASE ITSELF.  An address in
    # 169.254.0.0/16 with no DHCP4 lines beside it is not a network with a strange
    # mask -- it is IPv4LL, the address a DHCP client invents for itself after it
    # has given up on the router, and seeing the two together in one dump is the
    # whole of that bug on one screen.
    run systemctl --no-pager status NetworkManager
    run nmcli general status
    run nmcli radio all
    run nmcli device status
    run nmcli device show
    run nmcli connection show
    run ip -4 route
    show /etc/resolv.conf
    # NAMES AND DATES ONLY, DELIBERATELY.  Every file in that directory has a
    # Wi-Fi passphrase in it in plain text, and this log is written to the FAT
    # partition that anybody can read on any PC.  `ls' answers what is actually
    # being asked -- is the network saved at all, and did the save survive the
    # reboot -- and `cat' would answer it by publishing the key.
    run ls -l /etc/NetworkManager/system-connections
    showall "NetworkManager's own configuration" \
        /etc/NetworkManager/NetworkManager.conf /etc/NetworkManager/conf.d/*.conf
    # Started by NetworkManager over D-Bus here, not by a unit of its own, so an
    # inactive unit on this line is the normal state and not a finding.
    run systemctl --no-pager status wpa_supplicant

    showall "the CONSYS/WMT nodes, if the driver made any" \
        /sys/class/misc/wmtdetect/uevent /dev/wmtWifi /dev/stpwmt
    kgrep "Wi-Fi and the connectivity MCU" \
        'wlan|wifi|wmt|consys|stp_|btif|mt66|mt76|cfg80211|ieee80211|nl80211'

    printf '\n\n########## 2.  BATTERY AND CHARGER ##########\n'
    run ls -l /sys/class/power_supply
    showall "every power supply this kernel registered" \
        /sys/class/power_supply/*/uevent

    # THE ONE BIT THAT SAYS WHY usb/online IS ZERO, and without it the two cases
    # are indistinguishable from userspace -- which is how "the charger is plugged
    # in and the dashboard says no cable" gets reported as a driver that cannot
    # see a cable.  THERE ARE TWO CONNECTORS ON A J36 ULTRA: a DC inlet, which
    # charges and has no data lines, and an OTG port, which carries the data and
    # sources 5 V the whole time the board is on.  CHRDET is the inlet; this pad
    # is the port; they do not meet.  So 1 here is the ordinary state of a running
    # console and says nothing at all about whether a charger is attached -- read
    # online for that, and current_now to find out whether it is doing anything.
    # -1 means no j36,drvvbus-pad in the device tree.
    #
    # The driver used to hold the charger off whenever this read 1, on the belief
    # that the two were one socket.  They are not, this reads 1 always, and the
    # console spent a release discharging on the mains.  chrin_shared restores
    # that behaviour for a board where the belief is true; on this one it is 0 and
    # is dumped below so a log says which of the two stories it is from.
    showall "is this board feeding the OTG port itself?" \
        /sys/class/power_supply/*/vbus_sourcing
    showall "and is the single-connector interlock on? (0/N is right here)" \
        /sys/module/j36_mt6592_pmic/parameters/chrin_shared \
        /sys/module/j36_mt6592_usb_phy/parameters/vbus

    kgrep "the PMIC, the gauge and the ADC" \
        'pmic|mt6323|battery|charger|auxadc|adc|power_supply|vchr|vbat|drvvbus'

    printf '\n\n########## 3.  GAMEPAD AND JOYSTICKS ##########\n'
    show /proc/bus/input/devices
    run ls -l /dev/input
    showall "what each input device calls itself" /sys/class/input/input*/name
    showall "and what it says it can do (EV bits)" /sys/class/input/input*/capabilities/ev
    showall "absolute axes, which is where a dead stick shows" \
        /sys/class/input/input*/capabilities/abs
    kgrep "input devices" 'input|joystick|gamepad|js[0-9]|event[0-9]|j36_input|adc.*key'

    printf '\n\n########## 4.  USB ##########\n'
    run lsusb
    run ls -l /sys/bus/usb/devices
    showall "what is plugged in, as the devices describe themselves" \
        /sys/bus/usb/devices/*/product /sys/bus/usb/devices/*/manufacturer
    # The IDs as well as the names, because a driver matches on these and not on
    # what the device calls itself -- and one of them decides section 5's dock
    # question outright: 17e9 is DisplayLink, and it is the only vendor mainline
    # udl will bind to.  No 17e9 anywhere here means no HDMI mirror is possible on
    # this board no matter what the hub's box says.
    showall "and the IDs their drivers actually match on" \
        /sys/bus/usb/devices/*/idVendor /sys/bus/usb/devices/*/idProduct
    # ── CAN ANYTHING IN THIS BUILD DRIVE WHAT IS PLUGGED IN? ──────────────────
    #
    # The IDs above are the evidence; this is the verdict, one line per device,
    # because "the adapter does nothing" and "the adapter is fine and its HDMI
    # needs a thing this SoC does not physically have" are the same silence
    # otherwise, and the second one is not fixable by any amount of driver work.
    #
    # A USB device can put a picture on a monitor only by BEING a display device:
    # receiving pixels over the bus and driving the panel itself.  The other kind
    # of USB-C-to-HDMI -- the cheap kind, and the majority of them -- is
    # DisplayPort Alt Mode, where the connector's high-speed pairs are switched
    # away from USB and onto a DisplayPort transmitter inside the host.  That
    # needs a USB-C receptacle, CC pins, a USB-PD controller to negotiate the
    # mode, and a DisplayPort transmitter to switch onto.  MT6592 has a micro-USB
    # 2.0 MUSB core and none of the other four.  Nothing is disabled; there is
    # nothing there to enable.
    sec "what is on the bus, and whether anything in this build can drive it"
    _anydev=0
    for _u in /sys/bus/usb/devices/*; do
        [ -r "$_u/idVendor" ] || continue        # skip interfaces; devices only
        _vid="$(cat "$_u/idVendor" 2>/dev/null)"
        _pid="$(cat "$_u/idProduct" 2>/dev/null)"
        _cls="$(cat "$_u/bDeviceClass" 2>/dev/null)"
        _nam="$(cat "$_u/product" 2>/dev/null)"
        # 1d6b is the Linux Foundation root hub -- the kernel's own virtual hub at
        # the top of every bus, present on a board with an empty port.  It is not
        # a thing anybody plugged in, and reading it as one is a standing way to
        # conclude that a port enumerated something when it did not.
        case "$_vid" in 1d6b) continue ;; esac
        _anydev=1
        _drvs=""
        for _i in "$_u"/*:*; do
            [ -d "$_i" ] || continue
            _d="$(readlink "$_i/driver" 2>/dev/null)"
            _d="${_d##*/}"
            [ -n "$_d" ] || _d="(unclaimed)"
            _drvs="$_drvs $_d"
        done
        [ -n "$_drvs" ] || _drvs=" (no interfaces)"
        printf -- '--- %s  %s:%s  class %s  %s\n' \
            "${_u##*/}" "$_vid" "$_pid" "$_cls" "${_nam:-(no product string)}"
        printf '    drivers:%s\n' "$_drvs"
        case " $_drvs " in
        *" udl "*)
            printf '    VERDICT: DisplayLink, bound to udl.  It has a card node and j36-mixmirror\n'
            printf '             will find it.  This is the one adapter class that works here.\n'
            continue ;;
        esac
        case "$_vid" in
        17e9)
            printf '    VERDICT: DisplayLink silicon that mainline udl did NOT bind.  udl speaks the\n'
            printf '             USB 2.0 DL-1x0/DL-1x5 protocol only.  DL-3xxx/5xxx/6xxx need\n'
            printf '             DisplayLink proprietary plus evdi, which this image does not ship\n'
            printf '             and which has no armhf build.\n'
            continue ;;
        esac
        case "$_cls" in
        09)
            printf '    VERDICT: a hub.  A hub carries no video of its own.  If this one has an HDMI\n'
            printf '             socket then either a DisplayLink chip sits behind it -- which would\n'
            printf '             enumerate above as its own 17e9 device -- or that socket is\n'
            printf '             DisplayPort Alt Mode, which needs a USB-C host with a DisplayPort\n'
            printf '             transmitter.  No 17e9 line above means it is the second one, and no\n'
            printf '             driver can change that.\n'
            continue ;;
        esac
        printf '    VERDICT: nothing in this build claims it.  If this is the video half of the\n'
        printf '             adapter, look %s:%s up before buying another: Fresco Logic FL2000 and\n' "$_vid" "$_pid"
        printf '             the MacroSilicon parts have no mainline driver at all, on any\n'
        printf '             architecture, so there is no kernel option that turns them on.\n'
    done
    # ── AN EMPTY BUS HAS TWO CAUSES AND THEY LOOK IDENTICAL HERE ──────────────
    #
    # "Nothing but the root hub" was printed for a whole run of logs and read as
    # "nothing is plugged in", and it was not: the port had never become a host.
    # MUSB fixes which end of the cable it is when a session STARTS, the
    # bootloader starts one as a peripheral so the board can be flashed over this
    # socket, and until j36_mt6592_usb_phy learned to end that session first the
    # core stayed the peripheral the LK made it -- DEVCTL reading ADEV with HM
    # clear, a root hub with one port it never scans, and nothing able to
    # enumerate on it at all.  A hub, a stick and a display adapter all produce
    # exactly this silence, so the verdicts above mean nothing until the line
    # below says HOST.
    if [ "$_anydev" = 1 ]; then
        :
    else
        printf '(nothing but the root hub -- the port enumerated no device at all)\n'
        _hm=""
        _hm="$(dmesg 2>/dev/null | grep -a 'DEVCTL' | grep -a 'MUSB ' | tail -n 1)"
        case "$_hm" in
        *'HOST '*)
            printf '    The core IS the host, so the port is up and simply empty: nothing is\n'
            printf '    plugged in, or what is plugged in never pulled D+/D- up.\n' ;;
        *'PERIPHERAL '*)
            printf '    But the core is NOT the host -- the MUSB line below reads PERIPHERAL, so\n'
            printf '    the root hub never scans its one port and NOTHING can enumerate here:\n'
            printf '    not a hub, not a stick, not a mouse, not a display adapter.  Fix that\n'
            printf '    first; every verdict above is about a bus that was never scanned.\n' ;;
        *)
            printf '    No MUSB readout in dmesg to say whether the core became the host.\n'
            printf '    j36_mt6592_usb_phy prints one at power-on, at +3 s and at +9 s.\n' ;;
        esac
        [ -z "$_hm" ] || printf -- '    %s\n' "$_hm"
    fi
    # The next question after "is it the host" is "does the connect interrupt get
    # delivered", and that is a count that either moves when something is plugged
    # in or does not.  A musb line with 0 in it on a port that IS the host is the
    # device tree naming the wrong SPI.
    sec "the MUSB interrupt, and whether it has ever fired"
    grep -i 'musb\|usb' /proc/interrupts || printf '(no USB interrupt is registered at all)\n'
    kgrep "USB, the PHY and MUSB" 'usb|musb|phy|xhci|ehci|hub |otg|vbus'

    printf '\n\n########## 5.  THE PANEL, THE SPLASH AND THE DASHBOARD ##########\n'
    run ls -l /dev/fb0 /dev/dri
    showall "framebuffer geometry" \
        /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel \
        /sys/class/graphics/fb0/name /sys/class/graphics/fb0/state
    # THE SAME MEMORY, SEEN FROM THE OTHER END.  fb0 above is simple-framebuffer
    # over the LK's carveout; this is that carveout as a dma-buf, and the address
    # and geometry here have to match the two lines above them.  A /dev/j36fb that
    # is missing means j36_fbmem did not load, and a GL client can then only reach
    # the panel by copying into /dev/fb0 -- which is why `eglprobe -z' would say
    # nothing works when what is actually missing is this module.
    run ls -l /dev/j36fb
    showall "the LK framebuffer as a dma-buf" /sys/class/misc/j36fb/info
    show /sys/class/tty/tty0/active
    run ps -eo pid,ppid,stat,etimes,args
    run systemctl --no-pager status mixdash
    # mixdash moves its own stdout and stderr into this file once it has painted a
    # frame, so that a Qt warning is not drawn across the dashboard -- which means
    # everything the dashboard has said since boot is HERE and not in the journal,
    # and section 8 below would not show it.  Tailed rather than cat'd: the file is
    # wrapped at 256 KB and the last screen of it is the part with the answer in.
    sec "/run/j36/mixdash.log -- the dashboard's own output, last 200 lines"
    if [ -r /run/j36/mixdash.log ]; then
        tail -n 200 /run/j36/mixdash.log 2>&1 || printf '(unreadable)\n'
    else
        printf '(absent -- no first frame yet, so it is still on the console)\n'
    fi
    # ── THE USB-HDMI DOCK ─────────────────────────────────────────────────────
    #
    # The other thing on this board that can put a picture somewhere, and the one
    # whose failure leaves no trace anywhere else.  j36-mixmirror runs from boot
    # and is SILENT BY DESIGN while there is nothing docked -- so its absence from
    # the journal proves nothing, and "the service never started" and "it started
    # and nothing it can draw to was ever plugged in" read identically without
    # these lines.  That is exactly how the mirror got reported as never running.
    #
    # mirror.status is the mirror's own answer: one keyword, then one sentence,
    # rewritten on every state change.  Absent means the service is not running,
    # which systemctl above will then say why of.
    run systemctl --no-pager status j36-mixmirror
    show /run/j36/mirror.status
    # And the same question asked of the kernel rather than of the mirror: which
    # driver is behind each card node.  A DisplayLink adapter appears as a card
    # bound to udl and NOTHING ELSE DOES, so mediatek-drm alone here means nothing
    # on the port was DisplayLink -- a USB-C hub whose HDMI is DisplayPort Alt Mode
    # cannot appear at all, because MT6592 has no DisplayPort to alt-mode onto.
    # The dash test drops card0-DSI-1 and friends: those are connectors, not cards.
    sec "which driver owns each /sys/class/drm card"
    _anycard=0
    for _c in /sys/class/drm/card[0-9]*; do
        [ -d "$_c" ] || continue
        _b="${_c##*/}"
        case "$_b" in *-*) continue ;; esac
        _anycard=1
        # Plain readlink, not -f: the link's own text ends in the driver's name,
        # and readlink of something that is not a symlink prints nothing and
        # exits non-zero, which is the distinction wanted here.  `-f' would
        # canonicalise a path whose last component does not exist and print a
        # driver name for a card that has no driver bound.
        _drv="$(readlink "$_c/device/driver" 2>/dev/null)"
        _drv="${_drv##*/}"
        [ -n "$_drv" ] || _drv="(nothing bound to it)"
        printf -- '--- %s: %s\n' "$_b" "$_drv"
    done
    [ "$_anycard" = 1 ] || printf '(no card nodes -- no DRM driver registered one)\n'
    run systemctl --no-pager status batt_led
    kgrep "the display stack" 'fb0|fbcon|drm|mtkfb|mtkdrm|lima|mali|backlight|fbmem|j36fb'
    kgrep "the USB-HDMI mirror" 'udl|displaylink|17e9|mixmirror'

    printf '\n\n########## 6.  MODULES ##########\n'
    show /proc/modules

    printf '\n\n########## 7.  THE WHOLE KERNEL RING BUFFER ##########\n'
    sec "dmesg"
    dmesg 2>&1 || printf '(dmesg failed)\n'

    printf '\n\n########## 8.  THE JOURNAL FOR THIS BOOT ##########\n'
    sec "journalctl -b, last 4000 lines"
    journalctl -b --no-pager -n 4000 2>&1 || printf '(journalctl failed)\n'

    printf '\n\n===== end of diagnostic =====\n'
}

write_once() {
    if ! mount_boot; then
        echo "j36-logdump: no FAT partition on this card carries mvii/ or j36/"
        return 1
    fi
    dump "$1" > "$MNT/$TMP" 2>&1
    _bytes="$(wc -c < "$MNT/$TMP" 2>/dev/null || echo 0)"
    sync
    mv -f "$MNT/$TMP" "$MNT/$NAME" 2>/dev/null || true
    sync
    umount "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    echo "j36-logdump: wrote /$NAME on $BOOTDEV, $_bytes bytes ($1)"
    return 0
}

case "${1:-boot}" in
boot)
    # Two passes, and the first one is the point.  The failures worth reading are
    # the ones that make somebody give up and pull the power, so there has to be a
    # file on the card before that happens; twenty seconds in is after systemd has
    # finished starting things and before anybody has lost patience.  The second is
    # late enough that a restart loop or a ninety-second unit timeout has had room
    # to show its shape, and it overwrites the first, so the card ends up with
    # whichever picture is the more complete one the board survived to write.
    sleep 20
    write_once "boot+20s"
    sleep 55
    write_once "boot+75s"
    ;;
now|shutdown)
    write_once "shutdown"
    ;;
*)
    echo "usage: j36-logdump boot|now"
    exit 64
    ;;
esac
exit 0
LOGDUMP
    chmod 0755 /newroot/run/j36/bin/j36-logdump

    # ── the unit ─────────────────────────────────────────────────────────────
    #
    # Type=simple and not oneshot, and RemainAfterExit is what makes ExecStop mean
    # what it says.  A oneshot that sleeps for 75 s holds the multi-user.target job
    # open for 75 s, which delays nothing real but makes systemd-analyze and every
    # "startup finished" message wrong.  Simple starts, returns, and gets out of the
    # way -- and with RemainAfterExit the unit stays active after the script exits,
    # so ExecStop runs at shutdown rather than immediately after the second pass.
    #
    # DefaultDependencies are left on, which is what puts Before=shutdown.target on
    # this unit and gives the shutdown pass a block device that still exists.
    #
    # StandardOutput=journal and not journal+console: this is the unit whose whole
    # purpose is that nobody has to read the panel.
    cat > /newroot/run/systemd/system/j36-logdump.service <<'UNITLOG'
# Written by the J36 Ultra initramfs, into a tmpfs.  See setup_logdump in /init.
[Unit]
Description=Write a MixOS boot diagnostic onto the BOOT partition
# Late, but not ordered after the target that wants it -- that is a knot.  This is
# the last thing systemd starts on the way to a login, so it is the right anchor.
After=systemd-user-sessions.service

[Service]
Type=simple
RemainAfterExit=yes
ExecStart=/bin/sh /run/j36/bin/j36-logdump boot
ExecStop=/bin/sh /run/j36/bin/j36-logdump now
# The shutdown pass mounts a FAT, writes about a megabyte and unmounts it.  Thirty
# seconds is generous for that on a slow card and still short enough that a board
# which cannot do it does not hang the shutdown.
TimeoutStopSec=30
StandardOutput=journal
StandardError=journal
UNITLOG
    mkdir -p /newroot/run/systemd/system/multi-user.target.wants
    ln -sf ../j36-logdump.service \
           /newroot/run/systemd/system/multi-user.target.wants/j36-logdump.service

    say "logdump: BOOT:/mixos-log.txt, written 20 s and 75 s in and again at shutdown"
    say "         Take the card out and open it on any machine -- BOOT is the FAT one."
    return 0
}

# ── the restart loop that paints over the picture ─────────────────────────────
#
# batt_led.service comes from the shared RG351MP rootfs and runs
# /usr/local/bin/batt_life_warning.py, which on THAT board reads
# /sys/class/power_supply/battery/capacity and writes /sys/class/gpio/gpio77/value.
# Neither path exists here.  The unit is Restart=always with RestartSec=2 and
# StartLimitIntervalSec=0 -- rate limiting explicitly turned off -- so a script
# that exits nonzero is started again every two seconds for as long as the board
# is on, and every attempt puts an eight-line Python traceback and two systemd
# status lines on /dev/console.  On this board /dev/console is the panel.  That is
# the splash being overwritten, once per 2.3 seconds, for ever.
#
# THE SCRIPT ITSELF WAS ALREADY FIXED -- it walks /sys/class/power_supply by type
# now and treats a missing LED as normal -- but the fix is in the SHARED ROOTFS,
# which the R36 half of this build makes and which a J36-only run does not rebuild.
# So a card can carry a current kernel, a current dashboard and last season's
# batt_life_warning.py, and this is exactly what that card does.  Written from the
# initramfs, this reaches such a card without rebuilding anything on it.
#
# A DROP-IN AND NOT A MASK.  Masking takes the battery LED away from every board
# that has one; this only says the unit may fail a few times and then has to stop
# trying, and that what it prints goes to the journal.  A drop-in DIRECTORY merges
# across /run, /etc and /lib -- unlike a unit FILE, which does not -- so this
# reaches a unit installed in /etc/systemd/system without replacing it, and a card
# that already carries the fixed script is unaffected: that one never exits, so a
# start limit it never reaches costs it nothing.
tame_batt_led() {
    if [ -z "$rootdev" ]; then return 1; fi
    if ! ensure_run_tmpfs; then return 1; fi
    mkdir -p /newroot/run/systemd/system/batt_led.service.d
    cat > /newroot/run/systemd/system/batt_led.service.d/zz-j36-bounded.conf <<'BATTCONF'
# Written by the J36 Ultra initramfs, into a tmpfs.  See tame_batt_led in /init.
[Unit]
# Five starts in thirty seconds is a script that cannot run on this board.  systemd
# then puts the unit in failed state and stops, which is the correct end of that
# story and is what the unit's own StartLimitIntervalSec=0 asked it never to do.
StartLimitIntervalSec=30
StartLimitBurst=5

[Service]
# Whatever it has to say, it says to the journal.  The panel is showing a picture.
StandardOutput=journal
StandardError=journal
BATTCONF
    if [ -f /newroot/etc/systemd/system/batt_led.service ] || \
       [ -f /newroot/lib/systemd/system/batt_led.service ] || \
       [ -f /newroot/usr/lib/systemd/system/batt_led.service ]; then
        say "power: batt_led.service bounded to 5 starts and kept off the console"
    else
        say "power: no batt_led.service on this rootfs; the drop-in is inert"
    fi
    return 0
}

# ── WHAT USED TO BE HERE, AND WHY NOTHING REPLACED IT ─────────────────────────
#
# neuter_es(), which masked emulationstation.service with a symlink to /dev/null in
# /run/systemd/system.control -- the one runtime directory that outranks the /etc its
# unit was installed into -- and dropped a zz-j36-dash.conf beside it that reset
# ExecStart to an echo, in case the mask was ever ignored.  Two mechanisms for one
# job, because the cost of getting it wrong was six copies of an abort scrolling a
# 640x480 panel with no keyboard attached.
#
# It is gone because the thing it defended against is gone: no build_emulationstation.sh,
# no /usr/bin/emulationstation on the rootfs, and therefore no unit to mask.  Masking
# a unit that does not exist is not harmless housekeeping -- it is a line that makes a
# reader believe ES is still somewhere on this card.
#
# If EmulationStation ever comes back as a Debian package on this rootfs, this is
# where its mask goes, and the /run/systemd/system.control path is the part worth
# keeping from the old function: /etc/systemd/system outranks /run/systemd/system,
# so the obvious mask is silently ignored and the build believes it worked.

# ── the failure, said where it can be read ────────────────────────────────────
#
# WHY THIS UNIT EXISTS.  find_mixos already prints why it found nothing, but it prints
# it from the initramfs -- and then the kernel and systemd put a hundred lines of
# their own on a 640x480 panel, the last of them typically hostnamed deactivating
# thirty seconds in.  What the operator sees is a console that stopped, which is
# indistinguishable from a dashboard that crashed, from a unit that was never enabled,
# and from a panel that went black.  So the reason is repeated by a unit that runs
# after all of that, several times, and it names what the initramfs actually saw.
#
# There is no keyboard on this board and no getty worth logging into, so a message on
# the panel is the whole of the diagnostic interface.  Type=simple with the loop
# inside a shell rather than Restart=always: a restart loop would be journalled as a
# failing unit, and this is not a failure of this unit.
dash_notice() {
    mkdir -p /newroot/run/systemd/system
    cat > /newroot/run/systemd/system/mixdash-missing.service <<UNITNOTICE
# Written by the J36 Ultra initramfs, into a tmpfs.  Exists only when the dashboard
# payload was not found; a boot that finds it never writes this file.
[Unit]
Description=MixOS dashboard payload not found (report only)
# Late, but not last-by-target: ordering after the target that wants it is a knot.
After=systemd-user-sessions.service

[Service]
Type=simple
StandardOutput=journal+console
StandardError=journal+console
# The loop counts with six words and not with a shell variable on purpose: systemd
# expands \$FOO in a command line even inside quotes, and whether \$\$ escapes it
# depends on the version.  No dollar sign is the same loop with nothing to get wrong.
ExecStart=/bin/sh -c 'for i in 1 2 3 4 5 6; do \\
  echo ""; \\
  echo "j36: MixOS dashboard did not start -- its payload is not on this card."; \\
  echo "j36: the initramfs looked in the rootfs /opt/mixos and on every other"; \\
  echo "j36: partition it could mount read-only, and saw:"; \\
  echo "j36:   $dash_seen"; \\
  echo "j36: fix it on a PC with the card in a reader, on the ROOTFS partition:"; \\
  echo "j36:   sudo tar -C /mnt/ROOTFS -xzf sd-root.tar.gz"; \\
  echo "j36: which gives /opt/mixos, the first place /init looks.  Not BOOT:"; \\
  echo "j36: that one is FAT and holds none of the ~30 Qt SONAME symlinks --"; \\
  echo "j36: mixdash would then die before main()."; \\
  echo "j36: there is no other shell in this build and nothing is started as a"; \\
  echo "j36: fallback, so this console is what the board has until it is fixed."; \\
  sleep 20; \\
done'
UNITNOTICE
    mkdir -p /newroot/run/systemd/system/multi-user.target.wants
    ln -sf ../mixdash-missing.service \
           /newroot/run/systemd/system/multi-user.target.wants/mixdash-missing.service
    say "dash: mixdash-missing.service will repeat that on the console after boot"
    return 0
}

# Before any of them, because it is what puts /opt/mixos/j36 on the OS partition in
# the first place on a card updated from a Mac: find_payload below looks there first,
# and by the time it does the tarball has already been unpacked.
stage "Checking for an update"
progress 28
stage_from_boot

if [ "$want_lima" = 1 ] || [ "$want_mtkdrm" = 1 ] || [ "$want_gl" = 1 ] || \
   [ "$want_audio" = 1 ] || [ "$want_usb" = 1 ] || [ "$want_power" = 1 ] || \
   [ "$want_wifi" = 1 ]; then
    # find_payload is called by each of the four rather than once here, so that a card
    # with no payload at all gets one message per word that was asked for, naming the
    # word -- and so that this block does not have to know which of them needs what.
    #
    # lima first: it is the one payload with a hardware gate in front of it, and
    # nothing else should be loaded if the MFG domain does not come up.  mtkdrm
    # after it, so that if it does disturb the panel the thing that was already
    # proved has already run.  Audio next: it touches nothing the others touch,
    # and it has to be in place before systemd looks for a controlC0 to restore
    # into.  The GL front end last, because it is the only one of the four that
    # is not finished when this script ends -- it is a message to systemd, and
    # systemd has not started yet.
    # USB after mtkdrm and before the GL front end, and the position is not
    # arbitrary: udl and mtk_drm both pull in drm_kms_helper, so both payload
    # directories carry a copy of it.  Letting mtkdrm go first means it is loaded
    # once from the directory that has always owned it and run_usb skips its own.
    # The other way round works too -- run_mtkdrm has no such skip -- but it would
    # print a module-load failure on a boot that is working correctly.
    #
    # The progress numbers are not evenly spaced and should not be: they are
    # roughly where each of these lands in the wall-clock of a working boot, so
    # the bar tracks time rather than tasks.  run_lima carries the MFG power
    # sequence and takes the longest of the five.
    if [ "$want_lima" = 1 ]; then
        stage "Starting the graphics core"; detail "MFG power domain, lima"
        progress 40; run_lima
    fi
    if [ "$want_mtkdrm" = 1 ]; then
        stage "Starting the display controller"; detail "mediatek-drm, MIPI-DSI, panel"
        progress 55; run_mtkdrm
    fi
    if [ "$want_audio" = 1 ]; then
        stage "Starting audio"; detail "MT6592 AFE, MT6323 codec"
        progress 62; run_audio
        # Straight after the card, and not down with setup_dash.  run_audio has
        # just told us whether /dev/snd exists, which is the only thing worth
        # gating this on, and `default' is a property of the machine rather than
        # of the dashboard: a card with j36.dash=0 still has aplay, mpv, SDL and
        # every emulator on it asking alsa-lib for a device by that name.
        setup_asound
    fi
    if [ "$want_usb" = 1 ]; then
        stage "Starting USB"; detail "MUSB host, hub, HID"
        progress 68; run_usb
    fi
    # After USB, and one module, so it costs almost nothing in the bar.  The
    # order against run_usb is the one described on run_power: the PMIC samples
    # the DRVVBUS pad every poll, and letting the PHY set that pad first means the
    # very first poll sees the truth instead of the LK's leftovers.  It buys a
    # tidy first second of log rather than a working charger now -- the pad no
    # longer decides charging -- but there is no reason to give it up.
    if [ "$want_power" = 1 ]; then
        stage "Starting power management"; detail "MT6323 gauge, charger, poweroff"
        progress 71; run_power
    fi
    # After the PMIC and not before it: the connectivity rails are on the MT6323
    # and this module reaches them through symbols the PMIC module exports, so
    # the other order does not fail late, it fails at insmod.  It is the widest
    # step in the bar because it is the only one that waits on a peer -- see the
    # bound on the wait in run_wifi.
    if [ "$want_wifi" = 1 ]; then
        stage "Starting Wi-Fi"; detail "CONSYS power, BTIF link, firmware, wlan0"
        progress 73; run_wifi
    fi
    if [ "$want_gl" = 1 ]; then
        stage "Staging OpenGL"; detail "Mesa, EGL, GLESv2"
        progress 74; setup_gl
    fi
fi

# Outside that block, where it used to be inside it.  Everything above has been copied
# into the rootfs's /run or insmod'ed, so nothing still reads this mount -- and leaving
# a vfat mount for switch_root to carry across is a mount systemd would inherit and
# nobody would own.  stage_from_boot mounts /bootfs whether or not a single payload
# word was asked for, so a cleanup that only ran when one was is a cleanup that leaks.
# The payload in the rootfs needs no counterpart: that is the partition being switched
# into.
if [ "$bootfs_mounted" = 1 ]; then
    umount /bootfs 2>/dev/null
    bootfs_mounted=0
    # Cleared with the mount, so that nothing added after this line can find a path
    # under /bootfs that is no longer there.
    case "$payload" in /bootfs/*) payload="" ;; esac
fi

# Outside that block on purpose: the dashboard is on the OS partition and the BOOT
# partition has nothing to do with it, so a card whose j36/ directory was deleted --
# no Mesa, no modules, no probe -- still comes up in the dashboard.  Last of all, so
# that its unit is written after everything it reads has run.
#
# The else branch is not noise.  Without j36.dash=1 nothing stages a shell at all,
# and this rootfs enables none of its own: the panel would go to a login prompt on a
# board with no keyboard and nothing would say why.  That is the failure this line
# names in one word instead of an evening.
if [ "$want_dash" = 1 ]; then
    stage "Preparing the dashboard"
    progress 82
    setup_dash
    # Not inside setup_dash, and not conditional on it having worked.  setup_dash
    # returns early when there is no /opt/mixos on the card, and a machine with no
    # dashboard is exactly the machine somebody is about to plug a USB disk into to
    # fix it.  The rules only need a rootfs with a /run, which is checked inside.
    #
    # It is under want_dash for one reason: j36.dash=0 means "do not touch what this
    # rootfs starts by itself", and a udev rule dropped into /run is touching it.
    if [ "$want_usb" = 1 ]; then
        stage "Preparing removable disks"
        detail "udev rule, mount unit, /media"
        progress 85
        setup_automount
    else
        say "automount: j36.usb is not set, so there is no USB stack and no disks"
        say "           to mount.  The rules are not written."
    fi
else
    say "dash: j36.dash is not in the kernel command line, so no shell is staged"
    say "      and whatever the rootfs starts by itself is what you get.  Add"
    say "      j36.dash=1 to bootargs in mvii/boot.conf on the BOOT partition."
fi

# Outside every want_ block above, and last, which is the only placement that makes
# sense for either of these.  The boots worth explaining are exactly the ones where
# one of those words did not do its job, so a diagnostic conditional on them is a
# diagnostic missing from the boot that needed it -- and the console the battery
# daemon is scribbling on belongs to the splash whether or not a dashboard was
# asked for.  Both need a rootfs and nothing else, which is what is checked inside.
# Before the log and not after it, so that the very first line the journal writes
# under the new root already carries the right name.  Gated on a rootfs and on
# nothing else: this is not a feature any boot word turns on, it is who the machine
# is, and a boot with j36.dash=0 and no audio still answers to it on the network.
# The card's size used to be settled here too, by writing a unit that grew p3 before
# systemd mounted it.  There is no p3 any more and the partition that grows is the one
# this initramfs is standing on, so that work moved to expand_root, hundreds of lines
# above -- to the only point in the boot where the OS partition is unmounted.
if [ -n "$rootdev" ]; then
    setup_hostname
fi

if [ -n "$rootdev" ] && [ "$want_log" = 1 ]; then
    stage "Preparing the log"
    detail "BOOT:/mixos-log.txt"
    progress 86
    tame_batt_led
    setup_logdump
fi

if [ -n "$rootdev" ]; then
    say "switching root into $rootdev"
    stage "Starting MixOS"
    detail "$rootdev"
    progress 88
    # The splash keeps running across the hand-over -- its pages are mapped and
    # /dev goes with us -- so this is not goodbye, it is "stop expecting me to
    # keep talking, and leave the console in graphics mode when you do stop".
    # Without it the idle timeout would eventually hand the panel back to fbcon
    # in the middle of systemd's boot.
    if [ "$splash_on" = 1 ]; then echo "handover" >> "$splash_chan"; fi
    # Carried across rather than left behind: the real init inherits them
    # already mounted, and if the move is refused it mounts its own.
    mount --move /dev /newroot/dev 2>/dev/null
    mount --move /proc /newroot/proc 2>/dev/null
    mount --move /sys /newroot/sys 2>/dev/null
    sync
    exec switch_root /newroot /sbin/init
    # Only reached if switch_root itself failed, and by then /dev, /proc and /sys
    # have moved out from under us -- put them back before saying so, or say()
    # has no /dev/tty1 to write to and the failure is invisible.
    mount -t devtmpfs devtmpfs /dev 2>/dev/null
    mount -t proc proc /proc 2>/dev/null
    mount -t sysfs sysfs /sys 2>/dev/null
    say "switch_root failed; staying in the initramfs"
fi

# Everything from here down is a post-mortem, and a post-mortem behind a picture
# is a post-mortem nobody reads.  devtmpfs is a single instance, so the remount
# above brought the channel back with it and this still reaches the splash.
splash_off

say ""
# Reached two ways: no root was found, or one was and switch_root refused it.
# Only the first is a missing-card story, and only it should be told as one.
if [ -z "$rootdev" ]; then
    if [ -n "$root_hint" ]; then
        say "root=$root_hint held no filesystem with /sbin/init"
    fi
    say "No rootfs after ${waited}s; staying in the initramfs."
    say ""
fi

# ── Why the card did not come up ──────────────────────────────────────────────
#
# /proc/interrupts is the measurement that tells the two failure modes apart, so
# print it rather than leaving it to be typed -- the panel is the only output
# here and the J36 input driver emits gamepad events, not keystrokes.
#
# The mmc line's count is the answer:
#
#   absent      mtk-sd never bound.  Look for its probe error in dmesg, not here.
#   present, 0  The controller is being driven and nothing is coming back, which
#               means the interrupt is misrouted: every request then dies on
#               mtk-sd's 5 s software watchdog with host->error=0x2 REQ_CMD_TMO,
#               and a card that merely fails to answer would instead raise
#               MSDC_INT_CMDTMO in milliseconds.  That was the SPI-40 bug; the
#               DT now says 72, from INTID 104 - 32.
#   present, >0 The interrupt works.  The card, the pad tuning or the clock is
#               the problem -- read the mtk-sd errors in dmesg.
#
# mt6592-gpt is the control: it is the clockevent, so its count is always large,
# and if the mmc line reads 0 while that one climbs, interrupt delivery as such
# is fine and only this SPI number is in question.
say "Interrupts (compare the mmc count against mt6592-gpt):"
show /proc/interrupts
say "MMC hosts:"
ls /sys/class/mmc_host 2>/dev/null || say "  none"
say ""
say "Block devices seen by this kernel:"
show /proc/partitions
say "Framebuffer devices:"
ls -l /dev/fb* 2>/dev/null || say "  none"
say "Input devices:"
ls -l /dev/input 2>/dev/null || say "  none"
say "Use: hexdump -C /dev/input/event0"
say ""

# A shell on each console, because either cable may be the only one attached:
# the serial one in the background, the panel one as pid 1's replacement.
if [ -c /dev/ttyS0 ]; then
    setsid cttyhack sh </dev/ttyS0 >/dev/ttyS0 2>&1 &
fi
if [ -c /dev/tty1 ]; then
    exec setsid cttyhack sh </dev/tty1 >/dev/tty1 2>&1
fi
exec setsid cttyhack sh
INIT
chmod 0755 "$INITROOT/init"

# ── PARSE /init WITH A POSIX SHELL BEFORE IT GOES ON A CARD ───────────────────
#
# /init is 2600 lines of shell that nothing on this workstation ever runs: the
# first thing to read it is BusyBox ash on a board with no keyboard, and a syntax
# error there is a boot that stops with whatever the splash last said still on the
# panel.  There is no console to see the message on and no shell to fix it from --
# the card comes out and goes back in a reader.  So it gets parsed here.
#
# NOT WITH bash.  `bash -n' accepts a backtick inside an unquoted here-document
# body and ash does not, which is exactly the mistake this is here to catch: two
# comment lines in the mixdash.service heredoc said `handover' and `-', the
# backticks paired into a command substitution, and every bash on every machine
# involved said the file was fine.  dash is ash's near relative and is /bin/sh on
# the Ubuntu build VM, so it is what gets asked.  busybox first when it is there,
# because that is the parser that actually runs.
#
# -n is parse-only: it reads and checks and executes nothing, so this cannot mount,
# unmount or reboot anything by being wrong about the machine it is on.
init_parser=""
for candidate in busybox dash; do
    if command -v "$candidate" >/dev/null 2>&1; then
        init_parser="$candidate"
        break
    fi
done
case "$init_parser" in
    busybox) busybox sh -n "$INITROOT/init" || die "/init does not parse under busybox ash; the board would stop at whatever the splash last said" ;;
    dash)    dash -n "$INITROOT/init"       || die "/init does not parse under dash, which is ash's relative; the board would stop at whatever the splash last said" ;;
    *)
        # Neither is installed, which on the build VM means something is wrong with
        # it -- dash is /bin/sh there.  Say so rather than silently checking nothing.
        log "WARNING: neither busybox nor dash is installed, so /init went out unparsed"
        ;;
esac
if [[ -n "$init_parser" ]]; then
    log "init: parses under $init_parser ($(wc -l < "$INITROOT/init") lines)"
fi

# What this boot image is, in a file, because the heredoc above is single-quoted:
# nothing from out here can be interpolated into /init, so anything /init needs to know
# has to arrive as a file inside the initramfs.  init= identifies the boot image itself;
# mixdash= is what this build of the dashboard should be, which /init passes on to
# mixdash so it can compare it with what it was actually compiled from.
cat > "$INITROOT/etc/j36-build" <<BUILDID
init=$(sha256sum "$INITROOT/init" | cut -c1-12)
mixdash=$MIXDASH_BUILD_ID
BUILDID

# XZ AND NOT GZIP, AND THE 9 MiB BOOTIMG SLOT IS THE WHOLE REASON.
#
# boot.img is a header, the zImage with its device tree appended, and this file,
# each padded to a 2048-byte page.  BOOTIMG cannot grow: it is a fixed partition
# in the stock scatter with RECOVERY starting exactly 9 MiB later.  The kernel is
# 7,857,760 bytes of that budget on its own, so the ramdisk is the only part with
# any give, and gzip -9 stopped having enough of it -- gzip packs this tree to
# 1,582,806 bytes, and the build that found this came out 10,240 bytes past the
# slot, which is not an image that can be written.
#
# The same tree at xz is 1,194,912 bytes and the image 9,058,304, so the slot has
# 370 KiB spare rather than being 10 KiB short.
#
# THIS EXACT INVOCATION IS THE KERNEL'S OWN.  cmd_xzmisc in scripts/Makefile.lib
# packs initramfs images with `--check=crc32 --lzma2=dict=1MiB' and nothing else,
# and both halves earn their place.  The kernel's XZ decoder is built without
# CRC64, so a stream carrying xz's default check is refused outright.  And the
# dictionary is allocated before there is a rootfs to page against, so it stays
# at 1 MiB instead of the 64 MiB `-9e' would ask for; a 3.4 MB tree cannot use
# more than a fraction of that anyway, and the whole difference is 41 KB against
# a decoder already known to handle this shape.
#
# WHAT MAKES THE IMAGE BOOT is CONFIG_RD_XZ, which this config sets.  Losing it
# would not fail the build -- it would produce an image that stops before it can
# print why -- so it is asserted here rather than assumed.
grep -q '^CONFIG_RD_XZ=y' "$CONFIG" || \
    die "the kernel config has lost CONFIG_RD_XZ, so an xz initramfs cannot decompress"

log "Packing the bring-up initramfs"
(
    cd "$INITROOT"
    find . -print0 | cpio --null -o --format=newc --owner=0:0 2>/dev/null |
        xz --check=crc32 --lzma2=dict=1MiB
) > "$ARTIFACTS/initramfs-j36-ultra.cpio.xz"

fits_in "$ARTIFACTS/initramfs-j36-ultra.cpio.xz" $((0x06000000)) "the initramfs"
log "initramfs: $(stat -c %s "$ARTIFACTS/initramfs-j36-ultra.cpio.xz") bytes xz"

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
    --ramdisk "$ARTIFACTS/initramfs-j36-ultra.cpio.xz" \
    --output "$ARTIFACTS/boot.img"

# Every comment in this file that says "the fixed 9 MiB BOOTIMG slot" is quoting
# the stock scatter: BOOTIMG is partition SYS9 at 0x1f40000 with
# partition_size 0x900000, and RECOVERY begins at 0x2840000, exactly 9 MiB later.
# Assert it, because the kernel config above just grew MMC and three filesystem
# drivers and nothing else in this build would notice the payload crossing into
# RECOVERY.
fits_in "$ARTIFACTS/boot.img" $((0x00900000)) "the BOOTIMG payload"

# ── fbdoom: the first moving picture on this panel ────────────────────────────
#
# Everything above proves the machine boots.  Nothing above proves the panel can
# be driven by a program, which is the next question, and the cheapest honest
# answer to it is Doom writing 32-bit pixels into /dev/fb0 and reading the pad
# from /dev/input/event0.
#
# NOTHING ON THE ROOTFS COULD DO THIS.  The shared armhf rootfs already carries
# gzdoom and lzdoom.  SDL2 has no fbdev backend -- KMSDRM, X11, Wayland, offscreen
# and dummy are the whole list -- so both need either DRM/KMS, which this kernel
# has no driver for yet, or a GL stack, and the GL stack on that card is the
# RK3326's Mali-G31 Bifrost blob for a SoC whose GPU is a Mali-450.  doomgeneric
# needs none of it: no SDL, no X11, no GL, no DRM.
#
# WHERE IT LIVES, AND WHY NOT IN THE INITRAMFS OR ON BOOT.  The initramfs goes
# into both payloads, and boot.img is capped at the 9 MiB BOOTIMG slot asserted
# just above.  A static ARM Doom is around 2 MiB and the IWAD is 24 MiB more, so
# putting either there would push the eMMC payload into RECOVERY.  It used to go
# on the FAT BOOT partition and /init used to run it before the hand-over; both
# have changed, because BOOT is a small vfat partition shared with an R36S's own
# boot files and 26 MiB of game is not boot payload.  Doom is userland software
# now: it ships in the second partition's /opt/mixos tree beside the dashboard,
# and the dashboard launches it.  That is a better fit than it sounds -- fbdoom
# writes 32-bit pixels into /dev/fb0 and reads evdev, which is exactly the layer
# mixdash draws in, so it is the one thing on this card guaranteed to be able to
# take the panel and hand it back.
#
# THE SOURCE LIST IS CHECKED, NOT WRITTEN.  doomgeneric's own Makefile is a
# hand-maintained object list for its X11 front end.  Deriving ours from the tree
# by exclusion -- every .c except the other front ends and the SDL/Allegro sound
# and GUS/icon/MIDI files -- and then diffing it against that list means an
# upstream file this recipe does not know about is a build failure here rather
# than a link error against a library this board has not got.  That check is only
# meaningful because DOOM_COMMIT is pinned; overriding it is what should trip it.
#
# NONE OF IT IS FATAL.  A game must never cost the user the kernel artifacts, so
# the build runs with errexit suspended and a failure only means the payload is
# not staged.  That matters most on the first build on a new machine, where this
# is the one step whose compiler has never been exercised here.
FBDOOM_SRC="$ROOT/device/j36-ultra/fbdoom/doomgeneric_j36.c"
DOOM_BIN=""
DOOM_WAD=""

build_fbdoom() {
    local dir stamp want expected actual
    local -a srcs

    [[ -f "$FBDOOM_SRC" ]] || { log "fbdoom: $FBDOOM_SRC is missing"; return 1; }

    if [[ ! -d "$DOOM_SRC/.git" ]]; then
        log "Cloning doomgeneric once for the framebuffer Doom payload"
        # Not --depth=1: the pinned commit has to be in the clone.
        git clone "$DOOM_URL" "$DOOM_SRC" || return 1
    fi
    git -C "$DOOM_SRC" checkout -q "$DOOM_COMMIT" 2>/dev/null || {
        git -C "$DOOM_SRC" fetch -q origin || return 1
        git -C "$DOOM_SRC" checkout -q "$DOOM_COMMIT" || return 1
    }

    dir="$DOOM_SRC/doomgeneric"
    [[ -d "$dir" ]] || { log "fbdoom: $dir is not in that checkout"; return 1; }
    cp "$FBDOOM_SRC" "$dir/" || return 1

    mapfile -t srcs < <(cd "$dir" && ls ./*.c | sed 's|^\./||' |
        grep -vE '^(doomgeneric_|i_sdl|i_allegro|gusconf\.c|icon\.c|mus2mid\.c)') || return 1
    (( ${#srcs[@]} > 50 )) || { log "fbdoom: only ${#srcs[@]} sources found"; return 1; }

    expected="$(sed -n 's/^SRC_DOOM = //p' "$dir/Makefile" | tr ' ' '\n' |
        sed 's|\.o$|.c|' | grep -v '^doomgeneric_xlib\.c$' | sort)"
    actual="$(printf '%s\n' "${srcs[@]}" | sort)"
    if [[ "$expected" != "$actual" ]]; then
        log "fbdoom: the derived source list no longer matches doomgeneric's own Makefile:"
        diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") || true
        return 1
    fi

    DOOM_BIN="$dir/doom-j36"
    stamp="$dir/.j36-built"
    want="$DOOM_COMMIT $(sha256sum "$FBDOOM_SRC" | awk '{print $1}')"
    if [[ -x "$DOOM_BIN" && "$(cat "$stamp" 2>/dev/null)" == "$want" ]]; then
        log "fbdoom: reusing $DOOM_BIN"
    else
        # 640x400 is a clean 2x of doom's 320x200 inside the 640x480 panel;
        # doomgeneric_j36.c explains why 480 would leave garbage on screen.
        # -std=gnu17 pins the dialect: this is 1997 C, and gcc 14 promoted
        # implicit declarations and int/pointer mismatches to hard errors.
        #
        # THE TWO -Wno- FLAGS, AND WHY SILENCING BEATS PATCHING HERE.  This
        # engine emits exactly five warnings and every one of them is upstream's,
        # in a tree pinned to DOOM_COMMIT that this recipe deliberately does not
        # fork -- the source list a few lines up is checked against upstream's
        # own Makefile precisely so that this stays a checkout and not a patch
        # queue.  Both flags are blanket over sixty-odd files, which is the cost.
        #
        # -Wno-format-truncation covers four of them: "map%i" and "map0%i" in
        # p_setup.c, "CWILV%2.2d" and "WIA%d%.2d%.2d" in wi_stuff.c.  The target
        # is char[9] because a WAD lump name is eight characters and a NUL.  gcc
        # sees plain ints and has to reason about INT_MIN..INT_MAX; the actual
        # values are a map number and two loop counters bounded by NUMEPISODES
        # and NUMMAPS, so the longest name any of them can produce is eight
        # characters.  There is nothing to fix even in the impossible case:
        # these are snprintf, which truncates and terminates rather than
        # overflowing, so the warning describes a wrong lump name -- a missing
        # graphic -- and not a memory error.  The only way to convince gcc is to
        # narrow the types in someone else's game engine.
        #
        # -Wno-unused-result covers the fifth, m_menu.c ignoring fread's result
        # while reading the 24-byte description out of each save file.  That one
        # is a real gap: a truncated save leaves the previous contents of a
        # static buffer showing as the slot name in the load menu.  It is still
        # a static buffer and still NUL-terminated, so it is a stale label and
        # not a crash, and it takes a corrupt save to see it at all.
        #
        # If DOOM_COMMIT is ever bumped, drop both flags for one build and read
        # what comes out before putting them back.
        log "fbdoom: compiling ${#srcs[@]} sources in one pass for ARMv7"
        ( cd "$dir" && arm-linux-gnueabihf-gcc \
            -O2 -std=gnu17 -fcommon -static \
            -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
            -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
            -Wno-error=implicit-function-declaration \
            -Wno-format-truncation -Wno-unused-result \
            -o doom-j36 "${srcs[@]}" doomgeneric_j36.c -lm ) || return 1
        printf '%s\n' "$want" >"$stamp"
    fi

    # The same two tests verify_arm_elf makes, spelled out rather than called:
    # that helper reports through die(), and a failure here must not take the
    # kernel artifacts down with it.  Static on purpose -- this runs from the
    # initramfs, before switch_root, so there is no ld.so and no /lib.
    local header
    header="$(readelf -hd "$DOOM_BIN" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "fbdoom: not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "fbdoom: not an ARM ELF"; return 1; }
    if grep -q 'NEEDED' <<<"$header"; then
        log "fbdoom: the binary wants shared libraries and the initramfs has none"
        return 1
    fi

    log "fbdoom: $(stat -c %s "$DOOM_BIN") bytes, static ARM"
    return 0
}

fetch_fbdoom_wad() {
    local name
    if [[ -n "${J36_DOOM_WAD:-}" ]]; then
        [[ -f "$J36_DOOM_WAD" ]] || { log "fbdoom: J36_DOOM_WAD=$J36_DOOM_WAD is not a file"; return 1; }
        name="$(basename "$J36_DOOM_WAD")"
        # d_iwad.c identifies an IWAD by filename before it opens it, so a WAD
        # under a name its iwads[] table does not carry is refused by the engine.
        case "$name" in
            doom.wad|doom1.wad|doom2.wad|plutonia.wad|tnt.wad|chex.wad|hacx.wad|\
            freedm.wad|freedoom1.wad|freedoom2.wad) ;;
            *) log "fbdoom: $name is not a name doomgeneric's iwads[] table knows; it will be refused" ;;
        esac
        DOOM_WAD="$J36_DOOM_WAD"
        return 0
    fi
    python3 "$ROOT/device/j36-ultra/fetch_freedoom.py" \
        --cache "$CACHE" --out "$CACHE/freedoom1.wad" || return 1
    DOOM_WAD="$CACHE/freedoom1.wad"
    return 0
}

# Off by default, and the default is about the download rather than about the game:
# J36_DOOM=1 clones doomgeneric and fetches a 26 MiB Freedoom IWAD, which is not
# something a kernel build should do unasked.  What it no longer costs is the BOOT
# partition -- with J36_DOOM=1 the binary and the IWAD go into the second partition's
# /opt/mixos tree, and the dashboard grows a Doom card that runs them.  With
# J36_DOOM=0 that card says so and everything else is unchanged.
if [[ "${J36_DOOM:-0}" == 1 ]]; then
    set +e
    build_fbdoom
    doom_rc=$?
    set -e
    if (( doom_rc != 0 )); then
        DOOM_BIN=""
        log "fbdoom: not staged, see the error above -- the kernel payload is unaffected"
    else
        set +e
        fetch_fbdoom_wad
        wad_rc=$?
        set -e
        if (( wad_rc != 0 )); then
            DOOM_WAD=""
            log "fbdoom: no IWAD, so the binary ships without one; drop a doom.wad into j36/ on the card"
        fi
    fi
else
    log "fbdoom: J36_DOOM=0, skipping the Doom payload"
fi

# ── The lima payload: one helper and the module set it gates ──────────────────
#
# CONFIG_DRM_LIMA is =m, so the driver is not in the kernel and not in the
# initramfs either.  It goes on the FAT BOOT partition beside the kernel, with the
# userspace helper that has to run before it, for the reasons the GPU section of
# the kernel configuration gives: the MFG power domain is gated when Linux starts,
# and a driver that probes an unpowered MTK subsystem stalls the AXI bus into a
# watchdog reset.  Staging it here means the whole GPU experiment is a directory
# on a card and one word in boot.conf -- removable from any machine that can read
# an SD card, with no reflash and no rebuild.
#
# THE LOAD ORDER IS READ OUT OF THE MODULES, NOT WRITTEN HERE.  lima.ko needs
# gpu-sched.ko and drm_shmem_helper.ko, which need nothing more, and the
# initramfs has insmod and not modprobe -- it resolves nothing itself and fails on
# an unresolved symbol.  So the set is walked transitively from lima with
# `modinfo -F depends' and emitted dependency-first into j36/modules/load.order,
# which /init reads line by line.  A dependency whose .ko is absent is not an
# error: it means that symbol's owner is built into vmlinux, which is exactly what
# DRM=y produces for the drm core itself.
#
# Non-fatal, like fbdoom: the GPU must never cost the user the kernel artifacts.
MFGPOWER_SRC="$ROOT/device/j36-ultra/tools/mfgpower.c"
MFGPOWER_BIN=""
LIMA_MODULE_PATHS=()
LIMA_MODULE_ORDER=()

build_mfgpower() {
    local out="$WORK/mfgpower" header

    [[ -f "$MFGPOWER_SRC" ]] || { log "lima: $MFGPOWER_SRC is missing"; return 1; }
    # Static for the same reason Doom is: this runs from the initramfs, before
    # switch_root, where there is no ld.so and no /lib.
    arm-linux-gnueabihf-gcc -O2 -std=gnu11 -Wall -Wextra -static \
        -o "$out" "$MFGPOWER_SRC" || return 1

    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "lima: mfgpower is not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "lima: mfgpower is not an ARM ELF"; return 1; }
    if grep -q 'NEEDED' <<<"$header"; then
        log "lima: mfgpower wants shared libraries and the initramfs has none"
        return 1
    fi
    MFGPOWER_BIN="$out"
    log "lima: mfgpower is $(stat -c %s "$out") bytes, static ARM"
    return 0
}

# collect_modules <label> <order-array> <paths-array> <root>...
#
# ONE ROOT IS NOT ENOUGH, AND THAT IS WHY THIS TAKES A LIST.  The walk below
# follows `modinfo -F depends', which is a SYMBOL relationship, and the display
# set has a member that no symbol points at: mediatek-drm reaches the MIPI-TX PHY
# through the generic phy API, so a walk seeded only at mediatek-drm would build a
# load.order with phy-mtk-mipi-dsi-drv missing from it and mtk_dsi would sit in
# deferred probe forever with nothing in the log naming the absent module.  The
# out-of-tree panel module is the same case for the same reason.  So every module
# that has to be present is named as a root, and the walk's job is only to find
# what those roots need underneath them and to order the result.
#
# Search paths are $KERNEL_OUT and $MODULE_SRC in that order, because the panel
# is built out-of-tree and is not under the kernel build directory at all.
collect_modules() {
    local label="$1"
    local -n order_ref="$2" paths_ref="$3"
    shift 3
    local -A ko_path=() ko_deps=() emitted=() builtin=()
    local -a pending=("$@") roots=("$@") discovery=() deplist=()
    local name path deps dep ready progress header dir root

    # `builtin' is not bookkeeping for its own sake: without it a name that has no
    # .ko is re-searched once per module that depends on it, and each search walks
    # the whole kernel build tree.  drm is exactly that case, being DRM=y.
    while (( ${#pending[@]} )); do
        name="${pending[0]}"
        pending=("${pending[@]:1}")
        if [[ -n "${ko_path[$name]:-}" || -n "${builtin[$name]:-}" ]]; then continue; fi
        path=""
        for dir in "$KERNEL_OUT" "$MODULE_SRC"; do
            path="$(find "$dir" -name "$name.ko" -print -quit 2>/dev/null)"
            [[ -n "$path" ]] && break
        done
        if [[ -z "$path" ]]; then
            builtin[$name]=1
            log "$label: $name.ko was not built; its symbols must be in vmlinux"
            continue
        fi
        header="$(readelf -h "$path" 2>/dev/null)" || { log "$label: cannot read $path"; return 1; }
        grep -q 'Machine:.*ARM' <<<"$header" || { log "$label: $name.ko is not an ARM object"; return 1; }
        ko_path[$name]="$path"
        discovery+=("$name")
        deps="$(modinfo -F depends "$path" 2>/dev/null || true)"
        ko_deps[$name]="$deps"
        read -ra deplist <<<"${deps//,/ }"
        for dep in "${deplist[@]}"; do pending+=("$dep"); done
    done

    # A root that resolved to nothing is fatal, not a "must be built in": a root is
    # named here precisely because this payload is useless without it.
    for root in "${roots[@]}"; do
        if [[ -z "${ko_path[$root]:-}" ]]; then
            log "$label: $root.ko was not built even though its Kconfig symbol was asserted =m"
            return 1
        fi
    done

    # Emit dependency-first.  Repeated passes rather than a recursive walk: the set
    # is a handful of modules deep at most, and a cycle -- which the module loader
    # could not resolve either -- shows up as a pass that emits nothing.
    #
    # The inner loop walks `discovery' and not the associative array's own key
    # order, so the output is deterministic and follows the order the roots were
    # given.  Insmod order among independent modules does not have to be right --
    # deferred probe sorts that out -- but a load.order that shuffles between
    # builds is one more thing to rule out when a boot goes wrong.
    while (( ${#order_ref[@]} < ${#ko_path[@]} )); do
        progress=0
        for name in "${discovery[@]}"; do
            if [[ -n "${emitted[$name]:-}" ]]; then continue; fi
            ready=1
            read -ra deplist <<<"${ko_deps[$name]//,/ }"
            for dep in "${deplist[@]}"; do
                if [[ -n "${ko_path[$dep]:-}" && -z "${emitted[$dep]:-}" ]]; then ready=0; fi
            done
            if (( ready )); then
                emitted[$name]=1
                order_ref+=("$name.ko")
                paths_ref+=("${ko_path[$name]}")
                progress=1
            fi
        done
        if (( progress == 0 )); then
            log "$label: circular dependency among ${!ko_path[*]}"
            return 1
        fi
    done

    log "$label: load order ${order_ref[*]}"
    return 0
}

if [[ "${J36_LIMA:-1}" == 1 ]]; then
    set +e
    build_mfgpower
    mfg_rc=$?
    set -e
    if (( mfg_rc != 0 )); then
        MFGPOWER_BIN=""
        log "lima: mfgpower not staged, see the error above -- the kernel payload is unaffected"
    else
        set +e
        collect_modules lima LIMA_MODULE_ORDER LIMA_MODULE_PATHS lima
        lima_rc=$?
        set -e
        if (( lima_rc != 0 )); then
            MFGPOWER_BIN=""
            LIMA_MODULE_ORDER=()
            LIMA_MODULE_PATHS=()
            log "lima: modules not staged; the helper is withheld too, since it has nothing to gate"
        fi
    fi
else
    log "lima: J36_LIMA=0, skipping the GPU payload"
fi

# The display set, collected the same way and staged just as removably.  There is
# no mfgpower equivalent here and none is needed: the DISP domain is already
# powered and already scanning out, because the LK left it that way, so there is no
# unpowered-bus-stall class of failure to gate against.  What makes this payload
# safe is elsewhere -- DRM_FBDEV_EMULATION=n means mediatek-drm binds, registers
# card0 and programs not one display register until userspace opens the node.
#
# All six names are roots, and the two that matter are the last two: mediatek-drm
# reaches phy-mtk-mipi-dsi-drv through the generic phy API and the panel through
# the DSI host bus, neither of which is a symbol relationship, so a dependency walk
# cannot find either from mediatek-drm alone.
#
# j36_fbmem is first, and it is here for what it is about rather than for what it
# depends on -- which is nothing: dma_buf_export is in vmlinux, so it has no
# depends edge to anything else in this list and could load in any order.  It is a
# display module because the memory it exports is the memory the DDP is scanning,
# and the same sentence that makes the rest of this payload safe makes it safe:
# it programs no display register at all.  First rather than last so that
# /dev/j36fb exists even on a boot where the DSI chain below it fails, because a
# board with no card node is exactly the board somebody wants to ask "is the
# bootloader's framebuffer still where it said it was" about.
MTKDRM_MODULE_PATHS=()
MTKDRM_MODULE_ORDER=()
if [[ "${J36_MTKDRM:-1}" == 1 ]]; then
    set +e
    collect_modules mtkdrm MTKDRM_MODULE_ORDER MTKDRM_MODULE_PATHS \
        j36_fbmem phy-mtk-mipi-dsi-drv mtk-mmsys mtk-mutex mediatek-drm \
        j36_jd9365_panel
    mtkdrm_rc=$?
    set -e
    if (( mtkdrm_rc != 0 )); then
        MTKDRM_MODULE_ORDER=()
        MTKDRM_MODULE_PATHS=()
        log "mtkdrm: modules not staged, see the error above -- the kernel payload is unaffected"
    fi
else
    log "mtkdrm: J36_MTKDRM=0, skipping the display payload"
fi

# The ALSA core and the AFE adapter, collected the same way and staged the same
# way.  Two roots and only two: snd-pcm brings snd and snd-timer with it through
# `modinfo -F depends', and j36_mt6592_audio is a root because it is out-of-tree
# and nothing in the kernel tree points at it.
#
# snd-dummy is deliberately NOT a root and is deliberately not staged.  It was
# built for one reason -- CONFIG_SND_PCM has no prompt and only exists to be
# selected, and snd-dummy is the smallest in-tree card that selects it -- and
# staging it would register a second sound card that would race the AFE for card0.
#
# The one failure this cannot report is the interesting one: whether the AFE's DMA
# actually runs.  That is a measurement, it needs the board, and the driver makes
# it on the first stream and puts the verdict in dmesg.
AUDIO_MODULE_PATHS=()
AUDIO_MODULE_ORDER=()
if [[ "${J36_AUDIO:-1}" == 1 ]]; then
    set +e
    collect_modules audio AUDIO_MODULE_ORDER AUDIO_MODULE_PATHS \
        snd-pcm j36_mt6592_audio
    audio_rc=$?
    set -e
    if (( audio_rc != 0 )); then
        AUDIO_MODULE_ORDER=()
        AUDIO_MODULE_PATHS=()
        log "audio: modules not staged, see the error above -- the kernel payload is unaffected"
    fi
else
    log "audio: J36_AUDIO=0, skipping the audio payload"
fi

# The USB host set: one controller, the HID pair, and the DisplayLink display.
#
# The roots, and why each one is a root rather than something the walk finds:
#
#   j36_mt6592_usb_phy  out-of-tree, so nothing in the kernel tree names it, and
#                       it MUST load before musb_hdrc -- it is the driver that
#                       ungates the PERI clock.  The walk gets that for free:
#                       usbcore is the one thing under it, so the dependency-first
#                       emit puts usbcore, then the PHY, then musb_hdrc.  usbcore
#                       is a real edge and not an accident of the layout -- the
#                       PHY calls usb_for_each_dev() to ask whether the thing on
#                       the port ever got an address, which is what separates a
#                       device from a charger holding the same line high.
#   mediatek            drivers/usb/musb/mediatek.ko, the glue.  A root because
#                       the relationship to musb_hdrc runs the other way: musb
#                       exports the symbols, the glue is what binds the node.
#                       The generic name is worth a second look if a kernel bump
#                       ever adds another mediatek.ko -- collect_modules takes
#                       the first match under $KERNEL_OUT and today there is
#                       exactly one.
#   musb_hdrc           named anyway rather than left to the glue's depends, so
#                       that a build which somehow produced the glue without the
#                       core fails here instead of at insmod time on the board.
#   usbhid, hid-generic the mouse and the keyboard.  hid-generic is a root
#                       because no symbol points at it: it is the driver that
#                       claims any HID device no specific driver wanted, so a
#                       walk seeded at usbhid alone would build a payload that
#                       enumerates a mouse and binds nothing to it.
#   udl                 the USB->HDMI adapter.  Also a root for a reason of its
#                       own -- it hangs off usbcore and DRM, neither of which
#                       knows it exists.
#   usb-storage, sd_mod the external disk.  Two roots and not one, because the
#                       edge between them runs through neither: usb-storage is a
#                       SCSI HOST and sd_mod is a SCSI UPPER LAYER, they are
#                       siblings under scsi_mod rather than parent and child, and
#                       a walk seeded at usb-storage alone stages a payload that
#                       enumerates the disk and never creates /dev/sda.  scsi_mod
#                       itself is not a root: it is a depends edge from both.
#   ntfs3               the filesystem on the disk somebody actually plugs in.  A
#                       root because no module points at a filesystem driver --
#                       nothing does until mount(2) asks for the type by name,
#                       which is far too late to discover it was never staged.
#
# usbcore, hid, phy-generic, roles and scsi_mod are NOT roots and do not need to
# be: every one of them is a `modinfo -F depends' edge from something above.  Hub
# support has no symbol at all -- it is inside usbcore -- which is worth writing
# down because "there is no CONFIG_USB_HUB" is the first thing anyone looks for.
USB_MODULE_PATHS=()
USB_MODULE_ORDER=()
if [[ "${J36_USB:-1}" == 1 ]]; then
    set +e
    collect_modules usb USB_MODULE_ORDER USB_MODULE_PATHS \
        j36_mt6592_usb_phy musb_hdrc mediatek usbhid hid-generic udl \
        usb-storage sd_mod ntfs3
    usb_rc=$?
    set -e
    if (( usb_rc != 0 )); then
        USB_MODULE_ORDER=()
        USB_MODULE_PATHS=()
        log "usb: modules not staged, see the error above -- the kernel payload is unaffected"
    fi
    # `mediatek' is the one root here with a name generic enough to collide.  The
    # MUSB glue really is drivers/usb/musb/mediatek.ko, and collect_modules takes
    # the first `find -name mediatek.ko' hit in the build tree, so say out loud
    # which file that was rather than discover at insmod time that it was some
    # other subsystem's driver of the same name.
    if (( ${#USB_MODULE_ORDER[@]} > 0 )); then
        for i in "${!USB_MODULE_ORDER[@]}"; do
            [[ "${USB_MODULE_ORDER[$i]}" == mediatek.ko ]] || continue
            [[ "${USB_MODULE_PATHS[$i]}" == */usb/* ]] || \
                die "usb: mediatek.ko resolved to ${USB_MODULE_PATHS[$i]}, which is not the MUSB glue"
        done
    fi
else
    log "usb: J36_USB=0, skipping the USB payload"
fi

# The PMIC and the backlight, which are a payload of exactly two modules.
#
# Neither has any depends at all: the power supply class and the backlight class
# are both =y, asserted by name in the config section above, the sys-off handler
# is core, and everything else either of them touches it reaches through a
# phandle and an ioremap.  So the walk finds two files and those two files are
# the whole payload -- which is worth stating, because a load.order with two
# lines looks like a truncated one.
#
# THE BACKLIGHT IS HERE AND NOT WITH mtkdrm, and the reason is what the word
# means rather than what the block is.  j36.mtkdrm is the modesetting experiment
# -- it is allowed to be off, and the board still shows a picture without it,
# because the LK's simple-framebuffer scanout does not need DRM.  Brightness is
# not part of that experiment: it is the largest single load on this battery, it
# is the setting a handheld gets asked for first, and it belongs with the word
# that is about what the board draws.  It also has no relationship to the DRM
# module set at all -- it binds the BLS block directly and registers a backlight
# device, so putting it in mtkdrm's load.order would have coupled it to a chain
# of five modules it neither needs nor is needed by.
#
# It is a payload rather than an initramfs module for a reason the other four do
# not have: it is the only one that writes registers a reboot does not clear.
# The MT6323's charger bank keeps CV, the current-sense threshold and the enable
# bits across a warm reset, so a bad charger sequence is a state the next boot
# inherits.  Behind its own word and its own directory, the recovery is the same
# as for audio -- delete j36/power/ from any machine that reads SD cards, or drop
# the word, and the board boots with the charger in whatever state the LK left it,
# which is the state it shipped in.
POWER_MODULE_PATHS=()
POWER_MODULE_ORDER=()
if [[ "${J36_POWER:-1}" == 1 ]]; then
    set +e
    collect_modules power POWER_MODULE_ORDER POWER_MODULE_PATHS \
        j36_mt6592_pmic j36_mt6592_backlight
    power_rc=$?
    set -e
    if (( power_rc != 0 )); then
        POWER_MODULE_ORDER=()
        POWER_MODULE_PATHS=()
        log "power: modules not staged, see the error above -- the kernel payload is unaffected"
    fi
else
    log "power: J36_POWER=0, skipping the PMIC payload"
fi

# The connectivity subsystem: one out-of-tree module built from five translation
# units, plus the two ROM patches it sends the connectivity MCU.
#
# THE WALK FINDS THE PMIC AND THAT IS CORRECT, not something to filter out.
# j36_mt6592_wifi links against j36_mt6592_pmic's two exported wrapper calls --
# there is one owner of the WACS2 bridge in this kernel and this is not it -- so
# `modinfo -F depends' reports the edge and the payload gets a second copy of a
# 40 KB module.  Two copies is the same trade j36/usb/ makes with the DRM helper
# pair, and it buys the same thing: j36/wifi/ is removable on its own, and its
# load order is complete on its own.  run_wifi skips whichever of them run_power
# already loaded, which on any boot that reaches it is the PMIC.
#
# THE WALK ALSO FINDS CFG80211 AND RFKILL, and they are not named as roots for
# the same reason: they are real symbol dependencies of the radio module, so
# `modinfo -F depends' reports them and the emit-dependency-first pass puts them
# ahead of j36_mt6592_wifi.ko in load.order.  That ordering is the whole point --
# the initramfs has insmod and not modprobe, it resolves nothing itself, and
# loading the radio first fails on unresolved cfg80211 symbols.
#
# Nothing else is a root.  In particular there is no in-tree WLAN driver here to
# find: CONFIG_WLAN is refused in the kernel configuration above, because the
# only radio on this board is driven from device/j36-ultra/linux.
WIFI_MODULE_PATHS=()
WIFI_MODULE_ORDER=()
if [[ "${J36_WIFI:-1}" == 1 ]]; then
    set +e
    collect_modules wifi WIFI_MODULE_ORDER WIFI_MODULE_PATHS j36_mt6592_wifi
    wifi_rc=$?
    set -e
    if (( wifi_rc != 0 )); then
        WIFI_MODULE_ORDER=()
        WIFI_MODULE_PATHS=()
        log "wifi: modules not staged, see the error above -- the kernel payload is unaffected"
    fi
else
    log "wifi: J36_WIFI=0, skipping the connectivity payload"
fi

# ── The GL runtime, which turned out not to need building ─────────────────────
#
# The plan of record for this step was "armhf Mesa with lima and kmsro on a
# J36-only library path". Two thirds of that had already happened without anyone
# arranging it, which measuring the built rootfs is the only way to find out:
#
#   /usr/lib/arm-linux-gnueabihf/dri/lima_dri.so       -> libdril_dri.so
#   /usr/lib/arm-linux-gnueabihf/dri/mediatek_dri.so   -> libdril_dri.so
#   /usr/lib/arm-linux-gnueabihf/dri/libdril_dri.so    present, 132 KB
#   libgallium-25.0.7-2+deb13u1.so                     present, 25 MB
#   libEGL_mesa.so.0.0.0                               present, 265 KB
#   libdrm.so.2.124.0                                  present
#   /usr/share/glvnd/egl_vendor.d/50_mesa.json          present, names libEGL_mesa.so.0
#
# The two _dri.so entries being symlinks is not a broken install, it is Mesa's
# current layout: one megadriver, libdril_dri.so, with a symlink per kernel driver
# name, and the loader picks by the name it looked up. So mediatek_dri.so existing
# is what makes an mtk_drm card usable as a kmsro display device, and lima_dri.so
# existing is what makes the Mali-450 render node the renderer behind it.
#
# Debian trixie's armhf Mesa is 25.0.7 and it ships both halves of the pair this
# board needs: lima for the Mali-450 render node and mediatek for kmsro, which is
# the kmsro entry that pairs an mtk_drm card with a lima renderer. So there is
# nothing to cross-compile. MixOS installs that Mesa and then, in utils.sh,
# overwrites the FRONT of it -- libEGL.so, libGLESv2.so*, libgbm.so* and
# libGLESv1_CM.so are replaced by symlinks to the RK3326's Mali-G31 blob. The
# back end was never touched.
#
# So this payload is the front end, and only the front end: the five glvnd/Mesa
# libraries whose names MixOS took over, staged where only this board looks.
# Nothing on the shared rootfs is modified or moved -- the R36S keeps its
# libMali.so symlinks exactly as they are, which it has to, because that blob is
# the only thing that drives its GPU.
#
# Which names it actually took over, read off the built image rather than off
# utils.sh, because the two do not agree -- utils.sh names four libraries and what
# it leaves behind is this:
#
#   libEGL.so             -> libMali.so     clobbered   the bare -dev name
#   libgbm.so             -> libMali.so     clobbered
#   libgbm.so.1           -> libMali.so     clobbered   an SONAME, see below
#   libgbm.so.1.0.0       -> libMali.so     clobbered
#   libGLESv1_CM.so       -> libMali.so     clobbered
#   libGLESv1_CM.so.1.1.0 -> libMali.so     clobbered   stale SONAME, unused
#   libEGL.so.1           -> libEGL.so.1.1.0        intact, real glvnd
#   libGLESv1_CM.so.1     -> libGLESv1_CM.so.1.2.0  intact, real glvnd
#
# The versioned SONAMEs of libEGL and libGLESv1_CM survive; the bare development
# names do not, and all three libgbm names do not. That split is what decides which
# payload entries are load-bearing, and the answer is not the obvious one:
#
#   - libEGL.so.1 is the entry point everything on this board goes through:
#     eglprobe dlopens exactly that soname, and so does SDL2's KMSDRM backend
#     (SDL_VIDEO_EGL_DRIVER defaults to it). The bare libEGL.so beside it is
#     normally a -dev symlink no runtime binary should reference, and here it
#     resolves to a Mali blob for a GPU that is not on this board, so anything that
#     does reference it takes SIGILL before main(). Both names are shipped.
#   - libgbm.so.1 is load-bearing TWICE, and the second time is the one that would
#     have been missed: SDL2's KMSDRM backend dlopens it, and libEGL_mesa.so.0
#     carries it in its own DT_NEEDED. So with the rootfs as it stands, glvnd reads
#     50_mesa.json, dlopens Mesa's EGL vendor, and that pulls the RK3326 blob into
#     the process as its gbm. Mesa's EGL cannot initialise on this board without
#     this payload -- the point is not merely that one program fails to link.
#   - libGL.so.1 and libGLESv1_CM.so.1 are here for the probe rather than for
#     anything in the boot. eglprobe asks each node for a desktop-GL context and an
#     ES1 one as well as ES2, because the three answers together say whether a
#     failure is the driver or the Mesa build: ES1 is EGL_BAD_ALLOC on lima, on
#     llvmpipe AND on softpipe, which is Debian's armhf Mesa being a
#     -Dgles1=disabled build and not anything about this GPU. libGL.so.1 brings
#     libGLX.so.0 with it (its DT_NEEDED), which wants libX11.so.6 -- present on
#     this rootfs; nothing here opens a display, it is glvnd's
#     one-library-for-both-APIs shape.
#   - libGLESv2.so.2 is the one a program on this board actually draws through --
#     eglprobe's cube dlopens it by that soname -- and libGLdispatch.so.0 is
#     glvnd's own dependency and the only DT_NEEDED any of these have on each
#     other. Only one copy of it is ever loaded, and LD_LIBRARY_PATH puts the
#     payload's ahead of the rootfs's, so whichever front end a symbol came from
#     dispatches into whatever eglMakeCurrent installed.
#
# The intact ones are shipped anyway. Which names survive is an accident of which
# utils.sh path ran on the day the image was built, and a payload that depends on
# an accident is not a payload. Shipping all of them costs about 2.5 MB and
# removes the question.
GL_PACKAGES=(libgbm1 libglvnd0 libegl1 libgl1 libglx0 libgles1 libgles2)
GL_PAYLOAD=""
GL_MIRROR="${J36_DEBIAN_MIRROR:-http://deb.debian.org/debian}"
GL_SUITES=("${DEBIAN_RELEASE:-trixie}" "${DEBIAN_RELEASE:-trixie}-updates")

# Resolved out of the archive's own Packages index rather than from hand-written
# .deb URLs, because a pinned URL stops existing the moment Debian supersedes the
# revision -- the pool keeps one version per source and a point release moves it.
# The index says where the current one is, whatever it is called this month.
collect_gl_payload() {
    local out="$WORK/gl-payload" idx="$WORK/gl-index" pkg suite url filename deb
    local -a found=()

    rm -rf "$out" "$idx"
    mkdir -p "$out" "$idx"

    # .xz first and .gz second, because the archive does not carry both for every
    # suite: trixie has a Packages.gz and trixie-updates answers 404 for it. Trying
    # one compression and giving up would silently stop looking at -updates, which
    # is the half that carries a superseded revision.
    local ext indices=0
    for suite in "${GL_SUITES[@]}"; do
        for ext in xz gz; do
            url="$GL_MIRROR/dists/$suite/main/binary-armhf/Packages.$ext"
            curl -fsSL --retry 3 --max-time 300 --remove-on-error \
                -o "$idx/$suite.$ext" "$url" || continue
            case "$ext" in
                xz) xz -dc "$idx/$suite.$ext" > "$idx/$suite" ;;
                gz) gzip -dc "$idx/$suite.$ext" > "$idx/$suite" ;;
            esac || return 1
            log "gl: read the $suite armhf package index ($ext)"
            break
        done
        if [[ -s "$idx/$suite" ]]; then
            indices=$((indices + 1))
        else
            log "gl: no $suite index in the archive"
        fi
    done
    (( indices > 0 )) || { log "gl: no package index could be fetched"; return 1; }

    for pkg in "${GL_PACKAGES[@]}"; do
        filename=""
        # A later suite wins only when it actually carries the package: a
        # -updates index that has never shipped this source must not blank out the
        # hit the base suite gave. awk rather than grep -A, because a Filename:
        # field can sit anywhere in its stanza.
        local hit
        for suite in "${GL_SUITES[@]}"; do
            [[ -f "$idx/$suite" ]] || continue
            hit="$(awk -v p="$pkg" '
                /^Package: /  { inpkg = ($2 == p) }
                /^Filename: / { if (inpkg) found = $2 }
                END           { print found }' "$idx/$suite")"
            [[ -n "$hit" ]] && filename="$hit"
        done
        if [[ -z "$filename" ]]; then
            log "gl: $pkg is not in any index that was fetched"
            return 1
        fi
        deb="$out/$(basename "$filename")"
        curl -fsSL --retry 3 --max-time 300 -o "$deb" "$GL_MIRROR/$filename" || {
            log "gl: could not download $filename"; return 1; }
        # --fsys-tarfile and not -x, so that the package's own symlinks are
        # discarded here instead of being copied onto a filesystem that cannot
        # hold them. The links are rebuilt from SONAMEs below.
        dpkg-deb --fsys-tarfile "$deb" | \
            tar -C "$out" -xf - --wildcards --no-same-owner \
                './usr/lib/arm-linux-gnueabihf/*.so*' 2>/dev/null || true
        rm -f "$deb"
        found+=("$pkg")
    done

    local libdir="$out/usr/lib/arm-linux-gnueabihf" so soname base
    [[ -d "$libdir" ]] || { log "gl: the packages contained no armhf libraries"; return 1; }

    GL_PAYLOAD="$out/staged"
    mkdir -p "$GL_PAYLOAD"
    : > "$GL_PAYLOAD/links"

    # The links file exists because vfat has no symlinks. Each line is
    # "name target" and /init makes them in a tmpfs at boot. Targets are derived
    # from each library's own SONAME rather than written down, so a Debian
    # revision that bumps a minor version does not silently produce a payload
    # whose links point at filenames that are not in it.
    for so in "$libdir"/*.so*; do
        [[ -f "$so" && ! -L "$so" ]] || continue
        base="$(basename "$so")"
        verify_arm_elf "$so" "the GL library $base"
        cp "$so" "$GL_PAYLOAD/$base"
        soname="$(readelf -d "$so" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')"
        if [[ -n "$soname" && "$soname" != "$base" ]]; then
            printf '%s %s\n' "$soname" "$base" >> "$GL_PAYLOAD/links"
        fi
    done

    # And one link that is not a SONAME. Anything built against this rootfs on the
    # device records the bare development name `libEGL.so' in its DT_NEEDED, because
    # that is what the rootfs's libEGL.so -> libMali.so symlink is called and the
    # blob exported EGL and GLES1 and GLES2 all from one object. glvnd's libEGL is
    # only ever installed as libEGL.so.1, so without this alias such a binary does
    # not load at all -- and with the rootfs's own symlink first it loads the ARMv8
    # blob and takes SIGILL, which is the failure this whole payload exists to stop.
    local egl
    egl="$(sed -n 's/^libEGL\.so\.1 \(.*\)$/\1/p' "$GL_PAYLOAD/links")"
    if [[ -z "$egl" ]]; then
        log "gl: no library in the payload has SONAME libEGL.so.1"
        return 1
    fi
    printf '%s %s\n' "libEGL.so" "$egl" >> "$GL_PAYLOAD/links"

    log "gl: staged $(ls -1 "$GL_PAYLOAD" | grep -c '\.so') libraries from ${found[*]}"
    log "gl: links $(tr '\n' ';' < "$GL_PAYLOAD/links")"
    return 0
}

# The one question a status 134 does not answer: which API, on which node, and
# with which EGL error.  tools/j36-eglprobe.c asks it directly -- the reasoning is
# at the top of that file.  Built here rather than downloaded because nothing in
# Debian answers it: eglinfo pulls libX11 and a dev stack into the image, and this
# pulls nothing.
#
# Dynamic, unlike mfgpower, because it dlopens the very libraries the payload
# above stages -- so it runs after switch_root, where there is an ld.so.  Nothing
# beyond libc and the interpreter may be needed: dlopen moved into libc in glibc
# 2.34 and both the VM's and the rootfs's are past that, so -ldl would add nothing
# but a stub.  That is asserted rather than assumed, because a probe that cannot
# load is worse than no probe -- it would look like the GL payload is what failed.
# armhf gcc names the interpreter in DT_NEEDED as well as in PT_INTERP, so
# ld-linux-armhf.so.3 is expected there and is not a dependency this has to ship.
#
# Non-fatal, like fbdoom and mfgpower: a diagnostic must never cost the user the
# kernel artifacts.  Hence the local readelf tests instead of verify_arm_elf,
# which dies.
EGLPROBE_SRC="$ROOT/device/j36-ultra/tools/j36-eglprobe.c"
EGLPROBE_BIN=""

build_eglprobe() {
    local out="$WORK/j36-eglprobe" header needed

    [[ -f "$EGLPROBE_SRC" ]] || { log "gl: $EGLPROBE_SRC is missing"; return 1; }
    arm-linux-gnueabihf-gcc -O2 -std=gnu11 -Wall -Wextra \
        -o "$out" "$EGLPROBE_SRC" || return 1

    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "gl: eglprobe is not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "gl: eglprobe is not an ARM ELF"; return 1; }
    needed="$(sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' <<<"$header" | tr '\n' ' ')"
    local unexpected="" lib
    for lib in $needed; do
        case "$lib" in
            libc.so.6|ld-linux-armhf.so.3) ;;
            *) unexpected="$unexpected $lib" ;;
        esac
    done
    if [[ -n "$unexpected" ]]; then
        log "gl: eglprobe needs libraries the rootfs may not have:$unexpected"
        return 1
    fi

    EGLPROBE_BIN="$out"
    log "gl: eglprobe is $(stat -c %s "$out") bytes, dynamic ARM, needs $needed"

    return 0
}

# ── The USB-HDMI mirror ───────────────────────────────────────────────────────
#
# WHAT IT IS FOR.  Plug a DisplayLink dongle into this board and udl binds, a
# /dev/dri/cardN appears with the monitor's modes on it, and the screen stays black.
# (A DisplayLink dongle, and not the USB-C hub with an HDMI socket on the side that
# most people mean by "USB HDMI adapter".  Those are DisplayPort Alt Mode -- the
# connector's high-speed pairs switched onto a DisplayPort transmitter in the host --
# and MT6592 has neither the connector nor the transmitter.  Nothing below applies to
# one, because udl never binds and no card node ever appears.)  Nothing is broken: CONFIG_DRM_FBDEV_EMULATION is =n on
# purpose -- see the DRM prune far above, it is a global bool and turning it on would
# have mtk_drm register a second /dev/fb and take the panel -- so udl has a card node
# and no framebuffer, while mixdash is Qt on linuxfb and knows only /dev/fb0.  The two
# cannot see each other.  j36-mixmirror copies between them: it diffs /dev/fb0 in
# tiles, blits the changed ones into a dumb buffer on the udl node, and pushes them
# with DRM_IOCTL_MODE_DIRTYFB, which is the ioctl udl actually transfers on.
#
# The file's own header has the rest, including why this is a mirror and not a second
# display server, and what stops it ever modesetting the panel by mistake.
#
# STATIC, and for a reason beyond neatness: this way it can also be run from the
# initramfs shell, where there is no ld.so and no /lib, which is the shell somebody
# ends up in when the dock is the thing that is not working.
#
# Non-fatal, like eglprobe, fbdoom and mfgpower.  A board with no dongle plugged into
# it must not lose its kernel artifacts because a 700-line accessory failed to build.
MIXMIRROR_SRC="$ROOT/device/j36-ultra/tools/j36-mixmirror.c"
MIXMIRROR_BIN=""

build_mixmirror() {
    local out="$WORK/j36-mixmirror" header

    [[ -f "$MIXMIRROR_SRC" ]] || { log "mirror: $MIXMIRROR_SRC is missing"; return 1; }
    arm-linux-gnueabihf-gcc -O2 -std=gnu11 -Wall -Wextra -static \
        -o "$out" "$MIXMIRROR_SRC" || return 1

    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "mirror: not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "mirror: not an ARM ELF"; return 1; }
    if grep -q 'NEEDED' <<<"$header"; then
        log "mirror: the binary wants shared libraries and -static was asked for"
        return 1
    fi

    MIXMIRROR_BIN="$out"
    log "mirror: j36-mixmirror is $(stat -c %s "$out") bytes, static ARM"
    return 0
}

# ── The armhf chroot ──────────────────────────────────────────────────────────
#
# One emulated Debian armhf trixie tree, built once and kept, in which the dashboard
# is compiled against Debian's own qtbase5-dev.  Nothing is cross-compiled: the
# target runs the same package versions, so what links here links there.
#
# IT USED TO BUILD TWO THINGS.  The other was EmulationStation, rebuilt from
# christianhaitian/EmulationStation-fcamod at a pinned commit with a third renderer
# -- es/Renderer_GLES20.cpp -- and a patch that took `EGL' off the link line, because
# GLES1 is the one API this stack cannot provide: eglCreateContext for an ES1 context
# returns 0x3003 EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike, since
# Debian's armhf Mesa is a -Dgles1=disabled build.  That binary went onto the BOOT
# partition and /init bind-mounted it over the rootfs's copy.  All of it is gone with
# EmulationStation itself, along with device/j36-ultra/es/ and the libvlc-dev,
# libfreeimage-dev, rapidjson-dev half of what this chroot used to install.
#
# What survives from that work is the part that was never ES-specific and is directly
# above: Mesa staged into /run/j36/gl, and eglprobe, which is now run by
# mixdash-probe.service and by the dashboard's 3D cube card.
#
# The directory was $WORK/es-chroot, and it is renamed here rather than left alone
# because the name is the last thing on the host side still saying EmulationStation.
# An existing one is moved rather than rebuilt -- re-extracting the base is cheap but
# re-running apt for qtbase5-dev under qemu-arm is not.
ARMHF_CHROOT="$WORK/armhf-chroot"

armhf_chroot_run() {
    sudo chroot "$ARMHF_CHROOT" bash -c "$1"
}

# The chroot is built once and kept.  Preferred base is MixOS's own armhf rootfs
# cache, because if the R36 base build has run then it is already on disk and it is
# the same debootstrap the target was made from; debootstrap is the fallback for a
# machine where only this build has ever run.
#
# The base and the dependency install are separate steps, and chroot_install_deps
# keeps a stamp per set.  That mattered when two different programs built in here;
# with one left it is what stops a re-run from doing the qtbase5-dev apt again.
ensure_armhf_chroot() {
    local suite="${DEBIAN_RELEASE:-trixie}" base m
    base="$ROOT/MixOSBuild_package_cache/debian_${suite}_userspace-armhf_rootfs.tar.gz"

    # The rename described above ARMHF_CHROOT, done once and quietly.  Guarded on the
    # stamp file so a half-made or already-migrated tree is left alone, and on the
    # destination not existing so this can never write over a good chroot.  If the
    # move fails -- a bind mount still up inside it, most likely -- nothing is said
    # and the block below rebuilds, which is slow but correct.
    if [[ -f "$WORK/es-chroot/.j36-base" && ! -e "$ARMHF_CHROOT" ]]; then
        log "chroot: moving the armhf chroot from es-chroot to armhf-chroot"
        sudo mv "$WORK/es-chroot" "$ARMHF_CHROOT" || true
    fi

    if [[ ! -f "$ARMHF_CHROOT/.j36-base" ]]; then
        sudo rm -rf "$ARMHF_CHROOT" "$WORK/armhf-chroot-x"
        mkdir -p "$WORK/armhf-chroot-x"
        if [[ -f "$base" ]]; then
            log "chroot: unpacking the armhf $suite rootfs from the MixOS package cache"
            sudo tar -xpzf "$base" -C "$WORK/armhf-chroot-x" || return 1
            # The cache tarball carries MixOS's own chroot name at the top.
            sudo mv "$WORK/armhf-chroot-x/MixOSBuild" "$ARMHF_CHROOT" || return 1
        else
            log "chroot: no armhf rootfs in the package cache, debootstrapping one"
            command -v qemu-arm-static >/dev/null || {
                log "chroot: qemu-arm-static is not installed and there is no cached rootfs"; return 1; }
            sudo eatmydata debootstrap --no-check-gpg --include=eatmydata \
                --resolve-deps --arch=armhf --foreign "$suite" "$ARMHF_CHROOT" \
                "${J36_DEBIAN_MIRROR:-http://deb.debian.org/debian}" || return 1
            sudo cp /usr/bin/qemu-arm-static "$ARMHF_CHROOT/usr/bin/" || return 1
            sudo chroot "$ARMHF_CHROOT" /debootstrap/debootstrap --second-stage || return 1
        fi
        sudo rm -rf "$WORK/armhf-chroot-x"
        sudo touch "$ARMHF_CHROOT/.j36-base"
    fi

    # /dev, /proc and /sys are bound for apt's maintainer scripts and for nproc.
    # They are unmounted in armhf_chroot_teardown when the build is done: a bind mount
    # of /sys left inside a directory tree is something the next rsync of this
    # machine trips over, with an unlink() permission denied that names a sysfs
    # file and explains nothing.
    #
    # --make-rslave on each of them, because a shared bind is a two-way street: on a
    # machine with / mounted shared, anything a maintainer script mounts under the
    # chroot's /dev or /proc appears on the BUILD MACHINE's, and stays there.  That is
    # how the r36 bootstrap left a stray devpts on the VM's own /dev/pts and broke sudo
    # for every later build.  Nothing in here mounts anything today; slave costs one
    # syscall and means it stays that way.
    for m in dev proc sys; do
        mountpoint -q "$ARMHF_CHROOT/$m" && continue
        sudo mount --bind "/$m" "$ARMHF_CHROOT/$m" || return 1
        sudo mount --make-rslave "$ARMHF_CHROOT/$m" || return 1
    done
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
        sudo tee "$ARMHF_CHROOT/etc/resolv.conf" >/dev/null
    printf 'exit 101\n' | sudo tee "$ARMHF_CHROOT/usr/sbin/policy-rc.d" >/dev/null
    sudo chmod 0755 "$ARMHF_CHROOT/usr/sbin/policy-rc.d"
    return 0
}

# chroot_install_deps <stamp-name> <package>...
#
# One stamp per set, so a re-run does not repeat the apt.  The stamp names are part of
# the chroot's state and not of this file's, which is why `qt' is spelled the way it
# already is on disk: change it and an existing chroot does twenty emulated minutes of
# apt again for packages it already has.  The unnamed stamp, .j36-deps, was the ES
# set's and nothing writes it any more.
chroot_install_deps() {
    local stamp="$ARMHF_CHROOT/.j36-deps${1:+-$1}" ; shift
    [[ -f "$stamp" ]] && return 0
    log "chroot: installing $# build dependencies into the armhf chroot"
    armhf_chroot_run "eatmydata apt-get -y update" || return 1
    armhf_chroot_run "DEBIAN_FRONTEND=noninteractive eatmydata apt-get -y \
        --no-install-recommends install $*" || return 1
    sudo touch "$stamp"
    return 0
}

armhf_chroot_teardown() {
    local m
    for m in dev/pts dev proc sys; do
        mountpoint -q "$ARMHF_CHROOT/$m" && sudo umount -l "$ARMHF_CHROOT/$m"
    done
    return 0
}

# ── mixdash: the dashboard, and what it does not need ─────────────────────────
#
# WHY THIS EXISTS.  Everything tried on this panel before it reached it through
# SDL's KMSDRM backend, so EGL, so GBM, so Mesa's lima on a Mali-450 -- five layers
# that each fail silently, and a black panel is what all five look like from the
# outside.  mixdash reaches the panel through none of them:
# Qt5 Widgets on the `linuxfb' platform plugin writes into /dev/fb0, and on this
# board /dev/fb0 is the framebuffer the LK already lit.  That is not a hope, it is
# the consequence of two decisions further up this file: FB_SIMPLE binds the device
# tree's simple-framebuffer at 0x82700000, and CONFIG_DRM_FBDEV_EMULATION=n means
# mediatek-drm never takes /dev/fb0 away from it.  The kernel console drawing on
# this panel already proved the path end to end.
#
# WHAT IS BUILT.  Every .cpp and .h in device/j36-ultra/tools/mixdash, against
# Debian's own qtbase5-dev in the armhf chroot above.  Nothing is cross-compiled, no
# Qt is built from source, and QT does not include opengl.  The staging loop below
# is a glob and mixdash.pro is the only list, so a new source file needs adding in
# one place and not in two.
#
# WHERE IT GOES, AND WHY NOT ONTO BOOT.  Qt's runtime closure for the linuxfb path
# is 14 MB of Qt plus 35 MB of ICU -- libQt5Core.so.5 carries libicui18n.so.76 and
# libicuuc.so.76 in its DT_NEEDED and there is no build option here that drops them.
# The BOOT partition is vfat, shared with an R36S's own boot files, and already
# carries a kernel, an initramfs, three module payloads and Mesa.  So the Qt payload
# and the dashboard go on the SECOND partition instead, under /opt/mixos, which is a
# directory neither ArkOS nor the R36S has ever had: nothing on the shared rootfs is
# overwritten, and an R36S booting the same card never looks in it.  That also buys
# real symlinks, so this payload needs no `links' file the way j36/gl does.
#
# ONE UNUSED DEPENDENCY WORTH KNOWING ABOUT.  Debian's libQt5Gui.so.5 links
# libGLESv2.so.2 whether or not anything asks for GL, so the loader must resolve that
# name before main() runs -- and on this shared rootfs /usr/lib's copy is a symlink
# to the RK3326's ARMv8-A libMali.so.  The payload therefore carries glvnd's own
# libGLESv2.so.2, which is a dispatch stub that dlopens a driver only when a context
# is made current, and mixdash never makes one.  The binary's RPATH puts /run/j36/gl
# first all the same, so anything launched FROM the dashboard that does want GL gets
# Mesa from the boot payload rather than this stub.
# MIXDASH_SRC and MIXDASH_SOURCE_ID are set far above, next to the initramfs, because
# the boot image is packed long before anything here is compiled and /init has to carry
# the id with it.
MIXDASH_BIN=""
QT_PAYLOAD=""

# No t64 package names here on purpose.  trixie's armhf Qt is libqt5core5t64,
# libqt5gui5t64 and so on, forkward's rename is suite-specific, and qtbase5-dev
# depends on whichever names the suite actually uses -- including libqt5gui5t64,
# which is the package that ships platforms/libqlinuxfb.so.  So the -dev package is
# named and the runtime set is left to apt.
MIXDASH_BUILD_DEPS=(build-essential pkg-config qtbase5-dev fonts-dejavu-core)

# The eight names that must come from the card's own rootfs and never from this
# chroot: the ELF interpreter, the C library's set, and the two GCC runtimes.  Every
# other library the closure names is staged, because "the rootfs surely has this one"
# is an assumption that costs a boot to disprove, and 40 MB of SD card to avoid.
# libutil.so.1 is in the list for the same reason libpthread.so.0 is: since glibc
# 2.34 it is an empty compatibility shim that belongs to the rootfs's own glibc,
# and mixdash names it only because the Terminal page links -lutil for forkpty(3).
QT_PAYLOAD_SKIP='ld-linux|libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libutil\.so\.1|libgcc_s\.so\.1|libstdc\+\+\.so\.6'

build_mixdash() {
    local src="$ARMHF_CHROOT/home/build/mixdash" out="$CACHE/mixdash"
    local stamp="$CACHE/mixdash.stamp" want f header needed lib dynsyms dangling

    [[ -d "$MIXDASH_SRC" ]] || { log "mixdash: $MIXDASH_SRC is missing"; return 1; }

    # A range-for over `QStringList() << a << b' iterates FREED MEMORY, and this is
    # the one grep that catches it before the board does.  operator<< returns a
    # reference to the temporary, so the loop's hidden `auto &&__range' is
    # initialised from a reference rather than from the temporary itself -- and
    # lifetime extension only applies to the direct case.  The list is destroyed at
    # the semicolon and begin() then dereferences a released QArrayData.
    #
    # It cost a boot: the dashboard died with `std::bad_alloc' in the phase that
    # builds the pages, because a QString built from the freed header asked for a
    # nonsense length.  g++ DOES warn -- "'d' is used uninitialized", pointed at
    # qlist.h and inlined from the loop -- but it is a -Wuninitialized buried in a
    # wall of Qt template context, in a build whose exit status was 0.
    #
    # The pattern is deliberately narrow: `: Identifier() <<'.  The two loops that
    # iterate `dir.entryList(QStringList() << pat, ...)' are correct -- entryList
    # returns BY VALUE, which the range-for does extend -- and must not trip this.
    dangling="$(grep -nE 'for[[:space:]]*\([^;]*:[[:space:]]*[A-Za-z_][A-Za-z0-9_:<>, ]*\(\)[[:space:]]*<<' \
                     "$MIXDASH_SRC"/*.cpp "$MIXDASH_SRC"/*.h 2>/dev/null || true)"
    # Comments are allowed to quote the bad form -- dashboard.cpp does, right where it
    # was fixed, because the next reader of that loop needs to see what not to write.
    dangling="$(grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' <<<"$dangling" || true)"
    if [[ -n "$dangling" ]]; then
        log "mixdash: a range-for iterates a temporary container built with <<, which"
        log "    is a use-after-free -- the container dies at the semicolon.  Assign it"
        log "    to a named local first.  Offending lines:"
        while IFS= read -r f; do log "      $f"; done <<<"$dangling"
        return 1
    fi

    want="$MIXDASH_SOURCE_ID"
    if [[ -x "$out" && "$(cat "$stamp" 2>/dev/null)" == "$want" ]]; then
        MIXDASH_BIN="$out"
        log "mixdash: reusing $out ($(stat -c %s "$out") bytes)"
        return 0
    fi

    ensure_armhf_chroot || { armhf_chroot_teardown; return 1; }
    chroot_install_deps qt "${MIXDASH_BUILD_DEPS[@]}" || { armhf_chroot_teardown; return 1; }

    # ccache, and on its OWN stamp rather than appended to MIXDASH_BUILD_DEPS: the qt
    # stamp is already on disk in every existing chroot, so a package added to that
    # list would never be installed in any of them -- see chroot_install_deps.
    #
    # This is the change that makes rebuilding the dashboard bearable.  The staging
    # step below wipes $src and copies the sources in fresh, so every file has a new
    # mtime and make rebuilds all twenty-one of them every single time, even when one
    # line of one file changed -- and each of those compiles is an emulated armhf g++
    # chewing through Qt's headers.  ccache keys on CONTENT, not on timestamps, so the
    # twenty files that did not change become cache hits and only the one that did is
    # actually compiled.  Non-fatal: no network, or an apt that will not resolve, costs
    # a slow build and not a failed one.
    chroot_install_deps ccache ccache || \
        log "mixdash: no ccache in the chroot, so this build compiles everything"

    sudo rm -rf "$src"
    sudo mkdir -p "$src"
    for f in "$MIXDASH_SRC"/*.cpp "$MIXDASH_SRC"/*.h "$MIXDASH_SRC"/mixdash.pro; do
        sudo cp "$f" "$src/" || { armhf_chroot_teardown; return 1; }
    done

    # Generated, not committed, and generated AFTER the copy loop so it cannot feed back
    # into the hash it is derived from.  main.cpp includes it through __has_include, so
    # the tree still builds by hand with a plain qmake && make and simply reports an
    # unknown build then.
    sudo tee "$src/buildid.h" >/dev/null <<BUILDIDH
/* Generated by build-in-vm.sh.  The first twelve characters of the sha256 of this
 * dashboard's own sources -- the same value /init writes into MIXDASH_EXPECT, so a
 * boot image and an /opt/mixos payload from different builds say so out loud. */
#define MIXDASH_BUILD_ID "${want:0:12}"
BUILDIDH

    # RPATH and not RUNPATH: --disable-new-dtags makes ld emit DT_RPATH, which glibc
    # searches for the whole dependency chain rather than only for this object's
    # direct dependencies -- and the chain here is real, because libQt5Core reaches
    # ICU and the linuxfb plugin reaches libinput.  It is what makes
    #     /opt/mixos/bin/mixdash --probe
    # work from a plain shell with no environment set, which is how this will be run
    # the first time.  /run/j36/gl comes first so the Mesa payload wins for the GL
    # names; /opt/mixos/qt/lib is where everything Qt lives.
    #
    # CCACHE_DIR is under /home/build and the wipe above is of /home/build/mixdash,
    # so the cache outlives the sources it was filled from and only a chroot rebuild
    # empties it.  /usr/lib/ccache goes on PATH for the make and not for the qmake:
    # qmake bakes the plain names `g++' and `cc' into the Makefile, and it is make's
    # PATH that decides which g++ those resolve to.
    log "mixdash: building the dashboard for armhf (emulated), -j$JOBS"
    armhf_chroot_run "cd /home/build/mixdash && \
        q=\$(command -v qmake || true); \
        [ -n \"\$q\" ] || q=\$(ls /usr/lib/*/qt5/bin/qmake 2>/dev/null | head -1); \
        [ -n \"\$q\" ] || { echo 'no qmake in this chroot'; exit 1; }; \
        QT_SELECT=qt5 \"\$q\" \
            QMAKE_LFLAGS+='-Wl,--disable-new-dtags' \
            QMAKE_LFLAGS+='-Wl,-rpath,/run/j36/gl' \
            QMAKE_LFLAGS+='-Wl,-rpath,/opt/mixos/qt/lib' && \
        export CCACHE_DIR=/home/build/.ccache CCACHE_MAXSIZE=2G && \
        if [ -d /usr/lib/ccache ]; then export PATH=/usr/lib/ccache:\$PATH; fi && \
        make -j$JOBS && strip mixdash && \
        { command -v ccache >/dev/null && ccache -s | \
            sed -n 's/^\(cache hit rate\|Hits\|Misses\)/mixdash ccache: &/p'; true; }" \
        || { armhf_chroot_teardown; return 1; }

    [[ -f "$src/mixdash" ]] || { log "mixdash: make left no binary"; armhf_chroot_teardown; return 1; }

    mkdir -p "$CACHE"
    sudo cp "$src/mixdash" "$out" || { armhf_chroot_teardown; return 1; }
    sudo chown "$(id -u):$(id -g)" "$out"
    chmod 0755 "$out"

    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "mixdash: not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "mixdash: not an ARM ELF"; return 1; }
    grep -q 'Library rpath' <<<"$header" || {
        log "mixdash: the binary carries no RPATH, so it would only run with"
        log "    LD_LIBRARY_PATH set -- and the first thing anybody does is run it by hand"
        return 1
    }
    needed="$(sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' <<<"$header" | tr '\n' ' ')"
    for lib in $needed; do
        case "$lib" in
            libQt5Widgets.so.5|libQt5Gui.so.5|libQt5Core.so.5) ;;
            libGL.so*|libEGL*|libMali*|libmali*)
                log "mixdash: the binary links $lib.  QT does not include opengl in"
                log "    mixdash.pro, and every one of those names on this rootfs is a"
                log "    symlink to an ARMv8-A libMali.so.  Refusing to stage it."
                return 1
                ;;
        esac
    done

    # -rdynamic, checked rather than assumed.  It is one line in a .pro file that a
    # later edit can drop with nothing failing to build, and what it buys is only ever
    # visible on the board: without it alloc.cpp's out-of-memory backtrace prints bare
    # addresses for mixdash's own frames, and the strip above has already removed the
    # .symtab that could have resolved them afterwards.  A dynamic symbol table with
    # nothing but imports in it is exactly what that looks like.
    #
    # Into a variable and then a herestring, like the header checks above, and NOT
    # `readelf ... | grep -q'.  This script runs under `set -o pipefail'; grep -q exits
    # the instant it matches, readelf is still writing, and it dies of SIGPIPE -- so the
    # pipeline reports 141 precisely BECAUSE the symbol was found.  That inversion cost
    # a build: it failed here on a binary carrying ten Trace symbols, blanked
    # MIXDASH_BIN, and left the tarball -- and so the card -- on the previous dashboard.
    dynsyms="$(readelf --dyn-syms -W "$out" 2>/dev/null || true)"
    if ! grep -q 'Trace' <<<"$dynsyms"; then
        log "mixdash: the binary exports none of its own symbols, so -rdynamic was lost"
        log "    from mixdash.pro.  An out-of-memory backtrace would then be a column of"
        log "    hex with no names, on a stripped binary nothing can symbolize later."
        return 1
    fi

    printf '%s\n' "$want" >"$stamp"
    MIXDASH_BIN="$out"
    log "mixdash: $(stat -c %s "$out") bytes, stripped ARM"
    log "mixdash: needs $needed"
    return 0
}

# The runtime closure, measured rather than listed.  ldd inside the chroot resolves
# exactly what the loader will resolve on the board -- the same suite, the same
# architecture -- for the binary AND for libqlinuxfb.so, which Qt dlopens and whose
# own dependencies (libinput, libmtdev, libxkbcommon, libdrm, glib, fontconfig,
# freetype) are not in the binary's DT_NEEDED at all.  Miss the plugin out of this
# walk and the dashboard starts, finds no platform plugin, and aborts with the
# "This application failed to start because no Qt platform plugin could be
# initialized" message that says nothing about which library was missing.
# A fontconfig of two files, written here rather than inline in collect_qt_payload so
# that a REUSED payload gets it too.  That cache is keyed on the libraries in it, which
# a new configuration file does not change, so a resumed build would otherwise ship a
# payload whose fonts.conf the unit file expects and cannot find -- and /init's test for
# it would silently fall back to scanning the rootfs.
#
# QT_QPA_FONTDIR does not do this job: only QBasicFontDatabase reads that variable, and
# Debian's QtGui links libfontconfig, so what Qt actually uses is QFontconfigDatabase --
# which reads /etc/fonts/fonts.conf, scans every font directory in the rootfs and builds
# a cache if there is not one already.  On this board that scan is the first substantial
# read the dashboard does, off a card that had just spent 102 seconds handing over a
# kernel, and there is no way to watch it happen: the console is not being drawn on by
# then.  Two faces in one directory instead of a system scan, and a cache under /run so
# a rootfs with none is not a first-run penalty every boot.
#
# This is also what makes the dashboard look the same on a rootfs with no fonts
# installed at all, which is the case the payload's own copies exist for.
write_fontconfig() {
    local out="$1"
    mkdir -p "$out/fonts" || return 1
    cat > "$out/fonts/fonts.conf" <<'FCCONF'
<?xml version="1.0"?>
<!-- Written by device/j36-ultra/build-in-vm.sh.  See write_fontconfig for why. -->
<fontconfig>
  <dir>/opt/mixos/qt/fonts</dir>
  <cachedir>/run/mixdash/fontconfig</cachedir>
  <!-- The three generic families Qt asks for by name, all answered by the one
       family that is here.  Without these a request for "sans-serif" has no match
       and Qt falls back to its own last resort. -->
  <match target="pattern">
    <test qual="any" name="family"><string>sans-serif</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>
  </match>
  <match target="pattern">
    <test qual="any" name="family"><string>serif</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>
  </match>
  <!-- monospace answers with the mono face and NOT with DejaVu Sans.  The
       Terminal page measures the font it gets and refuses to draw a character
       grid with a proportional one, so a monospace request that lands on DejaVu
       Sans is the difference between a terminal and a column of overlapping
       glyphs. -->
  <match target="pattern">
    <test qual="any" name="family"><string>monospace</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans Mono</string></edit>
  </match>
</fontconfig>
FCCONF
}

# Bumped whenever this function stages something it did not stage before.  The
# payload cache is keyed on the chroot's Qt rather than on anything in this tree,
# so without a recipe number a resumed build happily reuses a payload that predates
# the imageformats plugins and the mono font -- and the failure that produces is
# "pictures do not open" on a card whose build log says the payload was fine.
QT_PAYLOAD_RECIPE=2

collect_qt_payload() {
    local out="$CACHE/qt-payload" plugin list p base real n=0 ttf
    local imgdir imgplugins img walk
    QT_PAYLOAD=""

    [[ -x "$MIXDASH_BIN" ]] || { log "qt: no dashboard binary to walk"; return 1; }

    # Keyed on nothing but its own completeness, because what shapes this payload is
    # the chroot's Qt rather than anything in this tree.  Delete $CACHE/qt-payload to
    # pick up a Qt that apt has upgraded since.
    if [[ -e "$out/lib/libQt5Core.so.5" && -e "$out/plugins/platforms/libqlinuxfb.so"
          && "$(cat "$out/.recipe" 2>/dev/null)" == "$QT_PAYLOAD_RECIPE" ]]; then
        write_fontconfig "$out" || return 1
        QT_PAYLOAD="$out"
        log "qt: reusing $out ($(du -sh "$out" | awk '{print $1}'))"
        return 0
    fi

    ensure_armhf_chroot || { armhf_chroot_teardown; return 1; }
    # Put the binary where ldd can see it whichever path got us here -- build_mixdash
    # may have reused a cached one and never entered the chroot at all.
    sudo mkdir -p "$ARMHF_CHROOT/home/build/mixdash"
    sudo cp "$MIXDASH_BIN" "$ARMHF_CHROOT/home/build/mixdash/mixdash" || return 1

    plugin="$(armhf_chroot_run 'ls /usr/lib/*/qt5/plugins/platforms/libqlinuxfb.so 2>/dev/null | head -1')"
    [[ -n "$plugin" ]] || {
        log "qt: this chroot's Qt has no platforms/libqlinuxfb.so, which is the whole"
        log "    reason Qt was chosen -- libqt5gui5t64 is the package that ships it"
        return 1
    }

    # The image format plugins.  QImageReader has PNG, BMP, PPM, XBM and XPM built
    # into QtGui and NOTHING else: JPEG and GIF are dlopened plugins, and without
    # them the Media page opens a photograph, gets a null QImage back and can only
    # say that it does not know the format.  Every camera and every phone on earth
    # writes JPEG, so this is the difference between a picture viewer and a PNG
    # viewer.  They go through the ldd walk below too, because libqjpeg.so pulls in
    # libjpeg, which nothing else here names.
    imgdir="$(armhf_chroot_run 'ls -d /usr/lib/*/qt5/plugins/imageformats 2>/dev/null | head -1')"
    imgplugins=""
    if [[ -n "$imgdir" ]]; then
        for img in libqjpeg.so libqgif.so; do
            if [[ -f "$ARMHF_CHROOT$imgdir/$img" ]]; then
                imgplugins="$imgplugins $imgdir/$img"
            else
                log "qt: this chroot has no $img -- that format will not open on the board"
            fi
        done
    else
        log "qt: this chroot's Qt has no imageformats directory; JPEG will not open"
    fi

    walk="$plugin$imgplugins"
    list="$(armhf_chroot_run "ldd /home/build/mixdash/mixdash $walk 2>/dev/null | \
        sed -n 's|.*=> \\(/[^ ]*\\).*|\\1|p' | sort -u")" || return 1
    [[ -n "$list" ]] || { log "qt: ldd resolved nothing in the chroot"; return 1; }

    rm -rf "$out"
    mkdir -p "$out/lib" "$out/plugins/platforms" "$out/plugins/imageformats" "$out/fonts"

    while read -r p; do
        [[ -n "$p" ]] || continue
        base="${p##*/}"
        [[ "$base" =~ ^($QT_PAYLOAD_SKIP) ]] && continue
        real="$(sudo readlink -f "$ARMHF_CHROOT$p" 2>/dev/null)" || continue
        [[ -f "$real" ]] || continue
        sudo cp "$real" "$out/lib/${real##*/}" || return 1
        # The SONAME the loader asks for, when the file it lands on is named after a
        # version.  ext2 holds symlinks, which is half of why this payload is on the
        # OS partition and not on the vfat one.
        [[ "${real##*/}" == "$base" ]] || ln -sf "${real##*/}" "$out/lib/$base"
        n=$((n + 1))
    done <<<"$list"

    sudo cp "$ARMHF_CHROOT$plugin" "$out/plugins/platforms/libqlinuxfb.so" || return 1
    for img in $imgplugins; do
        sudo cp "$ARMHF_CHROOT$img" "$out/plugins/imageformats/${img##*/}" || return 1
    done
    sudo chown -R "$(id -u):$(id -g)" "$out"
    chmod -R u+rw "$out"

    # Three faces of one family, and no more: 640x480 shows about 24 rows of text and
    # a font family is a megabyte.  DejaVu because Debian's fontconfig defaults to it
    # and because main.cpp prefers the payload's copy, so the dashboard looks the
    # same on a rootfs with no fonts installed at all.  The Mono face is the
    # Terminal's: it checks that the font it was given is really fixed-pitch before
    # it draws a character grid in it.
    for ttf in DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf; do
        if [[ -f "$ARMHF_CHROOT/usr/share/fonts/truetype/dejavu/$ttf" ]]; then
            cp "$ARMHF_CHROOT/usr/share/fonts/truetype/dejavu/$ttf" "$out/fonts/" || return 1
        fi
    done

    write_fontconfig "$out" || return 1

    # qt.conf next to the binary, because Qt reads it from the executable's own
    # directory before it looks at any environment.  It is what makes the dashboard
    # find its plugins when it is run by hand from a shell, with no unit file and no
    # variables set.
    mkdir -p "$out/bin"
    cat > "$out/bin/qt.conf" <<'QTCONF'
# Read by Qt from the directory the executable is in, before QT_PLUGIN_PATH and
# before any compiled-in path.  Without it Qt looks in /usr/lib/<triplet>/qt5/
# plugins, which on this card is a directory that does not exist.
[Paths]
Plugins=/opt/mixos/qt/plugins
QTCONF

    # The three names the loader must find before main(), asserted here rather than
    # discovered on the board: a Qt payload missing one of them fails with a message
    # that names the file and nothing else.
    for base in libQt5Widgets.so.5 libQt5Gui.so.5 libQt5Core.so.5; do
        [[ -e "$out/lib/$base" ]] || { log "qt: the payload has no $base"; return 1; }
    done
    ls "$out"/lib/libicuuc.so.* >/dev/null 2>&1 || {
        log "qt: the payload has no ICU, and libQt5Core.so.5 names libicuuc in its"
        log "    DT_NEEDED -- the closure walk missed it, which means ldd did"
        return 1
    }

    printf '%s\n' "$QT_PAYLOAD_RECIPE" >"$out/.recipe"

    QT_PAYLOAD="$out"
    log "qt: staged $n libraries, libqlinuxfb.so, $(ls -1 "$out/plugins/imageformats" 2>/dev/null | wc -l) image plugins and $(ls -1 "$out/fonts" | wc -l) fonts ($(du -sh "$out" | awk '{print $1}'))"
    return 0
}

# J36_GL is Mesa and the probe, and it is worth keeping even now that the dashboard
# needs no GL: eglprobe is still the only thing here that says whether a frame reaches
# the glass, and anything launched from the dashboard that does want GL resolves it
# out of this payload.  J36_ES was the name this word had; it is gone with the rest of
# that spelling, and the default is on either way.
if [[ "${J36_GL:-1}" == 1 ]]; then
    set +e
    collect_gl_payload
    gl_rc=$?
    set -e
    if (( gl_rc != 0 )); then
        GL_PAYLOAD=""
        log "gl: the GL front end was not staged, see the error above -- the kernel payload is unaffected"
    fi

    set +e
    build_eglprobe
    probe_rc=$?
    set -e
    if (( probe_rc != 0 )); then
        EGLPROBE_BIN=""
        log "gl: eglprobe was not built, see the error above -- j36.gl=debug will just be quieter"
    fi
else
    log "gl: J36_GL=0, skipping the GL front end"
fi

# The mirror is built outside the J36_GL block on purpose.  It touches no GL at all --
# it is memcpy, one modeset and one dirty ioctl -- so a build with the Mesa front end
# switched off should still come out able to drive a dock.
set +e
build_mixmirror
mirror_rc=$?
set -e
if (( mirror_rc != 0 )); then
    MIXMIRROR_BIN=""
    log "mirror: j36-mixmirror was not built, see the error above -- a USB-HDMI adapter"
    log "    will still enumerate and still get a /dev/dri node, it just will not show"
    log "    the dashboard.  Nothing else in the image depends on it."
fi

# The dashboard, and it is on by default: it is what takes over the panel now.
if [[ "${J36_DASH:-1}" == 1 ]]; then
    set +e
    build_mixdash
    dash_rc=$?
    if (( dash_rc == 0 )); then
        collect_qt_payload
        dash_rc=$?
    fi
    armhf_chroot_teardown
    set -e
    if (( dash_rc != 0 )); then
        MIXDASH_BIN=""
        QT_PAYLOAD=""
        log "mixdash: the dashboard was not built, see the error above."
        log "    THIS RUN'S ARTIFACTS CARRY NO DASHBOARD.  sd-root.tar.gz is written"
        log "    without opt/mixos/bin/mixdash, so a card updated from it keeps whatever"
        log "    dashboard it already had -- the build looks like it succeeded and the"
        log "    board comes up on the PREVIOUS binary.  /init will say so on the console"
        log "    (mixdash-missing.service names the partitions it searched), and if an"
        log "    older mixdash is still on the card it prints HALF THIS CARD IS STALE,"
        log "    because /etc/j36-build already names this run's build id."
    fi
else
    log "mixdash: J36_DASH=0, skipping the dashboard"
fi

# J36_ES_BUILD=1 built the GLES 2.0 EmulationStation here, and it is gone rather than
# defaulted off.  It had already been off by default for a while, and not because the
# renderer was wrong: the last measurement was a context that was created, a frame
# that was submitted and a panel that stayed black, with nothing in EGL, in SDL or in
# Mesa saying why.  Five layers deep is a bad place to be stuck, and the dashboard
# reaches the same panel through none of them.  Something still has to prove a frame
# on lima the day mtk_drm lands one, and eglprobe is that something now -- it is 400
# lines instead of a fork of a fork of a front end, and mixdash-probe.service already
# runs it once per boot.
#
# Set by habit, by a stale environment, or by a script that has not been updated: say
# so rather than silently doing nothing.
if [[ -n "${J36_ES_BUILD:-}" ]]; then
    log "es: J36_ES_BUILD=${J36_ES_BUILD} is ignored; EmulationStation is not part of this build"
fi

# ── The SD BOOT payload: the launcher, and nothing else ───────────────────────
#
# Copy this tree onto the FAT partition labelled BOOT and the MVII LK boots the
# card instead of the eMMC.  /mvii/boot.conf is written because an R36S card
# already carries a boot.ini, and that boot.ini names the RK3326's arm64 `Image`
# and an rk3326 device tree.  The LK parses boot.ini first and boot.conf second
# precisely so this file gets the last word; without it the LK would load the
# arm64 kernel, refuse it at the magic check, and fall back to the eMMC.
#
# Load addresses are deliberately absent.  They are the LK's business -- it knows
# this SoC's DRAM map and the address of the framebuffer the DTB hands to
# simple-framebuffer -- and boot.conf can only get them wrong.
#
# WHAT BELONGS HERE, AND WHY THAT IS NOW A SHORT LIST.  The MVII LK reads FAT32 and
# nothing else, so BOOT exists because the loader has to be able to read it -- which
# makes it the launcher and only the launcher: zImage, the device tree, initrd.img and
# boot.conf, the four files something other than Linux has to open.  Everything the
# LK never touches went to the OS partition, which is ext2 and therefore holds symlinks
# and execute bits and is not a 100 MB partition an R36S card shares with its own boot
# files.  vfat's own limits stop being anything to work around at that point: the GL
# payload's `links' file existed because vfat cannot hold a symlink, and it stays only
# because a card written by an older build still has its payload on BOOT.
log "Staging the SD card BOOT payload"
SDBOOT="$ARTIFACTS/sd-boot"
rm -rf "$SDBOOT"
mkdir -p "$SDBOOT/mvii"

# The OS-partition tree, declared here because the j36/ payload below now goes into it
# and the staging has to be able to write to it before the /opt/mixos section further
# down fills in the dashboard.  One rm -rf, at the top, for the same reason.
SDROOT="$ARTIFACTS/sd-root"
rm -rf "$SDROOT" "$ARTIFACTS/sd-root.tar.gz"

# WHERE THE j36/ PAYLOAD GOES.  Modules, mfgpower, the Mesa front end and the probe are
# read by /init, never by the LK, so BOOT is the wrong partition for them: it is the
# small vfat one, and vfat costs the payload its symlinks and its modes.  They live in
# the OS partition's /opt/mixos/j36 now, which /init reaches through the rootfs it has
# already mounted -- one mount fewer, and the same tarball that carries the dashboard.
#
# J36_PAYLOAD_ON=boot puts them back on BOOT.  That is not symmetry for its own sake:
# /init still looks there second, so a card written by an earlier build keeps working,
# and this switch is how that path stays buildable and testable instead of becoming
# code nothing exercises.
PAYLOAD_ON="${J36_PAYLOAD_ON:-root}"
case "$PAYLOAD_ON" in
    root) PAYDIR="$SDROOT/opt/mixos/j36"; PAYREL="sd-root/opt/mixos/j36" ;;
    boot) PAYDIR="$SDBOOT/j36";           PAYREL="sd-boot/j36" ;;
    *)    die "J36_PAYLOAD_ON must be root or boot, not $PAYLOAD_ON" ;;
esac
log "payload: j36/ goes to $PAYREL (J36_PAYLOAD_ON=$PAYLOAD_ON)"
cp "$ZIMAGE" "$SDBOOT/zImage"
cp "$DTB_OUT/mt6592-j36-ultra.dtb" "$SDBOOT/"
cp "$ARTIFACTS/initramfs-j36-ultra.cpio.xz" "$SDBOOT/initrd.img"

# $PAYDIR from here down, which is opt/mixos/j36 in the OS partition unless
# J36_PAYLOAD_ON=boot.  Everything in it is read by /init, never by the LK, so nothing
# here goes through a load window and nothing here has a size limit worth worrying
# about.  Delete a directory under it on the card and the boot is exactly what it was
# before -- /init says so and carries on.
#
# Doom and its IWAD are in the same partition, under opt/mixos/bin and
# opt/mixos/share; see the /opt/mixos section below.  They were on BOOT once, and 26 MiB
# of game on the launcher partition is what started the move that this whole layout
# finished.

# Remove j36/mfgpower or j36/modules and j36.lima=1 finds nothing, says so, and the
# boot continues.  load.order is written from the walk above, one module per line in
# the order insmod needs them.
if [[ -n "$MFGPOWER_BIN" && ${#LIMA_MODULE_ORDER[@]} -gt 0 ]]; then
    mkdir -p "$PAYDIR/modules"
    cp "$MFGPOWER_BIN" "$PAYDIR/mfgpower"
    chmod 0755 "$PAYDIR/mfgpower"
    : > "$PAYDIR/modules/load.order"
    for i in "${!LIMA_MODULE_ORDER[@]}"; do
        cp "${LIMA_MODULE_PATHS[$i]}" "$PAYDIR/modules/${LIMA_MODULE_ORDER[$i]}"
        printf '%s\n' "${LIMA_MODULE_ORDER[$i]}" >> "$PAYDIR/modules/load.order"
    done
    log "lima: staged ${#LIMA_MODULE_ORDER[@]} modules and mfgpower into $PAYREL/"
fi

# j36/mtkdrm/ is the display set, in its own directory and with its own load.order
# rather than merged into j36/modules/, because the two payloads answer to different
# command-line words and fail independently.  Deleting this one directory takes the
# whole mtk_drm experiment off the card and leaves the lima payload exactly as it
# was; j36.mtkdrm=1 then finds nothing, says so, and the boot carries on.
if (( ${#MTKDRM_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$PAYDIR/mtkdrm"
    : > "$PAYDIR/mtkdrm/load.order"
    for i in "${!MTKDRM_MODULE_ORDER[@]}"; do
        cp "${MTKDRM_MODULE_PATHS[$i]}" "$PAYDIR/mtkdrm/${MTKDRM_MODULE_ORDER[$i]}"
        printf '%s\n' "${MTKDRM_MODULE_ORDER[$i]}" >> "$PAYDIR/mtkdrm/load.order"
    done
    log "mtkdrm: staged ${#MTKDRM_MODULE_ORDER[@]} modules into $PAYREL/mtkdrm/"
fi

# j36/audio/ is the ALSA core and the AFE adapter, on the same terms: its own
# directory, its own load.order, its own command-line word, and deleting it takes
# the sound experiment off the card without touching anything else.  That matters
# more here than for the other payloads, because this is the one whose failure
# mode is the board switching off: j36.audio=speaker powers a class-D amp on VBAT,
# which is the system node, and a board with no cell fitted cannot hold it up.
# Removing this directory is the recovery, from any machine that reads SD cards.
if (( ${#AUDIO_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$PAYDIR/audio"
    : > "$PAYDIR/audio/load.order"
    for i in "${!AUDIO_MODULE_ORDER[@]}"; do
        cp "${AUDIO_MODULE_PATHS[$i]}" "$PAYDIR/audio/${AUDIO_MODULE_ORDER[$i]}"
        printf '%s\n' "${AUDIO_MODULE_ORDER[$i]}" >> "$PAYDIR/audio/load.order"
    done
    log "audio: staged ${#AUDIO_MODULE_ORDER[@]} modules into $PAYREL/audio/"
fi

# j36/usb/ is the host stack: the PHY, MUSB and its glue, the HID pair, udl, and
# the mass-storage set that turns a plugged-in disk into /dev/sda and mounts it.
# Its own directory and its own load.order on the same terms as the other three,
# and the removal contract matters here as much as it does for audio, for the
# same class of reason: this is the payload whose failure mode is a bus stall.
# An APB access to a clock-gated MediaTek peripheral hangs until the watchdog
# fires, and while the PHY driver's whole job is to make sure that cannot happen,
# it has not yet been proved on this board.  Delete this directory from any
# machine that reads SD cards and j36.usb=1 finds nothing, says so, and the boot
# carries on.
#
# The overlap with j36/mtkdrm/ is deliberate and not a mistake to clean up: udl
# and mtk_drm both need drm_kms_helper, so both directories carry a copy of it.
# (udl's drm_shmem_helper is not shared -- it comes in through udl's own
# dependency walk and mtk_drm never asks for it.)  A second copy of one helper is
# cheaper than a load order that spans payloads, and run_usb skips it when the
# mtkdrm payload has already loaded it.
if (( ${#USB_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$PAYDIR/usb"
    : > "$PAYDIR/usb/load.order"
    for i in "${!USB_MODULE_ORDER[@]}"; do
        cp "${USB_MODULE_PATHS[$i]}" "$PAYDIR/usb/${USB_MODULE_ORDER[$i]}"
        printf '%s\n' "${USB_MODULE_ORDER[$i]}" >> "$PAYDIR/usb/load.order"
    done
    log "usb: staged ${#USB_MODULE_ORDER[@]} modules into $PAYREL/usb/"
fi

# j36/power/ is the PMIC and the backlight, and it is the payload with the largest
# consequence per kilobyte on the card: 40 KB of module is the difference between
# a dashboard that draws a battery and one that draws nothing, and between a
# `poweroff' that halts the CPU with the rail still up and one that actually
# switches the board off.  The second module adds /sys/class/backlight, which is
# the difference between a brightness slider and a row of text explaining why
# there is not one.
#
# Same removal contract as the rest, and here it is a real recovery rather than a
# formality.  This is the one payload that programs a charger, and a charger is
# the one peripheral that can be wrong in a way you cannot see: a bad current
# limit is a warm battery, not a log line.  Delete this directory, or drop the
# j36.power word, and nothing in Linux touches CHR_CON again -- the LK's own
# settings stand, which is how every boot before this driver existed ran.
# j36.power=nocharge is the middle setting: the gauge and the power-off path stay,
# and the charger bank is left exactly as found.
if (( ${#POWER_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$PAYDIR/power"
    : > "$PAYDIR/power/load.order"
    for i in "${!POWER_MODULE_ORDER[@]}"; do
        cp "${POWER_MODULE_PATHS[$i]}" "$PAYDIR/power/${POWER_MODULE_ORDER[$i]}"
        printf '%s\n' "${POWER_MODULE_ORDER[$i]}" >> "$PAYDIR/power/load.order"
    done
    log "power: staged ${#POWER_MODULE_ORDER[@]} modules into $PAYREL/power/"
fi

# j36/wifi/ is the connectivity module set and, unlike every other payload here,
# the firmware it needs travels with it.
#
# The blobs are MediaTek's, off this device's own stock system image, and they go
# under j36/wifi/firmware/ rather than into the rootfs's /lib/firmware for the
# invariant this whole card is built on: one Debian rootfs serves two machines and
# nothing writes to it.  /init points the kernel's firmware_class path at this
# directory before it insmods, which also solves a problem a /lib/firmware install
# would not have solved anyway -- the module is loaded from the initramfs, before
# switch_root, and the loader searches whatever is mounted at the time.
#
# The removal contract is therefore the cleanest of the five: delete j36/wifi/ and
# both the driver and its firmware are gone in one step, with no orphaned blobs
# left behind in a system directory.  That matters more here than for the others
# because this payload brings up a power domain the AP shares a bus with.
if (( ${#WIFI_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$PAYDIR/wifi"
    : > "$PAYDIR/wifi/load.order"
    for i in "${!WIFI_MODULE_ORDER[@]}"; do
        cp "${WIFI_MODULE_PATHS[$i]}" "$PAYDIR/wifi/${WIFI_MODULE_ORDER[$i]}"
        printf '%s\n' "${WIFI_MODULE_ORDER[$i]}" >> "$PAYDIR/wifi/load.order"
    done
    log "wifi: staged ${#WIFI_MODULE_ORDER[@]} modules into $PAYREL/wifi/"

    # The names under mediatek/mt6592/ are what request_firmware() asks for, so
    # the tree is copied whole rather than the files flattened.  A missing
    # blob is a warning and not a build failure: the modules still stage, the
    # bring-up still runs, and it reports rom-patch-missing at the exact stage it
    # needed them -- which is a better answer than a build that refused.
    #
    # EVERY FILE, NOT *.bin.  This used to glob for .bin and there were only ever
    # two files, both ending in it, so nothing showed.  The WLAN firmware itself is
    # named WIFI_RAM_CODE_SOC with no extension at all -- that is the name the
    # driver asks the firmware loader for, because it is the name in the device's
    # own /system/etc/firmware -- and a glob that cannot see it stages a card whose
    # radio has its ROM patches and not the image they patch.
    WIFI_FW_SRC="$ROOT/device/j36-ultra/firmware"
    if [[ -d "$WIFI_FW_SRC/mediatek/mt6592" ]]; then
        mkdir -p "$PAYDIR/wifi/firmware/mediatek"
        cp -a "$WIFI_FW_SRC/mediatek/mt6592" \
              "$PAYDIR/wifi/firmware/mediatek/"
        log "wifi: staged $(ls -1 "$PAYDIR/wifi/firmware/mediatek/mt6592" | wc -l) firmware blobs into $PAYREL/wifi/firmware/"
    else
        log "wifi: $WIFI_FW_SRC/mediatek/mt6592 is missing -- the ROM patches are not on the card and the radio will stop at rom-patch-missing"
    fi
fi

# j36/gl/ is the GL front end plus the links file that stands in for symlinks.  On the
# OS partition it no longer has to -- ext2 holds symlinks -- and it is kept anyway
# because /init reads the same file whichever partition the payload came off, and one
# code path that works in both places beats two that each work in one.  Same removal
# contract as the other three: delete the directory and j36.gl=1 finds nothing, says
# so, and the boot carries on against the rootfs's own libraries, which is to say
# against the RK3326 Mali blob -- the behaviour this payload replaces.
if [[ -n "$GL_PAYLOAD" ]]; then
    mkdir -p "$PAYDIR/gl"
    cp "$GL_PAYLOAD"/*.so* "$PAYDIR/gl/"
    cp "$GL_PAYLOAD/links" "$PAYDIR/gl/links"
    log "gl: staged $(ls -1 "$PAYDIR/gl" | wc -l) files into $PAYREL/gl/"
fi

# The probe sits beside the payload rather than inside j36/gl/, because that
# directory is copied wholesale into the loader's search path and a binary is not
# a library. mixdash-probe.service runs -f once per boot and j36.gl=debug adds the
# node probes; j36.gl=1 on its own never touches it. Deleting it is the same
# contract as the rest: the boot loses the colour bars and this report, and nothing
# else changes.
if [[ -n "$EGLPROBE_BIN" ]]; then
    mkdir -p "$PAYDIR"
    cp "$EGLPROBE_BIN" "$PAYDIR/eglprobe"
    chmod 0755 "$PAYDIR/eglprobe"
    log "gl: staged $PAYREL/eglprobe ($(stat -c %s "$PAYDIR/eglprobe") bytes)"
fi

# rdinit=/init stays even though root= is now present, and the two do not
# conflict: rdinit means the kernel never mounts a root filesystem itself, so a
# root= it could not honour can no longer panic it.  /init does the mounting, and
# treats root= as a hint it verifies before switching -- delete the root= below
# and the card boots to the initramfs shell exactly as it did before.
#
# The prose that used to explain each bootargs word lives in README.txt now.  The
# LK reads boot.conf into a fixed 2 KiB buffer, COMMENTS INCLUDED, and this file
# has been over that twice: once rewriting the /opt/mixos paragraph, to 2109
# bytes, and once adding a single line about --no-splash, to 2059.  Both were
# comments.  The assertion below turns either into a failed build rather than a
# silently truncated bootargs line, and it prints the size so the next one says
# by how much.  A comment here costs the same as a boot argument -- keep them
# terse, and put the explanation in README.txt, which nothing parses.
cat > "$SDBOOT/mvii/boot.conf" <<'CONF'
# MVII LK SD hand-off, J36 Ultra (MT6592, ARMv7).
#
# Read after the card's own boot.ini, so these override it: an R36S boot.ini
# names the RK3326 arm64 kernel, which this SoC cannot execute.  Keep it short
# -- a fixed 2 KiB buffer.  ../README.txt explains every word below.
kernel=zImage
dtb=mt6592-j36-ultra.dtb
initrd=initrd.img

# root= is a hint /init verifies, not an order to the kernel.  console=tty0 comes
# last so /dev/console is the panel.  Both masked units are RK3326-only.
#
# Every j36 word is removable on its own: delete one, or the matching directory
# under /opt/mixos/j36 on the OS partition, and the boot carries straight on.
# lima gives a render node, mtkdrm a display node, gl puts Mesa ahead of the
# RK3326 blob, dash runs the MixOS dashboard, audio a sound card, usb the one
# MUSB port, splash the MixOS picture with the boot stage on it, wifi brings the
# connectivity MCU up and registers wlan0 -- and it implies j36.power, because
# the radio's rails come off the PMIC.
#
# Only the four files the LK reads are on BOOT; the rest is in sd-root.tar.gz,
# unpacked as /opt/mixos on the ext2 OS partition.
#
# j36.audio=speaker powers the class-D amp off VBAT, the system node: if the board
# cuts out in playback, `amixer -c0 set "Speaker Amp" off' -- no reboot needed.
# j36.usb=1 sources 5 V on the OTG port off that same rail: j36.usb=novbus with no
# cell.  It is not the connector that charges -- that is the DC inlet beside it.
# j36.gl=debug adds Mesa's EGL trace; a diagnostic, not a default.
# j36.splash=0 with loglevel=7 boots to text, which is the pair
# ./build-j36-ultra.sh --no-splash writes here.
# Each boot writes mixos-log.txt at the top of this partition; j36.log=0 stops it.
bootargs=console=ttyS0,115200n8 console=tty0 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init root=/dev/mmcblk0p2 rw rootwait loglevel=4 vt.global_cursor_default=0 systemd.mask=firstboot.service j36.lima=1 j36.mtkdrm=1 j36.gl=1 j36.dash=1 j36.audio=speaker j36.usb=1 j36.power=1 j36.wifi=1 j36.splash=1
CONF

# ── --no-splash, applied to the line rather than written into it ──────────────
#
# J36_SPLASH=0 is `./build-j36-ultra.sh --no-splash': boot this card to the text
# console, with the kernel talking, so that whatever the picture was covering is
# on the screen instead.  It is a diagnostic switch and it is a pair of changes,
# not one -- turning the splash off and leaving loglevel=4 gives a blank panel
# with the interesting messages still suppressed, which is a worse view than the
# splash was.
#
# WHY A sed AND NOT AN INTERPOLATION.  The heredoc above is quoted, and quoted is
# what keeps every $ and ` in a 2 KiB file that goes to a bootloader from being
# read by bash on the way past.  Unquoting it to substitute two words would put
# the whole of boot.conf at the mercy of shell expansion for the rest of its life.
# Both substitutions are the same length as what they replace, so the assertion
# below is measuring the same file either way.
if [[ "${J36_SPLASH:-1}" == 0 ]]; then
    sed -i -e 's/ j36\.splash=1/ j36.splash=0/' \
           -e 's/ loglevel=4 / loglevel=7 /' "$SDBOOT/mvii/boot.conf"
    grep -q ' j36\.splash=0' "$SDBOOT/mvii/boot.conf" || \
        die "J36_SPLASH=0 but boot.conf still asks for the splash; the bootargs line has changed shape"
    log "splash: boot.conf says j36.splash=0 loglevel=7 (--no-splash); this card boots to text"
fi

# The LK reads boot.conf into a fixed 2 KiB buffer and a longer file is silently
# truncated mid-line.  The size goes in the message: "too big" sends a reader
# hunting through a file where every line looks necessary, and "by 11 bytes"
# points straight at whichever comment was last touched.
boot_conf_bytes="$(stat -c %s "$SDBOOT/mvii/boot.conf")"
(( boot_conf_bytes <= 2048 )) || \
    die "boot.conf is ${boot_conf_bytes} bytes, $(( boot_conf_bytes - 2048 )) over the LK's 2048-byte read buffer; shorten a comment in the CONF heredoc"
log "boot.conf: ${boot_conf_bytes} bytes, $(( 2048 - boot_conf_bytes )) to spare in the LK's buffer"
verify_armv7_kernel "$SDBOOT/zImage" "the SD payload kernel"

cat > "$SDBOOT/README.txt" <<'README'
J36 Ultra (MT6592, ARMv7) SD card BOOT payload -- the launcher.

Copy the contents of this directory into the root of the FAT partition labelled
BOOT.  Existing files are not disturbed: an R36S card keeps its Image,
uInitrd, rk3326 device trees and boot.ini, and mvii/boot.conf points the MVII LK
at the ARMv7 payload instead.

  zImage                    plain 32-bit ARM kernel, no appended device tree
  mt6592-j36-ultra.dtb      the tree the LK loads separately and patches
  initrd.img                bring-up initramfs (busybox, the input module, and
                            the boot splash with its picture)
  mvii/boot.conf            filenames and command line for the MVII LK
  LICENSE.txt               which licence covers which file above, and where the
                            GPL-2.0-only source is; keep it with the payload

That is the whole partition as it is written, and the shortness is the design.
The board adds one file of its own on every boot -- mixos-log.txt, described
below -- and nothing else ever appears here.  The MVII LK reads FAT32 and nothing
else, so BOOT exists because the loader has to be able to open
it -- which makes it the launcher and only the launcher: the four files something
other than Linux has to read.  Everything else went to the OS partition, which is
ext2, and therefore holds symlinks and execute bits and is not a 100 MB partition
an R36S card shares with its own boot files.

THE OTHER HALF: sd-root.tar.gz, beside this directory.  Unpack it into the root of
the OS partition -- the shared armhf Debian rootfs -- and it adds /opt/mixos and
changes nothing else:

  sudo tar -C /path/to/the/mounted/ROOTFS -xzf sd-root.tar.gz

  opt/mixos/bin/mixdash    the dashboard: Qt5 Widgets straight into /dev/fb0
  opt/mixos/qt/            Qt 5.15 and its runtime closure, ~30 SONAME symlinks
  opt/mixos/share/mixdash/startup.mp3
                           the chime the dashboard plays once, on the frame it
                           first paints.  Delete it and the dashboard is silent
                           at boot and otherwise unchanged.
  opt/mixos/bin/doom       framebuffer Doom, and share/doom/ its IWAD
  opt/mixos/bin/j36-mixmirror
                           copies /dev/fb0 onto a USB-HDMI (DisplayLink) adapter
                           whenever one is plugged in, tile-diffed so a still
                           screen costs no USB traffic.  See "j36.usb and HDMI"
  opt/mixos/j36/mfgpower   powers the Mali-450 and reads its ID back; the gate
  opt/mixos/j36/modules/   lima and its dependencies, plus load.order
  opt/mixos/j36/mtkdrm/    the MT6592 display driver set, plus load.order
  opt/mixos/j36/audio/     the ALSA core and the MT6592 AFE driver, plus load.order
  opt/mixos/j36/usb/       the USB host stack -- PHY, MUSB, HID, udl, and the disk
                           set (scsi_mod, sd_mod, usb-storage, ntfs3) -- plus load.order
  opt/mixos/j36/power/     the MT6592 PMIC: battery gauge, charger, poweroff --
                           and the panel backlight, which is the same subject
  opt/mixos/j36/wifi/      the radio and wlan0: CONSYS rails, BTIF link, cfg80211
                           and the blobs, which ship in wifi/firmware/ with it
  opt/mixos/j36/gl/        Mesa's GL front end, plus links
  opt/mixos/j36/eglprobe   -f reports and paints /dev/fb0 with no DRM at all and
                           runs on every boot; -o draws the GPU's cube into
                           /dev/fb0 without a modeset; -z draws the same cube
                           straight into the scanout through /dev/j36fb, with no
                           copy at either end; the other modes say what can
                           create a GL context, and why not, and whether a frame
                           reaches the glass.  -p and -c own the screen while they
                           run and need -y; the dashboard is back when they exit.
                           See "j36/eglprobe -f", "-o", "-z" and "-p" below.

A TARBALL AND NOT A DIRECTORY, on purpose: this payload's symlinks, modes and
ownership are load-bearing -- the Qt SONAME aliases are symlinks, mfgpower and the
probe have to stay executable -- and a tarball is the copy that cannot lose them
whatever machine does the copying.  Unpack it as root.

Everywhere below, "j36/..." means /opt/mixos/j36/... on that partition.  A card
written by a build from before this layout has the same directory on BOOT instead,
and /init looks there second and says which one it found, so such a card still
boots -- but the tarball above is where new payloads go.

The R36S kernel on the same card is arm64 and stays there for the R36S.  The
armhf Debian rootfs is shared, and this kernel can now mount it: MSDC1, the
microSD host, is driven by mtk-sd through a mediatek,mt6592-mmc node, and ext2 --
which is what the rootfs is -- is built in, along with ext4 and btrfs for the
cards earlier builds wrote.  /init verifies a candidate partition by mounting it
read-only and looking for /sbin/init, then switch_roots into it.  If nothing
qualifies -- or if you delete root= from mvii/boot.conf -- it stops at a busybox
shell on the panel and on the serial port instead, and prints /proc/partitions so
you can see what the kernel did find.

The log on this partition
-------------------------

mixos-log.txt, at the top level, beside zImage.  Take the card out, put it in any
machine that can read a FAT, and open it: that is the whole procedure, and it is
the only one this board has.

WHY IT IS HERE AND NOT SOMEWHERE SENSIBLE.  The journal on this image is volatile
-- the rootfs build deletes /var/log/journal, so journald keeps everything in /run
and the whole boot's log dies with the power.  The two partitions that do hold
things are ext2, which is the one filesystem a Mac will not mount.  And the panel,
which is the only output the board has, belongs to the dashboard within a few
seconds of the splash finishing.  So a diagnostic that is not on the FAT is a
diagnostic that has to be photographed, which is how the last several bugs on this
board were reported.

WHAT IS IN IT.  The four sections at the top are the four things that go wrong on
this board -- Wi-Fi, the battery and charger, the gamepad and its sticks, and USB
-- each one being the sysfs nodes for that subsystem and the kernel messages that
mention it, pulled out of the ring buffer and put under a heading.  Then the panel
and dashboard, then the loaded modules, then the entire dmesg and the last 4000
lines of the journal.  Section 0 is what systemd says failed.  Nothing is
collected that is not already on the screen of a machine with a serial cable
attached.

WHEN IT IS WRITTEN.  Twenty seconds after systemd reaches the login target, again
at seventy-five seconds, and once more if the board is shut down cleanly.  The
second pass overwrites the first, so the file on the card is the most complete
picture the board survived long enough to write; a board that hangs before twenty
seconds leaves no file at all, and that is itself the finding.

WHAT IT COSTS.  Each pass mounts this partition read-write, writes about a
megabyte, syncs and unmounts -- a second or so, three times in a session.  The
write goes to a temporary name and is renamed over the previous log only once it
is complete, so pulling the power mid-write loses the new log and keeps the old
one rather than leaving a truncated file under the name you were told to read.
Outside those three windows BOOT is not mounted, which matters on a handheld with
a physical power switch.  j36.log=0 in the bootargs turns the whole thing off.

The command line, word by word
------------------------------

console=ttyS0,115200n8 console=tty0
    Both consoles receive every printk; what the order decides is /dev/console,
    which is whichever came LAST.  With the UART last, a boot appears to stop
    dead at "random: crng init done" -- systemd logs to /dev/kmsg only until
    journald takes over, and from then on everything it says goes to a serial
    port that may have nothing plugged into it.  tty0 last puts /dev/console on
    the panel.

systemd.journald.forward_to_console=1, gone
    It copied every service log line to /dev/console, and /dev/console is the
    panel, so the panel showed services starting and failing instead of a blinking
    cursor.  That was the right trade while there was no other way to read the
    journal.  There is one now -- mixos-log.txt, below -- and the word had a cost
    the whole time: it is the mechanism by which anything a unit prints lands on
    top of the boot splash, at a redraw per line on a 640x480 framebuffer.

    Nothing worth reading was lost with it.  The units that have to reach the
    panel say so themselves: mixdash.service and mixdash-missing.service are both
    StandardOutput=journal+console, and the kernel's own printk never went through
    journald at all -- it goes to both consoles because of the console= words
    above, and loglevel=4 is what bounds it.  Put the word back in the bootargs
    line if you want the running commentary; it is a diagnostic, not a default.

j36.log=1
    Default ON, and the only j36 word that is.  Twenty seconds into the boot, and
    again at seventy-five, and once more on a clean shutdown, /init's staged
    j36-logdump.service mounts this partition read-write, writes mixos-log.txt at
    the top of it and unmounts again.  See "The log on this partition" below.
    j36.log=0 turns it off.

systemd.mask=firstboot.service
    MixOS's first-boot script is written for the RK3326 image and this
    configuration cannot finish it.  It expands the partitions in two stages with
    a reboot between them, then untars /roms.tar and /tempthemes -- which a
    GUI-mode build does not ship.  With the tars missing, its two progress loops
    spin 15000 subshells apiece before giving up, which is minutes of dead panel,
    and then it reboots.

    IT IS ALSO DISABLED IN THE ROOTFS NOW, and this word is the belt to that
    brace.  What it does is carve a third partition out of the free space at the
    end of the card -- exactly the space /init hands to ROOTFS on the first boot
    -- so on this layout it is not merely useless, it is the one thing that would
    undo the expansion.  Do not delete this word.

    THE ONE THING IT DID THAT THIS BOARD STILL NEEDS -- giving the rest of the
    card to the operator -- is done instead by expand_root in /init, before
    switch_root.  ext2 has no online resize, so it has to happen while the
    filesystem is unmounted, and that is the only moment in the boot when the
    root filesystem is not in use.  It extends ROOTFS to the end of the disk with
    sfdisk, e2fsck's it and then resize2fs's the ext2 inside it, and it does none
    of that once there is nothing left to take.  It never mkfs's anything, which
    is the whole reason it is not the script above.

batt_led.service, no longer masked
    The RK3326 battery LED daemon, and the first unit the forwarded log caught:

      batt_life_warning.py[829]: FileNotFoundError: [Errno 2] No such file or
        directory: '/sys/class/power_supply/battery/capacity'
      batt_led.service: Scheduled restart job, restart counter is at 21.

    It reads that capacity file and writes /sys/class/gpio/gpio77/value, and for
    every build up to this one the kernel had neither, so the unit was masked
    here: Restart=always, RestartSec=2, StartLimitIntervalSec=0 -- explicitly
    unbounded -- is a Python traceback on the console every 2.3 s for as long as
    the machine is up.

    Both halves are now fixed rather than masked.  j36.power=1 loads the MT6592
    PMIC driver, which registers a battery supply, and the daemon finds it by
    walking /sys/class/power_supply and taking the first entry whose type file
    says Battery -- by type, not by the RK3326 name, so it does not care what
    this driver called it.  The LED half was the one that actually crashed: this
    kernel has no sysfs GPIO export and no gpio77, EVERY branch of the old loop
    read the LED including the "battery is fine" one, and the unit is
    Restart=always with RestartSec=2 and StartLimitIntervalSec=0 -- explicitly
    unbounded -- so the first pass raised and the console got an eight-line
    traceback every 2.3 s for as long as the machine was up.  On a board whose
    console IS the panel, that is the splash screen overdrawn with a traceback
    forever.  batt_life_warning.py no longer assumes any path exists and nothing
    in it is fatal: a missing LED means it has nothing to blink and it keeps
    reading the gauge, a missing gauge means the driver has not come up yet and
    it waits.  It does not exit, so it is never restarted.

    It is left unmasked deliberately: it prints the percentage it is reading,
    which is the cheapest standing check that the power_supply registration
    survived a kernel bump.  Add systemd.mask=batt_led.service back to boot.conf
    if even that is not worth it on a particular card; nothing else depends on
    it.

    That script lives in the Debian rootfs, not in the J36 payload --
    finishing_touches.sh copies device/rg351mp/*.py to /usr/local/bin and
    enables the unit -- so a card written by an older build still carries the
    crashing copy.  --mix-only does not refresh it; a full build does, the
    finalization stage running on a resume as well.

    Which is why /init no longer relies on the script being current.  It writes
    /run/systemd/system/batt_led.service.d/zz-j36-bounded.conf on every boot:
    StartLimitIntervalSec=30 with StartLimitBurst=5, undoing the unit's own
    "never give up", and StandardOutput/StandardError=journal so that whatever it
    does have to say is not said on the picture.  A card carrying the old script
    now gets five tracebacks in the journal and a failed unit instead of one
    traceback on the panel every 2.3 s for ever; a card carrying the fixed script
    never exits, so a start limit it cannot reach costs it nothing.  A drop-in
    directory merges across /run, /etc and /lib -- a unit file does not -- which
    is what lets a file in a tmpfs modify a unit installed in /etc.

    Worth knowing when reading its output: with no power-path FET on this board,
    VBAT is the system node, so the percentage the daemon reads is the gauge's
    integrated estimate seeded from the PMIC's wakeup OCV latch, not a direct
    reading of a battery-only rail.  It is honest, and it moves slowly on
    purpose.

    Other RK3326-only units are left alone on purpose: 351mp.service (power LED,
    backlight, amixer), audiopath, audiostate and wifi_importer are all
    Type=oneshot, so they fail once and stay failed.  Bounded noise is evidence;
    only the unbounded one had to go.

    The three audio units are worth watching now that j36.audio=1 registers a card:
    they were failing on a machine with no /dev/snd at all, and with one present
    their amixer calls will look for RK3326 control names -- "Playback", "HP", a
    dozen SoC-specific ones -- that this card does not have.  A oneshot that fails
    on a missing control is the same bounded noise as before and is left alone; what
    would not be bounded is a control this card DOES have being set to something
    unwanted, so "Master Playback Switch", "Master Playback Volume", "Speaker Amp
    Switch" and "Headphone Switch" are the four names to check against those scripts
    if the sound ever changes by itself.  "Headphone" is the likeliest collision of
    the four, because an RK3326 audio script naming a route is naming something the
    simple mixer will now find here.

rdinit=/init root=/dev/mmcblk0p2 rw rootwait
    See above: /init does the mounting, so root= cannot panic the kernel.

j36.doom=1
    Gone.  /init does not read this word any more and there is nothing on BOOT for
    it to run: a J36_DOOM=1 build stages doomgeneric and its IWAD as
    /opt/mixos/bin/doom and /opt/mixos/share/doom on the OS partition, and the
    dashboard's Doom card launches them -- after the boot, from a shell, rather than
    in the middle of an initramfs.  It is still the quickest thing to reach for when
    the GL path breaks: Doom needs no DRM and no GL, so if Doom draws and the cube
    does not, the panel is fine and the fault is above it.

j36.lima=1
    Power the Mali-450 and load the DRM lima driver, in that order and only in
    that order -- see below.  Same removal story as the rest: delete the word, or
    j36/mfgpower, or j36/modules, and the GPU is never touched.

j36.mtkdrm=1
    Load the MT6592 display driver set from j36/mtkdrm/ -- mtk_drm, the DDP
    components, the MIPI-TX PHY and the panel -- which is what produces
    /dev/dri/card0.  Same removal story again: delete the word or the directory and
    not one line of it is loaded.  Loading it is visually a no-op, and that is by
    construction rather than by luck: see below.

j36.audio=1
    Load the ALSA core and the AFE driver from j36/audio/, which is what gives this
    kernel a /dev/snd and a controlC0 at all -- CONFIG_SOUND and CONFIG_SND were off
    until now.  Independent of the three words above: nothing here touches DRM, GL
    or the panel, and it is loaded before switch_root so systemd finds a card to
    restore into.  Same removal story as the rest.

    IT PLAYS OUT OF THE HEADPHONE JACK AND NOT OUT OF THE SPEAKER.  What this word
    switches on is the digital half -- the AFE's DL1 memif, the interconnect route
    to the I2S DAC, the MT6323 ABB downlink, and one 16-bit stereo playback PCM at
    8-48 kHz -- plus the analog headphone buffers, which are on by default because
    they run off the reference the DAC already needs and there is no rail for them
    to take down.  What it does not switch on is the class-D speaker amp: that has
    its own word, below, because it can switch the board off.

    NOTHING ON THIS BOARD DETECTS A PLUG.  The MT6592 has an ACCDET block and the
    vendor HAL drives it, but through an ioctl on a kernel driver that does not
    exist here, and the J36 brings no headphone-detect GPIO out either.  So which
    output is live is a setting and never a discovery.  Two mixer switches decide
    it, both saved by alsa-restore, and the dashboard puts them on Settings > Sound:

      amixer -c0 set Headphone off        speaker only
      amixer -c0 set "Speaker Amp" off    jack only
      both on                             both at once, which is a real setting

    "Master Playback Volume" moves whichever of them is up: the class-D level and
    the headphone buffer gain are two different registers under one element, so the
    volume keys on the side of the case do not have to know where the sound is
    going.

    The one line to read afterwards is the driver's own:

      j36-mt6592-audio ...: AFE DL1 DMA is live (cursor moving)

    on the first stream.  This is the first thing on this board ever to ungate the
    AFE's functional clocks -- MVII has the sequence and compiles it out -- so
    whether the DMA runs at all was an open question until that line appeared.  If
    instead you get "AFE DL1 cursor has not moved in 250 ms", the memif is not
    fetching and the driver falls back to pacing the stream from the wall clock, so
    audio applications keep running rather than blocking on a period that never
    completes.  CLK_CFG_AUD, printed before and after at probe, is the next thing to
    read: bits 31 and 23 must both be 0 afterwards.

j36.audio=speaker
    Everything j36.audio=1 does, and then the class-D amp, once the DL1 cursor has
    been seen advancing -- never before, because an amp fed by an unclocked DAC
    drives the coil with DC.

    FIT A CELL FIRST.  The amp is the largest load on this board and it hangs off
    VBAT, which on this PMIC is the system node, not a battery-only rail.  With no
    cell fitted VBAT is held up by the charger's current source alone, and MVII
    measured the amp at output pulling it under the PMIC's undervoltage lockout: the
    board switches off a few seconds into playback.  The driver opens at level 8 of
    11 rather than at the vendor's maximum for the same reason.  Recovery from a
    board that will not stay up is to delete j36/audio from the card, or this word
    from mvii/boot.conf, from any machine that reads SD cards.

    WHAT `default' WAS ON THIS CARD.  The shared rootfs links /etc/asound.conf to
    /home/virtua/.asoundrc -- see finishing_touches.sh -- and that file is the
    RG351MP's:

      pcm.!default { type plug  slave.pcm "dmixer" }
      pcm.dmixer   { type dmix  ipc_key 1024
                     slave { pcm "hw:0,0" period_size 1024 buffer_size 4096
                             rate 44100 } }

    So every stream that named `default' on this board went through a shared-memory
    software mixer carrying an RK3326-era buffer geometry, on top of an AFE that has
    no playback interrupt at all -- j36_mt6592_audio polls the DL1 cursor from a
    work item and calls snd_pcm_period_elapsed() from there.  dmix exists so several
    processes can share one card.  This handheld has one audio consumer at a time,
    so the layer bought nothing and stood between the player and the only DAC on the
    machine, with a hard-coded rate and buffer shape that nothing here chose.

    WHAT `default' IS NOW.  With j36.audio in the command line the initramfs stages
    its own two stanzas into /run/j36/asound.conf and j36-asound.service binds them
    over the card's file before sysinit.target:

      pcm.!default { type plug  slave.pcm "hw:CARD=j36,DEV=0" }
      ctl.!default { type hw    card j36 }

    No softvol layer over that, and there does not need to be one: the volume on
    this card is analog on both outputs, so nothing has to attenuate in software.

    Three files, on the same pattern as the automounter under j36.usb below:

      /run/j36/asound.conf                    the two stanzas above
      /run/j36/bin/j36-asound                 the script that binds them
      /run/systemd/system/j36-asound.service  which runs it once, early

    plug over hw is what `default' has always meant: rate, format and channel
    conversion in alsa-lib for anything that does not match this card's one PCM,
    which is stereo s16 from 8 to 48 kHz.  CARD=j36 is the id the driver itself
    passes to snd_devm_card_new, so it stays correct when a USB headset or an HDMI
    adapter enumerates as card 0.  ctl.!default is there so amixer and alsamixer
    with no -c land on the same card as the sound does.

    THAT IS A BIND MOUNT AND NOT A WRITE, which is the invariant this whole
    initramfs is built on: nothing on the shared rootfs is written, and .asoundrc
    lives in /home/virtua, a directory on the rootfs an R36S boots from as well.
    The bytes on the card are untouched, the other launcher gets its own file back,
    and there is
    nothing to undo.  The service resolves /etc/asound.conf, /home/virtua/.asoundrc
    and /root/.asoundrc with readlink -f and binds each distinct target once, so the
    symlink between the first two costs one mount rather than two.

    A conf.d drop-in would have been the polite way to do this and it cannot work.
    alsa-lib reads /usr/share/alsa/alsa.conf.d/, then /etc/alsa/conf.d/, then
    /etc/asound.conf, then ~/.asoundrc, and later wins: pcm.!default is an override
    in both files, so the drop-in loses to the very file it is correcting.  The only
    thing that beats ~/.asoundrc is ~/.asoundrc.

    NO dmix, AND THAT IS A DECISION.  It buys one thing -- several processes sharing
    the DAC -- and the cost on an AFE with no playback interrupt is a software mixer
    clocked off a polling work item.  The price is that the second opener gets EBUSY
    while the first is playing.  To trade it back, put this in the file instead:

      pcm.!default { type plug  slave.pcm "dmixer" }
      pcm.dmixer   { type dmix  ipc_key 1024
                     slave { pcm "hw:CARD=j36,DEV=0" rate 48000 } }

    Either edit /home/virtua/.asoundrc from a PC -- it is on ROOTFS, so that means a
    Linux machine -- and drop j36.audio back to a word that does not stage the
    override; there is none, so delete /run/j36/asound.conf and restart
    j36-asound.service instead.  Or change setup_asound in the initramfs and rebuild.
    Editing the file on the card changes it for the R36S too; the /run file does not.

    THE SAME UNIT ALSO FIXES aplay, and for the same reason it fixes `default': the
    file the machine reaches for is the wrong one.

      aplay: symbol lookup error: undefined symbol: snd_pcm_subformat_value

    That symbol arrived in alsa-lib 1.2.10.  aplay is from the alsa-utils apt put on
    the card and wants it; the libasound.so.2 the loader hands it is older and does
    not have it.  Both are soname 2, so nothing complains until the call.  It is not
    an aplay problem -- every alsa-lib caller on the machine goes through the same
    copy -- so j36-asound looks through the library directories, in apt's order
    first and the frozen /usr/local and /opt copies after, finds one that exports
    the symbol, and bind-mounts it over the ones that do not.  Again a cover and not
    a write.  It prints which file it put in front of which, or

      j36-asound: no libasound.so.2 here exports snd_pcm_subformat_value

    if every copy on the card is old, which is a rootfs to fix rather than something
    a boot can paper over.

    THE DASHBOARD STILL NAMES THE CARD, and that is independent of all of the above.
    MediaPage::alsaDevice() reads /dev/snd, takes the lowest-numbered pcmC*D*p, and
    hands ffmpeg `plughw:C,D'.  It resolves entirely inside alsa-lib's own
    definitions, so the player keeps working on a card where none of this ran, and
    it prints the device it opened on its Output row -- so "there is no sound" and
    "there is no sound from plughw:0,0 at 48 kHz" are distinguishable without a
    serial console.

j36.usb=1
    Load the USB host stack from j36/usb/: the out-of-tree PHY driver, musb_hdrc
    and its MediaTek glue, usbhid and hid-generic, udl, and the mass-storage set --
    scsi_mod, sd_mod, usb-storage and ntfs3.  That is a keyboard, a mouse, a hub, a
    DisplayLink adapter as a second DRM node, and external disks.  Same removal
    story as the rest: delete the word or the directory and not one line of it is
    loaded.

    EXTERNAL DISKS MOUNT THEMSELVES, which is new and is the reason SCSI is in this
    kernel at all -- a USB disk is a SCSI target, and usb-storage without sd_mod
    enumerates one and creates no /dev/sda.  /init does not mount them; it writes
    three files into the rootfs's /run and lets udev and systemd do it:

      /run/udev/rules.d/99-mixos-automount.rules   sd* carrying a filesystem
      /run/systemd/system/mixos-automount@.service one instance per volume
      /run/j36/bin/mixos-automount                 the script that mounts

    A volume lands at /media/<its label>, or at /media/sda1 when it has no label,
    or under /run/media when the rootfs is read-only.  vfat, exfat and NTFS are
    mounted owned by the login user, because none of those three has ownership on
    disk and the default would be root; ext and btrfs keep their own.  Everything
    is mounted nosuid,nodev -- somebody else's disk does not get to bring a setuid
    binary or a device node -- but NOT noexec, so a program on a stick still runs.

    UNPLUG IS HANDLED BY BindsTo=dev-<name>.device, which is systemd's own device
    tracking: pull the disk, the device unit goes away, the instance stops, and its
    ExecStop unmounts.  A tidy unmount is tried first and a lazy one after it, so a
    disk yanked mid-write cannot wedge the boot.  Nothing polls, and there is no
    udisks2 or dbus involved -- neither is on this image.

    A dirty NTFS volume -- a Windows machine that fast-booted instead of shutting
    down -- is mounted READ-ONLY rather than forced.  That is ntfs3's own refusal
    and it is the right one: forcing it means two operating systems believing they
    own the same journal.  Shut Windows down properly and it mounts read-write.

    This SoC has exactly ONE USB core -- a MUSB at 0x11200000 -- and no EHCI, OHCI
    or XHCI at all.  So the single port is the whole bus: whatever you want plugged
    in at once goes through a hub, and every host controller symbol is refused at
    kernel-config time rather than left to probe something that is not there.

    IT SOURCES 5 V, off VBAT.  This used to say the opposite, and the old text was
    wrong for a specific reason worth recording: VBUS on this board is not the
    MT6322 boost that a phone reference design would use, it is a plain GPIO.  The
    stock Android kernel proves it in four instructions.  mt_usb_set_vbus(), at
    0xc052e938 -- located through the __func__ pointer its own printk loads, line
    60 of MediaTek's musb glue -- does, on the `on' path and nothing else:

      mt_set_gpio_mode(0x8000000f, 0);    ops slot 0x3c, GPIO base + 0x600
      mt_set_gpio_out (0x8000000f, 1);    ops slot 0x30, GPIO base + 0x400

    0x80000000 is MediaTek's marker on a GPIO_..._PIN constant, stripped by the
    wrappers before they bounds-check the pad against 0xa8, so the pad is 15 and
    the drive is active high.  Neither callee has a symbol in that image; both were
    identified from the ops table by which register they write, and both offsets
    are ones mt6592_led.c already drives on this board.  Pad 15 is loaded by
    exactly two functions in the whole 12 MB kernel -- that one and the pad-config
    routine beside it -- so nothing else here wants it.  The device tree carries it
    as j36,drvvbus-pad = <15>, and j36_mt6592_usb_phy.ko raises it whenever it has
    decided the port is a host, and drops it otherwise and in .power_off.

    So a bus-powered hub now enumerates.  FIT A CELL FIRST, for the same reason as
    j36.audio=speaker above: the 5 V is a boost off VBAT, and VBAT on this PMIC is
    the system node.  A bus-powered load on a cell-less board is the same class of
    load as the class-D amp, which MVII measured pulling VBAT under the
    undervoltage lockout.

    THE ROLE IS NOT MEASURED, AND THAT IS A CORRECTION.  Everything this text used
    to say about measuring it rested on one belief -- that a J36 Ultra has a single
    connector doing both jobs, so sourcing 5 V and sensing a charger were mutually
    exclusive in the hardware and software had to keep asking which one was wanted.

    THERE ARE TWO CONNECTORS.  A DC inlet, which charges and has no data lines in
    it at all, and the OTG port, which carries the data.  CHRDET hangs off the
    inlet; DRVVBUS hangs off the port; they are separate sockets on separate nets
    and neither can be mistaken for the other.  So there is nothing to arbitrate:
    the port is a host, it sources 5 V, and it keeps doing so.  vbus defaults to 1
    now, which pins exactly that.

    WHAT THE BELIEF COST.  The PMIC held the charger off for as long as DRVVBUS
    was up, and on this board DRVVBUS is up the whole time the board is on.  So
    online read 0 with a charger in the inlet, and -- because that same value is
    what arms the charger -- nothing charged.  CAPACITY falling, CURRENT_NOW at
    -35 mA, STATUS Discharging, plugged into the wall.  The PMIC reads CHRDET on
    its own now; chrin_shared=1 restores the interlock for a board where the belief
    is true, and it is 0 here.

    AND WHAT THE MEASUREMENT ITSELF COST, which is the other half.  Measuring means
    dropping DRVVBUS, and dropping DRVVBUS on the port that IS the data port is a
    hundred-millisecond power cut to whatever is plugged into it, every fifteen
    seconds, for the life of the board.  With a dedicated charge inlet there was
    never anything to learn from it.

    THE MEASUREMENT IS STILL IN THE DRIVER, under vbus=-1, because some boards do
    share the socket.  With DRVVBUS low and the PHY's force_* overrides released
    for the stock 800 us settle, DEVCTL's VBUS field is a live comparator on the
    pin: above AValid means something outside is feeding the bus -- a charger, a
    host PC -- and the answer is to be a device and let the PMIC charge; below it
    means nothing is out there and the answer is to be a host.  It is re-asked
    every 3 s and skipped whenever something that has enumerated is attached.

    WHAT "ATTACHED" HAS TO MEAN, for that path.  DEVCTL's FSDEV/LSDEV is a pull-up
    on D+ or D-, and it was once taken as "a device is here".  A divider-type
    charger presents the same pull-up -- an Apple 2.4 A brick holds D+ near 2.7 V,
    the Samsung scheme holds both near 1.2 V -- so on a shared-socket board,
    plugging a charger in latched the port to host and held DRVVBUS high for the
    whole uptime.  The distinction is not in any register and does not have to be:
    a device answers a bus reset and is given an address, a charger answers
    nothing.  So the PHY asks usbcore -- usb_for_each_dev(), root hubs skipped --
    and the latch holds only while something on the bus HAS an address.  Something
    that holds the lines high without ever becoming a device is measured after
    attach_grace_polls (default 3, about ten seconds).  A stick, a mouse or a hub
    is never power-cycled by it, because it enumerates and is exempt from the
    moment it does.

    Which is why "DEVCTL bits 3 and 4 report whether VBUS is valid" -- what this
    text used to say -- is a trap rather than a fact.  Those bits are only a
    measurement while the overrides are OFF.  With force_vbusvalid and force_sessend
    asserted, which is most of the time, DEVCTL reports back the values the role
    sequence wrote into the PHY, not the state of the pin.  An early log read
    "VBUS below SessionEnd" 49 us after "DRVVBUS pad 15 high" and never changed for
    the whole uptime, which is exactly what a forced register does and nothing a
    real rail would ever do.

    The role still sets DEVCTL.SESSION going into host and clears it coming out,
    because musb_start() runs once on this board -- from musb_hub_control's
    USB_PORT_FEAT_POWER at boot, and nothing calls it again -- so a port that went
    to device and came back would otherwise be a host with no session.  At the
    default it goes in once at power-on and stays.

    THE DIAGNOSTICS PAGE HAS A ROW FOR ALL OF THIS, under Power: "Connectors" puts
    the DC inlet (CHRDET), the OTG port (the DRVVBUS pad) and the sign of the cell
    current on three lines side by side, and colours itself on whether the charger
    is actually putting anything in.  A charger attached with the current still
    negative is the old bug coming back, and it names chrin_shared on the spot.

j36.usb=novbus
    The same stack with the pad never driven -- /init passes vbus=0 to the PHY
    module, which is where it has to go, because a kernel-cmdline modname.param=
    reaches built-in modules only and every one of these is loadable.  Two cases
    want it: no cell fitted, and a self-powered hub, which brings its own 5 V and
    has no use for the port's.

    The role is still measured with this word.  What it removes is only the
    sourcing half: the port will still go to device role for a charger and still
    charges normally, and a self-powered hub still enumerates, because the hub's
    own 5 V is what the measurement sees.  A bus-powered device will not come up,
    which is the entire point of asking for it.

j36.usb=vbus
    The opposite pin: never measure, always be a host, hold DRVVBUS high for the
    whole uptime.  This is what the port did before the role became automatic, and
    it is kept for the board whose port the measurement gets wrong.

    Two costs, both real.  The measurement's 60 ms VBUS gap is gone, which is the
    only reason to want this -- a device that latches its own power-up and dislikes
    a brief drop would prefer it.  And the charger is permanently blind: with the
    pad held high the PMIC cannot distinguish a charger from our own boost, refuses
    to arm, and the diagnostics page says No cable forever and means it.

    It is =m, and loaded from /init rather than built in, for a reason worth knowing
    before changing it: an APB access to a clock-gated MediaTek peripheral does not
    fault, it hangs the bus until the watchdog fires.  The PHY driver's .init is the
    first hardware contact anything in this chain makes -- musb_core calls it before
    reading a single MUSB register -- and ungating PERI is what it does there.
    Nothing may probe 0x11200000 before that has run.

    The interrupt is a MEASUREMENT, not a fact.  The MUSB IRQ number is not present
    anywhere in the stock Android kernel image -- that driver passes no resources
    and hardcodes the base -- so the device tree carries a best estimate, SPI 64,
    extrapolated from MT6735's table against this SoC's known MSDC0.  Being wrong is
    safe and says so: musb simply receives no interrupts and enumeration fails
    quietly.  The PHY driver scans GICD_ISPENDR across power-on and prints

      GIC INTID nnn became pending: device tree cell is <0 nnn-32 8>

    because an unclaimed level-sensitive line latches pending whether or not it is
    enabled.  One boot log with something plugged in yields the real number.

j36.usb and HDMI
    The HDMI on a USB dock is not this SoC's -- MT6592 has no HDMI or DP encoder of
    any kind.  It is a DisplayLink chip, and the whole path is drivers/usb plus
    udl.ko, which is why the word that switches it on is j36.usb and not a display
    word.

    Mainline udl speaks the USB 2.0 DL-1x0/DL-1x5 protocol only.  DL-3xxx and later
    adapters are USB 3.0 and need the out-of-tree evdi driver, which is not here;
    this port is USB 2.0 high speed, so those are the wrong adapter for this board
    whatever the driver.

    What you get when it works is a DRM card node with a connected output, listed by
    run_usb at boot.  PAINTING it is a further step, and it is the reason
    j36-mixmirror exists: DRM_FBDEV_EMULATION is off on purpose -- it is a global
    bool, and turning it on for udl's sake would also make mtk_drm create a second
    /dev/fb, which is exactly what keeping /dev/fb0 as simplefb's is meant to
    prevent.  So udl has a card node and no framebuffer, while the dashboard is Qt on
    linuxfb and knows only /dev/fb0, and neither one can see the other.

    opt/mixos/bin/j36-mixmirror closes that gap by copying between them.  It polls
    /dev/dri every two seconds; when a node whose DRM driver NAME is "udl" appears it
    picks the connected connector, chooses the mode that fits the most whole copies of
    640x480, creates a dumb buffer, modesets, and from then on diffs /dev/fb0 in 64x64
    tiles and pushes only the tiles that changed through DRM_IOCTL_MODE_DIRTYFB --
    which is the ioctl udl transfers on, so a dumb buffer written and never marked
    dirty shows nothing at all.  A still dashboard therefore costs no USB traffic.

    It MIRRORS.  The handheld keeps its own screen, the dock shows the same picture
    scaled by a whole number and centred, and unplugging is a process that goes back
    to polling rather than a display server losing its output.  There is no extended
    desktop and no input routing.

    The driver-name test is the safety interlock and not a convenience: minor numbers
    here are assigned in probe order across up to three DRM drivers (lima, mediatek,
    udl), so cardN moves between boots, and a modeset aimed at mtk_drm would take the
    panel with no way back.  Any node that is not named "udl" is closed again without
    a second ioctl being sent to it.

      j36-mixmirror -s     say what it found and what it would do, and exit
      j36-mixmirror -v     the same reasoning, while running
      j36-mixmirror -1     no scaling, 1:1 centred, for a mode that reads oddly

    It is started by j36-mixmirror.service, which /init writes into /run only when the
    binary is on the card.  Delete opt/mixos/bin/j36-mixmirror and the feature is gone
    with nothing left behind to fail.

    WHERE IT SAYS WHAT IT IS DOING, which took a bug to get right.  A mirror with
    nothing to mirror onto should not fill a journal, so every "not yet" branch used
    to be quiet -- and quiet is indistinguishable from a service that never started.
    A board with a dock plugged into it and a black television wrote nothing at all
    for its whole uptime, and that is how this feature came to be reported as never
    running.  So it now says every state CHANGE, in the journal and in one file:

      /run/j36/mirror.status    a keyword -- starting, no-drm, no-adapter, no-screen,
                                no-mode, mirroring, stopped -- and under it the same
                                sentence it put in the journal.  Rewritten on every
                                change, silent between them, and on tmpfs: nothing on
                                the shared rootfs is written.

    The dashboard's Diagnostics page shows that line under "USB-HDMI dock", because
    the person this matters to is holding the handheld and has no journal in front of
    them.  When the file is absent the dashboard says so and reads the three facts
    itself -- is udl.ko loaded, is a card node bound to udl, is there a 17e9 on the
    bus -- so "the mirror is not running" and "there is nothing to mirror onto" are
    different rows and not the same silence.  j36-logdump section 5 carries all of it.

    The keyword exists so that the sentence can be reworded without breaking anything
    that reads the file; nothing outside this program should parse the sentence.
    j36-mixmirror -s and -o deliberately do NOT write the file, so looking at a dock
    by hand cannot overwrite what the running service is reporting.

j36.power=1
    The MT6592 PMIC: the battery gauge, the charger and a poweroff that actually
    switches the board off -- and the panel backlight, which is the same subject
    seen from the other end.  Two modules, j36/power/j36_mt6592_pmic.ko and
    j36/power/j36_mt6592_backlight.ko, loaded after the USB payload.

    The backlight half registers /sys/class/backlight/j36-backlight, with a
    max_brightness of 1023 -- the 10-bit duty of the BLS block at 0x1400a000,
    which is what the LK programs and what the TPS61161 in front of the LED
    string dims on.  It does not pick a level: it reads the duty the LK left
    behind and reports that, so brightness starts at whatever the loader handed
    over.  Nothing in Linux writes the block until something asks for a
    different number, and the dashboard's Settings -> Display page is what
    normally does.

    It never blanks by itself -- not at unload, not at shutdown.  The panel is
    the only output this board has, and a backlight that goes dark when its
    module is removed is one that can take the machine away from whoever is
    trying to debug it.

    What it registers is two supplies.  Both readers -- batt_led.service and
    mixdash -- walk /sys/class/power_supply and take the first entry whose type
    file says Battery, so neither has to be told this driver exists.  It is
    called `battery' anyway, that being the path every RK3326 script and every
    forum answer for this family reaches for by hand.  Beside it,
    /sys/class/power_supply/usb reports online,
    usb_type (SDP, CDP, DCP or an Apple brick) and current_max, which is what the
    BC1.2 handshake decided the wall will actually give us.

    It also reports one attribute that is not a standard power_supply property,
    vbus_sourcing, and what it is for has changed. It was added because online had
    two ways of being zero that meant opposite things, on the belief that this
    handheld had ONE connector doing both jobs -- so the port sourcing 5 V and the
    PMIC seeing a charger were the same net, and the driver held the charger off
    whenever the pad was up.

    IT HAS TWO SOCKETS AND ONE SENSE PIN, which is not the same thing. A DC inlet,
    which charges and has no data lines, and an OTG port, which carries the data and
    sources 5 V the whole time the board is on -- but CHRIN is a single pin and it
    is on the OTG net. MVII's LK says so outright: its charger block "only ever saw
    the OTG port". So holding the charger off whenever the pad was up was the wrong
    RESPONSE, and it cost the board its charge; the premise behind it was right.

    That is what "it says charging all the time, even if it's unplugged" was. The
    pad is up for the whole uptime, the pin is therefore above the detect threshold
    for the whole uptime, and CHRDET was never lying -- it was reporting our own
    5 V. The PMIC now measures the same pin on AUXADC channel 4 every poll and,
    while the pad is up, requires the input to clear the PACK before it will call it
    a charger: a switch fed from VBAT cannot exceed VBAT and a charger is 4.4 V at
    worst. The result moves the report, the plug edge and the gauge, and never
    reaches the arm -- so a wrong answer costs a wrong icon and cannot stop a charge.

    What vbus_sourcing publishes is the port: 1 means the board is feeding it, which
    is what makes a bus-powered stick or a mouse work AND is what puts a supply on
    the pin CHRDET watches, 0 means it is not, and -1 is a device tree with no
    j36,drvvbus-pad. Read beside battery/status it is what separates "charging" from
    "driving its own input". Diagnostics -> Power puts it next to online and the
    cell current in one row, and section 2 of mixos-log.txt dumps it beside
    chrin_shared so a log says which of the two stories it is from.

    IF THE PORT'S 5 V IS A BOOST RATHER THAN A SWITCH the measurement cannot help,
    because both read about 5 V, and the port has to stand down instead of being
    assumed -- j36.usb=automeasure. The dmesg line that answers which board this is
    prints every twenty seconds without turning anything on:

        pmic: charging: VBUS=0 CHRDET=1 src=1 VCHR 4050 mV ... VBAT 4020 mV

    With the cable OUT and src=1: VCHR sitting on VBAT is a switch and the bar above
    has already fixed it; VCHR a volt clear of VBAT is a boost, and automeasure is
    the lever.

    THE ONE FACT THAT EXPLAINS THE REST: this board has no power-path FET, so
    VBAT is the system node rather than a battery-only rail.  Every live ADC
    channel measures what the whole board is doing at that instant -- the
    backlight, the amp, the GPU -- not the state of the cell.  So the gauge does
    not read a voltage and look it up in a curve.  It seeds from the OCV the PMIC
    latched at wakeup, when nothing was running, and from there integrates
    current measured as a differential across the sense resistor.  The percentage
    moves slowly and does not jump when the backlight changes, which is the point.
    There is deliberately no PRESENT property: with VBAT tied to VSYS there is no
    honest way to answer it, and a battery supply that claims a cell is fitted
    when none is would be worse than one that declines to say.

    poweroff goes through the PMIC's RTC BBPU latch, unlocked with the two key
    words and retired through the wrapper's busy flag.  Without this module,
    `poweroff' reaches the end of systemd's shutdown and halts the CPU with the
    rail still up: the panel goes dark and the board stays warm until somebody
    holds the power button.  Pass poweroff=0 to the module to keep the old
    behaviour.

    It only cuts the rail ON BATTERY.  VBAT is VSYS here, so a charger holds the
    system rail up no matter what the RTC is told; the driver asks eight times,
    reports the charger state, and then restarts rather than leave the board warm
    with mixdash last frame stuck on the glass.  chgreboot=0 keeps the halt.

    The charger is armed once per plug event, at the limit BC1.2 negotiated, and
    the constant voltage is only ever raised -- never lowered below the node,
    because lowering it on a board where VBAT is VSYS is a way to brown out the
    machine you are charging.  If it was given no pericfg/usb-phy pair in the
    device tree it cannot run BC1.2 at all and falls back to a conservative
    limit; that is a working charger, just a slow one.

    THE CHARGER USED TO BE DISARMED WHILE THE USB PORT WAS SOURCING 5 V, and that
    is the single biggest thing corrected here.  The driver was written believing
    CHRDET and the port's own boost were one net, so it read GPIO pad 15 every poll
    and held the charger off whenever the pad was up.  On this board the pad is up
    the whole time the board is on, and the value it zeroed is the same one that
    arms the charger -- so nothing charged, for the whole uptime, on every boot.
    CAPACITY falling with a charger in the wall is what that looked like.

    There are two connectors.  CHRDET is the DC inlet, the pad is the OTG port, and
    CHRDET decides charging by itself now.  chrin_shared=1 puts the interlock back
    for a board where the two really are one socket -- it is writable at runtime and
    the poll picks it up -- and it is 0 here and should stay 0.  The pad is still
    read every poll, still published through vbus_sourcing, and now says once in the
    log when a charger and a sourcing port are up together, which on this handheld
    is simply a console charging with a stick plugged in.

j36.power=nocharge
    Everything above except CHR_CON.  The gauge samples, the supplies appear, the
    plug edge is reported and poweroff still cuts the rail -- and the charger
    keeps exactly the settings the LK left it with.  Two cases want it: a board
    with no cell fitted, and any session where the question is whether a change
    in charging behaviour came from this driver.  /init translates it to charge=0
    on the insmod line, the same mechanism and the same reason as j36.usb=novbus.

    Deleting j36/power/ from the card is the harder version of the same thing and
    works from any machine that reads SD cards.  It is worth knowing that this is
    the only payload whose writes outlive a reboot: the MT6323 charger bank keeps
    its CV, its current-sense threshold and its enable bits across a warm reset,
    so a boot without the word starts from whatever the last boot with it left
    behind, not from a clean slate.

j36.wifi=1
    The radio, and wlan0.  Three modules -- cfg80211.ko, rfkill.ko and
    j36/wifi/j36_mt6592_wifi.ko -- loaded in that order after the power payload,
    and four stages between a cold MT6592 and an interface NetworkManager can
    use.

    WHAT IT DOES.  It powers the CONSYS block -- the MT6323's VCN28/VCN33 rails
    through the PMIC's wrapper, then MTCMOS, then the INFRA_CONNMCU clock, then
    the EMI mapping register that tells the MCU which megabyte of DRAM is its own
    -- and then talks to it.  The link is BTIF at 0x1100c000, a 4-wire UART with
    no pins, running MediaTek's STP framing with WMT commands inside it, and what
    goes down it is the pair of ROM patches in j36/wifi/firmware/.

    Then the third stage, over the WLAN AHB HIF at 0x180f0000: MediaTek's own
    WIFI_RAM_CODE_SOC, 257 KiB in 2 KiB chunks, each with a CRC-32 the boot ROM
    checks, then the MT6625L A-die probe, then WIFI_START, then WLAN_READY.  At
    that point the firmware is executing on the connectivity core.

    Then the fourth: the firmware's own command and event protocol -- scan,
    channel privilege, station record, BSS info, keys -- and cfg80211 on top of
    it.  The part is FULLMAC, which is the one fact that explains the shape of
    everything above: the firmware owns the MAC, so it beacons, ACKs, retries and
    does the CCMP itself, and mac80211 is deliberately not in this build because
    there is nothing here for it to do.  What is left for the driver is the join
    sequence and an Ethernet frame in each direction; the PSK and the four-way
    handshake belong to wpa_supplicant, exactly as on any other fullmac radio.

    The link is 2.4 GHz, WPA2-PSK with CCMP, up to 54 Mb/s.  No 802.11n: the
    association request does not advertise HT and the station record declares a
    non-QoS peer, and claiming it in one place and not the others is how a link
    comes up and then carries nothing.

    Reading the boot log is the whole test.  "wlan0 is up: chip 0x..., WLAN_READY"
    means all four stages passed.  Each of the other three lines names the stage
    that stopped: "WLAN firmware running: ... but no interface was registered"
    is stage 4, "connectivity MCU up: ... but the WLAN firmware did not start" is
    stage 3, and "Wi-Fi bring-up stopped at [stage]" is stages 1 and 2 -- the
    stage names are the driver's own, and consys-power, btif-link, rom-patch,
    wmt-handshake, wlan-firmware-missing, firmware-ready-timeout,
    wlan-basic-config-refused, hif-abnormal-interrupt and wlan-netdev-register
    are the ones worth grepping for.

    Two lines below that are worth reading even on success.  "A-die probe ran"
    means the RF front end was configured by the ROM; "timed out" or "ROM went
    silent" means the driver forced the probe's completion flag to get the
    firmware up, and a receiver left at reset values hears no beacons.  And
    "connectivity PC N changes in M samples" is what separates a firmware that is
    running from a core that stopped: zero changes is a stopped core, and the
    last PC is the address it stopped at.

    WHY IT IMPLIES j36.power.  The PMIC wrapper is one hardware state machine with
    one result register and no arbitration between users, so exactly one driver
    owns it: j36_mt6592_pmic, which lends the other two accessors.  Ask for
    j36.wifi without j36.power and /init turns j36.power on and says it did;
    j36.power=nocharge alongside it is honoured, since the charger is a separate
    question.  Load order follows from the same fact -- the radio probe defers
    until the PMIC has bound.

    WHERE THE FIRMWARE IS.  In j36/wifi/firmware/, with the driver, and not in the
    rootfs's /lib/firmware.  Two reasons, and the second is the hard one: the
    rootfs is shared with the R36S and nothing here writes to it, and the module
    is insmodded from the initramfs, seconds before switch_root, so /lib/firmware
    is not mounted yet whatever we put in it.  /init points the kernel's
    firmware_class path at the payload directory instead.  The consequence worth
    knowing: delete j36/wifi/ and the driver and its blobs leave together.

    The three files are MediaTek's, off this device's stock system image.  Two are
    the ROM patches, and their names lie about their order -- ROMv1_patch_1_1_hdr.bin
    is patch 1 and ROMv1_patch_1_0_hdr.bin is patch 2.  The driver reads the
    sequence out of byte 24 of each header and sorts on that, because sending them
    in filename order makes the MCU stop answering rather than complain.

    The third is WIFI_RAM_CODE_SOC, the WLAN firmware, and it has no extension at
    all -- that is the name the device's own /system/etc/firmware uses and so it is
    the name the driver asks the loader for.  Worth saying out loud because a *.bin
    staging glob cannot see it, which is exactly how it went missing from the card
    for a while: ROM patches present, the image they patch absent, and a radio that
    stopped one stage earlier than the log suggested.

j36.gl=1
    Stage j36/gl/ -- Debian's armhf Mesa -- into a tmpfs, so that a program looking
    for libEGL.so.1 finds it there instead of at the RK3326's Mali blob, which is
    what /usr/lib on the shared rootfs points those names at and which is an
    ARMv8-A object on this Cortex-A7.  Nothing on the shared rootfs is written; see
    below.  It needs j36.lima=1 to be any use, and j36.mtkdrm=1 as well before
    anything can reach the panel through DRM.

    The dashboard itself does not need this: it is Qt drawing with the CPU into
    /dev/fb0.  What needs it is the Diagnostics page's "GPU render test", which
    runs eglprobe -o: the cube on lima, read back and copied into /dev/fb0.  It
    used to run eglprobe -c, which scans the cube out through DRM instead, and
    that took the panel for the rest of the boot every time.  preserve_lk_state has
    since made that survivable -- see "Giving the panel back" below -- but -o is
    still the right test for a card that is only asked whether the GPU draws.

    j36.gl=debug adds Mesa's own EGL trace and runs the node probes before the
    dashboard starts, so the journal names every /dev/dri node and says which one
    can modeset.  eglprobe -f runs either way.

    It is not the default, and was: those node probes create EGL contexts on
    lima, EGL_LOG_LEVEL=debug turns each one into hundreds of libEGL lines on a
    640x480 console, and the run they buried was the dashboard's own error.  On
    this board a boot that did it repeatedly ended with the kernel not answering.
    Ask for it when the question is GL; the bars from eglprobe -f already say
    whether anything userspace draws can be seen at all.

j36.dash=1
    Run the MixOS dashboard as the shell.  mixdash.service is written into
    /run/systemd/system and wanted from multi-user.target through a symlink there.
    All of it is in tmpfs and none of it survives a reboot.

    The dashboard is not on this partition: /init looks for opt/mixos/bin/mixdash in
    the rootfs first and then on every other partition of the card, read-only.  Every
    partition it tries is named on the console -- as carrying no opt/mixos, or as
    unmountable, which is what a btrfs data partition looks like here because the
    initramfs carries no modules.  With nothing found it says so and starts nothing
    in its place.  There is no fallback front end to fall back to: the dashboard is
    the only shell this build has, and a board with no keyboard is better served by
    a console that says what is missing than by something else taking the panel.

    And because those lines are printed from the initramfs, where a hundred lines of
    kernel and systemd output push them off a 640x480 panel long before anybody reads
    them, a boot that finds nothing also gets mixdash-missing.service: it prints the
    reason, the inventory of partitions and the tar command that fixes it, six times,
    twenty seconds apart, after the boot has gone quiet.  A board with no keyboard has
    no other diagnostic interface.  If what you see instead is a console that simply
    stops -- typically at hostnamed deactivating -- then j36.dash=1 never reached
    /proc/cmdline, and /init says that too: without the word nothing is staged and
    whatever the rootfs starts by itself is what you get, which on this rootfs is a
    login prompt on a console nothing is plugged into.

    One more thing it arranges: /run/j36/card, which is where the dashboard's Files
    page opens.  That is not a convenience -- with no keyboard there is no way to
    mount anything by hand, and a file browser rooted in an empty directory is a file
    browser showing nothing.

    That path is a SYMLINK to /home/virtua, the login user's home -- a directory on
    the ROOTFS partition, which the initramfs has mounted before it makes the link.
    Nothing is mounted for it and nothing is probed.

    It used to be a probe, and a fairly long one: this card had a third partition,
    ext2 and labelled DATA, mounted at /home/virtua, and /init identified it by a
    .mixos-home stamp at its root -- it has no blkid to read a label with -- then
    unmounted its own probe and left the real mount to systemd, because a device
    cannot be held ro and mounted rw at the same time.  There is no third partition
    now; the space it used to occupy is given to ROOTFS on the first boot.

    A card written before this layout still works, and works through the same one
    line, because the link names a PATH and not a device: such a card mounts its p3
    at /home/virtua from its own fstab a few seconds later, and the Files page
    follows it there without knowing which layout it is on.

    Why a framebuffer dashboard rather than something on GL: everything that had
    been tried on the panel before it went through five layers that each fail
    silently -- SDL, KMSDRM, EGL, gbm, the driver -- and a black panel is what all
    five look like from the outside.  Qt on linuxfb has none of them.  It needs no
    context, no modeset and no DRM node, so the only thing between it and the glass
    is the memory LK already lit, and when it does not draw there is one place to
    look.  GL is still on the card and still measured, but it is now a card in the
    dashboard rather than the thing the boot depends on.

j36.splash=1
    The boot picture, and the boot stage written on it.  Both halves of it are in
    initrd.img and nothing on either partition of the card is needed for it:
    /splash.mixspl is resources/MixOS.jpg decoded to raw pixels at build time, and
    /bin/mixsplash is a static ARM binary that mmaps /dev/fb0 and draws.  Nothing
    is linked, because before switch_root there is no ld.so.

    It is not plymouth, and could not be: plymouth wants a DRM device or its fbdev
    renderer plus a theme, udev and a D-Bus name, and this initramfs is BusyBox and
    one shell script.  lima registers without DRIVER_MODESET, so there is no CRTC
    for anyone to take -- /dev/fb0 really is the only way to a pixel here.

    While it runs it holds the VT in KD_GRAPHICS, which is what stops fbcon
    painting over it, exactly as Doom above does.  /init still says everything it
    ever said; the panel shows the headline and the serial console gets the whole
    text, which is the reverse of the split before this existed.  The console is
    handed back in text mode if the boot fails, so a board that stops somewhere is
    still readable -- and NOT handed back after switch_root, because mixdash draws
    through Qt's linuxfb plugin with nographicsmodeswitch and wants the mode left
    where it is.  Everything /init printed is still in the VT's scrollback and
    reappears the moment the mode goes back to text.

    /init talks to it by appending lines to /dev/.mixsplash -- `stage:', `detail:',
    `progress:0-100', `handover', `quit', `abort'.  A file and not a pipe: `echo
    x > fifo' blocks until something reads it, so a splash that was not running
    would hang the boot at the next message, and this is a device whose only
    recovery is taking the card out.  /dev is `mount --move'd across switch_root,
    so the same path keeps working afterwards: j36-splash.service uses it to mark
    sysinit.target, and mixdash.service's ExecStartPre uses it to finish the bar
    and stop the splash a second before Qt paints.

    j36.splash=0 -- or `nosplash' -- boots to the text console instead.  So does a
    build where the binary or the picture failed to build: /init tests for both
    files, and for /dev/fb0, before it starts anything.

    ./build-j36-ultra.sh --no-splash writes that word for you, and raises loglevel
    to 7 in the same line, because the two go together: with the picture off the
    panel is the console, and a console showing only the four highest printk levels
    is a nearly blank screen where the splash used to be.  It is the switch to
    reach for when a board stops somewhere and the headline does not say where --
    the splash summarises, the text console does not summarise anything.  Editing
    this file on the card by hand does exactly the same thing, so a card built with
    the splash does not have to be rebuilt to lose it.

Doom, what it was for, and why it is no longer on the card
----------------------------------------------------------

It answered a question the boot itself does not: whether a program can drive this
panel and read this pad.  It did, the answer was yes, and the dashboard now
answers the same question every boot -- so J36_DOOM defaults to 0 and neither
j36/doom nor an IWAD is written to this partition.  Everything below is what a
J36_DOOM=1 build stages, kept because /init still runs it and because it is the
fastest way to split "the panel is broken" from "GL is broken".

Nothing else on the card could ask it at the time -- SDL2 here has no fbdev
backend, so anything SDL-based needs DRM/KMS or GL, this kernel had no DRM driver
bound yet, and the GL stack in the shared rootfs is the RK3326's Mali-G31 blob for
a GPU this SoC has not got.  doomgeneric needs none of that: it writes 32-bit
pixels into /dev/fb0, which is the framebuffer the MVII LK was already scanning out
when it jumped to the kernel, and reads /dev/input/event0 from
j36_mt6592_input.ko.

It runs from the initramfs, before switch_root, so it touches nothing on the
shared rootfs and competes with no systemd unit for the panel.  It takes the VT
into KD_GRAPHICS while it runs, which is what stops kernel messages painting
over the frame, and puts it back on the way out -- including out through a
signal, so a crash cannot leave the panel frozen on the last frame.

  D-pad, left stick   turn and walk        640x400 centred in the 640x480 panel,
  A                   fire                 which is a clean 2x of Doom's 320x200
  B                   use, open doors      with 40 black lines top and bottom
  X                   run
  Y                   automap
  L1, R1              strafe left, right
  L2, R2              weapons 3 and 4
  stick clicks        weapons 1 and 2
  START               enter (menu select)
  SELECT              escape (menu)
  VOL-, VOL+          smaller, larger view
  MENU                quit, and the boot continues

The WAD is Freedoom 0.13.0, which is freely redistributable; the engine has no
game data of its own.  Any IWAD works in its place, as long as the filename is
one doomgeneric's d_iwad.c recognises -- doom.wad, doom1.wad, doom2.wad,
freedoom1.wad, freedoom2.wad, freedm.wad, plutonia.wad, tnt.wad, chex.wad or
hacx.wad.  /init takes the first of those it finds in j36/.

There is no sound from Doom: doomgeneric is built with sound compiled out.  That
was honest about the kernel when it was written -- nothing drove the MT6592 audio
path at all -- and it is now merely a build option that was never revisited.  The
audio path is j36.audio=1 above, and it goes through ALSA, which this build of
doomgeneric was not linked against.

The Mali-450, and why a helper runs before the driver
-----------------------------------------------------

This SoC has a Mali-450 MP4 and DRM lima drives that generation.  The register
map in the device tree is not inferred from a datasheet: the stock kernel's own
`struct resource' array names all eighteen blocks -- GP at 0x13040000 with IRQ
234, its MMU at 0x13043000/235, four PPs at 0x13048000, 0x1304a000, 0x1304c000
and 0x1304e000 with 236/238/240/242, their MMUs at 0x13044000..0x13047000 with
237/239/241/243, both L2 caches, the broadcast and DLBU blocks -- and every offset
from that base agrees with lima's own mali450 table, and with the MALI_BASE the
MVII LK's bare-metal Utgard driver uses on this board.  Three sources, one map.

What is NOT already done for us is power.  MT6592 gates the MFG domain through
the SPM's MTCMOS, and nothing on this boot path un-gates it: the MVII LK can, but
only its own kernel ever calls that code, not the hand-off to Linux.  A read into
an unpowered MTK subsystem does not return garbage -- it stalls the bus, and the
watchdog reboots the board with nothing in any log.  A lima built into the kernel
would do that during probe, on every boot, before the console or Doom or anything
else this card is for.

So lima is a module and j36/mfgpower is the gate.  It powers the domain through
the SPM the way the LK does -- PWR_ON, PWR_ON_S, wait for both status bits, drop
the isolation cell and the clock disable, assert reset-not, bring the SRAM up and
wait for its ack, then open the SMI-common and MFG clock gates -- and then reads
the GP and PP version registers back.  It prints every value on the way, and it
exits non-zero unless a Mali-450 answered.  Only on zero does /init insmod the
modules named in j36/modules/load.order, in that order.  On failure it says so and
the boot carries on untouched.

If it works you get /dev/dri/renderD128 and no /dev/dri/card0.  That is not a
fault: lima is a render-only driver.  It rasterises; it does not own a display.

The MT6592 display, and why loading it changes nothing on screen
---------------------------------------------------------------

j36/mtkdrm/ is a DRM/KMS driver for this SoC's display path, which is the piece
lima cannot supply: lima rasterises and owns no display, so without this there is
a render node and nothing to present through.  It is mainline's mediatek-drm with
an MT6592 entry added, and it is mainline's because MT6592's display block IS the
mt2701/mt8173 generation -- that was checked register by register against the MVII
LK's own display code rather than assumed from the family name:

  - The DDP path the LK builds is OVL0 -> RDMA0 -> COLOR0 -> DSI0, which is the
    component list the patch adds.  BLS stays out of the mutex, as in the LK.
  - MUTEX0_MOD in the LK is 0x488.  mainline's mt2701 mutex table puts OVL0 at
    bit 3, COLOR0 at 7 and RDMA0 at 10: 0x488 exactly.  MUTEX0_SOF is 1 in both.
  - The MIPI-TX PLL arithmetic agrees to the quotient.  mainline computes
    pcw = (data_rate * 2 * txdiv << 24) / 26000000; the LK computes
    data_rate * txdiv / 13.  Same number, same txdiv ladder.  What decided
    mt2701 over mt8173 was one field: mppll_preserve is 3 in the mt2701 data and
    3 is what the LK writes to PLL_TOP bits 9:8.  mt8173's is 0.

The one measured divergence is the data rate: mtk_dsi computes 192 MHz where the
LK programmed 214, because it derives it from the pixel clock and no device-tree
or panel hook can override it.  It is left alone deliberately -- it lands on the
same txdiv, the porch budget has an order of magnitude of margin, and the refresh
rate comes out the same 62.5 Hz -- but it is the first thing to suspect if the
panel ever tears after a modeset.

Loading these modules is a no-op on screen, and that is by construction:

  - Every piece is a module, including mtk-mmsys and mtk-mutex.  Those two are
    `default ARCH_MEDIATEK' in Kconfig, so they were silently =y here and would
    have bound the new device-tree nodes on every boot; the build now forces them
    =m and fails if the assertion ever comes back the other way.  With j36.mtkdrm
    absent, nothing in this set is in memory.
  - DRM_FBDEV_EMULATION is off, so mediatek-drm registers no /dev/fb.  /dev/fb0
    stays simplefb's window onto the framebuffer the LK is scanning out.
  - mediatek-drm programs its first display register in atomic commit, which is to
    say when userspace opens card0 and sets a mode.  Until then it has enumerated
    hardware and touched none of it.
  - j36_fbmem, loaded first in this set, is display MEMORY and not display: it
    exports the LK carveout as a dma-buf and never ioremaps it, never reads a
    register and never touches the pipe.  See /dev/j36fb further down.

The panel module is out of tree and small, and it is worth knowing why it exists:
mtk_dsi calls component_add from inside mtk_dsi_host_attach, and host_attach only
runs when a mipi_dsi_driver has probed on the panel node.  No panel driver means no
DRM master and no card0, however correct everything else is.  All four of its power
callbacks are empty on purpose -- the LK has already powered the panel, released
reset, pushed 155 init records and lit the backlight -- and it refuses to probe
without j36,preserve-lk-state in the tree rather than pretend it can bring a dark
panel up.  Cold start is not implemented; the device tree keeps the init table,
the PMIC sequence and the GPIO sequence so that it can be.

Giving the panel back
---------------------

That empty cold start used to have a price, and it was the largest single fault on
this board: any program that opened card0 and set a mode spent the display for the
rest of the boot.  Not because the program did anything wrong -- because closing the
device is enough.  drm_fb_release() removes a client's framebuffers on close, taking
one out from under an active CRTC disables the CRTC, and mtk_crtc_atomic_disable()
then stops OVL, RDMA, COLOR and DSI, disconnects the mmsys routing and drops the
mutex.  With DRM_FBDEV_EMULATION=n there is no in-kernel client for
drm_client_dev_restore() to hand the pipe to afterwards, and with no cold start there
is nothing else that can light the panel.  The screen went black, /dev/fb0 went on
accepting writes nobody would ever see, and only a reboot fixed it.  The "vblank wait
timed out on crtc 0" WARN in dmesg was the same event from inside: the 100 ms wait
that lets disabled planes reach the hardware at the next SOF, on a pipe being torn
down underneath it.

preserve_lk_state, in linux/0002-drm-mediatek-mt6592.patch and set on this SoC's
mmsys driver data alone, makes that disable do nothing at all: no plane teardown, no
vblank wait, no mtk_crtc_ddp_hw_fini().  The DDP, the DSI and the panel stay exactly
as the bootloader left them, and the next client to arrive gets mtk_crtc_ddp_hw_init()
over a live pipe -- which is precisely what the FIRST modeset after boot has always
done, so it is a path that was already load-bearing.

Leaving the pipe up is only half of it.  The same commit that disables the CRTC also
turns every plane off, and an overlay with no layers scans out an opaque background
that nothing can paint over.  So mtk_disp_ovl keeps a copy of the programming it
finds at probe -- taken there because the first modeset's mtk_ovl_config() pulses
OVL_RST and wipes it -- and mtk_crtc_atomic_flush() writes that copy back, under the
DDP mutex, once the CRTC has gone inactive.  What comes back on screen is the
bootloader's own framebuffer at 0x82700000, which is the buffer simplefb, /dev/fb0,
the dashboard and eglprobe -f have been drawing into all along.  A snapshot is only
marked usable if the block really was running at probe, so this costs nothing on a
board that boots its own display.

What that buys: eglprobe -p, -c and -z can be run at any time and the dashboard is
back when they exit, and a Qt application on eglfs can start and stop as often as it
likes.  Running the dashboard itself on GL stops being a one-way decision.

Sound, and where it comes out
-----------------------------

j36/audio/ is the ALSA core plus one driver, j36_mt6592_audio.ko, and the driver is
ours: sound/soc/mediatek starts at MT2701, so there has never been an MT6592 audio
driver upstream, and the vendor path is an Android HAL talking to a kernel
interface this kernel does not have.  What the driver does have is the register
sequences, taken from the freestanding MVII driver written against this board --
PowerEngine OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mt6592_audio.c -- which in
turn distilled them from the reference HAL under External/MediaTek/Audio/MT6592.

It is a native ALSA card and not an ASoC machine driver.  ASoC is the right shape
for a SoC with a codec on a bus, and this is that, but the whole point of the
exercise is one measurement on one memif; snd_pcm_new plus five callbacks gets
there against an API that has not moved in years, where the 6.12 ASoC specifics
would have cost several rebuilds for the same result.

It runs with no interrupt, which is a deliberate choice and not a gap.  The AFE's
IRQ block is not in the reference material at all, and a period wakeup does not
need it: DL1's read cursor is a register, so a delayed work item polls it at 5 ms
and calls snd_pcm_period_elapsed, exactly as MVII paces its own playback.

Two things about it were open questions rather than code, and the first is now
answered.  The AFE functional clock: MVII has the ungate sequence -- two power-down
bits in TOPCKGEN CLK_CFG_3 and one in INFRACFG -- and compiles it out, so its audio
is soft-paced silence and nobody had ever seen AFE_DL1_CUR advance on this SoC.
This driver ungates, logs CLK_CFG_AUD before and after, and reports on the first
stream whether the cursor moved; it moves, and the DMA feeds the DAC.  What is
still not measured is the class-D speaker under load, and that is why it stays
behind its own command-line word -- see j36.audio=speaker above for the VBAT story.

THERE ARE TWO ANALOG OUTPUTS AND THEY SHARE ONE FRONT END.  AUDTOP_CON0/CON4/CON6
are the ABB downlink: the bias, the reference, and the input mux for the buffers
hanging off it.  The value the old speaker path wrote into CON6, 0xB7F6, shorts the
headphone inputs to ground -- which is what the vendor HAL does in its SPEAKER case
and exactly the wrong thing when the sound is meant to leave through the jack.  So
the driver no longer has a "speaker sequence"; it has j36_front_up(), which is
handed the pair (headphone, speaker) and programs CON6 and CON4 for one of the
three combinations the HAL knows -- 0xF5BA/0x007C for the jack, 0xB7F6/0x0014 for
the speaker alone -- and then j36_speaker_up() adds the class-D on top if it was
asked for.  Changing which outputs are live tears the front end down and rebuilds
it, because the mux is not a runtime switch.

NOTHING DETECTS A PLUG, so both outputs are switches and neither is a status.  The
MT6592 has ACCDET, but no register map for it exists in the reference material --
only the vendor HAL's ioctls on /dev/accdet, a kernel driver this tree does not
have -- and the J36 board header brings no HP-detect pin out.  "Headphone" is
therefore on by default (the buffers run off the reference the DAC already needs,
and there is no rail for them to pull down) and "Speaker Amp" stays opt-in.

Volume is analog on both.  "Master Playback Volume" writes the class-D level over
SPK_CON9 and the headphone buffer gain into AUDTOP_CON5, whose two 4-bit fields are
an 8-step -5..+9 dB attenuator -- the vendor's own HWgain[] table, which survives in
AudioMachineDevice.cpp only as a comment.  That is why /etc/asound.conf carries no
softvol: there is nothing left for software to attenuate.

One thing that is NOT related, because it looked like it was: the boot log's

  /usr/lib/udev/rules.d/90-alsa-restore.rules:1 GOTO="alsa_restore_std" has no
  matching label, ignoring.

comes out of the shared Debian rootfs and has nothing to do with any of this.  Its
first two lines are early-exit GOTOs whose LABEL is in a file udev is not reading
here; it was there when there was no ALSA core at all, and it is still there now.

Mesa, and what GL on this board can do
--------------------------------------

The dashboard needs no GL at all: it is Qt drawing with the CPU into /dev/fb0.  GL
is here for j36/eglprobe -- the display tests below, and the "GPU render test" row
on the Diagnostics page, which is the dashboard running eglprobe -o -- and for
anything added later that wants a context.  Four links had to close before any of that worked, each one measured
rather than assumed, and the list is worth keeping because it is also the fault
tree:

  1. A KMS device.  j36.mtkdrm=1 gives /dev/dri/card0 for the display, j36.lima=1
     gives /dev/dri/renderD128 for rendering.  Before this the panel was simplefb
     only -- a framebuffer, not a DRM device -- and nothing that wants a modeset
     can use one.
  2. A client that can drive it.  The SDL2 in the shared rootfs has KMSDRM,
     wayland, offscreen and dummy compiled in.  There is no fbdev backend, which
     is why /dev/fb0 is not a route to anything SDL-based, and why KMSDRM needs
     the card node step 1 produces.
  3. A Mesa that can pair the two.  This is why the display driver is
     mediatek-drm specifically: Debian's armhf Mesa 25.0.7 ships BOTH halves of
     the pair, dri/lima_dri.so for the renderer and dri/mediatek_dri.so for the
     kmsro display, so a GBM surface on an mtk_drm card is rendered by lima
     without anything having to be told.  simpledrm has no such entry, which is
     one reason it was never the answer -- the other being that it binds the same
     `simple-framebuffer' the working display is on and evicts simplefb.
  4. A GL front end that is Mesa's.  Debian's Mesa was in the rootfs all along;
     what was not was its front door.  The shared rootfs points libEGL.so,
     libgbm.so{,.1,.1.0.0} and libGLESv1_CM.so at the RK3326's Mali-G31 blob.
     Bifrost is a different architecture from this SoC's Utgard part, so that
     library cannot drive this GPU whatever else is true.  j36.gl=1 closes it:
     see below.

So there was no Mesa to cross-compile.  The whole of step 4 is five small
libraries -- about 1.4 MB, most of it glvnd's dispatch table -- and an environment.

Worth knowing before reading further, because it is the one that decides whether
any of this can work: libgbm.so.1 is a name the blob took over, and
libEGL_mesa.so.0 -- Mesa's own EGL vendor, the library /usr/share/glvnd/
egl_vendor.d/50_mesa.json names -- carries libgbm.so.1 in its DT_NEEDED.  So on
the rootfs as it stands, asking for EGL loads glvnd, glvnd loads Mesa's vendor,
and Mesa's vendor pulls the Mali-G31 blob in as its GBM.  Mesa's EGL cannot come
up on this board without the payload, whatever is asking for it.

Why the payload is a tmpfs
--------------------------

THE SHARED ROOTFS IS NOT WRITTEN.  That is the constraint everything here follows
from.  One Debian armhf rootfs serves two machines, and the R36S needs its
libEGL.so -> libMali.so symlinks to stay exactly where they are, because that blob
is the only thing that drives its Mali-G31.  Replacing them would trade this
board's GL for the other board's.

So with j36.gl=1 the initramfs, after it has mounted the rootfs and before it hands
over, mounts a tmpfs on the rootfs's /run and copies j36/gl/ into /run/j36/gl.
Anything that wants Mesa is run with LD_LIBRARY_PATH=/run/j36/gl, which puts the
payload's libEGL.so.1, libgbm.so.1 and libGLESv2.so.2 ahead of the rootfs's
symlinks to the blob.  Mounting /run from the initrd is the documented half of the
handover: PID 1 adopts a /run that is already a tmpfs instead of mounting another
over the top, which is also what keeps /run/j36 visible after systemd starts.

Pull the card into an R36S and none of it exists.  Nothing was written to the
filesystem, so there is nothing to undo.

And it is brought up once, on purpose, by j36-glwarm.service -- the same
eglprobe with no arguments, run after the dashboard has painted, output to the
journal only.  The first program on this board to want graphics otherwise pays
for the whole stack arriving off the card: the loader maps Mesa and everything
under it, lima opens its render node for the first time, and the config table
is built.  That is most of a second on this SoC, and it is charged to whatever
the operator just launched.  Running it where nobody is waiting moves the cost
off them; the probe exits and what it leaves behind is page cache, the lima
state and a shader cache under /run/j36/glcache.  Under j36.gl=debug the unit
is not written at all, because mixdash-probe.service has already done it twice
before the dashboard.

Which APIs come up, and which one does not
------------------------------------------

Measured with j36/eglprobe, per DRM node, and against a software control row:

  ES2 = ctx/cur "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1" on lima, current on an
        ARGB8888 window surface
  GL  = ctx/cur "4.5 (Compatibility Profile) Mesa 25.0.7-2+deb13u1" on swrast
  ES1 = 0x3003 EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike

The ES2 row clears the whole lower half of the stack in one measurement: gbm, the
kmsro mediatek-to-lima pairing, the config, the ARGB8888 visual, lima's own context
creation and the kernel uAPI all work.  ES2 is what to write against here.

The ES1 row is a property of the package and not of this SoC: Debian's armhf Mesa
is a -Dgles1=disabled build, so there is no fixed-function GL on this image at all
and no environment variable brings one back.  The error code says that before the
source does.  Mesa's validate_context_version rejects an API the screen does not
advertise with __DRI_CTX_ERROR_BAD_API, which surfaces as EGL_BAD_MATCH; BAD_ALLOC
is __DRI_CTX_ERROR_NO_MEMORY, a driver whose own context creation returned NULL.
Both ES1 rows are BAD_ALLOC, so the version gate passed and the versions are
advertised -- MESA_GL_VERSION_OVERRIDE has nothing to override and is not worth
trying.  Anything ported here that draws with glMatrixMode and glEnableClientState
has to be rewritten to shaders; it cannot be configured into working.

Three things about writing ES2 for this hardware, learned the expensive way:

  - Link no GL library.  Resolve entry points through eglGetProcAddress or
    SDL_GL_GetProcAddress instead.  Every unversioned GL name on this rootfs --
    libEGL.so, libGLESv2.so, libGLESv1_CM.so* -- is a symlink to an SONAME-less
    ARMv8-A libMali.so, so any -l against them records a DT_NEEDED that is SIGILL
    on a Cortex-A7.  With none of them named, one binary is correct on both
    machines.
  - Ask for ES2 explicitly, and set MAJOR then MINOR.  Setting
    SDL_GL_CONTEXT_MAJOR_VERSION twice is a common typo, and with major_version 0
    SDL sends no context attributes at all, so EGL falls back to its default of
    desktop OpenGL: a program that thought it asked for ES2 gets a compat context
    by accident, which works on swrast and nowhere else on this board.
  - Utgard fragment hardware has no branching, so one small program per case beats
    one program with a mode uniform; and an ALPHA texture has to follow the spec's
    MODULATE row (Cv = Cf, Av = Af * At) rather than multiplying the texel in,
    which a naive shader does and which renders every glyph black.

Two failure signatures are worth knowing on sight, because each one looks like a
bug in whatever printed it and is not.

  <program>: Illegal instruction
  <unit>.service: Main process exited, code=exited, status=132/n/a

Status 132 is 128+4, SIGILL, and it happens before main().  It means the process
loaded the RK3326 Mali blob: libMali.so is an ARMv8-A object (readelf -A says
Tag_CPU_arch: v8) and the MT6592 is a Cortex-A7, so the first instruction the blob
executes is one this SoC does not have.  SIGILL here is never a Mesa bug and never
the program's -- it means the GL payload did not take and the loader fell back to
/usr/lib.  Check for "gl: payload in /run/j36/gl" in the boot log and `ls
/run/j36/gl` on the device: if libEGL.so.1 is not in there, nothing else in this
section matters yet.

  terminate called after throwing an instance of 'std::logic_error'
    what():  basic_string: construction from null is not valid
  <unit>.service: Main process exited, code=exited, status=134/n/a

134 is 128+6, SIGABRT from an uncaught C++ exception, and it is NOT a missing
binary -- a missing binary is status 127 and a different message.  It is the shape
of every program that calls SDL_GL_CreateContext, does not check what came back,
and then reads glGetString(GL_EXTENSIONS): with no context current, glvnd's no-op
dispatch returns NULL and std::string(NULL) throws.  So the abort is a report that
eglCreateContext failed, and that the reason was discarded one line earlier.  Ask
eglprobe for the reason rather than reading the abort.

j36.gl=debug asks for it at boot instead: EGL_LOG_LEVEL=debug, MESA_DEBUG=1 and
LIBGL_DEBUG=verbose, plus the node probes before the dashboard starts.  Two things
in that trace are noise and not failures:

  libGL: Can't open configuration file /home/virtua/.drirc: No such file
  libEGL debug: No DRI config supports native format <name>          (repeatedly)

The .drirc line is Mesa looking for a tuning file that was never written, and the
"No DRI config supports native format" lines come from Mesa's own per-visual walk
at eglInitialize: libEGL_mesa.so.0 carries that literal next to dri2_create_context
and DRI2: failed to validate config, it prints a pipe-format name, and it says one
line for every format in Mesa's visual table that this driver pair does not expose.
A kmsro display device exposes few, so most of the table misses and the noise is
expected.

  libEGL debug: EGL user error <code> in dri2_create_context

That one is a failure, and the code is the whole message: BAD_MATCH is the DRI
layer rejecting the API or the version, BAD_ALLOC is the driver's own context
creation returning NULL.  Those have different fixes and look identical from
anywhere above EGL.

What the probe measures, and why it probes each node separately
---------------------------------------------------------------

j36/eglprobe prints, per DRM node: the EGL version and client APIs, the config
table summarised (how many configs carry each renderable bit, and how many window
configs are AR24 and XR24), then one row of GL / ES1 / ES2 each showing either a
created context taken all the way to glGetString on a current ARGB8888 window
surface, or the exact EGL error.

It probes /dev/dri/card0 and /dev/dri/renderD128 separately because those are two
different chips here -- mtk_drm displays, lima renders, Mesa bridges them with
kmsro -- so contexts on the render node but not the display node means the kmsro
pairing, and failures on both mean lima.  A second run adds a row with no DRM
device at all and LIBGL_ALWAYS_SOFTWARE=1, which is the control: swrast does
desktop GL, GLES1 and GLES2 on any machine, so a row that fails there is the
payload's fault and not the board's.  That control row is what found the ES1
result above -- the same failure on lima, on llvmpipe and on softpipe is not a
driver, it is a build option.

Black panel from something that draws through DRM
--------------------------------------------------

Nothing in the boot depends on this any more -- the dashboard is on /dev/fb0 -- but
it is the trap anything GL-based here falls into, so it is worth writing down.
Black is the one symptom that three unrelated faults share, and guessing between
them costs a boot each:

  1. nothing is drawn -- a program that did not link, or a projection that puts
     pixel coordinates outside the frustum;
  2. nothing is asked for -- an empty scene, a resource that did not load;
  3. everything is drawn correctly and the buffers never reach the panel -- the
     format chosen for the gbm surface, the connector or the CRTC picked, or a
     page flip the display driver refused.

They separate cheaply and in that order.  Draw one quad straight in NDC with both
matrices at identity and read it back with glReadPixels: a correct centre pixel
retires fault 1 outright, and black there, or a shader link error just above it,
IS fault 1 and the log says which line of GLSL.  Count draws per frame: a frame
with zero draws is fault 2, and no shader will change that.  Clear to a colour
nothing else uses: if the panel takes the clear colour then the buffers being
swapped are the buffers being scanned out, fault 3 is retired, and anything still
black on top of it is the program's own drawing.

One candidate for fault 3 has to be withdrawn before it costs a boot.  Asking for
SDL_GL_ALPHA_SIZE 8 reads like the cause of an ARGB8888 framebuffer a plane might
refuse -- but SDL is not choosing: KMSDRM_CreateSurfaces hardcodes
GBM_FORMAT_ARGB8888 for its gbm surface (SDL_kmsdrmvideo.c:1197) and then pins that
visual with SDL_EGL_SetRequiredVisualId whatever was asked for.  Dropping
ALPHA_SIZE to 0 changes which EGL config is chosen and not one byte of the buffer
that is scanned out, so it is not the experiment it looks like.  What remains for
fault 3 is fbcon -- a console released without the CRTC handed over leaves the
panel scanning out nothing while the program renders perfectly into buffers nobody
reads -- and the two things a program's own log cannot see: whether a modeset
reaches the glass at all, and whether the OVL blends per-pixel alpha.  eglprobe -f
and -p below answer both without a DRM node in the way.

j36/eglprobe -f, and the colour bars
------------------------------------

Before any of that, the cheap question: is the panel in the fault at all?  -f is
the only mode here that opens no DRM node.  It opens /dev/fb0 -- simple-framebuffer
over the memory the LK lit, which is also exactly what the dashboard draws through
-- and it answers in three parts.

First a census, taken before it writes anything: how many of 4096 sampled pixels
carry colour.  All black means nothing has drawn into the framebuffer and the
display path is not implicated at all; a picture in memory with a black panel in
front of it means the opposite, that something took the scanout away from
simple-framebuffer, and -i then names the node it would have been.

Then the two states that are black on purpose and are one write to undo: a backlight
at brightness 0, and /dev/tty0 left in KD_GRAPHICS by an SDL or a Qt that died
without restoring it.  Both are reported; both are undone.  Either one is a working
display showing nothing, and neither is distinguishable by eye from a dead panel.

Then eight colour bars, a one-pixel white border and a corner-to-corner diagonal,
written with the CPU.  The border and the diagonal are not decoration: a wrong
line_length shears a vertical bar into a diagonal one and slides the border off the
edge, which is the one display fault that looks like working hardware.

It runs from mixdash-probe.service, once per boot, for one second, and that makes it
a handover signal.  Once per BOOT and not once per start attempt: as mixdash's own
ExecStartPre it re-ran on every restart, so a dashboard that could not start painted
the bars over its own error message three times before systemd gave up -- and under
j36.gl=debug it re-initialised lima three times with it.

  bars, then the dashboard    both halves work
  bars that stay              mixdash never started -- read the journal, not the
                              display stack
  no bars at all              nothing userspace draws will ever be seen, and the
                              dashboard is not the thing to debug

Nothing in -f has to be given back, which is what makes it safe to run at boot.
That is not true of what follows.

j36/eglprobe -p, and the five colours
-------------------------------------

Everything in the probe's other modes asks whether a context can be built; -p
paints, and it takes the client, then SDL, then GL, then gbm out of the path one
step at a time, holding each frame three seconds because the instrument for this
one is an eye.  It speaks DRM with raw ioctls -- the uapi structs are ABI and libdrm would be
a fourth library that can be missing -- and prints the connector, the mode it was
given, the CRTC and whatever framebuffer was already on it.

It is NOT run at boot and it REFUSES to run without -y, because for the fifteen
seconds it is up it owns the panel: it sets a mode of its own and whatever was on
screen -- the dashboard, a video -- is gone until it exits.  That is the whole cost
now, and it used to be very much more.

A DRM client that sets a mode and exits used to leave the panel black for good.  On
close the kernel runs drm_fb_release() over that client's framebuffers, and removing
the framebuffer a CRTC is scanning out disables the CRTC; this kernel is built
CONFIG_DRM_FBDEV_EMULATION=n on purpose, so there was no in-kernel fbdev client for
drm_client_dev_restore() to hand the pipe back to, and no cold start anywhere here to
light the panel again.  -p and -c therefore held the panel until the next reboot,
and /dev/fb0 kept accepting writes that were no longer seen.

preserve_lk_state in linux/0002-drm-mediatek-mt6592.patch ends that -- the CRTC
disable leaves the pipe running and the overlay is put back the way the bootloader
programmed it, so /dev/fb0 is live again the moment the client exits.  See "Giving
the panel back" above.  Run -f afterwards if you want the numbers back on screen;
you no longer have to run it first.

The other half of the old warning was that -p and -c failed at display_node() when
mediatek-drm did not bind, harmlessly, which is why the dashboard was once allowed
to run -c behind a confirmation.  They succeed now.  `eglprobe -p' on its own prints
what it would cost and what to run instead; `eglprobe -p -y' runs it.

  1  RED, XR24, filled with memset()          modeset + DSI + panel + OVL, no
                                              alpha and no Mesa anywhere
  2  MAGENTA, AR24 alpha ff, memset()         the same, in the format SDL uses
  3  MAGENTA, AR24 alpha 00, memset()         the same buffer, transparent
  4  MAGENTA, lima into a gbm surface         a GL client's path with the client,
                                              SDL and the renderer removed
  5  GREEN, lima's second frame               the swap chain rotating

Read it as four verdicts:

  nothing at all           the modeset does not reach the glass, and no part of
                           EGL is involved.  Look at mtk_dsi against the state the
                           LK left, and at the mode the panel driver reports:
                           j36_jd9365_panel.c adopts a live panel and sends it no
                           init table, so a DSI re-initialised to a different
                           timing has nothing that puts it back.
  1 and 2 but not 3        the OVL blends per-pixel alpha against a black
                           background.  That alone explains a black panel under a
                           GL client, and the fix is in the client, not the
                           kernel: a renderer that clears to (0, 0, 0, 0) makes
                           every pixel it does not overdraw transparent black.
  1, 2 and 3 but not 4/5   the display path is sound and the gbm/kmsro pairing is
                           the fault: lima renders into a buffer the OVL never
                           fetches.  The ADDFB2 line names the handle and stride
                           it refused.
  all five                 the display path is sound end to end, so a black panel
                           is the client's own drawing -- fault 1 or 2 from the
                           section above, and a self test and a per-frame draw
                           count are the evidence.

`/run/j36/eglprobe -p -y' by hand does the same thing from a console at any time,
and it keeps the panel afterwards for the reason above -- reboot when it is done.

j36/eglprobe -o, and the cube that costs nothing
------------------------------------------------

-o draws the cube -c draws -- same two shaders, same 36 vertices, same lima -- and
lands it somewhere else.  It opens /dev/dri/renderD128, renders into a
renderbuffer-backed FBO at the panel's size, reads the pixels back with
glReadPixels and stores them into /dev/fb0 with the CPU, converting to whatever
the fb's bpp and colour offsets say.  No modesetting node is opened at all, so
there is no CRTC to take and nothing to hand back; when it exits the dashboard
repaints over it and the panel is exactly as it was.

The readback is the price, and -o prints the split -- GL, read, blit -- per run so
it is visible.  On this board it is the readback and not the drawing that sets the
frame rate, which is itself the finding: it is the number a zero-copy path has to
beat, and the reason /dev/j36fb and -z below exist.

This is what the Diagnostics page's "GPU render test" row runs, and what anything
that just wants to see whether the GPU draws should run.  `eglprobe -o 20' for
twenty seconds; `eglprobe -o 20 /dev/dri/card1' to force a particular node.

j36/eglprobe -z, and the same cube with no copies at all
--------------------------------------------------------

-z is -o with the two middle steps deleted.  It opens /dev/j36fb, asks it for a
dma-buf over the LK carveout, imports that into EGL as an EGLImage
(EGL_LINUX_DMA_BUF_EXT), hangs it off an FBO -- as a renderbuffer if the driver
takes one, otherwise as a texture -- and draws the cube directly into the memory
the DDP is scanning.  There is no glReadPixels and no fb_store: the frame is on
the glass because lima wrote it where the display was already looking.

So -z prints one number where -o prints three, and the pair is the measurement:

  offload: 300 frames in 20.0s, 15.0 fps -- gl 8.1 ms, read 41.2 ms, blit 17.9 ms
  scanout: 900 frames in 20.0s, 45.0 fps -- gl 8.1 ms per frame, and that is the
           whole cost: no read, no blit

The `gl' figure should be the same in both, because it is the same cube on the
same GPU.  Everything else was copying.

Two things it deliberately does not do.  It never opens a modesetting node and it
never programs a display register, so like -o it cannot black the panel -- the
LK's pipe is still running and this only changes what is in the buffer it reads.
And it is single-buffered, so it tears: there is one carveout, the display does
not wait for the draw, and adding a second buffer means a flip, which means owning
the CRTC.  That is the same bargain /dev/fb0 makes, and every frame this board has
ever painted has torn the same way.

It needs the EGL_EXT_image_dma_buf_import extension, which it checks for and names
if it is missing, and it needs /dev/j36fb, which needs j36.mtkdrm=1 in boot.conf.
`eglprobe -z 20' for twenty seconds; `eglprobe -z 20 /dev/dri/card1' to force a
node.

/dev/j36fb, the LK framebuffer as a dma-buf
-------------------------------------------

j36_fbmem.ko is ours and it is fourteen registers short of trivial, because it
programs nothing: it exports the carveout the LK left at 0x82700000 -- 640x480,
32 bpp, 2560-byte pitch -- as a dma-buf, and that is the whole driver.  It is the
first module in j36/mtkdrm/ and it has no dependency on the rest of the set, so
/dev/j36fb appears even when the DSI chain under it does not bind.

Why a driver at all, when the address is a constant and /dev/mem is right there:
lima imports dma-bufs, not addresses.  There is no interface that hands EGL a
physical address, and there is no reason to invent one when dma_buf_export is
eleven lines.

The one thing worth knowing about the implementation is that the sg_table it
hands out carries DMA ADDRESSES AND NO PAGES.  The carveout is `no-map' in the
device tree, and on ARM32 that means memblock never mapped it, pfn_valid() is
false over it and there are no struct pages to put in an sg_table at all.  So
j36fb_map_dma_buf builds a one-entry table with sg_dma_address/sg_dma_len set by
hand and the page pointer left NULL.  That is exactly what lima wants --
lima_vm.c walks an imported sgt with for_each_sgtable_dma_page and
sg_page_iter_dma_address, and never asks for a page -- and there is no IOMMU and
no dma-ranges between the GPU and DRAM, so the bus address is the physical one.
An importer that does want pages will fail on this buffer, and that is honest.

`no-map' has to stay, incidentally, and not only for this: ARM32's ioremap
refuses memory that is in the linear map, so dropping no-map would break
simplefb's own ioremap_wc and take /dev/fb0 with it.

Two device-tree nodes therefore point at one carveout -- simple-framebuffer for
/dev/fb0 and this for the dma-buf -- which is correct rather than a conflict.
Neither owns the memory, neither modesets, and the LK's pipe is what makes either
of them visible.

  cat /sys/class/misc/j36fb/info
  phys=0x82700000 size=1228800 640x480 stride=2560 bpp=32 fourcc=XR24

The ioctls are in j36_fbmem.c and there are two: J36FB_IOC_INFO fills that same
geometry into a struct, and J36FB_IOC_EXPORT returns a dma-buf fd.  The character
device also mmaps, write-combined, for a client that wants the CPU view without
going through /dev/fb0.

Films on the GPU, which is -z with a film in place of the cube
--------------------------------------------------------------

-z proved a zero-copy path and then stopped, because a cube is not the thing
anybody was waiting on.  The media player uses the same path now:
tools/mixdash/glvideo.cpp is -z's import, FBO and absence of a modeset, with three
planes and a colour-conversion shader where the cube was.

What it deletes is worth counting, because none of it was ever the decode.  A
640x480 frame used to be moved six times: swscale turned the decoder's yuv420p
into bgra (reads 460 KB, writes 1.2 MB), ffmpeg wrote that down a pipe, mixdash
read it back, QImage::copy took it out of the ring buffer, QPainter::drawImage put
it in Qt's backing store, and linuxfb memcpy'd that into /dev/fb0.  Seven and a
half megabytes per frame at 25 fps, on a Cortex-A7 with LPDDR2.  On the GPU path
ffmpeg emits yuv420p -- the form the decoder already produced -- so the pipe
carries 460 KB instead of 1.2 MB, and everything after it is one upload and a GPU
pass into the memory the DDP is scanning.

The transport strip along the foot is drawn by the GPU too, which looks like scope
creep and is not.  Qt's linuxfb backend presents by memcpy'ing the dirty rectangle
of its backing store into /dev/fb0, and that backing store has never heard of a
frame lima put there, so any Qt repaint over the picture erases it -- and the clock
in that strip changes once a second.  So while a film is up the page belongs to the
GPU: mixdash re-renders the strip only when its TEXT changes, hands it over as an
ARGB image, and the same pass blends it over the picture.  MediaPage::paintEvent
draws nothing at all in that state.

Nothing else in the dashboard changes.  Every other page is still rasterised by Qt
into /dev/fb0, which is the right trade for a UI that repaints when something
happens and the wrong one only for a surface that repaints 25 times a second.

It fails soft, in three places and all the way to the old path:

  - no libEGL, no libgbm, no render node, no /dev/j36fb, no
    EGL_EXT_image_dma_buf_import, or a shader that will not link -- the player
    asks ffmpeg for bgra as before and nothing is different but the frame rate
  - the window is not the whole framebuffer (the HDMI mirror is a separate
    process, so this is a guard rather than a limitation)
  - the driver stops answering mid-film, in which case ffmpeg is restarted from
    the position it had reached, on the software path, with a note saying so

Mesa is dlopen'd and never linked, exactly as eglprobe does it, so mixdash still
has no GL in its DT_NEEDED and still starts on a card with no GL payload staged,
with j36.gl=0, or on a board where lima refused to bind.  The reason -- which step
came up or which one refused -- goes into the MixOS log the first time anything
asks, and is the "GL video" row on the Diagnostics page.  Building it is what
answers the question, so opening that page is what puts the line in the log; it
draws nothing and cannot disturb what is on the screen.

What this does NOT fix is the decode itself.  MT6592 has no mainline hardware
video decoder -- mtk-vcodec starts at MT8173 -- so libavcodec is on the CPU
whatever the GPU does here, and a 1080p source is still a 1080p source.  What
changed is that the cost is now the decode and not the plumbing around it.

Reading the dashboard's startup trace
-------------------------------------

The panel IS the console: the bootargs are `console=ttyS0,115200n8 console=tty0'
and the last console= is the one /dev/console becomes.  That is why the boot is
readable on the glass at all, and it is also the trap this dashboard fell into.
Qt's linuxfb plugin, from inside the QApplication constructor, opens /dev/tty0 and
sets KD_GRAPHICS -- and fbcon draws nothing whatever in KD_GRAPHICS.  A dashboard
that then failed to reach a frame left a panel frozen mid-boot with no dashboard on
it and no way to ask why: every message after that point, kernel printk included,
went only to a journal on an ext2 partition that the machine which flashed the card
cannot mount.

So the mode switch is mixdash's now, taken at its FIRST PAINT and not before, and
until then the console stays live and mixdash announces every phase into it.  Two
lines come before the phases and they are worth reading first:

  mixdash: build 4f2a1c9b03de
  mixdash: the boot image expected 4f2a1c9b03de, so both halves are this build

This card is updated across two partitions by two different means -- the boot image
is dragged onto vfat BOOT, which is the only partition a Mac can write, while
/opt/mixos sits on an ext2 partition a Mac cannot even mount and arrives either baked
into the image or unpacked from sd-root.tar.gz by /init.  Either half can be a version
behind with nothing to show it, and an old dashboard failing in an old way reads
exactly like a new one failing.  Both ids are the first twelve characters of a sha256
over the dashboard's sources, so MISMATCH means one half was not updated: if the boot
image is the newer one the tarball never unpacked (look for /init's `stage:' lines
further up), and if the binary is the newer one the boot image on BOOT is stale.
Either way the trace below it is not evidence about whatever was last edited.

Those two lines are also reprinted at the bottom of every failure report, and that is
not redundancy.  This panel is thirty lines tall and a crash is thirty lines down, so
the first bad_alloc off this board was read for a day as evidence about sources that
were never on the card.  A photograph of a failure has to say which build it is of.

  [   0.01s] framebuffer      /dev/fb0's id, geometry, bpp, stride and channels
  [   0.05s] QApplication     loads libqlinuxfb.so and opens /dev/fb0; the line
                              after it is Qt's version, the platform NAME Qt
                              actually chose and the screen size it chose
  [   0.9?s] fonts            QFontDatabase, and fontconfig behind it.  The one
                              phase that can legitimately take minutes on a cold
                              card, which is why FONTCONFIG_FILE confines it
  [   ?    ] Dashboard        four pages, the dock, and a QFileSystemModel on
                              /run/j36/card -- a read-only mount of this card
                              . StatusBar / CardGrid / FilesPage / InfoPage /
                                Dock / toast / connections / dock pages /
                                buildPages / Joypad / setPage(0)
  [   ?    ] show             fullscreen, and then the first frame
  [   ?    ] first paint      the console is now KD_GRAPHICS, and this is the last
                              line that will ever be seen on the panel

The last line on the glass names the phase that did not finish, and the indented
`. ' lines under Dashboard name the object.  They exist because that phase aborted
once and naming eleven constructors at once named none of them.

There is a 90 s deadline on each phase and each step (MIXDASH_DEADLINE=<seconds> in
the unit, 0 to disable): when it fires, mixdash puts the console back into KD_TEXT,
prints WATCHDOG and the phase name, and exits 3.  An exception that reaches the top
of main -- std::bad_alloc, most likely -- prints what() with the phase and step and
exits 4.  RestartPreventExitStatus=3 4 stops systemd restarting into either one and
scrolling the verdict away.  Every fatal signal does the same restore before
re-raising, so a Qt abort cannot take the board's console with it either.

EVERY THROW is reported where it happens, before any unwinding, on whatever thread it
happened on -- which is the only place the frames that threw still exist:

  mixdash: THROW St9bad_alloc in phase "Dashboard -- ...", step "FilesPage: QListView"
  mixdash: backtrace:
           /opt/mixos/qt/lib/libQt5Core.so.5(_Z9qBadAllocv+0x1c) [0xb6a...]
           ...

The type is printed mangled because demangling allocates: St9bad_alloc is
std::bad_alloc.  This exists because Qt's containers never touch operator new --
QArrayData::allocate calls ::malloc through qMallocAligned and the caller's
Q_CHECK_PTR throws from inside libQt5Core -- so a QString or QVector that cannot be
allocated produces a bad_alloc the replaced operator new below never sees.

An allocation that fails THROUGH operator new prints more still, because std::bad_alloc
carries nothing -- no size, no address, no stack:

  mixdash: OUT OF MEMORY asking for 4294967232 bytes (4194303 KiB) in phase
           "Dashboard -- four pages, the dock and the evdev map", step "FilesPage:
           setRootPath -- starts the gatherer thread on the card"
  mixdash:   VmSize:   123456 kB
  mixdash: backtrace:
           /opt/mixos/bin/mixdash(_Znwj+0x5c) [0xb6f0]
           ...

The size is the whole question.  This board has 1 GB and the process is 32-bit with
~3 GB of address space, so malloc does not return null a quarter of a second into a
boot unless it was asked for something absurd: a seven-digit size means memory
really ran out and the leak is upstream, a ten-digit one means a length was computed
negative and widened into a size_t, and the backtrace names the frame that did it.
Any single allocation of 8 MiB or more is printed the same way even when it succeeds
(the first eight of them), because a 640x480 dashboard has no honest reason to ask
for one and the request before the fatal one is usually the same code path getting
away with it.

Rebooting and powering off
--------------------------

`reboot' works from here on: the device tree describes the TOPRGU watchdog at
0x10007000, and mtk_wdt registers the restart handler that machine_restart()
calls.  Without that node userspace shuts down cleanly and then prints "Reboot
failed -- System halted", which is a halt, not a crash -- the card is safe to
pull at that point.

`poweroff' goes through the PMIC module, which pulls PWRBB low through the RTC
BBPU latch.  UNPLUG THE CHARGER FIRST.  There is no power-path FET on this PMIC
family, so VBAT is VSYS: with a cable in, VBUS holds the system rail up and no
amount of writing to the RTC can take the board down.  The driver asks eight
times, then -- because the alternative is halting the CPU with the rail up and
mixdash last frame frozen on a panel nobody can take back -- restarts the board
instead, and says why in the log.  So a power-off attempted on the charger looks
like a reboot.  That is the hardware, not the driver; stock Android answers the
same case the same way and lands in a loader that draws a charging animation,
which this board does not have.  chgreboot=0 on the module line restores the
halt.  On battery, `poweroff' cuts the rail.

Licence
-------

See LICENSE.txt beside this file.  In short: the MixOS bring-up work is dual
licensed, MPL-2.0 or GPL-2.0-or-later at your option; the seven MixOS kernel
modules and the kernel itself are GPL-2.0-only; and everything else on the card
is Debian's under its own terms.  The operating system underneath is Debian, and
MixOS is a device port on top of it -- the build scripts descend from dArkOS,
which continues ArkOS, and nothing else here does.  MixOS supports the MediaTek
line of processors, and this card is that support.
README

# MPL-2.0 section 3.2 and GPLv2 section 3 are the reason this is written and not just
# linked from the repository: a card handed to somebody else is a distribution, and a
# binary distribution has to tell its recipient what the terms are and where the source
# is.  It is not added to SHA256SUMS, for the same reason README.txt is not -- the sums
# cover what the machine executes.
#
# The mapping below is written here because it is specific to this card; the two full
# licence texts are appended from the repository just after the heredoc, so there is
# one copy of each in the tree and no chance of the card's copy drifting from it.
cat > "$SDBOOT/LICENSE.txt" <<'LICENCE'
MixOS -- J36 Ultra (MediaTek MT6592, ARMv7) SD card payload
Copyright (c) 2025-2026 the MixOS project and contributors

MixOS supports the MediaTek line of processors.  This card is that support: a
32-bit ARM kernel, an mtk_drm display path, mtk-sd storage, the MT6592 AFE, a
keypad adapter and Mesa's lima/kmsro pair, on a Cortex-A7 from 2013.

This file sits on the FAT32 BOOT partition, which is where a card is opened, but
it covers everything MixOS put on the card.  Two partitions carry it, and BOOT
is FAT because the MVII LK reads FAT32 and nothing else:

    BOOT, FAT32   the launcher, and only that: zImage, mt6592-j36-ultra.dtb,
                  initrd.img, mvii/boot.conf, README.txt and this file.
    ROOTFS, ext2  Debian, and MixOS's own tree at /opt/mixos -- unpacked there
                  from sd-root.tar.gz.  Every "bin/", "qt/" and "j36/" path below
                  means /opt/mixos/... on this partition.  Your home directory,
                  /home/virtua, is a directory on it: a shell starts there, the
                  dashboard's Files page opens there, and nothing MixOS ships is
                  licensed by this file inside it -- what is in it is yours.
                  ROOTFS grows to fill the card on the first boot, so the space
                  that is yours is whatever the card has.

This payload is not licensed uniformly.  Saying otherwise would be a false
statement about other people's code.

MPL-2.0 OR GPL-2.0-or-later -- take either one, at your option.  Both texts are
appended in full at the end of this file:

    bin/mixdash             the MixOS dashboard (Qt5 Widgets on linuxfb)
    bin/j36-mixmirror       the panel-to-USB-HDMI mirror
    j36/eglprobe            the EGL/GBM/DRM scanout probe
    j36/mfgpower            the MFG power-domain bring-up probe
    mvii/boot.conf          the MVII LK hand-off
    README.txt on either partition, and this file -- the documentation

GNU General Public License, version 2 only:

    zImage                  Linux 6.12 LTS, plus two MixOS patches (mtk-sd and
                            drm/mediatek for MT6592)
    initrd.img              busybox and a shell /init
    j36/modules/*.ko        lima and its dependencies -- kernel modules
    j36/mtkdrm/*.ko         mtk_drm, and MixOS's j36_jd9365_panel and j36_fbmem
    j36/audio/*.ko          the ALSA core, and MixOS's j36_mt6592_audio
    j36/usb/*.ko            musb and its MediaTek glue, usbhid, udl, the SCSI and
                            mass-storage set, ntfs3, and MixOS's
                            j36_mt6592_usb_phy
    j36/power/*.ko          MixOS's j36_mt6592_pmic and j36_mt6592_backlight
    j36/wifi/*.ko           cfg80211 and rfkill, plus MixOS's j36_mt6592_wifi --
                            the CONSYS MCU's power, the BTIF link, the ROM patch
                            and WLAN firmware downloads over the AHB HIF, and
                            wlan0 on top of them
    j36_mt6592_input.ko     MixOS's keypad and GPIO key adapter

    The seven MixOS modules are GPL-2.0-ONLY deliberately, and are not part of
    the dual grant above: they derive from and link against GPL-2.0-only kernel
    internals, which is narrower than either half of it.  Code may move from the
    dual-licensed list into them; it may not move back out.

Their own terms:

    j36/wifi/firmware/      MediaTek's connectivity ROM patches and WLAN
                            firmware, redistributed unmodified as this device
                            shipped them; MediaTek's terms, not MixOS's, and not
                            GPL
    j36/gl/*.so*            Mesa, from Debian (MIT and others)
    qt/lib, qt/plugins      Qt 5.15 and its runtime closure, from Debian: LGPL-3
                            with Qt's own exceptions, and GPL/LGPL/MIT/others for
                            the libraries it needs
    qt/fonts                DejaVu Sans, under the Bitstream Vera licence
    bin/doom, share/doom    doomgeneric and Doom's engine source, id Software's
                            under the GPL, as Debian and doomgeneric ship them
    the rootfs on ROOTFS    Debian.  Per-package terms are in
                            /usr/share/doc/*/copyright on the running device.

SOURCE.  This is the written offer both licences ask for, and it is a short one
because everything here is built from public source.  Linux 6.12 LTS comes from
kernel.org with the two patches in the MixOS repository under
device/j36-ultra/linux/; the seven MixOS modules are in the same directory; and
busybox, Mesa, Qt, the fonts and doomgeneric are as Debian packages them.  The
dual-licensed MixOS work is in the same repository: the dashboard in
device/j36-ultra/tools/mixdash, and eglprobe, mfgpower and j36-mixmirror beside
it.  The MixOS build script that assembled this card is
device/j36-ultra/build-in-vm.sh, and it is the complete recipe -- nothing here was
produced by hand.

WHY TWO LICENCES FOR THE MIXOS WORK.  Commercial use had to stay permitted, the
source had to stay under a copyleft that returns fixes, and it all had to be
combinable with the kernel it drives.  MPL-2.0 is file-level copyleft that
section 3.3 lets you combine with proprietary code in a Larger Work, and sections
1.12 and 3.3 name the GPL family as Secondary Licenses, so MPL-2.0 code may be
redistributed under the GPL when it meets GPL code.  Offering GPL-2.0-or-later
outright alongside it removes the last of the ambiguity.  This work was
previously under the Microsoft Public License, which is not GPL-compatible;
copies obtained under Ms-PL stay under Ms-PL, and from that revision on the terms
are the dual grant.

ATTRIBUTION.  The operating system this device runs is Debian, and MixOS is a
device port on top of it: a second SoC vendor and a 32-bit kernel added to a build
pipeline that assumed neither.  That pipeline is the one thing here that descends
from dArkOS, itself a Debian-based continuation of ArkOS by christianhaitian, and
it keeps their copyright and their MIT licence; nothing MixOS runs comes from
either any more.  Neither dArkOS nor ArkOS endorses MixOS, is affiliated with it,
or should receive its bug reports.  Thanks are owed to the Debian project above
all, and to ArkOS and dArkOS, to MediaTek's documentation and vendor sources, to
Mesa for lima and kmsro, and to the Linux kernel, Qt, SDL and busybox projects.
Neither half of the dual grant conveys any right in a trademark, and every
trademark named here belongs to its owner.  MixOS is not affiliated with or
endorsed by the Debian project, MediaTek, the Mozilla Foundation or the Qt
Company.


The full text of both licences follows, appended to this file when the card was
built: the Mozilla Public License 2.0 first, then the GNU General Public License
version 2.  Either one covers the MixOS work listed above; you choose.
LICENCE

# The two texts themselves, appended verbatim from the repository rather than inlined
# in this script.  One copy of each in the tree is the point: the card cannot end up
# quoting a licence that differs from the one the source is offered under, and seven
# hundred lines of legal text stay out of a build script.
#
# A missing file is a warning and not a failure.  A partial checkout still produces a
# bootable card, the notice above already names both licences and says where to get
# them, and refusing to write an SD card over a documentation file would be the wrong
# trade in every direction.
for licence_text in "$ROOT/device/j36-ultra/LICENSE.MPL-2.0" \
                    "$ROOT/device/j36-ultra/LICENSE.GPL-2"; do
    if [[ -f "$licence_text" ]]; then
        {
            printf '\n\n'
            printf '%s\n\n' '================================================================================'
            cat "$licence_text"
        } >> "$SDBOOT/LICENSE.txt"
    else
        log "licence: $licence_text is missing; LICENSE.txt names it without quoting it"
        printf '\n\n(%s was not in the source tree when this card was built.  Get it\nfrom the MixOS repository, or from the URL named above.)\n' \
               "$(basename "$licence_text")" >> "$SDBOOT/LICENSE.txt"
    fi
done

# ── The OS-partition payload: /opt/mixos, and the dashboard in it ─────────────
#
# The four files on BOOT are the ones something other than Linux has to read.  This is
# everything else -- the dashboard, its Qt, Doom, and the j36/ tree staged further up --
# and it goes on the ext2 OS partition, where symlinks and execute bits survive and
# where 50 MB is not competing with an R36S card's own boot files.
#
# WHY A NEW DIRECTORY AND NOT /usr.  One Debian rootfs on this card serves two
# machines -- this board and an R36S -- and the rule that has held all the way through
# this bring-up is that nothing on it is modified.  /opt/mixos is a path neither ArkOS
# nor dArkOS nor Debian has ever used, so unpacking this payload adds files and
# changes none: an R36S booting the same card gets its own libEGL.so symlink and its
# own units, and never looks in /opt.
#
# WHY IT IS A TARBALL AS WELL AS A TREE.  The SONAME aliases in qt/lib are symlinks and
# mfgpower, eglprobe, mixdash, j36-mixmirror and doom have to stay executable.  A tarball is the copy
# that cannot lose either -- or the ownership -- whatever machine does the copying, and
# the reason it is not simply a directory to drag across in a file manager.
#
#   sudo tar -C /path/to/the/mounted/ROOTFS -xzf sd-root.tar.gz
#
# It is not fatal for any of this to be absent, but it is not silent either, and that
# is the lesson of a boot that ended at hostnamed with nothing on the panel: with no
# /opt/mixos on the card /init writes mixdash-missing.service instead, which says on
# the console what it looked for and where.  Nothing is started in the dashboard's
# place, so a card with no payload comes up to a readable console rather than to
# something else taking the panel.
# SDROOT was declared and cleared up beside SDBOOT, because the j36/ payload above is
# staged into it.  So this section adds to a tree that may already have files in it,
# and the tarball below is written whenever ANY of them are there -- not only when the
# dashboard is.  With J36_MIXDASH=0 and no Doom, opt/mixos/j36 is still the modules,
# mfgpower and the probe, and a build that silently shipped no tarball for them would
# leave a card that boots to a dashboard-less console with no GPU either.
if [[ -n "$MIXDASH_BIN" || -n "$DOOM_BIN" || -n "$MIXMIRROR_BIN" ]]; then
    mkdir -p "$SDROOT/opt/mixos/bin"

    if [[ -n "$MIXDASH_BIN" && -n "$QT_PAYLOAD" ]]; then
        cp "$MIXDASH_BIN" "$SDROOT/opt/mixos/bin/mixdash"
        chmod 0755 "$SDROOT/opt/mixos/bin/mixdash"
        cp "$QT_PAYLOAD/bin/qt.conf" "$SDROOT/opt/mixos/bin/qt.conf"
        mkdir -p "$SDROOT/opt/mixos/qt"
        # -a: the SONAME aliases in lib/ are symlinks and have to stay symlinks.
        cp -a "$QT_PAYLOAD/lib" "$QT_PAYLOAD/plugins" "$QT_PAYLOAD/fonts" \
              "$SDROOT/opt/mixos/qt/"
        log "dash: staged the dashboard and $(ls -1 "$SDROOT/opt/mixos/qt/lib" | wc -l) Qt files into opt/mixos/"
    elif [[ -n "$MIXDASH_BIN" ]]; then
        # Deliberately not staged alone.  A dashboard with no Qt beside it is a
        # binary the loader cannot start, and a card that carries it would look
        # configured while failing before main().
        log "dash: the dashboard was built but its Qt payload was not, so neither is staged"
    fi

    # The startup chime.
    #
    # STAGED LIKE THE IWAD AND NOT LIKE THE SPLASH, and the difference is worth
    # naming because both are files in resources/.  MixOS.jpg is decoded by
    # tools/jpeg2raw.py at build time into /splash.mixspl in the INITRAMFS, because
    # it has to be on the glass before any partition is mounted.  This is played by
    # the dashboard, seconds later, off a filesystem that is by then mounted -- so
    # it is an ordinary payload asset under share/, in the same shape as Doom's
    # IWAD, and mixdash looks for it at exactly this path.
    #
    # Guarded on the dashboard being staged: nothing else on this card can play it,
    # and half a megabyte of mp3 that no binary will ever open is half a megabyte of
    # an image whose whole point is to stay small.
    if [[ -n "$MIXDASH_BIN" && -n "$QT_PAYLOAD" &&
          -f "$ROOT/device/j36-ultra/resources/startup.mp3" ]]; then
        mkdir -p "$SDROOT/opt/mixos/share/mixdash"
        cp "$ROOT/device/j36-ultra/resources/startup.mp3" \
           "$SDROOT/opt/mixos/share/mixdash/startup.mp3"
        log "dash: staged the startup chime into opt/mixos/share/mixdash/"
    fi

    # Doom, and the IWAD under the name doomgeneric's iwads[] table knows it by --
    # d_iwad.c matches the filename before it opens the file.
    if [[ -n "$DOOM_BIN" ]]; then
        cp "$DOOM_BIN" "$SDROOT/opt/mixos/bin/doom"
        chmod 0755 "$SDROOT/opt/mixos/bin/doom"
        if [[ -n "$DOOM_WAD" ]]; then
            mkdir -p "$SDROOT/opt/mixos/share/doom"
            cp "$DOOM_WAD" "$SDROOT/opt/mixos/share/doom/$(basename "$DOOM_WAD")"
        fi
        log "dash: staged doom into opt/mixos/bin/"
    fi

    # The Browser card's start page.
    #
    # THE CARD IS links2 IN THE DASHBOARD'S OWN TERMINAL, and the long answer to
    # "why not a real browser" is in buildPages() in dashboard.cpp -- the short one
    # is that Edge has no armhf build, that Debian's netsurf-fb and links2 are both
    # built without a framebuffer surface, and that an X server on this board is the
    # VT switch that the Console card was removed for.  What is left is a text
    # browser driven by the pad, and text browsers open on a blank screen.
    #
    # So the card opens THIS, off the card, with no network needed and nothing typed:
    # the keys are on it, because links2's are not the pad's and nobody should have to
    # guess `g' for go-to-URL on an eleven-button device, and there are four links so
    # that pressing Enter on one is the whole test of "does this thing browse".
    #
    # Written here and not in mixdash, because a page is a file and dashboard.cpp
    # would have to escape every quote in it into a C++ string literal to say the
    # same thing.  dashboard.cpp holds the path and falls back to a search engine
    # when this file is not on the card.
    #
    # No CSS and no JavaScript: links2's text mode reads neither, and a start page
    # that renders differently in the browser it ships for is a start page that was
    # written for a different browser.
    mkdir -p "$SDROOT/opt/mixos/share/browser"
    cat > "$SDROOT/opt/mixos/share/browser/start.html" <<'BROWSERSTART'
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>MixOS</title>
</head>
<body>

<h1>MixOS</h1>

<p>This is links2, running in the dashboard's terminal. It renders tables,
frames, forms and cookies, and it speaks TLS. It does not run JavaScript.</p>

<h2>Driving it</h2>

<table>
<tr><td>Up, Down</td><td>move between links</td></tr>
<tr><td>Enter</td><td>follow the link under the cursor</td></tr>
<tr><td>Left</td><td>back one page</td></tr>
<tr><td>ESC</td><td>the menu bar: File, View, Link, Downloads, Setup, Help</td></tr>
<tr><td>g</td><td>go to a URL</td></tr>
<tr><td>s</td><td>bookmarks</td></tr>
<tr><td>q</td><td>quit, back to the shell</td></tr>
</table>

<p>The Menu button raises the dashboard's on-screen keyboard, which is how a URL
gets typed on a device with no keyboard. B leaves the terminal and returns to the
cards. Downloads and links2's own bookmarks land in /home/virtua, which is the
directory the Sharing card exports over SMB.</p>

<h2>Somewhere to start</h2>

<ul>
<li><a href="https://html.duckduckgo.com/html/">DuckDuckGo</a> &mdash; the
no-script version, which is the one that works here</li>
<li><a href="https://en.wikipedia.org/wiki/Main_Page">Wikipedia</a></li>
<li><a href="https://lite.cnn.com/">CNN Lite</a> &mdash; text-only news</li>
<li><a href="https://www.debian.org/distrib/packages">Debian packages</a>
&mdash; what the Packages card installs from</li>
</ul>

<h2>If nothing loads</h2>

<p>The Wi-Fi card joins a network; this page is on the card itself and opens
whether or not one has been joined. Once Wi-Fi is up, any of the links above is
the test.</p>

</body>
</html>
BROWSERSTART
    log "dash: staged the browser start page into opt/mixos/share/browser/"

    # The mirror goes beside them and is staged on its own merits -- unlike the
    # dashboard it has no payload it needs next to it, being static, so a build that
    # produced no Qt still ships a working dock.  /init writes its unit only when this
    # file is here, so removing it from the card takes the feature off cleanly.
    if [[ -n "$MIXMIRROR_BIN" ]]; then
        cp "$MIXMIRROR_BIN" "$SDROOT/opt/mixos/bin/j36-mixmirror"
        chmod 0755 "$SDROOT/opt/mixos/bin/j36-mixmirror"
        log "dash: staged j36-mixmirror into opt/mixos/bin/"
    fi
fi

# The same notice obligation again: mixdash is MixOS's own code, so the tree it ships
# in has to carry its terms.  Short, and pointing at the full text on BOOT rather than
# repeating seven hundred lines of licence in two places on one card.
#
# From here the test is the TREE and not the two binaries.  $SDROOT already holds
# opt/mixos/j36 -- the lima and mtk_drm modules, mfgpower, the Mesa front end, the
# probe -- staged about twelve hundred lines up, and that payload ships on its own
# merits: with J36_MIXDASH=0 and no Doom the old guard fell straight to the else
# below and wrote no tarball at all, leaving the payload it HAD built unreachable.
if [[ -d "$SDROOT/opt" ]]; then
    mkdir -p "$SDROOT/opt/mixos"
    cat > "$SDROOT/opt/mixos/README.txt" <<'MIXOSREADME'
MixOS -- J36 Ultra (MediaTek MT6592, ARMv7) OS-partition payload.

This is everything the card carries that is not the launcher.  The BOOT partition is
FAT32 because the MVII LK reads FAT32 and nothing else, and it holds only the four
files the LK itself reads -- zImage, mt6592-j36-ultra.dtb, initrd.img and
mvii/boot.conf.  Once /init has the rootfs mounted, nothing is loading off FAT any
more, so the rest lives here, on the ext2 OS partition, where symlinks and execute
bits survive and where 50 MB is not competing with an R36S card's own boot files.

Unpack it into the root of the card's second partition -- the shared armhf Debian
rootfs -- and nothing already on it is touched: everything here is under /opt/mixos,
a directory neither Debian nor ArkOS nor dArkOS uses.  An R36S booting the same card
is unaffected and never looks in it.

The build normally does that for you: the flashable image ships with this tree
already inside its ext2 OS partition, so a card that was written from that image
needs nothing poured onto it.  The tarball is here for updating a card in place, and
for a workstation that cannot write ext2 there is no way round reflashing.

Your own space is /home/virtua, a directory on this same partition: a shell starts
there, the dashboard's Files page opens there, and it is the one place on the card
meant to be written from the device.  Nothing in this payload is installed into it.
It used to be a partition of its own; it is not any more, and the space it had went
to this one, which grows to fill the card on the first boot.

The dashboard and the games:

  bin/mixdash          the dashboard.  Qt5 Widgets on the linuxfb platform plugin,
                       which writes into /dev/fb0 -- the framebuffer the MVII LK
                       already lit and simplefb still owns.  No EGL, no GBM, no mode
                       set.  Run it by hand with --probe to have it report the
                       framebuffer's geometry and exit.
  bin/qt.conf          where Qt finds its plugins.  Read from the executable's own
                       directory, so mixdash works from a plain shell.
  qt/lib               Qt 5.15 and its runtime closure, from Debian trixie armhf.
                       The dashboard's RPATH names this directory and /run/j36/gl.
                       These are SONAME symlinks: unpack on ext2, not on the FAT
                       BOOT partition, or the loader finds nothing to open.
  qt/plugins/platforms libqlinuxfb.so, the one plugin this needs.
  qt/fonts             DejaVu Sans, two faces, and a fonts.conf that names this one
                       directory.  /init points FONTCONFIG_FILE at it when it is
                       there, which is what keeps the dashboard's font phase off a
                       whole-rootfs fontconfig scan.
  bin/doom             framebuffer Doom (doomgeneric), if the build staged it.
  share/doom           its IWAD.
  share/mixdash        startup.mp3, played once when the dashboard's first frame
                       reaches the glass.  Optional: the dashboard checks that the
                       file is readable and stays quiet if it is not.

j36/ -- what /init loads, one directory per bootargs word.  Delete any of them and
the matching j36. word finds nothing, says so on the console, and the boot carries
straight on; that is the recovery path for all of them, from any machine that reads
SD cards.

  j36/modules          lima and its dependencies, plus modules/load.order naming
                       them in the order insmod needs.        j36.lima=1
  j36/mfgpower         static ARMv7 helper that brings the GPU power domain up
                       through the SPM before lima probes.    j36.lima=1
  j36/mtkdrm           the display set and its own load.order.  j36_fbmem loads
                       first and gives /dev/j36fb, the LK framebuffer as a
                       dma-buf, whether or not the rest binds.   j36.mtkdrm=1
  j36/audio            the ALSA core and the MT6592 AFE adapter.  j36.audio=1 gets
                       the card and the headphone jack; j36.audio=speaker also
                       powers the class-D amp, which hangs off VBAT and so needs a
                       cell fitted.  Nothing detects a plug: "Speaker Amp" and
                       "Headphone" are mixer switches, both may be on at once.
  j36/usb              the host stack for the MUSB OTG port -- the data connector,
                       which is not the DC inlet the board charges from: the PHY,
                       musb and its glue, usbhid, udl for a DisplayLink dock's
                       HDMI, and the disk set -- scsi_mod, sd_mod, usb-storage and
                       ntfs3 -- which is what makes an external drive appear and
                       automount under /media.  The PHY holds DRVVBUS high, so the
                       port sources 5 V off VBAT and a cell should be fitted;
                       j36.usb=novbus never sources, and j36.usb=automeasure is the
                       old measured role, for a board with one shared socket.  Two
                       modules here also live in j36/mtkdrm; whichever loaded first
                       wins.  j36.usb=1
  j36/power            the MT6592 PMIC -- battery gauge, charger and a poweroff
                       that cuts the rail -- and the panel backlight.  j36.power=1;
                       j36.power=nocharge keeps the charger as the LK set it.
  j36/wifi             the radio, and wlan0: the CONSYS MCU's rails, the BTIF
                       link, wifi/firmware/ holding the two ROM patches and the
                       WLAN firmware, and cfg80211 on top.  2.4 GHz WPA2-PSK,
                       driven by the wpa_supplicant and NetworkManager already on
                       the rootfs; the boot log says which stage it reached.
                       j36.wifi=1 implies j36.power, because one driver owns the
                       PMIC wrapper.
  j36/gl               Mesa's GL front end, staged in /run/j36/gl ahead of the
                       rootfs's RK3326 Mali blob.  links/ records the SONAME
                       aliases, kept from when this payload was on FAT.  j36.gl=1
  j36/eglprobe         asks the DRI nodes what they can do and prints the answer.
                       j36.gl=debug runs it; -f works from a shell at any time.

The initramfs writes mixdash.service into /run/systemd/system -- in memory, never on
the card -- and wants it from multi-user.target, so this payload does not depend on
any unit the rootfs happens to have installed or enabled.  Delete this directory and
it is not written: instead the console gets mixdash-missing.service, saying which
partitions were searched.  Nothing is started in the dashboard's place either way;
to hand the boot back to the rootfs's own shell, drop j36.dash=1 from the bootargs
in mvii/boot.conf on the BOOT partition.

Licence: the MixOS work here -- bin/mixdash, bin/j36-mixmirror, j36/mfgpower and
j36/eglprobe -- is dual licensed, MPL-2.0 or GPL-2.0-or-later at your option; both
full texts are in LICENSE.txt on the BOOT partition.  Qt, its dependencies and the
fonts are Debian's packages under their own terms (LGPL-3 with Qt's exceptions for Qt
itself; see /usr/share/doc on a Debian machine).  The kernel modules under j36/ are
GPL-2.0, from the Linux tree they were built from.  Mesa in j36/gl is under the MIT
licence.  doomgeneric and Doom's engine source are id Software's under the GPL, as
Debian and doomgeneric ship them.  The operating system underneath is Debian, and
MixOS is a device port on top of it; the build scripts descend from dArkOS, which
continues ArkOS, and nothing else here does.  MixOS supports the MediaTek line of
processors.
MIXOSREADME

    ( cd "$SDROOT" && tar -czf "$ARTIFACTS/sd-root.tar.gz" \
        --owner=root --group=root --numeric-owner opt )
    log "sd-root: sd-root.tar.gz is $(stat -c %s "$ARTIFACTS/sd-root.tar.gz") bytes"
    log "sd-root: $(find "$SDROOT/opt" -type f | wc -l) files, $(du -sh "$SDROOT/opt" | cut -f1) as a tree"

    # ── The same tarball, on BOOT, for a workstation that cannot write ext2 ───
    #
    # This is the only way an already-flashed card can be updated from macOS: BOOT is
    # the one partition it mounts.  The tarball goes on it whole and /init unpacks it
    # onto the OS partition on the next boot -- see stage_from_boot().  Copying the
    # tree there by hand instead is what produces "invalid ELF header", because vfat
    # keeps neither the SONAME symlinks nor the execute bits.
    #
    # Only if it fits.  BOOT is ${SYSTEM_SIZE:-100} MB and its real job is the
    # launcher; a tarball that crowds out the kernel would trade a working card for a
    # convenient update.  FAT32 also rounds every file up to a cluster, so the budget
    # here is deliberately short of the partition.
    boot_used=$(du -s --block-size=1 "$SDBOOT" | cut -f1)
    tar_bytes=$(stat -c %s "$ARTIFACTS/sd-root.tar.gz")
    boot_budget=$(( (${SYSTEM_SIZE:-100} - 8) * 1024 * 1024 ))
    if [[ "${J36_BOOT_TARBALL:-1}" != 1 ]]; then
        log "sd-boot: J36_BOOT_TARBALL=0, so the tarball is not staged on BOOT"
    elif (( boot_used + tar_bytes <= boot_budget )); then
        cp "$ARTIFACTS/sd-root.tar.gz" "$SDBOOT/sd-root.tar.gz"
        log "sd-boot: sd-root.tar.gz staged on BOOT too ($(( (boot_used + tar_bytes) / 1048576 )) MB of the ${SYSTEM_SIZE:-100} MB budget)"
        log "sd-boot: copy it onto BOOT from any machine and the next boot unpacks it"
        log "sd-boot: onto the OS partition -- no reflash, no Linux machine needed"
    else
        log "sd-boot: NOT staging sd-root.tar.gz on BOOT: the launcher is $(( boot_used / 1048576 )) MB,"
        log "sd-boot: the tarball is $(( tar_bytes / 1048576 )) MB and the budget is $(( boot_budget / 1048576 )) MB."
        log "sd-boot: Update the card by reflashing the image, or untar it onto the OS"
        log "sd-boot: partition from a Linux machine."
    fi
else
    # Only reachable with J36_PAYLOAD_ON=boot and no dashboard: everything went to
    # $SDBOOT, and the OS partition needs nothing.  Worth saying out loud, because
    # with the default J36_PAYLOAD_ON=root it would mean the whole j36/ payload
    # failed to build.
    log "sd-root: nothing staged for the OS partition, so no sd-root.tar.gz"
fi

(
    cd "$ARTIFACTS"
    # The Doom payload is optional, and sha256sum takes a missing operand as an
    # error, so it is named only when it was staged.
    sums=(boot.img zImage zImage-j36-ultra mt6592-j36-ultra.dtb
          j36_mt6592_input.ko initramfs-j36-ultra.cpio.xz
          sd-boot/zImage sd-boot/mvii/boot.conf)
    if [[ -f sd-root/opt/mixos/bin/mixdash ]]; then
        sums+=(sd-root/opt/mixos/bin/mixdash)
    fi
    if [[ -f sd-root/opt/mixos/bin/doom ]]; then
        sums+=(sd-root/opt/mixos/bin/doom)
    fi
    # The tarball is what actually gets copied to the card, so it is what a reader
    # wants to check before unpacking it.  The files under sd-root/ below are summed
    # as well, because a mismatch there says which one went wrong.
    if [[ -f sd-root.tar.gz ]]; then
        sums+=(sd-root.tar.gz)
    fi
    # $PAYREL, not a literal path: the j36/ payload is sd-root/opt/mixos/j36 by
    # default and sd-boot/j36 under J36_PAYLOAD_ON=boot, and this block is relative
    # to $ARTIFACTS either way.
    if [[ -f $PAYREL/mfgpower ]]; then
        sums+=("$PAYREL/mfgpower" "$PAYREL/modules/load.order")
        # Named individually rather than by glob, so that a module that failed to
        # copy is a missing line here instead of a silently shorter list.
        while IFS= read -r ko; do
            sums+=("$PAYREL/modules/$ko")
        done < "$PAYREL/modules/load.order"
    fi
    if [[ -f $PAYREL/mtkdrm/load.order ]]; then
        sums+=("$PAYREL/mtkdrm/load.order")
        while IFS= read -r ko; do
            sums+=("$PAYREL/mtkdrm/$ko")
        done < "$PAYREL/mtkdrm/load.order"
    fi
    if [[ -f $PAYREL/audio/load.order ]]; then
        sums+=("$PAYREL/audio/load.order")
        while IFS= read -r ko; do
            sums+=("$PAYREL/audio/$ko")
        done < "$PAYREL/audio/load.order"
    fi
    if [[ -f $PAYREL/gl/links ]]; then
        sums+=("$PAYREL/gl/links")
        # Glob here and not a manifest walk, because links names the SONAMEs and
        # not the files: the same library is one file and one or two names.
        for gl in "$PAYREL"/gl/*.so*; do
            [[ -f "$gl" ]] && sums+=("$gl")
        done
    fi
    # An if, not a && -- this runs under set -e and a card built without the probe
    # would make the AND-list fail and take the manifest with it.
    if [[ -f $PAYREL/eglprobe ]]; then
        sums+=("$PAYREL/eglprobe")
    fi
    sha256sum "${sums[@]}" > SHA256SUMS
    {
        echo "licence=MPL-2.0 OR GPL-2.0-or-later for the MixOS bring-up work, GPL-2.0-only for the kernel and the seven MixOS modules, per-payload in sd-boot/LICENSE.txt and summarised in sd-root/opt/mixos/README.txt"
        echo "kernel_branch=$KERNEL_BRANCH"
        echo "kernel_release=$KERNEL_RELEASE"
        echo "kernel_arch=arm (ARMv7, 32-bit)"
        echo "zimage_size=$(stat -c %s zImage)"
        echo "dtb_sha256=$(sha256sum mt6592-j36-ultra.dtb | awk '{print $1}')"
        echo "bootimg_size=$(stat -c %s boot.img) (slot 0x900000)"
        echo "storage=msdc1 mtk-sd mediatek,mt6592-mmc (ext2, ext4, btrfs, exfat, vfat)"
        echo "card_layout=p1 BOOT vfat = launcher only (zImage, dtb, initrd.img, mvii/boot.conf, LICENSE.txt, README.txt); p2 ROOTFS ext2 = the OS, /opt/mixos included, and the login user's home at ${DATA_MOUNT_POINT:-/home/virtua} as an ordinary directory in it.  Two partitions: there is no p3, and p2 is last on the disk so /init can grow it to the card's size on the first boot"
        echo "card_expand=/init's expand_root, before switch_root: sfdisk -N extends p2 to the end of the disk, e2fsck -fp, then resize2fs with no size argument.  ext2 has no online resize, so this is the only moment in the boot it can happen; the three tools and their libraries are copied out of the rootfs before it is unmounted, and a copy that will not run leaves the card alone"
        echo "rootfs_format=ext2, set in setup_partition.sh and device/r36-ultra/build-in-vm.sh; the MVII LK reads FAT32 only, so BOOT is FAT and the OS partition is free to be the simplest filesystem both kernels on this card handle"
        echo "payload=$PAYREL (J36_PAYLOAD_ON=$PAYLOAD_ON; /init looks in the rootfs /opt/mixos/j36 first, then j36/ on BOOT for a card written by an older build)"
        echo "msdc1_irq=GIC_SPI 72 (INTID 104 - 32)"
        echo "userspace=net+unix+namespaces (systemd 257 on the shared armhf rootfs)"
        echo "wireless=off"
        echo "reboot=mtk_wdt via watchdog@10007000 (mediatek,mt6589-wdt)"
        echo "console=tty0 last, journald forwarded to it"
        echo "firstboot=disabled in the rootfs and masked in the bootargs (RK3326 script, no /roms.tar in a GUI-mode build -- and what it carves a third partition out of is exactly the free space expand_root gives to ROOTFS)"
        echo "batt_led=enabled (batt_life_warning.py finds the battery by power_supply type and treats a missing LED as normal; it no longer exits, so Restart=always never fires)"
        echo "bootimg_kernel=zImage-j36-ultra (device tree appended, ATAG path)"
        echo "sd_kernel=sd-boot/zImage (plain, LK passes the tree in r2)"
        echo "display=stock-lk-simple-framebuffer"
        echo "native_dsi=disabled"
        echo "input_adapter=j36_mt6592_input.ko"
        # The shell, and the payload it lives in.  The ext2 OS partition, not BOOT: the
        # Qt closure is 50 MB of SONAME symlinks, and BOOT is 100 MiB of vfat that holds
        # neither symlinks nor execute bits and is shared with an R36S card's own
        # launcher.  sd-root.tar.gz carries the whole tree now -- the dashboard, Qt,
        # Doom and opt/mixos/j36 -- so this one line is what has to reach the card.
        if [[ -f sd-root/opt/mixos/bin/mixdash ]]; then
            echo "shell=mixdash ($(stat -c %s sd-root/opt/mixos/bin/mixdash) bytes, Qt5 Widgets on linuxfb)"
            echo "shell_payload=sd-root.tar.gz ($(stat -c %s sd-root.tar.gz) bytes), untarred at the root of the ext2 ROOTFS partition as /opt/mixos"
            echo "shell_start=j36.dash=1; /init writes /run/systemd/system/mixdash.service and wants it from multi-user.target"
            echo "shell_find=/init looks in the rootfs first, then mounts every other partition read-only looking for opt/mixos/bin/mixdash (or mixos/bin/mixdash, for a tarball unpacked one level down); every partition it tries is named on the console, mounted or unreadable"
            echo "shell_missing=when nothing is found, /init also writes /run/systemd/system/mixdash-missing.service, which repeats the reason and the fix on the console six times at 20 s -- because the initramfs lines have scrolled off by then and a boot that ends at hostnamed looks the same as ten other faults"
            echo "shell_card=/run/j36/card is what the dashboard's Files page opens on, there being no keyboard here to mount anything by hand; it is a symlink to ${DATA_MOUNT_POINT:-/home/virtua}, which on a current card is a directory on ROOTFS and needs no mount of its own.  The link names a path and not a device, so on a card written before this layout -- where /home/virtua is p3, ext2 and labelled DATA -- systemd's own fstab entry mounts it there a few seconds later and the same link is still right"
            echo "shell_nodash=without j36.dash=1 nothing is staged at all and /init says so, naming the word to add -- this rootfs enables no shell of its own, so the alternative is a board that boots to nothing and explains nothing"
            echo "shell_render=Qt5 raster into /dev/fb0, which is simplefb's window onto the framebuffer the LK lit -- no EGL, no GBM, no DRM master, no modeset"
            echo "shell_input=evdev directly, QT_QPA_FB_DISABLE_INPUT=1 (gpio-keys plus the keypad, per the device tree)"
            if [[ -f sd-root/opt/mixos/bin/doom ]]; then
                echo "fbdoom=sd-root/opt/mixos/bin/doom ($(stat -c %s sd-root/opt/mixos/bin/doom) bytes, static ARMv7, 640x400 in 640x480)"
                echo "fbdoom_commit=$DOOM_COMMIT"
                if [[ -n "$DOOM_WAD" ]]; then
                    echo "fbdoom_iwad=opt/mixos/share/doom/$(basename "$DOOM_WAD")"
                else
                    echo "fbdoom_iwad=none (drop one into /opt/mixos/share/doom on the card)"
                fi
                echo "fbdoom_start=the dashboard's Doom card; there is no j36.doom word any more"
            else
                echo "fbdoom=not staged (J36_DOOM=1 builds it into the second partition)"
            fi
        else
            echo "shell=not staged; /init says so on the console, and there is no fallback shell in this build to start instead"
        fi
        if [[ -f $PAYREL/mfgpower ]]; then
            echo "gpu=mali-450 mp4 at 0x13040000 (gp irq 234..pp_bcast 244, GIC_SPI 202..212)"
            echo "gpu_driver=lima, CONFIG_DRM_LIMA=m ($(tr '\n' ' ' < $PAYREL/modules/load.order))"
            echo "gpu_power=j36/mfgpower ($(stat -c %s $PAYREL/mfgpower) bytes, static ARMv7, SPM MTCMOS via /dev/mem)"
            echo "gpu_start=j36.lima=1 on the command line; /init loads the modules only if mfgpower exits 0"
            echo "gpu_nodes=renderD128 from lima; card0 comes from mtk_drm, not from lima"
        else
            echo "gpu=not staged"
        fi
        if [[ -f $PAYREL/mtkdrm/load.order ]]; then
            echo "display_drm=mediatek-drm, mt6592 via mt2701 fallback compatibles"
            echo "display_ddp=ovl0 0x14007000 -> rdma0 0x14008000 -> color0 0x1400b000 -> dsi0 0x1400c000"
            echo "display_mutex=mod 0x488, sof 1 (matches the LK register for register)"
            echo "display_phy=mediatek,mt2701-mipi-tx (mppll_preserve 3, as the LK writes)"
            echo "display_data_rate=192 MHz from the pixel clock; the LK programmed 214"
            echo "display_modules=$(tr '\n' ' ' < $PAYREL/mtkdrm/load.order)"
            echo "display_start=j36.mtkdrm=1; no register is programmed until card0 is opened"
            echo "display_fbdev=CONFIG_DRM_FBDEV_EMULATION=n, so /dev/fb0 stays simplefb's"
        else
            echo "display_drm=not staged"
        fi
        if [[ -f $PAYREL/audio/load.order ]]; then
            echo "audio=mt6592 afe at 0x11220000, dl1 memif -> i2s dac -> mt6323 abb"
            echo "audio_driver=j36_mt6592_audio.ko, native ALSA (no MT6592 ASoC driver exists upstream)"
            echo "audio_reference=PowerEngine OS/MVII/.../mt6592_audio.c, from the MT6592 HAL"
            echo "audio_pcm=1 playback, S16_LE stereo 8k-48k, 64 KiB ring, cursor polled at 5 ms, no IRQ"
            echo "audio_modules=$(tr '\n' ' ' < $PAYREL/audio/load.order)"
            echo "audio_core=CONFIG_SOUND=y (soundcore only); snd, snd-timer, snd-pcm are =m and staged here"
            echo "audio_snd_pcm=selected by SND_DUMMY=m, which is built and deliberately not staged"
            echo "audio_start=$(grep -o 'j36\.audio=[a-z0-9]*' sd-boot/mvii/boot.conf)"
            echo "audio_clock=first ungate of AFE_CG on this board; dmesg reports whether DL1_CUR advances on the first stream"
            echo "audio_outputs=headphone jack (on by default) and class-D speaker; two mixer switches, no jack detect on this board"
            echo "audio_volume=analog on both: SPK_CON9 level and AUDTOP_CON5 gain under one Master element, no softvol"
            echo "audio_speaker=off unless j36.audio=speaker, and then only after the cursor moves"
            echo "audio_speaker_hazard=class-D amp on VBAT, which is the system node; battery-less it trips the PMIC UVLO"
        else
            echo "audio=not staged"
        fi
        if [[ -f $PAYREL/gl/links ]]; then
            echo "gl=debian armhf mesa 25.0.7 from the shared rootfs (lima_dri.so + mediatek_dri.so)"
            echo "gl_front_end=$(ls $PAYREL/gl/*.so* | xargs -n1 basename | tr '\n' ' ')"
            echo "gl_reason=the shared rootfs points libEGL.so, libgbm.so{,.1,.1.0.0} and libGLESv1_CM.so at the RK3326 Mali blob"
            echo "gl_load_bearing=libgbm.so.1 -- libEGL_mesa.so.0 needs it, so mesa's own EGL cannot load without this payload"
            echo "gl_install=tmpfs on the rootfs /run, named by LD_LIBRARY_PATH in mixdash.service; nothing is written to the card"
            echo "gl_boot_word=$(grep -o 'j36\.gl=[a-z0-9]*' sd-boot/mvii/boot.conf)"
            echo "gl_users=eglprobe, and through it mixdash's 3D cube card; mixdash itself is Qt on linuxfb and needs no GL at all"
            echo "gl_es1=not available and not a driver bug: Debian's armhf mesa is a -Dgles1=disabled build, so eglCreateContext for an ES1 context is 0x3003 EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike. ES2 is what comes up."
            if [[ -f $PAYREL/eglprobe ]]; then
                echo "gl_probe=j36/eglprobe ($(stat -c %s $PAYREL/eglprobe) bytes, dynamic ARMv7, dlopens libEGL.so.1 and libgbm.so.1)"
                echo "gl_probe_run=-f 1 from mixdash-probe.service, Type=oneshot RemainAfterExit=yes, so once per boot and NOT once per mixdash start attempt; under j36.gl=debug that unit also runs the node probes and -s with LIBGL_ALWAYS_SOFTWARE=1 as the control, and mixdash replays the log from ExecStopPost. As mixdash's own ExecStartPre these re-ran on all three restarts, which meant three EGL inits on lima and the bars repainted over each attempt's error"
                echo "gl_probe_fb=-f opens /dev/fb0 and nothing else: it counts the pixels already there (all black = nothing drew, a picture = something took the scanout), undoes a backlight at brightness 0 and a tty0 left in KD_GRAPHICS, then paints eight colour bars with the CPU. Bars then a dashboard = both halves work; bars that stay = mixdash never started; no bars = nothing userspace draws will be seen."
                echo "gl_probe_nodes=-i names every /dev/dri node and says which one modesets; nothing here hard-codes card0 any more, because on this kernel card0 is lima and GETRESOURCES on it returns EOPNOTSUPP"
                echo "gl_probe_paint=-p (five CPU and lima frames) and -c (a rotating cube, GLES2 through lima, page-flipped) are NOT run at boot: both modeset, so whatever is on screen is theirs until they exit. Either one used to hold the panel until the next reboot -- on close the kernel releases their framebuffer and disables the CRTC, and with CONFIG_DRM_FBDEV_EMULATION=n nothing re-enabled it -- which preserve_lk_state in 0002-drm-mediatek-mt6592.patch ends: the pipe stays up and the OVL is put back the way the LK programmed it, so /dev/fb0 is live again on exit. The Diagnostics GPU row runs -o, which draws the same cube on the same GPU and copies it into /dev/fb0, so it takes no screen from anyone."
            else
                echo "gl_probe=not staged; j36.gl=debug will report only what Mesa says"
            fi
        else
            echo "gl=not staged"
            echo "gl_users=none; the dashboard needs no GL, and its 3D cube card will report that libEGL is absent"
        fi
    } > manifest.txt
)

# ── Folding both halves into the flashable image ──────────────────────────────
#
# WHY THIS EXISTS.  The instructions up to here read "copy sd-boot/ onto BOOT and untar
# sd-root.tar.gz onto ROOTFS", and on a macOS workstation the second half is impossible:
# macOS mounts FAT and exFAT and nothing else, so it can write BOOT and cannot write an
# ext2 -- or btrfs -- OS partition at all.  That is not a documentation problem, it is
# a card that can only ever be half-written from the machine the operator actually has.
#
# So the build does it.  This VM is Linux, the image is a file in this checkout, and
# both partitions are a losetup away.  The result is one image that boots as flashed:
# no copying, no ext2 driver on the workstation, no step that can be done wrong.
#
# The image is still only patched, never created here -- build-r36-ultra.sh owns it.
# With J36_RESUME_R36=0 and no previous image there is simply nothing to patch, and
# this section says so and leaves the two artifacts for a manual copy.
R36_STATE_ROOT="${DARKOS_R36_STATE_DIR:-$HOME/darkos-r36-state}"

# Which state directory the base build last finished in; its `latest-image' names the
# image.  Found rather than recomputed: the key is built from
# DEBIAN_RELEASE/USERSPACE_ARCH/BUILD_PROFILE in two other scripts and a third copy of
# that expression here would be a third thing to keep in step.
r36_state_dir() {
    local d newest=""
    for d in "$R36_STATE_ROOT"/*/; do
        [[ -f "${d}latest-image" ]] || continue
        if [[ -z "$newest" || "${d}latest-image" -nt "${newest}/latest-image" ]]; then
            newest="${d%/}"
        fi
    done
    printf '%s\n' "$newest"
}

# The base build's own record first, the mtime glob only as a fallback: after a layout
# change there can be two images in this checkout -- the current one and the one kept as
# `-old-layout' to flash meanwhile -- and newest-mtime is not the same question as which
# one the base build considers finished.
#
# ONLY MixOS_*.img.  The glob used to include dArkOS_R36_ULTRA_GUI_BASE_* and
# dArkOS_RG351MP_FULL_*, and those two names are now exactly the images this build must
# not touch: an image with a dArkOS_ name was produced before the rename, which means
# before the ext2/DATA layout, so injecting into it hands back a card that boots the old
# partitioning with today's payload in it -- the most confusing possible result.  A
# rebuild is the answer, and saying so is more useful than quietly patching the wrong
# image.  The old ones are named in the log so it is clear why they were passed over.
#
# AND ONLY A `_base' IMAGE IN THE FALLBACK.  The mtime glob used to take the newest
# MixOS_*.img, which since the base/output split is very often one of THIS pipeline's own
# outputs -- an image that has already been injected into once.  Patching that lays this
# run's payload over the last one's with nothing to say which file came from where, which
# is the iteration the split exists to prevent.  So the fallback asks for the base by
# name, and finding none is reported as having none rather than papered over with an
# image that would appear to work.
find_base_image() {
    local state name
    state="$(r36_state_dir)"
    if [[ -n "$state" && -f "$state/latest-image" ]]; then
        read -r name < "$state/latest-image"
        if [[ -n "$name" && -f "$ROOT/$name" ]]; then
            printf '%s\n' "$ROOT/$name"
            return 0
        fi
    fi
    ls -1t "$ROOT"/MixOS_*_*_base.img 2>/dev/null | head -n 1 || true
}

# What the injected image is a function of: the BASE it was copied from, and the two
# payloads.  Recorded after a successful injection so a re-run that changes neither can
# skip the injection -- which, with the copy, is the whole cost of this section.
#
# THE BASE AND NOT THE OUTPUT, since the split.  The output's size and mtime move every
# time this section runs, so signing the output would sign the answer with the question in
# it and never match twice.  The base is written only by build-r36-ultra.sh, which is
# exactly the event that should invalidate this.  The output's existence is checked
# separately, at the call site, because a stamp cannot speak for a file that was deleted.
image_export_signature() {
    local img="$1"
    {
        stat -c 'base %s %Y' "$img"
        if [[ -f "$ARTIFACTS/sd-root.tar.gz" ]]; then
            stat -c 'sd-root %s %Y' "$ARTIFACTS/sd-root.tar.gz"
        else
            echo "sd-root absent"
        fi
        find "$SDBOOT" -type f -printf 'sd-boot %P %s %T@\n' 2>/dev/null | sort
    } | md5sum | cut -d' ' -f1
}

IMAGE_STAMP="$WORK/.image-export"

# ── Making the OS partition's filesystem as big as the partition it is in ─────
#
# WHY THIS IS NEEDED BEFORE ANYTHING CAN BE WRITTEN TO p2.  write_rootfs.sh used to run
# `resize2fs -M' on the build root before it dds it into the image -- the filesystem
# shrunk to the smallest size that holds its own contents, so that the dd copied what the
# rootfs weighs rather than the 52 GB build root.  What that left in p2 was an ext2 with
# ZERO free blocks in a partition several hundred megabytes bigger than it.  So the
# payload's first mkdir answered "No space left on device", and every file after it "No
# such file or directory" -- because its parent directory was the mkdir that had just
# failed.
#
# Nothing on the device fixes it in time either.  firstboot.service is disabled in the
# rootfs and masked in the bootargs, and /init's own expand_root runs on the DEVICE, one
# boot after this build has to write into p2.  So the filesystem is grown here,
# to the end of its own partition, which is what both machines want anyway: an OS
# partition whose filesystem stops short of its own end is not a smaller image, it is
# space the running system cannot use.  The loop device is sizelimited to the partition,
# so `resize2fs' with no size cannot run past the partition's end.
#
# THAT SHRINK IS GONE NOW: write_rootfs.sh shrinks straight to STORAGE_SIZE, so an image
# this build produced arrives here already filling its partition and there is nothing to
# grow.  This stays anyway, and not out of sentiment -- it is the only thing standing
# between a rootfs that came from somewhere else and a payload that cannot write a single
# file.  What it does not do any more is PAY for the answer: see below.
grow_to_partition() {
    local loop="$1" fstype="$2" fsck_rc=0
    local geom blocks bsize span

    case "$fstype" in
        ext2|ext3|ext4)
            # ── ASK BEFORE PAYING ─────────────────────────────────────────────
            #
            # The e2fsck below reads all four gigabytes of the partition, and on
            # every build that write_rootfs.sh produced it reads them in order to
            # discover that resize2fs has nothing to do.  dumpe2fs states the same
            # fact from the superblock in no time at all: if the filesystem's own
            # block count already spans the loop device to within one block, it
            # fills the partition and there is no growing to be done.
            #
            # This is a short-cut around work already done, not around the check.
            # A filesystem that really is smaller than its partition falls through
            # to exactly the fsck-then-grow it always did.
            geom="$(sudo dumpe2fs -h "$loop" 2>/dev/null)"
            blocks="$(printf '%s\n' "$geom" | awk -F: '/^Block count:/{gsub(/ /,"",$2); print $2}')"
            bsize="$(printf '%s\n' "$geom" | awk -F: '/^Block size:/{gsub(/ /,"",$2); print $2}')"
            span="$(sudo blockdev --getsize64 "$loop" 2>/dev/null || echo 0)"
            if [[ -n "$blocks" && -n "$bsize" ]] && (( span > 0 )) &&
               (( span - blocks * bsize < bsize )); then
                log "image: p2's filesystem already fills its partition"
                return 0
            fi
            # resize2fs refuses a filesystem it has not seen checked, and the last thing
            # to touch this one was a dd.  1 and 2 mean "found and fixed", not "broken".
            sudo e2fsck -p -f "$loop" >/dev/null 2>&1 || fsck_rc=$?
            if (( fsck_rc > 2 )); then
                log "image: e2fsck could not clean p2 (exit $fsck_rc); not growing it"
                return 1
            fi
            if ! sudo resize2fs "$loop" >/dev/null 2>&1; then
                log "image: resize2fs could not grow p2 to fill its partition, so the"
                log "image: payload has only the free space the shrink left it: none"
                return 1
            fi
            ;;
        btrfs)
            # btrfs only resizes mounted, so this one is done after the mount below.
            return 0
            ;;
    esac
    return 0
}

inject_into_image() {
    local img="$1" part_json p1_start p1_size p2_start p2_size loop mnt fstype rc=0
    local tar_rc=0

    log "image: folding both payloads into $(basename "$img")"

    # sfdisk -J and not the arithmetic from setup_partition.sh: those values live in
    # two other scripts and this one would be a third copy to keep in step.  The
    # partition table in the image is the authority on where its partitions are.
    part_json="$(sfdisk -J "$img" 2>/dev/null)" || {
        log "image: sfdisk could not read the partition table; skipping"
        return 0
    }
    # One python3 for all four numbers, read into the shell with `read'.  Four separate
    # invocations would each re-parse the same JSON, and any one of them failing would
    # leave a subset of the geometry set -- which is a losetup at the wrong offset.
    read -r p1_start p1_size p2_start p2_size <<<"$(printf '%s' "$part_json" | python3 -c '
import json, sys
p = json.load(sys.stdin)["partitiontable"]["partitions"]
print(p[0]["start"], p[0]["size"], p[1]["start"], p[1]["size"])
' 2>/dev/null || true)"
    if [[ -z "$p1_start" || -z "$p2_start" ]]; then
        log "image: could not read p1/p2 geometry from the partition table; skipping"
        return 0
    fi

    mnt="$WORK/image-mnt"
    mkdir -p "$mnt"

    # ── p1, the FAT32 launcher.  Added to, never replaced: the R36S's own Image,
    # uInitrd, rk3326 trees and boot.ini are on this partition and stay there.
    #
    # The whole of $SDBOOT rather than a list of four filenames, because that list has
    # already changed twice in this refactor and a copy that names files is a copy that
    # silently stops shipping the next one added.
    #
    # cp -r and NOT cp -a.  vfat has no ownership to preserve, so -a's chown fails on
    # every single file with "Operation not permitted" and cp exits non-zero -- after
    # copying the files perfectly well.  That turned a successful copy into a reported
    # failure and a bogus "is p1 full?".  -r asks for none of it, which is all vfat can
    # give; the LK reads these files and cares about neither owner nor mode.
    loop="$(sudo losetup --find --show \
        --offset $((p1_start * 512)) --sizelimit $((p1_size * 512)) "$img")"
    if sudo mount -t vfat "$loop" "$mnt"; then
        if sudo cp -r "$SDBOOT/." "$mnt/"; then
            sync
            log "image: p1 (vfat) now carries $(find "$SDBOOT" -type f | wc -l) launcher files, mvii/boot.conf included"
        else
            log "image: p1 mounted but the launcher copy failed -- is p1 full?  ${SYSTEM_SIZE:-100} MB is the budget"
            rc=1
        fi
        sudo umount "$mnt"
    else
        log "image: p1 would not mount as vfat; the launcher was NOT written"
        rc=1
    fi
    sudo losetup -d "$loop"

    # ── p2, the OS partition.  Only if there is a tarball, and only onto a filesystem
    # this can actually write.
    #
    # btrfs is accepted as well as ext2, and that is not a retreat from the ext2
    # refactor: an image built before it is entirely btrfs and self-consistent -- its
    # own /etc/fstab says btrfs for / -- so injecting the payload into it produces a
    # card that boots and runs the dashboard today.  Refusing would leave the operator
    # with no working card at all until a full base rebuild finished, and the base
    # rebuild is the thing that cannot be hurried: the build ROOT filesystem is btrfs
    # too, and write_rootfs.sh dds it into the image, so nothing short of recreating it
    # changes what is on p2.  The message says which layout was written either way.
    if [[ -f "$ARTIFACTS/sd-root.tar.gz" ]]; then
        loop="$(sudo losetup --find --show \
            --offset $((p2_start * 512)) --sizelimit $((p2_size * 512)) "$img")"
        fstype="$(sudo blkid -o value -s TYPE "$loop" 2>/dev/null || true)"
        case "$fstype" in
            ext2|ext3|ext4|btrfs)
                # Not fatal on its own: the verdict on this partition is whether the
                # payload came out of the tarball, not whether one resize2fs worked.  A
                # filesystem that already had room needs neither.
                grow_to_partition "$loop" "$fstype" || true
                if sudo mount -t "$fstype" "$loop" "$mnt"; then
                    if [[ "$fstype" == btrfs ]]; then
                        sudo btrfs filesystem resize max "$mnt" >/dev/null 2>&1 || \
                            log "image: btrfs would not resize to max; free space is as dd left it"
                    fi
                    log "image: p2 has $(sudo df -m --output=avail "$mnt" | tail -1 | tr -d ' ') MB free and /opt/mixos unpacks to $(du -sm "$SDROOT" | cut -f1) MB"
                    # -p and --numeric-owner: the tarball was written --owner=root
                    # --group=root, and /opt/mixos has to come out root-owned with
                    # the execute bits and the ~30 Qt SONAME symlinks intact.
                    #
                    # AND ITS EXIT STATUS IS READ.  It was not, and this whole function
                    # runs inside an `if !' so errexit is suppressed for it -- so a tar
                    # that failed on every single member with "No space left on device"
                    # was followed by "the card boots as flashed".  The verdict now comes
                    # from the artifact as well as the status: an unpack can report
                    # success and still be missing the one file that matters.
                    sudo tar -C "$mnt" -xzpf "$ARTIFACTS/sd-root.tar.gz" --numeric-owner \
                        || tar_rc=$?
                    sync
                    if (( tar_rc != 0 )); then
                        log "image: tar exited $tar_rc -- /opt/mixos was NOT written properly."
                        log "image: The lines above tar's are the reason; 'No space left on"
                        log "image: device' means p2's filesystem could not be grown."
                        rc=1
                    elif [[ ! -x "$mnt/opt/mixos/bin/mixdash" ]]; then
                        log "image: tar succeeded but $mnt/opt/mixos/bin/mixdash is not"
                        log "image: there and executable, so the dashboard would not start."
                        rc=1
                    else
                        log "image: p2 ($fstype) now carries /opt/mixos -- the card boots as flashed"
                        log "image: $(sudo du -sm "$mnt/opt/mixos" | cut -f1) MB of it, $(sudo find "$mnt/opt/mixos" -type f | wc -l) files, $(sudo df -m --output=avail "$mnt" | tail -1 | tr -d ' ') MB still free"
                    fi
                    if [[ "$fstype" == btrfs ]]; then
                        log "image: NOTE p2 is btrfs, so this image predates the ext2 layout."
                        log "image: It boots and the dashboard runs, but it still has the old"
                        log "image: three-partition table -- a vfat/exfat p3 sitting behind p2,"
                        log "image: which is what stops /init growing ROOTFS to the card's size."
                        log "image: To get the current layout the base image has to be rebuilt"
                        log "image: from its filesystem stage -- see the README; resuming will"
                        log "image: not do it, because the build root itself is btrfs."
                    fi
                    sudo umount "$mnt"
                else
                    log "image: p2 would not mount as $fstype; /opt/mixos was NOT written"
                    rc=1
                fi
                ;;
            *)
                log "image: p2 reports filesystem '${fstype:-unknown}', which this does"
                log "image: not write to.  The payload was NOT written."
                rc=1
                ;;
        esac
        sudo losetup -d "$loop"
    else
        log "image: no sd-root.tar.gz, so p2 was left alone"
    fi

    rmdir "$mnt" 2>/dev/null || true
    return $rc
}

# ── Which of the two things this run produces ─────────────────────────────────
#
# THE 7z IS GONE, AND WITH IT THE REASON THIS SECTION WAS HARD.  There used to be a
# refresh_image_archive() here, and it existed because the image never reached the
# workstation: what reached it was <image>.img.7z.001/.002, written by create_image.sh
# during the BASE build's finalization -- which finishes before this script starts.  So
# the operator unpacked and flashed a snapshot taken before the payload went in, no
# matter how loudly the injection reported success, and the fix was to spend minutes
# re-compressing 8 GB here.  The pipeline now ships the raw .img, which is the same file
# this injects into, so there is nothing to rebuild and nothing that can go stale between
# the two.  J36_IMAGE_EXPORT went with it: there is no archive to opt out of.
#
# Not fatal on failure: the artifacts are complete either way, and a failure here costs
# the operator a manual copy rather than the whole build.  It is loud, though.
BASE_IMAGE="$(find_base_image)"

# ── THE COPY, AND WHY THE BASE IS NEVER THE THING THAT GETS PATCHED ──────────
#
# This section used to inject straight into the base and hand the base over, which made
# the base and the deliverable the same file.  Two things followed from that, and both
# were paid on every single run.
#
# The base could not be trusted, so it could not be kept.  An image that has been
# patched once is not a base any more; patching it again layers this run's payload on
# top of the last one's and there is nothing that says which files came from where.  The
# way the pipeline avoided that was to have build-r36-ultra.sh produce a NEW image every
# commit -- which it did by naming the image after the commit, which invalidated its own
# image checkpoint, which re-ran the entire final stage: a six-minute restore of the
# cached root, an hour of package churn, a fresh 8 GB write.  An hour of work whose only
# product was a clean file to patch.
#
# A copy is that clean file, and it costs about three minutes.  The base is built once
# per Debian release and never written to again; every run copies it, patches the copy,
# and hands the copy over under this checkout's commit.  Nothing is iterative: this run's
# output is a function of the base and this run's payload, and of no run before it.
#
# --sparse=always because both files are mostly holes -- 8.3 GB apparent, a fraction of
# that allocated -- and a copy that filled them in would be the slowest step in the
# build.  Written to .part and renamed, so an interrupted copy cannot be mistaken for an
# image by the next run, or by an operator with a card in the slot.
OUTPUT_IMAGE=""
if [[ -n "$BASE_IMAGE" && "$MIX_ONLY" != 1 ]]; then
    OUTPUT_IMAGE="$ROOT/${J36_IMAGE_NAME:-MixOS_j36_output.img}"
    # The one case where they are allowed to be the same file: an operator who pointed
    # J36_IMAGE_NAME at the base, or a base build that was told to use this name.  Copying
    # a file onto itself would truncate it, so the copy is skipped and the injection goes
    # in place -- the old behaviour, for the one configuration that asks for it.
    if [[ "$OUTPUT_IMAGE" == "$BASE_IMAGE" ]]; then
        log "image: the output name IS the base name, so this run patches the base itself"
    fi
fi

copy_base_to_output() {
    [[ "$OUTPUT_IMAGE" != "$BASE_IMAGE" ]] || return 0
    log "image: copying $(basename "$BASE_IMAGE") to $(basename "$OUTPUT_IMAGE")"
    rm -f "$OUTPUT_IMAGE.part"
    if ! cp --reflink=auto --sparse=always "$BASE_IMAGE" "$OUTPUT_IMAGE.part"; then
        rm -f "$OUTPUT_IMAGE.part"
        log "image: the copy failed, so there is nothing to inject into"
        return 1
    fi
    mv -f "$OUTPUT_IMAGE.part" "$OUTPUT_IMAGE"
    return 0
}

# Every commit made one of these, and four were found in a build VM at 4.3 GB each.  They
# are not intermediates that something might still want: each is a complete flashable
# image from a commit that has been superseded, and the one this run is about to write
# replaces all of them.  The base is not in the glob's shape and cannot be caught by it;
# a `.part' from an interrupted copy is, and is exactly as dead.
prune_previous_outputs() {
    local old
    for old in "$ROOT"/MixOS_*_*_*.img "$ROOT"/MixOS_*_*_*.img.part; do
        [[ -f "$old" ]] || continue
        [[ "$old" != "$OUTPUT_IMAGE" && "$old" != "$BASE_IMAGE" ]] || continue
        case "$(basename "$old")" in
            *_base.img|*-old-layout.img) continue ;;
        esac
        log "image: removing $(basename "$old"), superseded by this run"
        rm -f "$old"
    done
}

if [[ "$MIX_ONLY" == 1 ]]; then
    log "image: --mix-only, so the base image is left alone.  boot/ and root/ below are"
    log "image: what this run produced; copy boot/ onto the card's BOOT partition and the"
    log "image: next boot unpacks sd-root.tar.gz from it onto the OS partition."
    rm -f "$IMAGE_STAMP"
elif [[ -z "$BASE_IMAGE" ]]; then
    log "image: no MixOS_*.img in $ROOT, so nothing to fold the payload into."
    log "image: Run the full build (no --mix-only) to make one."
    # Named, not injected into: see find_base_image.
    for stale in "$ROOT"/dArkOS_*.img; do
        [[ -e "$stale" ]] || continue
        log "image: NOTE $(basename "$stale") is here but predates the ext2 layout and the"
        log "image: rename, so it is not a base for today's payload."
    done
elif [[ -f "$IMAGE_STAMP" && -s "$OUTPUT_IMAGE" ]] && \
     [[ "$(cat "$IMAGE_STAMP")" == "$(image_export_signature "$BASE_IMAGE")" ]]; then
    log "image: $(basename "$OUTPUT_IMAGE") was built from this base with this exact"
    log "image: payload -- nothing to copy and nothing to inject."
    prune_previous_outputs
elif copy_base_to_output && inject_into_image "$OUTPUT_IMAGE"; then
    image_export_signature "$BASE_IMAGE" > "$IMAGE_STAMP"
    prune_previous_outputs
else
    log "image: SOME OR ALL of the payload did not reach the image -- read the lines"
    log "image: above.  sd-boot/ and sd-root.tar.gz are still in the artifacts and can"
    log "image: be copied onto the card from a Linux machine."
    rm -f "$IMAGE_STAMP"
fi

# What build-j36-ultra.sh reads to decide what to hand over, and what to say while doing
# it.  Written even when there is no image, because "there is no flashable image" is the
# thing the operator most needs told.  `volumes=' is gone with the archive; the wrapper
# copies one file now.
{
    if [[ "$MIX_ONLY" == 1 ]]; then
        printf 'image=mix-only\n'
        printf 'payload=exported\n'
    elif [[ -s "$OUTPUT_IMAGE" ]] && [[ -f "$IMAGE_STAMP" ]]; then
        printf 'image=%s\n' "$(basename "$OUTPUT_IMAGE")"
        printf 'payload=in-image\n'
    elif [[ -s "$OUTPUT_IMAGE" ]]; then
        # A copy that was made and then not fully injected into.  It is named rather than
        # hidden, because it is still a bootable base and the operator may want it -- but
        # `stale' is what stops the wrapper from handing it over as this run's article.
        printf 'image=%s\n' "$(basename "$OUTPUT_IMAGE")"
        printf 'payload=stale\n'
    elif [[ -n "$BASE_IMAGE" ]]; then
        # The copy itself failed, so what is here is the untouched base.  Naming it says
        # there is something to flash and that it carries none of this run's work.
        printf 'image=%s\n' "$(basename "$BASE_IMAGE")"
        printf 'payload=stale\n'
    else
        printf 'image=none\n'
        printf 'payload=stale\n'
    fi
} > "$ARTIFACTS/flashable-image.txt"

# ── boot/ and root/, and nothing else ─────────────────────────────────────────
#
# WHY ONLY TWO DIRECTORIES, AND ONLY IN --mix-only.  This used to rsync the whole of
# $ARTIFACTS to the host on every run: boot.img, the bare zImage, the dtb, the .ko, the
# cpio, the tarball, the manifest, the sums -- twenty-odd files of which the operator
# copies exactly two trees onto the card.  The rest are intermediates that the image
# already contains, and having them on the workstation next to a flashable image invites
# copying the stale one.  So a full build exports nothing at all (its output is the
# image), and --mix-only exports the two trees the card is updated from:
#
#   boot/  = $SDBOOT, the FAT32 launcher, sd-root.tar.gz included.  Complete on its own:
#            macOS can write it, and /init unpacks the tarball onto the OS partition once
#            per tarball, so this alone updates an already-flashed card.
#   root/  = $SDROOT, the same payload unpacked, for copying onto the OS partition
#            directly from a Linux machine (or just for reading what was built).
#
# --delete over the two of them and a sweep of anything else at the top level, because
# this directory is the operator's staging area: an old boot.img sitting next to boot/
# is a file that looks current and is not.
if [[ "$MIX_ONLY" == 1 ]]; then
    # --omit-link-times, because $EXPORT_DIR is a share and not a filesystem.  The Qt
    # payload has symlinks in it -- lib/libQt5Core.so.5 and its thirty-odd SONAME
    # aliases -- and utimensat(AT_SYMLINK_NOFOLLOW) on the host share answers EOVERFLOW,
    # which rsync reports as "failed to set times ... Value too large for defined data
    # type" once per link and then exits 23.  The links themselves copy correctly; it is
    # only their mtime that cannot be set, and nothing reads a symlink's mtime.
    rsync -a --omit-link-times --delete "$SDBOOT/" "$EXPORT_DIR/boot/"
    if [[ -d "$SDROOT" ]]; then
        rsync -a --omit-link-times --delete "$SDROOT/" "$EXPORT_DIR/root/"
    else
        log "export: nothing staged for the OS partition, so no root/"
        rm -rf "$EXPORT_DIR/root"
    fi

    # Everything the old blanket rsync used to leave here.  Removed rather than left,
    # for the reason above; done by name-exclusion rather than a wipe-and-recopy so that
    # the two trees keep their incremental rsync.
    for leftover in "$EXPORT_DIR"/*; do
        [[ -e "$leftover" ]] || continue
        case "$(basename "$leftover")" in
            boot|root) continue ;;
        esac
        log "export: removing $(basename "$leftover"), which this build no longer ships"
        rm -rf "$leftover"
    done

    log "J36 Ultra board artifacts are ready (--mix-only; the base image was not touched)"
    printf '  %s\n' \
        "$EXPORT_DIR/boot/ -> the card's BOOT partition (vfat; macOS can write it)" \
        "$EXPORT_DIR/root/ -> the card's OS partition, opt/mixos and all (ext2; Linux only)"
else
    log "J36 Ultra board artifacts are in the image; nothing was exported separately"
    log "(--mix-only exports boot/ and root/ for updating a card without reflashing)"
fi
