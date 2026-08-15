/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * console.h -- who owns the panel, and where this program's own words go.
 *
 * WHY IT IS ITS OWN TRANSLATION UNIT, which is the same story trace.h tells.  All
 * of this was in main.cpp, in an anonymous namespace, and that was right while the
 * only things that needed it were main() and the signal handlers.  Then a page
 * turned out to need it too, and for a reason no timer in main() can cover: a page
 * that shells out waits for the child with the event loop STOPPED, so for as long
 * as that wait lasts nothing in this program can notice anything or repaint
 * anything.  hold() is what a blocking wait calls between slices.
 *
 * THE MODE IS NOT A LOCK, AND THAT IS THE WHOLE PROBLEM.  KD_GRAPHICS is a flag on
 * the virtual terminal that says userspace is driving the display; fbcon draws
 * nothing while it is set, and any process that opens the same terminal can clear
 * it with no notification to whoever set it.  On this board several do, and every
 * one of them is right to:
 *
 *   - agetty resets the terminal it is given before printing the issue, and
 *     `reset_vc' is KDSETMODE KD_TEXT.
 *   - systemd-vconsole-setup loads a font and a keymap onto every VT.
 *   - systemd resets the console it was told to log a unit to -- and mixdash.service
 *     is StandardOutput=journal+console, so starting ANY unit from a page can be
 *     the thing that does it.  That is what "changing the sharing settings puts the
 *     console back on the panel" was.
 *
 * mixsplash learnt this first and answered it by re-taking the mode once a second
 * (console_hold in mixsplash.c, with the same list of suspects).  That was enough
 * there because the splash redraws every frame: whatever fbcon had painted was
 * covered within 40 ms of the mode coming back.  This program is not an animation.
 * Qt flushes only what a widget marked dirty, so console text drawn over the
 * dashboard would sit there until something happened to be repainted -- which is
 * why hold() reports whether it had to act, and every caller repaints the whole
 * window when it says yes.
 *
 * AND THE STREAM GOES TO A FILE ONCE THERE IS A DASHBOARD TO PROTECT.  Until the
 * first frame this program's stdout and stderr are the most useful thing the panel
 * can show, and the unit sends them to the console on purpose.  After the first
 * frame they are the opposite: one Qt warning drawn across the grid is exactly the
 * failure this file exists to prevent, and it arrives through a channel no amount
 * of re-taking the mode can close, because the text is OURS.  So toLog() moves
 * them, children included -- an inherited fd 1 is why a page's shell-out can print
 * on the panel even when the page captured its output -- and text() moves them
 * back before a single word of any failure report is written.
 */
#ifndef MIXDASH_CONSOLE_H
#define MIXDASH_CONSOLE_H

namespace Console {

/*
 * /dev/tty0, O_RDWR because KDSETMODE needs it, and kept open for the life of the
 * process because the signal handlers use it and open() is not something to do
 * from one.  False when there is no VT to be had -- run over ssh, or a kernel with
 * CONFIG_VT off -- which is not an error and not unusual; errno is left set for the
 * caller to report.
 */
bool open();

/* The handover, from the first paint and not one moment sooner.  Everything before
 * that is still readable on the glass. */
void take();

/*
 * Re-take the mode if something cleared it.  True ONLY when it had to act, so that
 * the caller can repaint on the rare occasion instead of every tick.
 *
 * KDGETMODE first: the common case is that nobody touched it, and that case costs
 * a read and no write.  Does nothing at all before take(), because before the first
 * paint the console is deliberately the console.
 */
bool hold();

/*
 * KD_TEXT, and the stream back on the console with it.  Every failure path calls
 * this BEFORE it writes a word, because a console in KD_GRAPHICS is a console fbcon
 * is not drawing and a report written to a log file on a tmpfs is a report nobody
 * will ever read off this board.
 *
 * Async-signal-safe: ioctl, dup2 and close are, there is nothing allocated here and
 * no stdio call in it.  Calling it twice is a no-op the second time.
 */
void text();

/*
 * stdout and stderr into `path', from the first paint.  Silently does nothing if
 * the file will not open -- a dashboard run from a shell on a machine with no
 * /run/j36 keeps its terminal, which is what somebody running it by hand wants.
 *
 * The file is truncated when it passes a few hundred kilobytes, from inside hold(),
 * because /run is a tmpfs and a warning printed once per poll for a fortnight is a
 * slow way to fill it.
 */
void toLog(const char *path);

} /* namespace Console */

#endif /* MIXDASH_CONSOLE_H */
