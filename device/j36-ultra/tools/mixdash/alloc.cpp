/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * alloc.cpp -- the two places a failure throws its own evidence away: the allocation
 *              that returns null, and the throw that reports it.
 *
 * WHAT THIS IS FOR.  The dashboard died with
 *
 *     terminate called after throwing an instance of 'std::bad_alloc'
 *     mixdash: died in phase "Dashboard -- four pages, the dock and the evdev map"
 *
 * and that is everything the C++ runtime is willing to say.  std::bad_alloc carries
 * no size, no address and no stack; what::() returns the string "std::bad_alloc".
 * So the one number that decides what kind of bug this is -- HOW BIG the request was
 * -- is thrown away at the throw site and cannot be recovered afterwards.
 *
 * WHY THE SIZE IS THE WHOLE QUESTION.  This board has 1 GB (the device tree's
 * memory@80000000 is 0x40000000) and the process is 32-bit, so it has ~3 GB of
 * address space and had been running for a quarter of a second.  malloc does not
 * return null under those conditions unless it is asked for something absurd.  A
 * seven-digit size means memory really did run out and the leak is upstream; a
 * ten-digit one means some length was computed as a negative number and widened into
 * a size_t, and the backtrace names the line that did it.  Those are opposite bugs
 * and the message above cannot tell them apart.
 *
 * WHAT IT COSTS.  One comparison per allocation.  malloc is what the default
 * operator new calls anyway, so nothing about the allocator changes; the replacement
 * only adds the branch that was missing and the report on the way to the throw.
 *
 * WHY REPLACING operator new IS NOT ENOUGH, AND WHAT __cxa_throw IS DOING HERE.
 * Qt's containers do not allocate through operator new.  QArrayData::allocate calls
 * qMallocAligned, which calls ::malloc; when that returns null the caller's
 * Q_CHECK_PTR calls qBadAlloc(), which throws std::bad_alloc from inside libQt5Core
 * without operator new ever being entered.  So a QString, QByteArray, QVector or
 * QImage that cannot be allocated produces exactly the console this file was written
 * to explain -- bad_alloc, no size, no frame -- and the replacement above sees
 * nothing at all.
 *
 * Interposing malloc itself would catch that, and it is not worth what it costs: a
 * malloc replacement must own free, realloc, calloc, memalign and everything in libc
 * that hands out a pointer, and the allocations made before its own initialiser runs
 * have to come from somewhere that free() can survive being handed.  That is a lot of
 * new failure modes bolted underneath a program in order to diagnose one.
 *
 * __cxa_throw is where those paths converge instead.  Every C++ throw goes through it,
 * on every thread, from every library -- and it runs BEFORE any unwinding, so the
 * stack it stands on is the stack that threw.  That is the frame the catch in main()
 * cannot report, because by the time a handler runs the frames below it are gone.
 * The type comes from the std::type_info the ABI passes in, printed mangled, since
 * demangling allocates: std::bad_alloc reads as `St9bad_alloc'.
 *
 * NOTHING BELOW ALLOCATES.  It runs at the moment allocation has just failed, so
 * printf -- which can allocate, and which takes a lock -- is not used.  Trace's
 * writeAll and writeUnsigned go straight to write(2), backtrace_symbols_fd is the
 * documented malloc-free half of the backtrace pair, and Trace::begin() has already
 * made backtrace() dlopen its unwinder.
 */
#include "trace.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <typeinfo>
#include <unistd.h>

namespace {

/*
 * Reported even when it SUCCEEDS.  A dashboard at 640x480 has no honest reason to
 * ask for eight megabytes in one piece, so a request this size is worth a line
 * whether or not it is the one that eventually fails -- the allocation before the
 * fatal one is usually the same code path getting away with it.
 */
const size_t kLoud = 8u * 1024 * 1024;

/* Capped, because a program that legitimately makes big allocations in a loop would
 * otherwise fill the console with them and scroll the useful line away. */
int g_loudLeft = 8;

void writeVmLines()
{
    char buf[4096];
    const int fd = ::open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return;
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    /* Vm* and Threads: enough to separate "the address space is full" from "one
     * request was nonsense", which is the question this whole file exists to
     * answer. */
    for (char *line = buf; line != NULL && *line != '\0'; ) {
        char *end = ::strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        if (::strncmp(line, "Vm", 2) == 0 || ::strncmp(line, "Threads", 7) == 0) {
            Trace::writeAll(2, "mixdash:   ");
            Trace::writeAll(2, line);
            Trace::writeAll(2, "\n");
        }
        line = (end != NULL) ? end + 1 : NULL;
    }
}

void report(const char *what, size_t bytes, bool withMemory)
{
    Trace::writeAll(2, "\nmixdash: ");
    Trace::writeAll(2, what);
    Trace::writeAll(2, " ");
    Trace::writeUnsigned(2, (unsigned long)bytes);
    Trace::writeAll(2, " bytes (");
    Trace::writeUnsigned(2, (unsigned long)(bytes / 1024));
    Trace::writeAll(2, " KiB) in phase \"");
    Trace::writeAll(2, Trace::current());
    Trace::writeAll(2, "\", step \"");
    Trace::writeAll(2, Trace::currentStep());
    Trace::writeAll(2, "\"\n");

    if (withMemory)
        writeVmLines();

    /*
     * The binary is linked -rdynamic and its Qt is a shared library, so the frames
     * resolve to exported names on both sides of the boundary -- which is what says
     * whether the request came from the dashboard's own code or from inside Qt, and
     * from which thread.  QFileSystemModel does its work on one of its own.
     */
    Trace::backtraceTo(2);
    Trace::writeAll(2, "\n");
}

void *allocate(size_t bytes)
{
    /* operator new(0) must still return a distinct pointer. */
    void *p = ::malloc(bytes != 0 ? bytes : 1);

    if (p != NULL) {
        if (bytes >= kLoud && g_loudLeft > 0) {
            --g_loudLeft;
            report("a single allocation of", bytes, false);
        }
        return p;
    }

    report("OUT OF MEMORY asking for", bytes, true);
    return NULL;
}

/* ── every throw, on every thread, at the frame that threw ───────────────────── */

typedef void (*ThrowFn)(void *, void *, void (*)(void *));

ThrowFn realThrow()
{
    /* RTLD_NEXT skips this executable and finds libstdc++'s.  dlsym has lived in
     * libc since glibc 2.34, so this costs no library and no package. */
    static ThrowFn fn = (ThrowFn)::dlsym(RTLD_NEXT, "__cxa_throw");
    return fn;
}

/* Resolved before main, because the loader is not something to be asking for a symbol
 * from inside the first throw -- which on this board is likely to be an allocation
 * failure. */
struct PrimeThrow {
    PrimeThrow() { (void)realThrow(); }
};
PrimeThrow g_primeThrow;

/* Qt throws in normal operation about as often as it segfaults, so this is not a
 * volume problem -- but a program that got into a loop of them would scroll its own
 * first and most useful report off a thirty-line panel. */
int g_throwsLeft = 6;

} /* namespace */

extern "C" void __cxa_throw(void *thrown, void *tinfo, void (*dest)(void *))
{
    if (g_throwsLeft > 0) {
        --g_throwsLeft;
        Trace::writeAll(2, "\nmixdash: THROW ");
        Trace::writeAll(2, tinfo != NULL
                            ? ((const std::type_info *)tinfo)->name()
                            : "(no type)");
        Trace::writeAll(2, " in phase \"");
        Trace::writeAll(2, Trace::current());
        Trace::writeAll(2, "\", step \"");
        Trace::writeAll(2, Trace::currentStep());
        Trace::writeAll(2, "\"\n");
        Trace::backtraceTo(2);
        Trace::writeAll(2, "\n");
    }

    ThrowFn fn = realThrow();
    if (fn == NULL) {
        /* Nothing can be done with the exception now: passing it on is the only way
         * out of a function the ABI declares noreturn, and there is nobody to pass it
         * to.  Say why, rather than looping or returning into undefined behaviour. */
        Trace::writeAll(2, "mixdash: libstdc++'s __cxa_throw could not be found, so this "
                           "throw cannot be delivered\n");
        ::abort();
    }
    fn(thrown, tinfo, dest);
    ::abort();          /* not reached: __cxa_throw does not return */
}

void *operator new(std::size_t bytes)
{
    void *p = allocate(bytes);
    if (p == NULL)
        throw std::bad_alloc();
    return p;
}

void *operator new[](std::size_t bytes)
{
    void *p = allocate(bytes);
    if (p == NULL)
        throw std::bad_alloc();
    return p;
}

void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept
{
    return allocate(bytes);
}

void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept
{
    return allocate(bytes);
}

/*
 * The matching deletes.  Replacing new without replacing delete is legal here --
 * both ends are malloc/free either way -- but a translation unit that replaces half
 * of a pair invites the next reader to wonder, and free() is the whole body.
 */
void operator delete(void *p) noexcept { ::free(p); }
void operator delete[](void *p) noexcept { ::free(p); }
void operator delete(void *p, std::size_t) noexcept { ::free(p); }
void operator delete[](void *p, std::size_t) noexcept { ::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { ::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { ::free(p); }
