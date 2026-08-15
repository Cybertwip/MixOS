/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
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
 *
 * AND THE POLL TICK IS ALSO THE HOTPLUG.  There is no udev in this image and no
 * uevent listener, so once a second the same tick lists /dev/input and opens what
 * is new; an unplug is quicker than that because poll(2) reports the dead
 * descriptor immediately.  See syncDevices().  This is the difference between a
 * mouse that works when it is plugged in and a mouse that works after its owner
 * has found the Diagnostics page, which is what this file used to require.
 *
 * THREE KINDS OF OUTPUT, because this device has three kinds of input and the
 * dashboard should not have to know which one is plugged in:
 *
 *   nav()     -- an action.  The D-pad, the left stick, the face buttons, and a
 *                USB keyboard's arrows all land here.
 *   pointer*  -- pixels and buttons.  The right stick and a USB mouse both land
 *                here, already scaled by the user's settings, because the axis
 *                ranges live in this file and the poll tick that integrates them
 *                is already running.
 *   key()     -- an evdev code and the modifier state.  Only devices that look
 *                like real keyboards produce it, and only the Terminal listens.
 *
 * AND ONE THING THAT IS NOT AN INPUT AT ALL: the headphone jack.  It is here
 * because of where the kernel puts it and not because it belongs with the
 * buttons -- j36_mt6592_input.c owns the SoC's ADC and its GPIO pads, so the
 * detect line is sampled by the driver behind the pad and arrives as an EV_SW
 * event on the pad's own evdev node.  This file already has every descriptor in
 * /dev/input open and a 15 ms poll(2) over them, so noticing SW_HEADPHONE_INSERT
 * is one more branch in a loop that is already running; a second reader would be
 * a second open of the same node to hear one bit change.  What to DO about it --
 * which of the two ALSA switches goes on and which goes off -- is not this file's
 * business and is in Dashboard::onHeadphoneJack.
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
        NavQuit,
        /*
         * The two hardware keys on the side of the case.  They are Nav actions
         * and not raw key() codes on purpose: key() only reaches a page that
         * asked for it with wantsKeys(), which is the Terminal and the Media
         * player, and the volume keys have to work everywhere -- including on
         * the Apps grid, where no page wants keys at all.  Being actions also
         * puts them within reach of the autorepeat, which they are named into by
         * repeats() in joypad.cpp so that holding VOL+ ramps.
         */
        NavVolumeUp,
        NavVolumeDown
    };

    /* Bit flags on key().  Tracked here because only this file sees the press
     * and the release of a shift that a consumer only ever sees the effect of. */
    enum Modifier {
        ModNone = 0,
        ModShift = 1,
        ModCtrl = 2,
        ModAlt = 4
    };

    explicit Joypad(QObject *parent = nullptr);
    ~Joypad() override;

    int deviceCount() const { return m_devs.size(); }
    QStringList deviceNames() const;
    /* For the Diagnostics page: how many of the open devices look like a mouse
     * and how many like a keyboard.  Answers "is my USB dongle working" without
     * the user having to find a text field to type into. */
    int mouseCount() const;
    int keyboardCount() const;

    /*
     * THE JACK, AND THE THREE-VALUED ANSWER IT REALLY HAS.
     *
     * jackKnown() is false when no open device reports SW_HEADPHONE_INSERT at
     * all, and on this board that is the ordinary case for a kernel built before
     * the detect line was found -- j36_mt6592_input only advertises the switch
     * when jack_adc= or jack_gpio= gave it something to sample.  It is NOT the
     * same as "nothing is plugged in", and everything that acts on this has to
     * tell those two apart: with no detect line the two output switches are a
     * setting the user makes, and something that quietly turned the speaker off
     * because it read an unknown jack as empty would be worse than useless.
     *
     * jackPlugged() is only meaningful while jackKnown() is true.
     */
    bool jackKnown() const { return m_jackKnown; }
    bool jackPlugged() const { return m_jackPlugged; }

    /*
     * Off while a child process owns the screen, then on again with whatever
     * arrived in the meantime thrown away -- otherwise every button the child was
     * pressed with replays into the dashboard the moment it exits.
     */
    void setSuspended(bool suspended);

    /*
     * Text mode: a page that wants characters rather than actions.  Keyboard-class
     * devices stop producing nav() and produce only key(); the pad keeps producing
     * nav(), so B still leaves the Terminal even while something is being typed
     * into it.  That split is the whole point -- a handheld with a USB keyboard in
     * it is two devices, not one.
     */
    void setTextMode(bool textMode) { m_textMode = textMode; }
    bool textMode() const { return m_textMode; }

    /*
     * Throw every descriptor away and open /dev/input from scratch.
     *
     * NOT THE HOTPLUG PATH ANY MORE.  It used to be the only one, and the comment
     * that stood here said so: "called when a hotplug is suspected -- there is no
     * udev in this image, so suspected means the user opened Diagnostics and
     * asked."  That was a mouse that did nothing until its owner found the right
     * page and pressed a button on it, which is not what plugging a mouse in is
     * supposed to feel like.  syncDevices() now runs from the poll tick and picks
     * arrivals and departures up by itself.
     *
     * What is left for this to do is the heavier thing it was always better at:
     * re-reading the axis ranges and the classification of devices that are ALREADY
     * open, which nothing else ever revisits.  Diagnostics still offers it, and the
     * shell calls nothing else.
     */
    void rescan();

signals:
    /*
     * A node appeared under /dev/input or went away, noticed by the poll tick.
     *
     * `mouse' and `keyboard' are this file's own classification -- what the device
     * said about itself through EVIOCGBIT, not what its name looks like -- because
     * the one thing a user wants to hear when a dongle goes in is whether the thing
     * that arrived is the thing they plugged in.
     */
    void deviceAdded(const QString &name, bool mouse, bool keyboard);
    void deviceRemoved(const QString &name);
    /* Emitted once after any batch of the two above, for anything that only wants
     * to know that the answer to deviceCount() has changed. */
    void devicesChanged();

    /*
     * `repeat' is false for the press itself and true for every one of the
     * autorepeats that follow it while the key or the stick is held.
     *
     * Almost nothing cares -- walking a list is the same whether the press was
     * held or made twice, which is the whole reason autorepeat exists.  What cares
     * is anything a repeat should NOT be able to do over and over: the shell turns
     * a left or a right the page refused into a change of root page, and at eleven
     * repeats a second a leaned stick would spin through the tabs.  Those are
     * gestures, and a gesture is a press.
     */
    void nav(int action, bool repeat);

    /*
     * The same action, let go of.
     *
     * ADDED FOR ONE GESTURE and it is worth saying which, because a release
     * signal invites everything to start acting on releases and that would make
     * this dashboard feel slack.  The card grid tells a tap from a long press --
     * launch the card, or pick it up and rearrange the grid -- and those two are
     * indistinguishable at the moment the button goes down.  Nothing else listens.
     *
     * Emitted at most once per press.  m_down is what guarantees that: a release
     * with no matching press -- which is what the input core hands over after a
     * suspend, and what two devices reporting the same button produce -- is
     * dropped rather than delivered.
     */
    void navReleased(int action);

    /* Pixels, already scaled and accelerated.  Fractional because at low speeds a
     * 15 ms tick moves less than a pixel and truncating each tick would stop the
     * pointer dead rather than move it slowly. */
    void pointerMove(qreal dx, qreal dy);
    /* button is a Qt::MouseButton value. */
    void pointerButton(int button, bool pressed);
    /* Eighths of a degree, like QWheelEvent: 120 is one notch. */
    void pointerWheel(int delta);

    void key(int code, bool pressed, int modifiers);

    /*
     * Something went into the headphone jack, or came out of it.
     *
     * EMITTED ONLY WHILE THERE IS A DETECT LINE TO BELIEVE.  A driver that stops
     * reporting the switch -- the module unloaded, the node gone -- does not emit
     * a final unplugged: an absent jack is unknown and not empty, and the one
     * thing this signal must never do is tell the shell to switch a user's
     * headphones off because the thing that could see them went away.
     *
     * Emitted on change only, and not at construction: nothing is connected to
     * this yet when openDevices() runs from the constructor, so the shell reads
     * jackKnown()/jackPlugged() once for the state it started in and listens to
     * this for every state after that.
     */
    void headphoneJack(bool plugged);

private slots:
    void poll();

private:
    struct Dev {
        int fd = -1;
        /* The directory entry this was opened from -- "event3", not the path and
         * not the EVIOCGNAME string.  It is the key syncDevices() matches on, so it
         * has to be the name the kernel uses and not the one the device chose:
         * three identical dongles in a hub all call themselves "USB OPTICAL
         * MOUSE". */
        QString node;
        QString name;
        /* Only the four axes the device tree's adc-joystick declares. */
        int absCode[4] = { -1, -1, -1, -1 };
        int absLo[4] = { 0, 0, 0, 0 };
        int absHi[4] = { 0, 0, 0, 0 };
        int absState[4] = { 0, 0, 0, 0 };
        /* Raw value of the two right-stick axes, kept because the pointer
         * integrates a position from them on every tick rather than reacting to
         * the edges the way nav does. */
        int absRaw[4] = { 0, 0, 0, 0 };
        bool absSeen[4] = { false, false, false, false };

        bool keyboard = false;
        bool mouse = false;

        /* This node reports SW_HEADPHONE_INSERT, and what it last said.  Per
         * device rather than per Joypad because a USB headset's node reports one
         * too, and two of them disagreeing is a state this has to hold rather
         * than overwrite -- see updateJack(). */
        bool jack = false;
        bool jackPlugged = false;
    };

    void openDevices();
    /* Open one entry of /dev/input and classify it.  True if it was kept. */
    bool openNode(const QString &node);
    void closeDevices();
    /*
     * The hotplug.  Compares the directory against what is open, opens what is new
     * and closes what is gone, and touches nothing that is unchanged -- so a mouse
     * arriving in a hub does not reset the axis state of the stick that is being
     * held at that moment.
     */
    void syncDevices();
    /*
     * Recompute the one jack answer from however many devices are reporting one,
     * and emit if it moved.  Called from everywhere the set of devices or the
     * state of one of them changes -- an arrival, a departure, an EV_SW event,
     * and the re-read after a child process handed the descriptors back.
     */
    void updateJack();
    void drain();
    void feed(int action, bool pressed);
    void axis(Dev &d, int slot, int value);
    /* EV_KEY from a device that reports relative motion, or a stick click. */
    bool pointerKey(int code, bool pressed);
    void driveStick(int ms);
    void relative(int code, int value);

    QVector<Dev> m_devs;
    QTimer *m_timer = nullptr;
    bool m_suspended = false;
    bool m_textMode = false;
    int m_mods = ModNone;

    /* The current answer to the two accessors, so a consumer can ask without
     * this having to walk the device list on every call. */
    bool m_jackKnown = false;
    bool m_jackPlugged = false;

    /* Our own key repeat: the input core only autorepeats if a driver asks it to,
     * and neither the keypad nor gpio-keys does here. */
    int m_held = NavNone;
    QElapsedTimer m_heldSince;
    qint64 m_nextRepeat = 0;

    /*
     * Which actions are down, one bit per Nav value.
     *
     * A quint32 and not a QSet: there are fourteen actions and this is tested on
     * every key event.  It exists so navReleased() cannot fire for a press that
     * never happened -- the keypad matrix and a USB gamepad both map to BTN_SOUTH
     * here, so one physical press can arrive twice, and setSuspended() throws
     * presses away whose releases arrive afterwards.
     */
    quint32 m_down = 0;

    /* Wall clock between poll ticks, so pointer speed is in pixels per second and
     * not pixels per however-often-the-event-loop-got-round-to-us. */
    QElapsedTimer m_tick;

    /*
     * When /dev/input was last listed.
     *
     * ONCE A SECOND, NOT EVERY TICK.  The poll tick is 15 ms and a directory
     * listing is an opendir, a getdents and a QStringList of eight entries -- 66
     * times a second of that on a Cortex-A7 is real work to notice a mouse that a
     * human took a second to plug in anyway.  A second is under the time it takes
     * to move a hand back to the buttons, which is the only latency that matters
     * here.  An unplug is noticed sooner than that and from somewhere else: poll(2)
     * reports the dead descriptor on the very next tick.
     */
    QElapsedTimer m_scanClock;

    /*
     * A mouse reports REL_X, REL_Y and then SYN_REPORT: one movement, three
     * events.  Emitting on each of the first two would send the pointer down a
     * staircase and would make every diagonal produce two hover events.  These
     * hold the packet until the SYN.
     */
    qreal m_relPendX = 0.0;
    qreal m_relPendY = 0.0;
    int m_wheelPend = 0;
};

#endif /* MIXDASH_JOYPAD_H */
