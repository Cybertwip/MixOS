#!/bin/bash

# Write rootfs to disk
sync Arkbuild
if [ "${ROOT_FILESYSTEM_FORMAT}" == "xfs" ]; then
  mkdir Arkbuild-final
  sudo mount -o loop ${LOOP_DEV}p4 Arkbuild-final/
  sudo rsync -av --exclude={'home/ark/Arkbuild_ccache','proc','dev','sys'} Arkbuild/ Arkbuild-final/
  sudo umount Arkbuild-final/
  sudo rm -rf Arkbuild-final/
elif [[ "${ROOT_FILESYSTEM_FORMAT}" == *"ext"* ]]; then
  # e2fsck and resize2fs both need the filesystem OFF-LINE, and nothing before this
  # point unmounts it -- clean_mounts.sh, which does, runs after this script.  Handing
  # a mounted image to e2fsck is not caught for us either: ext2fs_check_if_mounted()
  # looks its argument up in /proc/mounts, and what is listed there is the autoloop
  # device, not this file, so e2fsck would cheerfully "repair" a live filesystem
  # underneath the mount.  Unmount first.  remove_arkbuild guards every umount on
  # /proc/mounts, so clean_mounts.sh calling it again a few lines later is a no-op.
  echo -e "Unmounting the build root before checking and shrinking it"
  remove_arkbuild
  if [[ "${BUILD_ARMHF}" == "y" ]]; then
    remove_arkbuild32
  fi
  # umount -l only promises to detach the tree from the namespace; the filesystem can
  # still be in flight behind it, and the autoloop device stays until the last
  # reference goes away.  Wait for both, then take down any loop still holding the
  # image -- e2fsck reads the file, and a loop device with dirty pages would have it
  # reading a version of the filesystem that is not the one on disk.
  for _ in $(seq 1 30); do
    mountpoint -q Arkbuild || break
    sync
    sleep 1
  done
  if mountpoint -q Arkbuild; then
    printf "\n\nArkbuild is still mounted; refusing to fsck a live filesystem.  Exiting...\n\n"
    exit 1
  fi
  for stubborn_loop in $(sudo losetup -j "${FILESYSTEM}" -O NAME --noheadings 2>/dev/null); do
    sudo losetup -d "${stubborn_loop}" || true
  done
  sync

  # -p is preen mode: it fixes what is safe to fix unattended and reports anything
  # else.  Exit 1 is "errors were corrected" and 2 is "corrected, reboot advised",
  # which is meaningless for an image file; both are fine.  Above that is a build root
  # this script cannot vouch for, and shipping it would move the failure onto the
  # device, where it looks like a bad flash instead of a bad build.
  e2fsck_rc=0
  e2fsck -p -f "${FILESYSTEM}" || e2fsck_rc=$?
  if [[ ${e2fsck_rc} -gt 2 ]]; then
    printf "\n\ne2fsck could not clean %s (exit %s).  Exiting...\n\n" "${FILESYSTEM}" "${e2fsck_rc}"
    exit 1
  fi
  resize2fs -M "${FILESYSTEM}" || {
    printf "\n\nFailed to shrink %s.  Exiting...\n\n" "${FILESYSTEM}"
    exit 1
  }

  # resize2fs shrank the FILESYSTEM.  The FILE is still BUILD_SIZE -- 52 GB -- and the
  # dd below copies the file, not the filesystem, so without this it wrote 52 GB from
  # STORAGE_PART_START: over the ROMS partition and off the end of the image.  The
  # btrfs branch truncates before its dd; this one never did, because nothing had put
  # an ext rootfs through it yet.
  fs_geometry="$(dumpe2fs -h "${FILESYSTEM}" 2>/dev/null)"
  fs_blocks="$(printf '%s\n' "${fs_geometry}" | awk -F: '/^Block count:/{gsub(/ /,"",$2); print $2}')"
  fs_bsize="$(printf '%s\n' "${fs_geometry}" | awk -F: '/^Block size:/{gsub(/ /,"",$2); print $2}')"
  if [[ -z "${fs_blocks}" || -z "${fs_bsize}" ]]; then
    printf "\n\nCould not read the block geometry of %s.  Exiting...\n\n" "${FILESYSTEM}"
    exit 1
  fi
  # Rounded up to a whole MiB: the dd writes 512-byte sectors either way, and a size in
  # MiB is the number a human compares against STORAGE_SIZE without arithmetic.
  fs_mib=$(( ((fs_blocks * fs_bsize) + 1048575) / 1048576 ))
  echo -e "Root filesystem shrank to ${fs_mib} MB of the ${STORAGE_SIZE} MB partition"
  if [[ ${fs_mib} -gt ${STORAGE_SIZE} ]]; then
    printf "\n\nThe root filesystem needs %s MB and the partition is %s MB.\n" "${fs_mib}" "${STORAGE_SIZE}"
    printf "btrfs hid this behind compress=zlib:1; ext2 does not compress, so the\n"
    printf "rootfs now costs what it actually weighs.  Raise STORAGE_SIZE in\n"
    printf "setup_partition.sh (and in the copy of those values in the device\n"
    printf "build-in-vm.sh) or take content out of the build root.\n"
    printf "Refusing to dd over the ROMS partition.  Exiting...\n\n"
    exit 1
  fi
  sudo truncate -s "${fs_mib}M" "${FILESYSTEM}"
  sync
  sudo dd if="${FILESYSTEM}" of="${DISK}" bs=512 seek="${STORAGE_PART_START}" conv=fsync,notrunc
elif [ "${ROOT_FILESYSTEM_FORMAT}" == "btrfs" ]; then
  sudo btrfs balance start --full-balance Arkbuild
  sudo sync Arkbuild
  sizes=(8000 7700 7300 7250 7100)
  i=0
  count=0
  while [[ $i -lt ${#sizes[@]} ]]; do
    size=${sizes[$i]}
    sudo btrfs filesystem resize "${size}M" Arkbuild/
    if [ $? -eq 0 ]; then
      tsize=$((size + 350))
      ((i++)) || true
    else
      if [[ -z $tsize ]] && [[ $count -le 4 ]]; then
        sudo btrfs balance start --full-balance Arkbuild
        sudo sync Arkbuild
        ((count++)) || true
        i=0
      else
        break
      fi
    fi
  done
  #verify_action
  sync Arkbuild
  if [[ ! -z $tsize ]]; then
    sudo truncate -s ${tsize}MB ${FILESYSTEM}
  else
    printf "\n\nFailed to resize Arkbuild.  Exiting...\n\n"
    exit 1
  fi
  sync Arkbuild
  sudo dd if="${FILESYSTEM}" of="${DISK}" bs=512 seek="${STORAGE_PART_START}" conv=fsync,notrunc
fi
sync ${DISK}