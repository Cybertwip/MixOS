/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * sharing.h -- the device's storage, on the network, over SMB.
 *
 * WHAT IT IS FOR.  This is a handheld with one USB port and a card slot, and
 * every way of getting a file onto it up to now has been "power it off and take
 * the card out".  A share turns that into a drag and drop from a PC on the same
 * Wi-Fi: /home/virtua for the things the dashboard itself reads -- media, ROMs,
 * IWADs -- and /media for whatever is plugged into the port.
 *
 * /home/virtua IS THE DATA PARTITION, and that is the reason it is the share
 * rather than one directory inside it.  p3 on the card is ext2, labelled DATA,
 * and the rootfs fstab mounts it rw at /home/virtua -- so exporting that path
 * exports the whole of the partition the card is carrying for the user, roms/
 * and all, and nothing of the rootfs.  It is also the one partition the image
 * writer is told never to overwrite, which makes it the only place on the card
 * where dropping a file from a PC is a decision that survives a reflash.
 *
 * WHY /media AS ONE SHARE AND NOT A SHARE PER VOLUME.  Because the volumes are
 * not known when the configuration is written.  setup_automount's udev rule
 * mounts each one under /media/<label> as it arrives, and to smbd a mount point
 * below an exported path is just a directory -- it descends into it like any
 * other.  So a stick plugged in five minutes after the share was started shows up
 * inside \\mixos\Disks with no configuration change and no restart.  The
 * alternative, rewriting smb.conf and reloading smbd from a udev hook, is a
 * moving part that exists only to reproduce what the filesystem already does.
 *
 * WHICH IS WHY THIS SHIPS BEFORE THE PORT WORKS.  The USB stack on this board
 * does not enumerate anything yet -- see j36_mt6592_usb_phy.c and the DRVVBUS
 * note in j36_mt6592_pmic.c -- and none of that is this file's problem.  The
 * share is over the radio; the disks appear underneath it if and when the port
 * starts producing them.  Nothing here has to change on the day it does.
 *
 * SAMBA IS ALREADY ON THE CARD.  It is line 83 of needed_packages.txt and has
 * been for as long as this tree has existed, with winbind and samba-ad-dc
 * disabled in finishing_touches.sh.  What was never there was a configuration or
 * any way to turn it on, so smbd sat installed and stopped, sharing Debian's
 * default -- which is printers.  This page is that missing half.
 *
 * ── THE PASSWORD IS GENERATED ON THE DEVICE, AND THAT IS DELIBERATE ──────────
 *
 * finishing_touches.sh removed the old access-point configuration for exactly one
 * reason, written down there: it shipped a fixed WPA passphrase, so every card
 * ever built from this tree had the same one, and it was in public git history.
 * A share is the same shape of mistake with a bigger blast radius -- a writable
 * export of somebody's home directory, on whatever network the handheld last
 * joined.
 *
 * So there is no password in this file.  The first time sharing is switched on,
 * twelve characters come out of /dev/urandom, go to smbpasswd, and are written to
 * /etc/mixos/sharing.pass at mode 0600.  The page shows them, because a
 * credential you cannot read is a credential you cannot use, and this device has
 * no other screen to put it on.  "New password" throws it away and makes another.
 *
 * GUEST ACCESS IS NOT OFFERED.  It is one line of smb.conf and it would make the
 * page simpler, and it would also mean that joining a cafe's Wi-Fi published a
 * writable copy of /home/virtua to everyone on it.
 *
 * AND THE CARD ARRIVES WITH EXACTLY THAT ON IT.  finishing_touches.sh appends two
 * shares to Debian's smb.conf -- [opt] on /opt and [home] on the DATA partition --
 * and both carry `guest ok = yes'.  They are harmless today only because smbd is
 * disabled and nothing in the shell could ever start it; this page is the thing
 * that changes that, so it cannot leave that file in place.  Every path here that
 * makes smbd run -- the switch, and enabling it for the next boot -- goes through
 * ensureConfigured() first, which replaces the file unless it already carries this
 * page's marker line.  configIsOurs() is that check, and when it says no while
 * smbd is up anyway, the page says so on the glass rather than claiming a
 * password protects a share that is open to the network.
 *
 * finishing_touches.sh itself is left alone deliberately: it is the rootfs
 * pipeline every board in this tree shares, and turning guest access off there
 * would change what an R36 card does with no one having asked.
 *
 * WHAT DRIVES SMBD.  systemctl, and only systemctl -- start, stop, enable,
 * disable, is-active, is-enabled.  The same reasoning as wifi.h's on wpa_cli:
 * smbd's lifecycle belongs to the init system, every command here can be checked
 * by hand over ssh, and a page that forked its own daemon would be a second
 * opinion about whether the share is up.
 */
#ifndef MIXDASH_SHARING_H
#define MIXDASH_SHARING_H

#include <QString>
#include <QStringList>

#include "widgets.h"

class ListPane;
class QTimer;

class SharingPage : public PageWidget
{
    Q_OBJECT

public:
    explicit SharingPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Sharing"); }
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void poll();

private:
    /* What a row stands for.  In ListRow::id, for the same reason wifi.cpp does
     * it: the pane is a dumb list and the page is what knows the meaning. */
    enum RowId {
        IdInert = 0,
        IdShare,          /* the switch that starts and stops smbd */
        IdAtBoot,         /* systemctl enable/disable */
        IdNewPassword,
        IdRewrite,        /* put smb.conf back the way this page writes it */
        IdShowPassword
    };

    /* ── the pieces underneath ── */

    static QString systemctl(const QStringList &args, int timeoutMs = 4000);
    static bool unitActive(const QString &unit);
    static bool unitEnabled(const QString &unit);
    static bool sambaInstalled();
    static QString hostName();
    /* Every IPv4 address that is not loopback, in the order getifaddrs gives
     * them.  Plural because a board with Wi-Fi up and a USB-Ethernet adapter
     * plugged in has two, and the one to type is not knowable from here. */
    static QStringList addresses();
    /* One line per volume currently mounted below /media, label only. */
    static QStringList mountedVolumes();

    /* Write /etc/samba/smb.conf.  Returns an empty string on success and the
     * reason on failure, because that reason is going on the glass. */
    QString writeConfig();
    /* True when smb.conf carries this page's marker line.  False means the file
     * is Debian's plus finishing_touches.sh's two guest-readable shares, which is
     * what a card that has never had sharing switched on is carrying. */
    static bool configIsOurs();
    /* writeConfig() unless the file is already ours, then a password unless there
     * already is one.  Every path that makes smbd run calls this first. */
    QString ensureConfigured();
    /* Make sure the samba account exists and holds the stored password. */
    QString applyPassword(const QString &password);
    QString storedPassword() const;
    QString generatePassword() const;
    QString newPassword();

    void start();
    void stop();
    void rebuild();

    ListPane *m_list = nullptr;
    QTimer *m_timer = nullptr;

    bool m_active = false;
    bool m_enabled = false;
    bool m_reveal = false;      /* the password is masked until it is asked for */
    QString m_note;             /* one line under the title: what just happened */
};

#endif /* MIXDASH_SHARING_H */
