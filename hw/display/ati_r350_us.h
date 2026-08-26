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
} R300UsTex;

/*
 * Which frame register the rasterizer drops each interpolated quantity
 * into, resolved out of RS_INST/RS_IP. The vertex stage of this model
 * emits one texture coordinate and two colours, so an RS instruction
 * naming a third colour or a second coordinate is refused rather than
 * approximated. `col_pkt[n]` is RS_IP's COL_PTR -- which of the vertex
 * stage's colour outputs feeds frame register `col_reg[n]`.
 */
#define R300_US_RS_COLS   2

typedef struct R300UsRs {
    int tex_reg;                        /* register for the coordinate, -1 */
    int col_reg[R300_US_RS_COLS];       /* registers for the colours, -1 */
    uint8_t col_pkt[R300_US_RS_COLS];   /* RS_IP COL_PTR of each */
    uint8_t col_fmt[R300_US_RS_COLS];   /* RS_IP COL_FMT of each */
} R300UsRs;

/* constructs met that this model does not implement */
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
    /*
     * The one texture fetch, resolved: which frame register receives the
     * texel, and -1 when the program fetches nothing. A program with more
     * than one live fetch, or one naming a unit other than 0, is refused
     * -- this model binds a single texture.
     */
    int tex_dst;
    /* the program writes at least one output component */
    bool writes_out;
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
 * floats; `rs_inst`/`rs_ip` are the rasterizer routing tables.
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
                     const uint32_t *rs_ip);

/*
 * Run the ALU half of the program: the caller has already placed the
 * interpolated colours and the fetched texel in `g` per the routing
 * r300_us_analyse() resolved. Leaves the shaded fragment in `g->out`.
 */
void r300_us_run(const R300UsProgram *p, R300UsRegs *g);

/*
 * Translate the program to a GLSL 3.30 function
 *
 *     void us_main(vec4 tex0, vec4 col0, vec4 col1, out vec4 outc);
 *
 * whose arithmetic is the interpreter's, expression for expression,
 * including the host compiler's fusion (see ati_r350_us_glsl.c). Returns
 * false, without writing a usable shader, for anything
 * `p->expressible` refuses.
 */
bool r300_us_glsl(const R300UsProgram *p, char *buf, size_t cap);

#endif /* ATI_R350_US_H */
