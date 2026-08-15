/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "settingspage.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QStringList>
#include <QSysInfo>

#include "joypad.h"
#include "stringsdb.h"
#include "theme.h"
#include "volume.h"

namespace {

/* Rows that are not a destination.  Above every Destination value so one `id'
 * field can carry both without a second tag. */
enum { RowVolume = 900, RowMute, RowInert, RowSpeaker, RowHeadphones };

/*
 * The floor the brightness slider will not go below, in per cent.
 *
 * See the note on DisplayPage in the header: the panel is this board's only
 * output, so the setting is not allowed to turn it off.  Five per cent of a
 * 1023-count duty is 51, which the TPS61161 in front of the LED string still
 * drives -- and it is far above the sub-millisecond low it treats as a shutdown
 * request, so dimming this far cannot latch the driver off.
 *
 * It also keeps this out of a fight it would lose.  j36-eglprobe runs before the
 * dashboard on every boot and repairs a backlight it finds at exactly zero, on
 * the grounds that a lit panel with the lamp off is indistinguishable from a
 * dead one.  It is right, and a slider that could store 0 would look like a
 * setting that silently refuses to stick.
 */
const int MinBrightness = 5;

/*
 * The backlight, looked up every time rather than cached.
 *
 * Caching it would be one fewer readdir per rebuild and would be wrong: the
 * module is in the j36.power payload and can be inserted or removed while the
 * dashboard is running, which is exactly what happens while somebody is bringing
 * the driver up.  A page that decided at startup that there was no backlight, and
 * kept saying so after one appeared, would send that person looking for a bug in
 * the kernel.
 */
struct BacklightDevice {
    QString dir;
    QString name;
    int max = 0;

    bool valid() const { return !dir.isEmpty() && max > 0; }
};

BacklightDevice findBacklight()
{
    BacklightDevice bl;

    const QStringList names = QDir(QStringLiteral("/sys/class/backlight"))
                                  .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        const QString dir = "/sys/class/backlight/" + name;
        const int max = SysInfo::readTrimmed(dir + "/max_brightness").toInt();
        /* A zero max_brightness is a device that registered without a usable
         * scale; dividing by it later would be the crash. */
        if (max <= 0)
            continue;

        /* The board's own is taken by name, anything else only because it is the
         * only one there -- a USB display or a debug LED class device turning up
         * first in readdir order must not become "the screen". */
        const bool ours = name.startsWith(QLatin1String("j36"));
        if (ours || !bl.valid()) {
            bl.dir = dir;
            bl.name = name;
            bl.max = max;
        }
        if (ours)
            break;
    }

    return bl;
}

/* Per cent to the driver's own scale, never landing on zero: 1 is the dimmest
 * thing that is still a backlight. */
int percentToRaw(int percent, int max)
{
    return qMax(1, qRound(qBound(0, percent, 100) * max / 100.0));
}

/* -1 for "there is nothing to read", which is not the same as 0 per cent. */
int readBacklightPercent(const BacklightDevice &bl)
{
    if (!bl.valid())
        return -1;
    const QString raw = SysInfo::readTrimmed(bl.dir + "/brightness");
    if (raw.isEmpty())
        return -1;
    return qBound(0, qRound(raw.toInt() * 100.0 / bl.max), 100);
}

/*
 * One integer into one sysfs file.
 *
 * flush() before close() on purpose.  The backlight driver verifies each write by
 * reading the BLS block back and returns -EIO when the block is not taking them,
 * and that error surfaces at write(2) -- which is inside flush(), not inside
 * QFile::write(), because QFile buffers.  Closing without flushing would throw
 * the one signal worth having away.
 */
bool writeSysfs(const QString &path, int value)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    const QByteArray bytes = QByteArray::number(value) + '\n';
    const bool ok = (f.write(bytes) == bytes.size()) && f.flush();
    f.close();
    return ok;
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
    /*
     * Read on the way in, every time, and never cached at this level.  The
     * hardware volume keys work from anywhere and have almost certainly moved the
     * level since this page was last looked at -- a slider showing what the mixer
     * said the last time somebody opened Settings would be a slider that lies.
     */
    Volume::read(&m_volume, &m_muted);
    m_speaker = Volume::isOn(Volume::Speaker);
    m_headphones = Volume::isOn(Volume::Headphones);
    rebuild();
}

/* ── rows ────────────────────────────────────────────────────────────────── */

void SettingsPage::rebuild()
{
    const int keep = m_list->current();
    QVector<ListRow> rows;

    ListRow h;
    h.kind = ListRow::Header;

    h.text = tr("Input");
    rows << h;

    ListRow r;
    r.kind = ListRow::Item;
    r.glyph = GlyphMouse;
    r.accent = Theme::purple();
    r.text = tr("Mouse and pointer");
    r.detail = tr("Speed, tracking, double click, idle");
    r.id = OpenMouse;
    rows << r;

    h.text = tr("Display");
    rows << h;

    /* The current level goes on the hub row rather than only inside the page:
     * "how bright is it" is the question this row is opened to answer, and half
     * the time reading it is the whole errand. */
    const int lit = readBacklightPercent(findBacklight());
    r = ListRow();
    r.kind = ListRow::Item;
    r.glyph = GlyphDisplay;
    r.accent = Theme::yellow();
    r.text = tr("Screen and backlight");
    r.detail = lit < 0
                   ? tr("No backlight device -- the loader owns the brightness")
                   : tr("Brightness %1 %").arg(lit);
    r.id = OpenDisplay;
    rows << r;

    h.text = tr("Sound");
    rows << h;

    const QString ctl = Volume::control();
    if (ctl.isEmpty()) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Volume");
        r.detail = Volume::haveAmixer()
                       ? tr("No playback control -- is a card registered?")
                       : tr("amixer is missing.  Install alsa-utils.");
        r.enabled = false;
        r.id = RowInert;
        rows << r;
    } else {
        r = ListRow();
        r.kind = ListRow::Slider;
        r.text = tr("Volume");
        r.detail = ctl;
        r.minimum = 0;
        r.maximum = 100;
        /* The same step the hardware keys take, so the slider and the rocker
         * cannot land on levels the other one can never reach. */
        r.stepSize = Volume::Step;
        r.value = m_volume < 0 ? 60 : m_volume;
        r.valueText = QString("%1 %").arg(r.value);
        r.accent = Theme::teal();
        r.id = RowVolume;
        rows << r;

        r = ListRow();
        r.kind = ListRow::Toggle;
        r.text = tr("Mute");
        r.on = m_muted;
        r.id = RowMute;
        rows << r;
    }

    /*
     * Where the sound comes out.  Two switches and not one three-way row,
     * because both at once is a real answer -- the amp and the jack are separate
     * outputs off one DAC and the driver will drive both.
     *
     * OUTSIDE THE `else' ABOVE ON PURPOSE.  These are reachable even on a card
     * with no level control at all: routing is not volume, and a board whose
     * playback element failed to register still has a jack somebody may need to
     * switch to.
     *
     * THE DETAIL LINE ON THE JACK IS NOT AN APOLOGY, AND IT NOW HAS THREE THINGS
     * TO SAY.  It used to have one -- "this board brings no jack-detect line out
     * to anything the kernel can read" -- because a person who does not know that
     * will plug headphones in, hear the speaker keep playing, and conclude the
     * jack is broken.  That sentence is still the honest one on a kernel that was
     * never told where the line is, and it is still printed there.  When there IS
     * a line, this says what it currently reads instead, which answers a
     * different and better question: the user who has just plugged headphones in
     * and heard nothing wants to know whether the system saw the plug at all.
     *
     * WHICH MEANS THIS ROW IS NOT ALWAYS A SETTING THE USER OWNS.  With detection
     * on, the shell moves both of these on every plug -- so the speaker row says
     * so rather than letting a toggle that gets overwritten look broken.
     */
    const Volume::JackState jackNow = Volume::jack();

    if (Volume::present(Volume::Speaker)) {
        r = ListRow();
        r.kind = ListRow::Toggle;
        r.text = tr("Speaker");
        if (jackNow == Volume::JackPlugged)
            r.detail = tr("Off while there is something in the jack");
        else if (jackNow == Volume::JackEmpty && !Settings::instance().speakerWanted())
            r.detail = tr("Off by choice -- a plug will not turn it back on");
        r.on = m_speaker;
        r.id = RowSpeaker;
        rows << r;
    }
    if (Volume::present(Volume::Headphones)) {
        r = ListRow();
        r.kind = ListRow::Toggle;
        r.text = tr("Headphones");
        switch (jackNow) {
        case Volume::JackPlugged:
            r.detail = tr("Something is in the jack");
            r.badge = tr("plugged in");
            r.badgeColour = Theme::green();
            break;
        case Volume::JackEmpty:
            r.detail = tr("The jack is empty -- switch to it anyway if you like");
            break;
        case Volume::JackUnknown:
        default:
            r.detail = tr("The jack has no detect line; switch to it here");
            break;
        }
        r.on = m_headphones;
        r.id = RowHeadphones;
        rows << r;
    }

    h.text = tr("Region");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Item;
    r.glyph = GlyphGlobe;
    r.accent = Theme::green();
    r.text = tr("Region & Language");
    /*
     * The language in the language itself, and that is the point: somebody who
     * has landed on a language they cannot read has to be able to find the way
     * out of it by recognising the name of their own.  The zone is beside it
     * because this one row is now the door to both, and because a device whose
     * clock is wrong is a device whose owner is looking for the word "time" --
     * which is not in "Language".
     */
    r.detail = Strings::nativeName(Strings::instance().language());
    {
        const QString zone = Settings::instance().timezone();
        if (!zone.isEmpty())
            r.detail += QStringLiteral("  --  ") + zone;
    }
    r.id = OpenRegion;
    rows << r;

    h.text = tr("About");
    rows << h;

    Settings &s = Settings::instance();
    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("Settings file");
    r.detail = s.path().isEmpty() ? tr("nowhere writable") : s.path();
    r.enabled = false;
    r.id = RowInert;
    if (!s.writable()) {
        r.badge = tr("read-only");
        r.badgeColour = Theme::orange();
    }
    rows << r;

    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("MixOS on J36 Ultra");
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
        m_volume = Volume::setPercent(value);
        ListRow r = rows[index];
        r.valueText = QString("%1 %").arg(m_volume);
        m_list->updateRow(index, r);
        /* Unmute on a deliberate raise: a slider that does nothing because
         * something else is muted is the oldest bug in audio. */
        if (m_muted && value > 0) {
            Volume::setMuted(false);
            m_muted = false;
            for (int i = 0; i < rows.size(); ++i) {
                if (rows[i].id == RowMute) {
                    ListRow t = rows[i];
                    t.on = false;
                    m_list->updateRow(i, t);
                    break;
                }
            }
        }
        m_note = tr("Volume %1 %").arg(m_volume);
        update();
        return;
    }

    if (rows[index].id == RowMute) {
        m_muted = (value != 0);
        Volume::setMuted(m_muted);
        m_note = m_muted ? tr("Muted") : tr("Unmuted");
        update();
        return;
    }

    if (rows[index].id == RowSpeaker || rows[index].id == RowHeadphones) {
        const bool jack = (rows[index].id == RowHeadphones);
        const bool on = (value != 0);

        Volume::setOn(jack ? Volume::Headphones : Volume::Speaker, on);
        if (jack) {
            m_headphones = on;
        } else {
            m_speaker = on;
            /*
             * AND THIS IS WHERE THE INTENTION IS WRITTEN DOWN.  On a board with a
             * detect line the shell switches the speaker off on a plug and back on
             * when the plug comes out -- and "back on" has to mean back to what the
             * user had chosen, not literally on, or somebody who turned the speaker
             * off in a quiet room gets it turned back on by pulling their
             * headphones out.  The mixer cannot hold that: by then it holds what
             * the plug did to it.  See Settings::speakerWanted().
             *
             * Written even while headphones are in, because a person reaching for
             * this row with headphones in is saying what they want the speaker to
             * do, and there is no other reading of it.
             */
            Settings::instance().setSpeakerWanted(on);
        }

        /*
         * Said as where the sound is now rather than as what was just toggled,
         * because that is the question the row was pressed to answer -- and
         * because turning the last one off is worth seeing before the next
         * track starts and nothing happens.
         */
        if (m_speaker && m_headphones)
            m_note = tr("Speaker and headphones");
        else if (m_headphones)
            m_note = tr("Headphones");
        else if (m_speaker)
            m_note = tr("Speaker");
        else
            m_note = tr("No output selected");
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
                             ? tr("A opens, Left and Right change a value.")
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

    h.text = tr("Stick pointer");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Toggle;
    r.text = tr("Right stick moves a pointer");
    r.detail = tr("Off makes it a second D-pad");
    r.on = m_cfg.enabled;
    r.id = IdEnabled;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = tr("Pointer speed");
    r.detail = tr("At full deflection");
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
    r.text = tr("Acceleration");
    r.detail = tr("Bends small movements slower, big ones faster");
    r.minimum = 0;
    r.maximum = 100;
    r.stepSize = 5;
    r.value = m_cfg.acceleration;
    r.valueText = m_cfg.acceleration == 0 ? tr("Linear")
                                          : QString("%1 %").arg(m_cfg.acceleration);
    r.accent = Theme::purple();
    r.id = IdAccel;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = tr("Dead zone");
    r.detail = tr("Ignore this much around centre");
    r.minimum = 2;
    r.maximum = 60;
    r.stepSize = 2;
    r.value = m_cfg.deadzone;
    r.valueText = QString("%1 %").arg(m_cfg.deadzone);
    r.accent = Theme::teal();
    r.id = IdDeadzone;
    rows << r;

    h.text = tr("USB mouse");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = tr("Tracking speed");
    r.detail = tr("Scales what the mouse reports");
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
    r.text = tr("Left-handed");
    r.detail = tr("Swap the two buttons");
    r.on = m_cfg.leftHanded;
    r.id = IdLeftHanded;
    rows << r;

    h.text = tr("Clicking");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = tr("Double click speed");
    r.detail = tr("Two presses inside this are one double click");
    r.minimum = 120;
    r.maximum = 1200;
    r.stepSize = 20;
    r.value = m_cfg.doubleClickMs;
    r.valueText = QString("%1 ms").arg(m_cfg.doubleClickMs);
    r.accent = Theme::orange();
    r.id = IdDoubleClick;
    rows << r;

    h.text = tr("Idle");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Slider;
    r.text = tr("Hide the pointer after");
    r.detail = tr("It comes back the moment the stick moves");
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
    r.text = tr("Reset to defaults");
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
        r.valueText = value == 0 ? tr("Linear")
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
    emit toastRequested(tr("Pointer settings reset"), 1600);
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
                                       : tr("not saved"));

    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter,
               tr("Left and Right change a value.  Changes apply as you make them."));

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
               tr("Click here twice to test the double click speed"));

    QString state;
    if (m_clicks == 0)
        state = tr("nothing yet");
    else if (m_gapMs < 0)
        state = tr("%1 click").arg(m_clicks);
    else
        state = tr("%1 clicks, %2 doubles, last gap %3 ms")
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
                   wasDouble ? tr("counted as a double click")
                             : tr("counted as two clicks"));
    }
}

/* ── the display page ────────────────────────────────────────────────────── */

DisplayPage::DisplayPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    connect(m_list, &ListPane::activated, this, &DisplayPage::onActivated);
    connect(m_list, &ListPane::valueChanged, this, &DisplayPage::onValueChanged);
}

void DisplayPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26);
    QWidget::resizeEvent(event);
}

void DisplayPage::onEnter()
{
    /*
     * Read the hardware rather than trusting what was left in m_percent.  The
     * driver is the owner of that number and this program is not the only thing
     * that can write it: echo into the sysfs file from the Terminal page, and the
     * slider should agree with the panel the next time it is looked at.
     */
    m_percent = readBacklightPercent(findBacklight());
    m_note.clear();
    rebuild();
}

void DisplayPage::rebuild()
{
    const int keep = m_list->current();
    QVector<ListRow> rows;

    ListRow h;
    h.kind = ListRow::Header;

    ListRow r;

    const BacklightDevice bl = findBacklight();

    h.text = tr("Backlight");
    rows << h;

    if (!bl.valid()) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Brightness");
        r.detail = tr("Nothing in /sys/class/backlight -- is j36_mt6592_backlight loaded?");
        r.enabled = false;
        r.id = IdInert;
        rows << r;

        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Why the panel is still lit");
        r.detail = tr("The loader set the duty and nothing has changed it since");
        r.enabled = false;
        r.id = IdInert;
        rows << r;
    } else {
        /* m_percent is -1 only if the read failed on a device that does exist,
         * which leaves the slider somewhere sane rather than at its floor. */
        const int now = qBound(MinBrightness, m_percent < 0 ? 100 : m_percent, 100);

        r = ListRow();
        r.kind = ListRow::Slider;
        r.text = tr("Brightness");
        /* The raw duty is on the row because this is a bring-up: when the slider
         * moves and the panel does not, the next question is always whether the
         * number reached the driver, and this is where that is answered. */
        r.detail = tr("%1, duty %2 of %3")
                       .arg(bl.name)
                       .arg(percentToRaw(now, bl.max))
                       .arg(bl.max);
        r.minimum = MinBrightness;
        r.maximum = 100;
        r.stepSize = 5;
        r.value = now;
        r.valueText = QString("%1 %").arg(now);
        r.accent = Theme::yellow();
        r.id = IdBrightness;
        rows << r;

        r = ListRow();
        r.kind = ListRow::Action;
        r.text = tr("Full brightness");
        r.detail = tr("One press back to 100, for a room brighter than the last one");
        r.accent = Theme::teal();
        r.id = IdFull;
        rows << r;
    }

    h.text = tr("Panel");
    rows << h;

    /*
     * From QScreen and not from /dev/fb0.  What matters on a settings page is the
     * surface this program is drawing into, which under the linuxfb plugin is the
     * framebuffer as Qt understood it -- and if Qt read it differently from the
     * driver, this row is where that shows.  System information opens the device
     * itself and prints the stride and the channel layout; that is the page for
     * the ioctl, this one only has to name the screen.
     */
    const QScreen *screen = QGuiApplication::primaryScreen();
    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("Resolution");
    r.detail = screen ? tr("%1 x %2, %3-bit colour")
                            .arg(screen->geometry().width())
                            .arg(screen->geometry().height())
                            .arg(screen->depth())
                      : tr("Qt reports no screen at all");
    r.enabled = false;
    r.id = IdInert;
    rows << r;

    if (bl.valid()) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Backlight device");
        r.detail = bl.dir;
        r.enabled = false;
        r.id = IdInert;
        rows << r;
    }

    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
}

void DisplayPage::applyPercent(int percent)
{
    const BacklightDevice bl = findBacklight();
    if (!bl.valid()) {
        m_note = tr("There is no backlight device to write to");
        update();
        return;
    }

    const int want = qBound(MinBrightness, percent, 100);
    if (!writeSysfs(bl.dir + "/brightness", percentToRaw(want, bl.max))) {
        /*
         * Said out loud, with the path in it.  The two ways this fails are a
         * dashboard that is not root and a driver whose readback check refused
         * the write, and the difference between them is one ls away -- but only
         * for somebody who knows the write was attempted at all.
         */
        m_note = tr("Cannot write %1/brightness").arg(bl.dir);
        update();
        return;
    }

    m_percent = want;
    /*
     * Remembered on every notch, like the mouse page and for the same reason:
     * this device stops by having its power button held, and a level the user
     * watched themselves choose should not be the one thing that did not survive
     * it.  A QSettings write is a rename on an ext2 partition; the panel takes
     * longer to respond than the file does.
     */
    Settings::instance().setBrightness(want);
    m_note = tr("Brightness %1 %").arg(want);
    update();
}

void DisplayPage::onValueChanged(int index, int value)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size() || rows[index].id != IdBrightness)
        return;

    ListRow r = rows[index];
    applyPercent(value);

    const BacklightDevice bl = findBacklight();
    r.valueText = QString("%1 %").arg(value);
    if (bl.valid())
        r.detail = tr("%1, duty %2 of %3")
                       .arg(bl.name)
                       .arg(percentToRaw(value, bl.max))
                       .arg(bl.max);
    m_list->updateRow(index, r);
}

void DisplayPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    if (rows[index].id != IdFull)
        return;

    applyPercent(100);
    rebuild();
    emit toastRequested(tr("Brightness at full"), 1400);
}

void DisplayPage::restoreSaved()
{
    const int want = Settings::instance().brightness();
    if (want < 0)
        return;

    const BacklightDevice bl = findBacklight();
    if (!bl.valid())
        return;

    const int raw = percentToRaw(qBound(MinBrightness, want, 100), bl.max);
    /* Nothing to do is the common case -- a board that boots at full and was left
     * at full -- and writing anyway would poke the BLS block on every start for
     * no reason. */
    if (SysInfo::readTrimmed(bl.dir + "/brightness").toInt() == raw)
        return;

    writeSysfs(bl.dir + "/brightness", raw);
}

bool DisplayPage::handleNav(int action)
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

void DisplayPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    const QRectF body = paintSheet(p, card, title(),
                                   Settings::instance().writable()
                                       ? QString()
                                       : tr("not saved"));

    const QString line =
        m_note.isEmpty()
            ? tr("Left and Right dim and brighten.  The panel follows as you go.")
            : m_note;
    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}
