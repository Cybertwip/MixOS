/* SPDX-License-Identifier: MS-PL */
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
 */
#ifndef MIXDASH_POINTER_H
#define MIXDASH_POINTER_H

#include <QElapsedTimer>
#include <QEvent>
#include <QPointF>
#include <QPointer>
#include <QWidget>

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

public slots:
    void onMove(qreal dx, qreal dy);
    void onButton(int button, bool pressed);
    void onWheel(int delta);

signals:
    /* Emitted when the cursor appears or disappears, so the shell can drop the
     * keyboard-focus highlight while a pointer is in use and bring it back after. */
    void awakeChanged(bool awake);

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

    QWidget *m_host = nullptr;
    /* Fractional, because a slow stick moves a third of a pixel per tick and
     * rounding each tick to zero would make low speeds mean "does not move". */
    QPointF m_exact;

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
};

#endif /* MIXDASH_POINTER_H */
