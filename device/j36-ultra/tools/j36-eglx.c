/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/* Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later. */
/*
 * j36-eglx -- lima EGL backend for X11 clients.
 *
 * WHY THIS EXISTS.  Mesa's EGL X11 platform talks DRI2/DRI3 to a KMS
 * display.  On this board the panel is simplefb and card0 is lima with
 * no CRTC, so eglGetDisplay(XOpenDisplay()) fails:
 * "glxtest: libEGL initialize failed" / "GLX extension missing".
 *
 * GBM on the lima fd already works -- eglprobe -o and j36lima's own
 * import context use it.  This library is the missing platform: it
 * turns an X11 (or default) EGL display into that GBM lima display,
 * presents window surfaces with XPutImage, and creates the context
 * the client asked for.
 *
 * APIS, WHEN REQUESTED.
 *
 *   GLES2     always, that is lima's native API.
 *   GLES 3.2  MESA_GLES_VERSION_OVERRIDE=3.2 around CreateContext.
 *   GL 4.6    compatibility profile, MESA_GL_VERSION_OVERRIDE=4.6COMPAT
 *             and GLSL 460 -- the highest compat profile Mesa exposes.
 *             Clients that never ask stay on GLES2.
 *
 * Loaded with LD_PRELOAD so every X client -- Firefox glxtest, SDL
 * x11+EGL, a hand-typed eglinfo -- hits these entry points without a
 * GLVND vendor dance.  Mesa's real EGL is RTLD_NEXT.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <dirent.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <drm/drm.h>
#include <gbm.h>
#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR 0x0010
#endif

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef void *EGLImage;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t EGLint;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_DEFAULT_DISPLAY		((EGLNativeDisplayType)0)
#define EGL_NO_DISPLAY			((EGLDisplay)0)
#define EGL_NO_CONTEXT			((EGLContext)0)
#define EGL_NO_SURFACE			((EGLSurface)0)
#define EGL_NO_IMAGE			((EGLImage)0)
#define EGL_FALSE			0
#define EGL_TRUE			1
#define EGL_SUCCESS			0x3000
#define EGL_NOT_INITIALIZED		0x3001
#define EGL_BAD_ACCESS			0x3002
#define EGL_BAD_ALLOC			0x3003
#define EGL_BAD_ATTRIBUTE		0x3004
#define EGL_BAD_CONFIG			0x3005
#define EGL_BAD_CONTEXT			0x3006
#define EGL_BAD_CURRENT_SURFACE		0x3007
#define EGL_BAD_DISPLAY			0x3008
#define EGL_BAD_MATCH			0x3009
#define EGL_BAD_NATIVE_PIXMAP		0x300A
#define EGL_BAD_NATIVE_WINDOW		0x300B
#define EGL_BAD_PARAMETER		0x300C
#define EGL_BAD_SURFACE			0x300D
#define EGL_NONE			0x3038
#define EGL_VENDOR			0x3053
#define EGL_VERSION			0x3054
#define EGL_EXTENSIONS			0x3055
#define EGL_CLIENT_APIS			0x308D
#define EGL_ALPHA_SIZE			0x3021
#define EGL_BLUE_SIZE			0x3022
#define EGL_GREEN_SIZE			0x3023
#define EGL_RED_SIZE			0x3024
#define EGL_DEPTH_SIZE			0x3025
#define EGL_STENCIL_SIZE		0x3026
#define EGL_SURFACE_TYPE		0x3033
#define EGL_RENDERABLE_TYPE		0x3040
#define EGL_WINDOW_BIT			0x0004
#define EGL_PBUFFER_BIT			0x0001
#define EGL_OPENGL_ES_BIT		0x0001
#define EGL_OPENGL_ES2_BIT		0x0004
#define EGL_OPENGL_BIT			0x0008
#define EGL_OPENGL_ES3_BIT		0x00000040
#define EGL_OPENGL_ES_API		0x30A0
#define EGL_OPENGL_API			0x30A2
#define EGL_CONTEXT_CLIENT_VERSION	0x3098
#define EGL_CONTEXT_MAJOR_VERSION	0x3098
#define EGL_CONTEXT_MINOR_VERSION	0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK	0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT		0x00000001
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT	0x00000002
#define EGL_PLATFORM_X11_EXT		0x31D5
#define EGL_PLATFORM_X11_KHR		0x31D5
#define EGL_PLATFORM_GBM_KHR		0x31D7
#define EGL_PLATFORM_SURFACELESS_MESA	0x31DD
#define EGL_WIDTH			0x3057
#define EGL_HEIGHT			0x3056
#define EGL_GL_RENDERER			0x1F01
#define EGL_GL_VERSION			0x1F02

#define MAX_DPY		8
#define MAX_WIN		16

struct Real {
	EGLDisplay (*GetDisplay)(EGLNativeDisplayType);
	EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const EGLint *);
	EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
	EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
	EGLBoolean (*Terminate)(EGLDisplay);
	const char *(*QueryString)(EGLDisplay, EGLint);
	EGLBoolean (*GetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
	EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
				   EGLint, EGLint *);
	EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
	EGLBoolean (*BindAPI)(EGLenum);
	EGLenum (*QueryAPI)(void);
	EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext,
				    const EGLint *);
	EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
	EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig,
					  EGLNativeWindowType, const EGLint *);
	EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
	EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLDisplay (*GetCurrentDisplay)(void);
	EGLContext (*GetCurrentContext)(void);
	EGLSurface (*GetCurrentSurface)(EGLint);
	EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
	EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
	EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
	EGLint (*GetError)(void);
	__eglMustCastToProperFunctionPointerType (*GetProcAddress)(const char *);
	EGLBoolean (*WaitClient)(void);
	EGLBoolean (*WaitGL)(void);
	EGLBoolean (*WaitNative)(EGLint);
	EGLImage (*CreateImageKHR)(EGLDisplay, EGLContext, EGLenum, void *,
				   const EGLint *);
	EGLBoolean (*DestroyImageKHR)(EGLDisplay, EGLImage);
	const unsigned char *(*glGetString)(unsigned int);
};

struct Dpy {
	int used;
	EGLDisplay public;	/* what we hand the client */
	EGLDisplay mesa;	/* GBM lima display */
	struct gbm_device *gbm;
	int dri_fd;
	Display *xdpy;
	char path[280];
	int inited;
};

struct Win {
	int used;
	EGLDisplay public;
	EGLSurface mesa;
	struct gbm_surface *gs;
	Display *xdpy;
	Window xw;
	int w, h;
};

static struct Real R;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct Dpy dpys[MAX_DPY];
static struct Win wins[MAX_WIN];
static EGLint tls_err = EGL_SUCCESS;
static char vendor_str[] = "MixOS j36-eglx (lima GBM)";
static char version_str[] = "1.5";
static char client_apis[] = "OpenGL_ES OpenGL";
static char ext_str[] =
	"EGL_KHR_platform_x11 EGL_EXT_platform_x11 EGL_KHR_platform_gbm "
	"EGL_MESA_platform_gbm EGL_KHR_platform_surfaceless "
	"EGL_KHR_create_context EGL_KHR_surfaceless_context "
	"EGL_EXT_image_dma_buf_import EGL_KHR_image_base";

static void set_err(EGLint e) { tls_err = e; }

static void *load_sym(void *h, const char *name)
{
	void *s = h ? dlsym(h, name) : NULL;
	if (!s)
		s = dlsym(RTLD_NEXT, name);
	return s;
}

static void load_real(void)
{
	static int once;
	void *mesa;

	if (once)
		return;
	once = 1;
	/* Mesa's vendor, never libEGL.so.1 -- that is glvnd and would recurse. */
	mesa = dlopen("libEGL_mesa.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!mesa)
		mesa = RTLD_NEXT;
#define S(field, name) R.field = (__typeof__(R.field))load_sym(mesa, name)
	S(GetDisplay, "eglGetDisplay");
	S(GetPlatformDisplay, "eglGetPlatformDisplay");
	S(GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
	S(Initialize, "eglInitialize");
	S(Terminate, "eglTerminate");
	S(QueryString, "eglQueryString");
	S(GetConfigs, "eglGetConfigs");
	S(ChooseConfig, "eglChooseConfig");
	S(GetConfigAttrib, "eglGetConfigAttrib");
	S(BindAPI, "eglBindAPI");
	S(QueryAPI, "eglQueryAPI");
	S(CreateContext, "eglCreateContext");
	S(DestroyContext, "eglDestroyContext");
	S(CreateWindowSurface, "eglCreateWindowSurface");
	S(CreatePbufferSurface, "eglCreatePbufferSurface");
	S(DestroySurface, "eglDestroySurface");
	S(MakeCurrent, "eglMakeCurrent");
	S(GetCurrentDisplay, "eglGetCurrentDisplay");
	S(GetCurrentContext, "eglGetCurrentContext");
	S(GetCurrentSurface, "eglGetCurrentSurface");
	S(SwapBuffers, "eglSwapBuffers");
	S(SwapInterval, "eglSwapInterval");
	S(QuerySurface, "eglQuerySurface");
	S(GetError, "eglGetError");
	S(GetProcAddress, "eglGetProcAddress");
	S(WaitClient, "eglWaitClient");
	S(WaitGL, "eglWaitGL");
	S(WaitNative, "eglWaitNative");
	S(CreateImageKHR, "eglCreateImageKHR");
	S(DestroyImageKHR, "eglDestroyImageKHR");
	S(glGetString, "glGetString");
#undef S
	if (!R.CreateImageKHR && R.GetProcAddress)
		R.CreateImageKHR = (__typeof__(R.CreateImageKHR))
			R.GetProcAddress("eglCreateImageKHR");
	if (!R.DestroyImageKHR && R.GetProcAddress)
		R.DestroyImageKHR = (__typeof__(R.DestroyImageKHR))
			R.GetProcAddress("eglDestroyImageKHR");
}

static int dri_is_lima(int fd)
{
	struct drm_version v;
	char name[32];

	memset(&v, 0, sizeof v);
	memset(name, 0, sizeof name);
	v.name = name;
	v.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
		return 0;
	return strcmp(name, "lima") == 0;
}

static int dri_has_modeset(int fd)
{
	struct drm_mode_card_res res;

	memset(&res, 0, sizeof res);
	return ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0;
}

static int open_lima(char *path, size_t path_sz)
{
	static const char *const first[] = {
		"/dev/dri/card0", "/dev/dri/renderD128", NULL
	};
	DIR *d;
	struct dirent *e;
	int i, fd;

	for (i = 0; first[i]; i++) {
		fd = open(first[i], O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (dri_is_lima(fd) && !dri_has_modeset(fd)) {
			snprintf(path, path_sz, "%s", first[i]);
			return fd;
		}
		close(fd);
	}
	d = opendir("/dev/dri");
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[280];

		if (strncmp(e->d_name, "card", 4) &&
		    strncmp(e->d_name, "renderD", 7))
			continue;
		snprintf(p, sizeof p, "/dev/dri/%s", e->d_name);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (dri_is_lima(fd) && !dri_has_modeset(fd)) {
			snprintf(path, path_sz, "%s", p);
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	return -1;
}

static struct Dpy *dpy_find(EGLDisplay pub)
{
	int i;

	if (!pub)
		return NULL;
	for (i = 0; i < MAX_DPY; i++)
		if (dpys[i].used && dpys[i].public == pub)
			return &dpys[i];
	return NULL;
}

static EGLDisplay dpy_mesa(EGLDisplay pub)
{
	struct Dpy *d = dpy_find(pub);
	return d ? d->mesa : pub;
}

static struct Dpy *open_lima_display(Display *xdpy)
{
	struct Dpy *d = NULL;
	int i, fd;
	char path[280];
	EGLDisplay mesa = EGL_NO_DISPLAY;

	load_real();
	for (i = 0; i < MAX_DPY; i++) {
		if (dpys[i].used && dpys[i].xdpy == xdpy && dpys[i].mesa)
			return &dpys[i];
	}
	for (i = 0; i < MAX_DPY; i++) {
		if (!dpys[i].used) {
			d = &dpys[i];
			break;
		}
	}
	if (!d)
		return NULL;

	fd = open_lima(path, sizeof path);
	if (fd < 0)
		return NULL;
	memset(d, 0, sizeof *d);
	d->dri_fd = fd;
	d->gbm = gbm_create_device(fd);
	snprintf(d->path, sizeof d->path, "%s", path);
	d->xdpy = xdpy;

	if (d->gbm && R.GetPlatformDisplay)
		mesa = R.GetPlatformDisplay(EGL_PLATFORM_GBM_KHR, d->gbm, NULL);
	if ((!mesa || mesa == EGL_NO_DISPLAY) && d->gbm && R.GetPlatformDisplayEXT)
		mesa = R.GetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, d->gbm, NULL);
	if ((!mesa || mesa == EGL_NO_DISPLAY) && d->gbm && R.GetDisplay)
		mesa = R.GetDisplay(d->gbm);
	if ((!mesa || mesa == EGL_NO_DISPLAY) && R.GetPlatformDisplayEXT)
		mesa = R.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
					       NULL, NULL);
	if (!mesa || mesa == EGL_NO_DISPLAY) {
		if (d->gbm)
			gbm_device_destroy(d->gbm);
		close(fd);
		return NULL;
	}
	d->mesa = mesa;
	d->public = (EGLDisplay)d;
	d->used = 1;
	return d;
}

static struct Win *win_find(EGLSurface s)
{
	int i;

	for (i = 0; i < MAX_WIN; i++)
		if (wins[i].used && wins[i].mesa == s)
			return &wins[i];
	return NULL;
}

static void env_set(const char *k, const char *v, char *old, size_t old_sz)
{
	const char *cur = getenv(k);

	if (old && old_sz) {
		if (cur)
			snprintf(old, old_sz, "%s", cur);
		else
			old[0] = '\0';
	}
	if (v)
		setenv(k, v, 1);
	else
		unsetenv(k);
}

static void env_restore(const char *k, const char *old)
{
	if (old && old[0])
		setenv(k, old, 1);
	else
		unsetenv(k);
}

/* ── exported EGL ───────────────────────────────────────────────────────── */

EGLDisplay eglGetDisplay(EGLNativeDisplayType native)
{
	struct Dpy *d;

	load_real();
	pthread_mutex_lock(&lock);
	/* DEFAULT or an X Display *: both become GBM lima.  A GBM device
	 * pointer is passed through so eglprobe / the DDX keep working. */
	if (native == EGL_DEFAULT_DISPLAY) {
		d = open_lima_display(NULL);
		pthread_mutex_unlock(&lock);
		if (!d) {
			set_err(EGL_NOT_INITIALIZED);
			return EGL_NO_DISPLAY;
		}
		set_err(EGL_SUCCESS);
		return d->public;
	}
	d = open_lima_display((Display *)native);
	pthread_mutex_unlock(&lock);
	if (d) {
		set_err(EGL_SUCCESS);
		return d->public;
	}
	if (R.GetDisplay)
		return R.GetDisplay(native);
	set_err(EGL_NOT_INITIALIZED);
	return EGL_NO_DISPLAY;
}

EGLDisplay eglGetPlatformDisplay(EGLenum plat, void *native, const EGLint *attr)
{
	struct Dpy *d;

	load_real();
	if (plat == EGL_PLATFORM_X11_KHR || plat == EGL_PLATFORM_GBM_KHR ||
	    native == NULL) {
		pthread_mutex_lock(&lock);
		d = open_lima_display(plat == EGL_PLATFORM_X11_KHR
					  ? (Display *)native : NULL);
		pthread_mutex_unlock(&lock);
		if (d) {
			set_err(EGL_SUCCESS);
			return d->public;
		}
	}
	if (R.GetPlatformDisplay)
		return R.GetPlatformDisplay(plat, native, attr);
	if (R.GetPlatformDisplayEXT)
		return R.GetPlatformDisplayEXT(plat, native, attr);
	set_err(EGL_BAD_PARAMETER);
	return EGL_NO_DISPLAY;
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum plat, void *native, const EGLint *attr)
{
	return eglGetPlatformDisplay(plat, native, attr);
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *maj, EGLint *min)
{
	struct Dpy *d;
	EGLBoolean ok;

	load_real();
	pthread_mutex_lock(&lock);
	d = dpy_find(dpy);
	pthread_mutex_unlock(&lock);
	if (!R.Initialize) {
		set_err(EGL_NOT_INITIALIZED);
		return EGL_FALSE;
	}
	ok = R.Initialize(d ? d->mesa : dpy, maj, min);
	if (ok && d)
		d->inited = 1;
	if (ok && maj && *maj == 0)
		*maj = 1;
	if (ok && min && *min == 0)
		*min = 5;
	return ok;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
	struct Dpy *d;

	load_real();
	if (!R.Terminate)
		return EGL_TRUE;
	pthread_mutex_lock(&lock);
	d = dpy_find(dpy);
	pthread_mutex_unlock(&lock);
	return R.Terminate(d ? d->mesa : dpy);
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
	load_real();
	if (name == EGL_VENDOR)
		return vendor_str;
	if (name == EGL_VERSION)
		return version_str;
	if (name == EGL_CLIENT_APIS)
		return client_apis;
	if (name == EGL_EXTENSIONS)
		return ext_str;
	if (R.QueryString)
		return R.QueryString(dpy_mesa(dpy), name);
	return NULL;
}

EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig *cfgs, EGLint sz, EGLint *n)
{
	load_real();
	if (!R.GetConfigs) {
		set_err(EGL_NOT_INITIALIZED);
		return EGL_FALSE;
	}
	return R.GetConfigs(dpy_mesa(dpy), cfgs, sz, n);
}

/*
 * lima only lists GLES2 configs.  A client that asks for GLES3 or
 * desktop GL would get nocfg and never reach CreateContext.  Drop those
 * bits so ChooseConfig succeeds; CreateContext then builds the version
 * that was actually requested.
 */
EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attr,
			   EGLConfig *cfgs, EGLint sz, EGLint *n)
{
	EGLint local[64];
	int i = 0, j = 0;
	EGLint want = 0;

	load_real();
	if (!R.ChooseConfig) {
		set_err(EGL_NOT_INITIALIZED);
		return EGL_FALSE;
	}
	if (attr) {
		for (i = 0; attr[i] != EGL_NONE && j < 60; i++) {
			if (attr[i] == EGL_RENDERABLE_TYPE && attr[i + 1] != EGL_NONE) {
				want = attr[i + 1];
				local[j++] = EGL_RENDERABLE_TYPE;
				/* Keep ES2 so lima matches; remember the rest. */
				local[j++] = EGL_OPENGL_ES2_BIT;
				i++;
				continue;
			}
			local[j++] = attr[i];
		}
	}
	local[j] = EGL_NONE;
	(void)want;
	if (!R.ChooseConfig(dpy_mesa(dpy), attr ? local : attr, cfgs, sz, n) ||
	    (n && *n == 0)) {
		/* Last resort: any config on this display. */
		return R.ChooseConfig(dpy_mesa(dpy), NULL, cfgs, sz, n);
	}
	return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig cfg, EGLint a, EGLint *v)
{
	load_real();
	if (!R.GetConfigAttrib)
		return EGL_FALSE;
	if (a == EGL_RENDERABLE_TYPE && v) {
		if (!R.GetConfigAttrib(dpy_mesa(dpy), cfg, a, v))
			return EGL_FALSE;
		/* Advertise what we will actually create when asked. */
		*v |= EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT;
		return EGL_TRUE;
	}
	return R.GetConfigAttrib(dpy_mesa(dpy), cfg, a, v);
}

EGLBoolean eglBindAPI(EGLenum api)
{
	load_real();
	if (!R.BindAPI)
		return EGL_FALSE;
	return R.BindAPI(api);
}

EGLenum eglQueryAPI(void)
{
	load_real();
	if (!R.QueryAPI)
		return EGL_OPENGL_ES_API;
	return R.QueryAPI();
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
			    const EGLint *attr)
{
	EGLint major = 2, minor = 0, profile = 0, have_major = 0;
	EGLenum api;
	EGLContext ctx = EGL_NO_CONTEXT;
	char old_gl[64], old_glsl[64], old_gles[64];
	EGLint es32[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
			  EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE };
	EGLint gl46[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4,
		EGL_CONTEXT_MINOR_VERSION, 6,
		EGL_CONTEXT_OPENGL_PROFILE_MASK,
		EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		EGL_NONE
	};
	EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int i;

	load_real();
	if (!R.CreateContext || !R.BindAPI) {
		set_err(EGL_NOT_INITIALIZED);
		return EGL_NO_CONTEXT;
	}
	api = R.QueryAPI ? R.QueryAPI() : EGL_OPENGL_ES_API;
	if (attr) {
		for (i = 0; attr[i] != EGL_NONE; i++) {
			if (attr[i] == EGL_CONTEXT_MAJOR_VERSION) {
				major = attr[++i];
				have_major = 1;
			} else if (attr[i] == EGL_CONTEXT_MINOR_VERSION) {
				minor = attr[++i];
			} else if (attr[i] == EGL_CONTEXT_OPENGL_PROFILE_MASK) {
				profile = attr[++i];
			} else {
				i++;
			}
		}
	}

	env_set("MESA_GL_VERSION_OVERRIDE", NULL, old_gl, sizeof old_gl);
	env_set("MESA_GLSL_VERSION_OVERRIDE", NULL, old_glsl, sizeof old_glsl);
	env_set("MESA_GLES_VERSION_OVERRIDE", NULL, old_gles, sizeof old_gles);

	if (api == EGL_OPENGL_API ||
	    (have_major && major >= 3 && api != EGL_OPENGL_ES_API) ||
	    (profile & EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT) ||
	    (have_major && major == 4)) {
		/* Desktop GL 4.6 compatibility -- the highest compat profile. */
		R.BindAPI(EGL_OPENGL_API);
		setenv("MESA_GL_VERSION_OVERRIDE", "4.6COMPAT", 1);
		setenv("MESA_GLSL_VERSION_OVERRIDE", "460", 1);
		unsetenv("MESA_GLES_VERSION_OVERRIDE");
		ctx = R.CreateContext(dpy_mesa(dpy), cfg, share, gl46);
		if (!ctx || ctx == EGL_NO_CONTEXT)
			ctx = R.CreateContext(dpy_mesa(dpy), cfg, share, attr);
	} else if (api == EGL_OPENGL_ES_API && (major >= 3 || !have_major)) {
		/* GLES 3.2 when asked, or when the client left the version
		 * at the EGL default and we can offer more than ES2. */
		R.BindAPI(EGL_OPENGL_ES_API);
		setenv("MESA_GLES_VERSION_OVERRIDE", "3.2", 1);
		unsetenv("MESA_GL_VERSION_OVERRIDE");
		ctx = R.CreateContext(dpy_mesa(dpy), cfg, share,
				      (have_major && major >= 3) ? attr : es32);
		if ((!ctx || ctx == EGL_NO_CONTEXT) && major >= 3)
			ctx = R.CreateContext(dpy_mesa(dpy), cfg, share, es32);
	}

	if (!ctx || ctx == EGL_NO_CONTEXT) {
		/* Honest GLES2 -- lima's native API. */
		R.BindAPI(EGL_OPENGL_ES_API);
		env_restore("MESA_GL_VERSION_OVERRIDE", old_gl);
		env_restore("MESA_GLSL_VERSION_OVERRIDE", old_glsl);
		env_restore("MESA_GLES_VERSION_OVERRIDE", old_gles);
		ctx = R.CreateContext(dpy_mesa(dpy), cfg, share,
				      attr ? attr : es2);
		if (!ctx || ctx == EGL_NO_CONTEXT)
			ctx = R.CreateContext(dpy_mesa(dpy), cfg, share, es2);
	}

	env_restore("MESA_GL_VERSION_OVERRIDE", old_gl);
	env_restore("MESA_GLSL_VERSION_OVERRIDE", old_glsl);
	env_restore("MESA_GLES_VERSION_OVERRIDE", old_gles);
	(void)minor;
	return ctx;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
	load_real();
	if (!R.DestroyContext)
		return EGL_TRUE;
	return R.DestroyContext(dpy_mesa(dpy), ctx);
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
				  EGLNativeWindowType native, const EGLint *attr)
{
	struct Dpy *d;
	struct Win *w = NULL;
	Display *xdpy;
	Window xw = (Window)(uintptr_t)native;
	Window root;
	int x, y, i;
	unsigned bw, depth, uw, uh;
	uint32_t fmt = 0x34325258; /* GBM_FORMAT_XRGB8888 */
	EGLSurface surf;

	load_real();
	pthread_mutex_lock(&lock);
	d = dpy_find(dpy);
	pthread_mutex_unlock(&lock);

	if (!d || !d->gbm) {
		if (R.CreateWindowSurface)
			return R.CreateWindowSurface(dpy_mesa(dpy), cfg, native, attr);
		set_err(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}
	xdpy = d->xdpy;
	if (!xdpy)
		xdpy = XOpenDisplay(NULL);
	if (!xdpy) {
		set_err(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}
	d->xdpy = xdpy;
	if (!XGetGeometry(xdpy, xw, &root, &x, &y, &uw, &uh, &bw, &depth) ||
	    uw == 0 || uh == 0) {
		set_err(EGL_BAD_NATIVE_WINDOW);
		return EGL_NO_SURFACE;
	}

	pthread_mutex_lock(&lock);
	for (i = 0; i < MAX_WIN; i++) {
		if (!wins[i].used) {
			w = &wins[i];
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	if (!w) {
		set_err(EGL_BAD_ALLOC);
		return EGL_NO_SURFACE;
	}

	w->gs = gbm_surface_create(d->gbm, uw, uh, fmt,
				   GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!w->gs) {
		set_err(EGL_BAD_ALLOC);
		return EGL_NO_SURFACE;
	}
	surf = R.CreateWindowSurface(d->mesa, cfg, (EGLNativeWindowType)w->gs, attr);
	if (!surf || surf == EGL_NO_SURFACE) {
		gbm_surface_destroy(w->gs);
		w->gs = NULL;
		set_err(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}
	w->used = 1;
	w->public = d->public;
	w->mesa = surf;
	w->xdpy = xdpy;
	w->xw = xw;
	w->w = (int)uw;
	w->h = (int)uh;
	return surf;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig cfg, const EGLint *attr)
{
	load_real();
	if (!R.CreatePbufferSurface) {
		set_err(EGL_BAD_MATCH);
		return EGL_NO_SURFACE;
	}
	return R.CreatePbufferSurface(dpy_mesa(dpy), cfg, attr);
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surf)
{
	struct Win *w;

	load_real();
	pthread_mutex_lock(&lock);
	w = win_find(surf);
	if (w) {
		if (R.DestroySurface)
			R.DestroySurface(dpy_mesa(dpy), surf);
		if (w->gs)
			gbm_surface_destroy(w->gs);
		memset(w, 0, sizeof *w);
		pthread_mutex_unlock(&lock);
		return EGL_TRUE;
	}
	pthread_mutex_unlock(&lock);
	if (R.DestroySurface)
		return R.DestroySurface(dpy_mesa(dpy), surf);
	return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
			  EGLContext ctx)
{
	load_real();
	if (!R.MakeCurrent)
		return EGL_FALSE;
	return R.MakeCurrent(dpy_mesa(dpy), draw, read, ctx);
}

EGLDisplay eglGetCurrentDisplay(void)
{
	EGLDisplay m;
	int i;

	load_real();
	if (!R.GetCurrentDisplay)
		return EGL_NO_DISPLAY;
	m = R.GetCurrentDisplay();
	for (i = 0; i < MAX_DPY; i++)
		if (dpys[i].used && dpys[i].mesa == m)
			return dpys[i].public;
	return m;
}

EGLContext eglGetCurrentContext(void)
{
	load_real();
	return R.GetCurrentContext ? R.GetCurrentContext() : EGL_NO_CONTEXT;
}

EGLSurface eglGetCurrentSurface(EGLint readdraw)
{
	load_real();
	return R.GetCurrentSurface ? R.GetCurrentSurface(readdraw) : EGL_NO_SURFACE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surf)
{
	struct Win *w;
	struct gbm_bo *bo;
	uint32_t stride = 0;
	void *map = NULL, *data = NULL;
	XImage *img;
	GC gc;
	int ok;

	load_real();
	if (!R.SwapBuffers)
		return EGL_FALSE;
	if (!R.SwapBuffers(dpy_mesa(dpy), surf))
		return EGL_FALSE;

	pthread_mutex_lock(&lock);
	w = win_find(surf);
	pthread_mutex_unlock(&lock);
	if (!w || !w->gs || !w->xdpy)
		return EGL_TRUE;

	bo = gbm_surface_lock_front_buffer(w->gs);
	if (!bo)
		return EGL_TRUE;
	map = gbm_bo_map(bo, 0, 0, (uint32_t)w->w, (uint32_t)w->h,
			 GBM_BO_TRANSFER_READ, &stride, &data);
	if (!map) {
		gbm_surface_release_buffer(w->gs, bo);
		return EGL_TRUE;
	}
	img = XCreateImage(w->xdpy, DefaultVisual(w->xdpy, DefaultScreen(w->xdpy)),
			   24, ZPixmap, 0, map, (unsigned)w->w, (unsigned)w->h,
			   32, (int)stride);
	if (!img) {
		gbm_bo_unmap(bo, data);
		gbm_surface_release_buffer(w->gs, bo);
		return EGL_TRUE;
	}
	/* XDestroyImage would free map; the BO owns it. */
	img->data = map;
	gc = XCreateGC(w->xdpy, w->xw, 0, NULL);
	ok = !gc ? 0 : XPutImage(w->xdpy, w->xw, gc, img, 0, 0, 0, 0,
				 (unsigned)w->w, (unsigned)w->h);
	if (gc)
		XFreeGC(w->xdpy, gc);
	XFlush(w->xdpy);
	img->data = NULL;
	XDestroyImage(img);
	gbm_bo_unmap(bo, data);
	gbm_surface_release_buffer(w->gs, bo);
	(void)ok;
	return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
	load_real();
	if (!R.SwapInterval)
		return EGL_TRUE;
	return R.SwapInterval(dpy_mesa(dpy), interval);
}

EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface surf, EGLint a, EGLint *v)
{
	struct Win *w;

	load_real();
	pthread_mutex_lock(&lock);
	w = win_find(surf);
	pthread_mutex_unlock(&lock);
	if (w && v && (a == EGL_WIDTH || a == EGL_HEIGHT)) {
		*v = (a == EGL_WIDTH) ? w->w : w->h;
		return EGL_TRUE;
	}
	if (!R.QuerySurface)
		return EGL_FALSE;
	return R.QuerySurface(dpy_mesa(dpy), surf, a, v);
}

EGLint eglGetError(void)
{
	EGLint e = tls_err;

	tls_err = EGL_SUCCESS;
	if (e != EGL_SUCCESS)
		return e;
	load_real();
	return R.GetError ? R.GetError() : EGL_SUCCESS;
}

EGLBoolean eglWaitClient(void)
{
	load_real();
	return R.WaitClient ? R.WaitClient() : EGL_TRUE;
}

EGLBoolean eglWaitGL(void)
{
	load_real();
	return R.WaitGL ? R.WaitGL() : EGL_TRUE;
}

EGLBoolean eglWaitNative(EGLint engine)
{
	load_real();
	return R.WaitNative ? R.WaitNative(engine) : EGL_TRUE;
}

EGLImage eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
			   void *buf, const EGLint *attr)
{
	load_real();
	if (!R.CreateImageKHR)
		return EGL_NO_IMAGE;
	return R.CreateImageKHR(dpy_mesa(dpy), ctx, target, buf, attr);
}

EGLBoolean eglDestroyImageKHR(EGLDisplay dpy, EGLImage img)
{
	load_real();
	if (!R.DestroyImageKHR)
		return EGL_TRUE;
	return R.DestroyImageKHR(dpy_mesa(dpy), img);
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
	static const struct {
		const char *n;
		void *fn;
	} ours[] = {
		{ "eglGetDisplay", (void *)eglGetDisplay },
		{ "eglGetPlatformDisplay", (void *)eglGetPlatformDisplay },
		{ "eglGetPlatformDisplayEXT", (void *)eglGetPlatformDisplayEXT },
		{ "eglInitialize", (void *)eglInitialize },
		{ "eglTerminate", (void *)eglTerminate },
		{ "eglQueryString", (void *)eglQueryString },
		{ "eglChooseConfig", (void *)eglChooseConfig },
		{ "eglGetConfigAttrib", (void *)eglGetConfigAttrib },
		{ "eglCreateContext", (void *)eglCreateContext },
		{ "eglCreateWindowSurface", (void *)eglCreateWindowSurface },
		{ "eglDestroySurface", (void *)eglDestroySurface },
		{ "eglSwapBuffers", (void *)eglSwapBuffers },
		{ "eglCreateImageKHR", (void *)eglCreateImageKHR },
		{ "eglDestroyImageKHR", (void *)eglDestroyImageKHR },
		{ "eglGetProcAddress", (void *)eglGetProcAddress },
	};
	size_t i;

	if (!name)
		return NULL;
	for (i = 0; i < sizeof ours / sizeof ours[0]; i++)
		if (strcmp(name, ours[i].n) == 0)
			return (__eglMustCastToProperFunctionPointerType)ours[i].fn;
	load_real();
	return R.GetProcAddress ? R.GetProcAddress(name) : NULL;
}
