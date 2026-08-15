/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * shell.cpp -- the sliced waits.  shell.h says why they are sliced.
 */
#include "shell.h"

#include <QApplication>
#include <QProcess>
#include <QWidget>
#include <QWidgetList>

#include "console.h"

namespace {

/*
 * 200 ms, and the number is a compromise between two things that both matter and
 * pull opposite ways.  Shorter means the console is stolen back sooner, and it is
 * the visible half: this is how long kernel text can sit on the panel.  Longer means
 * fewer wakeups on eight Cortex-A7s that are also running whatever the child is.  A
 * fifth of a second is under the threshold where a flash reads as a fault rather
 * than a flicker, and it is five ioctls a second on a wait that is doing nothing
 * else -- which is nothing at all next to the child.
 */
const int kSlice = 200;

/*
 * NOT update(), and the whole file turns on that word.  update() posts a paint event
 * to a queue that the event loop drains, and the event loop is precisely what the
 * caller has stopped by waiting for its child -- so the repaint would land when the
 * wait ended, which is the moment it stopped being needed.  repaint() paints now,
 * on this stack, before this function returns.
 *
 * Top-level widgets rather than a widget the caller passes in, for two reasons: the
 * callers include static member functions with no `this' to offer, and the thing to
 * put back is the whole window anyway.  fbcon does not draw inside the page's rect,
 * it draws wherever the text cursor was, so repainting anything smaller than
 * everything would leave whatever it wrote outside that rect exactly where it is.
 *
 * Hidden windows are skipped because repaint() on one is defined to do nothing, and
 * saying so here is cheaper than finding out later that it does.
 */
void refresh()
{
    /*
     * ONCE AT A TIME, and the guard is here rather than argued away because the
     * argument would have to be re-made every time a page grew a new paintEvent.
     * Nothing in this program shells out from inside a paint today; the day one
     * does, this turns a stack that recurses until it runs out into a repaint that
     * simply does not happen twice.
     */
    static bool busy = false;

    if (busy)
        return;
    busy = true;

    const QWidgetList tops = QApplication::topLevelWidgets();
    for (int i = 0; i < tops.size(); ++i) {
        QWidget *w = tops.at(i);
        if (w && w->isVisible())
            w->repaint();
    }

    busy = false;
}

/* One slice's worth of the budget, or a whole slice when there is no budget. */
int nextSlice(int timeoutMs, int left)
{
    if (timeoutMs < 0)
        return kSlice;
    return (left < kSlice) ? left : kSlice;
}

} /* namespace */

namespace Shell {

bool waitForStarted(QProcess &proc, int timeoutMs)
{
    int left = timeoutMs;

    for (;;) {
        const int slice = nextSlice(timeoutMs, left);

        if (proc.waitForStarted(slice))
            return true;

        /*
         * A missing binary is not a slow one.  QProcess reports FailedToStart the
         * instant the fork or the exec fails, and there is nothing further to wait
         * for -- carrying on would spend the caller's whole timeout discovering
         * again, once per slice, that /usr/bin/whatever is still not there.
         */
        if (proc.error() == QProcess::FailedToStart)
            return false;

        if (Console::hold())
            refresh();

        if (timeoutMs >= 0) {
            left -= slice;
            if (left <= 0)
                return false;
        }
    }
}

bool waitForFinished(QProcess &proc, int timeoutMs)
{
    int left = timeoutMs;

    for (;;) {
        const int slice = nextSlice(timeoutMs, left);

        if (proc.waitForFinished(slice))
            return true;

        /*
         * FALSE HERE IS TWO DIFFERENT ANSWERS and the state is what separates them.
         * Qt returns false both for "the slice ran out" and for "this process had
         * already finished before you asked", and the second of those would loop
         * until the timeout and then be reported as a hang -- a child that exited
         * inside the previous slice would be killed and its output thrown away.
         *
         * NotRunning covers one more case, though: a child that never started at
         * all.  Every caller in this program checks waitForStarted first, so it
         * should not arrive here -- but "should not" is not a thing to return true
         * on, because true is what makes the caller read exitCode(), and exitCode()
         * on a process that never ran is 0, which is to say success.
         */
        if (proc.state() == QProcess::NotRunning)
            return proc.error() != QProcess::FailedToStart;

        if (Console::hold())
            refresh();

        if (timeoutMs >= 0) {
            left -= slice;
            if (left <= 0)
                return false;
        }
    }
}

} /* namespace Shell */
