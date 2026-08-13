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
    sudo tar -xvzpf "${ROOTFS_CACHE}.tar.gz"
else
	if [[ "${ENABLE_CACHE}" == "y" ]]; then
	  export DEBIAN_LOCATION="http://127.0.0.1:3142/deb.debian.org/debian/"
	else
	  export DEBIAN_LOCATION="http://deb.debian.org/debian/"
	fi
	# Bootstrap base system
	sudo eatmydata debootstrap --no-check-gpg --include=eatmydata --resolve-deps --arch=${DEBOOTSTRAP_ARCH} --foreign ${DEBIAN_CODE_NAME} MixOSBuild ${DEBIAN_LOCATION}
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
		sudo tar -cvpzf "${ROOTFS_CACHE}.tar.gz" MixOSBuild/
fi

# Bind essential host filesystems into chroot for networking
sudo mount --bind /dev MixOSBuild/dev
sudo mount -t devpts none MixOSBuild/dev/pts -o newinstance,ptmxmode=0666
#sudo mount --bind /dev/pts MixOSBuild/dev/pts -o newinstance,ptmxmode=0666
sudo mount --bind /proc MixOSBuild/proc
sudo mount --bind /sys MixOSBuild/sys
echo -e "nameserver 8.8.8.8\nnameserver 1.1.1.1" | sudo tee MixOSBuild/etc/resolv.conf > /dev/null

# Avoid service autostarts
echo "exit 101" | sudo tee MixOSBuild/usr/sbin/policy-rc.d > /dev/null
sudo chmod 0755 MixOSBuild/usr/sbin/policy-rc.d
sudo chroot MixOSBuild/ mount -t proc proc /proc

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
sudo chroot MixOSBuild/ umount /proc
