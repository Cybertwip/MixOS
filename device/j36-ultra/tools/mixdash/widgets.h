/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * widgets.h -- the four things the dashboard is made of.
 *
 * Each is one QWidget that paints itself with QPainter.  None of them owns child
 * widgets and none of them uses a layout: at 640x480 with six cards there is
 * nothing to lay out, and painting a grid in a loop is both less code and one
 * repaint instead of seven.
 *
 * The chrome is genuinely translucent rather than a flat approximation of it.  A
 * child QWidget with no autoFillBackground has no background of its own, so Qt
 * repaints the Dashboard's wallpaper underneath before StatusBar and Dock paint
 * over it at MVII's own alphas.
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
 * fail to find at runtime. */
enum Glyph {
    GlyphGames = 0,
    GlyphFiles,
    GlyphVideo,
    GlyphDisplay,
    GlyphSettings,
    GlyphPower,
    GlyphTerminal,
    GlyphBack
};

void paintGlyph(QPainter &p, const QRectF &box, int glyph, const QColor &ink);

/* One entry in a card grid.  An empty exe means the card does something internal,
 * named by `internal'; the Dashboard owns that enum. */
struct AppEntry {
    QString title;
    QString subtitle;
    QColor accent;
    int glyph = GlyphGames;
    QString exe;
    QStringList args;
    int internal = 0;
    bool available = true;
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
};

/*
 * The card grid.  Selection moves with the D-pad and wraps on the horizontal
 * axis only -- wrapping vertically on a two-row grid makes up and down feel like
 * the same button.
 */
class CardGrid : public QWidget
{
    Q_OBJECT

public:
    explicit CardGrid(QWidget *parent = nullptr);

    void setEntries(const QVector<AppEntry> &entries);
    const QVector<AppEntry> &entries() const { return m_entries; }
    int index() const { return m_index; }
    QString currentTitle() const;

    void moveBy(int dx, int dy);
    void activate();

signals:
    void activated(int index);
    void indexChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF cardRect(int i) const;
    void paintCard(QPainter &p, const AppEntry &e, const QRectF &r, bool selected);

    QVector<AppEntry> m_entries;
    int m_index = 0;
};

/* The dock: one slot per page, the active one lit. */
class Dock : public QWidget
{
    Q_OBJECT

public:
    explicit Dock(QWidget *parent = nullptr);

    void setPages(const QStringList &names);
    void setCurrent(int page);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QStringList m_pages;
    int m_current = 0;
};

/*
 * The information sheet -- what the boot log has had to be read for until now,
 * on the glass instead: the framebuffer geometry the panel is actually running
 * at, whether card0 and controlC0 exist, which j36 words this boot carried.
 */
class InfoPage : public QWidget
{
    Q_OBJECT

public:
    explicit InfoPage(QWidget *parent = nullptr);

    void setInputSummary(const QString &summary);
    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    QVector<QPair<QString, QString> > m_rows;
    QString m_inputs;
};

#endif /* MIXDASH_WIDGETS_H */
