#!/usr/bin/env python3
"""Build the MVII asset slot — every picture the device draws before the OS.

The container format, and the argument for having one at all, is documented in
OS/MVII/Kernel/ARM/MediaTek/J36Ultra/Drivers/mvii_assets.h. In one line: the boot
pictures used to be C (axis-aligned rectangles and a 3x5 bitmap font scaled ten
times) plus an `.incbin`'d logo, so changing art meant rebuilding a bootloader.
This writes them to their own partition instead, flashable on their own with

    ./flash -target=arm -dbg -device /dev/cu.usbmodem... -assets -yes

WHY THE ART IS DRAWN HERE AND NOT IMPORTED.

Everything except the logo is generated from geometry in this file rather than
read from PNGs. That is deliberate: the shapes have to agree pixel-for-pixel with
what the OS status bar draws (DashboardRenderer's drawPlug565 and friends), and a
silhouette that lives as numbers in one file is far easier to keep in agreement
than one that lives as a binary somebody exported once. Anyone who wants a
different battery edits kBattery* below and reflashes the slot; nothing rebuilds.

HOW THE SMOOTHNESS IS PRODUCED.

Geometry is rasterised at SUPERSAMPLE x and box-filtered down. A box filter over
a supersampled binary mask is exactly area coverage — the fraction of the pixel
the shape covers — which is precisely what an A8 coverage mask means and what the
loader's blend expects. Numerals come from a real outline font, rendered at final
size so FreeType's own hinting and grey AA apply.

Usage:
    create-mvii-asset-slot.py --output assets.bin [--logo Microsoft.jpg]
                              [--width 640] [--height 480]

If Pillow is unavailable the script writes a 16-byte stub with no magic, which
every consumer treats as "no slot" and falls back to the built-in drawing — the
build never breaks over art.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

# ── The container, mirrored from mvii_assets.h ──

MAGIC0 = 0x4949564D  # "MVII"
MAGIC1 = 0x31535341  # "ASS1"
VERSION = 1
HEADER_BYTES = 32
ENTRY_BYTES = 40
STAGE_MAX = 0x00200000

FMT_A8 = 0
FMT_ARGB8888 = 1
CODEC_RAW = 0
CODEC_RLE8 = 1

ID_LOGO = 1
ID_BATTERY = 2
ID_PLUG = 3
ID_PLUG_BROKEN = 4
ID_BOLT = 5
ID_BATTERY_FILL = 6
ID_DIGIT_0 = 16
ID_PERCENT = 26

# ── Geometry ──
#
# The park's layout is unchanged in spirit from the rectangle version: a 300x140
# body with a 16x56 terminal nub, so a 640x480 panel puts it comfortably above
# centre with room for the numerals and the badge underneath. What changed is
# that the corners are round, the stroke is a real stroke, and every edge is
# resolved to a fraction of a pixel.

SUPERSAMPLE = 4

BATTERY_BODY_W = 300
BATTERY_BODY_H = 140
BATTERY_STROKE = 7
BATTERY_RADIUS = 22
BATTERY_NUB_W = 16
BATTERY_NUB_H = 56
BATTERY_NUB_RADIUS = 7
# Clearance between the inside of the stroke and the fill. Without it the fill
# touches the outline and the two read as one blob at a glance.
BATTERY_FILL_GAP = 8
BATTERY_FILL_RADIUS = 10

PLUG_W = 72
PLUG_H = 120

# Figure height for the numerals. The rectangle version was 5 rows at scale 10,
# i.e. 50 px of staircase; this is 76 px of outline.
DIGIT_HEIGHT = 76

FONT_CANDIDATES = (
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
)


def write_stub(path: Path, reason: str) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x00" * 16)
    print(f"asset slot: stub written to {path} ({reason})")
    return 0


# ── RLE8 ──
#
#   n < 0x80   the next single byte, repeated n + 1 times   (1..128)
#   n >= 0x80  the next (n - 0x80) + 1 bytes, verbatim      (1..128)
#
# A run costs 2 bytes and a literal costs 1 + n, so a run of 2 is a wash and a
# run of 3 wins; the encoder only breaks a literal for 3 or more.


def rle8_encode(data: bytes) -> bytes:
    out = bytearray()
    literal = bytearray()
    i = 0
    n = len(data)

    def flush() -> None:
        pos = 0
        while pos < len(literal):
            chunk = literal[pos:pos + 128]
            out.append(0x80 + len(chunk) - 1)
            out.extend(chunk)
            pos += len(chunk)
        literal.clear()

    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            flush()
            out.append(run - 1)
            out.append(data[i])
            i += run
        else:
            literal.append(data[i])
            if len(literal) == 128:
                flush()
            i += 1
    flush()
    return bytes(out)


def rle8_roundtrip(encoded: bytes, expect: bytes) -> bool:
    """Decode with the loader's own rules and compare. Cheap here, and the one
    place the two implementations can be held against each other at all."""
    out = bytearray()
    i = 0
    while i < len(encoded):
        n = encoded[i]
        i += 1
        if n < 0x80:
            out.extend(bytes([encoded[i]]) * (n + 1))
            i += 1
        else:
            count = n - 0x80 + 1
            out.extend(encoded[i:i + count])
            i += count
    return bytes(out) == expect


# ── Assets ──


class Asset:
    def __init__(self, ident, width, height, fmt, raw, inset=(0, 0, 0, 0), force_raw=False):
        self.id = ident
        self.width = width
        self.height = height
        self.fmt = fmt
        self.raw = raw
        self.inset = inset
        encoded = rle8_encode(raw)
        if force_raw:
            # See mvii_asset_raw() in the loader. stage1 paints a full-screen
            # BGRA frame with one memcpy into the live framebuffer; it can only
            # do that if the pixels are already pixels. Compressing the logo
            # would save partition space nobody is short of and cost a visibly
            # slower splash, which is the opposite of what this boot needs.
            self.codec = CODEC_RAW
            self.stored = raw
        elif len(encoded) < len(raw) and rle8_roundtrip(encoded, raw):
            self.codec = CODEC_RLE8
            self.stored = encoded
        else:
            self.codec = CODEC_RAW
            self.stored = raw
        self.offset = 0


def supersampled(width, height):
    from PIL import Image, ImageDraw
    img = Image.new("L", (width * SUPERSAMPLE, height * SUPERSAMPLE), 0)
    return img, ImageDraw.Draw(img)


def resolve(img, width, height) -> bytes:
    from PIL import Image
    return img.resize((width, height), Image.Resampling.BOX).tobytes()


def make_battery() -> Asset:
    """Outline plus terminal nub, with the fill window recorded in the entry.

    The nub is drawn as a rounded rect that reaches back UNDER the body's right
    stroke. Butting it against the stroke leaves a hairline seam once both edges
    are anti-aliased, because two adjacent partial coverages do not add to one.

    Only its RIGHT corners are rounded. With all four rounded, the two left ones
    land outside the stroke and the terminal reads as a separate lozenge pushed
    up against the battery rather than as part of it — visible in the very first
    render this script produced."""
    total_w = BATTERY_BODY_W + BATTERY_NUB_W
    img, d = supersampled(total_w, BATTERY_BODY_H)
    s = SUPERSAMPLE

    nub_y0 = (BATTERY_BODY_H - BATTERY_NUB_H) // 2
    d.rounded_rectangle(
        [(BATTERY_BODY_W - BATTERY_STROKE * 2) * s, nub_y0 * s,
         total_w * s - 1, (nub_y0 + BATTERY_NUB_H) * s - 1],
        radius=BATTERY_NUB_RADIUS * s, fill=255,
        corners=(False, True, True, False))
    d.rounded_rectangle(
        [0, 0, BATTERY_BODY_W * s - 1, BATTERY_BODY_H * s - 1],
        radius=BATTERY_RADIUS * s, outline=255, width=BATTERY_STROKE * s)

    pad = BATTERY_STROKE + BATTERY_FILL_GAP
    inset = (pad, pad, BATTERY_BODY_W - 2 * pad, BATTERY_BODY_H - 2 * pad)
    return Asset(ID_BATTERY, total_w, BATTERY_BODY_H, FMT_A8,
                 resolve(img, total_w, BATTERY_BODY_H), inset)


def make_battery_fill() -> Asset:
    """The bar itself, at exactly the inset's size, so the loader positions it by
    adding the inset origin and reveals it by clipping its width."""
    pad = BATTERY_STROKE + BATTERY_FILL_GAP
    w = BATTERY_BODY_W - 2 * pad
    h = BATTERY_BODY_H - 2 * pad
    img, d = supersampled(w, h)
    s = SUPERSAMPLE
    d.rounded_rectangle([0, 0, w * s - 1, h * s - 1], radius=BATTERY_FILL_RADIUS * s, fill=255)
    return Asset(ID_BATTERY_FILL, w, h, FMT_A8, resolve(img, w, h))


def make_plug(connected: bool) -> Asset:
    """The same silhouette as the rectangle version and as the OS status bar's
    drawPlug565: two prongs, a body, a trailing cable — on a 6x10 cell grid, so
    the proportions carry over exactly and only the edges change."""
    img, d = supersampled(PLUG_W, PLUG_H)
    s = SUPERSAMPLE
    c = PLUG_W * s // 6  # one grid cell, supersampled

    def rr(x0, y0, x1, y1, r):
        d.rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=r, fill=255)

    prong_r = c // 3
    rr(1 * c, 0, 2 * c, int(2.6 * c), prong_r)
    rr(4 * c, 0, 5 * c, int(2.6 * c), prong_r)
    rr(0, 2 * c, 6 * c, 6 * c, c // 2)

    cable_r = c // 3
    if connected:
        rr(2 * c, 6 * c, 4 * c, 10 * c, cable_r)
    else:
        # A stub off the body, a gap, then the loose end kinked sideways. This is
        # the last frame on the panel before the backlight goes.
        rr(2 * c, 6 * c, 4 * c, 7 * c, cable_r)
        rr(int(3.6 * c), 8 * c, int(5.6 * c), 10 * c, cable_r)

    # The body rectangle, in final pixels, carried in the entry's inset. The bolt
    # is centred in THIS, not in the plug: the plug is prongs and cable as well,
    # and centring on the whole silhouette puts the bolt over the prong gap.
    body = (0, PLUG_H * 2 // 10, PLUG_W, PLUG_H * 4 // 10)

    ident = ID_PLUG if connected else ID_PLUG_BROKEN
    return Asset(ident, PLUG_W, PLUG_H, FMT_A8, resolve(img, PLUG_W, PLUG_H), body)


def make_bolt() -> Asset:
    """Punched out of the plug body in the background colour, so it is sized to
    the body's 6x4 cells rather than to the whole plug."""
    w = int(PLUG_W * 0.40)
    h = (PLUG_H * 4) // 10 - 14
    img, d = supersampled(w, h)
    s = SUPERSAMPLE
    pts = [(0.62, 0.00), (0.14, 0.58), (0.44, 0.58), (0.36, 1.00), (0.88, 0.40), (0.56, 0.40)]
    d.polygon([(px * (w * s - 1), py * (h * s - 1)) for px, py in pts], fill=255)
    return Asset(ID_BOLT, w, h, FMT_A8, resolve(img, w, h))


def load_font():
    from PIL import ImageFont
    for path in FONT_CANDIDATES:
        if not Path(path).exists():
            continue
        for size in (DIGIT_HEIGHT * 2, ):
            try:
                font = ImageFont.truetype(path, size)
            except Exception:  # noqa: BLE001 - a .ttc face that will not open
                continue
            return font, path
    return None, None


def make_numerals(font) -> list:
    """Digits and '%', cropped horizontally to their ink and NOT vertically.

    The vertical part is the subtle one. Each glyph keeps a common cell height
    with a common top edge, so the loader can draw a run at one y and have them
    line up; cropping each glyph to its own bbox would make '1' and '%' sit at
    different heights for no visible reason. Horizontally they ARE cropped, with
    their own tracking added by the loader, because a fixed cell makes a
    percentage read like a serial number."""
    from PIL import Image, ImageDraw

    glyphs = "0123456789%"
    scratch_w, scratch_h = DIGIT_HEIGHT * 6, DIGIT_HEIGHT * 6

    # One pass to find the common ink band across every glyph, then one to render
    # into it. Measuring off the font's own ascent/descent would include the
    # accent zone and leave the numerals floating in the top half of the cell.
    top, bottom = None, None
    boxes = {}
    for ch in glyphs:
        img = Image.new("L", (scratch_w, scratch_h), 0)
        ImageDraw.Draw(img).text((DIGIT_HEIGHT, DIGIT_HEIGHT), ch, font=font, fill=255)
        bbox = img.getbbox()
        if bbox is None:
            return []
        boxes[ch] = (img, bbox)
        top = bbox[1] if top is None else min(top, bbox[1])
        bottom = bbox[3] if bottom is None else max(bottom, bbox[3])

    band = bottom - top
    scale = DIGIT_HEIGHT / float(band)
    out = []
    for ch in glyphs:
        img, bbox = boxes[ch]
        cell = img.crop((bbox[0], top, bbox[2], bottom))
        w = max(1, int(round(cell.width * scale)))
        cell = cell.resize((w, DIGIT_HEIGHT), Image.Resampling.LANCZOS)
        ident = ID_PERCENT if ch == "%" else ID_DIGIT_0 + int(ch)
        out.append(Asset(ident, w, DIGIT_HEIGHT, FMT_A8, cell.tobytes()))
    return out


def make_logo(path: Path, width: int, height: int) -> Asset | None:
    """ARGB8888, stored B,G,R,A per pixel — little-endian, so a 32-bit load gives
    the 0xAARRGGBB the framebuffer wants. Identical byte order to the blob
    microsoft_logo_real.S has always embedded, which is what lets stage1 take
    either source without a second painter."""
    from PIL import Image
    if not path or not path.exists():
        return None
    expected = width * height * 4
    if path.suffix.lower() == ".bin":
        data = path.read_bytes()
        if len(data) != expected:
            print(f"asset slot: {path} is {len(data)}B, need {expected}B; logo omitted")
            return None
        return Asset(ID_LOGO, width, height, FMT_ARGB8888, data, force_raw=True)
    im = Image.open(path).convert("RGBA").resize((width, height), Image.Resampling.LANCZOS)
    r, g, b, _ = im.split()
    bgra = Image.merge("RGBA", (b, g, r, Image.new("L", im.size, 255)))
    return Asset(ID_LOGO, width, height, FMT_ARGB8888, bgra.tobytes(), force_raw=True)


# ── Serialisation ──


def build(assets: list) -> bytes:
    table_bytes = ENTRY_BYTES * len(assets)
    cursor = HEADER_BYTES + table_bytes
    payload = bytearray()
    for a in assets:
        pad = (-cursor) & 3
        payload.extend(b"\x00" * pad)
        cursor += pad
        a.offset = cursor
        payload.extend(a.stored)
        cursor += len(a.stored)

    table = bytearray()
    for a in assets:
        table.extend(struct.pack(
            "<IHHBBHIIIHHHHII",
            a.id, a.width, a.height, a.fmt, a.codec, 0,
            a.offset, len(a.stored), len(a.raw),
            a.inset[0], a.inset[1], a.inset[2], a.inset[3],
            zlib.crc32(a.stored) & 0xFFFFFFFF, 0))
    assert len(table) == table_bytes, f"entry is {len(table) // max(1, len(assets))}B"

    body = bytes(table) + bytes(payload)
    total = HEADER_BYTES + len(body)
    header = struct.pack("<IIIIIIII", MAGIC0, MAGIC1, VERSION, HEADER_BYTES, ENTRY_BYTES,
                         len(assets), total, zlib.crc32(body) & 0xFFFFFFFF)
    assert len(header) == HEADER_BYTES
    return header + body


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--logo", type=Path, help="boot splash source (jpg/png, or a raw BGRA .bin)")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    args = ap.parse_args()

    try:
        import PIL  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        return write_stub(args.output, f"Pillow unavailable: {exc}")

    try:
        assets = [make_battery(), make_battery_fill(), make_plug(True), make_plug(False),
                  make_bolt()]

        font, font_path = load_font()
        if font is None:
            # The loader falls back to its own 3x5 numerals glyph by glyph, so an
            # asset set with no digits still renders — just not smoothly.
            print("asset slot: no outline font found; numerals left to the loader's fallback")
        else:
            numerals = make_numerals(font)
            if numerals:
                assets.extend(numerals)
                print(f"asset slot: numerals from {font_path} at {DIGIT_HEIGHT}px")

        logo = make_logo(args.logo, args.width, args.height)
        if logo is not None:
            assets.append(logo)

        blob = build(assets)
    except Exception as exc:  # noqa: BLE001
        return write_stub(args.output, f"generation failed: {exc}")

    if len(blob) > STAGE_MAX:
        return write_stub(args.output,
                          f"container is {len(blob)}B, over the {STAGE_MAX}B staging cap")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    raw_total = sum(len(a.raw) for a in assets)
    print(f"asset slot: {len(assets)} assets, {raw_total} raw -> {len(blob)} bytes "
          f"({args.output})")
    for a in assets:
        kind = "argb" if a.fmt == FMT_ARGB8888 else "a8"
        codec = "rle" if a.codec == CODEC_RLE8 else "raw"
        print(f"  id={a.id:<3} {a.width}x{a.height} {kind:<4} {codec} "
              f"{len(a.raw)} -> {len(a.stored)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
