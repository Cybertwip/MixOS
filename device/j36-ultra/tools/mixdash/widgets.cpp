/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "widgets.h"
#include "theme.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QTimer>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

QString readTrimmed(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLocal8Bit(f.readAll()).trimmed();
}

/*
 * The cell, if the kernel has anything to say about it.  It does not yet: this
 * kernel has no MT6592 PMIC driver, so there is no power_supply class and this
 * returns -1, which the status bar draws as a dash rather than as 0%.  When the
 * driver lands nothing here has to change.
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

QString firstWords(const QString &s, int n)
{
    const QStringList parts = s.split(' ', Qt::SkipEmptyParts);
    return QStringList(parts.mid(0, n)).join(' ');
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
    default:
        break;
    }

    p.restore();
}

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
    const int cap = batteryCapacity(&m_charging);
    const bool net = networkUp();
    if (cap != m_capacity || net != m_net) {
        m_capacity = cap;
        m_net = net;
    }
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
     * kernel exposes no power_supply class, which on this board it does not yet. */
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

    /* Three bars for the network, lit only if something is actually up. */
    for (int i = 0; i < 3; ++i) {
        const qreal barH = 4.0 + i * 3.0;
        const QRectF b(rx - 12.0 + i * 5.0, by + 5.0 - barH, 3.0, barH);
        p.setPen(Qt::NoPen);
        p.setBrush(m_net ? Theme::ink() : Theme::ink3());
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
    : QWidget(parent)
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

void Dock::paintEvent(QPaintEvent *)
{
    if (m_pages.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QFont f = Theme::font(12, true);
    const QFontMetrics fm(f);
    p.setFont(f);

    /* Slots sized to their labels, so the bar is as wide as it needs to be and
     * centred -- which is what makes it read as a dock and not as a tab strip. */
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

    Theme::softShadow(p, bar, 16, 6, 30);
    QColor glass = Theme::dock();
    glass.setAlpha(Theme::DockAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush(glass);
    p.drawRoundedRect(bar, 16, 16);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawRoundedRect(bar.adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);

    qreal x = bar.x() + pad;
    for (int i = 0; i < m_pages.size(); ++i) {
        const QRectF slot(x, bar.y() + 5, slotW[i], barH - 10);
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
        x += slotW[i];
    }
}

/* ── InfoPage ────────────────────────────────────────────────────────────── */

InfoPage::InfoPage(QWidget *parent)
    : QWidget(parent)
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
    const int cap = batteryCapacity(&charging);
    m_rows.append(qMakePair(QString("Cell"),
                            cap < 0 ? QString("no power_supply class -- this kernel has no PMIC driver yet")
                                    : QString::number(cap) + "%"
                                          + QString(charging ? ", charging" : "")));

    m_rows.append(qMakePair(QString("Sound"),
                            QFileInfo::exists("/dev/snd/controlC0")
                                ? QString("controlC0")
                                : QString("no /dev/snd/controlC0 -- add j36.audio=1")));

    QStringList dri;
    if (QFileInfo::exists("/dev/dri/card0"))
        dri << "card0";
    if (QFileInfo::exists("/dev/dri/renderD128"))
        dri << "renderD128";
    m_rows.append(qMakePair(QString("DRM"),
                            dri.isEmpty() ? QString("nothing in /dev/dri -- the dashboard does not need it")
                                          : dri.join(", ")));

    if (!m_inputs.isEmpty())
        m_rows.append(qMakePair(QString("Input"), m_inputs));

    /* Only our own words: the rest of the command line is long and known. */
    QStringList words;
    for (const QString &w : readTrimmed("/proc/cmdline").split(' ', Qt::SkipEmptyParts))
        if (w.startsWith("j36."))
            words << w;
    m_rows.append(qMakePair(QString("Boot words"),
                            words.isEmpty() ? QString("none") : words.join(' ')));

    update();
}

void InfoPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    Theme::softShadow(p, card, Theme::Radius, 6, 24);
    Theme::vgrad(p, card, Theme::window(), Theme::window().darker(112), Theme::Radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), Theme::Radius, Theme::Radius);

    /* A title bar inside the card, clipped to its top corners. */
    QPainterPath clip;
    clip.addRoundedRect(card, Theme::Radius, Theme::Radius);
    p.save();
    p.setClipPath(clip);
    const QRectF head(card.x(), card.y(), card.width(), 34);
    Theme::vgrad(p, head, Theme::titlebar(), Theme::titlebarLow());
    p.setPen(QPen(Theme::separator(), 1.0));
    p.drawLine(QPointF(head.x(), head.bottom() - 0.5), QPointF(head.right(), head.bottom() - 0.5));
    p.restore();

    p.setFont(Theme::font(14, true));
    p.setPen(Theme::ink());
    p.drawText(head.adjusted(14, 0, -14, 0), Qt::AlignLeft | Qt::AlignVCenter, "System");

    const QFont labelFont = Theme::font(12, true);
    const QFont valueFont = Theme::font(12);
    const QFontMetrics valueMetrics(valueFont);
    const qreal labelW = 96.0;
    const qreal rowH = 21.0;
    qreal y = head.bottom() + 8;

    for (int i = 0; i < m_rows.size(); ++i) {
        if (y + rowH > card.bottom() - 6)
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
