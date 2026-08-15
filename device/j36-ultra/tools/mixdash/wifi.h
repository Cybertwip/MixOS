/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * wifi.h -- joining a network from the glass.
 *
 * WHAT IS UNDERNEATH, AND WHAT THIS FILE USED TO GET WRONG.  NetworkManager runs
 * in this image and it takes wlan0 in full:
 *
 *     device (wlan0): state change: unmanaged -> unavailable
 *                     (reason 'managed', managed-type: 'full')
 *
 * It starts wpa_supplicant itself, over D-Bus, and drives it.  This page used to
 * talk to that same supplicant behind NetworkManager's back with wpa_cli, and the
 * two bugs that came out of it are worth naming, because both of them looked like
 * radio faults and neither one was.
 *
 *   The strange address.  An association made behind NetworkManager's back is one
 *   it has no connection object for, so the device stays `disconnected' as far as
 *   it is concerned: it never runs its DHCP client, it goes on scanning on the
 *   disconnected cadence -- a thirteen-channel sweep every few seconds, which on
 *   this radio takes the antenna off the AP's channel -- and it flushes addresses
 *   it did not put there.  This page then started a DHCP client of its own, whose
 *   requests went out into those gaps and were never answered.  dhcpcd's reply to
 *   a lease that never arrives is IPv4LL: it assigns 169.254.x.x/16 and calls it a
 *   success.  That is the "strange IP not related to the network we are on" -- not
 *   a mask the router handed out, but the address a DHCP client picks when it has
 *   given up on the router entirely.
 *
 *   The network that was never saved.  The supplicant NetworkManager starts is
 *   configured over D-Bus and has no configuration file, so `save_config' has
 *   nothing to write to and fails; wpa_cli's added networks lived in the
 *   supplicant's memory and went away with the next reboot.  There was nothing on
 *   the card for anything to auto-reconnect to.
 *
 * SO THERE IS ONE OWNER NOW.  Every operation here goes through NetworkManager,
 * which means it owns the association, the four-way handshake, the DHCP lease, the
 * routes, the resolver, the saved profile and the reconnect.  This page selects
 * and reports; it does not configure the interface.
 *
 * WHY nmcli AND NOT libnm.  libnm is the better API and it is the wrong dependency
 * here: it would pull GLib's main loop into a Qt process for four queries and a
 * verb.  Every command this file runs can be pasted into an ssh session and
 * checked by hand, which on a board whose interesting failures are in the radio is
 * worth more than the type safety.
 *
 * THE PASSPHRASE IS NEVER AN ARGUMENT.  `nmcli device wifi connect ... password X'
 * would be one line, and it puts the key in /proc/<pid>/cmdline where every user
 * on the box can read it for as long as the process lives.  A key that is briefly
 * public is public.  So a new network is joined by writing the keyfile profile
 * ourselves, 0600, through a dotfile that NetworkManager's directory watcher
 * ignores and an atomic rename -- and then only the profile's name and UUID ever
 * appear in an argument list.
 *
 * THE POLL DOES NOT BLOCK ANY MORE, AND THAT WAS NOT A TIDYING-UP.  This page used
 * to run its three status queries the way every other page in this program runs
 * its shell-outs: QProcess, Shell::waitForStarted, Shell::waitForFinished, one
 * after another, straight out of the poll timer.  shell.h is explicit that such a
 * wait does not pump the event loop -- "no timer fires inside it, no key is read,
 * nothing repaints" -- and the arithmetic that made that acceptable everywhere
 * else does not hold for this tool.  nmcli is not a small program: it links libnm,
 * gio, glib and dbus, and on eight Cortex-A7s at this clock a single run costs a
 * couple of hundred milliseconds before it has answered anything.  Three of them
 * per two-second tick is most of a second of frozen panel, every two seconds, for
 * as long as the page is open -- which is precisely what "the OS stays stuck for a
 * brief moment every few cycles" is from the outside.  It only appeared when this
 * page moved to NetworkManager because what it replaced, wpa_cli, is a couple of
 * hundred kilobytes with no GLib behind it and starts in about a millisecond.
 *
 * So the periodic queries go through a queue instead: at most one child alive at a
 * time, started and reaped by signal, results folded into the page's state when
 * they arrive, and the panel repainting throughout.  The queue de-duplicates, so a
 * tick that arrives while the previous one is still draining adds nothing.  The
 * only calls left synchronous are the ones a person just pressed a button to
 * cause -- deleting a profile, reloading them -- where a short pause is the
 * expected shape of a button and the state has to be current before the next line
 * runs.
 */
#ifndef MIXDASH_WIFI_H
#define MIXDASH_WIFI_H

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "widgets.h"

class ListPane;
class QProcess;
class QTimer;

class WifiPage : public PageWidget
{
    Q_OBJECT

public:
    explicit WifiPage(QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;
    void textEntered(const QString &text, bool accepted) override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void poll();

private:
    /* One access point, as `nmcli device wifi list' reported it. */
    struct Ap {
        QString ssid;
        QString ssidHex;    /* the SSID's bytes, from nmcli's SSID-HEX column */
        QString bssid;
        QString security;   /* "WPA2", "WPA2 WPA3", "WPA1 WPA2 802.1X", empty if open */
        int signal = 0;     /* per cent, which is what nmcli reports -- not dBm */
        int frequency = 0;  /* MHz */
        bool saved = false;
        bool current = false;
        QString uuid;       /* the saved profile for this SSID, if there is one */
    };

    /* A saved connection profile, as `nmcli connection show' reported it. */
    struct Profile {
        QString name;
        QString uuid;
        QString ssid;
    };

    /* What a row in the pane stands for.  Encoded in ListRow::id because the
     * pane is a dumb list and the page is what knows the meaning. */
    enum RowKind {
        RowNetwork = 0,
        RowRescan = 1000,
        RowDisconnect,
        RowForget,
        RowReconnect,
        RowEnableRadio,
        RowStartManager,
        RowManage
    };

    /*
     * One background query, and what it is asking about.  The id is what tells
     * the finished handler which parser to hand the bytes to; `arg' carries the
     * one query that is about a particular thing rather than about the device.
     */
    enum QueryId {
        QueryRadio = 1,     /* is NetworkManager there, is the radio on */
        QueryDevice,        /* state, connection, address, gateway */
        QueryProfiles,      /* the saved networks */
        QuerySsid,          /* arg = profile UUID: its 802-11-wireless.ssid */
        QueryScan,          /* the AP list NetworkManager already has */
        QueryRescan         /* ask for a fresh sweep; nothing to parse */
    };

    struct Query {
        int id = 0;
        QString arg;
        int timeoutMs = 4000;
    };

    QString nmcli(const QStringList &args, int timeoutMs = 4000,
                  int *exitCode = nullptr, QString *errorOut = nullptr) const;
    bool busy() const;
    void startAction(const QStringList &args, const QString &note);

    QStringList queryArgs(const Query &query) const;
    void enqueue(int id, const QString &arg = QString());
    void pumpQueries();
    void queryFinished(int exitCode, bool answered);

    void applyRadio(int exitCode, const QString &out);
    void applyDevice(int exitCode, const QString &out);
    void applyProfiles(int exitCode, const QString &out);
    void applySsid(const QString &uuid, int exitCode, const QString &out);
    void applyScan(int exitCode, const QString &out);
    void matchProfiles();

    void refreshStatus();
    void refreshScan();
    void refreshProfiles();
    /* The blocking one.  Only the forget path uses it, because that path has to
     * know the current profiles before it can delete the right ones. */
    void syncProfiles();
    void rebuild();

    /*
     * What went wrong, in the language the device is set to.  `reasonIn' digs the
     * NMDeviceStateReason out of the bracketed number nmcli prints in front of its
     * own sentence -- "Error: Connection activation failed: (7) Secrets were
     * required, but not provided." -- and failureText() turns that number, or the
     * exit status when there is no number, into one line worth reading.
     */
    static int reasonIn(const QString &text);
    QString failureText(int exitCode, const QString &err) const;

    void connectTo(const Ap &ap);
    void connectWithKey(const Ap &ap, const QString &passphrase);
    bool writeProfile(const Ap &ap, const QString &passphrase, QString *uuidOut,
                      QString *errorOut);
    void forgetProfilesFor(const QString &ssid);

    static QStringList splitTerse(const QString &line);
    static QString keyfileEscape(const QString &value);
    static QString ssidBytes(const Ap &ap);
    static int quality(int percent);

    QString m_iface;
    ListPane *m_list = nullptr;
    QTimer *m_timer = nullptr;
    QProcess *m_action = nullptr;   /* the one long verb in flight, if any */

    /* The background query pump.  m_query is reused for every query; m_guard
     * kills one that will not answer, because QProcess has no timeout of its
     * own once you have stopped waiting on it. */
    QProcess *m_query = nullptr;
    QTimer *m_guard = nullptr;
    QVector<Query> m_queue;
    Query m_inFlight;
    bool m_querying = false;

    QVector<Ap> m_aps;
    QVector<Profile> m_profiles;
    QHash<QString, QString> m_ssidCache;   /* profile UUID -> SSID */

    bool m_managerUp = false;   /* NetworkManager answered at all */
    bool m_radioOn = true;      /* nmcli radio wifi */
    int m_deviceState = -1;     /* NMDeviceState, numeric: 100 is activated */
    bool m_entryScanPending = false;  /* onEnter asked; applyDevice decides */
    QString m_ssid;             /* what NetworkManager says we are on */
    QString m_address;          /* IP4.ADDRESS[1], with its prefix */
    QString m_gateway;
    QString m_note;             /* one line under the title: what just happened */

    /* The AP a passphrase is being typed for. */
    Ap m_pending;
    bool m_awaitingKey = false;

    /*
     * The network whose SAVED passphrase NetworkManager just told us was refused.
     * Without this the page is a trap: a profile with the wrong key is still a
     * saved profile, so pressing its row runs `connection up' against the same
     * wrong key for ever and there is no way left to type a new one.  While an
     * SSID is in here its row ignores the saved profile and asks again.
     */
    QString m_badKeySsid;

    /* Rescans are asynchronous: ask for one, then read the list on later polls. */
    QElapsedTimer m_scanAge;
    /* Profiles change rarely; do not spend a process on them every two seconds. */
    int m_profileAge = 0;
    /* Ticks since the page opened, so the heavy queries can run on a slower beat
     * than the cheap ones without a second timer. */
    int m_tick = 0;
    /* SSID-HEX was added to nmcli long ago, but an older one must not go blind. */
    bool m_ssidHex = true;
};

#endif /* MIXDASH_WIFI_H */
