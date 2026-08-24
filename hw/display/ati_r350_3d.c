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
    uint32_t dst_off;       /* VRAM byte offset of the colour buffer */
    uint32_t dst_pitch;     /* bytes per scanline */
    bool textured;
    bool blend;
    uint32_t tex_off;       /* card address of texture level 0 */
    int tex_w, tex_h;
    uint32_t tex_pitch;     /* bytes per texel row */
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

    tx = MIN(MAX(tx, 0), d->tex_w - 1);
    ty = MIN(MAX(ty, 0), d->tex_h - 1);
    addr = d->tex_off + (uint32_t)ty * d->tex_pitch + (uint32_t)tx * 4;
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
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    /* no explicit dst height register; the VRAM bound in the pixel
     * helpers is the real limit, this just caps the loop */
    x1 = MIN(x1, 8191);
    y1 = MIN(y1, 8191);

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
            cr = w0 * v0->r + w1 * v1->r + w2 * v2->r;
            cg = w0 * v0->g + w1 * v1->g + w2 * v2->g;
            cb = w0 * v0->b + w1 * v1->b + w2 * v2->b;
            ca = w0 * v0->a + w1 * v1->a + w2 * v2->a;
            if (d->textured) {
                float ts = w0 * v0->s + w1 * v1->s + w2 * v2->s;
                float tt = w0 * v0->t + w1 * v1->t + w2 * v2->t;
                uint32_t texel = r300_sample_tex(s, d, (int)ts, (int)tt);

                cr *= ((texel >> 16) & 0xff) / 255.0f;
                cg *= ((texel >> 8) & 0xff) / 255.0f;
                cb *= (texel & 0xff) / 255.0f;
                ca *= ((texel >> 24) & 0xff) / 255.0f;
            }
            if (d->blend && ca < 1.0f) {
                uint32_t dst = r300_read_dst(s, d, x, y);
                float dr = ((dst >> 16) & 0xff) / 255.0f;
                float dg = ((dst >> 8) & 0xff) / 255.0f;
                float db = (dst & 0xff) / 255.0f;
                float da = ((dst >> 24) & 0xff) / 255.0f;

                cr = cr * ca + dr * (1.0f - ca);
                cg = cg * ca + dg * (1.0f - ca);
                cb = cb * ca + db * (1.0f - ca);
                ca = ca + da * (1.0f - ca);
            }
            out = ((uint32_t)(MIN(MAX(ca, 0.0f), 1.0f) * 255.0f) << 24) |
                  ((uint32_t)(MIN(MAX(cr, 0.0f), 1.0f) * 255.0f) << 16) |
                  ((uint32_t)(MIN(MAX(cg, 0.0f), 1.0f) * 255.0f) << 8) |
                  (uint32_t)(MIN(MAX(cb, 0.0f), 1.0f) * 255.0f);
            r300_write_dst(s, d, x, y, out);
        }
    }
}

static void r300_load_vtx(const R300DrawState *d, const uint32_t *dw,
                          unsigned vsize, R300Vtx *v)
{
    v->x = r300_f32(dw[0]);
    v->y = vsize >= 2 ? r300_f32(dw[1]) : 0.0f;
    v->z = vsize >= 3 ? r300_f32(dw[2]) : 0.0f;
    v->w = vsize >= 4 ? r300_f32(dw[3]) : 1.0f;
    v->r = v->g = v->b = v->a = 1.0f;
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
        /* pos.xyzw + one more 4-dword attribute: texcoords when a
         * texture is bound, a colour otherwise */
        if (d->textured) {
            v->s = r300_f32(dw[4]);
            v->t = r300_f32(dw[5]);
        } else {
            v->r = r300_f32(dw[4]);
            v->g = r300_f32(dw[5]);
            v->b = r300_f32(dw[6]);
            v->a = r300_f32(dw[7]);
        }
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
    uint32_t colorpitch = s->regs[R300_RB3D_COLORPITCH0 >> 2];
    uint32_t txfmt0 = s->regs[R300_TX_FORMAT0_0 >> 2];
    uint32_t txfmt2 = s->regs[R300_TX_FORMAT2_0 >> 2];
    R300DrawState d;
    unsigned i;

    if (walk != 3 || !nvtx) {
        trace_ati_r350_3d_skip(vf, vsize, 0);
        return;
    }
    if (!vsize) {
        vsize = nvtx ? (n - 1) / nvtx : 0;
    }
    if (!vsize || 1 + nvtx * vsize > n) {
        trace_ati_r350_3d_skip(vf, vsize, n);
        return;
    }

    d.vram = memory_region_get_ram_ptr(&s->vram);
    if (!ati_r350_mc_to_vram(s, s->regs[R300_RB3D_COLOROFFSET0 >> 2] & ~0x1fu,
                             &d.dst_off)) {
        /* colour buffer outside VRAM -- nothing we can show anyway */
        trace_ati_r350_3d_skip(vf, vsize, s->regs[R300_RB3D_COLOROFFSET0 >> 2]);
        return;
    }
    d.dst_pitch = (colorpitch & 0x3fff) * 4;
    if (!d.dst_pitch) {
        return;
    }
    d.textured = (s->regs[R300_TX_ENABLE >> 2] & 1) && vsize >= 8;
    d.blend = s->regs[R300_RB3D_BLENDCNTL >> 2] != 0;
    d.tex_off = s->regs[R300_TX_OFFSET_0 >> 2] & ~0x1fu;
    d.tex_w = (txfmt0 & 0x7ff) + 1;
    d.tex_h = ((txfmt0 >> 11) & 0x7ff) + 1;
    d.tex_pitch = ((txfmt2 & 0x3fff) + 1) * 4;

    trace_ati_r350_3d_draw(prim, nvtx, vsize, d.dst_off, d.dst_pitch,
                           d.textured, d.blend, d.tex_off);

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);

        for (i = 0; i < nvtx; i++) {
            r300_load_vtx(&d, &dw[1 + i * vsize], vsize, &vb[i]);
        }
        switch (prim) {
        case 4:     /* triangle list */
            for (i = 0; i + 3 <= nvtx; i += 3) {
                r300_raster_tri(s, &d, &vb[i], &vb[i + 1], &vb[i + 2]);
            }
            break;
        case 5:     /* triangle fan */
            for (i = 2; i < nvtx; i++) {
                r300_raster_tri(s, &d, &vb[0], &vb[i - 1], &vb[i]);
            }
            break;
        case 6:     /* triangle strip */
            for (i = 2; i < nvtx; i++) {
                r300_raster_tri(s, &d, &vb[i - 2], &vb[i - 1], &vb[i]);
            }
            break;
        case 13:    /* quad list */
            for (i = 0; i + 4 <= nvtx; i += 4) {
                r300_raster_tri(s, &d, &vb[i], &vb[i + 1], &vb[i + 2]);
                r300_raster_tri(s, &d, &vb[i], &vb[i + 2], &vb[i + 3]);
            }
            break;
        default:
            trace_ati_r350_3d_skip(vf, vsize, prim);
            break;
        }
    }
}
