/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * j36-mixmirror.c -- put whatever is on the panel onto a USB-HDMI adapter too.
 *
 * WHY THIS EXISTS, which is the same as saying why the obvious thing does not work.
 *
 * Plug a USB-C hub or a DisplayLink dongle into this board and the kernel does bind
 * it: DRM_UDL is =m, j36/usb/load.order carries udl.ko, and run_usb insmods it, so a
 * /dev/dri/cardN appears with a connected connector and a mode list read off the
 * monitor's EDID.  And nothing shows up on the screen.  The reason is one Kconfig
 * symbol:
 *
 *     CONFIG_DRM_FBDEV_EMULATION=n
 *
 * which is deliberate and has to stay that way.  It is a GLOBAL bool, not a per-driver
 * one: turning it on for udl's sake would also make mtk_drm register an fbdev, and
 * /dev/fb0 -- the one mixdash draws into, the one simplefb owns because the LK already
 * left a working framebuffer there -- would stop being the thing this board boots with.
 * The note at the DRM prune in build-in-vm.sh says the same in more detail.
 *
 * So udl gives a card node with no /dev/fb beside it, and mixdash is Qt Widgets on the
 * linuxfb platform plugin, which knows how to open /dev/fb0 and nothing else.  The two
 * halves cannot see each other.  Neither one is broken.
 *
 * THE THREE WAYS OUT, and why this is the one.
 *
 *   1. Turn on fbdev emulation.  Costs the panel, per the above.  No.
 *   2. Run the dashboard on EGLFS-KMS against the udl node.  That is a real answer and
 *      it is also a much bigger one: it needs GBM, a working EGL on a device that has
 *      no GPU behind it (so llvmpipe, on a Cortex-A7), and it MOVES the dashboard --
 *      the handheld's own screen goes dark, which is exactly wrong for a device you
 *      dock and undock.
 *   3. Copy the pixels.  /dev/fb0 is memory; a dumb buffer on the udl node is memory;
 *      a loop between them is about two hundred lines and costs the panel nothing.
 *
 * This is 3.  The board keeps its screen, the dock gets the same picture, and unplug
 * is a process that goes back to sleep rather than a display server that loses its
 * output.  It is a MIRROR and it is not a compositor: there is no second desktop here,
 * no extended mode, no input routing.  One picture, two screens.
 *
 * ── the safety property, which is the whole reason to read this file ─────────────
 *
 * This program modesets.  On a board whose panel is brought up by the LK and held by
 * simplefb, a modeset aimed at the wrong node is not a bug you find in a log -- it is
 * a black screen with no way back.  So the node is never guessed and never taken from
 * the command line by default:
 *
 *     DRM_IOCTL_VERSION is asked for the driver's NAME, and unless that name is on the
 *     allow-list -- "udl", and nothing else without -n -- the node is closed again
 *     without another ioctl being sent to it.
 *
 * That is a stronger test than "card1 and not card0".  Minor numbers here are assigned
 * in probe order and this board has up to three DRM drivers in play (lima, mediatek,
 * udl), so the numbering moves between boots and between builds.  The name does not.
 * lima is refused, mediatek is refused, and a node that answers no version ioctl at
 * all is refused.
 *
 * ── hotplug, without udev ────────────────────────────────────────────────────────
 *
 * There is no udev rule and no netlink socket.  This process polls: every couple of
 * seconds it reads /dev/dri, and when a udl node turns up it starts mirroring; when
 * the ioctls start answering ENODEV it stops and goes back to reading /dev/dri.  A
 * readdir of a five-entry directory every two seconds is not a measurable cost on this
 * SoC, and what it buys is that the feature works identically from the initramfs, from
 * a rescue shell and under systemd, with no rule file to get wrong.
 *
 * The connector is re-read on the same tick for the same reason: a hub is very often
 * plugged in before the TV at the other end of it, and "adapter present, nothing
 * connected" has to become "connected" without the user unplugging anything.
 *
 * ── what it costs on the wire ────────────────────────────────────────────────────
 *
 * USB 2.0 bulk is about 30 MB/s in practice and 1280x960 at 32bpp is 4.7 MB, so a full
 * frame every time would be six or seven frames a second and a bus with no room left
 * for the mouse.  It does not send full frames.  The source is diffed against a shadow
 * copy in 64x64 tiles and only the tiles that changed are copied and handed to
 * DRM_IOCTL_MODE_DIRTYFB, which is the ioctl udl actually pushes on -- a dumb buffer
 * written to and never marked dirty produces nothing on the monitor at all.  A
 * dashboard sitting still therefore costs one diff pass and no USB traffic whatsoever.
 *
 * Scaling is integer and centred, never fractional.  640x480 doubled into 1280x960 in
 * the middle of a 1280x1024 mode is sharp; 640x480 stretched to 1920x1080 by a
 * nearest-neighbour loop is a mess of uneven pixel widths, and this board has no
 * cycles to spare for a filtered one.  The mode is chosen to maximise that integer
 * factor and, among modes that tie, to be the smallest -- biggest picture, fewest
 * bytes.
 *
 * Build: it is C99 with no libraries beyond libc.  DRM is spoken to with raw ioctls
 * rather than libdrm, for the reason j36-eglprobe.c gives at its own DRM section --
 * the uapi structures are ABI, drm_ioctl() tolerates a caller whose struct is shorter
 * than the kernel's, and a fourth shared library that can be absent from the rootfs is
 * a fourth way for this to fail on the board rather than in the build.
 *
 *     arm-linux-gnueabihf-gcc -O2 -std=gnu11 -static -o j36-mixmirror j36-mixmirror.c
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ── uapi, by hand ──────────────────────────────────────────────────────────────
 *
 * Everything below is copied from the kernel's include/uapi/drm/drm_mode.h and
 * include/uapi/linux/fb.h.  Same reasoning as j36-eglprobe.c: these are ABI and the
 * cross sysroot's headers are a package that can be missing from a build that
 * otherwise needs no headers at all.
 */

#define J36_IOC(dir, nr, sz) (((unsigned)(dir) << 30) | ('d' << 8) | \
                              (unsigned)(nr) | ((unsigned)(sz) << 16))
#define J36_IO(nr)           J36_IOC(0u, nr, 0)
#define J36_IOWR(nr, type)   J36_IOC(3u, nr, sizeof(type))

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

/* The damage rectangle.  x2 and y2 are EXCLUSIVE -- udl_handle_damage() takes the
 * width as clip->x2 - clip->x1 -- and they are unsigned short, so a mode wider than
 * 65535 pixels would wrap.  Nothing that plugs into USB 2.0 is. */
struct drm_clip_rect {
    unsigned short x1, y1, x2, y2;
};

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id, flags, color, num_clips;
    uint64_t clips_ptr;
};

/* The lengths are __kernel_size_t -- unsigned long, 32 bits here -- and the kernel's
 * drm_copy_field() copies min(strlen, len) bytes and does NOT terminate, so the
 * buffers below are zeroed first and asked for one byte less than they have. */
struct drm_version {
    int           version_major, version_minor, version_patchlevel;
    unsigned long name_len;
    char         *name;
    unsigned long date_len;
    char         *date;
    unsigned long desc_len;
    char         *desc;
};

#define DRM_IOCTL_VERSION           J36_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_SET_MASTER        J36_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER       J36_IO(0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES J36_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_SETCRTC      J36_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   J36_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR J36_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_RMFB         J36_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_DIRTYFB      J36_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB  J36_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     J36_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB J36_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_ADDFB2       J36_IOWR(0xB8, struct drm_mode_fb_cmd2)

#define DRM_MODE_CONNECTED 1

/* fourcc('X','R','2','4') -- 32 bits, no alpha.  udl advertises this and RGB565, and
 * this board's simplefb is x8r8g8b8, so XRGB8888 makes the common case a memcpy. */
#define DRM_FORMAT_XRGB8888 0x34325258u

#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOGET_FSCREENINFO 0x4602u

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char          id[16];
    unsigned long smem_start;
    uint32_t      smem_len, type, type_aux, visual;
    uint16_t      xpanstep, ypanstep, ywrapstep;
    uint32_t      line_length;
    unsigned long mmio_start;
    uint32_t      mmio_len, accel;
    uint16_t      capabilities, reserved[2];
};

/* ── tunables ───────────────────────────────────────────────────────────────────
 *
 * TILE is the diff granularity in SOURCE pixels.  64 gives 10x8 = 80 tiles over a
 * 640x480 panel: small enough that a clock ticking in the status bar sends one tile
 * and not one screen, large enough that the per-tile bookkeeping and the per-clip
 * URB setup in udl do not dominate.
 *
 * MAX_CLIPS is where "send the changed tiles" stops paying.  Past it the setup cost
 * of each rectangle beats the bytes saved and one rectangle over the whole visible
 * area is cheaper -- which is also exactly what a page transition looks like.
 */
#define TILE       64
#define MAX_CLIPS  24

/* Frame pacing.  20 Hz is chosen against the bus and not against the eye: a busy
 * frame at 1280x960 is a few hundred KB after tiling, and twenty of those is most of
 * what USB 2.0 will carry while a mouse and a disk are on the same controller.  The
 * idle path is much slower because a dashboard that is not moving should not be
 * costing anything at all. */
#define BUSY_INTERVAL_MS  50
#define IDLE_INTERVAL_MS  120

/* How often the connector is re-read while mirroring: a hub plugged in before the TV
 * has to become a picture without anything being unplugged. */
#define RECHECK_MS 2000

/* Between scans when there is no adapter.  Two seconds of nothing, forever, is the
 * steady state of this program on a board nobody has docked. */
#define SCAN_INTERVAL_MS 2000

static volatile sig_atomic_t stop_requested;
static int verbose;
static const char *allow_name = "udl";

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void chat(const char *fmt, ...)
{
    va_list ap;
    if (!verbose)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void nap(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* EINTR and EAGAIN retried, because a SIGTERM arriving in the middle of a modeset
 * should not be read as the modeset failing.  stop_requested is checked by the
 * callers, which is where stopping is a decision rather than an error. */
static int drm_call(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && (errno == EINTR || errno == EAGAIN) && !stop_requested);
    return r;
}

/* ── finding the node ───────────────────────────────────────────────────────────
 *
 * The allow-list check, which is the safety property this whole file rests on.  It is
 * done before any other ioctl is sent, so a node belonging to mediatek or lima is
 * opened, named, and closed -- nothing is asked of it and nothing is set on it.
 *
 * Returns an open fd, or -1.  On success `path' holds what was opened.
 */
static int open_mirror_node(char *path, size_t pathsz)
{
    static const char *dir = "/dev/dri";
    DIR *d;
    struct dirent *e;
    int found = -1;

    d = opendir(dir);
    if (!d) {
        chat("mirror: %s: %s", dir, strerror(errno));
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        /* Sized off d_name and not off "/dev/dri/cardN", because the compiler cannot
         * know the entry is short and -Wformat-truncation is right to say so. */
        char candidate[sizeof(e->d_name) + 32], name[32], desc[64];
        struct drm_version v;
        int fd;

        if (strncmp(e->d_name, "card", 4) != 0)
            continue;
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, e->d_name);

        fd = open(candidate, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            chat("mirror: %s: %s", candidate, strerror(errno));
            continue;
        }

        memset(name, 0, sizeof(name));
        memset(desc, 0, sizeof(desc));
        memset(&v, 0, sizeof(v));
        v.name_len = sizeof(name) - 1;
        v.name = name;
        v.desc_len = sizeof(desc) - 1;
        v.desc = desc;

        if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
            /* No version ioctl means this is not a DRM node we understand, and a
             * node we do not understand is one we do not modeset. */
            chat("mirror: %s answers no DRM_IOCTL_VERSION -- left alone", candidate);
            close(fd);
            continue;
        }

        if (strcmp(name, allow_name) != 0) {
            chat("mirror: %s is \"%s\" -- not \"%s\", left alone", candidate, name,
                 allow_name);
            close(fd);
            continue;
        }

        chat("mirror: %s is \"%s\" %d.%d.%d (%s)", candidate, name, v.version_major,
             v.version_minor, v.version_patchlevel, desc);
        snprintf(path, pathsz, "%s", candidate);
        found = fd;
        break;
    }

    closedir(d);
    return found;
}

/* ── what the card can do ───────────────────────────────────────────────────────
 *
 * Every list ioctl here is two calls: once with the counts zeroed, so the kernel
 * fills in how many there are, and once with pointers to buffers of that size.  It is
 * not optional -- GETCONNECTOR with count_modes == 0 on entry is also what makes the
 * kernel re-probe the connector rather than answer from its cache, which is what turns
 * "the TV was switched on just now" into a mode list.
 */
struct target {
    uint32_t conn_id;
    uint32_t crtc_id;
    struct drm_mode_modeinfo mode;
    int connected;
};

/* Score a mode: how many whole times the panel fits into it.  Bigger is better, and
 * among equal factors the smaller mode wins because every pixel past the picture is a
 * byte of black going over USB for nothing. */
static int mode_scale(const struct drm_mode_modeinfo *m, uint32_t sw, uint32_t sh)
{
    int fx, fy;
    if (!m->hdisplay || !m->vdisplay || !sw || !sh)
        return 0;
    fx = (int)(m->hdisplay / sw);
    fy = (int)(m->vdisplay / sh);
    return fx < fy ? fx : fy;
}

static int pick_target(int fd, uint32_t src_w, uint32_t src_h, struct target *out)
{
    struct drm_mode_card_res res;
    uint32_t *conns = NULL, *crtcs = NULL;
    int rc = -1;
    uint32_t i;

    memset(out, 0, sizeof(*out));

    memset(&res, 0, sizeof(res));
    if (drm_call(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        chat("mirror: GETRESOURCES: %s", strerror(errno));
        return -1;
    }
    if (!res.count_connectors || !res.count_crtcs) {
        chat("mirror: the node has %u connectors and %u crtcs -- nothing to drive",
             res.count_connectors, res.count_crtcs);
        return -1;
    }

    conns = calloc(res.count_connectors, sizeof(*conns));
    crtcs = calloc(res.count_crtcs, sizeof(*crtcs));
    if (!conns || !crtcs)
        goto out;

    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.fb_id_ptr = 0;
    res.encoder_id_ptr = 0;
    res.count_fbs = 0;
    res.count_encoders = 0;
    if (drm_call(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        chat("mirror: GETRESOURCES (pass 2): %s", strerror(errno));
        goto out;
    }

    for (i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        struct drm_mode_modeinfo *modes = NULL;
        uint32_t *encs = NULL;
        uint32_t j;
        int best = -1, best_scale = -1;

        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conns[i];
        if (drm_call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
            continue;

        if (conn.connection != DRM_MODE_CONNECTED) {
            chat("mirror: connector %u is not connected", conns[i]);
            continue;
        }
        out->connected = 1;
        if (!conn.count_modes) {
            chat("mirror: connector %u is connected but offers no modes", conns[i]);
            continue;
        }

        modes = calloc(conn.count_modes, sizeof(*modes));
        encs = conn.count_encoders ? calloc(conn.count_encoders, sizeof(*encs)) : NULL;
        if (!modes) {
            free(encs);
            continue;
        }
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr = (uint64_t)(uintptr_t)encs;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        conn.count_props = 0;
        if (!encs)
            conn.count_encoders = 0;
        if (drm_call(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            free(modes);
            free(encs);
            continue;
        }

        for (j = 0; j < conn.count_modes; j++) {
            int s = mode_scale(&modes[j], src_w, src_h);
            int better;

            if (best < 0) {
                better = 1;
            } else if (s != best_scale) {
                better = s > best_scale;
            } else {
                /* Same integer factor: take the smaller mode, and among equal sizes
                 * the one the monitor marked preferred by listing it first. */
                uint32_t a = (uint32_t)modes[j].hdisplay * modes[j].vdisplay;
                uint32_t b = (uint32_t)modes[best].hdisplay * modes[best].vdisplay;
                better = a < b;
            }
            if (better) {
                best = (int)j;
                best_scale = s;
            }
        }
        if (best < 0) {
            free(modes);
            free(encs);
            continue;
        }

        /* The CRTC.  A connector that is already lit names its encoder, and that
         * encoder usually names its crtc; a cold one names neither, and then the
         * answer is the first crtc this connector's first encoder is allowed to
         * drive.  udl has exactly one of each, so both paths land in the same place
         * -- the long way round is written out because nothing here should depend on
         * that staying true. */
        {
            struct drm_mode_get_encoder enc;
            uint32_t enc_id = conn.encoder_id;

            if (!enc_id && encs && conn.count_encoders)
                enc_id = encs[0];

            memset(&enc, 0, sizeof(enc));
            enc.encoder_id = enc_id;
            if (enc_id && drm_call(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
                if (enc.crtc_id) {
                    out->crtc_id = enc.crtc_id;
                } else {
                    uint32_t k;
                    for (k = 0; k < res.count_crtcs; k++) {
                        if (enc.possible_crtcs & (1u << k)) {
                            out->crtc_id = crtcs[k];
                            break;
                        }
                    }
                }
            }
            if (!out->crtc_id)
                out->crtc_id = crtcs[0];
        }

        out->conn_id = conns[i];
        out->mode = modes[best];
        rc = 0;

        free(modes);
        free(encs);
        break;
    }

out:
    free(conns);
    free(crtcs);
    return rc;
}

/* ── the source ─────────────────────────────────────────────────────────────────
 *
 * /dev/fb0, mapped read-only.  Read-only is not politeness: this program has no
 * business writing to the panel, and a bug that scribbled there would be one that
 * takes the handheld's own screen down while trying to add a second one.
 */
struct source {
    int fd;
    const uint8_t *pix;
    size_t map_len;
    uint32_t w, h, stride, bytespp;
    /* RGB565 has to be widened on the way out.  Anything else is refused, because a
     * guess about a pixel format is a mirror full of noise. */
    int rgb565;
};

static int source_open(struct source *s, const char *path)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    void *m;

    memset(s, 0, sizeof(*s));
    s->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (s->fd < 0) {
        chat("mirror: %s: %s", path, strerror(errno));
        return -1;
    }

    memset(&var, 0, sizeof(var));
    memset(&fix, 0, sizeof(fix));
    if (ioctl(s->fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(s->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        note("mirror: %s: FBIOGET_?SCREENINFO: %s", path, strerror(errno));
        close(s->fd);
        s->fd = -1;
        return -1;
    }

    s->w = var.xres;
    s->h = var.yres;
    s->stride = fix.line_length;
    s->bytespp = var.bits_per_pixel / 8;

    if (var.bits_per_pixel == 16) {
        s->rgb565 = 1;
    } else if (var.bits_per_pixel != 32) {
        note("mirror: %s is %u bpp and this only understands 16 and 32",
             path, var.bits_per_pixel);
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    if (!s->w || !s->h || s->stride < s->w * s->bytespp) {
        note("mirror: %s reports %ux%u with a %u byte line -- refusing it",
             path, s->w, s->h, s->stride);
        close(s->fd);
        s->fd = -1;
        return -1;
    }

    /* yres and not yres_virtual: a panned framebuffer would need the offset applied
     * per frame and simplefb never pans.  Mapping only what is displayed also means
     * a driver that lies about the virtual size cannot walk this off the end. */
    s->map_len = (size_t)s->stride * s->h;
    m = mmap(NULL, s->map_len, PROT_READ, MAP_SHARED, s->fd, 0);
    if (m == MAP_FAILED) {
        note("mirror: %s: mmap: %s", path, strerror(errno));
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    s->pix = m;
    return 0;
}

static void source_close(struct source *s)
{
    if (s->pix) {
        munmap((void *)s->pix, s->map_len);
        s->pix = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

/* ── the destination ────────────────────────────────────────────────────────────
 *
 * One dumb buffer, one framebuffer object, one SETCRTC.  There is deliberately no
 * double buffering: udl has no scanout of its own to tear against -- the "display" is
 * a USB transfer that happens when DIRTYFB says so -- so a second buffer would double
 * the memory and the copying to solve a problem this device does not have.
 */
struct sink {
    int fd;
    uint32_t handle, fb_id, pitch;
    uint32_t w, h;
    uint64_t size;
    uint8_t *pix;
};

static void sink_close(struct sink *k)
{
    if (k->pix) {
        munmap(k->pix, (size_t)k->size);
        k->pix = NULL;
    }
    if (k->fb_id) {
        unsigned int id = k->fb_id;
        drm_call(k->fd, DRM_IOCTL_MODE_RMFB, &id);
        k->fb_id = 0;
    }
    if (k->handle) {
        struct drm_mode_destroy_dumb d;
        memset(&d, 0, sizeof(d));
        d.handle = k->handle;
        drm_call(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        k->handle = 0;
    }
}

static int sink_open(struct sink *k, int fd, const struct drm_mode_modeinfo *mode)
{
    struct drm_mode_create_dumb create;
    struct drm_mode_fb_cmd2 fb;
    struct drm_mode_map_dumb map;
    void *m;

    memset(k, 0, sizeof(*k));
    k->fd = fd;
    k->w = mode->hdisplay;
    k->h = mode->vdisplay;

    memset(&create, 0, sizeof(create));
    create.width = k->w;
    create.height = k->h;
    create.bpp = 32;
    if (drm_call(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        note("mirror: CREATE_DUMB %ux%u: %s", k->w, k->h, strerror(errno));
        return -1;
    }
    k->handle = create.handle;
    k->pitch = create.pitch;
    k->size = create.size;

    memset(&fb, 0, sizeof(fb));
    fb.width = k->w;
    fb.height = k->h;
    fb.pixel_format = DRM_FORMAT_XRGB8888;
    fb.handles[0] = k->handle;
    fb.pitches[0] = k->pitch;
    if (drm_call(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        note("mirror: ADDFB2: %s", strerror(errno));
        sink_close(k);
        return -1;
    }
    k->fb_id = fb.fb_id;

    memset(&map, 0, sizeof(map));
    map.handle = k->handle;
    if (drm_call(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        note("mirror: MAP_DUMB: %s", strerror(errno));
        sink_close(k);
        return -1;
    }
    m = mmap(NULL, (size_t)k->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
             (off_t)map.offset);
    if (m == MAP_FAILED) {
        note("mirror: mmap of the dumb buffer: %s", strerror(errno));
        sink_close(k);
        return -1;
    }
    k->pix = m;

    /* Black, once.  Everything outside the centred picture is written here and never
     * again, so the letterbox costs one memset for the life of the session. */
    memset(k->pix, 0, (size_t)k->size);
    return 0;
}

/* ── the copy ───────────────────────────────────────────────────────────────────
 *
 * Layout is fixed for the session: an integer scale factor and a centred origin.  If
 * the mode is smaller than the panel in either direction -- which no HDMI monitor
 * does, but a badly-read EDID might -- the factor is 1 and the picture is cropped
 * rather than squeezed, because a cropped picture still says what is wrong with it.
 */
struct layout {
    int scale;
    uint32_t off_x, off_y;   /* in destination pixels */
    uint32_t src_w, src_h;   /* the part of the source that is visible */
};

static void layout_compute(struct layout *l, const struct source *s,
                           const struct sink *k, int force_one)
{
    int fx, fy;

    fx = (int)(k->w / s->w);
    fy = (int)(k->h / s->h);
    l->scale = fx < fy ? fx : fy;
    if (l->scale < 1 || force_one)
        l->scale = 1;

    l->src_w = s->w;
    l->src_h = s->h;
    if (l->src_w * (uint32_t)l->scale > k->w)
        l->src_w = k->w / (uint32_t)l->scale;
    if (l->src_h * (uint32_t)l->scale > k->h)
        l->src_h = k->h / (uint32_t)l->scale;

    l->off_x = (k->w - l->src_w * (uint32_t)l->scale) / 2;
    l->off_y = (k->h - l->src_h * (uint32_t)l->scale) / 2;
}

static inline uint32_t widen565(uint16_t p)
{
    /* Replicate the high bits into the low ones rather than shifting in zeros, so
     * full-scale stays full-scale: 0x1f must become 0xff and not 0xf8, or white comes
     * out grey on the second screen and matches nothing on the first. */
    uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* One tile, from the shadow (which the caller has just refreshed) into the sink.
 * The shadow and not the framebuffer, because the framebuffer is very likely
 * write-combining memory: reads off it are slow and this reads every pixel again to
 * expand it. */
static void blit_tile(const struct source *s, const struct sink *k,
                      const struct layout *l, const uint8_t *shadow,
                      uint32_t sx0, uint32_t sy0, uint32_t sx1, uint32_t sy1)
{
    uint32_t sy;

    for (sy = sy0; sy < sy1; sy++) {
        const uint8_t *srow = shadow + (size_t)sy * s->stride + (size_t)sx0 * s->bytespp;
        uint32_t dy0 = l->off_y + sy * (uint32_t)l->scale;
        int ky;

        for (ky = 0; ky < l->scale; ky++) {
            uint8_t *drow = k->pix + (size_t)(dy0 + (uint32_t)ky) * k->pitch +
                            (size_t)(l->off_x + sx0 * (uint32_t)l->scale) * 4;

            if (l->scale == 1 && !s->rgb565) {
                memcpy(drow, srow, (size_t)(sx1 - sx0) * 4);
                continue;
            }
            if (ky > 0) {
                /* The row above is already the answer: copy it sideways rather than
                 * expanding the source a second time.  At scale 2 that halves the
                 * per-pixel work and at scale 3 it cuts it to a third. */
                memcpy(drow, drow - k->pitch, (size_t)(sx1 - sx0) * (size_t)l->scale * 4);
                continue;
            }
            {
                uint32_t *d = (uint32_t *)drow;
                uint32_t sx;
                for (sx = 0; sx < sx1 - sx0; sx++) {
                    uint32_t px;
                    int kx;
                    if (s->rgb565)
                        px = widen565(((const uint16_t *)srow)[sx]);
                    else
                        px = ((const uint32_t *)srow)[sx];
                    for (kx = 0; kx < l->scale; kx++)
                        *d++ = px;
                }
            }
        }
    }
}

/* ── one session ────────────────────────────────────────────────────────────────
 *
 * Runs until the adapter goes away, the connector changes under us, or a signal
 * arrives.  Everything it allocates it frees; the caller loops.
 */
static int mirror_session(int fd, const char *node, const char *fbpath, int force_one,
                          int once)
{
    struct source src;
    struct sink snk;
    struct target tgt;
    struct layout lay;
    struct drm_mode_crtc crtc;
    struct drm_clip_rect clips[MAX_CLIPS];
    uint8_t *shadow = NULL;
    uint64_t last_check;
    uint32_t conn_array[1];
    int rc = -1;
    int first = 1;

    if (source_open(&src, fbpath) < 0)
        return -1;

    if (pick_target(fd, src.w, src.h, &tgt) < 0) {
        if (tgt.connected)
            chat("mirror: %s has a connected output but no usable mode yet", node);
        else
            chat("mirror: %s has nothing connected yet", node);
        source_close(&src);
        return -1;
    }

    /* Best effort.  The first process to open a card with no master becomes master on
     * open, so this normally succeeds trivially; it fails when something else -- an X
     * server on a card this program was pointed at by hand -- already holds it, and
     * saying so is more useful than a SETCRTC that fails with EACCES further down. */
    if (drm_call(fd, DRM_IOCTL_SET_MASTER, NULL) < 0)
        chat("mirror: SET_MASTER: %s (continuing -- open() usually grants it)",
             strerror(errno));

    if (sink_open(&snk, fd, &tgt.mode) < 0) {
        source_close(&src);
        return -1;
    }

    layout_compute(&lay, &src, &snk, force_one);

    memset(&crtc, 0, sizeof(crtc));
    conn_array[0] = tgt.conn_id;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)conn_array;
    crtc.count_connectors = 1;
    crtc.crtc_id = tgt.crtc_id;
    crtc.fb_id = snk.fb_id;
    crtc.x = 0;
    crtc.y = 0;
    crtc.mode_valid = 1;
    crtc.mode = tgt.mode;
    if (drm_call(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        note("mirror: SETCRTC on %s crtc %u: %s", node, tgt.crtc_id, strerror(errno));
        goto out;
    }

    shadow = malloc(src.map_len);
    if (!shadow) {
        note("mirror: no memory for a %zu byte shadow of the panel", src.map_len);
        goto out;
    }
    /* Deliberately NOT a copy of the panel.  Seeding it with something the source
     * cannot match forces every tile dirty on the first pass, which is what paints
     * the initial picture; seeding it from the source would show black until
     * something on the dashboard happened to change. */
    memset(shadow, 0xa5, src.map_len);

    note("mirror: %s %ux%u \"%s\" <- %s %ux%u, %dx integer scale at +%u+%u",
         node, tgt.mode.hdisplay, tgt.mode.vdisplay, tgt.mode.name, fbpath,
         src.w, src.h, lay.scale, lay.off_x, lay.off_y);

    last_check = now_ms();

    while (!stop_requested) {
        uint32_t tx, ty, tiles_x, tiles_y;
        int nclips = 0, overflow = 0;
        struct drm_mode_fb_dirty_cmd dirty;

        tiles_x = (lay.src_w + TILE - 1) / TILE;
        tiles_y = (lay.src_h + TILE - 1) / TILE;

        for (ty = 0; ty < tiles_y; ty++) {
            uint32_t sy0 = ty * TILE;
            uint32_t sy1 = sy0 + TILE;
            if (sy1 > lay.src_h)
                sy1 = lay.src_h;

            for (tx = 0; tx < tiles_x; tx++) {
                uint32_t sx0 = tx * TILE;
                uint32_t sx1 = sx0 + TILE;
                size_t span;
                uint32_t sy;
                int changed = 0;

                if (sx1 > lay.src_w)
                    sx1 = lay.src_w;
                span = (size_t)(sx1 - sx0) * src.bytespp;

                for (sy = sy0; sy < sy1; sy++) {
                    size_t off = (size_t)sy * src.stride + (size_t)sx0 * src.bytespp;
                    if (memcmp(src.pix + off, shadow + off, span) != 0) {
                        changed = 1;
                        break;
                    }
                }
                if (!changed)
                    continue;

                for (sy = sy0; sy < sy1; sy++) {
                    size_t off = (size_t)sy * src.stride + (size_t)sx0 * src.bytespp;
                    memcpy(shadow + off, src.pix + off, span);
                }
                blit_tile(&src, &snk, &lay, shadow, sx0, sy0, sx1, sy1);

                if (nclips < MAX_CLIPS) {
                    clips[nclips].x1 = (unsigned short)(lay.off_x + sx0 * (uint32_t)lay.scale);
                    clips[nclips].y1 = (unsigned short)(lay.off_y + sy0 * (uint32_t)lay.scale);
                    clips[nclips].x2 = (unsigned short)(lay.off_x + sx1 * (uint32_t)lay.scale);
                    clips[nclips].y2 = (unsigned short)(lay.off_y + sy1 * (uint32_t)lay.scale);
                    nclips++;
                } else {
                    overflow = 1;
                }
            }
        }

        if (overflow || first) {
            /* One rectangle over the whole picture.  `first' is in here because the
             * seeded shadow makes every tile dirty and that is exactly the case the
             * cap exists for. */
            clips[0].x1 = (unsigned short)lay.off_x;
            clips[0].y1 = (unsigned short)lay.off_y;
            clips[0].x2 = (unsigned short)(lay.off_x + lay.src_w * (uint32_t)lay.scale);
            clips[0].y2 = (unsigned short)(lay.off_y + lay.src_h * (uint32_t)lay.scale);
            nclips = 1;
        }

        if (nclips) {
            memset(&dirty, 0, sizeof(dirty));
            dirty.fb_id = snk.fb_id;
            dirty.num_clips = (uint32_t)nclips;
            dirty.clips_ptr = (uint64_t)(uintptr_t)clips;
            if (drm_call(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) < 0) {
                if (errno == ENODEV || errno == ENOENT || errno == EIO) {
                    note("mirror: %s went away", node);
                    rc = 0;
                    goto out;
                }
                note("mirror: DIRTYFB: %s", strerror(errno));
                goto out;
            }
        }
        first = 0;

        if (once) {
            rc = 0;
            goto out;
        }

        /* Has the far end changed?  A monitor switched on behind an adapter that was
         * already plugged in, or switched off, or swapped for one with a different
         * EDID -- all of them mean this session's mode is wrong and the cheapest fix
         * is to tear down and let the caller build a new one. */
        if (now_ms() - last_check >= RECHECK_MS) {
            struct target again;
            last_check = now_ms();
            if (pick_target(fd, src.w, src.h, &again) < 0) {
                note("mirror: %s lost its output", node);
                rc = 0;
                goto out;
            }
            if (again.conn_id != tgt.conn_id ||
                again.mode.hdisplay != tgt.mode.hdisplay ||
                again.mode.vdisplay != tgt.mode.vdisplay) {
                note("mirror: %s changed mode -- restarting the session", node);
                rc = 0;
                goto out;
            }
        }

        nap(nclips ? BUSY_INTERVAL_MS : IDLE_INTERVAL_MS);
    }
    rc = 0;

out:
    free(shadow);
    sink_close(&snk);
    source_close(&src);
    /* Not DROP_MASTER on the way out of a session that ended in an error: the node is
     * closed immediately after, which drops it anyway, and asking a device that has
     * just been unplugged for one more ioctl is one more thing to hang on. */
    return rc;
}

static void usage(void)
{
    printf(
"j36-mixmirror -- mirror /dev/fb0 onto a USB-HDMI (DisplayLink) adapter.\n"
"\n"
"  -f PATH   source framebuffer (default /dev/fb0)\n"
"  -n NAME   DRM driver name to accept (default udl).  This is the safety\n"
"            interlock -- a node with any other name is never modeset.\n"
"  -1        do not scale; centre the panel at 1:1 whatever the mode allows\n"
"  -o        mirror one frame and exit, for testing\n"
"  -s        scan, report what was found, and exit without touching anything\n"
"  -v        say what is being looked at and why it was skipped\n"
"  -h        this\n"
"\n"
"With no options it runs forever: it polls /dev/dri, mirrors while an adapter is\n"
"there, and goes back to polling when it is unplugged.  Started that way by\n"
"j36-mixmirror.service.\n");
}

int main(int argc, char **argv)
{
    const char *fbpath = "/dev/fb0";
    int force_one = 0, once = 0, scan_only = 0;
    int opt;
    struct sigaction sa;

    while ((opt = getopt(argc, argv, "f:n:1osvh")) != -1) {
        switch (opt) {
        case 'f': fbpath = optarg; break;
        case 'n': allow_name = optarg; break;
        case '1': force_one = 1; break;
        case 'o': once = 1; break;
        case 's': scan_only = 1; verbose = 1; break;
        case 'v': verbose = 1; break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    /* SA_RESTART deliberately off: the point of the handler is to break the nap and
     * the ioctl retry loop, and a restarted nanosleep would sit out its full interval
     * before anything noticed the signal. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (scan_only) {
        char node[288];
        int fd = open_mirror_node(node, sizeof(node));
        if (fd < 0) {
            note("mirror: no \"%s\" node in /dev/dri -- nothing to mirror onto",
                 allow_name);
            return 1;
        }
        {
            struct source src;
            struct target tgt;
            int ok = 0;
            if (source_open(&src, fbpath) == 0) {
                note("mirror: source %s is %ux%u, %u bpp, %u byte line",
                     fbpath, src.w, src.h, src.bytespp * 8, src.stride);
                if (pick_target(fd, src.w, src.h, &tgt) == 0) {
                    struct sink fake;
                    struct layout lay;
                    memset(&fake, 0, sizeof(fake));
                    fake.w = tgt.mode.hdisplay;
                    fake.h = tgt.mode.vdisplay;
                    layout_compute(&lay, &src, &fake, force_one);
                    note("mirror: %s would drive connector %u on crtc %u at %ux%u "
                         "\"%s\", %dx integer scale at +%u+%u",
                         node, tgt.conn_id, tgt.crtc_id, tgt.mode.hdisplay,
                         tgt.mode.vdisplay, tgt.mode.name, lay.scale, lay.off_x,
                         lay.off_y);
                    ok = 1;
                } else {
                    note("mirror: %s is present but has %s", node,
                         tgt.connected ? "no usable mode" : "nothing connected");
                }
                source_close(&src);
            }
            close(fd);
            return ok ? 0 : 1;
        }
    }

    while (!stop_requested) {
        char node[288];
        int fd = open_mirror_node(node, sizeof(node));

        if (fd < 0) {
            if (once) {
                note("mirror: no \"%s\" node in /dev/dri", allow_name);
                return 1;
            }
            nap(SCAN_INTERVAL_MS);
            continue;
        }

        if (mirror_session(fd, node, fbpath, force_one, once) < 0 && once) {
            close(fd);
            return 1;
        }
        close(fd);

        if (once)
            return 0;
        /* A session that ended because the connector was not ready yet comes straight
         * back round; the scan interval is what keeps that from being a spin. */
        nap(SCAN_INTERVAL_MS);
    }

    note("mirror: stopping");
    return 0;
}
