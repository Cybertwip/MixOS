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
 * ── SAYING WHY, WHICH IS MOST OF WHAT THIS PROGRAM TURNED OUT TO BE FOR ──────────
 *
 * This started life silent by design -- a mirror with nothing to mirror onto should
 * not fill a journal -- and silent by design is exactly what "the HDMI mirror never
 * runs" is a report of.  Every "not yet" branch was a chat(), which is -v only, so a
 * board with a dock plugged into it and a black television wrote NOT ONE LINE for its
 * whole uptime.  From the outside that is indistinguishable from a unit systemd never
 * started, and there was no way to tell the two apart without a serial console.
 *
 * So it is a state machine now, and it says every transition at note() level while
 * still saying nothing at all when nothing has changed.  The states are the questions
 * somebody would ask in order:
 *
 *   no /dev/dri          no DRM driver registered anything.  The boot did not get as
 *                        far as loading modules.
 *   no "udl" node        the interesting one, and the one with three different
 *                        answers underneath it -- see explain_no_node().
 *   nothing connected    a DisplayLink adapter is bound and its HDMI socket is empty,
 *                        or the television at the far end is switched off.
 *   no mode              connected, and the EDID has not been read yet.
 *   mirroring            with the mode, the scale and the offsets.
 *
 * The same line goes into /run/j36/mirror.status, overwritten whole on every change,
 * because the dashboard's Diagnostics page reads it: the person this matters to is
 * holding the handheld and has no journal in front of them.  Two lines -- a keyword
 * for the dashboard to take a colour from, then the sentence for the person to read
 * -- so that rewording the sentence, which will happen, never breaks the colour.  It
 * is a tmpfs the initramfs already makes and NOTHING here writes to the shared
 * rootfs.  -s and -o deliberately do not publish, so running this by hand to look at
 * something cannot overwrite what the running service is reporting.
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
#include <sys/stat.h>
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

/* Where the current state goes for the dashboard to read.  A tmpfs path: /init makes
 * /run/j36 on every boot and the rootfs on the card is never written to. */
#define STATUS_PATH "/run/j36/mirror.status"

/* DisplayLink's USB vendor ID, and the single most useful fact this program can
 * report.  Mainline udl matches on it and on nothing else, so a bus with no 17e9 on
 * it is a bus no version of this software will ever draw to. */
#define DISPLAYLINK_VENDOR "17e9"

/*
 * ── THE TARGET IS "NOT THIS BOARD'S OWN", NOT "udl" ────────────────────────────
 *
 * The allow-list used to be the single string "udl", and the paragraph at the top of
 * this file argued for it: a name is stabler than a minor number.  That half is still
 * true.  What was wrong was making the name a WHITELIST OF ONE, because it asks the
 * question backwards.  This program does not care which chip is on the far end of the
 * cable -- it cares that the node has a CRTC it can modeset and that the node is not
 * the panel it is copying FROM.  Written as a whitelist, every USB display part that
 * is not DisplayLink -- Fresco Logic FL2000, Silicon Motion SM76x, MCT/Trigger, and
 * whatever binds next -- is refused by this program even when the kernel has already
 * bound it and put a working card node in /dev/dri.  That is a rebuild standing
 * between the user and a screen that is already lit.
 *
 * So it is a DENY-list of the two drivers this board brings up itself:
 *
 *   lima        the GPU.  It has no CRTC and no connector at all -- render-only --
 *               so modesetting it would fail anyway, but it is named here because it
 *               is card0 on this board and the loop must not stop at it.
 *   mediatek    the panel.  It IS the source; mirroring it onto itself is a loop.
 *
 * Anything else in /dev/dri got there because a device was plugged in, and a plugged
 * in device with a DRM node is what this program exists to draw on.  -n still forces
 * one exact name for when that judgement is wrong.
 */
static const char *const board_drivers[] = {
    "lima", "mediatek", "mediatek-drm", "mtk-drm", NULL
};

static volatile sig_atomic_t stop_requested;
static int verbose;
static int publishing = 1;
static const char *allow_name;      /* -n: exact match, and nothing else */

static int is_board_driver(const char *name)
{
    int i;

    for (i = 0; board_drivers[i]; ++i)
        if (strcmp(name, board_drivers[i]) == 0)
            return 1;
    return 0;
}

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

/* ── where the program has got to, said once ────────────────────────────────────
 *
 * The header has the reasoning.  The mechanism is two lines: remember the last thing
 * said, and say a new one only when it differs.  Comparing the whole STRING and not
 * just the state is deliberate -- "no DisplayLink node, and here is what is on the
 * bus" has to be said again when something is plugged into the hub, because that is
 * the moment somebody is watching for it.
 */
enum stage {
    STAGE_START = 0,
    STAGE_NO_DRI,
    STAGE_NO_UDL,
    STAGE_NO_OUTPUT,
    STAGE_NO_MODE,
    STAGE_MIRRORING,
    STAGE_STOPPED,
};

static enum stage stage_now = STAGE_START;
static char stage_line[512];

/* The first line of the status file, and the only part of it any other program is
 * allowed to depend on.  The sentence underneath is written for a person and will be
 * reworded whenever a better wording turns up; this will not, so the dashboard can
 * pick a colour from it without pattern-matching English.  Kept deliberately short
 * and lower-case: it is a token, not a label. */
static const char *stage_word(enum stage s)
{
    switch (s) {
    case STAGE_NO_DRI:      return "no-drm";
    case STAGE_NO_UDL:      return "no-adapter";
    case STAGE_NO_OUTPUT:   return "no-screen";
    case STAGE_NO_MODE:     return "no-mode";
    case STAGE_MIRRORING:   return "mirroring";
    case STAGE_STOPPED:     return "stopped";
    case STAGE_START:       break;
    }
    return "starting";
}

/*
 * Two lines, always: the keyword, then the sentence.  A reader that wants a colour
 * takes the first; a reader that wants to know what happened takes the rest.  The
 * file is truncated and rewritten whole on every change, so a torn read is a short
 * read of the previous state rather than a mixture of two -- and every state here is
 * re-asserted within SCAN_INTERVAL_MS anyway.
 */
static void publish(enum stage s, const char *line)
{
    char buf[sizeof(stage_line) + 64];
    int n, fd;

    if (!publishing)
        return;
    /* Best effort from here down.  /run/j36 exists on every boot that got as far as
     * a dashboard, but this also runs from a rescue shell, and a mirror that refused
     * to start because it could not write a status file would be the tail wagging
     * the dog.  mkdir's failure is EEXIST in the normal case and is not read. */
    mkdir("/run/j36", 0755);
    fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    n = snprintf(buf, sizeof(buf), "%s\n%s\n", stage_word(s), line);
    if (n > 0 && write(fd, buf, (size_t)n) < 0) {
        /* Nothing useful to do: this is the reporting path, so failing to report
         * that reporting failed is where it has to stop. */
    }
    close(fd);
}

static void report(enum stage s, const char *fmt, ...)
{
    char line[sizeof(stage_line)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (s == stage_now && strcmp(line, stage_line) == 0)
        return;
    stage_now = s;
    snprintf(stage_line, sizeof(stage_line), "%s", line);
    publish(s, line);
    note("%s", line);
}

/* ── what is actually on the port ───────────────────────────────────────────────
 *
 * sysfs only, and every one of these is a read of a file the kernel generates on
 * demand: no ioctl, nothing opened that could be modeset, nothing that can disturb a
 * screen.  They are run only when the mirror has just decided it has nothing to draw
 * on, which is a handful of opens every two seconds on a board with a dock plugged
 * into it and none at all on one that is mirroring.
 */
static int read_line_file(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return 0;
}

/* /sys/module/udl is the cheapest question in this file and it halves the search
 * space: no directory means the module was never insmodded -- an old j36/usb payload
 * on the card, or a boot without j36.usb -- and nothing plugged in will ever bind. */
static int udl_loaded(void)
{
    return access("/sys/module/udl", F_OK) == 0;
}

/*
 * Every USB device on the bus as "vvvv:pppp" in one line, root hubs included because
 * their absence is itself an answer.  Returns 1 if any of them is DisplayLink, 0 if
 * none is, -1 if there is no /sys/bus/usb/devices at all -- which means usbcore is
 * not loaded and the question is a different one.  An empty `out' with a 0 return is
 * a bus with a controller and nothing on it.
 */
static int usb_inventory(char *out, size_t outsz)
{
    static const char *dir = "/sys/bus/usb/devices";
    DIR *d;
    struct dirent *e;
    size_t used = 0;
    int displaylink = 0;

    out[0] = '\0';
    d = opendir(dir);
    if (!d)
        return -1;

    while ((e = readdir(d)) != NULL) {
        char path[sizeof(e->d_name) + 64], vid[16], pid[16];

        if (e->d_name[0] == '.')
            continue;
        /* "1-1:1.0" is an interface of a device already listed as "1-1", and it
         * carries no idVendor of its own.  Skipping it keeps one physical thing to
         * one entry in the line. */
        if (strchr(e->d_name, ':'))
            continue;

        snprintf(path, sizeof(path), "%s/%s/idVendor", dir, e->d_name);
        if (read_line_file(path, vid, sizeof(vid)) < 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s/idProduct", dir, e->d_name);
        if (read_line_file(path, pid, sizeof(pid)) < 0)
            continue;

        if (strcmp(vid, DISPLAYLINK_VENDOR) == 0)
            displaylink = 1;
        used += (size_t)snprintf(out + used, used < outsz ? outsz - used : 0,
                                 "%s%s:%s", used ? " " : "", vid, pid);
        if (used >= outsz)
            break;          /* truncated, and snprintf already stopped writing */
    }

    closedir(d);
    return displaylink;
}

/*
 * ── THE LINE THIS WHOLE EXERCISE WAS ABOUT ──────────────────────────────────────
 *
 * Called when /dev/dri holds no node named "udl".  Three quite different things can
 * be true underneath that and they want three different answers, so all three are
 * measured rather than guessed at:
 *
 *   udl.ko not loaded         the j36/usb payload on the card is older than this
 *                             feature, or the boot had j36.usb=0.  Nothing will ever
 *                             bind, and it is fixed by unpacking sd-root.tar.gz.
 *
 *   loaded, nothing with      the common case, and it is NOT a software fault.  A
 *   vendor 17e9 on the bus    USB-C hub whose HDMI socket is DisplayPort Alt Mode
 *                             carries video on wires MT6592 does not have, and no
 *                             driver can conjure a DisplayPort transmitter onto a
 *                             2013 SoC.  Only a DisplayLink adapter has a chance.
 *
 *   loaded, 17e9 present,     a DL-3xxx or later part: USB 3.0, a different protocol,
 *   still no card node        no in-tree driver.  Mainline udl is DL-1x0/DL-1x5 only.
 *
 * `seen' is what /dev/dri did hold, as "card0=mediatek card1=lima", because the first
 * thing anybody wants confirmed is that the directory is not empty and that what is
 * in it is the board's own two drivers.
 */
static void explain_no_node(const char *seen)
{
    char bus[256];
    int dl = usb_inventory(bus, sizeof(bus));

    if (!udl_loaded()) {
        report(STAGE_NO_UDL,
               "mirror: no DisplayLink display and udl.ko is not loaded -- "
               "/dev/dri holds %s.  Nothing plugged into the port can bind.  "
               "Unpack sd-root.tar.gz onto the OS partition and boot with j36.usb=1.",
               seen[0] ? seen : "no cards at all");
        return;
    }
    if (dl < 0) {
        report(STAGE_NO_UDL,
               "mirror: udl.ko is loaded but there is no /sys/bus/usb/devices -- "
               "usbcore did not register, so the port is not enumerating anything.  "
               "/dev/dri holds %s.", seen[0] ? seen : "no cards at all");
        return;
    }
    if (dl == 0) {
        report(STAGE_NO_UDL,
               "mirror: udl.ko is loaded and waiting; nothing on the port is "
               "DisplayLink (vendor %s).  The bus has %s and /dev/dri holds %s.  "
               "A USB-C hub whose HDMI is DisplayPort Alt Mode cannot work here -- "
               "MT6592 has no DisplayPort.  Only a DisplayLink DL-1x0/DL-1x5 "
               "adapter drives a screen on this board.",
               DISPLAYLINK_VENDOR, bus[0] ? bus : "nothing on it",
               seen[0] ? seen : "no cards at all");
        return;
    }
    report(STAGE_NO_UDL,
           "mirror: a DisplayLink device is on the bus (%s) and udl.ko did not claim "
           "it -- /dev/dri holds %s.  Mainline udl speaks the USB 2.0 DL-1x0/DL-1x5 "
           "protocol only; a DL-3xxx or later part needs the out-of-tree evdi driver "
           "and is USB 3.0 besides.  dmesg has udl's own refusal.",
           bus, seen[0] ? seen : "no cards at all");
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
 * Returns an open fd, -1 when /dev/dri was read and held no "udl", or -2 when the
 * directory could not be read at all.  Those two are told apart because they are
 * different faults: -2 is "no DRM driver registered anything", which is a boot that
 * did not get as far as loading modules, and -1 is the interesting one.
 *
 * On success `path' holds what was opened.  Either way `seen' is filled in with what
 * WAS there, as "card0=mediatek card1=lima", for the message that follows.
 */
static int open_mirror_node(char *path, size_t pathsz, char *seen, size_t seensz)
{
    static const char *dir = "/dev/dri";
    DIR *d;
    struct dirent *e;
    size_t used = 0;
    int found = -1;

    seen[0] = '\0';
    d = opendir(dir);
    if (!d) {
        chat("mirror: %s: %s", dir, strerror(errno));
        return -2;
    }

    while ((e = readdir(d)) != NULL) {
        /* Sized off d_name and not off "/dev/dri/cardN", because the compiler cannot
         * know the entry is short and -Wformat-truncation is right to say so. */
        char candidate[sizeof(e->d_name) + 32], name[32], desc[64];
        struct drm_version v;
        int fd;
        int versioned;

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

        versioned = 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
            /* No version ioctl means this is not a DRM node we understand, and a
             * node we do not understand is one we do not modeset.  Under a deny-list
             * that has to be tracked explicitly: "?" is not a board driver, so
             * without this flag an unreadable node would pass the test. */
            chat("mirror: %s answers no DRM_IOCTL_VERSION -- left alone", candidate);
            snprintf(name, sizeof(name), "?");
            versioned = 0;
        }

        /* Recorded before the allow-list refuses it, because "what IS in /dev/dri"
         * is half of the answer when there is no udl in there. */
        used += (size_t)snprintf(seen + used, used < seensz ? seensz - used : 0,
                                 "%s%s=%s", used ? " " : "", e->d_name, name);
        if (used >= seensz)
            used = seensz;      /* full; snprintf has already stopped writing */

        if (!versioned ||
            (allow_name ? strcmp(name, allow_name) != 0 : is_board_driver(name))) {
            chat("mirror: %s is \"%s\" -- %s, left alone", candidate, name,
                 allow_name ? "not the driver -n asked for"
                            : "one of this board's own displays");
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
        /* Both of these used to be chat(), which is -v only, and between them they
         * cover every dock that IS bound and shows nothing.  A DisplayLink adapter
         * with an empty HDMI socket, or one behind a television that is switched
         * off, produced total silence and looked exactly like a service that was
         * never started. */
        if (tgt.connected)
            report(STAGE_NO_MODE,
                   "mirror: %s is a DisplayLink display with something connected to "
                   "it that has offered no mode list yet -- its EDID has not been "
                   "read.  Retrying every %d ms.", node, SCAN_INTERVAL_MS);
        else
            report(STAGE_NO_OUTPUT,
                   "mirror: %s is bound and ready and nothing is connected to its "
                   "HDMI socket -- plug a screen in, or switch on the one that is "
                   "there.  Retrying every %d ms.", node, SCAN_INTERVAL_MS);
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

    report(STAGE_MIRRORING,
           "mirror: %s %ux%u \"%s\" <- %s %ux%u, %dx integer scale at +%u+%u",
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
                /* No note here on purpose: the caller comes straight back round,
                 * pick_target fails the same way at the top of the next session and
                 * the state machine says which of "unplugged" and "switched off" it
                 * was.  One line, not two, and the useful one. */
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
"j36-mixmirror.service.  In that mode it writes its current state to\n"
STATUS_PATH ", which is where the dashboard's Diagnostics page reads it\n"
"from: one keyword on the first line -- starting, no-drm, no-adapter, no-screen,\n"
"no-mode, mirroring, stopped -- and then the same sentence it put in the\n"
"journal.  -s and -o do not write it, so running this by hand cannot overwrite\n"
"what the service is reporting.\n");
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
        /* Neither of the by-hand modes publishes: somebody looking at a dock from a
         * shell must not overwrite the status the running service is showing on the
         * dashboard. */
        case 'o': once = 1; publishing = 0; break;
        case 's': scan_only = 1; verbose = 1; publishing = 0; break;
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
        char node[288], seen[256];
        int fd = open_mirror_node(node, sizeof(node), seen, sizeof(seen));
        if (fd == -2) {
            note("mirror: there is no /dev/dri at all -- no DRM driver has "
                 "registered a card, so nothing could appear as one");
            return 1;
        }
        if (fd < 0) {
            /* The same three-way answer the service gives, because this is the
             * command somebody runs when the service has already told them
             * something they want to check by hand. */
            explain_no_node(seen);
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

    /* Said before the first scan so that the status file exists from the moment the
     * service does.  Without it there is a window -- short, but real, and longer on
     * a board that is still loading modules -- in which a running mirror has published
     * nothing, and the dashboard reads a missing file as a mirror that never started.
     * That is the exact confusion this whole mechanism was added to end. */
    report(STAGE_START, "mirror: started, looking for a DisplayLink adapter");

    while (!stop_requested) {
        char node[288], seen[256];
        int fd = open_mirror_node(node, sizeof(node), seen, sizeof(seen));

        if (fd == -2) {
            report(STAGE_NO_DRI,
                   "mirror: there is no /dev/dri at all -- no DRM driver has "
                   "registered a card, so a dock has nothing to appear as.  "
                   "j36.usb=1 on the kernel command line is what loads udl.ko.");
            if (once)
                return 1;
            nap(SCAN_INTERVAL_MS);
            continue;
        }
        if (fd < 0) {
            /* THE LINE THIS TASK EXISTED FOR.  What used to be here was `nap and
             * continue', silently, forever -- so a board with a dock plugged into it
             * and a black television said nothing at all for its whole uptime. */
            explain_no_node(seen);
            if (once)
                return 1;
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

    /* Said through report() rather than note() so the status file stops claiming a
     * state that is no longer true: a stale `mirroring' left behind by a systemctl
     * stop would put a green row on the Diagnostics page with nothing behind it. */
    report(STAGE_STOPPED, "mirror: stopped on a signal -- nothing is being mirrored");
    return 0;
}
