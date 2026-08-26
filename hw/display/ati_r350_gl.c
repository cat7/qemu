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

#ifdef CONFIG_DARWIN

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

struct R350GlCtx {
    CGLContextObj ctx;
    GLuint prog, vao, vbo, fbo, cbuf, dst;
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
    /* uniform locations, resolved once at link time */
    GLint u_rect, u_org, u_texsize, u_clamp, u_textured;
    GLint u_alphatest, u_affunc, u_afref, u_discard;
    GLint u_blend, u_blendread, u_cfac, u_afac, u_konst;
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
"}\n";

static const char *fs_src =
"#version 330 core\n"
"#extension GL_ARB_gpu_shader5 : require\n"
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
"out uvec4 o_col;\n"
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
"    precise vec2 st = fma(vec2(w2), f_t2,\n"
"                          fma(vec2(w1), f_t1, w0 * f_t0));\n"
"    ts = st.x; tt = st.y;\n"
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
"        c.a *= u_n255[int(tu.a)];\n"
"        c.r *= u_n255[int(tu.r)];\n"
"        c.g *= u_n255[int(tu.g)];\n"
"        c.b *= u_n255[int(tu.b)];\n"
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
"}\n";

static GLuint gl_compile(GLenum type, const char *src, const char **err)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(sh);
        *err = "GLSL 3.30 core shader would not compile";
        return 0;
    }
    return sh;
}

R350GlCtx *ati_r350_gl_open(const char **err)
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
    R350GlCtx *g;
    GLuint vs, fs;
    GLint npix = 0, ok = 0;
    float n255[256];
    unsigned k;

    *err = NULL;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) || !pix) {
        *err = "no accelerated offscreen GL 3.3 core pixel format";
        return NULL;
    }
    g = g_new0(R350GlCtx, 1);
    if (CGLCreateContext(pix, NULL, &g->ctx)) {
        CGLDestroyPixelFormat(pix);
        g_free(g);
        *err = "CGLCreateContext failed";
        return NULL;
    }
    CGLDestroyPixelFormat(pix);
    CGLSetCurrentContext(g->ctx);

    vs = gl_compile(GL_VERTEX_SHADER, vs_src, err);
    fs = vs ? gl_compile(GL_FRAGMENT_SHADER, fs_src, err) : 0;
    if (!vs || !fs) {
        ati_r350_gl_close(g);
        return NULL;
    }
    g->prog = glCreateProgram();
    glAttachShader(g->prog, vs);
    glAttachShader(g->prog, fs);
    glLinkProgram(g->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        ati_r350_gl_close(g);
        *err = "GLSL program would not link";
        return NULL;
    }
    glUseProgram(g->prog);

    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenBuffers(1, &g->vbo);
    glGenFramebuffers(1, &g->fbo);
    glGenTextures(1, &g->cbuf);
    glGenTextures(R350_GL_TEXSLOTS + 1, g->tex);
    glGenTextures(1, &g->white);
    glGenTextures(1, &g->dst);
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

    glUniform1i(glGetUniformLocation(g->prog, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(g->prog, "u_dst"), 1);
    for (k = 0; k < 256; k++) {
        n255[k] = k / 255.0f;
    }
    glUniform1fv(glGetUniformLocation(g->prog, "u_n255"), 256, n255);

    g->u_rect = glGetUniformLocation(g->prog, "u_rect");
    g->u_org = glGetUniformLocation(g->prog, "u_org");
    g->u_texsize = glGetUniformLocation(g->prog, "u_texsize");
    g->u_clamp = glGetUniformLocation(g->prog, "u_clamp");
    g->u_textured = glGetUniformLocation(g->prog, "u_textured");
    g->u_alphatest = glGetUniformLocation(g->prog, "u_alphatest");
    g->u_affunc = glGetUniformLocation(g->prog, "u_affunc");
    g->u_afref = glGetUniformLocation(g->prog, "u_afref");
    g->u_discard = glGetUniformLocation(g->prog, "u_discard");
    g->u_blend = glGetUniformLocation(g->prog, "u_blend");
    g->u_blendread = glGetUniformLocation(g->prog, "u_blendread");
    g->u_cfac = glGetUniformLocation(g->prog, "u_cfac");
    g->u_afac = glGetUniformLocation(g->prog, "u_afac");
    g->u_konst = glGetUniformLocation(g->prog, "u_konst");

    {
        static const struct { GLint loc, n, off; } at[] = {
            { 0, 2, 0 }, { 1, 4, 2 }, { 2, 2, 6 },
            { 3, 2, 8 }, { 4, 2, 10 }, { 5, 2, 12 },
            { 6, 4, 14 }, { 7, 4, 18 }, { 8, 4, 22 },
            { 9, 2, 26 }, { 10, 2, 28 }, { 11, 2, 30 },
            { 12, 1, 32 },
        };

        glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
        for (k = 0; k < ARRAY_SIZE(at); k++) {
            glEnableVertexAttribArray(at[k].loc);
            glVertexAttribPointer(at[k].loc, at[k].n, GL_FLOAT, GL_FALSE,
                                  R350_GL_VSTRIDE * 4,
                                  (void *)(size_t)(at[k].off * 4));
        }
    }

    snprintf(g->desc, sizeof(g->desc), "CGL offscreen, %s / GLSL %s",
             (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    if (glGetError() != GL_NO_ERROR) {
        ati_r350_gl_close(g);
        *err = "GL reported an error while setting the backend up";
        return NULL;
    }
    return g;
}

void ati_r350_gl_close(R350GlCtx *g)
{
    if (!g) {
        return;
    }
    if (g->ctx) {
        CGLSetCurrentContext(g->ctx);
        glDeleteTextures(1, &g->dst);
        glDeleteTextures(R350_GL_TEXSLOTS + 1, g->tex);
        glDeleteTextures(1, &g->white);
        glDeleteTextures(1, &g->cbuf);
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteBuffers(1, &g->vbo);
        glDeleteVertexArrays(1, &g->vao);
        glDeleteProgram(g->prog);
        CGLSetCurrentContext(NULL);
        CGLDestroyContext(g->ctx);
    }
    g_free(g->stage);
    g_free(g);
}

const char *ati_r350_gl_describe(R350GlCtx *g)
{
    return g ? g->desc : "none";
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
    CGLSetCurrentContext(g->ctx);
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
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->cbuf, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glGetError() != GL_NO_ERROR) {
        g->fb_w = g->fb_h = 0;
        return false;
    }
    g->fb_w = w;
    g->fb_h = h;
    *lost = true;
    return true;
}

bool ati_r350_gl_seed(R350GlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr)
{
    uint8_t *st;
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
    CGLSetCurrentContext(g->ctx);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->cbuf);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, w, h, GL_RGBA_INTEGER,
                    GL_UNSIGNED_BYTE, st);
    return glGetError() == GL_NO_ERROR;
}

bool ati_r350_gl_fetch(R350GlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr)
{
    uint8_t *st;
    int x, y;

    if (!g || w <= 0 || h <= 0 ||
        x0 < 0 || y0 < 0 || x0 + w > g->fb_w || y0 + h > g->fb_h) {
        return false;
    }
    st = gl_stage(g, (size_t)w * h * 4);
    CGLSetCurrentContext(g->ctx);
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
    return glGetError() == GL_NO_ERROR;
}

bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *r)
{
    int sx0, sy0, sx1, sy1;

    if (!g || r->w <= 0 || r->h <= 0 || !r->nvert ||
        r->surf_w > g->fb_w || r->surf_h > g->fb_h) {
        return false;
    }
    /*
     * Made current per draw rather than once: the device may reach this
     * from whichever thread runs the vCPU, and the big QEMU lock is what
     * keeps two of them from being here at the same time. The call is a
     * no-op when the context is already current.
     */
    CGLSetCurrentContext(g->ctx);

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
    if (r->blend && r->blend_read) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g->dst);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r->x0, r->y0,
                            r->x0, r->y0, r->w, r->h);
    }

    glActiveTexture(GL_TEXTURE0);
    if (r->textured && r->tex_slot <= R350_GL_TEXSLOTS) {
        unsigned sl = r->tex_slot;

        glBindTexture(GL_TEXTURE_2D, g->tex[sl]);
        if (r->tex_fresh && r->tex) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            if (g->tex_w[sl] == r->tex_w && g->tex_h[sl] == r->tex_h) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r->tex_w, r->tex_h,
                                GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, r->tex);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, r->tex_w,
                             r->tex_h, 0, GL_RGBA_INTEGER,
                             GL_UNSIGNED_BYTE, r->tex);
                g->tex_w[sl] = r->tex_w;
                g->tex_h[sl] = r->tex_h;
            }
        } else if (g->tex_w[sl] != r->tex_w || g->tex_h[sl] != r->tex_h) {
            /*
             * The caller said the slot was current and it is not. That
             * can only be a bookkeeping error, and rendering from the
             * wrong texture is the silent kind of wrong, so refuse.
             */
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

    glUniform4f(g->u_rect, 0.0f, 0.0f, (float)r->surf_w, (float)r->surf_h);
    glUniform2f(g->u_org, 0.0f, 0.0f);
    glUniform2i(g->u_texsize, r->tex_w, r->tex_h);
    glUniform2i(g->u_clamp, r->clamp_s, r->clamp_t);
    glUniform1i(g->u_textured, r->textured);
    glUniform1i(g->u_alphatest, r->alpha_test);
    glUniform1i(g->u_affunc, r->af_func);
    glUniform1f(g->u_afref, r->af_ref);
    glUniform1i(g->u_discard, r->discard);
    glUniform1i(g->u_blend, r->blend);
    glUniform1i(g->u_blendread, r->blend_read);
    glUniform3i(g->u_cfac, r->src_factor, r->dst_factor, r->comb_fcn);
    glUniform3i(g->u_afac, r->a_src_factor, r->a_dst_factor, r->a_comb_fcn);
    glUniform4f(g->u_konst, r->k_r, r->k_g, r->k_b, r->k_a);

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
    if (r->npass > 1) {
        unsigned p;

        for (p = 0; p < r->npass; p++) {
            if (p) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g->dst);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r->x0, r->y0,
                                    r->x0, r->y0, r->w, r->h);
                glActiveTexture(GL_TEXTURE0);
            }
            glDrawArrays(GL_TRIANGLES, (GLint)r->pass[p],
                         (GLsizei)(r->pass[p + 1] - r->pass[p]));
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
    return glGetError() == GL_NO_ERROR;
}

#else /* !CONFIG_DARWIN */

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

#endif /* CONFIG_DARWIN */
