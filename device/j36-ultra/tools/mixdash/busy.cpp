/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * busy.cpp -- see busy.h for what this is and why it can stop being a widget.
 */
#include "busy.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QtMath>

#include "theme.h"

namespace {

/* Twelve steps at 80 ms is a revolution a second.  Both numbers are here rather
 * than spread through the file because they are one decision: change the count
 * and the interval has to change with it or the ring changes speed. */
const int StepCount = 12;
const int StepMs = 80;

/* The panel: a ring on top, one line of caption under it.  Sized once, as a
 * constant, because the shell centres it and a widget that resized itself while
 * the caption changed would walk about the screen as it worked. */
const int PanelW = 232;
const int PanelH = 108;
const int RingR = 22;           /* radius of the circle the marks travel on */
const int RingCx = PanelW / 2;
const int RingCy = 36;
const int CaptionTop = 70;

}

Busy::Busy(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    resize(PanelW, PanelH);
    hide();

    m_timer = new QTimer(this);
    m_timer->setInterval(StepMs);
    connect(m_timer, &QTimer::timeout, this, &Busy::onStep);
}

void Busy::placeIn(const QRect &panel)
{
    move(panel.x() + (panel.width() - width()) / 2,
         panel.y() + (panel.height() - height()) / 2);
}

void Busy::start(const QString &caption)
{
    const bool wasSpinning = m_spinning;
    m_caption = caption;
    m_spinning = true;
    if (!wasSpinning)
        m_timer->start();
    if (!m_redirected && !wasSpinning) {
        show();
        raise();
    }
    update();
    emit changed();
}

void Busy::stop()
{
    if (!m_spinning)
        return;
    m_spinning = false;
    m_caption.clear();
    m_timer->stop();
    /*
     * The phase is reset HERE and not in start(), so that a spinner which comes
     * back for the next file starts from the top rather than from wherever the
     * last one happened to stop -- and so that two spinners in a row look like two
     * things happening rather than one thing that never finished.
     */
    m_phase = 0;
    if (!m_redirected)
        hide();
    emit changed();
}

void Busy::setPointerStyle(bool on)
{
    if (m_dots == on)
        return;
    m_dots = on;
    if (!m_spinning)
        return;
    update();
    emit changed();
}

void Busy::setRedirected(bool on)
{
    if (m_redirected == on)
        return;
    m_redirected = on;
    if (on) {
        if (isVisible())
            hide();
    } else if (m_spinning) {
        show();
        raise();
        update();
    }
    /* Unconditionally: whoever is taking over has nothing on their layer yet, and
     * whoever is handing back has just left a texture behind that has to be
     * cleared.  Both of those are "the picture is not what you have". */
    emit changed();
}

void Busy::onStep()
{
    m_phase = (m_phase + 1) % StepCount;
    if (!m_redirected)
        update();
    emit changed();
}

QImage Busy::snapshot() const
{
    if (!m_spinning)
        return QImage();

    QImage img(size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintBody(p);
    return img;
}

void Busy::paintEvent(QPaintEvent *)
{
    if (!m_spinning)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    paintBody(p);
}

void Busy::paintBody(QPainter &p) const
{
    /*
     * A plate under it, because this lands on a film as often as on a menu and
     * white marks on a bright scene are marks nobody can see.  Rounded and mostly
     * opaque: enough of the picture shows through to say the film is still there,
     * not enough for the ring to compete with it.
     */
    const QRectF plate(0, 0, width(), height());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(8, 9, 14, 210));
    p.drawRoundedRect(plate.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);

    const QPointF centre(RingCx, RingCy);

    if (m_dots) {
        /*
         * THE CHASING RING.  Five marks a little way apart, all of them going the
         * same way round, each one behind the one in front -- which is what makes
         * it read as motion rather than as five separate dots blinking.  The one
         * at the head is full size and full brightness and the tail fades, so the
         * direction is unambiguous even in a still screenshot.
         */
        const int lead = 5;
        for (int i = 0; i < lead; ++i) {
            const qreal turn = (m_phase - i) * (360.0 / StepCount) - 90.0;
            const qreal rad = qDegreesToRadians(turn);
            const qreal fade = 1.0 - (qreal)i / lead;
            const qreal r = 2.0 + 2.0 * fade;
            QColor c = Theme::ink();
            c.setAlphaF(0.25 + 0.75 * fade);
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(centre.x() + RingR * qCos(rad),
                                  centre.y() + RingR * qSin(rad)), r, r);
        }
    } else {
        /*
         * THE ARC.  A dim ring for the whole circle so there is something to
         * measure the bright part against, and a quarter of it lit and sweeping.
         * drawArc counts in sixteenths of a degree from three o'clock going
         * anticlockwise, hence the negation -- this turns the way a clock does.
         */
        const QRectF ring(centre.x() - RingR, centre.y() - RingR, RingR * 2, RingR * 2);

        QPen back(QColor(255, 255, 255, 46), 4);
        back.setCapStyle(Qt::FlatCap);
        p.setBrush(Qt::NoBrush);
        p.setPen(back);
        p.drawEllipse(ring);

        QPen lit(Theme::blue(), 4);
        lit.setCapStyle(Qt::RoundCap);
        p.setPen(lit);
        const int start = (90 - m_phase * (360 / StepCount)) * 16;
        p.drawArc(ring, start, -100 * 16);
    }

    if (m_caption.isEmpty())
        return;

    p.setPen(Theme::ink2());
    p.setFont(Theme::font(12));
    p.drawText(QRect(10, CaptionTop, width() - 20, height() - CaptionTop - 8),
               Qt::AlignHCenter | Qt::AlignTop, m_caption);
}
