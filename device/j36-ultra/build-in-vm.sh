#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
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
# default now that EmulationStation draws on it -- J36_DOOM=1 stages it again.
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

[[ "$(uname -s)" == Linux ]] || die "build-in-vm.sh must run on Linux"
[[ -d "$DRIVERS" ]] || die "MVII J36 Drivers not found: $DRIVERS"

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

# ── The two changes this build makes to the kernel itself ─────────────────────
#
# mtk-sd carries no compatible for MT6592 and refuses to probe without pinctrl;
# linux/0001-mtk-sd-mt6592.patch fixes both, and its header records why neither
# can be worked around from the device tree instead.
#
# linux/0002-drm-mediatek-mt6592.patch is the display half, and it is four hunks
# in two files because MT6592's DDP is the MT2701/MT8173 generation: only the
# pipeline order and the OVL colour-format numbering are genuinely this SoC's own.
# Its header records, register by register, what was measured against the MVII LK
# and against the stock 3.4 kernel to prove the rest of the mt2701 driver data
# exact.  Everything else that port needs is device tree, not code.
#
# Applied idempotently rather than from a stamp file, because this checkout
# persists across runs and a stamp can outlive the tree it describes -- a
# J36_UPDATE_KERNEL reset above throws the patch away and would leave the stamp
# claiming otherwise.  `apply --reverse --check' succeeds only when the change is
# already present, so the tree is asked instead of a bookkeeping file.
apply_kernel_patch() {
    local patch="$ROOT/device/j36-ultra/linux/$1" what="$2"
    [[ -f "$patch" ]] || die "missing kernel patch: $patch"
    if git -C "$KERNEL_SRC" apply --reverse --check "$patch" 2>/dev/null; then
        log "The $what patch is already applied"
    elif git -C "$KERNEL_SRC" apply --check "$patch" 2>/dev/null; then
        log "Applying the $what patch"
        git -C "$KERNEL_SRC" apply "$patch"
    else
        die "$1 neither applies to nor is applied in $KERNEL_SRC; refresh it against $KERNEL_BRANCH"
    fi
}
apply_kernel_patch 0001-mtk-sd-mt6592.patch "mtk-sd MT6592"
apply_kernel_patch 0002-drm-mediatek-mt6592.patch "mtk_drm MT6592 display"

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
# WIRELESS, WLAN and BT stay off, and they are disabled here rather than left
# out: all three live inside `if NET' in net/Kconfig and WIRELESS defaults to y,
# so once NET is on they would come back by default. Both the =y for NET and the
# "is not set" lines for these are in .config before the single olddefconfig
# below, which is what makes the explicit n stick.
#
# SCSI USED TO BE IN THIS LIST and is not any more.  It left for one reason: a USB
# disk is a SCSI device.  usb-storage is a SCSI host adapter that speaks Bulk-Only
# Transport, sd_mod is what turns the LUN behind it into /dev/sda, and there is no
# arrangement of the USB menu that reaches a mountable partition without both. So
# the storage section below turns SCSI on as a MODULE and prunes the rest of that
# menu by name; ATA stays refused, because libata is the other thing under SCSI
# and there is no SATA or PATA anywhere on this SoC.
for symbol in \
    MEDIA_SUPPORT WIRELESS WLAN BT ATA \
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
# EXFAT and VFAT are not for /init -- they are for the other partitions on the same
# card, and the rootfs mounts those itself.  finishing_touches.sh writes the
# post-expansion fstab as
#
#   LABEL=BOOT /boot vfat defaults 0 2
#   LABEL=DATA /home/virtua ext2 defaults,auto,noatime,nofail 0 2
#
# and firstboot installs it over /etc/fstab as its last act, so from the second boot
# onwards this kernel is asked for both.  p3 is the login user's home directory now,
# ext2 and labelled DATA -- it used to be exfat and called EASYROMS, and exfat stays
# built in for the cards already written that way.  The BOOT entry carries no nofail,
# so a vfat driver this kernel did not have would fail local-fs and take a machine
# with no keyboard driver into emergency mode; the home entry does carry it, because
# a card whose p3 is missing or unformatted still has to reach a shell.
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
# same off j36,drvvbus-pad.  So a bus-powered hub enumerates.  The cost is that
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
                DRM DEVMEM SOUND POWER_SUPPLY BACKLIGHT_CLASS_DEVICE \
                USB_SUPPORT USB_PHY GENERIC_PHY HID_SUPPORT \
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

# Off on purpose, and worth failing over: WIRELESS defaults to y under NET, and
# an accidental =y here drags cfg80211 and a WLAN menu into an image with 2.5 MiB
# of slack in a fixed partition.
for refused in WIRELESS BT; do
    if grep -q "^CONFIG_${refused}=y$" "$CONFIG"; then
        die "CONFIG_${refused}=y came back after olddefconfig; it must stay off until the WiFi driver lands"
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

log "Building the out-of-tree J36 modules: the input adapter, the panel, the AFE, the USB PHY, the PMIC and the backlight"
mkdir -p "$MODULE_SRC"
rsync -a --delete "$ROOT/device/j36-ultra/linux/" "$MODULE_SRC/"
make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- M="$MODULE_SRC" -j"$(nproc)" modules
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
# And the USB PHY, staged with the usb payload.  It is the first module of the
# USB chain to load, because it is the only hook that runs before musb_hdrc
# touches 0x11200000 -- and an APB read of that window while PERI still gates it
# hangs the bus rather than faulting.  A missing .ko here would put musb_hdrc
# first in load.order, so check it at build time.
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
# systemd drop-in pointing LD_LIBRARY_PATH at it was written, and the directory
# stayed empty. EmulationStation's one GL DT_NEEDED is the bare name `libEGL.so',
# so the loader missed the empty path and resolved it in /usr/lib, where the
# shared rootfs has pointed that name at the RK3326's libMali.so. That blob is Tag_CPU_arch v8
# -- ARMv8-A -- and this SoC is a Cortex-A7. ES died on SIGILL, status 132,
# before main(), six times, until systemd gave up on the restart counter.
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
# rmdir is in this list because /init runs it, and it was missing: mount_card removes
# its own mount point when there is nothing to mount there and again when it replaces
# it with a symlink to the home partition.  A missing applet is not a no-op in the
# second case -- `ln -s target dir' with dir still present creates the link INSIDE it,
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

    make -C "$BUSYBOX_SRC" CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)"
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
# Nor is this one, and without it `mount -o bind' silently becomes a mount attempt
# with a filesystem type of "bind": busybox parses -o flag words only when
# FEATURE_MOUNT_FLAGS is on.  /init bind-mounts the GLES 2.0 EmulationStation over
# the rootfs's, which is the one place in the boot that needs it.
grep -q '^CONFIG_FEATURE_MOUNT_FLAGS=y$' "$BUSYBOX_SRC/.config" || \
    die "busybox CONFIG_FEATURE_MOUNT_FLAGS is off; /init needs \`mount -o bind'"

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
usb_vbus=1
want_power=0
power_charge=1
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        j36.audio|j36.audio=1)
            want_audio=1
            ;;
        # The class-D amp, and it is a separate word because it is the only thing
        # in this payload that can switch the board off.  The amp hangs off VBAT,
        # which on this PMIC is the system node: with no cell fitted VBAT is held
        # up only by the charger's current source, and the amp at output pulls it
        # under the undervoltage lockout.  So a card configured for audio still
        # boots silent, and this word is the deliberate second step -- on a board
        # with a cell in it.
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
        # cannot carry.  With it the pad stays exactly as the LK left it.
        j36.usb=novbus)
            want_usb=1
            usb_vbus=0
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
        # Mesa, staged where the loader will find it ahead of the RK3326 blob.
        # j36.es is the name this word had while EmulationStation was the thing
        # that used it; cards written before the dashboard existed still say it,
        # and they still boot.
        j36.gl|j36.gl=1|j36.es|j36.es=1)
            want_gl=1
            ;;
        # Same payload, plus the things that make a failed GL bring-up say why:
        # Mesa's EGL loader trace, and eglprobe run before the shell starts.  It
        # is a separate word rather than a build option because boot.conf is on
        # the vfat partition, so it can be turned off from any machine that can
        # read the card.
        j36.gl=debug|j36.es=debug)
            want_gl=1
            gl_debug=1
            ;;
        # The dashboard, in place of EmulationStation.  A word of its own because
        # the two are alternatives: this one writes a drop-in that resets the
        # unit's ExecStart, so with it the shell is mixdash and without it the
        # rootfs's own EmulationStation still starts.
        j36.dash|j36.dash=1)
            want_dash=1
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

# Set once, read by run_lima, run_mtkdrm, run_audio and setup_es_gl.  Empty means
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
# The OS partition is ext2 and the home partition is ext2, and macOS mounts neither.
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
    # headline for that reason, and no progress number: the bar would sit still
    # for a minute and look wedged either way, so it eases toward the next step
    # instead.
    stage "Installing the update"
    detail "unpacking sd-root.tar.gz -- this takes a minute"
    mkdir -p /newroot/opt
    gunzip -c /bootfs/sd-root.tar.gz | tar -x -C /newroot
    # tar's exit status is not the test.  This is a pipeline in ash, so what is
    # reported is tar's, and a gunzip that dies half way through a truncated file
    # leaves tar perfectly happy with the part it did get.  What matters is whether
    # the thing this exists to install is there and executable.
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
    if [ "$audio_speaker" != 1 ]; then
        say "audio: the speaker amp is off -- add j36.audio=speaker with a cell fitted"
    fi
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
# is a DRM driver, so it depends on drm_kms_helper and drm_shmem_helper -- the
# same two modules the mtkdrm payload stages in its own directory.  A boot that
# asks for both words would otherwise reach the second copy and insmod would fail
# with EEXIST, which reads in the log exactly like a broken module.  /sys/module
# names have underscores where filenames have hyphens, hence the tr.
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
        # one of these is loadable.  So j36.usb=novbus is translated here.
        args=""
        case "$ko" in
            j36_mt6592_usb_phy.ko)
                [ "$usb_vbus" = 1 ] || args="vbus=0"
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
    # the pad it drove; this says which of the two arrangements the card asked
    # for, in the words the operator would have to change.
    if [ "$usb_vbus" = 1 ]; then
        say "usb: the port is sourcing 5 V off VBAT -- fit a cell, or say j36.usb=novbus"
    else
        say "usb: VBUS held off by j36.usb=novbus -- the hub must have its own power"
    fi
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
# LOAD IT AFTER USB, not before, and the ordering is real rather than tidiness.
# The PMIC reads GPIO pad 15 every poll to find out whether this port is
# sourcing 5 V, because CHRDET cannot tell our own boost from a charger and
# arming the charger against a rail we are driving is the one genuinely bad
# outcome available here.  Loading the PHY first means the pad is already in
# whatever state the boot asked for by the time the first poll looks at it,
# rather than the driver seeing the LK's state once and reporting a charger that
# is not there.  It recovers either way -- the interlock is re-read every second
# -- but the first second of log is worth getting right.
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
    # `battery' and `usb' are not arbitrary.  mixdash walks
    # /sys/class/power_supply/* looking for one whose type is Battery, and
    # batt_led.service reads /sys/class/power_supply/battery/capacity by that
    # literal path, so the directory being called `battery' is what makes both
    # of them work without either one being told about this driver.
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

# ── EmulationStation's GL front end, installed without touching the rootfs ───
#
# This is the last link in the chain: card0 from mtkdrm, a render node from lima,
# and now a libEGL/libgbm/libGLES that are Mesa's rather than the RK3326 blob's.
#
# NOTHING ON THE SHARED ROOTFS IS WRITTEN, and that is the whole design of this
# function. The card carries one Debian rootfs for two machines, and the R36S needs
# its libEGL.so -> libMali.so symlinks to stay exactly where they are: that blob is
# the only thing that drives its Mali-G31. So the five libraries go into a tmpfs
# and the environment that points at them goes into a systemd drop-in in the same
# tmpfs. Pull this card into an R36S and there is no trace of any of it.
#
# The mechanism is the initrd's half of a contract systemd documents: /run must be
# a tmpfs, and if the initrd has already mounted one, PID 1 adopts it with its
# contents rather than mounting its own over the top. Drop-ins under
# /run/systemd/system/<unit>.d/ are read exactly like ones in /etc, and they are
# read AFTER the unit file, so an Environment= here replaces the unit's own
# SDL_VIDEO_EGL_DRIVER=libEGL.so instead of adding to it.
#
# Why each variable is set:
#   LD_LIBRARY_PATH   puts the five staged libraries ahead of
#                     /usr/lib/arm-linux-gnueabihf, where the blob's symlinks live.
#                     Ahead of, not instead of: everything else EmulationStation
#                     needs -- SDL2, freetype, VLC, and Mesa's own back end,
#                     libgallium and dri/lima_dri.so and dri/mediatek_dri.so --
#                     still comes from the rootfs, untouched.
#   LD_PRELOAD        Only for the rootfs's own binary, and only as a fallback:
#                     its 29 undefined GL symbols are GLES1 fixed-function calls
#                     and it names only libEGL.so as a dependency, because the blob
#                     it was linked against exported both from one object. glvnd's
#                     libEGL does not export a single gl* entry point, so the GLES1
#                     library has to be forced into the global scope or the binary
#                     will not start. The GLES 2.0 binary staged below needs none
#                     of this -- it has no GL library in its DT_NEEDED.
#   SDL_VIDEODRIVER   kmsdrm. SDL2 here has KMSDRM, wayland, offscreen and dummy
#                     and no fbdev at all, so this is the only backend that can
#                     drive this panel, and naming it stops SDL trying wayland
#                     first on a machine with no compositor.
#   SDL_VIDEO_*_DRIVER  the versioned SONAMEs, because SDL dlopens by name and the
#                     unversioned ones in /usr/lib are the blob's symlinks.
# One tmpfs, two callers.  The GL payload and the dashboard both put their files
# and their drop-in under /newroot/run, and both are optional, so whichever runs
# first mounts it -- and mounting a second one over the top would hide the first
# one's work.  It has to be a tmpfs and not the rootfs's own /run because of the
# invariant this whole card is built on: nothing on the shared rootfs is written.
run_tmpfs=0
ensure_run_tmpfs() {
    if [ "$run_tmpfs" = 1 ]; then return 0; fi
    if mount -t tmpfs -o mode=0755 tmpfs /newroot/run 2>/dev/null; then
        run_tmpfs=1
        return 0
    fi
    return 1
}

# Set by setup_es_gl for setup_dash to read: whether the Mesa payload is complete
# enough to point a child at, and whether the probe was staged.  They are separate
# because the payload can be there without the probe and the dashboard's 3D cube
# card needs the first while the boot-time diagnostics need the second.
gl_ready=0
probe_ready=0

setup_es_gl() {
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
            say "es: could not copy $so"
        fi
    done
    # The links file stands in for the symlinks vfat cannot store: "name target", one
    # pair per line, targets relative to this directory except libEGL.so's, which
    # is a plain filename too.  Read whichever partition the payload came off, because
    # a card written before the payload moved has it on the vfat one.
    while read -r name target; do
        case "$name" in ''|'#'*) continue ;; esac
        ln -sf "$target" "/newroot/run/j36/gl/$name"
    done < "$payload/gl/links"

    # ── The GLES 2.0 EmulationStation ────────────────────────────────────────────
    #
    # j36/es/emulationstation is built from the same upstream commit as the one in
    # the rootfs, with one difference: -DUSE_OPENGLES_20 and Renderer_GLES20.cpp
    # instead of -DUSE_OPENGLES_10 and Renderer_GLES10.cpp.
    #
    # Why a different renderer at all, measured rather than assumed: GLES1 is the
    # one API this stack cannot give. eglCreateContext for an ES1 context returns
    # EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike, because Debian's
    # Mesa 25.0.7 is a -Dgles1=disabled build -- the package, not this SoC. ES2 is
    # what does come up on lima, as "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1". The
    # rootfs's binary cannot ask for it: its renderer is fixed-function, and
    # Renderer_GLES10.cpp does
    #     std::string glExts = (const char *)glGetString(GL_EXTENSIONS);
    # at line 129 without ever checking SDL_GL_CreateContext, so a context that was
    # never created arrives as std::string(NULL) -- abort, status 134, reason
    # discarded. That 134 is the whole reason this file exists.
    #
    # It goes in over the rootfs's copy as a BIND MOUNT, which is the only way to
    # replace it without writing to a card the R36S also boots: the mount table is
    # in memory, the file underneath is untouched, and switch_root moves the mount
    # across with everything else under /newroot. Pull this card into an R36S and
    # its own binary is the one that runs.
    #
    # /run/j36/es and not /run/j36/gl, for the same reason the probe is kept out of
    # there: /run/j36/gl is an ld.so search path and an executable in it is a name
    # the loader has to stat and skip.
    es_gles20=0
    if [ "$want_dash" = 1 ]; then
        # j36.dash=1 means EmulationStation is not the shell on this boot and its
        # binary is never executed, so replacing that binary is work with no
        # observable effect.  The bind mount is skipped rather than made harmless:
        # a mount over a file on the shared rootfs that nothing will open is still a
        # mount somebody would have to explain.
        say "es: j36.dash=1, so the GLES 2.0 EmulationStation is not staged at all"
    elif [ -f "$payload/es/emulationstation" ]; then
        mkdir -p /newroot/run/j36/es
        if cp "$payload/es/emulationstation" /newroot/run/j36/es/emulationstation; then
            chmod 0755 /newroot/run/j36/es/emulationstation
            if [ ! -f /newroot/usr/bin/emulationstation/emulationstation ]; then
                say "es: the rootfs has no emulationstation to mount over"
            # -o bind rather than --bind: this is BusyBox's mount.
            elif mount -o bind /newroot/run/j36/es/emulationstation \
                               /newroot/usr/bin/emulationstation/emulationstation; then
                es_gles20=1
                say "es: the GLES 2.0 binary is mounted over the rootfs's"
            else
                say "es: could not bind-mount the GLES 2.0 binary; the rootfs's GLES1 one will run"
            fi
        else
            say "es: could not copy the GLES 2.0 binary"
        fi
    else
        say "es: no j36/es/emulationstation on the card, so the rootfs's GLES1 binary will run"
    fi

    # The EGL probe rides in beside the libraries, not among them: /run/j36/gl is
    # a loader search path and a binary in it would be a name ld.so has to skip.
    # It is only ever run by the j36.es=debug drop-in below.
    if [ -f "$payload/eglprobe" ]; then
        if cp "$payload/eglprobe" /newroot/run/j36/eglprobe; then
            chmod 0755 /newroot/run/j36/eglprobe
            probe_ready=1
            # The drop-in tees the probe's output to a file so ExecStopPost can
            # print it again after ES's own spew has scrolled past.  emulation-
            # station.service is User=ark and this directory is root-owned 0755,
            # so tee could not create that file -- it said "permission denied"
            # and the repeat had nothing to print.  Create it here, where we are
            # root, and let anyone write it.  A log in a tmpfs that is thrown
            # away on reboot does not need to be guarded.
            : > /newroot/run/j36/eglprobe.log
            chmod 0666 /newroot/run/j36/eglprobe.log
        else
            say "es: could not copy the EGL probe"
        fi
    fi

    # Nothing above is allowed to fail quietly, because of what the fallback is.
    # The drop-in tells the loader to look in this directory; if the directory is
    # empty the loader simply misses and finds the same names in /usr/lib, where
    # the shared rootfs has pointed them at the RK3326's libMali.so -- an ARMv8-A object on a
    # Cortex-A7. For the rootfs's binary that is SIGILL before main(), which names
    # neither this directory nor the blob; for the GLES 2.0 one it is SDL dlopening
    # the blob and reporting no EGL. Neither message points here, so it has to be
    # caught here.
    #
    # Which three are load-bearing depends on which binary is going to run, and
    # they are not the same three:
    #
    #   GLES 2.0 binary.  It has no GL library in its DT_NEEDED at all, so the bare
    #     libEGL.so alias stops mattering.  What matters is the pair SDL dlopens:
    #     libEGL.so.1 for the context and libGLESv2.so.2 for the entry points
    #     SDL_GL_GetProcAddress hands back.
    #   Rootfs's GLES1 binary.  libEGL.so is the literal string in its DT_NEEDED,
    #     and libGL.so.1 is the LD_PRELOAD that supplies the fixed-function calls.
    #
    # libgbm.so.1 is load-bearing either way, and twice over: SDL2's KMSDRM backend
    # dlopens it and libEGL_mesa.so.0 carries it in its own DT_NEEDED, so without it
    # Mesa's EGL pulls the RK3326 blob in as its gbm and cannot initialise.
    #   The dashboard.  mixdash itself needs none of them -- it is Qt on linuxfb and
    #     it draws with the CPU -- but the 3D cube card runs eglprobe, which dlopens
    #     the same pair the GLES 2.0 binary does.
    #
    # libgbm.so.1 is load-bearing either way, and twice over: SDL2's KMSDRM backend
    # dlopens it and libEGL_mesa.so.0 carries it in its own DT_NEEDED, so without it
    # Mesa's EGL pulls the RK3326 blob in as its gbm and cannot initialise.
    missing=""
    if [ "$es_gles20" = 1 ] || [ "$want_dash" = 1 ]; then
        needs="libEGL.so.1 libgbm.so.1 libGLESv2.so.2"
    else
        needs="libEGL.so libgbm.so.1 libGL.so.1"
    fi
    for need in $needs; do
        [ -e "/newroot/run/j36/gl/$need" ] || missing="$missing $need"
    done
    if [ -n "$missing" ]; then
        say "es: the GL payload is incomplete, missing:$missing"
        say "    ($staged of the libraries copied.)  The drop-in is deliberately"
        say "    NOT written: pointing LD_LIBRARY_PATH at a directory that cannot"
        say "    satisfy those names sends ES to /usr/lib and the RK3326 Mali blob,"
        say "    which is ARMv8-A on this Cortex-A7.  Leaving the environment alone"
        say "    fails in the same place but without this initramfs having claimed"
        say "    to fix it."
        return 1
    fi
    gl_ready=1

    # And with the dashboard as the shell there is no EmulationStation drop-in to
    # write: every line below configures SDL, a videodriver and a GL front end for a
    # binary that will not be executed on this boot.  setup_dash puts the one thing
    # from here that a Qt dashboard still needs -- the search path its children use
    # to find Mesa rather than the RK3326 blob -- into mixdash.service, where it can
    # be read next to the process it applies to instead of inherited from a unit
    # named after a program that is gone.
    if [ "$want_dash" = 1 ]; then
        say "gl: payload in /run/j36/gl; no ES drop-in, the dashboard is the shell"
        say "    $(ls /newroot/run/j36/gl | tr '\n' ' ')"
        return 0
    fi

    mkdir -p /newroot/run/systemd/system/emulationstation.service.d
    cat > /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPIN'
# Written by the J36 Ultra initramfs, into a tmpfs. Not on the card, not in the
# rootfs: it exists only for as long as this boot does.
[Service]
Environment="LD_LIBRARY_PATH=/run/j36/gl"
Environment="SDL_VIDEODRIVER=kmsdrm"
Environment="SDL_VIDEO_EGL_DRIVER=libEGL.so.1"
DROPIN

    # The GL front end SDL is told to dlopen, and it follows the binary rather than
    # the board -- these two paragraphs are the whole difference between them.
    if [ "$es_gles20" = 1 ]; then
        cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINES2'

# The GLES 2.0 binary is the one mounted, and ES2 is the one API measured to build
# a context on lima here: "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1".  ES1 cannot,
# anywhere -- Debian's Mesa 25.0.7 is a -Dgles1=disabled build, so eglCreateContext
# returns EGL_BAD_ALLOC for it on lima, on llvmpipe and on softpipe alike.
#
# No LD_PRELOAD.  Renderer_GLES20.cpp resolves all 43 entry points through
# SDL_GL_GetProcAddress, which under this videodriver is eglGetProcAddress, so the
# binary carries no GL library in its DT_NEEDED and there is nothing that has to be
# forced into the global scope.  That is also what makes one binary correct on both
# machines: nothing in it names libMali.so or libGLESv2.so.
Environment="SDL_VIDEO_GL_DRIVER=libGLESv2.so.2"
DROPINES2
    else
        cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINGL'

# Fallback: the rootfs's own binary, which is fixed-function.  The GL_DRIVER name
# is libGL and not libGLESv1_CM because a GLES1 CONTEXT cannot be created at all
# here -- Debian's Mesa 25.0.7 is built without GLES1, so eglCreateContext for ES1
# returns EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike.  What does come
# up is desktop GL as a COMPATIBILITY profile, and compat GL is a superset of the
# fixed-function subset that binary uses: all 29 gl* symbols in its undefined list
# are exported by glvnd's libGL.so.1 with the signatures GLES1 gives them.  It is
# also the context SDL asks for anyway -- Renderer_GLES10.cpp's setupWindow() never
# sets SDL_GL_CONTEXT_PROFILE_MASK and sets MAJOR_VERSION twice, so SDL sends no
# context attribs and EGL falls back to its own default of OpenGL.
#
# What the preload does NOT do is fix a broken dispatch, because there was not one.
# Measured against the image's own libraries: with a compat context current, the
# GLES1 stub and libGL both return "4.5 (Compatibility Profile) Mesa 25.0.7" from
# glGetString and both accept glMatrixMode/glLoadMatrixf/glEnableClientState with
# glGetError 0 -- glvnd gives libEGL and both GL front ends one shared
# current-context table.  It is there because that binary names only libEGL.so as a
# dependency and glvnd's libEGL exports no gl* entry point at all.
#
# This path has never drawn a frame on this board: it is the 134.  It is kept
# because it is what happens when j36/es is not on the card, and it should say so
# rather than look like the intended configuration.
Environment="LD_PRELOAD=libGL.so.1"
Environment="SDL_VIDEO_GL_DRIVER=libGL.so.1"
DROPINGL
    fi

    # j36.es=debug appends the diagnostics.  Kept out of the default drop-in
    # because it is noise on a board that works, and kept in the same file
    # because systemd merges drop-ins in name order and one file cannot be
    # overridden by half of itself.
    #
    # What each line buys, and why this particular set:
    #
    #   --debug          EmulationStation's log is normally a file that is never
    #                    written.  Log::init() sees LogLevel=disabled in
    #                    es_settings.cfg, deletes the file and returns, so
    #                    es_log.txt does not exist -- but Log::~Log() also writes
    #                    to stderr whenever the reporting level is LogDebug or
    #                    above, and --debug sets exactly that.  So this puts ES's
    #                    whole trace on the panel without touching the rootfs, and
    #                    the launcher passes "$@" through to the binary.
    #   EGL_LOG_LEVEL    Mesa's EGL prints which vendor it loaded, which DRI
    #                    driver it paired the card with, and the reason
    #                    eglInitialize or eglCreateContext failed.  This is the
    #                    one that matters most, because of what ES does NOT do:
    #                    Renderer_GLES10.cpp calls SDL_GL_CreateContext and
    #                    SDL_GL_MakeCurrent and checks neither return value, then
    #                    at line 129 does
    #                        std::string glExts = (const char *)glGetString(GL_EXTENSIONS);
    #                    With no current context glvnd's libGL stub returns NULL,
    #                    and std::string(NULL) throws std::logic_error
    #                    "basic_string: construction from null is not valid" --
    #                    abort, status 134.  So a 134 means the context failed and
    #                    the reason was thrown away.  Mesa is the only party left
    #                    that still knows it.
    #   MESA_DEBUG       Mesa's own GL error stream, in case a context is created
    #                    and then something inside it is rejected.
    #   LIBGL_DEBUG      names the dri module it tried to dlopen when it cannot.
    #                    dri/mediatek_dri.so and dri/lima_dri.so are both symlinks
    #                    to one megadriver here, and a failure to find either
    #                    looks identical to a failure to find a GPU.
    #   J36_ES_GL_PROBE  read by the GLES 2.0 renderer only.  It turns the clear
    #                    colour magenta and, either way, that renderer logs a
    #                    startup self test -- one magenta quad drawn straight in
    #                    NDC and read back with glReadPixels -- plus a draw count
    #                    per frame.  Between them they split the one symptom that
    #                    has three causes:
    #                      magenta panel      the swap chain reaches the CRTC, so a
    #                                         black UI on top of it is ES's drawing
    #                      black panel, self
    #                      test pixel ff00ff  GL is correct and the frames never
    #                                         reach the panel -- a display problem,
    #                                         not a GL one
    #                      self test black    the pipeline swallows the draw; the
    #                                         compile and link lines say why
    #                      frame N, 0 draws   ES asked for nothing; a theme or a
    #                                         gamelist, not a shader
    #                    The fixed-function binary ignores it entirely.
    #   Restart=no       the unit restarts on failure, and six identical stack
    #                    traces scroll the first one off a 640x480 panel before it
    #                    can be read.  One attempt, one trace.
    #   StandardError    the trace is on stderr, and this boot's whole purpose is
    #                    to read it off the panel, so it is stated rather than
    #                    inherited.  StandardOutput joins it because the probe
    #                    below writes to stdout.
    if [ "$gl_debug" = 1 ]; then
        cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINDBG'

# j36.es=debug
Environment="EGL_LOG_LEVEL=debug"
Environment="MESA_DEBUG=1"
Environment="LIBGL_DEBUG=verbose"
Environment="J36_ES_GL_PROBE=1"
Restart=no
StandardOutput=journal+console
StandardError=journal+console
DROPINDBG

        # The `--debug' ExecStart override that used to be in that block is written
        # only when EmulationStation is the shell, and that is not a tidiness point.
        # neuter_es replaces emulationstation.service in the SAME directory with a
        # unit that only echoes a line, and a drop-in beside it outranks nothing --
        # it MERGES.  So `ExecStart=' followed by the real binary resurrected the
        # very thing j36.dash=1 exists to keep out of the boot: under j36.gl=debug
        # the stub's echo was cleared and ES ran, aborted at
        # Renderer_GLES10.cpp:129, and did it while holding the panel.
        if [ "$want_dash" != 1 ]; then
            cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINDBGES'
ExecStart=
ExecStart=/usr/bin/emulationstation/emulationstation.sh --debug
DROPINDBGES
        else
            say "gl: j36.dash=1, so the debug drop-in does not re-add an ExecStart --"
            say "    a drop-in merges with the stub unit and would start ES anyway"
        fi

        # And the probe, if it was staged.  Written separately so that a card
        # without it produces a drop-in that does not mention it, rather than a
        # tolerated failure the reader has to recognise as harmless.
        #
        # ExecStartPre inherits this unit's Environment, which is the point: the
        # probe resolves libEGL.so.1 and libgbm.so.1 through LD_LIBRARY_PATH the
        # same way ES will, so it reports on the payload rather than on Debian's
        # copies.  It is prefixed with - because a probe is not a precondition:
        # whatever it says, ES still gets its attempt.
        #
        # It runs before ES and is repeated after ES exits because of the panel:
        # ES with --debug prints more than 30 lines and the probe's verdict would
        # scroll away before it could be photographed.  The second copy comes
        # from the log rather than a second run, so it cannot disagree with the
        # first.
        # The second line is the same probe with no DRM device at all and the
        # software driver forced.  swrast does desktop GL, GLES1 and GLES2
        # everywhere, so it says whether this Mesa can build those contexts at
        # all -- and a row that works there but returns BAD_ALLOC on the nodes
        # above is lima's, not the payload's.  It appends to the same log so the
        # repeat after the shell exits carries both.
        #
        # WHAT IS NOT HERE ANY MORE IS -p, and the reason is worth writing down.
        # -p sets a mode of its own, and on a board with no fbdev emulation
        # nothing puts the scanout back afterwards: the panel keeps showing -p's
        # last frame and the shell that starts next -- which draws into the LK's
        # framebuffer through /dev/fb0 -- is never seen again.  Run before
        # EmulationStation that cost five colours and nothing else, because ES
        # would have taken the panel with a modeset of its own.  Run before
        # mixdash it would hide the dashboard for the whole boot.  So it is a
        # thing the dashboard's own 3D cube card starts, on purpose, after asking
        # twice -- and the first line below already prints what -p's opening
        # lines were the useful part of: which node is lima and which one, if
        # any, can modeset at all.
        #
        # Same reasoning as the ExecStart above: this drop-in merges into whatever
        # emulationstation.service is, stub included, so with the dashboard as the
        # shell these lines were a SECOND full EGL probe in the boot -- one from the
        # stub unit and one from mixdash's own ExecStartPre.  Every one of them
        # creates contexts on lima, and this board does not survive many of those.
        # The dashboard boot gets exactly one, from mixdash-probe.service.
        if [ -x /newroot/run/j36/eglprobe ] && [ "$want_dash" != 1 ]; then
            cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINPROBE'
ExecStartPre=-/bin/sh -c '/run/j36/eglprobe 2>&1 | tee /run/j36/eglprobe.log'
ExecStartPre=-/bin/sh -c 'LIBGL_ALWAYS_SOFTWARE=1 /run/j36/eglprobe -s 2>&1 | tee -a /run/j36/eglprobe.log'
ExecStopPost=-/bin/sh -c 'echo "--- eglprobe, repeated now that the shell has exited ---"; cat /run/j36/eglprobe.log'
DROPINPROBE
        fi
    fi

    say "gl: front end in /run/j36/gl, drop-in in /run/systemd/system"
    say "    $(ls /newroot/run/j36/gl | tr '\n' ' ')"
    return 0
}

# ── The dashboard, in place of EmulationStation ───────────────────────────────
#
# A UNIT OF OUR OWN, AND WHERE A MASK HAS TO GO ON THIS CARD.  EmulationStation
# should not be an autostart service in these builds at all, and the way to say that
# is to mask it -- a symlink to /dev/null over the unit name.  The catch is which
# directory: build_emulationstation.sh installs the unit as
# /etc/systemd/system/emulationstation.service, and /etc/systemd/system OUTRANKS
# /run/systemd/system, so the obvious mask would be silently ignored and this build
# would think it had removed ES while ES was still starting.
#
# systemd.unit(5)'s precedence list has one runtime directory above /etc, and it is
# the one `systemctl mask --runtime' writes into:
#
#     /etc/systemd/system.control/     highest
#     /run/systemd/system.control/     <-- this is where the mask goes
#     /run/systemd/transient/
#     /run/systemd/generator.early/
#     /etc/systemd/system/             <-- where the ES unit is
#     /run/systemd/system/             <-- where mixdash.service goes
#     /usr/lib/systemd/system/         lowest
#
# So the removal is three writes, all of them into the tmpfs mounted a few lines up,
# and all of them gone on the next boot:
#
#   1  /run/systemd/system.control/emulationstation.service -> /dev/null.  The real
#      mask.  The unit becomes unloadable, so multi-user.target cannot pull it in and
#      nothing can start it by hand either.
#   2  mixdash.service in /run/systemd/system, wanted by multi-user.target through a
#      symlink in /run/systemd/system/multi-user.target.wants/.  Unit FILES do not
#      merge across the trees but dependency DIRECTORIES do -- .wants is additive --
#      so a want written in /run is honoured for a target whose unit is in /usr/lib.
#      This is the only thing that starts the dashboard.
#   3  a drop-in on emulationstation.service that resets ExecStart to an echo.
#      Redundant if (1) took, and it is there for the case where it did not: a
#      systemd without system.control in its search path, or a derived image that
#      moved the unit somewhere with even higher precedence.  Drop-ins from /run are
#      merged into a unit in /etc, so this still stops the binary from being
#      executed even when the mask is ignored.  Type=oneshot and Restart=no as well,
#      because the unit's own Restart=on-failure is what turned a single 134 into six
#      identical stack traces scrolling a 640x480 panel.
#
# It matters that (3) does not launch mixdash even though it could.  If it did, then
# on a boot where the mask was ignored the dashboard would start twice -- once from
# each unit -- and two processes drawing into /dev/fb0 while both read the same evdev
# nodes is a fault that looks exactly like a broken dashboard.  One owner.
#
# The units that name ES in ordering -- ogage.service After=, welcome-message and
# wifi_importer Before= -- are ordering only, with no Requires anywhere, so a masked
# ES drops out of their dependency graph without failing them.  That was read out of
# the unit files, not assumed.
#
# Nothing here writes to the card, and the whole arrangement is gone on the next
# boot: /run/systemd/system is the tmpfs mounted a few lines up.
#
#
# WHERE THE PAYLOAD IS, asked rather than assumed.  find_mixos() looks for it in the
# rootfs first, because extracting sd-root.tar.gz there is what the artifact README
# says to do, and then on every other partition of the card -- a tree extracted onto
# a data partition works, read-only mounted, without a keyboard and without a shell.
#
# WHY User=root.  The unit ES ran under is User=ark, and three things the dashboard
# does are not ark's: it puts /dev/tty0 into KD_GRAPHICS at its first paint so the
# kernel's console stops drawing over the dashboard -- and back into KD_TEXT if it
# fails or is stopped, which is the only reason a failure on this board is readable at
# all -- and that is an ioctl on a device ark cannot open; the Restart and Power off
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
        # exfat is in the list because that is what firstboot used to convert p3 to,
        # and a payload unpacked there has to be reported as "found but crippled"
        # rather than as "no such partition" -- the libQt5Core check below is what
        # says which.
        #
        # A payload found on the home partition is left mounted read-only here, and
        # that does cost systemd's rw mount of it at /home/virtua: one device cannot
        # be both.  It is the price of a card whose rootfs has no /opt/mixos at all,
        # it is announced on the console by the line below, and the fix is the
        # documented one -- put the tarball in the rootfs.
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

# ── the card, mounted because nobody can mount it by hand ─────────────────────
#
# There is no keyboard on this board and the dashboard is the only shell, so a data
# partition that systemd does not mount is a partition that cannot be reached at all.
# The rootfs's own fstab mounts what it knows about; this covers the rest, and it is
# what makes the Files page show something other than an empty home directory.
#
# Read-only, and that is a decision rather than caution: the operator writes this
# partition from a PC, the dashboard only reads it, and a data partition mounted rw by
# an initramfs is a partition that gets replayed dirty the next time the battery gives
# out mid-write.
#
# exfat and vfat are still in the list, for a card written before this layout: p3 used
# to be made vfat here and converted to exfat by firstboot.  On a current card p3 is
# ext2, labelled DATA, and it is the login user's HOME -- the rootfs fstab mounts it
# rw at /home/virtua.  That partition is handled by pointing at systemd's mount instead
# of making a second one, for the reason in the paragraph below.  BOOT is skipped by
# what is in it rather than by its device name, because the name is only known when
# mount_bootfs happened to run this boot.
#
# WHY THE HOME PARTITION IS NOT MOUNTED HERE.  A block device cannot be mounted ro and
# rw at the same time: the ro mount holds the superblock, and systemd's fstab mount of
# the same device comes back EBUSY.  With nofail on that fstab line -- which it needs,
# so that a missing p3 still reaches a shell -- the failure is silent, and the symptom
# is a home directory that is quietly the rootfs copy underneath the mount point while
# everything the operator writes goes to the wrong partition.  So this identifies that
# partition, unmounts it, and leaves /run/j36/card as a symlink to where systemd will
# mount it a few seconds later.  One mount, writable, owned by systemd.
mount_card() {
    # /newroot and not /run: this runs before switch_root, so that is the path the
    # kernel has recorded for the mount.
    if grep -q " /newroot/run/j36/card " /proc/mounts 2>/dev/null; then return 0; fi

    # Where the rootfs intends to mount it, read out of its own fstab rather than
    # assumed.  Pure shell rather than awk: this initramfs has neither awk nor cut,
    # and `read' splits a line into fields for free -- see INIT_APPLETS.
    home_mp=""
    if [ -r /newroot/etc/fstab ]; then
        while read -r fs_spec fs_mp fs_rest; do
            case "$fs_spec" in
                LABEL=DATA) home_mp="$fs_mp"; break ;;
            esac
        done < /newroot/etc/fstab
    fi
    [ -n "$home_mp" ] || home_mp=/home/virtua

    # mmcblk ONLY, and /dev/sd* was deliberately taken out of this glob.  It used to
    # be here and it used to be dead: with SCSI refused there was no path by which a
    # /dev/sda could exist on this board.  There is one now -- usb-storage is in the
    # j36/usb/ payload and run_usb has already loaded it by the time this runs -- so
    # a stick left in the port while a card had no home partition would be mounted
    # read-only as "the card" and the Files page would open on somebody's USB drive.
    # External disks are not the card.  They are handled after switch_root, by udev
    # and mixos-automount, and they land under /media with their own names on them.
    mkdir -p /newroot/run/j36/card
    for dev in /dev/mmcblk*p*; do
        if [ ! -b "$dev" ]; then continue; fi
        if [ "$dev" = "$rootdev" ] || [ "$dev" = "$bootdev" ]; then continue; fi
        for fs in exfat vfat ext2 ext4 btrfs; do
            if ! mount -t "$fs" -o ro "$dev" /newroot/run/j36/card 2>/dev/null; then
                continue
            fi
            if [ -d /newroot/run/j36/card/j36 ] || \
               [ -d /newroot/run/j36/card/mvii ]; then
                umount /newroot/run/j36/card
                break
            fi
            # The stamp is written by finishing_touches.sh at the root of p3, next to
            # the dotfiles it seeds there, and it is the only way this initramfs can
            # recognise that partition: identifying it by LABEL would need blkid, which
            # is not in this busybox.  A card without the stamp predates this layout
            # and falls through to the read-only mount below, which is right for it.
            if [ -f /newroot/run/j36/card/.mixos-home ]; then
                umount /newroot/run/j36/card
                rmdir /newroot/run/j36/card 2>/dev/null
                # Checked, because `ln -s target dir' with the directory still there
                # puts the link inside it instead of failing, and the Files page would
                # then open on a directory holding one dangling symlink.
                if [ -d /newroot/run/j36/card ]; then
                    say "dash: could not remove /run/j36/card, so it stays a directory"
                    say "      and the Files page will show nothing.  The home"
                    say "      partition is still mounted by systemd at $home_mp."
                    return 1
                fi
                ln -sfn "$home_mp" /newroot/run/j36/card
                say "dash: $dev ($fs) is the home partition; /run/j36/card -> $home_mp"
                say "      left to systemd to mount rw -- a read-only mount here would"
                say "      make its fstab entry fail with EBUSY."
                return 0
            fi
            say "dash: $dev ($fs) mounted read-only at /run/j36/card"
            return 0
        done
    done
    # Removed rather than left empty: mixdash opens its Files page on /run/j36/card
    # when that directory exists, and an empty directory would read as an empty card.
    rmdir /newroot/run/j36/card 2>/dev/null
    say "dash: no data partition to mount at /run/j36/card"
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
        say "      EmulationStation is NOT started as a fallback --"
        say "      it aborts at Renderer_GLES10.cpp:129 on this board and its unit"
        say "      restarts it, so the panel would end up black with six identical"
        say "      stack traces on it instead of this message.  A readable console"
        say "      is the better failure."
        neuter_es
        dash_notice
        return 1
    fi

    # The card, before the unit, so that the dashboard's Files page has it from its
    # first paint rather than after a rescan the operator has no way to trigger.
    mount_card

    # ── the dashboard's own unit ─────────────────────────────────────────────────
    #
    # The directory is made here and not assumed: setup_es_gl used to be the only
    # thing that created it, and with the dashboard as the shell that function returns
    # before it gets there -- and on a card with no j36/ directory at all it never
    # runs.  A `cat >' into a directory that does not exist would take the dashboard
    # out of the boot for the sake of one mkdir.
    mkdir -p /newroot/run/systemd/system
    cat > /newroot/run/systemd/system/mixdash.service <<UNITDASH
# Written by the J36 Ultra initramfs, into a tmpfs.  Not on the card, not in the
# rootfs: it exists only for as long as this boot does.
[Unit]
Description=MixOS dashboard (J36 Ultra)
Documentation=file:///opt/mixos/README.txt
# Ordering only, and the same place EmulationStation sat: firstboot resizes the data
# partition and a dashboard that lists it should not race that.  A unit that is not
# installed is simply not ordered against, so naming it costs nothing.
After=firstboot.service systemd-user-sessions.service
# Three tries a minute and then it stops.  EmulationStation's own unit had
# Restart=on-failure against a binary that aborts in 200 ms, and what that produced on
# a 640x480 panel was the same stack trace six times with the first line -- the only
# one that said anything -- already scrolled away.  A dashboard that cannot start
# should leave its last error on the glass.
StartLimitIntervalSec=60
StartLimitBurst=3

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=$mixos_root/bin
# ── the last word to the splash ──────────────────────────────────────────────
#
# The splash has been running since the initramfs and is still on the panel: it
# survived switch_root, /dev came with it, so /dev/.mixsplash is the same file it
# has had open all along.  This finishes the bar, gives it a second to ease up to
# the end, and then tells it to stop -- because from the next line on there are
# two processes writing to /dev/fb0 and only one of them should be.
#
# It stops BEFORE mixdash rather than after its first paint, and the second is
# not a gap: mixsplash exits with the console still in KD_GRAPHICS (that is what
# the `handover' message bought), so what stays on the glass is its own last
# frame, held there until Qt has finished its dynamic linking and painted over
# it.  A frozen splash is the correct thing to look at during that; a text
# console suddenly reappearing is not.
#
# Appending to a file, never a pipe, so this cannot block even if nothing is
# reading; and `-' in front so that a boot with no splash at all does not fail
# its ExecStartPre.  Re-running on every restart attempt is harmless by design.
ExecStartPre=-/bin/sh -c '{ echo "stage:Starting the dashboard"; echo "progress:100"; } >> /dev/.mixsplash; sleep 1; echo quit >> /dev/.mixsplash'
ExecStart=$mixos_root/bin/mixdash
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

    # ── one milestone in the middle of systemd ───────────────────────────────────
    #
    # Between switch_root and mixdash there is a stretch of systemd -- fsck, udev,
    # the journal, whatever the rootfs enables -- that on this SoC is the longest
    # quiet part of the boot.  /init cannot narrate it (it is gone) and mixdash
    # cannot (it has not started), so the splash would sit on the last line /init
    # wrote for half a minute.  The spinner keeps turning, so it does not look
    # crashed, but it does not say anything either.  This is one oneshot unit
    # whose whole body is two echoes.
    #
    # After=sysinit.target and nothing else: that is the point in the graph where
    # the low-level work is done and the ordinary services are about to start, and
    # ordering against only a target that always exists cannot make a cycle.
    cat > /newroot/run/systemd/system/j36-splash.service <<'UNITSPLASH'
# Written by the J36 Ultra initramfs, into a tmpfs.
[Unit]
Description=Tell the boot splash that systemd has got this far
After=sysinit.target
DefaultDependencies=no

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=-/bin/sh -c '{ echo "stage:Starting system services"; echo "detail:systemd"; echo "progress:94"; } >> /dev/.mixsplash'

[Install]
WantedBy=multi-user.target
UNITSPLASH
    mkdir -p /newroot/run/systemd/system/multi-user.target.wants
    ln -sf ../j36-splash.service \
           /newroot/run/systemd/system/multi-user.target.wants/j36-splash.service

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

    # The probe, before the dashboard.  -f is the one mode that touches nothing it
    # cannot give back: it counts the pixels already in /dev/fb0, undoes a backlight
    # at zero and a console left in KD_GRAPHICS, and paints colour bars with the CPU.
    # Run here it is a handover signal that costs a second -- bars and then a
    # dashboard means both halves work, bars that stay mean mixdash never started,
    # and no bars at all mean nothing userspace draws will be seen and the dashboard
    # is not the thing to debug.  It is NOT -p or -c: those two modeset, and on a
    # kernel with no fbdev emulation the CRTC is disabled when the client exits, so
    # either one would hide the dashboard for the rest of the boot.
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
    if [ "$probe_ready" = 1 ]; then
        cat >> /newroot/run/systemd/system/mixdash.service <<'UNITPROBEDEP'

[Unit]
Wants=mixdash-probe.service
After=mixdash-probe.service
UNITPROBEDEP
        cat > /newroot/run/systemd/system/mixdash-probe.service <<'UNITPROBE'
# Written by the J36 Ultra initramfs.  Once per boot, before the dashboard.
[Unit]
Description=MixOS panel probe (J36 Ultra)
Before=mixdash.service

[Service]
Type=oneshot
RemainAfterExit=yes
StandardOutput=journal+console
StandardError=journal+console
# - because a probe is not a precondition: whatever it says, the dashboard still
# gets its attempt.
ExecStart=-/bin/sh -c '/run/j36/eglprobe -f 1 2>&1 | tee /run/j36/eglprobe.log'
UNITPROBE
        # And under j36.gl=debug the two library questions as well.  The replay stays
        # on mixdash.service, where it belongs: the dashboard covers the panel with its
        # own drawing, so the only time the probe's verdict can be read is after the
        # shell has exited.
        if [ "$gl_debug" = 1 ]; then
            cat >> /newroot/run/systemd/system/mixdash-probe.service <<'UNITDBG'
ExecStart=-/bin/sh -c '/run/j36/eglprobe 2>&1 | tee -a /run/j36/eglprobe.log'
ExecStart=-/bin/sh -c 'LIBGL_ALWAYS_SOFTWARE=1 /run/j36/eglprobe -s 2>&1 | tee -a /run/j36/eglprobe.log'
Environment="EGL_LOG_LEVEL=debug"
Environment="LIBGL_DEBUG=verbose"
UNITDBG
            cat >> /newroot/run/systemd/system/mixdash.service <<'UNITDBGREPLAY'

[Service]
ExecStopPost=-/bin/sh -c 'echo "--- eglprobe, repeated now that the shell has exited ---"; cat /run/j36/eglprobe.log'
UNITDBGREPLAY
        fi
        say "dash: mixdash-probe.service runs the probe once per boot, not once per"
        say "      start attempt -- three restarts used to mean three EGL inits"
    fi

    neuter_es

    say "dash: mixdash.service is the shell; EmulationStation is not started"
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
    # holding the handheld.  The login user is looked up rather than assumed,
    # because this rootfs is shared with another machine whose user is called
    # something else.
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

# ── EmulationStation, taken out of the boot ───────────────────────────────────
#
# The mask and the drop-in described above setup_dash.  Called on the failure path
# too, and that is deliberate: a card with no dashboard on it should show the reason
# on the console, not six copies of ES's abort over the top of it.
neuter_es() {
    # /run/systemd/system.control, because that is the one runtime directory that
    # outranks the /etc the unit lives in.  ln -sf and not a file: a mask IS a
    # symlink to /dev/null, and systemd tests for exactly that.
    mkdir -p /newroot/run/systemd/system.control
    if ln -sf /dev/null \
              /newroot/run/systemd/system.control/emulationstation.service; then
        say "es: emulationstation.service is masked in /run/systemd/system.control"
    else
        say "es: could not mask emulationstation.service; the drop-in below is what"
        say "    stops it, and it is why that drop-in exists"
    fi

    mkdir -p /newroot/run/systemd/system/emulationstation.service.d
    cat > /newroot/run/systemd/system/emulationstation.service.d/zz-j36-dash.conf <<'DROPINDASH'
# Written by the J36 Ultra initramfs, into a tmpfs.  EmulationStation is not part of
# these builds: mixdash.service is the shell, the unit is masked in
# /run/systemd/system.control, and this drop-in is the belt to that braces -- if the
# mask is ever ignored, what starts here is an echo and not EmulationStation.
[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=
ExecStart=/bin/echo "j36: EmulationStation is not started in this build -- mixdash.service is the shell."
# The unit's own Restart=on-failure is what turned one abort into six stack traces
# scrolling a 640x480 panel.  Nothing here can fail, but it is stated anyway.
Restart=no
DROPINDASH
    return 0
}

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
  echo "j36: EmulationStation is masked in this build on purpose and is not a"; \\
  echo "j36: fallback: it aborts in Renderer_GLES10.cpp on this board."; \\
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
   [ "$want_audio" = 1 ] || [ "$want_usb" = 1 ] || [ "$want_power" = 1 ]; then
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
    # arbitrary: udl depends on drm_kms_helper and drm_shmem_helper, which the
    # mtkdrm payload also carries, so letting mtkdrm go first means the shared
    # pair is loaded once from the directory that has always owned it and run_usb
    # skips its own copies.  The other way round works too -- run_mtkdrm has no
    # such skip -- but it would print two module-load failures on a boot that is
    # working correctly.
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
    fi
    if [ "$want_usb" = 1 ]; then
        stage "Starting USB"; detail "MUSB host, hub, HID"
        progress 68; run_usb
    fi
    # After USB, and one module, so it costs almost nothing in the bar.  The
    # order against run_usb is the interlock described on run_power: the PMIC
    # samples the DRVVBUS pad to decide whether the 5 V on the port is ours, and
    # letting the PHY set that pad first means the very first poll sees the
    # truth instead of the LK's leftovers.
    if [ "$want_power" = 1 ]; then
        stage "Starting power management"; detail "MT6323 gauge, charger, poweroff"
        progress 71; run_power
    fi
    if [ "$want_gl" = 1 ]; then
        stage "Staging OpenGL"; detail "Mesa, EGL, GLESv2"
        progress 74; setup_es_gl
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

# Outside that block on purpose: the dashboard is in the second partition and the
# BOOT partition has nothing to do with it, so a card whose j36/ directory was
# deleted still comes up in the dashboard rather than in EmulationStation.  Last of
# all, so that the drop-in it writes has the final word on ExecStart.
#
# The else branch is not noise.  A card still carrying a boot.conf written before
# j36.dash existed boots to a rootfs whose EmulationStation may not even be enabled,
# and then nothing at all starts and nothing says why -- which is precisely the
# failure this line names in one word instead of an evening.
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

# What this boot image is, in a file, because the heredoc above is single-quoted:
# nothing from out here can be interpolated into /init, so anything /init needs to know
# has to arrive as a file inside the initramfs.  init= identifies the boot image itself;
# mixdash= is what this build of the dashboard should be, which /init passes on to
# mixdash so it can compare it with what it was actually compiled from.
cat > "$INITROOT/etc/j36-build" <<BUILDID
init=$(sha256sum "$INITROOT/init" | cut -c1-12)
mixdash=$MIXDASH_BUILD_ID
BUILDID

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
# gzdoom, lzdoom and EmulationStation.  SDL2 has no fbdev backend -- KMSDRM,
# X11, Wayland, offscreen and dummy are the whole list -- so all three need
# either DRM/KMS, which this kernel has no driver for yet, or a GL stack, and the
# GL stack on that card is the RK3326's Mali-G31 Bifrost blob for a SoC whose GPU
# is a Mali-450.  doomgeneric needs none of it: no SDL, no X11, no GL, no DRM.
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
        log "fbdoom: compiling ${#srcs[@]} sources in one pass for ARMv7"
        ( cd "$dir" && arm-linux-gnueabihf-gcc \
            -O2 -std=gnu17 -fcommon -static \
            -DNORMALUNIX -DLINUX -DSNDSERV -D_DEFAULT_SOURCE \
            -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
            -Wno-error=implicit-function-declaration \
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
# All five names are roots, and the two that matter are the last two: mediatek-drm
# reaches phy-mtk-mipi-dsi-drv through the generic phy API and the panel through
# the DSI host bus, neither of which is a symbol relationship, so a dependency walk
# cannot find either from mediatek-drm alone.
MTKDRM_MODULE_PATHS=()
MTKDRM_MODULE_ORDER=()
if [[ "${J36_MTKDRM:-1}" == 1 ]]; then
    set +e
    collect_modules mtkdrm MTKDRM_MODULE_ORDER MTKDRM_MODULE_PATHS \
        phy-mtk-mipi-dsi-drv mtk-mmsys mtk-mutex mediatek-drm j36_jd9365_panel
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
#                       it MUST be first in the load order -- it is the driver
#                       that ungates the PERI clock, and the walk puts it first
#                       for free because usbcore is the only thing under it.
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
#   libEGL.so             -> libMali.so     clobbered   ES's only GL DT_NEEDED
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
#   - libEGL.so is load-bearing because it is the literal string in
#     emulationstation's DT_NEEDED. Not libEGL.so.1 -- the bare name, which is
#     normally a -dev symlink no runtime binary should reference, and which here
#     resolves to a Mali blob for a GPU that is not on this board.
#   - libgbm.so.1 is load-bearing TWICE, and the second time is the one that would
#     have been missed: SDL2's KMSDRM backend dlopens it, and libEGL_mesa.so.0
#     carries it in its own DT_NEEDED. So with the rootfs as it stands, glvnd reads
#     50_mesa.json, dlopens Mesa's EGL vendor, and that pulls the RK3326 blob into
#     the process as its gbm. Mesa's EGL cannot initialise on this board without
#     this payload -- the point is not merely that ES fails to link.
#   - libGL.so.1 is what supplies the entry points EmulationStation calls. Its 29
#     undefined GL symbols are glMatrixMode, glLoadMatrixf, glEnableClientState --
#     fixed function, which reads as GLES1 and is equally desktop GL compat, and
#     all 29 are exported by glvnd's libGL.so.1. libGLESv1_CM.so.1 is the obvious
#     choice and it is the wrong one: Debian's Mesa is built without GLES1, so an
#     ES1 context is EGL_BAD_ALLOC on every driver including llvmpipe, and a stub
#     with no context under it returns NULL. libGL.so.1 brings libGLX.so.0 with it
#     (its DT_NEEDED), which wants libX11.so.6 -- present on this rootfs; nothing
#     here opens a display, it is glvnd's one-library-for-both-APIs shape.
#   - libGLESv2.so.2 is for when SDL's context request comes out as ES2, and
#     libGLdispatch.so.0 is glvnd's own dependency and the only DT_NEEDED any of
#     these have on each other. It is also what makes the libGL preload work at
#     all: glvnd's libEGL and libGL share one current-dispatch table, so a context
#     created through eglMakeCurrent is the one libGL's stubs dispatch into.
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

    # And one link that is not a SONAME. EmulationStation's DT_NEEDED entry is the
    # bare development name `libEGL.so', because it was linked against the rootfs's
    # libEGL.so -> libMali.so symlink, where the blob exported EGL and GLES1 and
    # GLES2 all from one object. glvnd's libEGL is only ever installed as
    # libEGL.so.1, so without this alias the binary does not load at all.
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
ESPROBE_SRC="$ROOT/device/j36-ultra/tools/j36-eglprobe.c"
ESPROBE_BIN=""

build_eglprobe() {
    local out="$WORK/j36-eglprobe" header needed

    [[ -f "$ESPROBE_SRC" ]] || { log "gl: $ESPROBE_SRC is missing"; return 1; }
    arm-linux-gnueabihf-gcc -O2 -std=gnu11 -Wall -Wextra \
        -o "$out" "$ESPROBE_SRC" || return 1

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

    ESPROBE_BIN="$out"
    log "gl: eglprobe is $(stat -c %s "$out") bytes, dynamic ARM, needs $needed"

    return 0
}

# ── EmulationStation, rebuilt with a GLES 2.0 renderer ────────────────────────
#
# The one thing the GL payload above cannot fix.  The binary in the shared rootfs
# was compiled -DUSE_OPENGLES_10, and GLES1 is the single API this stack cannot
# provide: eglCreateContext for an ES1 context returns 0x3003 EGL_BAD_ALLOC on
# lima, on llvmpipe AND on softpipe, because Debian's armhf Mesa 25.0.7 is a
# -Dgles1=disabled build.  That is the package and not this SoC, so there is no
# driver work that would make the stock binary run and no MESA_GL_VERSION_OVERRIDE
# that would help -- BAD_ALLOC is __DRI_CTX_ERROR_NO_MEMORY, the driver's own
# create returning NULL, not the BAD_MATCH an unadvertised API gives.
#
# ES2 is what does come up on lima here, measured with j36-eglprobe:
# "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1", context created and made current.  So the
# renderer is a third one, es/Renderer_GLES20.cpp, and this is where it is compiled.
# Everything else about the binary is upstream at the same commit the rootfs's own
# copy was built from, so the only difference between the two is the renderer.
#
# THREE THINGS ABOUT THIS BUILD ARE DELIBERATE AND NOT CONVENIENCES:
#
#   - It links no GL library at all, and es/patch-gles20.py exists to take `EGL'
#     off the link line.  Every unversioned GL name in this rootfs -- libEGL.so,
#     libGLESv2.so, libGLESv2.so.2, libGLESv1_CM.so* -- is a symlink to an
#     SONAME-less ARMv8-A libMali.so, so linking any of them records a DT_NEEDED
#     that is SIGILL on a Cortex-A7.  The renderer resolves all 43 entry points
#     through SDL_GL_GetProcAddress instead, and the assertion at the end of this
#     function is what keeps it that way.  One binary is then correct on both
#     machines -- the R36S blob and this board's Mesa -- with no LD_PRELOAD.
#   - It builds in an armhf chroot rather than cross-compiling, because ES needs
#     SDL2, SDL2_mixer, FreeImage, FreeType, cURL, VLC, ALSA and RapidJSON headers
#     and Debian trixie armhf has all of them at the versions the target runs.
#     Its SDL2 is 2.32.4, the same as the rootfs's.
#   - It never touches the rootfs image.  The binary is staged onto the vfat BOOT
#     partition and /init bind-mounts it over /usr/bin/emulationstation/
#     emulationstation, so the card stays byte-identical for the R36S.
#
# Non-fatal, like everything else in this file: if it does not build the kernel
# artifacts still ship, /init finds no j36/es, says so, and the rootfs's own binary
# runs and fails in the way it already fails.
ES_URL="https://github.com/christianhaitian/EmulationStation-fcamod"
# The rootfs's own ES commit, read out of the MixOS package cache's
# emulationstation_351v_armhf.commit.  Pinned rather than tracking the branch so
# that the renderer patch's anchors stay valid and the only difference from the
# installed binary stays the renderer.
ES_COMMIT="74498be31cd016af6a42d00310f876d7256eff52"
ES_RENDERER="$ROOT/device/j36-ultra/es/Renderer_GLES20.cpp"
ES_PATCH="$ROOT/device/j36-ultra/es/patch-gles20.py"
ARMHF_CHROOT="$WORK/es-chroot"
ES_BIN=""

# Debian trixie armhf has all of ES's dependencies.  --no-install-recommends
# because the recommends of libvlc-dev alone pull a desktop in.
ES_BUILD_DEPS=(build-essential cmake git pkg-config ca-certificates
               libsdl2-dev libsdl2-mixer-dev libfreeimage-dev libfreetype-dev
               libcurl4-openssl-dev libvlc-dev libasound2-dev rapidjson-dev)

armhf_chroot_run() {
    sudo chroot "$ARMHF_CHROOT" bash -c "$1"
}

# The chroot is built once and kept.  Preferred base is MixOS's own armhf rootfs
# cache, because if the R36 base build has run then it is already on disk and it is
# the same debootstrap the target was made from; debootstrap is the fallback for a
# machine where only this build has ever run.
#
# Two things build in here now -- the dashboard and, when it is asked for,
# EmulationStation -- so the base and the dependencies are separate steps.  A card
# that only wants mixdash should not spend twenty emulated minutes installing
# libvlc-dev, and chroot_install_deps below is what keeps the two apart.
ensure_armhf_chroot() {
    local suite="${DEBIAN_RELEASE:-trixie}" base m
    base="$ROOT/Arkbuild_package_cache/debian_${suite}_userspace-armhf_rootfs.tar.gz"

    if [[ ! -f "$ARMHF_CHROOT/.j36-base" ]]; then
        sudo rm -rf "$ARMHF_CHROOT" "$WORK/es-chroot-x"
        mkdir -p "$WORK/es-chroot-x"
        if [[ -f "$base" ]]; then
            log "chroot: unpacking the armhf $suite rootfs from the MixOS package cache"
            sudo tar -xpzf "$base" -C "$WORK/es-chroot-x" || return 1
            # The cache tarball carries MixOS's own chroot name at the top.
            sudo mv "$WORK/es-chroot-x/Arkbuild" "$ARMHF_CHROOT" || return 1
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
        sudo rm -rf "$WORK/es-chroot-x"
        sudo touch "$ARMHF_CHROOT/.j36-base"
    fi

    # /dev, /proc and /sys are bound for apt's maintainer scripts and for nproc.
    # They are unmounted in armhf_chroot_teardown when the build is done: a bind mount
    # of /sys left inside a directory tree is something the next rsync of this
    # machine trips over, with an unlink() permission denied that names a sysfs
    # file and explains nothing.
    for m in dev proc sys; do
        mountpoint -q "$ARMHF_CHROOT/$m" || sudo mount --bind "/$m" "$ARMHF_CHROOT/$m" || return 1
    done
    printf 'nameserver 8.8.8.8\nnameserver 1.1.1.1\n' | \
        sudo tee "$ARMHF_CHROOT/etc/resolv.conf" >/dev/null
    printf 'exit 101\n' | sudo tee "$ARMHF_CHROOT/usr/sbin/policy-rc.d" >/dev/null
    sudo chmod 0755 "$ARMHF_CHROOT/usr/sbin/policy-rc.d"
    return 0
}

# chroot_install_deps <stamp-name> <package>...
#
# One stamp per set, so the two builds do not install each other's dependencies and
# neither reinstalls on a re-run.  The stamp names are part of the chroot's state and
# not of this file's: .j36-deps is the name the ES set has always had, and keeping it
# is what stops an existing chroot from doing twenty emulated minutes of apt again.
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

build_es_gles20() {
    local src="$ARMHF_CHROOT/home/build/es" out="$CACHE/emulationstation-gles20"
    local stamp="$CACHE/emulationstation-gles20.stamp" want header needed lib

    [[ -f "$ES_RENDERER" ]] || { log "es: $ES_RENDERER is missing"; return 1; }
    [[ -f "$ES_PATCH" ]]    || { log "es: $ES_PATCH is missing"; return 1; }

    # Keyed on the commit and on both files that shape the build, so editing the
    # renderer rebuilds and re-running the script does not.
    want="$ES_COMMIT $(sha256sum "$ES_RENDERER" | awk '{print $1}') \
$(sha256sum "$ES_PATCH" | awk '{print $1}')"
    if [[ -x "$out" && "$(cat "$stamp" 2>/dev/null)" == "$want" ]]; then
        ES_BIN="$out"
        log "es: reusing $out ($(stat -c %s "$out") bytes)"
        return 0
    fi

    ensure_armhf_chroot || { armhf_chroot_teardown; return 1; }
    # The empty stamp name is .j36-deps, which is what this set has always used.
    chroot_install_deps "" "${ES_BUILD_DEPS[@]}" || { armhf_chroot_teardown; return 1; }

    # Cloned from outside the chroot: git over TLS under qemu-arm is minutes of
    # emulated crypto for no reason.  --depth 1 of one SHA rather than of a branch,
    # because the pin has to be in the clone and GitHub serves a reachable SHA.
    if [[ ! -d "$src/.git" ]]; then
        log "es: cloning EmulationStation-fcamod at $ES_COMMIT"
        sudo rm -rf "$src"
        sudo mkdir -p "$src"
        sudo chown "$(id -u):$(id -g)" "$ARMHF_CHROOT/home/build" "$src" || true
        git -C "$src" init -q || return 1
        git -C "$src" remote add origin "$ES_URL" || return 1
        git -C "$src" fetch -q --depth 1 origin "$ES_COMMIT" || return 1
        git -C "$src" checkout -q FETCH_HEAD || return 1
        # external/pugixml is the tree's one submodule; nanosvg is vendored.
        git -C "$src" submodule update --init --depth 1 || return 1
    fi

    cp "$ES_RENDERER" "$src/es-core/src/renderers/" || return 1
    # Patched from a clean copy every time, so a re-run cannot stack the patch on
    # top of itself and so a moved upstream fails in patch-gles20.py's anchor
    # check rather than halfway through cmake.
    git -C "$src" checkout -- CMakeLists.txt es-core/CMakeLists.txt || return 1
    python3 "$ES_PATCH" "$src" || return 1

    # CMAKE_POLICY_VERSION_MINIMUM because upstream's cmake_minimum_required is
    # 2.8 and trixie's cmake is 3.31, which makes anything under 3.5 an error.
    log "es: configuring and building EmulationStation for armhf (emulated; this is slow)"
    armhf_chroot_run "cd /home/build/es && rm -rf CMakeCache.txt CMakeFiles && \
        cmake -DGLES20=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release ." \
        || { armhf_chroot_teardown; return 1; }
    armhf_chroot_run "cd /home/build/es && make -j\$(nproc)" \
        || { armhf_chroot_teardown; return 1; }
    armhf_chroot_teardown

    [[ -f "$src/emulationstation" ]] || { log "es: make left no binary"; return 1; }

    mkdir -p "$CACHE"
    # Stripped, because this goes on a 100 MB vfat partition beside the kernel, the
    # initrd and the module payloads, and ES's debug info is most of its size.
    arm-linux-gnueabihf-strip -o "$out" "$src/emulationstation" || return 1
    chmod 0755 "$out"

    # The assertion the whole design rests on: no GL library in DT_NEEDED.  A
    # regression here does not fail to build, it builds a binary that dlopens the
    # RK3326 Mali blob on a Cortex-A7 and dies with SIGILL before main().
    header="$(readelf -hd "$out" 2>/dev/null)" || return 1
    grep -q 'Class:.*ELF32' <<<"$header" || { log "es: not a 32-bit ELF"; return 1; }
    grep -q 'Machine:.*ARM' <<<"$header" || { log "es: not an ARM ELF"; return 1; }
    needed="$(sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' <<<"$header" | tr '\n' ' ')"
    for lib in $needed; do
        case "$lib" in
            libEGL*|libGL*|libGLES*|libMali*|libmali*|libgbm*)
                log "es: the binary links $lib -- patch-gles20.py did not take the GL"
                log "    library off the link line, and every one of those names on this"
                log "    rootfs is a symlink to an ARMv8-A libMali.so.  Refusing to stage it."
                return 1
                ;;
        esac
    done

    printf '%s\n' "$want" >"$stamp"
    ES_BIN="$out"
    log "es: $(stat -c %s "$out") bytes, stripped ARM, no GL in DT_NEEDED"
    log "es: needs $needed"
    return 0
}

# ── mixdash: the dashboard, and what it does not need ─────────────────────────
#
# WHY THIS EXISTS.  EmulationStation reaches the panel through SDL's KMSDRM
# backend, so EGL, so GBM, so Mesa's lima on a Mali-450 -- and it stayed black with
# no error past eglCreateContext.  mixdash reaches the panel through none of them:
# Qt5 Widgets on the `linuxfb' platform plugin writes into /dev/fb0, and on this
# board /dev/fb0 is the framebuffer the LK already lit.  That is not a hope, it is
# the consequence of two decisions further up this file: FB_SIMPLE binds the device
# tree's simple-framebuffer at 0x82700000, and CONFIG_DRM_FBDEV_EMULATION=n means
# mediatek-drm never takes /dev/fb0 away from it.  The kernel console drawing on
# this panel already proved the path end to end.
#
# WHAT IS BUILT.  Five files in device/j36-ultra/tools/mixdash, against Debian's own
# qtbase5-dev in the same armhf chroot EmulationStation used.  Nothing is
# cross-compiled, no Qt is built from source, and QT does not include opengl.
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
    log "mixdash: building the dashboard for armhf (emulated)"
    armhf_chroot_run "cd /home/build/mixdash && \
        q=\$(command -v qmake || true); \
        [ -n \"\$q\" ] || q=\$(ls /usr/lib/*/qt5/bin/qmake 2>/dev/null | head -1); \
        [ -n \"\$q\" ] || { echo 'no qmake in this chroot'; exit 1; }; \
        QT_SELECT=qt5 \"\$q\" \
            QMAKE_LFLAGS+='-Wl,--disable-new-dtags' \
            QMAKE_LFLAGS+='-Wl,-rpath,/run/j36/gl' \
            QMAKE_LFLAGS+='-Wl,-rpath,/opt/mixos/qt/lib' && \
        make -j\$(nproc) && strip mixdash" \
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

# J36_GL is the GL front end and the probe, and it is worth keeping even now that
# the dashboard needs no GL: eglprobe is still the only thing here that says whether
# a frame reaches the glass, and anything launched from the dashboard that does want
# GL resolves it out of this payload.  J36_ES is the name that word used to have.
if [[ "${J36_GL:-${J36_ES:-1}}" == 1 ]]; then
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
        ESPROBE_BIN=""
        log "gl: eglprobe was not built, see the error above -- j36.gl=debug will just be quieter"
    fi
else
    log "gl: J36_GL=0, skipping the GL front end"
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

# EmulationStation, and it is off by default now.  Not because the GLES 2.0 renderer
# was wrong -- it is still here, and the reasoning above it still holds -- but because
# it never drew: the last measurement was a context that was created, a frame that was
# submitted and a panel that stayed black, with nothing in EGL, in SDL or in Mesa
# saying why.  Five layers deep is a bad place to be stuck, and the dashboard reaches
# the same panel through none of them.  J36_ES_BUILD=1 brings it back verbatim, and it
# is worth keeping buildable: it is the only real GL application on this board, so the
# day mtk_drm and lima do land a frame, this is what proves it.
if [[ "${J36_ES_BUILD:-0}" == 1 ]]; then
    set +e
    build_es_gles20
    es_rc=$?
    set -e
    if (( es_rc != 0 )); then
        ES_BIN=""
        armhf_chroot_teardown
        log "es: the GLES 2.0 binary was not built, see the error above -- the card"
        log "    will carry no j36/es"
    fi
else
    log "es: J36_ES_BUILD=0, EmulationStation is not built (the dashboard replaces it)"
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
cp "$ARTIFACTS/initramfs-j36-ultra.cpio.gz" "$SDBOOT/initrd.img"

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
# needs drm_kms_helper and drm_shmem_helper, so both directories carry a copy.
# Two copies of a 200 KB pair is cheaper than a load order that spans payloads,
# and run_usb skips whichever of them mtkdrm already loaded.
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
# a library. j36.es=debug runs it; j36.es=1 never touches it. Deleting it is the
# same contract as the rest: the debug boot simply loses this report.
if [[ -n "$ESPROBE_BIN" ]]; then
    mkdir -p "$PAYDIR"
    cp "$ESPROBE_BIN" "$PAYDIR/eglprobe"
    chmod 0755 "$PAYDIR/eglprobe"
    log "gl: staged $PAYREL/eglprobe ($(stat -c %s "$PAYDIR/eglprobe") bytes)"
fi

# j36/es/ is the GLES 2.0 EmulationStation, and it is the one payload here that
# REPLACES something rather than adding to it -- /init bind-mounts it over
# /usr/bin/emulationstation/emulationstation.  The removal contract is therefore
# the interesting one: delete this directory and the rootfs's own binary runs
# instead, which is the status-134 abort this exists to fix, so /init says which of
# the two it mounted and the drop-in it writes differs accordingly.
#
# Its own directory and not j36/ directly, so that the one file that has to be
# deleted to go back to the old behaviour is a directory a reader can see the
# purpose of.
if [[ -n "$ES_BIN" ]]; then
    mkdir -p "$PAYDIR/es"
    cp "$ES_BIN" "$PAYDIR/es/emulationstation"
    chmod 0755 "$PAYDIR/es/emulationstation"
    log "es: staged $PAYREL/es/emulationstation ($(stat -c %s "$PAYDIR/es/emulationstation") bytes)"
fi

# rdinit=/init stays even though root= is now present, and the two do not
# conflict: rdinit means the kernel never mounts a root filesystem itself, so a
# root= it could not honour can no longer panic it.  /init does the mounting, and
# treats root= as a hint it verifies before switching -- delete the root= below
# and the card boots to the initramfs shell exactly as it did before.
#
# The prose that used to explain each bootargs word lives in README.txt now.  The
# LK reads boot.conf into a fixed 2 KiB buffer, comments included, and this file
# has already been over it once -- rewriting the /opt/mixos paragraph took it to
# 2109 bytes, which the assertion below turns into a failed build rather than a
# silently truncated bootargs line.  Keep the comments terse; README.txt is where
# the explanations belong.
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
# MUSB port host-only, splash the MixOS picture with the boot stage on it.
#
# Only the four files the LK reads are on BOOT; the rest is in sd-root.tar.gz,
# unpacked as /opt/mixos on the ext2 OS partition.
#
# j36.audio=speaker powers the class-D amp off VBAT -- the system node -- so
# battery-less it pulls the board under its own lockout.  j36.usb=1 sources 5 V
# off that same VBAT; with no cell, or with a self-powered hub, say
# j36.usb=novbus.  j36.gl=debug adds Mesa's EGL trace and the node probes -- a
# diagnostic, not a default: a frozen kernel behind hundreds of libEGL lines is
# that trace.  j36.es is the old j36.gl.
# Drop j36.dash=1 and EmulationStation is neither masked nor replaced, and
# j36.splash=0 boots to text.  loglevel=4 keeps the panel clear until the
# splash starts; errors and warnings still print, and dmesg keeps the rest.
# ./build-j36-ultra.sh --no-splash writes j36.splash=0 loglevel=7 here.
bootargs=console=ttyS0,115200n8 console=tty0 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init root=/dev/mmcblk0p2 rw rootwait loglevel=4 vt.global_cursor_default=0 systemd.mask=firstboot.service systemd.journald.forward_to_console=1 j36.lima=1 j36.mtkdrm=1 j36.gl=1 j36.dash=1 j36.audio=1 j36.usb=1 j36.power=1 j36.splash=1
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
# truncated mid-line.
(( $(stat -c %s "$SDBOOT/mvii/boot.conf") <= 2048 )) || \
    die "boot.conf exceeds the LK's 2048-byte read buffer"
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

That is the whole partition, and the shortness is the design.  The MVII LK reads
FAT32 and nothing else, so BOOT exists because the loader has to be able to open
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
  opt/mixos/bin/doom       framebuffer Doom, and share/doom/ its IWAD
  opt/mixos/j36/mfgpower   powers the Mali-450 and reads its ID back; the gate
  opt/mixos/j36/modules/   lima and its dependencies, plus load.order
  opt/mixos/j36/mtkdrm/    the MT6592 display driver set, plus load.order
  opt/mixos/j36/audio/     the ALSA core and the MT6592 AFE driver, plus load.order
  opt/mixos/j36/usb/       the USB host stack -- PHY, MUSB, HID, udl, and the disk
                           set (scsi_mod, sd_mod, usb-storage, ntfs3) -- plus load.order
  opt/mixos/j36/power/     the MT6592 PMIC: battery gauge, charger, poweroff --
                           and the panel backlight, which is the same subject
  opt/mixos/j36/gl/        Mesa's GL front end, plus links
  opt/mixos/j36/eglprobe   -f reports and paints /dev/fb0 with no DRM at all and
                           runs on every boot; the other modes say what can create
                           a GL context, and why not, and whether a frame reaches
                           the glass.  See "j36/eglprobe -f" below.

A TARBALL AND NOT A DIRECTORY, on purpose: this payload's symlinks, modes and
ownership are load-bearing -- the Qt SONAME aliases are symlinks, mfgpower and the
probe have to stay executable -- and a tarball is the copy that cannot lose them
whatever machine does the copying.  Unpack it as root.

Everywhere below, "j36/..." means /opt/mixos/j36/... on that partition.  A card
written by a build from before this layout has the same directory on BOOT instead,
and /init looks there second and says which one it found, so such a card still
boots -- but the tarball above is where new payloads go.

EmulationStation is not part of these builds and is not started.  There is no
j36/es/ directory any more.

The R36S kernel on the same card is arm64 and stays there for the R36S.  The
armhf Debian rootfs is shared, and this kernel can now mount it: MSDC1, the
microSD host, is driven by mtk-sd through a mediatek,mt6592-mmc node, and ext2 --
which is what the rootfs is -- is built in, along with ext4 and btrfs for the
cards earlier builds wrote.  /init verifies a candidate partition by mounting it
read-only and looking for /sbin/init, then switch_roots into it.  If nothing
qualifies -- or if you delete root= from mvii/boot.conf -- it stops at a busybox
shell on the panel and on the serial port instead, and prints /proc/partitions so
you can see what the kernel did find.

The command line, word by word
------------------------------

console=ttyS0,115200n8 console=tty0
    Both consoles receive every printk; what the order decides is /dev/console,
    which is whichever came LAST.  With the UART last, a boot appears to stop
    dead at "random: crng init done" -- systemd logs to /dev/kmsg only until
    journald takes over, and from then on everything it says goes to a serial
    port that may have nothing plugged into it.  tty0 last puts /dev/console on
    the panel.

systemd.journald.forward_to_console=1
    The other half of the same problem: it copies the service log to
    /dev/console, so the panel shows services starting and failing instead of a
    blinking cursor.  Drop it once there is a shell or a network to read the
    journal with -- it costs a redraw per line on a 640x480 framebuffer.

systemd.mask=firstboot.service
    MixOS's first-boot script is written for the RK3326 image and this
    configuration cannot finish it.  It expands the partitions in two stages with
    a reboot between them, then untars /roms.tar and /tempthemes -- which a
    GUI-mode build does not ship.  With the tars missing, its two progress loops
    spin 15000 subshells apiece before giving up, which is minutes of dead panel,
    and then it reboots.  Delete this word to let it run on a card that does
    carry the tars; it grows ROOTFS to fill the card and rebuilds p3 -- the home
    partition, ext2 and labelled DATA -- from /roms.tar.  It no longer converts
    that partition to exfat, though this kernel still has exfat and vfat built in
    for cards written before the change.

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

    Half of that is now fixed rather than masked.  j36.power=1 loads the MT6592
    PMIC driver, which registers its battery supply under exactly that name, so
    the capacity file the daemon opens first is there and the traceback is gone.
    The GPIO half is not: this kernel has no sysfs GPIO export, so the daemon
    still cannot write gpio77 and still exits -- but it now exits on the LED,
    which is a bounded and specific failure, and it exits having read a real
    charge percentage.  It is left unmasked deliberately: a unit that fails on
    the second thing it tries is evidence that the first thing works, and this is
    the cheapest standing check that the power_supply registration survived a
    kernel bump.  Add systemd.mask=batt_led.service back to boot.conf if the
    noise is not worth it on a particular card; nothing else depends on it.

    Worth knowing when reading its output: with no power-path FET on this board,
    VBAT is the system node, so the percentage the daemon reads is the gauge's
    integrated estimate seeded from the PMIC's wakeup OCV latch, not a direct
    reading of a battery-only rail.  It is honest, and it moves slowly on
    purpose.

    Other RK3326-only units are left alone on purpose: 351mp.service (power LED,
    backlight, amixer), audiopath, audiostate and wifi_importer are all
    Type=oneshot, so they fail once and stay failed, and emulationstation is
    Restart=on-failure under the default 5-starts-in-10-s limit.  Bounded noise
    is evidence; only the unbounded one had to go.

    The three audio units are worth watching now that j36.audio=1 registers a card:
    they were failing on a machine with no /dev/snd at all, and with one present
    their amixer calls will look for RK3326 control names -- "Playback", "HP", a
    dozen SoC-specific ones -- that this card does not have.  A oneshot that fails
    on a missing control is the same bounded noise as before and is left alone; what
    would not be bounded is a control this card DOES have being set to something
    unwanted, so "Master Playback Switch" and "Master Playback Volume" are the two
    names to check against those scripts if the sound ever changes by itself.

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

    It is SILENT, and that is deliberate rather than a shortfall.  What this word
    switches on is the digital half: the AFE's DL1 memif, the interconnect route to
    the I2S DAC, the MT6323 ABB downlink, and one 16-bit stereo playback PCM at
    8-48 kHz.  What it does not switch on is the class-D speaker amp -- see the next
    word.  So a stream opens, is accepted and is paced, and nothing comes out.

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
    as j36,drvvbus-pad = <15>, and j36_mt6592_usb_phy.ko raises it in .power_on
    after the host role is forced, and drops it in .power_off.

    So a bus-powered hub now enumerates.  FIT A CELL FIRST, for the same reason as
    j36.audio=speaker above: the 5 V is a boost off VBAT, and VBAT on this PMIC is
    the system node.  A bus-powered load on a cell-less board is the same class of
    load as the class-D amp, which MVII measured pulling VBAT under the
    undervoltage lockout.  DEVCTL bits 3 and 4 report whether VBUS is valid, which
    is how to tell whether the port or the hub is carrying it.

j36.usb=novbus
    The same stack with the pad left exactly as the LK left it -- /init passes
    vbus=0 to the PHY module, which is where it has to go, because a kernel-cmdline
    modname.param= reaches built-in modules only and every one of these is
    loadable.  Two cases want it: no cell fitted, and a self-powered hub, which
    brings its own 5 V and has no use for the port's.

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
    run_usb at boot.  PAINTING it is a further step: DRM_FBDEV_EMULATION is off on
    purpose -- it is a global bool, and turning it on for udl's sake would also make
    mtk_drm create a second /dev/fb, which is exactly what keeping /dev/fb0 as
    simplefb's is meant to prevent.  So the dashboard, which draws into /dev/fb0
    with the CPU, does not appear on the dock by itself.  A compositor or a Qt
    EGLFS-KMS front end pointed at that card node is what would put it there.

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

    What it registers is two supplies, and their names are load-bearing rather
    than descriptive.  /sys/class/power_supply/battery is what batt_led.service
    opens by that literal path, and it is also what mixdash finds when it walks
    the directory looking for a supply whose type is Battery -- so calling it
    `battery' is what makes two programs that know nothing about this driver work
    unmodified.  Beside it, /sys/class/power_supply/usb reports online,
    usb_type (SDP, CDP, DCP or an Apple brick) and current_max, which is what the
    BC1.2 handshake decided the wall will actually give us.

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

    The charger is armed once per plug event, at the limit BC1.2 negotiated, and
    the constant voltage is only ever raised -- never lowered below the node,
    because lowering it on a board where VBAT is VSYS is a way to brown out the
    machine you are charging.  If it was given no pericfg/usb-phy pair in the
    device tree it cannot run BC1.2 at all and falls back to a conservative
    limit; that is a working charger, just a slow one.

    While the USB port is sourcing 5 V -- j36.usb=1 without novbus -- the charger
    is disarmed and the supply reports offline, because CHRDET inside the PMIC
    cannot tell our own boost from a wall charger.  The driver reads GPIO pad 15
    directly to find out, three registers per poll, which is why it loads after
    the PHY rather than before.

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

j36.gl=1  (j36.es=1 is the old spelling of the same word)
    Stage j36/gl/ -- Debian's armhf Mesa -- into a tmpfs, so that a program looking
    for libEGL.so.1 finds it there instead of at the RK3326's Mali blob, which is
    what /usr/lib on the shared rootfs points those names at and which is an
    ARMv8-A object on this Cortex-A7.  Nothing on the shared rootfs is written; see
    below.  It needs j36.lima=1 to be any use, and j36.mtkdrm=1 as well before
    anything can reach the panel through DRM.

    The dashboard itself does not need this: it is Qt drawing with the CPU into
    /dev/fb0.  What needs it is the "3D cube" card, which runs eglprobe -c.

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
    Run the MixOS dashboard as the shell, and take EmulationStation out of the boot:
    the unit is masked in /run/systemd/system.control -- the one runtime directory
    that outranks the /etc its unit file lives in -- and its ExecStart is reset to an
    echo as well, in case a systemd ever ignores the mask.  mixdash.service is
    written into /run/systemd/system and wanted from multi-user.target through a
    symlink there.  All of it is in tmpfs and none of it survives a reboot.

    The dashboard is not on this partition: /init looks for opt/mixos/bin/mixdash in
    the rootfs first and then on every other partition of the card, read-only.  Every
    partition it tries is named on the console -- as carrying no opt/mixos, or as
    unmountable, which is what a btrfs data partition looks like here because the
    initramfs carries no modules.  With nothing found it says so and still does not
    start EmulationStation, because that binary aborts with status 134 on this board
    and its unit restarts it -- six identical stack traces over the message explaining
    the fault is worse than the message.

    And because those lines are printed from the initramfs, where a hundred lines of
    kernel and systemd output push them off a 640x480 panel long before anybody reads
    them, a boot that finds nothing also gets mixdash-missing.service: it prints the
    reason, the inventory of partitions and the tar command that fixes it, six times,
    twenty seconds apart, after the boot has gone quiet.  A board with no keyboard has
    no other diagnostic interface.  If what you see instead is a console that simply
    stops -- typically at hostnamed deactivating -- then j36.dash=1 never reached
    /proc/cmdline, and /init says that too: without the word nothing is staged and
    whatever the rootfs starts by itself is what you get, which on a rootfs whose
    EmulationStation is not enabled is nothing at all.

    One more thing it arranges: /run/j36/card, which is where the dashboard's Files
    page opens.  That is not a convenience -- with no keyboard there is no way to
    mount anything by hand, and a file browser rooted in an empty directory is a file
    browser showing nothing.

    On a current card that path is a SYMLINK to /home/virtua, the login user's home
    and the mount point of p3: ext2, labelled DATA, and the one partition on the card
    meant to be written.  /init recognises it by a .mixos-home stamp at its root --
    it has no blkid to read the label with -- unmounts its own probe and leaves the
    mounting to systemd's fstab entry.  Deliberately: a device cannot be mounted ro
    and rw at once, so a read-only mount here would make that entry fail with EBUSY,
    and because the entry carries nofail it would fail silently, leaving a home
    directory that is really the rootfs copy underneath the mount point.

    On a card written before this layout it is a read-only mount instead, of the
    first partition that is neither the rootfs nor BOOT -- exfat and vfat tried
    first, p3 having been made vfat and converted to exfat by firstboot back then.
    BOOT is recognised and skipped by carrying j36/ or mvii/ rather than by its
    device name, which is only known on a boot that had reason to mount it.

    Why ES was dropped rather than fixed: the rootfs's binary was compiled with the
    fixed-function renderer, and GLES1 is the one API this stack cannot supply.
    Debian's armhf Mesa 25.0.7 is a -Dgles1=disabled build, so an ES1 context is
    0x3003 EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike.
    Renderer_GLES10.cpp then reads glGetString(GL_EXTENSIONS) without having checked
    SDL_GL_CreateContext, so a context that was never created arrives as
    std::string(NULL) -- abort, 134.  A GLES 2.0 rebuild did get a context ("OpenGL
    ES 2.0 Mesa 25.0.7-2+deb13u1") and still drew a black panel, through five
    silent layers -- ES, SDL, KMSDRM, EGL, GBM.  The dashboard removes all five.

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

Doom, what it was for, and why it is no longer on the card
----------------------------------------------------------

It answered a question the boot itself does not: whether a program can drive this
panel and read this pad.  It did, the answer was yes, and EmulationStation now
answers the same question further up the stack -- so J36_DOOM defaults to 0 and
neither j36/doom nor an IWAD is written to this partition.  Everything below is
what a J36_DOOM=1 build stages, kept because /init still runs it and because it
is the fastest way to split "the panel is broken" from "GL is broken".

Nothing already on the card can ask that question -- SDL2 has no
fbdev backend, so gzdoom, lzdoom and EmulationStation all need DRM/KMS or GL,
this kernel has no DRM driver bound yet, and the GL stack in the shared rootfs is
the RK3326's Mali-G31 blob for a GPU this SoC has not got.  doomgeneric needs
none of that: it writes 32-bit pixels into /dev/fb0, which is the framebuffer the
MVII LK was already scanning out when it jumped to the kernel, and reads
/dev/input/event0 from j36_mt6592_input.ko.

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

The panel module is out of tree and small, and it is worth knowing why it exists:
mtk_dsi calls component_add from inside mtk_dsi_host_attach, and host_attach only
runs when a mipi_dsi_driver has probed on the panel node.  No panel driver means no
DRM master and no card0, however correct everything else is.  All four of its power
callbacks are empty on purpose -- the LK has already powered the panel, released
reset, pushed 155 init records and lit the backlight -- and it refuses to probe
without j36,preserve-lk-state in the tree rather than pretend it can bring a dark
panel up.  Cold start is not implemented; the device tree keeps the init table,
the PMIC sequence and the GPIO sequence so that it can be.

Sound, and the two things about it that are not yet measured
------------------------------------------------------------

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

Two things it does are NOT proven on this board, and they are the reason it exists:

  1. The AFE functional clock.  MVII has the ungate sequence -- two power-down bits
     in TOPCKGEN CLK_CFG_3 and one in INFRACFG -- and compiles it out; its audio is
     soft-paced silence.  So nobody has ever seen AFE_DL1_CUR advance on this SoC.
     This driver ungates, logs CLK_CFG_AUD before and after, and reports on the
     first stream whether the cursor moved.  That log line is the deliverable.
  2. The class-D speaker.  It is off unless j36.audio=speaker asks for it, and even
     then it is powered only after the cursor has been seen moving.  VBAT is the
     system node on this PMIC, the amp is the largest load on the board, and MVII
     recorded it pulling VBAT under the undervoltage lockout with no cell fitted.
     The driver opens at level 8 of 11 and steps up one at a time.

So with the default word this card is silent by construction.  What it gains is
/dev/snd, a PCM that accepts and paces audio, a controlC0 with a Master volume and
switch, and one line in dmesg that says whether the hardware consumed any of it.

One thing that is NOT related, because it looked like it was: the boot log's

  /usr/lib/udev/rules.d/90-alsa-restore.rules:1 GOTO="alsa_restore_std" has no
  matching label, ignoring.

comes out of the shared Debian rootfs and has nothing to do with any of this.  Its
first two lines are early-exit GOTOs whose LABEL is in a file udev is not reading
here; it was there when there was no ALSA core at all, and it is still there now.

EmulationStation
----------------

It is the default now.  Four links had to close for that, each one measured rather
than assumed, and it is worth keeping the list because it is also the fault tree:

  1. A KMS device.  j36.mtkdrm=1 gives /dev/dri/card0 for the display, j36.lima=1
     gives /dev/dri/renderD128 for rendering.  Before this the panel was simplefb
     only -- a framebuffer, not a DRM device -- and SDL cannot use one.
  2. An SDL2 that can drive it.  This one needed nothing: the SDL2 in the shared
     rootfs has KMSDRM, wayland, offscreen and dummy compiled in.  There is no
     fbdev backend, which is why /dev/fb0 was never a route to ES, and KMSDRM
     needs the card node step 1 produces.
  3. A Mesa that can pair the two.  This is why the display driver is
     mediatek-drm specifically: Debian's armhf Mesa 25.0.7 ships BOTH halves of
     the pair, dri/lima_dri.so for the renderer and dri/mediatek_dri.so for the
     kmsro display, so a GBM surface on an mtk_drm card is rendered by lima
     without anything having to be told.  simpledrm has no such entry, which is
     one reason it was never the answer -- the other being that it binds the same
     `simple-framebuffer' the working display is on and evicts simplefb.
  4. A GL front end that is Mesa's.  Debian's Mesa was in the rootfs all along;
     what was not was its front door.  The shared rootfs points libEGL.so, libgbm.so{,.1,.1.0.0}
     and libGLESv1_CM.so at the RK3326's Mali-G31 blob, and
     emulationstation.service pins SDL to it with SDL_VIDEO_EGL_DRIVER=libEGL.so.
     Bifrost is a different architecture from this SoC's Utgard part, so that
     library cannot drive this GPU whatever else is true.  j36.es=1 closes it:
     see the next section.

So there was no Mesa to cross-compile.  The whole of step 4 is five small
libraries -- about 1.4 MB, most of it glvnd's dispatch table -- and an environment.

Worth knowing before reading further, because it is the one that decides whether
any of this can work: libgbm.so.1 is a name the blob took over, and
libEGL_mesa.so.0 -- Mesa's own EGL vendor, the library /usr/share/glvnd/
egl_vendor.d/50_mesa.json names -- carries libgbm.so.1 in its DT_NEEDED.  So on
the rootfs as it stands, asking for EGL loads glvnd, glvnd loads Mesa's vendor,
and Mesa's vendor pulls the Mali-G31 blob in as its GBM.  Mesa's EGL cannot come
up on this board without the payload.  That is a stronger statement than "ES will
not link", and it is why the fix is a library path rather than a rebuild.

The GL front end, and why it is a tmpfs
---------------------------------------

THE SHARED ROOTFS IS NOT WRITTEN.  That is the constraint everything here follows
from.  One Debian armhf rootfs serves two machines, and the R36S needs its
libEGL.so -> libMali.so symlinks to stay exactly where they are, because that blob
is the only thing that drives its Mali-G31.  Replacing them would trade this
board's GL for the other board's.

So with j36.es=1 the initramfs, after it has mounted the rootfs and before it hands
over, mounts a tmpfs on the rootfs's /run and puts three things in it: the
libraries from j36/gl/, the binary from j36/es/, and a systemd drop-in at
/run/systemd/system/emulationstation.service.d/j36-gl.conf that sets
LD_LIBRARY_PATH=/run/j36/gl and the SDL variables.  systemd reads drop-ins from
/run exactly as it reads them from /etc, and it reads them after the unit file, so
the drop-in's SDL_VIDEO_EGL_DRIVER replaces the unit's own rather than fighting it.
Mounting /run from the initrd is the documented half of the handover: PID 1 adopts
a /run that is already a tmpfs instead of mounting another over the top.

The binary goes in over /usr/bin/emulationstation/emulationstation as a BIND MOUNT,
which is the same constraint solved the same way: the mount table is in memory, the
file underneath is not touched, and switch_root moves the mount across with
everything else under /newroot.

Pull the card into an R36S and none of it exists.  Nothing was written to the
filesystem, so there is nothing to undo.

The renderer, and why the binary had to be rebuilt
-------------------------------------------------

GLES1 is the one API this stack cannot supply, and that is a property of the
package rather than of this SoC: Debian's armhf Mesa 25.0.7 is a -Dgles1=disabled
build, so eglCreateContext for an ES1 context returns 0x3003 EGL_BAD_ALLOC on lima,
on llvmpipe AND on softpipe.  The rootfs's EmulationStation is compiled
-DUSE_OPENGLES_10, so on this board it asks for the one thing that cannot be given,
does not check the answer, and aborts -- the 134 below.

The error code says there is nothing to configure around it.  Mesa's
validate_context_version rejects an API the screen does not advertise with
__DRI_CTX_ERROR_BAD_API, which surfaces as EGL_BAD_MATCH; BAD_ALLOC is
__DRI_CTX_ERROR_NO_MEMORY, a driver whose own context creation returned NULL.  ES1
is BAD_ALLOC everywhere here, so the version gate passed and
MESA_GL_VERSION_OVERRIDE has nothing to override.

What lima does give is ES2: "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1", context created
and made current on an ARGB8888 window surface.  So es/Renderer_GLES20.cpp is a
third renderer -- the 351v tree has only Renderer_GLES10.cpp and Renderer_GL21.cpp,
both fixed-function -- and the build compiles the same upstream commit the rootfs's
copy came from with -DUSE_OPENGLES_20 instead.  The renderer's own header comment
carries the details; three of its decisions matter from outside:

  - It links no GL library and resolves all 43 entry points through
    SDL_GL_GetProcAddress.  Every unversioned GL name on this rootfs -- libEGL.so,
    libGLESv2.so, libGLESv2.so.2, libGLESv1_CM.so* -- is a symlink to an
    SONAME-less ARMv8-A libMali.so, so any -l against them records a DT_NEEDED that
    is SIGILL on a Cortex-A7.  With none of them named, one binary is correct on
    both machines and no LD_PRELOAD is needed at all.
  - It asks for ES2 explicitly.  Both upstream renderers set
    SDL_GL_CONTEXT_MAJOR_VERSION twice instead of MAJOR then MINOR (GLES10: 1 then
    0; GL21: 2 then 1), and with major_version 0 SDL sends no context attributes,
    so EGL falls back to its own default of desktop OpenGL.  That typo is why the
    fixed-function binary was getting a compat context by accident.
  - Three one-line fragment programs instead of one with a mode uniform, because
    Utgard fragment hardware has no branching; and the ALPHA-texture program
    follows the spec's MODULATE row (Cv = Cf, Av = Af * At) rather than multiplying
    the texel in, which a naive shader does and which renders every glyph black.

The fallback, and why LD_PRELOAD is still in the file
----------------------------------------------------

Delete j36/es/ from the card and the rootfs's own binary runs.  /init notices,
says which one it mounted, and writes a different drop-in: LD_PRELOAD=libGL.so.1
and SDL_VIDEO_GL_DRIVER=libGL.so.1.  That path has never drawn a frame on this
board -- it is the 134 -- but it is what the card does without the directory, so it
is configured deliberately rather than left to chance.

Its reasoning, since the file still carries it.  That binary's undefined GL symbols
are glMatrixMode, glLoadMatrixf, glEnableClientState and 26 more like them, while
the only GL library it declares a dependency on is libEGL.so.  That works against a
vendor blob, where one object exports EGL and GLES1 and GLES2 together; glvnd's
libEGL exports no gl* entry point at all, so a library that has them must be in the
global scope before the binary starts or it does not start.  libGLESv1_CM.so.1 is
the obvious choice and the wrong one, for the reason above.  libGL.so.1 works
because all 29 of those symbols are exported by it with the signatures GLES1 gives
them -- client arrays, the matrix stack, glTexImage2D with GL_ALPHA,
GL_CLAMP_TO_EDGE and the stencil calls are common to GLES1 and to GL compat -- and
a compat context does come up, reporting "4.5 (Compatibility Profile) Mesa 25.0.7"
on llvmpipe.

Be precise about what the preload changes, because it is easy to overclaim: the
dispatch was never broken.  Run against the image's own libraries, in a binary
linked the way ES is -- gl* undefined, no libGL in DT_NEEDED -- with a compat
context current:

  LD_PRELOAD=libGL.so.1          glGetString -> "4.5 (Compatibility Profile)"
  LD_PRELOAD=libGLESv1_CM.so.1   glGetString -> "4.5 (Compatibility Profile)"
  no LD_PRELOAD                  glGetString -> NULL, from the stub it was linked
                                 against, which is the 134

Both preloads work, and glMatrixMode, glLoadMatrixf and glEnableClientState are
accepted with glGetError 0 through either.  glvnd is why: libEGL.so.1 and both GL
front ends share one libGLdispatch.so.0 current-context table, so whatever
eglMakeCurrent installed is what the stubs dispatch into, no matter which stub
library the symbol came from.  Only one copy of libGLdispatch is ever loaded, and
LD_LIBRARY_PATH puts the payload's ahead of the rootfs's.

So the preload was necessary and its name was not the fault.  ES aborting means the
CONTEXT was never created, and on the DRM nodes it never is -- which is what the
rebuild above is for, and why the probe's GL row was the measurement that decided
between the two:

  GL=ctx    desktop compat works on lima; the library payload alone would do.
  GL=0x3003 lima cannot create a compat context either.  Then no fixed-function
            context of any kind exists on this stack and no environment can make
            one: the ES1 half is a Mesa built without it, the compat half is the
            driver, and the renderer has to change.  It did.

Note that the error code discriminates before the source does.  Mesa's
validate_context_version rejects an API the screen does not advertise with
__DRI_CTX_ERROR_BAD_API, which surfaces as EGL_BAD_MATCH; BAD_ALLOC is
__DRI_CTX_ERROR_NO_MEMORY, which is what a driver whose own context creation
returned NULL produces.  Both ES1 rows here are BAD_ALLOC, so the version gate
passed and the versions are advertised -- MESA_GL_VERSION_OVERRIDE has nothing to
override and is not worth trying.

One failure has a signature worth knowing on sight:

  emulationstation.sh: line 27: NNN Illegal instruction "$esdir/emulationstation"
  emulationstation.service: Main process exited, code=exited, status=132/n/a

Status 132 is 128+4, SIGILL, and it happens before main().  It means the process
loaded the RK3326 Mali blob: libMali.so is an ARMv8-A object (readelf -A says
Tag_CPU_arch: v8) and the MT6592 is a Cortex-A7, so the first instruction the blob
executes is one this SoC does not have.  EmulationStation itself is v7 and fine.
So SIGILL is never an ES bug and never a Mesa bug -- it means the GL payload did
not take, and the loader fell back to /usr/lib.  Check for "es: GL front end in
/run/j36/gl" in the boot log, and `ls /run/j36/gl` on the device: if libEGL.so is
not in there, nothing else in this section matters yet.  With j36/es/ on the card
this one should be gone for good: that binary names no GL library at all, so there
is nothing for the loader to resolve to the blob.

The other signature is the one after that one is fixed, and it is the fixed-function
binary's -- if it appears while "es: the GLES 2.0 binary is mounted over the
rootfs's" is in the boot log, then the bind mount and the abort disagree and the
mount is what to doubt first:

  emulationstation.sh[878]: terminate called after throwing an instance of
                            'std::logic_error'
  emulationstation.sh[878]:   what():  basic_string: construction from null is not valid
  emulationstation.service: Main process exited, code=exited, status=134/n/a

134 is 128+6, SIGABRT from an uncaught C++ exception, and it is NOT a missing
EmulationStation -- a missing binary is status 127 and a different message.  It is
one line of upstream ES, es-core/src/renderers/Renderer_GLES10.cpp:129:

  sdlContext = SDL_GL_CreateContext(getSDLWindow());
  SDL_GL_MakeCurrent(getSDLWindow(), sdlContext);
  ...
  std::string glExts = (const char *)glGetString(GL_EXTENSIONS);

Neither return value is checked, so when the context cannot be created the next GL
call goes to glvnd's no-op dispatch, returns NULL, and std::string(NULL) throws.
The abort is therefore a report that eglCreateContext failed and that the reason
was discarded one line earlier.

j36.es=debug asks for the reason.  It adds ES's --debug -- which reaches stderr
even with LogLevel=disabled in es_settings.cfg, because Log::~Log() writes to
stderr at LogDebug while Log::init() never opens the file -- plus
EGL_LOG_LEVEL=debug, MESA_DEBUG=1, LIBGL_DEBUG=verbose and Restart=no, so there is
one trace to read instead of six.  What that boot showed, in order:

  libGL: Can't open configuration file /home/ark/.drirc: No such file
  libEGL debug: No DRI config supports native format <name>          (repeatedly)
  libEGL debug: EGL user error <code> in dri2_create_context
  what():  basic_string: construction from null is not valid

Only the third line is a failure.  The .drirc lines are Mesa looking for tuning
files that were never written, and the "No DRI config supports native format"
lines come from Mesa's own per-visual walk at eglInitialize: libEGL_mesa.so.0
carries that literal next to dri2_create_context and DRI2: failed to validate
config, it prints a pipe-format name, and it says one line for every format in
Mesa's visual table that this driver pair does not expose.  A kmsro display device
exposes few, so most of the table misses and the noise is expected.

The third line is eglCreateContext being rejected inside Mesa, and the order of
SDL's calls narrows it a long way before anything is measured.  SDL's KMSDRM
backend chooses a config, creates a gbm surface, calls eglCreateWindowSurface and
only then creates a context (SDL_kmsdrmvideo.c: KMSDRM_CreateSurfaces at 1190,
SDL_EGL_SetRequiredVisualId with GBM_FORMAT_ARGB8888 at 1236).  So by the time
dri2_create_context runs, eglChooseConfig has already matched and the window
surface has already been created:

  - a config was found for EGL_RENDERABLE_TYPE=EGL_OPENGL_BIT, and Mesa sets a
    config's RenderableType from the display's supported client APIs, so this
    driver pair does advertise desktop OpenGL;
  - the surface came up on an ARGB8888 gbm surface, and surface creation performs
    the same surface-type/colorspace pairing check that the context path does and
    fails with its own message -- so the config, the visual and the missing alpha
    channel are all cleared.

That leaves the driver refusing the context itself, and the EGL error code says
which way: BAD_MATCH is the DRI layer rejecting the API or version, BAD_ALLOC is
the driver's own context creation returning NULL.  Those have different fixes and
the same 134.

Which is what j36/eglprobe is for.  It runs before ES under j36.es=debug and is
repeated after ES exits, so it survives the scroll, and it prints per DRM node:
the EGL version and client APIs, the config table summarised (how many configs
carry each renderable bit, and how many window configs are AR24 and XR24), then
one row of GL / ES1 / ES2 each showing either a created context taken all the way
to glGetString on a current ARGB8888 window surface, or the exact EGL error.  It
probes /dev/dri/card0 and /dev/dri/renderD128 separately because those are two
different chips here -- mtk_drm displays, lima renders, Mesa bridges them with
kmsro -- so contexts on the render node but not the display node means the kmsro
pairing, and failures on both mean lima.  A second run adds a row with no DRM
device at all and LIBGL_ALWAYS_SOFTWARE=1, which is the control: swrast does
desktop GL, GLES1 and GLES2 on any machine, so a row that fails there is the
payload's fault and not the board's.

What it found, and it was the control row that found it:

  ES1 = 0x3003(BAD_ALLOC) on lima, on llvmpipe and on softpipe alike
  GL  = ctx/cur "4.5 (Compatibility Profile) Mesa 25.0.7-2+deb13u1" on swrast
  ES2 = ctx/cur "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1" on lima, current on an
        ARGB8888 window surface

The third line clears the whole lower half of the stack in one measurement: gbm,
the kmsro mediatek-to-lima pairing, the config, the ARGB8888 visual, lima's own
context creation and the kernel uAPI all work.  The first line is a Mesa built
without GLES1.  Between them they are the whole argument for the rebuild: the one
API measured to work here is the one no renderer in the tree was written against.

The GL row on the DRM nodes is what the fixed-function binary's fate hung on,
because it asks for desktop GL and nothing else -- setupWindow() sets
SDL_GL_CONTEXT_MAJOR_VERSION twice (1, then 0), never sets
SDL_GL_CONTEXT_PROFILE_MASK, and with no RPI video driver in this SDL2
KMSDRM_GLES_DefaultProfileConfig is compiled out to an empty function.  With
major_version 0 SDL sends no context attributes at all, so the probe's GL row and
SDL's request were the same call.  The GLES 2.0 binary does not depend on it: it
sets PROFILE_MASK_ES and MAJOR 2 / MINOR 0, so its row is the ES2 one, which is
measured working.

The black panel, and the three faults it can be
-----------------------------------------------

Where this stands: the GLES 2.0 binary starts, does not abort, and the panel goes
black.  That is progress and not a new failure -- 134 is gone, so a context was
created and made current, and the console text disappearing means something took
the CRTC.  But "black" is the one symptom that three unrelated faults share, and
guessing between them costs a boot each:

  1. this renderer draws nothing -- a program that did not link, or a projection
     that puts ES's 0..640 pixel coordinates outside the frustum;
  2. ES asks for nothing to be drawn -- a theme that did not parse, a gamelist
     that is empty, a resource that did not load;
  3. everything is drawn correctly and the buffers never reach the panel -- the
     format the KMSDRM backend chose for its gbm surface, the connector or the
     CRTC it picked, or a page flip the display driver refused.

j36.es=debug now separates all three in one boot, and none of it is on in
j36.es=1:

  GLES2: self test 640x480, centre pixel ff 00 ff ff ..., glGetError 0x0000
      One magenta quad, drawn straight in NDC with both matrices at identity,
      read back with glReadPixels before ES has drawn anything.  ff 00 ff means
      the context, the three programs, the attribute arrays and the draw path all
      work, which retires fault 1 outright.  Black here, or a shader/link error
      logged just above it, is fault 1 and the log says which line of GLSL.

  GLES2: first draw, program N, 4 verts, v0 (0.0, 0.0), proj sx 0.00312 sy -0.00417
      ES's first real draw.  sx should be 2/screenWidth and sy -2/screenHeight;
      sx 1.00000 sy 1.00000 is a projection left at identity, which clips the
      whole UI away without raising one GL error.

  GLES2: frame 1, 37 draws since the last line
      Frames 1-3 and then one line every 600.  A frame with 0 draws is fault 2:
      ES decided there was nothing to show, and no shader will change that.

  A magenta panel
      The clear colour under J36_ES_GL_PROBE.  If the panel is magenta, the
      buffers this renderer swaps are the buffers being scanned out -- fault 3 is
      retired and anything still black on top of the magenta is ES's drawing.  If
      the panel stays black while the self test reads ff 00 ff, that is fault 3
      exactly, and the SDL_LOG_PRIORITY_VERBOSE lines from the same boot carry the
      KMSDRM backend's own account of the modeset: which connector, which CRTC,
      which plane format, and what drmModeAddFB2 or drmModePageFlip said.

One candidate for fault 3 has to be withdrawn before it costs a boot.  This
renderer asks for SDL_GL_ALPHA_SIZE 8 where the fixed-function one asked for no
alpha, and that reads like the cause of an ARGB8888 framebuffer a plane might
refuse -- but SDL is not choosing: KMSDRM_CreateSurfaces hardcodes
GBM_FORMAT_ARGB8888 for its gbm surface (SDL_kmsdrmvideo.c:1197) and then pins
that visual with SDL_EGL_SetRequiredVisualId whatever was asked for.  Dropping
ALPHA_SIZE to 0 changes which EGL config is chosen and not one byte of the buffer
that is scanned out, so it is not the experiment it looks like.  What remains for
fault 3 is fbcon -- a console released without the CRTC handed over leaves the
panel scanning out nothing while ES renders perfectly into buffers nobody reads --
and the two things ES's own log cannot see: whether a modeset reaches the glass at
all, and whether the OVL blends per-pixel alpha.

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
paints, and it takes ES, then SDL, then GL, then gbm out of the path one step at a
time, holding each frame three seconds because the instrument for this one is an
eye.  It speaks DRM with raw ioctls -- the uapi structs are ABI and libdrm would be
a fourth library that can be missing -- and prints the connector, the mode it was
given, the CRTC and whatever framebuffer was already on it.

It is NOT run at boot, and the reason is worth knowing before running it by hand.
A DRM client that sets a mode and exits leaves the panel black: on close the kernel
runs drm_fb_release() over that client's framebuffers, and removing the framebuffer
a CRTC is scanning out disables the CRTC.  This kernel is built
CONFIG_DRM_FBDEV_EMULATION=n on purpose, so there is no in-kernel fbdev client for
drm_client_dev_restore() to hand the pipe back to.  -p and -c therefore hold the
panel until the next reboot, and /dev/fb0 keeps accepting writes that are no longer
seen.  Run -f first, not after.

  1  RED, XR24, filled with memset()          modeset + DSI + panel + OVL, no
                                              alpha and no Mesa anywhere
  2  MAGENTA, AR24 alpha ff, memset()         the same, in the format SDL uses
  3  MAGENTA, AR24 alpha 00, memset()         the same buffer, transparent
  4  MAGENTA, lima into a gbm surface         ES's path with ES, SDL and the
                                              renderer removed
  5  GREEN, lima's second frame               the swap chain rotating

Read it as four verdicts:

  nothing at all           the modeset does not reach the glass, and no part of
                           EGL is involved.  Look at mtk_dsi against the state the
                           LK left, and at the mode the panel driver reports:
                           j36_jd9365_panel.c adopts a live panel and sends it no
                           init table, so a DSI re-initialised to a different
                           timing has nothing that puts it back.
  1 and 2 but not 3        the OVL blends per-pixel alpha against a black
                           background.  That alone explains a black ES, and the
                           fix is in the renderer, not the kernel:
                           Renderer_GLES20.cpp clears to (0, 0, 0, 0) and every
                           pixel ES does not overdraw is transparent black.
  1, 2 and 3 but not 4/5   the display path is sound and the gbm/kmsro pairing is
                           the fault: lima renders into a buffer the OVL never
                           fetches.  The ADDFB2 line names the handle and stride
                           it refused.
  all five                 the display path is sound end to end and a black ES is
                           ES's own drawing -- fault 1 or 2, and the self test and
                           per-frame draw count above are the evidence.

`/run/j36/eglprobe -p' by hand does the same thing from a console at any time, and
it keeps the panel afterwards for the reason above -- reboot when it is done.  The
dashboard's own "3D cube" card runs -c the same way, which is why that card asks
twice before it starts.

EmulationStation is not started in these builds at all: /init masks the unit in
/run/systemd/system.control and resets its ExecStart as well.  To get it back for
one boot, drop j36.dash=1 from the bootargs in mvii/boot.conf.

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

Rebooting
---------

`reboot' works from here on: the device tree describes the TOPRGU watchdog at
0x10007000, and mtk_wdt registers the restart handler that machine_restart()
calls.  Without that node userspace shuts down cleanly and then prints "Reboot
failed -- System halted", which is a halt, not a crash -- the card is safe to
pull at that point.  `poweroff' still ends the same way, because nothing drives
the PMIC yet; hold the power button instead.

Licence
-------

See LICENSE.txt beside this file.  In short: the MixOS bring-up work is under the
Microsoft Public License, the three kernel modules and the kernel itself are
GPL-2.0-only, and everything else on the card is Debian's under its own terms.
MixOS is a divergent fork of dArkOS, which continues ArkOS; the operating system
underneath is Debian.  MixOS supports the MediaTek line of processors, and this
card is that support.
README

# Ms-PL section 3(C) is the reason this is written and not just linked from the
# repository: a card handed to somebody else is a distribution, and it has to carry
# its notices with it.  It is not added to SHA256SUMS, for the same reason
# README.txt is not -- the sums cover what the machine executes.
cat > "$SDBOOT/LICENSE.txt" <<'LICENCE'
MixOS -- J36 Ultra (MediaTek MT6592, ARMv7) SD card payload
Copyright (c) 2025-2026 the MixOS project and contributors

MixOS supports the MediaTek line of processors.  This card is that support: a
32-bit ARM kernel, an mtk_drm display path, mtk-sd storage, the MT6592 AFE, a
keypad adapter and Mesa's lima/kmsro pair, on a Cortex-A7 from 2013.

This file sits on the FAT32 BOOT partition, which is where a card is opened, but
it covers everything MixOS put on the card.  Three partitions carry it, and BOOT
is FAT because the MVII LK reads FAT32 and nothing else:

    BOOT, FAT32   the launcher, and only that: zImage, mt6592-j36-ultra.dtb,
                  initrd.img, mvii/boot.conf, README.txt and this file.
    ROOTFS, ext2  Debian, and MixOS's own tree at /opt/mixos -- unpacked there
                  from sd-root.tar.gz.  Every "bin/", "qt/" and "j36/" path below
                  means /opt/mixos/... on this partition.
    DATA, ext2    your home directory, mounted at /home/virtua.  A shell starts
                  here, the dashboard's Files page opens here, and nothing MixOS
                  ships is licensed by this file on it -- what is on it is yours.
                  roms/ inside it is the old EmulationStation tree, which /roms
                  still points at.

This payload is not licensed uniformly.  Saying otherwise would be a false
statement about other people's code.

Microsoft Public License (Ms-PL), in full below:

    bin/mixdash             the MixOS dashboard (Qt5 Widgets on linuxfb)
    j36/eglprobe            the EGL/GBM/DRM scanout probe
    j36/mfgpower            the MFG power-domain bring-up probe
    mvii/boot.conf          the MVII LK hand-off
    README.txt on either partition, and this file -- the documentation

GNU General Public License, version 2 only:

    zImage                  Linux 6.12 LTS, plus two MixOS patches (mtk-sd and
                            drm/mediatek for MT6592)
    initrd.img              busybox and a shell /init
    j36/modules/*.ko        lima and its dependencies -- kernel modules
    j36/mtkdrm/*.ko         mtk_drm, and MixOS's j36_jd9365_panel
    j36/audio/*.ko          the ALSA core, and MixOS's j36_mt6592_audio
    j36/usb/*.ko            musb and its MediaTek glue, usbhid, udl, the SCSI and
                            mass-storage set, ntfs3, and MixOS's
                            j36_mt6592_usb_phy
    j36/power/*.ko          MixOS's j36_mt6592_pmic and j36_mt6592_backlight
    j36_mt6592_input.ko     MixOS's keypad and GPIO key adapter

    The six MixOS modules are GPL-2.0-only deliberately and are not Ms-PL: they
    derive from and link against GPL-2.0-only kernel internals, and Ms-PL is not
    GPL-compatible.

Their own terms:

    j36/gl/*.so*            Mesa, from Debian (MIT and others)
    qt/lib, qt/plugins      Qt 5.15 and its runtime closure, from Debian: LGPL-3
                            with Qt's own exceptions, and GPL/LGPL/MIT/others for
                            the libraries it needs
    qt/fonts                DejaVu Sans, under the Bitstream Vera licence
    bin/doom, share/doom    doomgeneric and Doom's engine source, id Software's
                            under the GPL, as Debian and doomgeneric ship them
    j36/es/emulationstation EmulationStation-fcamod, under its own licence, with a
                            third renderer added by MixOS that follows it
    the rootfs on ROOTFS    Debian.  Per-package terms are in
                            /usr/share/doc/*/copyright on the running device.

SOURCE.  Everything GPL-2.0-only here is built from public source: Linux 6.12 LTS
from kernel.org with the two patches in the MixOS repository under
device/j36-ultra/linux/, the three MixOS modules in the same directory, and
busybox, Mesa, Qt, the fonts, doomgeneric and EmulationStation as Debian packages
them.  The MixOS work is in the same repository: the dashboard in
device/j36-ultra/tools/mixdash, eglprobe and mfgpower beside it.  The MixOS build
script that assembled this card is device/j36-ultra/build-in-vm.sh, and it is the
complete recipe -- nothing here was produced by hand.

ATTRIBUTION.  MixOS is a divergent fork of dArkOS, itself a Debian-based
continuation of ArkOS by christianhaitian.  Divergent is meant literally: MixOS
adds a second SoC vendor and a 32-bit kernel to a build system that assumed
neither, and neither dArkOS nor ArkOS endorses it, is affiliated with it, or should
receive its bug reports.  Thanks are owed to the Debian project above all -- the
operating system this device runs is Debian, and MixOS is a device port on top of
it -- and to ArkOS and dArkOS, to MediaTek's documentation and vendor sources, to
Mesa for lima and kmsro, and to the Linux kernel, SDL, busybox and
EmulationStation projects.  MixOS is not affiliated with or endorsed by the Debian
project, MediaTek, or Microsoft; "Ms-PL" is simply the licence chosen for the
MixOS work.


Microsoft Public License (Ms-PL)

This license governs use of the accompanying software. If you use the software,
you accept this license. If you do not accept the license, do not use the
software.

1. Definitions

The terms "reproduce," "reproduction," "derivative works," and "distribution"
have the same meaning here as under U.S. copyright law.

A "contribution" is the original software, or any additions or changes to the
software.

A "contributor" is any person that distributes its contribution under this
license.

"Licensed patents" are a contributor's patent claims that read directly on its
contribution.

2. Grant of Rights

(A) Copyright Grant- Subject to the terms of this license, including the license
conditions and limitations in section 3, each contributor grants you a
non-exclusive, worldwide, royalty-free copyright license to reproduce its
contribution, prepare derivative works of its contribution, and distribute its
contribution or any derivative works that you create.

(B) Patent Grant- Subject to the terms of this license, including the license
conditions and limitations in section 3, each contributor grants you a
non-exclusive, worldwide, royalty-free license under its licensed patents to make,
have made, use, sell, offer for sale, import, and/or otherwise dispose of its
contribution in the software or derivative works of the contribution in the
software.

3. Conditions and Limitations

(A) No Trademark License- This license does not grant you rights to use any
contributors' name, logo, or trademarks.

(B) If you bring a patent claim against any contributor over patents that you
claim are infringed by the software, your patent license from such contributor to
the software ends automatically.

(C) If you distribute any portion of the software, you must retain all copyright,
patent, trademark, and attribution notices that are present in the software.

(D) If you distribute any portion of the software in source code form, you may do
so only under this license by including a complete copy of this license with your
distribution. If you distribute any portion of the software in compiled or binary
form, you may do so only under a license that complies with this license.

(E) The software is licensed "as-is." You bear the risk of using it. The
contributors give no express warranties, guarantees, or conditions. You may have
additional consumer rights under your local laws which this license cannot change.
To the extent permitted under your local laws, the contributors exclude the
implied warranties of merchantability, fitness for a particular purpose and
non-infringement.
LICENCE

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
# changes none: an R36S booting the same card gets its own libEGL.so symlink, its own
# EmulationStation and its own units, and never looks in /opt.
#
# WHY IT IS A TARBALL AS WELL AS A TREE.  The SONAME aliases in qt/lib are symlinks and
# mfgpower, eglprobe, mixdash and doom have to stay executable.  A tarball is the copy
# that cannot lose either -- or the ownership -- whatever machine does the copying, and
# the reason it is not simply a directory to drag across in a file manager.
#
#   sudo tar -C /path/to/the/mounted/ROOTFS -xzf sd-root.tar.gz
#
# It is not fatal for any of this to be absent, but it is not silent either, and that
# is the lesson of a boot that ended at hostnamed with nothing on the panel: with no
# /opt/mixos on the card /init writes mixdash-missing.service instead, which says on
# the console what it looked for and where.  EmulationStation is still not started --
# it aborts 134 on this board -- so a card with no payload comes up to a readable
# console and not to a shell.
# SDROOT was declared and cleared up beside SDBOOT, because the j36/ payload above is
# staged into it.  So this section adds to a tree that may already have files in it,
# and the tarball below is written whenever ANY of them are there -- not only when the
# dashboard is.  With J36_MIXDASH=0 and no Doom, opt/mixos/j36 is still the modules,
# mfgpower and the probe, and a build that silently shipped no tarball for them would
# leave a card that boots to a dashboard-less console with no GPU either.
if [[ -n "$MIXDASH_BIN" || -n "$DOOM_BIN" ]]; then
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
fi

# Ms-PL 3(C) again: mixdash is MixOS's own code, so the tree it ships in has to carry
# its notice.  Short, and pointing at the full text on BOOT rather than repeating 40
# lines of licence in two places on one card.
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

The third partition, DATA, is not this one.  It is your home directory, mounted at
/home/virtua: a shell starts there, the dashboard's Files page opens there, and it is
the only partition on the card meant to be written from the device.  Nothing in this
payload is installed to it.

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

j36/ -- what /init loads, one directory per bootargs word.  Delete any of them and
the matching j36. word finds nothing, says so on the console, and the boot carries
straight on; that is the recovery path for all of them, from any machine that reads
SD cards.

  j36/modules          lima and its dependencies, plus modules/load.order naming
                       them in the order insmod needs.        j36.lima=1
  j36/mfgpower         static ARMv7 helper that brings the GPU power domain up
                       through the SPM before lima probes.    j36.lima=1
  j36/mtkdrm           the display set and its own load.order.  j36.mtkdrm=1
  j36/audio            the ALSA core and the MT6592 AFE adapter.  j36.audio=1;
                       j36.audio=speaker also powers the class-D amp, which hangs
                       off VBAT and so needs a cell fitted.
  j36/usb              the host stack for the one MUSB port: the PHY, musb and its
                       glue, usbhid, udl for a DisplayLink dock's HDMI, and the
                       disk set -- scsi_mod, sd_mod, usb-storage and ntfs3 -- which
                       is what makes an external drive appear and automount under
                       /media.  The PHY drives DRVVBUS, so the port sources 5 V off
                       VBAT and a cell should be fitted; j36.usb=novbus leaves the
                       pad alone.  Two modules here also live in j36/mtkdrm;
                       whichever loaded first wins.            j36.usb=1
  j36/gl               Mesa's GL front end, bind-mounted at /run/j36/gl ahead of
                       the rootfs's RK3326 Mali blob.  links/ records the SONAME
                       aliases, kept from when this payload was on FAT.  j36.gl=1
  j36/eglprobe         asks the DRI nodes what they can do and prints the answer.
                       j36.gl=debug runs it; -f works from a shell at any time.

The initramfs writes mixdash.service into /run/systemd/system -- in memory, never on
the card -- and wants it from multi-user.target, so this payload does not depend on
any unit the rootfs happens to have installed or enabled.  EmulationStation is masked
at the same time, in /run/systemd/system.control.  Delete this directory and neither
is written: instead the console gets mixdash-missing.service, saying which partitions
were searched.  EmulationStation stays masked even then, because it aborts on this
board; to hand the boot back to the rootfs's own shell, drop j36.dash=1 from the
bootargs in mvii/boot.conf on the BOOT partition.

Licence: the MixOS work here -- bin/mixdash, j36/mfgpower, j36/eglprobe -- is under
the Microsoft Public License; the full text is in LICENSE.txt on the BOOT partition.
Qt, its dependencies and the fonts are Debian's packages under their own terms
(LGPL-3 with Qt's exceptions for Qt itself; see /usr/share/doc on a Debian machine).
The kernel modules under j36/ are GPL-2.0, from the Linux tree they were built from.
Mesa in j36/gl is under the MIT licence.  doomgeneric and Doom's engine source are
id Software's under the GPL, as Debian and doomgeneric ship them.  MixOS is a
divergent fork of dArkOS, which continues ArkOS; the operating system underneath is
Debian.  MixOS supports the MediaTek line of processors.
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
          j36_mt6592_input.ko initramfs-j36-ultra.cpio.gz
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
    if [[ -f $PAYREL/es/emulationstation ]]; then
        sums+=("$PAYREL/es/emulationstation")
    fi
    sha256sum "${sums[@]}" > SHA256SUMS
    {
        echo "licence=Ms-PL for the MixOS bring-up work, GPL-2.0-only for the kernel and the three MixOS modules, per-payload in sd-boot/LICENSE.txt and summarised in sd-root/opt/mixos/README.txt"
        echo "kernel_branch=$KERNEL_BRANCH"
        echo "kernel_release=$KERNEL_RELEASE"
        echo "kernel_arch=arm (ARMv7, 32-bit)"
        echo "zimage_size=$(stat -c %s zImage)"
        echo "dtb_sha256=$(sha256sum mt6592-j36-ultra.dtb | awk '{print $1}')"
        echo "bootimg_size=$(stat -c %s boot.img) (slot 0x900000)"
        echo "storage=msdc1 mtk-sd mediatek,mt6592-mmc (ext2, ext4, btrfs, exfat, vfat)"
        echo "card_layout=p1 BOOT vfat = launcher only (zImage, dtb, initrd.img, mvii/boot.conf, LICENSE.txt, README.txt); p2 ROOTFS ext2 = the OS, /opt/mixos included; p3 DATA ext2 = the login user's home, mounted at ${DATA_MOUNT_POINT:-/home/virtua}, with the legacy roms/ tree inside it and /roms a symlink to that"
        echo "rootfs_format=ext2, set in setup_partition.sh and device/r36-ultra/build-in-vm.sh; the MVII LK reads FAT32 only, so BOOT is FAT and the OS partition is free to be the simplest filesystem both kernels on this card handle"
        echo "payload=$PAYREL (J36_PAYLOAD_ON=$PAYLOAD_ON; /init looks in the rootfs /opt/mixos/j36 first, then j36/ on BOOT for a card written by an older build)"
        echo "msdc1_irq=GIC_SPI 72 (INTID 104 - 32)"
        echo "userspace=net+unix+namespaces (systemd 257 on the shared armhf rootfs)"
        echo "wireless=off"
        echo "reboot=mtk_wdt via watchdog@10007000 (mediatek,mt6589-wdt)"
        echo "console=tty0 last, journald forwarded to it"
        echo "firstboot=masked (RK3326 script, no /roms.tar in a GUI-mode build)"
        echo "batt_led=masked (Restart=always on a power_supply this kernel has not got)"
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
            echo "shell_es=emulationstation.service is masked in /run/systemd/system.control (the one runtime dir that outranks the /etc its unit is in), plus a drop-in that resets ExecStart to an echo in case the mask is ignored"
            echo "shell_find=/init looks in the rootfs first, then mounts every other partition read-only looking for opt/mixos/bin/mixdash (or mixos/bin/mixdash, for a tarball unpacked one level down); every partition it tries is named on the console, mounted or unreadable"
            echo "shell_missing=when nothing is found, /init also writes /run/systemd/system/mixdash-missing.service, which repeats the reason and the fix on the console six times at 20 s -- because the initramfs lines have scrolled off by then and a boot that ends at hostnamed looks the same as ten other faults"
            echo "shell_card=/run/j36/card is what the dashboard's Files page opens on, there being no keyboard here to mount anything by hand; on a current card it is a symlink to ${DATA_MOUNT_POINT:-/home/virtua}, the home partition's mount point, which /init recognises by a .mixos-home stamp at the partition root and leaves for systemd to mount rw -- a read-only mount from the initramfs would make that fstab entry fail with EBUSY, silently, it carrying nofail; on a card written before this layout it is a read-only mount of the first partition that is neither the rootfs nor BOOT (skipped by carrying j36/ or mvii/, not by name), exfat and vfat first"
            echo "shell_nodash=without j36.dash=1 nothing is staged at all and /init says so, naming the word to add -- a rootfs whose EmulationStation is not even enabled otherwise boots to nothing and explains nothing"
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
            echo "shell=not staged; /init says so on the console and still does not start EmulationStation (it aborts 134 here and its unit restarts it)"
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
            echo "audio_clock=UNPROVEN: this is the first ungate of AFE_CG on this board; dmesg says whether DL1_CUR advances"
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
            echo "gl_install=tmpfs on the rootfs /run plus a systemd drop-in; nothing is written to the card"
            echo "gl_boot_word=$(grep -o 'j36\.gl=[a-z0-9]*' sd-boot/mvii/boot.conf)"
            echo "gl_users=mixdash's 3D cube card only; with j36.dash=1 no ES drop-in is written and the GLES 2.0 binary is not staged either"
            if [[ -f $PAYREL/es/emulationstation ]]; then
                echo "es_binary=j36/es/emulationstation ($(stat -c %s $PAYREL/es/emulationstation) bytes, stripped ARMv7)"
                echo "es_commit=$ES_COMMIT (the rootfs's own EmulationStation commit)"
                echo "es_renderer=USE_OPENGLES_20, es/Renderer_GLES20.cpp, no GL library in DT_NEEDED"
                echo "es_renderer_reason=ES1 is 0x3003 EGL_BAD_ALLOC everywhere (mesa built -Dgles1=disabled); ES2 is ctx/cur on lima"
                echo "es_install=bind mount over /usr/bin/emulationstation/emulationstation from a tmpfs; the rootfs file is untouched"
                echo "es_gl_driver=SDL_VIDEO_GL_DRIVER=libGLESv2.so.2, no LD_PRELOAD"
            else
                echo "es_binary=not staged; the rootfs's fixed-function one runs and aborts with status 134"
                echo "es_gl_driver=SDL_VIDEO_GL_DRIVER=libGL.so.1 with LD_PRELOAD=libGL.so.1 (the fallback)"
            fi
            if [[ -f $PAYREL/eglprobe ]]; then
                echo "gl_probe=j36/eglprobe ($(stat -c %s $PAYREL/eglprobe) bytes, dynamic ARMv7, dlopens libEGL.so.1 and libgbm.so.1)"
                echo "gl_probe_run=-f 1 from mixdash-probe.service, Type=oneshot RemainAfterExit=yes, so once per boot and NOT once per mixdash start attempt; under j36.gl=debug that unit also runs the node probes and -s with LIBGL_ALWAYS_SOFTWARE=1 as the control, and mixdash replays the log from ExecStopPost. As mixdash's own ExecStartPre these re-ran on all three restarts, which meant three EGL inits on lima and the bars repainted over each attempt's error"
                echo "gl_probe_fb=-f opens /dev/fb0 and nothing else: it counts the pixels already there (all black = nothing drew, a picture = something took the scanout), undoes a backlight at brightness 0 and a tty0 left in KD_GRAPHICS, then paints eight colour bars with the CPU. Bars then a dashboard = both halves work; bars that stay = mixdash never started; no bars = nothing userspace draws will be seen."
                echo "gl_probe_nodes=-i names every /dev/dri node and says which one modesets; nothing here hard-codes card0 any more, because on this kernel card0 is lima and GETRESOURCES on it returns EOPNOTSUPP"
                echo "gl_probe_paint=-p (five CPU and lima frames) and -c (a rotating cube, GLES2 through lima, page-flipped) are NOT run at boot: both modeset, and on close the kernel releases their framebuffer and disables the CRTC, which with CONFIG_DRM_FBDEV_EMULATION=n nothing re-enables -- so either one holds the panel until the next reboot. The 3D cube card starts -c on request, after asking twice."
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
    ls -1t "$ROOT"/MixOS_*_*_*.img 2>/dev/null | head -n 1 || true
}

# What the injected image is a function of: the image it was injected into, and the two
# payloads.  Recorded after a successful injection so a re-run that changes neither can
# skip the injection -- which is the whole cost of this section.  The image's own size and
# mtime are enough for the first half because injection is the only thing that writes to
# it here, and the recorded value is read back after that write; a base rebuild moves
# both, and so does a new commit, because the image is named after the commit.
image_export_signature() {
    local img="$1"
    {
        stat -c 'image %s %Y' "$img"
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
# Nothing on the device fixes it either.  firstboot.service is masked in bootargs, and
# even unmasked, what it resizes is the DATA partition.  So the filesystem is grown here,
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
                        log "image: It boots and the dashboard runs, but p3 is still vfat/exfat"
                        log "image: and there is no /home/virtua home partition in it.  To get"
                        log "image: that layout the base image has to be rebuilt from its"
                        log "image: filesystem stage -- see the README; resuming will not do it,"
                        log "image: because the build root itself is btrfs."
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
elif [[ -f "$IMAGE_STAMP" ]] && \
     [[ "$(cat "$IMAGE_STAMP")" == "$(image_export_signature "$BASE_IMAGE")" ]]; then
    log "image: $(basename "$BASE_IMAGE") already carries this exact payload -- nothing"
    log "image: to inject."
elif inject_into_image "$BASE_IMAGE"; then
    image_export_signature "$BASE_IMAGE" > "$IMAGE_STAMP"
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
    elif [[ -n "$BASE_IMAGE" ]] && [[ -f "$IMAGE_STAMP" ]]; then
        printf 'image=%s\n' "$(basename "$BASE_IMAGE")"
        printf 'payload=in-image\n'
    elif [[ -n "$BASE_IMAGE" ]]; then
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
