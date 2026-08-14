/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * wifi.h -- joining a network from the glass.
 *
 * WHAT IS UNDERNEATH.  wpa_supplicant is already running as a system service in
 * this image, with a control socket in /run/wpa_supplicant.  This page is a
 * front end to that socket by way of wpa_cli, and it is deliberately nothing
 * more: the supplicant owns the association, the four-way handshake and the
 * saved network list, and duplicating any of that here would mean two things
 * disagreeing about what the device is connected to.
 *
 * WHY wpa_cli AND NOT THE SOCKET DIRECTLY.  The control protocol is a handful of
 * lines over a unix datagram socket and writing a client for it is maybe two
 * hundred lines -- but wpa_cli is on the card, it is the reference client, and
 * every reply this file parses can be checked by hand over ssh with the same
 * command.  On a board where the interesting failures are in the radio and not
 * in the parser, that is worth more than the dependency costs.
 *
 * THE ADDRESS IS A SEPARATE PROBLEM.  wpa_supplicant associates; it does not do
 * DHCP.  Debian normally has ifupdown or networkd do that, and this image has
 * neither wired to the wireless interface, so after the association completes
 * this page runs whichever DHCP client is on the card.  That is the step whose
 * absence looks exactly like "the Wi-Fi does not work".
 */
#ifndef MIXDASH_WIFI_H
#define MIXDASH_WIFI_H

#include <QElapsedTimer>
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
    /* One access point, as scan_results reported it. */
    struct Ap {
        QString ssid;
        QString bssid;
        QString flags;
        int signalDbm = -100;
        int frequency = 0;
        bool saved = false;
        int networkId = -1;
        bool current = false;
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
        RowStartSupplicant
    };

    QString cli(const QStringList &args, int timeoutMs = 2500) const;
    bool supplicantUp() const;
    void bringInterfaceUp();
    void unblockRadio();

    void refreshStatus();
    void refreshScan();
    void rebuild();

    void connectTo(const Ap &ap);
    void connectWithKey(const Ap &ap, const QString &passphrase);
    QString psk(const QString &ssid, const QString &passphrase) const;
    void startDhcp();
    QString ipv4() const;

    static bool isOpenNetwork(const QString &flags);
    static QString security(const QString &flags);
    static int quality(int dbm);

    QString m_iface;
    ListPane *m_list = nullptr;
    QTimer *m_timer = nullptr;
    QProcess *m_dhcp = nullptr;

    QVector<Ap> m_aps;
    QString m_state;        /* wpa_state, verbatim */
    QString m_ssid;         /* what we are associated with, if anything */
    QString m_address;
    QString m_note;         /* one line under the title: what just happened */

    /* The AP a passphrase is being typed for. */
    Ap m_pending;
    bool m_awaitingKey = false;

    /* Scans are asynchronous: fire one, then read results on the next few polls. */
    QElapsedTimer m_scanAge;
    bool m_scanPending = false;
    /* Polls spent COMPLETED with no address before DHCP is started. */
    int m_addressWait = 0;
    bool m_dhcpTried = false;
};

#endif /* MIXDASH_WIFI_H */
