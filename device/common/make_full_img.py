#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""Assemble a phone's full OS image: one GPT-partitioned file holding the
Android boot.img bytes (p1, BOOT) and the ext4 trixie rootfs bytes (p2,
ROOTFS), each placed verbatim so splitting recovers flashable inputs.

This is the phone analog of the J36 card image: one MixOS_<arch>_<debian>_
<commit>.img per model. It is NOT written to the phone's eMMC whole -- the
eMMC keeps its stock partition table and bootloader. It is split on the
workstation (the dd commands print below the build) and the parts go to
their partitions: boot.img via `fastboot flash boot`, the rootfs onto a
PARTLABEL=ROOTFS partition.

Layout, all 1 MiB aligned: protective MBR, GPT header + 128 entries,
p1 BOOT, p2 ROOTFS, backup GPT. Pure struct/zlib, no sgdisk/sfdisk: the
format is ~200 significant bytes and depending on a partitioner for that
is what mkbootimg.py already refused for the boot header.

Usage:
    make_full_img.py --boot boot.img --rootfs trixie.img --output full.img
Prints KEY=VALUE (sectors, for dd bs=512): boot_skip, boot_count,
rootfs_skip, rootfs_count, image_bytes.
"""

from __future__ import annotations

import argparse
import struct
import sys
import uuid
import zlib

SECTOR = 512
ALIGN_SECTORS = 2048  # 1 MiB
N_ENTRIES = 128
ENTRY_SIZE = 128
HEADER_SIZE = 92
# Linux filesystem partition type, mixed-endian on disk (bytes_le).
LINUX_FS_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le

PMBR_FMT = "<446sB3sB3sII48sH"
HEADER_FMT = "<8sIIIIQQQQ16sQIII"
ENTRY_FMT = "<16s16sQQQ72s"


def round_up(n: int, unit: int) -> int:
    return (n + unit - 1) // unit * unit


def protective_mbr(total_sectors: int) -> bytes:
    size32 = min(total_sectors - 1, 0xFFFFFFFF)
    return struct.pack(
        PMBR_FMT, b"\x00" * 446,
        0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff",
        1, size32, b"\x00" * 48, 0xAA55,
    )


def gpt_header(total: int, disk_guid: bytes, entries_crc: int,
               current: int, backup: int) -> bytes:
    last = total - 1
    hdr = struct.pack(
        HEADER_FMT, b"EFI PART", 0x00010000, HEADER_SIZE, 0, 0,
        current, backup, 34, total - 34, disk_guid, 2,
        N_ENTRIES, ENTRY_SIZE, entries_crc,
    )
    crc = zlib.crc32(hdr[:HEADER_SIZE]) & 0xFFFFFFFF
    hdr = hdr[:16] + struct.pack("<I", crc) + hdr[20:HEADER_SIZE]
    return hdr + b"\x00" * (SECTOR - HEADER_SIZE)


def entry(type_guid: bytes, first: int, count: int, name: str) -> bytes:
    # last_lba is INCLUSIVE -- first+count-1, not first+count.
    return struct.pack(
        ENTRY_FMT, type_guid, uuid.uuid4().bytes_le,
        first, first + count - 1, 0,
        name.encode("utf-16-le")[:72].ljust(72, b"\x00"),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble a phone full OS image")
    ap.add_argument("--boot", required=True)
    ap.add_argument("--rootfs", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--boot-name", default="BOOT")
    ap.add_argument("--rootfs-name", default="ROOTFS")
    args = ap.parse_args()

    try:
        with open(args.boot, "rb") as f:
            boot = f.read()
        with open(args.rootfs, "rb") as f:
            rootfs = f.read()
    except OSError as e:
        print(f"make_full_img: {e}", file=sys.stderr)
        return 1
    if not boot or not rootfs:
        print("make_full_img: refusing an empty input", file=sys.stderr)
        return 1

    boot_start = ALIGN_SECTORS
    # Sectors first, then alignment: rounding the byte count to a multiple
    # of 2048 and calling it sectors inflates a 3 MiB part to 1.6 GiB.
    boot_count = round_up((len(boot) + SECTOR - 1) // SECTOR, ALIGN_SECTORS)
    rootfs_start = boot_start + boot_count
    rootfs_count = round_up((len(rootfs) + SECTOR - 1) // SECTOR, ALIGN_SECTORS)
    total = rootfs_start + rootfs_count + ALIGN_SECTORS

    disk_guid = uuid.uuid4().bytes_le
    entries = (
        entry(LINUX_FS_GUID, boot_start, boot_count, args.boot_name)
        + entry(LINUX_FS_GUID, rootfs_start, rootfs_count, args.rootfs_name)
        + b"\x00" * ENTRY_SIZE * (N_ENTRIES - 2)
    )
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    last = total - 1

    image = bytearray(total * SECTOR)
    image[0:SECTOR] = protective_mbr(total)
    image[SECTOR:2 * SECTOR] = gpt_header(total, disk_guid, entries_crc, 1, last)
    image[2 * SECTOR:34 * SECTOR] = entries
    image[boot_start * SECTOR:boot_start * SECTOR + len(boot)] = boot
    image[rootfs_start * SECTOR:rootfs_start * SECTOR + len(rootfs)] = rootfs
    image[(last - 32) * SECTOR:last * SECTOR] = entries
    image[last * SECTOR:] = gpt_header(total, disk_guid, entries_crc, last, 1)

    with open(args.output, "wb") as f:
        f.write(image)
    print(f"boot_skip={boot_start}")
    print(f"boot_count={boot_count}")
    print(f"rootfs_skip={rootfs_start}")
    print(f"rootfs_count={rootfs_count}")
    print(f"image_bytes={len(image)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
