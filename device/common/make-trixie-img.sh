#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Build a phone's trixie image: an ext4 filesystem holding a Debian rootfs
# tree, written to a phone partition with dd or flashed with fastboot.
#
# Usage: make-trixie-img.sh ROOTFS_DIR OUTPUT_IMG [LABEL]
#
# This is a filesystem image, not a disk image, and that is deliberate. The
# console builds hand over whole partitioned cards; a phone's eMMC already
# has a partition table holding its bootloader, so writing a disk image to
# it would brick the phone. The one partition this fills must be named
# ROOTFS in the GPT (PARTLABEL=ROOTFS, what /init looks for) and hold at
# least the byte count this script prints.
#
# Sizing: measured used kilobytes plus half again, plus 128 MiB for the
# journal and the superblock tables, minimum 256 MiB, rounded up to 4 MiB.
# Fixed at build time: /init does not grow it (resize2fs from the booted
# system if the partition has room to spare).
set -Eeuo pipefail

ROOTFS="${1:?usage: make-trixie-img.sh ROOTFS_DIR OUTPUT_IMG [LABEL]}"
OUTPUT="${2:?usage: make-trixie-img.sh ROOTFS_DIR OUTPUT_IMG [LABEL]}"
LABEL="${3:-ROOTFS}"

[[ -d "$ROOTFS" ]] || { echo "make-trixie-img: no such rootfs: $ROOTFS" >&2; exit 1; }
command -v mke2fs >/dev/null || { echo "make-trixie-img: mke2fs missing (e2fsprogs)" >&2; exit 1; }

used_kb="$(du -sk -- "$ROOTFS" | cut -f1)"
img_kb=$(( used_kb + used_kb / 2 + 131072 ))
[[ "$img_kb" -lt 262144 ]] && img_kb=262144
img_kb=$(( (img_kb + 4095) / 4096 * 4096 ))

rm -f "$OUTPUT"
# -F: the output is a regular file, never a block device, so the "this is
# not a block device, proceed anyway?" prompt must never appear in a build.
mke2fs -q -F -t ext4 -L "$LABEL" -d "$ROOTFS" "$OUTPUT" "${img_kb}k"
if command -v e2fsck >/dev/null; then
    e2fsck -n -f "$OUTPUT" >/dev/null
fi
# GNU first: on Linux `stat -f' means filesystem mode and SUCCEEDS, printing
# a filesystem dump instead of the size. (BSD `stat -c' fails, so macOS
# still falls through to the second spelling.)
bytes="$(stat -c %s "$OUTPUT" 2>/dev/null || stat -f %z "$OUTPUT")"
echo "make-trixie-img: $OUTPUT ($bytes bytes, rootfs used ${used_kb}k, label $LABEL)"
