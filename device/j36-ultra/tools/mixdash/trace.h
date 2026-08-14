/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * trace.h -- the startup trace, and the only diagnostic channel this board has.
 *
 * WHY IT IS ITS OWN TRANSLATION UNIT.  It began inside main.cpp, in an anonymous
 * namespace, which was fine while the only thing that could fail was a phase main()
 * had entered.  Then the dashboard died inside `Dashboard dash;' -- one statement,
 * one phase, eleven objects constructed behind it -- and the console said the phase
 * and nothing more.  A phase that names eleven suspects names none of them, so the
 * constructor announces its own steps now, and to do that it needs what main() had
 * kept to itself: the clock, the phase strings and the watchdog.
 *
 * ASYNC-SIGNAL SAFETY.  current() and currentStep() are read from the SIGSEGV,
 * SIGABRT and SIGALRM handlers, and writeAll() is what those handlers print with.
 * Both globals only ever hold string LITERALS -- never a QString's data, never
 * anything owned -- so a handler cannot see a half-written pointer, and there is
 * nothing for it to free.  printf is deliberately not used from a handler: it takes
 * a lock the interrupted code may already hold.
 */
#ifndef MIXDASH_TRACE_H
#define MIXDASH_TRACE_H

namespace Trace {

/*
 * Starts the clock, and pays two costs up front that must not be paid later:
 * glibc resolves backtrace()'s unwinder with dlopen("libgcc_s.so.1") on first use,
 * and the first use here is from inside a failed allocation.
 */
void begin();

/* 0 disables the watchdog.  Every phase() and step() re-arms it, so what it
 * measures is "this step has taken too long" rather than "startup has". */
void setDeadline(int seconds);

/* Announce, then enter.  The last line on the glass names what did not finish. */
void phase(const char *name);

/* A step within a phase: same contract, one indent in.  Use it wherever a single
 * statement builds more than one thing that can fail. */
void step(const char *name);

/* Literals, safe to read from a signal handler. */
const char *current();
const char *currentStep();

double elapsed();

/* write(2) in a loop.  Nothing here allocates, which is why the out-of-memory
 * report and the signal handlers both print with it. */
void writeAll(int fd, const char *s);

/* Decimal, into the same channel, without printf and without allocating. */
void writeUnsigned(int fd, unsigned long value);

/*
 * The stack, symbolized as far as the binary allows.  backtrace_symbols_fd is the
 * malloc-free half of the backtrace pair -- backtrace_symbols is not -- which is why
 * this is what the out-of-memory report, the throw report and the signal handlers all
 * call.  begin() has already made backtrace() dlopen its unwinder.
 */
void backtraceTo(int fd);

} /* namespace Trace */

#endif /* MIXDASH_TRACE_H */
