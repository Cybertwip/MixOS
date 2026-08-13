/* SPDX-License-Identifier: MS-PL */
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
 * repaints the Dashboard's wallpaper underneath before StatusBar and Dock paint
 * over it at MVII's own alphas.
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
    GlyphInfo
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
 * this build has eleven.  A page now says what it does with a button and what it
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
 * wrong with one -- Doom with no IWAD -- is said by the card going grey and by
 * the toast that comes when it is pressed, not by a caption that is there whether
 * or not anything is wrong.
 */
struct AppEntry {
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
 * The card grid.  Selection moves with the D-pad and wraps on the horizontal
 * axis only -- wrapping vertically on a two-row grid makes up and down feel like
 * the same button.
 */
class CardGrid : public PageWidget
{
    Q_OBJECT

public:
    explicit CardGrid(QWidget *parent = nullptr);

    void setEntries(const QVector<AppEntry> &entries);
    const QVector<AppEntry> &entries() const { return m_entries; }
    int index() const { return m_index; }
    /* Absolute, unlike moveBy(), which walks the grid and wraps.  For putting the
     * selection back after setEntries() has reset it. */
    void setIndex(int index);
    QString currentTitle() const;

    void moveBy(int dx, int dy);
    /*
     * One card left or right in reading order, WITHOUT the wrap moveBy() does,
     * and false when there is nowhere to go.  That false is the whole point: it
     * is what lets the shoulder buttons step through the grid and then carry on
     * into the next tab instead of chasing their own tail round one row.
     */
    bool stepBy(int dx);
    void activate();

    void setPageTitle(const QString &t) { m_pageTitle = t; }
    QString title() const override;
    bool handleNav(int action) override;

signals:
    void activated(int index);
    void indexChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRectF cardRect(int i) const;
    int cardAt(const QPoint &p) const;
    void paintCard(QPainter &p, const AppEntry &e, const QRectF &r, bool selected);

    QVector<AppEntry> m_entries;
    int m_index = 0;
    QString m_pageTitle;
    /* The card the press landed on, so a press that slides off it does not
     * activate the one it slid onto. */
    int m_pressed = -1;
};

/* The dock: one slot per page, the active one lit.  Clickable, because a dock
 * that can only be reached with the shoulder buttons is a tab strip. */
class Dock : public QWidget
{
    Q_OBJECT

public:
    explicit Dock(QWidget *parent = nullptr);

    void setPages(const QStringList &names);
    void setCurrent(int page);

signals:
    void pageClicked(int page);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QVector<QRectF> slotRects() const;

    QStringList m_pages;
    int m_current = 0;
};

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
int batteryCapacity(bool *charging);
bool networkUp();
/* The wireless interface, from /sys/class/net/<if>/wireless or /phy80211. */
QString wirelessInterface();
}

#endif /* MIXDASH_WIDGETS_H */
