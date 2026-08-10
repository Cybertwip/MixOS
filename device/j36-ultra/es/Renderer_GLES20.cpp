// EmulationStation's third renderer: OpenGL ES 2.0, shader based.
//
// ES-fcamod 351v carries two renderers and both are fixed function --
// Renderer_GLES10.cpp calls glMatrixMode and glEnableClientState, Renderer_GL21.cpp
// calls the same functions through the desktop headers. On this board neither can
// run, and the reason was measured rather than guessed. j36-eglprobe, run on the
// card's own libraries against card0 and renderD128:
//
//   ES2 = ctx/cur "OpenGL ES 2.0 Mesa 25.0.7-2+deb13u1"     lima, the render node
//   ES1 = 0x3003 (EGL_BAD_ALLOC)                            lima
//   ES1 = 0x3003 (EGL_BAD_ALLOC)                            llvmpipe AND softpipe
//
// The third row is the one that settles it: an ES1 context is impossible in
// Debian's Mesa 25.0.7 on every driver it ships, software included, so it is a
// -Dgles1=disabled build and no amount of work on this SoC produces a GLES1
// context. GLES 2.0 is what this stack can hand out, and a GLES 2.0 context has no
// matrix stack, no client-state enables and no texture environment -- hence a
// renderer, not a patch.
//
// NOT ONE GL LIBRARY IS LINKED. Every entry point below is resolved at runtime
// through SDL_GL_GetProcAddress, and that is deliberate. The card carries one armhf
// Debian rootfs for two machines, and on it libEGL.so, libGLESv2.so and
// libGLESv1_CM.so are symlinks to the RK3326's Mali-G31 blob -- which has no
// SONAME, so whatever a linker is pointed at becomes this binary's DT_NEEDED by
// accident, and which is compiled for ARMv8-A and SIGILLs instantly on this
// Cortex-A7. Resolving through SDL means the library name comes from
// SDL_VIDEO_EGL_DRIVER at runtime, the binary has no GL DT_NEEDED at all, and one
// build runs on the R36S's blob and on this board's Mesa without a preload.
//
// Three programs rather than one program with a mode uniform, because the GPU
// behind this is a Mali-400 class Utgard and its fragment hardware has no
// branching; lima's fragment compiler is correspondingly narrow. Three shaders of
// one line each cost nothing to compile and ask nothing of it.
//
// The third of them is the one that is easy to get wrong. ES uploads its glyph
// atlases as GL_ALPHA and expects coloured text out of them, which under fixed
// function is not "modulate by the texel": the spec's MODULATE row for an ALPHA
// texture is Cv = Cf, Av = Af * At, i.e. the fragment keeps the vertex colour and
// only alpha is modulated. A shader that wrote v_col * texel would multiply by an
// RGB of zero and every string on the panel would be black. gles20_alpha_fs
// reproduces the spec row.
#include <string>
#if defined(USE_OPENGLES_20)

#include "renderers/Renderer.h"
#include "Log.h"
#include "Settings.h"
#include "math/Transform4x4f.h"
#include "math/Misc.h"

#include <SDL_opengles2.h>
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>

namespace Renderer
{
	static SDL_GLContext sdlContext = nullptr;

	// The GL entry points this renderer calls, as one table, because the list has to
	// be written three times -- a pointer, a cast and a name string -- and three
	// hand-written copies of forty lines is where a typo lives.
#define J36_GLES2_ENTRY_POINTS(X)                                                                          \
	X(const GLubyte*, GetString,               (GLenum))                                                   \
	X(GLenum,         GetError,                (void))                                                     \
	X(void,           Viewport,                (GLint, GLint, GLsizei, GLsizei))                           \
	X(void,           Scissor,                 (GLint, GLint, GLsizei, GLsizei))                           \
	X(void,           Enable,                  (GLenum))                                                   \
	X(void,           Disable,                 (GLenum))                                                   \
	X(void,           Clear,                   (GLbitfield))                                               \
	X(void,           ClearColor,              (GLclampf, GLclampf, GLclampf, GLclampf))                   \
	X(void,           BlendFunc,               (GLenum, GLenum))                                           \
	X(void,           ColorMask,               (GLboolean, GLboolean, GLboolean, GLboolean))               \
	X(void,           DepthMask,               (GLboolean))                                                \
	X(void,           StencilFunc,             (GLenum, GLint, GLuint))                                    \
	X(void,           StencilOp,               (GLenum, GLenum, GLenum))                                   \
	X(void,           StencilMask,             (GLuint))                                                   \
	X(void,           PixelStorei,             (GLenum, GLint))                                            \
	X(void,           GenTextures,             (GLsizei, GLuint*))                                         \
	X(void,           DeleteTextures,          (GLsizei, const GLuint*))                                   \
	X(void,           BindTexture,             (GLenum, GLuint))                                           \
	X(void,           TexImage2D,              (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
	X(void,           TexSubImage2D,           (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
	X(void,           TexParameterf,           (GLenum, GLenum, GLfloat))                                  \
	X(GLuint,         CreateShader,            (GLenum))                                                   \
	X(void,           ShaderSource,            (GLuint, GLsizei, const GLchar* const*, const GLint*))      \
	X(void,           CompileShader,           (GLuint))                                                   \
	X(void,           GetShaderiv,             (GLuint, GLenum, GLint*))                                   \
	X(void,           GetShaderInfoLog,        (GLuint, GLsizei, GLsizei*, GLchar*))                       \
	X(void,           DeleteShader,            (GLuint))                                                   \
	X(GLuint,         CreateProgram,           (void))                                                     \
	X(void,           AttachShader,            (GLuint, GLuint))                                           \
	X(void,           BindAttribLocation,      (GLuint, GLuint, const GLchar*))                            \
	X(void,           LinkProgram,             (GLuint))                                                   \
	X(void,           GetProgramiv,            (GLuint, GLenum, GLint*))                                   \
	X(void,           GetProgramInfoLog,       (GLuint, GLsizei, GLsizei*, GLchar*))                       \
	X(void,           DeleteProgram,           (GLuint))                                                   \
	X(void,           UseProgram,              (GLuint))                                                   \
	X(GLint,          GetUniformLocation,      (GLuint, const GLchar*))                                    \
	X(void,           Uniform1i,               (GLint, GLint))                                             \
	X(void,           UniformMatrix4fv,        (GLint, GLsizei, GLboolean, const GLfloat*))                \
	X(void,           EnableVertexAttribArray, (GLuint))                                                   \
	X(void,           VertexAttribPointer,     (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))   \
	X(void,           DrawArrays,              (GLenum, GLint, GLsizei))                              \
	X(void,           Finish,                  (void))                                                \
	X(void,           ReadPixels,              (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))

#define J36_GLES2_DECLARE(ret, name, args) static ret (GL_APIENTRY *p_gl##name) args = nullptr;
	J36_GLES2_ENTRY_POINTS(J36_GLES2_DECLARE)
#undef J36_GLES2_DECLARE

	// Attribute locations are bound before the link instead of being queried after
	// it, so all three programs agree and the arrays below are set up once.
	enum
	{
		ATTRIB_POS = 0,
		ATTRIB_TEX = 1,
		ATTRIB_COL = 2
	};

	struct Program
	{
		GLuint id   = 0;
		GLint  proj = -1;
		GLint  mv   = -1;
		GLint  tex  = -1;

	}; // Program

	static Program      programFlat;
	static Program      programRGBA;
	static Program      programAlpha;

	static Transform4x4f projection    = Transform4x4f::Identity();
	static Transform4x4f modelview     = Transform4x4f::Identity();

	static unsigned int  boundTexture  = 0;
	static Texture::Type boundType     = Texture::RGBA;
	static int           stencilBits   = 0;
	static bool          ready         = false;

	// A black panel has three causes that look identical from the outside: this
	// renderer drew nothing (no program, or geometry outside the frustum), ES asked
	// for nothing to be drawn, or everything was drawn and the buffers never reached
	// the CRTC. These four counters and J36_ES_GL_PROBE separate them; see selfTest()
	// and swapBuffers().
	static bool          probeMode     = false;
	static bool          selfTesting   = false;
	static unsigned long drawCalls     = 0;
	static unsigned long frames        = 0;
	static unsigned long drawsReported = 0;

	static std::map<unsigned int, Texture::Type> textureTypes;

	static const char* gles20_vs =
		"attribute vec2 a_pos;\n"
		"attribute vec2 a_tex;\n"
		"attribute vec4 a_col;\n"
		"uniform mat4 u_proj;\n"
		"uniform mat4 u_mv;\n"
		"varying vec2 v_tex;\n"
		"varying vec4 v_col;\n"
		"void main()\n"
		"{\n"
		"	v_tex = a_tex;\n"
		"	v_col = a_col;\n"
		"	gl_Position = u_proj * u_mv * vec4(a_pos, 0.0, 1.0);\n"
		"}\n";

	static const char* gles20_flat_fs =
		"varying vec2 v_tex;\n"
		"varying vec4 v_col;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = v_col;\n"
		"}\n";

	static const char* gles20_rgba_fs =
		"uniform sampler2D u_tex;\n"
		"varying vec2 v_tex;\n"
		"varying vec4 v_col;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = v_col * texture2D(u_tex, v_tex);\n"
		"}\n";

	// See the file header: an ALPHA texture leaves the fragment colour alone.
	static const char* gles20_alpha_fs =
		"uniform sampler2D u_tex;\n"
		"varying vec2 v_tex;\n"
		"varying vec4 v_col;\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = vec4(v_col.rgb, v_col.a * texture2D(u_tex, v_tex).a);\n"
		"}\n";

	// GLSL ES 1.00 has no default precision for float in a fragment shader and
	// refuses to compile without one; desktop GLSL 1.20 has no precision keyword at
	// all. Which of the two this context speaks is read off GL_VERSION, so one set
	// of sources compiles on either context this stack can produce.
	//
	// It goes on the vertex shader as well, which is not decoration. In GLSL ES 1.00
	// the default float precision is highp in a vertex shader and undeclared in a
	// fragment shader, so declaring mediump in only one of them leaves v_tex and
	// v_col with a different precision on each side of the link -- and ESSL 1.00
	// requires a varying's qualifiers to match, so a strict linker is entitled to
	// reject the program. A rejected program is a black panel and one line in a log,
	// which is exactly the failure that is hardest to tell from the others. Both
	// sides at mediump also matches the hardware: Utgard's fragment core is mediump.
	static std::string shaderPrologue;

	static const char* safeGetString(const GLenum _name)
	{
		const GLubyte* str = p_glGetString ? p_glGetString(_name) : nullptr;
		return str ? (const char*)str : "";

	} // safeGetString

	static GLuint compileShader(const GLenum _type, const std::string& _source)
	{
		const GLuint  shader = p_glCreateShader(_type);
		const GLchar* text   = _source.c_str();
		GLint         status = GL_FALSE;

		p_glShaderSource(shader, 1, &text, nullptr);
		p_glCompileShader(shader);
		p_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

		if(status != GL_TRUE)
		{
			char log[1024] = { 0 };
			p_glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
			LOG(LogError) << "GLES2: shader would not compile:\n" << log << "\nsource:\n" << _source;
			p_glDeleteShader(shader);
			return 0;
		}

		return shader;

	} // compileShader

	static bool buildProgram(Program& _program, const GLuint _vs, const char* _fs, const bool _textured)
	{
		const GLuint fs     = compileShader(GL_FRAGMENT_SHADER, shaderPrologue + _fs);
		GLint        status = GL_FALSE;

		if(fs == 0)
			return false;

		_program.id = p_glCreateProgram();
		p_glAttachShader(_program.id, _vs);
		p_glAttachShader(_program.id, fs);
		p_glBindAttribLocation(_program.id, ATTRIB_POS, "a_pos");
		p_glBindAttribLocation(_program.id, ATTRIB_TEX, "a_tex");
		p_glBindAttribLocation(_program.id, ATTRIB_COL, "a_col");
		p_glLinkProgram(_program.id);
		p_glGetProgramiv(_program.id, GL_LINK_STATUS, &status);
		p_glDeleteShader(fs);

		if(status != GL_TRUE)
		{
			char log[1024] = { 0 };
			p_glGetProgramInfoLog(_program.id, sizeof(log) - 1, nullptr, log);
			LOG(LogError) << "GLES2: program would not link:\n" << log;
			p_glDeleteProgram(_program.id);
			_program.id = 0;
			return false;
		}

		_program.proj = p_glGetUniformLocation(_program.id, "u_proj");
		_program.mv   = p_glGetUniformLocation(_program.id, "u_mv");
		_program.tex  = _textured ? p_glGetUniformLocation(_program.id, "u_tex") : -1;

		return true;

	} // buildProgram

	static bool buildShaders()
	{
		const GLuint vs = compileShader(GL_VERTEX_SHADER, shaderPrologue + gles20_vs);

		if(vs == 0)
			return false;

		const bool ok = buildProgram(programFlat,  vs, gles20_flat_fs,  false) &&
		                buildProgram(programRGBA,  vs, gles20_rgba_fs,  true)  &&
		                buildProgram(programAlpha, vs, gles20_alpha_fs, true);

		p_glDeleteShader(vs);

		return ok;

	} // buildShaders

	static const Program& currentProgram()
	{
		if(boundTexture == 0)
			return programFlat;

		return (boundType == Texture::ALPHA) ? programAlpha : programRGBA;

	} // currentProgram

	static GLenum convertBlendFactor(const Blend::Factor _blendFactor)
	{
		switch(_blendFactor)
		{
			case Blend::ZERO:                { return GL_ZERO;                } break;
			case Blend::ONE:                 { return GL_ONE;                 } break;
			case Blend::SRC_COLOR:           { return GL_SRC_COLOR;           } break;
			case Blend::ONE_MINUS_SRC_COLOR: { return GL_ONE_MINUS_SRC_COLOR; } break;
			case Blend::SRC_ALPHA:           { return GL_SRC_ALPHA;           } break;
			case Blend::ONE_MINUS_SRC_ALPHA: { return GL_ONE_MINUS_SRC_ALPHA; } break;
			case Blend::DST_COLOR:           { return GL_DST_COLOR;           } break;
			case Blend::ONE_MINUS_DST_COLOR: { return GL_ONE_MINUS_DST_COLOR; } break;
			case Blend::DST_ALPHA:           { return GL_DST_ALPHA;           } break;
			case Blend::ONE_MINUS_DST_ALPHA: { return GL_ONE_MINUS_DST_ALPHA; } break;
			default:                         { return GL_ZERO;                }
		}

	} // convertBlendFactor

	static GLenum convertTextureType(const Texture::Type _type)
	{
		switch(_type)
		{
			case Texture::RGBA:  { return GL_RGBA;  } break;
			case Texture::ALPHA: { return GL_ALPHA; } break;
			default:             { return GL_ZERO;  }
		}

	} // convertTextureType

	// One path for both primitive types: bind the program the current texture
	// implies, hand it the two matrices, point the three arrays at the caller's
	// vertices. GLES 2.0 takes client-side arrays, so this is the same shape the
	// fixed-function renderers had, one glVertexAttribPointer per glVertexPointer.
	static void drawClientArrays(const GLenum _mode, const Vertex* _vertices, const unsigned int _numVertices, const Blend::Factor _srcBlendFactor, const Blend::Factor _dstBlendFactor)
	{
		if(!ready)
			return;

		const Program& program = currentProgram();

		// Once, on the first draw of the process: how many vertices, where the first
		// one is in ES's own pixel coordinates, and what the projection is doing to
		// it. The two scales and the two translations are enough to recognise the
		// classic shader-port failure -- a projection left at identity, which puts
		// every one of ES's 0..640 coordinates hundreds of units outside the frustum
		// and clips the entire UI away without one GL error being raised.
		if(++drawCalls == 1 && !selfTesting)
		{
			const float* p = (const float*)&projection;
			char         line[192];

			snprintf(line, sizeof(line),
				"GLES2: first draw, program %u, %u verts, v0 (%.1f, %.1f), proj sx %.5f sy %.5f tx %.2f ty %.2f",
				(unsigned int)program.id, _numVertices,
				(double)_vertices[0].pos.x(), (double)_vertices[0].pos.y(),
				(double)p[0], (double)p[5], (double)p[12], (double)p[13]);

			LOG(LogInfo) << line;
		}

		p_glEnable(GL_BLEND);
		p_glBlendFunc(convertBlendFactor(_srcBlendFactor), convertBlendFactor(_dstBlendFactor));

		p_glUseProgram(program.id);
		p_glUniformMatrix4fv(program.proj, 1, GL_FALSE, (const GLfloat*)&projection);
		p_glUniformMatrix4fv(program.mv,   1, GL_FALSE, (const GLfloat*)&modelview);

		if(program.tex != -1)
			p_glUniform1i(program.tex, 0);

		p_glVertexAttribPointer(ATTRIB_POS, 2, GL_FLOAT,         GL_FALSE, sizeof(Vertex), &_vertices[0].pos);
		p_glVertexAttribPointer(ATTRIB_TEX, 2, GL_FLOAT,         GL_FALSE, sizeof(Vertex), &_vertices[0].tex);
		p_glVertexAttribPointer(ATTRIB_COL, 4, GL_UNSIGNED_BYTE, GL_TRUE,  sizeof(Vertex), &_vertices[0].col);

		p_glDrawArrays(_mode, 0, _numVertices);

		p_glDisable(GL_BLEND);

	} // drawClientArrays

	// Draw one magenta quad straight in NDC, with both matrices at identity, and read
	// the centre pixel back. It runs once, before ES has drawn anything, and it is
	// the only measurement that separates the three faults behind a black panel:
	//
	//   pixel is magenta          the context, the shaders, the attribute arrays and
	//                             the draw all work. Whatever is black after this is
	//                             ES's geometry, ES's own logic, or the scanout.
	//   pixel is black, no error  the pipeline swallows the draw. The compile and
	//                             link lines above say why.
	//   glGetError is set         named here rather than at the next unrelated call.
	//
	// It costs one clear and one four-vertex draw at startup, and Renderer::init()
	// clears and swaps immediately afterwards, so nothing of it survives to frame 1.
	// glReadPixels of a single pixel is a full pipeline flush on a tiler, which is
	// exactly why it is here and not in the frame loop.
	static void selfTest()
	{
		const int w = getWindowWidth();
		const int h = getWindowHeight();

		// 0xFFFF00FF is ABGR, which is what convertColor() produces and what the
		// GL_UNSIGNED_BYTE/GL_TRUE colour array reads back as r,g,b,a in memory.
		Vertex        quad[4];
		unsigned char px[4] = { 0, 0, 0, 0 };
		char          line[192];

		quad[0] = Vertex(Vector2f(-1.0f, -1.0f), Vector2f(0.0f, 0.0f), 0xFFFF00FF);
		quad[1] = Vertex(Vector2f(-1.0f,  1.0f), Vector2f(0.0f, 0.0f), 0xFFFF00FF);
		quad[2] = Vertex(Vector2f( 1.0f, -1.0f), Vector2f(0.0f, 0.0f), 0xFFFF00FF);
		quad[3] = Vertex(Vector2f( 1.0f,  1.0f), Vector2f(0.0f, 0.0f), 0xFFFF00FF);

		projection = Transform4x4f::Identity();
		modelview  = Transform4x4f::Identity();

		p_glViewport(0, 0, w, h);
		p_glDisable(GL_SCISSOR_TEST);
		p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		p_glClear(GL_COLOR_BUFFER_BIT);

		bindTexture(0);
		selfTesting = true;
		drawClientArrays(GL_TRIANGLE_STRIP, quad, 4, Blend::ONE, Blend::ZERO);
		selfTesting = false;

		p_glFinish();
		p_glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

		snprintf(line, sizeof(line),
			"GLES2: self test %dx%d, centre pixel %02x %02x %02x %02x (expect ff 00 ff ..), glGetError 0x%04x",
			w, h, px[0], px[1], px[2], px[3], (unsigned int)p_glGetError());

		LOG(LogInfo) << line;

		if(px[0] != 0xFF || px[1] != 0x00 || px[2] != 0xFF)
		{
			LOG(LogError) << "GLES2: the self test quad did not reach the framebuffer -- nothing this renderer draws will either";
		}

		// Back to the state createContext() would have left, and drop the counter so
		// the "first draw" line below reports ES's first draw and not this one.
		drawCalls = 0;
		p_glClear(GL_COLOR_BUFFER_BIT);

	} // selfTest

	unsigned int convertColor(const unsigned int _color)
	{
		// convert from rgba to abgr
		unsigned char r = ((_color & 0xff000000) >> 24) & 255;
		unsigned char g = ((_color & 0x00ff0000) >> 16) & 255;
		unsigned char b = ((_color & 0x0000ff00) >>  8) & 255;
		unsigned char a = ((_color & 0x000000ff)      ) & 255;

		return ((a << 24) | (b << 16) | (g << 8) | (r));

	} // convertColor

	unsigned int getWindowFlags()
	{
		return SDL_WINDOW_OPENGL;

	} // getWindowFlags

	void setupWindow()
	{
		// Read here rather than in createContext() because this is the last hook
		// before SDL_CreateWindow, and the KMSDRM backend's own account of the
		// modeset -- which connector, which CRTC, which plane format, and what
		// drmModeAddFB2 or drmModePageFlip said if it refused -- is all logged during
		// that call. SDL's default priority hides everything below SDL_LOG_ERROR, and
		// ES never raises it, so that account is thrown away on a normal run. It is
		// the half of a black panel that no amount of GL instrumentation can see.
		probeMode = (getenv("J36_ES_GL_PROBE") != nullptr);

		if(probeMode)
			SDL_LogSetAllPriority(SDL_LOG_PRIORITY_VERBOSE);

		// Asked for explicitly, all of it. The fixed-function renderers set
		// SDL_GL_CONTEXT_MAJOR_VERSION twice -- 1 then 0, and 2 then 1 -- which is a
		// typo for MINOR in both, and a major version of 0 makes SDL send no context
		// attributes at all, so EGL falls back to its own default of desktop GL. On
		// this board that is the one API of the three that has nothing behind it.
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

		// Alpha is not optional here, and not because anything reads it: SDL's
		// KMSDRM backend hardcodes GBM_FORMAT_ARGB8888 for its gbm surface
		// (SDL_kmsdrmvideo.c:1197) and then pins that visual with
		// SDL_EGL_SetRequiredVisualId, so a config with no alpha channel is a config
		// SDL will not accept for this window whatever is asked for here. Which also
		// means the framebuffer that gets scanned out has an alpha channel that this
		// renderer is responsible for -- see the clear colour in createContext().
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE,     8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,   8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,    8);
		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,   8);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  24);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	} // setupWindow

	void createContext()
	{
		sdlContext = SDL_GL_CreateContext(getSDLWindow());

		// Renderer::createWindow() does not check this, and the fixed-function
		// renderers went straight on to std::string(glGetString(GL_EXTENSIONS)) --
		// which is a std::string constructed from nullptr, i.e. the abort that put
		// status 134 in the journal instead of a reason. Say the reason.
		if(sdlContext == nullptr)
		{
			LOG(LogError) << "Error creating an OpenGL ES 2.0 context!\n\t" << SDL_GetError();
			return;
		}

		if(SDL_GL_MakeCurrent(getSDLWindow(), sdlContext) != 0)
		{
			LOG(LogError) << "Error making the OpenGL ES 2.0 context current!\n\t" << SDL_GetError();
			return;
		}

		std::string missing;

#define J36_GLES2_LOAD(ret, name, args)                                                        \
		p_gl##name = (ret (GL_APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name);               \
		if(p_gl##name == nullptr) missing += " gl" #name;
		J36_GLES2_ENTRY_POINTS(J36_GLES2_LOAD)
#undef J36_GLES2_LOAD

		if(!missing.empty())
		{
			LOG(LogError) << "GLES2: the context is missing entry points:" << missing;
			return;
		}

		const std::string version  = safeGetString(GL_VERSION);
		const std::string renderer = safeGetString(GL_RENDERER);
		const std::string exts     = safeGetString(GL_EXTENSIONS);

		LOG(LogInfo) << "GLES2: " << renderer << ", " << version;
		LOG(LogInfo) << "GLES2: non-power-of-two textures: "
			<< ((exts.find("texture_npot") != std::string::npos || exts.find("texture_non_power_of_two") != std::string::npos) ? "ok" : "MISSING");

		shaderPrologue = (version.find("OpenGL ES") != std::string::npos) ? "precision mediump float;\n" : "";

		if(!buildShaders())
			return;

		// Asked of SDL rather than assumed, because a config with 8 stencil bits is
		// a request and not a guarantee: SDL scores configs by distance and will
		// hand back one without stencil if that is the closest the driver has.
		// enableRoundCornerStencil() checks this and draws square corners instead of
		// masking against a buffer that is not there.
		SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencilBits);
		if(stencilBits == 0)
		{
			LOG(LogInfo) << "GLES2: no stencil buffer in the chosen config, corners will not be rounded";
		}

		p_glEnableVertexAttribArray(ATTRIB_POS);
		p_glEnableVertexAttribArray(ATTRIB_TEX);
		p_glEnableVertexAttribArray(ATTRIB_COL);

		ready = true;

		selfTest();

		// The other half of J36_ES_GL_PROBE (setupWindow() read it): a magenta clear
		// colour, which answers the one question a log cannot. A magenta panel means
		// the buffers this renderer swaps are the buffers the panel scans out, so
		// anything still black on top of it is ES's own drawing. A panel that stays
		// black while the self test above passed means the frames are correct and
		// never reach the CRTC. /init sets it under j36.es=debug only -- on a board
		// that works this would be a magenta flash at every startup.
		if(probeMode)
		{
			LOG(LogInfo) << "GLES2: J36_ES_GL_PROBE is set, clearing to magenta instead of black";
			p_glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
		}
		else
			// Opaque black, and the fourth argument is the whole point of the line.
			// The other renderers in this tree clear to (0, 0, 0, 0) and get away with
			// it because a desktop compositor throws destination alpha away; here the
			// buffer this clears IS the scanout buffer, and it is ARGB8888 because
			// SDL's KMSDRM backend allows nothing else. On a display controller that
			// blends per-pixel alpha against its background -- which MT6592's OVL does
			// for an AR24 layer -- a frame cleared to alpha 0 is composited to the
			// background colour, and a correct frame that has been blended away is
			// indistinguishable from a frame that never arrived. Whether this board
			// blends it is what `eglprobe -p' phase 3 measures; alpha 1 is right either
			// way, since nothing downstream of ES wants a transparent UI.
			p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	} // createContext

	void destroyContext()
	{
		if(ready)
		{
			p_glDeleteProgram(programFlat.id);
			p_glDeleteProgram(programRGBA.id);
			p_glDeleteProgram(programAlpha.id);
		}

		programFlat  = Program();
		programRGBA  = Program();
		programAlpha = Program();
		ready        = false;

		SDL_GL_DeleteContext(sdlContext);
		sdlContext = nullptr;

	} // destroyContext

	unsigned int createTexture(const Texture::Type _type, const bool _linear, const bool _repeat, const unsigned int _width, const unsigned int _height, void* _data)
	{
		if(!ready)
			return 0;

		const GLenum type = convertTextureType(_type);
		unsigned int texture;

		p_glGenTextures(1, &texture);
		textureTypes[texture] = _type;
		bindTexture(texture);

		p_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, _repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
		p_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, _repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);

		p_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		p_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, _linear ? GL_LINEAR : GL_NEAREST);

		p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
		p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		p_glTexImage2D(GL_TEXTURE_2D, 0, type, _width, _height, 0, type, GL_UNSIGNED_BYTE, _data);

		return texture;

	} // createTexture

	void destroyTexture(const unsigned int _texture)
	{
		if(!ready)
			return;

		textureTypes.erase(_texture);
		p_glDeleteTextures(1, &_texture);

	} // destroyTexture

	void updateTexture(const unsigned int _texture, const Texture::Type _type, const unsigned int _x, const unsigned _y, const unsigned int _width, const unsigned int _height, void* _data)
	{
		if(!ready)
			return;

		textureTypes[_texture] = _type;
		bindTexture(_texture);

		if(_x == (unsigned int)-1 && _y == (unsigned int)-1)
		{
			const GLenum type = convertTextureType(_type);
			p_glTexImage2D(GL_TEXTURE_2D, 0, type, _width, _height, 0, type, GL_UNSIGNED_BYTE, _data);
		}
		else
			p_glTexSubImage2D(GL_TEXTURE_2D, 0, _x, _y, _width, _height, convertTextureType(_type), GL_UNSIGNED_BYTE, _data);

		bindTexture(0);

	} // updateTexture

	void bindTexture(const unsigned int _texture)
	{
		if(!ready)
			return;

		p_glBindTexture(GL_TEXTURE_2D, _texture);

		// Which program the next draw uses, in place of the GL_TEXTURE_2D enable the
		// fixed-function renderers toggled here.
		boundTexture = _texture;

		if(_texture != 0)
		{
			const std::map<unsigned int, Texture::Type>::const_iterator it = textureTypes.find(_texture);
			boundType = (it != textureTypes.cend()) ? it->second : Texture::RGBA;
		}

	} // bindTexture

	void drawLines(const Vertex* _vertices, const unsigned int _numVertices, const Blend::Factor _srcBlendFactor, const Blend::Factor _dstBlendFactor)
	{
		drawClientArrays(GL_LINES, _vertices, _numVertices, _srcBlendFactor, _dstBlendFactor);

	} // drawLines

	void drawTriangleStrips(const Vertex* _vertices, const unsigned int _numVertices, const Blend::Factor _srcBlendFactor, const Blend::Factor _dstBlendFactor)
	{
		drawClientArrays(GL_TRIANGLE_STRIP, _vertices, _numVertices, _srcBlendFactor, _dstBlendFactor);

	} // drawTriangleStrips

	void setProjection(const Transform4x4f& _projection)
	{
		projection = _projection;

	} // setProjection

	void setMatrix(const Transform4x4f& _matrix)
	{
		modelview = _matrix;
		modelview.round();

	} // setMatrix

	void setViewport(const Rect& _viewport)
	{
		if(!ready)
			return;

		// glViewport starts at the bottom left of the window
		p_glViewport(_viewport.x, getWindowHeight() - _viewport.y - _viewport.h, _viewport.w, _viewport.h);

	} // setViewport

	void setScissor(const Rect& _scissor)
	{
		if(!ready)
			return;

		if((_scissor.x == 0) && (_scissor.y == 0) && (_scissor.w == 0) && (_scissor.h == 0))
		{
			p_glDisable(GL_SCISSOR_TEST);
		}
		else
		{
			// glScissor starts at the bottom left of the window
			p_glScissor(_scissor.x, getWindowHeight() - _scissor.y - _scissor.h, _scissor.w, _scissor.h);
			p_glEnable(GL_SCISSOR_TEST);
		}

	} // setScissor

	void setSwapInterval()
	{
		// vsync
		if(Settings::getInstance()->getBool("VSync"))
		{
			// SDL_GL_SetSwapInterval(0) for immediate updates (no vsync, default),
			// 1 for updates synchronized with the vertical retrace,
			// or -1 for late swap tearing.
			// SDL_GL_SetSwapInterval returns 0 on success, -1 on error.
			// if vsync is requested, try normal vsync; if that doesn't work, try late swap tearing
			// if that doesn't work, report an error
			if(SDL_GL_SetSwapInterval(1) != 0 && SDL_GL_SetSwapInterval(-1) != 0)
				LOG(LogWarning) << "Tried to enable vsync, but failed! (" << SDL_GetError() << ")";
		}
		else
			SDL_GL_SetSwapInterval(0);

	} // setSwapInterval

	void swapBuffers()
	{
		SDL_GL_SwapWindow(getSDLWindow());

		// The first three frames and then one line every ten seconds or so. This is
		// the counter that says whether a black panel is this renderer's fault at all:
		// a frame with zero draws is ES deciding there is nothing to show, which is a
		// theme or a gamelist, not a shader.
		++frames;

		if(frames <= 3 || (frames % 600) == 0)
		{
			char line[128];

			snprintf(line, sizeof(line), "GLES2: frame %lu, %lu draws since the last line",
				frames, drawCalls - drawsReported);

			drawsReported = drawCalls;
			LOG(LogInfo) << line;
		}

		if(ready)
			p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	} // swapBuffers

#define ROUNDING_PIECES 8.0f

	void drawGLRoundedCorner(float x, float y, double sa, double arc, float r, unsigned int color, std::vector<Vertex> &vertex)
	{
		// centre of the arc, for clockwise sense
		float cent_x = x + r * Math::cosf(sa + ES_PI / 2.0f);
		float cent_y = y + r * Math::sinf(sa + ES_PI / 2.0f);

		// build up piecemeal including end of the arc
		int n = ceil(ROUNDING_PIECES * arc / ES_PI * 2.0f);
		for (int i = 0; i <= n; i++)
		{
			float ang = sa + arc * (double)i / (double)n;

			// compute the next point
			float next_x = cent_x + r * Math::sinf(ang);
			float next_y = cent_y - r * Math::cosf(ang);

			Vertex vx;
			vx.pos = Vector2f(next_x, next_y);
			vx.tex = Vector2f(0, 0);
			vx.col = color;
			vertex.push_back(vx);
		}
	}

	void drawRoundRect(float x, float y, float width, float height, float radius, unsigned int color, const Blend::Factor _srcBlendFactor, const Blend::Factor _dstBlendFactor)
	{
		auto finalColor = convertColor(color);

		std::vector<Vertex> vertex;
		drawGLRoundedCorner(x, y + radius, 3.0f * ES_PI / 2.0f, ES_PI / 2.0f, radius, finalColor, vertex);
		drawGLRoundedCorner(x + width - radius, y, 0.0, ES_PI / 2.0f, radius, finalColor, vertex);
		drawGLRoundedCorner(x + width, y + height - radius, ES_PI / 2.0f, ES_PI / 2.0f, radius, finalColor, vertex);
		drawGLRoundedCorner(x + radius, y + height, ES_PI, ES_PI / 2.0f, radius, finalColor, vertex);

		if(vertex.empty())
			return;

		bindTexture(0);
		drawClientArrays(GL_TRIANGLE_FAN, vertex.data(), (unsigned int)vertex.size(), _srcBlendFactor, _dstBlendFactor);
	}

	void enableRoundCornerStencil(float x, float y, float width, float height, float radius)
	{
		if(!ready || stencilBits == 0)
			return;

		const unsigned int texture = boundTexture;

		p_glClear(GL_DEPTH_BUFFER_BIT);
		p_glEnable(GL_STENCIL_TEST);
		p_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		p_glDepthMask(GL_FALSE);
		p_glStencilFunc(GL_NEVER, 1, 0xFF);
		p_glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);

		p_glStencilMask(0xFF);
		p_glClear(GL_STENCIL_BUFFER_BIT);

		drawRoundRect(x, y, width, height, radius, 0xFFFFFFFF);

		p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		p_glDepthMask(GL_TRUE);
		p_glStencilMask(0x00);
		p_glStencilFunc(GL_EQUAL, 1, 0xFF);

		bindTexture(texture);
	}

	void disableStencil()
	{
		if(!ready || stencilBits == 0)
			return;

		p_glDisable(GL_STENCIL_TEST);
	}
} // Renderer::

#endif // USE_OPENGLES_20
