/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * busy.h -- the thing that turns while something else is taking its time.
 *
 * WHY THIS EXISTS AT ALL.  Everything slow in this dashboard used to be slow with
 * the event loop stopped: Shell::waitForFinished() sliced the wait into 200 ms
 * chunks and repainted the visible windows between them, which kept the panel from
 * going black but could not make anything MOVE -- a repaint of a still picture is
 * still a still picture.  So a four-second ffprobe looked exactly like a crash,
 * and the only honest thing on the glass was the last frame drawn before the fork.
 *
 * The waits are asynchronous now, which is what makes an animation possible; this
 * is the animation.  It has no idea what it is waiting for and deliberately so --
 * one spinner, one caption, started and stopped by whoever knows.
 *
 * ── IT MUST BE ABLE TO NOT BE A WIDGET ──────────────────────────────────────
 *
 * The one place a spinner is most needed is over a film that is being opened, and
 * that is exactly the place a Qt widget cannot go: while GlVideo owns the scanout
 * the pixels on the glass did not come from Qt's backing store, so a child widget
 * drawn over the picture is a memcpy of stale pixels that the next frame paints
 * over -- and, worse, it DIRTIES the rectangle underneath it, which is the bug
 * that used to drag a grey square around behind the mouse cursor.
 *
 * So this follows the same contract as VolumeOverlay and Pointer: setRedirected()
 * hides the widget, snapshot() hands the picture over as pixels, and changed()
 * says when the picture is not what it was -- which for a spinner is every frame
 * of the animation, and is why the timer stops the moment nothing is spinning.
 */
#ifndef MIXDASH_BUSY_H
#define MIXDASH_BUSY_H

#include <QImage>
#include <QString>
#include <QWidget>

class QPainter;
class QTimer;

class Busy : public QWidget
{
    Q_OBJECT

public:
    explicit Busy(QWidget *parent = nullptr);

    /*
     * Start turning, with a line of text under the ring saying what for.  Calling
     * it again while it is up only replaces the caption -- the phase is not reset,
     * because a spinner that jumps back to twelve o'clock every time the message
     * changes reads as a stall rather than as progress.
     */
    void start(const QString &caption);
    void stop();
    bool spinning() const { return m_spinning; }

    /* Centred in `panel'.  The shell calls this from its resizeEvent for the same
     * reason it does for the volume bar: only the shell knows what the panel is. */
    void placeIn(const QRect &panel);

    /*
     * Which spinner: false is the plain arc, true is the ring of chasing dots
     * anybody who has waited for a desktop will recognise.
     *
     * IT FOLLOWS THE MOUSE, and that is not decoration.  A handheld with no
     * pointer is read at arm's length by somebody holding it, and a single sweeping
     * arc is legible there at a glance.  A board with a mouse plugged into it is
     * being driven by somebody sitting at a desk who already knows what a busy
     * ring means, and next to an arrow on the glass the dots are the idiom that
     * matches.  The shell flips it when the pointer wakes; nothing else does.
     */
    void setPointerStyle(bool on);

    /* Hand the pixels over instead of painting them -- see the class comment. */
    void setRedirected(bool on);
    bool isRedirected() const { return m_redirected; }

    /* What this would paint, premultiplied ARGB at its own size.  Null when
     * nothing is spinning, which the caller should read as "clear the layer". */
    QImage snapshot() const;

signals:
    /* The ring turned, the caption changed, or it stopped.  Emitted in both modes
     * on purpose: a signal whose meaning depends on a mode is one somebody
     * eventually connects in the other one. */
    void changed();

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onStep();

private:
    void paintBody(QPainter &p) const;

    /* 0..StepCount-1.  One revolution a second, which is slow enough to look
     * deliberate and fast enough to look alive on a panel that is also decoding. */
    int m_phase = 0;
    bool m_spinning = false;
    bool m_dots = false;
    bool m_redirected = false;
    QString m_caption;
    QTimer *m_timer = nullptr;
};

#endif /* MIXDASH_BUSY_H */
