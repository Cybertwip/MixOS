/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * diagnostics.h -- what the "3D cube" card grew into.
 *
 * THE CARD IT REPLACES ran eglprobe -c 20, which exited 1, and the dashboard said
 * "3D cube exited 1" -- true, useless, and the reason this page exists.  eglprobe
 * fails at display_node(): it walks /dev/dri looking for a card with a CRTC and a
 * connector to flip on, and on this board there is none.  lima registers as a
 * RENDER-ONLY driver -- no DRIVER_MODESET, no CRTC, no connector, no
 * /sys/class/drm/card0-* at all -- because the display side of an MT6592 is
 * mtk_drm, which in this image is a module nobody loads.  Every derivative fails
 * the same way for the same reason: mpv's gpu output, SDL's KMSDRM backend, kmscube.
 * There is exactly one thing that reaches this panel, and it is the LK's
 * simple-framebuffer at 0x82700000 that mixdash is drawing into right now.
 *
 * SO THE CUBE IS STILL HERE, and it still turns -- rasterised by QPainter into
 * that framebuffer instead of by the GPU into a buffer nothing can scan out.  It
 * is now a measurement rather than a demo: the frame rate it reports is what this
 * CPU can do at 640x480, which is the number every other decision on this device
 * has to be made against.
 *
 * AND THE REST OF THE PAGE ANSWERS THE QUESTION THE CARD RAISED -- for the display
 * stack, the input devices, sound, USB and power.  Each row is a probe with the
 * evidence next to it, so a failure names its own cause instead of an exit code.
 */
#ifndef MIXDASH_DIAGNOSTICS_H
#define MIXDASH_DIAGNOSTICS_H

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "widgets.h"

class Joypad;
class ListPane;
class QTimer;

class DiagnosticsPage : public PageWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsPage(Joypad *pad, QWidget *parent = nullptr);

    QString title() const override;
    bool handleNav(int action) override;
    void onEnter() override;
    void onLeave() override;
    bool wantsFullscreen() const override { return m_cube; }

signals:
    /* The shell owns launching: it is the thing that can suspend the pad, warn
     * about a child that keeps the panel, and put the toast up afterwards. */
    void launchRequested(const QString &title, const QString &exe,
                         const QStringList &args, bool confirm);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onActivated(int index);
    void spin();

private:
    /* Rows the page can act on.  Everything else is a reading. */
    enum RowKind {
        RowReading = 0,
        RowCube = 1000,
        RowEglProbe,
        RowRescanInput,
        RowRefresh,
        RowLoadMtkDrm
    };

    struct Finding {
        QString name;
        QString detail;
        QString badge;
        QColor colour;
    };

    void probe();
    void rebuild();

    void probeDisplay(QVector<Finding> &out);
    void probeGpu(QVector<Finding> &out);
    void probeInput(QVector<Finding> &out);
    void probeAudio(QVector<Finding> &out);
    void probeUsb(QVector<Finding> &out);
    void probePower(QVector<Finding> &out);
    void probeSystem(QVector<Finding> &out);

    void paintCube(QPainter &p);

    Joypad *m_pad = nullptr;
    ListPane *m_list = nullptr;
    QTimer *m_timer = nullptr;

    /* Section name, then the findings under it, in the order they are shown. */
    QVector<QPair<QString, QVector<Finding> > > m_sections;

    /* The render test. */
    bool m_cube = false;
    double m_angle = 0.0;
    QElapsedTimer m_frameClock;
    QElapsedTimer m_fpsWindow;
    int m_framesInWindow = 0;
    double m_fps = 0.0;
    double m_worstMs = 0.0;
};

#endif /* MIXDASH_DIAGNOSTICS_H */
