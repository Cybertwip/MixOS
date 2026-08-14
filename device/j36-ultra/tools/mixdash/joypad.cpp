/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "joypad.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <Qt>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "settings.h"

namespace {

/*
 * The map, and it is the whole reason this file exists.  Every code here is the
 * one mt6592-j36-ultra.dts assigns -- the four gpio-keys directions, the eleven
 * keypad-matrix buttons and the two volume keys -- plus the handful a USB keyboard
 * sends, because bring-up happens with one plugged in.
 *
 * BTN_SOUTH is the physical A on this shell and BTN_EAST is B, which is why OK and
 * Back are those two and not the other way round.
 *
 * BTN_THUMBL and BTN_THUMBR are deliberately absent: the two stick clicks are the
 * pointer's buttons, handled in pointerKey() before this table is consulted.
 */
struct Map {
    int code;
    int action;
};

const Map kMap[] = {
    /* gpio-keys: the D-pad. */
    { KEY_UP,        Joypad::NavUp },
    { KEY_DOWN,      Joypad::NavDown },
    { KEY_LEFT,      Joypad::NavLeft },
    { KEY_RIGHT,     Joypad::NavRight },

    /* The keypad matrix. */
    { BTN_SOUTH,     Joypad::NavOk },        /* A */
    { BTN_EAST,      Joypad::NavBack },      /* B */
    { BTN_TL,        Joypad::NavPrevPage },
    { BTN_TR,        Joypad::NavNextPage },
    { BTN_TL2,       Joypad::NavPrevPage },
    { BTN_TR2,       Joypad::NavNextPage },
    { BTN_SELECT,    Joypad::NavMenu },
    { BTN_START,     Joypad::NavMenu },
    { BTN_MODE,      Joypad::NavQuit },

    /* The two volume keys the keypad matrix carries -- generate_dts.py puts
     * MT6592_J36_KEY_VOL_UP_MATRIX and _DOWN_ on KEY_VOLUMEUP and KEY_VOLUMEDOWN.
     * The shell answers these itself, before any page sees them: see
     * Dashboard::onNav. */
    { KEY_VOLUMEUP,   Joypad::NavVolumeUp },
    { KEY_VOLUMEDOWN, Joypad::NavVolumeDown },

    /* The device tree's special-home key, which has no BTN_ name. */
    { 88,            Joypad::NavMenu },

    /* A keyboard, for bring-up. */
    { KEY_ENTER,     Joypad::NavOk },
    { KEY_KPENTER,   Joypad::NavOk },
    { KEY_SPACE,     Joypad::NavOk },
    { KEY_ESC,       Joypad::NavBack },
    { KEY_BACKSPACE, Joypad::NavBack },
    { KEY_PAGEUP,    Joypad::NavPrevPage },
    { KEY_PAGEDOWN,  Joypad::NavNextPage },
    { KEY_TAB,       Joypad::NavNextPage },
    { KEY_M,         Joypad::NavMenu },
    { KEY_Q,         Joypad::NavQuit }
};

/*
 * Slot order matches the device tree: ABS_X, ABS_Y, ABS_Z, ABS_RZ.  Slots 0 and
 * 1 are the left stick and navigate; 2 and 3 are the right stick and drive the
 * pointer unless the user has turned the pointer off.
 *
 * SLOT 3 IS ABS_RZ AND NOT ABS_RY, AND THAT IS WHY THE POINTER DID NOT WORK.
 * generate_dts.py emits `j36,axis-map = <ch15 ABS_X 1  ch14 ABS_Y 1  ch12 ABS_Z 0
 * ch13 ABS_RZ 0>' and j36_mt6592_input.c passes those codes to
 * input_set_abs_params verbatim, so the fourth axis this board reports is code 5.
 * This table asked for code 4.  The right stick's X arrived on slot 2 and its Y
 * arrived on an axis nothing was listening to, so absSeen[3] was never set --
 * and driveStick() skips a device outright unless BOTH of its right-stick axes
 * have been seen, so the pointer was not half-broken, it never moved at all.
 *
 * The disabled adc-joystick node in the same DTS says <5> for its axis@3 too, so
 * both descriptions of this hardware agree and it was only this file that did not.
 */
const int kAxis[4] = { ABS_X, ABS_Y, ABS_Z, ABS_RZ };

const int kRepeatFirstMs = 380;
const int kRepeatNextMs = 90;

int lookup(int code)
{
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i)
        if (kMap[i].code == code)
            return kMap[i].action;
    return Joypad::NavNone;
}

/*
 * Which actions autorepeat while held.  It was called isDirection() and it was
 * the four D-pad directions, which is what "hold it and it keeps going" meant
 * when the only thing worth holding was a selection.
 *
 * The two volume keys belong here for the same reason and are NOT directions:
 * they are the only other action on this case whose whole purpose is to be held
 * down.  Everything else in the map is a decision -- A, B, the shoulders, Menu --
 * and an action that repeats is an action that can happen twice from one press,
 * which for Power off is not a thing to risk.
 */
bool repeats(int action)
{
    return action == Joypad::NavUp || action == Joypad::NavDown
        || action == Joypad::NavLeft || action == Joypad::NavRight
        || action == Joypad::NavVolumeUp || action == Joypad::NavVolumeDown;
}

/* EVIOCGBIT hands back a bitmap in longs.  Two lines of arithmetic rather than a
 * dependency on libevdev, which is not in the armhf chroot. */
bool testBit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

int modifierFor(int code)
{
    switch (code) {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        return Joypad::ModShift;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        return Joypad::ModCtrl;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        return Joypad::ModAlt;
    default:
        return Joypad::ModNone;
    }
}

/* The directory, listed the one way both callers here want it. */
QStringList inputNodes()
{
    return QDir("/dev/input").entryList(QStringList() << "event*", QDir::System, QDir::Name);
}

qreal clampReal(qreal v, qreal lo, qreal hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

} /* namespace */

Joypad::Joypad(QObject *parent)
    : QObject(parent)
{
    openDevices();
    m_heldSince.start();
    m_tick.start();
    m_scanClock.start();

    m_timer = new QTimer(this);
    m_timer->setInterval(15);
    connect(m_timer, &QTimer::timeout, this, &Joypad::poll);
    m_timer->start();
}

Joypad::~Joypad()
{
    closeDevices();
}

void Joypad::openDevices()
{
    for (const QString &n : inputNodes())
        openNode(n);
}

bool Joypad::openNode(const QString &node)
{
    const QString path = "/dev/input/" + node;
    const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;

    /*
     * A device with no keys, no axes and no relative motion is not an input we
     * can navigate with -- on this board that is the power button's own node
     * and anything a USB hub brings along.  Closed rather than polled.
     */
    unsigned long evbits = 0;
    if (::ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0
        || !((evbits & (1UL << EV_KEY)) || (evbits & (1UL << EV_ABS))
             || (evbits & (1UL << EV_REL)))) {
        ::close(fd);
        return false;
    }

    Dev d;
    d.fd = fd;
    d.node = node;

    char name[128] = { 0 };
    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0)
        d.name = QString::fromLocal8Bit(name);
    else
        d.name = node;

    /*
     * Classification, and it decides two things: whether this device's keys
     * become text in the Terminal, and whether its motion becomes pointer
     * motion.  Both are asked of the device rather than of its name, because
     * "SEM HID Device" is what half the USB dongles on this planet call
     * themselves.
     */
    if (evbits & (1UL << EV_KEY)) {
        unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
        ::memset(keys, 0, sizeof(keys));
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0) {
            /* A real keyboard has letters.  A pad that reports BTN_* does not,
             * and a mouse reports BTN_LEFT and nothing alphabetic. */
            d.keyboard = testBit(keys, KEY_A) && testBit(keys, KEY_Z)
                && testBit(keys, KEY_SPACE);
            if (testBit(keys, BTN_LEFT) && (evbits & (1UL << EV_REL)))
                d.mouse = true;
        }
    }
    if (evbits & (1UL << EV_REL)) {
        unsigned long rels = 0;
        if (::ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rels)), &rels) >= 0
            && (rels & (1UL << REL_X)) && (rels & (1UL << REL_Y)))
            d.mouse = true;
    }

    if (evbits & (1UL << EV_ABS)) {
        for (int slot = 0; slot < 4; ++slot) {
            struct input_absinfo info;
            ::memset(&info, 0, sizeof(info));
            if (::ioctl(fd, EVIOCGABS(kAxis[slot]), &info) < 0)
                continue;
            if (info.maximum <= info.minimum)
                continue;
            d.absCode[slot] = kAxis[slot];
            d.absLo[slot] = info.minimum;
            d.absHi[slot] = info.maximum;
            /* Seed with the value the driver already has, so a stick that is
             * resting off-centre does not lurch on the first report. */
            d.absRaw[slot] = info.value;
            d.absSeen[slot] = true;
        }
    }

    m_devs.append(d);
    return true;
}

void Joypad::closeDevices()
{
    for (Dev &d : m_devs)
        if (d.fd >= 0)
            ::close(d.fd);
    m_devs.clear();
}

/*
 * THE HOTPLUG.  Everything above this line was written for a machine whose input
 * devices are soldered to it, which the pad and the sticks are -- and then a USB
 * mouse goes into the one port on the side and none of it notices.  There is no
 * udev here and there is no netlink listener either: this reads a directory.
 *
 * WHY A DIRECTORY LISTING RATHER THAN A UEVENT SOCKET.  A NETLINK_KOBJECT_UEVENT
 * socket would be the notified version of this, and it is more code in the two
 * places that matter -- it needs a second descriptor in the poll set, a parser
 * for the uevent payload, and it still has to list the directory afterwards
 * because the node the message names may not have been created yet when the
 * message arrives.  The listing is the part that cannot be skipped, so this does
 * only the part that cannot be skipped, once a second.
 *
 * WHAT IS DELIBERATELY NOT TOUCHED: everything already open.  The stick that is
 * held over at the moment a hub enumerates keeps its axis state, its seeded
 * centre and its descriptor, because a device that did not change has no reason
 * to be reopened.  That is the entire difference between this and rescan(), and
 * it is why rescan() is no longer on the hotplug path.
 */
void Joypad::syncDevices()
{
    const QStringList nodes = inputNodes();
    bool changed = false;

    /* Gone.  Backwards, so removing one does not renumber the ones not yet
     * looked at. */
    for (int i = m_devs.size() - 1; i >= 0; --i) {
        if (nodes.contains(m_devs[i].node))
            continue;
        const QString name = m_devs[i].name;
        if (m_devs[i].fd >= 0)
            ::close(m_devs[i].fd);
        m_devs.remove(i);
        changed = true;
        emit deviceRemoved(name);
    }

    /* Arrived. */
    for (const QString &n : nodes) {
        bool known = false;
        for (const Dev &d : m_devs) {
            if (d.node == n) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        /*
         * A node can exist for a moment before its permissions are set, and a
         * device can be claimed by something else.  openNode() returning false is
         * not an error to report -- the next sweep, a second later, tries again,
         * and a node that is genuinely uninteresting (the power button, a hub's
         * own descriptor) fails the same way every time and costs one open(2) a
         * second, which is the price of not keeping a list of things to ignore.
         */
        if (!openNode(n))
            continue;
        changed = true;
        const Dev &d = m_devs.last();
        emit deviceAdded(d.name, d.mouse, d.keyboard);
    }

    if (changed)
        emit devicesChanged();
}

void Joypad::rescan()
{
    closeDevices();
    openDevices();
    m_held = NavNone;
    m_down = 0;
    m_mods = ModNone;
    emit devicesChanged();
}

QStringList Joypad::deviceNames() const
{
    QStringList out;
    for (const Dev &d : m_devs)
        out << d.name;
    return out;
}

int Joypad::mouseCount() const
{
    int n = 0;
    for (const Dev &d : m_devs)
        if (d.mouse)
            ++n;
    return n;
}

int Joypad::keyboardCount() const
{
    int n = 0;
    for (const Dev &d : m_devs)
        if (d.keyboard)
            ++n;
    return n;
}

void Joypad::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;

    if (suspended) {
        m_timer->stop();
        m_held = NavNone;
        /* Whatever was down when the child took the input devices is not down as
         * far as this dashboard is concerned: the release will be delivered to
         * the child, or discarded by drain() on the way back. */
        m_down = 0;
        m_mods = ModNone;
    } else {
        /*
         * Sync BEFORE the drain, and both before the timer restarts.
         *
         * The tick is stopped while a child owns the screen, so nothing has been
         * watching /dev/input for however long the game ran -- and a mouse plugged
         * in during it, or the one pulled out to make room for a headset, is
         * exactly the state this dashboard comes back to.  Doing it here rather
         * than waiting for the next sweep also means the drain that follows flushes
         * the devices that just appeared as well as the ones that were already
         * open, which is the whole point of the drain.
         */
        syncDevices();
        m_scanClock.restart();
        drain();
        m_tick.restart();
        m_timer->start();
    }
}

void Joypad::drain()
{
    struct input_event ev;
    for (const Dev &d : m_devs)
        while (::read(d.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
            ;
}

void Joypad::poll()
{
    /*
     * BEFORE the early return below, and that ordering is the whole fix.  A board
     * that booted with nothing in the port has an empty m_devs, and every version
     * of this function before this one returned here and therefore could never
     * open anything ever again.  The pad is soldered on so it never actually
     * happened on this device -- which is exactly why it went unnoticed until a
     * mouse was the first thing plugged into a running system.
     */
    if (m_scanClock.elapsed() >= 1000) {
        m_scanClock.restart();
        syncDevices();
    }

    if (m_devs.isEmpty())
        return;

    /*
     * How long since the last tick, clamped.  The lower bound stops a division
     * from a zero interval; the upper stops a pointer from teleporting across the
     * panel after the event loop was blocked -- by a child process, by a page that
     * read /proc synchronously, by anything.  Distance is a function of time here,
     * so time is the thing that has to be sane.
     */
    qint64 ms = m_tick.restart();
    if (ms < 1)
        ms = 1;
    if (ms > 100)
        ms = 100;

    struct pollfd pfd[16];
    int n = 0;
    for (int i = 0; i < m_devs.size() && n < 16; ++i) {
        pfd[n].fd = m_devs[i].fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        ++n;
    }

    if (::poll(pfd, n, 0) > 0) {
        /*
         * Descriptors whose device stopped existing under them.  Collected rather
         * than removed on the spot: pfd[i] was built from m_devs[i] and removing an
         * element would renumber everything after it, so the list is drained once
         * the read loop is done with the indices.
         */
        QVector<int> gone;

        for (int i = 0; i < n; ++i) {
            /*
             * THE UNPLUG, AND IT COSTS NOTHING TO NOTICE.  evdev_poll() sets
             * EPOLLHUP|EPOLLERR the moment the input device behind the descriptor
             * is gone, so a mouse pulled out of the port is known on the next 15 ms
             * tick -- long before the once-a-second directory sweep would have got
             * to it.  Reading on instead would give a hard -ENODEV forever and the
             * while loop below would simply never see another event, which is how
             * this used to end: a dead descriptor polled sixty-six times a second
             * for the rest of the session.
             */
            if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                gone.append(i);
                continue;
            }
            if (!(pfd[i].revents & POLLIN))
                continue;
            Dev &d = m_devs[i];
            struct input_event ev;
            while (::read(d.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    /* value 2 is the input core's own autorepeat; ours is below.
                     * A USB keyboard DOES autorepeat, and in text mode that is the
                     * repeat the user wants, so it is passed through to key(). */
                    const bool pressed = ev.value != 0;

                    if (d.keyboard) {
                        const int mod = modifierFor(ev.code);
                        if (mod != ModNone) {
                            if (pressed)
                                m_mods |= mod;
                            else
                                m_mods &= ~mod;
                        }
                        if (ev.value != 2 || m_textMode)
                            emit key(ev.code, pressed, m_mods);
                        if (m_textMode)
                            continue;
                    }

                    if (ev.value == 2)
                        continue;
                    if (pointerKey(ev.code, pressed))
                        continue;
                    const int action = lookup(ev.code);
                    if (action != NavNone)
                        feed(action, pressed);
                } else if (ev.type == EV_ABS) {
                    for (int slot = 0; slot < 4; ++slot) {
                        if (d.absCode[slot] != (int)ev.code)
                            continue;
                        d.absRaw[slot] = ev.value;
                        /* The right stick only navigates when it is not pointing. */
                        if (slot >= 2 && Settings::instance().mouse().enabled)
                            continue;
                        axis(d, slot, ev.value);
                    }
                } else if (ev.type == EV_REL) {
                    relative(ev.code, ev.value);
                } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    if (m_relPendX != 0.0 || m_relPendY != 0.0) {
                        emit pointerMove(m_relPendX, m_relPendY);
                        m_relPendX = 0.0;
                        m_relPendY = 0.0;
                    }
                    if (m_wheelPend != 0) {
                        emit pointerWheel(m_wheelPend);
                        m_wheelPend = 0;
                    }
                }
            }
        }

        for (int i = gone.size() - 1; i >= 0; --i) {
            const int idx = gone[i];
            const QString name = m_devs[idx].name;
            if (m_devs[idx].fd >= 0)
                ::close(m_devs[idx].fd);
            m_devs.remove(idx);
            emit deviceRemoved(name);
        }
        if (!gone.isEmpty()) {
            /*
             * A button that was down on the device that just left is not coming
             * back up: its release was going to arrive on the descriptor that no
             * longer exists.  Forgetting the press here is what stops a held
             * direction from repeating forever after the pad it was held on was
             * unplugged.
             */
            m_held = NavNone;
            m_down = 0;
            m_mods = ModNone;
            emit devicesChanged();
        }
    }

    driveStick((int)ms);

    /* Key repeat, for the actions repeats() names -- the four directions and the
     * two volume keys.  The true is what tells the shell this one was not a
     * press, so it can refuse to act on it. */
    if (m_held != NavNone) {
        const qint64 now = m_heldSince.elapsed();
        if (now >= m_nextRepeat) {
            emit nav(m_held, true);
            m_nextRepeat = now + kRepeatNextMs;
        }
    }
}

void Joypad::relative(int code, int value)
{
    const MouseConfig &cfg = Settings::instance().mouse();
    const qreal scale = cfg.trackingSpeed / 100.0;

    switch (code) {
    case REL_X:
        m_relPendX += value * scale;
        break;
    case REL_Y:
        m_relPendY += value * scale;
        break;
    case REL_WHEEL:
        /* evdev counts notches, QWheelEvent counts eighths of a degree. */
        m_wheelPend += value * 120;
        break;
    default:
        break;
    }
}

bool Joypad::pointerKey(int code, bool pressed)
{
    const MouseConfig &cfg = Settings::instance().mouse();

    int button = 0;
    switch (code) {
    case BTN_LEFT:
        button = Qt::LeftButton;
        break;
    case BTN_RIGHT:
        button = Qt::RightButton;
        break;
    case BTN_MIDDLE:
        button = Qt::MiddleButton;
        break;
    case BTN_THUMBR:
        /* Right stick click.  The stick that moves the pointer carries the button
         * that clicks with it, which is the only arrangement that can be used
         * one-handed. */
        if (!cfg.enabled)
            return false;
        button = cfg.leftHanded ? Qt::RightButton : Qt::LeftButton;
        break;
    case BTN_THUMBL:
        if (!cfg.enabled)
            return false;
        button = cfg.leftHanded ? Qt::LeftButton : Qt::RightButton;
        break;
    default:
        return false;
    }

    emit pointerButton(button, pressed);
    return true;
}

void Joypad::driveStick(int ms)
{
    const MouseConfig &cfg = Settings::instance().mouse();
    if (!cfg.enabled)
        return;

    /* The first device that reports both right-stick axes wins.  There is exactly
     * one adc-joystick on this board; the loop is here so that a USB pad plugged
     * in later is not silently ignored. */
    qreal nx = 0.0;
    qreal ny = 0.0;
    bool found = false;
    for (const Dev &d : m_devs) {
        if (!d.absSeen[2] || !d.absSeen[3])
            continue;
        const qreal dz = cfg.deadzone / 100.0;
        for (int slot = 2; slot <= 3; ++slot) {
            const qreal mid = (d.absLo[slot] + d.absHi[slot]) / 2.0;
            const qreal half = (d.absHi[slot] - d.absLo[slot]) / 2.0;
            if (half <= 0.0)
                continue;
            qreal v = clampReal((d.absRaw[slot] - mid) / half, -1.0, 1.0);
            /* Rescale outside the deadzone so the first pixel of movement past it
             * is slow rather than a jump to deadzone-speed. */
            const qreal mag = fabs(v);
            if (mag <= dz)
                v = 0.0;
            else
                v = (v < 0.0 ? -1.0 : 1.0) * (mag - dz) / (1.0 - dz);
            if (slot == 2)
                nx = v;
            else
                ny = v;
        }
        found = true;
        break;
    }
    if (!found)
        return;

    qreal mag = sqrt(nx * nx + ny * ny);
    if (mag <= 0.0)
        return;
    if (mag > 1.0) {
        /* A square stick gate reads 1.41 on the diagonal.  Normalising the
         * direction and clamping the magnitude keeps diagonals from being half as
         * fast again as the axes. */
        nx /= mag;
        ny /= mag;
        mag = 1.0;
    }

    /*
     * The response curve.  acceleration 0 is linear -- deflection is speed.  Above
     * that the exponent rises to 2.5, which makes a small push crawl and a full
     * push cross the 640 px panel, and that spread is what lets a 12 mm stick both
     * travel and land on a scrollbar.
     */
    const qreal exponent = 1.0 + cfg.acceleration * 0.015;
    const qreal curved = pow(mag, exponent);
    const qreal step = cfg.pointerSpeed * curved * ms / 1000.0;

    emit pointerMove(nx / mag * step, ny / mag * step);
}

void Joypad::feed(int action, bool pressed)
{
    /* NavNone is what lookup() answers for a code this board does not map, and it
     * is not a bit worth setting -- every unmapped key on a USB keyboard would
     * share it and they would release each other. */
    if (action == NavNone)
        return;
    const quint32 bit = 1u << (action & 31);

    if (!pressed) {
        if (repeats(action) && m_held == action)
            m_held = NavNone;
        /* Only for a press this object saw.  See m_down in the header. */
        if (m_down & bit) {
            m_down &= ~bit;
            emit navReleased(action);
        }
        return;
    }

    /*
     * A REPEATED PRESS IS NOT A SECOND PRESS.  Two devices can report the same
     * button -- the keypad matrix and a gamepad both map to BTN_SOUTH here -- and
     * the second press would otherwise leave the bit set after the first release,
     * so the second release would emit nothing and the card grid would sit
     * waiting for a release that already went past.  nav() still fires either
     * way: that is a press, and a page that acts on presses should hear it.
     */
    m_down |= bit;
    emit nav(action, false);

    if (repeats(action)) {
        m_held = action;
        m_nextRepeat = m_heldSince.elapsed() + kRepeatFirstMs;
    }
}

void Joypad::axis(Dev &d, int slot, int value)
{
    const int lo = d.absLo[slot];
    const int hi = d.absHi[slot];
    if (hi <= lo)
        return;

    /*
     * Engage at 55% of half travel and release at 30%, because an ADC joystick
     * with no calibration sits a long way off centre and a single threshold makes
     * it stutter.  Hysteresis, not a deadzone.
     */
    const int mid = (lo + hi) / 2;
    const int half = (hi - lo) / 2;
    const int engage = half * 55 / 100;
    const int release = half * 30 / 100;
    const int dv = value - mid;

    const int was = d.absState[slot];
    int now = was;
    if (was == 0) {
        if (dv >= engage)
            now = 1;
        else if (dv <= -engage)
            now = -1;
    } else if (was > 0) {
        if (dv < release)
            now = 0;
    } else {
        if (dv > -release)
            now = 0;
    }
    if (now == was)
        return;

    const bool horizontal = (slot == 0 || slot == 2);
    const int negative = horizontal ? NavLeft : NavUp;
    const int positive = horizontal ? NavRight : NavDown;

    if (was != 0)
        feed(was > 0 ? positive : negative, false);
    d.absState[slot] = now;
    if (now != 0)
        feed(now > 0 ? positive : negative, true);
}
