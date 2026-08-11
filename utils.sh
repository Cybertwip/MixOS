#!/bin/bash

# Set build date
BUILD_DATE=$(date "+%m%d%Y")

# Set http/https buffer to over 500MB to minimize on possible git clone infinite hangs
git config --global http.postBuffer 524288000

# Verify the correct toolchain is available
OPT_TOOLCHAIN_DIR="/opt/toolchains/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu"
LOCAL_TOOLCHAIN_DIR="prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu"

if [ -d "$OPT_TOOLCHAIN_DIR" ]; then
  echo "Using existing system-wide toolchain at $OPT_TOOLCHAIN_DIR"
elif [ ! -d "$LOCAL_TOOLCHAIN_DIR" ]; then
  echo "Toolchain not found. Downloading Linaro toolchain to local prebuilts directory..."
  mkdir -p "$LOCAL_TOOLCHAIN_DIR"
  git clone --depth=1 https://github.com/christianhaitian/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu.git "$LOCAL_TOOLCHAIN_DIR"
  verify_action
else
  echo "Using existing local toolchain at $LOCAL_TOOLCHAIN_DIR"
fi

# Verify package cache directory exists
if [ ! -d "Arkbuild_package_cache/${CHIPSET}" ]; then
  mkdir -p Arkbuild_package_cache/${CHIPSET}
fi

# Setup the necessary exports
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
if [ -d "$OPT_TOOLCHAIN_DIR" ]; then
    export PATH="$OPT_TOOLCHAIN_DIR"/bin/:$PATH
else
    export PATH="$LOCAL_TOOLCHAIN_DIR"/bin/:$PATH
fi
if [ "$CHIPSET" == "rk3326" ]; then
  export whichmali=libmali-bifrost-g31-rxp0-gbm.so
else
  export whichmali=libmali-bifrost-g52-g13p0-gbm.so
fi

function verify_action() {
  code=$?
  if [ $code != 0 ]; then
    echo -e "Exiting build with return code ${code}"
    exit 1
  fi
}

function get_file() {
  wget --retry-connrefused --retry-on-http-error=429 --waitretry=20 -t 65 -T 30 --no-check-certificate "$@"
  if [ -f "wget-log" ]; then
    rm -f wget-log*
  fi
}

function call_chroot() {
  sudo chroot Arkbuild bash -c "source /root/.bashrc && $@"
}

function call_chroot32() {
  if [ ! -d Arkbuild32 ]; then
    setup_arkbuild32
  fi
  sudo chroot Arkbuild32 bash -c "source /root/.bashrc && $@"
}

function setup_ark_user() {
  if [ "$1" == "32" ]; then
    CHROOT_DIR="Arkbuild32"
  else
    CHROOT_DIR="Arkbuild"
  fi
  # The home directory is the DATA partition's mount point, /home/virtua, and not
  # /home/ark: p3 is where the user's files are meant to live, so a login has to land
  # on it or every file the operator creates goes onto the rootfs instead.  bash starts
  # in $HOME, so this one field is what makes a shell open on the writable partition.
  #
  # -m creates it on the ROOTFS as well, under what will be the mount point, and that
  # is deliberate: it is the fallback.  A card with no p3, or a p3 that failed its
  # fsck, still has a home directory with these dotfiles in it -- nofail in the fstab
  # line means such a boot carries on, and it would otherwise carry on into a $HOME
  # that does not exist.  finishing_touches.sh copies this tree onto p3 so the mounted
  # and unmounted cases look the same.
  ARK_HOME="${DATA_MOUNT_POINT:-/home/virtua}"
  sudo chroot ${CHROOT_DIR}/ useradd ark -k /etc/skel -d "${ARK_HOME}" -m -s /bin/bash
  # /home/ark is a symlink to it.  Around 800 lines of the emulator build scripts write
  # into /home/ark, and dozens of runtime scripts read from it; the symlink means all of
  # them keep working, at build time inside the chroot and on the device, without a
  # rename that would touch every one of those files.
  #
  # RELATIVE, and that is the whole trick.  Most of those 800 lines are host-side writes
  # of the form `sudo tee Arkbuild/home/ark/.config/.DEVICE`, and an absolute symlink
  # would send them to /home/virtua on the BUILD MACHINE -- outside the build tree
  # entirely, failing if the path does not exist there and quietly writing to somebody's
  # real home directory if it does.  A link to "virtua" resolves within whichever /home
  # it is read from, so the same symlink is correct on the host, in the chroot, and on
  # the device.  A mount point outside /home cannot have that and gets the absolute form.
  if [[ "$(dirname "${ARK_HOME}")" == "/home" ]]; then
    sudo chroot ${CHROOT_DIR}/ ln -sfnv "$(basename "${ARK_HOME}")" /home/ark
  else
    sudo chroot ${CHROOT_DIR}/ ln -sfnv "${ARK_HOME}" /home/ark
  fi
  sudo chroot ${CHROOT_DIR}/ bash -c "echo ark:ark | chpasswd"
  sudo chroot ${CHROOT_DIR}/ chage -I -1 -m 0 -M 99999 -E -1 ark
  sudo mkdir -p ${CHROOT_DIR}/etc/sudoers.d
  echo "ark     ALL= NOPASSWD: ALL" | sudo tee ${CHROOT_DIR}/etc/sudoers.d/ark-no-sudo-password
  echo "Defaults        !secure_path" | sudo tee ${CHROOT_DIR}/etc/sudoers.d/ark-no-secure-path
  sudo chmod 0440 ${CHROOT_DIR}/etc/sudoers.d/ark-no-sudo-password
  sudo chmod 0440 ${CHROOT_DIR}/etc/sudoers.d/ark-no-secure-path
  sudo chroot ${CHROOT_DIR}/ usermod -G video,sudo,render,netdev,input,audio,adm,ark ark
  directories=(".config" ".emulationstation")
  for dir in "${directories[@]}"; do
    # ${ARK_HOME} and not /home/ark: the symlink above resolves inside the chroot, but
    # only for chroot'd commands.  These mkdirs run on the HOST against a path in the
    # build tree, where /home/ark points at an absolute path that means something quite
    # different on the host -- so they name the real directory.
    sudo mkdir -p "${CHROOT_DIR}${ARK_HOME}/${dir}"
  done
  echo -e "export LC_All=en_US.UTF-8" | sudo tee -a ${CHROOT_DIR}${ARK_HOME}/.bashrc > /dev/null
  echo -e "export LC_CTYPE=en_US.UTF-8" | sudo tee -a ${CHROOT_DIR}${ARK_HOME}/.bashrc > /dev/null
  sudo chroot ${CHROOT_DIR}/ chown -R ark:ark "${ARK_HOME}/"
}

function setup_arkbuild32() {
  if [ ! -d Arkbuild32 ]; then
	    # Bootstrap base system
	    sudo debootstrap --no-check-gpg --include=eatmydata --resolve-deps --arch=armhf --foreign ${DEBIAN_CODE_NAME} Arkbuild32 http://deb.debian.org/debian/
	    sudo cp /usr/bin/qemu-arm-static Arkbuild32/usr/bin/
	    if [[ "${ENABLE_CACHE}" == "y" ]]; then
	      echo 'Acquire::http::proxy "http://127.0.0.1:3142";' | sudo tee Arkbuild32/etc/apt/apt.conf.d/99proxy
	    fi
    sudo chroot Arkbuild32/ apt -y update
    sudo chroot Arkbuild32/ apt -y install eatmydata
    sudo chroot Arkbuild32/ eatmydata /debootstrap/debootstrap --second-stage

    # Bind essential host filesystems into chroot for networking
    sudo mount --bind /dev Arkbuild32/dev
    sudo mount -t devpts none Arkbuild32/dev/pts -o newinstance,ptmxmode=0666
    #sudo mount --bind /dev/pts Arkbuild32/dev/pts
    sudo mount --bind /proc Arkbuild32/proc
    sudo mount --bind /sys Arkbuild32/sys
    echo -e "nameserver 8.8.8.8\nnameserver 1.1.1.1" | sudo tee Arkbuild32/etc/resolv.conf > /dev/null
    # Install libmali, DRM, and GBM libraries for rk3326 or rk3566
    sudo chroot Arkbuild32/ apt install -y libdrm-dev libgbm1
    setup_ark_user 32
    sudo mkdir -p Arkbuild32/home/ark
    #sudo chroot Arkbuild32/ umount /proc
    source build_deps.sh 32
    source build_sdl2.sh 32
    sudo cp -a Arkbuild32/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} Arkbuild/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension}
    sudo chroot Arkbuild/ bash -c "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2.so /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"
    sudo chroot Arkbuild/ bash -c "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} /usr/lib/arm-linux-gnueabihf/libSDL2.so"
    sudo cp -a Arkbuild32/home/ark/linux-rga/build/librga.so* Arkbuild/usr/lib/arm-linux-gnueabihf/
    sudo cp -a Arkbuild32/home/ark/libgo2/libgo2.so* Arkbuild/usr/lib/arm-linux-gnueabihf/
    # Place libmali manually (assumes you have libmali.so or mali drivers ready)
    sudo mkdir -p Arkbuild32/usr/lib/arm-linux-gnueabihf/
    wget -t 3 -T 60 --no-check-certificate https://github.com/christianhaitian/${CHIPSET}_core_builds/raw/refs/heads/master/mali/armhf/${whichmali}
    sudo mv ${whichmali} Arkbuild32/usr/lib/arm-linux-gnueabihf/.
    cd Arkbuild32/usr/lib/arm-linux-gnueabihf
    sudo ln -sf ${whichmali} libMali.so
    for LIB in libEGL.so libEGL.so.1 libEGL.so.1.1.0 libGLES_CM.so libGLES_CM.so.1 libGLESv1_CM.so libGLESv1_CM.so.1 libGLESv1_CM.so.1.1.0 libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0 libGLESv2.so.2.1.0 libGLESv3.so libGLESv3.so.3 libgbm.so libgbm.so.1 libgbm.so.1.0.0 libmali.so libmali.so.1 libMaliOpenCL.so libOpenCL.so libwayland-egl.so libwayland-egl.so.1 libwayland-egl.so.1.0.0
    do
      sudo rm -fv ${LIB}
      sudo ln -sfv libMali.so ${LIB}
    done
    cd ../../../../
	sudo chroot Arkbuild32/ ldconfig
  fi
}

function remove_arkbuild() {
  for m in home/ark/Arkbuild_ccache proc dev/pts dev dev sys
  do
    if grep -qs "Arkbuild/${m} " /proc/mounts; then
      sudo umount -l Arkbuild/${m}
      verify_action
      sync
      sleep 1
    fi
  done
  sudo rm -rf Arkbuild/home/ark/Arkbuild_ccache
  (cat /proc/mounts | grep -qs "Arkbuild") && sudo umount -l Arkbuild
  (cat /proc/mounts | grep -qs "Arkbuild-final") && sudo umount -l Arkbuild-final
  return 0
}

function remove_arkbuild32() {
  for m in home/ark/Arkbuild_ccache proc dev/pts dev sys
  do
    if grep -qs "Arkbuild32/${m} " /proc/mounts; then
      sudo umount -l Arkbuild32/${m}
      verify_action
      sync
      sleep 1
    fi
  done
  (cat /proc/mounts | grep -qs "Arkbuild32") && sudo umount -l Arkbuild32
  [ -d "Arkbuild32" ] && sudo rm -rf Arkbuild32
  return 0
}

updateapt="N"
function install_package() {
  if [ "$1" == "native" ]; then
    NEEDED_ARCH=""
    CHROOT_DIR="Arkbuild"
  elif [ "$1" == "32" ]; then
    NEEDED_ARCH=""
    CHROOT_DIR="Arkbuild32"
  elif [ "$1" == "armhf" ]; then
    NEEDED_ARCH=":armhf"
    CHROOT_DIR="Arkbuild"
  else
    NEEDED_ARCH=":arm64"
    CHROOT_DIR="Arkbuild"
  fi
  neededlibs=( ${@:2} )
  for libs in "${neededlibs[@]}"
  do
     sudo chroot ${CHROOT_DIR}/ dpkg -s "${libs}${NEEDED_ARCH}" &>/dev/null
     if [[ $? != "0" ]]; then
       if [[ "$updateapt" == "N" ]]; then
         if test -z "$(cat ${CHROOT_DIR}/etc/apt/sources.list | grep contrib)"
         then
           sudo sed -i '/main/s//main contrib non-free non-free-firmware/' ${CHROOT_DIR}/etc/apt/sources.list
		 fi
         sudo chroot ${CHROOT_DIR}/ apt -y update
         updateapt="Y"
       fi
       sudo chroot ${CHROOT_DIR}/ bash -c "DEBIAN_FRONTEND=noninteractive eatmydata apt -y install ${libs}${NEEDED_ARCH}"
       if [[ $? != "0" ]]; then
         echo " "
         echo "Could not install needed library ${libs}${NEEDED_ARCH}."
       else
	     echo "${libs}${NEEDED_ARCH} was successfully installed."
       fi
     fi
  done
}

function protect_package() {
  if [ "$1" == "32" ]; then
    CHROOT_DIR="Arkbuild32"
  else
    CHROOT_DIR="Arkbuild"
  fi
  protectlibs=( ${@:2} )
  for protectedlib in "${protectlibs[@]}"
  do
     sudo chroot ${CHROOT_DIR}/ apt-mark manual "${protectedlib}"
     if [[ $? != "0" ]]; then
       echo "${protectedlib} could not mark as manually installed."
     else
	   echo "$${protectedlib} has been marked as manually installed."
     fi
  done
}
