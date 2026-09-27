#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""Minimal Android boot image packer (header v1).

Why this exists: Ubuntu's `abootimg` package provides `abootimg`, not
`mkbootimg` -- the phone builds died at the boot.img step with
"mkbootimg: command not found". The v1 header is 1632 bytes of struct and
padding; depending on AOSP build tools for that is silly, so it lives
here, shared by the OPPO and LG builds.

Header v1 (not v2): both bootloaders in play -- the MTK LK on the OPPO
side and 2016-era Qualcomm aboot on the LG side -- parse v0/v1. Neither
is promised to understand v2's dtb/dtopro split. The device tree reaches
the kernel appended to it (`cat Image board.dtb > Image-dtb`), which is
how both loaders have found it since forever; this tool just packs
kernel + ramdisk + cmdline.

Usage:
    mkbootimg.py --kernel Image-dtb --ramdisk initramfs.cpio.gz \
        --cmdline "..." --base 0x40000000 --pagesize 4096 \
        --name mixos-20181 --output boot.img
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
# v1: magic[8], 8 size/addr words, page_size, header_version, os_version,
# name[16], cmdline[512], id[32], extra_cmdline[1024].
HEADER_FMT = "<8s10I16s512s32s1024s"
HEADER_VERSION = 1

KERNEL_OFFSET = 0x8000
RAMDISK_OFFSET = 0x1000000
SECOND_OFFSET = 0xF00000
TAGS_OFFSET = 0x100


def pad(data: bytes, page: int) -> bytes:
    over = len(data) % page
    return data if over == 0 else data + b"\x00" * (page - over)


def main() -> None:
    ap = argparse.ArgumentParser(description="Pack an Android boot image (v1)")
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk", required=True)
    ap.add_argument("--second", default=None)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--base", default="0x40000000")
    ap.add_argument("--pagesize", type=int, default=4096)
    ap.add_argument("--name", default="mixos")
    ap.add_argument("--os-version", default="0")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    try:
        with open(args.kernel, "rb") as f:
            kernel = f.read()
        with open(args.ramdisk, "rb") as f:
            ramdisk = f.read()
        second = b""
        if args.second:
            with open(args.second, "rb") as f:
                second = f.read()
    except OSError as e:
        print(f"mkbootimg: {e}", file=sys.stderr)
        return 1

    cmdline = args.cmdline.encode()
    if len(cmdline) > 512 + 1024:
        print("mkbootimg: cmdline too long for v1 header", file=sys.stderr)
        return 1
    name = args.name.encode()[:16]
    base = int(args.base, 0)
    page = args.pagesize

    digest = hashlib.sha1(kernel + ramdisk + second).digest() + b"\x00" * 12
    header = struct.pack(
        HEADER_FMT, BOOT_MAGIC,
        len(kernel), base + KERNEL_OFFSET,
        len(ramdisk), base + RAMDISK_OFFSET,
        len(second), base + SECOND_OFFSET,
        base + TAGS_OFFSET, page,
        HEADER_VERSION, int(args.os_version, 0),
        name, cmdline[:512], digest, cmdline[512:],
    )
    image = pad(header, page) + pad(kernel, page) + pad(ramdisk, page)
    if second:
        image += pad(second, page)
    with open(args.output, "wb") as f:
        f.write(image)
    print(f"mkbootimg: wrote {args.output} "
          f"(kernel {len(kernel)}, ramdisk {len(ramdisk)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
