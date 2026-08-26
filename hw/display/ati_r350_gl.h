/*
 * ATI R300/R350 -- the host-GPU rendering backend interface.
 *
 * This header is the whole contract between the device and whatever
 * draws its triangles on the host. It deliberately names no QEMU type:
 * a request is plain data, the backend keeps no device state, and the
 * three entry points below are the only ones the device calls. That is
 * what lets phase 2's milestone M3 move the backend onto its own thread
 * -- a request is already everything a worker needs -- and what lets a
 * Metal or SDL_GPU implementation replace ati_r350_gl.c without the
 * draw path noticing.
 *
 * Coordinates in a request are the device's own: y increases downward
 * and the origin is the render target's top-left corner. The backend
 * renders into an offscreen buffer of the request's rectangle, seeded
 * from `before`, and leaves the result in `out`. It never touches VRAM,
 * a QEMU display backend, or a window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ATI_R350_GL_H
#define ATI_R350_GL_H

/*
 * Floats per vertex in a request's vertex array. Each vertex carries
 * its own position, colour and texture coordinate AND its whole
 * triangle's, flat: the fragment stage rebuilds the software
 * rasterizer's own barycentric weights from them, which is what makes
 * the two paths agree to the last bit rather than merely to the eye.
 * The layout, in order:
 *
 *   0..1    x, y            this vertex
 *   2..5    r, g, b, a
 *   6..7    s, t
 *   8..13   triangle vertex 0/1/2 positions
 *   14..25  triangle vertex 0/1/2 colours
 *   26..31  triangle vertex 0/1/2 texture coordinates
 *   32      1.0f / signed area, computed on the HOST
 */
#define R350_GL_VSTRIDE 33

typedef struct R350GlReq {
    /* destination rectangle, in the render target's own coordinates */
    int x0, y0, w, h;
    /* scissor, same coordinates, bottom-right exclusive */
    int sx0, sy0, sx1, sy1;

    const float *verts;         /* R350_GL_VSTRIDE floats per vertex */
    unsigned nvert;             /* 3 * triangle count */

    const uint8_t *tex;         /* RGBA8, tex_w x tex_h; NULL if untextured */
    int tex_w, tex_h;
    int clamp_s, clamp_t;       /* TX_FILTER0 clamp modes; <= 1 is repeat */
    int textured;

    uint32_t wmask;             /* RB3D_COLOR_CHANNEL_MASK as an ARGB mask */

    int alpha_test, af_func;
    float af_ref;
    int discard;                /* DISCARD_SRC_PIXELS selector */

    int blend, blend_read;
    int src_factor, dst_factor, comb_fcn;
    int a_src_factor, a_dst_factor, a_comb_fcn;
    float k_r, k_g, k_b, k_a;

    const uint8_t *before;      /* RGBA8 w*h: the destination as it stands */
    uint8_t *out;               /* RGBA8 w*h: where the backend leaves it */
} R350GlReq;

typedef struct R350GlCtx R350GlCtx;

/*
 * Create a backend. Returns NULL and points *err at a static reason
 * string on failure -- a host without a usable GL context is a
 * configuration fact to report, not an abort.
 */
R350GlCtx *ati_r350_gl_open(const char **err);
void ati_r350_gl_close(R350GlCtx *g);

/*
 * Render one request. Returns false if the backend could not run it,
 * in which case `out` is undefined and the caller must fall back.
 */
bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *req);

/* a one-line description of the backend actually in use, for `qom-get gl` */
const char *ati_r350_gl_describe(R350GlCtx *g);

#endif /* ATI_R350_GL_H */
