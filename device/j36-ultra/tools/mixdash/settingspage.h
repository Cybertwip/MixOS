/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * settingspage.h -- the settings hub, and the mouse page under it.
 *
 * TWO CLASSES IN ONE FILE because they are one screen split in two: the hub is a
 * list of destinations with the two things that are genuinely global on it
 * (volume, mute), and the mouse page is the only settings page long enough to
 * want its own file.  Wi-Fi, Packages and Diagnostics are reached FROM here but
 * are not settings -- they are applications, and they live in their own files.
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
        OpenMedia
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

#endif /* MIXDASH_SETTINGSPAGE_H */
