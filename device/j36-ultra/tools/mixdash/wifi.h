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

    QString nmcli(const QStringList &args, int timeoutMs = 4000,
                  int *exitCode = nullptr, QString *errorOut = nullptr) const;
    bool busy() const;
    void startAction(const QStringList &args, const QString &note);

    void refreshStatus();
    void refreshScan();
    void refreshProfiles();
    QString ssidOfProfile(const QString &uuid);
    void rebuild();

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

    QVector<Ap> m_aps;
    QVector<Profile> m_profiles;
    QHash<QString, QString> m_ssidCache;   /* profile UUID -> SSID */

    bool m_managerUp = false;   /* NetworkManager answered at all */
    bool m_radioOn = true;      /* nmcli radio wifi */
    int m_deviceState = -1;     /* NMDeviceState, numeric: 100 is activated */
    QString m_ssid;             /* what NetworkManager says we are on */
    QString m_address;          /* IP4.ADDRESS[1], with its prefix */
    QString m_gateway;
    QString m_note;             /* one line under the title: what just happened */

    /* The AP a passphrase is being typed for. */
    Ap m_pending;
    bool m_awaitingKey = false;

    /* Rescans are asynchronous: ask for one, then read the list on later polls. */
    QElapsedTimer m_scanAge;
    /* Profiles change rarely; do not spend a process on them every two seconds. */
    int m_profileAge = 0;
    /* SSID-HEX was added to nmcli long ago, but an older one must not go blind. */
    bool m_ssidHex = true;
};

#endif /* MIXDASH_WIFI_H */
