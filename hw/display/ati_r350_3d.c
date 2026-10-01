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
#include "system/physmem.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "qemu/units.h"
#include "exec/target_page.h"
#include "ati_r350_int.h"
#include "ati_r350_regs.h"
#include "ati_r350_gl.h"
#include "trace.h"

typedef struct R300Vtx {
    float x, y, z, w;
    float r, g, b, a;
    /*
     * The second interpolated colour. The rasterizer routes up to four,
     * and a fragment program that names two is what Chess.app's board is:
     * `MAD OUT.rgb = texel, colour0, colour1` -- diffuse times the wood
     * plus a specular term this model had no second colour to carry.
     */
    float r1, g1, b1, a1;
    /*
     * The interpolated texture coordinate SETS, `tc[k][0]` and
     * `tc[k][1]` being set k's s and t. The rasterizer routes each set
     * to a frame register of its own (R300UsRs::tex_reg), and a
     * fragment program may sample several units with any of them --
     * Mac OS X 10.5's compositor fetches unit 0 with set 0 and unit 1
     * with set 1 in a single program.
     *
     * UNITS: TEXELS of `R300DrawState::tc_unit[k]`, and that is a
     * deliberate choice rather than the hardware's. The hardware
     * interpolates the vertex program's own output and normalises
     * nothing; this model multiplies by the bound size in
     * r300_vs_texcoord() and divides it out again in r300_raster_tri()
     * on the way into the fragment frame, so that this struct -- which
     * is written verbatim into every R350CAP3 record and read straight
     * by the specialised executor and the GL backend -- keeps one stable
     * meaning. See the comment at r300_vs_texcoord() for why the two
     * readers cannot tell.
     */
    float tc[R300_TEXCOORDS][2];
    /*
     * Each set's four components as the vertex stage produced them, in
     * the guest's units, with no size scaling and no divide by q: what
     * the rasterizer interpolates for a set no fetch reads.
     */
    float tcr[R300_TEXCOORDS][4];
} R300Vtx;

/*
 * One bound texture unit. The TX_* register blocks are sixteen deep with
 * a four-byte stride, so unit u's state is simply the u'th word of each
 * block; everything the sampler and the GL decode need is resolved into
 * here once per draw.
 */
/* NUM_LEVELS counts to 11: a 2048-texel level 0 and eleven halvings */
#define R300_TEX_LEVELS 12

typedef struct R300TexUnit {
    bool en;                /* TX_ENABLE names this unit */
    uint32_t off;           /* card address of texture level 0 */
    int w, h;
    uint32_t pitch;         /* bytes per texel row */
    unsigned bpp;           /* bits per texel: 8, 16, 32 or 64 */
    unsigned code;          /* TX_FORMAT1 TXFORMAT, to tell the widths apart */
    unsigned sel[4];        /* TX_FORMAT1 component select, A R G B */
    unsigned clamp_s, clamp_t;  /* TX_FILTER0 clamp modes (0 = repeat) */
    unsigned lanes;         /* TX_OFFSET ENDIAN_SWAP as a byte-lane xor */
    unsigned yuv;           /* 4:2:2 only: YUV_TO_RGB, SWAP_YUV in bit 2 */
    /*
     * Filtering. A unit with `filt` clear samples one texel of level 0
     * through r300_sample_tex() exactly as before; see r300_tex_filter()
     * for the rest.
     */
    bool filt;
    bool need_lod;          /* the result depends on the pixel footprint */
    unsigned mag, min, mip; /* 1 point, 2 linear; min 3 anisotropic */
    unsigned aniso_l2;      /* log2 of the MAX_ANISO ratio */
    unsigned first, last;   /* MAX_MIP_LEVEL and NUM_LEVELS */
    int bias;               /* LOD_BIAS in 1/256 levels */
    unsigned wl2, hl2;      /* floor(log2) of the level-0 size */
    uint32_t border;        /* TX_BORDER_COLOR as a texel of this format */
    uint32_t loff[R300_TEX_LEVELS];     /* card address of each level */
    uint32_t lpitch[R300_TEX_LEVELS];   /* bytes per row of each level */
    int lw[R300_TEX_LEVELS], lh[R300_TEX_LEVELS];
    unsigned nlev;          /* levels the GL backend is handed: 0..nlev-1 */
    uint32_t chain;         /* bytes from `off` to the end of that chain */
    size_t ltexels;         /* texels in those levels */
} R300TexUnit;

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
    bool vs_color2;         /* the program emits a second colour as well */
    unsigned vs_color2_out;
    /*
     * The fragment program in force. It lives in the device rather than
     * here so that the GL backend can key its shader cache on it, and it
     * is NULL only when the control registers describe nothing.
     */
    const R300UsProgram *fs;
    bool fs_run;            /* this model can execute it */
    bool fs_col1;           /* it names a second interpolated colour */
    /*
     * Per coordinate SET: take it from the vertex program's output, and
     * which output register that is. The sets the vertex stage declares
     * are consecutive outputs from `first_texcoord`.
     */
    bool vs_texcoord[R300_TEXCOORDS];
    unsigned vs_tex_out[R300_TEXCOORDS];
    /*
     * Dwords this vertex carries for each of the vertex program's input
     * registers, indexed by REGISTER: VAP_PROG_STREAM_CNTL's DST_VEC_LOC
     * decides which register a stream element lands in, and it is not
     * always the element's own index. A register no element feeds is
     * zero and reads the (0,0,0,1) default.
     */
    unsigned attr_size[R300_AOS_MAX];
    unsigned attr_count;
    float vp[6];            /* SE_VPORT XSCALE,XOFF,YSCALE,YOFF,ZSCALE,ZOFF */
    bool vte_xs, vte_xo;    /* VAP_VTE_CNTL: apply that scale/offset at all */
    bool vte_ys, vte_yo;
    uint32_t dst_off;       /* VRAM byte offset of the colour buffer */
    uint32_t dst_pitch;     /* bytes per scanline */
    unsigned cb_fmt;        /* RB3D_COLORPITCH0 COLORFORMAT */
    unsigned cb_bpp;        /* bytes per colour buffer pixel: 1, 2 or 4 */
    unsigned cb_sel;        /* I8: bit position of the stored channel */
    uint32_t wmask;         /* RB3D_COLOR_CHANNEL_MASK as an ARGB byte mask */
    bool resolve;           /* colour buffer in AA-resolve mode */
    uint32_t res_off;       /* the buffer being resolved FROM */
    uint32_t res_pitch;
    bool textured;
    bool lod_any;           /* a bound unit wants coordinate derivatives */
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
    R300TexUnit tex[R300_TEX_UNITS];
    /* vertex attribute holding each coordinate set's s,t; -1 = none */
    int tex_attr[R300_TEXCOORDS];
    /*
     * Which unit's texture size each coordinate set is scaled by. This
     * model samples in TEXELS while the hardware samples normalised, so
     * a coordinate the vertex stage produced is multiplied by the bound
     * texture's dimensions on the way out of the vertex stage -- and
     * with more than one unit bound there is a choice of which. It is
     * the FIRST unit the fragment program fetches with that set, which
     * for every single-texture draw is unit 0 and therefore exactly the
     * arithmetic that predates multitexturing.
     */
    unsigned tc_unit[R300_TEXCOORDS];
    /*
     * How many coordinate sets this draw actually carries -- one past
     * the highest the fragment program's routing names, and 1 for every
     * draw that predates multitexturing. The rasterizer interpolates
     * exactly this many, so a single-texture draw pays nothing.
     */
    unsigned ntc;
    /* sets read by no LD/PROJ/LODBIAS: the frame gets tcr, all four */
    unsigned tc_raw;
    float flat_r, flat_g, flat_b, flat_a;
    uint8_t *vram;
    /*
     * The colour buffer's bytes: VRAM, or for a buffer in GART a host
     * copy of rows [0, cb_size / dst_pitch) at card address cb_card,
     * swapped by COLORENDIAN (cb_xr) rather than the aperture.
     */
    uint8_t *cb;
    uint32_t cb_size;
    uint32_t cb_card;
    unsigned cb_xr;
    bool cb_host;
} R300DrawState;

/*
 * DOES THIS DRAW USE A TEXTURE? That is not the question `textured`
 * answers.
 *
 * `d->textured` says a texture is BOUND and that this model can produce
 * a coordinate for it -- TX_ENABLE's first bit, plus a vertex wide
 * enough to carry a coordinate positionally or a program that computes
 * one. A guest binds a texture for the draws that sample it and leaves
 * it bound across the ones that do not, so "bound" and "used" are
 * different sets, and three decisions below need the second one.
 *
 * What USES a texture is the fragment program, and it has already said
 * so: `tex_dst` is the frame register its first LD/PROJ writes, and -1
 * when the program performs no fetch at all. That is the same answer the
 * shading path acts on -- a program with no texture instruction samples
 * nothing however much is bound -- so this only brings the setup into
 * agreement with what the pixels already do.
 *
 * iTunes' visualiser is what named this. Its full-surface clears are
 * giant point sprites carrying a real dark COLOR_0 and a fragment
 * program with no texture instruction (US_CONFIG's FIRST_TEX clear, so
 * the level contributes no texture slots at all), issued while the
 * texture the NEXT draws sample is already bound. Read as `textured`
 * they lost that colour three times over: discarded as a texture
 * coordinate, replaced by a white flat constant, and then whitened again
 * at the sprite's corners. 255 of 255 bound-but-fetchless draws in the
 * 2026-08-31 evidence capture recorded (1,1,1) for a dark wash.
 */
static inline bool r300_draw_fetches(const R300DrawState *d)
{
    return d->textured && d->fs && d->fs->tex_dst >= 0;
}

/*
 * The GL backend's contract states its own texture-unit and coordinate-
 * set counts rather than including this file's headers -- it is meant to
 * be replaceable without the draw path noticing. That independence is
 * only safe if the numbers are checked rather than trusted. The unit
 * counts must AGREE; the backend's coordinate-set count may be smaller
 * than the device's, because it only ever receives a draw whose program
 * reads set 0 (see ati_r350_gl.h), and it must never be larger. The
 * capture format repeats the unit count for the same reason.
 */
QEMU_BUILD_BUG_ON(R350_GL_TEXUNITS != R300_TEX_UNITS);
QEMU_BUILD_BUG_ON(R350_GL_TEXCOORDS > R300_TEXCOORDS);
QEMU_BUILD_BUG_ON(R350_CAP_TEX_UNITS != R300_TEX_UNITS);

/*
 * PARALLEL RASTERISATION. A primitive's triangles are drawn in stripes
 * of 1 << R300_RASTER_STRIPE_SHIFT absolute screen rows, which the
 * submitting thread and the workers take in turn until none is left. A
 * pixel belongs to one stripe whichever triangle covers it, and a stripe
 * is drawn by one thread with its triangles in submission order, so
 * every pixel is written in the order it was serially and comes out bit
 * for bit the same. Colour, Z and stencil stores are byte-granular and a
 * pixel's bytes are its own, so two stripes never share a byte.
 *
 * Workers read the draw state and the triangles and write VRAM or the
 * staged colour buffer; they take no lock and never reach the bus. The
 * submitting thread holds whatever it held for the draw and waits for
 * the workers without taking anything else. Bus-memory textures are
 * copied to the host before a split, and a draw that reads what it
 * writes is drawn serially.
 */
#define R300_RASTER_MAX_THREADS 8
/* small stripes keep the last one short: its drawer is waited for */
#define R300_RASTER_STRIPE_SHIFT 1
/* below this many bounding-box pixels the wakeup costs more */
#define R300_RASTER_MIN_PX 4096
/* bus-memory texture copied per draw */
#define R300_SHADOW_MIN_BYTES (256 * KiB)
#define R300_SHADOW_MAX_BYTES (32 * MiB)

typedef struct R300RasterTri {
    const R300Vtx *v[3];
    unsigned cull;
    int y0, y1;                 /* rows it can touch, inclusive */
} R300RasterTri;

typedef struct R300RasterWorker {
    ATIR350State *s;
    QemuThread thread;
    QemuSemaphore start;
    uint32_t job;               /* the job it was last woken for */
    ATIR350SwapMemo memo;
} R300RasterWorker;

/* a batch is flushed at this many triangles even inside its packet */
#define R300_RASTER_QUEUE_MAX 65536
/* rows per bin of the batch's per-row-range triangle index */
#define R300_RASTER_BIN_SHIFT 6
#define R300_RASTER_BINS (8192 >> R300_RASTER_BIN_SHIFT)
/* vertices per block of the batch's own vertex store */
#define R300_RASTER_VTX_BLOCK 1024
/* a stripe never straddles two bins */
QEMU_BUILD_BUG_ON(R300_RASTER_BIN_SHIFT < R300_RASTER_STRIPE_SHIFT);

typedef struct R300Raster {
    R300RasterWorker worker[R300_RASTER_MAX_THREADS - 1];
    unsigned nworkers;
    bool quit;
    /* the primitive's triangles are being queued rather than drawn */
    bool queueing;
    const R300DrawState *d;
    R300RasterTri *tri;
    unsigned ntri, cap;
    uint64_t px;                /* bounding-box pixels queued */
    int y0, y1;                 /* rows the batch spans, inclusive */
    /*
     * The batch's triangles by bin of rows, each bin's in submission
     * order: bin b's are bin_tri[bin_at[b]] .. bin_tri[bin_at[b + 1] - 1].
     */
    unsigned bin_at[R300_RASTER_BINS + 1];
    unsigned *bin_tri;
    size_t bin_cap;
    /*
     * Vertices the primitive walk made up -- sprite and line corners, a
     * rectangle's fourth -- kept until the batch is drawn. Blocks never
     * move, so a queued triangle's pointers stay good.
     */
    R300Vtx **vblk;
    unsigned nvblk, vblk_cur, vblk_used;
    uint32_t job;
    /* job << 32 | last stripe << 16 | next stripe to take */
    uint64_t next;
    unsigned stripes_done;
    /* bus-memory textures of the primitive in hand, as host copies */
    uint32_t *shadow[R300_TEX_UNITS];
    size_t shadow_size[R300_TEX_UNITS];
    uint32_t shadow_base[R300_TEX_UNITS];
    uint32_t shadow_dw[R300_TEX_UNITS];
    bool shadowed[R300_TEX_UNITS];
} R300Raster;

/*
 * A texture dword outside VRAM: the host copy while one is in force,
 * the bus otherwise.
 */
static inline uint32_t r300_tex_bus32(ATIR350State *s, unsigned unit,
                                      uint32_t addr)
{
    R300Raster *q = s->raster;

    if (q && q->shadowed[unit]) {
        uint32_t i = (addr - q->shadow_base[unit]) >> 2;

        return i < q->shadow_dw[unit] ? q->shadow[unit][i] : 0;
    }
    return ati_r350_mc_read32(s, addr);
}

static inline float r300_f32(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

/*
 * A FRAGMENT-STAGE CONSTANT, out of PFS_PARAM / US_ALU_CONST.
 *
 * These are 24-bit floats and they are NOT an IEEE float with the low
 * mantissa byte dropped, which is what this model read them as for
 * months. The format is its own: sign at bit 23, a SEVEN-bit exponent
 * biased 63 at [22:16], and sixteen mantissa bits at [15:0] -- so an
 * IEEE value is packed by rebiasing the exponent (127 -> 63) and
 * shifting the mantissa down by seven, and unpacked by undoing both.
 * Exponent zero is zero.
 *
 * The truncation reading is wrong by a factor of 2^(e - 63 - (e' - 127))
 * that varies with the value, which is why it was invisible: every
 * constant the 10.4 and OS 9 corpus actually READS is a literal 0.0,
 * 1.0 or 0.5 taken from the instruction's own source SELECT, never from
 * this file. Mac OS X 10.5's compositor is the first guest to multiply
 * by one, and the six it uses in a menu draw decode under this rule to
 * exactly 1/width and 1/height of the three textures that draw samples
 * -- six numbers agreeing with six dimensions that come from a
 * different register block entirely, which is what settled the format.
 */
static inline float r300_us_f24(uint32_t v)
{
    uint32_t sign = v & (1u << 23);
    uint32_t exp = (v >> 16) & 0x7f;

    if (!exp) {
        return sign ? -0.0f : 0.0f;
    }
    return r300_f32((sign << 8) | ((exp + 127 - 63) << 23) |
                    ((v & 0xffff) << 7));
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

/* TX_FMT_5_6_5 (code 0x6): no W component, read as one */
static inline uint32_t r300_texel_565(uint32_t v)
{
    uint32_t y = (v >> 5) & 0x3f;

    return r300_pack_xyzw(r300_c5to8(v & 0x1f), (y << 2) | (y >> 4),
                          r300_c5to8((v >> 11) & 0x1f), 0xff);
}

/* TX_FMT_4_4_4_4 (code 0xa) */
static inline uint32_t r300_texel_4444(uint32_t v)
{
    return r300_pack_xyzw((v & 0xf) * 0x11, ((v >> 4) & 0xf) * 0x11,
                          ((v >> 8) & 0xf) * 0x11, ((v >> 12) & 0xf) * 0x11);
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

static inline bool r300_tex_yuv422(unsigned code)
{
    return code == R300_TX_FMT_VYUY422 || code == R300_TX_FMT_YVYU422;
}

/*
 * A 4:2:2 texel as (X,Y,Z,W) = (U,Y,V,A) with YUV_TO_RGB off, or
 * (B,G,R,A) with it on; TX_FORMAT1's component select is ignored for
 * YUV formats (R3xx register reference). The conversion is BT.601
 * with Y in 16-235 and chroma in 16-240. Components are eight bits
 * here, so the unclamped mode clamps too. SWAP_YUV inverts the top bit
 * of U and V.
 */
static inline uint32_t r300_texel_yuv(unsigned mode, int y, int cb, int cr,
                                      uint32_t a)
{
    int c, d, e;

    if (mode & 4) {
        cb ^= 0x80;
        cr ^= 0x80;
    }
    if (!(mode & 3)) {
        return r300_pack_xyzw(cb, y, cr, a);
    }
    c = 298 * (y - 16) + 128;
    d = cb - 128;
    e = cr - 128;
    return r300_pack_xyzw(MIN(MAX((c + 516 * d) >> 8, 0), 255),
                          MIN(MAX((c - 100 * d - 208 * e) >> 8, 0), 255),
                          MIN(MAX((c + 409 * e) >> 8, 0), 255), a);
}

/*
 * One texel of a 4:2:2 pair, from the pair's dword with the lowest
 * address in the low byte. VYUY is Y0 U Y1 V and YVYU is U Y0 V Y1 from
 * the low byte up (R5xx acceleration guide, C_VYUY / C_YVYU). Both
 * texels of a pair take its U and V. Filtering blends texels after the
 * conversion, so chroma is interpolated between pairs.
 */
static inline uint32_t r300_texel_422(const R300TexUnit *u, uint32_t p,
                                      bool odd)
{
    unsigned sh = u->code == R300_TX_FMT_VYUY422 ? 0 : 8;

    return r300_texel_yuv(u->yuv, (p >> (sh + (odd ? 16 : 0))) & 0xff,
                          (p >> (8 - sh)) & 0xff, (p >> (24 - sh)) & 0xff,
                          0xff);
}

/*
 * TX_OFFSET's ENDIAN_SWAP applies to a texture fetched over the bus,
 * as COLORENDIAN does to a colour buffer in GART; in VRAM the surface
 * swapper decides the byte order. 16-bit, 32-bit and half-dword swaps
 * are the lane xors 1, 3 and 2 within each dword.
 */
static inline uint32_t r300_lane_xor32(uint32_t v, unsigned x)
{
    switch (x) {
    case 1:
        return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
    case 2:
        return (v << 16) | (v >> 16);
    case 3:
        return bswap32(v);
    default:
        return v;
    }
}

/* the texel at card address `addr`, in the unit's format */
static uint32_t r300_tex_at(ATIR350State *s, const R300DrawState *d,
                            unsigned unit, uint32_t addr)
{
    const R300TexUnit *u = &d->tex[unit];
    uint32_t off;

    if (u->bpp == 8) {
        /* single-component format: the byte is component X */
        uint8_t a;

        if (ati_r350_mc_to_vram(s, addr, &off)) {
            if (off + 1 > ATI_R350_VRAM_SIZE) {
                return 0;
            }
            a = ((uint8_t *)memory_region_get_ram_ptr(&s->vram))
                [off ^ (ati_r350_vram_xor(s, off) & 3)];
        } else {
            a = (r300_tex_bus32(s, unit, addr & ~3u) >>
                 (((addr ^ u->lanes) & 3) * 8)) & 0xff;
        }
        return a;
    }
    if (u->bpp == 16) {
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

        if (r300_tex_yuv422(u->code)) {
            /* the pair's dword, through the same swapper */
            if (!ati_r350_mc_to_vram(s, addr & ~3u, &off)) {
                v = r300_lane_xor32(r300_tex_bus32(s, unit, addr & ~3u),
                                    u->lanes);
            } else {
                v = ati_r350_vram_ld32(s, off);
            }
            return r300_texel_422(u, v, addr & 2);
        }
        if (!ati_r350_mc_to_vram(s, addr, &off)) {
            v = (r300_lane_xor32(r300_tex_bus32(s, unit, addr & ~3u),
                                 u->lanes) >> ((addr & 2) * 8)) & 0xffff;
        } else if (off + 2 > ATI_R350_VRAM_SIZE) {
            return 0;
        } else {
            xr = ati_r350_vram_xor(s, off);
            v = (uint32_t)d->vram[off ^ xr] |
                ((uint32_t)d->vram[(off + 1) ^ xr] << 8);
        }
        switch (u->code) {
        case R300_TX_FMT_1_5_5_5:
            return r300_texel_1555(v);
        case R300_TX_FMT_5_6_5:
            return r300_texel_565(v);
        case R300_TX_FMT_4_4_4_4:
            return r300_texel_4444(v);
        default:
            return v;
        }
    }
    if (u->bpp == 64) {
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
        return r300_texel_16x4(
            r300_lane_xor32(r300_tex_bus32(s, unit, addr), u->lanes),
            r300_lane_xor32(r300_tex_bus32(s, unit, addr + 4), u->lanes));
    }
    if (ati_r350_mc_to_vram(s, addr, &off)) {
        if (off + 4 > ATI_R350_VRAM_SIZE) {
            return 0;
        }
        return ati_r350_vram_ld32(s, off);
    }
    /* texture staged in GART/system memory */
    return r300_lane_xor32(r300_tex_bus32(s, unit, addr), u->lanes);
}

static uint32_t r300_sample_tex(ATIR350State *s, const R300DrawState *d,
                                unsigned unit, int tx, int ty)
{
    const R300TexUnit *u = &d->tex[unit];

    /*
     * TX_FILTER0 clamp modes: 0 is wrap/repeat -- OS X paints its
     * title-bar gradient by drawing a 16x20 tile as a window-wide
     * quad and letting the sampler repeat it; clamping instead
     * smeared whatever sat next to the tile in VRAM. Treat mirror
     * modes as repeat, everything else clamps to the edge.
     */
    if (u->clamp_s <= 1 && u->w > 0) {
        tx %= u->w;
        if (tx < 0) {
            tx += u->w;
        }
    } else {
        tx = MIN(MAX(tx, 0), u->w - 1);
    }
    if (u->clamp_t <= 1 && u->h > 0) {
        ty %= u->h;
        if (ty < 0) {
            ty += u->h;
        }
    } else {
        ty = MIN(MAX(ty, 0), u->h - 1);
    }
    return r300_tex_at(s, d, unit, u->off + (uint32_t)ty * u->pitch +
                                   (uint32_t)tx * (u->bpp / 8));
}

/*
 * FILTERED SAMPLING, for a unit with `filt` set.
 *
 * Coordinates arrive in level-0 texels, with their screen derivatives
 * (ds/dx, dt/dx, ds/dy, dt/dy) in the same units. Every step after the
 * derivatives is integer or exact in floating point, so the GL backend's
 * shader (ati_r350_gl.c, tex_filter) reproduces it bit for bit given the
 * same derivatives:
 *
 *   LOD      log2 in 1/256 levels, read off the float's exponent and the
 *            top eight mantissa bits, halved for the square root; plus
 *            LOD_BIAS. LOD <= 0 magnifies at level MAX_MIP_LEVEL.
 *   levels   the LOD clamped to [MAX_MIP_LEVEL, NUM_LEVELS]; MIP_FILTER
 *            point rounds it, linear blends two levels by its 8-bit
 *            fraction.
 *   texels   point: floor; linear: 2x2 texels around s - 1/2, weights
 *            quantised to 1/256. A weight of 0 or 256 fetches one column
 *            or row, so a 1:1 draw at texel centres reads exactly the
 *            texel nearest sampling would.
 *   aniso    MIN_FILTER anisotropic with MAX_ANISO > 1:1: the footprint's
 *            axis ratio rounded up to a power of two, capped by MAX_ANISO,
 *            is the number of probes spaced along the major axis; the
 *            LOD is taken from the major axis divided by that count.
 *   clamps   0 wrap, 1 mirror, 2 edge, 3 mirror once to edge, 4/5 (mirror
 *            once) half way to the border colour, 6/7 (mirror once) to
 *            the border colour, per the R3xx register reference.
 *
 * Results are the format's packed components, one byte each, like
 * r300_sample_tex()'s: filtering acts on those bytes, before the
 * component select.
 */
static inline float r300_tc_pre(float c, int n, unsigned mode)
{
    if (mode == 3 || mode == 5 || mode == 7) {
        c = fabsf(c);
    }
    if (mode == 4 || mode == 5) {
        c = fminf(fmaxf(c, 0.0f), (float)n);
    }
    return fminf(fmaxf(c, -16777216.0f), 16777216.0f);
}

/* the texel index along one axis, or -1 for the border colour */
static inline int r300_tc_idx(int i, int n, unsigned mode, bool point)
{
    switch (mode) {
    case 0:
        i %= n;
        return i < 0 ? i + n : i;
    case 1: {
        int p = 2 * n;

        i %= p;
        if (i < 0) {
            i += p;
        }
        return i >= n ? p - 1 - i : i;
    }
    case 2:
    case 3:
        return MIN(MAX(i, 0), n - 1);
    case 4:
    case 5:
        if (point) {
            return MIN(MAX(i, 0), n - 1);
        }
        /* fall through */
    default:
        return i < 0 || i >= n ? -1 : i;
    }
}

static inline uint32_t r300_tex_lvl_texel(ATIR350State *s,
                                          const R300DrawState *d,
                                          unsigned unit, unsigned l,
                                          int i, int j)
{
    const R300TexUnit *u = &d->tex[unit];

    if (i < 0 || j < 0) {
        return u->border;
    }
    return r300_tex_at(s, d, unit, u->loff[l] + (uint32_t)j * u->lpitch[l] +
                                   (uint32_t)i * (u->bpp / 8));
}

static inline uint32_t r300_lerp4(uint32_t a, uint32_t b, int f)
{
    uint32_t o = 0;
    unsigned c;

    for (c = 0; c < 32; c += 8) {
        int x = (a >> c) & 0xff, y = (b >> c) & 0xff;

        o |= (uint32_t)((x * (256 - f) + y * f + 128) >> 8) << c;
    }
    return o;
}

/* one level: `fs`, `ft` in level-0 texels */
static uint32_t r300_tex_level(ATIR350State *s, const R300DrawState *d,
                               unsigned unit, unsigned l, float fs, float ft,
                               bool lin)
{
    const R300TexUnit *u = &d->tex[unit];
    int w = u->lw[l], h = u->lh[l];
    float ss = r300_tc_pre(ldexpf(fs, -(int)MIN(l, u->wl2)), w, u->clamp_s);
    float tt = r300_tc_pre(ldexpf(ft, -(int)MIN(l, u->hl2)), h, u->clamp_t);
    float fx, fy, x0, y0;
    int wx, wy, i0, j0, i1, j1;
    uint32_t t00, t10, t01, t11, o = 0;
    unsigned c;

    if (!lin) {
        return r300_tex_lvl_texel(s, d, unit, l,
                                  r300_tc_idx((int)floorf(ss), w,
                                              u->clamp_s, true),
                                  r300_tc_idx((int)floorf(tt), h,
                                              u->clamp_t, true));
    }
    fx = ss - 0.5f;
    fy = tt - 0.5f;
    x0 = floorf(fx);
    y0 = floorf(fy);
    wx = (int)((fx - x0) * 256.0f + 0.5f);
    wy = (int)((fy - y0) * 256.0f + 0.5f);
    i0 = (int)x0;
    j0 = (int)y0;
    if (wx == 256) {
        i0++;
        wx = 0;
    }
    if (wy == 256) {
        j0++;
        wy = 0;
    }
    i1 = r300_tc_idx(i0 + 1, w, u->clamp_s, false);
    j1 = r300_tc_idx(j0 + 1, h, u->clamp_t, false);
    i0 = r300_tc_idx(i0, w, u->clamp_s, false);
    j0 = r300_tc_idx(j0, h, u->clamp_t, false);
    t00 = r300_tex_lvl_texel(s, d, unit, l, i0, j0);
    t10 = wx ? r300_tex_lvl_texel(s, d, unit, l, i1, j0) : t00;
    if (!wy) {
        return wx ? r300_lerp4(t00, t10, wx) : t00;
    }
    t01 = r300_tex_lvl_texel(s, d, unit, l, i0, j1);
    t11 = wx ? r300_tex_lvl_texel(s, d, unit, l, i1, j1) : t01;
    for (c = 0; c < 32; c += 8) {
        int a = (t00 >> c) & 0xff, b = (t10 >> c) & 0xff;
        int e = (t01 >> c) & 0xff, f = (t11 >> c) & 0xff;
        int top = a * (256 - wx) + b * wx;
        int bot = e * (256 - wx) + f * wx;

        o |= (uint32_t)((top * (256 - wy) + bot * wy + 32768) >> 16) << c;
    }
    return o;
}

/* the mip stage at `lod` (1/256 levels, > 0) */
static uint32_t r300_tex_mip(ATIR350State *s, const R300DrawState *d,
                             unsigned unit, int lod, float fs, float ft)
{
    const R300TexUnit *u = &d->tex[unit];
    bool lin = u->min != R300_TX_FILTER_POINT;
    int lo = MIN(MAX(lod, (int)u->first * 256), (int)u->last * 256);
    unsigned l;

    if (u->mip == R300_TX_FILTER_POINT) {
        return r300_tex_level(s, d, unit, MIN((unsigned)(lo + 128) >> 8,
                                              u->last), fs, ft, lin);
    }
    l = lo >> 8;
    if (u->mip != R300_TX_FILTER_LINEAR || l >= u->last || !(lo & 255)) {
        return r300_tex_level(s, d, unit, l, fs, ft, lin);
    }
    return r300_lerp4(r300_tex_level(s, d, unit, l, fs, ft, lin),
                      r300_tex_level(s, d, unit, l + 1, fs, ft, lin),
                      lo & 255);
}

/* log2(v) in 1/256 units: the exponent and the top 8 mantissa bits */
static inline int r300_log2_fx(float v)
{
    uint32_t b;

    if (!(v > 0.0f)) {
        return -65536;
    }
    memcpy(&b, &v, sizeof(b));
    if (b >= 0x7f800000u) {
        return 65536;
    }
    return ((int)(b >> 23) - 127) * 256 + (int)((b >> 15) & 0xff);
}

static uint32_t r300_tex_filter(ATIR350State *s, const R300DrawState *d,
                                unsigned unit, float fs, float ft,
                                const float der[4])
{
    const R300TexUnit *u = &d->tex[unit];
    float ax = 0.0f, ay = 0.0f, px, py, m, n;
    int lod = 0, nl = 0, k, N;
    unsigned sum[4] = { 0, 0, 0, 0 }, c;
    uint32_t o = 0;

    if (u->need_lod) {
        m = der[0] * der[0];
        n = der[1] * der[1];
        px = m + n;
        m = der[2] * der[2];
        n = der[3] * der[3];
        py = m + n;
        if (u->min == R300_TX_FILTER_ANISO) {
            int lmaj = r300_log2_fx(px >= py ? px : py);
            int lmin = r300_log2_fx(px >= py ? py : px);

            nl = MIN(MAX(((lmaj - lmin) / 2 + 255) >> 8, 0),
                     (int)u->aniso_l2);
            lod = (lmaj >> 1) - nl * 256;
            ax = px >= py ? der[0] : der[2];
            ay = px >= py ? der[1] : der[3];
        } else {
            lod = r300_log2_fx(px >= py ? px : py) >> 1;
        }
        lod += u->bias;
    }
    if (lod <= 0) {
        return r300_tex_level(s, d, unit, u->first, fs, ft,
                              u->mag != R300_TX_FILTER_POINT);
    }
    if (!nl) {
        return r300_tex_mip(s, d, unit, lod, fs, ft);
    }
    N = 1 << nl;
    for (k = 0; k < N; k++) {
        float o_k = (float)(2 * k + 1 - N) / (float)(2 * N);
        float ds = ax * o_k, dt = ay * o_k;
        uint32_t t = r300_tex_mip(s, d, unit, lod, fs + ds, ft + dt);

        for (c = 0; c < 4; c++) {
            sum[c] += (t >> (c * 8)) & 0xff;
        }
    }
    for (c = 0; c < 4; c++) {
        o |= ((sum[c] + (N >> 1)) >> nl) << (c * 8);
    }
    return o;
}

/*
 * One output channel of the texture unit. `texel` holds the format's own
 * components packed from the least significant bits up -- X, Y, Z, W --
 * and TX_FORMAT1 says which of them (or a constant) this channel takes.
 * Reading a texel as ARGB regardless is right only for the selector
 * Mac OS X's tiles use; Chess.app's board texture selects the same four
 * bytes in the opposite order, which is a red/blue exchange.
 */
static float r300_texel_chan(const R300TexUnit *u, uint32_t texel,
                             unsigned ch)
{
    unsigned sel = u->sel[ch];

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

/* bytes per colour buffer pixel, 0 for a reserved COLORFORMAT */
static unsigned r300_cb_bytes(unsigned fmt)
{
    switch (fmt) {
    case R300_COLORFORMAT_I8:
        return 1;
    case R300_COLORFORMAT_ARGB1555:
    case R300_COLORFORMAT_RGB565:
    case R300_COLORFORMAT_ARGB4444:
    case R300_COLORFORMAT_VYUY:
    case R300_COLORFORMAT_YVYU:
    case R300_COLORFORMAT_UV88:
        return 2;
    case R300_COLORFORMAT_ARGB8888:
        return 4;
    case R300_COLORFORMAT_ARGB16161616:
        return 8;
    case R300_COLORFORMAT_ARGB32323232:
        return 16;
    default:
        return 0;
    }
}

/* the formats r300_read_dst() and r300_write_dst() can store */
static bool r300_cb_modelled(unsigned fmt)
{
    switch (fmt) {
    case R300_COLORFORMAT_I8:
    case R300_COLORFORMAT_ARGB1555:
    case R300_COLORFORMAT_RGB565:
    case R300_COLORFORMAT_ARGB4444:
    case R300_COLORFORMAT_ARGB8888:
        return true;
    default:
        return false;
    }
}

/*
 * Both take the destination address the caller already computed for the
 * span, and neither marks the region dirty: that is done once per row by
 * r300_raster_tri(), over the range it actually wrote. Marking eight
 * bytes per pixel meant a dirty-bitmap update for every pixel of every
 * triangle, which for a full-screen blended quad is 786432 of them.
 */
/* 16bpp colour buffer pixel <-> ARGB8888 */
static uint32_t r300_cb_unpack16(unsigned fmt, uint32_t v)
{
    uint32_t a, r, g, b;

    switch (fmt) {
    case R300_COLORFORMAT_RGB565:
        a = 0xff;
        r = r300_c5to8((v >> 11) & 0x1f);
        g = ((v >> 5) & 0x3f) << 2 | ((v >> 9) & 3);
        b = r300_c5to8(v & 0x1f);
        break;
    case R300_COLORFORMAT_ARGB4444:
        a = ((v >> 12) & 0xf) * 0x11;
        r = ((v >> 8) & 0xf) * 0x11;
        g = ((v >> 4) & 0xf) * 0x11;
        b = (v & 0xf) * 0x11;
        break;
    default:                                    /* ARGB1555 */
        a = (v >> 15) & 1 ? 0xff : 0;
        r = r300_c5to8((v >> 10) & 0x1f);
        g = r300_c5to8((v >> 5) & 0x1f);
        b = r300_c5to8(v & 0x1f);
        break;
    }
    return a << 24 | r << 16 | g << 8 | b;
}

static uint32_t r300_cb_pack16(unsigned fmt, uint32_t argb)
{
    uint32_t a = argb >> 24, r = (argb >> 16) & 0xff;
    uint32_t g = (argb >> 8) & 0xff, b = argb & 0xff;

    switch (fmt) {
    case R300_COLORFORMAT_RGB565:
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    case R300_COLORFORMAT_ARGB4444:
        return (a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4;
    default:                                    /* ARGB1555 */
        return (a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
    }
}

static uint32_t r300_read_dst(ATIR350State *s, const R300DrawState *d,
                              uint32_t addr)
{
    unsigned xr = d->cb_host ? d->cb_xr : ati_r350_vram_xor(s, addr);

    if (d->cb_bpp == 1) {
        return d->cb[addr ^ xr] * 0x01010101u;
    }
    if (d->cb_bpp == 4) {
        return (uint32_t)d->cb[addr ^ xr] |
               ((uint32_t)d->cb[(addr + 1) ^ xr] << 8) |
               ((uint32_t)d->cb[(addr + 2) ^ xr] << 16) |
               ((uint32_t)d->cb[(addr + 3) ^ xr] << 24);
    }
    return r300_cb_unpack16(d->cb_fmt, (uint32_t)d->cb[addr ^ xr] |
                            (uint32_t)d->cb[(addr + 1) ^ xr] << 8);
}

static void r300_write_dst(ATIR350State *s, const R300DrawState *d,
                           uint32_t addr, uint32_t argb)
{
    unsigned xr;
    uint32_t v;

    if (d->wmask != 0xffffffff) {
        /* masked-off channels keep whatever the destination holds */
        argb = (argb & d->wmask) | (r300_read_dst(s, d, addr) & ~d->wmask);
    }
    xr = d->cb_host ? d->cb_xr : ati_r350_vram_xor(s, addr);
    if (d->cb_bpp == 1) {
        d->cb[addr ^ xr] = argb >> d->cb_sel;
        return;
    }
    if (d->cb_bpp == 2) {
        v = r300_cb_pack16(d->cb_fmt, argb);
        d->cb[addr ^ xr] = v & 0xff;
        d->cb[(addr + 1) ^ xr] = (v >> 8) & 0xff;
        return;
    }
    d->cb[(addr + 0) ^ xr] = argb & 0xff;
    d->cb[(addr + 1) ^ xr] = (argb >> 8) & 0xff;
    d->cb[(addr + 2) ^ xr] = (argb >> 16) & 0xff;
    d->cb[(addr + 3) ^ xr] = (argb >> 24) & 0xff;
}

static void r300_st32(ATIR350State *s, const R300DrawState *d,
                      uint32_t addr, uint32_t val)
{
    unsigned xr = ati_r350_vram_xor(s, addr);

    d->vram[(addr + 0) ^ xr] = val & 0xff;
    d->vram[(addr + 1) ^ xr] = (val >> 8) & 0xff;
    d->vram[(addr + 2) ^ xr] = (val >> 16) & 0xff;
    d->vram[(addr + 3) ^ xr] = (val >> 24) & 0xff;
}

/*
 * Z buffer addressing. The guest reads the depth buffer back through
 * the aperture (Chess.app picks a square that way), so the layout is
 * the one the driver's untiler expects. Measured from the driver's own
 * reads of a two-sample, macro+micro-tiled 24+8 surface of pitch 704:
 * pixel (x, y), sample s, sits at
 *
 *   ((y >> 3) * (pitch / 32) + (x >> 5)) * 2048
 *     + x0 << 2 | y0 << 3 | s << 4 | x1 << 5 | y1 << 6 | y2 << 7
 *     | x2 << 8 | x3 << 9 | x4 << 10          (xN = bit N of x)
 *
 * i.e. a 32-byte micro block of 2x2 pixels x 2 samples and a 2 KB macro
 * block of 16x4 micro blocks. The single-sample form gives the sample
 * bit back to x (4x2 micro block, 8x8 per macro block); no guest has
 * read one back, so it is unmeasured.
 */
static uint32_t r300_zb_addr(const ATIR350State *s, unsigned x, unsigned y,
                             unsigned sample)
{
    uint32_t a;

    if (!s->zb.macro && !s->zb.micro) {
        return s->zb.off + ((uint32_t)y * s->zb.pitch + x) * 4;
    }
    if (s->zb.aa) {
        a = ((x & 1) << 2) | ((y & 1) << 3) | (sample << 4);
        if (s->zb.macro) {
            a |= (((x >> 1) & 1) << 5) | (((y >> 1) & 3) << 6) |
                 (((x >> 2) & 7) << 8);
            a += ((y >> 3) * (s->zb.pitch / 32) + (x >> 5)) * 2048;
        } else {
            a += ((y >> 1) * (s->zb.pitch / 2) + (x >> 1)) * 32;
        }
    } else {
        a = ((x & 1) << 2) | (((x >> 1) & 1) << 3) | ((y & 1) << 4);
        if (s->zb.macro) {
            a |= (((x >> 2) & 1) << 5) | (((y >> 1) & 3) << 6) |
                 (((x >> 3) & 3) << 8) | (((y >> 3) & 1) << 10);
            a += ((y >> 4) * (s->zb.pitch / 32) + (x >> 5)) * 2048;
        } else {
            a += ((y >> 1) * (s->zb.pitch / 4) + (x >> 2)) * 32;
        }
    }
    return s->zb.off + a;
}

/* ZFUNC / STENCILFUNC: `a` is the incoming value, `b` the stored one */
static inline bool r300_zs_cmp(unsigned fn, uint32_t a, uint32_t b)
{
    switch (fn) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a == b;
    case 4: return a >= b;
    case 5: return a > b;
    case 6: return a != b;
    default: return true;
    }
}

static inline uint32_t r300_stencil_op(unsigned op, uint32_t v, uint32_t ref)
{
    switch (op) {
    case 1: return 0;
    case 2: return ref;
    case 3: return MIN(v + 1, 0xffu);
    case 4: return v - (v != 0);
    case 5: return ~v & 0xff;
    case 6: return (v + 1) & 0xff;
    case 7: return (v - 1) & 0xff;
    default: return v;
    }
}

/*
 * Depth and stencil test and write for one pixel; false means the
 * fragment is killed. 24-bit Z sits above the 8 stencil bits.
 */
static bool r300_zb_pixel(ATIR350State *s, const R300DrawState *d,
                          unsigned x, unsigned y, float zf, bool back)
{
    uint32_t addr = r300_zb_addr(s, x, y, 0);
    uint32_t old, znew, zold, sold, snew, val;
    bool zpass, spass = true;

    if (s->zb.z16) {
        /*
         * 16-bit Z, no stencil. Linear within the pitch: the layout
         * of a tiled 16-bit surface is unmeasured, and nothing but
         * this model has been seen to read one.
         */
        unsigned xr;

        addr = s->zb.off + ((uint32_t)y * s->zb.pitch + x) * 2;
        if (addr + 2 > ATI_R350_VRAM_SIZE) {
            return true;
        }
        xr = ati_r350_vram_xor(s, addr);
        zold = d->vram[addr ^ xr] | (uint32_t)d->vram[(addr + 1) ^ xr] << 8;
        znew = (uint32_t)(MIN(MAX(zf, 0.0f), 1.0f) * 65535.0f);
        zpass = !s->zb.z_test || r300_zs_cmp(s->zb.zsc & 7, znew, zold);
        if (zpass && s->zb.z_wr && znew != zold) {
            d->vram[addr ^ xr] = znew & 0xff;
            d->vram[(addr + 1) ^ xr] = znew >> 8;
        }
        return zpass;
    }
    if (addr + 4 > ATI_R350_VRAM_SIZE) {
        return true;
    }
    old = r300_ld32(s, d, addr);
    zold = old >> 8;
    sold = old & 0xff;
    snew = sold;
    znew = (uint32_t)(MIN(MAX(zf, 0.0f), 1.0f) * 16777215.0f);
    zpass = !s->zb.z_test || r300_zs_cmp(s->zb.zsc & 7, znew, zold);
    if (s->zb.s_en) {
        unsigned f = back && s->zb.s_fb ? (s->zb.zsc >> 15) & 0xfff
                                        : (s->zb.zsc >> 3) & 0xfff;
        unsigned op;

        spass = r300_zs_cmp(f & 7, s->zb.s_ref & s->zb.s_mask,
                            sold & s->zb.s_mask);
        op = !spass ? (f >> 3) & 7 : !zpass ? (f >> 9) & 7 : (f >> 6) & 7;
        snew = r300_stencil_op(op, sold, s->zb.s_ref);
        snew = (sold & ~s->zb.s_wmask) | (snew & s->zb.s_wmask);
    }
    if (!(spass && zpass && s->zb.z_wr)) {
        znew = zold;
    }
    if (znew != zold || snew != sold) {
        val = (znew << 8) | snew;
        r300_st32(s, d, addr, val);
        if (s->zb.aa) {
            addr = r300_zb_addr(s, x, y, 1);
            if (addr + 4 <= ATI_R350_VRAM_SIZE) {
                r300_st32(s, d, addr, val);
            }
        }
    }
    return spass && zpass;
}

/*
 * 3D_CLEAR_ZMASK. A zero entry marks its tiles cleared, which reads as
 * ZB_DEPTHCLEARVALUE; the depth buffer here is kept uncompressed, so
 * the clear writes that value into the tiles. One dword covers 32x16
 * pixels (8x4 tiles of 4x4 on two pipes), row-major over
 * ZB_ZMASK_PITCH pixels: a 640x480 clear is 600 dwords.
 */
void ati_r350_r300_clear_zmask(ATIR350State *s, uint32_t first, uint32_t n,
                               uint32_t val)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t zp = s->regs[R300_ZB_DEPTHPITCH >> 2];
    uint32_t clr = s->regs[R300_ZB_DEPTHCLEARVALUE >> 2];
    unsigned bw = (s->regs[R300_ZB_ZMASK_PITCH >> 2] & 0x3fff) / 32;
    unsigned smp = (s->regs[R300_GB_AA_CONFIG >> 2] & R300_AA_ENABLE) ? 2 : 1;
    unsigned zfmt = s->regs[R300_ZB_FORMAT >> 2] & 0xf;
    uint32_t i, off;
    unsigned x, y, k;

    if (val || !bw || !((zp >> 2) & 0xfff) ||
        (zfmt != R300_ZB_FORMAT_24_8 && zfmt != R300_ZB_FORMAT_16) ||
        !ati_r350_mc_to_vram(s, s->regs[R300_ZB_DEPTHOFFSET >> 2] & ~0x1fu,
                             &off)) {
        return;
    }
    s->zb.off = off;
    s->zb.pitch = ((zp >> 2) & 0xfff) * 4;
    s->zb.macro = zp & R300_ZB_MACROTILE;
    s->zb.micro = (zp >> R300_ZB_MICROTILE_SHIFT) & 3;
    s->zb.aa = smp == 2;
    s->zb.z16 = zfmt == R300_ZB_FORMAT_16;
    n = MIN(n, 0x100000);
    for (i = first; i < first + n; i++) {
        unsigned bx = (i % bw) * 32, by = (i / bw) * 16;

        for (y = by; y < by + 16; y++) {
            for (x = bx; x < bx + 32; x++) {
                if (s->zb.z16) {
                    /* the linear layout r300_zb_pixel() uses */
                    uint32_t a = off + (y * s->zb.pitch + x) * 2;
                    unsigned xr;

                    if (a + 2 > ATI_R350_VRAM_SIZE) {
                        continue;
                    }
                    xr = ati_r350_vram_xor(s, a);
                    vram[a ^ xr] = clr & 0xff;
                    vram[(a + 1) ^ xr] = (clr >> 8) & 0xff;
                    continue;
                }
                for (k = 0; k < smp; k++) {
                    uint32_t a = r300_zb_addr(s, x, y, k);
                    unsigned xr;

                    if (a + 4 > ATI_R350_VRAM_SIZE) {
                        continue;
                    }
                    xr = ati_r350_vram_xor(s, a);
                    vram[(a + 0) ^ xr] = clr & 0xff;
                    vram[(a + 1) ^ xr] = (clr >> 8) & 0xff;
                    vram[(a + 2) ^ xr] = (clr >> 16) & 0xff;
                    vram[(a + 3) ^ xr] = (clr >> 24) & 0xff;
                }
            }
        }
    }
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

/*
 * How the fragment executor samples a texture.
 *
 * The interpreter is device-state-free by design, so the fetch reaches
 * it as a callback; this is the device's end of it, and it is where the
 * NORMALISED coordinate the hardware's texture unit takes becomes the
 * texel index this model's sampler wants -- one multiply by the size of
 * the unit BEING FETCHED, which is the only unit whose size the answer
 * can depend on.
 *
 * The guests are the authority on the unit, and they say it in their own
 * constants: Mac OS X 10.5's menu-bar filter binds unit 0 at 1103x24 and
 * carries k21 = (0.000906616, 0.0416665) = (1/1103, 1/24), which it
 * multiplies into every coordinate immediately before the fetch; its
 * neighbour binds 1024x22 and carries (1/1024, 1/22); a third binds
 * 32x22 and carries (1/32, 1/22). A program that divides by the bound
 * size just before LD is a program whose LD consumes a normalised
 * coordinate.
 *
 * LD and PROJ still do the same thing: r300_fs_frame() puts 1.0 in the
 * routed coordinate's fourth component, so the projective divide a
 * directly-routed coordinate would take is the identity. A COMPUTED
 * coordinate with a real q is not yet divided by it -- see the note in
 * doc; that is a separate gap and not this one.
 *
 * A unit this draw does not bind reads WHITE, not black: the same 1x1
 * white texture the GL backend binds for an untextured draw, and the
 * value that leaves a modulate program computing its colour operand
 * alone rather than blacking the draw out.
 */
typedef struct R300SampleCtx {
    ATIR350State *s;
    const R300DrawState *d;
    /*
     * Screen derivatives of each interpolated coordinate set, in the
     * guest's units: ds/dx, dt/dx, ds/dy, dt/dy. Filled only when a bound
     * unit needs a level of detail.
     */
    float der[R300_TEXCOORDS][4];
} R300SampleCtx;

/*
 * The footprint of a fetch is that of the coordinate set the rasterizer
 * routed into its source register. A dependent read's coordinate is an
 * ALU result, whose derivatives this model does not have: it takes those
 * of the set that addresses the same unit, else of set 0.
 */
static const float *r300_us_der(const R300SampleCtx *c, unsigned unit,
                                unsigned src)
{
    const R300DrawState *d = c->d;
    unsigned n;

    for (n = 0; n < d->ntc; n++) {
        if (d->fs->rs.tex_reg[n] == (int)src) {
            return c->der[n];
        }
    }
    for (n = 0; n < d->ntc; n++) {
        if (d->tc_unit[n] == unit) {
            return c->der[n];
        }
    }
    return c->der[0];
}

static void r300_us_sample(void *ctx, unsigned unit, bool proj,
                           unsigned src, const float coord[4],
                           float texel[4])
{
    R300SampleCtx *c = ctx;
    const R300DrawState *d = c->d;
    const R300TexUnit *u;
    uint32_t t;

    if (unit >= R300_TEX_UNITS || !d->tex[unit].en) {
        texel[0] = texel[1] = texel[2] = texel[3] = 1.0f;
        return;
    }
    u = &d->tex[unit];
    if (u->filt) {
        float der[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        if (u->need_lod) {
            const float *g = r300_us_der(c, unit, src);

            der[0] = g[0] * (float)u->w;
            der[1] = g[1] * (float)u->h;
            der[2] = g[2] * (float)u->w;
            der[3] = g[3] * (float)u->h;
        }
        t = r300_tex_filter(c->s, d, unit, coord[0] * (float)u->w,
                            coord[1] * (float)u->h, der);
    } else {
        t = r300_sample_tex(c->s, d, unit, (int)(coord[0] * (float)u->w),
                            (int)(coord[1] * (float)u->h));
    }
    texel[0] = r300_texel_chan(u, t, 1);
    texel[1] = r300_texel_chan(u, t, 2);
    texel[2] = r300_texel_chan(u, t, 3);
    texel[3] = r300_texel_chan(u, t, 0);
}

/*
 * The screen derivatives of one interpolated attribute at a pixel whose
 * interpolated value is `v`, from the corners' values `t0..t2`, the
 * weights' gradients ga (x) and gb (y) -- already multiplied by each
 * corner's 1/w for a perspective triangle -- and iq, the reciprocal of
 * the interpolated 1/w (1 when affine):
 *
 *   dv/dx = iq * sum(ga_i * (t_i - v))
 *
 * One operation per statement: the GL backend's shader evaluates the
 * same expression and has to round exactly where this does.
 */
static inline void r300_tc_der(const float ga[3], const float gb[3],
                               float t0, float t1, float t2, float v,
                               float iq, float *dx, float *dy)
{
    float e0 = t0 - v, e1 = t1 - v, e2 = t2 - v;
    float m0 = ga[0] * e0, m1 = ga[1] * e1, m2 = ga[2] * e2;
    float r = m0 + m1;

    r = r + m2;
    *dx = r * iq;
    m0 = gb[0] * e0;
    m1 = gb[1] * e1;
    m2 = gb[2] * e2;
    r = m0 + m1;
    r = r + m2;
    *dy = r * iq;
}

/*
 * The pixel stack frame the fragment program starts from: the
 * rasterizer's own outputs, dropped into the registers RS_INST named.
 * Registers it does not name read zero, which is what the hardware's
 * frame holds and what the GLSL translation declares.
 *
 * This is the whole of the interface between the two stages. What used
 * to sit here instead was `colour *= texel` for every textured draw --
 * right for two of the ten programs the guests in this project's corpus
 * upload and wrong for the rest.
 */
static inline void r300_fs_frame(const R300DrawState *d, R300UsRegs *f,
                                 const float tc[R300_TEXCOORDS][4],
                                 const float col[2][4])
{
    const R300UsProgram *p = d->fs;
    unsigned n;

    for (n = 0; n < p->nregs_used; n++) {
        f->r[n][0] = f->r[n][1] = f->r[n][2] = f->r[n][3] = 0.0f;
    }
    f->out[0] = f->out[1] = f->out[2] = f->out[3] = 0.0f;
    f->kill = false;
    /*
     * The interpolated texture COORDINATE, in the units the GUEST'S OWN
     * VERTEX PROGRAM emitted it in -- which is the whole of what a
     * fragment program is entitled to see, because the hardware's
     * rasterizer interpolates that output and hands it over untouched.
     *
     * The caller has already divided the interpolant by the size the
     * vertex stage multiplied it by, so what arrives here is the guest's
     * number back again and every constant the program adds to it is in
     * the same units it is. Getting that wrong is not academic: 10.5's
     * menu-bar filter offsets its coordinate by a constant before
     * normalising, and against a value 1103 times too large the offset
     * simply disappears -- which is the ramp that saturates along the
     * bottom edge of the bar. The nine-part Aqua strips are worse still:
     * they tile with FRC, and FRC of a texel count is a number in [0,1)
     * that (int) turns into texel (0,0) for every pixel of every draw in
     * the family.
     *
     * Verified against the whole offline corpus -- 29 of 29 LD/PROJ
     * fetches name exactly this register as their source -- so for every
     * program that predates indirection levels the executor samples with
     * exactly what the routing put here.
     */
    for (n = 0; n < R300_TEXCOORDS; n++) {
        if (p->rs.tex_reg[n] >= 0) {
            float *r = f->r[p->rs.tex_reg[n]];

            r[0] = tc[n][0]; r[1] = tc[n][1]; r[2] = tc[n][2]; r[3] = tc[n][3];
        }
    }
    for (n = 0; n < R300_US_RS_COLS; n++) {
        if (p->rs.col_reg[n] >= 0) {
            const float *c = col[p->rs.col_pkt[n]];
            float *r = f->r[p->rs.col_reg[n]];

            r[0] = c[0]; r[1] = c[1]; r[2] = c[2]; r[3] = c[3];
        }
    }
}

/*
 * area > 0 is clockwise as displayed, Y growing downwards. `cull` is
 * RE_CULL_CNTL's cull-front, cull-back and clockwise-front bits, plus
 * bit 3 for a primitive that has no back and bit 4 for a triangle
 * wound against its strip.
 */
static inline bool r300_back_face(unsigned cull, float area)
{
    bool cw = (area > 0.0f) != !!(cull & 16);

    return !(cull & 8) && cw != !!(cull & 4);
}

/* the pixels a triangle's scan covers: [x0, x1) x [y0, y1) */
static inline void r300_tri_bounds(const R300DrawState *d,
                                   const R300Vtx *v0, const R300Vtx *v1,
                                   const R300Vtx *v2, int *x0, int *y0,
                                   int *x1, int *y1)
{
    *x0 = (int)floorf(MIN(v0->x, MIN(v1->x, v2->x)));
    *y0 = (int)floorf(MIN(v0->y, MIN(v1->y, v2->y)));
    *x1 = (int)ceilf(MAX(v0->x, MAX(v1->x, v2->x)));
    *y1 = (int)ceilf(MAX(v0->y, MAX(v1->y, v2->y)));
    *x0 = MAX(*x0, MAX(d->sc_x0, 0));
    *y0 = MAX(*y0, MAX(d->sc_y0, 0));
    /*
     * scissor right/bottom are inclusive; the VRAM bound in the pixel
     * helpers is the real limit beyond that
     */
    *x1 = MIN(*x1, MIN(d->sc_x1 + 1, 8191));
    *y1 = MIN(*y1, MIN(d->sc_y1 + 1, 8191));
}

/*
 * One triangle, over the screen rows [ylo, yhi] only. `cull` is the
 * r300_back_face() word for this triangle. Every pixel depends on the
 * triangle and on nothing but its own position, so drawing the rows in
 * pieces draws exactly the pixels drawing them at once does.
 */
static void r300_raster_tri(ATIR350State *s, const R300DrawState *d,
                            const R300Vtx *v0, const R300Vtx *v1,
                            const R300Vtx *v2, unsigned cull,
                            int ylo, int yhi)
{
    float area = r300_edge(v0, v1, v2->x, v2->y);
    float inv, dx0, dy0, dx1, dy1, dx2, dy2;
    float a0, b0, c0, a1, b1, c1;
    float ga[3] = { 0.0f, 0.0f, 0.0f }, gb[3] = { 0.0f, 0.0f, 0.0f };
    float tcinv[R300_TEXCOORDS][2] = { { 1.0f, 1.0f } };
    bool flip, tl0, tl1, tl2, back;
    bool persp = v0->w != v1->w || v1->w != v2->w;
    int x0, y0, x1, y1, x, y;
    unsigned n;

    if (area == 0.0f) {
        return;
    }
    back = r300_back_face(cull, area);
    if (cull & (back ? 2 : 1)) {
        return;
    }
    /*
     * WHAT UNDOES THE VERTEX STAGE'S SCALING, once per triangle.
     *
     * r300_vs_texcoord() multiplies each coordinate set by the size of
     * the unit r300_us_setup() decided fetches with it, so the value the
     * rasterizer interpolates is in TEXELS of that unit -- which is what
     * the specialised executor and the GL backend read straight, and
     * what every capture on disk records. The fragment INTERPRETER wants
     * the other thing: the guest's own number, because the guest's own
     * program does arithmetic on it. Dividing once per triangle and
     * folding the reciprocal into the per-pixel interpolation is the
     * cheapest place the two can be reconciled, and it keeps the pair of
     * scalings exact for a program that only fetches -- the same two
     * factors, applied in the other order.
     *
     * Only the sets the routing named are computed -- one, for every
     * draw that predates multitexturing -- because those are the only
     * ones the per-pixel loop reads. The declaration's initialiser is
     * there for a compiler that cannot follow this guard to the one
     * below, and for nothing else.
     */
    if (d->fs_run && !d->fs->fast) {
        for (n = 0; n < d->ntc && n < R300_TEXCOORDS; n++) {
            const R300TexUnit *u = &d->tex[d->tc_unit[n]];

            tcinv[n][0] = u->w ? 1.0f / (float)u->w : 1.0f;
            tcinv[n][1] = u->h ? 1.0f / (float)u->h : 1.0f;
        }
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

    /*
     * The weights' screen gradients, for the texture footprint: w0 and
     * w1 change by a0/a1 per pixel in x and b0/b1 in y, w2 by the rest;
     * for a perspective triangle each is scaled by its corner's 1/w.
     */
    if (d->lod_any) {
        ga[0] = a0;
        ga[1] = a1;
        ga[2] = -(a0 + a1);
        gb[0] = b0;
        gb[1] = b1;
        gb[2] = -(b0 + b1);
        if (persp) {
            ga[0] = ga[0] * v0->w;
            ga[1] = ga[1] * v1->w;
            ga[2] = ga[2] * v2->w;
            gb[0] = gb[0] * v0->w;
            gb[1] = gb[1] * v1->w;
            gb[2] = gb[2] * v2->w;
        }
    }

    r300_tri_bounds(d, v0, v1, v2, &x0, &y0, &x1, &y1);
    y0 = MAX(y0, ylo);
    if (yhi < y1) {
        y1 = yhi + 1;
    }

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
            float pw0, pw1, pw2, iq = 1.0f;
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
            if (!d->wmask) {
                /* depth-only pass: nothing to shade */
                if (s->zb.z_en) {
                    r300_zb_pixel(s, d, x, y,
                                  w0 * v0->z + w1 * v1->z + w2 * v2->z,
                                  back);
                }
                continue;
            }
            addr = row + (uint32_t)x * d->cb_bpp;
            if (addr + d->cb_bpp > d->cb_size) {
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
                               (uint32_t)x * d->cb_bpp;

                if (src + d->cb_bpp <= ATI_R350_VRAM_SIZE) {
                    r300_write_dst(s, d, addr, r300_read_dst(s, d, src));
                }
                continue;
            }
            if (persp) {
                /*
                 * Attributes interpolate perspective-correctly: linear
                 * in screen space divided by w, renormalised by the
                 * interpolated 1/w (v->w). Z stays screen-linear.
                 */
                float q0 = w0 * v0->w, q1 = w1 * v1->w, q2 = w2 * v2->w;

                iq = 1.0f / (q0 + q1 + q2);
                pw0 = q0 * iq;
                pw1 = q1 * iq;
                pw2 = q2 * iq;
            } else {
                pw0 = w0;
                pw1 = w1;
                pw2 = w2;
            }
            cr = pw0 * v0->r + pw1 * v1->r + pw2 * v2->r;
            cg = pw0 * v0->g + pw1 * v1->g + pw2 * v2->g;
            cb = pw0 * v0->b + pw1 * v1->b + pw2 * v2->b;
            ca = pw0 * v0->a + pw1 * v1->a + pw2 * v2->a;
            if (d->fs_run) {
                float tex[4], col[2][4], fsout[4];
                float ts = pw0 * v0->tc[0][0] + pw1 * v1->tc[0][0] +
                           pw2 * v2->tc[0][0];
                float tt = pw0 * v0->tc[0][1] + pw1 * v1->tc[0][1] +
                           pw2 * v2->tc[0][1];

                if (d->fs->fast) {
                    /*
                     * The specialised path is handed a finished texel,
                     * so its one fetch is done for it here -- the same
                     * sampler the executor would have called, hoisted
                     * out of a program that cannot need a second one.
                     * `fast` is granted only to a program whose single
                     * fetch is unit 0 addressed by coordinate set 0, so
                     * the hoist has exactly one thing to sample and the
                     * further sets below are never its business.
                     */
                    if (d->tex[0].en && d->fs->tex_dst >= 0) {
                        uint32_t texel;

                        if (d->tex[0].filt) {
                            float der[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

                            if (d->tex[0].need_lod) {
                                r300_tc_der(ga, gb, v0->tc[0][0],
                                            v1->tc[0][0], v2->tc[0][0], ts,
                                            iq, &der[0], &der[2]);
                                r300_tc_der(ga, gb, v0->tc[0][1],
                                            v1->tc[0][1], v2->tc[0][1], tt,
                                            iq, &der[1], &der[3]);
                            }
                            texel = r300_tex_filter(s, d, 0, ts, tt, der);
                        } else {
                            texel = r300_sample_tex(s, d, 0, (int)ts,
                                                    (int)tt);
                        }

                        tex[0] = r300_texel_chan(&d->tex[0], texel, 1);
                        tex[1] = r300_texel_chan(&d->tex[0], texel, 2);
                        tex[2] = r300_texel_chan(&d->tex[0], texel, 3);
                        tex[3] = r300_texel_chan(&d->tex[0], texel, 0);
                    } else {
                        tex[0] = tex[1] = tex[2] = tex[3] = 1.0f;
                    }
                }
                col[0][0] = cr;
                col[0][1] = cg;
                col[0][2] = cb;
                col[0][3] = ca;
                if (d->fs_col1) {
                    col[1][0] = pw0 * v0->r1 + pw1 * v1->r1 + pw2 * v2->r1;
                    col[1][1] = pw0 * v0->g1 + pw1 * v1->g1 + pw2 * v2->g1;
                    col[1][2] = pw0 * v0->b1 + pw1 * v1->b1 + pw2 * v2->b1;
                    col[1][3] = pw0 * v0->a1 + pw1 * v1->a1 + pw2 * v2->a1;
                } else {
                    col[1][0] = col[1][1] = col[1][2] = col[1][3] = 0.0f;
                }
                if (d->fs->fast) {
                    /*
                     * Every program any guest in this project uploads is
                     * of the shape the analyser resolves at decode time.
                     * Walking the interpreter's thirty-two-register
                     * frame and operand switch per pixel instead cost
                     * this rasterizer 38 % of its frame rate on the
                     * Flurry workload; this arm costs 21 %, and the
                     * offline harness holds the two BIT-IDENTICAL.
                     *
                     * `fsout` is a local of its own rather than a member
                     * of the interpreter's frame: that frame is five
                     * hundred bytes whose address escapes into
                     * r300_fs_frame(), which stops the compiler keeping
                     * the shaded colour in registers at all.
                     */
                    r300_us_run_fast(d->fs, tex, col[0], col[1], fsout);
                } else {
                    R300SampleCtx sc = { .s = s, .d = d };
                    R300UsRegs f;
                    float tc[R300_TEXCOORDS][4];
                    unsigned k, c;

                    /*
                     * The further coordinate sets are interpolated only
                     * on this arm. A program that needs one is running
                     * the general interpreter anyway; the specialised
                     * path above is a single unit-0 fetch by
                     * construction, so it must not pay for them.
                     * `ntc` is how many sets the routing named, so a
                     * one-coordinate program does no extra work at all.
                     *
                     * Each set leaves the interpolation in the guest's
                     * own units -- the triangle's reciprocals undoing
                     * the vertex stage's multiply by the bound size, so
                     * that the program's arithmetic and its constants
                     * are in the same units as each other.
                     */
                    tc[0][0] = ts * tcinv[0][0];
                    tc[0][1] = tt * tcinv[0][1];
                    tc[0][2] = 0.0f;
                    tc[0][3] = 1.0f;
                    for (k = 0; k < d->ntc; k++) {
                        float is, it;

                        if (k) {
                            is = pw0 * v0->tc[k][0] + pw1 * v1->tc[k][0] +
                                 pw2 * v2->tc[k][0];
                            it = pw0 * v0->tc[k][1] + pw1 * v1->tc[k][1] +
                                 pw2 * v2->tc[k][1];
                            tc[k][0] = is * tcinv[k][0];
                            tc[k][1] = it * tcinv[k][1];
                            tc[k][2] = 0.0f;
                            tc[k][3] = 1.0f;
                        } else {
                            is = ts;
                            it = tt;
                        }
                        if (d->lod_any) {
                            float *g = sc.der[k];

                            r300_tc_der(ga, gb, v0->tc[k][0], v1->tc[k][0],
                                        v2->tc[k][0], is, iq, &g[0], &g[2]);
                            r300_tc_der(ga, gb, v0->tc[k][1], v1->tc[k][1],
                                        v2->tc[k][1], it, iq, &g[1], &g[3]);
                            g[0] *= tcinv[k][0];
                            g[1] *= tcinv[k][1];
                            g[2] *= tcinv[k][0];
                            g[3] *= tcinv[k][1];
                        }
                    }
                    for (; k < R300_TEXCOORDS; k++) {
                        tc[k][0] = tc[k][1] = tc[k][2] = 0.0f;
                        tc[k][3] = 1.0f;
                    }
                    for (k = 0; d->tc_raw && k < d->ntc; k++) {
                        if (!(d->tc_raw & (1u << k))) {
                            continue;
                        }
                        for (c = 0; c < 4; c++) {
                            tc[k][c] = pw0 * v0->tcr[k][c] +
                                       pw1 * v1->tcr[k][c] +
                                       pw2 * v2->tcr[k][c];
                        }
                    }
                    r300_fs_frame(d, &f, tc, col);
                    r300_us_run(d->fs, &f, r300_us_sample, &sc);
                    if (f.kill) {
                        continue;       /* TEXKILL */
                    }
                    fsout[0] = f.out[0];
                    fsout[1] = f.out[1];
                    fsout[2] = f.out[2];
                    fsout[3] = f.out[3];
                }
                cr = fsout[0];
                cg = fsout[1];
                cb = fsout[2];
                ca = fsout[3];
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
            if (s->zb.z_en &&
                !r300_zb_pixel(s, d, x, y,
                               w0 * v0->z + w1 * v1->z + w2 * v2->z,
                               back)) {
                continue;
            }
            if (d->discard) {
                /*
                 * DISCARD_SRC_PIXELS: skip the colour write for source
                 * values that could not change the destination under
                 * the configured blend, before it costs a read. The
                 * blender runs after the depth and stencil test, so a
                 * discarded fragment still writes Z.
                 */
                bool a_zero = ca == 0.0f, a_one = ca == 1.0f;
                bool rgb_black = cr == 0.0f && cg == 0.0f && cb == 0.0f;
                bool rgb_white = cr == 1.0f && cg == 1.0f && cb == 1.0f;
                bool kill;

                switch (d->discard) {
                case 1:
                    kill = a_zero;
                    break;
                case 2:
                    kill = rgb_black;
                    break;
                case 3:
                    kill = a_zero && rgb_black;
                    break;
                case 4:
                    kill = a_one;
                    break;
                case 5:
                    kill = rgb_white;
                    break;
                case 6:
                    kill = a_one && rgb_white;
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
        if (dirty && !d->cb_host) {
            /*
             * One dirty update for the row's whole written extent. The
             * range can cover a few pixels the span skipped after
             * marking them -- an alpha test or a discard rule can still
             * reject one -- which costs a redraw of pixels that did not
             * change and never the other way round.
             */
            uint64_t lo = dirty_lo & ~7ull;
            uint64_t hi = (dirty_hi + d->cb_bpp + 7) & ~7ull;

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
static void r300_xform_vtx(const ATIR350State *s, const R300DrawState *d,
                           R300Vtx *v, const float *clip)
{
    uint32_t f = s->zb.vte_fmt;
    float cx, cy, cz, cw, zw;

    if (!d->xform) {
        v->w = 1.0f;
        return;
    }
    if (clip) {
        /* the vertex program computed this position itself */
        cx = clip[0];
        cy = clip[1];
        cz = clip[2];
        cw = clip[3];
    } else {
        cx = d->mat[0] * v->x + d->mat[1] * v->y +
             d->mat[2] * v->z + d->mat[3] * v->w;
        cy = d->mat[4] * v->x + d->mat[5] * v->y +
             d->mat[6] * v->z + d->mat[7] * v->w;
        cz = d->mat[8] * v->x + d->mat[9] * v->y +
             d->mat[10] * v->z + d->mat[11] * v->w;
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
    zw = cw;
    v->w = 1.0f / cw;
    if ((f & (R300_VTE_VTX_XY_FMT | R300_VTE_VTX_W0_FMT)) !=
        R300_VTE_VTX_W0_FMT) {
        /*
         * VTX_XY_FMT: x and y already carry the divide. VTX_W0_FMT
         * clear: the w handed over is 1/w already, so the divide is a
         * multiply by it.
         */
        float rw = (f & R300_VTE_VTX_W0_FMT) ? 1.0f / cw : cw;

        if (!(f & R300_VTE_VTX_XY_FMT)) {
            cx *= rw;
            cy *= rw;
        }
        if (!(f & R300_VTE_VTX_Z_FMT)) {
            cz *= rw;
        }
        cw = zw = 1.0f;
        v->w = rw;
    } else if (f & R300_VTE_VTX_Z_FMT) {
        zw = 1.0f;          /* z already carries the divide */
    }
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
    if (s->zb.vte_zs) {
        v->z = s->zb.vte_zo ? (cz / zw) * d->vp[4] + d->vp[5]
                            : (cz / zw) * d->vp[4];
    } else {
        v->z = s->zb.vte_zo ? cz / zw + d->vp[5] : cz / zw;
    }
    if (!isfinite(v->x) || !isfinite(v->y)) {
        v->x = v->y = r300_vtx_nowhere;
    }
    if (!isfinite(v->w) || v->w <= 0.0f) {
        v->w = 1.0f;
    }
}

/*
 * THE FORMAT EACH VERTEX ELEMENT ARRIVES IN, indexed by the vertex
 * program INPUT REGISTER it feeds -- the same index `attr_size` uses.
 *
 * It is filled in by r300_stream_route() and ONLY when the stream
 * control registers demonstrably describe the vertex in hand (see the
 * MISFIT census in that function's comment). A zeroed R300VtxFmt means
 * "the registers were not believed", and every reader then falls back
 * to the raw-float read this model did before there was a decoder --
 * which is exactly the behaviour of every capture and every guest that
 * renders correctly today.
 *
 * It is deliberately NOT part of R300DrawState. That structure is
 * written verbatim into a draw capture and its size is the capture
 * format's version number, so growing it would invalidate every corpus
 * on disk -- and a capture could not use these fields anyway, since it
 * records vertices that have already been through the vertex stage.
 */
typedef struct R300VtxFmt {
    bool valid;                     /* the registers described this vertex */
    uint8_t type[R300_AOS_MAX];     /* DATA_TYPE */
    uint16_t snf[R300_AOS_MAX];     /* SIGNED / NORMALIZE, as the reg bits */
    uint8_t meth[R300_AOS_MAX];     /* VAP_PSC_SGN_NORM_CNTL method */
    uint16_t swz[R300_AOS_MAX];     /* PROG_STREAM_CNTL_EXT half-word */
} R300VtxFmt;

/* the types this model fetches as plain IEEE floats, one per component */
static bool r300_psc_is_float(unsigned type)
{
    return type <= R300_PSC_TYPE_FLOAT_4;
}

/*
 * ONE FIXED-POINT COMPONENT, CONVERTED THE WAY THE VAP CONVERTS IT.
 *
 * `raw` is the field's bits, `bits` how many of them there are. SIGNED
 * says whether they are two's complement and NORMALIZE whether the
 * result is a fraction or a count, which is the table the register
 * reference prints:
 *
 *   SIGNED NORMALIZE  range
 *     0        0      0.0 .. 2^n - 1        (8-bit: 0 .. 255)
 *     0        1      0.0 .. 1.0            (8-bit: value / 255)
 *     1        0      -2^(n-1) .. 2^(n-1)-1
 *     1        1      -1.0 .. 1.0, by one of three methods
 *
 * The three signed-normalised methods come from VAP_PSC_SGN_NORM_CNTL.
 * SGN_NORM_NO_ZERO is written "(2 * value + 1)/2^n" in the reference
 * but the two results the same sentence quotes -- -128 -> -255/255 and
 * 127 -> 255/255 -- are the odd denominator, so 2^n - 1 is what is
 * implemented here.
 */
static float r300_psc_fixed(uint32_t raw, unsigned bits, unsigned snf,
                            unsigned meth)
{
    uint32_t full = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
    bool normalize = !!(snf & R300_PSC_NORMALIZE);
    int32_t sv;

    if (!(snf & R300_PSC_SIGNED)) {
        return normalize ? (float)raw / (float)full : (float)raw;
    }
    sv = (int32_t)(raw << (32 - bits)) >> (32 - bits);
    if (!normalize) {
        return (float)sv;
    }
    switch (meth) {
    case R300_PSC_SGN_NORM_NO_ZERO:
        return (float)(2 * sv + 1) / (float)full;
    case R300_PSC_SGN_NORM_ZERO_CLAMP:
        return MAX((float)sv / (float)(full >> 1), -1.0f);
    default:
        return (float)sv / (float)(full >> 1);
    }
}

/* SE5M10 with an exponent bias of 15, denormals included */
static float r300_psc_f16(uint32_t h)
{
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t man = h & 0x3ff;

    if (!exp) {
        return sign ? -ldexpf((float)man, -24) : ldexpf((float)man, -24);
    }
    if (exp == 0x1f) {
        return r300_f32(sign | 0x7f800000u | (man << 13));
    }
    return r300_f32(sign | ((exp + 127 - 15) << 23) | (man << 13));
}

/*
 * ONE STREAM ELEMENT, UNPACKED.
 *
 * `dw` points at the element's first dword and `navail` is how many of
 * them the vertex actually carries, so a short element cannot read past
 * its own array. Components the element does not supply keep the
 * (0,0,0,1) the input registers reset to, which is what lets a
 * three-dword model-space position meet a 4x4 matrix and still pick up
 * its translation column.
 *
 * The lane assignments are the register reference's, verbatim; the two
 * that matter are BYTE (X = bits 7:0, W = bits 31:24) and D3DCOLOR,
 * which is the same thing with X and Z exchanged.
 */
static void r300_psc_unpack(const R300VtxFmt *f, unsigned idx,
                            const uint32_t *dw, unsigned navail,
                            float out[4])
{
    unsigned type = f->type[idx], snf = f->snf[idx], meth = f->meth[idx];
    uint32_t v = navail ? dw[0] : 0;
    uint32_t v1 = navail > 1 ? dw[1] : 0;
    unsigned c;

    switch (type) {
    case R300_PSC_TYPE_BYTE:
        for (c = 0; c < 4; c++) {
            out[c] = r300_psc_fixed((v >> (c * 8)) & 0xff, 8, snf, meth);
        }
        break;
    case R300_PSC_TYPE_D3DCOLOR:
        out[0] = r300_psc_fixed((v >> 16) & 0xff, 8, snf, meth);
        out[1] = r300_psc_fixed((v >> 8) & 0xff, 8, snf, meth);
        out[2] = r300_psc_fixed(v & 0xff, 8, snf, meth);
        out[3] = r300_psc_fixed((v >> 24) & 0xff, 8, snf, meth);
        break;
    case R300_PSC_TYPE_SHORT_2:
        out[0] = r300_psc_fixed(v & 0xffff, 16, snf, meth);
        out[1] = r300_psc_fixed((v >> 16) & 0xffff, 16, snf, meth);
        break;
    case R300_PSC_TYPE_SHORT_4:
        out[0] = r300_psc_fixed(v & 0xffff, 16, snf, meth);
        out[1] = r300_psc_fixed((v >> 16) & 0xffff, 16, snf, meth);
        out[2] = r300_psc_fixed(v1 & 0xffff, 16, snf, meth);
        out[3] = r300_psc_fixed((v1 >> 16) & 0xffff, 16, snf, meth);
        break;
    case R300_PSC_TYPE_VECTOR_3_TTT:
        out[0] = r300_psc_fixed(v & 0x3ff, 10, snf, meth);
        out[1] = r300_psc_fixed((v >> 10) & 0x3ff, 10, snf, meth);
        out[2] = r300_psc_fixed((v >> 20) & 0x3ff, 10, snf, meth);
        break;
    case R300_PSC_TYPE_VECTOR_3_EET:
        out[0] = r300_psc_fixed(v & 0x7ff, 11, snf, meth);
        out[1] = r300_psc_fixed((v >> 11) & 0x7ff, 11, snf, meth);
        out[2] = r300_psc_fixed((v >> 22) & 0x3ff, 10, snf, meth);
        break;
    case R300_PSC_TYPE_FLT16_2:
        out[0] = r300_psc_f16(v & 0xffff);
        out[1] = r300_psc_f16(v >> 16);
        break;
    case R300_PSC_TYPE_FLT16_4:
        out[0] = r300_psc_f16(v & 0xffff);
        out[1] = r300_psc_f16(v >> 16);
        out[2] = r300_psc_f16(v1 & 0xffff);
        out[3] = r300_psc_f16(v1 >> 16);
        break;
    default:
        /*
         * A float type, or one r300_stream_route() already reported as
         * a gap and left the format at FLOAT_1 for: read what is there.
         */
        for (c = 0; c < navail && c < 4; c++) {
            out[c] = r300_f32(dw[c]);
        }
        break;
    }
}

/*
 * VAP_PROG_STREAM_CNTL_EXT's per-component select and write enable,
 * applied to the four components the element produced.
 *
 * A write-enable of zero is the register's reset value and would
 * discard the element entirely, which no driver programs deliberately;
 * treated as "this word does not describe the element" and skipped, the
 * same discipline the routing itself is held to. Everything else is
 * taken literally -- and taken literally it is the whole of the
 * BYTE-vs-D3DCOLOR question for a big-endian guest, because Mac OS X
 * 10.5 pairs its one BYTE element with a W,Z,Y,X select.
 */
static void r300_psc_swizzle(const R300VtxFmt *f, unsigned idx,
                             const float in[4], float out[4])
{
    unsigned w = f->swz[idx], ena, c;

    ena = (w >> R300_PSC_WRITE_ENA_SHIFT) & R300_PSC_WRITE_ENA_MASK;
    if (!ena) {
        memcpy(out, in, sizeof(float) * 4);
        return;
    }
    for (c = 0; c < 4; c++) {
        unsigned sel = (w >> (c * R300_PSC_SWIZZLE_SHIFT)) &
                       R300_PSC_SWIZZLE_MASK;

        if (!(ena & (1u << c))) {
            continue;           /* keeps the input register's default */
        }
        if (sel < 4) {
            out[c] = in[sel];
        } else if (sel == R300_PSC_SWIZZLE_FP_ZERO) {
            out[c] = 0.0f;
        } else if (sel == R300_PSC_SWIZZLE_FP_ONE) {
            out[c] = 1.0f;
        } else {
            out[c] = in[c];     /* reserved; reported as a gap at setup */
        }
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
static void r300_load_vtx(const R300DrawState *d, const R300VtxFmt *f,
                          const uint32_t *dw,
                          unsigned vsize, unsigned pos, R300Vtx *v)
{
    /* set by the caller for vertices that carry no colour of their own */
    v->x = r300_f32(dw[0]);
    v->y = pos >= 2 ? r300_f32(dw[1]) : 0.0f;
    v->z = pos >= 3 ? r300_f32(dw[2]) : 0.0f;
    v->w = pos >= 4 ? r300_f32(dw[3]) : 1.0f;
    /*
     * A position is four IEEE floats in every capture this project
     * holds, but nothing says it has to be: the stream control word
     * describes element 0 exactly as it describes the others. When it
     * says the position is packed, unpack it -- and when it says
     * anything else, or was not believed, the four reads above stand
     * untouched.
     */
    if (f->valid && d->attr_count && !r300_psc_is_float(f->type[0])) {
        float raw[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        r300_psc_unpack(f, 0, dw, d->attr_size[0], raw);
        r300_psc_swizzle(f, 0, raw, p);
        v->x = p[0];
        v->y = p[1];
        v->z = p[2];
        v->w = p[3];
    }
    v->r = d->flat_r;
    v->g = d->flat_g;
    v->b = d->flat_b;
    v->a = d->flat_a;
    v->r1 = v->g1 = v->b1 = v->a1 = 0.0f;
    memset(v->tc, 0, sizeof(v->tc));
    {
        unsigned k;

        for (k = 0; k < R300_TEXCOORDS; k++) {
            v->tcr[k][0] = v->tcr[k][1] = v->tcr[k][2] = 0.0f;
            v->tcr[k][3] = 1.0f;
        }
    }
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
        v->tc[0][0] = r300_f32(dw[8]);
        v->tc[0][1] = r300_f32(dw[9]);
    } else if (vsize >= 8) {
        /*
         * pos.xyzw + texcoords. Live captures show the 8-dword layout
         * is always position + texture coordinates -- the untextured
         * users (window shadows via DRAW_VBUF_2) just ignore them and
         * take the fragment constant colour like every colourless
         * vertex. Reading the second attribute as a colour fed
         * texcoords into the blender as RGBA.
         */
        v->tc[0][0] = r300_f32(dw[4]);
        v->tc[0][1] = r300_f32(dw[5]);
    }
}

/* dwords one stream element of each VAP_PROG_STREAM_CNTL DATA_TYPE eats */
static unsigned r300_psc_dwords(unsigned type)
{
    static const uint8_t n[16] = {
        [0] = 1, [1] = 2, [2] = 3, [3] = 4,     /* FLOAT_1 .. FLOAT_4 */
        [4] = 1, [5] = 1, [6] = 1, [7] = 2,     /* BYTE, D3DCOLOR, SHORT_* */
        [8] = 1, [9] = 1, [10] = 8, [11] = 1, [12] = 2,
    };

    return n[type & 0xf];
}

/*
 * WHICH INPUT REGISTER EACH VERTEX ELEMENT FEEDS.
 *
 * The caller has already worked out how many dwords each element of this
 * vertex carries -- from the bound arrays for a VBUF draw, four at a time
 * for an inline IMMD one -- and put them in `size[]` in submission order.
 * What it cannot know from the vertex alone is which of the vertex
 * program's SIXTEEN input registers each element is written to, and that
 * is not always the element's own index: VAP_PROG_STREAM_CNTL's
 * DST_VEC_LOC says, per element, and Mac OS X 10.5's compositor sends a
 * two-element vertex whose second element lands in **in[2]**. Its menu
 * and Dock programs read the texture coordinate from in[2], so with the
 * identity assumption every coordinate they computed came out of the
 * (0,0,0,1) default -- a constant, the same texel for every pixel, and a
 * blank white panel where the menu items should be.
 *
 * `size[]` is rebuilt INDEXED BY INPUT REGISTER, with a register no
 * element feeds left at zero. r300_vs_input() finds an element's dwords
 * by summing the sizes before it, and that stays exact because the
 * skipped registers contribute nothing and DST_VEC_LOC ascends.
 *
 * THE ROUTING IS ONLY BELIEVED WHEN IT DESCRIBES THE VERTEX WE ACTUALLY
 * HAVE, and that guard is the whole reason this is safe to add to the
 * shared vertex path. A previous attempt at reading these registers
 * regressed the 10.4 desktop to flat blocks, and the census says why:
 * across five 10.4 captures (20867 draws) the register is a MISFIT for
 * 1633 of them -- never written, still reading its all-zero reset value,
 * which decodes as sixteen FLOAT_1 elements all pointing at in[0]. Take
 * the routing only when the element count and every element's dword
 * count agree with the vertex, no element skips dwords, and no element
 * names a register this model does not keep. Under that test the census
 * changes exactly TWO draws in everything this project has ever
 * captured, and both of them are Leopard's multi-tap compositor draws.
 *
 * `guess` IS WHAT AN INLINE VERTEX HAS INSTEAD OF ARRAY BOUNDARIES: the
 * whole vertex's dword count from a caller that could only split it four
 * at a time, and zero from a caller whose sizes are real. A VBUF draw
 * fetches element i from bound array i, so array i's dword count IS
 * element i's and a disagreement means the registers are stale -- refuse,
 * as above. An IMMD draw has no such thing, so the guard above refuses
 * every inline vertex whose elements are not all FLOAT_4. Mac OS X 10.5's
 * Aqua chrome strip is one: 0x2150/0x2154 = 0x01030001/0x23030201 names
 * FOUR elements of 2, 4, 2 and 4 dwords into in[0..3], the guess splits
 * the same twelve dwords 4, 4, 4 into in[0..2], the counts disagree, and
 * in[3] -- which that draw's vertex program forwards to a texture
 * coordinate -- reads the (0,0,0,1) default. One texel for every pixel,
 * again. So when the caller has only a guess, and the elements the
 * registers describe add up to EXACTLY the vertex in hand, the registers
 * are the better answer and replace the guess wholesale.
 *
 * That the sum has to match is not a formality; it is the same test as
 * before by another route. The all-zero reset value is refused twice over
 * (no LAST_VEC, and its second element does not ascend past its first),
 * and a stale layout left by another draw only survives if it describes a
 * vertex of exactly this size out of elements this model keeps -- which
 * is the most any register can claim.
 *
 * The added branch has its own census, over every register log this
 * project holds -- 40 logs, 276835 draws, each decided by BOTH the
 * shipped function and this one. It moves 209 draws, every one of them
 * an inline draw in a Mac OS X 10.5 capture, and not one draw in any
 * 10.4, OS 9 or saver capture: those hold 20599 inline draws for it to
 * have moved, so the zero is a result and not an absence. Seeded the
 * VBUF way -- the bound arrays, `guess` zero -- the two functions
 * disagree on 0 of the 276835.
 */
static void r300_stream_route(ATIR350State *s, unsigned *size,
                              unsigned *count, unsigned guess,
                              R300VtxFmt *fmt)
{
    unsigned loc[R300_AOS_MAX], dw[R300_AOS_MAX], el[R300_AOS_MAX];
    unsigned ext[R300_AOS_MAX];
    uint32_t sgn_norm = s->regs[R300_VAP_PSC_SGN_NORM_CNTL >> 2];
    unsigned n = 0, i, c, last = 0, tot = 0;
    bool done = false, ident = true, fits, derive = false;

    for (i = 0; i < 8 && !done; i++) {
        uint32_t v = s->regs[(R300_VAP_PROG_STREAM_CNTL_0 >> 2) + i];
        uint32_t x = s->regs[(R300_VAP_PROG_STREAM_CNTL_EXT_0 >> 2) + i];
        unsigned half;

        for (half = 0; half < 2; half++) {
            uint32_t w = (v >> (half * 16)) & 0xffff;

            if (n == ARRAY_SIZE(loc)) {
                return;         /* more elements than this model keeps */
            }
            if ((w >> R300_PSC_SKIP_DWORDS_SHIFT) &
                R300_PSC_SKIP_DWORDS_MASK) {
                return;         /* a gap inside the vertex, not modelled */
            }
            el[n] = w;
            ext[n] = (x >> (half * 16)) & 0xffff;
            dw[n] = r300_psc_dwords(w & R300_PSC_DATA_TYPE_MASK);
            loc[n] = (w >> R300_PSC_DST_VEC_LOC_SHIFT) &
                     R300_PSC_DST_VEC_LOC_MASK;
            if (loc[n] >= ARRAY_SIZE(loc) || (n && loc[n] <= last)) {
                return;         /* out of range, or not ascending */
            }
            ident = ident && loc[n] == n;
            last = loc[n];
            n++;
            if (w & R300_PSC_LAST_VEC) {
                done = true;
                break;
            }
        }
    }
    if (!done) {
        return;
    }
    /*
     * `fits` is the shipped test, kept exactly: the element count and
     * every element's dword count so far agree with the caller's sizes.
     * It stops being true at the first disagreement and is never revived,
     * so at any point in the loop it means what it meant before.
     */
    fits = (n == *count);
    for (i = 0; i < n; i++) {
        if ((el[i] & R300_PSC_DATA_TYPE_MASK) > R300_PSC_TYPE_FLT16_4) {
            /*
             * A reserved code: not even the number of dwords it eats is
             * known, so nothing about this vertex can be believed -- and
             * `tot` cannot be completed either, so the derived route is
             * out too. Say so and leave the caller's own sizes and the
             * float read. Reported under the shipped condition, so a
             * vertex these registers never described reports no more
             * than it did before.
             */
            if (fits) {
                ati_r350_note_gap(s, R350_GAP_VTX_DATA_TYPE,
                                  el[i] & R300_PSC_DATA_TYPE_MASK);
            }
            return;
        }
        tot += dw[i];
        if (fits && dw[i] != size[i]) {
            fits = false;
        }
    }
    if (!fits) {
        /*
         * The caller's sizes are not what these registers describe. With
         * array boundaries behind them that settles it -- keep them. With
         * only a positional guess behind them, and a register layout that
         * accounts for every dword of the vertex, the registers win and
         * the guess is discarded.
         */
        if (!guess || tot != guess) {
            return;
        }
        derive = true;
    }
    /*
     * PAST HERE THE REGISTERS DESCRIBE THIS VERTEX, so their element
     * FORMATS can be believed even when the destinations are the
     * identity and there is no routing left to do. This is the only
     * reason a draw whose stream control is the identity now reads
     * these registers at all: without it a packed one-dword colour
     * (Mac OS X 10.5's compositor sends exactly one, and sends it to
     * in[1], its own index) would still be read as an IEEE float.
     */
    for (i = 0; i < n; i++) {
        unsigned type = el[i] & R300_PSC_DATA_TYPE_MASK;

        if (type == R300_PSC_TYPE_FLOAT_8) {
            /*
             * FLOAT_8 feeds TWO consecutive input registers from one
             * element and this model routes one. Fall back to the float
             * read for the register it does route, and say so.
             */
            ati_r350_note_gap(s, R350_GAP_VTX_DATA_TYPE, type);
            type = R300_PSC_TYPE_FLOAT_1;
        }
        for (c = 0; c < 4; c++) {
            unsigned sel = (ext[i] >> (c * R300_PSC_SWIZZLE_SHIFT)) &
                           R300_PSC_SWIZZLE_MASK;

            if (sel > R300_PSC_SWIZZLE_FP_ONE &&
                ((ext[i] >> R300_PSC_WRITE_ENA_SHIFT) & (1u << c))) {
                ati_r350_note_gap(s, R350_GAP_VTX_DATA_TYPE, 0x10 | sel);
            }
        }
        fmt->type[loc[i]] = type;
        fmt->snf[loc[i]] = el[i] & (R300_PSC_SIGNED | R300_PSC_NORMALIZE);
        fmt->meth[loc[i]] = (sgn_norm >> (2 * i)) & 3;
        fmt->swz[loc[i]] = ext[i];
        fmt->valid = true;
    }
    if (derive) {
        /*
         * The caller had a guess and these registers have a layout, so
         * there is nothing of the guess to keep: every input register
         * takes the dwords its own element carries, and one no element
         * names takes none and reads the (0,0,0,1) default.
         */
        for (i = 0; i < R300_AOS_MAX; i++) {
            size[i] = 0;
        }
        for (i = 0; i < n; i++) {
            size[loc[i]] = dw[i];
        }
        *count = last + 1;
        return;
    }
    if (ident) {
        return;                 /* nothing to move */
    }
    for (i = n; i-- > 0; ) {
        size[loc[i]] = size[i];
        if (loc[i] != i) {
            size[i] = 0;
        }
    }
    *count = last + 1;
}

/*
 * One of the vertex program's input registers, read out of this vertex.
 *
 * `attr_size` is indexed by INPUT REGISTER: r300_stream_route() has
 * already applied VAP_PROG_STREAM_CNTL's DST_VEC_LOC, so a register no
 * element feeds has size zero and reads its default here. Components a
 * vertex does not supply keep the (0,0,0,1) default, which is what lets
 * a three-dword model-space position meet a 4x4 matrix and still pick up
 * its translation column.
 *
 * THE ELEMENT IS NOT ALWAYS FOUR IEEE FLOATS, and reading it as though
 * it were is what row 0-RED-STATE was. VAP_PROG_STREAM_CNTL's DATA_TYPE
 * says how each element is packed, and r300_psc_dwords() has always
 * known that five of the codes fit a whole vector in ONE dword -- it
 * just used that to SIZE the element and then read the dword as a
 * float anyway. Mac OS X 10.5's compositor sends its per-vertex colour
 * as a normalised BYTE quad, so a perfectly ordinary grey 0x666666ff
 * arrived as (2.7e23, 0, 0, 1): the huge positive component saturates
 * the fragment stage's multiply-add to RED (the pressed Dock icon) and
 * a top bit set makes it hugely negative instead, which clamps to
 * BLACK (a tooltip's text). `f` carries the formats and is empty --
 * every element a plain float -- for every draw whose stream control
 * registers did not describe the vertex in hand.
 */
static void r300_vs_input(const R300DrawState *d, const R300VtxFmt *f,
                          const uint32_t *dw, unsigned idx, float out[4])
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
    if (!f->valid) {
        for (c = 0; c < d->attr_size[idx] && c < 4; c++) {
            out[c] = r300_f32(dw[off + c]);
        }
        return;
    }
    {
        float raw[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        r300_psc_unpack(f, idx, dw + off, d->attr_size[idx], raw);
        r300_psc_swizzle(f, idx, raw, out);
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
 * The texture coordinate the program computed, in the TEXELS of the unit
 * that fetches with this set -- which is the unit the interpolant is
 * carried in and NOT the unit the fragment program sees.
 *
 * The hardware samples with normalised coordinates and interpolates the
 * vertex program's output untouched; this model instead multiplies by
 * the bound size here and divides again in r300_raster_tri() on the way
 * into the fragment frame. That looks like work undone one line later,
 * and for a draw running the interpreter it is. It is kept because the
 * interpolant is a FILE FORMAT: R300Vtx goes verbatim into every
 * R350CAP3 record, and because the two paths that read it straight --
 * the specialised executor and the GL backend -- provably cannot see the
 * difference. `gl_simple` requires the program's one fetch to name the
 * routed coordinate register itself, with no ALU in between, so neither
 * path ever exposes the coordinate to guest arithmetic; leaving them in
 * texels is what keeps them not merely equivalent but bit-identical.
 *
 * Taking the raw attribute instead has worked so far because Mac OS X's
 * compositor hands the vertex a coordinate in texels and its program's
 * texture matrix is exactly diag(1/w, 1/h, 1, 1), so the two paths agree
 * by construction (1887 of 1887 draws across the captures). Chess.app's
 * board has no coordinate attribute at all -- its program generates one
 * -- so there the attribute path samples texel (0,0) for every pixel and
 * the board loses its texture entirely.
 */
static void r300_vs_texcoord(const R300DrawState *d, R300Vtx *v,
                             unsigned set, const float c[4])
{
    const R300TexUnit *u = &d->tex[d->tc_unit[set]];
    float s = c[0], t = c[1], q = c[3];

    v->tcr[set][0] = c[0];
    v->tcr[set][1] = c[1];
    v->tcr[set][2] = c[2];
    v->tcr[set][3] = c[3];
    if (!isfinite(s) || !isfinite(t)) {
        return;
    }
    if (isfinite(q) && q != 0.0f && q != 1.0f) {
        s /= q;
        t /= q;
    }
    v->tc[set][0] = s * u->w;
    v->tc[set][1] = t * u->h;
}

/*
 * WHERE EACH COORDINATE SET COMES FROM, decided once per draw.
 *
 * `use` marks a set whose vertex program COMPUTES the coordinate with a
 * matrix this model can apply without running the program -- see
 * r300_texcoord_src() for the guard that decides it, which is the whole
 * of the change's blast radius.
 */
typedef struct R300TexSrc {
    R300PvsTexMat tm;
    bool use;
} R300TexSrc;

/* the dword r300_load_vtx() takes tc[0] from, or -1 when it takes none */
static int r300_positional_tc(unsigned vsize, unsigned pos)
{
    if (pos < 4) {
        return -1;
    }
    if (vsize >= 12) {
        return 8;
    }
    if (vsize >= 8) {
        return 4;
    }
    return -1;
}

/*
 * IS THE COORDINATE THIS MATRIX COMPUTES THE ONE THE VERTEX ALREADY
 * HANDED OVER? This is the guard, and it exists so that the guests that
 * render correctly today take not one new instruction.
 *
 * r300_load_vtx() reads tc[0] out of a fixed dword and leaves it in
 * TEXELS. A program whose texture matrix is exactly diag(1/w, 1/h, *, 1)
 * for the bound texture undoes precisely the scaling r300_vs_texcoord()
 * would then apply, so if it also reads the input register those very
 * dwords feed, the two routes are the same number arrived at two ways --
 * except that one of them is two float operations longer, and would move
 * pixels by a unit in the last place for no gain.
 *
 * Mac OS X 10.4's compositor is that program in 4480 of 4480 computed
 * coordinates across every capture this project holds (Chess 4256,
 * Flurry 150, iTunes 74) and so is Mac OS X 10.5's menu in all 20 of
 * its own; not one of them is touched. 10.5's System Preferences is
 * where it stops holding: 158 draws put the coordinate somewhere the
 * fixed read does not look and 650 more are too narrow for it to happen
 * at all. Census: scratchpad texguard.py over the six captures carrying
 * `ati_r350_pm4_reg`, which agrees under both readings of an ambiguous
 * vertex layout.
 */
static bool r300_texmat_is_positional(const R300DrawState *d,
                                      const R300PvsTexMat *tm, unsigned set,
                                      unsigned vsize, unsigned pos)
{
    const R300TexUnit *u = &d->tex[d->tc_unit[set]];
    int ptc = r300_positional_tc(vsize, pos);
    unsigned off = 0, r, c;

    if (set || ptc < 0 || tm->in >= d->attr_count ||
        d->attr_size[tm->in] < 2 || !u->w || !u->h) {
        return false;
    }
    for (c = 0; c < tm->in; c++) {
        off += d->attr_size[c];
    }
    if (off != (unsigned)ptc) {
        return false;
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (r != c && tm->m[r][c] != 0.0f) {
                return false;
            }
        }
    }
    return tm->m[0][0] == 1.0f / (float)u->w &&
           tm->m[1][1] == 1.0f / (float)u->h &&
           tm->m[3][3] == 1.0f;
}

static void r300_texcoord_src(const R300DrawState *d, unsigned vsize,
                              unsigned pos, R300TexSrc *ts)
{
    unsigned k;

    for (k = 0; k < R300_TEXCOORDS; k++) {
        ts[k].use = false;
        if (k >= d->ntc || !d->textured || d->tex_attr[k] >= 0 ||
            d->vs_texcoord[k]) {
            continue;           /* absent, forwarded, or the interpreter's */
        }
        if (!r300_pvs_texmat(&d->vs, d->vs_tex_out[k], &ts[k].tm)) {
            continue;
        }
        ts[k].use = !r300_texmat_is_positional(d, &ts[k].tm, k, vsize, pos);
    }
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
static void r300_attr_texcoord(const R300DrawState *d, const R300VtxFmt *f,
                               const uint32_t *dw,
                               const R300TexSrc *ts, R300Vtx *v)
{
    float c[4];
    unsigned k;

    for (k = 0; k < d->ntc; k++) {
        int a = d->tex_attr[k];

        if (ts[k].use) {
            /*
             * The program COMPUTES this set and the fast path is still
             * worth keeping for the position, so apply just the four
             * texture-coordinate rows here. Written with the constant
             * first and summed left to right, exactly as the
             * interpreter's DOT_PRODUCT is, so the two agree to the bit
             * for any draw that could take either route.
             */
            const float (*m)[4] = ts[k].tm.m;
            float in[4];
            unsigned r;

            r300_vs_input(d, f, dw, ts[k].tm.in, in);
            for (r = 0; r < 4; r++) {
                c[r] = m[r][0] * in[0] + m[r][1] * in[1] +
                       m[r][2] * in[2] + m[r][3] * in[3];
            }
            r300_vs_texcoord(d, v, k, c);
            continue;
        }
        if (a < 0 || (unsigned)a >= d->attr_count ||
            d->attr_size[a] < 2) {
            continue;
        }
        r300_vs_input(d, f, dw, (unsigned)a, c);
        r300_vs_texcoord(d, v, k, c);
    }
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
static void r300_trace_texcoord(const R300DrawState *d, const R300VtxFmt *f,
                                const uint32_t *dw, const R300Vtx *v)
{
    float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!trace_event_get_state_backends(TRACE_ATI_R350_3D_TEXCOORD)) {
        return;
    }
    if (d->tex_attr[0] >= 0 && (unsigned)d->tex_attr[0] < d->attr_count) {
        r300_vs_input(d, f, dw, (unsigned)d->tex_attr[0], c);
    }
    trace_ati_r350_3d_texcoord(d->tex_attr[0], d->tex[0].w, d->tex[0].h,
                               (int32_t)(c[0] * 1000), (int32_t)(c[1] * 1000),
                               (int32_t)(c[3] * 1000),
                               (int32_t)(v->tc[0][0] * 1000),
                               (int32_t)(v->tc[0][1] * 1000));
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
 * The second colour the vertex stage emits. Chess.app's lighting program
 * writes its specular term there and its fragment program adds it, which
 * is the one arithmetic gap milestone M5 set out to close; nothing else
 * in the corpus emits two.
 */
static void r300_vs_color1(R300Vtx *v, const float c[4])
{
    if (!isfinite(c[0]) || !isfinite(c[1]) ||
        !isfinite(c[2]) || !isfinite(c[3])) {
        return;
    }
    v->r1 = c[0];
    v->g1 = c[1];
    v->b1 = c[2];
    v->a1 = c[3];
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
                        const R300VtxFmt *f, const uint32_t *dw,
                        R300Vtx *v, float clip[4])
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
            r300_vs_input(d, f, dw, src, col);
            r300_vs_color(v, col);
        }
        return false;
    }

    memset(&r, 0, sizeof(r));
    for (a = 0; a < R300_PVS_IN_REGS; a++) {
        r.in[a][3] = 1.0f;
    }
    for (a = 0; a < d->attr_count; a++) {
        r300_vs_input(d, f, dw, a, r.in[a]);
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
    if (d->vs_color2 && (r.out_written & (1u << d->vs_color2_out))) {
        r300_vs_color1(v, r.out[d->vs_color2_out]);
    }
    for (a = 0; a < d->ntc; a++) {
        if (d->vs_texcoord[a] &&
            (r.out_written & (1u << d->vs_tex_out[a]))) {
            r300_vs_texcoord(d, v, a, r.out[d->vs_tex_out[a]]);
        }
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
 * Milestone M4's live coverage measurement, and nothing else: hand this
 * program to the GLSL translator and record whether it could express it.
 *
 * It is NOT on any pixel's path. The offline three-way harness
 * (doc/radeon9800/pvs-offline-test) proves the translation COMPUTES what
 * the interpreter computes, over the seven programs one application
 * uploads; what no corpus can answer is how much of what real guests
 * upload it COVERS, and that is a question only a running desktop asks.
 * So the answer is counted here, behind a property that is off by
 * default -- with it off this function returns before touching anything,
 * which is the whole of its no-op proof.
 *
 * A program is translated once per program, not once per draw: the
 * signature is the instruction range and constant base the control
 * registers name, and Chess's thousand board draws all carry one.
 */
static void r300_pvs_translate(ATIR350State *s, const R300PvsProgram *p)
{
    static char body[192 * 1024];
    R300PvsGlsl info;
    uint64_t sig;
    bool ok;

    if (!s->pvs_glsl || !p->valid) {
        return;
    }
    sig = 1 | ((uint64_t)p->first << 1) | ((uint64_t)p->last << 12) |
          ((uint64_t)p->cbase << 24) | ((uint64_t)p->cmax << 32);
    if (sig == s->pvs_tr_sig) {
        return;
    }
    s->pvs_tr_sig = sig;

    ok = r300_pvs_glsl(p, body, sizeof(body), &info);
    if (ok) {
        s->pvs_tr_ok++;
        s->pvs_tr_last_bytes = strlen(body);
        s->pvs_tr_last_nconst = info.nconst;
        s->pvs_tr_last_in = info.in_mask;
        s->pvs_tr_last_out = info.out_mask;
    } else {
        s->pvs_tr_refused++;
        if (info.gaps.has_vec_op) {
            s->pvs_tr_by_reason[0]++;
            ati_r350_note_gap(s, R350_GAP_VS_VECTOR_OP, info.gaps.vec_op);
        }
        if (info.gaps.has_math_op) {
            s->pvs_tr_by_reason[1]++;
            ati_r350_note_gap(s, R350_GAP_VS_MATH_OP, info.gaps.math_op);
        }
        if (info.gaps.has_dst_file) {
            s->pvs_tr_by_reason[2]++;
            ati_r350_note_gap(s, R350_GAP_VS_DST_FILE, info.gaps.dst_file);
        }
    }
    trace_ati_r350_pvs_glsl(p->first, p->last, p->cbase, p->cmax, ok,
                            (uint32_t)strlen(body), info.nconst,
                            info.in_mask, info.out_mask);
}

/*
 * Milestone M5: resolve the fragment program this draw runs.
 *
 * The six US banks are RAM holding every program the guest has ever
 * uploaded; which slots of them a draw executes is US_CONFIG,
 * US_CODE_ADDR_3 and the US_CODE_OFFSET relocation, and what those slots
 * mean for a pixel also needs the rasterizer routing that says which
 * frame register each interpolated quantity lands in. All of it is
 * decoded here, once per draw, and the result is what the rasterizer and
 * the GL backend both shade with.
 *
 * A program the interpreter cannot express is COUNTED and the draw
 * renders its interpolated colour unmodulated -- deliberately not the
 * old `texel * colour`, which was never anything but a guess at what a
 * program computes and is what this milestone removes.
 */
static bool r300_fs_setup(ATIR350State *s, R300DrawState *d)
{
    R300UsProgram *p = &s->us_prog;
    const uint32_t *regs = s->regs;
    float konst[R300_US_CONSTS][4];
    uint64_t sig;
    unsigned i, tc_named = 0;

    /*
     * One decode per program, not per draw: the signature is US_CONFIG
     * and US_CODE_OFFSET -- the register whose whole purpose is to move
     * a program rather than rewrite it in place. A changed write to
     * US_CODE_ADDR_*, an ALU or texture bank, or the RS_INST/RS_IP
     * routing clears it (ati_r350_reg_write32()).
     * US_OUT_FMT_0 is compared as well: its component select is decoded
     * into the program's output permutation, and the same program is
     * run with different selects (a glyph cache copied out to an I8
     * buffer selects alpha for every component).
     */
    sig = 1 | ((uint64_t)regs[R300_US_CONFIG >> 2] << 1) |
          ((uint64_t)(regs[R300_US_CODE_OFFSET >> 2] & 0xffffff) << 25);
    if (sig != s->us_sig ||
        regs[R300_US_OUT_FMT_0 >> 2] != s->us_out_fmt ||
        regs[R300_VAP_OUTPUT_VTX_FMT_1 >> 2] != s->us_vtx_fmt1 ||
        p->nregs != (regs[R300_US_PIXSIZE >> 2] & 0x1f) + 1) {
        s->us_sig = sig;
        s->us_out_fmt = regs[R300_US_OUT_FMT_0 >> 2];
        s->us_vtx_fmt1 = regs[R300_VAP_OUTPUT_VTX_FMT_1 >> 2];
        for (i = 0; i < R300_US_CONSTS; i++) {
            unsigned k = (R300_PFS_PARAM_0_X >> 2) + i * 4;

            konst[i][0] = r300_us_f24(regs[k]);
            konst[i][1] = r300_us_f24(regs[k + 1]);
            konst[i][2] = r300_us_f24(regs[k + 2]);
            konst[i][3] = r300_us_f24(regs[k + 3]);
        }
        r300_us_analyse(p, regs[R300_US_CONFIG >> 2],
                        regs[R300_US_CODE_OFFSET >> 2],
                        &regs[R300_US_CODE_ADDR_0 >> 2],
                        regs[R300_US_PIXSIZE >> 2],
                        regs[R300_US_OUT_FMT_0 >> 2],
                        &regs[R300_US_TEX_INST_0 >> 2],
                        &regs[R300_US_ALU_RGB_ADDR_0 >> 2],
                        &regs[R300_US_ALU_RGB_INST_0 >> 2],
                        &regs[R300_US_ALU_ALPHA_ADDR_0 >> 2],
                        &regs[R300_US_ALU_ALPHA_INST_0 >> 2],
                        konst,
                        regs[R300_RS_INST_COUNT >> 2],
                        &regs[R300_RS_INST_0 >> 2],
                        &regs[R300_RS_IP_0 >> 2],
                        regs[R300_VAP_OUTPUT_VTX_FMT_1 >> 2]);
        if (!p->expressible) {
            const R300UsGaps *g = &p->gaps;

            if (g->has_rgb_op) {
                ati_r350_note_gap(s, R350_GAP_FS_RGB_OP, g->rgb_op);
            }
            if (g->has_a_op) {
                ati_r350_note_gap(s, R350_GAP_FS_ALPHA_OP, g->a_op);
            }
            if (g->has_tex_op) {
                ati_r350_note_gap(s, R350_GAP_FS_TEX_OP, g->tex_op);
            }
            if (g->has_indirect) {
                ati_r350_note_gap(s, R350_GAP_FS_INDIRECT, g->indirect);
            }
            if (g->has_rs_route) {
                ati_r350_note_gap(s, R350_GAP_FS_RS_ROUTE, g->rs_route);
            }
            if (g->has_out_fmt) {
                ati_r350_note_gap(s, R350_GAP_FS_OUT_FMT, g->out_fmt);
            }
            /*
             * The words that describe the refused program, so a gap
             * found in a live guest can be specified offline instead of
             * guessed at. A gap name says WHICH construct; only the
             * encoding says what it asks for.
             *
             * The texture instructions are read at the RELOCATED slot.
             * US_CODE_OFFSET exists so a driver can move a program
             * rather than rewrite it, and slot 0 is very often not the
             * one in force -- printing it would describe some other
             * program entirely.
             */
            unsigned t0 = (R300_US_TEX_INST_0 >> 2) + p->tex_first;

            trace_ati_r350_us_refused(regs[R300_US_CONFIG >> 2],
                                      regs[(R300_US_CODE_ADDR_0 >> 2) + 3],
                                      regs[R300_US_OUT_FMT_0 >> 2],
                                      regs[R300_RS_INST_COUNT >> 2],
                                      regs[R300_RS_INST_0 >> 2],
                                      regs[(R300_RS_INST_0 >> 2) + 1],
                                      regs[R300_RS_IP_0 >> 2],
                                      regs[(R300_RS_IP_0 >> 2) + 1],
                                      p->ntex > 0 ? regs[t0] : 0,
                                      p->ntex > 1 ? regs[t0 + 1] : 0);
            /*
             * The WHOLE routing table, because two entries of it are
             * not enough to say what a refusal asks for: an RS_INST
             * naming IP entry 2 tells you nothing unless you can see
             * IP 2, and the first cut of this trace printed entries 0
             * and 1 only. RS_INST_COUNT is what says how many of the
             * instructions below are live.
             */
            trace_ati_r350_us_refused_ip(
                (regs[R300_RS_INST_COUNT >> 2] & 0xf) + 1,
                regs[(R300_RS_IP_0 >> 2) + 0], regs[(R300_RS_IP_0 >> 2) + 1],
                regs[(R300_RS_IP_0 >> 2) + 2], regs[(R300_RS_IP_0 >> 2) + 3],
                regs[(R300_RS_IP_0 >> 2) + 4], regs[(R300_RS_IP_0 >> 2) + 5],
                regs[(R300_RS_IP_0 >> 2) + 6], regs[(R300_RS_IP_0 >> 2) + 7]);
            trace_ati_r350_us_refused_ri(
                regs[(R300_RS_INST_0 >> 2) + 0],
                regs[(R300_RS_INST_0 >> 2) + 1],
                regs[(R300_RS_INST_0 >> 2) + 2],
                regs[(R300_RS_INST_0 >> 2) + 3],
                regs[(R300_RS_INST_0 >> 2) + 4],
                regs[(R300_RS_INST_0 >> 2) + 5],
                regs[(R300_RS_INST_0 >> 2) + 6],
                regs[(R300_RS_INST_0 >> 2) + 7]);
        }
        trace_ati_r350_fs_program(p->alu_first, p->nalu, p->tex_first,
                                  p->ntex, p->nregs_used, p->tex_dst,
                                  p->rs.col_reg[0], p->rs.col_reg[1],
                                  p->expressible);
        /*
         * The same program for the host GPU. Translated here, with the
         * decode, so a thousand draws of one program cost one
         * translation and one shader link -- and so that a program the
         * translator refuses is refused before any pixel depends on it.
         */
        s->us_glsl_ok = r300_us_glsl(p, s->us_glsl, sizeof(s->us_glsl));
        /*
         * The key the GL backend caches its linked shader under is a
         * hash of the TEXT, not of the control words. Those change far
         * more often than the program does -- US_CODE_OFFSET exists
         * precisely so the driver can move a program rather than
         * rewrite it, and a live Chess session relocates one 4720 times
         * while uploading ten distinct programs. Keyed on the words, a
         * shader cache would relink on nearly every draw.
         */
        if (s->us_glsl_ok) {
            const char *c = s->us_glsl;
            uint64_t h = 1469598103934665603ULL;

            while (*c) {
                h = (h ^ (uint8_t)*c++) * 1099511628211ULL;
            }
            s->us_glsl_key = h | 1;
            s->us_glsl_ok_n++;
        } else {
            s->us_glsl_key = 0;
            s->us_glsl_refused_n++;
        }
        trace_ati_r350_us_glsl(p->alu_first, p->nalu, s->us_glsl_ok,
                               (uint32_t)strlen(s->us_glsl), 0);
    } else {
        /* the constants are per-draw even when the program is not */
        for (i = 0; i < R300_US_CONSTS; i++) {
            unsigned k = (R300_PFS_PARAM_0_X >> 2) + i * 4;

            p->konst[i][0] = r300_us_f24(regs[k]);
            p->konst[i][1] = r300_us_f24(regs[k + 1]);
            p->konst[i][2] = r300_us_f24(regs[k + 2]);
            p->konst[i][3] = r300_us_f24(regs[k + 3]);
        }
    }

    /* the backend takes the constant file as a flat array, per draw */
    for (i = 0; i < R300_US_CONSTS; i++) {
        s->us_konst_flat[i * 4 + 0] = p->konst[i][0];
        s->us_konst_flat[i * 4 + 1] = p->konst[i][1];
        s->us_konst_flat[i * 4 + 2] = p->konst[i][2];
        s->us_konst_flat[i * 4 + 3] = p->konst[i][3];
    }

    d->fs = p;
    d->fs_run = p->valid && p->expressible;
    d->fs_col1 = d->fs_run && p->rs.col_reg[1] >= 0;
    /*
     * How many interpolated coordinate sets this draw carries, and which
     * unit's texels each is CARRIED IN.
     *
     * This decides a representation, not an answer. The vertex stage
     * multiplies a set by this unit's size and r300_raster_tri() divides
     * it out again on the way into the fragment frame, so which unit is
     * named affects only the intermediate -- while the fetch itself
     * scales by the size of the unit BEING FETCHED, per fetch, in
     * r300_us_sample(). That is why two differently sized units sharing
     * one coordinate set is now exact and no longer worth a gap: each of
     * them reads the guest's own coordinate through its own size.
     *
     * A fetch whose source register is not one the rasterizer routed is
     * a dependent read of an ALU result and names no set at all, so it
     * is passed over. Every draw that predates multitexturing lands on
     * ntc 1 and unit 0, which is the arithmetic it always had.
     */
    d->ntc = 1;
    d->tc_raw = 0;
    for (i = 0; i < R300_TEXCOORDS; i++) {
        d->tc_unit[i] = 0;
        if (p->rs.tex_reg[i] >= 0) {
            d->ntc = i + 1;
            d->tc_raw |= 1u << i;
        }
    }
    for (i = 0; d->fs_run && i < p->ntex; i++) {
        const R300UsTex *t = &p->tex[i];
        unsigned k;

        if (t->op == R300_US_TEXOP_NOP || t->op == R300_US_TEXOP_TEXKILL) {
            continue;
        }
        for (k = 0; k < d->ntc; k++) {
            if (p->rs.tex_reg[k] == t->src) {
                d->tc_raw &= ~(1u << k);
            }
        }
    }
    if (!d->fs_run) {
        d->tc_raw = 0;
    }
    for (i = 0; d->fs_run && i < p->ntex; i++) {
        const R300UsTex *t = &p->tex[i];
        unsigned k;

        if ((t->op != R300_US_TEXOP_LD && t->op != R300_US_TEXOP_PROJ) ||
            t->unit >= R300_TEX_UNITS) {
            continue;
        }
        for (k = 0; k < d->ntc; k++) {
            if (p->rs.tex_reg[k] == t->src && !(tc_named & (1u << k))) {
                d->tc_unit[k] = t->unit;
                tc_named |= 1u << k;
            }
        }
    }
    s->us_draws++;
    if (p->valid && !p->expressible) {
        s->us_refused++;
    }
    /*
     * An ALU that writes neither a frame register nor the output fifo
     * cannot produce a colour, so the draw contributes nothing to the
     * colour buffer -- the same situation RB3D_COLOR_CHANNEL_MASK == 0
     * describes above, arrived at from the shader side. Dropping it is
     * not an optimisation: shading it would paint something the hardware
     * never paints.
     */
    return !d->fs_run || p->writes_out;
}

/*
 * One texture unit's state, out of its own word of each TX_* register
 * block. The blocks are sixteen deep with a four-byte stride, so unit u
 * is simply the u'th word of each -- and the same code therefore serves
 * every unit, which is what multitexturing needs and what reading
 * TX_OFFSET_0 by name could never give.
 *
 * `en` carries the DRAW-level gate as well as TX_ENABLE's bit, because
 * a vertex with no room for a texture coordinate cannot sample whatever
 * the register says. Gaps are reported only for a unit that is enabled:
 * the other fifteen blocks hold whatever the last guest to use them
 * left, and reporting on those would be inventing telemetry.
 */
static unsigned r300_ilog2(unsigned v)
{
    unsigned n = 0;

    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

/*
 * Filter state and the mip chain.
 *
 * The sampler finds every level from TX_OFFSET alone: level l is
 * max(w >> l, 1) x max(h >> l, 1), padded to the unit's tile alignment,
 * and follows the previous level with no gap. The alignment, in texels
 * (8/16/32/64 bits per texel):
 *
 *   micro-tiled         8x4   8x2   4x2   2x2   (16bpp square: 4x4)
 *   macro+micro-tiled  64x32 64x16 32x16 16x16
 *   macro-tiled only  256x8 128x8  64x8  32x8
 *   linear             32x1  16x1   8x1   4x1
 *
 * A level keeps macro tiling only while both of its dimensions exceed
 * the macro tile (reach it, under TX_FILTER1 MACRO_SWITCH). Mac OS X 10.5
 * uploads every mipmapped texture macro+micro-tiled, one 2D blit per
 * level, and the blits land exactly there: 256x256 32bpp levels at +0,
 * 0x40000, 0x50000, 0x54000, 0x55000, 0x55400, 0x55500, 0x55540, 0x55560
 * with row pitches 1024 ... 32, 16, 16, 16 (4x4 and smaller rows padded
 * to 4 texels, 2x2 to two rows); 16bpp 256x256 pads its 8x8 and smaller
 * levels to 8 texels; 8bpp 256x256 its 8x8 and smaller to 8 texels.
 * The linear and macro-only rows are from Mesa's r300 driver and
 * unobserved here.
 *
 * Level 0 keeps the pitch r300_sample_tex() has always used, so a
 * texture that is not mipmapped is addressed exactly as before.
 */
static void r300_tex_filter_setup(ATIR350State *s, R300TexUnit *u,
                                  unsigned unit, uint32_t txfmt0,
                                  uint32_t filt0)
{
    static const uint8_t micro_a[4][2] = { {8, 4}, {8, 2}, {4, 2}, {2, 2} };
    static const uint16_t mm_a[4][2] = { {64, 32}, {64, 16}, {32, 16},
                                         {16, 16} };
    static const uint16_t macro_a[4][2] = { {256, 8}, {128, 8}, {64, 8},
                                            {32, 8} };
    uint32_t txo = s->regs[(R300_TX_OFFSET_0 >> 2) + unit];
    uint32_t filt1 = s->regs[(R300_TX_FILTER1_0 >> 2) + unit];
    uint32_t bc = s->regs[(R300_TX_BORDER_COLOR_0 >> 2) + unit];
    unsigned bytes = u->bpp / 8;
    unsigned bi = bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : 3;
    unsigned micro = (txo >> R300_TXO_MICRO_TILE_SHIFT) & 3;
    bool macro = txo & R300_TXO_MACRO_TILE;
    bool rv350 = filt1 & R300_TX_MACRO_SWITCH;
    unsigned l, maxmip;
    uint32_t a = u->off;
    int bias;

    u->mag = (filt0 >> R300_TX_MAG_SHIFT) & 3;
    u->min = (filt0 >> R300_TX_MIN_SHIFT) & 3;
    u->mip = (filt0 >> R300_TX_MIP_SHIFT) & 3;
    /* anisotropic magnification is bilinear; reserved codes point sample */
    if (u->mag == R300_TX_FILTER_ANISO) {
        u->mag = R300_TX_FILTER_LINEAR;
    } else if (u->mag == 0) {
        u->mag = R300_TX_FILTER_POINT;
    }
    if (u->min == 0) {
        u->min = R300_TX_FILTER_POINT;
    }
    if (u->mip == 3) {
        u->mip = R300_TX_FILTER_LINEAR;
    }
    u->aniso_l2 = MIN((filt0 >> R300_TX_ANISO_SHIFT) & 7, 4u);
    if (u->min == R300_TX_FILTER_ANISO && !u->aniso_l2) {
        u->min = R300_TX_FILTER_LINEAR;
    }
    u->last = (txfmt0 >> R300_TX_NUM_LEVELS_SHIFT) & R300_TX_NUM_LEVELS_MASK;
    u->last = MIN(u->last, R300_TEX_LEVELS - 1u);
    maxmip = (filt0 >> R300_TX_MAX_MIP_SHIFT) & 0xf;
    u->first = MIN(maxmip, u->last);
    if (!u->mip) {
        u->last = u->first;
    }
    /* s4.5 */
    bias = (filt1 >> R300_TX_LOD_BIAS_SHIFT) & R300_TX_LOD_BIAS_MASK;
    if (bias & 0x200) {
        bias -= 0x400;
    }
    u->bias = bias * 8;
    u->wl2 = r300_ilog2(u->w);
    u->hl2 = r300_ilog2(u->h);

    u->filt = u->mag != R300_TX_FILTER_POINT ||
              u->min != R300_TX_FILTER_POINT || u->last != 0;
    u->need_lod = u->mag != u->min || u->last > u->first;

    switch (u->bpp) {
    case 8:
        u->border = bc & 0xff;
        break;
    case 16:
        switch (u->code) {
        case R300_TX_FMT_1_5_5_5:
            u->border = r300_texel_1555(bc & 0xffff);
            break;
        case R300_TX_FMT_5_6_5:
            u->border = r300_texel_565(bc & 0xffff);
            break;
        case R300_TX_FMT_4_4_4_4:
            u->border = r300_texel_4444(bc & 0xffff);
            break;
        case R300_TX_FMT_VYUY422:
        case R300_TX_FMT_YVYU422:
            /* AVYU */
            u->border = r300_texel_yuv(u->yuv, (bc >> 8) & 0xff, bc & 0xff,
                                       (bc >> 16) & 0xff, bc >> 24);
            break;
        default:
            u->border = bc & 0xffff;
            break;
        }
        break;
    default:
        u->border = bc;
        break;
    }

    for (l = 0; l <= u->last; l++) {
        int w = MAX(u->w >> l, 1), h = MAX(u->h >> l, 1);
        unsigned aw = 1, ah = 1;

        if (macro) {
            aw = micro ? mm_a[bi][0] : macro_a[bi][0];
            ah = micro ? mm_a[bi][1] : macro_a[bi][1];
        }
        if (macro && (rv350 ? (w >= (int)aw && h >= (int)ah)
                            : (w > (int)aw && h > (int)ah))) {
            /* aligned above */
        } else if (micro == 2 && bytes == 2) {
            aw = 4;
            ah = 4;
        } else if (micro) {
            aw = micro_a[bi][0];
            ah = micro_a[bi][1];
        } else {
            aw = 32 / bytes;
            ah = 1;
        }
        u->lw[l] = w;
        u->lh[l] = h;
        u->loff[l] = a;
        u->lpitch[l] = l ? ROUND_UP(w, aw) * bytes : u->pitch;
        a += ROUND_UP(w, aw) * bytes * ROUND_UP(h, ah);
    }
    /* one level is addressed, and cached, exactly as before */
    u->nlev = u->filt ? u->last + 1 : 1;
    u->chain = u->nlev > 1 ? a - u->off : (uint32_t)u->h * u->pitch;
    u->ltexels = 0;
    for (l = 0; l < u->nlev; l++) {
        u->ltexels += (size_t)u->lw[l] * u->lh[l];
    }
}

static void r300_tex_setup(ATIR350State *s, R300DrawState *d, unsigned unit)
{
    static const uint8_t endian_lanes[4] = { 0, 1, 3, 2 };
    R300TexUnit *u = &d->tex[unit];
    uint32_t txfmt0 = s->regs[(R300_TX_FORMAT0_0 >> 2) + unit];
    uint32_t txfmt1 = s->regs[(R300_TX_FORMAT1_0 >> 2) + unit];
    uint32_t txfmt2 = s->regs[(R300_TX_FORMAT2_0 >> 2) + unit];
    uint32_t filt0 = s->regs[(R300_TX_FILTER0_0 >> 2) + unit];
    unsigned txcode = txfmt1 & R300_TX_FORMAT1_CODE_MASK;
    unsigned ch;

    u->en = d->textured && (s->regs[R300_TX_ENABLE >> 2] & (1u << unit));
    u->off = s->regs[(R300_TX_OFFSET_0 >> 2) + unit] & ~0x1fu;
    u->lanes = endian_lanes[s->regs[(R300_TX_OFFSET_0 >> 2) + unit] &
                            R300_TXO_ENDIAN_MASK];
    u->w = (txfmt0 & 0x7ff) + 1;
    u->h = ((txfmt0 >> 11) & 0x7ff) + 1;
    /*
     * TX_FORMAT1's low format code (R3xx register reference, TXFORMAT
     * [4:0]): 0 is TX_FMT_8, the single-component format window drop
     * shadows arrive in (TX_FORMAT1=0x00124000); 3 is TX_FMT_8_8, the
     * two-component luminance/alpha sprite Flurry.saver's particles
     * are drawn with; 0x6 is TX_FMT_5_6_5 and 0xa TX_FMT_4_4_4_4;
     * 0xb is TX_FMT_1_5_5_5, which Abstract.saver asks for; 0xc is
     * TX_FMT_8_8_8_8, what the compositor and most
     * apps use; 0xe is TX_FMT_16_16_16_16, which RSS Visualizer.saver
     * asks for; 0x14 and 0x15 are the packed 4:2:2 TX_FMT_VYUY422 and
     * TX_FMT_YVYU422, two bytes per texel with each pair of texels
     * sharing one U and one V. r300_sample_tex() hands all of them to
     * the component select as four bytes, so one selector
     * implementation serves every format. TXPITCH counts texels, so
     * the byte pitch scales with the texel size -- reading an 8_8
     * texture as four bytes per texel doubled both the pitch and the
     * stride and made one dword span two texels.
     */
    u->code = txcode;
    u->yuv = 0;
    switch (txcode) {
    case R300_TX_FMT_8:
        u->bpp = 8;
        break;
    case R300_TX_FMT_8_8:
    case R300_TX_FMT_5_6_5:
    case R300_TX_FMT_4_4_4_4:
    case R300_TX_FMT_1_5_5_5:
        u->bpp = 16;
        break;
    case R300_TX_FMT_VYUY422:
    case R300_TX_FMT_YVYU422:
        u->bpp = 16;
        u->yuv = ((txfmt1 >> R300_TX_FORMAT1_YUV_SHIFT) &
                  R300_TX_FORMAT1_YUV_MASK) |
                 (txfmt1 & R300_TX_FORMAT1_SWAP_YUV ? 4 : 0);
        break;
    case R300_TX_FMT_16_16_16_16:
        u->bpp = 64;
        break;
    case R300_TX_FMT_8_8_8_8:
        u->bpp = 32;
        break;
    default:
        /* the rest are being read as if their components were bytes */
        u->bpp = 32;
        if (u->en) {
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
        if (r300_tex_yuv422(txcode)) {
            /* fixed (W,Z,Y,X) */
            u->sel[ch] = R300_TX_SEL_W - ch;
            continue;
        }
        u->sel[ch] = (txfmt1 >> (R300_TX_FORMAT1_SEL_SHIFT + ch * 3)) &
                     R300_TX_FORMAT1_SEL_MASK;
        if (u->en && u->sel[ch] > R300_TX_SEL_ONE) {
            ati_r350_note_gap(s, R350_GAP_TEX_SWIZZLE, u->sel[ch]);
        }
    }
    u->clamp_s = filt0 & 7;
    u->clamp_t = (filt0 >> 3) & 7;
    /*
     * The pitch register only applies when TX_FORMAT0 says so;
     * otherwise rows are exactly the texture's width.
     */
    if (txfmt0 & R300_TX_PITCH_EN) {
        u->pitch = ((txfmt2 & 0x3fff) + 1) * (u->bpp / 8);
    } else {
        u->pitch = (uint32_t)u->w * (u->bpp / 8);
    }
    r300_tex_filter_setup(s, u, unit, txfmt0, filt0);
}

/*
 * 3D_DRAW_IMMD_2: dw[0] is VAP_VF_CNTL (primitive type, walk mode,
 * vertex count), the rest is vertex data laid out VAP_VTX_SIZE dwords
 * per vertex.
 */
/* largest GART colour buffer staged per draw */
#define R300_CB_GART_MAX (16 * 1024 * 1024)

/*
 * A colour buffer in GART: the driver renders surface page-outs there.
 * Only what the draw can use is noted here -- the pitch and the rows up
 * to the scissor's bottom -- and r300_run_prims() stages those rows.
 */
static bool r300_cb_gart(ATIR350State *s, R300DrawState *d,
                         uint32_t colorpitch)
{
    static const uint8_t endian_xr[4] = { 0, 1, 3, 2 };
    uint32_t card = s->regs[R300_RB3D_COLOROFFSET0 >> 2] & ~0x1fu;
    unsigned fmt = (colorpitch >> R300_COLORFORMAT_SHIFT) &
                   R300_COLORFORMAT_MASK;
    unsigned bpp = r300_cb_bytes(fmt);
    uint32_t pitch = (colorpitch & 0x3fff) * bpp;
    uint32_t sc = s->regs[R300_SC_SCISSOR1 >> 2];
    int rows = (int)((sc >> 13) & 0x1fff) - R300_SCISSOR_OFFSET + 1;
    uint64_t size = (uint64_t)pitch * (rows > 0 ? rows : 0);

    if (!sc || !size || size > R300_CB_GART_MAX) {
        return false;
    }
    d->cb_host = true;
    d->cb_card = card;
    d->cb_size = size;
    d->cb_xr = endian_xr[(colorpitch >> R300_COLORENDIAN_SHIFT) & 3];
    d->dst_off = 0;
    return true;
}

static bool r300_setup_draw(ATIR350State *s, R300DrawState *d,
                            unsigned vsize)
{
    uint32_t colorpitch = s->regs[R300_RB3D_COLORPITCH0 >> 2];
    unsigned first_color = 0, ncolor = 0, first_tex = 0;
    bool vs_live = false;
    unsigned i;

    d->cb_fmt = (colorpitch >> R300_COLORFORMAT_SHIFT) &
                R300_COLORFORMAT_MASK;
    d->cb_bpp = r300_cb_bytes(d->cb_fmt);
    if (!r300_cb_modelled(d->cb_fmt)) {
        /* drawn as 32bpp it would overrun a narrower buffer */
        ati_r350_note_gap(s, R350_GAP_CB_FORMAT, d->cb_fmt);
        return false;
    }
    d->vram = memory_region_get_ram_ptr(&s->vram);
    d->cb = d->vram;
    d->cb_size = ATI_R350_VRAM_SIZE;
    d->cb_host = false;
    if (!ati_r350_mc_to_vram(s, s->regs[R300_RB3D_COLOROFFSET0 >> 2] & ~0x1fu,
                             &d->dst_off) &&
        !r300_cb_gart(s, d, colorpitch)) {
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
    {
        static const uint8_t sel_shift[4] = { 24, 16, 8, 0 };

        d->cb_sel = sel_shift[(s->regs[R300_US_OUT_FMT_0 >> 2] >>
                               R300_US_OUT_C0_SEL_SHIFT) & 3];
    }
    d->dst_pitch = (colorpitch & 0x3fff) * d->cb_bpp;
    if (!d->dst_pitch) {
        return false;
    }
    {
        uint32_t cm = s->regs[R300_RB3D_COLOR_CHANNEL_MASK >> 2];
        uint32_t zc = s->regs[R300_ZB_CNTL >> 2];
        uint32_t zp = s->regs[R300_ZB_DEPTHPITCH >> 2];
        uint32_t aa = s->regs[R300_GB_AA_CONFIG >> 2];
        unsigned zfmt = s->regs[R300_ZB_FORMAT >> 2] & 0xf;

        s->zb.z_en = false;
        if (zc & (R300_ZB_Z_ENABLE | R300_ZB_STENCIL_ENABLE)) {
            if (zfmt != R300_ZB_FORMAT_24_8 && zfmt != R300_ZB_FORMAT_16) {
                ati_r350_note_gap(s, R350_GAP_ZB_FORMAT, zfmt);
            } else if ((aa & R300_AA_ENABLE) && (aa & 6)) {
                /* three, four or six samples: layout unmeasured */
                ati_r350_note_gap(s, R350_GAP_ZB_FORMAT, 0x10 | (aa & 7));
            } else if (((zp >> 2) & 0xfff) &&
                       ati_r350_mc_to_vram(s,
                           s->regs[R300_ZB_DEPTHOFFSET >> 2] & ~0x1fu,
                           &s->zb.off)) {
                uint32_t rm = s->regs[R300_ZB_STENCILREFMASK >> 2];

                s->zb.z_en = true;
                s->zb.z_test = zc & R300_ZB_Z_ENABLE;
                s->zb.z_wr = s->zb.z_test && (zc & R300_ZB_ZWRITEENABLE);
                s->zb.s_en = zc & R300_ZB_STENCIL_ENABLE;
                s->zb.s_fb = zc & R300_ZB_STENCIL_FRONT_BACK;
                s->zb.zsc = s->regs[R300_ZB_ZSTENCILCNTL >> 2];
                s->zb.zfunc = s->zb.z_test ? s->zb.zsc & 7 : 7;
                s->zb.s_ref = rm & 0xff;
                s->zb.s_mask = (rm >> 8) & 0xff;
                s->zb.s_wmask = (rm >> 16) & 0xff;
                s->zb.pitch = ((zp >> 2) & 0xfff) * 4;
                s->zb.macro = zp & R300_ZB_MACROTILE;
                s->zb.micro = (zp >> R300_ZB_MICROTILE_SHIFT) & 3;
                s->zb.aa = aa & R300_AA_ENABLE;
                s->zb.z16 = zfmt == R300_ZB_FORMAT_16;
            }
        }
        if (!(cm & (R300_COLORMASK_BLUE | R300_COLORMASK_GREEN |
                    R300_COLORMASK_RED | R300_COLORMASK_ALPHA))) {
            /*
             * Every channel masked off: the colour buffer discards the
             * quads. Chess's depth-only passes arrive this way, and
             * shading them smeared a texture across the board; they
             * still write the Z buffer.
             */
            if (!s->zb.z_en) {
                return false;
            }
            d->wmask = 0;
        } else {
            d->wmask = (cm & R300_COLORMASK_ALPHA ? 0xff000000u : 0) |
                       (cm & R300_COLORMASK_RED   ? 0x00ff0000u : 0) |
                       (cm & R300_COLORMASK_GREEN ? 0x0000ff00u : 0) |
                       (cm & R300_COLORMASK_BLUE  ? 0x000000ffu : 0);
        }
    }
    /*
     * An AA resolve keeps rasterizing over the same geometry but sends
     * the colour buffer's contents, not the shaded fragment, to the
     * resolve buffer. Swap the destination here so scissor, cliprects
     * and the write mask all keep applying unchanged.
     */
    d->resolve = s->regs[R300_RB3D_AARESOLVE_CTL >> 2] & R300_AARESOLVE_MODE;
    if (d->resolve && d->cb_host) {
        ati_r350_note_gap(s, R350_GAP_DEST_OFF_VRAM, 1);
        return false;
    }
    d->res_off = 0;
    d->res_pitch = 0;
    if (d->resolve) {
        uint32_t roff;
        uint32_t rpitch = ((s->regs[R300_RB3D_AARESOLVE_PITCH >> 2] >> 1) &
                           0x1fff) * 2 * d->cb_bpp;

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
    /*
     * WHAT PROGRAM IS IN FORCE, resolved here rather than after the
     * texture and fragment stages because the gate immediately below
     * needs it. The analysis is a pure function of the control
     * registers and the program RAM -- it reads nothing this function
     * has computed -- so hoisting it changes nothing about its answer;
     * everything that DEPENDS on the texture state (which attribute a
     * coordinate lives in, whether the program's colour is a colour)
     * still happens further down, where that state exists.
     */
    d->xform = false;
    d->vte_xs = d->vte_xo = d->vte_ys = d->vte_yo = false;
    d->vs_run = false;
    d->vs_color = false;
    for (i = 0; i < R300_TEXCOORDS; i++) {
        d->vs_texcoord[i] = false;
        d->vs_tex_out[i] = 0;
        d->tex_attr[i] = -1;
    }
    memset(&d->vs, 0, sizeof(d->vs));
    if (!(s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_PVS_BYPASS) &&
        s->pvs_const_dwords >= 16 &&
        r300_f32(s->regs[R300_SE_VPORT_XSCALE >> 2]) != 0.0f) {
        r300_pvs_out_layout(s->regs[R300_VAP_OUTPUT_VTX_FMT_0 >> 2],
                            &first_color, &ncolor, &first_tex);
        r300_pvs_analyse(&d->vs, s->pvs_code, s->pvs_code_slot_valid,
                         R300_PVS_CODE_SLOTS,
                         s->pvs_const, R300_PVS_CONST_SLOTS,
                         s->regs[R300_VAP_PVS_CODE_CNTL_0 >> 2],
                         s->regs[R300_VAP_PVS_CONST_CNTL >> 2],
                         first_tex);
        vs_live = true;
    }
    /*
     * A VERTEX EIGHT DWORDS WIDE IS NOT THE ONLY VERTEX THAT CARRIES A
     * TEXTURE COORDINATE. The width test is here because a coordinate
     * read POSITIONALLY -- the dwords after a four-dword position --
     * needs the room; it is not a statement about the guest. Mac OS X
     * 10.5's layer-backed UIs (the Dock's icons, System Preferences'
     * labels and its wallpaper thumbnails) send a SEVEN-dword vertex,
     * FLOAT_4 position, one dword, FLOAT_2 coordinate, and compute the
     * coordinate in the vertex program -- so the width test refused the
     * texture outright, no unit was enabled, and the draws painted their
     * flat constant colour: (0,0,0,0) through a blend, i.e. nothing at
     * all.
     *
     * A program that COMPUTES the first coordinate says the draw is
     * textured as surely as the vertex width does, and r300_vs_texcoord
     * below can evaluate it wherever it lives. The census over every
     * capture this project holds is what makes this safe to widen: of
     * 20867 Mac OS X 10.4 and OS 9 draws, NOT ONE is textured, under
     * eight dwords wide and carrying a computed coordinate, so the
     * relaxation reaches nothing that renders today (scratchpad
     * texguard.py). It is deliberately NOT widened to a FORWARDED
     * coordinate: four of Beach.saver's draws would change and there is
     * no evidence they should.
     */
    d->textured = (s->regs[R300_TX_ENABLE >> 2] & 1) &&
                  (vsize >= 8 || r300_pvs_computes(&d->vs, first_tex));
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
            unsigned n;

            for (n = 0; n < ARRAY_SIZE(f); n++) {
                if (!r300_blend_known(f[n])) {
                    ati_r350_note_gap(s, R350_GAP_BLEND_FACTOR, f[n]);
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
    d->lod_any = false;
    for (i = 0; i < R300_TEX_UNITS; i++) {
        r300_tex_setup(s, d, i);
        d->lod_any |= d->tex[i].en && d->tex[i].need_lod;
    }
    /*
     * The fragment program is resolved HERE, before the vertex stage
     * below, because it is what says how many coordinate sets this draw
     * carries and which unit's texture scales each of them -- and the
     * vertex stage has to produce exactly those.
     */
    if (!r300_fs_setup(s, d)) {
        return false;
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

    if (vs_live) {
        int k;
        uint32_t vte;
        unsigned cb;

        {
            /*
             * The program in force was resolved above -- deciding it per
             * draw from the control registers, not from how much has
             * ever been uploaded, is what makes the result independent
             * of what ran before: an earlier attempt at this took the
             * bounds from a high-water mark of the upload stream, and the
             * same draw then rendered differently according to its
             * history.
             */
            r300_pvs_translate(s, &d->vs);
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
             * The format bits say which divides the setup engine still
             * owes: XY_FMT / Z_FMT clear, divide that coordinate;
             * W0_FMT set, the w handed over is w and not 1/w. The usual
             * draw is W0_FMT alone; pre-transformed vertices come with
             * all three clear (w is 1/w, typically 1).
             */
            s->zb.vte_fmt = vte & (R300_VTE_VTX_XY_FMT | R300_VTE_VTX_Z_FMT |
                                   R300_VTE_VTX_W0_FMT);
            d->vte_xs = vte & R300_VTE_VPORT_X_SCALE_ENA;
            d->vte_xo = vte & R300_VTE_VPORT_X_OFFSET_ENA;
            d->vte_ys = vte & R300_VTE_VPORT_Y_SCALE_ENA;
            d->vte_yo = vte & R300_VTE_VPORT_Y_OFFSET_ENA;
            s->zb.vte_zs = vte & R300_VTE_VPORT_Z_SCALE_ENA;
            s->zb.vte_zo = vte & R300_VTE_VPORT_Z_OFFSET_ENA;
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
            for (i = 0; i < d->ntc; i++) {
                unsigned o = first_tex + i;

                if (d->textured && d->vs.valid && o < R300_PVS_OUT_REGS &&
                    (d->vs.out_mask & (1u << o))) {
                    d->tex_attr[i] = d->vs.out_src[o];
                }
            }
            /*
             * The colour is the program's only when the vertex stage says
             * it emits one and the program really writes it. A colour it
             * merely forwards from an attribute this model is already
             * sampling as texture coordinates is not a colour at all --
             * the eight-dword textured vertices whose second attribute is
             * a coordinate pair would otherwise arrive painted with it.
             * Which attribute that is comes from the program: directly
             * where the coordinate is FORWARDED (tex_attr), and from the
             * input register its texture matrix reads where the program
             * COMPUTES it instead -- the matrix names that register as
             * surely as a forward does, and a computed coordinate is not
             * obliged to sit where a flat vertex would keep it. Mac OS X
             * 10.5's layer-backed UIs put a FLOAT_4 colour there and a
             * coordinate computed from the attribute after it, so the
             * position of the coordinate in a flat vertex answers only
             * for the programs whose matrix will not resolve.
             *
             * And nothing is being sampled as a coordinate at all unless
             * the fragment program fetches, which is why the test is
             * r300_draw_fetches() and not `textured`: a draw issued with
             * a texture merely BOUND has a colour in that attribute and
             * a colour is what it is.
             */
            if (d->vs_run && ncolor &&
                (d->vs.out_mask & (1u << first_color))) {
                R300PvsTexMat tm;
                int src = d->vs.out_src[first_color];
                int tsrc = d->tex_attr[0];
                bool computed = !d->vs.plain_matrix || src >= 0;
                bool is_texcoord;

                if (tsrc < 0) {
                    tsrc = r300_pvs_texmat(&d->vs, first_tex, &tm) ?
                           (int)tm.in : (vsize >= 12 ? 2 : 1);
                }
                is_texcoord = r300_draw_fetches(d) && src >= 0 &&
                              src == tsrc;

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
            for (i = 0; i < d->ntc; i++) {
                unsigned o = first_tex + i;

                d->vs_texcoord[i] = d->vs_run && !d->vs.plain_matrix &&
                                    d->textured && o < R300_PVS_OUT_REGS &&
                                    (d->vs.out_mask & (1u << o));
                d->vs_tex_out[i] = o;
            }
            /*
             * The second colour, for the fragment program that adds one.
             * VAP_OUTPUT_VTX_FMT_0 packs the colours after the position,
             * so it is simply the next output register, and it exists
             * only when the vertex stage declares two.
             */
            d->vs_color2_out = first_color + 1;
            d->vs_color2 = d->vs_run && !d->vs.plain_matrix && ncolor >= 2 &&
                           d->vs_color2_out < R300_PVS_OUT_REGS &&
                           (d->vs.out_mask & (1u << d->vs_color2_out));
        }
    }
    /*
     * Where each coordinate SET comes from, for the one question a live
     * multitexturing guest can answer and no corpus can: the rasterizer
     * routes a set, but something has to PRODUCE it, and a set nobody
     * produces interpolates zeros and samples texel (0,0) for every
     * pixel -- which looks exactly like a flat white panel.
     */
    if (trace_event_get_state_backends(TRACE_ATI_R350_3D_TEXSETS)) {
        uint32_t vsm = 0, atm = 0, un = 0, en = 0, fetched = 0;

        for (i = 0; i < d->ntc && i < 8; i++) {
            vsm |= d->vs_texcoord[i] ? (1u << i) : 0;
            atm |= d->tex_attr[i] >= 0 ? (1u << i) : 0;
            un |= (d->tc_unit[i] & 0xf) << (i * 4);
        }
        for (i = 0; i < R300_TEX_UNITS; i++) {
            en |= d->tex[i].en ? (1u << i) : 0;
        }
        for (i = 0; d->fs && i < d->fs->ntex; i++) {
            if (d->fs->tex[i].op == R300_US_TEXOP_LD ||
                d->fs->tex[i].op == R300_US_TEXOP_PROJ) {
                fetched |= 1u << (d->fs->tex[i].unit & 7);
            }
        }
        trace_ati_r350_3d_texsets(d->ntc, d->vs_tex_out[0], d->vs.out_mask,
                                  vsm, atm, un,
                                  s->regs[R300_VAP_OUTPUT_VTX_FMT_1 >> 2],
                                  s->regs[R300_TX_ENABLE >> 2], en, fetched);
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
     * constant colour: OS X's solid-fill shader outputs PFS_PARAM_0,
     * read through the same 24-bit float decode as the rest of the
     * constant file. The desktop backdrop fill arrives exactly this
     * way, a colourless full-screen quad with the blue in PFS_PARAM_0.
     *
     * "Carries no colour" is a statement about the VERTEX, so the test
     * has to be whether this draw's second attribute is being read as a
     * coordinate -- which it is only when the program fetches. A bound
     * but unfetched texture used to send the constant to white here, and
     * white over a whole surface is what a clear looks like when its
     * colour has been thrown away.
     */
    if (vsize < 12 && !r300_draw_fetches(d)) {
        d->flat_r = r300_us_f24(s->regs[(R300_PFS_PARAM_0_X >> 2)]);
        d->flat_g = r300_us_f24(s->regs[(R300_PFS_PARAM_0_X >> 2) + 1]);
        d->flat_b = r300_us_f24(s->regs[(R300_PFS_PARAM_0_X >> 2) + 2]);
        d->flat_a = r300_us_f24(s->regs[(R300_PFS_PARAM_0_X >> 2) + 3]);
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
 * Take a stripe of the given job, or -1 once they are all taken. The job
 * and its stripes share one word, so a thread that wakes late finds its
 * job over and takes nothing: the submitting thread waits only for the
 * stripes being drawn.
 */
static int r300_raster_take(R300Raster *q, uint32_t job)
{
    uint64_t next = qatomic_load_acquire(&q->next);

    for (;;) {
        uint64_t seen;

        if (next >> 32 != job ||
            extract64(next, 0, 16) > extract64(next, 16, 16)) {
            return -1;
        }
        seen = qatomic_cmpxchg(&q->next, next, next + 1);
        if (seen == next) {
            return extract64(next, 0, 16);
        }
        next = seen;
    }
}

static void r300_raster_stripes(ATIR350State *s, R300Raster *q, uint32_t job)
{
    int stripe;

    while ((stripe = r300_raster_take(q, job)) >= 0) {
        int first = stripe << R300_RASTER_STRIPE_SHIFT;
        int last = first + (1 << R300_RASTER_STRIPE_SHIFT) - 1;
        unsigned bin = first >> R300_RASTER_BIN_SHIFT;
        unsigned i;

        for (i = q->bin_at[bin]; i < q->bin_at[bin + 1]; i++) {
            const R300RasterTri *t = &q->tri[q->bin_tri[i]];

            if (t->y1 >= first && t->y0 <= last) {
                r300_raster_tri(s, q->d, t->v[0], t->v[1], t->v[2],
                                t->cull, first, last);
            }
        }
        qatomic_inc(&q->stripes_done);
    }
}

static void *r300_raster_thread(void *opaque)
{
    R300RasterWorker *w = opaque;
    ATIR350State *s = w->s;
    R300Raster *q = s->raster;

    /* stripes resolve VRAM and mark it dirty inside RCU read sections */
    rcu_register_thread();
    ati_r350_swap_memo_bind(&w->memo);
    for (;;) {
        qemu_sem_wait(&w->start);
        if (qatomic_read(&q->quit)) {
            break;
        }
        r300_raster_stripes(s, q, qatomic_load_acquire(&w->job));
    }
    rcu_unregister_thread();
    return NULL;
}

/* Draw the job's stripes with the submitting thread and `helpers` more. */
static void r300_raster_run(ATIR350State *s, R300Raster *q, unsigned helpers)
{
    int first = q->y0 >> R300_RASTER_STRIPE_SHIFT;
    int last = q->y1 >> R300_RASTER_STRIPE_SHIFT;
    unsigned stripes = last - first + 1;
    unsigned i;

    q->job++;
    qatomic_set(&q->stripes_done, 0);
    for (i = 0; i < helpers; i++) {
        qatomic_store_release(&q->worker[i].job, q->job);
    }
    qatomic_store_release(&q->next, (uint64_t)q->job << 32 |
                                    (uint64_t)last << 16 | first);
    for (i = 0; i < helpers; i++) {
        qemu_sem_post(&q->worker[i].start);
    }
    r300_raster_stripes(s, q, q->job);
    while (qatomic_load_acquire(&q->stripes_done) < stripes) {
        cpu_relax();
    }
}

static inline bool r300_ranges_meet(uint64_t a0, uint64_t a1,
                                    uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

/*
 * May the batch in hand be split, drawing rows [y0, y1] of about `px`
 * pixels? Everything a worker would read from the bus is copied to the
 * host here, and a draw that reads bytes it also writes is not split:
 * the order of that read and that write is the serial one only when a
 * single thread draws.
 */
static bool r300_raster_prepare(ATIR350State *s, R300Raster *q,
                                const R300DrawState *d, int y0, int y1,
                                uint64_t px)
{
    uint64_t limit = MIN(MAX(px * 4, R300_SHADOW_MIN_BYTES),
                         R300_SHADOW_MAX_BYTES);
    uint64_t rows = (uint64_t)y1 + 1;
    uint64_t w0 = 0, w1 = 0, z0 = 0, z1 = 0;
    unsigned u;

    if (d->wmask && !d->cb_host) {
        w0 = d->dst_off + (uint64_t)y0 * d->dst_pitch;
        w1 = d->dst_off + rows * d->dst_pitch;
    }
    if (s->zb.z_en) {
        /* every byte a tiled layout can place above row y1 */
        z0 = s->zb.off;
        z1 = s->zb.off + QEMU_ALIGN_UP(rows, 16) * s->zb.pitch *
                         (s->zb.z16 ? 2 : 4) * (s->zb.aa ? 2 : 1);
    }
    if (d->resolve) {
        uint64_t r0 = d->res_off + (uint64_t)y0 * d->res_pitch;
        uint64_t r1 = d->res_off + rows * d->res_pitch;

        if (d->cb_host || r300_ranges_meet(r0, r1, w0, w1) ||
            r300_ranges_meet(r0, r1, z0, z1)) {
            return false;
        }
    }
    for (u = 0; u < R300_TEX_UNITS; u++) {
        const R300TexUnit *t = &d->tex[u];
        uint32_t len, first, last;

        if (!t->en) {
            continue;
        }
        if (t->w <= 0 || t->h <= 0) {
            return false;
        }
        len = (uint32_t)(t->h - 1) * t->pitch + (uint32_t)t->w * (t->bpp / 8);
        if (ati_r350_mc_to_vram(s, t->off, &first) &&
            ati_r350_mc_to_vram(s, t->off + len - 1, &last) &&
            last == first + len - 1) {
            if (r300_ranges_meet(first, first + len, w0, w1) ||
                r300_ranges_meet(first, first + len, z0, z1)) {
                return false;
            }
            continue;
        }
        if (len > limit) {
            return false;
        }
        if (q->shadow_size[u] < len + 4) {
            g_free(q->shadow[u]);
            q->shadow_size[u] = len + 4;
            q->shadow[u] = g_malloc(q->shadow_size[u]);
        }
        q->shadow_base[u] = t->off & ~3u;
        q->shadow_dw[u] = (len + 3) / 4;
        ati_r350_mc_read_block(s, q->shadow_base[u], q->shadow[u],
                               q->shadow_dw[u]);
        q->shadowed[u] = true;
    }
    return true;
}

/* index the batch's triangles by the bins of rows they touch */
static void r300_raster_bin(R300Raster *q)
{
    unsigned i, b, n = 0;

    memset(q->bin_at, 0, sizeof(q->bin_at));
    for (i = 0; i < q->ntri; i++) {
        for (b = q->tri[i].y0 >> R300_RASTER_BIN_SHIFT;
             b <= q->tri[i].y1 >> R300_RASTER_BIN_SHIFT; b++) {
            q->bin_at[b + 1]++;
            n++;
        }
    }
    for (b = 0; b < R300_RASTER_BINS; b++) {
        q->bin_at[b + 1] += q->bin_at[b];
    }
    if (q->bin_cap < n) {
        q->bin_cap = MAX(n, q->bin_cap * 2);
        g_free(q->bin_tri);
        q->bin_tri = g_new(unsigned, q->bin_cap);
    }
    for (i = 0; i < q->ntri; i++) {
        for (b = q->tri[i].y0 >> R300_RASTER_BIN_SHIFT;
             b <= q->tri[i].y1 >> R300_RASTER_BIN_SHIFT; b++) {
            q->bin_tri[q->bin_at[b]++] = i;
        }
    }
    /* each bin's cursor ended at the next bin's start: shift back */
    for (b = R300_RASTER_BINS; b > 0; b--) {
        q->bin_at[b] = q->bin_at[b - 1];
    }
    q->bin_at[0] = 0;
}

/* Draw everything queued, in submission order per pixel. */
static void r300_raster_flush(ATIR350State *s)
{
    R300Raster *q = s->raster;
    const R300DrawState *d = q->d;
    unsigned i, n = q->ntri;
    unsigned stripes, helpers;

    if (!n) {
        return;
    }
    stripes = (q->y1 >> R300_RASTER_STRIPE_SHIFT) -
              (q->y0 >> R300_RASTER_STRIPE_SHIFT) + 1;
    helpers = MIN(q->nworkers, stripes - 1);
    if (helpers && (q->px < R300_RASTER_MIN_PX ||
                    !r300_raster_prepare(s, q, d, q->y0, q->y1, q->px))) {
        helpers = 0;
    }
    if (!helpers) {
        s->raster_tri_serial += n;
        for (i = 0; i < n; i++) {
            const R300RasterTri *t = &q->tri[i];

            r300_raster_tri(s, d, t->v[0], t->v[1], t->v[2], t->cull,
                            INT_MIN, INT_MAX);
        }
    } else {
        s->raster_tri_split += n;
        r300_raster_bin(q);
        r300_raster_run(s, q, helpers);
    }
    q->ntri = 0;
    q->px = 0;
    q->vblk_cur = 0;
    q->vblk_used = 0;
}

/*
 * Where the primitive walk may keep `n` vertices it made up: the batch's
 * own store while queueing, since the triangles naming them are drawn
 * later, and the caller's copy otherwise.
 */
static const R300Vtx *r300_keep(ATIR350State *s, const R300Vtx *v,
                                unsigned n)
{
    R300Raster *q = s->raster;
    R300Vtx *p;

    if (!q || !q->queueing) {
        return v;
    }
    if (q->ntri >= R300_RASTER_QUEUE_MAX) {
        /* the caller holds none of the store: it may start over */
        r300_raster_flush(s);
    }
    if (q->nvblk && q->vblk_used + n > R300_RASTER_VTX_BLOCK) {
        q->vblk_cur++;
        q->vblk_used = 0;
    }
    if (q->vblk_cur == q->nvblk) {
        q->vblk = g_renew(R300Vtx *, q->vblk, q->nvblk + 1);
        q->vblk[q->nvblk++] = g_new(R300Vtx, R300_RASTER_VTX_BLOCK);
    }
    p = q->vblk[q->vblk_cur] + q->vblk_used;
    q->vblk_used += n;
    memcpy(p, v, n * sizeof(*v));
    return p;
}

/*
 * Queue a primitive's triangles from here on, when there is a pool to
 * draw them and nothing that must watch them drawn one by one.
 */
static void r300_raster_begin(ATIR350State *s, const R300DrawState *d)
{
    R300Raster *q = s->raster;

    if (q && !s->cap_fp) {
        q->queueing = true;
        q->d = d;
    }
}

/* The primitive is walked: nothing it queued outlives it. */
static void r300_raster_end(ATIR350State *s)
{
    R300Raster *q = s->raster;
    unsigned u;

    if (!q || !q->queueing) {
        return;
    }
    r300_raster_flush(s);
    for (u = 0; u < R300_TEX_UNITS; u++) {
        q->shadowed[u] = false;
    }
    q->queueing = false;
    q->d = NULL;
}

/* one triangle of a primitive: queued for the batch, or drawn here */
static void r300_tri(ATIR350State *s, const R300DrawState *d,
                     const R300Vtx *v0, const R300Vtx *v1, const R300Vtx *v2,
                     unsigned cull)
{
    R300Raster *q = s->raster;
    int x0, y0, x1, y1;

    if (!q || !q->queueing) {
        s->raster_tri_serial++;
        r300_raster_tri(s, d, v0, v1, v2, cull, INT_MIN, INT_MAX);
        return;
    }
    r300_tri_bounds(d, v0, v1, v2, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) {
        return;                     /* the scan is empty */
    }
    if (q->ntri == q->cap) {
        q->cap = MAX(q->cap * 2, 64);
        q->tri = g_renew(R300RasterTri, q->tri, q->cap);
    }
    q->tri[q->ntri++] = (R300RasterTri) {
        .v = { v0, v1, v2 }, .cull = cull, .y0 = y0, .y1 = y1 - 1,
    };
    q->px += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
    q->y0 = q->ntri == 1 ? y0 : MIN(q->y0, y0);
    q->y1 = q->ntri == 1 ? y1 - 1 : MAX(q->y1, y1 - 1);
    if (q->ntri >= R300_RASTER_QUEUE_MAX && !q->vblk_cur && !q->vblk_used) {
        /*
         * The draw state is the packet's throughout, so a batch may end
         * anywhere inside it. A primitive that makes up vertices ends
         * it in r300_keep() instead, where the caller holds none.
         */
        r300_raster_flush(s);
    }
}

void ati_r350_raster_init(ATIR350State *s)
{
    unsigned want = s->raster_threads;
    R300Raster *q;
    unsigned i;

    if (!want) {
        unsigned cpus = g_get_num_processors();

        /* leave the vCPUs and the main loop room */
        want = cpus > 2 ? cpus / 2 : 1;
    }
    want = MIN(want, R300_RASTER_MAX_THREADS);
    if (want <= 1) {
        return;
    }
    q = g_new0(R300Raster, 1);
    s->raster = q;
    for (i = 0; i < want - 1; i++) {
        R300RasterWorker *w = &q->worker[i];
        char name[24];

        w->s = s;
        qemu_sem_init(&w->start, 0);
        snprintf(name, sizeof(name), "ati-r350-raster%u", i);
        qemu_thread_create(&w->thread, name, r300_raster_thread, w,
                           QEMU_THREAD_JOINABLE);
    }
    q->nworkers = want - 1;
}

void ati_r350_raster_fini(ATIR350State *s)
{
    R300Raster *q = s->raster;
    unsigned i;

    if (!q) {
        return;
    }
    qatomic_set(&q->quit, true);
    for (i = 0; i < q->nworkers; i++) {
        qemu_sem_post(&q->worker[i].start);
    }
    for (i = 0; i < q->nworkers; i++) {
        qemu_thread_join(&q->worker[i].thread);
        qemu_sem_destroy(&q->worker[i].start);
    }
    for (i = 0; i < R300_TEX_UNITS; i++) {
        g_free(q->shadow[i]);
    }
    for (i = 0; i < q->nvblk; i++) {
        g_free(q->vblk[i]);
    }
    g_free(q->vblk);
    g_free(q->bin_tri);
    g_free(q->tri);
    g_free(q);
    s->raster = NULL;
}

/*
 * One line segment, expanded to a quad a pixel wide across its own
 * direction and handed to the triangle rasterizer so it picks up the
 * same texturing, blending and clipping as everything else.
 */
static void r300_raster_line(ATIR350State *s, const R300DrawState *d,
                             const R300Vtx *a, const R300Vtx *b,
                             unsigned cull)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    R300Vtx q[4];
    const R300Vtx *k;
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
    k = r300_keep(s, q, 4);
    r300_tri(s, d, &k[0], &k[1], &k[2], cull);
    r300_tri(s, d, &k[0], &k[2], &k[3], cull);
}

static void r300_raster_prims(ATIR350State *s, R300DrawState *d,
                              const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    unsigned i;
    /* points, lines and rectangles are never culled and always front */
    unsigned cull = (prim >= 4 && prim <= 7) || (prim >= 13 && prim <= 15)
                    ? s->regs[R300_RE_CULL_CNTL >> 2] & 7 : 8;

    r300_raster_begin(s, d);
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
        float s0 = r300_f32(s->regs[R300_GA_POINT_S0 >> 2]) * d->tex[0].w;
        float s1 = r300_f32(s->regs[R300_GA_POINT_S1 >> 2]) * d->tex[0].w;
        /*
         * T0 pairs with the sprite's BOTTOM edge and T1 with the top
         * (GL-style v axis): the full-screen composite arrives as
         * T0=1, T1=0 over a layer stored top-down, and mapping T0 to
         * the top edge mirrored the whole desktop vertically.
         */
        float t1 = r300_f32(s->regs[R300_GA_POINT_T0 >> 2]) * d->tex[0].h;
        float t0 = r300_f32(s->regs[R300_GA_POINT_T1 >> 2]) * d->tex[0].h;
        unsigned u;

        /*
         * The sprite path re-derives `textured` from TX_ENABLE alone --
         * a point vertex has no room for a coordinate, so the vertex
         * size gate the setup applied does not apply here. The per-unit
         * enables have to follow it, or a unit would stay switched off
         * against the flag that is meant to speak for it.
         *
         * Which is a statement about BINDING, and the sprite's colour is
         * a question about USE -- so the whitening below asks
         * r300_draw_fetches() while the enables keep following
         * TX_ENABLE.
         */
        d->textured = s->regs[R300_TX_ENABLE >> 2] & 1;
        for (u = 0; u < R300_TEX_UNITS; u++) {
            d->tex[u].en = d->textured &&
                           (s->regs[R300_TX_ENABLE >> 2] & (1u << u));
        }
        for (i = 0; i < nvtx && sx > 0.0f && sy > 0.0f; i++) {
            R300Vtx q[4];
            const R300Vtx *k;
            int c;

            for (c = 0; c < 4; c++) {
                q[c] = vb[i];
                if (r300_draw_fetches(d)) {
                    /* composite sprites modulate by nothing */
                    q[c].r = q[c].g = q[c].b = q[c].a = 1.0f;
                }
            }
            q[0].x = vb[i].x - sx / 2; q[0].y = vb[i].y - sy / 2;
            q[0].tc[0][0] = s0; q[0].tc[0][1] = t0;
            q[1].x = vb[i].x + sx / 2; q[1].y = q[0].y;
            q[1].tc[0][0] = s1; q[1].tc[0][1] = t0;
            q[2].x = q[1].x; q[2].y = vb[i].y + sy / 2;
            q[2].tc[0][0] = s1; q[2].tc[0][1] = t1;
            q[3].x = q[0].x; q[3].y = q[2].y;
            q[3].tc[0][0] = s0; q[3].tc[0][1] = t1;
            k = r300_keep(s, q, 4);
            r300_tri(s, d, &k[0], &k[1], &k[2], cull);
            r300_tri(s, d, &k[0], &k[2], &k[3], cull);
        }
        break;
    }
    case 2:     /* line list */
        for (i = 0; i + 2 <= nvtx; i += 2) {
            r300_raster_line(s, d, &vb[i], &vb[i + 1], cull);
        }
        break;
    case 3:     /* line strip */
        for (i = 1; i < nvtx; i++) {
            r300_raster_line(s, d, &vb[i - 1], &vb[i], cull);
        }
        break;
    case 12:    /* line loop: a strip that closes back on itself */
        for (i = 1; i < nvtx; i++) {
            r300_raster_line(s, d, &vb[i - 1], &vb[i], cull);
        }
        if (nvtx > 2) {
            r300_raster_line(s, d, &vb[nvtx - 1], &vb[0], cull);
        }
        break;
    case 4:     /* triangle list */
    case 7:     /* TRI_TYPE2: a triangle list with its own vertex
                 * routing; the assembly into triangles is the same */
        for (i = 0; i + 3 <= nvtx; i += 3) {
            r300_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2], cull);
        }
        break;
    case 5:     /* triangle fan */
    case 15:    /* polygon: fan-assembled, convex by definition here */
        for (i = 2; i < nvtx; i++) {
            r300_tri(s, d, &vb[0], &vb[i - 1], &vb[i], cull);
        }
        break;
    case 6:     /* triangle strip: every second triangle is wound backwards */
        for (i = 2; i < nvtx; i++) {
            r300_tri(s, d, &vb[i - 2], &vb[i - 1], &vb[i],
                     (cull & ~16u) | (i & 1) << 4);
        }
        break;
    case 8:     /* rectangle list: three corners, fourth implied */
        for (i = 0; i + 3 <= nvtx; i += 3) {
            R300Vtx v3 = vb[i + 2];
            unsigned k;

            /* the missing corner is v0 + (v1 - v0) + (v2 - v0) */
            v3.x = vb[i + 1].x + vb[i + 2].x - vb[i].x;
            v3.y = vb[i + 1].y + vb[i + 2].y - vb[i].y;
            for (k = 0; k < R300_TEXCOORDS; k++) {
                unsigned c;

                v3.tc[k][0] = vb[i + 1].tc[k][0] + vb[i + 2].tc[k][0] -
                              vb[i].tc[k][0];
                v3.tc[k][1] = vb[i + 1].tc[k][1] + vb[i + 2].tc[k][1] -
                              vb[i].tc[k][1];
                for (c = 0; c < 4; c++) {
                    v3.tcr[k][c] = vb[i + 1].tcr[k][c] +
                                   vb[i + 2].tcr[k][c] - vb[i].tcr[k][c];
                }
            }
            r300_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2], cull);
            r300_tri(s, d, &vb[i + 1], r300_keep(s, &v3, 1), &vb[i + 2],
                     cull);
        }
        break;
    case 14:    /* quad strip: each further vertex pair closes a quad
                 * against the previous pair (Chess.app draws its board
                 * and pieces almost entirely out of these) */
        for (i = 2; i + 2 <= nvtx; i += 2) {
            r300_tri(s, d, &vb[i - 2], &vb[i - 1], &vb[i + 1], cull);
            r300_tri(s, d, &vb[i - 2], &vb[i + 1], &vb[i], cull);
        }
        break;
    case 13:    /* quad list */
        for (i = 0; i + 4 <= nvtx; i += 4) {
            r300_tri(s, d, &vb[i], &vb[i + 1], &vb[i + 2], cull);
            r300_tri(s, d, &vb[i], &vb[i + 2], &vb[i + 3], cull);
        }
        break;
    default:
        trace_ati_r350_3d_skip(0, nvtx, prim);
        ati_r350_note_gap(s, R350_GAP_PRIM, prim);
        break;
    }
    r300_raster_end(s);
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
 *
 * `empty`, when not NULL, comes back true for the ONE refusal that is a
 * PROOF that the software rasterizer would paint nothing either: the
 * widened bounding box is empty after the scissor. That is a proof and
 * not an impression, because this box is a superset of every box
 * r300_raster_tri() will scan for this draw --
 *
 *   - the vertex loop above takes the minimum/maximum over ALL of `vb`
 *     (plus the corner a rectangle list implies, plus the point
 *     sprite's RE_POINTSIZE half-extent, both of which the rasterizer
 *     synthesises from the same registers), so any triangle it
 *     assembles has its own min/max inside fx0..fx1, fy0..fy1;
 *   - floorf()-1 / ceilf()+1 widen that by a further pixel, so the
 *     rasterizer's un-widened floorf()/ceilf() bounds stay inside;
 *   - the four clamps below are character for character the ones
 *     r300_raster_tri() applies, and r300_span_clip() only ever
 *     narrows a row further.
 *
 * So x1 <= x0 or y1 <= y0 here means every scan loop there is empty.
 * The other refusals are NOT that proof and must not be treated as one:
 * a non-finite or out-of-range coordinate says only that this helper
 * declined to think about the draw, a zero pitch still lets the
 * rasterizer write row 0 over and over, and the VRAM trim below drops
 * rows using a widened x1 the real primitive may fall well short of.
 */
static bool r300_cap_rect(ATIR350State *s, const R300DrawState *d,
                          const R300Vtx *vb, unsigned nvtx, unsigned prim,
                          int *rx0, int *ry0, int *rx1, int *ry1,
                          bool *empty)
{
    float fx0 = vb[0].x, fy0 = vb[0].y, fx1 = fx0, fy1 = fy0;
    int x0, y0, x1, y1;
    unsigned i;

    if (empty) {
        *empty = false;
    }
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
    if (x1 <= x0 || y1 <= y0) {
        if (empty) {
            *empty = true;
        }
        return false;
    }
    if (!d->dst_pitch) {
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
    uint32_t tex_hash[R300_TEX_UNITS] = { 0 };
    uint32_t tx_enable = s->regs[R300_TX_ENABLE >> 2];
    unsigned dxr = 0;
    int x0, y0, x1, y1;
    unsigned i, u;

    /*
     * A draw is recorded only when a record can describe it exactly.
     * Anything else is counted and rasterized as usual: a capture that
     * quietly stored an approximation would be worse than a short one,
     * because the harness reading it cannot tell the two apart.
     */
    if (d->resolve || d->cb_bpp != 4 || d->cb_host ||
        !r300_cap_rect(s, d, vb, nvtx, prim, &x0, &y0, &x1, &y1, NULL) ||
        (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0) > s->cap_max_px ||
        !r300_cap_xor(s, d->dst_off + (uint32_t)y0 * d->dst_pitch,
                      (uint32_t)(y1 - 1 - y0) * d->dst_pitch +
                      (uint32_t)x1 * 4, &dxr)) {
        s->cap_skipped++;
        r300_raster_prims(s, d, vb, nvtx, prim);
        return;
    }
    /*
     * Every unit this draw samples, each with its own range. The point
     * sprite path re-derives its texturing from TX_ENABLE after the
     * setup ran, so a prim=1 draw is recorded on the register rather
     * than on the flag -- which is what the single-texture code did
     * too, one unit at a time.
     */
    for (u = 0; u < R300_TEX_UNITS; u++) {
        const R300TexUnit *tu = &d->tex[u];
        uint32_t off, last, len;

        if (!tu->en && !(prim == 1 && (tx_enable & (1u << u)))) {
            continue;
        }
        len = tu->pitch * (uint32_t)tu->h;
        if (!len || !ati_r350_mc_to_vram(s, tu->off, &off) ||
            !ati_r350_mc_to_vram(s, tu->off + len - 1, &last) ||
            last != off + len - 1 ||
            !r300_cap_xor(s, off, len, &h.tex[u].xor)) {
            s->cap_skipped++;
            r300_raster_prims(s, d, vb, nvtx, prim);
            return;
        }
        h.tex[u].vram_off = off;
        h.tex[u].bytes = len;
        h.tex[u].txfmt1 = s->regs[(R300_TX_FORMAT1_0 >> 2) + u];
        tex_hash[u] = r300_cap_hash(d->vram + off, len);
    }

    h.magic = R350_CAP_REC_MAGIC;
    h.index = s->cap_index;
    h.prim = prim;
    h.nvtx = nvtx;
    h.x0 = x0; h.y0 = y0; h.x1 = x1; h.y1 = y1;
    h.rect_bytes = (uint32_t)(x1 - x0) * 4 * (uint32_t)(y1 - y0);
    h.dst_xor = dxr;
    h.pointsize = s->regs[R300_RE_POINTSIZE >> 2];
    h.point_s0 = s->regs[R300_GA_POINT_S0 >> 2];
    h.point_s1 = s->regs[R300_GA_POINT_S1 >> 2];
    h.point_t0 = s->regs[R300_GA_POINT_T0 >> 2];
    h.point_t1 = s->regs[R300_GA_POINT_T1 >> 2];
    h.tx_enable = tx_enable;
    h.flags = (d->vs_run ? R350_CAP_F_VS_RUN : 0) |
              (d->vs.plain_matrix ? R350_CAP_F_PLAIN_MAT : 0);

    for (u = 0; u < R300_TEX_UNITS; u++) {
        R350CapTex *t = &h.tex[u];

        if (!t->bytes) {
            continue;
        }
        for (i = 0; i < s->cap_tex_n; i++) {
            if (s->cap_tex[i].off == t->vram_off &&
                s->cap_tex[i].len == t->bytes &&
                s->cap_tex[i].xr == t->xor &&
                s->cap_tex[i].hash == tex_hash[u]) {
                t->bytes = 0;
                t->ref = s->cap_tex[i].rec;
                t->dedup = 1;
                break;
            }
        }
        if (!t->dedup) {
            i = s->cap_index % R350_CAP_TEX_CACHE;
            s->cap_tex[i].off = t->vram_off;
            s->cap_tex[i].len = t->bytes;
            s->cap_tex[i].xr = t->xor;
            s->cap_tex[i].hash = tex_hash[u];
            s->cap_tex[i].rec = s->cap_index;
            if (s->cap_tex_n < R350_CAP_TEX_CACHE) {
                s->cap_tex_n++;
            }
        }
    }

    /*
     * The VRAM pointer, the vertex program and the fragment program are
     * the only members of the state that are not plain data. The harness
     * substitutes its own VRAM and never resurrects a vertex program --
     * the positions are already transformed. The FRAGMENT program is a
     * different matter: since milestone M5 it is what the draw shades
     * with, so it is written out by value and the pointer is cleared.
     */
    st = *d;
    st.vram = NULL;
    st.fs = NULL;
    memset(&st.vs, 0, sizeof(st.vs));

    before = g_malloc(h.rect_bytes);
    after = g_malloc(h.rect_bytes);
    r300_cap_read_rect(d, x0, y0, x1, y1, before);
    r300_raster_prims(s, d, vb, nvtx, prim);
    r300_cap_read_rect(d, x0, y0, x1, y1, after);

    r300_cap_write(s, &h, sizeof(h));
    r300_cap_write(s, &st, sizeof(st));
    r300_cap_write(s, &s->us_prog, sizeof(s->us_prog));
    r300_cap_write(s, vb, sizeof(*vb) * nvtx);
    for (u = 0; u < R300_TEX_UNITS; u++) {
        if (h.tex[u].bytes) {
            r300_cap_write(s, d->vram + h.tex[u].vram_off, h.tex[u].bytes);
        }
    }
    r300_cap_write(s, before, h.rect_bytes);
    r300_cap_write(s, after, h.rect_bytes);
    trace_ati_r350_3d_cap(s->cap_index, prim, nvtx, x0, y0, x1, y1,
                          h.tex[0].bytes);
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

static void r300_gl_texdrop(ATIR350State *s)
{
    unsigned k;

    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        s->gl_tex[k].live = false;
        s->gl_tex[k].up = false;
    }
    s->gl_tex_any = false;
}

static const char *const r300_gl_rel_names[R350_GLR_MAX] = {
    [R350_GLR_SCANOUT]  = "scanout",
    [R350_GLR_RING]     = "ring end",
    [R350_GLR_IB]       = "indirect buffer end",
    [R350_GLR_FIFO]     = "CP FIFO push",
    [R350_GLR_READ]     = "ranged read",
    [R350_GLR_2D]       = "2D engine",
    [R350_GLR_TARGET]   = "target changed",
    [R350_GLR_FALLBACK] = "draw fell back",
    [R350_GLR_BACKEND]  = "backend declined",
    [R350_GLR_RESET]    = "reset",
    [R350_GLR_FENCE]    = "fence",
};

const char *ati_r350_gl_rel_name(ATIR350GlRel why)
{
    return why < R350_GLR_MAX && r300_gl_rel_names[why]
           ? r300_gl_rel_names[why] : "?";
}

/*
 * TEXTURE CACHE LIFETIME -- the rule, and why it is no longer the
 * target's.
 *
 * M3 tied a decoded texture's life to the resident render target's:
 * both died at every release, on the argument that those are the
 * moments something outside the 3D engine may have touched VRAM. That
 * is SUFFICIENT but far stronger than necessary, and it is expensive:
 * on Flurry.saver the target is released about once per draw, so the
 * cache read 34 hits against 5990 decodes -- 0.6% -- and
 * r300_gl_decode_tex() took 41% of the vCPU's executing time.
 *
 * What a decoded entry actually depends on is its SOURCE BYTES: it is
 * valid exactly while VRAM in [off, off+len) is unwritten. Every writer
 * is therefore an enforcement point, and each one already exists or is
 * added here:
 *
 *   the 2D engine, host-data     ati_r350_gl_dirty() -> gl_wrote()
 *   pushes, CP write-backs,      kills every overlapping entry
 *   the MM_DATA window
 *
 *   a 3D draw rendering into     r300_gl_bind()'s loop, same test
 *   a cached range
 *
 *   the GUEST CPU through the    THE DIRTY BITMAP. Its stores are
 *   frame-buffer BAR             ordinary host stores with no callback,
 *                                but they set DIRTY_MEMORY_VGA, which
 *                                is the same mechanism the scanout has
 *                                always relied on to notice them.
 *
 *   reset, unrealize             r300_gl_texdrop()
 *
 * READING THE BITMAP WITHOUT DISTURBING IT, AND THE ONE-BIT PROBLEM.
 * The scanout owns that bitmap: it snapshots and CLEARS the whole of
 * VRAM on every refresh, and it decides both what to redraw and where
 * the framebuffer is from what it finds. So this guard never clears a
 * bit and never claims a range -- it only reads.
 *
 * Reading alone is enough only because of how an entry is ADMITTED. A
 * dirty bit does not say WHEN the page was written, so a page that is
 * already dirty when the texture is decoded can never afterwards be
 * distinguished from one written a moment later: the bit is set either
 * way. A baseline of "dirty at decode time" would therefore be a
 * permanent blind spot over exactly those pages. So an entry is
 * admitted to the cache ONLY when every page of its source range reads
 * clean, and from then on ANY set bit is a write that happened since.
 *
 *   admission          every page clean, or the texture is decoded into
 *                      the scratch this time and not cached
 *   between refreshes  r300_gl_tex_current() re-reads the live flags
 *   at a refresh       ati_r350_gl_epoch() is handed the snapshot the
 *                      clear produced -- the only record of what was
 *                      set -- applies the same test to every live entry
 *                      before it is discarded, and carries the
 *                      survivors into the next generation
 *
 * An entry whose generation is not the current one was not carried, so
 * it is killed. Between them the two checks see every write, at page
 * granularity rounded outwards, so the guard errs towards killing.
 *
 * THE PRICE of admission, stated: a texture cannot enter the cache
 * until the bitmap has been cleared since it was last written -- one
 * display refresh after its upload, 16 ms on a live display. It is
 * decoded normally in the meantime.
 *
 * The device's own writes set the same bits and so also kill entries.
 * That is conservative rather than wrong: a flush or a blit into a
 * texture's range really does stale it.
 *
 * THE CONTROL. `gl-texlife=never` switches off EVERY one of the
 * enforcement points above -- the dirty guard, the writer hook's kill
 * and the render-into-a-cached-range kill alike -- so that "the cache
 * is invalidated correctly" is a claim something can disprove. A
 * gl=verify run in that mode must FAIL, and offline the replay harness
 * must diverge, which is what makes this rule a measurement rather
 * than an argument.
 */
static unsigned r300_gl_pages(ATIR350State *s, uint32_t off, uint32_t len)
{
    unsigned bits = s->gl_pgbits;

    return (unsigned)((((uint64_t)off + len + ((1u << bits) - 1)) >> bits) -
                      (off >> bits));
}

/* is any page of this VRAM range marked written? */
static bool r300_gl_range_dirty(ATIR350State *s, uint32_t off, unsigned npg)
{
    ram_addr_t base = memory_region_get_ram_addr(&s->vram) +
                      (off & ~(ram_addr_t)((1u << s->gl_pgbits) - 1));
    unsigned i;

    for (i = 0; i < npg; i++) {
        if (physical_memory_get_dirty_flag(base +
                                           ((ram_addr_t)i << s->gl_pgbits),
                                           DIRTY_MEMORY_VGA)) {
            return true;
        }
    }
    return false;
}

static bool r300_gl_tex_current(ATIR350State *s, unsigned k)
{
    if (s->gl_texlife != R350_TEXLIFE_DIRTY) {
        /* burst: dropped at release instead. never: the control. */
        return true;
    }
    return s->gl_tex[k].epoch == s->gl_epoch &&
           !r300_gl_range_dirty(s, s->gl_tex[k].off, s->gl_tex[k].npg);
}

void ati_r350_gl_epoch(ATIR350State *s, DirtyBitmapSnapshot *snap)
{
    bool any = false;
    unsigned k, i;

    s->gl_epoch++;
    if (s->gl_texlife != R350_TEXLIFE_DIRTY) {
        return;
    }
    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        uint32_t p0;
        bool stale = false;

        if (!s->gl_tex[k].live) {
            continue;
        }
        p0 = s->gl_tex[k].off & ~((1u << s->gl_pgbits) - 1);
        for (i = 0; i < s->gl_tex[k].npg && !stale; i++) {
            stale = memory_region_snapshot_get_dirty(&s->vram, snap,
                        p0 + ((uint64_t)i << s->gl_pgbits),
                        (uint64_t)1 << s->gl_pgbits);
        }
        if (stale) {
            s->gl_tex[k].live = false;
            s->gl_tex[k].up = false;
            s->gl_tex_stale++;
        } else {
            s->gl_tex[k].epoch = s->gl_epoch;
            any = true;
        }
    }
    s->gl_tex_any = any;
}

void ati_r350_gl_release(ATIR350State *s, ATIR350GlRel why)
{
    uint64_t px;

    if (!ati_r350_gl_mine(s)) {
        return;         /* the command processor's until it finishes */
    }
    if (s->gl_texlife == R350_TEXLIFE_BURST) {
        r300_gl_texdrop(s);
    }
    if (!s->gl_res) {
        return;
    }
    px = s->gl_flush_px;
    r300_gl_flush(s);
    r300_gl_discard(s);
    s->gl_res = false;
    s->gl_rel[why]++;
    s->gl_rel_px[why] += s->gl_flush_px - px;
}

/*
 * Device reset. The target goes back and the decoded textures are
 * dropped. The backend keeps its grow-only colour buffer, and
 * gl_tex_w/gl_tex_h must keep describing it: zeroing them here left
 * ati_r350_gl_target() answering "big enough" without ever reporting a
 * grow, so req.surf_w/surf_h stayed 0 and every offloaded draw after a
 * warm reboot ran under glViewport(0, 0, 0, 0) -- the flush then wrote
 * the untouched seed back and the framebuffer never changed.
 */
void ati_r350_gl_reset(ATIR350State *s)
{
    ati_r350_gl_release(s, R350_GLR_RESET);
    r300_gl_texdrop(s);
}

/*
 * A READER of this range: it must see VRAM as the engine left it, so a
 * resident target overlapping it is flushed and given back. Decoded
 * textures are NOT dropped -- reading a texture is what the cache
 * exists for, and dropping it here made every draw invalidate the very
 * entry it had just filled (measured live: 0 hits in 3732 decodes over
 * a Chess session).
 */
void ati_r350_gl_sync(ATIR350State *s, uint32_t off, uint32_t len)
{
    uint32_t lo, hi;

    if (!ati_r350_gl_mine(s) ||
        !r300_gl_span(s, &lo, &hi) || off + len <= lo || off >= hi) {
        return;
    }
    ati_r350_gl_release(s, R350_GLR_READ);
}

/* a WRITER of this range: whatever was decoded from it is now stale */
void ati_r350_gl_wrote(ATIR350State *s, uint32_t off, uint32_t len)
{
    unsigned k;

    qemu_rec_mutex_lock(&s->gl_tex_lock);
    for (k = 0; s->gl_texlife != R350_TEXLIFE_NEVER &&
                k < R300_GL_TEXCACHE; k++) {
        if (s->gl_tex[k].live && off < s->gl_tex[k].off + s->gl_tex[k].len &&
            off + len > s->gl_tex[k].off) {
            s->gl_tex_wrote++;
            s->gl_tex[k].live = false;
            s->gl_tex[k].up = false;
        }
    }
    qemu_rec_mutex_unlock(&s->gl_tex_lock);
    ati_r350_gl_sync(s, off, len);
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
        /* a different target entirely */
        ati_r350_gl_release(s, R350_GLR_TARGET);
    }
    if (x1 > s->gl_tex_w || y1 > s->gl_tex_h) {
        r300_gl_flush(s);               /* the grow below discards it */
    }
    if (!ati_r350_gl_target(s->gl_ctx, x1, y1, &lost)) {
        ati_r350_gl_release(s, R350_GLR_BACKEND);
        return false;
    }
    if (lost) {
        s->gl_tex_w = MAX(x1, s->gl_tex_w);
        s->gl_tex_h = MAX(y1, s->gl_tex_h);
        r300_gl_discard(s);
    }
    for (i = 0; s->gl_texlife != R350_TEXLIFE_NEVER &&
                i < R300_GL_TEXCACHE; i++) {
        /* rendering into a range some cached texture came from */
        if (s->gl_tex[i].live &&
            d->dst_off + (uint32_t)y0 * d->dst_pitch <
            s->gl_tex[i].off + s->gl_tex[i].len &&
            d->dst_off + (uint32_t)y1 * d->dst_pitch > s->gl_tex[i].off) {
            s->gl_tex_over++;
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
                ati_r350_gl_release(s, R350_GLR_BACKEND);
                return false;
            }
            s->gl_seed_px += (uint64_t)sw * sh;
        }
    }
    s->gl_vx0 = ux0; s->gl_vy0 = uy0;
    s->gl_vx1 = ux1; s->gl_vy1 = uy1;
    return true;
}

/*
 * What r300_gl_prims() did with a draw, and what the caller therefore
 * owes the resident render target.
 *
 * R300_GL_SOFTWARE and R300_GL_NOWORK are BOTH fallbacks and both
 * counted as such -- they differ only in whether VRAM is about to
 * change. A fallback that is going to run the software rasterizer over
 * the destination must have the target back first, because the
 * rasterizer writes VRAM the GPU copy shadows; a fallback that has been
 * PROVED to paint no pixels writes nothing, reads nothing, and needs no
 * coherency action at all. Tearing the target off the GPU for one of
 * those is pure loss: measured on Xbench's Spinning Squares, ~83% of
 * ~4400 quads a frame have an empty post-scissor rectangle, and
 * releasing for each of them cost 46,116 synchronous glReadPixels
 * round trips and 1.66 G px each way in a single run.
 */
typedef enum R300GlOutcome {
    R300_GL_SOFTWARE,   /* fall back; the rasterizer will write VRAM */
    R300_GL_DRAWN,      /* the backend rendered it */
    R300_GL_NOWORK      /* provably zero pixels; neither path need run */
} R300GlOutcome;

static R300GlOutcome r300_gl_fallback(ATIR350State *s, ATIR350GlFallback why,
                                      unsigned prim, unsigned nvtx)
{
    s->gl_fb[why][prim & (R350_GAP_SLOTS - 1)]++;
    trace_ati_r350_3d_gl_fallback(ati_r350_gl_fb_name(why), prim, nvtx);
    return R300_GL_SOFTWARE;
}

/*
 * The same fallback, counted identically -- the cause tally and the
 * trace stay exactly what they were, so nothing that reads them has to
 * learn a new name -- plus the sub-count `gl-stats` reports, which is
 * the only place the changed BEHAVIOUR is visible. Only a caller
 * holding a proof of emptiness may use this.
 */
static R300GlOutcome r300_gl_nowork(ATIR350State *s, ATIR350GlFallback why,
                                    unsigned prim, unsigned nvtx)
{
    r300_gl_fallback(s, why, prim, nvtx);
    s->gl_nowork++;
    return R300_GL_NOWORK;
}

/*
 * The triangles r300_raster_prims() assembles, as an index list. Kept
 * beside that switch and in the same order on purpose: if the two ever
 * disagree the offload draws different geometry from the oracle, which
 * is the one divergence gl=verify could not attribute.
 *
 * Point lists and lines are deliberately absent. A point SPRITE is a
 * whole per-vertex quad expansion driven by registers the vertex buffer
 * does not carry, so it is synthesised by r300_gl_point_list() beside
 * the rectangle list rather than indexed here. Lines are expanded
 * across their direction and still fall back, and are counted.
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
 * A point SPRITE is a whole quad per vertex, expanded from registers the
 * vertex buffer knows nothing about: RE_POINTSIZE gives its size in
 * sixths of a pixel and GA_POINT_S0/S1/T0/T1 the texture window, with
 * T0 pairing with the sprite's BOTTOM edge and T1 with its top. The
 * expansion below is r300_raster_prims()'s case 1, vertex for vertex,
 * and it has to stay that way: WindowServer composites whole layers as
 * single sprites, so getting it wrong is getting the desktop wrong.
 *
 * Worth offloading rather than leaving to fall back, because a fallback
 * costs far more than the draw. Flurry.saver issues one sprite per
 * frame between two draws the backend does render, and handing the
 * render target back for it flushed and re-seeded the whole thing twice
 * a frame -- 5369 of the 6839 flushes in one measured session.
 */
static unsigned r300_gl_point_list(ATIR350State *s, R300DrawState *d,
                                   const R300Vtx *vb, unsigned nvtx,
                                   R300Vtx *out, unsigned max)
{
    uint32_t psize = s->regs[R300_RE_POINTSIZE >> 2];
    float sx = ((psize >> 16) & 0xffff) / 6.0f;
    float sy = (psize & 0xffff) / 6.0f;
    float s0 = r300_f32(s->regs[R300_GA_POINT_S0 >> 2]) * d->tex[0].w;
    float s1 = r300_f32(s->regs[R300_GA_POINT_S1 >> 2]) * d->tex[0].w;
    float t1 = r300_f32(s->regs[R300_GA_POINT_T0 >> 2]) * d->tex[0].h;
    float t0 = r300_f32(s->regs[R300_GA_POINT_T1 >> 2]) * d->tex[0].h;
    unsigned n = 0, i, u;

    /*
     * The sprite path re-derives this from TX_ENABLE alone -- a trap the
     * ledger names, because the setup flag is not the sprite's state.
     */
    d->textured = s->regs[R300_TX_ENABLE >> 2] & 1;
    for (u = 0; u < R300_TEX_UNITS; u++) {
        d->tex[u].en = d->textured &&
                       (s->regs[R300_TX_ENABLE >> 2] & (1u << u));
    }
    if (!(sx > 0.0f) || !(sy > 0.0f)) {
        return 0;
    }
    for (i = 0; i < nvtx && n + 6 <= max; i++) {
        R300Vtx q[4];
        int c;

        for (c = 0; c < 4; c++) {
            q[c] = vb[i];
            if (r300_draw_fetches(d)) {
                q[c].r = q[c].g = q[c].b = q[c].a = 1.0f;
            }
        }
        q[0].x = vb[i].x - sx / 2; q[0].y = vb[i].y - sy / 2;
        q[0].tc[0][0] = s0; q[0].tc[0][1] = t0;
        q[1].x = vb[i].x + sx / 2; q[1].y = q[0].y;
        q[1].tc[0][0] = s1; q[1].tc[0][1] = t0;
        q[2].x = q[1].x; q[2].y = vb[i].y + sy / 2;
        q[2].tc[0][0] = s1; q[2].tc[0][1] = t1;
        q[3].x = q[0].x; q[3].y = q[2].y;
        q[3].tc[0][0] = s0; q[3].tc[0][1] = t1;
        out[n++] = q[0]; out[n++] = q[1]; out[n++] = q[2];
        out[n++] = q[0]; out[n++] = q[2]; out[n++] = q[3];
    }
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
        unsigned k;

        v3.x = vb[i + 1].x + vb[i + 2].x - vb[i].x;
        v3.y = vb[i + 1].y + vb[i + 2].y - vb[i].y;
        for (k = 0; k < R300_TEXCOORDS; k++) {
            v3.tc[k][0] = vb[i + 1].tc[k][0] + vb[i + 2].tc[k][0] -
                          vb[i].tc[k][0];
            v3.tc[k][1] = vb[i + 1].tc[k][1] + vb[i + 2].tc[k][1] -
                          vb[i].tc[k][1];
        }
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

/*
 * Two triangles with a common edge whose third vertices lie strictly on
 * opposite sides of it are disjoint. The projection test cannot see this
 * once the edge is not axis-aligned: the two endpoints' projections onto
 * the edge's own normal differ by a rounding, so the halves of a rotated
 * quad read as overlapping. Ties and near-collinear thirds are left to
 * the projection test.
 */
static bool r300_tris_split_by_edge(const R300Vtx * const a[3],
                                    const R300Vtx * const b[3])
{
    unsigned i, j;

    for (i = 0; i < 3; i++) {
        const R300Vtx *p = a[i], *q = a[(i + 1) % 3], *ra = a[(i + 2) % 3];

        for (j = 0; j < 3; j++) {
            const R300Vtx *u = b[j], *v = b[(j + 1) % 3];
            const R300Vtx *rb = b[(j + 2) % 3];
            float ex, ey, oa, ob, la, lb, len;

            if (!((u->x == p->x && u->y == p->y &&
                   v->x == q->x && v->y == q->y) ||
                  (u->x == q->x && u->y == q->y &&
                   v->x == p->x && v->y == p->y))) {
                continue;
            }
            ex = q->x - p->x;
            ey = q->y - p->y;
            oa = ex * (ra->y - p->y) - ey * (ra->x - p->x);
            ob = ex * (rb->y - p->y) - ey * (rb->x - p->x);
            len = fabsf(ex) + fabsf(ey);
            la = len * (fabsf(ra->x - p->x) + fabsf(ra->y - p->y));
            lb = len * (fabsf(rb->x - p->x) + fabsf(rb->y - p->y));
            if (fabsf(oa) <= la * 0.0001f || fabsf(ob) <= lb * 0.0001f) {
                return false;
            }
            return (oa > 0.0f) != (ob > 0.0f);
        }
    }
    return false;
}

static bool r300_tris_overlap(const R300Vtx * const a[3],
                              const R300Vtx * const b[3])
{
    unsigned i;

    if (r300_tris_split_by_edge(a, b)) {
        return false;
    }
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
 * ...and the draw that needs none of it, because GL's own blender can
 * keep the device's order for free.
 *
 * THE PREDICATE, stated exactly: for colour and for alpha alike, the
 * combine function is ADD, the DESTINATION factor is ONE, and the
 * SOURCE factor does not read the destination. The blend is then
 *
 *      dst' = clamp(dst + f(src))
 *
 * and two things follow that nothing else here enjoys. The destination
 * term is the destination unchanged, so a per-primitive quantisation
 * needs no read: the device's byte is already an integer and the
 * fragment shader can hand GL floor(255*f(src)) to add to it, which is
 * the device's truncating pack, once per primitive, in primitive order.
 * And no factor looks at the destination, so no snapshot of it is
 * needed -- the seed and the between-pass copy both disappear.
 *
 * That is what makes the ordered partition unnecessary for this family
 * rather than merely cheaper. Flurry.saver's ribbons are the family:
 * every one of the 1899 draws M3 measured as `self-overlapping blend
 * prim 13` fallbacks blends additively, each a quad list of up to 456
 * self-crossing triangles that the partition either spread over many
 * passes or refused outright. The SAT machinery stays for everything
 * whose destination term is not the destination -- an ordinary
 * SRC_ALPHA/ONE_MINUS_SRC_ALPHA composite cannot use this, because
 * there the destination is scaled before it is added and the scaled
 * value is not on the byte grid.
 *
 * IT IS NOT EXACT, AND THAT IS WHY IT IS BEHIND gl=fast RATHER THAN ON
 * BY DEFAULT. Measured on a Flurry corpus of 94 replayable draws
 * (doc/radeon9800/flurry-glcap-2026-08-26.bin.gz) against the software
 * rasterizer, and live with gl=verify:
 *
 *   ordered passes, limit raised so nothing is refused
 *       INTERIOR 99.9314% at delta 0, EDGE 0.0001%, ribbon max delta 4
 *   this path
 *       INTERIOR 99.3857% at delta 0, EDGE 1.1854%, ribbon max delta 45
 *   live gl=verify over a Flurry session
 *       VALUE 99.9628% -> 98.8267% at delta 0, COVER 0.1022%
 *
 * What it buys is 22.93 fps against 13.19, with the self-overlap
 * fallback count going 665 -> 0 (1899 -> 0 against the milestone's own
 * pass limit) and the vCPU at 71.5% of a core. The residue is a
 * per-primitive rounding decomposition that accumulates over a pixel's
 * overlap depth: the device computes trunc((t + D/255)*255) and this
 * path computes D + trunc(255*t), and the two disagree wherever 255*t
 * lands within a few ULPs of an integer -- which for a ribbon crossing
 * itself thirty times is thirty chances to differ by one. On screen the
 * two are indistinguishable; in numbers they are not, so the choice is
 * the user's and `on` stays exact.
 *
 * Codes are r300_blend_f()'s own; both encodings of each factor are
 * listed because the register uses both.
 */
static bool r300_blend_reads_dst(unsigned code)
{
    switch (code) {
    case 9: case 36:                    /* DST_COLOR */
    case 10: case 37:
    case 7: case 40:                    /* DST_ALPHA */
    case 8: case 41:
    case 11: case 42:                   /* SRC_ALPHA_SATURATE: min(sa,1-da) */
        return true;
    default:
        return false;
    }
}

static bool r300_gl_addblend(const R300DrawState *d)
{
    /* r300_blend_comb(): everything outside 2..7 is ADD */
    if ((d->comb_fcn >= 2 && d->comb_fcn <= 7) ||
        (d->a_comb_fcn >= 2 && d->a_comb_fcn <= 7)) {
        return false;
    }
    if ((d->dst_factor != 2 && d->dst_factor != 33) ||
        (d->a_dst_factor != 2 && d->a_dst_factor != 33)) {
        return false;
    }
    return !r300_blend_reads_dst(d->src_factor) &&
           !r300_blend_reads_dst(d->a_src_factor);
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

/* r300_texel_chan() scaled back to a byte, which is exactly the byte */
static inline uint8_t r300_texel_byte(const R300TexUnit *u, uint32_t texel,
                                      unsigned ch)
{
    unsigned sel = u->sel[ch];

    if (sel == R300_TX_SEL_ONE) {
        return 255;
    }
    if (sel > R300_TX_SEL_W) {
        return 0;
    }
    return (texel >> (sel * 8)) & 0xff;
}

/*
 * The 32bpp case of the loop below without the per-texel sampler: the
 * coordinates are in range, so neither wrap nor clamp applies, and a row
 * outside VRAM is fetched with one translation per page.
 */
static bool r300_gl_decode_tex32(ATIR350State *s, const R300DrawState *d,
                                 unsigned unit, uint8_t *rgba)
{
    const R300TexUnit *u = &d->tex[unit];
    g_autofree uint32_t *row = NULL;
    int tx, ty;

    if (u->bpp != 32 || u->w <= 0 || u->h <= 0) {
        return false;
    }
    row = g_new(uint32_t, u->w);
    for (ty = 0; ty < u->h; ty++) {
        uint32_t a = u->off + (uint32_t)ty * u->pitch;
        uint32_t end = a + (uint32_t)(u->w - 1) * 4;
        uint32_t off;
        uint8_t *p = rgba + (size_t)ty * u->w * 4;

        if (!ati_r350_mc_to_vram(s, a, &off) &&
            !ati_r350_mc_to_vram(s, end, &off) && end >= a) {
            ati_r350_mc_read_block(s, a, row, u->w);
            for (tx = 0; u->lanes && tx < u->w; tx++) {
                row[tx] = r300_lane_xor32(row[tx], u->lanes);
            }
        } else {
            for (tx = 0; tx < u->w; tx++) {
                uint32_t ta = a + (uint32_t)tx * 4;

                if (!ati_r350_mc_to_vram(s, ta, &off)) {
                    row[tx] = r300_lane_xor32(ati_r350_mc_read32(s, ta),
                                              u->lanes);
                } else {
                    row[tx] = off + 4 > ATI_R350_VRAM_SIZE
                              ? 0 : r300_ld32(s, d, off);
                }
            }
        }
        for (tx = 0; tx < u->w; tx++, p += 4) {
            p[0] = r300_texel_byte(u, row[tx], 1);
            p[1] = r300_texel_byte(u, row[tx], 2);
            p[2] = r300_texel_byte(u, row[tx], 3);
            p[3] = r300_texel_byte(u, row[tx], 0);
        }
    }
    return true;
}

static void r300_gl_decode_tex(ATIR350State *s, const R300DrawState *d,
                               unsigned unit, uint8_t *rgba)
{
    const R300TexUnit *u = &d->tex[unit];
    uint8_t *p = rgba + (size_t)u->w * u->h * 4;
    unsigned l;
    int tx, ty;

    /* the levels after the first, one after another behind it */
    for (l = 1; l < u->nlev; l++) {
        for (ty = 0; ty < u->lh[l]; ty++) {
            for (tx = 0; tx < u->lw[l]; tx++, p += 4) {
                uint32_t texel = r300_tex_lvl_texel(s, d, unit, l, tx, ty);

                p[0] = r300_texel_byte(u, texel, 1);
                p[1] = r300_texel_byte(u, texel, 2);
                p[2] = r300_texel_byte(u, texel, 3);
                p[3] = r300_texel_byte(u, texel, 0);
            }
        }
    }
    if (r300_gl_decode_tex32(s, d, unit, rgba)) {
        return;
    }
    for (ty = 0; ty < u->h; ty++) {
        for (tx = 0; tx < u->w; tx++) {
            uint32_t texel = r300_sample_tex(s, d, unit, tx, ty);
            uint8_t *px = rgba + ((size_t)ty * u->w + tx) * 4;

            px[0] = (uint8_t)(r300_texel_chan(u, texel, 1) * 255.0f + 0.5f);
            px[1] = (uint8_t)(r300_texel_chan(u, texel, 2) * 255.0f + 0.5f);
            px[2] = (uint8_t)(r300_texel_chan(u, texel, 3) * 255.0f + 0.5f);
            px[3] = (uint8_t)(r300_texel_chan(u, texel, 0) * 255.0f + 0.5f);
        }
    }
}

/* a unit's filter state in the backend's terms; see ati_r350_gl.h */
static void r300_gl_filt(const R300TexUnit *u, R350GlReq *r, unsigned i)
{
    int *f = r->filt[i];

    f[0] = u->filt;
    f[1] = u->need_lod;
    f[2] = u->mag;
    f[3] = u->min;
    f[4] = u->mip;
    f[5] = u->aniso_l2;
    f[6] = u->first;
    f[7] = u->last;
    f[8] = u->bias;
    f[9] = u->wl2;
    f[10] = u->hl2;
    r->levels[i] = u->nlev;
    r->border[i][0] = r300_texel_byte(u, u->border, 1);
    r->border[i][1] = r300_texel_byte(u, u->border, 2);
    r->border[i][2] = r300_texel_byte(u, u->border, 3);
    r->border[i][3] = r300_texel_byte(u, u->border, 0);
}

/* everything the decode above depends on, and nothing else */
static bool r300_gl_tex_same(const ATIR350State *s, unsigned k,
                             const R300TexUnit *u, uint32_t off,
                             uint32_t len, unsigned xr)
{
    return s->gl_tex[k].live &&
           s->gl_tex[k].off == off && s->gl_tex[k].len == len &&
           s->gl_tex[k].pitch == u->pitch &&
           s->gl_tex[k].bpp == u->bpp &&
           s->gl_tex[k].code == u->code &&
           s->gl_tex[k].yuv == u->yuv &&
           s->gl_tex[k].w == u->w && s->gl_tex[k].h == u->h &&
           s->gl_tex[k].xr == xr &&
           s->gl_tex[k].sel[0] == u->sel[0] &&
           s->gl_tex[k].sel[1] == u->sel[1] &&
           s->gl_tex[k].sel[2] == u->sel[2] &&
           s->gl_tex[k].sel[3] == u->sel[3] &&
           s->gl_tex[k].nlev == u->nlev &&
           s->gl_tex[k].lay == u->loff[u->nlev - 1] - u->off;
}

/*
 * The decoded texture for this draw: from the cache when the same bytes
 * were decoded the same way inside this burst, and decoded into the
 * least recently used entry otherwise. A texture that does not resolve
 * to a uniformly swapped range of VRAM is decoded into the scratch
 * buffer and not cached -- there would be no range to invalidate it on.
 */
static const uint8_t *r300_gl_texture(ATIR350State *s, const R300DrawState *d,
                                      unsigned unit, unsigned *slot,
                                      int *fresh)
{
    const R300TexUnit *u = &d->tex[unit];
    uint32_t off, len;
    unsigned k, victim = 0, xr = 0;
    size_t need = u->ltexels * 4;

    *slot = R350_GL_TEXSLOTS;           /* the scratch: uploaded every time */
    *fresh = 1;

    len = u->chain;
    /*
     * Not cacheable, so decoded into the scratch: too big to keep, no
     * range to invalidate on, or -- new with the dirty guard -- a range
     * spanning more pages than a baseline has room to describe.
     */
    if ((size_t)u->w * u->h > R300_GL_TEXCACHE_MAX ||
        !u->pitch || !len ||
        !ati_r350_mc_to_vram(s, u->off, &off) ||
        (uint64_t)off + len > ATI_R350_VRAM_SIZE ||
        r300_gl_pages(s, off, len) > R300_GL_DIRTY_PAGES ||
        !r300_cap_xor(s, off, len, &xr)) {
        s->gl_tex_miss++;
        r300_gl_decode_tex(s, d, unit, s->gl_texbuf);
        return s->gl_texbuf;
    }
    for (k = 0; k < R300_GL_TEXCACHE; k++) {
        /*
         * The dirty guard is applied to the entry this draw is about to
         * USE, and only to that one -- an entry nobody matches cannot
         * be read stale, and walking every entry's pages on every draw
         * would cost more than the decode it saves. A stale match is
         * killed here and falls through to the decode below, with its
         * now-free slot the natural victim.
         */
        if (r300_gl_tex_same(s, k, u, off, len, xr) &&
            !r300_gl_tex_current(s, k)) {
            s->gl_tex[k].live = false;
            s->gl_tex[k].up = false;
            s->gl_tex_stale++;
        }
        if (r300_gl_tex_same(s, k, u, off, len, xr)) {
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
    r300_gl_decode_tex(s, d, unit, s->gl_tex[victim].rgba);
    s->gl_tex[victim].off = off;
    s->gl_tex[victim].len = len;
    s->gl_tex[victim].pitch = u->pitch;
    s->gl_tex[victim].bpp = u->bpp;
    s->gl_tex[victim].code = u->code;
    s->gl_tex[victim].yuv = u->yuv;
    s->gl_tex[victim].w = u->w;
    s->gl_tex[victim].h = u->h;
    s->gl_tex[victim].xr = xr;
    for (k = 0; k < 4; k++) {
        s->gl_tex[victim].sel[k] = u->sel[k];
    }
    s->gl_tex[victim].nlev = u->nlev;
    s->gl_tex[victim].lay = u->loff[u->nlev - 1] - u->off;
    s->gl_tex[victim].used = ++s->gl_tex_seq;
    s->gl_tex[victim].epoch = s->gl_epoch;
    s->gl_tex[victim].npg = r300_gl_pages(s, off, len);
    /*
     * ADMISSION: only a range whose every page reads clean can be
     * guarded, because a bit already set cannot afterwards be told from
     * one set later. An entry that fails here is used for this draw and
     * then dropped, and the next decode after a refresh admits it.
     */
    s->gl_tex_evict += s->gl_tex[victim].live;
    s->gl_tex[victim].live = s->gl_texlife != R350_TEXLIFE_DIRTY ||
                             !r300_gl_range_dirty(s, off,
                                                  s->gl_tex[victim].npg) ||
                             ati_r350_gl_admit(s, off, len);
    s->gl_tex_noadmit += !s->gl_tex[victim].live;
    s->gl_tex[victim].up = true;
    s->gl_tex_any |= s->gl_tex[victim].live;
    *slot = victim;
    *fresh = 1;
    return s->gl_tex[victim].rgba;
}

/*
 * One vertex of the expanded triangle list, in the backend's layout.
 * Only R350_GL_TEXCOORDS sets are carried, which is one: the caller
 * offloads a draw only when the translator could express its program,
 * and that shape reads coordinate set 0 and nothing else.
 */
static void r300_gl_vtx(float *v, const R300Vtx *me, const R300Vtx *t0,
                        const R300Vtx *t1, const R300Vtx *t2, float inv)
{
    const unsigned C = R350_GL_TEXCOORDS;
    unsigned k;

    v[0] = me->x; v[1] = me->y;
    v[2] = me->r; v[3] = me->g; v[4] = me->b; v[5] = me->a;
    for (k = 0; k < C; k++) {
        v[6 + 2 * k] = me->tc[k][0];
        v[7 + 2 * k] = me->tc[k][1];
    }
    v[6 + 2 * C] = t0->x;  v[7 + 2 * C] = t0->y;
    v[8 + 2 * C] = t1->x;  v[9 + 2 * C] = t1->y;
    v[10 + 2 * C] = t2->x; v[11 + 2 * C] = t2->y;
    v[12 + 2 * C] = t0->r; v[13 + 2 * C] = t0->g;
    v[14 + 2 * C] = t0->b; v[15 + 2 * C] = t0->a;
    v[16 + 2 * C] = t1->r; v[17 + 2 * C] = t1->g;
    v[18 + 2 * C] = t1->b; v[19 + 2 * C] = t1->a;
    v[20 + 2 * C] = t2->r; v[21 + 2 * C] = t2->g;
    v[22 + 2 * C] = t2->b; v[23 + 2 * C] = t2->a;
    /*
     * Every set of one corner together, so that each corner's whole
     * coordinate block is one vertex attribute in the shader.
     */
    for (k = 0; k < C; k++) {
        v[24 + 2 * C + 2 * k] = t0->tc[k][0];
        v[25 + 2 * C + 2 * k] = t0->tc[k][1];
        v[24 + 4 * C + 2 * k] = t1->tc[k][0];
        v[25 + 4 * C + 2 * k] = t1->tc[k][1];
        v[24 + 6 * C + 2 * k] = t2->tc[k][0];
        v[25 + 6 * C + 2 * k] = t2->tc[k][1];
    }
    v[24 + 8 * C] = inv;
    v[25 + 8 * C] = t0->r1; v[26 + 8 * C] = t0->g1;
    v[27 + 8 * C] = t0->b1; v[28 + 8 * C] = t0->a1;
    v[29 + 8 * C] = t1->r1; v[30 + 8 * C] = t1->g1;
    v[31 + 8 * C] = t1->b1; v[32 + 8 * C] = t1->a1;
    v[33 + 8 * C] = t2->r1; v[34 + 8 * C] = t2->g1;
    v[35 + 8 * C] = t2->b1; v[36 + 8 * C] = t2->a1;
    v[37 + 8 * C] = inv;
    v[38 + 8 * C] = t0->w; v[39 + 8 * C] = t1->w; v[40 + 8 * C] = t2->w;
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
 * Returns R300_GL_DRAWN when the backend rendered the draw and the
 * caller must not rasterize it again. In gl=verify the software
 * rasterizer HAS been run by the time that is returned -- its result is
 * what stays in VRAM, and the GL result is only compared against it.
 * R300_GL_NOWORK means the draw was refused AND proved to paint
 * nothing; anything else is R300_GL_SOFTWARE. See R300GlOutcome.
 */
static R300GlOutcome r300_gl_prims(ATIR350State *s, R300DrawState *d,
                                   const R300Vtx *vb, unsigned nvtx,
                                   unsigned prim)
{
    g_autofree R300Vtx *rect_vb = NULL;
    g_autofree unsigned *idx = NULL;
    const R300Vtx *gvb = vb;
    R350GlReq req = { 0 };
    const uint8_t *texbuf[R300_TEX_UNITS] = { NULL };
    unsigned texslot[R300_TEX_UNITS];
    int texfresh[R300_TEX_UNITS];
    unsigned ntri = 0, npass = 1, i, xr = 0;
    int x0, y0, x1, y1, w, h;
    size_t rect_sz, texels = 0;
    bool sc_empty = false;

    for (i = 0; i < R300_TEX_UNITS; i++) {
        texslot[i] = R350_GL_TEXSLOTS;
        texfresh[i] = 1;
    }
    if (d->resolve) {
        return r300_gl_fallback(s, R350_GLF_RESOLVE, prim, nvtx);
    }
    /*
     * A fragment program the translator refused. The software path runs
     * the same refusal through the interpreter and paints the
     * interpolated colour; sending this draw to a shader that computes
     * something else would be the one thing worse than either.
     */
    if (!d->fs_run || !s->us_glsl_ok) {
        return r300_gl_fallback(s, R350_GLF_FSPROG, prim, nvtx);
    }
    /* the GL target has no depth or stencil buffer: the Z buffer is ours */
    if (s->zb.z_en && (s->zb.s_en || s->zb.zfunc != 7)) {
        return r300_gl_fallback(s, R350_GLF_ZTEST, prim, nvtx);
    }
    /*
     * Assemble first: a primitive this path does not know is the
     * commonest fallback and the cheapest one to detect. `vb`/`nvtx`
     * stay the draw as the guest issued it -- only `gvb`/`idx` are the
     * expansion GL is handed.
     */
    if (prim == 8 || prim == 1) {
        rect_vb = g_new(R300Vtx, (size_t)nvtx * 6 + 6);
        ntri = prim == 8
               ? r300_gl_rect_list(vb, nvtx, rect_vb, nvtx * 6 + 6)
               : r300_gl_point_list(s, d, vb, nvtx, rect_vb, nvtx * 6 + 6);
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
    if (prim != 8 && prim != 1 &&
        (s->regs[R300_RE_CULL_CNTL >> 2] & 3)) {
        /* the triangles r300_raster_prims() would cull, same test */
        unsigned cull = s->regs[R300_RE_CULL_CNTL >> 2] & 7, j, k = 0;

        for (j = 0; j < ntri; j++) {
            const unsigned *t = &idx[j * 3];
            float area = r300_edge(&gvb[t[0]], &gvb[t[1]],
                                   gvb[t[2]].x, gvb[t[2]].y);
            unsigned c = cull | (prim == 6 && (t[2] & 1) ? 16 : 0);

            if (area == 0.0f ||
                (c & (r300_back_face(c, area) ? 2 : 1))) {
                continue;
            }
            memmove(&idx[k * 3], t, 3 * sizeof(*t));
            k++;
        }
        ntri = k;
        if (!ntri) {
            return R300_GL_NOWORK;
        }
    }
    if (!r300_gl_clip(d, &req.sx0, &req.sy0, &req.sx1, &req.sy1)) {
        return r300_gl_fallback(s, R350_GLF_CLIPRULE, prim, nvtx);
    }
    if (d->blend && d->blend_read && ntri > 1) {
        npass = ntri > R300_GL_TRI_MAX
                ? 0 : r300_gl_passes(s, gvb, idx, ntri);
        /*
         * The partition is asked FIRST, and a draw it puts in a single
         * pass keeps the shader blend, which reproduces the device bit
         * for bit. Only a draw that needed several passes -- or that the
         * partition refused outright -- is handed to GL's own blender,
         * and only under gl=fast, because the hand-off is a measured
         * trade of accuracy for frame rate rather than a free win.
         */
        if (s->gl_fast && npass != 1 && r300_gl_addblend(d)) {
            req.add_blend = 1;
            npass = 1;
            s->gl_addblend++;
        } else if (!npass) {
            return r300_gl_fallback(s, R350_GLF_SELFBLEND, prim, nvtx);
        } else if (npass > 1) {
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
    if (d->cb_bpp != 4 || d->cb_host) {
        return r300_gl_fallback(s, R350_GLF_CBFMT, prim, nvtx);
    }
    if ((d->dst_off | d->dst_pitch) & 3) {
        return r300_gl_fallback(s, R350_GLF_ALIGN, prim, nvtx);
    }
    if (!r300_cap_rect(s, d, vb, nvtx, prim, &x0, &y0, &x1, &y1, &sc_empty)) {
        /*
         * Empty after the scissor, off-screen, or not a finite
         * rectangle at all.
         *
         * When it is the FIRST of those -- `sc_empty`, the case
         * r300_cap_rect() carries a proof for -- the software path
         * paints nothing either, so there is nothing for the resident
         * render target to be coherent WITH: no release, and no
         * rasterizer call to make one necessary. This is the one
         * fallback that costs nothing, and now it also costs nothing to
         * the target.
         *
         * The other two keep the release. "Off-screen" and "not finite"
         * arrive here together, and only the clipped-to-empty half of
         * that pair is provable: a coordinate outside +-100000 is a
         * draw this helper declined to reason about, not one that
         * misses the screen, and the rasterizer will happily scan it.
         */
        if (sc_empty) {
            return r300_gl_nowork(s, R350_GLF_RECT, prim, nvtx);
        }
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
    for (i = 0; i < R300_TEX_UNITS; i++) {
        size_t n;

        if (!d->tex[i].en) {
            continue;
        }
        n = (size_t)d->tex[i].w * d->tex[i].h;
        if (!n || n > R300_GL_TEX_MAX) {
            return r300_gl_fallback(s, R350_GLF_TEXTURE, prim, nvtx);
        }
        n = d->tex[i].ltexels;
        texels = MAX(texels, n);
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
    for (i = 0; i < R300_TEX_UNITS; i++) {
        if (d->tex[i].en) {
            texbuf[i] = r300_gl_texture(s, d, i, &texslot[i], &texfresh[i]);
        }
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
    for (i = 0; i < R300_TEX_UNITS; i++) {
        req.tex[i] = d->tex[i].en ? texbuf[i] : NULL;
        req.tex_slot[i] = texslot[i];
        req.tex_fresh[i] = texfresh[i];
        req.tex_w[i] = d->tex[i].w;
        req.tex_h[i] = d->tex[i].h;
        req.clamp_s[i] = d->tex[i].clamp_s;
        req.clamp_t[i] = d->tex[i].clamp_t;
        r300_gl_filt(&d->tex[i], &req, i);
        req.textured |= d->tex[i].en ? (1u << i) : 0;
    }
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
    req.us_glsl = s->us_glsl;
    req.us_key = s->us_glsl_key;
    req.us_konst = s->us_konst_flat;

    if (s->gl_mode == R350_GL_VERIFY) {
        r300_gl_rd_rect(d, xr, x0, y0, w, h, s->gl_before);
    }
    if (!ati_r350_gl_draw(s->gl_ctx, &req)) {
        ati_r350_gl_release(s, R350_GLR_BACKEND);
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
        return R300_GL_DRAWN;
    }
    /* the GPU now holds bytes VRAM does not, over this rectangle */
    if (s->gl_dx1 <= s->gl_dx0) {
        s->gl_dx0 = x0; s->gl_dy0 = y0; s->gl_dx1 = x1; s->gl_dy1 = y1;
    } else {
        s->gl_dx0 = MIN(s->gl_dx0, x0); s->gl_dy0 = MIN(s->gl_dy0, y0);
        s->gl_dx1 = MAX(s->gl_dx1, x1); s->gl_dy1 = MAX(s->gl_dy1, y1);
    }
    return R300_GL_DRAWN;
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
    float s0 = vb[0].tc[0][0], t0 = vb[0].tc[0][1], s1 = s0, t1 = t0;
    unsigned i;

    for (i = 1; i < nvtx; i++) {
        x0 = MIN(x0, vb[i].x); x1 = MAX(x1, vb[i].x);
        y0 = MIN(y0, vb[i].y); y1 = MAX(y1, vb[i].y);
        s0 = MIN(s0, vb[i].tc[0][0]); s1 = MAX(s1, vb[i].tc[0][0]);
        t0 = MIN(t0, vb[i].tc[0][1]); t1 = MAX(t1, vb[i].tc[0][1]);
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

/* rasterize into a GART colour buffer through a staged host copy */
static void r300_raster_gart(ATIR350State *s, R300DrawState *d,
                             const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    g_autofree uint8_t *buf = g_malloc(d->cb_size);
    uint32_t i;

    for (i = 0; i + 4 <= d->cb_size; i += 4) {
        stl_le_p(buf + i, ati_r350_mc_read32(s, d->cb_card + i));
    }
    d->cb = buf;
    r300_raster_prims(s, d, vb, nvtx, prim);
    for (i = 0; i + 4 <= d->cb_size; i += 4) {
        ati_r350_mc_write32(s, d->cb_card + i, ldl_le_p(buf + i));
    }
    d->cb = d->vram;
}

static void r300_run_prims_locked(ATIR350State *s, R300DrawState *d,
                                  const R300Vtx *vb, unsigned nvtx,
                                  unsigned prim);

/*
 * The decoded-texture cache and the dirty bits it claims are shared with
 * the display's refresh, which runs beside the command processor thread.
 */
static void r300_run_prims(ATIR350State *s, R300DrawState *d,
                           const R300Vtx *vb, unsigned nvtx, unsigned prim)
{
    qemu_rec_mutex_lock(&s->gl_tex_lock);
    r300_run_prims_locked(s, d, vb, nvtx, prim);
    qemu_rec_mutex_unlock(&s->gl_tex_lock);
}

static void r300_run_prims_locked(ATIR350State *s, R300DrawState *d,
                                  const R300Vtx *vb, unsigned nvtx,
                                  unsigned prim)
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
        unsigned u;

        for (u = 0; u < R300_TEX_UNITS; u++) {
            const R300TexUnit *t = &d->tex[u];
            uint32_t toff;

            if (t->en && t->pitch && t->h > 0 &&
                ati_r350_mc_to_vram(s, t->off, &toff)) {
                ati_r350_gl_sync(s, toff, (uint32_t)t->h * t->pitch);
            }
        }
        if (d->resolve) {
            ati_r350_gl_release(s, R350_GLR_FALLBACK);
        }
    }
    if (s->cap_fp && s->cap_arm && nvtx) {
        ati_r350_gl_release(s, R350_GLR_FALLBACK);
        r300_cap_draw(s, d, vb, nvtx, prim);
        return;
    }
    if (s->gl_ctx && nvtx && d->wmask) {
        R300GlOutcome o = r300_gl_prims(s, d, vb, nvtx, prim);

        if (o == R300_GL_DRAWN) {
            /*
             * The backend rendered the colour. The Z buffer is the
             * CPU's to read, so the depth still goes through the
             * software path, shading nothing.
             */
            if (s->zb.z_en && s->zb.z_wr) {
                d->wmask = 0;
                r300_raster_prims(s, d, vb, nvtx, prim);
            }
            return;
        }
        if (o == R300_GL_NOWORK) {
            /*
             * A fallback with a proof that it paints no pixels. It
             * writes no VRAM, so the rules above ask nothing of it: the
             * resident target stays exactly as correct as it was, and
             * the rasterizer is not called because it would walk an
             * empty scan. The one rule this must not break is that the
             * proof is a proof -- see r300_cap_rect()'s `empty`.
             */
            return;
        }
    }
    /* the software rasterizer writes VRAM the GPU copy shadows */
    ati_r350_gl_release(s, R350_GLR_FALLBACK);
    if (d->cb_host) {
        r300_raster_gart(s, d, vb, nvtx, prim);
        return;
    }
    r300_raster_prims(s, d, vb, nvtx, prim);
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
    R300VtxFmt fmt = { 0 };
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
                           d.textured, d.blend, d.tex[0].off);

    /*
     * Inline vertices have no array boundaries, so their dwords are
     * taken four at a time in submission order -- a guess, and passed as
     * one -- and then handed to the stream routing, which says which
     * input register each of them is and replaces the guess outright
     * wherever VAP_PROG_STREAM_CNTL describes a vertex of exactly this
     * size.
     */
    for (i = 0; i < ARRAY_SIZE(d.attr_size) && i * 4 < vsize; i++) {
        d.attr_size[i] = MIN(vsize - i * 4, 4u);
    }
    d.attr_count = i;
    r300_stream_route(s, d.attr_size, &d.attr_count, vsize, &fmt);

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);
        R300TexSrc ts[R300_TEXCOORDS];

        r300_texcoord_src(&d, vsize, vsize, ts);
        for (i = 0; i < nvtx; i++) {
            const uint32_t *vd = &dw[1 + i * vsize];
            float clip[4];

            r300_load_vtx(&d, &fmt, vd, vsize, vsize, &vb[i]);
            r300_attr_texcoord(&d, &fmt, vd, ts, &vb[i]);
            if (i == 0 && d.textured) {
                r300_trace_texcoord(&d, &fmt, vd, &vb[i]);
            }
            if (d.vs_run && r300_vs_vtx(s, &d, &fmt, vd, &vb[i], clip)) {
                r300_xform_vtx(s, &d, &vb[i], clip);
            } else {
                r300_xform_vtx(s, &d, &vb[i], NULL);
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
static void r300_draw_aos(ATIR350State *s, uint32_t vf, const uint16_t *idx)
{
    unsigned prim = vf & 0xf;
    unsigned nvtx = (vf >> 16) & 0xffff;
    unsigned nfetch = nvtx;
    unsigned narr = s->regs[R300_VAP_VTX_AOS_CNT >> 2] &
                    R300_VAP_VTX_NUM_ARRAYS_MASK;
    uint32_t addr[R300_AOS_MAX];
    unsigned size[R300_AOS_MAX], stride[R300_AOS_MAX];
    unsigned vsize = 0;
    unsigned swap = s->regs[R300_VAP_CNTL_STATUS >> 2] & R300_VAP_VC_SWAP;
    R300DrawState d;
    R300VtxFmt fmt = { 0 };
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

    /* NUM_VERTICES is 16 bits; the vertices are fetched to the heap */
    if (idx) {
        nfetch = 0;
        for (i = 0; i < nvtx; i++) {
            nfetch = MAX(nfetch, idx[i] + 1u);
        }
    }
    if (!nvtx || !vsize || vsize > R300_VTX_DWORDS_MAX) {
        trace_ati_r350_3d_skip(vf, vsize, nvtx);
        return;
    }
    if (!r300_setup_draw(s, &d, vsize)) {
        trace_ati_r350_3d_skip(vf, vsize, s->regs[R300_RB3D_COLOROFFSET0 >> 2]);
        return;
    }

    trace_ati_r350_3d_draw(prim, nvtx, vsize, d.dst_off, d.dst_pitch,
                           d.textured, d.blend, d.tex[0].off);

    /* the bound arrays in order, then the stream routing over them */
    d.attr_count = 0;
    for (a = 0; a < narr; a++) {
        d.attr_size[a] = size[a];
        if (size[a]) {
            d.attr_count = a + 1;
        }
    }
    /*
     * Zero, not `vsize`: these sizes are the bound arrays themselves,
     * and element i is fetched from array i, so registers that describe
     * a different split of the same dwords are stale and not a better
     * answer. (The whole packed-colour family reaches the fetch through
     * here, and reaches it with the arrays and the registers agreeing.)
     */
    r300_stream_route(s, d.attr_size, &d.attr_count, 0, &fmt);

    {
        g_autofree R300Vtx *vb = g_new(R300Vtx, nvtx);
        g_autofree uint32_t *pre = NULL;
        uint32_t *arr[R300_AOS_MAX] = { NULL };
        size_t span[R300_AOS_MAX], total = 0;
        R300TexSrc ts[R300_TEXCOORDS];
        uint32_t dw[R300_VTX_DWORDS_MAX];

        /*
         * Each array outside VRAM is fetched whole before the walk; VRAM
         * reads are coherency points and stay where they were.
         */
        for (a = 0; a < narr; a++) {
            uint32_t off;

            span[a] = size[a] ? (size_t)(nfetch - 1) * stride[a] + size[a]
                              : 0;
            if (span[a] && !ati_r350_mc_to_vram(s, addr[a], &off)) {
                total += span[a];
            } else {
                span[a] = 0;
            }
        }
        if (total) {
            uint32_t *p;

            pre = p = g_new(uint32_t, total);
            for (a = 0; a < narr; a++) {
                if (span[a]) {
                    arr[a] = p;
                    ati_r350_mc_read_block(s, addr[a], p, span[a]);
                    p += span[a];
                }
            }
        }
        r300_texcoord_src(&d, vsize, size[0], ts);
        for (i = 0; i < nvtx; i++) {
            unsigned n = 0;
            unsigned vi = idx ? idx[i] : i;

            for (a = 0; a < narr; a++) {
                unsigned base = n;

                for (c = 0; c < size[a] && n < R300_VTX_DWORDS_MAX; c++) {
                    uint32_t card = addr[a] + (vi * stride[a] + c) * 4;
                    uint32_t val = arr[a] ? arr[a][vi * stride[a] + c]
                                          : ati_r350_mc_read32(s, card);
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
                    if (trace_event_get_state_backends(
                            TRACE_ATI_R350_3D_VBUF_AOS_SRC)) {
                        uint64_t target;
                        const char *win = ati_r350_mc_describe(s, addr[a],
                                                               &target);

                        trace_ati_r350_3d_vbuf_aos_src(a, size[a], stride[a],
                                                       addr[a], win, target);
                    }
                    trace_ati_r350_3d_vbuf_aos_dw(a,
                        n > base ? dw[base] : 0,
                        n > base + 1 ? dw[base + 1] : 0,
                        n > base + 2 ? dw[base + 2] : 0,
                        n > base + 3 ? dw[base + 3] : 0);
                }
            }
            r300_load_vtx(&d, &fmt, dw, vsize, size[0], &vb[i]);
            r300_attr_texcoord(&d, &fmt, dw, ts, &vb[i]);
            if (i == 0 && d.textured) {
                r300_trace_texcoord(&d, &fmt, dw, &vb[i]);
            }
            if (d.vs_run) {
                float clip[4];

                if (r300_vs_vtx(s, &d, &fmt, dw, &vb[i], clip)) {
                    r300_xform_vtx(s, &d, &vb[i], clip);
                    continue;
                }
            }
            r300_xform_vtx(s, &d, &vb[i], NULL);
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

void ati_r350_r300_draw_vbuf(ATIR350State *s, uint32_t vf)
{
    r300_draw_aos(s, vf, NULL);
}

/*
 * 3D_DRAW_INDX_2: VAP_VF_CNTL (PRIM_WALK=1, NUM_VERTICES = the index
 * count) followed by the indices into the arrays bound for DRAW_VBUF_2,
 * two 16-bit indices per dword, low half first, or one per dword with
 * VAP_VF_CNTL.INDEX_SIZE set.
 */
void ati_r350_r300_draw_indx(ATIR350State *s, const uint32_t *dw, unsigned n)
{
    uint32_t vf = dw[0];
    unsigned nidx = (vf >> 16) & 0xffff;
    unsigned walk = (vf >> 4) & 3;
    bool idx32 = vf & R300_VF_CNTL_INDEX_SIZE_32;
    g_autofree uint16_t *idx = NULL;
    unsigned i;

    if (walk != 1 || !nidx) {
        if (nidx) {
            ati_r350_note_gap(s, R350_GAP_VTX_WALK, walk);
        }
        trace_ati_r350_3d_skip(vf, 0, n);
        return;
    }
    if ((idx32 ? nidx : (nidx + 1) / 2) > n - 1) {
        trace_ati_r350_3d_skip(vf, 0, n);
        return;
    }
    idx = g_new(uint16_t, nidx);
    for (i = 0; i < nidx; i++) {
        uint32_t w = idx32 ? dw[1 + i] : dw[1 + i / 2];

        idx[i] = idx32 ? MIN(w, 0xffffu) : (i & 1) ? w >> 16 : w & 0xffff;
    }
    trace_ati_r350_3d_indx(vf, nidx, idx[0], nidx > 1 ? idx[1] : 0,
                           nidx > 2 ? idx[2] : 0, nidx > 3 ? idx[3] : 0);
    r300_draw_aos(s, vf, idx);
}
