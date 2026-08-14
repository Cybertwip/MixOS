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

# Verify package cache directory exists.  A build VM that last ran before the
# Arkbuild -> MixOSBuild rename still holds the debootstrap tarball under the old
# name, and not finding it is not an error -- bootstrap_rootfs.sh simply debootstraps
# again, which is the 30-to-60 minute half of a cold build.  So move it across rather
# than create an empty one next to it.  Drop this once no build VM predates the rename.
if [ -d "Arkbuild_package_cache" ] && [ ! -d "MixOSBuild_package_cache" ]; then
  echo -e "Moving Arkbuild_package_cache to MixOSBuild_package_cache\n"
  mv Arkbuild_package_cache MixOSBuild_package_cache
fi
if [ ! -d "MixOSBuild_package_cache/${CHIPSET}" ]; then
  mkdir -p MixOSBuild_package_cache/${CHIPSET}
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
  sudo chroot MixOSBuild bash -c "source /root/.bashrc && $@"
}

function call_chroot32() {
  if [ ! -d MixOSBuild32 ]; then
    setup_mixosbuild32
  fi
  sudo chroot MixOSBuild32 bash -c "source /root/.bashrc && $@"
}

function setup_virtua_user() {
  if [ "$1" == "32" ]; then
    CHROOT_DIR="MixOSBuild32"
  else
    CHROOT_DIR="MixOSBuild"
  fi
  # The login user is `virtua' and its home directory is the DATA partition's mount
  # point.  The two names match on purpose: p3 is where the user's files are meant to
  # live, so a login has to land on it or every file the operator creates goes onto the
  # rootfs instead.  bash starts in $HOME, so this one field is what makes a shell open
  # on the writable partition.
  #
  # It used to be `ark', with /home/ark a symlink to the mount point, because the
  # emulator build scripts wrote 800-odd lines into /home/ark and the runtime scripts
  # read from it.  Those build scripts are out of the tree and the runtime scripts now
  # name the real path, so there is one name for one directory and no link in the middle
  # -- which is worth having, because that link was relative for reasons that took a
  # paragraph to explain and broke silently for any mount point outside /home.
  #
  # -m creates it on the ROOTFS as well, under what will be the mount point, and that
  # is deliberate: it is the fallback.  A card with no p3, or a p3 that failed its
  # fsck, still has a home directory with these dotfiles in it -- nofail in the fstab
  # line means such a boot carries on, and it would otherwise carry on into a $HOME
  # that does not exist.  finishing_touches.sh copies this tree onto p3 so the mounted
  # and unmounted cases look the same.
  MIXOS_HOME="${DATA_MOUNT_POINT:-/home/virtua}"
  sudo chroot ${CHROOT_DIR}/ useradd virtua -k /etc/skel -d "${MIXOS_HOME}" -m -s /bin/bash
  sudo chroot ${CHROOT_DIR}/ bash -c "echo virtua:virtua | chpasswd"
  sudo chroot ${CHROOT_DIR}/ chage -I -1 -m 0 -M 99999 -E -1 virtua
  sudo mkdir -p ${CHROOT_DIR}/etc/sudoers.d
  echo "virtua     ALL= NOPASSWD: ALL" | sudo tee ${CHROOT_DIR}/etc/sudoers.d/virtua-no-sudo-password
  echo "Defaults        !secure_path" | sudo tee ${CHROOT_DIR}/etc/sudoers.d/virtua-no-secure-path
  sudo chmod 0440 ${CHROOT_DIR}/etc/sudoers.d/virtua-no-sudo-password
  sudo chmod 0440 ${CHROOT_DIR}/etc/sudoers.d/virtua-no-secure-path
  sudo chroot ${CHROOT_DIR}/ usermod -G video,sudo,render,netdev,input,audio,adm,virtua virtua
  directories=(".config")
  for dir in "${directories[@]}"; do
    # ${MIXOS_HOME} rather than a literal: these mkdirs run on the HOST against a path
    # inside the build tree, so they have to name whatever useradd was given above.
    sudo mkdir -p "${CHROOT_DIR}${MIXOS_HOME}/${dir}"
  done
  echo -e "export LC_All=en_US.UTF-8" | sudo tee -a ${CHROOT_DIR}${MIXOS_HOME}/.bashrc > /dev/null
  echo -e "export LC_CTYPE=en_US.UTF-8" | sudo tee -a ${CHROOT_DIR}${MIXOS_HOME}/.bashrc > /dev/null
  sudo chroot ${CHROOT_DIR}/ chown -R virtua:virtua "${MIXOS_HOME}/"
}

function setup_mixosbuild32() {
  if [ ! -d MixOSBuild32 ]; then
	    # Bootstrap base system
	    sudo debootstrap --no-check-gpg --include=eatmydata --resolve-deps --arch=armhf --foreign ${DEBIAN_CODE_NAME} MixOSBuild32 http://deb.debian.org/debian/
	    sudo cp /usr/bin/qemu-arm-static MixOSBuild32/usr/bin/
	    if [[ "${ENABLE_CACHE}" == "y" ]]; then
	      echo 'Acquire::http::proxy "http://127.0.0.1:3142";' | sudo tee MixOSBuild32/etc/apt/apt.conf.d/99proxy
	    fi
    sudo chroot MixOSBuild32/ apt -y update
    sudo chroot MixOSBuild32/ apt -y install eatmydata
    sudo chroot MixOSBuild32/ eatmydata /debootstrap/debootstrap --second-stage

    # Bind essential host filesystems into chroot for networking.  --rbind + rslave and
    # no separate devpts: see bootstrap_rootfs.sh for why a shared bind here put a
    # `newinstance' devpts on the build machine's own /dev/pts and broke sudo on it.
    sudo mount --rbind /dev MixOSBuild32/dev
    sudo mount --make-rslave MixOSBuild32/dev
    sudo mount --rbind /proc MixOSBuild32/proc
    sudo mount --make-rslave MixOSBuild32/proc
    sudo mount --rbind /sys MixOSBuild32/sys
    sudo mount --make-rslave MixOSBuild32/sys
    echo -e "nameserver 8.8.8.8\nnameserver 1.1.1.1" | sudo tee MixOSBuild32/etc/resolv.conf > /dev/null
    # Install libmali, DRM, and GBM libraries for rk3326 or rk3566
    sudo chroot MixOSBuild32/ apt install -y libdrm-dev libgbm1
    setup_virtua_user 32
    sudo mkdir -p MixOSBuild32/home/virtua
    #sudo chroot MixOSBuild32/ umount /proc
    source build_deps.sh 32
    source build_sdl2.sh 32
    sudo cp -a MixOSBuild32/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} MixOSBuild/usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension}
    sudo chroot MixOSBuild/ bash -c "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2.so /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0"
    sudo chroot MixOSBuild/ bash -c "ln -sfv /usr/lib/arm-linux-gnueabihf/libSDL2-2.0.so.0.${extension} /usr/lib/arm-linux-gnueabihf/libSDL2.so"
    sudo cp -a MixOSBuild32/home/virtua/linux-rga/build/librga.so* MixOSBuild/usr/lib/arm-linux-gnueabihf/
    sudo cp -a MixOSBuild32/home/virtua/libgo2/libgo2.so* MixOSBuild/usr/lib/arm-linux-gnueabihf/
    # Place libmali manually (assumes you have libmali.so or mali drivers ready)
    sudo mkdir -p MixOSBuild32/usr/lib/arm-linux-gnueabihf/
    wget -t 3 -T 60 --no-check-certificate https://github.com/christianhaitian/${CHIPSET}_core_builds/raw/refs/heads/master/mali/armhf/${whichmali}
    sudo mv ${whichmali} MixOSBuild32/usr/lib/arm-linux-gnueabihf/.
    cd MixOSBuild32/usr/lib/arm-linux-gnueabihf
    sudo ln -sf ${whichmali} libMali.so
    for LIB in libEGL.so libEGL.so.1 libEGL.so.1.1.0 libGLES_CM.so libGLES_CM.so.1 libGLESv1_CM.so libGLESv1_CM.so.1 libGLESv1_CM.so.1.1.0 libGLESv2.so libGLESv2.so.2 libGLESv2.so.2.0.0 libGLESv2.so.2.1.0 libGLESv3.so libGLESv3.so.3 libgbm.so libgbm.so.1 libgbm.so.1.0.0 libmali.so libmali.so.1 libMaliOpenCL.so libOpenCL.so libwayland-egl.so libwayland-egl.so.1 libwayland-egl.so.1.0.0
    do
      sudo rm -fv ${LIB}
      sudo ln -sfv libMali.so ${LIB}
    done
    cd ../../../../
	sudo chroot MixOSBuild32/ ldconfig
  fi
}

# ── Taking down the ccache bind mount, and why it is not a plain rm -rf ───────
#
# <root>/home/virtua/MixOSBuild_ccache is a bind of the HOST's $PWD/MixOSBuild_ccache, so an
# rm -rf that runs while it is still mounted does not fail safely: it recurses THROUGH
# the mount, deletes the host's ccache, and reports only "Device or resource busy" for
# the mount point itself -- which is why a build that printed that line came back to
# all-miss compiles.  Both callers already guarded on /proc/mounts and the message
# appeared anyway, for three reasons:
#
#   * The mount can be STACKED.  ensure_ccache_mount in device/*/build-in-vm.sh binds
#     only when the path is not already a mount point; build_deps.sh binds
#     unconditionally.  A resumed build therefore has two mounts on one directory, and
#     one umount leaves one behind.
#   * `umount -l' returns before the mount is gone, so a check made immediately after
#     it can read "not mounted" while the directory is still busy.
#   * The bind PROPAGATES.  Ubuntu mounts / shared, so a service living in its own
#     mount namespace can hold a copy that never appears in this process's
#     /proc/mounts.  rmdir() refuses on a flag carried by the directory itself, which
#     is set while anything anywhere is mounted there, so no amount of care with our
#     own mount table can predict it.
#
# So: unmount every layer, wait for it to actually leave, and decide whether to delete
# from `mountpoint' -- which asks the kernel about THIS path -- rather than from a
# substring search of /proc/mounts.  If the directory survives regardless, what is left
# is the empty underlying directory: nothing was deleted through anything, and a
# shipped rootfs carrying an empty /home/virtua/MixOSBuild_ccache is not worth an error line
# from rm, so report the outcome and carry on.
function drop_ccache_mount() {
  local mnt="$1" i

  for i in 1 2 3 4 5; do
    mountpoint -q "${mnt}" || break
    sudo umount "${mnt}" 2>/dev/null || sudo umount -l "${mnt}" 2>/dev/null || break
    sync
  done
  for i in 1 2 3 4 5 6 7 8 9 10; do
    mountpoint -q "${mnt}" || break
    sync
    sleep 1
  done

  if mountpoint -q "${mnt}"; then
    echo "${mnt} is still mounted, so it is being left alone."
    echo "Deleting it now would delete the host's ccache through the mount."
    echo "The shipped rootfs will carry an empty directory there."
    return 0
  fi

  # Not a mount point here, so nothing under it belongs to the host and the delete is
  # safe.  It can still be busy -- see the propagation case above -- and that is not
  # something the log's reader can act on.
  if ! sudo rm -rf "${mnt}" 2>/dev/null; then
    echo "${mnt} is still referenced elsewhere on the system and could not be removed."
    echo "The shipped rootfs will carry an empty directory there."
  fi
  return 0
}

function remove_mixosbuild() {
  drop_ccache_mount MixOSBuild/home/virtua/MixOSBuild_ccache
  # `dev' was listed twice here, which is what a stacked bind mount needs and what the
  # other three would need just as much: ensure_rootfs_mounts binds only what is not
  # already mounted, the individual build_*.sh scripts bind unconditionally, so any of
  # these can carry more than one layer.  Unmount until the path is not a mount point
  # instead of counting the layers by hand.
  for m in proc dev/pts dev sys
  do
    for i in 1 2 3
    do
      mountpoint -q MixOSBuild/${m} || break
      sudo umount -l MixOSBuild/${m}
      verify_action
      sync
      sleep 1
    done
  done
  (cat /proc/mounts | grep -qs "MixOSBuild") && sudo umount -l MixOSBuild
  (cat /proc/mounts | grep -qs "MixOSBuild-final") && sudo umount -l MixOSBuild-final
  return 0
}

function remove_mixosbuild32() {
  drop_ccache_mount MixOSBuild32/home/virtua/MixOSBuild_ccache
  for m in proc dev/pts dev sys
  do
    for i in 1 2 3
    do
      mountpoint -q MixOSBuild32/${m} || break
      sudo umount -l MixOSBuild32/${m}
      verify_action
      sync
      sleep 1
    done
  done
  (cat /proc/mounts | grep -qs "MixOSBuild32") && sudo umount -l MixOSBuild32
  # The whole tree, so the ccache bind inside it is the thing this must not walk into:
  # `rm -rf MixOSBuild32' with that mount still up deletes the host's ccache through it
  # and says nothing about having done so.  drop_ccache_mount above prints why when it
  # cannot clear it; this only has to decline.
  if mountpoint -q MixOSBuild32/home/virtua/MixOSBuild_ccache; then
    echo "Leaving MixOSBuild32 in place: its ccache bind mount is still up"
  elif [ -d "MixOSBuild32" ]; then
    sudo rm -rf MixOSBuild32
  fi
  return 0
}

# One package name per line from a needed_*.txt list, comments and blanks dropped.
#
# Four callers had this loop inline as `while read pkg; do [[ ! "$pkg" =~ ^# ]] ...',
# and each of them then called install_package once per line -- which is what made a
# 15 MB download take a quarter of an hour.  Reading the list into an array instead is
# what lets the whole list be handed to apt in one go.  Carriage returns are stripped
# too: a CRLF list turned every name into `libfoo\r', which apt cannot satisfy.
function read_package_list() {
  local file="$1"
  [[ -f "$file" ]] || return 0
  sed -e 's/\r$//' -e 's/#.*//' -e 's/[[:space:]]*$//' "$file" | grep -v '^[[:space:]]*$' || true
}

updateapt="N"

# The sources.list fix and one `apt update' for the whole build, not one per package.
#
# ── WHY THE PROXY IS RE-ARMED HERE AND NOT ONLY AT BOOTSTRAP ─────────────────
#
# bootstrap_rootfs.sh writes 99proxy into the tree it creates, and that was the only
# copy.  cleanup_filesystem.sh then DELETES it at the end of the build, which is
# right -- a shipped image must not carry an apt proxy pointing at a machine that is
# not the handheld.  But the stripped-root snapshot is taken AFTER cleanup, so every
# resumed build restores a rootfs with no proxy configured, and its `apt update' plus
# the dozen-odd packages cleanup removed go straight out to deb.debian.org.  That is
# the 16 MB of indexes and the package downloads on a build that was supposed to be
# resuming from a cached root: apt-cacher-ng was running the whole time with the
# packages already in it, and nothing was pointed at it.
#
# Writing it here, on the one function every install goes through, makes it true for
# whichever root this run is working on and however that root got here.  It is one
# file in a chroot, it is idempotent, and cleanup removes it again exactly as before.
function apt_update_once() {
  local chroot_dir="$1"
  [[ "$updateapt" == "Y" ]] && return 0
  if ! grep -qs contrib "${chroot_dir}/etc/apt/sources.list"; then
    sudo sed -i '/main/s//main contrib non-free non-free-firmware/' "${chroot_dir}/etc/apt/sources.list"
  fi
  if [[ "${ENABLE_CACHE}" == "y" ]]; then
    sudo mkdir -p "${chroot_dir}/etc/apt/apt.conf.d"
    echo 'Acquire::http::proxy "http://127.0.0.1:3142";' |
      sudo tee "${chroot_dir}/etc/apt/apt.conf.d/99proxy" > /dev/null
  fi
  sudo chroot ${chroot_dir}/ apt -y update
  updateapt="Y"
  return 0
}

# install_package MODE PACKAGE [PACKAGE...]
#
# ── ONE APT TRANSACTION, NOT ONE PER PACKAGE ──────────────────────────────────
#
# This used to loop: a `dpkg -s' and then an entire `apt -y install' for every single
# package, which is what "it downloads them individually" was.  Each install re-read
# the package lists, resolved dependencies again, ran its own dpkg and its own trigger
# pass -- several seconds of fixed cost times a hundred and fifty packages, on a step
# whose actual download is fifteen megabytes.  Batched, it is one list read, one
# download set and one trigger pass.
#
# THE FALLBACK IS THE POINT.  apt is all-or-nothing: one name that this Debian release
# no longer carries fails the whole transaction, where the old loop installed
# everything else and named the one casualty.  So a failed batch is retried one
# package at a time.  The slow path costs exactly what the old code always cost, and
# only a genuinely unsatisfiable name pays it.
function install_package() {
  if [ "$1" == "native" ]; then
    NEEDED_ARCH=""
    CHROOT_DIR="MixOSBuild"
  elif [ "$1" == "32" ]; then
    NEEDED_ARCH=""
    CHROOT_DIR="MixOSBuild32"
  elif [ "$1" == "armhf" ]; then
    NEEDED_ARCH=":armhf"
    CHROOT_DIR="MixOSBuild"
  else
    NEEDED_ARCH=":arm64"
    CHROOT_DIR="MixOSBuild"
  fi
  local needed=( "${@:2}" )
  (( ${#needed[@]} )) || return 0

  local spec want=() missing=() installed one
  for spec in "${needed[@]}"; do
    want+=( "${spec}${NEEDED_ARCH}" )
  done

  # One dpkg-query for the lot.  A name dpkg has never heard of makes it exit non-zero
  # and complain on stderr while still reporting the others on stdout, hence the
  # redirect.  Both the bare name and name:arch are recorded as present, so a request
  # for `foo' is satisfied by foo:armhf and a request for `foo:armhf' by itself.
  installed=" $(sudo chroot ${CHROOT_DIR}/ dpkg-query -W \
      -f '${Package} ${Package}:${Architecture} ${db:Status-Status}\n' "${want[@]}" 2>/dev/null |
      awk '$NF == "installed" { print $1; print $2 }' | tr '\n' ' ')"
  for spec in "${want[@]}"; do
    [[ "$installed" == *" ${spec} "* ]] || missing+=( "$spec" )
  done

  if (( ${#missing[@]} == 0 )); then
    echo "All ${#want[@]} requested package(s) are already installed in ${CHROOT_DIR}."
    return 0
  fi

  apt_update_once "${CHROOT_DIR}"

  echo "Installing ${#missing[@]} of ${#want[@]} package(s) into ${CHROOT_DIR} in one transaction:"
  echo "  ${missing[*]}"
  if sudo chroot ${CHROOT_DIR}/ bash -c \
      "DEBIAN_FRONTEND=noninteractive eatmydata apt -y install ${missing[*]}"; then
    echo "${#missing[@]} package(s) installed."
    return 0
  fi

  echo " "
  echo "The batch install failed, so it is being retried one package at a time to find"
  echo "which name apt could not satisfy.  Each casualty is named below."
  for one in "${missing[@]}"; do
    if sudo chroot ${CHROOT_DIR}/ bash -c \
        "DEBIAN_FRONTEND=noninteractive eatmydata apt -y install ${one}"; then
      echo "${one} was successfully installed."
    else
      echo " "
      echo "Could not install needed library ${one}."
    fi
  done
  return 0
}

# protect_package MODE PACKAGE [PACKAGE...]
#
# apt-mark takes a list, so this is one chroot rather than one per package.  The old
# success message read "$${protectedlib}", which bash expands to the shell's own PID
# followed by a literal brace -- the "39947{protectedlib} has been marked as manually
# installed" that filled the build log.
function protect_package() {
  if [ "$1" == "32" ]; then
    CHROOT_DIR="MixOSBuild32"
  else
    CHROOT_DIR="MixOSBuild"
  fi
  local protectlibs=( "${@:2}" )
  (( ${#protectlibs[@]} )) || return 0
  if sudo chroot ${CHROOT_DIR}/ apt-mark manual "${protectlibs[@]}" >/dev/null; then
    echo "${#protectlibs[@]} package(s) marked as manually installed (held against autoremove)."
  else
    echo "Could not mark all of these as manually installed: ${protectlibs[*]}"
  fi
  return 0
}
