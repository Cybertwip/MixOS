/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * The header says what this is for and why it is worth having.  This file is the
 * plumbing, and it is deliberately the same plumbing as tools/j36-eglprobe.c's
 * -z mode, function call for function call: that mode exists so this one could be
 * written against something already proved to work on this board.  Where the two
 * differ, eglprobe is the reference and this is the copy.
 *
 * EGL, GBM, GLES2 AND /dev/j36fb ARE ALL DECLARED BY HAND below, exactly as they
 * are at the top of j36-eglprobe.c.  Not for elegance: mixdash builds in the
 * armhf chroot against Qt and libc, and pulling in Mesa's headers would mean
 * putting Mesa's development packages in the chroot, and linking would put
 * libEGL.so.1 in DT_NEEDED, and then a card with no GL payload staged has a
 * dashboard that will not start.  The numbers below are from Khronos' registry
 * and from the kernel's uapi, and device/j36-ultra/linux/j36_fbmem.c is the
 * authority for the last group.
 */
#include "glvideo.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── /dev/j36fb, from linux/j36_fbmem.c ──────────────────────────────────── */

#define J36FB_IOC(dir, nr, sz) (((unsigned)(dir) << 30) | ('j' << 8) | \
                                (unsigned)(nr) | ((unsigned)(sz) << 16))

struct j36fb_info {
    uint64_t phys;
    uint64_t size;
    uint32_t width, height, stride, bpp, fourcc, reserved;
};

struct j36fb_export {
    uint32_t flags;
    int32_t  fd;
};

#define J36FB_IOC_INFO   J36FB_IOC(2u, 1u, sizeof(struct j36fb_info))
#define J36FB_IOC_EXPORT J36FB_IOC(3u, 2u, sizeof(struct j36fb_export))

/* ── EGL ─────────────────────────────────────────────────────────────────── */

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLClientBuffer;
typedef void *EGLImageKHR;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;
typedef int EGLint;

#define EGL_FALSE                     0
#define EGL_NO_CONTEXT                ((EGLContext)0)
#define EGL_NO_IMAGE_KHR              ((EGLImageKHR)0)
#define EGL_NONE                      0x3038
#define EGL_BLUE_SIZE                 0x3022
#define EGL_GREEN_SIZE                0x3023
#define EGL_RED_SIZE                  0x3024
#define EGL_RENDERABLE_TYPE           0x3040
#define EGL_OPENGL_ES2_BIT            0x0004
#define EGL_OPENGL_ES_API             0x30A0
#define EGL_CONTEXT_CLIENT_VERSION    0x3098
#define EGL_EXTENSIONS                0x3055
#define EGL_HEIGHT                    0x3056
#define EGL_WIDTH                     0x3057
#define EGL_PLATFORM_GBM_KHR          0x31D7
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_LINUX_DMA_BUF_EXT         0x3270
#define EGL_LINUX_DRM_FOURCC_EXT      0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT     0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT  0x3274

/* ── GLES2 ───────────────────────────────────────────────────────────────── */

#define GL_NO_ERROR                0x0000
#define GL_TRIANGLE_STRIP          0x0005
#define GL_SRC_ALPHA               0x0302
#define GL_ONE_MINUS_SRC_ALPHA     0x0303
#define GL_CULL_FACE               0x0B44
#define GL_DEPTH_TEST              0x0B71
#define GL_BLEND                   0x0BE2
#define GL_SCISSOR_TEST            0x0C11
#define GL_UNPACK_ALIGNMENT        0x0CF5
#define GL_TEXTURE_2D              0x0DE1
#define GL_UNSIGNED_BYTE           0x1401
#define GL_FLOAT                   0x1406
#define GL_RENDERER                0x1F01
#define GL_RGBA                    0x1908
#define GL_LUMINANCE               0x1909
#define GL_NEAREST                 0x2600
#define GL_LINEAR                  0x2601
#define GL_TEXTURE_MAG_FILTER      0x2800
#define GL_TEXTURE_MIN_FILTER      0x2801
#define GL_TEXTURE_WRAP_S          0x2802
#define GL_TEXTURE_WRAP_T          0x2803
#define GL_COLOR_BUFFER_BIT        0x4000
#define GL_TEXTURE0                0x84C0
#define GL_TEXTURE1                0x84C1
#define GL_TEXTURE2                0x84C2
#define GL_CLAMP_TO_EDGE           0x812F
#define GL_FRAGMENT_SHADER         0x8B30
#define GL_VERTEX_SHADER           0x8B31
#define GL_COMPILE_STATUS          0x8B81
#define GL_LINK_STATUS             0x8B82
#define GL_FRAMEBUFFER_COMPLETE    0x8CD5
#define GL_COLOR_ATTACHMENT0       0x8CE0
#define GL_FRAMEBUFFER             0x8D40
#define GL_RENDERBUFFER            0x8D41

namespace {

/*
 * WHY THE SHADER IS THE POINT OF ALL OF THIS.  Every one of these lines is work
 * that swscale was doing per pixel on a Cortex-A7: three multiply-accumulates
 * and three clamps, 307200 times a frame, twenty-five times a second.  A
 * Mali-450 MP4 does it in the fragment stage for nothing, because the fragment
 * stage is running anyway to put the texel on the screen.
 *
 * BT.601 limited range, which is what every SD-sized h264 file on a handheld
 * actually is, and what ffmpeg's own `format=bgra' would have assumed.  The
 * constants are the standard ones: luma is scaled from [16,235] to [0,1] and
 * chroma from [16,240] to [-0.5,0.5], and the 3x3 is Rec.601's inverse.
 *
 * mediump is deliberate.  On this GPU that is a 16-bit float, which carries ten
 * bits of mantissa -- comfortably more than the eight bits that come out the
 * other end, and half the register pressure of highp on a shader core that has
 * very little to spare.
 */
const char *const kVertexShader =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main()\n"
    "{\n"
    "    v_tex = a_tex;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char *const kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_tex;\n"
    "uniform sampler2D u_y;\n"
    "uniform sampler2D u_u;\n"
    "uniform sampler2D u_v;\n"
    "void main()\n"
    "{\n"
    "    float y = texture2D(u_y, v_tex).r;\n"
    "    float u = texture2D(u_u, v_tex).r - 0.5;\n"
    "    float v = texture2D(u_v, v_tex).r - 0.5;\n"
    "    y = 1.1643 * (y - 0.0625);\n"
    "    gl_FragColor = vec4(y + 1.5958 * v,\n"
    "                        y - 0.39173 * u - 0.81290 * v,\n"
    "                        y + 2.017 * u,\n"
    "                        1.0);\n"
    "}\n";

/*
 * The strip.  Straight texel out, blended by fixed function -- the alpha in the
 * texture is what QPainter put there, and GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA is
 * the same "over" that QPainter would have done on the CPU.
 */
const char *const kOverlayFragmentShader =
    "precision mediump float;\n"
    "varying vec2 v_tex;\n"
    "uniform sampler2D u_tex;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(u_tex, v_tex);\n"
    "}\n";

} /* namespace */

/* ── the private state ───────────────────────────────────────────────────── */

struct GlVideo::Priv {
    void *egl = nullptr;
    void *gbm = nullptr;
    void *gles = nullptr;

    /* EGL */
    EGLint (*eglGetError)() = nullptr;
    EGLDisplay (*eglGetDisplay)(void *) = nullptr;
    EGLDisplay (*eglGetPlatformDisplay)(EGLenum, void *, const intptr_t *) = nullptr;
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = nullptr;
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = nullptr;
    EGLBoolean (*eglBindAPI)(EGLenum) = nullptr;
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint,
                                  EGLint *) = nullptr;
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                   const EGLint *) = nullptr;
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                 EGLContext) = nullptr;
    const char *(*eglQueryString)(EGLDisplay, EGLint) = nullptr;
    void *(*eglGetProcAddress)(const char *) = nullptr;
    EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
                                     EGLClientBuffer, const EGLint *) = nullptr;
    EGLBoolean (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;

    /* GBM */
    void *(*gbm_create_device)(int) = nullptr;

    /* GLES2 */
    const unsigned char *(*glGetString)(unsigned) = nullptr;
    unsigned (*glGetError)() = nullptr;
    void (*glGenFramebuffers)(int, unsigned *) = nullptr;
    void (*glBindFramebuffer)(unsigned, unsigned) = nullptr;
    void (*glGenRenderbuffers)(int, unsigned *) = nullptr;
    void (*glBindRenderbuffer)(unsigned, unsigned) = nullptr;
    void (*glDeleteRenderbuffers)(int, const unsigned *) = nullptr;
    void (*glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned) = nullptr;
    void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
    unsigned (*glCheckFramebufferStatus)(unsigned) = nullptr;
    void (*glGenTextures)(int, unsigned *) = nullptr;
    void (*glBindTexture)(unsigned, unsigned) = nullptr;
    void (*glDeleteTextures)(int, const unsigned *) = nullptr;
    void (*glTexParameteri)(unsigned, unsigned, int) = nullptr;
    void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                         const void *) = nullptr;
    void (*glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                            const void *) = nullptr;
    void (*glPixelStorei)(unsigned, int) = nullptr;
    void (*glActiveTexture)(unsigned) = nullptr;
    unsigned (*glCreateShader)(unsigned) = nullptr;
    void (*glShaderSource)(unsigned, int, const char *const *, const int *) = nullptr;
    void (*glCompileShader)(unsigned) = nullptr;
    void (*glGetShaderiv)(unsigned, unsigned, int *) = nullptr;
    void (*glGetShaderInfoLog)(unsigned, int, int *, char *) = nullptr;
    void (*glDeleteShader)(unsigned) = nullptr;
    unsigned (*glCreateProgram)() = nullptr;
    void (*glAttachShader)(unsigned, unsigned) = nullptr;
    void (*glLinkProgram)(unsigned) = nullptr;
    void (*glGetProgramiv)(unsigned, unsigned, int *) = nullptr;
    void (*glGetProgramInfoLog)(unsigned, int, int *, char *) = nullptr;
    void (*glUseProgram)(unsigned) = nullptr;
    int (*glGetAttribLocation)(unsigned, const char *) = nullptr;
    int (*glGetUniformLocation)(unsigned, const char *) = nullptr;
    void (*glUniform1i)(int, int) = nullptr;
    void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int,
                                  const void *) = nullptr;
    void (*glEnableVertexAttribArray)(unsigned) = nullptr;
    void (*glDrawArrays)(unsigned, int, int) = nullptr;
    void (*glViewport)(int, int, int, int) = nullptr;
    void (*glScissor)(int, int, int, int) = nullptr;
    void (*glClearColor)(float, float, float, float) = nullptr;
    void (*glClear)(unsigned) = nullptr;
    void (*glEnable)(unsigned) = nullptr;
    void (*glDisable)(unsigned) = nullptr;
    void (*glBlendFunc)(unsigned, unsigned) = nullptr;
    void (*glFinish)() = nullptr;
    void (*glEGLImageTargetRenderbufferStorageOES)(unsigned, void *) = nullptr;
    void (*glEGLImageTargetTexture2DOES)(unsigned, void *) = nullptr;

    /* objects */
    EGLDisplay dpy = nullptr;
    EGLContext ctx = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    void *bo = nullptr;                 /* the gbm_device, opaque here */
    int nodefd = -1;
    int fbfd = -1;
    int dmafd = -1;
    unsigned fbo = 0;
    unsigned colour = 0;                /* renderbuffer, when that path took it */
    unsigned colourtex = 0;             /* texture, when it did not */
    unsigned prog = 0;
    unsigned tex[3] = { 0, 0, 0 };
    int texw[3] = { 0, 0, 0 };
    int texh[3] = { 0, 0, 0 };
    int a_pos = -1;
    int a_tex = -1;
    unsigned ovprog = 0;
    int ov_a_pos = -1;
    int ov_a_tex = -1;
    /* One entry per GlVideo::Layer.  All four share texture unit 3 and the one
     * overlay program: they are drawn one after the other, so the unit is rebound
     * between draws rather than a unit spent per layer -- GLES2 guarantees only
     * eight, and three of those are the film's planes. */
    unsigned ovtex[GlVideo::LayerCount] = { 0, 0, 0, 0 };
    int ovw[GlVideo::LayerCount] = { 0, 0, 0, 0 };
    int ovh[GlVideo::LayerCount] = { 0, 0, 0, 0 };
    QRect ovat[GlVideo::LayerCount];
    QString renderer;
    QByteArray repack;                  /* only used for a padded plane */
    QElapsedTimer clock;

    /*
     * GL entry points come from eglGetProcAddress first, because on Mesa that is
     * the one function guaranteed to return the dispatch belonging to the display
     * that has just been initialised.  libGLESv2.so.2 is opened only if it does
     * not answer, and only then, so a board where the two disagree cannot end up
     * silently calling into the wrong one.
     */
    void *sym(const char *name)
    {
        void *p = eglGetProcAddress ? eglGetProcAddress(name) : nullptr;
        if (p)
            return p;
        if (!gles)
            gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        return gles ? dlsym(gles, name) : nullptr;
    }

    /*
     * Compile a pair and link them, or return 0 with *err carrying the driver's
     * own log.  The log is the point: a shader that will not compile on a Mali-450
     * fails for reasons no amount of reading the source here will show, and
     * "the program did not link" on its own has never helped anybody.
     */
    unsigned link(const char *vsrc, const char *fsrc, const char *what, QString *err)
    {
        char log[512];
        int ok = 0;
        unsigned prog;
        const unsigned vs = glCreateShader(GL_VERTEX_SHADER);
        const unsigned fs = glCreateShader(GL_FRAGMENT_SHADER);

        if (!vs || !fs) {
            *err = QString("glCreateShader returned nothing for the %1 program")
                       .arg(QString::fromLatin1(what));
            return 0;
        }

        const char *srcs[2] = { vsrc, fsrc };
        const unsigned ids[2] = { vs, fs };
        for (int i = 0; i < 2; ++i) {
            glShaderSource(ids[i], 1, &srcs[i], NULL);
            glCompileShader(ids[i]);
            ok = 0;
            glGetShaderiv(ids[i], GL_COMPILE_STATUS, &ok);
            if (ok)
                continue;
            log[0] = '\0';
            glGetShaderInfoLog(ids[i], (int)sizeof(log), NULL, log);
            log[sizeof(log) - 1] = '\0';
            *err = QString("the %1 %2 shader did not compile: %3")
                       .arg(QString::fromLatin1(what))
                       .arg(i ? "fragment" : "vertex")
                       .arg(QString::fromLatin1(log));
            return 0;
        }

        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (glDeleteShader) {
            glDeleteShader(vs);
            glDeleteShader(fs);
        }
        if (!ok) {
            log[0] = '\0';
            glGetProgramInfoLog(prog, (int)sizeof(log), NULL, log);
            log[sizeof(log) - 1] = '\0';
            *err = QString("the %1 program did not link: %2")
                       .arg(QString::fromLatin1(what))
                       .arg(QString::fromLatin1(log));
            return 0;
        }
        return prog;
    }
};

/* ── construction ────────────────────────────────────────────────────────── */

GlVideo *GlVideo::instance()
{
    /*
     * Built on first use and never destroyed.  A function-local static is the one
     * form of this that is thread-safe without a mutex and that cannot run before
     * main(), and neither of those is idle worry here: the first caller is a
     * key press on the media page, long after Qt has a screen.
     */
    static GlVideo *self = new GlVideo();
    return self;
}

GlVideo::GlVideo()
    : m_ready(false), m_lastMs(0.0), d(new Priv)
{
    if (load() && import() && programs()) {
        m_ready = true;
        m_reason = QString("GL video on \"%1\": the LK carveout at %2x%3 imported "
                           "as a dma-buf, no modeset")
                       .arg(d->renderer.isEmpty() ? QString("unknown") : d->renderer)
                       .arg(m_size.width())
                       .arg(m_size.height());
        say();
        return;
    }

    /*
     * Nothing here is retried, so what did open is given back rather than held
     * for the life of the process.  The EGL display and context are deliberately
     * NOT torn down: eglTerminate on a display Mesa may have handed to something
     * else is a worse bargain than one leaked handle in a process that has
     * already decided it is not using GL.
     */
    if (d->dmafd >= 0) {
        close(d->dmafd);
        d->dmafd = -1;
    }
    if (d->fbfd >= 0) {
        close(d->fbfd);
        d->fbfd = -1;
    }
    if (d->nodefd >= 0) {
        close(d->nodefd);
        d->nodefd = -1;
    }
    m_size = QSize();
    say();
}

/*
 * Into the log, once, the first time anything asks for this object -- opening a
 * film or opening the Diagnostics page.  NOT at startup: building this dlopen's
 * Mesa and imports the scanout, and a dashboard that has to come up on a card
 * with a broken GL payload should not be doing either before it draws.  mixdash's
 * stderr is the MixOS log, so this is the line to ask somebody to paste when a
 * film is slow and they want to know why.
 */
void GlVideo::say() const
{
    fprintf(stderr, "mixdash: %s\n", m_reason.toUtf8().constData());
    fflush(stderr);
}

void GlVideo::fail(const QString &why)
{
    m_ready = false;
    m_reason = why;
}

/* ── dlopen ──────────────────────────────────────────────────────────────── */

bool GlVideo::load()
{
    /*
     * The same two sonames SDL's KMSDRM backend opens, and the same ones eglprobe
     * opens, so a payload that satisfies one satisfies all three.  RTLD_LOCAL
     * because nothing else in this process should be able to see these symbols by
     * accident -- Qt in particular must not find a GL here and decide it has one.
     */
    d->egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!d->egl) {
        fail(QString("no GL video: libEGL.so.1 would not load (%1) -- no GL "
                     "payload is staged, or j36.gl=0")
                 .arg(QString::fromLocal8Bit(dlerror() ? dlerror() : "?")));
        return false;
    }
    d->gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!d->gbm) {
        fail(QString("no GL video: libgbm.so.1 would not load -- EGL is staged "
                     "but the buffer manager beside it is not"));
        return false;
    }

#define EGLSYM(n) \
    *(void **)(&d->n) = dlsym(d->egl, #n)
    EGLSYM(eglGetError);
    EGLSYM(eglGetDisplay);
    EGLSYM(eglInitialize);
    EGLSYM(eglBindAPI);
    EGLSYM(eglChooseConfig);
    EGLSYM(eglCreateContext);
    EGLSYM(eglMakeCurrent);
    EGLSYM(eglQueryString);
    EGLSYM(eglGetProcAddress);
    EGLSYM(eglGetPlatformDisplay);
#undef EGLSYM
    *(void **)(&d->gbm_create_device) = dlsym(d->gbm, "gbm_create_device");

    if (!d->eglGetProcAddress || !d->eglInitialize || !d->eglChooseConfig ||
        !d->eglCreateContext || !d->eglMakeCurrent || !d->eglBindAPI ||
        !d->eglQueryString || !d->gbm_create_device) {
        fail("no GL video: the libEGL/libgbm that loaded is missing core entry "
             "points, so it is not a Mesa build this can use");
        return false;
    }

    /* Extension entry points are eglGetProcAddress' business by definition. */
    *(void **)(&d->eglGetPlatformDisplayEXT) =
        d->eglGetProcAddress("eglGetPlatformDisplayEXT");
    *(void **)(&d->eglCreateImageKHR) = d->eglGetProcAddress("eglCreateImageKHR");
    *(void **)(&d->eglDestroyImageKHR) = d->eglGetProcAddress("eglDestroyImageKHR");
    if (!d->eglCreateImageKHR) {
        fail("no GL video: this EGL has no eglCreateImageKHR, so a dma-buf "
             "cannot become anything GL will draw into");
        return false;
    }
    return true;
}

/* ── the import ──────────────────────────────────────────────────────────── */

bool GlVideo::import()
{
    struct j36fb_info info;
    struct j36fb_export req;
    EGLint cfgattr[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint iattr[16];
    EGLConfig pick[64];
    EGLint n = 0;
    const char *exts;

    /*
     * /dev/j36fb before anything else, because with no exporter there is no mode
     * at all, and saying so before a GPU is opened keeps the message about the
     * piece that is actually missing.
     */
    d->fbfd = open("/dev/j36fb", O_RDWR | O_CLOEXEC);
    if (d->fbfd < 0) {
        fail("no GL video: /dev/j36fb is not there -- j36_fbmem is not loaded, or "
             "the device tree has no j36,lk-framebuffer node, so nothing can name "
             "the memory the panel is scanning out of");
        return false;
    }

    memset(&info, 0, sizeof(info));
    if (ioctl(d->fbfd, J36FB_IOC_INFO, &info) < 0) {
        fail("no GL video: J36FB_IOC_INFO was refused -- the driver behind "
             "/dev/j36fb is not the one this was built against");
        return false;
    }
    if (!info.width || !info.height || !info.stride) {
        fail("no GL video: /dev/j36fb reports a geometry that cannot be right");
        return false;
    }
    m_size = QSize((int)info.width, (int)info.height);

    memset(&req, 0, sizeof(req));
    req.flags = O_CLOEXEC;
    if (ioctl(d->fbfd, J36FB_IOC_EXPORT, &req) < 0) {
        fail("no GL video: J36FB_IOC_EXPORT was refused, so the carveout could "
             "not be turned into a dma-buf");
        return false;
    }
    d->dmafd = req.fd;

    /*
     * The render node, not the card node.  mtk_drm is display-only and has no
     * render node at all, so renderD128 is lima's -- but the loop is here because
     * that numbering is allocation order and not a promise, and asking twice
     * costs nothing next to guessing wrong.
     *
     * No card node is opened anywhere in this file.  That is what makes this
     * mode cost the panel nothing: no DRM master, no SETCRTC, no page flip.
     */
    static const char *const nodes[] = { "/dev/dri/renderD128", "/dev/dri/renderD129" };
    for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]) && !d->dpy; ++i) {
        d->nodefd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (d->nodefd < 0)
            continue;
        d->bo = d->gbm_create_device(d->nodefd);
        if (d->bo) {
            if (d->eglGetPlatformDisplayEXT)
                d->dpy = d->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, d->bo, NULL);
            if (!d->dpy && d->eglGetPlatformDisplay)
                d->dpy = d->eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, d->bo, NULL);
            if (!d->dpy && d->eglGetDisplay)
                d->dpy = d->eglGetDisplay(d->bo);
        }
        if (!d->dpy) {
            close(d->nodefd);
            d->nodefd = -1;
            d->bo = nullptr;
        }
    }
    if (!d->dpy) {
        /* Surfaceless will render, but on whatever driver Mesa picked -- which on
         * a board with no lima is llvmpipe, and llvmpipe writing into the scanout
         * is slower than the path this replaces.  Taken, but named. */
        if (d->eglGetPlatformDisplayEXT)
            d->dpy = d->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
        if (!d->dpy && d->eglGetPlatformDisplay)
            d->dpy = d->eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    }
    if (!d->dpy || !d->eglInitialize(d->dpy, NULL, NULL)) {
        fail("no GL video: no EGL display on /dev/dri/renderD128 -- lima is not "
             "loaded, or j36.lima=0, or mfgpower refused to power the MFG domain");
        return false;
    }

    /*
     * Asked for by name, because the failure it prevents is the confusing one:
     * without this extension eglCreateImageKHR returns EGL_NO_IMAGE with
     * EGL_BAD_PARAMETER, which reads as "your attributes are wrong" and sends
     * the reader off checking fourccs and strides that were right all along.
     */
    exts = d->eglQueryString(d->dpy, EGL_EXTENSIONS);
    if (!exts || !strstr(exts, "EGL_EXT_image_dma_buf_import")) {
        fail("no GL video: this EGL does not advertise "
             "EGL_EXT_image_dma_buf_import -- Mesa builds it in for every "
             "gallium driver, so its absence means the EGL that loaded is not "
             "the payload's");
        return false;
    }

    if (!d->eglBindAPI(EGL_OPENGL_ES_API) ||
        !d->eglChooseConfig(d->dpy, cfgattr, pick, 64, &n) || n == 0) {
        fail("no GL video: no ES2-renderable config on this display");
        return false;
    }

    d->ctx = d->eglCreateContext(d->dpy, pick[0], NULL, ctxattr);
    if (!d->ctx || !d->eglMakeCurrent(d->dpy, NULL, NULL, d->ctx)) {
        fail("no GL video: no current surfaceless ES2 context");
        return false;
    }

#define GLSYM(n) *(void **)(&d->n) = d->sym(#n)
    GLSYM(glGetString);          GLSYM(glGetError);
    GLSYM(glGenFramebuffers);    GLSYM(glBindFramebuffer);
    GLSYM(glGenRenderbuffers);   GLSYM(glBindRenderbuffer);
    GLSYM(glDeleteRenderbuffers); GLSYM(glFramebufferRenderbuffer);
    GLSYM(glFramebufferTexture2D); GLSYM(glCheckFramebufferStatus);
    GLSYM(glGenTextures);        GLSYM(glBindTexture);
    GLSYM(glDeleteTextures);     GLSYM(glTexParameteri);
    GLSYM(glTexImage2D);         GLSYM(glTexSubImage2D);
    GLSYM(glPixelStorei);        GLSYM(glActiveTexture);
    GLSYM(glCreateShader);       GLSYM(glShaderSource);
    GLSYM(glCompileShader);      GLSYM(glGetShaderiv);
    GLSYM(glGetShaderInfoLog);   GLSYM(glDeleteShader);
    GLSYM(glCreateProgram);      GLSYM(glAttachShader);
    GLSYM(glLinkProgram);        GLSYM(glGetProgramiv);
    GLSYM(glGetProgramInfoLog);  GLSYM(glUseProgram);
    GLSYM(glGetAttribLocation);  GLSYM(glGetUniformLocation);
    GLSYM(glUniform1i);          GLSYM(glVertexAttribPointer);
    GLSYM(glEnableVertexAttribArray); GLSYM(glDrawArrays);
    GLSYM(glViewport);           GLSYM(glScissor);
    GLSYM(glClearColor);         GLSYM(glClear);
    GLSYM(glEnable);             GLSYM(glDisable);
    GLSYM(glBlendFunc);          GLSYM(glFinish);
    GLSYM(glEGLImageTargetRenderbufferStorageOES);
    GLSYM(glEGLImageTargetTexture2DOES);
#undef GLSYM

    if (!d->glGenFramebuffers || !d->glBindFramebuffer || !d->glTexImage2D ||
        !d->glDrawArrays || !d->glCreateProgram || !d->glGetError ||
        !d->glClear || !d->glViewport) {
        fail("no GL video: the GLES2 that loaded is missing core entry points");
        return false;
    }
    if (!d->glEGLImageTargetRenderbufferStorageOES &&
        !d->glEGLImageTargetTexture2DOES) {
        fail("no GL video: this GLES2 has neither glEGLImageTargetRenderbuffer"
             "StorageOES nor glEGLImageTargetTexture2DOES, so GL_OES_EGL_image "
             "is missing and the imported buffer cannot be attached to anything");
        return false;
    }

    if (d->glGetString) {
        const unsigned char *s = d->glGetString(GL_RENDERER);
        if (s)
            d->renderer = QString::fromLatin1((const char *)s);
    }

    /*
     * EGL_NO_CONTEXT, because a dma_buf image belongs to the display and not to
     * any one context -- passing a context here is the EGL_BAD_PARAMETER that
     * catches everybody once.  One plane at offset zero: this is a single linear
     * buffer and there is nothing else in it.  No modifier attributes, which
     * leaves the image implicitly linear, which is what the DDP is scanning and
     * the only thing it can scan.
     */
    n = 0;
    iattr[n++] = EGL_WIDTH;                     iattr[n++] = (EGLint)info.width;
    iattr[n++] = EGL_HEIGHT;                    iattr[n++] = (EGLint)info.height;
    iattr[n++] = EGL_LINUX_DRM_FOURCC_EXT;      iattr[n++] = (EGLint)info.fourcc;
    iattr[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     iattr[n++] = d->dmafd;
    iattr[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; iattr[n++] = 0;
    iattr[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  iattr[n++] = (EGLint)info.stride;
    iattr[n++] = EGL_NONE;

    d->image = d->eglCreateImageKHR(d->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                    NULL, iattr);
    if (d->image == EGL_NO_IMAGE_KHR) {
        fail("no GL video: eglCreateImageKHR over the carveout's dma-buf failed "
             "-- the import is what refused, before any GL was asked for.  A "
             "stride this driver will not take, or a fourcc it will not render "
             "to, are the two usual answers");
        return false;
    }

    d->glGenFramebuffers(1, &d->fbo);
    d->glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);

    /*
     * Renderbuffer first, texture second -- both are GL_OES_EGL_image, both end
     * up as the same colour attachment, and the renderbuffer is the one that says
     * "render target and nothing else".  A driver that implements only the
     * texture entry point still works through the second path.
     */
    if (d->glEGLImageTargetRenderbufferStorageOES && d->glGenRenderbuffers) {
        (void)d->glGetError();
        d->glGenRenderbuffers(1, &d->colour);
        d->glBindRenderbuffer(GL_RENDERBUFFER, d->colour);
        d->glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, d->image);
        if (d->glGetError() == GL_NO_ERROR) {
            d->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                         GL_RENDERBUFFER, d->colour);
        } else {
            d->glDeleteRenderbuffers(1, &d->colour);
            d->colour = 0;
        }
    }
    if (!d->colour && d->glEGLImageTargetTexture2DOES && d->glGenTextures &&
        d->glFramebufferTexture2D) {
        (void)d->glGetError();
        d->glGenTextures(1, &d->colourtex);
        d->glBindTexture(GL_TEXTURE_2D, d->colourtex);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        d->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, d->image);
        if (d->glGetError() == GL_NO_ERROR) {
            d->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D, d->colourtex, 0);
        } else {
            d->glDeleteTextures(1, &d->colourtex);
            d->colourtex = 0;
        }
    }
    if (!d->colour && !d->colourtex) {
        fail("no GL video: the image imported but neither a renderbuffer nor a "
             "texture would take it, so this driver will not render into an "
             "imported linear buffer");
        return false;
    }

    /* No depth buffer is attached and none is wanted: this draws one flat quad
     * with no overlap, and a 16-bit depth buffer at 640x480 is 600 KB of tile
     * traffic for nothing. */
    if (d->glCheckFramebufferStatus &&
        d->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fail("no GL video: the FBO over the scanout did not come out complete");
        return false;
    }
    return true;
}

/* ── the programs ────────────────────────────────────────────────────────── */

bool GlVideo::programs()
{
    QString err;

    d->prog = d->link(kVertexShader, kFragmentShader, "YUV", &err);
    if (!d->prog) {
        fail("no GL video: " + err);
        return false;
    }
    d->ovprog = d->link(kVertexShader, kOverlayFragmentShader, "overlay", &err);
    if (!d->ovprog) {
        fail("no GL video: " + err);
        return false;
    }

    d->a_pos = d->glGetAttribLocation(d->prog, "a_pos");
    d->a_tex = d->glGetAttribLocation(d->prog, "a_tex");
    d->ov_a_pos = d->glGetAttribLocation(d->ovprog, "a_pos");
    d->ov_a_tex = d->glGetAttribLocation(d->ovprog, "a_tex");
    if (d->a_pos < 0 || d->a_tex < 0 || d->ov_a_pos < 0 || d->ov_a_tex < 0) {
        fail("no GL video: a linked program has no a_pos/a_tex");
        return false;
    }

    /* The samplers are bound to their units once and never again -- a sampler
     * uniform is program state, not per-frame state, and setting it every frame
     * is a driver round trip for a value that never changes. */
    d->glUseProgram(d->prog);
    const char *names[3] = { "u_y", "u_u", "u_v" };
    for (int i = 0; i < 3; ++i) {
        const int u = d->glGetUniformLocation(d->prog, names[i]);
        if (u < 0) {
            fail(QString("no GL video: the YUV program has no %1")
                     .arg(QString::fromLatin1(names[i])));
            return false;
        }
        d->glUniform1i(u, i);
    }
    d->glUseProgram(d->ovprog);
    const int uov = d->glGetUniformLocation(d->ovprog, "u_tex");
    if (uov < 0) {
        fail("no GL video: the overlay program has no u_tex");
        return false;
    }
    d->glUniform1i(uov, 3);

    d->glGenTextures(3, d->tex);
    d->glGenTextures(LayerCount, d->ovtex);
    /* The three planes on units 0..2, then every overlay layer in turn on unit 3:
     * the parameters are per-texture and not per-unit, so each one has to be bound
     * once to receive them even though they will share the unit from here on. */
    for (int i = 0; i < 3 + LayerCount; ++i) {
        d->glActiveTexture(GL_TEXTURE0 + (unsigned)(i < 3 ? i : 3));
        d->glBindTexture(GL_TEXTURE_2D, i < 3 ? d->tex[i] : d->ovtex[i - 3]);
        /* GL_LINEAR throughout: the luma is drawn about 1:1 so it barely matters
         * there, but the chroma planes are half size and are being stretched over
         * the luma, and nearest on those is what makes cheap players show blocky
         * colour edges.  The strip is drawn 1:1 and does not care either way. */
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        d->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    /*
     * Rows are uploaded byte-packed.  The default is 4, and a 4-byte alignment on
     * a GL_LUMINANCE plane whose width is not a multiple of four makes the driver
     * read past the end of every row -- which is a shear down the picture and, on
     * the last row, a read off the end of the frame.
     */
    d->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* Blending is set up once and enabled only around the strip: the video quad
     * is opaque and covers what it covers, and blending it would be a read of the
     * destination per pixel for nothing. */
    if (d->glBlendFunc)
        d->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    d->glDisable(GL_DEPTH_TEST);
    d->glDisable(GL_CULL_FACE);
    d->glDisable(GL_BLEND);
    d->glViewport(0, 0, m_size.width(), m_size.height());
    return true;
}

/* ── drawing ─────────────────────────────────────────────────────────────── */

bool GlVideo::bind()
{
    if (!m_ready)
        return false;
    /*
     * Made current every time rather than once at startup.  Nothing else in this
     * process holds an EGL context today, so this is almost always a no-op inside
     * Mesa -- but "almost always" is the wrong basis for a call that silently
     * draws into whatever the last context bound if it is wrong.
     */
    if (!d->eglMakeCurrent(d->dpy, NULL, NULL, d->ctx))
        return false;
    d->glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    return true;
}

/*
 * Panel rectangle in, clip coordinates out -- and the sign of y here was the
 * whole of "the film comes out upside down".
 *
 * What used to be here mirrored every y, and the reasoning behind it was right
 * about the panel and wrong about the framebuffer.  It IS true that the
 * scanout's row 0 is the top of the glass, and true that GL calls its own
 * origin the bottom left.  Those two facts do not compose into a flip, because
 * this is a USER FBO and not a window-system surface.  The winsys surface is
 * where that flip lives: its first row in memory is the top of the screen, GL
 * wants y to grow upward, so Mesa turns the buffer over on the way through.  An
 * FBO gets no such treatment.  Its window coordinate (0,0) is the first pixel
 * in memory, which here is the first pixel of the carveout, which is the top
 * left of the panel.
 *
 * So in this context GL's y grows DOWN the glass, panel row and clip y run the
 * same way, and mapping one to the other needs no mirror at all -- just the
 * ordinary [0,h] -> [-1,+1].  Mirroring anyway is what stood the picture on its
 * head.
 *
 * eglprobe -z never caught it because a spinning cube is plausible either way
 * up.  The first thing ever drawn through here that had a top and a bottom to
 * it was a film.
 */
static inline void quad(float *v, const QRect &r, const QSize &fb)
{
    const float w = (float)fb.width(), h = (float)fb.height();
    const float x0 = 2.0f * (float)r.left() / w - 1.0f;
    const float x1 = 2.0f * (float)(r.left() + r.width()) / w - 1.0f;
    const float y0 = 2.0f * (float)r.top() / h - 1.0f;            /* top edge */
    const float y1 = 2.0f * (float)(r.top() + r.height()) / h - 1.0f;

    /* A strip: top-left, top-right, bottom-left, bottom-right.  Texture t runs
     * with the image, 0 at its top row, and y0 is the top edge -- so they pair,
     * and they pair exactly as they did before.  What changed is which end of
     * the panel y0 lands on, not which end of the image t=0 is.
     *
     * The strip now winds the other way round, which costs nothing: culling is
     * disabled once in programs() and these are the only triangles drawn. */
    v[0]  = x0; v[1]  = y0; v[2]  = 0.0f; v[3]  = 0.0f;
    v[4]  = x1; v[5]  = y0; v[6]  = 1.0f; v[7]  = 0.0f;
    v[8]  = x0; v[9]  = y1; v[10] = 0.0f; v[11] = 1.0f;
    v[12] = x1; v[13] = y1; v[14] = 1.0f; v[15] = 1.0f;
}

void GlVideo::upload(int unit, const unsigned char *src, int stride, int w, int h)
{
    d->glActiveTexture(GL_TEXTURE0 + (unsigned)unit);
    d->glBindTexture(GL_TEXTURE_2D, d->tex[unit]);

    /*
     * GLES2 has no GL_UNPACK_ROW_LENGTH -- that is GLES3, or the optional
     * EXT_unpack_subimage -- so a plane with padding between its rows has to be
     * packed down before it can go up.  rawvideo out of ffmpeg is always tight,
     * so this branch is not taken on this board; it is here because a plane
     * pointer that turns out to be strided is otherwise a picture with a
     * diagonal shear in it and no error anywhere.
     */
    if (stride != w) {
        d->repack.resize(w * h);
        char *dst = d->repack.data();
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * w, src + (size_t)y * stride, (size_t)w);
        src = (const unsigned char *)d->repack.constData();
    }

    if (d->texw[unit] != w || d->texh[unit] != h) {
        d->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, src);
        d->texw[unit] = w;
        d->texh[unit] = h;
    } else {
        /* The size is the same as last frame's almost always, and respecifying
         * costs a fresh BO and a fresh mapping every time.  Sub-image reuses
         * both. */
        d->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                           GL_LUMINANCE, GL_UNSIGNED_BYTE, src);
    }
}

bool GlVideo::drawFrame(const unsigned char *y, int ystride,
                        const unsigned char *u, int ustride,
                        const unsigned char *v, int vstride,
                        int w, int h, const QRect &into)
{
    float verts[16];

    if (!y || !u || !v || w <= 0 || h <= 0 || into.isEmpty())
        return false;
    if (!bind())
        return false;

    d->clock.start();

    /* Letterbox inside `into', keeping the frame's aspect.  ffmpeg is padding to
     * the exact rectangle today, so this comes out as a no-op -- but it is what
     * lets the pad filter be dropped later without this having to change. */
    QSize shown(w, h);
    shown.scale(into.size(), Qt::KeepAspectRatio);
    const QRect at(into.x() + (into.width() - shown.width()) / 2,
                   into.y() + (into.height() - shown.height()) / 2,
                   shown.width(), shown.height());

    /*
     * Only when the frame does not fill its rectangle, and scissored so that the
     * clear cannot reach the transport strip Qt is painting underneath.  Two
     * things draw on this panel and neither may write in the other's rectangle.
     */
    if (at != into) {
        d->glEnable(GL_SCISSOR_TEST);
        /* Panel row straight through, for the reason quad() gives at length: an
         * FBO's window origin is the first pixel in memory and that is the top
         * left of the panel.  Subtracting from the height, which is what this
         * used to do, cleared a band mirrored about the panel's middle -- so the
         * letterbox bars were painted somewhere other than where the film was. */
        d->glScissor(into.left(), into.top(), into.width(), into.height());
        d->glClearColor(8.0f / 255.0f, 9.0f / 255.0f, 14.0f / 255.0f, 1.0f);
        d->glClear(GL_COLOR_BUFFER_BIT);
        d->glDisable(GL_SCISSOR_TEST);
    }

    upload(0, y, ystride, w, h);
    upload(1, u, ustride, (w + 1) / 2, (h + 1) / 2);
    upload(2, v, vstride, (w + 1) / 2, (h + 1) / 2);

    quad(verts, at, m_size);
    d->glUseProgram(d->prog);
    d->glVertexAttribPointer((unsigned)d->a_pos, 2, GL_FLOAT, 0,
                             4 * (int)sizeof(float), verts);
    d->glVertexAttribPointer((unsigned)d->a_tex, 2, GL_FLOAT, 0,
                             4 * (int)sizeof(float), verts + 2);
    d->glEnableVertexAttribArray((unsigned)d->a_pos);
    d->glEnableVertexAttribArray((unsigned)d->a_tex);
    d->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /*
     * The strip goes on in the same pass, over the frame that has just been
     * drawn.  It has to be every frame and not only when it changes: the video
     * quad has already covered the pixels it was sitting on.  Re-blending a
     * 640x56 texture is a few thousand fragments, which is nothing next to
     * re-rendering it with QPainter -- that is what setOverlay() is rationed for.
     */
    bool blending = false;
    for (int L = 0; L < LayerCount; ++L) {
        if (d->ovw[L] <= 0 || d->ovh[L] <= 0 || d->ovat[L].isEmpty())
            continue;
        if (!blending) {
            /* The program, the attribute arrays and GL_BLEND are the same for
             * every layer, so they are set once for however many there are. */
            d->glUseProgram(d->ovprog);
            d->glActiveTexture(GL_TEXTURE0 + 3u);
            d->glEnableVertexAttribArray((unsigned)d->ov_a_pos);
            d->glEnableVertexAttribArray((unsigned)d->ov_a_tex);
            d->glEnable(GL_BLEND);
            blending = true;
        }
        quad(verts, d->ovat[L], m_size);
        d->glBindTexture(GL_TEXTURE_2D, d->ovtex[L]);
        /* Re-pointed per layer because `verts' is a stack array that quad() has
         * just rewritten -- these are client-side pointers, read at draw time. */
        d->glVertexAttribPointer((unsigned)d->ov_a_pos, 2, GL_FLOAT, 0,
                                 4 * (int)sizeof(float), verts);
        d->glVertexAttribPointer((unsigned)d->ov_a_tex, 2, GL_FLOAT, 0,
                                 4 * (int)sizeof(float), verts + 2);
        d->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (blending)
        d->glDisable(GL_BLEND);

    finish();
    m_lastMs = (double)d->clock.nsecsElapsed() / 1000000.0;

    /*
     * One check, after the frame rather than before it, and a failure retires the
     * whole path.  What can go wrong here is a lost context or a driver that has
     * stopped answering, and neither comes right on the next frame -- retrying
     * twenty-five times a second would turn one fault into a locked-up player.
     */
    if (d->glGetError() != GL_NO_ERROR) {
        fail("GL video stopped: the driver returned an error mid-frame, so the "
             "film is back on the software path");
        return false;
    }
    return true;
}

void GlVideo::setOverlay(Layer which, const QImage &argb, const QRect &at)
{
    if (!m_ready || which < 0 || which >= LayerCount)
        return;
    if (argb.isNull() || at.isEmpty()) {
        clearOverlay(which);
        return;
    }
    if (!bind())
        return;

    /*
     * RGBA8888 is the one 32-bit format whose bytes are in the order GL_RGBA
     * names, on either endianness.  Qt's own ARGB32 is BGRA in memory on a
     * little-endian machine, and GLES2 without EXT_texture_format_BGRA8888 has
     * no way to say so -- which comes out as a picture with the red and blue
     * swapped and no error anywhere.  The convert is 143 KB for the strip, paid
     * only when the strip's text changes.
     *
     * Not premultiplied, because GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA is the
     * straight "over" and expects straight alpha.  convertToFormat un-multiplies
     * on the way out of QPainter's premultiplied working format.
     */
    const QImage src = argb.format() == QImage::Format_RGBA8888
                           ? argb
                           : argb.convertToFormat(QImage::Format_RGBA8888);
    if (src.isNull())
        return;

    const int w = src.width(), h = src.height();
    const uchar *bits = src.constBits();

    d->glActiveTexture(GL_TEXTURE0 + 3u);
    d->glBindTexture(GL_TEXTURE_2D, d->ovtex[which]);

    /* Same GLES2 limitation as the planes: no GL_UNPACK_ROW_LENGTH, so a padded
     * QImage has to be packed down.  At four bytes a pixel Qt's scanlines are
     * already 4-aligned, so this is never taken -- but a caller that hands over
     * a sub-image would otherwise get a sheared strip. */
    if (src.bytesPerLine() != w * 4) {
        d->repack.resize(w * h * 4);
        char *dst = d->repack.data();
        for (int y = 0; y < h; ++y)
            memcpy(dst + (size_t)y * w * 4, src.constScanLine(y), (size_t)w * 4);
        bits = (const uchar *)d->repack.constData();
    }

    if (d->ovw[which] != w || d->ovh[which] != h) {
        d->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, bits);
        d->ovw[which] = w;
        d->ovh[which] = h;
    } else {
        d->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                           GL_RGBA, GL_UNSIGNED_BYTE, bits);
    }
    d->ovat[which] = at;
}

void GlVideo::clearOverlay(Layer which)
{
    /* The texture is kept -- only the rectangle is dropped, which is what
     * drawFrame() tests.  Nothing here is freed because the next film wants the
     * same texture at the same size. */
    if (d && which >= 0 && which < LayerCount)
        d->ovat[which] = QRect();
}

void GlVideo::clearOverlays()
{
    for (int L = 0; L < LayerCount; ++L)
        clearOverlay((Layer)L);
}

bool GlVideo::fill(const QRect &r, unsigned int argb)
{
    if (r.isEmpty() || !bind())
        return false;

    d->glEnable(GL_SCISSOR_TEST);
    /* Same origin as quad() and as the scissor in drawFrame: the panel row goes
     * straight in.  This is the call that clears the film's rectangle on the way
     * out of a film, so while it was mirrored it left a band of the last frame
     * on the glass and wiped a band that had the dashboard in it. */
    d->glScissor(r.left(), r.top(), r.width(), r.height());
    d->glClearColor((float)((argb >> 16) & 0xff) / 255.0f,
                    (float)((argb >> 8) & 0xff) / 255.0f,
                    (float)(argb & 0xff) / 255.0f, 1.0f);
    d->glClear(GL_COLOR_BUFFER_BIT);
    d->glDisable(GL_SCISSOR_TEST);
    finish();
    return true;
}

void GlVideo::finish()
{
    /*
     * glFinish and not eglSwapBuffers, because there is no surface and nothing to
     * swap: the frame is finished when lima's writeback has landed in the
     * carveout, and from that instant the display controller is scanning it.
     * Waiting for it here is also what keeps the decoder honest -- readFrames()
     * returns to the event loop only once the picture is really on the glass, so
     * the drop counter counts frames that were skipped and not frames that are
     * still in a queue somewhere.
     */
    if (d->glFinish)
        d->glFinish();
}
