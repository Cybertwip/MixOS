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
#include <sys/types.h>

#include <linux/input.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

/* ── the pads ────────────────────────────────────────────────────────────── */

#define MAX_PADS 4

static int   pad_fd[MAX_PADS];
static char  pad_name[MAX_PADS][128];
static int   pads;

#define BITS_PER_LONG  (int)(sizeof(long) * 8)
#define NLONGS(x)      (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

static int verbose;

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

static void close_pads(void)
{
    int i;
    for (i = 0; i < pads; i++) {
        if (pad_fd[i] >= 0) {
            /* Explicit, though close() would do it: the ungrab is the thing that
             * gives the dashboard its buttons back, and it should be visible on
             * the page rather than implied by a close. */
            ioctl(pad_fd[i], EVIOCGRAB, 0);
            close(pad_fd[i]);
            pad_fd[i] = -1;
        }
    }
    pads = 0;
}

/*
 * Is this /dev/input/eventN the built-in pad?
 *
 * Positive test: it carries BTN_A and BTN_START.  Those two together are what the
 * device tree's key map puts on this board and what nothing else on it has.  A
 * touchscreen or a tablet has ABS_X and BTN_TOUCH and neither of these, so it is
 * excluded by the positive test without needing a rule of its own.
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

static int find_pads(int grab)
{
    DIR *d;
    struct dirent *e;
    char path[320];
    char name[128];
    int fd;

    d = opendir("/dev/input");
    if (!d) {
        fail("no /dev/input: %s", strerror(errno));
        return 0;
    }

    while ((e = readdir(d)) && pads < MAX_PADS) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        name[0] = '\0';
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
            snprintf(name, sizeof(name), "%s", path);
        name[sizeof(name) - 1] = '\0';

        if (!looks_like_pad(fd, name)) {
            close(fd);
            continue;
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

        pad_fd[pads] = fd;
        snprintf(pad_name[pads], sizeof(pad_name[pads]), "%s", name);
        note("using %s (%s)", path, name);
        pads++;
    }

    closedir(d);
    return pads;
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

/*
 * J36_AXIS_FULL_SCALE in j36_mt6592_input.c: the axes are registered -4096..4096
 * with fuzz 16 and flat 0, and the driver has already subtracted its own deadzone
 * from the raw ADC reading and rescaled what is left across the whole range.  So
 * what arrives here is centred on zero and reaches the ends at the stops.
 */
#define AXIS_FULL   4096.0

/*
 * A SECOND DEADZONE ON TOP OF THE DRIVER'S, small, and it earns its place: the
 * driver's is computed from a centre that was measured when the device tree was
 * written, and a stick that has been used for a year does not return to the same
 * place it did then.  Six per cent of travel is below what a thumb can aim and
 * above what wear produces.  Without it the pointer drifts on its own, and on a
 * device with no mouse to correct it that is the difference between a browser and
 * a cursor that walks into a corner while you read.
 */
#define STICK_DEAD  0.06

#define STICK_MAX   1100.0   /* pixels per second at full deflection */
#define SCROLL_MAX  16.0     /* wheel notches per second at full deflection */

static int axis_lx, axis_ly;   /* left stick  -- ABS_X, ABS_Y  */
static int axis_rx, axis_ry;   /* right stick -- ABS_Z, ABS_RZ */

/* Squared, sign-preserving, zero inside the deadzone.  See the response-curve
 * paragraph in the file header for why squared. */
static double stick(int raw, double full_scale_rate)
{
    double t = raw / AXIS_FULL;

    if (t > 1.0)
        t = 1.0;
    else if (t < -1.0)
        t = -1.0;
    if (t > -STICK_DEAD && t < STICK_DEAD)
        return 0.0;
    return (t < 0.0 ? -1.0 : 1.0) * t * t * full_scale_rate;
}

static int sticks_active(void)
{
    return stick(axis_lx, 1.0) != 0.0 || stick(axis_ly, 1.0) != 0.0
        || stick(axis_rx, 1.0) != 0.0 || stick(axis_ry, 1.0) != 0.0;
}

/* ── the D-pad ───────────────────────────────────────────────────────────── */

#define DIR_UP    0x1
#define DIR_DOWN  0x2
#define DIR_LEFT  0x4
#define DIR_RIGHT 0x8

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

    unsigned dirs = 0;
    long move_start = 0, move_last = 0;
    double frac_x = 0.0, frac_y = 0.0;
    double scroll_x = 0.0, scroll_y = 0.0;
    long menu_down_at = 0;

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
        pad_fd[i] = -1;

    if (list_only) {
        verbose = 1;
        if (!find_pads(0)) {
            fail("no built-in pad among /dev/input/event*");
            return 1;
        }
        for (i = 0; i < pads; i++)
            printf("%s\n", pad_name[i]);
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

    if (!find_pads(grab)) {
        fail("no built-in pad among /dev/input/event*, so nothing can drive this "
             "session -- run with --list to see what was rejected");
        XCloseDisplay(dpy);
        return 1;
    }

    move_last = now_ms();

    while (!stop_asked) {
        struct pollfd fds[MAX_PADS];
        int timeout, n;
        long now;
        double dt, dx, dy;
        int ix, iy;

        for (i = 0; i < pads; i++) {
            fds[i].fd = pad_fd[i];
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }

        /* One frame while something is moving or repeating, so motion runs at the
         * panel's own rate; a quarter of a second otherwise, which is short enough
         * that a browser that exits is noticed at once and long enough that an idle
         * session is not a process spinning.  A deflected stick counts as moving
         * even when no event has arrived, because an axis held still sends nothing
         * -- evdev reports changes, and a stick at rest against its stop has
         * stopped changing. */
        timeout = (dirs || menu_down_at || rep_any() || sticks_active()) ? TICK_MS : 250;
        n = poll(fds, (nfds_t)pads, timeout);
        if (n < 0 && errno != EINTR)
            break;

        now = now_ms();

        for (i = 0; i < pads && n > 0; i++) {
            struct input_event ev;

            if (!(fds[i].revents & POLLIN))
                continue;

            while (read(pad_fd[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                int down;

                if (ev.type == EV_ABS) {
                    /* Only the value is kept; what to do with it happens once per
                     * tick below, so a chatty ADC cannot make the pointer faster. */
                    switch (ev.code) {
                    case ABS_X:  axis_lx = ev.value; break;
                    case ABS_Y:  axis_ly = ev.value; break;
                    case ABS_Z:  axis_rx = ev.value; break;
                    case ABS_RZ: axis_ry = ev.value; break;
                    default: break;
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
                case KEY_UP:
                case KEY_DOWN:
                case KEY_LEFT:
                case KEY_RIGHT: {
                    unsigned bit = ev.code == KEY_UP    ? DIR_UP
                                 : ev.code == KEY_DOWN  ? DIR_DOWN
                                 : ev.code == KEY_LEFT  ? DIR_LEFT
                                                        : DIR_RIGHT;
                    unsigned was = dirs;
                    if (down)
                        dirs |= bit;
                    else
                        dirs &= ~bit;
                    /* The ramp belongs to the press and not to the direction: a
                     * thumb rolling from up to up-right is one movement and should
                     * not drop back to walking pace halfway across the screen. */
                    if (!was && dirs)
                        move_start = now;
                    break;
                }

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

        dx = stick(axis_lx, STICK_MAX) * dt;
        dy = stick(axis_ly, STICK_MAX) * dt;

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
        scroll_y += stick(axis_ry, SCROLL_MAX) * dt;
        while (scroll_y >= 1.0)  { wheel(5); scroll_y -= 1.0; }
        while (scroll_y <= -1.0) { wheel(4); scroll_y += 1.0; }
        scroll_x += stick(axis_rx, SCROLL_MAX) * dt;
        while (scroll_x >= 1.0)  { wheel(7); scroll_x -= 1.0; }
        while (scroll_x <= -1.0) { wheel(6); scroll_x += 1.0; }

        for (i = 0; i < REP_N; i++)
            rep_fire(i, now);

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
