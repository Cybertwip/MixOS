/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
 * option; see device/j36-ultra/LICENSE for the texts and for what they do not
 * cover. */
/*
 * j36-padx -- the pad, as a mouse and a keyboard, inside an X server.
 *
 * WHY THIS EXISTS AT ALL.  Everything graphical in Debian that is not a text
 * browser wants an X server or a Wayland compositor, and both of them want a
 * pointer.  This board has no touchscreen and no trackpad; it has two analog
 * sticks, a D-pad, four face buttons, four shoulders and Start/Select/Menu, and
 * the kernel presents all of them as ONE evdev device:
 *
 *     j36_mt6592_input.c:  input->name = "J36 Ultra built-in gamepad"
 *         EV_KEY: KEY_UP/DOWN/LEFT/RIGHT, KEY_VOLUME*, KEY_F12,
 *                 BTN_A..BTN_MODE, BTN_THUMBL/R
 *         EV_ABS: ABS_X/ABS_Y (left stick), ABS_Z/ABS_RZ (right stick),
 *                 -4096..4096, with the driver's deadzone already taken out
 *
 * Nothing downstream can use that.  udev's input_id tags anything carrying
 * BTN_SOUTH..BTN_THUMBR as ID_INPUT_JOYSTICK, libinput refuses joysticks by
 * design, and even the older xf86-input-evdev cannot help: it maps an evdev code
 * to X keycode `code + 8', and BTN_A is 0x130, so the four face buttons land at
 * 312 and up -- outside the 8..255 an X keycode can be.  A pad is not a pointer
 * and no configuration file turns it into one.
 *
 * So this does the translation itself, in the one place where the codes are still
 * ours: it reads the raw evdev device and synthesises pointer motion, button
 * presses and key presses through the XTEST extension.  XTEST events enter the
 * server exactly where a real device's would -- they go through the same event
 * queue, hit the same focus and grab rules, and every client sees them as
 * ordinary input.  There is nothing browser-specific here; this is what makes ANY
 * X program usable on this device.
 *
 * WHY XTEST AND NOT uinput.  uinput would be the other way to do this: build a
 * virtual mouse in the kernel and let X pick it up through udev like any other
 * device.  It was rejected for two reasons, one of them decisive.  The decisive
 * one is that CONFIG_INPUT_UINPUT is not set in the kernel this image ships, so
 * that route costs a kernel option, a rebuild, and a udev classification that has
 * to come out right on a device we invented -- for a result no better than this.
 * The second is that a uinput mouse is visible to EVERYTHING, including mixdash's
 * own pad reader and any game launched later; a pointer that appears system-wide
 * because a browser is open is a bug waiting for its bug report.  XTEST reaches
 * exactly one X server and stops existing when that server does.
 *
 * ── ONE PAD OWNER, WITH AN EXPLICIT HAND-OFF ─────────────────────────
 *
 * mixdash and this bridge both have the built-in evdev node open, but only the
 * program whose pixels are on the panel may consume it.  The session therefore
 * starts this bridge with --grab.  On a Menu hold it releases that grab, hides
 * the X cursor, asks mixdash for the switcher, and stops ITSELF before another
 * event can reach X.  mixdash records this pid and continues it only after the X
 * frame has been restored; the SIGCONT path drains events used by the switcher
 * before taking the grab back.
 *
 * The explicit self-stop matters because xinit can put descendants in another
 * process group.  Stopping only the launcher's group left this bridge alive over
 * the dashboard: its software cursor then restored pieces of X's old framebuffer
 * wherever it moved.  That looked like a transparent, destructive Qt cursor, but
 * was a second live cursor owner painting stale X pixels.  There is no shared
 * interval now: release happens before SIGUSR1 and re-grab after SIGCONT.
 *
 * WHAT IS DELIBERATELY NOT READ, which is the same narrow match the grab used and
 * matters for its own reasons.  A USB keyboard or mouse in the dock is a real X
 * input device that libinput will pick up and drive properly, and it must keep
 * working -- so a device that looks like a keyboard (it has the letter keys) or
 * like a mouse (it has EV_REL) is left alone even if something about it also
 * looks like a pad.  See looks_like_pad().
 *
 * A USB PAD IS READ TOO, and on purpose.  An Xbox, PlayStation or Switch pad in
 * the port matches the same test, so it drives the browser exactly as the built-in
 * one does -- which is the right answer on a console, and it is also why the axis
 * handling below reads each device's real ranges instead of assuming this board's.
 * Those pads do not agree with each other about anything:
 *
 *     built-in    left ABS_X/ABS_Y  right ABS_Z/ABS_RZ   -4096..4096
 *     xpad        left ABS_X/ABS_Y  right ABS_RX/ABS_RY  -32768..32767,
 *                 and ABS_Z/ABS_RZ are the TRIGGERS, 0..255 or 0..1023
 *     hid-sony    left ABS_X/ABS_Y  right ABS_Z/ABS_RZ   0..255, centre 128
 *     hid-playstation, hid-nintendo  right on ABS_RX/ABS_RY
 *
 * Reading ABS_Z as a right stick on an xpad would have turned the left trigger
 * into a scroll wheel.  So the right stick is ABS_RX/ABS_RY when the device has
 * that pair and ABS_Z/ABS_RZ when it does not, and every axis is scaled by the
 * minimum, maximum and flat that EVIOCGABS reports for it rather than by a
 * constant.  The D-pad is read from KEY_UP..KEY_RIGHT, from BTN_DPAD_UP..RIGHT
 * and from the ABS_HAT0X/ABS_HAT0Y hat, because those three are how the same four
 * directions arrive from the three families.
 *
 * THE MAP.  Chosen so that the four things a browser needs -- point, click, go
 * back, scroll -- are the four things nearest the thumbs, and so that no binding
 * needs a chord:
 *
 *     Left stick     pointer, proportional to deflection
 *     Right stick    scroll, proportional to deflection
 *     D-pad          arrow-key navigation (the only pad navigation source)
 *     A              left click
 *     B              Back (Alt+Left); exit when the desktop is empty
 *     X              Return
 *     Y              Escape
 *     L1 / R1        wheel up / wheel down, repeating while held
 *     L2 / R2        Page Up / Page Down, repeating while held
 *     L3 / R3        middle click / right click
 *     Select         show or hide the on-screen keyboard
 *     Start          focus the address bar   (Ctrl+L)
 *     Menu           TAP next window; HOLD dashboard task switcher
 *     Vol- / Vol+    zoom out / zoom in      (Ctrl+minus / Ctrl+plus)
 *     Home (F12)     the start page          (Alt+Home)
 *
 * The D-pad deliberately never moves the pointer.  It remains a digital arrow-key
 * source for applications and for the on-screen keyboard, while the left stick is
 * the only built-in pointer and the right stick is only a wheel.
 *
 * Menu is a hold and not a press because it is the only irreversible binding on
 * the pad and it sits under the thumb: a press would close the browser by
 * accident often enough to matter, and there is no undo for "the page you were
 * reading is gone".  Volume is bound to zoom rather than to volume because
 * nothing in this session makes a sound and 640x480 is small enough that a page
 * built for a phone needs one press of zoom-out to be readable.
 *
 * BOTH RESPONSE CURVES ARE SQUARED, and for the same reason: the slow end is where
 * a link is actually aimed at, so it gets most of the range.  On a stick that is
 * deflection squared -- a third of the way over is a ninth of the speed.  On the
 * D-pad, which has no deflection to measure, it is time-held squared over about a
 * second, reset on release, so a tap nudges and a hold crosses the panel.  The
 * remainder is kept between ticks -- it lives in the sub-pixel position, see
 * pointer_move() -- so that the slow end still moves at all; XTEST takes whole
 * pixels, and a per-tick step that rounds to zero is a pointer that does not move.
 *
 * THE POINTER IS PLACED AND NOT PUSHED, which is the other half of that: the
 * server accelerates a relative XTEST delta exactly as it accelerates a mouse's,
 * so the speeds above were arriving on the glass multiplied by something this file
 * had no say in.  pointer_move() says where instead, and the long comment there is
 * the whole of it.  The speeds themselves are a fraction of the SCREEN per second
 * rather than a count of pixels, because what a thumb judges is how much of the
 * panel went by.
 *
 * HOW IT ENDS.  Three ways, all of them tidy:
 *   - --watch PID and that process exits: the browser closed itself, so the
 *     session is over and this returns 0.
 *   - Menu held: SIGTERM to the watched pid, then return 0.  The session script
 *     is what actually tears the X server down; this only ever asks.
 *   - The X server goes away underneath us: the Xlib I/O error handler runs, and
 *     because Xlib's contract is that the handler must not return, it _exit()s
 *     after closing the pads.  That is the path taken when the session script is
 *     killed from outside.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <linux/input.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

/* ── shared shape ────────────────────────────────────────────────────────── */

/* Four is not a limit anyone will reach -- it is the built-in pad plus whatever
 * fits on the one USB port through a hub. */
#define MAX_PADS 4

/* The four axis roles this program has a use for.  Triggers and hats are not in
 * the list: a hat is read as the D-pad, and a trigger has no binding. */
#define AX_LX 0
#define AX_LY 1
#define AX_RX 2
#define AX_RY 3
#define AX_N  4

#define DIR_UP    0x1
#define DIR_DOWN  0x2
#define DIR_LEFT  0x4
#define DIR_RIGHT 0x8

static int    pad_fd[MAX_PADS];
static dev_t  pad_dev[MAX_PADS];      /* which device node, so a rescan skips it */
static char   pad_name[MAX_PADS][128];
static unsigned pad_dirs[MAX_PADS];   /* the D-pad, per pad: see drop_pad */

static int    ax_code[MAX_PADS][AX_N];    /* evdev code feeding this role, or -1 */
static double ax_centre[MAX_PADS][AX_N];
static double ax_half[MAX_PADS][AX_N];
static double ax_val[MAX_PADS][AX_N];     /* -1..1, deadzone already removed */

#define BITS_PER_LONG  (int)(sizeof(long) * 8)
#define NLONGS(x)      (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int verbose;

/* The two pieces of held-button state that are not per-pad.  They live here, and
 * the repeat clock is reached through a forward declaration, because drop_pad()
 * has to cancel both and it is defined long before either: see the comment there. */
static long menu_down_at;
static void rep_clear(void);
static unsigned dirs_all(void);
static void release_directions(unsigned dirs);

static void note(const char *fmt, ...)
{
    va_list ap;
    if (!verbose)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "j36-padx: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "j36-padx: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ── the pads ────────────────────────────────────────────────────────────── */

static void clear_pad(int slot)
{
    int r;

    pad_fd[slot] = -1;
    pad_dev[slot] = 0;
    pad_name[slot][0] = '\0';
    /* Both of these matter on the unplug path and not at startup.  A pad pulled
     * out mid-hold would otherwise leave its direction bit set for ever -- the
     * release event is on the wire that just left the socket -- and a stick pulled
     * out mid-deflection would leave the pointer travelling with nothing able to
     * stop it.  Per-pad state is what makes both of those a two-line reset. */
    pad_dirs[slot] = 0;
    for (r = 0; r < AX_N; r++) {
        ax_code[slot][r] = -1;
        ax_val[slot][r] = 0.0;
    }
}

static void drop_pad(int slot)
{
    unsigned held;
    if (pad_fd[slot] < 0)
        return;
    note("%s went away", pad_name[slot]);
    ioctl(pad_fd[slot], EVIOCGRAB, 0);
    close(pad_fd[slot]);
    held = pad_dirs[slot];
    clear_pad(slot);
    release_directions(held & ~dirs_all());

    /*
     * Cancel every held button, and not only this pad's.
     *
     * A release event arrives on the wire that has just been unplugged, so a
     * shoulder held at the moment the cable came out never gets one -- and the
     * auto-repeat below has no owner to attribute it to, so it would keep sending
     * Page Down until the session ended.  Menu is the same shape with a worse
     * result: held at the wrong moment, it would close the browser a second after
     * the pad was pulled out.
     *
     * Cancelling a second pad's genuine hold along with it is the cost, and it is
     * the right trade: the worst case there is one button that has to be pressed
     * again, against a page that scrolls for ever on its own.
     */
    rep_clear();
    menu_down_at = 0;
}

static void close_pads(void)
{
    int i;
    for (i = 0; i < MAX_PADS; i++) {
        if (pad_fd[i] < 0)
            continue;
        /* The ungrab is explicit though close() would do it: it is the thing that
         * gives the dashboard its buttons back, and it should be visible on the
         * page rather than implied by a close. */
        ioctl(pad_fd[i], EVIOCGRAB, 0);
        close(pad_fd[i]);
        clear_pad(i);
    }
}

/*
 * Take the pad, or give it back, without closing anything.
 *
 * ADDED FOR THE TASK SWITCHER, and it is worth saying why a grab has to be
 * temporary now.  Holding Menu used to close this session outright; it asks
 * mixdash for its switcher instead (see the hand-over in main()), and mixdash
 * cannot show a switcher it is unable to drive.  The grab is what stops it
 * reading the pad, and a grab is a property of the open descriptor -- it survives
 * this process being SIGSTOP'd along with the rest of the session, so "we are
 * stopped, so we are not using it" is not something the kernel knows.  It has to
 * be handed back explicitly, before the stop, and taken again afterwards.
 *
 * A failure is ignored on purpose: the ungrab side cannot meaningfully fail, and
 * on the way back in something else having taken the device is the same case
 * scan_pads() already tolerates -- the session keeps working, and the dashboard
 * sees the buttons too.
 */
static void set_grab(int on)
{
    int i;
    for (i = 0; i < MAX_PADS; i++)
        if (pad_fd[i] >= 0)
            (void)ioctl(pad_fd[i], EVIOCGRAB, on ? 1 : 0);
}

static int pad_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_PADS; i++) {
        if (pad_fd[i] >= 0)
            n++;
    }
    return n;
}

/*
 * Is this /dev/input/eventN a pad?
 *
 * Positive test: it carries BTN_A and BTN_START.  Together those are what this
 * board's key map puts on the built-in pad and what xpad, hid-sony,
 * hid-playstation, hid-nintendo and hid-generic all put on a USB one, and nothing
 * else on this device has either.  A touchscreen or a tablet has ABS_X and
 * BTN_TOUCH and neither of these, so it is excluded here without a rule of its own.
 *
 * Negative tests, and they matter more than the positive one, because what is
 * being decided is whether to take a device away from the rest of the system:
 *   - EV_REL means a mouse, and a mouse in the dock is X's to drive.
 *   - KEY_A and KEY_Z mean a real keyboard, and so is that.
 *
 * EV_ABS is NOT one of them, and that is the whole point: j36_mt6592_input.c
 * registers the analog sticks on the SAME input device as the buttons -- one
 * input_allocate_device, input_set_capability(EV_KEY) for every mapped code and
 * input_set_abs_params() for every axis the device tree declares -- so a rule that
 * rejected axes would reject the one device this needs, on every board whose DTS
 * has a joystick.
 */
static int looks_like_pad(int fd, const char *name)
{
    unsigned long keys[NLONGS(KEY_MAX + 1)];
    unsigned long evs[NLONGS(EV_MAX + 1)];

    memset(keys, 0, sizeof(keys));
    memset(evs, 0, sizeof(evs));

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evs)), evs) < 0)
        return 0;
    if (!TEST_BIT(EV_KEY, evs))
        return 0;
    if (TEST_BIT(EV_REL, evs)) {
        note("%s has EV_REL, so it is a mouse -- left for X", name);
        return 0;
    }

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
        return 0;
    if (TEST_BIT(KEY_A, keys) && TEST_BIT(KEY_Z, keys)) {
        note("%s has the letter keys, so it is a keyboard -- left for X", name);
        return 0;
    }
    if (!TEST_BIT(BTN_A, keys) || !TEST_BIT(BTN_START, keys))
        return 0;

    return 1;
}

/*
 * Learn one axis from the device rather than from a constant.
 *
 * min and max give the centre and the half-travel, which is what makes the same
 * arithmetic work for -4096..4096, -32768..32767 and 0..255 without knowing which
 * pad is on the other end.  `flat' is deliberately not applied here: mixdash does
 * not apply that hint either, and both paths use the same Mouse deadzone setting.
 *
 * The current value is deliberately NOT read into ax_val.  A stick at rest sends
 * no events, so zero is the honest starting point; seeding from EVIOCGABS would
 * mean a pad that happened to be pushed over at startup left the pointer drifting
 * until it was touched.
 */
static int learn_axis(int slot, int role, int fd, int code)
{
    struct input_absinfo ai;
    double half;

    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return 0;
    if (ai.maximum <= ai.minimum)
        return 0;

    half = ((double)ai.maximum - (double)ai.minimum) / 2.0;
    ax_centre[slot][role] = ((double)ai.maximum + (double)ai.minimum) / 2.0;
    ax_half[slot][role]   = half;
    ax_code[slot][role]   = code;
    ax_val[slot][role]    = 0.0;
    return 1;
}

/*
 * Which axes this pad has, and which of them are the two sticks.
 *
 * The left stick is ABS_X/ABS_Y on everything, so it needs no rule.  The right
 * one does: ABS_RX/ABS_RY is what Documentation/input/gamepad.rst says and what
 * xpad, hid-playstation and hid-nintendo follow, while this board's device tree
 * and hid-sony's DualShock 3 both put it on ABS_Z/ABS_RZ.  Preferring RX/RY when
 * the pair exists resolves that in the only direction that is safe -- on a pad
 * that has both, ABS_Z and ABS_RZ are the analog triggers, and reading them as a
 * stick would scroll the page whenever a trigger was pulled.
 */
static void learn_axes(int slot, int fd)
{
    unsigned long abs[NLONGS(ABS_MAX + 1)];

    memset(abs, 0, sizeof(abs));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
        return;

    if (TEST_BIT(ABS_X, abs))
        learn_axis(slot, AX_LX, fd, ABS_X);
    if (TEST_BIT(ABS_Y, abs))
        learn_axis(slot, AX_LY, fd, ABS_Y);

    if (TEST_BIT(ABS_RX, abs) && TEST_BIT(ABS_RY, abs)) {
        learn_axis(slot, AX_RX, fd, ABS_RX);
        learn_axis(slot, AX_RY, fd, ABS_RY);
    } else if (TEST_BIT(ABS_Z, abs) && TEST_BIT(ABS_RZ, abs)) {
        learn_axis(slot, AX_RX, fd, ABS_Z);
        learn_axis(slot, AX_RY, fd, ABS_RZ);
    }

    note("%s: left stick %s, right stick %s", pad_name[slot],
         ax_code[slot][AX_LX] >= 0 ? "yes" : "none",
         ax_code[slot][AX_RX] >= 0 ? "yes" : "none");
}

static int already_open(dev_t rdev)
{
    int i;
    for (i = 0; i < MAX_PADS; i++) {
        if (pad_fd[i] >= 0 && pad_dev[i] == rdev)
            return 1;
    }
    return 0;
}

/*
 * Fill any free slot from /dev/input.  Called once at startup and then every
 * couple of seconds, which is what makes a pad plugged in halfway through a page
 * work; there is no udev in this session to be told by, and an opendir of a
 * directory with a dozen entries costs less than the poll it replaces.
 *
 * Devices are remembered by st_rdev rather than by path, because event node
 * numbers are reused: unplug the pad on event4 and plug in another and it is
 * event4 again, with a different major:minor.  Comparing paths would either miss
 * the new device or re-open the one already held.
 */
static int scan_pads(int grab)
{
    DIR *d;
    struct dirent *e;
    struct stat st;
    char path[320];
    char name[128];
    int fd, slot, found = 0;

    if (pad_count() >= MAX_PADS)
        return 0;

    d = opendir("/dev/input");
    if (!d) {
        fail("no /dev/input: %s", strerror(errno));
        return 0;
    }

    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        if (pad_count() >= MAX_PADS)
            break;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (fstat(fd, &st) < 0 || already_open(st.st_rdev)) {
            close(fd);
            continue;
        }
        name[0] = '\0';
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            /*
             * The node has no name of its own, so it is called after its path.
             *
             * The precision is what keeps -Wformat-truncation quiet, and it is not
             * only to keep it quiet: path is 320 bytes and name is 128, so gcc is
             * right that the copy can be cut short.  Saying where it is cut makes
             * the truncation the one this code chose rather than the one snprintf
             * happened to do, and a driverless pad is named "/dev/input/eventN"
             * either way -- the case that would actually overflow is a d_name two
             * hundred characters long, which /dev/input does not have.
             */
            snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, path);
        }
        name[sizeof(name) - 1] = '\0';

        if (!looks_like_pad(fd, name)) {
            close(fd);
            continue;
        }

        for (slot = 0; slot < MAX_PADS && pad_fd[slot] >= 0; slot++)
            ;
        if (slot == MAX_PADS) {
            close(fd);
            break;
        }

        /*
         * The grab can fail -- something else got here first -- and that is not
         * fatal.  Events still arrive on this fd; they simply also arrive
         * wherever the other reader is, which is a dashboard walking its grid
         * behind the browser.  Worth saying out loud and worth carrying on for,
         * because a browser with a working pointer and a confused menu behind it
         * is better than no browser.
         */
        if (grab && ioctl(fd, EVIOCGRAB, 1) < 0)
            fail("could not take %s for this session (%s); presses may also reach "
                 "whatever else has it open", name, strerror(errno));

        clear_pad(slot);
        pad_fd[slot] = fd;
        pad_dev[slot] = st.st_rdev;
        snprintf(pad_name[slot], sizeof(pad_name[slot]), "%s", name);
        learn_axes(slot, fd);
        note("using %s (%s)", path, name);
        found++;
    }

    closedir(d);
    return found;
}

/* ── the clock ───────────────────────────────────────────────────────────── */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ── X ───────────────────────────────────────────────────────────────────── */

static Display *dpy;
static int      scr;              /* the one screen this session has */
static int      scr_w, scr_h;     /* and its size, which sets every speed below */
static int      kbd_visible;
static int      kbd_menu_hidden;  /* restored only when Menu proved to be a tap */

/* The dashboard-equivalent response, loaded once after the X screen is known. */
static double   stick_max;
static double   stick_acceleration = 45.0;
static double   stick_deadzone = 0.16;

#define POINTER_STATE "/run/j36/pointer.state"

/*
 * X errors are reported and not fatal, because the only calls here that can raise
 * one are the window walk in kbd_win_find() and the send that follows it, and both
 * race with a keyboard that can exit between the two.  A BadWindow there is the
 * ordinary answer to "is it still there", not a bug: Xlib's default handler would
 * print it and carry on anyway, so this only stops it printing when nobody asked.
 */
static int on_x_error(Display *d, XErrorEvent *e)
{
    char buf[128];
    if (!verbose)
        return 0;
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    note("X error: %s (request %d)", buf, (int)e->request_code);
    return 0;
}

/* The keycodes are resolved once, at startup, because XKeysymToKeycode walks the
 * server's whole keymap and the answer cannot change without a keymap change --
 * and there is no keymap change in this session, since the only keyboard driving
 * it is this program. */
static struct {
    KeyCode ret, esc, page_up, page_down, home, left, right, up, down;
    KeyCode alt, ctrl, shift, minus, plus, l;
} kc;

static KeyCode want(KeySym sym, const char *what)
{
    KeyCode c = XKeysymToKeycode(dpy, sym);
    if (!c)
        fail("this X server's keymap has no %s, so that binding does nothing", what);
    return c;
}

static void resolve_keys(void)
{
    kc.ret       = want(XK_Return,    "Return");
    kc.esc       = want(XK_Escape,    "Escape");
    kc.page_up   = want(XK_Page_Up,   "Page Up");
    kc.page_down = want(XK_Page_Down, "Page Down");
    kc.home      = want(XK_Home,      "Home");
    kc.left      = want(XK_Left,      "Left");
    kc.right     = want(XK_Right,     "Right");
    kc.up        = want(XK_Up,        "Up");
    kc.down      = want(XK_Down,      "Down");
    kc.alt       = want(XK_Alt_L,     "Alt");
    kc.ctrl      = want(XK_Control_L, "Control");
    kc.shift     = want(XK_Shift_L,   "Shift");
    kc.minus     = want(XK_minus,     "minus");
    kc.plus      = want(XK_plus,      "plus");
    kc.l         = want(XK_l,         "L");
}

static void tap(KeyCode mod, KeyCode key)
{
    if (!key)
        return;
    if (mod)
        XTestFakeKeyEvent(dpy, mod, True, 0);
    XTestFakeKeyEvent(dpy, key, True, 0);
    XTestFakeKeyEvent(dpy, key, False, 0);
    if (mod)
        XTestFakeKeyEvent(dpy, mod, False, 0);
    XFlush(dpy);
}

static void key_state(KeyCode key, int down)
{
    if (!key)
        return;
    XTestFakeKeyEvent(dpy, key, down ? True : False, 0);
    XFlush(dpy);
}

static void release_directions(unsigned dirs)
{
    if (!dpy || kbd_visible)
        return;
    if (dirs & DIR_UP) key_state(kc.up, 0);
    if (dirs & DIR_DOWN) key_state(kc.down, 0);
    if (dirs & DIR_LEFT) key_state(kc.left, 0);
    if (dirs & DIR_RIGHT) key_state(kc.right, 0);
}

/* A session card owns the PID of the shell that launched its command.  Most small
 * clients exec in place, but browsers commonly leave that shell waiting while a
 * child owns the X window.  Walk /proc rather than requiring those two PIDs to be
 * identical, so the card still names the whole launched client. */
static int pid_belongs_to(unsigned long candidate, unsigned long launcher)
{
    int depth;

    for (depth = 0; candidate > 1 && depth < 64; depth++) {
        char path[64], stat[512], *end;
        char state;
        long parent;
        int fd;
        ssize_t got;

        if (candidate == launcher)
            return 1;
        snprintf(path, sizeof path, "/proc/%lu/stat", candidate);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            break;
        got = read(fd, stat, sizeof stat - 1);
        close(fd);
        if (got <= 0)
            break;
        stat[got] = '\0';
        /* comm is parenthesised and may itself contain spaces or parentheses;
         * everything following its last ')' begins with state and PPID. */
        end = strrchr(stat, ')');
        if (!end || sscanf(end + 1, " %c %ld", &state, &parent) != 2
            || parent <= 0 || (unsigned long)parent == candidate)
            break;
        candidate = (unsigned long)parent;
    }
    return 0;
}

/* Find the client carrying the launcher's _NET_WM_PID (or a descendant's), even
 * when matchbox has reparented the X window. */
static Window window_for_pid(Window at, unsigned long wanted, int depth)
{
    Atom atom, actual;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    Window root, parent, *children = NULL;
    unsigned count = 0, i;

    if (depth > 8)
        return None;
    atom = XInternAtom(dpy, "_NET_WM_PID", True);
    if (atom != None
        && XGetWindowProperty(dpy, at, atom, 0, 1, False, XA_CARDINAL,
                              &actual, &format, &nitems, &after, &data) == Success) {
        if (data && actual == XA_CARDINAL && format == 32 && nitems == 1
            && pid_belongs_to(*(unsigned long *)data, wanted)) {
            XFree(data);
            return at;
        }
        if (data)
            XFree(data);
    }

    if (!XQueryTree(dpy, at, &root, &parent, &children, &count))
        return None;
    for (i = 0; i < count; i++) {
        Window hit = window_for_pid(children[i], wanted, depth + 1);
        if (hit != None) {
            XFree(children);
            return hit;
        }
    }
    if (children)
        XFree(children);
    return None;
}

static int focus_client(pid_t pid)
{
    Window root = RootWindow(dpy, scr);
    Window client = window_for_pid(root, (unsigned long)pid, 0);
    XEvent ev;
    Atom active;

    if (client == None)
        return 0;
    active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = client;
    ev.xclient.message_type = active;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2; /* pager: this request came from the Desktop page */
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XRaiseWindow(dpy, client);
    XSetInputFocus(dpy, client, RevertToPointerRoot, CurrentTime);
    XFlush(dpy);
    return 1;
}

/* ── the screen ──────────────────────────────────────────────────────────── */

/*
 * ── PUTTING BACK A SCREEN X DOES NOT KNOW IT LOST ───────────────────────────
 *
 * There is one framebuffer on this board and two programs draw into it: this
 * session's X server through xf86-video-fbdev, and mixdash through Qt's linuxfb
 * platform.  They take turns -- whichever task is in front draws, the rest of the
 * group is SIGSTOPped -- and taking turns is enough for the pixels ONLY because
 * mixdash copies the panel out before it stops a task and copies it back before
 * it continues one.  See Panel::grab() and Panel::restore().
 *
 * WHAT THAT CANNOT COVER.  The copy back is a memcpy into /dev/fb0, and the X
 * server is not looking at /dev/fb0: fbdev runs with ShadowFB, so X draws into
 * its own buffer in ordinary memory and copies out only the rectangles it thinks
 * changed.  Nothing mixdash does to the framebuffer is a change as far as X is
 * concerned.  So anything mixdash left behind that the restore did not cover --
 * the dashboard's own pointer, a toast, the whole screen when there was no saved
 * frame to put back because the session had not drawn one yet -- stays on the
 * glass until some client happens to repaint over it.  That is the trail the user
 * sees, and it is why a desktop that is really running can look black.
 *
 * ASKING FOR THE REPAINT IS THE FIX, and X has had the mechanism since X10: a
 * window with no background, mapped over everything and immediately destroyed.
 * Mapping it changes not one pixel, because a background of None means the server
 * paints nothing; destroying it uncovers everything underneath, and uncovering is
 * exposure, so every client is told to redraw the part of itself that was behind
 * it.  Their drawing is damage, damage is what ShadowFB copies, and the copy is
 * what reaches the panel.  This is what xrefresh(1) is and does; it is written out
 * here because x11-xserver-utils is not on this image and would be a package for
 * one twelve-line function.
 *
 * The root is cleared as well, for the pixels no window covers -- that is where
 * the card desktop_paint() hangs on the root background gets put back.
 */
static void screen_refresh(void)
{
    XSetWindowAttributes at;
    Window w;

    if (!dpy || scr_w < 1 || scr_h < 1)
        return;

    XClearWindow(dpy, RootWindow(dpy, scr));

    /* override_redirect because this is not a window in any sense the window
     * manager should hear about -- it exists for two round trips and matchbox
     * would otherwise try to decorate and stack it. */
    at.override_redirect = True;
    at.background_pixmap = None;
    at.backing_store     = NotUseful;
    at.save_under        = False;
    w = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0,
                      (unsigned)scr_w, (unsigned)scr_h, 0,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWOverrideRedirect | CWBackPixmap | CWBackingStore | CWSaveUnder,
                      &at);
    XMapRaised(dpy, w);
    XDestroyWindow(dpy, w);
    XFlush(dpy);
}

/*
 * The X cursor is not part of the framebuffer snapshot mixdash saves.  With
 * xf86-video-fbdev it is composited by the X server and the pixels underneath it
 * are restored only when X moves or hides it.  SIGSTOP freezes the whole session,
 * including that cleanup, so handing the panel to the task switcher while the
 * cursor is visible leaves the cursor painted over the switcher's first frame.
 *
 * XFixes hides the server cursor globally, including cursors a client installed
 * on its own window.  XSync is intentional: the dashboard may stop X immediately
 * after SIGUSR1, so the hide must have reached the server before that signal is
 * sent.  The cursor is restored on SIGCONT, before the refresh which repaints the
 * resumed desktop.
 */
static void screen_cursor(int visible)
{
    static int available = -1;
    static int hidden;
    int event_base, error_base;

    if (!dpy)
        return;
    if (available < 0)
        available = XFixesQueryExtension(dpy, &event_base, &error_base) ? 1 : 0;
    if (!available)
        return;

    if (visible) {
        if (!hidden)
            return;
        XFixesShowCursor(dpy, RootWindow(dpy, scr));
        hidden = 0;
    } else {
        if (hidden)
            return;
        XFixesHideCursor(dpy, RootWindow(dpy, scr));
        hidden = 1;
    }
    XSync(dpy, False);
}

/*
 * ── THE DESKTOP IS NOT BLACK ANY MORE ───────────────────────────────────────
 *
 * A session with no window open shows the root, and an X root with nothing set on
 * it is black.  That is correct and it is also indistinguishable from a session
 * that failed to start, which is exactly what got reported: "Desktop is black".
 * A person holding this device has no way to tell the two apart and no reason to
 * guess -- there is no title bar, no taskbar and no menu to click, because the
 * pad is the only input and every gesture it has is invisible.
 *
 * So the root gets a card that says what the pad does.  It is a PIXMAP HUNG ON
 * THE ROOT'S BACKGROUND rather than a window: the server then repaints it by
 * itself, for free, whenever a window moves off it or screen_refresh() clears it,
 * and there is no extra client to stack, focus, stop or kill.  This is what
 * xsetroot -bitmap does, and the reason the pixmap can be freed on the next line
 * is the same -- the server keeps its own reference for as long as a window uses
 * it as a background.
 *
 * WHY THIS PROGRAM DRAWS IT.  j36-padx is already an X client in every session,
 * already links -lX11, and already knows the whole binding table because it is
 * the thing that implements it -- the list below and the switch in main() cannot
 * drift apart without somebody editing both.  A separate program would be a new
 * binary, a new package and a second copy of the truth.
 *
 * CORE FONTS, and no fontconfig.  10x20 and 9x15 come from xfonts-base, which is
 * on this image because the X server refuses to start without `fixed'.  If some
 * future image drops them the card degrades to its background and a session with
 * no window is a plain dark screen instead of a black one, which is still a
 * better answer than nothing.
 */
static const struct {
    const char *key;
    const char *what;
} help_rows[] = {
    { "FN held",       "the task switcher -- and the way back to the dashboard" },
    { "FN tapped",     "the next window" },
    { "Select",        "the on-screen keyboard" },
    { "Left stick",    "the pointer" },
    { "D-pad",         "arrow-key navigation" },
    { "A",             "click  (L3 and R3 are the middle and right buttons)" },
    { "B",             "back -- or exit when the desktop is empty" },
    { "X, Y",          "Enter, Escape" },
    { "L1, R1",        "scroll  (so does the right stick)" },
    { "L2, R2",        "page up, page down" },
    { "Start",         "the address bar" },
    { "Vol -, Vol +",  "zoom out, zoom in" },
};

static unsigned long card_colour(Colormap cm, const char *spec, unsigned long fallback)
{
    XColor c;

    if (XParseColor(dpy, cm, spec, &c) && XAllocColor(dpy, cm, &c))
        return c.pixel;
    return fallback;
}

static XFontStruct *card_font(const char *const *names)
{
    XFontStruct *f;
    int i;

    for (i = 0; names[i]; i++) {
        f = XLoadQueryFont(dpy, names[i]);
        if (f)
            return f;
    }
    return NULL;
}

static void desktop_paint(void)
{
    static const char *const big_names[]  = { "10x20", "9x15bold", "9x15", "fixed", NULL };
    static const char *const body_names[] = { "9x15", "8x13", "fixed", NULL };
    const char *title = "MixOS desktop";
    const char *lead  = "Windows open on top of this.  The pad drives them:";
    const char *foot  = "j36-xrun COMMAND, from the dashboard's Terminal, opens a window here";
    Window root;
    Pixmap pm;
    GC gc;
    Colormap cm;
    XFontStruct *big, *body;
    unsigned long bg, fg, key_fg, dim;
    int rows = (int)(sizeof help_rows / sizeof help_rows[0]);
    int line_h, key_w, block_h, x, y, i;

    if (!dpy || scr_w < 1 || scr_h < 1)
        return;

    root = RootWindow(dpy, scr);
    cm   = DefaultColormap(dpy, scr);

    bg     = card_colour(cm, "#0f131a", BlackPixel(dpy, scr));
    fg     = card_colour(cm, "#e6edf7", WhitePixel(dpy, scr));
    key_fg = card_colour(cm, "#78b0ff", WhitePixel(dpy, scr));
    dim    = card_colour(cm, "#9aa7b8", WhitePixel(dpy, scr));

    pm = XCreatePixmap(dpy, root, (unsigned)scr_w, (unsigned)scr_h,
                       (unsigned)DefaultDepth(dpy, scr));
    gc = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, pm, gc, 0, 0, (unsigned)scr_w, (unsigned)scr_h);

    big  = card_font(big_names);
    body = card_font(body_names);
    if (!body)
        body = big;
    if (!big)
        big = body;

    if (body) {
        line_h = body->ascent + body->descent + 4;

        /* The key column is as wide as the widest key and not one pixel more, so
         * the two columns line up at whatever size the fonts turn out to be. */
        key_w = 0;
        for (i = 0; i < rows; i++) {
            int w = XTextWidth(body, help_rows[i].key, (int)strlen(help_rows[i].key));
            if (w > key_w)
                key_w = w;
        }

        block_h = line_h * (rows + 4);
        x = scr_w / 12;
        y = (scr_h - block_h) / 2;
        if (y < line_h * 2)
            y = line_h * 2;
        y += big->ascent;

        XSetFont(dpy, gc, big->fid);
        XSetForeground(dpy, gc, fg);
        XDrawString(dpy, pm, gc, x, y, title, (int)strlen(title));
        y += line_h + big->descent;

        XSetFont(dpy, gc, body->fid);
        XSetForeground(dpy, gc, dim);
        XDrawString(dpy, pm, gc, x, y, lead, (int)strlen(lead));
        y += line_h * 2;

        for (i = 0; i < rows; i++) {
            XSetForeground(dpy, gc, key_fg);
            XDrawString(dpy, pm, gc, x, y, help_rows[i].key,
                        (int)strlen(help_rows[i].key));
            XSetForeground(dpy, gc, fg);
            XDrawString(dpy, pm, gc, x + key_w + line_h, y, help_rows[i].what,
                        (int)strlen(help_rows[i].what));
            y += line_h;
        }

        XSetForeground(dpy, gc, dim);
        XDrawString(dpy, pm, gc, x, y + line_h, foot, (int)strlen(foot));
    }

    XSetWindowBackgroundPixmap(dpy, root, pm);
    XClearWindow(dpy, root);

    /* The server has its own reference now; this only gives up ours.  The fonts
     * go the same way -- the text is already pixels in the pixmap. */
    XFreePixmap(dpy, pm);
    XFreeGC(dpy, gc);
    if (big && big != body)
        XFreeFont(dpy, big);
    if (body)
        XFreeFont(dpy, body);

    /* The card outlives this connection.  Without it, a j36-padx that is killed
     * and restarted -- or that crashes -- takes the root background with it and
     * leaves the black screen this exists to prevent. */
    XSetCloseDownMode(dpy, RetainPermanent);
    XFlush(dpy);
}

/* ── where the pointer is ────────────────────────────────────────────────── */

/*
 * THE POINTER IS PLACED, NOT PUSHED, and that is a bug fix and not a preference.
 *
 * This used XTestFakeRelativeMotionEvent, which hands the server a delta -- and
 * the server puts that delta through the same pointer acceleration a real mouse's
 * goes through.  Xext/xtest.c asks for POINTER_RELATIVE without POINTER_NORAW, so
 * dix/getevents.c runs accelPointer() over it, and the XTEST device is created
 * with the server's default profile: threshold 4, accel 2.  One tick of a stick
 * pushed anywhere past halfway is 18 pixels, which is four times the threshold, so
 * the pointer moved at about twice the speed this file asked for -- and every
 * curve and ramp below was being read through a multiplier it knew nothing about.
 * That is what "faster than the mouse" was.
 *
 * XTestFakeMotionEvent takes a position instead, and a position is not
 * accelerated: it arrives at the screen as the number given.  So the position is
 * kept here, in doubles, and the fraction that used to be carried between ticks is
 * carried by the position itself.  Two things fall out of that for free, and both
 * are wanted.  The screen edges become a clamp, so the pointer cannot be driven
 * off a panel that has no second screen to sweep it back from.  And a real mouse
 * in the dock stays in charge of where it left the pointer, because the server is
 * asked before every move: if the pointer is not where this program last put it,
 * somebody else moved it, and that is the position to move on from.
 */
static double ptr_x, ptr_y;         /* where it is, to the sub-pixel */
static int    sent_x, sent_y;       /* the last whole position asked for */

static int pointer_state_read(int *x, int *y)
{
    char buf[64];
    int fd = open(POINTER_STATE, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return sscanf(buf, "%d %d", x, y) == 2;
}

static void pointer_state_write(void)
{
    char buf[64];
    int fd, n;
    (void)mkdir("/run/j36", 0755);
    fd = open(POINTER_STATE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    n = snprintf(buf, sizeof(buf), "%d %d\n", sent_x, sent_y);
    if (n > 0)
        (void)write(fd, buf, (size_t)n);
    close(fd);
}

static void pointer_resync(void)
{
    Window root_ret, child_ret;
    int rx, ry, wx, wy;
    unsigned mask;

    if (!XQueryPointer(dpy, RootWindow(dpy, scr), &root_ret, &child_ret,
                       &rx, &ry, &wx, &wy, &mask))
        return;
    if (rx == sent_x && ry == sent_y)
        return;                     /* exactly where this put it: keep the fraction */
    ptr_x = (double)rx;
    ptr_y = (double)ry;
    sent_x = rx;
    sent_y = ry;
    pointer_state_write();
}

static void pointer_move(double dx, double dy)
{
    int ix, iy;

    if (dx == 0.0 && dy == 0.0)
        return;

    pointer_resync();
    ptr_x += dx;
    ptr_y += dy;
    if (ptr_x < 0.0)
        ptr_x = 0.0;
    else if (ptr_x > (double)(scr_w - 1))
        ptr_x = (double)(scr_w - 1);
    if (ptr_y < 0.0)
        ptr_y = 0.0;
    else if (ptr_y > (double)(scr_h - 1))
        ptr_y = (double)(scr_h - 1);

    ix = (int)ptr_x;
    iy = (int)ptr_y;
    /* Below a pixel per tick this is the slow end doing its job, not a stall: the
     * fraction stays in ptr_x/ptr_y and the move happens a few ticks later. */
    if (ix == sent_x && iy == sent_y)
        return;
    XTestFakeMotionEvent(dpy, scr, ix, iy, 0);
    XFlush(dpy);
    sent_x = ix;
    sent_y = iy;
    pointer_state_write();
}

/* The dashboard arrow is a white classic pointer with a dark outline.  X's core
 * cursor is two-colour, which is exactly enough to draw the same body: the mask
 * is the outline and the source is the white inset. */
static void pointer_install_cursor(void)
{
    Window root = RootWindow(dpy, scr);
    Pixmap source = XCreatePixmap(dpy, root, 20, 28, 1);
    Pixmap mask = XCreatePixmap(dpy, root, 20, 28, 1);
    GC gc = XCreateGC(dpy, mask, 0, NULL);
    XPoint outer[] = {{0,0},{0,21},{5,16},{8,24},{13,22},{9,14},{16,14}};
    XPoint inner[] = {{2,3},{2,17},{5,13},{9,21},{10,20},{7,12},{12,12}};
    XColor white, dark;
    Cursor cursor;

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, mask, gc, 0, 0, 20, 28);
    XFillRectangle(dpy, source, gc, 0, 0, 20, 28);
    XSetForeground(dpy, gc, 1);
    XFillPolygon(dpy, mask, gc, outer, 7, Complex, CoordModeOrigin);
    XFillPolygon(dpy, source, gc, inner, 7, Complex, CoordModeOrigin);

    memset(&white, 0, sizeof(white));
    white.red = white.green = white.blue = 65535;
    memset(&dark, 0, sizeof(dark));
    dark.red = 24 * 257;
    dark.green = 26 * 257;
    dark.blue = 34 * 257;
    cursor = XCreatePixmapCursor(dpy, source, mask, &white, &dark, 0, 0);
    XDefineCursor(dpy, root, cursor);
    /* Replace the common names too, so a client asking for the ordinary arrow
     * does not resurrect the server's unrelated default shape over its window. */
    XFixesChangeCursorByName(dpy, cursor, "left_ptr");
    XFixesChangeCursorByName(dpy, cursor, "default");
    XFreeCursor(dpy, cursor);
    XFreeGC(dpy, gc);
    XFreePixmap(dpy, source);
    XFreePixmap(dpy, mask);
    XFlush(dpy);
}

static void click(unsigned button, int press)
{
    XTestFakeButtonEvent(dpy, button, press ? True : False, 0);
    XFlush(dpy);
}

static void wheel(unsigned button)
{
    XTestFakeButtonEvent(dpy, button, True, 0);
    XTestFakeButtonEvent(dpy, button, False, 0);
    XFlush(dpy);
}

/* ── the shared-style on-screen keyboard ─────────────────────────────────── */

/* Matchbox's keyboard could share the dashboard layout, but not its appearance,
 * selection model or pointer behaviour.  This small X window is therefore drawn
 * by the same bridge that already synthesises its keys.  Its five rows, palette,
 * D-pad focus and pointer click model mirror mixdash/keyboard.cpp. */
enum { KA_CHAR, KA_SHIFT, KA_BACK, KA_SYMBOLS, KA_SPACE,
       KA_LEFT, KA_RIGHT, KA_ESCAPE, KA_ENTER };

struct KbdCap {
    char label[12];
    KeySym sym;
    int action;
    double span;
};

#define KBD_ROWS 5
#define KBD_COLS 12
static struct KbdCap kbd_caps[KBD_ROWS][KBD_COLS];
static int kbd_count[KBD_ROWS];
static Window kbd_win;
static GC kbd_gc;
static XFontStruct *kbd_font;
static int kbd_row = 1, kbd_col;
static int kbd_upper, kbd_caps_lock, kbd_symbols;
static int kbd_pressed = -1;
static int kbd_h;

static void kbd_add(int row, const char *label, KeySym sym, int action, double span)
{
    struct KbdCap *c;
    if (row < 0 || row >= KBD_ROWS || kbd_count[row] >= KBD_COLS)
        return;
    c = &kbd_caps[row][kbd_count[row]++];
    snprintf(c->label, sizeof(c->label), "%s", label);
    c->sym = sym;
    c->action = action;
    c->span = span;
}

static void kbd_chars(int row, const char *chars)
{
    char s[2] = {0, 0};
    while (*chars) {
        s[0] = *chars++;
        kbd_add(row, s, XStringToKeysym(s), KA_CHAR, 1.0);
    }
}

static void kbd_build(void)
{
    int i;
    const char *lower[KBD_ROWS - 1] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
    };
    const char *upper[KBD_ROWS - 1] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    const char *symbols[KBD_ROWS - 1] = {
        "!@#$%^&*()", "-_=+[]{}|\\", ":;'\",.<>/?", "`~"
    };
    const char **layer = kbd_symbols ? symbols : (kbd_upper || kbd_caps_lock) ? upper : lower;

    memset(kbd_count, 0, sizeof(kbd_count));
    for (i = 0; i < 3; i++)
        kbd_chars(i, layer[i]);
    kbd_add(3, kbd_caps_lock ? "CAPS" : kbd_upper ? "SHIFT" : "shift",
            NoSymbol, KA_SHIFT, 1.5);
    kbd_chars(3, layer[3]);
    kbd_add(3, "back", XK_BackSpace, KA_BACK, 1.5);

    kbd_add(4, kbd_symbols ? "abc" : "?123", NoSymbol, KA_SYMBOLS, 1.6);
    kbd_add(4, "<", XK_Left, KA_LEFT, 0.9);
    kbd_add(4, "space", XK_space, KA_SPACE, 4.0);
    kbd_add(4, ">", XK_Right, KA_RIGHT, 0.9);
    kbd_add(4, "esc", XK_Escape, KA_ESCAPE, 1.6);
    kbd_add(4, "enter", XK_Return, KA_ENTER, 1.6);
    if (kbd_col >= kbd_count[kbd_row])
        kbd_col = kbd_count[kbd_row] - 1;
}

static void kbd_rect(int row, int col, int *x, int *y, int *w, int *h)
{
    const int pad = 8, gap = 4;
    double spans = 0.0, before = 0.0, unit;
    int i;
    for (i = 0; i < kbd_count[row]; i++) {
        spans += kbd_caps[row][i].span;
        if (i < col)
            before += kbd_caps[row][i].span;
    }
    unit = (scr_w - 2 * pad - (kbd_count[row] - 1) * gap) / spans;
    *h = (kbd_h - 2 * pad - (KBD_ROWS - 1) * gap) / KBD_ROWS;
    *x = pad + (int)(before * unit) + col * gap;
    *y = pad + row * (*h + gap);
    *w = (int)(kbd_caps[row][col].span * unit);
}

static unsigned long kbd_colour(const char *name, unsigned long fallback)
{
    XColor c;
    Colormap cm = DefaultColormap(dpy, scr);
    return XParseColor(dpy, cm, name, &c) && XAllocColor(dpy, cm, &c)
               ? c.pixel : fallback;
}

static void kbd_paint(void)
{
    int r, c;
    unsigned long back = kbd_colour("#11141c", BlackPixel(dpy, scr));
    unsigned long card = kbd_colour("#292e3a", WhitePixel(dpy, scr));
    unsigned long edge = kbd_colour("#4a5263", WhitePixel(dpy, scr));
    unsigned long ink = kbd_colour("#f4f6fb", WhitePixel(dpy, scr));
    unsigned long blue = kbd_colour("#397de5", WhitePixel(dpy, scr));
    unsigned long teal = kbd_colour("#25a9a0", WhitePixel(dpy, scr));

    if (!kbd_win)
        return;
    XSetForeground(dpy, kbd_gc, back);
    XFillRectangle(dpy, kbd_win, kbd_gc, 0, 0, (unsigned)scr_w, (unsigned)kbd_h);
    if (kbd_font)
        XSetFont(dpy, kbd_gc, kbd_font->fid);
    for (r = 0; r < KBD_ROWS; r++) {
        for (c = 0; c < kbd_count[r]; c++) {
            int x, y, w, h, tw;
            const struct KbdCap *cap = &kbd_caps[r][c];
            const int selected = r == kbd_row && c == kbd_col;
            const int pressed = kbd_pressed == r * 100 + c;
            const int lit = cap->action == KA_ENTER ||
                            (cap->action == KA_SHIFT && (kbd_upper || kbd_caps_lock));
            kbd_rect(r, c, &x, &y, &w, &h);
            XSetForeground(dpy, kbd_gc, pressed ? edge : lit ? teal : selected ? blue : card);
            XFillRectangle(dpy, kbd_win, kbd_gc, x, y, (unsigned)w, (unsigned)h);
            XSetForeground(dpy, kbd_gc, selected ? blue : edge);
            XDrawRectangle(dpy, kbd_win, kbd_gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
            XSetForeground(dpy, kbd_gc, ink);
            tw = kbd_font ? XTextWidth(kbd_font, cap->label, (int)strlen(cap->label))
                          : (int)strlen(cap->label) * 8;
            XDrawString(dpy, kbd_win, kbd_gc, x + (w - tw) / 2,
                        y + (h + (kbd_font ? kbd_font->ascent : 10)) / 2 - 2,
                        cap->label, (int)strlen(cap->label));
        }
    }
    /* D-pad selection repaints must be complete before the next evdev event.
     * Flushing only queued the fill and labels; on the fbdev X server a later
     * cursor save-under could race that queue and restore half-painted keys. */
    XSync(dpy, False);
}

static void kbd_send(KeySym sym)
{
    KeyCode code = XKeysymToKeycode(dpy, sym);
    int shifted = 0;
    if (!code)
        return;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    shifted = XKeycodeToKeysym(dpy, code, 0) != sym
              && XKeycodeToKeysym(dpy, code, 1) == sym;
#pragma GCC diagnostic pop
    if (shifted)
        key_state(kc.shift, 1);
    key_state(code, 1);
    key_state(code, 0);
    if (shifted)
        key_state(kc.shift, 0);
}

static void kbd_activate(int row, int col)
{
    const struct KbdCap cap = kbd_caps[row][col];
    switch (cap.action) {
    case KA_SHIFT:
        if (!kbd_upper && !kbd_caps_lock) kbd_upper = 1;
        else if (kbd_upper) { kbd_upper = 0; kbd_caps_lock = 1; }
        else kbd_caps_lock = 0;
        break;
    case KA_SYMBOLS: kbd_symbols = !kbd_symbols; break;
    case KA_BACK: case KA_SPACE: case KA_LEFT: case KA_RIGHT:
    case KA_ESCAPE: case KA_ENTER: kbd_send(cap.sym); break;
    case KA_CHAR:
        kbd_send(cap.sym);
        if (kbd_upper && !kbd_caps_lock)
            kbd_upper = 0;
        break;
    }
    kbd_build();
    kbd_paint();
}

static void kbd_move(unsigned bit)
{
    int x, y, w, h, c, best = 0;
    if (bit == DIR_LEFT)
        kbd_col = (kbd_col + kbd_count[kbd_row] - 1) % kbd_count[kbd_row];
    else if (bit == DIR_RIGHT)
        kbd_col = (kbd_col + 1) % kbd_count[kbd_row];
    else {
        int target = kbd_row + (bit == DIR_UP ? -1 : 1);
        int centre, distance = 0x7fffffff;
        if (target < 0 || target >= KBD_ROWS)
            return;
        kbd_rect(kbd_row, kbd_col, &x, &y, &w, &h);
        centre = x + w / 2;
        for (c = 0; c < kbd_count[target]; c++) {
            int d;
            kbd_rect(target, c, &x, &y, &w, &h);
            d = abs((x + w / 2) - centre);
            if (d < distance) { distance = d; best = c; }
        }
        kbd_row = target;
        kbd_col = best;
    }
    kbd_paint();
}

static int kbd_hit(int px, int py)
{
    int r, c;
    for (r = 0; r < KBD_ROWS; r++)
        for (c = 0; c < kbd_count[r]; c++) {
            int x, y, w, h;
            kbd_rect(r, c, &x, &y, &w, &h);
            if (px >= x && px < x + w && py >= y && py < y + h)
                return r * 100 + c;
        }
    return -1;
}

static void kbd_events(void)
{
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.xany.window != kbd_win)
            continue;
        if (ev.type == Expose) {
            kbd_paint();
        } else if (ev.type == MotionNotify) {
            int hit = kbd_hit(ev.xmotion.x, ev.xmotion.y);
            if (hit >= 0 && (kbd_row != hit / 100 || kbd_col != hit % 100)) {
                kbd_row = hit / 100;
                kbd_col = hit % 100;
                kbd_paint();
            }
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            kbd_pressed = kbd_hit(ev.xbutton.x, ev.xbutton.y);
            kbd_paint();
        } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
            int hit = kbd_hit(ev.xbutton.x, ev.xbutton.y);
            kbd_pressed = -1;
            if (hit >= 0)
                kbd_activate(hit / 100, hit % 100);
            else
                kbd_paint();
        }
    }
}

static void kbd_set_visible(int visible)
{
    if (visible == kbd_visible)
        return;
    if (visible) {
        /* A direction held while Select is pressed was sent to the application
         * as a real key-down.  Release it before the keyboard takes ownership. */
        release_directions(dirs_all());
        kbd_visible = 1;
        XMapRaised(dpy, kbd_win);
        kbd_paint();
    } else {
        kbd_visible = 0;
        XUnmapWindow(dpy, kbd_win);
        /* Menu can hand the framebuffer away immediately after this call.  Wait
         * until X has restored the keyboard's save-under before mixdash paints. */
        XSync(dpy, False);
    }
}

static void kbd_toggle(void)
{
    kbd_set_visible(!kbd_visible);
}

static void kbd_start(void)
{
    XSetWindowAttributes a;
    static const char *const fonts[] = { "9x15bold", "9x15", "8x13", "fixed", NULL };
    int i;
    kbd_h = scr_h * 2 / 5;
    if (kbd_h < 150) kbd_h = 150;
    if (kbd_h > scr_h) kbd_h = scr_h;
    memset(&a, 0, sizeof(a));
    a.override_redirect = True;
    /* A saved-under keyboard caches pixels from whichever owner last used the
     * physical framebuffer.  After a dashboard hand-off those pixels are stale
     * and unmapping the keyboard paints them back over X (or vice versa). */
    a.save_under = False;
    a.event_mask = ExposureMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    kbd_win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, scr_h - kbd_h,
                            (unsigned)scr_w, (unsigned)kbd_h, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWSaveUnder | CWEventMask, &a);
    XStoreName(dpy, kbd_win, "MixOS Keyboard");
    kbd_gc = XCreateGC(dpy, kbd_win, 0, NULL);
    for (i = 0; fonts[i] && !kbd_font; i++)
        kbd_font = XLoadQueryFont(dpy, fonts[i]);
    kbd_build();
}

/*
 * Xlib's contract for an I/O error handler is that it does not return; the
 * connection is gone and every Xlib call after it is undefined.  So this is the
 * one place that exits without unwinding -- after closing the pads, which is the
 * only cleanup that matters to anything outside this process.
 */
static int on_io_error(Display *unused)
{
    (void)unused;
    close_pads();
    fprintf(stderr, "j36-padx: the X server went away\n");
    _exit(0);
}

/* ── the sticks ──────────────────────────────────────────────────────────── */

/*
 * SPEEDS ARE A FRACTION OF THE SCREEN, not a number of pixels.
 *
 * 1100 pixels per second was written for no panel in particular and read as
 * "crosses this one in half a second", which on 640x480 is a pointer nobody can
 * land on a link with -- and that was before the acceleration described above
 * doubled it.  What a thumb is actually judging is how much of the SCREEN goes by,
 * so that is what these say, and the panel decides the rest.
 *
 * AND THEY ARE THE FALLBACK, NOT THE ANSWER.  A screen-fraction is the right shape
 * for a program that knows nothing about the machine it is on, and this program is
 * not in that position: it is started by mixdash, which has a Mouse settings page
 * whose whole subject is how fast this pointer should move.  A browser session that
 * ignored that setting gave the device two pointer speeds, one of them unreachable
 * from any screen -- so the file is read, and the constant only decides what
 * happens on a card with no dashboard speed setting on it yet.  See
 * read_dash_mouse().
 *
 * 0.31 of a 640 px panel is 200 px/s, which is the number that was actually asked
 * for after the 1.0 above -- 640 px/s -- turned out to be exactly the "extremely
 * fast, difficult to navigate" the whole comment above was written to fix.  The
 */
#define STICK_SCREENS_PER_S  0.3125
#define SCROLL_MAX  14.0     /* same wheel notches/second as mixdash */

/*
 * ── THE DASHBOARD'S POINTER SPEED ────────────────────────────────────────────
 *
 * mixdash keeps its settings in a plain INI file -- see pickStorePath() in
 * mixdash/settings.cpp for why that path and not another -- and the Mouse page
 * writes the three values which define the stick's logical response:
 *
 *     [mouse]
 *     pointerSpeed=200
 *     acceleration=45
 *     deadzone=16
 *
 * Speed is pixels per second at full deflection, acceleration becomes the same
 * 1.0..2.5 magnitude exponent used by Joypad::driveStick(), and deadzone is a
 * percentage.  Reading only speed made the two cursors feel different even when
 * their end-to-end travel time happened to agree.
 *
 * PARSED BY HAND AND NOT WITH A LIBRARY.  They are three numbers under one section
 * in a file this project writes, and linking an INI parser into this bridge to read
 * them would be the larger change.  The parse is deliberately narrow: a section
 * header other than [mouse] is skipped, whitespace either side of the = is
 * allowed, anything that is not a number is ignored, and every failure -- no file,
 * no key, an unreadable value -- leaves the caller's default alone.  A browser
 * that will not start because a config file has a typo in it is a worse outcome
 * than a pointer moving at the built-in speed.
 *
 * THE RANGE IS THE SAME ONE mixdash CLAMPS TO (80..2400), and it is applied here
 * too rather than trusted: this file is on a partition somebody can mount on a PC,
 * and a hand-edited 0 is a pointer that cannot move on a device whose only other
 * input is the pad now driving it.
 */
#define DASH_CONF     "/var/lib/mixos/mixdash.conf"
#define DASH_SPEED_LO 80.0
#define DASH_SPEED_HI 2400.0

struct DashMouse {
    double speed;
    double acceleration;
    double deadzone;
};

static void read_dash_mouse(struct DashMouse *cfg)
{
    FILE *f;
    char line[256];
    int in_mouse = 0;

    cfg->speed = 0.0;
    cfg->acceleration = 45.0;
    cfg->deadzone = 16.0;

    f = fopen(DASH_CONF, "r");
    if (!f)
        return;

    while (fgets(line, sizeof line, f)) {
        char *p = line, *eq, *end;
        double v;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == ';')
            continue;

        if (*p == '[') {
            /* Any other section, including one this build has never heard of.
             * strncmp and not strcmp: the line still has its newline on it. */
            in_mouse = (strncmp(p, "[mouse]", 7) == 0);
            continue;
        }
        if (!in_mouse)
            continue;

        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        /* Trailing space before the =, which QSettings does not write but a person
         * editing the file by hand does. */
        for (end = eq; end > p && (end[-1] == ' ' || end[-1] == '\t'); end--)
            end[-1] = '\0';
        v = strtod(eq + 1, &end);
        if (end == eq + 1)
            continue;               /* the value was not a number at all */
        if (strcmp(p, "pointerSpeed") == 0)
            cfg->speed = v;
        else if (strcmp(p, "acceleration") == 0)
            cfg->acceleration = v;
        else if (strcmp(p, "deadzone") == 0)
            cfg->deadzone = v;
        /* No break: a file with the key twice should mean what its last line says,
         * which is what QSettings itself would read back. */
    }
    fclose(f);

    if (cfg->speed > 0.0 && cfg->speed < DASH_SPEED_LO)
        cfg->speed = DASH_SPEED_LO;
    else if (cfg->speed > DASH_SPEED_HI)
        cfg->speed = DASH_SPEED_HI;
    if (cfg->acceleration < 0.0)
        cfg->acceleration = 0.0;
    else if (cfg->acceleration > 100.0)
        cfg->acceleration = 100.0;
    if (cfg->deadzone < 2.0)
        cfg->deadzone = 2.0;
    else if (cfg->deadzone > 60.0)
        cfg->deadzone = 60.0;
}

/*
 * Raw reading to -1..1, with the deadzone taken out and what is left stretched
 * back over the whole range, so that the first movement past the deadzone is a
 * small one rather than a step.
 *
 * The deadzone is the same user setting mixdash applies.  Keeping the setting in
 * screen-independent axis space makes the center response identical too, rather
 * than merely matching full-stick speed.
 */
static double axis_norm(int slot, int role, int raw)
{
    double t, d;

    if (ax_half[slot][role] <= 0.0)
        return 0.0;
    t = ((double)raw - ax_centre[slot][role]) / ax_half[slot][role];
    if (t > 1.0)
        t = 1.0;
    else if (t < -1.0)
        t = -1.0;

    d = stick_deadzone;
    if (t > -d && t < d)
        return 0.0;
    return (t < 0.0 ? t + d : t - d) / (1.0 - d);
}

/* Exactly mixdash/Joypad::driveStick: the first complete left stick wins, its
 * circular magnitude is clamped, then the configured response exponent is
 * applied to that magnitude rather than independently to each axis. */
static void pointer_rate(double *rx, double *ry)
{
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        double x, y, mag, curved;
        if (ax_code[i][AX_LX] < 0 || ax_code[i][AX_LY] < 0)
            continue;
        x = ax_val[i][AX_LX];
        y = ax_val[i][AX_LY];
        mag = sqrt(x * x + y * y);
        if (mag > 1.0) {
            x /= mag;
            y /= mag;
            mag = 1.0;
        }
        if (mag <= 0.0) {
            *rx = *ry = 0.0;
            return;
        }
        curved = pow(mag, 1.0 + stick_acceleration * 0.015);
        *rx = x / mag * stick_max * curved;
        *ry = y / mag * stick_max * curved;
        return;
    }
    *rx = *ry = 0.0;
}

/* Same first-device rule and linear 14-notch response as the dashboard's right
 * stick. */
static void scroll_rate(double *rx, double *ry)
{
    int i;
    for (i = 0; i < MAX_PADS; i++) {
        if (ax_code[i][AX_RX] < 0 || ax_code[i][AX_RY] < 0)
            continue;
        *rx = ax_val[i][AX_RX] * SCROLL_MAX;
        *ry = ax_val[i][AX_RY] * SCROLL_MAX;
        return;
    }
    *rx = *ry = 0.0;
}

static int sticks_active(void)
{
    int i, r;
    for (i = 0; i < MAX_PADS; i++) {
        for (r = 0; r < AX_N; r++) {
            if (ax_val[i][r] != 0.0)
                return 1;
        }
    }
    return 0;
}

/* ── the D-pad ───────────────────────────────────────────────────────────── */

static unsigned dirs_all(void)
{
    unsigned d = 0;
    int i;
    for (i = 0; i < MAX_PADS; i++)
        d |= pad_dirs[i];
    return d;
}

/*
 * Aggregate before emitting so two attached pads holding the same direction are
 * one X key press with one final release.  While the keyboard is visible, a press
 * walks its selected cap instead; no analogue axis reaches this function.
 */
static void set_dir(int slot, unsigned bit, int on)
{
    unsigned was = dirs_all();
    unsigned is;

    if (on)
        pad_dirs[slot] |= bit;
    else
        pad_dirs[slot] &= ~bit;
    is = dirs_all();
    if ((was ^ is) & bit) {
        KeyCode key = bit == DIR_UP ? kc.up
                    : bit == DIR_DOWN ? kc.down
                    : bit == DIR_LEFT ? kc.left : kc.right;
        if (kbd_visible) {
            if (is & bit)
                kbd_move(bit);
        } else {
            key_state(key, (is & bit) != 0);
        }
    }
}

/* ── auto-repeat for the shoulders ───────────────────────────────────────── */

#define REP_L1 0
#define REP_R1 1
#define REP_L2 2
#define REP_R2 3
#define REP_N  4

/* The first repeat waits long enough that a tap is a tap; the rest come fast
 * enough that holding R1 reads as scrolling rather than as clicking. */
#define REP_DELAY_MS  360
#define REP_EVERY_MS  70

static long rep_due[REP_N];

static void rep_clear(void)
{
    int i;
    for (i = 0; i < REP_N; i++)
        rep_due[i] = 0;
}

static void rep_set(int which, int down, long now)
{
    rep_due[which] = down ? now + REP_DELAY_MS : 0;
}

static void rep_fire(int which, long now)
{
    if (!rep_due[which] || now < rep_due[which])
        return;
    rep_due[which] = now + REP_EVERY_MS;
    switch (which) {
    case REP_L1: wheel(4); break;
    case REP_R1: wheel(5); break;
    case REP_L2: tap(0, kc.page_up); break;
    case REP_R2: tap(0, kc.page_down); break;
    }
}

static int rep_any(void)
{
    int i;
    for (i = 0; i < REP_N; i++) {
        if (rep_due[i])
            return 1;
    }
    return 0;
}

/*
 * ── COMING BACK FROM A STOP: THROW AWAY EVERYTHING FROM WHILE WE WERE AWAY ──
 *
 * This process spends the switcher SIGSTOPped, and the pad does not stop with
 * it.  evdev keeps a buffer per open descriptor, so every press the user made
 * driving the dashboard's switcher -- A to choose a row, the D-pad to walk it,
 * Menu to get there in the first place -- is sitting in this program's queue the
 * instant it is allowed to run again.  Read them normally and they would be
 * replayed into the browser through XTEST: a click somewhere on the page, a
 * Return in whatever had focus, Alt+Left navigating away.  The events were meant
 * for a different program on a different screen and they are not ours to deliver.
 *
 * WHAT THE RESET IS FOR, and it is a separate fault with the same cause.  A
 * button that was down when the stop landed has a release in that same discarded
 * batch, so the state this program keeps -- direction bits, stick deflection,
 * shoulder repeats, and above all menu_down_at -- describes a hand that has since
 * let go.  Left alone, the first tick after SIGCONT could interpret stale hold
 * state as a new gesture and ask for the switcher again.  Zeroing the lot is the
 * whole fix, and it costs at most one button that has to be pressed again.
 *
 * The drain is best-effort by design: EAGAIN ends it, anything else means the
 * device left while we were stopped and the next poll will drop the slot through
 * the ordinary path.
 */
static void forget_pads(void)
{
    struct input_event ev;
    int i, r;
    const unsigned held = dirs_all();

    for (i = 0; i < MAX_PADS; i++) {
        if (pad_fd[i] < 0)
            continue;
        while (read(pad_fd[i], &ev, sizeof ev) == (ssize_t)sizeof ev)
            ;
        pad_dirs[i] = 0;
        for (r = 0; r < AX_N; r++)
            ax_val[i][r] = 0.0;
    }
    release_directions(held);
    rep_clear();
    menu_down_at = 0;
    kbd_menu_hidden = 0;
}

/* ── The session's control pipe ──────────────────────────────────────────── */

/*
 * WHERE `next' GOES WHEN MENU IS TAPPED, and nothing at all when --ctl was not
 * given.
 *
 * The session this bridge belongs to -- /opt/mixos/bin/j36-xsession-main -- holds
 * a FIFO under /run open for reading and writing and takes one line at a time:
 * `next', `prev', `quit', or `run' and a command line.  Menu tapped means "show me
 * the next window", which is the same gesture the dashboard behind this session
 * uses for the same idea one level up.  The two do not collide: a hold is a
 * thousand milliseconds and a tap is what a press that ends before then is.
 *
 * WHY THE WINDOW MANAGER IS ASKED AND NOT A KEY SYNTHESISED.  matchbox ships
 * matchbox-remote, which sends the WM the same ClientMessage its own key bindings
 * do, so there is no keycode to find, no modifier state to get wrong and nothing
 * to configure.  Through XTEST it would be a key put into whichever window has
 * focus -- and a browser is perfectly entitled to have bound that key itself.
 *
 * O_NONBLOCK ON THE OPEN, ALWAYS.  A FIFO opened for writing blocks until a reader
 * appears, and the case that has to be survived is a session that died without
 * removing its pipe: without the flag that is a pad bridge wedged inside a button
 * handler, still holding the grab, with the pointer frozen and no way back.  With
 * it the open fails ENXIO and the tap does nothing, which is the right amount of
 * nothing.  The line is far below PIPE_BUF, so the write is atomic against the
 * other writers -- mixdash and j36-xrun -- and cannot be interleaved with theirs.
 */
static const char *ctl_path;

static void ctl_send(const char *word)
{
    char line[64];
    int fd, n;

    if (!ctl_path)
        return;

    fd = open(ctl_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        note("no session listening on %s (%s)", ctl_path, strerror(errno));
        return;
    }
    n = snprintf(line, sizeof line, "%s\n", word);
    if (n > 0 && write(fd, line, (size_t)n) < 0)
        note("could not ask the session for `%s': %s", word, strerror(errno));
    close(fd);
}

/* ── main ────────────────────────────────────────────────────────────────── */

/* Long enough that it cannot be a fumble, short enough that nobody wonders
 * whether it is working. */
#define MENU_HOLD_MS 1000

/* One frame at the panel's rate.  Everything that moves is stepped on this. */
#define TICK_MS 16

/* How often a free slot goes looking for a pad that was not there before. */
#define RESCAN_MS 2000

static volatile sig_atomic_t stop_asked;

static void on_signal(int sig)
{
    (void)sig;
    stop_asked = 1;
}

/*
 * This session has just been let run again.
 *
 * mixdash stops the whole process group when the switcher takes the panel, and
 * continues it when the user comes back -- so SIGCONT is the only notification
 * this process gets that it is in front once more.  Two things follow from it:
 * everything the pad queued while we were away is thrown out (forget_pads, and
 * the note there is the important one), and with --grab the pad is taken back.
 * A flag and nothing else: both are work for the loop, not for a handler.
 *
 * Harmless when nothing stopped us.  SIGCONT is delivered to a process that was
 * never stopped as well; the queue is empty in that case and a re-grab of a
 * device already grabbed is a no-op.
 */
static volatile sig_atomic_t cont_asked;

static void on_cont(int sig)
{
    (void)sig;
    cont_asked = 1;
}

static void usage(void)
{
    fprintf(stderr,
        "j36-padx -- the J36 Ultra pad as an X pointer and keyboard\n"
        "\n"
        "  --watch PID       quit when PID exits; Menu held closes the session,\n"
        "                    or asks $MIXDASH_PID for its task switcher instead\n"
        "  --ctl PATH        the session's control pipe: Menu TAPPED asks it to\n"
        "                    page to the next window\n"
        "  --display NAME    which server (default $DISPLAY)\n"
        "  --focus PID       focus the X client with _NET_WM_PID=PID, then exit\n"
        "  --grab            take the pad while X owns the panel; Menu hold\n"
        "                    explicitly releases it before the dashboard hand-off\n"
        "  --no-grab         share the pad (the command-line default)\n"
        "  --list            name the devices that would be used, then exit\n"
        "  -v                say what is happening\n"
        "\n"
        "Select toggles the on-screen keyboard.\n");
}

static void pointer_reload_settings(void)
{
    struct DashMouse mouse;

    read_dash_mouse(&mouse);
    stick_max = mouse.speed > 0.0 ? mouse.speed
                                  : STICK_SCREENS_PER_S * (double)scr_w;
    stick_acceleration = mouse.acceleration;
    stick_deadzone = mouse.deadzone / 100.0;
    note("screen %dx%d: %.0f px/s, acceleration %.0f, deadzone %.0f%% (%s); "
         "D-pad is navigation", scr_w, scr_h, stick_max, stick_acceleration,
         stick_deadzone * 100.0,
         mouse.speed > 0.0 ? "from " DASH_CONF
                           : "built-in speed; no dashboard speed setting on this card");
}

/*
 * The screen, the dashboard's setting, and the speeds that come out of them.
 *
 * Down here rather than beside the rest of the pointer code because it reads the
 * stick ceiling and the D-pad's ratio to it, and those are declared with the stick
 * and the D-pad they belong to.
 */
static void pointer_start(void)
{
    scr   = DefaultScreen(dpy);
    scr_w = DisplayWidth(dpy, scr);
    scr_h = DisplayHeight(dpy, scr);
    /* A server that answers with nothing usable would otherwise make every speed
     * below zero, which is a pointer that never moves and no message about why. */
    if (scr_w < 2 || scr_h < 2) {
        fail("this screen says it is %dx%d; using 640x480 for the speeds", scr_w, scr_h);
        scr_w = 640;
        scr_h = 480;
    }

    /*
     * The dashboard's number wins where there is one, and the screen fraction is
     * what a card with no settings file falls back to.  Which of the two was used
     * goes in the log line below, because "the browser's pointer is a different
     * speed from the dashboard's" is exactly the report this code exists to answer
     * and the answer is either "it did not read the file" or "it read this".
     */
    pointer_reload_settings();
    /* The linuxfb cursor and this X cursor hand one coordinate pair through /run.
     * Warp before the first client appears, so there is never a centre-screen X
     * cursor followed by the dashboard cursor somewhere else. */
    if (pointer_state_read(&sent_x, &sent_y)) {
        if (sent_x < 0) sent_x = 0;
        if (sent_y < 0) sent_y = 0;
        if (sent_x >= scr_w) sent_x = scr_w - 1;
        if (sent_y >= scr_h) sent_y = scr_h - 1;
        ptr_x = sent_x;
        ptr_y = sent_y;
        XTestFakeMotionEvent(dpy, scr, sent_x, sent_y, 0);
        XFlush(dpy);
    } else {
        sent_x = -1;
        sent_y = -1;
        pointer_resync();
    }
    pointer_install_cursor();

}

int main(int argc, char **argv)
{
    const char *display_name = NULL;
    pid_t watch_pid = 0;
    pid_t focus_pid = 0;
    int grab = 0, list_only = 0;
    /*
     * Whether the pad is grabbed AT THIS MOMENT, as against whether it was asked
     * for on the command line.  The two differ for as long as the switcher is up:
     * see the hand-over below.
     */
    int grabbing;
    /*
     * The dashboard, if this session was started by one.  mixdash puts its pid in
     * every child's environment; nothing else sets this, and when it is unset --
     * j36-padx run by hand, or a session started some other way -- the Menu hold
     * keeps the behaviour it has always had and closes the session.
     */
    pid_t mixdash_pid = 0;
    const char *mixdash_env;
    int i, xt_event, xt_error, xt_major, xt_minor;

    long move_last = 0, next_scan = 0;
    double scroll_x = 0.0, scroll_y = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch") && i + 1 < argc)
            watch_pid = (pid_t)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ctl") && i + 1 < argc)
            ctl_path = argv[++i];
        else if (!strcmp(argv[i], "--display") && i + 1 < argc)
            display_name = argv[++i];
        else if (!strcmp(argv[i], "--focus") && i + 1 < argc)
            focus_pid = (pid_t)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--grab"))
            grab = 1;
        else if (!strcmp(argv[i], "--no-grab"))
            grab = 0;
        else if (!strcmp(argv[i], "--list"))
            list_only = 1;
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else {
            usage();
            return 2;
        }
    }

    mixdash_env = getenv("MIXDASH_PID");
    if (mixdash_env && *mixdash_env)
        mixdash_pid = (pid_t)strtol(mixdash_env, NULL, 10);
    if (mixdash_pid < 0)
        mixdash_pid = 0;

    for (i = 0; i < MAX_PADS; i++)
        clear_pad(i);

    if (list_only) {
        verbose = 1;
        if (!scan_pads(0)) {
            fail("no pad among /dev/input/event*");
            return 1;
        }
        for (i = 0; i < MAX_PADS; i++) {
            if (pad_fd[i] >= 0)
                printf("%s\n", pad_name[i]);
        }
        close_pads();
        return 0;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    /* The session script kills this and the browser in whatever order the shell
     * gets to them; a write to a dead X connection must not take the process out
     * before close_pads() has run. */
    signal(SIGPIPE, SIG_IGN);
    /* The dashboard's switcher stopped this whole group and has now let it run
     * again; the pad has to be taken back.  See on_cont(). */
    signal(SIGCONT, on_cont);

    dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fail("no X server on %s", display_name ? display_name
                                               : (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return 1;
    }
    XSetIOErrorHandler(on_io_error);
    XSetErrorHandler(on_x_error);

    scr = DefaultScreen(dpy);
    if (focus_pid > 0) {
        const int found = focus_client(focus_pid);
        if (!found)
            fail("no X window advertises pid %ld", (long)focus_pid);
        XCloseDisplay(dpy);
        return found ? 0 : 1;
    }

    if (!XTestQueryExtension(dpy, &xt_event, &xt_error, &xt_major, &xt_minor)) {
        fail("this X server has no XTEST extension, so the pad cannot drive it");
        XCloseDisplay(dpy);
        return 1;
    }
    note("XTEST %d.%d", xt_major, xt_minor);

    /* Fake events become impervious to server grabs.  Without this, a client that
     * grabs the pointer -- which is every menu in GTK -- would freeze the pad for
     * as long as its menu is open, which is exactly when the pad is needed. */
    XTestGrabControl(dpy, True);

    resolve_keys();
    pointer_start();
    kbd_start();

    /* Before the first window maps, so that a session that takes a while to open
     * one -- or is asked to open none at all -- has something on it that says so.
     * The screen size it lays out to comes from pointer_start() above. */
    desktop_paint();

    grabbing = grab;
    if (!scan_pads(grabbing)) {
        fail("no pad among /dev/input/event*, so nothing can drive this session "
             "-- run with --list to see what was rejected");
        XCloseDisplay(dpy);
        return 1;
    }

    move_last = now_ms();
    next_scan = move_last + RESCAN_MS;

    while (!stop_asked) {
        struct pollfd fds[MAX_PADS + 1];
        int map[MAX_PADS + 1];
        int nfd = 0, timeout, n, k;
        long now;
        double dt, dx, dy, sx, sy;

        for (i = 0; i < MAX_PADS; i++) {
            if (pad_fd[i] < 0)
                continue;
            map[nfd] = i;
            fds[nfd].fd = pad_fd[i];
            fds[nfd].events = POLLIN;
            fds[nfd].revents = 0;
            nfd++;
        }
        map[nfd] = -1;
        fds[nfd].fd = ConnectionNumber(dpy);
        fds[nfd].events = POLLIN;
        fds[nfd].revents = 0;
        nfd++;

        /* One frame while something is moving or repeating, so motion runs at the
         * panel's own rate; a quarter of a second otherwise, which is short enough
         * that a browser that exits is noticed at once and long enough that an idle
         * session is not a process spinning.  A deflected stick counts as moving
         * even when no event has arrived, because an axis held still sends nothing
         * -- evdev reports changes, and a stick resting against its stop has
         * stopped changing. */
        timeout = (dirs_all() || menu_down_at || rep_any() || sticks_active())
                  ? TICK_MS : 250;
        n = poll(fds, (nfds_t)nfd, timeout);
        if (n < 0 && errno != EINTR)
            break;

        now = now_ms();

        /*
         * Let run again, and the first thing that happens is the only thing that
         * can happen safely: nothing the pad said while we were stopped is ours.
         *
         * Checked here, ahead of the reads, because after them is too late -- the
         * queue would already have been replayed into the browser.  The iteration
         * is then abandoned: the poll that was interrupted by the SIGCONT has
         * nothing left to report, and the next one round starts from a pad with no
         * buttons down and no backlog.
         */
        if (cont_asked) {
            cont_asked = 0;
            forget_pads();
            /* Mouse settings can be changed while this X session is stopped on
             * the dashboard.  Reload all three response values before accepting
             * another stick event, or a long-lived Desktop would retain its old
             * curve while Qt immediately used the new one. */
            pointer_reload_settings();
            /* A keyboard hidden by the Menu press belongs to the screen we left,
             * not to this resumed one.  Keep it closed across a real hand-off. */
            kbd_menu_hidden = 0;
            move_last = now;
            if (grab && !grabbing) {
                note("continued by the dashboard, taking the pad back");
                set_grab(1);
                grabbing = 1;
            }
            /* AND THE SCREEN IS NOT OURS EITHER.  The dashboard has been drawing
             * on the panel for as long as this was stopped; whatever it left that
             * its restore did not cover is still there, and X does not know a
             * pixel changed.  Keep the software cursor hidden while the X shadow
             * buffer is exposed: showing it first makes X save the dashboard
             * pixels underneath and restore them later as a transparent hole. */
            screen_cursor(0);
            screen_refresh();
            screen_cursor(1);
            continue;
        }

        for (k = 0; k < nfd && n > 0; k++) {
            struct input_event ev;
            int slot = map[k];
            int gone = 0;

            if (slot < 0) {
                if (fds[k].revents & POLLIN)
                    kbd_events();
                continue;
            }

            if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                drop_pad(slot);
                continue;
            }
            if (!(fds[k].revents & POLLIN))
                continue;

            while (!gone) {
                ssize_t got = read(pad_fd[slot], &ev, sizeof(ev));
                int down;

                if (got != (ssize_t)sizeof(ev)) {
                    /* EAGAIN is the ordinary end of a batch on a non-blocking fd.
                     * Anything else on an evdev node means the device is gone --
                     * ENODEV is what an unplug gives -- and the slot has to be
                     * released, or this spins on a dead fd for the rest of the
                     * session. */
                    if (got < 0 && (errno == EAGAIN || errno == EINTR))
                        break;
                    drop_pad(slot);
                    gone = 1;
                    break;
                }

                if (ev.type == EV_ABS) {
                    int r;

                    /* The hat is the D-pad on every USB pad that has one, and it
                     * is three states on one axis rather than two buttons, so it
                     * clears the opposite direction as well as setting its own. */
                    if (ev.code == ABS_HAT0X) {
                        set_dir(slot, DIR_LEFT,  ev.value < 0);
                        set_dir(slot, DIR_RIGHT, ev.value > 0);
                        continue;
                    }
                    if (ev.code == ABS_HAT0Y) {
                        set_dir(slot, DIR_UP,   ev.value < 0);
                        set_dir(slot, DIR_DOWN, ev.value > 0);
                        continue;
                    }
                    /* Only the value is kept; what to do with it happens once per
                     * tick below, so a chatty ADC cannot make the pointer faster.
                     * An axis with no role -- a trigger, a second hat, a DualSense
                     * accelerometer -- matches nothing here and is dropped. */
                    for (r = 0; r < AX_N; r++) {
                        if (ax_code[slot][r] == (int)ev.code) {
                            ax_val[slot][r] = axis_norm(slot, r, ev.value);
                            break;
                        }
                    }
                    continue;
                }
                if (ev.type != EV_KEY)
                    continue;
                /* value 2 is the kernel's own auto-repeat.  Every repeating
                 * binding here does its own timing, and the ones that do not
                 * repeat must not start because a thumb rested on a button. */
                if (ev.value == 2)
                    continue;
                down = (ev.value == 1);

                switch (ev.code) {
                /* Two spellings of the same four directions: this board's device
                 * tree uses the keyboard codes, and a USB pad whose D-pad is not a
                 * hat uses BTN_DPAD_*. */
                case KEY_UP:
                case BTN_DPAD_UP:    set_dir(slot, DIR_UP,    down); break;
                case KEY_DOWN:
                case BTN_DPAD_DOWN:  set_dir(slot, DIR_DOWN,  down); break;
                case KEY_LEFT:
                case BTN_DPAD_LEFT:  set_dir(slot, DIR_LEFT,  down); break;
                case KEY_RIGHT:
                case BTN_DPAD_RIGHT: set_dir(slot, DIR_RIGHT, down); break;

                case BTN_A:
                    if (kbd_visible) {
                        if (down)
                            kbd_activate(kbd_row, kbd_col);
                    } else {
                        click(1, down);
                    }
                    break;
                case BTN_THUMBL: click(2, down); break;
                case BTN_THUMBR: click(3, down); break;

                case BTN_B:
                    if (down) {
                        /* Still Back in every client.  In an empty Desktop there
                         * is no client to receive it, so the session accepts the
                         * companion request as the explicit way out. */
                        if (kbd_visible)
                            kbd_set_visible(0);
                        tap(kc.alt, kc.left);
                        ctl_send("quit-empty");
                    }
                    break;
                case BTN_X: if (down) tap(0, kc.ret);       break;
                case BTN_Y: if (down) tap(0, kc.esc);       break;

                /* The shoulders fire once on the press and then on the repeat
                 * clock, so a tap is one notch and a hold is a scroll. */
                case BTN_TL:
                    if (down)
                        wheel(4);
                    rep_set(REP_L1, down, now);
                    break;
                case BTN_TR:
                    if (down)
                        wheel(5);
                    rep_set(REP_R1, down, now);
                    break;
                case BTN_TL2:
                    if (down)
                        tap(0, kc.page_up);
                    rep_set(REP_L2, down, now);
                    break;
                case BTN_TR2:
                    if (down)
                        tap(0, kc.page_down);
                    rep_set(REP_R2, down, now);
                    break;

                case BTN_START:
                    if (down)
                        tap(kc.ctrl, kc.l);
                    break;
                case KEY_F12:
                    if (down)
                        tap(kc.alt, kc.home);
                    break;

                case KEY_VOLUMEDOWN: if (down) tap(kc.ctrl, kc.minus); break;
                case KEY_VOLUMEUP:   if (down) tap(kc.ctrl, kc.plus);  break;

                case BTN_SELECT:
                    if (down)
                        kbd_toggle();
                    break;

                case BTN_MODE:
                    /*
                     * ── MENU TAPPED: THE NEXT WINDOW ─────────────────────
                     *
                     * A press released before MENU_HOLD_MS is a tap, and the
                     * hold branch further down has therefore not fired -- it
                     * clears menu_down_at when it does, so a non-zero value
                     * here is proof that this release ends a press that was
                     * still short.  That single test is what keeps the two
                     * gestures from ever both happening on one press.
                     *
                     * With no --ctl there is no session to ask and a tap does
                     * what it has always done, which is nothing.
                     */
                    if (down) {
                        pointer_resync();
                        pointer_state_write();
                        menu_down_at = now;
                        /* Hide the keyboard on the press so it cannot be frozen
                         * onto the dashboard; restore it only if release proves
                         * this was the short next-window gesture. */
                        if (kbd_visible) {
                            kbd_menu_hidden = 1;
                            kbd_set_visible(0);
                        }
                        /* Hide on the press, so the server has restored the
                         * pixels under its cursor before the hold branch hands
                         * the panel away.  A tap restores it on release; a hold
                         * restores it after the session is continued. */
                        if (mixdash_pid > 0 && kill(mixdash_pid, 0) == 0)
                            screen_cursor(0);
                    } else {
                        if (menu_down_at && now - menu_down_at < MENU_HOLD_MS) {
                            note("Menu tapped, asking for the next window");
                            ctl_send("next");
                            if (kbd_menu_hidden)
                                kbd_set_visible(1);
                        }
                        kbd_menu_hidden = 0;
                        screen_cursor(1);
                        menu_down_at = 0;
                    }
                    break;

                default:
                    break;
                }
            }
        }

        /*
         * Motion, once per tick and not once per event.  Per event would make a
         * D-pad diagonal twice as fast as a straight direction -- it is two evdev
         * events for one movement -- and would tie the stick's speed to how often
         * the ADC feels like reporting rather than to how far it is pushed.
         */
        dt = (double)(now - move_last) / 1000.0;
        move_last = now;
        if (dt > (double)TICK_MS * 4.0 / 1000.0)
            dt = (double)TICK_MS * 4.0 / 1000.0;   /* woken from idle; do not lurch */

        pointer_rate(&dx, &dy);
        dx *= dt;
        dy *= dt;

        /* The remainder is kept inside the position, so a speed below one pixel per
         * tick is a slow pointer and not a still one. */
        pointer_move(dx, dy);

        /* The right stick, as a wheel.  Same carry: a notch is a discrete thing, so
         * what accumulates is fractions of one.  Buttons 6 and 7 are the horizontal
         * wheel; a client that does not understand them ignores them, which is the
         * right outcome for a page that does not scroll sideways. */
        scroll_rate(&sx, &sy);
        scroll_y += sy * dt;
        while (scroll_y >= 1.0)  { wheel(5); scroll_y -= 1.0; }
        while (scroll_y <= -1.0) { wheel(4); scroll_y += 1.0; }
        scroll_x += sx * dt;
        while (scroll_x >= 1.0)  { wheel(7); scroll_x -= 1.0; }
        while (scroll_x <= -1.0) { wheel(6); scroll_x += 1.0; }

        for (i = 0; i < REP_N; i++)
            rep_fire(i, now);

        /*
         * Look for a pad that was not there before.  A slow cadence and only when
         * there is somewhere to put one, so the usual case -- the built-in pad and
         * nothing else, three free slots -- costs one opendir every two seconds.
         *
         * Losing the LAST pad is not an exit.  Nothing else in this session can
         * drive the browser, so quitting would take the page away because a cable
         * was knocked; instead this keeps the X server up and waits, and the next
         * scan picks the pad back up when it is plugged in again.
         */
        if (now >= next_scan) {
            next_scan = now + RESCAN_MS;
            scan_pads(grabbing);
        }

        /*
         * ── MENU HELD: THE SWITCHER, OR THE OLD WAY OUT ──────────────────────
         *
         * With a dashboard behind this session the gesture means "show me what is
         * running", which is what the same hold means everywhere else on this
         * device.  Closing the browser is one of the things that switcher can do,
         * so nothing has been taken away -- it has been moved behind a screen
         * that says what it is about to close.
         *
         * THE PAD GOES BACK BEFORE THE SIGNAL, and that ordering is the whole
         * trick.  mixdash is about to draw a switcher and drive it with the pad;
         * it cannot read a device this process has grabbed, and a moment later it
         * will stop this process -- with the grab still held, because a grab
         * belongs to the descriptor and not to whether its owner is scheduled.
         * Handing it over first is the only order that leaves a usable switcher.
         *
         * Then the hold state is cleared and this process stops itself.  That is
         * stronger than waiting for the launcher's process-group stop: xinit may
         * have put this bridge in another group, which is how the second live X
         * cursor escaped onto the dashboard in the reported video.  mixdash has
         * the pid file needed to continue this exact process.
         *
         * With no dashboard to ask, the old behaviour stands unchanged.
         */
        if (menu_down_at && now - menu_down_at >= MENU_HOLD_MS) {
            if (mixdash_pid > 0 && kill(mixdash_pid, 0) == 0) {
                note("Menu held, asking the dashboard for its switcher");
                set_grab(0);
                grabbing = 0;
                screen_cursor(0);
                kill(mixdash_pid, SIGUSR1);
                menu_down_at = 0;
                kbd_menu_hidden = 0;
                /* The buttons that were down belong to a session that is about to
                 * be stopped, and their releases will be delivered to a switcher
                 * instead.  Cancelling the repeats here is what stops a shoulder
                 * paging the browser for the whole time the switcher is up. */
                rep_clear();
                /* xinit may have moved this bridge outside the launcher's process
                 * group.  Stop ourselves before a release or stick event can
                 * restore X cursor pixels over the dashboard.  mixdash continues
                 * this exact pid after restoring the X frame. */
                raise(SIGSTOP);
            } else {
                note("Menu held, closing the session");
                if (watch_pid > 0)
                    kill(watch_pid, SIGTERM);
                break;
            }
        }

        /*
         * kill(pid, 0) rather than waitpid: the browser is a child of the session
         * SCRIPT and not of this process, so there is no status to reap here, only
         * a question to ask.  ESRCH is the answer that ends the session; EPERM
         * would mean the pid exists and belongs to someone else, which cannot
         * happen in a session that runs as one user but is not worth exiting over
         * if it somehow does.
         */
        if (watch_pid > 0 && kill(watch_pid, 0) < 0 && errno == ESRCH) {
            note("the browser exited");
            break;
        }
    }

    pointer_resync();
    pointer_state_write();
    close_pads();
    XTestGrabControl(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
