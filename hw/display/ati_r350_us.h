/*
 * ATI R300/R350 universal shader (US) -- fragment program encoding,
 * analysis and interpreter.
 *
 * Deliberately free of any device state, like ati_r350_pvs.h: a program
 * arrives as the six register banks the guest uploaded plus the four
 * control words that say which slots of them are in force, and the
 * interpreter runs over plain arrays. The same code that runs inside the
 * model is therefore drivable from a host harness against programs
 * lifted out of a capture.
 *
 * Field names and opcode semantics are transcribed from
 * R3xx_3D_Registers.pdf, section US (US_CONFIG, US_CODE_OFFSET,
 * US_CODE_ADDR_[0-3], US_TEX_INST_[0-31], US_ALU_{RGB,ALPHA}_{ADDR,INST}
 * and US_OUT_FMT_[0-3]) and section RS (RS_COUNT, RS_INST_COUNT,
 * RS_INST_[0-15], RS_IP_[0-7]).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ATI_R350_US_H
#define ATI_R350_US_H

/*
 * How many texture units this model binds, and how many interpolated
 * texture coordinate SETS the rasterizer carries. The two are
 * independent: a unit is bound texture state (TX_OFFSET_n and friends,
 * whose register blocks are sixteen deep on this part), a coordinate set
 * is interpolated per-vertex data that any number of units may sample
 * with.
 *
 * Both numbers are read off a live Mac OS X 10.5 desktop rather than
 * chosen. Its compositor fetches from units 0, 1 and 2, and its RS_IP
 * table is the full eight-entry identity -- TEX_PTR 0, 4, 8, 12, 16,
 * 20, 24, 28, so IP entry k is coordinate set k -- with RS_INST_COUNT
 * naming up to seven live instructions and fragment programs whose LD
 * instructions read the registers sets 3, 4, 5 and 6 land in. That is a
 * multi-tap filter, not a driver writing a table it does not use, and
 * eight is also the number the hardware has: RS_IP is eight entries
 * deep.
 *
 * The first attempt at this row set the count to TWO, from the two
 * TEX_PTR values a truncated refusal trace happened to print, and a
 * live boot then reported `rasterizer attribute routing 0x1` 49 times
 * with the menus still blank. Widening the trace to the whole table is
 * what produced the number above. Units cost draw state only; a
 * coordinate set costs per-vertex memory, and the rasterizer
 * interpolates only the sets a draw's own routing names, so a
 * single-texture draw pays for none of them.
 */
#define R300_TEX_UNITS          8
#define R300_TEXCOORDS          8

/* instruction RAM, and the pixel stack frame the instructions address */
#define R300_US_ALU_SLOTS       64
#define R300_US_TEX_SLOTS       32
#define R300_US_REGS            32
#define R300_US_CONSTS          32
#define R300_US_RS_INSTS        16
#define R300_US_RS_IPS          8

/* US_CONFIG */
#define R300_US_CFG_NLEVEL_MASK      0x7
#define R300_US_CFG_FIRST_TEX        (1u << 3)

/* US_CODE_OFFSET */
#define R300_US_CO_ALU_OFFSET_MASK   0x3f
#define R300_US_CO_TEX_OFFSET_SHIFT  13
#define R300_US_CO_TEX_OFFSET_MASK   0x1f

/* US_CODE_ADDR_n */
#define R300_US_CA_ALU_START_MASK    0x3f
#define R300_US_CA_ALU_SIZE_SHIFT    6
#define R300_US_CA_ALU_SIZE_MASK     0x3f
#define R300_US_CA_TEX_START_SHIFT   12
#define R300_US_CA_TEX_START_MASK    0x1f
#define R300_US_CA_TEX_SIZE_SHIFT    17
#define R300_US_CA_TEX_SIZE_MASK     0x1f

/* US_TEX_INST_n */
#define R300_US_TEX_SRC_MASK         0x1f
#define R300_US_TEX_DST_SHIFT        6
#define R300_US_TEX_DST_MASK         0x1f
#define R300_US_TEX_ID_SHIFT         11
#define R300_US_TEX_ID_MASK          0xf
#define R300_US_TEX_INST_SHIFT       15
#define R300_US_TEX_INST_MASK        0x7

/* US_TEX_INST_n INST field */
#define R300_US_TEXOP_NOP            0
#define R300_US_TEXOP_LD             1
#define R300_US_TEXOP_TEXKILL        2
#define R300_US_TEXOP_PROJ           3
#define R300_US_TEXOP_LODBIAS        4

/*
 * US_ALU_RGB_ADDR_n and US_ALU_ALPHA_ADDR_n. The three source addresses
 * are six bits each: 0-31 name a pixel stack frame register, 32-63 name
 * a constant, which is what the sixth bit means.
 */
#define R300_US_ADDR_SRC_SHIFT(n)    ((n) * 6)
#define R300_US_ADDR_SRC_MASK        0x1f
#define R300_US_ADDR_SRC_CONST       0x20
#define R300_US_ADDR_DST_SHIFT       18
#define R300_US_ADDR_DST_MASK        0x1f
#define R300_US_ADDR_RGB_WMASK_SHIFT 23     /* three bits, R G B */
#define R300_US_ADDR_RGB_OMASK_SHIFT 26
#define R300_US_ADDR_RGB_MASK        0x7
#define R300_US_ADDR_A_WMASK         (1u << 23)
#define R300_US_ADDR_A_OMASK         (1u << 24)

/* US_ALU_RGB_INST_n and US_ALU_ALPHA_INST_n */
#define R300_US_INST_SEL_SHIFT(n)    ((n) * 7)
#define R300_US_INST_SEL_MASK        0x1f
#define R300_US_INST_MOD_SHIFT(n)    ((n) * 7 + 5)
#define R300_US_INST_MOD_MASK        0x3
#define R300_US_INST_SRCP_SHIFT      21
#define R300_US_INST_SRCP_MASK       0x3
#define R300_US_INST_OP_SHIFT        23
#define R300_US_INST_OP_MASK         0xf
#define R300_US_INST_OMOD_SHIFT      27
#define R300_US_INST_OMOD_MASK       0x7
#define R300_US_INST_CLAMP           (1u << 30)

/* input modifiers */
#define R300_US_MOD_NOP              0
#define R300_US_MOD_NEG              1
#define R300_US_MOD_ABS              2
#define R300_US_MOD_NAB              3

/* RGB-side opcodes */
#define R300_US_RGB_MAD              0
#define R300_US_RGB_DP3              1
#define R300_US_RGB_DP4              2
#define R300_US_RGB_D2A              3
#define R300_US_RGB_MIN              4
#define R300_US_RGB_MAX              5
#define R300_US_RGB_CND              7
#define R300_US_RGB_CMP              8
#define R300_US_RGB_FRC              9
#define R300_US_RGB_SOP              10

/* alpha-side opcodes */
#define R300_US_A_MAD                0
#define R300_US_A_DP                 1
#define R300_US_A_MIN                2
#define R300_US_A_MAX                3
#define R300_US_A_CND                5
#define R300_US_A_CMP                6
#define R300_US_A_FRC                7
#define R300_US_A_EX2                8
#define R300_US_A_LN2                9
#define R300_US_A_RCP                10
#define R300_US_A_RSQ                11

/* RS_COUNT / RS_INST_COUNT / RS_INST_n / RS_IP_n */
#define R300_RS_COUNT_IT_MASK        0x7f
#define R300_RS_COUNT_IC_SHIFT       7
#define R300_RS_COUNT_IC_MASK        0xf
#define R300_RS_INST_COUNT_MASK      0xf
#define R300_RS_INST_TEX_ID_MASK     0x7
#define R300_RS_INST_TEX_CN_SHIFT    3
#define R300_RS_INST_TEX_ADDR_SHIFT  6
#define R300_RS_INST_TEX_ADDR_MASK   0x1f
#define R300_RS_INST_COL_ID_SHIFT    11
#define R300_RS_INST_COL_ID_MASK     0x7
#define R300_RS_INST_COL_CN_SHIFT    14
#define R300_RS_INST_COL_ADDR_SHIFT  17
#define R300_RS_INST_COL_ADDR_MASK   0x1f
#define R300_RS_IP_TEX_PTR_MASK      0x3f
#define R300_RS_IP_COL_PTR_SHIFT     6
#define R300_RS_IP_COL_PTR_MASK      0x7
#define R300_RS_IP_COL_FMT_SHIFT     9
#define R300_RS_IP_COL_FMT_MASK      0xf
#define R300_RS_IP_SEL_SHIFT(n)      (13 + (n) * 3)
#define R300_RS_IP_SEL_MASK          0x7
#define R300_RS_IP_SEL_K0            4      /* the value 0.0 */
#define R300_RS_IP_SEL_K1            5      /* the value 1.0 */

/* US_OUT_FMT_n */
#define R300_US_OUT_FMT_MASK         0x1f
#define R300_US_OUT_FMT_C4_8         0
#define R300_US_OUT_FMT_C4_10        1
#define R300_US_OUT_SEL_SHIFT(n)     (8 + (n) * 2)
#define R300_US_OUT_SEL_MASK         0x3
#define R300_US_OUT_SEL_ALPHA        0
#define R300_US_OUT_SEL_RED          1
#define R300_US_OUT_SEL_GREEN        2
#define R300_US_OUT_SEL_BLUE         3

/*
 * One ALU slot, both banks, decoded. `src[n]` is a frame register when
 * `src_const[n]` is false and an entry of the constant file when it is.
 */
typedef struct R300UsAlu {
    uint8_t rgb_src[3], a_src[3];
    bool rgb_src_const[3], a_src_const[3];
    uint8_t rgb_sel[3], a_sel[3];
    uint8_t rgb_mod[3], a_mod[3];
    uint8_t rgb_srcp_op, a_srcp_op;
    uint8_t rgb_op, a_op;
    uint8_t rgb_omod, a_omod;
    bool rgb_clamp, a_clamp;
    uint8_t rgb_dst, a_dst;
    uint8_t rgb_wmask;          /* three bits, R G B */
    uint8_t rgb_omask;
    bool a_wmask, a_omask;
} R300UsAlu;

/* one texture slot, decoded */
typedef struct R300UsTex {
    uint8_t op;                 /* R300_US_TEXOP_* */
    uint8_t src, dst, unit;
    /*
     * The coordinate set whose screen derivatives a filtered fetch takes:
     * the set routed into `src`, else the first set carried in `unit`'s
     * texels, else set 0 (see r300_us_der() in ati_r350_3d.c).
     */
    uint8_t dset;
} R300UsTex;

/*
 * One INDIRECTION LEVEL: a run of texture instructions followed by a run
 * of ALU instructions. US_CONFIG.NLEVEL says how many are live and
 * US_CODE_ADDR_n names each one's two ranges.
 *
 * Levels exist so that a texture fetch can be addressed by an earlier
 * level's ALU result -- a dependent read. That is the whole reason the
 * fetch is performed INSIDE this executor rather than in front of it:
 * with one level the coordinate is known before the program runs and a
 * caller could sample for it, and with two it is not.
 *
 * `alu_at`/`tex_at` index the program's flat decoded arrays; `alu_slot`
 * and `tex_slot` are the relocated microcode slots the level came from,
 * kept for the trace.
 */
#define R300_US_LEVELS  4

typedef struct R300UsLevel {
    uint8_t alu_at, nalu, alu_slot;
    uint8_t tex_at, ntex, tex_slot;
} R300UsLevel;

/*
 * How the executor samples a texture. The interpreter is deliberately
 * free of device state, so the fetch arrives as a callback: `coord` is
 * the frame register the texture instruction names -- s, t, r, q in the
 * units the caller established -- and `texel` receives R, G, B, A.
 * `proj` distinguishes PROJ from LD; `src` is the register's index, which
 * tells the caller whether the coordinate is an interpolated one.
 */
typedef void (*R300UsSampleFn)(void *ctx, unsigned unit, bool proj,
                               unsigned src, const float coord[4],
                               float texel[4]);

/*
 * Which frame register the rasterizer drops each interpolated quantity
 * into, resolved out of RS_INST/RS_IP. The vertex stage of this model
 * emits R300_TEXCOORDS texture coordinate sets and two colours, so an RS
 * instruction naming a third colour, or a coordinate set past that
 * number, is refused rather than approximated. `col_pkt[n]` is RS_IP's
 * COL_PTR -- which of the vertex stage's colour outputs feeds frame
 * register `col_reg[n]`.
 *
 * `tex_reg[k]` is the frame register interpolated coordinate SET k lands
 * in, and -1 when the routing names no register for it. RS_IP's TEX_PTR
 * is a pointer into the rasterizer's input packet in floats, so the set
 * it names is TEX_PTR / 4 -- the corpus reads 0 and 4.
 */
#define R300_US_RS_COLS   2

typedef struct R300UsRs {
    int tex_reg[R300_TEXCOORDS];        /* register per coordinate set, -1 */
    int col_reg[R300_US_RS_COLS];       /* registers for the colours, -1 */
    uint8_t col_pkt[R300_US_RS_COLS];   /* RS_IP COL_PTR of each */
    uint8_t col_fmt[R300_US_RS_COLS];   /* RS_IP COL_FMT of each */
} R300UsRs;

/*
 * One argument of the SPECIALISED path, resolved at decode time.
 *
 * The whole corpus is one ALU slot of MAD, and running a general
 * interpreter over a 32-register frame for every pixel cost the software
 * rasterizer 38 % of its frame rate (9.37 -> 5.85 fps on the Flurry
 * workload). So a program of that shape is resolved once per draw into
 * six of these, and the per-pixel work becomes six loads and a
 * multiply-add -- with no frame to clear and no operand switch.
 *
 * It is the same move `plain_matrix` is for the vertex stage, and it is
 * held to the same standard: the offline harness runs both paths over
 * every program in the corpus and requires them BIT-IDENTICAL.
 */
typedef enum R300UsArgSrc {
    R300_US_ARG_LIT = 0,        /* a literal 0.0, 1.0 or 0.5 */
    R300_US_ARG_TEX,            /* the fetched texel */
    R300_US_ARG_COL0,           /* the first interpolated colour */
    R300_US_ARG_COL1,           /* the second */
    R300_US_ARG_CONST,          /* US_ALU_CONST[ki] */
} R300UsArgSrc;

typedef struct R300UsArgFast {
    uint8_t src;                /* R300UsArgSrc */
    uint8_t ki;                 /* constant index, when src is CONST */
    /* 0 = the vector's own r,g,b; 1..4 = broadcast of r, g, b or a */
    uint8_t chan;
    float lit;
} R300UsArgFast;

/*
 * Constructs met that this model does not implement.
 *
 * US_OUT_FMT_0's component SELECT was one of these until it was
 * implemented as a permutation; only the pixel format can refuse now.
 * The two were once reported as one, which was actively misleading -- a
 * guest asking for a supported format with an unsupported select was
 * reported as "unimplemented fragment output format 0x0", naming a
 * format the model has always run.
 */
typedef struct R300UsGaps {
    uint8_t rgb_op, a_op, tex_op, indirect, rs_route, out_fmt;
    bool has_rgb_op, has_a_op, has_tex_op;
    bool has_indirect, has_rs_route, has_out_fmt;
} R300UsGaps;

typedef struct R300UsProgram {
    bool valid;                 /* the control words describe a program */
    bool expressible;           /* ... and this model can run it */
    unsigned nalu, ntex;
    unsigned alu_first, tex_first;      /* relocated slot numbers */
    R300UsAlu alu[R300_US_ALU_SLOTS];
    R300UsTex tex[R300_US_TEX_SLOTS];
    float konst[R300_US_CONSTS][4];
    unsigned nregs;             /* US_PIXSIZE, the frame this program uses */
    /*
     * One past the highest frame register the program or the rasterizer
     * routing actually names -- US_PIXSIZE is what the guest reserved,
     * this is what has to be initialised per pixel.
     */
    unsigned nregs_used;
    R300UsRs rs;
    R300UsGaps gaps;
    /* the live indirection levels, in execution order (ascending) */
    R300UsLevel level[R300_US_LEVELS];
    uint8_t nlevels;
    bool has_kill;              /* the program contains a TEXKILL */
    /*
     * ONE level, at most one fetch, from unit 0 addressed by coordinate
     * set 0, and no TEXKILL: the shape whose fetch the GL backend and
     * the specialised executor perform in front of the program. Any
     * other program is translated with its fetches inside it.
     */
    bool gl_simple;
    /*
     * The interpolated coordinate sets a draw of this program carries.
     * `ntc` is one past the highest set the routing names; `tc_unit[k]`
     * is the unit whose texels set k is carried in, the unit of the first
     * fetch addressed directly by it; `tc_raw` has bit k set for a routed
     * set no fetch reads directly, which the rasterizer interpolates in
     * all four components as the vertex stage emitted them.
     */
    unsigned ntc;
    uint8_t tc_unit[R300_TEXCOORDS];
    uint32_t tc_raw;
    /*
     * The one texture fetch, resolved: which frame register receives the
     * texel, and -1 when the program fetches nothing. A program with more
     * than one live fetch, or one naming a unit other than 0, is refused
     * -- this model binds a single texture.
     */
    int tex_dst;
    /* the program writes at least one output component */
    bool writes_out;
    /*
     * US_OUT_FMT_0's component select, resolved into a permutation of
     * the shaded fragment.
     *
     * The output fifo has four components and the select says, for each
     * of them, which of the shader's R/G/B/A channels it carries. The
     * destination writer assembles a plain ARGB dword -- which is
     * exactly the select 0x1b (C0 = Blue, C1 = Green, C2 = Red,
     * C3 = Alpha) that this project's whole 10.4 and OS 9 corpus reads,
     * so for those `out_perm` is the identity and `out_permuted` false.
     *
     * Mac OS X 10.5's compositor reads 0x39 (C0 = Red, C1 = Green,
     * C2 = Blue, C3 = Alpha), the same four channels into a destination
     * whose low component is red: red and blue exchanged. Rather than a
     * special case for that value, the select is inverted here into
     * `out[i] := shaded[out_perm[i]]` in the writer's own R,G,B,A order,
     * which expresses all 256 encodings including the ones that repeat a
     * channel. That is why there is no gap for a select any more.
     */
    uint8_t out_perm[4];
    /*
     * out_perm is not the identity. A program with one is deliberately
     * NOT `fast`: the specialised executor runs once per PIXEL, and a
     * branch there is a cost every guest pays for a select only Leopard
     * uses. Measured, three runs against one: 7.35 / 7.51 / 7.48 fps
     * with the branch in the fast path, 7.70 without it. So the
     * permutation lives only in the interpreter, the fast executor is
     * byte-identical to the one that predates the select, and a
     * permuted program pays the interpreter's price -- which it is
     * worth, because before this it was not rendered at all.
     */
    bool out_permuted;
    /*
     * The specialised path: one MAD per bank over resolved arguments.
     * `fast_rgb_mask` and `fast_a` are the output components it writes.
     */
    bool fast;
    R300UsArgFast fast_rgb[3], fast_a[3];
    uint8_t fast_rgb_mask;
    bool fast_a_out;
} R300UsProgram;

/* the pixel stack frame an instruction addresses */
typedef struct R300UsRegs {
    float r[R300_US_REGS][4];   /* [R,G,B,A] */
    float out[4];               /* the output fifo, [R,G,B,A] */
    bool kill;                  /* TEXKILL fired */
} R300UsRegs;

/*
 * Decode the program the control words name. `alu_rgb_addr` and friends
 * are the four 64-entry ALU banks and the 32-entry texture bank as the
 * guest uploaded them; `konst` is US_ALU_CONST as 32 vectors of four
 * floats; `rs_inst`/`rs_ip` are the rasterizer routing tables and
 * `vtx_fmt1` is VAP_OUTPUT_VTX_FMT_1, the component count of each
 * coordinate set in the rasterizer's input packet.
 *
 * Leaves `p->expressible` false, and names the reason in `p->gaps`, for
 * anything the interpreter would not compute correctly -- a caller must
 * check it rather than run the program anyway.
 */
void r300_us_analyse(R300UsProgram *p,
                     uint32_t us_config, uint32_t us_code_offset,
                     const uint32_t *us_code_addr,
                     uint32_t us_pixsize, uint32_t us_out_fmt0,
                     const uint32_t *tex_inst,
                     const uint32_t *rgb_addr, const uint32_t *rgb_inst,
                     const uint32_t *a_addr, const uint32_t *a_inst,
                     const float (*konst)[4],
                     uint32_t rs_inst_count, const uint32_t *rs_inst,
                     const uint32_t *rs_ip, uint32_t vtx_fmt1);

/*
 * Run the program. The caller has placed the interpolated colours, and
 * the interpolated texture COORDINATE, in `g` per the routing
 * r300_us_analyse() resolved; the texture instructions are executed here
 * in program order through `sample`, so a fetch addressed by an earlier
 * level's result works like any other. Leaves the shaded fragment in
 * `g->out`, and `g->kill` set if a TEXKILL fired.
 */
void r300_us_run(const R300UsProgram *p, R300UsRegs *g,
                 R300UsSampleFn sample, void *ctx);

/*
 * The same computation for a program `p->fast` accepted, without
 * building a frame: `tex`, `col0` and `col1` are four floats each in
 * R,G,B,A order and `out` receives the shaded fragment. Callers must
 * check `p->fast` first; the offline harness proves the two agree bit
 * for bit over every program in the corpus.
 *
 * It lives in the header, and is inline, because it runs once per PIXEL:
 * as an out-of-line call in another translation unit it cost the
 * software rasterizer a fifth of its frame rate on its own.
 */
/*
 * The specialised executor. One MAD per bank over arguments resolved at
 * decode time, with no frame to clear and no operand switch: this is
 * what the software rasterizer runs for every program in the corpus,
 * and `r300_us_run()` above remains the specification it is checked
 * against.
 */
static inline void us_fetch3(float out[3], const R300UsArgFast *a,
                             const R300UsProgram *p, const float *v[4])
{
    const float *s;

    if (a->src == R300_US_ARG_LIT) {
        out[0] = out[1] = out[2] = a->lit;
        return;
    }
    s = a->src == R300_US_ARG_CONST ? p->konst[a->ki] : v[a->src];
    if (a->chan == 0) {
        out[0] = s[0];
        out[1] = s[1];
        out[2] = s[2];
    } else {
        out[0] = out[1] = out[2] = s[a->chan - 1];
    }
}

static inline float us_fetch1(const R300UsArgFast *a, const R300UsProgram *p,
                              const float *v[4])
{
    const float *s;

    if (a->src == R300_US_ARG_LIT) {
        return a->lit;
    }
    s = a->src == R300_US_ARG_CONST ? p->konst[a->ki] : v[a->src];
    return s[a->chan - 1];
}

static inline void r300_us_run_fast(const R300UsProgram *p,
                                    const float *tex, const float *col0,
                                    const float *col1, float out[4])
{
    const float *v[4];
    float A[3], B[3], C[3];
    unsigned n;

    v[R300_US_ARG_LIT] = NULL;
    v[R300_US_ARG_TEX] = tex;
    v[R300_US_ARG_COL0] = col0;
    v[R300_US_ARG_COL1] = col1;

    out[0] = out[1] = out[2] = out[3] = 0.0f;
    us_fetch3(A, &p->fast_rgb[0], p, v);
    us_fetch3(B, &p->fast_rgb[1], p, v);
    us_fetch3(C, &p->fast_rgb[2], p, v);
    for (n = 0; n < 3; n++) {
        if (p->fast_rgb_mask & (1u << n)) {
            /*
             * The interpreter's own expression, so the host compiler
             * contracts both of them the same way.
             */
            out[n] = A[n] * B[n] + C[n];
        }
    }
    if (p->fast_a_out) {
        out[3] = us_fetch1(&p->fast_a[0], p, v) *
                 us_fetch1(&p->fast_a[1], p, v) +
                 us_fetch1(&p->fast_a[2], p, v);
    }
}


/*
 * Translate the program to GLSL 3.30 whose arithmetic is the
 * interpreter's, expression for expression, including the host
 * compiler's fusion (see ati_r350_us_glsl.c). A `gl_simple` program
 * becomes
 *
 *     void us_main(vec4 tex0, vec4 col0, vec4 col1, out vec4 outc);
 *
 * and any other defines R350_USGEN and US_NTC, declares `us_tc[8]` and
 *
 *     vec4 us_fetch(int unit, vec4 coord, int dset);
 *
 * for the caller to supply, and becomes
 *
 *     void us_main(vec4 col0, vec4 col1, out vec4 outc, out bool kill);
 *
 * Returns false, without writing a usable shader, for anything
 * `p->expressible` refuses.
 */
bool r300_us_glsl(const R300UsProgram *p, char *buf, size_t cap);

/* the general form for any program, `gl_simple` or not */
bool r300_us_glsl_general(const R300UsProgram *p, char *buf, size_t cap);

/*
 * S3TC, TX_FORMAT1 TXFORMAT 0x0f/0x10/0x11 (Mesa r300_reg.h
 * R300_TX_FORMAT_DXT1/3/5). A block is 4x4 texels, 8 bytes for DXT1 and
 * 16 for DXT3/5, read as a byte stream in ascending address order with
 * multi-byte fields little-endian.
 *
 * Output texels are packed X = blue, Y = green, Z = red, W = alpha, one
 * byte each from the low lane: the component numbering of the 5_6_5
 * endpoint words (X in bits 4:0).
 */
static inline unsigned r300_dxt_block_bytes(unsigned dxt)
{
    return dxt == 1 ? 8 : 16;
}

static inline void r300_dxt_565(uint32_t c, uint32_t rgb[3])
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;

    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

static inline void r300_dxt_decode(unsigned dxt, const uint8_t *b,
                                   uint32_t out[16])
{
    const uint8_t *cb = dxt == 1 ? b : b + 8;
    uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
    uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) |
                   ((uint32_t)cb[7] << 24);
    uint32_t pal[4][3], pa[4] = { 255, 255, 255, 255 }, apal[8];
    uint64_t abits = 0;
    unsigned k, t;

    r300_dxt_565(c0, pal[0]);
    r300_dxt_565(c1, pal[1]);
    for (k = 0; k < 3; k++) {
        if (dxt != 1 || c0 > c1) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
    if (dxt == 1 && c0 <= c1) {
        pa[3] = 0;
    }
    if (dxt == 5) {
        uint32_t a0 = b[0], a1 = b[1];

        for (k = 0; k < 6; k++) {
            abits |= (uint64_t)b[2 + k] << (8 * k);
        }
        apal[0] = a0;
        apal[1] = a1;
        for (k = 2; k < 8; k++) {
            if (a0 > a1) {
                apal[k] = ((8 - k) * a0 + (k - 1) * a1) / 7;
            } else if (k < 6) {
                apal[k] = ((6 - k) * a0 + (k - 1) * a1) / 5;
            } else {
                apal[k] = k == 6 ? 0 : 255;
            }
        }
    }
    for (t = 0; t < 16; t++) {
        uint32_t a;

        k = (idx >> (2 * t)) & 3;
        if (dxt == 1) {
            a = pa[k];
        } else if (dxt == 3) {
            a = ((b[t >> 1] >> ((t & 1) * 4)) & 15) * 17;
        } else {
            a = apal[(abits >> (3 * t)) & 7];
        }
        out[t] = pal[k][2] | (pal[k][1] << 8) | (pal[k][0] << 16) | (a << 24);
    }
}

/*
 * Cube-map face selection (OpenGL 1.3 table 3.19; faces +X -X +Y -Y +Z
 * -Z are 0..5, stored in that order per level, R5xx acceleration guide
 * 4.6). The major axis names the face; sc, tc and ma are components
 * `is`, `it` and `im` of the direction times `ss`, `st` and `sm`.
 */
typedef struct R300CubeSel {
    unsigned face;
    unsigned is, it, im;
    float ss, st, sm;
} R300CubeSel;

static inline R300CubeSel r300_cube_sel(const float v[3])
{
    float ax = v[0] < 0.0f ? -v[0] : v[0];
    float ay = v[1] < 0.0f ? -v[1] : v[1];
    float az = v[2] < 0.0f ? -v[2] : v[2];
    R300CubeSel c;
    bool p;

    if (ax >= ay && ax >= az) {
        p = v[0] >= 0.0f;
        c.face = p ? 0 : 1;
        c.im = 0;
        c.is = 2;
        c.it = 1;
        c.ss = p ? -1.0f : 1.0f;
        c.st = -1.0f;
    } else if (ay >= az) {
        p = v[1] >= 0.0f;
        c.face = p ? 2 : 3;
        c.im = 1;
        c.is = 0;
        c.it = 2;
        c.ss = 1.0f;
        c.st = p ? 1.0f : -1.0f;
    } else {
        p = v[2] >= 0.0f;
        c.face = p ? 4 : 5;
        c.im = 2;
        c.is = 0;
        c.it = 1;
        c.ss = p ? 1.0f : -1.0f;
        c.st = -1.0f;
    }
    c.sm = p ? 1.0f : -1.0f;
    return c;
}

/*
 * The face coordinates in that face's level-0 texels, and their screen
 * derivatives from those of the direction (dv[c][0] = d/dx, dv[c][1] =
 * d/dy, or NULL). One operation per statement, mirrored by the GLSL
 * below.
 */
static inline unsigned r300_cube_coord(const float v[3], const float dv[3][2],
                                       int w, int h, float *fs, float *ft,
                                       float der[4])
{
    R300CubeSel c = r300_cube_sel(v);
    float ma = c.sm * v[c.im];
    float sc = c.ss * v[c.is];
    float tc = c.st * v[c.it];
    float qs = 0.0f, qt = 0.0f, hs, ht, fw = (float)w, fh = (float)h;
    unsigned k;

    for (k = 0; k < 4; k++) {
        der[k] = 0.0f;
    }
    if (ma > 0.0f) {
        qs = sc / ma;
        qt = tc / ma;
    }
    hs = qs * 0.5f;
    hs = hs + 0.5f;
    *fs = hs * fw;
    ht = qt * 0.5f;
    ht = ht + 0.5f;
    *ft = ht * fh;
    if (dv && ma > 0.0f) {
        for (k = 0; k < 2; k++) {
            float dm = c.sm * dv[c.im][k];
            float ds = c.ss * dv[c.is][k];
            float dt = c.st * dv[c.it][k];
            float e = qs * dm;

            e = ds - e;
            e = e / ma;
            e = e * 0.5f;
            der[k * 2] = e * fw;
            e = qt * dm;
            e = dt - e;
            e = e / ma;
            e = e * 0.5f;
            der[k * 2 + 1] = e * fh;
        }
    }
    return c.face;
}

/* r300_cube_coord() in GLSL: returns the face, `fd` = (fs, ft) */
#define R300_CUBE_GLSL \
"int cube_coord(vec3 v, vec3 dx, vec3 dy, bool lod, int w, int h,\n" \
"               out vec2 fd, out vec4 der)\n" \
"{\n" \
"    vec3 a = abs(v);\n" \
"    int face, im, is, it;\n" \
"    float ss, st, sm;\n" \
"    bool p;\n" \
"    if (a.x >= a.y && a.x >= a.z) {\n" \
"        p = v.x >= 0.0; face = p ? 0 : 1; im = 0; is = 2; it = 1;\n" \
"        ss = p ? -1.0 : 1.0; st = -1.0;\n" \
"    } else if (a.y >= a.z) {\n" \
"        p = v.y >= 0.0; face = p ? 2 : 3; im = 1; is = 0; it = 2;\n" \
"        ss = 1.0; st = p ? 1.0 : -1.0;\n" \
"    } else {\n" \
"        p = v.z >= 0.0; face = p ? 4 : 5; im = 2; is = 0; it = 1;\n" \
"        ss = p ? 1.0 : -1.0; st = -1.0;\n" \
"    }\n" \
"    sm = p ? 1.0 : -1.0;\n" \
"    precise float ma = sm * v[im];\n" \
"    precise float sc = ss * v[is];\n" \
"    precise float tc = st * v[it];\n" \
"    precise float qs = 0.0, qt = 0.0;\n" \
"    precise float fw = float(w), fh = float(h);\n" \
"    if (ma > 0.0) { qs = sc / ma; qt = tc / ma; }\n" \
"    precise float hs = qs * 0.5;\n" \
"    hs = hs + 0.5;\n" \
"    precise float ht = qt * 0.5;\n" \
"    ht = ht + 0.5;\n" \
"    precise float os = hs * fw;\n" \
"    precise float ot = ht * fh;\n" \
"    fd = vec2(os, ot);\n" \
"    der = vec4(0.0);\n" \
"    if (lod && ma > 0.0) {\n" \
"        for (int k = 0; k < 2; k++) {\n" \
"            vec3 g = k == 0 ? dx : dy;\n" \
"            precise float dm = sm * g[im];\n" \
"            precise float ds = ss * g[is];\n" \
"            precise float dt = st * g[it];\n" \
"            precise float e = qs * dm;\n" \
"            e = ds - e;\n" \
"            e = e / ma;\n" \
"            e = e * 0.5;\n" \
"            precise float es = e * fw;\n" \
"            e = qt * dm;\n" \
"            e = dt - e;\n" \
"            e = e / ma;\n" \
"            e = e * 0.5;\n" \
"            precise float et = e * fh;\n" \
"            if (k == 0) { der.x = es; der.y = et; }\n" \
"            else { der.z = es; der.w = et; }\n" \
"        }\n" \
"    }\n" \
"    return face;\n" \
"}\n"

#endif /* ATI_R350_US_H */
