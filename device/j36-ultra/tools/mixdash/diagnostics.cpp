/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 */
#include "diagnostics.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTimer>

#include <math.h>
#include <sys/statvfs.h>

#include "glvideo.h"
#include "joypad.h"
#include "theme.h"

namespace {

QString readFirstLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromLocal8Bit(f.readLine()).trimmed();
}

/* ── WHAT THE INITRAMFS DID TO THE CARD, WHICH NOTHING ELSE CAN STILL SAY ──
 *
 * /init grows the OS partition to the size of the card before it hands over, and
 * everything it says about that goes to a console the splash owns.  There is no
 * journal to fall back on -- cleanup_filesystem.sh deletes /var/log/journal -- so by
 * the time anybody is in a position to ask, the only account of the step is gone,
 * and "it just skipped" was as much as anyone could report.  So /init writes one
 * line per boot to the file below, and this reads the last of them.
 *
 * From the END of the file: it is appended to on every boot, and the line that
 * matters is the newest.  The seek is because "read it all and take the last" is a
 * habit that only looks harmless until the file has been appended to for a year.
 */
QString lastLineOf(const QString &path, qint64 tailBytes = 4096)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    if (f.size() > tailBytes)
        f.seek(f.size() - tailBytes);
    const QStringList lines = QString::fromLocal8Bit(f.readAll())
                                  .split('\n', Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (!line.isEmpty())
            return line;
    }
    return QString();
}

bool moduleLoaded(const QString &name)
{
    /* /sys/module is the honest answer for both a module and something built in;
     * /proc/modules only lists what was inserted. */
    return QFileInfo::exists("/sys/module/" + name);
}

QString linkTarget(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isSymLink())
        return QString();
    return QFileInfo(info.symLinkTarget()).fileName();
}

QString firstExisting(const QStringList &paths)
{
    for (const QString &p : paths)
        if (QFileInfo(p).isExecutable())
            return p;
    return QString();
}

} /* namespace */

DiagnosticsPage::DiagnosticsPage(Joypad *pad, QWidget *parent)
    : PageWidget(parent)
    , m_pad(pad)
{
    m_list = new ListPane(this);
    m_list->setRowHeight(30);
    m_list->setPlaceholder(tr("Nothing probed yet."));
    connect(m_list, &ListPane::activated, this, &DiagnosticsPage::onActivated);

    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, &DiagnosticsPage::spin);
}

QString DiagnosticsPage::title() const
{
    return m_cube ? tr("Render test") : tr("Diagnostics");
}

void DiagnosticsPage::resizeEvent(QResizeEvent *event)
{
    const QRect card(Theme::Margin, Theme::Margin,
                     width() - 2 * Theme::Margin, height() - 2 * Theme::Margin);
    m_list->setGeometry(card.x() + 6, card.y() + 36 + 4, card.width() - 12,
                        card.height() - 36 - 10);
    QWidget::resizeEvent(event);
}

void DiagnosticsPage::onEnter()
{
    probe();
    rebuild();
}

void DiagnosticsPage::onLeave()
{
    m_timer->stop();
    m_cube = false;
    m_list->setVisible(true);
}

/* ── the probes ──────────────────────────────────────────────────────────── */

void DiagnosticsPage::probeDisplay(QVector<Finding> &out)
{
    Finding fb;
    fb.name = tr("Framebuffer");
    const QString size = SysInfo::readTrimmed("/sys/class/graphics/fb0/virtual_size");
    const QString bpp = SysInfo::readTrimmed("/sys/class/graphics/fb0/bits_per_pixel");
    const QString stride = SysInfo::readTrimmed("/sys/class/graphics/fb0/stride");
    const QString name = SysInfo::readTrimmed("/sys/class/graphics/fb0/name");
    if (size.isEmpty()) {
        fb.detail = tr("no /dev/fb0 -- and yet you are reading this");
        fb.badge = tr("odd");
        fb.colour = Theme::orange();
    } else {
        fb.detail = tr("%1  %2 bpp  stride %3  %4")
                        .arg(QString(size).replace(',', 'x')).arg(bpp).arg(stride).arg(name);
        fb.badge = tr("live");
        fb.colour = Theme::green();
    }
    out.append(fb);

    Finding painter;
    painter.name = tr("Painting path");
    painter.detail = tr("Qt raster -> linuxfb -> /dev/fb0.  No GL anywhere.");
    painter.badge = tr("ok");
    painter.colour = Theme::green();
    out.append(painter);
}

void DiagnosticsPage::probeGpu(QVector<Finding> &out)
{
    /* Which DRM devices exist and what drives them. */
    const QStringList cards = QDir("/sys/class/drm").entryList(QStringList() << "card*"
                                                                            << "renderD*",
                                                               QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList primary;
    QStringList render;
    QStringList connectors;
    for (const QString &c : cards) {
        if (c.startsWith("renderD"))
            render << c;
        else if (c.contains('-'))
            connectors << c;   /* card0-HDMI-A-1 and the like: a modesetting driver */
        else
            primary << c;
    }

    Finding drm;
    drm.name = tr("DRM nodes");
    if (primary.isEmpty() && render.isEmpty()) {
        drm.detail = tr("nothing in /sys/class/drm -- no DRM driver bound");
        drm.badge = tr("none");
        drm.colour = Theme::red();
    } else {
        QStringList bits;
        for (const QString &c : primary) {
            const QString driver = linkTarget("/sys/class/drm/" + c + "/device/driver");
            bits << (driver.isEmpty() ? c : (c + " (" + driver + ")"));
        }
        for (const QString &c : render) {
            const QString driver = linkTarget("/sys/class/drm/" + c + "/device/driver");
            bits << (driver.isEmpty() ? c : (c + " (" + driver + ")"));
        }
        drm.detail = bits.join(", ");
        drm.badge = QString::number(primary.size() + render.size());
        drm.colour = Theme::blue();
    }
    out.append(drm);

    /*
     * THE FINDING THIS PAGE WAS BUILT FOR.  A driver with no connector directory
     * has no KMS: it can rasterise into a buffer and it cannot put that buffer on
     * a panel.  Everything that "does not draw on the screen" on this board --
     * eglprobe, kmscube, mpv's gpu and drm outputs, SDL's KMSDRM backend -- stops
     * exactly here, and every one of them reports it as its own separate failure.
     */
    Finding kms;
    kms.name = tr("Mode setting (KMS)");
    if (connectors.isEmpty()) {
        kms.detail = tr("no card*-* connectors: the DRM driver is render-only.\n"
                        "Nothing but this framebuffer can reach the panel.");
        kms.badge = tr("absent");
        kms.colour = Theme::red();
    } else {
        kms.detail = connectors.join(", ");
        kms.badge = tr("present");
        kms.colour = Theme::green();
    }
    out.append(kms);

    Finding lima;
    lima.name = QStringLiteral("lima");
    if (moduleLoaded("lima")) {
        lima.detail = tr("bound -- Mali-450 rendering works, scanout does not");
        lima.badge = tr("loaded");
        lima.colour = Theme::green();
    } else {
        lima.detail = tr("not loaded");
        lima.badge = tr("no");
        lima.colour = Theme::ink3();
    }
    out.append(lima);

    Finding mtk;
    mtk.name = QStringLiteral("mtk_drm");
    if (moduleLoaded("mtk_drm") || moduleLoaded("mediatek_drm")) {
        mtk.detail = tr("loaded -- the display controller has a driver");
        mtk.badge = tr("loaded");
        mtk.colour = Theme::green();
    } else {
        mtk.detail = tr("not loaded.  This is the missing half: mtk_drm is the\n"
                        "CRTC and the connector lima has none of.");
        mtk.badge = tr("no");
        mtk.colour = Theme::orange();
    }
    out.append(mtk);

    /* The userspace half. */
    static const char *kLibDirs[] = { "/usr/lib/arm-linux-gnueabihf", "/usr/lib", "/lib" };
    QString egl;
    QString gles;
    QString limaDri;
    for (size_t i = 0; i < sizeof(kLibDirs) / sizeof(kLibDirs[0]); ++i) {
        const QString dir = QString::fromLatin1(kLibDirs[i]);
        if (egl.isEmpty() && QFileInfo::exists(dir + "/libEGL.so.1"))
            egl = dir + "/libEGL.so.1";
        if (gles.isEmpty() && QFileInfo::exists(dir + "/libGLESv2.so.2"))
            gles = dir + "/libGLESv2.so.2";
        if (limaDri.isEmpty() && QFileInfo::exists(dir + "/dri/lima_dri.so"))
            limaDri = dir + "/dri/lima_dri.so";
    }

    Finding mesa;
    mesa.name = tr("EGL / GLES userspace");
    QStringList have;
    if (!egl.isEmpty())     have << "libEGL";
    if (!gles.isEmpty())    have << "libGLESv2";
    if (!limaDri.isEmpty()) have << "lima_dri";
    if (have.isEmpty()) {
        mesa.detail = tr("none installed");
        mesa.badge = tr("none");
        mesa.colour = Theme::ink3();
    } else {
        mesa.detail = have.join(", ") + "  --  " + tr("present, but with no KMS to present to");
        mesa.badge = tr("present");
        mesa.colour = Theme::blue();
    }
    out.append(mesa);

    Finding probe;
    probe.name = QStringLiteral("eglprobe");
    const QString exe = firstExisting(QStringList() << "/run/j36/eglprobe"
                                                    << "/opt/mixos/bin/eglprobe");
    if (exe.isEmpty()) {
        probe.detail = tr("not on this card");
        probe.badge = tr("absent");
        probe.colour = Theme::ink3();
    } else {
        probe.detail = exe + "\n" + tr("-o and -z answer \"does the GPU draw\" without taking the panel.");
        probe.badge = tr("present");
        probe.colour = Theme::orange();
    }
    out.append(probe);

    /*
     * THE ROW THAT SAYS WHETHER A FILM WILL BE DRAWN BY THE GPU.  The media page
     * asks GlVideo for the same thing and quietly falls back to the software
     * painter when the answer is no, which is right for a person watching a film
     * and useless for one asking why it is slow.  So the reason is printed here,
     * and asking for it is what builds the path: dlopen libEGL, import the LK's
     * carveout from /dev/j36fb, link the shaders.  No modeset, nothing drawn --
     * opening this page cannot disturb what is on the screen.
     */
    Finding glv;
    glv.name = tr("GL video");
    if (GlVideo::instance()->available()) {
        const QSize s = GlVideo::instance()->size();
        glv.detail = GlVideo::instance()->reason() + "\n"
                     + tr("Films go up as three planes and a shader, not as %1 KB of "
                          "converted pixels per frame.")
                           .arg((s.width() * s.height() * 4) / 1024);
        glv.badge = tr("on");
        glv.colour = Theme::green();
    } else {
        glv.detail = GlVideo::instance()->reason() + "\n"
                     + tr("Films still play; the CPU converts and copies every frame.");
        glv.badge = tr("off");
        glv.colour = Theme::orange();
    }
    out.append(glv);
}

void DiagnosticsPage::probeInput(QVector<Finding> &out)
{
    Finding devices;
    devices.name = tr("evdev devices");
    if (!m_pad || m_pad->deviceCount() == 0) {
        devices.detail = tr("no /dev/input/event* opened");
        devices.badge = tr("none");
        devices.colour = Theme::red();
    } else {
        devices.detail = m_pad->deviceNames().join(", ");
        devices.badge = QString::number(m_pad->deviceCount());
        devices.colour = Theme::green();
    }
    out.append(devices);

    Finding pointer;
    pointer.name = tr("Pointing devices");
    const int mice = m_pad ? m_pad->mouseCount() : 0;
    pointer.detail = mice > 0 ? tr("%1 attached, plus the right stick").arg(mice)
                              : tr("none attached -- the right stick drives the pointer");
    pointer.badge = QString::number(mice);
    pointer.colour = mice > 0 ? Theme::green() : Theme::ink3();
    out.append(pointer);

    Finding keys;
    keys.name = tr("Keyboards");
    const int kbd = m_pad ? m_pad->keyboardCount() : 0;
    keys.detail = kbd > 0 ? tr("typing goes straight through to the Terminal")
                          : tr("none -- Menu puts the on-screen keyboard up");
    keys.badge = QString::number(kbd);
    keys.colour = kbd > 0 ? Theme::green() : Theme::ink3();
    out.append(keys);
}

void DiagnosticsPage::probeAudio(QVector<Finding> &out)
{
    Finding cards;
    cards.name = tr("ALSA cards");
    const QString list = SysInfo::readTrimmed("/proc/asound/cards");
    if (list.isEmpty() || list.contains("no soundcards")) {
        cards.detail = tr("none registered");
        cards.badge = tr("none");
        cards.colour = Theme::red();
    } else {
        /* The first line of each pair holds the name in brackets. */
        QStringList names;
        for (const QString &line : list.split('\n')) {
            const int open = line.indexOf('[');
            const int close = line.indexOf(']');
            if (open > 0 && close > open)
                names << line.mid(open + 1, close - open - 1).trimmed();
        }
        cards.detail = names.join(", ");
        cards.badge = QString::number(names.size());
        cards.colour = Theme::green();
    }
    out.append(cards);

    Finding tools;
    tools.name = tr("Playback tools");
    QStringList found;
    if (!firstExisting(QStringList() << "/usr/bin/aplay" << "/bin/aplay").isEmpty())
        found << "aplay";
    if (!firstExisting(QStringList() << "/usr/bin/ffmpeg" << "/bin/ffmpeg").isEmpty())
        found << "ffmpeg";
    if (!firstExisting(QStringList() << "/usr/bin/ffprobe").isEmpty())
        found << "ffprobe";
    tools.detail = found.isEmpty() ? tr("none -- Media cannot play anything")
                                   : found.join(", ");
    tools.badge = found.isEmpty() ? tr("none") : tr("ok");
    tools.colour = found.isEmpty() ? Theme::red() : Theme::green();
    out.append(tools);
}

void DiagnosticsPage::probeUsb(QVector<Finding> &out)
{
    Finding controller;
    controller.name = tr("USB controller");
    const QStringList hosts = QDir("/sys/bus/usb/devices").entryList(QStringList() << "usb*",
                                                                     QDir::Dirs);
    if (hosts.isEmpty()) {
        controller.detail = tr("no root hub -- musb did not bind");
        controller.badge = tr("down");
        controller.colour = Theme::red();
    } else {
        QStringList names;
        for (const QString &h : hosts)
            names << SysInfo::readTrimmed("/sys/bus/usb/devices/" + h + "/product");
        controller.detail = names.join(", ");
        controller.badge = tr("up");
        controller.colour = Theme::green();
    }
    out.append(controller);

    /*
     * WHAT IS PLUGGED IN, and how much current each one asked for.  bMaxPower is
     * the number the reported "usb devices not powering properly" is really about:
     * a device that asked for 500 mA and is being fed from a rail that cannot give
     * it enumerates, then browns out under load, which looks exactly like a device
     * that half works.
     */
    const QStringList devices = QDir("/sys/bus/usb/devices").entryList(
        QStringList() << "[0-9]*-[0-9]*", QDir::Dirs);
    int attached = 0;
    int requestedMa = 0;
    bool displaylink = false;
    QStringList lines;
    for (const QString &d : devices) {
        if (d.contains(':'))
            continue;   /* an interface, not a device */
        const QString base = "/sys/bus/usb/devices/" + d;
        const QString product = SysInfo::readTrimmed(base + "/product");
        const QString maxPower = SysInfo::readTrimmed(base + "/bMaxPower");
        const QString speed = SysInfo::readTrimmed(base + "/speed");
        /* Kept for the dock row below: DisplayLink's vendor ID is the one fact that
         * decides whether an HDMI socket on this bus can ever carry a picture. */
        if (SysInfo::readTrimmed(base + "/idVendor").compare(QLatin1String("17e9"),
                                                             Qt::CaseInsensitive) == 0)
            displaylink = true;
        ++attached;
        requestedMa += maxPower.left(maxPower.indexOf("mA")).trimmed().toInt();
        lines << tr("%1 (%2, %3 Mb/s)")
                     .arg(product.isEmpty() ? d : product)
                     .arg(maxPower.isEmpty() ? QString("?") : maxPower)
                     .arg(speed);
    }

    Finding attachedF;
    attachedF.name = tr("Attached devices");
    attachedF.detail = lines.isEmpty() ? tr("none") : lines.join("\n");
    attachedF.badge = QString::number(attached);
    attachedF.colour = attached > 0 ? Theme::green() : Theme::ink3();
    out.append(attachedF);

    Finding budget;
    budget.name = tr("Bus current asked for");
    budget.detail = tr("%1 mA requested by attached devices.\n"
                       "VBUS here is a load switch off VBAT, not a 5 V boost:\n"
                       "a battery at 3.6 V cannot make 5 V, and the switch cannot\n"
                       "raise it.  Anything that needs a real 500 mA at 5 V needs a\n"
                       "powered hub.").arg(requestedMa);
    budget.badge = requestedMa > 500 ? tr("over") : tr("info");
    budget.colour = requestedMa > 500 ? Theme::orange() : Theme::ink3();
    out.append(budget);

    Finding vbus;
    vbus.name = tr("VBUS switch");
    /* The gpio the device tree names j36,drvvbus-pad, if the driver exported it. */
    const QString musb = QDir("/sys/bus/platform/drivers/musb-mtk").exists()
                             ? QStringLiteral("musb-mtk")
                             : QDir("/sys/bus/platform/drivers/musb-hdrc").exists()
                                   ? QStringLiteral("musb-hdrc")
                                   : QString();
    if (musb.isEmpty()) {
        vbus.detail = tr("no musb driver bound");
        vbus.badge = tr("none");
        vbus.colour = Theme::red();
    } else {
        vbus.detail = musb + " " + tr("bound.  VBUS is driven from the pad the device tree\n"
                                      "calls j36,drvvbus-pad through an external load switch.");
        vbus.badge = tr("ok");
        vbus.colour = Theme::green();
    }
    out.append(vbus);

    probeDock(out, displaylink);
}

/*
 * ── THE DOCK, AND WHY IT NEEDED A ROW OF ITS OWN ─────────────────────────────
 *
 * j36-mixmirror runs from boot and copies the panel onto a DisplayLink adapter
 * whenever one turns up.  Its steady state is silence, which is right for a
 * journal and was exactly wrong for the person holding the handheld: "the mirror
 * never runs" and "the mirror is running and there is nothing to mirror onto"
 * looked identical from here -- a black television either way, and no way to
 * tell them apart without a serial console.
 *
 * So the mirror now publishes one keyword and one sentence to
 * /run/j36/mirror.status on every state change, and the first half of this shows
 * it.  The colour comes from the keyword, never from the sentence, so the
 * sentence can be reworded without this file caring.
 *
 * The second half is for when the file is not there at all, which means the
 * mirror is not running -- and that is worth saying plainly rather than papering
 * over with an empty row.  In that case the same three facts the mirror would
 * have measured are read here directly: is udl.ko loaded, is there a card node
 * bound to udl, and is there anything with DisplayLink's vendor ID on the bus.
 * Pure sysfs, no ioctl, nothing opened that could take a modeset away from
 * something else.
 */
void DiagnosticsPage::probeDock(QVector<Finding> &out, bool displaylink)
{
    Finding dock;
    dock.name = tr("USB-HDMI dock");

    const QString status = SysInfo::readTrimmed("/run/j36/mirror.status");
    if (!status.isEmpty()) {
        const int nl = status.indexOf('\n');
        const QString word = nl < 0 ? status : status.left(nl);
        const QString said = nl < 0 ? QString() : status.mid(nl + 1).trimmed();

        dock.detail = said.isEmpty() ? word : said;
        if (word == QLatin1String("mirroring")) {
            dock.badge = tr("on");
            dock.colour = Theme::green();
        } else if (word == QLatin1String("no-screen") || word == QLatin1String("no-mode")) {
            dock.badge = tr("no tv");
            dock.colour = Theme::orange();
        } else if (word == QLatin1String("no-adapter")) {
            /* Not a fault: this is what an undocked handheld says, all day. */
            dock.badge = tr("none");
            dock.colour = Theme::ink3();
        } else if (word == QLatin1String("no-drm") || word == QLatin1String("stopped")) {
            dock.badge = word == QLatin1String("stopped") ? tr("stopped") : tr("no drm");
            dock.colour = Theme::red();
        } else {
            dock.badge = tr("wait");
            dock.colour = Theme::ink3();
        }
        out.append(dock);
        return;
    }

    /* No status file, so the mirror is not running.  Work out what it would have
     * found, and say which of the two things went wrong. */
    QStringList cards;
    QString udlCard;
    const QStringList nodes = QDir("/sys/class/drm").entryList(QStringList() << "card*",
                                                               QDir::Dirs);
    for (const QString &c : nodes) {
        if (c.contains('-'))
            continue;   /* card0-DSI-1 and friends are connectors, not cards */
        const QString drv = linkTarget("/sys/class/drm/" + c + "/device/driver");
        cards << c + "=" + (drv.isEmpty() ? QStringLiteral("?") : drv);
        if (drv == QLatin1String("udl"))
            udlCard = c;
    }

    QStringList why;
    why << tr("j36-mixmirror is not reporting: %1 is absent, so the\n"
              "service is not running.  systemctl status j36-mixmirror says why.")
               .arg("/run/j36/mirror.status");
    why << tr("udl.ko: %1").arg(moduleLoaded("udl") ? tr("loaded") : tr("not loaded"));
    why << tr("/dev/dri: %1").arg(cards.isEmpty() ? tr("no cards at all") : cards.join(", "));
    if (!udlCard.isEmpty())
        why << tr("%1 is a DisplayLink display and nothing is driving it.").arg(udlCard);
    else if (displaylink)
        why << tr("A DisplayLink device is on the bus and udl did not claim it.\n"
                  "Mainline udl speaks the USB 2.0 DL-1x0/DL-1x5 protocol only.");
    else
        why << tr("Nothing on the bus is DisplayLink (vendor 17e9).  A USB-C hub\n"
                  "whose HDMI is DisplayPort Alt Mode cannot work here -- MT6592\n"
                  "has no DisplayPort.");

    dock.detail = why.join("\n");
    dock.badge = tr("off");
    dock.colour = Theme::red();
    out.append(dock);
}

void DiagnosticsPage::probePower(QVector<Finding> &out)
{
    const QStringList supplies = QDir("/sys/class/power_supply").entryList(QDir::Dirs
                                                                          | QDir::NoDotAndDotDot);
    Finding battery;
    battery.name = tr("Power supplies");
    if (supplies.isEmpty()) {
        battery.detail = tr("nothing in /sys/class/power_supply.\n"
                            "The PMIC has no Linux driver yet -- charge state,\n"
                            "battery voltage and the USB current limit are all\n"
                            "invisible from userspace until it does.");
        battery.badge = tr("none");
        battery.colour = Theme::orange();
    } else {
        QStringList bits;
        for (const QString &s : supplies) {
            const QString base = "/sys/class/power_supply/" + s;
            const QString capacity = SysInfo::readTrimmed(base + "/capacity");
            const QString status = SysInfo::readTrimmed(base + "/status");
            bits << QString("%1 %2%3").arg(s)
                        .arg(capacity.isEmpty() ? QString() : capacity + "%  ")
                        .arg(status);
        }
        battery.detail = bits.join("\n");
        battery.badge = QString::number(supplies.size());
        battery.colour = Theme::green();
    }
    out.append(battery);

    /*
     * ── WHICH SOCKET IS DOING WHAT ───────────────────────────────────────────
     *
     * There are two of them on a J36 Ultra and telling them apart from the
     * outside is the whole difficulty.  The DC inlet charges and has no data
     * lines; the OTG port carries data and sources 5 V the whole time the board
     * is on.  The driver was written believing they were one socket, so it held
     * the charger off whenever the port was sourcing -- which is always -- and
     * the console ran its battery down on the mains for a whole release while
     * every reading involved said something true and unhelpful.
     *
     * So this row puts the two side by side and names them.  `online' is CHRDET,
     * which is the inlet; vbus_sourcing is the DRVVBUS pad, which is the OTG
     * port; and the sign of current_now is the only one of the three that says
     * whether any of it is working.  A charger in the inlet with the current
     * still negative is the bug coming back, and this is the row it shows up on.
     */
    const QString usbSupply = QStringLiteral("/sys/class/power_supply/usb");
    if (QDir(usbSupply).exists()) {
        Finding ports;
        ports.name = tr("Connectors");
        const bool online = SysInfo::readTrimmed(usbSupply + "/online")
                            == QLatin1String("1");
        const QString sourcing = SysInfo::readTrimmed(usbSupply + "/vbus_sourcing");
        const int ua = SysInfo::readTrimmed(
                           "/sys/class/power_supply/battery/current_now").toInt();

        QStringList lines;
        lines << tr("DC inlet: %1 (CHRDET)")
                     .arg(online ? tr("a charger is attached") : tr("nothing attached"));
        lines << tr("OTG port: %1 (DRVVBUS pad)")
                     .arg(sourcing == QLatin1String("1")
                              ? tr("sourcing 5 V, so bus-powered devices work")
                              : sourcing == QLatin1String("0")
                                    ? tr("not sourcing")
                                    : tr("no pad in the device tree"));
        lines << tr("Cell: %1 mA").arg(ua / 1000);

        if (online && ua > 0) {
            ports.badge = tr("charging");
            ports.colour = Theme::green();
        } else if (online) {
            /* A charger in and nothing going into the cell.  Full is the benign
             * reading and the gauge says which. */
            const bool full = SysInfo::readTrimmed(
                                  "/sys/class/power_supply/battery/status")
                              == QLatin1String("Full");
            lines << (full ? tr("The cell is full, so nothing is being taken in.")
                           : tr("A charger is attached and the cell is not taking\n"
                                "current in.  chrin_shared in j36_mt6592_pmic holds\n"
                                "the charger off while the OTG port is up; it is 0\n"
                                "on this board and should stay 0."));
            ports.badge = full ? tr("full") : tr("check");
            ports.colour = full ? Theme::green() : Theme::orange();
        } else {
            ports.badge = tr("battery");
            ports.colour = Theme::ink3();
        }
        ports.detail = lines.join("\n");
        out.append(ports);
    }

    Finding thermal;
    thermal.name = tr("Temperature");
    const QStringList zones = QDir("/sys/class/thermal").entryList(QStringList() << "thermal_zone*",
                                                                   QDir::Dirs);
    if (zones.isEmpty()) {
        thermal.detail = tr("no thermal zones");
        thermal.badge = tr("none");
        thermal.colour = Theme::ink3();
    } else {
        QStringList bits;
        int hottest = 0;
        for (const QString &z : zones) {
            const int milli = SysInfo::readTrimmed("/sys/class/thermal/" + z + "/temp").toInt();
            if (milli <= 0)
                continue;
            hottest = qMax(hottest, milli / 1000);
            bits << QString("%1 %2 C").arg(SysInfo::readTrimmed("/sys/class/thermal/" + z + "/type"))
                        .arg(milli / 1000);
        }
        thermal.detail = bits.join(", ");
        thermal.badge = QString("%1 C").arg(hottest);
        thermal.colour = hottest > 80 ? Theme::red() : hottest > 65 ? Theme::orange() : Theme::green();
    }
    out.append(thermal);
}

void DiagnosticsPage::probeSystem(QVector<Finding> &out)
{
    Finding kernel;
    kernel.name = tr("Kernel");
    kernel.detail = SysInfo::readTrimmed("/proc/sys/kernel/osrelease") + "  "
                    + SysInfo::readTrimmed("/proc/sys/kernel/version");
    kernel.badge = tr("info");
    kernel.colour = Theme::ink3();
    out.append(kernel);

    Finding cpu;
    cpu.name = tr("CPU");
    QString model;
    int cores = 0;
    /* SysInfo::readLines and not a readLine() loop, here and below: atEnd() is
     * true from the start on anything in /proc.  See widgets.cpp. */
    const QStringList cpuLines = SysInfo::readLines("/proc/cpuinfo");
    for (const QString &line : cpuLines) {
        if (line.startsWith("processor"))
            ++cores;
        else if (model.isEmpty() && line.startsWith("model name"))
            model = line.section(':', 1).trimmed();
        else if (model.isEmpty() && line.startsWith("Hardware"))
            model = line.section(':', 1).trimmed();
    }
    const int khz = SysInfo::readTrimmed(
                        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq").toInt();
    cpu.detail = tr("%1  --  %2 cores%3")
                     .arg(model.isEmpty() ? tr("unknown") : model)
                     .arg(cores)
                     .arg(khz > 0 ? tr(", cpu0 at %1 MHz").arg(khz / 1000) : QString());
    cpu.badge = QString::number(cores);
    cpu.colour = Theme::ink3();
    out.append(cpu);

    Finding mem;
    mem.name = tr("Memory");
    long totalKb = 0;
    long availKb = 0;
    const QStringList memLines = SysInfo::readLines("/proc/meminfo");
    for (const QString &line : memLines) {
        if (line.startsWith("MemTotal:"))
            totalKb = line.section(':', 1).trimmed().split(' ').first().toLong();
        else if (line.startsWith("MemAvailable:"))
            availKb = line.section(':', 1).trimmed().split(' ').first().toLong();
    }
    mem.detail = tr("%1 MB total, %2 MB available")
                     .arg(totalKb / 1024).arg(availKb / 1024);
    mem.badge = totalKb > 0 ? QString("%1%").arg(100 - (availKb * 100 / qMax(1L, totalKb)))
                            : tr("?");
    mem.colour = Theme::ink3();
    out.append(mem);

    Finding root;
    root.name = tr("Root filesystem");
    struct statvfs vfs;
    if (::statvfs("/", &vfs) == 0) {
        const qulonglong total = (qulonglong)vfs.f_blocks * vfs.f_frsize;
        const qulonglong free = (qulonglong)vfs.f_bavail * vfs.f_frsize;
        root.detail = tr("%1 MB free of %2 MB")
                          .arg(free / (1024 * 1024)).arg(total / (1024 * 1024));
        const int usedPercent = total ? (int)(100 - free * 100 / total) : 0;
        root.badge = QString("%1%").arg(usedPercent);
        root.colour = usedPercent > 92 ? Theme::red() : Theme::ink3();
    } else {
        root.detail = tr("statvfs failed");
        root.badge = tr("?");
        root.colour = Theme::ink3();
    }
    out.append(root);

    /* Directly under the size, because the size is what makes somebody ask.  A 4 GB
     * root on a 32 GB card is the whole of the complaint this row exists to answer,
     * and the answer is one line /init left behind on its way past. */
    Finding grown;
    grown.name = tr("Card expansion");
    const QString note = lastLineOf("/var/log/mixos-expand.log");
    if (note.isEmpty()) {
        /* Not a fault.  A card flashed by a build older than the log, or one whose
         * /var/log was cleared, has nothing to say here and is not broken for it. */
        grown.detail = tr("no record on this card -- /init has not written one yet");
        grown.badge = tr("info");
        grown.colour = Theme::ink3();
    } else {
        /* "[+12.34s] " is /init's own stamp: uptime, because this board has no RTC
         * and a wall clock in the initramfs is 1970 on every boot.  It says which
         * moment of the boot, which is worth nothing to the reader of one line. */
        QString text = note;
        if (text.startsWith('['))
            text = text.section(']', 1).trimmed();
        const bool full = (text.contains("now fills") && !text.contains("READ-ONLY"))
                          || text.contains("nothing to grow");
        grown.detail = text;
        grown.badge = full ? tr("done") : tr("check");
        /* Orange for anything unrecognised as well as for the known failures: a line
         * this does not recognise is precisely a line somebody should read. */
        grown.colour = full ? Theme::green() : Theme::orange();
    }
    out.append(grown);

    Finding uptime;
    uptime.name = tr("Uptime");
    const double seconds = readFirstLine("/proc/uptime").section(' ', 0, 0).toDouble();
    const int mins = (int)(seconds / 60);
    uptime.detail = tr("%1h %2m since boot").arg(mins / 60).arg(mins % 60);
    uptime.badge = tr("info");
    uptime.colour = Theme::ink3();
    out.append(uptime);
}

void DiagnosticsPage::probe()
{
    m_sections.clear();

    QVector<Finding> display;
    probeDisplay(display);
    m_sections.append(qMakePair(tr("Display"), display));

    QVector<Finding> gpu;
    probeGpu(gpu);
    m_sections.append(qMakePair(tr("Graphics"), gpu));

    QVector<Finding> input;
    probeInput(input);
    m_sections.append(qMakePair(tr("Input"), input));

    QVector<Finding> audio;
    probeAudio(audio);
    m_sections.append(qMakePair(tr("Audio"), audio));

    QVector<Finding> usb;
    probeUsb(usb);
    m_sections.append(qMakePair(QString("USB"), usb));

    QVector<Finding> power;
    probePower(power);
    m_sections.append(qMakePair(tr("Power"), power));

    QVector<Finding> system;
    probeSystem(system);
    m_sections.append(qMakePair(tr("System"), system));
}

void DiagnosticsPage::rebuild()
{
    QVector<ListRow> rows;

    ListRow tools;
    tools.kind = ListRow::Header;
    tools.text = tr("Tests");
    rows.append(tools);

    ListRow cube;
    cube.kind = ListRow::Action;
    cube.text = tr("Render test");
    cube.detail = tr("The cube, rasterised by the CPU into this framebuffer.\n"
                     "Reports what this board can actually draw.");
    cube.glyph = GlyphDisplay;
    cube.accent = Theme::purple();
    cube.id = RowCube;
    rows.append(cube);

    const QString egl = firstExisting(QStringList() << "/run/j36/eglprobe"
                                                    << "/opt/mixos/bin/eglprobe");
    if (!egl.isEmpty()) {
        ListRow probeRow;
        probeRow.kind = ListRow::Action;
        probeRow.text = tr("GPU render test");
        probeRow.detail = tr("The same cube, drawn by lima through GLES2.\n"
                             "Copied into this framebuffer, so the panel is safe.");
        probeRow.glyph = GlyphChip;
        probeRow.accent = Theme::orange();
        probeRow.id = RowEglProbe;
        rows.append(probeRow);
    }

    ListRow rescan;
    rescan.kind = ListRow::Action;
    rescan.text = tr("Re-scan input devices");
    rescan.detail = tr("Hotplug is automatic now; this re-reads the ones already open.");
    rescan.glyph = GlyphMouse;
    rescan.accent = Theme::teal();
    rescan.id = RowRescanInput;
    rows.append(rescan);

    ListRow refresh;
    refresh.kind = ListRow::Action;
    refresh.text = tr("Probe again");
    refresh.glyph = GlyphInfo;
    refresh.accent = Theme::blue();
    refresh.id = RowRefresh;
    rows.append(refresh);

    for (int s = 0; s < m_sections.size(); ++s) {
        ListRow header;
        header.kind = ListRow::Header;
        header.text = m_sections[s].first;
        rows.append(header);

        const QVector<Finding> &findings = m_sections[s].second;
        for (int i = 0; i < findings.size(); ++i) {
            ListRow r;
            r.kind = ListRow::Item;
            r.text = findings[i].name;
            r.detail = findings[i].detail;
            r.badge = findings[i].badge;
            r.badgeColour = findings[i].colour;
            r.accent = findings[i].colour;
            r.id = RowReading;
            /* Readings are not pressable, but they ARE landable: the detail runs to
             * three lines and the pane only shows them for the selected row. */
            rows.append(r);
        }
    }

    const int keep = m_list->current();
    m_list->setRows(rows);
    if (keep > 0 && keep < rows.size())
        m_list->setCurrent(keep);
    update();
}

/* ── actions ─────────────────────────────────────────────────────────────── */

void DiagnosticsPage::onActivated(int index)
{
    const QVector<ListRow> &rows = m_list->rows();
    if (index < 0 || index >= rows.size())
        return;

    switch (rows[index].id) {
    case RowCube:
        m_cube = true;
        m_list->setVisible(false);
        m_angle = 0.0;
        m_fps = 0.0;
        m_worstMs = 0.0;
        m_framesInWindow = 0;
        m_frameClock.start();
        m_fpsWindow.start();
        m_timer->start();
        emit titleChanged();
        update();
        return;
    case RowEglProbe:
        /*
         * -o, and never -c.  Both draw the same cube with the same shaders on the
         * same GPU; the difference is where the result lands.  -c hands its buffer
         * to a CRTC, which used to cost a black screen every time this row was
         * pressed: a DRM client that had set a mode and then exited left the panel
         * dark for the rest of the boot, because the kernel drops the client's
         * framebuffers on close, dropping the framebuffer a CRTC scans out disables
         * that CRTC, and this kernel is CONFIG_DRM_FBDEV_EMULATION=n so nothing
         * turned it back on.  The confirmation prompt that used to be here only made
         * the black screen deliberate.  preserve_lk_state has since fixed that at
         * the source -- the CRTC disable now restores the LK's overlay instead of
         * tearing the pipe down -- so -c would come back cleanly today.
         *
         * It is still -o, because giving the panel back is not the same as never
         * taking it: -c sets a mode, so the dashboard is off the screen for the
         * twenty seconds it runs.  -o renders into an FBO on the render node and
         * copies the result into /dev/fb0 with the CPU.  No modesetting node is
         * opened at all, so there is nothing to take away and nothing to hand back;
         * when it exits the dashboard repaints over it.  It proves the whole GL path
         * -- lima, Mesa, both shaders, 36 vertices -- which is all this row is for.
         */
        emit launchRequested(QStringLiteral("eglprobe"),
                             firstExisting(QStringList() << "/run/j36/eglprobe"
                                                         << "/opt/mixos/bin/eglprobe"),
                             QStringList() << "-o" << "20", false);
        return;
    case RowRescanInput:
        if (m_pad)
            m_pad->rescan();
        probe();
        rebuild();
        emit toastRequested(m_pad ? tr("%1 input devices").arg(m_pad->deviceCount())
                                  : tr("no pad"), 2500);
        return;
    case RowRefresh:
        probe();
        rebuild();
        return;
    default:
        return;
    }
}

bool DiagnosticsPage::handleNav(int action)
{
    if (m_cube) {
        if (action == Joypad::NavBack || action == Joypad::NavOk) {
            m_timer->stop();
            m_cube = false;
            m_list->setVisible(true);
            emit titleChanged();
            update();
        }
        return true;
    }

    switch (action) {
    case Joypad::NavUp:   m_list->step(-1); return true;
    case Joypad::NavDown: m_list->step(1); return true;
    case Joypad::NavOk:   m_list->press(); return true;
    case Joypad::NavMenu: probe(); rebuild(); return true;
    default:              return false;
    }
}

/* ── the cube ────────────────────────────────────────────────────────────── */

void DiagnosticsPage::spin()
{
    const qint64 ms = m_frameClock.restart();
    m_angle += (ms / 1000.0) * 0.9;   /* radians per second */

    ++m_framesInWindow;
    m_worstMs = qMax(m_worstMs, (double)ms);
    if (m_fpsWindow.elapsed() >= 1000) {
        m_fps = m_framesInWindow * 1000.0 / m_fpsWindow.elapsed();
        m_framesInWindow = 0;
        m_worstMs = 0.0;
        m_fpsWindow.restart();
    }
    update();
}

void DiagnosticsPage::paintCube(QPainter &p)
{
    const QRectF area = rect();
    p.fillRect(area, QColor(10, 11, 18));

    static const double kVerts[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 }
    };
    /* Wound counter-clockwise seen from outside, which is what makes the culling
     * test below a sign test and nothing more. */
    static const int kFaces[6][4] = {
        { 0, 3, 2, 1 },   /* back   */
        { 4, 5, 6, 7 },   /* front  */
        { 0, 1, 5, 4 },   /* bottom */
        { 2, 3, 7, 6 },   /* top    */
        { 0, 4, 7, 3 },   /* left   */
        { 1, 2, 6, 5 }    /* right  */
    };
    static const QColor kColours[6] = {
        QColor(10, 132, 255), QColor(255, 99, 132), QColor(40, 200, 64),
        QColor(254, 188, 46), QColor(148, 112, 219), QColor(48, 176, 199)
    };

    const double cx = area.center().x();
    const double cy = area.center().y();
    const double scale = qMin(area.width(), area.height()) * 0.32;

    const double sa = sin(m_angle);
    const double ca = cos(m_angle);
    const double sb = sin(m_angle * 0.61);
    const double cb = cos(m_angle * 0.61);

    QPointF projected[8];
    double depth[8];
    for (int i = 0; i < 8; ++i) {
        /* Rotate about Y, then about X. */
        const double x0 = kVerts[i][0] * ca + kVerts[i][2] * sa;
        const double z0 = -kVerts[i][0] * sa + kVerts[i][2] * ca;
        const double y1 = kVerts[i][1] * cb - z0 * sb;
        const double z1 = kVerts[i][1] * sb + z0 * cb;

        const double dist = 4.0;
        const double w = 1.0 / (dist + z1);
        projected[i] = QPointF(cx + x0 * scale * dist * w, cy + y1 * scale * dist * w);
        depth[i] = z1;
    }

    /* Painter's algorithm over six quads: sort by mean depth, draw far to near.
     * With a convex solid that is exact, and it costs one sort of six items. */
    int order[6] = { 0, 1, 2, 3, 4, 5 };
    double faceDepth[6];
    for (int f = 0; f < 6; ++f) {
        faceDepth[f] = 0.0;
        for (int v = 0; v < 4; ++v)
            faceDepth[f] += depth[kFaces[f][v]];
        faceDepth[f] /= 4.0;
    }
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j)
            if (faceDepth[order[j]] > faceDepth[order[i]]) {
                const int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }

    p.setRenderHint(QPainter::Antialiasing, true);
    for (int k = 0; k < 6; ++k) {
        const int f = order[k];
        QPolygonF poly;
        for (int v = 0; v < 4; ++v)
            poly << projected[kFaces[f][v]];

        /* Back-face cull by the sign of the projected area: a face turned away
         * winds the other way once it is on the screen. */
        double area2 = 0.0;
        for (int v = 0; v < 4; ++v) {
            const QPointF &a = poly[v];
            const QPointF &b = poly[(v + 1) % 4];
            area2 += a.x() * b.y() - b.x() * a.y();
        }
        if (area2 <= 0.0)
            continue;

        /* Shade by depth, so the solid reads as a solid without a light model. */
        const double shade = qBound(0.35, 0.5 + faceDepth[f] * -0.28, 1.0);
        QColor c = kColours[f];
        c = QColor((int)(c.red() * shade), (int)(c.green() * shade), (int)(c.blue() * shade));
        p.setBrush(c);
        p.setPen(QPen(c.lighter(140), 1.0));
        p.drawPolygon(poly);
    }

    /* The numbers, which are the actual output of this test. */
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRect foot(0, height() - 44, width(), 44);
    p.fillRect(foot, QColor(8, 9, 14, 210));
    p.setFont(Theme::font(12, true));
    p.setPen(Theme::ink());
    p.drawText(foot.adjusted(12, 4, -12, -22), Qt::AlignLeft | Qt::AlignVCenter,
               tr("%1 fps at %2x%3, CPU rasterised")
                   .arg(m_fps, 0, 'f', 1).arg(width()).arg(height()));
    p.setFont(Theme::font(11));
    p.setPen(Theme::ink3());
    p.drawText(foot.adjusted(12, 22, -12, -4), Qt::AlignLeft | Qt::AlignVCenter,
               tr("No GPU involved -- lima has no CRTC to scan out from.  B returns."));
}

void DiagnosticsPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    if (m_cube) {
        paintCube(p);
        return;
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF card(Theme::Margin, Theme::Margin,
                      width() - 2.0 * Theme::Margin, height() - 2.0 * Theme::Margin);
    paintSheet(p, card, tr("Diagnostics"), tr("A opens, Menu re-probes"));
}
