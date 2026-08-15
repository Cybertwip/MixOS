/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * switcher.h -- what is running, and how to get back to it.
 *
 * ── WHY THERE IS ANYTHING TO SWITCH BETWEEN ──────────────────────────────────
 *
 * This dashboard used to run exactly one child at a time and said so in
 * Dashboard::launch(): "THERE MUST BE ONLY ONE.  One panel, one set of input
 * devices."  That reasoning is still completely correct about the PANEL -- there
 * is no compositor on this board, /dev/fb0 is one buffer, and two programs
 * drawing into it are two programs drawing over each other.  What it got wrong
 * is that it treated "only one may DRAW" as "only one may EXIST", and those are
 * not the same sentence.  A browser that has finished loading a page and a game
 * that is sitting on its title screen are both perfectly happy not to be looked
 * at; what they cannot survive is being killed because somebody wanted to check
 * the Wi-Fi.
 *
 * So the rule is now the narrow one it always should have been: ONE TASK IS IN
 * FRONT.  The one in front owns the framebuffer and the input devices.  Every
 * other task is SIGSTOP'd -- not asked politely to idle, stopped by the kernel,
 * so it cannot draw a single pixel and costs no CPU while it waits.  Dashboard
 * ::setForeground() is where that happens and the long comment there is the
 * one worth reading; this file is only the picture the user chooses from.
 *
 * ── THE PICTURE, AND WHY IT IS AN OVERLAY ────────────────────────────────────
 *
 * It is not a PageWidget and must not become one.  Pages live on a stack that
 * belongs to the shell, and the whole point of this thing is that it appears
 * over a CHILD PROCESS -- a moment when the shell's stack is not on the glass at
 * all and the page underneath is whatever the child last drew.  Like the
 * keyboard, the toast and the volume bar, it is a plain widget the shell raises
 * over everything and sizes to the whole panel.
 *
 * It can be drawn at all for one reason: by the time it goes up, the task in
 * front has ALREADY been stopped.  Nothing else is writing to the framebuffer,
 * so ordinary Qt painting works and none of the setRedirected()/snapshot()
 * machinery that Busy and Pointer need applies here.  That ordering is not an
 * implementation detail, it is the contract -- see Dashboard::showSwitcher().
 *
 * ── FULL PANEL, AND DELIBERATELY OPAQUE ──────────────────────────────────────
 *
 * A translucent overlay would show the stopped child through it, which sounds
 * nice and is actively bad here: the frame underneath is a frozen game, the
 * pixels are arbitrary, and text over arbitrary pixels on a 640x480 panel is
 * text nobody can read.  It fills.
 */
#ifndef MIXDASH_SWITCHER_H
#define MIXDASH_SWITCHER_H

#include <QString>
#include <QVector>
#include <QWidget>

class Switcher : public QWidget
{
    Q_OBJECT

public:
    /*
     * One row.  `detail' is the small right-hand text -- what the task is doing
     * and what it costs -- and is built by the shell, because the shell is what
     * knows a pid from a page.
     *
     * `closable' is false for exactly one row, the dashboard's, and it is a flag
     * rather than an index test so that this file never has to know that row 0
     * is special.
     */
    struct Entry {
        QString title;
        QString detail;
        bool closable = true;
    };

    explicit Switcher(QWidget *parent = nullptr);

    /*
     * Put it up.  `current' is the row to start the highlight on -- the task that
     * is in front, so that pressing A straight away is a no-op rather than a
     * surprise, and so that one tap of FN moves to the NEXT thing, which is the
     * gesture everybody already has in their fingers.
     */
    void open(const QVector<Entry> &entries, int current);
    /* Not close(): QWidget has one of those and it means something else. */
    void dismiss();

    /*
     * New rows for the same open switcher -- a task exited while it was up, or
     * one of them was closed from here.  The highlight is kept on the same row
     * where that still exists and clamped into range where it does not; it is
     * NOT reset to the top, because the list changing under a user who was
     * halfway down it is not a reason to move them.
     */
    void refresh(const QVector<Entry> &entries);

    int selected() const { return m_sel; }
    int count() const { return m_entries.size(); }

    /* True if the action was used.  The shell routes to this before pages, and
     * before its own fallbacks, for as long as this is visible. */
    bool handleNav(int action);

signals:
    /* Bring this row to the front.  The row index, which the shell maps back to
     * a task -- this widget does not know what a task is. */
    void chosen(int index);
    /* Close the process behind this row.  Never emitted for a row whose
     * `closable' is false. */
    void closeRequested(int index);
    /* B, or FN held a second time.  Nothing changes; whatever was in front goes
     * back in front, which the shell does by choosing it. */
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    /* The rectangle row `i' occupies, in this widget's coordinates.  One
     * function so the painter and any future hit test cannot disagree. */
    QRect rowRect(int i) const;
    /* Where the column of rows starts, given how many there are: the block is
     * centred vertically, which on a panel with four rows on it looks composed
     * and with one row on it does not look broken. */
    int listTop() const;

    QVector<Entry> m_entries;
    int m_sel = 0;
};

/*
 * ── ASKING FOR THE SWITCHER FROM OUTSIDE THE PROCESS ─────────────────────────
 *
 * Almost every request comes from Joypad, which sees FN held and emits
 * switcherRequested().  There is one case where it cannot, and it is not a
 * corner: j36-padx calls EVIOCGRAB during a browser session, and a grab means
 * the kernel delivers the pad to that descriptor and to nobody else.  For as
 * long as the browser is up, this program reads nothing at all from the pad --
 * deliberately, because otherwise every press would drive the card grid behind
 * the browser as well as the browser.
 *
 * So padx forwards the gesture instead.  mixdash puts its own pid in every
 * child's environment as MIXDASH_PID; padx sees Menu held, gives the pad back,
 * and sends SIGUSR1 here.  The handler in main.cpp can do exactly one thing
 * safely, which is set a flag, and Dashboard polls that flag while a child is in
 * front.
 *
 * NOT A QSocketNotifier ON A SELF-PIPE, which is the usual answer and is the
 * wrong one in this program: QSocketNotifier::activated() is overloaded and
 * deprecated in the exact Qt this image ships, and the rest of this codebase has
 * already decided that a polled flag is worth more than a connect that produces
 * a deprecation warning on every build.
 */
namespace SwitcherRequest {

/*
 * Safe from a signal handler, and that is the whole specification: one store to
 * a volatile sig_atomic_t, no allocation, no Qt, no libc beyond the assignment.
 */
void post();

/*
 * True once for each post(), and clears it.  Two signals that arrive between two
 * polls are one request -- a switcher that opened twice would be a switcher that
 * closed itself.
 */
bool take();

} /* namespace SwitcherRequest */

#endif /* MIXDASH_SWITCHER_H */
