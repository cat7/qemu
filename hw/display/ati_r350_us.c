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

/*
 * THE SPECIALISED PATH.
 *
 * Every program in this project's corpus is one ALU slot of MAD on both
 * banks, and running a general interpreter over a thirty-two-register
 * frame for each pixel cost the software rasterizer 38 % of its frame
 * rate. So a program of exactly that shape has its six arguments
 * resolved here, once per draw, into loads from the three things the
 * rasterizer actually produces -- the texel and two colours -- plus the
 * constant file.
 *
 * The conditions are deliberately narrow and every one of them is a
 * thing the fast executor does not do: one instruction, MAD on both
 * banks, no input or output modifier, no clamp, no pre-subtract, and
 * nothing written to the frame (so no instruction can read what another
 * wrote). Anything else runs the interpreter, which is still the
 * specification -- and the offline harness requires the two to agree bit
 * for bit over the whole corpus.
 */
static bool us_arg_fast(const R300UsProgram *p, R300UsArgFast *out,
                        unsigned reg, bool is_const, unsigned sel,
                        bool alpha)
{
    unsigned k;

    /* the literals, common to both banks */
    if (alpha ? (sel >= 16 && sel <= 18) : (sel >= 20 && sel <= 22)) {
        static const float lit[3] = { 0.0f, 1.0f, 0.5f };

        out->src = R300_US_ARG_LIT;
        out->lit = lit[sel - (alpha ? 16 : 20)];
        return true;
    }
    /* which slot, and which component of it */
    if (alpha) {
        if (sel < 9) {
            out->chan = 1 + (sel % 3);
        } else if (sel < 12) {
            out->chan = 4;
        } else {
            return false;               /* the pre-subtract */
        }
    } else {
        if (sel < 12) {
            out->chan = sel % 4;        /* 0 = .rgb, 1..3 = r/g/b */
        } else if (sel < 15) {
            out->chan = 4;              /* .aaa */
        } else {
            return false;               /* srcp, or a rotate */
        }
    }
    if (is_const) {
        out->src = R300_US_ARG_CONST;
        out->ki = reg;
        return true;
    }
    if (p->tex_dst >= 0 && reg == (unsigned)p->tex_dst) {
        out->src = R300_US_ARG_TEX;
        return true;
    }
    for (k = 0; k < R300_US_RS_COLS; k++) {
        if (p->rs.col_reg[k] >= 0 && reg == (unsigned)p->rs.col_reg[k]) {
            out->src = p->rs.col_pkt[k] == 0 ? R300_US_ARG_COL0
                                             : R300_US_ARG_COL1;
            return true;
        }
    }
    /*
     * A register nothing filled. The interpreter reads a cleared frame
     * and gets zero, so a literal zero is the same answer -- and saying
     * so here is what keeps the two paths identical rather than merely
     * close.
     */
    out->src = R300_US_ARG_LIT;
    out->lit = 0.0f;
    return true;
}

static void us_try_fast(R300UsProgram *p)
{
    const R300UsAlu *a = &p->alu[0];
    unsigned n;

    /*
     * A non-identity output select is the interpreter's job. Putting it
     * in the specialised executor costs a per-pixel branch in every
     * guest for a construct only one of them uses; see `out_permuted`.
     *
     * `gl_simple` is required for the same reason it gates the
     * translator: the specialised executor is handed a texel the caller
     * sampled, so it cannot run a program whose fetch depends on an
     * earlier level, fetches more than once, or kills the fragment.
     */
    if (p->out_permuted || !p->gl_simple) {
        return;
    }

    p->fast = false;
    if (!p->valid || !p->expressible || !p->writes_out || p->nalu != 1) {
        return;
    }
    if (a->rgb_op != R300_US_RGB_MAD || a->a_op != R300_US_A_MAD) {
        return;
    }
    if (a->rgb_omod || a->a_omod || a->rgb_clamp || a->a_clamp) {
        return;
    }
    if (a->rgb_wmask || a->a_wmask) {
        return;                         /* it writes the frame as well */
    }
    for (n = 0; n < 3; n++) {
        if (a->rgb_mod[n] != R300_US_MOD_NOP ||
            a->a_mod[n] != R300_US_MOD_NOP) {
            return;
        }
    }
    for (n = 0; n < 3; n++) {
        unsigned rs = a->rgb_sel[n] < 12 ? a->rgb_sel[n] / 4
                                         : a->rgb_sel[n] - 12;
        unsigned as = a->a_sel[n] < 9 ? a->a_sel[n] / 3 : a->a_sel[n] - 9;

        if (a->rgb_sel[n] < 15) {
            if (!us_arg_fast(p, &p->fast_rgb[n], a->rgb_src[rs],
                             a->rgb_src_const[rs], a->rgb_sel[n], false)) {
                return;
            }
        } else if (!us_arg_fast(p, &p->fast_rgb[n], 0, false,
                                a->rgb_sel[n], false)) {
            return;
        }
        if (a->a_sel[n] < 12) {
            if (!us_arg_fast(p, &p->fast_a[n], a->a_src[as],
                             a->a_src_const[as], a->a_sel[n], true)) {
                return;
            }
        } else if (!us_arg_fast(p, &p->fast_a[n], 0, false,
                                a->a_sel[n], true)) {
            return;
        }
    }
    p->fast_rgb_mask = a->rgb_omask;
    p->fast_a_out = a->a_omask;
    p->fast = true;
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
    unsigned nfetch = 0;
    unsigned i, lv;

    memset(p, 0, sizeof(*p));
    p->tex_dst = -1;
    p->rs.tex_reg = -1;
    p->rs.col_reg[0] = p->rs.col_reg[1] = -1;

    /*
     * US_CONFIG.NLEVEL names the live indirection levels, counting DOWN
     * from level 3 (R3xx/R5xx reference, US_CONFIG): 0 = level 3 only,
     * the DX7-style single pass; 1 = levels 2 and 3, DX8-style bump
     * mapping; 2 = levels 1 to 3; 3 = all four. They execute in
     * ascending order, so an earlier level's ALU result is what a later
     * level's texture fetch can be addressed by.
     */
    if (nlevel >= R300_US_LEVELS) {
        /*
         * NLEVEL is three bits and only 0-3 are defined; 4-7 are
         * reserved. Refusing them is not pedantry -- `3 - nlevel` is
         * how a level indexes US_CODE_ADDR, and it underflows here.
         */
        us_gap_indirect(&p->gaps, nlevel);
        return;
    }
    p->nlevels = nlevel + 1;
    p->nregs = (us_pixsize & 0x1f) + 1;

    for (lv = 0; lv < p->nlevels; lv++) {
        uint32_t ca = us_code_addr[3 - nlevel + lv];
        R300UsLevel *L = &p->level[lv];
        unsigned astart = (ca & R300_US_CA_ALU_START_MASK) + aoff;
        unsigned asize = ((ca >> R300_US_CA_ALU_SIZE_SHIFT) &
                          R300_US_CA_ALU_SIZE_MASK) + 1;
        unsigned tstart = ((ca >> R300_US_CA_TEX_START_SHIFT) &
                           R300_US_CA_TEX_START_MASK) + toff;
        unsigned tsize = ((ca >> R300_US_CA_TEX_SIZE_SHIFT) &
                          R300_US_CA_TEX_SIZE_MASK) + 1;

        /*
         * FIRST_TEX gates the texture code of the FIRST valid level, and
         * only that one -- the register's own wording. For NLEVEL 0 the
         * first valid level IS level 3, which is why this reads the same
         * as the single-level code it replaces.
         */
        if (lv == 0 && !(us_config & R300_US_CFG_FIRST_TEX)) {
            tsize = 0;
        }
        if (astart + asize > R300_US_ALU_SLOTS ||
            tstart + tsize > R300_US_TEX_SLOTS ||
            p->nalu + asize > R300_US_ALU_SLOTS ||
            p->ntex + tsize > R300_US_TEX_SLOTS) {
            return;             /* not valid: a range leaves the RAM */
        }
        L->alu_slot = astart;
        L->tex_slot = tstart;
        L->alu_at = p->nalu;
        L->nalu = asize;
        L->tex_at = p->ntex;
        L->ntex = tsize;
        p->nalu += asize;
        p->ntex += tsize;
    }
    p->alu_first = p->level[0].alu_slot;
    p->tex_first = p->level[0].tex_slot;
    p->valid = true;
    p->expressible = true;

    for (i = 0; i < R300_US_CONSTS; i++) {
        p->konst[i][0] = konst[i][0];
        p->konst[i][1] = konst[i][1];
        p->konst[i][2] = konst[i][2];
        p->konst[i][3] = konst[i][3];
    }

    for (lv = 0; lv < p->nlevels; lv++) {
        const R300UsLevel *L = &p->level[lv];

        for (i = 0; i < L->ntex; i++) {
            uint32_t v = tex_inst[L->tex_slot + i];
            R300UsTex *t = &p->tex[L->tex_at + i];

            t->op = (v >> R300_US_TEX_INST_SHIFT) & R300_US_TEX_INST_MASK;
            t->src = v & R300_US_TEX_SRC_MASK;
            t->dst = (v >> R300_US_TEX_DST_SHIFT) & R300_US_TEX_DST_MASK;
            t->unit = (v >> R300_US_TEX_ID_SHIFT) & R300_US_TEX_ID_MASK;
            switch (t->op) {
            case R300_US_TEXOP_NOP:
                break;
            case R300_US_TEXOP_TEXKILL:
                p->has_kill = true;
                break;
            case R300_US_TEXOP_LD:
            case R300_US_TEXOP_PROJ:
                /*
                 * The fetch itself happens in the executor, which reads
                 * the coordinate out of the frame register the
                 * instruction names -- so a second fetch, or one at a
                 * later level, needs nothing special here. Only a unit
                 * this model does not bind is a gap. `tex_dst` records
                 * the first unit-0 fetch for the specialised path and
                 * the GL translator, both of which take a texel that was
                 * sampled for them.
                 */
                if (t->unit != 0) {
                    us_gap_tex_op(&p->gaps, 0x10 | t->unit);
                    p->expressible = false;
                } else {
                    nfetch++;
                    if (p->tex_dst < 0) {
                        p->tex_dst = t->dst;
                    }
                }
                break;
            default:
                us_gap_tex_op(&p->gaps, t->op);
                p->expressible = false;
                break;
            }
        }

        for (i = 0; i < L->nalu; i++) {
            R300UsAlu *a = &p->alu[L->alu_at + i];
            unsigned k = L->alu_slot + i;

            us_decode_alu(a, rgb_addr[k], rgb_inst[k], a_addr[k], a_inst[k]);
            if (!us_rgb_op_known(a->rgb_op)) {
                us_gap_rgb_op(&p->gaps, a->rgb_op);
                p->expressible = false;
            }
            if (!us_a_op_known(a->a_op)) {
                us_gap_a_op(&p->gaps, a->a_op);
                p->expressible = false;
            }
            if (a->rgb_omask || a->a_omask) {
                p->writes_out = true;
            }
        }
    }
    p->gl_simple = p->nlevels == 1 && nfetch <= 1 && !p->has_kill;

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
    for (i = 0; i < p->ntex; i++) {
        const R300UsTex *t = &p->tex[i];

        if (t->op == R300_US_TEXOP_NOP) {
            continue;
        }
        if (t->src + 1u > p->nregs_used) {
            p->nregs_used = t->src + 1u;
        }
        if (t->op != R300_US_TEXOP_TEXKILL && t->dst + 1u > p->nregs_used) {
            p->nregs_used = t->dst + 1u;
        }
    }
    /* the register the rasterizer drops the interpolated coordinate in */
    if (p->rs.tex_reg >= 0 &&
        (unsigned)p->rs.tex_reg + 1u > p->nregs_used) {
        p->nregs_used = p->rs.tex_reg + 1u;
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
     * How the output fifo maps onto the render target's components.
     *
     * The FORMAT is still a refusal: C4_8 and C4_10 both deliver four
     * components the destination writer can store, and the others do
     * not. A program that writes no output at all never reaches the
     * target, so neither half is consulted for it.
     *
     * The SELECT is not a refusal any more -- it is resolved into a
     * permutation. See `out_perm` in the header for the derivation; the
     * short version is that the writer's fixed ARGB assembly IS the
     * select 0x1b, so inverting the guest's select against it expresses
     * every encoding and leaves 0x1b costing nothing.
     */
    p->out_perm[0] = 0;
    p->out_perm[1] = 1;
    p->out_perm[2] = 2;
    p->out_perm[3] = 3;
    if (p->writes_out) {
        /* which shader channel each of A,R,G,B names, in the writer's order */
        static const uint8_t chan_to_out[4] = { 3, 0, 1, 2 };
        /* which writer slot holds output component k */
        static const uint8_t comp_to_slot[4] = { 2, 1, 0, 3 };
        unsigned fmt = us_out_fmt0 & R300_US_OUT_FMT_MASK;
        unsigned k;

        if (fmt != R300_US_OUT_FMT_C4_8 && fmt != R300_US_OUT_FMT_C4_10) {
            us_gap_out_fmt(&p->gaps, fmt);
            p->expressible = false;
        }
        for (k = 0; k < 4; k++) {
            unsigned sel = (us_out_fmt0 >> R300_US_OUT_SEL_SHIFT(k)) &
                           R300_US_OUT_SEL_MASK;

            p->out_perm[comp_to_slot[k]] = chan_to_out[sel];
        }
        p->out_permuted = p->out_perm[0] != 0 || p->out_perm[1] != 1 ||
                          p->out_perm[2] != 2 || p->out_perm[3] != 3;
    }

    us_try_fast(p);
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

/*
 * One ALU slot, both banks. Lifted out of r300_us_run() when the
 * executor grew an outer loop over indirection levels: the arithmetic
 * is unchanged line for line, and keeping it at one indentation level
 * is what makes that checkable by eye.
 */
static void us_run_alu(const R300UsProgram *p, R300UsRegs *g,
                       const R300UsAlu *a)
{
    R300UsSrc s[3];
    float srcp[3], srcp_a;
    float A[3], B[3], C[3], res[3];
    float aA, aB, aC, ares;
    float dot;
    unsigned n;

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

void r300_us_run(const R300UsProgram *p, R300UsRegs *g,
                 R300UsSampleFn sample, void *ctx)
{
    unsigned i, n, lv;

    for (lv = 0; lv < p->nlevels; lv++) {
        const R300UsLevel *L = &p->level[lv];

        /*
         * The level's texture instructions, in program order and BEFORE
         * its ALU slots. That order is what an indirection level means:
         * the previous level's ALU has already written the frame, so a
         * fetch here can be addressed by its result.
         */
        for (i = 0; i < L->ntex; i++) {
            const R300UsTex *t = &p->tex[L->tex_at + i];

            switch (t->op) {
            case R300_US_TEXOP_TEXKILL:
                /*
                 * "Kill pixel if any component is < 0" -- the register
                 * reference's own wording, over the four components of
                 * the frame register the instruction names.
                 */
                if (g->r[t->src][0] < 0.0f || g->r[t->src][1] < 0.0f ||
                    g->r[t->src][2] < 0.0f || g->r[t->src][3] < 0.0f) {
                    g->kill = true;
                }
                break;
            case R300_US_TEXOP_LD:
            case R300_US_TEXOP_PROJ:
                if (sample) {
                    sample(ctx, t->unit, t->op == R300_US_TEXOP_PROJ,
                           g->r[t->src], g->r[t->dst]);
                }
                break;
            default:
                break;
            }
        }

        for (i = L->alu_at; i < L->alu_at + L->nalu; i++) {
            us_run_alu(p, g, &p->alu[i]);
        }
    }

    /*
     * US_OUT_FMT_0's component select, applied once at the end. The
     * output fifo is what the destination writer stores, so the
     * permutation belongs after the last instruction has written it and
     * not inside the loop, where an output component is also readable as
     * a source of a later slot.
     *
     * The identity is the guarded case, not an incidental one: every
     * program in this project's 10.4 and OS 9 corpus reads select 0x1b,
     * which IS the writer's own order, so the branch is not taken and
     * nothing moves.
     */
    if (p->out_permuted) {
        float o[4];

        for (n = 0; n < 4; n++) {
            o[n] = g->out[p->out_perm[n]];
        }
        for (n = 0; n < 4; n++) {
            g->out[n] = o[n];
        }
    }
}
