/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * panel.cpp -- see panel.h for why a stopped program's frame is kept for it.
 */
#include "panel.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

/*
 * The mapping, made once and kept.
 *
 * MMAP AND NOT read()/write(), for a reason that is about evidence rather than
 * speed: Qt's linuxfb plugin mmaps this same device and it demonstrably works on
 * this board, where the generic fb_read/fb_write path is something the mainline
 * fbmem.c provides and this kernel's simple-framebuffer has never been asked for.
 * Using the call that is already known to work here is worth more than the two
 * lines it saves.
 *
 * O_RDWR, because both halves of this file want the same mapping and opening it
 * twice would be two descriptors on one device for no gain.  A failure is
 * remembered so the ioctls are not retried on every switch: a board with no
 * framebuffer is a board that will still have none in four seconds.
 */
struct Map {
    unsigned char *base = nullptr;
    /* Bytes of the mapping that are actually on the glass: the visible window,
     * which is not the whole mapping on a panel with a pan area behind it. */
    int visible = 0;
    /* Where that window starts, from yoffset.  Zero on this board and read
     * anyway, because a plugin that panned would make every frame here land one
     * screen out and the symptom would be baffling. */
    int start = 0;
    int length = 0;
    bool tried = false;
    bool ok = false;
};

Map &mapping()
{
    static Map m;
    if (m.tried)
        return m;
    m.tried = true;

    const int fd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return m;

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    ::memset(&var, 0, sizeof(var));
    ::memset(&fix, 0, sizeof(fix));
    if (::ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0
        || ::ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        ::close(fd);
        return m;
    }

    const int stride = (int)fix.line_length;
    const int rows = (int)var.yres;
    const int total = (int)fix.smem_len;
    const int start = (int)var.yoffset * stride;
    const int visible = stride * rows;

    if (stride > 0 && rows > 0 && total > 0
        && start >= 0 && visible > 0 && start + visible <= total) {
        void *p = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            m.base = (unsigned char *)p;
            m.length = total;
            m.start = start;
            m.visible = visible;
            m.ok = true;
        }
    }

    /*
     * The descriptor goes even when the mapping stayed.  mmap keeps its own
     * reference to the file, so the mapping outlives the close, and holding a
     * spare fd open on the panel for the life of the process buys nothing.
     */
    ::close(fd);
    return m;
}

} /* namespace */

QByteArray Panel::grab()
{
    const Map &m = mapping();
    if (!m.ok)
        return QByteArray();
    return QByteArray((const char *)(m.base + m.start), m.visible);
}

bool Panel::restore(const QByteArray &frame)
{
    const Map &m = mapping();
    if (!m.ok || frame.isEmpty())
        return false;
    /*
     * A frame that is not the size of the panel is a frame from a different
     * panel, and there is no sensible thing to do with it.  It cannot happen on
     * this device -- the geometry is fixed by the device tree and read once --
     * but "cannot happen" is not a reason to memcpy an unchecked length into a
     * mapping.
     */
    if (frame.size() != m.visible)
        return false;
    ::memcpy(m.base + m.start, frame.constData(), (size_t)m.visible);
    return true;
}
