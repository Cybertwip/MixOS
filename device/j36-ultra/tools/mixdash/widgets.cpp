/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "widgets.h"
#include "joypad.h"
#include "theme.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace SysInfo {

QString readTrimmed(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLocal8Bit(f.readAll()).trimmed();
}

/*
 * The cell, if the kernel has anything to say about it.  Until the MT6592 PMIC
 * driver lands there is no power_supply class and this returns -1, which the
 * status bar draws as a dash rather than as 0%.
 */
int batteryCapacity(bool *charging)
{
    if (charging)
        *charging = false;

    const QString base = "/sys/class/power_supply";
    const QStringList supplies = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &s : supplies) {
        const QString dir = base + "/" + s;
        if (readTrimmed(dir + "/type") != "Battery")
            continue;
        const QString cap = readTrimmed(dir + "/capacity");
        if (cap.isEmpty())
            continue;
        if (charging) {
            const QString st = readTrimmed(dir + "/status");
            *charging = (st == "Charging" || st == "Full");
        }
        bool ok = false;
        const int v = cap.toInt(&ok);
        return ok ? qBound(0, v, 100) : -1;
    }
    return -1;
}

bool networkUp()
{
    const QString base = "/sys/class/net";
    const QStringList ifaces = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &i : ifaces) {
        if (i == "lo")
            continue;
        if (readTrimmed(base + "/" + i + "/operstate") == "up")
            return true;
    }
    return false;
}

/*
 * The wireless interface, asked of the kernel rather than guessed from a name.
 * "wlan0" is only a convention, and a USB dongle on this board can just as easily
 * come up as wlx0013effe1234; what all of them have is a phy80211 link.
 */
QString wirelessInterface()
{
    const QString base = "/sys/class/net";
    const QStringList ifaces = QDir(base).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &i : ifaces) {
        if (i == "lo")
            continue;
        if (QFileInfo::exists(base + "/" + i + "/phy80211")
            || QFileInfo::exists(base + "/" + i + "/wireless"))
            return i;
    }
    return QString();
}

} /* namespace SysInfo */

namespace {

using SysInfo::readTrimmed;

QString firstWords(const QString &s, int n)
{
    const QStringList parts = s.split(' ', Qt::SkipEmptyParts);
    return QStringList(parts.mid(0, n)).join(' ');
}

/*
 * Link quality out of 100, from the same file iwconfig reads.  Column 3 of a
 * /proc/net/wireless line is "link", which the kernel reports 0..70 for most
 * drivers.  -1 for "no wireless" and for "wireless but not associated", because
 * three empty bars is the honest drawing of both.
 */
int wirelessQuality()
{
    QFile f("/proc/net/wireless");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;
    while (!f.atEnd()) {
        const QString line = QString::fromLatin1(f.readLine());
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QStringList cols = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        if (cols.size() < 2)
            continue;
        bool ok = false;
        const double link = cols.at(1).remove('.').toDouble(&ok);
        if (!ok || link <= 0.0)
            continue;
        return qBound(1, (int)(link * 100.0 / 70.0), 100);
    }
    return -1;
}

} /* namespace */

/* ── Glyphs ──────────────────────────────────────────────────────────────── */

void paintGlyph(QPainter &p, const QRectF &box, int glyph, const QColor &ink)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(ink, 2.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal x = box.x();
    const qreal y = box.y();
    const qreal w = box.width();
    const qreal h = box.height();

    switch (glyph) {
    case GlyphGames: {
        /* A pad: body, a cross on the left, two buttons on the right. */
        const QRectF body(x, y + h * 0.22, w, h * 0.56);
        p.drawRoundedRect(body, h * 0.24, h * 0.24);
        const qreal cx = x + w * 0.28;
        const qreal cy = body.center().y();
        p.drawLine(QPointF(cx - w * 0.10, cy), QPointF(cx + w * 0.10, cy));
        p.drawLine(QPointF(cx, cy - h * 0.14), QPointF(cx, cy + h * 0.14));
        p.setBrush(ink);
        p.drawEllipse(QPointF(x + w * 0.68, cy - h * 0.08), w * 0.055, w * 0.055);
        p.drawEllipse(QPointF(x + w * 0.80, cy + h * 0.06), w * 0.055, w * 0.055);
        break;
    }
    case GlyphFiles: {
        /* A folder, with the tab drawn as part of the outline. */
        QPainterPath path;
        path.moveTo(x + w * 0.06, y + h * 0.80);
        path.lineTo(x + w * 0.06, y + h * 0.24);
        path.lineTo(x + w * 0.40, y + h * 0.24);
        path.lineTo(x + w * 0.48, y + h * 0.36);
        path.lineTo(x + w * 0.94, y + h * 0.36);
        path.lineTo(x + w * 0.94, y + h * 0.80);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case GlyphVideo: {
        p.drawRoundedRect(QRectF(x + w * 0.05, y + h * 0.20, w * 0.90, h * 0.60),
                          h * 0.14, h * 0.14);
        QPainterPath tri;
        tri.moveTo(x + w * 0.42, y + h * 0.36);
        tri.lineTo(x + w * 0.66, y + h * 0.50);
        tri.lineTo(x + w * 0.42, y + h * 0.64);
        tri.closeSubpath();
        p.setBrush(ink);
        p.drawPath(tri);
        break;
    }
    case GlyphDisplay: {
        p.drawRoundedRect(QRectF(x + w * 0.06, y + h * 0.20, w * 0.88, h * 0.48),
                          h * 0.10, h * 0.10);
        p.drawLine(QPointF(x + w * 0.34, y + h * 0.82), QPointF(x + w * 0.66, y + h * 0.82));
        p.drawLine(QPointF(x + w * 0.50, y + h * 0.68), QPointF(x + w * 0.50, y + h * 0.82));
        break;
    }
    case GlyphSettings: {
        /* Three sliders: legible at 22 px in a way a gear is not. */
        for (int i = 0; i < 3; ++i) {
            const qreal ly = y + h * (0.30 + 0.20 * i);
            p.drawLine(QPointF(x + w * 0.12, ly), QPointF(x + w * 0.88, ly));
            p.setBrush(ink);
            p.drawEllipse(QPointF(x + w * (i == 1 ? 0.66 : 0.34), ly), w * 0.075, w * 0.075);
            p.setBrush(Qt::NoBrush);
        }
        break;
    }
    case GlyphPower: {
        const QRectF arc(x + w * 0.18, y + h * 0.22, w * 0.64, h * 0.64);
        p.drawArc(arc, -60 * 16, 300 * 16);
        p.drawLine(QPointF(x + w * 0.50, y + h * 0.14), QPointF(x + w * 0.50, y + h * 0.46));
        break;
    }
    case GlyphTerminal: {
        p.drawRoundedRect(QRectF(x + w * 0.06, y + h * 0.20, w * 0.88, h * 0.60),
                          h * 0.12, h * 0.12);
        p.drawLine(QPointF(x + w * 0.26, y + h * 0.40), QPointF(x + w * 0.38, y + h * 0.50));
        p.drawLine(QPointF(x + w * 0.38, y + h * 0.50), QPointF(x + w * 0.26, y + h * 0.60));
        p.drawLine(QPointF(x + w * 0.48, y + h * 0.62), QPointF(x + w * 0.72, y + h * 0.62));
        break;
    }
    case GlyphBack: {
        p.drawLine(QPointF(x + w * 0.62, y + h * 0.26), QPointF(x + w * 0.36, y + h * 0.50));
        p.drawLine(QPointF(x + w * 0.36, y + h * 0.50), QPointF(x + w * 0.62, y + h * 0.74));
        break;
    }
    case GlyphWifi: {
        /* Three arcs and a dot, struck about a centre below the box so the
         * curvature reads as a broadcast rather than as a rainbow. */
        const QPointF c(x + w * 0.50, y + h * 0.78);
        for (int i = 0; i < 3; ++i) {
            const qreal r = w * (0.16 + 0.14 * i);
            p.drawArc(QRectF(c.x() - r, c.y() - r, r * 2, r * 2), 35 * 16, 110 * 16);
        }
        p.setBrush(ink);
        p.drawEllipse(c, w * 0.05, w * 0.05);
        break;
    }
    case GlyphMouse: {
        p.drawRoundedRect(QRectF(x + w * 0.28, y + h * 0.16, w * 0.44, h * 0.68),
                          w * 0.22, w * 0.22);
        p.drawLine(QPointF(x + w * 0.50, y + h * 0.16), QPointF(x + w * 0.50, y + h * 0.44));
        p.drawLine(QPointF(x + w * 0.28, y + h * 0.44), QPointF(x + w * 0.72, y + h * 0.44));
        break;
    }
    case GlyphPackage: {
        /* A carton: a box with a lid seam and a strap. */
        p.drawRect(QRectF(x + w * 0.12, y + h * 0.32, w * 0.76, h * 0.50));
        p.drawLine(QPointF(x + w * 0.12, y + h * 0.48), QPointF(x + w * 0.88, y + h * 0.48));
        p.drawLine(QPointF(x + w * 0.50, y + h * 0.32), QPointF(x + w * 0.50, y + h * 0.82));
        p.drawLine(QPointF(x + w * 0.12, y + h * 0.32), QPointF(x + w * 0.30, y + h * 0.18));
        p.drawLine(QPointF(x + w * 0.88, y + h * 0.32), QPointF(x + w * 0.70, y + h * 0.18));
        break;
    }
    case GlyphMusic: {
        p.setBrush(ink);
        p.drawEllipse(QPointF(x + w * 0.30, y + h * 0.70), w * 0.12, w * 0.10);
        p.drawEllipse(QPointF(x + w * 0.70, y + h * 0.60), w * 0.12, w * 0.10);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(x + w * 0.42, y + h * 0.70), QPointF(x + w * 0.42, y + h * 0.24));
        p.drawLine(QPointF(x + w * 0.82, y + h * 0.60), QPointF(x + w * 0.82, y + h * 0.16));
        p.drawLine(QPointF(x + w * 0.42, y + h * 0.24), QPointF(x + w * 0.82, y + h * 0.16));
        break;
    }
    case GlyphImage: {
        p.drawRoundedRect(QRectF(x + w * 0.08, y + h * 0.22, w * 0.84, h * 0.56),
                          h * 0.10, h * 0.10);
        p.setBrush(ink);
        p.drawEllipse(QPointF(x + w * 0.32, y + h * 0.40), w * 0.06, w * 0.06);
        p.setBrush(Qt::NoBrush);
        QPainterPath hill;
        hill.moveTo(x + w * 0.14, y + h * 0.72);
        hill.lineTo(x + w * 0.40, y + h * 0.48);
        hill.lineTo(x + w * 0.58, y + h * 0.66);
        hill.lineTo(x + w * 0.70, y + h * 0.56);
        hill.lineTo(x + w * 0.88, y + h * 0.72);
        p.drawPath(hill);
        break;
    }
    case GlyphChip: {
        p.drawRoundedRect(QRectF(x + w * 0.26, y + h * 0.26, w * 0.48, h * 0.48), 3, 3);
        for (int i = 0; i < 3; ++i) {
            const qreal t = x + w * (0.36 + 0.14 * i);
            p.drawLine(QPointF(t, y + h * 0.26), QPointF(t, y + h * 0.12));
            p.drawLine(QPointF(t, y + h * 0.74), QPointF(t, y + h * 0.88));
            const qreal s = y + h * (0.36 + 0.14 * i);
            p.drawLine(QPointF(x + w * 0.26, s), QPointF(x + w * 0.12, s));
            p.drawLine(QPointF(x + w * 0.74, s), QPointF(x + w * 0.88, s));
        }
        break;
    }
    case GlyphInfo: {
        p.drawEllipse(QRectF(x + w * 0.16, y + h * 0.16, w * 0.68, h * 0.68));
        p.drawLine(QPointF(x + w * 0.50, y + h * 0.46), QPointF(x + w * 0.50, y + h * 0.70));
        p.setBrush(ink);
        p.drawEllipse(QPointF(x + w * 0.50, y + h * 0.34), w * 0.045, w * 0.045);
        break;
    }
    default:
        break;
    }

    p.restore();
}

/* ── Sheet chrome ────────────────────────────────────────────────────────── */

QRectF paintSheet(QPainter &p, const QRectF &card, const QString &title,
                  const QString &rightText)
{
    p.setRenderHint(QPainter::Antialiasing, true);

    Theme::softShadow(p, card, Theme::Radius, 6, 24);
    Theme::vgrad(p, card, Theme::window(), Theme::window().darker(112), Theme::Radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), Theme::Radius, Theme::Radius);

    QPainterPath clip;
    clip.addRoundedRect(card, Theme::Radius, Theme::Radius);
    p.save();
    p.setClipPath(clip);
    const QRectF head(card.x(), card.y(), card.width(), 34);
    Theme::vgrad(p, head, Theme::titlebar(), Theme::titlebarLow());
    p.setPen(QPen(Theme::separator(), 1.0));
    p.drawLine(QPointF(head.x(), head.bottom() - 0.5), QPointF(head.right(), head.bottom() - 0.5));
    p.restore();

    qreal rightW = 0;
    if (!rightText.isEmpty()) {
        const QFont rf = Theme::font(12);
        const QFontMetrics rfm(rf);
        rightW = rfm.horizontalAdvance(rightText) + 8;
        p.setFont(rf);
        p.setPen(Theme::ink3());
        p.drawText(QRectF(head.right() - 14 - rightW, head.y(), rightW, head.height()),
                   Qt::AlignRight | Qt::AlignVCenter, rightText);
    }

    const QFont f = Theme::font(13, true);
    const QFontMetrics fm(f);
    p.setFont(f);
    p.setPen(Theme::ink());
    const QRectF text = head.adjusted(14, 0, -14 - rightW, 0);
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(title, Qt::ElideMiddle, (int)qMax(10.0, text.width())));

    return QRectF(card.x() + 1, head.bottom() + 1, card.width() - 2,
                  card.bottom() - head.bottom() - 2);
}

/* ── PageWidget ──────────────────────────────────────────────────────────── */

PageWidget::PageWidget(QWidget *parent)
    : QWidget(parent)
{
    /* Every page is pointer-driven as well as pad-driven, and hover highlights
     * need moves with no button down. */
    setMouseTracking(true);
}

bool PageWidget::handleNav(int) { return false; }
void PageWidget::onEnter() {}
void PageWidget::onLeave() {}
bool PageWidget::wantsFullscreen() const { return false; }
bool PageWidget::wantsKeys() const { return false; }
void PageWidget::keyPressed(int, bool, int) {}
void PageWidget::textEntered(const QString &, bool) {}

/* ── StatusBar ───────────────────────────────────────────────────────────── */

StatusBar::StatusBar(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(Theme::StatusH);

    refresh();
    QTimer *t = new QTimer(this);
    t->setInterval(5000);
    connect(t, &QTimer::timeout, this, &StatusBar::refresh);
    t->start();
}

void StatusBar::setTitle(const QString &title)
{
    if (m_title == title)
        return;
    m_title = title;
    update();
}

void StatusBar::refresh()
{
    m_capacity = SysInfo::batteryCapacity(&m_charging);
    m_net = SysInfo::networkUp();
    m_wifi = wirelessQuality();
    /* The clock is in here too, so one timer covers everything that moves. */
    update();
}

void StatusBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal w = width();
    const qreal h = height();

    /* Glass, then MVII's white sheen along the top and its hairline at the foot. */
    QColor bar = Theme::menubar();
    bar.setAlpha(Theme::ChromeAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush(bar);
    p.drawRect(QRectF(0, 0, w, h));
    p.setPen(QPen(QColor(255, 255, 255, 150), 1.0));
    p.drawLine(QPointF(0, 0.5), QPointF(w, 0.5));
    p.setPen(QPen(Theme::menubarLine(), 1.0));
    p.drawLine(QPointF(0, h - 0.5), QPointF(w, h - 0.5));

    /* The brand mark: three lamps, ours rather than a copy of somebody's. */
    const qreal by = h / 2.0;
    const struct { qreal dx; QColor c; } lamps[3] = {
        { 18.0, Theme::blue() }, { 25.0, Theme::teal() }, { 32.0, Theme::pink() }
    };
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        QColor c = lamps[i].c;
        c.setAlpha(215);
        p.setBrush(c);
        p.drawEllipse(QPointF(lamps[i].dx, by), 6.0, 6.0);
    }

    const QFont brandFont = Theme::font(14, true);
    const QFontMetrics brandMetrics(brandFont);
    p.setFont(brandFont);
    p.setPen(Theme::ink());
    const qreal brandX = 46.0;
    p.drawText(QRectF(brandX, 0, 90, h), Qt::AlignLeft | Qt::AlignVCenter, "MixOS");

    /* ── the right-hand cluster, laid out from the right edge inwards ── */
    qreal rx = w - 12.0;

    const QFont clockFont = Theme::font(13, true);
    const QFontMetrics clockMetrics(clockFont);
    const QString clock = QDateTime::currentDateTime().toString("HH:mm");
    const qreal clockW = clockMetrics.horizontalAdvance(clock);
    p.setFont(clockFont);
    p.setPen(Theme::ink());
    p.drawText(QRectF(rx - clockW, 0, clockW, h), Qt::AlignRight | Qt::AlignVCenter, clock);
    rx -= clockW + 14.0;

    const QFont smallFont = Theme::font(12);
    const QFontMetrics smallMetrics(smallFont);
    if (m_capacity >= 0) {
        const QString pct = QString::number(m_capacity) + "%";
        const qreal pw = smallMetrics.horizontalAdvance(pct);
        p.setFont(smallFont);
        p.setPen(Theme::ink2());
        p.drawText(QRectF(rx - pw, 0, pw, h), Qt::AlignRight | Qt::AlignVCenter, pct);
        rx -= pw + 8.0;
    }

    /* The cell. An empty outline with a dash in it is the honest drawing when the
     * kernel exposes no power_supply class. */
    const qreal bw = 24.0;
    const qreal bh = 12.0;
    const QRectF cell(rx - bw, by - bh / 2.0, bw, bh);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::ink2(), 1.0));
    p.drawRoundedRect(cell, 3.0, 3.0);
    p.setBrush(Theme::ink2());
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(cell.right() + 1.0, by - 3.0, 2.0, 6.0), 1.0, 1.0);
    if (m_capacity >= 0) {
        const QColor fillColour = m_capacity <= 15 ? Theme::red()
                               : m_capacity <= 35 ? Theme::yellow()
                                                  : Theme::green();
        const qreal fw = qMax(2.0, (bw - 4.0) * m_capacity / 100.0);
        p.setBrush(fillColour);
        p.drawRoundedRect(QRectF(cell.x() + 2.0, cell.y() + 2.0, fw, bh - 4.0), 1.5, 1.5);
        if (m_charging) {
            QPainterPath bolt;
            bolt.moveTo(cell.center().x() + 1.0, cell.y() + 1.5);
            bolt.lineTo(cell.center().x() - 2.5, by + 0.5);
            bolt.lineTo(cell.center().x() + 0.5, by + 0.5);
            bolt.lineTo(cell.center().x() - 1.5, cell.bottom() - 1.0);
            bolt.lineTo(cell.center().x() + 3.0, by - 1.0);
            bolt.lineTo(cell.center().x() + 0.0, by - 1.0);
            bolt.closeSubpath();
            p.setBrush(Theme::ink());
            p.drawPath(bolt);
        }
    } else {
        p.setPen(QPen(Theme::ink3(), 1.5));
        p.drawLine(QPointF(cell.center().x() - 4.0, by), QPointF(cell.center().x() + 4.0, by));
    }
    rx -= bw + 14.0;

    /*
     * Three bars.  Lit progressively from the wireless link quality when there is
     * one, and all three lit for a wired link that is up -- a cable has no
     * strength to report and drawing one bar for it would read as a bad signal.
     */
    const int lit = m_wifi >= 0 ? (m_wifi >= 66 ? 3 : m_wifi >= 33 ? 2 : 1)
                                : (m_net ? 3 : 0);
    for (int i = 0; i < 3; ++i) {
        const qreal barH = 4.0 + i * 3.0;
        const QRectF b(rx - 12.0 + i * 5.0, by + 5.0 - barH, 3.0, barH);
        p.setPen(Qt::NoPen);
        p.setBrush(i < lit ? Theme::ink() : Theme::ink3());
        p.drawRoundedRect(b, 1.0, 1.0);
    }
    rx -= 20.0;

    /* Whatever is focused, between the brand and the cluster, elided rather than
     * allowed to run under the clock. */
    if (!m_title.isEmpty()) {
        const qreal tx = brandX + brandMetrics.horizontalAdvance("MixOS") + 14.0;
        const qreal tw = rx - tx - 10.0;
        if (tw > 24.0) {
            const QFont f = Theme::font(13);
            const QFontMetrics fm(f);
            p.setFont(f);
            p.setPen(Theme::ink2());
            p.drawText(QRectF(tx, 0, tw, h), Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(m_title, Qt::ElideRight, (int)tw));
        }
    }
}

/* ── CardGrid ────────────────────────────────────────────────────────────── */

CardGrid::CardGrid(QWidget *parent)
    : PageWidget(parent)
{
}

void CardGrid::setEntries(const QVector<AppEntry> &entries)
{
    m_entries = entries;
    if (m_index >= m_entries.size())
        m_index = qMax(0, m_entries.size() - 1);
    update();
    emit indexChanged(m_index);
}

QString CardGrid::currentTitle() const
{
    if (m_index < 0 || m_index >= m_entries.size())
        return QString();
    return m_entries[m_index].title;
}

QString CardGrid::title() const
{
    const QString t = currentTitle();
    if (m_pageTitle.isEmpty())
        return t;
    if (t.isEmpty())
        return m_pageTitle;
    return m_pageTitle + " -- " + t;
}

bool CardGrid::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:    moveBy(0, -1); return true;
    case Joypad::NavDown:  moveBy(0, 1); return true;
    case Joypad::NavLeft:  moveBy(-1, 0); return true;
    case Joypad::NavRight: moveBy(1, 0); return true;
    case Joypad::NavOk:    activate(); return true;
    default: return false;
    }
}

QRectF CardGrid::cardRect(int i) const
{
    /* qMin<int> spelled out: Theme::GridCols is an unnamed enum, so template
     * deduction against an int has nothing to deduce and the two-argument form
     * does not compile. */
    const int cols = qMin<int>(Theme::GridCols, qMax(1, m_entries.size()));
    const int rows = qMax(1, (m_entries.size() + cols - 1) / cols);

    const qreal availW = width() - 2.0 * Theme::Margin - (cols - 1) * Theme::Gap;
    const qreal availH = height() - 2.0 * Theme::Margin - (rows - 1) * Theme::Gap;
    const qreal cw = availW / cols;
    /* Capped, so a one-row page does not become three enormous slabs, and the
     * block is then centred in what is left. */
    const qreal ch = qMin(availH / rows, 190.0);
    const qreal used = ch * rows + Theme::Gap * (rows - 1);
    const qreal top = Theme::Margin + (height() - 2.0 * Theme::Margin - used) / 2.0;

    const int c = i % cols;
    const int r = i / cols;
    return QRectF(Theme::Margin + c * (cw + Theme::Gap), top + r * (ch + Theme::Gap), cw, ch);
}

int CardGrid::cardAt(const QPoint &p) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (cardRect(i).contains(QPointF(p)))
            return i;
    return -1;
}

void CardGrid::mouseMoveEvent(QMouseEvent *event)
{
    /* Hover selects.  The selection and the cursor are two different things on
     * this device -- the pad owns one and the stick owns the other -- and having
     * the cursor move the selection is what keeps them from disagreeing about
     * what "the current card" is when the user switches hands mid-page. */
    const int i = cardAt(event->pos());
    if (i >= 0 && i != m_index) {
        m_index = i;
        update();
        emit indexChanged(m_index);
    }
    event->accept();
}

void CardGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_pressed = cardAt(event->pos());
    if (m_pressed >= 0 && m_pressed != m_index) {
        m_index = m_pressed;
        update();
        emit indexChanged(m_index);
    }
    event->accept();
}

void CardGrid::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    /* Only if the release is on the card the press landed on, which is what lets
     * a mis-aimed press be slid off and abandoned. */
    const int i = cardAt(event->pos());
    if (i >= 0 && i == m_pressed)
        emit activated(i);
    m_pressed = -1;
    event->accept();
}

void CardGrid::moveBy(int dx, int dy)
{
    if (m_entries.isEmpty())
        return;

    const int cols = qMin<int>(Theme::GridCols, m_entries.size());
    const int rows = qMax(1, (m_entries.size() + cols - 1) / cols);
    int c = m_index % cols;
    int r = m_index / cols;

    if (dx) {
        /* Horizontal wraps -- it is a row of things and the ends should meet. */
        c = (c + dx + cols) % cols;
    }
    if (dy) {
        /* Vertical does not: on two rows, wrapping makes up and down feel like
         * one button. */
        r = qBound(0, r + dy, rows - 1);
    }

    int candidate = r * cols + c;
    if (candidate >= m_entries.size()) {
        /* A short last row: land on its last card rather than nothing. */
        candidate = m_entries.size() - 1;
    }
    if (candidate == m_index)
        return;

    m_index = candidate;
    update();
    emit indexChanged(m_index);
}

void CardGrid::activate()
{
    if (m_index >= 0 && m_index < m_entries.size())
        emit activated(m_index);
}

void CardGrid::paintCard(QPainter &p, const AppEntry &e, const QRectF &r, bool selected)
{
    Theme::softShadow(p, r, Theme::Radius, 6, selected ? 34 : 22);

    const QColor top = selected ? Theme::card().lighter(114) : Theme::card();
    const QColor bottom = selected ? Theme::cardLow().lighter(108) : Theme::cardLow();
    Theme::vgrad(p, r, top, bottom, Theme::Radius);

    /* The gradient foot MVII gives its cards: a darker band along the bottom edge,
     * clipped to the same rounded outline. */
    QPainterPath clip;
    clip.addRoundedRect(r, Theme::Radius, Theme::Radius);
    p.save();
    p.setClipPath(clip);
    QColor foot = Theme::glass();
    foot.setAlpha(46);
    Theme::vgrad(p, QRectF(r.x(), r.bottom() - r.height() * 0.28, r.width(), r.height() * 0.28),
                 QColor(0, 0, 0, 0), foot);
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(selected ? Theme::blue() : Theme::border(), selected ? 2.0 : 1.0));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), Theme::Radius, Theme::Radius);
    if (selected) {
        QColor glow = Theme::blue();
        glow.setAlpha(70);
        p.setPen(QPen(glow, 1.0));
        p.drawRoundedRect(r.adjusted(-2.5, -2.5, 2.5, 2.5), Theme::Radius + 2, Theme::Radius + 2);
    }

    /* The accent block and its glyph. */
    const QRectF icon(r.x() + 16, r.y() + 16, 46, 46);
    const QColor accent = e.available ? e.accent : Theme::separator();
    Theme::vgrad(p, icon, accent.lighter(112), accent.darker(135), 12);
    paintGlyph(p, icon.adjusted(11, 11, -11, -11), e.glyph,
               e.available ? QColor(255, 255, 255, 235) : Theme::ink3());

    const qreal tx = r.x() + 16;
    const qreal tw = r.width() - 32;

    const QFont titleFont = Theme::font(15, true);
    const QFontMetrics titleMetrics(titleFont);
    p.setFont(titleFont);
    p.setPen(e.available ? Theme::ink() : Theme::ink3());
    p.drawText(QRectF(tx, icon.bottom() + 12, tw, 20), Qt::AlignLeft | Qt::AlignVCenter,
               titleMetrics.elidedText(e.title, Qt::ElideRight, (int)tw));

    const QFont subFont = Theme::font(12);
    p.setFont(subFont);
    p.setPen(e.available ? Theme::ink2() : Theme::ink3());
    const QString sub = e.available ? e.subtitle : QString("not on this card");
    p.drawText(QRectF(tx, icon.bottom() + 34, tw, r.bottom() - icon.bottom() - 42),
               Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, sub);
}

void CardGrid::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    for (int i = 0; i < m_entries.size(); ++i)
        if (i != m_index)
            paintCard(p, m_entries[i], cardRect(i), false);

    /* The selected card last, so its glow is not painted over by a neighbour. */
    if (m_index >= 0 && m_index < m_entries.size())
        paintCard(p, m_entries[m_index], cardRect(m_index), true);
}

/* ── Dock ────────────────────────────────────────────────────────────────── */

Dock::Dock(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(Theme::DockH);
}

void Dock::setPages(const QStringList &names)
{
    m_pages = names;
    update();
}

void Dock::setCurrent(int page)
{
    if (m_current == page)
        return;
    m_current = page;
    update();
}

QVector<QRectF> Dock::slotRects() const
{
    QVector<QRectF> out;
    if (m_pages.isEmpty())
        return out;

    const QFont f = Theme::font(12, true);
    const QFontMetrics fm(f);

    QVector<qreal> slotW;
    qreal total = 0;
    for (const QString &name : m_pages) {
        const qreal sw = qMax(64.0, fm.horizontalAdvance(name) + 30.0);
        slotW.append(sw);
        total += sw;
    }
    const qreal pad = 8.0;
    const qreal barW = total + pad * 2;
    const qreal barH = 40.0;
    const QRectF bar((width() - barW) / 2.0, (height() - barH) / 2.0, barW, barH);

    qreal x = bar.x() + pad;
    for (int i = 0; i < m_pages.size(); ++i) {
        out.append(QRectF(x, bar.y() + 5, slotW[i], barH - 10));
        x += slotW[i];
    }
    return out;
}

void Dock::mousePressEvent(QMouseEvent *event)
{
    const QVector<QRectF> slots = slotRects();
    for (int i = 0; i < slots.size(); ++i) {
        if (!slots[i].contains(QPointF(event->pos())))
            continue;
        emit pageClicked(i);
        event->accept();
        return;
    }
    event->ignore();
}

void Dock::paintEvent(QPaintEvent *)
{
    if (m_pages.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QFont f = Theme::font(12, true);
    p.setFont(f);

    const QVector<QRectF> slots = slotRects();
    if (slots.isEmpty())
        return;

    /* Slots sized to their labels, so the bar is as wide as it needs to be and
     * centred -- which is what makes it read as a dock and not as a tab strip. */
    const qreal pad = 8.0;
    const QRectF bar(slots.first().x() - pad, slots.first().y() - 5,
                     slots.last().right() - slots.first().x() + pad * 2, 40.0);

    Theme::softShadow(p, bar, 16, 6, 30);
    QColor glass = Theme::dock();
    glass.setAlpha(Theme::DockAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush(glass);
    p.drawRoundedRect(bar, 16, 16);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawRoundedRect(bar.adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);

    for (int i = 0; i < m_pages.size() && i < slots.size(); ++i) {
        const QRectF slot = slots[i];
        if (i == m_current) {
            Theme::vgrad(p, slot, Theme::blue(), Theme::blueLow(), 11);
            p.setPen(Theme::ink());
        } else {
            QColor hi = Theme::dockHi();
            hi.setAlpha(90);
            p.setPen(Qt::NoPen);
            p.setBrush(hi);
            p.drawRoundedRect(slot, 11, 11);
            p.setPen(Theme::ink2());
        }
        p.drawText(slot, Qt::AlignCenter, m_pages[i]);
    }
}

/* ── ListPane ────────────────────────────────────────────────────────────── */

ListPane::ListPane(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
}

void ListPane::setRows(const QVector<ListRow> &rows)
{
    m_rows = rows;
    m_pressed = -1;
    m_dragging = false;
    if (m_current >= m_rows.size() || m_current < 0 || !selectable(m_current))
        m_current = nextSelectable(-1, 1);
    m_scroll = 0;
    if (m_current >= 0)
        ensureVisible(m_current);
    clampScroll();
    update();
    emit currentChanged(m_current);
}

void ListPane::updateRow(int index, const ListRow &row)
{
    if (index < 0 || index >= m_rows.size())
        return;
    m_rows[index] = row;
    update();
}

const ListRow *ListPane::currentRow() const
{
    if (m_current < 0 || m_current >= m_rows.size())
        return nullptr;
    return &m_rows[m_current];
}

void ListPane::setRowHeight(int px)
{
    m_rowHeight = qMax(16, px);
    update();
}

void ListPane::setPlaceholder(const QString &text)
{
    m_placeholder = text;
    update();
}

bool ListPane::selectable(int index) const
{
    if (index < 0 || index >= m_rows.size())
        return false;
    const ListRow &r = m_rows[index];
    return r.kind != ListRow::Header && r.enabled;
}

int ListPane::nextSelectable(int from, int delta) const
{
    for (int i = from + delta; i >= 0 && i < m_rows.size(); i += delta)
        if (selectable(i))
            return i;
    return -1;
}

int ListPane::rowHeightFor(const ListRow &r) const
{
    switch (r.kind) {
    case ListRow::Header:
        return m_rowHeight - 4;
    case ListRow::Slider:
        return m_rowHeight + 10;
    default:
        return r.detail.isEmpty() ? m_rowHeight : m_rowHeight + 12;
    }
}

int ListPane::rowTop(int index) const
{
    int y = 0;
    for (int i = 0; i < index && i < m_rows.size(); ++i)
        y += rowHeightFor(m_rows[i]);
    return y;
}

int ListPane::contentHeight() const
{
    int y = 0;
    for (int i = 0; i < m_rows.size(); ++i)
        y += rowHeightFor(m_rows[i]);
    return y;
}

int ListPane::rowAt(const QPoint &p) const
{
    int y = -m_scroll;
    for (int i = 0; i < m_rows.size(); ++i) {
        const int h = rowHeightFor(m_rows[i]);
        if (p.y() >= y && p.y() < y + h)
            return i;
        y += h;
    }
    return -1;
}

void ListPane::clampScroll()
{
    const int maxScroll = qMax(0, contentHeight() - height());
    m_scroll = qBound(0, m_scroll, maxScroll);
}

void ListPane::ensureVisible(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    const int top = rowTop(index);
    const int h = rowHeightFor(m_rows[index]);
    if (top < m_scroll)
        m_scroll = top;
    else if (top + h > m_scroll + height())
        m_scroll = top + h - height();
    clampScroll();
}

void ListPane::setCurrent(int index)
{
    if (!selectable(index))
        return;
    if (m_current == index)
        return;
    m_current = index;
    ensureVisible(index);
    update();
    emit currentChanged(m_current);
}

void ListPane::step(int delta)
{
    if (m_rows.isEmpty())
        return;
    const int next = nextSelectable(m_current, delta > 0 ? 1 : -1);
    if (next < 0)
        return;
    m_current = next;
    ensureVisible(m_current);
    update();
    emit currentChanged(m_current);
}

void ListPane::pageStep(int delta)
{
    const int rows = qMax(1, height() / qMax(1, m_rowHeight));
    for (int i = 0; i < rows; ++i)
        step(delta);
}

bool ListPane::adjust(int delta)
{
    if (m_current < 0 || m_current >= m_rows.size())
        return false;
    ListRow &r = m_rows[m_current];
    if (r.kind == ListRow::Slider) {
        const int was = r.value;
        r.value = qBound(r.minimum, r.value + delta * qMax(1, r.stepSize), r.maximum);
        if (r.value == was)
            return true;
        update();
        emit valueChanged(m_current, r.value);
        return true;
    }
    if (r.kind == ListRow::Toggle) {
        r.on = !r.on;
        update();
        emit valueChanged(m_current, r.on ? 1 : 0);
        return true;
    }
    return false;
}

bool ListPane::press()
{
    if (m_current < 0 || m_current >= m_rows.size())
        return false;
    ListRow &r = m_rows[m_current];
    if (r.kind == ListRow::Toggle) {
        r.on = !r.on;
        update();
        emit valueChanged(m_current, r.on ? 1 : 0);
        return true;
    }
    emit activated(m_current);
    return true;
}

void ListPane::resizeEvent(QResizeEvent *event)
{
    clampScroll();
    QWidget::resizeEvent(event);
}

void ListPane::wheelEvent(QWheelEvent *event)
{
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        event->ignore();
        return;
    }
    m_scroll -= notches * m_rowHeight * 2;
    clampScroll();
    update();
    event->accept();
}

void ListPane::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && m_pressed >= 0 && m_pressed < m_rows.size()) {
        ListRow &r = m_rows[m_pressed];
        const int y = rowTop(m_pressed) - m_scroll;
        const QRectF track(width() * 0.44, y + rowHeightFor(r) * 0.62,
                           width() * 0.44, 6.0);
        const qreal t = qBound(0.0, (event->pos().x() - track.x()) / qMax(1.0, track.width()), 1.0);
        const int was = r.value;
        r.value = r.minimum + qRound(t * (r.maximum - r.minimum));
        if (r.value != was) {
            update();
            emit valueChanged(m_pressed, r.value);
        }
        event->accept();
        return;
    }

    const int i = rowAt(event->pos());
    if (i >= 0 && selectable(i) && i != m_current) {
        m_current = i;
        update();
        emit currentChanged(m_current);
    }
    event->accept();
}

void ListPane::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    const int i = rowAt(event->pos());
    m_pressed = i;
    if (i >= 0 && selectable(i)) {
        if (i != m_current) {
            m_current = i;
            emit currentChanged(m_current);
        }
        /* A press on a slider's half of the row starts a drag; a press on its
         * label does not, so a label can still be read without moving the value. */
        if (m_rows[i].kind == ListRow::Slider && event->pos().x() > width() * 0.42) {
            m_dragging = true;
            mouseMoveEvent(event);
            return;
        }
        update();
    }
    event->accept();
}

void ListPane::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (m_dragging) {
        m_dragging = false;
        m_pressed = -1;
        event->accept();
        return;
    }

    const int i = rowAt(event->pos());
    if (i >= 0 && i == m_pressed && selectable(i)) {
        m_current = i;
        press();
    }
    m_pressed = -1;
    event->accept();
}

void ListPane::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_rows.isEmpty()) {
        if (!m_placeholder.isEmpty()) {
            p.setFont(Theme::font(13));
            p.setPen(Theme::ink3());
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        }
        return;
    }

    const QFont textFont = Theme::font(13);
    const QFont boldFont = Theme::font(13, true);
    const QFont smallFont = Theme::font(11);
    const QFontMetrics textMetrics(textFont);
    const QFontMetrics smallMetrics(smallFont);

    int y = -m_scroll;
    for (int i = 0; i < m_rows.size(); ++i) {
        const ListRow &r = m_rows[i];
        const int h = rowHeightFor(r);
        if (y + h < 0) {
            y += h;
            continue;
        }
        if (y > height())
            break;

        const QRectF row(4, y, width() - 8, h);

        if (r.kind == ListRow::Header) {
            p.setFont(smallFont);
            p.setPen(Theme::ink3());
            p.drawText(row.adjusted(8, 0, 0, 0), Qt::AlignLeft | Qt::AlignBottom,
                       r.text.toUpper());
            p.setPen(QPen(Theme::separator(), 1.0));
            p.drawLine(QPointF(row.x() + 8, row.bottom() - 1.5),
                       QPointF(row.right() - 8, row.bottom() - 1.5));
            y += h;
            continue;
        }

        const bool selected = (i == m_current);
        if (selected) {
            QColor sel = Theme::blue();
            sel.setAlpha(r.enabled ? 210 : 90);
            p.setPen(Qt::NoPen);
            p.setBrush(sel);
            p.drawRoundedRect(row.adjusted(2, 1, -2, -1), 8, 8);
        }

        qreal x = row.x() + 10;

        if (r.glyph >= 0) {
            const QRectF icon(x, row.center().y() - 11, 22, 22);
            paintGlyph(p, icon, r.glyph,
                       selected ? Theme::ink() : (r.accent.isValid() ? r.accent : Theme::ink2()));
            x += 30;
        }

        /* The right-hand furniture first, so the label knows how much room it has. */
        qreal right = row.right() - 12;

        if (r.kind == ListRow::Toggle) {
            const QRectF sw(right - 40, row.center().y() - 10, 40, 20);
            p.setPen(Qt::NoPen);
            p.setBrush(r.on ? Theme::green() : Theme::separator());
            p.drawRoundedRect(sw, 10, 10);
            p.setBrush(QColor(250, 251, 255));
            p.drawEllipse(QPointF(r.on ? sw.right() - 10 : sw.x() + 10, sw.center().y()), 8, 8);
            right = sw.x() - 10;
        } else if (r.kind == ListRow::Slider) {
            const QString vt = r.valueText.isEmpty() ? QString::number(r.value) : r.valueText;
            const qreal vw = qMax(38.0, (qreal)smallMetrics.horizontalAdvance(vt) + 6.0);
            p.setFont(smallFont);
            p.setPen(selected ? Theme::ink() : Theme::ink2());
            p.drawText(QRectF(right - vw, row.y(), vw, h * 0.6),
                       Qt::AlignRight | Qt::AlignVCenter, vt);

            const QRectF track(width() * 0.44, row.y() + h * 0.62, width() * 0.44, 6.0);
            p.setPen(Qt::NoPen);
            p.setBrush(selected ? QColor(255, 255, 255, 70) : Theme::separator());
            p.drawRoundedRect(track, 3, 3);
            const qreal span = qMax(1, r.maximum - r.minimum);
            const qreal t = qBound(0.0, (r.value - r.minimum) / span, 1.0);
            QRectF fill = track;
            fill.setWidth(track.width() * t);
            p.setBrush(selected ? Theme::ink() : Theme::blue());
            p.drawRoundedRect(fill, 3, 3);
            p.setBrush(QColor(250, 251, 255));
            p.drawEllipse(QPointF(track.x() + track.width() * t, track.center().y()), 6.5, 6.5);
            right = track.x() - 10;
        } else if (!r.badge.isEmpty()) {
            p.setFont(smallFont);
            const qreal bw = smallMetrics.horizontalAdvance(r.badge) + 14;
            const QRectF pill(right - bw, row.center().y() - 9, bw, 18);
            QColor bc = r.badgeColour.isValid() ? r.badgeColour : Theme::separator();
            if (selected)
                bc = bc.lighter(120);
            p.setPen(Qt::NoPen);
            p.setBrush(bc);
            p.drawRoundedRect(pill, 9, 9);
            p.setPen(Theme::ink());
            p.drawText(pill, Qt::AlignCenter, r.badge);
            right = pill.x() - 10;
        }

        const qreal tw = qMax(20.0, right - x);
        const bool twoLine = (r.kind != ListRow::Slider) && !r.detail.isEmpty();

        p.setFont(selected ? boldFont : textFont);
        p.setPen(r.enabled ? (selected ? Theme::ink() : Theme::ink())
                           : Theme::ink3());
        const QFontMetrics fm(selected ? boldFont : textFont);
        p.drawText(QRectF(x, twoLine ? row.y() + 5 : row.y(), tw, twoLine ? 18 : h),
                   Qt::AlignLeft | (twoLine ? Qt::AlignTop : Qt::AlignVCenter),
                   fm.elidedText(r.text, Qt::ElideRight, (int)tw));

        if (twoLine) {
            p.setFont(smallFont);
            p.setPen(selected ? QColor(230, 238, 255) : Theme::ink2());
            p.drawText(QRectF(x, row.y() + 22, tw, 16), Qt::AlignLeft | Qt::AlignTop,
                       smallMetrics.elidedText(r.detail, Qt::ElideRight, (int)tw));
        } else if (r.kind == ListRow::Slider && !r.detail.isEmpty()) {
            p.setFont(smallFont);
            p.setPen(selected ? QColor(230, 238, 255) : Theme::ink3());
            p.drawText(QRectF(x, row.y() + h * 0.52, tw, 14), Qt::AlignLeft | Qt::AlignTop,
                       smallMetrics.elidedText(r.detail, Qt::ElideRight, (int)tw));
        }

        y += h;
    }

    /* A scrollbar, only when there is something to scroll. */
    const int total = contentHeight();
    if (total > height()) {
        const qreal t = (qreal)height() / total;
        const qreal barH = qMax(24.0, height() * t);
        const qreal barY = (height() - barH) * m_scroll / qMax(1, total - height());
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::dockHi());
        p.drawRoundedRect(QRectF(width() - 5, barY, 3, barH), 1.5, 1.5);
    }
}

/* ── InfoPage ────────────────────────────────────────────────────────────── */

InfoPage::InfoPage(QWidget *parent)
    : PageWidget(parent)
{
    QTimer *t = new QTimer(this);
    t->setInterval(2000);
    connect(t, &QTimer::timeout, this, &InfoPage::refresh);
    t->start();
}

void InfoPage::setInputSummary(const QString &summary)
{
    m_inputs = summary;
    refresh();
}

void InfoPage::showEvent(QShowEvent *event)
{
    refresh();
    QWidget::showEvent(event);
}

bool InfoPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        m_scroll = qMax(0, m_scroll - 1);
        update();
        return true;
    case Joypad::NavDown:
        m_scroll = qMin(qMax(0, m_rows.size() - 4), m_scroll + 1);
        update();
        return true;
    default:
        return false;
    }
}

void InfoPage::wheelEvent(QWheelEvent *event)
{
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        event->ignore();
        return;
    }
    m_scroll = qBound(0, m_scroll - notches, qMax(0, m_rows.size() - 4));
    update();
    event->accept();
}

void InfoPage::refresh()
{
    if (!isVisible())
        return;

    m_rows.clear();

    /*
     * The framebuffer, asked rather than assumed.  This is the one row worth the
     * whole page: it is the geometry the panel is running at, straight out of the
     * driver that is scanning it out, so a wrong stride or a 16-bit surprise shows
     * up here instead of as a black screen.
     */
    QString fbLine = "no /dev/fb0";
    const int fb = ::open("/dev/fb0", O_RDONLY);
    if (fb >= 0) {
        struct fb_var_screeninfo v;
        struct fb_fix_screeninfo fx;
        if (::ioctl(fb, FBIOGET_VSCREENINFO, &v) == 0 && ::ioctl(fb, FBIOGET_FSCREENINFO, &fx) == 0) {
            fbLine = QString("%1x%2, %3 bpp, stride %4, r%5@%6 g%7@%8 b%9@%10")
                         .arg(v.xres).arg(v.yres).arg(v.bits_per_pixel).arg(fx.line_length)
                         .arg(v.red.length).arg(v.red.offset)
                         .arg(v.green.length).arg(v.green.offset)
                         .arg(v.blue.length).arg(v.blue.offset);
        } else {
            fbLine = "/dev/fb0 opened, but it answered no geometry";
        }
        ::close(fb);
    }
    m_rows.append(qMakePair(QString("Panel"), fbLine));
    m_rows.append(qMakePair(QString("Painted by"),
                            QString("Qt %1, %2").arg(QT_VERSION_STR, QGuiApplication::platformName())));

    m_rows.append(qMakePair(QString("Kernel"), firstWords(readTrimmed("/proc/version"), 3)));

    const QString up = readTrimmed("/proc/uptime").section(' ', 0, 0);
    if (!up.isEmpty()) {
        const int secs = (int)up.toDouble();
        m_rows.append(qMakePair(QString("Uptime"),
                                QString("%1m %2s").arg(secs / 60).arg(secs % 60)));
    }

    /* Memory, from the two lines that matter. */
    QFile mem("/proc/meminfo");
    if (mem.open(QIODevice::ReadOnly | QIODevice::Text)) {
        long total = 0;
        long avail = 0;
        while (!mem.atEnd()) {
            const QString line = QString::fromLatin1(mem.readLine());
            if (line.startsWith("MemTotal:"))
                total = line.section(':', 1).trimmed().section(' ', 0, 0).toLong();
            else if (line.startsWith("MemAvailable:"))
                avail = line.section(':', 1).trimmed().section(' ', 0, 0).toLong();
        }
        if (total > 0)
            m_rows.append(qMakePair(QString("Memory"),
                                    QString("%1 MB free of %2 MB").arg(avail / 1024).arg(total / 1024)));
    }

    bool charging = false;
    const int cap = SysInfo::batteryCapacity(&charging);
    m_rows.append(qMakePair(QString("Cell"),
                            cap < 0 ? QString("no power_supply class -- see Diagnostics")
                                    : QString::number(cap) + "%"
                                          + QString(charging ? ", charging" : "")));

    m_rows.append(qMakePair(QString("Sound"),
                            QFileInfo::exists("/dev/snd/controlC0")
                                ? QString("controlC0")
                                : QString("no /dev/snd/controlC0 -- add j36.audio=1")));

    /*
     * The DRI nodes, classified rather than counted.  eglprobe answered
     * "GETRESOURCES: Operation not supported" on card0, and that error means one
     * thing exactly: the driver behind that node registered without DRIVER_MODESET.
     * lima is such a driver -- it is a GPU and has no scanout of its own -- so on
     * this SoC the node that rasterises and the node that drives the panel are not
     * the same node, and which is card0 is a probe-order accident.  Assuming card0
     * was the display cost this bring-up a week, so the answer belongs on the glass:
     * a node with a connector under it modesets, a node without one cannot.
     *
     * All of it from sysfs, so nothing here opens a DRM device, takes master or
     * risks a modeset from inside the dashboard.
     */
    const QDir cls("/sys/class/drm");
    const QStringList nodes =
        cls.entryList(QStringList() << "card[0-9]", QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (nodes.isEmpty()) {
        m_rows.append(qMakePair(QString("DRM"),
                                QString("nothing in /sys/class/drm -- the dashboard does not need it")));
    }
    for (int i = 0; i < nodes.size(); ++i) {
        const QString node = nodes.at(i);
        const QString driver =
            QFileInfo("/sys/class/drm/" + node + "/device/driver").symLinkTarget().section('/', -1);

        QStringList conns;
        for (const QString &c : cls.entryList(QStringList() << node + "-*",
                                              QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString status = readTrimmed("/sys/class/drm/" + c + "/status");
            conns << c.section('-', 1) + (status.isEmpty() ? QString() : " " + status);
        }

        m_rows.append(qMakePair(i == 0 ? QString("DRM") : QString(),
                                QString("%1 %2 -- %3")
                                    .arg(node, driver.isEmpty() ? QString("?") : driver,
                                         conns.isEmpty()
                                             ? QString("render only, no connector, no modesetting")
                                             : conns.join(", "))));
    }

    if (!m_inputs.isEmpty())
        m_rows.append(qMakePair(QString("Input"), m_inputs));

    /* Only our own words: the rest of the command line is long and known. */
    QStringList words;
    for (const QString &w : readTrimmed("/proc/cmdline").split(' ', Qt::SkipEmptyParts))
        if (w.startsWith("j36."))
            words << w;
    m_rows.append(qMakePair(QString("Boot words"),
                            words.isEmpty() ? QString("none") : words.join(' ')));

    if (m_scroll > qMax(0, m_rows.size() - 4))
        m_scroll = qMax(0, m_rows.size() - 4);

    update();
}

void InfoPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, QStringLiteral("System"));

    const QFont labelFont = Theme::font(12, true);
    const QFont valueFont = Theme::font(12);
    const QFontMetrics valueMetrics(valueFont);
    const qreal labelW = 96.0;
    const qreal rowH = 21.0;
    qreal y = body.y() + 8;

    for (int i = m_scroll; i < m_rows.size(); ++i) {
        if (y + rowH > body.bottom() - 6)
            break;
        p.setFont(labelFont);
        p.setPen(Theme::ink3());
        p.drawText(QRectF(card.x() + 14, y, labelW, rowH),
                   Qt::AlignRight | Qt::AlignVCenter, m_rows[i].first);
        p.setFont(valueFont);
        p.setPen(Theme::ink());
        const qreal vx = card.x() + 14 + labelW + 12;
        const qreal vw = card.right() - 14 - vx;
        p.drawText(QRectF(vx, y, vw, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                   valueMetrics.elidedText(m_rows[i].second, Qt::ElideRight, (int)vw));
        y += rowH;
    }
}
