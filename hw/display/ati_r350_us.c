/*
 * ATI R300/R350 universal shader (US) -- fragment program decode and
 * interpreter.
 *
 * This is the executable specification of the fragment stage. Before it
 * existed the model computed `texel * colour` for every textured draw
 * and the interpolated colour alone for every untextured one -- a
 * heuristic that happens to be what two of the ten programs the guests
 * in this project's corpus upload actually compute. The other eight do
 * something else: take the alpha from the colour and not the texel, take
 * the colour from the texture and ignore the vertex colour, modulate the
 * alpha by a shader constant, add a second interpolated colour (Chess's
 * specular term), or write no colour at all.
 *
 * Deliberately free of device state, like ati_r350_pvs.c: everything
 * arrives as plain arrays, so the offline harness drives exactly the
 * code the device runs.
 *
 * Field names and semantics are transcribed from R3xx_3D_Registers.pdf,
 * sections US and RS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include <float.h>
#include "ati_r350_us.h"

/*
 * A source SLOT is two halves: the RGB bank's address supplies the R, G
 * and B components and the alpha bank's address the A component, which is
 * why `srcN.aaa` on the RGB side reads a register the RGB bank never
 * names. Resolved once per instruction into this shape so both the
 * selector table and the translator can be written from the PDF's own
 * wording.
 */
typedef struct R300UsSrc {
    float rgb[3];
    float a;
} R300UsSrc;

static void us_gap_rgb_op(R300UsGaps *g, uint8_t op)
{
    if (g && !g->has_rgb_op) {
        g->has_rgb_op = true;
        g->rgb_op = op;
    }
}

static void us_gap_a_op(R300UsGaps *g, uint8_t op)
{
    if (g && !g->has_a_op) {
        g->has_a_op = true;
        g->a_op = op;
    }
}

static void us_gap_tex_op(R300UsGaps *g, uint8_t op)
{
    if (g && !g->has_tex_op) {
        g->has_tex_op = true;
        g->tex_op = op;
    }
}

static void us_gap_indirect(R300UsGaps *g, uint8_t n)
{
    if (g && !g->has_indirect) {
        g->has_indirect = true;
        g->indirect = n;
    }
}

static void us_gap_rs(R300UsGaps *g, uint8_t what)
{
    if (g && !g->has_rs_route) {
        g->has_rs_route = true;
        g->rs_route = what;
    }
}

static void us_gap_out_fmt(R300UsGaps *g, uint8_t v)
{
    if (g && !g->has_out_fmt) {
        g->has_out_fmt = true;
        g->out_fmt = v;
    }
}

static void us_decode_alu(R300UsAlu *a, uint32_t rgb_addr, uint32_t rgb_inst,
                          uint32_t a_addr, uint32_t a_inst)
{
    unsigned n;

    for (n = 0; n < 3; n++) {
        unsigned ra = (rgb_addr >> R300_US_ADDR_SRC_SHIFT(n)) & 0x3f;
        unsigned aa = (a_addr >> R300_US_ADDR_SRC_SHIFT(n)) & 0x3f;

        a->rgb_src[n] = ra & R300_US_ADDR_SRC_MASK;
        a->rgb_src_const[n] = (ra & R300_US_ADDR_SRC_CONST) != 0;
        a->a_src[n] = aa & R300_US_ADDR_SRC_MASK;
        a->a_src_const[n] = (aa & R300_US_ADDR_SRC_CONST) != 0;
        a->rgb_sel[n] = (rgb_inst >> R300_US_INST_SEL_SHIFT(n)) &
                        R300_US_INST_SEL_MASK;
        a->a_sel[n] = (a_inst >> R300_US_INST_SEL_SHIFT(n)) &
                      R300_US_INST_SEL_MASK;
        a->rgb_mod[n] = (rgb_inst >> R300_US_INST_MOD_SHIFT(n)) &
                        R300_US_INST_MOD_MASK;
        a->a_mod[n] = (a_inst >> R300_US_INST_MOD_SHIFT(n)) &
                      R300_US_INST_MOD_MASK;
    }
    a->rgb_srcp_op = (rgb_inst >> R300_US_INST_SRCP_SHIFT) &
                     R300_US_INST_SRCP_MASK;
    a->a_srcp_op = (a_inst >> R300_US_INST_SRCP_SHIFT) &
                   R300_US_INST_SRCP_MASK;
    a->rgb_op = (rgb_inst >> R300_US_INST_OP_SHIFT) & R300_US_INST_OP_MASK;
    a->a_op = (a_inst >> R300_US_INST_OP_SHIFT) & R300_US_INST_OP_MASK;
    a->rgb_omod = (rgb_inst >> R300_US_INST_OMOD_SHIFT) &
                  R300_US_INST_OMOD_MASK;
    a->a_omod = (a_inst >> R300_US_INST_OMOD_SHIFT) & R300_US_INST_OMOD_MASK;
    a->rgb_clamp = (rgb_inst & R300_US_INST_CLAMP) != 0;
    a->a_clamp = (a_inst & R300_US_INST_CLAMP) != 0;
    a->rgb_dst = (rgb_addr >> R300_US_ADDR_DST_SHIFT) & R300_US_ADDR_DST_MASK;
    a->a_dst = (a_addr >> R300_US_ADDR_DST_SHIFT) & R300_US_ADDR_DST_MASK;
    a->rgb_wmask = (rgb_addr >> R300_US_ADDR_RGB_WMASK_SHIFT) &
                   R300_US_ADDR_RGB_MASK;
    a->rgb_omask = (rgb_addr >> R300_US_ADDR_RGB_OMASK_SHIFT) &
                   R300_US_ADDR_RGB_MASK;
    a->a_wmask = (a_addr & R300_US_ADDR_A_WMASK) != 0;
    a->a_omask = (a_addr & R300_US_ADDR_A_OMASK) != 0;
}

/*
 * The rasterizer routing. RS_INST_n names an entry of the RS_IP table
 * for its texture address and another for its colour; RS_IP says where
 * in the rasterizer's input packet each of them comes from. This model
 * emits one coordinate and two colours, so COL_PTR 0 and 1 are the two
 * colour outputs and anything above them is refused.
 */
static void us_decode_rs(R300UsRs *rs, R300UsGaps *gaps,
                         uint32_t rs_inst_count, const uint32_t *rs_inst,
                         const uint32_t *rs_ip)
{
    unsigned n, ninst = (rs_inst_count & R300_RS_INST_COUNT_MASK) + 1;
    unsigned ncol = 0;

    rs->tex_reg = -1;
    for (n = 0; n < R300_US_RS_COLS; n++) {
        rs->col_reg[n] = -1;
        rs->col_pkt[n] = 0;
        rs->col_fmt[n] = 0;
    }
    if (ninst > R300_US_RS_INSTS) {
        ninst = R300_US_RS_INSTS;
    }
    for (n = 0; n < ninst; n++) {
        uint32_t v = rs_inst[n];
        unsigned id;

        if ((v >> R300_RS_INST_TEX_CN_SHIFT) & 0x7) {
            id = v & R300_RS_INST_TEX_ID_MASK;
            if (id >= R300_US_RS_IPS ||
                (rs_ip[id] & R300_RS_IP_TEX_PTR_MASK) != 0) {
                /* a second interpolated coordinate: not emitted here */
                us_gap_rs(gaps, 1);
            } else if (rs->tex_reg >= 0) {
                us_gap_rs(gaps, 2);
            } else {
                rs->tex_reg = (v >> R300_RS_INST_TEX_ADDR_SHIFT) &
                              R300_RS_INST_TEX_ADDR_MASK;
            }
        }
        if ((v >> R300_RS_INST_COL_CN_SHIFT) & 0x7) {
            unsigned ptr, fmt;

            id = (v >> R300_RS_INST_COL_ID_SHIFT) & R300_RS_INST_COL_ID_MASK;
            if (id >= R300_US_RS_IPS) {
                us_gap_rs(gaps, 3);
                continue;
            }
            ptr = (rs_ip[id] >> R300_RS_IP_COL_PTR_SHIFT) &
                  R300_RS_IP_COL_PTR_MASK;
            fmt = (rs_ip[id] >> R300_RS_IP_COL_FMT_SHIFT) &
                  R300_RS_IP_COL_FMT_MASK;
            if (ptr >= R300_US_RS_COLS || ncol >= R300_US_RS_COLS) {
                us_gap_rs(gaps, 4);
                continue;
            }
            rs->col_reg[ncol] = (v >> R300_RS_INST_COL_ADDR_SHIFT) &
                                R300_RS_INST_COL_ADDR_MASK;
            rs->col_pkt[ncol] = ptr;
            rs->col_fmt[ncol] = fmt;
            ncol++;
        }
    }
}

/* opcodes this interpreter implements; anything else refuses the program */
static bool us_rgb_op_known(uint8_t op)
{
    switch (op) {
    case R300_US_RGB_MAD:
    case R300_US_RGB_DP3:
    case R300_US_RGB_DP4:
    case R300_US_RGB_D2A:
    case R300_US_RGB_MIN:
    case R300_US_RGB_MAX:
    case R300_US_RGB_CND:
    case R300_US_RGB_CMP:
    case R300_US_RGB_FRC:
    case R300_US_RGB_SOP:
        return true;
    default:
        return false;
    }
}

static bool us_a_op_known(uint8_t op)
{
    switch (op) {
    case R300_US_A_MAD:
    case R300_US_A_DP:
    case R300_US_A_MIN:
    case R300_US_A_MAX:
    case R300_US_A_CND:
    case R300_US_A_CMP:
    case R300_US_A_FRC:
    case R300_US_A_EX2:
    case R300_US_A_LN2:
    case R300_US_A_RCP:
    case R300_US_A_RSQ:
        return true;
    default:
        return false;
    }
}

void r300_us_analyse(R300UsProgram *p,
                     uint32_t us_config, uint32_t us_code_offset,
                     const uint32_t *us_code_addr,
                     uint32_t us_pixsize, uint32_t us_out_fmt0,
                     const uint32_t *tex_inst,
                     const uint32_t *rgb_addr, const uint32_t *rgb_inst,
                     const uint32_t *a_addr, const uint32_t *a_inst,
                     const float (*konst)[4],
                     uint32_t rs_inst_count, const uint32_t *rs_inst,
                     const uint32_t *rs_ip)
{
    unsigned nlevel = us_config & R300_US_CFG_NLEVEL_MASK;
    unsigned aoff = us_code_offset & R300_US_CO_ALU_OFFSET_MASK;
    unsigned toff = (us_code_offset >> R300_US_CO_TEX_OFFSET_SHIFT) &
                    R300_US_CO_TEX_OFFSET_MASK;
    uint32_t ca = us_code_addr[3];
    unsigned i;

    memset(p, 0, sizeof(*p));
    p->tex_dst = -1;
    p->rs.tex_reg = -1;
    p->rs.col_reg[0] = p->rs.col_reg[1] = -1;

    /*
     * Only level 3, the level a DX7-style single-pass program occupies,
     * is modelled: a lower level's texture fetch is addressed by an
     * earlier level's ALU result, which needs the fetch inside the
     * interpreter rather than in front of it. Every draw in every
     * capture this project holds reads NLEVEL 0 (20867 of 20867), so
     * this refuses nothing that has ever been seen.
     */
    if (nlevel != 0) {
        us_gap_indirect(&p->gaps, nlevel);
        return;
    }

    p->alu_first = (ca & R300_US_CA_ALU_START_MASK) + aoff;
    p->nalu = ((ca >> R300_US_CA_ALU_SIZE_SHIFT) & R300_US_CA_ALU_SIZE_MASK)
              + 1;
    p->tex_first = ((ca >> R300_US_CA_TEX_START_SHIFT) &
                    R300_US_CA_TEX_START_MASK) + toff;
    p->ntex = (us_config & R300_US_CFG_FIRST_TEX) ?
              (((ca >> R300_US_CA_TEX_SIZE_SHIFT) &
                R300_US_CA_TEX_SIZE_MASK) + 1) : 0;
    p->nregs = (us_pixsize & 0x1f) + 1;

    if (p->alu_first + p->nalu > R300_US_ALU_SLOTS ||
        p->tex_first + p->ntex > R300_US_TEX_SLOTS) {
        return;                 /* not valid: the range leaves the RAM */
    }
    p->valid = true;
    p->expressible = true;

    for (i = 0; i < R300_US_CONSTS; i++) {
        p->konst[i][0] = konst[i][0];
        p->konst[i][1] = konst[i][1];
        p->konst[i][2] = konst[i][2];
        p->konst[i][3] = konst[i][3];
    }

    for (i = 0; i < p->ntex; i++) {
        uint32_t v = tex_inst[p->tex_first + i];
        R300UsTex *t = &p->tex[i];

        t->op = (v >> R300_US_TEX_INST_SHIFT) & R300_US_TEX_INST_MASK;
        t->src = v & R300_US_TEX_SRC_MASK;
        t->dst = (v >> R300_US_TEX_DST_SHIFT) & R300_US_TEX_DST_MASK;
        t->unit = (v >> R300_US_TEX_ID_SHIFT) & R300_US_TEX_ID_MASK;
        switch (t->op) {
        case R300_US_TEXOP_NOP:
            break;
        case R300_US_TEXOP_LD:
        case R300_US_TEXOP_PROJ:
            /*
             * Where the texel lands. How the coordinate reaches the
             * sampler is settled upstream -- r300_attr_texcoord() and
             * the vertex program's own texture-coordinate output already
             * deliver s and t in texel units, projection applied -- so
             * LD and PROJ differ here only in a name.
             */
            if (t->unit != 0 || p->tex_dst >= 0) {
                us_gap_tex_op(&p->gaps, 0x10 | t->unit);
                p->expressible = false;
            } else {
                p->tex_dst = t->dst;
            }
            break;
        default:
            us_gap_tex_op(&p->gaps, t->op);
            p->expressible = false;
            break;
        }
    }

    for (i = 0; i < p->nalu; i++) {
        unsigned k = p->alu_first + i;

        us_decode_alu(&p->alu[i], rgb_addr[k], rgb_inst[k],
                      a_addr[k], a_inst[k]);
        if (!us_rgb_op_known(p->alu[i].rgb_op)) {
            us_gap_rgb_op(&p->gaps, p->alu[i].rgb_op);
            p->expressible = false;
        }
        if (!us_a_op_known(p->alu[i].a_op)) {
            us_gap_a_op(&p->gaps, p->alu[i].a_op);
            p->expressible = false;
        }
        if (p->alu[i].rgb_omask || p->alu[i].a_omask) {
            p->writes_out = true;
        }
    }

    us_decode_rs(&p->rs, &p->gaps, rs_inst_count, rs_inst, rs_ip);
    if (p->gaps.has_rs_route) {
        p->expressible = false;
    }

    /*
     * How much of the pixel stack frame has to be initialised per pixel.
     * US_PIXSIZE is what the guest reserved; the instructions can name a
     * register above it, and the corpus's programs use one, two or three
     * of thirty-two, so this is worth resolving rather than clearing the
     * whole file for every fragment.
     */
    p->nregs_used = p->nregs;
    for (i = 0; i < p->nalu; i++) {
        const R300UsAlu *a = &p->alu[i];
        unsigned n;

        for (n = 0; n < 3; n++) {
            if (!a->rgb_src_const[n] && a->rgb_src[n] + 1u > p->nregs_used) {
                p->nregs_used = a->rgb_src[n] + 1u;
            }
            if (!a->a_src_const[n] && a->a_src[n] + 1u > p->nregs_used) {
                p->nregs_used = a->a_src[n] + 1u;
            }
        }
        if (a->rgb_wmask && a->rgb_dst + 1u > p->nregs_used) {
            p->nregs_used = a->rgb_dst + 1u;
        }
        if (a->a_wmask && a->a_dst + 1u > p->nregs_used) {
            p->nregs_used = a->a_dst + 1u;
        }
    }
    if (p->tex_dst >= 0 && (unsigned)p->tex_dst + 1u > p->nregs_used) {
        p->nregs_used = p->tex_dst + 1u;
    }
    for (i = 0; i < R300_US_RS_COLS; i++) {
        if (p->rs.col_reg[i] >= 0 &&
            (unsigned)p->rs.col_reg[i] + 1u > p->nregs_used) {
            p->nregs_used = p->rs.col_reg[i] + 1u;
        }
    }
    if (p->nregs_used > R300_US_REGS) {
        p->nregs_used = R300_US_REGS;
    }

    /*
     * How the output fifo maps onto the render target's components. The
     * whole corpus reads 0x1b01 -- C4_10 with C0 = Blue, C1 = Green,
     * C2 = Red, C3 = Alpha -- which is the plain ARGB the destination
     * writer already assembles. Anything else would need that writer to
     * permute, so it is named rather than silently ignored. A program
     * that writes no output at all never reaches the target and its
     * format is not consulted.
     */
    if (p->writes_out) {
        unsigned fmt = us_out_fmt0 & R300_US_OUT_FMT_MASK;
        unsigned sel = (us_out_fmt0 >> R300_US_OUT_SEL_SHIFT(0)) & 0xff;

        if ((fmt != R300_US_OUT_FMT_C4_8 && fmt != R300_US_OUT_FMT_C4_10) ||
            sel != ((R300_US_OUT_SEL_BLUE << 0) |
                    (R300_US_OUT_SEL_GREEN << 2) |
                    (R300_US_OUT_SEL_RED << 4) |
                    (R300_US_OUT_SEL_ALPHA << 6))) {
            us_gap_out_fmt(&p->gaps, fmt);
            p->expressible = false;
        }
    }
}

/* ---------------------------------------------------------------- */
/* the interpreter                                                    */

static inline float us_mod(float v, uint8_t mod)
{
    switch (mod) {
    case R300_US_MOD_NEG:
        return -v;
    case R300_US_MOD_ABS:
        return fabsf(v);
    case R300_US_MOD_NAB:
        return -fabsf(v);
    default:
        return v;
    }
}

static inline float us_omod(float v, uint8_t omod)
{
    switch (omod) {
    case 1: return v * 2.0f;
    case 2: return v * 4.0f;
    case 3: return v * 8.0f;
    case 4: return v * 0.5f;
    case 5: return v * 0.25f;
    case 6: return v * 0.125f;
    default: return v;
    }
}

static inline float us_clamp01(float v, bool clamp)
{
    if (!clamp) {
        return v;
    }
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/*
 * One RGB-side argument. `s` is the three resolved source slots and
 * `srcp` the pre-subtract value; the selector table is the PDF's, in its
 * own order.
 */
static void us_arg_rgb(float out[3], const R300UsSrc s[3],
                       const float srcp[3], float srcp_a,
                       uint8_t sel, uint8_t mod)
{
    unsigned n;

    if (sel < 12) {
        const R300UsSrc *v = &s[sel / 4];

        switch (sel % 4) {
        case 0:
            out[0] = v->rgb[0]; out[1] = v->rgb[1]; out[2] = v->rgb[2];
            break;
        case 1:
            out[0] = out[1] = out[2] = v->rgb[0];
            break;
        case 2:
            out[0] = out[1] = out[2] = v->rgb[1];
            break;
        default:
            out[0] = out[1] = out[2] = v->rgb[2];
            break;
        }
    } else if (sel < 15) {
        out[0] = out[1] = out[2] = s[sel - 12].a;
    } else if (sel < 20) {
        switch (sel) {
        case 15:
            out[0] = srcp[0]; out[1] = srcp[1]; out[2] = srcp[2];
            break;
        case 16:
            out[0] = out[1] = out[2] = srcp[0];
            break;
        case 17:
            out[0] = out[1] = out[2] = srcp[1];
            break;
        case 18:
            out[0] = out[1] = out[2] = srcp[2];
            break;
        default:
            out[0] = out[1] = out[2] = srcp_a;
            break;
        }
    } else if (sel < 23) {
        float k = sel == 20 ? 0.0f : (sel == 21 ? 1.0f : 0.5f);

        out[0] = out[1] = out[2] = k;
    } else if (sel < 32) {
        const R300UsSrc *v = &s[(sel - 23) % 3];

        switch ((sel - 23) / 3) {
        case 0:         /* .gbr */
            out[0] = v->rgb[1]; out[1] = v->rgb[2]; out[2] = v->rgb[0];
            break;
        case 1:         /* .brg */
            out[0] = v->rgb[2]; out[1] = v->rgb[0]; out[2] = v->rgb[1];
            break;
        default:        /* .abg */
            out[0] = v->a; out[1] = v->rgb[2]; out[2] = v->rgb[1];
            break;
        }
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    if (mod != R300_US_MOD_NOP) {
        for (n = 0; n < 3; n++) {
            out[n] = us_mod(out[n], mod);
        }
    }
}

/* one alpha-side argument, same table reduced to scalars */
static float us_arg_a(const R300UsSrc s[3], const float srcp[3], float srcp_a,
                      uint8_t sel, uint8_t mod)
{
    float v;

    if (sel < 9) {
        v = s[sel / 3].rgb[sel % 3];
    } else if (sel < 12) {
        v = s[sel - 9].a;
    } else if (sel < 15) {
        v = srcp[sel - 12];
    } else if (sel == 15) {
        v = srcp_a;
    } else if (sel == 16) {
        v = 0.0f;
    } else if (sel == 17) {
        v = 1.0f;
    } else if (sel == 18) {
        v = 0.5f;
    } else {
        v = 0.0f;
    }
    return us_mod(v, mod);
}

static void us_srcp_rgb(float out[3], const R300UsSrc s[3], uint8_t op)
{
    unsigned n;

    for (n = 0; n < 3; n++) {
        switch (op) {
        case 0:
            out[n] = 1.0f - 2.0f * s[0].rgb[n];
            break;
        case 1:
            out[n] = s[1].rgb[n] - s[0].rgb[n];
            break;
        case 2:
            out[n] = s[1].rgb[n] + s[0].rgb[n];
            break;
        default:
            out[n] = 1.0f - s[0].rgb[n];
            break;
        }
    }
}

static float us_srcp_a(const R300UsSrc s[3], uint8_t op)
{
    switch (op) {
    case 0: return 1.0f - 2.0f * s[0].a;
    case 1: return s[1].a - s[0].a;
    case 2: return s[1].a + s[0].a;
    default: return 1.0f - s[0].a;
    }
}

void r300_us_run(const R300UsProgram *p, R300UsRegs *g)
{
    unsigned i, n;

    for (i = 0; i < p->nalu; i++) {
        const R300UsAlu *a = &p->alu[i];
        R300UsSrc s[3];
        float srcp[3], srcp_a;
        float A[3], B[3], C[3], res[3];
        float aA, aB, aC, ares;
        float dot;

        for (n = 0; n < 3; n++) {
            const float *rv = a->rgb_src_const[n] ? p->konst[a->rgb_src[n]]
                                                  : g->r[a->rgb_src[n]];
            const float *av = a->a_src_const[n] ? p->konst[a->a_src[n]]
                                                : g->r[a->a_src[n]];

            s[n].rgb[0] = rv[0];
            s[n].rgb[1] = rv[1];
            s[n].rgb[2] = rv[2];
            s[n].a = av[3];
        }
        us_srcp_rgb(srcp, s, a->rgb_srcp_op);
        srcp_a = us_srcp_a(s, a->a_srcp_op);

        us_arg_rgb(A, s, srcp, srcp_a, a->rgb_sel[0], a->rgb_mod[0]);
        us_arg_rgb(B, s, srcp, srcp_a, a->rgb_sel[1], a->rgb_mod[1]);
        us_arg_rgb(C, s, srcp, srcp_a, a->rgb_sel[2], a->rgb_mod[2]);
        aA = us_arg_a(s, srcp, srcp_a, a->a_sel[0], a->a_mod[0]);
        aB = us_arg_a(s, srcp, srcp_a, a->a_sel[1], a->a_mod[1]);
        aC = us_arg_a(s, srcp, srcp_a, a->a_sel[2], a->a_mod[2]);

        /*
         * The dot products are shared between the banks: the RGB side's
         * DP4 takes its fourth term from the alpha arguments, and the
         * alpha side's DP reads the result the RGB side computed. Both
         * are evaluated here so either bank can name it.
         */
        dot = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];

        switch (a->rgb_op) {
        case R300_US_RGB_DP3:
            res[0] = res[1] = res[2] = dot;
            break;
        case R300_US_RGB_DP4:
            res[0] = res[1] = res[2] = dot + aA * aB;
            break;
        case R300_US_RGB_D2A:
            res[0] = res[1] = res[2] = A[0] * B[0] + A[1] * B[1] + C[2];
            break;
        case R300_US_RGB_MIN:
            for (n = 0; n < 3; n++) {
                res[n] = A[n] < B[n] ? A[n] : B[n];
            }
            break;
        case R300_US_RGB_MAX:
            for (n = 0; n < 3; n++) {
                res[n] = A[n] > B[n] ? A[n] : B[n];
            }
            break;
        case R300_US_RGB_CND:
            for (n = 0; n < 3; n++) {
                res[n] = C[n] > 0.5f ? A[n] : B[n];
            }
            break;
        case R300_US_RGB_CMP:
            for (n = 0; n < 3; n++) {
                res[n] = C[n] >= 0.0f ? A[n] : B[n];
            }
            break;
        case R300_US_RGB_FRC:
            for (n = 0; n < 3; n++) {
                res[n] = A[n] - floorf(A[n]);
            }
            break;
        case R300_US_RGB_SOP:
            res[0] = res[1] = res[2] = 0.0f;    /* filled in below */
            break;
        default:
            /*
             * MAD. Written as one expression so the host compiler
             * contracts it into a fused multiply-add exactly as it does
             * everywhere else in this model's pixel path -- the GLSL
             * translator emits fma() to match, and the offline harness
             * measures that they agree.
             */
            for (n = 0; n < 3; n++) {
                res[n] = A[n] * B[n] + C[n];
            }
            break;
        }

        switch (a->a_op) {
        case R300_US_A_DP:
            ares = dot;
            break;
        case R300_US_A_MIN:
            ares = aA < aB ? aA : aB;
            break;
        case R300_US_A_MAX:
            ares = aA > aB ? aA : aB;
            break;
        case R300_US_A_CND:
            ares = aC > 0.5f ? aA : aB;
            break;
        case R300_US_A_CMP:
            ares = aC >= 0.0f ? aA : aB;
            break;
        case R300_US_A_FRC:
            ares = aA - floorf(aA);
            break;
        case R300_US_A_EX2:
            ares = exp2f(aA);
            break;
        case R300_US_A_LN2:
            ares = aA > 0.0f ? log2f(aA) : -FLT_MAX;
            break;
        case R300_US_A_RCP:
            ares = aA != 0.0f ? 1.0f / aA : FLT_MAX;
            break;
        case R300_US_A_RSQ:
            ares = aA != 0.0f ? 1.0f / sqrtf(fabsf(aA)) : FLT_MAX;
            break;
        default:
            ares = aA * aB + aC;
            break;
        }
        if (a->rgb_op == R300_US_RGB_SOP) {
            res[0] = res[1] = res[2] = ares;
        }

        for (n = 0; n < 3; n++) {
            res[n] = us_clamp01(us_omod(res[n], a->rgb_omod), a->rgb_clamp);
        }
        ares = us_clamp01(us_omod(ares, a->a_omod), a->a_clamp);

        for (n = 0; n < 3; n++) {
            if (a->rgb_wmask & (1u << n)) {
                g->r[a->rgb_dst][n] = res[n];
            }
            if (a->rgb_omask & (1u << n)) {
                g->out[n] = res[n];
            }
        }
        if (a->a_wmask) {
            g->r[a->a_dst][3] = ares;
        }
        if (a->a_omask) {
            g->out[3] = ares;
        }
    }
}
