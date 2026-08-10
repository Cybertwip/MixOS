/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * dashboard.h -- the shell itself: four pages, one dock, one input path.
 */
#ifndef MIXDASH_DASHBOARD_H
#define MIXDASH_DASHBOARD_H

#include <QString>
#include <QStringList>
#include <QWidget>

#include "widgets.h"

class Joypad;
class QFileSystemModel;
class QLabel;
class QListView;
class QTimer;

/*
 * The file browser.  QFileSystemModel and QListView do the work; what is written
 * here is the D-pad contract -- up and down move, A descends or opens, B climbs and
 * then leaves the page at the top.  A real file manager is a later iteration; this
 * one exists because a dashboard that cannot show you the card is not much of a
 * shell.
 */
class FilesPage : public QWidget
{
    Q_OBJECT

public:
    explicit FilesPage(QWidget *parent = nullptr);

    void step(int delta);
    void enter();
    /* False when there is nowhere further up, which the Dashboard reads as
     * "go back to Apps". */
    bool leave();
    QString rootPath() const { return m_root; }

signals:
    void openRequested(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setRoot(const QString &path);

    QFileSystemModel *m_model = nullptr;
    QListView *m_view = nullptr;
    QString m_root;
    QString m_base;
};

class Dashboard : public QWidget
{
    Q_OBJECT

public:
    enum Internal {
        InternalNone = 0,
        InternalFiles,
        InternalInfo,
        InternalPower,
        InternalReboot,
        InternalPoweroff,
        InternalConsole
    };

    explicit Dashboard(QWidget *parent = nullptr);

public slots:
    void onNav(int action);

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

private:
    void buildPages();
    void setPage(int page);
    void activate(const AppEntry &entry);
    void launch(const QString &title, const QString &exe, const QStringList &args);
    void toast(const QString &text, int ms = 2400);
    static QString firstExisting(const QStringList &candidates);
    static QString firstWad();

    StatusBar *m_bar = nullptr;
    CardGrid *m_apps = nullptr;
    FilesPage *m_files = nullptr;
    InfoPage *m_info = nullptr;
    CardGrid *m_power = nullptr;
    Dock *m_dock = nullptr;
    QLabel *m_toast = nullptr;
    QTimer *m_toastTimer = nullptr;
    Joypad *m_pad = nullptr;

    int m_page = 0;
    int m_armed = InternalNone;
};

#endif /* MIXDASH_DASHBOARD_H */
