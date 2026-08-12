/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "dashboard.h"

#include "diagnostics.h"
#include "joypad.h"
#include "keyboard.h"
#include "media.h"
#include "packages.h"
#include "pointer.h"
#include "settingspage.h"
#include "strings.h"
#include "terminal.h"
#include "theme.h"
#include "trace.h"
#include "wifi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/*
 * Quote a path for /bin/sh.  Inside single quotes everything is literal except a
 * single quote, which has to leave the quoting to be written: ' -> '\''.  Paths
 * on an SD card come from whoever wrote the card, and a filename with a quote or
 * a space in it is not an attack, it is Tuesday.
 */
QString shellQuote(const QString &s)
{
    QString out = s;
    out.replace('\'', "'\\''");
    return "'" + out + "'";
}

} /* namespace */

/* ── FilesPage ───────────────────────────────────────────────────────────── */

/*
 * ANNOUNCED STEP BY STEP, and here more than anywhere else in this file.  This page
 * is the only one that hands a Qt class a path off the SD card and lets it go and
 * look: QFileSystemModel does its work on a thread of its own (QFileInfoGatherer),
 * so a throw inside it aborts the process while the main thread is still building
 * widgets, and the last thing printed is the only evidence of where that was.  Every
 * other page is arithmetic and QPainter calls.
 */
FilesPage::FilesPage(QWidget *parent)
    : PageWidget(parent)
{
    /*
     * /run/j36/card first: that is the card's data partition, mounted read-only by
     * the initramfs because there is no keyboard on this board and no other way to
     * reach it.  It is also the only directory here whose contents the operator put
     * there, which makes it the useful place to open on.  The home directories are
     * the fallback for a boot without that mount, and / for a rootfs with neither.
     */
    Trace::step("FilesPage: choosing a base directory");
    m_base = QFileInfo::exists("/run/j36/card") ? QString("/run/j36/card")
           : QFileInfo::exists("/home/ark")     ? QString("/home/ark")
           : QFileInfo::exists("/root")         ? QString("/root")
                                                : QString("/");

    Trace::step("FilesPage: QFileSystemModel");
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);

    /* Starts the gatherer thread walking the card. */
    Trace::step("FilesPage: setRootPath -- starts the gatherer thread on the card");
    m_model->setRootPath(m_base);

    Trace::step("FilesPage: QListView");
    m_view = new QListView(this);
    m_view->setModel(m_model);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setUniformItemSizes(true);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    m_view->viewport()->setAutoFillBackground(false);

    /*
     * :!active is not optional.  The dashboard drives this view from evdev rather
     * than through Qt's focus, so the window may never be "active" as Qt counts it,
     * and without that selector the selection is drawn in the inactive palette --
     * grey on grey, which reads as nothing being selected at all.
     */
    Trace::step("FilesPage: stylesheet");
    m_view->setStyleSheet(
        "QListView { background: transparent; border: none; color: #E8EAF2;"
        "            font-size: 13px; outline: none; }"
        "QListView::item { height: 24px; padding-left: 6px; border-radius: 6px; }"
        "QListView::item:selected, QListView::item:selected:!active {"
        "            background: #0A84FF; color: #FFFFFF; }"
        "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #5C606C; border-radius: 3px;"
        "            min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "            background: transparent; }");

    Trace::step("FilesPage: layout");
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(Theme::Margin + 12, Theme::Margin + 42,
                               Theme::Margin + 12, Theme::Margin + 10);
    layout->addWidget(m_view);

    /*
     * QFileSystemModel populates on a worker thread, so the row count is zero for
     * a moment after every setRootPath and selecting row 0 straight away selects
     * nothing.  This puts the cursor on the first entry as soon as the directory
     * actually arrives.
     */
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) != m_root)
            return;
        if (!m_view->currentIndex().isValid()) {
            const QModelIndex root = m_model->index(m_root);
            if (m_model->rowCount(root) > 0)
                m_view->setCurrentIndex(m_model->index(0, 0, root));
        }
        update();
    });

    Trace::step("FilesPage: setRoot -- reads the directory");
    setRoot(m_base);
}

QString FilesPage::title() const
{
    return m_root.isEmpty() ? tr("Files") : m_root;
}

void FilesPage::setRoot(const QString &path)
{
    m_root = QDir::cleanPath(path);
    m_model->setRootPath(m_root);
    const QModelIndex root = m_model->index(m_root);
    m_view->setRootIndex(root);
    m_view->setCurrentIndex(m_model->index(0, 0, root));
    emit titleChanged();
    update();
}

void FilesPage::step(int delta)
{
    const QModelIndex root = m_model->index(m_root);
    const int rows = m_model->rowCount(root);
    if (rows <= 0)
        return;

    const QModelIndex current = m_view->currentIndex();
    int row = current.isValid() ? current.row() + delta : 0;
    row = qBound(0, row, rows - 1);
    const QModelIndex next = m_model->index(row, 0, root);
    m_view->setCurrentIndex(next);
    m_view->scrollTo(next);
}

void FilesPage::enter()
{
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    const QString path = m_model->filePath(current);
    if (m_model->isDir(current))
        setRoot(path);
    else
        emit openRequested(path);
}

bool FilesPage::leave()
{
    if (m_root == "/")
        return false;
    QDir dir(m_root);
    if (!dir.cdUp())
        return false;
    const QString child = m_root;
    setRoot(dir.absolutePath());
    /* Come back to the directory we just left, not to the top of its parent. */
    const QModelIndex idx = m_model->index(child);
    if (idx.isValid()) {
        m_view->setCurrentIndex(idx);
        m_view->scrollTo(idx);
    }
    return true;
}

bool FilesPage::handleNav(int action)
{
    switch (action) {
    case Joypad::NavUp:
        step(-1);
        return true;
    case Joypad::NavDown:
        step(1);
        return true;
    case Joypad::NavRight:
    case Joypad::NavOk:
        enter();
        return true;
    case Joypad::NavLeft:
    case Joypad::NavBack:
        /* False at the top of the tree: the shell takes that as "pop me". */
        return leave();
    default:
        return false;
    }
}

void FilesPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    Theme::softShadow(p, card, Theme::Radius, 6, 24);
    Theme::vgrad(p, card, Theme::window(), Theme::window().darker(112), Theme::Radius);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), Theme::Radius, Theme::Radius);

    QPainterPath clip;
    clip.addRoundedRect(card, Theme::Radius, Theme::Radius);
    p.save();
    p.setClipPath(clip);
    const QRectF head(card.x(), card.y(), card.width(), 34);
    Theme::vgrad(p, head, Theme::titlebar(), Theme::titlebarLow());
    p.setPen(QPen(Theme::separator(), 1.0));
    p.drawLine(QPointF(head.x(), head.bottom() - 0.5), QPointF(head.right(), head.bottom() - 0.5));
    p.restore();

    const QFont f = Theme::font(13, true);
    const QFontMetrics fm(f);
    p.setFont(f);
    p.setPen(Theme::ink());
    const QRectF text = head.adjusted(14, 0, -14, 0);
    p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(m_root, Qt::ElideMiddle, (int)text.width()));
}

/* ── Dashboard ───────────────────────────────────────────────────────────── */

Dashboard::Dashboard(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("MixOS");
    /* Opaque, because this is the wallpaper and there is nothing behind it: Qt can
     * skip clearing the backing store before every paint. */
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    /*
     * WHY EVERY LINE OF THIS CONSTRUCTOR IS ANNOUNCED.  It is one statement at the
     * call site -- `Dashboard dash;' -- and twenty objects here, and when it aborted
     * the console named the statement, which named all twenty at once and therefore
     * none of them.  A step costs one store and one line of console; it buys the name
     * of the object that was being built when the process died.
     */
    Trace::step("StatusBar");
    m_bar = new StatusBar(this);
    Trace::step("CardGrid (apps)");
    m_apps = new CardGrid(this);
    m_apps->setPageTitle(tr("Apps"));
    Trace::step("MediaPage");
    m_media = new MediaPage(this);
    Trace::step("SettingsPage");
    m_settings = new SettingsPage(this);
    Trace::step("CardGrid (power)");
    m_power = new CardGrid(this);
    m_power->setPageTitle(tr("Power"));

    Trace::step("FilesPage");
    m_files = new FilesPage(this);
    Trace::step("TerminalPage");
    m_terminal = new TerminalPage(this);
    Trace::step("WifiPage");
    m_wifi = new WifiPage(this);
    Trace::step("PackagesPage");
    m_packages = new PackagesPage(this);
    Trace::step("MousePage");
    m_mouse = new MousePage(this);
    Trace::step("DisplayPage");
    m_display = new DisplayPage(this);
    Trace::step("LanguagePage");
    m_language = new LanguagePage(this);
    Trace::step("InfoPage");
    m_info = new InfoPage(this);

    /*
     * The one thing built here that reaches the hardware before anybody asks for
     * it.  j36_mt6592_backlight adopts the duty the MVII loader left in the BLS
     * block rather than choosing a level, and the loader always hands over at
     * full -- so a brightness the user set is applied by nothing at all unless
     * the shell applies it, and the setting would appear to reset on every boot.
     *
     * Safe to do before the first frame: only a level this dashboard itself wrote
     * is ever stored, the slider that wrote it cannot go below 5 per cent, and
     * Settings clamps the same floor again on the way in.
     */
    Trace::step("backlight/restore");
    DisplayPage::restoreSaved();

    Trace::step("Dock");
    m_dock = new Dock(this);

    Trace::step("toast label and its timer");
    m_toast = new QLabel(this);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setStyleSheet(
        "QLabel { background: rgba(8, 9, 14, 216); color: #E8EAF2;"
        "         border: 1px solid #4A4E5C; border-radius: 12px;"
        "         padding: 7px 16px; font-size: 13px; }");
    m_toast->hide();

    m_toastTimer = new QTimer(this);
    m_toastTimer->setSingleShot(true);
    connect(m_toastTimer, &QTimer::timeout, this, [this]() {
        m_toast->hide();
        /* An armed Restart or Power off expires with its own prompt.  Anything else
         * would leave a button that shuts the board down on the next press, minutes
         * later, with no warning on screen. */
        m_armed = InternalNone;
        m_armedExe.clear();
    });

    Trace::step("Keyboard overlay");
    m_keyboard = new Keyboard(this);
    connect(m_keyboard, &Keyboard::finished, this, &Dashboard::onKeyboardFinished);

    /*
     * The pad comes before the Diagnostics page because that page reports on it --
     * how many devices are open, how many of them look like a mouse -- and holding
     * the pointer to a Joypad that does not exist yet would be a null on the first
     * probe.
     */
    Trace::step("Joypad -- opens /dev/input/event*");
    m_pad = new Joypad(this);

    Trace::step("DiagnosticsPage");
    m_diagnostics = new DiagnosticsPage(m_pad, this);

    Trace::step("Pointer");
    m_pointer = new Pointer(this);

    Trace::step("page tables");
    m_roots << m_apps << m_media << m_settings << m_power;
    m_all << m_apps << m_media << m_settings << m_power
          << m_files << m_terminal << m_wifi << m_packages
          << m_diagnostics << m_mouse << m_display << m_language << m_info;
    for (PageWidget *page : m_all) {
        adopt(page);
        page->hide();
    }

    Trace::step("connections");
    connect(m_apps, &CardGrid::activated, this, &Dashboard::onAppActivated);
    connect(m_power, &CardGrid::activated, this, &Dashboard::onPowerActivated);
    connect(m_apps, &CardGrid::indexChanged, this, [this](int) {
        if (m_current == m_apps)
            m_bar->setTitle(m_apps->title());
    });
    connect(m_power, &CardGrid::indexChanged, this, [this](int) {
        if (m_current == m_power)
            m_bar->setTitle(m_power->title());
    });
    connect(m_files, &FilesPage::openRequested, this, &Dashboard::onOpenRequested);
    connect(m_settings, &SettingsPage::openRequested, this, &Dashboard::onSettingsOpen);
    connect(m_packages, &PackagesPage::terminalRequested,
            this, &Dashboard::onTerminalRequested);
    connect(m_diagnostics, &DiagnosticsPage::launchRequested,
            this, &Dashboard::onLaunchRequested);
    connect(m_dock, &Dock::pageClicked, this, &Dashboard::setRoot);
    connect(&Strings::instance(), &Strings::languageChanged,
            this, &Dashboard::retranslate);

    connect(m_pad, &Joypad::nav, this, &Dashboard::onNav);
    connect(m_pad, &Joypad::key, this, &Dashboard::onKey);
    connect(m_pad, &Joypad::pointerMove, m_pointer, &Pointer::onMove);
    connect(m_pad, &Joypad::pointerButton, m_pointer, &Pointer::onButton);
    connect(m_pad, &Joypad::pointerWheel, m_pointer, &Pointer::onWheel);

    Trace::step("dock pages");
    m_dock->setPages(QStringList() << tr("Apps") << tr("Media")
                                   << tr("Settings") << tr("Power"));

    /* Stats every candidate executable and IWAD on the card. */
    Trace::step("buildPages -- looks for the apps on disk");
    buildPages();

    m_info->setInputSummary(
        m_pad->deviceCount() == 0
            ? tr("no /dev/input/event* -- nothing to navigate with")
            : QString("%1: %2").arg(m_pad->deviceCount()).arg(m_pad->deviceNames().join(", ")));

    Trace::step("setRoot(0)");
    setRoot(0);
    qApp->installEventFilter(this);
    Trace::step("constructed");
}

/*
 * Every page gets the same four wires.  Written once here rather than four times
 * per page at the call site, so a page added next month is connected to the toast
 * and the keyboard by the act of being put in m_all.
 */
void Dashboard::adopt(PageWidget *page)
{
    connect(page, &PageWidget::toastRequested, this, &Dashboard::onToastRequested);
    connect(page, &PageWidget::closeRequested, this, &Dashboard::onCloseRequested);
    connect(page, &PageWidget::titleChanged, this, &Dashboard::onTitleChanged);
    connect(page, &PageWidget::textRequested, this, &Dashboard::onTextRequested);
}

QString Dashboard::firstExisting(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

/*
 * doomgeneric identifies an IWAD by its filename before it opens it -- d_iwad.c
 * matches against a fixed iwads[] table -- so this looks for those names and not
 * for *.wad.  The order is the order that table has them in.
 */
QString Dashboard::firstWad()
{
    const QStringList names = QStringList()
        << "freedoom1.wad" << "freedoom2.wad" << "freedm.wad"
        << "doom.wad" << "doom1.wad" << "doom2.wad"
        << "plutonia.wad" << "tnt.wad" << "chex.wad" << "hacx.wad";
    /*
     * NAMED, and it has to be.  Written inline --
     *
     *     for (const QString &dir : QStringList() << "/opt/mixos/share/doom" << ...)
     *
     * -- this loop iterates freed memory, and it is the bad_alloc that killed the
     * dashboard in phase "Dashboard -- four pages, the dock and the evdev map".
     * QStringList::operator<< returns a REFERENCE to the temporary, so what the
     * range-for binds is `auto &&__range = <QStringList&>' -- a reference
     * initialised from another reference.  The lifetime-extension rule only fires
     * when the temporary is bound to the reference DIRECTLY, so it does not fire
     * here: the QStringList is destroyed at the semicolon, before the first
     * iteration, and begin()/end() then walk a released QArrayData.  Constructing
     * a QString from that garbage asks qMallocAligned for whatever the freed
     * header now says the length is, and Q_CHECK_PTR turns the null into a
     * std::bad_alloc thrown from inside libQt5Core -- no size, no frame, no
     * operator new involved, which is exactly the console this board printed.
     *
     * g++ said so, in the only terms it has for a dead object: two
     * `-Wuninitialized' hits on this line, in a build that otherwise had none.
     *
     * A named list is bound directly and lives to the end of the function.  C++23
     * extends the inline form's lifetime too (P2718R0), but this is built as
     * gnu++17.
     */
    const QStringList dirs = QStringList()
        << "/opt/mixos/share/doom" << "/roms/doom";
    for (const QString &dir : dirs)
        for (const QString &n : names)
            if (QFileInfo(dir + "/" + n).isFile())
                return dir + "/" + n;
    return QString();
}

/*
 * WHAT IS NOT ON THE APPS GRID, AND WHY.
 *
 * Media, Settings and Power each used to have a card here as well as a slot in
 * the dock, and pressing the card did nothing but setRoot() to the tab that was
 * already one shoulder press away.  Two ways to reach the same page is not two
 * features; it is one feature and one piece of furniture the user has to learn is
 * furniture.  The dock is the way to a root page.  The grid is the way to the
 * pages that have no other way in -- which is exactly the six below.
 */
void Dashboard::buildPages()
{
    QVector<AppEntry> apps;

    /*
     * Doom, and it is first because it is the one thing on this card already proved
     * to put a moving picture on this panel: doomgeneric writes 32-bit pixels into
     * /dev/fb0 and reads the pad through evdev, which is exactly the layer this
     * dashboard draws in.  No EGL, no GBM, no mode set -- so it hands the panel back
     * when it exits, which nothing that sets a mode on this board does.
     */
    AppEntry doom;
    doom.title = tr("Doom");
    doom.accent = Theme::blue();
    doom.glyph = GlyphGames;
    doom.exe = firstExisting(QStringList() << "/opt/mixos/bin/doom");
    const QString wad = firstWad();
    if (!doom.exe.isEmpty() && !wad.isEmpty())
        doom.args = QStringList() << "-iwad" << wad;
    doom.available = !doom.exe.isEmpty() && !wad.isEmpty();
    /* Said when the card is pressed, not printed under it: see AppEntry. */
    if (doom.exe.isEmpty())
        doom.reason = tr("Not installed. Build with J36_DOOM=1 to put it in /opt/mixos.");
    else if (wad.isEmpty())
        doom.reason = tr("No IWAD in /opt/mixos/share/doom.");
    apps.append(doom);

    AppEntry terminal;
    terminal.title = tr("Terminal");
    terminal.accent = Theme::green();
    terminal.glyph = GlyphTerminal;
    terminal.internal = InternalTerminal;
    apps.append(terminal);

    AppEntry files;
    files.title = tr("Files");
    files.accent = Theme::teal();
    files.glyph = GlyphFiles;
    files.internal = InternalFiles;
    apps.append(files);

    AppEntry packages;
    packages.title = tr("Packages");
    packages.accent = Theme::yellow();
    packages.glyph = GlyphPackage;
    packages.internal = InternalPackages;
    apps.append(packages);

    AppEntry wifi;
    wifi.title = tr("Wi-Fi");
    wifi.accent = Theme::blue();
    wifi.glyph = GlyphWifi;
    wifi.internal = InternalWifi;
    apps.append(wifi);

    /*
     * What the "3D cube" card became.  The cube is still in there and still turns;
     * it is rasterised by QPainter now, because eglprobe's GLES2 cube cannot be
     * SHOWN on this board -- lima has no CRTC and mtk_drm is not loaded, so there is
     * nothing to flip a rendered buffer onto.  The page says so, with the evidence.
     */
    AppEntry diag;
    diag.title = tr("Diagnostics");
    diag.accent = Theme::purple();
    diag.glyph = GlyphChip;
    diag.internal = InternalDiagnostics;
    apps.append(diag);

    m_apps->setEntries(apps);

    QVector<AppEntry> powers;

    AppEntry restart;
    restart.title = tr("Restart");
    restart.accent = Theme::orange();
    restart.glyph = GlyphPower;
    restart.internal = InternalReboot;
    powers.append(restart);

    AppEntry off;
    off.title = tr("Power off");
    off.accent = Theme::red();
    off.glyph = GlyphPower;
    off.internal = InternalPoweroff;
    powers.append(off);

    AppEntry console;
    console.title = tr("Console");
    console.accent = Theme::teal();
    console.glyph = GlyphTerminal;
    console.internal = InternalConsole;
    powers.append(console);

    AppEntry info;
    info.title = tr("System");
    info.accent = Theme::ink3();
    info.glyph = GlyphInfo;
    info.internal = InternalInfo;
    powers.append(info);

    m_power->setEntries(powers);
}

/*
 * A language was picked.  Everything that is rebuilt on the way into a page
 * retranslates itself the next time that page is entered -- which for a page you
 * had to walk to the Language list from is immediately, on the way back.  What is
 * left is the furniture: the two card grids, built once in the constructor, and
 * the dock, which never leaves the glass.
 */
void Dashboard::retranslate()
{
    const int apps = m_apps->index();
    const int power = m_power->index();

    m_apps->setPageTitle(tr("Apps"));
    m_power->setPageTitle(tr("Power"));
    m_dock->setPages(QStringList() << tr("Apps") << tr("Media")
                                   << tr("Settings") << tr("Power"));
    buildPages();

    /* setEntries resets the selection; putting it back is what keeps a language
     * change from also being a jump to the first card. */
    m_apps->setIndex(apps);
    m_power->setIndex(power);

    applyChrome();
    update();
}

/* ── the page stack ──────────────────────────────────────────────────────── */

PageWidget *Dashboard::current() const
{
    if (!m_stack.isEmpty())
        return m_stack.last();
    if (m_page >= 0 && m_page < m_roots.size())
        return m_roots[m_page];
    return nullptr;
}

/*
 * The one place a page becomes visible or stops being.  onLeave() and onEnter()
 * are what stop a Wi-Fi scan, a video decode and a 60 Hz cube from running behind
 * a page nobody is looking at, so they have to be paired here rather than at the
 * six call sites that change pages.
 */
void Dashboard::showPage(PageWidget *page)
{
    if (m_current == page) {
        applyChrome();
        return;
    }

    if (m_current) {
        m_current->onLeave();
        m_current->hide();
    }
    m_current = page;
    if (m_current) {
        /* Geometry before onEnter: a page that measures itself while populating
         * -- ListPane's scroll clamp, the Terminal's column count -- has to be
         * asked at the size it is about to be shown at. */
        applyChrome();
        m_current->onEnter();
        m_current->show();
    }
    applyChrome();
}

void Dashboard::setRoot(int page)
{
    m_page = qBound(0, page, m_roots.size() - 1);
    /*
     * Switching root pages empties the stack.  The alternative -- a stack per root
     * -- means the shoulder buttons take you somewhere different depending on
     * where you have been, which is not something a dock can show.
     */
    m_stack.clear();
    showPage(m_roots[m_page]);
}

void Dashboard::push(PageWidget *page)
{
    if (!page || current() == page)
        return;
    /* A page can only be on the stack once; pushing one that is already there
     * unwinds back to it instead of stacking a second copy that Back would have
     * to be pressed twice for. */
    const int at = m_stack.indexOf(page);
    if (at >= 0)
        m_stack.resize(at + 1);
    else
        m_stack.append(page);
    showPage(m_stack.last());
}

void Dashboard::pop()
{
    if (m_stack.isEmpty())
        return;
    m_stack.removeLast();
    showPage(current());
}

void Dashboard::applyChrome()
{
    PageWidget *page = current();
    const bool full = page && page->wantsFullscreen();

    m_bar->setVisible(!full);
    m_dock->setVisible(!full);

    const QRect normal(0, Theme::StatusH, width(),
                       qMax(0, height() - Theme::StatusH - Theme::DockH));
    if (page)
        page->setGeometry(full ? rect() : normal);

    m_dock->setCurrent(m_page);
    if (page)
        m_bar->setTitle(page->title());

    /*
     * A page that owns every pixel is showing something -- a picture, a video, a
     * shell -- that an arrow sitting in the middle of would be on top of.  It comes
     * straight back the moment the stick moves.
     */
    if (full)
        m_pointer->sleep();

    if (m_keyboard->isVisible())
        m_keyboard->raise();
    if (m_toast->isVisible())
        m_toast->raise();
    m_pointer->raise();

    syncInputMode();
    update();
}

/*
 * Text mode splits the input devices in two: anything that looks like a keyboard
 * stops producing nav actions and produces raw key codes instead, while the pad
 * keeps producing nav actions.  That is what lets B leave the Terminal while a
 * USB keyboard is typing into it.
 */
void Dashboard::syncInputMode()
{
    const bool wantKeys = m_keyboard->isVisible()
                          || (current() && current()->wantsKeys());
    m_pad->setTextMode(wantKeys);
}

void Dashboard::resizeEvent(QResizeEvent *event)
{
    m_bar->setGeometry(0, 0, width(), Theme::StatusH);
    m_dock->setGeometry(0, height() - Theme::DockH, width(), Theme::DockH);

    const QRect normal(0, Theme::StatusH, width(),
                       qMax(0, height() - Theme::StatusH - Theme::DockH));
    for (PageWidget *page : m_all)
        page->setGeometry(normal);

    /* The keyboard is an overlay over the bottom of the screen rather than a page:
     * what is being typed into stays visible above it. */
    const int kb = qMin(300, height());
    m_keyboard->setGeometry(0, height() - kb, width(), kb);

    applyChrome();
    QWidget::resizeEvent(event);
}

/* ── the pages talking back ──────────────────────────────────────────────── */

void Dashboard::onToastRequested(const QString &text, int ms)
{
    toast(text, ms > 0 ? ms : 2400);
}

void Dashboard::onCloseRequested()
{
    if (!m_stack.isEmpty())
        pop();
    else if (m_page != 0)
        setRoot(0);
}

void Dashboard::onTitleChanged()
{
    /* Only the page on screen may retitle the bar -- a Wi-Fi scan finishing behind
     * a Terminal must not rename the Terminal. */
    if (sender() != current())
        return;
    applyChrome();
}

void Dashboard::onTextRequested(const QString &prompt, const QString &initial, bool password)
{
    m_textTarget = qobject_cast<PageWidget *>(sender());
    m_keyboard->open(prompt, initial, password);
    m_keyboard->raise();
    m_pointer->raise();
    syncInputMode();
}

void Dashboard::onKeyboardFinished(const QString &text, bool accepted)
{
    PageWidget *target = m_textTarget.data();
    m_textTarget.clear();
    syncInputMode();
    if (target)
        target->textEntered(text, accepted);
    update();
}

void Dashboard::onSettingsOpen(int destination)
{
    openDestination(destination);
}

/*
 * The Packages page hands installs to the Terminal rather than running them
 * itself: apt takes minutes, asks questions, and prints the only progress report
 * anyone has ever needed.  See packages.h.
 */
void Dashboard::onTerminalRequested(const QString &command)
{
    push(m_terminal);
    m_terminal->runCommand(command);
}

void Dashboard::onLaunchRequested(const QString &title, const QString &exe,
                                  const QStringList &args, bool confirm)
{
    if (confirm && m_armedExe != exe) {
        m_armedExe = exe;
        toast(title + " takes the panel for good.\nPress A again to run it.", 6000);
        return;
    }
    m_armedExe.clear();
    launch(title, exe, args);
}

void Dashboard::onKey(int code, bool pressed, int modifiers)
{
    if (m_keyboard->isVisible()) {
        m_keyboard->keyPressed(code, pressed, modifiers);
        return;
    }
    PageWidget *page = current();
    if (page && page->wantsKeys())
        page->keyPressed(code, pressed, modifiers);
}

/* ── painting, toast, launching ──────────────────────────────────────────── */

void Dashboard::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    /* The wallpaper: MVII's vertical desktop gradient, and then a soft blue bloom
     * under the menu bar so the chrome has something to be translucent over. */
    Theme::vgrad(p, QRectF(rect()), Theme::desk(), Theme::deskLow());

    p.setRenderHint(QPainter::Antialiasing, true);
    QRadialGradient bloom(QPointF(width() / 2.0, Theme::StatusH), width() * 0.75);
    QColor centre = Theme::blue();
    centre.setAlpha(34);
    bloom.setColorAt(0.0, centre);
    centre.setAlpha(0);
    bloom.setColorAt(1.0, centre);
    p.setPen(Qt::NoPen);
    p.setBrush(bloom);
    p.drawRect(rect());

    /*
     * Last, and after the painter has done its work, because what main() does with
     * this is switch the console out of the mode fbcon draws in: say it only once
     * there is genuinely a frame here to replace the console with.
     */
    if (!m_firstPaint) {
        m_firstPaint = true;
        emit firstPainted();
    }
}

void Dashboard::toast(const QString &text, int ms)
{
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move(qMax(0, (width() - m_toast->width()) / 2),
                  qMax(0, height() - Theme::DockH - m_toast->height() - 6));
    m_toast->show();
    m_toast->raise();
    m_pointer->raise();
    m_toastTimer->start(ms);
}

void Dashboard::launch(const QString &title, const QString &exe, const QStringList &args)
{
    if (exe.isEmpty() || !QFileInfo(exe).isExecutable()) {
        toast(tr("%1 is not on this card").arg(title));
        return;
    }

    toast(tr("Starting %1").arg(title), 60000);
    /*
     * Painted before we block, or the toast never reaches the glass: everything
     * below this line runs with the event loop stopped.
     */
    m_toast->repaint();
    QCoreApplication::processEvents();

    /* The child gets the input devices to itself, and whatever it was pressed with
     * is discarded before the dashboard listens again. */
    m_pad->setSuspended(true);
    m_pointer->sleep();
    const int rc = QProcess::execute(exe, args);
    m_pad->setSuspended(false);

    /*
     * The child had the framebuffer, so every pixel is suspect.  A child that set a
     * mode of its own through /dev/dri/card0 has taken the scanout away from the
     * LK's simple-framebuffer as well, and no amount of repainting here brings that
     * back -- which is worth knowing when a launch comes back to a black panel.
     */
    update();
    if (m_current)
        m_current->update();

    if (rc == -2)
        toast(tr("%1 would not start").arg(title));
    else if (rc == -1)
        toast(tr("%1 crashed").arg(title));
    else if (rc != 0)
        toast(tr("%1 exited %2").arg(title).arg(rc));
    else
        toast(tr("%1 exited").arg(title));
}

/* ── activating a card ───────────────────────────────────────────────────── */

/*
 * THREE DESTINATIONS, AND THAT IS THE POINT.
 *
 * Settings used to end in a "System" section that opened Packages, Terminal,
 * Files, Diagnostics and System information, and a "Network" section that opened
 * Wi-Fi.  Every one of those is a card on the Apps grid or a tab on the dock, so
 * every one of them was a second door into a room that already had one -- and a
 * second door that had to be kept in step with the first, which is how the Wi-Fi
 * row came to describe an interface the Wi-Fi page names differently.
 *
 * What is left here is what Settings is for: the three pages that exist ONLY as
 * settings and are reachable nowhere else.
 */
void Dashboard::openDestination(int destination)
{
    switch (destination) {
    case SettingsPage::OpenMouse:
        push(m_mouse);
        break;
    case SettingsPage::OpenDisplay:
        push(m_display);
        break;
    case SettingsPage::OpenLanguage:
        push(m_language);
        break;
    default:
        break;
    }
}

void Dashboard::activate(const AppEntry &entry)
{
    /*
     * Why a card is greyed out, said when it is pressed.  This is where the
     * description under the card went: a caption is on the glass whether or not
     * anything is wrong, and this is on the glass exactly when somebody has asked.
     */
    if (!entry.available) {
        toast(entry.reason.isEmpty()
                  ? tr("%1 is not on this card").arg(entry.title)
                  : entry.reason,
              5000);
        return;
    }

    if (!entry.exe.isEmpty() || entry.internal == InternalNone) {
        /* Same two-press gate as Power off, and for the same reason: what it does
         * cannot be undone from here.  The warning has to be shown before the child
         * starts, because after it starts the panel is no longer ours to draw on. */
        if (entry.confirm && m_armedExe != entry.exe) {
            m_armedExe = entry.exe;
            toast(tr("%1 takes the panel for good.\nPress A again to run it.")
                      .arg(entry.title),
                  6000);
            return;
        }
        m_armedExe.clear();
        launch(entry.title, entry.exe, entry.args);
        return;
    }

    switch (entry.internal) {
    case InternalFiles:
        push(m_files);
        break;
    case InternalTerminal:
        push(m_terminal);
        break;
    case InternalWifi:
        push(m_wifi);
        break;
    case InternalPackages:
        push(m_packages);
        break;
    case InternalDiagnostics:
        push(m_diagnostics);
        break;
    case InternalInfo:
        m_info->refresh();
        push(m_info);
        break;
    case InternalReboot:
        if (m_armed != InternalReboot) {
            m_armed = InternalReboot;
            toast(tr("Press A again to restart"));
            return;
        }
        m_armed = InternalNone;
        launch(tr("Restart"), firstExisting(QStringList() << "/sbin/reboot" << "/usr/sbin/reboot"),
               QStringList());
        break;
    case InternalPoweroff:
        if (m_armed != InternalPoweroff) {
            m_armed = InternalPoweroff;
            toast(tr("Press A again to power off"));
            return;
        }
        m_armed = InternalNone;
        launch(tr("Power off"), firstExisting(QStringList() << "/sbin/poweroff" << "/usr/sbin/poweroff"),
               QStringList());
        break;
    case InternalConsole:
        qApp->quit();
        break;
    default:
        break;
    }
}

void Dashboard::onAppActivated(int index)
{
    const QVector<AppEntry> &entries = m_apps->entries();
    if (index >= 0 && index < entries.size())
        activate(entries[index]);
}

void Dashboard::onPowerActivated(int index)
{
    const QVector<AppEntry> &entries = m_power->entries();
    if (index >= 0 && index < entries.size())
        activate(entries[index]);
}

/*
 * A file chosen in the browser.  Media is asked first because it is the only
 * thing here that can actually SHOW a file; everything else is handed to the
 * Terminal, which at least lets the user decide what to do with it.
 */
void Dashboard::onOpenRequested(const QString &path)
{
    const QFileInfo info(path);

    if (m_media->openPath(path)) {
        setRoot(1);
        return;
    }

    static const char *kText[] = { ".txt", ".log", ".conf", ".cfg", ".ini", ".md",
                                   ".sh", ".py", ".json", ".xml", ".dts", ".c",
                                   ".h", ".cpp", ".service", ".list" };
    const QString lower = path.toLower();
    for (size_t i = 0; i < sizeof(kText) / sizeof(kText[0]); ++i) {
        if (!lower.endsWith(QLatin1String(kText[i])))
            continue;
        push(m_terminal);
        /* less, not cat: a 40 MB log poured into a terminal emulator that keeps
         * 600 lines of scrollback is 40 MB of work to show 600 lines. */
        m_terminal->runCommand("less -- " + shellQuote(path));
        return;
    }

    if (info.isFile() && info.isExecutable()) {
        push(m_terminal);
        m_terminal->runCommand(shellQuote(path));
        return;
    }

    toast(tr("Nothing here opens %1").arg(info.fileName()));
}

/* ── input ───────────────────────────────────────────────────────────────── */

void Dashboard::onNav(int action)
{
    /* The keyboard is an overlay, so it gets first refusal: Back closes it rather
     * than popping the page being typed into. */
    if (m_keyboard->isVisible()) {
        if (m_keyboard->handleNav(action))
            return;
    }

    if (action == Joypad::NavQuit) {
        toast(tr("Power, then Console, leaves the dashboard"));
        return;
    }

    PageWidget *page = current();
    if (page && page->handleNav(action))
        return;

    /* Whatever the page did not want. */
    switch (action) {
    case Joypad::NavPrevPage:
        setRoot(m_page == 0 ? m_roots.size() - 1 : m_page - 1);
        return;
    case Joypad::NavNextPage:
        setRoot(m_page == m_roots.size() - 1 ? 0 : m_page + 1);
        return;
    case Joypad::NavMenu:
        /* Menu is the settings button when the page in front has no use for it. */
        setRoot(m_current == m_settings ? 0 : 2);
        return;
    case Joypad::NavBack:
        if (!m_stack.isEmpty())
            pop();
        else if (m_page != 0)
            setRoot(0);
        return;
    default:
        break;
    }
}

bool Dashboard::eventFilter(QObject *watched, QEvent *event)
{
    /*
     * Only reached when Qt's own input handlers are running -- QT_QPA_FB_DISABLE_INPUT
     * is set on the device, so on the J36 every press arrives through Joypad instead.
     * This is what makes the same binary navigable on a development machine.
     */
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);

        /*
         * A page that wants raw keys gets them here too, so a workstation build can
         * type into the Terminal.  Qt has already mapped the scan code to a Qt key,
         * and the pages want evdev codes, so nativeScanCode is what is forwarded:
         * on Linux that is the evdev code plus 8, which is the same offset X11 has
         * used since it had a keyboard driver at all.
         */
        PageWidget *page = current();
        const bool toKeyboard = m_keyboard->isVisible();
        /* Escape is never forwarded: on a workstation it is the only way out of a
         * page that has asked for every other key, and a terminal with no way out
         * of it is a terminal you reboot the machine to leave. */
        const bool escapes = key->key() == Qt::Key_Escape;
        if (!escapes && (toKeyboard || (page && page->wantsKeys()))) {
            const int code = (int)key->nativeScanCode() - 8;
            int mods = Joypad::ModNone;
            if (key->modifiers() & Qt::ShiftModifier)
                mods |= Joypad::ModShift;
            if (key->modifiers() & Qt::ControlModifier)
                mods |= Joypad::ModCtrl;
            if (key->modifiers() & Qt::AltModifier)
                mods |= Joypad::ModAlt;
            if (code > 0) {
                if (toKeyboard)
                    m_keyboard->keyPressed(code, true, mods);
                else
                    page->keyPressed(code, true, mods);
                return true;
            }
        }

        int action = Joypad::NavNone;
        switch (key->key()) {
        case Qt::Key_Up:        action = Joypad::NavUp; break;
        case Qt::Key_Down:      action = Joypad::NavDown; break;
        case Qt::Key_Left:      action = Joypad::NavLeft; break;
        case Qt::Key_Right:     action = Joypad::NavRight; break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:     action = Joypad::NavOk; break;
        case Qt::Key_Escape:
        case Qt::Key_Backspace: action = Joypad::NavBack; break;
        case Qt::Key_PageUp:    action = Joypad::NavPrevPage; break;
        case Qt::Key_Tab:
        case Qt::Key_PageDown:  action = Joypad::NavNextPage; break;
        case Qt::Key_M:         action = Joypad::NavMenu; break;
        case Qt::Key_Q:         action = Joypad::NavQuit; break;
        default: break;
        }
        if (action != Joypad::NavNone) {
            onNav(action);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
