/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "settingspage.h"

#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStringList>
#include <QSysInfo>

#include "joypad.h"
#include "theme.h"

namespace {

/* Rows that are not a destination.  Above every Destination value so one `id'
 * field can carry both without a second tag. */
enum { RowVolume = 900, RowMute, RowInert };

QString firstExecutable(const QStringList &paths)
{
    for (const QString &p : paths)
        if (QFileInfo(p).isExecutable())
            return p;
    return QString();
}

QString amixerPath()
{
    static const QString p = firstExecutable(QStringList()
                                             << "/usr/bin/amixer" << "/bin/amixer");
    return p;
}

/*
 * Bounded, and short.  amixer talks to the kernel and returns; if it has not
 * returned in two seconds the card is wedged and waiting longer only moves the
 * freeze from the mixer to the dashboard.
 */
QString runShort(const QString &program, const QStringList &args, int timeoutMs = 2000)
{
    if (program.isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(1000))
        return QString();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(400);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAll());
}

} /* namespace */

/* ── the hub ─────────────────────────────────────────────────────────────── */

SettingsPage::SettingsPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    connect(m_list, &ListPane::activated, this, &SettingsPage::onActivated);
    connect(m_list, &ListPane::valueChanged, this, &SettingsPage::onValueChanged);
}

void SettingsPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26);
    QWidget::resizeEvent(event);
}

void SettingsPage::onEnter()
{
    readMixer();
    rebuild();
}

/* ── sound ───────────────────────────────────────────────────────────────── */

/*
 * Which control to drive.  A board in bring-up has whatever the codec driver
 * happened to register, and the names are not standard: this one may expose
 * "Master", the next only "PCM", an HDMI-only card only "IEC958".  Preferring in
 * order and falling back to "the first one that has a volume" is what every
 * mixer applet does, and is the only thing that works without knowing the card.
 */
QString SettingsPage::mixer()
{
    if (m_mixerProbed)
        return m_mixer;
    m_mixerProbed = true;

    m_haveAmixer = !amixerPath().isEmpty();
    if (!m_haveAmixer)
        return m_mixer;

    const QString out = runShort(amixerPath(), QStringList() << "scontrols");
    QStringList names;
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int a = line.indexOf('\'');
        const int b = line.lastIndexOf('\'');
        if (a >= 0 && b > a)
            names << line.mid(a + 1, b - a - 1);
    }

    QStringList preferred;
    preferred << "Master" << "PCM" << "Speaker" << "Headphone" << "Digital"
              << "DAC" << "Playback";
    for (const QString &want : preferred) {
        if (names.contains(want)) {
            m_mixer = want;
            return m_mixer;
        }
    }
    if (!names.isEmpty())
        m_mixer = names.first();
    return m_mixer;
}

/*
 * -M is the mapped (perceptual) scale.  Raw ALSA volume is a dB register value
 * pretending to be a percentage: at "50%" a raw control is usually already near
 * silent.  Mapped means the middle of the slider sounds like the middle.
 */
void SettingsPage::readMixer()
{
    m_volume = -1;
    m_muted = false;

    const QString ctl = mixer();
    if (ctl.isEmpty())
        return;

    const QString out = runShort(amixerPath(),
                                 QStringList() << "-M" << "get" << ctl);
    if (out.isEmpty())
        return;

    QRegularExpression vol("\\[(\\d{1,3})%\\]");
    const QRegularExpressionMatch m = vol.match(out);
    if (m.hasMatch())
        m_volume = m.captured(1).toInt();

    /* [off] appears once per channel; one is enough to call it muted. */
    if (out.contains("[off]"))
        m_muted = true;
}

void SettingsPage::writeVolume(int percent)
{
    const QString ctl = mixer();
    if (ctl.isEmpty())
        return;
    m_volume = qBound(0, percent, 100);
    runShort(amixerPath(), QStringList() << "-M" << "-q" << "set" << ctl
                                         << QString("%1%").arg(m_volume));
}

void SettingsPage::writeMute(bool muted)
{
    const QString ctl = mixer();
    if (ctl.isEmpty())
        return;
    m_muted = muted;
    /* Not every control has a switch.  amixer says so on stderr and changes
     * nothing, which is the right failure -- the slider still works. */
    runShort(amixerPath(), QStringList() << "-q" << "set" << ctl
                                         << (muted ? "mute" : "unmute"));
}

/* ── rows ────────────────────────────────────────────────────────────────── */

void SettingsPage::rebuild()
{
    const int keep = m_list->current();
    QVector<ListRow> rows;

    ListRow h;
    h.kind = ListRow::Header;

    h.text = QStringLiteral("Input");
    rows << h;

    ListRow r;
    r.kind = ListRow::Item;
    r.glyph = GlyphMouse;
    r.accent = Theme::purple();
    r.text = QStringLiteral("Mouse and pointer");
    r.detail = QStringLiteral("Speed, tracking, double click, idle");
    r.id = OpenMouse;
    rows << r;

    h.text = QStringLiteral("Network");
    rows << h;

    const QString iface = SysInfo::wirelessInterface();
    r = ListRow();
    r.kind = ListRow::Item;
    r.glyph = GlyphWifi;
    r.accent = Theme::blue();
    r.text = QStringLiteral("Wi-Fi");
    r.detail = iface.isEmpty()
                   ? QStringLiteral("No wireless interface found")
                   : QString("Scan and join, on %1").arg(iface);
    r.id = OpenWifi;
    rows << r;

    h.text = QStringLiteral("Sound");
    rows << h;

    const QString ctl = mixer();
    if (ctl.isEmpty()) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = QStringLiteral("Volume");
        r.detail = m_haveAmixer
                       ? QStringLiteral("No playback control -- is a card registered?")
                       : QStringLiteral("amixer is missing.  Install alsa-utils.");
        r.enabled = false;
        r.id = RowInert;
        rows << r;
    } else {
        r = ListRow();
        r.kind = ListRow::Slider;
        r.text = QStringLiteral("Volume");
        r.detail = ctl;
        r.minimum = 0;
        r.maximum = 100;
        r.stepSize = 5;
        r.value = m_volume < 0 ? 60 : m_volume;
        r.valueText = QString("%1 %").arg(r.value);
        r.accent = Theme::teal();
        r.id = RowVolume;
        rows << r;

        r = ListRow();
        r.kind = ListRow::Toggle;
        r.text = QStringLiteral("Mute");
        r.on = m_muted;
        r.id = RowMute;
        rows << r;
    }

    h.text = QStringLiteral("System");
    rows << h;

    struct Dest {
        const char *text;
        const char *detail;
        int glyph;
        QColor accent;
        int id;
    };
    const Dest dests[] = {
        { "Packages", "Install anything Debian has", GlyphPackage, Theme::green(),
          OpenPackages },
        { "Terminal", "A shell on the glass", GlyphTerminal, Theme::orange(),
          OpenTerminal },
        { "Files", "Browse the card and the rootfs", GlyphFiles, Theme::yellow(),
          OpenFiles },
        { "Diagnostics", "Display, GPU, input, sound, USB, power", GlyphChip,
          Theme::pink(), OpenDiagnostics },
        { "System information", "CPU, memory, disks, USB, network", GlyphInfo,
          Theme::ink3(), OpenSystem }
    };
    for (uint i = 0; i < sizeof(dests) / sizeof(dests[0]); ++i) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = QString::fromLatin1(dests[i].text);
        r.detail = QString::fromLatin1(dests[i].detail);
        r.glyph = dests[i].glyph;
        r.accent = dests[i].accent;
        r.id = dests[i].id;
        rows << r;
    }

    h.text = QStringLiteral("About");
    rows << h;

    Settings &s = Settings::instance();
    r = ListRow();
    r.kind = ListRow::Item;
    r.text = QStringLiteral("Settings file");
    r.detail = s.path().isEmpty() ? QStringLiteral("nowhere writable") : s.path();
    r.enabled = false;
    r.id = RowInert;
    if (!s.writable()) {
        r.badge = QStringLiteral("read-only");
        r.badgeColour = Theme::orange();
    }
    rows << r;

    r = ListRow();
    r.kind = ListRow::Item;
    r.text = QStringLiteral("MixOS on J36 Ultra");
    r.detail = QString("Linux %1, %2")
                   .arg(QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture());
    r.enabled = false;
    r.id = RowInert;
    rows << r;

    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
}

void SettingsPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    const int id = rows[index].id;
    if (id <= OpenNone || id >= RowVolume)
        return;
    emit openRequested(id);
}

void SettingsPage::onValueChanged(int index, int value)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;

    if (rows[index].id == RowVolume) {
        writeVolume(value);
        ListRow r = rows[index];
        r.valueText = QString("%1 %").arg(m_volume);
        m_list->updateRow(index, r);
        /* Unmute on a deliberate raise: a slider that does nothing because
         * something else is muted is the oldest bug in audio. */
        if (m_muted && value > 0) {
            writeMute(false);
            for (int i = 0; i < rows.size(); ++i) {
                if (rows[i].id == RowMute) {
                    ListRow t = rows[i];
                    t.on = false;
                    m_list->updateRow(i, t);
                    break;
                }
            }
        }
        m_note = QString("Volume %1 %").arg(m_volume);
        update();
        return;
    }

    if (rows[index].id == RowMute) {
        writeMute(value != 0);
        m_note = m_muted ? QStringLiteral("Muted") : QStringLiteral("Unmuted");
        update();
    }
}

bool SettingsPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        m_list->step(-1);
        return true;
    case Joypad::NavDown:
        m_list->step(1);
        return true;
    case Joypad::NavLeft:
        return m_list->adjust(-1);
    case Joypad::NavRight:
        return m_list->adjust(1);
    case Joypad::NavOk:
        return m_list->press();
    default:
        break;
    }
    return false;
}

void SettingsPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, title());

    const QString line = m_note.isEmpty()
                             ? QStringLiteral("A opens, Left and Right change a value.")
                             : m_note;
    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}

/* ── the mouse page ──────────────────────────────────────────────────────── */

MousePage::MousePage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    connect(m_list, &ListPane::activated, this, &MousePage::onActivated);
    connect(m_list, &ListPane::valueChanged, this, &MousePage::onValueChanged);

    m_padClock.start();
    m_cfg = Settings::instance().mouse();
}

void MousePage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    /* The list stops short of the foot: the test pad lives there and a child
     * widget over it would eat the clicks it exists to count. */
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26 - 56);
    QWidget::resizeEvent(event);
}

void MousePage::onEnter()
{
    m_cfg = Settings::instance().mouse();
    m_clicks = 0;
    m_doubles = 0;
    m_gapMs = -1;
    m_lastPadMs = -1;
    rebuild();
}

void MousePage::onLeave()
{
    commit();
}

void MousePage::commit()
{
    Settings::instance().setMouse(m_cfg);
}

void MousePage::rebuild()
{
    const int keep = m_list->current();
    QVector<ListRow> rows;

    ListRow h;
    h.kind = ListRow::Header;

    ListRow r;

    h.text = QStringLiteral("Stick pointer");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Toggle;
    r.text = QStringLiteral("Right stick moves a pointer");
    r.detail = QStringLiteral("Off makes it a second D-pad");
    r.on = m_cfg.enabled;
    r.id = IdEnabled;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Pointer speed");
    r.detail = QStringLiteral("At full deflection");
    r.minimum = 80;
    r.maximum = 2400;
    r.stepSize = 40;
    r.value = m_cfg.pointerSpeed;
    r.valueText = QString("%1 px/s").arg(m_cfg.pointerSpeed);
    r.accent = Theme::purple();
    r.id = IdSpeed;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Acceleration");
    r.detail = QStringLiteral("Bends small movements slower, big ones faster");
    r.minimum = 0;
    r.maximum = 100;
    r.stepSize = 5;
    r.value = m_cfg.acceleration;
    r.valueText = m_cfg.acceleration == 0 ? QStringLiteral("Linear")
                                          : QString("%1 %").arg(m_cfg.acceleration);
    r.accent = Theme::purple();
    r.id = IdAccel;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Dead zone");
    r.detail = QStringLiteral("Ignore this much around centre");
    r.minimum = 2;
    r.maximum = 60;
    r.stepSize = 2;
    r.value = m_cfg.deadzone;
    r.valueText = QString("%1 %").arg(m_cfg.deadzone);
    r.accent = Theme::teal();
    r.id = IdDeadzone;
    rows << r;

    h.text = QStringLiteral("USB mouse");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Tracking speed");
    r.detail = QStringLiteral("Scales what the mouse reports");
    r.minimum = 10;
    r.maximum = 400;
    r.stepSize = 5;
    r.value = m_cfg.trackingSpeed;
    r.valueText = QString("%1 %").arg(m_cfg.trackingSpeed);
    r.accent = Theme::blue();
    r.id = IdTracking;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Toggle;
    r.text = QStringLiteral("Left-handed");
    r.detail = QStringLiteral("Swap the two buttons");
    r.on = m_cfg.leftHanded;
    r.id = IdLeftHanded;
    rows << r;

    h.text = QStringLiteral("Clicking");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Double click speed");
    r.detail = QStringLiteral("Two presses inside this are one double click");
    r.minimum = 120;
    r.maximum = 1200;
    r.stepSize = 20;
    r.value = m_cfg.doubleClickMs;
    r.valueText = QString("%1 ms").arg(m_cfg.doubleClickMs);
    r.accent = Theme::orange();
    r.id = IdDoubleClick;
    rows << r;

    h.text = QStringLiteral("Idle");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = QStringLiteral("Hide the pointer after");
    r.detail = QStringLiteral("It comes back the moment the stick moves");
    r.minimum = 1;
    r.maximum = 60;
    r.stepSize = 1;
    r.value = m_cfg.hideSeconds;
    r.valueText = QString("%1 s").arg(m_cfg.hideSeconds);
    r.accent = Theme::yellow();
    r.id = IdHide;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Action;
    r.text = QStringLiteral("Reset to defaults");
    r.accent = Theme::red();
    r.id = IdReset;
    rows << r;

    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
}

void MousePage::onValueChanged(int index, int value)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;

    ListRow r = rows[index];
    switch (r.id) {
    case IdEnabled:
        m_cfg.enabled = value != 0;
        break;
    case IdSpeed:
        m_cfg.pointerSpeed = value;
        r.valueText = QString("%1 px/s").arg(value);
        break;
    case IdAccel:
        m_cfg.acceleration = value;
        r.valueText = value == 0 ? QStringLiteral("Linear")
                                 : QString("%1 %").arg(value);
        break;
    case IdDeadzone:
        m_cfg.deadzone = value;
        r.valueText = QString("%1 %").arg(value);
        break;
    case IdTracking:
        m_cfg.trackingSpeed = value;
        r.valueText = QString("%1 %").arg(value);
        break;
    case IdLeftHanded:
        m_cfg.leftHanded = value != 0;
        break;
    case IdDoubleClick:
        m_cfg.doubleClickMs = value;
        r.valueText = QString("%1 ms").arg(value);
        break;
    case IdHide:
        m_cfg.hideSeconds = value;
        r.valueText = QString("%1 s").arg(value);
        break;
    default:
        return;
    }

    m_list->updateRow(index, r);
    /*
     * Written through on every notch rather than on leaving the page.  Pointer
     * and Joypad read Settings on their next tick, so this is what makes a slider
     * change the cursor that is dragging it -- and it is a QSettings write to a
     * file on an ext2 partition, once per notch, which is nothing.
     */
    commit();
    update();
}

void MousePage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    if (rows[index].id != IdReset)
        return;

    m_cfg = MouseConfig();
    commit();
    rebuild();
    emit toastRequested(QStringLiteral("Pointer settings reset"), 1600);
}

bool MousePage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        m_list->step(-1);
        return true;
    case Joypad::NavDown:
        m_list->step(1);
        return true;
    case Joypad::NavLeft:
        return m_list->adjust(-1);
    case Joypad::NavRight:
        return m_list->adjust(1);
    case Joypad::NavOk:
        return m_list->press();
    default:
        break;
    }
    return false;
}

/* ── the test pad ────────────────────────────────────────────────────────── */

QRectF MousePage::padRect() const
{
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    return QRectF(card.x() + 12, card.bottom() - 56, card.width() - 24, 44);
}

void MousePage::mousePressEvent(QMouseEvent *event)
{
    if (!padRect().contains(event->pos())) {
        PageWidget::mousePressEvent(event);
        return;
    }

    const qint64 now = m_padClock.elapsed();
    m_gapMs = m_lastPadMs < 0 ? -1 : now - m_lastPadMs;
    m_lastPadMs = now;
    ++m_clicks;
    m_padLit = true;
    update();
    event->accept();
}

void MousePage::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_padLit) {
        m_padLit = false;
        update();
    }
    PageWidget::mouseReleaseEvent(event);
}

/*
 * Qt sends press, release, DOUBLE CLICK -- the double click does not replace the
 * second press, it follows it.  So the counter above has already counted two and
 * this only records that the pair was close enough, which is exactly the thing
 * the slider above is for.
 */
void MousePage::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!padRect().contains(event->pos())) {
        PageWidget::mouseDoubleClickEvent(event);
        return;
    }
    ++m_doubles;
    update();
    event->accept();
}

void MousePage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, title(),
                                   Settings::instance().writable()
                                       ? QString()
                                       : QStringLiteral("not saved"));

    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Left and Right change a value.  Changes apply as you make them."));

    /* The pad. */
    const QRectF pad = padRect();
    QColor fill = m_padLit ? Theme::blueLow() : Theme::cardLow();
    Theme::vgrad(p, pad, fill.lighter(112), fill, 10.0);
    p.setPen(QPen(Theme::border(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(pad.adjusted(0.5, 0.5, -0.5, -0.5), 10.0, 10.0);

    p.setFont(Theme::font(12, true));
    p.setPen(Theme::ink());
    p.drawText(QRectF(pad.x() + 12, pad.y() + 4, pad.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Click here twice to test the double click speed"));

    QString state;
    if (m_clicks == 0)
        state = QStringLiteral("nothing yet");
    else if (m_gapMs < 0)
        state = QString("%1 click").arg(m_clicks);
    else
        state = QString("%1 clicks, %2 doubles, last gap %3 ms")
                    .arg(m_clicks).arg(m_doubles).arg(m_gapMs);

    p.setFont(Theme::font(11));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(pad.x() + 12, pad.y() + 22, pad.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, state);

    if (m_gapMs >= 0) {
        const bool wasDouble = m_gapMs <= m_cfg.doubleClickMs;
        p.setPen(wasDouble ? Theme::green() : Theme::ink3());
        p.drawText(QRectF(pad.x() + 12, pad.y() + 22, pad.width() - 24, 18),
                   Qt::AlignRight | Qt::AlignVCenter,
                   wasDouble ? QStringLiteral("counted as a double click")
                             : QStringLiteral("counted as two clicks"));
    }
}
