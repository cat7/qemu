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
    unsigned attr_size[4];  /* dwords each input register takes per vertex */
    unsigned attr_count;
    float vp[6];            /* SE_VPORT XSCALE,XOFF,YSCALE,YOFF,ZSCALE,ZOFF */
    uint32_t dst_off;       /* VRAM byte offset of the colour buffer */
    uint32_t dst_pitch;     /* bytes per scanline */
    uint32_t wmask;         /* RB3D_COLOR_CHANNEL_MASK as an ARGB byte mask */
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
    unsigned tex_bpp;       /* 32 (ARGB8888) or 8 (A8 masks) */
    unsigned tex_sel_alpha; /* TX_FORMAT1 SEL_ALPHA swizzle */
    unsigned clamp_s, clamp_t; /* TX_FILTER0 clamp modes (0 = repeat) */
    float flat_r, flat_g, flat_b, flat_a;
    uint8_t *vram;
} R300DrawState;

static inline float r300_f32(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
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
        /* A8 mask: alpha from the byte, black colour */
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
        return (uint32_t)a << 24;
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

static void r300_write_dst(ATIR350State *s, const R300DrawState *d,
                           int x, int y, uint32_t argb)
{
    uint32_t addr = d->dst_off + (uint32_t)y * d->dst_pitch + (uint32_t)x * 4;
    unsigned xr;

    if (addr + 4 > ATI_R350_VRAM_SIZE) {
        return;
    }
    if (d->wmask != 0xffffffff) {
        /* masked-off channels keep whatever the destination holds */
        argb = (argb & d->wmask) | (ati_r350_vram_ld32(s, addr) & ~d->wmask);
    }
    xr = ati_r350_vram_xor(s, addr);
    d->vram[(addr + 0) ^ xr] = argb & 0xff;
    d->vram[(addr + 1) ^ xr] = (argb >> 8) & 0xff;
    d->vram[(addr + 2) ^ xr] = (argb >> 16) & 0xff;
    d->vram[(addr + 3) ^ xr] = (argb >> 24) & 0xff;
    memory_region_set_dirty(&s->vram, addr & ~7ull, 8);
}

static uint32_t r300_read_dst(ATIR350State *s, const R300DrawState *d,
                              int x, int y)
{
    uint32_t addr = d->dst_off + (uint32_t)y * d->dst_pitch + (uint32_t)x * 4;

    if (addr + 4 > ATI_R350_VRAM_SIZE) {
        return 0;
    }
    return ati_r350_vram_ld32(s, addr);
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

static void r300_raster_tri(ATIR350State *s, const R300DrawState *d,
                            const R300Vtx *v0, const R300Vtx *v1,
                            const R300Vtx *v2)
{
    float area = r300_edge(v0, v1, v2->x, v2->y);
    int x0, y0, x1, y1, x, y;

    if (area == 0.0f) {
        return;
    }
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
        for (x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = r300_edge(v1, v2, px, py) / area;
            float w1 = r300_edge(v2, v0, px, py) / area;
            float w2 = 1.0f - w0 - w1;
            float cr, cg, cb, ca;
            uint32_t out;

            if (w0 < 0.0f || w1 < 0.0f || w2 < -0.001f) {
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
            cr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
            cg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
            cb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
            ca = w0 * v0->a + w1 * v1->a + w2 * v2->a;
            if (d->textured) {
                float ts = w0 * v0->s + w1 * v1->s + w2 * v2->s;
                float tt = w0 * v0->t + w1 * v1->t + w2 * v2->t;
                uint32_t texel = r300_sample_tex(s, d, (int)ts, (int)tt);
                float ta;

                switch (d->tex_sel_alpha) {
                case 4:  ta = 0.0f; break;
                case 5:  ta = 1.0f; break;
                default: ta = ((texel >> 24) & 0xff) / 255.0f; break;
                }
                cr *= ((texel >> 16) & 0xff) / 255.0f;
                cg *= ((texel >> 8) & 0xff) / 255.0f;
                cb *= (texel & 0xff) / 255.0f;
                ca *= ta;
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
                uint32_t dst = d->blend_read ? r300_read_dst(s, d, x, y) : 0;
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
            r300_write_dst(s, d, x, y, out);
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
    v->x = (cx / cw) * d->vp[0] + d->vp[1];
    v->y = (cy / cw) * d->vp[2] + d->vp[3];
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
     * TX_FORMAT1's low format code: 0 is the 8-bit single-channel
     * format (window drop shadows arrive as A8 gradient masks,
     * TX_FORMAT1=0x00124000); 0xc is ARGB8888, which everything else
     * uses. TXPITCH counts texels, so the byte pitch scales with the
     * texel size.
     */
    d->tex_bpp = (s->regs[R300_TX_FORMAT1_0 >> 2] & 0x1f) == 0 ? 8 : 32;
    if (d->textured) {
        unsigned txcode = s->regs[R300_TX_FORMAT1_0 >> 2] & 0x1f;

        /* everything else is being read as if it were ARGB8888 */
        if (txcode != 0 && txcode != 0xc) {
            ati_r350_note_gap(s, R350_GAP_TEX_FORMAT, txcode);
        }
    }
    /*
     * TX_FORMAT1 SEL_ALPHA ([11:9]) swizzles the alpha the shader
     * sees: 3 takes the texel's alpha component, 5 forces 1.0 (how
     * X8R8G8B8 window tiles present -- their memory alpha byte is 0,
     * and the alpha test would discard every pixel), 4 forces 0.
     */
    d->tex_sel_alpha = (s->regs[R300_TX_FORMAT1_0 >> 2] >> 9) & 7;
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
    d->vs_run = false;
    d->vs_color = false;
    memset(&d->vs, 0, sizeof(d->vs));
    if (!(s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_PVS_BYPASS) &&
        s->pvs_const_dwords >= 16) {
        int k;
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
            d->xform = true;
            d->vs_run = d->vs.valid;
            d->vs_color_out = first_color;
            /*
             * The colour is the program's only when the vertex stage says
             * it emits one and the program really writes it. A colour it
             * merely forwards from an attribute this model is already
             * sampling as texture coordinates is not a colour at all --
             * the eight-dword textured vertices whose second attribute is
             * a coordinate pair would otherwise arrive painted with it.
             */
            if (d->vs_run && ncolor &&
                (d->vs.out_mask & (1u << first_color))) {
                int src = d->vs.out_src[first_color];
                bool computed = !d->vs.plain_matrix || src >= 0;
                bool is_texcoord = d->textured && src >= 0 &&
                                   (unsigned)src == (vsize >= 12 ? 2u : 1u);

                d->vs_color = computed && !is_texcoord;
            }
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
     * Where this draw lands and which of its channels survive, read
     * where the draw itself reads them: a post-hoc read of the mask
     * says only what the last writer left behind.
     */
    trace_ati_r350_3d_cb(d->dst_off, d->dst_pitch, d->wmask);
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

static void r300_run_prims(ATIR350State *s, R300DrawState *d,
                           const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    unsigned i;

    /*
     * Where this draw actually lands, straight from the transformed
     * vertices. Offline replay of a command stream has to reconstruct
     * this and can get it wrong -- reading it from the engine is the
     * ground truth to check such a reconstruction against.
     */
    if (trace_event_get_state_backends(TRACE_ATI_R350_3D_RECT) && nvtx) {
        float x0 = vb[0].x, y0 = vb[0].y, x1 = x0, y1 = y0;
        float s0 = vb[0].s, t0 = vb[0].t, s1 = s0, t1 = t0;

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
 * vertex arrays bound at VAP_VTX_AOS_ADDR0/1 (written either directly
 * or via 3D_LOAD_VBPNTR): each array contributes `size` dwords per
 * vertex at `stride` dwords apart, concatenated in array order. OS X
 * uses this for its texture page-in blits (GART-resident vertices and
 * textures rendered into VRAM window stores).
 */
void ati_r350_r300_draw_vbuf(ATIR350State *s, uint32_t vf)
{
    unsigned prim = vf & 0xf;
    unsigned nvtx = (vf >> 16) & 0xffff;
    uint32_t ctl = s->regs[R300_VAP_VTX_AOS_CTL >> 2];
    uint32_t addr[2] = {
        s->regs[R300_VAP_VTX_AOS_ADDR0 >> 2],
        s->regs[R300_VAP_VTX_AOS_ADDR1 >> 2],
    };
    unsigned size[2] = { ctl & 0xff, (ctl >> 16) & 0xff };
    unsigned stride[2] = { (ctl >> 8) & 0xff, (ctl >> 24) & 0xff };
    unsigned vsize = size[0] + size[1];
    unsigned swap = s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_VC_SWAP;
    R300DrawState d;
    unsigned i, a, c;

    /*
     * Record the array state this draw actually runs on. The registers
     * hold whatever the LAST draw left behind, so reading them after
     * the fact says nothing about any particular draw -- capture here
     * or do not claim.
     */
    trace_ati_r350_3d_vbuf_aos(vf, nvtx,
                               s->regs[R300_VAP_VTX_AOS_CNT >> 2],
                               ctl, addr[0], addr[1]);

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
    d.attr_size[0] = size[0];
    d.attr_size[1] = size[1];
    d.attr_count = size[1] ? 2 : 1;

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);
        uint32_t dw[16];

        for (i = 0; i < nvtx; i++) {
            unsigned n = 0;

            for (a = 0; a < 2; a++) {
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
