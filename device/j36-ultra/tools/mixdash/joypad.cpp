/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "joypad.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

/*
 * The map, and it is the whole reason this file exists.  Every code here is the
 * one mt6592-j36-ultra.dts assigns -- the four gpio-keys directions, the eleven
 * keypad-matrix buttons and the two volume keys -- plus the handful a USB keyboard
 * sends, because bring-up happens with one plugged in.
 *
 * BTN_SOUTH is the physical A on this shell and BTN_EAST is B, which is why OK and
 * Back are those two and not the other way round.
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

/* Slot order matches the adc-joystick node: ABS_X, ABS_Y, ABS_Z, ABS_RY. */
const int kAxis[4] = { ABS_X, ABS_Y, ABS_Z, ABS_RY };

const int kRepeatFirstMs = 380;
const int kRepeatNextMs = 90;

int lookup(int code)
{
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i)
        if (kMap[i].code == code)
            return kMap[i].action;
    return Joypad::NavNone;
}

bool isDirection(int action)
{
    return action == Joypad::NavUp || action == Joypad::NavDown
        || action == Joypad::NavLeft || action == Joypad::NavRight;
}

} /* namespace */

Joypad::Joypad(QObject *parent)
    : QObject(parent)
{
    openDevices();
    m_heldSince.start();

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
    const QStringList names =
        QDir("/dev/input").entryList(QStringList() << "event*", QDir::System, QDir::Name);

    for (const QString &n : names) {
        const QString path = "/dev/input/" + n;
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        /*
         * A device with no keys and no axes is not an input we can navigate with --
         * on this board that is the power button's own node and anything a USB hub
         * brings along.  Closed rather than polled.
         */
        unsigned long evbits = 0;
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0
            || !((evbits & (1UL << EV_KEY)) || (evbits & (1UL << EV_ABS)))) {
            ::close(fd);
            continue;
        }

        Dev d;
        d.fd = fd;

        char name[128] = { 0 };
        if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0)
            d.name = QString::fromLocal8Bit(name);
        else
            d.name = n;

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
            }
        }

        m_devs.append(d);
    }
}

void Joypad::closeDevices()
{
    for (Dev &d : m_devs)
        if (d.fd >= 0)
            ::close(d.fd);
    m_devs.clear();
}

QStringList Joypad::deviceNames() const
{
    QStringList out;
    for (const Dev &d : m_devs)
        out << d.name;
    return out;
}

void Joypad::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;

    if (suspended) {
        m_timer->stop();
        m_held = NavNone;
    } else {
        drain();
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
    if (m_devs.isEmpty())
        return;

    struct pollfd pfd[16];
    int n = 0;
    for (int i = 0; i < m_devs.size() && n < 16; ++i) {
        pfd[n].fd = m_devs[i].fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        ++n;
    }

    if (::poll(pfd, n, 0) > 0) {
        for (int i = 0; i < n; ++i) {
            if (!(pfd[i].revents & POLLIN))
                continue;
            Dev &d = m_devs[i];
            struct input_event ev;
            while (::read(d.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY) {
                    /* value 2 is the input core's own autorepeat; ours is below. */
                    if (ev.value == 2)
                        continue;
                    const int action = lookup(ev.code);
                    if (action != NavNone)
                        feed(action, ev.value != 0);
                } else if (ev.type == EV_ABS) {
                    for (int slot = 0; slot < 4; ++slot)
                        if (d.absCode[slot] == (int)ev.code)
                            axis(d, slot, ev.value);
                }
            }
        }
    }

    /* Key repeat, for the four directions only. */
    if (m_held != NavNone) {
        const qint64 now = m_heldSince.elapsed();
        if (now >= m_nextRepeat) {
            emit nav(m_held);
            m_nextRepeat = now + kRepeatNextMs;
        }
    }
}

void Joypad::feed(int action, bool pressed)
{
    if (!pressed) {
        if (isDirection(action) && m_held == action)
            m_held = NavNone;
        return;
    }

    emit nav(action);

    if (isDirection(action)) {
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
