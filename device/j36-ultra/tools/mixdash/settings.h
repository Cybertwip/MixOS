/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * settings.h -- everything the dashboard remembers between boots.
 *
 * ONE FILE, WRITTEN WHERE THERE IS SOMEWHERE TO WRITE.  /var/lib/mixos is on the
 * ext2 OS partition and survives a power cut; /run/mixdash is a tmpfs and does
 * not.  The fallback is not a nicety -- /init mounts the OS partition read-only
 * on some of the recovery paths in this bring-up, and a dashboard that aborts
 * because it cannot save a pointer speed would be a worse dashboard than one
 * whose pointer speed resets.  path() says which of the two won, and the Settings
 * page prints it, so "my settings do not stick" is answerable from the glass.
 *
 * QSettings and not a hand-rolled parser: it is already linked, it writes
 * atomically through a temporary and a rename, and it is the only code here that
 * would have to care that the card can be pulled out mid-write.
 */
#ifndef MIXDASH_SETTINGS_H
#define MIXDASH_SETTINGS_H

#include <QObject>
#include <QString>
#include <QStringList>

class QSettings;

/*
 * The mouse block, as a struct rather than as eight separate getters, because
 * Pointer reads all of it on every 15 ms tick and a QSettings lookup is a hash
 * of a string.  Settings::mouse() hands back a const reference to a cached copy
 * and save() is what writes.
 */
struct MouseConfig {
    /*
     * Pixels per second at full stick deflection.  A speed rather than a
     * per-sample step: the poll interval is a property of Joypad and a user who
     * changes this should not have to know it.
     */
    int pointerSpeed = 560;

    /*
     * Per cent applied to the deltas a real USB mouse reports.  Separate from
     * pointerSpeed because the two have nothing in common -- one integrates a
     * position, the other scales a distance the hardware already measured.
     */
    int trackingSpeed = 100;

    /*
     * 0 is linear -- the stick's deflection maps straight to a speed.  Higher
     * values bend the response so small deflections are slower than linear and
     * large ones are faster, which is what makes a 12 mm stick able to both
     * cross the panel and land on a 20 px target.
     */
    int acceleration = 45;

    /* Per cent of half travel ignored around centre.  An ADC joystick with no
     * calibration does not read its own centre as centre. */
    int deadzone = 16;

    /* The window two presses have to fall inside to be a double click. */
    int doubleClickMs = 400;

    /* Idle seconds before the pointer fades out.  Four, as asked for. */
    int hideSeconds = 4;

    /* Off means the right stick goes back to being a second D-pad. */
    bool enabled = true;

    /* Swap the two pointer buttons, which is what a left-handed user wants and
     * costs one branch to offer. */
    bool leftHanded = false;
};

class Settings : public QObject
{
    Q_OBJECT

public:
    static Settings &instance();

    const MouseConfig &mouse() const { return m_mouse; }
    void setMouse(const MouseConfig &config);

    /* The Wi-Fi interface the user picked, when the board has more than one.
     * Empty means "whichever /sys/class/net says is wireless". */
    QString wifiInterface() const { return m_wifiInterface; }
    void setWifiInterface(const QString &iface);

    /* Where the last Media page browse ended up, so it opens where it was left
     * rather than at the top of the card every time. */
    QString mediaRoot() const { return m_mediaRoot; }
    void setMediaRoot(const QString &path);

    /*
     * How the music player walks the queue: 0 plays the directory through and
     * stops, 1 goes back round, 2 repeats the one track.  An int and not an enum
     * for the same reason the language is a code -- this file gets read on a PC,
     * and Settings has no business knowing MediaPage's enum either way.  Anything
     * outside 0..2 is clamped on load, so a hand-edited file cannot wedge the
     * player in a state it has no row for.
     */
    int mediaRepeat() const { return m_mediaRepeat; }
    void setMediaRepeat(int mode);

    bool mediaShuffle() const { return m_mediaShuffle; }
    void setMediaShuffle(bool on);

    /*
     * How the user arranged the cards on the Apps grid, as AppEntry keys in slot
     * order.
     *
     * KEYS AND NOT INDICES, because the set of cards is not fixed: Doom appears
     * when an IWAD is on the card and packages will add their own, and a saved
     * list of slot numbers would silently mean something different the moment the
     * number of cards changed.  A key that no longer matches anything is skipped
     * on load, and a card whose key is not in this list lands at the end -- so
     * this file never has to be migrated and never has to be complete.
     *
     * QStringList, which QSettings writes as a comma-separated value.  That is
     * deliberate too: the file is a plain INI on a partition somebody will mount
     * on a PC, and `cards=doom, terminal, files' explains itself there.
     */
    QStringList cardOrder() const { return m_cardOrder; }
    void setCardOrder(const QStringList &keys);

    /*
     * Screen brightness as a percentage, or -1 for "nobody has moved it here".
     *
     * It is remembered because nothing else remembers it.  The backlight driver
     * adopts the duty the MVII loader left in the BLS block rather than picking a
     * level of its own, and the loader always hands over at full -- so without
     * this, a brightness the user chose would silently go back to maximum on the
     * next boot.  -1 is meaningfully different from any percentage: it means
     * leave the panel exactly as the loader set it.
     */
    int brightness() const { return m_brightness; }
    void setBrightness(int percent);

    /*
     * The interface language, as a two-letter ISO 639-1 code.
     *
     * A CODE AND NOT AN ENUM, because this file is a plain INI on a partition
     * somebody will eventually mount on a PC: "language=fr" says what it is, and
     * it keeps meaning that when a seventh language is inserted in the middle of
     * Lang::Id.  Empty means nobody has chosen -- which is not the same as having
     * chosen English, and is what lets the first boot follow the environment.
     */
    QString language() const { return m_language; }
    void setLanguage(const QString &code);

    /*
     * The IANA time zone the user picked on the map, "Europe/Paris".
     *
     * REMEMBERED HERE AS WELL AS IN /etc, and the duplication is the point.  The
     * real home of a time zone is /etc/localtime, which is a symlink into
     * /usr/share/zoneinfo -- but /init mounts the OS partition read-only on some
     * of the recovery paths in this bring-up, and on those boots the symlink
     * cannot be replaced.  This file lives wherever path() found room, so the
     * choice survives on a read-only rootfs as a TZ the dashboard sets on itself
     * at startup: the clock in the status bar is then right even when the rest of
     * the system's idea of local time is not.  Empty means nobody has chosen and
     * whatever /etc/localtime says stands, which is what a device that was never
     * taken to the Region page should do.
     */
    QString timezone() const { return m_timezone; }
    void setTimezone(const QString &zone);

    /*
     * Whether the built-in speaker should be on when there is nothing in the
     * headphone jack.
     *
     * AN INTENTION AND NOT A MIXER STATE, which is the entire reason it is in
     * this file rather than being read back off the card.  Plugging headphones
     * in switches the speaker off, and the only honest thing to switch back on
     * unplugging is what the user had chosen BEFORE the plug -- so that choice
     * has to be recorded somewhere the plug does not touch.  Reading the
     * "Speaker Amp" control at that moment would answer "off", because that is
     * what the insert just made it, and the speaker would never come back.
     *
     * IT ALSO OUTLIVES alsa-restore.  The mixer state is saved at shutdown and
     * replayed at boot, so a device switched off with headphones in comes back
     * up with the speaker muted and nothing in the jack -- silent, with no
     * indication why.  The shell applies this at startup against what the jack
     * actually says, and that is the boot that fixes itself.
     *
     * Default true: a handheld whose speaker is off by default is a handheld
     * that appears broken.
     */
    bool speakerWanted() const { return m_speakerWanted; }
    void setSpeakerWanted(bool on);

    /* The file the settings actually landed in, for the Settings page to print. */
    QString path() const;
    /* False when even the tmpfs fallback would not open, which is worth saying
     * out loud rather than silently discarding every change. */
    bool writable() const { return m_writable; }

signals:
    void mouseChanged();

private:
    Settings();

    void load();

    QSettings *m_store = nullptr;
    MouseConfig m_mouse;
    QString m_wifiInterface;
    QString m_mediaRoot;
    QString m_language;
    QString m_timezone;
    QStringList m_cardOrder;
    int m_brightness = -1;
    int m_mediaRepeat = 0;
    bool m_mediaShuffle = false;
    bool m_speakerWanted = true;
    bool m_writable = false;
};

#endif /* MIXDASH_SETTINGS_H */
