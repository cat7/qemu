/*
 * ATI Radeon R350 -- R300 3D draw engine, as used by Mac OS X's
 * ATIRadeon9700 accelerator for its "2D" blits.
 *
 * The R300 family has no dedicated 2D blitter worth using: the OS X
 * driver (see init_r300_3d_blit_state_packet in ATIRadeon9700) paints
 * everything -- window surfaces, fills, cursor saves -- by streaming
 * R300 3D state packets and 3D_DRAW_IMMD_2 quad lists through the CP.
 * This file rasterizes those draws straight into VRAM.
 *
 * Scope, matched to what the driver actually submits (live-captured
 * corpus, 2026-08-24): QUADS/TRIANGLE lists with vertices embedded in
 * the command stream (PRIM_WALK=3), 12/8/3-dword vertex layouts,
 * one texture unit, nearest sampling of ARGB8888 textures with
 * unnormalized (pixel) coordinates, vertex-color modulation, and
 * src-alpha/inv-src-alpha blending. Everything else traces and skips.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "hw/pci/pci_device.h"
#include "ati_r350_int.h"
#include "ati_r350_regs.h"
#include "ati_r350_gl.h"
#include "trace.h"

typedef struct R300Vtx {
    float x, y, z, w;
    float r, g, b, a;
    float s, t;
} R300Vtx;

typedef struct R300DrawState {
    int sc_x0, sc_y0, sc_x1, sc_y1;   /* inclusive scissor window */
    uint32_t clip_rule;               /* RE_CLIPRECT_CNTL truth table */
    int cr[4][4];                     /* cliprects: x0,y0,x1,y1 (BR excl) */
    bool xform;             /* run positions through PVS matrix + viewport */
    float mat[16];          /* row-major position matrix (PVS consts 0-3) */
    R300PvsProgram vs;      /* the vertex program in force, if any */
    bool vs_run;            /* consume it: it is uploaded and not bypassed */
    bool vs_color;          /* take the per-vertex colour from its output */
    unsigned vs_color_out;  /* which output register that colour is */
    bool vs_texcoord;       /* take the texture coordinate from its output */
    unsigned vs_tex_out;    /* which output register that coordinate is */
    unsigned attr_size[R300_AOS_MAX];  /* dwords per vertex, per input reg */
    unsigned attr_count;
    float vp[6];            /* SE_VPORT XSCALE,XOFF,YSCALE,YOFF,ZSCALE,ZOFF */
    bool vte_xs, vte_xo;    /* VAP_VTE_CNTL: apply that scale/offset at all */
    bool vte_ys, vte_yo;
    uint32_t dst_off;       /* VRAM byte offset of the colour buffer */
    uint32_t dst_pitch;     /* bytes per scanline */
    uint32_t wmask;         /* RB3D_COLOR_CHANNEL_MASK as an ARGB byte mask */
    bool resolve;           /* colour buffer in AA-resolve mode */
    uint32_t res_off;       /* the buffer being resolved FROM */
    uint32_t res_pitch;
    bool textured;
    bool blend;
    bool blend_read;                   /* READ_ENABLE: may we read dst? */
    unsigned discard;                  /* DISCARD_SRC_PIXELS selector */
    unsigned src_factor, dst_factor;   /* RB3D_BLENDCNTL 6-bit codes */
    unsigned comb_fcn;                 /* colour combine function */
    unsigned a_src_factor, a_dst_factor;   /* RB3D_ABLENDCNTL, when
                                            * SEPARATE_ALPHA is set */
    unsigned a_comb_fcn;
    float k_r, k_g, k_b, k_a;          /* RB3D_BLEND_COLOR constant */
    bool alpha_test;
    unsigned af_func;                  /* FG_ALPHA_FUNC compare 0-7 */
    float af_ref;
    uint32_t tex_off;       /* card address of texture level 0 */
    int tex_w, tex_h;
    uint32_t tex_pitch;     /* bytes per texel row */
    unsigned tex_bpp;       /* bits per texel: 8, 16, 32 or 64 */
    unsigned tex_code;      /* TX_FORMAT1 TXFORMAT, to tell 16bpp/64bpp apart */
    int tex_attr;           /* vertex attribute holding s,t; -1 = none */
    unsigned tex_sel[4];    /* TX_FORMAT1 component select, A R G B */
    unsigned clamp_s, clamp_t; /* TX_FILTER0 clamp modes (0 = repeat) */
    float flat_r, flat_g, flat_b, flat_a;
    uint8_t *vram;
} R300DrawState;

static inline float r300_f32(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

/*
 * Every format this model decodes is handed to r300_texel_chan() in one
 * shape: the four components as bytes, X in the low lane and W in the
 * high one, exactly as TX_FMT_8_8_8_8 already arrives. Narrower or wider
 * components are widened or reduced to eight bits here, so the
 * TX_FORMAT1 component select stays one piece of code for every format
 * rather than growing a per-format extraction rule.
 */
static inline uint32_t r300_pack_xyzw(uint32_t x, uint32_t y,
                                      uint32_t z, uint32_t w)
{
    return (x & 0xff) | ((y & 0xff) << 8) |
           ((z & 0xff) << 16) | ((w & 0xff) << 24);
}

/* 5-bit component to 8 bits, replicating the high bits so 31 maps to 255 */
static inline uint32_t r300_c5to8(uint32_t c)
{
    return (c << 3) | (c >> 2);
}

/*
 * TX_FMT_1_5_5_5 (TXFORMAT code 0xb, 16 bits per texel). Components are
 * numbered right to left, so Component0 is bits [4:0], Component1
 * [9:5], Component2 [14:10] and Component3 the single bit 15 -- which
 * under the usual (W,Z,Y,X) select is plain ARGB1555.
 */
static inline uint32_t r300_texel_1555(uint32_t v)
{
    return r300_pack_xyzw(r300_c5to8(v & 0x1f),
                          r300_c5to8((v >> 5) & 0x1f),
                          r300_c5to8((v >> 10) & 0x1f),
                          (v >> 15) & 1 ? 0xff : 0);
}

/*
 * TX_FMT_16_16_16_16 (code 0xe, 64 bits per texel), from the two dwords
 * in ascending address order: Component0 is the low half of the first,
 * Component3 the high half of the second. The pipeline below carries
 * eight bits per channel, so each component keeps its high byte.
 */
static inline uint32_t r300_texel_16x4(uint32_t lo, uint32_t hi)
{
    return r300_pack_xyzw(lo >> 8, lo >> 24, hi >> 8, hi >> 24);
}

static uint32_t r300_sample_tex(ATIR350State *s, const R300DrawState *d,
                                int tx, int ty)
{
    uint32_t addr, off;

    /*
     * TX_FILTER0 clamp modes: 0 is wrap/repeat -- OS X paints its
     * title-bar gradient by drawing a 16x20 tile as a window-wide
     * quad and letting the sampler repeat it; clamping instead
     * smeared whatever sat next to the tile in VRAM. Treat mirror
     * modes as repeat, everything else clamps to the edge.
     */
    if (d->clamp_s <= 1 && d->tex_w > 0) {
        tx %= d->tex_w;
        if (tx < 0) {
            tx += d->tex_w;
        }
    } else {
        tx = MIN(MAX(tx, 0), d->tex_w - 1);
    }
    if (d->clamp_t <= 1 && d->tex_h > 0) {
        ty %= d->tex_h;
        if (ty < 0) {
            ty += d->tex_h;
        }
    } else {
        ty = MIN(MAX(ty, 0), d->tex_h - 1);
    }
    addr = d->tex_off + (uint32_t)ty * d->tex_pitch +
           (uint32_t)tx * (d->tex_bpp / 8);
    if (d->tex_bpp == 8) {
        /* single-component format: the byte is component X */
        uint8_t a;

        if (ati_r350_mc_to_vram(s, addr, &off)) {
            if (off + 1 > ATI_R350_VRAM_SIZE) {
                return 0;
            }
            a = ((uint8_t *)memory_region_get_ram_ptr(&s->vram))
                [off ^ (ati_r350_vram_xor(s, off) & 3)];
        } else {
            a = (ati_r350_mc_read32(s, addr & ~3u) >> ((addr & 3) * 8))
                & 0xff;
        }
        return a;
    }
    if (d->tex_bpp == 16) {
        /*
         * One 16-bit unit per texel, assembled from its two bytes in
         * ascending address order. The byte lanes go through the same
         * aperture swapper as every other VRAM read -- a 32-bit swapper
         * reverses the lanes of a halfword just as it does for the 2D
         * engine's 16bpp path. TX_FMT_8_8 is then already in the packed
         * shape (X the low byte, Y the high one); TX_FMT_1_5_5_5 has to
         * have its components spread into their own lanes.
         */
        unsigned xr;
        uint32_t v;

        if (!ati_r350_mc_to_vram(s, addr, &off)) {
            v = (ati_r350_mc_read32(s, addr & ~3u) >>
                 ((addr & 2) * 8)) & 0xffff;
        } else if (off + 2 > ATI_R350_VRAM_SIZE) {
            return 0;
        } else {
            xr = ati_r350_vram_xor(s, off);
            v = (uint32_t)d->vram[off ^ xr] |
                ((uint32_t)d->vram[(off + 1) ^ xr] << 8);
        }
        return d->tex_code == R300_TX_FMT_1_5_5_5 ? r300_texel_1555(v) : v;
    }
    if (d->tex_bpp == 64) {
        /*
         * TX_FMT_16_16_16_16. The swapper is a byte-lane permutation
         * inside each dword, so the two halves of the texel are read
         * exactly as two independent dwords, in address order.
         */
        if (ati_r350_mc_to_vram(s, addr, &off)) {
            if (off + 8 > ATI_R350_VRAM_SIZE) {
                return 0;
            }
            return r300_texel_16x4(ati_r350_vram_ld32(s, off),
                                   ati_r350_vram_ld32(s, off + 4));
        }
        return r300_texel_16x4(ati_r350_mc_read32(s, addr),
                               ati_r350_mc_read32(s, addr + 4));
    }
    if (ati_r350_mc_to_vram(s, addr, &off)) {
        if (off + 4 > ATI_R350_VRAM_SIZE) {
            return 0;
        }
        return ati_r350_vram_ld32(s, off);
    }
    /* texture staged in GART/system memory */
    return ati_r350_mc_read32(s, addr);
}

/*
 * One output channel of the texture unit. `texel` holds the format's own
 * components packed from the least significant bits up -- X, Y, Z, W --
 * and TX_FORMAT1 says which of them (or a constant) this channel takes.
 * Reading a texel as ARGB regardless is right only for the selector
 * Mac OS X's tiles use; Chess.app's board texture selects the same four
 * bytes in the opposite order, which is a red/blue exchange.
 */
static float r300_texel_chan(const R300DrawState *d, uint32_t texel,
                             unsigned ch)
{
    unsigned sel = d->tex_sel[ch];

    if (sel == R300_TX_SEL_ONE) {
        return 1.0f;
    }
    if (sel > R300_TX_SEL_W) {
        return 0.0f;                    /* ZERO, and the CUT_* variants */
    }
    return ((texel >> (sel * 8)) & 0xff) / 255.0f;
}

/*
 * A VRAM dword through the aperture swapper, from the pointer the draw
 * already holds. ati_r350_vram_ld32() resolves the RAM pointer on every
 * call, which is real work to repeat for each of the two or three
 * fetches a single pixel can make.
 */
static inline uint32_t r300_ld32(ATIR350State *s, const R300DrawState *d,
                                 uint32_t addr)
{
    unsigned xr = ati_r350_vram_xor(s, addr);

    return (uint32_t)d->vram[addr ^ xr] |
           ((uint32_t)d->vram[(addr + 1) ^ xr] << 8) |
           ((uint32_t)d->vram[(addr + 2) ^ xr] << 16) |
           ((uint32_t)d->vram[(addr + 3) ^ xr] << 24);
}

/*
 * Both take the destination address the caller already computed for the
 * span, and neither marks the region dirty: that is done once per row by
 * r300_raster_tri(), over the range it actually wrote. Marking eight
 * bytes per pixel meant a dirty-bitmap update for every pixel of every
 * triangle, which for a full-screen blended quad is 786432 of them.
 */
static void r300_write_dst(ATIR350State *s, const R300DrawState *d,
                           uint32_t addr, uint32_t argb)
{
    unsigned xr;

    if (d->wmask != 0xffffffff) {
        /* masked-off channels keep whatever the destination holds */
        argb = (argb & d->wmask) | (r300_ld32(s, d, addr) & ~d->wmask);
    }
    xr = ati_r350_vram_xor(s, addr);
    d->vram[(addr + 0) ^ xr] = argb & 0xff;
    d->vram[(addr + 1) ^ xr] = (argb >> 8) & 0xff;
    d->vram[(addr + 2) ^ xr] = (argb >> 16) & 0xff;
    d->vram[(addr + 3) ^ xr] = (argb >> 24) & 0xff;
}

static uint32_t r300_read_dst(ATIR350State *s, const R300DrawState *d,
                              uint32_t addr)
{
    return r300_ld32(s, d, addr);
}

/* the factor codes r300_blend_f() below actually implements */
static bool r300_blend_known(unsigned code)
{
    return (code >= 1 && code <= 11) || (code >= 32 && code <= 46);
}

/*
 * One blend factor for one channel. `sc`/`dc` are the source and
 * destination values of the channel being blended, `sa`/`da` the
 * alphas, `kc`/`ka` the RB3D_BLEND_COLOR constant for this channel.
 * Codes 32+ are the GL names, 1-11 the D3D aliases.
 */
static float r300_blend_f(unsigned code, float sc, float sa,
                          float dc, float da, float kc, float ka)
{
    switch (code) {
    case 1: case 32: return 0.0f;                    /* ZERO */
    case 2: case 33: return 1.0f;                    /* ONE */
    case 3: case 34: return sc;                      /* SRC_COLOR */
    case 4: case 35: return 1.0f - sc;
    case 9: case 36: return dc;                      /* DST_COLOR */
    case 10: case 37: return 1.0f - dc;
    case 5: case 38: return sa;                      /* SRC_ALPHA */
    case 6: case 39: return 1.0f - sa;
    case 7: case 40: return da;                      /* DST_ALPHA */
    case 8: case 41: return 1.0f - da;
    case 11: case 42: return MIN(sa, 1.0f - da);     /* SRC_ALPHA_SATURATE */
    case 43: return kc;                              /* CONST_COLOR */
    case 44: return 1.0f - kc;
    case 45: return ka;                              /* CONST_ALPHA */
    case 46: return 1.0f - ka;
    default: return 1.0f;
    }
}

/*
 * How the weighted source and destination terms are combined. The
 * no-clamp variants differ only in the intermediate, and the caller
 * clamps on the way to the framebuffer either way.
 */
static float r300_blend_comb(unsigned fcn, float s, float d)
{
    switch (fcn) {
    case 2: case 3: return s - d;      /* SUBTRACT */
    case 4: return MIN(s, d);
    case 5: return MAX(s, d);
    case 6: case 7: return d - s;      /* REVERSE_SUBTRACT */
    default: return s + d;             /* ADD */
    }
}

static inline float r300_edge(const R300Vtx *a, const R300Vtx *b,
                              float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * The x range of one row that can possibly satisfy `w >= lim`, for a
 * barycentric weight that varies linearly across the row as w(x) = a*x
 * + k. Widened by a pixel on each side and left deliberately loose: the
 * exact acceptance test still runs per pixel inside the range, so this
 * only decides how much empty space the loop skips, never which pixels
 * are painted.
 */
static void r300_span_clip(float a, float k, float lim, int *lo, int *hi)
{
    float cut;

    if (a == 0.0f) {
        if (k < lim) {
            *lo = 1;                /* the whole row fails; make it empty */
            *hi = 0;
        }
        return;
    }
    cut = (lim - k) / a;
    if (!isfinite(cut)) {
        return;                     /* learn nothing rather than guess */
    }
    if (a > 0.0f) {
        cut = floorf(cut) - 1.0f;
        if (cut > (float)*lo) {
            *lo = cut > 8191.0f ? 8191 : (int)cut;
        }
    } else {
        cut = ceilf(cut) + 1.0f;
        if (cut < (float)*hi) {
            *hi = cut < -8191.0f ? -8191 : (int)cut;
        }
    }
}

/*
 * The fill convention, as a predicate on one edge.
 *
 * A quad is two triangles sharing a diagonal, and a pixel that lands
 * exactly on that diagonal must be shaded by exactly one of them --
 * shade it twice and a blended draw blends it twice, which is a seam;
 * shade it neither and the quad has a crack down the middle. The rule
 * every rasterizer uses is top-left: of the two triangles the shared
 * edge belongs to the one for which it is a top or a left edge, and
 * they cannot both say yes because they traverse it in opposite
 * directions.
 *
 * `dx`/`dy` are the edge's direction, `flip` is set when the triangle's
 * signed area is negative so that the direction is expressed in the
 * winding the acceptance test is written for (interior on the left, y
 * increasing downwards). In that frame an edge is LEFT when it points
 * up the screen and TOP when it is horizontal and points right.
 */
static inline bool r300_top_left(float dx, float dy, bool flip)
{
    if (flip) {
        dx = -dx;
        dy = -dy;
    }
    return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

static inline bool r300_edge_accept(float w, bool top_left)
{
    return w > 0.0f || (w == 0.0f && top_left);
}

static void r300_raster_tri(ATIR350State *s, const R300DrawState *d,
                            const R300Vtx *v0, const R300Vtx *v1,
                            const R300Vtx *v2)
{
    float area = r300_edge(v0, v1, v2->x, v2->y);
    float inv, dx0, dy0, dx1, dy1, dx2, dy2;
    float a0, b0, c0, a1, b1, c1;
    bool flip, tl0, tl1, tl2;
    int x0, y0, x1, y1, x, y;

    if (area == 0.0f) {
        return;
    }
    /*
     * Two things come out of the triangle once instead of per pixel.
     *
     * The division: r300_edge() divided by the area is two floating-
     * point divisions for every pixel of every span, and the reciprocal
     * does the same job. The edge expression itself is kept exactly as
     * it was -- it subtracts coordinates before multiplying them, which
     * is what keeps it accurate for the far-apart vertices a
     * screen-filling triangle has. (Folding it into a*px + b*py + c
     * looks tidier and is measurably worse: an A/B over 120000 random
     * triangles put 81759 pixels up to 4/255 out, against 15864 pixels
     * at most 1/255 for the form below.)
     *
     * The coefficients: the same weights written as a*px + b*py + c,
     * used only to solve each acceptance test for x and give the row a
     * span. Precision does not matter there because the result is
     * widened by a pixel and every pixel inside it still faces the
     * exact test.
     */
    inv = 1.0f / area;
    dx0 = v2->x - v1->x;
    dy0 = v2->y - v1->y;
    dx1 = v0->x - v2->x;
    dy1 = v0->y - v2->y;
    dx2 = v1->x - v0->x;
    dy2 = v1->y - v0->y;
    flip = area < 0.0f;
    tl0 = r300_top_left(dx0, dy0, flip);
    tl1 = r300_top_left(dx1, dy1, flip);
    tl2 = r300_top_left(dx2, dy2, flip);
    a0 = -dy0 * inv;
    b0 = dx0 * inv;
    c0 = (dy0 * v1->x - dx0 * v1->y) * inv;
    a1 = -dy1 * inv;
    b1 = dx1 * inv;
    c1 = (dy1 * v2->x - dx1 * v2->y) * inv;

    x0 = (int)floorf(MIN(v0->x, MIN(v1->x, v2->x)));
    y0 = (int)floorf(MIN(v0->y, MIN(v1->y, v2->y)));
    x1 = (int)ceilf(MAX(v0->x, MAX(v1->x, v2->x)));
    y1 = (int)ceilf(MAX(v0->y, MAX(v1->y, v2->y)));
    x0 = MAX(x0, MAX(d->sc_x0, 0));
    y0 = MAX(y0, MAX(d->sc_y0, 0));
    /* scissor right/bottom are inclusive; the VRAM bound in the pixel
     * helpers is the real limit beyond that */
    x1 = MIN(x1, MIN(d->sc_x1 + 1, 8191));
    y1 = MIN(y1, MIN(d->sc_y1 + 1, 8191));

    for (y = y0; y < y1; y++) {
        float py = y + 0.5f;
        float ry0 = py - v1->y, ry1 = py - v2->y, ry2 = py - v0->y;
        /* w0 and w1 along this row, as w = a*x + k */
        float k0 = b0 * py + c0 + 0.5f * a0;
        float k1 = b1 * py + c1 + 0.5f * a1;
        uint32_t row = d->dst_off + (uint32_t)y * d->dst_pitch;
        int sx0 = x0, sx1 = x1 - 1;
        uint32_t dirty_lo = 0, dirty_hi = 0;
        bool dirty = false;

        /*
         * The three acceptance tests are three half-planes; on this row
         * each is an interval of x. Intersecting them first is what
         * stops a long thin triangle from being scanned across the full
         * width of its bounding box, which for the screen-filling
         * geometry a screensaver draws is most of the work.
         */
        r300_span_clip(a0, k0, 0.0f, &sx0, &sx1);
        r300_span_clip(a1, k1, 0.0f, &sx0, &sx1);
        /*
         * The third bound stays at the old -0.001 slack even though the
         * acceptance test no longer has any: a looser bound is a
         * superset of the accepted range, which is all a loop bound has
         * to be, and tightening it would only re-derive a limit the
         * exact test applies anyway.
         */
        r300_span_clip(-(a0 + a1), -(k0 + k1), -1.001f, &sx0, &sx1);

        for (x = sx0; x <= sx1; x++) {
            float px = x + 0.5f;
            float w0 = (dx0 * ry0 - dy0 * (px - v1->x)) * inv;
            float w1 = (dx1 * ry1 - dy1 * (px - v2->x)) * inv;
            float w2e = (dx2 * ry2 - dy2 * (px - v0->x)) * inv;
            float w2 = 1.0f - w0 - w1;
            float cr, cg, cb, ca;
            uint32_t addr;
            uint32_t out;

            /*
             * The third weight is tested from its own edge expression
             * and interpolated from 1 - w0 - w1. They agree in exact
             * arithmetic, but only the edge form produces the exact
             * zero a tie is made of: the subtraction's cancellation
             * leaves a rounding residue instead, which is a tie the
             * fill rule can no longer see. Interpolation keeps the
             * subtraction so that every accepted pixel shades exactly
             * as it did before -- this changes WHICH pixels are
             * accepted, and nothing about what they come out as.
             */
            if (!r300_edge_accept(w0, tl0) ||
                !r300_edge_accept(w1, tl1) ||
                !r300_edge_accept(w2e, tl2)) {
                continue;
            }
            if (d->clip_rule != 0xffff) {
                unsigned idx = 0, r;

                for (r = 0; r < 4; r++) {
                    if (x >= d->cr[r][0] && x < d->cr[r][2] &&
                        y >= d->cr[r][1] && y < d->cr[r][3]) {
                        idx |= 1u << r;
                    }
                }
                if (!((d->clip_rule >> idx) & 1)) {
                    continue;
                }
            }
            addr = row + (uint32_t)x * 4;
            if (addr + 4 > ATI_R350_VRAM_SIZE) {
                continue;
            }
            if (!dirty) {
                dirty_lo = dirty_hi = addr;
                dirty = true;
            } else {
                dirty_hi = addr;
            }
            if (d->resolve) {
                /*
                 * In resolve mode the fragment the shader produced is
                 * not what lands: the colour buffer's own samples for
                 * this pixel are filtered and written to the resolve
                 * buffer. We rasterize one sample per pixel, so that
                 * filter degenerates to a copy -- but a copy is still
                 * the whole point of the pass, and writing the shaded
                 * fragment instead destroys the source.
                 */
                uint32_t src = d->res_off + (uint32_t)y * d->res_pitch +
                               (uint32_t)x * 4;

                if (src + 4 <= ATI_R350_VRAM_SIZE) {
                    r300_write_dst(s, d, addr, r300_ld32(s, d, src));
                }
                continue;
            }
            cr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
            cg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
            cb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
            ca = w0 * v0->a + w1 * v1->a + w2 * v2->a;
            if (d->textured) {
                float ts = w0 * v0->s + w1 * v1->s + w2 * v2->s;
                float tt = w0 * v0->t + w1 * v1->t + w2 * v2->t;
                uint32_t texel = r300_sample_tex(s, d, (int)ts, (int)tt);

                ca *= r300_texel_chan(d, texel, 0);
                cr *= r300_texel_chan(d, texel, 1);
                cg *= r300_texel_chan(d, texel, 2);
                cb *= r300_texel_chan(d, texel, 3);
            }
            if (d->alpha_test) {
                bool pass;

                switch (d->af_func) {
                case 0: pass = false; break;                /* NEVER */
                case 1: pass = ca < d->af_ref; break;
                case 2: pass = ca == d->af_ref; break;
                case 3: pass = ca <= d->af_ref; break;
                case 4: pass = ca > d->af_ref; break;       /* GREATER */
                case 5: pass = ca != d->af_ref; break;
                case 6: pass = ca >= d->af_ref; break;
                default: pass = true; break;                /* ALWAYS */
                }
                if (!pass) {
                    continue;
                }
            }
            if (d->discard) {
                /*
                 * DISCARD_SRC_PIXELS: kill the fragment outright for
                 * source values that could not change the destination
                 * under the configured blend, before it costs a read.
                 */
                bool a0 = ca == 0.0f, a1 = ca == 1.0f;
                bool c0 = cr == 0.0f && cg == 0.0f && cb == 0.0f;
                bool c1 = cr == 1.0f && cg == 1.0f && cb == 1.0f;
                bool kill;

                switch (d->discard) {
                case 1:
                    kill = a0;
                    break;
                case 2:
                    kill = c0;
                    break;
                case 3:
                    kill = a0 && c0;
                    break;
                case 4:
                    kill = a1;
                    break;
                case 5:
                    kill = c1;
                    break;
                case 6:
                    kill = a1 && c1;
                    break;
                default:
                    kill = false;
                    break;
                }
                if (kill) {
                    continue;
                }
            }
            if (d->blend) {
                /*
                 * READ_ENABLE clear means the blender does not fetch
                 * the destination at all; the destination terms then
                 * see zero rather than whatever is in memory.
                 */
                uint32_t dst = d->blend_read ? r300_read_dst(s, d, addr) : 0;
                float dr = ((dst >> 16) & 0xff) / 255.0f;
                float dg = ((dst >> 8) & 0xff) / 255.0f;
                float db = (dst & 0xff) / 255.0f;
                float da = ((dst >> 24) & 0xff) / 255.0f;
                float nr, ng, nb;

                nr = r300_blend_comb(d->comb_fcn,
                        cr * r300_blend_f(d->src_factor, cr, ca, dr, da,
                                          d->k_r, d->k_a),
                        dr * r300_blend_f(d->dst_factor, cr, ca, dr, da,
                                          d->k_r, d->k_a));
                ng = r300_blend_comb(d->comb_fcn,
                        cg * r300_blend_f(d->src_factor, cg, ca, dg, da,
                                          d->k_g, d->k_a),
                        dg * r300_blend_f(d->dst_factor, cg, ca, dg, da,
                                          d->k_g, d->k_a));
                nb = r300_blend_comb(d->comb_fcn,
                        cb * r300_blend_f(d->src_factor, cb, ca, db, da,
                                          d->k_b, d->k_a),
                        db * r300_blend_f(d->dst_factor, cb, ca, db, da,
                                          d->k_b, d->k_a));
                ca = r300_blend_comb(d->a_comb_fcn,
                        ca * r300_blend_f(d->a_src_factor, ca, ca, da, da,
                                          d->k_a, d->k_a),
                        da * r300_blend_f(d->a_dst_factor, ca, ca, da, da,
                                          d->k_a, d->k_a));
                cr = nr;
                cg = ng;
                cb = nb;
            }
            out = ((uint32_t)(MIN(MAX(ca, 0.0f), 1.0f) * 255.0f) << 24) |
                  ((uint32_t)(MIN(MAX(cr, 0.0f), 1.0f) * 255.0f) << 16) |
                  ((uint32_t)(MIN(MAX(cg, 0.0f), 1.0f) * 255.0f) << 8) |
                  (uint32_t)(MIN(MAX(cb, 0.0f), 1.0f) * 255.0f);
            r300_write_dst(s, d, addr, out);
        }
        if (dirty) {
            /*
             * One dirty update for the row's whole written extent. The
             * range can cover a few pixels the span skipped after
             * marking them -- an alpha test or a discard rule can still
             * reject one -- which costs a redraw of pixels that did not
             * change and never the other way round.
             */
            uint64_t lo = dirty_lo & ~7ull;
            uint64_t hi = (dirty_hi + 4 + 7) & ~7ull;

            memory_region_set_dirty(&s->vram, lo, hi - lo);
        }
    }
}

/*
 * A vertex the transform cannot place: far enough outside any render
 * target that the scissor drops it, but small enough to stay an ordinary
 * float and an in-range int once floored.
 */
static const float r300_vtx_nowhere = -32768.0f;

/*
 * Position, through the vertex program's matrix and then the viewport.
 *
 * Every program the driver and its applications upload computes the
 * clip-space position the same way -- four dot products of the incoming
 * position against constants 0-3 (confirmed across all seven programs in
 * the Chess corpus) -- so the matrix stands in for the program for that
 * one output. What comes out is CLIP space, and clip space only becomes
 * normalized device space after dividing by w.
 *
 * The compositor never needed the divide: its projection is orthographic,
 * so w is 1 and dividing changes nothing, which is why the desktop always
 * looked right. A perspective projection is what exposes it -- Chess's
 * board arrived scaled by whatever its w happened to be, landing the
 * geometry tens of thousands of pixels outside the render target and
 * filling the window with streaks.
 */
static void r300_xform_vtx(const R300DrawState *d, R300Vtx *v,
                           const float *clip)
{
    float cx, cy, cw;

    if (!d->xform) {
        return;
    }
    if (clip) {
        /* the vertex program computed this position itself */
        cx = clip[0];
        cy = clip[1];
        cw = clip[3];
    } else {
        cx = d->mat[0] * v->x + d->mat[1] * v->y +
             d->mat[2] * v->z + d->mat[3] * v->w;
        cy = d->mat[4] * v->x + d->mat[5] * v->y +
             d->mat[6] * v->z + d->mat[7] * v->w;
        cw = d->mat[12] * v->x + d->mat[13] * v->y +
             d->mat[14] * v->z + d->mat[15] * v->w;
    }
    /*
     * Nothing here clips against the w = 0 plane, so a vertex level with
     * or behind the eye has no screen position to compute. Refuse the
     * division rather than let an infinity or a NaN reach the rasterizer:
     * a NaN compares false against every bound, so it survives the
     * scissor and floors into an INT_MIN rectangle that smears across the
     * whole surface. Park the vertex off-screen instead -- the same
     * treatment the raw, untransformable coordinates need, since those
     * are unbounded floats that floor into nonsense of their own.
     */
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cw) ||
        fabsf(cw) < 0.000001f) {
        v->x = v->y = r300_vtx_nowhere;
        return;
    }
    /*
     * The viewport's scale and offset are enabled per component, and a
     * guest that is already handing over screen coordinates turns the
     * offset off rather than writing zero into it. iTunes Artwork's
     * per-frame erase is one such draw: a full-surface point sprite at
     * VTE 0x405 -- scales on at 1.0, offsets OFF -- which the model
     * displaced by the whole SE_VPORT offset, so it erased only the
     * bottom-right quadrant of the saver's surface and left the rest
     * showing whatever that VRAM held before.
     *
     * The both-enabled arm is written out as the one expression it has
     * always been rather than as a scale followed by an add: the
     * compiler contracts it into a fused multiply-add, and splitting
     * the two would round in between and move pixels in every draw
     * this model has ever got right.
     */
    if (d->vte_xs) {
        v->x = d->vte_xo ? (cx / cw) * d->vp[0] + d->vp[1]
                         : (cx / cw) * d->vp[0];
    } else {
        v->x = d->vte_xo ? cx / cw + d->vp[1] : cx / cw;
    }
    if (d->vte_ys) {
        v->y = d->vte_yo ? (cy / cw) * d->vp[2] + d->vp[3]
                         : (cy / cw) * d->vp[2];
    } else {
        v->y = d->vte_yo ? cy / cw + d->vp[3] : cy / cw;
    }
    if (!isfinite(v->x) || !isfinite(v->y)) {
        v->x = v->y = r300_vtx_nowhere;
    }
}

/*
 * `pos` is how many of the leading dwords belong to the position
 * attribute, which is not always the whole vertex: an AOS draw's first
 * array can be three dwords of model-space x,y,z with the next array
 * holding something else entirely. Taking w from the fourth dword
 * regardless then feeds a foreign attribute into the perspective
 * divide. Inline (IMMD) vertices have no array boundaries, so their
 * caller passes the whole vertex size and nothing changes for them.
 */
static void r300_load_vtx(const R300DrawState *d, const uint32_t *dw,
                          unsigned vsize, unsigned pos, R300Vtx *v)
{
    /* set by the caller for vertices that carry no colour of their own */
    v->x = r300_f32(dw[0]);
    v->y = pos >= 2 ? r300_f32(dw[1]) : 0.0f;
    v->z = pos >= 3 ? r300_f32(dw[2]) : 0.0f;
    v->w = pos >= 4 ? r300_f32(dw[3]) : 1.0f;
    v->r = d->flat_r;
    v->g = d->flat_g;
    v->b = d->flat_b;
    v->a = d->flat_a;
    v->s = v->t = 0.0f;
    /*
     * Everything below reads the vertex as one flat block whose first
     * four dwords are the position, which is only true when the
     * position attribute really is four dwords wide. Chess.app's board
     * vertex is a three-dword position followed by a normal, so dwords
     * four and five are two thirds of the normal and not a texture
     * coordinate; sampling with them smears the texture. Such a vertex
     * gets its coordinate from the vertex program instead.
     */
    if (pos < 4) {
        return;
    }
    if (vsize >= 12) {
        /* pos.xyzw | color.rgba | tex.stpq */
        v->r = r300_f32(dw[4]);
        v->g = r300_f32(dw[5]);
        v->b = r300_f32(dw[6]);
        v->a = r300_f32(dw[7]);
        v->s = r300_f32(dw[8]);
        v->t = r300_f32(dw[9]);
    } else if (vsize >= 8) {
        /*
         * pos.xyzw + texcoords. Live captures show the 8-dword layout
         * is always position + texture coordinates -- the untextured
         * users (window shadows via DRAW_VBUF_2) just ignore them and
         * take the fragment constant colour like every colourless
         * vertex. Reading the second attribute as a colour fed
         * texcoords into the blender as RGBA.
         */
        v->s = r300_f32(dw[4]);
        v->t = r300_f32(dw[5]);
    }
}

/*
 * One of the vertex program's input registers, read out of this vertex.
 *
 * There is no VAP_INPUT_ROUTE decoding here and none is wanted: for an
 * array (VBUF) draw the bound arrays are the layout -- array 0 lands in
 * in[0], array 1 in in[1], and so on -- and an inline (IMMD) vertex has no
 * array boundaries at all, so its dwords fill the input registers four at
 * a time. Components a vertex does not supply keep the (0,0,0,1) default,
 * which is what lets a three-dword model-space position meet a 4x4 matrix
 * and still pick up its translation column.
 */
static void r300_vs_input(const R300DrawState *d, const uint32_t *dw,
                          unsigned idx, float out[4])
{
    unsigned off = 0, c;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (idx >= d->attr_count) {
        return;
    }
    for (c = 0; c < idx; c++) {
        off += d->attr_size[c];
    }
    for (c = 0; c < d->attr_size[idx] && c < 4; c++) {
        out[c] = r300_f32(dw[off + c]);
    }
}

/*
 * A colour the program only forwards from an attribute is a colour only
 * if the vertex actually carries that attribute. Missing ones read as the
 * (0,0,0,1) default, and painting with it turns a draw opaque black --
 * which is exactly what a three-dword point sprite compositing a window
 * would become, since its one attribute is the position.
 */
static bool r300_vs_has_color(const R300DrawState *d)
{
    int src = d->vs.out_src[d->vs_color_out];

    return d->vs_color && (src < 0 || (unsigned)src < d->attr_count);
}

/*
 * The texture coordinate the program computed, in the texels this
 * model's sampler works in.
 *
 * The hardware samples with normalised coordinates, so every program
 * that emits one divides by the bound texture's size on the way out --
 * which is why taking the raw attribute instead has worked so far:
 * Mac OS X's compositor hands the vertex a coordinate in texels and its
 * program's texture matrix is exactly diag(1/w, 1/h, 1, 1), so the two
 * paths agree by construction (1887 of 1887 draws across the captures).
 * Chess.app's board has no coordinate attribute at all -- its program
 * generates one -- so there the attribute path samples texel (0,0) for
 * every pixel and the board loses its texture entirely.
 */
static void r300_vs_texcoord(const R300DrawState *d, R300Vtx *v,
                             const float c[4])
{
    float s = c[0], t = c[1], q = c[3];

    if (!isfinite(s) || !isfinite(t)) {
        return;
    }
    if (isfinite(q) && q != 0.0f && q != 1.0f) {
        s /= q;
        t /= q;
    }
    v->s = s * d->tex_w;
    v->t = t * d->tex_h;
}

/*
 * The texture coordinate a vertex really carries.
 *
 * r300_load_vtx() reads one at a fixed place -- the dwords after a
 * four-dword position -- and takes it as it finds it, in TEXELS.
 * `tex_attr` is the better answer wherever the vertex program names
 * the attribute the coordinate lives in: an output the program only
 * FORWARDS is that attribute's own value, so it can be read straight
 * from the vertex without running the interpreter, and it arrives
 * NORMALISED, exactly as it would out of the program -- which is why
 * it goes through r300_vs_texcoord(), the same scaling by the bound
 * texture's size that the interpreter's own coordinates get.
 *
 * The positional reading stays for the draws whose program COMPUTES a
 * coordinate instead of forwarding one (out_src = -1, so tex_attr
 * stays -1): Mac OS X's compositor multiplies by a texture matrix that
 * is exactly diag(1/w, 1/h, 1, 1), which makes the attribute it
 * transforms already a texel count. Those are 881 of the 881 textured
 * draws in the desktop drag capture, and none of them reaches here.
 *
 * The savers built on Core Image are what this buys. Beach, Cosmos,
 * Forest, Nature Patterns, Paper Shadow and Abstract all paint their
 * image as a stack of full-width bands, one 512x8 or 1024x8 strip
 * texture each, with s running 0..1 across the band and t a single
 * eighth-step -- normalised. Read as texels that is texel (0,0) for
 * every pixel of a band, so each band came out one flat colour and the
 * picture became horizontal stripes.
 */
static void r300_attr_texcoord(const R300DrawState *d, const uint32_t *dw,
                               R300Vtx *v)
{
    float c[4];

    if (d->tex_attr < 0 || (unsigned)d->tex_attr >= d->attr_count ||
        d->attr_size[d->tex_attr] < 2) {
        return;
    }
    r300_vs_input(d, dw, (unsigned)d->tex_attr, c);
    r300_vs_texcoord(d, v, c);
}

/*
 * What a draw's first vertex says about its texture coordinate, in both
 * of the units it could be in: the raw attribute the vertex program
 * names (s, t and the projective q) against the coordinate this model
 * ended up sampling with. A guest that hands over NORMALISED
 * coordinates prints a raw s of about 1.0 and a sampled s of about 1.0
 * too -- one texel of a whole texture -- while a guest whose attribute
 * is already in texels prints the two the same and large.
 */
static void r300_trace_texcoord(const R300DrawState *d, const uint32_t *dw,
                                const R300Vtx *v)
{
    float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!trace_event_get_state_backends(TRACE_ATI_R350_3D_TEXCOORD)) {
        return;
    }
    if (d->tex_attr >= 0 && (unsigned)d->tex_attr < d->attr_count) {
        r300_vs_input(d, dw, (unsigned)d->tex_attr, c);
    }
    trace_ati_r350_3d_texcoord(d->tex_attr, d->tex_w, d->tex_h,
                               (int32_t)(c[0] * 1000), (int32_t)(c[1] * 1000),
                               (int32_t)(c[3] * 1000),
                               (int32_t)(v->s * 1000), (int32_t)(v->t * 1000));
}

static void r300_vs_color(R300Vtx *v, const float c[4])
{
    if (!isfinite(c[0]) || !isfinite(c[1]) ||
        !isfinite(c[2]) || !isfinite(c[3])) {
        return;
    }
    v->r = c[0];
    v->g = c[1];
    v->b = c[2];
    v->a = c[3];
}

/*
 * Run the vertex program for this vertex, as far as this model consumes
 * it: the clip-space position, and the colour the rasterizer interpolates.
 *
 * The colour is the point of it. Chess.app's board arrives as a position
 * and a normal and carries no colour of its own; the shade of a square is
 * a lighting term its program computes and writes to the first colour
 * output, so without running the program the board is drawn in whatever
 * the fragment stage's constant happens to be. Texture coordinates stay on
 * the attribute path -- the model interpolates those from the vertex, and
 * the programs here compute them with a matrix that is the identity
 * against a pixel-space sampler.
 *
 * Returns true when `clip` holds a position the program computed. A
 * program that is exactly the 4x4 matrix the fixed path already applies
 * returns false and leaves the position to it: the arithmetic is the same
 * four dot products either way, and the desktop's every draw is that
 * program.
 */
static bool r300_vs_vtx(ATIR350State *s, const R300DrawState *d,
                        const uint32_t *dw, R300Vtx *v, float clip[4])
{
    R300PvsRegs r;
    R300PvsGaps g;
    unsigned a;

    if (d->vs.plain_matrix) {
        int src = d->vs.out_src[d->vs_color_out];
        float col[4];

        /*
         * Its position is the matrix the caller already has; its colour,
         * if it emits one at all, is an attribute forwarded unchanged.
         */
        if (src >= 0 && r300_vs_has_color(d)) {
            r300_vs_input(d, dw, src, col);
            r300_vs_color(v, col);
        }
        return false;
    }

    memset(&r, 0, sizeof(r));
    for (a = 0; a < R300_PVS_IN_REGS; a++) {
        r.in[a][3] = 1.0f;
    }
    for (a = 0; a < d->attr_count; a++) {
        r300_vs_input(d, dw, a, r.in[a]);
    }
    memset(&g, 0, sizeof(g));
    r300_pvs_run(&d->vs, &r, &g);
    if (g.has_vec_op) {
        ati_r350_note_gap(s, R350_GAP_VS_VECTOR_OP, g.vec_op);
    }
    if (g.has_math_op) {
        ati_r350_note_gap(s, R350_GAP_VS_MATH_OP, g.math_op);
    }
    if (g.has_dst_file) {
        ati_r350_note_gap(s, R350_GAP_VS_DST_FILE, g.dst_file);
    }

    if ((r.out_written & (1u << d->vs_color_out)) && r300_vs_has_color(d)) {
        r300_vs_color(v, r.out[d->vs_color_out]);
    }
    if (d->vs_texcoord && (r.out_written & (1u << d->vs_tex_out))) {
        r300_vs_texcoord(d, v, r.out[d->vs_tex_out]);
    }
    if (!(r.out_written & 1)) {
        /*
         * A program that never wrote the position leaves nothing to
         * transform; the matrix is a better answer than out[0]'s zeroes.
         */
        return false;
    }
    memcpy(clip, r.out[0], sizeof(r.out[0]));
    return true;
}

/*
 * 3D_DRAW_IMMD_2: dw[0] is VAP_VF_CNTL (primitive type, walk mode,
 * vertex count), the rest is vertex data laid out VAP_VTX_SIZE dwords
 * per vertex.
 */
static bool r300_setup_draw(ATIR350State *s, R300DrawState *d,
                            unsigned vsize)
{
    uint32_t colorpitch = s->regs[R300_RB3D_COLORPITCH0 >> 2];
    uint32_t txfmt0 = s->regs[R300_TX_FORMAT0_0 >> 2];
    uint32_t txfmt2 = s->regs[R300_TX_FORMAT2_0 >> 2];

    d->vram = memory_region_get_ram_ptr(&s->vram);
    if (!ati_r350_mc_to_vram(s, s->regs[R300_RB3D_COLOROFFSET0 >> 2] & ~0x1fu,
                             &d->dst_off)) {
        /*
         * Colour buffer outside VRAM. Nothing here can render into it,
         * but say so rather than dropping the draw without a word: a
         * guest that composes somewhere we refuse to follow looks
         * exactly like a guest that never drew at all, and the two need
         * telling apart.
         */
        ati_r350_note_gap(s, R350_GAP_DEST_OFF_VRAM, 0);
        return false;
    }
    d->dst_pitch = (colorpitch & 0x3fff) * 4;
    if (!d->dst_pitch) {
        return false;
    }
    {
        uint32_t cm = s->regs[R300_RB3D_COLOR_CHANNEL_MASK >> 2];

        if (!(cm & (R300_COLORMASK_BLUE | R300_COLORMASK_GREEN |
                    R300_COLORMASK_RED | R300_COLORMASK_ALPHA))) {
            /*
             * Every channel masked off: the colour buffer discards the
             * quads. Chess's depth-only passes arrive this way, and
             * shading them smeared a texture across the board.
             */
            return false;
        }
        d->wmask = (cm & R300_COLORMASK_ALPHA ? 0xff000000u : 0) |
                   (cm & R300_COLORMASK_RED   ? 0x00ff0000u : 0) |
                   (cm & R300_COLORMASK_GREEN ? 0x0000ff00u : 0) |
                   (cm & R300_COLORMASK_BLUE  ? 0x000000ffu : 0);
    }
    /*
     * An AA resolve keeps rasterizing over the same geometry but sends
     * the colour buffer's contents, not the shaded fragment, to the
     * resolve buffer. Swap the destination here so scissor, cliprects
     * and the write mask all keep applying unchanged.
     */
    d->resolve = s->regs[R300_RB3D_AARESOLVE_CTL >> 2] & R300_AARESOLVE_MODE;
    d->res_off = 0;
    d->res_pitch = 0;
    if (d->resolve) {
        uint32_t roff;
        uint32_t rpitch = ((s->regs[R300_RB3D_AARESOLVE_PITCH >> 2] >> 1) &
                           0x1fff) * 2 * 4;

        if (!ati_r350_mc_to_vram(s,
                                 s->regs[R300_RB3D_AARESOLVE_OFFSET >> 2] &
                                 ~0x1fu, &roff)) {
            ati_r350_note_gap(s, R350_GAP_DEST_OFF_VRAM, 0);
            return false;
        }
        if (!rpitch) {
            return false;
        }
        d->res_off = d->dst_off;
        d->res_pitch = d->dst_pitch;
        d->dst_off = roff;
        d->dst_pitch = rpitch;
    }
    d->textured = (s->regs[R300_TX_ENABLE >> 2] & 1) && vsize >= 8;
    /*
     * RB3D_BLENDCNTL (R5xx accel guide): bit 0 is ALPHA_BLEND_ENABLE,
     * SRCBLEND lives in [21:16] and DESTBLEND in [29:24] as 6-bit
     * factor codes (GL names from 32 up, D3D names from 1). Quartz
     * composites premultiplied: ONE / ONE_MINUS_SRC_ALPHA. Treating
     * any non-zero value as source-alpha blending drew window content
     * (BLENDCNTL 0x27210006 -- blending DISABLED) translucent and
     * drop shadows opaque black.
     */
    {
        uint32_t bl = s->regs[R300_RB3D_BLENDCNTL >> 2];
        uint32_t af = s->regs[R300_FG_ALPHA_FUNC >> 2];

        uint32_t ab = s->regs[R300_RB3D_ABLENDCNTL >> 2];
        uint32_t kc = s->regs[R300_RB3D_BLEND_COLOR >> 2];

        d->blend = bl & R300_BLEND_ENABLE;
        d->blend_read = bl & R300_BLEND_READ_ENABLE;
        d->discard = (bl >> R300_BLEND_DISCARD_SHIFT) & 7;
        d->src_factor = (bl >> R300_BLEND_SRC_SHIFT) & R300_BLEND_FACTOR_MASK;
        d->dst_factor = (bl >> R300_BLEND_DST_SHIFT) & R300_BLEND_FACTOR_MASK;
        d->comb_fcn = (bl >> R300_BLEND_COMB_FCN_SHIFT) & 7;
        /*
         * Only CBLEND carries the enables; when SEPARATE_ALPHA is set
         * the alpha channel takes its factors and combine from ABLEND
         * instead. Mac OS X sets it on every blended draw, with the
         * same premultiplied ONE / ONE_MINUS_SRC_ALPHA pair in both
         * registers -- so honouring it changes nothing on the desktop
         * and everything for a program that sets them differently.
         */
        if (bl & R300_BLEND_SEPARATE_ALPHA) {
            d->a_src_factor = (ab >> R300_BLEND_SRC_SHIFT) &
                              R300_BLEND_FACTOR_MASK;
            d->a_dst_factor = (ab >> R300_BLEND_DST_SHIFT) &
                              R300_BLEND_FACTOR_MASK;
            d->a_comb_fcn = (ab >> R300_BLEND_COMB_FCN_SHIFT) & 7;
        } else {
            d->a_src_factor = d->src_factor;
            d->a_dst_factor = d->dst_factor;
            d->a_comb_fcn = d->comb_fcn;
        }
        d->k_a = ((kc >> 24) & 0xff) / 255.0f;
        d->k_r = ((kc >> 16) & 0xff) / 255.0f;
        d->k_g = ((kc >> 8) & 0xff) / 255.0f;
        d->k_b = (kc & 0xff) / 255.0f;
        if (d->blend) {
            unsigned f[4] = { d->src_factor, d->dst_factor,
                              d->a_src_factor, d->a_dst_factor };
            unsigned i;

            for (i = 0; i < ARRAY_SIZE(f); i++) {
                if (!r300_blend_known(f[i])) {
                    ati_r350_note_gap(s, R350_GAP_BLEND_FACTOR, f[i]);
                }
            }
        }
        /*
         * FG_ALPHA_FUNC: AF_EN in bit 11, compare function in [10:8],
         * 8-bit reference in [7:0]. OS X composes its cursor tile
         * with AF_GREATER ref 0 -- transparent cursor pixels are
         * DISCARDED, not blended; painting them drew the cursor as an
         * opaque black box.
         */
        d->alpha_test = af & (1 << 11);
        d->af_func = (af >> 8) & 7;
        d->af_ref = (af & 0xff) / 255.0f;
    }
    d->tex_off = s->regs[R300_TX_OFFSET_0 >> 2] & ~0x1fu;
    d->tex_w = (txfmt0 & 0x7ff) + 1;
    d->tex_h = ((txfmt0 >> 11) & 0x7ff) + 1;
    /*
     * TX_FORMAT1's low format code (R3xx register reference, TXFORMAT
     * [4:0]): 0 is TX_FMT_8, the single-component format window drop
     * shadows arrive in (TX_FORMAT1=0x00124000); 3 is TX_FMT_8_8, the
     * two-component luminance/alpha sprite Flurry.saver's particles
     * are drawn with; 0xb is TX_FMT_1_5_5_5, which Abstract.saver
     * asks for; 0xc is TX_FMT_8_8_8_8, what the compositor and most
     * apps use; 0xe is TX_FMT_16_16_16_16, which RSS Visualizer.saver
     * asks for. r300_sample_tex() hands all of them to the component
     * select as four bytes, so one selector implementation serves
     * every format. TXPITCH counts texels, so the byte pitch scales
     * with the texel size -- reading an 8_8 texture as four bytes per
     * texel doubled both the pitch and the stride and made one dword
     * span two texels.
     */
    {
        uint32_t txfmt1 = s->regs[R300_TX_FORMAT1_0 >> 2];
        unsigned txcode = txfmt1 & R300_TX_FORMAT1_CODE_MASK;
        unsigned ch;

        d->tex_code = txcode;
        switch (txcode) {
        case R300_TX_FMT_8:
            d->tex_bpp = 8;
            break;
        case R300_TX_FMT_8_8:
        case R300_TX_FMT_1_5_5_5:
            d->tex_bpp = 16;
            break;
        case R300_TX_FMT_16_16_16_16:
            d->tex_bpp = 64;
            break;
        case R300_TX_FMT_8_8_8_8:
            d->tex_bpp = 32;
            break;
        default:
            /* the rest are being read as if their components were bytes */
            d->tex_bpp = 32;
            if (d->textured) {
                ati_r350_note_gap(s, R350_GAP_TEX_FORMAT, txcode);
            }
            break;
        }
        /*
         * Which component feeds each of A, R, G and B. Reading the
         * texel as ARGB regardless happens to be right for the
         * selector Mac OS X's window tiles use, and wrong for
         * Chess.app's board texture, which orders the same four bytes
         * the other way round.
         */
        for (ch = 0; ch < 4; ch++) {
            d->tex_sel[ch] = (txfmt1 >> (R300_TX_FORMAT1_SEL_SHIFT + ch * 3)) &
                             R300_TX_FORMAT1_SEL_MASK;
            if (d->textured && d->tex_sel[ch] > R300_TX_SEL_ONE) {
                ati_r350_note_gap(s, R350_GAP_TEX_SWIZZLE, d->tex_sel[ch]);
            }
        }
    }
    d->clamp_s = s->regs[R300_TX_FILTER0_0 >> 2] & 7;
    d->clamp_t = (s->regs[R300_TX_FILTER0_0 >> 2] >> 3) & 7;
    /*
     * The pitch register only applies when TX_FORMAT0 says so;
     * otherwise rows are exactly the texture's width.
     */
    if (txfmt0 & R300_TX_PITCH_EN) {
        d->tex_pitch = ((txfmt2 & 0x3fff) + 1) * (d->tex_bpp / 8);
    } else {
        d->tex_pitch = (uint32_t)d->tex_w * (d->tex_bpp / 8);
    }
    /*
     * Position transform: unless the draw bypasses the vertex program
     * (VAP_CNTL_STATUS bit 8 -- the point-sprite composites do),
     * positions run through the blit shader's 4x4 matrix (PVS
     * constants 0-3) and then the SE_VPORT scale/offset. The matrix
     * is how the driver retargets one command stream at differently
     * sized destinations; ignoring it wrote atlas/dirty-strip draws
     * at raw window coordinates, striping icons and the Dock.
     */
    /*
     * Window scissor: SC_SCISSOR0/1 hold top-left and bottom-right,
     * 13-bit fields biased by +1440 on R300/R400. The driver relies
     * on it -- the menu bar redraws scissored to rows 0-21, and some
     * passes park a zero-area scissor to mask a draw off entirely.
     * Zero registers (engine bring-up) mean no scissor yet.
     */
    {
        uint32_t sc0 = s->regs[R300_SC_SCISSOR0 >> 2];
        uint32_t sc1 = s->regs[R300_SC_SCISSOR1 >> 2];

        if (sc1) {
            d->sc_x0 = (int)(sc0 & 0x1fff) - R300_SCISSOR_OFFSET;
            d->sc_y0 = (int)((sc0 >> 13) & 0x1fff) - R300_SCISSOR_OFFSET;
            d->sc_x1 = (int)(sc1 & 0x1fff) - R300_SCISSOR_OFFSET;
            d->sc_y1 = (int)((sc1 >> 13) & 0x1fff) - R300_SCISSOR_OFFSET;
        } else {
            d->sc_x0 = d->sc_y0 = 0;
            d->sc_x1 = d->sc_y1 = 0x1fff;
        }
    }

    /*
     * Clip rectangles: up to four rects plus a 16-entry truth table in
     * RE_CLIPRECT_CNTL indexed by which rects contain the pixel
     * (0xffff = pass everything, 0xaaaa = pass only inside rect 0).
     * WindowServer clips every window-content draw with rect 0; the
     * coordinates carry the same +1440 bias as the scissor, with an
     * exclusive bottom-right. A never-written CNTL means no clipping.
     */
    d->clip_rule = s->regs[R300_RE_CLIPRECT_CNTL >> 2] & 0xffff;
    if (d->clip_rule && d->clip_rule != 0xffff) {
        int r;

        for (r = 0; r < 4; r++) {
            uint32_t tl = s->regs[(R300_RE_CLIPRECT_TL_0 >> 2) + r * 2];
            uint32_t br = s->regs[(R300_RE_CLIPRECT_TL_0 >> 2) + r * 2 + 1];

            d->cr[r][0] = (int)(tl & 0x1fff) - R300_SCISSOR_OFFSET;
            d->cr[r][1] = (int)((tl >> 13) & 0x1fff) - R300_SCISSOR_OFFSET;
            d->cr[r][2] = (int)(br & 0x1fff) - R300_SCISSOR_OFFSET;
            d->cr[r][3] = (int)((br >> 13) & 0x1fff) - R300_SCISSOR_OFFSET;
        }
    } else {
        d->clip_rule = 0xffff;
    }

    d->xform = false;
    d->vte_xs = d->vte_xo = d->vte_ys = d->vte_yo = false;
    d->vs_run = false;
    d->vs_color = false;
    d->vs_texcoord = false;
    d->vs_tex_out = 0;
    d->tex_attr = -1;
    memset(&d->vs, 0, sizeof(d->vs));
    if (!(s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_PVS_BYPASS) &&
        s->pvs_const_dwords >= 16) {
        int k;
        uint32_t vte;
        float xs = r300_f32(s->regs[R300_SE_VPORT_XSCALE >> 2]);

        if (xs != 0.0f) {
            unsigned first_color, ncolor, first_tex, cb;

            /*
             * What the program in force is, and whether it is the plain
             * 4x4 matrix the desktop is painted with. Deciding this per
             * draw from the control registers -- not from how much has
             * ever been uploaded -- is what makes the result independent
             * of what ran before: an earlier attempt at this took the
             * bounds from a high-water mark of the upload stream, and the
             * same draw then rendered differently according to its
             * history.
             */
            r300_pvs_out_layout(s->regs[R300_VAP_OUTPUT_VTX_FMT_0 >> 2],
                                &first_color, &ncolor, &first_tex);
            r300_pvs_analyse(&d->vs, s->pvs_code, s->pvs_code_slot_valid,
                             R300_PVS_CODE_SLOTS,
                             s->pvs_const, R300_PVS_CONST_SLOTS,
                             s->regs[R300_VAP_PVS_CODE_CNTL_0 >> 2],
                             s->regs[R300_VAP_PVS_CONST_CNTL >> 2],
                             first_tex);
            cb = d->vs.valid ? d->vs.cbase * 4 : 0;
            if (cb + 16 > ARRAY_SIZE(s->pvs_const)) {
                cb = 0;
            }
            for (k = 0; k < 16; k++) {
                d->mat[k] = r300_f32(s->pvs_const[cb + k]);
            }
            for (k = 0; k < 6; k++) {
                d->vp[k] = r300_f32(s->regs[(R300_SE_VPORT_XSCALE >> 2) + k]);
            }
            vte = s->regs[R300_VAP_VTE_CNTL >> 2];
            /*
             * The two format bits describe the perspective divide this
             * model performs unconditionally: XY_FMT clear means the
             * setup engine still has to divide x and y by w, and
             * W0_FMT set means the w it was handed is w rather than
             * 1/w. Every draw in every capture reads them that way.
             * Anything else would need a different divide, so say so
             * rather than transform the vertex wrongly in silence.
             */
            if ((vte & (R300_VTE_VTX_XY_FMT | R300_VTE_VTX_W0_FMT)) !=
                R300_VTE_VTX_W0_FMT) {
                ati_r350_note_gap(s, R350_GAP_VTE_FMT,
                                  (vte >> 8) & (R350_GAP_SLOTS - 1));
            }
            d->vte_xs = vte & R300_VTE_VPORT_X_SCALE_ENA;
            d->vte_xo = vte & R300_VTE_VPORT_X_OFFSET_ENA;
            d->vte_ys = vte & R300_VTE_VPORT_Y_SCALE_ENA;
            d->vte_yo = vte & R300_VTE_VPORT_Y_OFFSET_ENA;
            d->xform = true;
            d->vs_run = d->vs.valid;
            d->vs_color_out = first_color;
            /*
             * Where the coordinate lives in the vertex -- see
             * r300_attr_texcoord(). Only a FORWARDED output names an
             * attribute; the compositor's blit program multiplies its
             * coordinate by a texture matrix instead, so out_src is -1
             * for every draw the desktop is painted with and this is a
             * no-op there by construction.
             */
            if (d->textured && d->vs.valid &&
                first_tex < R300_PVS_OUT_REGS &&
                (d->vs.out_mask & (1u << first_tex))) {
                d->tex_attr = d->vs.out_src[first_tex];
            }
            /*
             * The colour is the program's only when the vertex stage says
             * it emits one and the program really writes it. A colour it
             * merely forwards from an attribute this model is already
             * sampling as texture coordinates is not a colour at all --
             * the eight-dword textured vertices whose second attribute is
             * a coordinate pair would otherwise arrive painted with it.
             * Which attribute that is comes from the program where the
             * program says (tex_attr), and only otherwise from the
             * position of the coordinate in a flat vertex.
             */
            if (d->vs_run && ncolor &&
                (d->vs.out_mask & (1u << first_color))) {
                int src = d->vs.out_src[first_color];
                bool computed = !d->vs.plain_matrix || src >= 0;
                bool is_texcoord = d->textured && src >= 0 &&
                                   src == (d->tex_attr >= 0 ? d->tex_attr :
                                           (vsize >= 12 ? 2 : 1));

                d->vs_color = computed && !is_texcoord;
            }
            /*
             * The texture coordinate is the program's only when the model
             * is going to run the program at all: a program recognised as
             * the plain matrix keeps the fast path, which never evaluates
             * an output, and its coordinate is the attribute the vertex
             * already carries. The two agree anyway -- that program's
             * texture matrix is the exact inverse of the scaling below --
             * so this is a decision about cost, not about semantics.
             */
            d->vs_texcoord = d->vs_run && !d->vs.plain_matrix &&
                             d->textured && first_tex < R300_PVS_OUT_REGS &&
                             (d->vs.out_mask & (1u << first_tex));
            d->vs_tex_out = first_tex;
        }
    }
    /*
     * A draw whose program this model will not execute -- the bounds name
     * instruction slots the guest has not uploaded -- still runs, on the
     * matrix, and is still counted: that is the one case left where the
     * position is an approximation rather than the program's own answer.
     */
    if (!(s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_PVS_BYPASS) &&
        s->pvs_code_dwords && !d->vs_run) {
        ati_r350_note_gap(s, R350_GAP_VTX_PROGRAM, 0);
    }
    /*
     * Vertices without a colour attribute take the fragment program's
     * constant colour: OS X's solid-fill shader outputs PFS_PARAM_0
     * (stored as 24-bit floats -- IEEE with the low mantissa byte
     * dropped). The desktop backdrop fill arrives exactly this way, a
     * colourless full-screen quad with the blue in PFS_PARAM_0.
     */
    if (vsize < 12 && !d->textured) {
        d->flat_r = r300_f32(s->regs[(R300_PFS_PARAM_0_X >> 2)] << 8);
        d->flat_g = r300_f32(s->regs[(R300_PFS_PARAM_0_X >> 2) + 1] << 8);
        d->flat_b = r300_f32(s->regs[(R300_PFS_PARAM_0_X >> 2) + 2] << 8);
        d->flat_a = r300_f32(s->regs[(R300_PFS_PARAM_0_X >> 2) + 3] << 8);
    } else {
        d->flat_r = d->flat_g = d->flat_b = d->flat_a = 1.0f;
    }
    /*
     * What the colour buffer was configured to do with this draw, read
     * where the draw reads it. These three registers decide whether a
     * draw lands at all, and a post-hoc read of them says only what the
     * last writer left behind.
     */
    trace_ati_r350_3d_cb(d->dst_off, d->dst_pitch, d->wmask, d->resolve,
                         d->res_off, d->res_pitch);
    return true;
}

/*
 * One line segment, expanded to a quad a pixel wide across its own
 * direction and handed to the triangle rasterizer so it picks up the
 * same texturing, blending and clipping as everything else.
 */
static void r300_raster_line(ATIR350State *s, const R300DrawState *d,
                             const R300Vtx *a, const R300Vtx *b)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    R300Vtx q[4];
    int c;

    if (len < 0.000001f) {
        return;
    }
    /* half-pixel normal to the segment */
    nx = -dy / len * 0.5f;
    ny = dx / len * 0.5f;
    for (c = 0; c < 4; c++) {
        q[c] = (c == 0 || c == 3) ? *a : *b;
    }
    q[0].x = a->x + nx; q[0].y = a->y + ny;
    q[1].x = b->x + nx; q[1].y = b->y + ny;
    q[2].x = b->x - nx; q[2].y = b->y - ny;
    q[3].x = a->x - nx; q[3].y = a->y - ny;
    r300_raster_tri(s, d, &q[0], &q[1], &q[2]);
    r300_raster_tri(s, d, &q[0], &q[2], &q[3]);
}

static void r300_raster_prims(ATIR350State *s, R300DrawState *d,
                              const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    unsigned i;

    switch (prim) {
    case 1:     /* point list -- WindowServer's screen composites are
                 * point SPRITES: RE_POINTSIZE gives the width/height
                 * in 1/6-pixel units, GA_POINT_S0/T0 (top-left) and
                 * S1/T1 (bottom-right) give the normalized texture
                 * window. A full-screen layer flip is a single
                 * 1024x768 sprite at (512,384); the menu bar repaints
                 * as a 1023x1 strip. */
    {
        uint32_t psize = s->regs[R300_RE_POINTSIZE >> 2];
        float sx = ((psize >> 16) & 0xffff) / 6.0f;
        float sy = (psize & 0xffff) / 6.0f;
        float s0 = r300_f32(s->regs[R300_GA_POINT_S0 >> 2]) * d->tex_w;
        float s1 = r300_f32(s->regs[R300_GA_POINT_S1 >> 2]) * d->tex_w;
        /*
         * T0 pairs with the sprite's BOTTOM edge and T1 with the top
         * (GL-style v axis): the full-screen composite arrives as
         * T0=1, T1=0 over a layer stored top-down, and mapping T0 to
         * the top edge mirrored the whole desktop vertically.
         */
        float t1 = r300_f32(s->regs[R300_GA_POINT_T0 >> 2]) * d->tex_h;
        float t0 = r300_f32(s->regs[R300_GA_POINT_T1 >> 2]) * d->tex_h;

        d->textured = s->regs[R300_TX_ENABLE >> 2] & 1;
        for (i = 0; i < nvtx && sx > 0.0f && sy > 0.0f; i++) {
            R300Vtx q[4];
            int c;

            for (c = 0; c < 4; c++) {
                q[c] = vb[i];
                if (d->textured) {
                    /* composite sprites modulate by nothing */
                    q[c].r = q[c].g = q[c].b = q[c].a = 1.0f;
                }
            }
            q[0].x = vb[i].x - sx / 2; q[0].y = vb[i].y - sy / 2;
            q[0].s = s0; q[0].t = t0;
            q[1].x = vb[i].x + sx / 2; q[1].y = q[0].y;
            q[1].s = s1; q[1].t = t0;
            q[2].x = q[1].x; q[2].y = vb[i].y + sy / 2;
            q[2].s = s1; q[2].t = t1;
            q[3].x = q[0].x; q[3].y = q[2].y;
            q[3].s = s0; q[3].t = t1;
            r300_raster_tri(s, d, &q[0], &q[1], &q[2]);
            r300_raster_tri(s, d, &q[0], &q[2], &q[3]);
        }
        break;
    }
    case 2:     /* line list */
        for (i = 0; i + 2 <= nvtx; i += 2) {
            r300_raster_line(s, d, &vb[i], &vb[i + 1]);
        }
        break;
    case 3:     /* line strip */
        for (i = 1; i < nvtx; i++) {
            r300_raster_line(s, d, &vb[i - 1], &vb[i]);
        }
        break;
    case 12:    /* line loop: a strip that closes back on itself */
        for (i = 1; i < nvtx; i++) {
            r300_raster_line(s, d, &vb[i - 1], &vb[i]);
        }
        if (nvtx > 2) {
            r300_raster_line(s, d, &vb[nvtx - 1], &vb[0]);
        }
        break;
    case 4:     /* triangle list */
    case 7:     /* TRI_TYPE2: a triangle list with its own vertex
                 * routing; the assembly into triangles is the same */
        for (i = 0; i + 3 <= nvtx; i += 3) {
            r300_raster_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2]);
        }
        break;
    case 5:     /* triangle fan */
    case 15:    /* polygon: fan-assembled, convex by definition here */
        for (i = 2; i < nvtx; i++) {
            r300_raster_tri(s, d, &vb[0], &vb[i - 1], &vb[i]);
        }
        break;
    case 6:     /* triangle strip */
        for (i = 2; i < nvtx; i++) {
            r300_raster_tri(s, d, &vb[i - 2], &vb[i - 1], &vb[i]);
        }
        break;
    case 8:     /* rectangle list: three corners, fourth implied */
        for (i = 0; i + 3 <= nvtx; i += 3) {
            R300Vtx v3 = vb[i + 2];

            /* the missing corner is v0 + (v1 - v0) + (v2 - v0) */
            v3.x = vb[i + 1].x + vb[i + 2].x - vb[i].x;
            v3.y = vb[i + 1].y + vb[i + 2].y - vb[i].y;
            v3.s = vb[i + 1].s + vb[i + 2].s - vb[i].s;
            v3.t = vb[i + 1].t + vb[i + 2].t - vb[i].t;
            r300_raster_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2]);
            r300_raster_tri(s, d, &vb[i + 1], &v3, &vb[i + 2]);
        }
        break;
    case 14:    /* quad strip: each further vertex pair closes a quad
                 * against the previous pair (Chess.app draws its board
                 * and pieces almost entirely out of these) */
        for (i = 2; i + 2 <= nvtx; i += 2) {
            r300_raster_tri(s, d, &vb[i - 2], &vb[i - 1], &vb[i + 1]);
            r300_raster_tri(s, d, &vb[i - 2], &vb[i + 1], &vb[i]);
        }
        break;
    case 13:    /* quad list */
        for (i = 0; i + 4 <= nvtx; i += 4) {
            r300_raster_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2]);
            r300_raster_tri(s, d, &vb[i], &vb[i + 2], &vb[i + 3]);
        }
        break;
    default:
        trace_ati_r350_3d_skip(0, nvtx, prim);
        ati_r350_note_gap(s, R350_GAP_PRIM, prim);
        break;
    }
}

/*
 * Draw capture, for the offline GL replay harness in
 * doc/radeon9800/gl-replay/. Everything below runs only when the
 * "draw-capture" property named a file; see ati_r350_cap.h for what a
 * record holds and why the capture is taken here rather than off the
 * command stream.
 */
static uint32_t r300_cap_hash(const uint8_t *p, uint32_t len)
{
    uint32_t h = 2166136261u;
    uint32_t i;

    for (i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/*
 * A record carries one swapper xor per region, so a region the swapper
 * does not treat uniformly cannot be represented and its draw is skipped
 * rather than recorded wrong. Surface descriptors cover contiguous
 * multi-page ranges, so a stride well under a page settles it.
 */
static bool r300_cap_xor(ATIR350State *s, uint32_t off, uint32_t len,
                         unsigned *xr)
{
    unsigned v = ati_r350_vram_xor(s, off);
    uint32_t i;

    for (i = 256; i < len; i += 256) {
        if (ati_r350_vram_xor(s, off + i) != v) {
            return false;
        }
    }
    if (len && ati_r350_vram_xor(s, off + len - 1) != v) {
        return false;
    }
    *xr = v;
    return true;
}

/*
 * Where this draw can write: the primitive's own bounding box, widened
 * by a pixel because a line is expanded across its direction and a
 * rectangle list implies a fourth corner, then clipped exactly the way
 * r300_raster_tri() clips its scan.
 */
static bool r300_cap_rect(ATIR350State *s, const R300DrawState *d,
                          const R300Vtx *vb, unsigned nvtx, unsigned prim,
                          int *rx0, int *ry0, int *rx1, int *ry1)
{
    float fx0 = vb[0].x, fy0 = vb[0].y, fx1 = fx0, fy1 = fy0;
    int x0, y0, x1, y1;
    unsigned i;

    for (i = 1; i < nvtx; i++) {
        fx0 = MIN(fx0, vb[i].x); fx1 = MAX(fx1, vb[i].x);
        fy0 = MIN(fy0, vb[i].y); fy1 = MAX(fy1, vb[i].y);
    }
    if (prim == 8) {
        /* the corner a rectangle list leaves implied */
        for (i = 0; i + 3 <= nvtx; i += 3) {
            float px = vb[i + 1].x + vb[i + 2].x - vb[i].x;
            float py = vb[i + 1].y + vb[i + 2].y - vb[i].y;

            fx0 = MIN(fx0, px); fx1 = MAX(fx1, px);
            fy0 = MIN(fy0, py); fy1 = MAX(fy1, py);
        }
    }
    if (prim == 1) {
        uint32_t psize = s->regs[R300_RE_POINTSIZE >> 2];
        float hw = ((psize >> 16) & 0xffff) / 12.0f;
        float hh = (psize & 0xffff) / 12.0f;

        fx0 -= hw; fx1 += hw;
        fy0 -= hh; fy1 += hh;
    }
    if (!isfinite(fx0) || !isfinite(fy0) || !isfinite(fx1) ||
        !isfinite(fy1) || fx0 < -100000.0f || fx1 > 100000.0f ||
        fy0 < -100000.0f || fy1 > 100000.0f) {
        return false;
    }
    x0 = (int)floorf(fx0) - 1;
    y0 = (int)floorf(fy0) - 1;
    x1 = (int)ceilf(fx1) + 1;
    y1 = (int)ceilf(fy1) + 1;
    x0 = MAX(x0, MAX(d->sc_x0, 0));
    y0 = MAX(y0, MAX(d->sc_y0, 0));
    x1 = MIN(x1, MIN(d->sc_x1 + 1, 8191));
    y1 = MIN(y1, MIN(d->sc_y1 + 1, 8191));
    if (x1 <= x0 || y1 <= y0 || !d->dst_pitch) {
        return false;
    }
    /* every byte of the rectangle has to be inside VRAM to be captured */
    while (y1 > y0 &&
           d->dst_off + (uint32_t)(y1 - 1) * d->dst_pitch +
           (uint32_t)x1 * 4 > ATI_R350_VRAM_SIZE) {
        y1--;
    }
    if (y1 <= y0) {
        return false;
    }
    *rx0 = x0; *ry0 = y0; *rx1 = x1; *ry1 = y1;
    return true;
}

/* one packed copy of the destination rectangle, raw VRAM bytes */
static void r300_cap_read_rect(const R300DrawState *d, int x0, int y0,
                               int x1, int y1, uint8_t *out)
{
    uint32_t row = (uint32_t)(x1 - x0) * 4;
    int y;

    for (y = y0; y < y1; y++) {
        memcpy(out + (uint32_t)(y - y0) * row,
               d->vram + d->dst_off + (uint32_t)y * d->dst_pitch +
               (uint32_t)x0 * 4, row);
    }
}

static void r300_cap_write(ATIR350State *s, const void *p, size_t n)
{
    if (s->cap_fp && fwrite(p, 1, n, s->cap_fp) != n) {
        fclose(s->cap_fp);
        s->cap_fp = NULL;
    }
}

static void r300_cap_draw(ATIR350State *s, R300DrawState *d,
                          const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    R350CapRecHdr h = { 0 };
    R300DrawState st;
    g_autofree uint8_t *before = NULL;
    g_autofree uint8_t *after = NULL;
    unsigned dxr = 0, txr = 0;
    uint32_t tex_off = 0, tex_len = 0, tex_hash = 0;
    int x0, y0, x1, y1;
    unsigned i;

    /*
     * A draw is recorded only when a record can describe it exactly.
     * Anything else is counted and rasterized as usual: a capture that
     * quietly stored an approximation would be worse than a short one,
     * because the harness reading it cannot tell the two apart.
     */
    if (d->resolve ||
        !r300_cap_rect(s, d, vb, nvtx, prim, &x0, &y0, &x1, &y1) ||
        (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0) > s->cap_max_px ||
        !r300_cap_xor(s, d->dst_off + (uint32_t)y0 * d->dst_pitch,
                      (uint32_t)(y1 - 1 - y0) * d->dst_pitch +
                      (uint32_t)x1 * 4, &dxr)) {
        s->cap_skipped++;
        r300_raster_prims(s, d, vb, nvtx, prim);
        return;
    }
    if (d->textured || prim == 1) {
        uint32_t last;

        tex_len = d->tex_pitch * (uint32_t)d->tex_h;
        if (!tex_len || !ati_r350_mc_to_vram(s, d->tex_off, &tex_off) ||
            !ati_r350_mc_to_vram(s, d->tex_off + tex_len - 1, &last) ||
            last != tex_off + tex_len - 1 ||
            !r300_cap_xor(s, tex_off, tex_len, &txr)) {
            s->cap_skipped++;
            r300_raster_prims(s, d, vb, nvtx, prim);
            return;
        }
        tex_hash = r300_cap_hash(d->vram + tex_off, tex_len);
    }

    h.magic = R350_CAP_REC_MAGIC;
    h.index = s->cap_index;
    h.prim = prim;
    h.nvtx = nvtx;
    h.x0 = x0; h.y0 = y0; h.x1 = x1; h.y1 = y1;
    h.rect_bytes = (uint32_t)(x1 - x0) * 4 * (uint32_t)(y1 - y0);
    h.dst_xor = dxr;
    h.tex_xor = txr;
    h.tex_vram_off = tex_off;
    h.tex_bytes = tex_len;
    h.txfmt1 = s->regs[R300_TX_FORMAT1_0 >> 2];
    h.pointsize = s->regs[R300_RE_POINTSIZE >> 2];
    h.point_s0 = s->regs[R300_GA_POINT_S0 >> 2];
    h.point_s1 = s->regs[R300_GA_POINT_S1 >> 2];
    h.point_t0 = s->regs[R300_GA_POINT_T0 >> 2];
    h.point_t1 = s->regs[R300_GA_POINT_T1 >> 2];
    h.tx_enable = s->regs[R300_TX_ENABLE >> 2];
    h.flags = (d->vs_run ? R350_CAP_F_VS_RUN : 0) |
              (d->vs.plain_matrix ? R350_CAP_F_PLAIN_MAT : 0);

    for (i = 0; tex_len && i < s->cap_tex_n; i++) {
        if (s->cap_tex[i].off == tex_off && s->cap_tex[i].len == tex_len &&
            s->cap_tex[i].xr == txr && s->cap_tex[i].hash == tex_hash) {
            h.tex_bytes = 0;
            h.tex_ref = s->cap_tex[i].rec;
            h.flags |= R350_CAP_F_TEXDEDUP;
            break;
        }
    }
    if (tex_len && !(h.flags & R350_CAP_F_TEXDEDUP)) {
        i = s->cap_index % R350_CAP_TEX_CACHE;
        s->cap_tex[i].off = tex_off;
        s->cap_tex[i].len = tex_len;
        s->cap_tex[i].xr = txr;
        s->cap_tex[i].hash = tex_hash;
        s->cap_tex[i].rec = s->cap_index;
        if (s->cap_tex_n < R350_CAP_TEX_CACHE) {
            s->cap_tex_n++;
        }
    }

    /*
     * The VRAM pointer and the vertex program are the only members of
     * the state that are not plain data, and nothing below
     * r300_run_prims() reads either, so a record drops both: the harness
     * substitutes its own VRAM and never resurrects a program.
     */
    st = *d;
    st.vram = NULL;
    memset(&st.vs, 0, sizeof(st.vs));

    before = g_malloc(h.rect_bytes);
    after = g_malloc(h.rect_bytes);
    r300_cap_read_rect(d, x0, y0, x1, y1, before);
    r300_raster_prims(s, d, vb, nvtx, prim);
    r300_cap_read_rect(d, x0, y0, x1, y1, after);

    r300_cap_write(s, &h, sizeof(h));
    r300_cap_write(s, &st, sizeof(st));
    r300_cap_write(s, vb, sizeof(*vb) * nvtx);
    if (h.tex_bytes) {
        r300_cap_write(s, d->vram + tex_off, h.tex_bytes);
    }
    r300_cap_write(s, before, h.rect_bytes);
    r300_cap_write(s, after, h.rect_bytes);
    trace_ati_r350_3d_cap(s->cap_index, prim, nvtx, x0, y0, x1, y1,
                          h.tex_bytes);
    s->cap_index++;
    if (s->cap_fp) {
        fflush(s->cap_fp);
        if (s->cap_index >= s->cap_max) {
            fclose(s->cap_fp);
            s->cap_fp = NULL;
        }
    }
}

/* the two sizes a capture file's header has to agree on with its reader */
uint32_t ati_r350_cap_state_bytes(void)
{
    return sizeof(R300DrawState);
}

uint32_t ati_r350_cap_vtx_bytes(void)
{
    return sizeof(R300Vtx);
}

/*
 * Host-GPU offload (phase 2, milestone M2).
 *
 * Everything below runs only when the "gl" property opened a backend,
 * and the draw path tests nothing but s->gl_ctx, so a device left at
 * the default `gl=off` reaches r300_raster_prims() through exactly the
 * branch it always did.
 *
 * The shape is deliberately the same as the M1 replay harness, because
 * that is what was measured: this builds the same self-contained
 * request out of live device state that a capture record carried on
 * disk, hands it to the backend, and puts the rectangle back into VRAM
 * for the existing scanout to display. Anything the backend cannot
 * render falls back to the software rasterizer PER DRAW and is counted
 * -- a correct hybrid frame beats a complete GL frame that is wrong.
 */
/*
 * =====================================================================
 * GL-OWNED RENDER TARGET
 * =====================================================================
 *
 * The offload keeps the render target on the host GPU across draws.
 * That is where M3's speed comes from -- M2 uploaded the destination
 * rectangle twice and read it back for every single draw, 5.2 ms of the
 * 6.5 ms a full-screen draw cost on this host -- and it is also the one
 * place in this project where being wrong is SILENT. So the rules are
 * written down here, and every one of them is enforced by a call
 * somewhere rather than by a convention.
 *
 * THE INVARIANT. At any moment either no target is resident (`gl_res`
 * false, and the device behaves exactly as gl=off does), or:
 *
 *   - the resident target is the VRAM rectangle at `gl_res_off` with
 *     pitch `gl_res_pitch` under swapper xor `gl_res_xr`;
 *   - inside the SEEDED rectangle (gl_v*) the GPU copy is correct;
 *   - inside the DRAWN rectangle (gl_d*), which is always contained in
 *     the seeded one, the GPU copy is NEWER than VRAM;
 *   - outside the seeded rectangle the GPU holds nothing anyone may read.
 *
 * WHO MAY LOOK, AND WHAT THEY MUST DO FIRST. Everything that reads or
 * writes VRAM outside the 3D draw path calls ati_r350_gl_release() or
 * ati_r350_gl_touch() before doing so, which fetches the drawn
 * rectangle back and stops trusting the GPU copy:
 *
 *   scanout            ati_r350_update_display(), and the cursor's own
 *                      VRAM read, which runs off a timer
 *   the 2D engine      every blit, host-data push and scaler run
 *   the CP             ring and indirect-buffer fetches, write-backs
 *   MM_DATA            the register-indirect CPU window
 *   a 3D draw that     r300_run_prims(), before the software rasterizer
 *   falls back         touches the same VRAM
 *   a texture fetch    r300_run_prims(), when the sampled range overlaps
 *   reset, unrealize   ati_r350_reset_hold(), ati_r350_exit()
 *
 * THE ONE READER THAT CANNOT BE HOOKED, and what is done about it. The
 * guest CPU reaches VRAM through a plain RAM BAR: its loads are host
 * loads and no callback exists to intercept them, and its stores are
 * visible only after the fact through the dirty bitmap. So residency is
 * never allowed to survive a point at which the guest could execute an
 * instruction. In this device that point is exact rather than
 * approximate: a whole ring or indirect buffer is drained inside the
 * single guest store to CP_RB_WPTR or CP_IB_BUFSZ that kicked it, with
 * the BQL held throughout, so no guest instruction, no display refresh
 * and no monitor command can interleave with a burst of draws. Each of
 * those three entry points releases on the way out. A burst is
 * therefore the exact lifetime of a resident target, and the rule needs
 * no dirty-bitmap tracking to be correct.
 *
 * The cost of that conservatism is one seed per burst, and it is
 * measured rather than assumed: `gl-stats` reports the flush count and
 * the pixels moved each way, which is what says whether the batching is
 * working. If a guest turns out to submit one draw per burst the
 * numbers say so directly.
 *
 * gl=verify never lets a GPU pixel reach VRAM at all -- the drawn
 * rectangle is not recorded, so a flush has nothing to do, and the
 * software rasterizer's write to VRAM invalidates the GPU copy behind
 * it. A verify session's VRAM is byte-identical to a gl=off session's,
 * which is the property that makes it a measurement.
 */
#define R300_GL_SURF_MAX 4096

/* the VRAM bytes the seeded rectangle covers; empty when nothing is */
static bool r300_gl_span(ATIR350State *s, uint32_t *lo, uint32_t *hi)
{
    if (!s->gl_res || s->gl_vy1 <= s->gl_vy0) {
        return false;
    }
    *lo = s->gl_res_off + (uint32_t)s->gl_vy0 * s->gl_res_pitch;
    *hi = s->gl_res_off + (uint32_t)s->gl_vy1 * s->gl_res_pitch;
    return true;
}

/* the host GPU's newer bytes, back into VRAM, and marked for the display */
static void r300_gl_flush(ATIR350State *s)
{
    uint8_t *vram;
    int y, w, h;

    if (!s->gl_res || s->gl_dx1 <= s->gl_dx0 || s->gl_dy1 <= s->gl_dy0) {
        return;
    }
    w = s->gl_dx1 - s->gl_dx0;
    h = s->gl_dy1 - s->gl_dy0;
    vram = memory_region_get_ram_ptr(&s->vram);
    if (ati_r350_gl_fetch(s->gl_ctx, s->gl_dx0, s->gl_dy0, w, h,
                          vram + s->gl_res_off, s->gl_res_pitch,
                          s->gl_res_xr)) {
        for (y = s->gl_dy0; y < s->gl_dy1; y++) {
            uint64_t lo = s->gl_res_off + (uint32_t)y * s->gl_res_pitch +
                          (uint32_t)s->gl_dx0 * 4;
            uint64_t hi = (lo + (uint32_t)w * 4 + 7) & ~7ull;

            memory_region_set_dirty(&s->vram, lo & ~7ull, hi - (lo & ~7ull));
        }
        s->gl_flushes++;
        s->gl_flush_px += (uint64_t)w * h;
    }
    s->gl_dx0 = s->gl_dx1 = s->gl_dy0 = s->gl_dy1 = 0;
}

/* stop trusting the GPU copy, WITHOUT writing it back */
static void r300_gl_discard(ATIR350State *s)
{
    s->gl_dx0 = s->gl_dx1 = s->gl_dy0 = s->gl_dy1 = 0;
    s->gl_vx0 = s->gl_vx1 = s->gl_vy0 = s->gl_vy1 = 0;
}

/*
 * Decoded textures die with the target, and for the same reason: the
 * moments at which residency ends are exactly the moments at which
 * something outside the 3D engine may have changed VRAM.
 */
static void r300_gl_texdrop(ATIR350State *s)
{
    unsigned k;

    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        s->gl_tex[k].live = false;
        s->gl_tex[k].up = false;
    }
    s->gl_tex_any = false;
}

void ati_r350_gl_release(ATIR350State *s)
{
    r300_gl_texdrop(s);
    if (!s->gl_res) {
        return;
    }
    r300_gl_flush(s);
    r300_gl_discard(s);
    s->gl_res = false;
}

void ati_r350_gl_sync(ATIR350State *s, uint32_t off, uint32_t len)
{
    uint32_t lo, hi;
    unsigned k;

    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        if (s->gl_tex[k].live && off < s->gl_tex[k].off + s->gl_tex[k].len &&
            off + len > s->gl_tex[k].off) {
            s->gl_tex[k].live = false;
            s->gl_tex[k].up = false;
        }
    }
    if (!r300_gl_span(s, &lo, &hi) || off + len <= lo || off >= hi) {
        return;
    }
    ati_r350_gl_release(s);
}

/*
 * Make `d`'s colour buffer the resident target and make sure the GPU
 * holds the rectangle this draw is about to blend against. Growing the
 * backend texture throws its contents away, so anything drawn goes back
 * to VRAM before that happens.
 */
static bool r300_gl_bind(ATIR350State *s, const R300DrawState *d,
                         unsigned xr, int x0, int y0, int x1, int y1)
{
    bool lost = false;
    unsigned i;
    int ux0, uy0, ux1, uy1;

    if (x1 > R300_GL_SURF_MAX || y1 > R300_GL_SURF_MAX) {
        return false;
    }
    if (s->gl_res && (s->gl_res_off != d->dst_off ||
                      s->gl_res_pitch != d->dst_pitch ||
                      s->gl_res_xr != xr)) {
        ati_r350_gl_release(s);         /* a different target entirely */
    }
    if (x1 > s->gl_tex_w || y1 > s->gl_tex_h) {
        r300_gl_flush(s);               /* the grow below discards it */
    }
    if (!ati_r350_gl_target(s->gl_ctx, x1, y1, &lost)) {
        ati_r350_gl_release(s);
        return false;
    }
    if (lost) {
        s->gl_tex_w = MAX(x1, s->gl_tex_w);
        s->gl_tex_h = MAX(y1, s->gl_tex_h);
        r300_gl_discard(s);
    }
    for (i = 0; i < R300_GL_TEXCACHE; i++) {
        /* rendering into a range some cached texture came from */
        if (s->gl_tex[i].live &&
            d->dst_off + (uint32_t)y0 * d->dst_pitch <
            s->gl_tex[i].off + s->gl_tex[i].len &&
            d->dst_off + (uint32_t)y1 * d->dst_pitch > s->gl_tex[i].off) {
            s->gl_tex[i].live = false;
            s->gl_tex[i].up = false;
        }
    }
    s->gl_res = true;
    s->gl_res_off = d->dst_off;
    s->gl_res_pitch = d->dst_pitch;
    s->gl_res_xr = xr;

    if (s->gl_vx1 > s->gl_vx0 && x0 >= s->gl_vx0 && y0 >= s->gl_vy0 &&
        x1 <= s->gl_vx1 && y1 <= s->gl_vy1) {
        return true;                    /* the GPU already has it */
    }
    if (s->gl_vx1 <= s->gl_vx0) {
        ux0 = x0; uy0 = y0; ux1 = x1; uy1 = y1;
    } else {
        ux0 = MIN(s->gl_vx0, x0); uy0 = MIN(s->gl_vy0, y0);
        ux1 = MAX(s->gl_vx1, x1); uy1 = MAX(s->gl_vy1, y1);
    }
    /*
     * Seed the bounding rectangle MINUS what is already seeded, as up to
     * four strips. Everything drawn so far lies inside the seeded
     * rectangle, so none of the four can overwrite a GPU-newer pixel --
     * which is what makes growing the region safe without a flush.
     */
    {
        const uint8_t *base = d->vram + d->dst_off;
        struct { int x0, y0, x1, y1; } strip[4];
        unsigned k, n = 0;

        if (s->gl_vx1 <= s->gl_vx0) {
            strip[n].x0 = ux0; strip[n].y0 = uy0;
            strip[n].x1 = ux1; strip[n].y1 = uy1; n++;
        } else {
            strip[n].x0 = ux0; strip[n].y0 = uy0;
            strip[n].x1 = ux1; strip[n].y1 = s->gl_vy0; n++;
            strip[n].x0 = ux0; strip[n].y0 = s->gl_vy1;
            strip[n].x1 = ux1; strip[n].y1 = uy1; n++;
            strip[n].x0 = ux0; strip[n].y0 = s->gl_vy0;
            strip[n].x1 = s->gl_vx0; strip[n].y1 = s->gl_vy1; n++;
            strip[n].x0 = s->gl_vx1; strip[n].y0 = s->gl_vy0;
            strip[n].x1 = ux1; strip[n].y1 = s->gl_vy1; n++;
        }
        for (k = 0; k < n; k++) {
            int sw = strip[k].x1 - strip[k].x0;
            int sh = strip[k].y1 - strip[k].y0;

            if (sw <= 0 || sh <= 0) {
                continue;
            }
            if (!ati_r350_gl_seed(s->gl_ctx, strip[k].x0, strip[k].y0,
                                  sw, sh, base, d->dst_pitch, xr)) {
                ati_r350_gl_release(s);
                return false;
            }
            s->gl_seed_px += (uint64_t)sw * sh;
        }
    }
    s->gl_vx0 = ux0; s->gl_vy0 = uy0;
    s->gl_vx1 = ux1; s->gl_vy1 = uy1;
    return true;
}

static bool r300_gl_fallback(ATIR350State *s, ATIR350GlFallback why,
                             unsigned prim, unsigned nvtx)
{
    s->gl_fb[why][prim & (R350_GAP_SLOTS - 1)]++;
    trace_ati_r350_3d_gl_fallback(ati_r350_gl_fb_name(why), prim, nvtx);
    return false;
}

/*
 * The triangles r300_raster_prims() assembles, as an index list. Kept
 * beside that switch and in the same order on purpose: if the two ever
 * disagree the offload draws different geometry from the oracle, which
 * is the one divergence gl=verify could not attribute.
 *
 * Point lists, line lists, strips and loops are deliberately absent.
 * A point SPRITE is a whole per-vertex quad expansion driven by
 * registers the request does not carry, and a line is expanded across
 * its direction; both are milestone M3 work at the earliest. They fall
 * back and are counted.
 */
static unsigned r300_gl_tris(unsigned prim, unsigned nvtx, unsigned *idx,
                             unsigned max)
{
    unsigned n = 0, i;

#define R300_GL_EMIT(a, b, c)                           \
    do {                                                \
        if (n + 3 > max) {                              \
            return n / 3;                               \
        }                                               \
        idx[n++] = (a); idx[n++] = (b); idx[n++] = (c); \
    } while (0)

    switch (prim) {
    case 4: case 7:
        for (i = 0; i + 3 <= nvtx; i += 3) {
            R300_GL_EMIT(i, i + 1, i + 2);
        }
        break;
    case 5: case 15:
        for (i = 2; i < nvtx; i++) {
            R300_GL_EMIT(0, i - 1, i);
        }
        break;
    case 6:
        for (i = 2; i < nvtx; i++) {
            R300_GL_EMIT(i - 2, i - 1, i);
        }
        break;
    case 13:
        for (i = 0; i + 4 <= nvtx; i += 4) {
            R300_GL_EMIT(i, i + 1, i + 2);
            R300_GL_EMIT(i, i + 2, i + 3);
        }
        break;
    case 14:
        for (i = 2; i + 2 <= nvtx; i += 2) {
            R300_GL_EMIT(i - 2, i - 1, i + 1);
            R300_GL_EMIT(i - 2, i + 1, i);
        }
        break;
    default:
        return 0;
    }
#undef R300_GL_EMIT
    return n / 3;
}

/*
 * A rectangle-list draw implies a fourth corner that is not in the
 * vertex buffer, so it cannot be expressed as an index list over vb[].
 * The vertices are synthesised into a private array instead, which is
 * why prim 8 is handled separately rather than inside r300_gl_tris().
 */
static unsigned r300_gl_rect_list(const R300Vtx *vb, unsigned nvtx,
                                  R300Vtx *out, unsigned max)
{
    unsigned n = 0, i;

    for (i = 0; i + 3 <= nvtx && n + 6 <= max; i += 3) {
        R300Vtx v3 = vb[i + 2];

        v3.x = vb[i + 1].x + vb[i + 2].x - vb[i].x;
        v3.y = vb[i + 1].y + vb[i + 2].y - vb[i].y;
        v3.s = vb[i + 1].s + vb[i + 2].s - vb[i].s;
        v3.t = vb[i + 1].t + vb[i + 2].t - vb[i].t;
        out[n++] = vb[i]; out[n++] = vb[i + 1]; out[n++] = vb[i + 2];
        out[n++] = vb[i + 1]; out[n++] = v3; out[n++] = vb[i + 2];
    }
    return n / 3;
}

/*
 * Does this draw blend against pixels it has already written itself?
 *
 * The backend's shader reads the destination it is blending against as
 * a texture, seeded once. That reproduces the software rasterizer's
 * arithmetic exactly -- truncating pack included -- but only while no
 * two primitives of the draw cover the same pixel. The software
 * rasterizer paints them in order and each blends against what the
 * previous one left; a single pass blends both against the ORIGINAL.
 * Flurry.saver's additive ribbons cross themselves inside one draw and
 * differ by up to 229/255 because of it, measured by gl=verify.
 *
 * M2 made those draws fall back to the software rasterizer, which cost
 * the offload most of its speed: the same binary without the fallback
 * measured 14.96 fps against 10.23 -- and rendered Flurry wrong. M3
 * wins that back by ORDERING them instead. The triangles are
 * partitioned into passes such that no two in a pass overlap and any
 * overlapping pair lands in the device's own order, and the backend
 * refreshes the blend's source between passes with a GPU-side copy.
 * The result is the software path's ordering, computed on the GPU.
 *
 * The test is exact rather than a bounding box, because a bounding box
 * would put every quad of Chess's board in its own pass -- 1048 blended
 * quad-strip draws the offline harness measures as correct through GL
 * to a maximum channel delta of 1. Two triangles are separated when
 * some edge normal projects them to intervals that do not overlap in a
 * positive length, so a shared edge (the two halves of a quad, or two
 * quads of a strip) reads as disjoint, which is what keeps an ordinary
 * mesh in a single pass.
 *
 * Unblended draws need none of this, and neither do blended ones whose
 * READ_ENABLE is clear: GL updates the framebuffer in primitive order,
 * so a later primitive simply overwrites an earlier one.
 */

/*
 * The separating-axis test, one axis. Two convex shapes are disjoint
 * exactly when some axis projects them to intervals that do not
 * overlap, and for two triangles it is enough to try the six edge
 * normals. "Do not overlap" is taken as touching-counts-as-disjoint, so
 * a shared edge separates -- which is what a mesh is made of.
 */
static bool r300_axis_sep(float nx, float ny, const R300Vtx * const a[3],
                          const R300Vtx * const b[3])
{
    float alo, ahi, blo, bhi;
    unsigned i;

    if (nx == 0.0f && ny == 0.0f) {
        return false;               /* a degenerate edge separates nothing */
    }
    alo = ahi = nx * a[0]->x + ny * a[0]->y;
    blo = bhi = nx * b[0]->x + ny * b[0]->y;
    for (i = 1; i < 3; i++) {
        float va = nx * a[i]->x + ny * a[i]->y;
        float vb = nx * b[i]->x + ny * b[i]->y;

        alo = MIN(alo, va); ahi = MAX(ahi, va);
        blo = MIN(blo, vb); bhi = MAX(bhi, vb);
    }
    return ahi <= blo || bhi <= alo;
}

static bool r300_tris_overlap(const R300Vtx * const a[3],
                              const R300Vtx * const b[3])
{
    unsigned i;

    for (i = 0; i < 3; i++) {
        const R300Vtx *p = a[i], *q = a[(i + 1) % 3];
        const R300Vtx *r = b[i], *t = b[(i + 1) % 3];

        if (r300_axis_sep(-(q->y - p->y), q->x - p->x, a, b) ||
            r300_axis_sep(-(t->y - r->y), t->x - r->x, a, b)) {
            return false;
        }
    }
    return true;
}

/*
 * Assign each triangle the earliest pass that keeps the device's order:
 * one later than the last earlier triangle it overlaps, or pass 0 if it
 * overlaps none. That is correct by construction in both directions --
 * two triangles in the same pass never cover a common pixel, and an
 * overlapping pair is always drawn earlier-first with the later one
 * blending against a destination that already holds the earlier one.
 *
 * The inner loop runs backwards and skips any triangle already in a
 * pass no later than the one this triangle has reached, because such a
 * triangle cannot push it further; and a bounding-box test screens the
 * exact one. A mesh therefore costs one bounding-box comparison per
 * pair and stays in a single pass.
 *
 * Returns the number of passes, or 0 when the draw needs more than
 * R300_GL_PASS_MAX -- the caller falls back and counts it.
 */
static unsigned r300_gl_passes(ATIR350State *s, const R300Vtx *vb,
                               const unsigned *idx, unsigned ntri)
{
    unsigned i, j, n = 1;

    for (i = 0; i < ntri; i++) {
        const R300Vtx *a[3] = { &vb[idx[i * 3]], &vb[idx[i * 3 + 1]],
                                &vb[idx[i * 3 + 2]] };
        float *bb = s->gl_bbox[i];

        bb[0] = MIN(a[0]->x, MIN(a[1]->x, a[2]->x));
        bb[1] = MIN(a[0]->y, MIN(a[1]->y, a[2]->y));
        bb[2] = MAX(a[0]->x, MAX(a[1]->x, a[2]->x));
        bb[3] = MAX(a[0]->y, MAX(a[1]->y, a[2]->y));
        s->gl_pass[i] = 0;
        for (j = i; j-- > 0;) {
            const float *cb = s->gl_bbox[j];
            const R300Vtx *b[3];

            if (s->gl_pass[j] < s->gl_pass[i]) {
                continue;
            }
            if (bb[2] <= cb[0] || cb[2] <= bb[0] ||
                bb[3] <= cb[1] || cb[3] <= bb[1]) {
                continue;
            }
            b[0] = &vb[idx[j * 3]];
            b[1] = &vb[idx[j * 3 + 1]];
            b[2] = &vb[idx[j * 3 + 2]];
            if (!r300_tris_overlap(a, b)) {
                continue;
            }
            if (s->gl_pass[j] + 1u >= R300_GL_PASS_MAX) {
                return 0;
            }
            s->gl_pass[i] = s->gl_pass[j] + 1;
        }
        n = MAX(n, s->gl_pass[i] + 1u);
    }
    return n;
}

/* the triangles in pass order, and where in the vertex array each begins */
static void r300_gl_order(ATIR350State *s, unsigned ntri, unsigned npass)
{
    unsigned p, i, n = 0;

    for (p = 0; p < npass; p++) {
        s->gl_pass_first[p] = n * 3;
        for (i = 0; i < ntri; i++) {
            if (s->gl_pass[i] == p) {
                s->gl_order[n++] = i;
            }
        }
    }
    s->gl_pass_first[npass] = n * 3;
}

/*
 * RE_CLIPRECT_CNTL is a 16-entry truth table indexed by which of the
 * four clip rectangles contain the pixel, and in general it has no GL
 * equivalent. The compositor does not use it in general, though: it
 * clips each window-content draw with ONE rectangle, and "scissor AND
 * one rect" is still a rectangle, which glScissor expresses exactly.
 * The no-clip rule and the four single-rect rules fold in; a genuine
 * truth table falls back rather than being approximated.
 */
static bool r300_gl_clip(const R300DrawState *d, int *ex0, int *ey0,
                         int *ex1, int *ey1)
{
    int r = -1;

    switch (d->clip_rule) {
    case 0xffff:
        break;
    case 0xaaaa:
        r = 0;
        break;
    case 0xcccc:
        r = 1;
        break;
    case 0xf0f0:
        r = 2;
        break;
    case 0xff00:
        r = 3;
        break;
    default:
        return false;
    }
    *ex0 = MAX(d->sc_x0, 0);
    *ey0 = MAX(d->sc_y0, 0);
    *ex1 = MIN(d->sc_x1 + 1, 8191);     /* the scissor's edges are inclusive */
    *ey1 = MIN(d->sc_y1 + 1, 8191);
    if (r >= 0) {
        /* a cliprect's bottom-right is exclusive */
        *ex0 = MAX(*ex0, d->cr[r][0]);
        *ey0 = MAX(*ey0, d->cr[r][1]);
        *ex1 = MIN(*ex1, d->cr[r][2]);
        *ey1 = MIN(*ey1, d->cr[r][3]);
    }
    return true;
}

/*
 * The destination rectangle, as RGBA8 the way GL wants it. The swapper
 * is a byte-lane permutation inside an aligned dword and the record's
 * xor is uniform over the region, so a row reads exactly as
 * r300_ld32() reads it -- lane j of the dword is byte (j ^ xr).
 */
static void r300_gl_rd_rect(const R300DrawState *d, unsigned xr,
                            int x0, int y0, int w, int h, uint8_t *rgba)
{
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *p = d->vram + d->dst_off +
                           (uint32_t)(y0 + y) * d->dst_pitch +
                           (uint32_t)x0 * 4;
        uint8_t *o = rgba + (size_t)y * w * 4;

        for (x = 0; x < w; x++, p += 4, o += 4) {
            o[0] = p[2 ^ xr];           /* R */
            o[1] = p[1 ^ xr];           /* G */
            o[2] = p[0 ^ xr];           /* B */
            o[3] = p[3 ^ xr];           /* A */
        }
    }
}

/*
 * The texture, decoded by the device's OWN r300_sample_tex() and
 * r300_texel_chan(): format decode, bytes per texel, the aperture
 * swapper and the four-way TX_FORMAT1 component select all included.
 * Both paths therefore share the sampler exactly, and what gl=verify
 * measures is the rasterization, not the texture unit.
 *
 * The decoded result is CACHED (milestone M3). The cache key is every
 * piece of state the decode reads -- the resolved VRAM offset, the
 * width, height and pitch, the bytes per texel, the format code, all
 * four component selects and the aperture swapper's xor -- so a hit is
 * a hit on the same bytes decoded the same way, not on an address. What
 * makes it safe is that an entry lives exactly as long as the resident
 * render target does and dies at the same moments, which is the same
 * argument written out at "GL-OWNED RENDER TARGET" above: inside a
 * command-processor burst nothing else can touch VRAM without saying
 * so, and a burst is where the repetition is. A texture in system
 * memory rather than VRAM is not cached at all, because there is no
 * range to invalidate on.
 */
#define R300_GL_TEX_MAX (1024 * 1024)

static void r300_gl_decode_tex(ATIR350State *s, const R300DrawState *d,
                               uint8_t *rgba)
{
    int tx, ty;

    for (ty = 0; ty < d->tex_h; ty++) {
        for (tx = 0; tx < d->tex_w; tx++) {
            uint32_t texel = r300_sample_tex(s, d, tx, ty);
            uint8_t *p = rgba + ((size_t)ty * d->tex_w + tx) * 4;

            p[0] = (uint8_t)(r300_texel_chan(d, texel, 1) * 255.0f + 0.5f);
            p[1] = (uint8_t)(r300_texel_chan(d, texel, 2) * 255.0f + 0.5f);
            p[2] = (uint8_t)(r300_texel_chan(d, texel, 3) * 255.0f + 0.5f);
            p[3] = (uint8_t)(r300_texel_chan(d, texel, 0) * 255.0f + 0.5f);
        }
    }
}

/* everything the decode above depends on, and nothing else */
static bool r300_gl_tex_same(const ATIR350State *s, unsigned k,
                             const R300DrawState *d, uint32_t off,
                             uint32_t len, unsigned xr)
{
    return s->gl_tex[k].live &&
           s->gl_tex[k].off == off && s->gl_tex[k].len == len &&
           s->gl_tex[k].pitch == d->tex_pitch &&
           s->gl_tex[k].bpp == d->tex_bpp &&
           s->gl_tex[k].code == d->tex_code &&
           s->gl_tex[k].w == d->tex_w && s->gl_tex[k].h == d->tex_h &&
           s->gl_tex[k].xr == xr &&
           s->gl_tex[k].sel[0] == d->tex_sel[0] &&
           s->gl_tex[k].sel[1] == d->tex_sel[1] &&
           s->gl_tex[k].sel[2] == d->tex_sel[2] &&
           s->gl_tex[k].sel[3] == d->tex_sel[3];
}

/*
 * The decoded texture for this draw: from the cache when the same bytes
 * were decoded the same way inside this burst, and decoded into the
 * least recently used entry otherwise. A texture that does not resolve
 * to a uniformly swapped range of VRAM is decoded into the scratch
 * buffer and not cached -- there would be no range to invalidate it on.
 */
static const uint8_t *r300_gl_texture(ATIR350State *s, const R300DrawState *d,
                                      unsigned *slot, int *fresh)
{
    uint32_t off, len;
    unsigned k, victim = 0, xr = 0;
    size_t need = (size_t)d->tex_w * d->tex_h * 4;

    *slot = R350_GL_TEXSLOTS;           /* the scratch: uploaded every time */
    *fresh = 1;

    len = (uint32_t)d->tex_h * d->tex_pitch;
    if ((size_t)d->tex_w * d->tex_h > R300_GL_TEXCACHE_MAX ||
        !d->tex_pitch || !len ||
        !ati_r350_mc_to_vram(s, d->tex_off, &off) ||
        (uint64_t)off + len > ATI_R350_VRAM_SIZE ||
        !r300_cap_xor(s, off, len, &xr)) {
        s->gl_tex_miss++;
        r300_gl_decode_tex(s, d, s->gl_texbuf);
        return s->gl_texbuf;
    }
    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        if (r300_gl_tex_same(s, k, d, off, len, xr)) {
            s->gl_tex[k].used = ++s->gl_tex_seq;
            s->gl_tex_hit++;
            *slot = k;
            /*
             * The backend still holds it uploaded unless the slot was
             * given to a different texture since; `up` is what says so,
             * and it is cleared wherever an entry is.
             */
            *fresh = !s->gl_tex[k].up;
            s->gl_tex[k].up = true;
            return s->gl_tex[k].rgba;
        }
        if (!s->gl_tex[k].live) {
            victim = k;
        } else if (s->gl_tex[victim].live &&
                   s->gl_tex[k].used < s->gl_tex[victim].used) {
            victim = k;
        }
    }
    s->gl_tex_miss++;
    if (need > s->gl_tex[victim].sz) {
        s->gl_tex[victim].rgba = g_realloc(s->gl_tex[victim].rgba, need);
        s->gl_tex[victim].sz = need;
    }
    r300_gl_decode_tex(s, d, s->gl_tex[victim].rgba);
    s->gl_tex[victim].off = off;
    s->gl_tex[victim].len = len;
    s->gl_tex[victim].pitch = d->tex_pitch;
    s->gl_tex[victim].bpp = d->tex_bpp;
    s->gl_tex[victim].code = d->tex_code;
    s->gl_tex[victim].w = d->tex_w;
    s->gl_tex[victim].h = d->tex_h;
    s->gl_tex[victim].xr = xr;
    for (k = 0; k < 4; k++) {
        s->gl_tex[victim].sel[k] = d->tex_sel[k];
    }
    s->gl_tex[victim].used = ++s->gl_tex_seq;
    s->gl_tex[victim].live = true;
    s->gl_tex[victim].up = true;
    s->gl_tex_any = true;
    *slot = victim;
    *fresh = 1;
    return s->gl_tex[victim].rgba;
}

/* one vertex of the expanded triangle list, carrying its whole triangle */
static void r300_gl_vtx(float *v, const R300Vtx *me, const R300Vtx *t0,
                        const R300Vtx *t1, const R300Vtx *t2, float inv)
{
    v[0] = me->x; v[1] = me->y;
    v[2] = me->r; v[3] = me->g; v[4] = me->b; v[5] = me->a;
    v[6] = me->s; v[7] = me->t;
    v[8] = t0->x;  v[9] = t0->y;
    v[10] = t1->x; v[11] = t1->y;
    v[12] = t2->x; v[13] = t2->y;
    v[14] = t0->r; v[15] = t0->g; v[16] = t0->b; v[17] = t0->a;
    v[18] = t1->r; v[19] = t1->g; v[20] = t1->b; v[21] = t1->a;
    v[22] = t2->r; v[23] = t2->g; v[24] = t2->b; v[25] = t2->a;
    v[26] = t0->s; v[27] = t0->t;
    v[28] = t1->s; v[29] = t1->t;
    v[30] = t2->s; v[31] = t2->t;
    v[32] = inv;
}

/*
 * gl=verify's scoring, over one rectangle.
 *
 * The offline M1 harness scores a draw by replaying the rasterizer's
 * own acceptance test to decide which pixels are INTERIOR and which are
 * EDGE, because the two classes have very different criteria: an
 * interior pixel is arithmetic and must agree to 1/255, an edge pixel
 * is a coverage tie that GL and a software fill rule are allowed to
 * resolve differently. Doing that here would mean rasterizing a third
 * time.
 *
 * It is not necessary. The record already holds the destination BEFORE
 * the draw, so "did this path write this pixel" is readable from the
 * data: a pixel where exactly one of the two paths changed the
 * destination is a COVERAGE disagreement -- the edge class by
 * definition -- and a pixel both of them changed (or neither) is a
 * VALUE disagreement, the interior class. No rasterization, and the
 * classification is the one the criteria are actually about.
 *
 * Conservative in the safe direction: a path that writes a pixel the
 * value it already held reads as "did not write", which can only move a
 * pixel out of the coverage class and into the stricter one.
 *
 * One class of tie this cannot see, and it is counted separately rather
 * than left to be argued about. Inside a MESH -- a strip, a fan, or a
 * quad list of more than one quad -- two adjacent triangles share an
 * edge, and a pixel exactly on it is awarded to one of them by the fill
 * rule. Both paths write such a pixel, so the coverage test above puts
 * it in the value class, but the two triangles can carry completely
 * different texture coordinates (Chess's board samples a different part
 * of its wood texture per square), so the disagreement is full-range.
 * `gl_v_mesh` is how many of the value-class disagreements come from a
 * draw with more than two triangles; the offline harness, which
 * replays the acceptance test and can see the tie, classifies exactly
 * those as EDGE.
 */
static void r300_gl_verify(ATIR350State *s, const R300DrawState *d,
                           unsigned prim, unsigned ntri, unsigned xr,
                           int x0, int y0, int w, int h)
{
    size_t npx = (size_t)w * h, i;
    unsigned maxd = 0;
    uint64_t diff = 0;

    r300_gl_rd_rect(d, xr, x0, y0, w, h, s->gl_sw);
    for (i = 0; i < npx; i++) {
        const uint8_t *g = s->gl_out + i * 4;
        const uint8_t *o = s->gl_sw + i * 4;
        const uint8_t *b = s->gl_before + i * 4;
        unsigned dmax = 0, c;
        bool gw = false, ow = false;

        for (c = 0; c < 4; c++) {
            unsigned k = g[c] > o[c] ? g[c] - o[c] : o[c] - g[c];

            dmax = MAX(dmax, k);
            gw |= g[c] != b[c];
            ow |= o[c] != b[c];
        }
        maxd = MAX(maxd, dmax);
        diff += dmax != 0;
        if (gw != ow) {
            /* one path covered this pixel and the other did not */
            s->gl_v_cover_px++;
            if (dmax) {
                s->gl_v_cover++;
            }
            continue;
        }
        s->gl_v_hist[dmax == 0 ? 0 : dmax == 1 ? 1 : dmax <= 4 ? 2 : 3]++;
        s->gl_v_vmax = MAX(s->gl_v_vmax, dmax);
        if (dmax > 1 && ntri > 2) {
            s->gl_v_mesh++;
        }
    }
    s->gl_v_px += npx;
    s->gl_v_draws++;
    s->gl_v_max = MAX(s->gl_v_max, maxd);
    if (maxd > 1) {
        s->gl_v_bad++;
    }
    trace_ati_r350_3d_gl_verify(prim, (uint32_t)npx, (uint32_t)diff, maxd,
                                x0, y0, x0 + w, y0 + h);
}

/*
 * Returns true when the backend rendered the draw and the caller must
 * not rasterize it again. In gl=verify the software rasterizer HAS been
 * run by the time this returns true -- its result is what stays in
 * VRAM, and the GL result is only compared against it.
 */
static bool r300_gl_prims(ATIR350State *s, R300DrawState *d,
                          const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    g_autofree R300Vtx *rect_vb = NULL;
    g_autofree unsigned *idx = NULL;
    const R300Vtx *gvb = vb;
    R350GlReq req = { 0 };
    const uint8_t *texbuf = NULL;
    unsigned ntri = 0, npass = 1, i, xr = 0, texslot = R350_GL_TEXSLOTS;
    int texfresh = 1;
    int x0, y0, x1, y1, w, h;
    size_t rect_sz, texels = 0;

    if (d->resolve) {
        return r300_gl_fallback(s, R350_GLF_RESOLVE, prim, nvtx);
    }
    /*
     * Assemble first: a primitive this path does not know is the
     * commonest fallback and the cheapest one to detect. `vb`/`nvtx`
     * stay the draw as the guest issued it -- only `gvb`/`idx` are the
     * expansion GL is handed.
     */
    if (prim == 8) {
        rect_vb = g_new(R300Vtx, (size_t)nvtx * 2 + 6);
        ntri = r300_gl_rect_list(vb, nvtx, rect_vb, nvtx * 2 + 6);
        idx = g_new(unsigned, (size_t)ntri * 3 + 3);
        for (i = 0; i < ntri * 3; i++) {
            idx[i] = i;
        }
        gvb = rect_vb;
    } else {
        idx = g_new(unsigned, (size_t)nvtx * 3 + 3);
        ntri = r300_gl_tris(prim, nvtx, idx, nvtx * 3 + 3);
    }
    if (!ntri) {
        return r300_gl_fallback(s, R350_GLF_PRIM, prim, nvtx);
    }
    if (!r300_gl_clip(d, &req.sx0, &req.sy0, &req.sx1, &req.sy1)) {
        return r300_gl_fallback(s, R350_GLF_CLIPRULE, prim, nvtx);
    }
    if (d->blend && d->blend_read && ntri > 1) {
        if (ntri > R300_GL_TRI_MAX) {
            return r300_gl_fallback(s, R350_GLF_SELFBLEND, prim, nvtx);
        }
        npass = r300_gl_passes(s, gvb, idx, ntri);
        if (!npass) {
            return r300_gl_fallback(s, R350_GLF_SELFBLEND, prim, nvtx);
        }
        if (npass > 1) {
            r300_gl_order(s, ntri, npass);
            s->gl_multipass++;
            s->gl_passes += npass;
        }
    }
    /*
     * The rectangle, and the swapper over it, from the same two helpers
     * the draw capture uses -- the bounding box widened by a pixel and
     * clipped exactly the way r300_raster_tri() clips its scan.
     */
    if ((d->dst_off | d->dst_pitch) & 3) {
        return r300_gl_fallback(s, R350_GLF_ALIGN, prim, nvtx);
    }
    if (!r300_cap_rect(s, d, vb, nvtx, prim, &x0, &y0, &x1, &y1)) {
        /*
         * Empty after the scissor, off-screen, or not a finite
         * rectangle at all. The software path paints nothing for these
         * either, so this is the one fallback that costs nothing.
         */
        return r300_gl_fallback(s, R350_GLF_RECT, prim, nvtx);
    }
    w = x1 - x0;
    h = y1 - y0;
    /*
     * r300_cap_rect() trims rows off the bottom to keep a rectangle
     * inside VRAM. A capture may be short; an offload may not lose
     * pixels the software path would have drawn, so a rectangle that
     * comes anywhere near the end of VRAM is refused outright rather
     * than silently rendered short.
     */
    if ((uint64_t)d->dst_off + (uint64_t)y1 * d->dst_pitch +
        (uint64_t)x1 * 4 > ATI_R350_VRAM_SIZE) {
        return r300_gl_fallback(s, R350_GLF_VRAMEND, prim, nvtx);
    }
    if (!r300_cap_xor(s, d->dst_off + (uint32_t)y0 * d->dst_pitch,
                      (uint32_t)(y1 - 1 - y0) * d->dst_pitch +
                      (uint32_t)x1 * 4, &xr)) {
        return r300_gl_fallback(s, R350_GLF_XOR, prim, nvtx);
    }
    if (d->textured) {
        texels = (size_t)d->tex_w * d->tex_h;
        if (!texels || texels > R300_GL_TEX_MAX) {
            return r300_gl_fallback(s, R350_GLF_TEXTURE, prim, nvtx);
        }
    }

    /* scratch, grown on demand and reused for the life of the device */
    rect_sz = (size_t)w * h * 4;
    if (rect_sz > s->gl_rect_sz) {
        s->gl_before = g_realloc(s->gl_before, rect_sz);
        s->gl_out = g_realloc(s->gl_out, rect_sz);
        s->gl_sw = g_realloc(s->gl_sw, rect_sz);
        s->gl_rect_sz = rect_sz;
    }
    if (texels * 4 > s->gl_texbuf_sz) {
        s->gl_texbuf = g_realloc(s->gl_texbuf, texels * 4);
        s->gl_texbuf_sz = texels * 4;
    }
    if ((size_t)ntri * 3 * R350_GL_VSTRIDE * sizeof(float) > s->gl_verts_sz) {
        s->gl_verts_sz = (size_t)ntri * 3 * R350_GL_VSTRIDE * sizeof(float);
        s->gl_verts = g_realloc(s->gl_verts, s->gl_verts_sz);
    }

    for (i = 0; i < ntri; i++) {
        /* pass order when there is one, submission order otherwise */
        unsigned src = (npass > 1 ? s->gl_order[i] : i) * 3;
        const R300Vtx *t0 = &gvb[idx[src + 0]];
        const R300Vtx *t1 = &gvb[idx[src + 1]];
        const R300Vtx *t2 = &gvb[idx[src + 2]];
        float inv = 1.0f / r300_edge(t0, t1, t2->x, t2->y);
        unsigned k;

        for (k = 0; k < 3; k++) {
            r300_gl_vtx(s->gl_verts + (size_t)(i * 3 + k) * R350_GL_VSTRIDE,
                        &gvb[idx[src + k]], t0, t1, t2, inv);
        }
    }
    if (d->textured) {
        texbuf = r300_gl_texture(s, d, &texslot, &texfresh);
    }
    /*
     * The target becomes resident here, and this is the last point at
     * which the draw can still be refused: everything above it is pure
     * inspection, nothing has been seeded, and a fallback costs nothing.
     */
    if (!r300_gl_bind(s, d, xr, x0, y0, x1, y1)) {
        return r300_gl_fallback(s, R350_GLF_SURFACE, prim, nvtx);
    }

    req.x0 = x0; req.y0 = y0; req.w = w; req.h = h;
    req.surf_w = s->gl_tex_w;
    req.surf_h = s->gl_tex_h;
    req.verts = s->gl_verts;
    req.nvert = ntri * 3;
    req.pass = npass > 1 ? s->gl_pass_first : NULL;
    req.npass = npass;
    req.tex = d->textured ? texbuf : NULL;
    req.tex_slot = texslot;
    req.tex_fresh = texfresh;
    req.tex_w = d->tex_w;
    req.tex_h = d->tex_h;
    req.clamp_s = d->clamp_s;
    req.clamp_t = d->clamp_t;
    req.textured = d->textured;
    req.wmask = d->wmask;
    req.alpha_test = d->alpha_test;
    req.af_func = d->af_func;
    req.af_ref = d->af_ref;
    req.discard = d->discard;
    req.blend = d->blend;
    req.blend_read = d->blend_read;
    req.src_factor = d->src_factor;
    req.dst_factor = d->dst_factor;
    req.comb_fcn = d->comb_fcn;
    req.a_src_factor = d->a_src_factor;
    req.a_dst_factor = d->a_dst_factor;
    req.a_comb_fcn = d->a_comb_fcn;
    req.k_r = d->k_r; req.k_g = d->k_g;
    req.k_b = d->k_b; req.k_a = d->k_a;
    /* only verify wants the pixels on the host; see the coherency block */
    req.out = s->gl_mode == R350_GL_VERIFY ? s->gl_out : NULL;

    if (s->gl_mode == R350_GL_VERIFY) {
        r300_gl_rd_rect(d, xr, x0, y0, w, h, s->gl_before);
    }
    if (!ati_r350_gl_draw(s->gl_ctx, &req)) {
        ati_r350_gl_release(s);
        return r300_gl_fallback(s, R350_GLF_BACKEND, prim, nvtx);
    }
    s->gl_drawn++;
    trace_ati_r350_3d_gl(prim, nvtx, x0, y0, x1, y1, ntri);

    if (s->gl_mode == R350_GL_VERIFY) {
        /*
         * Both paths ran; the software one is what lands. The offload is
         * being measured here, not trusted, so VRAM must come out of a
         * verify session byte-identical to a gl=off session -- which is
         * why the drawn rectangle is never recorded and the GPU copy is
         * dropped, unwritten, the moment the rasterizer changes VRAM
         * underneath it.
         */
        r300_raster_prims(s, d, vb, nvtx, prim);
        r300_gl_verify(s, d, prim, ntri, xr, x0, y0, w, h);
        r300_gl_discard(s);
        return true;
    }
    /* the GPU now holds bytes VRAM does not, over this rectangle */
    if (s->gl_dx1 <= s->gl_dx0) {
        s->gl_dx0 = x0; s->gl_dy0 = y0; s->gl_dx1 = x1; s->gl_dy1 = y1;
    } else {
        s->gl_dx0 = MIN(s->gl_dx0, x0); s->gl_dy0 = MIN(s->gl_dy0, y0);
        s->gl_dx1 = MAX(s->gl_dx1, x1); s->gl_dy1 = MAX(s->gl_dy1, y1);
    }
    return true;
}

/*
 * Where this draw actually lands, straight from the transformed
 * vertices. Offline replay of a command stream has to reconstruct this
 * and can get it wrong -- reading it from the engine is the ground
 * truth to check such a reconstruction against.
 *
 * Emitted from r300_run_prims() rather than from the rasterizer, which
 * is where it used to live: once a draw can be rendered by the GL
 * backend instead, a trace inside the software path silently stops
 * describing most of a frame. It reported 243 of Chess's 1648 board
 * draws under gl=on before it was moved -- exactly the fallbacks -- and
 * the standing rect baselines are measured with it.
 */
static void r300_trace_rect(ATIR350State *s, const R300DrawState *d,
                            const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    float x0 = vb[0].x, y0 = vb[0].y, x1 = x0, y1 = y0;
    float s0 = vb[0].s, t0 = vb[0].t, s1 = s0, t1 = t0;
    unsigned i;

    for (i = 1; i < nvtx; i++) {
        x0 = MIN(x0, vb[i].x); x1 = MAX(x1, vb[i].x);
        y0 = MIN(y0, vb[i].y); y1 = MAX(y1, vb[i].y);
        s0 = MIN(s0, vb[i].s); s1 = MAX(s1, vb[i].s);
        t0 = MIN(t0, vb[i].t); t1 = MAX(t1, vb[i].t);
    }
    if (prim == 1) {
        /* a point sprite covers RE_POINTSIZE around its centre */
        uint32_t psize = s->regs[R300_RE_POINTSIZE >> 2];
        float hw = ((psize >> 16) & 0xffff) / 12.0f;
        float hh = (psize & 0xffff) / 12.0f;

        x0 -= hw; x1 += hw;
        y0 -= hh; y1 += hh;
    }
    trace_ati_r350_3d_rect(d->dst_off, (int)x0, (int)y0, (int)x1, (int)y1,
                           (int)s0, (int)t0, (int)s1, (int)t1);
}

static void r300_run_prims(ATIR350State *s, R300DrawState *d,
                           const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    if (nvtx && trace_event_get_state_backends(TRACE_ATI_R350_3D_RECT)) {
        r300_trace_rect(s, d, vb, nvtx, prim);
    }
    /*
     * A draw that samples the resident target as a texture reads it out
     * of VRAM, and the resolve path reads the colour buffer itself.
     * Both are ordinary VRAM readers as far as the rules go: give the
     * target back first. Chess's compositor does exactly this -- it
     * samples the resolve buffer the board was rendered into.
     */
    if (unlikely(s->gl_res)) {
        if (d->textured && d->tex_pitch && d->tex_h > 0) {
            uint32_t toff;

            if (ati_r350_mc_to_vram(s, d->tex_off, &toff)) {
                ati_r350_gl_sync(s, toff,
                                 (uint32_t)d->tex_h * d->tex_pitch);
            }
        }
        if (d->resolve) {
            ati_r350_gl_release(s);
        }
    }
    if (s->cap_fp && nvtx) {
        ati_r350_gl_release(s);
        r300_cap_draw(s, d, vb, nvtx, prim);
    } else if (s->gl_ctx && nvtx &&
               r300_gl_prims(s, d, vb, nvtx, prim)) {
        /* the backend rendered it (and, under verify, so did we) */
    } else {
        /* the software rasterizer writes VRAM the GPU copy shadows */
        ati_r350_gl_release(s);
        r300_raster_prims(s, d, vb, nvtx, prim);
    }
}

/*
 * 3D_DRAW_IMMD_2: dw[0] is VAP_VF_CNTL (primitive type, walk mode,
 * vertex count), the rest is vertex data laid out VAP_VTX_SIZE dwords
 * per vertex.
 */
void ati_r350_r300_draw_immd(ATIR350State *s, const uint32_t *dw, unsigned n)
{
    uint32_t vf = dw[0];
    unsigned prim = vf & 0xf;
    unsigned walk = (vf >> 4) & 3;
    unsigned nvtx = (vf >> 16) & 0xffff;
    unsigned vsize = s->regs[R300_VAP_VTX_SIZE >> 2] & 0x7f;
    R300DrawState d;
    unsigned i;

    if (walk != 3 || !nvtx) {
        trace_ati_r350_3d_skip(vf, vsize, 0);
        if (nvtx) {
            /* IMMD carries its vertices inline; any other walk mode
             * means they live somewhere we are not fetching from
             */
            ati_r350_note_gap(s, R350_GAP_VTX_WALK, walk);
        }
        return;
    }
    if (!vsize) {
        vsize = nvtx ? (n - 1) / nvtx : 0;
    }
    if (!vsize || 1 + nvtx * vsize > n) {
        trace_ati_r350_3d_skip(vf, vsize, n);
        return;
    }
    if (!r300_setup_draw(s, &d, vsize)) {
        trace_ati_r350_3d_skip(vf, vsize, s->regs[R300_RB3D_COLOROFFSET0 >> 2]);
        return;
    }

    trace_ati_r350_3d_draw(prim, nvtx, vsize, d.dst_off, d.dst_pitch,
                           d.textured, d.blend, d.tex_off);

    /*
     * Inline vertices have no array boundaries, so the vertex program's
     * input registers take four dwords each in submission order.
     */
    for (i = 0; i < ARRAY_SIZE(d.attr_size) && i * 4 < vsize; i++) {
        d.attr_size[i] = MIN(vsize - i * 4, 4u);
    }
    d.attr_count = i;

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);

        for (i = 0; i < nvtx; i++) {
            const uint32_t *vd = &dw[1 + i * vsize];
            float clip[4];

            r300_load_vtx(&d, vd, vsize, vsize, &vb[i]);
            r300_attr_texcoord(&d, vd, &vb[i]);
            if (i == 0 && d.textured) {
                r300_trace_texcoord(&d, vd, &vb[i]);
            }
            if (d.vs_run && r300_vs_vtx(s, &d, vd, &vb[i], clip)) {
                r300_xform_vtx(&d, &vb[i], clip);
            } else {
                r300_xform_vtx(&d, &vb[i], NULL);
            }
        }
        r300_run_prims(s, &d, vb, nvtx, prim);
    }
}

/*
 * VAP_CNTL_STATUS.VC_SWAP, the vertex fetcher's own endian swapper
 * (R3xx 3D register reference: 0 = none, 1 = 16-bit, 2 = 32-bit,
 * 3 = half-dword). A big-endian host uses it to leave vertex arrays in
 * memory in its native order.
 *
 * It only has to be applied on the way out of system memory here. VRAM
 * in this model stores what the CPU wrote and folds the frame-buffer
 * aperture's byte swapper into every VRAM reader instead
 * (ati_r350_vram_xor), so a VRAM-resident array has already been put
 * right by the time the fetch returns and swapping again would undo it.
 */
static uint32_t r300_vc_swap(uint32_t val, unsigned mode)
{
    switch (mode) {
    case R300_VAP_VC_SWAP_16BIT:
        return ((val & 0x00ff00ffu) << 8) | ((val >> 8) & 0x00ff00ffu);
    case R300_VAP_VC_SWAP_32BIT:
        return bswap32(val);
    case R300_VAP_VC_SWAP_HDW:
        return (val << 16) | (val >> 16);
    default:
        return val;
    }
}

/*
 * 3D_DRAW_VBUF_2: like DRAW_IMMD_2 but the single payload dword is
 * VAP_VF_CNTL (PRIM_WALK=2) and the vertices are fetched from the
 * vertex arrays bound at VAP_VTX_AOS_ADDR0..n (written either directly
 * or via 3D_LOAD_VBPNTR): each array contributes `size` dwords per
 * vertex at `stride` dwords apart, concatenated in array order. OS X
 * uses this for its texture page-in blits (GART-resident vertices and
 * textures rendered into VRAM window stores).
 *
 * How MANY arrays is VAP_VTX_NUM_ARRAYS's business and nobody else's.
 * Fetching a fixed two was right for everything the compositor draws
 * and wrong for Chess.app, which binds three: position, normal, and
 * the texture coordinate its board is painted with. The third array
 * went unread, so its vertex program's coordinate input read the
 * (0,0,0,1) default and the board sampled one texel for every pixel.
 */
void ati_r350_r300_draw_vbuf(ATIR350State *s, uint32_t vf)
{
    unsigned prim = vf & 0xf;
    unsigned nvtx = (vf >> 16) & 0xffff;
    unsigned narr = s->regs[R300_VAP_VTX_AOS_CNT >> 2] &
                    R300_VAP_VTX_NUM_ARRAYS_MASK;
    uint32_t addr[R300_AOS_MAX];
    unsigned size[R300_AOS_MAX], stride[R300_AOS_MAX];
    unsigned vsize = 0;
    unsigned swap = s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_VC_SWAP;
    R300DrawState d;
    unsigned i, a, c;

    if (narr > R300_AOS_MAX) {
        ati_r350_note_gap(s, R350_GAP_AOS_ARRAYS, narr);
        narr = R300_AOS_MAX;
    }
    for (a = 0; a < narr; a++) {
        uint32_t attr = s->regs[R300_VAP_VTX_AOS_ATTR(a / 2) >> 2];
        unsigned sh = (a & 1) ? R300_VAP_AOS_ODD_SHIFT : 0;

        size[a] = (attr >> sh) & R300_VAP_AOS_COUNT_MASK;
        stride[a] = (attr >> (sh + R300_VAP_AOS_STRIDE_SHIFT)) &
                    R300_VAP_AOS_STRIDE_MASK;
        addr[a] = s->regs[R300_VAP_VTX_AOS_ADDR(a) >> 2];
        vsize += size[a];
    }

    /*
     * Record the array state this draw actually runs on. The registers
     * hold whatever the LAST draw left behind, so reading them after
     * the fact says nothing about any particular draw -- capture here
     * or do not claim.
     */
    trace_ati_r350_3d_vbuf_aos(vf, nvtx,
                               s->regs[R300_VAP_VTX_AOS_CNT >> 2],
                               s->regs[R300_VAP_VTX_AOS_ATTR(0) >> 2],
                               narr > 0 ? addr[0] : 0,
                               narr > 1 ? addr[1] : 0);

    if (!nvtx || !vsize || vsize > 16 || nvtx > 4096) {
        trace_ati_r350_3d_skip(vf, vsize, nvtx);
        return;
    }
    if (!r300_setup_draw(s, &d, vsize)) {
        trace_ati_r350_3d_skip(vf, vsize, s->regs[R300_RB3D_COLOROFFSET0 >> 2]);
        return;
    }

    trace_ati_r350_3d_draw(prim, nvtx, vsize, d.dst_off, d.dst_pitch,
                           d.textured, d.blend, d.tex_off);

    /* one vertex-program input register per bound array, in array order */
    d.attr_count = 0;
    for (a = 0; a < narr; a++) {
        d.attr_size[a] = size[a];
        if (size[a]) {
            d.attr_count = a + 1;
        }
    }

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);
        uint32_t dw[16];

        for (i = 0; i < nvtx; i++) {
            unsigned n = 0;

            for (a = 0; a < narr; a++) {
                unsigned base = n;

                for (c = 0; c < size[a] && n < 16; c++) {
                    uint32_t card = addr[a] + (i * stride[a] + c) * 4;
                    uint32_t val = ati_r350_mc_read32(s, card);
                    uint32_t off;

                    if (swap && !ati_r350_mc_to_vram(s, card, &off)) {
                        val = r300_vc_swap(val, swap);
                    }
                    dw[n++] = val;
                }
                if (i == 0) {
                    /*
                     * Where this array resolved to, and the dwords the
                     * first vertex fetched from it -- enough to tell
                     * plausible float coordinates from garbage without
                     * re-reading memory (which would change what the
                     * trace observes).
                     */
                    uint64_t target;
                    const char *win = ati_r350_mc_describe(s, addr[a],
                                                           &target);

                    trace_ati_r350_3d_vbuf_aos_src(a, size[a], stride[a],
                                                   addr[a], win, target);
                    trace_ati_r350_3d_vbuf_aos_dw(a,
                        n > base ? dw[base] : 0,
                        n > base + 1 ? dw[base + 1] : 0,
                        n > base + 2 ? dw[base + 2] : 0,
                        n > base + 3 ? dw[base + 3] : 0);
                }
            }
            r300_load_vtx(&d, dw, vsize, size[0], &vb[i]);
            r300_attr_texcoord(&d, dw, &vb[i]);
            if (i == 0 && d.textured) {
                r300_trace_texcoord(&d, dw, &vb[i]);
            }
            if (d.vs_run) {
                float clip[4];

                if (r300_vs_vtx(s, &d, dw, &vb[i], clip)) {
                    r300_xform_vtx(&d, &vb[i], clip);
                    continue;
                }
            }
            r300_xform_vtx(&d, &vb[i], NULL);
        }
        trace_ati_r350_3d_vbuf_vtx((int32_t)(r300_f32(dw[0]) * 1000),
                                   (int32_t)(r300_f32(dw[1]) * 1000),
                                   size[0] >= 4 && vsize >= 8 ?
                                   (int32_t)(r300_f32(dw[4]) * 1000) : 0,
                                   size[0] >= 4 && vsize >= 8 ?
                                   (int32_t)(r300_f32(dw[5]) * 1000) : 0);
        r300_run_prims(s, &d, vb, nvtx, prim);
    }
}
