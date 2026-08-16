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
 * Wi-Fi: /home/virtua for the things the dashboard itself reads -- media and
 * ROMs -- and /media for whatever is plugged into the port.
 *
 * /home/virtua IS THE USER'S HALF OF THE CARD, and that is the reason it is the
 * share rather than one directory inside it.  Exporting that path exports
 * everything the dashboard reads on the operator's behalf -- media, roms/ -- and
 * nothing of the rootfs around it.
 *
 * IT USED TO BE A PARTITION.  There was a p3, ext2, labelled DATA, mounted rw at
 * /home/virtua, and it was the one partition the image writer was told never to
 * overwrite.  It is gone: ROOTFS is the last partition on the disk now so that
 * the initramfs can grow it to the end of the card, and /home/virtua is an
 * ordinary directory on it.  What that costs is the reflash guarantee -- a
 * dropped file no longer survives one -- and what it buys is the whole card
 * instead of a fixed slice of it.  Nothing about this share changes either way;
 * it exports a path and never cared what was mounted under it.
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
 *
 * ── WHY THE SWITCH USED TO FLICK STRAIGHT BACK OFF ───────────────────────────
 *
 * It did, on this board, every time, and the line under the title said "smbd would
 * not start" -- which was not true and was the least useful sentence available.
 * Three separate things were wrong and all three are fixed here, because any one of
 * them on its own is enough to reproduce the report.
 *
 * ONE: THE START WAS SAMPLED, NOT WAITED FOR.  smbd.service is Type=notify, so
 * `systemctl start smbd' does not return until smbd itself has told systemd it is
 * ready.  Before it gets that far the unit runs testparm THREE times -- once for
 * its own ExecCondition and twice inside the ExecStartPre apparmor script -- and
 * each of those is a cold dynamic link of forty-odd samba libraries off an SD card
 * on a Cortex-A7.  The old code allowed the whole sequence fifteen seconds, killed
 * the systemctl client when that ran out, read is-active ONCE, and called what it
 * got the answer.  What it got was `activating'.  So the switch went back to off
 * while smbd was still coming up behind it, and the note said the daemon had
 * refused.  Now a start that has not finished within the budget is a start still in
 * progress: the page says so, polls faster until the unit settles, and only calls
 * it a failure when systemd has.
 *
 * TWO: A SKIPPED UNIT LOOKS EXACTLY LIKE A SUCCESSFUL ONE.  Both units carry
 * `ExecCondition=/usr/share/samba/is-configured <smb|nmb>', and to systemd a
 * non-zero ExecCondition is not an error -- it is a reason to SKIP the unit.
 * `systemctl start smbd' exits 0 with no output, the unit stays inactive, and there
 * is nothing anywhere to read.  That script's first line is
 * `[ -f /etc/samba/smb.conf ] || exit 1', and its second is a testparm.  So a
 * missing or torn smb.conf produced a silent, reasonless refusal.  The failure path
 * now asks systemd for Result= and says so in words, and runs testparm itself to
 * quote samba's own complaint about the file.
 *
 * THREE: THE FILE WAS WRITTEN WITHOUT LOOKING.  writeConfig() ended with
 * `s.flush(); f.close(); return QString();' -- success, unconditionally, whatever
 * had become of the bytes.  On the rootfs this image shipped with there were about
 * forty megabytes free to a non-root writer and two hundred to root, and a card
 * that fills up mid-write leaves a truncated smb.conf and a page reporting that all
 * is well.  Truncated is the worst of the three states the file can be in: samba
 * skips itself over it (see TWO), so the symptom is once again a switch that will
 * not move and nothing said.  Both writers check now -- the config and the password
 * file -- and configIsOurs() no longer accepts a file that has this page's marker
 * on line one and nothing after it.
 *
 * ── AND IT STILL DID, SO: FOUR, FIVE AND SIX ─────────────────────────────────
 *
 * The report came back unchanged after all of the above -- "it does not share, it
 * just turns off every time I enable it" -- and with no reason quoted, which is
 * itself the finding.  Three of the four things wrong were about that.
 *
 * FOUR: THE REASON WAS WRITTEN WHERE NOBODY COULD READ IT.  m_note is one line of
 * twelve-point text in an eighteen-pixel strip on a 640 px panel, drawn with
 * drawText and no elide, and most of the sentences above are longer than that.  So
 * the half of the diagnosis that says WHY was painted past the right edge of the
 * card and thrown away by the clip.  The strip takes two lines now when it needs
 * them and elides the last one, which is the difference between a diagnosis and a
 * fragment of one.
 *
 * FIVE: AND IT DID NOT SURVIVE THE PAGE BEING LEFT.  onEnter() clears m_note --
 * rightly, a stale complaint is worse than none -- so the moment the operator
 * backed out to look at the Wi-Fi page the only record of the failure on the whole
 * device was gone.  There is no journal to fall back on: cleanup_filesystem.sh
 * deletes /var/log/journal, so journald keeps the boot's log in /run and it dies
 * with the power.  Every failed start now writes what it knows to
 * /boot/mixos-sharing.txt -- p1, vfat, the one filesystem on this card that the PC
 * the operator is standing at can read -- and there is a row that writes the same
 * report on demand.  That report carries what nothing on the glass could: smbd's
 * own journal lines, systemd's Result and ConditionResult, testparm's complaint,
 * and the smb.conf that produced them.
 *
 * SIX: THE START BLOCKED THE PANEL FOR FIFTEEN SECONDS TO LEARN NOTHING.  The wait
 * existed so that the state read after it would be the final one, and on this board
 * it never was -- that is bug ONE, and the watch that fixed it made the wait
 * pointless rather than merely slow.  It is `--no-block' now: systemd takes the
 * job, the page starts watching immediately, and the fifteen seconds in which the
 * dashboard could not repaint or read the pad are gone.  nmbd moved with it -- it
 * is started only once smbd is actually up, because smbd.service carries
 * `After=nmb.service' and a queued nmbd job is therefore something a queued smbd
 * job can be made to wait for.  The share does not need the NetBIOS name; it needs
 * smbd.
 *
 * ── AND ONCE MORE: SEVEN AND EIGHT, WHICH ARE THE SAME MISTAKE TWICE ─────────
 *
 * "Sharing does not work, the knobs reset, it never shares over the network,
 * although we're connected to internet."  KNOBS, plural, and the plural is the
 * finding: bugs ONE to SIX are all about the start, and there is a second switch on
 * this page that has nothing to do with starting anything.
 *
 * SEVEN: "I COULD NOT ASK" WAS DRAWN AS "OFF".  systemctl() returns an empty string
 * for a fork that timed out or never started, and the note above it called every
 * caller reading that as "no" a deliberate bargain.  It is -- for is-active, where a
 * systemd nobody can reach is genuinely not a share anybody can open.  It is not for
 * is-enabled: "start at boot" is a symlink on the card, and a page that could not
 * read the card has learnt nothing about the setting.  poll() assigned BOTH flags
 * from those queries every four seconds, so one slow fork redrew both switches in
 * the off position and the next tick put them back -- which, watched from the sofa,
 * is a knob that resets.
 *
 * And a slow fork is the ordinary case here, not the exceptional one.  The budget
 * was one second to get systemctl RUNNING; systemctl links libsystemd-shared, which
 * is several megabytes off an SD card on a Cortex-A7, and Shell's waits spend part
 * of that same budget repainting a 640x480 panel in software between slices.  So the
 * fix is in three places: the budget is four seconds, the answer carries whether it
 * IS an answer, and a tick that did not get one leaves both switches where they
 * were.
 *
 * EIGHT: A QUEUED JOB WAS TIMED WITH A STOPWATCH INSTEAD OF ASKED ABOUT.  A start
 * job PID 1 has accepted but not begun leaves ActiveState at `inactive' -- settled,
 * indistinguishable from a refusal -- and poll() gave that three seconds' benefit of
 * the doubt before calling the start dead.  Three seconds is a guess about how long
 * systemd takes to dequeue, and it is wrong whenever the job is ordered behind
 * something: smbd.service is `After=network-online.target nmbd.service
 * winbind.service', and any of those still moving holds the job at `inactive' for as
 * long as it takes.  systemd publishes the job -- `Job=' in `systemctl show' -- so
 * the page asks rather than counts, and the three-second window is gone.
 *
 * Both of those come out of the same fork now.  A tick used to be two systemctl
 * runs, is-active and is-enabled; it is one `show' with three properties in it,
 * which halves the exposure to the very slowness that caused SEVEN.
 */
#ifndef MIXDASH_SHARING_H
#define MIXDASH_SHARING_H

#include <QMap>
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
        IdShowPassword,
        IdDiagnose        /* everything this page knows, onto the BOOT partition */
    };

    /* ── the pieces underneath ── */

    /* `rc', when asked for, is the child's exit code -- or -1 when it never ran or
     * never finished, which is not the same answer as "it ran and said no". */
    static QString systemctl(const QStringList &args, int timeoutMs = 4000,
                             int *rc = nullptr);
    /* What `is-active' printed, verbatim: active, activating, deactivating,
     * reloading, inactive, failed -- or empty when the query itself did not
     * answer.  The distinction between the first two is the whole of bug ONE. */
    static QString activeState(const QString &unit);
    static bool unitActive(const QString &unit);
    /*
     * `answered', when asked for, says whether systemd was reached AT ALL -- which
     * is not the same question as whether the unit is enabled, and conflating the
     * two is bug SEVEN.  A fork that never started returns false with *answered
     * false, and a caller that draws a switch from it must keep what it had.
     */
    static bool unitEnabled(const QString &unit, bool *answered = nullptr);
    /* True while the unit is still moving, so waiting longer might change the
     * answer.  Anything else has settled and will not. */
    static bool stateIsTransient(const QString &state);

    /*
     * ── WHAT ONE POLL TICK NEEDS, OUT OF ONE FORK ────────────────────────────
     *
     * `answered' is the field that matters and the one that did not exist.  Every
     * query on this page returns empty on a fork that did not run, and empty read
     * as "no" is right for is-active -- a wedged systemd is not a share anybody can
     * reach -- and WRONG for is-enabled, because "start at boot" is a setting on the
     * card and not a live state.  One slow fork redrew it as off.
     *
     * `job' is the other half.  A queued start job that PID 1 has not begun leaves
     * ActiveState at `inactive', which is a settled state and reads as a refusal;
     * the page used to guess its way past that with a three-second stopwatch.
     * systemd will simply say, so it is asked instead of timed.
     */
    struct UnitSnapshot {
        bool answered = false;  /* systemd was reached; the rest means something */
        QString state;          /* ActiveState: active, activating, inactive, ... */
        QString fileState;      /* UnitFileState: enabled, disabled, masked, ... */
        bool job = false;       /* a job for this unit is queued or running */
    };
    static UnitSnapshot readUnit(const QString &unit);
    /* One field out of `systemctl show', right-hand side only. */
    static QString unitProperty(const QString &unit, const QString &prop);
    /* systemd's FragmentPath for the unit: where the .service file really is. */
    static QString unitPath(const QString &unit);
    /* Why a unit that was asked to start is not running.  systemd's Result= put
     * into words, with samba's own complaint about smb.conf when that is what it
     * turns on.  Never empty: an unexplained switch that will not move is the
     * thing this page keeps being reported for. */
    static QString startFailureReason(const QString &unit);
    /* What testparm says about the file on disk, first complaint only, empty when
     * it has none.  Forked only on the failure path -- it is another cold load of
     * the whole samba library stack, which is not a thing to do on the way in. */
    static QString configComplaint();
    /*
     * Several `systemctl show' properties out of ONE fork, as a key=value map.
     * One fork rather than one per property, because the failure path asks for
     * half a dozen of them and every fork on this board is a cold dynamic link.
     */
    static QMap<QString, QString> unitProperties(const QString &unit,
                                                 const QStringList &props);
    /*
     * The last `lines' the journal holds for the unit, oldest first.
     *
     * THIS BOOT ONLY, and that is not a limitation here.  cleanup_filesystem.sh
     * deletes /var/log/journal from the shared rootfs, so journald keeps its ring
     * in /run and the whole log dies with the power -- but the window that matters
     * is the ten seconds after a start that has just refused, and in that window
     * this is the only place on the device that holds smbd's own words.
     */
    static QString journalTail(const QString &unit, int lines);
    /* enable or disable, VERIFIED -- and with the .wants symlink written by hand
     * when systemctl could not.  Empty on success, the reason otherwise. */
    static QString setEnabled(const QString &unit, bool on);
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
    /* Geometry, which is not constant: the note under the title takes one line or
     * two depending on how much there is to say, and a page that reserved two
     * permanently would give a row of the list away to a line that is usually
     * short.  Called from resizeEvent and from the top of rebuild(). */
    void layOut();
    /* What the note strip shows -- m_note when there is one, and the standing
     * description of the state otherwise.  Never empty. */
    QString noteLine() const;
    int noteLines() const;

    /*
     * ── THE PART THAT SURVIVES THE PAGE BEING LEFT ───────────────────────────
     *
     * Every failure this page can describe was, until now, described into m_note
     * -- which onEnter() clears.  So a start that refused while nobody was looking
     * left nothing behind, and "it just turns itself off" is the only report that
     * could be made about it.  There is no journal to go back to either: this image
     * runs journald in /run.
     *
     * So the diagnosis is written to a FILE, and to the one filesystem on this
     * device that a PC can read without any of this working -- p1, vfat, mounted at
     * /boot.  It is the same answer build-in-vm.sh gives for the boot log and for
     * the same reason: the panel is 640x480 and the alternative is photographing it.
     */
    QString collectReport(const QString &reason) const;
    /* collectReport() onto the card.  Returns where it landed, empty when nothing
     * would take it. */
    QString writeReport(const QString &reason);
    /* Remember why, and put it on the card.  Every path that decides a start did
     * not take goes through here. */
    void noteFailure(const QString &reason);

    /* The start did not finish inside start()'s budget, so the page is now
     * watching for it.  Sets the note, speeds the poll up and opens the grace. */
    void beginWatch();
    /* It settled.  `ok' writes the success line; otherwise the reason comes from
     * startFailureReason().  Puts the poll back to its idle pace either way. */
    void endWatch(bool ok);

    ListPane *m_list = nullptr;
    QTimer *m_timer = nullptr;

    bool m_active = false;
    bool m_enabled = false;
    bool m_reveal = false;      /* the password is masked until it is asked for */
    QString m_note;             /* one line under the title: what just happened */

    /* A start is in flight and this page is waiting for it -- see bug ONE in the
     * block at the top of this file.  The grace counts down by one poll interval
     * a tick and exists only as a backstop: a unit that reaches a settled state
     * ends the wait long before it runs out. */
    bool m_starting = false;
    int m_startGraceMs = 0;

    /* Why the last start did not take, kept for the life of the page -- m_note is
     * cleared on every entry and this is not, so the diagnosis row still has
     * something to say about it two visits later. */
    QString m_lastFailure;
    /* Where the last diagnosis was written, so the row can say so instead of
     * making the operator guess at a path. */
    QString m_reportPath;
};

#endif /* MIXDASH_SHARING_H */
