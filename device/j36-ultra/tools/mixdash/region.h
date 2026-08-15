/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * region.h -- Region & Language: the world map that sets the clock and the words.
 *
 * THIS REPLACES THE LANGUAGE PAGE, and it replaces it rather than sitting next to
 * it because the two settings are one question asked twice.  A device that has
 * just been taken out of a box does not know where it is, and everything that
 * follows from where it is -- what the clock says, what language the shell speaks
 * -- was previously either unaskable (there was no way at all to set a time zone
 * short of a terminal and a symlink) or asked as a list of six words with no
 * context.  A map answers both in one press: the city is the zone, and the
 * country the city is in suggests the language.
 *
 * SUGGESTS.  Pressing A on Paris does not silently start speaking French; it sets
 * the zone and then asks, on the page, with A for yes and B for no.  The distance
 * between those two behaviours is the difference between a handheld that adapts
 * and a handheld that has to be recovered from by somebody who can recognise the
 * word for their own language in a list -- which is exactly the failure the old
 * page was designed around, and it is cheaper to not cause it.
 *
 * THE MAP IS DRAWN, NOT LOADED.  The coastlines below are about two hundred and
 * fifty integer degree pairs in .rodata, stroked and filled by QPainter at
 * whatever size the page is.  The alternative was an equirectangular bitmap,
 * which means an asset to stage in build-in-vm.sh, a decode at page-open on a
 * board with no GPU, and a fixed resolution -- three costs, for a picture whose
 * entire job is to let somebody find Portugal.  At 3-5 degrees the outlines are
 * coarse and every continent is unmistakable, which is the whole requirement.
 *
 * THREE PANES, SWITCHED WITH THE SHOULDERS.  The map is the front door, but sixty
 * dots on a 560 px world puts eighteen of them inside Europe, so there is a plain
 * list of the same zones behind it for when the D-pad is losing an argument with
 * geography, and the six languages behind that for when the map's suggestion was
 * not the wanted one.  L1 and R1 move between them; the shell does not use the
 * shoulders on a pushed page, so nothing is being taken away from anything.
 */
#ifndef MIXDASH_REGION_H
#define MIXDASH_REGION_H

#include <QPointF>
#include <QRectF>
#include <QString>

#include "widgets.h"

class ListPane;
class QPainter;
class QTimer;

class RegionPage : public PageWidget
{
    Q_OBJECT

public:
    explicit RegionPage(QWidget *parent = nullptr);

    QString title() const override { return tr("Region & Language"); }
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;

    /*
     * Put the remembered zone into this process, at startup, before anything
     * draws a clock.
     *
     * STATIC AND CALLED FROM main(), because the page that owns the setting is
     * not built until the Dashboard is and the status bar starts telling the time
     * a few milliseconds later.  It only ever sets TZ on mixdash itself -- it
     * writes nothing and needs no privilege -- so it is safe on a read-only
     * rootfs, which is the case it exists for: see Settings::timezone().
     */
    static void applyStoredTimezone();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private slots:
    void onZoneActivated(int index);
    void onLanguageActivated(int index);
    void onTick();

private:
    enum Pane {
        PaneMap = 0,
        PaneZones,
        PaneLanguage,
        PaneCount
    };

    void setPane(int pane);
    void rebuildZones();
    void rebuildLanguages();

    /* Where the map is drawn inside the sheet, and the projection onto it.  Plain
     * equirectangular: x is longitude and y is latitude, which is the projection
     * a 2:1 rectangle already implies and the only one whose inverse -- needed by
     * the pointer -- is two divisions. */
    QRectF mapRect() const;
    QPointF project(int lon, int lat, const QRectF &r) const;

    void paintMap(QPainter &p);
    void paintFooter(QPainter &p, const QRectF &body);
    void paintOffer(QPainter &p, const QRectF &body);

    /* Nearest dot in the pressed direction, which is what a D-pad over a map has
     * to mean: there are no rows to step through. */
    void stepMap(int dx, int dy);

    void commit(int zone);
    bool writeZone(const QString &zone, QString *how);

    /* "UTC+02:00 -- 14:32", for the city under the crosshair.  Built when the
     * cursor moves and on the tick, and NOT in paintEvent: reading a zone means
     * opening its file under /usr/share/zoneinfo, and doing that per frame on
     * this board is a stutter you can feel through the D-pad. */
    void refreshCursorInfo();

    int m_pane = PaneMap;
    /* The dot under the crosshair.  Not the zone in force: moving the cursor
     * shows a city and changes nothing until A. */
    int m_cursor = 0;
    /* The zone that is actually in force, or -1 when it is one this table does
     * not carry -- which is not an error, only a map with no ring on it. */
    int m_applied = -1;
    /* While >= 0, the page is asking whether to switch language too and A and B
     * are the answer rather than their usual selves. */
    int m_offer = -1;

    ListPane *m_zones = nullptr;
    ListPane *m_langs = nullptr;
    QTimer *m_clock = nullptr;

    /* Which mechanism took the last write -- timedatectl, the symlink, or
     * neither.  Printed on the page, because "the time zone did not stick" is
     * otherwise a question nobody on this board can answer. */
    QString m_how;
    QString m_cursorInfo;
};

#endif /* MIXDASH_REGION_H */
