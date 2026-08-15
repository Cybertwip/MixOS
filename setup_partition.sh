#!/bin/bash

echo -e "Creating partitions...\n\n"
# Partition setup
#
# ext2, not btrfs: the J36 Ultra's MVII LK reads FAT32 only, so the card carries a FAT32
# BOOT partition purely as the launcher, and the OS partition is then free to be the
# simplest filesystem the two kernels sharing this card both handle.  See the longer
# note in device/r36-ultra/build-in-vm.sh, which sets the same three values for the
# checkpointed build and is what an R36/J36 build actually runs.
ROOT_FILESYSTEM_FORMAT="ext2"
if [ "$ROOT_FILESYSTEM_FORMAT" == "xfs" ] || [ "$ROOT_FILESYSTEM_FORMAT" == "btrfs" ]; then
  if [ "$ROOT_FILESYSTEM_FORMAT" != "btrfs" ]; then
    ROOT_FILESYSTEM_FORMAT_PARAMETERS="-f -L ROOTFS"
    ROOT_FILESYSTEM_MOUNT_OPTIONS="defaults,noatime"
  else
    # Disable free-space-tree
    # In some btrfs-progs versions, this is listed as a filesystem feature (-O)
    if sudo mkfs.btrfs -O list-all 2>&1 | grep -q "free-space-tree"; then
      ROOT_FILESYSTEM_FORMAT_PARAMETERS="-O ^free-space-tree -f -L ROOTFS"
    # In some btrfs-progs versions, this is listed as a runtime features (-R)
    elif sudo mkfs.btrfs -R list-all 2>&1 | grep -q "free-space-tree"; then
      ROOT_FILESYSTEM_FORMAT_PARAMETERS="-R ^free-space-tree -f -L ROOTFS"
    else
      ROOT_FILESYSTEM_FORMAT_PARAMETERS="-f -L ROOTFS"
    fi
    ROOT_FILESYSTEM_MOUNT_OPTIONS="defaults,noatime,compress=zlib:1"
  fi
elif [[ "$ROOT_FILESYSTEM_FORMAT" == *"ext"* ]]; then
  ROOT_FILESYSTEM_FORMAT_PARAMETERS="-F -L ROOTFS"
  ROOT_FILESYSTEM_MOUNT_OPTIONS="defaults,noatime"
fi
# ── THERE IS NO THIRD PARTITION ANY MORE ──────────────────────────────────────
#
# p3 was EASYROMS, then DATA: a separate ext2 partition mounted at /home/virtua, sized
# 300 MB in the image and grown to the end of the card on the device.  It is gone, and
# with it every DATA_* variable this file used to export.
#
# WHY.  Two partitions were being grown into one card and only one of them could have
# the slack.  ROOTFS was the one that could not -- it is not the last partition when p3
# is there -- so the image had to carry its own package headroom, which is how
# STORAGE_SIZE reached 16000 MB and the artifact reached 16.5 GB.  Deleting p3 makes
# ROOTFS the last partition on the disk, which is the whole precondition for growing it
# in place: the image ships at the size of what is in it, and the card's real capacity
# is handed to the OS partition on the first boot instead.
#
# WHAT REPLACES IT.  Nothing, at the block layer.  /home/virtua is an ordinary directory
# on the rootfs -- which is what it always was underneath the mount -- so a login lands
# on the same tree it did before, on a partition that now has the whole card behind it.
# The account is still `virtua', uid 1000, named after the directory rather than the
# other way round; a card written before that still has `ark', which is why the J36's USB
# mount helper looks the login user up instead of assuming it.
#
# DATA_MOUNT_POINT survives as a path and nothing else.  bootstrap_rootfs.sh and
# finishing_touches.sh write dotfiles into it, and spelling it out in twenty places is
# how the old `ark'/`virtua' split happened.  It names a directory now, not a mount.
DATA_MOUNT_POINT="${DATA_MOUNT_POINT:-/home/virtua}"

SYSTEM_SIZE=100      # FAT32 boot partition size in MB
# The OS partition, and the number that decides how big the shipped image is.
#
# THE RULE: the rootfs, rounded up to the next whole GiB.  Nothing else.  A build that
# measures 3384 MB gets 4096, and the shipped image is 4.2 GB end to end.
#
# It used to be 16000, and the reasoning behind that number was sound for the layout it
# was written in: the partition had to carry every package anybody would ever install,
# because p3 sat immediately after it and there was nowhere for it to grow.  So 12 GB of
# emptiness was shipped, compressed, downloaded and written to a card by every single
# person who flashed it, on the chance that some of them would use it.
#
# p3 is gone (see above), so ROOTFS is the last partition on the disk and the initramfs
# grows it to the end of the card on the first boot -- see setup_expand() in
# device/j36-ultra/build-in-vm.sh.  The headroom now comes from the card the operator
# actually bought, which is both bigger than 12 GB and free.  What the image has to hold
# is the base system and a little slack for the first boot before the resize lands, and
# a whole GiB of rounding is more than enough of that.
#
# The floor is still enforced from below: write_rootfs.sh refuses to dd a rootfs that
# does not fit and prints the exact value to put here.
#
# Assigned conditionally so that a caller which already set it wins: this file is
# sourced by the checkpointed build in device/r36-ultra/build-in-vm.sh, which sets the
# same geometry for the runs where this stage is skipped.  Two unconditional copies of
# one number is one number that goes stale, and a build whose partition table and image
# disagree about where p2 ends writes a filesystem off the end of the disk.
STORAGE_SIZE="${STORAGE_SIZE:-4096}"   # Root filesystem size in MB
BUILD_SIZE=52000     # Initial file system size in MB during the build.  Then will be reduced to the DISK_SIZE or below upon completion

SYSTEM_PART_START=32768
SYSTEM_PART_END=$(( SYSTEM_PART_START + (SYSTEM_SIZE * 1024 * 1024 / 512) - 1 ))
STORAGE_PART_START=$(( SYSTEM_PART_END + 1 ))
STORAGE_PART_END=$(( STORAGE_PART_START + (STORAGE_SIZE * 1024 * 1024 / 512) - 1 ))

DISK_START_PADDING=$(( (SYSTEM_PART_START + 2048 - 1) / 2048 ))
DISK_SIZE=$(( DISK_START_PADDING + SYSTEM_SIZE + STORAGE_SIZE + 1 ))
# Device-specific builders may supply a profile-specific filesystem name so a
# minimal image cannot accidentally reuse a full application build.
FILESYSTEM="${FILESYSTEM:-MixOS_File_System.img}"

# Create filesystem image
dd if=/dev/zero of="${FILESYSTEM}" bs=1M count=0 seek="${BUILD_SIZE}" conv=fsync
sudo mkfs.${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_FORMAT_PARAMETERS} "${FILESYSTEM}"
mkdir -p MixOSBuild/
sudo mount -t ${ROOT_FILESYSTEM_FORMAT} -o ${ROOT_FILESYSTEM_MOUNT_OPTIONS},loop ${FILESYSTEM} MixOSBuild/
