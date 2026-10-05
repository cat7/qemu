/*
 * ATI R300/R350 -- host GPU backend dispatch on macOS.
 *
 * Based on lyons88/qemu-g5 934422f4f9.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ati_r350_gpu.h"

struct R350GlCtx {
    bool mtl;
    void *impl;
};

#define OGL(g) ((R350OglCtx *)(g)->impl)
#define MTL(g) ((R350MtlCtx *)(g)->impl)
#define FWD(g, fn, ...) \
    ((g)->mtl ? r350_mtl_##fn(MTL(g), ##__VA_ARGS__) \
              : r350_ogl_##fn(OGL(g), ##__VA_ARGS__))

static R350GlCtx *gpu_wrap(bool mtl, void *impl)
{
    R350GlCtx *g;

    if (!impl) {
        return NULL;
    }
    g = g_new0(R350GlCtx, 1);
    g->mtl = mtl;
    g->impl = impl;
    return g;
}

R350GlCtx *ati_r350_gl_open(const char **err, unsigned workers)
{
    return gpu_wrap(false, r350_ogl_open(err, workers));
}

R350GlCtx *ati_r350_gl_open_metal(const char **err, unsigned workers)
{
    return gpu_wrap(true, r350_mtl_open(err, workers));
}

void ati_r350_gl_close(R350GlCtx *g)
{
    if (g) {
        if (g->mtl) {
            r350_mtl_close(MTL(g));
        } else {
            r350_ogl_close(OGL(g));
        }
        g_free(g);
    }
}

int ati_r350_gl_prog_ready(R350GlCtx *g, uint64_t key, bool add,
                           const char *glsl, uint32_t wmask)
{
    return g ? FWD(g, prog_ready, key, add, glsl, wmask) : -1;
}

void ati_r350_gl_worker_stats(R350GlCtx *g, uint64_t *warms, uint64_t *failed,
                              uint64_t *waits, unsigned *inflight)
{
    if (g && g->mtl) {
        r350_mtl_worker_stats(MTL(g), warms, failed, waits, inflight);
    } else {
        r350_ogl_worker_stats(g ? OGL(g) : NULL, warms, failed, waits,
                              inflight);
    }
}

bool ati_r350_gl_target(R350GlCtx *g, int w, int h, bool *lost)
{
    if (!g) {
        *lost = false;
        return false;
    }
    return FWD(g, target, w, h, lost);
}

bool ati_r350_gl_seed(R350GlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr)
{
    return g && FWD(g, seed, x0, y0, w, h, base, pitch, xr);
}

bool ati_r350_gl_fetch(R350GlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr)
{
    return g && FWD(g, fetch, x0, y0, w, h, base, pitch, xr);
}

bool ati_r350_gl_depth(R350GlCtx *g)
{
    return g && FWD(g, depth);
}

bool ati_r350_gl_zseed(R350GlCtx *g, int x0, int y0, int w, int h,
                       const uint32_t *z, int mode)
{
    return g && FWD(g, zseed, x0, y0, w, h, z, mode);
}

bool ati_r350_gl_zfetch(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t *z, int mode)
{
    return g && FWD(g, zfetch, x0, y0, w, h, z, mode);
}

bool ati_r350_gl_zclear(R350GlCtx *g, int x0, int y0, int w, int h,
                        uint32_t z, int mode)
{
    return g && FWD(g, zclear, x0, y0, w, h, z, mode);
}

bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *req)
{
    return g && FWD(g, draw, req);
}

uint64_t ati_r350_gl_zq_mark(R350GlCtx *g)
{
    return g ? FWD(g, zq_mark) : 0;
}

bool ati_r350_gl_zq_sum(R350GlCtx *g, uint64_t ticket, bool wait,
                        uint64_t *sum)
{
    return g && FWD(g, zq_sum, ticket, wait, sum);
}

void ati_r350_gl_tex_forget(R350GlCtx *g, unsigned slot)
{
    if (g) {
        FWD(g, tex_forget, slot);
    }
}

const char *ati_r350_gl_describe(R350GlCtx *g)
{
    return g ? FWD(g, describe) : "none";
}

void ati_r350_gl_prog_stats(R350GlCtx *g, uint64_t *hits, uint64_t *links,
                            uint64_t *failed)
{
    if (g) {
        FWD(g, prog_stats, hits, links, failed);
    } else {
        *hits = *links = *failed = 0;
    }
}

uint64_t ati_r350_gl_barriers(R350GlCtx *g)
{
    return g ? FWD(g, barriers) : 0;
}

void ati_r350_gl_queue_stats(R350GlCtx *g, uint64_t *units, uint64_t *flushes,
                             uint64_t *waves)
{
    if (g) {
        FWD(g, queue_stats, units, flushes, waves);
    } else {
        *units = *flushes = *waves = 0;
    }
}

void ati_r350_gl_hold(bool hold)
{
    r350_ogl_hold(hold);
}

void ati_r350_gl_unbind(R350GlCtx *g)
{
    if (g) {
        FWD(g, unbind);
    }
}
