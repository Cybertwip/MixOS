#!/bin/bash

BUILD_JOBS="${BUILD_JOBS:-4}"

echo -e "Installing build dependencies and needed packages...\n\n"

if [[ "${USERSPACE_ARCH:-}" == "armhf" ]]; then
  BIT="native"
  ARCH="arm-linux-gnueabihf"
  CHROOT_DIR="Arkbuild"
elif [[ "${USERSPACE_ARCH:-}" == "arm64" ]]; then
  BIT="64"
  ARCH="aarch64-linux-gnu"
  CHROOT_DIR="Arkbuild"
elif [ "$1" == "32" ]; then
  BIT="32"
  ARCH="arm-linux-gnueabihf"
  CHROOT_DIR="Arkbuild32"
else
  BIT="64"
  ARCH="aarch64-linux-gnu"
  CHROOT_DIR="Arkbuild"
fi

# Install additional needed packages and protect them from autoremove.
#
# THE WHOLE LIST IN ONE CALL.  This was a `while read' loop around install_package,
# and install_package is one apt transaction -- so the two lists below cost a hundred
# and fifty separate apt invocations, each re-reading the package lists, resolving
# dependencies and running its own trigger pass for a single package.  That is the
# "packages download individually" in the build log, and it is most of the wall clock
# of this stage.  install_package and protect_package both take a list; give them one.
mapfile -t NEEDED_PACKAGES < <(read_package_list needed_packages.txt)
if (( ${#NEEDED_PACKAGES[@]} )); then
  install_package $BIT "${NEEDED_PACKAGES[@]}"
  protect_package $BIT "${NEEDED_PACKAGES[@]}"
fi

# Install build dependencies
mapfile -t NEEDED_DEV_PACKAGES < <(read_package_list needed_dev_packages.txt)
if (( ${#NEEDED_DEV_PACKAGES[@]} )); then
  install_package $BIT "${NEEDED_DEV_PACKAGES[@]}"
  # Deliberately not protected: these are build-time only and cleanup_filesystem.sh
  # removes them from the shipped rootfs.
fi

# Default gcc and g++ to version 12 if gcc is newer than 12
GCC_VERSION=`sudo chroot ${CHROOT_DIR}/ bash -c "gcc --version | head -n 1 | awk '{print $3}' | cut -d' ' -f3 | cut -d'.' -f1"`
if (( GCC_VERSION > 12 )); then
  install_package $BIT gcc-12 g++-12
  sudo chroot ${CHROOT_DIR}/ bash -c "update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 10"
  sudo chroot ${CHROOT_DIR}/ bash -c "update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 20"
  sudo chroot ${CHROOT_DIR}/ bash -c "update-alternatives --set gcc /usr/bin/gcc-12"
  sudo chroot ${CHROOT_DIR}/ bash -c "update-alternatives --set g++ /usr/bin/g++-12"
fi

# Bind ccache to chroot to speed up consecutive builds.  Only when it is not already
# bound: this script is re-run whenever a later userspace component fails, and the
# resume runner mounts it too, so an unconditional `mount --bind' stacks one layer per
# attempt on the same directory -- which is what left a mount behind after the single
# umount at cleanup and produced the "Device or resource busy" on the way out.
[ ! -d "${CHROOT_DIR}/home/ark/Arkbuild_ccache" ] && sudo mkdir -p ${CHROOT_DIR}/home/ark/Arkbuild_ccache
if ! mountpoint -q ${CHROOT_DIR}/home/ark/Arkbuild_ccache; then
  sudo mount --bind ${PWD}/Arkbuild_ccache ${CHROOT_DIR}/home/ark/Arkbuild_ccache
fi
sudo chroot ${CHROOT_DIR}/ bash -c "[ -z \$(echo \$CCACHE_DIR | grep ccache) ]" && echo -e "export CCACHE_DIR=/home/ark/Arkbuild_ccache" | sudo tee -a ${CHROOT_DIR}/root/.bashrc > /dev/null
sudo chroot ${CHROOT_DIR}/ bash -c "[ -z \$(echo \$PATH | grep ccache) ]" && echo -e "export PATH=/usr/lib/ccache:\$PATH" | sudo tee -a ${CHROOT_DIR}/root/.bashrc > /dev/null
sudo chroot ${CHROOT_DIR}/ bash -c "/usr/sbin/update-ccache-symlinks"

# Symlink fix for DRM headers.  -f because this script is re-run when a later
# userspace component fails, and -n so that a second run replaces the symlink
# instead of creating /usr/include/drm/libdrm inside what it already points at.
sudo chroot ${CHROOT_DIR}/ bash -c "ln -sfn /usr/include/libdrm/ /usr/include/drm"

# Place libmali manually (assumes you have libmali.so or mali drivers ready)
if [[ "${USERSPACE_ARCH:-}" == "armhf" ]]; then
  ARCHITECTURE_ARRAY=("arm-linux-gnueabihf")
else
  ARCHITECTURE_ARRAY=("aarch64-linux-gnu")
fi
if [[ -z "${USERSPACE_ARCH:-}" && "${BUILD_ARMHF}" == "y" ]]; then
  ARCHITECTURE_ARRAY+=("arm-linux-gnueabihf")
fi
for ARCHITECTURE in "${ARCHITECTURE_ARRAY[@]}"
do
  if [ "$ARCHITECTURE" == "aarch64-linux-gnu" ]; then
    FOLDER="aarch64"
  else
    FOLDER="armhf"
  fi
  sudo mkdir -p Arkbuild/usr/lib/${ARCHITECTURE}/
  wget --retry-connrefused --retry-on-http-error=429 --waitretry=20 -t 65 -T 60 --no-check-certificate -O ${whichmali} https://github.com/christianhaitian/${CHIPSET}_core_builds/raw/refs/heads/master/mali/${FOLDER}/${whichmali}
  sudo mv ${whichmali} Arkbuild/usr/lib/${ARCHITECTURE}/.
  cd Arkbuild/usr/lib/${ARCHITECTURE}
  sudo ln -sf ${whichmali} libMali.so
  for LIB in libEGL.so libEGL.so.1 libEGL.so.1.1.0 libGLES_CM.so libGLES_CM.so.1 libGLESv1_CM.so libGLESv1_CM.so.1 libGLESv1_CM.so.1.1.0 libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0 libGLESv2.so.2.1.0 libGLESv3.so libGLESv3.so.3 libgbm.so libgbm.so.1 libgbm.so.1.0.0 libmali.so libmali.so.1 libMaliOpenCL.so libOpenCL.so libwayland-egl.so libwayland-egl.so.1 libwayland-egl.so.1.0.0
  do
    sudo rm -fv ${LIB}
    sudo ln -sfv libMali.so ${LIB}
  done
  cd ../../../../
done
sudo chroot Arkbuild/ ldconfig

# THE THREE BLOCKS BELOW HAVE TO SURVIVE A SECOND RUN.
#
# build-in-vm.sh marks each userspace component as it completes and re-runs the ones
# that did not, so a component that fails anywhere after this point brings the whole
# of this script round again -- and `git clone' into a directory that already exists
# is fatal, not a no-op.  It is also the LAST command here whose status is seen, so
# the libgo2 clone failing is what fails the build, several steps after the real
# problem.  Each block therefore asks whether the thing it installs is already in
# this chroot, skips itself if it is, and otherwise throws the old source tree away
# and starts from a clone -- half-configured meson and premake trees resume worse
# than they rebuild.  build_sdl2.sh guards its own clone the same way.

# Install meson
sudo chroot ${CHROOT_DIR}/ bash -c "
if [ ! -f /meson/meson.py ]; then
  rm -rf /meson && git clone https://github.com/mesonbuild/meson.git
fi &&
  ln -sfn /meson/meson.py /usr/bin/meson"

# Build and install librga
sudo chroot ${CHROOT_DIR}/ bash -c "
if ls /usr/lib/${ARCH}/librga.so* > /dev/null 2>&1 && [ -f /usr/local/include/rga/RgaApi.h ]; then
  echo 'librga is already installed in this chroot, skipping'
else
  cd /home/ark &&
  rm -rf linux-rga &&
  git clone https://github.com/christianhaitian/linux-rga.git &&
  cd linux-rga &&
	  git checkout 1fc02d56d97041c86f01bc1284b7971c6098c5fb &&
	  meson build && cd build &&
	  meson compile -j ${BUILD_JOBS} &&
  cp -r librga.so* /usr/lib/${ARCH}/ &&
  cd .. &&
  mkdir -p /usr/local/include/rga &&
  cp -f drmrga.h rga.h RgaApi.h RockchipRgaMacro.h /usr/local/include/rga/
fi
  "

# Build and install libgo2
sudo chroot ${CHROOT_DIR}/ bash -c "
if ls /usr/lib/${ARCH}/libgo2.so* > /dev/null 2>&1 && ls /usr/include/go2/*.h > /dev/null 2>&1; then
  echo 'libgo2 is already installed in this chroot, skipping'
else
  cd /home/ark &&
	  rm -rf libgo2 &&
	  git clone https://github.com/OtherCrashOverride/libgo2.git &&
	  cd libgo2 &&
	  premake4 gmake &&
	  make -j${BUILD_JOBS} &&
  cp libgo2.so* /usr/lib/${ARCH}/ &&
  mkdir -p /usr/include/go2 &&
  cp -L src/*.h /usr/include/go2/
fi
  "
