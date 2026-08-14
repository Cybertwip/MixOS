/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * packages.h -- Debian's archive, from the handheld.
 *
 * THE POINT OF THE WHOLE EXERCISE.  Everything else in this shell is fixed at
 * build time; this is the page that makes the device a computer rather than an
 * appliance.  The rootfs under it is real Debian, so the entire archive is one
 * `apt-get install' away -- including a desktop environment, which is the case
 * this was asked for: someone who wants KDE on this thing should be able to get
 * KDE on this thing.
 *
 * WHAT IT DRIVES AND WHY.
 *   - Reading is done here, with apt-cache and dpkg-query, because it is fast,
 *     bounded and needs no privileges.
 *   - WRITING IS NOT.  Installs and removals are handed to the Terminal page and
 *     run there as a visible apt-get.  That is deliberate: an install can take ten
 *     minutes, can ask a debconf question, can fail on a conflict, and can want a
 *     y/n answer.  A progress bar that hides all of that would turn every one of
 *     those into "it stopped".  The Terminal already renders apt's own output,
 *     which is the best UI anyone has ever built for apt.
 *
 * A NOTE ON THE COLLECTIONS.  The curated list at the top is not a package
 * manager feature, it is a shortcut: on a 640x480 screen with a D-pad, finding
 * `kde-plasma-desktop' by typing it is a minute of work with the on-screen
 * keyboard.  Search is still there for everything else.
 */
#ifndef MIXDASH_PACKAGES_H
#define MIXDASH_PACKAGES_H

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "widgets.h"

class ListPane;
class QTimer;

class PackagesPage : public PageWidget
{
    Q_OBJECT

public:
    explicit PackagesPage(QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;
    void textEntered(const QString &text, bool accepted) override;

signals:
    /* Run this in the Terminal and switch to it.  The shell wires it up. */
    void terminalRequested(const QString &command);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);

private:
    enum View { ViewHome = 0, ViewCollection, ViewSearch };

    enum RowKind {
        RowPackage = 0,
        RowCollection = 1000,
        RowSearch,
        RowUpdate,
        RowUpgrade,
        RowClean,
        RowBack,
        RowInstalledList
    };

    struct Pkg {
        QString name;
        QString summary;
        bool installed = false;
    };

    struct Collection {
        QString title;
        QString subtitle;
        QColor accent;
        int glyph;
        /* The metapackages, in the order they should be offered. */
        QStringList packages;
    };

    QString run(const QString &program, const QStringList &args, int timeoutMs = 8000) const;
    void loadInstalled();
    bool isInstalled(const QString &name) const;
    QString summaryFor(const QString &name) const;

    void buildCollections();
    void showHome();
    void showCollection(int index);
    void showSearch(const QString &term);
    void rebuild();

    void install(const QString &name);
    void remove(const QString &name);

    ListPane *m_list = nullptr;
    QVector<Collection> m_collections;
    QVector<Pkg> m_shown;      /* whatever the current view lists */
    QSet<QString> m_installed;

    int m_view = ViewHome;
    int m_collection = -1;
    QString m_term;
    QString m_note;
    bool m_awaitingSearch = false;
    /* The package a press is asking about, while the confirm prompt is up. */
    QString m_armed;
};

#endif /* MIXDASH_PACKAGES_H */
