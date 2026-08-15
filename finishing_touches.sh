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

# ── firstboot: STAGED, AND DELIBERATELY NOT ENABLED ──────────────────────────
#
# firstboot.sh grew p3 to the end of the card, MKFS'D it, and untarred /roms.tar back
# onto what it had just erased, in two stages with a reboot in the middle.  Every part
# of that describes a layout that no longer exists: there is no p3, /home/virtua is a
# directory on the rootfs, and roms.tar is not built any more.
#
# LEAVING IT ENABLED WOULD BE DESTRUCTIVE, not merely useless.  What sits after p2 on a
# flashed card is unallocated space, and that space is what the initramfs gives to
# ROOTFS on the first boot -- the same space firstboot.sh would carve a fresh partition
# out of, whichever of the two ran first.  So it is disabled here rather than left to
# the J36's `systemd.mask=firstboot.service' bootarg, which only covers one of the two
# boards that boot this rootfs.
#
# The scripts still go onto the BOOT partition.  They are the recovery path for a card
# whose table has been damaged, they are readable from any machine that mounts FAT, and
# neither of them runs unless somebody runs it.
sudo cp scripts/expandtoexfat.sh.${CHIPSET} ${mountpoint}/expandtoexfat.sh
sudo cp scripts/firstboot.sh ${mountpoint}/firstboot.sh
sudo cp scripts/firstboot.service MixOSBuild/etc/systemd/system/firstboot.service
sudo chroot MixOSBuild/ bash -c "systemctl disable firstboot"

# Add hotkeydaemon service and python script
sudo cp hotkeydaemon/killer_daemon.service MixOSBuild/etc/systemd/system/killer_daemon.service
sudo cp hotkeydaemon/killer_daemon.py MixOSBuild/usr/local/bin/killer_daemon.py
sudo chmod 777 MixOSBuild/usr/local/bin/killer_daemon.py
sudo chroot MixOSBuild/ bash -c "systemctl disable killer_daemon"

# The spare copy of the fstab, on the BOOT partition, where a card reader can get at it.
#
# Still called fstab.exfat because that is the name expandtoexfat.sh copies from /boot
# and renaming a file two scripts agree on is not worth the churn -- but nothing about
# it is exfat any more, and as of this layout nothing about it is a third partition
# either.  The DATA line went with the partition it mounted (see setup_partition.sh);
# ${DATA_MOUNT_POINT} is a plain directory on the rootfs now.
#
# What it is FOR has changed with it.  firstboot.sh used to install this over /etc/fstab
# as its last act, after growing p3; that script is disabled a few lines up, because the
# partition it grew is gone and the initramfs grows ROOTFS instead.  So this file is a
# recovery copy: two lines, in plain text, on the one partition a Windows or macOS
# machine will mount, for the boot where /etc/fstab on p2 has been edited into something
# that does not come up.
if [ "$ROOT_FILESYSTEM_FORMAT" == "btrfs" ]; then
  ROOT_FILESYSTEM_MOUNT_OPTIONS="${ROOT_FILESYSTEM_MOUNT_OPTIONS},ssd_spread"
fi
cat <<EOF | sudo tee ${mountpoint}/fstab.exfat
LABEL=ROOTFS / ${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_MOUNT_OPTIONS} 0 0

LABEL=BOOT /boot vfat defaults 0 2
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
# THE /opt/system TOOL MENU IS GONE, all of it.  Ten scripts were installed here --
# Change Password, Enable/Disable Remote Services, Network Info, Remove ._ Files, System
# Info, USB Drive Mount/Unmount, Wifi and Update -- and not one of them could run on an
# image built from this tree.  They draw with `msgbox' and `osk' and drive themselves
# with `gptokeyb' reading /opt/inttools/keys.gptk: no live script has ever copied
# scripts/msgbox, scripts/osk or inttools/ onto the rootfs, gptokeyb is not in this
# repository at all, and the urwid .deb that osk.py imports was built for arm64 against
# an armhf userspace.  They were the front end's Options menu; mixdash does not open
# them, so the only way to reach one was to type its path at a terminal and watch it
# fail on the first line.
#
# Update.sh is the one worth naming.  It fetched dArkOSUpdate.sh from
# christianhaitian/darkos-updates and executed it, and the release-tag loop that used to
# sit further down this file pre-marked every tag that repository already had -- so a
# card would have applied the NEXT upstream dArkOS release, written for /home/ark and a
# front end this image does not have.  MixOS gets its own updater when there is a
# network stack to carry it.
#
# The directory itself stays because the LED scripts land in it -- "Change LED to Red.sh"
# is copied here a few lines down -- and for no other reason.  /opt/system/Tools used to
# be a bind mount of the roms tree's tools/ directory, which is where global/importwifi.sh
# looked for wifikeyfile.txt; the roms tree is gone and so is the bind, which leaves that
# script looking at a path that does not exist and doing nothing, which is what it does on
# any card without a key file anyway.
# /opt/system/Advanced is not created any more either.  It was the second level of the
# front end's Options menu, and the only things that ever landed in it were the emulator
# settings tools and a few self-replacing toggles that copy their own opposite in from
# /usr/local/bin -- toggles for devices this tree no longer builds.
sudo mkdir -p MixOSBuild/opt/system
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
# The access point configuration used to be installed here: hostapd/hostapd.conf into
# /etc/hostapd, dnsmasq/dnsmasq.conf into /etc, and scripts/ap_mode.sh above to bring the
# pair up on wlan0 with a static 192.168.1.1.  It came from ArkOS, where it existed for
# RetroArch's local netplay, and it shipped a fixed WPA passphrase -- the same one on
# every card ever built from this tree, in public git history.  There is no working
# network interface on this board yet, so what it amounted to was a credential handed out
# for a radio nothing can bring up.  The hostapd and dnsmasq PACKAGES are still in
# needed_packages.txt and are disabled below; only the configuration is gone, so a MixOS
# access point can be written against whatever the MT6592 WiFi ends up being.
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

# Unit files are data, not programs, and systemd says so on every boot that finds
# one with the executable bit set:
#
#   Configuration file /etc/systemd/system/wifi_importer.service is marked
#   executable.  Please remove executable permission bits.  Proceeding anyway.
#
# It proceeds, so this is cosmetic -- but it is cosmetic on the console during
# early boot, which on this board is the panel with the splash on it.  The bit
# came in on scripts/wifi_importer.service and `cp' preserved it; that file is
# 0644 in the tree now, and this normalises the whole directory so the next unit
# added with a stray +x does not put the line back.
sudo chmod 0644 MixOSBuild/etc/systemd/system/*.service

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

# Configure default samba share setup.  One share, and there used to be four.
#
# [roms] pointed at ${DATA_MOUNT_POINT}/roms and [roms2] at /roms2.  Both were named for
# a directory layout EmulationStation invented, the first is gone with that layout, and
# the second never existed on any image this tree builds -- it was a share on a path
# nothing ever created, which samba reports as a share that cannot be connected to.
# [home] already covers everything [roms] did: the partition mounts AT the home
# directory, so the whole of it is one share with the name the operator expects.
#
# [opt] is gone too, and that one was not merely useless.  It exported /opt -- system
# software, not user data -- guest-writable to every machine on the network.  Anything
# that could reach the console could replace a binary under /opt and the console would
# run it at the next boot, with no password asked at any point.  A share that hands out
# write access to the system's own programs is not a convenience, and nothing on this
# image needs /opt over the network: the data partition is what the operator copies
# games and media into, and that is [home].
#
# [home] IS STILL GUEST-WRITABLE, AND THAT IS THE DELIBERATE PART.  It is the share the
# Sharing page exists to turn on, on a handheld whose whole point is dragging files onto
# it from a desktop, and both smbd and nmbd ship disabled -- nothing is exported until
# somebody switches it on.  The exposure is the data partition and stops there.
cat <<EOF | sudo tee -a MixOSBuild/etc/samba/smb.conf
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

# The OTA bookkeeping was here: an ls-remote against christianhaitian/darkos-updates and
# a .update<tag> marker touched for every tag it already had, so /opt/system/Update.sh
# would skip the whole history and apply only what came after.  Both halves are gone --
# see the note at the tool block above -- and with them a network round trip in the
# middle of every build, against a repository this project does not control.

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
# --sizelimit is what makes the loop device stop where the partition stops.  Without it
# it runs to the end of the image file and mkfs sizes the filesystem for all of that,
# which used to mean inode tables laid across p3.  p3 is gone and the image now ends
# where p2 does, so the same mistake would be invisible here and would surface on the
# device -- as a superblock claiming more blocks than the partition has, on the one
# filesystem the initramfs is about to hand the rest of the card to.
ROOTFS_PART_OFFSET=$((STORAGE_PART_START * 512))
ROOTFS_PART_SIZE_BYTES=$(( (STORAGE_PART_END - STORAGE_PART_START + 1) * 512 ))
LOOP_ROOTFS=$(sudo losetup --find --show --offset ${ROOTFS_PART_OFFSET} --sizelimit ${ROOTFS_PART_SIZE_BYTES} ${DISK})
sudo mkfs.${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_FORMAT_PARAMETERS} ${LOOP_ROOTFS}
sudo losetup -d ${LOOP_ROOTFS}

# ── THE ROMS/DATA PARTITION, AND WHY THERE IS NOTHING HERE ANY MORE ──────────
#
# A hundred and twenty lines stood here.  They attached a loop device to p3, ran
# mkfs.ext2 on it with the DATA label, mounted it at mnt/data, copied the rootfs's
# /home/virtua onto it, wrote a .mixos-home stamp at its root for the J36 initramfs to
# recognise, chowned the lot to 1000:1000, and tarred it up as MixOSBuild/roms.tar for a
# firstboot that might one day have to recreate it.
#
# All of it existed to make a separate partition look like the home directory it was
# mounted over.  The partition is gone -- see setup_partition.sh -- so the home
# directory is simply the home directory: the tree useradd -m built on the ROOTFS, that
# every line above this one has been writing dotfiles into, owned by the virtua user by
# the chroot chown further up and reached by a login with nothing mounted at all.
#
# WHAT WENT WITH IT, so that a reader who greps for these names finds this note:
#
#   .mixos-home    the stamp.  It told the initramfs "this partition is the home
#                  partition, leave it alone for systemd to mount rw".  With one
#                  filesystem on the card there is nothing to tell apart.
#   roms.tar       the seed archive, extracted by firstboot after it had reformatted
#                  p3.  firstboot is disabled now and there is nothing to reformat.
#   mnt/data       the build-side mount point.  Nothing to mount.
#
# The last thing this file used to do to that partition -- chown -R 1000:1000 -- still
# happens, in the chroot, against ${DATA_MOUNT_POINT} on the rootfs.  It is the one part
# of the block that was about the home directory rather than about the partition.
