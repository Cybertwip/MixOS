/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * joypad.h -- every button on the J36 Ultra, read straight from evdev.
 *
 * Qt's linuxfb platform plugin can create its own evdev keyboard handler, and this
 * dashboard switches that off (QT_QPA_FB_DISABLE_INPUT=1) and does the reading
 * here instead.  Two reasons, both specific to this board:
 *
 *   - Qt's evdev keyboard handler translates KEY_* through a keymap and drops
 *     everything else.  Nine of the eleven buttons on this device report BTN_*
 *     codes from the keypad matrix -- BTN_SOUTH, BTN_EAST, BTN_TL, BTN_START and
 *     so on -- so a keymap sees a D-pad and nothing to press with it.
 *   - The codes are not guesses.  They are the ones the device tree assigns, so
 *     this file and mt6592-j36-ultra.dts have to agree, and putting the map here
 *     rather than in a keymap file makes that one short table to check.
 *
 * Polled rather than notified.  QSocketNotifier's activated() is overloaded and
 * deprecated in the exact Qt version Debian trixie ships, and one poll(2) over a
 * handful of descriptors every 15 ms costs nothing measurable on a Cortex-A7 while
 * being the same code on every Qt 5.  It also puts key repeat and the suspend that
 * hands input to a launched child in one obvious place.
 */
#ifndef MIXDASH_JOYPAD_H
#define MIXDASH_JOYPAD_H

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QVector>

class QTimer;

class Joypad : public QObject
{
    Q_OBJECT

public:
    /*
     * Actions, not keys.  The dashboard handles one enum whether the press came
     * from the keypad matrix, the gpio-keys D-pad, the joystick or a USB keyboard
     * plugged in for bring-up.
     */
    enum Nav {
        NavNone = 0,
        NavUp,
        NavDown,
        NavLeft,
        NavRight,
        NavOk,
        NavBack,
        NavPrevPage,
        NavNextPage,
        NavMenu,
        NavQuit
    };

    explicit Joypad(QObject *parent = nullptr);
    ~Joypad() override;

    int deviceCount() const { return m_devs.size(); }
    QStringList deviceNames() const;

    /*
     * Off while a child process owns the screen, then on again with whatever
     * arrived in the meantime thrown away -- otherwise every button the child was
     * pressed with replays into the dashboard the moment it exits.
     */
    void setSuspended(bool suspended);

signals:
    void nav(int action);

private slots:
    void poll();

private:
    struct Dev {
        int fd = -1;
        QString name;
        /* Only the four axes the device tree's adc-joystick declares. */
        int absCode[4] = { -1, -1, -1, -1 };
        int absLo[4] = { 0, 0, 0, 0 };
        int absHi[4] = { 0, 0, 0, 0 };
        int absState[4] = { 0, 0, 0, 0 };
    };

    void openDevices();
    void closeDevices();
    void drain();
    void feed(int action, bool pressed);
    void axis(Dev &d, int slot, int value);

    QVector<Dev> m_devs;
    QTimer *m_timer = nullptr;
    bool m_suspended = false;

    /* Our own key repeat: the input core only autorepeats if a driver asks it to,
     * and neither the keypad nor gpio-keys does here. */
    int m_held = NavNone;
    QElapsedTimer m_heldSince;
    qint64 m_nextRepeat = 0;
};

#endif /* MIXDASH_JOYPAD_H */
