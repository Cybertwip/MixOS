/* SPDX-License-Identifier: MS-PL */
/* Copyright (c) 2025-2026 the MixOS project.  Microsoft Public License; see
 * device/j36-ultra/LICENSE for the full text and for what it does not cover. */
/*
 * j36-eglprobe -- ask EGL the question EmulationStation's abort throws away.
 *
 * ES dies at es-core/src/renderers/Renderer_GLES10.cpp:129 with
 *
 *     std::string glExts = (const char*)glGetString(GL_EXTENSIONS);
 *
 * two lines after an unchecked SDL_GL_CreateContext().  So a failed context
 * reaches us as std::logic_error "basic_string: construction from null is not
 * valid" and SIGABRT -- status 134 -- with the reason discarded.  Mesa's own
 * EGL_LOG_LEVEL=debug does print the reason, but it prints it once, in the
 * middle of a scrolling 30-line console, on a 640x480 panel photographed by
 * hand.
 *
 * This asks the same questions SDL asks, in the same order, and prints four
 * lines per DRM device instead:
 *
 *   - does gbm open the device and does EGL initialise on it,
 *   - which client APIs and how many configs come back,
 *   - which of desktop GL / GLES1 / GLES2 can create a context, with the exact
 *     EGL error when one cannot,
 *   - whether a window surface on an ARGB8888 gbm surface -- the only format
 *     SDL's KMSDRM backend will scan out (SDL_kmsdrmvideo.c:1197) -- can be made
 *     current, and what glGetString then answers.
 *
 * With -s it asks the same three questions of the surfaceless platform, with no
 * DRM device and no gbm in the picture, which is worth a row because of what
 * EGL_BAD_ALLOC means.  Mesa maps __DRI_CTX_ERROR_NO_MEMORY to EGL_BAD_ALLOC and
 * that is the bucket it uses whenever the driver's own context creation returns
 * NULL -- including when _mesa_compute_version() ends up at 0 because the
 * extensions that API requires are not all there.  So a BAD_ALLOC says "the
 * driver said no", not which API it cannot do.  Run -s with
 * LIBGL_ALWAYS_SOFTWARE=1 and the driver is swrast, which does desktop GL, GLES1
 * and GLES2 on any machine: a row that fails there fails for a reason that has
 * nothing to do with this SoC, and a row that works there but not on the DRM
 * nodes is lima's.
 *
 * It probes the display node and the render node separately, because on this SoC
 * they are different chips: one is mtk_drm (display, no GPU) and one is lima (GPU,
 * no scanout), and Mesa bridges them with kmsro.  If contexts come up on the render
 * node but not on the display node, the kmsro pairing is what is broken; if they
 * fail on both, lima's context creation is.  That distinction is the whole reason
 * this file exists and it is not observable from ES's abort.
 *
 * WHICH node is which is asked, not assumed.  This file used to hard-code card0 as
 * the display, and on the kernel we ship that is lima: -p answered "GETRESOURCES:
 * Operation not supported", which is what a driver registered without
 * DRIVER_MODESET returns, and every -p run before this one therefore measured the
 * GPU's inability to scan out rather than anything about the panel.  Minor numbers
 * go by probe order, so display_node() names every node and picks the one that has
 * a CRTC and a connector.  -i prints that table and does nothing else.
 *
 * Which API is asked for matters as much as whether it works.  ES is a pure
 * GLES1 fixed-function renderer -- glMatrixMode, glVertexPointer -- but it asks
 * SDL for DESKTOP GL: setupWindow() sets SDL_GL_CONTEXT_MAJOR_VERSION twice (1,
 * then 0) and never sets SDL_GL_CONTEXT_PROFILE_MASK, and this SDL2 has no RPI
 * driver, so KMSDRM_GLES_DefaultProfileConfig is compiled out to a no-op and the
 * profile stays at SDL's desktop default.  SDL_egl.c then asks for
 * EGL_OPENGL_BIT, calls eglBindAPI(EGL_OPENGL_API), and passes no context
 * attribs at all.  So the GL row below is what ES actually requests and the ES1
 * row is what it actually needs; the two need not agree, and if they disagree
 * that is the finding.
 *
 * Everything is dlopen'd.  Linking against libEGL and libgbm would mean armhf
 * -dev packages in the image and a build that fails differently from the run;
 * dlopen means the only DT_NEEDED is libc, a missing library is a printed result
 * rather than a dead loader, and resolution goes through whatever
 * LD_LIBRARY_PATH the caller has -- for us /run/j36/gl, the same search path ES
 * gets.  So this measures the libraries ES will use, not the ones Debian
 * shipped.
 *
 * With -f it looks at the panel instead of at a driver, and this is the mode to
 * reach for first when the report is "the screen is black".  Black is what every
 * layer produces when it fails -- a disabled CRTC, a backlight at zero, a console
 * left in KD_GRAPHICS, a framebuffer nothing has drawn into -- and -f is the only
 * mode here that can tell those four apart, because it counts the pixels in
 * /dev/fb0 before touching it and then draws colour bars into it with the CPU.
 * No DRM node is opened, so unlike -p it cannot take the panel away from anything;
 * it is safe on every boot, and run just before the shell the bars are a handover
 * signal.  See fb_report().
 *
 * With -p it stops asking and paints.  Everything above measures whether a
 * context can be built, and none of it can answer the question this board is
 * actually stuck on: ES2 comes up on lima, ES runs, and the panel is black.  A
 * config table says nothing about whether a frame reaches the glass.  So -p
 * drives the whole scanout chain itself, in five phases that remove ES, then
 * SDL, then GL, then gbm from the picture -- see paint() for what each one
 * proves and for the four verdicts the sequence can return.  It holds each
 * frame for three seconds, because the instrument for this one is an eye.
 *
 * With -c it draws a rotating cube.  Every phase of -p is a clear, so a driver
 * that could do nothing but clear a buffer would pass all five; -c compiles two
 * shaders, hands over 36 vertices and page-flips the result, which is the
 * smallest thing that cannot be faked -- see cube() for what that adds.
 *
 * WHAT -p AND -c COST, because it is not obvious and it is not undoable.  A DRM
 * client that sets a mode and then exits leaves the panel black: on close the
 * kernel runs drm_fb_release() over the client's framebuffers, and removing the
 * framebuffer a CRTC is scanning out disables that CRTC.  Nothing turns it back on,
 * because this kernel is built CONFIG_DRM_FBDEV_EMULATION=n on purpose -- there is
 * no in-kernel fbdev client for drm_client_dev_restore() to hand the pipe back to.
 * So -p and -c hold the panel until the next reboot, and /dev/fb0 -- which is
 * simple-framebuffer over the LK's memory, a different path to the same glass --
 * stops being visible even though writes to it still succeed.  That is why neither
 * one runs at boot any more, why the dashboard's cube card asks twice, and why -f
 * exists: everything -p was being used to prove about the panel, -f proves without
 * spending the panel to do it.
 *
 * Usage: eglprobe [-f [seconds] | -i | -s | -p | -c [seconds] | /dev/dri/node ...].
 * With no arguments it reports /dev/fb0 read-only and then probes the display node
 * and renderD128.  -f runs alone and needs no library.  Exit status: 0 if some API
 * created a context on some display (for -p, if the mode was set; for -c, if a frame
 * was drawn; for -i, if a modesetting node exists at all; for -f, if there was a
 * framebuffer to look at), 1 otherwise.  Apart from -f's bars, an unblank and a
 * backlight that measured zero, nothing is written anywhere; stdout is the output.
 */

#define _GNU_SOURCE
/*
 * The dumb buffer's mmap offset is a DRM fake offset, and on a 32-bit kernel
 * those start at 0x10000000 and run up from there.  A signed 32-bit off_t would
 * be a silent limit on a call whose failure looks like "the panel is black", so
 * ask for the 64-bit one.
 */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/*
 * EGL and GBM by hand.  These are ABI -- fixed by the specs and by drm_fourcc.h
 * -- so declaring them costs nothing and saves a sysroot.
 */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t EGLint;

#define EGL_SUCCESS                0x3000
#define EGL_NONE                   0x3038
#define EGL_BLUE_SIZE              0x3022
#define EGL_GREEN_SIZE             0x3023
#define EGL_RED_SIZE               0x3024
#define EGL_DEPTH_SIZE             0x3025
#define EGL_NATIVE_VISUAL_ID       0x302E
#define EGL_SURFACE_TYPE           0x3033
#define EGL_RENDERABLE_TYPE        0x3040
#define EGL_VENDOR                 0x3053
#define EGL_CLIENT_APIS            0x308D
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

#define EGL_WINDOW_BIT             0x0004
#define EGL_OPENGL_ES_BIT          0x0001
#define EGL_OPENGL_ES2_BIT         0x0004
#define EGL_OPENGL_BIT             0x0008

#define EGL_OPENGL_ES_API          0x30A0
#define EGL_OPENGL_API             0x30A2

#define EGL_PLATFORM_GBM_KHR       0x31D7
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_DONT_CARE              ((EGLint)-1)

#define GL_VERSION                 0x1F02
#define GL_EXTENSIONS              0x1F03

/* drm_fourcc.h: 'AR24' and 'XR24'. */
#define FOURCC_ARGB8888            0x34325241u
#define FOURCC_XRGB8888            0x34325258u

#define GBM_BO_USE_SCANOUT         (1u << 0)
#define GBM_BO_USE_RENDERING       (1u << 2)

struct gbm_device;
struct gbm_surface;
struct gbm_bo;

/*
 * Returned by value, and it is eight bytes wide, so it has to be declared
 * honestly: AAPCS returns a composite of this size through a hidden pointer, and
 * a uint64_t stand-in would be returned in r0/r1 instead and read back rubbish.
 */
union gbm_bo_handle {
    void    *ptr;
    int32_t  s32;
    uint32_t u32;
    int64_t  s64;
    uint64_t u64;
};

static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLDisplay (*p_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLDisplay (*p_eglGetPlatformDisplay)(EGLenum, void *, const intptr_t *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*p_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*p_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                       EGLint, EGLint *);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig, void *,
                                              const EGLint *);
static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLint (*p_eglGetError)(void);
static void *(*p_eglGetProcAddress)(const char *);

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);

static struct gbm_device *(*p_gbm_create_device)(int);
static void (*p_gbm_device_destroy)(struct gbm_device *);
static struct gbm_surface *(*p_gbm_surface_create)(struct gbm_device *, uint32_t,
                                                   uint32_t, uint32_t, uint32_t);
static void (*p_gbm_surface_destroy)(struct gbm_surface *);
static struct gbm_bo *(*p_gbm_surface_lock_front_buffer)(struct gbm_surface *);
static void (*p_gbm_surface_release_buffer)(struct gbm_surface *, struct gbm_bo *);
static union gbm_bo_handle (*p_gbm_bo_get_handle)(struct gbm_bo *);
static uint32_t (*p_gbm_bo_get_stride)(struct gbm_bo *);

/*
 * GLES2 by hand as well, and for the same reason: -c needs twenty-odd entry
 * points to draw a cube, and linking them would mean libGLESv2.so.2 in DT_NEEDED
 * -- which on this card's shared rootfs is a symlink to the R36S's ARMv8-A
 * libMali.so, so the loader would refuse this binary before main().  Resolved
 * through eglGetProcAddress, they come from whichever Mesa the caller's
 * LD_LIBRARY_PATH found, which is the one being measured.
 *
 * These are the GL types on this ABI: GLsizei and GLint are int, GLenum and
 * GLuint are unsigned, GLchar is char.  Written out rather than typedef'd so that
 * each prototype below reads as the spec writes it.
 */
#define GL_NO_ERROR                0x0000
#define GL_DEPTH_BUFFER_BIT        0x00000100
#define GL_TRIANGLES               0x0004
#define GL_LESS                    0x0201
#define GL_CULL_FACE               0x0B44
#define GL_DEPTH_TEST              0x0B71
#define GL_VENDOR                  0x1F00
#define GL_RENDERER                0x1F01
#define GL_FLOAT                   0x1406
#define GL_FRAGMENT_SHADER         0x8B30
#define GL_VERTEX_SHADER           0x8B31
#define GL_COMPILE_STATUS          0x8B81
#define GL_LINK_STATUS             0x8B82
#define GL_INFO_LOG_LENGTH         0x8B84
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

static void (*p_glViewport)(int, int, int, int);
static void (*p_glEnable)(unsigned int);
static void (*p_glDepthFunc)(unsigned int);
static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClear)(unsigned int);
static void (*p_glFinish)(void);
static unsigned int (*p_glGetError)(void);
static const unsigned char *(*p_glGetString)(unsigned int);
static unsigned int (*p_glCreateShader)(unsigned int);
static void (*p_glShaderSource)(unsigned int, int, const char *const *, const int *);
static void (*p_glCompileShader)(unsigned int);
static void (*p_glGetShaderiv)(unsigned int, unsigned int, int *);
static void (*p_glGetShaderInfoLog)(unsigned int, int, int *, char *);
static void (*p_glDeleteShader)(unsigned int);
static unsigned int (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned int, unsigned int);
static void (*p_glLinkProgram)(unsigned int);
static void (*p_glGetProgramiv)(unsigned int, unsigned int, int *);
static void (*p_glGetProgramInfoLog)(unsigned int, int, int *, char *);
static void (*p_glUseProgram)(unsigned int);
static int (*p_glGetAttribLocation)(unsigned int, const char *);
static int (*p_glGetUniformLocation)(unsigned int, const char *);
static void (*p_glUniformMatrix4fv)(int, int, unsigned char, const float *);
static void (*p_glVertexAttribPointer)(unsigned int, int, unsigned int,
                                       unsigned char, int, const void *);
static void (*p_glEnableVertexAttribArray)(unsigned int);
static void (*p_glDrawArrays)(unsigned int, int, int);

static const char *eglerr(EGLint e)
{
    switch (e) {
    case 0x3000: return "SUCCESS";
    case 0x3001: return "NOT_INITIALIZED";
    case 0x3002: return "BAD_ACCESS";
    case 0x3003: return "BAD_ALLOC";
    case 0x3004: return "BAD_ATTRIBUTE";
    case 0x3005: return "BAD_CONFIG";
    case 0x3006: return "BAD_CONTEXT";
    case 0x3007: return "BAD_CURRENT_SURFACE";
    case 0x3008: return "BAD_DISPLAY";
    case 0x3009: return "BAD_MATCH";
    case 0x300A: return "BAD_NATIVE_PIXMAP";
    case 0x300B: return "BAD_NATIVE_WINDOW";
    case 0x300C: return "BAD_PARAMETER";
    case 0x300D: return "BAD_SURFACE";
    case 0x300E: return "CONTEXT_LOST";
    default:     return "UNKNOWN";
    }
}

/* Drain the queue so the next failure is attributable to the next call. */
static void eglclear(void)
{
    if (!p_eglGetError)
        return;
    while (p_eglGetError() != EGL_SUCCESS)
        ;
}

/*
 * The per-device context verdict is built up as one line, because three APIs
 * times four possible outcomes each is unreadable spread over twelve lines of a
 * hand-photographed panel.  Appends past the end are dropped rather than
 * truncating arithmetic into an underflow.
 */
static char   line[1024];
static size_t line_n;

static void appendf(const char *fmt, ...)
{
    va_list ap;
    int r;

    if (line_n + 1 >= sizeof(line))
        return;
    va_start(ap, fmt);
    r = vsnprintf(line + line_n, sizeof(line) - line_n, fmt, ap);
    va_end(ap);
    if (r > 0) {
        line_n += (size_t)r;
        if (line_n >= sizeof(line))
            line_n = sizeof(line) - 1;
    }
}

#define SYM(handle, name)                                                       \
    do {                                                                        \
        *(void **)(&p_##name) = dlsym(handle, #name);                            \
        if (!p_##name) {                                                        \
            printf("eglprobe: no %s\n", #name);                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static int load(void)
{
    /*
     * The names SDL's KMSDRM backend dlopens: SDL_VIDEO_EGL_DRIVER is
     * libEGL.so.1 and gbm comes in as libgbm.so.1.  Using the same sonames means
     * a payload that is missing one fails here the way it fails for ES.
     */
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl) {
        printf("eglprobe: libEGL.so.1: %s\n", dlerror());
        return 1;
    }
    void *gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gbm) {
        printf("eglprobe: libgbm.so.1: %s\n", dlerror());
        return 1;
    }

    SYM(egl, eglGetDisplay);
    SYM(egl, eglInitialize);
    SYM(egl, eglTerminate);
    SYM(egl, eglQueryString);
    SYM(egl, eglGetConfigs);
    SYM(egl, eglGetConfigAttrib);
    SYM(egl, eglChooseConfig);
    SYM(egl, eglBindAPI);
    SYM(egl, eglCreateContext);
    SYM(egl, eglDestroyContext);
    SYM(egl, eglCreateWindowSurface);
    SYM(egl, eglDestroySurface);
    SYM(egl, eglMakeCurrent);
    SYM(egl, eglGetError);
    SYM(egl, eglGetProcAddress);

    /* Optional: only used to reach the gbm platform the way SDL does. */
    *(void **)(&p_eglGetPlatformDisplay) = dlsym(egl, "eglGetPlatformDisplay");
    *(void **)(&p_eglGetPlatformDisplayEXT) =
        p_eglGetProcAddress("eglGetPlatformDisplayEXT");

    SYM(gbm, gbm_create_device);
    SYM(gbm, gbm_device_destroy);
    SYM(gbm, gbm_surface_create);
    SYM(gbm, gbm_surface_destroy);

    /*
     * The five -p needs, optional on purpose: the question modes above work
     * without them, so a libgbm that lacks one should cost the GL phase of -p and
     * nothing else.  paint() checks for NULL and says which name was missing.
     */
    *(void **)(&p_eglSwapBuffers) = dlsym(egl, "eglSwapBuffers");
    *(void **)(&p_gbm_surface_lock_front_buffer) =
        dlsym(gbm, "gbm_surface_lock_front_buffer");
    *(void **)(&p_gbm_surface_release_buffer) =
        dlsym(gbm, "gbm_surface_release_buffer");
    *(void **)(&p_gbm_bo_get_handle) = dlsym(gbm, "gbm_bo_get_handle");
    *(void **)(&p_gbm_bo_get_stride) = dlsym(gbm, "gbm_bo_get_stride");
    return 0;
}
#undef SYM

struct api {
    const char *name;
    EGLenum     bind;
    EGLint      renderable;
    EGLint      client_version; /* 0 => send no attribs, which is exactly what
                                 * SDL does for a desktop-GL request whose
                                 * major_version is 0. */
};

static const struct api APIS[] = {
    { "GL",  EGL_OPENGL_API,    EGL_OPENGL_BIT,     0 }, /* what ES asks for */
    { "ES1", EGL_OPENGL_ES_API, EGL_OPENGL_ES_BIT,  1 }, /* what ES needs */
    { "ES2", EGL_OPENGL_ES_API, EGL_OPENGL_ES2_BIT, 2 },
};

/*
 * Try each API on an initialised display and build up the one-line verdict.
 * gbm == NULL means there is no native window to be had, so the context is taken
 * current against EGL_NO_SURFACE instead -- what the surfaceless platform is for.
 * Returns the number of APIs that produced a context.
 */
static int try_apis(EGLDisplay dpy, struct gbm_device *gbm, int scanout)
{
    EGLint e;
    int wins = 0;
    size_t a;

    line[0] = '\0';
    line_n = 0;

    for (a = 0; a < sizeof(APIS) / sizeof(APIS[0]); a++) {
        const struct api *api = &APIS[a];
        EGLint attr[] = {
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_DEPTH_SIZE,      24,
            EGL_RENDERABLE_TYPE, api->renderable,
            /* EGL_WINDOW_BIT is eglChooseConfig's default, so a surfaceless
             * display -- whose configs are pbuffer-only -- has to be told not to
             * care or every row would come back nocfg for the wrong reason. */
            EGL_SURFACE_TYPE,    gbm ? EGL_WINDOW_BIT : EGL_DONT_CARE,
            EGL_NONE
        };
        EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, api->client_version, EGL_NONE };
        EGLConfig pick[64], cfg;
        EGLint count = 0, i;
        EGLContext ctx;
        struct gbm_surface *gs = NULL;
        EGLSurface surf = NULL;

        eglclear();
        if (!p_eglBindAPI(api->bind)) {
            appendf(" %s=nobind", api->name);
            continue;
        }
        if (!p_eglChooseConfig(dpy, attr, pick, 64, &count) || count == 0) {
            appendf(" %s=nocfg", api->name);
            continue;
        }

        /*
         * Prefer the ARGB8888 visual, which is what SDL_EGL_SetRequiredVisualId
         * pins for a KMSDRM window, and fall back to the first config the way
         * SDL does when nothing carries that visual.
         */
        cfg = pick[0];
        for (i = 0; i < count; i++) {
            EGLint vis = 0;
            p_eglGetConfigAttrib(dpy, pick[i], EGL_NATIVE_VISUAL_ID, &vis);
            if ((uint32_t)vis == FOURCC_ARGB8888) {
                cfg = pick[i];
                break;
            }
        }

        ctx = p_eglCreateContext(dpy, cfg, NULL,
                                 api->client_version ? cattr : NULL);
        if (!ctx) {
            e = p_eglGetError();
            appendf(" %s=0x%04x(%s)", api->name, e, eglerr(e));
            continue;
        }
        wins++;
        appendf(" %s=ctx", api->name);

        /*
         * A context alone is not what ES needs; it needs glGetString to answer
         * on a current one.  So take it the whole way: an ARGB8888 gbm surface
         * the size of the panel, a window surface on it, make current, ask.
         */
        if (gbm) {
            gs = p_gbm_surface_create(gbm, 640, 480, FOURCC_ARGB8888,
                                      scanout ? (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)
                                              : GBM_BO_USE_RENDERING);
            surf = gs ? p_eglCreateWindowSurface(dpy, cfg, gs, NULL) : NULL;
        }
        if (gbm && !gs) {
            appendf("/nogbm");
        } else if (gbm && !surf) {
            e = p_eglGetError();
            appendf("/srf0x%04x", e);
        } else if (!p_eglMakeCurrent(dpy, surf, surf, ctx)) {
            e = p_eglGetError();
            appendf("/cur0x%04x", e);
        } else {
            const unsigned char *(*gl_get_string)(unsigned int) =
                (const unsigned char *(*)(unsigned int))
                    p_eglGetProcAddress("glGetString");
            const unsigned char *ver = gl_get_string ? gl_get_string(GL_VERSION) : NULL;
            const unsigned char *ext = gl_get_string ? gl_get_string(GL_EXTENSIONS) : NULL;
            appendf("/cur \"%s\" ext=%s", ver ? (const char *)ver : "NULL",
                    ext ? "ok" : "NULL");
            p_eglMakeCurrent(dpy, NULL, NULL, NULL);
        }
        if (surf)
            p_eglDestroySurface(dpy, surf);
        if (gs)
            p_gbm_surface_destroy(gs);
        p_eglDestroyContext(dpy, ctx);
    }
    return wins;
}

/* Returns the number of APIs that produced a context on this device. */
static int probe(const char *path, int scanout)
{
    EGLDisplay dpy = NULL;
    EGLConfig cfgs[256];
    EGLint major = 0, minor = 0, got = 0, e;
    const char *how = "getdisplay";
    struct gbm_device *gbm;
    int fd, wins = 0;

    printf("== %s\n", path);

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open: %m\n");
        return 0;
    }

    gbm = p_gbm_create_device(fd);
    if (!gbm) {
        printf("gbm_create_device: failed\n");
        close(fd);
        return 0;
    }

    if (p_eglGetPlatformDisplayEXT) {
        dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        how = "platformEXT";
    }
    if (!dpy && p_eglGetPlatformDisplay) {
        dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
        how = "platform1.5";
    }
    if (!dpy) {
        dpy = p_eglGetDisplay(gbm);
        how = "getdisplay";
    }
    if (!dpy) {
        e = p_eglGetError();
        printf("no display: 0x%04x %s\n", e, eglerr(e));
        goto out_gbm;
    }

    if (!p_eglInitialize(dpy, &major, &minor)) {
        e = p_eglGetError();
        printf("eglInitialize(%s): 0x%04x %s\n", how, e, eglerr(e));
        goto out_gbm;
    }

    printf("egl %d.%d %s vendor=%s apis=%s\n", major, minor, how,
           p_eglQueryString(dpy, EGL_VENDOR) ?: "?",
           p_eglQueryString(dpy, EGL_CLIENT_APIS) ?: "?");

    /*
     * One line for the whole config table: how many there are, how many carry
     * each renderable bit, and how many window-capable ones are in each of the
     * two formats that matter -- ARGB8888 because SDL pins that visual for a
     * scanout surface, XRGB8888 because that is where a config with no alpha
     * lands and ES never asks for alpha.
     */
    if (p_eglGetConfigs(dpy, cfgs, 256, &got) && got > 0) {
        int gl = 0, es1 = 0, es2 = 0, win = 0, ar24 = 0, xr24 = 0, oth = 0, d24 = 0;
        EGLint i;
        for (i = 0; i < got; i++) {
            EGLint rt = 0, st = 0, vis = 0, depth = 0;
            p_eglGetConfigAttrib(dpy, cfgs[i], EGL_RENDERABLE_TYPE, &rt);
            p_eglGetConfigAttrib(dpy, cfgs[i], EGL_SURFACE_TYPE, &st);
            p_eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vis);
            p_eglGetConfigAttrib(dpy, cfgs[i], EGL_DEPTH_SIZE, &depth);
            if (rt & EGL_OPENGL_BIT)     gl++;
            if (rt & EGL_OPENGL_ES_BIT)  es1++;
            if (rt & EGL_OPENGL_ES2_BIT) es2++;
            if (depth >= 24)             d24++;
            if (st & EGL_WINDOW_BIT) {
                win++;
                if ((uint32_t)vis == FOURCC_ARGB8888)      ar24++;
                else if ((uint32_t)vis == FOURCC_XRGB8888) xr24++;
                else                                       oth++;
            }
        }
        printf("cfg=%d win=%d GL=%d ES1=%d ES2=%d z24=%d AR24=%d XR24=%d oth=%d\n",
               got, win, gl, es1, es2, d24, ar24, xr24, oth);
    } else {
        printf("eglGetConfigs: none\n");
    }

    wins = try_apis(dpy, gbm, scanout);
    printf("ctx:%s\n", line);

    p_eglTerminate(dpy);
out_gbm:
    p_gbm_device_destroy(gbm);
    close(fd);
    return wins;
}

/*
 * The same three questions with no DRM device and no gbm anywhere in the path.
 * Run by the caller with LIBGL_ALWAYS_SOFTWARE=1, so the driver underneath is
 * swrast, which does desktop GL, GLES1 and GLES2 on any machine: this row says
 * whether the Mesa in the payload can build those contexts at all, which is the
 * half of a BAD_ALLOC that the DRM nodes cannot tell us.
 */
static int probe_surfaceless(void)
{
    EGLDisplay dpy = NULL;
    EGLint major = 0, minor = 0, e;
    int wins;

    printf("== surfaceless\n");

    if (p_eglGetPlatformDisplayEXT)
        dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (!dpy && p_eglGetPlatformDisplay)
        dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (!dpy) {
        e = p_eglGetError();
        printf("no display: 0x%04x %s\n", e, eglerr(e));
        return 0;
    }
    if (!p_eglInitialize(dpy, &major, &minor)) {
        e = p_eglGetError();
        printf("eglInitialize: 0x%04x %s\n", e, eglerr(e));
        return 0;
    }

    printf("egl %d.%d surfaceless vendor=%s apis=%s\n", major, minor,
           p_eglQueryString(dpy, EGL_VENDOR) ?: "?",
           p_eglQueryString(dpy, EGL_CLIENT_APIS) ?: "?");

    wins = try_apis(dpy, NULL, 0);
    printf("ctx:%s\n", line);

    p_eglTerminate(dpy);
    return wins;
}

/* ── -f: the panel, with no DRM anywhere in the path ───────────────────────────
 *
 * Everything above asks a driver a question.  Not one of them can answer the
 * question this board is actually stuck on -- "the screen is black" -- and the
 * reason is that black is what EVERY layer produces when it fails.  It is what a
 * disabled CRTC looks like, and a backlight at zero, and a console left in
 * KD_GRAPHICS, and a framebuffer nobody has drawn into yet.  Four causes, one
 * symptom, and up to now this probe could not tell them apart because it never
 * looked at the framebuffer that is on the glass.
 *
 * So this mode opens /dev/fb0 and nothing else.  No DRM node, no master, no
 * modeset, no gbm, no EGL.  On this board /dev/fb0 is simple-framebuffer over the
 * memory the LK left at 0x82700000 -- the fix.smem_start line below is where that
 * is confirmed rather than believed -- and it is also the exact path mixdash draws
 * through, so what happens here is what will happen to the dashboard.
 *
 * It answers in three parts, in the order that splits the four causes:
 *
 *   1  the census.  Sample the framebuffer BEFORE touching it and count the
 *      pixels that carry any colour.  All black means nothing has drawn and the
 *      display path is not implicated at all.  A picture in memory with a black
 *      panel in front of it means the opposite: something has taken the scanout
 *      away from simple-framebuffer, and -i says who.
 *   2  the two states that are black on purpose.  A backlight at brightness 0 and
 *      a console in KD_GRAPHICS are both "working perfectly, showing nothing", and
 *      both are one write to undo.  They are undone here, because a probe that can
 *      see the cause of a black panel and leaves it black has not finished.
 *   3  the bars.  Eight colour bars, a one-pixel border and a corner-to-corner
 *      diagonal, written with the CPU straight into the mapping.  Nothing here can
 *      be given back the way a modeset cannot -- the next thing to draw simply
 *      draws over it -- so unlike -p this is safe to run on every boot, and when
 *      it runs just before the dashboard the bars are the handover: bars then a
 *      dashboard means both work, bars that stay means mixdash never started, and
 *      no bars at all means nothing userspace draws will ever be seen and the
 *      dashboard is not the thing to debug.
 *
 * The border and the diagonal are not decoration.  A wrong line_length shears a
 * vertical bar into a diagonal one and slides the border off the edge, which is
 * the one display fault that looks like working hardware.
 */

/* fbdev and console ABI by hand, for the reason EGL and GBM are: these numbers
 * are uapi, and linux/fb.h in the cross sysroot is a package that can be absent
 * from a build that otherwise needs no headers at all. */
#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOGET_FSCREENINFO 0x4602u
#define FBIOBLANK           0x4611u
#define FB_BLANK_UNBLANK    0

#define KDGETMODE           0x4B3Bu
#define KDSETMODE           0x4B3Au
#define KD_TEXT             0x00
#define KD_GRAPHICS         0x01

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

/* 160 bytes on this ABI, and the whole of it is copied by the ioctl, so it is
 * declared in full even though six fields are read. */
struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char          id[16];
    unsigned long smem_start;
    uint32_t      smem_len, type, type_aux, visual;
    uint16_t      xpanstep, ypanstep, ywrapstep;
    uint32_t      line_length;
    unsigned long mmio_start;
    uint32_t      mmio_len, accel;
    uint16_t      capabilities, reserved[2];
};

static int read_long_file(const char *path, long *out)
{
    char buf[32];
    int fd, n;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    *out = strtol(buf, NULL, 10);
    return 0;
}

static int write_long_file(const char *path, long v)
{
    char buf[32];
    int fd, n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = snprintf(buf, sizeof(buf), "%ld\n", v);
    n = (int)write(fd, buf, (size_t)n);
    close(fd);
    return n > 0 ? 0 : -1;
}

/*
 * A backlight at zero is a working display showing nothing, and on this board it
 * is one PWM register.  Reported always; written only when it is measurably the
 * cause, which is what "brightness 0" is.
 */
static void backlight_report(int repair)
{
    DIR *d;
    struct dirent *e;
    int seen = 0;

    d = opendir("/sys/class/backlight");
    if (!d) {
        printf("fb: no /sys/class/backlight, so the panel's brightness is whatever "
               "the LK left in DISP_PWM\n");
        return;
    }
    while ((e = readdir(d)) != NULL) {
        /*
         * Big enough that -Wformat-truncation has nothing to say.  d_name is
         * NAME_MAX (255) and the longest prefix+suffix here is 36 characters, so
         * 256 was five warnings per build about a path that in practice is
         * "/sys/class/backlight/mtk-pwm/max_brightness".  Truncation was never
         * going to happen and would only have meant a failed open if it did --
         * but five paragraphs of gcc note per build is how the one warning that
         * DID matter (a dangling QStringList in mixdash) went unread.
         */
        char path[512];
        long b = -1, mx = -1, pw = -1;

        if (e->d_name[0] == '.')
            continue;
        seen++;
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness", e->d_name);
        read_long_file(path, &mx);
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power", e->d_name);
        read_long_file(path, &pw);
        snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", e->d_name);
        read_long_file(path, &b);

        printf("fb: backlight %s brightness %ld of %ld, bl_power %ld\n",
               e->d_name, b, mx, pw);
        if (b == 0 && mx > 0) {
            printf("fb: that is a lit panel with the lamp off -- brightness 0 is "
                   "black whatever the framebuffer holds\n");
            if (repair) {
                if (write_long_file(path, mx) == 0)
                    printf("fb: set %s to %ld\n", e->d_name, mx);
                else
                    printf("fb: could not write %s: %m\n", path);
            }
        }
        if (pw > 0) {
            snprintf(path, sizeof(path), "/sys/class/backlight/%s/bl_power", e->d_name);
            printf("fb: bl_power %ld is FB_BLANK_* and not unblanked\n", pw);
            if (repair && write_long_file(path, 0) == 0)
                printf("fb: set %s bl_power to 0\n", e->d_name);
        }
    }
    closedir(d);
    if (!seen)
        printf("fb: /sys/class/backlight is empty -- no driver claims the lamp\n");
}

/*
 * KD_GRAPHICS is the other honest black.  Qt's linuxfb plugin sets it so the
 * kernel console stops drawing over the dashboard, and SDL sets it too; either one
 * that dies without restoring it leaves a panel with no text and no picture, which
 * is indistinguishable by eye from a dead display.  This is also the reason a boot
 * that "renders black with no output at all" can still be a working framebuffer.
 */
static void tty_report(int repair)
{
    int fd, mode = -1;

    fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("fb: /dev/tty0: %m -- cannot say whether the console is in graphics "
               "mode\n");
        return;
    }
    if (ioctl(fd, KDGETMODE, &mode) < 0) {
        printf("fb: KDGETMODE: %m\n");
        close(fd);
        return;
    }
    if (mode == KD_GRAPHICS) {
        printf("fb: /dev/tty0 is in KD_GRAPHICS, so the kernel console is not "
               "drawing anything at all.  Something put it there and did not put it "
               "back -- SDL, Qt's linuxfb or a plymouth -- and a black panel with no "
               "text is exactly what that looks like.\n");
        if (repair) {
            if (ioctl(fd, KDSETMODE, KD_TEXT) == 0)
                printf("fb: put /dev/tty0 back into KD_TEXT; the console will "
                       "repaint over anything drawn before this line\n");
            else
                printf("fb: KDSETMODE(KD_TEXT): %m\n");
        }
    } else {
        printf("fb: /dev/tty0 is in KD_TEXT, the console is drawing\n");
    }
    close(fd);
}

static uint32_t fb_colour(const struct fb_var_screeninfo *var,
                          unsigned r, unsigned g, unsigned b)
{
    /* Built from the driver's own bitfields rather than assumed to be XRGB8888:
     * simplefb takes its format from the DT and a 16bpp panel would otherwise
     * come out as noise that looks like a broken GPU. */
    uint32_t px = 0;
    px |= (uint32_t)(r >> (8 - var->red.length))   << var->red.offset;
    px |= (uint32_t)(g >> (8 - var->green.length)) << var->green.offset;
    px |= (uint32_t)(b >> (8 - var->blue.length))  << var->blue.offset;
    if (var->transp.length)
        px |= (uint32_t)((1u << var->transp.length) - 1u) << var->transp.offset;
    return px;
}

static void fb_put(unsigned char *base, uint32_t stride, uint32_t bpp,
                   uint32_t x, uint32_t y, uint32_t px)
{
    unsigned char *p = base + (size_t)y * stride + (size_t)x * (bpp / 8u);

    if (bpp == 32)
        *(volatile uint32_t *)p = px;
    else if (bpp == 16)
        *(volatile uint16_t *)p = (uint16_t)px;
    else if (bpp == 8)
        *(volatile uint8_t *)p = (uint8_t)px;
}

static uint32_t fb_get(const unsigned char *base, uint32_t stride, uint32_t bpp,
                       uint32_t x, uint32_t y)
{
    const unsigned char *p = base + (size_t)y * stride + (size_t)x * (bpp / 8u);

    if (bpp == 32)
        return *(const volatile uint32_t *)p;
    if (bpp == 16)
        return *(const volatile uint16_t *)p;
    if (bpp == 8)
        return *(const volatile uint8_t *)p;
    return 0;
}

/*
 * repair: undo the two states that are black on purpose.  paint_it: leave the
 * bars.  Returns 0 if there was a framebuffer to look at, 1 if there was not.
 */
static int fb_report(int repair, int paint_it, int secs)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    unsigned char *base;
    uint32_t mask, stride, bpp, w, h;
    size_t len;
    int fd, x, y, lit = 0, sampled = 0;

    fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("fb: /dev/fb0: %m\n");
        printf("fb: with no framebuffer device there is nothing for the kernel "
               "console or for Qt's linuxfb to draw into, and the panel can only be "
               "black.  simple-framebuffer comes from the framebuffer@82700000 node "
               "in the DT and needs CONFIG_FB_SIMPLE; in dmesg: \"simple-framebuffer\".\n");
        return 1;
    }

    memset(&var, 0, sizeof(var));
    memset(&fix, 0, sizeof(fix));
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        printf("fb: FBIOGET_?SCREENINFO: %m\n");
        close(fd);
        return 1;
    }

    fix.id[sizeof(fix.id) - 1] = 0;
    bpp = var.bits_per_pixel;
    stride = fix.line_length;
    w = var.xres;
    h = var.yres;
    printf("fb: /dev/fb0 \"%s\" %ux%u %ubpp, stride %u, %u bytes at 0x%08lx, "
           "visual %u\n", fix.id, w, h, bpp, stride, fix.smem_len,
           fix.smem_start, fix.visual);
    printf("fb: r%u+%u g%u+%u b%u+%u a%u+%u\n",
           var.red.offset, var.red.length, var.green.offset, var.green.length,
           var.blue.offset, var.blue.length, var.transp.offset, var.transp.length);

    {
        long blank = -1;
        if (read_long_file("/sys/class/graphics/fb0/blank", &blank) == 0 && blank != 0) {
            printf("fb: /sys/class/graphics/fb0/blank is %ld, so the framebuffer is "
                   "blanked -- another black that is not a fault\n", blank);
            if (repair) {
                if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) == 0)
                    printf("fb: unblanked it\n");
                else
                    printf("fb: FBIOBLANK: %m\n");
            }
        }
    }

    if (bpp != 32 && bpp != 16 && bpp != 8) {
        printf("fb: %ubpp is not a depth this probe writes, so the census and the "
               "bars are skipped\n", bpp);
        close(fd);
        return 0;
    }
    if (!w || !h || stride < w * (bpp / 8u)) {
        printf("fb: %ux%u at stride %u cannot be right, so nothing is written into "
               "it\n", w, h, stride);
        close(fd);
        return 0;
    }

    len = (size_t)stride * h;
    if (fix.smem_len && len > fix.smem_len)
        len = fix.smem_len;
    base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        printf("fb: mmap %u bytes: %m -- the geometry is readable but the memory is "
               "not, so nothing in userspace can draw here\n", (unsigned)len);
        close(fd);
        return 1;
    }

    /* The census, before a single pixel of ours goes in. */
    mask = ((var.red.length   ? ((1u << var.red.length)   - 1u) << var.red.offset   : 0u) |
            (var.green.length ? ((1u << var.green.length) - 1u) << var.green.offset : 0u) |
            (var.blue.length  ? ((1u << var.blue.length)  - 1u) << var.blue.offset  : 0u));
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            uint32_t px = fb_get(base, stride, bpp,
                                 (uint32_t)x * w / 64u, (uint32_t)y * h / 64u);
            sampled++;
            if (px & mask)
                lit++;
        }
    }
    printf("fb: %d of %d sampled pixels carry colour\n", lit, sampled);
    if (lit == 0)
        printf("fb: the framebuffer itself is black.  Nothing has drawn into it, so "
               "a black panel here is not a scanout fault and not a GPU one: it is "
               "the absence of a shell.  Whatever was supposed to draw did not.\n");
    else
        printf("fb: there is a picture in the framebuffer.  If the panel is still "
               "black in front of it then something took the scanout away from "
               "simple-framebuffer -- a DRM client that modeset and exited leaves the "
               "CRTC disabled, and -i names the node it would have been.\n");

    /* Both of these can repaint the screen, so they run before the bars. */
    backlight_report(repair);
    tty_report(repair);

    if (paint_it) {
        static const unsigned char bars[8][3] = {
            { 255, 255, 255 }, { 255, 255,   0 }, {   0, 255, 255 },
            {   0, 255,   0 }, { 255,   0, 255 }, { 255,   0,   0 },
            {   0,   0, 255 }, {   0,   0,   0 },
        };
        uint32_t px[8], white, i, ux, uy;

        for (i = 0; i < 8; i++)
            px[i] = fb_colour(&var, bars[i][0], bars[i][1], bars[i][2]);
        white = px[0];

        for (uy = 0; uy < h; uy++)
            for (ux = 0; ux < w; ux++)
                fb_put(base, stride, bpp, ux, uy, px[ux * 8u / w]);
        /* One-pixel border: it says where the visible extent really is. */
        for (ux = 0; ux < w; ux++) {
            fb_put(base, stride, bpp, ux, 0, white);
            fb_put(base, stride, bpp, ux, h - 1, white);
        }
        for (uy = 0; uy < h; uy++) {
            fb_put(base, stride, bpp, 0, uy, white);
            fb_put(base, stride, bpp, w - 1, uy, white);
        }
        /* And the diagonal, which is what shears if line_length is wrong. */
        for (uy = 0; uy < h; uy++) {
            ux = uy * w / h;
            if (ux < w)
                fb_put(base, stride, bpp, ux, uy, white);
        }
        msync(base, len, MS_SYNC);

        printf("fb: eight bars, white border, one diagonal, straight into "
               "/dev/fb0 with no DRM in the path.  Left to right: white, yellow, "
               "cyan, green, magenta, red, blue, black.\n");
        printf("fb: if the panel shows that, then the LK's display pipe, the panel "
               "and simple-framebuffer all work and anything black after this line "
               "is the thing that draws, not the thing that scans out.\n");
        if (fb_get(base, stride, bpp, w / 2u, h / 2u) != px[4])
            printf("fb: read-back at the centre does not match what was written, so "
                   "the mapping is not the scanned-out memory\n");
        if (secs > 0)
            sleep((unsigned)secs);
    }

    munmap(base, len);
    close(fd);
    return 0;
}

/* ── -p: the scanout test ──────────────────────────────────────────────────────
 *
 * Everything above measures whether a context can be built.  None of it can
 * answer the question the board is stuck on -- ES2 comes up on lima, ES runs,
 * and the panel is black -- because a config table says nothing about whether a
 * frame reaches the glass.
 *
 * DRM is spoken to with raw ioctls rather than through libdrm, for the reason EGL
 * and GBM are declared by hand at the top of this file: the uapi structures are
 * ABI, drm_ioctl() deliberately tolerates a caller whose struct is shorter than
 * the kernel's (it allocates max(user, driver), copies the user's size in and
 * zero-fills the rest), and dlopening libdrm.so.2 would add a fourth library
 * that can be absent.
 */

#define J36_IOC(dir, nr, sz) (((unsigned)(dir) << 30) | ('d' << 8) | \
                              (unsigned)(nr) | ((unsigned)(sz) << 16))
#define J36_IO(nr)           J36_IOC(0u, nr, 0)
#define J36_IOWR(nr, type)   J36_IOC(3u, nr, sizeof(type))

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

/*
 * The flip, for -c.  A modeset per frame is what -p does and it is right there --
 * five frames, three seconds each, and SETCRTC is the call that exercises the DSI.
 * An animation cannot pay for that: SETCRTC tears down and re-commits the whole
 * pipe, so a cube driven by it runs at the rate the DSI can be re-initialised
 * rather than at the rate the panel scans.  PAGE_FLIP swaps the plane's buffer at
 * the next vblank and answers with an event on the DRM fd, which is both the
 * cheap path and the paced one.
 *
 * user_data is a __u64 at offset 16 after four __u32, so this is 24 bytes on ARM
 * as it is in the kernel -- no padding to get wrong.
 */
struct drm_mode_crtc_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};

/* What read() returns on the DRM fd: a header, then length-8 bytes of body. */
struct drm_event {
    uint32_t type, length;
};

#define DRM_MODE_PAGE_FLIP_EVENT  0x01
#define DRM_EVENT_FLIP_COMPLETE   2

#define DRM_IOCTL_SET_MASTER        J36_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER       J36_IO(0x1f)
#define DRM_IOCTL_MODE_GETRESOURCES J36_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      J36_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      J36_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   J36_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR J36_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_RMFB         J36_IOWR(0xAF, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB  J36_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     J36_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB J36_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_IOCTL_MODE_ADDFB2       J36_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_PAGE_FLIP    J36_IOWR(0xB0, struct drm_mode_crtc_page_flip)

/*
 * DRM_IOCTL_VERSION, so a node can be named instead of guessed at.  The lengths are
 * __kernel_size_t, which is unsigned long -- 32 bits here -- and the kernel's
 * drm_copy_field() copies min(strlen, len) bytes and does NOT terminate, so every
 * buffer below is zeroed first and asked for one byte less than it has.
 */
struct drm_version {
    int           version_major, version_minor, version_patchlevel;
    unsigned long name_len;
    char         *name;
    unsigned long date_len;
    char         *date;
    unsigned long desc_len;
    char         *desc;
};

#define DRM_IOCTL_VERSION           J36_IOWR(0x00, struct drm_version)

#define GL_COLOR_BUFFER_BIT        0x00004000

/* How long each frame is held up.  Long enough to see, short enough that five of
 * them do not look like a hang on a board whose next line is ES starting. */
#define HOLD_SECONDS 3

static int      paint_fd = -1;
static uint32_t paint_crtc, paint_conn;
static struct   drm_mode_modeinfo paint_mode;
static int      paint_phase;

static int drm_ioctl(unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(paint_fd, req, arg);
    } while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* ── which node drives the panel ───────────────────────────────────────────────
 *
 * This file assumed /dev/dri/card0 was mtk_drm.  On the kernel we ship it is not,
 * and -p said so exactly:
 *
 *     GETRESOURCES: Operation not supported
 *
 * drm_mode_getresources() returns EOPNOTSUPP for one reason and no other: the
 * driver behind that node was registered without DRIVER_MODESET.  lima is
 * registered that way on purpose -- a Mali-450 has no scanout, it rasterises into
 * memory and something else puts that memory on a wire -- so card0 there is the
 * GPU, and that message was never a fault in the display path.  It was this probe
 * aimed at the wrong chip, and every -p run so far measured nothing.
 *
 * Which minor number each driver gets is decided by probe order, so it is not a
 * thing to hard-code on either side of a kernel change.  Ask instead: name every
 * node, test each for modesetting, and take the first that has both a CRTC and a
 * connector.  The table is printed whatever the outcome, because "card0 is lima and
 * card1 is mediatek" is the single line that would have saved the week this cost.
 */

static char display_path[32];
static int  display_looked;

static int node_report(const char *path)
{
    struct drm_version v;
    struct drm_mode_card_res res;
    char name[32], desc[64];
    int fd, modeset, usable = 0;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("dri: %s: %m\n", path);
        return 0;
    }

    memset(name, 0, sizeof(name));
    memset(desc, 0, sizeof(desc));
    memset(&v, 0, sizeof(v));
    v.name_len = sizeof(name) - 1;
    v.name = name;
    v.desc_len = sizeof(desc) - 1;
    v.desc = desc;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
        snprintf(name, sizeof(name), "?");

    memset(&res, 0, sizeof(res));
    modeset = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0;

    if (!modeset) {
        printf("dri: %s \"%s\" %d.%d.%d -- no modesetting (%m): a render-only "
               "driver, so it can rasterise but cannot scan out\n",
               path, name, v.version_major, v.version_minor, v.version_patchlevel);
    } else {
        printf("dri: %s \"%s\" %d.%d.%d -- %u crtc, %u connector, %u encoder, "
               "%ux%u..%ux%u%s\n",
               path, name, v.version_major, v.version_minor, v.version_patchlevel,
               res.count_crtcs, res.count_connectors, res.count_encoders,
               res.min_width, res.min_height, res.max_width, res.max_height,
               (res.count_crtcs && res.count_connectors) ? "" :
                   " -- modesets, but has nothing to modeset");
        usable = res.count_crtcs > 0 && res.count_connectors > 0;
    }
    if (desc[0])
        printf("dri: %s is \"%s\"\n", path, desc);

    close(fd);
    return usable;
}

/*
 * NULL means no node on this board can scan out, which is a finding and not an
 * error to be retried: it says mediatek-drm never bound, and the picture currently
 * on the panel is the LK's framebuffer through simple-framebuffer, untouched by
 * anything in DRM.
 */
static const char *display_node(void)
{
    char path[32];
    int i;

    if (display_looked)
        return display_path[0] ? display_path : NULL;
    display_looked = 1;

    printf("dri: naming every node, because which minor a driver gets is probe "
           "order and not a property of the SoC\n");

    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (access(path, F_OK) != 0)
            continue;
        if (node_report(path) && !display_path[0])
            snprintf(display_path, sizeof(display_path), "%s", path);
    }
    /* Reported but never chosen: a render node is what lima is reached through, and
     * seeing its name here is what confirms the kmsro pairing has both halves. */
    for (i = 128; i < 132; i++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        if (access(path, F_OK) == 0)
            node_report(path);
    }

    if (display_path[0]) {
        printf("dri: the display is %s\n", display_path);
        return display_path;
    }

    printf("dri: no node in /dev/dri modesets, so nothing can be scanned out "
           "through DRM on this boot.\n");
    printf("dri: mediatek-drm did not bind.  What is on the panel right now is the "
           "framebuffer the LK left at 0x82700000, through simple-framebuffer, and "
           "it is not DRM's.  In dmesg: \"mediatek-drm\", \"mtk_dsi\", the panel's "
           "own compatible, and any -EPROBE_DEFER that never resolved.\n");
    return NULL;
}

/*
 * Put a framebuffer on the CRTC with a full modeset every time, rather than
 * page-flipping after the first.  A flip needs the event loop and tells us less:
 * SETCRTC is what SDL's KMSDRM backend does for its first frame and it is the one
 * call that exercises the DSI and the panel as well as the OVL.
 */
static int setcrtc(uint32_t fb)
{
    struct drm_mode_crtc c;
    uint32_t conn = paint_conn;

    memset(&c, 0, sizeof(c));
    c.crtc_id = paint_crtc;
    c.fb_id = fb;
    c.set_connectors_ptr = (uint64_t)(uintptr_t)&conn;
    c.count_connectors = 1;
    c.mode = paint_mode;
    c.mode_valid = 1;

    return drm_ioctl(DRM_IOCTL_MODE_SETCRTC, &c);
}

static int show(uint32_t fb, const char *what)
{
    paint_phase++;
    if (setcrtc(fb) < 0) {
        printf("paint %d: SETCRTC for %s: %m\n", paint_phase, what);
        return -1;
    }
    printf("paint %d: the panel should be %s for %ds now\n",
           paint_phase, what, HOLD_SECONDS);
    sleep(HOLD_SECONDS);
    return 0;
}

/*
 * A CPU-filled dumb buffer: no GL, no gbm, no lima, no Mesa at all.  This is the
 * half of the question that the GL phases cannot separate out -- if a solid
 * colour written with memset() does not appear, nothing about EGL is involved and
 * the fault is the modeset, the DSI or the panel.
 */
static int paint_dumb(uint32_t fourcc, uint32_t pixel, const char *what)
{
    struct drm_mode_create_dumb cd;
    struct drm_mode_map_dumb md;
    struct drm_mode_destroy_dumb dd;
    struct drm_mode_fb_cmd2 fb;
    uint32_t *p, x, n;
    void *map;
    int ret = -1;

    memset(&cd, 0, sizeof(cd));
    cd.width = paint_mode.hdisplay;
    cd.height = paint_mode.vdisplay;
    cd.bpp = 32;
    if (drm_ioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        printf("paint %d: CREATE_DUMB for %s: %m\n", paint_phase + 1, what);
        return -1;
    }

    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (drm_ioctl(DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        printf("paint %d: MAP_DUMB for %s: %m\n", paint_phase + 1, what);
        goto out_handle;
    }
    map = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
               paint_fd, (off_t)md.offset);
    if (map == MAP_FAILED) {
        printf("paint %d: mmap of %llu bytes for %s: %m\n", paint_phase + 1,
               (unsigned long long)cd.size, what);
        goto out_handle;
    }

    /* Row by row, because pitch is not width*4 on every allocator. */
    for (n = 0; n < cd.height; n++) {
        p = (uint32_t *)((char *)map + (size_t)n * cd.pitch);
        for (x = 0; x < cd.width; x++)
            p[x] = pixel;
    }
    munmap(map, (size_t)cd.size);

    memset(&fb, 0, sizeof(fb));
    fb.width = cd.width;
    fb.height = cd.height;
    fb.pixel_format = fourcc;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (drm_ioctl(DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        printf("paint %d: ADDFB2 %c%c%c%c for %s: %m -- this format is not "
               "scanout-capable on this plane\n", paint_phase + 1,
               fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff,
               (fourcc >> 24) & 0xff, what);
        goto out_handle;
    }

    /* show() is what numbers the phase, so that a phase number in the log always
     * means a frame that was actually put on the CRTC. */
    ret = show(fb.fb_id, what);
    drm_ioctl(DRM_IOCTL_MODE_RMFB, &fb.fb_id);

out_handle:
    memset(&dd, 0, sizeof(dd));
    dd.handle = cd.handle;
    drm_ioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    return ret;
}

/*
 * The same colour, this time drawn by lima into a gbm surface and scanned out of
 * the buffer EGL hands back -- which is the whole of ES's path with ES, SDL and
 * the renderer taken out of it.  ARGB8888 because that is the format SDL's KMSDRM
 * backend hardcodes for its gbm surface (SDL_kmsdrmvideo.c:1197) whatever
 * SDL_GL_ALPHA_SIZE was asked for, so it is the format ES will use.
 *
 * Two frames, in two colours.  The second one comes out of the other buffer of
 * the swap chain, so it says whether the chain rotates: a first frame that
 * appears and a second that does not is a buffer that was never imported.
 */
static int paint_gl(struct gbm_device *gbm)
{
    static const struct { float r, g, b; const char *name; } FRAMES[] = {
        { 1.0f, 0.0f, 1.0f, "MAGENTA, drawn by lima into an AR24 gbm surface" },
        { 0.0f, 1.0f, 0.0f, "GREEN, lima's second frame, the other buffer of the chain" },
    };
    void (*gl_clear_color)(float, float, float, float);
    void (*gl_clear)(unsigned int);
    void (*gl_finish)(void);
    EGLDisplay dpy = NULL;
    EGLConfig pick[64], cfg;
    EGLContext ctx;
    EGLSurface surf;
    struct gbm_surface *gs;
    struct gbm_bo *held = NULL;
    uint32_t held_fb = 0;
    EGLint count = 0, i, e;
    EGLint attr[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    size_t f;
    int shown = 0;

    if (!p_eglSwapBuffers || !p_gbm_surface_lock_front_buffer ||
        !p_gbm_surface_release_buffer || !p_gbm_bo_get_handle ||
        !p_gbm_bo_get_stride) {
        printf("paint: the GL phases need eglSwapBuffers, "
               "gbm_surface_lock_front_buffer, gbm_surface_release_buffer, "
               "gbm_bo_get_handle and gbm_bo_get_stride, and this payload is "
               "missing at least one of them\n");
        return 0;
    }

    if (p_eglGetPlatformDisplayEXT)
        dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!dpy && p_eglGetPlatformDisplay)
        dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!dpy)
        dpy = p_eglGetDisplay(gbm);
    if (!dpy || !p_eglInitialize(dpy, NULL, NULL)) {
        printf("paint: no EGL on this device, so the GL phases cannot run\n");
        return 0;
    }

    eglclear();
    if (!p_eglBindAPI(EGL_OPENGL_ES_API) ||
        !p_eglChooseConfig(dpy, attr, pick, 64, &count) || count == 0) {
        printf("paint: no ES2 window config\n");
        goto out_dpy;
    }
    cfg = pick[0];
    for (i = 0; i < count; i++) {
        EGLint vis = 0;
        p_eglGetConfigAttrib(dpy, pick[i], EGL_NATIVE_VISUAL_ID, &vis);
        if ((uint32_t)vis == FOURCC_ARGB8888) {
            cfg = pick[i];
            break;
        }
    }

    gs = p_gbm_surface_create(gbm, paint_mode.hdisplay, paint_mode.vdisplay,
                              FOURCC_ARGB8888,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gs) {
        printf("paint: gbm_surface_create %ux%u AR24 SCANOUT|RENDERING failed -- "
               "kmsro cannot pair lima with this display device\n",
               paint_mode.hdisplay, paint_mode.vdisplay);
        goto out_dpy;
    }

    ctx = p_eglCreateContext(dpy, cfg, NULL, cattr);
    surf = ctx ? p_eglCreateWindowSurface(dpy, cfg, gs, NULL) : NULL;
    if (!ctx || !surf || !p_eglMakeCurrent(dpy, surf, surf, ctx)) {
        e = p_eglGetError();
        printf("paint: no current ES2 context on that surface: 0x%04x %s\n",
               e, eglerr(e));
        goto out_gs;
    }

    *(void **)(&gl_clear_color) = p_eglGetProcAddress("glClearColor");
    *(void **)(&gl_clear) = p_eglGetProcAddress("glClear");
    *(void **)(&gl_finish) = p_eglGetProcAddress("glFinish");
    if (!gl_clear_color || !gl_clear) {
        printf("paint: eglGetProcAddress has no glClear\n");
        goto out_current;
    }

    for (f = 0; f < sizeof(FRAMES) / sizeof(FRAMES[0]); f++) {
        struct gbm_bo *bo;
        struct drm_mode_fb_cmd2 fb;
        union gbm_bo_handle h;

        /*
         * Alpha 1.  A clear to alpha 0 is the other experiment and the CPU phases
         * above run it without a GPU in the way; here the point is a frame that
         * cannot be blended away, so that a black panel means the frame did not
         * arrive rather than that it arrived transparent.
         */
        gl_clear_color(FRAMES[f].r, FRAMES[f].g, FRAMES[f].b, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        if (gl_finish)
            gl_finish();
        if (!p_eglSwapBuffers(dpy, surf)) {
            e = p_eglGetError();
            printf("paint: eglSwapBuffers: 0x%04x %s\n", e, eglerr(e));
            break;
        }

        bo = p_gbm_surface_lock_front_buffer(gs);
        if (!bo) {
            printf("paint: gbm_surface_lock_front_buffer returned nothing after "
                   "a swap -- there is no front buffer to scan out\n");
            break;
        }
        h = p_gbm_bo_get_handle(bo);

        memset(&fb, 0, sizeof(fb));
        fb.width = paint_mode.hdisplay;
        fb.height = paint_mode.vdisplay;
        fb.pixel_format = FOURCC_ARGB8888;
        fb.handles[0] = h.u32;
        fb.pitches[0] = p_gbm_bo_get_stride(bo);
        if (drm_ioctl(DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
            printf("paint %d: ADDFB2 on lima's buffer (handle %u, stride %u): %m "
                   "-- lima rendered it but the display device will not scan it "
                   "out\n", paint_phase + 1, fb.handles[0], fb.pitches[0]);
            p_gbm_surface_release_buffer(gs, bo);
            break;
        }

        if (show(fb.fb_id, FRAMES[f].name) == 0)
            shown++;

        /* Only now is the previous frame off the CRTC and safe to give back. */
        if (held) {
            drm_ioctl(DRM_IOCTL_MODE_RMFB, &held_fb);
            p_gbm_surface_release_buffer(gs, held);
        }
        held = bo;
        held_fb = fb.fb_id;
    }

    if (held) {
        drm_ioctl(DRM_IOCTL_MODE_RMFB, &held_fb);
        p_gbm_surface_release_buffer(gs, held);
    }

out_current:
    p_eglMakeCurrent(dpy, NULL, NULL, NULL);
    if (surf)
        p_eglDestroySurface(dpy, surf);
    if (ctx)
        p_eglDestroyContext(dpy, ctx);
out_gs:
    p_gbm_surface_destroy(gs);
out_dpy:
    /* eglTerminate takes the context and the surface with it on the paths that
     * jump past their destructors. */
    p_eglTerminate(dpy);
    return shown;
}

/*
 * Which connector, which mode, which CRTC.  -p and -c need exactly the same
 * answer, so they ask the same function for it: the first connected connector
 * that reports a mode, its mode 0, and a CRTC one of its encoders can drive.
 * Fills paint_conn, paint_mode and paint_crtc; returns 0 when this device has
 * nothing to scan out to.
 */
static int find_crtc(void)
{
    struct drm_mode_card_res res;
    uint32_t crtcs[8], conns[16], encs[16];
    struct drm_mode_get_connector conn;
    struct drm_mode_modeinfo modes[64];
    struct drm_mode_get_encoder enc;
    uint32_t i, j, k;
    int found = 0;

    paint_crtc = 0;

    memset(&res, 0, sizeof(res));
    res.count_crtcs = 8;
    res.count_connectors = 16;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    if (drm_ioctl(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        printf("GETRESOURCES: %m -- this node has no modesetting at all "
               "(a render node, or a driver without DRIVER_MODESET)\n");
        return 0;
    }
    printf("paint: %u crtc, %u connector, %u encoder, %ux%u..%ux%u\n",
           res.count_crtcs, res.count_connectors, res.count_encoders,
           res.min_width, res.min_height, res.max_width, res.max_height);

    for (i = 0; i < res.count_connectors && i < 16 && !found; i++) {
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = conns[i];
        /* Zero counts first: that is what makes the kernel probe the connector
         * and fill the counts in, and it is the only way to learn count_modes. */
        if (drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            printf("paint: connector %u: %m\n", conns[i]);
            continue;
        }
        printf("paint: connector %u type %u, connection %u, %u modes\n",
               conn.connector_id, conn.connector_type, conn.connection,
               conn.count_modes);
        if (conn.connection != 1 || conn.count_modes == 0)
            continue;

        /*
         * The counts go back unchanged, not clamped.  drm_mode_getconnector only
         * copies a list when the caller's count is >= the kernel's, so asking for
         * fewer than there are is not a truncated read -- it is no read at all,
         * with the count returned and the array left as it was.  A panel with more
         * than 64 modes is not a thing, but a garbage modes[0] would be a modeset
         * to a garbage timing, so it is refused rather than clamped.
         */
        if (conn.count_modes > 64) {
            printf("paint: connector %u reports %u modes, more than this probe "
                   "will read\n", conn.connector_id, conn.count_modes);
            continue;
        }
        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        if (conn.count_encoders > 16)
            conn.count_encoders = 0;   /* fall back to conn.encoder_id alone */
        else
            conn.encoders_ptr = (uint64_t)(uintptr_t)encs;
        conn.count_props = 0;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        if (drm_ioctl(DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            printf("paint: connector %u second pass: %m\n", conns[i]);
            continue;
        }
        if (conn.count_modes == 0 || conn.count_modes > 64) {
            printf("paint: connector %u's mode list moved between the two "
                   "passes\n", conn.connector_id);
            continue;
        }

        paint_conn = conn.connector_id;
        paint_mode = modes[0];
        printf("paint: mode \"%s\" %ux%u, %u kHz, flags 0x%x, type 0x%x\n",
               paint_mode.name, paint_mode.hdisplay, paint_mode.vdisplay,
               paint_mode.clock, paint_mode.flags, paint_mode.type);

        /* The encoder's current CRTC if it has one, otherwise the first CRTC any
         * of this connector's encoders can drive. */
        for (j = 0; j < conn.count_encoders + 1 && !paint_crtc; j++) {
            memset(&enc, 0, sizeof(enc));
            enc.encoder_id = j == 0 ? conn.encoder_id : encs[j - 1];
            if (!enc.encoder_id)
                continue;
            if (drm_ioctl(DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
                continue;
            if (enc.crtc_id) {
                paint_crtc = enc.crtc_id;
                break;
            }
            for (k = 0; k < res.count_crtcs && k < 8; k++) {
                if (enc.possible_crtcs & (1u << k)) {
                    paint_crtc = crtcs[k];
                    break;
                }
            }
        }
        if (!paint_crtc) {
            printf("paint: connector %u has no CRTC to drive it\n", paint_conn);
            continue;
        }
        found = 1;
    }

    if (!found)
        printf("paint: no connected connector with a mode -- the panel bridge "
               "never attached, so there is nothing to scan out to\n");

    return found;
}

/*
 * Five phases, in the order that narrows the fault:
 *
 *   1  RED, XR24, CPU-filled       modeset + DSI + panel + OVL, no alpha anywhere
 *   2  MAGENTA, AR24 alpha ff      the same with the format SDL will hand it
 *   3  MAGENTA, AR24 alpha 00      the same buffer, transparent
 *   4  MAGENTA, lima into gbm      the ES path with ES, SDL and the renderer out
 *   5  GREEN, lima's second frame  the swap chain rotating
 *
 * and four verdicts:
 *
 *   nothing at all          the modeset does not reach the glass.  Nothing about
 *                           EGL is involved: it is the DSI being re-initialised
 *                           under the panel, or the mode the panel driver
 *                           reports, and the place to look is mtk_dsi against the
 *                           state the LK left.
 *   1 and 2 but not 3       the OVL blends per-pixel alpha against a black
 *                           background.  Then ES is invisible for one reason and
 *                           it is in the renderer, not the kernel:
 *                           Renderer_GLES20.cpp clears to (0,0,0,0) and every
 *                           pixel it does not overdraw is transparent black.
 *   1, 2, 3 but not 4 or 5  the display path is sound and the fault is the
 *                           gbm/kmsro pairing -- lima's buffer imports but never
 *                           becomes what the OVL fetches.
 *   all five               the display path is sound end to end, and a black ES
 *                           is ES's own drawing.  The GLES2 self-test line and
 *                           the per-frame draw count are then the evidence.
 */
static int paint(const char *path)
{
    struct drm_mode_crtc old;
    struct gbm_device *gbm;
    int ok = 0;

    printf("== %s, scanout test\n", path);

    paint_fd = open(path, O_RDWR | O_CLOEXEC);
    if (paint_fd < 0) {
        printf("open: %m\n");
        return 1;
    }

    /*
     * Ask for master explicitly.  Opening the only open of the device makes us
     * master implicitly, but ES may have been here first on a re-run and the
     * error is worth naming: EBUSY here means something else owns the display and
     * every SETCRTC below would have failed with EACCES instead.
     */
    if (drm_ioctl(DRM_IOCTL_SET_MASTER, NULL) < 0)
        printf("paint: SET_MASTER: %m (a modeset may not be permitted)\n");

    if (!find_crtc())
        goto out;

    /*
     * What is on that CRTC right now, before anything of ours is.  A non-zero
     * fb_id means the kernel's own fbdev client is scanning out and the console
     * is visible, which is worth knowing: it means the pipe works and only the
     * frames are in question.
     */
    memset(&old, 0, sizeof(old));
    old.crtc_id = paint_crtc;
    if (drm_ioctl(DRM_IOCTL_MODE_GETCRTC, &old) == 0)
        printf("paint: crtc %u currently has fb %u, mode_valid %u \"%s\"\n",
               paint_crtc, old.fb_id, old.mode_valid, old.mode.name);

    printf("paint: five phases, %ds each.  Watch the panel and note which of "
           "them you see.\n", HOLD_SECONDS);

    if (paint_dumb(FOURCC_XRGB8888, 0xffff0000u,
                   "RED, no alpha channel (XR24), written by the CPU") == 0)
        ok++;
    if (paint_dumb(FOURCC_ARGB8888, 0xffff00ffu,
                   "MAGENTA, opaque (AR24, alpha ff), written by the CPU") == 0)
        ok++;
    if (paint_dumb(FOURCC_ARGB8888, 0x00ff00ffu,
                   "MAGENTA, transparent (AR24, alpha 00), written by the CPU -- "
                   "black here and colour above means the OVL blends alpha") == 0)
        ok++;

    gbm = p_gbm_create_device(paint_fd);
    if (!gbm) {
        printf("paint: gbm_create_device on the display node failed, so the two "
               "GL phases cannot run\n");
    } else {
        ok += paint_gl(gbm);
        p_gbm_device_destroy(gbm);
    }

    printf("paint: %d of the 5 phases reached the CRTC without an error.  In "
           "order, the panel should have shown: red, magenta, magenta (or black, "
           "if alpha is blended), magenta, green.\n", ok);
    /* Not "the console comes back": it does not, and saying so here was wrong.
     * Dropping master gives up the right to modeset, not the mode.  The CRTC keeps
     * scanning our last framebuffer, and when this process exits and the kernel
     * releases that framebuffer the CRTC is disabled instead -- with
     * CONFIG_DRM_FBDEV_EMULATION=n there is no in-kernel client to restore
     * anything.  Either way /dev/fb0 is no longer what is on the glass. */
    printf("paint: the panel belongs to this modeset until the next reboot.  "
           "Master is dropped, but nothing hands the pipe back to "
           "simple-framebuffer, so writes to /dev/fb0 still succeed and are no "
           "longer seen.  Run eglprobe -f before this, not after.\n");

out:
    drm_ioctl(DRM_IOCTL_DROP_MASTER, NULL);
    close(paint_fd);
    paint_fd = -1;
    return ok ? 0 : 1;
}

/* ── -c: the rotating cube ─────────────────────────────────────────────────────
 *
 * -p answers "does a frame reach the glass".  It cannot answer "does the GPU
 * work", because every one of its five phases is a clear: three written by the
 * CPU and two by glClear, and a driver that can only clear a buffer would pass
 * all five.  Nothing in this bring-up has yet asked lima to rasterise anything.
 *
 * So this does the smallest thing that cannot be faked by a clear: it compiles
 * two shaders, uploads 36 vertices and draws a cube that turns.  Everything it
 * exercises is something no other phase touches -- the shader compiler, the
 * vertex path, the depth buffer, the tiler -- and the answer is legible from
 * across the room, which on a 640x480 panel photographed by hand is the point.
 *
 * It is also the first thing here that pages: -p re-does a full modeset for each
 * of its five frames, which is right for five frames and impossible for sixty a
 * second, so this flips and waits for the vblank event.  If PAGE_FLIP is refused
 * it says so and falls back to the modeset, because a slow cube is still an
 * answer and a missing one is not.
 *
 * What it deliberately does NOT do is link against GLES.  Every entry point comes
 * through eglGetProcAddress, so the only DT_NEEDED here is still libc and the GL
 * it measures is whichever one LD_LIBRARY_PATH found -- for us /run/j36/gl, the
 * Mesa payload, and not the RK3326 Mali blob the shared rootfs points /usr/lib at.
 */

/* Long enough to see it turn several times, short enough that the dashboard does
 * not look wedged while it holds the panel. */
#define CUBE_SECONDS 20
#define CUBE_PI 3.14159265358979f

/*
 * sin by hand.  sinf() lives in libm, and libm in DT_NEEDED would break the one
 * property this file is built around -- that a missing library is a printed result
 * rather than a dead loader -- for the sake of two calls a frame.  Reduced to
 * [-pi/2, pi/2] first, where the ninth-order Taylor series is good to about 2e-6,
 * which is below float precision and far below one pixel of cube.
 */
static float rsin(float a)
{
    const float two = 2.0f * CUBE_PI;
    float x2;

    a -= (float)(int)(a / two) * two;
    if (a >  CUBE_PI) a -= two;
    if (a < -CUBE_PI) a += two;
    if (a >  CUBE_PI / 2.0f) a =  CUBE_PI - a;
    if (a < -CUBE_PI / 2.0f) a = -CUBE_PI - a;

    x2 = a * a;
    return a * (1.0f - x2 * (1.0f / 6.0f
                    - x2 * (1.0f / 120.0f
                    - x2 * (1.0f / 5040.0f
                    - x2 * (1.0f / 362880.0f)))));
}

static float rcos(float a)
{
    return rsin(a + CUBE_PI / 2.0f);
}

/* Column-major, the way GL wants them, so glUniformMatrix4fv takes them as they
 * are and there is no transpose to get backwards. */
static void m_zero(float *m)
{
    memset(m, 0, 16 * sizeof(float));
}

/* out = a * b.  out must not alias either input. */
static void m_mul(float *out, const float *a, const float *b)
{
    int c, r, k;

    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

static void m_perspective(float *m, float fovy_deg, float aspect, float zn, float zf)
{
    const float h = fovy_deg * CUBE_PI / 360.0f;   /* half the field, in radians */
    const float f = rcos(h) / rsin(h);             /* cot(h), i.e. 1/tan(h) */

    m_zero(m);
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1.0f;
    m[14] = 2.0f * zf * zn / (zn - zf);
}

static void m_translate(float *m, float x, float y, float z)
{
    m_zero(m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void m_rot_x(float *m, float a)
{
    const float s = rsin(a), c = rcos(a);

    m_zero(m);
    m[0] = 1.0f;
    m[5] = c;  m[6] = s;
    m[9] = -s; m[10] = c;
    m[15] = 1.0f;
}

static void m_rot_y(float *m, float a)
{
    const float s = rsin(a), c = rcos(a);

    m_zero(m);
    m[0] = c;  m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;  m[10] = c;
    m[15] = 1.0f;
}

/*
 * 36 vertices: six faces, two triangles each, position then colour.  Built from a
 * face table rather than written out, because the winding is the part that has to
 * be right -- u x v is the outward normal, so (o, o+u, o+u+v) and (o, o+u+v, o+v)
 * are counter-clockwise seen from outside, which is what GL_CULL_FACE's default
 * GL_CCW/GL_BACK pair expects.  Get that backwards and the cube renders
 * inside-out, which looks like a depth bug and is not one.
 *
 * The colours are MixOS's six accents, so the cube is recognisably this project's
 * and each face is distinguishable in a photograph.
 */
static float cube_verts[36 * 6];

static void build_cube(void)
{
    static const struct { float o[3], u[3], v[3], c[3]; } FACES[6] = {
        /* +X */ { { 1, -1, -1 }, { 0, 2, 0 }, { 0, 0, 2 }, { 0.039f, 0.518f, 1.000f } },
        /* -X */ { { -1, -1, -1 }, { 0, 0, 2 }, { 0, 2, 0 }, { 0.188f, 0.690f, 0.780f } },
        /* +Y */ { { -1, 1, -1 }, { 0, 0, 2 }, { 2, 0, 0 }, { 1.000f, 0.388f, 0.518f } },
        /* -Y */ { { -1, -1, -1 }, { 2, 0, 0 }, { 0, 0, 2 }, { 0.580f, 0.439f, 0.859f } },
        /* +Z */ { { -1, -1, 1 }, { 2, 0, 0 }, { 0, 2, 0 }, { 1.000f, 0.624f, 0.039f } },
        /* -Z */ { { -1, -1, -1 }, { 0, 2, 0 }, { 2, 0, 0 }, { 0.157f, 0.784f, 0.251f } }
    };
    static const int TRI[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 },
                                   { 0, 0 }, { 1, 1 }, { 0, 1 } };
    int f, k, i, n = 0;

    for (f = 0; f < 6; f++) {
        for (k = 0; k < 6; k++) {
            for (i = 0; i < 3; i++)
                cube_verts[n++] = FACES[f].o[i]
                                + (float)TRI[k][0] * FACES[f].u[i]
                                + (float)TRI[k][1] * FACES[f].v[i];
            for (i = 0; i < 3; i++)
                cube_verts[n++] = FACES[f].c[i];
        }
    }
}

/*
 * eglGetProcAddress first, because that is the one function guaranteed to come
 * from the same driver the context did.  libGLESv2.so.2 is the fallback for an EGL
 * older than 1.5, which is only allowed to return extension entry points -- and it
 * is a fallback and not the first choice on purpose: on this card's shared rootfs
 * /usr/lib's libGLESv2.so.2 is a symlink to the R36S's ARMv8-A libMali.so, so this
 * dlopen fails cleanly rather than loading the wrong GPU's driver.
 */
static void *cube_gles;

static void *glsym(const char *name)
{
    void *p = p_eglGetProcAddress(name);

    if (p)
        return p;
    if (!cube_gles)
        cube_gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    return cube_gles ? dlsym(cube_gles, name) : NULL;
}

#define GLSYM(name)                                                             \
    do {                                                                        \
        *(void **)(&p_##name) = glsym(#name);                                   \
        if (!p_##name) {                                                        \
            printf("cube: this GLES2 has no %s\n", #name);                      \
            return 0;                                                           \
        }                                                                       \
    } while (0)

static int load_gles(void)
{
    GLSYM(glViewport);
    GLSYM(glEnable);
    GLSYM(glDepthFunc);
    GLSYM(glClearColor);
    GLSYM(glClear);
    GLSYM(glGetError);
    GLSYM(glGetString);
    GLSYM(glCreateShader);
    GLSYM(glShaderSource);
    GLSYM(glCompileShader);
    GLSYM(glGetShaderiv);
    GLSYM(glGetShaderInfoLog);
    GLSYM(glDeleteShader);
    GLSYM(glCreateProgram);
    GLSYM(glAttachShader);
    GLSYM(glLinkProgram);
    GLSYM(glGetProgramiv);
    GLSYM(glGetProgramInfoLog);
    GLSYM(glUseProgram);
    GLSYM(glGetAttribLocation);
    GLSYM(glGetUniformLocation);
    GLSYM(glUniformMatrix4fv);
    GLSYM(glVertexAttribPointer);
    GLSYM(glEnableVertexAttribArray);
    GLSYM(glDrawArrays);

    /* Optional: eglSwapBuffers flushes, so this is only belt and braces. */
    *(void **)(&p_glFinish) = glsym("glFinish");
    return 1;
}
#undef GLSYM

static const char CUBE_VS[] =
    "attribute vec3 a_pos;\n"
    "attribute vec3 a_col;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec3 v_col;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "    v_col = a_col;\n"
    "}\n";

static const char CUBE_FS[] =
    "precision mediump float;\n"
    "varying vec3 v_col;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = vec4(v_col, 1.0);\n"
    "}\n";

static unsigned int compile_shader(unsigned int kind, const char *src, const char *what)
{
    unsigned int sh;
    int ok = 0, len = 0;
    char log[512];

    sh = p_glCreateShader(kind);
    if (!sh) {
        printf("cube: glCreateShader(%s) returned 0, gl error 0x%04x\n",
               what, p_glGetError());
        return 0;
    }
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        log[0] = '\0';
        p_glGetShaderInfoLog(sh, (int)sizeof(log), &len, log);
        log[sizeof(log) - 1] = '\0';
        printf("cube: the %s shader did not compile: %s\n",
               what, log[0] ? log : "(the driver returned no log)");
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/*
 * One fb per gbm buffer, kept.  A gbm surface rotates through a handful of buffers
 * and hands the same ones back for the rest of the run, so ADDFB2 per frame would
 * be an ioctl and a kernel allocation for a framebuffer that already exists.
 */
struct bofb { uint32_t handle, fb; };
static struct bofb cube_fbs[8];
static int cube_fb_n;

static uint32_t fb_for(struct gbm_bo *bo)
{
    struct drm_mode_fb_cmd2 fb;
    union gbm_bo_handle h = p_gbm_bo_get_handle(bo);
    int i;

    for (i = 0; i < cube_fb_n; i++)
        if (cube_fbs[i].handle == h.u32)
            return cube_fbs[i].fb;

    if (cube_fb_n == (int)(sizeof(cube_fbs) / sizeof(cube_fbs[0]))) {
        printf("cube: more than %d gbm buffers in the swap chain, which this "
               "cache was not built for\n", cube_fb_n);
        return 0;
    }

    memset(&fb, 0, sizeof(fb));
    fb.width = paint_mode.hdisplay;
    fb.height = paint_mode.vdisplay;
    fb.pixel_format = FOURCC_ARGB8888;
    fb.handles[0] = h.u32;
    fb.pitches[0] = p_gbm_bo_get_stride(bo);
    if (drm_ioctl(DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        printf("cube: ADDFB2 on lima's buffer (handle %u, stride %u): %m -- lima "
               "rendered it but the display device will not scan it out\n",
               fb.handles[0], fb.pitches[0]);
        return 0;
    }

    cube_fbs[cube_fb_n].handle = h.u32;
    cube_fbs[cube_fb_n].fb = fb.fb_id;
    cube_fb_n++;
    printf("cube: swap-chain buffer %d: handle %u, stride %u, fb %u\n",
           cube_fb_n, h.u32, fb.pitches[0], fb.fb_id);
    return fb.fb_id;
}

/*
 * Flip and wait for the vblank event.  Returns -1 if the ioctl was refused (the
 * caller falls back to a modeset for the rest of the run), 1 if the flip completed,
 * 0 if it was accepted but no event arrived.
 *
 * poll() rather than a bare read(), because read() on the DRM fd blocks: a driver
 * that accepts a flip and never completes it would be indistinguishable from a
 * hang, and a hang on this board costs a power cycle to find out about.
 */
static int flip(uint32_t fb)
{
    static int quiet;
    struct drm_mode_crtc_page_flip f;
    struct pollfd pfd;
    char buf[256];
    ssize_t n;
    size_t off;

    memset(&f, 0, sizeof(f));
    f.crtc_id = paint_crtc;
    f.fb_id = fb;
    f.flags = DRM_MODE_PAGE_FLIP_EVENT;
    if (drm_ioctl(DRM_IOCTL_MODE_PAGE_FLIP, &f) < 0)
        return -1;

    pfd.fd = paint_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    /* 500 ms is a couple of dozen vblanks.  A flip that has not completed by then
     * is not late, it is lost. */
    if (poll(&pfd, 1, 500) <= 0) {
        if (!quiet++)
            printf("cube: a flip was accepted but no completion event came within "
                   "500 ms -- the frames are unpaced from here on\n");
        return 0;
    }

    n = read(paint_fd, buf, sizeof(buf));
    for (off = 0; n > 0 && off + sizeof(struct drm_event) <= (size_t)n; ) {
        struct drm_event e;
        memcpy(&e, buf + off, sizeof(e));
        if (e.length < sizeof(e) || off + e.length > (size_t)n)
            break;
        if (e.type == DRM_EVENT_FLIP_COMPLETE)
            return 1;
        off += e.length;
    }
    return 0;
}

/*
 * Any button stops it, so the cube is not a twenty-second wait when it is being
 * launched from the dashboard.  struct input_event on a 32-bit kernel with 32-bit
 * userspace is 16 bytes -- two longs of timeval, then u16 type, u16 code, s32
 * value -- and it is written out here for the same reason the DRM structs are.
 */
struct j36_input_event {
    long     sec, usec;
    uint16_t type, code;
    int32_t  value;
};

#define J36_EV_KEY 0x01

static int quit_fds[8];
static int quit_n;

static void quit_open(void)
{
    char path[32];
    int i, fd;

    for (i = 0; i < 32 && quit_n < (int)(sizeof(quit_fds) / sizeof(quit_fds[0])); i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            quit_fds[quit_n++] = fd;
    }
}

static int quit_pressed(void)
{
    struct j36_input_event ev;
    int i;

    for (i = 0; i < quit_n; i++)
        while (read(quit_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
            if (ev.type == J36_EV_KEY && ev.value == 1)
                return 1;
    return 0;
}

static void quit_close(void)
{
    while (quit_n)
        close(quit_fds[--quit_n]);
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int cube(const char *path, int seconds)
{
    EGLint attr[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 16,          /* attr[7]: zeroed below if nothing has depth */
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay dpy = NULL;
    EGLConfig pick[64], cfg;
    EGLContext ctx = NULL;
    EGLSurface surf = NULL;
    struct gbm_device *gbm = NULL;
    struct gbm_surface *gs = NULL;
    struct gbm_bo *held = NULL;
    const unsigned char *s;
    unsigned int prog = 0, vs, fs;
    EGLint count = 0, i, depth = 0, e;
    int a_pos, a_col, u_mvp;
    long frames = 0, marked = 0;
    double t0, t, mark;
    int paged = 1, quit = 0;
    float proj[16];

    printf("== %s, rotating cube for %ds\n", path, seconds);

    if (!p_eglSwapBuffers || !p_gbm_surface_lock_front_buffer ||
        !p_gbm_surface_release_buffer || !p_gbm_bo_get_handle ||
        !p_gbm_bo_get_stride) {
        printf("cube: this payload's libgbm is missing one of the four surface "
               "entry points the cube needs\n");
        return 1;
    }

    paint_fd = open(path, O_RDWR | O_CLOEXEC);
    if (paint_fd < 0) {
        printf("open: %m\n");
        return 1;
    }
    if (drm_ioctl(DRM_IOCTL_SET_MASTER, NULL) < 0)
        printf("cube: SET_MASTER: %m (a modeset may not be permitted)\n");
    if (!find_crtc())
        goto out;

    gbm = p_gbm_create_device(paint_fd);
    if (!gbm) {
        printf("cube: gbm_create_device on the display node failed\n");
        goto out;
    }

    if (p_eglGetPlatformDisplayEXT)
        dpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!dpy && p_eglGetPlatformDisplay)
        dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (!dpy)
        dpy = p_eglGetDisplay(gbm);
    if (!dpy || !p_eglInitialize(dpy, NULL, NULL)) {
        printf("cube: no EGL on this device\n");
        goto out_gbm;
    }

    eglclear();
    if (!p_eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("cube: eglBindAPI(ES): 0x%04x\n", p_eglGetError());
        goto out_dpy;
    }
    if (!p_eglChooseConfig(dpy, attr, pick, 64, &count) || count == 0) {
        /* A cube is convex, so back-face culling alone draws it correctly and a
         * depth buffer is a nicety rather than a requirement. */
        printf("cube: no ES2 window config carries a depth buffer; asking again "
               "without one and relying on back-face culling\n");
        attr[7] = 0;
        if (!p_eglChooseConfig(dpy, attr, pick, 64, &count) || count == 0) {
            printf("cube: no ES2 window config at all\n");
            goto out_dpy;
        }
    }

    /* The ARGB8888 visual, for the reason paint_gl picks it: it is the one format
     * this display device is known to scan out. */
    cfg = pick[0];
    for (i = 0; i < count; i++) {
        EGLint vis = 0;
        p_eglGetConfigAttrib(dpy, pick[i], EGL_NATIVE_VISUAL_ID, &vis);
        if ((uint32_t)vis == FOURCC_ARGB8888) {
            cfg = pick[i];
            break;
        }
    }
    p_eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &depth);

    gs = p_gbm_surface_create(gbm, paint_mode.hdisplay, paint_mode.vdisplay,
                              FOURCC_ARGB8888,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gs) {
        printf("cube: gbm_surface_create %ux%u AR24 SCANOUT|RENDERING failed -- "
               "kmsro cannot pair lima with this display device\n",
               paint_mode.hdisplay, paint_mode.vdisplay);
        goto out_dpy;
    }

    ctx = p_eglCreateContext(dpy, cfg, NULL, cattr);
    surf = ctx ? p_eglCreateWindowSurface(dpy, cfg, gs, NULL) : NULL;
    if (!ctx || !surf || !p_eglMakeCurrent(dpy, surf, surf, ctx)) {
        e = p_eglGetError();
        printf("cube: no current ES2 context on that surface: 0x%04x %s\n",
               e, eglerr(e));
        goto out_gs;
    }

    if (!load_gles())
        goto out_current;

    s = p_glGetString(GL_RENDERER);
    printf("cube: renderer \"%s\"\n", s ? (const char *)s : "NULL");
    s = p_glGetString(GL_VERSION);
    printf("cube: version \"%s\"\n", s ? (const char *)s : "NULL");
    s = p_glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("cube: glsl \"%s\", depth buffer %d bits\n",
           s ? (const char *)s : "NULL", depth);

    /*
     * The shaders.  This is the measurement -p cannot make: a driver that can
     * clear a buffer will pass every phase of -p, and a driver whose compiler is
     * broken fails here with a log that says why.
     */
    vs = compile_shader(GL_VERTEX_SHADER, CUBE_VS, "vertex");
    fs = vs ? compile_shader(GL_FRAGMENT_SHADER, CUBE_FS, "fragment") : 0;
    if (!vs || !fs)
        goto out_current;

    prog = p_glCreateProgram();
    if (prog) {
        int ok = 0, len = 0;
        char log[512];
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        p_glLinkProgram(prog);
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            log[0] = '\0';
            p_glGetProgramInfoLog(prog, (int)sizeof(log), &len, log);
            log[sizeof(log) - 1] = '\0';
            printf("cube: the program did not link: %s\n",
                   log[0] ? log : "(the driver returned no log)");
            prog = 0;
        }
    }
    if (!prog) {
        printf("cube: no program, so there is nothing to draw with\n");
        goto out_current;
    }

    a_pos = p_glGetAttribLocation(prog, "a_pos");
    a_col = p_glGetAttribLocation(prog, "a_col");
    u_mvp = p_glGetUniformLocation(prog, "u_mvp");
    if (a_pos < 0 || a_col < 0 || u_mvp < 0) {
        printf("cube: the linked program has no a_pos/a_col/u_mvp (%d/%d/%d)\n",
               a_pos, a_col, u_mvp);
        goto out_current;
    }

    build_cube();
    m_perspective(proj, 45.0f,
                  (float)paint_mode.hdisplay / (float)paint_mode.vdisplay,
                  0.1f, 20.0f);

    p_glViewport(0, 0, (int)paint_mode.hdisplay, (int)paint_mode.vdisplay);
    if (depth > 0) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(GL_LESS);
    }
    p_glEnable(GL_CULL_FACE);
    p_glUseProgram(prog);
    /*
     * Client-side vertex arrays, which GLES2 allows and GLES3 core does not.  A
     * VBO would be three more entry points for 864 bytes of cube that is uploaded
     * once, and the driver copies it either way.
     */
    p_glEnableVertexAttribArray((unsigned int)a_pos);
    p_glEnableVertexAttribArray((unsigned int)a_col);
    p_glVertexAttribPointer((unsigned int)a_pos, 3, GL_FLOAT, 0,
                            6 * (int)sizeof(float), cube_verts);
    p_glVertexAttribPointer((unsigned int)a_col, 3, GL_FLOAT, 0,
                            6 * (int)sizeof(float), cube_verts + 3);

    quit_open();
    printf("cube: %d input device%s open; any button stops it early\n",
           quit_n, quit_n == 1 ? "" : "s");

    t0 = now_s();
    mark = t0;
    for (;;) {
        float mvp[16], mv[16], rot[16], rx[16], ry[16], view[16];
        struct gbm_bo *bo;
        uint32_t fb;
        float ang;

        t = now_s();
        if (t - t0 >= (double)seconds || quit)
            break;

        /* One turn every seven seconds about Y, and a slower one about X so that
         * more than two faces are ever visible. */
        ang = (float)(t - t0) * 0.9f;
        m_rot_y(ry, ang);
        m_rot_x(rx, ang * 0.6f);
        m_mul(rot, ry, rx);
        m_translate(view, 0.0f, 0.0f, -4.5f);
        m_mul(mv, view, rot);
        m_mul(mvp, proj, mv);

        /*
         * MVII's desktop grey rather than black, on purpose: a frame that arrives
         * with nothing drawn in it is then distinguishable from a frame that never
         * arrived at all, which is exactly the ambiguity that made the ES black
         * screen so expensive to read.
         */
        p_glClearColor(0.102f, 0.110f, 0.149f, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        p_glUniformMatrix4fv(u_mvp, 1, 0, mvp);
        p_glDrawArrays(GL_TRIANGLES, 0, 36);

        if (frames == 0) {
            unsigned int ge = p_glGetError();
            if (ge != GL_NO_ERROR)
                printf("cube: gl error 0x%04x after the first draw\n", ge);
        }

        if (!p_eglSwapBuffers(dpy, surf)) {
            e = p_eglGetError();
            printf("cube: eglSwapBuffers: 0x%04x %s\n", e, eglerr(e));
            break;
        }
        bo = p_gbm_surface_lock_front_buffer(gs);
        if (!bo) {
            printf("cube: no front buffer after a swap\n");
            break;
        }
        fb = fb_for(bo);
        if (!fb) {
            p_gbm_surface_release_buffer(gs, bo);
            break;
        }

        if (frames == 0) {
            /* The first frame needs the mode as well as the buffer. */
            if (setcrtc(fb) < 0) {
                printf("cube: SETCRTC: %m\n");
                p_gbm_surface_release_buffer(gs, bo);
                break;
            }
            printf("cube: the first frame is on crtc %u\n", paint_crtc);
        } else if (paged) {
            if (flip(fb) < 0) {
                printf("cube: PAGE_FLIP: %m -- falling back to a modeset per "
                       "frame, which will be slow but will still turn\n");
                paged = 0;
                if (setcrtc(fb) < 0) {
                    p_gbm_surface_release_buffer(gs, bo);
                    break;
                }
            }
        } else if (setcrtc(fb) < 0) {
            printf("cube: SETCRTC: %m\n");
            p_gbm_surface_release_buffer(gs, bo);
            break;
        }

        /* Only now is the previous frame off the CRTC and safe to give back. */
        if (held)
            p_gbm_surface_release_buffer(gs, held);
        held = bo;
        frames++;

        if (t - mark >= 1.0) {
            printf("cube: %.0fs, %ld frames, %.1f fps\n", t - t0, frames,
                   (double)(frames - marked) / (t - mark));
            mark = t;
            marked = frames;
        }
        if (quit_pressed()) {
            printf("cube: a button was pressed\n");
            quit = 1;
        }
    }

    t = now_s();
    printf("cube: %ld frames in %.1fs, %.1f fps, %s\n", frames, t - t0,
           t > t0 ? (double)frames / (t - t0) : 0.0,
           paged ? "page-flipped" : "one modeset per frame");
    if (frames == 0)
        printf("cube: nothing was drawn, so the messages above are the finding\n");
    else
        printf("cube: if the panel showed a turning cube then lima compiles, "
               "rasterises and scans out, and the GLES driver is not what is "
               "wrong with this board\n");

    quit_close();
    if (held)
        p_gbm_surface_release_buffer(gs, held);

out_current:
    for (i = 0; i < cube_fb_n; i++)
        drm_ioctl(DRM_IOCTL_MODE_RMFB, &cube_fbs[i].fb);
    cube_fb_n = 0;
    p_eglMakeCurrent(dpy, NULL, NULL, NULL);
    if (surf)
        p_eglDestroySurface(dpy, surf);
    if (ctx)
        p_eglDestroyContext(dpy, ctx);
out_gs:
    p_gbm_surface_destroy(gs);
out_dpy:
    p_eglTerminate(dpy);
out_gbm:
    p_gbm_device_destroy(gbm);
out:
    drm_ioctl(DRM_IOCTL_DROP_MASTER, NULL);
    close(paint_fd);
    paint_fd = -1;
    return frames > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    int wins = 0, painted = -1, cubed = -1, listed = -1, i;
    const char *node;
    int fb_secs = 3;

    setvbuf(stdout, NULL, _IOLBF, 0);

    /*
     * -f is handled here, before load(), and it runs alone.  It is the one mode
     * that needs no library at all -- no EGL, no gbm, no DRM -- and the boot where
     * the panel is black is exactly the boot where the GL payload may be the thing
     * that is missing.  A probe that cannot say "the framebuffer works, nothing
     * drew into it" because libEGL.so.1 was absent would be useless precisely when
     * it is needed.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f"))
            continue;
        if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
            fb_secs = atoi(argv[i + 1]);
        return fb_report(1, 1, fb_secs) ? 1 : 0;
    }

    /* And read-only on the default run, first of all, because it is two ioctls and
     * it says whether the panel is even in the question. */
    if (argc == 1)
        fb_report(0, 0, 0);

    if (load())
        return 1;

    if (argc > 1) {
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-s")) {
                wins += probe_surfaceless();
            } else if (!strcmp(argv[i], "-i")) {
                listed = display_node() ? 0 : 1;
            } else if (!strcmp(argv[i], "-p")) {
                /* No node that modesets is not a reason to fall back to card0 -- that
                 * is precisely the assumption that made -p meaningless before. */
                node = display_node();
                painted = node ? paint(node) : 1;
            } else if (!strcmp(argv[i], "-c")) {
                /* An optional number of seconds after it, so the dashboard can ask
                 * for a short one and a bring-up session for a long one. */
                int secs = CUBE_SECONDS;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                    secs = atoi(argv[++i]);
                node = display_node();
                cubed = node ? cube(node, secs > 0 ? secs : CUBE_SECONDS) : 1;
            } else {
                wins += probe(argv[i], strstr(argv[i], "/card") != NULL);
            }
        }
    } else {
        /* The display node first, because it is the one SDL will actually be handed
         * -- if there is one, which is the first thing worth knowing. */
        node = display_node();
        if (node)
            wins += probe(node, 1);
        wins += probe("/dev/dri/renderD128", 0);
    }

    if (painted >= 0 || cubed >= 0 || listed >= 0)
        return (painted > 0 || cubed > 0 || listed > 0) ? 1 : 0;

    printf("eglprobe: %s\n", wins ? "a context came up, see which API above"
                                  : "no API created a context on any node");
    return wins ? 0 : 1;
}
