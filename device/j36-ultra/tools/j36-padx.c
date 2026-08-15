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
 * THE PAD IS GRABBED, and that is not an optimisation.  mixdash is still running
 * behind the X server -- it is the process that launched it -- and its own reader
 * has /dev/input/event* open.  Without EVIOCGRAB every press would be delivered
 * twice: once here, and once to a dashboard that would walk its card grid behind
 * the browser and act on whatever it landed on.  EVIOCGRAB makes the kernel
 * deliver this device's events to this fd and nowhere else, so for as long as this
 * runs the whole pad -- sticks included, since they are on the same device -- is
 * X's.  It is released by closing the fd, which happens on every exit path
 * including a signal, so a crash here gives the pad back rather than wedging it.
 *
 * WHAT IS DELIBERATELY NOT GRABBED.  A USB keyboard or mouse in the dock is a
 * real X input device that libinput will pick up and drive properly, and it must
 * keep working.  So the match is narrow -- see looks_like_pad() -- and a device
 * that looks like a keyboard (it has the letter keys) or like a mouse (it has
 * EV_REL) is left alone even if something about it also looks like a pad.
 *
 * A USB PAD IS TAKEN TOO, and on purpose.  An Xbox, PlayStation or Switch pad in
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
 *     D-pad          pointer, accelerating while held
 *     A              left click
 *     B              Back            (Alt+Left)
 *     X              Return
 *     Y              Escape
 *     L1 / R1        wheel up / wheel down, repeating while held
 *     L2 / R2        Page Up / Page Down, repeating while held
 *     L3 / R3        middle click / right click
 *     Select         show or hide the on-screen keyboard
 *     Start          focus the address bar   (Ctrl+L)
 *     Menu           HOLD to close the session
 *     Vol- / Vol+    zoom out / zoom in      (Ctrl+minus / Ctrl+plus)
 *     Home (F12)     the start page          (Alt+Home)
 *
 * BOTH the left stick and the D-pad move the pointer, and that is not redundancy.
 * A stick is the right instrument for crossing the screen and a poor one for
 * landing on a twelve-pixel link, because it never comes back to exactly centre;
 * a D-pad is the opposite, and a tap of it is a nudge of two or three pixels.
 * They add, so one thumb can do both without a mode to switch between them.
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
 * remainder is carried between ticks (see the motion block in main) so that the
 * slow end still moves at all; XTEST takes whole pixels, and a per-tick delta that
 * rounds to zero is a pointer that does not move.
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

#include <X11/Xlib.h>
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
static double ax_dead[MAX_PADS][AX_N];    /* fraction of travel, 0..0.5 */
static double ax_val[MAX_PADS][AX_N];     /* -1..1, deadzone already removed */

#define BITS_PER_LONG  (int)(sizeof(long) * 8)
#define NLONGS(x)      (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int verbose;

/* When the current D-pad hold started, for the acceleration ramp.  File scope
 * because set_dir() is what notices the hold beginning and it is called from
 * three places. */
static long move_start;

/* The two pieces of held-button state that are not per-pad.  They live here, and
 * the repeat clock is reached through a forward declaration, because drop_pad()
 * has to cancel both and it is defined long before either: see the comment there. */
static long menu_down_at;
static void rep_clear(void);

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
    if (pad_fd[slot] < 0)
        return;
    note("%s went away", pad_name[slot]);
    ioctl(pad_fd[slot], EVIOCGRAB, 0);
    close(pad_fd[slot]);
    clear_pad(slot);

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
 * pad is on the other end.  `flat' is the driver's own idea of its deadzone and is
 * honoured when it is the larger of the two -- xpad says 128 counts out of 32768,
 * which is a tenth of what is wanted here, and hid-playstation says nothing at all.
 *
 * The current value is deliberately NOT read into ax_val.  A stick at rest sends
 * no events, so zero is the honest starting point; seeding from EVIOCGABS would
 * mean a pad that happened to be pushed over at startup left the pointer drifting
 * until it was touched.
 */
static int learn_axis(int slot, int role, int fd, int code)
{
    struct input_absinfo ai;
    double half, dead;

    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
        return 0;
    if (ai.maximum <= ai.minimum)
        return 0;

    half = ((double)ai.maximum - (double)ai.minimum) / 2.0;
    dead = ai.flat > 0 ? (double)ai.flat / half : 0.0;
    if (dead < 0.06)
        dead = 0.06;
    if (dead > 0.5)
        dead = 0.5;

    ax_centre[slot][role] = ((double)ai.maximum + (double)ai.minimum) / 2.0;
    ax_half[slot][role]   = half;
    ax_dead[slot][role]   = dead;
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

/* The keycodes are resolved once, at startup, because XKeysymToKeycode walks the
 * server's whole keymap and the answer cannot change without a keymap change --
 * and there is no keymap change in this session, since the only keyboard driving
 * it is this program. */
static struct {
    KeyCode ret, esc, page_up, page_down, home, left;
    KeyCode alt, ctrl, minus, plus, l;
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
    kc.alt       = want(XK_Alt_L,     "Alt");
    kc.ctrl      = want(XK_Control_L, "Control");
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

#define STICK_MAX   1100.0   /* pixels per second at full deflection */
#define SCROLL_MAX  16.0     /* wheel notches per second at full deflection */

/*
 * Raw reading to -1..1, with the deadzone taken out and what is left stretched
 * back over the whole range, so that the first movement past the deadzone is a
 * small one rather than a step.
 *
 * The deadzone is the second one this value passes through: the driver has
 * already subtracted its own, computed from a centre measured when the device
 * tree was written.  A stick used for a year does not come back to where it did
 * then, and on a device with no mouse to correct it, a pointer that drifts on its
 * own walks into a corner while you read.  Six per cent of travel is below what a
 * thumb can aim and above what wear produces.
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

    d = ax_dead[slot][role];
    if (t > -d && t < d)
        return 0.0;
    return (t < 0.0 ? t + d : t - d) / (1.0 - d);
}

/* Squared and sign-preserving, summed over every pad and clamped: two pads is not
 * a real arrangement, and if it happens the answer should be a pointer that moves
 * once rather than one that moves twice as fast. */
static double axis_rate(int role, double full_scale_rate)
{
    double s = 0.0;
    int i;

    for (i = 0; i < MAX_PADS; i++) {
        double t = ax_val[i][role];
        if (t != 0.0)
            s += (t < 0.0 ? -1.0 : 1.0) * t * t;
    }
    if (s > 1.0)
        s = 1.0;
    else if (s < -1.0)
        s = -1.0;
    return s * full_scale_rate;
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

/* Pixels per second at the start of a press and at the end of the ramp, and how
 * long the ramp takes. */
#define SPEED_MIN   110.0
#define SPEED_MAX   1000.0
#define RAMP_MS     900.0

static double speed_at(long held_ms)
{
    double t = (double)held_ms / RAMP_MS;
    if (t > 1.0)
        t = 1.0;
    return SPEED_MIN + (SPEED_MAX - SPEED_MIN) * t * t;
}

static unsigned dirs_all(void)
{
    unsigned d = 0;
    int i;
    for (i = 0; i < MAX_PADS; i++)
        d |= pad_dirs[i];
    return d;
}

/*
 * The ramp belongs to the hold and not to the direction: a thumb rolling from up
 * to up-right is one movement and should not drop back to walking pace halfway
 * across the screen.  So it restarts only when nothing was held and something now
 * is -- which is also why this is a function rather than three copies, since the
 * same transition arrives as a key, as a BTN_DPAD_* and as a hat.
 */
static void set_dir(int slot, unsigned bit, int on, long now)
{
    unsigned was = dirs_all();

    if (on)
        pad_dirs[slot] |= bit;
    else
        pad_dirs[slot] &= ~bit;
    if (!was && dirs_all())
        move_start = now;
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

static void usage(void)
{
    fprintf(stderr,
        "j36-padx -- the J36 Ultra pad as an X pointer and keyboard\n"
        "\n"
        "  --watch PID       quit when PID exits; Menu held sends it SIGTERM\n"
        "  --keyboard PID    Select sends SIGUSR1 to PID (the on-screen keyboard)\n"
        "  --display NAME    which server (default $DISPLAY)\n"
        "  --no-grab         read the pad without taking it from other readers\n"
        "  --list            name the devices that would be used, then exit\n"
        "  -v                say what is happening\n");
}

int main(int argc, char **argv)
{
    const char *display_name = NULL;
    pid_t watch_pid = 0, kbd_pid = 0;
    int grab = 1, list_only = 0;
    int i, xt_event, xt_error, xt_major, xt_minor;

    long move_last = 0, next_scan = 0;
    double frac_x = 0.0, frac_y = 0.0;
    double scroll_x = 0.0, scroll_y = 0.0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch") && i + 1 < argc)
            watch_pid = (pid_t)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--keyboard") && i + 1 < argc)
            kbd_pid = (pid_t)strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--display") && i + 1 < argc)
            display_name = argv[++i];
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

    dpy = XOpenDisplay(display_name);
    if (!dpy) {
        fail("no X server on %s", display_name ? display_name
                                               : (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return 1;
    }
    XSetIOErrorHandler(on_io_error);

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

    if (!scan_pads(grab)) {
        fail("no pad among /dev/input/event*, so nothing can drive this session "
             "-- run with --list to see what was rejected");
        XCloseDisplay(dpy);
        return 1;
    }

    move_last = now_ms();
    next_scan = move_last + RESCAN_MS;

    while (!stop_asked) {
        struct pollfd fds[MAX_PADS];
        int map[MAX_PADS];
        int nfd = 0, timeout, n, k;
        long now;
        double dt, dx, dy;
        unsigned dirs;
        int ix, iy;

        for (i = 0; i < MAX_PADS; i++) {
            if (pad_fd[i] < 0)
                continue;
            map[nfd] = i;
            fds[nfd].fd = pad_fd[i];
            fds[nfd].events = POLLIN;
            fds[nfd].revents = 0;
            nfd++;
        }

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

        for (k = 0; k < nfd && n > 0; k++) {
            struct input_event ev;
            int slot = map[k];
            int gone = 0;

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
                        set_dir(slot, DIR_LEFT,  ev.value < 0, now);
                        set_dir(slot, DIR_RIGHT, ev.value > 0, now);
                        continue;
                    }
                    if (ev.code == ABS_HAT0Y) {
                        set_dir(slot, DIR_UP,   ev.value < 0, now);
                        set_dir(slot, DIR_DOWN, ev.value > 0, now);
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
                case BTN_DPAD_UP:    set_dir(slot, DIR_UP,    down, now); break;
                case KEY_DOWN:
                case BTN_DPAD_DOWN:  set_dir(slot, DIR_DOWN,  down, now); break;
                case KEY_LEFT:
                case BTN_DPAD_LEFT:  set_dir(slot, DIR_LEFT,  down, now); break;
                case KEY_RIGHT:
                case BTN_DPAD_RIGHT: set_dir(slot, DIR_RIGHT, down, now); break;

                case BTN_A:      click(1, down); break;
                case BTN_THUMBL: click(2, down); break;
                case BTN_THUMBR: click(3, down); break;

                case BTN_B: if (down) tap(kc.alt, kc.left); break;
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
                    /*
                     * The on-screen keyboard is matchbox-keyboard started with
                     * --daemon, which is hidden until it is sent SIGUSR1 and
                     * toggles on every one after that.  Nothing here tracks
                     * whether it is up: the keyboard owns that state, and a
                     * counter kept on this side would drift the first time
                     * anything else toggled it.
                     */
                    if (down && kbd_pid > 0 && kill(kbd_pid, SIGUSR1) < 0)
                        fail("the on-screen keyboard is gone (%s)", strerror(errno));
                    break;

                case BTN_MODE:
                    menu_down_at = down ? now : 0;
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

        dx = axis_rate(AX_LX, STICK_MAX) * dt;
        dy = axis_rate(AX_LY, STICK_MAX) * dt;

        dirs = dirs_all();
        if (dirs) {
            double v = speed_at(now - move_start);
            double px = 0.0, py = 0.0;
            if (dirs & DIR_LEFT)  px -= v * dt;
            if (dirs & DIR_RIGHT) px += v * dt;
            if (dirs & DIR_UP)    py -= v * dt;
            if (dirs & DIR_DOWN)  py += v * dt;
            /* A diagonal would otherwise travel sqrt(2) times as far as a straight
             * line for the same hold, which reads as the pad being faster on the
             * slant. */
            if (px != 0.0 && py != 0.0) {
                px *= 0.7071;
                py *= 0.7071;
            }
            dx += px;
            dy += py;
        } else {
            move_start = now;
        }

        /* The remainder is kept, so a speed below one pixel per tick is a slow
         * pointer and not a still one. */
        frac_x += dx;
        frac_y += dy;
        ix = (int)frac_x;
        iy = (int)frac_y;
        frac_x -= ix;
        frac_y -= iy;
        if (ix || iy) {
            XTestFakeRelativeMotionEvent(dpy, ix, iy, 0);
            XFlush(dpy);
        }

        /* The right stick, as a wheel.  Same carry: a notch is a discrete thing, so
         * what accumulates is fractions of one.  Buttons 6 and 7 are the horizontal
         * wheel; a client that does not understand them ignores them, which is the
         * right outcome for a page that does not scroll sideways. */
        scroll_y += axis_rate(AX_RY, SCROLL_MAX) * dt;
        while (scroll_y >= 1.0)  { wheel(5); scroll_y -= 1.0; }
        while (scroll_y <= -1.0) { wheel(4); scroll_y += 1.0; }
        scroll_x += axis_rate(AX_RX, SCROLL_MAX) * dt;
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
            scan_pads(grab);
        }

        if (menu_down_at && now - menu_down_at >= MENU_HOLD_MS) {
            note("Menu held, closing the session");
            if (watch_pid > 0)
                kill(watch_pid, SIGTERM);
            break;
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

    close_pads();
    XTestGrabControl(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
