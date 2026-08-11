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



# Build uboot and install it to the image
if [ "$UNIT" == "rg351mp" ] || [ "$UNIT" == "g350" ] || [ "$UNIT" == "a10mini" ]; then
  git clone --depth=1 https://github.com/christianhaitian/RG351MP-u-boot u-boot-${CHIPSET}
else
  git clone --depth=1 https://github.com/christianhaitian/u-boot-${CHIPSET}
fi
cd u-boot-${CHIPSET}
./make.sh odroidgoa

dd if="sd_fuse/idbloader.img" of="../${DISK}" bs=512 seek=64 conv=sync,noerror,notrunc
dd if="sd_fuse/uboot.img" of="../${DISK}" bs=512 seek=16384 conv=sync,noerror,notrunc
dd if="sd_fuse/trust.img" of="../${DISK}" bs=512 seek=24576 conv=sync,noerror,notrunc
if [ "$UNIT" == "rg351mp" ] || [ "$UNIT" == "g350" ] || [ "$UNIT" == "a10mini" ]; then
  cp arch/arm/dts/${UNIT}-uboot.dtb /tmp/
fi
cd ..
rm -rf u-boot-${CHIPSET}
