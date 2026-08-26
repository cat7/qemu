/*
 * ATI R300/R350 universal shader (US) -- GLSL translation.
 *
 * `ati_r350_us.c` is the executable specification of the fragment
 * instruction set and this file is a second implementation of it that
 * happens to be emitted as text. As in `ati_r350_pvs_glsl.c`, every
 * arithmetic decision is made to match that interpreter rather than to
 * be idiomatic GLSL: a multiply-add is an explicit fma() because the
 * host compiler contracts `A*B + C` into one; a dot product is written
 * in the association the compiler contracts it into rather than as
 * dot(); MIN and MAX are ternaries so a NaN operand takes the same
 * branch; and every value carries `precise` so nothing is re-associated
 * underneath. `precise` and fma() are GLSL 4.00 constructs, legal at
 * #version 330 core only with GL_ARB_gpu_shader5, which the caller's
 * shader prologue already requires.
 *
 * The translation refuses exactly what the interpreter refuses -- it is
 * gated on the same `expressible` flag -- so a caller that gets `false`
 * back renders that draw on the software path instead of wrongly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ati_r350_us.h"

typedef struct UsBuf {
    char *p;
    size_t len, cap;
    bool full;
} UsBuf;

static void G_GNUC_PRINTF(2, 3) us_emit(UsBuf *b, const char *fmt, ...)
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

/*
 * The three source SLOTS of one instruction, as GLSL expressions. A slot
 * is two halves: `rgb` names the register the RGB bank addressed and `a`
 * the alpha component of the register the ALPHA bank addressed, which is
 * why `srcN.aaa` on the RGB side can read a register the RGB bank never
 * mentions. Constants come from the uniform the caller uploads per draw,
 * so a program's translation does not change when its constants do.
 */
typedef struct UsSrcName {
    char rgb[24];
    char a[24];
} UsSrcName;

static void us_slot_names(UsSrcName s[3], const R300UsAlu *a)
{
    unsigned n;

    for (n = 0; n < 3; n++) {
        if (a->rgb_src_const[n]) {
            snprintf(s[n].rgb, sizeof(s[n].rgb), "USK[%u]", a->rgb_src[n]);
        } else {
            snprintf(s[n].rgb, sizeof(s[n].rgb), "R%u", a->rgb_src[n]);
        }
        if (a->a_src_const[n]) {
            snprintf(s[n].a, sizeof(s[n].a), "USK[%u]", a->a_src[n]);
        } else {
            snprintf(s[n].a, sizeof(s[n].a), "R%u", a->a_src[n]);
        }
    }
}

static void us_wrap_mod(char *out, size_t cap, const char *e, uint8_t mod)
{
    switch (mod) {
    case R300_US_MOD_NEG:
        snprintf(out, cap, "(-(%s))", e);
        break;
    case R300_US_MOD_ABS:
        snprintf(out, cap, "abs(%s)", e);
        break;
    case R300_US_MOD_NAB:
        snprintf(out, cap, "(-abs(%s))", e);
        break;
    default:
        snprintf(out, cap, "%s", e);
        break;
    }
}

/* one RGB-side argument, the PDF's selector table in its own order */
static void us_arg_rgb(char *out, size_t cap, const UsSrcName s[3],
                       uint8_t sel, uint8_t mod)
{
    char e[96];

    if (sel < 12) {
        static const char *const sw[4] = { "rgb", "rrr", "ggg", "bbb" };

        snprintf(e, sizeof(e), "%s.%s", s[sel / 4].rgb, sw[sel % 4]);
    } else if (sel < 15) {
        snprintf(e, sizeof(e), "%s.aaa", s[sel - 12].a);
    } else if (sel < 20) {
        static const char *const sw[5] = { "rgb", "rrr", "ggg", "bbb", "aaa" };

        snprintf(e, sizeof(e), "SRCP.%s", sw[sel - 15]);
    } else if (sel == 20) {
        snprintf(e, sizeof(e), "vec3(0.0)");
    } else if (sel == 21) {
        snprintf(e, sizeof(e), "vec3(1.0)");
    } else if (sel == 22) {
        snprintf(e, sizeof(e), "vec3(0.5)");
    } else if (sel < 32) {
        unsigned k = (sel - 23) % 3, w = (sel - 23) / 3;

        if (w == 2) {
            /* .abg: the alpha half supplies the first component */
            snprintf(e, sizeof(e), "vec3(%s.a, %s.b, %s.g)",
                     s[k].a, s[k].rgb, s[k].rgb);
        } else {
            snprintf(e, sizeof(e), "%s.%s", s[k].rgb, w == 0 ? "gbr" : "brg");
        }
    } else {
        snprintf(e, sizeof(e), "vec3(0.0)");
    }
    us_wrap_mod(out, cap, e, mod);
}

/* one alpha-side argument, the same table reduced to scalars */
static void us_arg_a(char *out, size_t cap, const UsSrcName s[3],
                     uint8_t sel, uint8_t mod)
{
    char e[96];

    if (sel < 9) {
        snprintf(e, sizeof(e), "%s.%c", s[sel / 3].rgb, "rgb"[sel % 3]);
    } else if (sel < 12) {
        snprintf(e, sizeof(e), "%s.a", s[sel - 9].a);
    } else if (sel < 16) {
        snprintf(e, sizeof(e), "SRCP.%c", "rgba"[sel - 12]);
    } else if (sel == 16) {
        snprintf(e, sizeof(e), "0.0");
    } else if (sel == 17) {
        snprintf(e, sizeof(e), "1.0");
    } else if (sel == 18) {
        snprintf(e, sizeof(e), "0.5");
    } else {
        snprintf(e, sizeof(e), "0.0");
    }
    us_wrap_mod(out, cap, e, mod);
}

/* the pre-subtract, both halves, in the interpreter's own expressions */
static void us_srcp(UsBuf *b, const UsSrcName s[3], const R300UsAlu *a)
{
    char e[128];

    switch (a->rgb_srcp_op) {
    case 0:
        snprintf(e, sizeof(e), "(vec3(1.0) - 2.0 * %s.rgb)", s[0].rgb);
        break;
    case 1:
        snprintf(e, sizeof(e), "(%s.rgb - %s.rgb)", s[1].rgb, s[0].rgb);
        break;
    case 2:
        snprintf(e, sizeof(e), "(%s.rgb + %s.rgb)", s[1].rgb, s[0].rgb);
        break;
    default:
        snprintf(e, sizeof(e), "(vec3(1.0) - %s.rgb)", s[0].rgb);
        break;
    }
    us_emit(b, "        precise vec4 SRCP;\n"
               "        SRCP.rgb = %s;\n", e);
    switch (a->a_srcp_op) {
    case 0:
        snprintf(e, sizeof(e), "(1.0 - 2.0 * %s.a)", s[0].a);
        break;
    case 1:
        snprintf(e, sizeof(e), "(%s.a - %s.a)", s[1].a, s[0].a);
        break;
    case 2:
        snprintf(e, sizeof(e), "(%s.a + %s.a)", s[1].a, s[0].a);
        break;
    default:
        snprintf(e, sizeof(e), "(1.0 - %s.a)", s[0].a);
        break;
    }
    us_emit(b, "        SRCP.a = %s;\n", e);
}

static void us_omod_clamp(UsBuf *b, const char *var, uint8_t omod, bool clamp)
{
    /* the output modifier's factor, in the interpreter's own order */
    static const char *const m[8] = {
        NULL, "2.0", "4.0", "8.0", "0.5", "0.25", "0.125", NULL
    };

    if (omod < 8 && m[omod]) {
        us_emit(b, "        %s = %s * %s;\n", var, var, m[omod]);
    }
    if (clamp) {
        us_emit(b, "        %s = clamp(%s, 0.0, 1.0);\n", var, var);
    }
}

bool r300_us_glsl(const R300UsProgram *p, char *buf, size_t cap)
{
    UsBuf b = { .p = buf, .cap = cap };
    unsigned i, n;

    /*
     * `gl_simple` is the shape this translation can express. The GL
     * backend samples the texture itself and passes `us_main()` a
     * finished texel, so a program that fetches mid-flight -- a second
     * indirection level, a second fetch, a TEXKILL -- has no texel to be
     * given. Those are REFUSED here rather than approximated, which
     * makes the draw fall back to the software rasterizer, where the
     * executor performs the fetches and renders them correctly. The
     * offload is what is given up, not the picture.
     */
    if (!p->valid || !p->expressible || !p->gl_simple) {
        return false;
    }
    us_emit(&b, "void us_main(vec4 tex0, vec4 col0, vec4 col1,\n"
                "             out vec4 outc)\n{\n");
    us_emit(&b, "    outc = vec4(0.0);\n");
    /*
     * Exactly the frame the interpreter clears, so an instruction naming
     * a register the guest never wrote reads the same zero in both.
     */
    for (n = 0; n < p->nregs_used; n++) {
        us_emit(&b, "    precise vec4 R%u = vec4(0.0);\n", n);
    }
    if (p->tex_dst >= 0 && (unsigned)p->tex_dst < p->nregs_used) {
        us_emit(&b, "    R%d = tex0;\n", p->tex_dst);
    }
    for (n = 0; n < R300_US_RS_COLS; n++) {
        if (p->rs.col_reg[n] >= 0 &&
            (unsigned)p->rs.col_reg[n] < p->nregs_used) {
            us_emit(&b, "    R%d = %s;\n", p->rs.col_reg[n],
                    p->rs.col_pkt[n] == 0 ? "col0" : "col1");
        }
    }

    for (i = 0; i < p->nalu; i++) {
        const R300UsAlu *a = &p->alu[i];
        UsSrcName s[3];
        char A[128], B[128], C[128], aA[128], aB[128], aC[128];

        us_slot_names(s, a);
        us_arg_rgb(A, sizeof(A), s, a->rgb_sel[0], a->rgb_mod[0]);
        us_arg_rgb(B, sizeof(B), s, a->rgb_sel[1], a->rgb_mod[1]);
        us_arg_rgb(C, sizeof(C), s, a->rgb_sel[2], a->rgb_mod[2]);
        us_arg_a(aA, sizeof(aA), s, a->a_sel[0], a->a_mod[0]);
        us_arg_a(aB, sizeof(aB), s, a->a_sel[1], a->a_mod[1]);
        us_arg_a(aC, sizeof(aC), s, a->a_sel[2], a->a_mod[2]);

        us_emit(&b, "    {\n");
        us_srcp(&b, s, a);
        us_emit(&b, "        precise vec3 A = %s;\n", A);
        us_emit(&b, "        precise vec3 B = %s;\n", B);
        us_emit(&b, "        precise vec3 C = %s;\n", C);
        us_emit(&b, "        precise float aA = %s;\n", aA);
        us_emit(&b, "        precise float aB = %s;\n", aB);
        us_emit(&b, "        precise float aC = %s;\n", aC);
        /*
         * The shared dot product: the RGB side's DP4 adds the alpha
         * arguments' product to it and the alpha side's DP is it. Left
         * to right with the first product rounded and the rest fused,
         * which is what `A[0]*B[0] + A[1]*B[1] + A[2]*B[2]` compiles to.
         */
        us_emit(&b, "        precise float DOT = "
                    "fma(A.z, B.z, fma(A.y, B.y, A.x * B.x));\n");
        us_emit(&b, "        precise vec3 res;\n"
                    "        precise float ares;\n");

        switch (a->rgb_op) {
        case R300_US_RGB_DP3:
            us_emit(&b, "        res = vec3(DOT);\n");
            break;
        case R300_US_RGB_DP4:
            us_emit(&b, "        res = vec3(fma(aA, aB, DOT));\n");
            break;
        case R300_US_RGB_D2A:
            us_emit(&b, "        res = vec3(fma(A.y, B.y, A.x * B.x)"
                        " + C.z);\n");
            break;
        case R300_US_RGB_MIN:
            us_emit(&b, "        res = vec3(A.x < B.x ? A.x : B.x,"
                        " A.y < B.y ? A.y : B.y, A.z < B.z ? A.z : B.z);\n");
            break;
        case R300_US_RGB_MAX:
            us_emit(&b, "        res = vec3(A.x > B.x ? A.x : B.x,"
                        " A.y > B.y ? A.y : B.y, A.z > B.z ? A.z : B.z);\n");
            break;
        case R300_US_RGB_CND:
            us_emit(&b, "        res = vec3(C.x > 0.5 ? A.x : B.x,"
                        " C.y > 0.5 ? A.y : B.y, C.z > 0.5 ? A.z : B.z);\n");
            break;
        case R300_US_RGB_CMP:
            us_emit(&b, "        res = vec3(C.x >= 0.0 ? A.x : B.x,"
                        " C.y >= 0.0 ? A.y : B.y, C.z >= 0.0 ? A.z : B.z);\n");
            break;
        case R300_US_RGB_FRC:
            us_emit(&b, "        res = A - floor(A);\n");
            break;
        case R300_US_RGB_SOP:
            us_emit(&b, "        res = vec3(0.0);\n");
            break;
        default:
            us_emit(&b, "        res = fma(A, B, C);\n");
            break;
        }

        switch (a->a_op) {
        case R300_US_A_DP:
            us_emit(&b, "        ares = DOT;\n");
            break;
        case R300_US_A_MIN:
            us_emit(&b, "        ares = aA < aB ? aA : aB;\n");
            break;
        case R300_US_A_MAX:
            us_emit(&b, "        ares = aA > aB ? aA : aB;\n");
            break;
        case R300_US_A_CND:
            us_emit(&b, "        ares = aC > 0.5 ? aA : aB;\n");
            break;
        case R300_US_A_CMP:
            us_emit(&b, "        ares = aC >= 0.0 ? aA : aB;\n");
            break;
        case R300_US_A_FRC:
            us_emit(&b, "        ares = aA - floor(aA);\n");
            break;
        case R300_US_A_EX2:
            us_emit(&b, "        ares = exp2(aA);\n");
            break;
        case R300_US_A_LN2:
            us_emit(&b, "        ares = aA > 0.0 ? log2(aA)"
                        " : -3.402823466e38;\n");
            break;
        case R300_US_A_RCP:
            us_emit(&b, "        ares = aA != 0.0 ? 1.0 / aA"
                        " : 3.402823466e38;\n");
            break;
        case R300_US_A_RSQ:
            us_emit(&b, "        ares = aA != 0.0 ? "
                        "inversesqrt(abs(aA)) : 3.402823466e38;\n");
            break;
        default:
            us_emit(&b, "        ares = fma(aA, aB, aC);\n");
            break;
        }
        if (a->rgb_op == R300_US_RGB_SOP) {
            us_emit(&b, "        res = vec3(ares);\n");
        }

        us_omod_clamp(&b, "res", a->rgb_omod, a->rgb_clamp);
        us_omod_clamp(&b, "ares", a->a_omod, a->a_clamp);

        for (n = 0; n < 3; n++) {
            if (a->rgb_wmask & (1u << n)) {
                us_emit(&b, "        R%u.%c = res.%c;\n", a->rgb_dst,
                        "rgb"[n], "xyz"[n]);
            }
            if (a->rgb_omask & (1u << n)) {
                us_emit(&b, "        outc.%c = res.%c;\n",
                        "rgb"[n], "xyz"[n]);
            }
        }
        if (a->a_wmask) {
            us_emit(&b, "        R%u.a = ares;\n", a->a_dst);
        }
        if (a->a_omask) {
            us_emit(&b, "        outc.a = ares;\n");
        }
        us_emit(&b, "    }\n");
    }
    /*
     * US_OUT_FMT_0's component select. A swizzle is the same shuffle of
     * finished values the two software paths perform at the same point,
     * so no arithmetic is reordered and the identity emits nothing.
     */
    if (p->out_permuted) {
        us_emit(&b, "    outc = outc.%c%c%c%c;\n",
                "rgba"[p->out_perm[0]], "rgba"[p->out_perm[1]],
                "rgba"[p->out_perm[2]], "rgba"[p->out_perm[3]]);
    }
    us_emit(&b, "}\n");
    return !b.full;
}
