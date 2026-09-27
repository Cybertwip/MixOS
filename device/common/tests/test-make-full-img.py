#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""make_full_img.py test: assemble a container from random fixtures and
verify it with an independent GPT reader -- protective MBR, header and
entry-array CRCs, names, the inclusive last_lba both entries carry, the
backup structures, and a byte round-trip of both parts. Fails loudly
instead of passing silently: every check is an assertion with a witness.
"""
import os
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "make_full_img.py")
SECTOR = 512
LINUX_FS_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le

failures = []


def check(cond, witness):
    print(("  ok: " if cond else "FAIL: ") + witness)
    if not cond:
        failures.append(witness)


def main():
    work = tempfile.mkdtemp(prefix="full-img-test.")
    boot = os.urandom(3 * 1024 * 1024 + 123)  # unaligned sizes on purpose
    rootfs = os.urandom(5 * 1024 * 1024 + 777)
    boot_p, rootfs_p, out_p = (os.path.join(work, n) for n in
                               ("boot.bin", "rootfs.bin", "full.img"))
    for path, data in ((boot_p, boot), (rootfs_p, rootfs)):
        with open(path, "wb") as f:
            f.write(data)
    r = subprocess.run([sys.executable, SCRIPT, "--boot", boot_p,
                        "--rootfs", rootfs_p, "--output", out_p],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"builder exits 0 (rc={r.returncode} {r.stderr.strip()})")
    if r.returncode != 0:
        return 1
    kv = dict(line.split("=", 1) for line in r.stdout.split())
    boot_skip, boot_count = int(kv["boot_skip"]), int(kv["boot_count"])
    rootfs_skip, rootfs_count = int(kv["rootfs_skip"]), int(kv["rootfs_count"])
    with open(out_p, "rb") as f:
        img = f.read()
    total = len(img) // SECTOR
    check(len(img) == int(kv["image_bytes"]) == len(img) // SECTOR * SECTOR,
          f"size {len(img)} whole sectors, matches image_bytes")
    check(len(img) % (2048 * SECTOR) == 0, "total is MiB-aligned")

    # Protective MBR.
    check(img[510:512] == b"\x55\xaa", "PMBR boot signature 55 AA")
    check(img[446 + 4] == 0xEE, "PMBR partition type EE")
    check(struct.unpack("<I", img[446 + 8:446 + 12])[0] == 1, "PMBR starts at LBA 1")
    check(struct.unpack("<I", img[446 + 12:446 + 16])[0] == total - 1,
          f"PMBR spans the disk ({total - 1} sectors)")

    # Primary header.
    hdr = img[SECTOR:2 * SECTOR]
    (sig, rev, hsize, crc, _res, cur, bak, first_u, last_u, _guid,
     entries_lba, nent, entsz, entcrc) = struct.unpack("<8sIIIIQQQQ16sQIII", hdr[:92])
    check(sig == b"EFI PART", "header magic")
    check((rev, hsize) == (0x10000, 92), "revision + header size")
    zeroed = hdr[:16] + b"\x00" * 4 + hdr[20:92]
    check(zlib.crc32(zeroed) & 0xFFFFFFFF == crc, f"header CRC {crc:#x} recomputes")
    check((cur, bak) == (1, total - 1), f"primary/backup LBAs 1/{total - 1}")
    check((first_u, last_u) == (34, total - 34), "usable range 34..last-33")
    check((entries_lba, nent, entsz) == (2, 128, 128), "entry array 128x128 at LBA 2")

    # Entries.
    entries = img[2 * SECTOR:34 * SECTOR]
    check(zlib.crc32(entries) & 0xFFFFFFFF == entcrc, "entry-array CRC recomputes")
    e0 = struct.unpack("<16s16sQQQ72s", entries[:128])
    e1 = struct.unpack("<16s16sQQQ72s", entries[128:256])
    for i, e, nm, skip, count in ((0, e0, "BOOT", boot_skip, boot_count),
                                  (1, e1, "ROOTFS", rootfs_skip, rootfs_count)):
        tguid, _uniq, first, last, _attr, rawname = e
        name = rawname.decode("utf-16-le").rstrip("\x00")
        check(tguid == LINUX_FS_GUID, f"p{i + 1} type is Linux filesystem")
        check(first == skip, f"p{i + 1} starts at printed skip ({skip})")
        check(last == skip + count - 1, f"p{i + 1} last_lba inclusive ({last})")
        check(name == nm, f"p{i + 1} named {nm}")
        check(skip % 2048 == 0 and count % 2048 == 0, f"p{i + 1} MiB-aligned")
    check(entries[256:] == b"\x00" * (126 * 128), "entries 3..128 zero")
    check(rootfs_skip == boot_skip + boot_count, "p2 follows p1 exactly")

    # Backup structures.
    last = total - 1
    bhdr = img[last * SECTOR:]
    (bsig, _br, _bh, bcrc, _x, bcur, bbak) = struct.unpack("<8sIIIIQQ", bhdr[:40])
    bzeroed = bhdr[:16] + b"\x00" * 4 + bhdr[20:92]
    check(bsig == b"EFI PART", "backup header magic")
    check(zlib.crc32(bzeroed) & 0xFFFFFFFF == bcrc, "backup header CRC recomputes")
    check((bcur, bbak) == (last, 1), "backup header points at primary")
    check(img[(last - 32) * SECTOR:last * SECTOR] == entries, "backup entries match")

    # Content round-trip: the printed offsets recover both inputs exactly.
    check(img[boot_skip * SECTOR:boot_skip * SECTOR + len(boot)] == boot,
          f"p1 reads back {len(boot)} boot bytes")
    check(img[rootfs_skip * SECTOR:rootfs_skip * SECTOR + len(rootfs)] == rootfs,
          f"p2 reads back {len(rootfs)} rootfs bytes")

    # Error case.
    r = subprocess.run([sys.executable, SCRIPT, "--boot", os.path.join(work, "nope"),
                        "--rootfs", rootfs_p, "--output", out_p + ".2"],
                       capture_output=True, text=True)
    check(r.returncode != 0, "missing input rejected")

    print("PASS: make_full_img" if not failures else
          f"FAILED: {len(failures)} check(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
