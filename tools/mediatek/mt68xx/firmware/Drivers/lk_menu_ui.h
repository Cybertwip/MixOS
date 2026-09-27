/* The boot-choice look, as pure arithmetic.
 *
 * The LK's five-second window wears the MixOS loading screen: the 5x7 console
 * font, the shadowed stage/detail lines, the accent bar and the ring of
 * orbiting dots, all laid out in mixsplash's fractions of the panel. The
 * glyphs below ARE mixsplash's FONT5X7 (device/j36-ultra/tools/mixsplash.c),
 * byte for byte; the spinner is its easing and stagger in fixed point,
 * because this image has no floating point to spend.
 *
 * Everything here is registers-free and DRAM-free, so the firmware and the
 * host test (../tests/test-bootmenu.c) compile the same logic -- including
 * the save-box cover check, which is what keeps a bigger future panel (the
 * Oppo and LG ports) from painting outside the region the dismiss restores.
 */
#ifndef MVII_LK_MENU_UI_H
#define MVII_LK_MENU_UI_H

#include <stdint.h>

/* ── the 5x7 console font ────────────────────────────────────────────────
 * Column-major, five bytes a glyph, bit 0 the top row; ASCII 0x20..0x7e.
 * mixsplash's table, verbatim. */
static const unsigned char LK_FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, /*   ! */
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14}, /* " # */
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, /* $ % */
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, /* & ' */
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, /* ( ) */
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08}, /* * + */
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, /* , - */
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}, /* . / */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, /* 0 1 */
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, /* 2 3 */
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, /* 4 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, /* 6 7 */
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, /* 8 9 */
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00}, /* : ; */
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, /* < = */
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, /* > ? */
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, /* @ A */
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, /* B C */
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, /* D E */
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, /* F G */
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, /* H I */
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, /* J K */
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, /* L M */
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, /* N O */
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, /* P Q */
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, /* R S */
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, /* T U */
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, /* V W */
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, /* X Y */
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00}, /* Z [ */
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, /* \ ] */
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, /* ^ _ */
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, /* ` a */
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, /* b c */
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, /* d e */
    {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E}, /* f g */
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, /* h i */
    {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00}, /* j k */
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, /* l m */
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, /* n o */
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, /* p q */
    {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20}, /* r s */
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, /* t u */
    {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C}, /* v w */
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, /* x y */
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, /* z { */
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, /* | } */
    {0x08,0x08,0x2A,0x1C,0x08},                             /* ~   */
};

/* 5 columns + 1 gap per cell, less the last gap. */
static inline uint32_t lk_menu_text_width(const char* s, uint32_t scale) {
    uint32_t n = 0u;
    while (*s++ != 0) ++n;
    return n == 0u ? 0u : n * 6u * scale - scale;
}

/* mixsplash's mix_rgb, exactly -- truncating, no rounding bias -- lifted to an
 * opaque ARGB pixel, which is what this canvas holds. */
static inline uint32_t lk_menu_mix(uint32_t base, uint32_t over, uint32_t alpha) {
    int32_t r, g, b;
    if (alpha > 255u) alpha = 255u;
    /* Signed throughout, like the original: an overlay darker than the base
     * goes negative mid-expression, and unsigned would wrap it to white. */
    r = (int32_t)((base >> 16u) & 0xffu);
    r += ((int32_t)((over >> 16u) & 0xffu) - r) * (int32_t)alpha / 255;
    g = (int32_t)((base >> 8u) & 0xffu);
    g += ((int32_t)((over >> 8u) & 0xffu) - g) * (int32_t)alpha / 255;
    b = (int32_t)(base & 0xffu);
    b += ((int32_t)(over & 0xffu) - b) * (int32_t)alpha / 255;
    return 0xff000000u | ((uint32_t)r << 16u) | ((uint32_t)g << 8u) | (uint32_t)b;
}

/* mixsplash's add_rgb, exactly -- the bar glint's travelling highlight. */
static inline uint32_t lk_menu_add(uint32_t base, int32_t ar, int32_t ag, int32_t ab) {
    int32_t r = (int32_t)((base >> 16u) & 0xffu) + ar;
    int32_t g = (int32_t)((base >> 8u) & 0xffu) + ag;
    int32_t b = (int32_t)(base & 0xffu) + ab;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return 0xff000000u | ((uint32_t)r << 16u) | ((uint32_t)g << 8u) | (uint32_t)b;
}

enum {
    LK_MENU_SPIN_DOTS = 6,
    LK_MENU_SPIN_RADIUS_REF = 26, /* at the 640x480 reference; scaled by min dim */
    LK_MENU_SPIN_DOT_REF = 3,
    LK_MENU_SPIN_PERIOD_MS = 2000,
    LK_MENU_SPIN_STAGGER_MS = 100
};

/* sin(2π·i/256)·256 by Bhaskara I: within an LSB or two of libm, no table,
 * no floating point. i wraps, so callers pass raw phase sums. */
static inline int32_t lk_sin256(uint32_t i) {
    /* 1024·X(128−X) / (20480 − X(128−X)) on the half-wave, negated past it. */
    uint32_t x = i & 127u;
    uint32_t prod = x * (128u - x);
    int32_t v = (int32_t)((1024u * prod) / (20480u - prod));
    return (i & 128u) != 0u ? -v : v;
}

/* mixsplash's easing in 1/256 turns: ease(u) = u + k·sin(2πu)/2π with k = 3/4,
 * so the dots run at 1.75x through the top and 0.25x at the foot. p is the
 * raw phase; the answer overshoots it by ±31 at most and never runs back. */
static inline int32_t lk_spin_ease256(uint32_t p) {
    int32_t t = 3 * lk_sin256(p & 255u);
    t += (t >= 0) ? 12 : -12;
    return (int32_t)p + t / 25;
}

/* One dot's raw phase at t_ms, stagger i behind the leader, wrapped. */
static inline uint32_t lk_spin_phase256(uint32_t t_ms, uint32_t dot) {
    int32_t n = (int32_t)t_ms - (int32_t)(dot * (uint32_t)LK_MENU_SPIN_STAGGER_MS);
    n *= 256;
    n %= (int32_t)((uint32_t)LK_MENU_SPIN_PERIOD_MS * 256u);
    if (n < 0) n += (int32_t)((uint32_t)LK_MENU_SPIN_PERIOD_MS * 256u);
    return (uint32_t)n / (uint32_t)LK_MENU_SPIN_PERIOD_MS;
}

/* Orbit offset for an eased phase: top at 0, clockwise, y-down. */
static inline void lk_spin_offset(uint32_t eased, uint32_t r, int32_t* dx, int32_t* dy) {
    int32_t sx = lk_sin256(eased & 255u);
    int32_t sy = lk_sin256((eased + 64u) & 255u);
    *dx = (sx * (int32_t)r + (sx >= 0 ? 128 : -128)) / 256;
    *dy = -(sy * (int32_t)r + (sy >= 0 ? 128 : -128)) / 256;
}

typedef struct {
    uint32_t x, y, w, h;
} lk_menu_box_t;

static inline uint32_t lk_menu_frac(uint32_t dim, uint32_t permille) {
    return (dim * permille) / 1000u;
}

/* mixsplash's layout fractions, shared so both screens measure alike. */
static inline uint32_t lk_menu_spinner_cx(uint32_t w) { return w / 2u; }
static inline uint32_t lk_menu_spinner_cy(uint32_t h) { return lk_menu_frac(h, 775u); }
static inline uint32_t lk_menu_stage_y(uint32_t h) { return lk_menu_frac(h, 862u); }
static inline uint32_t lk_menu_detail_y(uint32_t h) { return lk_menu_frac(h, 910u); }
static inline uint32_t lk_menu_bar_w(uint32_t w) { return (56u * w) / 100u; }
static inline uint32_t lk_menu_bar_x(uint32_t w) { return (w - lk_menu_bar_w(w)) / 2u; }
static inline uint32_t lk_menu_bar_y(uint32_t h) { return lk_menu_frac(h, 958u); }

/* The orbit radius follows the smaller panel axis, so the ring keeps its
 * proportions on a bigger (Oppo/LG) panel instead of shrinking into a dot. */
static inline uint32_t lk_menu_spin_radius(uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h;
    uint32_t r = ((uint32_t)LK_MENU_SPIN_RADIUS_REF * m + 240u) / 480u;
    return r < 4u ? 4u : r;
}

static inline uint32_t lk_menu_spin_dot(uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h;
    uint32_t d = ((uint32_t)LK_MENU_SPIN_DOT_REF * m + 240u) / 480u;
    return d < 1u ? 1u : d;
}

/* The box one spinner repaint touches: orbit plus dot plus one AA ring. */
static inline lk_menu_box_t lk_menu_spinner_box(uint32_t w, uint32_t h) {
    lk_menu_box_t b;
    uint32_t c = lk_menu_spinner_cx(w), cy = lk_menu_spinner_cy(h);
    uint32_t r = lk_menu_spin_radius(w, h) + lk_menu_spin_dot(w, h) + 1u;
    b.x = c > r ? c - r : 0u;
    b.y = cy > r ? cy - r : 0u;
    b.w = 2u * r + 1u;
    b.h = 2u * r + 1u;
    return b;
}

/* mixsplash's text_bounds: mask plus the shadow's offset, so a message that
 * shrinks leaves no tail. */
static inline lk_menu_box_t lk_menu_text_bounds(uint32_t cx, uint32_t y, const char* s,
                                                uint32_t scale) {
    lk_menu_box_t b;
    uint32_t tw = lk_menu_text_width(s, scale);
    b.x = cx > tw / 2u + 2u ? cx - tw / 2u - 2u : 0u;
    b.y = y > 2u ? y - 2u : 0u;
    b.w = tw + 4u + 2u * scale;
    b.h = 7u * scale + 4u + 2u * scale;
    return b;
}

static inline lk_menu_box_t lk_menu_bar_box(uint32_t w, uint32_t h) {
    lk_menu_box_t b;
    b.x = lk_menu_bar_x(w);
    b.y = lk_menu_bar_y(h);
    b.w = lk_menu_bar_w(w);
    b.h = 4u;
    return b;
}

/* The dismiss region: one box over spinner, stage, detail and bar. The menu
 * saves the splash pixels under it on entry and puts them back when the tag
 * goes away, so the fractions must cover every paint box above on every
 * panel -- the host test pins that at three sizes. */
static inline lk_menu_box_t lk_menu_save_box(uint32_t w, uint32_t h) {
    lk_menu_box_t b;
    b.x = lk_menu_frac(w, 140u);
    b.y = lk_menu_frac(h, 704u);
    b.w = lk_menu_frac(w, 726u);
    b.h = lk_menu_frac(h, 267u);
    return b;
}

static inline uint32_t lk_menu_box_covers(lk_menu_box_t outer, lk_menu_box_t inner) {
    return outer.x <= inner.x && outer.y <= inner.y &&
           outer.x + outer.w >= inner.x + inner.w &&
           outer.y + outer.h >= inner.y + inner.h;
}

#endif /* MVII_LK_MENU_UI_H */
