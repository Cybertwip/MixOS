/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* desktop.h -- the graphical session represented as its live window cards. */
#ifndef MIXDASH_DESKTOP_H
#define MIXDASH_DESKTOP_H

#include "widgets.h"

#include <QVector>

class QTimer;

class DesktopPage : public PageWidget
{
    Q_OBJECT

public:
    explicit DesktopPage(QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;
    void handleNavRelease(int action) override;
    void onEnter() override;
    void onLeave() override;

signals:
    /* Zero asks for the empty desktop; a positive value is a client pid. */
    void windowRequested(qint64 pid);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void refresh();
    void activate(int index);

private:
    struct Window {
        qint64 pid = 0;
        QString title;
    };

    QVector<Window> readWindows() const;
    void rebuild();

    CardGrid *m_grid = nullptr;
    QTimer *m_timer = nullptr;
    QVector<Window> m_windows;
};

#endif /* MIXDASH_DESKTOP_H */
