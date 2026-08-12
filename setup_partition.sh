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
# The third partition -- p3, historically EASYROMS -- is ext2 and labelled DATA now,
# on the same reasoning as the rootfs and one more besides: it holds Linux content
# (roms, themes, tools, bios) with ownership and execute bits that vfat cannot express,
# which is why the old flow formatted it vfat here and had firstboot immediately
# reformat it to exfat and untar /roms.tar back onto it.  ext2 from the start removes
# that whole round trip, and this kernel and the R36S's both mount it as they are.
#
# Kept as its own set of variables and not folded into ROOT_FILESYSTEM_*: the two
# partitions are allowed to diverge, and a reader looking at p3 should not have to
# work out whether the rootfs's -b 4096 was meant for it as well.
DATA_LABEL="DATA"
DATA_FILESYSTEM_FORMAT="ext2"
# -b 1024 and not 4096: p3 starts at 300 MB and holds many small files, so the
# smaller block wastes less in tails.  resize2fs grows it in these blocks on the
# device just as happily.
DATA_FILESYSTEM_FORMAT_PARAMETERS="-F -b 1024 -L ${DATA_LABEL}"
# No umask/uid/gid: those are vfat and exfat options, and mount REFUSES them on ext2
# -- an fstab line carrying them is a partition that does not come up at all.  ext2
# stores real ownership instead, so the build chowns the tree to uid/gid 1000 once and
# the ark user can write to it from then on.
#
# nofail, which the old /roms line did not have: this partition is now the user's home
# and not a rom library, and a card whose p3 is missing, unformatted or half-written
# must still reach a shell.  Without it systemd holds local-fs.target and the boot
# stops at exactly the point the operator can no longer investigate.
DATA_MOUNT_OPTIONS="defaults,auto,noatime,nofail"
# /home/virtua and not /roms.  The mount point IS the login user's home directory, so a
# shell lands on this partition: it is where bash starts, where the file explorer opens,
# and the one place on the card meant to be written.  /roms was EmulationStation's name
# for it and MixOS does not run EmulationStation -- the legacy tree survives as roms/
# inside this partition, with /roms a symlink to it for the RK3326 scripts that still
# say so.  Under /home rather than a top-level /virtua because it is a home directory
# and nothing else, and $HOME under /home is what every tool on the card assumes.
#
# The account is still named ark, uid 1000: the name appears in ~800 lines of the
# emulator build scripts, in the sudoers drop-ins and in six group memberships, and
# renaming it buys nothing the home directory move does not already give.  /home/ark
# is a symlink to here, so those lines keep resolving.
DATA_MOUNT_POINT="/home/virtua"

SYSTEM_SIZE=100      # FAT32 boot partition size in MB
# The OS partition, and the number that decides how big the shipped image is.
#
# HOW 4000 WAS ARRIVED AT, AND THE RULE FOR CHANGING IT.  write_rootfs.sh shrinks the
# build root to what it actually weighs and prints that: "Root filesystem shrank to
# 3384 MB".  4000 is that number rounded up to the next whole 1000 MB, which is the
# rule -- keep the base system compact and give it one round step of headroom, no more.
# It was 7500, which put 4 GB of zeros in every image and 4 GB of nothing on every card;
# that cost nothing while btrfs compressed the image and a .7z shipped it, and both of
# those are gone.  If the rootfs ever outgrows this, write_rootfs.sh refuses to dd over
# the DATA partition and names the next 1000 MB step to put here.
#
# Assigned conditionally so that a caller which already set it wins: this file is
# sourced by the checkpointed build in device/r36-ultra/build-in-vm.sh, which sets the
# same geometry for the runs where this stage is skipped.  Two unconditional copies of
# one number is one number that goes stale, and a build whose partition table and image
# disagree about where p2 ends writes p2 over p3.
STORAGE_SIZE="${STORAGE_SIZE:-4000}"    # Root filesystem size in MB
ROM_PART_SIZE=300    # DATA partition size in MB (ext2, grown by firstboot)
BUILD_SIZE=52000     # Initial file system size in MB during the build.  Then will be reduced to the DISK_SIZE or below upon completion

SYSTEM_PART_START=32768
SYSTEM_PART_END=$(( SYSTEM_PART_START + (SYSTEM_SIZE * 1024 * 1024 / 512) - 1 ))
STORAGE_PART_START=$(( SYSTEM_PART_END + 1 ))
STORAGE_PART_END=$(( STORAGE_PART_START + (STORAGE_SIZE * 1024 * 1024 / 512) - 1 ))
ROM_PART_START=$(( STORAGE_PART_END + 1 ))
ROM_PART_END=$(( ROM_PART_START + (ROM_PART_SIZE * 1024 * 1024 / 512) - 1 ))

DISK_START_PADDING=$(( (SYSTEM_PART_START + 2048 - 1) / 2048 ))
DISK_SIZE=$(( DISK_START_PADDING + SYSTEM_SIZE + STORAGE_SIZE + ROM_PART_SIZE + 1 ))
# Device-specific builders may supply a profile-specific filesystem name so a
# minimal image cannot accidentally reuse a full application build.
FILESYSTEM="${FILESYSTEM:-ArkOS_File_System.img}"

# Create filesystem image
dd if=/dev/zero of="${FILESYSTEM}" bs=1M count=0 seek="${BUILD_SIZE}" conv=fsync
sudo mkfs.${ROOT_FILESYSTEM_FORMAT} ${ROOT_FILESYSTEM_FORMAT_PARAMETERS} "${FILESYSTEM}"
mkdir -p Arkbuild/
sudo mount -t ${ROOT_FILESYSTEM_FORMAT} -o ${ROOT_FILESYSTEM_MOUNT_OPTIONS},loop ${FILESYSTEM} Arkbuild/
