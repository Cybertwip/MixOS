/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "dashboard.h"
#include "joypad.h"
#include "theme.h"

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

/* ── FilesPage ───────────────────────────────────────────────────────────── */

FilesPage::FilesPage(QWidget *parent)
    : QWidget(parent)
{
    /*
     * /run/j36/card first: that is the card's data partition, mounted read-only by
     * the initramfs because there is no keyboard on this board and no other way to
     * reach it.  It is also the only directory here whose contents the operator put
     * there, which makes it the useful place to open on.  The home directories are
     * the fallback for a boot without that mount, and / for a rootfs with neither.
     */
    m_base = QFileInfo::exists("/run/j36/card") ? QString("/run/j36/card")
           : QFileInfo::exists("/home/ark")     ? QString("/home/ark")
           : QFileInfo::exists("/root")         ? QString("/root")
                                                : QString("/");

    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_model->setRootPath(m_base);

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

    setRoot(m_base);
}

void FilesPage::setRoot(const QString &path)
{
    m_root = QDir::cleanPath(path);
    m_model->setRootPath(m_root);
    const QModelIndex root = m_model->index(m_root);
    m_view->setRootIndex(root);
    m_view->setCurrentIndex(m_model->index(0, 0, root));
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

    m_bar = new StatusBar(this);
    m_apps = new CardGrid(this);
    m_files = new FilesPage(this);
    m_info = new InfoPage(this);
    m_power = new CardGrid(this);
    m_dock = new Dock(this);

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

    connect(m_apps, &CardGrid::activated, this, &Dashboard::onAppActivated);
    connect(m_power, &CardGrid::activated, this, &Dashboard::onPowerActivated);
    connect(m_apps, &CardGrid::indexChanged, this, [this](int) {
        if (m_page == 0)
            m_bar->setTitle(m_apps->currentTitle());
    });
    connect(m_files, &FilesPage::openRequested, this, &Dashboard::onOpenRequested);

    m_dock->setPages(QStringList() << "Apps" << "Files" << "System" << "Power");

    buildPages();

    m_pad = new Joypad(this);
    connect(m_pad, &Joypad::nav, this, &Dashboard::onNav);
    m_info->setInputSummary(
        m_pad->deviceCount() == 0
            ? QString("no /dev/input/event* -- nothing to navigate with")
            : QString("%1: %2").arg(m_pad->deviceCount()).arg(m_pad->deviceNames().join(", ")));

    setPage(0);
    qApp->installEventFilter(this);
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
    for (const QString &dir : QStringList() << "/opt/mixos/share/doom" << "/roms/doom")
        for (const QString &n : names)
            if (QFileInfo(dir + "/" + n).isFile())
                return dir + "/" + n;
    return QString();
}

void Dashboard::buildPages()
{
    QVector<AppEntry> apps;

    /*
     * Doom, and it is first because it is the one thing on this card already proved
     * to put a moving picture on this panel: doomgeneric writes 32-bit pixels into
     * /dev/fb0 and reads the pad through evdev, which is exactly the layer this
     * dashboard draws in.  No EGL, no GBM, no mode set -- so it hands the panel back
     * when it exits, which EmulationStation never did.
     */
    AppEntry doom;
    doom.title = "Doom";
    doom.accent = Theme::blue();
    doom.glyph = GlyphGames;
    doom.exe = firstExisting(QStringList() << "/opt/mixos/bin/doom");
    const QString wad = firstWad();
    if (!doom.exe.isEmpty() && !wad.isEmpty())
        doom.args = QStringList() << "-iwad" << wad;
    doom.available = !doom.exe.isEmpty() && !wad.isEmpty();
    if (doom.exe.isEmpty())
        doom.subtitle = "Not installed. J36_DOOM=1\nputs it in /opt/mixos.";
    else if (wad.isEmpty())
        doom.subtitle = "No IWAD in\n/opt/mixos/share/doom.";
    else
        doom.subtitle = QFileInfo(wad).fileName() + ".\n640x400 straight into /dev/fb0.";
    apps.append(doom);

    AppEntry files;
    files.title = "Files";
    files.subtitle = "Browse the card.";
    files.accent = Theme::teal();
    files.glyph = GlyphFiles;
    files.internal = InternalFiles;
    apps.append(files);

    AppEntry video;
    video.title = "Video";
    video.accent = Theme::pink();
    video.glyph = GlyphVideo;
    video.exe = firstExisting(QStringList() << "/usr/bin/mpv" << "/usr/bin/cvlc"
                                            << "/usr/bin/vlc" << "/usr/bin/ffplay");
    video.subtitle = video.exe.isEmpty() ? QString("No player installed.")
                                         : QFileInfo(video.exe).fileName();
    video.available = !video.exe.isEmpty();
    apps.append(video);

    /*
     * The GPU, and the only thing on this card that asks it to rasterise anything.
     * eglprobe's five paint phases are all clears -- a driver that could do nothing
     * but clear a buffer would pass every one -- so -c is the card that compiles two
     * shaders and turns a cube, and its result is the one that says whether lima
     * works.  It finds the modesetting node itself now rather than assuming card0,
     * which is what the "Operation not supported" run was really reporting.
     *
     * It asks twice because it cannot give the panel back: setting a mode of its own
     * moves the scanout off the LK's framebuffer, this dashboard keeps drawing into
     * that framebuffer, and nothing puts it back on screen short of a reboot.
     */
    AppEntry cube;
    cube.title = "3D cube";
    cube.accent = Theme::purple();
    cube.glyph = GlyphDisplay;
    cube.exe = firstExisting(QStringList() << "/run/j36/eglprobe"
                                           << "/opt/mixos/bin/eglprobe");
    cube.args = QStringList() << "-c" << "20";
    cube.subtitle = cube.exe.isEmpty() ? QString("eglprobe is not on this card.")
                                       : QString("GLES2 through lima, flipped.\nKeeps the panel: reboot after.");
    cube.available = !cube.exe.isEmpty();
    cube.confirm = true;
    apps.append(cube);

    AppEntry system;
    system.title = "System";
    system.subtitle = "What this boot brought up.";
    system.accent = Theme::orange();
    system.glyph = GlyphSettings;
    system.internal = InternalInfo;
    apps.append(system);

    AppEntry power;
    power.title = "Power";
    power.subtitle = "Restart, shut down, console.";
    power.accent = Theme::red();
    power.glyph = GlyphPower;
    power.internal = InternalPower;
    apps.append(power);

    m_apps->setEntries(apps);

    QVector<AppEntry> powers;

    AppEntry restart;
    restart.title = "Restart";
    restart.subtitle = "Press A twice.";
    restart.accent = Theme::orange();
    restart.glyph = GlyphPower;
    restart.internal = InternalReboot;
    powers.append(restart);

    AppEntry off;
    off.title = "Power off";
    off.subtitle = "Press A twice.";
    off.accent = Theme::red();
    off.glyph = GlyphPower;
    off.internal = InternalPoweroff;
    powers.append(off);

    AppEntry console;
    console.title = "Console";
    console.subtitle = "Leave the dashboard and\nhand the panel back to the tty.";
    console.accent = Theme::teal();
    console.glyph = GlyphTerminal;
    console.internal = InternalConsole;
    powers.append(console);

    m_power->setEntries(powers);
}

void Dashboard::resizeEvent(QResizeEvent *event)
{
    const int top = Theme::StatusH;
    const int bottom = height() - Theme::DockH;

    m_bar->setGeometry(0, 0, width(), Theme::StatusH);
    m_dock->setGeometry(0, height() - Theme::DockH, width(), Theme::DockH);

    const QRect page(0, top, width(), qMax(0, bottom - top));
    m_apps->setGeometry(page);
    m_files->setGeometry(page);
    m_info->setGeometry(page);
    m_power->setGeometry(page);

    QWidget::resizeEvent(event);
}

void Dashboard::setPage(int page)
{
    m_page = qBound(0, page, 3);

    m_apps->setVisible(m_page == 0);
    m_files->setVisible(m_page == 1);
    m_info->setVisible(m_page == 2);
    m_power->setVisible(m_page == 3);
    m_dock->setCurrent(m_page);

    switch (m_page) {
    case 0: m_bar->setTitle(m_apps->currentTitle()); break;
    case 1: m_bar->setTitle("Files"); break;
    case 2: m_bar->setTitle("System"); break;
    default: m_bar->setTitle("Power"); break;
    }

    if (m_toast->isVisible())
        m_toast->raise();
    update();
}

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
    m_toastTimer->start(ms);
}

void Dashboard::launch(const QString &title, const QString &exe, const QStringList &args)
{
    if (exe.isEmpty() || !QFileInfo(exe).isExecutable()) {
        toast(title + " is not on this card");
        return;
    }

    toast("Starting " + title, 60000);
    /*
     * Painted before we block, or the toast never reaches the glass: everything
     * below this line runs with the event loop stopped.
     */
    m_toast->repaint();
    QCoreApplication::processEvents();

    /* The child gets the input devices to itself, and whatever it was pressed with
     * is discarded before the dashboard listens again. */
    m_pad->setSuspended(true);
    const int rc = QProcess::execute(exe, args);
    m_pad->setSuspended(false);

    /*
     * The child had the framebuffer, so every pixel is suspect.  A child that set a
     * mode of its own through /dev/dri/card0 has taken the scanout away from the
     * LK's simple-framebuffer as well, and no amount of repainting here brings that
     * back -- which is worth knowing when a launch comes back to a black panel.
     */
    update();

    if (rc == -2)
        toast(title + " would not start");
    else if (rc == -1)
        toast(title + " crashed");
    else if (rc != 0)
        toast(QString("%1 exited %2").arg(title).arg(rc));
    else
        toast(title + " exited");
}

void Dashboard::activate(const AppEntry &entry)
{
    if (!entry.exe.isEmpty() || entry.internal == InternalNone) {
        /* Same two-press gate as Power off, and for the same reason: what it does
         * cannot be undone from here.  The warning has to be shown before the child
         * starts, because after it starts the panel is no longer ours to draw on. */
        if (entry.confirm && m_armedExe != entry.exe) {
            m_armedExe = entry.exe;
            toast(entry.title + " takes the panel for good.\nPress A again to run it.", 6000);
            return;
        }
        m_armedExe.clear();
        launch(entry.title, entry.exe, entry.args);
        return;
    }

    switch (entry.internal) {
    case InternalFiles:
        setPage(1);
        break;
    case InternalInfo:
        setPage(2);
        break;
    case InternalPower:
        setPage(3);
        break;
    case InternalReboot:
        if (m_armed != InternalReboot) {
            m_armed = InternalReboot;
            toast("Press A again to restart");
            return;
        }
        m_armed = InternalNone;
        launch("Restart", firstExisting(QStringList() << "/sbin/reboot" << "/usr/sbin/reboot"),
               QStringList());
        break;
    case InternalPoweroff:
        if (m_armed != InternalPoweroff) {
            m_armed = InternalPoweroff;
            toast("Press A again to power off");
            return;
        }
        m_armed = InternalNone;
        launch("Power off", firstExisting(QStringList() << "/sbin/poweroff" << "/usr/sbin/poweroff"),
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

void Dashboard::onOpenRequested(const QString &path)
{
    static const char *kPlayable[] = { ".mp4", ".mkv", ".avi", ".webm", ".mov",
                                       ".mp3", ".ogg", ".wav", ".flac", ".m4a" };
    const QString lower = path.toLower();
    for (size_t i = 0; i < sizeof(kPlayable) / sizeof(kPlayable[0]); ++i) {
        if (!lower.endsWith(QLatin1String(kPlayable[i])))
            continue;
        const QString player = firstExisting(QStringList() << "/usr/bin/mpv" << "/usr/bin/cvlc"
                                                           << "/usr/bin/vlc" << "/usr/bin/ffplay");
        if (player.isEmpty()) {
            toast("No player is installed");
            return;
        }
        launch(QFileInfo(path).fileName(), player, QStringList() << path);
        return;
    }
    toast("Nothing here opens " + QFileInfo(path).fileName());
}

void Dashboard::onNav(int action)
{
    switch (action) {
    case Joypad::NavPrevPage:
        setPage(m_page == 0 ? 3 : m_page - 1);
        return;
    case Joypad::NavNextPage:
        setPage(m_page == 3 ? 0 : m_page + 1);
        return;
    case Joypad::NavMenu:
        /* The most useful thing Menu can do during bring-up is show what came up. */
        setPage(m_page == 2 ? 0 : 2);
        return;
    case Joypad::NavQuit:
        toast("Power, then Console, leaves the dashboard");
        return;
    default:
        break;
    }

    CardGrid *grid = m_page == 0 ? m_apps : m_page == 3 ? m_power : nullptr;

    if (grid) {
        switch (action) {
        case Joypad::NavUp:    grid->moveBy(0, -1); break;
        case Joypad::NavDown:  grid->moveBy(0, 1); break;
        case Joypad::NavLeft:  grid->moveBy(-1, 0); break;
        case Joypad::NavRight: grid->moveBy(1, 0); break;
        case Joypad::NavOk:    grid->activate(); break;
        case Joypad::NavBack:  if (m_page != 0) setPage(0); break;
        default: break;
        }
        return;
    }

    if (m_page == 1) {
        switch (action) {
        case Joypad::NavUp:    m_files->step(-1); break;
        case Joypad::NavDown:  m_files->step(1); break;
        case Joypad::NavRight:
        case Joypad::NavOk:    m_files->enter(); break;
        case Joypad::NavLeft:
        case Joypad::NavBack:  if (!m_files->leave()) setPage(0); break;
        default: break;
        }
        return;
    }

    /* The System sheet: nothing to move, and Back is the way out. */
    if (action == Joypad::NavBack || action == Joypad::NavOk)
        setPage(0);
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
