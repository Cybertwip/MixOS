/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * settings.cpp -- load, clamp, save.
 */
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include "trace.h"

namespace {

/*
 * Clamping on the way IN, not on the way out.
 *
 * The file is plain INI on a partition the user can mount on a PC, so somebody
 * will eventually edit it by hand and put pointerSpeed=0 in it.  A zero there is
 * a pointer that cannot move, on a device whose only other input is the pad that
 * is now driving the pointer -- recoverable, but only by someone who knows the
 * file exists.  Every value that could brick an input path is clamped to a range
 * that still works, and the clamped value is what gets written back.
 */
int clampInt(int value, int lo, int hi)
{
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/*
 * The first of these whose directory can be created and written.
 *
 * /var/lib/mixos is the real answer.  /run/mixdash is the systemd RuntimeDirectory,
 * which exists for certain but is a tmpfs.  QDir::tempPath() is there so that a
 * developer build on a workstation, where neither of the first two is appropriate,
 * still has somewhere to go.
 */
QString pickStorePath(bool *persistent)
{
    const QStringList dirs = QStringList()
        << QStringLiteral("/var/lib/mixos")
        << QStringLiteral("/run/mixdash")
        << QDir::tempPath();

    for (int i = 0; i < dirs.size(); ++i) {
        const QString &dir = dirs.at(i);
        QDir d(dir);
        if (!d.exists() && !QDir().mkpath(dir))
            continue;

        /* mkpath succeeding does not mean the filesystem is writable -- a
         * read-only mount happily reports that an existing directory exists.
         * The only honest test is to open a file for writing. */
        const QString probe = dir + QStringLiteral("/.mixdash-probe");
        QFile f(probe);
        if (!f.open(QIODevice::WriteOnly))
            continue;
        f.close();
        QFile::remove(probe);

        if (persistent)
            *persistent = (i == 0);
        return dir + QStringLiteral("/mixdash.conf");
    }

    if (persistent)
        *persistent = false;
    return QString();
}

} /* namespace */

Settings &Settings::instance()
{
    /*
     * Function-local static: constructed on first use, which is inside
     * Dashboard's constructor, which is inside a phase that names itself.  A
     * file-scope global would construct before Trace::begin() and a failure in
     * it would print nothing at all.
     */
    static Settings s;
    return s;
}

Settings::Settings()
{
    Trace::step("settings/open");

    bool persistent = false;
    const QString file = pickStorePath(&persistent);
    if (!file.isEmpty()) {
        m_store = new QSettings(file, QSettings::IniFormat, this);
        /* QSettings defers its first read; force it now so a corrupt file is a
         * failure here, in a named step, and not later inside a paint. */
        m_store->sync();
        m_writable = (m_store->status() == QSettings::NoError);
    }

    Trace::step("settings/load");
    load();
}

QString Settings::path() const
{
    if (!m_store)
        return QStringLiteral("(nowhere writable)");
    return m_store->fileName();
}

void Settings::load()
{
    if (!m_store)
        return;

    const MouseConfig d;
    m_store->beginGroup(QStringLiteral("mouse"));
    m_mouse.pointerSpeed = clampInt(
        m_store->value(QStringLiteral("pointerSpeed"), d.pointerSpeed).toInt(), 80, 2400);
    m_mouse.trackingSpeed = clampInt(
        m_store->value(QStringLiteral("trackingSpeed"), d.trackingSpeed).toInt(), 10, 400);
    m_mouse.acceleration = clampInt(
        m_store->value(QStringLiteral("acceleration"), d.acceleration).toInt(), 0, 100);
    m_mouse.deadzone = clampInt(
        m_store->value(QStringLiteral("deadzone"), d.deadzone).toInt(), 2, 60);
    m_mouse.doubleClickMs = clampInt(
        m_store->value(QStringLiteral("doubleClickMs"), d.doubleClickMs).toInt(), 120, 1200);
    m_mouse.hideSeconds = clampInt(
        m_store->value(QStringLiteral("hideSeconds"), d.hideSeconds).toInt(), 1, 60);
    m_mouse.enabled = m_store->value(QStringLiteral("enabled"), d.enabled).toBool();
    m_mouse.leftHanded = m_store->value(QStringLiteral("leftHanded"), d.leftHanded).toBool();
    m_store->endGroup();

    m_wifiInterface = m_store->value(QStringLiteral("wifi/interface")).toString();
    m_mediaRoot = m_store->value(QStringLiteral("media/root")).toString();

    /* A remembered directory that has been deleted, or that lived on a card that
     * is no longer in the slot, would open the Media page on an error.  Drop it
     * rather than carry it. */
    if (!m_mediaRoot.isEmpty() && !QFileInfo(m_mediaRoot).isDir())
        m_mediaRoot.clear();

    /*
     * Brightness gets the same treatment as the pointer speeds, and for a harder
     * reason: this is the only value in the file that can make the machine look
     * dead.  A hand-edited display/brightness=0 would come back after a reboot as
     * a black panel on a board whose only output is that panel, so the floor is
     * clamped here as well as on the slider.  A negative value is left alone --
     * it is not a brightness, it is the absence of one.
     */
    const int saved = m_store->value(QStringLiteral("display/brightness"), -1).toInt();
    m_brightness = saved < 0 ? -1 : clampInt(saved, 5, 100);

    /* Not validated here.  Strings::fromCode() is the one place that knows which
     * codes exist, and it answers English for anything else -- so a hand-edited
     * language=xx costs a fallback, not a startup failure. */
    m_language = m_store->value(QStringLiteral("ui/language")).toString().trimmed().toLower();
}

void Settings::setMouse(const MouseConfig &config)
{
    m_mouse = config;
    m_mouse.pointerSpeed = clampInt(m_mouse.pointerSpeed, 80, 2400);
    m_mouse.trackingSpeed = clampInt(m_mouse.trackingSpeed, 10, 400);
    m_mouse.acceleration = clampInt(m_mouse.acceleration, 0, 100);
    m_mouse.deadzone = clampInt(m_mouse.deadzone, 2, 60);
    m_mouse.doubleClickMs = clampInt(m_mouse.doubleClickMs, 120, 1200);
    m_mouse.hideSeconds = clampInt(m_mouse.hideSeconds, 1, 60);

    if (m_store) {
        m_store->beginGroup(QStringLiteral("mouse"));
        m_store->setValue(QStringLiteral("pointerSpeed"), m_mouse.pointerSpeed);
        m_store->setValue(QStringLiteral("trackingSpeed"), m_mouse.trackingSpeed);
        m_store->setValue(QStringLiteral("acceleration"), m_mouse.acceleration);
        m_store->setValue(QStringLiteral("deadzone"), m_mouse.deadzone);
        m_store->setValue(QStringLiteral("doubleClickMs"), m_mouse.doubleClickMs);
        m_store->setValue(QStringLiteral("hideSeconds"), m_mouse.hideSeconds);
        m_store->setValue(QStringLiteral("enabled"), m_mouse.enabled);
        m_store->setValue(QStringLiteral("leftHanded"), m_mouse.leftHanded);
        m_store->endGroup();
        /*
         * sync() on every change rather than on a timer.  These are handheld
         * settings: the way this device stops is that somebody holds the power
         * button, and a deferred write would be a setting the user watched
         * themselves change and then lost.
         */
        m_store->sync();
    }

    emit mouseChanged();
}

void Settings::setWifiInterface(const QString &iface)
{
    if (m_wifiInterface == iface)
        return;
    m_wifiInterface = iface;
    if (m_store) {
        m_store->setValue(QStringLiteral("wifi/interface"), iface);
        m_store->sync();
    }
}

void Settings::setMediaRoot(const QString &path)
{
    if (m_mediaRoot == path)
        return;
    m_mediaRoot = path;
    if (m_store) {
        m_store->setValue(QStringLiteral("media/root"), path);
        m_store->sync();
    }
}

void Settings::setLanguage(const QString &code)
{
    if (m_language == code)
        return;
    m_language = code;
    if (m_store) {
        m_store->setValue(QStringLiteral("ui/language"), code);
        m_store->sync();
    }
}

void Settings::setBrightness(int percent)
{
    const int value = percent < 0 ? -1 : clampInt(percent, 5, 100);
    if (m_brightness == value)
        return;
    m_brightness = value;
    if (m_store) {
        m_store->setValue(QStringLiteral("display/brightness"), m_brightness);
        m_store->sync();
    }
}
