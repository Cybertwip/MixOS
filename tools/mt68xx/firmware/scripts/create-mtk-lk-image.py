#!/usr/bin/env python3
"""Wrap the MVII minimal LK in a MediaTek UBOOT/LK-slot image.

One payload, unlike the older two-payload takeover wrapper: the minimal LK
(OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mvii_lk_main.c) is a bootloader in
its own right and loads boot.img out of the BOOTIMG slot at run time, so nothing
else has to be smuggled into the UBOOT slot alongside it.

Layout, matching what the preloader and mt6592_bootstatus.h expect:

    0x000000  MediaTek image header (magic 58881688, payload size, name "LK")
    0x000200  the LK binary, loaded to 0x81e00000 and branched to
              ... zero padding ...
    slot_end - 0x10000   console ring (cleared by this padding)
    slot_end - 0x200     boot-status sector, baked to FLASH_PENDING

Padding the image to the full slot is deliberate: it wipes the previous boot's
console ring and status record, so `flash -mtk-read-boot-status` after a flash
can never show stale telemetry from the image that was just replaced.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


MTK_MAGIC = b"\x88\x16\x88\x58"
MTK_HEADER_SIZE = 512
BOOT_STATUS_MAGIC = 0x5342374D
BOOT_STATUS_VERSION = 3
BOOT_STATUS_SIZE = 512
BOOT_STATUS_STAGE_FLASH_PENDING = 0x1001
BOOT_STATUS_FLAG_WRITTEN_BY_FLASH = 1 << 3
BOOT_STATUS_MESSAGE_OFFSET = 96


def parse_u32(value: str) -> int:
    return int(value, 0) & 0xFFFFFFFF


def mtk_header(name: str, payload_size: int) -> bytes:
    raw = bytearray(MTK_HEADER_SIZE)
    raw[0:4] = MTK_MAGIC
    struct.pack_into("<I", raw, 4, payload_size)
    raw[8 : 8 + min(len(name), 31)] = name.encode("ascii")[:31]
    raw[40:] = b"\xff" * (MTK_HEADER_SIZE - 40)
    return bytes(raw)


def boot_status_sector(message: bytes) -> bytes:
    raw = bytearray(BOOT_STATUS_SIZE)
    struct.pack_into(
        "<IIIIII",
        raw,
        0,
        BOOT_STATUS_MAGIC,
        BOOT_STATUS_VERSION,
        BOOT_STATUS_SIZE,
        BOOT_STATUS_STAGE_FLASH_PENDING,
        BOOT_STATUS_FLAG_WRITTEN_BY_FLASH,
        0,
    )
    raw[BOOT_STATUS_MESSAGE_OFFSET : BOOT_STATUS_MESSAGE_OFFSET + len(message)] = message
    return bytes(raw)


def build_minimal_lk_image(loader: bytes, *, slot_size: int, name: str) -> bytes:
    if not loader:
        raise ValueError("minimal LK binary is empty")
    image = mtk_header(name, len(loader)) + loader
    limit = slot_size - BOOT_STATUS_SIZE
    if len(image) > limit:
        raise ValueError(
            f"minimal LK image 0x{len(image):x} reaches the reserved boot-status "
            f"sector at 0x{limit:x} in a 0x{slot_size:x} slot"
        )
    image += b"\x00" * (limit - len(image))
    image += boot_status_sector(b"minimal LK image baked FLASH_PENDING")
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", required=True, type=Path, help="minimal LK binary linked at 0x81e00000")
    parser.add_argument("--output", required=True, type=Path, help="output UBOOT/LK-slot image")
    parser.add_argument("--slot-size", default="0x200000", type=parse_u32, help="UBOOT slot size")
    parser.add_argument("--name", default="LK", help="MediaTek image name")
    args = parser.parse_args()

    loader = args.loader.read_bytes()
    image = build_minimal_lk_image(loader, slot_size=args.slot_size, name=args.name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"MediaTek minimal LK image: {args.output} "
        f"({len(image)} bytes, payload={len(loader)} bytes, slot=0x{args.slot_size:x})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
