/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "packages.h"

#include <QFileInfo>
#include <QPainter>
#include <QProcess>
#include <QResizeEvent>

#include <unistd.h>

#include "joypad.h"
#include "theme.h"

namespace {

/* mixdash normally runs as root on this device -- it has to, to hold /dev/fb0 and
 * the evdev nodes -- but it does not have to, and an apt-get that silently fails
 * on permissions is a worse answer than one that asks for a password. */
QString privileged(const QString &command)
{
    if (::geteuid() == 0)
        return command;
    if (QFileInfo("/usr/bin/sudo").isExecutable())
        return "sudo " + command;
    return command;
}

QString firstExisting(const QStringList &paths)
{
    for (const QString &p : paths)
        if (QFileInfo(p).isExecutable())
            return p;
    return QString();
}

QString aptCache()
{
    static const QString p = firstExisting(QStringList() << "/usr/bin/apt-cache");
    return p;
}

QString dpkgQuery()
{
    static const QString p = firstExisting(QStringList() << "/usr/bin/dpkg-query");
    return p;
}

/* Search results are capped: apt-cache search wifi returns hundreds of lines, and
 * a D-pad list of hundreds is not a list, it is a corridor. */
const int kMaxResults = 120;

} /* namespace */

PackagesPage::PackagesPage(QWidget *parent)
    : PageWidget(parent)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    m_list->setPlaceholder(tr("Nothing matched.\nB goes back."));
    connect(m_list, &ListPane::activated, this, &PackagesPage::onActivated);

    buildCollections();
}

QString PackagesPage::title() const
{
    if (m_view == ViewSearch)
        return "Search: " + m_term;
    if (m_view == ViewCollection && m_collection >= 0 && m_collection < m_collections.size())
        return m_collections[m_collection].title;
    return tr("Packages");
}

void PackagesPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 20, card.width() - 12,
                        card.height() - 36 - 26);
    QWidget::resizeEvent(event);
}

void PackagesPage::onEnter()
{
    loadInstalled();
    if (m_view == ViewHome)
        showHome();
    else
        rebuild();
}

/* ── talking to dpkg and apt ─────────────────────────────────────────────── */

QString PackagesPage::run(const QString &program, const QStringList &args, int timeoutMs) const
{
    if (program.isEmpty())
        return QString();

    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(program, args);
    if (!p.waitForStarted(2000))
        return QString();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

void PackagesPage::loadInstalled()
{
    /*
     * One dpkg-query for the whole set rather than one per package.  The status
     * field matters: a package that was removed but not purged still has an entry,
     * and only "install ok installed" means it is actually there.
     */
    m_installed.clear();
    const QString out = run(dpkgQuery(), QStringList()
                                             << "-W"
                                             << "-f=${Package}\\t${db:Status-Status}\\n",
                            15000);
    for (const QString &line : out.split('\n')) {
        const int tab = line.indexOf('\t');
        if (tab <= 0)
            continue;
        if (line.mid(tab + 1).trimmed() == "installed")
            m_installed.insert(line.left(tab));
    }
}

bool PackagesPage::isInstalled(const QString &name) const
{
    return m_installed.contains(name);
}

QString PackagesPage::summaryFor(const QString &name) const
{
    /* apt-cache show is one process per package, which is why this is only used
     * for the handful in a collection and never for a search result. */
    const QString out = run(aptCache(), QStringList() << "show" << name, 4000);
    for (const QString &line : out.split('\n')) {
        if (line.startsWith("Description-en:"))
            return line.section(':', 1).trimmed();
        if (line.startsWith("Description:"))
            return line.section(':', 1).trimmed();
    }
    return QString();
}

/* ── the curated list ────────────────────────────────────────────────────── */

void PackagesPage::buildCollections()
{
    Collection desktops;
    desktops.title = tr("Desktops");
    desktops.subtitle = tr("A full X or Wayland session.  Big, and worth knowing:\n"
                           "they need a display server this board has no KMS for.");
    desktops.accent = Theme::blue();
    desktops.glyph = GlyphDisplay;
    desktops.packages = QStringList()
                        << "kde-plasma-desktop" << "task-xfce-desktop" << "xfce4"
                        << "lxqt-core" << "lxde-core" << "task-gnome-desktop"
                        << "openbox" << "i3-wm" << "xserver-xorg" << "xinit";
    m_collections.append(desktops);

    Collection browsers;
    browsers.title = tr("Browsers and network");
    browsers.subtitle = tr("Getting at the rest of the world.");
    browsers.accent = Theme::teal();
    browsers.glyph = GlyphWifi;
    browsers.packages = QStringList()
                        << "firefox-esr" << "chromium" << "netsurf-gtk" << "lynx"
                        << "openssh-server" << "isc-dhcp-client" << "network-manager"
                        << "wireless-tools" << "iw" << "rfkill" << "curl" << "wget";
    m_collections.append(browsers);

    Collection media;
    media.title = tr("Media");
    media.subtitle = tr("What the Media page needs, and more of it.");
    media.accent = Theme::pink();
    media.glyph = GlyphMusic;
    media.packages = QStringList()
                     << "ffmpeg" << "alsa-utils" << "mpv" << "vlc" << "mpg123"
                     << "sox" << "libqt5gui5" << "qt5-image-formats-plugins"
                     << "fonts-dejavu-core";
    m_collections.append(media);

    Collection tools;
    tools.title = tr("Command line");
    tools.subtitle = tr("The things a bring-up console is missing.");
    tools.accent = Theme::orange();
    tools.glyph = GlyphTerminal;
    tools.packages = QStringList()
                     << "nano" << "vim" << "htop" << "tmux" << "git" << "rsync"
                     << "usbutils" << "pciutils" << "strace" << "gdb" << "file"
                     << "less" << "bash-completion" << "man-db";
    m_collections.append(tools);

    Collection dev;
    dev.title = tr("Building things");
    dev.subtitle = tr("A toolchain, on the device itself.");
    dev.accent = Theme::purple();
    dev.glyph = GlyphChip;
    dev.packages = QStringList()
                   << "build-essential" << "python3" << "python3-pip" << "cmake"
                   << "pkg-config" << "qtbase5-dev" << "libsdl2-dev" << "device-tree-compiler";
    m_collections.append(dev);

    Collection games;
    games.title = tr("Games and emulators");
    games.subtitle = tr("Anything that draws without a GPU has a chance here.");
    games.accent = Theme::green();
    games.glyph = GlyphGames;
    games.packages = QStringList()
                     << "chocolate-doom" << "prboom-plus" << "freedoom" << "nethack-console"
                     << "moon-buggy" << "bsdgames" << "mednafen" << "scummvm";
    m_collections.append(games);
}

/* ── views ───────────────────────────────────────────────────────────────── */

void PackagesPage::showHome()
{
    m_view = ViewHome;
    m_collection = -1;
    m_shown.clear();
    rebuild();
    emit titleChanged();
}

void PackagesPage::showCollection(int index)
{
    if (index < 0 || index >= m_collections.size())
        return;

    m_view = ViewCollection;
    m_collection = index;
    m_shown.clear();

    /*
     * Only the packages the archive actually has.  apt-cache show over the whole
     * collection at once is one process instead of a dozen, and the ones that come
     * back are exactly the ones that exist -- a name that is not in the archive is
     * simply absent from the output, which is the availability test as well.
     */
    const QStringList &names = m_collections[index].packages;
    QStringList args;
    args << "show";
    args += names;
    const QString out = run(aptCache(), args, 20000);

    QString current;
    QString summary;
    for (const QString &line : out.split('\n')) {
        if (line.startsWith("Package:")) {
            if (!current.isEmpty()) {
                Pkg pkg;
                pkg.name = current;
                pkg.summary = summary;
                pkg.installed = isInstalled(current);
                m_shown.append(pkg);
            }
            current = line.section(':', 1).trimmed();
            summary.clear();
        } else if (summary.isEmpty()
                   && (line.startsWith("Description-en:") || line.startsWith("Description:"))) {
            summary = line.section(':', 1).trimmed();
        }
    }
    if (!current.isEmpty()) {
        Pkg pkg;
        pkg.name = current;
        pkg.summary = summary;
        pkg.installed = isInstalled(current);
        m_shown.append(pkg);
    }

    /* apt-cache show emits one stanza per version, so the same package can arrive
     * twice.  Keep the first. */
    QVector<Pkg> unique;
    QSet<QString> seen;
    for (int i = 0; i < m_shown.size(); ++i) {
        if (seen.contains(m_shown[i].name))
            continue;
        seen.insert(m_shown[i].name);
        unique.append(m_shown[i]);
    }

    /* Back into the order the collection asked for: apt-cache answers in its own. */
    QVector<Pkg> ordered;
    for (const QString &want : names)
        for (int i = 0; i < unique.size(); ++i)
            if (unique[i].name == want)
                ordered.append(unique[i]);
    m_shown = ordered;

    if (m_shown.isEmpty())
        m_note = tr("apt has no package lists yet -- run Update first");
    else
        m_note.clear();

    rebuild();
    emit titleChanged();
}

void PackagesPage::showSearch(const QString &term)
{
    m_view = ViewSearch;
    m_term = term;
    m_shown.clear();

    if (term.trimmed().isEmpty()) {
        showHome();
        return;
    }

    /* --names-only, because a full-text search of every description in the archive
     * on this CPU takes long enough to look like a hang. */
    const QString out = run(aptCache(), QStringList() << "search" << "--names-only" << term,
                            20000);
    for (const QString &line : out.split('\n')) {
        const int dash = line.indexOf(" - ");
        if (dash <= 0)
            continue;
        Pkg pkg;
        pkg.name = line.left(dash).trimmed();
        pkg.summary = line.mid(dash + 3).trimmed();
        pkg.installed = isInstalled(pkg.name);
        m_shown.append(pkg);
        if (m_shown.size() >= kMaxResults)
            break;
    }

    if (m_shown.isEmpty())
        m_note = tr("nothing matched %1").arg(term);
    else if (m_shown.size() >= kMaxResults)
        m_note = tr("first %1 matches -- narrow the search for the rest").arg(kMaxResults);
    else
        m_note = tr("%1 matches").arg(m_shown.size());

    rebuild();
    emit titleChanged();
}

void PackagesPage::rebuild()
{
    QVector<ListRow> rows;

    if (m_view == ViewHome) {
        ListRow head;
        head.kind = ListRow::Header;
        head.text = tr("Find");
        rows.append(head);

        ListRow search;
        search.kind = ListRow::Action;
        search.text = tr("Search the archive");
        search.detail = tr("By name.  Menu opens the keyboard anywhere on this page.");
        search.glyph = GlyphPackage;
        search.accent = Theme::blue();
        search.id = RowSearch;
        rows.append(search);

        ListRow collectionsHead;
        collectionsHead.kind = ListRow::Header;
        collectionsHead.text = tr("Collections");
        rows.append(collectionsHead);

        for (int i = 0; i < m_collections.size(); ++i) {
            ListRow r;
            r.kind = ListRow::Item;
            r.text = m_collections[i].title;
            r.detail = m_collections[i].subtitle;
            r.glyph = m_collections[i].glyph;
            r.accent = m_collections[i].accent;
            r.id = RowCollection;
            r.value = i;
            r.key = m_collections[i].title;
            rows.append(r);
        }

        ListRow maintenance;
        maintenance.kind = ListRow::Header;
        maintenance.text = tr("Maintenance");
        rows.append(maintenance);

        ListRow update;
        update.kind = ListRow::Action;
        update.text = tr("Update package lists");
        update.detail = tr("apt-get update, in the Terminal.");
        update.glyph = GlyphPackage;
        update.accent = Theme::teal();
        update.id = RowUpdate;
        rows.append(update);

        ListRow upgrade;
        upgrade.kind = ListRow::Action;
        upgrade.text = tr("Upgrade everything installed");
        upgrade.detail = tr("apt-get upgrade.  Answer its questions in the Terminal.");
        upgrade.glyph = GlyphPackage;
        upgrade.accent = Theme::orange();
        upgrade.id = RowUpgrade;
        rows.append(upgrade);

        ListRow clean;
        clean.kind = ListRow::Action;
        clean.text = tr("Free up space");
        clean.detail = tr("apt-get clean and autoremove.  The card is small.");
        clean.glyph = GlyphPackage;
        clean.accent = Theme::ink2();
        clean.id = RowClean;
        rows.append(clean);

        ListRow installed;
        installed.kind = ListRow::Item;
        installed.text = m_installed.size() == 1
                             ? tr("1 package installed")
                             : tr("%1 packages installed").arg(m_installed.size());
        installed.detail = tr("What dpkg says is on this card right now.");
        installed.glyph = GlyphInfo;
        installed.accent = Theme::ink3();
        installed.enabled = false;
        installed.id = RowInstalledList;
        rows.append(installed);
    } else {
        ListRow back;
        back.kind = ListRow::Action;
        back.text = tr("Back");
        back.glyph = GlyphBack;
        back.accent = Theme::ink3();
        back.id = RowBack;
        rows.append(back);

        ListRow head;
        head.kind = ListRow::Header;
        head.text = (m_view == ViewSearch) ? tr("Matches for %1").arg(m_term)
                                           : m_collections[m_collection].title;
        rows.append(head);

        for (int i = 0; i < m_shown.size(); ++i) {
            ListRow r;
            r.kind = ListRow::Item;
            r.text = m_shown[i].name;
            r.detail = m_shown[i].summary;
            r.glyph = GlyphPackage;
            r.id = RowPackage;
            r.value = i;
            r.key = m_shown[i].name;
            if (m_shown[i].installed) {
                r.badge = tr("installed");
                r.badgeColour = Theme::green();
                r.accent = Theme::green();
            } else {
                r.accent = Theme::blue();
            }
            rows.append(r);
        }
    }

    const int keep = m_list->current();
    m_list->setRows(rows);
    if (keep > 0 && keep < rows.size())
        m_list->setCurrent(keep);
    update();
}

/* ── acting ──────────────────────────────────────────────────────────────── */

void PackagesPage::install(const QString &name)
{
    /*
     * DEBIAN_FRONTEND is left alone on purpose.  The whole reason this runs in the
     * Terminal is so debconf can ask its questions -- setting it to noninteractive
     * would answer them with defaults, and one of those defaults is which display
     * manager takes over the console this dashboard is drawn on.
     */
    emit terminalRequested(privileged("apt-get install " + name));
    emit toastRequested(tr("Installing %1 in the Terminal").arg(name), 3000);
}

void PackagesPage::remove(const QString &name)
{
    emit terminalRequested(privileged("apt-get remove " + name));
    emit toastRequested(tr("Removing %1 in the Terminal").arg(name), 3000);
}

void PackagesPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;
    const ListRow &row = rows[index];

    switch (row.id) {
    case RowSearch:
        m_awaitingSearch = true;
        emit textRequested(tr("Search packages"), m_term, false);
        return;
    case RowCollection:
        m_list->setCurrent(0);
        showCollection(row.value);
        return;
    case RowBack:
        m_list->setCurrent(0);
        showHome();
        return;
    case RowUpdate:
        emit terminalRequested(privileged(QStringLiteral("apt-get update")));
        return;
    case RowUpgrade:
        emit terminalRequested(privileged(QStringLiteral("apt-get upgrade")));
        return;
    case RowClean:
        emit terminalRequested(privileged(QStringLiteral("apt-get clean && apt-get -y autoremove")));
        return;
    case RowPackage: {
        if (row.value < 0 || row.value >= m_shown.size())
            return;
        const Pkg &pkg = m_shown[row.value];

        /*
         * Two presses, and the toast says which way it will go.  Removing a package
         * on a Debian system can take a desktop with it, and there is no undo on a
         * device whose only network may be the one this page is about to change.
         */
        if (m_armed != pkg.name) {
            m_armed = pkg.name;
            emit toastRequested(pkg.installed
                                    ? tr("Press A again to remove %1").arg(pkg.name)
                                    : tr("Press A again to install %1").arg(pkg.name),
                                5000);
            return;
        }
        m_armed.clear();
        if (pkg.installed)
            remove(pkg.name);
        else
            install(pkg.name);
        return;
    }
    default:
        return;
    }
}

void PackagesPage::textEntered(const QString &text, bool accepted)
{
    if (!m_awaitingSearch)
        return;
    m_awaitingSearch = false;
    if (!accepted)
        return;
    m_list->setCurrent(0);
    showSearch(text.trimmed());
}

bool PackagesPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:   m_list->step(-1); m_armed.clear(); return true;
    case Joypad::NavDown: m_list->step(1); m_armed.clear(); return true;
    case Joypad::NavOk:   m_list->press(); return true;
    case Joypad::NavMenu:
        m_awaitingSearch = true;
        emit textRequested(tr("Search packages"), m_term, false);
        return true;
    case Joypad::NavBack:
        if (m_view != ViewHome) {
            m_list->setCurrent(0);
            showHome();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void PackagesPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);

    QString right;
    if (aptCache().isEmpty())
        right = tr("apt-cache missing");
    else
        right = QString("%1 installed").arg(m_installed.size());

    const QRectF body = paintSheet(p, card, title(), right);

    QString line = m_note;
    if (line.isEmpty()) {
        if (m_view == ViewHome)
            line = tr("A opens, Menu searches.  Installs run in the Terminal.");
        else
            line = tr("A installs or removes -- twice, to be sure.");
    }
    p.setFont(Theme::font(12));
    p.setPen(Theme::ink2());
    p.drawText(QRectF(body.x() + 12, body.y() + 2, body.width() - 24, 18),
               Qt::AlignLeft | Qt::AlignVCenter, line);
}
