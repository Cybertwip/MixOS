/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * widgets.h -- the pieces every page of the dashboard is built out of.
 *
 * Each is one QWidget that paints itself with QPainter.  None of them owns child
 * widgets and none of them uses a layout: at 640x480 there is nothing to lay out,
 * and painting a grid in a loop is both less code and one repaint instead of
 * seven.
 *
 * The chrome is genuinely translucent rather than a flat approximation of it.  A
 * child QWidget with no autoFillBackground has no background of its own, so Qt
 * repaints the Dashboard's wallpaper underneath before StatusBar paints over it
 * at MVII's own alpha.
 *
 * TWO INPUT PATHS, ONE WIDGET.  Everything selectable here answers to the D-pad
 * (through PageWidget::handleNav) AND to a pointer (through the ordinary
 * QWidget mouse handlers, which Pointer synthesizes events for).  Neither is a
 * translation of the other: the pad moves a selection, the pointer moves a
 * cursor, and the row under the cursor becomes the selection when it lands.
 */
#ifndef MIXDASH_WIDGETS_H
#define MIXDASH_WIDGETS_H

#include <QColor>
#include <QPair>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

/* The icons, drawn as paths.  No image files, so nothing to stage and nothing to
 * fail to find at runtime.  Appended to rather than reordered -- the values are
 * stored in AppEntry literals all over dashboard.cpp. */
enum Glyph {
    GlyphGames = 0,
    GlyphFiles,
    GlyphVideo,
    GlyphDisplay,
    GlyphSettings,
    GlyphPower,
    GlyphTerminal,
    GlyphBack,
    GlyphWifi,
    GlyphMouse,
    GlyphPackage,
    GlyphMusic,
    GlyphImage,
    GlyphChip,
    GlyphInfo,
    GlyphGlobe,
    GlyphDrive
};

void paintGlyph(QPainter &p, const QRectF &box, int glyph, const QColor &ink);

/*
 * The window chrome every sheet-shaped page draws: rounded card, hairline border,
 * title bar clipped to the top corners.  Written once here because five pages
 * want it and a sixth will.  Returns the rectangle left for content.
 */
QRectF paintSheet(QPainter &p, const QRectF &card, const QString &title,
                  const QString &rightText = QString());

/*
 * The base every page derives from.
 *
 * WHY AN INTERFACE AND NOT A SWITCH IN THE SHELL.  The first version of this
 * dashboard routed every button in one function in dashboard.cpp, with a chain of
 * `if (m_page == 1)'.  Four pages was already the limit of what could be read;
 * this build has thirteen.  A page now says what it does with a button and what it
 * is called, and the shell only decides which page is on screen.
 */
class PageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PageWidget(QWidget *parent = nullptr);

    /* What the status bar shows while this page is up. */
    virtual QString title() const = 0;

    /* Return true if the action was consumed.  Returning false for NavBack is how
     * a page says "I am done, take me off the stack". */
    virtual bool handleNav(int action);

    /*
     * The button came back up.
     *
     * ALMOST NOTHING WANTS THIS, and that is deliberate: a press is a decision and
     * acting on it when it is made is what makes a menu feel like it answers.  One
     * page needs the other half -- the card grid, where holding A picks a card up
     * and tapping it launches, and those two cannot be told apart until the button
     * is either held long enough or let go.  So the default is empty and the shell
     * delivers this to whatever is on the glass without asking.
     */
    virtual void handleNavRelease(int action);

    /* Shown and hidden.  Pages that poll, decode or hold a child process start
     * and stop here rather than running while nobody is looking at them. */
    virtual void onEnter();
    virtual void onLeave();

    /* True while the page wants the whole panel -- no status bar, no dock.  Video
     * playback and the Terminal both do. */
    virtual bool wantsFullscreen() const;

    /* Raw evdev keys, forwarded by the shell only to the page that asked for them
     * by returning true from wantsKeys(). */
    virtual bool wantsKeys() const;
    virtual void keyPressed(int code, bool pressed, int modifiers);

signals:
    /* A message on the glass, without the page needing to know about the shell. */
    void toastRequested(const QString &text, int ms);
    /* "Pop me."  Same effect as Back falling through. */
    void closeRequested();
    /* The title changed under the page's own feet -- a directory was entered, a
     * track started, a scan finished. */
    void titleChanged();
    /* Ask the shell to put a text-entry keyboard up.  reply is delivered by
     * calling textEntered() on the page that asked. */
    void textRequested(const QString &prompt, const QString &initial, bool password);
    /*
     * "I am waiting for something, please say so."  The shell owns the spinner --
     * one of them, over whatever page is up -- because a page that owns the whole
     * panel (a film) cannot draw a widget over itself, and because two pages that
     * each drew their own would be two spinners the moment anything was pushed on
     * top of anything.  `what' is one short line under the ring; false takes it
     * down and the text is ignored.
     */
    void busyRequested(bool on, const QString &what);

public:
    /* Answer to textRequested.  Default does nothing. */
    virtual void textEntered(const QString &text, bool accepted);
};

/*
 * One entry in a card grid.  An empty exe means the card does something internal,
 * named by `internal'; the Dashboard owns that enum.
 *
 * NO DESCRIPTION FIELD, DELIBERATELY.  Every card used to carry a line of prose
 * under its name, and on a 640x480 panel that is the difference between a grid
 * you read and a grid you scan: nine cards times two lines of 12 px text is more
 * words than the rest of the shell put together, and none of them told you
 * anything the name and the icon had not.  A card is a glyph and a noun.  What is
 * wrong with one -- a card whose program is not installed -- is said by the card
 * going grey and by
 * the toast that comes when it is pressed, not by a caption that is there whether
 * or not anything is wrong.
 */
struct AppEntry {
    /*
     * A STABLE ASCII NAME FOR THIS CARD, AND IT IS NOT THE TITLE.
     *
     * The user's chosen order is written to the settings file as a list of these,
     * so the identifier has to be the one thing about a card that does not move:
     * the title is translated -- "Files" is "Fichiers" on a French boot -- and the
     * position is the very thing being stored.  Six languages times one saved
     * layout means a title-keyed order is a layout that resets when the language
     * does.
     *
     * It is also the hook for the packages this grid is being made ready for: an
     * installed application arrives with a key nothing here has seen, so it lands
     * at the end of the grid rather than in the middle of somebody's arrangement,
     * and it keeps whatever place it is dragged to across the reinstall that
     * changes its version.
     */
    QString key;

    QString title;
    QColor accent;
    int glyph = GlyphGames;
    QString exe;
    QStringList args;
    int internal = 0;
    /* Greys the card out and turns activation into an explanation instead of a
     * launch.  `reason' is what that explanation says; it is never painted. */
    bool available = true;
    QString reason;
    /* Ask twice.  For a child that sets its own mode through /dev/dri/card0: it
     * takes the scanout away from the framebuffer this dashboard draws into and
     * nothing gives it back, so the warning has to come before the launch -- once
     * the child has the panel, no toast of ours can be seen. */
    bool confirm = false;
    /* This card is a mounted USB volume, so its long-press menu has Eject on it.
     * A flag rather than a test on the key, because the grid is not the place that
     * knows what a volume is -- see volumes.h and Dashboard::rebuildApps. */
    bool ejectable = false;
};

/*
 * The menu bar: brand mark, the focused card's name, then network, battery and
 * clock on the right.  MVII's traffic lights are deliberately not here -- there
 * are no windows on this shell to close, and three dots that do nothing are worse
 * than none.
 */
class StatusBar : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBar(QWidget *parent = nullptr);

    void setTitle(const QString &title);

protected:
    void paintEvent(QPaintEvent *event) override;

private slots:
    void refresh();

private:
    QString m_title;
    int m_capacity = -1;      /* percent, or -1 for "no power_supply class" */
    bool m_charging = false;
    bool m_net = false;
    /* -1 when nothing is associated; otherwise 0..100 from the wpa_supplicant
     * signal level, so the bars mean something on a wireless board. */
    int m_wifi = -1;
};

/*
 * The card grid: a lattice of fixed-size slots the user can rearrange.
 *
 * THREE THINGS LIVE HERE AND THEY ARE EASY TO CONFUSE.
 *
 *   the ORDER   -- m_entries, which is what the grid means.  Slot n holds
 *                  m_entries[n], always.  Reordering the grid is reordering this
 *                  vector and nothing else; there is no separate layout to keep
 *                  in step with it, which is the bug a slot->card map would have
 *                  been.
 *   the SLOTS   -- geometry, computed from the page size by slotRect().  Nothing
 *                  is stored: the grid is a function of the widget's width, so a
 *                  panel that changes size (this build follows a USB-HDMI adapter
 *                  to whatever it reports) relays itself with no state to update.
 *   the MOTION  -- m_motion, one entry per card, which is where a card is being
 *                  DRAWN as opposed to where it belongs.  It exists only so the
 *                  cards can be seen moving between slots and is discarded and
 *                  rebuilt whenever the entries are.
 *
 * GRAVITY, meaning what it means on a phone: cards fall towards the front of the
 * grid and there are no holes in the middle of it.  Dropping a card into slot 4
 * inserts it there and everything from 4 on shuffles one along -- it does not
 * swap with whatever was in 4, and it does not leave slot 7 empty.  The animation
 * is the same idea made visible: every card is on a spring towards its slot,
 * slightly underdamped, so a rearrangement settles rather than snapping, and a
 * card that has just been let go of is given a downward shove so it drops into
 * place instead of sliding there.
 *
 * WHY THE ANIMATION COSTS NOTHING AT REST.  m_anim is a timer that is started
 * when a card is off its mark and STOPS ITSELF the moment every card has settled.
 * A dashboard sitting on the desk repaints exactly as often as it did before this
 * file learned to move -- which on a Cortex-A7 with no 2D engine and a software
 * rasteriser is not an optimisation, it is the difference between a pointer that
 * tracks and one that stutters.
 */
class CardGrid : public PageWidget
{
    Q_OBJECT

public:
    explicit CardGrid(QWidget *parent = nullptr);

    /*
     * `entries' is the natural order -- the order buildPages() writes them in.
     * What lands in m_entries is that list permuted by the arrangement the user
     * saved, with anything whose key is not in the saved list appended in the
     * order given.  So a new card appears at the end of the grid, which is the
     * only place it can appear without moving something the user put where it is.
     */
    void setEntries(const QVector<AppEntry> &entries);
    const QVector<AppEntry> &entries() const { return m_entries; }

    /*
     * The arrangement the user saved last time, as keys in slot order.
     *
     * Set BEFORE setEntries and it is remembered, so the rebuild a language change
     * causes does not also undo the arrangement.  This widget never opens the
     * settings file itself -- the same split ListPane has, where the pane owns the
     * value and the page owns writing it down.  What comes back out is
     * orderChanged().
     */
    void setOrder(const QStringList &keys);
    QStringList order() const;
    int index() const { return m_index; }
    /* Absolute, unlike moveBy(), which walks the grid a card at a time.  For
     * putting the selection back after setEntries() has reset it. */
    void setIndex(int index);
    QString currentTitle() const;

    /* False when the selection did not move -- the first or last card, or a grid
     * with nothing in it.  handleNav() used to hand that answer back to the shell
     * so a refused left or right became a change of root page; nothing reads it
     * now, and a step bigger than the grid is how the shoulder buttons reach the
     * two ends without a second entry point. */
    bool moveBy(int dx, int dy);
    void activate();

    /* True while a card has been picked up and is following the selection.
     *
     * The shell does not need to ask -- handleNav() consumes every direction
     * itself, so a card cannot be slid off the page -- but a page that wants to
     * know whether this grid is mid-gesture has no other way to find out, and the
     * state is worth being able to see from outside. */
    bool carrying() const { return m_carry >= 0; }

    void setPageTitle(const QString &t) { m_pageTitle = t; }
    QString title() const override;
    bool handleNav(int action) override;
    void handleNavRelease(int action) override;
    void onLeave() override;

signals:
    void activated(int index);
    void indexChanged(int index);
    /* The arrangement changed and should be written down.  The shell owns
     * Settings; this widget owns the order.  Carries the keys in slot order. */
    void orderChanged(const QStringList &keys);
    /* Eject was chosen from a card's long-press menu.  The grid knows the gesture
     * and nothing else: unmounting is the shell's, through volumes.h. */
    void ejectRequested(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private slots:
    /* The hold timer expired with A still down: the press was a long press. */
    void onHoldExpired();
    /* One frame of the spring.  Stops itself when everything has settled. */
    void step();

private:
    /*
     * Where a card is being drawn, as against where it belongs.
     *
     * `lift' is 0 for a card in the grid and 1 for one that has been picked up,
     * and it is animated rather than switched so the card grows into the hand
     * instead of jumping.  It scales the card and deepens its shadow, which is
     * the whole of "this one is in the air".
     */
    struct Motion {
        qreal x = 0.0;
        qreal y = 0.0;
        qreal vx = 0.0;
        qreal vy = 0.0;
        qreal lift = 0.0;
        bool placed = false;   /* false until the first layout gives it a home */
    };

    int columns() const;
    int rowCount() const;
    /* How many cards are in row r: columns() for every row but the last, and the
     * remainder for the last.  It is what centres a short row -- see slotRect. */
    int rowFill(int r) const;
    /* The slot's resting rectangle, in widget coordinates, scroll included. */
    QRectF slotRect(int i) const;
    /* Where card i is actually being drawn, motion and lift included. */
    QRectF drawRect(int i) const;
    /* slotRect/drawRect plus the shadow and glow drawn outside them -- the unit
     * of repaint for this page.  See the comment on selectTo() in widgets.cpp. */
    QRect dirtyRect(const QRectF &r) const;
    int cardAt(const QPoint &p) const;
    /* The slot a point falls in, gutters included, for dropping a carried card.
     * There is no longer an empty tail to aim at -- a short row is centred, so the
     * space either side of it is margin and not a slot. */
    int slotAt(const QPoint &p) const;

    /* Rebuild m_motion for the current entry count, keeping what it can. */
    void resetMotion();
    /* Wake the spring.  Idempotent; the timer stops itself. */
    void wake();
    /* Scroll so slot i is on screen, for a grid taller than the page. */
    void ensureVisible(int i);
    int maxScroll() const;

    /* Move the selection to `next', repaint only the two cards that changed, and
     * emit indexChanged().  Every caller that moves the selection goes through
     * here; nothing sets m_index by hand. */
    void selectTo(int next);
    /* Take the card at m_index out of the grid and into the hand. */
    void pickUp();
    /* Put it down where it now sits, save the order, and drop it with a thump. */
    void drop();
    /* Put it back where it was picked up from and forget the whole thing. */
    void cancelCarry();
    /* The gravity move: take the card out of `from' and insert it at `to', so
     * everything between shuffles by one.  NOT a swap -- see the class comment. */
    void moveEntry(int from, int to);

    void paintCard(QPainter &p, const AppEntry &e, const QRectF &r, bool selected,
                   qreal lift);

    /*
     * THE CARD ART CACHE, and why a grid that only moves nine rectangles needed
     * one.
     *
     * paintCard is not cheap: a soft shadow is six antialiased rounded strokes, the
     * body is three gradient fills, the foot needs a clip path, the glyph is a
     * handful of paths, and the title is measured, elided and shaped -- and none of
     * that is cached by Qt between calls.  At rest it happens twice a selection
     * move and nobody notices.  DURING A PLACEMENT IT HAPPENS TO EVERY CARD BETWEEN
     * THE TWO SLOTS, thirty times a second, on a Cortex-A7 with no 2D engine: the
     * cards are sliding, so every one of them is dirty on every frame.  That is the
     * choppiness.
     *
     * A sliding card is the same picture at a different place, so the picture is
     * drawn once into a pixmap and the frames blit it.  Only two cards are ever
     * drawn live: the selected one, whose outline differs, and the carried one,
     * which is also scaled by its lift.  Everything else is a blit.
     *
     * The pixmap is the card plus kArtPad on each side, because a card draws
     * outside its own rectangle -- see dirtyRect(), which uses the same margin for
     * the same reason.
     *
     * INVALIDATION IS THE PART THAT BITES.  The art depends on the entry and on the
     * slot size, so setEntries() and resizeEvent() drop the lot; moveEntry()
     * permutes the cache in step with m_entries rather than dropping it, which is
     * the whole point -- the placement animation must not be the thing that rebuilds
     * every card it is animating.
     */
    const QPixmap &cardArt(int i);
    void invalidateArt();

    /* ── the long-press menu ──────────────────────────────────────────────── */
    enum MenuAction {
        MenuOpen = 0,
        MenuMove,
        MenuEject
    };

    void openMenu(int index);
    void closeMenu();
    void runMenu();
    QVector<int> menuActions() const;
    QRectF menuRect() const;
    QRectF menuRowRect(int row) const;
    void paintMenu(QPainter &p);

    QVector<AppEntry> m_entries;
    QVector<Motion> m_motion;
    int m_index = 0;
    QString m_pageTitle;
    /* The card the press landed on, so a press that slides off it does not
     * activate the one it slid onto. */
    int m_pressed = -1;

    /* -1 when nothing is being carried; otherwise the slot the carried card is
     * currently sitting in, which is also its index in m_entries. */
    int m_carry = -1;
    /* Where it was picked up from, so B can put it back. */
    int m_carryFrom = -1;
    /* True between an A press and either its release or the hold expiring.  It is
     * what makes a long press not also be a launch: the hold clears it. */
    bool m_okArmed = false;
    /* Set while the carried card is following the pointer rather than the slot
     * grid, so the two input paths do not fight over its position. */
    bool m_carryByPointer = false;
    QPoint m_pointerAt;

    QTimer *m_hold = nullptr;
    QTimer *m_anim = nullptr;

    /* The saved arrangement, kept so that a rebuild -- which is what a language
     * change does to this page -- reapplies it instead of resetting it. */
    QStringList m_order;

    /* A grid with more cards than fit scrolls rather than paging.  Zero, and
     * unreachable, until something is installed that makes it taller. */
    int m_scroll = 0;

    /* Parallel to m_entries.  A null pixmap is "not drawn yet", which is how a
     * cleared cache and a cache with a hole in it are the same thing. */
    QVector<QPixmap> m_art;

    /* -1 when no menu is open; otherwise the card it belongs to. */
    int m_menu = -1;
    int m_menuRow = 0;
};

/*
 * THERE WAS A DOCK HERE: three slots along the bottom of the panel -- Apps, Media,
 * Settings -- with the current one lit, driven by the shoulder buttons and by a
 * click.
 *
 * IT IS GONE BECAUSE IT WAS A SECOND KIND OF NAVIGATION.  This shell already had
 * one: a grid of cards you walk with the D-pad and open with A.  The dock was a
 * different thing in a different place with a different gesture, and it existed to
 * reach exactly two pages -- both of which are now cards on that grid, next to the
 * nine that were always cards.  One way in, for everything.
 *
 * It also cost 56 px of every page's height for a row of three labels, which on a
 * 480-line panel is more than a row of cards.  See Dashboard::buildPages, and
 * Theme::DockH, which went with it.
 */

/*
 * One row of a ListPane.  A tagged union in all but name: most rows are a label
 * and a detail, some are a section header, some carry a slider or a switch.  One
 * struct rather than a class hierarchy because the whole point of this widget is
 * that it paints rows in a loop.
 */
struct ListRow {
    enum Kind { Item, Header, Slider, Toggle, Action };

    int kind = Item;
    QString text;
    QString detail;
    /* A short tag drawn in a pill: "WPA2", "installed", "current". */
    QString badge;
    QColor badgeColour;
    int glyph = -1;
    QColor accent;
    bool enabled = true;
    /* Whatever the page needs to identify this row again after a sort. */
    int id = 0;
    QString key;

    /* Slider and Toggle. */
    int value = 0;
    int minimum = 0;
    int maximum = 100;
    int stepSize = 1;
    QString valueText;
    bool on = false;
};

/*
 * A vertical list that is driven by the D-pad and by a pointer, scrolls, and can
 * hold sliders and switches as well as items.
 *
 * WHY NOT QListView.  Two reasons and both are structural: a model/view pair for
 * eight settings rows is more code than the rows, and QStyle draws the item, so
 * every row would look like Fusion rather than like MVII.  FilesPage does use
 * QListView -- it has a QFileSystemModel behind it and thousands of rows -- and
 * that is the line: model-backed and long goes to Qt, hand-built and short comes
 * here.
 */
class ListPane : public QWidget
{
    Q_OBJECT

public:
    explicit ListPane(QWidget *parent = nullptr);

    void setRows(const QVector<ListRow> &rows);
    const QVector<ListRow> &rows() const { return m_rows; }
    /* In-place, without disturbing the selection or the scroll -- for a value
     * that changed under a slider the user is still holding. */
    void updateRow(int index, const ListRow &row);

    int current() const { return m_current; }
    const ListRow *currentRow() const;
    void setCurrent(int index);
    void step(int delta);
    void pageStep(int delta);

    /* Rows are drawn this tall; sliders get a little more. */
    void setRowHeight(int px);
    /* Draw an "empty" line when there is nothing in the list. */
    void setPlaceholder(const QString &text);

signals:
    void activated(int index);
    /*
     * The pane owns the value.  A slider dragged with the pointer and a slider
     * nudged with the D-pad both end up here with the value already stored in the
     * row, so the page that listens does one thing -- write it to Settings --
     * rather than two.
     */
    void valueChanged(int index, int value);
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

public:
    /* Left/Right on the current row, or A on a switch.  Called by pages from
     * handleNav so the pane does not have to know the Nav enum. */
    bool adjust(int delta);
    bool press();

private:
    int rowHeightFor(const ListRow &r) const;
    int rowTop(int index) const;
    int rowAt(const QPoint &p) const;
    int contentHeight() const;
    void clampScroll();
    void ensureVisible(int index);
    /* True for headers and disabled rows: they are drawn but never landed on. */
    bool selectable(int index) const;
    int nextSelectable(int from, int delta) const;

    QVector<ListRow> m_rows;
    int m_current = -1;
    int m_scroll = 0;
    int m_rowHeight = 30;
    QString m_placeholder;
    int m_pressed = -1;
    /* A press inside a slider's track drags it; a press on the row does not. */
    bool m_dragging = false;
};

/*
 * The information sheet -- what the boot log has had to be read for until now,
 * on the glass instead: the framebuffer geometry the panel is actually running
 * at, what the CPU is and how fast it is being clocked, which disks the kernel
 * found and where they ended up mounted, what is sitting on the USB bus, and
 * which j36 words this boot carried.  It is the page to open before asking
 * anyone else what the hardware is doing.
 *
 * Everything on it is read out of /proc and /sys, fresh, on a timer.  Nothing
 * here opens a device node and nothing here writes: an information page that
 * can change the machine it is describing is a bug waiting for a stray button.
 */
class InfoPage : public PageWidget
{
    Q_OBJECT

public:
    /*
     * A row is either a section header -- label only, drawn as a rule across the
     * sheet -- or a label/value pair.  Keeping both in one vector keeps the
     * scrolling arithmetic to a single index, and makes headers scroll with
     * their section instead of floating above it, which is what you want when a
     * section is longer than the sheet is tall.
     */
    struct Row {
        QString label;
        QString value;
        bool header;
    };

    explicit InfoPage(QWidget *parent = nullptr);

    void setInputSummary(const QString &summary);
    void refresh();

    QString title() const override { return QStringLiteral("System"); }
    bool handleNav(int action) override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void addHeader(const QString &text);
    void add(const QString &label, const QString &value);
    /*
     * One label, many values: the first line carries the label and the rest are
     * left blank under it, which is how a list of six USB devices reads as one
     * entry rather than as six repetitions of the word "USB".  `empty' is what
     * to say when there are none, and saying something is the point -- a section
     * that vanishes when it is empty looks like a section that was never coded.
     */
    void addList(const QString &label, const QStringList &values, const QString &empty);

    /* The body rect paintSheet will hand back at the current size. */
    QRectF sheetBody() const;
    /* Largest first-row index that still fills the sheet, and one screenful. */
    int maxScroll() const;
    int pageStep() const;
    void scrollBy(int rows);
    /* The section the topmost visible row belongs to, for the sheet header. */
    QString sectionAt(int index) const;

    QVector<Row> m_rows;
    QString m_inputs;
    int m_scroll = 0;
};

/* Shared by more than one page, and by the status bar. */
namespace SysInfo {
QString readTrimmed(const QString &path);
/* Every line of a file, with the empty ones dropped.  Use this and never
 * `while (!f.atEnd()) f.readLine()' -- see the comment on the definition for
 * what that does to a file in /proc. */
QStringList readLines(const QString &path);
int batteryCapacity(bool *charging);
bool networkUp();
/* The wireless interface, from /sys/class/net/<if>/wireless or /phy80211. */
QString wirelessInterface();
}

#endif /* MIXDASH_WIDGETS_H */
