#!/bin/bash

BUILD_BUNDLED_APPS="${BUILD_BUNDLED_APPS:-y}"

# Create boot.ini
if [ "$UNIT" == "rg351mp" ]; then
  INITRD_LOADERADDRESS="0x01100000"
else
  INITRD_LOADERADDRESS="0x04000000"
fi
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

if [ "$UNIT" == "rgb10" ] || [ "$UNIT" == "rk2020" ]; then
  sudo cp logos/rotated/logo.bmp ${mountpoint}/
else
  sudo cp logos/unrotated/logo.bmp ${mountpoint}/
fi

if [ -d "optional" ]; then
  if [ ! -z "$(find optional/ -mindepth 1 -maxdepth 1)" ]; then
    sudo cp optional/* ${mountpoint}/
  fi
fi

# Tell systemd to ignore PowerKey presses.  Let the Global Hotkey daemon handle that
echo "HandlePowerKey=ignore" | sudo tee -a Arkbuild/etc/systemd/logind.conf

# Add some important exports to .bashrc for user ark
echo "export PATH=\"\$PATH:/usr/sbin\"" | sudo tee -a Arkbuild/home/ark/.bashrc
sudo chroot Arkbuild/ bash -c "chown ark:ark /home/ark/.bashrc"

# Set the name in the hostname and add it to the hosts file
if [[ "$UNIT" == *"353"* ]] || [[ "$UNIT" == *"503"* ]]; then
  NAME="rg${UNIT}"
else
  NAME="${UNIT}"
fi
echo "$NAME" | sudo tee Arkbuild/etc/hostname
echo -e "# This host address\n127.0.1.1\t${NAME}" | sudo tee -a Arkbuild/etc/hosts

# Copy the necessary .asoundrc file for proper audio in emulationstation and emulators
sudo cp audio/.asoundrc Arkbuild/home/ark/.asoundrc
sudo cp audio/.asoundrcbak Arkbuild/home/ark/.asoundrcbak
sudo chroot Arkbuild/ bash -c "chown ark:ark /home/ark/.asoundrc*"
sudo chroot Arkbuild/ bash -c "ln -sfv /home/ark/.asoundrc /etc/asound.conf"
sudo chroot Arkbuild/ bash -c "cp -fv /usr/share/alsa/alsa.conf /usr/share/alsa/alsa.conf.mednafen"
sudo chroot Arkbuild/ bash -c "sed -i '/\"\~\/.asoundrc\"/s//\"\~\/.asoundrc.mednafen\"/' /usr/share/alsa/alsa.conf.mednafen"
sudo chroot Arkbuild/ bash -c "cp -fv /usr/share/alsa/alsa.conf /usr/share/alsa/alsa.conf.gametank"
sudo chroot Arkbuild/ bash -c "sed -i '/\"\~\/.asoundrc\"/s//\"\~\/.asoundrc.gametank\"/' /usr/share/alsa/alsa.conf.gametank"

# Sleep script
sudo mkdir -p Arkbuild/usr/lib/systemd/system-sleep
sudo cp scripts/sleep.${CHIPSET} Arkbuild/usr/lib/systemd/system-sleep/sleep
sudo chmod 777 Arkbuild/usr/lib/systemd/system-sleep/sleep

# Set performance governor to ondemand on boot
sudo chroot Arkbuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/perfnorm quiet &\") | crontab -"

# Speaker Toggle to set audio output to SPK on boot
sudo mkdir -p Arkbuild/usr/local/bin
sudo cp scripts/spktoggle.sh Arkbuild/usr/local/bin/
sudo chmod 777 Arkbuild/usr/local/bin/spktoggle.sh
sudo chroot Arkbuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/spktoggle.sh &\") | crontab -"
sudo cp scripts/audiopath.service Arkbuild/etc/systemd/system/audiopath.service
sudo cp scripts/audiostate.service Arkbuild/etc/systemd/system/audiostate.service
sudo chroot Arkbuild/ bash -c "systemctl enable audiopath"
sudo chroot Arkbuild/ bash -c "systemctl enable audiostate"

# Copy necessary tools for expansion of ROOTFS and convert fat32 games partition to exfat on initial boot
sudo cp scripts/expandtoexfat.sh.${CHIPSET} ${mountpoint}/expandtoexfat.sh
sudo cp scripts/firstboot.sh ${mountpoint}/firstboot.sh
sudo cp scripts/firstboot.service Arkbuild/etc/systemd/system/firstboot.service
sudo chroot Arkbuild/ bash -c "systemctl enable firstboot"

# Add hotkeydaemon service and python script
sudo cp hotkeydaemon/killer_daemon.service Arkbuild/etc/systemd/system/killer_daemon.service
sudo cp hotkeydaemon/killer_daemon.py Arkbuild/usr/local/bin/killer_daemon.py
sudo chmod 777 Arkbuild/usr/local/bin/killer_daemon.py
sudo chroot Arkbuild/ bash -c "systemctl disable killer_daemon"

# Add amiga script
sudo cp amiga/amiga.sh Arkbuild/usr/local/bin/

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
sudo chroot Arkbuild/ bash -c "systemctl disable getty@tty0.service getty@tty1.service"

# Disable some other unneeded services
sudo chroot Arkbuild/ bash -c "systemctl disable ModemManager polkit"

# Disable ssh service from automatically starting
sudo chroot Arkbuild/ bash -c "systemctl disable ssh"

# Update Messaage of the Day
sudo cp -f scripts/00-header Arkbuild/etc/update-motd.d/00-header
sudo cp -f scripts/10-help-text Arkbuild/etc/update-motd.d/10-help-text
sudo rm -f Arkbuild/etc/motd
sudo chmod 777 Arkbuild/etc/update-motd.d/*

# Disable some unneeded interfaces in NetworkManager
cat <<EOF | sudo tee -a Arkbuild/etc/NetworkManager/NetworkManager.conf

[device]
wifi.scan-rand-mac-address=no

[keyfile]
unmanaged-devices=interface-name:p2p0;interface-name:ap0
EOF

# Remove requirement of sudo for controlling nmcli
cat <<EOF | sudo tee -a Arkbuild/etc/polkit-1/rules.d/10-networkmanager.rules
polkit.addRule(function(action, subject) {
    if (action.id.indexOf("org.freedesktop.NetworkManager") == 0 &&
        subject.isInGroup("netdev")) {
        return polkit.Result.YES;
    }
});
EOF

# Default set timezone to New York
sudo chroot Arkbuild/ bash -c "ln -sf /usr/share/zoneinfo/America/New_York /etc/localtime"

# Fetch older Debian library versions only for the optional application/port
# bundle.  The GUI-only image does not ship PortMaster or those applications.
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  source ./fetch_compat_libs.sh
fi

# Various tools available through Options added here
sudo mkdir -p Arkbuild/opt/system/Advanced
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  sudo cp dArkOS_Tools/*.sh Arkbuild/opt/system/
  if compgen -G "dArkOS_Tools/${CHIPSET}/*.sh" >/dev/null; then
    sudo cp dArkOS_Tools/${CHIPSET}/*.sh Arkbuild/opt/system/Advanced/
  fi
  sudo cp dArkOS_Tools/Advanced/*.sh Arkbuild/opt/system/Advanced/
  sudo cp scripts/"Enable Quick Mode".sh Arkbuild/opt/system/Advanced/
  if [[ "$UNIT" == *"rgb10"* ]] || [[ "$UNIT" == "rk2020" ]] || [[ "$UNIT" == *"oga"* ]]; then
    sudo cp dArkOS_Tools/OGA/*.sh Arkbuild/opt/system/Advanced/
  else
    sudo cp scripts/Switch* Arkbuild/usr/local/bin/
    sudo cp scripts/"Switch to SD2 for Roms.sh" Arkbuild/opt/system/Advanced/
  fi
else
  gui_tools=(
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
  for tool in "${gui_tools[@]}"; do
    sudo cp "dArkOS_Tools/$tool" Arkbuild/opt/system/
  done
fi
sudo chroot Arkbuild/ bash -c "chown -R ark:ark /opt"
sudo chmod -R 777 Arkbuild/opt/system/

# Add tool copy game roms for device RGB10
if [[ "$UNIT" == *"rgb10"* ]]; then
  sudo cp dArkOS_Tools/RGB10/*.sh Arkbuild/opt/system/
fi

# Copy performance scripts
sudo cp scripts/perf* Arkbuild/usr/local/bin/

# Add preservation of SDL_VIDEO_EGL_DRIVER to sudoers
cat <<EOF | sudo tee Arkbuild/etc/sudoers.d/ark_preserve_sdl_video_egl_driver
Defaults        env_keep += "SDL_VIDEO_EGL_DRIVER"
EOF
sudo chmod 0440 Arkbuild/etc/sudoers.d/ark_preserve_sdl_video_egl_driver

# Add USB DAC Support
echo -e "Generating 20-usb-alsa.rules udev for usb dac support"
echo -e "KERNEL==\"controlC[0-9]*\", DRIVERS==\"usb\", SYMLINK=\"snd/controlC7\"" | sudo tee Arkbuild/etc/udev/rules.d/20-usb-alsa.rules
sudo chroot Arkbuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/checknswitchforusbdac.sh &\") | crontab -"

# Fix LEDs on A10 Mini to default to the nice dim blue light while powered on
if [[ "$UNIT" == "a10mini" ]]; then
  sudo chroot Arkbuild/ bash -c "(crontab -l 2>/dev/null; echo \"@reboot /usr/local/bin/fix_power_led &\") | crontab -"
fi

# Disable requirement for sudo for setting niceness
echo "ark              -       nice            -20" | sudo tee -a Arkbuild/etc/security/limits.conf

# Copy various other backend tools
sudo cp scripts/checkbrightonboot Arkbuild/usr/local/bin/
sudo cp scripts/current_* Arkbuild/usr/local/bin/
sudo cp scripts/finish.sh Arkbuild/usr/local/bin/
sudo cp scripts/pause.sh Arkbuild/usr/local/bin/
sudo cp scripts/finish.sh.qm Arkbuild/usr/local/bin/
sudo cp scripts/pause.sh.qm Arkbuild/usr/local/bin/
sudo cp scripts/finish.sh Arkbuild/usr/local/bin/finish.sh.orig
sudo cp scripts/pause.sh Arkbuild/usr/local/bin/pause.sh.orig
sudo cp scripts/speak_bat_life.sh Arkbuild/usr/local/bin/
sudo cp scripts/spktoggle.sh Arkbuild/usr/local/bin/
sudo cp scripts/timezones Arkbuild/usr/local/bin/
sudo cp scripts/BaRT_QuickMode.sh Arkbuild/usr/local/bin/
sudo cp scripts/"Enable Quick Mode".sh Arkbuild/usr/local/bin/
sudo cp scripts/"Disable Quick Mode".sh Arkbuild/usr/local/bin/
sudo cp scripts/arkos_ap_mode.sh Arkbuild/usr/local/bin/
sudo cp scripts/auto_suspend* Arkbuild/usr/local/bin/
sudo cp scripts/processcheck.sh Arkbuild/usr/local/bin/
sudo cp scripts/autosuspend.service Arkbuild/etc/systemd/system/
sudo chroot Arkbuild/ bash -c "pip install --break-system-packages --root-user-action ignore inputs"
sudo chroot Arkbuild/ bash -c "systemctl disable autosuspend"
sudo cp scripts/wifi_importer.service Arkbuild/etc/systemd/system/
sudo chroot Arkbuild/ bash -c "systemctl enable wifi_importer"
sudo cp scripts/keystroke.py Arkbuild/usr/local/bin/
sudo cp scripts/b2.sh Arkbuild/usr/local/bin/
sudo cp scripts/freej2me.sh Arkbuild/usr/local/bin/
sudo cp scripts/easyrpg.sh Arkbuild/usr/local/bin/
sudo cp scripts/get_last_played.sh Arkbuild/usr/local/bin/
sudo cp scripts/gx4000.sh Arkbuild/usr/local/bin/
sudo cp scripts/isitpng.sh Arkbuild/usr/local/bin/
sudo cp scripts/neogeocd.sh Arkbuild/usr/local/bin/
sudo cp scripts/netplay.sh Arkbuild/usr/local/bin/
sudo mkdir -p Arkbuild/etc/hostapd
sudo cp hostapd/hostapd.conf Arkbuild/etc/hostapd/
sudo cp dnsmasq/dnsmasq.conf Arkbuild/etc/
sudo cp scripts/sleep_governors.sh Arkbuild/usr/local/bin/
sudo cp scripts/wasitpng.sh Arkbuild/usr/local/bin/
sudo cp global/* Arkbuild/usr/local/bin/
# Disable winbind as connectivity to Active Directory is not needed
sudo chroot Arkbuild/ bash -c "systemctl disable winbind"
# Disable samba-ad-dc as connectivity to Active Directory is not needed as well as some other services
sudo chroot Arkbuild/ bash -c "systemctl disable samba-ad-dc dnsmasq hostapd"
# Disable e2scrub_reap if ext file system is not being used for rootfs
if [ "$ROOT_FILESYSTEM_FORMAT" == "xfs" ] || [ "$ROOT_FILESYSTEM_FORMAT" == "btrfs" ]; then
  sudo chroot Arkbuild/ bash -c "systemctl disable e2scrub_reap"
fi
# Set the default graphical target to multi-user instead of graphical"
sudo chroot Arkbuild/ bash -c "systemctl set-default multi-user.target"
if [[ "$UNIT" == "rgb10" ]]; then
  sudo cp device/rgb10/* Arkbuild/usr/local/bin/
elif [[ "$UNIT" == "rg351mp" ]] || [[ "$UNIT" == "g350" ]] || [[ "$UNIT" == "a10mini" ]]; then
  sudo cp device/rg351mp/*.sh Arkbuild/usr/local/bin/
  sudo cp device/rg351mp/*.py Arkbuild/usr/local/bin/
  sudo cp device/rg351mp/*.green Arkbuild/usr/local/bin/
  sudo cp device/rg351mp/*.red Arkbuild/usr/local/bin/
  sudo cp device/rg351mp/fix_power_led Arkbuild/usr/local/bin/
  sudo cp device/rg351mp/checkbrightonboot Arkbuild/usr/local/bin/
  if [[ "$UNIT" == "a10mini" ]]; then
    sudo cp device/a10mini/"Change LED to Green.sh" Arkbuild/opt/system/"Change LED to Orange.sh"
    sudo sed -i '/Green.sh/s//Orange.sh/g' Arkbuild/opt/system/"Change LED to Orange.sh"
	sudo sed -i '/Red.sh/s//Blue.sh/g' Arkbuild/opt/system/"Change LED to Orange.sh"
    sudo cp Arkbuild/opt/system/"Change LED to Orange.sh" Arkbuild/usr/local/bin/"Change LED to Orange.sh"
    sudo rm Arkbuild/usr/local/bin/"Change LED to Green.sh"
    sudo mv -f Arkbuild/usr/local/bin/"Change LED to Red.sh" Arkbuild/usr/local/bin/"Change LED to Blue.sh"
    sudo sed -i '/Red.sh/s//Blue.sh/g' Arkbuild/usr/local/bin/"Change LED to Blue.sh"
	sudo sed -i '/Green.sh/s//Orange.sh/g' Arkbuild/usr/local/bin/"Change LED to Blue.sh"
    sudo chroot Arkbuild/ bash -c "chown -R ark:ark /opt"
    sudo chmod 777 Arkbuild/opt/system/"Change LED to Orange.sh"
  else
    sudo cp device/rg351mp/"Change LED to Red.sh" Arkbuild/opt/system/
    sudo chroot Arkbuild/ bash -c "chown -R ark:ark /opt"
    sudo chmod 777 Arkbuild/opt/system/"Change LED to Red.sh"
  fi
  sudo cp device/rg351mp/*.service Arkbuild/etc/systemd/system/
  sudo chroot Arkbuild/ bash -c "systemctl enable 351mp batt_led"
fi
if [[ "$UNIT" == "g350" ]]; then
  sudo cp scripts/g350/*.sh Arkbuild/usr/local/bin/
  sudo cp scripts/g350/logo.service Arkbuild/etc/systemd/system/logo.service
  sudo chroot Arkbuild/ bash -c "systemctl enable logo"
fi

# Make all scripts in /usr/local/bin executable, world style
sudo chmod 777 Arkbuild/usr/local/bin/*

# Link themes folder to /roms/themes and clone some themes to the folder
sudo rm -rf Arkbuild/etc/emulationstation/themes/
sudo chroot Arkbuild/ bash -c "ln -sfv /roms/themes/ /etc/emulationstation/themes"

# Also expose /roms2/themes via the user themes path so ES picks up themes
# from the second SD card in SD2-for-Roms mode (dangles harmlessly otherwise).
if [[ "$UNIT" != *"rgb10"* ]] && [[ "$UNIT" != "rk2020" ]] && [[ "$UNIT" != *"oga"* ]]; then
  sudo chroot Arkbuild/ bash -c "ln -sfv /roms2/themes/ /home/ark/.emulationstation/themes"
fi

# Link music folder to /roms/bgmusic
sudo rm -rf Arkbuild/home/ark/.emulationstation/music
sudo chroot Arkbuild/ bash -c "ln -sfv /roms/bgmusic/ /home/ark/.emulationstation/music"

# The GUI-only image does not build image-viewer, so use the built-in ASCII
# loading screen instead of leaving a launcher dependency on a skipped app.
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  sudo chroot Arkbuild/ touch /home/ark/.config/.GameLoadingIModePIC
else
  sudo chroot Arkbuild/ touch /home/ark/.config/.GameLoadingIModeASCII
fi

# Set default volume
sudo cp audio/asound.state.${CHIPSET} Arkbuild/var/local/asound.state

# Set SDL Video Driver for bash
echo "export SDL_VIDEO_EGL_DRIVER=libEGL.so" | sudo tee Arkbuild/etc/profile.d/SDL_VIDEO.sh

# Set device name 
dNAME=`echo $NAME | tr '[:lower:]' '[:upper:]'`
echo "$dNAME" | sudo tee Arkbuild/home/ark/.config/.DEVICE

# Configure default samba share setup.  Real paths, not /roms and /home/ark: those are
# symlinks into the DATA partition now, and samba defaults to "wide links = no" -- it
# refuses to follow a symlink that leaves the share, so a share rooted on one is a share
# that shows nothing.
cat <<EOF | sudo tee -a Arkbuild/etc/samba/smb.conf
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

[ark]
   comment = ark
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
  cat <<EOF | sudo tee -a Arkbuild/etc/samba/smb.conf
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

sudo chroot Arkbuild/ bash -c "systemctl disable smbd"
sudo chroot Arkbuild/ bash -c "systemctl disable nmbd"

# Set distro identification and version
sudo mkdir -p Arkbuild/usr/share/plymouth/themes/
cat <<EOF | sudo tee Arkbuild/usr/share/plymouth/themes/text.plymouth
title=MixOS (${BUILD_DATE})
EOF
echo "${BUILD_DATE}" | sudo tee Arkbuild/home/ark/.config/.VERSION

# Set boot up welcome text with distro and version
sudo cp scripts/boot_text.sh Arkbuild/usr/local/bin/
sudo chmod 777 Arkbuild/usr/local/bin/boot_text.sh
sudo cp scripts/welcome-message.service Arkbuild/etc/systemd/system/welcome-message.service
sudo chroot Arkbuild/ bash -c "systemctl enable welcome-message"

# Mark completed MixOS updates with this current build
release_tags=( $(git -c 'versionsort.suffix=-' ls-remote --tags --sort='v:refname' https://github.com/christianhaitian/darkos-updates.git | cut -d/ -f3- | sed 's/^v//I') )
if [[ ! -z "$release_tags" ]]; then
  for release_tag in "${release_tags[@]}"
  do
    sudo touch Arkbuild/home/ark/.config/.update${release_tag}
  done
fi

# Set the ownver of the ark folder and all sub content to ark
sudo chroot Arkbuild/ bash -c "chown -R ark:ark /home/ark"

# Clone some themes to the tempthemes folder
sudo mkdir -p Arkbuild/tempthemes
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  if [[ "$UNIT" == *"rgb10"* ]] || [[ "$UNIT" == "rk2020" ]] || [[ "$UNIT" == *"oga"* ]]; then
    sudo git clone --depth=1 https://github.com/pix33l/es-theme-pixui.git Arkbuild/tempthemes/es-theme-pixui
  fi
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-freeplay.git Arkbuild/tempthemes/es-theme-freeplay
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-minimal-arkos.git Arkbuild/tempthemes/es-theme-minimal-arkos
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-nes-box.git Arkbuild/tempthemes/es-theme-nes-box
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-switch.git Arkbuild/tempthemes/es-theme-switch
  sudo git clone --depth=1 https://github.com/dani7959/es-theme-replica.git Arkbuild/tempthemes/es-theme-replica
else
  # One theme is enough for the system-tools-only EmulationStation GUI.
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-nes-box.git Arkbuild/tempthemes/es-theme-nes-box
fi

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

# /roms on the rootfs becomes a symlink into the home partition.  Some 200 lines across
# the RK3326 scripts, the EmulationStation config and the PortMaster tooling say /roms,
# and they keep resolving through this; nothing has to be renamed to move the partition.
# If an earlier stage already put real files in Arkbuild/roms they are moved onto the
# partition rather than lost, because replacing a populated directory with a symlink
# would silently drop whatever was in it.
if [[ -d Arkbuild/roms && ! -L Arkbuild/roms ]]; then
  if [[ -n "$(sudo ls -A Arkbuild/roms 2>/dev/null)" ]]; then
    echo -e "Moving the existing Arkbuild/roms contents onto the DATA partition\n"
    sudo cp -a Arkbuild/roms/. ${fat32_mountpoint}/
  fi
  sudo rm -rf Arkbuild/roms
fi
# Relative, for the same reason /home/ark is: an absolute target would make a host-side
# `cp something Arkbuild/roms/...` write to /home/virtua/roms on the BUILD MACHINE.
# Stripping the leading slash makes it relative to whatever root it is read from, so it
# resolves to Arkbuild/home/virtua/roms here and /home/virtua/roms on the device.
sudo ln -sfn "${DATA_MOUNT_POINT#/}/roms" Arkbuild/roms
if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  while read GAME_SYSTEM; do
    if [[ ! "$GAME_SYSTEM" =~ ^# ]]; then
      echo -e "Creating ${fat32_mountpoint}/${GAME_SYSTEM}\n"
      sudo mkdir -p ${fat32_mountpoint}/${GAME_SYSTEM}
    fi
  done <game_systems.txt
else
  # The GUI-only image needs only system data and theme/media locations.  Do
  # not populate emulator folders, PortMaster, ThemeMaster, or sample games.
  gui_directories=(backup bgmusic bios launchimages shutdownimages themes tools)
  for directory in "${gui_directories[@]}"; do
    echo -e "Creating ${fat32_mountpoint}/${directory}\n"
    sudo mkdir -p "${fat32_mountpoint}/${directory}"
  done
fi

if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  # Add latest version of PortMaster install to roms/tools folder
  for (( ; ; ))
  do
   #wget -t 3 -T 60 --no-check-certificate https://github.com/PortsMaster/PortMaster-GUI/releases/download/8.5.22_0528/PortMaster.zip
   PMver=$(curl --silent -qI https://github.com/PortsMaster/PortMaster-GUI/releases/latest | awk -F '/' '/^location/ {print  substr($NF, 1, length($NF)-1)}')
   wget -t 3 -T 60 --no-check-certificate https://github.com/PortsMaster/PortMaster-GUI/releases/download/${PMver}/Install.PortMaster.sh
   if [ $? == 0 ]; then
    break
   fi
   sleep 10
  done
  sudo mv -f Install.PortMaster.sh ${fat32_mountpoint}/tools/Install.PortMaster.sh
  chmod 777 ${fat32_mountpoint}/tools/Install.PortMaster.sh

  # Add latest version of ThemeMaster to roms/tools folder
  for (( ; ; ))
  do
   wget -t 3 -T 60 --no-check-certificate https://github.com/JohnIrvine1433/ThemeMaster/archive/refs/heads/master.zip
   if [ $? == 0 ]; then
    break
   fi
   sleep 10
  done
  sudo unzip -X -o master.zip -d ${fat32_mountpoint}/tools/
  sudo rm -rf ${fat32_mountpoint}/tools/ThemeMaster
  sudo mv -f ${fat32_mountpoint}/tools/ThemeMaster-master/ThemeMaster ${fat32_mountpoint}/tools/
  sudo mv -f ${fat32_mountpoint}/tools/ThemeMaster-master/ThemeMaster.sh ${fat32_mountpoint}/tools/
  sudo rm -rf ${fat32_mountpoint}/tools/ThemeMaster-master/
  rm -f master.zip

  # Get some sample pico-8 games.  ${fat32_mountpoint} and not /roms: this line named an
  # absolute path, so it cleared the BUILD HOST's /roms rather than the partition being
  # populated two lines below.  Harmless while no such directory existed on the host and
  # not something to leave standing in a script that runs as root.
  sudo rm -rf ${fat32_mountpoint}/pico-8/carts/*
  sudo wget -t 3 -T 60 --no-check-certificate https://www.lexaloffle.com/bbs/cposts/1/15133.p8.png -O ${fat32_mountpoint}/pico-8/carts/celeste.p8.png
  sudo wget -t 3 -T 60 --no-check-certificate https://www.lexaloffle.com/bbs/cposts/sc/scrap_boy-6.p8.png -O ${fat32_mountpoint}/pico-8/carts/scrap_boy-6.p8.png
  sudo wget -t 3 -T 60 --no-check-certificate https://www.lexaloffle.com/bbs/cposts/di/dinkykong-0.p8.png -O ${fat32_mountpoint}/pico-8/carts/dinkykong-0.p8.png
  sudo wget -t 3 -T 60 --no-check-certificate https://www.lexaloffle.com/bbs/cposts/po/poom_0-9.p8.png -O ${fat32_mountpoint}/pico-8/carts/poom_0-9.p8.png
  sudo wget -t 3 -T 60 --no-check-certificate https://www.lexaloffle.com/bbs/cposts/ch/cherrybomb-0.p8.png -O ${fat32_mountpoint}/pico-8/carts/cherrybomb-0.p8.png
fi

# Copy default GUI launch and shutdown images.
sudo cp launchimages/loading.ascii.${UNIT} ${fat32_mountpoint}/launchimages/loading.ascii
sudo cp launchimages/loading.jpg.${UNIT} ${fat32_mountpoint}/launchimages/loading.jpg
sudo cp shutdownimages/bye.gif ${fat32_mountpoint}/shutdownimages/

if [[ "$BUILD_BUNDLED_APPS" == y ]]; then
  # Copy application-specific scanners and seed the pre-expansion theme folder.
  sudo cp -a ecwolf/Scan* ${fat32_mountpoint}/wolf/
  sudo cp -a scummvm/scripts/Scan* ${fat32_mountpoint}/scummvm/
  sudo cp -a hypseus-singe/scripts/Scan* ${fat32_mountpoint}/alg/
  sudo cp -a scummvm/scripts/menu.scummvm ${fat32_mountpoint}/scummvm/
  sudo git clone --depth=1 https://github.com/Jetup13/es-theme-nes-box.git ${fat32_mountpoint}/themes/es-theme-nes-box
fi
sync

# The home directory itself.  useradd -m in setup_ark_user built this tree on the ROOTFS,
# under what is about to become a mount point: the skeleton, .config, .emulationstation
# and the two locale lines in .bashrc.  Once p3 mounts over it none of that is reachable,
# so the same tree is copied onto the partition and the mounted and unmounted cases look
# alike.  Anything an emulator build dropped in /home/ark came here too -- that path is a
# symlink to this directory now, so those builds wrote into this very tree.
if [[ -d "Arkbuild${DATA_MOUNT_POINT}" ]]; then
  echo -e "Seeding ${DATA_MOUNT_POINT} on the DATA partition from the rootfs copy\n"
  sudo cp -a "Arkbuild${DATA_MOUNT_POINT}/." ${data_mountpoint}/ || \
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
the legacy EmulationStation tree, which /roms still points at.
Do not delete this file: the J36 Ultra initramfs reads it to tell this partition apart
from a plain data partition, and without it the boot mounts this one read-only.
EOF
sync

# Ownership and modes, BEFORE roms.tar is made, so the tar carries them too.
#
# chown is new here and vfat never needed it: vfat has no ownership at all, and the old
# fstab line handed the whole partition to uid 1000 with umask=000.  ext2 does have
# ownership, so it has to be set once, here, while the partition is a loop device --
# no mount option will do it afterwards.  1000:1000 is the ark user, created by
# setup_ark_user in bootstrap_rootfs.sh; numeric because this is the host's mount and
# the host has no such user.  775 so the ark group can write and everyone can read.
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
sudo tar -C ${data_mountpoint} --exclude=./lost+found -cvf Arkbuild/roms.tar .

sudo umount ${data_mountpoint}
sudo losetup -d ${LOOP_ROM}
sudo rm -rf ${data_mountpoint}
