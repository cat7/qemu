/*
 * ATI R300/R350 -- the Metal rendering backend.
 *
 * The interface is ati_r350_gl.h, the same one ati_r350_gl.c implements,
 * and the arithmetic is that file's fragment shader transcribed to MSL:
 * the explicit fma() calls, the host-computed 1/area, the k/255 table,
 * the integer filter. Fast math and contraction are off, so a multiply
 * and an add are fused only where the source says fma().
 *
 * THE FRAGMENT PROGRAM is the GLSL text ati_r350_us_glsl.c emits. Its
 * vocabulary is small, so a few typedefs and a scoped clamp() make it
 * valid MSL and its `out` parameters become references. Program-scope
 * variables do not exist in MSL, so the whole fragment stage is a struct
 * whose members are what GLSL keeps at program scope, and the translated
 * text is spliced into it.
 *
 * THE TARGET is resident, as in the GL backend: an RGBA8Uint colour
 * texture and a Depth32Float_Stencil8 depth texture, Z stored as
 * z * 2^-24 so that the hardware compare is the device's integer compare.
 * Seeds and fetches are blits in command-stream order.
 *
 * BLENDING reads the colour attachment in the fragment stage. An Apple
 * GPU runs the fragments that land on one pixel in primitive order, each
 * seeing what the one before it left, so a self-overlapping blended draw
 * is one draw call and needs neither passes nor a destination copy. The
 * colour write mask is applied the same way, which leaves one pipeline
 * per program.
 *
 * TEXTURES are the caller's decoded RGBA8 levels in buffers, read with
 * the device's own addressing; a cube map's faces are stacked per level.
 * A slot still referenced by an uncompleted command buffer is replaced
 * by a new buffer rather than written.
 *
 * Based on lyons88/qemu-g5 934422f4f9 (the GLSL-to-MSL adaptation of the
 * translated program and the exact-math compile options).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "qemu/thread.h"

#define R350_GPU_IMPL_MTL 1
#include "ati_r350_gpu.h"

#import <Metal/Metal.h>

/* linked pipelines kept, one per distinct guest fragment program */
#define MTL_PROGSLOTS 64
/* bytes in one vertex/staging arena; larger requests get their own */
#define MTL_ARENA (4 * 1024 * 1024)
#define MTL_ARENAS 16
/* draws after which the open command buffer is committed anyway */
#define MTL_CB_DRAWS 1024
/* draws worth committing when the engine goes idle */
#define MTL_IDLE_DRAWS 64
/* occlusion counters, one more for a verify draw; offsets stay < 256 KiB */
#define MTL_ZQ_SLOTS 16384
/* compile failures reported before going quiet */
#define MTL_REPORTS 4
/* depth/stencil states kept */
#define MTL_DSS 32
/* mip levels a texture slot may carry */
#define MTL_LEVELS 16

enum { MTL_P_EMPTY, MTL_P_PENDING, MTL_P_READY, MTL_P_FAILED };

typedef struct MtlProg {
    uint64_t key;
    int state;
    uint64_t gen;                       /* invalidates a stale compile */
    id<MTLRenderPipelineState> pso;
} MtlProg;

typedef struct MtlArena {
    id<MTLBuffer> buf;
    uint64_t serial;                    /* newest command buffer using it */
} MtlArena;

/* the fragment stage's per-draw values; `FsU` in the shader, 4-byte fields */
typedef struct MtlFsU {
    float konst[4];
    float afref, zscale;
    int32_t alphatest, affunc, discard, blend, blendread, zonly, dkeep;
    int32_t textured, tcstride, tcraw, lodany;
    int32_t cfac[3], afac[3];
    uint32_t wmask;
    int32_t txsize[16], txclamp[16];
    int32_t txtf[R350_GL_TEXUNITS * 16];
    int32_t txoff[R350_GL_TEXUNITS * MTL_LEVELS];
    int32_t txlen[R350_GL_TEXUNITS];
    float tcinv[R350_GL_TCSETS * 2];
} MtlFsU;

QEMU_BUILD_BUG_ON(sizeof(MtlFsU) != 4 * (24 + 32 + 128 + 128 + 8 + 16));

typedef struct MtlTex {
    id<MTLBuffer> buf;
    int w, h, nl, cube;
    int len;                            /* texels */
    int off[MTL_LEVELS];
    uint64_t use;                       /* newest command buffer reading it */
} MtlTex;

struct R350MtlCtx {
    id<MTLDevice> dev;
    id<MTLCommandQueue> q;
    id<MTLBuffer> n255, white, dummy, vis;
    id<MTLRenderPipelineState> zclear_pso;
    id<MTLDepthStencilState> zclear_dss;

    /* the resident target */
    id<MTLTexture> cbuf, zbuf;
    int fb_w, fb_h;

    MtlTex tex[R350_GL_TEXSLOTS + 1];

    QemuMutex plock;                    /* prog[] against compile handlers */
    MtlProg prog[MTL_PROGSLOTS];
    unsigned prog_next;
    uint64_t prog_gen;
    uint64_t prog_hits, prog_links, prog_failed, w_waits;
    int compiling;                      /* async compiles in flight */
    int reports;
    bool async;

    struct {
        R350GlZ z;
        id<MTLDepthStencilState> s;
    } dss[MTL_DSS];
    unsigned ndss, dss_next;

    /*
     * The open command buffer (serial `ser_open`, 0 when none) and its
     * open render pass. `ser_done` is the newest serial known complete;
     * one queue completes its buffers in order.
     */
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    id<MTLCommandBuffer> last;          /* newest committed */
    uint64_t ser_next, ser_open, ser_last, ser_done;
    unsigned cb_draws, cb_zq;
    int handlers;                       /* completion handlers not yet run */
    bool gpu_failed;

    /* what the open render pass has set */
    id<MTLRenderPipelineState> enc_pso;
    id<MTLDepthStencilState> enc_dss;
    int enc_sref;
    int enc_vw, enc_vh;
    id<MTLBuffer> enc_tx[R350_GL_TEXUNITS];

    MtlArena arena[MTL_ARENAS];
    unsigned narena, acur;
    size_t aused;

    /* occlusion counting; see ati_r350_gl_zq_mark() */
    uint64_t zq_issued, zq_done, zq_sum;
    uint64_t *zq_ser;

    uint64_t draws, commits, passes;
    char desc[160];
};

/* ------------------------------------------------------------------ */
/* the shaders */

static const char *mtl_fs_head =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"#if __METAL_VERSION__ >= 320\n"
"#pragma METAL fp math_mode(safe)\n"
"#pragma METAL fp contract(off)\n"
"#endif\n"
"typedef float2 vec2;\n"
"typedef float3 vec3;\n"
"typedef float4 vec4;\n"
"typedef int2 ivec2;\n"
"typedef int3 ivec3;\n"
"typedef int4 ivec4;\n"
"typedef uint4 uvec4;\n"
"#define precise\n"
"static inline float inversesqrt(float x) { return rsqrt(x); }\n"
"static inline float r3_clamp(float v, float lo, float hi)\n"
"{ return v < lo ? lo : (v > hi ? hi : v); }\n"
"static inline float3 r3_clamp(float3 v, float lo, float hi)\n"
"{ return select(select(v, float3(hi), v > hi), float3(lo), v < lo); }\n"
"static inline float4 r3_clamp(float4 v, float lo, float hi)\n"
"{ return select(select(v, float4(hi), v > hi), float4(lo), v < lo); }\n"
"\n"
"struct FsU {\n"
"    float konst[4];\n"
"    float afref, zscale;\n"
"    int alphatest, affunc, discard_, blend, blendread, zonly, dkeep;\n"
"    int textured, tcstride, tcraw, lodany;\n"
"    int cfac[3], afac[3];\n"
"    uint wmask;\n"
"    int txsize[16], txclamp[16];\n"
"    int txtf[128];\n"
"    int txoff[128];\n"
"    int txlen[8];\n"
"    float tcinv[16];\n"
"};\n"
"\n"
"struct VOut {\n"
"    float4 pos [[position]];\n"
"    float2 p0 [[flat]];\n"
"    float2 p1 [[flat]];\n"
"    float2 p2 [[flat]];\n"
"    float4 c0 [[flat]];\n"
"    float4 c1 [[flat]];\n"
"    float4 c2 [[flat]];\n"
"    float2 t0 [[flat]];\n"
"    float2 t1 [[flat]];\n"
"    float2 t2 [[flat]];\n"
"    float4 inv [[flat]];\n"
"    float4 s0 [[flat]];\n"
"    float4 s1 [[flat]];\n"
"    float4 s2 [[flat]];\n"
"    float4 z [[flat]];\n"
"    uint prim [[flat]];\n"
"};\n"
"\n"
"struct FOut {\n"
"    uint4 col [[color(0)]];\n"
"    float depth [[depth(any)]];\n"
"};\n"
"\n"
"struct FS {\n"
"    constant FsU *U;\n"
"    constant float4 *USK;\n"
"    constant float *N255;\n"
"    device const float4 *TCB;\n"
"    device const uchar4 *TX[8];\n"
"    float4 us_der[8];\n"
"    float3 us_cdx[8];\n"
"    float3 us_cdy[8];\n"
"\n"
"    float bf(int code, float sc, float sa, float dc, float da,\n"
"             float kc, float ka)\n"
"    {\n"
"        if (code == 1 || code == 32) return 0.0;\n"
"        if (code == 2 || code == 33) return 1.0;\n"
"        if (code == 3 || code == 34) return sc;\n"
"        if (code == 4 || code == 35) return 1.0 - sc;\n"
"        if (code == 9 || code == 36) return dc;\n"
"        if (code == 10 || code == 37) return 1.0 - dc;\n"
"        if (code == 5 || code == 38) return sa;\n"
"        if (code == 6 || code == 39) return 1.0 - sa;\n"
"        if (code == 7 || code == 40) return da;\n"
"        if (code == 8 || code == 41) return 1.0 - da;\n"
"        if (code == 11 || code == 42) return min(sa, 1.0 - da);\n"
"        if (code == 43) return kc;\n"
"        if (code == 44) return 1.0 - kc;\n"
"        if (code == 45) return ka;\n"
"        if (code == 46) return 1.0 - ka;\n"
"        return 1.0;\n"
"    }\n"
"\n"
"    float comb(int f, float s, float d)\n"
"    {\n"
"        if (f == 2 || f == 3) return s - d;\n"
"        if (f == 4) return min(s, d);\n"
"        if (f == 5) return max(s, d);\n"
"        if (f == 6 || f == 7) return d - s;\n"
"        return s + d;\n"
"    }\n"
"\n"
"    float4 n255(uint4 t)\n"
"    {\n"
"        return float4(N255[t.r], N255[t.g], N255[t.b], N255[t.a]);\n"
"    }\n"
"\n"
"    float tc_pre(float c, int n, int m)\n"
"    {\n"
"        float r = c;\n"
"        if (m == 3 || m == 5 || m == 7) r = abs(r);\n"
"        if (m == 4 || m == 5) r = min(max(r, 0.0), float(n));\n"
"        return min(max(r, -16777216.0), 16777216.0);\n"
"    }\n"
"\n"
"    int tc_mod(int i, int n)\n"
"    {\n"
"        return i >= 0 ? i % n : n - 1 - (-1 - i) % n;\n"
"    }\n"
"\n"
"    int tc_idx(int i, int n, int m, bool pt)\n"
"    {\n"
"        if (m == 0) return tc_mod(i, n);\n"
"        if (m == 1) {\n"
"            int r = tc_mod(i, 2 * n);\n"
"            return r >= n ? 2 * n - 1 - r : r;\n"
"        }\n"
"        if (m == 2 || m == 3 || ((m == 4 || m == 5) && pt))\n"
"            return clamp(i, 0, n - 1);\n"
"        return i < 0 || i >= n ? -1 : i;\n"
"    }\n"
"\n"
"    uint4 tlerp(uint4 a, uint4 b, int f)\n"
"    {\n"
"        return uint4((int4(a) * (256 - f) + int4(b) * f + 128) >> 8);\n"
"    }\n"
"\n"
"    int tlog2(float v)\n"
"    {\n"
"        if (!(v > 0.0)) return -65536;\n"
"        uint b = as_type<uint>(v);\n"
"        if (b >= 0x7f800000u) return 65536;\n"
"        return (int(b >> 23) - 127) * 256 + int((b >> 15) & 0xffu);\n"
"    }\n"
"\n"
"    void tc_der(float3 ga, float3 gb, float t0, float t1, float t2, float v,\n"
"                float iq, thread float &dx, thread float &dy)\n"
"    {\n"
"        float e0 = t0 - v;\n"
"        float e1 = t1 - v;\n"
"        float e2 = t2 - v;\n"
"        float m0 = ga.x * e0;\n"
"        float m1 = ga.y * e1;\n"
"        float m2 = ga.z * e2;\n"
"        float r = m0 + m1;\n"
"        r = r + m2;\n"
"        float rx = r * iq;\n"
"        m0 = gb.x * e0;\n"
"        m1 = gb.y * e1;\n"
"        m2 = gb.z * e2;\n"
"        r = m0 + m1;\n"
"        r = r + m2;\n"
"        float ry = r * iq;\n"
"        dx = rx;\n"
"        dy = ry;\n"
"    }\n"
"\n"
"    int cube_coord(float3 v, float3 dx, float3 dy, bool lod, int w, int h,\n"
"                   thread float2 &fd, thread float4 &der)\n"
"    {\n"
"        float3 a = abs(v);\n"
"        int face, im, is, it;\n"
"        float ss, st, sm;\n"
"        bool p;\n"
"        if (a.x >= a.y && a.x >= a.z) {\n"
"            p = v.x >= 0.0; face = p ? 0 : 1; im = 0; is = 2; it = 1;\n"
"            ss = p ? -1.0 : 1.0; st = -1.0;\n"
"        } else if (a.y >= a.z) {\n"
"            p = v.y >= 0.0; face = p ? 2 : 3; im = 1; is = 0; it = 2;\n"
"            ss = 1.0; st = p ? 1.0 : -1.0;\n"
"        } else {\n"
"            p = v.z >= 0.0; face = p ? 4 : 5; im = 2; is = 0; it = 1;\n"
"            ss = p ? 1.0 : -1.0; st = -1.0;\n"
"        }\n"
"        sm = p ? 1.0 : -1.0;\n"
"        float ma = sm * v[im];\n"
"        float sc = ss * v[is];\n"
"        float tc = st * v[it];\n"
"        float qs = 0.0, qt = 0.0;\n"
"        float fw = float(w), fh = float(h);\n"
"        if (ma > 0.0) { qs = sc / ma; qt = tc / ma; }\n"
"        float hs = qs * 0.5;\n"
"        hs = hs + 0.5;\n"
"        float ht = qt * 0.5;\n"
"        ht = ht + 0.5;\n"
"        float os = hs * fw;\n"
"        float ot = ht * fh;\n"
"        fd = float2(os, ot);\n"
"        der = float4(0.0);\n"
"        if (lod && ma > 0.0) {\n"
"            for (int k = 0; k < 2; k++) {\n"
"                float3 g = k == 0 ? dx : dy;\n"
"                float dm = sm * g[im];\n"
"                float ds = ss * g[is];\n"
"                float dt = st * g[it];\n"
"                float e = qs * dm;\n"
"                e = ds - e;\n"
"                e = e / ma;\n"
"                e = e * 0.5;\n"
"                float es = e * fw;\n"
"                e = qt * dm;\n"
"                e = dt - e;\n"
"                e = e / ma;\n"
"                e = e * 0.5;\n"
"                float et = e * fh;\n"
"                if (k == 0) { der.x = es; der.y = et; }\n"
"                else { der.z = es; der.w = et; }\n"
"            }\n"
"        }\n"
"        return face;\n"
"    }\n"
"\n"
"    uint4 gtexel(int un, int l, int2 ij)\n"
"    {\n"
"        int w = max(U->txsize[un * 2] >> l, 1);\n"
"        int i = U->txoff[un * 16 + min(l, 15)] + ij.y * w + ij.x;\n"
"        if (l > 15 || i < 0 || i >= U->txlen[un]) return uint4(0u);\n"
"        return uint4(TX[un][i]);\n"
"    }\n"
"\n"
"    uint4 gfetch(int un, int l, int f, int i, int j)\n"
"    {\n"
"        if (i < 0 || j < 0)\n"
"            return uint4(int4(U->txtf[un * 16 + 11], U->txtf[un * 16 + 12],\n"
"                              U->txtf[un * 16 + 13], U->txtf[un * 16 + 14]));\n"
"        return gtexel(un, l, int2(i, j + f * max(U->txsize[un * 2 + 1] >> l,\n"
"                                                 1)));\n"
"    }\n"
"\n"
"    uint4 glevel(int un, int f, int l, float fs, float ft, bool lin)\n"
"    {\n"
"        int2 cl = int2(U->txclamp[un * 2], U->txclamp[un * 2 + 1]);\n"
"        int w = max(U->txsize[un * 2] >> l, 1);\n"
"        int h = max(U->txsize[un * 2 + 1] >> l, 1);\n"
"        float ss = tc_pre(ldexp(fs, -min(l, U->txtf[un * 16 + 9])), w, cl.x);\n"
"        float tt = tc_pre(ldexp(ft, -min(l, U->txtf[un * 16 + 10])), h, cl.y);\n"
"        if (!lin)\n"
"            return gfetch(un, l, f, tc_idx(int(floor(ss)), w, cl.x, true),\n"
"                          tc_idx(int(floor(tt)), h, cl.y, true));\n"
"        float fx = ss - 0.5;\n"
"        float fy = tt - 0.5;\n"
"        float x0 = floor(fx);\n"
"        float y0 = floor(fy);\n"
"        float qx = fx - x0;\n"
"        float qy = fy - y0;\n"
"        int wx = int(qx * 256.0 + 0.5);\n"
"        int wy = int(qy * 256.0 + 0.5);\n"
"        int i0 = int(x0), j0 = int(y0);\n"
"        if (wx == 256) { i0++; wx = 0; }\n"
"        if (wy == 256) { j0++; wy = 0; }\n"
"        int i1 = tc_idx(i0 + 1, w, cl.x, false);\n"
"        int j1 = tc_idx(j0 + 1, h, cl.y, false);\n"
"        i0 = tc_idx(i0, w, cl.x, false);\n"
"        j0 = tc_idx(j0, h, cl.y, false);\n"
"        uint4 t00 = gfetch(un, l, f, i0, j0);\n"
"        uint4 t10 = wx != 0 ? gfetch(un, l, f, i1, j0) : t00;\n"
"        if (wy == 0) return wx != 0 ? tlerp(t00, t10, wx) : t00;\n"
"        uint4 t01 = gfetch(un, l, f, i0, j1);\n"
"        uint4 t11 = wx != 0 ? gfetch(un, l, f, i1, j1) : t01;\n"
"        int4 top = int4(t00) * (256 - wx) + int4(t10) * wx;\n"
"        int4 bot = int4(t01) * (256 - wx) + int4(t11) * wx;\n"
"        return uint4((top * (256 - wy) + bot * wy + 32768) >> 16);\n"
"    }\n"
"\n"
"    uint4 gmip(int un, int f, int lod, float fs, float ft)\n"
"    {\n"
"        int b = un * 16;\n"
"        bool lin = U->txtf[b + 3] != 1;\n"
"        int lo = min(max(lod, U->txtf[b + 6] * 256), U->txtf[b + 7] * 256);\n"
"        if (U->txtf[b + 4] == 1)\n"
"            return glevel(un, f, min((lo + 128) >> 8, U->txtf[b + 7]), fs, ft,\n"
"                          lin);\n"
"        int l = lo >> 8;\n"
"        if (U->txtf[b + 4] != 2 || l >= U->txtf[b + 7] || (lo & 255) == 0)\n"
"            return glevel(un, f, l, fs, ft, lin);\n"
"        return tlerp(glevel(un, f, l, fs, ft, lin),\n"
"                     glevel(un, f, l + 1, fs, ft, lin), lo & 255);\n"
"    }\n"
"\n"
"    uint4 gfilter(int un, int f, float fs, float ft, float4 der)\n"
"    {\n"
"        int b = un * 16;\n"
"        int lod = 0, nl = 0;\n"
"        float ax = 0.0, ay = 0.0;\n"
"        if (U->txtf[b + 1] != 0) {\n"
"            float m = der.x * der.x;\n"
"            float n = der.y * der.y;\n"
"            float px = m + n;\n"
"            m = der.z * der.z;\n"
"            n = der.w * der.w;\n"
"            float py = m + n;\n"
"            if (U->txtf[b + 3] == 3) {\n"
"                int lmaj = tlog2(px >= py ? px : py);\n"
"                int lmin = tlog2(px >= py ? py : px);\n"
"                nl = min(max(((lmaj - lmin) / 2 + 255) >> 8, 0),\n"
"                         U->txtf[b + 5]);\n"
"                lod = (lmaj >> 1) - nl * 256;\n"
"                ax = px >= py ? der.x : der.z;\n"
"                ay = px >= py ? der.y : der.w;\n"
"            } else {\n"
"                lod = tlog2(px >= py ? px : py) >> 1;\n"
"            }\n"
"            lod += U->txtf[b + 8];\n"
"        }\n"
"        if (lod <= 0)\n"
"            return glevel(un, f, U->txtf[b + 6], fs, ft, U->txtf[b + 2] != 1);\n"
"        if (nl == 0) return gmip(un, f, lod, fs, ft);\n"
"        int N = 1 << nl;\n"
"        uint4 sum = uint4(0u);\n"
"        for (int k = 0; k < N; k++) {\n"
"            float ok = float(2 * k + 1 - N) / float(2 * N);\n"
"            float ds = ax * ok;\n"
"            float dt = ay * ok;\n"
"            float s1 = fs + ds;\n"
"            float t1 = ft + dt;\n"
"            sum += gmip(un, f, lod, s1, t1);\n"
"        }\n"
"        return (sum + uint(N >> 1)) >> uint(nl);\n"
"    }\n"
"\n"
"    uint4 gnearest(int un, float fs, float ft)\n"
"    {\n"
"        int w = U->txsize[un * 2], h = U->txsize[un * 2 + 1];\n"
"        int tx = int(fs);\n"
"        int ty = int(ft);\n"
"        if (U->txclamp[un * 2] <= 1 && w > 0) {\n"
"            tx = tx % w;\n"
"            if (tx < 0) tx += w;\n"
"        } else {\n"
"            tx = clamp(tx, 0, w - 1);\n"
"        }\n"
"        if (U->txclamp[un * 2 + 1] <= 1 && h > 0) {\n"
"            ty = ty % h;\n"
"            if (ty < 0) ty += h;\n"
"        } else {\n"
"            ty = clamp(ty, 0, h - 1);\n"
"        }\n"
"        return gtexel(un, 0, int2(tx, ty));\n"
"    }\n"
"\n"
"#define clamp r3_clamp\n"
;

static const char *mtl_fs_tail =
"#undef clamp\n"
"\n"
"    FOut run(VOut in, uint4 dst);\n"
"};\n"
"\n"
"#ifdef R350_USGEN\n"
"vec4 FS::us_fetch(int un, vec4 coord, int dset)\n"
"{\n"
"    if (((U->textured >> un) & 1) == 0) return float4(1.0);\n"
"    int w = U->txsize[un * 2], h = U->txsize[un * 2 + 1];\n"
"    float fs = coord.x * float(w);\n"
"    float ft = coord.y * float(h);\n"
"    uint4 tu;\n"
"    if (U->txtf[un * 16 + 15] != 0) {\n"
"        float2 fd;\n"
"        float4 der;\n"
"        bool lod = U->txtf[un * 16 + 1] != 0 && ((U->tcraw >> dset) & 1) != 0;\n"
"        int f = cube_coord(coord.xyz, us_cdx[dset], us_cdy[dset], lod,\n"
"                           w, h, fd, der);\n"
"        tu = gfilter(un, f, fd.x, fd.y, der);\n"
"    } else if (U->txtf[un * 16] != 0) {\n"
"        float4 der = float4(0.0);\n"
"        if (U->txtf[un * 16 + 1] != 0) {\n"
"            float4 g = us_der[dset];\n"
"            float d0 = g.x * float(w);\n"
"            float d1 = g.y * float(h);\n"
"            float d2 = g.z * float(w);\n"
"            float d3 = g.w * float(h);\n"
"            der = float4(d0, d1, d2, d3);\n"
"        }\n"
"        tu = gfilter(un, 0, fs, ft, der);\n"
"    } else {\n"
"        tu = gnearest(un, fs, ft);\n"
"    }\n"
"    return n255(tu);\n"
"}\n"
"#endif\n"
"\n"
"FOut FS::run(VOut in, uint4 dst)\n"
"{\n"
"    FOut o;\n"
"    float4 c;\n"
"    float ts, tt;\n"
"    float inv = in.inv.x;\n"
"    float px = in.pos.x;\n"
"    float py = in.pos.y;\n"
"    float q0 = (in.p2.y - in.p1.y) * (px - in.p1.x);\n"
"    float q1 = (in.p0.y - in.p2.y) * (px - in.p2.x);\n"
"    float d0 = fma(in.p2.x - in.p1.x, py - in.p1.y, -q0);\n"
"    float d1 = fma(in.p0.x - in.p2.x, py - in.p2.y, -q1);\n"
"    float w0 = d0 * inv;\n"
"    float w1 = d1 * inv;\n"
"    float w2 = 1.0 - w0 - w1;\n"
"    {\n"
"        float zp = w1 * in.z.y;\n"
"        float zf = fma(w2, in.z.z, fma(w0, in.z.x, zp));\n"
"        float zc = zf > 0.0 ? zf : 0.0;\n"
"        zc = zc < 1.0 ? zc : 1.0;\n"
"        float zs = zc * U->zscale;\n"
"        o.depth = float(uint(zs)) * 5.9604644775390625e-8;\n"
"    }\n"
"    if (U->zonly != 0) {\n"
"        o.col = dst;\n"
"        return o;\n"
"    }\n"
"    float iq = 1.0;\n"
"    bool persp = in.inv.y != in.inv.z || in.inv.z != in.inv.w;\n"
"    if (persp) {\n"
"        float pq0 = w0 * in.inv.y;\n"
"        float pq1 = w1 * in.inv.z;\n"
"        float pq2 = w2 * in.inv.w;\n"
"        float qs = pq0 + pq1 + pq2;\n"
"        iq = 1.0 / qs;\n"
"        float qe = fma(-qs, iq, 1.0);\n"
"        float qr = fma(iq, qe, iq);\n"
"        if (!isnan(qr)) iq = qr;\n"
"        w0 = pq0 * iq; w1 = pq1 * iq; w2 = pq2 * iq;\n"
"    }\n"
"    c = fma(float4(w2), in.c2, fma(float4(w0), in.c0, w1 * in.c1));\n"
"    float2 st = fma(float2(w2), in.t2, fma(float2(w0), in.t0, w1 * in.t1));\n"
"    ts = st.x; tt = st.y;\n"
"    float4 c1 = fma(float4(w2), in.s2, fma(float4(w0), in.s0, w1 * in.s1));\n"
"#ifdef R350_USGEN\n"
"    {\n"
"        int tb = int(in.prim) * U->tcstride;\n"
"        float3 ga = float3(0.0);\n"
"        float3 gb = float3(0.0);\n"
"        if (U->lodany != 0) {\n"
"            float a0 = -(in.p2.y - in.p1.y) * in.inv.x;\n"
"            float b0 = (in.p2.x - in.p1.x) * in.inv.x;\n"
"            float a1 = -(in.p0.y - in.p2.y) * in.inv.x;\n"
"            float b1 = (in.p0.x - in.p2.x) * in.inv.x;\n"
"            ga = float3(a0, a1, -(a0 + a1));\n"
"            gb = float3(b0, b1, -(b0 + b1));\n"
"            if (persp) {\n"
"                ga = ga * in.inv.yzw;\n"
"                gb = gb * in.inv.yzw;\n"
"            }\n"
"        }\n"
"        for (int k = 0; k < 8; k++) {\n"
"            us_tc[k] = float4(0.0, 0.0, 0.0, 1.0);\n"
"            us_der[k] = float4(0.0);\n"
"            us_cdx[k] = float3(0.0);\n"
"            us_cdy[k] = float3(0.0);\n"
"        }\n"
"        for (int k = 0; k < US_NTC; k++) {\n"
"            float4 s0 = TCB[tb + k * 6];\n"
"            float4 s1 = TCB[tb + k * 6 + 2];\n"
"            float4 s2 = TCB[tb + k * 6 + 4];\n"
"            float is = fma(w2, s2.x, fma(w0, s0.x, w1 * s1.x));\n"
"            float it = fma(w2, s2.y, fma(w0, s0.y, w1 * s1.y));\n"
"            float ns = is * U->tcinv[k * 2];\n"
"            float nt = it * U->tcinv[k * 2 + 1];\n"
"            float4 tc = float4(ns, nt, 0.0, 1.0);\n"
"            if (U->lodany != 0) {\n"
"                float dx, dy, ex, ey;\n"
"                tc_der(ga, gb, s0.x, s1.x, s2.x, is, iq, dx, dy);\n"
"                tc_der(ga, gb, s0.y, s1.y, s2.y, it, iq, ex, ey);\n"
"                float g0 = dx * U->tcinv[k * 2];\n"
"                float g1 = ex * U->tcinv[k * 2 + 1];\n"
"                float g2 = dy * U->tcinv[k * 2];\n"
"                float g3 = ey * U->tcinv[k * 2 + 1];\n"
"                us_der[k] = float4(g0, g1, g2, g3);\n"
"            }\n"
"            if (((U->tcraw >> k) & 1) != 0) {\n"
"                float4 r0 = TCB[tb + k * 6 + 1];\n"
"                float4 r1 = TCB[tb + k * 6 + 3];\n"
"                float4 r2 = TCB[tb + k * 6 + 5];\n"
"                tc = fma(float4(w2), r2, fma(float4(w0), r0, w1 * r1));\n"
"                if (U->lodany != 0) {\n"
"                    float x0, y0, x1, y1, x2, y2;\n"
"                    tc_der(ga, gb, r0.x, r1.x, r2.x, tc.x, iq, x0, y0);\n"
"                    tc_der(ga, gb, r0.y, r1.y, r2.y, tc.y, iq, x1, y1);\n"
"                    tc_der(ga, gb, r0.z, r1.z, r2.z, tc.z, iq, x2, y2);\n"
"                    us_cdx[k] = float3(x0, x1, x2);\n"
"                    us_cdy[k] = float3(y0, y1, y2);\n"
"                }\n"
"            }\n"
"            us_tc[k] = tc;\n"
"        }\n"
"        float4 shaded;\n"
"        bool kill;\n"
"        us_main(c, c1, shaded, kill);\n"
"        if (kill) discard_fragment();\n"
"        c = shaded;\n"
"    }\n"
"#else\n"
"    float4 texel = float4(1.0);\n"
"    if (U->textured != 0 && U->txtf[0] != 0) {\n"
"        float4 der = float4(0.0);\n"
"        if (U->txtf[1] != 0) {\n"
"            float a0 = -(in.p2.y - in.p1.y) * in.inv.x;\n"
"            float b0 = (in.p2.x - in.p1.x) * in.inv.x;\n"
"            float a1 = -(in.p0.y - in.p2.y) * in.inv.x;\n"
"            float b1 = (in.p0.x - in.p2.x) * in.inv.x;\n"
"            float3 ga = float3(a0, a1, -(a0 + a1));\n"
"            float3 gb = float3(b0, b1, -(b0 + b1));\n"
"            if (persp) {\n"
"                ga = ga * in.inv.yzw;\n"
"                gb = gb * in.inv.yzw;\n"
"            }\n"
"            float dx, dy;\n"
"            tc_der(ga, gb, in.t0.x, in.t1.x, in.t2.x, ts, iq, dx, dy);\n"
"            der.x = dx; der.z = dy;\n"
"            tc_der(ga, gb, in.t0.y, in.t1.y, in.t2.y, tt, iq, dx, dy);\n"
"            der.y = dx; der.w = dy;\n"
"        }\n"
"        texel = n255(gfilter(0, 0, ts, tt, der));\n"
"    } else if (U->textured != 0) {\n"
"        texel = n255(gnearest(0, ts, tt));\n"
"    }\n"
"    {\n"
"        float4 shaded;\n"
"        us_main(texel, c, c1, shaded);\n"
"        c = shaded;\n"
"    }\n"
"#endif\n"
"    if (U->alphatest != 0) {\n"
"        bool pass;\n"
"        float r = U->afref;\n"
"        if (U->affunc == 0)      pass = false;\n"
"        else if (U->affunc == 1) pass = c.a <  r;\n"
"        else if (U->affunc == 2) pass = c.a == r;\n"
"        else if (U->affunc == 3) pass = c.a <= r;\n"
"        else if (U->affunc == 4) pass = c.a >  r;\n"
"        else if (U->affunc == 5) pass = c.a != r;\n"
"        else if (U->affunc == 6) pass = c.a >= r;\n"
"        else                     pass = true;\n"
"        if (!pass) discard_fragment();\n"
"    }\n"
"    if (U->discard_ != 0) {\n"
"        bool a0 = c.a == 0.0, a1 = c.a == 1.0;\n"
"        bool z0 = c.r == 0.0 && c.g == 0.0 && c.b == 0.0;\n"
"        bool z1 = c.r == 1.0 && c.g == 1.0 && c.b == 1.0;\n"
"        bool kill = false;\n"
"        int m = U->discard_;\n"
"        if (m == 1) kill = a0;\n"
"        else if (m == 2) kill = z0;\n"
"        else if (m == 3) kill = a0 && z0;\n"
"        else if (m == 4) kill = a1;\n"
"        else if (m == 5) kill = z1;\n"
"        else if (m == 6) kill = a1 && z1;\n"
"        if (kill && U->dkeep != 0) {\n"
"            o.col = dst;\n"
"            return o;\n"
"        }\n"
"        if (kill) discard_fragment();\n"
"    }\n"
"    if (U->blend != 0) {\n"
"        float4 d = U->blendread != 0 ? n255(dst) : float4(0.0);\n"
"        float kr = U->konst[0], kg = U->konst[1], kb = U->konst[2];\n"
"        float ka = U->konst[3];\n"
"        int cs = U->cfac[0], cd = U->cfac[1], cf = U->cfac[2];\n"
"        int as = U->afac[0], ad = U->afac[1], af = U->afac[2];\n"
"        float nr = comb(cf, c.r * bf(cs, c.r, c.a, d.r, d.a, kr, ka),\n"
"                        d.r * bf(cd, c.r, c.a, d.r, d.a, kr, ka));\n"
"        float ng = comb(cf, c.g * bf(cs, c.g, c.a, d.g, d.a, kg, ka),\n"
"                        d.g * bf(cd, c.g, c.a, d.g, d.a, kg, ka));\n"
"        float nb = comb(cf, c.b * bf(cs, c.b, c.a, d.b, d.a, kb, ka),\n"
"                        d.b * bf(cd, c.b, c.a, d.b, d.a, kb, ka));\n"
"        c.a = comb(af, c.a * bf(as, c.a, c.a, d.a, d.a, ka, ka),\n"
"                   d.a * bf(ad, c.a, c.a, d.a, d.a, ka, ka));\n"
"        c.r = nr; c.g = ng; c.b = nb;\n"
"    }\n"
"    uint4 v = uint4(floor(min(max(c, float4(0.0)), float4(1.0)) * 255.0));\n"
"    uint m = U->wmask;\n"
"    bool4 wm = bool4((m & 0x00ff0000u) != 0u, (m & 0x0000ff00u) != 0u,\n"
"                     (m & 0x000000ffu) != 0u, (m & 0xff000000u) != 0u);\n"
"    o.col = select(dst, v, wm);\n"
"    return o;\n"
"}\n"
"\n"
"vertex VOut r350_vs(uint vid [[vertex_id]],\n"
"                    device const float *V [[buffer(0)]],\n"
"                    constant float2 &S [[buffer(1)]])\n"
"{\n"
"    device const float *a = V + vid * R350_VSTRIDE;\n"
"    VOut o;\n"
"    float nx = a[0] / S.x * 2.0 - 1.0;\n"
"    float ny = a[1] / S.y * 2.0 - 1.0;\n"
"    o.pos = float4(nx, -ny, 0.0, 1.0);\n"
"    o.p0 = float2(a[OFF_P0], a[OFF_P0 + 1]);\n"
"    o.p1 = float2(a[OFF_P1], a[OFF_P1 + 1]);\n"
"    o.p2 = float2(a[OFF_P2], a[OFF_P2 + 1]);\n"
"    o.c0 = float4(a[OFF_C0], a[OFF_C0 + 1], a[OFF_C0 + 2], a[OFF_C0 + 3]);\n"
"    o.c1 = float4(a[OFF_C1], a[OFF_C1 + 1], a[OFF_C1 + 2], a[OFF_C1 + 3]);\n"
"    o.c2 = float4(a[OFF_C2], a[OFF_C2 + 1], a[OFF_C2 + 2], a[OFF_C2 + 3]);\n"
"    o.t0 = float2(a[OFF_T0], a[OFF_T0 + 1]);\n"
"    o.t1 = float2(a[OFF_T1], a[OFF_T1 + 1]);\n"
"    o.t2 = float2(a[OFF_T2], a[OFF_T2 + 1]);\n"
"    o.inv = float4(a[OFF_INV], a[OFF_INV + 1], a[OFF_INV + 2],\n"
"                   a[OFF_INV + 3]);\n"
"    o.s0 = float4(a[OFF_S0], a[OFF_S0 + 1], a[OFF_S0 + 2], a[OFF_S0 + 3]);\n"
"    o.s1 = float4(a[OFF_S1], a[OFF_S1 + 1], a[OFF_S1 + 2], a[OFF_S1 + 3]);\n"
"    o.s2 = float4(a[OFF_S2], a[OFF_S2 + 1], a[OFF_S2 + 2], a[OFF_S2 + 3]);\n"
"    o.z = float4(a[OFF_Z], a[OFF_Z + 1], a[OFF_Z + 2], a[OFF_Z + 3]);\n"
"    o.prim = vid / 3;\n"
"    return o;\n"
"}\n"
"\n"
"fragment FOut r350_fs(VOut in [[stage_in]], uint4 dst [[color(0)]],\n"
"                      constant FsU &u [[buffer(0)]],\n"
"                      constant float4 *usk [[buffer(1)]],\n"
"                      constant float *n255 [[buffer(2)]],\n"
"                      device const float4 *tcb [[buffer(3)]],\n"
"                      device const uchar4 *x0 [[buffer(4)]],\n"
"                      device const uchar4 *x1 [[buffer(5)]],\n"
"                      device const uchar4 *x2 [[buffer(6)]],\n"
"                      device const uchar4 *x3 [[buffer(7)]],\n"
"                      device const uchar4 *x4 [[buffer(8)]],\n"
"                      device const uchar4 *x5 [[buffer(9)]],\n"
"                      device const uchar4 *x6 [[buffer(10)]],\n"
"                      device const uchar4 *x7 [[buffer(11)]])\n"
"{\n"
"    FS f;\n"
"    f.U = &u;\n"
"    f.USK = usk;\n"
"    f.N255 = n255;\n"
"    f.TCB = tcb;\n"
"    f.TX[0] = x0; f.TX[1] = x1; f.TX[2] = x2; f.TX[3] = x3;\n"
"    f.TX[4] = x4; f.TX[5] = x5; f.TX[6] = x6; f.TX[7] = x7;\n"
"    return f.run(in, dst);\n"
"}\n"
;

static const char *mtl_util_src =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct UOut { float4 pos [[position]]; };\n"
"vertex UOut r350_uvs(uint vid [[vertex_id]])\n"
"{\n"
"    float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"    UOut o;\n"
"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    return o;\n"
"}\n"
"struct ZOut { uint4 col [[color(0)]]; float depth [[depth(any)]]; };\n"
"fragment ZOut r350_zfs(UOut in [[stage_in]],\n"
"                       constant float &z [[buffer(0)]])\n"
"{\n"
"    ZOut o;\n"
"    o.col = uint4(0u);\n"
"    o.depth = z;\n"
"    return o;\n"
"}\n";

/* fast math off, contraction off */
static void mtl_exact_math(MTLCompileOptions *o)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#if defined(MAC_OS_VERSION_15_0) && \
    MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) {
        o.mathMode = MTLMathModeSafe;
        o.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    } else {
        o.fastMathEnabled = NO;
    }
#else
    o.fastMathEnabled = NO;
#endif
#pragma clang diagnostic pop
}

/*
 * One program's source: head, the translated program with its `out`
 * parameters made references, tail. NULL if the text is not one the
 * adaptation knows.
 */
static char *mtl_source(const char *us)
{
    const int C = R350_GL_TEXCOORDS;
    GString *s = g_string_new(NULL);
    gchar **parts;
    char *u1, *u2;

    if (!us || !strstr(us, "out vec4 outc")) {
        g_string_free(s, true);
        return NULL;
    }
    parts = g_strsplit(us, "out vec4 outc", -1);
    u1 = g_strjoinv("thread vec4 &outc", parts);
    g_strfreev(parts);
    parts = g_strsplit(u1, "out bool kill", -1);
    u2 = g_strjoinv("thread bool &kill", parts);
    g_strfreev(parts);
    g_free(u1);

    g_string_append_printf(s,
        "#define R350_VSTRIDE %d\n"
        "#define OFF_P0 %d\n#define OFF_P1 %d\n#define OFF_P2 %d\n"
        "#define OFF_C0 %d\n#define OFF_C1 %d\n#define OFF_C2 %d\n"
        "#define OFF_T0 %d\n#define OFF_T1 %d\n#define OFF_T2 %d\n"
        "#define OFF_INV %d\n"
        "#define OFF_S0 %d\n#define OFF_S1 %d\n#define OFF_S2 %d\n"
        "#define OFF_Z %d\n",
        R350_GL_VSTRIDE,
        6 + 2 * C, 8 + 2 * C, 10 + 2 * C,
        12 + 2 * C, 16 + 2 * C, 20 + 2 * C,
        24 + 2 * C, 24 + 4 * C, 24 + 6 * C,
        37 + 8 * C,
        25 + 8 * C, 29 + 8 * C, 33 + 8 * C,
        41 + 8 * C);
    g_string_append(s, mtl_fs_head);
    g_string_append(s, u2);
    g_string_append(s, mtl_fs_tail);
    g_free(u2);
    return g_string_free(s, false);
}

static void mtl_report(R350MtlCtx *g, uint64_t key, const char *what,
                       NSError *e)
{
    if (qatomic_fetch_inc(&g->reports) >= MTL_REPORTS) {
        return;
    }
    error_report("ati-radeon9800: Metal %s failed for program %016" PRIx64
                 "; its draws fall back to software: %s", what, key,
                 e ? e.localizedDescription.UTF8String : "");
}

static MTLRenderPipelineDescriptor *mtl_pdesc(id<MTLLibrary> lib,
                                              NSString *vs, NSString *fs,
                                              MTLColorWriteMask wm)
{
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc]
                                       init];
    id<MTLFunction> vf = [lib newFunctionWithName:vs];
    id<MTLFunction> ff = [lib newFunctionWithName:fs];

    pd.vertexFunction = vf;
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Uint;
    pd.colorAttachments[0].writeMask = wm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    [vf release];
    [ff release];
    return pd;
}

/* a pipeline built on this thread; nil if it would not build */
static id<MTLRenderPipelineState> mtl_build_sync(R350MtlCtx *g, uint64_t key,
                                                 const char *src)
{
    MTLCompileOptions *opt = [[MTLCompileOptions alloc] init];
    MTLRenderPipelineDescriptor *pd;
    id<MTLRenderPipelineState> pso = nil;
    id<MTLLibrary> lib;
    NSError *e = nil;

    mtl_exact_math(opt);
    lib = [g->dev newLibraryWithSource:[NSString stringWithUTF8String:src]
                               options:opt error:&e];
    [opt release];
    if (!lib) {
        mtl_report(g, key, "shader compile", e);
        return nil;
    }
    pd = mtl_pdesc(lib, @"r350_vs", @"r350_fs", MTLColorWriteMaskAll);
    e = nil;
    pso = [g->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!pso) {
        mtl_report(g, key, "pipeline creation", e);
    }
    [pd release];
    [lib release];
    return pso;
}

static void mtl_prog_done(R350MtlCtx *g, unsigned k, uint64_t gen,
                          id<MTLRenderPipelineState> pso)
{
    qemu_mutex_lock(&g->plock);
    if (g->prog[k].gen == gen && g->prog[k].state == MTL_P_PENDING) {
        if (pso) {
            g->prog[k].pso = [pso retain];
            g->prog[k].state = MTL_P_READY;
            g->prog_links++;
        } else {
            g->prog[k].state = MTL_P_FAILED;
            g->prog_failed++;
        }
    }
    qemu_mutex_unlock(&g->plock);
    qatomic_dec(&g->compiling);
}

/* compile into slot k, generation gen, on Metal's own threads */
static void mtl_build_async(R350MtlCtx *g, unsigned k, uint64_t gen,
                            uint64_t key, char *src)
{
    MTLCompileOptions *opt = [[MTLCompileOptions alloc] init];
    id<MTLDevice> dev = g->dev;

    mtl_exact_math(opt);
    qatomic_inc(&g->compiling);
    [dev newLibraryWithSource:[NSString stringWithUTF8String:src]
                      options:opt
            completionHandler:^(id<MTLLibrary> lib, NSError *e) {
        MTLRenderPipelineDescriptor *pd;

        if (!lib) {
            mtl_report(g, key, "shader compile", e);
            mtl_prog_done(g, k, gen, nil);
            return;
        }
        pd = mtl_pdesc(lib, @"r350_vs", @"r350_fs", MTLColorWriteMaskAll);
        [dev newRenderPipelineStateWithDescriptor:pd
                completionHandler:^(id<MTLRenderPipelineState> pso,
                                    NSError *e2) {
            if (!pso) {
                mtl_report(g, key, "pipeline creation", e2);
            }
            mtl_prog_done(g, k, gen, pso);
        }];
        [pd release];
    }];
    [opt release];
    g_free(src);
}

/* a slot for `key`, evicting one with no compile in flight; plock held */
static int mtl_prog_slot(R350MtlCtx *g, uint64_t key)
{
    unsigned k, v;

    for (k = 0; k < MTL_PROGSLOTS; k++) {
        v = (g->prog_next + k) % MTL_PROGSLOTS;
        if (g->prog[v].state != MTL_P_PENDING) {
            break;
        }
    }
    if (k == MTL_PROGSLOTS) {
        return -1;
    }
    g->prog_next = (v + 1) % MTL_PROGSLOTS;
    /* an in-flight command buffer keeps its own reference */
    [g->prog[v].pso release];
    g->prog[v].pso = nil;
    g->prog[v].key = key;
    g->prog[v].state = MTL_P_EMPTY;
    g->prog[v].gen = ++g->prog_gen;
    return v;
}

static int mtl_prog_find(R350MtlCtx *g, uint64_t key)
{
    unsigned k;

    for (k = 0; k < MTL_PROGSLOTS; k++) {
        if (g->prog[k].state != MTL_P_EMPTY && g->prog[k].key == key) {
            return k;
        }
    }
    return -1;
}

int ati_r350_gl_prog_ready(R350MtlCtx *g, uint64_t key, bool add,
                           const char *glsl, uint32_t wmask)
{
    uint64_t gen;
    int k, ret;
    char *src;

    if (!g) {
        return -1;
    }
    if (!g->async) {
        return 1;
    }
    qemu_mutex_lock(&g->plock);
    k = mtl_prog_find(g, key);
    if (k >= 0) {
        ret = g->prog[k].state == MTL_P_READY ? 1
              : g->prog[k].state == MTL_P_FAILED ? -1 : 0;
        if (!ret) {
            g->w_waits++;
        }
        qemu_mutex_unlock(&g->plock);
        return ret;
    }
    g->w_waits++;
    src = mtl_source(glsl);
    if (!src) {
        qemu_mutex_unlock(&g->plock);
        return -1;
    }
    k = mtl_prog_slot(g, key);
    if (k < 0) {
        qemu_mutex_unlock(&g->plock);
        g_free(src);
        return 0;
    }
    g->prog[k].state = MTL_P_PENDING;
    gen = g->prog[k].gen;
    /* a handler may run before the call returns, and takes plock */
    qemu_mutex_unlock(&g->plock);
    @autoreleasepool {
        mtl_build_async(g, k, gen, key, src);
    }
    return 0;
}

void ati_r350_gl_worker_stats(R350MtlCtx *g, uint64_t *warms, uint64_t *failed,
                              uint64_t *waits, unsigned *inflight)
{
    *warms = g ? g->prog_links : 0;
    *failed = g ? g->prog_failed : 0;
    *waits = g ? g->w_waits : 0;
    *inflight = g ? qatomic_read(&g->compiling) : 0;
}

/* the pipeline for this request, building it here if nobody has */
static id<MTLRenderPipelineState> mtl_prog_for(R350MtlCtx *g,
                                               const R350GlReq *r)
{
    id<MTLRenderPipelineState> pso = nil;
    char *src;
    int k;

    qemu_mutex_lock(&g->plock);
    k = mtl_prog_find(g, r->us_key);
    if (k >= 0) {
        if (g->prog[k].state == MTL_P_READY) {
            g->prog_hits++;
            pso = g->prog[k].pso;
        }
        qemu_mutex_unlock(&g->plock);
        return pso;
    }
    k = mtl_prog_slot(g, r->us_key);
    if (k >= 0) {
        g->prog[k].state = MTL_P_PENDING;
    }
    qemu_mutex_unlock(&g->plock);
    src = mtl_source(r->us_glsl);
    if (k < 0 || !src) {
        g_free(src);
        if (k >= 0) {
            qemu_mutex_lock(&g->plock);
            g->prog[k].state = MTL_P_FAILED;
            qemu_mutex_unlock(&g->plock);
        }
        return nil;
    }
    pso = mtl_build_sync(g, r->us_key, src);
    g_free(src);
    qemu_mutex_lock(&g->plock);
    if (pso) {
        g->prog[k].pso = pso;
        g->prog[k].state = MTL_P_READY;
        g->prog_links++;
    } else {
        g->prog[k].state = MTL_P_FAILED;
        g->prog_failed++;
    }
    qemu_mutex_unlock(&g->plock);
    return pso;
}

/* ------------------------------------------------------------------ */
/* command buffers and the render pass */

static void mtl_end_enc(R350MtlCtx *g)
{
    if (g->enc) {
        [g->enc endEncoding];
        [g->enc release];
        g->enc = nil;
    }
}

static id<MTLCommandBuffer> mtl_cb(R350MtlCtx *g)
{
    if (!g->cb) {
        g->cb = [[g->q commandBuffer] retain];
        g->ser_open = ++g->ser_next;
        g->cb_draws = 0;
        g->cb_zq = 0;
    }
    return g->cb;
}

static void mtl_done_to(R350MtlCtx *g, uint64_t s)
{
    uint64_t d = qatomic_read(&g->ser_done);

    while (d < s) {
        uint64_t o = qatomic_cmpxchg(&g->ser_done, d, s);

        if (o == d) {
            break;
        }
        d = o;
    }
}

/* commit the open command buffer, if any */
static void mtl_commit(R350MtlCtx *g)
{
    uint64_t s = g->ser_open;

    mtl_end_enc(g);
    if (!g->cb) {
        return;
    }
    qatomic_inc(&g->handlers);
    [g->cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        if (b.status == MTLCommandBufferStatusError) {
            g->gpu_failed = true;
        }
        mtl_done_to(g, s);
        qatomic_dec(&g->handlers);
    }];
    [g->cb commit];
    [g->last release];
    g->last = g->cb;
    g->ser_last = s;
    g->cb = nil;
    g->ser_open = 0;
    g->commits++;
}

/* until every command buffer up to serial s has completed */
static void mtl_wait(R350MtlCtx *g, uint64_t s)
{
    if (qatomic_read(&g->ser_done) >= s) {
        return;
    }
    if (g->ser_open && s >= g->ser_open) {
        mtl_commit(g);
    }
    if (g->last) {
        [g->last waitUntilCompleted];
        if (g->last.status == MTLCommandBufferStatusError) {
            g->gpu_failed = true;
        }
        mtl_done_to(g, g->ser_last);
    }
}

/* everything so far, completed */
static void mtl_finish(R350MtlCtx *g)
{
    mtl_commit(g);
    mtl_wait(g, g->ser_last);
}

static id<MTLRenderCommandEncoder> mtl_enc(R350MtlCtx *g)
{
    MTLRenderPassDescriptor *rp;
    unsigned k;

    if (g->enc) {
        return g->enc;
    }
    mtl_cb(g);
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g->cbuf;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = g->zbuf;
    rp.depthAttachment.loadAction = MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = g->zbuf;
    rp.stencilAttachment.loadAction = MTLLoadActionLoad;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.visibilityResultBuffer = g->vis;
    g->enc = [[g->cb renderCommandEncoderWithDescriptor:rp] retain];
    [g->enc setFrontFacingWinding:MTLWindingClockwise];
    [g->enc setCullMode:MTLCullModeNone];
    [g->enc setFragmentBuffer:g->n255 offset:0 atIndex:2];
    g->enc_pso = nil;
    g->enc_dss = nil;
    g->enc_sref = -1;
    g->enc_vw = g->enc_vh = 0;
    for (k = 0; k < R350_GL_TEXUNITS; k++) {
        g->enc_tx[k] = nil;
    }
    g->passes++;
    return g->enc;
}

static id<MTLBlitCommandEncoder> mtl_blit(R350MtlCtx *g)
{
    mtl_end_enc(g);
    return [mtl_cb(g) blitCommandEncoder];
}

/*
 * `len` bytes of CPU-written, GPU-read memory for the open command
 * buffer, from an arena when it fits in one.
 */
static id<MTLBuffer> mtl_alloc(R350MtlCtx *g, size_t len, size_t *off)
{
    uint64_t s = mtl_cb(g) ? g->ser_open : 0;
    uint64_t done = qatomic_read(&g->ser_done);
    unsigned k;

    len = ROUND_UP(MAX(len, 16), 256);
    if (len > MTL_ARENA / 4) {
        *off = 0;
        return [[g->dev newBufferWithLength:len
                                    options:MTLResourceStorageModeShared]
                autorelease];
    }
    if (g->narena && g->aused + len <= MTL_ARENA) {
        MtlArena *a = &g->arena[g->acur];

        a->serial = s;
        *off = g->aused;
        g->aused += len;
        return a->buf;
    }
    for (k = 0; k < g->narena; k++) {
        if (g->arena[k].serial <= done) {
            break;
        }
    }
    if (k == g->narena) {
        if (g->narena < MTL_ARENAS) {
            g->arena[k].buf = [g->dev newBufferWithLength:MTL_ARENA
                                    options:MTLResourceStorageModeShared];
            g->narena++;
        } else {
            *off = 0;
            return [[g->dev newBufferWithLength:len
                                        options:MTLResourceStorageModeShared]
                    autorelease];
        }
    }
    g->acur = k;
    g->arena[k].serial = s;
    g->aused = len;
    *off = 0;
    return g->arena[k].buf;
}

/* ------------------------------------------------------------------ */
/* depth/stencil state */

static const MTLCompareFunction mtl_zfunc[8] = {
    MTLCompareFunctionNever, MTLCompareFunctionLess,
    MTLCompareFunctionLessEqual, MTLCompareFunctionEqual,
    MTLCompareFunctionGreaterEqual, MTLCompareFunctionGreater,
    MTLCompareFunctionNotEqual, MTLCompareFunctionAlways,
};

static const MTLStencilOperation mtl_sop[8] = {
    MTLStencilOperationKeep, MTLStencilOperationZero,
    MTLStencilOperationReplace, MTLStencilOperationIncrementClamp,
    MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
    MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap,
};

static id<MTLDepthStencilState> mtl_dss(R350MtlCtx *g, const R350GlZ *zin)
{
    MTLDepthStencilDescriptor *d;
    R350GlZ z;
    unsigned k, f;

    memset(&z, 0, sizeof(z));
    if (zin && zin->mode) {
        z = *zin;
    }
    for (k = 0; k < g->ndss; k++) {
        if (!memcmp(&g->dss[k].z, &z, sizeof(z))) {
            return g->dss[k].s;
        }
    }
    d = [[MTLDepthStencilDescriptor alloc] init];
    if (z.mode && z.test) {
        d.depthCompareFunction = mtl_zfunc[z.func & 7];
        d.depthWriteEnabled = z.write ? YES : NO;
    } else {
        d.depthCompareFunction = MTLCompareFunctionAlways;
        d.depthWriteEnabled = NO;
    }
    if (z.mode == 1 && z.stencil) {
        for (f = 0; f < 2; f++) {
            MTLStencilDescriptor *s = [[MTLStencilDescriptor alloc] init];

            s.stencilCompareFunction = mtl_zfunc[z.sfunc[f] & 7];
            s.stencilFailureOperation = mtl_sop[z.sfail[f] & 7];
            s.depthFailureOperation = mtl_sop[z.szfail[f] & 7];
            s.depthStencilPassOperation = mtl_sop[z.szpass[f] & 7];
            s.readMask = z.smask & 0xff;
            s.writeMask = z.swmask & 0xff;
            if (f) {
                d.backFaceStencil = s;
            } else {
                d.frontFaceStencil = s;
            }
            [s release];
        }
    }
    if (g->ndss < MTL_DSS) {
        k = g->ndss++;
    } else {
        k = g->dss_next;
        g->dss_next = (g->dss_next + 1) % MTL_DSS;
        [g->dss[k].s release];
    }
    g->dss[k].z = z;
    g->dss[k].s = [g->dev newDepthStencilStateWithDescriptor:d];
    [d release];
    return g->dss[k].s;
}

/* ------------------------------------------------------------------ */
/* open, close, statistics */

R350MtlCtx *ati_r350_gl_open(const char **err, unsigned workers)
{
    R350MtlCtx *g;
    float n255[256];
    uint8_t white[16];
    unsigned k;

    *err = NULL;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLLibrary> lib;
        MTLRenderPipelineDescriptor *pd;
        MTLDepthStencilDescriptor *dd;
        MTLStencilDescriptor *sd;
        MTLCompileOptions *opt;
        NSError *e = nil;
        char *probe;

        if (!dev) {
            *err = "no Metal device";
            return NULL;
        }
        if (![dev supportsFamily:MTLGPUFamilyApple1]) {
            /* the blend reads the colour attachment in the fragment stage */
            [dev release];
            *err = "the Metal backend needs an Apple GPU";
            return NULL;
        }
        g = g_new0(R350MtlCtx, 1);
        g->dev = dev;
        g->q = [dev newCommandQueue];
        for (k = 0; k < 256; k++) {
            n255[k] = (float)k / 255.0f;
        }
        g->n255 = [dev newBufferWithBytes:n255 length:sizeof(n255)
                                  options:MTLResourceStorageModeShared];
        memset(white, 0xff, sizeof(white));
        g->white = [dev newBufferWithBytes:white length:sizeof(white)
                                   options:MTLResourceStorageModeShared];
        g->dummy = [dev newBufferWithLength:sizeof(float) * 4 * R350_GL_USK
                                    options:MTLResourceStorageModeShared];
        g->vis = [dev newBufferWithLength:(MTL_ZQ_SLOTS + 1) * 8
                                  options:MTLResourceStorageModeShared];
        memset(g->vis.contents, 0, (MTL_ZQ_SLOTS + 1) * 8);
        g->zq_ser = g_new0(uint64_t, MTL_ZQ_SLOTS);
        qemu_mutex_init(&g->plock);

        opt = [[MTLCompileOptions alloc] init];
        lib = [dev newLibraryWithSource:@(mtl_util_src) options:opt
                                  error:&e];
        [opt release];
        if (lib) {
            pd = mtl_pdesc(lib, @"r350_uvs", @"r350_zfs",
                           MTLColorWriteMaskNone);
            g->zclear_pso = [dev newRenderPipelineStateWithDescriptor:pd
                                                                error:&e];
            [pd release];
            [lib release];
        }
        dd = [[MTLDepthStencilDescriptor alloc] init];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        sd = [[MTLStencilDescriptor alloc] init];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.readMask = 0xff;
        sd.writeMask = 0xff;
        dd.frontFaceStencil = sd;
        dd.backFaceStencil = sd;
        g->zclear_dss = [dev newDepthStencilStateWithDescriptor:dd];
        [sd release];
        [dd release];

        /* a head or tail that will not build says so now */
        probe = mtl_source("void us_main(vec4 tex0, vec4 col0, vec4 col1,\n"
                           "             out vec4 outc)\n"
                           "{\n"
                           "    precise vec4 R0 = col0 * tex0;\n"
                           "    R0.rgb = clamp(R0.rgb + USK[0].rgb, 0.0,"
                           " 1.0);\n"
                           "    outc = vec4(R0.rgb, inversesqrt(abs(col1.a)"
                           " + 1.0));\n"
                           "}\n");
        {
            id<MTLRenderPipelineState> p = mtl_build_sync(g, 0, probe);

            g_free(probe);
            if (!p || !g->zclear_pso) {
                *err = "Metal shaders would not build";
                [p release];
                ati_r350_gl_close(g);
                return NULL;
            }
            [p release];
        }
        g->async = workers > 0;
        snprintf(g->desc, sizeof(g->desc), "Metal %s",
                 dev.name.UTF8String);
    }
    return g;
}

void ati_r350_gl_close(R350MtlCtx *g)
{
    unsigned k;

    if (!g) {
        return;
    }
    @autoreleasepool {
        mtl_finish(g);
        while (qatomic_read(&g->compiling) || qatomic_read(&g->handlers)) {
            g_usleep(1000);
        }
        [g->last release];
        for (k = 0; k < MTL_PROGSLOTS; k++) {
            [g->prog[k].pso release];
        }
        for (k = 0; k < g->ndss; k++) {
            [g->dss[k].s release];
        }
        for (k = 0; k <= R350_GL_TEXSLOTS; k++) {
            [g->tex[k].buf release];
        }
        for (k = 0; k < g->narena; k++) {
            [g->arena[k].buf release];
        }
        [g->cbuf release];
        [g->zbuf release];
        [g->zclear_pso release];
        [g->zclear_dss release];
        [g->n255 release];
        [g->white release];
        [g->dummy release];
        [g->vis release];
        [g->q release];
        [g->dev release];
    }
    qemu_mutex_destroy(&g->plock);
    g_free(g->zq_ser);
    g_free(g);
}

const char *ati_r350_gl_describe(R350MtlCtx *g)
{
    return g ? g->desc : "none";
}

void ati_r350_gl_prog_stats(R350MtlCtx *g, uint64_t *hits, uint64_t *links,
                            uint64_t *failed)
{
    *hits = g ? g->prog_hits : 0;
    *links = g ? g->prog_links : 0;
    *failed = g ? g->prog_failed : 0;
}

uint64_t ati_r350_gl_barriers(R350MtlCtx *g)
{
    return g ? g->passes : 0;
}

void ati_r350_gl_queue_stats(R350MtlCtx *g, uint64_t *units, uint64_t *flushes,
                             uint64_t *waves)
{
    *units = g ? g->draws : 0;
    *flushes = g ? g->commits : 0;
    *waves = g ? g->passes : 0;
}

void ati_r350_gl_hold(bool hold)
{
}

/*
 * The engine went idle: start the GPU on a batch worth a render pass.
 * Ending a pass stores and reloads the whole target, so short batches
 * stay open until something reads them back.
 */
void ati_r350_gl_unbind(R350MtlCtx *g)
{
    if (g && g->cb && g->cb_draws >= MTL_IDLE_DRAWS) {
        @autoreleasepool {
            mtl_commit(g);
        }
    }
}

/* ------------------------------------------------------------------ */
/* the target, seed and fetch */

bool ati_r350_gl_target(R350MtlCtx *g, int w, int h, bool *lost)
{
    MTLTextureDescriptor *td;

    *lost = false;
    if (!g || w <= 0 || h <= 0) {
        return false;
    }
    if (w <= g->fb_w && h <= g->fb_h) {
        return true;
    }
    /* grow only, as the GL backend does */
    w = MAX(w, g->fb_w);
    h = MAX(h, g->fb_h);
    if (w > 16384 || h > 16384) {
        return false;
    }
    @autoreleasepool {
        mtl_commit(g);
        [g->cbuf release];
        [g->zbuf release];
        td = [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
                                           width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        g->cbuf = [g->dev newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:
                  MTLPixelFormatDepth32Float_Stencil8
                                           width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        g->zbuf = [g->dev newTextureWithDescriptor:td];
    }
    if (!g->cbuf || !g->zbuf) {
        [g->cbuf release];
        [g->zbuf release];
        g->cbuf = g->zbuf = nil;
        g->fb_w = g->fb_h = 0;
        return false;
    }
    g->fb_w = w;
    g->fb_h = h;
    *lost = true;
    return true;
}

static bool mtl_rect_ok(R350MtlCtx *g, int x0, int y0, int w, int h)
{
    return g && g->cbuf && w > 0 && h > 0 && x0 >= 0 && y0 >= 0 &&
           x0 + w <= g->fb_w && y0 + h <= g->fb_h;
}

bool ati_r350_gl_seed(R350MtlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr)
{
    if (!mtl_rect_ok(g, x0, y0, w, h)) {
        return false;
    }
    @autoreleasepool {
        size_t off;
        id<MTLBuffer> b = mtl_alloc(g, (size_t)w * h * 4, &off);
        uint8_t *st = (uint8_t *)b.contents + off;
        id<MTLBlitCommandEncoder> bl;
        int x, y;

        for (y = 0; y < h; y++) {
            const uint8_t *p = base + (size_t)(y0 + y) * pitch +
                               (size_t)x0 * 4;
            uint8_t *o = st + (size_t)y * w * 4;

            for (x = 0; x < w; x++, p += 4, o += 4) {
                o[0] = p[2 ^ xr];
                o[1] = p[1 ^ xr];
                o[2] = p[0 ^ xr];
                o[3] = p[3 ^ xr];
            }
        }
        bl = mtl_blit(g);
        [bl copyFromBuffer:b sourceOffset:off
            sourceBytesPerRow:(NSUInteger)w * 4
            sourceBytesPerImage:(NSUInteger)w * h * 4
            sourceSize:MTLSizeMake(w, h, 1)
            toTexture:g->cbuf destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(x0, y0, 0)];
        [bl endEncoding];
    }
    return true;
}

/* the colour rectangle as packed RGBA8 rows of w, completed */
static bool mtl_read_rect(R350MtlCtx *g, int x0, int y0, int w, int h,
                          uint8_t *out)
{
    id<MTLBuffer> b = [[g->dev newBufferWithLength:(size_t)w * h * 4
                                           options:MTLResourceStorageModeShared]
                       autorelease];
    id<MTLBlitCommandEncoder> bl = mtl_blit(g);

    [bl copyFromTexture:g->cbuf sourceSlice:0 sourceLevel:0
           sourceOrigin:MTLOriginMake(x0, y0, 0)
             sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:b destinationOffset:0
    destinationBytesPerRow:(NSUInteger)w * 4
  destinationBytesPerImage:(NSUInteger)w * h * 4];
    [bl endEncoding];
    mtl_finish(g);
    memcpy(out, b.contents, (size_t)w * h * 4);
    return !g->gpu_failed;
}

bool ati_r350_gl_fetch(R350MtlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr)
{
    bool ok;

    if (!mtl_rect_ok(g, x0, y0, w, h)) {
        return false;
    }
    @autoreleasepool {
        uint8_t *st = g_malloc((size_t)w * h * 4);
        int x, y;

        ok = mtl_read_rect(g, x0, y0, w, h, st);
        for (y = 0; ok && y < h; y++) {
            uint8_t *p = base + (size_t)(y0 + y) * pitch + (size_t)x0 * 4;
            const uint8_t *i = st + (size_t)y * w * 4;

            for (x = 0; x < w; x++, p += 4, i += 4) {
                p[2 ^ xr] = i[0];
                p[1 ^ xr] = i[1];
                p[0 ^ xr] = i[2];
                p[3 ^ xr] = i[3];
            }
        }
        g_free(st);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* depth */

#define MTL_ZUNIT (1.0f / 16777216.0f)

bool ati_r350_gl_depth(R350MtlCtx *g)
{
    return g != NULL;
}

/* Z words of the rectangle, r300_zb_pixel()'s layout for `mode` */
static bool mtl_zread(R350MtlCtx *g, int x0, int y0, int w, int h,
                      uint32_t *z, int mode)
{
    size_t n = (size_t)w * h, i;
    id<MTLBuffer> bz = [[g->dev newBufferWithLength:n * 4
                                            options:MTLResourceStorageModeShared]
                        autorelease];
    id<MTLBuffer> bs = [[g->dev newBufferWithLength:n
                                            options:MTLResourceStorageModeShared]
                        autorelease];
    id<MTLBlitCommandEncoder> bl = mtl_blit(g);
    const float *fz;
    const uint8_t *fs;

    [bl copyFromTexture:g->zbuf sourceSlice:0 sourceLevel:0
           sourceOrigin:MTLOriginMake(x0, y0, 0)
             sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:bz destinationOffset:0
    destinationBytesPerRow:(NSUInteger)w * 4
  destinationBytesPerImage:n * 4
                 options:MTLBlitOptionDepthFromDepthStencil];
    [bl copyFromTexture:g->zbuf sourceSlice:0 sourceLevel:0
           sourceOrigin:MTLOriginMake(x0, y0, 0)
             sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:bs destinationOffset:0
    destinationBytesPerRow:(NSUInteger)w
  destinationBytesPerImage:n
                 options:MTLBlitOptionStencilFromDepthStencil];
    [bl endEncoding];
    mtl_finish(g);
    fz = bz.contents;
    fs = bs.contents;
    for (i = 0; i < n; i++) {
        uint32_t zi = (uint32_t)(fz[i] * 16777216.0f);

        z[i] = mode == 2 ? zi & 0xffff : (zi << 8) | fs[i];
    }
    return !g->gpu_failed;
}

bool ati_r350_gl_zseed(R350MtlCtx *g, int x0, int y0, int w, int h,
                       const uint32_t *z, int mode)
{
    if (!mtl_rect_ok(g, x0, y0, w, h)) {
        return false;
    }
    @autoreleasepool {
        size_t n = (size_t)w * h, i, oz, os;
        id<MTLBuffer> bz = mtl_alloc(g, n * 4, &oz);
        id<MTLBuffer> bs = mtl_alloc(g, n, &os);
        float *fz = (float *)((uint8_t *)bz.contents + oz);
        uint8_t *fs = (uint8_t *)bs.contents + os;
        id<MTLBlitCommandEncoder> bl;

        for (i = 0; i < n; i++) {
            uint32_t zi = mode == 2 ? z[i] & 0xffff : z[i] >> 8;

            fz[i] = (float)zi * MTL_ZUNIT;
            fs[i] = mode == 2 ? 0 : z[i] & 0xff;
        }
        bl = mtl_blit(g);
        [bl copyFromBuffer:bz sourceOffset:oz
            sourceBytesPerRow:(NSUInteger)w * 4 sourceBytesPerImage:n * 4
            sourceSize:MTLSizeMake(w, h, 1)
            toTexture:g->zbuf destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(x0, y0, 0)
            options:MTLBlitOptionDepthFromDepthStencil];
        [bl copyFromBuffer:bs sourceOffset:os
            sourceBytesPerRow:(NSUInteger)w sourceBytesPerImage:n
            sourceSize:MTLSizeMake(w, h, 1)
            toTexture:g->zbuf destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(x0, y0, 0)
            options:MTLBlitOptionStencilFromDepthStencil];
        [bl endEncoding];
    }
    return true;
}

bool ati_r350_gl_zfetch(R350MtlCtx *g, int x0, int y0, int w, int h,
                        uint32_t *z, int mode)
{
    bool ok;

    if (!mtl_rect_ok(g, x0, y0, w, h)) {
        return false;
    }
    @autoreleasepool {
        ok = mtl_zread(g, x0, y0, w, h, z, mode);
    }
    return ok;
}

bool ati_r350_gl_zclear(R350MtlCtx *g, int x0, int y0, int w, int h,
                        uint32_t z, int mode)
{
    uint32_t zi = mode == 2 ? z & 0xffff : z >> 8;
    float zf = (float)zi * MTL_ZUNIT;

    if (!mtl_rect_ok(g, x0, y0, w, h)) {
        return false;
    }
    @autoreleasepool {
        id<MTLRenderCommandEncoder> e = mtl_enc(g);
        MTLViewport vp = { 0, 0, g->fb_w, g->fb_h, 0.0, 1.0 };
        MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0,
                              (NSUInteger)w, (NSUInteger)h };

        [e setRenderPipelineState:g->zclear_pso];
        [e setDepthStencilState:g->zclear_dss];
        [e setStencilReferenceValue:mode == 2 ? 0 : z & 0xff];
        [e setViewport:vp];
        [e setScissorRect:sc];
        [e setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        [e setFragmentBytes:&zf length:sizeof(zf) atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
              vertexCount:3];
        g->enc_pso = nil;
        g->enc_dss = nil;
        g->enc_sref = -1;
        g->enc_vw = g->enc_vh = 0;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* occlusion counting */

uint64_t ati_r350_gl_zq_mark(R350MtlCtx *g)
{
    if (!g) {
        return 0;
    }
    if (g->cb_zq) {
        @autoreleasepool {
            mtl_commit(g);
        }
    }
    return g->zq_issued;
}

bool ati_r350_gl_zq_sum(R350MtlCtx *g, uint64_t ticket, bool wait,
                        uint64_t *sum)
{
    uint64_t *vis;
    bool ok = true;

    if (!g || ticket < g->zq_done || ticket > g->zq_issued) {
        return false;
    }
    vis = g->vis.contents;
    @autoreleasepool {
        while (g->zq_done < ticket) {
            unsigned sl = g->zq_done % MTL_ZQ_SLOTS;
            uint64_t s = g->zq_ser[sl];

            if (qatomic_read(&g->ser_done) < s) {
                if (!wait) {
                    if (g->ser_open && s >= g->ser_open) {
                        mtl_commit(g);
                    }
                    ok = false;
                    break;
                }
                mtl_wait(g, s);
            }
            g->zq_sum += vis[sl];
            vis[sl] = 0;
            g->zq_done++;
        }
    }
    *sum = g->zq_sum;
    return ok;
}

/* ------------------------------------------------------------------ */
/* textures */

void ati_r350_gl_tex_forget(R350MtlCtx *g, unsigned slot)
{
    if (!g || slot >= R350_GL_TEXSLOTS) {
        return;
    }
    [g->tex[slot].buf release];
    memset(&g->tex[slot], 0, sizeof(g->tex[slot]));
}

static int mtl_levels_unit(const R350GlReq *r, unsigned un)
{
    return r->filt[un][0] ? MAX(r->levels[un], 1) : 1;
}

/* unit un's levels into the slot's buffer */
static bool mtl_upload(R350MtlCtx *g, unsigned slot, const R350GlReq *r,
                       unsigned un)
{
    MtlTex *t = &g->tex[slot];
    int nl = MIN(mtl_levels_unit(r, un), MTL_LEVELS);
    int nf = r->cube[un] ? 6 : 1;
    size_t texels = 0;
    int l;

    for (l = 0; l < nl; l++) {
        t->off[l] = texels;
        texels += (size_t)MAX(r->tex_w[un] >> l, 1) *
                  MAX(r->tex_h[un] >> l, 1) * nf;
    }
    if (!texels || texels > INT32_MAX / 4) {
        return false;
    }
    for (; l < MTL_LEVELS; l++) {
        t->off[l] = texels;
    }
    if (t->buf && t->len == (int)texels &&
        t->use <= qatomic_read(&g->ser_done)) {
        memcpy(t->buf.contents, r->tex[un], texels * 4);
    } else {
        [t->buf release];
        t->buf = [g->dev newBufferWithBytes:r->tex[un] length:texels * 4
                                    options:MTLResourceStorageModeShared];
    }
    t->w = r->tex_w[un];
    t->h = r->tex_h[un];
    t->nl = nl;
    t->cube = r->cube[un];
    t->len = texels;
    return t->buf != nil;
}

static bool mtl_slot_current(R350MtlCtx *g, unsigned sl, const R350GlReq *r,
                             unsigned un)
{
    const MtlTex *t = &g->tex[sl];

    return t->buf && t->w == r->tex_w[un] && t->h == r->tex_h[un] &&
           t->nl == MIN(mtl_levels_unit(r, un), MTL_LEVELS) &&
           t->cube == r->cube[un];
}

/*
 * Every unit the request samples, uploaded into the slot it names, once
 * when several units share one. False when the request cannot be bound:
 * two units wanting the scratch, one slot named for two textures, or a
 * slot the caller called current that is not.
 */
static bool mtl_bind_units(R350MtlCtx *g, const R350GlReq *r, uint32_t units)
{
    uint32_t done = 0;
    unsigned un, k;

    for (un = 0; un < R350_GL_TEXUNITS; un++) {
        unsigned sl = r->tex_slot[un];

        if (!(units & (1u << un))) {
            continue;
        }
        if (sl > R350_GL_TEXSLOTS) {
            return false;
        }
        for (k = 0; k < un; k++) {
            if ((units & (1u << k)) && r->tex_slot[k] == sl &&
                (sl == R350_GL_TEXSLOTS || r->tex[k] != r->tex[un])) {
                return false;
            }
        }
        if (r->tex_fresh[un] && r->tex[un]) {
            if (done & (1u << un)) {
                continue;
            }
            for (k = un + 1; k < R350_GL_TEXUNITS; k++) {
                if ((units & (1u << k)) && r->tex_slot[k] == sl) {
                    done |= 1u << k;
                }
            }
            if (!mtl_upload(g, sl, r, un)) {
                return false;
            }
        } else if (!mtl_slot_current(g, sl, r, un)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* the draw */

static void mtl_fsu_unit(MtlFsU *u, const R350GlReq *r, unsigned un,
                         const MtlTex *t)
{
    unsigned k;

    u->txsize[un * 2] = r->tex_w[un];
    u->txsize[un * 2 + 1] = r->tex_h[un];
    u->txclamp[un * 2] = r->clamp_s[un];
    u->txclamp[un * 2 + 1] = r->clamp_t[un];
    for (k = 0; k < 11; k++) {
        u->txtf[un * 16 + k] = r->filt[un][k];
    }
    for (k = 0; k < 4; k++) {
        u->txtf[un * 16 + 11 + k] = r->border[un][k];
    }
    u->txtf[un * 16 + 15] = r->cube[un];
    if (t) {
        for (k = 0; k < MTL_LEVELS; k++) {
            u->txoff[un * MTL_LEVELS + k] = t->off[k];
        }
        u->txlen[un] = t->len;
    } else {
        /* white: one texel, every coordinate on it */
        u->txsize[un * 2] = u->txsize[un * 2 + 1] = 1;
        u->txlen[un] = 1;
    }
}

bool ati_r350_gl_draw(R350MtlCtx *g, const R350GlReq *r)
{
    id<MTLRenderPipelineState> pso;
    uint32_t units;
    int sx0, sy0, sx1, sy1;
    unsigned un;
    bool ok = true;

    if (!g || !g->cbuf || r->w <= 0 || r->h <= 0 || !r->nvert ||
        r->surf_w > g->fb_w || r->surf_h > g->fb_h || g->gpu_failed) {
        return false;
    }
    /* the simple form samples unit 0 only, and only from a real slot */
    units = r->tcx ? r->textured
            : (r->textured & 1) && r->tex_slot[0] <= R350_GL_TEXSLOTS;
    if (r->zq && !r->zq_out &&
        g->zq_issued - g->zq_done >= MTL_ZQ_SLOTS) {
        return false;
    }
    @autoreleasepool {
        id<MTLRenderCommandEncoder> e;
        id<MTLBuffer> vb, tb = g->dummy;
        size_t voff, toff = 0;
        MtlFsU u;

        pso = mtl_prog_for(g, r);
        if (!pso || !mtl_bind_units(g, r, units)) {
            return false;
        }
        memset(&u, 0, sizeof(u));
        u.konst[0] = r->k_r;
        u.konst[1] = r->k_g;
        u.konst[2] = r->k_b;
        u.konst[3] = r->k_a;
        u.afref = r->af_ref;
        u.zscale = r->z.mode == 2 ? 65535.0f : 16777215.0f;
        u.alphatest = r->alpha_test;
        u.affunc = r->af_func;
        u.discard = r->discard;
        u.blend = r->blend;
        u.blendread = r->blend_read;
        u.zonly = r->zonly;
        u.dkeep = r->dkeep;
        u.textured = r->tcx ? (int32_t)r->textured : (r->textured & 1);
        u.cfac[0] = r->src_factor;
        u.cfac[1] = r->dst_factor;
        u.cfac[2] = r->comb_fcn;
        u.afac[0] = r->a_src_factor;
        u.afac[1] = r->a_dst_factor;
        u.afac[2] = r->a_comb_fcn;
        u.wmask = r->wmask;
        for (un = 0; un < R350_GL_TEXUNITS; un++) {
            mtl_fsu_unit(&u, r, un, (units & (1u << un))
                         ? &g->tex[r->tex_slot[un]] : NULL);
        }
        if (r->tcx) {
            size_t len = sizeof(float) * 4 * r->tc_stride * (r->nvert / 3);

            u.tcstride = (int32_t)r->tc_stride;
            u.tcraw = (int32_t)r->tc_raw;
            u.lodany = r->lod_any;
            for (un = 0; un < R350_GL_TCSETS; un++) {
                u.tcinv[un * 2] = r->tcinv[un][0];
                u.tcinv[un * 2 + 1] = r->tcinv[un][1];
            }
            tb = mtl_alloc(g, len, &toff);
            memcpy((uint8_t *)tb.contents + toff, r->tcx, len);
        }
        vb = mtl_alloc(g, sizeof(float) * R350_GL_VSTRIDE * r->nvert, &voff);
        memcpy((uint8_t *)vb.contents + voff, r->verts,
               sizeof(float) * R350_GL_VSTRIDE * r->nvert);

        sx0 = MAX(MAX(r->sx0, r->x0), 0);
        sy0 = MAX(MAX(r->sy0, r->y0), 0);
        sx1 = MIN(MIN(r->sx1, r->x0 + r->w), r->surf_w);
        sy1 = MIN(MIN(r->sy1, r->y0 + r->h), r->surf_h);

        if (sx1 > sx0 && sy1 > sy0) {
            id<MTLDepthStencilState> ds = mtl_dss(g, &r->z);
            int sref = r->z.mode == 1 ? (r->z.sref & 0xff) : 0;
            MTLScissorRect sc = { (NSUInteger)sx0, (NSUInteger)sy0,
                                  (NSUInteger)(sx1 - sx0),
                                  (NSUInteger)(sy1 - sy0) };
            float surf[2] = { (float)r->surf_w, (float)r->surf_h };

            e = mtl_enc(g);
            if (g->enc_pso != pso) {
                [e setRenderPipelineState:pso];
                g->enc_pso = pso;
            }
            if (g->enc_dss != ds) {
                [e setDepthStencilState:ds];
                g->enc_dss = ds;
            }
            if (g->enc_sref != sref) {
                [e setStencilReferenceValue:sref];
                g->enc_sref = sref;
            }
            if (g->enc_vw != r->surf_w || g->enc_vh != r->surf_h) {
                MTLViewport vp = { 0, 0, r->surf_w, r->surf_h, 0.0, 1.0 };

                [e setViewport:vp];
                g->enc_vw = r->surf_w;
                g->enc_vh = r->surf_h;
            }
            [e setScissorRect:sc];
            [e setVertexBuffer:vb offset:voff atIndex:0];
            [e setVertexBytes:surf length:sizeof(surf) atIndex:1];
            [e setFragmentBytes:&u length:sizeof(u) atIndex:0];
            if (r->us_konst) {
                [e setFragmentBytes:r->us_konst
                             length:sizeof(float) * 4 * R350_GL_USK
                            atIndex:1];
            } else {
                [e setFragmentBuffer:g->dummy offset:0 atIndex:1];
            }
            [e setFragmentBuffer:tb offset:toff atIndex:3];
            for (un = 0; un < R350_GL_TEXUNITS; un++) {
                MtlTex *t = (units & (1u << un))
                            ? &g->tex[r->tex_slot[un]] : NULL;
                id<MTLBuffer> b = t ? t->buf : g->white;

                if (t) {
                    t->use = g->ser_open;
                }
                if (g->enc_tx[un] != b) {
                    [e setFragmentBuffer:b offset:0 atIndex:4 + un];
                    g->enc_tx[un] = b;
                }
            }
            if (r->zq_out) {
                ((uint64_t *)g->vis.contents)[MTL_ZQ_SLOTS] = 0;
                [e setVisibilityResultMode:MTLVisibilityResultModeCounting
                                    offset:MTL_ZQ_SLOTS * 8];
            } else if (r->zq) {
                unsigned sl = g->zq_issued % MTL_ZQ_SLOTS;

                ((uint64_t *)g->vis.contents)[sl] = 0;
                g->zq_ser[sl] = g->ser_open;
                g->zq_issued++;
                g->cb_zq++;
                [e setVisibilityResultMode:MTLVisibilityResultModeCounting
                                    offset:sl * 8];
            }
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                  vertexCount:r->nvert];
            if (r->zq || r->zq_out) {
                [e setVisibilityResultMode:MTLVisibilityResultModeDisabled
                                    offset:0];
            }
            g->draws++;
            g->cb_draws++;
        } else if (r->zq_out) {
            ((uint64_t *)g->vis.contents)[MTL_ZQ_SLOTS] = 0;
        }

        if (r->out) {
            ok = mtl_read_rect(g, r->x0, r->y0, r->w, r->h, r->out);
        }
        if (ok && r->zout && r->z.mode) {
            ok = mtl_zread(g, r->x0, r->y0, r->w, r->h, r->zout, r->z.mode);
        }
        if (r->zq_out) {
            mtl_finish(g);
            *r->zq_out = ((uint64_t *)g->vis.contents)[MTL_ZQ_SLOTS];
        }
        if (g->cb_draws >= MTL_CB_DRAWS) {
            mtl_commit(g);
        }
    }
    return ok && !g->gpu_failed;
}
