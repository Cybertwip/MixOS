#!/bin/sh
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# MT6765 DA test: the extractor lifts the right entry out of a synthetic
# AllInOne bundle (wrong SoC refused, truncated input refused), and the
# vendored DA still walks as the single MT6765 entry it was lifted as --
# a corrupted blob would otherwise fail deep in a BROM session, far from
# the cause.
set -u

HERE="$(cd -- "$(dirname -- "$0")" && pwd)"
FW="$HERE/../firmware"
DA="$FW/DA/MTK_DA_mt6765.bin"
DA_SIZE=570588
DA_SHA256="156aaf9cdb02bc4fdcd5ee59c740aa3735aa9cc3af2450c64b7e8c308bb84f41"

command -v python3 >/dev/null || { echo "FAIL: python3 missing"; exit 1; }
if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
fi

fail=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/test-da.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# A two-entry bundle: an MT6592 decoy first, then the MT6765 entry with
# two blob regions. Mirrors the layout TestParseMTKDALoaderFindsMT6592Stages
# builds on the Go side (count at 0x68, 0xDC records, magic + hwcode up front,
# region table at +20).
python3 - "$WORK/bundle.bin" <<'PY'
import struct
import sys
data = bytearray(0x800)
struct.pack_into("<I", data, 0x68, 2)
def entry(hw, blobs):
    e = bytearray(0xDC)
    struct.pack_into("<H", e, 0, 0xDADA)
    struct.pack_into("<H", e, 2, hw)
    struct.pack_into("<H", e, 18, len(blobs))
    return e, blobs
recs = []
for hw, parts in ((0x6592, (b"wrong1",)), (0x6765, (b"da-one", b"second-blob"))):
    e, parts = entry(hw, parts)
    recs.append((e, parts))
off = 0x6C + 2 * 0xDC
spans = []
for e, parts in recs:
    for i, b in enumerate(parts):
        struct.pack_into("<I", e, 20 + i * 20, off)
        struct.pack_into("<I", e, 20 + i * 20 + 4, len(b))
        struct.pack_into("<I", e, 20 + i * 20 + 8, 0x200000 + off)
        spans.append((off, b))
        off += len(b)
for i, (e, _) in enumerate(recs):
    data[0x6C + i * 0xDC:0x6C + (i + 1) * 0xDC] = e
for at, b in spans:
    data[at:at + len(b)] = b
with open(sys.argv[1], "wb") as f:
    f.write(bytes(data[:off]))
PY
[ -s "$WORK/bundle.bin" ] || { echo "FAIL: fixture not built"; exit 1; }
echo "  fixture: ok"

if ! python3 "$FW/extract-mtk-da.py" "$WORK/bundle.bin" 0x6765 "$WORK/out.bin"; then
    echo "FAIL: extractor refused the synthetic bundle"; fail=1
else
    if ! python3 - "$WORK/out.bin" <<'PY'; then
import struct
import sys
data = open(sys.argv[1], "rb").read()
assert struct.unpack("<I", data[0x68:0x6C])[0] == 1, "count"
r = data[0x6C:0x6C + 0xDC]
assert struct.unpack("<H", r[0:2])[0] == 0xDADA, "magic"
assert struct.unpack("<H", r[2:4])[0] == 0x6765, "hwcode"
assert struct.unpack("<H", r[18:20])[0] == 2, "regions"
got = []
for i in range(2):
    bo, ln, _, _, _ = struct.unpack("<5I", r[20 + i * 20:40 + i * 20])
    got.append(data[bo:bo + ln])
assert got == [b"da-one", b"second-blob"], got
PY
        echo "FAIL: extracted entry is not the MT6765 one"; fail=1
    else
        echo "  extract 0x6765: ok"
    fi
fi

if python3 "$FW/extract-mtk-da.py" "$WORK/bundle.bin" 0x6768 "$WORK/nope.bin" 2>/dev/null; then
    echo "FAIL: missing HW code accepted"; fail=1
else
    echo "  missing hwcode refused: ok"
fi

head -c 100 "$WORK/bundle.bin" > "$WORK/short.bin"
if python3 "$FW/extract-mtk-da.py" "$WORK/short.bin" 0x6765 "$WORK/nope2.bin" 2>/dev/null; then
    echo "FAIL: truncated bundle accepted"; fail=1
else
    echo "  truncated bundle refused: ok"
fi

if [ ! -f "$DA" ]; then
    echo "FAIL: $DA missing (run firmware/fetch-da-mt6765.sh)"; fail=1
else
    size="$(wc -c < "$DA" | tr -d ' ')"
    got="$(sha256 "$DA")"
    if [ "$size" != "$DA_SIZE" ]; then
        echo "FAIL: vendored DA is $size bytes, want $DA_SIZE"; fail=1
    elif [ "$got" != "$DA_SHA256" ]; then
        echo "FAIL: vendored DA hash $got"; fail=1
    elif ! python3 - "$DA" <<'PY'; then
import struct
import sys
data = open(sys.argv[1], "rb").read()
assert struct.unpack("<I", data[0x68:0x6C])[0] == 1, "count"
r = data[0x6C:0x6C + 0xDC]
assert struct.unpack("<H", r[0:2])[0] == 0xDADA, "magic"
assert struct.unpack("<H", r[2:4])[0] == 0x6765, "hwcode"
nreg = struct.unpack("<H", r[18:20])[0]
assert nreg == 3, nreg
for i in range(nreg):
    bo, ln, _, _, _ = struct.unpack("<5I", r[20 + i * 20:40 + i * 20])
    assert bo + ln <= len(data), i
PY
        echo "FAIL: vendored DA does not re-walk"; fail=1
    else
        echo "  vendored MTK_DA_mt6765.bin: ok ($size bytes)"
    fi
fi

[ "$fail" -eq 0 ] && echo "PASS: MT6765 DA"
exit "$fail"
