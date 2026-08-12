/*
 * doomgeneric front end for the J36 Ultra: the framebuffer the stock LK left
 * running, the built-in gamepad, and nothing else.
 *
 * Derived from doomgeneric_linuxvt.c, Copyright (C) 2025 Techflash, itself
 * based on doomgeneric_sdl.c.  doomgeneric is GPLv2 and so is this file.
 *
 * WHY A FRAMEBUFFER PORT AT ALL.  The shared armhf rootfs already carries gzdoom
 * and lzdoom, and neither of them can draw here: SDL2 has no fbdev backend
 * (KMSDRM, X11, Wayland, offscreen and dummy are the whole list), the GL stack in
 * that rootfs is the RK3326's Mali-G31 Bifrost blob,
 * which is the wrong architecture for this SoC's Mali-450, and this kernel has
 * no DRM driver bound yet.  What it does have is /dev/fb0: simple-framebuffer
 * over the buffer the MVII LK was already scanning out when it jumped to the
 * kernel.  A 32-bit blit into that is the shortest path from "the panel shows
 * console text" to "the panel shows a moving picture", and it needs no
 * userspace on the card at all -- this runs from the initramfs, before
 * switch_root, so nothing on the shared rootfs is touched or even mounted.
 *
 * WHY NOT UPSTREAM'S doomgeneric_linuxvt.c, WHICH IS ALSO FBDEV.  Four reasons,
 * all of them fatal on this board:
 *
 *   - Its isKeyboard() accepts a device only if the EV_KEY bitmap contains both
 *     KEY_A and KEY_ENTER.  The one input device here is "J36 Ultra built-in
 *     gamepad", which advertises BTN_A..BTN_MODE, four arrows and two volume
 *     keys -- no KEY_A, no KEY_ENTER.  It finds nothing and dies in DG_Init
 *     with "Failed to find any compatible input device".
 *   - convertToDoomKey() knows no BTN_* code at all, so even a device that got
 *     past the filter would produce no input.
 *   - It hands every event it reads to the key queue, EV_ABS and EV_SYN
 *     included.  This gamepad reports two analog sticks on a 5 ms poll.
 *   - It leaves the VT in KD_TEXT, so fbcon keeps painting: every printk, and
 *     with journald forwarded to /dev/console every service line, lands on top
 *     of the frame.
 *
 * THE PIXEL FORMAT IS A COINCIDENCE WORTH RECORDING.  The device tree declares
 * the LK's buffer as format = "x8r8g8b8", so FBIOGET_VSCREENINFO reports 32 bpp
 * with red at bit 16, green at 8, blue at 0.  doomgeneric's default -gfxmode,
 * "rgba8888", packs (r << 16) | (g << 8) | (b << 0) -- the same word.  So the
 * whole blit is one memcpy per line and there is no swizzle anywhere in this
 * file.  If a future panel comes up as rgb565, doom can be told (-gfxmode
 * rgb565) but this file would need the matching 2-byte source stride, so it
 * refuses 16 bpp loudly instead of drawing garbage.
 *
 * 640x400 INTO A 640x480 PANEL.  doomgeneric renders 320x200 scaled by an
 * integer factor, chosen in i_video.c as min(xres/320, yres/200) -- so building
 * with DOOMGENERIC_RESY=480 would still pick 2, fill 400 lines of
 * DG_ScreenBuffer and leave the last 80 as malloc garbage (I_FinishUpdate
 * computes a y_offset and then never applies it).  Keep the build at 640x400,
 * which is a clean 2x, and letterbox it here: 40 black lines top and bottom of
 * a framebuffer this file clears once at startup.
 */

#include "doomkeys.h"
#include "doomgeneric.h"
#include "i_system.h"

/*
 * doomkeys.h and <linux/input-event-codes.h> both own the KEY_* namespace and
 * disagree about most of it.  Take the values this file needs out of doomkeys.h
 * first; from the #undef block onwards KEY_* means an evdev code and nothing
 * else.
 */
enum j36_doom_key {
	DK_LEFTARROW  = KEY_LEFTARROW,
	DK_RIGHTARROW = KEY_RIGHTARROW,
	DK_UPARROW    = KEY_UPARROW,
	DK_DOWNARROW  = KEY_DOWNARROW,
	DK_STRAFE_L   = KEY_STRAFE_L,
	DK_STRAFE_R   = KEY_STRAFE_R,
	DK_USE        = KEY_USE,
	DK_FIRE       = KEY_FIRE,
	DK_RSHIFT     = KEY_RSHIFT,
	DK_ESCAPE     = KEY_ESCAPE,
	DK_ENTER      = KEY_ENTER,
	DK_TAB        = KEY_TAB,
	DK_EQUALS     = KEY_EQUALS,
	DK_MINUS      = KEY_MINUS,
};

/* Exactly the names doomkeys.h and input-event-codes.h both define. */
#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12
#undef KEY_BACKSPACE
#undef KEY_PAUSE
#undef KEY_MINUS
#undef KEY_CAPSLOCK
#undef KEY_NUMLOCK
#undef KEY_HOME
#undef KEY_END

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

/* ── The framebuffer ──────────────────────────────────────────────────────── */

static uint8_t *fb_mem;
static size_t fb_len;
static int fb_fd = -1;
static unsigned fb_stride;
static unsigned fb_copy_w;	/* pixels per line actually blitted */
static unsigned fb_copy_h;	/* lines actually blitted */
static size_t fb_origin;	/* byte offset of doom's top-left pixel */

/* ── The console this is painting over ────────────────────────────────────── */

static int vt_fd = -1;
static long vt_saved_mode = KD_TEXT;
static int vt_switched;

/*
 * Put the VT back before the process goes away, however it goes away.  On a
 * board whose only output is this panel, a segfault that left fbcon in
 * KD_GRAPHICS looks exactly like a hang: the last frame stays up and nothing
 * the initramfs prints afterwards is ever seen.
 */
static void vt_restore(void)
{
	if (vt_switched && vt_fd >= 0) {
		ioctl(vt_fd, KDSETMODE, vt_saved_mode);
		vt_switched = 0;
	}
}

static void vt_restore_on_signal(int sig)
{
	vt_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void vt_take_over(void)
{
	static const char *const candidates[] = { "/dev/tty0", "/dev/tty1", NULL };
	int i;

	for (i = 0; candidates[i] != NULL; i++) {
		vt_fd = open(candidates[i], O_RDWR);
		if (vt_fd >= 0)
			break;
	}
	if (vt_fd < 0) {
		printf("j36: no VT to quiet down (%s); kernel messages will "
		       "paint over the frame\n", strerror(errno));
		return;
	}

	if (ioctl(vt_fd, KDGETMODE, &vt_saved_mode) < 0)
		vt_saved_mode = KD_TEXT;
	if (ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		printf("j36: KDSETMODE KD_GRAPHICS failed (%s); kernel messages "
		       "will paint over the frame\n", strerror(errno));
		return;
	}
	vt_switched = 1;
	atexit(vt_restore);
	signal(SIGSEGV, vt_restore_on_signal);
	signal(SIGBUS, vt_restore_on_signal);
	signal(SIGILL, vt_restore_on_signal);
	signal(SIGABRT, vt_restore_on_signal);
	signal(SIGTERM, vt_restore_on_signal);
	signal(SIGINT, vt_restore_on_signal);
}

/* ── The gamepad ──────────────────────────────────────────────────────────── */

#define J36_MAX_INPUT	4
#define KEYQUEUE_SIZE	32

static int in_fd[J36_MAX_INPUT];
static struct pollfd in_poll[J36_MAX_INPUT];
static int in_count;

static unsigned short key_queue[KEYQUEUE_SIZE];
static unsigned key_write, key_read;

static int quit_requested;

/*
 * The pad, as it is wired on this board.  Codes on the left are what
 * j36_mt6592_input.c reports, and they come from the device tree's
 * direct-key-map and matrix-key-map -- generate_dts.py builds both from the
 * MVII board header, so this is the stock button layout, not a guess:
 *
 *   D-pad            arrows          turn and walk
 *   left stick       arrows          same, latched below
 *   A                fire
 *   B                use / open
 *   X                run             (KEY_RSHIFT is doom's run modifier)
 *   Y                automap
 *   L1 / R1          strafe left / right
 *   L2 / R2          weapons 3 and 4
 *   stick clicks     weapons 1 and 2
 *   START            enter           menu select
 *   SELECT           escape          menu
 *   VOL- / VOL+      - / =           doom's screen-size keys
 *   MENU             quit            handled separately, see handle_event()
 *
 * KEY_F12, which the DT calls special-home, is deliberately unmapped: in doom
 * F12 is the multiplayer spy key and there is no multiplayer here.
 *
 * The four keyboard codes at the end are not for this board.  They cost six
 * lines and make a USB keyboard work if one is ever plugged into the OTG port,
 * which is worth it the first time the pad itself is what is being debugged.
 */
static unsigned char doom_key_for(unsigned int code)
{
	switch (code) {
	case KEY_UP:		return DK_UPARROW;
	case KEY_DOWN:		return DK_DOWNARROW;
	case KEY_LEFT:		return DK_LEFTARROW;
	case KEY_RIGHT:		return DK_RIGHTARROW;
	case BTN_A:		return DK_FIRE;
	case BTN_B:		return DK_USE;
	case BTN_X:		return DK_RSHIFT;
	case BTN_Y:		return DK_TAB;
	case BTN_TL:		return DK_STRAFE_L;
	case BTN_TR:		return DK_STRAFE_R;
	case BTN_TL2:		return '3';
	case BTN_TR2:		return '4';
	case BTN_THUMBL:	return '1';
	case BTN_THUMBR:	return '2';
	case BTN_START:		return DK_ENTER;
	case BTN_SELECT:	return DK_ESCAPE;
	case KEY_VOLUMEUP:	return DK_EQUALS;
	case KEY_VOLUMEDOWN:	return DK_MINUS;

	case KEY_ESC:		return DK_ESCAPE;
	case KEY_ENTER:		return DK_ENTER;
	case KEY_SPACE:		return DK_USE;
	case KEY_LEFTCTRL:	return DK_FIRE;
	case KEY_LEFTSHIFT:	return DK_RSHIFT;
	case KEY_TAB:		return DK_TAB;
	default:		return 0;
	}
}

/*
 * Drop rather than overwrite when the queue is full.  Overwriting the read
 * cursor -- which is what doomgeneric's other front ends do -- turns an
 * overflow into a stuck key: the release event is what gets lost, and doom then
 * walks into a wall until the next press of the same button.
 */
static void queue_key(int pressed, unsigned char key)
{
	unsigned next;

	if (key == 0)
		return;
	next = (key_write + 1) % KEYQUEUE_SIZE;
	if (next == key_read)
		return;
	key_queue[key_write] = (unsigned short)(((pressed ? 1 : 0) << 8) | key);
	key_write = next;
}

/*
 * The sticks report -4096..4096 with fuzz 16 (J36_AXIS_FULL_SCALE in
 * j36_mt6592_input.c) and doom has no analog input, so latch each axis into the
 * arrow keys with hysteresis: press past 44% of full scale, release back inside
 * 20%, and no events at all in between.  Without the gap a stick resting near a
 * threshold would emit a press/release pair every 5 ms poll.
 *
 * Positive ABS_Y means down, the evdev convention.  If it turns out inverted on
 * the hardware, the third cell of the DT's axis-map triple is where that is
 * fixed for every consumer at once -- not here.
 */
#define AXIS_PRESS	1800
#define AXIS_RELEASE	 800

static int axis_latched[2];

static void handle_axis(unsigned int code, int value)
{
	int slot, negative, positive, want;

	switch (code) {
	case ABS_X:
		slot = 0; negative = DK_LEFTARROW; positive = DK_RIGHTARROW;
		break;
	case ABS_Y:
		slot = 1; negative = DK_UPARROW; positive = DK_DOWNARROW;
		break;
	default:
		/* ABS_Z/ABS_RZ are the right stick; doom has nothing to do with it. */
		return;
	}

	want = axis_latched[slot];
	if (value >= AXIS_PRESS)
		want = 1;
	else if (value <= -AXIS_PRESS)
		want = -1;
	else if (value > -AXIS_RELEASE && value < AXIS_RELEASE)
		want = 0;

	if (want == axis_latched[slot])
		return;
	if (axis_latched[slot] != 0)
		queue_key(0, axis_latched[slot] < 0 ? negative : positive);
	if (want != 0)
		queue_key(1, want < 0 ? negative : positive);
	axis_latched[slot] = want;
}

static void handle_event(const struct input_event *ev)
{
	if (ev->type == EV_KEY) {
		/*
		 * MENU is the way out.  This runs from the initramfs with no
		 * keyboard and no window manager, so doom's own F10 quit dialog
		 * -- which wants a literal 'y' -- is unreachable.  I_Quit()
		 * rather than exit(): it runs doom's exit list, saves the
		 * config, and then the atexit above puts the console back.
		 * /init carries on with the boot afterwards.
		 */
		if (ev->code == BTN_MODE) {
			if (ev->value == 1)
				quit_requested = 1;
			return;
		}
		if (ev->value > 1)	/* evdev autorepeat; doom does its own */
			return;
		queue_key(ev->value, doom_key_for(ev->code));
	} else if (ev->type == EV_ABS) {
		handle_axis(ev->code, ev->value);
	}
}

static void pump_input(void)
{
	struct input_event ev[32];
	int rounds;

	if (in_count == 0)
		return;

	/* Bounded: a device that is somehow always readable must not stop doom. */
	for (rounds = 0; rounds < 64; rounds++) {
		int ready, i;

		ready = poll(in_poll, (unsigned)in_count, 0);
		if (ready <= 0)
			return;

		for (i = 0; i < in_count; i++) {
			ssize_t got;
			size_t k;

			if ((in_poll[i].revents & POLLIN) == 0)
				continue;
			got = read(in_fd[i], ev, sizeof(ev));
			if (got < (ssize_t)sizeof(ev[0]))
				continue;
			for (k = 0; k < (size_t)got / sizeof(ev[0]); k++)
				handle_event(&ev[k]);
		}
	}
}

#define BIT_SET(bits, n)	((bits)[(n) / 8] & (1 << ((n) % 8)))

static int looks_like_a_controller(int fd)
{
	unsigned long ev_bits = 0;
	unsigned char key_bits[KEY_MAX / 8 + 1];

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), &ev_bits) < 0)
		return 0;
	if ((ev_bits & (1UL << EV_KEY)) == 0)
		return 0;

	memset(key_bits, 0, sizeof(key_bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return 0;

	/* This board's pad, a generic gamepad, or a keyboard -- any will do. */
	return BIT_SET(key_bits, BTN_A) || BIT_SET(key_bits, KEY_UP) ||
	       BIT_SET(key_bits, KEY_ENTER);
}

static void open_input_devices(void)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir("/dev/input");
	if (dir == NULL) {
		printf("j36: /dev/input is not there (%s)\n", strerror(errno));
		return;
	}

	while ((entry = readdir(dir)) != NULL && in_count < J36_MAX_INPUT) {
		char path[280];
		char name[128];
		int fd;

		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			printf("j36: %s: %s\n", path, strerror(errno));
			continue;
		}
		if (!looks_like_a_controller(fd)) {
			close(fd);
			continue;
		}

		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
			snprintf(name, sizeof(name), "unnamed");
		printf("j36: input %s: %s\n", path, name);

		/*
		 * Grab it, so the busybox shell /init leaves on the panel does
		 * not also receive the buttons while doom has them.
		 */
		ioctl(fd, EVIOCGRAB, 1);

		in_poll[in_count].fd = fd;
		in_poll[in_count].events = POLLIN;
		in_fd[in_count] = fd;
		in_count++;
	}
	closedir(dir);

	/*
	 * Not fatal, unlike upstream.  With no input at all doom still runs its
	 * attract-mode demo, and a moving picture on the panel is exactly the
	 * measurement this build is for -- failing here would throw it away.
	 */
	if (in_count == 0)
		printf("j36: no input device found; the demo loop will play "
		       "unattended\n");
}

/* ── doomgeneric's side of the contract ───────────────────────────────────── */

static struct timespec start_time;

void DG_Init(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	const char *node = getenv("J36_FBDEV");
	unsigned bytes_per_pixel;

	if (node == NULL || node[0] == '\0')
		node = "/dev/fb0";

	fb_fd = open(node, O_RDWR);
	if (fb_fd < 0)
		I_Error("j36: cannot open %s: %s", node, strerror(errno));
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) < 0)
		I_Error("j36: FBIOGET_VSCREENINFO on %s: %s", node, strerror(errno));
	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0)
		I_Error("j36: FBIOGET_FSCREENINFO on %s: %s", node, strerror(errno));

	if (var.bits_per_pixel != 32)
		I_Error("j36: %s is %u bpp and this front end only blits 32; the "
			"device tree declares the LK buffer as x8r8g8b8",
			node, var.bits_per_pixel);
	if (var.red.offset != 16 || var.green.offset != 8 || var.blue.offset != 0)
		printf("j36: %s packs r%u g%u b%u, doomgeneric's default mode packs "
		       "r16 g8 b0; colours will be wrong\n", node,
		       var.red.offset, var.green.offset, var.blue.offset);

	bytes_per_pixel = var.bits_per_pixel / 8;
	fb_stride = fix.line_length ? fix.line_length : var.xres * bytes_per_pixel;
	fb_len = (size_t)fb_stride * var.yres;

	fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED)
		I_Error("j36: cannot mmap %zu bytes of %s: %s", fb_len, node,
			strerror(errno));

	/* Centre what fits.  A panel smaller than doom's frame is cropped, not
	 * wrapped, which is why both dimensions are clamped before the offset
	 * is computed -- the unsigned subtraction upstream does would wrap. */
	fb_copy_w = var.xres < DOOMGENERIC_RESX ? var.xres : DOOMGENERIC_RESX;
	fb_copy_h = var.yres < DOOMGENERIC_RESY ? var.yres : DOOMGENERIC_RESY;
	fb_origin = (size_t)((var.yres - fb_copy_h) / 2) * fb_stride +
		    (size_t)((var.xres - fb_copy_w) / 2) * bytes_per_pixel;

	/* Once, so the letterbox bars are black instead of leftover console. */
	memset(fb_mem, 0, fb_len);

	printf("j36: %s %ux%u %ubpp stride %u, doom %dx%d at +%zu\n", node,
	       var.xres, var.yres, var.bits_per_pixel, fb_stride,
	       DOOMGENERIC_RESX, DOOMGENERIC_RESY, fb_origin);

	vt_take_over();
	open_input_devices();

	clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void DG_DrawFrame(void)
{
	const uint8_t *src = (const uint8_t *)DG_ScreenBuffer;
	uint8_t *dst = fb_mem + fb_origin;
	unsigned line;

	for (line = 0; line < fb_copy_h; line++) {
		memcpy(dst, src, (size_t)fb_copy_w * 4);
		dst += fb_stride;
		src += (size_t)DOOMGENERIC_RESX * 4;
	}

	pump_input();

	if (quit_requested)
		I_Quit();
}

void DG_SleepMs(uint32_t ms)
{
	struct timespec req;

	req.tv_sec = (time_t)(ms / 1000);
	req.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&req, NULL);
}

uint32_t DG_GetTicksMs(void)
{
	struct timespec now;

	/* Monotonic, not gettimeofday: doom's whole clock is this function and
	 * a wall-clock step would move it. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start_time.tv_sec) * 1000 +
			  (now.tv_nsec - start_time.tv_nsec) / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	unsigned short data;

	pump_input();

	if (key_read == key_write)
		return 0;

	data = key_queue[key_read];
	key_read = (key_read + 1) % KEYQUEUE_SIZE;
	*pressed = data >> 8;
	*doomKey = data & 0xff;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	/* There is no window.  Say it on the serial console, where /init sends
	 * this process's stdout, so the log records which IWAD was loaded. */
	printf("j36: %s\n", title);
}

int main(int argc, char **argv)
{
	doomgeneric_Create(argc, argv);

	for (;;)
		doomgeneric_Tick();

	return 0;
}
