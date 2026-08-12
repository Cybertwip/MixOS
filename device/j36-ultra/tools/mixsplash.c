/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * mixsplash -- the boot splash, from the initramfs to the dashboard.
 *
 * WHAT IT REPLACES.  /init used to say where the boot had got to by echoing
 * lines at /dev/tty1, so the panel showed a wall of kernel text with the
 * occasional sentence of ours in it.  Everything below is still said on the
 * serial console, verbatim and in the same order; this draws the same story on
 * the panel instead, over resources/MixOS.jpg.
 *
 * WHY IT IS NOT PLYMOUTH.  Plymouth wants a DRM device or, failing that, its
 * fbdev renderer plus a theme, a udev, a D-Bus name and a place in the initrd
 * that dracut builds.  This board has none of that: the initramfs is BusyBox and
 * one shell script, there is no ld.so before switch_root, and the display is the
 * framebuffer the LK left lit at 0x82700000 -- lima registers WITHOUT
 * DRIVER_MODESET, so there is no CRTC for anyone to take.  So: one static ARM
 * binary, one mmap of /dev/fb0, no dependencies at all.
 *
 * HOW IT KEEPS THE SCREEN.  The kernel's fbcon owns the same pixels and will
 * scribble over them the moment anything writes to the console.  The fix is the
 * one plymouth uses -- ioctl(KDSETMODE, KD_GRAPHICS) on the active VT, which
 * stops fbcon painting without touching what is already in the framebuffer.  It
 * is put back to KD_TEXT on the way out UNLESS the boot reached its hand-over,
 * because the thing that comes next is mixdash, which draws through Qt's linuxfb
 * plugin with nographicsmodeswitch and wants the console left exactly as it is.
 * A boot that dies before hand-over gets its text console back and can be read.
 *
 * THE PROTOCOL is a file that /init appends lines to -- see the message channel
 * further down for why a file and not a pipe -- one message per line:
 *
 *     stage:<text>     the big line -- "Mounting the OS partition"
 *     detail:<text>    the small line under it -- a device node, a module name
 *     progress:<0-100> the bar at the foot of the screen
 *     handover         the rootfs is about to take over; do not time out, and
 *                      leave the console in graphics mode when asked to quit
 *     quit             exit now, honouring any hand-over
 *     abort            exit now and put the text console back regardless
 *
 * Anything without a recognised prefix is taken as stage text, so `say' can be
 * teed into the channel without every call site learning a vocabulary.
 *
 * SURVIVING switch_root.  This process keeps running across it.  Its binary is
 * unlinked with the rest of the initramfs, which costs nothing -- the text is
 * already mapped.  The channel lives in /dev, which /init hands across with
 * `mount --move', so it is still writable from the new root by the same path:
 * systemd units can go on narrating.  The animation carries on until
 * mixdash.service's ExecStartPre kills it.
 *
 * THE EFFECTS are the ones the picture asks for: a specular sweep of the kind
 * that crosses a Windows lock screen, a slow breath of light behind the
 * four-pane mark, and the ring of orbiting dots that has meant "Windows is
 * starting" since Windows 8.  All of it is additive over the wallpaper and all
 * of it is redrawn from a damage list, so a frame costs about 160k pixels rather
 * than 307k -- which on a Cortex-A7 sharing one DRAM bus with the display
 * controller is the difference between 25 fps and a slideshow.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── the picture ─────────────────────────────────────────────────────────── */

#define MIXSPL_MAGIC "MIXSPL1\0"
#define MIXSPL_HDR   24

/* ── the layout, as fractions of the panel ───────────────────────────────────
 *
 * Fractions and not pixels: the wallpaper is 640x480 and so is this panel, but
 * the one thing about a hand-held that changes between revisions is the LCD, and
 * a splash that has to be re-measured when it does is a splash that will be
 * wrong.  The reference positions below were read off MixOS.jpg at 640x480. */
#define SPINNER_CY_F   0.775   /* under the wordmark, above the text           */
#define STAGE_Y_F      0.862
#define DETAIL_Y_F     0.910
#define BAR_Y_F        0.958
#define BAR_W_F        0.56
#define LOGO_CX_F      0.308   /* the centre of the four-pane mark             */
#define LOGO_CY_F      0.490

/* ── the 5x7 console font ────────────────────────────────────────────────────
 *
 * Column-major, five bytes a glyph, bit 0 the top row; ASCII 0x20..0x7e and
 * nothing else, because nothing else is ever passed to it -- /init is a shell
 * script in the C locale.  Drawn at an integer scale with a one-pixel drop
 * shadow, which is what makes 5x7 legible over a photograph. */
static const unsigned char FONT5X7[95][5] = {
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

/* ── state ───────────────────────────────────────────────────────────────── */

struct fbdev {
    int fd;
    unsigned char *mem;
    size_t maplen;
    size_t origin;      /* byte offset of pixel (0,0) inside the mapping     */
    int w, h;
    unsigned stride;    /* bytes per row                                     */
    unsigned bpp;       /* 16 or 32                                          */
    struct fb_bitfield r, g, b;
};

/*
 * THE DAMAGE LIST, AND WHY THE EFFECTS DO NOT RESTORE THEIR OWN BOXES.
 *
 * Every effect is additive over the wallpaper, so each one has to start from
 * wallpaper -- and the first version had each effect memcpy its own box back
 * from the picture before drawing.  That is wrong the moment two of them
 * overlap: the sweep crosses the logo, the glow restores the logo's box, and the
 * band comes out with a circular bite taken out of it.
 *
 * So restoring happens once, at the top of the frame, over the rectangles the
 * LAST frame touched; after that every effect reads and writes the canvas and
 * composites over whatever is already there.  The list is carried between frames
 * for that reason, and the blit at the end covers the union of both -- a
 * rectangle that was restored but not redrawn still has to reach the panel.
 *
 * Overflowing the list is not a correctness problem: damage() gives up and marks
 * the whole screen, which is slower and completely correct.
 */
#define DAMAGE_MAX 16

struct rect { int x, y, w, h; };

static struct rect g_damage[DAMAGE_MAX];
static int g_ndamage;
static int g_damage_all;

static struct rect g_prev[DAMAGE_MAX];
static int g_nprev;
static int g_prev_all = 1;      /* frame one restores everything */

static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_winch;   /* SIGUSR1: repaint everything        */

static int g_verbose;

/* ── the two transcendentals this needs, so that it needs no libm ────────────
 *
 * cos and sin of an angle in radians, to about six decimal places, out of the
 * minimax polynomials for the first quadrant.  Linking libm instead would work
 * -- libm.a is in the same libc6-dev the kernel build already requires -- but
 * this binary is -static and lives in the initramfs, and forty lines of
 * polynomial is a better trade than a link dependency on the boot path for two
 * calls a frame.  Accuracy is irrelevant here anyway: the results index pixels.
 */
static double sp_cos(double x);

static double sp_sin(double x)
{
    static const double TWO_PI = 6.283185307179586;
    double x2, y;
    int neg = 0;

    /* Fold to [0, 2pi), then to [0, pi/2] with the quadrant symmetries. */
    x -= TWO_PI * (double)(long)(x / TWO_PI);
    if (x < 0.0)
        x += TWO_PI;
    if (x > 3.141592653589793) {
        x -= 3.141592653589793;
        neg = 1;
    }
    if (x > 1.5707963267948966)
        x = 3.141592653589793 - x;

    x2 = x * x;
    y = x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 +
        x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0)))));
    return neg ? -y : y;
}

static double sp_cos(double x)
{
    return sp_sin(x + 1.5707963267948966);
}

static void note(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "mixsplash: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void on_term(int sig) { (void)sig; g_quit = 1; }
static void on_usr1(int sig) { (void)sig; g_winch = 1; }

/* ── damage ──────────────────────────────────────────────────────────────── */

static void damage(int x, int y, int w, int h)
{
    if (g_damage_all)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (g_ndamage >= DAMAGE_MAX) {
        g_damage_all = 1;
        return;
    }
    g_damage[g_ndamage].x = x;
    g_damage[g_ndamage].y = y;
    g_damage[g_ndamage].w = w;
    g_damage[g_ndamage].h = h;
    ++g_ndamage;
}

/* Put last frame's rectangles back to wallpaper and mark them for this frame's
 * blit, so a shrinking effect leaves nothing of itself behind. */
static void restore_damage(uint32_t *canvas, const uint32_t *img, int w, int h)
{
    int i, y;

    g_ndamage = 0;
    g_damage_all = 0;

    if (g_prev_all) {
        memcpy(canvas, img, (size_t)w * h * 4);
        g_damage_all = 1;
        return;
    }
    for (i = 0; i < g_nprev; ++i) {
        const struct rect *r = &g_prev[i];
        for (y = r->y; y < r->y + r->h; ++y) {
            if (y < 0 || y >= h)
                continue;
            memcpy(canvas + (size_t)y * w + r->x, img + (size_t)y * w + r->x,
                   (size_t)r->w * 4);
        }
        damage(r->x, r->y, r->w, r->h);
    }
}

static void keep_damage(void)
{
    g_prev_all = g_damage_all;
    g_nprev = g_ndamage;
    memcpy(g_prev, g_damage, sizeof(g_prev[0]) * (size_t)g_ndamage);
}

/* ── the framebuffer ─────────────────────────────────────────────────────── */

static int fb_open(struct fbdev *fb, const char *path)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) {
        fprintf(stderr, "mixsplash: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "mixsplash: %s: screeninfo: %s\n", path, strerror(errno));
        close(fb->fd);
        return -1;
    }
    if (var.bits_per_pixel != 16 && var.bits_per_pixel != 32) {
        fprintf(stderr, "mixsplash: %u bpp is not supported (16 or 32)\n",
                var.bits_per_pixel);
        close(fb->fd);
        return -1;
    }

    fb->w = (int)var.xres;
    fb->h = (int)var.yres;
    fb->stride = fix.line_length ? fix.line_length
                                 : var.xres * (var.bits_per_pixel / 8);
    fb->bpp = var.bits_per_pixel;
    fb->r = var.red;
    fb->g = var.green;
    fb->b = var.blue;

    /* The visible window may be panned inside a taller virtual screen.  Map the
     * whole thing and remember where (0,0) actually is: mapping from the pan
     * offset instead would work until something else panned it. */
    fb->origin = (size_t)var.yoffset * fb->stride +
                 (size_t)var.xoffset * (fb->bpp / 8);
    fb->maplen = fix.smem_len ? fix.smem_len
                              : (size_t)var.yres_virtual * fb->stride;

    fb->mem = mmap(NULL, fb->maplen, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        fprintf(stderr, "mixsplash: mmap %s: %s\n", path, strerror(errno));
        close(fb->fd);
        return -1;
    }

    note("%s %dx%d %ubpp stride %u r%u/%u g%u/%u b%u/%u", path, fb->w, fb->h,
         fb->bpp, fb->stride, fb->r.offset, fb->r.length, fb->g.offset,
         fb->g.length, fb->b.offset, fb->b.length);
    return 0;
}

static inline uint32_t fb_pack(const struct fbdev *fb, uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return ((uint32_t)(r >> (8 - fb->r.length)) << fb->r.offset) |
           ((uint32_t)(g >> (8 - fb->g.length)) << fb->g.offset) |
           ((uint32_t)(b >> (8 - fb->b.length)) << fb->b.offset);
}

static void fb_blit(const struct fbdev *fb, const uint32_t *canvas,
                    int x, int y, int w, int h)
{
    int row, col;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb->w) w = fb->w - x;
    if (y + h > fb->h) h = fb->h - y;
    if (w <= 0 || h <= 0)
        return;

    for (row = 0; row < h; ++row) {
        const uint32_t *src = canvas + (size_t)(y + row) * fb->w + x;
        unsigned char *dst = fb->mem + fb->origin +
                             (size_t)(y + row) * fb->stride +
                             (size_t)x * (fb->bpp / 8);
        if (fb->bpp == 32) {
            uint32_t *d = (uint32_t *)dst;
            for (col = 0; col < w; ++col)
                d[col] = fb_pack(fb, src[col]);
        } else {
            uint16_t *d = (uint16_t *)dst;
            for (col = 0; col < w; ++col)
                d[col] = (uint16_t)fb_pack(fb, src[col]);
        }
    }
}

/* ── colour helpers ──────────────────────────────────────────────────────── */

static inline uint32_t add_rgb(uint32_t base, int ar, int ag, int ab)
{
    int r = (int)((base >> 16) & 0xff) + ar;
    int g = (int)((base >> 8) & 0xff) + ag;
    int b = (int)(base & 0xff) + ab;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t mix_rgb(uint32_t base, uint32_t over, int alpha)
{
    int br = (base >> 16) & 0xff, bg = (base >> 8) & 0xff, bb = base & 0xff;
    int orr = (over >> 16) & 0xff, og = (over >> 8) & 0xff, ob = over & 0xff;
    int r = br + (orr - br) * alpha / 255;
    int g = bg + (og - bg) * alpha / 255;
    int b = bb + (ob - bb) * alpha / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── text ────────────────────────────────────────────────────────────────── */

static int text_width(const char *s, int scale)
{
    int n = 0;
    while (*s++)
        ++n;
    return n ? n * 6 * scale - scale : 0;   /* 5 columns + 1 gap, less the last */
}

/*
 * TEXT IS DRAWN THROUGH A COVERAGE MASK, NOT STRAIGHT ONTO THE CANVAS.
 *
 * A 5x7 cell blown up to an integer scale is a grid of hard squares, and next to
 * the wordmark in the wallpaper -- which is a real typeface, anti-aliased --
 * that reads as a fault rather than as a style.  Supersampling cannot fix it:
 * every edge in the glyph is axis-aligned at an integer multiple of the scale,
 * so more samples land on the same two answers.
 *
 * What does fix it is a blur.  The glyph goes into a byte-per-pixel mask at the
 * final size, gets one pass of a 1-2-1 tent in each axis, and the result is
 * scaled up by 5/4 and clipped -- so the interior of a stroke stays fully
 * opaque and only the outermost pixel picks up a partial value.  One pixel of
 * softness, strokes that stay their proper weight.
 *
 * The mask carries a two-pixel margin so the blur and the drop shadow have
 * somewhere to land.  The shadow is not decoration either: this wallpaper runs
 * from near-black at the corners to a bright cyan behind the mark, and white on
 * that cyan is unreadable without something behind it.
 */
#define TEXT_MASK_W 800
#define TEXT_MASK_H 48
#define TEXT_MARGIN 2

static unsigned char g_mask[TEXT_MASK_W * TEXT_MASK_H];
static unsigned char g_blur[TEXT_MASK_W * TEXT_MASK_H];

static void mask_glyph(int mw, int mh, int x, unsigned char c, int scale)
{
    const unsigned char *glyph;
    int col, row, sx, sy;

    if (c < 0x20 || c > 0x7e)
        c = '?';
    glyph = FONT5X7[c - 0x20];

    for (col = 0; col < 5; ++col) {
        unsigned bits = glyph[col];
        for (row = 0; row < 7; ++row) {
            if (!(bits & (1u << row)))
                continue;
            for (sy = 0; sy < scale; ++sy) {
                int my = TEXT_MARGIN + row * scale + sy;
                if (my < 0 || my >= mh)
                    continue;
                for (sx = 0; sx < scale; ++sx) {
                    int mx = x + col * scale + sx;
                    if (mx < 0 || mx >= mw)
                        continue;
                    g_mask[(size_t)my * mw + mx] = 255;
                }
            }
        }
    }
}

static void mask_blur(int mw, int mh)
{
    int x, y;

    /* Horizontal, into g_blur. */
    for (y = 0; y < mh; ++y) {
        const unsigned char *s = g_mask + (size_t)y * mw;
        unsigned char *d = g_blur + (size_t)y * mw;
        for (x = 0; x < mw; ++x) {
            int l = x > 0 ? s[x - 1] : 0;
            int r = x + 1 < mw ? s[x + 1] : 0;
            d[x] = (unsigned char)((l + 2 * s[x] + r) / 4);
        }
    }
    /* Vertical, back into g_mask, with the 5/4 gain and the clip. */
    for (y = 0; y < mh; ++y) {
        unsigned char *d = g_mask + (size_t)y * mw;
        for (x = 0; x < mw; ++x) {
            int u = y > 0 ? g_blur[(size_t)(y - 1) * mw + x] : 0;
            int c = g_blur[(size_t)y * mw + x];
            int b = y + 1 < mh ? g_blur[(size_t)(y + 1) * mw + x] : 0;
            int v = (u + 2 * c + b) / 4;
            v = v * 5 / 4;
            d[x] = (unsigned char)(v > 255 ? 255 : v);
        }
    }
}

static void draw_text(uint32_t *canvas, int cw, int ch, int cx, int y,
                      const char *s, int scale, uint32_t colour, int alpha)
{
    int tw = text_width(s, scale);
    int mw = tw + 2 * TEXT_MARGIN + scale;   /* +scale for the shadow offset  */
    int mh = 7 * scale + 2 * TEXT_MARGIN + scale;
    int ox = cx - tw / 2 - TEXT_MARGIN;
    int oy = y - TEXT_MARGIN;
    int x, my;
    const char *p;

    if (mw > TEXT_MASK_W)
        mw = TEXT_MASK_W;
    if (mh > TEXT_MASK_H)
        mh = TEXT_MASK_H;
    if (mw <= 0 || mh <= 0)
        return;

    memset(g_mask, 0, (size_t)mw * mh);
    x = TEXT_MARGIN;
    for (p = s; *p; ++p) {
        mask_glyph(mw, mh, x, (unsigned char)*p, scale);
        x += 6 * scale;
    }
    mask_blur(mw, mh);

    /* The shadow, from the same mask offset down and right; then the glyph. */
    for (my = 0; my < mh; ++my) {
        int py = oy + my + scale;
        const unsigned char *row = g_mask + (size_t)my * mw;
        if (py < 0 || py >= ch)
            continue;
        for (x = 0; x < mw; ++x) {
            int cov = row[x];
            int px = ox + x + scale;
            if (!cov || px < 0 || px >= cw)
                continue;
            canvas[(size_t)py * cw + px] =
                mix_rgb(canvas[(size_t)py * cw + px], 0x000000,
                        cov * alpha / 255 * 55 / 100);
        }
    }
    for (my = 0; my < mh; ++my) {
        int py = oy + my;
        const unsigned char *row = g_mask + (size_t)my * mw;
        if (py < 0 || py >= ch)
            continue;
        for (x = 0; x < mw; ++x) {
            int cov = row[x];
            int px = ox + x;
            if (!cov || px < 0 || px >= cw)
                continue;
            canvas[(size_t)py * cw + px] =
                mix_rgb(canvas[(size_t)py * cw + px], colour, cov * alpha / 255);
        }
    }
}

/* The box draw_text() will touch, for the damage list.  It has to match what the
 * loop above actually writes -- mask plus the shadow's offset -- or a message
 * that shrinks leaves its own tail on the panel. */
static void text_bounds(int cx, int y, const char *s, int scale,
                        int *bx, int *by, int *bw, int *bh)
{
    int tw = text_width(s, scale);
    *bx = cx - tw / 2 - TEXT_MARGIN;
    *by = y - TEXT_MARGIN;
    *bw = tw + 2 * TEXT_MARGIN + 2 * scale;
    *bh = 7 * scale + 2 * TEXT_MARGIN + 2 * scale;
}

/* ── the picture ─────────────────────────────────────────────────────────── */

/*
 * Load the MIXSPL1 blob and fit it to the panel.
 *
 * Fit is "cover": scale up by whichever axis needs it more and crop the
 * overhang, so the wallpaper always fills the screen and never letterboxes.
 * Nearest neighbour, because at build time jpeg2raw.py has already been given
 * the chance to resize properly and this path only runs when the panel turned
 * out not to be the size the build expected.
 */
static uint32_t *load_image(const char *path, int dw, int dh)
{
    unsigned char hdr[MIXSPL_HDR];
    uint32_t *src, *dst;
    unsigned sw, sh, stride;
    ssize_t got;
    size_t want;
    int fd, y, x;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mixsplash: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        memcmp(hdr, MIXSPL_MAGIC, 8) != 0) {
        fprintf(stderr, "mixsplash: %s is not a MIXSPL1 blob\n", path);
        close(fd);
        return NULL;
    }
    sw     = (unsigned)hdr[8]  | ((unsigned)hdr[9] << 8)  |
             ((unsigned)hdr[10] << 16) | ((unsigned)hdr[11] << 24);
    sh     = (unsigned)hdr[12] | ((unsigned)hdr[13] << 8) |
             ((unsigned)hdr[14] << 16) | ((unsigned)hdr[15] << 24);
    stride = (unsigned)hdr[16] | ((unsigned)hdr[17] << 8) |
             ((unsigned)hdr[18] << 16) | ((unsigned)hdr[19] << 24);

    if (!sw || !sh || stride != sw * 4 || sw > 8192 || sh > 8192) {
        fprintf(stderr, "mixsplash: %s has an implausible geometry %ux%u\n",
                path, sw, sh);
        close(fd);
        return NULL;
    }

    want = (size_t)sw * sh * 4;
    src = malloc(want);
    if (!src) {
        close(fd);
        return NULL;
    }
    {
        size_t done = 0;
        while (done < want) {
            got = read(fd, (unsigned char *)src + done, want - done);
            if (got <= 0)
                break;
            done += (size_t)got;
        }
        if (done != want) {
            fprintf(stderr, "mixsplash: %s is short (%zu of %zu bytes)\n",
                    path, done, want);
            free(src);
            close(fd);
            return NULL;
        }
    }
    close(fd);

    if ((int)sw == dw && (int)sh == dh)
        return src;

    dst = malloc((size_t)dw * dh * 4);
    if (!dst) {
        free(src);
        return NULL;
    }
    {
        /* Cover: one scale for both axes, the larger of the two ratios, in
         * 16.16 fixed point so the inner loop stays integer. */
        int32_t sx16 = (int32_t)(((int64_t)sw << 16) / dw);
        int32_t sy16 = (int32_t)(((int64_t)sh << 16) / dh);
        int32_t s16 = sx16 < sy16 ? sx16 : sy16;
        int32_t ox = ((int32_t)sw << 16) - s16 * dw;
        int32_t oy = ((int32_t)sh << 16) - s16 * dh;

        for (y = 0; y < dh; ++y) {
            int32_t syy = (oy / 2 + s16 * y) >> 16;
            const uint32_t *srow;
            if (syy < 0) syy = 0;
            if (syy >= (int)sh) syy = (int)sh - 1;
            srow = src + (size_t)syy * sw;
            for (x = 0; x < dw; ++x) {
                int32_t sxx = (ox / 2 + s16 * x) >> 16;
                if (sxx < 0) sxx = 0;
                if (sxx >= (int)sw) sxx = (int)sw - 1;
                dst[(size_t)y * dw + x] = srow[sxx];
            }
        }
    }
    free(src);
    note("wallpaper %ux%u fitted to %dx%d", sw, sh, dw, dh);
    return dst;
}

/* ── the effects ─────────────────────────────────────────────────────────── */

/*
 * THE SPECULAR SWEEP.
 *
 * A soft diagonal band of light that crosses the wallpaper every few seconds --
 * the shine that runs across a Windows lock screen.  It is additive and tinted
 * toward blue, so it reads as the panel catching a light rather than as a grey
 * wash: the wallpaper's own highlight is #2f7fd8-ish, and adding equal parts of
 * r, g and b to that would desaturate it toward white.
 *
 * The band is vertical-ish rather than horizontal because the picture's own
 * geometry -- the ghosted panes behind the wordmark -- runs that way.
 */
#define SWEEP_PERIOD   7.0    /* seconds between sweeps                       */
#define SWEEP_TRAVEL   2.4    /* seconds the band takes to cross              */
#define SWEEP_HALF     96     /* half-width of the band, in pixels            */
#define SWEEP_SLANT    56     /* x shift from the top of the screen to the foot */
#define SWEEP_PEAK     40     /* peak added blue                              */

/*
 * Where the band is this frame, clipped to the screen.  Returns 0 when the
 * sweep is between passes and there is nothing to draw -- which is most of the
 * time, and is why this is a separate function: the caller needs the same answer
 * to know what it has to restore from the wallpaper, and computing it twice from
 * two slightly different expressions is how a smear gets left behind.
 */
static int sweep_band(int w, double t, int *x0, int *x1)
{
    double phase = t - SWEEP_PERIOD * (double)(long)(t / SWEEP_PERIOD);
    double travel;
    int centre, lo, hi;

    if (phase > SWEEP_TRAVEL)
        return 0;

    travel = phase / SWEEP_TRAVEL;
    /* Ease in and out, so the band does not appear and vanish at the edges. */
    travel = travel * travel * (3.0 - 2.0 * travel);

    centre = (int)(travel * (double)(w + 2 * SWEEP_HALF + SWEEP_SLANT))
             - SWEEP_HALF - SWEEP_SLANT;
    lo = centre - SWEEP_HALF;
    hi = centre + SWEEP_HALF + SWEEP_SLANT;
    if (lo < 0) lo = 0;
    if (hi > w) hi = w;
    if (lo >= hi)
        return 0;

    *x0 = lo;
    *x1 = hi;
    return 1;
}

static void draw_sweep(uint32_t *canvas, int w, int h, double t)
{
    int x0, x1, x, y;

    if (!sweep_band(w, t, &x0, &x1))
        return;

    for (y = 0; y < h; ++y) {
        /* The band leans: its centre moves right as y increases. */
        int centre = (x0 + x1) / 2 - SWEEP_SLANT / 2 +
                     (int)((long)SWEEP_SLANT * y / (h ? h : 1));
        int lo = centre - SWEEP_HALF, hi = centre + SWEEP_HALF;
        uint32_t *crow = canvas + (size_t)y * w;

        if (lo < 0) lo = 0;
        if (hi > w) hi = w;
        for (x = lo; x < hi; ++x) {
            int d = x - centre;
            /* Squared falloff.  A linear one puts a visible crease down the
             * middle of the band on a gradient this smooth. */
            int a = SWEEP_HALF - (d < 0 ? -d : d);
            a = SWEEP_PEAK * a * a / (SWEEP_HALF * SWEEP_HALF);
            if (a <= 0)
                continue;
            crow[x] = add_rgb(crow[x], a * 40 / 100, a * 62 / 100, a);
        }
    }
    damage(x0, 0, x1 - x0, h);
}

/*
 * THE BREATH BEHIND THE MARK.
 *
 * A radial glow centred on the four-pane logo, rising and falling on a five
 * second sine.  Quadratic falloff, computed on the square of the distance so
 * there is no sqrt in the inner loop -- 200x200 pixels every frame is 40k
 * square roots a frame otherwise, on a core with no hardware divider to spare.
 */
#define GLOW_RADIUS  105
#define GLOW_PEAK    26
#define GLOW_PERIOD  5.0

static void draw_glow(uint32_t *canvas, int w, int h, double t)
{
    int cx = (int)(LOGO_CX_F * w), cy = (int)(LOGO_CY_F * h);
    int x0 = cx - GLOW_RADIUS, y0 = cy - GLOW_RADIUS;
    int x1 = cx + GLOW_RADIUS, y1 = cy + GLOW_RADIUS;
    double s = 0.5 - 0.5 * sp_cos(6.283185307 * t / GLOW_PERIOD);
    int peak = (int)(GLOW_PEAK * (0.35 + 0.65 * s));
    int r2 = GLOW_RADIUS * GLOW_RADIUS;
    int x, y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    for (y = y0; y < y1; ++y) {
        int dy = y - cy, dy2 = dy * dy;
        uint32_t *crow = canvas + (size_t)y * w;
        for (x = x0; x < x1; ++x) {
            int dx = x - cx;
            int d2 = dx * dx + dy2;
            int a;
            if (d2 >= r2)
                continue;
            a = peak * (r2 - d2) / r2;
            a = a * (r2 - d2) / r2;     /* squared falloff: a soft edge       */
            if (a <= 0)
                continue;
            crow[x] = add_rgb(crow[x], a * 55 / 100, a * 78 / 100, a);
        }
    }
    damage(x0, y0, x1 - x0, y1 - y0);
}

/*
 * THE RING OF DOTS.
 *
 * Windows' boot spinner: five dots chasing each other around a circle, each
 * one accelerating through the top of the orbit and slowing at the bottom, with
 * a fixed stagger between them.  The easing is what makes it recognisable -- at
 * constant angular speed it reads as a loading throbber from any other system.
 *
 * Drawn with a 3x3 box of coverage samples per pixel rather than a real
 * anti-aliased circle: at radius 4 on a 640x480 panel that is indistinguishable
 * from the real thing and it is nine integer compares instead of a sqrt.
 */
#define SPIN_DOTS    6
#define SPIN_RADIUS  26
#define SPIN_DOT     3
#define SPIN_PERIOD  2.0      /* seconds for one dot to go all the way round  */
#define SPIN_STAGGER 0.10     /* seconds between one dot and the next         */
#define SPIN_SWING   0.75     /* how much faster through the top than the foot */

/*
 * u in [0,1) -> [0,1), monotone, fast through the top of the orbit and slow
 * through the foot of it.
 *
 *     ease(u) = u + k*sin(2*pi*u)/(2*pi)   =>   ease'(u) = 1 + k*cos(2*pi*u)
 *
 * so the dot runs at (1+k) times the mean rate as it crosses the top and (1-k)
 * at the bottom.  k < 1 keeps it monotone, which is what stops the ring ever
 * appearing to run backwards -- the failure mode of every hand-tuned pair of
 * smoothsteps, and the reason this is one closed form instead.
 */
static double spin_ease(double u)
{
    return u + SPIN_SWING * sp_sin(6.283185307179586 * u) / 6.283185307179586;
}

static void draw_spinner(uint32_t *canvas, int w, int h, double t)
{
    int cx = w / 2, cy = (int)(SPINNER_CY_F * h);
    int box = SPIN_RADIUS + SPIN_DOT + 2;
    int x0 = cx - box, y0 = cy - box, x1 = cx + box, y1 = cy + box;
    int i, x, y;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    for (i = 0; i < SPIN_DOTS; ++i) {
        double u = (t - i * SPIN_STAGGER) / SPIN_PERIOD;
        double ang, dx, dy;
        int px, py;

        u -= (double)(long)u;
        if (u < 0.0)
            u += 1.0;
        /* -pi/2 puts the start of the orbit at the top of the circle. */
        ang = spin_ease(u) * 6.283185307 - 1.570796327;
        dx = sp_cos(ang) * SPIN_RADIUS;
        dy = sp_sin(ang) * SPIN_RADIUS;
        px = cx + (int)(dx < 0 ? dx - 0.5 : dx + 0.5);
        py = cy + (int)(dy < 0 ? dy - 0.5 : dy + 0.5);

        for (y = py - SPIN_DOT - 1; y <= py + SPIN_DOT + 1; ++y) {
            if (y < y0 || y >= y1)
                continue;
            for (x = px - SPIN_DOT - 1; x <= px + SPIN_DOT + 1; ++x) {
                int ddx, ddy, d2, alpha;
                if (x < x0 || x >= x1)
                    continue;
                ddx = x - px;
                ddy = y - py;
                d2 = ddx * ddx + ddy * ddy;
                if (d2 > (SPIN_DOT + 1) * (SPIN_DOT + 1))
                    continue;
                alpha = d2 <= SPIN_DOT * SPIN_DOT
                            ? 255
                            : 255 - 255 * (d2 - SPIN_DOT * SPIN_DOT) /
                                        ((SPIN_DOT + 1) * (SPIN_DOT + 1) -
                                         SPIN_DOT * SPIN_DOT);
                canvas[(size_t)y * w + x] =
                    mix_rgb(canvas[(size_t)y * w + x], 0xffffff, alpha);
            }
        }
    }
    damage(x0, y0, x1 - x0, y1 - y0);
}

/*
 * THE BAR.
 *
 * Determinate, because the point of the exercise is to say WHERE the boot is,
 * and a barber's pole says only that something is happening.  /init sets it at
 * each stage; the fill eases toward the target rather than jumping, which is
 * the difference between a progress bar and a set of flash cards.
 *
 * Accent #0078D4 -- Microsoft's own -- over a track that is the wallpaper
 * darkened rather than a flat grey, so the bar sits in the picture.
 */
#define BAR_H 4

static void draw_bar(uint32_t *canvas, int w, int h, double shown, double t)
{
    int bw = (int)(BAR_W_F * w);
    int x0 = (w - bw) / 2, y0 = (int)(BAR_Y_F * h);
    int fill = (int)(bw * (shown < 0 ? 0 : shown > 1 ? 1 : shown));
    int x, y;
    /* A highlight that travels along the filled part: alive, but bounded by the
     * fill, so it can never imply progress the boot has not made. */
    int glint = fill > 24 ? (int)((t - (double)(long)t) * fill) : -1;

    for (y = y0; y < y0 + BAR_H && y < h; ++y) {
        uint32_t *crow = canvas + (size_t)y * w;
        for (x = x0; x < x0 + bw && x < w; ++x) {
            if (x - x0 < fill) {
                uint32_t c = mix_rgb(crow[x], 0x0078D4, 235);
                if (glint >= 0) {
                    int d = (x - x0) - glint;
                    if (d < 0) d = -d;
                    if (d < 18)
                        c = add_rgb(c, (18 - d) * 4, (18 - d) * 5, (18 - d) * 5);
                }
                crow[x] = c;
            } else {
                crow[x] = mix_rgb(crow[x], 0x000000, 96);
            }
        }
    }
    damage(x0, y0, bw, BAR_H);
}

/* ── the message channel ─────────────────────────────────────────────────── */

/*
 * A PLAIN APPENDED-TO FILE, AND NOT THE FIFO THIS STARTED AS.
 *
 * The obvious channel for this is a named pipe, and the first version was one.
 * It is the wrong choice here for a reason that has nothing to do with elegance:
 * the writer is /init, a BusyBox ash script, and `echo x > fifo' BLOCKS UNTIL
 * SOMETHING READS IT.  So the moment the splash is not running -- it failed to
 * find /dev/fb0, it was killed, the board has no panel -- the next stage message
 * in /init wedges the boot forever, on a device whose only recovery is taking
 * the card out.  A splash that can hang the boot it is decorating is not worth
 * having.  (The initramfs BusyBox has no `mkfifo' applet either, so the shell
 * side could not create one without `mknod p'.)
 *
 * Appending to a regular file in devtmpfs cannot block, cannot fail for want of
 * a reader, and needs nothing of the writer but `>>'.  Reading it is just as
 * simple: an fd keeps its own offset, so read() returning 0 means "nothing new
 * yet" and the next read() after an append picks up exactly the new bytes.
 *
 * A path that already exists as a FIFO is still honoured -- opened O_RDWR so it
 * never sees EOF between writers -- because a caller who knows what they are
 * doing may want one.  /init does not.
 */
struct msgs {
    int fd;
    int isfifo;
    off_t pos;          /* what we have consumed, for the shrink check          */
    char buf[512];
    size_t len;
};

static int msgs_open(struct msgs *m, const char *path)
{
    struct stat st;

    m->len = 0;
    m->fd = -1;
    m->isfifo = 0;
    m->pos = 0;
    if (!path)
        return 0;

    if (stat(path, &st) == 0 && S_ISFIFO(st.st_mode))
        m->isfifo = 1;

    m->fd = m->isfifo ? open(path, O_RDWR | O_NONBLOCK)
                      : open(path, O_RDONLY | O_CREAT | O_NONBLOCK, 0644);
    if (m->fd < 0) {
        fprintf(stderr, "mixsplash: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Copy a message into the fixed slot it is drawn from, truncating if it does not
 * fit.
 *
 * snprintf(dst, sizeof dst, "%s", src) did this, and did it correctly, until GCC
 * started pointing out that a 512-byte line cannot fit a 96-byte slot.  It is
 * right, and the truncation is the intent: `stage' is one centred line under a
 * spinner on a 640-pixel panel, so anything past the first few dozen characters
 * was never going to be drawn whatever this function did with it.  Saying so with
 * a copy that stops on purpose is clearer than a format string that stops by
 * accident -- and it keeps printf's parser off a path that runs once per message
 * on a machine that is still in early boot.
 */
static void set_text(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);

    if (n > dstsz - 1)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Pull whatever is waiting and hand back complete lines one at a time.  Returns
 * 1 and fills `line' when there was one, 0 when there was not. */
static int msgs_next(struct msgs *m, char *line, size_t linesz)
{
    /*
     * ── READ INTO A WHOLE OBJECT, THEN APPEND BY HAND ──
     *
     * The obvious shape for this is read(fd, m->buf + m->len, sizeof(m->buf) -
     * m->len), and that is what it was, and glibc's fortified read() would not
     * have it: "writing 528 or more bytes into a region of size 512".  The
     * complaint is not about a real code path.  __builtin_object_size() of
     * `m->buf + m->len' with a non-constant len is the whole 512-byte member, and
     * the count is a subtraction GCC will not correlate with the offset it was
     * derived from -- so it pairs the largest count it can imagine with the
     * smallest region and reports the overlap.
     *
     * Two attempts at arguing with it failed, and both failed for the same
     * reason: `>=' instead of `==' on the full check, and then a saturating
     * assignment to m->len, are both statements about the OFFSET, and the offset
     * was never what it could not prove.  So the argument is dropped instead.
     * read() is given `chunk' -- a complete object with a constant size, which
     * leaves fortify nothing to be unsure about -- and the bytes are appended
     * with an index that is compared against the destination's size on every
     * single store.  There is no expression left for anyone to mis-range.
     *
     * It costs a copy of at most half a kilobyte per message on a path that runs
     * a few dozen times in a boot.  A build with no warnings in it is worth more
     * than that, because the next warning here will be a real one.
     */
    char chunk[512];
    char *nl;
    ssize_t got;
    size_t i;

    for (;;) {
        nl = memchr(m->buf, '\n', m->len);
        if (nl) {
            size_t n = (size_t)(nl - m->buf);
            size_t copy = n < linesz - 1 ? n : linesz - 1;
            memcpy(line, m->buf, copy);
            line[copy] = '\0';
            memmove(m->buf, nl + 1, m->len - n - 1);
            m->len -= n + 1;
            return 1;
        }
        if (m->fd < 0)
            return 0;
        /*
         * A writer with no newline in half a kilobyte.  Take what is there rather
         * than wedging: the alternative is a splash that stops updating because
         * somebody echoed -n.  The clamp is against sizeof(m->buf) and not against
         * m->len because what limits the copy is the buffer, not the count.
         */
        if (m->len >= sizeof(m->buf)) {
            size_t copy = sizeof(m->buf) < linesz - 1 ? sizeof(m->buf) : linesz - 1;

            memcpy(line, m->buf, copy);
            line[copy] = '\0';
            m->len = 0;
            return 1;
        }
        /* Somebody truncated it -- `>' instead of `>>' -- so everything after
         * our offset is a different file's worth of bytes.  Start again rather
         * than sitting past the end of a file that is now shorter than we are. */
        if (!m->isfifo) {
            struct stat st;
            if (fstat(m->fd, &st) == 0 && st.st_size < m->pos) {
                lseek(m->fd, 0, SEEK_SET);
                m->pos = 0;
                m->len = 0;
            }
        }
        /* Never more than there is room for, so nothing read is ever dropped --
         * and never more than `chunk' holds, which is the clamp against a
         * constant that lets fortify check the call instead of guessing at it. */
        space = sizeof(m->buf) - m->len;
        if (space > sizeof chunk)
            space = sizeof chunk;
        got = read(m->fd, chunk, space);
        if (got <= 0)
            return 0;
        m->pos += got;
        for (i = 0; i < (size_t)got && m->len < sizeof(m->buf); ++i)
            m->buf[m->len++] = chunk[i];
    }
}

/* ── the console ─────────────────────────────────────────────────────────── */

/*
 * Take the console away from fbcon.
 *
 * KD_GRAPHICS is a promise that userspace is driving the display; the console
 * driver stops painting and stops clearing.  Everything already in the
 * framebuffer stays exactly as it is, which is why this can be done AFTER the
 * wallpaper has been drawn -- and it is, so that a failure here costs the
 * animation and not the picture.
 *
 * /dev/tty0 is the ACTIVE virtual terminal, whichever that is, and is the right
 * handle for this; /dev/tty1 is the fallback for a kernel or an initramfs that
 * only made the numbered nodes.  Neither existing is not an error: a kernel with
 * CONFIG_VT off has no fbcon to fight in the first place.
 */
static int console_grab(void)
{
    static const char *const nodes[] = { "/dev/tty0", "/dev/tty1", NULL };
    int i;

    for (i = 0; nodes[i]; ++i) {
        int fd = open(nodes[i], O_RDWR | O_NOCTTY);
        if (fd < 0)
            continue;
        if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == 0) {
            note("console %s is in graphics mode", nodes[i]);
            return fd;   /* held open: the mode is undone when this fd closes
                          * on some kernels, so it must outlive the splash */
        }
        close(fd);
    }
    note("no VT to grab; fbcon may repaint over this");
    return -1;
}

static void console_release(int fd, int keep_graphics)
{
    if (fd < 0)
        return;
    if (!keep_graphics)
        ioctl(fd, KDSETMODE, KD_TEXT);
    close(fd);
}

/* ── main ────────────────────────────────────────────────────────────────── */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: mixsplash [options]\n"
        "  -i FILE   the MIXSPL1 wallpaper (default /splash.mixspl)\n"
        "  -f FILE   message channel, appended to by /init (default\n"
        "            /dev/.mixsplash); an existing FIFO at that path works too\n"
        "  -d NODE   framebuffer (default /dev/fb0)\n"
        "  -s TEXT   the first stage line\n"
        "  -t SECS   give up if nothing arrives and no hand-over (default 90)\n"
        "  -T SECS   give up this long after hand-over (default 180, 0 = never)\n"
        "  -1        draw one frame and exit (no animation, no channel)\n"
        "  -k        leave the console in graphics mode on exit, always\n"
        "  -v        say what it is doing, on stderr\n");
}

int main(int argc, char **argv)
{
    const char *imgpath = "/splash.mixspl";
    const char *chanpath = "/dev/.mixsplash";
    const char *fbpath = "/dev/fb0";
    const char *first_stage = "Starting MixOS";
    double idle_timeout = 90.0;
    double handover_timeout = 180.0;
    int oneshot = 0, keep_graphics = 0;

    struct fbdev fb;
    struct msgs msgs;
    uint32_t *img, *canvas;
    char stage[96], detail[96], line[512];
    double target = 0.0, shown = 0.0;
    double t0, last_msg, handover_at = -1.0;
    int console = -1, handover = 0, opt;

    while ((opt = getopt(argc, argv, "i:f:d:s:t:T:1kvh")) != -1) {
        switch (opt) {
        case 'i': imgpath = optarg; break;
        case 'f': chanpath = optarg; break;
        case 'd': fbpath = optarg; break;
        case 's': first_stage = optarg; break;
        case 't': idle_timeout = atof(optarg); break;
        case 'T': handover_timeout = atof(optarg); break;
        case '1': oneshot = 1; break;
        case 'k': keep_graphics = 1; break;
        case 'v': g_verbose = 1; break;
        default: usage(); return opt == 'h' ? 0 : 2;
        }
    }

    /*
     * ── THE ANIMATION IS THE LOWEST-VALUE WORK ON THE MACHINE AND IT NEEDS THE
     *    CPU ANYWAY ──
     *
     * Nothing here is urgent in the sense that matters to a scheduler: a dropped
     * frame costs nothing.  But a spinner that stops is the one thing on this
     * panel that a person reads as "it has crashed", so the frames have to keep
     * landing while /init does the heavy part of the boot -- and /init's heavy
     * part is a fork of BusyBox per mount, per ls, per probe, all of them at the
     * same priority as this.
     *
     * -10 is a nudge and not a real-time class on purpose.  It buys a share of a
     * loaded run queue without ever being able to hold the CPU off the work the
     * splash exists to describe, which a SCHED_FIFO process on a single-core
     * early boot absolutely can.  Failing is fine and is not reported: without
     * CAP_SYS_NICE this simply keeps the priority it was given.
     */
    setpriority(PRIO_PROCESS, 0, -10);

    if (fb_open(&fb, fbpath) < 0)
        return 1;

    img = load_image(imgpath, fb.w, fb.h);
    if (!img)
        return 1;

    canvas = malloc((size_t)fb.w * fb.h * 4);
    if (!canvas) {
        fprintf(stderr, "mixsplash: out of memory for a %dx%d canvas\n",
                fb.w, fb.h);
        return 1;
    }
    memcpy(canvas, img, (size_t)fb.w * fb.h * 4);

    /* The wallpaper first, THEN the console grab: if KDSETMODE fails, or if
     * there is no VT at all, the picture is already up and the worst case is
     * fbcon eventually drawing over it. */
    fb_blit(&fb, canvas, 0, 0, fb.w, fb.h);
    if (oneshot)
        return 0;

    console = console_grab();

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_usr1);

    if (msgs_open(&msgs, chanpath) < 0) {
        /* Not fatal.  Without a channel this is still a splash; it just has
         * nothing new to say, which beats a text console. */
        msgs.fd = -1;
    }

    set_text(stage, sizeof(stage), first_stage);
    detail[0] = '\0';
    t0 = now_seconds();
    last_msg = t0;

    while (!g_quit) {
        double t = now_seconds() - t0;
        int i;

        while (msgs_next(&msgs, line, sizeof(line))) {
            last_msg = now_seconds();
            if (!strncmp(line, "stage:", 6)) {
                set_text(stage, sizeof(stage), line + 6);
                detail[0] = '\0';
            } else if (!strncmp(line, "detail:", 7)) {
                set_text(detail, sizeof(detail), line + 7);
            } else if (!strncmp(line, "progress:", 9)) {
                int pc = atoi(line + 9);
                target = (pc < 0 ? 0 : pc > 100 ? 100 : pc) / 100.0;
            } else if (!strcmp(line, "handover")) {
                handover = 1;
                handover_at = now_seconds();
            } else if (!strcmp(line, "quit")) {
                g_quit = 1;
            } else if (!strcmp(line, "abort")) {
                /*
                 * `quit' and `abort' differ in exactly one thing and it matters
                 * on exactly one path.  /init sends `handover' immediately
                 * before switch_root -- and switch_root can fail, at which point
                 * /init falls through to a post-mortem it prints on the console
                 * that this process is still holding in graphics mode.  A `quit'
                 * there would honour the hand-over and keep it, and the panel
                 * would sit on a picture with the explanation invisible behind
                 * it.  `abort' means "stop, and give the text console back
                 * whatever you were told earlier".
                 */
                handover = 0;
                g_quit = 1;
            } else if (line[0]) {
                set_text(stage, sizeof(stage), line);
                detail[0] = '\0';
            }
            note("msg %s", line);
        }

        if (g_winch) {
            g_winch = 0;
            g_prev_all = 1;
        }

        /* Ease the bar toward the target: a tenth of the remaining distance per
         * frame, which at 25 fps closes a jump in under half a second. */
        shown += (target - shown) * 0.10;
        if (target - shown < 0.002 && target - shown > -0.002)
            shown = target;

        restore_damage(canvas, img, fb.w, fb.h);

        draw_sweep(canvas, fb.w, fb.h, t);
        draw_glow(canvas, fb.w, fb.h, t);
        draw_spinner(canvas, fb.w, fb.h, t);
        draw_bar(canvas, fb.w, fb.h, shown, t);

        {
            int sy = (int)(STAGE_Y_F * fb.h), dy = (int)(DETAIL_Y_F * fb.h);
            int bx, by, bw, bh;

            draw_text(canvas, fb.w, fb.h, fb.w / 2, sy, stage, 2, 0xffffff, 255);
            text_bounds(fb.w / 2, sy, stage, 2, &bx, &by, &bw, &bh);
            damage(bx, by, bw, bh);

            if (detail[0]) {
                draw_text(canvas, fb.w, fb.h, fb.w / 2, dy, detail, 1,
                          0xbfd8f0, 220);
                text_bounds(fb.w / 2, dy, detail, 1, &bx, &by, &bw, &bh);
                damage(bx, by, bw, bh);
            }
        }

        keep_damage();

        if (g_damage_all) {
            fb_blit(&fb, canvas, 0, 0, fb.w, fb.h);
        } else {
            for (i = 0; i < g_ndamage; ++i)
                fb_blit(&fb, canvas, g_damage[i].x, g_damage[i].y,
                        g_damage[i].w, g_damage[i].h);
        }

        /*
         * The two ways this gives up on its own.
         *
         * Before hand-over: /init is still running and has gone quiet for a long
         * time, which means it has stopped somewhere it did not expect to.  Put
         * the text console back so whoever is looking at the panel can read what
         * it says.  This is the safety net that makes it reasonable to take the
         * console away in the first place.
         *
         * After hand-over: the rootfs took over and mixdash never arrived to
         * kill us.  Same reasoning, longer fuse, and it can be switched off
         * entirely with -T 0 for a board that boots to something else.
         */
        if (!handover && idle_timeout > 0 &&
            now_seconds() - last_msg > idle_timeout) {
            note("no message in %.0fs and no hand-over; giving the console back",
                 idle_timeout);
            break;
        }
        if (handover && handover_timeout > 0 &&
            now_seconds() - handover_at > handover_timeout) {
            note("%.0fs since hand-over and nothing replaced us",
                 handover_timeout);
            break;
        }

        {
            struct timespec ts = { 0, 40 * 1000 * 1000 };   /* 25 fps */
            nanosleep(&ts, NULL);
        }
    }

    /*
     * Whether the console goes back to text is the whole question of what
     * happens next.  A hand-over means mixdash is coming and wants the screen
     * left alone -- Qt's linuxfb plugin is started with nographicsmodeswitch
     * precisely so that it does not do this itself.  No hand-over means
     * something went wrong, and a text console is the only diagnostic this
     * board has that does not need a serial cable.
     */
    console_release(console, keep_graphics || handover);
    note("exit (handover=%d)", handover);
    return 0;
}
