/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * files.h -- the file browser.  Four panes, one D-pad.
 *
 * IT USED TO LIVE IN dashboard.h and be a QListView with a title bar over it: up
 * and down moved, A descended, Left climbed, B left the page.  That is still the
 * middle third of this page and it is still the part that does the work.  What was
 * added around it is what a browser needs the moment there is more than one disk in
 * the machine -- somewhere to see the disks, somewhere to type a path, somewhere to
 * type a name, and somewhere to read what the highlighted thing actually is.
 *
 * THE FOUR PANES, and why they are panes rather than pages.  A 640x480 panel is
 * small, and the temptation is to hide the places list behind a button.  But the
 * whole reason this page changed is that a USB stick arrives while you are looking
 * at it: a places list that has to be summoned is a places list nobody sees the new
 * disk appear in.  So all four are on the glass at once, and the shoulders move
 * between them:
 *
 *   Address   the path, typed.  A opens the on-screen keyboard on it.
 *   Search    a name filter over the listing.  Same keyboard.
 *   Places    Home, the card, and one row per volume mounted under /media.
 *   List      the directory itself, and the only pane with a scrollbar.
 *
 * L1 and R1 cycle them, which is free on a pushed page -- the shell only uses the
 * shoulders on the card grid.  Up and Down also spill between them in the obvious
 * places (up off the top of the listing lands on Search, up off that lands on
 * Address), because the shoulders are a thing you have to be told about and the
 * D-pad is not.
 *
 * SCOPE, AND WHY IT IS A STRING AND NOT A BOOL.  Opening a volume's card opens this
 * page ON that volume and NOT ANYWHERE ELSE: Left refuses to climb above the mount
 * point, the places list shows that volume alone, and a path typed into the address
 * bar that is outside it is refused with a toast rather than followed.  The Files
 * card opens the same page with an empty scope, which is the whole filesystem.  One
 * page, two modes, because two instances would be two directory-loader threads and
 * two copies of the state that says where you were.
 *
 * THE INFO PANEL DOES NOT SHOW THE PATH.  It is deliberate and it was asked for:
 * the path is already in the address bar, two panes to the left, in full and in a
 * field you can edit.  Repeating it in the panel would spend a third of the panel's
 * height on the one fact already on screen.  What is there instead is what the
 * listing cannot show -- what kind of thing it is, how big, when it was last
 * written, and whether it can be written at all.
 */
#ifndef MIXDASH_FILES_H
#define MIXDASH_FILES_H

#include <QString>
#include <QStringList>
#include <QVector>

#include "widgets.h"

class QFileSystemModel;
class QListView;
class QModelIndex;

class FilesPage : public PageWidget
{
    Q_OBJECT

public:
    explicit FilesPage(QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;

    QString rootPath() const { return m_root; }

    /*
     * Open on `path', confined to `scope'.
     *
     * An empty scope is the whole filesystem, which is what the Files card asks
     * for.  A volume's card passes its mount point as both, and from then until the
     * next call this page cannot leave that filesystem.
     */
    void openAt(const QString &path, const QString &scope = QString());

signals:
    void openRequested(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void textEntered(const QString &text, bool accepted) override;

private:
    enum Pane {
        PaneAddress = 0,
        PaneSearch,
        PanePlaces,
        PaneList,
        PaneCount
    };

    /* Which field the on-screen keyboard was opened for, so the answer goes to the
     * right one.  The keyboard is the shell's and is shared by every page on it;
     * this page asks twice and has to tell the two answers apart. */
    enum Asking {
        AskNothing = 0,
        AskAddress,
        AskSearch
    };

    /* One row of the places panel. */
    struct Place {
        QString label;
        QString path;
        int glyph = GlyphFiles;
        /* A mounted volume rather than a directory on the card.  Drawn with the
         * drive glyph and, when it is read-only, a dot after the name. */
        bool volume = false;
        bool readOnly = false;
    };

    void setRoot(const QString &path);
    void step(int delta);
    void enter();
    /* False when there is nowhere further up -- the top of the scope, or of the
     * filesystem.  Left reads that as "move to the places panel". */
    bool leave();

    void setPane(int pane);
    void cyclePane(int delta);
    void rebuildPlaces();
    void applyFilter();
    /* Refuse a path outside the scope, and say so.  True when it is allowed. */
    bool withinScope(const QString &path) const;
    void navigateTo(const QString &path);

    QRectF bodyRect() const;
    QRectF addressRect() const;
    QRectF placesRect() const;
    QRectF searchRect() const;
    QRectF listRect() const;
    QRectF infoRect() const;
    QRectF placeRowRect(int i) const;

    void paintField(QPainter &p, const QRectF &r, const QString &text,
                    const QString &placeholder, bool focused) const;
    void paintPlaces(QPainter &p);
    void paintInfo(QPainter &p);

    QFileSystemModel *m_model = nullptr;
    QListView *m_view = nullptr;
    QString m_root;
    QString m_base;
    QString m_scope;
    QString m_scopeName;

    QVector<Place> m_places;
    int m_place = 0;
    int m_pane = PaneList;
    int m_asking = AskNothing;
    QString m_search;
};

#endif /* MIXDASH_FILES_H */
