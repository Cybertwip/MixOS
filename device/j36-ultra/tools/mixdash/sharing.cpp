/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * sharing.cpp -- see sharing.h for what this is and why the password is not in
 * this file.
 */
#include "sharing.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QProcess>
#include <QResizeEvent>
#include <QTextStream>
#include <QTimer>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "joypad.h"
#include "shell.h"
#include "theme.h"
#include "widgets.h"

namespace {

/* The account the share is served as.  It is the unix user the dashboard's home
 * belongs to and the one every other page already assumes -- Media opens
 * /home/virtua, Files starts there -- so a share served as anybody else would
 * export a directory tree the rest of the shell cannot write. */
const char kUser[] = "virtua";

/*
 * The DATA partition's mount point.  p3, ext2, labelled DATA, mounted rw here by
 * the rootfs fstab -- so this one path is the whole of the user's half of the
 * card, roms/ included, and /roms is a symlink into it.  Exporting it exports
 * the partition; exporting anything narrower would hide the tree every other
 * page of this shell already works in.
 */
const char kHome[] = "/home/virtua";

/* setup_automount's first choice.  It falls back to /run/media when the rootfs
 * will not take a mkdir, and that case is reported on the page rather than
 * exported: a share pointing into a tmpfs that a reboot empties would look like
 * data loss. */
const char kDisks[] = "/media";

const char kConfig[] = "/etc/samba/smb.conf";
/* The first line writeConfig() emits, and the only way to tell this page's file
 * apart from the one the image ships.  Grepped for rather than stat'ed: a
 * timestamp says when the file changed, not who wrote it. */
const char kMarker[] = "# Written by the MixOS dashboard's Sharing page.";
const char kSecretDir[] = "/etc/mixos";
const char kSecret[] = "/etc/mixos/sharing.pass";

/*
 * TWO UNITS, AND BOTH ARE NAMED EVERYWHERE.  smbd is the file server and is the
 * one that matters; nmbd answers the NetBIOS name lookup that lets a Windows
 * machine find \\mixos without being told an address.  A modern client resolves
 * by mDNS or by typing the IP, so nmbd failing is not a failure of the share --
 * which is why start() treats it as best-effort and the status the page reports
 * is smbd's alone.
 */
const char kSmbd[] = "smbd";
const char kNmbd[] = "nmbd";

QString firstExecutable(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

QString systemctlPath()
{
    /* Named, not built inline: an inline `QStringList() << ...' as a range-for's
     * range is a use-after-free -- the temporary dies at the end of the full
     * expression and the loop then walks what is left of it. */
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/bin/systemctl") << QStringLiteral("/bin/systemctl");
    static const QString found = firstExecutable(paths);
    return found;
}

QString smbpasswdPath()
{
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/bin/smbpasswd") << QStringLiteral("/usr/sbin/smbpasswd");
    static const QString found = firstExecutable(paths);
    return found;
}

/* samba's own configuration checker, out of samba-common-bin -- which the samba
 * package depends on, so it is on the card wherever smbd is.  It is what
 * /usr/share/samba/is-configured runs to decide whether smbd should run at all,
 * which is exactly why this page runs it too: asking the same question the same
 * way is the only way to get the same answer. */
QString testparmPath()
{
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/bin/testparm") << QStringLiteral("/usr/sbin/testparm");
    static const QString found = firstExecutable(paths);
    return found;
}

QString journalctlPath()
{
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/bin/journalctl") << QStringLiteral("/bin/journalctl");
    static const QString found = firstExecutable(paths);
    return found;
}

/*
 * ── WHERE A DIAGNOSIS GOES SO THAT SOMEBODY CAN READ IT ──────────────────────
 *
 * /boot first, and it is the whole point: p1 is vfat, the rootfs mounts it there
 * (`LABEL=BOOT /boot vfat defaults 0 0'), and it is the ONE filesystem on this card
 * that the machine the operator will take it to can open.  p2 is ext2, which is the
 * filesystem that made "send me the log" unanswerable on this board in the first
 * place -- see the boot-log note in build-in-vm.sh, which reaches the same
 * conclusion for the same reason.
 *
 * /var/log second, for a system booted with no BOOT partition mounted -- an NFS
 * root, a card mounted by hand -- where the file is at least fetchable over ssh.
 */
const char kReportName[] = "mixos-sharing.txt";

QStringList reportDirs()
{
    return QStringList() << QStringLiteral("/boot") << QStringLiteral("/var/log");
}

/*
 * Two lines at most, wrapped on a space, and the last one ELIDED rather than
 * clipped.
 *
 * drawText into a rectangle CLIPS, which is what the note strip did: the reason a
 * start failed is a sentence, sentences are longer than 640 pixels of twelve-point
 * text, and the part that got thrown away was reliably the part that named the
 * cause.  A clipped diagnosis reads as no diagnosis, which is exactly the report
 * this page keeps coming back with.
 */
int fitPrefix(const QFontMetrics &fm, const QString &s, int width)
{
    int lo = 0;
    int hi = s.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fm.horizontalAdvance(s.left(mid)) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (lo >= s.size())
        return s.size();
    /* On a space where there is one: a break in the middle of a word reads as a
     * typo, and half the words here are paths. */
    const int space = s.lastIndexOf(QLatin1Char(' '), lo);
    return space > 0 ? space : qMax(1, lo);
}

void paintNote(QPainter &p, const QRect &box, const QString &text, int rows)
{
    const QFontMetrics fm(p.font());
    const int lh = box.height() / qMax(1, rows);
    QString rest = text;

    for (int i = 0; i < rows; ++i) {
        const QRect line(box.x(), box.y() + i * lh, box.width(), lh);
        if (i == rows - 1 || fm.horizontalAdvance(rest) <= box.width()) {
            p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                       fm.elidedText(rest, Qt::ElideRight, box.width()));
            return;
        }
        const int cut = fitPrefix(fm, rest, box.width());
        p.drawText(line, Qt::AlignLeft | Qt::AlignVCenter, rest.left(cut).trimmed());
        rest = rest.mid(cut).trimmed();
    }
}

/*
 * How long the page keeps watching a start that has not finished.
 *
 * Ninety seconds because that is systemd's own DefaultTimeoutStartSec, and neither
 * unit overrides it: declaring failure any earlier would be this page calling a
 * start dead while the init system is still waiting for it, which is the mistake
 * the old fifteen-second sample made in miniature.
 */
const int kStartGraceMs = 90000;

/* Idle, and while a start is being watched.  Four seconds is enough for an address
 * appearing or a stick being plugged in; a start wants to be seen landing. */
const int kPollIdleMs = 4000;
const int kPollWatchMs = 1500;

/* A settle window used to live here: three seconds during which an inactive unit
 * was assumed to be a job systemd had not begun rather than one it had refused.
 * poll() asks systemd for the job now instead of timing it -- see bug EIGHT. */

/* The note strip under the title: one line's worth, and how many of them there can
 * be.  Two, because the sentences that say WHY a start refused are about that long
 * and the third line would come out of the list. */
const int kNoteRowH = 18;
const int kNoteMaxRows = 2;

} /* namespace */

/* ── the shell-outs ──────────────────────────────────────────────────────── */

/*
 * BLOCKING AND BOUNDED, the same bargain wifi.cpp strikes with wpa_cli.  These
 * are local unit-state queries that answer in milliseconds; a start is the one
 * that can take a second, which is what the wider default timeout is for.  A
 * timeout returns empty -- and `rc' is how a caller tells that apart from an
 * answer, because reading empty as "no" is right for is-active and was bug SEVEN
 * for is-enabled.  rc is -1 for a fork that never ran or never finished and the
 * child's own exit code otherwise, so `rc >= 0' is the question "did systemd
 * answer at all".
 *
 * AND THE WAITS ARE Shell'S, WHICH ON THIS PAGE IS NOT A DETAIL.  Starting a unit
 * is what makes systemd reset the console it logs to, and this function is the
 * only thing in the program that asks systemd to start one -- so the fifteen
 * seconds spent inside `systemctl start smbd' were both the likeliest moment for
 * the panel to be taken and the one stretch in which nothing here could notice.
 * That was "changing the sharing settings puts the console back on the glass".
 * shell.h has the mechanism; console.h has the reason there is one.
 */
QString SharingPage::systemctl(const QStringList &args, int timeoutMs, int *rc)
{
    if (rc)
        *rc = -1;               /* "it did not run", which is not "it said no" */

    if (systemctlPath().isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(systemctlPath(), args);
    /*
     * FOUR SECONDS TO GET IT RUNNING, AND IT WAS ONE.  This budget is not about
     * how long systemd takes to answer -- that is the timeout below -- it is about
     * how long the kernel takes to fork, map and relocate the systemctl binary,
     * which links libsystemd-shared and is several megabytes off an SD card on a
     * Cortex-A7.  On a cold page cache that is comfortably more than a second.
     *
     * And the budget is not spent purely on waiting: Shell's waits are sliced so
     * that the console can be held and the panel repainted between slices, and a
     * software repaint of this page costs real milliseconds out of the same
     * allowance.  A budget that ran out returned empty, and empty was drawn as a
     * switch in the off position -- see bug SEVEN in sharing.h.
     */
    if (!Shell::waitForStarted(p, 4000))
        return QString();
    if (!Shell::waitForFinished(p, timeoutMs)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }
    if (rc)
        *rc = p.exitCode();
    return QString::fromUtf8(p.readAll()).trimmed();
}

QString SharingPage::activeState(const QString &unit)
{
    return systemctl(QStringList() << QStringLiteral("is-active") << unit, 2500);
}

bool SharingPage::unitActive(const QString &unit)
{
    return activeState(unit) == QLatin1String("active");
}

/*
 * `activating' IS NOT `inactive', AND CONFLATING THEM WAS THE BUG.  unitActive()
 * answers the question the switch on the glass asks -- is the share up -- and for
 * that, everything that is not "active" is "no".  This answers the other question,
 * the one the old code never asked: is there still something happening.  A
 * Type=notify unit sits in `activating' from the moment its ExecCondition forks
 * until smbd sends READY=1, and on this board that stretch is measured in tens of
 * seconds, not the fifteen the start was given.
 */
bool SharingPage::stateIsTransient(const QString &state)
{
    /* Empty is one of them, and deliberately.  Empty means the query did not
     * answer -- a systemctl that timed out or did not run -- and "I could not
     * find out" is not grounds for declaring a start dead.  The grace still
     * bounds it, so a systemd that never answers ends the wait; it just does not
     * end it on the first missed tick. */
    return state.isEmpty()
        || state == QLatin1String("activating")
        || state == QLatin1String("deactivating")
        || state == QLatin1String("reloading");
}

QString SharingPage::unitProperty(const QString &unit, const QString &prop)
{
    return systemctl(QStringList() << QStringLiteral("show")
                                   << (QStringLiteral("--property=") + prop)
                                   << QStringLiteral("--value") << unit,
                     4000);
}

QMap<QString, QString> SharingPage::unitProperties(const QString &unit,
                                                   const QStringList &props)
{
    QMap<QString, QString> out;
    if (props.isEmpty())
        return out;

    QStringList args;
    args << QStringLiteral("show");
    for (const QString &p : props)
        args << (QStringLiteral("--property=") + p);
    args << unit;

    /* No --value here, deliberately: the answer has to carry the names, because
     * systemd omits a property it has nothing for and a positional read would then
     * quietly shift every value up by one. */
    const QString text = systemctl(args, 5000);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        out.insert(line.left(eq), line.mid(eq + 1).trimmed());
    }
    return out;
}

QString SharingPage::journalTail(const QString &unit, int lines)
{
    if (journalctlPath().isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    /* -o cat is the message and nothing else -- no timestamp, no hostname, no unit
     * name repeated on every line.  On a panel and in a file that a human is going
     * to read on a phone, the twenty characters of preamble are the difference
     * between a readable line and a wrapped one.  -b is this boot: journald has no
     * persistent store on this image, but saying so costs nothing and makes the
     * report honest about what it is showing. */
    p.start(journalctlPath(),
            QStringList() << QStringLiteral("--no-pager") << QStringLiteral("-b")
                          << QStringLiteral("-u") << unit
                          << QStringLiteral("-n") << QString::number(lines)
                          << QStringLiteral("-o") << QStringLiteral("cat"));
    if (!Shell::waitForStarted(p, 2000))
        return QString();
    if (!Shell::waitForFinished(p, 8000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }
    return QString::fromUtf8(p.readAll()).trimmed();
}

bool SharingPage::unitEnabled(const QString &unit, bool *answered)
{
    int rc = -1;
    /* `is-enabled' prints enabled, disabled, static, masked or alias.  Only the
     * first of those means the unit comes up by itself, and "static" -- which
     * Debian's smbd is not, but a future packaging change could make it -- means
     * there is no install section to enable, so the switch would lie. */
    const QString out =
        systemctl(QStringList() << QStringLiteral("is-enabled") << unit, 4000, &rc);

    /* It EXITS non-zero for a unit that is merely disabled, which is an answer and
     * not a failure -- so the test is whether the child ran to completion at all,
     * not what it exited with.  See bug SEVEN in sharing.h. */
    if (answered)
        *answered = (rc >= 0);
    return out == QLatin1String("enabled");
}

/*
 * ── ONE FORK PER TICK, AND IT SAYS WHETHER IT IS AN ANSWER ───────────────────
 *
 * A poll used to be two: is-active and is-enabled, each a cold exec of systemctl.
 * `show' gives both in one, plus the thing neither of them can say -- whether there
 * is a JOB in flight for the unit.
 *
 * Job= is the fix for bug EIGHT.  systemd prints it as `1234 start' while a job for
 * the unit is queued or running and leaves it empty (or omits it, on a systemd old
 * enough not to export it) when there is none, so non-empty means "this is not the
 * final answer, come back".  Nothing here depends on the property existing: a
 * systemd that does not export it simply falls back to the state alone, which is
 * where this page was before.
 */
SharingPage::UnitSnapshot SharingPage::readUnit(const QString &unit)
{
    UnitSnapshot snap;

    const QMap<QString, QString> props =
        unitProperties(unit, QStringList() << QStringLiteral("ActiveState")
                                           << QStringLiteral("UnitFileState")
                                           << QStringLiteral("Job"));

    /* An empty map is a fork that did not run.  systemd answering about a unit it
     * has never heard of still prints ActiveState=inactive, so the presence of that
     * one key is what separates "no answer" from "no such unit". */
    if (!props.contains(QStringLiteral("ActiveState")))
        return snap;

    snap.answered = true;
    snap.state = props.value(QStringLiteral("ActiveState"));
    snap.fileState = props.value(QStringLiteral("UnitFileState"));
    snap.job = !props.value(QStringLiteral("Job")).isEmpty();
    return snap;
}

/* Where systemd says the unit file actually is.  Asked rather than assumed: it is
 * /usr/lib/systemd/system on this rootfs and /lib/systemd/system on an older one,
 * and a symlink pointing at the wrong one is a unit that silently does not exist. */
QString SharingPage::unitPath(const QString &unit)
{
    return unitProperty(unit, QStringLiteral("FragmentPath"));
}

/*
 * ── SAYING WHY, WHICH THIS PAGE COULD NOT DO ─────────────────────────────────
 *
 * `systemctl start' is a poor witness to its own failure here.  It prints a line
 * when a unit FAILS, which is the case this board hardly ever hits; it prints
 * nothing at all when a unit is SKIPPED, which is the case this board hits every
 * time smb.conf is not readable, because both samba units carry
 *
 *     ExecCondition=/usr/share/samba/is-configured smb
 *
 * and a non-zero ExecCondition means skip, not fail -- exit status 0, no output,
 * unit inactive.  And it prints nothing when it was killed for taking too long,
 * because the killing was ours.
 *
 * So the reason is asked for afterwards, from the unit itself.  Result= is
 * systemd's own word for what happened and it survives the job: `exec-condition'
 * for the skip, `timeout' for a Type=notify daemon that never said READY, and
 * `exit-code' with ExecMainStatus= for one that died.  Where the answer points at
 * the configuration, samba is asked what it thinks of the file, because "samba
 * skipped itself" is only half a sentence without it.
 *
 * This function never returns an empty string.  That is the contract that matters:
 * the report this whole rewrite came from was a switch that moved back with
 * nothing written anywhere.
 */
QString SharingPage::startFailureReason(const QString &unit)
{
    const QString result = unitProperty(unit, QStringLiteral("Result"));
    const QString state = activeState(unit);

    if (result == QLatin1String("exec-condition")) {
        const QFileInfo cfg((QString::fromLatin1(kConfig)));
        if (!cfg.exists())
            return tr("samba skipped itself: there is no %1")
                       .arg(QString::fromLatin1(kConfig));
        if (cfg.size() == 0)
            return tr("samba skipped itself: %1 is empty -- the card may be full")
                       .arg(QString::fromLatin1(kConfig));
        const QString why = configComplaint();
        if (!why.isEmpty())
            return tr("samba will not read %1: %2")
                       .arg(QString::fromLatin1(kConfig), why);
        return tr("samba skipped itself over %1")
                   .arg(QString::fromLatin1(kConfig));
    }

    if (result == QLatin1String("timeout"))
        return tr("%1 did not finish starting").arg(unit);

    if (result == QLatin1String("exit-code")) {
        const QString why = configComplaint();
        if (!why.isEmpty())
            return why;
        const QString status = unitProperty(unit, QStringLiteral("ExecMainStatus"));
        return tr("%1 exited %2")
                   .arg(unit, status.isEmpty() ? QStringLiteral("?") : status);
    }

    if (result == QLatin1String("signal") || result == QLatin1String("core-dump"))
        return tr("%1 was killed as it started").arg(unit);

    if (result == QLatin1String("resources"))
        return tr("there was not enough memory to start %1").arg(unit);

    /* Result= says success and the unit is not running, which is the shape a start
     * that was never issued leaves behind -- no systemctl on the card, or a systemd
     * that did not answer.  Say what state it IS in rather than inventing a cause. */
    const QString why = configComplaint();
    if (!why.isEmpty())
        return why;
    if (!state.isEmpty())
        return tr("%1 is %2 and did not start").arg(unit, state);
    return tr("systemd did not answer about %1").arg(unit);
}

QString SharingPage::configComplaint()
{
    if (testparmPath().isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    /* -s is "do not wait for a keypress before dumping", which is the difference
     * between a check and a hang on a page with no terminal behind it. */
    p.start(testparmPath(),
            QStringList() << QStringLiteral("-s") << QString::fromLatin1(kConfig));
    if (!Shell::waitForStarted(p, 2000))
        return QString();
    if (!Shell::waitForFinished(p, 20000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }

    const QString out = QString::fromUtf8(p.readAll());
    /* testparm's complaints and its dump come out of the same pipe, so the dump is
     * walked for the words it puts on a bad file.  The lines that matter look like
     * "Error loading services." and "... failed", and the first of them is the one
     * worth putting on a 640x480 panel. */
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        /* The dump and the diagnostics share this pipe, and they are told apart by
         * where they start: testparm indents every parameter it echoes back with a
         * tab and writes its own complaints hard against the left margin.  Without
         * that test a share whose path had the word "error" in it would be reported
         * as the reason smbd did not start. */
        if (raw.startsWith(QLatin1Char('\t')) || raw.startsWith(QLatin1Char(' ')))
            continue;
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.contains(QLatin1String("Error"), Qt::CaseInsensitive)
            || line.contains(QLatin1String("failed"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("Unknown parameter")))
            return line;
    }
    /* Nothing recognisable, but it still refused: its last word is better than
     * this page's guess. */
    if (p.exitCode() != 0) {
        for (int i = lines.size() - 1; i >= 0; --i)
            if (!lines.at(i).trimmed().isEmpty())
                return lines.at(i).trimmed();
        return tr("testparm exited %1").arg(p.exitCode());
    }
    return QString();
}

/*
 * ── "AT BOOT" HAS TO BE VERIFIED, NOT ISSUED ─────────────────────────────────
 *
 * This used to be two systemctl calls and a re-read of is-enabled, and when the
 * enable did not take there was nothing to say so: the switch simply went back to
 * off, which reads from the outside as "the setting is not saved between boots".
 *
 * So the exit code is looked at now, the state is re-read, and when the two
 * disagree there is a fallback -- because `systemctl enable' has more ways to fail
 * on this image than on a desktop.  It writes into /etc, which is on the card and
 * has been remounted rw by the initramfs but is not guaranteed to have stayed that
 * way; it wants to talk to PID 1 over D-Bus, and finishing_touches.sh disables
 * polkit; and it refuses outright for a unit systemd calls `static' or `alias',
 * which is a packaging decision the samba maintainers can make without telling us.
 *
 * The fallback is the .wants symlink, written by hand.  That is not a trick: it is
 * exactly what `systemctl enable' produces, .wants directories are merged across
 * /etc, /run and /usr/lib, and build-in-vm.sh already boots mixdash itself this way
 * for the same reason -- see the note above the multi-user.target.wants link there.
 * is-enabled reads the same symlink back, so the switch on the glass and the state
 * on the card cannot drift apart afterwards.
 *
 * Returns an empty string when the unit really is in the asked-for state, and the
 * reason -- systemd's own first line, where there is one -- when it is not.
 */
QString SharingPage::setEnabled(const QString &unit, bool on)
{
    int rc = -1;
    const QString out = systemctl(QStringList()
                                      << (on ? QStringLiteral("enable")
                                             : QStringLiteral("disable"))
                                      << unit,
                                  8000, &rc);

    /* The verification has to be able to say "I could not check", because the two
     * failures below both write to the card and neither is a thing to do on the
     * strength of a fork that did not run.  See bug SEVEN. */
    bool answered = false;
    if (unitEnabled(unit, &answered) == on && answered)
        return QString();

    const QString wants =
        QStringLiteral("/etc/systemd/system/multi-user.target.wants");
    const QString link = wants + QLatin1Char('/') + unit + QStringLiteral(".service");

    if (on) {
        const QString frag = unitPath(unit);

        if (frag.isEmpty() || !QFileInfo(frag).exists())
            return tr("there is no %1.service on this card").arg(unit);
        QDir().mkpath(wants);
        /* Removed first: QFile::link will not overwrite, and a stale link is the
         * likeliest thing to be standing here. */
        QFile::remove(link);
        if (!QFile::link(frag, link))
            return tr("cannot write %1").arg(link);
    } else if (QFileInfo(link).isSymLink() && !QFile::remove(link)) {
        return tr("cannot remove %1").arg(link);
    }

    /* So that this boot's systemd agrees with the card, and so that the is-enabled
     * below is answered from the same picture the next boot will build. */
    systemctl(QStringList() << QStringLiteral("daemon-reload"), 8000);

    answered = false;
    if (unitEnabled(unit, &answered) == on)
        return QString();

    /*
     * A CHECK THAT DID NOT RUN IS NOT A FAILURE.  The symlink above is what
     * is-enabled reads, and it is on the card whether or not this fork managed to
     * ask about it; reporting "systemctl enable smbd exited -1" here would put a
     * systemctl problem on the glass under a setting that has in fact been made.
     * Silence, and the next poll reads the card and draws the switch from it.
     */
    if (!answered)
        return QString();

    /* Still not.  systemd's own words if it left any, and the exit code if it did
     * not -- an empty message with a switch that will not move is the thing this
     * whole function exists to stop happening again. */
    if (!out.isEmpty())
        return out.section('\n', 0, 0);
    return tr("systemctl %1 %2 exited %3")
               .arg(on ? QStringLiteral("enable") : QStringLiteral("disable"),
                    unit, QString::number(rc));
}

bool SharingPage::sambaInstalled()
{
    static const QStringList paths = QStringList()
        << QStringLiteral("/usr/sbin/smbd") << QStringLiteral("/usr/bin/smbd");
    static const bool found = !firstExecutable(paths).isEmpty();
    return found;
}

QString SharingPage::hostName()
{
    char buf[256] = { 0 };
    if (::gethostname(buf, sizeof(buf) - 1) != 0)
        return QStringLiteral("mixos");
    QString name = QString::fromLatin1(buf).section('.', 0, 0);
    return name.isEmpty() ? QStringLiteral("mixos") : name;
}

/*
 * getifaddrs and not `ip addr', and not QNetworkInterface either.  The tool would
 * be a parse of a format that changes; QtNetwork would be a new DT_NEEDED on
 * libQt6Network for one loop, and the Qt payload stager in build-in-vm.sh stages
 * the libraries this binary links -- so adding the module here quietly adds two
 * megabytes to the card.  wifi.cpp already reaches for the socket layer directly
 * (SIOCGIFADDR) for the same reason; this is the every-interface version of it.
 */
QStringList SharingPage::addresses()
{
    QStringList out;
    struct ifaddrs *list = nullptr;

    if (::getifaddrs(&list) != 0)
        return out;

    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        char buf[INET_ADDRSTRLEN] = { 0 };
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            out << QString::fromLatin1(buf);
    }

    ::freeifaddrs(list);
    return out;
}

/*
 * What is under /media right now.  Directories only, and the directory name is
 * the volume label because that is what setup_automount names them -- see the
 * README block in build-in-vm.sh: "/media/<its label>, or /media/sda1 when it has
 * no label".  An empty list is the ordinary state on this board today and the
 * page says so in those words rather than leaving a blank row.
 */
QStringList SharingPage::mountedVolumes()
{
    const QDir dir(QString::fromLatin1(kDisks));
    if (!dir.exists())
        return QStringList();
    return dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

/* ── the password ────────────────────────────────────────────────────────── */

/*
 * TWELVE CHARACTERS FROM /dev/urandom, out of an alphabet with no I, l, 1, O or
 * 0 in it.  The alphabet is the whole reason this is not just a base64 of some
 * random bytes: this password is going to be read off a 640x480 panel and typed
 * into a PC by hand, and a capital I that turns out to have been a lower-case l
 * is a share that "does not work" for reasons nobody can see.
 *
 * 58 symbols to the twelfth is a shade over 70 bits, which is far past what a
 * share on a home network needs and costs nothing, because nobody memorises it.
 */
QString SharingPage::generatePassword() const
{
    static const char alphabet[] =
        "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const int span = int(sizeof(alphabet)) - 1;

    QFile urandom(QStringLiteral("/dev/urandom"));
    if (!urandom.open(QIODevice::ReadOnly))
        return QString();

    QString out;
    while (out.size() < 12) {
        char c = 0;
        if (urandom.read(&c, 1) != 1)
            return QString();
        /*
         * REJECTION, NOT MODULO.  256 is not a multiple of 58, so `byte % 58'
         * would make the first 24 letters of the alphabet slightly likelier than
         * the rest.  It would not matter here -- it is a share password on a
         * handheld -- but a biased sampler that gets copied into somewhere it
         * does matter is how bias travels, and the fix is one comparison.
         */
        const unsigned char byte = (unsigned char)c;
        if (byte >= (256 / span) * span)
            continue;
        out.append(QLatin1Char(alphabet[byte % span]));
    }
    return out;
}

QString SharingPage::storedPassword() const
{
    QFile f(QString::fromLatin1(kSecret));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

/*
 * Hand the password to samba's own database and keep a copy the page can show.
 *
 * The order is deliberate: smbpasswd first, the file second.  If smbpasswd fails
 * -- no samba, no such unix user, a corrupt tdb -- then nothing is written, and
 * the next visit to this page reads back the password that is still genuinely in
 * force rather than one the page believes in and the server has never heard of.
 */
QString SharingPage::applyPassword(const QString &password)
{
    if (password.isEmpty())
        return tr("could not read /dev/urandom");
    if (smbpasswdPath().isEmpty())
        return tr("smbpasswd is not installed");

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    /* -a adds the account or updates it, -s reads the password twice from stdin
     * instead of opening the terminal this process does not have. */
    p.start(smbpasswdPath(),
            QStringList() << QStringLiteral("-a") << QStringLiteral("-s")
                          << QString::fromLatin1(kUser));
    if (!Shell::waitForStarted(p, 2000))
        return tr("smbpasswd would not start");

    const QByteArray twice = (password + "\n" + password + "\n").toUtf8();
    p.write(twice);
    p.closeWriteChannel();
    if (!Shell::waitForFinished(p, 8000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return tr("smbpasswd did not finish");
    }
    if (p.exitCode() != 0)
        return tr("smbpasswd failed: %1")
                   .arg(QString::fromUtf8(p.readAll()).trimmed().section('\n', 0, 0));

    QDir().mkpath(QString::fromLatin1(kSecretDir));
    QFile f(QString::fromLatin1(kSecret));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return tr("cannot write %1").arg(QString::fromLatin1(kSecret));
    f.write(password.toUtf8());
    f.write("\n");
    f.close();
    /*
     * CHECKED AFTER THE CLOSE, not after the writes.  A QFile write of thirteen
     * bytes lands in the buffer and the buffer is not handed to the kernel until
     * the close, so a full card fails HERE and nowhere earlier.  The old code
     * returned success unconditionally at this point, and the account samba had
     * just been given would then be one this page could never show again -- the
     * share works and the only copy of its password is gone.
     */
    if (f.error() != QFile::NoError || f.size() <= 0) {
        return tr("the password reached samba but could not be saved to %1 -- "
                  "is the card full?").arg(QString::fromLatin1(kSecret));
    }
    /* Owner only.  It is the credential to a writable export of somebody's home
     * directory and this image has a Terminal page on it. */
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return QString();
}

QString SharingPage::newPassword()
{
    const QString pw = generatePassword();
    const QString err = applyPassword(pw);
    if (!err.isEmpty())
        return err;
    m_reveal = true;
    return QString();
}

/* ── the configuration ───────────────────────────────────────────────────── */

/*
 * WRITTEN BY THIS PAGE AND NOT SHIPPED IN THE IMAGE, and that is the one
 * structural decision in this file worth arguing about.
 *
 * The image's smb.conf is Debian's, which shares printers.  Replacing it during
 * the build would mean the rootfs pipeline -- shared with every other board this
 * tree has ever built -- carrying a J36 share layout, and it would mean a device
 * that has been serving files since first boot with no one having asked it to.
 * Writing it here instead makes the share exactly as old as the first time
 * somebody switched it on, which is what a Sharing page in any other OS does.
 *
 * The cost is that a hand-edited smb.conf is overwritten by "Rewrite
 * configuration" -- so that row is a row and not something the page does silently
 * on every visit.  It is written once, when sharing is first turned on, and after
 * that only when asked.
 */
QString SharingPage::writeConfig()
{
    QDir().mkpath(QFileInfo(QString::fromLatin1(kConfig)).absolutePath());
    /* Best effort, and not an error if it fails: an existing /media is the
     * normal case, and a rootfs that will not take the mkdir is the case
     * setup_automount already fell back to /run/media for. */
    QDir().mkpath(QString::fromLatin1(kDisks));

    QFile f(QString::fromLatin1(kConfig));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return tr("cannot write %1").arg(QString::fromLatin1(kConfig));

    QTextStream s(&f);
    s << QString::fromLatin1(kMarker) << "\n"
      << "# It replaces the two guest-readable shares finishing_touches.sh appends\n"
      << "# to Debian's own file.  Edits are kept until this page is asked to\n"
      << "# rewrite, or until sharing is switched on with the file no longer ours.\n"
      << "\n"
      << "[global]\n"
      << "   workgroup = WORKGROUP\n"
      << "   server string = MixOS on " << hostName() << "\n"
      << "   netbios name = " << hostName() << "\n"
      /* SMB1 is off in every client worth supporting and has been for years;
       * naming the floor here means an old client fails to negotiate instead of
       * negotiating something that has not been safe since 2017. */
      << "   server min protocol = SMB2\n"
      << "   server role = standalone server\n"
      /* No printers, and three lines to say it because samba asks the printing
       * system three different questions.  Without them smbd spawns a printcap
       * probe at startup on a device that has never had a printer. */
      << "   load printers = no\n"
      << "   printing = bsd\n"
      << "   printcap name = /dev/null\n"
      << "   disable spoolss = yes\n"
      /* Port 445 only.  139 is NetBIOS-over-TCP, which nmbd's name service does
       * not need and which is the older of the two transports. */
      << "   smb ports = 445\n"
      /* A Cortex-A7 at this clock is the bottleneck long before the radio is, so
       * this is where the throughput comes from: sendfile keeps a read off the
       * copy path entirely, and the socket options are samba's own documented
       * defaults for a link with real latency on it. */
      << "   use sendfile = yes\n"
      << "   socket options = TCP_NODELAY IPTOS_LOWDELAY\n"
      /* Names off a Windows PC arrive in any case and the volumes under /media
       * come from vfat and exfat, where case is not preserved the way ext4
       * preserves it. */
      << "   case sensitive = no\n"
      << "   map to guest = never\n"
      << "\n"
      /* The DATA partition, whole.  Named Home because that is what it is to
       * everything on the device, and because \\mixos\Home is what somebody
       * types when they want the handheld's files. */
      << "[Home]\n"
      << "   comment = MixOS home -- the DATA partition of the card\n"
      << "   path = " << QString::fromLatin1(kHome) << "\n"
      << "   valid users = " << QString::fromLatin1(kUser) << "\n"
      << "   read only = no\n"
      << "   browseable = yes\n"
      /* Files arriving from a PC have to be usable by the dashboard, which runs
       * as root, and by everything else that runs as virtua.  0644/0755 is what
       * the rest of this home directory already looks like. */
      << "   create mask = 0644\n"
      << "   directory mask = 0755\n"
      << "\n"
      << "[Disks]\n"
      << "   comment = Removable disks, as they are plugged in\n"
      << "   path = " << QString::fromLatin1(kDisks) << "\n"
      << "   valid users = " << QString::fromLatin1(kUser) << "\n"
      << "   read only = no\n"
      << "   browseable = yes\n"
      << "   create mask = 0644\n"
      << "   directory mask = 0755\n";

    /*
     * ── THE WRITE IS LOOKED AT NOW, AND IT WAS NOT ───────────────────────────
     *
     * This function used to end `s.flush(); f.close(); return QString();' -- three
     * statements, no question asked, success reported whatever had happened to the
     * bytes.  QTextStream buffers, so every one of those `<<' above succeeds even
     * on a card with nothing left on it; the failure arrives at the flush and at
     * the close, which is precisely where nobody was looking.
     *
     * What came out the other side was a TRUNCATED /etc/samba/smb.conf and a page
     * saying the configuration had been written.  And a truncated smb.conf is the
     * worst of the three states this file can be in: samba's is-configured returns
     * non-zero over it, so systemd SKIPS smbd rather than failing it, so
     * `systemctl start smbd' exits 0 and prints nothing and the switch goes back to
     * off with no reason recorded anywhere on the device.
     *
     * The half-written file is deliberately LEFT WHERE IT IS rather than rolled
     * back.  What it would be rolled back to is Debian's file plus
     * finishing_touches.sh's `guest ok = yes' share of the whole DATA partition,
     * and a card that cannot write its own configuration is not a card to hand a
     * guest-writable export to.  A skipped smbd shares nothing, which is the right
     * way for this to fail.
     */
    s.flush();
    const bool streamOk = (s.status() == QTextStream::Ok);
    f.close();
    if (!streamOk || f.error() != QFile::NoError || f.size() <= 0) {
        return tr("could not finish writing %1 -- is the card full?")
                   .arg(QString::fromLatin1(kConfig));
    }
    return QString();
}

bool SharingPage::configIsOurs()
{
    QFile f(QString::fromLatin1(kConfig));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    /* The marker is the first line, so one readLine is the whole test.  Reading
     * the file to look for it anywhere would find it in a comment somebody
     * pasted, and this question deserves a stricter answer than that. */
    const QByteArray head = f.readLine(256).trimmed();
    if (head != QByteArray(kMarker))
        return false;

    /*
     * AND THE FILE HAS TO REACH ITS END, which the marker alone does not prove.
     * A write that ran out of card gets the first line down and stops somewhere in
     * the middle, and a file like that answered "yes, ours" for ever after --
     * ensureConfigured() would then skip the rewrite that is the one thing which
     * could have repaired it, on every visit, permanently.  Both share headers are
     * looked for because they are the last things written; the rest of the file is
     * small enough that reading it to find them costs nothing.
     */
    const QByteArray rest = f.readAll();
    return rest.contains("\n[Home]\n") && rest.contains("\n[Disks]\n");
}

/*
 * The gate in front of everything that makes smbd run.
 *
 * A card that has never had this page touched carries finishing_touches.sh's
 * [home] -- `guest ok = yes' and writable -- so starting smbd against that file
 * would put the whole DATA partition on the network with no password at all.
 * That is the exact failure this page exists not to have, and the only reliable
 * place to prevent it is here, before the start.
 *
 * There used to be an [opt] beside it, on the same terms, exporting the system's
 * own programs guest-writable.  It is gone from finishing_touches.sh, and this
 * gate would have covered it anyway -- but a share nobody can name a use for is
 * not something to leave standing behind a gate and hope the gate holds.
 */
QString SharingPage::ensureConfigured()
{
    if (!configIsOurs()) {
        const QString err = writeConfig();
        if (!err.isEmpty())
            return err;
    }
    if (storedPassword().isEmpty())
        return newPassword();
    return QString();
}

/* ── start and stop ──────────────────────────────────────────────────────── */

void SharingPage::start()
{
    if (!sambaInstalled()) {
        noteFailure(tr("samba is not installed on this card"));
        return;
    }
    if (systemctlPath().isEmpty()) {
        noteFailure(tr("there is no systemctl on this card, so nothing can start smbd"));
        return;
    }

    /* Config first: smbd reads it at startup, so writing it afterwards would
     * serve the guest-readable shares for the life of this boot. */
    const QString err = ensureConfigured();
    if (!err.isEmpty()) {
        noteFailure(err);
        return;
    }

    /*
     * --no-block, WHICH IS THE WHOLE OF BUG SIX.
     *
     * This used to wait fifteen seconds for `systemctl start smbd' and then read the
     * unit's state once.  Both halves of that were wrong.  The wait was fifteen
     * seconds in which the dashboard did not repaint and did not read the pad -- and
     * on a Type=notify unit that runs testparm three times off a cold card, it was
     * reliably too short anyway, so the answer it bought was `activating', which is
     * not an answer.  The watch below is what actually settles the question, and
     * once there is a watch the wait is pure cost.
     *
     * So the job is queued and the page carries on.  rc still means something: a
     * non-zero exit here is systemd REFUSING THE JOB -- no such unit, no D-Bus, a
     * masked unit -- which is a real answer and comes with a message.  rc == 0 is
     * only "queued", which is why it leads to the watch and not to a verdict.
     */
    int rc = -1;
    const QString out = systemctl(QStringList() << QStringLiteral("--no-block")
                                                << QStringLiteral("start")
                                                << QString::fromLatin1(kSmbd),
                                  8000, &rc);

    /* One query, three answers wanted out of it: up, still moving, or settled and
     * not up.  Asked as the state rather than as unitActive() because the second
     * of those is the one the old code could not see. */
    const QString state = activeState(QString::fromLatin1(kSmbd));
    m_active = (state == QLatin1String("active"));
    if (m_active) {
        /* nmbd only now, and only here and in endWatch(true).  smbd.service carries
         * `After=nmb.service', so queueing nmbd while an smbd job is in flight makes
         * smbd wait for the whole of nmbd's startup -- the one wait on this page
         * anybody is watching, lengthened by a daemon whose result this page does
         * not even consult.  Once smbd is up there is no job left for the ordering
         * to apply to.  --no-block for the same reason: see the note on kNmbd. */
        systemctl(QStringList() << QStringLiteral("--no-block")
                                << QStringLiteral("start")
                                << QString::fromLatin1(kNmbd),
                  4000);
        const QStringList ips = addresses();
        m_note = ips.isEmpty()
                     ? tr("sharing is on, but this device has no network address yet")
                     : tr("sharing is on at %1").arg(ips.first());
        m_reveal = true;
        /* The standing complaint goes when the thing it complained about works.
         * The file on the card stays -- it is dated, and it is the only history
         * this device keeps of a start that did not. */
        m_lastFailure.clear();
        return;
    }

    /*
     * QUEUED IS NOT REFUSED, and telling the two apart is the whole point.
     *
     * rc == 0 means PID 1 took the job and nothing about what it will do with it;
     * rc < 0 means systemctl itself never finished, and killing the client cancels
     * nothing, because the job belongs to PID 1 either way.  In both cases smbd is
     * coming up behind this function and the page has to watch rather than guess.
     */
    if (rc <= 0 || stateIsTransient(state)) {
        beginWatch();
        return;
    }

    /* systemd said no to the job itself, which it does with a message.  Its own
     * line where there is one, and otherwise the reason dug out of the unit --
     * which is never empty. */
    noteFailure(out.isEmpty() ? startFailureReason(QString::fromLatin1(kSmbd))
                              : out.section('\n', 0, 0));
}

void SharingPage::stop()
{
    /* Any watch in flight is over: whatever the start was going to do, this is the
     * newer instruction, and a watch that outlived it would report "sharing is on"
     * a second after the user switched it off. */
    m_starting = false;
    m_startGraceMs = 0;
    m_timer->setInterval(kPollIdleMs);

    systemctl(QStringList() << QStringLiteral("stop") << QString::fromLatin1(kSmbd),
              10000);
    systemctl(QStringList() << QStringLiteral("--no-block")
                            << QStringLiteral("stop") << QString::fromLatin1(kNmbd),
              4000);
    m_active = unitActive(QString::fromLatin1(kSmbd));
    m_reveal = false;
    m_note = m_active ? tr("smbd is still running") : tr("sharing is off");
}

/*
 * ── WATCHING A START THAT HAS NOT LANDED ─────────────────────────────────────
 *
 * The alternative was to keep blocking until systemd answered, and systemd's own
 * patience for a Type=notify unit is ninety seconds.  Ninety seconds inside
 * Shell::waitForFinished is ninety seconds of a panel that does not repaint, does
 * not read the pad and cannot be backed out of -- which is not a fix, it is the
 * same bug wearing a longer coat.
 *
 * So the wait moves into the poll timer this page already had.  The page keeps
 * running, the note says what is actually happening, and the tick that finds the
 * unit settled writes the real answer.  The cost is one `is-active' fork every
 * second and a half for as long as it takes, which is a fork this page was already
 * doing every four seconds anyway.
 */
void SharingPage::beginWatch()
{
    m_starting = true;
    m_startGraceMs = kStartGraceMs;
    m_timer->setInterval(kPollWatchMs);
    m_note = tr("smbd is still starting -- this takes a moment on the first try");
}

void SharingPage::endWatch(bool ok)
{
    m_starting = false;
    m_startGraceMs = 0;
    m_timer->setInterval(kPollIdleMs);

    if (ok) {
        /* The other half of the nmbd move -- see start().  smbd is up, so its job
         * is done and `After=nmb.service' has nothing left to hold back. */
        systemctl(QStringList() << QStringLiteral("--no-block")
                                << QStringLiteral("start") << QString::fromLatin1(kNmbd),
                  4000);
        const QStringList ips = addresses();
        m_note = ips.isEmpty()
                     ? tr("sharing is on, but this device has no network address yet")
                     : tr("sharing is on at %1").arg(ips.first());
        /* Shown on the way up and only on the way up.  It is the one moment the
         * password is certain to be wanted, and onEnter() masks it again. */
        m_reveal = true;
        m_lastFailure.clear();
        emit toastRequested(tr("Sharing is on"), 2500);
        return;
    }
    noteFailure(startFailureReason(QString::fromLatin1(kSmbd)));
}

/* ── the diagnosis ───────────────────────────────────────────────────────── */

namespace {

/* The head of a text file, bounded twice -- by lines and by bytes.  Both bounds
 * matter: this goes onto a small vfat partition, and smb.conf is only smb.conf on
 * a card where nothing has gone wrong, which is not the card this runs on. */
QString fileHead(const QString &path, int maxLines, int maxBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("  (cannot read it)");

    QStringList lines;
    int bytes = 0;
    while (lines.size() < maxLines && bytes < maxBytes && !f.atEnd()) {
        const QByteArray raw = f.readLine(512);
        bytes += raw.size();
        lines << QString::fromUtf8(raw).remove(QLatin1Char('\n')).remove(QLatin1Char('\r'));
    }
    if (!f.atEnd())
        lines << QStringLiteral("  ... (truncated)");
    return lines.join(QLatin1Char('\n'));
}

/* Seconds since boot.  Written beside the wall clock because this board has no
 * RTC that survives a battery pull: the timestamp above it can be 1970 and the
 * uptime still tells you which start went with which report. */
QString uptimeSeconds()
{
    QFile f(QStringLiteral("/proc/uptime"));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(f.readLine(64)).section(QLatin1Char(' '), 0, 0);
}

} /* namespace */

/*
 * EVERYTHING THIS PAGE CAN FIND OUT, IN ONE FILE.
 *
 * Not a summary: the whole of it, because the reader is somebody standing at a PC
 * with the card in it and no way to ask a follow-up question.  systemd's own
 * account of both units, smbd's journal for this boot, samba's opinion of the
 * configuration, and the configuration itself -- the four things that between them
 * name every cause this page has ever had to explain.
 *
 * THE PASSWORD IS NOT IN IT, deliberately, and that is not an oversight to be
 * "fixed" later.  This file lands on the vfat partition, which is the one part of
 * the card that any machine can read without a password of its own; a share
 * credential written there would be a share credential written in the clear to
 * whoever picks the device up.  Whether one EXISTS is in the report, because that
 * is the part that can be wrong.
 */
QString SharingPage::collectReport(const QString &reason) const
{
    static const QStringList props = QStringList()
        << QStringLiteral("LoadState") << QStringLiteral("ActiveState")
        << QStringLiteral("SubState") << QStringLiteral("UnitFileState")
        << QStringLiteral("Result") << QStringLiteral("ConditionResult")
        << QStringLiteral("AssertResult") << QStringLiteral("ExecMainStatus")
        << QStringLiteral("ExecMainCode") << QStringLiteral("StatusText")
        << QStringLiteral("NRestarts") << QStringLiteral("FragmentPath");

    QStringList o;
    o << QStringLiteral("================================================================");
    o << QStringLiteral("MixOS sharing diagnosis");
    o << QStringLiteral("  written    %1")
             .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    const QString up = uptimeSeconds();
    if (!up.isEmpty())
        o << QStringLiteral("  uptime     %1 s  (trust this over the clock above -- "
                            "there is no RTC)").arg(up);
    o << QStringLiteral("  reason     %1").arg(reason);
    o << QString();

    o << QStringLiteral("-- this device --");
    o << QStringLiteral("  host       %1").arg(hostName());
    const QStringList ips = addresses();
    o << QStringLiteral("  addresses  %1")
             .arg(ips.isEmpty() ? QStringLiteral("(none -- not on a network)")
                                : ips.join(QStringLiteral(", ")));
    const QStringList vols = mountedVolumes();
    o << QStringLiteral("  volumes    %1")
             .arg(vols.isEmpty() ? QStringLiteral("(nothing mounted under /media)")
                                 : vols.join(QStringLiteral(", ")));
    o << QStringLiteral("  samba      %1")
             .arg(sambaInstalled() ? QStringLiteral("installed")
                                   : QStringLiteral("NOT INSTALLED"));
    o << QStringLiteral("  systemctl  %1")
             .arg(systemctlPath().isEmpty() ? QStringLiteral("(not on this card)")
                                            : systemctlPath());
    o << QStringLiteral("  testparm   %1")
             .arg(testparmPath().isEmpty() ? QStringLiteral("(not on this card)")
                                           : testparmPath());
    o << QStringLiteral("  journalctl %1")
             .arg(journalctlPath().isEmpty() ? QStringLiteral("(not on this card)")
                                             : journalctlPath());
    o << QString();

    o << QStringLiteral("-- configuration --");
    const QFileInfo cfg((QString::fromLatin1(kConfig)));
    o << QStringLiteral("  %1  %2, %3 bytes")
             .arg(QString::fromLatin1(kConfig),
                  cfg.exists() ? QStringLiteral("present") : QStringLiteral("MISSING"),
                  QString::number(cfg.size()));
    o << QStringLiteral("  written by this page   %1")
             .arg(configIsOurs() ? QStringLiteral("yes")
                                 : QStringLiteral("NO -- it is the image's own file"));
    /* Whether, not what.  See the block above this function. */
    o << QStringLiteral("  share password stored  %1")
             .arg(storedPassword().isEmpty() ? QStringLiteral("NO")
                                             : QStringLiteral("yes (not printed here)"));
    const QString complaint = configComplaint();
    o << QStringLiteral("  testparm says          %1")
             .arg(complaint.isEmpty() ? QStringLiteral("nothing -- it accepts the file")
                                      : complaint);
    o << QString();

    const char *units[] = { kSmbd, kNmbd };
    for (const char *name : units) {
        const QString unit = QString::fromLatin1(name);
        o << QStringLiteral("-- %1 --").arg(unit);
        const QMap<QString, QString> p = unitProperties(unit, props);
        if (p.isEmpty()) {
            o << QStringLiteral("  systemd did not answer about it");
        } else {
            /* In the order asked for rather than the map's, because Result= beside
             * ConditionResult= is the pair that names the cause and a report sorted
             * alphabetically puts them at opposite ends. */
            for (const QString &key : props) {
                if (p.contains(key))
                    o << QStringLiteral("  %1=%2").arg(key, p.value(key));
            }
        }
        const QString tail = journalTail(unit, 40);
        o << QStringLiteral("  -- journal, this boot --");
        o << (tail.isEmpty() ? QStringLiteral("  (nothing there)") : tail);
        o << QString();
    }

    o << QStringLiteral("-- %1 --").arg(QString::fromLatin1(kConfig));
    o << fileHead(QString::fromLatin1(kConfig), 120, 8192);
    o << QString();

    return o.join(QLatin1Char('\n'));
}

QString SharingPage::writeReport(const QString &reason)
{
    /* CRLF, and only here.  The report exists to be opened on the PC the operator
     * is standing at, and that is the one place in this tree where the DOS line
     * ending is the correct one rather than an accident. */
    QString text = collectReport(reason);
    text.replace(QLatin1String("\n"), QLatin1String("\r\n"));

    const QStringList dirs = reportDirs();
    for (const QString &dir : dirs) {
        if (!QFileInfo(dir).isDir())
            continue;

        const QString path =
            dir + QLatin1Char('/') + QString::fromLatin1(kReportName);

        /*
         * APPENDED, AND CAPPED.  Appended because the second failure is usually the
         * one that explains the first, and a file that overwrote itself would lose
         * the comparison the moment somebody tried again.  Capped because /boot is a
         * small vfat partition and this page has a button on it: past the cap the
         * file starts over rather than filling the partition the device boots from.
         */
        const bool fresh = QFileInfo(path).size() >= 96 * 1024;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text
                    | (fresh ? QIODevice::Truncate : QIODevice::Append)))
            continue;

        QTextStream s(&f);
        s << text << "\r\n";
        s.flush();
        const bool streamOk = (s.status() == QTextStream::Ok);
        f.close();
        /* Checked for the same reason writeConfig()'s write is -- a card with no
         * room left is one of the things this report is written to diagnose, and a
         * report that silently did not land would be the worst of both. */
        if (!streamOk || f.error() != QFile::NoError)
            continue;
        return path;
    }
    return QString();
}

void SharingPage::noteFailure(const QString &reason)
{
    m_lastFailure = reason;

    const QString path = writeReport(reason);
    if (!path.isEmpty())
        m_reportPath = path;

    /* The reason first and the path second: the strip takes two lines now, but if
     * only one of them survives the elide it has to be the half that says why. */
    m_note = m_reportPath.isEmpty()
                 ? reason
                 : tr("%1  --  written to %2").arg(reason, m_reportPath);
}

/* ── the page ────────────────────────────────────────────────────────────── */

SharingPage::SharingPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    connect(m_list, &ListPane::activated, this, &SharingPage::onActivated);

    /*
     * Slow, and it can afford to be.  Nothing on this page changes on its own
     * except the address (DHCP finishing) and the volume list (a disk arriving),
     * and both of those are events measured in seconds.  Every tick is three
     * systemctl forks, which is not a thing to do at a UI frame rate.
     */
    m_timer = new QTimer(this);
    m_timer->setInterval(4000);
    connect(m_timer, &QTimer::timeout, this, &SharingPage::poll);
}

/*
 * ── THE NOTE STRIP, AND WHY IT IS NOT A FIXED HEIGHT ─────────────────────────
 *
 * It was one line of eighteen pixels, unconditionally, and drawText CLIPS: so
 * "samba will not read /etc/samba/smb.conf: Unknown parameter encountered ..." was
 * painted as far as the right edge of the card and the rest was thrown away.  The
 * half that got thrown away is the half that names the cause, every time, because
 * the cause is always at the end of the sentence.  That is bug FOUR, and it is why
 * three fixed bugs still produced the same report: the page HAD the reason and had
 * nowhere to put it.
 *
 * Two lines when two are needed and one otherwise, with the list moving down to
 * make room.  Not two permanently -- on the ordinary "sharing is off" the second
 * line would be blank and a row of the list would have been given away for it.
 */
QString SharingPage::noteLine() const
{
    if (!m_note.isEmpty())
        return m_note;
    if (!sambaInstalled())
        return tr("Samba is not on this card");
    if (m_active)
        return tr("Home and any plugged-in disk are on the network");
    if (m_starting)
        return tr("Starting -- smbd has not answered yet");
    /* Survives onEnter(), which m_note deliberately does not.  A failure the
     * operator walked away from and came back to is the case this page was
     * reported for, and saying nothing about it is how it stayed unexplained. */
    if (!m_lastFailure.isEmpty())
        return tr("Last try: %1").arg(m_lastFailure);
    return tr("Switch it on to reach this device's files from a PC");
}

int SharingPage::noteLines() const
{
    const QFontMetrics fm(Theme::font(12));
    const int box = qMax(1, width() - 2 * Theme::Margin - 26);
    return fm.horizontalAdvance(noteLine()) > box ? kNoteMaxRows : 1;
}

void SharingPage::layOut()
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    /* 36 + 20 down from the top of the card and 26 up from the bottom is where this
     * list has always sat; the extra is the second note line pushing it down. */
    const int extra = (noteLines() - 1) * kNoteRowH;
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20 + extra, card.width() - 12,
                        card.height() - 36 - 26 - extra);
}

void SharingPage::resizeEvent(QResizeEvent *event)
{
    layOut();
    QWidget::resizeEvent(event);
}

void SharingPage::onEnter()
{
    m_note.clear();
    /* Masked again on every entry.  The page is on a handheld somebody hands to
     * somebody else, and a password that stays on screen from the last visit is
     * one that gets shoulder-surfed by accident. */
    m_reveal = false;
    /* Not resumed across a visit.  The unit carries on starting whether this page
     * is on the glass or not, so coming back and reading is-active tells the truth
     * without any state having to survive the trip. */
    m_starting = false;
    m_startGraceMs = 0;
    m_timer->setInterval(kPollIdleMs);
    poll();
    m_timer->start();
}

void SharingPage::onLeave()
{
    m_timer->stop();
    m_starting = false;
    m_startGraceMs = 0;
}

void SharingPage::poll()
{
    const UnitSnapshot smbd = readUnit(QString::fromLatin1(kSmbd));

    /*
     * ASSIGNED ONLY WHEN THERE IS SOMETHING TO ASSIGN, which is bug SEVEN.  A tick
     * whose fork did not run has learnt nothing about either switch, and the old
     * code drew both of them from it anyway -- so a slow systemctl put "Share over
     * the network" and "Start at boot" in the off position for four seconds and the
     * next tick put them back.  That is the knob that resets.
     *
     * The watch below still treats a missing answer as "keep waiting", which is a
     * different question and is answered by the grace rather than by the flags.
     */
    if (smbd.answered) {
        m_active = (smbd.state == QLatin1String("active"));
        m_enabled = (smbd.fileState == QLatin1String("enabled"));
    }

    if (m_starting) {
        m_startGraceMs -= m_timer->interval();

        if (smbd.answered && smbd.state == QLatin1String("active")) {
            endWatch(true);
        } else if (smbd.answered && !smbd.job && !stateIsTransient(smbd.state)) {
            /*
             * It has stopped moving, there is no job left for it, and it is not up.
             * Waiting out the rest of the grace would only make the page slower to
             * say so.
             *
             * `!smbd.job' IS THE WHOLE GUARD, and it used to be a three-second
             * stopwatch.  A start job PID 1 has accepted but not begun leaves the
             * unit at `inactive' -- settled, and indistinguishable from a refusal --
             * and smbd.service is ordered after network-online.target, nmbd and
             * winbind, so how long it sits there is not a number anybody can pick in
             * advance.  systemd publishes the job; asking is exact and counting was
             * a guess.  See bug EIGHT in sharing.h.
             */
            endWatch(false);
        } else if (m_startGraceMs <= 0) {
            endWatch(false);
        }
    }

    /* Rebuilt every tick rather than only when one of those two changed: the
     * address and the volume list live in row details, and neither of them is
     * in a flag this function looks at.  Rebuilding a dozen rows is nothing
     * beside the three forks that got us here. */
    rebuild();
    update();
}

void SharingPage::rebuild()
{
    /* First, because the note that is about to be drawn may have just become two
     * lines long, and the list has to have moved out of its way before the paint. */
    layOut();

    const int keep = m_list->current();
    QVector<ListRow> rows;

    ListRow h;
    h.kind = ListRow::Header;
    ListRow r;

    if (!sambaInstalled()) {
        h.text = tr("Network share");
        rows << h;

        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Samba is not installed");
        r.detail = tr("No /usr/sbin/smbd on this card -- apt install samba");
        r.enabled = false;
        r.id = IdInert;
        rows << r;

        m_list->setRows(rows);
        if (keep >= 0 && keep < rows.size())
            m_list->setCurrent(keep);
        return;
    }

    h.text = tr("Network share");
    rows << h;

    /*
     * Said only when it is true AND smbd is up or coming up.  On a card where
     * sharing has never been switched on the shipped file is inert -- smbd is
     * disabled and nothing runs -- and a warning about a daemon that is not
     * running is a warning that teaches people to ignore warnings.  If smbd got
     * started or enabled by some other hand, though, this is the one row on the
     * page that matters, so it goes above everything.
     */
    if (!configIsOurs() && (m_active || m_enabled)) {
        r = ListRow();
        r.kind = ListRow::Action;
        r.text = tr("Shared without a password");
        r.detail = tr("smbd is using the configuration the image ships, which lets "
                      "anyone on the network read and write /opt and Home.  Press "
                      "to replace it.");
        r.accent = Theme::red();
        r.badge = tr("open");
        r.badgeColour = Theme::red();
        r.id = IdRewrite;
        rows << r;
    }

    r = ListRow();
    r.kind = ListRow::Toggle;
    r.text = tr("Share over the network");
    /*
     * THE SWITCH FOLLOWS THE INSTRUCTION AND THE LINE UNDER IT FOLLOWS THE DAEMON,
     * which is the only honest way to draw a thing that takes half a minute to
     * happen.  Every report this page has ever drawn was some version of "the knob
     * pulls back to off", and half of the reason was this line: the switch was
     * wired straight to is-active, so between the press and smbd saying READY it
     * sat in the off position as though the press had been refused.  It had not
     * been.  On is now "a start is in flight or has landed", and the detail says
     * which of the two, so nothing here claims the share is reachable before it is.
     */
    r.detail = m_active   ? tr("smbd is running")
             : m_starting ? tr("Starting -- smbd has not answered yet")
                          : tr("Off.  Nothing on this device is reachable from the network.");
    r.on = m_active || m_starting;
    if (m_starting && !m_active) {
        r.badge = tr("starting");
        r.badgeColour = Theme::yellow();
    }
    r.accent = Theme::green();
    r.id = IdShare;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Toggle;
    r.text = tr("Start at boot");
    r.detail = m_enabled ? tr("smbd comes up by itself")
                         : tr("Off.  Sharing has to be switched on after each boot.");
    r.on = m_enabled;
    r.accent = Theme::blue();
    r.id = IdAtBoot;
    rows << r;

    /* ── how to reach it ── */

    h.text = tr("From a PC");
    rows << h;

    const QStringList ips = addresses();
    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("Address");
    if (ips.isEmpty()) {
        r.detail = tr("No network address -- join a network on the Wi-Fi page first");
    } else {
        /* Both forms, because the two operating systems somebody is likely to be
         * sitting in front of want different ones and neither accepts the
         * other's.  Windows Explorer takes the backslash form; macOS Finder's
         * Connect to Server and every Linux file manager take the URL. */
        r.detail = QStringLiteral("\\\\%1\\   ---   smb://%1/").arg(ips.first());
    }
    r.enabled = false;
    r.id = IdInert;
    rows << r;

    if (ips.size() > 1) {
        r = ListRow();
        r.kind = ListRow::Item;
        r.text = tr("Other addresses");
        r.detail = ips.mid(1).join(QStringLiteral(", "));
        r.enabled = false;
        r.id = IdInert;
        rows << r;
    }

    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("User name");
    r.detail = QString::fromLatin1(kUser);
    r.enabled = false;
    r.id = IdInert;
    rows << r;

    const QString pw = storedPassword();
    r = ListRow();
    r.kind = ListRow::Action;
    r.text = tr("Password");
    if (pw.isEmpty())
        r.detail = tr("Not set yet -- it is made when sharing is first switched on");
    else if (m_reveal)
        r.detail = pw;
    else
        r.detail = tr("Hidden.  Press to show it.");
    r.accent = Theme::yellow();
    r.id = pw.isEmpty() ? IdInert : IdShowPassword;
    r.enabled = !pw.isEmpty();
    rows << r;

    /* ── what is in there ── */

    h.text = tr("What is shared");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("Home");
    r.detail = tr("%1 -- the DATA partition, all of it").arg(QString::fromLatin1(kHome));
    r.badge = tr("read and write");
    r.badgeColour = Theme::teal();
    r.enabled = false;
    r.id = IdInert;
    rows << r;

    const QStringList volumes = mountedVolumes();
    r = ListRow();
    r.kind = ListRow::Item;
    r.text = tr("Disks");
    if (volumes.isEmpty()) {
        /*
         * The state this board is in today, and the row says so in the words
         * that make it not look broken.  The share exists either way: a stick
         * mounted after the fact appears inside it with no restart, which is the
         * whole reason /media is exported as one share.
         */
        r.detail = tr("%1 -- nothing plugged in yet; disks appear here as they mount")
                       .arg(QString::fromLatin1(kDisks));
    } else if (volumes.size() == 1) {
        r.detail = tr("%1 -- %2").arg(QString::fromLatin1(kDisks), volumes.first());
    } else {
        r.detail = tr("%1 -- %2").arg(QString::fromLatin1(kDisks),
                                      volumes.join(QStringLiteral(", ")));
    }
    r.badge = tr("read and write");
    r.badgeColour = Theme::teal();
    r.enabled = false;
    r.id = IdInert;
    rows << r;

    /* ── the two things that rewrite something ── */

    h.text = tr("Maintenance");
    rows << h;

    r = ListRow();
    r.kind = ListRow::Action;
    r.text = tr("New password");
    r.detail = tr("Makes a new one and forgets the old.  Clients have to be told again.");
    r.accent = Theme::orange();
    r.id = IdNewPassword;
    rows << r;

    r = ListRow();
    r.kind = ListRow::Action;
    r.text = tr("Rewrite configuration");
    r.detail = tr("Puts %1 back the way this page writes it")
                   .arg(QString::fromLatin1(kConfig));
    r.accent = Theme::purple();
    r.id = IdRewrite;
    rows << r;

    /*
     * ALWAYS PRESENT, not only after a failure.  The report this row writes is the
     * one thing on this device that can answer "why did it not share" from the other
     * side of a card reader, and the moment somebody wants it is not necessarily a
     * moment this page happens to be in a failed state -- "it shares but the PC
     * cannot see it" is the same question with none of the same symptoms.
     */
    r = ListRow();
    r.kind = ListRow::Action;
    r.text = tr("Write a diagnosis");
    r.detail = m_reportPath.isEmpty()
                   ? tr("Everything this page knows about smbd, onto the BOOT "
                        "partition -- readable on a PC with the card in it")
                   : tr("Written to %1 -- press to write it again").arg(m_reportPath);
    r.accent = Theme::blue();
    r.id = IdDiagnose;
    rows << r;

    m_list->setRows(rows);
    if (keep >= 0 && keep < rows.size())
        m_list->setCurrent(keep);
}

void SharingPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;

    switch (rows[index].id) {
    case IdShare:
        /* m_starting counts as on, because that is the position the switch is
         * drawn in -- pressing a switch that looks on has to turn it off, and a
         * stop is also the right way to call off a start still in flight. */
        if (m_active || m_starting)
            stop();
        else
            start();
        break;

    case IdAtBoot: {
        const bool turningOn = !m_enabled;
        /*
         * Configure before enabling, not before starting only.  Enabling and then
         * rebooting is a path to a running smbd that never went through start(),
         * and on a card whose smb.conf is still the shipped one that reboot is
         * what publishes /opt and the DATA partition to the network with no
         * password.  The one line below is the whole of the fix.
         */
        if (turningOn) {
            const QString err = ensureConfigured();
            if (!err.isEmpty()) {
                m_note = err;
                break;
            }
        }
        const QString err = setEnabled(QString::fromLatin1(kSmbd), turningOn);
        /* nmbd's is best-effort here for the same reason its start is: the share
         * works without the NetBIOS name, so its failure is not the switch's. */
        setEnabled(QString::fromLatin1(kNmbd), turningOn);

        /* Same rule as poll()'s: a switch is redrawn from an answer and not from
         * silence.  If this fork did not run, the press stands as made and the
         * next tick reads the card. */
        bool answered = false;
        const bool now = unitEnabled(QString::fromLatin1(kSmbd), &answered);
        if (answered)
            m_enabled = now;
        else
            m_enabled = turningOn;
        if (!err.isEmpty())
            m_note = err;
        else
            m_note = m_enabled ? tr("sharing will start at boot")
                               : tr("sharing will not start at boot");
        break;
    }

    case IdShowPassword:
        m_reveal = !m_reveal;
        break;

    case IdNewPassword: {
        const QString err = newPassword();
        if (err.isEmpty()) {
            m_note = tr("new password set");
            emit toastRequested(tr("New share password set"), 2500);
        } else {
            m_note = err;
        }
        break;
    }

    case IdRewrite: {
        /* writeConfig() and not ensureConfigured(): this row's job is to put the
         * file back even when it already carries the marker, which is the case
         * ensureConfigured() deliberately skips.  The password is still made if
         * there is none, because a share with no samba account in it is a share
         * nobody can open. */
        QString err = writeConfig();
        if (err.isEmpty() && storedPassword().isEmpty())
            err = newPassword();
        if (!err.isEmpty()) {
            m_note = err;
            break;
        }
        /* Only if it is running.  A reload of a stopped unit is an error on some
         * systemd versions and a no-op on others, and neither is worth putting on
         * the glass when the answer is "it will read it when it starts". */
        if (m_active) {
            systemctl(QStringList() << QStringLiteral("reload")
                                    << QString::fromLatin1(kSmbd),
                      8000);
            m_note = tr("configuration rewritten and reloaded");
        } else {
            m_note = tr("configuration rewritten");
        }
        break;
    }

    case IdDiagnose: {
        /* Seconds, not milliseconds: this forks journalctl twice and testparm once,
         * and testparm is a cold load of the whole samba library stack off the card.
         * It blocks the page for as long as that takes, which is the right trade for
         * a row somebody pressed on purpose -- and Shell's waits keep the console
         * from being taken while it happens. */
        const QString reason = m_lastFailure.isEmpty()
                                   ? tr("asked for from the Sharing page")
                                   : m_lastFailure;
        const QString path = writeReport(reason);
        if (path.isEmpty()) {
            m_note = tr("could not write the diagnosis -- is the card full, or "
                        "mounted read-only?");
        } else {
            m_reportPath = path;
            m_note = tr("diagnosis written to %1").arg(path);
            emit toastRequested(tr("Diagnosis written"), 2500);
        }
        break;
    }

    default:
        return;
    }

    rebuild();
    update();
}

bool SharingPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:   m_list->step(-1); return true;
    case Joypad::NavDown: m_list->step(1); return true;
    case Joypad::NavOk:   m_list->press(); return true;
    /*
     * NO `m_list->adjust()' ON LEFT AND RIGHT, which is a deliberate break with
     * settingspage.cpp.  There it is right: the rows are sliders, and nudging
     * brightness is meant to be cheap.  Here the switches start a daemon, write
     * /etc and put this device's files on the network, and a d-pad nudge is not
     * an amount of intent that should do any of that.  A is the only way to flip
     * them.
     *
     * They fall through to the shell, which does nothing with them here: it turns
     * a refused left or right into a change of root page only from a root page,
     * and this one is pushed.  B is the way out.  See Dashboard::onNav.
     */
    default:
        return false;
    }
}

void SharingPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    /* Not "on" and "off" on their own: those are two of the most generic words
     * this program could put in the phrase table, and stringsdb keys on the
     * English literal -- a row called "on" would translate every other page's
     * "on" with it. */
    const QString right = !sambaInstalled() ? tr("no samba")
                          : m_active        ? tr("sharing on")
                          : m_starting      ? tr("sharing starting")
                                            : tr("sharing off");
    const QRectF body = paintSheet(p, card, tr("Sharing"), right);

    /* The same one line wifi.cpp puts here, for the same reason: what just
     * happened, above the list rather than in a toast that has already gone --
     * except that here it is allowed to be two, and it elides instead of being
     * clipped.  See noteLine() for why that mattered. */
    p.setFont(Theme::font(12));
    p.setPen(m_active ? Theme::green() : m_starting ? Theme::yellow() : Theme::ink2());
    const int rows = noteLines();
    const QRect box(int(body.x()) + 12, int(body.y()) + 2,
                    int(body.width()) - 24, rows * kNoteRowH);
    paintNote(p, box, noteLine(), rows);
}
