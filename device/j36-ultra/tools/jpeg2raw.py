#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project and contributors
# See device/j36-ultra/LICENSE for the licence text and what it covers.
#
# jpeg2raw.py -- resources/MixOS.jpg into something a static ARM binary can blit.
#
# WHY THERE IS A JPEG DECODER IN THIS TREE.  The splash is drawn from the
# initramfs, before switch_root, where there is no ld.so and therefore no
# libjpeg; and it is drawn by /bin/mixsplash, which build-in-vm.sh links -static
# for exactly that reason.  So the decode has to happen at BUILD time, and the
# build's one-time dependency list is
#
#     bc bison build-essential ccache cpio device-tree-compiler flex
#     gcc-arm-linux-gnueabihf git gzip libelf-dev libssl-dev python3 rsync xz-utils
#
# -- no Pillow, no ImageMagick, no ffmpeg.  Adding one is worse than it looks:
# that list is installed once and guarded by $WORK/.deps-installed, so a package
# added to it is a package every already-provisioned build VM does NOT get, and
# the failure lands months later on somebody else's machine as "convert: command
# not found".  A decoder that needs nothing but python3 cannot rot that way.
#
# WHAT IT DECODES.  Baseline sequential DCT, Huffman, 8-bit, 1 or 3 components,
# any sampling factors, restart intervals included -- which is precisely what
# MixOS.jpg is (SOF0, 640x480, 4:2:0, DRI present).  It refuses everything else
# by name rather than producing a plausible-looking wrong picture: progressive
# (SOF2) and arithmetic coding (SOF9/SOF10) are the two a photo editor will
# silently hand you, so they get their own error messages.
#
# THE OUTPUT is deliberately the dullest format that can exist:
#
#     magic   8 bytes  "MIXSPL1\0"
#     width   u32 LE
#     height  u32 LE
#     stride  u32 LE   bytes per row, always width*4
#     flags   u32 LE   0
#     pixels  height*stride bytes, one u32 LE each, 0x00RRGGBB
#
# 0x00RRGGBB and NOT the framebuffer's own layout, because mixsplash reads the
# real layout out of FBIOGET_VSCREENINFO at run time and shifts each channel into
# place.  Baking x8r8g8b8 in here would be baking in the one thing about this
# panel that a kernel change can move.

import struct
import sys

ZIGZAG = (
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
)


class JpegError(Exception):
    pass


def _idct_tables():
    """COS[u][x] = C(u)/2 * cos((2x+1)*u*pi/16), the separable 1-D IDCT kernel.

    Built once as a tuple of tuples: the row and column passes below index it in
    their innermost loop, and a list-of-lists costs a bounds check per access on
    a 7200-block image."""
    import math
    out = []
    for u in range(8):
        cu = (1.0 / math.sqrt(2.0)) if u == 0 else 1.0
        out.append(tuple(0.5 * cu * math.cos((2 * x + 1) * u * math.pi / 16.0)
                         for x in range(8)))
    return tuple(out)


COS = _idct_tables()

# Saturating 0..255 for the level shift, as a table.  The clamp is on the hot
# path -- 307200 pixels times three components -- and `CLAMP[v]' with the offset
# folded in beats two comparisons per sample by a wide margin in CPython.
CLAMP = bytes(min(255, max(0, i - 384)) for i in range(1024))


def idct_block(coef, out):
    """8x8 inverse DCT, level-shifted and clamped, into `out' (a list of 64).

    The all-zero-AC shortcut is not an optimisation for its own sake: in a
    photograph most blocks have a handful of low-frequency coefficients and
    nothing else, so the row pass below skips ~6 of its 8 rows on real input and
    the decode of MixOS.jpg drops from tens of seconds to a few."""
    tmp = [0.0] * 64

    # Rows.
    for i in range(0, 64, 8):
        c1 = coef[i + 1]
        if (c1 == 0 and coef[i + 2] == 0 and coef[i + 3] == 0 and coef[i + 4] == 0
                and coef[i + 5] == 0 and coef[i + 6] == 0 and coef[i + 7] == 0):
            v = coef[i] * 0.35355339059327373  # COS[0][x], constant across x
            if v != 0.0:
                for x in range(8):
                    tmp[i + x] = v
            continue
        for x in range(8):
            s = 0.0
            for u in range(8):
                cu = coef[i + u]
                if cu:
                    s += cu * COS[u][x]
            tmp[i + x] = s

    # Columns, with the level shift and the clamp folded into the store.
    for x in range(8):
        col = tmp[x::8]
        if (col[1] == 0.0 and col[2] == 0.0 and col[3] == 0.0 and col[4] == 0.0
                and col[5] == 0.0 and col[6] == 0.0 and col[7] == 0.0):
            v = col[0] * 0.35355339059327373
            b = CLAMP[int(v + 512.5) & 1023]
            for y in range(8):
                out[y * 8 + x] = b
            continue
        for y in range(8):
            s = 0.0
            for u in range(8):
                cu = col[u]
                if cu:
                    s += cu * COS[u][y]
            out[y * 8 + x] = CLAMP[int(s + 512.5) & 1023]


class HuffTable:
    """Canonical JPEG Huffman, decoded a bit at a time.

    `lut' maps (length, code) -> value.  A 16-bit direct-mapped table would be
    faster and is what a C decoder builds; a dict is faster in CPython, where the
    win comes from doing less interpreted work per symbol rather than from fewer
    memory accesses."""

    __slots__ = ("lut", "maxlen")

    def __init__(self, counts, symbols):
        self.lut = {}
        code = 0
        k = 0
        self.maxlen = 0
        for length in range(1, 17):
            for _ in range(counts[length - 1]):
                self.lut[(length, code)] = symbols[k]
                code += 1
                k += 1
            code <<= 1
            if counts[length - 1]:
                self.maxlen = length


class BitReader:
    """The entropy-coded segment, MSB first.

    Two JPEG quirks live in here and nowhere else.  A literal 0xFF in the coded
    data is stored as FF 00, so the 00 is dropped on the way past; and a restart
    marker (FFD0..FFD7) is a byte-aligned resynchronisation point, which the
    scan loop asks for by name through restart() rather than tripping over."""

    __slots__ = ("data", "pos", "bits", "nbits")

    def __init__(self, data, pos):
        self.data = data
        self.pos = pos
        self.bits = 0
        self.nbits = 0

    def _fill(self):
        d = self.data
        p = self.pos
        if p >= len(d):
            # Ran off the end: feed 1s.  A truncated file then produces a
            # degraded bottom edge instead of an exception, which is the same
            # thing every real decoder does and is far easier to diagnose from a
            # picture than from a traceback.
            self.bits = (self.bits << 8) | 0xFF
            self.nbits += 8
            return
        b = d[p]
        p += 1
        if b == 0xFF:
            nxt = d[p] if p < len(d) else 0
            if nxt == 0x00:
                p += 1
            else:
                # A real marker.  Do not consume it; hand back 1s so the current
                # symbol completes, and let restart() step over it.
                self.bits = (self.bits << 8) | 0xFF
                self.nbits += 8
                return
        self.pos = p
        self.bits = (self.bits << 8) | b
        self.nbits += 8

    def bit(self):
        if self.nbits == 0:
            self._fill()
        self.nbits -= 1
        return (self.bits >> self.nbits) & 1

    def receive(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.bit()
        return v

    def decode(self, table):
        code = 0
        lut = table.lut
        for length in range(1, 17):
            code = (code << 1) | self.bit()
            v = lut.get((length, code))
            if v is not None:
                return v
        raise JpegError("bad Huffman code in the entropy-coded data")

    def restart(self):
        """Drop to the next byte boundary and step over one RSTn marker."""
        self.bits = 0
        self.nbits = 0
        d = self.data
        p = self.pos
        while p + 1 < len(d):
            if d[p] == 0xFF and 0xD0 <= d[p + 1] <= 0xD7:
                self.pos = p + 2
                return
            p += 1
        self.pos = len(d)


def extend(v, n):
    """JPEG's signed-magnitude to two's complement (F.2.2.1, EXTEND)."""
    return v - (1 << n) + 1 if n and v < (1 << (n - 1)) else v


def decode_jpeg(data):
    """-> (width, height, ncomp, [plane0, plane1, plane2]) with planes as
    bytearrays of the FULL image size, chroma already upsampled."""
    if data[0:2] != b"\xff\xd8":
        raise JpegError("not a JPEG (no SOI)")

    qt = {}
    huff_dc = {}
    huff_ac = {}
    frame = None
    restart_interval = 0
    pos = 2

    while pos < len(data):
        if data[pos] != 0xFF:
            pos += 1
            continue
        marker = data[pos + 1]
        pos += 2
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            continue
        if marker == 0xD9:
            break
        if pos + 2 > len(data):
            break
        seglen = struct.unpack_from(">H", data, pos)[0]
        seg = data[pos + 2:pos + seglen]

        if marker == 0xDB:  # DQT
            i = 0
            while i < len(seg):
                pq, tq = seg[i] >> 4, seg[i] & 15
                i += 1
                tbl = [0] * 64
                for k in range(64):
                    if pq:
                        tbl[ZIGZAG[k]] = struct.unpack_from(">H", seg, i)[0]
                        i += 2
                    else:
                        tbl[ZIGZAG[k]] = seg[i]
                        i += 1
                qt[tq] = tbl

        elif marker in (0xC0, 0xC1):  # SOF0 baseline, SOF1 extended sequential
            prec, h, w, nc = seg[0], struct.unpack_from(">H", seg, 1)[0], \
                struct.unpack_from(">H", seg, 3)[0], seg[5]
            if prec != 8:
                raise JpegError("only 8-bit sample precision is supported")
            if nc not in (1, 3):
                raise JpegError("only greyscale and YCbCr JPEGs are supported")
            comps = []
            for c in range(nc):
                cid, hv, tq = seg[6 + c * 3], seg[7 + c * 3], seg[8 + c * 3]
                comps.append({"id": cid, "h": hv >> 4, "v": hv & 15, "tq": tq})
            frame = {"w": w, "h": h, "comps": comps}

        elif marker == 0xC2:
            raise JpegError("progressive JPEG (SOF2); re-save as baseline")
        elif marker in (0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            raise JpegError("arithmetic-coded JPEG; re-save as baseline Huffman")
        elif marker == 0xC3:
            raise JpegError("lossless JPEG (SOF3); re-save as baseline")

        elif marker == 0xC4:  # DHT
            i = 0
            while i < len(seg):
                tc, th = seg[i] >> 4, seg[i] & 15
                i += 1
                counts = list(seg[i:i + 16])
                i += 16
                total = sum(counts)
                symbols = list(seg[i:i + total])
                i += total
                (huff_ac if tc else huff_dc)[th] = HuffTable(counts, symbols)

        elif marker == 0xDD:  # DRI
            restart_interval = struct.unpack_from(">H", seg, 0)[0]

        elif marker == 0xDA:  # SOS -- the scan, and the end of the header walk
            if frame is None:
                raise JpegError("SOS before SOF")
            ns = seg[0]
            scan = []
            for s in range(ns):
                cs, td_ta = seg[1 + s * 2], seg[2 + s * 2]
                comp = next(c for c in frame["comps"] if c["id"] == cs)
                comp["dc"] = huff_dc[td_ta >> 4]
                comp["ac"] = huff_ac[td_ta & 15]
                scan.append(comp)
            return _decode_scan(data, pos + seglen, frame, scan, qt,
                                restart_interval)

        pos += seglen

    raise JpegError("no scan found")


def _decode_scan(data, pos, frame, scan, qt, restart_interval):
    w, h = frame["w"], frame["h"]
    comps = frame["comps"]
    hmax = max(c["h"] for c in comps)
    vmax = max(c["v"] for c in comps)
    mcux = (w + 8 * hmax - 1) // (8 * hmax)
    mcuy = (h + 8 * vmax - 1) // (8 * vmax)

    for c in comps:
        c["bw"] = mcux * c["h"] * 8          # padded plane width
        c["bh"] = mcuy * c["v"] * 8
        c["plane"] = bytearray(c["bw"] * c["bh"])
        c["pred"] = 0
        c["q"] = qt[c["tq"]]

    br = BitReader(data, pos)
    coef = [0.0] * 64
    block = [0] * 64
    mcus_done = 0

    for my in range(mcuy):
        for mx in range(mcux):
            if restart_interval and mcus_done == restart_interval:
                br.restart()
                for c in comps:
                    c["pred"] = 0
                mcus_done = 0

            for c in scan:
                q = c["q"]
                plane = c["plane"]
                bw = c["bw"]
                for by in range(c["v"]):
                    for bx in range(c["h"]):
                        for k in range(64):
                            coef[k] = 0.0

                        t = br.decode(c["dc"])
                        diff = extend(br.receive(t), t) if t else 0
                        c["pred"] += diff
                        coef[0] = c["pred"] * q[0]

                        k = 1
                        while k < 64:
                            rs = br.decode(c["ac"])
                            r, s = rs >> 4, rs & 15
                            if s == 0:
                                if r != 15:
                                    break       # EOB
                                k += 16
                                continue
                            k += r
                            if k > 63:
                                break
                            z = ZIGZAG[k]
                            coef[z] = extend(br.receive(s), s) * q[z]
                            k += 1

                        idct_block(coef, block)

                        ox = (mx * c["h"] + bx) * 8
                        oy = (my * c["v"] + by) * 8
                        for row in range(8):
                            off = (oy + row) * bw + ox
                            plane[off:off + 8] = bytes(block[row * 8:row * 8 + 8])
            mcus_done += 1

    # Upsample every component to the full image grid.  Nearest neighbour: the
    # chroma of a 4:2:0 JPEG is already a two-pixel blur, and interpolating it
    # would cost a second of build time to move edges by half a pixel on an
    # image whose subject is a flat blue gradient.
    planes = []
    for c in comps:
        sx, sy = hmax // c["h"], vmax // c["v"]
        src, bw = c["plane"], c["bw"]
        out = bytearray(w * h)
        if sx == 1 and sy == 1:
            for y in range(h):
                o = y * w
                s = y * bw
                out[o:o + w] = src[s:s + w]
        else:
            for y in range(h):
                s = (y // sy) * bw
                row = src[s:s + (w + sx - 1) // sx]
                o = y * w
                if sx == 2:
                    doubled = bytearray(len(row) * 2)
                    doubled[0::2] = row
                    doubled[1::2] = row
                    out[o:o + w] = doubled[:w]
                else:
                    for x in range(w):
                        out[o + x] = row[x // sx]
        planes.append(out)

    return w, h, len(comps), planes


def to_rgb(w, h, ncomp, planes):
    """YCbCr (JFIF, full range) to packed 0x00RRGGBB, as a bytes object of
    w*h*4 in little-endian order."""
    out = bytearray(w * h * 4)
    if ncomp == 1:
        y = planes[0]
        out[0::4] = y
        out[1::4] = y
        out[2::4] = y
        return bytes(out)

    yp, cb, cr = planes
    # The fixed-point coefficients are the JFIF ones scaled by 2^16, which keeps
    # the whole conversion in machine integers.
    for i in range(w * h):
        Y = yp[i] << 16
        u = cb[i] - 128
        v = cr[i] - 128
        r = (Y + 91881 * v) >> 16
        g = (Y - 22554 * u - 46802 * v) >> 16
        b = (Y + 116130 * u) >> 16
        j = i * 4
        out[j] = 255 if b > 255 else (0 if b < 0 else b)
        out[j + 1] = 255 if g > 255 else (0 if g < 0 else g)
        out[j + 2] = 255 if r > 255 else (0 if r < 0 else r)
    return bytes(out)


def resize(pix, sw, sh, dw, dh):
    """Box-free bilinear resize of packed 0x00RRGGBB.

    Only called when the panel is not the picture's own size, which today it is
    -- MixOS.jpg is 640x480 and so is the framebuffer.  It exists so that a
    panel change is a one-line argument to this script rather than a trip
    through an image editor."""
    if (sw, sh) == (dw, dh):
        return pix
    out = bytearray(dw * dh * 4)
    for dy in range(dh):
        fy = (dy + 0.5) * sh / dh - 0.5
        y0 = int(fy) if fy >= 0 else 0
        y1 = min(y0 + 1, sh - 1)
        wy = fy - y0
        if wy < 0:
            wy = 0.0
        for dx in range(dw):
            fx = (dx + 0.5) * sw / dw - 0.5
            x0 = int(fx) if fx >= 0 else 0
            x1 = min(x0 + 1, sw - 1)
            wx = fx - x0
            if wx < 0:
                wx = 0.0
            o = (dy * dw + dx) * 4
            for ch in range(3):
                a = pix[(y0 * sw + x0) * 4 + ch]
                b = pix[(y0 * sw + x1) * 4 + ch]
                c = pix[(y1 * sw + x0) * 4 + ch]
                d = pix[(y1 * sw + x1) * 4 + ch]
                top = a + (b - a) * wx
                bot = c + (d - c) * wx
                out[o + ch] = int(top + (bot - top) * wy + 0.5)
    return bytes(out)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(
            "usage: jpeg2raw.py IN.jpg OUT.mixspl [WIDTH HEIGHT]\n"
            "  Decodes a baseline JPEG into the MIXSPL1 blob /bin/mixsplash blits.\n")
        return 2

    src, dst = argv[1], argv[2]
    with open(src, "rb") as f:
        data = f.read()

    try:
        w, h, ncomp, planes = decode_jpeg(data)
    except JpegError as e:
        sys.stderr.write("jpeg2raw: %s: %s\n" % (src, e))
        return 1

    pix = to_rgb(w, h, ncomp, planes)

    if len(argv) >= 5:
        dw, dh = int(argv[3]), int(argv[4])
        if dw <= 0 or dh <= 0:
            sys.stderr.write("jpeg2raw: target size must be positive\n")
            return 1
        pix = resize(pix, w, h, dw, dh)
        w, h = dw, dh

    with open(dst, "wb") as f:
        f.write(b"MIXSPL1\0")
        f.write(struct.pack("<IIII", w, h, w * 4, 0))
        f.write(pix)

    sys.stderr.write("jpeg2raw: %s -> %s, %dx%d, %d bytes\n"
                     % (src, dst, w, h, 24 + len(pix)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
