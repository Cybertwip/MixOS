#!/bin/bash

SELECTED_USERSPACE_ARCH="${USERSPACE_ARCH:-}"
if [[ "$SELECTED_USERSPACE_ARCH" == armhf ]]; then
  NATIVE_PACKAGE_MODE=native
else
  NATIVE_PACKAGE_MODE=64
fi

# Cleanup to reduce image size and remove build remnants
echo -e "Cleaning up filesystem"
call_chroot "rm -rf /home/ark/libgo2"
call_chroot "rm -rf /home/ark/linux-rga"
call_chroot "rm -rf /home/ark/${CHIPSET}_core_builds"
if [[ "${CHIPSET}" == "rk3566" ]]; then
  call_chroot "apt-mark hold ffmpeg"
fi
# ── ASK dpkg WHAT IS ACTUALLY HERE, THEN NAME ONLY THAT ──────────────────────
#
# This was one `apt remove -y' carrying the whole list below, and on any run that
# started from the stripped snapshot it printed fifty lines beginning with the
# word "Error:".  Not one of them was one.  Cleanup ends by deleting
# /var/lib/apt/lists, so a second run cannot RESOLVE a name -- and it is being
# asked about packages the first run already removed, so there is nothing to
# resolve them to.  "Unable to locate package libboost-dev" meant "that is gone,
# as intended".  Harmless, and on a terminal completely indistinguishable from a
# build that has broken, which is how it came to be reported as one.
#
# dpkg needs no lists.  /var/lib/dpkg/status is the record of what is installed
# and cleanup never touches it, so the list is filtered through dpkg-query first
# and apt is handed only names that are really present.  On a restored run that is
# none of them and the transaction is skipped in silence; on a first run it is all
# of them and nothing has changed.  Same idea as install_package in utils.sh,
# which has asked dpkg before calling apt for a while now.
#
# AND IT FIXES g++.  apt reads a trailing `+' on an argument as "...and install
# this one", so `apt remove g++' asked about a package called `g+' -- which is the
# "Unable to locate package g+" in the log, a name nobody ever wrote.  dpkg-query
# answers with `g++:armhf', and an arch-qualified name does not end in `+', so
# there is no modifier left for apt to read.
BUILD_ONLY_PACKAGES=(
  autotools-dev
  build-essential
  ccache
  clang
  cmake
  g++
  liba52-0.7.4-dev
  libasound2-dev
  libboost-date-time-dev
  libboost-dev
  libboost-filesystem-dev
  libboost-locale-dev
  libboost-regex-dev
  libboost-system-dev
  libcurl4-openssl-dev
  libdrm-dev
  libeigen3-dev
  libevdev-dev
  libxext-dev
  libfaad-dev
  libflac-dev
  libfontconfig1-dev
  libfreeimage-dev
  libfreetype-dev
  libfribidi-dev
  libglew-dev
  libglfw3-dev
  libjpeg62-turbo-dev
  libluajit-5.1-dev
  libmad0-dev
  libmpeg2-4-dev
  libncurses-dev
  libnl-3-dev
  libnl-genl-3-dev
  libnl-route-3-dev
  libogg-dev
  libopenal-dev
  libphysfs-dev
  libpng-dev
  libsdl2-dev
  libsdl2-gfx-dev
  libsdl2-image-dev
  libsdl2-mixer-dev
  libsdl2-ttf-dev
  libshaderc-dev
  libslirp-dev
  libsm-dev
  libsoxr-dev
  libspeechd-dev
  libssl-dev
  libssl-ocaml-dev
  libstdc++-12-dev
  libtheora-dev
  libudev-dev
  libvlc-dev
  libvlccore-dev
  libvorbis-dev
  libvorbisidec-dev
  libvpx-dev
  libvulkan-dev
  libx11-dev
  libx11-xcb1
  libxcb-dri2-0
  libyaml-dev
  libzip-dev
  ninja-build
  pkg-config
  premake4
  rapidjson-dev
  zlib1g-dev
)
# A name dpkg has never heard of makes it exit non-zero and complain on stderr
# while still reporting the others on stdout, hence the redirect -- the same
# reason install_package has one.
REMOVABLE_PACKAGES=()
while read -r INSTALLED_PACKAGE; do
  REMOVABLE_PACKAGES+=( "$INSTALLED_PACKAGE" )
done < <(sudo chroot Arkbuild/ dpkg-query -W \
    -f '${Package}:${Architecture} ${db:Status-Status}\n' "${BUILD_ONLY_PACKAGES[@]}" 2>/dev/null |
    awk '$NF == "installed" { print $1 }')

if (( ${#REMOVABLE_PACKAGES[@]} )); then
  echo -e "Removing ${#REMOVABLE_PACKAGES[@]} of ${#BUILD_ONLY_PACKAGES[@]} build-time packages"
  call_chroot "apt remove -y ${REMOVABLE_PACKAGES[*]}"
else
  echo -e "None of the ${#BUILD_ONLY_PACKAGES[@]} build-time packages is installed; nothing to remove"
fi

call_chroot "apt -y autoremove"
call_chroot "apt -y clean"

# ── Putting back what the autoremove above took out, in one transaction each ──
#
# These four blocks were `while read' loops calling install_package once per line, and
# install_package is a whole apt transaction: the runtime lists alone cost a hundred and
# fifty of them here, on top of the hundred and fifty build_deps.sh had already paid.
# Both functions take a list; the loops are now only about which list.
if [[ -z "$SELECTED_USERSPACE_ARCH" && "${BUILD_ARMHF}" == "y" ]]; then
  # Ensure additional needed packages are still in place
  mapfile -t ARMHF_PACKAGES < <(read_package_list needed_packages32.txt)
  if (( ${#ARMHF_PACKAGES[@]} )); then
    install_package armhf "${ARMHF_PACKAGES[@]}"
  fi
  sync Arkbuild
fi

# Ensure additional needed packages for Kodi are still in place if Kodi is built
if [[ "$CHIPSET" == *"3566"* ]] && [[ "$BUILD_KODI" == "y" ]]; then
  KODI_PACKAGES=()
  while read -r KODI_NEEDED_PACKAGE; do
    [[ "$KODI_NEEDED_PACKAGE" == *"-dev"* ]] || KODI_PACKAGES+=( "$KODI_NEEDED_PACKAGE" )
  done < <(read_package_list kodi_needed_dev_packages.txt)
  if (( ${#KODI_PACKAGES[@]} )); then
    install_package 64 "${KODI_PACKAGES[@]}"
    protect_package 64 "${KODI_PACKAGES[@]}"
  fi
fi

RUNTIME_PACKAGES=()
while read -r NEEDED_PACKAGE; do
  # ffmpeg is held back on rk3566 -- see the apt-mark hold above.
  if [[ "$CHIPSET" == *"3566"* && "$NEEDED_PACKAGE" == "ffmpeg" ]]; then
    continue
  fi
  RUNTIME_PACKAGES+=( "$NEEDED_PACKAGE" )
done < <(read_package_list needed_packages.txt)
if (( ${#RUNTIME_PACKAGES[@]} )); then
  install_package ${NATIVE_PACKAGE_MODE} "${RUNTIME_PACKAGES[@]}"
  protect_package ${NATIVE_PACKAGE_MODE} "${RUNTIME_PACKAGES[@]}"
fi
sync

if [[ "$BUILD_BLUEALSA" == "y" ]]; then
  mapfile -t BLUETOOTH_PACKAGES < <(read_package_list bluetooth_needed_packages.txt)
  if (( ${#BLUETOOTH_PACKAGES[@]} )); then
    install_package ${NATIVE_PACKAGE_MODE} "${BLUETOOTH_PACKAGES[@]}"
    protect_package ${NATIVE_PACKAGE_MODE} "${BLUETOOTH_PACKAGES[@]}"
  fi
  call_chroot "systemctl disable watchforbtaudio bluetooth bluealsa"
fi

if [[ "$SELECTED_USERSPACE_ARCH" == armhf || ( -z "$SELECTED_USERSPACE_ARCH" && "${BUILD_ARMHF}" == "y" ) ]]; then
  cd Arkbuild/usr/lib/arm-linux-gnueabihf
  for LIB in libEGL.so libEGL.so.1 libGLES_CM.so libGLES_CM.so.1 libGLESv1_CM.so libGLESv1_CM.so.1 libGLESv1_CM.so.1.1.0 libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0 libGLESv2.so.2.1.0 libGLESv3.so libGLESv3.so.3 libgbm.so libgbm.so.1 libgbm.so.1.0.0 libmali.so libmali.so.1 libMaliOpenCL.so libOpenCL.so libwayland-egl.so libwayland-egl.so.1 libwayland-egl.so.1.0.0
  do
    sudo rm -fv ${LIB}
    sudo ln -sfv libMali.so ${LIB}
  done
  cd ../../../../

  # We need to replace the armhf version of libasound2t64 with the older libasound2 binary from Bookworm
  # because the current one supplied wtih Trixie has a ioctl error issue which leads to no audio for 32bit apps
  # This can be retrieved from snapshot.debian.org
  for (( ; ; ))
  do
    wget -t 3 -T 60 --no-check-certificate https://snapshot.debian.org/archive/debian/20230104T090216Z/pool/main/a/alsa-lib/libasound2_1.2.8-1%2Bb1_armhf.deb
    if [ $? == 0 ]; then
     break
    fi
	sleep 10
  done
  dpkg --fsys-tarfile libasound2_1.2.8-1+b1_armhf.deb | tar -xO ./usr/lib/arm-linux-gnueabihf/libasound.so.2.0.0 > libasound.so.2.0.0
  sudo mv -f libasound.so.2.0.0 Arkbuild/usr/lib/arm-linux-gnueabihf/
  call_chroot "chown root:root /usr/lib/arm-linux-gnueabihf/libasound.so.2.0.0"
  rm -f libasound2_1.2.8-1+b1_armhf.deb
fi

if [[ "$SELECTED_USERSPACE_ARCH" == armhf ]]; then
  call_chroot "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2.so /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"
  call_chroot "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} /usr/lib/arm-linux-gnueabihf/libSDL2.so"
  call_chroot "ln -sfv /usr/lib/arm-linux-gnueabihf/bin/sdl2-config /usr/bin/sdl2-config"
else
  cd Arkbuild/usr/lib/aarch64-linux-gnu
  for LIB in libEGL.so libEGL.so.1 libGLES_CM.so libGLES_CM.so.1 libGLESv1_CM.so libGLESv1_CM.so.1 libGLESv1_CM.so.1.1.0 libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0 libGLESv2.so.2.1.0 libGLESv3.so libGLESv3.so.3 libgbm.so libgbm.so.1 libgbm.so.1.0.0 libmali.so libmali.so.1 libMaliOpenCL.so libOpenCL.so libwayland-egl.so libwayland-egl.so.1 libwayland-egl.so.1.0.0
  do
    sudo rm -fv ${LIB}
    sudo ln -sfv libMali.so ${LIB}
  done
  cd ../../../../

  # Make sure the built librga shared libs are still available in aarch64.
  sudo cp -av Arkbuild/usr/lib/librga.so* Arkbuild/usr/lib/aarch64-linux-gnu/ || true
  call_chroot "ln -sfv /usr/lib/aarch64-linux-gnu/libSDL2.so /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0"
  call_chroot "ln -sfv /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0.${extension} /usr/lib/aarch64-linux-gnu/libSDL2.so"
  if [[ -z "$SELECTED_USERSPACE_ARCH" && "${BUILD_ARMHF}" == "y" ]]; then
    call_chroot "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2.so /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"
    call_chroot "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} /usr/lib/arm-linux-gnueabihf/libSDL2.so"
  fi
  call_chroot "ln -sfv /usr/lib/aarch64-linux-gnu/bin/sdl2-config /usr/bin/sdl2-config"
fi

if [[ "${ENABLE_CACHE}" == "y" ]]; then
  sudo rm -f Arkbuild/etc/apt/apt.conf.d/99proxy
  sudo sed -i '/127.0.0.1:3142\//s///' Arkbuild/etc/apt/sources.list
fi
# Ensure sdl-image is symlinked properly -- IF THERE IS ONE TO SYMLINK.
#
# libSDL_image-1.2 comes from build_linapple.sh, which only the full arm64 profile
# runs, so on the armhf GUI base this was `rm' on a file that does not exist ("rm:
# cannot remove '/lib/libSDL_image-1.2.so.0': No such file or directory") followed by
# something worse: with no versioned library to find, the `ln -sf $(find ...)' below
# collapsed to a single argument, and `ln -sf /lib/libSDL_image-1.2.so.0' creates a
# link IN THE CURRENT DIRECTORY pointing at itself -- a self-referential dangling
# symlink installed into /lib of the shipped rootfs.  Ask for the versioned file
# first, and do nothing at all when it is absent.
sdl_image_real="$(call_chroot "find /lib -name 'libSDL_image-1.2.so.0.*' 2>/dev/null | head -n 1" | tr -d '\r')"
if [[ -n "$sdl_image_real" ]]; then
  call_chroot "rm -f /lib/libSDL_image-1.2.so.0"
  call_chroot "ln -sf ${sdl_image_real} /lib/libSDL_image-1.2.so.0"
else
  echo "No libSDL_image-1.2 in this rootfs (SDL 1.2 is a full-profile build); nothing to symlink"
fi
call_chroot "ldconfig"

# The ccache bind mount.  This used to be twenty lines of unmount-wait-then-rm here and
# another set of them in utils.sh's remove_arkbuild, and both got the guard wrong in the
# same way -- see drop_ccache_mount in utils.sh, which is now the only copy, for what
# `rm -rf' does to the host's ccache when the mount is still up and for the three
# reasons a /proc/mounts check does not catch it.
drop_ccache_mount Arkbuild/home/ark/Arkbuild_ccache
sudo rm -rf Arkbuild/var/log/journal
# -f: this is written by bootstrap_rootfs.sh and removed here, so a resumed build that
# reaches this stage twice finds it already gone.
sudo rm -f Arkbuild/usr/sbin/policy-rc.d
sudo rm -f Arkbuild/etc/resolv.conf
sudo rm -f Arkbuild/etc/network/interfaces
sudo rm -rf Arkbuild/usr/share/man/*
#for i in {1..8}; do sudo mkdir -p Arkbuild/usr/share/man/man"$i"; done
sudo rm -rf Arkbuild/var/lib/apt/lists/*
sudo rm -f Arkbuild/var/log/*.log
sudo rm -f Arkbuild/var/log/apt/*.log
sudo rm -f Arkbuild/tmp/reboot-needed
if [[ "${CHIPSET}" == "rk3566" ]]; then
  sudo rm -f Arkbuild/usr/share/vulkan/icd.d/*_icd.*
fi

# Ensure the legacy arm64 Vulkan loader is symlinked properly.  The armhf-only
# GUI profile does not install or use this arm64 library directory.
if [[ "$SELECTED_USERSPACE_ARCH" != armhf ]]; then
  call_chroot "find /usr/lib/aarch64-linux-gnu -type f -name 'libvulkan.so*' -not -name 'libvulkan.so.1.3.274' -delete"
  call_chroot "rm -f /usr/lib/aarch64-linux-gnu/libvulkan.so.1 /usr/lib/aarch64-linux-gnu/libvulkan.so"
  call_chroot "ln -sf /usr/lib/aarch64-linux-gnu/libvulkan.so.1.3.274 /usr/lib/aarch64-linux-gnu/libvulkan.so.1"
  call_chroot "ln -sf /usr/lib/aarch64-linux-gnu/libvulkan.so.1 /usr/lib/aarch64-linux-gnu/libvulkan.so"
fi
