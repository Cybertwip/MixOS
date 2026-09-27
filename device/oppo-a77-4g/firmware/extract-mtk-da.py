#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
"""Lift one SoC's DA entry out of an MTK AllInOne DA bundle.

Walks the same header tools/mediatek/mvii-flash/mtk_serial.go uses
(parseMTKDALoader/parseDAEntry): entry count at 0x68, 0xDC or 0xD8
records, 0xDADA magic, HW code at +2. Picks the best match for the
requested HW code the same way (highest HW then SW version whose
regions all land inside the file), and repacks it as a single-entry
loader with its region blobs appended and rebased. Re-walks the output
before writing, so a layout the Go side would read differently fails
here instead of on the bench.

Usage: extract-mtk-da.py BUNDLE HWCODE OUT  (HWCODE like 0x6765)
"""
import struct
import sys


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def detect_record(data):
    if len(data) >= 0x6C + 0xD8 + 2 and data[0x6C + 0xD8:0x6C + 0xD8 + 2] == b"\xda\xda":
        return 0xD8
    return 0xDC


def walk(data):
    """Return (entries, record_size). Each entry carries its raw record."""
    if len(data) < 0x6C:
        fail("too small for a DA loader")
    count = struct.unpack("<I", data[0x68:0x6C])[0]
    if count == 0 or count > 4096:
        fail(f"invalid DA entry count {count}")
    rec = detect_record(data)
    entries = []
    for i in range(count):
        pos = 0x6C + i * rec
        if pos + rec > len(data):
            break
        r = data[pos:pos + rec]
        if struct.unpack("<H", r[0:2])[0] != 0xDADA:
            continue
        hw = struct.unpack("<H", r[2:4])[0]
        hver = struct.unpack("<H", r[6:8])[0]
        old = (rec == 0xD8)
        if old:
            sw, cur = 0, 14  # old records: no SW field, count at +14
        else:
            sw, cur = struct.unpack("<H", r[8:10])[0], 18
        nreg = struct.unpack("<H", r[cur:cur + 2])[0]
        cur += 2
        regs = []
        ok = True
        for _ in range(nreg):
            if cur + 20 > len(r):
                ok = False
                break
            bo, ln, sa, so, sl = struct.unpack("<5I", r[cur:cur + 20])
            cur += 20
            if bo + ln < bo or bo + ln > len(data):
                ok = False
                break
            regs.append((bo, ln, sa, so, sl))
        if not ok:
            continue
        entries.append({"raw": r, "rec": rec, "hw": hw, "hver": hver,
                        "sw": sw, "regs": regs})
    return entries, rec


def main():
    if len(sys.argv) != 4:
        fail("usage: extract-mtk-da.py BUNDLE HWCODE OUT  (HWCODE like 0x6765)")
    bundle, out = sys.argv[1], sys.argv[3]
    try:
        want = int(sys.argv[2], 0)
    except ValueError:
        fail(f"bad HW code {sys.argv[2]!r}")
    try:
        with open(bundle, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"cannot read {bundle}: {e}")
    entries, rec = walk(data)
    cands = [e for e in entries if e["hw"] == want]
    if not cands:
        fail(f"no DA entry for hw code 0x{want:04x}")
    cands.sort(key=lambda e: (e["hver"], e["sw"]), reverse=True)
    pick = cands[0]
    # Repack: header with count 1, the record with rebased buffer offsets,
    # then the blobs back to back.
    header = bytearray(data[:0x6C])
    struct.pack_into("<I", header, 0x68, 1)
    record = bytearray(pick["raw"])
    base = 0x6C + rec
    blobs = []
    # Region table starts 2 bytes after the count field handled above:
    # non-old records put regions at +20, old ones at +16.
    table = 16 if rec == 0xD8 else 20
    for i, (bo, ln, sa, so, sl) in enumerate(pick["regs"]):
        struct.pack_into("<I", record, table + i * 20, base + sum(len(b) for b in blobs))
        blobs.append(data[bo:bo + ln])
    packed = bytes(header) + bytes(record) + b"".join(blobs)
    # Verify the output the same way it will be read: one entry, same
    # record flavor, same HW code, all regions in bounds with identical
    # bytes to the source regions.
    outs, orec = walk(packed)
    if orec != rec or len([e for e in outs if e["hw"] == want]) != 1:
        fail("repacked file does not re-walk cleanly")
    [got] = [e for e in outs if e["hw"] == want]
    if len(got["regs"]) != len(pick["regs"]):
        fail("region count changed in repack")
    for (bo, ln, _, _, _), (gbo, gln, _, _, _) in zip(pick["regs"], got["regs"]):
        if ln != gln or packed[gbo:gbo + gln] != data[bo:bo + ln]:
            fail("region bytes changed in repack")
    try:
        with open(out, "wb") as f:
            f.write(packed)
    except OSError as e:
        fail(f"cannot write {out}: {e}")
    print(f"wrote {out}: hw 0x{want:04x}, {len(pick['regs'])} regions, "
          f"{len(packed)} bytes (was {len(data)})")


main()
