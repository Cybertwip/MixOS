#!/usr/bin/env python3
"""Check the FAT boot partition of an R36 disk image without mounting it.

The question this answers -- "is there a filesystem in partition 1, and does it
hold the files u-boot is about to load?" -- does not need a loop device, and
reading the MBR and the FAT directly has one property a mount does not: it sees
what is actually inside the artifact that gets archived, rather than what some
earlier stage believed it wrote.  The 08042026 GUI image shipped with an
all-zero BPB in partition 1 while every command in the build reported success,
so the build now has to prove the partition before it archives it.

  verify_boot.py IMAGE --require Image --require uInitrd ...
  verify_boot.py IMAGE --bpb-only

Exit status is 0 when the partition satisfies the request, 1 when it does not,
and 2 for a usage or I/O problem.
"""

import argparse
import struct
import sys

SECTOR = 512
FAT_TYPES = {0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E}

ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_LONG_NAME = 0x0F


class ImageError(Exception):
    """The image cannot be read far enough to answer the question."""


def read_partition(fh, index):
    """Return (type, start_lba, sector_count) for a 1-based MBR partition."""
    fh.seek(0)
    mbr = fh.read(SECTOR)
    if len(mbr) < SECTOR:
        raise ImageError("the image is smaller than a single sector")
    if mbr[510:512] != b"\x55\xaa":
        raise ImageError("no MBR signature; the image was never partitioned")
    if not 1 <= index <= 4:
        raise ImageError("partition index must be 1..4")
    entry = mbr[0x1BE + (index - 1) * 16:0x1BE + index * 16]
    ptype = entry[4]
    start, count = struct.unpack_from("<II", entry, 8)
    if ptype == 0 or count == 0:
        raise ImageError("partition %d is not present in the MBR" % index)
    return ptype, start, count


class Fat:
    """Enough of a FAT12/16/32 reader to list the root directory."""

    def __init__(self, fh, start_lba):
        fh.seek(start_lba * SECTOR)
        bpb = fh.read(SECTOR)
        if len(bpb) < SECTOR:
            raise ImageError("partition starts past the end of the image")
        self.fh = fh
        self.start_lba = start_lba
        self.oem = bpb[3:11].decode("latin1").strip()
        self.bytes_per_sector, self.sectors_per_cluster = struct.unpack_from("<HB", bpb, 11)
        self.reserved = struct.unpack_from("<H", bpb, 14)[0]
        self.fats = bpb[16]
        self.root_entries = struct.unpack_from("<H", bpb, 17)[0]
        total16 = struct.unpack_from("<H", bpb, 19)[0]
        fat16_size = struct.unpack_from("<H", bpb, 22)[0]
        total32 = struct.unpack_from("<I", bpb, 32)[0]
        fat32_size = struct.unpack_from("<I", bpb, 36)[0]
        self.fat_sectors = fat16_size or fat32_size
        self.total_sectors = total16 or total32
        self.is_fat32 = fat16_size == 0
        self.root_cluster = struct.unpack_from("<I", bpb, 44)[0] if self.is_fat32 else 0
        label = bpb[71:82] if self.is_fat32 else bpb[43:54]
        self.label = label.decode("latin1").strip()

    @property
    def formatted(self):
        """A blank BPB is exactly what an unformatted partition looks like."""
        return (
            self.bytes_per_sector in (512, 1024, 2048, 4096)
            and self.sectors_per_cluster > 0
            and self.reserved > 0
            and self.fats > 0
            and self.fat_sectors > 0
            and self.total_sectors > 0
        )

    def describe(self):
        return (
            "FAT%s label=%r oem=%r bytes/sector=%d sectors/cluster=%d "
            "size=%.1f MiB"
            % (
                "32" if self.is_fat32 else "16",
                self.label,
                self.oem,
                self.bytes_per_sector,
                self.sectors_per_cluster,
                self.total_sectors * self.bytes_per_sector / 1048576.0,
            )
        )

    def _root_bytes(self):
        bps = self.bytes_per_sector
        if not self.is_fat32:
            root_lba = self.start_lba + self.reserved + self.fats * self.fat_sectors
            self.fh.seek(root_lba * bps)
            return self.fh.read(self.root_entries * 32)
        self.fh.seek((self.start_lba + self.reserved) * bps)
        fat = self.fh.read(self.fat_sectors * bps)
        first_data = self.start_lba + self.reserved + self.fats * self.fat_sectors
        cluster = self.root_cluster
        data = bytearray()
        seen = set()
        while 2 <= cluster < 0x0FFFFFF8 and cluster not in seen:
            seen.add(cluster)
            self.fh.seek((first_data + (cluster - 2) * self.sectors_per_cluster) * bps)
            data += self.fh.read(self.sectors_per_cluster * bps)
            offset = cluster * 4
            if offset + 4 > len(fat):
                break
            cluster = struct.unpack_from("<I", fat, offset)[0] & 0x0FFFFFFF
        return bytes(data)

    def root_entries_list(self):
        """Return [(name, size, is_dir)] for the root directory."""
        data = self._root_bytes()
        out = []
        long_parts = {}
        for off in range(0, len(data), 32):
            entry = data[off:off + 32]
            if len(entry) < 32 or entry[0] == 0x00:
                break
            if entry[0] == 0xE5:
                long_parts = {}
                continue
            attr = entry[11]
            if attr == ATTR_LONG_NAME:
                long_parts[entry[0] & 0x1F] = (
                    (entry[1:11] + entry[14:26] + entry[28:32])
                    .decode("utf-16-le", "replace")
                    .split("\x00")[0]
                )
                continue
            if attr & ATTR_VOLUME_ID:
                long_parts = {}
                continue
            if long_parts:
                name = "".join(long_parts[k] for k in sorted(long_parts))
            else:
                stem = entry[0:8].decode("latin1").strip()
                ext = entry[8:11].decode("latin1").strip()
                name = stem + ("." + ext if ext else "")
            long_parts = {}
            if name in (".", ".."):
                continue
            size = struct.unpack_from("<I", entry, 28)[0]
            out.append((name, size, bool(attr & ATTR_DIRECTORY)))
        return out


def main(argv):
    parser = argparse.ArgumentParser(add_help=True, description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--partition", type=int, default=1)
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="NAME",
        help="a file that must exist and be non-empty (repeatable, "
             "case-insensitive; a directory satisfies a name with no extension)",
    )
    parser.add_argument(
        "--bpb-only",
        action="store_true",
        help="only ask whether the partition carries a filesystem at all",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    def say(message):
        if not args.quiet:
            print(message)

    try:
        with open(args.image, "rb") as fh:
            ptype, start, count = read_partition(fh, args.partition)
            fat = Fat(fh, start)
            if ptype not in FAT_TYPES:
                print(
                    "partition %d has type 0x%02x, which is not a FAT type"
                    % (args.partition, ptype),
                    file=sys.stderr,
                )
                return 1
            say(
                "partition %d: type 0x%02x start=%d sectors=%d (%.1f MiB)"
                % (args.partition, ptype, start, count, count * SECTOR / 1048576.0)
            )
            if not fat.formatted:
                print(
                    "partition %d has no filesystem: the BPB is blank "
                    "(bytes/sector=%d sectors/cluster=%d fats=%d)"
                    % (
                        args.partition,
                        fat.bytes_per_sector,
                        fat.sectors_per_cluster,
                        fat.fats,
                    ),
                    file=sys.stderr,
                )
                return 1
            say("  " + fat.describe())
            if args.bpb_only:
                return 0

            entries = fat.root_entries_list()
            for name, size, is_dir in entries:
                say("  %-4s %10d  %s" % ("dir" if is_dir else "file", size, name))
            total = sum(size for _, size, is_dir in entries if not is_dir)
            say("  -- %d entries, %d bytes of files in the root" % (len(entries), total))

            index = {name.lower(): (size, is_dir) for name, size, is_dir in entries}
            missing = []
            for want in args.require:
                found = index.get(want.lower())
                if found is None:
                    missing.append("%s is missing" % want)
                elif not found[1] and found[0] == 0:
                    missing.append("%s is empty" % want)
            if missing:
                for problem in missing:
                    print("boot partition: %s" % problem, file=sys.stderr)
                return 1
            return 0
    except ImageError as exc:
        print("%s: %s" % (args.image, exc), file=sys.stderr)
        return 1
    except OSError as exc:
        print("%s: %s" % (args.image, exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
