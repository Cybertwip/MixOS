/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * dashboard.h -- the shell: one grid of cards, a stack above it, one input path.
 *
 * WHAT CHANGED AND WHY.  The first version of this file was four pages and a
 * chain of `if (m_page == 1)' in onNav().  That was honest at four pages and
 * unreadable at twelve, and twelve is what this build has.  Every page answers
 * handleNav() for itself and the shell only decides WHICH page is on screen.
 *
 * AND THEN THE TABS WENT.  There were three root pages -- Apps, Media, Settings --
 * with a dock along the bottom to switch between them, the shoulder buttons doing
 * the same thing, and the edges of the D-pad doing it a third time.  That is three
 * gestures and 56 px of furniture to reach two pages that are perfectly good
 * cards.  Now there is ONE root, the card grid, and everything else -- Media and
 * Settings included -- is pushed on top of it and popped with B.  One way in, one
 * way out, and the only thing on the glass besides the page is the status bar.
 *
 * The shell's remaining jobs are the ones no page can do alone:
 *
 *   - the stack.  Back falls through a page that does not consume it, and the
 *     shell pops.  That is the whole navigation model, and now it is the only one.
 *   - the volume.  Two keys on the side of the case that no page ever sees.
 *   - launching.  A child process needs the pad suspended, the panel warned
 *     about and a toast afterwards; a page cannot do that to itself.
 *   - the keyboard overlay.  It is one widget shared by every page that asks for
 *     text, and the answer goes back to whichever page asked.
 *   - the pointer.  One cursor over every page, above everything, asleep while a
 *     page owns the whole panel.
 *   - the chrome.  The status bar goes away for a page that says
 *     wantsFullscreen(), and comes back when it stops saying it.
 *   - the hotplug.  A mouse or a keyboard arriving is announced here, because the
 *     page in front has no idea what is plugged into the board.
 */
#ifndef MIXDASH_DASHBOARD_H
#define MIXDASH_DASHBOARD_H

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "widgets.h"

class Busy;
class DiagnosticsPage;
class DisplayPage;
class FilesPage;
class Joypad;
class Keyboard;
class RegionPage;
class MediaPage;
class MousePage;
class PackagesPage;
class Pointer;
class QLabel;
class QProcess;
class QTimer;
class SettingsPage;
class SharingPage;
class TerminalPage;
class VolumeOverlay;
class WifiPage;

/*
 * THE FILE BROWSER USED TO BE DECLARED HERE and is now files.h.
 *
 * It was one QListView and a title bar while there was one filesystem to look at.
 * There is not: a USB stick appears under /media without anybody asking, gets a
 * card on the grid, and opens a browser confined to itself -- which needs a places
 * panel, an address bar, a search box, an info panel and a scope, and none of that
 * belongs in the shell's own header.
 */

class Dashboard : public QWidget
{
    Q_OBJECT

public:
    /*
     * A card with no exe does one of these instead of starting a process.
     *
     * InternalMedia and InternalSettings are back.  They were deleted when Media
     * and Settings were dock tabs and the cards that led to them did nothing but
     * switch tab -- two doors into one room.  There is no dock now, so these two
     * are the only door, and they push their page like every other card here.
     *
     * InternalConsole is NOT back, and that one was removed for a worse reason: it
     * hung the dashboard.  The note where the card used to be built, in
     * buildPages(), says why.
     *
     * InternalReboot is gone too, for no reason worse than the board having a power
     * button that already does it.  A card that duplicates hardware is a card you
     * can hit by accident.
     *
     * InternalBrowser is an odd one out and deliberately so: it is not a page of
     * its own, it is the Terminal page opened on a command -- and it is now the
     * FALLBACK half of the Browser card, taken only when there is no X server on
     * the rootfs to run a graphical browser in.  With one, the same card carries an
     * exe and goes out through launch() like the cards that are programs.
     * buildPages() says why both.
     *
     * APPENDED, NEVER INSERTED.  These values are compared against m_armed and
     * passed around as ints; a new one in the middle renumbers every card below it
     * for no gain.  The order cards appear in is buildPages()' order, not this one.
     */
    enum Internal {
        InternalNone = 0,
        InternalFiles,
        InternalTerminal,
        InternalWifi,
        InternalSharing,
        InternalPackages,
        InternalDiagnostics,
        InternalMedia,
        InternalSettings,
        InternalInfo,
        InternalPoweroff,
        InternalBrowser,
        /* A mounted USB volume.  The card carries no path -- the key is the handle,
         * and Volumes::byKey turns it back into a mount point at the moment it is
         * pressed, which is the only moment the answer is known to be current. */
        InternalVolume
    };

    explicit Dashboard(QWidget *parent = nullptr);

signals:
    /*
     * Emitted once, from the first paintEvent.  main() waits for it before it takes
     * the console away from fbcon: until there is a frame worth showing, kernel
     * messages and this program's own trace are the more useful thing on the glass.
     */
    void firstPainted();

public slots:
    /* `repeat' is Joypad's: false for a press, true for an autorepeat of one still
     * being held.  Nothing in the shell reads it since the tabs went -- see onNav. */
    void onNav(int action, bool repeat);
    /* The same action let go of.  Delivered to the current page and nowhere else;
     * one page listens.  See onNavReleased in dashboard.cpp. */
    void onNavReleased(int action);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    /* Installed on the application so one function handles the keypad, the
     * joystick and a USB keyboard, whichever child widget happens to have focus. */
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onAppActivated(int index);
    void onOpenRequested(const QString &path);

    /*
     * A disk was plugged in or pulled out, so the grid has a card more or a card
     * fewer.  The whole grid is rebuilt rather than one card being inserted: the
     * cards are built in one function from one list, and a second path that adds
     * one card would be a second place for the arrangement to be applied.
     */
    void onVolumesChanged();
    /* Eject was chosen from a card's long-press menu.  The unmount is bounded and
     * blocking -- see Volumes::eject -- so this puts the spinner up first. */
    void onEjectRequested(int index);

    /* From any page, through PageWidget's signals. */
    void onToastRequested(const QString &text, int ms);
    void onCloseRequested();
    void onTitleChanged();
    void onTextRequested(const QString &prompt, const QString &initial, bool password);
    void onKeyboardFinished(const QString &text, bool accepted);

    /* From the pages that ask the shell for something only it can do. */
    void onSettingsOpen(int destination);
    void onTerminalRequested(const QString &command);
    void onLaunchRequested(const QString &title, const QString &exe,
                           const QStringList &args, bool confirm);

    void onKey(int code, bool pressed, int modifiers);

    /*
     * The volume bar changed and something has to draw it.
     *
     * WHY THE SHELL AND NOT THE BAR.  While a film is up the bar cannot paint
     * itself -- see the note on setRedirected() in volume.h -- so the pixels have
     * to be handed to whoever owns the scanout.  The bar does not know who that
     * is and must not: it is an overlay, and an overlay that reached into the page
     * stack to find the current page would be the page stack's problem for the
     * rest of its life.  The shell owns both ends, so the routing is here, and it
     * is one function so there is exactly one answer to "where do the pixels go".
     */
    void syncVolumeOverlay();

    /* The cursor moved, or the thing under it stopped being a Qt page.  Same
     * routing, same reason, one extra job: this one also decides the mode, because
     * the cursor is the only thing that knows when it needs deciding. */
    void syncPointerOverlay();

    /*
     * A page is waiting for something, or has stopped waiting.  The shell owns the
     * one spinner on the glass for the same reason it owns the one toast: a page
     * that drew its own would draw it again the moment anything was pushed on top
     * of it, and a page that has taken the whole panel cannot draw a widget over
     * itself at all.
     */
    void onBusyRequested(bool on, const QString &what);
    /* The ring turned.  Same three-line routing as the volume bar and the cursor,
     * and it exists for the same reason: over a film the pixels are the GPU's. */
    void syncBusyOverlay();

    /*
     * Something was plugged into the port, or pulled out of it.
     *
     * WHY THE SHELL SAYS SO OUT LOUD.  This device has one USB connector and no
     * status LED on it, and the pointer only appears once it has been moved -- so
     * a mouse that arrives silently is indistinguishable from a mouse that did not
     * arrive at all until the user has already started doubting the cable.  One
     * toast naming what the kernel saw ends that.  It is also the only feedback a
     * keyboard gets, since a keyboard changes nothing on screen until something
     * asks for text.
     */
    void onInputDeviceAdded(const QString &name, bool mouse, bool keyboard);
    void onInputDeviceRemoved(const QString &name);

    /*
     * Headphones went into the jack, or came out of it.
     *
     * WHY THE SHELL AND NOT THE DRIVER.  The kernel knows a pin changed; it does
     * not know that this system has two ALSA switches, that the user may have
     * turned the speaker off on purpose, or that there is a Settings page drawing
     * both of them.  A driver that muted the amp itself would be making that
     * policy in the one place nothing can see it or override it -- and it would
     * need the audio module and the input module to know about each other, which
     * on this board means a modprobe dependency between two things the kernel
     * command line is supposed to be able to leave out independently.  So the
     * kernel reports and the shell decides, which is also why the decision is one
     * function and not two halves in two drivers.
     */
    void onHeadphoneJack(bool plugged);
    /* Re-file the device list into the System information page, which is the one
     * place that lists them by name. */
    void refreshInputSummary();

    /*
     * The language changed under the shell's feet.
     *
     * The cards are the only strings in this program built once and kept: every
     * other page fills its rows in onEnter(), so walking back to a page is already
     * enough to retranslate it.  The grid is not walked back to -- it is the root,
     * and you arrive at it by leaving something else -- so it is rebuilt here.
     */
    void retranslate();

private:
    void buildPages();
    /* Wire the signals every PageWidget has.  Called once per page, so a page
     * added later cannot forget to be connected to the toast. */
    void adopt(PageWidget *page);

    PageWidget *current() const;
    void showPage(PageWidget *page);
    /* Empty the stack and show the grid.  What setRoot(0) used to be, with the
     * other two roots and the number gone. */
    void goHome();
    void push(PageWidget *page);
    void pop();
    /* Status bar, page geometry and the pointer, from whatever is current. */
    void applyChrome();
    void syncInputMode();
    void openDestination(int destination);

    /*
     * Put the two output switches where the jack says they belong.  `announce'
     * is false for the one at startup, which is describing a plug that happened
     * before this program existed and has no business toasting about it.
     */
    void applyJackRouting(bool plugged, bool announce);

    void activate(const AppEntry &entry);
    void launch(const QString &title, const QString &exe, const QStringList &args);
    /* Everything launch() has to undo, in one place, so that the two ways a child
     * can end -- it exited, it never started -- cannot drift apart. */
    void childDone(const QString &message);
    /* Not launch(): a shutdown is the one child that must not be waited for, and
     * the one that needs the panel to say so first.  See dashboard.cpp. */
    void powerOff();
    void toast(const QString &text, int ms = 2400);
    static QString firstExisting(const QStringList &candidates);

    StatusBar *m_bar = nullptr;

    /*
     * The root, and there is exactly one of it.
     *
     * There were four once: this grid, Media, Settings, and a second CardGrid
     * holding nothing but Power off and System.  They are all cards on this one
     * now; buildPages() says why at more length.
     */
    CardGrid *m_apps = nullptr;

    /* Pushed on top of the grid. */
    MediaPage *m_media = nullptr;
    SettingsPage *m_settings = nullptr;
    FilesPage *m_files = nullptr;
    TerminalPage *m_terminal = nullptr;
    WifiPage *m_wifi = nullptr;
    SharingPage *m_sharing = nullptr;
    PackagesPage *m_packages = nullptr;
    DiagnosticsPage *m_diagnostics = nullptr;
    MousePage *m_mouse = nullptr;
    DisplayPage *m_display = nullptr;
    RegionPage *m_region = nullptr;
    InfoPage *m_info = nullptr;

    QVector<PageWidget *> m_all;
    /* Empty means the card grid is on screen; otherwise the last one is. */
    QVector<PageWidget *> m_stack;
    PageWidget *m_current = nullptr;

    Keyboard *m_keyboard = nullptr;
    /* The page that asked for text, so the answer goes back to it and not to
     * whatever happens to be on screen when the keyboard closes. */
    QPointer<PageWidget> m_textTarget;

    Pointer *m_pointer = nullptr;

    QLabel *m_toast = nullptr;
    QTimer *m_toastTimer = nullptr;
    /* The vertical bar VOL+ and VOL- put on the glass for three seconds.  An
     * overlay, like the toast and the keyboard, and for the same reason: it has
     * to appear over a page that has taken the whole panel. */
    VolumeOverlay *m_volumeBar = nullptr;
    /* The ring that turns while something is being opened.  One of them, over
     * whichever page asked -- see onBusyRequested(). */
    Busy *m_busy = nullptr;
    Joypad *m_pad = nullptr;

    bool m_firstPaint = false;
    int m_armed = InternalNone;
    /* The exe of a confirm-first card that has been pressed once.  Keyed on the path
     * rather than on a bool so two such cards cannot arm each other. */
    QString m_armedExe;

    /*
     * The launched child, while there is one.  Null the rest of the time, and that
     * null is also the "may I start another" test -- there is one framebuffer and
     * one set of input devices, so two children would be two programs drawing over
     * each other with the pad going to neither.
     */
    QPointer<QProcess> m_child;
    QString m_childTitle;
};

#endif /* MIXDASH_DASHBOARD_H */
