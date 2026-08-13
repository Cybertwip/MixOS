#!/bin/bash

# Create boot.ini
#
# 0x01100000 is the RG351MP's initrd load address, and it is a literal now because
# UNIT is one.  device/r36-ultra/build-in-vm.sh sets UNIT=rg351mp and is the only thing
# that runs this file; the other branch held 0x04000000 for the eleven boards whose
# build scripts and Makefile targets are gone.
INITRD_LOADERADDRESS="0x01100000"
cat <<EOF | sudo tee ${mountpoint}/boot.ini
odroidgoa-uboot-config

setenv bootargs "root=${BOOT_ROOT_SPEC:-/dev/mmcblk0p2} rootwait rw fsck.repair=yes net.ifnames=0 fbcon=rotate:${SCREEN_ROTATION} console=/dev/ttyFIQ0 quiet splash consoleblank=0 vt.global_cursor_default=0"

# Booting
setenv loadaddr "0x02000000"
setenv initrd_loadaddr "${INITRD_LOADERADDRESS}"
setenv dtb_loadaddr "0x01f00000"

load mmc 1:1 \${loadaddr} Image
load mmc 1:1 \${initrd_loadaddr} uInitrd

load mmc 1:1 \${dtb_loadaddr} ${BOOT_KERNEL_DTB:-${KERNEL_DTB}}

booti \${loadaddr} \${initrd_loadaddr} \${dtb_loadaddr}
EOF

# logos/rotated/ went with it: the RGB10 and RK2020 panels were mounted a quarter turn
# round, so they got a pre-rotated copy of the same bitmap.  Neither board is built here.
sudo cp logos/unrotated/logo.bmp ${mountpoint}/

if [ -d "optional" ]; then
  if [ ! -z "$(find optional/ -mindepth 1 -maxdepth 1)" ]; then
    sudo cp optional/* ${mountpoint}/
  fi
fi

# Tell systemd to ignore PowerKey presses.  Let the Global Hotkey daemon handle that
echo "HandlePowerKey=ignore" | sudo tee -a MixOSBuild/etc/systemd/logind.conf

# Add some important exports to .bashrc for user virtua
echo "export PATH=\"\$PATH:/usr/sbin\"" | sudo tee -a MixOSBuild${DATA_MOUNT_POINT}/.bashrc
sudo chroot MixOSBuild/ bash -c "chown virtua:virtua ${DATA_MOUNT_POINT}/.bashrc"

# Set the name in the hostname and add it to the hosts file.  The `rg' prefix branch was
# for the RG353x and RG503, whose UNIT strings were bare numbers; those boards are gone.
NAME="${UNIT}"
echo "$NAME" | sudo tee MixOSBuild/etc/hostname
echo -e "# This host address\n127.0.1.1\t${NAME}" | sudo tee -a MixOSBuild/etc/hosts

# Copy the necessary .asoundrc file for proper audio
sudo cp audio/.asoundrc MixOSBuild${DATA_MOUNT_POINT}/.asoundrc
sudo cp audio/.asoundrcbak MixOSBuild${DATA_MOUNT_POINT}/.asoundrcbak
sudo chroot MixOSBuild/ bash -c "chown virtua:virtua ${DATA_MOUNT_POINT}/.asoundrc*"
sudo chroot MixOSBuild/ bash -c "ln -sfv ${DATA_MOUNT_POINT}/.asoundrc /etc/asound.conf"
sudo chroot MixOSBuild/ bash -c "cp -fv /usr/share/alsa/alsa.conf /usr/share/alsa/alsa.conf.mednafen"
sudo chroot MixOSBuild/ bash -c "sed -i '/\"\~\/.asoundrc\"/s//\"\~\/.asoundrc.mednafen\"/' /usr/share/alsa/alsa.conf.mednafen"
sudo chroot MixOSBuild/ bash -c "cp -fv /usr/share/alsa/alsa.conf /usr/share/alsa/alsa.conf.gametank"
sudo chroot MixOSBuild/ bash -c "sed -i '/\"\~\/.asoundrc\"/s//\"\~\/.asoundrc.gametank\"/' /usr/share/alsa/alsa.conf.gametank"

# Sleep script
sudo mkdir -p MixOSBuild/usr/lib/systemd/system-sleep
sudo cp scripts/sleep.${CHIPSET} MixOSBuild/usr/lib/systemd/system-sleep/sleep
sudo chmod 777 MixOSBuild/usr/lib/systemd/system-sleep/sleep

# Set performance governor to ondemand on boot
sudo chroot MixOSBuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/perfnorm quiet &\") | crontab -"

# Speaker Toggle to set audio output to SPK on boot
sudo mkdir -p MixOSBuild/usr/local/bin
sudo cp scripts/spktoggle.sh MixOSBuild/usr/local/bin/
sudo chmod 777 MixOSBuild/usr/local/bin/spktoggle.sh
sudo chroot MixOSBuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/spktoggle.sh &\") | crontab -"
sudo cp scripts/audiopath.service MixOSBuild/etc/systemd/system/audiopath.service
sudo cp scripts/audiostate.service MixOSBuild/etc/systemd/system/audiostate.service
sudo chroot MixOSBuild/ bash -c "systemctl enable audiopath"
sudo chroot MixOSBuild/ bash -c "systemctl enable audiostate"

# Copy necessary tools for expansion of ROOTFS and convert fat32 games partition to exfat on initial boot
sudo cp scripts/expandtoexfat.sh.${CHIPSET} ${mountpoint}/expandtoexfat.sh
sudo cp scripts/firstboot.sh ${mountpoint}/firstboot.sh
sudo cp scripts/firstboot.service MixOSBuild/etc/systemd/system/firstboot.service
sudo chroot MixOSBuild/ bash -c "systemctl enable firstboot"

# Add hotkeydaemon service and python script
sudo cp hotkeydaemon/killer_daemon.service MixOSBuild/etc/systemd/system/killer_daemon.service
sudo cp hotkeydaemon/killer_daemon.py MixOSBuild/usr/local/bin/killer_daemon.py
sudo chmod 777 MixOSBuild/usr/local/bin/killer_daemon.py
sudo chroot MixOSBuild/ bash -c "systemctl disable killer_daemon"

# Generate the fstab firstboot installs after it has grown the partitions.  Still
# called fstab.exfat because that is the name expandtoexfat.sh copies from /boot, and
# renaming a file two scripts agree on is not worth the churn -- but nothing about it is
# exfat any more: p3 is ext2 labelled DATA, mounted with real ownership rather than
# vfat's umask/uid/gid, which mount would refuse on ext2 outright.
#
# It is mounted at ${DATA_MOUNT_POINT} -- /home/virtua, the login user's home directory
# -- and not at /roms.  The tools bind mount below therefore reaches one level further
# in, at the legacy roms/ tree inside that partition, and names the real path rather
# than going through the /roms compatibility symlink: a bind mount is the one place
# where resolving through a symlink to a not-yet-mounted partition can order wrong.
if [ "$ROOT_FILESYSTEM_FORMAT" == "btrfs" ]; then
  ROOT_FILESYSTEM_MOUNT_OPTIONS="${ROOT_FILESYSTEM_MOUNT_OPTIONS},ssd_spread"
fi
cat <<EOF | sudo tee ${mountpoint}/fstab.exfat
LABEL=ROOTFS / ${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_MOUNT_OPTIONS} 0 0

LABEL=BOOT /boot vfat defaults 0 2
LABEL=${DATA_LABEL} ${DATA_MOUNT_POINT} ${DATA_FILESYSTEM_FORMAT} ${DATA_MOUNT_OPTIONS} 0 2
${DATA_MOUNT_POINT}/roms/tools /opt/system/Tools none nofail,x-systemd.device-timeout=7,bind
EOF

# Disable getty on tty0 and tty1
sudo chroot MixOSBuild/ bash -c "systemctl disable getty@tty0.service getty@tty1.service"

# Disable some other unneeded services
sudo chroot MixOSBuild/ bash -c "systemctl disable ModemManager polkit"

# Disable ssh service from automatically starting
sudo chroot MixOSBuild/ bash -c "systemctl disable ssh"

# Update Messaage of the Day
sudo cp -f scripts/00-header MixOSBuild/etc/update-motd.d/00-header
sudo cp -f scripts/10-help-text MixOSBuild/etc/update-motd.d/10-help-text
sudo rm -f MixOSBuild/etc/motd
sudo chmod 777 MixOSBuild/etc/update-motd.d/*

# Disable some unneeded interfaces in NetworkManager
cat <<EOF | sudo tee -a MixOSBuild/etc/NetworkManager/NetworkManager.conf

[device]
wifi.scan-rand-mac-address=no

[keyfile]
unmanaged-devices=interface-name:p2p0;interface-name:ap0
EOF

# Remove requirement of sudo for controlling nmcli
cat <<EOF | sudo tee -a MixOSBuild/etc/polkit-1/rules.d/10-networkmanager.rules
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager") == 0 &&
        subject.isInGroup("netdev")) {
        return polkit.Result.YES;
    }
});
EOF

# Default set timezone to New York
sudo chroot MixOSBuild/ bash -c "ln -sf /usr/share/zoneinfo/America/New_York /etc/localtime"

# fetch_compat_libs.sh used to run here: nineteen wget calls pulling Debian 10 and 11
# library sonames out of snapshot.debian.org so PortMaster's prebuilt ports could find
# the libavcodec, libx264 and libssh versions they were linked against.  Nothing on this
# image is linked against any of them now that the ports and the emulators are gone, so
# what it installed was nineteen stale shared objects and a hard build dependency on
# archive.debian.org staying up.
#
# Various tools available through Options added here.  Only the ones that do something
# on an image with no front end and no emulators: the rest of MixOS_Tools drove
# RetroArch, Drastic, PPSSPP, ECWolf and EmulationStation's collections, and is gone.
#
# /opt/system/Advanced is not created any more either.  It was the second level of the
# front end's Options menu, and the only things that ever landed in it were the emulator
# settings tools and a few self-replacing toggles that copy their own opposite in from
# /usr/local/bin -- toggles for devices this tree no longer builds.
sudo mkdir -p MixOSBuild/opt/system
system_tools=(
  "Change Password.sh"
  "Disable Remote Services.sh"
  "Enable Remote Services.sh"
  "Network Info.sh"
  "Remove ._ Files.sh"
  "System Info.sh"
  "USB Drive Mount.sh"
  "USB Drive Unmount.sh"
  "Update.sh"
  "Wifi.sh"
)
for tool in "${system_tools[@]}"; do
  sudo cp "MixOS_Tools/$tool" MixOSBuild/opt/system/
done
sudo chroot MixOSBuild/ bash -c "chown -R virtua:virtua /opt"
sudo chmod -R 777 MixOSBuild/opt/system/

# Copy performance scripts
sudo cp scripts/perf* MixOSBuild/usr/local/bin/

# Add preservation of SDL_VIDEO_EGL_DRIVER to sudoers
cat <<EOF | sudo tee MixOSBuild/etc/sudoers.d/virtua_preserve_sdl_video_egl_driver
Defaults        env_keep += "SDL_VIDEO_EGL_DRIVER"
EOF
sudo chmod 0440 MixOSBuild/etc/sudoers.d/virtua_preserve_sdl_video_egl_driver

# Add USB DAC Support
echo -e "Generating 20-usb-alsa.rules udev for usb dac support"
echo -e "KERNEL==\"controlC[0-9]*\", DRIVERS==\"usb\", SYMLINK=\"snd/controlC7\"" | sudo tee MixOSBuild/etc/udev/rules.d/20-usb-alsa.rules
sudo chroot MixOSBuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/checknswitchforusbdac.sh &\") | crontab -"

# Disable requirement for sudo for setting niceness
echo "virtua              -       nice            -20" | sudo tee -a MixOSBuild/etc/security/limits.conf

# Copy various other backend tools
sudo cp scripts/checkbrightonboot MixOSBuild/usr/local/bin/
sudo cp scripts/current_* MixOSBuild/usr/local/bin/
sudo cp scripts/finish.sh MixOSBuild/usr/local/bin/
sudo cp scripts/pause.sh MixOSBuild/usr/local/bin/
sudo cp scripts/speak_bat_life.sh MixOSBuild/usr/local/bin/
sudo cp scripts/spktoggle.sh MixOSBuild/usr/local/bin/
sudo cp scripts/timezones MixOSBuild/usr/local/bin/
# Quick Mode used to be installed here: BaRT_QuickMode.sh, its Enable/Disable pair,
# .qm and .orig copies of finish.sh and pause.sh, get_last_played.sh, and the
# isitpng.sh/wasitpng.sh hooks that ES ran at game-start and game-end.  All of it
# existed to boot straight into the last game instead of into EmulationStation, and
# it read that game out of ~/.emulationstation.  With no front end on this image
# there is no last game to read and nothing to skip past, so the whole cluster is
# gone rather than left installed and inert.
sudo cp scripts/ap_mode.sh MixOSBuild/usr/local/bin/
sudo cp scripts/auto_suspend* MixOSBuild/usr/local/bin/
sudo cp scripts/processcheck.sh MixOSBuild/usr/local/bin/
sudo cp scripts/autosuspend.service MixOSBuild/etc/systemd/system/
sudo chroot MixOSBuild/ bash -c "pip install --break-system-packages --root-user-action ignore inputs"
sudo chroot MixOSBuild/ bash -c "systemctl disable autosuspend"
sudo cp scripts/wifi_importer.service MixOSBuild/etc/systemd/system/
sudo chroot MixOSBuild/ bash -c "systemctl enable wifi_importer"
sudo cp scripts/keystroke.py MixOSBuild/usr/local/bin/
# Seven emulator launcher wrappers went here -- amiga.sh, b2.sh, freej2me.sh,
# easyrpg.sh, gx4000.sh, neogeocd.sh and netplay.sh.  Each one existed to be
# exec'd by the front end with a rom path, and each one ended in a RetroArch
# command line naming a libretro core that is no longer built.
sudo mkdir -p MixOSBuild/etc/hostapd
sudo cp hostapd/hostapd.conf MixOSBuild/etc/hostapd/
sudo cp dnsmasq/dnsmasq.conf MixOSBuild/etc/
sudo cp scripts/sleep_governors.sh MixOSBuild/usr/local/bin/
sudo cp global/* MixOSBuild/usr/local/bin/
# Disable winbind as connectivity to Active Directory is not needed
sudo chroot MixOSBuild/ bash -c "systemctl disable winbind"
# Disable samba-ad-dc as connectivity to Active Directory is not needed as well as some other services
sudo chroot MixOSBuild/ bash -c "systemctl disable samba-ad-dc dnsmasq hostapd"
# Disable e2scrub_reap if ext file system is not being used for rootfs
if [ "$ROOT_FILESYSTEM_FORMAT" == "xfs" ] || [ "$ROOT_FILESYSTEM_FORMAT" == "btrfs" ]; then
  sudo chroot MixOSBuild/ bash -c "systemctl disable e2scrub_reap"
fi
# Set the default graphical target to multi-user instead of graphical"
sudo chroot MixOSBuild/ bash -c "systemctl set-default multi-user.target"
# The RG351MP board scripts: brightness, volume, the battery LED and its two colour
# states.  This used to be a four-way branch -- device/rgb10 for the RGB10, device/a10mini
# for the A10 Mini's inverted LED naming (Green became Orange, Red became Blue, applied by
# sed to copies of the RG351MP scripts), and scripts/g350 for the G350's boot logo unit.
# All three trees are gone with their build scripts, and UNIT is rg351mp for every build
# this file can be reached from, so the branch is one path now.
sudo cp device/rg351mp/*.sh MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/*.py MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/*.green MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/*.red MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/fix_power_led MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/checkbrightonboot MixOSBuild/usr/local/bin/
sudo cp device/rg351mp/"Change LED to Red.sh" MixOSBuild/opt/system/
sudo chroot MixOSBuild/ bash -c "chown -R virtua:virtua /opt"
sudo chmod 777 MixOSBuild/opt/system/"Change LED to Red.sh"
sudo cp device/rg351mp/*.service MixOSBuild/etc/systemd/system/
sudo chroot MixOSBuild/ bash -c "systemctl enable 351mp batt_led"

# Make all scripts in /usr/local/bin executable, world style
sudo chmod 777 MixOSBuild/usr/local/bin/*

# Three symlink sets used to be written here -- /etc/emulationstation/themes,
# ~/.emulationstation/themes and ~/.emulationstation/music, all pointing into the
# rom partition.  There is no front end on this image to read any of them, so they
# were three dangling links and a directory tree created for their sake.

# A .GameLoadingIModePIC or .GameLoadingIModeASCII stamp was dropped here to tell the
# game launcher which kind of loading screen to draw between the front end and the
# emulator.  There is neither a front end nor an emulator on this image, so nothing ever
# reads the stamp and there is no moment for it to describe.

# Set default volume
sudo cp audio/asound.state.${CHIPSET} MixOSBuild/var/local/asound.state

# Set SDL Video Driver for bash
echo "export SDL_VIDEO_EGL_DRIVER=libEGL.so" | sudo tee MixOSBuild/etc/profile.d/SDL_VIDEO.sh

# Set device name 
dNAME=`echo $NAME | tr '[:lower:]' '[:upper:]'`
echo "$dNAME" | sudo tee MixOSBuild${DATA_MOUNT_POINT}/.config/.DEVICE

# Configure default samba share setup.  The real path, not /roms: that is a symlink into
# the DATA partition now, and samba defaults to "wide links = no" -- it refuses to follow
# a symlink that leaves the share, so a share rooted on one is a share that shows nothing.
cat <<EOF | sudo tee -a MixOSBuild/etc/samba/smb.conf
[roms]
   comment = ROMS
   path = ${DATA_MOUNT_POINT}/roms
   browsable = yes
   read only = no
   map archive = no
   map system = no
   map hidden = no
   guest ok = yes
   read list = guest

[opt]
   comment = OPT
   path = /opt
   browsable = yes
   read only = no
   map archive = no
   map system = no
   map hidden = no
   guest ok = yes
   read list = guest

[home]
   comment = HOME
   path = ${DATA_MOUNT_POINT}
   browsable = yes
   read only = no
   map archive = no
   map system = no
   map hidden = no
   guest ok = yes
   read list = guest
EOF
if [[ "$UNIT" != *"rgb10"* ]] && [[ "$UNIT" != "rk2020" ]] && [[ "$UNIT" != *"oga"* ]]; then
  cat <<EOF | sudo tee -a MixOSBuild/etc/samba/smb.conf
[roms2]
   comment = ROMS2
   path = /roms2
   browsable = yes
   read only = no
   map archive = no
   map system = no
   map hidden = no
   guest ok = yes
   read list = guest
EOF
  fi

sudo chroot MixOSBuild/ bash -c "systemctl disable smbd"
sudo chroot MixOSBuild/ bash -c "systemctl disable nmbd"

# Set distro identification and version
sudo mkdir -p MixOSBuild/usr/share/plymouth/themes/
cat <<EOF | sudo tee MixOSBuild/usr/share/plymouth/themes/text.plymouth
title=MixOS (${BUILD_DATE})
EOF
echo "${BUILD_DATE}" | sudo tee MixOSBuild${DATA_MOUNT_POINT}/.config/.VERSION

# Set boot up welcome text with distro and version
sudo cp scripts/boot_text.sh MixOSBuild/usr/local/bin/
sudo chmod 777 MixOSBuild/usr/local/bin/boot_text.sh
sudo cp scripts/welcome-message.service MixOSBuild/etc/systemd/system/welcome-message.service
sudo chroot MixOSBuild/ bash -c "systemctl enable welcome-message"

# Mark completed MixOS updates with this current build
release_tags=( $(git -c 'versionsort.suffix=-' ls-remote --tags --sort='v:refname' https://github.com/christianhaitian/darkos-updates.git | cut -d/ -f3- | sed 's/^v//I') )
if [[ ! -z "$release_tags" ]]; then
  for release_tag in "${release_tags[@]}"
  do
    sudo touch MixOSBuild${DATA_MOUNT_POINT}/.config/.update${release_tag}
  done
fi

# Set the owner of the home directory and everything under it to the login user
sudo chroot MixOSBuild/ bash -c "chown -R virtua:virtua ${DATA_MOUNT_POINT}"

# /tempthemes was six git clones of front-end themes, staged on the rootfs for
# firstboot to untar onto the rom partition.  Nothing reads a theme on this image, so
# the clones are gone and with them six network fetches in the middle of a build.
# The directory itself stays: firstboot.sh moves it if it is there and says nothing
# if it is not, and an empty one keeps that path the same on every image.
sudo mkdir -p MixOSBuild/tempthemes

sync
sudo umount -l ${mountpoint}
sudo losetup -d ${LOOP_BOOT}

# Format rootfs partition in final image
#
# --sizelimit, like the DATA branch below: without it the loop device runs to the end of
# the image, and mkfs then lays inode tables and group descriptors across the DATA
# partition as well.  Today that is survivable only because the DATA mkfs.ext2 a few
# lines down happens afterwards and wipes them; it is not a property worth depending on.
ROOTFS_PART_OFFSET=$((STORAGE_PART_START * 512))
ROOTFS_PART_SIZE_BYTES=$(( (STORAGE_PART_END - STORAGE_PART_START + 1) * 512 ))
LOOP_ROOTFS=$(sudo losetup --find --show --offset ${ROOTFS_PART_OFFSET} --sizelimit ${ROOTFS_PART_SIZE_BYTES} ${DISK})
sudo mkfs.${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_FORMAT_PARAMETERS} ${LOOP_ROOTFS}
sudo losetup -d ${LOOP_ROOTFS}

# Format ROMS partition in final image
ROM_PART_OFFSET=$((ROM_PART_START * 512))
ROM_PART_SIZE_BYTES=$(( (ROM_PART_END - ROM_PART_START + 1) * 512 ))
LOOP_ROM=$(sudo losetup --find --show --offset ${ROM_PART_OFFSET} --sizelimit ${ROM_PART_SIZE_BYTES} ${DISK})
if [ -z "$LOOP_ROM" ]; then
  echo "❌ Failed to create loop device for ROMS partition!"
  echo "ROM_PART_START: $ROM_PART_START"
  echo "ROM_PART_END: $ROM_PART_END"
  echo "ROM_PART_OFFSET: $ROM_PART_OFFSET"
  echo "ROM_PART_SIZE_BYTES: $ROM_PART_SIZE_BYTES"
  exit 1
fi
# ext2 and labelled DATA, not vfat and EASYROMS.  This partition holds Linux content --
# roms, bios, themes, tools, and whatever is dropped on it from a PC -- and the old flow
# formatted it vfat here only for firstboot to reformat it exfat on the device and untar
# /roms.tar back onto it.  Formatting it as its final filesystem here removes that whole
# round trip.  ${DATA_FILESYSTEM_FORMAT_PARAMETERS} carries the label.
sudo mkfs.${DATA_FILESYSTEM_FORMAT} ${DATA_FILESYSTEM_FORMAT_PARAMETERS} ${LOOP_ROM}
# The partition mounts at ${DATA_MOUNT_POINT} -- /home/virtua -- on the device, so its
# root is a home directory and the rom library is one directory inside it.  Here it is
# mounted at mnt/data and ${fat32_mountpoint} points one level in, at that library:
# every line below writes through that variable and none of them has to change.
#
# The variable name is left alone on purpose: it is read a few dozen times below and
# renaming it would be churn in exchange for nothing.  It is not fat32 any more, and it
# is not the partition root either.
data_mountpoint=mnt/data
mkdir -p ${data_mountpoint}
sudo mount -t ${DATA_FILESYSTEM_FORMAT} ${LOOP_ROM} ${data_mountpoint}
fat32_mountpoint=${data_mountpoint}/roms
sudo mkdir -p ${fat32_mountpoint}

# /roms on the rootfs becomes a symlink into the home partition.  The RK3326 scripts,
# the samba share and firstboot still say /roms, and they keep resolving through this;
# nothing has to be renamed to move the partition.
# If an earlier stage already put real files in MixOSBuild/roms they are moved onto the
# partition rather than lost, because replacing a populated directory with a symlink
# would silently drop whatever was in it.
if [[ -d MixOSBuild/roms && ! -L MixOSBuild/roms ]]; then
  if [[ -n "$(sudo ls -A MixOSBuild/roms 2>/dev/null)" ]]; then
    echo -e "Moving the existing MixOSBuild/roms contents onto the DATA partition\n"
    sudo cp -a MixOSBuild/roms/. ${fat32_mountpoint}/
  fi
  sudo rm -rf MixOSBuild/roms
fi
# Relative on purpose: an absolute target would make a host-side
# `cp something MixOSBuild/roms/...` write to /home/virtua/roms on the BUILD MACHINE.
# Stripping the leading slash makes it relative to whatever root it is read from, so it
# resolves to MixOSBuild${DATA_MOUNT_POINT}/roms here and /home/virtua/roms on the device.
sudo ln -sfn "${DATA_MOUNT_POINT#/}/roms" MixOSBuild/roms
# The media tree.  This used to be one directory per entry in game_systems.txt -- a
# hundred and some emulator folders, wolf/ and alg/ and scummvm/ and pico-8/carts/ --
# followed by a PortMaster installer, a ThemeMaster unzip, five pico-8 cartridges pulled
# from lexaloffle, a theme cloned from GitHub and the scanner scripts each application
# shipped.  Every one of those existed to be read by an emulator or by the front end
# that launched it, and both are gone; what is left is the handful of places the system
# itself writes to.  Seven mkdir calls in place of six network fetches.
media_directories=(backup bgmusic bios shutdownimages themes tools)
for directory in "${media_directories[@]}"; do
  echo -e "Creating ${fat32_mountpoint}/${directory}\n"
  sudo mkdir -p "${fat32_mountpoint}/${directory}"
done

# The one image still read at runtime: finish.sh and pause.sh play it on shutdown.
sudo cp shutdownimages/bye.gif ${fat32_mountpoint}/shutdownimages/
sync

# The home directory itself.  useradd -m in setup_virtua_user built this tree on the ROOTFS,
# under what is about to become a mount point: the skeleton, .config and the two
# locale lines in .bashrc.  Once p3 mounts over it none of that is reachable,
# so the same tree is copied onto the partition and the mounted and unmounted cases look
# alike.
if [[ -d "MixOSBuild${DATA_MOUNT_POINT}" ]]; then
  echo -e "Seeding ${DATA_MOUNT_POINT} on the DATA partition from the rootfs copy\n"
  sudo cp -a "MixOSBuild${DATA_MOUNT_POINT}/." ${data_mountpoint}/ || \
    echo "⚠️  Could not copy the whole home tree onto p3 -- ${ROM_PART_SIZE:-300} MB may be too small for what is in it"
fi

# The stamp the J36's initramfs looks for.  /init has to recognise this partition to
# leave it alone -- it is the one partition systemd mounts rw, and a read-only mount
# from the initramfs would make that fstab entry fail with EBUSY -- and it cannot
# identify it by label, having no blkid.  A file at the partition root is what it can
# read.  Contents are for whoever finds it with a card reader; only the name matters.
cat <<EOF | sudo tee ${data_mountpoint}/.mixos-home > /dev/null
This partition is the MixOS home directory: it mounts at ${DATA_MOUNT_POINT}.
Label ${DATA_LABEL}, ${DATA_FILESYSTEM_FORMAT}, owned by uid 1000.  roms/ inside it is
the legacy media tree, which /roms still points at.
Do not delete this file: the J36 Ultra initramfs reads it to tell this partition apart
from a plain data partition, and without it the boot mounts this one read-only.
EOF
sync

# Ownership and modes, BEFORE roms.tar is made, so the tar carries them too.
#
# chown is new here and vfat never needed it: vfat has no ownership at all, and the old
# fstab line handed the whole partition to uid 1000 with umask=000.  ext2 does have
# ownership, so it has to be set once, here, while the partition is a loop device --
# no mount option will do it afterwards.  1000:1000 is the virtua user, created by
# setup_virtua_user in bootstrap_rootfs.sh; numeric because this is the host's mount and
# the host has no such user.  775 so the virtua group can write and everyone can read.
#
# The whole partition and not just the rom library: its root is $HOME, and a home
# directory owned by root is a login that cannot write its own dotfiles.  chmod skips
# lost+found on purpose -- it is left at whatever mkfs made it.
sudo chown -R 1000:1000 ${data_mountpoint}
sudo find ${data_mountpoint} -mindepth 1 -name lost+found -prune -o -print0 | \
  sudo xargs -0 --no-run-if-empty chmod 775
sync

# roms.tar, kept for firstboot on a card whose p3 it had to recreate.  With p3 already
# ext2 there is nothing to restore in the normal case, and firstboot only reaches for
# this if it repartitioned.  Run as root, so the 1000:1000 set above is what goes into
# the archive; the matching half is on the extract side, in expandtoexfat.sh, which used
# to pass --no-same-owner because the destination was exfat and could not hold it.
#
# The archive is now the whole partition -- the home directory with roms/ inside it --
# rather than a single roms/ top level, so firstboot extracts it AT ${DATA_MOUNT_POINT}
# and not at /.  lost+found is excluded: the recreated filesystem has its own.
sudo tar -C ${data_mountpoint} --exclude=./lost+found -cvf MixOSBuild/roms.tar .

sudo umount ${data_mountpoint}
sudo losetup -d ${LOOP_ROM}
sudo rm -rf ${data_mountpoint}
