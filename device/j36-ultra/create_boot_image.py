#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/j36-ultra/LICENSE for the texts and for what they do not cover.
"""Create a stock-LK-compatible MT6592 Android boot.img for J36 bring-up."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

ANDROID_MAGIC = b"ANDROID!"
MTK_MAGIC = b"\x88\x16\x88\x58"
PAGE_SIZE = 2048
BOOTIMG_LIMIT = 0x900000


def pad(data: bytes) -> bytes:
    return data + b"\0" * ((-len(data)) % PAGE_SIZE)


def mtk_header(name: str, size: int) -> bytes:
    data = bytearray(512)
    data[:4] = MTK_MAGIC
    struct.pack_into("<I", data, 4, size)
    data[8:40] = name.encode("ascii")[:31].ljust(32, b"\0")
    data[40:] = b"\xff" * (512 - 40)
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--ramdisk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--cmdline",
        default=(
            "console=tty0 console=ttyS0,115200n8 "
            "earlycon=mtk8250,mmio32,0x11002000 rdinit=/init "
            "loglevel=8 ignore_loglevel"
        ),
    )
    args = parser.parse_args()

    kernel = args.kernel.read_bytes()
    ramdisk = args.ramdisk.read_bytes()
    kernel_payload = mtk_header("KERNEL", len(kernel)) + kernel
    ramdisk_payload = mtk_header("ROOTFS", len(ramdisk)) + ramdisk

    header = bytearray(PAGE_SIZE)
    header[:8] = ANDROID_MAGIC
    values = (
        len(kernel_payload), 0x80008000,
        len(ramdisk_payload), 0x84000000,
        0, 0x80F00000,
        0x80000100, PAGE_SIZE,
        0, 0,
    )
    for index, value in enumerate(values):
        struct.pack_into("<I", header, 8 + index * 4, value)
    header[48:64] = b"j36-ultra".ljust(16, b"\0")
    header[64:576] = args.cmdline.encode("ascii")[:512].ljust(512, b"\0")

    digest = hashlib.sha1()
    for payload in (kernel_payload, ramdisk_payload, b""):
        digest.update(payload)
        digest.update(struct.pack("<I", len(payload)))
    header[576:596] = digest.digest()

    image = bytes(header) + pad(kernel_payload) + pad(ramdisk_payload)
    if len(image) > BOOTIMG_LIMIT:
        raise SystemExit(
            f"boot.img is {len(image)} bytes, larger than the J36 BOOTIMG "
            f"partition ({BOOTIMG_LIMIT} bytes)"
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(
        f"J36 boot.img: {args.output} ({len(image)} bytes; "
        f"kernel={len(kernel)}, ramdisk={len(ramdisk)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
