/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * console.cpp -- the VT mode and the stream.  console.h says why.
 *
 * NO Qt IN THIS FILE, on purpose.  text() is reachable from a signal handler by way
 * of main.cpp's textMode(), and hold() is reachable from anywhere at all including
 * the middle of a blocking wait, so what those two may call is the async-signal-safe
 * list and nothing else: open, close, dup2, ioctl, write, lseek, ftruncate.  A
 * QString in either would be a malloc inside SIGSEGV.
 *
 * toLog() is the one exception and it is a deliberate one: it is called exactly once,
 * from the first paint, on the event loop, and it needs stdio's buffer flushed before
 * it moves the descriptor out from under it.  Nothing calls it from a handler.
 */
#include "console.h"

#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

/* The VT.  -1 when there was none to open, which every function below tolerates. */
int g_tty = -1;

/*
 * Set once take() has decided the panel belongs to the dashboard.  It is NOT the
 * same question as "did this process set the mode": Qt's linuxfb plugin sets
 * KD_GRAPHICS itself on a card whose unit file predates `nographicsmodeswitch', and
 * on that card take() finds the mode already right and sets nothing -- but the
 * panel is still the dashboard's and still has to be held, because Qt's plugin
 * takes the mode once and never looks at it again.
 */
volatile sig_atomic_t g_held = 0;

/* The stream, when it has been moved off the console. */
int g_log = -1;
int g_savedOut = -1;
int g_savedErr = -1;
volatile sig_atomic_t g_redirected = 0;

/*
 * 256 KB, which is about four thousand Qt warnings and about four seconds of a
 * process that has genuinely run away.  Small enough that /run cannot be filled by
 * it and large enough that the interesting first page of a real problem is still
 * there when somebody looks.
 */
const long kLogMax = 256L * 1024L;

void restoreStream()
{
    if (!g_redirected)
        return;
    /* Cleared FIRST: this runs from signal handlers, and a second signal arriving
     * between the dup2s must not start the same work again. */
    g_redirected = 0;
    if (g_savedOut >= 0)
        ::dup2(g_savedOut, 1);
    if (g_savedErr >= 0)
        ::dup2(g_savedErr, 2);
    /* The saved descriptors are deliberately not closed.  text() is reached from
     * atexit AND from every fatal handler, so it can run more than once, and a
     * closed fd here would turn the second report into a write to nothing. */
}

/*
 * Wrapped round rather than rotated, and the difference is one open file instead of
 * two.  fd 1, fd 2 and g_log all share ONE open file description -- that is what
 * dup2 makes -- so the offset lseek reports is the one the writes are using, and
 * putting it back to zero after a truncate is enough to start the file again.
 */
void trimLog()
{
    if (g_log < 0)
        return;
    if (::lseek(g_log, 0, SEEK_CUR) < kLogMax)
        return;
    if (::ftruncate(g_log, 0) == 0)
        ::lseek(g_log, 0, SEEK_SET);
}

} /* namespace */

namespace Console {

bool open()
{
    if (g_tty >= 0)
        return true;
    g_tty = ::open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
    /* tty1 for a kernel or an initramfs that only made the numbered nodes -- the
     * same fallback mixsplash's console_grab() has, and for the same boards. */
    if (g_tty < 0)
        g_tty = ::open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
    return g_tty >= 0;
}

void take()
{
    int mode = KD_TEXT;

    if (g_tty < 0)
        return;
    /*
     * The flag goes up whether or not the ioctl was needed, because it means "the
     * dashboard is on the panel now" and not "this call changed something".  hold()
     * reads it, and a card where Qt had already taken the mode needs holding just
     * as much as one where this did.
     */
    g_held = 1;
    if (::ioctl(g_tty, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS)
        return;
    ::ioctl(g_tty, KDSETMODE, KD_GRAPHICS);
}

bool hold()
{
    int mode = KD_GRAPHICS;

    if (g_tty < 0 || !g_held)
        return false;

    trimLog();

    if (::ioctl(g_tty, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS)
        return false;
    if (::ioctl(g_tty, KDSETMODE, KD_GRAPHICS) != 0)
        return false;
    return true;
}

void text()
{
    /* The stream first, so that a caller which writes its report immediately after
     * this returns writes it to the panel and not to the file. */
    restoreStream();
    g_held = 0;
    if (g_tty >= 0)
        ::ioctl(g_tty, KDSETMODE, KD_TEXT);
}

void toLog(const char *path)
{
    int fd;

    if (g_redirected || path == NULL)
        return;

    fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return;

    /*
     * THE BUFFER BEFORE THE DESCRIPTOR.  dup2 moves where fd 1 points; it does not
     * move what stdio has already accumulated behind it, and on this board fd 1 is a
     * socket to journald rather than a terminal, so stdio is fully buffered and the
     * build banner announceBuild() printed seconds ago can still be sitting in it.
     * Without this it would be written out on the next printf -- into the log, which
     * is the one place the banner is no use, and possibly split across the two
     * destinations mid-line.
     */
    ::fflush(NULL);

    /*
     * F_DUPFD_CLOEXEC and not dup(2): these two live for the rest of the process
     * and every page here spawns children, so a plain dup would hand each of them
     * a spare console to print on -- which is the exact bug this function is
     * closing, moved one file descriptor to the left.
     */
    if (g_savedOut < 0)
        g_savedOut = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (g_savedErr < 0)
        g_savedErr = ::fcntl(2, F_DUPFD_CLOEXEC, 3);

    /*
     * The children are the point as much as this process is.  QProcess inherits fd
     * 1 and fd 2 unless the caller redirects them, and the pages here mostly use
     * MergedChannels, which does -- but "mostly" is not a guarantee anybody can
     * hold, and a helper that forks a shell of its own is outside it entirely.
     * After this, every one of them writes into the log by default.
     */
    ::dup2(fd, 1);
    ::dup2(fd, 2);
    ::close(fd);

    /* Kept open as a third name for the same file description, purely so trimLog()
     * has something to lseek and ftruncate that is not fd 1. */
    g_log = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    g_redirected = 1;
}

} /* namespace Console */
