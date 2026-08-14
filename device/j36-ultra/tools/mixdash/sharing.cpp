/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * sharing.cpp -- see sharing.h for what this is and why the password is not in
 * this file.
 */
#include "sharing.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    /* Named, not built inline: see dashboard.cpp's firstWad() for the
     * use-after-free that an inline `QStringList() << ...' in a range-for is. */
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

} /* namespace */

/* ── the shell-outs ──────────────────────────────────────────────────────── */

/*
 * BLOCKING AND BOUNDED, the same bargain wifi.cpp strikes with wpa_cli.  These
 * are local unit-state queries that answer in milliseconds; a start is the one
 * that can take a second, which is what the wider default timeout is for.  A
 * timeout returns empty, and every caller reads empty as "no" -- so a wedged
 * systemd shows up as a share that will not switch on rather than as a dashboard
 * that stops painting.
 */
QString SharingPage::systemctl(const QStringList &args, int timeoutMs)
{
    if (systemctlPath().isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(systemctlPath(), args);
    if (!p.waitForStarted(1000))
        return QString();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    return QString::fromUtf8(p.readAll()).trimmed();
}

bool SharingPage::unitActive(const QString &unit)
{
    return systemctl(QStringList() << QStringLiteral("is-active") << unit, 2500)
               == QLatin1String("active");
}

bool SharingPage::unitEnabled(const QString &unit)
{
    /* `is-enabled' prints enabled, disabled, static, masked or alias.  Only the
     * first of those means the unit comes up by itself, and "static" -- which
     * Debian's smbd is not, but a future packaging change could make it -- means
     * there is no install section to enable, so the switch would lie. */
    return systemctl(QStringList() << QStringLiteral("is-enabled") << unit, 2500)
               == QLatin1String("enabled");
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
    if (!p.waitForStarted(2000))
        return tr("smbpasswd would not start");

    const QByteArray twice = (password + "\n" + password + "\n").toUtf8();
    p.write(twice);
    p.closeWriteChannel();
    if (!p.waitForFinished(8000)) {
        p.kill();
        p.waitForFinished(500);
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
    s.flush();
    f.close();
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
    return head == QByteArray(kMarker);
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
        m_note = tr("samba is not installed on this card");
        return;
    }

    /* Config first: smbd reads it at startup, so writing it afterwards would
     * serve the guest-readable shares for the life of this boot. */
    const QString err = ensureConfigured();
    if (!err.isEmpty()) {
        m_note = err;
        return;
    }

    const QString out = systemctl(QStringList() << QStringLiteral("start")
                                                << QString::fromLatin1(kSmbd),
                                  15000);
    /* Best effort, and its failure is not the share's: see the note on kNmbd. */
    systemctl(QStringList() << QStringLiteral("start") << QString::fromLatin1(kNmbd),
              8000);

    m_active = unitActive(QString::fromLatin1(kSmbd));
    if (m_active) {
        const QStringList ips = addresses();
        m_note = ips.isEmpty()
                     ? tr("sharing is on, but this device has no network address yet")
                     : tr("sharing is on at %1").arg(ips.first());
        m_reveal = true;
    } else {
        m_note = out.isEmpty() ? tr("smbd would not start") : out.section('\n', 0, 0);
    }
}

void SharingPage::stop()
{
    systemctl(QStringList() << QStringLiteral("stop") << QString::fromLatin1(kSmbd),
              10000);
    systemctl(QStringList() << QStringLiteral("stop") << QString::fromLatin1(kNmbd),
              8000);
    m_active = unitActive(QString::fromLatin1(kSmbd));
    m_reveal = false;
    m_note = m_active ? tr("smbd is still running") : tr("sharing is off");
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

void SharingPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26);
    QWidget::resizeEvent(event);
}

void SharingPage::onEnter()
{
    m_note.clear();
    /* Masked again on every entry.  The page is on a handheld somebody hands to
     * somebody else, and a password that stays on screen from the last visit is
     * one that gets shoulder-surfed by accident. */
    m_reveal = false;
    poll();
    m_timer->start();
}

void SharingPage::onLeave()
{
    m_timer->stop();
}

void SharingPage::poll()
{
    m_active = unitActive(QString::fromLatin1(kSmbd));
    m_enabled = unitEnabled(QString::fromLatin1(kSmbd));

    /* Rebuilt every tick rather than only when one of those two changed: the
     * address and the volume list live in row details, and neither of them is
     * in a flag this function looks at.  Rebuilding a dozen rows is nothing
     * beside the three forks that got us here. */
    rebuild();
    update();
}

void SharingPage::rebuild()
{
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
    r.detail = m_active ? tr("smbd is running")
                        : tr("Off.  Nothing on this device is reachable from the network.");
    r.on = m_active;
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
        if (m_active)
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
        const QString verb = turningOn ? QStringLiteral("enable")
                                       : QStringLiteral("disable");
        systemctl(QStringList() << verb << QString::fromLatin1(kSmbd), 8000);
        systemctl(QStringList() << verb << QString::fromLatin1(kNmbd), 8000);
        m_enabled = unitEnabled(QString::fromLatin1(kSmbd));
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
                                            : tr("sharing off");
    const QRectF body = paintSheet(p, card, tr("Sharing"), right);

    /* The same one line wifi.cpp puts here, for the same reason: what just
     * happened, above the list rather than in a toast that has already gone. */
    QString line = m_note;
    if (line.isEmpty()) {
        if (!sambaInstalled())
            line = tr("Samba is not on this card");
        else if (m_active)
            line = tr("Home and any plugged-in disk are on the network");
        else
            line = tr("Switch it on to reach this device's files from a PC");
    }
    p.setFont(Theme::font(12));
    p.setPen(m_active ? Theme::green() : Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}
