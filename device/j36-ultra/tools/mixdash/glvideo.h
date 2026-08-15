/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * glvideo -- the film, drawn by the GPU into the memory the panel is reading.
 *
 * WHAT IS SLOW ABOUT A FILM ON THIS BOARD, counted rather than guessed.  A frame
 * is 640x480, and the old path moved it five times before it was seen:
 *
 *   1  swscale converts the decoder's yuv420p into bgra          reads 460 KB, writes 1.2 MB
 *   2  ffmpeg writes those 1.2 MB down a 64 KiB pipe             1.2 MB
 *   3  MediaPage reads them back                                 1.2 MB
 *   4  QImage::copy() takes them out of the ring buffer          1.2 MB
 *   5  QPainter::drawImage into Qt's backing store               1.2 MB
 *   6  linuxfb memcpy's the dirty region into /dev/fb0           1.2 MB
 *
 * Seven and a half megabytes of memory traffic per frame, twenty-five times a
 * second, on a Cortex-A7 with LPDDR2 -- and none of it is the decode anybody
 * meant to be paying for.  This class deletes steps 1 and 4 through 6, and cuts
 * 2 and 3 to 460 KB by leaving the frame in the planar form the decoder already
 * produced.  What is left per frame is one 460 KB upload and a GPU pass.
 *
 * HOW, and every piece of it was proved by eglprobe before a line of this was
 * written -- `eglprobe -z' is the same import, the same FBO and the same absence
 * of a modeset, with a cube in place of a film:
 *
 *   - The MVII bootloader lit this panel and left the display controller
 *     scanning out of a carveout at 0x82700000.  Linux never re-programs that
 *     path.  So the carveout is LIVE SCANOUT, continuously, and anything written
 *     into it is on the glass with no flip and no modeset.
 *   - j36_fbmem exports that carveout as a dma-buf.  This imports the fd with
 *     EGL_EXT_image_dma_buf_import, hangs it off a renderbuffer and binds an FBO
 *     over it, and from then on the GPU's colour buffer IS the scanout.
 *   - The three planes go up as three GL_LUMINANCE textures and a fragment
 *     shader does the BT.601 conversion that swscale was doing on the CPU.
 *
 * NO DT_NEEDED ON MESA, WHICH IS NOT NEGOTIABLE HERE.  mixdash is the shell: it
 * has to start on a card with no GL payload staged, with j36.gl=0, with a Mali
 * blob where libEGL should be, and on a board where lima refused to bind.  So
 * libEGL and libgbm are dlopen'd and every entry point comes through
 * eglGetProcAddress or dlsym, exactly as eglprobe does it -- a missing library
 * is a sentence in the log and a fall back to the QImage path, never a loader
 * error before main().  reason() is that sentence.
 *
 * ONE BUFFER, SO IT TEARS, and that is parity rather than a regression: every
 * write to /dev/fb0 this dashboard has ever made went into the same buffer while
 * the panel was reading it.  There is no second buffer to flip to because the
 * bootloader allocated one, and taking a second would mean a modeset, which
 * would mean taking the panel away from /dev/fb0 and from everything else that
 * draws on it.
 *
 * WHY THE TRANSPORT STRIP IS DRAWN HERE TOO, which looks like scope creep and is
 * not.  Qt's linuxfb backend presents by memcpy'ing the dirty rectangle of its
 * backing store into /dev/fb0, and its backing store knows nothing about a film
 * the GPU put there.  So any Qt repaint that overlaps the picture erases it --
 * and the clock in the strip changes once a second, which would mean erasing and
 * restoring the picture once a second forever.  Splitting the screen into a Qt
 * rectangle and a GL rectangle avoids the overlap but costs the translucent bar
 * over the picture, and still leaves Qt memcpy'ing 87 KB every second for a
 * clock.  So while a film is up this owns the whole page: setOverlay() takes the
 * strip as an ARGB image, which MediaPage re-renders only when its text changes,
 * and it is blended over the picture by the same GPU pass.  MediaPage::paintEvent
 * draws nothing at all in that state.
 *
 * WHAT THIS DOES NOT DO is composite the rest of the dashboard.  Every other page
 * is still rasterised by Qt into /dev/fb0, which is the right trade for a UI that
 * repaints when something changes and the wrong one only for a surface that
 * repaints twenty-five times a second.
 */
#ifndef MIXDASH_GLVIDEO_H
#define MIXDASH_GLVIDEO_H

#include <QRect>
#include <QSize>
#include <QString>

class QImage;

class GlVideo
{
public:
    /*
     * One per process, built on first use and never destroyed: the EGL context
     * and the imported scanout are per-display state, and tearing them down
     * between films would pay the import cost for nothing.  Returns a usable
     * object either way -- ask available().
     */
    static GlVideo *instance();

    /* True when the whole chain came up: libEGL, libgbm, the render node, the
     * dma-buf import and the shaders.  False means the caller should keep doing
     * whatever it did before, and reason() says which step refused. */
    bool available() const { return m_ready; }

    /* One line, in the past tense, naming the step that failed -- or how the
     * chain came up, when it did.  Written to the log the first time anything
     * asks for this object and shown on the Diagnostics page, so it is written to
     * be read by somebody who is not holding this file open. */
    QString reason() const { return m_reason; }

    /* The scanout's own geometry, from J36FB_IOC_INFO.  Zero when !available(). */
    QSize size() const { return m_size; }

    /*
     * Draw one planar frame, letterboxed inside `into' (framebuffer
     * coordinates, origin top left), with everything else inside `into' filled
     * with `black'.  Nothing outside `into' is touched, which is what lets Qt
     * keep the transport strip.
     *
     * The three pointers are the decoder's own planes and are read, not kept.
     * `w' and `h' are the luma dimensions; the chroma planes are half each way,
     * which is what yuv420p means.  Strides are in bytes.
     *
     * Returns false if the frame could not be drawn, in which case the caller
     * should fall back for good -- a failure here is a lost context or a driver
     * that has stopped answering, not something that comes right next frame.
     */
    bool drawFrame(const unsigned char *y, int ystride,
                   const unsigned char *u, int ustride,
                   const unsigned char *v, int vstride,
                   int w, int h, const QRect &into);

    /*
     * The things that go over the picture: an ARGB image and where on the
     * framebuffer to put it.  Kept as a texture and re-blended by every
     * drawFrame() until it is replaced or cleared, so the caller should call this
     * only when the CONTENT changes -- once a second for the clock, not once a
     * frame.  Any QImage format is taken; it is converted here.
     *
     * Alpha is honoured, which is the whole reason these are textures and not
     * fills: the bar over a film is meant to be see-through.
     *
     * THERE ARE TWO OF THEM, AND THE SECOND ONE IS NOT A CONVENIENCE.  Everything
     * that has to appear over a film has to be blended by this pass, because this
     * pass is what is actually in the scanout -- a Qt widget drawn over the film
     * is a memcpy that the next frame overwrites twenty-five times a second,
     * which is not a stacking-order problem and cannot be fixed with raise().
     * The transport strip was the first such thing and the volume bar is the
     * second; they change on completely different schedules and live in
     * completely different rectangles, so folding them into one texture would
     * mean re-rendering the strip every time the volume moved and carrying a
     * texture the size of both.  Two slots, drawn in order, Volume last because a
     * volume bar over the clock is right and the clock over the volume bar is
     * not.
     */
    enum Layer { ChromeLayer, VolumeLayer, LayerCount };
    void setOverlay(Layer which, const QImage &argb, const QRect &at);
    void clearOverlay(Layer which);
    /* Both at once, for the way out of a film. */
    void clearOverlays();

    /*
     * Fill a rectangle of the scanout, in framebuffer coordinates.  Used to
     * clear the film's rectangle on the way out, so the dashboard does not come
     * back with the last frame of a film behind it.
     */
    bool fill(const QRect &r, unsigned int argb);

    /*
     * Everything drawn since the last present, made visible and finished.  It is
     * a glFinish and not a flip: there is nothing to flip.  Called by drawFrame,
     * so a caller that only uses drawFrame never needs it.
     */
    void finish();

    /* Milliseconds the GPU spent on the last drawFrame, for the transport strip
     * to show next to the dropped-frame count.  0 before the first frame. */
    double lastFrameMs() const { return m_lastMs; }

private:
    GlVideo();
    GlVideo(const GlVideo &);
    GlVideo &operator=(const GlVideo &);

    bool load();       /* dlopen libEGL/libgbm and resolve what is needed */
    bool import();     /* /dev/j36fb -> EGLImage -> renderbuffer -> FBO */
    bool programs();   /* both programs, the four textures, the state */
    bool bind();       /* make current and bind the FBO over the scanout */
    void upload(int unit, const unsigned char *src, int stride, int w, int h);
    void fail(const QString &why);
    void say() const;  /* reason(), once, into mixdash's stderr */

    bool m_ready;
    QString m_reason;
    QSize m_size;
    double m_lastMs;

    /* Opaque so this header stays free of EGL and GL declarations -- nothing
     * else in mixdash should be able to reach them by including this. */
    struct Priv;
    Priv *d;
};

#endif /* MIXDASH_GLVIDEO_H */
