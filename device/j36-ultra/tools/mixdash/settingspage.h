/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * settingspage.h -- the settings hub, and the three pages under it.
 *
 * FOUR CLASSES IN ONE FILE because they are one screen and its leaves: the hub
 * is a list of destinations with the two things that are genuinely global on it
 * (volume, mute), and the mouse, display and language pages are the settings
 * proper.
 *
 * WHAT THE HUB NO LONGER OPENS, AND WHY.  It used to end in a "System" section
 * that led to Packages, Terminal, Files, Diagnostics and System information, and
 * a "Network" section that led to Wi-Fi.  Every one of those is an Apps card or
 * a dock tab, so every one of those rows was a second door into a room that
 * already had one -- and a second door somebody has to keep in step with the
 * first.  Those are applications, not settings; they live in their own files and
 * they are reached from the grid.  What is left here is the three pages that are
 * settings and are reachable nowhere else.
 *
 * THE HUB DOES NOT OWN THE PAGES IT OPENS.  It emits openRequested() with a
 * destination and the shell pushes whichever page that is.  A hub that held its
 * leaves would mean their timers run whenever the dashboard has ever shown
 * Settings.
 *
 * SOUND IS DRIVEN THROUGH amixer rather than through libasound.  Linking ALSA
 * would put a hard NEEDED entry on a device where the codec is still in bring-up;
 * amixer is in alsa-utils, which this image already stages for the Media page,
 * and if it is missing the row says so instead of the slider silently doing
 * nothing.
 *
 * AND IT IS DRIVEN THROUGH volume.h, not from here.  The plumbing used to be four
 * private methods on this class, which was right while the slider below was the
 * only way to change the volume.  It is not: VOL+ and VOL- on the side of the
 * case work from the Media player and from a full-screen game, and two copies of
 * "which control is the playback control" would be two probes of amixer and a
 * slider showing the level from before the last press.  What is left here is
 * m_volume and m_muted, which are the state of a row and nothing more.
 */
#ifndef MIXDASH_SETTINGSPAGE_H
#define MIXDASH_SETTINGSPAGE_H

#include <QElapsedTimer>
#include <QString>

#include "settings.h"
#include "widgets.h"

class ListPane;

class SettingsPage : public PageWidget
{
    Q_OBJECT

public:
    /*
     * Where a row goes.  Numbered, not pointered, so this header does not have to
     * include every page in the build to say "open the mouse one".
     */
    enum Destination {
        OpenNone = 0,
        OpenMouse,
        OpenDisplay,
        OpenLanguage
    };

    explicit SettingsPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Settings"); }
    bool handleNav(int action) override;
    void onEnter() override;

signals:
    void openRequested(int destination);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void onValueChanged(int index, int value);

private:
    void rebuild();

    ListPane *m_list = nullptr;

    /* What the mixer said the last time this page was entered, which is all these
     * two are: the state of the slider and of the toggle.  The mixer itself lives
     * in volume.h, because the hardware volume keys drive it from pages that are
     * not this one. */
    int m_volume = -1;          /* per cent, -1 for "no control" */
    bool m_muted = false;
    /* Which analog outputs are switched on.  Read on the way in like the two
     * above, and for the same reason: something else may have moved them --
     * alsa-restore at boot, an ssh session, the driver itself refusing to power
     * the amp on a rail that will not hold it. */
    bool m_speaker = false;
    bool m_headphones = false;
    QString m_note;
};

/*
 * The mouse page.
 *
 * Everything here writes straight through to Settings, which Pointer and Joypad
 * read on their next tick -- so a slider being dragged changes the pointer that
 * is dragging it, live.  That is not a trick, it is the only way to tune a
 * pointer speed: a number in pixels per second means nothing until the cursor
 * moves at it.
 *
 * THE TEST PAD at the foot exists for the same reason.  Double click speed is the
 * one setting nobody can pick from a number, so the page counts clicks in a box
 * and shows what it decided the last two presses were.
 */
class MousePage : public PageWidget
{
    Q_OBJECT

public:
    explicit MousePage(QWidget *parent = nullptr);

    QString title() const override { return tr("Mouse and pointer"); }
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private slots:
    void onActivated(int index);
    void onValueChanged(int index, int value);

private:
    enum RowId {
        IdNone = 0,
        IdEnabled,
        IdSpeed,
        IdAccel,
        IdDeadzone,
        IdTracking,
        IdLeftHanded,
        IdDoubleClick,
        IdHide,
        IdReset
    };

    void rebuild();
    void commit();
    QRectF padRect() const;

    ListPane *m_list = nullptr;
    MouseConfig m_cfg;

    /* The test pad's state: what the last press was taken as, and how long after
     * the one before it. */
    int m_clicks = 0;
    int m_doubles = 0;
    qint64 m_gapMs = -1;
    QElapsedTimer m_padClock;
    qint64 m_lastPadMs = -1;
    bool m_padLit = false;
};

/*
 * The display page.
 *
 * IT IS THE ONLY PAGE IN THE DASHBOARD THAT WRITES TO HARDWARE.  Everything else
 * here either changes a number this program itself reads back (the mouse page) or
 * shells out to a tool that owns the device (amixer, wpa_cli, apt).  Brightness
 * has no tool: it is one integer in /sys/class/backlight/<name>/brightness, put
 * there by j36_mt6592_backlight.ko, and the honest thing is to write it.
 *
 * THE SLIDER STOPS WELL ABOVE ZERO.  On this board the panel is the only output
 * there is: no serial header brought out, no second screen, and the recovery for
 * a backlight at duty 0 is to reboot and hope the loader relights it.  So the
 * floor is 5 per cent -- dim enough to be worth having in a dark room, bright
 * enough that the slider that got you there is still readable.  Settings clamps
 * the same range on load, because the INI file is on a partition somebody will
 * eventually edit from a PC.
 *
 * IF THERE IS NO BACKLIGHT DEVICE THE PAGE SAYS SO, in the same words the rest of
 * the dashboard uses for a payload that did not load -- a dead slider that moves
 * and changes nothing is the worst of the three possible answers.
 */
class DisplayPage : public PageWidget
{
    Q_OBJECT

public:
    explicit DisplayPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Display"); }
    bool handleNav(int action) override;
    void onEnter() override;

    /*
     * Push the remembered level back at the panel, once, at startup.
     *
     * Static and callable before anything is on screen, because that is when it
     * has to happen: the driver adopts the loader's duty, so a brightness the
     * user chose yesterday is not applied by anybody unless the shell applies it.
     * Silent by design -- there is no page up yet to put an error on, and the one
     * failure that matters (no backlight at all) is already reported by this page
     * the moment it is opened.
     */
    static void restoreSaved();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void onValueChanged(int index, int value);

private:
    enum RowId {
        IdNone = 0,
        IdBrightness,
        IdFull,
        IdInert
    };

    void rebuild();
    /* Write it, remember it, and say what happened.  One place, so the slider and
     * the Full brightness row cannot drift apart. */
    void applyPercent(int percent);

    ListPane *m_list = nullptr;
    /* Per cent as last read or written; -1 when there is no backlight to read. */
    int m_percent = -1;
    QString m_note;
};

/*
 * The language page: one row per language the strings database carries.
 *
 * EVERY ROW IS WRITTEN IN THE LANGUAGE IT SELECTS, and that is not decoration.
 * This is a handheld with no keyboard and no second screen: somebody who picks
 * the wrong row is looking at a settings hub they cannot read, and the only way
 * back is to recognise the word for their own language in a list.  "Deutsch"
 * next to "German" costs one column and is the difference between a wrong press
 * being a mistake and a wrong press being a reflash.
 *
 * The second column is the English name, for the same reason from the other
 * side: somebody helping over a phone can say "the row that says French".
 */
class LanguagePage : public PageWidget
{
    Q_OBJECT

public:
    explicit LanguagePage(QWidget *parent = nullptr);

    QString title() const override { return tr("Language"); }
    bool handleNav(int action) override;
    void onEnter() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);

private:
    void rebuild();

    ListPane *m_list = nullptr;
};

#endif /* MIXDASH_SETTINGSPAGE_H */
