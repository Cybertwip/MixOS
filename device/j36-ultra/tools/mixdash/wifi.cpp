/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "wifi.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QResizeEvent>
#include <QTimer>

#include <algorithm>

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "joypad.h"
#include "settings.h"
#include "shell.h"
#include "theme.h"

namespace {

const char *kCtrlDir = "/run/wpa_supplicant";

QString firstExecutable(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

QString wpaCli()
{
    static const QString path = firstExecutable(QStringList()
                                                << "/usr/sbin/wpa_cli"
                                                << "/sbin/wpa_cli"
                                                << "/usr/bin/wpa_cli");
    return path;
}

QString wpaPassphrase()
{
    static const QString path = firstExecutable(QStringList()
                                                << "/usr/bin/wpa_passphrase"
                                                << "/usr/sbin/wpa_passphrase"
                                                << "/sbin/wpa_passphrase");
    return path;
}

/*
 * Hex, so the SSID never has to survive a round trip through quoting.
 * wpa_supplicant accepts an unquoted hex string for ssid and treats it as the
 * raw bytes, which is the only encoding that is right for an SSID containing a
 * space, a quote or a byte that is not UTF-8 at all -- and those exist.
 */
QString hexSsid(const QString &ssid)
{
    return QString::fromLatin1(ssid.toUtf8().toHex());
}

} /* namespace */

WifiPage::WifiPage(QWidget *parent)
    : PageWidget(parent)
{
    m_iface = Settings::instance().wifiInterface();
    if (m_iface.isEmpty())
        m_iface = SysInfo::wirelessInterface();

    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    m_list->setPlaceholder(tr("Nothing found yet.\nPress A on Scan again."));
    connect(m_list, &ListPane::activated, this, &WifiPage::onActivated);

    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &WifiPage::poll);

    m_scanAge.start();
}

QString WifiPage::title() const
{
    /* Not translated, and deliberately: it is a trademark of the Wi-Fi Alliance
     * and it is spelled "Wi-Fi" on the box in all six of these languages. */
    return QStringLiteral("Wi-Fi");
}

void WifiPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26);
    QWidget::resizeEvent(event);
}

void WifiPage::onEnter()
{
    if (m_iface.isEmpty())
        m_iface = SysInfo::wirelessInterface();

    unblockRadio();
    bringInterfaceUp();

    refreshStatus();
    rebuild();

    /* A scan on entry, because the first thing anybody wants from this page is
     * the list, and a two-second wait for it is shorter than a press. */
    if (supplicantUp()) {
        cli(QStringList() << "scan");
        m_scanPending = true;
        m_scanAge.restart();
    }

    m_timer->start();
}

void WifiPage::onLeave()
{
    m_timer->stop();
}

/* ── the control socket ──────────────────────────────────────────────────── */

QString WifiPage::cli(const QStringList &args, int timeoutMs) const
{
    if (wpaCli().isEmpty() || m_iface.isEmpty())
        return QString();

    QStringList full;
    full << "-p" << QString::fromLatin1(kCtrlDir) << "-i" << m_iface;
    full += args;

    /*
     * BLOCKING, ON PURPOSE, AND BOUNDED.  Every command here is a request over a
     * unix datagram socket to a process on the same machine; the reply comes back
     * in single-digit milliseconds or the supplicant is wedged, in which case a
     * two-and-a-half-second stall and an empty string is a better answer than a
     * state machine that has to remember what it was in the middle of.  The one
     * genuinely slow operation -- the scan itself -- is NOT waited for: `scan'
     * returns immediately and the results are read on a later poll.
     */
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(wpaCli(), full);
    if (!Shell::waitForStarted(p, 1000))
        return QString();
    if (!Shell::waitForFinished(p, timeoutMs)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAll());
}

bool WifiPage::supplicantUp() const
{
    if (m_iface.isEmpty())
        return false;
    /* The socket file is the honest test.  `wpa_cli status' would also answer, at
     * the cost of a process spawn every two seconds. */
    return QFileInfo::exists(QString::fromLatin1(kCtrlDir) + "/" + m_iface);
}

void WifiPage::unblockRadio()
{
    /*
     * rfkill through sysfs rather than through the rfkill binary: it is three
     * reads and a write, it cannot fail in a way that needs parsing, and it works
     * on an image that never installed the tool.
     */
    const QString base = "/sys/class/rfkill";
    const QStringList nodes = QDir(base).entryList(QStringList() << "rfkill*", QDir::Dirs);
    for (const QString &n : nodes) {
        if (SysInfo::readTrimmed(base + "/" + n + "/type") != "wlan")
            continue;
        if (SysInfo::readTrimmed(base + "/" + n + "/soft") == "0")
            continue;
        QFile f(base + "/" + n + "/soft");
        if (f.open(QIODevice::WriteOnly))
            f.write("0\n");
    }
}

void WifiPage::bringInterfaceUp()
{
    if (m_iface.isEmpty())
        return;

    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;

    struct ifreq ifr;
    ::memset(&ifr, 0, sizeof(ifr));
    ::strncpy(ifr.ifr_name, m_iface.toLatin1().constData(), IFNAMSIZ - 1);
    if (::ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP)) {
        ifr.ifr_flags |= IFF_UP;
        ::ioctl(s, SIOCSIFFLAGS, &ifr);
    }
    ::close(s);
}

QString WifiPage::ipv4() const
{
    if (m_iface.isEmpty())
        return QString();

    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return QString();

    struct ifreq ifr;
    ::memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_addr.sa_family = AF_INET;
    ::strncpy(ifr.ifr_name, m_iface.toLatin1().constData(), IFNAMSIZ - 1);
    QString out;
    if (::ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        char buf[INET_ADDRSTRLEN] = { 0 };
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            out = QString::fromLatin1(buf);
    }
    ::close(s);
    return out;
}

/* ── state ───────────────────────────────────────────────────────────────── */

void WifiPage::refreshStatus()
{
    m_state.clear();
    m_ssid.clear();

    if (!supplicantUp())
        return;

    const QString status = cli(QStringList() << "status");
    const QStringList lines = status.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        if (key == "wpa_state")
            m_state = value;
        else if (key == "ssid")
            m_ssid = value;
    }

    m_address = ipv4();
}

bool WifiPage::isOpenNetwork(const QString &flags)
{
    return !flags.contains("WPA") && !flags.contains("WEP") && !flags.contains("SAE");
}

QString WifiPage::security(const QString &flags)
{
    if (flags.contains("SAE"))
        return QStringLiteral("WPA3");
    if (flags.contains("WPA2"))
        return QStringLiteral("WPA2");
    if (flags.contains("WPA"))
        return QStringLiteral("WPA");
    if (flags.contains("WEP"))
        return QStringLiteral("WEP");
    return tr("open");
}

/* -30 dBm is as good as it gets in the room with the router; -90 is the floor
 * below which nothing associates.  Linear between them is not physics, but it is
 * the same approximation every other status bar makes. */
int WifiPage::quality(int dbm)
{
    return qBound(0, (dbm + 90) * 100 / 60, 100);
}

void WifiPage::refreshScan()
{
    if (!supplicantUp())
        return;

    /* Saved networks first, so a scanned AP can be marked as one we know. */
    QVector<QPair<QString, int> > saved;   /* ssid, network id */
    int currentId = -1;
    const QStringList netLines = cli(QStringList() << "list_networks").split('\n');
    for (const QString &line : netLines) {
        const QStringList cols = line.split('\t');
        if (cols.size() < 2)
            continue;
        bool ok = false;
        const int id = cols.at(0).trimmed().toInt(&ok);
        if (!ok)
            continue;
        saved.append(qMakePair(cols.at(1), id));
        if (cols.size() >= 4 && cols.at(3).contains("CURRENT"))
            currentId = id;
    }

    QVector<Ap> found;
    const QStringList lines = cli(QStringList() << "scan_results").split('\n');
    for (const QString &line : lines) {
        const QStringList cols = line.split('\t');
        /* bssid, frequency, signal level, flags, ssid -- and a hidden network has
         * an empty fifth column, which is why the size test is >= 5 and the ssid
         * is then checked separately. */
        if (cols.size() < 5)
            continue;
        if (cols.at(0) == "bssid")
            continue;

        Ap ap;
        ap.bssid = cols.at(0);
        ap.frequency = cols.at(1).toInt();
        ap.signalDbm = cols.at(2).toInt();
        ap.flags = cols.at(3);
        ap.ssid = cols.at(4);
        if (ap.ssid.isEmpty())
            continue;   /* Hidden: nothing to show and nothing to press. */

        /* The same network on two bands, or two radios of one mesh, comes back as
         * several rows with one name.  Keep the strongest. */
        bool merged = false;
        for (int i = 0; i < found.size(); ++i) {
            if (found[i].ssid != ap.ssid)
                continue;
            if (ap.signalDbm > found[i].signalDbm)
                found[i] = ap;
            merged = true;
            break;
        }
        if (!merged)
            found.append(ap);
    }

    for (int i = 0; i < found.size(); ++i) {
        for (int j = 0; j < saved.size(); ++j) {
            if (saved[j].first != found[i].ssid)
                continue;
            found[i].saved = true;
            found[i].networkId = saved[j].second;
            found[i].current = (saved[j].second == currentId);
            break;
        }
    }

    std::sort(found.begin(), found.end(), [](const Ap &a, const Ap &b) {
        if (a.current != b.current)
            return a.current;
        if (a.saved != b.saved)
            return a.saved;
        return a.signalDbm > b.signalDbm;
    });

    m_aps = found;
}

void WifiPage::poll()
{
    refreshStatus();

    if (m_scanPending && m_scanAge.elapsed() > 1800) {
        refreshScan();
        m_scanPending = false;
    } else if (!m_scanPending && m_scanAge.elapsed() > 20000) {
        /* A slow background rescan so the list does not go stale while the page
         * is open, without hammering the radio while it is trying to associate. */
        if (m_state != "SCANNING" && m_state != "ASSOCIATING")
            cli(QStringList() << "scan");
        m_scanPending = true;
        m_scanAge.restart();
    }

    /*
     * The step every "the Wi-Fi does not work" report is really about.  The
     * supplicant has associated and there is still no address, because nothing in
     * this image runs a DHCP client for a wireless interface.  Give it a few
     * seconds in case something else is doing it, then do it here.
     */
    if (m_state == "COMPLETED" && m_address.isEmpty()) {
        if (++m_addressWait >= 3 && !m_dhcpTried) {
            m_dhcpTried = true;
            startDhcp();
        }
    } else {
        m_addressWait = 0;
        if (!m_address.isEmpty())
            m_dhcpTried = false;
    }

    rebuild();
}

void WifiPage::startDhcp()
{
    if (m_dhcp && m_dhcp->state() != QProcess::NotRunning)
        return;

    /*
     * Whichever client this card has.  dhclient is what Debian installs, udhcpc is
     * what a busybox-slimmed image has, dhcpcd is what somebody may have added.
     * The flags differ enough that a table of (binary, arguments) is clearer than
     * a single command line with conditionals in it.
     */
    struct Client { const char *path; const char *args; };
    static const Client kClients[] = {
        { "/sbin/dhclient",     "-1 -v" },
        { "/usr/sbin/dhclient", "-1 -v" },
        { "/sbin/dhcpcd",       "-t 20" },
        { "/usr/sbin/dhcpcd",   "-t 20" },
        { "/sbin/udhcpc",       "-n -q -t 5 -i" },
        { "/usr/bin/udhcpc",    "-n -q -t 5 -i" },
        { "/bin/busybox",       "udhcpc -n -q -t 5 -i" }
    };

    QString exe;
    QStringList args;
    for (size_t i = 0; i < sizeof(kClients) / sizeof(kClients[0]); ++i) {
        if (!QFileInfo(QString::fromLatin1(kClients[i].path)).isExecutable())
            continue;
        exe = QString::fromLatin1(kClients[i].path);
        args = QString::fromLatin1(kClients[i].args).split(' ', Qt::SkipEmptyParts);
        break;
    }

    if (exe.isEmpty()) {
        m_note = tr("associated, but no DHCP client is installed");
        emit toastRequested(tr("Joined, but nothing here can ask for an address.\n"
                               "Install isc-dhcp-client from Packages."), 5000);
        update();
        return;
    }

    /* udhcpc wants the interface after -i; dhclient and dhcpcd take it last. */
    args << m_iface;

    if (!m_dhcp) {
        m_dhcp = new QProcess(this);
        connect(m_dhcp, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this](int, QProcess::ExitStatus) {
                    m_address = ipv4();
                    m_note = m_address.isEmpty() ? tr("DHCP finished with no address")
                                                 : QString();
                    rebuild();
                });
    }

    m_note = tr("asking for an address");
    update();
    m_dhcp->start(exe, args);
}

/* ── joining ─────────────────────────────────────────────────────────────── */

QString WifiPage::psk(const QString &ssid, const QString &passphrase) const
{
    /* A 64-character hex string is already the PSK; wpa_passphrase would refuse
     * it for being longer than 63 characters. */
    if (passphrase.size() == 64) {
        bool hex = true;
        for (int i = 0; i < 64 && hex; ++i)
            hex = passphrase.at(i).isDigit()
                || (passphrase.at(i).toLower() >= QChar('a') && passphrase.at(i).toLower() <= QChar('f'));
        if (hex)
            return passphrase.toLower();
    }

    if (wpaPassphrase().isEmpty())
        return QString();

    /*
     * The passphrase goes in on STDIN, not in argv.  Anything in argv is world
     * readable in /proc/<pid>/cmdline for as long as the process lives, and on a
     * device where the whole point is that other people's packages get installed
     * later, a Wi-Fi key that is briefly public is a key that is public.
     */
    QProcess p;
    p.start(wpaPassphrase(), QStringList() << ssid);
    if (!Shell::waitForStarted(p, 1000))
        return QString();
    p.write(passphrase.toUtf8());
    p.write("\n");
    p.closeWriteChannel();
    if (!Shell::waitForFinished(p, 3000)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        return QString();
    }

    const QStringList lines = QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n');
    for (const QString &line : lines) {
        const QString t = line.trimmed();
        /* "#psk=..." is the comment wpa_passphrase writes with the plaintext in
         * it; the real one has no hash. */
        if (t.startsWith("psk=") && t.size() == 68)
            return t.mid(4);
    }
    return QString();
}

void WifiPage::connectTo(const Ap &ap)
{
    if (!supplicantUp()) {
        emit toastRequested(tr("wpa_supplicant is not running on %1").arg(m_iface), 3500);
        return;
    }

    if (ap.saved && ap.networkId >= 0) {
        /* Already configured -- and the passphrase is in the supplicant's own
         * config, which is where it belongs.  Just switch to it. */
        cli(QStringList() << "select_network" << QString::number(ap.networkId));
        m_note = tr("joining %1").arg(ap.ssid);
        m_dhcpTried = false;
        m_addressWait = 0;
        update();
        return;
    }

    if (isOpenNetwork(ap.flags)) {
        connectWithKey(ap, QString());
        return;
    }

    m_pending = ap;
    m_awaitingKey = true;
    emit textRequested(tr("Passphrase for %1").arg(ap.ssid), QString(), true);
}

void WifiPage::textEntered(const QString &text, bool accepted)
{
    if (!m_awaitingKey)
        return;
    m_awaitingKey = false;
    if (!accepted)
        return;

    if (text.size() < 8 && text.size() != 0) {
        emit toastRequested(tr("A WPA passphrase is at least 8 characters"), 3000);
        return;
    }
    connectWithKey(m_pending, text);
}

void WifiPage::connectWithKey(const Ap &ap, const QString &passphrase)
{
    const QString idText = cli(QStringList() << "add_network").trimmed();
    bool ok = false;
    /* add_network answers with the id on its own line, sometimes after the
     * "Selected interface" banner wpa_cli prints. */
    int id = -1;
    for (const QString &line : idText.split('\n')) {
        const int v = line.trimmed().toInt(&ok);
        if (ok) {
            id = v;
            break;
        }
    }
    if (id < 0) {
        emit toastRequested(tr("wpa_supplicant would not add a network"), 3000);
        return;
    }

    cli(QStringList() << "set_network" << QString::number(id) << "ssid" << hexSsid(ap.ssid));

    if (passphrase.isEmpty()) {
        cli(QStringList() << "set_network" << QString::number(id) << "key_mgmt" << "NONE");
    } else {
        const QString hex = psk(ap.ssid, passphrase);
        if (hex.isEmpty()) {
            /* No wpa_passphrase on the card.  Fall back to the quoted form, which
             * is correct for every passphrase that does not contain a quote. */
            cli(QStringList() << "set_network" << QString::number(id) << "psk"
                              << ("\"" + passphrase + "\""));
        } else {
            cli(QStringList() << "set_network" << QString::number(id) << "psk" << hex);
        }
        if (ap.flags.contains("SAE")) {
            /* WPA3 personal.  Offering both key managements lets the supplicant
             * pick, which is what a transition-mode AP needs. */
            cli(QStringList() << "set_network" << QString::number(id) << "key_mgmt"
                              << "WPA-PSK SAE");
            cli(QStringList() << "set_network" << QString::number(id) << "ieee80211w" << "1");
        }
    }

    cli(QStringList() << "enable_network" << QString::number(id));
    cli(QStringList() << "select_network" << QString::number(id));
    /* Written to wpa_supplicant.conf so the network is still there after a
     * reboot.  Fails harmlessly if update_config=1 is not set in the file. */
    cli(QStringList() << "save_config");

    m_note = tr("joining %1").arg(ap.ssid);
    m_dhcpTried = false;
    m_addressWait = 0;
    m_scanAge.restart();
    update();
}

/* ── the list ────────────────────────────────────────────────────────────── */

void WifiPage::rebuild()
{
    QVector<ListRow> rows;

    if (m_iface.isEmpty()) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = tr("No wireless interface");
        rows.append(r);

        ListRow explain;
        explain.text = tr("Nothing in /sys/class/net has a phy80211");
        explain.detail = tr("No Wi-Fi hardware, or its driver did not load. See Diagnostics.");
        explain.enabled = false;
        explain.glyph = GlyphWifi;
        rows.append(explain);

        m_list->setRows(rows);
        update();
        return;
    }

    if (!supplicantUp()) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = "wpa_supplicant";
        rows.append(r);

        ListRow start;
        start.kind = ListRow::Action;
        start.text = tr("Start wpa_supplicant on %1").arg(m_iface);
        start.detail = tr("No control socket in /run/wpa_supplicant");
        start.glyph = GlyphWifi;
        start.accent = Theme::orange();
        start.id = RowStartSupplicant;
        rows.append(start);

        m_list->setRows(rows);
        update();
        return;
    }

    ListRow head;
    head.kind = ListRow::Header;
    head.text = tr("Networks");
    rows.append(head);

    for (int i = 0; i < m_aps.size(); ++i) {
        const Ap &ap = m_aps[i];
        ListRow r;
        r.kind = ListRow::Item;
        r.text = ap.ssid;
        r.glyph = GlyphWifi;
        r.id = RowNetwork;
        r.key = ap.ssid;
        r.accent = ap.current ? Theme::green() : Theme::ink2();

        const int q = quality(ap.signalDbm);
        QString detail = tr("%1%  %2 dBm  %3")
                             .arg(q).arg(ap.signalDbm).arg(security(ap.flags));
        if (ap.frequency >= 5000)
            detail += "  5 GHz";
        if (ap.saved)
            detail += "  " + tr("saved");
        r.detail = detail;

        if (ap.current && m_state == "COMPLETED") {
            r.badge = m_address.isEmpty() ? tr("no address") : m_address;
            r.badgeColour = m_address.isEmpty() ? Theme::orange() : Theme::green();
        } else if (ap.current) {
            r.badge = m_state.toLower();
            r.badgeColour = Theme::blue();
        }
        rows.append(r);
    }

    ListRow actions;
    actions.kind = ListRow::Header;
    actions.text = tr("Actions");
    rows.append(actions);

    ListRow rescan;
    rescan.kind = ListRow::Action;
    rescan.text = m_scanPending ? tr("Scanning...") : tr("Scan again");
    rescan.glyph = GlyphWifi;
    rescan.accent = Theme::blue();
    rescan.id = RowRescan;
    rows.append(rescan);

    if (!m_ssid.isEmpty()) {
        ListRow disconnect;
        disconnect.kind = ListRow::Action;
        disconnect.text = tr("Disconnect");
        disconnect.detail = tr("Stay on %1 but drop %2").arg(m_iface, m_ssid);
        disconnect.glyph = GlyphPower;
        disconnect.accent = Theme::orange();
        disconnect.id = RowDisconnect;
        rows.append(disconnect);

        ListRow forget;
        forget.kind = ListRow::Action;
        forget.text = tr("Forget %1").arg(m_ssid);
        forget.detail = tr("Remove it from wpa_supplicant.conf");
        forget.glyph = GlyphBack;
        forget.accent = Theme::red();
        forget.id = RowForget;
        rows.append(forget);
    } else {
        ListRow reconnect;
        reconnect.kind = ListRow::Action;
        reconnect.text = tr("Reconnect to a saved network");
        reconnect.glyph = GlyphWifi;
        reconnect.accent = Theme::teal();
        reconnect.id = RowReconnect;
        rows.append(reconnect);
    }

    /* Rebuilt every two seconds, so the selection has to survive it.  Keyed on
     * the SSID rather than the index, because the list re-sorts by signal. */
    const ListRow *was = m_list->currentRow();
    const QString wasKey = was ? was->key : QString();
    const int wasId = was ? was->id : -1;

    m_list->setRows(rows);

    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].kind == ListRow::Header)
            continue;
        if ((!wasKey.isEmpty() && rows[i].key == wasKey)
            || (wasKey.isEmpty() && wasId >= 0 && rows[i].id == wasId)) {
            m_list->setCurrent(i);
            break;
        }
    }

    update();
}

void WifiPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    const ListRow &row = rows[index];

    switch (row.id) {
    case RowNetwork: {
        for (int i = 0; i < m_aps.size(); ++i) {
            if (m_aps[i].ssid != row.key)
                continue;
            connectTo(m_aps[i]);
            return;
        }
        return;
    }
    case RowRescan:
        cli(QStringList() << "scan");
        m_scanPending = true;
        m_scanAge.restart();
        m_note.clear();
        rebuild();
        return;
    case RowDisconnect:
        cli(QStringList() << "disconnect");
        m_note = tr("disconnected");
        rebuild();
        return;
    case RowForget: {
        const QStringList netLines = cli(QStringList() << "list_networks").split('\n');
        for (const QString &line : netLines) {
            const QStringList cols = line.split('\t');
            if (cols.size() < 4 || !cols.at(3).contains("CURRENT"))
                continue;
            cli(QStringList() << "remove_network" << cols.at(0).trimmed());
            cli(QStringList() << "save_config");
            m_note = tr("forgot %1").arg(m_ssid);
            break;
        }
        rebuild();
        return;
    }
    case RowReconnect:
        cli(QStringList() << "reconnect");
        m_note = tr("reconnecting");
        rebuild();
        return;
    case RowStartSupplicant: {
        /*
         * Only reached when the service is not running.  systemd owns it in this
         * image, so ask systemd; falling back to starting the daemon by hand would
         * leave two supplicants fighting over the interface the moment the unit
         * came up on its own.
         */
        const QString systemctl = firstExecutable(QStringList()
                                                  << "/bin/systemctl" << "/usr/bin/systemctl");
        if (systemctl.isEmpty()) {
            emit toastRequested(tr("No systemctl on this card"), 3000);
            return;
        }
        QProcess::startDetached(systemctl,
                                QStringList() << "start" << ("wpa_supplicant@" + m_iface + ".service"));
        m_note = tr("starting wpa_supplicant@%1").arg(m_iface);
        update();
        QTimer::singleShot(2500, this, [this]() {
            if (supplicantUp()) {
                cli(QStringList() << "scan");
                m_scanPending = true;
                m_scanAge.restart();
            }
            rebuild();
        });
        return;
    }
    default:
        return;
    }
}

bool WifiPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:    m_list->step(-1); return true;
    case Joypad::NavDown:  m_list->step(1); return true;
    case Joypad::NavOk:    m_list->press(); return true;
    case Joypad::NavMenu:
        cli(QStringList() << "scan");
        m_scanPending = true;
        m_scanAge.restart();
        rebuild();
        return true;
    default:
        return false;
    }
}

void WifiPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    QString right = m_iface.isEmpty() ? tr("no interface") : m_iface;
    if (!m_state.isEmpty())
        right += "  " + m_state.toLower();
    const QRectF body = paintSheet(p, card, QStringLiteral("Wi-Fi"), right);

    /* One line under the title bar: what the page is doing, or what the address
     * is.  It is the answer to "did that work", and it belongs above the list
     * rather than in a toast that has already gone. */
    QString line = m_note;
    if (line.isEmpty()) {
        if (!m_ssid.isEmpty() && !m_address.isEmpty())
            line = m_ssid + " -- " + m_address;
        else if (!m_ssid.isEmpty())
            line = m_ssid;
        else
            line = tr("Not connected");
    }
    p.setFont(Theme::font(12));
    p.setPen(m_address.isEmpty() ? Theme::ink2() : Theme::green());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}
