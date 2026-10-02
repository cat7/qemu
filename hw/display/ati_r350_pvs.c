/*
 * ATI R300/R350 programmable vertex shader (PVS) interpreter.
 *
 * Mac OS X's accelerator paints the desktop with a vertex program that is
 * nothing but a 4x4 matrix multiply, which is why approximating every
 * program by that matrix rendered the whole compositor correctly. An
 * application's own program is a different animal: Chess.app uploads seven,
 * the longest thirty instructions, and its board's vertices carry a
 * position and a normal and no colour at all -- the colour a board square
 * is painted with is a lighting term the program computes.
 *
 * Opcode semantics here are transcribed from the R5xx Acceleration guide's
 * vertex-shader chapter, which documents the R300 instruction set. Two
 * details of it are easy to get wrong and were: the math engine reads only
 * the *w* channel of its sources, and its third source operand disappears
 * when PVS_DST_DUAL_MATH_OP is set, becoming a second, math-engine
 * instruction encoded in that word. A disassembler that does not know
 * about the second one cannot see the reciprocal square roots that every
 * lighting program in the corpus normalises its vectors with.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include <float.h>
#include "ati_r350_pvs.h"

static inline float r300_pvs_f32(uint32_t v)
{
    union { uint32_t u; float f; } c = { .u = v };
    return c.f;
}

void r300_pvs_out_layout(uint32_t fmt0, unsigned *first_color,
                         unsigned *ncolor, unsigned *first_texcoord)
{
    unsigned n = 0, i;

    for (i = 0; i < 4; i++) {
        if (fmt0 & (2u << i)) {
            n++;
        }
    }
    *first_color = (fmt0 & 1) ? 1 : 0;
    *ncolor = n;
    *first_texcoord = *first_color + n;
}

void r300_pvs_const(const R300PvsProgram *p, unsigned off, float v[4])
{
    unsigned idx = (p->cbase + off) * 4, c;

    /*
     * PVS_MAX_CONST_ADDR is the highest constant the current shader may
     * name; the hardware returns (0,0,0,0) above it. Honouring it is what
     * keeps a program from reading constants a previous one left behind --
     * the constant file is RAM and nothing else clears it.
     */
    if ((p->bounded && off > p->cmax) || idx + 4 > p->const_slots * 4) {
        v[0] = v[1] = v[2] = v[3] = 0.0f;
        return;
    }
    for (c = 0; c < 4; c++) {
        v[c] = r300_pvs_f32(p->cnst[idx + c]);
    }
}

/* an operand's ADDR_MODE, MODE_1 the msb and MODE_0 the lsb */
static unsigned r300_pvs_addr_mode(bool mode1, bool mode0)
{
    return (mode1 ? 2 : 0) | (mode0 ? 1 : 0);
}

static void r300_pvs_addr_gap(R300PvsGaps *gaps, unsigned mode)
{
    if (gaps && !gaps->has_addr_mode) {
        gaps->has_addr_mode = true;
        gaps->addr_mode = mode;
    }
}

/*
 * A register read relative to the address register. The constant file
 * returns (0,0,0,0) for an address outside 0..PVS_MAX_CONST_ADDR (R5xx
 * guide 7.5); the other files are given the same answer for an address
 * outside the file.
 */
static void r300_pvs_read_rel(const R300PvsProgram *p, const R300PvsRegs *r,
                              unsigned type, int idx, float v[4])
{
    switch (type) {
    case R300_PVS_SRC_REG_INPUT:
        if (idx >= 0 && idx < R300_PVS_IN_REGS) {
            memcpy(v, r->in[idx], 4 * sizeof(float));
        }
        break;
    case R300_PVS_SRC_REG_CONSTANT:
        if (idx >= 0) {
            r300_pvs_const(p, idx, v);
        }
        break;
    case R300_PVS_SRC_REG_ALT_TEMP:
        if (idx >= 0 && idx < R300_PVS_ATMP_REGS) {
            memcpy(v, r->atmp[idx], 4 * sizeof(float));
        }
        break;
    default:
        if (idx >= 0 && idx < R300_PVS_TMP_REGS) {
            memcpy(v, r->tmp[idx], 4 * sizeof(float));
        }
        break;
    }
}

/*
 * An ordinary source operand: register file, swizzle, negate, abs.
 * False, with the gap recorded, for an addressing mode not modelled.
 */
static bool r300_pvs_src(const R300PvsProgram *p, const R300PvsRegs *r,
                         uint32_t dw, float out[4], R300PvsGaps *gaps)
{
    unsigned type = dw & R300_PVS_SRC_REG_TYPE_MASK;
    unsigned off = (dw >> R300_PVS_SRC_OFFSET_SHIFT) &
                   R300_PVS_SRC_OFFSET_MASK;
    unsigned mode = r300_pvs_addr_mode(dw & R300_PVS_SRC_ADDR_MODE_1,
                                       dw & R300_PVS_SRC_ADDR_MODE_0);
    unsigned asel = (dw >> R300_PVS_SRC_ADDR_SEL_SHIFT) &
                    R300_PVS_SRC_ADDR_SEL_MASK;
    float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    unsigned c;

    if (mode == R300_PVS_ADDR_RELATIVE_A0) {
        r300_pvs_read_rel(p, r, type, (int)off + r->a0[asel], v);
    } else if (mode != R300_PVS_ADDR_ABSOLUTE) {
        r300_pvs_addr_gap(gaps, mode);
        return false;
    } else {
        switch (type) {
        case R300_PVS_SRC_REG_INPUT:
            memcpy(v, r->in[off % R300_PVS_IN_REGS], sizeof(v));
            break;
        case R300_PVS_SRC_REG_CONSTANT:
            r300_pvs_const(p, off, v);
            break;
        case R300_PVS_SRC_REG_ALT_TEMP:
            memcpy(v, r->atmp[off % R300_PVS_ATMP_REGS], sizeof(v));
            break;
        default:
            memcpy(v, r->tmp[off % R300_PVS_TMP_REGS], sizeof(v));
            break;
        }
    }

    for (c = 0; c < 4; c++) {
        unsigned sel = (dw >> (R300_PVS_SRC_SWIZZLE_SHIFT + 3 * c)) &
                       R300_PVS_SRC_SWIZZLE_MASK;
        float f;

        if (sel < 4) {
            f = v[sel];
        } else {
            f = sel == R300_PVS_SRC_SELECT_FORCE_1 ? 1.0f : 0.0f;
        }
        if (dw & R300_PVS_SRC_ABS_XYZW) {
            f = fabsf(f);
        }
        if ((dw >> (R300_PVS_SRC_MODIFIER_SHIFT + c)) & 1) {
            f = -f;
        }
        out[c] = f;
    }
    return true;
}

/*
 * The math engine, given the three source vectors of the instruction. It
 * only ever looks at their w channels; the compiler is expected to have
 * replicated the last meaningful operand into the ones an opcode does not
 * use, so a single-source op reading in_a.w and a three-source one reading
 * in_c.w can be written the way the guide writes them.
 */
static bool r300_pvs_math(unsigned opcode, const float a[4], const float b[4],
                          const float c[4], float res[4])
{
    float x = a[3], y;

    switch (opcode) {
    case R300_ME_LIGHT_COEFF_DX:
        /*
         * The lighting coefficients, and the only math opcode whose four
         * channels differ: the diffuse term is the clamped n.l in b.w, the
         * specular term the n.h in a.w raised to the exponent in c.w, and
         * it is suppressed entirely on a surface facing away from the
         * light. Chess.app runs this once per vertex of every lit piece.
         */
        res[0] = 1.0f;
        res[1] = MAX(b[3], 0.0f);
        if (b[3] > 0.0f) {
            res[2] = powf(MAX(a[3], 0.0f), MIN(MAX(c[3], -128.0f), 128.0f));
        } else {
            res[2] = 0.0f;
        }
        res[3] = 1.0f;
        return true;
    case R300_ME_EXP_BASE2_DX:
        res[0] = exp2f(floorf(x));
        res[1] = x > 128.0f ? 0.0f : x - floorf(x);
        res[2] = exp2f(x);
        res[3] = 1.0f;
        return true;
    case R300_ME_LOG_BASE2_DX:
        if (x == 0.0f) {
            res[0] = res[2] = -FLT_MAX;
            res[1] = res[3] = 1.0f;
        } else {
            int e;

            res[1] = fabsf(frexpf(x, &e)) * 2.0f;   /* mantissa, 1.0-2.0 */
            res[0] = (float)(e - 1);
            res[2] = log2f(fabsf(x));
            res[3] = 1.0f;
        }
        return true;
    case R300_ME_RECIP_DX:
        y = x != 0.0f ? 1.0f / x : FLT_MAX;
        break;
    case R300_ME_RECIP_FF:
        y = x != 0.0f ? 1.0f / x : 0.0f;
        break;
    case R300_ME_RECIP_SQRT_DX:
        y = x != 0.0f ? 1.0f / sqrtf(fabsf(x)) : FLT_MAX;
        break;
    case R300_ME_RECIP_SQRT_FF:
        y = x != 0.0f ? 1.0f / sqrtf(fabsf(x)) : 0.0f;
        break;
    case R300_ME_MULTIPLY:
        y = x * b[3];
        break;
    case R300_ME_POWER_FUNC_FF:
        /* base in a.w, exponent in b.w; a negative base keeps its sign */
        y = powf(fabsf(x), b[3]);
        if (x < 0.0f) {
            y = -y;
        }
        break;
    case R300_ME_EXP_BASE2_FULL_DX:
        y = exp2f(x);
        break;
    case R300_ME_LOG_BASE2_FULL_DX:
        y = x != 0.0f ? log2f(fabsf(x)) : -FLT_MAX;
        break;
    default:
        return false;
    }
    res[0] = res[1] = res[2] = res[3] = y;
    return true;
}

static bool r300_pvs_vector(unsigned opcode, const float a[4],
                            const float b[4], const float c[4], float res[4])
{
    unsigned i;

    switch (opcode) {
    case R300_VE_DOT_PRODUCT:
        res[0] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        res[1] = res[2] = res[3] = res[0];
        return true;
    case R300_VE_MULTIPLY:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] * b[i];
        }
        return true;
    case R300_VE_ADD:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] + b[i];
        }
        return true;
    case R300_VE_MULTIPLY_ADD:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] * b[i] + c[i];
        }
        return true;
    case R300_VE_MULTIPLYX2_ADD:
        for (i = 0; i < 4; i++) {
            res[i] = 2.0f * (a[i] * b[i]) + c[i];
        }
        return true;
    case R300_VE_DISTANCE_VECTOR:
        res[0] = 1.0f;
        res[1] = a[1] * b[1];
        res[2] = a[2];
        res[3] = b[3];
        return true;
    case R300_VE_FRACTION:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] - floorf(a[i]);
        }
        return true;
    case R300_VE_MAXIMUM:
        for (i = 0; i < 4; i++) {
            res[i] = MAX(a[i], b[i]);
        }
        return true;
    case R300_VE_MINIMUM:
        for (i = 0; i < 4; i++) {
            res[i] = MIN(a[i], b[i]);
        }
        return true;
    case R300_VE_SET_GREATER_THAN_EQUAL:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] >= b[i] ? 1.0f : 0.0f;
        }
        return true;
    case R300_VE_SET_LESS_THAN:
        for (i = 0; i < 4; i++) {
            res[i] = a[i] < b[i] ? 1.0f : 0.0f;
        }
        return true;
    case R300_VE_MULTIPLY_CLAMP:
        /* point-size clamp: one scalar, replicated */
        if (c[3] < a[3] * b[3]) {
            res[0] = c[3];
        } else if (c[0] >= a[0] * b[0]) {
            res[0] = c[0];
        } else {
            res[0] = a[0] * b[0];
        }
        res[1] = res[2] = res[3] = res[0];
        return true;
    case R300_VE_FLT2FIX_DX:
        /* the address-register load: floor() of each component */
        for (i = 0; i < 4; i++) {
            res[i] = floorf(a[i]);
        }
        return true;
    case R300_VE_FLT2FIX_DX_RND:
        /* ... and its rounding form, floor(x + 0.5) */
        for (i = 0; i < 4; i++) {
            res[i] = floorf(a[i] + 0.5f);
        }
        return true;
    default:
        return false;
    }
}

/*
 * The math-engine half of a dual-issue instruction. Its operand vector is
 * a single register with only two swizzled channels, which stand in for
 * the w channels the math engine would otherwise read.
 */
static void r300_pvs_dual_math(const R300PvsProgram *p, R300PvsRegs *r,
                               uint32_t dw, R300PvsGaps *gaps)
{
    unsigned opcode = ((dw >> R300_PVS_DUAL_OPCODE_SHIFT) &
                       R300_PVS_DUAL_OPCODE_MASK) |
                      ((dw & R300_PVS_DUAL_OPCODE_MSB) ? 16 : 0);
    unsigned doff = (dw >> R300_PVS_DUAL_DST_OFF_SHIFT) &
                    R300_PVS_DUAL_DST_OFF_MASK;
    unsigned we = (dw >> R300_PVS_DUAL_WE_SEL_SHIFT) &
                  R300_PVS_DUAL_WE_SEL_MASK;
    float src[4], a[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float b[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float res[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    unsigned c;

    if (opcode == R300_ME_NO_OP) {
        return;
    }
    /*
     * The operand word keeps the ordinary register, swizzle-x/y, abs and
     * negate-x/y fields, but bits 19-24 -- where a full operand would hold
     * the z and w swizzles -- carry this instruction's destination and
     * opcode instead. Clear them so the shared decoder reads the two
     * channels that do exist and nothing else, then present each as a w
     * channel, which is all the math engine ever reads.
     */
    if (!r300_pvs_src(p, r, dw & ~(0x3fu << 19), src, gaps)) {
        return;
    }
    a[3] = src[0];
    b[3] = src[1];
    if (!r300_pvs_math(opcode, a, b, b, res)) {
        if (gaps && !gaps->has_math_op) {
            gaps->has_math_op = true;
            gaps->math_op = opcode;
        }
        return;
    }
    c = we;
    r->atmp[doff][c] = res[c];
}

/*
 * The register a destination writes in a file of `n`: the offset itself,
 * or relative to the address register, where a register outside the file
 * is not written at all. -1 for that.
 */
static int r300_pvs_dst_slot(unsigned mode, unsigned doff, int32_t a0,
                             unsigned n)
{
    int idx;

    if (mode == R300_PVS_ADDR_ABSOLUTE) {
        return doff % n;
    }
    idx = (int)doff + a0;
    return idx >= 0 && idx < (int)n ? idx : -1;
}

void r300_pvs_run(const R300PvsProgram *p, R300PvsRegs *r, R300PvsGaps *gaps)
{
    unsigned i;

    if (!p->valid) {
        return;
    }
    for (i = p->first; i <= p->last; i++) {
        const uint32_t *w = &p->code[i * 4];
        uint32_t op = w[0];
        unsigned opcode = op & R300_PVS_DST_OPCODE_MASK;
        bool math = op & R300_PVS_DST_MATH_INST;
        bool dual = op & R300_PVS_DST_DUAL_MATH_OP;
        unsigned dtype = (op >> R300_PVS_DST_REG_TYPE_SHIFT) &
                         R300_PVS_DST_REG_TYPE_MASK;
        unsigned doff = (op >> R300_PVS_DST_OFFSET_SHIFT) &
                        R300_PVS_DST_OFFSET_MASK;
        unsigned we = (op >> R300_PVS_DST_WE_SHIFT) & R300_PVS_DST_WE_MASK;
        float a[4], b[4], c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float res[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        unsigned dmode = r300_pvs_addr_mode(op & R300_PVS_DST_ADDR_MODE_1,
                                            op & R300_PVS_DST_ADDR_MODE_0);
        unsigned dsel = (op >> R300_PVS_DST_ADDR_SEL_SHIFT) &
                        R300_PVS_DST_ADDR_SEL_MASK;
        float *dst;
        int idx;
        unsigned k;

        if (!r300_pvs_src(p, r, w[1], a, gaps) ||
            !r300_pvs_src(p, r, w[2], b, gaps)) {
            continue;
        }
        if (dual) {
            r300_pvs_dual_math(p, r, w[3], gaps);
        } else if (!r300_pvs_src(p, r, w[3], c, gaps)) {
            continue;
        }

        if (op & R300_PVS_DST_MACRO_INST) {
            /*
             * The macro bit only ever marks a multiply-add whose three
             * temporaries the hardware has to read in two passes; the
             * arithmetic is the plain one, with opcode 1 selecting the
             * doubling form.
             */
            for (k = 0; k < 4; k++) {
                res[k] = a[k] * b[k] * (opcode ? 2.0f : 1.0f) + c[k];
            }
        } else if (math) {
            if (opcode == R300_ME_NO_OP) {
                continue;
            }
            if (!r300_pvs_math(opcode, a, b, c, res)) {
                if (gaps && !gaps->has_math_op) {
                    gaps->has_math_op = true;
                    gaps->math_op = opcode;
                }
                continue;
            }
        } else {
            if (opcode == R300_VE_NO_OP) {
                continue;
            }
            if (!r300_pvs_vector(opcode, a, b, c, res)) {
                if (gaps && !gaps->has_vec_op) {
                    gaps->has_vec_op = true;
                    gaps->vec_op = opcode;
                }
                continue;
            }
        }

        if (op & (math ? R300_PVS_DST_ME_SAT : R300_PVS_DST_VE_SAT)) {
            for (k = 0; k < 4; k++) {
                res[k] = MIN(MAX(res[k], 0.0f), 1.0f);
            }
        }

        if (dtype == R300_PVS_DST_REG_A0 && !math &&
            !(op & R300_PVS_DST_MACRO_INST) &&
            (opcode == R300_VE_FLT2FIX_DX ||
             opcode == R300_VE_FLT2FIX_DX_RND)) {
            /*
             * The only writers of A0 (R5xx guide 7.5.9): the integral
             * result, clamped to -256..255. The register is a single
             * vector, so the destination offset and addressing mode are
             * meaningless here.
             */
            for (k = 0; k < 4; k++) {
                if (we & (1u << k)) {
                    float f = res[k] > R300_PVS_ADDR_MIN ?
                              (res[k] < R300_PVS_ADDR_MAX ?
                               res[k] : R300_PVS_ADDR_MAX) :
                              R300_PVS_ADDR_MIN;

                    r->a0[k] = (int32_t)f;
                }
            }
            continue;
        }

        if (dmode != R300_PVS_ADDR_ABSOLUTE &&
            dmode != R300_PVS_ADDR_RELATIVE_A0) {
            r300_pvs_addr_gap(gaps, dmode);
            continue;
        }

        switch (dtype) {
        case R300_PVS_DST_REG_OUT:
        case R300_PVS_DST_REG_OUT_REPL_X:
            idx = r300_pvs_dst_slot(dmode, doff, r->a0[dsel],
                                    R300_PVS_OUT_REGS);
            if (idx < 0) {
                continue;
            }
            dst = r->out[idx];
            r->out_written |= 1u << idx;
            break;
        case R300_PVS_DST_REG_TEMPORARY:
            idx = r300_pvs_dst_slot(dmode, doff, r->a0[dsel],
                                    R300_PVS_TMP_REGS);
            if (idx < 0) {
                continue;
            }
            dst = r->tmp[idx];
            break;
        case R300_PVS_DST_REG_ALT_TEMP:
            idx = r300_pvs_dst_slot(dmode, doff, r->a0[dsel],
                                    R300_PVS_ATMP_REGS);
            if (idx < 0) {
                continue;
            }
            dst = r->atmp[idx];
            break;
        default:
            /*
             * Writing the input file back is a shader-model-3 trick, and
             * A0 is written only by the float-to-fixed loads above;
             * either would give a wrong answer silently rather than an
             * approximate one.
             */
            if (gaps && !gaps->has_dst_file) {
                gaps->has_dst_file = true;
                gaps->dst_file = dtype;
            }
            continue;
        }
        for (k = 0; k < 4; k++) {
            if (we & (1u << k)) {
                dst[k] = dtype == R300_PVS_DST_REG_OUT_REPL_X ?
                         res[0] : res[k];
            }
        }
    }
}

/* is this operand register file `type` index `off`, read straight through? */
static bool r300_pvs_src_is(uint32_t dw, unsigned type, unsigned off)
{
    unsigned c;

    if ((dw & R300_PVS_SRC_REG_TYPE_MASK) != type ||
        ((dw >> R300_PVS_SRC_OFFSET_SHIFT) & R300_PVS_SRC_OFFSET_MASK) != off ||
        (dw & (R300_PVS_SRC_ABS_XYZW | R300_PVS_SRC_ADDR_MODE_0 |
               R300_PVS_SRC_ADDR_MODE_1))) {
        return false;
    }
    for (c = 0; c < 4; c++) {
        if (((dw >> (R300_PVS_SRC_SWIZZLE_SHIFT + 3 * c)) &
             R300_PVS_SRC_SWIZZLE_MASK) != c ||
            ((dw >> (R300_PVS_SRC_MODIFIER_SHIFT + c)) & 1)) {
            return false;
        }
    }
    return true;
}

/* is this operand the constant vector (1,1,1,1) the swizzler can force? */
static bool r300_pvs_src_is_one(uint32_t dw)
{
    unsigned c;

    for (c = 0; c < 4; c++) {
        if (((dw >> (R300_PVS_SRC_SWIZZLE_SHIFT + 3 * c)) &
             R300_PVS_SRC_SWIZZLE_MASK) != R300_PVS_SRC_SELECT_FORCE_1 ||
            ((dw >> (R300_PVS_SRC_MODIFIER_SHIFT + c)) & 1)) {
            return false;
        }
    }
    return true;
}

/*
 * One row of a texture-coordinate matrix:
 *
 *     out[o].<x|y|z|w> = const[c + row] . in[k]
 *
 * read straight through on both operands. The row is the write enable,
 * which is how the four instructions of a 4x4 are told apart, and it is
 * the same shape the position matrix is recognised by a few lines below
 * -- with the constant's index free rather than pinned to the row, since
 * a texture matrix sits wherever the compiler put it in the file.
 */
static bool r300_pvs_texmat_row(const uint32_t *w, unsigned *out,
                                unsigned *row, unsigned *in, unsigned *cbase)
{
    uint32_t op = w[0];
    unsigned dtype = (op >> R300_PVS_DST_REG_TYPE_SHIFT) &
                     R300_PVS_DST_REG_TYPE_MASK;
    unsigned we = (op >> R300_PVS_DST_WE_SHIFT) & R300_PVS_DST_WE_MASK;
    unsigned k = (w[2] >> R300_PVS_SRC_OFFSET_SHIFT) &
                 R300_PVS_SRC_OFFSET_MASK;
    unsigned c = (w[1] >> R300_PVS_SRC_OFFSET_SHIFT) &
                 R300_PVS_SRC_OFFSET_MASK;

    if ((op & R300_PVS_DST_OPCODE_MASK) != R300_VE_DOT_PRODUCT ||
        (op & (R300_PVS_DST_MATH_INST | R300_PVS_DST_MACRO_INST |
               R300_PVS_DST_DUAL_MATH_OP | R300_PVS_DST_PRED_ENABLE |
               R300_PVS_DST_VE_SAT | R300_PVS_DST_ME_SAT |
               R300_PVS_DST_ADDR_MODE_0 | R300_PVS_DST_ADDR_MODE_1)) ||
        (dtype != R300_PVS_DST_REG_OUT &&
         dtype != R300_PVS_DST_REG_OUT_REPL_X) ||
        (we != 1 && we != 2 && we != 4 && we != 8) ||
        k >= R300_PVS_IN_REGS ||
        !r300_pvs_src_is(w[2], R300_PVS_SRC_REG_INPUT, k) ||
        !r300_pvs_src_is(w[1], R300_PVS_SRC_REG_CONSTANT, c)) {
        return false;
    }
    *out = (op >> R300_PVS_DST_OFFSET_SHIFT) & R300_PVS_DST_OFFSET_MASK;
    *row = we == 1 ? 0 : we == 2 ? 1 : we == 4 ? 2 : 3;
    *in = k;
    *cbase = c - *row;
    return true;
}

bool r300_pvs_texmat(const R300PvsProgram *p, unsigned out,
                     R300PvsTexMat *tm)
{
    unsigned rows = 0, in = 0, cbase = 0, i, r;

    if (!r300_pvs_computes(p, out)) {
        return false;
    }
    for (i = p->first; i <= p->last; i++) {
        unsigned o, row, k, c;

        if (!r300_pvs_texmat_row(&p->code[i * 4], &o, &row, &k, &c) ||
            o != out) {
            continue;
        }
        if (rows && (k != in || c != cbase)) {
            return false;       /* two different matrices on one output */
        }
        in = k;
        cbase = c;
        rows |= 1u << row;
    }
    if (rows != 0xf) {
        return false;
    }
    tm->in = in;
    for (r = 0; r < 4; r++) {
        r300_pvs_const(p, cbase + r, tm->m[r]);
    }
    return true;
}

void r300_pvs_analyse(R300PvsProgram *p, const uint32_t *code,
                      const uint32_t *slot_valid, unsigned code_slots,
                      const uint32_t *cnst, unsigned const_slots,
                      uint32_t code_cntl, uint32_t const_cntl,
                      unsigned first_texcoord)
{
    unsigned matrix_rows = 0, i;
    bool plain;

    memset(p, 0, sizeof(*p));
    for (i = 0; i < R300_PVS_OUT_REGS; i++) {
        p->out_src[i] = -1;
    }
    p->code = code;
    p->cnst = cnst;
    p->code_slots = code_slots;
    p->const_slots = const_slots;
    p->first = code_cntl & 0x3ff;
    p->last = (code_cntl >> 20) & 0x3ff;
    p->cbase = const_cntl & 0xff;
    p->cmax = (const_cntl >> 16) & 0xff;
    p->bounded = const_cntl != 0;

    if (p->last < p->first || p->last >= code_slots) {
        return;
    }
    /*
     * Program RAM is written a slot at a time and never cleared, so the
     * bounds alone do not say the instructions are the guest's: a range
     * reaching past what has been uploaded would execute whatever the last
     * program left there. That is what made an earlier attempt at this
     * depend on the upload history -- the same draw rendering differently
     * according to what had run before it.
     */
    for (i = p->first; i <= p->last; i++) {
        if (!((slot_valid[i / 32] >> (i % 32)) & 1)) {
            return;
        }
    }
    p->valid = true;

    plain = true;
    for (i = p->first; i <= p->last; i++) {
        const uint32_t *w = &p->code[i * 4];
        uint32_t op = w[0];
        unsigned opcode = op & R300_PVS_DST_OPCODE_MASK;
        unsigned dtype = (op >> R300_PVS_DST_REG_TYPE_SHIFT) &
                         R300_PVS_DST_REG_TYPE_MASK;
        unsigned doff = (op >> R300_PVS_DST_OFFSET_SHIFT) &
                        R300_PVS_DST_OFFSET_MASK;
        unsigned we = (op >> R300_PVS_DST_WE_SHIFT) & R300_PVS_DST_WE_MASK;
        bool is_out = dtype == R300_PVS_DST_REG_OUT ||
                      dtype == R300_PVS_DST_REG_OUT_REPL_X;

        if (is_out && (op & (R300_PVS_DST_ADDR_MODE_0 |
                             R300_PVS_DST_ADDR_MODE_1))) {
            /* a relative destination may land on any output */
            p->out_mask |= (1u << R300_PVS_OUT_REGS) - 1;
        } else if (is_out && doff < R300_PVS_OUT_REGS) {
            p->out_mask |= 1u << doff;
            /* out[n] = in[k] * (1,1,1,1): an attribute forwarded intact */
            if (opcode == R300_VE_MULTIPLY && we == 0xf &&
                !(op & (R300_PVS_DST_MATH_INST | R300_PVS_DST_MACRO_INST |
                        R300_PVS_DST_DUAL_MATH_OP | R300_PVS_DST_PRED_ENABLE |
                        R300_PVS_DST_VE_SAT)) &&
                r300_pvs_src_is_one(w[2]) &&
                r300_pvs_src_is(w[1], R300_PVS_SRC_REG_INPUT,
                                (w[1] >> R300_PVS_SRC_OFFSET_SHIFT) &
                                R300_PVS_SRC_OFFSET_MASK) &&
                (((w[1] >> R300_PVS_SRC_OFFSET_SHIFT) &
                  R300_PVS_SRC_OFFSET_MASK) < R300_PVS_IN_REGS)) {
                p->out_src[doff] = (w[1] >> R300_PVS_SRC_OFFSET_SHIFT) &
                                   R300_PVS_SRC_OFFSET_MASK;
            }
        }
        if (!plain) {
            continue;
        }
        if (op & (R300_PVS_DST_MATH_INST | R300_PVS_DST_MACRO_INST |
                  R300_PVS_DST_DUAL_MATH_OP | R300_PVS_DST_PRED_ENABLE |
                  R300_PVS_DST_VE_SAT | R300_PVS_DST_ME_SAT |
                  R300_PVS_DST_ADDR_MODE_0 | R300_PVS_DST_ADDR_MODE_1) ||
            !is_out) {
            plain = false;
            continue;
        }
        if (opcode == R300_VE_DOT_PRODUCT && doff == 0 &&
            (we == 1 || we == 2 || we == 4 || we == 8) &&
            r300_pvs_src_is(w[2], R300_PVS_SRC_REG_INPUT, 0)) {
            unsigned row = we == 1 ? 0 : we == 2 ? 1 : we == 4 ? 2 : 3;

            if (r300_pvs_src_is(w[1], R300_PVS_SRC_REG_CONSTANT, row)) {
                matrix_rows |= 1u << row;
                continue;
            }
        }
        if (doff && p->out_src[doff] >= 0) {
            continue;               /* a forwarded attribute, recorded above */
        }
        /*
         * An instruction landing on a texture-coordinate output does not
         * make the POSITION matrix wrong, so it does not decide `plain`
         * here -- but it is not "beside the point" either, which is what
         * the comment that used to sit here said. It was checked against
         * Mac OS X 10.4, whose blit program multiplies its coordinate by
         * exactly diag(1/w, 1/h, 1, 1) -- the inverse of this model's own
         * attribute scaling, so ignoring the instruction was free.
         * Mac OS X 10.5 sends the same shape over a vertex that puts the
         * coordinate somewhere the fixed positional read does not look,
         * and there ignoring it costs the whole coordinate. The loop
         * below decides `plain` for these: a computed coordinate keeps
         * the fast path only while r300_pvs_texmat() can hand the caller
         * the matrix to apply.
         */
        if (doff >= first_texcoord) {
            continue;
        }
        plain = false;
    }
    for (i = first_texcoord; i < R300_PVS_OUT_REGS; i++) {
        R300PvsTexMat tm;

        if (r300_pvs_computes(p, i) && !r300_pvs_texmat(p, i, &tm)) {
            plain = false;
        }
    }
    p->plain_matrix = plain && matrix_rows == 0xf;
}
