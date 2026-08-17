/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * switcher.cpp -- see switcher.h for what this is and why it is an overlay.
 */
#include "switcher.h"

#include <signal.h>

#include <QMouseEvent>
#include <QPainter>

#include "joypad.h"
#include "theme.h"

namespace {

/* Set by a signal handler, read by the event loop.  volatile sig_atomic_t is
 * the only type the standard promises is safe for that. */
volatile sig_atomic_t g_requested = 0;
/* The same again for "there is a command line waiting" -- see RunRequest in
 * switcher.h.  Two flags and not one enum: they are set by two different signals
 * and either may arrive while the other is pending. */
volatile sig_atomic_t g_runRequested = 0;

/* Rows stay card-sized at the usual task count and compress just enough when one
 * persistent X service contributes several real windows.  Eight useful cards on
 * a 480-line panel still fit without turning the switcher into a scrolling page. */
const int RowH = 46;
const int RowGap = 6;
const int PanelW = 420;
const int HeadH = 34;
const int FootH = 26;
const int Edge = 16;

}

void SwitcherRequest::post()
{
    g_requested = 1;
}

bool SwitcherRequest::take()
{
    if (!g_requested)
        return false;
    g_requested = 0;
    return true;
}

void RunRequest::post()
{
    g_runRequested = 1;
}

bool RunRequest::take()
{
    if (!g_runRequested)
        return false;
    g_runRequested = 0;
    return true;
}

Switcher::Switcher(QWidget *parent)
    : QWidget(parent)
{
    /* The D-pad remains the digital navigation source, while the left stick's
     * shared dashboard pointer may hover and click the same cards. */
    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    /* And it says so, which is not the same statement: NoSystemBackground stops
     * Qt filling this widget, OpaquePaintEvent tells Qt it need not paint the
     * page UNDERNEATH it either.  paintEvent covers every pixel of rect(), so
     * that promise is kept -- and on a 640x480 panel it is the difference
     * between one full repaint per switcher and two. */
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void Switcher::open(const QVector<Entry> &entries, int current)
{
    m_entries = entries;
    m_sel = qBound(0, current, qMax(0, entries.size() - 1));
    show();
    raise();
    update();
}

void Switcher::dismiss()
{
    hide();
    /* The rows are dropped rather than kept for next time: they hold a snapshot
     * of what was running, and a snapshot that is displayed again later is a
     * list of things that may since have exited. */
    m_entries.clear();
}

void Switcher::refresh(const QVector<Entry> &entries)
{
    if (!isVisible())
        return;
    m_entries = entries;
    m_sel = qBound(0, m_sel, qMax(0, entries.size() - 1));
    update();
}

int Switcher::listTop() const
{
    const int n = qMax(1, m_entries.size());
    const int block = n * rowHeight() + (n - 1) * rowGap();
    const int available = qMax(0, height() - HeadH - FootH - 2 * Edge);
    return HeadH + Edge + qMax(0, (available - block) / 2);
}

int Switcher::rowHeight() const
{
    const int n = qMax(1, m_entries.size());
    const int available = qMax(0, height() - HeadH - FootH - 2 * Edge);
    return qMax(30, qMin(RowH, (available - (n - 1) * RowGap) / n));
}

int Switcher::rowGap() const
{
    return RowGap;
}

QRect Switcher::rowRect(int i) const
{
    const int x = (width() - PanelW) / 2;
    const int h = rowHeight();
    return QRect(x, listTop() + i * (h + rowGap()), PanelW, h);
}

int Switcher::rowAt(const QPoint &point) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (rowRect(i).contains(point))
            return i;
    return -1;
}

/*
 * THE BINDINGS, AND WHY THEY ARE THESE.
 *
 * Up/Down walks the list because the list is a column.  Left/Right do the same
 * thing rather than nothing: this switcher is opened by holding a button on a
 * device whose D-pad is under a thumb that has just moved, and a direction that
 * silently does nothing reads as the switcher having hung.
 *
 * A switches.  B cancels, which means "put back whatever was in front", and the
 * shell does that by choosing the row it opened on -- so cancelling is not a
 * separate path through setForeground() that could drift from the normal one.
 *
 * FN ITSELF CYCLES.  Hold FN to open, then tap FN to step down the list: that is
 * alt-tab, it is what the fingers already do, and it costs one case here.  It is
 * also why the highlight starts on the CURRENT task -- one tap then lands on the
 * next one, which is the thing a person holding this device wants nine times out
 * of ten.
 *
 * START closes the highlighted task.  It is the one destructive binding on this
 * overlay, so it is on the button that is furthest from the two being used to
 * navigate, and it refuses on the dashboard's own row rather than being absent
 * from it -- a binding that works everywhere except one place is a binding
 * people trust.
 *
 * IT IS CALLED START HERE BECAUSE THAT IS WHAT IS PRINTED ON THE CASE.  The
 * action behind it is NavMenu and the footer used to say "Menu", which is the
 * name of the action and the name of no button on this device -- and the button
 * a hand reaches for when it reads "menu" is FN, which steps the list.  So the
 * one way to close the desktop was named after the one button that would not do
 * it: "there does not seem a way to exit desktop", exactly as reported.  The
 * silkscreen wins over the enum; see the same rule in terminal.cpp's footer.
 */
bool Switcher::handleNav(int action)
{
    if (!isVisible() || m_entries.isEmpty())
        return false;

    switch (action) {
    case Joypad::NavUp:
    case Joypad::NavLeft:
        m_sel = (m_sel + m_entries.size() - 1) % m_entries.size();
        update();
        return true;
    case Joypad::NavDown:
    case Joypad::NavRight:
    case Joypad::NavQuit:
        m_sel = (m_sel + 1) % m_entries.size();
        update();
        return true;
    case Joypad::NavOk:
        emit chosen(m_sel);
        return true;
    case Joypad::NavBack:
        emit dismissed();
        return true;
    case Joypad::NavMenu:
        if (m_sel >= 0 && m_sel < m_entries.size() && m_entries[m_sel].closable)
            emit closeRequested(m_sel);
        return true;
    default:
        break;
    }

    /*
     * Everything else is swallowed.
     *
     * NOT `return false', which would let the action fall through to the page
     * underneath -- and the page underneath is either a stopped child process or
     * a dashboard page that is not on the glass.  Either way, acting on it would
     * be acting on something the user cannot see.  The volume keys are the
     * exception and they never reach here: the shell answers those before any of
     * this, which is what makes them work on every screen in the program.
     */
    return true;
}

void Switcher::mouseMoveEvent(QMouseEvent *event)
{
    const int row = rowAt(event->pos());
    if (row >= 0 && row != m_sel) {
        m_sel = row;
        update();
    }
    event->accept();
}

void Switcher::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->accept();
        return;
    }
    const int row = rowAt(event->pos());
    if (row >= 0 && row != m_sel) {
        m_sel = row;
        update();
    }
    event->accept();
}

void Switcher::mouseReleaseEvent(QMouseEvent *event)
{
    const int row = rowAt(event->pos());
    if (event->button() == Qt::LeftButton && row >= 0) {
        m_sel = row;
        emit chosen(row);
    }
    event->accept();
}

void Switcher::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* Opaque, for the reason in the header: the frozen frame underneath is
     * arbitrary pixels and text over it would be unreadable. */
    Theme::vgrad(p, rect(), Theme::desk(), Theme::deskLow());

    const int x = (width() - PanelW) / 2;
    const int top = listTop();

    p.setFont(Theme::font(13, true));
    p.setPen(Theme::ink3());
    /* Upper-cased here and not in the table, the way ListPane does its section
     * headers -- the phrase a translator is given should be a word, not a
     * shouted one. */
    p.drawText(QRect(x, top - HeadH, PanelW, HeadH - 8),
               Qt::AlignLeft | Qt::AlignBottom, tr("Running").toUpper());

    const QFont titleFont = Theme::font(15, true);
    const QFont detailFont = Theme::font(11);

    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries[i];
        const QRectF row = rowRect(i);
        const bool sel = (i == m_sel);

        if (sel) {
            Theme::softShadow(p, row, Theme::Radius, 5, 30);
            Theme::vgrad(p, row, Theme::blue(), Theme::blueLow(), Theme::Radius);
        } else {
            Theme::vgrad(p, row, Theme::card(), Theme::cardLow(), Theme::Radius);
            p.setPen(QPen(Theme::border(), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(row.adjusted(0.5, 0.5, -0.5, -0.5),
                              Theme::Radius, Theme::Radius);
        }

        p.setFont(titleFont);
        p.setPen(Theme::ink());
        p.drawText(row.adjusted(16, 0, -16, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, e.title);

        if (!e.detail.isEmpty()) {
            p.setFont(detailFont);
            p.setPen(sel ? Theme::ink() : Theme::ink3());
            p.drawText(row.adjusted(16, 0, -16, 0),
                       Qt::AlignRight | Qt::AlignVCenter, e.detail);
        }
    }

    /*
     * The footer names the bindings, and it names the destructive one LAST so it
     * is not the first thing a thumb reaches for.  It is spelled out rather than
     * left to be discovered because this overlay is reached by holding a button
     * that, until this build, did nothing at all outside the Terminal.
     */
    p.setFont(Theme::font(11));
    p.setPen(Theme::ink3());
    const int count = m_entries.size();
    const int footY = top + count * rowHeight()
                    + qMax(0, count - 1) * rowGap() + 8;
    p.drawText(QRect(x, footY, PanelW, FootH), Qt::AlignCenter,
               tr("A switches  --  FN steps  --  Start closes  --  B cancels"));
}
