#!/usr/bin/env bash
# Incremental J36 Ultra ARMv7 bring-up builder. Run inside Ubuntu, normally via
# ./build-j36-ultra.sh on macOS. It never invokes the RG351MP/R36 build target.

set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
WORK="${J36_WORK_DIR:-$HOME/j36-ultra-work}"
# The MVII board sources are vendored in this checkout, so nothing here reaches
# outside it. They used to be rsynced into the VM from a PowerEngine tree on the
# host, which made a dArkOS build depend on a sibling repository being present.
DRIVERS="${J36_DRIVERS_DIR:-$ROOT/device/j36-ultra/mvii-board}"
EXPORT_DIR="${J36_EXPORT_DIR:-$WORK/export}"
KERNEL_URL="${J36_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
KERNEL_BRANCH="${J36_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_SRC="$WORK/linux"
KERNEL_OUT="$WORK/kernel-build"
BUSYBOX_URL="${J36_BUSYBOX_URL:-https://git.busybox.net/busybox}"
BUSYBOX_BRANCH="${J36_BUSYBOX_BRANCH:-1_36_stable}"
BUSYBOX_SRC="$WORK/busybox"
# fbdoom: the first thing that draws a moving picture on this panel.  Pinned to a
# commit rather than a branch because the build recipe below derives its source
# list from the layout of that tree -- see the fbdoom section for why.
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
# Audio and a native DSI/display driver are added only after the serial/fb/input
# bring-up is proven. Storage is no longer in that list, and neither is NET --
# see below -- and DRM has now left it too, for the GPU section further down.
#
# WIRELESS, WLAN and BT stay off, and they are disabled here rather than left
# out: all three live inside `if NET' in net/Kconfig and WIRELESS defaults to y,
# so once NET is on they would come back by default. Both the =y for NET and the
# "is not set" lines for these are in .config before the single olddefconfig
# below, which is what makes the explicit n stick.
for symbol in \
    SOUND SND MEDIA_SUPPORT WIRELESS WLAN BT USB_SUPPORT SCSI ATA \
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
# BTRFS because the rootfs this card already carries is btrfs: dArkOS's own
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
# MODULE_COMPRESS_NONE is in the list for the lima payload: /init loads modules by
# filename with busybox insmod, which decompresses nothing, and turning module
# compression on would rename every .ko to .ko.xz and leave load.order naming
# files that are not there. DEVMEM is what mfgpower reaches the SPM through.
for required in MACH_MT6592 ARM_APPENDED_DTB ARM_ATAG_DTB_COMPAT \
                FB_SIMPLE SERIAL_8250_MT6577 BLK_DEV_INITRD MODULES \
                MMC MMC_BLOCK MMC_MTK REGULATOR_FIXED_VOLTAGE \
                EXT4_FS BTRFS_FS EXFAT_FS VFAT_FS NLS_UTF8 NLS_CODEPAGE_437 \
                WATCHDOG WATCHDOG_CORE MEDIATEK_WATCHDOG \
                NET UNIX INET SECCOMP_FILTER NAMESPACES NET_NS PID_NS \
                CGROUPS FHANDLE INOTIFY_USER SIGNALFD TIMERFD EPOLL \
                DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_XATTR TMPFS_POSIX_ACL \
                PROC_FS PROC_SYSCTL SYSFS BTRFS_FS_POSIX_ACL \
                DRM DEVMEM MODULE_COMPRESS_NONE; do
    grep -q "^CONFIG_${required}=y$" "$CONFIG" || \
        die "required kernel option CONFIG_${required}=y was not selected"
done

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

log "Building the out-of-tree J36 modules: the input adapter and the panel"
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

    # And the applets /init actually invokes. defconfig has all of them today;
    # asserting it turns a future defconfig change into a build failure here
    # instead of an init that dies on the device with `sh: not found'.
    for sym in CONFIG_ASH CONFIG_SH_IS_ASH CONFIG_MOUNT CONFIG_MKDIR CONFIG_MKNOD \
               CONFIG_CAT CONFIG_ECHO CONFIG_SLEEP CONFIG_DMESG CONFIG_INSMOD \
               CONFIG_LS CONFIG_HEXDUMP CONFIG_SETSID CONFIG_CTTYHACK CONFIG_HALT \
               CONFIG_UNAME CONFIG_SWITCH_ROOT CONFIG_UMOUNT CONFIG_SYNC; do
        grep -q "^$sym=y\$" "$BUSYBOX_SRC/.config" || \
            die "busybox $sym is off; /init uses that applet"
    done

    make -C "$BUSYBOX_SRC" CROSS_COMPILE=arm-linux-gnueabihf- -j"$(nproc)"
fi
BUSYBOX="$BUSYBOX_SRC/busybox"
[[ -x "$BUSYBOX" ]] || die "static ARM BusyBox was not produced"
# The kernel and the module are checked for their machine type; this was not, and
# it is the one binary in the initramfs that the SoC executes first. A busybox
# built for the host is a perfectly valid ELF that this board cannot run, and the
# symptom on the device would be an unhelpful `/init: not found' at hand-over.
verify_arm_elf "$BUSYBOX" "the initramfs BusyBox"

rm -rf "$INITROOT"
mkdir -p "$INITROOT"/{bin,dev,etc,lib/modules/$KERNEL_RELEASE/extra,proc,root,sbin,sys,tmp}
cp "$BUSYBOX" "$INITROOT/bin/busybox"
chmod 0755 "$INITROOT/bin/busybox"
for applet in sh mount umount mkdir mknod cat echo sleep dmesg insmod ls hexdump \
              setsid cttyhack switch_root sync poweroff reboot uname; do
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
say "Display: the LK's framebuffer on /dev/fb0. mtk_drm loads only with j36.mtkdrm=1."
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
for arg in $(cat /proc/cmdline); do
    case "$arg" in
        j36.doom|j36.doom=1)
            want_doom=1
            ;;
        j36.lima|j36.lima=1)
            want_lima=1
            ;;
        j36.mtkdrm|j36.mtkdrm=1)
            want_mtkdrm=1
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

# btrfs first because that is what dArkOS formats ROOTFS as, ext4 second because
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

if [ "$want_doom" = 1 ] || [ "$want_lima" = 1 ] || [ "$want_mtkdrm" = 1 ]; then
    if mount_bootfs; then
        # Doom first: it owns the panel while it runs, and the two driver payloads
        # leave modules loaded that have no reason to be disturbed by a game
        # exiting.  mtkdrm last of the three, so that if it does disturb the panel
        # the two things that were already proved have already run.
        if [ "$want_doom" = 1 ]; then run_doom; fi
        if [ "$want_lima" = 1 ]; then run_lima; fi
        if [ "$want_mtkdrm" = 1 ]; then run_mtkdrm; fi
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

if [[ "${J36_DOOM:-1}" == 1 ]]; then
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
# initramfs either.  It goes on the FAT BOOT partition beside Doom, with the
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
        collect_lima_modules
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
# Read after the card's own boot.ini, so these override it.  A dArkOS boot.ini
# names the RK3326 arm64 kernel, which this SoC cannot execute.  Keep this file
# short: the LK reads it into a fixed 2 KiB buffer.  ../README.txt is the long
# form, and explains every word of bootargs below.
kernel=zImage
dtb=mt6592-j36-ultra.dtb
initrd=initrd.img

# root= is a hint /init verifies, not an order to the kernel.  console=tty0 comes
# last so /dev/console is the panel and not a serial port with nothing plugged
# into it.  The two masked units are RK3326-only: firstboot is dArkOS's expansion
# script, which a GUI-mode build has no tars for, and batt_led is the battery LED
# daemon, which restarts forever on hardware this kernel does not describe.  Any
# of them can be deleted here.  j36.doom=1 runs j36/doom off this partition
# before the hand-over, as the panel and pad test; MENU quits it and the boot
# carries on.  j36.lima=1 powers the Mali-450 and loads the DRM driver, but only
# if j36/mfgpower reads the right GPU back.  Delete either word, or the j36
# directory, to boot straight through.
bootargs=console=ttyS0,115200n8 console=tty0 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init root=/dev/mmcblk0p2 rw rootwait systemd.mask=firstboot.service systemd.mask=batt_led.service systemd.journald.forward_to_console=1 j36.doom=1 j36.lima=1
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
  j36/doom                  framebuffer Doom, static ARMv7, run before hand-over
  j36/freedoom1.wad         the game data it loads (Freedoom, freely licensed)
  j36/mfgpower              powers the Mali-450 and reads its ID back; the gate
  j36/modules/              lima and its dependencies, plus load.order

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
    dArkOS's first-boot script is written for the RK3326 image and this
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

rdinit=/init root=/dev/mmcblk0p2 rw rootwait
    See above: /init does the mounting, so root= cannot panic the kernel.

j36.doom=1
    Run j36/doom off this partition after the card comes up and before the
    hand-over.  Delete the word, or the j36 directory, and the boot is exactly
    what it was: /init says so on the panel and carries on.

j36.lima=1
    Power the Mali-450 and load the DRM lima driver, in that order and only in
    that order -- see below.  Same removal story as j36.doom: delete the word, or
    j36/mfgpower, or j36/modules, and the GPU is never touched.

Doom, and what it is for
------------------------

It answers a question the boot itself does not: whether a program can drive this
panel and read this pad.  Nothing already on the card can ask it -- SDL2 has no
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

There is no sound: doomgeneric is built with sound compiled out, which is honest
about this kernel, since nothing drives the MT6592 audio path yet.

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

EmulationStation, and what is still missing
-------------------------------------------

ES on this card cannot start yet, and no combination of flags reaches it.  The
chain, each link measured rather than assumed:

  1. lima gives a render node and no KMS device, because a Mali-450 has no
     display controller.  The panel is driven by simplefb, which is a framebuffer
     and not a DRM device.
  2. SDL2's video backends are KMSDRM, X11, Wayland, offscreen and dummy.  There
     is no fbdev backend, so /dev/fb0 is of no use to it, and KMSDRM needs a card
     node -- the one thing step 1 does not produce.
  3. simpledrm could produce that node, and it is deliberately disabled: it binds
     the same `simple-framebuffer' the working display is on and evicts simplefb
     when it does.  Even enabled it would not help, because Mesa's renderonly
     driver table has no simpledrm entry, so there would be no GBM and no EGL to
     put on top of it.
  4. The GL stack in the shared rootfs is the RK3326's Mali-G31 Bifrost blob, and
     emulationstation.service forces it with SDL_VIDEO_EGL_DRIVER=libEGL.so.
     Bifrost is a different architecture from this Utgard part; that library
     cannot drive this GPU whatever else is true.

What closes it is a real MT6592 display driver -- a DRM/KMS device for the
DISP/OVL/DSI path, so there is a card0 for KMSDRM to open and something for
Mesa's lima driver to present through.  That is the next piece of work, not a
setting.  ES is left enabled and failing on purpose: its unit is Restart=on-failure
under the default 5-starts-in-10-s limit, so it gives up on its own and the log
says why.  Add systemd.mask=emulationstation.service to bootargs to silence it.

Rebooting
---------

`reboot' works from here on: the device tree describes the TOPRGU watchdog at
0x10007000, and mtk_wdt registers the restart handler that machine_restart()
calls.  Without that node userspace shuts down cleanly and then prints "Reboot
failed -- System halted", which is a halt, not a crash -- the card is safe to
pull at that point.  `poweroff' still ends the same way, because nothing drives
the PMIC yet; hold the power button instead.
README

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
    sha256sum "${sums[@]}" > SHA256SUMS
    {
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
            echo "gpu_nodes=renderD128 only; lima is render-only and there is no card0"
            echo "emulationstation=blocked on a KMS driver, not on configuration (see sd-boot/README.txt)"
        else
            echo "gpu=not staged"
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
