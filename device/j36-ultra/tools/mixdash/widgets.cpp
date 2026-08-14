/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "widgets.h"
#include "joypad.h"
#include "theme.h"

/*
 * The same generated header main.cpp picks up, and picked up the same way: the
 * information sheet names the build it is, and a tree built by hand with a plain
 * qmake has no buildid.h and simply says "unknown" instead of failing to compile.
 */
#if defined(__has_include)
#  if __has_include("buildid.h")
#    include "buildid.h"
#  endif
#endif
#ifndef MIXDASH_BUILD_ID
#define MIXDASH_BUILD_ID "unknown"
#endif

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/fb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
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
 * EVERY LINE OF A FILE, AND WHY IT IS THIS AND NOT A readLine() LOOP.
 *
 * Seven places in this dashboard walked a /proc file like this:
 *
 *     while (!f.atEnd()) { const QString line = f.readLine(); ... }
 *
 * and every one of them read nothing whatsoever.  QFileDevice::atEnd() answers
 * from the file's SIZE, and Qt says so in as many words: "For regular empty
 * files on Unix (e.g. those in /proc), this function returns true, since the
 * file system reports that the size of such a file is 0."  procfs generates its
 * contents when they are read, so it reports every file as zero bytes:
 *
 *     stat -c %s /proc/meminfo   ->  0
 *     wc -c    < /proc/meminfo   ->  1531
 *
 * atEnd() is therefore true before the first read and the loop body never runs
 * once.  That is one bug rather than seven, and it is the whole reason the
 * System page called the chip "unknown", counted no cores, showed no RAM row at
 * all, listed no mounted volumes and no sound card, and why the status bar's
 * Wi-Fi meter never had a signal to draw.
 *
 * readAll() reads until read(2) returns 0, which is the only thing these files
 * answer to, so everything that wants lines comes through here.  Empty lines are
 * dropped: /proc/cpuinfo separates processors with them and no caller here uses
 * them as a delimiter.  Local 8-bit and not Latin-1 because one of the callers
 * is /proc/self/mounts, where a mount point can be a UTF-8 volume label.
 */
QStringList readLines(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringList();
    return QString::fromLocal8Bit(f.readAll()).split('\n', Qt::SkipEmptyParts);
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

using SysInfo::readLines;
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
    const QStringList lines = readLines("/proc/net/wireless");
    for (const QString &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QStringList cols = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        if (cols.size() < 2)
            continue;
        /* The kernel prints this column as "70." -- the trailing dot marks it as
         * an updated value, and it has to come off before the number parses.  A
         * copy, because at() hands back a const reference and remove() writes. */
        QString link = cols.at(1);
        link.remove('.');
        bool ok = false;
        const double value = link.toDouble(&ok);
        if (!ok || value <= 0.0)
            continue;
        return qBound(1, (int)(value * 100.0 / 70.0), 100);
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

void CardGrid::setIndex(int index)
{
    if (m_entries.isEmpty())
        return;
    const int clamped = qBound(0, index, m_entries.size() - 1);
    if (clamped == m_index)
        return;
    selectTo(clamped);
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

    /*
     * THE SHOULDERS ARE NOT HANDLED HERE, AND THAT IS THE POINT.
     *
     * L1/L2 and R1/R2 arrive as NavPrevPage and NavNextPage.  For one release
     * this page consumed them to step the SELECTION card by card, on the theory
     * that a hand already on the shoulder should not have to move back to the
     * D-pad.  It is the wrong theory: the selection is what the D-pad is for, so
     * the shoulders were a second, slower way to do the one thing the pad already
     * does well, and the thing nothing else could do -- change which of the four
     * root pages is on the glass -- needed you to walk to the end of a grid first.
     *
     * So they fall through, every time, to Dashboard::onNav, whose setRoot(-1)
     * and setRoot(+1) move the page left and right and wrap at both ends.  Both
     * shoulders on a side do the same thing on purpose: L1 and L2 are one gesture
     * to a thumb, and so are R1 and R2.
     *
     * Returning false rather than deleting the case labels would have been the
     * same behaviour; there are no case labels at all so that the next person
     * reading this switch does not see the shoulders listed and assume the grid
     * has an opinion about them.
     */

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
    /*
     * Capped, so a one-row page does not become three enormous slabs, and the
     * block is then centred in what is left.  150 rather than the 190 this used
     * to be: the cards carry a glyph and a name now and nothing else, and a card
     * tall enough for two lines of description with no description in it is a
     * card that reads as if something failed to load.
     */
    const qreal ch = qMin(availH / rows, 150.0);
    const qreal used = ch * rows + Theme::Gap * (rows - 1);
    const qreal top = Theme::Margin + (height() - 2.0 * Theme::Margin - used) / 2.0;

    const int c = i % cols;
    const int r = i / cols;
    return QRectF(Theme::Margin + c * (cw + Theme::Gap), top + r * (ch + Theme::Gap), cw, ch);
}

/*
 * Eight pixels of margin, and the number is derived rather than picked.  A card
 * draws OUTSIDE cardRect() twice: Theme::softShadow strokes out to `spread' (6)
 * px, one further at the bottom, and a selected card adds an outline at
 * r.adjusted(-2.5, -2.5, 2.5, 2.5).  Eight covers the larger of those with a
 * pixel over for antialiasing, and toAlignedRect() rounds outwards so a card on
 * a half-pixel boundary does not leave a seam.
 */
QRect CardGrid::dirtyRect(int i) const
{
    return cardRect(i).adjusted(-8, -8, 8, 8).toAlignedRect();
}

/*
 * WHY MOVING THE SELECTION MARKS TWO RECTANGLES AND NOT THE WHOLE PAGE.
 *
 * Hover moves the selection and the stick moves the cursor sixty times a second,
 * so on a page being pointed at this runs constantly.  Every caller used to end
 * in the argument-less update(), which marks the entire grid dirty, and
 * paintEvent then redrew all nine cards -- nine soft shadows of six rounded
 * strokes each, nine rounded-rect gradients, nine glyphs, nine elided titles --
 * in order to change the outline on two of them.  Qt's raster engine does that
 * in software on a Cortex-A7 with no 2D engine it can reach, on the same thread
 * that is trying to move the cursor, which is exactly the "pointer gets stuck
 * for a small instant when hovering any menu item" this replaces.
 *
 * Only two cards change: the one losing the selection and the one gaining it.
 * Marking those two, and having paintEvent skip cards the marked region does not
 * touch, leaves the rest of the page in the backing store where it already is.
 *
 * The pixels stay correct because Qt repaints a dirty region from the top level
 * down: Dashboard::paintEvent redraws the desk under the marked rectangles
 * before this page draws anything into them, so the cards are not composited on
 * top of their own previous alpha.
 */
void CardGrid::selectTo(int next)
{
    const int prev = m_index;
    m_index = next;
    if (prev >= 0 && prev < m_entries.size())
        update(dirtyRect(prev));
    if (next >= 0 && next < m_entries.size())
        update(dirtyRect(next));
    emit indexChanged(m_index);
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
    if (i >= 0 && i != m_index)
        selectTo(i);
    event->accept();
}

void CardGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_pressed = cardAt(event->pos());
    if (m_pressed >= 0 && m_pressed != m_index)
        selectTo(m_pressed);
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

    selectTo(candidate);
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

    /*
     * Glyph over name, both centred, and the pair centred in the card as one
     * block rather than pinned to the top corner.  The old layout put the icon at
     * a fixed (16, 16) because the description hanging below it was what filled
     * the rest; with the description gone, a top-left icon leaves a hole where
     * the words used to be.  Measuring the block and centring it means the card
     * looks deliberate at whatever height cardRect() lands on.
     */
    const qreal side = qMin(56.0, qMin(r.width() * 0.42, r.height() * 0.44));
    const qreal titleH = 22.0;
    const qreal blockH = side + 12.0 + titleH;
    const qreal blockTop = r.y() + (r.height() - blockH) / 2.0;

    const QRectF icon(r.center().x() - side / 2.0, blockTop, side, side);
    const QColor accent = e.available ? e.accent : Theme::separator();
    Theme::vgrad(p, icon, accent.lighter(112), accent.darker(135), 12);
    /* The inset is a fraction of the block, not the 11 px it used to be, so the
     * glyph keeps its proportions when `side' is clamped on a narrow card. */
    const qreal inset = side * 0.24;
    paintGlyph(p, icon.adjusted(inset, inset, -inset, -inset), e.glyph,
               e.available ? QColor(255, 255, 255, 235) : Theme::ink3());

    const qreal tx = r.x() + 10;
    const qreal tw = r.width() - 20;

    const QFont titleFont = Theme::font(15, true);
    const QFontMetrics titleMetrics(titleFont);
    p.setFont(titleFont);
    p.setPen(e.available ? Theme::ink() : Theme::ink3());
    p.drawText(QRectF(tx, icon.bottom() + 12, tw, titleH), Qt::AlignCenter,
               titleMetrics.elidedText(e.title, Qt::ElideRight, (int)tw));
}

void CardGrid::paintEvent(QPaintEvent *event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /*
     * The other half of selectTo()'s story.  Marking two rectangles saves nothing
     * on its own: QPainter would still be asked to draw all nine cards and would
     * still build every gradient and lay out every title before the clip rejected
     * the pixels.  Testing here is what makes the saving real.
     *
     * region() and not rect(): rect() is the bounding box of the dirty region, so
     * a selection moving from the first card to the last would report the whole
     * page and this test would pass for everything.
     *
     * The neighbours of a changed card usually intersect it -- the gap is 12 px
     * and dirtyRect() grows each side by 8 -- and repainting them is not waste but
     * correctness: their shadows are drawn into that same overlap.
     */
    const QRegion dirty = event->region();

    for (int i = 0; i < m_entries.size(); ++i)
        if (i != m_index && dirty.intersects(dirtyRect(i)))
            paintCard(p, m_entries[i], cardRect(i), false);

    /* The selected card last, so its glow is not painted over by a neighbour. */
    if (m_index >= 0 && m_index < m_entries.size() && dirty.intersects(dirtyRect(m_index)))
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
    /* `slots' is a Qt keyword -- qobjectdefs.h defines it away to nothing -- so it
     * cannot be a variable name in a translation unit that has not asked for
     * QT_NO_KEYWORDS, and this one has not. */
    const QVector<QRectF> boxes = slotRects();
    for (int i = 0; i < boxes.size(); ++i) {
        if (!boxes[i].contains(QPointF(event->pos())))
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

    const QVector<QRectF> boxes = slotRects();
    if (boxes.isEmpty())
        return;

    /* Slots sized to their labels, so the bar is as wide as it needs to be and
     * centred -- which is what makes it read as a dock and not as a tab strip. */
    const qreal pad = 8.0;
    const QRectF bar(boxes.first().x() - pad, boxes.first().y() - 5,
                     boxes.last().right() - boxes.first().x() + pad * 2, 40.0);

    Theme::softShadow(p, bar, 16, 6, 30);
    QColor glass = Theme::dock();
    glass.setAlpha(Theme::DockAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush(glass);
    p.drawRoundedRect(bar, 16, 16);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawRoundedRect(bar.adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);

    for (int i = 0; i < m_pages.size() && i < boxes.size(); ++i) {
        const QRectF slot = boxes[i];
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

namespace {

/*
 * Row geometry, in one place because three functions have to agree on it: the
 * one that draws, the one that decides how far down the list you can scroll and
 * the one that pages.  Headers are taller than they need to be on purpose --
 * the extra six pixels are drawn as air above the text, which is what separates
 * one section from the tail of the one before it.
 */
const qreal kRowH = 20.0;
const qreal kHeadH = 26.0;
const qreal kPadTop = 7.0;
const qreal kPadBottom = 6.0;

/* Bytes the way a disk is talked about, and the same shape MediaPage prints for
 * files, so the same stick reads the same size on every page of this dashboard. */
QString humanBytes(qulonglong bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 1) + " GB";
    if (bytes >= 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
    if (bytes >= 1024)
        return QString::number(bytes / 1024) + " kB";
    return QString::number(bytes) + " B";
}

/* /proc/device-tree properties are NUL-terminated strings; the NUL survives
 * readTrimmed, and a QString with a NUL in it draws as a box. */
QString readDT(const QString &path)
{
    QString s = readTrimmed(path);
    const int nul = s.indexOf(QChar('\0'));
    if (nul >= 0)
        s.truncate(nul);
    return s.trimmed();
}

/*
 * The mount table escapes space, tab, newline and backslash as octal, so a stick
 * labelled "My Disk" arrives as /media/My\040Disk.  Undoing it here is what makes
 * the automounted volumes on this page match what the Files page shows.
 */
QString unescapeMount(const QString &s)
{
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('\\') && i + 3 < s.size()) {
            bool ok = false;
            const int c = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok && c > 0) {
                out.append(QChar(c));
                i += 3;
                continue;
            }
        }
        out.append(s.at(i));
    }
    return out;
}

/*
 * The CPU, from the only file that knows.  On ARM there is no "model name" line
 * -- the kernel prints the implementer and the part number and leaves the naming
 * to userspace, which is why every ARM board says "Processor: ARMv7 rev 3" in
 * tools that stopped reading at the first line.  The J36 is 0x41/0xc07, and
 * saying "Cortex-A7" out loud is worth the ten-entry table.
 */
QString armPartName(const QString &implementer, const QString &part)
{
    if (implementer.toLower() != QLatin1String("0x41")) /* not ARM Ltd */
        return QString();

    static const struct { const char *id; const char *name; } parts[] = {
        { "0xc05", "Cortex-A5" },  { "0xc07", "Cortex-A7" },
        { "0xc08", "Cortex-A8" },  { "0xc09", "Cortex-A9" },
        { "0xc0d", "Cortex-A12" }, { "0xc0e", "Cortex-A17" },
        { "0xc0f", "Cortex-A15" }, { "0xd03", "Cortex-A53" },
        { "0xd04", "Cortex-A35" }, { "0xd08", "Cortex-A72" },
    };
    for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; ++i)
        if (part.toLower() == QLatin1String(parts[i].id))
            return QString::fromLatin1(parts[i].name);
    return QString();
}

struct CpuInfo {
    QString model;    /* "Cortex-A7 (ARMv7, rev 3)" or whatever could be had */
    QString hardware; /* the board line, when the kernel still prints one */
    int cores;
};

/*
 * How many CPUs a cpulist names.  sysfs writes these as "0-7", or "0-3,6,7" once
 * something has been hotplugged out of the middle, and putting that string
 * straight on the glass is how the System page came to say "0-7 present" -- which
 * a reader takes as a seven, not as eight of them.  0 for anything unparseable,
 * including the empty string a kernel without CONFIG_HOTPLUG_CPU gives.
 */
int cpuListCount(const QString &list)
{
    int n = 0;
    const QStringList ranges = list.split(',', Qt::SkipEmptyParts);
    for (const QString &range : ranges) {
        const QStringList ends = range.split('-');
        bool ok = false;
        const int first = ends.value(0).toInt(&ok);
        if (!ok)
            continue;
        int last = first;
        if (ends.size() > 1) {
            last = ends.value(1).toInt(&ok);
            if (!ok)
                continue;
        }
        if (last < first)
            continue;
        n += last - first + 1;
    }
    return n;
}

CpuInfo cpuInfo()
{
    CpuInfo info;
    info.cores = 0;

    QString name, impl, part, arch, rev;
    const QStringList lines = readLines("/proc/cpuinfo");
    for (const QString &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed();
        if (key == QLatin1String("processor"))
            ++info.cores;
        else if (key == QLatin1String("model name"))
            name = val;
        else if (key == QLatin1String("Hardware"))
            info.hardware = val;
        else if (key == QLatin1String("CPU implementer"))
            impl = val;
        else if (key == QLatin1String("CPU part"))
            part = val;
        else if (key == QLatin1String("CPU architecture"))
            arch = val;
        else if (key == QLatin1String("CPU revision"))
            rev = val;
    }

    const QString decoded = armPartName(impl, part);
    QStringList bits;
    if (!decoded.isEmpty())
        bits << decoded;
    else if (!name.isEmpty())
        bits << name.section('(', 0, 0).trimmed();
    else if (!part.isEmpty())
        bits << "part " + part;

    QStringList paren;
    if (!arch.isEmpty())
        paren << "ARMv" + arch;
    if (!rev.isEmpty())
        paren << "rev " + rev;
    if (!paren.isEmpty())
        bits << "(" + paren.join(", ") + ")";

    info.model = bits.join(" ");
    return info;
}

/*
 * Whole disks only.  A partition has a `partition' file in its sysfs directory
 * and a disk does not, which is the distinction sysfs actually makes; matching
 * on the name instead gets it wrong for both mmcblk0p1 and for a disk called
 * sda1 that does not exist.  ram, loop and zram are skipped because they are
 * kernel bookkeeping and this section is about hardware you can hold.
 */
QStringList blockDisks()
{
    QStringList out;
    const QDir dir("/sys/class/block");
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (name.startsWith("ram") || name.startsWith("loop") || name.startsWith("zram"))
            continue;
        const QString base = "/sys/class/block/" + name;
        if (QFileInfo::exists(base + "/partition"))
            continue;

        const qulonglong sectors = readTrimmed(base + "/size").toULongLong();
        if (sectors == 0)
            continue;

        QStringList bits;
        /* The kernel reports capacity in 512-byte sectors whatever the device's
         * own block size is; that is the one unit sysfs never varies. */
        bits << name << humanBytes(sectors * 512ULL);
        bits << (readTrimmed(base + "/removable") == QLatin1String("1")
                     ? QObject::tr("removable")
                     : QObject::tr("fixed"));

        /* USB disks answer through SCSI and have vendor/model; the eMMC and the
         * SD card answer through the MMC layer and have name/type instead. */
        QString id = (readTrimmed(base + "/device/vendor") + " "
                      + readTrimmed(base + "/device/model")).simplified();
        if (id.isEmpty())
            id = readTrimmed(base + "/device/name");
        if (!id.isEmpty())
            bits << id;

        const int parts = QDir(base).entryList(QStringList() << name + "*",
                                               QDir::Dirs | QDir::NoDotAndDotDot).size();
        if (parts > 0)
            /* Spelled out rather than tr("%n partition(s)", "", parts): the
             * translator behind these strings is a table, not a .qm, so it has
             * no numerus forms and %n would reach the glass unsubstituted. */
            bits << (parts == 1 ? QObject::tr("1 partition")
                                : QObject::tr("%1 partitions").arg(parts));

        out << bits.join("  ");
    }
    return out;
}

/*
 * Everything mounted off a block device, with how full it is.  Sources that do
 * not start with /dev/ are skipped -- proc, sysfs, devtmpfs, the dozen tmpfs
 * mounts systemd makes -- because none of them is a disk and all of them would
 * bury the two lines that are.  This is where the automounted /media volumes
 * show up, and showing them is half the reason this section exists.
 */
QStringList mountedVolumes()
{
    QStringList out;
    const QStringList lines = readLines("/proc/self/mounts");
    for (const QString &line : lines) {
        const QStringList c = line.split(' ', Qt::SkipEmptyParts);
        if (c.size() < 4 || !c.at(0).startsWith("/dev/"))
            continue;

        const QString dev = unescapeMount(c.at(0));
        const QString mnt = unescapeMount(c.at(1));

        QStringList bits;
        bits << mnt << dev.section('/', -1) << c.at(2);

        /*
         * statvfs and not the size in sysfs: this is the filesystem's own idea of
         * how much room is left, which is the number anyone asking has in mind.
         *
         * Guarded on the device node still existing, and the guard is not
         * theoretical: a stick pulled without ejecting loses /dev/sda1 the
         * instant the kernel notices, while the mount lingers the second or two
         * systemd needs to run the automount unit's ExecStop.  statvfs on a
         * mount whose disk has left blocks on I/O, and this runs on the thread
         * that draws, so blocking here is a frozen dashboard.
         */
        struct statvfs vs;
        if (QFileInfo::exists(dev)
            && ::statvfs(QFile::encodeName(mnt).constData(), &vs) == 0 && vs.f_blocks > 0) {
            const qulonglong unit = vs.f_frsize ? vs.f_frsize : vs.f_bsize;
            bits << QObject::tr("%1 free of %2")
                        .arg(humanBytes((qulonglong)vs.f_bavail * unit),
                             humanBytes((qulonglong)vs.f_blocks * unit));
        }
        /* The first option is always rw or ro.  Read-only is worth saying:
         * on an NTFS volume it means the disk was unplugged from Windows
         * without ejecting it, and ntfs3 refused to touch a dirty journal. */
        if (c.at(3) == QLatin1String("ro") || c.at(3).startsWith("ro,"))
            bits << QObject::tr("read-only");

        out << bits.join("  ");
    }
    return out;
}

/*
 * The USB bus as the kernel has it.  Directories with a colon in the name are
 * interfaces of a device already listed, so they are skipped; what is left is
 * one line per physical thing plugged in, plus the root hub, which is named
 * rather than hidden because "usb1  root hub" is how you tell an empty bus from
 * a bus that never came up at all.
 */
QStringList usbDevices()
{
    QStringList out;
    const QDir dir("/sys/bus/usb/devices");
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (name.contains(':'))
            continue;
        const QString base = "/sys/bus/usb/devices/" + name;
        const QString vid = readTrimmed(base + "/idVendor");
        if (vid.isEmpty())
            continue;

        QString what = (readTrimmed(base + "/manufacturer") + " "
                        + readTrimmed(base + "/product")).simplified();
        if (what.isEmpty())
            what = "class " + readTrimmed(base + "/bDeviceClass");

        QStringList bits;
        bits << name << what << vid + ":" + readTrimmed(base + "/idProduct");
        const QString speed = readTrimmed(base + "/speed");
        if (!speed.isEmpty())
            bits << speed + " Mbit/s";
        if (name.startsWith("usb"))
            bits << QObject::tr("root hub");

        out << bits.join("  ");
    }
    return out;
}

/* Every evdev node with the name its driver gave it -- the D-pad, the sticks,
 * the volume keys, and whatever was plugged into the USB port. */
QStringList inputDevices()
{
    QStringList out;
    const QDir dir("/sys/class/input");
    const QStringList nodes = dir.entryList(QStringList() << "event*",
                                            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &node : nodes) {
        const QString name = readTrimmed("/sys/class/input/" + node + "/device/name");
        out << node + "  " + (name.isEmpty() ? QObject::tr("unnamed") : name);
    }
    return out;
}

/*
 * The interfaces, with their addresses.  getifaddrs and not a file, because the
 * IPv4 address of an interface is the one thing about it that sysfs does not
 * carry -- /proc/net/fib_trie has it, in a shape nobody should have to parse.
 */
QStringList netInterfaces()
{
    QMap<QString, QString> addrs;
    struct ifaddrs *list = nullptr;
    if (::getifaddrs(&list) == 0) {
        for (struct ifaddrs *p = list; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_name)
                continue;
            char buf[INET_ADDRSTRLEN];
            const struct sockaddr_in *in = (const struct sockaddr_in *)p->ifa_addr;
            if (::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf))
                addrs.insert(QString::fromLatin1(p->ifa_name), QString::fromLatin1(buf));
        }
        ::freeifaddrs(list);
    }

    QStringList out;
    const QDir dir("/sys/class/net");
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (name == QLatin1String("lo"))
            continue;
        const QString base = "/sys/class/net/" + name;

        QStringList bits;
        bits << name << readTrimmed(base + "/operstate");
        if (addrs.contains(name))
            bits << addrs.value(name);
        const QString mac = readTrimmed(base + "/address");
        if (!mac.isEmpty() && mac != QLatin1String("00:00:00:00:00:00"))
            bits << mac;
        if (QFileInfo::exists(base + "/phy80211") || QFileInfo::exists(base + "/wireless"))
            bits << "wireless";

        out << bits.join("  ");
    }
    return out;
}

/*
 * The power_supply class, listed rather than reduced to a percentage.  Until the
 * MT6592 PMIC driver lands this is empty, and empty is the answer: it is the one
 * place on this machine that says out loud why the status bar draws a dash where
 * the battery should be.  When the driver does land, the charger appears here as
 * a second supply and its `online' flips as the cable goes in and out.
 */
QStringList powerSupplies()
{
    QStringList out;
    const QString root = "/sys/class/power_supply";
    const QStringList names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QString base = root + "/" + name;

        QStringList bits;
        bits << name;
        const QString type = readTrimmed(base + "/type");
        if (!type.isEmpty())
            bits << type.toLower();
        const QString online = readTrimmed(base + "/online");
        if (!online.isEmpty())
            bits << (online == QLatin1String("1") ? "plugged in" : "not plugged in");
        const QString status = readTrimmed(base + "/status");
        if (!status.isEmpty())
            bits << status.toLower();
        const QString cap = readTrimmed(base + "/capacity");
        if (!cap.isEmpty())
            bits << cap + "%";

        /* The kernel reports microvolts and microamps for everything. */
        const QString uv = readTrimmed(base + "/voltage_now");
        if (!uv.isEmpty())
            bits << QString::number(uv.toDouble() / 1000000.0, 'f', 2) + " V";
        const QString ua = readTrimmed(base + "/current_now");
        if (!ua.isEmpty())
            bits << QString::number(ua.toDouble() / 1000.0, 'f', 0) + " mA";

        out << bits.join("  ");
    }
    return out;
}

/*
 * ONE SUPPLY OF A GIVEN TYPE, asked for rather than named.  The PMIC driver
 * registers "battery" and "usb" and nothing else on this board registers either,
 * but the class is keyed on `type' and not on a directory name for a reason: a
 * USB-attached bank arriving as ucsi-source-psy would defeat a hardcoded name and
 * does not defeat this.  Empty means no such supply, which on this machine means
 * the PMIC module did not load.
 */
QString supplyOfType(const QString &type)
{
    const QString root = "/sys/class/power_supply";
    const QStringList names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        if (readTrimmed(root + "/" + name + "/type") == type)
            return root + "/" + name;
    }
    return QString();
}

/*
 * A micro-unit attribute rendered in milli-units, because milli is the scale
 * this board is actually discussed in: 4021 mV and 450 mA are the numbers in the
 * driver's own logs and on the schematic, and 4.021 V is a number nobody has ever
 * said out loud about this cell.
 *
 * Empty means the attribute is missing OR the driver answered -ENODATA, and those
 * two collapsing into one answer is deliberate: "no such reading" and "no reading
 * yet" are both a dash on the glass, and the caller has no different action for
 * them.  `withSign' prefixes a plus, which matters for exactly one attribute --
 * current_now is signed and positive INTO the cell, so +412 mA and -412 mA are a
 * charge and a discharge and not a rounding difference.
 */
QString milliUnits(const QString &path, const QString &unit, bool withSign = false)
{
    bool ok = false;
    const qlonglong micro = readTrimmed(path).toLongLong(&ok);
    if (!ok)
        return QString();
    const qlonglong milli = micro / 1000;
    return (withSign && milli > 0 ? QStringLiteral("+") : QString())
           + QString::number(milli) + " " + unit;
}

/*
 * usb_type, which sysfs writes as the whole SUPPORTED set with the live one in
 * brackets -- "Unknown SDP [DCP] CDP" -- because the one attribute doubles as the
 * driver's capability list.  Take what is inside the brackets when there are any
 * and the whole string when there are not, so this keeps working whichever way
 * the kernel decides to spell it.
 */
QString usbPortType(const QString &base)
{
    const QString raw = readTrimmed(base + "/usb_type");
    const int open = raw.indexOf('[');
    const int close = raw.indexOf(']', open + 1);
    if (open >= 0 && close > open)
        return raw.mid(open + 1, close - open - 1);
    return raw;
}

/* Millidegrees, which is the only unit the thermal class speaks. */
QStringList thermalZones()
{
    QStringList out;
    const QString root = "/sys/class/thermal";
    const QStringList zones = QDir(root).entryList(QStringList() << "thermal_zone*",
                                                   QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &zone : zones) {
        bool ok = false;
        const double c = readTrimmed(root + "/" + zone + "/temp").toDouble(&ok) / 1000.0;
        if (!ok)
            continue;
        const QString type = readTrimmed(root + "/" + zone + "/type");
        out << QString("%1  %2 C").arg(type.isEmpty() ? zone : type, QString::number(c, 'f', 1));
    }
    return out;
}

/* "0 [Audio ]: mt6592-sound - MT6592 Audio", minus the bracket bookkeeping.
 * The second line of each entry has no bracket, which is how it is skipped. */
QStringList soundCards()
{
    QStringList out;
    const QStringList lines = readLines("/proc/asound/cards");
    for (const QString &line : lines) {
        if (!line.contains('['))
            continue;
        const QString what = line.section(':', 1).trimmed();
        if (!what.isEmpty())
            out << line.trimmed().section(' ', 0, 0) + "  " + what;
    }
    if (out.isEmpty() && QFileInfo::exists("/dev/snd/controlC0"))
        out << "controlC0, but /proc/asound says nothing";
    return out;
}

} /* namespace */

InfoPage::InfoPage(QWidget *parent)
    : PageWidget(parent)
{
    /*
     * Three seconds, not the two this page used when it was ten rows long.  A
     * refresh now walks four sysfs classes and statvfs's every mount, and while
     * that is still only a handful of milliseconds on this SoC, none of what it
     * reads changes fast enough to be worth doing more often.
     */
    QTimer *t = new QTimer(this);
    t->setInterval(3000);
    connect(t, &QTimer::timeout, this, &InfoPage::refresh);
    t->start();
}

void InfoPage::addHeader(const QString &text)
{
    Row r;
    r.label = text;
    r.header = true;
    m_rows.append(r);
}

void InfoPage::add(const QString &label, const QString &value)
{
    Row r;
    r.label = label;
    r.value = value;
    r.header = false;
    m_rows.append(r);
}

void InfoPage::addList(const QString &label, const QStringList &values, const QString &empty)
{
    if (values.isEmpty()) {
        add(label, empty);
        return;
    }
    for (int i = 0; i < values.size(); ++i)
        add(i == 0 ? label : QString(), values.at(i));
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

/*
 * The rect paintSheet returns at this size.  It is worked out twice -- there and
 * here -- because the scroll arithmetic needs it outside a paint event, and a
 * scroll limit computed against a guess at the sheet's height is a page whose
 * last row you can never quite reach.  paintSheet lays its title bar out at a
 * fixed 34 pixels and gives back the inside of the card below it; keep the two
 * in step if that ever changes.
 */
QRectF InfoPage::sheetBody() const
{
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    return QRectF(card.x() + 1, card.y() + 35, card.width() - 2, card.height() - 36);
}

/*
 * Walking backwards from the last row is what makes "scrolled all the way down"
 * mean the last row is on the glass rather than one row past it, and it is the
 * only way to get that right when headers are taller than the rows under them.
 */
int InfoPage::maxScroll() const
{
    const qreal avail = sheetBody().height() - kPadTop - kPadBottom;
    qreal used = 0;
    int i = m_rows.size() - 1;
    for (; i >= 0; --i) {
        used += m_rows.at(i).header ? kHeadH : kRowH;
        if (used > avail)
            break;
    }
    /* Never past the last row, however small the sheet has been made: a limit of
     * m_rows.size() is a page scrolled to a blank, and this is called before the
     * widget has been laid out at least once. */
    return qBound(0, i + 1, qMax(0, m_rows.size() - 1));
}

int InfoPage::pageStep() const
{
    const qreal avail = sheetBody().height() - kPadTop - kPadBottom;
    /* One row of overlap, so the eye has something to land on after the jump. */
    return qMax(1, (int)(avail / kRowH) - 1);
}

void InfoPage::scrollBy(int rows)
{
    const int was = m_scroll;
    m_scroll = qBound(0, m_scroll + rows, maxScroll());
    if (m_scroll != was)
        update();
}

/* The section the row at `index' belongs to, by looking back for its header.
 * Drawn in the sheet's title bar, so scrolling never loses which list you are
 * halfway down. */
QString InfoPage::sectionAt(int index) const
{
    for (int i = qMin(index, m_rows.size() - 1); i >= 0; --i)
        if (m_rows.at(i).header)
            return m_rows.at(i).label;
    return QString();
}

bool InfoPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        scrollBy(-1);
        return true;
    case Joypad::NavDown:
        scrollBy(1);
        return true;
    /*
     * Left and right page rather than doing nothing.  Neither means anything
     * else here -- the dashboard drops them when a page declines them -- and
     * this sheet is several screens long on a machine whose only scroll wheel
     * is a thumbstick nudged one row at a time.
     */
    case Joypad::NavLeft:
        scrollBy(-pageStep());
        return true;
    case Joypad::NavRight:
        scrollBy(pageStep());
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
    scrollBy(-notches);
    event->accept();
}

void InfoPage::refresh()
{
    if (!isVisible())
        return;

    m_rows.clear();

    /* ── System ──────────────────────────────────────────────────────────── */
    addHeader(tr("System"));

    /* The board's own name for itself, out of the device tree the bootloader
     * handed the kernel -- the one string on this page nothing in userspace
     * chose. */
    const QString model = readDT("/proc/device-tree/model");
    if (!model.isEmpty())
        add(tr("Device"), model);

    QString kernel = firstWords(readTrimmed("/proc/version"), 3);
    if (kernel.isEmpty())
        kernel = readTrimmed("/proc/sys/kernel/osrelease");
    add(tr("Kernel"), kernel.isEmpty() ? tr("unknown") : kernel);
    add(tr("Dashboard"), tr("build %1, Qt %2, %3")
                         .arg(QString(MIXDASH_BUILD_ID), QString(QT_VERSION_STR),
                              QGuiApplication::platformName()));

    const QString up = readTrimmed("/proc/uptime").section(' ', 0, 0);
    if (!up.isEmpty()) {
        const int secs = (int)up.toDouble();
        add(tr("Uptime"), secs >= 3600
                              ? QString("%1h %2m").arg(secs / 3600).arg((secs / 60) % 60)
                              : QString("%1m %2s").arg(secs / 60).arg(secs % 60));
    }

    /* Only our own words: the rest of the command line is long and known. */
    QStringList words;
    for (const QString &w : readTrimmed("/proc/cmdline").split(' ', Qt::SkipEmptyParts))
        if (w.startsWith("j36."))
            words << w;
    add(tr("Boot words"), words.isEmpty() ? tr("none") : words.join(' '));

    /* ── Processor ───────────────────────────────────────────────────────── */
    addHeader(tr("Processor"));

    const CpuInfo cpu = cpuInfo();
    add(tr("Chip"), cpu.model.isEmpty() ? tr("unknown") : cpu.model);
    if (!cpu.hardware.isEmpty())
        add(tr("Platform"), cpu.hardware);
    /*
     * Both numbers, but only when they differ.  This SoC hotplugs cores out under
     * thermal load and an octa-core reporting two is exactly the thing worth
     * noticing, so the discrepancy is what gets spelled out; when every core the
     * board has is running there is nothing to compare and a bare count reads
     * better than "8 online of 8".
     */
    const int present = cpuListCount(readTrimmed("/sys/devices/system/cpu/present"));
    if (cpu.cores <= 0 && present <= 0)
        add(tr("Cores"), tr("unknown"));
    else if (present > 0 && present != cpu.cores)
        add(tr("Cores"), tr("%1 online of %2").arg(cpu.cores).arg(present));
    else
        add(tr("Cores"), QString::number(qMax(cpu.cores, present)));

    const QString freq = "/sys/devices/system/cpu/cpu0/cpufreq/";
    const QString cur = readTrimmed(freq + "scaling_cur_freq");
    if (!cur.isEmpty()) {
        const QString max = readTrimmed(freq + "cpuinfo_max_freq");
        const QString gov = readTrimmed(freq + "scaling_governor");
        QString line = QString("%1 MHz").arg(cur.toLongLong() / 1000);
        if (!max.isEmpty())
            line += QString(" of %1 MHz").arg(max.toLongLong() / 1000);
        if (!gov.isEmpty())
            line += ", " + gov;
        add(tr("Clock"), line);
    }

    const QString load = readTrimmed("/proc/loadavg");
    if (!load.isEmpty())
        add(tr("Load"), firstWords(load, 3));

    addList(tr("Thermal"), thermalZones(),
            tr("no thermal zones -- the SoC sensor has no driver"));

    /* ── Memory ──────────────────────────────────────────────────────────── */
    addHeader(tr("Memory"));

    long total = 0, avail = 0, swapTotal = 0, swapFree = 0;
    const QStringList meminfo = readLines("/proc/meminfo");
    for (const QString &line : meminfo) {
        const QString value = line.section(':', 1).trimmed().section(' ', 0, 0);
        if (line.startsWith("MemTotal:"))
            total = value.toLong();
        else if (line.startsWith("MemAvailable:"))
            avail = value.toLong();
        else if (line.startsWith("SwapTotal:"))
            swapTotal = value.toLong();
        else if (line.startsWith("SwapFree:"))
            swapFree = value.toLong();
    }
    /*
     * The RAM row is drawn whatever happens.  It used to be `if (total > 0)', so
     * a read that came back with nothing did not print a zero -- it printed no
     * row at all, and a Memory section with only a Swap line under it reads as a
     * page that gave up halfway rather than as a number that could not be had.
     * That is what "no RAM size reported there" was looking at.
     */
    if (total > 0)
        add(tr("RAM"), tr("%1 free of %2")
                           .arg(humanBytes((qulonglong)avail * 1024),
                                humanBytes((qulonglong)total * 1024)));
    else
        add(tr("RAM"), tr("no /proc/meminfo"));
    add(tr("Swap"), swapTotal > 0 ? tr("%1 free of %2")
                                        .arg(humanBytes((qulonglong)swapFree * 1024),
                                             humanBytes((qulonglong)swapTotal * 1024))
                                  : tr("none"));

    /* ── Display ─────────────────────────────────────────────────────────── */
    addHeader(tr("Display"));

    /*
     * The framebuffer, asked rather than assumed.  This is the one row worth the
     * whole page: it is the geometry the panel is running at, straight out of the
     * driver that is scanning it out, so a wrong stride or a 16-bit surprise shows
     * up here instead of as a black screen.
     */
    QString fbLine = tr("no /dev/fb0");
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
            fbLine = tr("/dev/fb0 opened, but it answered no geometry");
        }
        ::close(fb);
    }
    add(tr("Panel"), fbLine);

    const QStringList lights =
        QDir("/sys/class/backlight").entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (!lights.isEmpty()) {
        const QString base = "/sys/class/backlight/" + lights.first();
        const QString now = readTrimmed(base + "/brightness");
        const QString top = readTrimmed(base + "/max_brightness");
        if (!now.isEmpty() && !top.isEmpty())
            add(tr("Backlight"), QString("%1 of %2 (%3)").arg(now, top, lights.first()));
    }

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
    QStringList drm;
    for (const QString &node : nodes) {
        const QString driver =
            QFileInfo("/sys/class/drm/" + node + "/device/driver").symLinkTarget().section('/', -1);

        QStringList conns;
        for (const QString &c : cls.entryList(QStringList() << node + "-*",
                                              QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString status = readTrimmed("/sys/class/drm/" + c + "/status");
            conns << c.section('-', 1) + (status.isEmpty() ? QString() : " " + status);
        }

        drm << QString("%1 %2 -- %3")
                   .arg(node, driver.isEmpty() ? QString("?") : driver,
                        conns.isEmpty() ? tr("render only, no connector, no modesetting")
                                        : conns.join(", "));
    }
    addList(tr("DRM"), drm,
            tr("nothing in /sys/class/drm -- the dashboard does not need it"));

    /* ── Storage ─────────────────────────────────────────────────────────── */
    addHeader(tr("Storage"));

    addList(tr("Disks"), blockDisks(),
            tr("no block devices -- which cannot be, since you booted"));
    addList(tr("Mounted"), mountedVolumes(), tr("nothing mounted from a block device"));

    /* ── Peripherals ─────────────────────────────────────────────────────── */
    addHeader(tr("Peripherals"));

    addList(tr("USB"), usbDevices(),
            QFileInfo::exists("/sys/bus/usb")
                ? tr("the bus is up and empty")
                : tr("no USB stack -- add j36.usb=1 to the command line"));
    addList(tr("Input"), inputDevices(), tr("no /dev/input nodes"));
    if (!m_inputs.isEmpty())
        add(tr("Reading"), m_inputs);

    /* ── Network ─────────────────────────────────────────────────────────── */
    addHeader(tr("Network"));

    addList(tr("Interfaces"), netInterfaces(), tr("none besides loopback"));

    /* ── Power ───────────────────────────────────────────────────────────── */
    addHeader(tr("Power"));

    /*
     * mV and mA throughout, and both halves of every pair on ONE row, because
     * neither half means anything alone.  A 5040 mV charger over a 4021 mV cell
     * with +412 mA between them is a board that is charging.  The same two
     * voltages with 0 mA between them is a board with a cable in and a charger
     * that never armed -- which is the exact failure this section exists to make
     * visible from the glass, on a handheld with no serial console attached.
     *
     * Two current numbers and they are not the same number: "Charger" carries
     * what the PORT licensed after BC1.2, "Charging" carries what CHR_CON4 was
     * then programmed to ask for.  They differ whenever the charger's table has
     * no exact step for the licence -- a 500 mA SDP becomes the 450 mA step --
     * and a board charging slower than the wall allows is that gap, which is why
     * both are on the page rather than one being taken as the other.
     */
    const QString batt = supplyOfType(QStringLiteral("Battery"));
    const QString usb = supplyOfType(QStringLiteral("USB"));

    bool charging = false;
    const int cap = SysInfo::batteryCapacity(&charging);
    add(tr("Cell"), cap < 0
                        ? tr("no power_supply class -- see Diagnostics")
                        : QString::number(cap) + "%"
                              + (charging ? ", " + tr("charging") : QString()));

    if (!batt.isEmpty()) {
        const QString mv = milliUnits(batt + "/voltage_now", "mV");
        const QString ma = milliUnits(batt + "/current_now", "mA", true);
        QStringList bits;
        if (!mv.isEmpty())
            bits << mv;
        if (!ma.isEmpty())
            bits << ma + " " + (ma.startsWith('-') ? tr("out of the cell")
                                                   : tr("into the cell"));
        add(tr("Battery"), bits.isEmpty() ? tr("no sample yet") : bits.join(", "));

        /*
         * The IR-corrected open circuit voltage, which is the number the gauge
         * actually converts to a percentage -- the loaded reading above it can
         * sit two hundred millivolts lower under a game without the cell having
         * lost anything.  Seeing the two disagree is normal; seeing them equal
         * under load means the shunt is reading zero.
         */
        const QString ocv = milliUnits(batt + "/voltage_ocv", "mV");
        if (!ocv.isEmpty())
            add(tr("Resting"), ocv + " " + tr("open circuit"));

        const QString cv = milliUnits(batt + "/constant_charge_voltage", "mV");
        const QString cc = milliUnits(batt + "/constant_charge_current", "mA");
        QStringList set;
        if (!cv.isEmpty())
            set << cv + " " + tr("limit");
        if (!cc.isEmpty())
            set << cc + " " + tr("asked for");
        if (!set.isEmpty())
            add(tr("Charging"), set.join(", "));
    }

    if (!usb.isEmpty()) {
        const bool plugged = readTrimmed(usb + "/online") == QLatin1String("1");
        if (!plugged) {
            add(tr("Charger"), tr("no cable"));
        } else {
            const QString mv = milliUnits(usb + "/voltage_now", "mV");
            const QString ma = milliUnits(usb + "/current_max", "mA");
            const QString port = usbPortType(usb);
            QStringList bits;
            bits << (mv.isEmpty() ? tr("cable in") : mv);
            if (!ma.isEmpty())
                bits << ma + " " + tr("allowed");
            if (!port.isEmpty() && port != QLatin1String("Unknown"))
                bits << port;
            add(tr("Charger"), bits.join(", "));
        }
    }

    addList(tr("Supplies"), powerSupplies(),
            tr("nothing in /sys/class/power_supply -- the PMIC has no driver yet"));

    /* ── Sound ───────────────────────────────────────────────────────────── */
    addHeader(tr("Sound"));

    addList(tr("Cards"), soundCards(),
            tr("no card -- add j36.audio=1 to the command line"));

    if (m_scroll > maxScroll())
        m_scroll = maxScroll();

    update();
}

void InfoPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /*
     * The section name in the title bar, but only once its own header has
     * scrolled off the top: while the header is still on the glass it would just
     * be the same word twice.  Halfway down a long list it is the only thing
     * saying which list you are in.
     */
    const bool atHeader = m_scroll < m_rows.size() && m_rows.at(m_scroll).header;
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, tr("System"),
                                   atHeader ? QString() : sectionAt(m_scroll));

    const QFont headFont = Theme::font(11, true);
    const QFont labelFont = Theme::font(12, true);
    const QFont valueFont = Theme::font(12);
    const QFontMetrics headMetrics(headFont);
    const QFontMetrics valueMetrics(valueFont);

    const qreal left = card.x() + 14;
    const qreal right = card.right() - 14;
    const qreal labelW = 84.0;
    const qreal vx = left + labelW + 12;
    const qreal vw = right - vx;
    qreal y = body.y() + kPadTop;

    for (int i = m_scroll; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        const qreal h = row.header ? kHeadH : kRowH;
        if (y + h > body.bottom() - kPadBottom)
            break;

        if (row.header) {
            /*
             * Bottom-aligned, so the six pixels a header has over a row land as
             * air above it rather than being split either side.  The rule runs
             * from the end of the text to the edge of the sheet, which reads as
             * a divider without needing a second colour on the page.
             */
            p.setFont(headFont);
            p.setPen(Theme::teal());
            p.drawText(QRectF(left, y, right - left, h),
                       Qt::AlignLeft | Qt::AlignBottom, row.label);
            const qreal tw = headMetrics.horizontalAdvance(row.label);
            const qreal ly = qRound(y + h - headMetrics.height() / 2.0) - 0.5;
            if (left + tw + 10 < right) {
                p.setPen(QPen(Theme::separator(), 1.0));
                p.drawLine(QPointF(left + tw + 10, ly), QPointF(right, ly));
            }
        } else {
            if (!row.label.isEmpty()) {
                p.setFont(labelFont);
                p.setPen(Theme::ink3());
                p.drawText(QRectF(left, y, labelW, h), Qt::AlignRight | Qt::AlignVCenter,
                           row.label);
            }
            p.setFont(valueFont);
            p.setPen(Theme::ink());
            p.drawText(QRectF(vx, y, vw, h), Qt::AlignLeft | Qt::AlignVCenter,
                       valueMetrics.elidedText(row.value, Qt::ElideRight, (int)vw));
        }

        y += h;
    }

    /*
     * A scrollbar, and on this page it earns its pixels: the sheet is long
     * enough that without one there is no way to tell a section boundary from
     * the end of the list.  Proportional to rows rather than to pixels, which is
     * close enough when all but nine of them are the same height.
     */
    const int limit = maxScroll();
    if (limit > 0) {
        const qreal trackY = body.y() + kPadTop;
        const qreal trackH = body.height() - kPadTop - kPadBottom;
        const qreal barH = qMax(24.0, trackH * (qreal)(m_rows.size() - limit) / m_rows.size());
        const qreal barY = trackY + (trackH - barH) * m_scroll / limit;
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::dockHi());
        p.drawRoundedRect(QRectF(right + 6, barY, 3, barH), 1.5, 1.5);
    }
}
