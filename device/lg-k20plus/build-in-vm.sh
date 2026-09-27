#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# LG K20 Plus layer, built inside the Multipass VM.
#
# Same architecture as the OPPO build (see device/oppo-mt6877/build-in-vm.sh
# for the shape): kernel + generated DTB + out-of-tree modules + initramfs +
# payload, incremental and checkpointed. The deliverable is the same pair:
#
#   boot.img          `fastboot flash boot` (LK hands over, panel already on)
#   rootfs.tar.gz     arm64 Debian with the QMI telephony userspace and
#                     /opt/mixos/lg/<device>/, unpacked once onto PARTLABEL=ROOTFS
#
# Environment (all set by build-lg.sh): LG_BUILD_DIR, LG_WORK_DIR,
# LG_EXPORT_DIR, LG_DEVICE (default lv517), LG_MIX_ONLY, LG_JOBS,
# LG_KERNEL_BRANCH/URL, LG_FIRMWARE_DIR, DEBIAN_CODE_NAME.

set -Eeuo pipefail

log() { printf '\n[build-lg] %s\n' "$*"; }
die() { printf '\n[build-lg] ERROR: %s\n' "$*" >&2; exit 1; }

ROOT="${LG_BUILD_DIR:?set by build-lg.sh}"
WORK="${LG_WORK_DIR:?set by build-lg.sh}"
EXPORT="${LG_EXPORT_DIR:?set by build-lg.sh}"
DEVICE="${LG_DEVICE:-lv517}"
MIX_ONLY="${LG_MIX_ONLY:-0}"
JOBS="${LG_JOBS:-$(nproc)}"
KERNEL_BRANCH="${LG_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_URL="${LG_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
FIRMWARE_DIR="${LG_FIRMWARE_DIR:-}"
DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"

DEVDIR="$ROOT/device/lg-k20plus"
KDIR="$WORK/kernel-6.12-arm64"
KOUT="$WORK/kernel-out"
ART="$WORK/artifacts"

# shellcheck source=device/lg-k20plus/devices.sh
source "$DEVDIR/devices.sh"
DEVICE_INFO="$(lg_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
[[ "$LG_ARCH" == "arm64" ]] || die "$DEVICE is $LG_ARCH, this build is arm64"
log "LG $LG_DEVICE ($LG_NOTES)"

mkdir -p "$WORK" "$ART"
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
    log "Installing the arm64 toolchain"
    sudo apt-get update -qq
    sudo apt-get install -y -qq gcc-aarch64-linux-gnu make bc bison flex \
        libssl-dev libelf-dev dwarves python3 device-tree-compiler git \
        debootstrap abootimg busybox-static 2>&1 | tail -n 2
}

# ── the device tree, first ──────────────────────────────────────────────────
DTS="$WORK/lg-$DEVICE.dts"
DTB="$ART/lg-$DEVICE.dtb"
python3 "$DEVDIR/generate_dts_lg.py" --board "$DEVDIR/board" \
    --device "$DEVICE" --panel "$LG_PANEL" --rev "$LG_REV" --out "$DTS"
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
apply_kernel_patch 0001-remoteproc-q6v5-mss-msm8917.patch "q6v5_mss MSM8917"

mkdir -p "$KOUT"
if [[ ! -f "$KOUT/.config" ]]; then
    log "Creating the arm64 kernel configuration"
    make -C "$KDIR" O="$KOUT" ARCH=arm64 \
        CROSS_COMPILE=aarch64-linux-gnu- defconfig
fi
config_y() { "$KDIR/scripts/config" --file "$KOUT/.config" -e "$1"; }
config_m() { "$KDIR/scripts/config" --file "$KOUT/.config" -m "$1"; }

# Built-in: serial, eMMC, SMD/RPMSG (modem+wifi control), RTC. Modular:
# everything behind an lg.* word.
for sym in SERIAL_MSM MMC_SDHCI_MSM QCOM_SMD QCOM_SMD_RPM RPMSG_QCOM_SMD \
        QCOM_SMEM QCOM_SMP2P QCOM_SMSM SPMI SPMI_PMIC_ARB SIMPLEFB PINCTRL; do
    config_y "$sym" 2>/dev/null || true
done
for sym in QCOM_Q6V5_MSS QCOM_WCNSS_CTRL WCN36XX CFG80211 RFKILL QRTR \
        RMI4_CORE RMI4_I2C RMI4_F11 SND_SOC_LPASS_CPU BACKLIGHT_QCOM_WLED \
        PM8941_PWRKEY POWER_SUPPLY IIO QCOM_SPMI_ADC5 INPUT_RMI4; do
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

# ── the initramfs ────────────────────────────────────────────────────────────
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
# LG MixOS /init: like the OPPO one, with lg.* words and the MSM console.
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
cmdline="$(cat /proc/cmdline)"
has() { case " $cmdline " in *" $1"*) return 0;; *) return 1;; esac; }
ins() { [ -f "$1" ] && insmod "$1" && echo "lg-init: loaded $1" || echo "lg-init: MISSING $1"; }
PAY=/opt/mixos/lg
if has lg.power=1 || has lg.power; then ins $PAY/power/lg_msm8917_pmic.ko; fi
ins $PAY/input/lg_msm8917_input.ko
ins $PAY/touch/lg_msm8917_touch.ko
if has lg.audio=1 || has lg.audio; then ins $PAY/audio/lg_msm8917_audio.ko; fi
if has lg.wifi=1 || has lg.wifi; then ins $PAY/wifi/lg_msm8917_wcnss.ko; fi
if has lg.modem=1 || has lg.modem; then ins $PAY/modem/lg_msm8917_modem.ko; fi
echo "lg-init: waiting for PARTLABEL=ROOTFS"
for _ in $(seq 1 30); do
    root="$(findfs PARTLABEL=ROOTFS 2>/dev/null)" && break
    sleep 1
done
if [ -z "${root:-}" ]; then
    echo "lg-init: no PARTLABEL=ROOTFS; rescue shell"
    exec /bin/sh
fi
mount -o ro "$root" /newroot || { echo "lg-init: cannot mount $root"; exec /bin/sh; }
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
mount --move /dev /newroot/dev
echo "lg-init: switching root to $root"
exec switch_root /newroot /sbin/init
INIT_EOF
chmod +x "$INITRD/init"

stage_payload() {
    local dest="$1"
    mkdir -p "$dest"/{audio,wifi,modem,power,input,touch,display}
    for m in audio/lg_msm8917_audio wifi/lg_msm8917_wcnss modem/lg_msm8917_modem \
             power/lg_msm8917_pmic input/lg_msm8917_input touch/lg_msm8917_touch \
             display/lg_msm8917_panel; do
        cp "$MODDIR"/lib/modules/*/extra/"$(basename "$m").ko" "$dest/$m.ko" 2>/dev/null || true
    done
    {
        echo "power/lg_msm8917_pmic.ko"
        echo "input/lg_msm8917_input.ko"
        echo "touch/lg_msm8917_touch.ko"
        echo "audio/lg_msm8917_audio.ko"
        echo "wifi/lg_msm8917_wcnss.ko"
        echo "modem/lg_msm8917_modem.ko"
        echo "display/lg_msm8917_panel.ko"
    } > "$dest/load.order"
}
stage_payload "$INITRD/opt/mixos/lg/$DEVICE"

if [[ -n "$FIRMWARE_DIR" && -d "$FIRMWARE_DIR" ]]; then
    mkdir -p "$INITRD/lib/firmware/lg/lv517"
    cp "$FIRMWARE_DIR"/modem.mdt "$FIRMWARE_DIR"/modem.b* \
       "$FIRMWARE_DIR"/wcnss.mdt "$FIRMWARE_DIR"/wcnss.b* \
       "$INITRD/lib/firmware/lg/lv517/" 2>/dev/null || true
    # A PIL set with a missing segment fails auth exactly like a driver
    # bug, so the build counts the set instead of hoping it is whole.
    for base in modem wcnss; do
        segs="$(ls "$INITRD/lib/firmware/lg/lv517/$base.b"* 2>/dev/null | wc -l)"
        log "Firmware set $base: $segs segments"
    done
else
    log "WARNING: no LG_FIRMWARE_DIR; modem/wifi will report missing blobs"
fi

CPIO="$ART/lg-$DEVICE.cpio"
( cd "$INITRD" && find . -print0 | cpio --quiet -o -H newc --null | gzip -9 > "$CPIO.gz" )

# ── boot.img ─────────────────────────────────────────────────────────────────
BOOTIMG="$ART/lg-$DEVICE-boot.img"
CMDLINE="earlycon console=ttyMSM0,115200n8 root=PARTLABEL=ROOTFS rw rootwait lg.audio=1 lg.wifi=1 lg.modem=1 lg.power=1 lg.device=$DEVICE lg.panel=$LG_PANEL"
# The device tree rides appended to the kernel -- the header is v1 so the
# 2016-era aboot parses it, and it has always found the DTB this way.
# Packing is device/common/mkbootimg.py: no AOSP host tools required.
cat "$KOUT/arch/arm64/boot/Image" "$DTB" > "$WORK/Image-dtb"
python3 "$ROOT/device/common/mkbootimg.py" \
    --kernel "$WORK/Image-dtb" --ramdisk "$CPIO.gz" \
    --cmdline "$CMDLINE" --base 0x80000000 --pagesize 4096 \
    --name "mixos-$DEVICE" --output "$BOOTIMG"
log "boot.img: $(stat -c %s "$BOOTIMG") bytes"

# ── the Debian rootfs (full builds only; checkpointed) ──────────────────────
ROOTFS_TGZ="$ART/lg-$DEVICE-rootfs.tar.gz"
if [[ "$MIX_ONLY" == 1 ]]; then
    log "--mix-only: boot.img + payload only, no rootfs"
else
    if [[ ! -f "$WORK/rootfs-$DEVICE.done" ]]; then
        log "Bootstrapping arm64 Debian $DEBIAN_CODE_NAME (checkpointed)"
        sudo rm -rf "$WORK/rootfs"
        sudo debootstrap --arch=arm64 "$DEBIAN_CODE_NAME" "$WORK/rootfs" \
            http://deb.debian.org/debian/
        sudo chroot "$WORK/rootfs" apt-get install -y -qq \
            modemmanager libqmi-utils libqrtr-glib-utils qrtr-tools iwd \
            wireless-tools kmod rfkill 2>&1 | tail -n 1
        : > "$WORK/rootfs-$DEVICE.done"
    else
        log "Reusing the checkpointed rootfs"
    fi
    sudo mkdir -p "$WORK/rootfs/opt/mixos/lg/$DEVICE" \
        "$WORK/rootfs/opt/mixos/lg/telephony" \
        "$WORK/rootfs/lib/firmware/lg/lv517" \
        "$WORK/rootfs/etc/udev/rules.d" "$WORK/rootfs/etc/systemd/system"
    sudo cp -r "$INITRD/opt/mixos/lg/$DEVICE" "$WORK/rootfs/opt/mixos/lg/"
    sudo cp "$DEVDIR"/telephony/modem-boot.sh "$DEVDIR"/telephony/check-telephony.sh \
        "$WORK/rootfs/opt/mixos/lg/telephony/"
    sudo cp "$DEVDIR"/telephony/lg-modem.rules "$WORK/rootfs/etc/udev/rules.d/"
    sudo cp "$DEVDIR"/telephony/lg-telephony.service \
        "$WORK/rootfs/etc/systemd/system/"
    sudo cp "$DEVDIR"/telephony/apn.conf.template \
        "$WORK/rootfs/opt/mixos/lg/telephony/"
    if [[ -n "$FIRMWARE_DIR" && -d "$FIRMWARE_DIR" ]]; then
        sudo cp "$FIRMWARE_DIR"/modem.mdt "$FIRMWARE_DIR"/modem.b* \
            "$FIRMWARE_DIR"/wcnss.mdt "$FIRMWARE_DIR"/wcnss.b* \
            "$WORK/rootfs/lib/firmware/lg/lv517/" 2>/dev/null || true
    fi
    sudo chroot "$WORK/rootfs" systemctl enable lg-telephony.service \
        2>/dev/null || true
    ( cd "$WORK/rootfs" && sudo tar -czf "$ROOTFS_TGZ" . )
    log "rootfs: $(stat -c %s "$ROOTFS_TGZ") bytes"
fi

# ── hand-over ────────────────────────────────────────────────────────────────
{
    echo "device=$DEVICE"
    echo "panel=$LG_PANEL"
    echo "bootimg=lg-$DEVICE-boot.img"
    if [[ "$MIX_ONLY" == 1 ]]; then
        echo "rootfs=none"
    else
        echo "rootfs=lg-$DEVICE-rootfs.tar.gz"
    fi
} > "$ART/lg-$DEVICE-manifest.txt"

if [[ "$MIX_ONLY" == 1 ]]; then
    mkdir -p "$EXPORT/boot" "$EXPORT/root"
    cp "$BOOTIMG" "$DTB" "$EXPORT/boot/"
    cp -r "$INITRD/opt/mixos" "$EXPORT/root/opt-mixos"
    cp "$ART/lg-$DEVICE-manifest.txt" "$EXPORT/"
else
    mkdir -p "$EXPORT"
    cp "$BOOTIMG" "$ROOTFS_TGZ" "$ART/lg-$DEVICE-manifest.txt" "$EXPORT/"
fi
log "LG $DEVICE done"
