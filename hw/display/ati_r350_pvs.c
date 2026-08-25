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

/* an ordinary source operand: register file, swizzle, negate, abs */
static void r300_pvs_src(const R300PvsProgram *p, const R300PvsRegs *r,
                         uint32_t dw, float out[4])
{
    unsigned type = dw & R300_PVS_SRC_REG_TYPE_MASK;
    unsigned off = (dw >> R300_PVS_SRC_OFFSET_SHIFT) &
                   R300_PVS_SRC_OFFSET_MASK;
    float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    unsigned c;

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
    r300_pvs_src(p, r, dw & ~(0x3fu << 19), src);
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
        float *dst;
        unsigned k;

        r300_pvs_src(p, r, w[1], a);
        r300_pvs_src(p, r, w[2], b);
        if (dual) {
            r300_pvs_dual_math(p, r, w[3], gaps);
        } else {
            r300_pvs_src(p, r, w[3], c);
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

        switch (dtype) {
        case R300_PVS_DST_REG_OUT:
        case R300_PVS_DST_REG_OUT_REPL_X:
            dst = r->out[doff % R300_PVS_OUT_REGS];
            r->out_written |= 1u << (doff % R300_PVS_OUT_REGS);
            break;
        case R300_PVS_DST_REG_TEMPORARY:
            dst = r->tmp[doff % R300_PVS_TMP_REGS];
            break;
        case R300_PVS_DST_REG_ALT_TEMP:
            dst = r->atmp[doff % R300_PVS_ATMP_REGS];
            break;
        default:
            /*
             * The address register drives the relative addressing this
             * interpreter does not model, and writing the input file back
             * is a shader-model-3 trick; either would give a wrong answer
             * silently rather than an approximate one.
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

        if (is_out && doff < R300_PVS_OUT_REGS) {
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
         * Anything landing on a texture-coordinate output is beside the
         * point: this model interpolates texture coordinates from the
         * vertex attributes and does not read them back out of the
         * program, so such an instruction cannot make the matrix wrong.
         */
        if (doff >= first_texcoord) {
            continue;
        }
        plain = false;
    }
    p->plain_matrix = plain && matrix_rows == 0xf;
}
