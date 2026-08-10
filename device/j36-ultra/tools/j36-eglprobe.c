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
 * It probes the display node and the render node separately, because on this SoC
 * they are different chips: /dev/dri/card0 is mtk_drm (display, no GPU) and
 * /dev/dri/renderD128 is lima (GPU, no scanout), and Mesa bridges them with
 * kmsro.  If contexts come up on the render node but not on the display node,
 * the kmsro pairing is what is broken; if they fail on both, lima's context
 * creation is.  That distinction is the whole reason this file exists and it is
 * not observable from ES's abort.
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
 * Exit status: 0 if some API created a context on some node, 1 otherwise.
 * Nothing is written anywhere; stdout is the whole output.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

#define GL_VERSION                 0x1F02
#define GL_EXTENSIONS              0x1F03

/* drm_fourcc.h: 'AR24' and 'XR24'. */
#define FOURCC_ARGB8888            0x34325241u
#define FOURCC_XRGB8888            0x34325258u

#define GBM_BO_USE_SCANOUT         (1u << 0)
#define GBM_BO_USE_RENDERING       (1u << 2)

struct gbm_device;
struct gbm_surface;

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

static struct gbm_device *(*p_gbm_create_device)(int);
static void (*p_gbm_device_destroy)(struct gbm_device *);
static struct gbm_surface *(*p_gbm_surface_create)(struct gbm_device *, uint32_t,
                                                   uint32_t, uint32_t, uint32_t);
static void (*p_gbm_surface_destroy)(struct gbm_surface *);

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

/* Returns the number of APIs that produced a context on this device. */
static int probe(const char *path, int scanout)
{
    EGLDisplay dpy = NULL;
    EGLConfig cfgs[256];
    EGLint major = 0, minor = 0, got = 0, e;
    const char *how = "getdisplay";
    struct gbm_device *gbm;
    int fd, wins = 0;
    size_t a;

    line[0] = '\0';
    line_n = 0;

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

    for (a = 0; a < sizeof(APIS) / sizeof(APIS[0]); a++) {
        const struct api *api = &APIS[a];
        EGLint attr[] = {
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_DEPTH_SIZE,      24,
            EGL_RENDERABLE_TYPE, api->renderable,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_NONE
        };
        EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, api->client_version, EGL_NONE };
        EGLConfig pick[64], cfg;
        EGLint count = 0, i;
        EGLContext ctx;
        struct gbm_surface *gs;
        EGLSurface surf;

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
        gs = p_gbm_surface_create(gbm, 640, 480, FOURCC_ARGB8888,
                                  scanout ? (GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)
                                          : GBM_BO_USE_RENDERING);
        surf = gs ? p_eglCreateWindowSurface(dpy, cfg, gs, NULL) : NULL;
        if (!gs) {
            appendf("/nogbm");
        } else if (!surf) {
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
    printf("ctx:%s\n", line);

    p_eglTerminate(dpy);
out_gbm:
    p_gbm_device_destroy(gbm);
    close(fd);
    return wins;
}

int main(int argc, char **argv)
{
    int wins = 0, i;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (load())
        return 1;

    if (argc > 1) {
        for (i = 1; i < argc; i++)
            wins += probe(argv[i], strstr(argv[i], "/card") != NULL);
    } else {
        /* Display node first: it is the one SDL will actually be handed. */
        wins += probe("/dev/dri/card0", 1);
        wins += probe("/dev/dri/renderD128", 0);
    }

    printf("eglprobe: %s\n", wins ? "a context came up, see which API above"
                                  : "no API created a context on any node");
    return wins ? 0 : 1;
}
