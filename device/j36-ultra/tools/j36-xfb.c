/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * j36-xfb.so -- give Xorg a shared-memory framebuffer instead of the panel.
 *
 * Xorg's fbdev driver has a useful cached ShadowFB, but its final damage copy is
 * still a memcpy into the mmap of /dev/fb0.  The MixOS dashboard also uses that
 * framebuffer directly.  Stopping the complete X service used to arbitrate the
 * two writers, but it stopped Firefox's clocks and compositor too: after every
 * task-switch pause Firefox reported multi-second renderer stalls, and eventually
 * killed its SWGL compositor.
 *
 * This tiny preload changes only Xorg's mmap of the configured framebuffer.  The
 * same length and offset are mapped from a file in /run instead, so Xorg and every
 * client remain live and render normally while the dashboard is visible.  PadX
 * presents changed rows from that file to the real panel only while a windowed
 * task owns the screen.  Pausing presentation is consequently instantaneous and
 * never pauses an application, the X server, or a shutdown signal.
 *
 * Fail closed.  If the private mapping cannot be made, return MAP_FAILED rather
 * than silently handing Xorg the real framebuffer and reintroducing two writers.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fb.h>

typedef void *(*mmap64_fn)(void *, size_t, int, int, int, off64_t);

static mmap64_fn real_mmap64;
static int shadow_fd = -1;
static int warned;

static const char *device_path(void)
{
    const char *p = getenv("J36_XFB_DEVICE");
    return p && *p ? p : "/dev/fb0";
}

static const char *shadow_path(void)
{
    const char *p = getenv("J36_XFB_SHADOW");
    return p && *p ? p : NULL;
}

static void say_once(const char *what)
{
    if (warned)
        return;
    warned = 1;
    dprintf(STDERR_FILENO, "j36-xfb: %s: %s\n", what, strerror(errno));
}

static int is_framebuffer(int fd)
{
    struct stat have, want;

    if (fd < 0 || fstat(fd, &have) < 0 || stat(device_path(), &want) < 0)
        return 0;
    return S_ISCHR(have.st_mode) && S_ISCHR(want.st_mode)
        && have.st_rdev == want.st_rdev;
}

static int private_fd(uint64_t need)
{
    const char *path = shadow_path();
    struct stat st;

    if (!path) {
        errno = ENOENT;
        say_once("J36_XFB_SHADOW is not set");
        return -1;
    }
    if (need > (uint64_t)INT64_MAX) {
        errno = EOVERFLOW;
        say_once("framebuffer mapping is too large");
        return -1;
    }
    if (shadow_fd < 0) {
        shadow_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (shadow_fd < 0) {
            say_once("cannot open the private framebuffer");
            return -1;
        }
    }
    if (fstat(shadow_fd, &st) < 0) {
        say_once("cannot stat the private framebuffer");
        return -1;
    }
    if ((uint64_t)st.st_size < need
        && ftruncate(shadow_fd, (off_t)need) < 0) {
        say_once("cannot size the private framebuffer");
        return -1;
    }
    return shadow_fd;
}

static mmap64_fn next_mmap64(void)
{
    if (!real_mmap64)
        real_mmap64 = (mmap64_fn)dlsym(RTLD_NEXT, "mmap64");
    return real_mmap64;
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd,
             off64_t offset)
{
    mmap64_fn call = next_mmap64();

    if (!call) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    if (is_framebuffer(fd)) {
        struct fb_fix_screeninfo fix;
        uint64_t end, need;
        int private;

        if (offset < 0 || (uint64_t)length > UINT64_MAX - (uint64_t)offset) {
            errno = EOVERFLOW;
            return MAP_FAILED;
        }
        end = (uint64_t)offset + (uint64_t)length;
        memset(&fix, 0, sizeof fix);
        if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
            say_once("cannot size the real framebuffer");
            return MAP_FAILED;
        }
        /* PadX maps fix.smem_len later.  fbdev normally asks Xorg for that exact
         * length, but sizing it explicitly keeps a short driver mapping from
         * turning the presenter's final pages into SIGBUS. */
        need = end;
        if ((uint64_t)fix.smem_len > need)
            need = (uint64_t)fix.smem_len;
        private = private_fd(need);
        if (private < 0) {
            return MAP_FAILED;
        }
        return call(addr, length, prot, flags, private, offset);
    }
    return call(addr, length, prot, flags, fd, offset);
}
