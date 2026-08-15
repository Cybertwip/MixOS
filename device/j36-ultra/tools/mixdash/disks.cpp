/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * disks.cpp -- the reader described in disks.h.
 */
#include "disks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

#include "shell.h"

namespace {

const char *const kStateDir = "/run/mixos/volumes";
const char *const kMountInfo = "/proc/self/mountinfo";
const char *const kAutomount = "/run/j36/bin/mixos-automount";

/* How long after a mount-table wake the scan happens.  Long enough for
 * mixos-automount to have written its state file after the mount that woke us --
 * it is two forks and a handful of writes -- and short enough that a card appears
 * while the hand that plugged it in is still on the stick. */
const int kSettleMs = 400;
/* The backstop.  See the header: this is not how volumes are noticed. */
const int kBackstopMs = 5000;

/*
 * mountinfo escapes space, tab, newline and backslash as three-digit octal, and a
 * volume called "My Disk" is not unusual enough to skip this.
 */
QString unescape(const QString &s)
{
    if (!s.contains(QLatin1Char('\\')))
        return s;
    QString out;
    out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('\\') && i + 3 < s.size()) {
            bool ok = false;
            const int code = s.mid(i + 1, 3).toInt(&ok, 8);
            if (ok && code > 0 && code < 256) {
                out.append(QChar(code));
                i += 3;
                continue;
            }
        }
        out.append(s.at(i));
    }
    return out;
}

/* One live mount, as mountinfo reports it. */
struct MountRow {
    QString point;
    QString source;
    QString fstype;
    bool readOnly = false;
};

/*
 * Every mount whose mount point is strictly below /media or /run/media.
 *
 * Strictly below: /media itself is a directory on the rootfs and /run/media is a
 * tmpfs the mounter may have made, and neither is a volume.  Later entries win,
 * because that is what the kernel does -- a second mount over the same point is
 * what you see when you look there.
 */
QVector<MountRow> liveMounts()
{
    QVector<MountRow> rows;
    QFile f{QLatin1String(kMountInfo)};
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return rows;

    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const int sep = line.indexOf(QLatin1String(" - "));
        if (sep < 0)
            continue;
        const QStringList pre = line.left(sep).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QStringList post = line.mid(sep + 3).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (pre.size() < 6 || post.size() < 2)
            continue;

        MountRow row;
        row.point = unescape(pre.at(4));
        row.fstype = post.at(0);
        row.source = unescape(post.at(1));
        if (!row.point.startsWith(QLatin1String("/media/"))
            && !row.point.startsWith(QLatin1String("/run/media/")))
            continue;

        /* Field 6 is the per-mount option list and `ro' or `rw' is always its
         * first word.  Asked of the kernel rather than inferred from the mount
         * command, because a driver is free to downgrade a mount by itself and
         * ntfs3 does exactly that on a volume Windows fast-booted out of. */
        row.readOnly = pre.at(5).startsWith(QLatin1String("ro"));

        bool replaced = false;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).point == row.point) {
                rows[i] = row;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            rows.append(row);
    }
    return rows;
}

bool isMounted(const QString &point)
{
    const QVector<MountRow> rows = liveMounts();
    for (const MountRow &r : rows) {
        if (r.point == point)
            return true;
    }
    return false;
}

/*
 * Where a tool is, searched rather than assumed.  This rootfs is usrmerged, so
 * /bin/umount and /usr/bin/umount are the same file -- but the payload also lands
 * on cards built before that was true, and a hard-coded path that is a dangling
 * symlink is a failure with no message on it.  Empty means "not installed", which
 * every caller treats as a rung of the ladder that is not there.
 */
QString findTool(const char *name)
{
    for (const char *dir : { "/usr/bin/", "/bin/", "/usr/sbin/", "/sbin/" }) {
        const QString path = QLatin1String(dir) + QLatin1String(name);
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

/* Run it, bounded, with the C locale.  True on a clean exit 0; `said' collects
 * both streams so a failure can be reported in the words the tool used. */
bool run(const QString &exe, const QStringList &args, int msec, QString *said)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, args);
    if (!Shell::waitForStarted(p, 2000)) {
        if (said)
            *said = QStringLiteral("%1: could not run").arg(QFileInfo(exe).fileName());
        p.kill();
        Shell::waitForFinished(p, 500);
        return false;
    }
    const bool done = Shell::waitForFinished(p, msec);
    if (said) {
        const QString out = QString::fromUtf8(p.readAll()).trimmed();
        if (!out.isEmpty())
            *said = out.section(QLatin1Char('\n'), -1).trimmed();
    }
    if (!done) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

} /* namespace */

/* ── Disk ──────────────────────────────────────────────────────────────── */

QString Disk::name() const
{
    if (!label.trimmed().isEmpty())
        return label.trimmed();
    const QString base = mountPoint.section(QLatin1Char('/'), -1);
    if (!base.isEmpty())
        return base;
    return kernel;
}

QString Disk::key() const
{
    return QStringLiteral("vol:") + mountPoint.section(QLatin1Char('/'), -1);
}

/* ── Disks ─────────────────────────────────────────────────────────────── */

Disks &Disks::instance()
{
    static Disks v;
    return v;
}

Disks::Disks(QObject *parent)
    : QObject(parent)
{
}

void Disks::start()
{
    if (m_started)
        return;
    m_started = true;

    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(kSettleMs);
    connect(m_settle, &QTimer::timeout, this, &Disks::rescan);

    /*
     * O_CLOEXEC because this dashboard forks children constantly -- every launch,
     * every amixer probe -- and a descriptor on procfs inherited into a game is a
     * descriptor nothing will ever close.
     */
    m_fd = ::open(kMountInfo, O_RDONLY | O_CLOEXEC);
    if (m_fd >= 0) {
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Exception, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &Disks::onMountEvent);
    }

    m_backstop = new QTimer(this);
    m_backstop->setInterval(kBackstopMs);
    connect(m_backstop, &QTimer::timeout, this, &Disks::rescan);
    m_backstop->start();

    m_list = scan();
}

void Disks::onMountEvent()
{
    if (m_fd < 0)
        return;

    /*
     * CLEAR THE EVENT BEFORE RETURNING, or this fires again the moment the event
     * loop comes round.  The kernel keeps POLLPRI raised on mountinfo until the
     * descriptor has been read to the end from the beginning; the read is what
     * acknowledges it.  The notifier is switched off across it so that a wake
     * cannot be delivered inside its own handler.
     */
    if (m_notifier)
        m_notifier->setEnabled(false);
    if (::lseek(m_fd, 0, SEEK_SET) == 0) {
        char buf[4096];
        while (::read(m_fd, buf, sizeof(buf)) > 0)
            ;
    }
    if (m_notifier)
        m_notifier->setEnabled(true);

    m_settle->start();
}

void Disks::rescan()
{
    const QVector<Disk> next = scan();

    bool same = next.size() == m_list.size();
    for (int i = 0; same && i < next.size(); ++i) {
        same = next.at(i).mountPoint == m_list.at(i).mountPoint
               && next.at(i).device == m_list.at(i).device
               && next.at(i).label == m_list.at(i).label
               && next.at(i).readOnly == m_list.at(i).readOnly;
    }
    if (same)
        return;

    m_list = next;
    emit changed();
}

QVector<Disk> Disks::scan() const
{
    QVector<MountRow> live = liveMounts();
    QVector<Disk> out;

    /*
     * The state files first, because they carry the one thing mountinfo cannot
     * give: the label the filesystem was formatted with, before the mounter
     * sanitised it into a directory name.  A state file whose mount point is not
     * in the mount table is stale -- /run outlived a lazy unmount -- and is
     * dropped rather than listed.
     */
    const QDir dir{QLatin1String(kStateDir)};
    const QStringList names = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &n : names) {
        QFile f(dir.filePath(n));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        Disk v;
        v.kernel = n;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0)
                continue;
            const QString k = line.left(eq);
            const QString val = line.mid(eq + 1);
            if (k == QLatin1String("device"))
                v.device = val;
            else if (k == QLatin1String("mountpoint"))
                v.mountPoint = val;
            else if (k == QLatin1String("fstype"))
                v.fstype = val;
            else if (k == QLatin1String("label"))
                v.label = val;
        }
        if (v.mountPoint.isEmpty())
            continue;

        int at = -1;
        for (int i = 0; i < live.size(); ++i) {
            if (live.at(i).point == v.mountPoint) {
                at = i;
                break;
            }
        }
        if (at < 0)
            continue;
        v.readOnly = live.at(at).readOnly;
        if (v.fstype.isEmpty())
            v.fstype = live.at(at).fstype;
        live.remove(at);
        out.append(v);
    }

    /* Whatever is left is mounted under /media and has no state file: a hand
     * mount, an older card, or the volume this dashboard was told about between
     * the mount and the file. */
    for (const MountRow &r : live) {
        Disk v;
        v.mountPoint = r.point;
        v.fstype = r.fstype;
        v.readOnly = r.readOnly;
        v.device = r.source;
        if (r.source.startsWith(QLatin1String("/dev/")))
            v.kernel = r.source.mid(5);
        out.append(v);
    }

    std::sort(out.begin(), out.end(), [](const Disk &a, const Disk &b) {
        return a.mountPoint < b.mountPoint;
    });
    return out;
}

const Disk *Disks::byKey(const QString &key) const
{
    for (const Disk &v : m_list) {
        if (v.key() == key)
            return &v;
    }
    return nullptr;
}

bool Disks::eject(const QString &key, QString *error)
{
    if (error)
        error->clear();

    const Disk *found = byKey(key);
    if (!found) {
        /* Already gone, which is the state the caller asked for. */
        return true;
    }
    const Disk v = *found;   /* by value: the rescan below invalidates the list */

    QString said;

    /* The right way, when systemd is the one that mounted it.  The unit name is
     * the kernel name and needs no escaping -- a kernel name has no slashes, which
     * mixos-automount asserts before it does anything else. */
    if (!v.kernel.isEmpty()) {
        const QString sc = findTool("systemctl");
        if (!sc.isEmpty()) {
            run(sc, QStringList() << QStringLiteral("stop")
                                  << QStringLiteral("mixos-automount@%1.service").arg(v.kernel),
                20000, &said);
            if (!isMounted(v.mountPoint)) {
                rescan();
                return true;
            }
        }

        /* The mounter's own remove path, for a mount it made without a unit
         * behind it -- --mix-only drops this payload onto cards whose /run was
         * populated by an older initramfs. */
        if (QFileInfo::exists(QLatin1String(kAutomount))) {
            run(QStringLiteral("/bin/sh"),
                QStringList() << QLatin1String(kAutomount) << QStringLiteral("remove") << v.kernel,
                20000, &said);
            if (!isMounted(v.mountPoint)) {
                rescan();
                return true;
            }
        }
    }

    /*
     * And by hand.  sync first, because this is the button whose whole promise is
     * that the stick can now be pulled out; nothing here is lazy, so a volume with
     * a file still open on it fails loudly rather than being detached from under
     * whatever has it.
     */
    const QString sync = findTool("sync");
    if (!sync.isEmpty())
        run(sync, QStringList(), 20000, nullptr);
    const QString umount = findTool("umount");
    if (!umount.isEmpty())
        run(umount, QStringList() << v.mountPoint, 20000, &said);
    const bool gone = !isMounted(v.mountPoint);
    rescan();
    if (!gone && error)
        *error = said;
    return gone;
}
