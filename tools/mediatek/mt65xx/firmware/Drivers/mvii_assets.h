#pragma once

/*
 * ══════════════════════════════════════════════════════════════════════════════
 *  THE MVII ASSET SLOT — every picture this device draws before the OS exists
 * ══════════════════════════════════════════════════════════════════════════════
 *
 * WHY THERE IS A SLOT AT ALL.
 *
 * Until now, changing a boot picture meant rebuilding a bootloader. The Microsoft
 * logo was `.incbin`'d into stage1 by a full-frame logo object, and every other
 * screen -- the battery outline, its fill, the percentage, the plug, the bolt --
 * was C in mvii_lk_main.c drawing axis-aligned rectangles, with the numerals
 * coming from a twelve-entry 3x5 bitmap table scaled ten times. Both of those
 * facts had the same consequence: art was code, so art needed a compile, a
 * relink, a 2 MiB LK image and a full flash. The operator's words for it were
 * "small iterations, not an embedded screen every time".
 *
 * So the pictures move out of the image and onto the medium, in their own
 * partition, written by their own flag:
 *
 *     ./flash -target=arm -dbg -device /dev/cu.usbmodem... -assets -yes
 *
 * That writes THIS container and nothing else. The bootloader does not change,
 * the OS does not change, and the round trip is one partition instead of three.
 *
 * WHERE IT LIVES: the stock LOGO partition, 0x38c0000, 8 MiB, from the J36
 * scatter (Reference/J36-ULTRA/MT6592_Android_scatter.txt, SYS13). It is the one
 * region on this eMMC that (a) is large, (b) is declared downloadable by the
 * vendor's own table, and (c) holds something MVII has never once read: the
 * Android boot logo. Nothing is displaced and nothing shifts, so a board flashed
 * with assets can still be flashed back to stock without a layout change.
 *
 * ── WHY A8 MASKS AND NOT PICTURES ──
 *
 * Almost everything in here is stored as an 8-bit COVERAGE MASK, not as colour.
 * That is the whole reason the slot can carry smooth artwork without giving up
 * what the loader needs.
 *
 * The park screen's ink is not a constant. It is white on mains, grey the instant
 * the charger is pulled, and the gauge fill tracks the centre LED so the two can
 * never disagree about what they are reporting. A stored ARGB icon fixes its
 * colour at generation time and would need one copy per state -- five copies of
 * a battery outline that differ only in ink. A mask stores COVERAGE (how much of
 * this pixel the shape covers, 0..255) and is tinted at draw time, so one asset
 * serves every state, and the fractional edge values are exactly what makes the
 * edges smooth. Anti-aliasing and recolouring turn out to be the same feature.
 *
 * The logo is the exception and is stored ARGB8888: it is a photograph-like
 * multi-colour image, there is nothing to tint, and it is drawn once.
 *
 * ── THE ART IS DESCRIBED BY THE DATA, NOT BY THE LOADER ──
 *
 * MVII_ASSET_BATTERY carries an `inset' rectangle: the window inside the outline
 * where the charge fill belongs. The loader reads that rectangle out of the
 * asset rather than deriving it from hardcoded border widths, which is what lets
 * the outline be redrawn -- thicker, rounder, a different aspect -- on the host
 * with no C change at all. Same idea for every other placement: the host decides
 * geometry, the loader composites.
 *
 * ── ENDIANNESS AND ALIGNMENT ──
 *
 * Little-endian throughout, matching the SoC. Header and entries are sized so
 * every field is naturally aligned, because this image runs with the MMU off and
 * -mno-unaligned-access: an unaligned wide load here is a data abort, not a
 * slowdown.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Where the container lives ── */

/* SYS13 LOGO in the stock scatter. */
#define MVII_ASSET_SLOT_OFFSET 0x38c0000u
#define MVII_ASSET_SLOT_BYTES 0x800000u

/*
 * Where the loader stages it in DRAM, and why here.
 *
 *   0x80008000  kernel, 8 MiB reserved
 *   0x81e00000  the LK image, 512 KiB
 *   0x82700000  the LK framebuffer, 640*480*4 = 0x12c000, ending 0x8282c000
 *   0x82900000  THIS, capped at MVII_ASSET_STAGE_MAX below
 *   0x82b00000  the LK's off-screen compose canvas, framebuffer-sized, ending
 *               0x82c2c000 -- deliberately starting where this stage's cap ends,
 *               because it once started AT 0x82900000 and its first full-screen
 *               clear painted black over the container it was blitting from
 *   0x82d00000  .gpubss, 3 MiB: mt6592_gpu_offload.c's Mali job buffers, placed
 *               here by mvii_lk_linker.ld because ~1.9 MiB of PLB heap and tile
 *               lists will not fit the LK's 512 KiB image window. Ends exactly
 *               at the firmware scratch below, so an overrun is a link error
 *   0x83000000  mvii_debug_console's WLAN firmware scratch, 512 KiB
 *   0x83100000  the CONSYS EMI share window (the firmware's own log is at
 *               0x83163010) -- do not go near it
 *   0x84000000  the ramdisk
 *
 * 0x82900000 is past the end of the framebuffer and short of the firmware
 * scratch. It is also ABOVE stage1's RAMDISK_SCAN_END (0x82600000), which is the
 * part that is not obvious: when stage1 cannot get a ramdisk address out of the
 * ATAGs it scans DRAM for an image magic, and a blob left lying inside that
 * window is exactly what such a scan finds by mistake.
 *
 * The 2 MiB cap is on what the loader will ACCEPT, not on what the partition can
 * hold. The partition is 8 MiB; a container that claims more than 2 is refused
 * rather than staged, because staging it would run into the firmware scratch.
 */
#define MVII_ASSET_STAGE_ADDR 0x82900000u
#define MVII_ASSET_STAGE_MAX 0x00200000u

/* ── The container ── */

#define MVII_ASSET_MAGIC0 0x4949564du /* "MVII" */
#define MVII_ASSET_MAGIC1 0x31535341u /* "ASS1" */
#define MVII_ASSET_VERSION 1u

typedef struct {
    uint32_t magic0;      /* MVII_ASSET_MAGIC0                                */
    uint32_t magic1;      /* MVII_ASSET_MAGIC1                                */
    uint32_t version;     /* MVII_ASSET_VERSION                               */
    uint32_t header_bytes;/* sizeof(mvii_asset_header_t); the table follows it */
    uint32_t entry_bytes; /* sizeof(mvii_asset_entry_t)                       */
    uint32_t count;       /* entries in the table                             */
    uint32_t total_bytes; /* header + table + every payload                   */
    uint32_t crc32;       /* over [header_bytes .. total_bytes)               */
} mvii_asset_header_t;

/* Pixel formats. */
enum {
    MVII_ASSET_FMT_A8 = 0,      /* one coverage byte per pixel, tinted at draw */
    MVII_ASSET_FMT_ARGB8888 = 1 /* four bytes per pixel, drawn as-is           */
};

/* Payload encodings. */
enum {
    MVII_ASSET_CODEC_RAW = 0,
    /*
     * RLE8, over BYTES (so it compresses an A8 mask and an ARGB image alike).
     *
     *   n < 0x80   the next single byte, repeated n + 1 times   (1..128)
     *   n >= 0x80  the next (n - 0x80) + 1 bytes, verbatim      (1..128)
     *
     * Chosen because these masks are overwhelmingly 0x00 and 0xff with a thin
     * anti-aliased seam between them, and because it decodes in a single pass
     * with no window and no state -- the loader streams it straight into the
     * framebuffer and never allocates a decode buffer at all.
     */
    MVII_ASSET_CODEC_RLE8 = 1
};

typedef struct {
    uint32_t id;         /* MVII_ASSET_*                                      */
    uint16_t width;
    uint16_t height;
    uint8_t format;      /* MVII_ASSET_FMT_*                                  */
    uint8_t codec;       /* MVII_ASSET_CODEC_*                                */
    uint16_t flags;      /* reserved, zero                                    */
    uint32_t offset;     /* payload, from the start of the container          */
    uint32_t length;     /* payload bytes as stored                           */
    uint32_t raw_length; /* payload bytes once decoded                        */
    /* The art's own opinion about where things go inside it. For the battery
     * this is the window the charge fill occupies; zero-sized means "none". */
    uint16_t inset_x;
    uint16_t inset_y;
    uint16_t inset_w;
    uint16_t inset_h;
    uint32_t crc32;      /* over the STORED payload                           */
    uint32_t reserved;
} mvii_asset_entry_t;

/* ── The catalogue ──
 *
 * Numbers are permanent once flashed: a container built by an older host must
 * still be readable, so ids are never reused for a different picture. Gaps are
 * deliberate and cheap. */
enum {
    MVII_ASSET_LOGO = 1,        /* the boot logo, ARGB8888, full screen        */
    MVII_ASSET_BATTERY = 2,     /* outline + terminal nub, with a fill inset   */
    MVII_ASSET_PLUG = 3,        /* mains plug, cable joined                    */
    MVII_ASSET_PLUG_BROKEN = 4, /* the same plug with the cable come apart     */
    MVII_ASSET_BOLT = 5,        /* lightning, punched out of the plug body     */
    /* The gauge bar, sized to MVII_ASSET_BATTERY's inset exactly, so the loader
     * places it by adding the inset origin and shows a level by clipping its
     * width. A separate asset rather than a rectangle because the bar has the
     * outline's rounded corners, and because the two then share one geometry:
     * change the outline on the host and the bar follows it. */
    MVII_ASSET_BATTERY_FILL = 6,

    MVII_ASSET_DIGIT_0 = 16,    /* .. MVII_ASSET_DIGIT_0 + 9                   */
    MVII_ASSET_PERCENT = 26
};

#define MVII_ASSET_DIGIT(d) ((uint32_t)MVII_ASSET_DIGIT_0 + (uint32_t)(d))

/* ── The loader ──
 *
 * All of it is safe to call before the slot has been read, and safe to call on a
 * board whose LOGO partition has never been written: every accessor answers
 * "absent" rather than faulting, and mvii_assets_ready() is the one question a
 * caller has to ask before choosing between the slot and its own fallback. */

/*
 * Read the container off eMMC into MVII_ASSET_STAGE_ADDR and validate it.
 *
 * Returns 0 when the slot is usable, negative otherwise:
 *   -1 the eMMC read failed
 *   -2 no magic, or a version this loader does not know
 *   -3 the container is larger than the staging window will accept
 *   -4 the CRC does not match
 * Idempotent: a second call after success is free.
 */
int mvii_assets_load(void);

/*
 * Validate a container SOMEBODY ELSE already staged, without touching eMMC.
 *
 * This is how stage1 gets the logo. stage1 runs after LK has loaded boot.img,
 * so the slot is already sitting at MVII_ASSET_STAGE_ADDR by then and a second
 * eMMC read would be two seconds of work to fetch bytes that are in DRAM. It is
 * also how the fallback stays honest: booted under the STOCK MediaTek LK nobody
 * staged anything, this returns non-zero, and stage1 paints its built-in blob.
 *
 * Same return codes as mvii_assets_load() minus -1.
 */
int mvii_assets_adopt(void);

/* Did a load succeed? Cheap; no eMMC traffic. */
int mvii_assets_ready(void);

/* The entry for @p id, or 0. */
const mvii_asset_entry_t* mvii_asset_find(uint32_t id);

/*
 * A direct pointer to an asset's pixels, valid ONLY when it was stored
 * uncompressed; 0 otherwise (and 0 if the slot is absent).
 *
 * The escape hatch for a caller that has its own painter and wants the bytes
 * rather than the blit. stage1 is that caller: it copies a full-screen BGRA
 * frame into the live framebuffer with one memcpy, and a per-pixel composite of
 * 307,200 uncached writes plus 307,200 uncached reads is a visibly slower boot
 * splash for no gain on an image with no transparency in it. The generator
 * therefore always stores MVII_ASSET_LOGO raw.
 */
const uint8_t* mvii_asset_raw(uint32_t id, uint32_t* out_bytes);

/*
 * Composite an A8 asset into a 32-bit framebuffer, tinted @p argb.
 *
 * Streams the RLE straight into the surface -- there is no decode buffer, which
 * is what makes it usable in a loader whose entire .bss has to fit under the
 * stack. Coverage is applied as an alpha blend against what is already there, so
 * the anti-aliased edge lands on the background it is actually drawn over.
 *
 * @p x @p y  top-left, in pixels; the asset is clipped to the surface.
 * Returns 0 on success, -1 if the asset is missing or the wrong format.
 */
int mvii_asset_blit_a8(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                       uint32_t fb_h, int x, int y, uint32_t argb);

/*
 * The same, showing only the leftmost @p clip_w columns.
 *
 * This is how a level is drawn. The alternative -- generating one bar per
 * percentage, or scaling one -- either bloats the slot a hundredfold or
 * resamples an already-anti-aliased edge twice. Clipping costs nothing: the RLE
 * still has to be walked for every pixel to stay in step, and a clipped column
 * simply is not stored.
 *
 * @p clip_w 0 draws nothing; anything at or past the asset width draws it whole.
 */
int mvii_asset_blit_a8_clip(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                            uint32_t fb_h, int x, int y, uint32_t clip_w, uint32_t argb);

/*
 * The same for an ARGB8888 asset, drawn opaque. @p x @p y as above; a
 * full-screen logo is drawn at 0,0 or centred by the caller.
 */
int mvii_asset_blit_argb(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                         uint32_t fb_h, int x, int y);

/* Total width of "<digits>%" if it were laid out with @p tracking pixels
 * between glyphs, so a caller can centre it before drawing it. 0 if the numerals
 * are not in the slot. */
uint32_t mvii_asset_percent_width(uint32_t pct, uint32_t tracking);

/*
 * Draw "<pct>%" with its left edge at @p x, baseline-free (top-aligned at @p y),
 * tinted @p argb. Pairs with mvii_asset_percent_width() for centring.
 * Returns 0 on success, -1 if any glyph is missing.
 */
int mvii_asset_draw_percent(uint32_t pct, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                            uint32_t fb_h, int x, int y, uint32_t tracking, uint32_t argb);

#ifdef __cplusplus
}
#endif
