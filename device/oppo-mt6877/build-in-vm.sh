#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# OPPO MT6877 layer, built inside the Multipass VM.
#
# THIS IS THE J36 BUILD'S YOUNGER SIBLING, NOT A COPY OF IT. It follows the
# same architecture -- kernel + generated DTB + out-of-tree modules +
# initramfs + payload, all incremental and checkpointed -- but its deliverable
# is a phone boot image, not an SD card image:
#
#   boot.img          Android boot image (kernel + ramdisk + DTB), flashed
#                     with `fastboot flash boot`. The LK loads it the way the
#                     MVII LK loads the J36 kernel: framebuffer already on.
#   rootfs.tar.gz     arm64 Debian (trixie) with the telephony userspace and
#                     /opt/mixos/oppo/<device>/, unpacked onto the phone's
#                     ROOTFS partition (PARTLABEL=ROOTFS) once, then updated
#                     with apt like any Debian.
#
# Environment (all set by build-oppo.sh):
#   OPPO_BUILD_DIR   synced checkout in the VM
#   OPPO_WORK_DIR    persistent work dir (kernel tree, rootfs, artifacts)
#   OPPO_EXPORT_DIR  where --mix-only artifacts go (a mount in that mode)
#   OPPO_DEVICE      codename from devices.sh (default 20181)
#   OPPO_MIX_ONLY    1 = boot.img + modules + payload only, no rootfs
#   OPPO_JOBS        parallelism (default nproc)
#   OPPO_KERNEL_BRANCH / OPPO_KERNEL_URL (defaults: linux-6.12.y, kernel.org)
#   OPPO_FIRMWARE_DIR  host-side blobs (modem.img, WIFI_RAM_CODE, ...); empty
#                      skips the firmware stage with a warning

set -Eeuo pipefail

log() { printf '\n[build-oppo] %s\n' "$*"; }
die() { printf '\n[build-oppo] ERROR: %s\n' "$*" >&2; exit 1; }

ROOT="${OPPO_BUILD_DIR:?set by build-oppo.sh}"
WORK="${OPPO_WORK_DIR:?set by build-oppo.sh}"
EXPORT="${OPPO_EXPORT_DIR:?set by build-oppo.sh}"
DEVICE="${OPPO_DEVICE:-20181}"
MIX_ONLY="${OPPO_MIX_ONLY:-0}"
JOBS="${OPPO_JOBS:-$(nproc)}"
KERNEL_BRANCH="${OPPO_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_URL="${OPPO_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
FIRMWARE_DIR="${OPPO_FIRMWARE_DIR:-}"
DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"

DEVDIR="$ROOT/device/oppo-mt6877"
KDIR="$WORK/kernel-6.12-arm64"
KOUT="$WORK/kernel-out"
ART="$WORK/artifacts"

# ── the device, first, so a typo fails in a second ──────────────────────────
# shellcheck source=device/oppo-mt6877/devices.sh
source "$DEVDIR/devices.sh"
DEVICE_INFO="$(oppo_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
[[ "$OPPO_ARCH" == "arm64" ]] || die "$DEVICE is $OPPO_ARCH, this build is arm64"
log "OPPO $OPPO_DEVICE ($OPPO_NOTES)"

mkdir -p "$WORK" "$ART"
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
    log "Installing the arm64 toolchain"
    sudo apt-get update -qq
    sudo apt-get install -y -qq gcc-aarch64-linux-gnu make bc bison flex \
        libssl-dev libelf-dev dwarves python3 device-tree-compiler git \
        debootstrap abootimg busybox-static 2>&1 | tail -n 2
}

# ── the device tree, second, for the same reason ─────────────────────────────
DTS="$WORK/oppo-$DEVICE.dts"
DTB="$ART/oppo-$DEVICE.dtb"
python3 "$DEVDIR/generate_dts_oppo.py" --board "$DEVDIR/board" \
    --device "$DEVICE" --out "$DTS"
dtc -I dts -O dtb -o "$DTB" "$DTS"

# ── the kernel ───────────────────────────────────────────────────────────────
if [[ ! -d "$KDIR/.git" ]]; then
    log "Cloning $KERNEL_BRANCH (arm64)"
    git clone --depth=1 --branch "$KERNEL_BRANCH" "$KERNEL_URL" "$KDIR"
fi

apply_kernel_patch() {
    local patch="$DEVDIR/linux/$1" what="$2"
    [[ -f "$patch" ]] || die "missing kernel patch: $patch"
    if git -C "$KDIR" apply --reverse --check "$patch" 2>/dev/null; then
        log "The $what patch is already applied"
        return
    fi
    git -C "$KDIR" apply --check "$patch" 2>/dev/null \
        || die "$1 does not apply to $KERNEL_BRANCH; refresh it"
    log "Applying the $what patch"
    git -C "$KDIR" apply "$patch"
}
apply_kernel_patch 0001-mtk-sd-mt6877.patch "mtk-sd MT6877"
apply_kernel_patch 0002-pmic-wrap-mt6877.patch "pwrap MT6877"

mkdir -p "$KOUT"
if [[ ! -f "$KOUT/.config" ]]; then
    log "Creating the arm64 kernel configuration"
    make -C "$KDIR" O="$KOUT" ARCH=arm64 \
        CROSS_COMPILE=aarch64-linux-gnu- defconfig
fi
SC="$KDIR/scripts/config --file $KOUT/.config"
config_y() { "$KDIR/scripts/config" --file "$KOUT/.config" -e "$1"; }
config_m() { "$KDIR/scripts/config" --file "$KOUT/.config" -m "$1"; }

# What the OPPO DTS binds to. Built-in when /init needs it before modules
# exist (serial, eMMC, RTC); modular when it sits behind an oppo.* word.
for sym in SERIAL_8250_MT6577 MTK_MMC MTK_PMIC_WRAP MT6397 MFD_MT6397 \
        RTC_MT6397 REGULATOR_MT6359 MTK_SPI_SIMPLEFB; do
    config_y "$sym" 2>/dev/null || true
done
for sym in SND_SOC_MT6359 CFG80211 RFKILL POWER_SUPPLY IIO \
        MTK_PMIC_KEYS INPUT_MATRIXKMAP; do
    config_m "$sym" 2>/dev/null || true
done
make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    olddefconfig
make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    -j"$JOBS" Image modules

# ── the out-of-tree modules ──────────────────────────────────────────────────
MODDIR="$WORK/modules"
rm -rf "$MODDIR"
mkdir -p "$MODDIR"
make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    M="$DEVDIR/linux" -j"$JOBS" modules
make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    M="$DEVDIR/linux" INSTALL_MOD_PATH="$MODDIR" modules_install >/dev/null
find "$MODDIR" -name '*.ko' | sort

# ── the initramfs: /init finds ROOTFS, loads the oppo.* payload, switches ──
INITRD="$WORK/initramfs"
rm -rf "$INITRD"
mkdir -p "$INITRD"/{bin,sbin,etc,proc,sys,dev,lib/firmware,newroot,opt/mixos}
cp /bin/busybox "$INITRD/bin/busybox" 2>/dev/null \
    || cp /usr/bin/busybox "$INITRD/bin/busybox" \
    || die "busybox-static provides no /bin/busybox"
( cd "$INITRD/bin" && for a in sh mount umount switch_root insmod lsmod \
        mkdir mknod sleep echo cat blkid findfs; do
    ln -sf busybox "$a"
done )
cat > "$INITRD/init" <<'INIT_EOF'
#!/bin/busybox sh
# OPPO MixOS /init: mount the essentials, find PARTLABEL=ROOTFS on the eMMC,
# insmod exactly the payloads the oppo.* command-line words ask for, and
# switch root. Any failure drops to a rescue shell on the serial console --
# a phone that cannot find its rootfs must still talk.
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
cmdline="$(cat /proc/cmdline)"
has() { case " $cmdline " in *" $1"*) return 0;; *) return 1;; esac; }
ins() { [ -f "$1" ] && insmod "$1" && echo "oppo-init: loaded $1" || echo "oppo-init: MISSING $1"; }
PAY=/opt/mixos/oppo
if has oppo.power=1 || has oppo.power; then ins $PAY/power/oppo_mt6877_pmic.ko; fi
ins $PAY/input/oppo_mt6877_input.ko
ins $PAY/touch/oppo_mt6877_touch_nt36672.ko
if has oppo.audio=1 || has oppo.audio; then ins $PAY/audio/oppo_mt6877_audio.ko; fi
if has oppo.wifi=1 || has oppo.wifi; then
    ins $PAY/wifi/oppo_mt6877_consys.ko
    ins $PAY/wifi/oppo_mt6877_wifi.ko
fi
if has oppo.modem=1 || has oppo.modem; then ins $PAY/modem/oppo_mt6877_modem.ko; fi
echo "oppo-init: waiting for PARTLABEL=ROOTFS"
for _ in $(seq 1 30); do
    root="$(findfs PARTLABEL=ROOTFS 2>/dev/null)" && break
    sleep 1
done
if [ -z "${root:-}" ]; then
    echo "oppo-init: no PARTLABEL=ROOTFS; rescue shell"
    exec /bin/sh
fi
mount -o ro "$root" /newroot || { echo "oppo-init: cannot mount $root"; exec /bin/sh; }
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
mount --move /dev /newroot/dev
echo "oppo-init: switching root to $root"
exec switch_root /newroot /sbin/init
INIT_EOF
chmod +x "$INITRD/init"

# The payload rides in the ramdisk AND on the rootfs: the ramdisk copy boots
# a phone whose ROOTFS is stale, the rootfs copy is what modprobe sees.
stage_payload() {
    local dest="$1"
    mkdir -p "$dest"/{audio,wifi,modem,power,input,touch,display}
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_audio.ko "$dest/audio/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_consys.ko "$dest/wifi/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_wifi.ko "$dest/wifi/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_modem.ko "$dest/modem/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_pmic.ko "$dest/power/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_input.ko "$dest/input/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_touch_nt36672.ko "$dest/touch/" 2>/dev/null || true
    cp "$MODDIR"/lib/modules/*/extra/oppo_mt6877_panel_td4330.ko "$dest/display/" 2>/dev/null || true
    {
        echo "power/oppo_mt6877_pmic.ko"
        echo "input/oppo_mt6877_input.ko"
        echo "touch/oppo_mt6877_touch_nt36672.ko"
        echo "audio/oppo_mt6877_audio.ko"
        echo "wifi/oppo_mt6877_consys.ko"
        echo "wifi/oppo_mt6877_wifi.ko"
        echo "modem/oppo_mt6877_modem.ko"
        echo "display/oppo_mt6877_panel_td4330.ko"
    } > "$dest/load.order"
}
stage_payload "$INITRD/opt/mixos/oppo/$DEVICE"

# Firmware rides when the operator supplied it; otherwise the wifi/modem
# drivers fail with the extraction instructions, not silence.
if [[ -n "$FIRMWARE_DIR" && -d "$FIRMWARE_DIR" ]]; then
    mkdir -p "$INITRD/lib/firmware/oppo/mt6877"
    cp "$FIRMWARE_DIR"/modem.img "$FIRMWARE_DIR"/dsp.img \
       "$FIRMWARE_DIR"/WIFI_RAM_CODE "$FIRMWARE_DIR"/WMT_SOC.cfg \
       "$INITRD/lib/firmware/oppo/mt6877/" 2>/dev/null || true
    log "Staged stock firmware from $FIRMWARE_DIR"
else
    log "WARNING: no OPPO_FIRMWARE_DIR; wifi/modem will report missing blobs"
fi

CPIO="$ART/oppo-$DEVICE.cpio"
( cd "$INITRD" && find . -print0 | cpio --quiet -o -H newc --null | gzip -9 > "$CPIO.gz" )

# ── boot.img ─────────────────────────────────────────────────────────────────
BOOTIMG="$ART/oppo-$DEVICE-boot.img"
CMDLINE="earlycon console=ttyS0,921600n8 root=PARTLABEL=ROOTFS rw rootwait oppo.audio=1 oppo.wifi=1 oppo.modem=1 oppo.power=1 oppo.device=$DEVICE"
mkbootimg --kernel "$KOUT/arch/arm64/boot/Image" \
    --ramdisk "$CPIO.gz" --dtb "$DTB" \
    --cmdline "$CMDLINE" --base 0x40000000 --pagesize 4096 \
    -o "$BOOTIMG"
log "boot.img: $(stat -c %s "$BOOTIMG") bytes"

# ── the Debian rootfs (full builds only; checkpointed) ──────────────────────
ROOTFS_TGZ="$ART/oppo-$DEVICE-rootfs.tar.gz"
if [[ "$MIX_ONLY" == 1 ]]; then
    log "--mix-only: boot.img + payload only, no rootfs"
else
    if [[ ! -f "$WORK/rootfs-$DEVICE.done" ]]; then
        log "Bootstrapping arm64 Debian $DEBIAN_CODE_NAME (checkpointed)"
        sudo rm -rf "$WORK/rootfs"
        sudo debootstrap --arch=arm64 "$DEBIAN_CODE_NAME" "$WORK/rootfs" \
            http://deb.debian.org/debian/
        sudo chroot "$WORK/rootfs" apt-get install -y -qq \
            modemmanager ppp iwd wireless-tools kmod rfkill 2>&1 | tail -n 1
        : > "$WORK/rootfs-$DEVICE.done"
    else
        log "Reusing the checkpointed rootfs"
    fi
    sudo mkdir -p "$WORK/rootfs/opt/mixos/oppo/$DEVICE" \
        "$WORK/rootfs/opt/mixos/oppo/telephony" \
        "$WORK/rootfs/lib/firmware/oppo/mt6877" \
        "$WORK/rootfs/etc/udev/rules.d" "$WORK/rootfs/etc/systemd/system"
    sudo cp -r "$INITRD/opt/mixos/oppo/$DEVICE" "$WORK/rootfs/opt/mixos/oppo/"
    sudo cp "$DEVDIR"/telephony/modem-boot.sh "$DEVDIR"/telephony/check-telephony.sh \
        "$WORK/rootfs/opt/mixos/oppo/telephony/"
    sudo cp "$DEVDIR"/telephony/oppo-modem.rules "$WORK/rootfs/etc/udev/rules.d/"
    sudo cp "$DEVDIR"/telephony/oppo-telephony.service \
        "$WORK/rootfs/etc/systemd/system/"
    sudo cp "$DEVDIR"/telephony/apn.conf.template \
        "$WORK/rootfs/opt/mixos/oppo/telephony/"
    if [[ -n "$FIRMWARE_DIR" && -d "$FIRMWARE_DIR" ]]; then
        sudo cp "$FIRMWARE_DIR"/modem.img "$FIRMWARE_DIR"/dsp.img \
            "$FIRMWARE_DIR"/WIFI_RAM_CODE "$FIRMWARE_DIR"/WMT_SOC.cfg \
            "$WORK/rootfs/lib/firmware/oppo/mt6877/" 2>/dev/null || true
    fi
    sudo chroot "$WORK/rootfs" systemctl enable oppo-telephony.service \
        2>/dev/null || true
    ( cd "$WORK/rootfs" && sudo tar -czf "$ROOTFS_TGZ" . )
    log "rootfs: $(stat -c %s "$ROOTFS_TGZ") bytes"
fi

# ── hand-over ────────────────────────────────────────────────────────────────
{
    echo "device=$DEVICE"
    echo "bootimg=oppo-$DEVICE-boot.img"
    if [[ "$MIX_ONLY" == 1 ]]; then
        echo "rootfs=none"
    else
        echo "rootfs=oppo-$DEVICE-rootfs.tar.gz"
    fi
} > "$ART/oppo-$DEVICE-manifest.txt"

if [[ "$MIX_ONLY" == 1 ]]; then
    mkdir -p "$EXPORT/boot" "$EXPORT/root"
    cp "$BOOTIMG" "$DTB" "$EXPORT/boot/"
    cp -r "$INITRD/opt/mixos" "$EXPORT/root/opt-mixos"
    cp "$ART/oppo-$DEVICE-manifest.txt" "$EXPORT/"
    log "Exported to $EXPORT"
else
    mkdir -p "$EXPORT"
    cp "$BOOTIMG" "$ROOTFS_TGZ" "$ART/oppo-$DEVICE-manifest.txt" "$EXPORT/"
    log "Exported to $EXPORT"
fi
log "OPPO $DEVICE done"