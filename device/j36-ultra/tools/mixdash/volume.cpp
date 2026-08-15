/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "volume.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include "shell.h"
#include "theme.h"

namespace {

QString firstExecutable(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

QString amixerPath()
{
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/bin/amixer") << QStringLiteral("/bin/amixer");
    static const QString p = firstExecutable(paths);
    return p;
}

/*
 * Bounded, and short.  Lifted verbatim from settingspage.cpp, which no longer has
 * a copy: amixer talks to the kernel and returns, and if it has not returned in
 * two seconds the card is wedged -- waiting longer only moves the freeze from the
 * mixer to the dashboard.
 */
QString runShort(const QString &program, const QStringList &args, int timeoutMs = 2000)
{
    if (program.isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!Shell::waitForStarted(p, 1000))
        return QString();
    if (!Shell::waitForFinished(p, timeoutMs)) {
        p.kill();
        Shell::waitForFinished(p, 400);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAll());
}

/* The probe, and the answers it produces.  File-scope rather than function
 * statics so invalidate() has something to clear. */
bool g_probed = false;
bool g_haveAmixer = false;
QString g_control;
/* The two output switches, by the simple-control name amixer gives them.  Empty
 * means this card does not have that one. */
QString g_speakerCtl;
QString g_headphoneCtl;

QString outputControl(Volume::Output which)
{
    return which == Volume::Speaker ? g_speakerCtl : g_headphoneCtl;
}

/*
 * The last level this program set or read, and how long ago.  Only nudge() reads
 * it, and only inside kFreshMs -- see the note on nudge() in volume.h for why a
 * held key cannot afford to ask amixer twice per repeat.
 *
 * A second and a half is chosen against the repeat interval and not against how
 * long a mixer stays put: 90 ms between repeats means the window has to cover a
 * gap of several of them so a burst of presses is one read, and it has to be
 * short enough that the next deliberate press -- a second or two later, after
 * something else may have moved the mixer -- goes and looks again.
 */
const int kFreshMs = 1500;
int g_level = -1;
bool g_muted = false;
QElapsedTimer g_stamp;

void remember(int level, bool muted)
{
    g_level = level;
    g_muted = muted;
    /* start() and not restart(): restart() returns the elapsed time of a timer
     * that may never have been started, which is a read of an uninitialised
     * value for a number nobody wants. */
    g_stamp.start();
}

bool recall(int *level, bool *muted)
{
    if (g_level < 0 || !g_stamp.isValid() || g_stamp.elapsed() > kFreshMs)
        return false;
    *level = g_level;
    *muted = g_muted;
    return true;
}

void probe()
{
    if (g_probed)
        return;
    g_probed = true;

    g_haveAmixer = !amixerPath().isEmpty();
    if (!g_haveAmixer)
        return;

    const QString out = runShort(amixerPath(), QStringList() << QStringLiteral("scontrols"));
    QStringList names;
    const QStringList lines = out.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int a = line.indexOf(QLatin1Char('\''));
        const int b = line.lastIndexOf(QLatin1Char('\''));
        if (a >= 0 && b > a)
            names << line.mid(a + 1, b - a - 1);
    }

    /*
     * Preferring in order and falling back to "the first one there is" is what
     * every mixer applet does, and it is the only thing that works without
     * knowing the card: a board in bring-up has whatever the codec driver
     * happened to register, and the names are not standard.  This one may expose
     * "Master", the next only "PCM", an HDMI-only card only "IEC958".
     */
    static const QStringList preferred = QStringList()
        << QStringLiteral("Master") << QStringLiteral("PCM")
        << QStringLiteral("Speaker") << QStringLiteral("Headphone")
        << QStringLiteral("Digital") << QStringLiteral("DAC")
        << QStringLiteral("Playback");

    /*
     * The output switches, found in the same pass as the level -- one fork of
     * amixer for all three, which matters because this runs at startup before
     * anything is on screen.
     *
     * Taken by name and not by "has a switch and no volume", because the names
     * are the contract: j36_mt6592_audio registers "Speaker Amp Switch" and
     * "Headphone Switch" and amixer's simple layer strips the suffix.  A card
     * that happens to have a control called Headphone with a level on it -- most
     * desktop codecs -- is not this, and setOn() on it would mute somebody's
     * output rather than route it.  So both have to be switch-only.
     */
    for (const QString &name : names) {
        if (name != QLatin1String("Speaker Amp") && name != QLatin1String("Headphone"))
            continue;
        const QString caps = runShort(amixerPath(),
                                      QStringList() << QStringLiteral("get") << name);
        if (!caps.contains(QLatin1String("pswitch")) || caps.contains(QLatin1String("pvolume")))
            continue;
        if (name == QLatin1String("Headphone"))
            g_headphoneCtl = name;
        else
            g_speakerCtl = name;
    }

    for (const QString &want : preferred) {
        if (names.contains(want)) {
            g_control = want;
            return;
        }
    }
    if (!names.isEmpty())
        g_control = names.first();
}

} /* namespace */

QString Volume::control()
{
    probe();
    return g_control;
}

bool Volume::haveAmixer()
{
    probe();
    return g_haveAmixer;
}

void Volume::invalidate()
{
    g_probed = false;
    g_haveAmixer = false;
    g_control.clear();
    g_speakerCtl.clear();
    g_headphoneCtl.clear();
    /* The remembered level goes with it: it was a level on the control that has
     * just been declared unknown. */
    g_level = -1;
    g_muted = false;
    g_stamp.invalidate();
}

bool Volume::read(int *percent, bool *muted)
{
    if (percent)
        *percent = -1;
    if (muted)
        *muted = false;

    const QString ctl = control();
    if (ctl.isEmpty())
        return false;

    const QString out = runShort(amixerPath(),
                                 QStringList() << QStringLiteral("-M")
                                               << QStringLiteral("get") << ctl);
    if (out.isEmpty())
        return false;

    static const QRegularExpression vol(QStringLiteral("\\[(\\d{1,3})%\\]"));
    const QRegularExpressionMatch m = vol.match(out);
    const int pct = m.hasMatch() ? m.captured(1).toInt() : -1;
    /* [off] appears once per channel; one is enough to call it muted. */
    const bool off = out.contains(QLatin1String("[off]"));

    if (percent)
        *percent = pct;
    if (muted)
        *muted = off;

    remember(pct, off);
    return pct >= 0;
}

int Volume::setPercent(int value)
{
    const QString ctl = control();
    if (ctl.isEmpty())
        return -1;

    const int want = qBound(0, value, 100);
    runShort(amixerPath(), QStringList() << QStringLiteral("-M") << QStringLiteral("-q")
                                         << QStringLiteral("set") << ctl
                                         << QStringLiteral("%1%").arg(want));
    remember(want, g_muted);
    return want;
}

void Volume::setMuted(bool value)
{
    const QString ctl = control();
    if (ctl.isEmpty())
        return;
    /* Not every control has a switch.  amixer says so on stderr and changes
     * nothing, which is the right failure -- the level still works. */
    runShort(amixerPath(), QStringList() << QStringLiteral("-q")
                                         << QStringLiteral("set") << ctl
                                         << (value ? QStringLiteral("mute")
                                                   : QStringLiteral("unmute")));
    remember(g_level, value);
}

bool Volume::present(Output which)
{
    probe();
    return !outputControl(which).isEmpty();
}

bool Volume::isOn(Output which)
{
    probe();
    const QString ctl = outputControl(which);
    if (ctl.isEmpty())
        return false;

    const QString out = runShort(amixerPath(),
                                 QStringList() << QStringLiteral("get") << ctl);
    /* [on] appears once per channel and these are mono, but the test is written
     * as "any channel is on" for the same reason read() writes mute as "any
     * channel is off": half an output is on. */
    return out.contains(QLatin1String("[on]"));
}

void Volume::setOn(Output which, bool on)
{
    probe();
    const QString ctl = outputControl(which);
    if (ctl.isEmpty())
        return;

    runShort(amixerPath(), QStringList() << QStringLiteral("-q")
                                         << QStringLiteral("set") << ctl
                                         << (on ? QStringLiteral("on")
                                                : QStringLiteral("off")));
}

int Volume::nudge(int delta, bool *mutedOut)
{
    const QString ctl = control();
    if (ctl.isEmpty()) {
        if (mutedOut)
            *mutedOut = false;
        return -1;
    }

    int now = -1;
    bool off = false;
    /* The one place a remembered answer is allowed.  See the note in volume.h on
     * why a held key cannot afford the fork. */
    if (!recall(&now, &off) && !read(&now, &off))
        return -1;
    if (now < 0)
        return -1;

    /*
     * Rounded to the step on the way, so a level some other program left at 43
     * comes back as 45 and then 50 rather than 48 and 53.  The bar is drawn in
     * steps of five and a level that is not one of them looks like a rounding
     * error on the glass.
     */
    int want = ((now + Step / 2) / Step) * Step + (delta > 0 ? Step : -Step);
    want = qBound(0, want, 100);
    /* At the ends the rounding above can land back on the level we started from,
     * and a key that does nothing reads as a key that is not wired up.  Step by
     * one from there so the bar always moves until it genuinely cannot. */
    if (want == now)
        want = qBound(0, now + (delta > 0 ? 1 : -1), 100);

    const int applied = setPercent(want);

    /*
     * Raising the volume unmutes -- once, on the press that was actually muted.
     * Anything else means the first three presses of VOL+ on a muted board move a
     * bar and produce no sound, which is the single most confusing thing a volume
     * key can do.  The `off' test is what keeps a held key from forking amixer a
     * second time on every one of the ninety-millisecond repeats after it.
     *
     * Lowering deliberately does NOT mute at zero: a control muted behind the
     * user's back is one they have to find the Settings page to undo.
     */
    if (delta > 0 && off && applied > 0) {
        setMuted(false);
        off = false;
    }

    if (mutedOut)
        *mutedOut = off;
    return applied;
}

/* ── the bar ─────────────────────────────────────────────────────────────── */

VolumeOverlay::VolumeOverlay(QWidget *parent)
    : QWidget(parent)
{
    /*
     * No background of its own, so the page underneath is repainted before this
     * paints over it at the chrome's alpha -- the same bargain StatusBar strikes,
     * and what makes the panel read as glass rather than as a grey box.
     */
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    hide();

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    /* Three seconds, which is what was asked for and is also about right: long
     * enough to read after the last press of a burst, short enough that it is
     * gone before it becomes part of the picture. */
    m_timer->setInterval(3000);
    connect(m_timer, &QTimer::timeout, this, &QWidget::hide);
}

void VolumeOverlay::placeIn(const QRect &panel)
{
    /* Fifty-four and not the forty the track needs: the width is set by the
     * no-card message, which is three words and has to be legible in six
     * languages.  See paintEvent. */
    const int w = 54;
    const int h = qMin(240, qMax(140, panel.height() / 2));
    /* Right-hand edge, vertically centred.  Right because the left of this panel
     * is where every page puts its list, and centred because the status bar owns
     * the top and the dock owns the bottom. */
    setGeometry(panel.right() - w - Theme::Margin - 4,
                panel.y() + (panel.height() - h) / 2, w, h);
}

void VolumeOverlay::flash(int value, bool isMuted)
{
    m_value = value;
    m_muted = isMuted;

    /* raise() on every flash, not only on the first: the shell shows and hides
     * pages by stacking order, so a page entered since the last flash would
     * otherwise be painted on top of this. */
    raise();
    show();
    update();
    /* start() on a running single-shot timer restarts it, which is exactly the
     * behaviour wanted while a key is held: three seconds from the LAST press. */
    m_timer->start();
}

void VolumeOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);

    QColor sheet = Theme::window();
    sheet.setAlpha(Theme::ChromeAlpha);
    Theme::softShadow(p, box, Theme::Radius);
    p.setPen(Qt::NoPen);
    p.setBrush(sheet);
    p.drawRoundedRect(box, Theme::Radius, Theme::Radius);
    p.setPen(QPen(Theme::border(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box, Theme::Radius, Theme::Radius);

    /* The glyph at the bottom, the track above it, the number at the top. */
    const qreal pad = 9.0;
    const QRectF icon(box.x() + pad, box.bottom() - pad - 22.0,
                      box.width() - 2 * pad, 22.0);
    const QRectF label(box.x() + pad, box.y() + pad, box.width() - 2 * pad, 16.0);
    const QRectF track(box.center().x() - 5.0, label.bottom() + 8.0,
                       10.0, icon.top() - label.bottom() - 16.0);

    if (m_value < 0) {
        /*
         * No control.  It says so rather than painting an empty track, because an
         * empty track is indistinguishable from a volume of zero -- and on this
         * board "there is no sound card" and "the volume is down" are two
         * genuinely different problems with two different fixes.
         */
        const QRectF area = box.adjusted(3, 0, -3, 0);
        const QString msg = tr("no\nsound\ncard");

        /*
         * The size is chosen against the longest word instead of fixed, because
         * this bar is fifty pixels wide and the string is three words in six
         * languages -- "nessuna", "tarjeta", "Soundkarte".  Qt wraps at spaces and
         * newlines and at nothing else, so a word wider than the box is not
         * wrapped, it is cut in half, and half a word is worse than a small one.
         */
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        const QStringList words = msg.split(ws, Qt::SkipEmptyParts);
        int px = 12;
        while (px > 8) {
            const QFontMetrics fm(Theme::font(px, true));
            bool fits = true;
            for (const QString &w : words) {
                if (fm.horizontalAdvance(w) > area.width()) {
                    fits = false;
                    break;
                }
            }
            if (fits)
                break;
            --px;
        }

        p.setFont(Theme::font(px, true));
        p.setPen(Theme::ink2());
        p.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, msg);
        return;
    }

    p.setFont(Theme::font(13, true));
    p.setPen(m_muted ? Theme::ink3() : Theme::ink());
    p.drawText(label, Qt::AlignCenter, QStringLiteral("%1").arg(m_value));

    /* The track. */
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::deskLow());
    p.drawRoundedRect(track, 5.0, 5.0);

    const qreal filled = track.height() * (m_value / 100.0);
    if (filled > 0.0) {
        const QRectF bar(track.x(), track.bottom() - filled, track.width(), filled);
        /*
         * Grey while muted rather than hidden.  The level is still there and the
         * next VOL+ restores the sound at it; a bar that emptied on mute would say
         * the level had been lost.
         */
        if (m_muted) {
            p.setBrush(Theme::ink3());
            p.drawRoundedRect(bar, 5.0, 5.0);
        } else {
            /* Blue up to 100, and orange only where the last quarter is: it is a
             * hint that this is loud, not a warning, so it colours the top of the
             * bar rather than the whole of it. */
            Theme::vgrad(p, bar, m_value > 75 ? Theme::orange() : Theme::blue(),
                         Theme::blueLow(), 5.0);
        }
    }

    /* The speaker, drawn rather than glyphed: Glyph has no speaker in it, and
     * appending one to that enum for a single 22-pixel icon would put a value in
     * a table every AppEntry literal in dashboard.cpp indexes. */
    const QColor ink = m_muted ? Theme::ink3() : Theme::ink();
    const qreal cx = icon.center().x();
    const qreal cy = icon.center().y();

    QPainterPath cone;
    cone.moveTo(cx - 7.0, cy - 2.5);
    cone.lineTo(cx - 3.5, cy - 2.5);
    cone.lineTo(cx + 0.5, cy - 7.0);
    cone.lineTo(cx + 0.5, cy + 7.0);
    cone.lineTo(cx - 3.5, cy + 2.5);
    cone.lineTo(cx - 7.0, cy + 2.5);
    cone.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPath(cone);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap));
    if (m_muted) {
        /* One stroke through it.  Two arcs plus a cross is more ink than a
         * 22-pixel icon has room for. */
        p.drawLine(QPointF(cx + 2.5, cy - 5.0), QPointF(cx + 8.0, cy + 5.0));
        p.drawLine(QPointF(cx + 8.0, cy - 5.0), QPointF(cx + 2.5, cy + 5.0));
    } else {
        p.drawArc(QRectF(cx - 2.0, cy - 5.0, 7.0, 10.0), -70 * 16, 140 * 16);
        if (m_value > 50)
            p.drawArc(QRectF(cx - 3.0, cy - 8.5, 12.0, 17.0), -70 * 16, 140 * 16);
    }
}
