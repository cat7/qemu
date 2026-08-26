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
 * never touches a QEMU display backend or a window.
 *
 * The render target is RESIDENT (milestone M3). ati_r350_gl_target()
 * sizes it, ati_r350_gl_seed() copies emulated VRAM into it and
 * ati_r350_gl_fetch() copies it back out, and between those the caller
 * may draw into it as often as it likes without a byte crossing the
 * bus. M2 uploaded the destination rectangle twice and read it back for
 * every single draw; on this host that was 5.2 ms of the 6.5 ms a
 * full-screen draw cost (doc/radeon9800/glbench).
 *
 * What that buys has to be paid for in coherency, and the rules are
 * stated where they are enforced -- see "GL-OWNED RENDER TARGET" in
 * ati_r350_3d.c. The backend's own part of the contract is only this:
 * it holds one target, it holds it until told otherwise, and seed and
 * fetch are the only ways bytes move.
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

/*
 * How many uploaded textures the backend keeps, plus one: slot
 * R350_GL_TEXSLOTS is a scratch the caller uses for a texture it is not
 * tracking, and it is uploaded every time. See R350GlReq.tex_slot.
 */
#define R350_GL_TEXSLOTS 8

typedef struct R350GlReq {
    /*
     * Every coordinate below is a coordinate IN THE RESIDENT TARGET,
     * with (0,0) its top-left pixel and y increasing downward. Row k of
     * the target is row k of the GL texture -- there is no flip
     * anywhere, in either direction, which is the only arrangement in
     * which a seed, a draw and a fetch can be composed in any order and
     * still agree.
     */
    /* destination rectangle: the bounding box this draw may write */
    int x0, y0, w, h;
    /* scissor, same coordinates, bottom-right exclusive */
    int sx0, sy0, sx1, sy1;
    /* the whole target's extent, which is what the draw renders into */
    int surf_w, surf_h;

    const float *verts;         /* R350_GL_VSTRIDE floats per vertex */
    unsigned nvert;             /* 3 * triangle count */

    /*
     * The vertices are ordered into PASSES. A blended draw whose own
     * primitives overlap cannot be rendered in one go -- the shader
     * blends against a snapshot of the destination, while the device
     * paints primitives in order and each blends against what the last
     * one left. So the caller partitions the triangles so that no two in
     * a pass overlap and any overlapping pair lands in the device's own
     * order, and the backend refreshes the blend's source between
     * passes. `pass[k]` is the first vertex of pass k and there are
     * npass + 1 entries, the last being nvert. A single pass (the
     * ordinary case) may leave both NULL and 0.
     */
    const unsigned *pass;
    unsigned npass;

    /*
     * The texture, RGBA8, and WHICH of the backend's texture objects it
     * belongs in. The caller already decides when a decoded texture is
     * still current -- it owns the VRAM ranges the answer depends on --
     * so it names a slot and says whether the bytes are new. A slot
     * whose bytes are unchanged is bound and not re-uploaded, which on
     * this host is 0.93 ms of caller time per full-screen draw.
     * `tex` may be NULL when tex_fresh is false. A slot of exactly
     * R350_GL_TEXSLOTS is the scratch, which is always uploaded.
     */
    const uint8_t *tex;
    unsigned tex_slot;          /* <= R350_GL_TEXSLOTS; the last is scratch */
    int tex_fresh;              /* upload `tex` into that slot first */
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

    /*
     * Hand this draw's blend to GL's OWN blender instead of computing it
     * in the fragment shader, and render every primitive in one pass
     * however much they overlap each other.
     *
     * The caller sets it only when the blend is
     *     dst' = dst + f(src)
     * -- destination factor ONE, combine ADD, and a source factor that
     * does not read the destination, for colour and alpha alike. Under
     * that shape the destination term is the destination unchanged, so
     * a per-primitive quantisation can be reproduced exactly without
     * ever reading it: the shader emits floor(255*f(src)) and GL's
     * blender adds it to a byte that is already an integer. `pass` and
     * `npass` are then not used and no snapshot of the destination is
     * needed or taken. See the r300_gl_addblend() comment in
     * ati_r350_3d.c for the predicate and why it is exactly this shape.
     *
     * It is not exact -- the per-primitive rounding decomposition
     * differs from the device's and accumulates over a pixel's overlap
     * depth -- so the caller only sets it under `gl=fast`. The numbers
     * are in that same comment.
     */
    int add_blend;

    /*
     * Where to leave a packed RGBA8 copy of the drawn rectangle, or
     * NULL. Only gl=verify wants one: the target is resident, so the
     * ordinary path leaves the pixels on the GPU and fetches them when
     * something outside the 3D engine needs to look.
     */
    uint8_t *out;
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
 * Size the resident render target to at least w x h. Returns false if
 * it could not be created, and sets *lost when the previous contents
 * did not survive -- the caller then has to seed again whatever it
 * needs, because nothing else can tell it.
 */
bool ati_r350_gl_target(R350GlCtx *g, int w, int h, bool *lost);

/*
 * Move one rectangle between emulated VRAM and the resident target.
 * `base` addresses the target's own pixel (0,0) in VRAM, `pitch` is its
 * bytes per row, and `xr` is the aperture swapper's byte-lane xor over
 * it -- byte (2^xr) of a pixel is red, (1^xr) green, (0^xr) blue and
 * (3^xr) alpha. GL can be asked for two of the four orders directly,
 * and measurably should not be: see the comment above ati_r350_gl_seed()
 * for the numbers.
 */
bool ati_r350_gl_seed(R350GlCtx *g, int x0, int y0, int w, int h,
                      const uint8_t *base, unsigned pitch, unsigned xr);
bool ati_r350_gl_fetch(R350GlCtx *g, int x0, int y0, int w, int h,
                       uint8_t *base, unsigned pitch, unsigned xr);

/*
 * Render one request into the resident target. Returns false if the
 * backend could not run it, in which case the target is unchanged and
 * the caller must fall back.
 */
bool ati_r350_gl_draw(R350GlCtx *g, const R350GlReq *req);

/* a one-line description of the backend actually in use, for `qom-get gl` */
const char *ati_r350_gl_describe(R350GlCtx *g);

#endif /* ATI_R350_GL_H */
