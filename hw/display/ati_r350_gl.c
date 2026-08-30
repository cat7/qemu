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
 *
 * r350_gl_done() is the only member of that list that is not obvious,
 * and it exists because WGL needs it: see the threading comment in the
 * win32 leg. On darwin it is nothing at all.
 */

#ifdef CONFIG_DARWIN

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

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
#define GL_TRUE                         1
#define GL_NO_ERROR                     0
#define GL_ONE                          1
#define GL_TRIANGLES                    0x0004
#define GL_CULL_FACE                    0x0B44
#define GL_DEPTH_TEST                   0x0B71
#define GL_BLEND                        0x0BE2
#define GL_SCISSOR_TEST                 0x0C11
#define GL_UNPACK_ALIGNMENT             0x0CF5
#define GL_PACK_ALIGNMENT               0x0D05
#define GL_TEXTURE_2D                   0x0DE1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VERSION                      0x1F02
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
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

/* GL 1.1: opengl32.dll's own exports */
static void (APIENTRY *glBindTexture)(GLenum, GLuint);
static void (APIENTRY *glBlendFunc)(GLenum, GLenum);
static void (APIENTRY *glColorMask)(GLboolean, GLboolean, GLboolean,
                                    GLboolean);
static void (APIENTRY *glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                            GLint, GLint, GLsizei, GLsizei);
static void (APIENTRY *glDeleteTextures)(GLsizei, const GLuint *);
static void (APIENTRY *glDisable)(GLenum);
static void (APIENTRY *glDrawArrays)(GLenum, GLint, GLsizei);
static void (APIENTRY *glEnable)(GLenum);
static void (APIENTRY *glGenTextures)(GLsizei, GLuint *);
static GLenum (APIENTRY *glGetError)(void);
static const GLubyte *(APIENTRY *glGetString)(GLenum);
static void (APIENTRY *glPixelStorei)(GLenum, GLint);
static void (APIENTRY *glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                     GLenum, void *);
static void (APIENTRY *glScissor)(GLint, GLint, GLsizei, GLsizei);
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
static GLint (APIENTRY *glGetUniformLocation)(GLuint, const GLchar *);
static void (APIENTRY *glLinkProgram)(GLuint);
static void (APIENTRY *glShaderSource)(GLuint, GLsizei,
                                       const GLchar *const *, const GLint *);
static void (APIENTRY *glUniform1f)(GLint, GLfloat);
static void (APIENTRY *glUniform1fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *glUniform1i)(GLint, GLint);
static void (APIENTRY *glUniform2f)(GLint, GLfloat, GLfloat);
static void (APIENTRY *glUniform2i)(GLint, GLint, GLint);
static void (APIENTRY *glUniform3i)(GLint, GLint, GLint, GLint);
static void (APIENTRY *glUniform4f)(GLint, GLfloat, GLfloat, GLfloat,
                                    GLfloat);
static void (APIENTRY *glUniform4fv)(GLint, GLsizei, const GLfloat *);
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
    R350_GL_PROC(glDisable),
    R350_GL_PROC(glDrawArrays),
    R350_GL_PROC(glEnable),
    R350_GL_PROC(glGenTextures),
    R350_GL_PROC(glGetError),
    R350_GL_PROC(glGetString),
    R350_GL_PROC(glPixelStorei),
    R350_GL_PROC(glReadPixels),
    R350_GL_PROC(glScissor),
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
    R350_GL_PROC(glGetUniformLocation),
    R350_GL_PROC(glLinkProgram),
    R350_GL_PROC(glShaderSource),
    R350_GL_PROC(glUniform1f),
    R350_GL_PROC(glUniform1fv),
    R350_GL_PROC(glUniform1i),
    R350_GL_PROC(glUniform2f),
    R350_GL_PROC(glUniform2i),
    R350_GL_PROC(glUniform3i),
    R350_GL_PROC(glUniform4f),
    R350_GL_PROC(glUniform4fv),
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

static void r350_gl_plat_close(R350GlPlat *pl)
{
    if (pl->ctx) {
        wglMakeCurrent(NULL, NULL);
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
 * thread (device realize); every draw runs on whichever vCPU thread
 * reached the command processor; and ati_r350_gl_fetch() runs on the
 * main thread again whenever ati_r350_update_display() releases the
 * target for scanout (ati_r350.c). The BQL keeps two of them from being
 * inside the backend at once, which is all CGL asks for -- but WGL asks
 * for more: wglMakeCurrent FAILS while the context is current to a
 * DIFFERENT thread, and no thread can release another thread's context.
 * So here the context is taken for the duration of one entry point and
 * given straight back, and r350_gl_done() is that give-back.
 */
static inline void r350_gl_makecurrent(R350GlPlat *pl)
{
    wglMakeCurrent(pl->dc, pl->ctx);
}

static inline void r350_gl_done(R350GlPlat *pl)
{
    (void)pl;
    wglMakeCurrent(NULL, NULL);
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
typedef struct R350GlProg {
    GLuint prog;
    GLint u_rect, u_org, u_texsize, u_clamp, u_textured;
    GLint u_alphatest, u_affunc, u_afref, u_discard;
    GLint u_blend, u_blendread, u_cfac, u_afac, u_konst;
    GLint u_usk;
} R350GlProg;

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
    /* uploaded textures by caller slot, plus the scratch at the end */
    GLuint tex[R350_GL_TEXSLOTS + 1];
    int tex_w[R350_GL_TEXSLOTS + 1], tex_h[R350_GL_TEXSLOTS + 1];
    GLuint white;                   /* the 1x1 an untextured draw samples */
    /* the resident target's size; a request smaller than it reuses it */
    int fb_w, fb_h;
    /* staging for the two swapper orders GL cannot produce directly */
    uint8_t *stage;
    size_t stage_sz;
    char desc[128];
};

static const char *vs_src =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
"layout(location = 0) in vec2 a_pos;\n"
"layout(location = 1) in vec4 a_col;\n"
"layout(location = 2) in vec2 a_st;\n"
"layout(location = 3) in vec2 a_p0;\n"
"layout(location = 4) in vec2 a_p1;\n"
"layout(location = 5) in vec2 a_p2;\n"
"layout(location = 6) in vec4 a_c0;\n"
"layout(location = 7) in vec4 a_c1;\n"
"layout(location = 8) in vec4 a_c2;\n"
"layout(location = 9) in vec2 a_t0;\n"
"layout(location = 10) in vec2 a_t1;\n"
"layout(location = 11) in vec2 a_t2;\n"
"layout(location = 12) in float a_inv;\n"
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
"flat out float f_inv;\n"
"flat out vec4 f_s0;\n"
"flat out vec4 f_s1;\n"
"flat out vec4 f_s2;\n"
"void main()\n"
"{\n"
"    float nx = (a_pos.x - u_rect.x) / u_rect.z * 2.0 - 1.0;\n"
"    float ny = (a_pos.y - u_rect.y) / u_rect.w * 2.0 - 1.0;\n"
/*
 * w = 1 keeps GL's own interpolation affine, which is what the software
 * rasterizer's screen-space barycentric weights already are.
 */
"    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
"    f_p0 = a_p0; f_p1 = a_p1; f_p2 = a_p2;\n"
"    f_c0 = a_c0; f_c1 = a_c1; f_c2 = a_c2;\n"
"    f_t0 = a_t0; f_t1 = a_t1; f_t2 = a_t2;\n"
"    f_inv = a_inv;\n"
"    f_s0 = a_s0; f_s1 = a_s1; f_s2 = a_s2;\n"
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
"out uvec4 o_col;\n";

static const char *fs_head_add =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
"#define R350_ADD 1\n"
"uniform vec4 USK[32];\n"
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
"flat in float f_inv;\n"
"flat in vec4 f_s0;\n"
"flat in vec4 f_s1;\n"
"flat in vec4 f_s2;\n"
"uniform usampler2D u_tex;\n"
"uniform usampler2D u_dst;\n"
"uniform float u_n255[256];\n"
"uniform vec2 u_org;\n"
"uniform ivec2 u_texsize;\n"
"uniform ivec2 u_clamp;\n"
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
"void main()\n"
"{\n"
"    precise vec4 c;\n"
"    precise float ts, tt;\n"
/*
 * r300_raster_tri()'s own weights, expression for expression, fusion
 * included -- see the file comment.
 */
"    precise float inv = f_inv;\n"
"    precise float px = gl_FragCoord.x + u_org.x;\n"
"    precise float py = gl_FragCoord.y + u_org.y;\n"
"    precise float q0 = (f_p2.y - f_p1.y) * (px - f_p1.x);\n"
"    precise float q1 = (f_p0.y - f_p2.y) * (px - f_p2.x);\n"
"    precise float d0 = fma(f_p2.x - f_p1.x, py - f_p1.y, -q0);\n"
"    precise float d1 = fma(f_p0.x - f_p2.x, py - f_p2.y, -q1);\n"
"    precise float w0 = d0 * inv;\n"
"    precise float w1 = d1 * inv;\n"
"    precise float w2 = 1.0 - w0 - w1;\n"
"    c = fma(vec4(w2), f_c2, fma(vec4(w1), f_c1, w0 * f_c0));\n"
/*
 * Coordinate set 0, the only one a request carries -- a translated
 * program reads no other. See the coordinate-set note in ati_r350_gl.h.
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
"    vec4 texel = vec4(1.0);\n"
"    if (u_textured != 0) {\n"
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
    glGenTextures(1, &g->cbuf);
    glGenTextures(R350_GL_TEXSLOTS + 1, g->tex);
    glGenTextures(1, &g->white);
    glGenTextures(1, &g->dst);
    glGenTextures(1, &g->acc);
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
            { 0, 2, 0 }, { 1, 4, 2 }, { 2, 2 * C, 6 },
            { 3, 2, 6 + 2 * C }, { 4, 2, 8 + 2 * C }, { 5, 2, 10 + 2 * C },
            { 6, 4, 12 + 2 * C }, { 7, 4, 16 + 2 * C }, { 8, 4, 20 + 2 * C },
            { 9, 2 * C, 24 + 2 * C }, { 10, 2 * C, 24 + 4 * C },
            { 11, 2 * C, 24 + 6 * C },
            { 12, 1, 24 + 8 * C },
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

    snprintf(g->desc, sizeof(g->desc), R350_GL_BACKEND_NAME ", %s / GLSL %s",
             (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
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
        glDeleteTextures(1, &g->dst);
        glDeleteTextures(1, &g->acc);
        glDeleteTextures(R350_GL_TEXSLOTS + 1, g->tex);
        glDeleteTextures(1, &g->white);
        glDeleteTextures(1, &g->cbuf);
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteBuffers(1, &g->vbo);
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
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->cbuf, 0);
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
    r350_gl_makecurrent(&g->plat);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->cbuf);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_RGBA_INTEGER,
                    GL_UNSIGNED_BYTE, st);
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
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->cbuf, 0);
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

bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *r)
{
    const R350GlProg *p;
    int sx0, sy0, sx1, sy1;
    bool ok;

    if (!g || r->w <= 0 || r->h <= 0 || !r->nvert ||
        r->surf_w > g->fb_w || r->surf_h > g->fb_h) {
        return false;
    }
    /*
     * Made current per draw rather than once: the device may reach this
     * from whichever thread runs the vCPU, and the big QEMU lock is what
     * keeps two of them from being here at the same time. On darwin the
     * call is a no-op when the context is already current and the paired
     * r350_gl_done() is nothing; on Windows both are real, and why is in
     * the win32 leg's threading comment.
     */
    r350_gl_makecurrent(&g->plat);

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
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->cbuf, 0);
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
        glBindTexture(GL_TEXTURE_2D, g->dst);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r->x0, r->y0,
                            r->x0, r->y0, r->w, r->h);
    }

    glActiveTexture(GL_TEXTURE0);
    if ((r->textured & 1) && r->tex_slot[0] <= R350_GL_TEXSLOTS) {
        unsigned sl = r->tex_slot[0];

        glBindTexture(GL_TEXTURE_2D, g->tex[sl]);
        if (r->tex_fresh[0] && r->tex[0]) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            if (g->tex_w[sl] == r->tex_w[0] && g->tex_h[sl] == r->tex_h[0]) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r->tex_w[0],
                                r->tex_h[0], GL_RGBA_INTEGER,
                                GL_UNSIGNED_BYTE, r->tex[0]);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, r->tex_w[0],
                             r->tex_h[0], 0, GL_RGBA_INTEGER,
                             GL_UNSIGNED_BYTE, r->tex[0]);
                g->tex_w[sl] = r->tex_w[0];
                g->tex_h[sl] = r->tex_h[0];
            }
        } else if (g->tex_w[sl] != r->tex_w[0] ||
                   g->tex_h[sl] != r->tex_h[0]) {
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
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g->acc, 0);
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
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nvert);
        glDisable(GL_BLEND);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, g->cbuf, 0);
        glUseProgram(g->n2ui);
        glBindVertexArray(g->vao_blit);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g->acc);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glUseProgram(p->prog);
        glBindVertexArray(g->vao);
        glActiveTexture(GL_TEXTURE0);
    } else if (r->npass > 1) {
        unsigned k;

        for (k = 0; k < r->npass; k++) {
            if (k) {
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
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nvert);
    }
    if (r->out) {
        /* gl=verify only; the resident target keeps the pixels otherwise */
        glReadPixels(r->x0, r->y0, r->w, r->h, GL_RGBA_INTEGER,
                     GL_UNSIGNED_BYTE, r->out);
    }

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

const char *ati_r350_gl_describe(R350GlCtx *g)
{
    return "none";
}

void ati_r350_gl_prog_stats(R350GlCtx *g, uint64_t *hits, uint64_t *links,
                            uint64_t *failed)
{
    *hits = *links = *failed = 0;
}

#endif /* CONFIG_DARWIN || _WIN32 */
