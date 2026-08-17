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

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "switcher.h"
#include "widgets.h"

class Busy;
class DiagnosticsPage;
class DesktopPage;
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
        InternalVolume,
        InternalDesktop
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
     * can end -- it exited, it never started -- cannot drift apart.  `index' is
     * into m_tasks and the task is removed from the list here. */
    void childDone(int index, const QString &message);

    /*
     * ── MULTITASKING, IN FIVE FUNCTIONS ──────────────────────────────────────
     *
     * Read setForeground() in dashboard.cpp first; it is the one that carries the
     * rules, and the other four are in service of it.
     */

    /*
     * Give the panel and the pad to one task, or to this dashboard.
     *
     * `index' is into m_tasks; -1 means the dashboard itself.  Everything else is
     * stopped.  Safe to call with the value it already has -- the switcher's
     * cancel path does exactly that on purpose, so that "put it back" is the same
     * code as "switch to it" and cannot drift from it.
     */
    void setForeground(int index);
    /*
     * Open a card's program as a WINDOW rather than giving it the panel.
     *
     * With a graphical session already running this is a line down its control
     * pipe and a switch to it; with none it starts one with this program as its
     * first window, which is one ordinary launch() of j36-xsession.  Either way
     * the session is ONE task -- see LaunchMode in widgets.h.
     */
    void launchWindowed(const QString &title, const QString &exe,
                        const QStringList &args);
    /*
     * One shell command line into the session, wherever that leaves it: down the
     * control pipe of the session that is running, or as the first window of one
     * started for it.  `cmd' is already quoted -- the far end runs it with sh -c
     * -- and `title' is only ever used in a sentence.
     *
     * The half of launchWindowed() that does not care who asked, because two
     * things now do: a card, and j36-xrun by way of the queue below.
     */
    void runInSession(const QString &title, const QString &cmd);
    /* Select one of the graphical session's client cards and reveal it. */
    void showDesktopWindow(qint64 pid);
    /*
     * Read /run/j36/xrun.queue and open what is in it.  Called from the request
     * poll when SIGUSR2 has been seen; see RunRequest in switcher.h for why the
     * command comes to this program rather than to the session.
     */
    void takeRunRequests();
    /* Put lines back, for the pass that could not finish them. */
    void queueRunRequests(const QStringList &lines);
    /* Where the graphical session is in m_tasks, or -1 if none is running.  By
     * exe, because there can only ever be one of it. */
    int sessionTask() const;
    /* Start :0 once after the first dashboard frame, then park it as an ordinary
     * background task as soon as its supervisor is ready. */
    void startDesktopInBackground();
    void pollDesktopWarmup();
    void cancelDesktopWarmup();
    /*
     * SIGSTOP whatever is in front and keep its frame, leaving nothing owning the
     * panel.  The half of setForeground() that launch() needs on its own -- it is
     * on its way to a program that does not exist yet.
     */
    void stopForeground();
    /*
     * Stop the task in front, if any, and put the switcher up.  The ordering is
     * the contract switcher.h describes: nothing may be drawing when the overlay
     * paints.
     */
    void showSwitcher();
    /* The rows, built from m_tasks.  Row 0 is always this dashboard, so row n+1
     * is task n and setForeground(row - 1) is the inverse. */
    QVector<Switcher::Entry> switcherRows() const;
    /* New rows for a switcher that is already up.  Does nothing when it is not,
     * which is most of the time, so callers need not check. */
    void refreshSwitcher();
    /*
     * Ask a task to go away: SIGCONT and then SIGTERM to its process group.
     *
     * THE SIGCONT IS NOT OPTIONAL.  A stopped process cannot run a signal
     * handler, so SIGTERM on its own would sit pending against a program that
     * never gets the chance to act on it -- the task would look closed, stay in
     * the list, and only die when something else eventually continued it.
     */
    void closeTask(int index);
    /* Where a QProcess is in m_tasks, or -1.  By pointer and not by a captured
     * index, because closing one task renumbers every task after it. */
    int indexOfTask(QProcess *proc) const;
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
    DesktopPage *m_desktop = nullptr;
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
     * ── THE TASKS ────────────────────────────────────────────────────────────
     *
     * This was `QPointer<QProcess> m_child' and a title, and the null was the "may
     * I start another" test.  There is a list now, and switcher.h explains at
     * length why one-at-a-time was the wrong rule to draw from a board with one
     * framebuffer.  What has NOT changed is the part that was always true: one of
     * these owns the panel and the rest are stopped.
     */
    /*
     * How many programs may be up at once, not counting this dashboard.
     *
     * Four, and both halves of the number are real.  It is what switcher.cpp can
     * lay out without a scrollbar on a 480 px panel -- five rows including the
     * dashboard's -- and it is about as much as 946 MB of RAM will hold, given
     * that a browser session is an X server plus Firefox and that alone is a
     * third of it.  A fifth task would be a switcher that scrolls leading to an
     * out-of-memory kill.
     */
    static const int kMaxTasks = 4;

    struct Task {
        /* A QPointer so a QProcess destroyed some other way cannot be mistaken
         * for a running one -- the same reason m_child was one. */
        QPointer<QProcess> proc;
        QString title;
        /* What was launched.  Kept so that pressing the same card twice switches
         * to the copy that is running instead of starting a second one. */
        QString exe;
        /*
         * The process group, which is the pid because launch() makes every child
         * a group leader.  Signals go to -pid: a session is a shell that runs
         * xinit that forks an X server, and stopping only the shell would leave
         * the server drawing over the switcher.
         *
         * Kept as a number of its own rather than read from `proc' at signal
         * time, because a QProcess that has finished answers 0 for its pid and
         * the SIGCONT on the way out of a close is sent to a task that is on its
         * way to being finished.
         */
        qint64 pgid = 0;
        bool stopped = false;
        /*
         * The panel as this task last drew it, while it is stopped.  Cleared the
         * moment it is put back, so the memory is only spent on tasks that are
         * actually in the background.  See panel.h for why an event-driven
         * program needs this and a game does not.
         */
        QByteArray frame;
    };

    QVector<Task> m_tasks;
    /*
     * Who is in front: an index into m_tasks, or -1 for this dashboard.
     *
     * -1 IS NOT "NOTHING IS RUNNING".  Tasks can be running -- stopped, but alive
     * -- with the dashboard in front, and that is the ordinary state after the
     * switcher is used to come back to the grid.  m_tasks.isEmpty() is the test
     * for nothing running, and the two are asked for different reasons.
     */
    int m_fg = -1;
    Switcher *m_switcher = nullptr;
    /* Which row the switcher was opened on, so that cancelling puts back exactly
     * what was in front rather than whatever the highlight has since moved to. */
    int m_switcherWas = -1;
    /*
     * Polls SwitcherRequest::take() and RunRequest::take().
     *
     * IT RUNS FOR THE WHOLE LIFE OF THE PROGRAM, and that is a fix and not an
     * oversight.  It used to be started when a task came to the front and stopped
     * when one went away, on the reasoning that a task in front is the only thing
     * that can send SIGUSR1 -- which was true of that signal and is not true of
     * SIGUSR2, since j36-xrun asks for a window from a shell on the dashboard's
     * own Terminal page with no task in front at all.  It was also wrong about the
     * first: a signal that arrived while the timer was stopped stayed pending in
     * the flag and was acted on much later, which is how holding FN once put the
     * switcher back up over the row the user had just chosen.  A quarter-second
     * timer costs nothing measurable and removes both.
     */
    QTimer *m_requestTimer = nullptr;
    /* The X server is warmed once at boot and stopped only after its control FIFO
     * exists, so the first graphical command never has to race Xorg startup. */
    QTimer *m_desktopWarmTimer = nullptr;
    bool m_warmingDesktop = false;

    /*
     * ── A LAUNCH IS NOT RE-ENTRANT ───────────────────────────────────────────
     *
     * launch() paints its toast and then turns the event loop once, so that the
     * sentence naming what is starting is on the glass before updates go off.
     * That one turn delivers timers, and the joypad's is 15 ms: a card press that
     * is still being held, or an FN that is already down, comes back INTO this
     * object while a QProcess is half started and m_tasks is being appended to.
     * Two launches interleaved is a task list mutated under a reference; a
     * switcher opened from inside a launch is an overlay that the rest of the
     * launch then covers with a child.
     *
     * So the loop is turned with a flag set.  A second launch is refused outright
     * -- pressing a card twice was always going to open one thing -- and the
     * switcher is remembered and put up the moment the launch is finished, which
     * is where it can do its job properly: the task exists, it has a pgid, and
     * stopping it stops something real.
     */
    bool m_launching = false;
    bool m_switcherPending = false;
};

#endif /* MIXDASH_DASHBOARD_H */
