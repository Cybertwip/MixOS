/* SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later */
/*
 * Copyright (c) 2025-2026 the MixOS project and contributors
 * See device/j36-ultra/LICENSE for the licence text and what it covers.
 *
 * j36lima -- Xorg video driver: lima EGL on the same simplefb path as fbdev.
 *
 * WHY THIS EXISTS.  xf86-video-fbdev mmaps /dev/fb0 and draws with the CPU.
 * That is the right PRESENTATION for this board -- the panel is the LK's
 * simple-framebuffer, lima has no display controller, and modesetting
 * mediatek-drm (or lima-as-card0 with DRIVER_MODESET) is how the glass goes
 * black.  j36-xfb.so still intercepts that mmap so PadX can park the picture
 * without freezing Firefox.
 *
 * What fbdev cannot do is hand a GL client the Mali-450.  There is no DRI3,
 * GLX is switched off to avoid a fourteen-second swrast probe, and SDL that
 * asks for the x11 video driver with a GLES renderer finds nothing that
 * talks to card0.  This driver is fbdev's presentation plus lima's render
 * node:
 *
 *   - the same /dev/fb0 mmap, the same ShadowFB, the same 640x480 x8r8g8b8,
 *     no VT, no CRTC, no ADDFB2, no DRM master;
 *   - an EGL 2.0 (GLES2) context is created on card0:lima, the same way
 *     eglprobe -o does -- GBM if the node accepts it, surfaceless if not;
 *   - DRI3 `open` returns that lima node so Mesa's lima_dri.so is the
 *     renderer behind every GLX/EGL client;
 *   - pixmap_from_fds imports a finished lima dma-buf through EGLImage
 *     and reads it into the shadow, which is GPU offload with simplefb
 *     scanout.
 *
 * Card0 on the kernel we ship IS lima.  The probe tries /dev/dri/card0
 * first and only walks the other nodes if that one is not lima.  A
 * mediatek-drm node is never opened: one GETRESOURCES on it is enough
 * to take the panel away from simplefb.
 *
 * If lima is not loaded this still starts, as software ShadowFB, so a boot
 * with j36.lima=0 keeps a desktop.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xorg-server.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <xf86.h>
#pragma GCC diagnostic pop
#include <xf86cmap.h>
#include <fb.h>
#include <micmap.h>
#include <mipointer.h>
#include <shadow.h>
#include <damage.h>
#include <xf86Module.h>

#ifdef HAVE_DRI3
#include <dri3.h>
#include <misyncshm.h>
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/dma-buf.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR	((uint64_t)0)
#endif

#define JLIMA_NAME		"j36lima"
#define JLIMA_VERSION		1000

enum {
	OPTION_FBDEV,
	OPTION_SHADOW,
	OPTION_DRI,
	OPTION_DRINODE,
};

typedef struct {
	int			fb_fd;
	unsigned char		*fb;
	size_t			fb_len;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;
	CloseScreenProcPtr	CloseScreen;
	CreateScreenResourcesProcPtr CreateScreenResources;
	Bool			shadow;
	int			dri_fd;
	char			dri_path[280];
	Bool			dri3;
	Bool			egl2;
	char			egl_renderer[96];
	void			*libegl;
	void			*libgbm;
	void			*gbm;
	void			*egl_dpy;
	void			*egl_cfg;
	void			*egl_ctx;
	OptionInfoPtr		options;
	EntityInfoPtr		pEnt;
} JLimaRec, *JLimaPtr;

#define JLIMAPTR(p) ((JLimaPtr)((p)->driverPrivate))

static const OptionInfoRec JLimaOptions[] = {
	{ OPTION_FBDEV,   "fbdev",    OPTV_STRING,  {0}, FALSE },
	{ OPTION_SHADOW,  "ShadowFB", OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_DRI,     "DRI",      OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_DRINODE, "DRINode",  OPTV_STRING,  {0}, FALSE },
	{ -1, NULL, OPTV_NONE, {0}, FALSE }
};

static const OptionInfoRec *JLimaAvailableOptions(int chipid, int busid)
{
	(void)chipid;
	(void)busid;
	return JLimaOptions;
}

static SymTabRec JLimaChips[] = {
	{ 0, "lima" },
	{ -1, NULL }
};

static void JLimaIdentify(int flags)
{
	(void)flags;
	/* A NULL chipset table is a read of chips->name at address 4
	 * on this xserver -- that is the boot SIGSEGV that left
	 * "The window service is not running".  The table is not const:
	 * xf86PrintChipsets takes a SymTabPtr. */
	xf86PrintChipsets(JLIMA_NAME,
			  "lima EGL offload on simple-framebuffer",
			  JLimaChips);
}

/*
 * Is this fd lima?  The version name is the only honest test: minor numbers
 * move with probe order, and card0 is lima on the kernel we ship.
 */
static Bool dri_is_lima(int fd)
{
	struct drm_version v;
	char name[32];

	memset(&v, 0, sizeof(v));
	memset(name, 0, sizeof(name));
	v.name = name;
	v.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
		return FALSE;
	return strcmp(name, "lima") == 0;
}

/*
 * Never open a modesetting node.  GETRESOURCES on mediatek-drm is how the
 * panel leaves simplefb and does not come back.  A render-only lima node
 * returns EINVAL (or zero CRTCs) and is the one we want.
 */
static Bool dri_has_modeset(int fd)
{
	struct drm_mode_card_res res;

	memset(&res, 0, sizeof(res));
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return FALSE;
	return res.count_crtcs > 0;
}

static int try_lima_path(const char *path, char *out, size_t out_sz)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (dri_has_modeset(fd) || !dri_is_lima(fd)) {
		close(fd);
		return -1;
	}
	snprintf(out, out_sz, "%s", path);
	return fd;
}

static int open_lima_node(const char *forced, char *out, size_t out_sz)
{
	DIR *dir;
	struct dirent *de;
	int fd;
	char path[280];

	if (forced && forced[0])
		return try_lima_path(forced, out, out_sz);

	/* card0 first.  On this kernel it is lima -- the GPU offload
	 * node this driver is named for -- and a render-only lima card
	 * cannot become DRM master. */
	fd = try_lima_path("/dev/dri/card0", out, out_sz);
	if (fd >= 0)
		return fd;

	dir = opendir("/dev/dri");
	if (!dir)
		return -1;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "card", 4) != 0 &&
		    strncmp(de->d_name, "renderD", 7) != 0)
			continue;
		if (strcmp(de->d_name, "card0") == 0)
			continue;
		snprintf(path, sizeof(path), "/dev/dri/%s", de->d_name);
		fd = try_lima_path(path, out, out_sz);
		if (fd >= 0) {
			closedir(dir);
			return fd;
		}
	}
	closedir(dir);
	return -1;
}

/*
 * EGL 2.0 on the lima fd.  Types and entry points are the ABI, declared
 * here so this .so's only DT_NEEDED stays libc -- the same reason
 * eglprobe dlopens.  Xorg is started with LD_LIBRARY_PATH=/run/j36/gl,
 * so these resolve to Mesa, not the RK3326 Mali blob.
 */
struct gbm_device;

typedef void *JLimaEGLDisplay;
typedef void *JLimaEGLConfig;
typedef void *JLimaEGLContext;
typedef void *JLimaEGLSurface;
typedef void *JLimaEGLImage;
typedef unsigned int JLimaEGLBoolean;
typedef unsigned int JLimaEGLenum;
typedef int32_t JLimaEGLint;

#define JL_EGL_NONE			0x3038
#define JL_EGL_BLUE_SIZE		0x3022
#define JL_EGL_GREEN_SIZE		0x3023
#define JL_EGL_RED_SIZE			0x3024
#define JL_EGL_SURFACE_TYPE		0x3033
#define JL_EGL_RENDERABLE_TYPE		0x3040
#define JL_EGL_CONTEXT_CLIENT_VERSION	0x3098
#define JL_EGL_OPENGL_ES2_BIT		0x0004
#define JL_EGL_OPENGL_ES3_BIT		0x00000040
#define JL_EGL_OPENGL_BIT		0x0008
#define JL_EGL_OPENGL_ES_API		0x30A0
#define JL_EGL_OPENGL_API		0x30A2
#define JL_EGL_CONTEXT_MAJOR_VERSION	0x3098
#define JL_EGL_CONTEXT_MINOR_VERSION	0x30FB
#define JL_EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define JL_EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 0x00000002
#define JL_EGL_PLATFORM_GBM_KHR		0x31D7
#define JL_EGL_PLATFORM_SURFACELESS	0x31DD
#define JL_EGL_WIDTH			0x3057
#define JL_EGL_HEIGHT			0x3056
#define JL_EGL_LINUX_DMA_BUF_EXT	0x3270
#define JL_EGL_LINUX_DRM_FOURCC_EXT	0x3271
#define JL_EGL_DMA_BUF_PLANE0_FD_EXT	0x3272
#define JL_EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define JL_EGL_DMA_BUF_PLANE0_PITCH_EXT	0x3274
#define JL_GL_RENDERER			0x1F01
#define JL_GL_VERSION			0x1F02
#define JL_GL_TEXTURE_2D		0x0DE1
#define JL_GL_RGBA			0x1908
#define JL_GL_UNSIGNED_BYTE		0x1401
#define JL_GL_FRAMEBUFFER		0x8D40
#define JL_GL_COLOR_ATTACHMENT0		0x8CE0
#define JL_GL_FRAMEBUFFER_COMPLETE	0x8CD5

static JLimaEGLDisplay (*jl_eglGetDisplay)(void *);
static JLimaEGLDisplay (*jl_eglGetPlatformDisplayEXT)(JLimaEGLenum, void *, const JLimaEGLint *);
static JLimaEGLBoolean (*jl_eglInitialize)(JLimaEGLDisplay, JLimaEGLint *, JLimaEGLint *);
static JLimaEGLBoolean (*jl_eglTerminate)(JLimaEGLDisplay);
static JLimaEGLBoolean (*jl_eglBindAPI)(JLimaEGLenum);
static JLimaEGLBoolean (*jl_eglChooseConfig)(JLimaEGLDisplay, const JLimaEGLint *,
					     JLimaEGLConfig *, JLimaEGLint, JLimaEGLint *);
static JLimaEGLContext (*jl_eglCreateContext)(JLimaEGLDisplay, JLimaEGLConfig,
					      JLimaEGLContext, const JLimaEGLint *);
static JLimaEGLBoolean (*jl_eglDestroyContext)(JLimaEGLDisplay, JLimaEGLContext);
static JLimaEGLBoolean (*jl_eglMakeCurrent)(JLimaEGLDisplay, JLimaEGLSurface,
					    JLimaEGLSurface, JLimaEGLContext);
static void *(*jl_eglGetProcAddress)(const char *);
static JLimaEGLImage (*jl_eglCreateImageKHR)(JLimaEGLDisplay, JLimaEGLContext,
					     JLimaEGLenum, void *, const JLimaEGLint *);
static JLimaEGLBoolean (*jl_eglDestroyImageKHR)(JLimaEGLDisplay, JLimaEGLImage);
static const unsigned char *(*jl_glGetString)(unsigned int);
static void (*jl_glGenTextures)(int, unsigned int *);
static void (*jl_glDeleteTextures)(int, const unsigned int *);
static void (*jl_glBindTexture)(unsigned int, unsigned int);
static void (*jl_glEGLImageTargetTexture2DOES)(unsigned int, JLimaEGLImage);
static void (*jl_glGenFramebuffers)(int, unsigned int *);
static void (*jl_glDeleteFramebuffers)(int, const unsigned int *);
static void (*jl_glBindFramebuffer)(unsigned int, unsigned int);
static void (*jl_glFramebufferTexture2D)(unsigned int, unsigned int, unsigned int,
					 unsigned int, int);
static unsigned int (*jl_glCheckFramebufferStatus)(unsigned int);
static void (*jl_glReadPixels)(int, int, int, int, unsigned int, unsigned int, void *);
static void (*jl_glPixelStorei)(unsigned int, int);
static struct gbm_device *(*jl_gbm_create_device)(int);
static void (*jl_gbm_device_destroy)(struct gbm_device *);

static void *jl_dlsym(void *h, const char *name)
{
	void *s = h ? dlsym(h, name) : NULL;

	if (!s && jl_eglGetProcAddress)
		s = jl_eglGetProcAddress(name);
	return s;
}

static void jlima_egl_fini(JLimaPtr p)
{
	if (!p)
		return;
	if (p->egl_dpy && jl_eglMakeCurrent)
		jl_eglMakeCurrent(p->egl_dpy, NULL, NULL, NULL);
	if (p->egl_ctx && p->egl_dpy && jl_eglDestroyContext)
		jl_eglDestroyContext(p->egl_dpy, p->egl_ctx);
	if (p->egl_dpy && jl_eglTerminate)
		jl_eglTerminate(p->egl_dpy);
	if (p->gbm && jl_gbm_device_destroy)
		jl_gbm_device_destroy(p->gbm);
	if (p->libgbm)
		dlclose(p->libgbm);
	if (p->libegl)
		dlclose(p->libegl);
	p->egl_dpy = p->egl_cfg = p->egl_ctx = p->gbm = NULL;
	p->libegl = p->libgbm = NULL;
	p->egl2 = FALSE;
	p->egl_renderer[0] = '\0';
}

static JLimaEGLContext jlima_try_context(JLimaPtr p, JLimaEGLConfig cfg,
					 JLimaEGLenum api, const JLimaEGLint *attr,
					 const char *gl_override, const char *gles_override)
{
	JLimaEGLContext ctx;

	if (gl_override)
		setenv("MESA_GL_VERSION_OVERRIDE", gl_override, 1);
	else
		unsetenv("MESA_GL_VERSION_OVERRIDE");
	if (gles_override)
		setenv("MESA_GLES_VERSION_OVERRIDE", gles_override, 1);
	else
		unsetenv("MESA_GLES_VERSION_OVERRIDE");
	if (gl_override)
		setenv("MESA_GLSL_VERSION_OVERRIDE", "460", 1);
	else
		unsetenv("MESA_GLSL_VERSION_OVERRIDE");
	if (!jl_eglBindAPI(api))
		return NULL;
	ctx = jl_eglCreateContext(p->egl_dpy, cfg, NULL, attr);
	if (ctx && jl_eglMakeCurrent(p->egl_dpy, NULL, NULL, ctx))
		return ctx;
	if (ctx && jl_eglDestroyContext)
		jl_eglDestroyContext(p->egl_dpy, ctx);
	return NULL;
}

static Bool jlima_egl_init(ScrnInfoPtr pScrn, JLimaPtr p)
{
	static const JLimaEGLint cfg_es[] = {
		JL_EGL_RENDERABLE_TYPE, JL_EGL_OPENGL_ES2_BIT,
		JL_EGL_RED_SIZE, 8,
		JL_EGL_GREEN_SIZE, 8,
		JL_EGL_BLUE_SIZE, 8,
		JL_EGL_SURFACE_TYPE, 0,
		JL_EGL_NONE
	};
	static const JLimaEGLint ctx_gl46[] = {
		JL_EGL_CONTEXT_MAJOR_VERSION, 4,
		JL_EGL_CONTEXT_MINOR_VERSION, 6,
		JL_EGL_CONTEXT_OPENGL_PROFILE_MASK,
		JL_EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
		JL_EGL_NONE
	};
	static const JLimaEGLint ctx_es32[] = {
		JL_EGL_CONTEXT_MAJOR_VERSION, 3,
		JL_EGL_CONTEXT_MINOR_VERSION, 2,
		JL_EGL_NONE
	};
	static const JLimaEGLint ctx_es2[] = {
		JL_EGL_CONTEXT_CLIENT_VERSION, 2,
		JL_EGL_NONE
	};
	JLimaEGLConfig pick[8];
	JLimaEGLint n = 0, major = 0, minor = 0;
	const unsigned char *s;
	const char *kind = "GLES2";

	if (p->dri_fd < 0)
		return FALSE;

	p->libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!p->libegl)
		p->libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	if (!p->libegl) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: libEGL not on this search path (%s)\n",
			   dlerror());
		return FALSE;
	}
	jl_eglGetProcAddress = dlsym(p->libegl, "eglGetProcAddress");
	jl_eglGetDisplay = jl_dlsym(p->libegl, "eglGetDisplay");
	jl_eglGetPlatformDisplayEXT = jl_dlsym(p->libegl, "eglGetPlatformDisplayEXT");
	jl_eglInitialize = jl_dlsym(p->libegl, "eglInitialize");
	jl_eglTerminate = jl_dlsym(p->libegl, "eglTerminate");
	jl_eglBindAPI = jl_dlsym(p->libegl, "eglBindAPI");
	jl_eglChooseConfig = jl_dlsym(p->libegl, "eglChooseConfig");
	jl_eglCreateContext = jl_dlsym(p->libegl, "eglCreateContext");
	jl_eglDestroyContext = jl_dlsym(p->libegl, "eglDestroyContext");
	jl_eglMakeCurrent = jl_dlsym(p->libegl, "eglMakeCurrent");
	if (!jl_eglInitialize || !jl_eglBindAPI || !jl_eglCreateContext ||
	    !jl_eglMakeCurrent || !jl_eglChooseConfig) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: libEGL is missing ES2 entry points\n");
		jlima_egl_fini(p);
		return FALSE;
	}

	p->libgbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (p->libgbm) {
		jl_gbm_create_device = dlsym(p->libgbm, "gbm_create_device");
		jl_gbm_device_destroy = dlsym(p->libgbm, "gbm_device_destroy");
		if (jl_gbm_create_device)
			p->gbm = jl_gbm_create_device(p->dri_fd);
	}

	if (p->gbm && jl_eglGetPlatformDisplayEXT)
		p->egl_dpy = jl_eglGetPlatformDisplayEXT(JL_EGL_PLATFORM_GBM_KHR,
							 p->gbm, NULL);
	if (!p->egl_dpy && p->gbm && jl_eglGetDisplay)
		p->egl_dpy = jl_eglGetDisplay(p->gbm);
	if (!p->egl_dpy && jl_eglGetPlatformDisplayEXT)
		p->egl_dpy = jl_eglGetPlatformDisplayEXT(JL_EGL_PLATFORM_SURFACELESS,
							 NULL, NULL);
	if (!p->egl_dpy && jl_eglGetDisplay)
		p->egl_dpy = jl_eglGetDisplay((void *)(intptr_t)p->dri_fd);
	if (!p->egl_dpy || !jl_eglInitialize(p->egl_dpy, &major, &minor)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: no EGL display on %s\n", p->dri_path);
		jlima_egl_fini(p);
		return FALSE;
	}
	if (!jl_eglChooseConfig(p->egl_dpy, cfg_es, pick, 8, &n) || n < 1) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: no EGL config on %s\n", p->dri_path);
		jlima_egl_fini(p);
		return FALSE;
	}
	p->egl_cfg = pick[0];
	/* Highest version Mesa will advertise, then GLES2.  Import/readback
	 * only needs a current context; GLES 3.2 / GL 4.6 compat are what
	 * Firefox and desktop GL request. */
	p->egl_ctx = jlima_try_context(p, p->egl_cfg, JL_EGL_OPENGL_API,
				       ctx_gl46, "4.6COMPAT", NULL);
	if (p->egl_ctx)
		kind = "GL 4.6 compat";
	if (!p->egl_ctx) {
		p->egl_ctx = jlima_try_context(p, p->egl_cfg, JL_EGL_OPENGL_ES_API,
					       ctx_es32, NULL, "3.2");
		if (p->egl_ctx)
			kind = "GLES 3.2";
	}
	if (!p->egl_ctx) {
		p->egl_ctx = jlima_try_context(p, p->egl_cfg, JL_EGL_OPENGL_ES_API,
					       ctx_es2, NULL, NULL);
		kind = "GLES2";
	}
	if (!p->egl_ctx) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: no EGL context on %s\n", p->dri_path);
		jlima_egl_fini(p);
		return FALSE;
	}

	jl_glGetString = jl_dlsym(p->libegl, "glGetString");
	jl_glGenTextures = jl_dlsym(p->libegl, "glGenTextures");
	jl_glDeleteTextures = jl_dlsym(p->libegl, "glDeleteTextures");
	jl_glBindTexture = jl_dlsym(p->libegl, "glBindTexture");
	jl_glEGLImageTargetTexture2DOES = jl_dlsym(p->libegl, "glEGLImageTargetTexture2DOES");
	jl_glGenFramebuffers = jl_dlsym(p->libegl, "glGenFramebuffers");
	jl_glDeleteFramebuffers = jl_dlsym(p->libegl, "glDeleteFramebuffers");
	jl_glBindFramebuffer = jl_dlsym(p->libegl, "glBindFramebuffer");
	jl_glFramebufferTexture2D = jl_dlsym(p->libegl, "glFramebufferTexture2D");
	jl_glCheckFramebufferStatus = jl_dlsym(p->libegl, "glCheckFramebufferStatus");
	jl_glReadPixels = jl_dlsym(p->libegl, "glReadPixels");
	jl_glPixelStorei = jl_dlsym(p->libegl, "glPixelStorei");
	jl_eglCreateImageKHR = jl_dlsym(p->libegl, "eglCreateImageKHR");
	jl_eglDestroyImageKHR = jl_dlsym(p->libegl, "eglDestroyImageKHR");

	s = jl_glGetString ? jl_glGetString(JL_GL_RENDERER) : NULL;
	snprintf(p->egl_renderer, sizeof(p->egl_renderer), "%s",
		 s ? (const char *)s : "EGL 2.0");
	s = jl_glGetString ? jl_glGetString(JL_GL_VERSION) : NULL;
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "lima: EGL %d.%d %s on %s -- renderer \"%s\" version \"%s\"\n",
		   major, minor, kind, p->dri_path, p->egl_renderer,
		   s ? (const char *)s : "?");
	p->egl2 = TRUE;
	return TRUE;
}

static Bool jlima_egl_import_pixmap(JLimaPtr p, PixmapPtr pix,
				    int fd, CARD16 width, CARD16 height,
				    CARD32 stride, CARD32 offset, CARD32 fourcc)
{
	JLimaEGLImage img;
	unsigned int tex = 0, fbo = 0;
	char *dst;
	int y, dst_stride;
	JLimaEGLint attr[16];
	int a = 0;

	if (!p->egl2 || !jl_eglCreateImageKHR || !jl_glReadPixels ||
	    !jl_glEGLImageTargetTexture2DOES)
		return FALSE;
	if (!jl_eglMakeCurrent(p->egl_dpy, NULL, NULL, p->egl_ctx))
		return FALSE;

	attr[a++] = JL_EGL_WIDTH; attr[a++] = width;
	attr[a++] = JL_EGL_HEIGHT; attr[a++] = height;
	attr[a++] = JL_EGL_LINUX_DRM_FOURCC_EXT; attr[a++] = (JLimaEGLint)fourcc;
	attr[a++] = JL_EGL_DMA_BUF_PLANE0_FD_EXT; attr[a++] = fd;
	attr[a++] = JL_EGL_DMA_BUF_PLANE0_OFFSET_EXT; attr[a++] = (JLimaEGLint)offset;
	attr[a++] = JL_EGL_DMA_BUF_PLANE0_PITCH_EXT; attr[a++] = (JLimaEGLint)stride;
	attr[a++] = JL_EGL_NONE;
	img = jl_eglCreateImageKHR(p->egl_dpy, NULL, JL_EGL_LINUX_DMA_BUF_EXT,
				   NULL, attr);
	if (!img)
		return FALSE;

	jl_glGenTextures(1, &tex);
	jl_glBindTexture(JL_GL_TEXTURE_2D, tex);
	jl_glEGLImageTargetTexture2DOES(JL_GL_TEXTURE_2D, img);
	jl_glGenFramebuffers(1, &fbo);
	jl_glBindFramebuffer(JL_GL_FRAMEBUFFER, fbo);
	jl_glFramebufferTexture2D(JL_GL_FRAMEBUFFER, JL_GL_COLOR_ATTACHMENT0,
				  JL_GL_TEXTURE_2D, tex, 0);
	if (jl_glCheckFramebufferStatus(JL_GL_FRAMEBUFFER) != JL_GL_FRAMEBUFFER_COMPLETE) {
		jl_glBindFramebuffer(JL_GL_FRAMEBUFFER, 0);
		jl_glDeleteFramebuffers(1, &fbo);
		jl_glDeleteTextures(1, &tex);
		jl_eglDestroyImageKHR(p->egl_dpy, img);
		return FALSE;
	}

	dst = pix->devPrivate.ptr;
	dst_stride = pix->devKind;
	if (jl_glPixelStorei)
		jl_glPixelStorei(0x0D05, 1); /* GL_PACK_ALIGNMENT */
	if (dst_stride == (int)width * 4) {
		jl_glReadPixels(0, 0, width, height, JL_GL_RGBA, JL_GL_UNSIGNED_BYTE, dst);
	} else {
		for (y = 0; y < height; y++)
			jl_glReadPixels(0, y, width, 1, JL_GL_RGBA, JL_GL_UNSIGNED_BYTE,
					dst + (size_t)y * dst_stride);
	}

	jl_glBindFramebuffer(JL_GL_FRAMEBUFFER, 0);
	jl_glDeleteFramebuffers(1, &fbo);
	jl_glDeleteTextures(1, &tex);
	jl_eglDestroyImageKHR(p->egl_dpy, img);
	return TRUE;
}

#ifdef HAVE_DRI3

static int jlima_dri3_open_client(ClientPtr client, ScreenPtr screen,
				  RRProviderPtr provider, int *fdp)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
	JLimaPtr p = JLIMAPTR(pScrn);
	int fd;

	(void)client;
	(void)provider;
	if (!p || p->dri_path[0] == '\0')
		return BadAlloc;
	fd = open(p->dri_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return BadAlloc;
	*fdp = fd;
	return Success;
}

/*
 * Import a lima dma-buf into an ordinary pixmap.  LINEAR is the only
 * layout we advertise, so a mmap of the fd is the finished picture and
 * the copy into ShadowFB is the same present eglprobe -o does.
 */
static PixmapPtr jlima_pixmap_from_fds(ScreenPtr screen, CARD8 num_fds,
				       const int *fds, CARD16 width,
				       CARD16 height, const CARD32 *strides,
				       const CARD32 *offsets, CARD8 depth,
				       CARD8 bpp, CARD64 modifier)
{
	PixmapPtr pix;
	char *dst;
	unsigned char *src;
	int i, stride, bytes;

	if (num_fds != 1 || !fds || fds[0] < 0)
		return NullPixmap;
	if (modifier != 0 && modifier != DRM_FORMAT_MOD_LINEAR)
		return NullPixmap;
	if (width == 0 || height == 0 || strides[0] == 0)
		return NullPixmap;

	pix = screen->CreatePixmap(screen, width, height, depth,
				   CREATE_PIXMAP_USAGE_SCRATCH);
	if (!pix)
		return NullPixmap;

	stride = pix->devKind;
	bytes = (bpp + 7) / 8;
	dst = pix->devPrivate.ptr;
	if (!dst) {
		screen->DestroyPixmap(pix);
		return NullPixmap;
	}

	/* GPU present: lima rendered it, EGL 2.0 reads it back. */
	{
		ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
		JLimaPtr jp = JLIMAPTR(pScrn);
		CARD32 fourcc = (depth == 32) ? 0x34325241u : 0x34325258u;

		if (jp && jp->egl2 &&
		    jlima_egl_import_pixmap(jp, pix, fds[0], width, height,
					    strides[0], offsets[0], fourcc))
			return pix;
	}

	src = mmap(NULL, (size_t)strides[0] * height + offsets[0],
		   PROT_READ, MAP_SHARED, fds[0], 0);
	if (src == MAP_FAILED) {
		screen->DestroyPixmap(pix);
		return NullPixmap;
	}

	{
		struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START |
						      DMA_BUF_SYNC_READ };
		(void)ioctl(fds[0], DMA_BUF_IOCTL_SYNC, &sync);
	}

	for (i = 0; i < height; i++) {
		memcpy(dst + (size_t)i * stride,
		       src + offsets[0] + (size_t)i * strides[0],
		       (size_t)width * bytes);
	}

	{
		struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END |
						      DMA_BUF_SYNC_READ };
		(void)ioctl(fds[0], DMA_BUF_IOCTL_SYNC, &sync);
	}
	munmap(src, (size_t)strides[0] * height + offsets[0]);
	return pix;
}

static int jlima_get_formats(ScreenPtr screen, CARD32 *num, CARD32 **fmts)
{
	static const CARD32 formats[] = {
		0x34325241, /* DRM_FORMAT_ARGB8888 */
		0x34325258, /* DRM_FORMAT_XRGB8888 */
	};

	(void)screen;
	*num = 2;
	*fmts = malloc(sizeof(formats));
	if (!*fmts)
		return FALSE;
	memcpy(*fmts, formats, sizeof(formats));
	return TRUE;
}

static int jlima_get_modifiers(ScreenPtr screen, uint32_t format,
			       uint32_t *num, uint64_t **mods)
{
	(void)screen;
	(void)format;
	*num = 1;
	*mods = malloc(sizeof(uint64_t));
	if (!*mods)
		return FALSE;
	(*mods)[0] = DRM_FORMAT_MOD_LINEAR;
	return TRUE;
}

static dri3_screen_info_rec jlima_dri3_info = {
	.version = 2,
	.open = NULL,
	.pixmap_from_fds = jlima_pixmap_from_fds,
	.get_formats = jlima_get_formats,
	.get_modifiers = jlima_get_modifiers,
	.open_client = jlima_dri3_open_client,
};

static Bool jlima_dri3_init(ScreenPtr screen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(screen);
	JLimaPtr p = JLIMAPTR(pScrn);

	if (!p || p->dri_fd < 0)
		return FALSE;
	if (!xf86LoadSubModule(pScrn, "dri3")) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "lima: dri3 module missing; GL clients stay on software\n");
		return FALSE;
	}
	if (!miSyncShmScreenInit(screen))
		return FALSE;
	if (!dri3_screen_init(screen, &jlima_dri3_info)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "lima: dri3_screen_init failed\n");
		return FALSE;
	}
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "lima: DRI3 on %s -- GL clients render GLES2 on the Mali-450, "
		   "this driver presents through simplefb%s\n",
		   p->dri_path,
		   p->egl2 ? " via EGL 2.0" : "");
	return TRUE;
}

#endif /* HAVE_DRI3 */

static Bool JLimaSaveScreen(ScreenPtr pScreen, int mode)
{
	(void)pScreen;
	(void)mode;
	return TRUE;
}

static Bool JLimaCloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	JLimaPtr p = JLIMAPTR(pScrn);

	pScreen->CloseScreen = p->CloseScreen;
	if (p->fb && p->fb != MAP_FAILED) {
		munmap(p->fb, p->fb_len);
		p->fb = NULL;
	}
	if (p->fb_fd >= 0) {
		close(p->fb_fd);
		p->fb_fd = -1;
	}
	jlima_egl_fini(p);
	if (p->dri_fd >= 0) {
		close(p->dri_fd);
		p->dri_fd = -1;
	}
	return (*pScreen->CloseScreen)(pScreen);
}

static void JLimaUpdatePacked(ScreenPtr pScreen, shadowBufPtr pBuf)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	JLimaPtr p = JLIMAPTR(pScrn);
	RegionPtr damage = DamageRegion(pBuf->pDamage);
	int nbox = RegionNumRects(damage);
	BoxPtr box = RegionRects(damage);
	int stride = p->fix.line_length;
	int bpp = p->var.bits_per_pixel;
	int i;

	if (!p->fb)
		return;
	for (i = 0; i < nbox; i++) {
		int x = box[i].x1;
		int y = box[i].y1;
		int w = box[i].x2 - box[i].x1;
		int h = box[i].y2 - box[i].y1;
		int bytes = w * ((bpp + 7) / 8);
		int row;
		unsigned char *src = (unsigned char *)pBuf->pPixmap->devPrivate.ptr;
		int src_stride = pBuf->pPixmap->devKind;

		if (x < 0 || y < 0 || w <= 0 || h <= 0)
			continue;
		for (row = 0; row < h; row++) {
			memcpy(p->fb + (size_t)(y + row) * stride +
				       (size_t)x * ((bpp + 7) / 8),
			       src + (size_t)(y + row) * src_stride +
				     (size_t)x * ((bpp + 7) / 8),
			       (size_t)bytes);
		}
	}
}

static Bool JLimaCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	JLimaPtr p = JLIMAPTR(pScrn);
	Bool ok;
	PixmapPtr pix;

	pScreen->CreateScreenResources = p->CreateScreenResources;
	ok = (*pScreen->CreateScreenResources)(pScreen);
	pScreen->CreateScreenResources = JLimaCreateScreenResources;
	if (!ok)
		return FALSE;

	if (p->shadow) {
		pix = pScreen->GetScreenPixmap(pScreen);
		if (!shadowAdd(pScreen, pix, JLimaUpdatePacked, NULL, 0, 0))
			return FALSE;
	}
	return TRUE;
}

static Bool JLimaScreenInit(ScreenPtr pScreen, int argc, char **argv)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	JLimaPtr p = JLIMAPTR(pScrn);
	VisualPtr visual;
	int displayWidth;

	(void)argc;
	(void)argv;

	p->fb = mmap(NULL, p->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		     p->fb_fd, 0);
	if (p->fb == MAP_FAILED) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "mmap %s failed: %s\n",
			   xf86GetOptValString(p->options, OPTION_FBDEV)
			       ? xf86GetOptValString(p->options, OPTION_FBDEV)
			       : "/dev/fb0",
			   strerror(errno));
		p->fb = NULL;
		return FALSE;
	}

	displayWidth = p->fix.line_length / ((p->var.bits_per_pixel + 7) / 8);
	if (displayWidth == 0)
		displayWidth = pScrn->virtualX;

	miClearVisualTypes();
	if (!miSetVisualTypes(pScrn->depth,
			      TrueColorMask | DirectColorMask,
			      pScrn->rgbBits, TrueColor))
		return FALSE;
	if (!miSetPixmapDepths())
		return FALSE;

	if (!fbScreenInit(pScreen, p->shadow ? NULL : p->fb,
			  pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi,
			  displayWidth, pScrn->bitsPerPixel))
		return FALSE;

	if (pScrn->bitsPerPixel > 8) {
		visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue = pScrn->offset.blue;
				visual->redMask = pScrn->mask.red;
				visual->greenMask = pScrn->mask.green;
				visual->blueMask = pScrn->mask.blue;
			}
		}
	}

	fbPictureInit(pScreen, 0, 0);
	xf86SetBlackWhitePixels(pScreen);

	if (p->shadow) {
		if (!shadowSetup(pScreen))
			return FALSE;
	}

	p->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = JLimaCreateScreenResources;

	xf86SetBackingStore(pScreen);
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());
	if (!miCreateDefColormap(pScreen))
		return FALSE;
	xf86HandleColormaps(pScreen, 256, 8, NULL, NULL,
			    CMAP_PALETTED_TRUECOLOR | CMAP_RELOAD_ON_MODE_SWITCH);

	p->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = JLimaCloseScreen;

	if (p->dri_fd >= 0)
		jlima_egl_init(pScrn, p);
#ifdef HAVE_DRI3
	if (p->dri_fd >= 0)
		p->dri3 = jlima_dri3_init(pScreen);
#endif
	/* xf86SaveScreen was removed from the xserver headers this
	 * chroot ships.  DPMS/blanking is already off in xorg.conf;
	 * a no-op keeps the ScreenRec populated. */
	pScreen->SaveScreen = JLimaSaveScreen;
	return TRUE;
}

static Bool JLimaSwitchMode(ScrnInfoPtr pScrn, DisplayModePtr mode)
{
	(void)pScrn;
	(void)mode;
	return TRUE;
}

static void JLimaAdjustFrame(ScrnInfoPtr pScrn, int x, int y)
{
	(void)pScrn;
	(void)x;
	(void)y;
}

static Bool JLimaEnterVT(ScrnInfoPtr pScrn)
{
	pScrn->vtSema = TRUE;
	return TRUE;
}

static void JLimaLeaveVT(ScrnInfoPtr pScrn)
{
	pScrn->vtSema = FALSE;
}

static ModeStatus JLimaValidMode(ScrnInfoPtr pScrn, DisplayModePtr mode,
				 Bool verbose, int flags)
{
	(void)pScrn;
	(void)mode;
	(void)verbose;
	(void)flags;
	return MODE_OK;
}

static void JLimaFreeScreen(ScrnInfoPtr pScrn)
{
	JLimaPtr p = JLIMAPTR(pScrn);

	if (!p)
		return;
	free(p->options);
	free(p->pEnt);
	free(p);
	pScrn->driverPrivate = NULL;
}

static void JLimaInstallMode(ScrnInfoPtr pScrn, JLimaPtr p)
{
	DisplayModePtr mode = xnfcalloc(1, sizeof(DisplayModeRec));
	char *name = xnfalloc(32);
	const int hdisplay = (int)p->var.xres;
	const int vdisplay = (int)p->var.yres;

	/* The server walks pScrn->currentMode the instant PreInit
	 * returns.  A NULL mode is a read at offset 0x18 -- the crash
	 * after "Using gamma correction" in the device log.
	 *
	 * The mode also needs honest timings.  A zero pixel clock or totals
	 * equal to the visible size leave helpers in the refresh path with a
	 * divide-by-zero, which is the later SIGFPE that brings down the
	 * window service. */
	snprintf(name, 32, "%dx%d", p->var.xres, p->var.yres);
	mode->name = name;
	mode->status = MODE_OK;
	mode->type = M_T_BUILTIN;
	mode->HDisplay = hdisplay;
	mode->VDisplay = vdisplay;
	if (hdisplay == 640 && vdisplay == 480) {
		/* Standard VGA timings.  The panel is scanned out by simplefb,
		 * but Xorg still expects a coherent modeline for bookkeeping. */
		mode->Clock = 25175;
		mode->HSyncStart = 656;
		mode->HSyncEnd = 752;
		mode->HTotal = 800;
		mode->VSyncStart = 490;
		mode->VSyncEnd = 492;
		mode->VTotal = 525;
	} else {
		/* Keep every divisor non-zero if another simplefb geometry ever
		 * appears on this path.  The exact porch values do not drive the
		 * glass here; they only satisfy the server's mode math. */
		mode->Clock = hdisplay * vdisplay * 60 / 1000;
		if (mode->Clock <= 0)
			mode->Clock = 1;
		mode->HSyncStart = hdisplay + 16;
		mode->HSyncEnd = mode->HSyncStart + 96;
		mode->HTotal = mode->HSyncEnd + 48;
		mode->VSyncStart = vdisplay + 10;
		mode->VSyncEnd = mode->VSyncStart + 2;
		mode->VTotal = mode->VSyncEnd + 33;
	}
	mode->next = mode;
	mode->prev = mode;
	pScrn->modes = mode;
	pScrn->currentMode = mode;
}

static Bool JLimaPreInit(ScrnInfoPtr pScrn, int flags)
{
	JLimaPtr p;
	const char *fbdev, *drinode;
	Bool shadow = TRUE, want_dri = TRUE;
	Gamma zeros = { 0.0, 0.0, 0.0 };
	rgb zeros_rgb = { 0, 0, 0 };

	if (flags & PROBE_DETECT)
		return TRUE;

	if (!xf86SetDepthBpp(pScrn, 24, 0, 32, Support32bppFb))
		return FALSE;
	xf86PrintDepthBpp(pScrn);

	if (pScrn->depth != 24 && pScrn->depth != 32) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "depth %d is not 24 or 32\n", pScrn->depth);
		return FALSE;
	}

	if (!xf86SetWeight(pScrn, zeros_rgb, zeros_rgb))
		return FALSE;
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	if (pScrn->numEntities < 1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "lima: no entity (the fb slot was not claimed)\n");
		return FALSE;
	}

	p = pScrn->driverPrivate = xnfcalloc(1, sizeof(JLimaRec));
	p->fb_fd = -1;
	p->dri_fd = -1;
	p->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);
	if (pScrn->confScreen)
		pScrn->monitor = pScrn->confScreen->monitor;

	/*
	 * Do not call xf86CollectOptions().  On this xserver it ends in
	 * xf86MergeOutputClassOptions(), which logs "unsupported bus type 0"
	 * for an FB-slot entity and then reads a NULL platform device
	 * (SIGSEGV at 0xb0).  That is the crash after "Default visual is
	 * TrueColor" in the device log.  Device options are already on the
	 * GDev from xorg.conf.
	 */
	if (!(p->options = malloc(sizeof(JLimaOptions))))
		return FALSE;
	memcpy(p->options, JLimaOptions, sizeof(JLimaOptions));
	{
		pointer opts = pScrn->options;

		if (!opts && p->pEnt && p->pEnt->device)
			opts = p->pEnt->device->options;
		xf86ProcessOptions(pScrn->scrnIndex, opts, p->options);
	}

	fbdev = xf86GetOptValString(p->options, OPTION_FBDEV);
	if (!fbdev)
		fbdev = "/dev/fb0";
	xf86GetOptValBool(p->options, OPTION_SHADOW, &shadow);
	xf86GetOptValBool(p->options, OPTION_DRI, &want_dri);
	drinode = xf86GetOptValString(p->options, OPTION_DRINODE);

	p->fb_fd = open(fbdev, O_RDWR | O_CLOEXEC);
	if (p->fb_fd < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "open %s: %s\n", fbdev, strerror(errno));
		return FALSE;
	}
	if (ioctl(p->fb_fd, FBIOGET_FSCREENINFO, &p->fix) < 0 ||
	    ioctl(p->fb_fd, FBIOGET_VSCREENINFO, &p->var) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "%s is not a framebuffer\n", fbdev);
		close(p->fb_fd);
		p->fb_fd = -1;
		return FALSE;
	}

	pScrn->virtualX = p->var.xres;
	pScrn->virtualY = p->var.yres;
	pScrn->displayWidth = p->fix.line_length /
			      ((p->var.bits_per_pixel + 7) / 8);
	p->fb_len = p->fix.smem_len;
	if (p->fb_len < (size_t)p->fix.line_length * p->var.yres)
		p->fb_len = (size_t)p->fix.line_length * p->var.yres;
	p->shadow = shadow;

	pScrn->offset.red = p->var.red.offset;
	pScrn->offset.green = p->var.green.offset;
	pScrn->offset.blue = p->var.blue.offset;
	pScrn->mask.red = ((1u << p->var.red.length) - 1) << p->var.red.offset;
	pScrn->mask.green = ((1u << p->var.green.length) - 1) << p->var.green.offset;
	pScrn->mask.blue = ((1u << p->var.blue.length) - 1) << p->var.blue.offset;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "%s is %dx%d, %u bpp, stride %u, shadow=%s\n",
		   fbdev, p->var.xres, p->var.yres, p->var.bits_per_pixel,
		   p->fix.line_length, p->shadow ? "on" : "off");

	if (p->shadow) {
		if (!xf86LoadSubModule(pScrn, "shadow")) {
			JLimaFreeScreen(pScrn);
			return FALSE;
		}
	}
	if (!xf86LoadSubModule(pScrn, "fb")) {
		JLimaFreeScreen(pScrn);
		return FALSE;
	}

	if (want_dri) {
		p->dri_fd = open_lima_node(drinode, p->dri_path,
					   sizeof(p->dri_path));
		if (p->dri_fd < 0) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "lima: no render-only lima node; "
				   "this session is software ShadowFB\n");
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "lima: card0 GPU offload on %s (EGL 2.0 / DRI3, no modeset)\n",
				   p->dri_path);
		}
	}

	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->chipset = "lima";
	pScrn->videoRam = (int)(p->fb_len / 1024);
	if (!xf86SetGamma(pScrn, zeros))
		return FALSE;
	JLimaInstallMode(pScrn, p);
	pScrn->SwitchMode = JLimaSwitchMode;
	pScrn->AdjustFrame = JLimaAdjustFrame;
	pScrn->EnterVT = JLimaEnterVT;
	pScrn->LeaveVT = JLimaLeaveVT;
	pScrn->ValidMode = JLimaValidMode;
	pScrn->FreeScreen = JLimaFreeScreen;

	/*
	 * fbdev finishes PreInit with the mode printed and DPI set.  The server
	 * does the same if the driver does not, and on this xserver both paths
	 * SIGFPE when the mode has no pixel clock or the monitor has no physical
	 * size -- exactly the one-millisecond crash after "Using gamma correction".
	 */
	if (pScrn->monitor) {
		if (pScrn->monitor->widthmm <= 0)
			pScrn->monitor->widthmm = 169;
		if (pScrn->monitor->heightmm <= 0)
			pScrn->monitor->heightmm = 127;
	}
	pScrn->displayWidth = pScrn->virtualX;
	xf86PrintModes(pScrn);
	xf86SetDpi(pScrn, 0, 0);

	pScrn->vtSema = FALSE;
	return TRUE;
}

static Bool JLimaProbe(DriverPtr drv, int flags)
{
	GDevPtr *devSections;
	int n, i, found = 0;
	Bool probeDetect = flags & PROBE_DETECT;

	n = xf86MatchDevice(JLIMA_NAME, &devSections);
	if (n <= 0)
		return FALSE;

	if (probeDetect) {
		free(devSections);
		return TRUE;
	}

	for (i = 0; i < n; i++) {
		int entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
		ScrnInfoPtr pScrn = xf86ConfigFbEntity(NULL, 0, entity,
						       NULL, NULL, NULL, NULL);

		if (!pScrn)
			continue;
		pScrn->driverVersion = JLIMA_VERSION;
		pScrn->driverName = JLIMA_NAME;
		pScrn->name = JLIMA_NAME;
		pScrn->Probe = JLimaProbe;
		pScrn->PreInit = JLimaPreInit;
		pScrn->ScreenInit = JLimaScreenInit;
		pScrn->SwitchMode = JLimaSwitchMode;
		pScrn->AdjustFrame = JLimaAdjustFrame;
		pScrn->EnterVT = JLimaEnterVT;
		pScrn->LeaveVT = JLimaLeaveVT;
		pScrn->ValidMode = JLimaValidMode;
		pScrn->FreeScreen = JLimaFreeScreen;
		found++;
	}
	free(devSections);
	return found != 0;
}

_X_EXPORT DriverRec J36LIMA = {
	.driverVersion = JLIMA_VERSION,
	.driverName = JLIMA_NAME,
	.Identify = JLimaIdentify,
	.Probe = JLimaProbe,
	.AvailableOptions = JLimaAvailableOptions,
};

static XF86ModuleVersionInfo JLimaVersRec = {
	JLIMA_NAME,
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	1, 0, 0,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	MOD_CLASS_VIDEODRV,
	{0, 0, 0, 0}
};

static pointer JLimaSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool set = FALSE;

	(void)opts;
	(void)errmin;
	if (set) {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
	set = TRUE;
	xf86AddDriver(&J36LIMA, module, 0);
	return (pointer)1;
}

_X_EXPORT XF86ModuleData j36limaModuleData = {
	&JLimaVersRec,
	JLimaSetup,
	NULL
};
