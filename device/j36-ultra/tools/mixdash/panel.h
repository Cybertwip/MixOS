/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * panel.h -- the pixels that are on the glass, borrowed and given back.
 *
 * ── WHY A STOPPED PROGRAM NEEDS ITS FRAME KEPT FOR IT ────────────────────────
 *
 * The task switcher stops the program that was in front (SIGSTOP, see
 * Dashboard::setForeground) and then draws over it.  Bringing it back is
 * SIGCONT, and for a game that is enough: a game renders another frame within
 * sixteen milliseconds of being allowed to run, and that frame covers the
 * switcher.
 *
 * A BROWSER DOES NOT.  An X server repaints damage, and being continued is not
 * damage -- nothing changed as far as it knows, so it draws nothing, and the
 * panel keeps showing the switcher over a session that is running perfectly well
 * underneath it.  Every event-driven program on this device has that shape.  The
 * fix cannot be to ask the program to redraw, because there is no way to ask
 * that is not specific to what the program is.
 *
 * So the shell keeps the frame instead.  Stop the task, copy the framebuffer,
 * and put that copy back the moment before it is continued: the panel shows
 * exactly what it showed when the user left, and whatever the program draws next
 * -- in a millisecond or in an hour -- lands on top of a correct picture rather
 * than on top of the switcher.
 *
 * ── WHAT IT COSTS ────────────────────────────────────────────────────────────
 *
 * 640x480 at 32 bits is 1.2 MB per stopped task, and Dashboard caps the tasks at
 * four, so the ceiling is under 5 MB of the board's 946.  It is bought only when
 * a task is actually stopped and given back the moment that task is resumed or
 * exits.
 *
 * ── AND WHY IT DOES NOT FIGHT QT ─────────────────────────────────────────────
 *
 * This writes into /dev/fb0 behind the linuxfb plugin's back, which is safe for
 * exactly one reason and it is worth naming: restore() is only ever called with
 * this window's updates DISABLED.  Qt flushes dirty regions and nothing else, so
 * with nothing dirty it writes nothing, and the frame put back here stays.  Call
 * it with updates on and the next repaint would race it.
 *
 * A device where none of this works -- no /dev/fb0, a geometry that is not what
 * it was when the frame was taken, a card running under X on a development
 * machine -- degrades to the honest old behaviour: grab() answers nothing,
 * restore() does nothing, and the resumed program repaints when it repaints.
 */
#ifndef MIXDASH_PANEL_H
#define MIXDASH_PANEL_H

#include <QByteArray>

namespace Panel {

/*
 * Everything currently visible, as bytes.
 *
 * CALL IT AFTER THE TASK IS STOPPED, not before.  A program that is still
 * running can be halfway through a frame while this copy is being made, and the
 * result would be a tear.  Once the kernel has stopped it, the framebuffer is
 * frozen and this reads whatever the user was actually looking at -- including a
 * torn frame, if that is what was on the glass, which is the correct answer.
 *
 * Empty if there is no framebuffer to read, which is not an error here.
 */
QByteArray grab();

/*
 * Put one back.  False if the frame is empty or no longer matches the panel it
 * was taken from, in which case nothing has been written.
 *
 * See the header comment: the caller must have updates disabled.
 */
bool restore(const QByteArray &frame);

} /* namespace Panel */

#endif /* MIXDASH_PANEL_H */
