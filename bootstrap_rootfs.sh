#!/bin/bash

echo -e "Boostraping Debian....\n\n"
if [[ "${USERSPACE_ARCH:-}" == "armhf" ]]; then
  DEBOOTSTRAP_ARCH=armhf
  QEMU_STATIC=qemu-arm-static
  ROOTFS_VARIANT=userspace-armhf
elif [[ "${USERSPACE_ARCH:-}" == "arm64" ]]; then
  DEBOOTSTRAP_ARCH=arm64
  QEMU_STATIC=qemu-aarch64-static
  ROOTFS_VARIANT=userspace-arm64
else
  # Legacy device builds retain the original arm64 + optional armhf behavior.
  DEBOOTSTRAP_ARCH=arm64
  QEMU_STATIC=qemu-aarch64-static
  ROOTFS_VARIANT="arm64-multiarch-${BUILD_ARMHF:-y}"
fi
ROOTFS_CACHE="MixOSBuild_package_cache/debian_${DEBIAN_CODE_NAME}_${ROOTFS_VARIANT}_rootfs"
if [ -f "${ROOTFS_CACHE}.tar.gz" ] && [ "$(cat "${ROOTFS_CACHE}.commit")" == "$(curl -s https://deb.debian.org/debian/dists/stable/Release | grep "^Version:" | cut -d' ' -f2)" ]; then
    # --strip-components=1 into a named destination, not a plain extract in $PWD.  The
    # archive carries whatever its top-level directory was called when it was written,
    # and a cache written before the Arkbuild -> MixOSBuild rename holds `Arkbuild/...'
    # members.  prepare.sh moves the cache DIRECTORY across, so that stale tarball is
    # found and trusted: tar unpacked a perfectly good rootfs into Arkbuild/, every
    # command after this one addressed MixOSBuild/, and the build spent a hundred lines
    # saying "No such file or directory" before dying with `stage bootstrap returned
    # 127'.  Naming the destination makes the extract independent of what the archive
    # calls its root -- for this rename and for the next one.
    #
    # -v is gone with it.  Listing forty thousand paths is what buried the real error.
    sudo mkdir -p MixOSBuild
    sudo tar -xzpf "${ROOTFS_CACHE}.tar.gz" --strip-components=1 -C MixOSBuild
else
	# ── ONE CACHE KEY, NOT TWO ───────────────────────────────────────────────
	#
	# DEBIAN_LOCATION always names the real mirror now.  It used to be rewritten
	# to http://127.0.0.1:3142/deb.debian.org/debian/ when the cache was on, and
	# debootstrap BAKES WHATEVER IT IS GIVEN into the sources.list of the tree it
	# creates -- so the chroot then asked for that URL through the proxy that the
	# 99proxy file a few lines down had just configured, and apt-cacher-ng saw a
	# request whose host was 127.0.0.1.
	#
	# That is a second cache key for the same file.  A package fetched by the
	# rewritten URL lands under 127.0.0.1/deb.debian.org/debian/... and the same
	# package fetched by any client that only had the proxy lands under debrep/...
	# -- and neither is a hit for the other.  Measured on this VM after one build:
	# 1014 MB under debrep/ and 960 MB under 127.0.0.1/, and `uniq -c' over every
	# .deb filename in the cache returned 2 for every single one.  Every package
	# downloaded twice, stored twice, and half of it re-downloaded on any run that
	# went through the other path.
	#
	# http_proxy on debootstrap's own environment does the same job with the same
	# key as everything downstream: the URL stays deb.debian.org, so acng maps it
	# to debrep/ exactly as it does for apt in the chroot.  `sudo env' and not
	# plain `sudo': sudo drops the environment, which is why this was ever done by
	# URL rewriting.  It also keeps the proxy out of the tree debootstrap writes,
	# so an image built with the cache on has no trace of 127.0.0.1 in it.
	export DEBIAN_LOCATION="http://deb.debian.org/debian/"
	if [[ "${ENABLE_CACHE}" == "y" ]]; then
	  DEBOOTSTRAP_PROXY=( env "http_proxy=http://127.0.0.1:3142/" )
	else
	  DEBOOTSTRAP_PROXY=( env )
	fi
	# Bootstrap base system
	sudo "${DEBOOTSTRAP_PROXY[@]}" eatmydata debootstrap --no-check-gpg --include=eatmydata --resolve-deps --arch=${DEBOOTSTRAP_ARCH} --foreign ${DEBIAN_CODE_NAME} MixOSBuild ${DEBIAN_LOCATION}
	sudo cp "/usr/bin/${QEMU_STATIC}" MixOSBuild/usr/bin/
	if [[ "${ENABLE_CACHE}" == "y" ]]; then
	  echo 'Acquire::http::proxy "http://127.0.0.1:3142";' | sudo tee MixOSBuild/etc/apt/apt.conf.d/99proxy
	fi
	sudo chroot MixOSBuild/ apt-get -y install ccache eatmydata
	sudo chroot MixOSBuild/ eatmydata /debootstrap/debootstrap --second-stage

	if [[ -z "${USERSPACE_ARCH:-}" && "${BUILD_ARMHF}" == "y" ]]; then
	  # Enable armhf architecture and update
	  sudo chroot MixOSBuild/ dpkg --add-architecture armhf
	  sudo chroot MixOSBuild/ eatmydata apt-get -y update
	  sudo chroot MixOSBuild/ eatmydata apt-get -y install libc6:armhf liblzma5:armhf libasound2t64:armhf libfreetype6:armhf libxkbcommon-x11-0:armhf libudev1:armhf libudev0:armhf libgbm1:armhf libstdc++6:armhf
	fi
		sudo cat MixOSBuild/etc/os-release | grep "^DEBIAN_VERSION_FULL=" | cut -d'=' -f2 > "${ROOTFS_CACHE}.commit"
		sudo tar -cpzf "${ROOTFS_CACHE}.tar.gz" MixOSBuild/
fi

# ── Bind the host filesystems into the chroot, ONE WAY ───────────────────────
#
# --rbind followed by --make-rslave, not --bind.  Ubuntu mounts / shared, so a plain
# bind puts the copy in the SAME peer group as the original: anything mounted underneath
# it inside the chroot propagates straight back out onto the host's own directory.
#
# That is not a theoretical concern here, it is what the next line used to do.  Mounting
# a `newinstance' devpts on MixOSBuild/dev/pts, through a shared bind of /dev, landed a
# second devpts instance on the BUILD MACHINE's /dev/pts -- and from that moment every
# sudo on the machine died with "unable to allocate pty: No such device", including the
# ones this script had not run yet.  Nothing unmounted it either, so the VM stayed
# broken across builds and looked like hardware.  `chroot ... mount -t proc proc /proc'
# did the milder version of the same thing, stacking one more proc on the host's /proc
# per run, and the matching `umount /proc' at the end took one of the HOST's layers off.
#
# rslave keeps propagation one-way: mounts the host makes still appear inside the
# chroot, mounts the chroot makes stay in the chroot.  And --rbind carries /dev/pts and
# /dev/shm across with /dev, so there is nothing left for a separate devpts mount to do.
#
# A VM already in the broken state cannot be repaired from here -- the stray mounts are
# not this tree's to find.  Restart it.
sudo mount --rbind /dev MixOSBuild/dev
sudo mount --make-rslave MixOSBuild/dev
sudo mount --rbind /proc MixOSBuild/proc
sudo mount --make-rslave MixOSBuild/proc
sudo mount --rbind /sys MixOSBuild/sys
sudo mount --make-rslave MixOSBuild/sys
echo -e "nameserver 8.8.8.8\nnameserver 1.1.1.1" | sudo tee MixOSBuild/etc/resolv.conf > /dev/null

# Avoid service autostarts
echo "exit 101" | sudo tee MixOSBuild/usr/sbin/policy-rc.d > /dev/null
sudo chmod 0755 MixOSBuild/usr/sbin/policy-rc.d
# `chroot MixOSBuild/ mount -t proc proc /proc' was here.  /proc is rbound above, so it
# was already there; see the propagation note for what the extra layer landed on.

# Install base runtime packages
sudo chroot MixOSBuild/ eatmydata apt-get -y update
sudo chroot MixOSBuild/ eatmydata apt-get -y upgrade
sudo chroot MixOSBuild/ eatmydata apt-get install -y btrfs-progs initramfs-tools sudo evtest network-manager systemd-sysv locales locales-all ssh dosfstools fluidsynth
sudo chroot MixOSBuild/ eatmydata apt-get install -y python3 python3-pip
sudo sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' MixOSBuild/etc/locale.gen
echo 'LANG="en_US.UTF-8"' | sudo tee -a MixOSBuild/etc/default/locale > /dev/null
echo -e "export LC_All=en_US.UTF-8" | sudo tee -a MixOSBuild/root/.bashrc > /dev/null
sudo chroot MixOSBuild/ bash -c "update-locale LANG=en_US.UTF-8"
sudo chroot MixOSBuild/ bash -c "locale-gen"
sudo chroot MixOSBuild/ systemctl enable NetworkManager

# Install libmali, DRM, and GBM libraries for ${CHIPSET}
sudo chroot MixOSBuild/ eatmydata apt-get install -y libdrm-dev libgbm1

setup_virtua_user
sleep 10
echo -e "Generating /etc/fstab"
# p3 is ext2 labelled DATA, not vfat labelled EASYROMS -- see setup_partition.sh.  No
# umask/uid/gid on that line: mount refuses them on ext2, so carrying them over would
# have been a home partition that never comes up.  Ownership is real instead, set once
# by finishing_touches.sh to the virtua user setup_virtua_user just created.
#
# ${DATA_MOUNT_POINT} is /home/virtua and is that user's home directory, so a login
# lands on p3.  It carries nofail: the home partition is not worth holding
# local-fs.target for, and a card whose p3 is missing still has to reach a shell.
echo -e "LABEL=ROOTFS / ${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_MOUNT_OPTIONS} 0 1
LABEL=BOOT /boot vfat defaults 0 0
LABEL=${DATA_LABEL} ${DATA_MOUNT_POINT} ${DATA_FILESYSTEM_FORMAT} ${DATA_MOUNT_OPTIONS} 0 2" | sudo tee MixOSBuild/etc/fstab
echo -e "Generating 10-standard.rules for udev"
echo -e "# Rules
KERNEL==\"mali0\", GROUP=\"video\", MODE=\"0660\"
KERNEL==\"rga\", GROUP=\"video\", MODE=\"0660\"
ACTION==\"add\", SUBSYSTEM==\"backlight\", RUN+=\"/bin/chgrp video /sys/class/backlight/%k/brightness\"
ACTION==\"add\", SUBSYSTEM==\"backlight\", RUN+=\"/bin/chmod g+w /sys/class/backlight/%k/brightness\"" | sudo tee MixOSBuild/etc/udev/rules.d/10-standard.rules
echo -e "Generating 40-usb_modeswitch.rules for udev"
echo -e "# Rules
ACTION!=\"add|change\", GOTO=\"end_modeswitch\"

# Atheros Wireless / Netgear WNDA3200
ATTRS{idVendor}==\"0cf3\", ATTRS{idProduct}==\"20ff\", RUN+=\"/usr/bin/eject '/dev/%k'\"

# Realtek RTL8821CU chipset 802.11ac NIC
#   initial cdrom mode 0bda:1a2b, wlan mode 0bda:c811
# Odroid WiFi Module 5B
#   initial cdrom mode 0bda:1a2b, wlan mode 0bda:c820
ATTR{idVendor}==\"0bda\", ATTR{idProduct}==\"1a2b\", RUN+=\"/usr/sbin/usb_modeswitch -K -v 0bda -p 1a2b\"
ATTR{idVendor}==\"0bda\", ATTR{idProduct}==\"c811\", RUN+=\"/usr/sbin/usb_modeswitch -K -v 0bda -p c811\"

LABEL=\"end_modeswitch\"" | sudo tee MixOSBuild/etc/udev/rules.d/40-usb_modeswitch.rules
sudo chroot MixOSBuild/ sync
sleep 5
# The matching `chroot MixOSBuild/ umount /proc' is gone with the mount above.  The
# later stages want /proc there, remove_mixosbuild in utils.sh is what takes these down,
# and while the bind was shared this line was unmounting a layer of the host's /proc.

# ── POSTCONDITION: DID ANY OF THAT ACTUALLY LAND? ────────────────────────────
#
# run_stage marks `bootstrap.done' from this file's exit status, and a sourced script's
# exit status is whatever its last command returned.  That last command was `sleep 5',
# which always returns 0 -- so this stage reported success unconditionally, including
# the run where sudo stopped working at line 60 and every command after it failed.  What
# that recorded as a completed Debian bootstrap was the unpacked debootstrap tarball and
# nothing else: no fstab, no login user, none of the base packages.  Every later stage
# skipped bootstrap on the strength of that file, and the image would have come out with
# a partition that mounts and a system that cannot boot.
#
# So the stage ends by checking for the things it exists to produce, and names the
# missing one.  Four cheap tests against a two-hour stage.
bootstrap_verify() {
  local missing=()

  if ! sudo test -s MixOSBuild/etc/fstab || \
     sudo grep -q 'UNCONFIGURED FSTAB' MixOSBuild/etc/fstab; then
    missing+=("a generated /etc/fstab")
  fi
  if ! sudo chroot MixOSBuild/ id -u virtua >/dev/null 2>&1; then
    missing+=("the virtua login user")
  fi
  if ! sudo chroot MixOSBuild/ bash -c 'command -v nmcli' >/dev/null 2>&1; then
    missing+=("network-manager")
  fi
  if ! sudo test -f MixOSBuild/etc/udev/rules.d/10-standard.rules; then
    missing+=("the 10-standard.rules udev file")
  fi

  if (( ${#missing[@]} )); then
    echo "Bootstrap did not finish.  The rootfs is missing: ${missing[*]}" >&2
    echo "Nothing downstream can be trusted, so this stage is not being marked done." >&2
    return 1
  fi
  echo "Bootstrap verified: fstab, the virtua user and the base packages are on the rootfs."
  return 0
}
bootstrap_verify
