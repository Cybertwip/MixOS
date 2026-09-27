#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# OPPO A77 5G (2022, MT6833) layer, built inside the Multipass VM.
#
# BRING-UP SCAFFOLD -- see device/oppo-a77/BRINGUP.md. Same shape as the
# proven phone builds (kernel + generated DTB + initramfs + rootfs, one
# MixOS image), but with NO out-of-tree drivers, NO subsystem DTS nodes
# and NO firmware staging yet: every hardware fact those need is still
# unknown, and this script emits nothing it cannot source. It builds; it
# does not boot. The wrapper refuses to run it without OPPO_A77_BRINGUP_ACK=1.
#
# Environment (all set by build-oppo-a77.sh): A77_BUILD_DIR, A77_WORK_DIR,
# A77_EXPORT_DIR, A77_DEVICE (default cph2381), A77_FULL_IMAGE_NAME,
# A77_MIX_ONLY, A77_JOBS, A77_KERNEL_BRANCH/URL, A77_FIRMWARE_DIR,
# A77_FB_BASE (0 disables simplefb), DEBIAN_CODE_NAME.

set -Eeuo pipefail

log() { printf '\n[build-a77] %s\n' "$*"; }
die() { printf '\n[build-a77] ERROR: %s\n' "$*" >&2; exit 1; }

ROOT="${A77_BUILD_DIR:?set by build-oppo-a77.sh}"
WORK="${A77_WORK_DIR:?set by build-oppo-a77.sh}"
EXPORT="${A77_EXPORT_DIR:?set by build-oppo-a77.sh}"
DEVICE="${A77_DEVICE:-cph2381}"
FULL_IMAGE_NAME="${A77_FULL_IMAGE_NAME:?set by build-oppo-a77.sh}"
MIX_ONLY="${A77_MIX_ONLY:-0}"
JOBS="${A77_JOBS:-$(nproc)}"
KERNEL_BRANCH="${A77_KERNEL_BRANCH:-linux-6.12.y}"
KERNEL_URL="${A77_KERNEL_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"
FIRMWARE_DIR="${A77_FIRMWARE_DIR:-}"
FB_BASE="${A77_FB_BASE:-0}"
DEBIAN_CODE_NAME="${DEBIAN_CODE_NAME:-trixie}"

DEVDIR="$ROOT/device/oppo-a77"
KDIR="$WORK/kernel-6.12-arm64"
KOUT="$WORK/kernel-out"
ART="$WORK/artifacts"

# ── the device, first, so a typo fails in a second ──────────────────────────
# shellcheck source=device/oppo-a77/devices.sh
source "$DEVDIR/devices.sh"
DEVICE_INFO="$(a77_device_info "$DEVICE")" || exit 1
eval "$DEVICE_INFO"
[[ "$A77_ARCH" == "arm64" ]] || die "$DEVICE is $A77_ARCH, this build is arm64"
log "A77 $A77_DEVICE ($A77_NOTES)"

mkdir -p "$WORK" "$ART"
command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || {
    log "Installing the arm64 toolchain"
    sudo apt-get update -qq
    sudo apt-get install -y -qq gcc-aarch64-linux-gnu make bc bison flex \
        libssl-dev libelf-dev dwarves python3 device-tree-compiler git \
        debootstrap busybox-static cpio e2fsprogs 2>&1 | tail -n 2
}

# ── the device tree, second, for the same reason ─────────────────────────────
DTS="$WORK/a77-$DEVICE.dts"
DTB="$ART/a77-$DEVICE.dtb"
# Geometry comes from devices.sh inside the generator; --fb-base stays empty
# until BRINGUP step 2 measures the LK framebuffer.
python3 "$DEVDIR/generate_dts_a77.py" --device "$DEVICE" \
    --fb-base "$FB_BASE" --out "$DTS"
dtc -I dts -O dtb -o "$DTB" "$DTS"

# ── the kernel: defconfig only, no patches, no extras ────────────────────────
# Nothing here is known-broken (no facts to patch against) and nothing is
# known-needed (no DTS nodes bind anything yet). Both change in BRINGUP.
if [[ ! -d "$KDIR/.git" ]]; then
    log "Cloning $KERNEL_BRANCH (arm64)"
    git clone --depth=1 --branch "$KERNEL_BRANCH" "$KERNEL_URL" "$KDIR"
fi
mkdir -p "$KOUT"
if [[ ! -f "$KOUT/.config" ]]; then
    log "Creating the arm64 kernel configuration"
    make -C "$KDIR" O="$KOUT" ARCH=arm64 \
        CROSS_COMPILE=aarch64-linux-gnu- defconfig
fi
make -C "$KDIR" O="$KOUT" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    -j"$JOBS" Image modules

# ── no out-of-tree modules yet ───────────────────────────────────────────────
log "No out-of-tree drivers yet (BRINGUP.md step 3); kernel modules only"

# ── the initramfs: /init finds ROOTFS and switches, nothing to insmod ───────
INITRD="$WORK/initramfs"
rm -rf "$INITRD"
mkdir -p "$INITRD"/{bin,sbin,etc,proc,sys,dev,newroot}
cp /bin/busybox "$INITRD/bin/busybox" 2>/dev/null \
    || cp /usr/bin/busybox "$INITRD/bin/busybox" \
    || die "busybox-static provides no /bin/busybox"
( cd "$INITRD/bin" && for a in sh mount umount switch_root insmod lsmod \
        mkdir mknod sleep echo cat blkid findfs; do
    ln -sf busybox "$a"
done )
cat > "$INITRD/init" <<'INIT_EOF'
#!/bin/busybox sh
# A77 MixOS /init: mount the essentials, find PARTLABEL=ROOTFS, switch.
# No payload to insmod yet (no drivers); any failure drops to a rescue
# shell on the serial console -- a phone that cannot find its rootfs
# must still talk.
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo "a77-init: waiting for PARTLABEL=ROOTFS"
for _ in $(seq 1 30); do
    root="$(findfs PARTLABEL=ROOTFS 2>/dev/null)" && break
    sleep 1
done
if [ -z "${root:-}" ]; then
    echo "a77-init: no PARTLABEL=ROOTFS; rescue shell"
    exec /bin/sh
fi
mount -o ro "$root" /newroot || { echo "a77-init: cannot mount $root"; exec /bin/sh; }
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
mount --move /dev /newroot/dev
echo "a77-init: switching root to $root"
exec switch_root /newroot /sbin/init
INIT_EOF
chmod +x "$INITRD/init"

# Firmware supplied but nothing requests it: no drivers, no request paths.
# Staging it nowhere is deliberate; extract-stock.sh still fills stock/
# for the day drivers exist.
if [[ -n "$FIRMWARE_DIR" && -d "$FIRMWARE_DIR" ]]; then
    log "Firmware supplied but no drivers request it yet; staging nothing"
fi

CPIO="$ART/a77-$DEVICE.cpio"
( cd "$INITRD" && find . -print0 | cpio --quiet -o -H newc --null | gzip -9 > "$CPIO.gz" )

# ── boot.img ─────────────────────────────────────────────────────────────────
BOOTIMG="$ART/a77-$DEVICE-boot.img"
# No earlycon: console UART unknown (BRINGUP step 2).
# oppo.device= matches the DTS bootargs word and the oppo-mt6877 tree.
CMDLINE="root=PARTLABEL=ROOTFS rw rootwait oppo.device=$DEVICE"
cat "$KOUT/arch/arm64/boot/Image" "$DTB" > "$WORK/Image-dtb"
python3 "$ROOT/device/common/mkbootimg.py" \
    --kernel "$WORK/Image-dtb" --ramdisk "$CPIO.gz" \
    --cmdline "$CMDLINE" --base 0x40000000 --pagesize 4096 \
    --name "mixos-$DEVICE" --output "$BOOTIMG"
log "boot.img: $(stat -c %s "$BOOTIMG") bytes"

# ── the Debian rootfs (full builds only; checkpointed) ──────────────────────
TRIXIE_IMG="$ART/a77-$DEVICE-trixie.img"
FULLIMG="$ART/$FULL_IMAGE_NAME"
if [[ "$MIX_ONLY" == 1 ]]; then
    log "--mix-only: boot.img + DTB + DTS only, no rootfs"
else
    if [[ ! -f "$WORK/rootfs-$DEVICE.done" ]]; then
        log "Bootstrapping arm64 Debian ($DEBIAN_CODE_NAME)"
        sudo rm -rf "$WORK/rootfs"
        sudo mkdir -p "$WORK/rootfs"
        sudo debootstrap --arch=arm64 --variant=minbase \
            "$DEBIAN_CODE_NAME" "$WORK/rootfs" \
            http://deb.debian.org/debian/
        sudo touch "$WORK/rootfs-$DEVICE.done"
    else
        log "Reusing the checkpointed rootfs"
    fi
    sudo mkdir -p "$WORK/rootfs/opt/mixos/a77/$DEVICE"
    echo "bring-up scaffold: payload lands here (BRINGUP.md step 3)" \
        | sudo tee "$WORK/rootfs/opt/mixos/a77/$DEVICE/BRINGUP-SCAFFOLD.txt" >/dev/null
    sudo bash "$ROOT/device/common/make-trixie-img.sh" \
        "$WORK/rootfs" "$TRIXIE_IMG" ROOTFS
    {
        echo "image=$FULL_IMAGE_NAME"
        python3 "$ROOT/device/common/make_full_img.py" \
            --boot "$BOOTIMG" --rootfs "$TRIXIE_IMG" --output "$FULLIMG"
    } > "$ART/full-image.txt"
    log "full image: $FULL_IMAGE_NAME ($(stat -c %s "$FULLIMG") bytes)"
fi

# ── hand-over: the one image, and nothing else ──────────────────────────────
if [[ "$MIX_ONLY" == 1 ]]; then
    mkdir -p "$EXPORT/boot"
    cp "$BOOTIMG" "$DTB" "$DTS" "$EXPORT/boot/"
    log "Exported to $EXPORT"
else
    mkdir -p "$EXPORT"
    cp "$FULLIMG" "$EXPORT/"
    log "Exported to $EXPORT"
fi
log "A77 $DEVICE done"
