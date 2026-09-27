/*
 * The MVII asset slot reader. Format, rationale and the DRAM map are in
 * mvii_assets.h; this file is the mechanism.
 *
 * Two properties shape all of it:
 *
 *   NO ALLOCATION. This links into a 512 KiB LK slot whose linker script asserts
 *   __bss_end stays clear of __stack_top, so a decode buffer is not available at
 *   any size worth having. Every blit therefore STREAMS: the RLE is walked once
 *   and each byte is composited into the framebuffer as it is produced. The only
 *   memory this file owns is a pointer and a validity flag.
 *
 *   NO UNALIGNED LOADS. The loader runs with the MMU off, where every access is
 *   Strongly-Ordered and an unaligned wide load is a data abort rather than a
 *   slow path (see the -mno-unaligned-access note in the ARMv7 CMakeLists). The
 *   header and entry structs are laid out so every field is naturally aligned,
 *   and the container is staged at a 1 MiB-aligned address, so the structs can
 *   be read directly. Payloads are byte streams and are read a byte at a time.
 */

#include "mvii_assets.h"

#include "mt6592_msdc.h"

/* ── State ── */

static const mvii_asset_header_t* g_hdr;
static const mvii_asset_entry_t* g_table;
static int g_ready;

/* ── CRC32, the ordinary reflected one (poly 0xedb88320) ──
 *
 * Computed a nibble at a time off a sixteen-entry table. The byte-at-a-time
 * table is 1 KiB of .rodata in an image measured in kilobytes; the nibble table
 * is 64 bytes and costs one extra shift per byte, on a check that runs once per
 * boot over a few hundred kilobytes. */
static const uint32_t kCrcNibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
    0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};

static uint32_t crc32_bytes(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0xffffffffu;
    uint32_t i;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ kCrcNibble[crc & 0xfu];
        crc = (crc >> 4) ^ kCrcNibble[crc & 0xfu];
    }
    return ~crc;
}

/* ── Load ── */

int mvii_assets_ready(void) { return g_ready; }

/* Everything that can be decided about a container without reading it again:
 * the magic, the struct sizes this loader was compiled against, whether `count'
 * can be trusted, and finally the CRC. Shared by load() and adopt() so a staged
 * container and a freshly read one are held to exactly the same standard. */
static int validate_staged(uint32_t available) {
    uint8_t* const stage = (uint8_t*)(uintptr_t)MVII_ASSET_STAGE_ADDR;
    const mvii_asset_header_t* hdr = (const mvii_asset_header_t*)(const void*)stage;
    uint32_t total;

    if (hdr->magic0 != MVII_ASSET_MAGIC0 || hdr->magic1 != MVII_ASSET_MAGIC1) return -2;
    if (hdr->version != MVII_ASSET_VERSION) return -2;
    if (hdr->header_bytes != (uint32_t)sizeof(mvii_asset_header_t)) return -2;
    if (hdr->entry_bytes != (uint32_t)sizeof(mvii_asset_entry_t)) return -2;

    total = hdr->total_bytes;
    if (total < hdr->header_bytes) return -2;
    if (total > MVII_ASSET_STAGE_MAX || total > MVII_ASSET_SLOT_BYTES) return -3;
    if (total > available) return -3;
    /* The table has to fit inside what the container claims, or `count' is a
     * licence to walk off the end of the staging window. */
    if (hdr->count > (total - hdr->header_bytes) / hdr->entry_bytes) return -2;

    if (crc32_bytes(stage + hdr->header_bytes, total - hdr->header_bytes) != hdr->crc32) return -4;

    g_hdr = hdr;
    g_table = (const mvii_asset_entry_t*)(const void*)(stage + hdr->header_bytes);
    g_ready = 1;
    return 0;
}

int mvii_assets_adopt(void) {
    if (g_ready) return 0;
    /* Nothing bounds what is already in DRAM except the window itself, so the
     * cap is the window. A container claiming more than that is refused by
     * validate_staged() rather than trusted. */
    return validate_staged(MVII_ASSET_STAGE_MAX);
}

int mvii_assets_load(void) {
    uint8_t* const stage = (uint8_t*)(uintptr_t)MVII_ASSET_STAGE_ADDR;
    const mvii_asset_header_t* hdr = (const mvii_asset_header_t*)(const void*)stage;
    uint32_t rounded;

    if (g_ready) return 0;

    /* The header first, on its own, so a container that is not there costs one
     * sector rather than two megabytes of eMMC traffic. */
    if (mt6592_emmc_read_user((uint64_t)MVII_ASSET_SLOT_OFFSET, stage, 512u) != MT6592_MSDC_OK)
        return -1;

    if (hdr->magic0 != MVII_ASSET_MAGIC0 || hdr->magic1 != MVII_ASSET_MAGIC1) return -2;
    if (hdr->version != MVII_ASSET_VERSION) return -2;
    if (hdr->total_bytes > MVII_ASSET_STAGE_MAX) return -3;

    /* Now the whole thing, header included -- re-reading 512 bytes is cheaper
     * than reasoning about a partial first transfer. Rounded up to a sector,
     * because the eMMC read is a block device underneath. */
    rounded = (hdr->total_bytes + 511u) & ~511u;
    if (rounded < 512u) rounded = 512u;
    if (rounded > MVII_ASSET_STAGE_MAX) return -3;
    if (mt6592_emmc_read_user((uint64_t)MVII_ASSET_SLOT_OFFSET, stage, rounded) != MT6592_MSDC_OK)
        return -1;

    return validate_staged(rounded);
}

const mvii_asset_entry_t* mvii_asset_find(uint32_t id) {
    uint32_t i;
    if (!g_ready) return 0;
    for (i = 0; i < g_hdr->count; ++i) {
        if (g_table[i].id == id) return &g_table[i];
    }
    return 0;
}

/* The payload, bounds-checked against what the container claimed for itself. A
 * malformed entry offset is the one way a valid CRC can still walk off the end,
 * because the CRC covers the bytes and not their meaning. */
static const uint8_t* asset_payload(const mvii_asset_entry_t* e) {
    const uint8_t* const base = (const uint8_t*)(uintptr_t)MVII_ASSET_STAGE_ADDR;
    if (!e || !g_ready) return 0;
    if (e->offset > g_hdr->total_bytes) return 0;
    if (e->length > g_hdr->total_bytes - e->offset) return 0;
    return base + e->offset;
}

const uint8_t* mvii_asset_raw(uint32_t id, uint32_t* out_bytes) {
    const mvii_asset_entry_t* e = mvii_asset_find(id);
    const uint8_t* payload = asset_payload(e);
    if (out_bytes) *out_bytes = 0u;
    if (!e || !payload || e->codec != MVII_ASSET_CODEC_RAW) return 0;
    if (e->length != e->raw_length) return 0;
    if (out_bytes) *out_bytes = e->length;
    return payload;
}

/* ── The streaming decoder ──
 *
 * One cursor over the stored payload, one over the decoded output. next_byte()
 * hands back decoded bytes in order for either codec, so both blits below are
 * written once against a flat byte stream and neither knows which encoding it
 * is reading. */

typedef struct {
    const uint8_t* p;   /* stored cursor           */
    const uint8_t* end; /* one past the payload    */
    int rle;
    uint32_t run;       /* bytes left in this run  */
    int literal;        /* run is literal, not a repeat */
    uint8_t value;      /* the repeated byte       */
} asset_stream_t;

static void stream_open(asset_stream_t* s, const mvii_asset_entry_t* e, const uint8_t* payload) {
    s->p = payload;
    s->end = payload + e->length;
    s->rle = (e->codec == MVII_ASSET_CODEC_RLE8);
    s->run = 0u;
    s->literal = 0;
    s->value = 0u;
}

/* Returns 0 when the stream is exhausted or malformed; the callers treat that as
 * "stop drawing", which degrades a truncated asset into a partial picture rather
 * than a fault. */
static int stream_next(asset_stream_t* s, uint8_t* out) {
    if (!s->rle) {
        if (s->p >= s->end) return 0;
        *out = *s->p++;
        return 1;
    }
    if (s->run == 0u) {
        uint8_t n;
        if (s->p >= s->end) return 0;
        n = *s->p++;
        if (n < 0x80u) {
            if (s->p >= s->end) return 0;
            s->run = (uint32_t)n + 1u;
            s->literal = 0;
            s->value = *s->p++;
        } else {
            s->run = (uint32_t)(n - 0x80u) + 1u;
            s->literal = 1;
        }
    }
    if (s->literal) {
        if (s->p >= s->end) return 0;
        *out = *s->p++;
    } else {
        *out = s->value;
    }
    --s->run;
    return 1;
}

/* ── Compositing ──
 *
 * Coverage against whatever is already in the surface. The park draws its
 * background first and its glyphs after, so this is the blend that makes an
 * anti-aliased edge sit on the colour it is actually over rather than on black.
 *
 * Eight-bit channels, /255 approximated as (x * cov + 127) / 255 done with the
 * usual two-shift reciprocal so there is no division in the inner loop. */
static uint32_t blend(uint32_t dst, uint32_t src, uint32_t cov) {
    uint32_t out = 0xff000000u;
    uint32_t shift;
    if (cov == 0u) return dst;
    if (cov == 255u) return src | 0xff000000u;
    for (shift = 0u; shift < 24u; shift += 8u) {
        const uint32_t d = (dst >> shift) & 0xffu;
        const uint32_t s = (src >> shift) & 0xffu;
        /* d + (s - d) * cov / 255, in unsigned arithmetic. */
        const uint32_t t = (s > d) ? ((s - d) * cov + 127u) : ((d - s) * cov + 127u);
        const uint32_t scaled = (t + ((t + 257u) >> 8)) >> 8;
        const uint32_t v = (s > d) ? (d + scaled) : (d - scaled);
        out |= (v & 0xffu) << shift;
    }
    return out;
}

int mvii_asset_blit_a8_clip(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                            uint32_t fb_h, int x, int y, uint32_t clip_w, uint32_t argb) {
    const mvii_asset_entry_t* e = mvii_asset_find(id);
    const uint8_t* payload = asset_payload(e);
    volatile uint32_t* const fb = (volatile uint32_t*)(uintptr_t)fb_addr;
    const uint32_t stride = pitch / 4u;
    asset_stream_t s;
    uint32_t row;

    if (!e || !payload || e->format != MVII_ASSET_FMT_A8) return -1;

    stream_open(&s, e, payload);

    for (row = 0u; row < e->height; ++row) {
        const int py = y + (int)row;
        uint32_t col;
        for (col = 0u; col < e->width; ++col) {
            uint8_t cov = 0u;
            const int px = x + (int)col;
            /* The stream is walked for EVERY pixel, clipped or not: it is a run
             * encoding, so skipping a byte would desynchronise the rest of the
             * image. Clipping happens at the store, not at the read. */
            if (!stream_next(&s, &cov)) return 0;
            if (cov == 0u || col >= clip_w) continue;
            if (px < 0 || py < 0 || (uint32_t)px >= fb_w || (uint32_t)py >= fb_h) continue;
            fb[(uint32_t)py * stride + (uint32_t)px] =
                blend(fb[(uint32_t)py * stride + (uint32_t)px], argb, cov);
        }
    }
    return 0;
}

int mvii_asset_blit_a8(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w, uint32_t fb_h,
                       int x, int y, uint32_t argb) {
    return mvii_asset_blit_a8_clip(id, fb_addr, pitch, fb_w, fb_h, x, y, 0xffffffffu, argb);
}

int mvii_asset_blit_argb(uint32_t id, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                         uint32_t fb_h, int x, int y) {
    const mvii_asset_entry_t* e = mvii_asset_find(id);
    const uint8_t* payload = asset_payload(e);
    volatile uint32_t* const fb = (volatile uint32_t*)(uintptr_t)fb_addr;
    const uint32_t stride = pitch / 4u;
    asset_stream_t s;
    uint32_t row;

    if (!e || !payload || e->format != MVII_ASSET_FMT_ARGB8888) return -1;

    stream_open(&s, e, payload);

    for (row = 0u; row < e->height; ++row) {
        const int py = y + (int)row;
        uint32_t col;
        for (col = 0u; col < e->width; ++col) {
            const int px = x + (int)col;
            uint8_t b0 = 0u, b1 = 0u, b2 = 0u, b3 = 0u;
            if (!stream_next(&s, &b0)) return 0;
            if (!stream_next(&s, &b1)) return 0;
            if (!stream_next(&s, &b2)) return 0;
            if (!stream_next(&s, &b3)) return 0;
            if (px < 0 || py < 0 || (uint32_t)px >= fb_w || (uint32_t)py >= fb_h) continue;
            /* Stored little-endian ARGB, i.e. B,G,R,A on the wire. */
            fb[(uint32_t)py * stride + (uint32_t)px] = ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) |
                                                       ((uint32_t)b1 << 8) | (uint32_t)b0;
        }
    }
    return 0;
}

/* ── Numerals ──
 *
 * The digits are separate assets rather than one strip, so the host can give
 * them proportional widths -- a '1' is not as wide as a '0' and a fixed cell
 * makes a percentage look like a serial number. Tracking is the loader's, since
 * it is the only thing here that depends on how big the run has to be. */

static uint32_t percent_digits(uint32_t pct, uint32_t* digits) {
    uint32_t n = 0u;
    if (pct > 100u) pct = 100u;
    if (pct == 0u) {
        digits[n++] = 0u;
        return n;
    }
    {
        uint32_t tmp[3];
        uint32_t t = 0u;
        uint32_t v = pct;
        while (v > 0u && t < 3u) {
            tmp[t++] = v % 10u;
            v /= 10u;
        }
        while (t > 0u) digits[n++] = tmp[--t];
    }
    return n;
}

uint32_t mvii_asset_percent_width(uint32_t pct, uint32_t tracking) {
    uint32_t digits[3];
    const uint32_t n = percent_digits(pct, digits);
    const mvii_asset_entry_t* sign = mvii_asset_find(MVII_ASSET_PERCENT);
    uint32_t w = 0u;
    uint32_t i;

    if (!sign) return 0u;
    for (i = 0u; i < n; ++i) {
        const mvii_asset_entry_t* g = mvii_asset_find(MVII_ASSET_DIGIT(digits[i]));
        if (!g) return 0u;
        w += g->width + tracking;
    }
    return w + sign->width;
}

int mvii_asset_draw_percent(uint32_t pct, uint32_t fb_addr, uint32_t pitch, uint32_t fb_w,
                            uint32_t fb_h, int x, int y, uint32_t tracking, uint32_t argb) {
    uint32_t digits[3];
    const uint32_t n = percent_digits(pct, digits);
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        const uint32_t id = MVII_ASSET_DIGIT(digits[i]);
        const mvii_asset_entry_t* g = mvii_asset_find(id);
        if (!g) return -1;
        if (mvii_asset_blit_a8(id, fb_addr, pitch, fb_w, fb_h, x, y, argb) != 0) return -1;
        x += (int)g->width + (int)tracking;
    }
    return mvii_asset_blit_a8(MVII_ASSET_PERCENT, fb_addr, pitch, fb_w, fb_h, x, y, argb);
}
