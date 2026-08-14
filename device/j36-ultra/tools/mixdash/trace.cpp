/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "trace.h"

#include <execinfo.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

/* Literals only.  See the note in trace.h. */
const char *volatile g_phase = "start";
const char *volatile g_step = "";
struct timespec g_t0;
int g_deadline = 90;

} /* namespace */

namespace Trace {

void begin()
{
    ::clock_gettime(CLOCK_MONOTONIC, &g_t0);

    /*
     * glibc's backtrace() has no unwinder of its own: it dlopens libgcc_s.so.1 the
     * first time it is called.  The first time alloc.cpp calls it is from inside an
     * allocation that has just failed, which is the worst possible moment to be
     * asking the loader for a library.  Called once here, where it costs nothing and
     * cannot fail for want of memory.
     */
    void *frame[2];
    (void)::backtrace(frame, 2);
}

void setDeadline(int seconds)
{
    g_deadline = seconds;
}

double elapsed()
{
    struct timespec now;
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec - g_t0.tv_sec) + double(now.tv_nsec - g_t0.tv_nsec) / 1e9;
}

void writeAll(int fd, const char *s)
{
    size_t left = ::strlen(s);
    while (left > 0) {
        const ssize_t n = ::write(fd, s, left);
        if (n <= 0)
            return;
        s += n;
        left -= size_t(n);
    }
}

void writeUnsigned(int fd, unsigned long value)
{
    /* Backwards into a stack buffer: no printf, no locale, no allocation, which is
     * the whole point -- this prints the size of an allocation that just failed. */
    char buf[24];
    int i = int(sizeof(buf));
    buf[--i] = '\0';
    if (value == 0)
        buf[--i] = '0';
    while (value > 0 && i > 0) {
        buf[--i] = char('0' + (value % 10));
        value /= 10;
    }
    writeAll(fd, buf + i);
}

void backtraceTo(int fd)
{
    void *frames[32];
    const int n = ::backtrace(frames, 32);
    writeAll(fd, "mixdash: backtrace:\n");
    ::backtrace_symbols_fd(frames, n, fd);
}

void phase(const char *name)
{
    g_phase = name;
    g_step = "";
    ::printf("mixdash: [%7.2fs] %s\n", elapsed(), name);
    ::fflush(stdout);
    if (g_deadline > 0)
        ::alarm(unsigned(g_deadline));
}

void step(const char *name)
{
    g_step = name;
    ::printf("mixdash: [%7.2fs]   . %s\n", elapsed(), name);
    ::fflush(stdout);
    if (g_deadline > 0)
        ::alarm(unsigned(g_deadline));
}

const char *current()
{
    return g_phase;
}

const char *currentStep()
{
    return g_step;
}

} /* namespace Trace */
