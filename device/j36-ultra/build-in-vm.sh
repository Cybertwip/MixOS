#!/usr/bin/env bash
# SPDX-License-Identifier: MS-PL
# Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
# device/j36-ultra/LICENSE for the full text and for what it does not cover.
# Incremental J36 Ultra ARMv7 bring-up builder. Run inside Ubuntu, normally via
# ./build-j36-ultra.sh on macOS. It never invokes the RG351MP/R36 build target.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
WORK="${J36_WORK_DIR:-$HOME/j36-ultra-work}"
# The MVII board sources are vendored in this checkout, so nothing here reaches
# outside it. They used to be rsynced into the VM from a PowerEngine tree on the
# host, which made a MixOS build depend on a sibling repository being present.
DRIVERS="${J36_DRIVERS_DIR:-$ROOT/device/j36-ultra/mvii-board}"
EXPORT_DIR="${J36_EXPORT_DIR:-$WORK/export}"
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
# the GPU section further down, and now SOUND and SND, for the audio section
# after it. A native DSI/display driver is still on the outside.
#
# WIRELESS, WLAN and BT stay off, and they are disabled here rather than left
# out: all three live inside `if NET' in net/Kconfig and WIRELESS defaults to y,
# so once NET is on they would come back by default. Both the =y for NET and the
# "is not set" lines for these are in .config before the single olddefconfig
# below, which is what makes the explicit n stick.
for symbol in \
    MEDIA_SUPPORT WIRELESS WLAN BT USB_SUPPORT SCSI ATA \
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
# BTRFS because the rootfs this card already carries is btrfs: MixOS's own
# scripts/setup_partition.sh sets ROOT_FILESYSTEM_FORMAT="btrfs". EXT4 because a
# hand-made card usually is not, and /init tries both.
#
# EXFAT and VFAT are not for /init -- they are the other two partitions on the
# same card, and the rootfs mounts them itself.  finishing_touches.sh writes the
# post-expansion fstab as
#
#   LABEL=BOOT /boot vfat defaults 0 2
#   LABEL=EASYROMS /roms exfat defaults,auto,umask=000,... 0 0
#
# and firstboot installs it over /etc/fstab as its last act, so from the second
# boot onwards this kernel is asked for both.  Neither entry carries nofail, so a
# filesystem the kernel does not know is not a missing ROMs directory: local-fs
# fails, and systemd takes a machine with no keyboard driver into emergency mode.
for symbol in \
    BLOCK BLK_DEV MMC MMC_BLOCK MMC_MTK REGULATOR REGULATOR_FIXED_VOLTAGE \
    EXT4_FS BTRFS_FS EXFAT_FS VFAT_FS; do
    config_y "$symbol"
done

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
# The two POSIX_ACL symbols matter because the rootfs is btrfs and systemd sets
# ACLs on the journal. FS_POSIX_ACL was already y; the per-filesystem ones were
# not, so the generic support was there with nothing able to use it.
for symbol in \
    NET UNIX INET NAMESPACES BTRFS_FS_POSIX_ACL EXT4_FS_POSIX_ACL; do
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
        DRM|DRM_LIMA|DRM_MEDIATEK) ;;
        DRM_*) config_n "$symbol" ;;
    esac
done < <(grep -E '^CONFIG_DRM_[A-Z0-9_]+=(y|m)$' "$CONFIG")
config_m DRM_LIMA
config_m MTK_MMSYS
config_m PHY_MTK_MIPI_DSI
config_m DRM_MEDIATEK
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
                EXT4_FS BTRFS_FS EXFAT_FS VFAT_FS NLS_UTF8 NLS_CODEPAGE_437 \
                WATCHDOG WATCHDOG_CORE MEDIATEK_WATCHDOG \
                NET UNIX INET SECCOMP_FILTER NAMESPACES NET_NS PID_NS \
                CGROUPS FHANDLE INOTIFY_USER SIGNALFD TIMERFD EPOLL \
                DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_XATTR TMPFS_POSIX_ACL \
                PROC_FS PROC_SYSCTL SYSFS BTRFS_FS_POSIX_ACL \
                DRM DEVMEM SOUND; do
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

log "Building the out-of-tree J36 modules: the input adapter, the panel and the AFE"
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
INIT_APPLETS=(sh mount umount mkdir mknod cat cp ln ls tr grep echo sleep dmesg
              insmod hexdump setsid cttyhack switch_root sync poweroff reboot
              uname chmod)

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
say() {
    echo "$@"
    if [ "$panel_is_console" = 0 ] && [ -c /dev/tty1 ]; then echo "$@" >/dev/tty1; fi
    return 0
}

# And the same for a file's contents.  `cat /proc/interrupts' goes to stdout
# only, which is the console, which is the serial port -- exactly the output that
# is invisible on a board being debugged from the panel.
show() {
    while IFS= read -r line; do say "  $line"; done < "$1"
    return 0
}

say ""
say "J36 Ultra ARMv7 bring-up initramfs"
say "Display: the LK's framebuffer on /dev/fb0 until something opens /dev/dri/card0."
insmod /lib/modules/*/extra/j36_mt6592_input.ko || say "input module load failed"

# ── Hand over to the rootfs on the card, if there is one ─────────────────────
#
# root= is treated as a hint and nothing more.  Partition numbering follows
# whichever MMC host attached first, and a card can be repartitioned between
# boots, so every candidate is proved by mounting it and looking for /sbin/init
# before the machine is handed to it.  Failing that we stay here with a shell,
# which is strictly better than the kernel panicking on a root= it cannot honour.
root_hint=""
want_doom=0
want_lima=0
want_mtkdrm=0
want_es=0
es_debug=0
want_audio=0
audio_speaker=0
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        j36.doom|j36.doom=1)
            want_doom=1
            ;;
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
        j36.es|j36.es=1)
            want_es=1
            ;;
        # Same payload, plus the three things that make a failed GL bring-up say
        # why: EmulationStation's --debug, Mesa's EGL loader trace, and one
        # attempt instead of six.  It is a separate word rather than a build
        # option because boot.conf is on the vfat partition, so it can be turned
        # off from any machine that can read the card.
        j36.es=debug)
            want_es=1
            es_debug=1
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

# btrfs first because that is what MixOS formats ROOTFS as, ext4 second because
# a hand-made card usually is not.  Mounted read-only to test, so a candidate
# that is not the root filesystem is never written to.
try_root() {
    dev="$1"
    if [ ! -b "$dev" ]; then return 1; fi
    for fs in btrfs ext4; do
        if ! mount -t "$fs" -o ro "$dev" /newroot 2>/dev/null; then continue; fi
        if [ -x /newroot/sbin/init ] || [ -L /newroot/sbin/init ]; then
            umount /newroot
            if mount -t "$fs" "$dev" /newroot; then
                say "root: $dev ($fs)"
                return 0
            fi
            return 1
        fi
        umount /newroot
    done
    return 1
}

find_root() {
    if [ -n "$root_hint" ] && try_root "$root_hint"; then
        rootdev="$root_hint"
        return 0
    fi
    for dev in /dev/mmcblk*p* /dev/mmcblk*; do
        if try_root "$dev"; then rootdev="$dev"; return 0; fi
    done
    return 1
}

# Waiting is this script's job.  `rootwait' on the command line is honoured by
# the kernel's own root mount, and rdinit= runs instead of that -- so a single
# scan here races the card: MSDC1 runs card identification on a workqueue, and
# an mmc host that is still deferred when /init starts has no block device yet.
rootdev=""
waited=0
while : ; do
    if find_root; then break; fi
    if [ "$waited" -ge 10 ]; then break; fi
    if [ "$waited" = 0 ]; then say "waiting for the microSD card"; fi
    waited=$((waited + 1))
    sleep 1
done

# ── Optional payloads on the FAT BOOT partition ──────────────────────────────
#
# Two of them now -- Doom and the Mali/lima bring-up -- and both are run here,
# after the wait loop and before the hand-over, for the same two reasons: the card
# is known to be up by now, and nothing of the rootfs is mounted yet, so systemd,
# journald and the RK3326 units are not competing for the panel.  Neither payload
# lives in this initramfs; see the fbdoom section of build-in-vm.sh for why.
#
# The partition is found the way try_root() finds the rootfs: by mounting
# candidates and looking inside, because partition numbering follows whichever
# MMC host attached first and this initramfs has no blkid.  Read-only, since
# nothing here writes to the card.  vfat gives every file mode 0755, so anything
# on it is executable straight off the mount.
#
# The probe looks for the j36 DIRECTORY and not for one file in it, so that a card
# carrying only the lima payload and a card carrying only Doom are found by the
# same code.  It runs at most once even when both are asked for.
bootfs_mounted=0
mount_bootfs() {
    if [ "$bootfs_mounted" = 1 ]; then return 0; fi
    mkdir -p /bootfs
    for dev in /dev/mmcblk*p*; do
        if [ ! -b "$dev" ]; then continue; fi
        if ! mount -t vfat -o ro "$dev" /bootfs 2>/dev/null; then continue; fi
        if [ -d /bootfs/j36 ]; then
            bootfs_mounted=1
            say "boot partition: $dev"
            return 0
        fi
        umount /bootfs
    done
    say "no FAT partition on this card carries a j36/ directory"
    return 1
}

# ── Doom, if the card carries it and the command line asks ───────────────────
#
# Doom's own stdout goes to the serial port when there is one: /dev/console is
# the panel, and the panel is in KD_GRAPHICS while Doom holds it, so anything
# printed there would be invisible anyway.  MENU quits, and then this script
# carries on with the boot exactly as if it had never run.
run_doom() {
    if [ ! -x /bootfs/j36/doom ]; then
        say "doom: j36.doom was asked for but j36/doom is not on the card"
        return 1
    fi
    wad=""
    for cand in freedoom1.wad freedoom2.wad doom.wad doom1.wad doom2.wad; do
        if [ -f "/bootfs/j36/$cand" ]; then wad="/bootfs/j36/$cand"; break; fi
    done
    if [ -z "$wad" ]; then
        say "doom: no IWAD in j36/ on the BOOT partition"
        return 1
    fi
    say "doom: $wad -- MENU quits and the boot continues"
    if [ -c /dev/ttyS0 ]; then
        HOME=/root /bootfs/j36/doom -iwad "$wad" >/dev/ttyS0 2>&1
    else
        HOME=/root /bootfs/j36/doom -iwad "$wad"
    fi
    say "doom: exited"
    return 0
}

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
    if [ ! -x /bootfs/j36/mfgpower ]; then
        say "lima: j36.lima was asked for but j36/mfgpower is not on the card"
        return 1
    fi
    if [ ! -f /bootfs/j36/modules/load.order ]; then
        say "lima: j36/modules/load.order is missing; nothing to load"
        return 1
    fi
    /bootfs/j36/mfgpower >/tmp/mfgpower.log 2>&1
    rc=$?
    show /tmp/mfgpower.log
    if [ "$rc" != 0 ]; then
        say "lima: mfgpower exited $rc; leaving the GPU alone"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        if insmod "/bootfs/j36/modules/$ko" >/tmp/insmod.log 2>&1; then
            say "lima: loaded $ko"
        else
            say "lima: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < /bootfs/j36/modules/load.order
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
    if [ ! -f /bootfs/j36/mtkdrm/load.order ]; then
        say "mtkdrm: j36.mtkdrm was asked for but j36/mtkdrm/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        if insmod "/bootfs/j36/mtkdrm/$ko" >/tmp/insmod.log 2>&1; then
            say "mtkdrm: loaded $ko"
        else
            say "mtkdrm: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < /bootfs/j36/mtkdrm/load.order
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
    if [ ! -f /bootfs/j36/audio/load.order ]; then
        say "audio: j36.audio was asked for but j36/audio/load.order is not on the card"
        return 1
    fi
    while IFS= read -r ko; do
        case "$ko" in ''|'#'*) continue ;; esac
        params=""
        if [ "$audio_speaker" = 1 ]; then
            case "$ko" in j36_mt6592_audio.ko) params="speaker=1" ;; esac
        fi
        if insmod "/bootfs/j36/audio/$ko" $params >/tmp/insmod.log 2>&1; then
            say "audio: loaded $ko $params"
        else
            say "audio: FAILED to load $ko"
            show /tmp/insmod.log
        fi
    done < /bootfs/j36/audio/load.order
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
setup_es_gl() {
    if [ ! -f /bootfs/j36/gl/links ]; then
        say "es: j36.es was asked for but j36/gl/ is not on the card"
        return 1
    fi
    if [ -z "$rootdev" ]; then
        say "es: no rootfs was found, so there is no systemd to configure"
        return 1
    fi

    if ! mount -t tmpfs -o mode=0755 tmpfs /newroot/run 2>/dev/null; then
        say "es: could not mount a tmpfs on the rootfs /run"
        return 1
    fi

    mkdir -p /newroot/run/j36/gl
    staged=0
    for so in /bootfs/j36/gl/*.so*; do
        [ -f "$so" ] || continue
        if cp "$so" /newroot/run/j36/gl/; then
            staged=$((staged + 1))
        else
            say "es: could not copy $so"
        fi
    done
    # The links file stands in for symlinks vfat cannot store: "name target", one
    # pair per line, targets relative to this directory except libEGL.so's, which
    # is a plain filename too.
    while read -r name target; do
        case "$name" in ''|'#'*) continue ;; esac
        ln -sf "$target" "/newroot/run/j36/gl/$name"
    done < /bootfs/j36/gl/links

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
    if [ -f /bootfs/j36/es/emulationstation ]; then
        mkdir -p /newroot/run/j36/es
        if cp /bootfs/j36/es/emulationstation /newroot/run/j36/es/emulationstation; then
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
    if [ -f /bootfs/j36/eglprobe ]; then
        if cp /bootfs/j36/eglprobe /newroot/run/j36/eglprobe; then
            chmod 0755 /newroot/run/j36/eglprobe
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
    missing=""
    if [ "$es_gles20" = 1 ]; then
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
    if [ "$es_debug" = 1 ]; then
        cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINDBG'

# j36.es=debug
Environment="EGL_LOG_LEVEL=debug"
Environment="MESA_DEBUG=1"
Environment="LIBGL_DEBUG=verbose"
Environment="J36_ES_GL_PROBE=1"
ExecStart=
ExecStart=/usr/bin/emulationstation/emulationstation.sh --debug
Restart=no
StandardOutput=journal+console
StandardError=journal+console
DROPINDBG

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
        # repeat after ES exits carries both.
        #
        # The third line is the one that answers the black panel, and it is last
        # so that its five colours are the last thing on the glass before ES
        # takes over: -p stops asking EGL questions and drives the scanout chain
        # itself, in five phases that remove ES, then SDL, then GL, then gbm from
        # the path.  The README section "j36/eglprobe -p, and the five colours"
        # has the verdicts.
        #
        # -+ and not -: the + runs it as root.  A modeset needs DRM master, and
        # SET_MASTER for a client that was never master is CAP_SYS_ADMIN, so as
        # User=ark this would have been fifteen seconds of EACCES.  Still
        # non-fatal, because a probe is not a precondition.
        if [ -x /newroot/run/j36/eglprobe ]; then
            cat >> /newroot/run/systemd/system/emulationstation.service.d/j36-gl.conf <<'DROPINPROBE'
ExecStartPre=-/bin/sh -c '/run/j36/eglprobe 2>&1 | tee /run/j36/eglprobe.log'
ExecStartPre=-/bin/sh -c 'LIBGL_ALWAYS_SOFTWARE=1 /run/j36/eglprobe -s 2>&1 | tee -a /run/j36/eglprobe.log'
ExecStartPre=-+/bin/sh -c '/run/j36/eglprobe -p 2>&1 | tee -a /run/j36/eglprobe.log'
ExecStopPost=-/bin/sh -c 'echo "--- eglprobe, repeated now that ES has exited ---"; cat /run/j36/eglprobe.log'
DROPINPROBE
        fi
    fi

    say "es: GL front end in /run/j36/gl, drop-in in /run/systemd/system"
    say "    $(ls /newroot/run/j36/gl | tr '\n' ' ')"
    return 0
}

if [ "$want_doom" = 1 ] || [ "$want_lima" = 1 ] || [ "$want_mtkdrm" = 1 ] || \
   [ "$want_es" = 1 ] || [ "$want_audio" = 1 ]; then
    if mount_bootfs; then
        # Doom first: it owns the panel while it runs, and the two driver payloads
        # leave modules loaded that have no reason to be disturbed by a game
        # exiting.  mtkdrm after those, so that if it does disturb the panel the
        # two things that were already proved have already run.  Audio next: it
        # touches nothing any of the others touch, and it has to be in place
        # before systemd looks for a controlC0 to restore into.  The GL front end
        # last, because it is the only one of the five that is not finished when
        # this script ends -- it is a message to systemd, and systemd has not
        # started yet.
        if [ "$want_doom" = 1 ]; then run_doom; fi
        if [ "$want_lima" = 1 ]; then run_lima; fi
        if [ "$want_mtkdrm" = 1 ]; then run_mtkdrm; fi
        if [ "$want_audio" = 1 ]; then run_audio; fi
        if [ "$want_es" = 1 ]; then setup_es_gl; fi
        umount /bootfs
    fi
fi

if [ -n "$rootdev" ]; then
    say "switching root into $rootdev"
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
# Assert it, because the kernel config above just grew MMC, btrfs and ext4 and
# nothing else in this build would notice the payload crossing into RECOVERY.
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
# WHERE IT LIVES, AND WHY NOT IN THE INITRAMFS.  The initramfs goes into both
# payloads, and boot.img is capped at the 9 MiB BOOTIMG slot asserted just above.
# A static ARM Doom is around 2 MiB and the IWAD is 24 MiB more, so putting
# either there would push the eMMC payload into RECOVERY.  Both go on the FAT
# BOOT partition instead, under j36/, where there is no size limit worth
# worrying about and no rule about what may be executed from a vfat mount: the
# default mount gives every file mode 0755.  /init mounts that partition, runs
# Doom, and carries on with the boot when it exits, so this costs the normal boot
# path nothing but a mount and an exec.
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

# Off by default.  fbdoom did its job -- it proved the panel, the pads and the
# framebuffer before there was a GPU stack to prove them with -- and everything it
# proved is now proved again, further along, by EmulationStation on mtk_drm + lima.
# Keeping it would put a game and a 35 MiB IWAD on the BOOT partition, which is for
# the boot payload, not for userland software.  J36_DOOM=1 brings it back verbatim.
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

    if [[ ! -f "$ARMHF_CHROOT/.j36-deps" ]]; then
        log "es: installing ${#ES_BUILD_DEPS[@]} build dependencies into the armhf chroot"
        armhf_chroot_run "eatmydata apt-get -y update" || return 1
        armhf_chroot_run "DEBIAN_FRONTEND=noninteractive eatmydata apt-get -y \
            --no-install-recommends install ${ES_BUILD_DEPS[*]}" || return 1
        sudo touch "$ARMHF_CHROOT/.j36-deps"
    fi
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

if [[ "${J36_ES:-1}" == 1 ]]; then
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
        log "gl: eglprobe was not built, see the error above -- j36.es=debug will just be quieter"
    fi

    # J36_ES_BUILD=0 skips the rebuild and leaves the rootfs's GLES1 binary in
    # place.  Worth having as its own switch because this is the one step in the
    # file measured in tens of minutes, and a boot that is only exercising the GL
    # payload or the kernel does not need it.
    if [[ "${J36_ES_BUILD:-1}" == 1 ]]; then
        set +e
        build_es_gles20
        es_rc=$?
        set -e
        if (( es_rc != 0 )); then
            ES_BIN=""
            armhf_chroot_teardown
            log "es: the GLES 2.0 binary was not built, see the error above -- the card"
            log "    will carry no j36/es and the rootfs's own EmulationStation will run,"
            log "    which on this board is the status 134"
        fi
    else
        log "es: J36_ES_BUILD=0, the rootfs's own EmulationStation will run"
    fi
else
    log "gl: J36_ES=0, skipping the GL front end"
fi

# ── The SD BOOT payload ───────────────────────────────────────────────────────
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
log "Staging the SD card BOOT payload"
SDBOOT="$ARTIFACTS/sd-boot"
rm -rf "$SDBOOT"
mkdir -p "$SDBOOT/mvii"
cp "$ZIMAGE" "$SDBOOT/zImage"
cp "$DTB_OUT/mt6592-j36-ultra.dtb" "$SDBOOT/"
cp "$ARTIFACTS/initramfs-j36-ultra.cpio.gz" "$SDBOOT/initrd.img"

# j36/ is read by /init, not by the LK, so nothing here goes through a load
# window and nothing here has a size limit.  Delete the directory on the card and
# the boot is exactly what it was before -- /init says so and carries on.
if [[ -n "$DOOM_BIN" ]]; then
    mkdir -p "$SDBOOT/j36"
    cp "$DOOM_BIN" "$SDBOOT/j36/doom"
    chmod 0755 "$SDBOOT/j36/doom"
    if [[ -n "$DOOM_WAD" ]]; then
        cp "$DOOM_WAD" "$SDBOOT/j36/$(basename "$DOOM_WAD")"
    fi
fi

# The same rule for the GPU payload, and the same consequence: remove
# j36/mfgpower or j36/modules and j36.lima=1 finds nothing, says so, and the boot
# continues.  load.order is written from the walk above, one module per line in
# the order insmod needs them.
if [[ -n "$MFGPOWER_BIN" && ${#LIMA_MODULE_ORDER[@]} -gt 0 ]]; then
    mkdir -p "$SDBOOT/j36/modules"
    cp "$MFGPOWER_BIN" "$SDBOOT/j36/mfgpower"
    chmod 0755 "$SDBOOT/j36/mfgpower"
    : > "$SDBOOT/j36/modules/load.order"
    for i in "${!LIMA_MODULE_ORDER[@]}"; do
        cp "${LIMA_MODULE_PATHS[$i]}" "$SDBOOT/j36/modules/${LIMA_MODULE_ORDER[$i]}"
        printf '%s\n' "${LIMA_MODULE_ORDER[$i]}" >> "$SDBOOT/j36/modules/load.order"
    done
    log "lima: staged ${#LIMA_MODULE_ORDER[@]} modules and mfgpower into j36/"
fi

# j36/mtkdrm/ is the display set, in its own directory and with its own load.order
# rather than merged into j36/modules/, because the two payloads answer to different
# command-line words and fail independently.  Deleting this one directory takes the
# whole mtk_drm experiment off the card and leaves the lima payload exactly as it
# was; j36.mtkdrm=1 then finds nothing, says so, and the boot carries on.
if (( ${#MTKDRM_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$SDBOOT/j36/mtkdrm"
    : > "$SDBOOT/j36/mtkdrm/load.order"
    for i in "${!MTKDRM_MODULE_ORDER[@]}"; do
        cp "${MTKDRM_MODULE_PATHS[$i]}" "$SDBOOT/j36/mtkdrm/${MTKDRM_MODULE_ORDER[$i]}"
        printf '%s\n' "${MTKDRM_MODULE_ORDER[$i]}" >> "$SDBOOT/j36/mtkdrm/load.order"
    done
    log "mtkdrm: staged ${#MTKDRM_MODULE_ORDER[@]} modules into j36/mtkdrm/"
fi

# j36/audio/ is the ALSA core and the AFE adapter, on the same terms: its own
# directory, its own load.order, its own command-line word, and deleting it takes
# the sound experiment off the card without touching anything else.  That matters
# more here than for the other payloads, because this is the one whose failure
# mode is the board switching off: j36.audio=speaker powers a class-D amp on VBAT,
# which is the system node, and a board with no cell fitted cannot hold it up.
# Removing this directory is the recovery, from any machine that reads SD cards.
if (( ${#AUDIO_MODULE_ORDER[@]} > 0 )); then
    mkdir -p "$SDBOOT/j36/audio"
    : > "$SDBOOT/j36/audio/load.order"
    for i in "${!AUDIO_MODULE_ORDER[@]}"; do
        cp "${AUDIO_MODULE_PATHS[$i]}" "$SDBOOT/j36/audio/${AUDIO_MODULE_ORDER[$i]}"
        printf '%s\n' "${AUDIO_MODULE_ORDER[$i]}" >> "$SDBOOT/j36/audio/load.order"
    done
    log "audio: staged ${#AUDIO_MODULE_ORDER[@]} modules into j36/audio/"
fi

# j36/gl/ is the GL front end plus the links file that stands in for the symlinks
# vfat cannot hold. Same removal contract as the other three: delete the directory
# and j36.es=1 finds nothing, says so, and EmulationStation starts against the
# rootfs's own libraries exactly as it does today -- which is to say against the
# RK3326 blob, and fails, which is the behaviour this replaces.
if [[ -n "$GL_PAYLOAD" ]]; then
    mkdir -p "$SDBOOT/j36/gl"
    cp "$GL_PAYLOAD"/*.so* "$SDBOOT/j36/gl/"
    cp "$GL_PAYLOAD/links" "$SDBOOT/j36/gl/links"
    log "gl: staged $(ls -1 "$SDBOOT/j36/gl" | wc -l) files into j36/gl/"
fi

# The probe sits beside the payload rather than inside j36/gl/, because that
# directory is copied wholesale into the loader's search path and a binary is not
# a library. j36.es=debug runs it; j36.es=1 never touches it. Deleting it is the
# same contract as the rest: the debug boot simply loses this report.
if [[ -n "$ESPROBE_BIN" ]]; then
    cp "$ESPROBE_BIN" "$SDBOOT/j36/eglprobe"
    chmod 0755 "$SDBOOT/j36/eglprobe"
    log "gl: staged j36/eglprobe ($(stat -c %s "$SDBOOT/j36/eglprobe") bytes)"
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
    mkdir -p "$SDBOOT/j36/es"
    cp "$ES_BIN" "$SDBOOT/j36/es/emulationstation"
    chmod 0755 "$SDBOOT/j36/es/emulationstation"
    log "es: staged j36/es/emulationstation ($(stat -c %s "$SDBOOT/j36/es/emulationstation") bytes)"
fi

# rdinit=/init stays even though root= is now present, and the two do not
# conflict: rdinit means the kernel never mounts a root filesystem itself, so a
# root= it could not honour can no longer panic it.  /init does the mounting, and
# treats root= as a hint it verifies before switching -- delete the root= below
# and the card boots to the initramfs shell exactly as it did before.
#
# The prose that used to explain each bootargs word lives in README.txt now.  The
# LK reads boot.conf into a fixed 2 KiB buffer, comments included, and the file
# had grown to 2003 bytes of it: 45 bytes from being silently truncated mid-line
# by the next sentence anybody added.
cat > "$SDBOOT/mvii/boot.conf" <<'CONF'
# MVII LK SD hand-off, J36 Ultra (MT6592, ARMv7).
#
# Read after the card's own boot.ini, so these override it: an R36S boot.ini
# names the RK3326 arm64 kernel, which this SoC cannot execute.  Keep this file
# short -- the LK reads it into a fixed 2 KiB buffer.  ../README.txt is the long
# form and explains every word below.
kernel=zImage
dtb=mt6592-j36-ultra.dtb
initrd=initrd.img

# root= is a hint /init verifies, not an order to the kernel.  console=tty0 comes
# last so /dev/console is the panel, not a serial port with nothing plugged in.
# The two masked units are RK3326-only: firstboot has no tars in a GUI-mode build,
# and batt_led restarts forever on hardware this kernel does not describe.
#
# Every j36 word is removable on its own: delete one, or the matching directory
# under j36/, and the boot carries straight on.  lima gives a render node, mtkdrm
# gives /dev/dri/card0, es points EmulationStation at Mesa instead of the RK3326
# blob, audio gives the ALSA core and a sound card.  j36.doom=1 is no longer built.
#
# j36.audio=speaker rather than =1 also powers the class-D amp: a second and
# deliberate step, and it needs a cell fitted, because the amp hangs off VBAT --
# the system node -- and battery-less it pulls the board under its own lockout.
#
# j36.es=debug adds ES's --debug, Mesa's EGL trace and one start attempt instead
# of six.  Change it to j36.es=1, here, from any machine, once ES draws.
bootargs=console=ttyS0,115200n8 console=tty0 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init root=/dev/mmcblk0p2 rw rootwait systemd.mask=firstboot.service systemd.mask=batt_led.service systemd.journald.forward_to_console=1 j36.lima=1 j36.mtkdrm=1 j36.es=debug j36.audio=1
CONF

# The LK reads boot.conf into a fixed 2 KiB buffer and a longer file is silently
# truncated mid-line.
(( $(stat -c %s "$SDBOOT/mvii/boot.conf") <= 2048 )) || \
    die "boot.conf exceeds the LK's 2048-byte read buffer"
verify_armv7_kernel "$SDBOOT/zImage" "the SD payload kernel"

cat > "$SDBOOT/README.txt" <<'README'
J36 Ultra (MT6592, ARMv7) SD card BOOT payload.

Copy the contents of this directory into the root of the FAT partition labelled
BOOT.  Existing files are not disturbed: an R36S card keeps its Image,
uInitrd, rk3326 device trees and boot.ini, and mvii/boot.conf points the MVII LK
at the ARMv7 payload instead.

  zImage                    plain 32-bit ARM kernel, no appended device tree
  mt6592-j36-ultra.dtb      the tree the LK loads separately and patches
  initrd.img                bring-up initramfs (busybox + the input module)
  mvii/boot.conf            filenames and command line for the MVII LK
  j36/mfgpower              powers the Mali-450 and reads its ID back; the gate
  j36/modules/              lima and its dependencies, plus load.order
  j36/mtkdrm/               the MT6592 display driver set, plus load.order
  j36/audio/                the ALSA core and the MT6592 AFE driver, plus load.order
  j36/gl/                   Mesa's GL front end, plus links (vfat has no symlinks)
  j36/eglprobe              what can create a GL context, and why not, and with
                            -p whether a frame reaches the glass; j36.es=debug
  j36/es/emulationstation   the same EmulationStation with a GLES 2.0 renderer,
                            bind-mounted over the rootfs's fixed-function one
  LICENSE.txt               which licence covers which file above, and where the
                            GPL-2.0-only source is; keep it with the payload

The R36S kernel on the same card is arm64 and stays there for the R36S.  The
armhf Debian rootfs is shared, and this kernel can now mount it: MSDC1, the
microSD host, is driven by mtk-sd through a mediatek,mt6592-mmc node, and btrfs
and ext4 are both built in.  /init verifies a candidate partition by mounting it
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
    carry the tars; it converts EASYROMS to exfat and grows ROOTFS to fill the
    card, and this kernel now has exfat and vfat built in for the result.

systemd.mask=batt_led.service
    The RK3326 battery LED daemon, and the first unit the forwarded log caught:

      batt_life_warning.py[829]: FileNotFoundError: [Errno 2] No such file or
        directory: '/sys/class/power_supply/battery/capacity'
      batt_led.service: Scheduled restart job, restart counter is at 21.

    It reads that capacity file and writes /sys/class/gpio/gpio77/value, and this
    kernel has neither: no power_supply driver for the MT6592 PMIC, and no sysfs
    GPIO export.  Its unit is Restart=always, RestartSec=2 and
    StartLimitIntervalSec=0 -- explicitly unbounded -- so it is a Python
    traceback on the console every 2.3 s for as long as the machine is up.
    Fitting a cell does not change it; the file is missing because the driver is,
    and the fix is a power_supply for the MT6592 PMIC, which is also what would
    make the LED daemon work unmodified.

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
    Does nothing on this card: j36/doom and its IWAD are not built and not staged
    any more.  They proved the panel and the pad before there was a GPU stack to
    prove them with, and EmulationStation on mtk_drm + lima now proves the same
    thing further along -- so the BOOT partition carries the boot payload and not
    a game.  /init still reads the word and still runs j36/doom if it is there, so
    rebuilding with J36_DOOM=1 (or copying a static doomgeneric and any IWAD
    d_iwad.c knows into j36/ by hand) brings it back unchanged.  It is still the
    quickest thing to reach for when the GL path breaks: Doom needs no DRM, no GL
    and no rootfs, so if Doom draws and ES does not, the panel is fine and the
    fault is above it.

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

j36.es=1
    Point EmulationStation at Mesa instead of the RK3326's Mali blob, by staging
    j36/gl/ into a tmpfs and writing a systemd drop-in into another one.  Nothing
    on the shared rootfs is written -- see below -- so this word is the whole
    difference between an ES that cannot start and one that can.  It needs
    j36.lima=1 and j36.mtkdrm=1 to be any use: without them there is no render node
    and no card node for Mesa to open.

    It also mounts j36/es/emulationstation over the rootfs's own, if the card
    carries one, and that is the second half of the fix.  The rootfs's binary was
    compiled with the fixed-function renderer, and GLES1 is the one API this stack
    cannot supply: Debian's armhf Mesa 25.0.7 is a -Dgles1=disabled build, so an ES1
    context is 0x3003 EGL_BAD_ALLOC on lima, on llvmpipe and on softpipe alike.
    Renderer_GLES10.cpp then reads glGetString(GL_EXTENSIONS) without having checked
    SDL_GL_CreateContext, so a context that was never created arrives as
    std::string(NULL) and aborts with status 134.  The binary in j36/es/ is the same
    upstream commit with a GLES 2.0 renderer instead, and ES2 is what lima does
    give: "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1".

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

j36/eglprobe -p, and the five colours
-------------------------------------

So that is measured instead of argued.  Everything in the probe's other modes asks
whether a context can be built; -p paints, and it takes ES, then SDL, then GL, then
gbm out of the path one step at a time.  It runs under j36.es=debug as the last
thing before ES starts, holding each frame three seconds because the instrument for
this one is an eye.  It speaks DRM with raw ioctls -- the uapi structs are ABI and
libdrm would be a fourth library that can be missing -- and prints the connector,
the mode it was given, the CRTC and whatever framebuffer was already on it.

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

`/run/j36/eglprobe -p' by hand does the same thing from a console at any time; it
takes DRM master for the fifteen seconds it runs and gives it back, and the console
framebuffer comes back with it.

Add systemd.mask=emulationstation.service to bootargs to get the machine back to a
console.

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
MixOS -- J36 Ultra (MediaTek MT6592, ARMv7) SD BOOT payload
Copyright (c) 2025-2026 the MixOS project and contributors

MixOS supports the MediaTek line of processors.  This card is that support: a
32-bit ARM kernel, an mtk_drm display path, mtk-sd storage, the MT6592 AFE, a
keypad adapter and Mesa's lima/kmsro pair, on a Cortex-A7 from 2013.

This payload is not licensed uniformly.  Saying otherwise would be a false
statement about other people's code.

Microsoft Public License (Ms-PL), in full below:

    j36/eglprobe            the EGL/GBM/DRM scanout probe
    j36/mfgpower            the MFG power-domain bring-up probe
    mvii/boot.conf          the MVII LK hand-off
    README.txt, this file   the documentation

GNU General Public License, version 2 only:

    zImage                  Linux 6.12 LTS, plus two MixOS patches (mtk-sd and
                            drm/mediatek for MT6592)
    initrd.img              busybox and a shell /init
    j36/modules/*.ko        lima and its dependencies -- kernel modules
    j36/mtkdrm/*.ko         mtk_drm, and MixOS's j36_jd9365_panel
    j36/audio/*.ko          the ALSA core, and MixOS's j36_mt6592_audio
    j36_mt6592_input.ko     MixOS's keypad and GPIO key adapter

    The three MixOS modules are GPL-2.0-only deliberately and are not Ms-PL: they
    derive from and link against GPL-2.0-only kernel internals, and Ms-PL is not
    GPL-compatible.

Their own terms:

    j36/gl/*.so*            Mesa, from Debian (MIT and others)
    j36/es/emulationstation EmulationStation-fcamod, under its own licence, with a
                            third renderer added by MixOS that follows it
    the rootfs on ROOTFS    Debian.  Per-package terms are in
                            /usr/share/doc/*/copyright on the running device.

SOURCE.  Everything GPL-2.0-only here is built from public source: Linux 6.12 LTS
from kernel.org with the two patches in the MixOS repository under
device/j36-ultra/linux/, the three MixOS modules in the same directory, and
busybox, Mesa and EmulationStation as Debian packages them.  The MixOS build
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

(
    cd "$ARTIFACTS"
    # The Doom payload is optional, and sha256sum takes a missing operand as an
    # error, so it is named only when it was staged.
    sums=(boot.img zImage zImage-j36-ultra mt6592-j36-ultra.dtb
          j36_mt6592_input.ko initramfs-j36-ultra.cpio.gz
          sd-boot/zImage sd-boot/mvii/boot.conf)
    if [[ -f sd-boot/j36/doom ]]; then
        sums+=(sd-boot/j36/doom)
    fi
    if [[ -f sd-boot/j36/mfgpower ]]; then
        sums+=(sd-boot/j36/mfgpower sd-boot/j36/modules/load.order)
        # Named individually rather than by glob, so that a module that failed to
        # copy is a missing line here instead of a silently shorter list.
        while IFS= read -r ko; do
            sums+=("sd-boot/j36/modules/$ko")
        done < sd-boot/j36/modules/load.order
    fi
    if [[ -f sd-boot/j36/mtkdrm/load.order ]]; then
        sums+=(sd-boot/j36/mtkdrm/load.order)
        while IFS= read -r ko; do
            sums+=("sd-boot/j36/mtkdrm/$ko")
        done < sd-boot/j36/mtkdrm/load.order
    fi
    if [[ -f sd-boot/j36/audio/load.order ]]; then
        sums+=(sd-boot/j36/audio/load.order)
        while IFS= read -r ko; do
            sums+=("sd-boot/j36/audio/$ko")
        done < sd-boot/j36/audio/load.order
    fi
    if [[ -f sd-boot/j36/gl/links ]]; then
        sums+=(sd-boot/j36/gl/links)
        # Glob here and not a manifest walk, because links names the SONAMEs and
        # not the files: the same library is one file and one or two names.
        for gl in sd-boot/j36/gl/*.so*; do
            [[ -f "$gl" ]] && sums+=("$gl")
        done
    fi
    # An if, not a && -- this runs under set -e and a card built without the probe
    # would make the AND-list fail and take the manifest with it.
    if [[ -f sd-boot/j36/eglprobe ]]; then
        sums+=(sd-boot/j36/eglprobe)
    fi
    if [[ -f sd-boot/j36/es/emulationstation ]]; then
        sums+=(sd-boot/j36/es/emulationstation)
    fi
    sha256sum "${sums[@]}" > SHA256SUMS
    {
        echo "licence=Ms-PL for the MixOS bring-up work, GPL-2.0-only for the kernel and the three MixOS modules, per-payload in sd-boot/LICENSE.txt"
        echo "kernel_branch=$KERNEL_BRANCH"
        echo "kernel_release=$KERNEL_RELEASE"
        echo "kernel_arch=arm (ARMv7, 32-bit)"
        echo "zimage_size=$(stat -c %s zImage)"
        echo "dtb_sha256=$(sha256sum mt6592-j36-ultra.dtb | awk '{print $1}')"
        echo "bootimg_size=$(stat -c %s boot.img) (slot 0x900000)"
        echo "storage=msdc1 mtk-sd mediatek,mt6592-mmc (btrfs, ext4, exfat, vfat)"
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
        if [[ -f sd-boot/j36/doom ]]; then
            echo "fbdoom=sd-boot/j36/doom ($(stat -c %s sd-boot/j36/doom) bytes, static ARMv7, 640x400 in 640x480)"
            echo "fbdoom_commit=$DOOM_COMMIT"
            if [[ -n "$DOOM_WAD" ]]; then
                echo "fbdoom_iwad=j36/$(basename "$DOOM_WAD")"
            else
                echo "fbdoom_iwad=none (drop one into j36/ on the card)"
            fi
            echo "fbdoom_start=j36.doom=1 on the command line, /init runs it before switch_root"
        else
            echo "fbdoom=not staged"
        fi
        if [[ -f sd-boot/j36/mfgpower ]]; then
            echo "gpu=mali-450 mp4 at 0x13040000 (gp irq 234..pp_bcast 244, GIC_SPI 202..212)"
            echo "gpu_driver=lima, CONFIG_DRM_LIMA=m ($(tr '\n' ' ' < sd-boot/j36/modules/load.order))"
            echo "gpu_power=j36/mfgpower ($(stat -c %s sd-boot/j36/mfgpower) bytes, static ARMv7, SPM MTCMOS via /dev/mem)"
            echo "gpu_start=j36.lima=1 on the command line; /init loads the modules only if mfgpower exits 0"
            echo "gpu_nodes=renderD128 from lima; card0 comes from mtk_drm, not from lima"
        else
            echo "gpu=not staged"
        fi
        if [[ -f sd-boot/j36/mtkdrm/load.order ]]; then
            echo "display_drm=mediatek-drm, mt6592 via mt2701 fallback compatibles"
            echo "display_ddp=ovl0 0x14007000 -> rdma0 0x14008000 -> color0 0x1400b000 -> dsi0 0x1400c000"
            echo "display_mutex=mod 0x488, sof 1 (matches the LK register for register)"
            echo "display_phy=mediatek,mt2701-mipi-tx (mppll_preserve 3, as the LK writes)"
            echo "display_data_rate=192 MHz from the pixel clock; the LK programmed 214"
            echo "display_modules=$(tr '\n' ' ' < sd-boot/j36/mtkdrm/load.order)"
            echo "display_start=j36.mtkdrm=1; no register is programmed until card0 is opened"
            echo "display_fbdev=CONFIG_DRM_FBDEV_EMULATION=n, so /dev/fb0 stays simplefb's"
        else
            echo "display_drm=not staged"
        fi
        if [[ -f sd-boot/j36/audio/load.order ]]; then
            echo "audio=mt6592 afe at 0x11220000, dl1 memif -> i2s dac -> mt6323 abb"
            echo "audio_driver=j36_mt6592_audio.ko, native ALSA (no MT6592 ASoC driver exists upstream)"
            echo "audio_reference=PowerEngine OS/MVII/.../mt6592_audio.c, from the MT6592 HAL"
            echo "audio_pcm=1 playback, S16_LE stereo 8k-48k, 64 KiB ring, cursor polled at 5 ms, no IRQ"
            echo "audio_modules=$(tr '\n' ' ' < sd-boot/j36/audio/load.order)"
            echo "audio_core=CONFIG_SOUND=y (soundcore only); snd, snd-timer, snd-pcm are =m and staged here"
            echo "audio_snd_pcm=selected by SND_DUMMY=m, which is built and deliberately not staged"
            echo "audio_start=$(grep -o 'j36\.audio=[a-z0-9]*' sd-boot/mvii/boot.conf)"
            echo "audio_clock=UNPROVEN: this is the first ungate of AFE_CG on this board; dmesg says whether DL1_CUR advances"
            echo "audio_speaker=off unless j36.audio=speaker, and then only after the cursor moves"
            echo "audio_speaker_hazard=class-D amp on VBAT, which is the system node; battery-less it trips the PMIC UVLO"
        else
            echo "audio=not staged"
        fi
        if [[ -f sd-boot/j36/gl/links ]]; then
            echo "gl=debian armhf mesa 25.0.7 from the shared rootfs (lima_dri.so + mediatek_dri.so)"
            echo "gl_front_end=$(ls sd-boot/j36/gl/*.so* | xargs -n1 basename | tr '\n' ' ')"
            echo "gl_reason=the shared rootfs points libEGL.so, libgbm.so{,.1,.1.0.0} and libGLESv1_CM.so at the RK3326 Mali blob"
            echo "gl_load_bearing=libgbm.so.1 -- libEGL_mesa.so.0 needs it, so mesa's own EGL cannot load without this payload"
            echo "gl_install=tmpfs on the rootfs /run plus a systemd drop-in; nothing is written to the card"
            echo "emulationstation=default; j36.es=1 supplies the GL front end"
            echo "es_boot_word=$(grep -o 'j36\.es=[a-z0-9]*' sd-boot/mvii/boot.conf)"
            if [[ -f sd-boot/j36/es/emulationstation ]]; then
                echo "es_binary=j36/es/emulationstation ($(stat -c %s sd-boot/j36/es/emulationstation) bytes, stripped ARMv7)"
                echo "es_commit=$ES_COMMIT (the rootfs's own EmulationStation commit)"
                echo "es_renderer=USE_OPENGLES_20, es/Renderer_GLES20.cpp, no GL library in DT_NEEDED"
                echo "es_renderer_reason=ES1 is 0x3003 EGL_BAD_ALLOC everywhere (mesa built -Dgles1=disabled); ES2 is ctx/cur on lima"
                echo "es_install=bind mount over /usr/bin/emulationstation/emulationstation from a tmpfs; the rootfs file is untouched"
                echo "es_gl_driver=SDL_VIDEO_GL_DRIVER=libGLESv2.so.2, no LD_PRELOAD"
            else
                echo "es_binary=not staged; the rootfs's fixed-function one runs and aborts with status 134"
                echo "es_gl_driver=SDL_VIDEO_GL_DRIVER=libGL.so.1 with LD_PRELOAD=libGL.so.1 (the fallback)"
            fi
            if [[ -f sd-boot/j36/eglprobe ]]; then
                echo "es_probe=j36/eglprobe ($(stat -c %s sd-boot/j36/eglprobe) bytes, dynamic ARMv7, dlopens libEGL.so.1 and libgbm.so.1)"
                echo "es_probe_run=j36.es=debug only, as ExecStartPre and again as ExecStopPost; card0 and renderD128 separately, then -s with LIBGL_ALWAYS_SOFTWARE=1 as the control"
                echo "es_probe_paint=-p as a third ExecStartPre, run as root for DRM master: five 3s frames -- XR24 red, AR24 opaque magenta, AR24 transparent magenta, all three CPU-filled, then two lima frames through gbm"
            else
                echo "es_probe=not staged; j36.es=debug will report only what Mesa says"
            fi
        else
            echo "gl=not staged"
            echo "emulationstation=enabled but will fail; without j36/gl it runs against the RK3326 blob"
        fi
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
