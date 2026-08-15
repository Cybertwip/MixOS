/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "dashboard.h"

#include "busy.h"
#include "diagnostics.h"
#include "files.h"
#include "joypad.h"
#include "keyboard.h"
#include "media.h"
#include "packages.h"
#include "panel.h"
#include "pointer.h"
#include "region.h"
#include "settingspage.h"
#include "sharing.h"
#include "stringsdb.h"
#include "switcher.h"
#include "terminal.h"
#include "theme.h"
#include "trace.h"
#include "volume.h"
#include "disks.h"
#include "wifi.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QTimer>

#include <signal.h>
#include <unistd.h>

/*
 * There is no <linux/vt.h>, <linux/kd.h>, <sys/ioctl.h>, <sys/wait.h>, <fcntl.h>,
 * <errno.h> or <string.h> here any more.  They were all for the Console card's
 * fork-a-shell-onto-a-spare-VT, which is gone.
 *
 * <signal.h> and <unistd.h> ARE back, and for one thing each: kill(-pgid, ...),
 * which is how a task in the background is stopped and continued, and setpgid(),
 * which is what makes there be a -pgid to send it to.  Both are in launch() and
 * setForeground() and nowhere else in this file.
 */

namespace {

/*
 * The browser, in the places that have to agree about it: the card's greyed-out
 * test, the process the card starts, and the line typed into the terminal when
 * there is no graphical session on the card.  buildPages() has the long version.
 *
 * kBrowserSession is a shell script the build stages beside the dashboard.  It
 * brings an X server up on /dev/fb0, runs a window manager, an on-screen keyboard
 * and whichever browser is installed inside it, and translates the pad into a
 * pointer with j36-padx.  Everything about that lives in the script and in
 * build-in-vm.sh, which is the point: none of it belongs in a Qt program that is
 * not running when it happens.
 *
 * kBrowserExe is the fallback for a card with no X on it -- links2 in the
 * dashboard's own terminal, which is what this card used to be and is still the
 * only thing that works with nothing but a pty.
 *
 * The start page is shipped in the payload rather than pointed at a search engine
 * because the first thing a browser has to do here is prove it works without
 * anybody typing a URL on an eleven-button keyboard, and because a card that
 * opens a page held on the card itself still opens with no network at all.
 */
const char kBrowserSession[] = "/opt/mixos/bin/j36-browser";
const char kBrowserExe[]   = "/usr/bin/links2";
const char kBrowserStart[] = "/opt/mixos/share/browser/start.html";
const char kBrowserFallbackUrl[] = "https://duckduckgo.com/";

/*
 * The graphical session's path if this card can actually run one, and an empty
 * string if it cannot.
 *
 * ALL THREE PIECES ARE TESTED and not just the script, because the script is in
 * the /opt/mixos payload and the other two come from Debian packages on the rootfs
 * -- so an image built with this feature and a rootfs built without the packages is
 * a real combination, and it must grey the card down to links2 rather than start
 * something that exits immediately.  The browser list is the same one the script
 * searches, in the same order, for the same reason: whichever browser the Packages
 * card installed later is the one that runs.
 *
 * QFileInfo::isExecutable and not exists(): a package that was removed can leave
 * its directory entries behind, and the failure this is guarding against is a card
 * that looks available and starts nothing.
 */
QString graphicalBrowserSession()
{
    static const char *const kServers[] = {
        "/usr/bin/Xorg", "/usr/lib/xorg/Xorg", "/usr/bin/X"
    };
    static const char *const kBrowsers[] = {
        "firefox-esr", "firefox", "netsurf-gtk", "netsurf", "epiphany-browser",
        "luakit", "surf", "dillo", "falkon", "qutebrowser", "chromium",
        "chromium-browser"
    };

    if (!QFileInfo(QString::fromLatin1(kBrowserSession)).isExecutable())
        return QString();
    if (!QFileInfo(QStringLiteral("/usr/bin/xinit")).isExecutable())
        return QString();

    bool haveServer = false;
    for (const char *const s : kServers) {
        if (QFileInfo(QString::fromLatin1(s)).isExecutable()) {
            haveServer = true;
            break;
        }
    }
    if (!haveServer)
        return QString();

    for (const char *const b : kBrowsers) {
        if (QFileInfo(QStringLiteral("/usr/bin/") + QLatin1String(b)).isExecutable())
            return QString::fromLatin1(kBrowserSession);
    }
    return QString();
}

/*
 * WHICH OF THE FOUR PIECES IS NOT ON THIS CARD, in a sentence, for the boot where
 * the answer above is empty.
 *
 * This exists because the fallback is silent and the fallback is WRONG-LOOKING.  A
 * card that cannot start the graphical session opens links2 -- a text browser with
 * no JavaScript -- and does it with no more ceremony than a card that opens Firefox,
 * so the whole report it produces is "the default browser is not Mozilla".  Every
 * one of the four reasons is a package or a payload file, none of them is a setting,
 * and none of them can be guessed at from the outside.
 *
 * AND THE REASON IS ALMOST ALWAYS THE SAME ONE: the rootfs.  firefox-esr, xinit and
 * the X server come from needed_packages.txt, which is the BASE build's business --
 * `--mix-only' rebuilds the kernel, the initramfs and /opt/mixos and cannot install a
 * package on a card that is already flashed.  So a card written before those packages
 * were added keeps opening links2 no matter how many board layers are copied onto it,
 * and this sentence is what says so out loud instead of leaving it to be inferred.
 *
 * The order matches graphicalBrowserSession()'s tests, so the sentence names the
 * first thing that failed and not merely something that is also absent.
 */
QString graphicalBrowserMissing()
{
    if (!QFileInfo(QString::fromLatin1(kBrowserSession)).isExecutable())
        return QCoreApplication::translate(
            "Dashboard", "the /opt/mixos payload has no j36-browser script");
    if (!QFileInfo(QStringLiteral("/usr/bin/xinit")).isExecutable())
        return QCoreApplication::translate(
            "Dashboard", "this card's rootfs has no xinit");

    static const char *const kServers[] = {
        "/usr/bin/Xorg", "/usr/lib/xorg/Xorg", "/usr/bin/X"
    };
    for (const char *const s : kServers) {
        if (QFileInfo(QString::fromLatin1(s)).isExecutable())
            return QCoreApplication::translate(
                "Dashboard", "this card's rootfs has no graphical browser");
    }
    return QCoreApplication::translate(
        "Dashboard", "this card's rootfs has no X server");
}

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

/*
 * A workstation keyboard's key, as one of the actions the pad produces.
 *
 * Out here rather than inline in eventFilter() because both halves of a key --
 * the press and the release -- have to map identically, and two copies of a
 * fourteen-case switch is two copies that can drift.  A release that mapped to a
 * different action than its press would leave the card grid holding a card it
 * thinks is still being pressed.
 */
int navForKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_Up:        return Joypad::NavUp;
    case Qt::Key_Down:      return Joypad::NavDown;
    case Qt::Key_Left:      return Joypad::NavLeft;
    case Qt::Key_Right:     return Joypad::NavRight;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:     return Joypad::NavOk;
    case Qt::Key_Escape:
    case Qt::Key_Backspace: return Joypad::NavBack;
    case Qt::Key_PageUp:    return Joypad::NavPrevPage;
    case Qt::Key_Tab:
    case Qt::Key_PageDown:  return Joypad::NavNextPage;
    case Qt::Key_M:         return Joypad::NavMenu;
    case Qt::Key_Q:         return Joypad::NavQuit;
    /* The two keys on the side of the case, for a workstation that has them.
     * Most desktops grab these before Qt sees them, which costs nothing: the
     * bar is still reachable there through the Settings slider. */
    case Qt::Key_VolumeUp:   return Joypad::NavVolumeUp;
    case Qt::Key_VolumeDown: return Joypad::NavVolumeDown;
    default: break;
    }
    return Joypad::NavNone;
}

} /* namespace */

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
    /* Before buildPages(), so the first setEntries() already lays the cards out
     * the way this card was left rather than laying them out and then moving
     * them. */
    m_apps->setOrder(Settings::instance().cardOrder());
    Trace::step("MediaPage");
    m_media = new MediaPage(this);
    Trace::step("SettingsPage");
    m_settings = new SettingsPage(this);

    Trace::step("FilesPage");
    m_files = new FilesPage(this);
    Trace::step("TerminalPage");
    m_terminal = new TerminalPage(this);
    Trace::step("WifiPage");
    m_wifi = new WifiPage(this);
    Trace::step("SharingPage");
    m_sharing = new SharingPage(this);
    Trace::step("PackagesPage");
    m_packages = new PackagesPage(this);
    Trace::step("MousePage");
    m_mouse = new MousePage(this);
    Trace::step("DisplayPage");
    m_display = new DisplayPage(this);
    Trace::step("RegionPage");
    m_region = new RegionPage(this);
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
        /* An armed Power off expires with its own prompt.  Anything else would leave
         * a button that shuts the board down on the next press, minutes later, with
         * no warning on screen. */
        m_armed = InternalNone;
        m_armedExe.clear();
    });

    /*
     * Built here, after the toast and before the keyboard, because the shell's
     * overlays are stacked in construction order and this one belongs between
     * them: over the toast, which is transient text the volume keys do not
     * interrupt, and under the keyboard, which is the only overlay a press can be
     * aimed at.  No mixer is touched -- the first probe happens on the first
     * press, so a board with no sound card costs nothing at startup.
     */
    Trace::step("Volume overlay");
    m_volumeBar = new VolumeOverlay(this);
    connect(m_volumeBar, &VolumeOverlay::changed, this, &Dashboard::syncVolumeOverlay);

    m_busy = new Busy(this);
    connect(m_busy, &Busy::changed, this, &Dashboard::syncBusyOverlay);

    Trace::step("Keyboard overlay");
    m_keyboard = new Keyboard(this);
    connect(m_keyboard, &Keyboard::finished, this, &Dashboard::onKeyboardFinished);

    /*
     * Last of the overlays, because it is the only one that has to appear over a
     * program this shell does not own -- see switcher.h.  Its three signals are
     * indices into the rows switcherRows() built, and row 0 is this dashboard, so
     * every one of them is `row - 1' away from a task.
     */
    Trace::step("Switcher overlay");
    m_switcher = new Switcher(this);
    connect(m_switcher, &Switcher::chosen, this, [this](int row) {
        setForeground(row - 1);
    });
    connect(m_switcher, &Switcher::closeRequested, this, [this](int row) {
        closeTask(row - 1);
    });
    connect(m_switcher, &Switcher::dismissed, this, [this]() {
        /* Cancel is "put back what was in front", and it is the ordinary switch
         * rather than a path of its own so that the two cannot drift apart. */
        setForeground(m_switcherWas);
    });

    /*
     * The other way in.  A child that grabbed the pad sends SIGUSR1 and main.cpp's
     * handler sets a flag; this is what notices.  Quarter of a second is under the
     * time it takes to let go of a button, and the timer only runs while a task is
     * in front -- which is the only time anything can send that signal.
     */
    m_requestTimer = new QTimer(this);
    m_requestTimer->setInterval(250);
    connect(m_requestTimer, &QTimer::timeout, this, [this]() {
        if (SwitcherRequest::take())
            showSwitcher();
    });

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
    /* The two spinners are the same wait; which one is drawn follows the mouse.
     * See the note on setPointerStyle() in busy.h. */
    connect(m_pointer, &Pointer::awakeChanged, m_busy, &Busy::setPointerStyle);

    Trace::step("page tables");
    m_all << m_apps << m_media << m_settings
          << m_files << m_terminal << m_wifi << m_sharing << m_packages
          << m_diagnostics << m_mouse << m_display << m_region << m_info;
    for (PageWidget *page : m_all) {
        adopt(page);
        page->hide();
    }

    Trace::step("connections");
    connect(m_apps, &CardGrid::activated, this, &Dashboard::onAppActivated);
    connect(m_apps, &CardGrid::indexChanged, this, [this](int) {
        if (m_current == m_apps)
            m_bar->setTitle(m_apps->title());
    });
    /*
     * The grid owns the arrangement and the shell owns writing it down -- the same
     * split ListPane has with its sliders, and for the same reason: widgets.cpp
     * has no business knowing where the settings file is.
     */
    connect(m_apps, &CardGrid::orderChanged, this, [](const QStringList &keys) {
        Settings::instance().setCardOrder(keys);
    });
    connect(m_apps, &CardGrid::ejectRequested, this, &Dashboard::onEjectRequested);
    /*
     * A stick arriving or leaving rebuilds the grid.  It is a rebuild rather than an
     * insert because the grid's order is a saved list of keys and only buildPages()
     * knows how to apply it; the cost is a handful of AppEntry copies once per plug.
     */
    connect(&Disks::instance(), &Disks::changed,
            this, &Dashboard::onVolumesChanged);
    connect(m_files, &FilesPage::openRequested, this, &Dashboard::onOpenRequested);
    connect(m_settings, &SettingsPage::openRequested, this, &Dashboard::onSettingsOpen);
    connect(m_packages, &PackagesPage::terminalRequested,
            this, &Dashboard::onTerminalRequested);
    connect(m_diagnostics, &DiagnosticsPage::launchRequested,
            this, &Dashboard::onLaunchRequested);
    connect(&Strings::instance(), &Strings::languageChanged,
            this, &Dashboard::retranslate);

    connect(m_pad, &Joypad::nav, this, &Dashboard::onNav);
    connect(m_pad, &Joypad::navReleased, this, &Dashboard::onNavReleased);
    connect(m_pad, &Joypad::key, this, &Dashboard::onKey);
    connect(m_pad, &Joypad::pointerMove, m_pointer, &Pointer::onMove);
    connect(m_pad, &Joypad::pointerButton, m_pointer, &Pointer::onButton);
    connect(m_pad, &Joypad::pointerWheel, m_pointer, &Pointer::onWheel);
    connect(m_pointer, &Pointer::changed, this, &Dashboard::syncPointerOverlay);
    connect(m_pad, &Joypad::deviceAdded, this, &Dashboard::onInputDeviceAdded);
    connect(m_pad, &Joypad::deviceRemoved, this, &Dashboard::onInputDeviceRemoved);
    connect(m_pad, &Joypad::devicesChanged, this, &Dashboard::refreshInputSummary);
    connect(m_pad, &Joypad::headphoneJack, this, &Dashboard::onHeadphoneJack);
    connect(m_pad, &Joypad::switcherRequested, this, &Dashboard::showSwitcher);

    /* Stats every candidate executable on the card. */
    Trace::step("buildPages -- looks for the apps on disk");
    buildPages();

    refreshInputSummary();

    /*
     * THE STATE THE BOARD WAS ALREADY IN, applied once, before anything is on
     * screen.
     *
     * A switch only produces an event when it moves, so headphones that were in
     * the jack when this program started generate nothing to listen for -- and
     * the mixer they meet was restored by alsa-restore from whatever the last
     * shutdown saved, which on a device switched off with headphones in is a
     * muted speaker and nothing plugged into it.  That boot is silent, and the
     * only clue is a Settings row two levels down.  Joypad read the level at
     * open, so the answer is already here; this is the one line that acts on it.
     *
     * Nothing happens when the jack is unknown.  See onHeadphoneJack.
     */
    Trace::step("headphone jack -- the state we booted into");
    Volume::noteJack(!m_pad->jackKnown()
                         ? Volume::JackUnknown
                         : (m_pad->jackPlugged() ? Volume::JackPlugged : Volume::JackEmpty));
    if (m_pad->jackKnown())
        applyJackRouting(m_pad->jackPlugged(), false);

    Trace::step("goHome");
    goHome();
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
    connect(page, &PageWidget::busyRequested, this, &Dashboard::onBusyRequested);
}

QString Dashboard::firstExisting(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo(c).isExecutable())
            return c;
    return QString();
}

/*
 * EVERYTHING IS ON THIS GRID NOW, AND THAT IS THE POINT.
 *
 * Media and Settings used to be dock tabs, and for a while they each had a card
 * here as well that did nothing but switch to the tab -- two doors into one room,
 * which is one feature and one piece of furniture the user has to learn is
 * furniture.  The answer was the wrong one: the cards were deleted and the dock
 * kept.  The dock is gone now and the cards are back, and they open their page the
 * same way every other card here does.  One kind of thing on the screen, one
 * gesture to open it, B to come back.
 *
 * WHY POWER IS NOT A TAB EITHER.  It was, and it held two cards -- Power off and
 * System -- on a page with room for eight.  A tab that is six-eighths empty is not
 * a section, it is a detour: three button presses to reach a thing, and a whole
 * root page's worth of navigation spent on it.  The two cards are at the end of
 * this list now, on a grid that has the room, and the tab is gone.  Power off is
 * LAST on purpose: the D-pad lands on the first card when the dashboard starts,
 * and the card that shuts the board down should be the furthest thing from where
 * a thumb rests.  It is still behind the two-press m_armed gate on top of that.
 *
 * EVERY ENTRY HAS A key.  It is the identity the saved arrangement is written in
 * -- see AppEntry -- so it is stable ASCII and it is not the title, which is
 * translated.  Changing one of these strings moves that card back to the end of
 * the grid for everybody who had already arranged it, which is the only real cost
 * of getting one wrong.
 */
void Dashboard::buildPages()
{
    QVector<AppEntry> apps;

    /*
     * A web browser, and a real graphical one: a window with a URL bar, images,
     * CSS, and a pointer the D-pad drives.  It gets there the way every card here
     * launches -- an external process that owns the framebuffer while it runs and
     * hands it back when it exits --
     * and the details are in build-in-vm.sh and in tools/j36-padx.c, where they
     * belong.  What is worth having HERE is why the answer is that and not
     * something smaller, because two of the three things that decide it look like
     * oversights until they are written down.
     *
     *   1. NOTHING GRAPHICAL IN THE ARCHIVE CAN REACH THIS PANEL BY ITSELF.
     *      netsurf-fb is the obvious candidate and does not work: Debian's copy of
     *      libnsfb is built with the sdl, xcb, vnc and wayland surfaces and WITHOUT
     *      the linux one, so it has no way to open /dev/fb0 -- the binary contains
     *      no /dev/fb, no FBIOGET and no fb0 -- and its sdl surface goes through
     *      libsdl1.2debian, which in trixie is sdl12-compat over SDL2, and SDL2
     *      dropped the fbcon driver years ago.  links2's OWN framebuffer driver
     *      would have been ideal, and the Debian build leaves it out too: -driver
     *      fb is listed in its help text, but the binary carries neither /dev/fb0
     *      nor FRAMEBUFFER nor KDSETMODE, only the X driver.  Everything else
     *      graphical wants an X server or a Wayland compositor.  All of that was
     *      checked against the binaries and all of it still holds.
     *
     *   2. SO THERE IS AN X SERVER, and it is NOT the Console card again.  The note
     *      further down, where that card used to be built, is the record of what a
     *      VT switch costs here: the kernel's console driver was never bound to this
     *      simplefb, so a shell moved to another VT is a shell nobody can see.  An X
     *      server needs none of that.  xf86-video-fbdev mmaps /dev/fb0 and draws its
     *      own pixels into it, exactly as this dashboard's linuxfb plugin already
     *      does, and it asks the console layer for nothing because there are no
     *      kernel-rendered glyphs anywhere in the picture.  The VT dance is separate
     *      and optional: `-sharevts -novtswitch -keeptty vt1' is Xorg's own way of
     *      being told the VT belongs to somebody else, and with those it issues no
     *      VT_SETMODE, no VT_ACTIVATE and no KDSETMODE -- which matters, because this
     *      process is still holding /dev/tty0 in KD_GRAPHICS while that runs.
     *
     *   3. X STILL HAD NOTHING TO DRIVE A BROWSER WITH, and that was the real gap.
     *      This device has eleven buttons and no touchscreen; pointer.cpp and
     *      keyboard.cpp draw a cursor and a keyboard that exist only inside this
     *      process.  Handing the pad to X does not work either -- udev tags it
     *      ID_INPUT_JOYSTICK, which libinput refuses by design, and BTN_A is evdev
     *      0x130, past the 255 an X keycode can hold, so the older evdev driver
     *      cannot map it.  tools/j36-padx.c closes that gap: it grabs the pad,
     *      connects to the server and synthesises motion, clicks and keys with
     *      XTEST, so the D-pad is a pointer and A is a click.  matchbox-keyboard is
     *      the on-screen keyboard inside the session, on Select.
     *
     * WHICH BROWSER IS NOT DECIDED HERE.  The image installs firefox-esr, because
     * the 2026 web is JavaScript and a browser without it shows a blank page on half
     * the sites anybody would open -- Debian trixie has a real armhf build, 140 ESR,
     * so this is Gecko with a JIT and not a compatibility shim.  It also installs
     * netsurf-gtk beside it, 4 MB and no JavaScript, for the day 946 MB of RAM with
     * no swap is not enough to start the other one.  Beyond those two the session
     * script takes whichever of a dozen browsers is on the card, chromium included,
     * so a browser installed from the Packages page later is the one this card runs.
     * Nothing in this file has to change for that; the list below only has to stay
     * in the same order as the script's.
     *
     * AND THE OLD CARD IS STILL HERE, one branch down, for a card with no X on it:
     * links2 in this dashboard's own terminal, driven by the arrow keys and Enter.
     * It needs nothing but a pty, so it is the honest answer for a rootfs that never
     * got the X packages -- and it is why this card is greyed out only when there is
     * neither.  HOME is pointed at /home/virtua in both, which is the one partition
     * on this card meant to be written and the one the Sharing page exports, so a
     * file saved out of the browser turns up on the laptop over SMB.
     */
    AppEntry browser;
    browser.key = QStringLiteral("browser");
    browser.title = tr("Browser");
    browser.accent = Theme::teal();
    browser.glyph = GlyphGlobe;
    /*
     * Both are set.  A non-empty exe makes activate() take the launch() path and
     * never reach the switch; an empty one falls through to InternalBrowser and the
     * terminal.  The card is one card either way, so the arrangement a user saved
     * survives a rootfs that gains or loses the X packages.
     */
    browser.internal = InternalBrowser;
    browser.exe = graphicalBrowserSession();
    browser.available = !browser.exe.isEmpty()
                        || !firstExisting(QStringList() << kBrowserExe).isEmpty();
    if (!browser.available)
        browser.reason = tr("No browser on this card. The Packages page can add "
                            "firefox-esr, or links2 for a text one.");
    apps.append(browser);

    /*
     * Second, because after "play a game" the next thing anybody does with a
     * handheld is watch something on it, and because this is the card that took the
     * dock's place: it has to be where a thumb lands, not at the end of a list.
     */
    AppEntry media;
    media.key = QStringLiteral("media");
    media.title = tr("Media");
    media.accent = Theme::orange();
    media.glyph = GlyphVideo;
    media.internal = InternalMedia;
    apps.append(media);

    AppEntry terminal;
    terminal.key = QStringLiteral("terminal");
    terminal.title = tr("Terminal");
    terminal.accent = Theme::green();
    terminal.glyph = GlyphTerminal;
    terminal.internal = InternalTerminal;
    apps.append(terminal);

    AppEntry files;
    files.key = QStringLiteral("files");
    files.title = tr("Files");
    files.accent = Theme::teal();
    files.glyph = GlyphFiles;
    files.internal = InternalFiles;
    apps.append(files);

    /*
     * ONE CARD PER MOUNTED USB VOLUME, next to Files because that is what they are:
     * the same browser, opened somewhere else and not allowed out of it.
     *
     * WHY A CARD AND NOT A NOTIFICATION.  A toast saying "BACKUP mounted" is gone in
     * two seconds and leaves the user to find the disk through a file browser they
     * have to open, walk to /media in, and remember the name of.  A card is where
     * everything else on this device already is, it is reached by the gesture that
     * reaches everything else, and it stays there for exactly as long as the disk
     * does -- which is the honest lifetime for it.
     *
     * THEY ARE NOT PINNED TO THIS POSITION.  This is the position they are BORN in,
     * once, and only for a volume the saved arrangement has never seen: setEntries
     * lays the saved keys out first, so a stick that has been picked up and moved to
     * the front of the grid comes back to the front of the grid the next time it is
     * plugged in.  That is the whole reason Volume::key is derived from the mount
     * point and not from the kernel name -- see disks.h.
     *
     * The greyed-out case is a read-only mount: a dirty NTFS volume, or a disk with
     * errors on it.  It still opens -- reading it is exactly what it is good for --
     * so `available' stays true and the state is said in the menu instead.
     */
    for (const Disk &v : Disks::instance().list()) {
        AppEntry disk;
        disk.key = v.key();
        disk.title = v.name();
        disk.accent = v.readOnly ? Theme::yellow() : Theme::teal();
        disk.glyph = GlyphDrive;
        disk.internal = InternalVolume;
        disk.ejectable = true;
        apps.append(disk);
    }

    AppEntry packages;
    packages.key = QStringLiteral("packages");
    packages.title = tr("Packages");
    packages.accent = Theme::yellow();
    packages.glyph = GlyphPackage;
    packages.internal = InternalPackages;
    apps.append(packages);

    AppEntry wifi;
    wifi.key = QStringLiteral("wifi");
    wifi.title = tr("Wi-Fi");
    wifi.accent = Theme::blue();
    wifi.glyph = GlyphWifi;
    wifi.internal = InternalWifi;
    apps.append(wifi);

    /*
     * Next to Wi-Fi rather than in Settings, and that is not an aesthetic choice:
     * settings.h says the hub holds settings and not applications, and this page
     * starts a daemon, writes /etc and generates a credential.  It is also the
     * card whose usefulness depends entirely on the one above it, so they sit
     * together.
     */
    AppEntry sharing;
    sharing.key = QStringLiteral("sharing");
    sharing.title = tr("Sharing");
    sharing.accent = Theme::pink();
    sharing.glyph = GlyphFiles;
    sharing.internal = InternalSharing;
    apps.append(sharing);

    /*
     * What the "3D cube" card became.  The cube is still in there and still turns;
     * it is rasterised by QPainter now, because eglprobe's GLES2 cube cannot be
     * SHOWN on this board -- lima has no CRTC and mtk_drm is not loaded, so there is
     * nothing to flip a rendered buffer onto.  The page says so, with the evidence.
     */
    AppEntry diag;
    diag.key = QStringLiteral("diagnostics");
    diag.title = tr("Diagnostics");
    diag.accent = Theme::purple();
    diag.glyph = GlyphChip;
    diag.internal = InternalDiagnostics;
    apps.append(diag);

    /*
     * There was a Console card on the old Power tab, between Power off and System,
     * and it is gone rather than fixed.  It forked a login shell onto a spare VT and
     * blocked the event loop until that shell exited, and on this board opening it
     * was a hang: the dashboard stops painting the moment the fork starts, and if
     * the VT switch does not take -- which is what happens when the panel is a
     * simplefb the kernel's console driver was never bound to -- then nothing is
     * drawn on the glass by anyone, and the only input path left is the one that was
     * suspended on purpose two lines earlier.  No frame, no pad, no way back.
     *
     * The recovery would have been VT_WAITACTIVE timeouts, a watchdog on the child
     * and a fallback that undoes the mode switch, which is a console driver's worth
     * of work to reach a prompt that the Terminal card above already gives --
     * in-process, on the panel that is known to draw, with the pad still live.
     * So: use Terminal.  See terminal.cpp.
     */

    /*
     * Near the end, next to System, because that is what it is: the two cards you
     * open to change something about the board rather than to do something with it.
     * It is also the card the Menu button opens from anywhere, so where it sits on
     * the grid matters less than it does for the rest.
     */
    AppEntry settings;
    settings.key = QStringLiteral("settings");
    settings.title = tr("Settings");
    settings.accent = Theme::teal();
    settings.glyph = GlyphSettings;
    settings.internal = InternalSettings;
    apps.append(settings);

    AppEntry info;
    info.key = QStringLiteral("system");
    info.title = tr("System");
    info.accent = Theme::ink3();
    info.glyph = GlyphInfo;
    info.internal = InternalInfo;
    apps.append(info);

    /*
     * Last, and the header of this function says why.  There was a Restart card
     * beside it once and it is gone because the board has a power button that
     * reboots it and a card cannot do that better -- it was only ever a second way
     * to press a button.
     *
     * "Last" is the DEFAULT position, not a fixed one: a user who wants Power off
     * first can pick it up and put it there, and the grid will remember.  That is
     * the same freedom every other card has, and taking it away from this one alone
     * would be pretending the two-press gate is not the real guard.
     */
    AppEntry off;
    off.key = QStringLiteral("poweroff");
    off.title = tr("Power off");
    off.accent = Theme::red();
    off.glyph = GlyphPower;
    off.internal = InternalPoweroff;
    apps.append(off);

    m_apps->setEntries(apps);
}

/*
 * A language was picked.  Everything that is rebuilt on the way into a page
 * retranslates itself the next time that page is entered -- which for a page you
 * had to walk to the Language list from is immediately, on the way back.  What is
 * left is the card grid, built once in the constructor and never entered from
 * anywhere: you arrive at it by leaving something else.
 */
void Dashboard::retranslate()
{
    const int apps = m_apps->index();

    m_apps->setPageTitle(tr("Apps"));
    buildPages();

    /*
     * setEntries resets the selection; putting it back is what keeps a language
     * change from also being a jump to the first card.  The ARRANGEMENT needs no
     * such help -- buildPages() hands over the same keys in the same order and the
     * grid re-applies the saved order to them, so translating the titles moves
     * nothing.  Which is the whole reason the order is keyed rather than titled.
     */
    m_apps->setIndex(apps);

    applyChrome();
    update();
}

/*
 * A disk was plugged in or pulled out.
 *
 * The same shape as retranslate(), and for the same reason: the grid's arrangement
 * lives in a saved list of keys and only buildPages() knows how to hand the entries
 * over in an order the grid can re-apply it to.  So the whole grid is rebuilt and the
 * selection is put back by hand.
 *
 * PUT BACK BY KEY, not by index.  A language change cannot move a card, but a disk
 * arriving can: a new volume card is appended, and if the saved order has it before
 * whatever was selected, every index after it shifts by one.  Restoring index 4 would
 * then select the card that moved into slot 4 rather than the one that was there --
 * which, at the moment a stick is plugged in, would look like the machine wandering
 * off on its own.
 */
void Dashboard::onVolumesChanged()
{
    const QVector<AppEntry> before = m_apps->entries();
    const int was = m_apps->index();
    const QString key = (was >= 0 && was < before.size()) ? before[was].key : QString();

    buildPages();

    const QVector<AppEntry> now = m_apps->entries();
    int at = -1;
    for (int i = 0; i < now.size(); ++i) {
        if (now[i].key == key) {
            at = i;
            break;
        }
    }
    /* The selected card was the disk that just left.  Nothing to go back to, so the
     * selection stays where the grid put it -- the first card -- rather than landing
     * on whichever unrelated card inherited the slot. */
    if (at >= 0)
        m_apps->setIndex(at);

    update();
}

/*
 * Eject, from the long-press menu on a volume's card.
 *
 * The work is Disks::eject(): stop the automounter's unit for that device, and if
 * that is not enough, sync and umount by hand.  It is seconds rather than instant --
 * a stick with a gigabyte of dirty pages behind it has to write them all before the
 * umount returns -- so the ring turns for the whole of it.  Blocking here is
 * deliberate: an eject that returned immediately and finished later is an eject the
 * user would answer by pulling the stick out mid-flush.
 */
void Dashboard::onEjectRequested(int index)
{
    const QVector<AppEntry> apps = m_apps->entries();
    if (index < 0 || index >= apps.size())
        return;

    const AppEntry entry = apps[index];
    const Disk *v = Disks::instance().byKey(entry.key);
    if (!v) {
        toast(tr("%1 is no longer plugged in").arg(entry.title), 3000);
        return;
    }

    /* If the browser is standing inside this volume it has to come out first: the
     * page would otherwise be sitting on a directory that is about to stop being one,
     * and its own reaction to the volume vanishing would fire while it is on screen. */
    while (m_stack.contains(m_files))
        pop();

    m_pointer->sleep();
    m_busy->setPointerStyle(false);
    m_busy->start(tr("Ejecting %1").arg(entry.title));

    QString error;
    const bool ok = Disks::instance().eject(entry.key, &error);

    m_busy->stop();

    if (ok)
        toast(tr("%1 can be removed").arg(entry.title), 3000);
    else
        toast(error.isEmpty() ? tr("%1 is busy").arg(entry.title) : error, 4000);
}

/* ── the page stack ──────────────────────────────────────────────────────── */

PageWidget *Dashboard::current() const
{
    if (!m_stack.isEmpty())
        return m_stack.last();
    return m_apps;
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

/*
 * All the way back to the grid, however deep the stack got.
 *
 * There is no wrapping ring of tabs to step through any more, so this is what is
 * left of setRoot(): one destination, and the only way to it that skips the stack.
 * Used when a page closes itself and when the shell needs somewhere safe to be.
 */
void Dashboard::goHome()
{
    m_stack.clear();
    showPage(m_apps);
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

    const QRect normal(0, Theme::StatusH, width(),
                       qMax(0, height() - Theme::StatusH));
    if (page)
        page->setGeometry(full ? rect() : normal);

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
    if (m_volumeBar->isVisible())
        m_volumeBar->raise();
    if (m_busy->isVisible())
        m_busy->raise();
    /* Over all of them, because it is the only overlay that is on the glass while
     * a program outside this process is what the panel belongs to. */
    if (m_switcher->isVisible())
        m_switcher->raise();
    m_pointer->raise();

    /* Leaving a film is the one cursor mode change nothing else notices: the arrow
     * is not moving, so it will not ask on its own.  The ring has the same blind
     * spot and for the same reason -- it may not be turning at the moment the film
     * goes away. */
    syncPointerOverlay();
    syncBusyOverlay();

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

    const QRect normal(0, Theme::StatusH, width(),
                       qMax(0, height() - Theme::StatusH));
    for (PageWidget *page : m_all)
        page->setGeometry(normal);

    /* The keyboard is an overlay over the bottom of the screen rather than a page:
     * what is being typed into stays visible above it. */
    const int kb = qMin(300, height());
    m_keyboard->setGeometry(0, height() - kb, width(), kb);

    /* The volume bar is given the WHOLE panel and not `normal', because it has to
     * be placeable over a page that took the status bar away -- the Media player
     * at full screen is the main thing anybody presses VOL+ during. */
    m_volumeBar->placeIn(rect());
    /* Centred on the whole panel, for the same reason: the page it is waiting for
     * is usually the one that has taken the status bar away. */
    m_busy->placeIn(rect());

    /* The whole panel too, and for the strongest version of that reason: what it
     * covers is not a page at all but a stopped program. */
    m_switcher->setGeometry(rect());

    applyChrome();
    QWidget::resizeEvent(event);
}

void Dashboard::syncVolumeOverlay()
{
    if (!m_volumeBar->isRedirected())
        return;
    if (!m_media || !m_media->glOwnsScreen()) {
        /* The film ended between the press and this signal.  Nothing is drawing
         * the bar any more, so give it back to Qt -- setRedirected() re-shows it
         * if its three seconds are still running. */
        m_volumeBar->setRedirected(false);
        return;
    }

    /*
     * mapToGlobal and not geometry(): the bar's geometry is in this widget's
     * coordinates, and GlVideo wants the framebuffer's.  They are the same
     * rectangle today, because the shell is the top level and it is full screen --
     * which is exactly the kind of "the same today" that stops being true the
     * first time anything is inset, and comes out as a volume bar drawn somewhere
     * else entirely.  MediaPage::present() maps its own rectangle the same way.
     */
    const QRect at(m_volumeBar->mapToGlobal(QPoint(0, 0)), m_volumeBar->size());
    m_media->setVolumeOverlay(m_volumeBar->snapshot(), at);

    /* An expired bar hands back a null image, which clears the layer -- and the
     * film is no longer being drawn over, so the redirect has nothing left to do.
     * Dropping it here means the next press is decided fresh in onNav(). */
    if (!m_volumeBar->isUp())
        m_volumeBar->setRedirected(false);
}

/*
 * THE CURSOR OVER A FILM, and the mode switch that gets it there.
 *
 * The volume bar can decide its mode at the moment of the press, because there is
 * exactly one moment and the shell is standing in it.  A cursor has no such
 * moment: it moves, and whether the thing underneath it is a GPU-owned film or a
 * Qt page is a question with a different answer every time.  So Pointer emits
 * changed() in both modes and this is where the answer is looked up -- once per
 * actual movement, which is what the dedup in Pointer::announce() buys.
 *
 * The film ending is the other direction, and applyChrome() calls this for it:
 * a cursor left redirected after the picture went away would be a cursor nobody
 * is drawing.
 */
void Dashboard::syncPointerOverlay()
{
    const bool overFilm = m_media && m_media->glOwnsScreen();

    if (m_pointer->isRedirected() != overFilm) {
        /* Take the old drawing down before switching, in whichever buffer it was:
         * setRedirected() hides or shows the Qt widget, and this clears the GPU
         * layer, and doing only one of the two leaves an arrow that never moves
         * again next to the one that does. */
        if (!overFilm && m_media)
            m_media->setPointerOverlay(QImage(), QRect());
        m_pointer->setRedirected(overFilm);
        /* setRedirected(true) announces, which re-enters here with the modes now
         * agreeing and hands the picture over; there is nothing left to do. */
        return;
    }

    if (!overFilm)
        return;

    /* mapToGlobal for the same reason the volume bar does it, and with the same
     * caveat: framebuffer coordinates are what GlVideo wants, and they are only
     * the shell's own coordinates for as long as the shell is full screen.  The
     * cursor's hot spot is its top-left corner, so this rectangle needs no
     * adjustment -- see pointer.h. */
    const QRect at(m_pointer->mapToGlobal(QPoint(0, 0)), m_pointer->size());
    m_media->setPointerOverlay(m_pointer->snapshot(), at);
}

/*
 * ── THE RING, AND WHY THE SHELL HOLDS IT ────────────────────────────────────
 *
 * Everything that used to freeze this dashboard froze it the same way: a fork and
 * a blocking wait on the UI thread.  Shell::waitForFinished() sliced those waits
 * up and repainted between the slices, which kept the panel from going black but
 * could not make anything move -- so a four-second ffprobe was indistinguishable
 * from a crash, and the honest answer would have been a spinner that no stopped
 * event loop could ever have turned.
 *
 * The waits are asynchronous now.  This is the spinner they earned.  It is the
 * shell's and not the page's for the two reasons every other overlay here is:
 * a page that has taken the whole panel cannot draw a widget over itself while
 * the GPU owns the pixels, and one spinner per page is several spinners the
 * moment anything is pushed on top of anything.
 */
void Dashboard::onBusyRequested(bool on, const QString &what)
{
    /*
     * From WHICHEVER page, including one that is no longer on the glass -- music
     * keeps playing after the Media page is popped, so its probe can answer to a
     * shell showing the card grid.  That is not a reason to ignore it: something
     * really is being loaded, and saying so on the grid is more honest than a
     * dashboard that looks idle while the SD card grinds.
     */
    if (!on) {
        m_busy->stop();
        syncBusyOverlay();
        return;
    }

    m_busy->setPointerStyle(m_pointer->awake());
    m_busy->start(what.isEmpty() ? tr("Loading") : what);
    m_busy->raise();
    syncBusyOverlay();
}

void Dashboard::syncBusyOverlay()
{
    const bool overFilm = m_media && m_media->glOwnsScreen();

    if (m_busy->isRedirected() != overFilm) {
        /* Down in whichever buffer it was drawn in, before it goes up in the
         * other one -- the same two-sided clear syncPointerOverlay() does. */
        if (!overFilm && m_media)
            m_media->setBusyOverlay(QImage(), QRect());
        m_busy->setRedirected(overFilm);
        /* setRedirected() emits changed(), which re-enters here with the modes
         * agreeing and hands the picture over.  Nothing left to do. */
        return;
    }

    if (!overFilm)
        return;

    const QRect at(m_busy->mapToGlobal(QPoint(0, 0)), m_busy->size());
    m_media->setBusyOverlay(m_busy->snapshot(), at);
}

/* ── the pages talking back ──────────────────────────────────────────────── */

void Dashboard::onToastRequested(const QString &text, int ms)
{
    toast(text, ms > 0 ? ms : 2400);
}

void Dashboard::onCloseRequested()
{
    /* No `else' any more.  A page that closes itself while the stack is empty is
     * the grid, and the grid closing itself would be the dashboard quitting. */
    if (!m_stack.isEmpty())
        pop();
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
        /* The same sentence the cards use, through tr() rather than concatenated
         * -- this path used to build it by hand, which meant a card opened from
         * the file browser got the English one in every language. */
        toast(tr("%1 takes the panel.  Hold FN to come back.\nPress A again to run it.")
                  .arg(title),
              6000);
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

void Dashboard::onInputDeviceAdded(const QString &name, bool mouse, bool keyboard)
{
    /*
     * A mouse is worth its own word because it is the one arrival that changes how
     * the whole shell is driven; a keyboard, because the Terminal is about to
     * behave differently.  Anything else -- a gamepad, a hub's own node, whatever
     * a docking station brings with it -- gets the neutral line, which is still
     * more than the nothing this used to say.
     */
    if (mouse)
        toast(tr("Mouse connected: %1").arg(name));
    else if (keyboard)
        toast(tr("Keyboard connected: %1").arg(name));
    else
        toast(tr("Input connected: %1").arg(name));

    /*
     * Show the cursor without moving it, so there is something on screen to aim
     * with.  Only for a mouse: waking the pointer for a gamepad would put an arrow
     * over a grid that is being driven with a D-pad.
     */
    if (mouse && m_pointer && Settings::instance().mouse().enabled)
        m_pointer->wake();
}

void Dashboard::onInputDeviceRemoved(const QString &name)
{
    toast(tr("Input disconnected: %1").arg(name));
}

void Dashboard::onHeadphoneJack(bool plugged)
{
    Volume::noteJack(plugged ? Volume::JackPlugged : Volume::JackEmpty);
    applyJackRouting(plugged, true);
}

/*
 * ── WHAT A PLUG ACTUALLY DOES, AND WHY IT IS NOT "MUTE" ──
 *
 * Two switches move, not one.  "Speaker Amp" goes off and "Headphone" goes on,
 * because they are two outputs off one DAC and this board will happily drive
 * both -- so switching the amp off without switching the buffers on is a device
 * that goes silent when headphones are plugged into it, which is a worse bug
 * than the one being fixed.  Neither is the master mute: muting would take the
 * level down for everything, the volume keys would fight it, and the bar would
 * show a muted card while headphones were playing.
 *
 * AND WHAT COMES BACK ON UNPLUGGING IS WHAT THE USER CHOSE, not "on".  The
 * speaker's state at the moment of the unplug is the one this function put
 * there, so reading it back would be reading our own answer; Settings holds the
 * intention instead, and speakerWanted() is that.  Somebody who deliberately
 * turned the speaker off -- a quiet room, a broken amp -- does not get it turned
 * back on by pulling their headphones out.
 *
 * The headphone amp is switched off on unplugging for the same symmetry, and
 * because there is nothing on the other end of it to hear.
 *
 * TWO FORKS OF amixer, once per plug.  This is a human-speed event.
 */
void Dashboard::applyJackRouting(bool plugged, bool announce)
{
    const bool haveSpeaker = Volume::present(Volume::Speaker);
    const bool haveHeadphones = Volume::present(Volume::Headphones);
    if (!haveSpeaker && !haveHeadphones)
        return;

    /* Headphones first, so the plug never produces a moment with both outputs
     * off -- which on a fast enough ear is an audible gap in whatever is
     * playing.  Unplugging is the other order for the same reason. */
    if (plugged) {
        if (haveHeadphones)
            Volume::setOn(Volume::Headphones, true);
        if (haveSpeaker)
            Volume::setOn(Volume::Speaker, false);
    } else {
        if (haveSpeaker)
            Volume::setOn(Volume::Speaker, Settings::instance().speakerWanted());
        if (haveHeadphones)
            Volume::setOn(Volume::Headphones, false);
    }

    /*
     * The Settings page caches the two toggles it drew, and it is the one page
     * that would sit there showing the state from before the plug.  Re-entering
     * it is how every other page in this shell refreshes, so that is what this
     * does -- and only when it is actually on screen, because a page that is not
     * visible rebuilds itself the next time it is walked into anyway.
     */
    if (m_settings && m_current == m_settings)
        m_settings->onEnter();

    if (!announce)
        return;

    if (plugged)
        toast(tr("Headphones in -- speaker off"), 2600);
    else if (haveSpeaker && Settings::instance().speakerWanted())
        toast(tr("Headphones out -- speaker on"), 2600);
    else
        toast(tr("Headphones out"), 2600);
}

void Dashboard::refreshInputSummary()
{
    if (!m_info || !m_pad)
        return;
    m_info->setInputSummary(
        m_pad->deviceCount() == 0
            ? tr("no /dev/input/event* -- nothing to navigate with")
            : QString("%1: %2").arg(m_pad->deviceCount()).arg(m_pad->deviceNames().join(", ")));
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
    /* It used to be lifted clear of the dock; with the dock gone it sits on the
     * same bottom margin every page's content does, which is where the eye
     * already is after the last row of cards. */
    m_toast->move(qMax(0, (width() - m_toast->width()) / 2),
                  qMax(0, height() - Theme::Margin - m_toast->height()));
    m_toast->show();
    m_toast->raise();
    m_pointer->raise();
    m_toastTimer->start(ms);
}

namespace {

/*
 * ── ITS OWN PROCESS GROUP, AND WHY A TASK IS NOT A PROCESS ───────────────────
 *
 * The Browser card starts a shell script, which execs xinit, which forks an X
 * server and a second shell script, which starts a window manager, an on-screen
 * keyboard, a pad bridge and Firefox.  That is one TASK and nine processes, and
 * stopping the one this program has a pid for would stop the shell and leave the
 * X server drawing over the switcher.
 *
 * setpgid(0, 0) between fork and exec makes the whole tree one group -- nothing
 * in it calls setsid, which was checked rather than assumed -- so
 * kill(-pgid, ...) reaches all of it.  It cannot be done from this side of the
 * fork: setpgid() on a child that has already exec'd fails with EACCES, and
 * started() is emitted after the exec, so by the time there is a pid to use it is
 * too late to use it.
 *
 * Hence a subclass.  setupChildProcess() is Qt 5's hook for exactly this and it
 * is the only one Qt 5 has -- setChildProcessModifier(), which would have saved
 * these eight lines, is Qt 6, and the armhf chroot has 5.15.15.  The override
 * runs in the forked child, so it may call nothing that is not
 * async-signal-safe: setpgid is one syscall and it is on the list.
 *
 * No Q_OBJECT: this adds no signals, slots or properties, so it needs no
 * metaobject of its own, and connecting to QProcess's own signals through a base
 * pointer works exactly as it did.
 */
class GroupLeaderProcess : public QProcess
{
public:
    explicit GroupLeaderProcess(QObject *parent = nullptr)
        : QProcess(parent) {}

protected:
    void setupChildProcess() override { ::setpgid(0, 0); }
};

}

/*
 * ── A LAUNCH DOES NOT STOP THE EVENT LOOP ANY MORE ───────────────────────────
 *
 * This was QProcess::execute, which is start() plus a blocking wait, and the wait
 * was the whole problem.  For as long as the child was up, mixdash was a process
 * sitting in waitpid(): no timers, so the console guard never ran and a unit
 * started behind the child could put fbcon back over it with nothing here to
 * notice; no signal delivery through the event loop; and -- the reason this came
 * up -- nothing could animate, so there was no way to tell a child that was slow
 * to start from a shell that had hung.  It also meant a child that never exited
 * was a dashboard that never came back, with no path in the program that could
 * even say so.
 *
 * So the child is started and the loop keeps turning.  Two things have to be true
 * for that to be an improvement rather than a mess:
 *
 *   THE SHELL MUST NOT DRAW.  The child owns the framebuffer -- that is what
 *   taking the panel means -- and every repaint here is a memcpy over whatever it
 *   is showing.  With the loop stopped that was free; now it is setUpdatesEnabled
 *   (false) on the top level, which suppresses paint events and the linuxfb flush
 *   for this window and every child of it, the status-bar clock and the console
 *   guard's update() included.
 *
 *   THERE MUST BE ONLY ONE ON THE GLASS.  One panel, one set of input devices.
 *   That sentence used to end "ONLY ONE" and the test for it was m_child being
 *   null, which meant a second card could not be opened at all -- switcher.h is
 *   the long version of why that was the wrong rule to draw.  Several children
 *   run now; exactly one is in front and every other one is stopped by the
 *   kernel, so the count of programs able to draw is still exactly one.
 *
 *   AND THERE IS A CEILING.  kMaxTasks, because each background task holds a
 *   framebuffer's worth of pixels (panel.h) and, far more expensively, its own
 *   address space: this board has 946 MB and a browser session on its own is a
 *   third of it.  Four is what the switcher can show without scrolling and it is
 *   more than this device can comfortably hold at once.
 */
void Dashboard::launch(const QString &title, const QString &exe, const QStringList &args)
{
    if (exe.isEmpty() || !QFileInfo(exe).isExecutable()) {
        toast(tr("%1 is not on this card").arg(title));
        return;
    }

    /*
     * THE SAME CARD TWICE IS THE SAME PROGRAM, NOT A SECOND ONE.
     *
     * Pressing a card whose program is already running switches to it.  Starting
     * a second copy would be wrong for the browser (two X servers on one
     * framebuffer) and merely wasteful for everything else, and "it is already
     * running, go and find it in the switcher" is an error message for a problem
     * the shell can simply solve.
     */
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i].exe != exe)
            continue;
        setForeground(i);
        return;
    }

    if (m_tasks.size() >= kMaxTasks) {
        toast(tr("Too many things are running -- close one first"));
        return;
    }

    toast(tr("Starting %1").arg(title), 60000);
    /*
     * Painted now, and not left to the next trip round the loop: updates are about
     * to be switched off, and the last thing on the glass before the child draws
     * should be the sentence naming what is starting.
     */
    m_toast->repaint();
    QCoreApplication::processEvents();

    /*
     * Anything already in front is stopped before the new one is started, so that
     * the two never overlap on the panel even for the length of an exec.  -1 is
     * not used here: this is on its way to the new task, not back to the grid.
     */
    stopForeground();

    /* The child gets the input devices, and this keeps reading them for one
     * button -- see the watch-mode comment in joypad.h. */
    m_pad->setWatching(true);
    m_pointer->sleep();

    /*
     * The ring turns for as long as exec does.  That is a blink on a warm page
     * cache and several seconds on a cold one -- a big binary comes off this SD
     * card at a few megabytes a second and ld.so touches every page of it -- and
     * the two used to be indistinguishable from a hang, because the old
     * QProcess::execute() stopped the event loop for the whole of it.  The pointer
     * has just been put to sleep, so the plain arc is the right one of the two.
     */
    m_busy->setPointerStyle(false);
    m_busy->start(title);

    /* Its own process group, so the whole tree can be stopped and continued as
     * one thing -- see GroupLeaderProcess above. */
    QProcess *child = new GroupLeaderProcess(this);
    /*
     * Inherited, which is what QProcess::execute did and what the child wants: its
     * output belongs wherever this program's own went -- the console before the
     * first frame, /run/j36/mixdash.log after it -- and the alternative is Qt
     * quietly accumulating a game's stdout in a buffer nobody reads.
     */
    child->setProcessChannelMode(QProcess::ForwardedChannels);

    /*
     * MIXDASH_PID, so a child that has taken the pad away from this program can
     * still ask for the switcher.  Only j36-padx does that today; the variable is
     * set for every child because a rule with one exception is a rule that gets
     * the exception wrong.  See switcher.h.
     */
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MIXDASH_PID"),
               QString::number(QCoreApplication::applicationPid()));
    child->setProcessEnvironment(env);

    connect(child, &QProcess::errorOccurred, this, [this, child](QProcess::ProcessError e) {
        /* Only the one that has no exit code to come.  Crashed, Timedout and the
         * write errors all still reach finished(), and handling them here as well
         * would report the same child twice. */
        if (e != QProcess::FailedToStart)
            return;
        const int i = indexOfTask(child);
        if (i >= 0)
            childDone(i, tr("%1 would not start").arg(m_tasks[i].title));
    });
    connect(child, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, child](int code, QProcess::ExitStatus status) {
        /*
         * Looked up by pointer, every time.  The index this task had when it was
         * started is not the index it has now -- closing an earlier one shifts
         * every task after it -- and a captured index would report the wrong
         * program's exit and remove the wrong row.
         */
        const int i = indexOfTask(child);
        if (i < 0)
            return;
        const QString what = m_tasks[i].title;
        if (status == QProcess::CrashExit)
            childDone(i, tr("%1 crashed").arg(what));
        else if (code != 0)
            childDone(i, tr("%1 exited %2").arg(what).arg(code));
        else
            childDone(i, tr("%1 exited").arg(what));
    });

    connect(child, &QProcess::started, this, [this, child]() {
        /*
         * The child is running and every pixel is its business from here.  Updates
         * go off HERE and not straight after start(): between the two calls this
         * dashboard is still the only thing on the glass, and a ring that cannot
         * be painted is a ring that does not turn.
         */
        m_busy->stop();
        setUpdatesEnabled(false);
        const int i = indexOfTask(child);
        if (i < 0)
            return;
        /*
         * The pid is only real once the fork has happened, which is what started()
         * means.  It is the group id too, because setupChildProcess() made the
         * child its own leader.
         */
        m_tasks[i].pgid = child->processId();
        m_fg = i;
        /* Something outside this process can ask for the switcher now, and only
         * now.  See switcher.h. */
        m_requestTimer->start();
    });

    Task task;
    task.proc = child;
    task.title = title;
    task.exe = exe;
    m_tasks.append(task);

    child->start(exe, args);
}

void Dashboard::childDone(int index, const QString &message)
{
    if (index < 0 || index >= m_tasks.size())
        return;

    if (QProcess *p = m_tasks[index].proc.data()) {
        p->disconnect(this);
        p->deleteLater();
    }
    const bool wasForeground = (m_fg == index);
    m_tasks.remove(index);

    /*
     * The indices after the removed one have all moved down by one.  m_fg is an
     * index, so it moves with them -- and the task that was in front is the one
     * case where it does not point at anything any more.
     */
    if (wasForeground)
        m_fg = -1;
    else if (m_fg > index)
        --m_fg;

    /* Either it never started or it has stopped; either way nothing is loading.
     * Idempotent, so the usual case -- started() took the ring down already --
     * costs nothing here. */
    m_busy->stop();

    /*
     * A background task exiting changes the switcher's list and nothing else.  It
     * is not on the glass, it did not have the pad, and taking the dashboard out
     * of the state a DIFFERENT task put it in would be a game losing its input
     * because something finished behind it.
     */
    if (!wasForeground) {
        refreshSwitcher();
        toast(message);
        return;
    }

    /*
     * It was the one in front, so the panel is free.  Where it goes depends on
     * whether anything else is left: with a background task waiting this could
     * switch straight to it, and deliberately does not -- a program ending should
     * put the user somewhere they chose to be, not somewhere the exit order
     * chose for them.
     */
    setForeground(-1);
    toast(message);
}

/*
 * ── WHO OWNS THE PANEL ───────────────────────────────────────────────────────
 *
 * The single rule this whole feature rests on: ONE TASK RUNS, EVERY OTHER TASK IS
 * STOPPED.  Not throttled, not asked to idle -- SIGSTOP, so the kernel will not
 * schedule it and it cannot write a pixel into a framebuffer it still has mapped.
 * That is what makes a compositor unnecessary on a board that has no compositor.
 *
 * The order inside this function is the part that matters, and every step of it
 * is there because the other order is visibly wrong:
 *
 *   1. Stop what is in front, and keep its frame.  Stop first, copy second: a
 *      running program can be halfway through drawing and the copy would tear.
 *   2. Turn updates off before anything is put back, and on again only when this
 *      dashboard is the thing that should be drawing.  Qt flushes dirty regions,
 *      so a repaint that arrives after step 3 would land on top of a resumed
 *      program's screen.
 *   3. Put the incoming task's frame back, THEN continue it.  panel.h says why:
 *      a game redraws immediately and does not need this, an X server redraws
 *      only damage and would otherwise leave the switcher on the glass for as
 *      long as the page sat still.
 *   4. Hand the pad over last, so no button can be delivered to a task that is
 *      not yet running.
 */
void Dashboard::setForeground(int index)
{
    if (index >= m_tasks.size())
        index = -1;

    stopForeground();

    /* The switcher, if it is up, is coming down whatever was chosen -- including
     * the case where what was chosen is what was already in front. */
    if (m_switcher->isVisible())
        m_switcher->dismiss();
    m_switcherWas = -1;

    if (index < 0) {
        /*
         * Back to the dashboard.  It draws every pixel of itself, so nothing is
         * restored here -- and a frame kept for the dashboard would be a frame of
         * a grid that may have changed while a game was in front.
         */
        m_fg = -1;
        m_requestTimer->stop();
        m_pad->setWatching(false);

        /*
         * The child had the framebuffer, so every pixel is suspect.  A child that
         * set a mode of its own through /dev/dri/card0 has taken the scanout away
         * from the LK's simple-framebuffer as well, and no amount of repainting
         * here brings that back -- which is worth knowing when a launch comes
         * back to a black panel.
         *
         * setUpdatesEnabled(true) marks this widget dirty by itself; the children
         * were never dirtied while updates were off, so they are asked
         * explicitly.
         */
        setUpdatesEnabled(true);
        update();
        if (m_current)
            m_current->update();
        return;
    }

    Task &task = m_tasks[index];

    /* Off before the frame goes back, and it stays off for as long as this task
     * is in front.  See the numbered note above. */
    setUpdatesEnabled(false);
    Panel::restore(task.frame);
    task.frame.clear();

    if (task.stopped && task.pgid > 0) {
        ::kill(-(pid_t)task.pgid, SIGCONT);
        task.stopped = false;
    }

    m_fg = index;
    m_pointer->sleep();
    m_pad->setWatching(true);
    m_requestTimer->start();
}

/*
 * Freeze whatever is in front, if it is a task.  Split out of setForeground()
 * because launch() needs exactly this half and none of the rest: it is on its way
 * to a program that does not exist yet, so there is nothing to hand the panel to.
 */
void Dashboard::stopForeground()
{
    if (m_fg < 0 || m_fg >= m_tasks.size())
        return;

    Task &task = m_tasks[m_fg];
    if (task.pgid > 0 && !task.stopped) {
        ::kill(-(pid_t)task.pgid, SIGSTOP);
        task.stopped = true;
        /* AFTER the stop.  panel.h: a program that is still running can be in
         * the middle of a frame, and what is wanted is the last thing the user
         * actually saw. */
        task.frame = Panel::grab();
    }
    m_fg = -1;
}

int Dashboard::indexOfTask(QProcess *proc) const
{
    for (int i = 0; i < m_tasks.size(); ++i)
        if (m_tasks[i].proc.data() == proc)
            return i;
    return -1;
}

void Dashboard::closeTask(int index)
{
    if (index < 0 || index >= m_tasks.size())
        return;

    Task &task = m_tasks[index];
    if (task.pgid <= 0)
        return;

    /*
     * SIGCONT FIRST, and the header says why: a stopped process cannot run a
     * signal handler, so SIGTERM on its own would sit pending against something
     * that never gets to act on it.  The task would vanish from the switcher and
     * carry on existing.
     *
     * Then SIGTERM, and then nothing.  There is no SIGKILL after a timeout here:
     * the browser session's own teardown gives its X server thirty seconds and
     * kills it itself, and a shell that decided a program had had long enough
     * would be second-guessing a script that already handles it.  A task that
     * genuinely ignores SIGTERM stays in the list, which is at least honest.
     */
    ::kill(-(pid_t)task.pgid, SIGCONT);
    task.stopped = false;
    ::kill(-(pid_t)task.pgid, SIGTERM);

    /*
     * The row is not removed here.  finished() is what removes it, through
     * childDone(), and letting the exit do it means the list says "still there"
     * for the half-second a browser takes to tear an X server down -- which is
     * true, and better than a row that disappears while the program behind it is
     * still on the way out.
     */
    toast(tr("Closing %1").arg(task.title));
    refreshSwitcher();
}

/*
 * ── PUTTING THE SWITCHER UP ──────────────────────────────────────────────────
 *
 * The contract switcher.h describes, in the order it has to happen: stop the task
 * in front FIRST, so that when the overlay paints there is nothing else writing
 * to /dev/fb0 and ordinary Qt painting is enough.  Then take the pad back, since
 * the switcher is driven by it and the task that had it is no longer running.
 *
 * Reachable two ways and they meet here: Joypad::switcherRequested (FN held) and
 * SwitcherRequest::take() (SIGUSR1 from a child that grabbed the pad).
 */
void Dashboard::showSwitcher()
{
    if (m_switcher->isVisible())
        return;

    /* Remembered before anything is stopped, because stopForeground() clears
     * m_fg and cancelling has to put back what was actually in front. */
    m_switcherWas = m_fg;
    stopForeground();

    m_requestTimer->stop();
    /* Full reporting again: the switcher is driven by the D-pad, A, B and Menu,
     * none of which arrive in watch mode. */
    m_pad->setWatching(false);

    /*
     * Shown while updates are still off, so the dashboard underneath is never
     * painted for the one frame between the two calls.  The switcher is opaque
     * and covers the panel, so the repaint that follows draws it and nothing
     * else.
     */
    m_switcher->setGeometry(rect());
    m_switcher->open(switcherRows(), m_switcherWas + 1);
    setUpdatesEnabled(true);
    update();
    /* The arrow belongs on top of every overlay, the same way the toast puts it
     * back.  Asleep behind a task, awake if this was opened from the grid. */
    m_pointer->raise();
}

/*
 * The rows, and the mapping between a row and a task: row 0 is this dashboard,
 * row n+1 is m_tasks[n].  One function so the two directions cannot disagree --
 * setForeground(row - 1) is the whole of the inverse.
 */
QVector<Switcher::Entry> Dashboard::switcherRows() const
{
    QVector<Switcher::Entry> rows;

    Switcher::Entry home;
    home.title = tr("Dashboard");
    home.detail = m_tasks.isEmpty() ? tr("nothing else is running")
                                    : tr("cards and settings");
    /* The one row that cannot be closed.  A flag rather than an index test, so
     * switcher.cpp never has to know that row 0 is special. */
    home.closable = false;
    rows.append(home);

    for (int i = 0; i < m_tasks.size(); ++i) {
        Switcher::Entry e;
        e.title = m_tasks[i].title;
        /*
         * "running" is the truth for the one in front and for one that is about
         * to be: a stopped task is stopped because it is not being looked at, and
         * telling the user their browser is "stopped" invites them to think it
         * crashed.  The word for the state that matters is which one is in front,
         * and the highlight already says that.
         */
        e.detail = (i == m_fg || i == m_switcherWas) ? tr("in front") : tr("waiting");
        rows.append(e);
    }
    return rows;
}

void Dashboard::refreshSwitcher()
{
    if (m_switcher && m_switcher->isVisible())
        m_switcher->refresh(switcherRows());
}

/*
 * WHY POWER OFF DOES NOT GO THROUGH launch().
 *
 * launch() is built for a child that comes back: it toasts "Starting", blocks the
 * event loop on QProcess::execute, and toasts an exit code afterwards.  poweroff
 * does come back, and immediately, because all it does is ask PID 1 -- and then
 * the dashboard carries on painting for the twenty or thirty seconds systemd
 * needs to stop the units, write the shutdown log onto BOOT and unmount the card.
 * What is on the glass for that whole window is a live menu that answers nothing,
 * followed by a picture that stops moving.  That is indistinguishable from a hang,
 * and it is most of the reason a power-off on this board has ever been reported
 * as one.
 *
 * So: a curtain that says what is happening, painted and flushed BEFORE the
 * request goes out; the pad and the pointer put away for good, because there is
 * nothing left to press; and the process started detached, because nothing here
 * wants its exit code and there is no event loop to come back to.
 *
 * The second line of the curtain is not padding.  There is no power-path FET on
 * this PMIC, so VBAT is VSYS: with a charger in, VBUS holds the system rail up
 * and the RTC cannot pull it down.  The driver restarts the board instead of
 * halting it warm -- see j36_mt6592_pmic.c -- so a power-off attempted on the
 * charger looks like a reboot, and the one thing the user needs to know is that
 * unplugging fixes it.
 */
void Dashboard::powerOff()
{
    const QString exe = firstExisting(QStringList() << "/sbin/poweroff"
                                                    << "/usr/sbin/poweroff");
    if (exe.isEmpty()) {
        toast(tr("poweroff is not on this card"));
        return;
    }

    m_toastTimer->stop();
    m_toast->hide();

    QLabel *curtain = new QLabel(this);
    curtain->setAlignment(Qt::AlignCenter);
    curtain->setWordWrap(true);
    curtain->setStyleSheet(
        "QLabel { background: #0A0B10; color: #E8EAF2; font-size: 17px; }");
    curtain->setText(tr("Powering off\n\nIf the board comes back up, unplug the charger and try again."));
    curtain->setGeometry(rect());
    curtain->show();
    curtain->raise();
    /* Painted here, not on the next trip round the loop: there is no next trip. */
    curtain->repaint();
    QCoreApplication::processEvents();

    /*
     * THE PAD IS DISCONNECTED, not suspended.  There is no setSuspended() any more
     * -- watch mode keeps reading, because a task switcher whose button is not
     * read is a switcher nobody can reach -- and "keeps reading" is exactly what
     * this one path does not want.  Everything after this line is a machine on its
     * way down behind a curtain, and a switcher opened over it, or a card grid
     * walked under it, would both be the shell answering buttons that can no
     * longer lead anywhere.
     *
     * Severed and not re-connected, because this function has no other side: the
     * request has gone to PID 1 and the only way back is a power cycle.
     */
    m_pad->disconnect(this);
    m_pad->setWatching(true);
    m_pointer->sleep();
    QProcess::startDetached(exe, QStringList());
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
    case SettingsPage::OpenRegion:
        push(m_region);
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
        /*
         * Same two-press gate as Power off, and it survives the switcher for a
         * smaller reason than it was written for.  It used to say the launch could
         * not be undone; it can now -- holding FN brings this back.  What it is
         * still worth saying is that the panel is about to change hands and how to
         * change it back, which is the one thing nothing on the glass can tell you
         * once the child is drawing.  The warning has to be shown before the child
         * starts, for exactly that reason.
         */
        if (entry.confirm && m_armedExe != entry.exe) {
            m_armedExe = entry.exe;
            toast(tr("%1 takes the panel.  Hold FN to come back.\nPress A again to run it.")
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
        /* No path and no scope: the whole filesystem, opened wherever the browser
         * was left.  openAt() says why it is remembered. */
        m_files->openAt(QString(), QString());
        push(m_files);
        break;
    case InternalVolume: {
        /*
         * The mount point is looked up NOW rather than carried on the card, because
         * the card outlives the truth by up to a rescan: a stick pulled out is a
         * card that is still on the grid until Disks notices and the grid is
         * rebuilt, and pressing it in that window must say so rather than open a
         * browser on a directory that is not a filesystem any more.
         */
        const Disk *v = Disks::instance().byKey(entry.key);
        if (!v) {
            toast(tr("%1 is no longer plugged in").arg(entry.title), 3000);
            break;
        }
        m_files->openAt(v->mountPoint, v->mountPoint);
        push(m_files);
        if (v->readOnly)
            toast(tr("%1 is mounted read-only").arg(v->name()), 3000);
        break;
    }
    case InternalTerminal:
        push(m_terminal);
        break;
    case InternalBrowser: {
        /*
         * ONLY REACHED WITHOUT X.  buildPages() sets browser.exe when the card
         * carries the graphical session, and activate() launches an entry with an
         * exe before it ever looks at `internal'.  So everything below is the
         * fallback: a card whose rootfs has links2 and no X server.
         *
         * The same door the Packages page uses for `apt install': open the
         * Terminal and type a line into the shell that is already running in it.
         * Nothing here launches a process -- the pty does, which is what keeps the
         * pad, the on-screen keyboard and the framebuffer where they are.
         *
         * The start page is the shipped one if the payload is on this card and a
         * search engine if it is not, so the card still opens something on a card
         * whose /opt/mixos never got unpacked.
         *
         * `cd ... || cd ~' and HOME="$PWD" as a PREFIX: the assignment applies to
         * links2 and to nothing after it, so the terminal the user backs out into
         * still has root's HOME.  A bare `HOME=...' would have quietly re-homed
         * the shell for the rest of the session.
         */
        const QString start =
            QFileInfo(QString::fromLatin1(kBrowserStart)).exists()
                ? QStringLiteral("file://") + QString::fromLatin1(kBrowserStart)
                : QString::fromLatin1(kBrowserFallbackUrl);
        push(m_terminal);
        m_terminal->runCommand(QStringLiteral("cd /home/virtua 2>/dev/null || cd ~; HOME=\"$PWD\" ")
                               + QString::fromLatin1(kBrowserExe) + QLatin1Char(' ')
                               + shellQuote(start));
        /*
         * After the push, so it lands on the terminal the user is now looking at.
         * The card is doing the right thing here -- links2 is what this rootfs can
         * run -- but "the browser has no JavaScript" is not a diagnosis anybody can
         * act on, and the sentence below is: it names the missing package, and the
         * Packages page two cards away is where that is fixed.
         */
        toast(tr("Text browser: %1").arg(graphicalBrowserMissing()), 5000);
        break;
    }
    case InternalWifi:
        push(m_wifi);
        break;
    case InternalSharing:
        push(m_sharing);
        break;
    case InternalPackages:
        push(m_packages);
        break;
    case InternalDiagnostics:
        push(m_diagnostics);
        break;
    case InternalMedia:
        push(m_media);
        break;
    case InternalSettings:
        push(m_settings);
        break;
    case InternalInfo:
        m_info->refresh();
        push(m_info);
        break;
    case InternalPoweroff:
        if (m_armed != InternalPoweroff) {
            m_armed = InternalPoweroff;
            toast(tr("Press A again to power off"));
            return;
        }
        m_armed = InternalNone;
        powerOff();
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

/*
 * A file chosen in the browser.  Media is asked first because it is the only
 * thing here that can actually SHOW a file; everything else is handed to the
 * Terminal, which at least lets the user decide what to do with it.
 */
void Dashboard::onOpenRequested(const QString &path)
{
    const QFileInfo info(path);

    /* Pushed, not switched to: Media is on the stack ABOVE the browser now, so B
     * out of the player lands back on the directory the file was chosen from
     * instead of dropping the user at the grid. */
    if (m_media->openPath(path)) {
        push(m_media);
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

void Dashboard::onNav(int action, bool repeat)
{
    /*
     * The volume keys come before EVERYTHING, the keyboard included.  They are two
     * physical keys on the side of the case, and on every handheld ever made they
     * do the same thing no matter what is on the screen: while a film is playing,
     * while a name is being typed, inside a full-screen game.  A page that could
     * swallow them would be a page you have to leave to turn the sound down, which
     * is exactly the thing the keys exist to avoid.
     *
     * Two hardware keys, one line each, and no page ever hears about them.
     */
    if (action == Joypad::NavVolumeUp || action == Joypad::NavVolumeDown) {
        bool muted = false;
        const int level = Volume::nudge(action == Joypad::NavVolumeUp ? 1 : -1, &muted);
        /* Decided before the flash, so flash() already knows whether it is drawing
         * or being drawn and never shows a Qt bar over a film for one frame. */
        m_volumeBar->setRedirected(m_media && m_media->glOwnsScreen());
        m_volumeBar->flash(level, muted);
        /* flash() raised the bar over everything, the cursor included.  The arrow
         * goes back on top the same way the toast puts it back.  Not while
         * redirected: nothing was raised, and raising the pointer over a film is
         * the same memcpy-over-the-GPU problem one layer down. */
        if (!m_volumeBar->isRedirected())
            m_pointer->raise();
        return;
    }

    /*
     * The switcher is above even the keyboard, and it is the one overlay that
     * swallows everything rather than passing on what it does not want.  It has
     * to: the thing underneath it is a stopped program or a page that is not on
     * the glass, and acting on either would be acting on something the user
     * cannot see.  handleNav() returns false only when it is not up at all.
     */
    if (m_switcher->isVisible()) {
        if (m_switcher->handleNav(action))
            return;
    }

    /* The keyboard is an overlay, so it gets first refusal: Back closes it rather
     * than popping the page being typed into. */
    if (m_keyboard->isVisible()) {
        if (m_keyboard->handleNav(action))
            return;
    }

    /*
     * FN (BTN_MODE, the MENU key on the matrix) used to be caught right here and
     * answered with a toast telling the reader to go and use the Power off card.
     * That is gone.  It was a dead end dressed up as help: the button did nothing,
     * every press cost a toast, and the sentence was advice about a DIFFERENT
     * control rather than an effect of the one that was pressed.  A button whose
     * only behaviour is to describe another button is a button with no behaviour.
     *
     * A TAP OF FN IS STILL THE PAGE'S, and a HOLD never arrives here at all.
     * Joypad splits the two -- see the switcherRequested() comment in joypad.h --
     * so holding it goes straight to showSwitcher() through a signal of its own,
     * and what reaches this function is only ever the tap.  The Terminal makes
     * that tap the interrupt key, which is the thing a handheld with no Ctrl key
     * cannot otherwise do; a page with no use for it lets it fall through to the
     * switch below, where it lands on `default' and nothing happens.  Nothing
     * happening is the honest outcome and it is silent.
     *
     * Quitting is still not among the options, and NavQuit keeps its name only
     * because it is what a bring-up keyboard's Q key has always sent.
     * mixdash.service is Restart=on-failure, so a clean exit is a clean stop and
     * systemd does not bring the dashboard back: qApp->quit() here would be a
     * one-way door out of the shell whose only way back is a power cycle.
     */

    PageWidget *page = current();
    if (page && page->handleNav(action))
        return;

    /*
     * Whatever the page did not want.
     *
     * THIS SWITCH USED TO BE THE TAB BAR, and most of it has gone with the dock.
     * L1/R1 stepped the root page, and left or right at the edge of a root page
     * did the same thing so that a D-pad on its own could get off the screen it
     * started on -- that gesture is why `repeat' is a parameter of this function.
     * There is one root now and every page above it is left with B, so the
     * shoulders belong to whatever is on the glass (the grid jumps to its ends
     * with them) and a direction the page refused is a direction with nothing
     * left to mean.  Joypad still reports the flag and this still takes it,
     * because the flag is true of the signal whether or not the shell has a use
     * for it and a slot that dropped it would have to be reconnected the day one
     * appears.
     */
    Q_UNUSED(repeat);

    switch (action) {
    case Joypad::NavMenu:
        /*
         * Menu is the settings button when the page in front has no use for it.
         * It is a toggle: pressing it inside Settings takes you back out, rather
         * than pushing a page that is already on the stack and leaving B as the
         * only way to undo a button that opened it.
         */
        if (current() == m_settings)
            pop();
        else
            push(m_settings);
        return;
    case Joypad::NavBack:
        if (!m_stack.isEmpty())
            pop();
        return;
    default:
        break;
    }
}

/*
 * A button let go of, delivered to whatever is on the glass and nowhere else.
 *
 * NO FALLBACK, unlike onNav().  A press the page refused can still open Settings
 * or pop the stack up there; a release the page refused is nothing at all,
 * because there is no gesture in this shell that a release alone should start.
 * If the page has since
 * changed -- a card was launched on the press and the launch pushed a page -- the
 * release lands on the new page, which will not know the action and will ignore
 * it.  That is the correct outcome and it is why this does not remember who was
 * in front at press time: remembering would mean delivering a release to a page
 * that is no longer visible.
 *
 * The volume keys are filtered out for the same reason they are intercepted in
 * onNav(): no page ever hears about them, and a release is still hearing about
 * them.
 */
void Dashboard::onNavReleased(int action)
{
    if (action == Joypad::NavVolumeUp || action == Joypad::NavVolumeDown)
        return;
    if (m_keyboard->isVisible() || m_switcher->isVisible())
        return;

    PageWidget *page = current();
    if (page)
        page->handleNavRelease(action);
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

        const int action = navForKey(key->key());
        if (action != Joypad::NavNone) {
            /* X11 and Wayland both synthesise a held key as a stream of presses
             * with this flag set, which is the same thing Joypad's own repeat
             * means -- so the edge-of-page gesture behaves the same on both. */
            onNav(action, key->isAutoRepeat());
            return true;
        }
    }

    /*
     * The other half of a press, so that a workstation build can tell a tap from
     * a hold the way the pad can.  Without this the card grid could be picked up
     * with a long press on the device and never put down on a desk, because the
     * press that arms the launch is only ever completed by a release.
     *
     * AUTO-REPEAT RELEASES ARE DROPPED.  X11 delivers a held key as an endless
     * release/press pair with isAutoRepeat() set on both, and every one of those
     * releases would look like the user letting go -- which would fire the tap
     * action over and over while the key was still down.
     *
     * The wantsKeys() branch above has no counterpart here on purpose: pages that
     * asked for raw keys are given presses only, so there is no release of theirs
     * to steal.  The guard below is what keeps a release from arriving as a nav
     * action while the Terminal is being typed into.
     */
    if (event->type() == QEvent::KeyRelease) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        PageWidget *page = current();
        const bool typing = m_keyboard->isVisible() || (page && page->wantsKeys());
        const bool escapes = key->key() == Qt::Key_Escape;
        if (!key->isAutoRepeat() && (escapes || !typing)) {
            const int action = navForKey(key->key());
            if (action != Joypad::NavNone) {
                onNavReleased(action);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
