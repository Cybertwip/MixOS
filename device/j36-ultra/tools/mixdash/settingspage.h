/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * settingspage.h -- the settings hub, and the two pages under it.
 *
 * THREE CLASSES IN ONE FILE because they are one screen and its leaves: the hub
 * is a list of destinations with the two things that are genuinely global on it
 * (volume, mute), and the mouse and display pages are the settings proper.
 * Wi-Fi, Packages and Diagnostics are reached FROM here but are not settings --
 * they are applications, and they live in their own files.
 *
 * THE HUB DOES NOT OWN THE PAGES IT OPENS.  It emits openRequested() with a
 * destination and the shell pushes whichever page that is.  A hub that held a
 * WifiPage and a PackagesPage would mean the Wi-Fi scanner runs whenever the
 * dashboard has ever shown Settings, and would make the same page reachable from
 * two owners once the Apps grid gained a Wi-Fi card -- which it has.
 *
 * SOUND IS DRIVEN THROUGH amixer rather than through libasound.  Linking ALSA
 * would put a hard NEEDED entry on a device where the codec is still in bring-up;
 * amixer is in alsa-utils, which this image already stages for the Media page,
 * and if it is missing the row says so instead of the slider silently doing
 * nothing.
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
     * include every page in the build to say "open the Wi-Fi one".
     */
    enum Destination {
        OpenNone = 0,
        OpenMouse,
        OpenWifi,
        OpenDiagnostics,
        OpenPackages,
        OpenTerminal,
        OpenSystem,
        OpenFiles,
        OpenMedia,
        OpenDisplay
    };

    explicit SettingsPage(QWidget *parent = nullptr);

    QString title() const override { return QStringLiteral("Settings"); }
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

    /* The first ALSA playback control that exists, cached.  Empty means either no
     * amixer or no card, which the rows distinguish between. */
    QString mixer();
    void readMixer();
    void writeVolume(int percent);
    void writeMute(bool muted);

    ListPane *m_list = nullptr;

    QString m_mixer;
    bool m_mixerProbed = false;
    bool m_haveAmixer = false;
    int m_volume = -1;          /* per cent, -1 for "no control" */
    bool m_muted = false;
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

    QString title() const override { return QStringLiteral("Mouse and pointer"); }
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

    QString title() const override { return QStringLiteral("Display"); }
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

#endif /* MIXDASH_SETTINGSPAGE_H */
