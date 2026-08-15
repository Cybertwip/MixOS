/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * shell.h -- waiting for a child without letting the panel rot.
 *
 * EVERY PAGE HERE SHELLS OUT AND EVERY ONE OF THEM WAITS.  systemctl on the
 * Sharing page, apt on Packages, wpa_cli on Wi-Fi, amixer on the volume overlay,
 * ffprobe in the media browser: all of them are QProcess plus waitForStarted plus
 * waitForFinished, all bounded, and that shape is deliberate -- a state query that
 * answers in milliseconds is not worth the signal plumbing an asynchronous read
 * would cost, and a bounded wait cannot wedge the dashboard for ever the way an
 * unbounded one can.
 *
 * WHAT IT DOES COST is the event loop, for the whole of the wait.  waitForFinished
 * is a poll on the child's pipe and nothing else: no timer fires inside it, no key
 * is read, nothing repaints.  That is a fifteenth of a second on `systemctl
 * is-active' and it is FIFTEEN SECONDS on `systemctl start smbd', and fifteen
 * seconds is long enough for the thing this file exists to stop.
 *
 * THE THING IT STOPS.  Starting a unit makes systemd reset the console it logs to,
 * which puts the VT back into KD_TEXT, which lets fbcon draw the kernel's next few
 * lines straight over the dashboard -- console.h has the full list of suspects and
 * why the mode is not a lock.  main() answers it with a one-second guard timer, and
 * that guard is exactly what a blocking wait switches off, in the one window where
 * the trigger is most likely: the page told systemd to start something and is now
 * sitting in waitForFinished while systemd does it.
 *
 * So the wait is sliced, and each slice does what the guard timer would have done:
 * Console::hold(), and a synchronous repaint of the window when it reports that it
 * had to act.  repaint() and not update(), because update() only posts an event and
 * the event loop that would deliver it is the thing that is stopped.
 *
 * IT DOES NOT PUMP THE EVENT LOOP, and that is the point of using repaint() rather
 * than QApplication::processEvents().  Delivering events here would let a D-pad
 * press start a second shell-out inside the first one, or let a page be deleted
 * underneath the `this' that is running the wait.  A repaint touches paintEvent and
 * nothing else, so the page is still exactly where it was when the child returns.
 *
 * The two functions are drop-in: same arguments, same return value, same meaning as
 * QProcess's own -- so converting a call site is deleting `p.' and typing `Shell::'.
 */
#ifndef MIXDASH_SHELL_H
#define MIXDASH_SHELL_H

class QProcess;

namespace Shell {

/*
 * QProcess::waitForStarted, sliced.  True when the child is running.  False when it
 * will never run -- a missing binary is reported as soon as the fork fails, and this
 * returns then rather than burning the rest of the budget on a wait for something
 * that has already failed -- or when `timeoutMs' is spent.  A negative timeout waits
 * for ever, as QProcess's does.
 */
bool waitForStarted(QProcess &proc, int timeoutMs);

/*
 * QProcess::waitForFinished, sliced.  True when the child has exited, whether it
 * exited during this call or had already gone before it -- Qt returns false for the
 * second of those and callers have always read false as "it timed out", so telling
 * the two apart here is not a nicety.  False on a real timeout, and false for a
 * child that never started, which is the one case where "not running" does not mean
 * "finished".
 */
bool waitForFinished(QProcess &proc, int timeoutMs);

} /* namespace Shell */

#endif /* MIXDASH_SHELL_H */
