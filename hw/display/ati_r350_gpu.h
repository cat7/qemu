/*
 * ATI R300/R350 -- host GPU backend selection on macOS.
 *
 * The device calls the interface in ati_r350_gl.h. On macOS two
 * implementations of it are built: OpenGL (ati_r350_gl.c) and Metal
 * (ati_r350_mtl.m). Each defines R350_GPU_IMPL_OGL or R350_GPU_IMPL_MTL
 * before including this header, which renames the interface to its own
 * prefix; ati_r350_gpu.c then provides the ati_r350_gl_* entry points
 * and forwards each call to the backend the context was opened with.
 * Other hosts build ati_r350_gl.c under the interface names directly.
 *
 * Based on lyons88/qemu-g5 934422f4f9.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ATI_R350_GPU_H
#define ATI_R350_GPU_H

#if defined(R350_GPU_IMPL_OGL)
#define R350GlCtx                   R350OglCtx
#define R350_GPU_P(n)               r350_ogl_##n
#elif defined(R350_GPU_IMPL_MTL)
#define R350GlCtx                   R350MtlCtx
#define R350_GPU_P(n)               r350_mtl_##n
#endif

#ifdef R350_GPU_P
#define ati_r350_gl_open            R350_GPU_P(open)
#define ati_r350_gl_close           R350_GPU_P(close)
#define ati_r350_gl_prog_ready      R350_GPU_P(prog_ready)
#define ati_r350_gl_worker_stats    R350_GPU_P(worker_stats)
#define ati_r350_gl_target          R350_GPU_P(target)
#define ati_r350_gl_seed            R350_GPU_P(seed)
#define ati_r350_gl_fetch           R350_GPU_P(fetch)
#define ati_r350_gl_depth           R350_GPU_P(depth)
#define ati_r350_gl_zseed           R350_GPU_P(zseed)
#define ati_r350_gl_zfetch          R350_GPU_P(zfetch)
#define ati_r350_gl_zclear          R350_GPU_P(zclear)
#define ati_r350_gl_draw            R350_GPU_P(draw)
#define ati_r350_gl_zq_mark         R350_GPU_P(zq_mark)
#define ati_r350_gl_zq_sum          R350_GPU_P(zq_sum)
#define ati_r350_gl_tex_forget      R350_GPU_P(tex_forget)
#define ati_r350_gl_describe        R350_GPU_P(describe)
#define ati_r350_gl_prog_stats      R350_GPU_P(prog_stats)
#define ati_r350_gl_barriers        R350_GPU_P(barriers)
#define ati_r350_gl_queue_stats     R350_GPU_P(queue_stats)
#define ati_r350_gl_hold            R350_GPU_P(hold)
#define ati_r350_gl_unbind          R350_GPU_P(unbind)
#endif

#include "ati_r350_gl.h"

#ifndef R350_GPU_P
/* the dispatcher's view of both implementations */
#define R350_GPU_DECLARE(T, P)                                              \
    typedef struct T T;                                                     \
    T *P##_open(const char **err, unsigned workers);                        \
    void P##_close(T *g);                                                   \
    int P##_prog_ready(T *g, uint64_t key, bool add, const char *glsl,      \
                       uint32_t wmask);                                     \
    void P##_worker_stats(T *g, uint64_t *warms, uint64_t *failed,          \
                          uint64_t *waits, unsigned *inflight);             \
    bool P##_target(T *g, int w, int h, bool *lost);                        \
    bool P##_seed(T *g, int x0, int y0, int w, int h,                       \
                  const uint8_t *base, unsigned pitch, unsigned xr);        \
    bool P##_fetch(T *g, int x0, int y0, int w, int h,                      \
                   uint8_t *base, unsigned pitch, unsigned xr);             \
    bool P##_depth(T *g);                                                   \
    bool P##_zseed(T *g, int x0, int y0, int w, int h, const uint32_t *z,   \
                   int mode);                                               \
    bool P##_zfetch(T *g, int x0, int y0, int w, int h, uint32_t *z,        \
                    int mode);                                              \
    bool P##_zclear(T *g, int x0, int y0, int w, int h, uint32_t z,         \
                    int mode);                                              \
    bool P##_draw(T *g, const R350GlReq *req);                              \
    uint64_t P##_zq_mark(T *g);                                             \
    bool P##_zq_sum(T *g, uint64_t ticket, bool wait, uint64_t *sum);       \
    void P##_tex_forget(T *g, unsigned slot);                               \
    const char *P##_describe(T *g);                                         \
    void P##_prog_stats(T *g, uint64_t *hits, uint64_t *links,              \
                        uint64_t *failed);                                  \
    uint64_t P##_barriers(T *g);                                            \
    void P##_queue_stats(T *g, uint64_t *units, uint64_t *flushes,          \
                         uint64_t *waves);                                  \
    void P##_hold(bool hold);                                               \
    void P##_unbind(T *g);

R350_GPU_DECLARE(R350OglCtx, r350_ogl)
R350_GPU_DECLARE(R350MtlCtx, r350_mtl)
#endif

#endif /* ATI_R350_GPU_H */
