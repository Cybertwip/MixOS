/* SPDX-License-Identifier: MS-PL */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * mixdash -- MixOS's dashboard for the J36 Ultra.
 *
 * WHY THIS EXISTS.  EmulationStation reaches the panel through SDL's KMSDRM
 * backend, which means EGL, which means GBM, which means Mesa's lima on a
 * Mali-450 -- five layers, and the screen stayed black with no error past
 * eglCreateContext.  This dashboard reaches the panel through none of them.
 *
 * WHY linuxfb AND NOT eglfs.  The LK has already lit this panel: it powered the
 * JD9365, pushed its 155 init records, turned the backlight on and left the display
 * controller scanning out of 0x82700000.  The device tree hands that buffer to
 * `simple-framebuffer', CONFIG_DRM_FBDEV_EMULATION is off so mediatek-drm never
 * takes /dev/fb0 away from it, and mediatek-drm programs no register until
 * somebody sets a mode.  So /dev/fb0 is a live 640x480 x8r8g8b8 window onto glass
 * that is already lit, and Qt's linuxfb plugin writes straight into it.  No EGL, no
 * GBM, no DRM master, no modeset, and nothing in the path that has not already been
 * proved by the kernel console drawing on this panel.
 *
 * WHY Qt AND NOT A HAND-WRITTEN COMPOSITOR.  Because the alternative was writing
 * one: rounded rectangles, gradients, alpha, a bitmap font, text metrics, elision,
 * a list view, a scroll bar.  Qt has all of that, Debian trixie packages it for
 * armhf, and `libqt5gui5t64' ships platforms/libqlinuxfb.so ready to use.  What is
 * ported from MVII's dashboard is its look -- theme.h is its palette, to the triple
 * -- and deliberately not its renderer.
 *
 * WHAT IT DOES NOT DO.  It does not draw with the GPU, so a child that sets its own
 * mode through /dev/dri/card0 takes the scanout with it and does not give it back.
 * That is a real limit and it is written down in launch() where it bites.
 */
#include "dashboard.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QScreen>
#include <QStringList>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

/*
 * --probe runs before QApplication on purpose.  If the platform plugin is missing
 * or the framebuffer is not what the device tree said, this still answers, and its
 * answer is the first thing worth having from a boot that drew nothing.
 */
int probe(void)
{
    const int fd = ::open("/dev/fb0", O_RDONLY);
    if (fd < 0) {
        ::printf("mixdash: /dev/fb0 cannot be opened (%s)\n", ::strerror(errno));
        ::printf("mixdash: FB_SIMPLE should have bound the device tree's "
                 "simple-framebuffer node -- check dmesg for simple-framebuffer\n");
        return 1;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    ::memset(&var, 0, sizeof(var));
    ::memset(&fix, 0, sizeof(fix));
    if (::ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0
        || ::ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        ::printf("mixdash: /dev/fb0 opened but answered no geometry (%s)\n", ::strerror(errno));
        ::close(fd);
        return 1;
    }

    ::printf("mixdash: /dev/fb0 is \"%s\", %ux%u, %u bpp, stride %u, %u bytes mapped\n",
             fix.id, var.xres, var.yres, var.bits_per_pixel, fix.line_length, fix.smem_len);
    ::printf("mixdash: channels r%u@%u g%u@%u b%u@%u a%u@%u\n",
             var.red.length, var.red.offset, var.green.length, var.green.offset,
             var.blue.length, var.blue.offset, var.transp.length, var.transp.offset);

    /*
     * 32 bits per pixel is what the device tree's `x8r8g8b8' promises and what Qt's
     * raster engine can blit into without a conversion pass.  16 would still work
     * and would still be worth saying out loud.
     */
    if (var.bits_per_pixel != 32 && var.bits_per_pixel != 16)
        ::printf("mixdash: %u bpp is neither 16 nor 32; linuxfb will refuse it\n",
                 var.bits_per_pixel);

    ::close(fd);
    return 0;
}

/*
 * One font, found rather than assumed.  The payload's own copy comes first so the
 * dashboard looks the same on a card whose rootfs has no fonts at all; the rootfs
 * copies are the fallback, and if neither is there Qt falls back to whatever
 * fontconfig can find and the dashboard still draws.
 */
QString loadFont()
{
    QStringList files;
    const QDir payload("/run/j36/qt/fonts");
    if (payload.exists())
        for (const QString &f : payload.entryList(QStringList() << "*.ttf", QDir::Files, QDir::Name))
            files << payload.filePath(f);
    files << "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
          << "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
          << "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf";

    QString family;
    for (const QString &f : files) {
        if (!QFileInfo::exists(f))
            continue;
        const int id = QFontDatabase::addApplicationFont(f);
        if (id < 0)
            continue;
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty() && family.isEmpty())
            family = families.first();
    }
    return family;
}

} /* namespace */

int main(int argc, char **argv)
{
    bool once = false;
    bool windowed = false;
    bool qtInput = false;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!::strcmp(a, "--probe"))
            return probe();
        if (!::strcmp(a, "--once"))
            once = true;
        else if (!::strcmp(a, "--windowed"))
            windowed = true;
        else if (!::strcmp(a, "--qt-input"))
            qtInput = true;
        else if (!::strcmp(a, "--help") || !::strcmp(a, "-h")) {
            ::printf("mixdash -- the MixOS dashboard\n"
                     "  --probe      report /dev/fb0's geometry and exit\n"
                     "  --once       draw one screen, wait three seconds, exit 0\n"
                     "  --windowed   do not go fullscreen (for a development machine)\n"
                     "  --qt-input   let Qt read evdev instead of the built-in map\n");
            return 0;
        }
    }

    /*
     * Set before QApplication, because the platform plugin reads its environment
     * while it is being constructed.  Defaults only: an explicit QT_QPA_PLATFORM
     * from the unit file or the command line still wins, which is how this same
     * binary runs under X or Wayland on a development machine.
     */
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "linuxfb");
    if (!qtInput && !qEnvironmentVariableIsSet("QT_QPA_FB_DISABLE_INPUT"))
        qputenv("QT_QPA_FB_DISABLE_INPUT", "1");

    QApplication app(argc, argv);
    app.setApplicationName("mixdash");
    app.setApplicationDisplayName("MixOS");
    app.setOverrideCursor(Qt::BlankCursor);

    const QString family = loadFont();
    if (!family.isEmpty()) {
        QFont f(family);
        f.setPixelSize(13);
        app.setFont(f);
    } else {
        QFont f = app.font();
        f.setPixelSize(13);
        app.setFont(f);
    }

    Dashboard dash;
    if (windowed) {
        dash.resize(640, 480);
        dash.show();
    } else {
        dash.showFullScreen();
    }

    /*
     * One line in the journal that says what actually happened, because this is the
     * line that will be pasted back after a boot: the platform Qt chose, the size it
     * chose it at, and the font it found.
     */
    const QScreen *screen = QApplication::primaryScreen();
    ::printf("mixdash: %s on %s, %dx%d, font \"%s\"\n",
             QT_VERSION_STR,
             qPrintable(QApplication::platformName()),
             screen ? screen->geometry().width() : 0,
             screen ? screen->geometry().height() : 0,
             family.isEmpty() ? "fontconfig default" : qPrintable(family));
    ::fflush(stdout);

    if (once)
        QTimer::singleShot(3000, &app, []() { QApplication::quit(); });

    return app.exec();
}
