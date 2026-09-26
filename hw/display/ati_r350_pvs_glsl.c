/*
 * ATI R300/R350 programmable vertex shader (PVS) -- GLSL translation.
 *
 * `ati_r350_pvs.c` is the executable specification of this instruction
 * set, and this file is a second implementation of it that happens to be
 * emitted as text. Every arithmetic decision here is made to match that
 * interpreter rather than to be idiomatic GLSL: the dot product is
 * written out in the association the C compiler contracts it into, a
 * multiply-add is an explicit fma(), MAX is a ternary rather than max()
 * so that a NaN operand takes the same branch, and every expression that
 * must not be re-associated carries `precise`. Both of those are GLSL
 * 4.00 constructs that Apple's compiler accepts at #version 330 core,
 * which is what the rest of this device's shaders already rely on.
 *
 * The translation refuses anything the interpreter would report as a gap
 * -- an unimplemented opcode, a destination register file it does not
 * model -- rather than emitting an approximation, so a caller that gets
 * `false` back falls back to the software path for that draw instead of
 * rendering it wrong.
 *
 * Two limits are shared with the interpreter deliberately, so that the
 * two agree by construction rather than by accident: relative addressing
 * (PVS_SRC_ADDR_MODE / PVS_DST_ADDR_MODE) is ignored in both, and the
 * math engine's transcendentals are the host's and the GPU's respective
 * best efforts at the same function, which cannot agree in the last bit.
 * The offline harness measures how far apart they land.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ati_r350_pvs.h"

typedef struct PvsBuf {
    char *p;
    size_t len, cap;
    bool full;
} PvsBuf;

static void G_GNUC_PRINTF(2, 3) pvs_emit(PvsBuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->full) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->full = true;
        return;
    }
    b->len += n;
}

/* the four channel letters a write mask names, e.g. 0xb -> "xyw" */
static const char *pvs_mask_swz(unsigned we, char out[5])
{
    unsigned c, n = 0;

    for (c = 0; c < 4; c++) {
        if (we & (1u << c)) {
            out[n++] = "xyzw"[c];
        }
    }
    out[n] = '\0';
    return out;
}

/* the same length, every letter x: what OUT_REPL_X broadcasts */
static const char *pvs_repl_swz(unsigned we, char out[5])
{
    unsigned c, n = 0;

    for (c = 0; c < 4; c++) {
        if (we & (1u << c)) {
            out[n++] = 'x';
        }
    }
    out[n] = '\0';
    return out;
}

/*
 * One source operand, as the interpreter builds it: pick the register,
 * apply the four 3-bit swizzle selectors (4 forces 0, 5 forces 1), then
 * the whole-vector absolute value, then the four per-channel negates.
 * The order matters -- an absolute value applied after a negate would
 * discard it -- and it is the interpreter's order.
 */
static void pvs_src(PvsBuf *b, const R300PvsProgram *p, uint32_t dw,
                    R300PvsGlsl *info)
{
    unsigned type = dw & R300_PVS_SRC_REG_TYPE_MASK;
    unsigned off = (dw >> R300_PVS_SRC_OFFSET_SHIFT) &
                   R300_PVS_SRC_OFFSET_MASK;
    char reg[32];
    unsigned c;

    switch (type) {
    case R300_PVS_SRC_REG_INPUT:
        snprintf(reg, sizeof(reg), "PVSA[%u]", off % R300_PVS_IN_REGS);
        info->in_mask |= 1u << (off % R300_PVS_IN_REGS);
        break;
    case R300_PVS_SRC_REG_CONSTANT:
        /*
         * PVSK is the constant file the program addresses, so the base
         * offset and PVS_MAX_CONST_ADDR are the host's business and are
         * already applied to what it uploads. A constant this program
         * cannot legally reach is a literal zero here, exactly as
         * r300_pvs_const() returns one.
         */
        if ((p->bounded && off > p->cmax) ||
            (p->cbase + off + 1) * 4 > p->const_slots * 4) {
            snprintf(reg, sizeof(reg), "vec4(0.0)");
        } else {
            snprintf(reg, sizeof(reg), "PVSK[%u]", off);
            if (off + 1 > info->nconst) {
                info->nconst = off + 1;
            }
        }
        break;
    case R300_PVS_SRC_REG_ALT_TEMP:
        snprintf(reg, sizeof(reg), "AT[%u]", off % R300_PVS_ATMP_REGS);
        break;
    default:
        snprintf(reg, sizeof(reg), "T[%u]", off % R300_PVS_TMP_REGS);
        break;
    }

    pvs_emit(b, "vec4(");
    for (c = 0; c < 4; c++) {
        unsigned sel = (dw >> (R300_PVS_SRC_SWIZZLE_SHIFT + 3 * c)) &
                       R300_PVS_SRC_SWIZZLE_MASK;
        bool neg = (dw >> (R300_PVS_SRC_MODIFIER_SHIFT + c)) & 1;
        char body[64];

        if (sel < 4) {
            snprintf(body, sizeof(body), "%s.%c", reg, "xyzw"[sel]);
        } else {
            snprintf(body, sizeof(body), "%s",
                     sel == R300_PVS_SRC_SELECT_FORCE_1 ? "1.0" : "0.0");
        }
        pvs_emit(b, "%s%s%s%s%s", c ? ", " : "", neg ? "-" : "",
                 (dw & R300_PVS_SRC_ABS_XYZW) ? "abs(" : "", body,
                 (dw & R300_PVS_SRC_ABS_XYZW) ? ")" : "");
    }
    pvs_emit(b, ")");
}

/*
 * The vector engine. Every form is written in the association the C
 * interpreter's own expression contracts into on this host, so that the
 * two differ by the transcendental library alone and not by rounding
 * order: a running sum picking up one fused multiply-add per term for
 * DOT_PRODUCT, a single fma() for the multiply-adds.
 */
static bool pvs_vector(PvsBuf *b, unsigned opcode)
{
    switch (opcode) {
    case R300_VE_DOT_PRODUCT:
        /*
         * The association is not the obvious one, and it is not a
         * choice: this is the code the C interpreter's own
         * `a0*b0 + a1*b1 + a2*b2 + a3*b3` compiles into on this host at
         * both -O1 and -O2 -- one rounded product for the SECOND term,
         * which the first term's fused multiply-add then accumulates
         * into, and the remaining two fused on top. Writing the chain
         * the other way round rounds a different product and moves the
         * result by an ULP.
         */
        pvs_emit(b, "    r = vec4(fma(a.w, bb.w, fma(a.z, bb.z,"
                    " fma(a.x, bb.x, a.y * bb.y))));\n");
        return true;
    case R300_VE_MULTIPLY:
        pvs_emit(b, "    r = a * bb;\n");
        return true;
    case R300_VE_ADD:
        pvs_emit(b, "    r = a + bb;\n");
        return true;
    case R300_VE_MULTIPLY_ADD:
        pvs_emit(b, "    r = fma(a, bb, cc);\n");
        return true;
    case R300_VE_MULTIPLYX2_ADD:
        pvs_emit(b, "    r = fma(vec4(2.0), a * bb, cc);\n");
        return true;
    case R300_VE_DISTANCE_VECTOR:
        pvs_emit(b, "    r = vec4(1.0, a.y * bb.y, a.z, bb.w);\n");
        return true;
    case R300_VE_FRACTION:
        pvs_emit(b, "    r = a - floor(a);\n");
        return true;
    case R300_VE_MAXIMUM:
        pvs_emit(b, "    r = vec4(a.x > bb.x ? a.x : bb.x,"
                    " a.y > bb.y ? a.y : bb.y,\n"
                    "             a.z > bb.z ? a.z : bb.z,"
                    " a.w > bb.w ? a.w : bb.w);\n");
        return true;
    case R300_VE_MINIMUM:
        pvs_emit(b, "    r = vec4(a.x < bb.x ? a.x : bb.x,"
                    " a.y < bb.y ? a.y : bb.y,\n"
                    "             a.z < bb.z ? a.z : bb.z,"
                    " a.w < bb.w ? a.w : bb.w);\n");
        return true;
    case R300_VE_SET_GREATER_THAN_EQUAL:
        pvs_emit(b, "    r = vec4(greaterThanEqual(a, bb));\n");
        return true;
    case R300_VE_SET_LESS_THAN:
        pvs_emit(b, "    r = vec4(lessThan(a, bb));\n");
        return true;
    case R300_VE_MULTIPLY_CLAMP:
        pvs_emit(b, "    r = vec4(cc.w < a.w * bb.w ? cc.w :\n"
                    "             cc.x >= a.x * bb.x ? cc.x :"
                    " a.x * bb.x);\n");
        return true;
    default:
        return false;
    }
}

/*
 * The math engine. It reads only the w channel of its operands (R5xx
 * guide 7.5.8) and replicates its scalar result across all four, except
 * for the three opcodes whose channels genuinely differ.
 *
 * `pvs_powf` stands in for powf() over the domain LIGHT_COEFF_DX uses.
 * GLSL leaves pow(0, y) undefined where C defines it, and the two
 * libraries do not agree in the last bit anywhere; the harness reports
 * how far apart they land rather than pretending they do.
 */
static bool pvs_math(PvsBuf *b, unsigned opcode)
{
    const char *y = NULL;

    switch (opcode) {
    case R300_ME_LIGHT_COEFF_DX:
        pvs_emit(b, "    r = vec4(1.0, bb.w > 0.0 ? bb.w : 0.0,\n"
                    "             bb.w > 0.0 ? pvs_powf(a.w > 0.0 ?"
                    " a.w : 0.0,\n"
                    "                 clamp(cc.w, -128.0, 128.0)) : 0.0,"
                    " 1.0);\n");
        return true;
    case R300_ME_EXP_BASE2_DX:
        pvs_emit(b, "    r = vec4(exp2(floor(a.w)),\n"
                    "             a.w > 128.0 ? 0.0 : a.w - floor(a.w),\n"
                    "             exp2(a.w), 1.0);\n");
        return true;
    case R300_ME_LOG_BASE2_DX:
        /*
         * frexpf's mantissa scaled to [1,2) and its exponent less one are
         * floor(log2|x|) and |x| over that power of two; GLSL 3.30 has no
         * frexp, and this identity is what the interpreter's frexpf call
         * computes.
         */
        pvs_emit(b, "    if (a.w == 0.0) {\n"
                    "        r = vec4(-3.402823466e38, 1.0,"
                    " -3.402823466e38, 1.0);\n"
                    "    } else {\n"
                    "        float e = floor(log2(abs(a.w)));\n"
                    "        r = vec4(e, abs(a.w) / exp2(e),"
                    " log2(abs(a.w)), 1.0);\n"
                    "    }\n");
        return true;
    case R300_ME_RECIP_DX:
        y = "a.w != 0.0 ? 1.0 / a.w : 3.402823466e38";
        break;
    case R300_ME_RECIP_FF:
        y = "a.w != 0.0 ? 1.0 / a.w : 0.0";
        break;
    case R300_ME_RECIP_SQRT_DX:
        y = "a.w != 0.0 ? 1.0 / sqrt(abs(a.w)) : 3.402823466e38";
        break;
    case R300_ME_RECIP_SQRT_FF:
        y = "a.w != 0.0 ? 1.0 / sqrt(abs(a.w)) : 0.0";
        break;
    case R300_ME_MULTIPLY:
        y = "a.w * bb.w";
        break;
    case R300_ME_EXP_BASE2_FULL_DX:
        y = "exp2(a.w)";
        break;
    case R300_ME_LOG_BASE2_FULL_DX:
        y = "a.w != 0.0 ? log2(abs(a.w)) : -3.402823466e38";
        break;
    default:
        return false;
    }
    pvs_emit(b, "    r = vec4(%s);\n", y);
    return true;
}

/*
 * The math-engine half of a dual-issue instruction. Its word keeps the
 * ordinary register, swizzle-x/y, abs and negate-x/y fields, but the bits
 * a full operand would spend on the z and w swizzles carry this
 * instruction's destination and opcode instead. Clearing them and reading
 * the result's first two channels as two w channels is exactly what the
 * interpreter does, and for the same reason.
 */
static bool pvs_dual_math(PvsBuf *b, const R300PvsProgram *p, uint32_t dw,
                          R300PvsGlsl *info)
{
    unsigned opcode = ((dw >> R300_PVS_DUAL_OPCODE_SHIFT) &
                       R300_PVS_DUAL_OPCODE_MASK) |
                      ((dw & R300_PVS_DUAL_OPCODE_MSB) ? 16 : 0);
    unsigned doff = (dw >> R300_PVS_DUAL_DST_OFF_SHIFT) &
                    R300_PVS_DUAL_DST_OFF_MASK;
    unsigned we = (dw >> R300_PVS_DUAL_WE_SEL_SHIFT) &
                  R300_PVS_DUAL_WE_SEL_MASK;

    if (opcode == R300_ME_NO_OP) {
        return true;
    }
    pvs_emit(b, "    {\n"
                "    precise vec4 s = ");
    pvs_src(b, p, dw & ~(0x3fu << 19), info);
    pvs_emit(b, ";\n"
                "    precise vec4 a = vec4(s.x), bb = vec4(s.y),"
                " cc = bb, r;\n");
    if (!pvs_math(b, opcode)) {
        return false;
    }
    pvs_emit(b, "    AT[%u].%c = r.%c;\n    }\n", doff, "xyzw"[we],
             "xyzw"[we]);
    return true;
}

void r300_pvs_glsl_consts(const R300PvsProgram *p, float *k, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        r300_pvs_const(p, i, &k[i * 4]);
    }
}

bool r300_pvs_glsl(const R300PvsProgram *p, char *buf, size_t cap,
                   R300PvsGlsl *info)
{
    R300PvsGlsl local;
    PvsBuf b = { .p = buf, .cap = cap };
    unsigned i;

    if (!info) {
        info = &local;
    }
    memset(info, 0, sizeof(*info));
    buf[0] = '\0';
    if (!p->valid) {
        return false;
    }

    /*
     * powf() over the domain LIGHT_COEFF_DX uses. GLSL's own pow() is
     * undefined for a zero or negative base, where C's is defined and
     * this instruction set relies on it.
     */
    pvs_emit(&b, "float pvs_powf(float x, float y)\n"
                 "{\n"
                 "    if (x > 0.0) { return pow(x, y); }\n"
                 "    return y == 0.0 ? 1.0 : 0.0;\n"
                 "}\n\n");

    pvs_emit(&b, "void pvs_main()\n{\n"
                 "    vec4 T[%u], AT[%u];\n"
                 "    for (int i = 0; i < %u; i++) { T[i] = vec4(0.0); }\n"
                 "    for (int i = 0; i < %u; i++) { AT[i] = vec4(0.0); }\n",
             R300_PVS_TMP_REGS, R300_PVS_ATMP_REGS,
             R300_PVS_TMP_REGS, R300_PVS_ATMP_REGS);

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
        char dstsw[5], srcsw[5];
        const char *file;

        pvs_emit(&b, "    /* %u */\n    {\n"
                     "    precise vec4 a = ", i);
        pvs_src(&b, p, w[1], info);
        pvs_emit(&b, ";\n    precise vec4 bb = ");
        pvs_src(&b, p, w[2], info);
        pvs_emit(&b, ";\n    precise vec4 cc = ");
        if (dual) {
            pvs_emit(&b, "vec4(0.0)");
        } else {
            pvs_src(&b, p, w[3], info);
        }
        pvs_emit(&b, ";\n    precise vec4 r = vec4(0.0);\n");

        /*
         * The dual-issued math instruction is evaluated between the
         * operand reads and the vector instruction's writeback, which is
         * where the interpreter puts it: it can therefore see an
         * alternate temporary the vector half is about to overwrite, and
         * cannot see the value the vector half writes.
         */
        if (dual && !pvs_dual_math(&b, p, w[3], info)) {
            info->gaps.has_math_op = true;
            info->gaps.math_op = ((w[3] >> R300_PVS_DUAL_OPCODE_SHIFT) &
                                  R300_PVS_DUAL_OPCODE_MASK) |
                                 ((w[3] & R300_PVS_DUAL_OPCODE_MSB) ? 16 : 0);
            return false;
        }

        if (op & R300_PVS_DST_MACRO_INST) {
            /* the two-pass multiply-add, opcode 1 selecting the doubling */
            pvs_emit(&b, "    r = fma(a * bb, vec4(%s), cc);\n",
                     opcode ? "2.0" : "1.0");
        } else if (math) {
            if (opcode == R300_ME_NO_OP) {
                pvs_emit(&b, "    }\n");
                continue;
            }
            if (!pvs_math(&b, opcode)) {
                info->gaps.has_math_op = true;
                info->gaps.math_op = opcode;
                return false;
            }
        } else {
            if (opcode == R300_VE_NO_OP) {
                pvs_emit(&b, "    }\n");
                continue;
            }
            if (!pvs_vector(&b, opcode)) {
                info->gaps.has_vec_op = true;
                info->gaps.vec_op = opcode;
                return false;
            }
        }

        if (op & (math ? R300_PVS_DST_ME_SAT : R300_PVS_DST_VE_SAT)) {
            pvs_emit(&b, "    r = clamp(r, 0.0, 1.0);\n");
        }

        switch (dtype) {
        case R300_PVS_DST_REG_OUT:
        case R300_PVS_DST_REG_OUT_REPL_X:
            file = "PVSo";
            doff %= R300_PVS_OUT_REGS;
            info->out_mask |= 1u << doff;
            break;
        case R300_PVS_DST_REG_TEMPORARY:
            file = "T";
            doff %= R300_PVS_TMP_REGS;
            break;
        case R300_PVS_DST_REG_ALT_TEMP:
            file = "AT";
            doff %= R300_PVS_ATMP_REGS;
            break;
        default:
            /*
             * The address register and a write back into the input file
             * are the two the interpreter refuses; refusing them here as
             * well is what keeps the translated program and the
             * interpreted one the same program.
             */
            info->gaps.has_dst_file = true;
            info->gaps.dst_file = dtype;
            return false;
        }
        if (we) {
            pvs_mask_swz(we, dstsw);
            if (dtype == R300_PVS_DST_REG_OUT_REPL_X) {
                pvs_repl_swz(we, srcsw);
            } else {
                pvs_mask_swz(we, srcsw);
            }
            pvs_emit(&b, "    %s[%u].%s = r.%s;\n", file, doff, dstsw, srcsw);
        }
        pvs_emit(&b, "    }\n");
    }
    pvs_emit(&b, "}\n");

    return !b.full;
}
