/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * region.cpp -- the map, the zones, and the two ways of writing a time zone.
 *
 * See region.h for why this page exists and why it draws its own world.  What
 * follows is the data and the arithmetic.
 */
#include "region.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QProcess>
#include <QResizeEvent>
#include <QTimeZone>
#include <QTimer>
#include <QtGlobal>

#include <stdlib.h>
#include <time.h>

#include "joypad.h"
#include "settings.h"
#include "shell.h"
#include "stringsdb.h"
#include "theme.h"
#include "widgets.h"

namespace {

/*
 * THE COASTLINES, AS WHOLE DEGREES OF LONGITUDE AND LATITUDE.
 *
 * One flat array of pairs, with (kSep, kSep) between one outline and the next,
 * because the alternative -- an array per landmass and a table of pointers to
 * them -- is thirty relocations and three times the source for a shape that is
 * only ever walked front to back.
 *
 * EVERY OUTLINE IS A CLOSED POLYGON, drawn filled and then stroked, so that the
 * map reads as land against sea rather than as a tangle of lines.  They are also
 * deliberately sloppy: an odd-even fill does not care that Jutland doubles back
 * on itself or that the Persian Gulf is cut in as a notch, and at this scale --
 * about 1.5 px to the degree on a 560 px world -- neither does the eye.
 *
 * NOTHING CROSSES THE ANTIMERIDIAN.  Siberia stops at 179 and Chukotka's last
 * few hundred kilometres are simply not drawn, because a polygon with a point at
 * 179 and the next at -179 does not wrap on an equirectangular projection: it
 * draws a bar straight across the Pacific.  Losing a headland nobody was looking
 * for is the cheaper of the two errors.
 */
enum { kSep = 32767 };

const short kLand[] = {
    /* Africa */
    -6,36,  3,37,  10,37,  11,34,  18,31,  25,32,  31,31,  34,31,
    35,24,  37,20,  39,15,  43,12,  51,12,  47,4,   43,0,   40,-4,
    40,-10, 36,-18, 35,-24, 31,-30, 25,-34, 18,-34, 15,-27, 12,-18,
    13,-13, 12,-6,  9,-1,   9,4,    4,6,    1,6,   -4,5,   -8,4,
    -12,8, -17,15, -16,20, -13,25, -10,30,  -9,33,
    kSep,kSep,

    /* Eurasia, from Galicia clockwise: the Mediterranean's north shore, Arabia,
     * India, the far east, the Arctic, Scandinavia and the Baltic, home. */
    -9,43,  -9,38,  -6,36,  -1,37,  0,39,   3,42,   7,43,   10,44,
    12,41,  16,38,  18,40,  13,45,  16,43,  19,42,  22,37,  24,40,
    27,41,  29,41,  36,36,  36,33,  34,29,  35,28,  39,21,  43,13,
    52,17,  58,24,  55,25,  51,25,  48,29,  50,30,  56,27,  61,25,
    67,25,  72,21,  73,16,  77,8,   80,13,  87,21,  92,21,  96,16,
    98,8,   104,1,  105,10, 109,13, 107,20, 113,22, 117,24, 122,31,
    119,35, 122,40, 126,38, 129,35, 129,39, 128,42, 131,43, 135,48,
    137,55, 142,59, 150,59, 160,60, 156,51, 163,57, 162,61, 170,60,
    175,65, 179,66, 179,70, 170,70, 160,70, 150,72, 140,73, 130,73,
    113,74, 100,76, 90,73,  80,73,  70,68,  60,70,  50,68,  40,68,
    32,70,  25,71,  20,70,  16,68,  12,65,  5,62,   6,58,   11,59,
    12,56,  16,56,  19,58,  21,60,  19,63,  22,66,  25,65,  25,60,
    28,60,  24,58,  21,57,  19,55,  15,54,  11,54,  10,57,  9,54,
    4,53,   0,50,   -2,48,  -5,48,  -1,46,  -2,43,
    kSep,kSep,

    /* North America, with Baja and Hudson Bay because without them the shape is
     * a blob and with them it is a continent. */
    -166,66, -162,60, -150,60, -140,60, -133,55, -124,48, -124,40, -119,34,
    -117,32, -112,24, -109,27, -114,31, -106,23, -100,17, -95,16,  -92,14,
    -87,13,  -83,9,   -79,9,   -82,10,  -84,16,  -88,21,  -91,18,  -95,19,
    -97,26,  -94,29,  -89,29,  -84,30,  -81,25,  -81,32,  -76,35,  -74,40,
    -70,42,  -67,45,  -64,45,  -60,47,  -64,50,  -56,54,  -64,60,  -78,62,
    -80,55,  -88,56,  -94,60,  -90,66,  -82,70,  -95,70,  -115,70, -130,70,
    -145,70, -160,71,
    kSep,kSep,

    /* South America */
    -78,8,  -72,11, -62,10, -52,5,  -45,-2, -38,-5, -35,-8, -39,-16,
    -43,-23, -48,-26, -55,-35, -62,-40, -65,-45, -68,-52, -71,-55, -75,-50,
    -73,-42, -73,-37, -71,-30, -70,-23, -76,-14, -81,-6, -80,0,  -77,4,
    kSep,kSep,

    /* Greenland */
    -45,60, -52,65, -55,70, -60,76, -50,82, -30,83, -22,73, -38,66,
    kSep,kSep,

    /* Australia */
    113,-22, 114,-26, 115,-34, 123,-34, 129,-32, 135,-35, 138,-35, 141,-38,
    146,-39, 150,-37, 153,-30, 153,-25, 146,-19, 142,-11, 137,-12, 130,-12,
    127,-14, 122,-17, 114,-21,
    kSep,kSep,

    /* Tasmania */
    145,-41, 148,-41, 148,-43, 145,-43,
    kSep,kSep,

    /* New Guinea */
    131,-1, 140,-3, 147,-8, 150,-10, 143,-9, 137,-8, 131,-3,
    kSep,kSep,

    /* Borneo */
    109,2, 117,7, 119,4, 117,-3, 110,-3,
    kSep,kSep,

    /* Sumatra */
    95,6, 98,5, 106,-5, 103,-6, 96,2,
    kSep,kSep,

    /* Java */
    105,-6, 115,-8, 114,-9, 105,-7,
    kSep,kSep,

    /* The Philippines, as one island because seven thousand is not a budget this
     * array has. */
    120,18, 122,14, 126,10, 126,6, 122,8, 120,13, 119,16,
    kSep,kSep,

    /* Japan */
    130,32, 132,34, 136,34, 140,36, 141,40, 141,45, 145,44, 140,42,
    137,37, 133,35, 130,34,
    kSep,kSep,

    /* Great Britain */
    -5,50, 1,51, 0,53, -1,55, -3,58, -5,57, -5,54, -3,51,
    kSep,kSep,

    /* Ireland */
    -10,52, -6,52, -6,55, -10,54,
    kSep,kSep,

    /* Iceland */
    -24,65, -18,66, -14,65, -18,63, -22,64,
    kSep,kSep,

    /* Madagascar */
    49,-12, 50,-16, 47,-25, 44,-20, 44,-16,
    kSep,kSep,

    /* New Zealand, both islands */
    173,-35, 178,-38, 176,-41, 172,-39,
    kSep,kSep,
    172,-41, 174,-42, 171,-46, 167,-46, 169,-43,
    kSep,kSep,

    /* Sri Lanka */
    80,9, 82,7, 80,6, 79,8,
    kSep,kSep,

    /* Cuba */
    -85,22, -80,23, -74,20, -78,21,
    kSep,kSep,

    /*
     * Antarctica, closed across the bottom of the projection rather than around
     * the pole.
     *
     * IT IS HERE BECAUSE OF THE HOLE IT LEAVES, not because anybody is setting
     * their clock to Vostok: an equirectangular world with nothing below sixty
     * south is a picture with a sixth of it missing, and the eye reads that as a
     * map that failed to draw rather than as an empty ocean.  The coast is the
     * real one to about five degrees; the last two points shut the polygon off
     * the bottom edge of the map, where the clip rectangle eats them.
     */
    -180,-78, -160,-78, -140,-75, -120,-73, -100,-73, -80,-73, -70,-70, -62,-65,
    -58,-63,  -60,-68,  -45,-73,  -30,-75,  -15,-72,  0,-70,   20,-70,  40,-68,
    60,-67,   80,-67,   100,-66,  120,-66,  140,-67,  160,-78,  170,-79, 179,-79,
    179,-88,  -180,-88
};

/*
 * THE ZONES, AND THERE ARE SIXTY OF THEM RATHER THAN THE FIVE HUNDRED THE TZ
 * DATABASE CARRIES.
 *
 * Every row here is a dot somebody has to be able to land on with a D-pad, and a
 * map with five hundred dots on it is a map with no dots on it.  What is kept is
 * one city per offset per populated region -- so the list covers every UTC offset
 * a user of this handheld is plausibly standing in, and a user standing somewhere
 * else picks the neighbour that shares their offset and their rules, which is
 * what the tz database's own `Link' lines do anyway.
 *
 * `lang' is the SUGGESTION, not a setting: Lang::Count means this row has nothing
 * to suggest, which is the honest answer for most of the world.  Six languages do
 * not cover a planet and pretending otherwise would mean a Polish user's time
 * zone quietly switching the shell to German because Warsaw is nearer Berlin than
 * London.
 *
 * The city names are NOT run through tr().  They are proper nouns, they are
 * written the way the place writes them where that fits in the font, and a
 * translated place name on a map is how somebody looking for "Lisboa" fails to
 * find "Lisbon".
 */
struct ZoneDot {
    const char *zone;
    const char *city;
    short lon;
    short lat;
    int lang;
};

const ZoneDot kZones[] = {
    /* The Americas */
    { "Pacific/Honolulu",                "Honolulu",     -158, 21, Lang::English },
    { "America/Anchorage",               "Anchorage",    -150, 61, Lang::English },
    { "America/Los_Angeles",             "Los Angeles",  -118, 34, Lang::English },
    { "America/Denver",                  "Denver",       -105, 40, Lang::English },
    { "America/Chicago",                 "Chicago",       -88, 42, Lang::English },
    { "America/Mexico_City",             "Ciudad de Mexico", -99, 19, Lang::Spanish },
    { "America/New_York",                "New York",      -74, 41, Lang::English },
    { "America/Toronto",                 "Toronto",       -79, 44, Lang::English },
    { "America/Halifax",                 "Halifax",       -64, 45, Lang::English },
    { "America/Havana",                  "La Habana",     -82, 23, Lang::Spanish },
    { "America/Bogota",                  "Bogota",        -74,  5, Lang::Spanish },
    { "America/Lima",                    "Lima",          -77,-12, Lang::Spanish },
    { "America/Santiago",                "Santiago",      -71,-33, Lang::Spanish },
    { "America/Argentina/Buenos_Aires",  "Buenos Aires",  -58,-35, Lang::Spanish },
    { "America/Sao_Paulo",               "Sao Paulo",     -47,-24, Lang::Portuguese },
    { "America/Manaus",                  "Manaus",        -60, -3, Lang::Portuguese },

    /* The Atlantic and Europe */
    { "UTC",                             "UTC",             0,-24, Lang::Count },
    { "Atlantic/Reykjavik",              "Reykjavik",     -22, 64, Lang::English },
    { "Atlantic/Azores",                 "Ponta Delgada", -26, 38, Lang::Portuguese },
    { "Europe/Lisbon",                   "Lisboa",         -9, 39, Lang::Portuguese },
    { "Europe/Dublin",                   "Dublin",         -6, 53, Lang::English },
    { "Europe/London",                   "London",          0, 52, Lang::English },
    { "Europe/Madrid",                   "Madrid",         -4, 40, Lang::Spanish },
    { "Europe/Paris",                    "Paris",           2, 49, Lang::French },
    { "Europe/Brussels",                 "Bruxelles",       4, 51, Lang::French },
    { "Europe/Amsterdam",                "Amsterdam",       5, 52, Lang::Count },
    { "Europe/Zurich",                   "Zurich",          8, 47, Lang::German },
    { "Europe/Rome",                     "Roma",           12, 42, Lang::Italian },
    { "Europe/Berlin",                   "Berlin",         13, 52, Lang::German },
    { "Europe/Vienna",                   "Wien",           16, 48, Lang::German },
    { "Europe/Stockholm",                "Stockholm",      18, 59, Lang::Count },
    { "Europe/Warsaw",                   "Warszawa",       21, 52, Lang::Count },
    { "Europe/Athens",                   "Athina",         24, 38, Lang::Count },
    { "Europe/Helsinki",                 "Helsinki",       25, 60, Lang::Count },
    { "Europe/Istanbul",                 "Istanbul",       29, 41, Lang::Count },
    { "Europe/Kyiv",                     "Kyiv",           31, 50, Lang::Count },
    { "Europe/Moscow",                   "Moskva",         38, 56, Lang::Count },

    /* Africa */
    { "Africa/Casablanca",               "Casablanca",     -8, 34, Lang::French },
    { "Africa/Lagos",                    "Lagos",           3,  6, Lang::English },
    { "Africa/Luanda",                   "Luanda",         13, -9, Lang::Portuguese },
    { "Africa/Cairo",                    "Cairo",          31, 30, Lang::Count },
    { "Africa/Nairobi",                  "Nairobi",        37, -1, Lang::English },
    { "Africa/Johannesburg",             "Johannesburg",   28,-26, Lang::English },

    /* Asia */
    { "Asia/Jerusalem",                  "Jerusalem",      35, 32, Lang::Count },
    { "Asia/Tehran",                     "Tehran",         51, 36, Lang::Count },
    { "Asia/Dubai",                      "Dubai",          55, 25, Lang::Count },
    { "Asia/Karachi",                    "Karachi",        67, 25, Lang::Count },
    { "Asia/Kolkata",                    "Kolkata",        88, 23, Lang::Count },
    { "Asia/Novosibirsk",                "Novosibirsk",    83, 55, Lang::Count },
    { "Asia/Bangkok",                    "Bangkok",       100, 14, Lang::Count },
    { "Asia/Singapore",                  "Singapore",     104,  1, Lang::English },
    { "Asia/Jakarta",                    "Jakarta",       107, -6, Lang::Count },
    { "Asia/Hong_Kong",                  "Hong Kong",     114, 22, Lang::Count },
    { "Asia/Manila",                     "Manila",        121, 15, Lang::Count },
    { "Asia/Shanghai",                   "Shanghai",      121, 31, Lang::Count },
    { "Asia/Seoul",                      "Seoul",         127, 38, Lang::Count },
    { "Asia/Tokyo",                      "Tokyo",         140, 36, Lang::Count },
    { "Asia/Vladivostok",                "Vladivostok",   132, 43, Lang::Count },

    /* Oceania */
    { "Australia/Perth",                 "Perth",         116,-32, Lang::English },
    { "Australia/Adelaide",              "Adelaide",      139,-35, Lang::English },
    { "Australia/Brisbane",              "Brisbane",      153,-27, Lang::English },
    { "Australia/Sydney",                "Sydney",        151,-34, Lang::English },
    { "Pacific/Auckland",                "Auckland",      175,-37, Lang::English }
};

const int kZoneCount = int(sizeof(kZones) / sizeof(kZones[0]));

QString zoneFile(const QString &zone)
{
    return QStringLiteral("/usr/share/zoneinfo/") + zone;
}

/*
 * Which of the two zones this table carries is the one in force, by name.
 *
 * BY THE NAME AND NOT BY THE OFFSET, because an offset is shared -- Lisbon and
 * London are the same hour and different rules -- and the ring on the map is a
 * claim about which row the user picked, not about what o'clock it is.
 */
int zoneIndexOf(const QString &zone)
{
    if (zone.isEmpty())
        return -1;
    for (int i = 0; i < kZoneCount; ++i) {
        if (zone == QLatin1String(kZones[i].zone))
            return i;
    }
    return -1;
}

/* What /etc/localtime points at, as a zone name, or empty.  This is how the page
 * opens with a ring already on the map on a device somebody set up last week. */
QString currentZone()
{
    const QString remembered = Settings::instance().timezone();
    if (!remembered.isEmpty())
        return remembered;

    const QFileInfo link(QStringLiteral("/etc/localtime"));
    if (link.isSymLink()) {
        const QString target = link.symLinkTarget();
        const int cut = target.indexOf(QStringLiteral("/zoneinfo/"));
        if (cut >= 0)
            return target.mid(cut + 10);
    }

    QFile f(QStringLiteral("/etc/timezone"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f.readLine()).trimmed();
    return QString();
}

/* timedatectl, if this rootfs has one where it is usually kept.  Empty means the
 * symlink path is the only path, which is not a failure: see writeZone(). */
QString timedatectlPath()
{
    static const char *const kPaths[] = {
        "/usr/bin/timedatectl", "/bin/timedatectl", "/usr/sbin/timedatectl"
    };
    for (unsigned i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); ++i) {
        if (QFileInfo::exists(QLatin1String(kPaths[i])))
            return QLatin1String(kPaths[i]);
    }
    return QString();
}

/* "UTC+02:00".  QTimeZone reads /usr/share/zoneinfo for this, so it is not free
 * and it is not called from a paint. */
QString offsetText(const QString &zone)
{
    const QTimeZone tz(zone.toUtf8());
    if (!tz.isValid())
        return QString();
    const int secs = tz.offsetFromUtc(QDateTime::currentDateTimeUtc());
    const int mins = qAbs(secs) / 60;
    return QStringLiteral("UTC%1%2:%3")
        .arg(QLatin1String(secs < 0 ? "-" : "+"))
        .arg(mins / 60, 2, 10, QLatin1Char('0'))
        .arg(mins % 60, 2, 10, QLatin1Char('0'));
}

} /* namespace */

/* ── construction and layout ─────────────────────────────────────────────── */

RegionPage::RegionPage(QWidget *parent)
    : PageWidget(parent)
{
    m_zones = new ListPane(this);
    m_zones->setRowHeight(30);
    connect(m_zones, &ListPane::activated, this, &RegionPage::onZoneActivated);

    m_langs = new ListPane(this);
    m_langs->setRowHeight(32);
    connect(m_langs, &ListPane::activated, this, &RegionPage::onLanguageActivated);

    /*
     * Ten seconds, and it is a minute clock.
     *
     * The footer shows HH:mm, so a second-by-second timer would repaint the whole
     * map fifty-nine times out of sixty to change nothing -- on a board where the
     * map is the most expensive thing this page draws.  Ten seconds means the
     * displayed minute is at most ten seconds stale, which nobody setting a time
     * zone is measuring, and the timer only runs while the page is on the glass.
     */
    m_clock = new QTimer(this);
    m_clock->setInterval(10000);
    connect(m_clock, &QTimer::timeout, this, &RegionPage::onTick);

    setPane(PaneMap);
}

QRectF RegionPage::mapRect() const
{
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body(card.x() + 1, card.y() + 35, card.width() - 2, card.height() - 36);

    /* Under the hint line and the tab strip, and above three lines of footer. */
    const qreal top = body.y() + 46;
    const qreal avail = body.bottom() - 8 - 58 - top;
    qreal w = body.width() - 24;
    qreal h = w / 2.0;
    if (h > avail) {
        h = avail;
        w = h * 2.0;
    }
    return QRectF(body.x() + (body.width() - w) / 2.0, top, w, h);
}

QPointF RegionPage::project(int lon, int lat, const QRectF &r) const
{
    return QPointF(r.x() + (lon + 180.0) * r.width() / 360.0,
                   r.y() + (90.0 - lat) * r.height() / 180.0);
}

void RegionPage::resizeEvent(QResizeEvent *event)
{
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRect list(int(card.x()) + 7, int(card.y()) + 35 + 46,
                     int(card.width()) - 14, int(card.height()) - 35 - 52);
    m_zones->setGeometry(list);
    m_langs->setGeometry(list);
    QWidget::resizeEvent(event);
}

/* ── coming and going ────────────────────────────────────────────────────── */

void RegionPage::onEnter()
{
    m_offer = -1;
    m_applied = zoneIndexOf(currentZone());
    if (m_applied >= 0)
        m_cursor = m_applied;

    rebuildZones();
    rebuildLanguages();
    refreshCursorInfo();
    m_clock->start();
    update();
}

void RegionPage::onLeave()
{
    m_clock->stop();
    m_offer = -1;
}

void RegionPage::onTick()
{
    refreshCursorInfo();
    update();
}

void RegionPage::refreshCursorInfo()
{
    m_cursorInfo.clear();
    if (m_cursor < 0 || m_cursor >= kZoneCount)
        return;

    const QString zone = QLatin1String(kZones[m_cursor].zone);
    const QString off = offsetText(zone);
    const QTimeZone tz(zone.toUtf8());
    if (!tz.isValid()) {
        /* The zone is not on this card.  Said plainly rather than shown as a
         * blank, because it is the one failure the user can act on: it means the
         * tzdata package is missing, not that they pressed the wrong thing. */
        m_cursorInfo = tr("not installed");
        return;
    }
    const QDateTime there = QDateTime::currentDateTimeUtc().toTimeZone(tz);
    m_cursorInfo = QStringLiteral("%1  %2").arg(off, there.toString(QStringLiteral("HH:mm")));
}

void RegionPage::setPane(int pane)
{
    m_pane = qBound(0, pane, int(PaneCount) - 1);
    m_zones->setVisible(m_pane == PaneZones);
    m_langs->setVisible(m_pane == PaneLanguage);
    update();
}

/* ── the two lists ───────────────────────────────────────────────────────── */

void RegionPage::rebuildZones()
{
    QVector<ListRow> rows;
    for (int i = 0; i < kZoneCount; ++i) {
        ListRow r;
        r.kind = ListRow::Item;
        r.glyph = GlyphGlobe;
        r.text = QString::fromUtf8(kZones[i].city);
        r.detail = QLatin1String(kZones[i].zone);
        r.id = i + 1;                    /* +1 so row zero is not the falsy id */
        r.accent = (i == m_applied) ? Theme::green() : Theme::blue();
        if (i == m_applied) {
            r.badge = tr("current");
            r.badgeColour = Theme::green();
        }
        rows << r;
    }
    m_zones->setRows(rows);
    m_zones->setCurrent(m_cursor >= 0 ? m_cursor : 0);
}

void RegionPage::rebuildLanguages()
{
    QVector<ListRow> rows;
    const int now = Strings::instance().language();
    for (int i = 0; i < Lang::Count; ++i) {
        ListRow r;
        r.kind = ListRow::Item;
        r.glyph = GlyphInfo;
        /*
         * The native name is the row and the English name is the detail, which is
         * the one thing this page inherits unchanged from the language page it
         * replaces.  Somebody who has landed on a language they cannot read gets
         * out of it by recognising the word for their own.
         */
        r.text = Strings::nativeName(i);
        r.detail = Strings::englishName(i);
        r.id = i + 1;
        r.accent = (i == now) ? Theme::green() : Theme::blue();
        if (i == now) {
            r.badge = tr("current");
            r.badgeColour = Theme::green();
        }
        rows << r;
    }
    m_langs->setRows(rows);
    m_langs->setCurrent(now);
}

void RegionPage::onZoneActivated(int index)
{
    const QVector<ListRow> &rows = m_zones->rows();
    if (index < 0 || index >= rows.size())
        return;
    commit(rows[index].id - 1);
}

void RegionPage::onLanguageActivated(int index)
{
    const QVector<ListRow> &rows = m_langs->rows();
    if (index < 0 || index >= rows.size())
        return;

    const int id = rows[index].id - 1;
    if (id < 0 || id >= Lang::Count || id == Strings::instance().language())
        return;

    /*
     * setLanguage() emits languageChanged(), which the Dashboard answers by
     * rebuilding the cards.  Both lists are rebuilt here so the "current" badge
     * moves in the same frame as the rest of the shell: a settings screen that
     * has to be left and re-entered before it agrees with itself reads as a
     * setting that did not take.
     */
    Strings::instance().setLanguage(id);
    rebuildZones();
    rebuildLanguages();
    emit titleChanged();
    emit toastRequested(tr("Language changed"), 2000);
    update();
}

/* ── writing the zone ────────────────────────────────────────────────────── */

bool RegionPage::writeZone(const QString &zone, QString *how)
{
    if (how)
        how->clear();

    if (!QFileInfo::exists(zoneFile(zone))) {
        if (how)
            *how = tr("no zone file on this card");
        return false;
    }

    /*
     * THIS PROCESS FIRST, AND UNCONDITIONALLY.
     *
     * Everything below can fail -- there may be no timedatectl, /etc may be
     * read-only -- and on the boot where it all fails the user has still told
     * this dashboard where they are.  Setting TZ here means the status bar clock
     * is right immediately and stays right for this run, which is the part of the
     * setting they can actually see.  tzset() is what makes libc notice; without
     * it the change lands on the next process, not this one.
     */
    qputenv("TZ", zone.toUtf8());
    tzset();

    const QString tdc = timedatectlPath();
    if (!tdc.isEmpty()) {
        /*
         * The right way, when there is a systemd to ask: it rewrites the symlink,
         * tells the kernel, and wakes everything holding a timer.  LC_ALL=C for
         * the same reason wifi.cpp does it -- a dashboard that speaks six
         * languages must not ask its tools to speak any of them.
         */
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
        p.setProcessEnvironment(env);
        p.start(tdc, QStringList() << QStringLiteral("set-timezone") << zone);
        if (Shell::waitForStarted(p, 1500) && Shell::waitForFinished(p, 6000)
            && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
            if (how)
                *how = QStringLiteral("timedatectl");
            return true;
        }
        p.kill();
        Shell::waitForFinished(p, 500);
    }

    /*
     * By hand, which is what timedatectl would have done.
     *
     * The symlink is the setting; /etc/timezone is a Debian convention that
     * several packages read and that costs one open to keep in step.  A relative
     * target -- ../usr/share/zoneinfo/... -- is what timedatectl writes and what
     * survives the tree being mounted somewhere else, which is exactly what the
     * build system does to this rootfs.
     */
    bool ok = false;
    QFile::remove(QStringLiteral("/etc/localtime"));
    if (QFile::link(QStringLiteral("../usr/share/zoneinfo/") + zone,
                    QStringLiteral("/etc/localtime"))) {
        ok = true;
        QFile tzf(QStringLiteral("/etc/timezone"));
        if (tzf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            tzf.write(zone.toUtf8() + '\n');
    }

    if (how)
        *how = ok ? QStringLiteral("/etc/localtime") : tr("read-only, this session only");
    return ok;
}

void RegionPage::commit(int zone)
{
    if (zone < 0 || zone >= kZoneCount)
        return;

    m_cursor = zone;
    const QString name = QLatin1String(kZones[zone].zone);
    const bool ok = writeZone(name, &m_how);

    /*
     * REMEMBERED WHETHER OR NOT THE WRITE LANDED, because the two failures this
     * has are both temporary: a read-only rootfs is a boot, not a device, and a
     * missing timedatectl is a package.  Storing the choice means the next boot
     * gets the clock right through applyStoredTimezone() even if /etc never
     * agreed, and it means the ring is on the right city when this page opens
     * again.
     */
    Settings::instance().setTimezone(name);
    m_applied = zone;
    rebuildZones();
    refreshCursorInfo();

    emit toastRequested(ok ? tr("Time zone set to %1").arg(name)
                           : tr("Time zone applied to the dashboard only"),
                        2200);

    /* The suggestion, asked rather than done.  See region.h. */
    const int lang = kZones[zone].lang;
    if (lang >= 0 && lang < Lang::Count && lang != Strings::instance().language())
        m_offer = lang;

    update();
}

void RegionPage::applyStoredTimezone()
{
    const QString zone = Settings::instance().timezone();
    if (zone.isEmpty())
        return;
    if (!QFileInfo::exists(zoneFile(zone)))
        return;
    qputenv("TZ", zone.toUtf8());
    tzset();
}

/* ── input ───────────────────────────────────────────────────────────────── */

void RegionPage::stepMap(int dx, int dy)
{
    if (m_cursor < 0 || m_cursor >= kZoneCount)
        return;

    const int fromLon = kZones[m_cursor].lon;
    const int fromLat = kZones[m_cursor].lat;

    /*
     * Nearest dot that is genuinely in the pressed direction.
     *
     * The score is the distance along the axis pressed plus four times the drift
     * across it, measured in pixels rather than in degrees: the map is twice as
     * wide as it is tall, so a degree of longitude is half a degree of latitude on
     * the glass, and both scores below are doubled to keep that in integers.  The
     * weighting is what stops Right out of Paris landing on Casablanca -- Rome is
     * further away in a straight line and very much more to the right.
     *
     * dy is LATITUDE and not screen y: Up is dy = +1 and is north.  Written that
     * way because everything else in this file is degrees, and one place that
     * flipped the sign would be the bug nobody finds by reading.
     */
    int best = -1;
    long bestScore = 0;
    for (int i = 0; i < kZoneCount; ++i) {
        if (i == m_cursor)
            continue;
        const int dLon = (kZones[i].lon - fromLon);
        const int dLat = (kZones[i].lat - fromLat);
        const int along = dx != 0 ? dLon * dx : dLat * dy;
        const int across = dx != 0 ? dLat : dLon;
        if (along <= 0)
            continue;
        const long scoreAlong = long(along) * (dx != 0 ? 1 : 2);
        const long scoreAcross = long(qAbs(across)) * (dx != 0 ? 8 : 4);
        const long score = scoreAlong + scoreAcross;
        if (best < 0 || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }

    if (best < 0)
        return;
    m_cursor = best;
    m_zones->setCurrent(m_cursor);
    refreshCursorInfo();
    update();
}

bool RegionPage::handleNav(int action)
{
    /*
     * While the language question is up, A and B are its answer and the D-pad is
     * nothing.  Swallowing the directions is deliberate: a cursor that walks off
     * to another continent under a question about Paris leaves a Yes that means
     * something the user did not read.
     */
    if (m_offer >= 0) {
        switch (action) {
        case Joypad::NavOk: {
            const int lang = m_offer;
            m_offer = -1;
            Strings::instance().setLanguage(lang);
            rebuildZones();
            rebuildLanguages();
            emit titleChanged();
            emit toastRequested(tr("Language changed"), 2000);
            update();
            return true;
        }
        case Joypad::NavBack:
        case Joypad::NavUp:
        case Joypad::NavDown:
        case Joypad::NavLeft:
        case Joypad::NavRight:
            m_offer = -1;
            update();
            return true;
        default:
            return true;
        }
    }

    switch (action) {
    case Joypad::NavPrevPage:
        setPane(m_pane == 0 ? int(PaneCount) - 1 : m_pane - 1);
        return true;
    case Joypad::NavNextPage:
        setPane(m_pane == int(PaneCount) - 1 ? 0 : m_pane + 1);
        return true;
    default:
        break;
    }

    if (m_pane == PaneMap) {
        switch (action) {
        case Joypad::NavUp:    stepMap(0, 1);  return true;
        case Joypad::NavDown:  stepMap(0, -1); return true;
        case Joypad::NavLeft:  stepMap(-1, 0); return true;
        case Joypad::NavRight: stepMap(1, 0);  return true;
        case Joypad::NavOk:    commit(m_cursor); return true;
        default: break;
        }
        return false;
    }

    ListPane *pane = (m_pane == PaneZones) ? m_zones : m_langs;
    switch (action) {
    case Joypad::NavUp:
        pane->step(-1);
        if (pane == m_zones) {
            m_cursor = pane->current();
            refreshCursorInfo();
        }
        return true;
    case Joypad::NavDown:
        pane->step(1);
        if (pane == m_zones) {
            m_cursor = pane->current();
            refreshCursorInfo();
        }
        return true;
    case Joypad::NavOk:
        return pane->press();
    default:
        break;
    }
    return false;
}

void RegionPage::mousePressEvent(QMouseEvent *event)
{
    if (m_offer >= 0) {
        /* A pointer press is a Yes nobody typed.  It dismisses the question and
         * changes nothing, which is the safe half of an ambiguous input. */
        m_offer = -1;
        update();
        return;
    }

    const QPointF at = event->pos();

    /* The tab strip: three pills, laid out by paintEvent from the same rects. */
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body(card.x() + 1, card.y() + 35, card.width() - 2, card.height() - 36);
    for (int i = 0; i < PaneCount; ++i) {
        const QRectF pill(body.x() + 12 + i * 96.0, body.y() + 22, 92, 20);
        if (pill.contains(at)) {
            setPane(i);
            return;
        }
    }

    if (m_pane != PaneMap)
        return;

    /* Nearest dot within a fingertip of the press.  The threshold is generous
     * because the dots are three pixels and the pointer on this board is driven
     * by a thumbstick. */
    const QRectF r = mapRect();
    int best = -1;
    qreal bestDist = 0;
    for (int i = 0; i < kZoneCount; ++i) {
        const QPointF p = project(kZones[i].lon, kZones[i].lat, r);
        const qreal dx = p.x() - at.x();
        const qreal dy = p.y() - at.y();
        const qreal d = dx * dx + dy * dy;
        if (best < 0 || d < bestDist) {
            best = i;
            bestDist = d;
        }
    }
    if (best < 0 || bestDist > 18 * 18)
        return;

    if (best == m_cursor) {
        /* The second press on a dot that is already under the crosshair is the
         * press that means it: one tap to look, one tap to set. */
        commit(best);
        return;
    }
    m_cursor = best;
    m_zones->setCurrent(m_cursor);
    refreshCursorInfo();
    update();
}

/* ── paint ───────────────────────────────────────────────────────────────── */

void RegionPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, title(),
                                   Settings::instance().writable() ? QString()
                                                                   : tr("not saved"));

    static const char *const kHints[PaneCount] = {
        QT_TR_NOOP("A sets the time zone.  L1 and R1 change the pane."),
        QT_TR_NOOP("A sets the time zone.  L1 and R1 change the pane."),
        QT_TR_NOOP("A changes the language everywhere at once.")
    };
    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, tr(kHints[m_pane]));

    /* The tab strip. */
    const QString names[PaneCount] = { tr("Map"), tr("Time zone"), tr("Language") };
    for (int i = 0; i < PaneCount; ++i) {
        const QRectF pill(body.x() + 12 + i * 96.0, body.y() + 22, 92, 20);
        const bool on = (i == m_pane);
        p.setPen(Qt::NoPen);
        p.setBrush(on ? Theme::blue() : Theme::cardLow());
        p.drawRoundedRect(pill, 9, 9);
        p.setFont(Theme::font(11, on));
        p.setPen(on ? Qt::white : Theme::ink2());
        p.drawText(pill, Qt::AlignCenter, names[i]);
    }

    if (m_pane == PaneMap) {
        paintMap(p);
        paintFooter(p, body);
    }

    /* Over whatever is behind it, in every pane, because the question can be
     * raised from the map and answered after a shoulder press. */
    if (m_offer >= 0)
        paintOffer(p, body);
}

void RegionPage::paintMap(QPainter &p)
{
    const QRectF r = mapRect();

    p.setPen(Qt::NoPen);
    p.setBrush(Theme::deskLow());
    p.drawRoundedRect(r, 8, 8);

    p.save();
    QPainterPath clip;
    clip.addRoundedRect(r, 8, 8);
    p.setClipPath(clip);

    /* The graticule, at thirty degrees, and the two lines that are not just a
     * grid drawn a shade brighter: the equator and Greenwich are how somebody
     * reads a coarse map, and they cost two pens. */
    p.setPen(QPen(QColor(255, 255, 255, 16), 1.0));
    for (int lat = -60; lat <= 60; lat += 30) {
        if (lat == 0)
            continue;
        const QPointF a = project(-180, lat, r);
        p.drawLine(QPointF(r.left(), a.y()), QPointF(r.right(), a.y()));
    }
    for (int lon = -150; lon <= 150; lon += 30) {
        if (lon == 0)
            continue;
        const QPointF a = project(lon, 0, r);
        p.drawLine(QPointF(a.x(), r.top()), QPointF(a.x(), r.bottom()));
    }
    p.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
    const QPointF eq = project(0, 0, r);
    p.drawLine(QPointF(r.left(), eq.y()), QPointF(r.right(), eq.y()));
    p.drawLine(QPointF(eq.x(), r.top()), QPointF(eq.x(), r.bottom()));

    /* The land.  Filled and then stroked in one call per outline; see kLand. */
    p.setBrush(Theme::card());
    p.setPen(QPen(Theme::border(), 1.0));
    QPolygonF poly;
    const int points = int(sizeof(kLand) / sizeof(kLand[0]));
    for (int i = 0; i + 1 < points; i += 2) {
        if (kLand[i] == short(kSep)) {
            if (poly.size() > 2)
                p.drawPolygon(poly);
            poly.clear();
            continue;
        }
        poly << project(kLand[i], kLand[i + 1], r);
    }
    if (poly.size() > 2)
        p.drawPolygon(poly);

    /* The dots.  Everything unselected is the same faint mark, because a map
     * where sixty cities compete is a map with no cursor on it. */
    p.setPen(Qt::NoPen);
    for (int i = 0; i < kZoneCount; ++i) {
        if (i == m_cursor || i == m_applied)
            continue;
        const QPointF at = project(kZones[i].lon, kZones[i].lat, r);
        p.setBrush(Theme::ink3());
        p.drawEllipse(at, 1.6, 1.6);
    }

    if (m_applied >= 0 && m_applied < kZoneCount) {
        const QPointF at = project(kZones[m_applied].lon, kZones[m_applied].lat, r);
        p.setBrush(Theme::green());
        p.drawEllipse(at, 3.0, 3.0);
    }

    if (m_cursor >= 0 && m_cursor < kZoneCount) {
        const QPointF at = project(kZones[m_cursor].lon, kZones[m_cursor].lat, r);
        /* Crosshair as well as a ring: a four pixel ring on a busy coastline is
         * hard to find, and two full-width hairlines are impossible to miss. */
        p.setPen(QPen(QColor(10, 132, 255, 90), 1.0));
        p.drawLine(QPointF(r.left(), at.y()), QPointF(r.right(), at.y()));
        p.drawLine(QPointF(at.x(), r.top()), QPointF(at.x(), r.bottom()));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Theme::blue(), 2.0));
        p.drawEllipse(at, 5.0, 5.0);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::blue());
        p.drawEllipse(at, 2.0, 2.0);

        /* The name, in a pill beside the dot and pushed back inside the map when
         * the dot is against an edge -- which Honolulu and Auckland both are. */
        const QString label = QString::fromUtf8(kZones[m_cursor].city);
        const QFont lf = Theme::font(11, true);
        const QFontMetrics lfm(lf);
        const qreal w = lfm.horizontalAdvance(label) + 12;
        qreal x = at.x() + 9;
        qreal y = at.y() - 20;
        if (x + w > r.right() - 4)
            x = at.x() - 9 - w;
        if (y < r.top() + 2)
            y = at.y() + 9;
        const QRectF pill(x, y, w, 17);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 9, 14, 210));
        p.drawRoundedRect(pill, 7, 7);
        p.setFont(lf);
        p.setPen(Theme::ink());
        p.drawText(pill, Qt::AlignCenter, label);
    }

    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void RegionPage::paintFooter(QPainter &p, const QRectF &body)
{
    const QRectF r = mapRect();
    const qreal x = r.x();
    const qreal w = r.width();
    qreal y = r.bottom() + 5;

    if (m_cursor >= 0 && m_cursor < kZoneCount) {
        p.setFont(Theme::font(14, true));
        p.setPen(Theme::ink());
        p.drawText(QRectF(x, y, w, 19), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(kZones[m_cursor].city));
        p.setFont(Theme::font(12));
        p.setPen(Theme::ink2());
        p.drawText(QRectF(x, y, w, 19), Qt::AlignRight | Qt::AlignVCenter, m_cursorInfo);
        y += 19;

        p.setFont(Theme::font(12));
        p.setPen(Theme::ink2());
        p.drawText(QRectF(x, y, w, 17), Qt::AlignLeft | Qt::AlignVCenter,
                   QLatin1String(kZones[m_cursor].zone));
        y += 17;
    }

    /*
     * What is actually in force, and by which of the two mechanisms.
     *
     * The mechanism is on the glass because it is the difference between a
     * setting that outlives the boot and one that does not, and because the
     * failing case -- a rootfs mounted read-only by /init's recovery path -- is
     * invisible from anywhere else on this device.
     */
    QString line;
    if (m_applied >= 0 && m_applied < kZoneCount) {
        line = tr("In force: %1").arg(QLatin1String(kZones[m_applied].zone));
        if (!m_how.isEmpty())
            line += QStringLiteral("  --  ") + m_how;
    } else {
        line = tr("No time zone has been chosen on this device.");
    }
    p.setFont(Theme::font(11));
    p.setPen(Theme::ink3());
    p.drawText(QRectF(x, y, w, 16), Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(line, Qt::ElideMiddle, int(w)));
    Q_UNUSED(body);
}

void RegionPage::paintOffer(QPainter &p, const QRectF &body)
{
    if (m_offer < 0 || m_offer >= Lang::Count)
        return;

    const QRectF bar(body.x() + 10, body.bottom() - 40, body.width() - 20, 32);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 159, 10, 232));            /* Theme::orange(), as glass */
    p.drawRoundedRect(bar, 10, 10);

    p.setFont(Theme::font(12, true));
    p.setPen(QColor(26, 20, 6));
    p.drawText(bar.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter,
               tr("Switch the language to %1?").arg(Strings::nativeName(m_offer)));
    p.setFont(Theme::font(12));
    p.drawText(bar.adjusted(12, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter,
               tr("A yes    B no"));
}
