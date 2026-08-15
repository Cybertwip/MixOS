/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "pointer.h"

#include <QApplication>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include "settings.h"
#include "theme.h"

namespace {

/* The widget is this big; the arrow is drawn inside it with its tip at 0,0. */
const int kWidth = 20;
const int kHeight = 28;

/* Fade out over roughly a fifth of a second, in seven steps.  Long enough to be
 * seen leaving rather than to look like a dropped frame, short enough that it is
 * over before the user wonders whether it is still there. */
const int kFadeStepMs = 28;
const int kFadeStep = 36;

/* How far a second press may be from the first and still be a double click.
 * Generous, because the thing aiming is a thumbstick. */
const int kDoubleClickSlop = 10;

QPainterPath arrowPath()
{
    /* The classic arrow, tip at the origin.  Written out rather than loaded from
     * an image because a 20 px bitmap would need a second one for the shadow and
     * a third if the panel is ever a different size. */
    QPainterPath p;
    p.moveTo(0.0, 0.0);
    p.lineTo(0.0, 20.4);
    p.lineTo(5.0, 15.8);
    p.lineTo(8.4, 23.5);
    p.lineTo(12.2, 21.8);
    p.lineTo(8.9, 14.4);
    p.lineTo(15.1, 13.9);
    p.closeSubpath();
    return p;
}

} /* namespace */

Pointer::Pointer(QWidget *host)
    : QWidget(host)
    , m_host(host)
{
    setFixedSize(kWidth, kHeight);
    setFocusPolicy(Qt::NoFocus);
    /* Without this the cursor would be found under itself by childAt() and every
     * click would land on the arrow. */
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    /* No background fill: the parent repaints under us, which is what makes the
     * unpainted corners of the arrow transparent. */
    setAttribute(Qt::WA_NoSystemBackground, true);
    hide();

    m_exact = QPointF(host->width() / 2.0, host->height() / 2.0);
    applyPosition();

    m_clickTimer.start();

    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, &Pointer::onIdle);

    m_fade = new QTimer(this);
    m_fade->setInterval(kFadeStepMs);
    connect(m_fade, &QTimer::timeout, this, &Pointer::onFade);
}

void Pointer::wake()
{
    m_fade->stop();
    if (m_opacity != 255) {
        m_opacity = 255;
        update();
        announce();
    }
    if (!isVisible()) {
        applyPosition();
        /* Redirected, the widget stays hidden for good: showing it is exactly the
         * memcpy of stale backing store this mode exists to stop.  announce() is
         * what puts the arrow back on the glass instead. */
        if (!m_redirected) {
            show();
            raise();
        }
        announce();
    }
    setAwake(true);

    /* Re-armed on every movement and every click, which is what makes the
     * timeout mean "idle for four seconds" rather than "visible for four". */
    m_idle->start(Settings::instance().mouse().hideSeconds * 1000);
}

void Pointer::sleep()
{
    m_idle->stop();
    m_fade->stop();
    m_opacity = 255;
    if (isVisible())
        hide();

    /*
     * A button held when the pointer is put away would otherwise be held for
     * ever, and the widget that received the press would stay in its pressed
     * state.  Release what is down, at the last known position.
     */
    if (m_buttons != Qt::NoButton) {
        const QPoint p = hotspot();
        const Qt::MouseButtons was = m_buttons;
        for (int i = 0; i < 3; ++i) {
            static const Qt::MouseButton kButtons[3] =
                { Qt::LeftButton, Qt::MiddleButton, Qt::RightButton };
            if (!(was & kButtons[i]))
                continue;
            m_buttons &= ~kButtons[i];
            dispatch(QEvent::MouseButtonRelease, kButtons[i], m_buttons, p);
        }
        m_buttons = Qt::NoButton;
    }
    m_grab = nullptr;

    if (m_under) {
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(m_under, &leave);
        m_under = nullptr;
    }

    setAwake(false);
}

void Pointer::setAwake(bool awake)
{
    if (m_awake == awake)
        return;
    m_awake = awake;
    announce();
    emit awakeChanged(awake);
}

/*
 * EMITTED IN BOTH MODES, and that is what makes the mode switch possible at all.
 * The shell cannot know a film has started owning the screen until something asks
 * it, and the only thing that asks is the cursor moving -- so a changed() that
 * fired only while redirected could never be the thing that turns the redirect
 * on.  The dedup below is what keeps that affordable: a still cursor is silent,
 * whichever mode it is in.
 */
void Pointer::announce()
{
    /* Asleep is a state with a picture too -- the empty one -- so it goes through
     * the same comparison rather than being special-cased into a separate signal
     * that the owner would have to remember to clear the layer from. */
    const int opacity = m_awake ? m_opacity : 0;
    if (pos() == m_shownAt && opacity == m_shownOpacity)
        return;
    m_shownAt = pos();
    m_shownOpacity = opacity;
    emit changed();
}

void Pointer::setRedirected(bool on)
{
    if (m_redirected == on)
        return;
    m_redirected = on;

    /* Whichever way this went, what the other side is showing is now wrong: on the
     * way in there is a Qt-drawn arrow to take down, and on the way out there is
     * a GPU-drawn one that nothing will refresh again.  -1 makes the announce()
     * below unconditional, and hide()/show() settle Qt's half. */
    m_shownOpacity = -1;

    if (on) {
        if (isVisible())
            hide();
        announce();
        return;
    }

    if (m_awake) {
        applyPosition();
        show();
        raise();
        update();
    }
}

void Pointer::onIdle()
{
    if (m_buttons != Qt::NoButton) {
        /* Something is being held down.  A cursor that vanishes mid-drag is a
         * cursor that has lost the drag, so wait for the release instead. */
        m_idle->start(Settings::instance().mouse().hideSeconds * 1000);
        return;
    }
    m_fade->start();
}

void Pointer::onFade()
{
    m_opacity -= kFadeStep;
    if (m_opacity <= 0) {
        m_opacity = 255;
        m_fade->stop();
        hide();
        if (m_under) {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(m_under, &leave);
            m_under = nullptr;
        }
        setAwake(false);
        return;
    }
    update();
    announce();
}

void Pointer::applyPosition()
{
    if (!m_host)
        return;

    /*
     * Clamped to the host, not to the host minus the cursor: the tip is the hot
     * spot, so letting it reach the last column is correct and the body being
     * clipped there is what every other cursor on every other system does.
     */
    const qreal maxX = qMax(0, m_host->width() - 1);
    const qreal maxY = qMax(0, m_host->height() - 1);
    if (m_exact.x() < 0.0)
        m_exact.setX(0.0);
    if (m_exact.y() < 0.0)
        m_exact.setY(0.0);
    if (m_exact.x() > maxX)
        m_exact.setX(maxX);
    if (m_exact.y() > maxY)
        m_exact.setY(maxY);

    const QPoint p(qRound(m_exact.x()), qRound(m_exact.y()));
    if (p != pos()) {
        move(p);
        /* move() on a hidden widget is still where the hot spot is, and while
         * redirected that is the only thing that carries the arrow. */
        announce();
    }
}

void Pointer::onMove(qreal dx, qreal dy)
{
    if (dx == 0.0 && dy == 0.0)
        return;

    const QPoint before = pos();
    m_exact += QPointF(dx, dy);
    applyPosition();
    wake();

    const QPoint now = pos();
    if (now == before && m_buttons == Qt::NoButton)
        return; /* Sub-pixel: nothing has actually moved yet. */

    updateEnterLeave(now);

    /*
     * A move with a button down goes to the widget the press landed on, at
     * coordinates that may well be outside it -- that is exactly what a scrollbar
     * drag needs, and what a list needs to keep extending a selection.
     */
    if (m_grab) {
        const QPoint local = m_grab->mapFrom(m_host, now);
        QMouseEvent ev(QEvent::MouseMove, local, now, m_host->mapToGlobal(now),
                       Qt::NoButton, m_buttons, Qt::NoModifier);
        QApplication::sendEvent(m_grab, &ev);
        return;
    }

    dispatch(QEvent::MouseMove, Qt::NoButton, m_buttons, now);
}

void Pointer::onButton(int button, bool pressed)
{
    const Qt::MouseButton b = static_cast<Qt::MouseButton>(button);
    const QPoint p = hotspot();

    /*
     * A click while the cursor is asleep wakes it and is otherwise ignored.
     * Clicking on something you cannot see is not a thing the user meant to do,
     * and on a handheld the stick click is easy to catch with a palm.
     */
    if (!m_awake) {
        if (pressed)
            wake();
        return;
    }
    wake();

    if (pressed) {
        m_buttons |= b;

        QEvent::Type type = QEvent::MouseButtonPress;
        const qint64 now = m_clickTimer.elapsed();
        if (m_lastPressMs >= 0 && m_lastPressButton == button
            && (now - m_lastPressMs) <= Settings::instance().mouse().doubleClickMs
            && (p - m_lastPressPos).manhattanLength() <= kDoubleClickSlop) {
            type = QEvent::MouseButtonDblClick;
            /* Do not let a third press inside the window become another double
             * click: Qt's own sequence is press, release, dblclick, release. */
            m_lastPressMs = -1;
        } else {
            m_lastPressMs = now;
            m_lastPressPos = p;
            m_lastPressButton = button;
        }

        m_grab = targetAt(p);
        dispatch(type, b, m_buttons, p);
        return;
    }

    m_buttons &= ~b;
    if (m_grab) {
        const QPoint local = m_grab->mapFrom(m_host, p);
        QMouseEvent ev(QEvent::MouseButtonRelease, local, p, m_host->mapToGlobal(p),
                       b, m_buttons, Qt::NoModifier);
        QApplication::sendEvent(m_grab, &ev);
    } else {
        dispatch(QEvent::MouseButtonRelease, b, m_buttons, p);
    }
    if (m_buttons == Qt::NoButton)
        m_grab = nullptr;
}

void Pointer::onWheel(int delta)
{
    if (delta == 0)
        return;
    wake();

    const QPoint p = hotspot();
    QWidget *w = targetAt(p);
    if (!w)
        return;

    const QPointF local = w->mapFrom(m_host, p);
    const QPointF global = m_host->mapToGlobal(p);

#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    QWheelEvent ev(local, global, QPoint(0, 0), QPoint(0, delta), Qt::NoButton,
                   Qt::NoModifier, Qt::NoScrollPhase, false);
#else
    QWheelEvent ev(local, global, QPoint(0, 0), QPoint(0, delta), delta, Qt::Vertical,
                   Qt::NoButton, Qt::NoModifier);
#endif
    QApplication::sendEvent(w, &ev);
}

QWidget *Pointer::targetAt(const QPoint &hostPos) const
{
    if (!m_host)
        return nullptr;
    /* childAt skips hidden children and anything WA_TransparentForMouseEvents,
     * which is why this does not find the cursor itself. */
    QWidget *w = m_host->childAt(hostPos);
    return w ? w : m_host;
}

void Pointer::updateEnterLeave(const QPoint &hostPos)
{
    QWidget *w = targetAt(hostPos);
    if (w == m_under)
        return;

    if (m_under) {
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(m_under, &leave);
    }
    m_under = w;
    if (w) {
        const QPointF local = w->mapFrom(m_host, hostPos);
        QEnterEvent enter(local, hostPos, m_host->mapToGlobal(hostPos));
        QApplication::sendEvent(w, &enter);
    }
}

void Pointer::dispatch(QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons,
                       const QPoint &hostPos)
{
    QWidget *w = targetAt(hostPos);
    if (!w)
        return;

    const QPointF local = w->mapFrom(m_host, hostPos);
    QMouseEvent ev(type, local, hostPos, m_host->mapToGlobal(hostPos), button, buttons,
                   Qt::NoModifier);
    /* QInputEvent's constructor accepts by default; clearing it is what lets
     * QApplication::notify's parent-walk run when nobody handles this. */
    ev.setAccepted(false);
    QApplication::sendEvent(w, &ev);
}

QImage Pointer::snapshot() const
{
    if (!m_awake || m_opacity <= 0)
        return QImage();

    /* Premultiplied, like every other thing handed to GlVideo::setOverlay: the
     * arrow is antialiased and its shadow is translucent, so the whole rectangle
     * carries meaningful alpha and the un-multiply happens on the way to the GPU. */
    QImage img(size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintBody(p);
    return img;
}

void Pointer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintBody(p);
}

void Pointer::paintBody(QPainter &p) const
{
    p.setOpacity(m_opacity / 255.0);

    const QPainterPath path = arrowPath();

    /* A shadow, offset by a pixel and a half, so the arrow stays visible over a
     * white image in the Media viewer as well as over the dark desktop. */
    p.translate(1.4, 1.8);
    p.setPen(Qt::NoPen);
    QColor shade = Theme::glass();
    shade.setAlpha(120);
    p.setBrush(shade);
    p.drawPath(path);
    p.translate(-1.4, -1.8);

    p.setBrush(QColor(250, 251, 255));
    p.setPen(QPen(QColor(24, 26, 34), 1.2));
    p.drawPath(path);
}
