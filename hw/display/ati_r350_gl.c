/*
 * ATI R300/R350 -- the OpenGL 3.3 core rendering backend.
 *
 * One request in, one rectangle of RGBA8 out. There is no window, no
 * QEMU display backend and no interaction with the console: the target
 * is an offscreen FBO, and the caller puts the result back into
 * emulated VRAM, where the existing scanout displays it exactly as it
 * displays software-rasterized pixels. That is deliberate --
 * CONFIG_OPENGL cannot even be enabled on macOS today (libepoxy on
 * darwin ships no epoxy/egl.h, and QEMU's own blit shaders are
 * `#version 300 es`, which desktop GL rejects), so nothing here may
 * depend on it.
 *
 * There are two hosts with a real backend, darwin (CGL) and Windows
 * (WGL), and the difference between them is confined to the platform
 * seam below: a context, and on Windows the loading of the GL entry
 * points. Everything else in this file is GL 3.3 core and shared. That
 * independence from CONFIG_OPENGL and from any display backend is what
 * makes a second leg thirty lines of context creation rather than a
 * port, and the same reasoning kept GL_NV_texture_barrier out (see the
 * M3 note below).
 *
 * The shaders are the ones phase-2 milestone M1 validated offline in
 * doc/radeon9800/gl-replay/ against the software rasterizer, on a
 * corpus of real captured draws: interior pixels 99.9992 % at delta 0
 * with a maximum channel delta of 1. Three things in them look odd and
 * are load-bearing, all measured rather than assumed:
 *
 *   - `precise` and the explicit `fma()` calls. ati_r350_3d.c is C at
 *     -O2 and the compiler contracts its multiply-adds; a fused product
 *     carries no intermediate rounding, so reproducing the software
 *     path bit for bit means reproducing the fusion. Rebuilding the M1
 *     harness with -ffp-contract=off made its oracle stop reproducing
 *     the device at all (106 of 150 records), which is the proof.
 *   - the triangle's reciprocal 1/area comes from the HOST as a flat
 *     attribute. A GPU fp32 divide is not required to be correctly
 *     rounded and one ULP there moves a texel boundary by a whole texel.
 *   - both samplers are INTEGER samplers indexed into a host-built
 *     table of k/255.0f, because neither GL's unorm-to-float conversion
 *     nor GLSL division is required to be correctly rounded either.
 *
 * The blend is computed in the fragment shader rather than by
 * glBlendFunc for the same reason: the device packs by TRUNCATION and
 * GL's blender rounds to nearest, which M1 measured as 311371 of 638826
 * pixels differing by exactly 1/255 -- half of every blended surface.
 *
 * Milestone M3 made the colour buffer an INTEGER (GL_RGBA8UI) texture
 * and the fragment output a uvec4. Three things follow, and all three
 * are why it was done:
 *
 *   - the destination the blend samples is refreshed with
 *     glCopyTexSubImage2D, which is legal only between matching format
 *     classes and which this host runs at 0.074 ms for a whole 1024x768
 *     surface. That is what lets a self-overlapping blended draw be
 *     ORDERED on the GPU rather than handed back to the software
 *     rasterizer. GL 4.1 has no ARB_texture_barrier; this host does
 *     offer GL_NV_texture_barrier, which would let the colour buffer be
 *     sampled directly and save the copy, and it is deliberately not
 *     used: the copy costs 0.074 ms against roughly 0.16 ms of
 *     per-pass overhead measured in total, and a vendor extension that
 *     no Windows GL or ANGLE path is promised to have is a poor thing
 *     for correctness to rest on.
 *   - it is also what lets the render target STAY on the GPU across
 *     draws, so the destination is neither uploaded nor read back per
 *     draw. In the same bench a 1024x768 readback taken away from a
 *     draw costs 0.402 ms against the 2.8 ms it costs immediately
 *     after one.
 *   - the truncating pack stops round-tripping through a normalized
 *     format: the shader writes the byte the device would have written.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ati_r350_gl.h"
#include <float.h>
#include <math.h>

#if defined(CONFIG_DARWIN) || defined(_WIN32)

/*
 * THE PLATFORM SEAM.
 *
 * Everything below this block down to the stub at the end of the file is
 * shared and is plain GL 3.3 core. What differs per host is only the
 * CONTEXT -- how one is created, made current on the thread that is
 * calling, and destroyed -- plus, on Windows, where the GL entry points
 * come from. Each leg defines exactly this much:
 *
 *   R350_GL_BACKEND_NAME              what `qom-get gl` calls it
 *   R350GlPlat                        the context, with a `ctx` member
 *   r350_gl_plat_open(pl, err)        create one and make it current
 *   r350_gl_plat_close(pl)            destroy it
 *   r350_gl_makecurrent(pl)           take it for this thread
 *   r350_gl_done(pl)                  give it back
 *   r350_gl_plat_has_barrier()        a texture barrier is offered
 *   r350_gl_barrier()                 issue it
 *   r350_gl_plat_hold(hold)           this thread keeps the context
 *                                     between entry points
 *   r350_gl_plat_unbind(pl)           ... until it gives it back here
 *
 * r350_gl_done() and the last two are the members that are not obvious,
 * and they exist because WGL needs them: see the threading comment in the
 * win32 leg. On darwin they are nothing at all.
 */

#ifdef CONFIG_DARWIN

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>

#define R350_GL_BACKEND_NAME "CGL offscreen"

typedef struct R350GlPlat {
    CGLContextObj ctx;
} R350GlPlat;

static bool r350_gl_plat_open(R350GlPlat *pl, const char **err)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAAccelerated,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;

    if (CGLChoosePixelFormat(attrs, &pix, &npix) || !pix) {
        *err = "no accelerated offscreen GL 3.3 core pixel format";
        return false;
    }
    if (CGLCreateContext(pix, NULL, &pl->ctx)) {
        CGLDestroyPixelFormat(pix);
        *err = "CGLCreateContext failed";
        return false;
    }
    CGLDestroyPixelFormat(pix);
    CGLSetCurrentContext(pl->ctx);
    return true;
}

static void r350_gl_plat_close(R350GlPlat *pl)
{
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(pl->ctx);
    pl->ctx = NULL;
}

static inline void r350_gl_makecurrent(R350GlPlat *pl)
{
    CGLSetCurrentContext(pl->ctx);
}

/*
 * Nothing. A CGL context stays current on the thread that set it, and
 * CGLSetCurrentContext on a second thread simply takes it over -- so
 * handing it back between entry points would be work for its own sake.
 */
static inline void r350_gl_done(R350GlPlat *pl)
{
    (void)pl;
}

static bool r350_gl_plat_has_barrier(void)
{
    GLint n = 0, i;

    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);

        if (e && !strcmp(e, "GL_NV_texture_barrier")) {
            return true;
        }
    }
    return false;
}

static inline void r350_gl_barrier(void)
{
    glTextureBarrierNV();
}

static inline void r350_gl_plat_hold(bool hold)
{
}

static inline void r350_gl_plat_unbind(R350GlPlat *pl)
{
}

#else /* _WIN32 */

/*
 * Windows has neither half of what the darwin leg gets for free, so both
 * are hand rolled here.
 *
 * THE CONTEXT is raw WGL on an invisible window. SDL is in the Windows
 * build and would have been the shorter road, but it solves neither of
 * the two problems this backend actually has. SDL_GL_MakeCurrent is a
 * thin wrapper over wglMakeCurrent and inherits its cross-thread rule
 * word for word, so the hard part below is not made any easier by it.
 * And SDL_CreateWindow needs SDL's video subsystem, which QEMU's own SDL
 * display owns: ui/sdl2.c calls SDL_Init(SDL_INIT_VIDEO) in
 * sdl_display_init() and SDL_QuitSubSystem(SDL_INIT_VIDEO) when it shuts
 * down, which would take this window away from a running guest, and
 * under -display gtk/vnc/none it is never initialised at all. SDL is
 * also an optional dependency (meson.build: `required: get_option(sdl)`,
 * auto), so binding a device's rendering to it would make gl=fast appear
 * and disappear with a UI package -- exactly the coupling the header
 * comment above exists to avoid.
 *
 * THE ENTRY POINTS come from wglGetProcAddress, because opengl32.dll
 * exports GL 1.1 and nothing later. The pointers below are named exactly
 * like the functions they hold, so the shared code calls them without
 * knowing they are pointers. No GL header is included at all: the types
 * and enums a 3.3 core context needs are spelled out, and the build then
 * depends on nothing but windows.h, which qemu/osdep.h has already
 * pulled in.
 */

#define R350_GL_BACKEND_NAME "WGL offscreen"

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE                        0
#define GL_ZERO                         0
#define GL_TRUE                         1
#define GL_NO_ERROR                     0
#define GL_ONE                          1
#define GL_NEVER                        0x0200
#define GL_LESS                         0x0201
#define GL_EQUAL                        0x0202
#define GL_LEQUAL                       0x0203
#define GL_GREATER                      0x0204
#define GL_NOTEQUAL                     0x0205
#define GL_GEQUAL                       0x0206
#define GL_ALWAYS                       0x0207
#define GL_FRONT                        0x0404
#define GL_BACK                         0x0405
#define GL_FRONT_AND_BACK               0x0408
#define GL_TRIANGLES                    0x0004
#define GL_CULL_FACE                    0x0B44
#define GL_DEPTH_TEST                   0x0B71
#define GL_STENCIL_TEST                 0x0B90
#define GL_BLEND                        0x0BE2
#define GL_SCISSOR_TEST                 0x0C11
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_TEXTURE_2D                   0x0DE1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_INVERT                       0x150A
#define GL_KEEP                         0x1E00
#define GL_REPLACE                      0x1E01
#define GL_INCR                         0x1E02
#define GL_DECR                         0x1E03
#define GL_INCR_WRAP                    0x8507
#define GL_DECR_WRAP                    0x8508
#define GL_DEPTH_STENCIL                0x84F9
#define GL_DEPTH_STENCIL_ATTACHMENT     0x821A
#define GL_DEPTH32F_STENCIL8            0x8CAD
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#define GL_VERSION                      0x1F02
#define GL_EXTENSIONS                   0x1F03
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_NEAREST_MIPMAP_NEAREST       0x2700
#define GL_TEXTURE_BASE_LEVEL           0x813C
#define GL_TEXTURE_MAX_LEVEL            0x813D
#define GL_FUNC_ADD                     0x8006
#define GL_RGBA8                        0x8058
#define GL_TEXTURE0                     0x84C0
#define GL_TEXTURE1                     0x84C1
#define GL_TEXTURE2                     0x84C2
#define GL_ARRAY_BUFFER                 0x8892
#define GL_STREAM_DRAW                  0x88E0
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_VERTEX_SHADER                0x8B31
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_SHADING_LANGUAGE_VERSION     0x8B8C
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER                  0x8D40
#define GL_RGBA8UI                      0x8D7C
#define GL_RGBA_INTEGER                 0x8D99
#define GL_NUM_EXTENSIONS               0x821D
#define GL_RGBA32F                      0x8814
#define GL_TEXTURE_BUFFER               0x8C2A
#define GL_MAX_TEXTURE_BUFFER_SIZE      0x8C2B

/* GL 1.1: opengl32.dll's own exports */
static void (APIENTRY *glBindTexture)(GLenum, GLuint);
static void (APIENTRY *glBlendFunc)(GLenum, GLenum);
static void (APIENTRY *glColorMask)(GLboolean, GLboolean, GLboolean,
                                    GLboolean);
static void (APIENTRY *glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                            GLint, GLint, GLsizei, GLsizei);
static void (APIENTRY *glDeleteTextures)(GLsizei, const GLuint *);
static void (APIENTRY *glDepthFunc)(GLenum);
static void (APIENTRY *glDepthMask)(GLboolean);
static void (APIENTRY *glDisable)(GLenum);
static void (APIENTRY *glDrawArrays)(GLenum, GLint, GLsizei);
static void (APIENTRY *glEnable)(GLenum);
static void (APIENTRY *glGenTextures)(GLsizei, GLuint *);
static GLenum (APIENTRY *glGetError)(void);
static void (APIENTRY *glGetIntegerv)(GLenum, GLint *);
static const GLubyte *(APIENTRY *glGetString)(GLenum);
static void (APIENTRY *glPixelStorei)(GLenum, GLint);
static void (APIENTRY *glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                     GLenum, void *);
static void (APIENTRY *glScissor)(GLint, GLint, GLsizei, GLsizei);
static void (APIENTRY *glStencilMask)(GLuint);
static void (APIENTRY *glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                     GLint, GLenum, GLenum, const void *);
static void (APIENTRY *glTexParameteri)(GLenum, GLenum, GLint);
static void (APIENTRY *glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei,
                                        GLsizei, GLenum, GLenum, const void *);
static void (APIENTRY *glViewport)(GLint, GLint, GLsizei, GLsizei);

/* GL 1.2 and later: only wglGetProcAddress knows these */
static void (APIENTRY *glActiveTexture)(GLenum);
static void (APIENTRY *glAttachShader)(GLuint, GLuint);
static void (APIENTRY *glBindBuffer)(GLenum, GLuint);
static void (APIENTRY *glBindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *glBindVertexArray)(GLuint);
static void (APIENTRY *glBlendEquation)(GLenum);
static void (APIENTRY *glBufferData)(GLenum, GLsizeiptr, const void *,
                                     GLenum);
static GLenum (APIENTRY *glCheckFramebufferStatus)(GLenum);
static void (APIENTRY *glClearBufferfi)(GLenum, GLint, GLfloat, GLint);
static void (APIENTRY *glCompileShader)(GLuint);
static GLuint (APIENTRY *glCreateProgram)(void);
static GLuint (APIENTRY *glCreateShader)(GLenum);
static void (APIENTRY *glDeleteBuffers)(GLsizei, const GLuint *);
static void (APIENTRY *glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (APIENTRY *glDeleteProgram)(GLuint);
static void (APIENTRY *glDeleteShader)(GLuint);
static void (APIENTRY *glDeleteVertexArrays)(GLsizei, const GLuint *);
static void (APIENTRY *glEnableVertexAttribArray)(GLuint);
static void (APIENTRY *glFramebufferTexture2D)(GLenum, GLenum, GLenum,
                                               GLuint, GLint);
static void (APIENTRY *glGenBuffers)(GLsizei, GLuint *);
static void (APIENTRY *glGenFramebuffers)(GLsizei, GLuint *);
static void (APIENTRY *glGenVertexArrays)(GLsizei, GLuint *);
static void (APIENTRY *glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *glGetShaderiv)(GLuint, GLenum, GLint *);
static const GLubyte *(APIENTRY *glGetStringi)(GLenum, GLuint);
static GLint (APIENTRY *glGetUniformLocation)(GLuint, const GLchar *);
static void (APIENTRY *glLinkProgram)(GLuint);
static void (APIENTRY *glShaderSource)(GLuint, GLsizei,
                                       const GLchar *const *, const GLint *);
static void (APIENTRY *glStencilFuncSeparate)(GLenum, GLenum, GLint, GLuint);
static void (APIENTRY *glStencilOpSeparate)(GLenum, GLenum, GLenum, GLenum);
static void (APIENTRY *glUniform1f)(GLint, GLfloat);
static void (APIENTRY *glUniform1fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *glUniform1i)(GLint, GLint);
static void (APIENTRY *glUniform1iv)(GLint, GLsizei, const GLint *);
static void (APIENTRY *glUniform2f)(GLint, GLfloat, GLfloat);
typedef void APIENTRY R350GlUniform2fv(GLint, GLsizei, const GLfloat *);
static R350GlUniform2fv *glUniform2fv;
static void (APIENTRY *glUniform2i)(GLint, GLint, GLint);
typedef void APIENTRY R350GlUniform2iv(GLint, GLsizei, const GLint *);
static R350GlUniform2iv *glUniform2iv;
static void (APIENTRY *glUniform3i)(GLint, GLint, GLint, GLint);
static void (APIENTRY *glUniform4f)(GLint, GLfloat, GLfloat, GLfloat,
                                    GLfloat);
static void (APIENTRY *glUniform4fv)(GLint, GLsizei, const GLfloat *);
typedef void APIENTRY R350GlTexBuffer(GLenum, GLenum, GLuint);
static R350GlTexBuffer *glTexBuffer;
static void (APIENTRY *glUseProgram)(GLuint);
static void (APIENTRY *glVertexAttribPointer)(GLuint, GLint, GLenum,
                                              GLboolean, GLsizei,
                                              const void *);

typedef void (APIENTRY *R350GlProc)(void);

#define R350_GL_PROC(f) { #f, (R350GlProc *)&f }

static const struct {
    const char *name;
    R350GlProc *slot;
} r350_gl_proctab[] = {
    R350_GL_PROC(glBindTexture),
    R350_GL_PROC(glBlendFunc),
    R350_GL_PROC(glColorMask),
    R350_GL_PROC(glCopyTexSubImage2D),
    R350_GL_PROC(glDeleteTextures),
    R350_GL_PROC(glDepthFunc),
    R350_GL_PROC(glDepthMask),
    R350_GL_PROC(glDisable),
    R350_GL_PROC(glDrawArrays),
    R350_GL_PROC(glEnable),
    R350_GL_PROC(glGenTextures),
    R350_GL_PROC(glGetError),
    R350_GL_PROC(glGetIntegerv),
    R350_GL_PROC(glGetString),
    R350_GL_PROC(glPixelStorei),
    R350_GL_PROC(glReadPixels),
    R350_GL_PROC(glScissor),
    R350_GL_PROC(glStencilMask),
    R350_GL_PROC(glTexImage2D),
    R350_GL_PROC(glTexParameteri),
    R350_GL_PROC(glTexSubImage2D),
    R350_GL_PROC(glViewport),
    R350_GL_PROC(glActiveTexture),
    R350_GL_PROC(glAttachShader),
    R350_GL_PROC(glBindBuffer),
    R350_GL_PROC(glBindFramebuffer),
    R350_GL_PROC(glBindVertexArray),
    R350_GL_PROC(glBlendEquation),
    R350_GL_PROC(glBufferData),
    R350_GL_PROC(glCheckFramebufferStatus),
    R350_GL_PROC(glClearBufferfi),
    R350_GL_PROC(glCompileShader),
    R350_GL_PROC(glCreateProgram),
    R350_GL_PROC(glCreateShader),
    R350_GL_PROC(glDeleteBuffers),
    R350_GL_PROC(glDeleteFramebuffers),
    R350_GL_PROC(glDeleteProgram),
    R350_GL_PROC(glDeleteShader),
    R350_GL_PROC(glDeleteVertexArrays),
    R350_GL_PROC(glEnableVertexAttribArray),
    R350_GL_PROC(glFramebufferTexture2D),
    R350_GL_PROC(glGenBuffers),
    R350_GL_PROC(glGenFramebuffers),
    R350_GL_PROC(glGenVertexArrays),
    R350_GL_PROC(glGetProgramiv),
    R350_GL_PROC(glGetShaderiv),
    R350_GL_PROC(glGetStringi),
    R350_GL_PROC(glGetUniformLocation),
    R350_GL_PROC(glLinkProgram),
    R350_GL_PROC(glShaderSource),
    R350_GL_PROC(glStencilFuncSeparate),
    R350_GL_PROC(glStencilOpSeparate),
    R350_GL_PROC(glUniform1f),
    R350_GL_PROC(glUniform1fv),
    R350_GL_PROC(glUniform1i),
    R350_GL_PROC(glUniform1iv),
    R350_GL_PROC(glUniform2f),
    R350_GL_PROC(glUniform2fv),
    R350_GL_PROC(glUniform2i),
    R350_GL_PROC(glUniform2iv),
    R350_GL_PROC(glUniform3i),
    R350_GL_PROC(glUniform4f),
    R350_GL_PROC(glUniform4fv),
    R350_GL_PROC(glTexBuffer),
    R350_GL_PROC(glUseProgram),
    R350_GL_PROC(glVertexAttribPointer),
};

/*
 * wglGetProcAddress answers for entry points newer than 1.1 and, on
 * several drivers, returns 1, 2, 3 or -1 rather than NULL when it has no
 * answer; the 1.1 ones are ordinary DLL exports it does not know about.
 * Both sources are tried for every name, so the table above does not
 * have to record which era a function belongs to -- one fewer thing to
 * get wrong on a host this file cannot be tested on.
 */
static R350GlProc r350_gl_getproc(HMODULE gl32, const char *name)
{
    PROC p = wglGetProcAddress(name);
    uintptr_t v = (uintptr_t)p;

    if (v <= 3 || v == (uintptr_t)-1) {
        p = GetProcAddress(gl32, name);
    }
    return (R350GlProc)p;
}

/*
 * Resolve every entry point the file uses, before any of them is called.
 * A partial load is refused rather than survived: a missing symbol names
 * itself in the reason string, which is what the device reports as
 * `gl=...: <reason>` and is the only diagnosis available on a host
 * nobody can attach a debugger to.
 */
static bool r350_gl_load(const char **err)
{
    static char miss[80];
    HMODULE gl32 = LoadLibraryA("opengl32.dll");
    unsigned k;

    if (!gl32) {
        *err = "opengl32.dll could not be loaded";
        return false;
    }
    for (k = 0; k < ARRAY_SIZE(r350_gl_proctab); k++) {
        R350GlProc fn = r350_gl_getproc(gl32, r350_gl_proctab[k].name);

        if (!fn) {
            snprintf(miss, sizeof(miss), "host GL is missing %s()",
                     r350_gl_proctab[k].name);
            *err = miss;
            return false;
        }
        *r350_gl_proctab[k].slot = fn;
    }
    return true;
}

#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef struct R350GlPlat {
    HWND win;
    HDC dc;
    HGLRC ctx;
} R350GlPlat;

/* the context current on this thread, and whether it keeps it; see below */
static __thread R350GlPlat *r350_gl_cur;
static __thread bool r350_gl_keep;

static void r350_gl_plat_close(R350GlPlat *pl)
{
    if (pl->ctx) {
        wglMakeCurrent(NULL, NULL);
        r350_gl_cur = NULL;
        wglDeleteContext(pl->ctx);
        pl->ctx = NULL;
    }
    if (pl->dc) {
        ReleaseDC(pl->win, pl->dc);
        pl->dc = NULL;
    }
    if (pl->win) {
        DestroyWindow(pl->win);
        pl->win = NULL;
    }
}

/*
 * An invisible 1x1 window, because WGL has no windowless context: a
 * pixel format belongs to a device context and a device context comes
 * from a window. Nothing is ever drawn to it -- every draw goes to the
 * FBO and the caller puts the result back in emulated VRAM -- so it is
 * never shown and never has a message pumped, neither of which WGL
 * context creation or rendering depends on.
 *
 * Then the two-step every WGL program performs: wglCreateContext makes
 * only the driver's default context, and the entry point that makes a
 * core-profile one is itself an extension, so it can be asked for only
 * through a context that already exists.
 *
 * 4.1 core is tried before 3.3 core because the fragment shaders say
 * `#extension GL_ARB_gpu_shader5 : require` -- `precise` and fma() are
 * GL 4.0 features that 3.3 hardware exposes as an extension, and the
 * darwin leg asks for kCGLOGLPVersion_GL4_Core for the same reason. A
 * driver that gives 3.3 without the extension will fail at the first
 * shader compile with the file's own "would not compile" message rather
 * than render something subtly different.
 */
static bool r350_gl_plat_open(R350GlPlat *pl, const char **err)
{
    static const char cls[] = "qemu-ati-r350-gl";
    static bool cls_done;
    static const int ver[][2] = { { 4, 1 }, { 3, 3 } };
    /*
     * wglCreateContextAttribsARB through a union rather than a cast: a
     * direct cast from PROC to it changes the function type, which every
     * compiler warns about (-Wcast-function-type), and this file has to
     * build warning-clean on a toolchain nobody here can run.
     */
    union {
        PROC any;
        HGLRC (APIENTRY *mk)(HDC, HGLRC, const int *);
    } arb = { NULL };
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC boot;
    int fmt;
    unsigned k;

    if (!cls_done) {
        WNDCLASSA wc;

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = cls;
        if (!RegisterClassA(&wc) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            *err = "could not register the offscreen GL window class";
            return false;
        }
        cls_done = true;
    }
    pl->win = CreateWindowExA(0, cls, cls, WS_POPUP, 0, 0, 1, 1,
                              NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!pl->win) {
        *err = "could not create the offscreen GL window";
        return false;
    }
    pl->dc = GetDC(pl->win);
    if (!pl->dc) {
        *err = "the offscreen GL window has no device context";
        goto fail;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cAlphaBits = 8;
    fmt = ChoosePixelFormat(pl->dc, &pfd);
    if (!fmt || !SetPixelFormat(pl->dc, fmt, &pfd)) {
        *err = "no GL-capable pixel format on this host";
        goto fail;
    }

    boot = wglCreateContext(pl->dc);
    if (!boot) {
        *err = "wglCreateContext failed";
        goto fail;
    }
    if (wglMakeCurrent(pl->dc, boot)) {
        arb.any = wglGetProcAddress("wglCreateContextAttribsARB");
        wglMakeCurrent(NULL, NULL);
    }
    wglDeleteContext(boot);
    if (!arb.mk) {
        *err = "host GL has no WGL_ARB_create_context, so no 3.3 core "
               "context can be made";
        goto fail;
    }

    for (k = 0; k < ARRAY_SIZE(ver) && !pl->ctx; k++) {
        const int attr[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, ver[k][0],
            WGL_CONTEXT_MINOR_VERSION_ARB, ver[k][1],
            WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };

        pl->ctx = arb.mk(pl->dc, NULL, attr);
    }
    if (!pl->ctx) {
        *err = "host GL offers no 3.3 core profile context";
        goto fail;
    }
    if (!wglMakeCurrent(pl->dc, pl->ctx)) {
        *err = "the 3.3 core context could not be made current";
        goto fail;
    }
    if (!r350_gl_load(err)) {
        goto fail;
    }
    return true;

fail:
    r350_gl_plat_close(pl);
    return false;
}

/*
 * The context moves between HOST THREADS, and this is the one place the
 * two legs genuinely differ. ati_r350_gl_open() runs on QEMU's main
 * thread (device realize); draws run on the command processor's thread,
 * or on a vCPU thread when it has none; and ati_r350_gl_fetch() runs on
 * the main thread whenever ati_r350_update_display() releases the target
 * for scanout (ati_r350.c). Only one of them may use the backend at a
 * time: the command processor's thread while it is busy, any other only
 * while it is idle and with the BQL held, or, with lockless register
 * access, while it has claimed the engine (ati_r350_gl_enter()). That is
 * all CGL asks for -- but WGL asks for more: wglMakeCurrent FAILS while
 * the context is current to a DIFFERENT thread, and no thread can release
 * another thread's context.
 *
 * So a thread gives the context back when it leaves the backend, with one
 * exception. Taking and giving back costs two driver calls and, on most
 * drivers, a flush of the context, and the command processor's thread
 * makes a draw call thousands of times a second. It holds the context
 * (r350_gl_plat_hold()) and gives it back once, before it goes idle
 * (ati_r350_gl_unbind()), which is the only point at which another
 * thread can become the backend's user.
 */
static inline void r350_gl_makecurrent(R350GlPlat *pl)
{
    if (r350_gl_cur != pl) {
        wglMakeCurrent(pl->dc, pl->ctx);
        r350_gl_cur = pl;
    }
}

static inline void r350_gl_done(R350GlPlat *pl)
{
    if (!r350_gl_keep) {
        wglMakeCurrent(NULL, NULL);
        r350_gl_cur = NULL;
    }
}

static inline void r350_gl_plat_hold(bool hold)
{
    r350_gl_keep = hold;
}

static inline void r350_gl_plat_unbind(R350GlPlat *pl)
{
    if (r350_gl_cur == pl) {
        wglMakeCurrent(NULL, NULL);
        r350_gl_cur = NULL;
    }
}

/*
 * GL_ARB_texture_barrier (core in 4.5) or GL_NV_texture_barrier, which
 * it was promoted from unchanged. Either lets the draw queue order
 * overlapping passes on the GPU; without one every draw is submitted on
 * its own and a blend that reads its destination copies it first.
 */
static void (APIENTRY *r350_gl_texbar)(void);

static bool r350_gl_plat_has_barrier(void)
{
    static const char *const ext[][2] = {
        { "GL_ARB_texture_barrier", "glTextureBarrier" },
        { "GL_NV_texture_barrier", "glTextureBarrierNV" },
    };
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    GLint n = 0, i;
    unsigned k;

    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (k = 0; k < ARRAY_SIZE(ext); k++) {
        for (i = 0; i < n; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);

            if (e && !strcmp(e, ext[k][0])) {
                r350_gl_texbar = r350_gl_getproc(gl32, ext[k][1]);
                if (r350_gl_texbar) {
                    return true;
                }
            }
        }
    }
    return false;
}

static inline void r350_gl_barrier(void)
{
    r350_gl_texbar();
}

#endif /* CONFIG_DARWIN */

/*
 * One draw program and its uniform locations, resolved once at link
 * time. Two variants are built from the same source with one #define
 * between them: `main` computes the blend itself and writes bytes into
 * the integer colour buffer, `add` writes only the source term as a
 * float and lets GL's blender add it. A uniform a variant does not use
 * resolves to -1, and glUniform on -1 is defined to do nothing, so both
 * are fed by the same code below.
 */
/* unit 0's filter uniform: the request's filt[0][], border RGBA, pad */
#define R350_GL_TF 16

typedef struct R350GlProg {
    GLuint prog;
    GLint u_rect, u_org, u_texsize, u_clamp, u_textured;
    GLint u_alphatest, u_affunc, u_afref, u_discard;
    GLint u_blend, u_blendread, u_cfac, u_afac, u_konst;
    GLint u_usk;
    GLint u_tf;
    /* the general form's; -1 in a program of the simple form */
    GLint u_txsize, u_txclamp, u_txtf, u_txen;
    GLint u_tcbase, u_tcstride, u_tcinv, u_tcraw, u_lodany;
    GLint u_zscale, u_zonly;
} R350GlProg;

/*
 * Where each guest texture unit is bound in the general form: unit 0
 * where the simple form binds its one texture, the others above the
 * destination copy (1) and the add-blend twin (2). The coordinate block
 * is a buffer texture after them.
 */
static inline unsigned gl_txunit(unsigned un)
{
    return un ? 2 + un : 0;
}

#define R350_GL_TCUNIT 10

/*
 * One guest fragment program, linked. There is no single draw shader any
 * more: milestone M5 splices the translated `us_main()` into the source
 * below, so a program is per US program per blend variant. The corpus
 * five captures hold contains ten distinct programs, and a guest changes
 * program far less often than it draws, so a small direct-mapped cache
 * keyed on the translation's signature keeps the link count at the
 * number of programs rather than the number of draws.
 */
#define R350_GL_PROGSLOTS 16

typedef struct R350GlProgSlot {
    uint64_t key;               /* 0 = empty */
    bool add;                   /* which blend variant this is */
    R350GlProg p;
} R350GlProgSlot;

struct R350GlCtx {
    R350GlPlat plat;
    R350GlProgSlot prog[R350_GL_PROGSLOTS];
    unsigned prog_next;         /* round-robin victim */
    uint64_t prog_hits, prog_links, prog_failed;
    /* the two format conversions the add-blend path needs, and their VAO */
    GLuint ui2n, n2ui, vao_blit;
    GLuint vao, vbo, fbo, cbuf, dst;
    /* the normalized colour buffer GL's own blender can write into */
    GLuint acc;
    /*
     * The depth/stencil buffer, GL_DEPTH32F_STENCIL8 beside the colour
     * buffer. A Z value z is stored as z * 2^-24, which the float format
     * holds exactly, so GL's compare is the device's integer compare.
     * `zcur` is the test state last applied, valid while `zset`.
     */
    GLuint zbuf;
    bool has_z;
    R350GlZ zcur;
    bool zset;
    /* uploaded textures by caller slot, plus the scratch at the end */
    GLuint tex[R350_GL_TEXSLOTS + 1];
    int tex_w[R350_GL_TEXSLOTS + 1], tex_h[R350_GL_TEXSLOTS + 1];
    int tex_nl[R350_GL_TEXSLOTS + 1];   /* mip levels specified */
    GLuint white;                   /* the 1x1 an untextured draw samples */
    /* the resident target's size; a request smaller than it reuses it */
    int fb_w, fb_h;
    /* staging for the two swapper orders GL cannot produce directly */
    uint8_t *stage;
    size_t stage_sz;
    /* the texture attached to `fbo`; 0 when unknown */
    GLuint att;
    /*
     * GL_NV_texture_barrier: the blend samples the colour buffer itself.
     * `wr` holds the rectangles rendered or uploaded into it since the
     * last barrier, merged into one once there are too many to keep; a
     * draw reading inside any of them is preceded by a barrier.
     */
    bool barrier;
    struct { int x0, y0, x1, y1; } wr[R350_GL_WRITTEN];
    unsigned nwr;
    uint64_t barriers;
    /* draws deferred into waves; see "THE DRAW QUEUE" */
    struct R350GlUnit *q;
    unsigned nq;
    float *qv;
    size_t nqv, qv_cap;
    bool q_prebarrier;
    uint64_t q_units, q_flushes, q_waves;
    /* the general form's coordinate block, per queue and on the GPU */
    GLuint tcbo, tctex;
    float *qt;
    size_t nqt, qt_cap;
    size_t tc_max;                      /* texels a buffer texture holds */
    char desc[128];
};

static void gl_flush_queue(R350GlCtx *g);

/*
 * Attaching a texture to the framebuffer ends the render pass on this
 * host even when it is the one already attached, and a pass is a Metal
 * command buffer. So it is done only when the texture changes.
 */
static void gl_attach(R350GlCtx *g, GLuint tex)
{
    if (g->att != tex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        g->att = tex;
    }
}

/* the colour buffer changed over [x0,x1) x [y0,y1) */
static void gl_wrote(R350GlCtx *g, int x0, int y0, int x1, int y1)
{
    unsigned k;

    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (g->nwr == R350_GL_WRITTEN) {
        for (k = 1; k < g->nwr; k++) {
            g->wr[0].x0 = MIN(g->wr[0].x0, g->wr[k].x0);
            g->wr[0].y0 = MIN(g->wr[0].y0, g->wr[k].y0);
            g->wr[0].x1 = MAX(g->wr[0].x1, g->wr[k].x1);
            g->wr[0].y1 = MAX(g->wr[0].y1, g->wr[k].y1);
        }
        g->nwr = 1;
    }
    g->wr[g->nwr].x0 = x0;
    g->wr[g->nwr].y0 = y0;
    g->wr[g->nwr].x1 = x1;
    g->wr[g->nwr].y1 = y1;
    g->nwr++;
}

static void gl_barrier(R350GlCtx *g)
{
    r350_gl_barrier();
    g->barriers++;
    g->nwr = 0;
}

/* make what was written visible to a read of [x0,x1) x [y0,y1) */
static void gl_before_read(R350GlCtx *g, int x0, int y0, int x1, int y1)
{
    unsigned k;

    for (k = 0; k < g->nwr; k++) {
        if (x0 < g->wr[k].x1 && g->wr[k].x0 < x1 &&
            y0 < g->wr[k].y1 && g->wr[k].y0 < y1) {
            gl_barrier(g);
            return;
        }
    }
}

/*
 * The depth and stencil test state of a draw, applied only when it
 * changes. ZB_ZSTENCILCNTL's codes are GL's order with LEQUAL and EQUAL
 * swapped, and its stencil operations are GL's in the usual order.
 */
static const GLenum gl_zfunc[8] = {
    GL_NEVER, GL_LESS, GL_LEQUAL, GL_EQUAL,
    GL_GEQUAL, GL_GREATER, GL_NOTEQUAL, GL_ALWAYS,
};

static const GLenum gl_sop[8] = {
    GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR,
    GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP,
};

static void gl_zstate(R350GlCtx *g, const R350GlZ *z)
{
    static const R350GlZ none;
    unsigned f;

    if (!z || !z->mode) {
        z = &none;
    }
    if (g->zset && !memcmp(&g->zcur, z, sizeof(*z))) {
        return;
    }
    g->zcur = *z;
    g->zset = true;
    if (z->mode && z->test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_zfunc[z->func & 7]);
        glDepthMask(z->write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (z->mode == 1 && z->stencil) {
        glEnable(GL_STENCIL_TEST);
        for (f = 0; f < 2; f++) {
            GLenum face = f ? GL_BACK : GL_FRONT;

            glStencilFuncSeparate(face, gl_zfunc[z->sfunc[f] & 7],
                                  z->sref & 0xff, z->smask & 0xff);
            glStencilOpSeparate(face, gl_sop[z->sfail[f] & 7],
                                gl_sop[z->szfail[f] & 7],
                                gl_sop[z->szpass[f] & 7]);
        }
        glStencilMask(z->swmask & 0xff);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
}

static inline float gl_zscale(const R350GlZ *z)
{
    return z->mode == 2 ? 65535.0f : 16777215.0f;
}

static const char *vs_src =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
"layout(location = 0) in vec2 a_pos;\n"
"layout(location = 1) in vec4 a_col;\n"
"layout(location = 2) in vec4 a_z;\n"
"layout(location = 3) in vec2 a_p0;\n"
"layout(location = 4) in vec2 a_p1;\n"
"layout(location = 5) in vec2 a_p2;\n"
"layout(location = 6) in vec4 a_c0;\n"
"layout(location = 7) in vec4 a_c1;\n"
"layout(location = 8) in vec4 a_c2;\n"
"layout(location = 9) in vec2 a_t0;\n"
"layout(location = 10) in vec2 a_t1;\n"
"layout(location = 11) in vec2 a_t2;\n"
"layout(location = 12) in vec4 a_inv;\n"
"layout(location = 13) in vec4 a_s0;\n"
"layout(location = 14) in vec4 a_s1;\n"
"layout(location = 15) in vec4 a_s2;\n"
"uniform vec4 u_rect;\n"
"flat out vec2 f_p0;\n"
"flat out vec2 f_p1;\n"
"flat out vec2 f_p2;\n"
"flat out vec4 f_c0;\n"
"flat out vec4 f_c1;\n"
"flat out vec4 f_c2;\n"
"flat out vec2 f_t0;\n"
"flat out vec2 f_t1;\n"
"flat out vec2 f_t2;\n"
"flat out vec4 f_inv;\n"
"flat out vec4 f_s0;\n"
"flat out vec4 f_s1;\n"
"flat out vec4 f_s2;\n"
"flat out vec4 f_z;\n"
"void main()\n"
"{\n"
"    float nx = (a_pos.x - u_rect.x) / u_rect.z * 2.0 - 1.0;\n"
"    float ny = (a_pos.y - u_rect.y) / u_rect.w * 2.0 - 1.0;\n"
/*
 * w = 1 keeps GL's own interpolation affine, which is what the software
 * rasterizer's screen-space barycentric weights already are; the
 * perspective correction is applied to those weights in the fragment
 * stage, from each corner's 1/w.
 */
"    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
"    f_p0 = a_p0; f_p1 = a_p1; f_p2 = a_p2;\n"
"    f_c0 = a_c0; f_c1 = a_c1; f_c2 = a_c2;\n"
"    f_t0 = a_t0; f_t1 = a_t1; f_t2 = a_t2;\n"
"    f_inv = a_inv;\n"
"    f_s0 = a_s0; f_s1 = a_s1; f_s2 = a_s2;\n"
"    f_z = a_z;\n"
"}\n";

/*
 * The two fragment-shader prologues. Everything after them is one
 * source; `R350_ADD` picks the variant. The integer output is the
 * device's truncating pack written literally; the float one is a source
 * term for GL's blender, biased so that its round-to-nearest lands on
 * the byte the device would have truncated to (see the add-blend
 * comment in ati_r350_gl_draw()).
 */
static const char *fs_head_main =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
"uniform vec4 USK[32];\n"
"#define R350_OZERO uvec4(0u)\n"
"out uvec4 o_col;\n";

static const char *fs_head_add =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
"#define R350_ADD 1\n"
"uniform vec4 USK[32];\n"
"#define R350_OZERO vec4(0.0)\n"
"out vec4 o_col;\n";

static const char *fs_src =
"flat in vec2 f_p0;\n"
"flat in vec2 f_p1;\n"
"flat in vec2 f_p2;\n"
"flat in vec4 f_c0;\n"
"flat in vec4 f_c1;\n"
"flat in vec4 f_c2;\n"
"flat in vec2 f_t0;\n"
"flat in vec2 f_t1;\n"
"flat in vec2 f_t2;\n"
"flat in vec4 f_inv;\n"
"flat in vec4 f_s0;\n"
"flat in vec4 f_s1;\n"
"flat in vec4 f_s2;\n"
"flat in vec4 f_z;\n"
"uniform float u_zscale;\n"
"uniform int u_zonly;\n"
"uniform usampler2D u_tex;\n"
"uniform usampler2D u_dst;\n"
"uniform float u_n255[256];\n"
"uniform vec2 u_org;\n"
"uniform ivec2 u_texsize;\n"
"uniform ivec2 u_clamp;\n"
"uniform int u_tf[16];\n"
"uniform int u_textured;\n"
"uniform int u_alphatest;\n"
"uniform int u_affunc;\n"
"uniform float u_afref;\n"
"uniform int u_discard;\n"
"uniform int u_blend;\n"
"uniform int u_blendread;\n"
"uniform ivec3 u_cfac;\n"
"uniform ivec3 u_afac;\n"
"uniform vec4 u_konst;\n"
"\n"
"float bf(int code, float sc, float sa, float dc, float da,\n"
"         float kc, float ka)\n"
"{\n"
"    if (code == 1 || code == 32) return 0.0;\n"
"    if (code == 2 || code == 33) return 1.0;\n"
"    if (code == 3 || code == 34) return sc;\n"
"    if (code == 4 || code == 35) return 1.0 - sc;\n"
"    if (code == 9 || code == 36) return dc;\n"
"    if (code == 10 || code == 37) return 1.0 - dc;\n"
"    if (code == 5 || code == 38) return sa;\n"
"    if (code == 6 || code == 39) return 1.0 - sa;\n"
"    if (code == 7 || code == 40) return da;\n"
"    if (code == 8 || code == 41) return 1.0 - da;\n"
"    if (code == 11 || code == 42) return min(sa, 1.0 - da);\n"
"    if (code == 43) return kc;\n"
"    if (code == 44) return 1.0 - kc;\n"
"    if (code == 45) return ka;\n"
"    if (code == 46) return 1.0 - ka;\n"
"    return 1.0;\n"
"}\n"
"\n"
"float comb(int f, float s, float d)\n"
"{\n"
"    if (f == 2 || f == 3) return s - d;\n"
"    if (f == 4) return min(s, d);\n"
"    if (f == 5) return max(s, d);\n"
"    if (f == 6 || f == 7) return d - s;\n"
"    return s + d;\n"
"}\n"
"\n"
/*
 * r300_tex_filter() and its helpers in ati_r350_3d.c, operation for
 * operation: the same clamps, the same integer LOD and weights, and the
 * same one-rounding-per-statement arithmetic on the floats, so a filtered
 * texel matches the device's bit for bit whenever the derivatives do.
 * u_tf = { on, need_lod, mag, min, mip, log2 aniso, first, last, bias,
 * log2 w, log2 h, border r, g, b, a }.
 */
"float tc_pre(float c, int n, int m)\n"
"{\n"
"    precise float r = c;\n"
"    if (m == 3 || m == 5 || m == 7) r = abs(r);\n"
"    if (m == 4 || m == 5) r = min(max(r, 0.0), float(n));\n"
"    return min(max(r, -16777216.0), 16777216.0);\n"
"}\n"
"\n"
"int tc_mod(int i, int n)\n"
"{\n"
"    return i >= 0 ? i % n : n - 1 - (-1 - i) % n;\n"
"}\n"
"\n"
"int tc_idx(int i, int n, int m, bool pt)\n"
"{\n"
"    if (m == 0) return tc_mod(i, n);\n"
"    if (m == 1) {\n"
"        int r = tc_mod(i, 2 * n);\n"
"        return r >= n ? 2 * n - 1 - r : r;\n"
"    }\n"
"    if (m == 2 || m == 3 || ((m == 4 || m == 5) && pt))\n"
"        return clamp(i, 0, n - 1);\n"
"    return i < 0 || i >= n ? -1 : i;\n"
"}\n"
"\n"
"uvec4 tfetch(int l, int i, int j)\n"
"{\n"
"    if (i < 0 || j < 0)\n"
"        return uvec4(u_tf[11], u_tf[12], u_tf[13], u_tf[14]);\n"
"    return texelFetch(u_tex, ivec2(i, j), l);\n"
"}\n"
"\n"
"uvec4 tlerp(uvec4 a, uvec4 b, int f)\n"
"{\n"
"    return uvec4((ivec4(a) * (256 - f) + ivec4(b) * f + 128) >> 8);\n"
"}\n"
"\n"
"uvec4 tlevel(int l, float fs, float ft, bool lin)\n"
"{\n"
"    int w = max(u_texsize.x >> l, 1), h = max(u_texsize.y >> l, 1);\n"
"    precise float ss = tc_pre(ldexp(fs, -min(l, u_tf[9])), w, u_clamp.x);\n"
"    precise float tt = tc_pre(ldexp(ft, -min(l, u_tf[10])), h, u_clamp.y);\n"
"    if (!lin)\n"
"        return tfetch(l, tc_idx(int(floor(ss)), w, u_clamp.x, true),\n"
"                      tc_idx(int(floor(tt)), h, u_clamp.y, true));\n"
"    precise float fx = ss - 0.5;\n"
"    precise float fy = tt - 0.5;\n"
"    precise float x0 = floor(fx);\n"
"    precise float y0 = floor(fy);\n"
"    precise float qx = fx - x0;\n"
"    precise float qy = fy - y0;\n"
"    int wx = int(qx * 256.0 + 0.5);\n"
"    int wy = int(qy * 256.0 + 0.5);\n"
"    int i0 = int(x0), j0 = int(y0);\n"
"    if (wx == 256) { i0++; wx = 0; }\n"
"    if (wy == 256) { j0++; wy = 0; }\n"
"    int i1 = tc_idx(i0 + 1, w, u_clamp.x, false);\n"
"    int j1 = tc_idx(j0 + 1, h, u_clamp.y, false);\n"
"    i0 = tc_idx(i0, w, u_clamp.x, false);\n"
"    j0 = tc_idx(j0, h, u_clamp.y, false);\n"
"    uvec4 t00 = tfetch(l, i0, j0);\n"
"    uvec4 t10 = wx != 0 ? tfetch(l, i1, j0) : t00;\n"
"    if (wy == 0) return wx != 0 ? tlerp(t00, t10, wx) : t00;\n"
"    uvec4 t01 = tfetch(l, i0, j1);\n"
"    uvec4 t11 = wx != 0 ? tfetch(l, i1, j1) : t01;\n"
"    ivec4 top = ivec4(t00) * (256 - wx) + ivec4(t10) * wx;\n"
"    ivec4 bot = ivec4(t01) * (256 - wx) + ivec4(t11) * wx;\n"
"    return uvec4((top * (256 - wy) + bot * wy + 32768) >> 16);\n"
"}\n"
"\n"
"uvec4 tmip(int lod, float fs, float ft)\n"
"{\n"
"    bool lin = u_tf[3] != 1;\n"
"    int lo = min(max(lod, u_tf[6] * 256), u_tf[7] * 256);\n"
"    if (u_tf[4] == 1)\n"
"        return tlevel(min((lo + 128) >> 8, u_tf[7]), fs, ft, lin);\n"
"    int l = lo >> 8;\n"
"    if (u_tf[4] != 2 || l >= u_tf[7] || (lo & 255) == 0)\n"
"        return tlevel(l, fs, ft, lin);\n"
"    return tlerp(tlevel(l, fs, ft, lin), tlevel(l + 1, fs, ft, lin),\n"
"                 lo & 255);\n"
"}\n"
"\n"
"int tlog2(float v)\n"
"{\n"
"    if (!(v > 0.0)) return -65536;\n"
"    uint b = floatBitsToUint(v);\n"
"    if (b >= 0x7f800000u) return 65536;\n"
"    return (int(b >> 23) - 127) * 256 + int((b >> 15) & 0xffu);\n"
"}\n"
"\n"
"uvec4 tfilter(float fs, float ft, vec4 der)\n"
"{\n"
"    int lod = 0, nl = 0;\n"
"    precise float ax = 0.0, ay = 0.0;\n"
"    if (u_tf[1] != 0) {\n"
"        precise float m = der.x * der.x;\n"
"        precise float n = der.y * der.y;\n"
"        precise float px = m + n;\n"
"        m = der.z * der.z;\n"
"        n = der.w * der.w;\n"
"        precise float py = m + n;\n"
"        if (u_tf[3] == 3) {\n"
"            int lmaj = tlog2(px >= py ? px : py);\n"
"            int lmin = tlog2(px >= py ? py : px);\n"
"            nl = min(max(((lmaj - lmin) / 2 + 255) >> 8, 0), u_tf[5]);\n"
"            lod = (lmaj >> 1) - nl * 256;\n"
"            ax = px >= py ? der.x : der.z;\n"
"            ay = px >= py ? der.y : der.w;\n"
"        } else {\n"
"            lod = tlog2(px >= py ? px : py) >> 1;\n"
"        }\n"
"        lod += u_tf[8];\n"
"    }\n"
"    if (lod <= 0) return tlevel(u_tf[6], fs, ft, u_tf[2] != 1);\n"
"    if (nl == 0) return tmip(lod, fs, ft);\n"
"    int N = 1 << nl;\n"
"    uvec4 sum = uvec4(0u);\n"
"    for (int k = 0; k < N; k++) {\n"
"        precise float ok = float(2 * k + 1 - N) / float(2 * N);\n"
"        precise float ds = ax * ok;\n"
"        precise float dt = ay * ok;\n"
"        precise float s1 = fs + ds;\n"
"        precise float t1 = ft + dt;\n"
"        sum += tmip(lod, s1, t1);\n"
"    }\n"
"    return (sum + uint(N >> 1)) >> uint(nl);\n"
"}\n"
"\n"
/* r300_tc_der() */
"void tc_der(vec3 ga, vec3 gb, float t0, float t1, float t2, float v,\n"
"            float iq, out float dx, out float dy)\n"
"{\n"
"    precise float e0 = t0 - v;\n"
"    precise float e1 = t1 - v;\n"
"    precise float e2 = t2 - v;\n"
"    precise float m0 = ga.x * e0;\n"
"    precise float m1 = ga.y * e1;\n"
"    precise float m2 = ga.z * e2;\n"
"    precise float r = m0 + m1;\n"
"    r = r + m2;\n"
"    precise float rx = r * iq;\n"
"    m0 = gb.x * e0;\n"
"    m1 = gb.y * e1;\n"
"    m2 = gb.z * e2;\n"
"    r = m0 + m1;\n"
"    r = r + m2;\n"
"    precise float ry = r * iq;\n"
"    dx = rx;\n"
"    dy = ry;\n"
"}\n"
"\n"
/*
 * The general form's fetch: r300_us_sample() for any unit, with the same
 * filter as above reading that unit's own state. A sampler array is
 * indexed by literals only, which GLSL 3.30 allows without qualification.
 */
"#ifdef R350_USGEN\n"
"uniform usampler2D u_txs[8];\n"
"uniform ivec2 u_txsize[8];\n"
"uniform ivec2 u_txclamp[8];\n"
"uniform int u_txtf[128];\n"
"uniform int u_txen;\n"
"uniform samplerBuffer u_tcbuf;\n"
"uniform int u_tcbase;\n"
"uniform int u_tcstride;\n"
"uniform vec2 u_tcinv[8];\n"
"uniform int u_tcraw;\n"
"uniform int u_lodany;\n"
"vec4 us_der[8];\n"
"\n"
"uvec4 gtexel(int un, int l, ivec2 ij)\n"
"{\n"
"    if (un == 0) return texelFetch(u_txs[0], ij, l);\n"
"    if (un == 1) return texelFetch(u_txs[1], ij, l);\n"
"    if (un == 2) return texelFetch(u_txs[2], ij, l);\n"
"    if (un == 3) return texelFetch(u_txs[3], ij, l);\n"
"    if (un == 4) return texelFetch(u_txs[4], ij, l);\n"
"    if (un == 5) return texelFetch(u_txs[5], ij, l);\n"
"    if (un == 6) return texelFetch(u_txs[6], ij, l);\n"
"    return texelFetch(u_txs[7], ij, l);\n"
"}\n"
"\n"
"uvec4 gfetch(int un, int l, int i, int j)\n"
"{\n"
"    if (i < 0 || j < 0)\n"
"        return uvec4(u_txtf[un * 16 + 11], u_txtf[un * 16 + 12],\n"
"                     u_txtf[un * 16 + 13], u_txtf[un * 16 + 14]);\n"
"    return gtexel(un, l, ivec2(i, j));\n"
"}\n"
"\n"
"uvec4 glevel(int un, int l, float fs, float ft, bool lin)\n"
"{\n"
"    ivec2 cl = u_txclamp[un];\n"
"    int w = max(u_txsize[un].x >> l, 1), h = max(u_txsize[un].y >> l, 1);\n"
"    precise float ss = tc_pre(ldexp(fs, -min(l, u_txtf[un * 16 + 9])), w,\n"
"                              cl.x);\n"
"    precise float tt = tc_pre(ldexp(ft, -min(l, u_txtf[un * 16 + 10])), h,\n"
"                              cl.y);\n"
"    if (!lin)\n"
"        return gfetch(un, l, tc_idx(int(floor(ss)), w, cl.x, true),\n"
"                      tc_idx(int(floor(tt)), h, cl.y, true));\n"
"    precise float fx = ss - 0.5;\n"
"    precise float fy = tt - 0.5;\n"
"    precise float x0 = floor(fx);\n"
"    precise float y0 = floor(fy);\n"
"    precise float qx = fx - x0;\n"
"    precise float qy = fy - y0;\n"
"    int wx = int(qx * 256.0 + 0.5);\n"
"    int wy = int(qy * 256.0 + 0.5);\n"
"    int i0 = int(x0), j0 = int(y0);\n"
"    if (wx == 256) { i0++; wx = 0; }\n"
"    if (wy == 256) { j0++; wy = 0; }\n"
"    int i1 = tc_idx(i0 + 1, w, cl.x, false);\n"
"    int j1 = tc_idx(j0 + 1, h, cl.y, false);\n"
"    i0 = tc_idx(i0, w, cl.x, false);\n"
"    j0 = tc_idx(j0, h, cl.y, false);\n"
"    uvec4 t00 = gfetch(un, l, i0, j0);\n"
"    uvec4 t10 = wx != 0 ? gfetch(un, l, i1, j0) : t00;\n"
"    if (wy == 0) return wx != 0 ? tlerp(t00, t10, wx) : t00;\n"
"    uvec4 t01 = gfetch(un, l, i0, j1);\n"
"    uvec4 t11 = wx != 0 ? gfetch(un, l, i1, j1) : t01;\n"
"    ivec4 top = ivec4(t00) * (256 - wx) + ivec4(t10) * wx;\n"
"    ivec4 bot = ivec4(t01) * (256 - wx) + ivec4(t11) * wx;\n"
"    return uvec4((top * (256 - wy) + bot * wy + 32768) >> 16);\n"
"}\n"
"\n"
"uvec4 gmip(int un, int lod, float fs, float ft)\n"
"{\n"
"    int b = un * 16;\n"
"    bool lin = u_txtf[b + 3] != 1;\n"
"    int lo = min(max(lod, u_txtf[b + 6] * 256), u_txtf[b + 7] * 256);\n"
"    if (u_txtf[b + 4] == 1)\n"
"        return glevel(un, min((lo + 128) >> 8, u_txtf[b + 7]), fs, ft,\n"
"                      lin);\n"
"    int l = lo >> 8;\n"
"    if (u_txtf[b + 4] != 2 || l >= u_txtf[b + 7] || (lo & 255) == 0)\n"
"        return glevel(un, l, fs, ft, lin);\n"
"    return tlerp(glevel(un, l, fs, ft, lin),\n"
"                 glevel(un, l + 1, fs, ft, lin), lo & 255);\n"
"}\n"
"\n"
"uvec4 gfilter(int un, float fs, float ft, vec4 der)\n"
"{\n"
"    int b = un * 16;\n"
"    int lod = 0, nl = 0;\n"
"    precise float ax = 0.0, ay = 0.0;\n"
"    if (u_txtf[b + 1] != 0) {\n"
"        precise float m = der.x * der.x;\n"
"        precise float n = der.y * der.y;\n"
"        precise float px = m + n;\n"
"        m = der.z * der.z;\n"
"        n = der.w * der.w;\n"
"        precise float py = m + n;\n"
"        if (u_txtf[b + 3] == 3) {\n"
"            int lmaj = tlog2(px >= py ? px : py);\n"
"            int lmin = tlog2(px >= py ? py : px);\n"
"            nl = min(max(((lmaj - lmin) / 2 + 255) >> 8, 0), u_txtf[b + 5]);\n"
"            lod = (lmaj >> 1) - nl * 256;\n"
"            ax = px >= py ? der.x : der.z;\n"
"            ay = px >= py ? der.y : der.w;\n"
"        } else {\n"
"            lod = tlog2(px >= py ? px : py) >> 1;\n"
"        }\n"
"        lod += u_txtf[b + 8];\n"
"    }\n"
"    if (lod <= 0) return glevel(un, u_txtf[b + 6], fs, ft, u_txtf[b + 2] != 1);\n"
"    if (nl == 0) return gmip(un, lod, fs, ft);\n"
"    int N = 1 << nl;\n"
"    uvec4 sum = uvec4(0u);\n"
"    for (int k = 0; k < N; k++) {\n"
"        precise float ok = float(2 * k + 1 - N) / float(2 * N);\n"
"        precise float ds = ax * ok;\n"
"        precise float dt = ay * ok;\n"
"        precise float s1 = fs + ds;\n"
"        precise float t1 = ft + dt;\n"
"        sum += gmip(un, lod, s1, t1);\n"
"    }\n"
"    return (sum + uint(N >> 1)) >> uint(nl);\n"
"}\n"
"\n"
"vec4 us_fetch(int un, vec4 coord, int dset)\n"
"{\n"
"    if (((u_txen >> un) & 1) == 0) return vec4(1.0);\n"
"    int w = u_txsize[un].x, h = u_txsize[un].y;\n"
"    precise float fs = coord.x * float(w);\n"
"    precise float ft = coord.y * float(h);\n"
"    uvec4 tu;\n"
"    if (u_txtf[un * 16] != 0) {\n"
"        vec4 der = vec4(0.0);\n"
"        if (u_txtf[un * 16 + 1] != 0) {\n"
"            vec4 g = us_der[dset];\n"
"            precise float d0 = g.x * float(w);\n"
"            precise float d1 = g.y * float(h);\n"
"            precise float d2 = g.z * float(w);\n"
"            precise float d3 = g.w * float(h);\n"
"            der = vec4(d0, d1, d2, d3);\n"
"        }\n"
"        tu = gfilter(un, fs, ft, der);\n"
"    } else {\n"
"        int tx = int(fs);\n"
"        int ty = int(ft);\n"
"        if (u_txclamp[un].x <= 1 && w > 0) {\n"
"            tx = tx % w;\n"
"            if (tx < 0) tx += w;\n"
"        } else {\n"
"            tx = clamp(tx, 0, w - 1);\n"
"        }\n"
"        if (u_txclamp[un].y <= 1 && h > 0) {\n"
"            ty = ty % h;\n"
"            if (ty < 0) ty += h;\n"
"        } else {\n"
"            ty = clamp(ty, 0, h - 1);\n"
"        }\n"
"        tu = gtexel(un, 0, ivec2(tx, ty));\n"
"    }\n"
"    return vec4(u_n255[int(tu.r)], u_n255[int(tu.g)],\n"
"                u_n255[int(tu.b)], u_n255[int(tu.a)]);\n"
"}\n"
"#endif\n"
"\n"
"void main()\n"
"{\n"
"    precise vec4 c;\n"
"    precise float ts, tt;\n"
/*
 * r300_raster_tri()'s own weights, expression for expression, fusion
 * included -- see the file comment.
 */
"    precise float inv = f_inv.x;\n"
"    precise float px = gl_FragCoord.x + u_org.x;\n"
"    precise float py = gl_FragCoord.y + u_org.y;\n"
"    precise float q0 = (f_p2.y - f_p1.y) * (px - f_p1.x);\n"
"    precise float q1 = (f_p0.y - f_p2.y) * (px - f_p2.x);\n"
"    precise float d0 = fma(f_p2.x - f_p1.x, py - f_p1.y, -q0);\n"
"    precise float d1 = fma(f_p0.x - f_p2.x, py - f_p2.y, -q1);\n"
"    precise float w0 = d0 * inv;\n"
"    precise float w1 = d1 * inv;\n"
"    precise float w2 = 1.0 - w0 - w1;\n"
/*
 * Z, screen-linear, with r300_raster_tri()'s weights and the fusion the
 * C compiler gives its three-term sum; then r300_zb_pixel()'s clamp,
 * scale and truncation. The integer goes out as z * 2^-24, exact.
 */
"    {\n"
"        precise float zp = w1 * f_z.y;\n"
"        precise float zf = fma(w2, f_z.z, fma(w0, f_z.x, zp));\n"
"        float zc = zf > 0.0 ? zf : 0.0;\n"
"        zc = zc < 1.0 ? zc : 1.0;\n"
"        precise float zs = zc * u_zscale;\n"
"        gl_FragDepth = float(uint(zs)) * 5.9604644775390625e-8;\n"
"    }\n"
/* a depth-only pass shades nothing and kills nothing */
"    if (u_zonly != 0) {\n"
"        o_col = R350_OZERO;\n"
"        return;\n"
"    }\n"
/*
 * Perspective-correct weights when the corners' 1/w differ, as
 * r300_raster_tri() computes them.
 */
"    precise float iq = 1.0;\n"
"    bool persp = f_inv.y != f_inv.z || f_inv.z != f_inv.w;\n"
"    if (persp) {\n"
"        precise float pq0 = w0 * f_inv.y;\n"
"        precise float pq1 = w1 * f_inv.z;\n"
"        precise float pq2 = w2 * f_inv.w;\n"
"        iq = 1.0 / (pq0 + pq1 + pq2);\n"
"        w0 = pq0 * iq; w1 = pq1 * iq; w2 = pq2 * iq;\n"
"    }\n"
"    c = fma(vec4(w2), f_c2, fma(vec4(w1), f_c1, w0 * f_c0));\n"
/*
 * Coordinate set 0 from the vertex array, all a simple-form program
 * reads. See the coordinate-set note in ati_r350_gl.h.
 */
"    precise vec2 st = fma(vec2(w2), f_t2,\n"
"                          fma(vec2(w1), f_t1, w0 * f_t0));\n"
"    ts = st.x; tt = st.y;\n"
/*
 * The fragment program's inputs, in the units it reads them: the texel
 * as four normalized floats through the same k/255 table the software
 * path's r300_texel_chan() divides by, and the two interpolated colours
 * with the rasterizer's own weights. `us_main()` above is the guest's
 * program, translated; nothing here decides what it computes.
 */
"    precise vec4 c1 = fma(vec4(w2), f_s2, fma(vec4(w1), f_s1, w0 * f_s0));\n"
"#ifdef R350_USGEN\n"
/*
 * Every coordinate set r300_raster_tri() interpolates, with its weights,
 * in the guest's units, and their derivatives where a unit filters with
 * a footprint; then the program, which fetches as it goes.
 */
"    {\n"
"        int tb = u_tcbase + gl_PrimitiveID * u_tcstride;\n"
"        precise vec3 ga = vec3(0.0);\n"
"        precise vec3 gb = vec3(0.0);\n"
"        if (u_lodany != 0) {\n"
"            precise float a0 = -(f_p2.y - f_p1.y) * f_inv.x;\n"
"            precise float b0 = (f_p2.x - f_p1.x) * f_inv.x;\n"
"            precise float a1 = -(f_p0.y - f_p2.y) * f_inv.x;\n"
"            precise float b1 = (f_p0.x - f_p2.x) * f_inv.x;\n"
"            ga = vec3(a0, a1, -(a0 + a1));\n"
"            gb = vec3(b0, b1, -(b0 + b1));\n"
"            if (persp) {\n"
"                ga = ga * f_inv.yzw;\n"
"                gb = gb * f_inv.yzw;\n"
"            }\n"
"        }\n"
"        for (int k = 0; k < 8; k++) {\n"
"            us_tc[k] = vec4(0.0, 0.0, 0.0, 1.0);\n"
"            us_der[k] = vec4(0.0);\n"
"        }\n"
"        for (int k = 0; k < US_NTC; k++) {\n"
"            vec4 s0 = texelFetch(u_tcbuf, tb + k * 6);\n"
"            vec4 s1 = texelFetch(u_tcbuf, tb + k * 6 + 2);\n"
"            vec4 s2 = texelFetch(u_tcbuf, tb + k * 6 + 4);\n"
"            precise float is = fma(w2, s2.x, fma(w1, s1.x, w0 * s0.x));\n"
"            precise float it = fma(w2, s2.y, fma(w1, s1.y, w0 * s0.y));\n"
"            precise float ns = is * u_tcinv[k].x;\n"
"            precise float nt = it * u_tcinv[k].y;\n"
"            precise vec4 tc = vec4(ns, nt, 0.0, 1.0);\n"
"            if (u_lodany != 0) {\n"
"                float dx, dy, ex, ey;\n"
"                tc_der(ga, gb, s0.x, s1.x, s2.x, is, iq, dx, dy);\n"
"                tc_der(ga, gb, s0.y, s1.y, s2.y, it, iq, ex, ey);\n"
"                precise float g0 = dx * u_tcinv[k].x;\n"
"                precise float g1 = ex * u_tcinv[k].y;\n"
"                precise float g2 = dy * u_tcinv[k].x;\n"
"                precise float g3 = ey * u_tcinv[k].y;\n"
"                us_der[k] = vec4(g0, g1, g2, g3);\n"
"            }\n"
"            if (((u_tcraw >> k) & 1) != 0) {\n"
"                vec4 r0 = texelFetch(u_tcbuf, tb + k * 6 + 1);\n"
"                vec4 r1 = texelFetch(u_tcbuf, tb + k * 6 + 3);\n"
"                vec4 r2 = texelFetch(u_tcbuf, tb + k * 6 + 5);\n"
"                tc = fma(vec4(w2), r2, fma(vec4(w1), r1, w0 * r0));\n"
"            }\n"
"            us_tc[k] = tc;\n"
"        }\n"
"        vec4 shaded;\n"
"        bool kill;\n"
"        us_main(c, c1, shaded, kill);\n"
"        if (kill) discard;\n"
"        c = shaded;\n"
"    }\n"
"#else\n"
"    vec4 texel = vec4(1.0);\n"
"    if (u_textured != 0 && u_tf[0] != 0) {\n"
"        vec4 der = vec4(0.0);\n"
"        if (u_tf[1] != 0) {\n"
/* r300_raster_tri()'s ga/gb */
"            precise float a0 = -(f_p2.y - f_p1.y) * f_inv.x;\n"
"            precise float b0 = (f_p2.x - f_p1.x) * f_inv.x;\n"
"            precise float a1 = -(f_p0.y - f_p2.y) * f_inv.x;\n"
"            precise float b1 = (f_p0.x - f_p2.x) * f_inv.x;\n"
"            precise vec3 ga = vec3(a0, a1, -(a0 + a1));\n"
"            precise vec3 gb = vec3(b0, b1, -(b0 + b1));\n"
"            if (persp) {\n"
"                ga = ga * f_inv.yzw;\n"
"                gb = gb * f_inv.yzw;\n"
"            }\n"
"            float dx, dy;\n"
"            tc_der(ga, gb, f_t0.x, f_t1.x, f_t2.x, ts, iq, dx, dy);\n"
"            der.x = dx; der.z = dy;\n"
"            tc_der(ga, gb, f_t0.y, f_t1.y, f_t2.y, tt, iq, dx, dy);\n"
"            der.y = dx; der.w = dy;\n"
"        }\n"
"        uvec4 tu = tfilter(ts, tt, der);\n"
"        texel = vec4(u_n255[int(tu.r)], u_n255[int(tu.g)],\n"
"                     u_n255[int(tu.b)], u_n255[int(tu.a)]);\n"
"    } else if (u_textured != 0) {\n"
"        int tx = int(ts);\n"
"        int ty = int(tt);\n"
"        if (u_clamp.x <= 1 && u_texsize.x > 0) {\n"
"            tx = tx % u_texsize.x;\n"
"            if (tx < 0) tx += u_texsize.x;\n"
"        } else {\n"
"            tx = clamp(tx, 0, u_texsize.x - 1);\n"
"        }\n"
"        if (u_clamp.y <= 1 && u_texsize.y > 0) {\n"
"            ty = ty % u_texsize.y;\n"
"            if (ty < 0) ty += u_texsize.y;\n"
"        } else {\n"
"            ty = clamp(ty, 0, u_texsize.y - 1);\n"
"        }\n"
"        uvec4 tu = texelFetch(u_tex, ivec2(tx, ty), 0);\n"
"        texel = vec4(u_n255[int(tu.r)], u_n255[int(tu.g)],\n"
"                     u_n255[int(tu.b)], u_n255[int(tu.a)]);\n"
"    }\n"
"    {\n"
"        vec4 shaded;\n"
"        us_main(texel, c, c1, shaded);\n"
"        c = shaded;\n"
"    }\n"
"#endif\n"
/*
 * GL core profile has no alpha test; DISCARD_SRC_PIXELS has no GL
 * equivalent at all. Both become a `discard`.
 */
"    if (u_alphatest != 0) {\n"
"        bool pass;\n"
"        if (u_affunc == 0)      pass = false;\n"
"        else if (u_affunc == 1) pass = c.a <  u_afref;\n"
"        else if (u_affunc == 2) pass = c.a == u_afref;\n"
"        else if (u_affunc == 3) pass = c.a <= u_afref;\n"
"        else if (u_affunc == 4) pass = c.a >  u_afref;\n"
"        else if (u_affunc == 5) pass = c.a != u_afref;\n"
"        else if (u_affunc == 6) pass = c.a >= u_afref;\n"
"        else                    pass = true;\n"
"        if (!pass) discard;\n"
"    }\n"
"    if (u_discard != 0) {\n"
"        bool a0 = c.a == 0.0, a1 = c.a == 1.0;\n"
"        bool z0 = c.r == 0.0 && c.g == 0.0 && c.b == 0.0;\n"
"        bool z1 = c.r == 1.0 && c.g == 1.0 && c.b == 1.0;\n"
"        bool kill = false;\n"
"        if (u_discard == 1) kill = a0;\n"
"        else if (u_discard == 2) kill = z0;\n"
"        else if (u_discard == 3) kill = a0 && z0;\n"
"        else if (u_discard == 4) kill = a1;\n"
"        else if (u_discard == 5) kill = z1;\n"
"        else if (u_discard == 6) kill = a1 && z1;\n"
"        if (kill) discard;\n"
"    }\n"
"#ifdef R350_ADD\n"
/*
 * The source term alone, in the software rasterizer's own expression --
 * the destination arguments are zero because the caller only routes a
 * draw here when no factor can look at them. GL adds the result to the
 * destination byte, so the byte is quantised once per primitive exactly
 * as the device quantises it.
 */
"    precise float sr = c.r * bf(u_cfac.x, c.r, c.a, 0.0, 0.0,\n"
"                                u_konst.r, u_konst.a);\n"
"    precise float sg = c.g * bf(u_cfac.x, c.g, c.a, 0.0, 0.0,\n"
"                                u_konst.g, u_konst.a);\n"
"    precise float sb = c.b * bf(u_cfac.x, c.b, c.a, 0.0, 0.0,\n"
"                                u_konst.b, u_konst.a);\n"
"    precise float sa = c.a * bf(u_afac.x, c.a, c.a, 0.0, 0.0,\n"
"                                u_konst.a, u_konst.a);\n"
"    vec4 t = clamp(vec4(sr, sg, sb, sa), 0.0, 1.0);\n"
"    o_col = (floor(t * 255.0) + 0.25) / 255.0;\n"
"#else\n"
"    if (u_blend != 0) {\n"
"        uvec4 du = texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0);\n"
"        vec4 d = u_blendread != 0\n"
"                 ? vec4(u_n255[int(du.r)], u_n255[int(du.g)],\n"
"                        u_n255[int(du.b)], u_n255[int(du.a)])\n"
"                 : vec4(0.0);\n"
"        precise float nr = comb(u_cfac.z,\n"
"            c.r * bf(u_cfac.x, c.r, c.a, d.r, d.a, u_konst.r, u_konst.a),\n"
"            d.r * bf(u_cfac.y, c.r, c.a, d.r, d.a, u_konst.r, u_konst.a));\n"
"        precise float ng = comb(u_cfac.z,\n"
"            c.g * bf(u_cfac.x, c.g, c.a, d.g, d.a, u_konst.g, u_konst.a),\n"
"            d.g * bf(u_cfac.y, c.g, c.a, d.g, d.a, u_konst.g, u_konst.a));\n"
"        precise float nb = comb(u_cfac.z,\n"
"            c.b * bf(u_cfac.x, c.b, c.a, d.b, d.a, u_konst.b, u_konst.a),\n"
"            d.b * bf(u_cfac.y, c.b, c.a, d.b, d.a, u_konst.b, u_konst.a));\n"
"        c.a = comb(u_afac.z,\n"
"            c.a * bf(u_afac.x, c.a, c.a, d.a, d.a, u_konst.a, u_konst.a),\n"
"            d.a * bf(u_afac.y, c.a, c.a, d.a, d.a, u_konst.a, u_konst.a));\n"
"        c.r = nr; c.g = ng; c.b = nb;\n"
"    }\n"
/*
 * The device packs with a truncation, not a round. Writing the byte
 * straight out of an integer attachment is that pack, with no
 * normalized round trip in between.
 */
"    o_col = uvec4(floor(clamp(c, 0.0, 1.0) * 255.0));\n"
"#endif\n"
"}\n";

/*
 * The add-blend path's two format conversions, and the full-viewport
 * triangle both are drawn with. The colour buffer is GL_RGBA8UI, which
 * GL's blender is not allowed to touch at all, so a draw that wants the
 * blender works in a normalized copy: bytes out, bytes back.
 *
 * Both directions are exact and that is the whole point of the biases.
 * Out: byte k becomes (k + 0.25)/255, which the normalized attachment
 * stores as k again. Back: the sampled float is k/255 to within an ULP,
 * and +0.5 before the floor turns it into k. Nothing outside the drawn
 * primitives can move, which is what keeps the OUTSIDE class a hard
 * zero for these draws as for every other.
 */
static const char *vs_blit_src =
"#version 330 core\n"
"void main()\n"
"{\n"
"    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char *fs_ui2n_src =
"#version 330 core\n"
"out vec4 o_col;\n"
"uniform usampler2D u_src;\n"
"void main()\n"
"{\n"
"    uvec4 v = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);\n"
"    o_col = (vec4(v) + 0.25) / 255.0;\n"
"}\n";

static const char *fs_n2ui_src =
"#version 330 core\n"
"out uvec4 o_col;\n"
"uniform sampler2D u_src;\n"
"void main()\n"
"{\n"
"    vec4 v = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);\n"
"    o_col = uvec4(floor(v * 255.0 + 0.5));\n"
"}\n";

static GLuint gl_compile(GLenum type, const char *head, const char *mid,
                         const char *src, const char **err)
{
    const char *parts[3];
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    GLsizei n = 0;

    if (head) {
        parts[n++] = head;
    }
    if (mid) {
        parts[n++] = mid;
    }
    parts[n++] = src;
    glShaderSource(sh, n, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(sh);
        *err = "GLSL 3.30 core shader would not compile";
        return 0;
    }
    return sh;
}

static GLuint gl_link(const char *vsrc, const char *fhead, const char *fmid,
                      const char *fsrc, const char **err)
{
    GLuint vs = gl_compile(GL_VERTEX_SHADER, NULL, NULL, vsrc, err);
    GLuint fs = vs ? gl_compile(GL_FRAGMENT_SHADER, fhead, fmid, fsrc, err)
                   : 0;
    GLuint prog;
    GLint ok = 0;

    if (!vs || !fs) {
        if (vs) {
            glDeleteShader(vs);
        }
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        *err = "GLSL program would not link";
        return 0;
    }
    return prog;
}

/* the draw programs differ in one #define; their uniforms are the same */
static void gl_prog_locs(R350GlProg *p)
{
    unsigned k;
    float n255[256];

    glUseProgram(p->prog);
    glUniform1i(glGetUniformLocation(p->prog, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(p->prog, "u_dst"), 1);
    for (k = 0; k < 256; k++) {
        n255[k] = k / 255.0f;
    }
    glUniform1fv(glGetUniformLocation(p->prog, "u_n255"), 256, n255);
    {
        GLint su[R350_GL_TEXUNITS];

        for (k = 0; k < R350_GL_TEXUNITS; k++) {
            su[k] = gl_txunit(k);
        }
        glUniform1iv(glGetUniformLocation(p->prog, "u_txs"),
                     R350_GL_TEXUNITS, su);
        glUniform1i(glGetUniformLocation(p->prog, "u_tcbuf"), R350_GL_TCUNIT);
    }

    p->u_rect = glGetUniformLocation(p->prog, "u_rect");
    p->u_org = glGetUniformLocation(p->prog, "u_org");
    p->u_texsize = glGetUniformLocation(p->prog, "u_texsize");
    p->u_clamp = glGetUniformLocation(p->prog, "u_clamp");
    p->u_textured = glGetUniformLocation(p->prog, "u_textured");
    p->u_alphatest = glGetUniformLocation(p->prog, "u_alphatest");
    p->u_affunc = glGetUniformLocation(p->prog, "u_affunc");
    p->u_afref = glGetUniformLocation(p->prog, "u_afref");
    p->u_discard = glGetUniformLocation(p->prog, "u_discard");
    p->u_blend = glGetUniformLocation(p->prog, "u_blend");
    p->u_blendread = glGetUniformLocation(p->prog, "u_blendread");
    p->u_cfac = glGetUniformLocation(p->prog, "u_cfac");
    p->u_afac = glGetUniformLocation(p->prog, "u_afac");
    p->u_konst = glGetUniformLocation(p->prog, "u_konst");
    p->u_usk = glGetUniformLocation(p->prog, "USK");
    p->u_tf = glGetUniformLocation(p->prog, "u_tf");
    p->u_txsize = glGetUniformLocation(p->prog, "u_txsize");
    p->u_txclamp = glGetUniformLocation(p->prog, "u_txclamp");
    p->u_txtf = glGetUniformLocation(p->prog, "u_txtf");
    p->u_txen = glGetUniformLocation(p->prog, "u_txen");
    p->u_tcbase = glGetUniformLocation(p->prog, "u_tcbase");
    p->u_tcstride = glGetUniformLocation(p->prog, "u_tcstride");
    p->u_tcinv = glGetUniformLocation(p->prog, "u_tcinv");
    p->u_tcraw = glGetUniformLocation(p->prog, "u_tcraw");
    p->u_lodany = glGetUniformLocation(p->prog, "u_lodany");
    p->u_zscale = glGetUniformLocation(p->prog, "u_zscale");
    p->u_zonly = glGetUniformLocation(p->prog, "u_zonly");
}

/*
 * The linked program for one guest fragment program and one blend
 * variant. A miss links; a hit is the ordinary case, because a guest
 * changes fragment program far less often than it draws.
 *
 * A program that will not compile is a REFUSAL, not a fallback to some
 * other shading: the caller renders that draw on the software path,
 * where the interpreter computes the same thing this text does. So a
 * failed link costs correctness nothing and is counted.
 */
/* the linked program for this request if there is one, without linking */
static bool gl_prog_cached(R350GlCtx *g, const R350GlReq *r, bool add)
{
    unsigned k;

    for (k = 0; k < R350_GL_PROGSLOTS; k++) {
        if (g->prog[k].key == r->us_key && g->prog[k].add == add) {
            return true;
        }
    }
    return false;
}

static R350GlProg *gl_prog_for(R350GlCtx *g, const R350GlReq *r, bool add)
{
    const char *err = NULL;
    R350GlProgSlot *sl;
    unsigned k;
    GLuint prog;

    for (k = 0; k < R350_GL_PROGSLOTS; k++) {
        if (g->prog[k].key == r->us_key && g->prog[k].add == add) {
            g->prog_hits++;
            return g->prog[k].p.prog ? &g->prog[k].p : NULL;
        }
    }
    prog = gl_link(vs_src, add ? fs_head_add : fs_head_main,
                   r->us_glsl, fs_src, &err);
    sl = &g->prog[g->prog_next];
    g->prog_next = (g->prog_next + 1) % R350_GL_PROGSLOTS;
    if (sl->p.prog) {
        glDeleteProgram(sl->p.prog);
    }
    memset(sl, 0, sizeof(*sl));
    sl->key = r->us_key;
    sl->add = add;
    sl->p.prog = prog;
    if (!prog) {
        g->prog_failed++;
        return NULL;
    }
    g->prog_links++;
    gl_prog_locs(&sl->p);
    return &sl->p;
}

R350GlCtx *ati_r350_gl_open(const char **err)
{
    R350GlCtx *g;
    unsigned k;

    *err = NULL;
    g = g_new0(R350GlCtx, 1);
    if (!r350_gl_plat_open(&g->plat, err)) {
        g_free(g);
        return NULL;
    }

    g->ui2n = gl_link(vs_blit_src, NULL, NULL, fs_ui2n_src, err);
    g->n2ui = g->ui2n ? gl_link(vs_blit_src, NULL, NULL, fs_n2ui_src, err)
                      : 0;
    if (!g->n2ui) {
        ati_r350_gl_close(g);
        return NULL;
    }
    glUseProgram(g->ui2n);
    glUniform1i(glGetUniformLocation(g->ui2n, "u_src"), 2);
    glUseProgram(g->n2ui);
    glUniform1i(glGetUniformLocation(g->n2ui, "u_src"), 2);

    glGenVertexArrays(1, &g->vao_blit);
    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenBuffers(1, &g->vbo);
    glGenFramebuffers(1, &g->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glGenTextures(1, &g->cbuf);
    glGenTextures(R350_GL_TEXSLOTS + 1, g->tex);
    glGenTextures(1, &g->white);
    glGenTextures(1, &g->dst);
    glGenTextures(1, &g->acc);
    glGenTextures(1, &g->zbuf);
    g->has_z = true;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    {
        /*
         * What an untextured draw samples. Specified once: giving a
         * texture new storage is a synchronisation point, and doing it
         * per draw cost 1.4 ms of caller time for every three draws.
         */
        static const uint8_t white[4] = { 255, 255, 255, 255 };

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 1, 1, 0,
                     GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, white);
    }
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    {
        /*
         * Sixteen attributes exactly, which is all GL 3.3 core
         * guarantees -- see the coordinate-set note in ati_r350_gl.h.
         * Every offset is derived from C rather than written out, so
         * the table and R350_GL_VSTRIDE cannot drift apart.
         */
        const int C = R350_GL_TEXCOORDS;
        const struct { GLint loc, n, off; } at[] = {
            { 0, 2, 0 }, { 1, 4, 2 }, { 2, 4, 41 + 8 * C },
            { 3, 2, 6 + 2 * C }, { 4, 2, 8 + 2 * C }, { 5, 2, 10 + 2 * C },
            { 6, 4, 12 + 2 * C }, { 7, 4, 16 + 2 * C }, { 8, 4, 20 + 2 * C },
            { 9, 2 * C, 24 + 2 * C }, { 10, 2 * C, 24 + 4 * C },
            { 11, 2 * C, 24 + 6 * C },
            { 12, 4, 37 + 8 * C },
            { 13, 4, 25 + 8 * C }, { 14, 4, 29 + 8 * C },
            { 15, 4, 33 + 8 * C },
        };

        glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        for (k = 0; k < ARRAY_SIZE(at); k++) {
            glEnableVertexAttribArray(at[k].loc);
            glVertexAttribPointer(at[k].loc, at[k].n, GL_FLOAT, GL_FALSE,
                                  R350_GL_VSTRIDE * 4,
                                  (void *)(size_t)(at[k].off * 4));
        }
    }

    {
        GLint mx = 0;

        glGenBuffers(1, &g->tcbo);
        glGenTextures(1, &g->tctex);
        glBindBuffer(GL_TEXTURE_BUFFER, g->tcbo);
        glBufferData(GL_TEXTURE_BUFFER, 16, NULL, GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + R350_GL_TCUNIT);
        glBindTexture(GL_TEXTURE_BUFFER, g->tctex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, g->tcbo);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &mx);
        g->tc_max = mx > 0 ? (size_t)mx : 0;
    }

    g->barrier = r350_gl_plat_has_barrier();
    snprintf(g->desc, sizeof(g->desc), R350_GL_BACKEND_NAME ", %s / GLSL %s%s",
             (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION),
             g->barrier ? ", texture barrier" : "");
    if (glGetError() != GL_NO_ERROR) {
        ati_r350_gl_close(g);
        *err = "GL reported an error while setting the backend up";
        return NULL;
    }
    r350_gl_done(&g->plat);
    return g;
}

void ati_r350_gl_close(R350GlCtx *g)
{
    if (!g) {
        return;
    }
    if (g->plat.ctx) {
        unsigned k;

        r350_gl_makecurrent(&g->plat);
        gl_flush_queue(g);
        glDeleteTextures(1, &g->dst);
        glDeleteTextures(1, &g->acc);
        glDeleteTextures(1, &g->zbuf);
        glDeleteTextures(R350_GL_TEXSLOTS + 1, g->tex);
        glDeleteTextures(1, &g->white);
        glDeleteTextures(1, &g->cbuf);
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteBuffers(1, &g->vbo);
        glDeleteBuffers(1, &g->tcbo);
        glDeleteTextures(1, &g->tctex);
        glDeleteVertexArrays(1, &g->vao);
        glDeleteVertexArrays(1, &g->vao_blit);
        for (k = 0; k < R350_GL_PROGSLOTS; k++) {
            if (g->prog[k].p.prog) {
                glDeleteProgram(g->prog[k].p.prog);
            }
        }
        glDeleteProgram(g->ui2n);
        glDeleteProgram(g->n2ui);
        r350_gl_plat_close(&g->plat);
    }
    g_free(g->stage);
    g_free(g->q);
    g_free(g->qv);
    g_free(g->qt);
    g_free(g);
}

const char *ati_r350_gl_describe(R350GlCtx *g)
{
    return g ? g->desc : "none";
}

/*
 * The shader cache, for `gl-stats`. A link count near the draw count
 * means the caller's key is changing when the program is not, which is
 * a real cost: relinking a GLSL program mid-frame is a pipeline stall.
 */
void ati_r350_gl_prog_stats(R350GlCtx *g, uint64_t *hits, uint64_t *links,
                            uint64_t *failed)
{
    *hits = g ? g->prog_hits : 0;
    *links = g ? g->prog_links : 0;
    *failed = g ? g->prog_failed : 0;
}

uint64_t ati_r350_gl_barriers(R350GlCtx *g)
{
    return g ? g->barriers : 0;
}

void ati_r350_gl_queue_stats(R350GlCtx *g, uint64_t *units, uint64_t *flushes,
                             uint64_t *waves)
{
    *units = g ? g->q_units : 0;
    *flushes = g ? g->q_flushes : 0;
    *waves = g ? g->q_waves : 0;
}

void ati_r350_gl_hold(bool hold)
{
    r350_gl_plat_hold(hold);
}

void ati_r350_gl_unbind(R350GlCtx *g)
{
    if (g) {
        r350_gl_plat_unbind(&g->plat);
    }
}

/*
 * Emulated VRAM stores a pixel with its bytes permuted by the aperture
 * swapper's xor: byte (2^xr) is red, (1^xr) green, (0^xr) blue and
 * (3^xr) alpha. GL can be asked for two of the four orders directly --
 * GL_BGRA_INTEGER with GL_UNSIGNED_BYTE is xr 0 and with
 * GL_UNSIGNED_INT_8_8_8_8 is xr 3, probed rather than reasoned about
 * (doc/radeon9800/glbench/fmtprobe.c) -- so a transfer could be a
 * straight DMA at the target's own pitch with no per-pixel work.
 *
 * It is not worth having, and that is a MEASUREMENT rather than a
 * preference. On this host, 1024x768 each way:
 *
 *   seed   packed xr3 1.51 ms   BGRA bytes xr0 1.12 ms   staged 0.55 ms
 *   fetch  packed xr3 1.81 ms   BGRA bytes xr0 1.02 ms   staged 1.00 ms
 *
 * The driver's packed-format paths are slower than reading plain RGBA
 * bytes and permuting them on the CPU, by two to three times. So every
 * xor goes through the same staging buffer, which is also one code path
 * instead of three and one less thing to be portable about. The two
 * implementations were checked against each other first: a round trip
 * through the packed format and through the staging permute disagree on
 * 0 of 3145728 bytes.
 */
static uint8_t *gl_stage(R350GlCtx *g, size_t need)
{
    if (need > g->stage_sz) {
        g->stage = g_realloc(g->stage, need);
        g->stage_sz = need;
    }
    return g->stage;
}

bool ati_r350_gl_target(R350GlCtx *g, int w, int h, bool *lost)
{
    *lost = false;
    if (!g || w <= 0 || h <= 0) {
        return false;
    }
    if (w <= g->fb_w && h <= g->fb_h) {
        return true;
    }
    /*
     * Grow only, and never shrink: a target that alternates between two
     * sizes would otherwise throw its contents away on every change.
     * Growing does lose them, and the caller is told so.
     */
    w = MAX(w, g->fb_w);
    h = MAX(h, g->fb_h);
    if (w > 16384 || h > 16384) {
        return false;
    }
    r350_gl_makecurrent(&g->plat);
    gl_flush_queue(g);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->cbuf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, w, h, 0, GL_RGBA_INTEGER,
                 GL_UNSIGNED_BYTE, NULL);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, w, h, 0, GL_RGBA_INTEGER,
                 GL_UNSIGNED_BYTE, NULL);
    /*
     * The normalized twin the add-blend path renders into. It holds
     * nothing between draws -- each such draw copies the colour buffer
     * into it and copies the result back -- so growing it loses nothing.
     */
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g->acc);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    g->att = 0;
    gl_attach(g, g->cbuf);
    gl_wrote(g, 0, 0, w, h);
    if (glGetError() == GL_NO_ERROR && g->has_z) {
        /*
         * The depth buffer, the same size. A host that cannot give one
         * keeps the colour target and refuses depth-tested draws.
         */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g->zbuf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, w, h, 0,
                     GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV,
                     NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                               GL_TEXTURE_2D, g->zbuf, 0);
        if (glGetError() != GL_NO_ERROR ||
            glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                   GL_TEXTURE_2D, 0, 0);
            g->has_z = false;
        }
        g->zset = false;
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        g->fb_w = g->fb_h = 0;
        r350_gl_done(&g->plat);
        return false;
    }
    g->fb_w = w;
    g->fb_h = h;
    *lost = true;
    r350_gl_done(&g->plat);
    return true;
}

bool ati_r350_gl_seed(R350GlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr)
{
    uint8_t *st;
    bool ok;
    int x, y;

    if (!g || w <= 0 || h <= 0 ||
        x0 < 0 || y0 < 0 || x0 + w > g->fb_w || y0 + h > g->fb_h) {
        return false;
    }
    st = gl_stage(g, (size_t)w * h * 4);
    for (y = 0; y < h; y++) {
        const uint8_t *p = base + (size_t)(y0 + y) * pitch + (size_t)x0 * 4;
        uint8_t *o = st + (size_t)y * w * 4;

        for (x = 0; x < w; x++, p += 4, o += 4) {
            o[0] = p[2 ^ xr];           /* R */
            o[1] = p[1 ^ xr];           /* G */
            o[2] = p[0 ^ xr];           /* B */
            o[3] = p[3 ^ xr];           /* A */
        }
    }
    /*
     * No queue flush: a seed only ever covers pixels outside what was
     * seeded before, and every queued draw lies inside that.
     */
    r350_gl_makecurrent(&g->plat);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->cbuf);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_RGBA_INTEGER,
                    GL_UNSIGNED_BYTE, st);
    gl_wrote(g, x0, y0, x0 + w, y0 + h);
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

bool ati_r350_gl_fetch(R350GlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr)
{
    uint8_t *st;
    bool ok;
    int x, y;

    if (!g || w <= 0 || h <= 0 ||
        x0 < 0 || y0 < 0 || x0 + w > g->fb_w || y0 + h > g->fb_h) {
        return false;
    }
    st = gl_stage(g, (size_t)w * h * 4);
    r350_gl_makecurrent(&g->plat);
    gl_flush_queue(g);
    gl_attach(g, g->cbuf);
    glReadPixels(x0, y0, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, st);
    for (y = 0; y < h; y++) {
        uint8_t *p = base + (size_t)(y0 + y) * pitch + (size_t)x0 * 4;
        const uint8_t *i = st + (size_t)y * w * 4;

        for (x = 0; x < w; x++, p += 4, i += 4) {
            p[2 ^ xr] = i[0];
            p[1 ^ xr] = i[1];
            p[0 ^ xr] = i[2];
            p[3 ^ xr] = i[3];
        }
    }
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

/*
 * THE DRAW QUEUE.
 *
 * With the texture barrier, a draw costs a couple of microseconds, and a
 * barrier -- a new render pass, which on this host is a new Metal command
 * buffer -- about fifty. A run of translucent overlapping quads takes one
 * barrier per quad in submission order. It needs far fewer: only draws
 * that overlap have an order to keep. So draws are queued and put in
 * waves: each goes after the latest earlier draw it overlaps -- one wave
 * later if either of the two reads the destination, the same wave (and
 * after it, since a wave is emitted in submission order) if neither
 * does. A wave is one pass; a barrier separates waves. Every pixel still
 * sees the draws that cover it in submission order, each reading what the
 * one before it left, so the result is the one the draws would give one
 * by one.
 *
 * "Overlaps" is a rectangle test on the pixels a draw can cover: those
 * whose centres lie inside its vertices' extent, widened by a margin for
 * the vertex transform's rounding, within its scissor. A pass of a
 * self-overlapping draw is queued as a draw of its own.
 *
 * The queue is emptied before anything else touches the colour buffer or
 * a texture a queued draw samples, and before a program is linked --
 * except a seed, which only writes pixels no queued draw covers.
 */
#define R350_GL_QUEUE 1024
#define R350_GL_QUEUE_VERTS (256 * 1024)

/* a request's general-form state, flattened the way the uniforms take it */
typedef struct R350GlGen {
    uint32_t textured;
    unsigned slot[R350_GL_TEXUNITS];
    GLint size[R350_GL_TEXUNITS * 2], clamp[R350_GL_TEXUNITS * 2];
    GLint tf[R350_GL_TEXUNITS * R350_GL_TF];
    float tcinv[R350_GL_TCSETS * 2];
    int tcstride, tcraw, lodany;
} R350GlGen;

typedef struct R350GlUnit {
    const R350GlProg *p;
    unsigned first, count;
    int rx0, ry0, rx1, ry1;             /* pixels it may write */
    int sx0, sy0, sx1, sy1;             /* its scissor */
    bool reads;
    int wave;
    int surf_w, surf_h;
    unsigned tex_slot;                  /* > R350_GL_TEXSLOTS: white */
    int tex_w, tex_h, clamp_s, clamp_t, textured;
    GLint tf[R350_GL_TF];
    uint32_t wmask;
    int alpha_test, af_func, discard;
    float af_ref;
    int blend, blend_read;
    int src_factor, dst_factor, comb_fcn;
    int a_src_factor, a_dst_factor, a_comb_fcn;
    float k_r, k_g, k_b, k_a;
    bool usk;
    float usk_v[R350_GL_USK * 4];
    /* a general-form program's units and coordinate block */
    bool gen;
    int tcbase;
    R350GlGen ge;
    R350GlZ z;
    int zonly;
} R350GlUnit;

/* one unit's filter uniform: `filt`, then the border colour */
static void gl_tf_unit(const R350GlReq *r, unsigned un, GLint *tf)
{
    unsigned k;

    for (k = 0; k < 11; k++) {
        tf[k] = r->filt[un][k];
    }
    for (k = 0; k < 4; k++) {
        tf[11 + k] = r->border[un][k];
    }
    tf[15] = 0;
}

static void gl_tf(const R350GlReq *r, GLint *tf)
{
    gl_tf_unit(r, 0, tf);
}

static int gl_levels_unit(const R350GlReq *r, unsigned un)
{
    return r->filt[un][0] ? MAX(r->levels[un], 1) : 1;
}

static int gl_levels(const R350GlReq *r)
{
    return gl_levels_unit(r, 0);
}

/*
 * Unit 0's texture into the bound object: every level the request
 * carries, each max(w >> l, 1) x max(h >> l, 1). The shader reads them
 * with texelFetch, which needs the chain complete, not filterable.
 */
static void gl_upload_tex_unit(R350GlCtx *g, unsigned slot,
                               const R350GlReq *r, unsigned un)
{
    int nl = gl_levels_unit(r, un), l;
    bool same = g->tex_w[slot] == r->tex_w[un] &&
                g->tex_h[slot] == r->tex_h[un] && g->tex_nl[slot] == nl;
    const uint8_t *p = r->tex[un];

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    nl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nl - 1);
    for (l = 0; l < nl; l++) {
        int w = MAX(r->tex_w[un] >> l, 1), h = MAX(r->tex_h[un] >> l, 1);

        if (same) {
            glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, w, h, GL_RGBA_INTEGER,
                            GL_UNSIGNED_BYTE, p);
        } else {
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8UI, w, h, 0,
                         GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, p);
        }
        p += (size_t)w * h * 4;
    }
    g->tex_w[slot] = r->tex_w[un];
    g->tex_h[slot] = r->tex_h[un];
    g->tex_nl[slot] = nl;
}

static void gl_upload_tex(R350GlCtx *g, unsigned slot, const R350GlReq *r)
{
    gl_upload_tex_unit(g, slot, r, 0);
}

static bool gl_rect_meet(int ax0, int ay0, int ax1, int ay1,
                         int bx0, int by0, int bx1, int by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

static bool gl_queue_uses_slot(R350GlCtx *g, unsigned slot)
{
    unsigned k;

    for (k = 0; k < g->nq; k++) {
        const R350GlUnit *u = &g->q[k];
        unsigned un;

        if (u->tex_slot == slot) {
            return true;
        }
        for (un = 0; u->gen && un < R350_GL_TEXUNITS; un++) {
            if ((u->ge.textured & (1u << un)) && u->ge.slot[un] == slot) {
                return true;
            }
        }
    }
    return false;
}

/*
 * The general form's textures: every unit the request samples, each
 * into the slot the caller named, uploaded once when several units share
 * one. False when the request cannot be bound -- two units wanting the
 * scratch, or a slot the caller called current that is not.
 * `queue` empties the queue before a slot a queued draw reads is
 * overwritten.
 */
static bool gl_bind_units(R350GlCtx *g, const R350GlReq *r, bool queue)
{
    uint32_t done = 0;
    unsigned un, k;

    for (un = 0; un < R350_GL_TEXUNITS; un++) {
        unsigned sl = r->tex_slot[un];

        if (!(r->textured & (1u << un))) {
            continue;
        }
        if (sl > R350_GL_TEXSLOTS) {
            return false;
        }
        for (k = 0; k < un; k++) {
            if ((r->textured & (1u << k)) && r->tex_slot[k] == sl &&
                (sl == R350_GL_TEXSLOTS || r->tex[k] != r->tex[un])) {
                /* one scratch, or one slot named for two textures */
                return false;
            }
        }
        if (r->tex_fresh[un] && r->tex[un]) {
            if (done & (1u << un)) {
                continue;
            }
            for (k = un + 1; k < R350_GL_TEXUNITS; k++) {
                if ((r->textured & (1u << k)) && r->tex_slot[k] == sl) {
                    done |= 1u << k;
                }
            }
            if (queue && gl_queue_uses_slot(g, sl)) {
                gl_flush_queue(g);
            }
            glActiveTexture(GL_TEXTURE0 + gl_txunit(un));
            glBindTexture(GL_TEXTURE_2D, g->tex[sl]);
            gl_upload_tex_unit(g, sl, r, un);
        } else if (g->tex_w[sl] != r->tex_w[un] ||
                   g->tex_h[sl] != r->tex_h[un] ||
                   g->tex_nl[sl] != gl_levels_unit(r, un)) {
            return false;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    return true;
}

/* bind every guest unit's texture, white where the request samples none */
static void gl_bind_txunits(R350GlCtx *g, uint32_t textured,
                            const unsigned *slots)
{
    unsigned un;

    for (un = 0; un < R350_GL_TEXUNITS; un++) {
        glActiveTexture(GL_TEXTURE0 + gl_txunit(un));
        glBindTexture(GL_TEXTURE_2D, (textured & (1u << un)) &&
                      slots[un] <= R350_GL_TEXSLOTS
                      ? g->tex[slots[un]] : g->white);
    }
    glActiveTexture(GL_TEXTURE0);
}

/* the general form's uniforms that do not change within a request */
static void gl_gen_uniforms(const R350GlProg *p, uint32_t textured,
                            const GLint *size, const GLint *clamp,
                            const GLint *tf, const float *tcinv,
                            int tcbase, int tcstride, int tcraw, int lodany)
{
    glUniform2iv(p->u_txsize, R350_GL_TEXUNITS, size);
    glUniform2iv(p->u_txclamp, R350_GL_TEXUNITS, clamp);
    glUniform1iv(p->u_txtf, R350_GL_TEXUNITS * R350_GL_TF, tf);
    glUniform1i(p->u_txen, (GLint)textured);
    glUniform2fv(p->u_tcinv, R350_GL_TCSETS, tcinv);
    glUniform1i(p->u_tcbase, tcbase);
    glUniform1i(p->u_tcstride, tcstride);
    glUniform1i(p->u_tcraw, tcraw);
    glUniform1i(p->u_lodany, lodany);
}

static void gl_gen_from(R350GlGen *e, const R350GlReq *r)
{
    unsigned un;

    e->textured = r->textured;
    for (un = 0; un < R350_GL_TEXUNITS; un++) {
        e->slot[un] = r->tex_slot[un];
        e->size[un * 2] = r->tex_w[un];
        e->size[un * 2 + 1] = r->tex_h[un];
        e->clamp[un * 2] = r->clamp_s[un];
        e->clamp[un * 2 + 1] = r->clamp_t[un];
        gl_tf_unit(r, un, &e->tf[un * R350_GL_TF]);
    }
    for (un = 0; un < R350_GL_TCSETS; un++) {
        e->tcinv[un * 2] = r->tcinv[un][0];
        e->tcinv[un * 2 + 1] = r->tcinv[un][1];
    }
    e->tcstride = (int)r->tc_stride;
    e->tcraw = (int)r->tc_raw;
    e->lodany = r->lod_any;
}


static void gl_emit(R350GlCtx *g, const R350GlUnit *u, const R350GlUnit *prev)
{
    const R350GlProg *p = u->p;

    if (!prev || prev->p != p) {
        glUseProgram(p->prog);
    }
    if (p->u_usk >= 0 && u->usk) {
        glUniform4fv(p->u_usk, R350_GL_USK, u->usk_v);
    }
    if (!prev || prev->surf_w != u->surf_w || prev->surf_h != u->surf_h) {
        glViewport(0, 0, u->surf_w, u->surf_h);
    }
    if (u->gen) {
        gl_bind_txunits(g, u->ge.textured, u->ge.slot);
        gl_gen_uniforms(p, u->ge.textured, u->ge.size, u->ge.clamp,
                        u->ge.tf, u->ge.tcinv, u->tcbase, u->ge.tcstride,
                        u->ge.tcraw, u->ge.lodany);
    } else if (!prev || prev->gen || prev->tex_slot != u->tex_slot) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, u->tex_slot <= R350_GL_TEXSLOTS
                      ? g->tex[u->tex_slot] : g->white);
    }
    glUniform4f(p->u_rect, 0.0f, 0.0f, (float)u->surf_w, (float)u->surf_h);
    glUniform2f(p->u_org, 0.0f, 0.0f);
    glUniform2i(p->u_texsize, u->tex_w, u->tex_h);
    glUniform2i(p->u_clamp, u->clamp_s, u->clamp_t);
    glUniform1iv(p->u_tf, R350_GL_TF, u->tf);
    glUniform1i(p->u_textured, u->textured);
    glUniform1i(p->u_alphatest, u->alpha_test);
    glUniform1i(p->u_affunc, u->af_func);
    glUniform1f(p->u_afref, u->af_ref);
    glUniform1i(p->u_discard, u->discard);
    glUniform1i(p->u_blend, u->blend);
    glUniform1i(p->u_blendread, u->blend_read);
    glUniform3i(p->u_cfac, u->src_factor, u->dst_factor, u->comb_fcn);
    glUniform3i(p->u_afac, u->a_src_factor, u->a_dst_factor, u->a_comb_fcn);
    glUniform4f(p->u_konst, u->k_r, u->k_g, u->k_b, u->k_a);
    glScissor(u->sx0, u->sy0, MAX(u->sx1 - u->sx0, 0),
              MAX(u->sy1 - u->sy0, 0));
    glColorMask(!!(u->wmask & 0x00ff0000), !!(u->wmask & 0x0000ff00),
                !!(u->wmask & 0x000000ff), !!(u->wmask & 0xff000000));
    glUniform1f(p->u_zscale, gl_zscale(&u->z));
    glUniform1i(p->u_zonly, u->zonly);
    gl_zstate(g, &u->z);
    glDrawArrays(GL_TRIANGLES, (GLint)u->first, (GLsizei)u->count);
}

static void gl_flush_queue(R350GlCtx *g)
{
    const R350GlUnit *prev = NULL;
    int w, maxw = 0;
    unsigned k;

    if (!g->nq) {
        return;
    }
    for (k = 0; k < g->nq; k++) {
        maxw = MAX(maxw, g->q[k].wave);
    }
    gl_attach(g, g->cbuf);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * g->nqv),
                 g->qv, GL_STREAM_DRAW);
    if (g->nqt) {
        glBindBuffer(GL_TEXTURE_BUFFER, g->tcbo);
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(sizeof(float) * g->nqt),
                     g->qt, GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + R350_GL_TCUNIT);
        glBindTexture(GL_TEXTURE_BUFFER, g->tctex);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->cbuf);
    glEnable(GL_SCISSOR_TEST);
    if (g->q_prebarrier) {
        gl_barrier(g);
    }
    for (w = 0; w <= maxw; w++) {
        if (w) {
            gl_barrier(g);
        }
        for (k = 0; k < g->nq; k++) {
            const R350GlUnit *u = &g->q[k];

            if (u->wave == w) {
                gl_emit(g, u, prev);
                gl_wrote(g, u->rx0, u->ry0, u->rx1, u->ry1);
                prev = u;
            }
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    gl_zstate(g, NULL);
    glActiveTexture(GL_TEXTURE0);
    g->q_flushes++;
    g->q_waves += maxw + 1;
    g->nq = 0;
    g->nqv = 0;
    g->nqt = 0;
    g->q_prebarrier = false;
}

/* queue one pass of a draw: vertices [v0, v0 + nv) of the request */
static void gl_enqueue_pass(R350GlCtx *g, const R350GlReq *r,
                            const R350GlProg *p, unsigned v0, unsigned nv,
                            unsigned slot)
{
    R350GlUnit *u;
    float fx0 = FLT_MAX, fy0 = FLT_MAX, fx1 = -FLT_MAX, fy1 = -FLT_MAX;
    size_t need = (size_t)nv * R350_GL_VSTRIDE;
    size_t tneed = r->tcx ? (size_t)nv / 3 * r->tc_stride * 4 : 0;
    unsigned k;

    if (g->nq == R350_GL_QUEUE || g->nqv + need > g->qv_cap ||
        (g->nqt + tneed) / 4 > g->tc_max) {
        gl_flush_queue(g);
        if (need > g->qv_cap) {
            g->qv_cap = MAX(need, (size_t)R350_GL_QUEUE_VERTS);
            g->qv = g_renew(float, g->qv, g->qv_cap);
        }
    }
    if (g->nqt + tneed > g->qt_cap) {
        g->qt_cap = MAX(g->nqt + tneed, 2 * g->qt_cap);
        g->qt = g_renew(float, g->qt, g->qt_cap);
    }
    if (!g->q) {
        g->q = g_new(R350GlUnit, R350_GL_QUEUE);
    }
    u = &g->q[g->nq];
    memcpy(g->qv + g->nqv, r->verts + (size_t)v0 * R350_GL_VSTRIDE,
           need * sizeof(float));
    for (k = 0; k < nv; k++) {
        const float *v = r->verts + (size_t)(v0 + k) * R350_GL_VSTRIDE;

        fx0 = MIN(fx0, v[0]); fx1 = MAX(fx1, v[0]);
        fy0 = MIN(fy0, v[1]); fy1 = MAX(fy1, v[1]);
    }
    u->p = p;
    u->first = g->nqv / R350_GL_VSTRIDE;
    u->count = nv;
    u->gen = r->tcx != NULL;
    if (u->gen) {
        memcpy(g->qt + g->nqt, r->tcx + (size_t)v0 / 3 * r->tc_stride * 4,
               tneed * sizeof(float));
        u->tcbase = (int)(g->nqt / 4);
        g->nqt += tneed;
        gl_gen_from(&u->ge, r);
    }
    u->sx0 = MAX(r->sx0, r->x0);
    u->sy0 = MAX(r->sy0, r->y0);
    u->sx1 = MIN(r->sx1, r->x0 + r->w);
    u->sy1 = MIN(r->sy1, r->y0 + r->h);
    /*
     * The pixels whose centres lie inside the extent, give or take a
     * sixteenth of a pixel for the vertex transform's rounding.
     */
    u->rx0 = MAX(u->sx0, (int)ceilf(fx0 - 0.5f - 0.0625f));
    u->ry0 = MAX(u->sy0, (int)ceilf(fy0 - 0.5f - 0.0625f));
    u->rx1 = MIN(u->sx1, (int)floorf(fx1 - 0.5f + 0.0625f) + 1);
    u->ry1 = MIN(u->sy1, (int)floorf(fy1 - 0.5f + 0.0625f) + 1);
    if (!isfinite(fx0) || !isfinite(fy0) || !isfinite(fx1) ||
        !isfinite(fy1)) {
        u->rx0 = u->sx0; u->ry0 = u->sy0;
        u->rx1 = u->sx1; u->ry1 = u->sy1;
    }
    u->reads = r->blend && r->blend_read;
    u->surf_w = r->surf_w;
    u->surf_h = r->surf_h;
    u->tex_slot = u->gen ? R350_GL_TEXSLOTS + 1 : slot;
    u->tex_w = r->tex_w[0];
    u->tex_h = r->tex_h[0];
    u->clamp_s = r->clamp_s[0];
    u->clamp_t = r->clamp_t[0];
    gl_tf(r, u->tf);
    u->textured = r->textured & 1;
    u->wmask = r->wmask;
    u->alpha_test = r->alpha_test;
    u->af_func = r->af_func;
    u->af_ref = r->af_ref;
    u->discard = r->discard;
    u->blend = r->blend;
    u->blend_read = r->blend_read;
    u->src_factor = r->src_factor;
    u->dst_factor = r->dst_factor;
    u->comb_fcn = r->comb_fcn;
    u->a_src_factor = r->a_src_factor;
    u->a_dst_factor = r->a_dst_factor;
    u->a_comb_fcn = r->a_comb_fcn;
    u->k_r = r->k_r; u->k_g = r->k_g; u->k_b = r->k_b; u->k_a = r->k_a;
    u->usk = r->us_konst != NULL;
    if (u->usk) {
        memcpy(u->usk_v, r->us_konst, sizeof(u->usk_v));
    }
    u->z = r->z;
    u->zonly = r->zonly;
    u->wave = 0;
    if (u->rx1 > u->rx0 && u->ry1 > u->ry0) {
        for (k = 0; k < g->nq; k++) {
            const R350GlUnit *e = &g->q[k];

            if (gl_rect_meet(u->rx0, u->ry0, u->rx1, u->ry1,
                             e->rx0, e->ry0, e->rx1, e->ry1)) {
                /*
                 * A pass may sample a pixel no draw in it writes, or one
                 * that only the sampling fragment writes; so a pair of
                 * which either reads is a barrier apart.
                 */
                u->wave = MAX(u->wave, e->wave +
                                       (u->reads || e->reads ? 1 : 0));
            }
        }
        if (u->reads && !u->wave) {
            /* what was drawn before the queue */
            for (k = 0; k < g->nwr && !g->q_prebarrier; k++) {
                g->q_prebarrier = gl_rect_meet(u->rx0, u->ry0, u->rx1, u->ry1,
                                               g->wr[k].x0, g->wr[k].y0,
                                               g->wr[k].x1, g->wr[k].y1);
            }
        }
    }
    g->nqv += need;
    g->nq++;
    g->q_units++;
}

static bool gl_enqueue(R350GlCtx *g, const R350GlReq *r)
{
    const R350GlProg *p;
    unsigned slot = R350_GL_TEXSLOTS + 1, k;

    if (!gl_prog_cached(g, r, false)) {
        gl_flush_queue(g);              /* a link may evict a queued one */
    }
    p = gl_prog_for(g, r, false);
    if (!p) {
        return false;
    }
    if (r->tcx) {
        if ((size_t)r->nvert / 3 * r->tc_stride > g->tc_max ||
            !gl_bind_units(g, r, true)) {
            return false;
        }
    } else if ((r->textured & 1) && r->tex_slot[0] <= R350_GL_TEXSLOTS) {
        slot = r->tex_slot[0];
        if (r->tex_fresh[0] && r->tex[0]) {
            if (gl_queue_uses_slot(g, slot)) {
                gl_flush_queue(g);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g->tex[slot]);
            gl_upload_tex(g, slot, r);
        } else if (g->tex_w[slot] != r->tex_w[0] ||
                   g->tex_h[slot] != r->tex_h[0] ||
                   g->tex_nl[slot] != gl_levels(r)) {
            return false;               /* see ati_r350_gl_draw() */
        }
    }
    if (r->npass > 1) {
        for (k = 0; k < r->npass; k++) {
            gl_enqueue_pass(g, r, p, r->pass[k], r->pass[k + 1] - r->pass[k],
                            slot);
        }
    } else {
        gl_enqueue_pass(g, r, p, 0, r->nvert, slot);
    }
    return glGetError() == GL_NO_ERROR;
}

/*
 * Depth words to and from the depth buffer. A Z value z travels as the
 * float z * 2^-24 and the stencil in the low byte of the second word,
 * which is GL_FLOAT_32_UNSIGNED_INT_24_8_REV; both conversions are exact.
 */
#define R350_GL_ZUNIT (1.0f / 16777216.0f)

static void gl_zread(R350GlCtx *g, int x0, int y0, int w, int h,
                     uint32_t *z, int mode)
{
    size_t n = (size_t)w * h, i;
    uint32_t *st = (uint32_t *)gl_stage(g, n * 8);

    glReadPixels(x0, y0, w, h, GL_DEPTH_STENCIL,
                 GL_FLOAT_32_UNSIGNED_INT_24_8_REV, st);
    for (i = 0; i < n; i++) {
        float f;
        uint32_t zi;

        memcpy(&f, &st[2 * i], sizeof(f));
        zi = (uint32_t)(f * 16777216.0f);
        z[i] = mode == 2 ? zi & 0xffff : (zi << 8) | (st[2 * i + 1] & 0xff);
    }
}

bool ati_r350_gl_depth(R350GlCtx *g)
{
    return g && g->has_z;
}

static bool gl_zrect_ok(R350GlCtx *g, int x0, int y0, int w, int h)
{
    return g && g->has_z && w > 0 && h > 0 && x0 >= 0 && y0 >= 0 &&
           x0 + w <= g->fb_w && y0 + h <= g->fb_h;
}

bool ati_r350_gl_zseed(R350GlCtx *g, int x0, int y0, int w, int h,
                       const uint32_t *z, int mode)
{
    size_t n = (size_t)w * h, i;
    uint32_t *st;
    bool ok;

    if (!gl_zrect_ok(g, x0, y0, w, h)) {
        return false;
    }
    st = (uint32_t *)gl_stage(g, n * 8);
    for (i = 0; i < n; i++) {
        uint32_t zi = mode == 2 ? z[i] & 0xffff : z[i] >> 8;
        float f = (float)zi * R350_GL_ZUNIT;

        memcpy(&st[2 * i], &f, sizeof(f));
        st[2 * i + 1] = mode == 2 ? 0 : z[i] & 0xff;
    }
    r350_gl_makecurrent(&g->plat);
    /* a queued draw may test against what is about to be replaced */
    gl_flush_queue(g);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->zbuf);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_DEPTH_STENCIL,
                    GL_FLOAT_32_UNSIGNED_INT_24_8_REV, st);
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

bool ati_r350_gl_zfetch(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t *z, int mode)
{
    bool ok;

    if (!gl_zrect_ok(g, x0, y0, w, h)) {
        return false;
    }
    r350_gl_makecurrent(&g->plat);
    gl_flush_queue(g);
    gl_zread(g, x0, y0, w, h, z, mode);
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

bool ati_r350_gl_zclear(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t z, int mode)
{
    uint32_t zi = mode == 2 ? z & 0xffff : z >> 8;
    bool ok;

    if (!gl_zrect_ok(g, x0, y0, w, h)) {
        return false;
    }
    r350_gl_makecurrent(&g->plat);
    gl_flush_queue(g);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0, y0, w, h);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    g->zset = false;
    glClearBufferfi(GL_DEPTH_STENCIL, 0,
                    (float)zi * R350_GL_ZUNIT,
                    mode == 2 ? 0 : (GLint)(z & 0xff));
    glDisable(GL_SCISSOR_TEST);
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *r)
{
    const R350GlProg *p;
    int sx0, sy0, sx1, sy1;
    bool ok;

    if (!g || r->w <= 0 || r->h <= 0 || !r->nvert ||
        r->surf_w > g->fb_w || r->surf_h > g->fb_h ||
        (r->z.mode && !g->has_z)) {
        return false;
    }
    /*
     * Made current per draw rather than once: the device may reach this
     * from more than one thread, one at a time. On darwin the call is a
     * no-op when the context is already current and the paired
     * r350_gl_done() is nothing; on Windows both are real unless this
     * thread holds the context, and why is in the win32 leg's threading
     * comment.
     */
    r350_gl_makecurrent(&g->plat);
    if (g->barrier && !r->add_blend && !r->out) {
        ok = gl_enqueue(g, r);
        r350_gl_done(&g->plat);
        return ok;
    }
    gl_flush_queue(g);

    p = gl_prog_for(g, r, r->add_blend);
    if (!p) {
        /* the program would not compile: fall back */
        r350_gl_done(&g->plat);
        return false;
    }
    glUseProgram(p->prog);
    if (p->u_usk >= 0 && r->us_konst) {
        glUniform4fv(p->u_usk, R350_GL_USK, r->us_konst);
    }
    gl_attach(g, g->cbuf);
    /*
     * The whole target, not the draw's rectangle. Device coordinates are
     * therefore target coordinates throughout: u_rect maps them to NDC
     * without an offset and u_org is zero, so gl_FragCoord.xy is the
     * device pixel plus a half. M2 rendered into a rectangle-sized FBO
     * and carried x0/y0 in both places, where any error in the pair
     * cancelled itself; here the offsets are gone rather than paired.
     */
    glViewport(0, 0, r->surf_w, r->surf_h);

    /*
     * The blend samples the destination through an integer sampler
     * rather than through GL's blender, so that the device's truncating
     * pack is reproduced exactly. The bytes come from the colour buffer
     * itself, copied on the GPU over the draw's own rectangle -- M2
     * uploaded them from the host for every draw, which the bench
     * measures at 1.44 ms full screen against 0.074 ms for the copy.
     */
    if (r->blend && r->blend_read && !r->add_blend) {
        glActiveTexture(GL_TEXTURE1);
        if (g->barrier) {
            /*
             * The colour buffer is sampled where it is written. Within
             * one pass no pixel is covered twice, which is the case the
             * extension defines; what earlier draws and passes left is
             * made visible by a barrier, taken only when the rectangle
             * this draw reads was written since the last one.
             */
            glBindTexture(GL_TEXTURE_2D, g->cbuf);
            gl_before_read(g, r->x0, r->y0, r->x0 + r->w, r->y0 + r->h);
        } else {
            glBindTexture(GL_TEXTURE_2D, g->dst);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r->x0, r->y0,
                                r->x0, r->y0, r->w, r->h);
        }
    }

    glActiveTexture(GL_TEXTURE0);
    if (r->tcx) {
        R350GlGen ge;

        if ((size_t)r->nvert / 3 * r->tc_stride > g->tc_max ||
            !gl_bind_units(g, r, false)) {
            r350_gl_done(&g->plat);
            return false;
        }
        gl_gen_from(&ge, r);
        gl_bind_txunits(g, ge.textured, ge.slot);
        gl_gen_uniforms(p, ge.textured, ge.size, ge.clamp, ge.tf, ge.tcinv,
                        0, ge.tcstride, ge.tcraw, ge.lodany);
        glBindBuffer(GL_TEXTURE_BUFFER, g->tcbo);
        glBufferData(GL_TEXTURE_BUFFER,
                     (GLsizeiptr)(sizeof(float) * 4 * r->tc_stride *
                                  (r->nvert / 3)),
                     r->tcx, GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + R350_GL_TCUNIT);
        glBindTexture(GL_TEXTURE_BUFFER, g->tctex);
        glActiveTexture(GL_TEXTURE0);
    } else if ((r->textured & 1) && r->tex_slot[0] <= R350_GL_TEXSLOTS) {
        unsigned sl = r->tex_slot[0];

        glBindTexture(GL_TEXTURE_2D, g->tex[sl]);
        if (r->tex_fresh[0] && r->tex[0]) {
            gl_upload_tex(g, sl, r);
        } else if (g->tex_w[sl] != r->tex_w[0] ||
                   g->tex_h[sl] != r->tex_h[0] ||
                   g->tex_nl[sl] != gl_levels(r)) {
            /*
             * The caller said the slot was current and it is not. That
             * can only be a bookkeeping error, and rendering from the
             * wrong texture is the silent kind of wrong, so refuse.
             */
            r350_gl_done(&g->plat);
            return false;
        }
    } else {
        /* specified once at open: re-specifying it per draw is a stall */
        glBindTexture(GL_TEXTURE_2D, g->white);
    }

    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)sizeof(float) * R350_GL_VSTRIDE * r->nvert,
                 r->verts, GL_STREAM_DRAW);

    glUniform4f(p->u_rect, 0.0f, 0.0f, (float)r->surf_w, (float)r->surf_h);
    glUniform2f(p->u_org, 0.0f, 0.0f);
    glUniform2i(p->u_texsize, r->tex_w[0], r->tex_h[0]);
    glUniform2i(p->u_clamp, r->clamp_s[0], r->clamp_t[0]);
    {
        GLint tf[R350_GL_TF];

        gl_tf(r, tf);
        glUniform1iv(p->u_tf, R350_GL_TF, tf);
    }
    glUniform1i(p->u_textured, r->textured & 1);
    glUniform1i(p->u_alphatest, r->alpha_test);
    glUniform1i(p->u_affunc, r->af_func);
    glUniform1f(p->u_afref, r->af_ref);
    glUniform1i(p->u_discard, r->discard);
    glUniform1i(p->u_blend, r->blend);
    glUniform1i(p->u_blendread, r->blend_read);
    glUniform3i(p->u_cfac, r->src_factor, r->dst_factor, r->comb_fcn);
    glUniform3i(p->u_afac, r->a_src_factor, r->a_dst_factor, r->a_comb_fcn);
    glUniform4f(p->u_konst, r->k_r, r->k_g, r->k_b, r->k_a);
    glUniform1f(p->u_zscale, gl_zscale(&r->z));
    glUniform1i(p->u_zonly, r->zonly);

    /*
     * Scissor, the one cliprect it absorbed, AND the draw's rectangle.
     * The rectangle used to bound the draw by being the whole
     * framebuffer; now it has to be said out loud, because it is also
     * the only region the caller seeded and the only one it will fetch.
     */
    sx0 = MAX(r->sx0, r->x0);
    sy0 = MAX(r->sy0, r->y0);
    sx1 = MIN(r->sx1, r->x0 + r->w);
    sy1 = MIN(r->sy1, r->y0 + r->h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx0, sy0, MAX(sx1 - sx0, 0), MAX(sy1 - sy0, 0));

    glColorMask(!!(r->wmask & 0x00ff0000), !!(r->wmask & 0x0000ff00),
                !!(r->wmask & 0x000000ff), !!(r->wmask & 0xff000000));

    /*
     * One pass in the ordinary case. A draw whose own primitives overlap
     * while blending gets several, and the destination the blend samples
     * is refreshed from the colour buffer between them -- entirely on the
     * GPU, which is the whole point: the software rasterizer's ordering
     * is reproduced without the draw going back to it.
     */
    if (r->add_blend) {
        /*
         * dst' = dst + f(src), in ONE pass, with GL's own blender doing
         * the adding and keeping primitive order while it does. The
         * blender cannot touch an integer attachment at all, so the
         * draw runs in the normalized twin: copy the colour buffer in,
         * blend, copy the result back. Both copies are exact (see the
         * conversion shaders) and both are GPU-side.
         *
         * The quantisation is what makes this a REPRODUCTION of the
         * device rather than an approximation of it. The device packs
         * every primitive with a truncation, so a pixel a ribbon
         * crosses twice is truncated twice; the shader hands GL
         * floor(255*f(src)) and GL adds it to a byte, which is the same
         * chain, one integer step per primitive.
         */
        gl_attach(g, g->acc);
        gl_zstate(g, NULL);
        glUseProgram(g->ui2n);
        glBindVertexArray(g->vao_blit);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g->cbuf);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glUseProgram(p->prog);
        glBindVertexArray(g->vao);
        glColorMask(!!(r->wmask & 0x00ff0000), !!(r->wmask & 0x0000ff00),
                    !!(r->wmask & 0x000000ff), !!(r->wmask & 0xff000000));
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        gl_zstate(g, &r->z);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nvert);
        gl_zstate(g, NULL);
        glDisable(GL_BLEND);

        gl_attach(g, g->cbuf);
        glUseProgram(g->n2ui);
        glBindVertexArray(g->vao_blit);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g->acc);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glUseProgram(p->prog);
        glBindVertexArray(g->vao);
        glActiveTexture(GL_TEXTURE0);
        /* the copy back covers the whole viewport */
        gl_wrote(g, 0, 0, r->surf_w, r->surf_h);
    } else if (r->npass > 1) {
        unsigned k;

        gl_zstate(g, &r->z);
        for (k = 0; k < r->npass; k++) {
            if (k && g->barrier) {
                gl_barrier(g);
            } else if (k) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g->dst);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r->x0, r->y0,
                                    r->x0, r->y0, r->w, r->h);
                glActiveTexture(GL_TEXTURE0);
            }
            glDrawArrays(GL_TRIANGLES, (GLint)r->pass[k],
                         (GLsizei)(r->pass[k + 1] - r->pass[k]));
        }
    } else {
        gl_zstate(g, &r->z);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nvert);
    }
    gl_zstate(g, NULL);
    if (r->out) {
        /* gl=verify only; the resident target keeps the pixels otherwise */
        glReadPixels(r->x0, r->y0, r->w, r->h, GL_RGBA_INTEGER,
                     GL_UNSIGNED_BYTE, r->out);
    }
    if (r->zout && r->z.mode) {
        gl_zread(g, r->x0, r->y0, r->w, r->h, r->zout, r->z.mode);
    }

    gl_wrote(g, r->x0, r->y0, r->x0 + r->w, r->y0 + r->h);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    ok = glGetError() == GL_NO_ERROR;
    r350_gl_done(&g->plat);
    return ok;
}

#else /* no host GL backend */

R350GlCtx *ati_r350_gl_open(const char **err)
{
    *err = "no host GL backend is built for this platform";
    return NULL;
}

void ati_r350_gl_close(R350GlCtx *g)
{
}

bool ati_r350_gl_target(R350GlCtx *g, int w, int h, bool *lost)
{
    *lost = false;
    return false;
}

bool ati_r350_gl_seed(R350GlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr)
{
    return false;
}

bool ati_r350_gl_fetch(R350GlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr)
{
    return false;
}

bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *req)
{
    return false;
}

bool ati_r350_gl_depth(R350GlCtx *g)
{
    return false;
}

bool ati_r350_gl_zseed(R350GlCtx *g, int x0, int y0, int w, int h,
                       const uint32_t *z, int mode)
{
    return false;
}

bool ati_r350_gl_zfetch(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t *z, int mode)
{
    return false;
}

bool ati_r350_gl_zclear(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t z, int mode)
{
    return false;
}

const char *ati_r350_gl_describe(R350GlCtx *g)
{
    return "none";
}

void ati_r350_gl_prog_stats(R350GlCtx *g, uint64_t *hits, uint64_t *links,
                            uint64_t *failed)
{
    *hits = *links = *failed = 0;
}

uint64_t ati_r350_gl_barriers(R350GlCtx *g)
{
    return 0;
}

void ati_r350_gl_queue_stats(R350GlCtx *g, uint64_t *units, uint64_t *flushes,
                             uint64_t *waves)
{
    *units = *flushes = *waves = 0;
}

void ati_r350_gl_hold(bool hold)
{
}

void ati_r350_gl_unbind(R350GlCtx *g)
{
}

#endif /* CONFIG_DARWIN || _WIN32 */
