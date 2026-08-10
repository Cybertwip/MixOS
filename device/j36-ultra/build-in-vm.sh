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

# ── The one change this build makes to the kernel itself ──────────────────────
#
# mtk-sd carries no compatible for MT6592 and refuses to probe without pinctrl;
# linux/0001-mtk-sd-mt6592.patch fixes both, and its header records why neither
# can be worked around from the device tree instead.
#
# Applied idempotently rather than from a stamp file, because this checkout
# persists across runs and a stamp can outlive the tree it describes -- a
# J36_UPDATE_KERNEL reset above throws the patch away and would leave the stamp
# claiming otherwise.  `apply --reverse --check' succeeds only when the change is
# already present, so the tree is asked instead of a bookkeeping file.
MTKSD_PATCH="$ROOT/device/j36-ultra/linux/0001-mtk-sd-mt6592.patch"
[[ -f "$MTKSD_PATCH" ]] || die "missing kernel patch: $MTKSD_PATCH"
if git -C "$KERNEL_SRC" apply --reverse --check "$MTKSD_PATCH" 2>/dev/null; then
    log "The mtk-sd MT6592 patch is already applied"
elif git -C "$KERNEL_SRC" apply --check "$MTKSD_PATCH" 2>/dev/null; then
    log "Applying the mtk-sd MT6592 patch"
    git -C "$KERNEL_SRC" apply "$MTKSD_PATCH"
else
    die "0001-mtk-sd-mt6592.patch neither applies to nor is applied in $KERNEL_SRC; refresh it against $KERNEL_BRANCH"
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

# Keep this first-stage image below the fixed 9 MiB BOOTIMG partition.
# Audio and native DRM/DSI are added only after the serial/fb/input bring-up is
# proven. Storage is no longer in that list, and neither is NET -- see below.
#
# WIRELESS, WLAN and BT stay off, and they are disabled here rather than left
# out: all three live inside `if NET' in net/Kconfig and WIRELESS defaults to y,
# so once NET is on they would come back by default. Both the =y for NET and the
# "is not set" lines for these are in .config before the single olddefconfig
# below, which is what makes the explicit n stick.
for symbol in \
    DRM SOUND SND MEDIA_SUPPORT WIRELESS WLAN BT USB_SUPPORT SCSI ATA \
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
for symbol in \
    BLOCK BLK_DEV MMC MMC_BLOCK MMC_MTK REGULATOR REGULATOR_FIXED_VOLTAGE \
    EXT4_FS BTRFS_FS; do
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

make -C "$KERNEL_SRC" O="$KERNEL_OUT" ARCH=arm \
    CROSS_COMPILE=arm-linux-gnueabihf- olddefconfig

# olddefconfig re-derives every symbol from its dependencies, so it has the last
# word on all of the above; the checks come after it, not before. MMC_MTK is
# asserted =y and not =m on purpose: a module for the driver that mounts the
# filesystem the modules live on cannot be loaded.
#
# This list is the real fix for the systemd freeze, not the NET=y above. UNIX was
# already in the enable list and had been silently dropped for being under `if
# NET' while NET was in the disable list -- a config_y whose result nothing
# checked. So everything PID 1 cannot start without is asserted here, including
# the symbols that come along by dependency: if a future prune takes NET out
# again, or turns SECCOMP off, the build fails here instead of the board
# freezing at 15 s with the reason four screens up.
for required in MACH_MT6592 ARM_APPENDED_DTB ARM_ATAG_DTB_COMPAT \
                FB_SIMPLE SERIAL_8250_MT6577 BLK_DEV_INITRD MODULES \
                MMC MMC_BLOCK MMC_MTK REGULATOR_FIXED_VOLTAGE \
                EXT4_FS BTRFS_FS \
                NET UNIX INET SECCOMP_FILTER NAMESPACES NET_NS PID_NS \
                CGROUPS FHANDLE INOTIFY_USER SIGNALFD TIMERFD EPOLL \
                DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_XATTR TMPFS_POSIX_ACL \
                PROC_FS PROC_SYSCTL SYSFS BTRFS_FS_POSIX_ACL; do
    grep -q "^CONFIG_${required}=y$" "$CONFIG" || \
        die "required kernel option CONFIG_${required}=y was not selected"
done

# Off on purpose, and worth failing over: WIRELESS defaults to y under NET, and
# an accidental =y here drags cfg80211 and a WLAN menu into an image with 2.5 MiB
# of slack in a fixed partition.
for refused in WIRELESS BT; do
    if grep -q "^CONFIG_${refused}=y$" "$CONFIG"; then
        die "CONFIG_${refused}=y came back after olddefconfig; it must stay off until the WiFi driver lands"
    fi
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
say() {
    echo "$@"
    if [ -c /dev/tty1 ]; then echo "$@" >/dev/tty1; fi
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
say "Display stays on the stock-LK framebuffer; native DSI is disabled."
insmod /lib/modules/*/extra/j36_mt6592_input.ko || say "input module load failed"

# ── Hand over to the rootfs on the card, if there is one ─────────────────────
#
# root= is treated as a hint and nothing more.  Partition numbering follows
# whichever MMC host attached first, and a card can be repartitioned between
# boots, so every candidate is proved by mounting it and looking for /sbin/init
# before the machine is handed to it.  Failing that we stay here with a shell,
# which is strictly better than the kernel panicking on a root= it cannot honour.
root_hint=""
for arg in $(cat /proc/cmdline); do
    case "$arg" in
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

# rdinit=/init stays even though root= is now present, and the two do not
# conflict: rdinit means the kernel never mounts a root filesystem itself, so a
# root= it could not honour can no longer panic it.  /init does the mounting, and
# treats root= as a hint it verifies before switching -- delete the root= below
# and the card boots to the initramfs shell exactly as it did before.
cat > "$SDBOOT/mvii/boot.conf" <<'CONF'
# MVII LK SD hand-off, J36 Ultra (MT6592, ARMv7).
#
# Read after the card's own boot.ini, so these override it.  A dArkOS boot.ini
# names the RK3326 arm64 kernel, which this SoC cannot execute.
kernel=zImage
dtb=mt6592-j36-ultra.dtb
initrd=initrd.img

# root= is a hint for /init, not an order to the kernel: rdinit keeps the kernel
# out of root mounting entirely.  /init mounts this partition read-only first and
# switches into it only if it really holds /sbin/init, otherwise it scans the
# other mmcblk partitions, otherwise it gives you a shell.  mmcblk0p2 is the
# dArkOS ROOTFS partition, and microSD is mmcblk0 because MSDC1 is the only MMC
# host in the device tree.  Remove root= to stop at the initramfs.
bootargs=console=tty0 console=ttyS0,115200n8 earlycon=mtk8250,mmio32,0x11002000 rdinit=/init root=/dev/mmcblk0p2 rw rootwait
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
armhf Debian rootfs is shared, and this kernel can now mount it: MSDC1, the
microSD host, is driven by mtk-sd through a mediatek,mt6592-mmc node, and btrfs
and ext4 are both built in.  /init verifies a candidate partition by mounting it
read-only and looking for /sbin/init, then switch_roots into it.  If nothing
qualifies -- or if you delete root= from mvii/boot.conf -- it stops at a busybox
shell on the panel and on the serial port instead, and prints /proc/partitions so
you can see what the kernel did find.
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
        echo "bootimg_size=$(stat -c %s boot.img) (slot 0x900000)"
        echo "storage=msdc1 mtk-sd mediatek,mt6592-mmc (btrfs, ext4)"
        echo "msdc1_irq=GIC_SPI 72 (INTID 104 - 32)"
        echo "userspace=net+unix+namespaces (systemd 257 on the shared armhf rootfs)"
        echo "wireless=off"
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
