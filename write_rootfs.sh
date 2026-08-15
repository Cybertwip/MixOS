#!/bin/bash

# Write rootfs to disk
sync MixOSBuild
if [ "${ROOT_FILESYSTEM_FORMAT}" == "xfs" ]; then
  mkdir MixOSBuild-final
  sudo mount -o loop ${LOOP_DEV}p4 MixOSBuild-final/
  sudo rsync -av --exclude={'home/virtua/MixOSBuild_ccache','proc','dev','sys'} MixOSBuild/ MixOSBuild-final/
  sudo umount MixOSBuild-final/
  sudo rm -rf MixOSBuild-final/
elif [[ "${ROOT_FILESYSTEM_FORMAT}" == *"ext"* ]]; then
  # e2fsck and resize2fs both need the filesystem OFF-LINE, and nothing before this
  # point unmounts it -- clean_mounts.sh, which does, runs after this script.  Handing
  # a mounted image to e2fsck is not caught for us either: ext2fs_check_if_mounted()
  # looks its argument up in /proc/mounts, and what is listed there is the autoloop
  # device, not this file, so e2fsck would cheerfully "repair" a live filesystem
  # underneath the mount.  Unmount first.  remove_mixosbuild guards every umount on
  # /proc/mounts, so clean_mounts.sh calling it again a few lines later is a no-op.
  echo -e "Unmounting the build root before checking and shrinking it"
  remove_mixosbuild
  if [[ "${BUILD_ARMHF}" == "y" ]]; then
    remove_mixosbuild32
  fi
  # umount -l only promises to detach the tree from the namespace; the filesystem can
  # still be in flight behind it, and the autoloop device stays until the last
  # reference goes away.  Wait for both, then take down any loop still holding the
  # image -- e2fsck reads the file, and a loop device with dirty pages would have it
  # reading a version of the filesystem that is not the one on disk.
  for _ in $(seq 1 30); do
    mountpoint -q MixOSBuild || break
    sync
    sleep 1
  done
  if mountpoint -q MixOSBuild; then
    printf "\n\nMixOSBuild is still mounted; refusing to fsck a live filesystem.  Exiting...\n\n"
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
  # ── SHRINK ONCE, TO THE SIZE IT IS GOING TO BE ────────────────────────────────
  #
  # This was `resize2fs -M' followed, sixty lines down, by a grow back to the size of
  # the partition -- shrink the filesystem to the smallest it can possibly be, copy
  # that into the image, then attach a loop device to the partition, fsck it again and
  # grow it back out.  Three filesystem passes and two full checks to arrive at a
  # filesystem exactly ${STORAGE_SIZE} MB across, which is a size that was known before
  # any of it started.
  #
  # Shrinking straight to ${STORAGE_SIZE} MB gets to the same place in one pass, and it
  # is a strictly cheaper pass than -M was: every block -M relocates is one this has to
  # relocate too, plus all the ones between the partition size and the minimum that
  # this one can leave exactly where they are.  -M also spends real time just deciding
  # what the minimum IS -- an iterative estimate before it moves anything -- and that
  # answer was then used for nothing except a diagnostic, which `-P' prints without
  # touching the filesystem at all.
  #
  # The dd copies ${STORAGE_SIZE} MB instead of the shrunk size, so the difference goes
  # across as zeros.  That was a few hundred MB and a couple of seconds when the
  # partition was 4000 MB; at 16000 it is around 12 GB and minutes, which is the price
  # of a card that has room for the packages this device exists to install.  It still
  # buys back the second e2fsck of the whole root, the second resize2fs, the loop
  # device, and every "it stays that size with no free space" half-failure that block
  # could end in.
  #
  # THE FILESYSTEM MUST FILL ITS PARTITION and not merely fit in it -- that is what the
  # grow-back was for.  A rootfs left at its minimum has ZERO free blocks, and the card
  # boots on it: a read-write root with nothing writable in it.  It surfaced as the J36
  # payload's first mkdir answering "No space left on device", but nothing about it was
  # J36-specific -- ldconfig, apt, a journal and every dpkg on the R36S meet the same
  # wall.  Nothing on the device fixes it either; firstboot resizes the DATA partition,
  # not this one.  Sizing the shrink to the partition satisfies that by construction,
  # which is the other reason this is the better shape: the free space is no longer
  # something a later step has to remember to add back.
  fs_geometry="$(dumpe2fs -h "${FILESYSTEM}" 2>/dev/null)"
  fs_bsize="$(printf '%s\n' "${fs_geometry}" | awk -F: '/^Block size:/{gsub(/ /,"",$2); print $2}')"
  if [[ -z "${fs_bsize}" ]]; then
    printf "\n\nCould not read the block geometry of %s.  Exiting...\n\n" "${FILESYSTEM}"
    exit 1
  fi
  # -P is read-only: it prints the estimate -M would have shrunk to and moves nothing.
  fs_min_blocks="$(resize2fs -P "${FILESYSTEM}" 2>/dev/null | awk -F: '/minimum size/{gsub(/ /,"",$2); print $2}')"
  fs_min_blocks="${fs_min_blocks:-0}"
  # Rounded up to a whole MiB: a size in MiB is the number a human compares against
  # STORAGE_SIZE without arithmetic.
  fs_mib=$(( ((fs_min_blocks * fs_bsize) + 1048575) / 1048576 ))
  echo -e "Root filesystem holds ${fs_mib} MB of the ${STORAGE_SIZE} MB partition"
  # THE PARTITION IS NOT SIZED FOR THE ROOTFS, and that is the whole point of the
  # numbers below.  It used to be -- "the rootfs, rounded up to the next whole 1000 MB"
  # -- and the card that rule produced shipped with 240 MB free, 200 of which is
  # mkfs.ext2's root reservation, on a device whose Packages page exists so that people
  # install things on it.  STORAGE_SIZE in device/r36-ultra/build-in-vm.sh is now sized
  # for the base system plus roughly 12 GB of somebody else's packages; the arithmetic
  # here is only the FLOOR, the point below which the dd would run over the DATA
  # partition.  It cannot be applied automatically either way: the partition table is
  # written in the `image' stage and this is the first moment the rootfs has a size at
  # all.  So the two cases below say the numbers out loud -- one as a failure, one as a
  # warning -- instead of leaving a reader to do the arithmetic.
  fs_next_step=$(( ((fs_mib + 999) / 1000) * 1000 ))
  # What the layout wants spare for packages, on top of whatever the base system
  # weighs.  Named rather than inlined so the two messages below cannot drift apart
  # from each other or from the comment beside STORAGE_SIZE.
  fs_package_headroom=12000
  if [[ ${fs_min_blocks} -gt 0 ]]; then
    if [[ ${fs_mib} -gt ${STORAGE_SIZE} ]]; then
      printf "\n\nThe root filesystem needs %s MB and the partition is %s MB.\n" "${fs_mib}" "${STORAGE_SIZE}"
      printf "btrfs hid this behind compress=zlib:1; ext2 does not compress, so the\n"
      printf "rootfs now costs what it actually weighs.\n\n"
      printf "  Set STORAGE_SIZE=%s in device/r36-ultra/build-in-vm.sh\n\n" \
             "$(( fs_next_step + fs_package_headroom ))"
      printf "That is %s MB rounded up to the next whole 1000 MB, plus the %s MB of\n" \
             "${fs_mib}" "${fs_package_headroom}"
      printf "package headroom this layout is sized by -- see the comment beside\n"
      printf "STORAGE_SIZE for why the headroom is the rule and not the leftovers.\n"
      printf "setup_partition.sh reads that value, so it is the only place to change.\n"
      printf "Taking content out of the build root works too.\n"
      printf "Refusing to dd over the DATA partition.  Exiting...\n\n"
      exit 1
    fi
    if [[ $(( STORAGE_SIZE - fs_mib )) -lt ${fs_package_headroom} ]]; then
      echo -e "The OS partition has $(( STORAGE_SIZE - fs_mib )) MB spare, and this layout is sized for ${fs_package_headroom} MB of packages"
      echo -e "Set STORAGE_SIZE=$(( fs_next_step + fs_package_headroom )) in device/r36-ultra/build-in-vm.sh to get it back"
    fi
  else
    echo -e "resize2fs -P did not report a minimum size; the shrink below is the real check"
  fi

  # In blocks, not `${STORAGE_SIZE}M', because the partition table counts sectors and
  # this has to land on the same byte it does: STORAGE_PART_END is derived from
  # STORAGE_SIZE * 1024 * 1024, so the filesystem is asked for exactly that many bytes
  # in its own block size and there is no suffix convention in the middle to disagree
  # about.
  fs_target_blocks=$(( STORAGE_SIZE * 1024 * 1024 / fs_bsize ))
  echo -e "Shrinking the root filesystem to the ${STORAGE_SIZE} MB of its partition"
  resize2fs "${FILESYSTEM}" "${fs_target_blocks}" || {
    printf "\n\nFailed to shrink %s to %s MB.\n\n" "${FILESYSTEM}" "${STORAGE_SIZE}"
    printf "If resize2fs said the new size is smaller than the minimum, the build root\n"
    printf "outgrew its partition: raise STORAGE_SIZE in device/r36-ultra/build-in-vm.sh\n"
    printf "to the next whole 1000 MB above the minimum it printed, or take content out\n"
    printf "of the build root.  Exiting...\n\n"
    exit 1
  }

  # resize2fs shrank the FILESYSTEM.  The FILE is still BUILD_SIZE -- 52 GB -- and the
  # dd below copies the file, not the filesystem, so without this it wrote 52 GB from
  # STORAGE_PART_START: over the ROMS partition and off the end of the image.  The
  # btrfs branch truncates before its dd; this one never did, because nothing had put
  # an ext rootfs through it yet.
  sudo truncate -s "${STORAGE_SIZE}M" "${FILESYSTEM}"
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
  echo -e "OS partition written: ${STORAGE_SIZE} MB of filesystem holding ${fs_mib} MB, so $(( STORAGE_SIZE - fs_mib )) MB free on the card"
  sync
elif [ "${ROOT_FILESYSTEM_FORMAT}" == "btrfs" ]; then
  sudo btrfs balance start --full-balance MixOSBuild
  sudo sync MixOSBuild
  sizes=(8000 7700 7300 7250 7100)
  i=0
  count=0
  while [[ $i -lt ${#sizes[@]} ]]; do
    size=${sizes[$i]}
    sudo btrfs filesystem resize "${size}M" MixOSBuild/
    if [ $? -eq 0 ]; then
      tsize=$((size + 350))
      ((i++)) || true
    else
      if [[ -z $tsize ]] && [[ $count -le 4 ]]; then
        sudo btrfs balance start --full-balance MixOSBuild
        sudo sync MixOSBuild
        ((count++)) || true
        i=0
      else
        break
      fi
    fi
  done
  #verify_action
  sync MixOSBuild
  if [[ ! -z $tsize ]]; then
    sudo truncate -s ${tsize}MB ${FILESYSTEM}
  else
    printf "\n\nFailed to resize MixOSBuild.  Exiting...\n\n"
    exit 1
  fi
  sync MixOSBuild
  # Same as the ext branch above: a byte seek so the block size can be a block size.
  sudo dd if="${FILESYSTEM}" of="${DISK}" bs=8M iflag=fullblock \
      oflag=seek_bytes seek=$(( STORAGE_PART_START * 512 )) conv=fsync,notrunc
fi
sync ${DISK}