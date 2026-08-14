/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * theme.h -- the palette and the metrics, taken from MVII's dashboard.
 *
 * Every triple below is copied out of PowerEngine's DashboardRenderer::applyTheme
 * so that this dashboard looks like that one rather than approximately like it.
 * MVII composites in RGB565 and these were written as five/six-bit values there;
 * they are the same numbers, at eight bits, because Qt's raster paint engine on
 * this framebuffer is x8r8g8b8 and converting them down would only lose the
 * gradients.
 *
 * What is deliberately NOT ported is how MVII draws them: it owns a hand-written
 * software compositor and a shader pipeline this board cannot afford.  Here the
 * pixels are Qt's problem.  This file is only the look.
 */
#ifndef MIXDASH_THEME_H
#define MIXDASH_THEME_H

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QRectF>

namespace Theme {

/* Desktop and chrome. */
inline QColor desk()        { return QColor( 26,  28,  38); }
inline QColor deskLow()     { return QColor( 15,  16,  24); }
inline QColor menubar()     { return QColor( 38,  40,  50); }
inline QColor menubarLine() { return QColor( 20,  22,  30); }
inline QColor window()      { return QColor( 44,  46,  56); }
inline QColor titlebar()    { return QColor( 54,  56,  68); }
inline QColor titlebarLow() { return QColor( 46,  48,  60); }
inline QColor card()        { return QColor( 56,  58,  70); }
inline QColor cardLow()     { return QColor( 48,  50,  62); }
inline QColor border()      { return QColor( 74,  78,  92); }
inline QColor separator()   { return QColor( 64,  68,  82); }
inline QColor glass()       { return QColor(  8,   9,  14); }

/* Ink, in three weights. */
inline QColor ink()         { return QColor(232, 234, 242); }
inline QColor ink2()        { return QColor(168, 174, 188); }
inline QColor ink3()        { return QColor(116, 122, 138); }

/* Accents. */
inline QColor blue()        { return QColor( 10, 132, 255); }
inline QColor blueLow()     { return QColor(  0, 104, 230); }
inline QColor teal()        { return QColor( 48, 176, 199); }
inline QColor pink()        { return QColor(255,  99, 132); }
inline QColor purple()      { return QColor(148, 112, 219); }
inline QColor orange()      { return QColor(255, 159,  10); }
inline QColor green()       { return QColor( 40, 200,  64); }
inline QColor red()         { return QColor(255,  95,  86); }
inline QColor yellow()      { return QColor(254, 188,  46); }
inline QColor dock()        { return QColor( 60,  64,  74); }
inline QColor dockHi()      { return QColor( 92,  96, 108); }

/* The two alphas MVII names, and they are what make the chrome read as glass. */
enum { ChromeAlpha = 232, DockAlpha = 220 };

/*
 * Metrics.  StatusH is 40 because DashboardLayout.hpp picks 40 for a panel
 * 480 rows or taller, and this panel is exactly 640x480 -- the LK's
 * simple-framebuffer node in the device tree says so.
 */
enum {
    StatusH = 40,
    DockH   = 56,
    Margin  = 14,
    Gap     = 12,
    Radius  = 14
};

/*
 * THE CARD GRID IS SLOTS OF A FIXED SIZE, NOT A FIXED NUMBER OF SLOTS.
 *
 * It was GridCols = 3 and GridRows = 2, and the card was whatever shape six of
 * them made out of the page -- so adding a seventh card made all seven shorter,
 * and a tab with two cards on it drew two slabs and a hole.  Both of those are
 * the same bug: the layout was derived from the contents.
 *
 * These two numbers are the contract instead.  A slot is 144x110 and stays 144x110
 * whether there are three cards or thirty; the column count is however many whole
 * slots fit across the page, and the row count is however many the cards need.
 * That is what makes the grid sortable -- a slot the user can point at exists
 * whether or not anything is in it -- and it is what makes an installed package
 * able to appear on this page without redesigning it.
 *
 * NOMINAL, and the width is stretched: 640 - 2*14 margin - 3*12 gap is 576, which
 * is exactly four of these, and a panel that is not 640 wide gets as many columns
 * as fit and shares the remainder between them.  That matters on this build --
 * the dashboard follows a USB-HDMI adapter to whatever it reports, and eight
 * columns of the same 144 px card is the right answer on a 1280-wide television,
 * not four cards stretched to 300.
 */
enum {
    SlotW = 144,
    SlotH = 110
};

/*
 * Pixel sizes, never point sizes.  Qt derives points from a DPI it has to guess,
 * and simplefb reports no physical size at all, so a point size here would render
 * at whatever Qt's fallback DPI happens to be on the day.
 */
inline QFont font(int px, bool bold = false)
{
    QFont f = QGuiApplication::font();
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

/* A vertical two-stop fill, which is most of MVII's look in one call. */
inline void vgrad(QPainter &p, const QRectF &r, const QColor &top,
                  const QColor &bottom, qreal radius = 0.0)
{
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0.0, top);
    g.setColorAt(1.0, bottom);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    if (radius > 0.0)
        p.drawRoundedRect(r, radius, radius);
    else
        p.drawRect(r);
}

/*
 * A soft shadow as concentric strokes rather than a blur.  QGraphicsDropShadow
 * would be a real Gaussian and a real full-surface repaint every frame; six
 * rounded strokes at a low alpha read the same at this size and cost nothing on a
 * Cortex-A7.
 */
inline void softShadow(QPainter &p, const QRectF &r, qreal radius, int spread = 6,
                       int alpha = 26)
{
    p.setBrush(Qt::NoBrush);
    for (int i = spread; i >= 1; --i) {
        QColor c = glass();
        c.setAlpha(alpha * (spread - i + 1) / spread);
        p.setPen(QPen(c, 1.0));
        p.drawRoundedRect(r.adjusted(-i, -i + 1, i, i + 1), radius + i, radius + i);
    }
}

} /* namespace Theme */

#endif /* MIXDASH_THEME_H */
