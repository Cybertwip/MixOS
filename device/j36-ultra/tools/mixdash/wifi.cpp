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
#include <QProcessEnvironment>
#include <QResizeEvent>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>

#include "joypad.h"
#include "settings.h"
#include "shell.h"
#include "theme.h"

namespace {

/* Where NetworkManager's keyfile plugin keeps profiles, and writes its own. */
const char *kProfileDir = "/etc/NetworkManager/system-connections";

/*
 * NMDeviceState, from NetworkManager.h.  The numbers are used rather than the
 * words beside them because nmcli translates the words and never the number, and
 * this page has to read the same output on a device set to Portuguese.
 */
enum {
    StateUnmanaged    = 10,
    StateUnavailable  = 20,
    StateDisconnected = 30,
    StatePrepare      = 40,
    StateConfig       = 50,
    StateNeedAuth     = 60,
    StateIpConfig     = 70,
    StateIpCheck      = 80,
    StateSecondaries  = 90,
    StateActivated    = 100,
    StateDeactivating = 110,
    StateFailed       = 120
};

QString firstExecutable(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

QString nmcliPath()
{
    static const QString path = firstExecutable(QStringList()
                                                << "/usr/bin/nmcli"
                                                << "/bin/nmcli"
                                                << "/usr/local/bin/nmcli");
    return path;
}

QString stateWord(int state)
{
    switch (state) {
    case StateUnmanaged:    return WifiPage::tr("not managed");
    case StateUnavailable:  return WifiPage::tr("radio off");
    case StateDisconnected: return WifiPage::tr("not connected");
    case StatePrepare:
    case StateConfig:       return WifiPage::tr("connecting");
    case StateNeedAuth:     return WifiPage::tr("passphrase refused");
    case StateIpConfig:     return WifiPage::tr("asking for an address");
    case StateIpCheck:
    case StateSecondaries:  return WifiPage::tr("checking");
    case StateActivated:    return WifiPage::tr("connected");
    case StateDeactivating: return WifiPage::tr("disconnecting");
    case StateFailed:       return WifiPage::tr("failed");
    default:                return QString();
    }
}

/*
 * 169.254.0.0/16 is IPv4LL, and its presence on a wireless interface means one
 * thing only: a DHCP client gave up waiting for the router and made an address up
 * so that it would have something to report.  It is not a lease, there is no
 * gateway behind it and nothing on the LAN can be reached through it -- which is
 * exactly what "connected but no internet" looks like from the outside.
 */
bool isLinkLocal(const QString &address)
{
    return address.startsWith(QLatin1String("169.254."));
}

/* NMDeviceStateReason, from NetworkManager.h, and only the ones this radio can
 * actually produce.  Numeric for the same reason NMDeviceState is: nmcli prints
 * the number in brackets in every language and the sentence in only one. */
enum {
    ReasonConfigFailed         = 4,
    ReasonIpConfigUnavailable  = 5,
    ReasonIpConfigExpired      = 6,
    ReasonNoSecrets            = 7,
    ReasonSupplicantDisconnect = 8,
    ReasonSupplicantConfig     = 9,
    ReasonSupplicantFailed     = 10,
    ReasonSupplicantTimeout    = 11,
    ReasonDhcpStartFailed      = 15,
    ReasonDhcpError            = 16,
    ReasonDhcpFailed           = 17,
    ReasonFirmwareMissing      = 35,
    ReasonRemoved              = 36,
    ReasonSleeping             = 37,
    ReasonConnectionRemoved    = 38,
    ReasonUserRequested        = 39,
    ReasonDependencyFailed     = 50,
    ReasonSsidNotFound         = 53
};

/*
 * nmcli's exit status, from its manual page.  It is coarser than the reason code
 * and it is always there, which is why both are read: a `--wait' that runs out
 * gives 3 and no reason at all, because nothing has failed yet.
 */
enum {
    ExitOk         = 0,
    ExitError      = 1,
    ExitBadInput   = 2,
    ExitTimeout    = 3,
    ExitActivation = 4,
    ExitNoManager  = 8,
    ExitNoSuch     = 10
};

/* True while NetworkManager is in the middle of bringing a connection up: from
 * `prepare' through `secondaries', which is everything between the button and an
 * address.  Nothing that takes the antenna off the AP's channel may happen in
 * here -- a thirteen-channel sweep during a four-way handshake or a DHCP exchange
 * is how a working network becomes "Timeout expired". */
bool isActivating(int state)
{
    return state >= StatePrepare && state <= StateSecondaries;
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

    /*
     * The query pump.  One QProcess, reused, plus a timer that kills a child that
     * has stopped answering -- QProcess enforces no deadline of its own once
     * nobody is sitting in waitForFinished, and a wedged nmcli holding the pump
     * would stop the page updating for ever.
     */
    m_query = new QProcess(this);
    m_query->setProcessChannelMode(QProcess::SeparateChannels);
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
        m_query->setProcessEnvironment(env);
    }
    connect(m_query, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus status) {
                queryFinished(code, status == QProcess::NormalExit);
            });
    /* A child that never started is not a child that answered "no": nmcli missing
     * or unrunnable must not be read as "NetworkManager is down". */
    connect(m_query, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            queryFinished(-1, false);
    });

    m_guard = new QTimer(this);
    m_guard->setSingleShot(true);
    connect(m_guard, &QTimer::timeout, this, [this]() {
        if (m_query->state() != QProcess::NotRunning)
            m_query->kill();
    });

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

    m_note.clear();
    m_profileAge = 0;
    m_tick = 0;

    /*
     * No rfkill poke and no SIOCSIFFLAGS here, and their absence is the point.
     * NetworkManager keeps its own record of whether the radio should be on and
     * re-applies it; a page that reached past it to /sys/class/rfkill would win
     * for about a second.  `nmcli radio wifi on' is the same switch, asked of the
     * thing that owns it.
     */
    refreshStatus();
    refreshProfiles();
    refreshScan();

    /* A rescan on entry, because the first thing anybody wants from this page is
     * the list.  It is asynchronous twice over -- it goes on the queue behind the
     * status queries, and NetworkManager then sweeps in its own time and a later
     * poll reads what it found. */
    enqueue(QueryRescan);
    m_scanAge.restart();

    pumpQueries();
    rebuild();

    m_timer->setInterval(2000);
    m_timer->start();
}

void WifiPage::onLeave()
{
    m_timer->stop();
    /* Drop what has not started.  The one in flight is left alone: killing it
     * would leave a half-read pipe for nothing, and its handler only writes to
     * fields this page owns. */
    m_queue.clear();
}

/* ── talking to NetworkManager ───────────────────────────────────────────── */

QString WifiPage::nmcli(const QStringList &args, int timeoutMs, int *exitCode,
                        QString *errorOut) const
{
    if (exitCode)
        *exitCode = -1;
    if (errorOut)
        errorOut->clear();
    if (nmcliPath().isEmpty())
        return QString();

    /*
     * BLOCKING, BOUNDED, AND NO LONGER ON THE POLL PATH.  What is left here is the
     * handful of verbs a person has just pressed a button to cause -- deleting a
     * profile, reloading them -- where the next line of the caller genuinely needs
     * the result and where a pause is the expected shape of a button.  Everything
     * that runs on a timer goes through enqueue()/pumpQueries() instead, because
     * Shell::waitForFinished does not pump the event loop and three of these per
     * tick froze the panel for most of every two seconds; see the note in wifi.h.
     *
     * The long verbs -- `connection up' and `device connect', either of which can
     * sit for the better part of a minute on a four-way handshake and a DHCP
     * exchange -- have always gone through startAction() and still do.
     */
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);

    /*
     * LC_ALL=C, because several of the columns read below are English words that
     * nmcli would otherwise translate -- `enabled', `disabled', the parenthesised
     * half of `100 (connected)'.  A dashboard that speaks six languages must not
     * ask its tools to speak any of them.
     */
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    p.setProcessEnvironment(env);

    p.start(nmcliPath(), args);
    if (!Shell::waitForStarted(p, 1500))
        return QString();
    if (!Shell::waitForFinished(p, timeoutMs)) {
        p.kill();
        Shell::waitForFinished(p, 500);
        if (errorOut)
            *errorOut = tr("nmcli did not answer");
        return QString();
    }

    if (exitCode)
        *exitCode = p.exitCode();
    if (errorOut)
        *errorOut = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return QString::fromUtf8(p.readAllStandardOutput());
}

/* ── the background query pump ───────────────────────────────────────────── */

QStringList WifiPage::queryArgs(const Query &query) const
{
    switch (query.id) {
    case QueryRadio:
        return QStringList() << "-t" << "radio" << "wifi";
    case QueryDevice:
        return QStringList() << "-t" << "-f"
                             << "GENERAL.STATE,GENERAL.CONNECTION,"
                                "IP4.ADDRESS,IP4.GATEWAY"
                             << "device" << "show" << m_iface;
    case QueryProfiles:
        return QStringList() << "-t" << "-f" << "NAME,UUID,TYPE"
                             << "connection" << "show";
    case QuerySsid:
        return QStringList() << "-t" << "-f" << "802-11-wireless.ssid"
                             << "connection" << "show" << "uuid" << query.arg;
    case QueryScan: {
        QStringList fields;
        fields << "IN-USE" << "SSID";
        if (m_ssidHex)
            fields << "SSID-HEX";
        fields << "BSSID" << "SIGNAL" << "FREQ" << "SECURITY";
        return QStringList() << "-t" << "-f" << fields.join(QChar(','))
                             << "device" << "wifi" << "list"
                             << "ifname" << m_iface << "--rescan" << "no";
    }
    case QueryRescan:
        return QStringList() << "device" << "wifi" << "rescan" << "ifname" << m_iface;
    default:
        return QStringList();
    }
}

/*
 * Add a query unless the same one is already waiting or running.  The
 * de-duplication is what makes it safe to enqueue unconditionally from the poll:
 * a tick that lands while the queue is still draining -- which is what happens on
 * a loaded board, or when nmcli is being slow -- adds nothing rather than piling
 * up a backlog that then runs all at once.
 */
void WifiPage::enqueue(int id, const QString &arg)
{
    if (nmcliPath().isEmpty() || m_iface.isEmpty())
        return;
    if (m_querying && m_inFlight.id == id && m_inFlight.arg == arg)
        return;
    for (int i = 0; i < m_queue.size(); ++i)
        if (m_queue[i].id == id && m_queue[i].arg == arg)
            return;

    Query q;
    q.id = id;
    q.arg = arg;
    /* A scan listing serialises every AP the daemon has seen, and a rescan can sit
     * behind the radio finishing whatever it was doing.  The rest are one D-Bus
     * property read. */
    q.timeoutMs = (id == QueryScan) ? 8000 : (id == QueryRescan ? 6000 : 5000);
    m_queue.append(q);
}

void WifiPage::pumpQueries()
{
    if (m_querying || m_queue.isEmpty())
        return;
    if (nmcliPath().isEmpty()) {
        m_queue.clear();
        return;
    }

    m_inFlight = m_queue.takeFirst();
    const QStringList args = queryArgs(m_inFlight);
    if (args.isEmpty()) {
        pumpQueries();
        return;
    }

    m_querying = true;
    m_guard->start(m_inFlight.timeoutMs);
    m_query->start(nmcliPath(), args);
}

/*
 * `answered' is the whole reason the exit code is not enough.  A child we killed
 * on the guard timer, or one that never started because nmcli is not there, has
 * a non-zero code and has said nothing -- and reading that as "NetworkManager is
 * down" would blank a working page every time the box was busy.  Silence leaves
 * the last known state exactly where it was.
 */
void WifiPage::queryFinished(int exitCode, bool answered)
{
    if (!m_querying)
        return;

    m_guard->stop();
    m_querying = false;

    const QString out = QString::fromUtf8(m_query->readAllStandardOutput());
    m_query->readAllStandardError();

    const Query done = m_inFlight;
    m_inFlight = Query();

    if (answered) {
        switch (done.id) {
        case QueryRadio:    applyRadio(exitCode, out); break;
        case QueryDevice:   applyDevice(exitCode, out); break;
        case QueryProfiles: applyProfiles(exitCode, out); break;
        case QuerySsid:     applySsid(done.arg, exitCode, out); break;
        case QueryScan:     applyScan(exitCode, out); break;
        case QueryRescan:   break;   /* nothing comes back but a scan request */
        default:            break;
        }
        rebuild();
    }

    pumpQueries();
}

bool WifiPage::busy() const
{
    return m_action && m_action->state() != QProcess::NotRunning;
}

void WifiPage::startAction(const QStringList &args, const QString &note)
{
    if (nmcliPath().isEmpty())
        return;
    if (busy()) {
        emit toastRequested(tr("Still working on the last one"), 2500);
        return;
    }

    if (!m_action) {
        m_action = new QProcess(this);
        m_action->setProcessChannelMode(QProcess::SeparateChannels);

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
        m_action->setProcessEnvironment(env);

        connect(m_action, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) {
                    const QString err =
                        QString::fromUtf8(m_action->readAllStandardError()).trimmed();
                    m_action->readAllStandardOutput();
                    if (code == 0) {
                        m_note.clear();
                        m_badKeySsid.clear();
                    } else {
                        /*
                         * WHAT WENT WRONG, IN WORDS, AND WHY THEY ARE NOT nmcli'S
                         * WORDS ANY MORE.  This used to print nmcli's own
                         * sentence, on the grounds that it was the only thing
                         * specific enough to be useful.  That was true while there
                         * was nothing to compare it against and stopped being true
                         * the moment the number in front of it was read: "Error:
                         * Connection activation failed: (7) Secrets were required,
                         * but not provided." is an accurate description of an
                         * internal event and it is not what a person needs to be
                         * told.  "Wrong passphrase" is.  failureText() names the
                         * cases this radio can actually produce and falls back to
                         * nmcli's sentence for the rest, so nothing is lost.
                         */
                        const int reason = reasonIn(err);
                        if (reason == ReasonNoSecrets) {
                            if (!m_pending.ssid.isEmpty())
                                m_badKeySsid = m_pending.ssid;
                            else if (!m_ssid.isEmpty())
                                m_badKeySsid = m_ssid;
                        }

                        const QString said = failureText(code, err);
                        m_note = said;
                        emit toastRequested(said, 6000);
                    }
                    refreshStatus();
                    pumpQueries();
                    rebuild();
                });
    }

    m_note = note;
    update();
    m_action->start(nmcliPath(), args);
}

/*
 * nmcli -t escapes a literal `:' and a literal `\' inside a value with a
 * backslash, so the fields cannot be recovered with QString::split(':') -- a
 * BSSID would come apart into six pieces and an SSID with a colon in it would
 * come apart into two.  Walk it instead.
 */
QStringList WifiPage::splitTerse(const QString &line)
{
    QStringList out;
    QString cur;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QChar('\\') && i + 1 < line.size()) {
            cur += line.at(++i);
        } else if (c == QChar(':')) {
            out.append(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.append(cur);
    return out;
}

/* ── what went wrong ─────────────────────────────────────────────────────── */

/*
 * The number in "Error: Connection activation failed: (7) Secrets were required,
 * but not provided."  It is an NMDeviceStateReason and it is the only part of that
 * line that is the same in every language, which is the whole reason for reading
 * it rather than the sentence.  -1 when there is no bracketed number at all --
 * `--wait' running out prints no reason, because at that moment nothing has
 * failed.
 */
int WifiPage::reasonIn(const QString &text)
{
    for (int i = 0; i + 1 < text.size(); ++i) {
        if (text.at(i) != QChar('('))
            continue;
        int j = i + 1;
        QString digits;
        while (j < text.size() && text.at(j).isDigit())
            digits += text.at(j++);
        if (!digits.isEmpty() && j < text.size() && text.at(j) == QChar(')'))
            return digits.toInt();
    }
    return -1;
}

QString WifiPage::failureText(int exitCode, const QString &err) const
{
    switch (reasonIn(err)) {
    /*
     * The one everybody meets.  NetworkManager asks for secrets when the AP
     * rejects the key it was given, and because this page always writes the
     * passphrase into the profile before it activates, "no secrets" here can only
     * mean the passphrase in that profile did not work.
     */
    case ReasonNoSecrets:
        return tr("Wrong passphrase");
    case ReasonSupplicantConfig:
        return tr("This radio cannot do that network's security");
    case ReasonSupplicantDisconnect:
    case ReasonSupplicantFailed:
        return tr("The access point dropped the connection");
    case ReasonSupplicantTimeout:
        return tr("The access point stopped answering");
    /* Associated, encrypted, and then nothing came back from the router.  This is
     * the DHCP half of the exchange failing, and it is a different fault from a
     * refused key even though both end with no network. */
    case ReasonIpConfigUnavailable:
    case ReasonIpConfigExpired:
    case ReasonDhcpStartFailed:
    case ReasonDhcpError:
    case ReasonDhcpFailed:
        return tr("The router never sent an address");
    case ReasonSsidNotFound:
        return tr("That network is not in range");
    case ReasonConfigFailed:
        return tr("NetworkManager could not apply those settings");
    case ReasonFirmwareMissing:
        return tr("The Wi-Fi firmware is missing");
    case ReasonRemoved:
        return tr("The wireless interface went away");
    case ReasonSleeping:
        return tr("The radio was put to sleep");
    case ReasonConnectionRemoved:
        return tr("That saved network was deleted");
    case ReasonUserRequested:
        return tr("Something else disconnected it");
    case ReasonDependencyFailed:
        return tr("Something it needed failed first");
    default:
        break;
    }

    switch (exitCode) {
    /*
     * `--wait' ran out.  This is NOT a failure and saying so is the difference
     * between a page that helps and one that lies: NetworkManager is still trying
     * when nmcli gives up on watching it, and the device state says how far it
     * got.  Stuck at ip-config is the DHCP exchange, which on this board was for a
     * long time the group key going in under the wrong address -- see
     * j36_wlan_cfg_add_key() in the driver.
     */
    case ExitTimeout:
        if (m_deviceState == StateIpConfig)
            return tr("Joined, but the router never sent an address");
        if (m_deviceState == StateNeedAuth)
            return tr("Wrong passphrase");
        if (isActivating(m_deviceState))
            return tr("Still trying; it is taking longer than usual");
        return tr("It did not finish in time");
    case ExitNoManager:
        return tr("NetworkManager is not running");
    case ExitNoSuch:
        return tr("That network is gone");
    case ExitBadInput:
        return tr("NetworkManager did not understand that");
    case ExitActivation:
    case ExitError:
    default:
        break;
    }

    /* Nothing recognised.  nmcli's own first line, which is at least exact, and
     * failing that the only thing that is certainly true. */
    const QString line = err.section('\n', 0, 0).trimmed();
    return line.isEmpty() ? tr("NetworkManager refused") : line;
}

/* ── state ───────────────────────────────────────────────────────────────── */

void WifiPage::refreshStatus()
{
    m_managerUp = false;
    m_radioOn = true;
    m_deviceState = -1;
    m_ssid.clear();
    m_address.clear();
    m_gateway.clear();

    if (nmcliPath().isEmpty() || m_iface.isEmpty())
        return;

    /* One command that answers two questions: nmcli exits non-zero with "Error:
     * NetworkManager is not running." when there is nothing to ask. */
    int rc = -1;
    const QString radio = nmcli(QStringList() << "-t" << "radio" << "wifi", 3000, &rc);
    if (rc != 0)
        return;
    m_managerUp = true;
    m_radioOn = (radio.trimmed() != QLatin1String("disabled"));

    const QString show = nmcli(QStringList() << "-t" << "-f"
                                             << "GENERAL.STATE,GENERAL.CONNECTION,"
                                                "IP4.ADDRESS,IP4.GATEWAY"
                                             << "device" << "show" << m_iface,
                               4000, &rc);
    if (rc != 0)
        return;

    for (const QString &line : show.split('\n', Qt::SkipEmptyParts)) {
        const QStringList cols = splitTerse(line);
        if (cols.size() < 2)
            continue;
        const QString key = cols.at(0);
        const QString value = cols.mid(1).join(QChar(':')).trimmed();
        if (value == QLatin1String("--"))
            continue;

        if (key == QLatin1String("GENERAL.STATE")) {
            /* "100 (connected)" in most versions, "100" in some.  Both work. */
            m_deviceState = value.section(' ', 0, 0).toInt();
        } else if (key == QLatin1String("GENERAL.CONNECTION")) {
            m_ssid = value;
        } else if (key.startsWith(QLatin1String("IP4.ADDRESS"))) {
            /* IP4.ADDRESS[1], and there can be several.  The first is the one
             * that matters, and the prefix stays on it: "192.168.1.42/24" is a
             * more useful thing to read on the glass than the address alone,
             * because the mask is half of what people come to this page to
             * check. */
            if (m_address.isEmpty())
                m_address = value;
        } else if (key.startsWith(QLatin1String("IP4.GATEWAY"))) {
            m_gateway = value;
        }
    }
}

void WifiPage::refreshProfiles()
{
    m_profiles.clear();
    if (!m_managerUp)
        return;

    int rc = -1;
    const QString out = nmcli(QStringList() << "-t" << "-f" << "NAME,UUID,TYPE"
                                            << "connection" << "show",
                              4000, &rc);
    if (rc != 0)
        return;

    QSet<QString> live;
    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
        const QStringList cols = splitTerse(line);
        if (cols.size() < 3)
            continue;
        /* Both spellings: nmcli prints the alias on new versions and the settings
         * name on old ones, and an image can be upgraded under us. */
        const QString type = cols.at(2).trimmed();
        if (type != QLatin1String("wifi") && type != QLatin1String("802-11-wireless"))
            continue;

        Profile pr;
        pr.name = cols.at(0);
        pr.uuid = cols.at(1).trimmed();
        if (pr.uuid.isEmpty())
            continue;
        pr.ssid = ssidOfProfile(pr.uuid);
        if (pr.ssid.isEmpty())
            pr.ssid = pr.name;
        m_profiles.append(pr);
        live.insert(pr.uuid);
    }

    /* Forget the SSID of a profile that has been deleted, so a network that is
     * removed and added again does not answer with the old name. */
    for (QHash<QString, QString>::iterator it = m_ssidCache.begin(); it != m_ssidCache.end(); ) {
        if (live.contains(it.key()))
            ++it;
        else
            it = m_ssidCache.erase(it);
    }
}

/*
 * The profile's name is usually its SSID and is not required to be, so the SSID
 * is asked for -- once per profile, and then remembered.  Without the cache this
 * would be a process spawn per saved network every two seconds.
 */
QString WifiPage::ssidOfProfile(const QString &uuid)
{
    QHash<QString, QString>::const_iterator hit = m_ssidCache.constFind(uuid);
    if (hit != m_ssidCache.constEnd())
        return hit.value();

    int rc = -1;
    const QString out = nmcli(QStringList() << "-t" << "-f" << "802-11-wireless.ssid"
                                            << "connection" << "show" << "uuid" << uuid,
                              3000, &rc);
    QString ssid;
    if (rc == 0) {
        for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
            const QStringList cols = splitTerse(line);
            if (cols.size() >= 2 && cols.at(0) == QLatin1String("802-11-wireless.ssid")) {
                ssid = cols.mid(1).join(QChar(':')).trimmed();
                break;
            }
        }
    }
    if (ssid == QLatin1String("--"))
        ssid.clear();

    m_ssidCache.insert(uuid, ssid);
    return ssid;
}

/* nmcli reports signal strength as a percentage already, which is the number
 * every other status bar on every other device shows.  There is no dBm column in
 * `device wifi list', and inventing one back out of the percentage would be
 * precision this page does not have. */
int WifiPage::quality(int percent)
{
    return qBound(0, percent, 100);
}

void WifiPage::refreshScan()
{
    if (!m_managerUp || m_iface.isEmpty())
        return;

    QStringList fields;
    fields << "IN-USE" << "SSID";
    if (m_ssidHex)
        fields << "SSID-HEX";
    fields << "BSSID" << "SIGNAL" << "FREQ" << "SECURITY";

    /*
     * `--rescan no' matters more than it looks.  Without it nmcli asks for a fresh
     * scan on every listing, and on this radio a scan is a thirteen-channel sweep
     * that takes the antenna off the AP for the duration -- which, done every two
     * seconds by a page somebody left open, is enough to break a DHCP exchange in
     * progress.  The list here is whatever NetworkManager last saw; the rescan is
     * a separate, deliberate, occasional thing.
     */
    int rc = -1;
    const QString out = nmcli(QStringList() << "-t" << "-f" << fields.join(QChar(','))
                                            << "device" << "wifi" << "list"
                                            << "ifname" << m_iface << "--rescan" << "no",
                              6000, &rc);
    if (rc != 0) {
        if (m_ssidHex) {
            /* An nmcli old enough not to know SSID-HEX rejects the whole field
             * list rather than the one field, and the page would go blank.  Drop
             * it, once, and never ask again. */
            m_ssidHex = false;
            refreshScan();
        }
        return;
    }

    const int hex = m_ssidHex ? 1 : 0;
    QVector<Ap> found;
    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
        const QStringList cols = splitTerse(line);
        if (cols.size() < 6 + hex)
            continue;

        Ap ap;
        ap.current = (cols.at(0).trimmed() == QLatin1String("*"));
        ap.ssid = cols.at(1);
        if (m_ssidHex)
            ap.ssidHex = cols.at(2).trimmed();
        ap.bssid = cols.at(2 + hex).trimmed();
        ap.signal = cols.at(3 + hex).trimmed().toInt();
        ap.frequency = cols.at(4 + hex).trimmed().section(' ', 0, 0).toInt();
        ap.security = cols.at(5 + hex).trimmed();
        if (ap.security == QLatin1String("--"))
            ap.security.clear();
        if (ap.ssid.isEmpty())
            continue;   /* Hidden: nothing to show and nothing to press. */

        /* The same network on two bands, or two radios of one mesh, comes back as
         * several rows with one name.  Keep the one we are on, or the strongest. */
        bool merged = false;
        for (int i = 0; i < found.size(); ++i) {
            if (found[i].ssid != ap.ssid)
                continue;
            if (ap.current || (!found[i].current && ap.signal > found[i].signal))
                found[i] = ap;
            merged = true;
            break;
        }
        if (!merged)
            found.append(ap);
    }

    for (int i = 0; i < found.size(); ++i) {
        for (int j = 0; j < m_profiles.size(); ++j) {
            if (m_profiles[j].ssid != found[i].ssid && m_profiles[j].name != found[i].ssid)
                continue;
            found[i].saved = true;
            found[i].uuid = m_profiles[j].uuid;
            break;
        }
    }

    std::sort(found.begin(), found.end(), [](const Ap &a, const Ap &b) {
        if (a.current != b.current)
            return a.current;
        if (a.saved != b.saved)
            return a.saved;
        return a.signal > b.signal;
    });

    m_aps = found;
}

void WifiPage::poll()
{
    refreshStatus();

    /* Profiles change when somebody presses a button on this page, and otherwise
     * never.  Every tenth second is often enough to notice one arriving from
     * somewhere else, and cheap enough not to matter. */
    if (++m_profileAge >= 5) {
        m_profileAge = 0;
        refreshProfiles();
    }

    refreshScan();

    /* NetworkManager scans on its own schedule while it is disconnected, and
     * stops when it associates.  This is only for the page that is left open on
     * the list: twenty seconds, and not while a verb is in flight. */
    if (m_managerUp && m_radioOn && !busy() && m_scanAge.elapsed() > 20000) {
        nmcli(QStringList() << "device" << "wifi" << "rescan" << "ifname" << m_iface, 3000);
        m_scanAge.restart();
    }

    rebuild();
}

/* ── joining ─────────────────────────────────────────────────────────────── */

/*
 * GKeyFile's escaping, which is what NetworkManager's keyfile plugin reads these
 * values back through.  A stray backslash makes it reject the whole value with
 * "invalid escape sequence", and a passphrase that begins or ends with a space
 * loses it to the parser's whitespace trimming -- so both are spelled out.
 */
QString WifiPage::keyfileEscape(const QString &value)
{
    QString out;
    out.reserve(value.size() + 8);
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QChar('\\'))
            out += QStringLiteral("\\\\");
        else if (c == QChar('\n'))
            out += QStringLiteral("\\n");
        else if (c == QChar('\t'))
            out += QStringLiteral("\\t");
        else if (c == QChar('\r'))
            out += QStringLiteral("\\r");
        else if (c == QChar(' ') && (i == 0 || i == value.size() - 1))
            out += QStringLiteral("\\s");
        else
            out += c;
    }
    return out;
}

/*
 * The SSID goes into the profile as a list of decimal bytes -- ssid=77;121;78;
 * -- which looks like the awkward way round and is the safe one.  It is the form
 * NetworkManager itself writes for any SSID that is not clean UTF-8, so its
 * reader is required to accept it, and it removes every question about how a
 * space, a semicolon or a byte that is not text at all survives the file.  The
 * bytes come from nmcli's own SSID-HEX column when there is one, so they are the
 * bytes off the air rather than a round trip through a QString.
 */
QString WifiPage::ssidBytes(const Ap &ap)
{
    QString clean;
    for (int i = 0; i < ap.ssidHex.size(); ++i) {
        const char l = ap.ssidHex.at(i).toLower().toLatin1();
        if ((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f'))
            clean += ap.ssidHex.at(i);
    }

    QByteArray raw;
    if (clean.size() >= 2 && (clean.size() % 2) == 0)
        raw = QByteArray::fromHex(clean.toLatin1());
    if (raw.isEmpty())
        raw = ap.ssid.toUtf8();

    QString out;
    for (int i = 0; i < raw.size(); ++i)
        out += QString::number(static_cast<unsigned char>(raw.at(i))) + QLatin1String(";");
    return out;
}

bool WifiPage::writeProfile(const Ap &ap, const QString &passphrase,
                            QString *uuidOut, QString *errorOut)
{
    const QString dirPath = QString::fromLatin1(kProfileDir);
    QDir dir(dirPath);
    if (!dir.exists() && !QDir().mkpath(dirPath)) {
        *errorOut = tr("Cannot write to %1").arg(dirPath);
        return false;
    }

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    /* The filename is cosmetic -- NetworkManager keys on the uuid inside -- so
     * anything a path cannot hold becomes an underscore, and a name that is left
     * with nothing at all falls back to the uuid. */
    QString base;
    for (int i = 0; i < ap.ssid.size(); ++i) {
        const QChar c = ap.ssid.at(i);
        base += (c.isLetterOrNumber() || c == QChar('-') || c == QChar('_'))
                    ? c : QChar('_');
    }
    if (base.isEmpty())
        base = uuid;

    QString path = dirPath + "/" + base + ".nmconnection";
    /* If something is already there it belongs to a different network -- the ones
     * for this SSID were deleted a moment ago -- so do not touch it. */
    if (QFile::exists(path))
        path = dirPath + "/" + uuid + ".nmconnection";

    QString body;
    body += QLatin1String("[connection]\n");
    body += "id=" + keyfileEscape(ap.ssid.isEmpty() ? uuid : ap.ssid) + "\n";
    body += "uuid=" + uuid + "\n";
    body += QLatin1String("type=wifi\n");
    body += "interface-name=" + m_iface + "\n";
    /*
     * The two lines this whole page exists for.  autoconnect=true is what makes
     * the network come back on its own after a reboot, and retries=0 means "keep
     * trying" -- NetworkManager's default is four attempts and then never again
     * until something asks, which on a handheld that gets carried out of range
     * and back is the wrong answer every time.
     */
    body += QLatin1String("autoconnect=true\n");
    body += QLatin1String("autoconnect-retries=0\n");

    body += QLatin1String("\n[wifi]\n");
    body += QLatin1String("mode=infrastructure\n");
    body += "ssid=" + ssidBytes(ap) + "\n";

    if (!passphrase.isEmpty()) {
        body += QLatin1String("\n[wifi-security]\n");
        body += QLatin1String("key-mgmt=wpa-psk\n");
        /*
         * This radio does CCMP and only CCMP -- see j36_wlan_crypto_supported()
         * in the driver, which refuses anything else with -EOPNOTSUPP.  Saying so
         * in the profile means a mixed-mode AP offering TKIP is ruled out by
         * NetworkManager before the association rather than by the driver in the
         * middle of it, and the failure that reaches the glass names the network
         * instead of the kernel.
         */
        body += QLatin1String("pairwise=ccmp;\n");
        body += QLatin1String("group=ccmp;\n");
        body += "psk=" + keyfileEscape(passphrase) + "\n";
    }

    body += QLatin1String("\n[ipv4]\n");
    body += QLatin1String("method=auto\n");
    body += QLatin1String("\n[ipv6]\n");
    body += QLatin1String("method=auto\n");

    /*
     * Written to a dotfile and renamed into place.  NetworkManager watches this
     * directory with inotify and would happily read a half-written profile;
     * names beginning with a dot are on its ignore list, and rename(2) within one
     * directory is atomic, so what it sees is the finished file or nothing.
     */
    const QString tmp = dirPath + "/.mixdash-profile.tmp";
    QFile::remove(tmp);

    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorOut = tr("Cannot write to %1").arg(dirPath);
        return false;
    }
    /* Before the passphrase goes in, not after.  NetworkManager also refuses to
     * load a profile that anyone but root can read, so this is both halves of the
     * same rule. */
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray raw = body.toUtf8();
    const bool written = (f.write(raw) == raw.size());
    f.close();
    if (!written) {
        QFile::remove(tmp);
        *errorOut = tr("Cannot write to %1").arg(dirPath);
        return false;
    }

    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        QFile::remove(tmp);
        *errorOut = tr("Cannot write to %1").arg(dirPath);
        return false;
    }

    *uuidOut = uuid;
    return true;
}

void WifiPage::forgetProfilesFor(const QString &ssid)
{
    if (ssid.isEmpty())
        return;

    refreshProfiles();
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].ssid != ssid && m_profiles[i].name != ssid)
            continue;
        nmcli(QStringList() << "connection" << "delete" << "uuid" << m_profiles[i].uuid, 8000);
    }
    m_ssidCache.clear();
    refreshProfiles();
}

void WifiPage::connectTo(const Ap &ap)
{
    if (!m_managerUp) {
        emit toastRequested(tr("NetworkManager is not running"), 3500);
        return;
    }
    if (busy()) {
        emit toastRequested(tr("Still working on the last one"), 2500);
        return;
    }

    if (ap.saved && !ap.uuid.isEmpty()) {
        /* The key is already in the profile, which is where it belongs and where
         * autoconnect will find it after the next reboot.  Just switch. */
        startAction(QStringList() << "--wait" << "45" << "connection" << "up"
                                  << "uuid" << ap.uuid << "ifname" << m_iface,
                    tr("joining %1").arg(ap.ssid));
        return;
    }

    /*
     * What this radio cannot do, said before the passphrase keyboard rather than
     * after four failed handshakes.  WPA2-PSK with CCMP is the whole list.
     */
    if (ap.security.contains(QLatin1String("802.1X"))) {
        emit toastRequested(tr("This network needs a company login, which this device cannot do"), 5000);
        return;
    }
    if (ap.security.contains(QLatin1String("WEP"))) {
        emit toastRequested(tr("WEP is not supported by this radio"), 4000);
        return;
    }
    if (ap.security.contains(QLatin1String("WPA3"))
        && !ap.security.contains(QLatin1String("WPA2"))) {
        emit toastRequested(tr("This radio does WPA2 only, and this network is WPA3"), 5000);
        return;
    }

    if (ap.security.isEmpty()) {
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
    if (!m_managerUp) {
        emit toastRequested(tr("NetworkManager is not running"), 3500);
        return;
    }

    /*
     * The passphrase never becomes an argument.  `nmcli device wifi connect X
     * password Y' is one line and it puts the key in /proc/<pid>/cmdline, where
     * every user on the box can read it for as long as the process lives -- and
     * on a device whose whole point is that other people's packages get installed
     * later, a key that is briefly public is a key that is public.  So the
     * profile is written here, 0600, and only its uuid is ever passed to nmcli.
     */
    forgetProfilesFor(ap.ssid);

    QString uuid;
    QString err;
    if (!writeProfile(ap, passphrase, &uuid, &err)) {
        m_note = tr("could not save the network");
        emit toastRequested(err.isEmpty() ? tr("Could not write the profile") : err, 5000);
        update();
        return;
    }

    /* inotify would find it on its own in a moment; asking makes the activation
     * below deterministic instead of a race with a directory watcher. */
    nmcli(QStringList() << "connection" << "reload", 8000);
    m_ssidCache.clear();
    refreshProfiles();

    startAction(QStringList() << "--wait" << "45" << "connection" << "up"
                              << "uuid" << uuid << "ifname" << m_iface,
                tr("joining %1").arg(ap.ssid));
    m_scanAge.restart();
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

    if (nmcliPath().isEmpty()) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = QStringLiteral("NetworkManager");
        rows.append(r);

        ListRow explain;
        explain.text = tr("No nmcli on this card");
        explain.detail = tr("NetworkManager owns the radio here. Install network-manager from Packages.");
        explain.enabled = false;
        explain.glyph = GlyphWifi;
        rows.append(explain);

        m_list->setRows(rows);
        update();
        return;
    }

    if (!m_managerUp) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = QStringLiteral("NetworkManager");
        rows.append(r);

        ListRow start;
        start.kind = ListRow::Action;
        start.text = tr("Start NetworkManager");
        start.detail = tr("Nothing is answering on the system bus");
        start.glyph = GlyphWifi;
        start.accent = Theme::orange();
        start.id = RowStartManager;
        rows.append(start);

        m_list->setRows(rows);
        update();
        return;
    }

    if (m_deviceState == StateUnmanaged) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = m_iface;
        rows.append(r);

        ListRow manage;
        manage.kind = ListRow::Action;
        manage.text = tr("Let NetworkManager manage %1").arg(m_iface);
        manage.detail = tr("Nothing will hold an address on it until something does");
        manage.glyph = GlyphWifi;
        manage.accent = Theme::orange();
        manage.id = RowManage;
        rows.append(manage);

        m_list->setRows(rows);
        update();
        return;
    }

    if (!m_radioOn) {
        ListRow r;
        r.kind = ListRow::Header;
        r.text = tr("Networks");
        rows.append(r);

        ListRow on;
        on.kind = ListRow::Action;
        on.text = tr("Turn the radio on");
        on.detail = tr("Wi-Fi is switched off");
        on.glyph = GlyphWifi;
        on.accent = Theme::orange();
        on.id = RowEnableRadio;
        rows.append(on);

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

        QString detail = QString::number(quality(ap.signal)) + "%";
        detail += "  " + (ap.security.isEmpty() ? tr("open") : ap.security);
        if (ap.frequency >= 5000)
            detail += "  5 GHz";
        if (ap.saved)
            detail += "  " + tr("saved");
        r.detail = detail;

        if (ap.current) {
            if (m_deviceState == StateActivated && !m_address.isEmpty()) {
                r.badge = m_address;
                r.badgeColour = isLinkLocal(m_address) ? Theme::orange() : Theme::green();
            } else if (m_deviceState == StateActivated) {
                r.badge = tr("no address");
                r.badgeColour = Theme::orange();
            } else {
                r.badge = stateWord(m_deviceState);
                r.badgeColour = Theme::blue();
            }
        }
        rows.append(r);
    }

    /*
     * The 169.254 line, spelled out where it happens.  An address in that range
     * is the one visible symptom of a DHCP exchange that never completed, and
     * without this row the page reads as a successful connection that mysteriously
     * has no internet behind it.
     */
    if (isLinkLocal(m_address)) {
        ListRow r;
        r.text = tr("The router never answered");
        r.detail = tr("%1 is the address a device gives itself when DHCP fails").arg(m_address);
        r.enabled = false;
        r.glyph = GlyphInfo;
        r.accent = Theme::orange();
        rows.append(r);
    }

    ListRow actions;
    actions.kind = ListRow::Header;
    actions.text = tr("Actions");
    rows.append(actions);

    ListRow rescan;
    rescan.kind = ListRow::Action;
    rescan.text = tr("Scan again");
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
        forget.detail = tr("Remove it from the saved networks");
        forget.glyph = GlyphBack;
        forget.accent = Theme::red();
        forget.id = RowForget;
        rows.append(forget);
    } else {
        ListRow reconnect;
        reconnect.kind = ListRow::Action;
        reconnect.text = tr("Reconnect to a saved network");
        reconnect.detail = m_profiles.isEmpty() ? tr("Nothing is saved yet") : QString();
        reconnect.glyph = GlyphWifi;
        reconnect.accent = Theme::teal();
        reconnect.enabled = !m_profiles.isEmpty();
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
        nmcli(QStringList() << "device" << "wifi" << "rescan" << "ifname" << m_iface, 3000);
        m_scanAge.restart();
        m_note.clear();
        refreshScan();
        rebuild();
        return;
    case RowDisconnect:
        /*
         * `device disconnect' and not `connection down', because the two differ
         * in exactly the way a button labelled Disconnect should: this one also
         * tells NetworkManager to stop auto-connecting on that device until
         * something asks it to, which is what somebody who just pressed it means.
         * It is a runtime flag and does not survive a reboot, so the saved
         * network still comes back on its own tomorrow.
         */
        startAction(QStringList() << "--wait" << "20" << "device" << "disconnect" << m_iface,
                    tr("disconnecting"));
        return;
    case RowForget: {
        const QString ssid = m_ssid;
        forgetProfilesFor(ssid);
        m_note = tr("forgot %1").arg(ssid);
        refreshStatus();
        refreshScan();
        rebuild();
        return;
    }
    case RowReconnect:
        /* No profile named: NetworkManager picks the best one it has for this
         * device, which is the whole point of having saved them. */
        startAction(QStringList() << "--wait" << "45" << "device" << "connect" << m_iface,
                    tr("reconnecting"));
        return;
    case RowEnableRadio:
        startAction(QStringList() << "radio" << "wifi" << "on",
                    tr("turning the radio on"));
        return;
    case RowManage:
        startAction(QStringList() << "device" << "set" << m_iface << "managed" << "yes",
                    tr("handing %1 to NetworkManager").arg(m_iface));
        return;
    case RowStartManager: {
        /*
         * Only reached when the daemon is not running.  systemd owns it in this
         * image, so ask systemd; starting it by hand would leave two of them
         * fighting over the interface the moment the unit came up on its own.
         */
        const QString systemctl = firstExecutable(QStringList()
                                                  << "/bin/systemctl" << "/usr/bin/systemctl");
        if (systemctl.isEmpty()) {
            emit toastRequested(tr("No systemctl on this card"), 3000);
            return;
        }
        QProcess::startDetached(systemctl,
                                QStringList() << "start" << "NetworkManager.service");
        m_note = tr("starting NetworkManager");
        update();
        QTimer::singleShot(3000, this, [this]() {
            refreshStatus();
            refreshProfiles();
            refreshScan();
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
        nmcli(QStringList() << "device" << "wifi" << "rescan" << "ifname" << m_iface, 3000);
        m_scanAge.restart();
        refreshScan();
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
    const QString word = stateWord(m_deviceState);
    if (!word.isEmpty())
        right += "  " + word;
    const QRectF body = paintSheet(p, card, QStringLiteral("Wi-Fi"), right);

    /* One line under the title bar: what the page is doing, or what the address
     * is.  It is the answer to "did that work", and it belongs above the list
     * rather than in a toast that has already gone. */
    QString line = m_note;
    if (line.isEmpty()) {
        if (!m_ssid.isEmpty() && !m_address.isEmpty()) {
            line = m_ssid + " -- " + m_address;
            if (!m_gateway.isEmpty() && !isLinkLocal(m_address))
                line += "  " + tr("via %1").arg(m_gateway);
        } else if (!m_ssid.isEmpty()) {
            line = m_ssid;
        } else {
            line = tr("Not connected");
        }
    }

    p.setFont(Theme::font(12));
    if (m_address.isEmpty())
        p.setPen(Theme::ink2());
    else
        p.setPen(isLinkLocal(m_address) ? Theme::orange() : Theme::green());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}
