/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * dashboard.h -- the shell: four root pages, a stack above them, one input path.
 *
 * WHAT CHANGED AND WHY.  The first version of this file was four pages and a
 * chain of `if (m_page == 1)' in onNav().  That was honest at four pages and
 * unreadable at twelve, and twelve is what this build has: Apps, Media, Settings
 * and Power at the root, and Files, Terminal, Wi-Fi, Packages, Diagnostics,
 * Mouse and System pushed on top of them.  Every page now answers handleNav()
 * for itself and the shell only decides WHICH page is on screen.  The shell's
 * remaining jobs are the ones no page can do alone:
 *
 *   - the stack.  Back falls through a page that does not consume it, and the
 *     shell pops.  That is the whole navigation model.
 *   - the tabs.  Left and right fall through the same way, and from a root page
 *     the shell reads that as "there was nothing that way on this page" and moves
 *     to the next one -- the D-pad's version of the shoulder buttons, so a board
 *     being driven with one thumb is not stuck on the tab it started on.
 *   - the volume.  Two keys on the side of the case that no page ever sees.
 *   - launching.  A child process needs the pad suspended, the panel warned
 *     about and a toast afterwards; a page cannot do that to itself.
 *   - the keyboard overlay.  It is one widget shared by every page that asks for
 *     text, and the answer goes back to whichever page asked.
 *   - the pointer.  One cursor over every page, above everything, asleep while a
 *     page owns the whole panel.
 *   - the chrome.  The status bar and the dock go away for a page that says
 *     wantsFullscreen(), and come back when it stops saying it.
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
     * There is no InternalMedia, InternalSettings or InternalPower any more: those
     * three cards did nothing but setRoot() to a tab the dock already shows, so
     * the cards are gone and so are the arms that served them.  InternalConsole
     * went the same way for a worse reason -- it hung the dashboard.  The note
     * where the card used to be built, in buildPages(), says why.
     *
     * InternalReboot is gone too, and that one for no reason worse than the board
     * having a power button that already does it.  A card that duplicates hardware
     * is a card you can hit by accident.
     */
    enum Internal {
        InternalNone = 0,
        InternalFiles,
        InternalTerminal,
        InternalWifi,
        InternalSharing,
        InternalPackages,
        InternalDiagnostics,
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
     * being held.  Only the edge-of-page gesture reads it -- see onNav. */
    void onNav(int action, bool repeat);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    /* Installed on the application so one function handles the keypad, the
     * joystick and a USB keyboard, whichever child widget happens to have focus. */
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onAppActivated(int index);
    void onPowerActivated(int index);
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
     * The language changed under the shell's feet.
     *
     * The cards and the dock are the only strings in this program built once and
     * kept: every other page fills its rows in onEnter(), so walking back to a
     * page is already enough to retranslate it.  These two are not walked back to
     * -- the dock is always on the glass -- so they are rebuilt here.
     */
    void retranslate();

private:
    void buildPages();
    /* Wire the signals every PageWidget has.  Called once per page, so a page
     * added later cannot forget to be connected to the toast. */
    void adopt(PageWidget *page);

    PageWidget *current() const;
    void showPage(PageWidget *page);
    void setRoot(int page);
    /* setRoot() one tab along, wrapping.  Both shoulders and, from a root page,
     * both edges of the D-pad come through here. */
    void stepRoot(int delta);
    void push(PageWidget *page);
    void pop();
    /* Status bar, dock, page geometry and the pointer, from whatever is current. */
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
    Dock *m_dock = nullptr;

    /* The four root pages, in dock order. */
    CardGrid *m_apps = nullptr;
    MediaPage *m_media = nullptr;
    SettingsPage *m_settings = nullptr;
    CardGrid *m_power = nullptr;

    /* Pushed on top of a root page. */
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

    QVector<PageWidget *> m_roots;
    QVector<PageWidget *> m_all;
    /* Empty means a root page is on screen; otherwise the last one is. */
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

    int m_page = 0;
    bool m_firstPaint = false;
    int m_armed = InternalNone;
    /* The exe of a confirm-first card that has been pressed once.  Keyed on the path
     * rather than on a bool so two such cards cannot arm each other. */
    QString m_armedExe;
};

#endif /* MIXDASH_DASHBOARD_H */
