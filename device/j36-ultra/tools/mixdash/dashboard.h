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

class DiagnosticsPage;
class DisplayPage;
class Joypad;
class Keyboard;
class LanguagePage;
class MediaPage;
class MousePage;
class PackagesPage;
class Pointer;
class QFileSystemModel;
class QLabel;
class QListView;
class QTimer;
class SettingsPage;
class SharingPage;
class TerminalPage;
class VolumeOverlay;
class WifiPage;

/*
 * The file browser.  QFileSystemModel and QListView do the work; what is written
 * here is the D-pad contract -- up and down move, A descends or opens, B climbs
 * and then falls through, which is how the shell knows to pop the page.
 *
 * IT IS THE ONE PAGE THAT USES A MODEL AND A VIEW.  Everything else in this
 * dashboard paints its own rows through ListPane, because eight settings do not
 * need a model.  A directory on an SD card can have four thousand entries in it,
 * and that is exactly what QFileSystemModel and QListView are for.
 */
class FilesPage : public PageWidget
{
    Q_OBJECT

public:
    explicit FilesPage(QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;

    QString rootPath() const { return m_root; }

signals:
    void openRequested(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setRoot(const QString &path);
    void step(int delta);
    void enter();
    /* False when there is nowhere further up, which the shell reads as "pop me". */
    bool leave();

    QFileSystemModel *m_model = nullptr;
    QListView *m_view = nullptr;
    QString m_root;
    QString m_base;
};

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
        InternalPoweroff
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

    void activate(const AppEntry &entry);
    void launch(const QString &title, const QString &exe, const QStringList &args);
    /* Not launch(): a shutdown is the one child that must not be waited for, and
     * the one that needs the panel to say so first.  See dashboard.cpp. */
    void powerOff();
    void toast(const QString &text, int ms = 2400);
    static QString firstExisting(const QStringList &candidates);
    static QString firstWad();

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
    LanguagePage *m_language = nullptr;
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
    Joypad *m_pad = nullptr;

    bool m_firstPaint = false;
    int m_armed = InternalNone;
    /* The exe of a confirm-first card that has been pressed once.  Keyed on the path
     * rather than on a bool so two such cards cannot arm each other. */
    QString m_armedExe;
};

#endif /* MIXDASH_DASHBOARD_H */
