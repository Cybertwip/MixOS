#!/bin/bash

echo -e "Setup the Image file...\n\n"

# Image creation
iName=`echo ${UNIT} | tr '[:lower:]' '[:upper:]'`
# Allow checkpoint/profile builders to keep their output images isolated.
DISK="${DISK:-MixOS_${iName}_${DEBIAN_CODE_NAME}_${BUILD_DATE}.img}"
dd if=/dev/zero of="${DISK}" bs=1M count=0 seek="${DISK_SIZE}" conv=fsync
parted -s "${DISK}" mklabel msdos
parted -s "${DISK}" -a min unit s mkpart primary fat32 ${SYSTEM_PART_START} ${SYSTEM_PART_END}
parted -s "${DISK}" set 1 boot on
parted -s "${DISK}" -a min unit s mkpart primary ${ROOT_FILESYSTEM_FORMAT} ${STORAGE_PART_START} ${STORAGE_PART_END}
#parted -s "${DISK}" set 2 lba off
# p3 is ${DATA_FILESYSTEM_FORMAT} -- ext2 -- and not fat32.  parted's fs-type argument
# writes nothing but the MBR type byte, and Linux ignores it and probes the superblock,
# so this was invisible on the device.  It is not invisible on a PC: a partition flagged
# 0x0B with an ext2 superblock in it is what makes Windows and macOS offer to reformat
# the card.  0x83 says Linux, which is what this partition is.
parted -s "${DISK}" -a min unit s mkpart primary ${DATA_FILESYSTEM_FORMAT} ${ROM_PART_START} ${ROM_PART_END}
sync



# Build uboot and install it to the image.
#
# Every step below is checked, and this stage is sourced by run_stage, so `return 1'
# aborts the build with a non-zero status the way an ordinary script's `exit 1' would.
# It used to end on `rm -rf u-boot-${CHIPSET}' -- a command that cannot fail -- so the
# stage's exit status said nothing about whether a bootloader had been produced.  A
# clone that could not reach github, or a make.sh that died on a toolchain change, wrote
# three "No such file or directory" lines from dd and then marked `image.done'.  What
# came out was a card with a correct partition table, a populated rootfs and nothing in
# the first 32 MB: no console output at power-on, no way to tell it from a bad flash.
# The same shape of silence is what let an empty rootfs get marked `bootstrap.done'.
#
# `cd' is checked for a second reason.  run_stage sources these scripts into the build's
# own shell, so a `cd u-boot-rk3326' that fails does not stop the script -- it leaves
# every later command running one directory up, and the `cd ..' at the bottom then walks
# the whole build out of its tree.
rm -rf u-boot-${CHIPSET}
if [ "$UNIT" == "rg351mp" ] || [ "$UNIT" == "g350" ] || [ "$UNIT" == "a10mini" ]; then
  git clone --depth=1 https://github.com/christianhaitian/RG351MP-u-boot u-boot-${CHIPSET} || return 1
else
  git clone --depth=1 https://github.com/christianhaitian/u-boot-${CHIPSET} || return 1
fi
cd u-boot-${CHIPSET} || return 1
if ! ./make.sh odroidgoa; then
  cd ..
  echo "u-boot did not build; refusing to mark the image stage done." >&2
  return 1
fi

for blob in sd_fuse/idbloader.img sd_fuse/uboot.img sd_fuse/trust.img; do
  if [ ! -s "$blob" ]; then
    cd ..
    echo "u-boot built but produced no ${blob}; the image would have no bootloader." >&2
    return 1
  fi
done

dd if="sd_fuse/idbloader.img" of="../${DISK}" bs=512 seek=64 conv=sync,noerror,notrunc || { cd ..; return 1; }
dd if="sd_fuse/uboot.img" of="../${DISK}" bs=512 seek=16384 conv=sync,noerror,notrunc || { cd ..; return 1; }
dd if="sd_fuse/trust.img" of="../${DISK}" bs=512 seek=24576 conv=sync,noerror,notrunc || { cd ..; return 1; }
if [ "$UNIT" == "rg351mp" ] || [ "$UNIT" == "g350" ] || [ "$UNIT" == "a10mini" ]; then
  cp arch/arm/dts/${UNIT}-uboot.dtb /tmp/ || { cd ..; return 1; }
fi
cd .. || return 1
rm -rf u-boot-${CHIPSET}
echo "Image ${DISK} partitioned and u-boot written to it."
