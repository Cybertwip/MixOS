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
  # The partition is sized by the rule "the rootfs, rounded up to the next whole
  # 1000 MB" -- see STORAGE_SIZE in device/r36-ultra/build-in-vm.sh.  It cannot be
  # applied automatically: the partition table is written in the `image' stage and this
  # is the first moment the rootfs has a size at all.  So the number is computed here
  # and the two cases below say it out loud -- one as a failure, one as a warning --
  # instead of leaving a reader to do the arithmetic.
  fs_next_step=$(( ((fs_mib + 999) / 1000) * 1000 ))
  if [[ ${fs_mib} -gt ${STORAGE_SIZE} ]]; then
    printf "\n\nThe root filesystem needs %s MB and the partition is %s MB.\n" "${fs_mib}" "${STORAGE_SIZE}"
    printf "btrfs hid this behind compress=zlib:1; ext2 does not compress, so the\n"
    printf "rootfs now costs what it actually weighs.\n\n"
    printf "  Set STORAGE_SIZE=%s in device/r36-ultra/build-in-vm.sh\n\n" "${fs_next_step}"
    printf "That is %s MB rounded up to the next whole 1000 MB, which is the rule this\n" "${fs_mib}"
    printf "layout is sized by.  setup_partition.sh reads that value, so it is the only\n"
    printf "place to change.  Taking content out of the build root works too.\n"
    printf "Refusing to dd over the DATA partition.  Exiting...\n\n"
    exit 1
  fi
  if [[ $(( STORAGE_SIZE - fs_mib )) -lt 256 ]]; then
    echo -e "The OS partition has only $(( STORAGE_SIZE - fs_mib )) MB spare, which the J36 payload alone can fill"
    echo -e "Set STORAGE_SIZE=$(( fs_next_step + 1000 )) in device/r36-ultra/build-in-vm.sh if the next build fails on space"
  fi
  sudo truncate -s "${fs_mib}M" "${FILESYSTEM}"
  sync
  # ── bs=8M AND A SEEK IN BYTES ─────────────────────────────────────────────
  #
  # This was `bs=512 seek=${STORAGE_PART_START}', and 512 bytes is not a block
  # size, it is a sector: three gigabytes of rootfs went across as 6.2 million
  # read/write pairs at 13.9 MB/s, which is most of four minutes of a build doing
  # nothing but syscall overhead.  The number was not chosen for throughput -- it
  # was chosen because `seek' counts in blocks, and the partition start is a
  # sector count, so 512 was the only block size that made the seek arithmetic
  # come out right.
  #
  # oflag=seek_bytes decouples the two: the seek is given in bytes, exactly where
  # the partition begins, and the block size is then free to be a real one.  Same
  # bytes, same place, one transfer per eight megabytes instead of per sector.
  # iflag=fullblock because a short read must not become a short block -- with a
  # seek this is the difference between a copy and a corrupted partition.
  sudo dd if="${FILESYSTEM}" of="${DISK}" bs=8M iflag=fullblock \
      oflag=seek_bytes seek=$(( STORAGE_PART_START * 512 )) conv=fsync,notrunc

  # AND THEN GROW IT BACK, INSIDE THE IMAGE.  The shrink above is only about how much of
  # this file the dd has to copy -- 52 GB of build root down to what the rootfs weighs.
  # What it leaves in the partition, though, is a filesystem with ZERO free blocks in a
  # ${STORAGE_SIZE} MB partition, and that is what the card boots on: a read-write root
  # with nothing writable in it.  It surfaced as the J36 payload's first mkdir answering
  # "No space left on device", but nothing about it is J36-specific -- ldconfig, apt, a
  # journal and every dpkg on the R36S would meet the same wall.
  #
  # Nothing on the device fixes it: firstboot resizes the DATA partition, not this one.
  # So it is grown here, to the end of its own partition.
  #
  # The blocks this adds are zeros, and they used to cost nothing because a .7z was what
  # shipped.  The raw image ships now, so they cost their full size -- which is the whole
  # reason STORAGE_SIZE is the rootfs rounded up to the next 1000 MB instead of a flat
  # 7500: unused partition is unused bytes in every copy of the image.
  #
  # The loop device is sizelimited to the partition, so resize2fs cannot run past its end.
  rootfs_loop="$(sudo losetup --find --show \
      --offset $(( STORAGE_PART_START * 512 )) \
      --sizelimit $(( (STORAGE_PART_END - STORAGE_PART_START + 1) * 512 )) "${DISK}")"
  if [[ -n "${rootfs_loop}" ]]; then
    # resize2fs will not touch a filesystem it has not seen checked, and the last thing
    # to touch this one was the dd.  1 and 2 mean "found and fixed", not "broken".
    grow_fsck_rc=0
    sudo e2fsck -p -f "${rootfs_loop}" >/dev/null 2>&1 || grow_fsck_rc=$?
    if [[ ${grow_fsck_rc} -gt 2 ]]; then
      echo -e "e2fsck could not clean the OS partition (exit ${grow_fsck_rc}); leaving it at ${fs_mib} MB"
    elif sudo resize2fs "${rootfs_loop}" >/dev/null 2>&1; then
      echo -e "OS partition filesystem grown from ${fs_mib} MB to fill its ${STORAGE_SIZE} MB partition"
    else
      echo -e "resize2fs could not grow the OS partition; it stays ${fs_mib} MB with no free space"
    fi
    sudo losetup -d "${rootfs_loop}"
  else
    echo -e "Could not attach a loop device to the OS partition; it stays ${fs_mib} MB with no free space"
  fi
  sync
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
  # Same as the ext branch above: a byte seek so the block size can be a block size.
  sudo dd if="${FILESYSTEM}" of="${DISK}" bs=8M iflag=fullblock \
      oflag=seek_bytes seek=$(( STORAGE_PART_START * 512 )) conv=fsync,notrunc
fi
sync ${DISK}