/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * pointer.h -- a mouse cursor on a framebuffer that has never heard of one.
 *
 * WHY THIS IS NOT QCursor.  Qt's linuxfb plugin can draw a software cursor, but
 * only for a mouse IT opened, and this dashboard sets QT_QPA_FB_DISABLE_INPUT=1
 * precisely so that it can read the pad itself (see joypad.h).  There is no
 * window system underneath to route a pointer either.  So the cursor is a small
 * transparent child widget that moves, and the clicks are QMouseEvents this class
 * builds and posts.
 *
 * THE PAYOFF FOR SYNTHESIZING REAL EVENTS.  Everything Qt already knows how to
 * click keeps working for free: QListView selects a row, a QScrollBar drags, a
 * QPushButton depresses.  A hand-rolled "which card is under the arrow" hit test
 * would have had to be repeated in every page, and would have stopped at the
 * first widget somebody added that this file had not heard of.
 *
 * THE HOT SPOT is the widget's top-left corner, which is why the arrow is drawn
 * with its tip there.  Everything below reports position as a top-level-relative
 * point, and on this device the top level fills the framebuffer, so top-level
 * coordinates and screen coordinates are the same numbers.  That is an assumption
 * worth naming: if the dashboard ever runs in a window on a workstation, mapping
 * through mapToGlobal is the change.
 *
 * ── AND OVER A FILM IT IS NOT A WIDGET AT ALL ────────────────────────────────
 *
 * A cursor is the one widget in this program that moves over whatever happens to
 * be underneath it, which makes it the one widget that finds every compositing
 * assumption the underneath is making.  While GlVideo owns the screen, MediaPage
 * paints nothing -- the pixels on the glass came from the GPU, not from Qt's
 * backing store -- so a child widget moving across it dirties a rectangle that Qt
 * then memcpy's from a backing store holding nothing but stale pixels.  What the
 * user saw was a grey square dragging the arrow around the film.
 *
 * raise() cannot fix that, because it is not a stacking-order problem: both things
 * really are being drawn, into two different buffers, one of which is the scanout.
 * So setRedirected(true) does what volume.h's does -- the widget hides, stops
 * being composited by Qt at all, and emits changed() whenever the picture of it
 * would differ.  Whoever owns the screen calls snapshot(), pairs it with
 * hotspot(), and blends it in its own pass.  Everything else about the cursor --
 * where it is, what it is over, what it has grabbed, the idle fade -- carries on
 * unchanged, because none of that was ever about being visible to Qt.
 */
#ifndef MIXDASH_POINTER_H
#define MIXDASH_POINTER_H

#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QWidget>

class QPainter;
class QTimer;

class Pointer : public QWidget
{
    Q_OBJECT

public:
    /* host must be the top-level widget; the cursor becomes its child. */
    explicit Pointer(QWidget *host);

    /* Where the tip is, in host coordinates. */
    QPoint hotspot() const { return pos(); }
    bool awake() const { return m_awake; }

    /* Bring it back without moving it -- used when a page opens that is only
     * usable with a pointer, so the user is not left hunting for it. */
    void wake();
    /* Put it away now.  A page that takes over the whole screen (the cube, a
     * launched child) calls this so the arrow is not burned into a screenshot. */
    void sleep();

    /*
     * Hand the drawing over to whoever owns the pixels.  While this is on the
     * widget stays hidden and every change that would have altered what is drawn
     * -- a move, the fade, going to sleep -- comes out as changed().
     */
    void setRedirected(bool on);
    bool isRedirected() const { return m_redirected; }

    /*
     * The arrow as premultiplied ARGB, the size of this widget, with its tip at
     * 0,0 so that the caller can place it at hotspot() with nothing to adjust.
     * A null image when there is nothing to draw, which the caller should treat as
     * "clear the layer" rather than "draw nothing this time".
     */
    QImage snapshot() const;

public slots:
    void onMove(qreal dx, qreal dy);
    void onButton(int button, bool pressed);
    void onWheel(int x, int y);

signals:
    /* Emitted when the cursor appears or disappears, so the shell can drop the
     * keyboard-focus highlight while a pointer is in use and bring it back after. */
    void awakeChanged(bool awake);

    /* What snapshot() would return, or where it goes, is not what it was.  Emitted
     * in both modes on purpose -- see the note on announce() in pointer.cpp -- and
     * not emitted at all while the cursor is still. */
    void changed();

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void onIdle();
    void onFade();

private:
    QWidget *targetAt(const QPoint &hostPos) const;
    /*
     * Sends to the DEEPEST widget under the tip and lets Qt do the rest.
     * QApplication::notify contains the parent-walk for unaccepted mouse events,
     * so going through sendEvent -- rather than calling the receiver's event()
     * directly -- gets propagation, mouse-tracking suppression and
     * WA_NoMousePropagation for nothing.
     */
    void dispatch(QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons,
                  const QPoint &hostPos);
    void updateEnterLeave(const QPoint &hostPos);
    void setAwake(bool awake);
    void applyPosition();
    /* /run is the hand-off between this linuxfb cursor and j36-padx's X cursor.
     * Only one is visible at a time; both read before waking and write after a
     * whole-pixel move, so switching surfaces never creates a second location. */
    void readSharedPosition();
    void writeSharedPosition();
    /* The arrow itself, into whatever painter is offered -- the widget's, or
     * snapshot()'s image.  Tip at the painter's origin. */
    void paintBody(QPainter &p) const;
    /* changed(), unless nothing about the picture actually moved. */
    void announce();

    QWidget *m_host = nullptr;
    /* Fractional, because a slow stick moves a third of a pixel per tick and
     * rounding each tick to zero would make low speeds mean "does not move". */
    QPointF m_exact;
    QPoint m_sharedAt = QPoint(-1, -1);

    /* Held from press to release so a drag keeps going to the widget the press
     * landed on, even once the cursor has left it -- which is how a scrollbar
     * works and how a list rubber-band selection works. */
    QPointer<QWidget> m_grab;
    QPointer<QWidget> m_under;
    Qt::MouseButtons m_buttons = Qt::NoButton;

    /* Double click: the previous press, for as long as it is a candidate. */
    QElapsedTimer m_clickTimer;
    qint64 m_lastPressMs = -1;
    QPoint m_lastPressPos;
    int m_lastPressButton = 0;

    QTimer *m_idle = nullptr;
    QTimer *m_fade = nullptr;
    int m_opacity = 255;
    bool m_awake = false;
    bool m_redirected = false;

    /* What was last handed to the owner of the pixels.  A stick produces a move
     * event per poll and several of the calls below wake() as well, so without
     * this a still cursor would re-render, re-upload and re-composite a film a
     * hundred times a second to say nothing had changed.  Opacity -1 means
     * "nothing handed over yet", which is what makes the first one go. */
    QPoint m_shownAt;
    int m_shownOpacity = -1;
};

#endif /* MIXDASH_POINTER_H */
