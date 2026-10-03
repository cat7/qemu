/*
 * translate-fp.c
 *
 * Standard FPU translation
 */

static inline void gen_reset_fpstatus(void)
{
    gen_helper_reset_fpstatus(tcg_env);
}

static inline void gen_compute_fprf_float64(TCGv_i64 arg)
{
    gen_helper_compute_fprf_float64(tcg_env, arg);
    gen_helper_float_check_status(tcg_env);
}

#if defined(TARGET_PPC64)
static void gen_set_cr1_from_fpscr(DisasContext *ctx)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_trunc_tl_i32(tmp, cpu_fpscr);
    tcg_gen_shri_i32(cpu_crf[1], tmp, 28);
}
#else
static void gen_set_cr1_from_fpscr(DisasContext *ctx)
{
    tcg_gen_shri_tl(cpu_crf[1], cpu_fpscr, 28);
}
#endif

/***                       Floating-Point arithmetic                       ***/

/*
 * Inline host FP (fparith_vec) for the A-form arithmetic and fcmpu, in
 * TBs with HFLAGS_FP_FAST.  The result, FPRF, FI, XX and FX
 * are those of the helper sequence; everything else branches to that
 * sequence, emitted after the TB's exit, which then leaves the TB.
 * PPC_NATIVE_FP=0 turns it off, PPC_NATIVE_FP_RELAXED=1 leaves FI, XX
 * and FX alone in the arithmetic, PPC_NATIVE_FCMP=0 keeps the helpers
 * for fcmpu.
 */
enum { NFP_AB, NFP_ACB, NFP_CMPU };

static bool native_fcmp_on;

static bool native_env(const char *name, bool dflt)
{
    const char *v = getenv(name);

    return v && *v ? *v != '0' : dflt;
}

static void native_fp_init(void)
{
#ifndef CONFIG_USER_ONLY
    if (!tcg_can_emit_fparith() || !native_env("PPC_NATIVE_FP", true) ||
        native_env("PPC_STRICT_FP", false)) {
        return;
    }
    ppc_native_fp = native_env("PPC_NATIVE_FP_RELAXED", false)
                    ? PPC_NATIVE_FP_RELAXED : PPC_NATIVE_FP_EXACT;
    tcg_set_fparith_ref(ppc_fparith_ref);
    native_fcmp_on = native_env("PPC_NATIVE_FCMP", true);
#endif
}

static bool native_fp(DisasContext *ctx)
{
    return ctx->fp_fast && ctx->n_nfp < ARRAY_SIZE(ctx->nfp)
           && !ctx->singlestep_flags && !ctx->pmu_insn_cnt
           && !(tb_cflags(ctx->base.tb) & CF_USE_ICOUNT);
}

/* mtfs* changing FPSCR[0:7] may change HFLAGS_FP_FAST */
static void native_fp_end_tb(DisasContext *ctx, uint32_t nibbles)
{
    if (ppc_native_fp && (nibbles & 3) && ctx->base.is_jmp == DISAS_NEXT) {
        ctx->base.is_jmp = DISAS_CHAIN_UPDATE;
    }
}

static TCGLabel *native_fp_slow(DisasContext *ctx, int kind, void *helper,
                                int frt, int x, int y, int z)
{
    NativeFPSlow *sp = &ctx->nfp[ctx->n_nfp++];

    *sp = (NativeFPSlow) {
        .label = gen_new_label(), .next = ctx->base.pc_next,
        .helper = helper, .kind = kind,
        .frt = frt, .x = x, .y = y, .z = z,
    };
    return sp->label;
}

static TCGv_vec native_fpr(int regno)
{
    TCGv_vec v = tcg_temp_new_vec(TCG_TYPE_V64);

    tcg_gen_ld_vec(v, tcg_env, fpr_offset(regno));
    return v;
}

/* FPRF by class and sign: 5 bits per entry, sign + 2 * class */
#define NATIVE_FPRF(cls, neg, fprf) \
    ((uint64_t)(fprf) << (FPSCR_FPRF + 5 * ((cls) * 2 + (neg))))
static const uint64_t native_fprf =
    NATIVE_FPRF(FPARITH_CLASS_NORMAL, 0, 0x04) |
    NATIVE_FPRF(FPARITH_CLASS_NORMAL, 1, 0x08) |
    NATIVE_FPRF(FPARITH_CLASS_ZERO, 0, 0x02) |
    NATIVE_FPRF(FPARITH_CLASS_ZERO, 1, 0x12) |
    NATIVE_FPRF(FPARITH_CLASS_SUBNORMAL, 0, 0x14) |
    NATIVE_FPRF(FPARITH_CLASS_SUBNORMAL, 1, 0x18) |
    NATIVE_FPRF(FPARITH_CLASS_INF, 0, 0x05) |
    NATIVE_FPRF(FPARITH_CLASS_INF, 1, 0x09) |
    NATIVE_FPRF(FPARITH_CLASS_QNAN, 0, 0x11) |
    NATIVE_FPRF(FPARITH_CLASS_QNAN, 1, 0x11);

/* FPSCR from the status: FPRF; FI, XX and FX unless relaxed */
static void native_fp_fpscr(TCGv_i64 st, bool relaxed)
{
    TCGv_i64 f = tcg_temp_new_i64();
    TCGv_i64 sh = tcg_temp_new_i64();
    TCGv t = tcg_temp_new();
    target_ulong mask = FP_FPRF;

    QEMU_BUILD_BUG_ON(FPARITH_ST_NEG != 2 || FPARITH_ST_CLASS != 2);
    tcg_gen_extract_i64(sh, st, 1, 4);
    tcg_gen_shli_i64(f, sh, 2);
    tcg_gen_add_i64(sh, sh, f);
    tcg_gen_shr_i64(f, tcg_constant_i64(native_fprf), sh);
    tcg_gen_andi_i64(f, f, FP_FPRF);
    if (!relaxed) {
        TCGv_i64 ix = tcg_temp_new_i64();

        tcg_gen_andi_i64(ix, st, FPARITH_ST_INEXACT);
        tcg_gen_neg_i64(ix, ix);
        tcg_gen_andi_i64(ix, ix, FP_FI | FP_XX | FP_FX);
        tcg_gen_or_i64(f, f, ix);
        mask |= FP_FI;
    }
    tcg_gen_trunc_i64_tl(t, f);
    tcg_gen_andi_tl(cpu_fpscr, cpu_fpscr, ~mask);
    tcg_gen_or_tl(cpu_fpscr, cpu_fpscr, t);
}

/* frt = op(x, y[, z]) for an FPARITH_* desc; x, y, z as the helper's */
static void native_fp_op(DisasContext *ctx, unsigned desc, int kind,
                         void *helper, int frt, int x, int y, int z)
{
    bool relaxed = ppc_native_fp == PPC_NATIVE_FP_RELAXED;
    TCGLabel *slow = native_fp_slow(ctx, kind, helper, frt, x, y, z);
    TCGv_vec r = tcg_temp_new_vec(TCG_TYPE_V64);
    TCGv_i64 st = tcg_temp_new_i64();
    TCGv_vec vx = native_fpr(x);
    TCGv_vec vy = y >= 0 ? native_fpr(y) : vx;
    TCGv_vec vz = z >= 0 ? native_fpr(z) : vx;

    if (!relaxed) {
        desc |= FPARITH_INEXACT;
    }
    if (kind == NFP_ACB) {
        tcg_gen_fparith_vec(desc, r, st, vx, vy, vz);
    } else {
        tcg_gen_fparith_vec(desc, r, st, vx, vx, vy);
    }
    tcg_gen_brcondi_i64(TCG_COND_TSTNE, st, FPARITH_ST_OFF, slow);
    tcg_gen_st_vec(r, tcg_env, fpr_offset(frt));
    tcg_gen_st_i64(tcg_constant_i64(0), tcg_env, vsr64_offset(frt, false));
    native_fp_fpscr(st, relaxed);
}

static int native_fp_desc(void *helper)
{
    static const struct { void *h; int desc; } map[] = {
        { gen_helper_FADD, FPARITH_ADD },
        { gen_helper_FADDS, FPARITH_ADD | FPARITH_SINGLE },
        { gen_helper_FSUB, FPARITH_SUB },
        { gen_helper_FSUBS, FPARITH_SUB | FPARITH_SINGLE },
        { gen_helper_FMUL, FPARITH_MUL },
        { gen_helper_FMULS, FPARITH_MUL | FPARITH_SINGLE },
        { gen_helper_FDIV, FPARITH_DIV },
        { gen_helper_FDIVS, FPARITH_DIV | FPARITH_SINGLE },
        { gen_helper_FMADD, FPARITH_MADD },
        { gen_helper_FMADDS, FPARITH_MADD | FPARITH_SINGLE },
        { gen_helper_FMSUB, FPARITH_MSUB },
        { gen_helper_FMSUBS, FPARITH_MSUB | FPARITH_SINGLE },
        { gen_helper_FNMADD, FPARITH_NMADD },
        { gen_helper_FNMADDS, FPARITH_NMADD | FPARITH_SINGLE },
        { gen_helper_FNMSUB, FPARITH_NMSUB },
        { gen_helper_FNMSUBS, FPARITH_NMSUB | FPARITH_SINGLE },
    };

    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].h == helper) {
            return map[i].desc;
        }
    }
    return -1;
}

static bool native_fp_try(DisasContext *ctx, void *helper, bool rc, int kind,
                          int frt, int x, int y, int z)
{
    int desc;

    if (rc || !native_fp(ctx)) {
        return false;
    }
    desc = native_fp_desc(helper);
    if (desc < 0) {
        return false;
    }
    native_fp_op(ctx, desc, kind, helper, frt, x, y, z);
    return true;
}

/*
 * fcmpu without NaNs: CR field and FPCC 8, 4 or 2, FI cleared.  The
 * doubles' bits become signed keys ordered like the doubles, -0 == +0.
 */
static bool native_fcmpu(DisasContext *ctx, arg_X_bf *a)
{
    TCGLabel *slow;
    TCGv_i64 k[2], m = tcg_temp_new_i64(), c = tcg_temp_new_i64();
    TCGv t = tcg_temp_new();

    if (!native_fcmp_on || !native_fp(ctx)) {
        return false;
    }
    slow = native_fp_slow(ctx, NFP_CMPU, gen_helper_FCMPU, a->bf,
                          a->ra, a->rb, -1);
    for (int i = 0; i < 2; i++) {
        TCGv_i64 v = tcg_temp_new_i64(), s = tcg_temp_new_i64();

        get_fpr(v, i ? a->rb : a->ra);
        tcg_gen_shli_i64(s, v, 1);
        if (i) {
            tcg_gen_umax_i64(m, m, s);
        } else {
            tcg_gen_mov_i64(m, s);
        }
        tcg_gen_sari_i64(s, v, 63);
        tcg_gen_andi_i64(v, v, INT64_MAX);
        tcg_gen_xor_i64(v, v, s);
        tcg_gen_sub_i64(v, v, s);
        k[i] = v;
    }
    tcg_gen_brcondi_i64(TCG_COND_GTU, m, 0xffe0000000000000ull, slow);
    tcg_gen_movcond_i64(TCG_COND_GT, c, k[0], k[1],
                        tcg_constant_i64(4), tcg_constant_i64(2));
    tcg_gen_movcond_i64(TCG_COND_LT, c, k[0], k[1],
                        tcg_constant_i64(8), c);
    tcg_gen_extrl_i64_i32(cpu_crf[a->bf], c);
    tcg_gen_shli_i64(c, c, FPSCR_FPCC);
    tcg_gen_trunc_i64_tl(t, c);
    tcg_gen_andi_tl(cpu_fpscr, cpu_fpscr, ~(target_ulong)(FP_FPCC | FP_FI));
    tcg_gen_or_tl(cpu_fpscr, cpu_fpscr, t);
    return true;
}

/* The helper sequences of the inline ops, then on to the next insn */
static void native_fp_emit_slow_paths(DisasContext *ctx)
{
    for (int i = 0; i < ctx->n_nfp; i++) {
        NativeFPSlow *sp = &ctx->nfp[i];
        TCGv_i64 t0 = tcg_temp_new_i64();
        TCGv_i64 t1 = tcg_temp_new_i64();
        TCGv_i64 t2 = tcg_temp_new_i64();
        TCGv_i64 t3 = tcg_temp_new_i64();

        gen_set_label(sp->label);
        gen_reset_fpstatus();
        get_fpr(t0, sp->x);
        switch (sp->kind) {
        case NFP_AB:
            get_fpr(t1, sp->y);
            ((void (*)(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64))sp->helper)
                (t3, tcg_env, t0, t1);
            break;
        case NFP_ACB:
            get_fpr(t1, sp->y);
            get_fpr(t2, sp->z);
            ((void (*)(TCGv_i64, TCGv_ptr, TCGv_i64, TCGv_i64, TCGv_i64))
                sp->helper)(t3, tcg_env, t0, t1, t2);
            break;
        case NFP_CMPU:
            get_fpr(t1, sp->y);
            ((void (*)(TCGv_ptr, TCGv_i64, TCGv_i64, TCGv_i32))sp->helper)
                (tcg_env, t0, t1, tcg_constant_i32(sp->frt));
            gen_helper_float_check_status(tcg_env);
            break;
        }
        if (sp->kind != NFP_CMPU) {
            set_fpr(sp->frt, t3);
            gen_compute_fprf_float64(t3);
        }
        gen_update_nip(ctx, sp->next);
        tcg_gen_lookup_and_goto_ptr();
    }
    ctx->n_nfp = 0;
}
static bool do_helper_acb(DisasContext *ctx, arg_A *a,
                          void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                         TCGv_i64, TCGv_i64))
{
    TCGv_i64 t0, t1, t2, t3;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (native_fp_try(ctx, helper, a->rc, NFP_ACB,
                      a->frt, a->fra, a->frc, a->frb)) {
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    t3 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frc);
    get_fpr(t2, a->frb);
    helper(t3, tcg_env, t0, t1, t2);
    set_fpr(a->frt, t3);
    gen_compute_fprf_float64(t3);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_ab(DisasContext *ctx, arg_A_tab *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                        TCGv_i64))
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (native_fp_try(ctx, helper, a->rc, NFP_AB,
                      a->frt, a->fra, a->frb, -1)) {
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frb);
    helper(t2, tcg_env, t0, t1);
    set_fpr(a->frt, t2);
    gen_compute_fprf_float64(t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_ac(DisasContext *ctx, arg_A_tac *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64,
                                        TCGv_i64))
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (native_fp_try(ctx, helper, a->rc, NFP_AB,
                      a->frt, a->fra, a->frc, -1)) {
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->fra);
    get_fpr(t1, a->frc);
    helper(t2, tcg_env, t0, t1);
    set_fpr(a->frt, t2);
    gen_compute_fprf_float64(t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_round_convert(DisasContext *ctx, arg_X_tb_rc *a,
                             void (*helper)(TCGv_i64, TCGv_env, TCGv_i64),
                             bool set_fprf)
{
    TCGv_i64 t0, t1;
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->rb);
    helper(t1, tcg_env, t0);
    set_fpr(a->rt, t1);
    if (set_fprf) {
        gen_helper_compute_fprf_float64(tcg_env, t1);
    }
    gen_helper_float_check_status(tcg_env);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_bs(DisasContext *ctx, arg_A_tb *a,
                         void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64))
{
    TCGv_i64 t0, t1;
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    get_fpr(t0, a->frb);
    helper(t1, tcg_env, t0);
    set_fpr(a->frt, t1);
    gen_compute_fprf_float64(t1);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_FSEL(DisasContext *ctx, arg_A *a)
{
    TCGv_i64 t0, t1, t2;

    REQUIRE_INSNS_FLAGS(ctx, FLOAT_FSEL);
    REQUIRE_FPU(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();

    get_fpr(t0, a->fra);
    get_fpr(t1, a->frb);
    get_fpr(t2, a->frc);

    gen_helper_FSEL(t0, t0, t1, t2);
    set_fpr(a->frt, t0);
    if (a->rc) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool do_helper_fsqrt(DisasContext *ctx, arg_A_tb *a,
                            void (*helper)(TCGv_i64, TCGv_ptr, TCGv_i64))
{
    TCGv_i64 t0, t1;

    REQUIRE_INSNS_FLAGS(ctx, FLOAT_FSQRT);
    REQUIRE_FPU(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    gen_reset_fpstatus();
    get_fpr(t0, a->frb);
    helper(t1, tcg_env, t0);
    set_fpr(a->frt, t1);
    gen_compute_fprf_float64(t1);
    if (unlikely(a->rc != 0)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

TRANS(FADD, do_helper_ab, gen_helper_FADD);
TRANS(FADDS, do_helper_ab, gen_helper_FADDS);
TRANS(FSUB, do_helper_ab, gen_helper_FSUB);
TRANS(FSUBS, do_helper_ab, gen_helper_FSUBS);
TRANS(FDIV, do_helper_ab, gen_helper_FDIV);
TRANS(FDIVS, do_helper_ab, gen_helper_FDIVS);
TRANS(FMUL, do_helper_ac, gen_helper_FMUL);
TRANS(FMULS, do_helper_ac, gen_helper_FMULS);

TRANS(FMADD, do_helper_acb, gen_helper_FMADD);
TRANS(FMADDS, do_helper_acb, gen_helper_FMADDS);
TRANS(FMSUB, do_helper_acb, gen_helper_FMSUB);
TRANS(FMSUBS, do_helper_acb, gen_helper_FMSUBS);

TRANS(FNMADD, do_helper_acb, gen_helper_FNMADD);
TRANS(FNMADDS, do_helper_acb, gen_helper_FNMADDS);
TRANS(FNMSUB, do_helper_acb, gen_helper_FNMSUB);
TRANS(FNMSUBS, do_helper_acb, gen_helper_FNMSUBS);

TRANS_FLAGS(FLOAT_EXT, FRE, do_helper_bs, gen_helper_FRE);
TRANS_FLAGS(FLOAT_FRES, FRES, do_helper_bs, gen_helper_FRES);
TRANS_FLAGS(FLOAT_FRSQRTE, FRSQRTE, do_helper_bs, gen_helper_FRSQRTE);
TRANS_FLAGS(FLOAT_FRSQRTES, FRSQRTES, do_helper_bs, gen_helper_FRSQRTES);

TRANS(FSQRT, do_helper_fsqrt, gen_helper_FSQRT);
TRANS(FSQRTS, do_helper_fsqrt, gen_helper_FSQRTS);

/***                     Floating-Point round & convert                    ***/
TRANS_FLAGS(FLOAT, FRSP, do_round_convert, gen_helper_FRSP, true);
TRANS_FLAGS(FLOAT_EXT, FRIN, do_round_convert, gen_helper_FRIN, true);
TRANS_FLAGS(FLOAT_EXT, FRIZ, do_round_convert, gen_helper_FRIZ, true);
TRANS_FLAGS(FLOAT_EXT, FRIP, do_round_convert, gen_helper_FRIP, true);
TRANS_FLAGS(FLOAT_EXT, FRIM, do_round_convert, gen_helper_FRIM, true);

TRANS_FLAGS(FLOAT, FCTIW, do_round_convert, gen_helper_FCTIW, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCTIWU, do_round_convert, gen_helper_FCTIWU, false);
TRANS_FLAGS(FLOAT, FCTIWZ, do_round_convert, gen_helper_FCTIWZ, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCTIWUZ, do_round_convert, gen_helper_FCTIWUZ, false);

TRANS_FLAGS2(FP_CVT_S64, FCTID, do_round_convert, gen_helper_FCTID, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCTIDU, do_round_convert, gen_helper_FCTIDU, false);
TRANS_FLAGS2(FP_CVT_S64, FCTIDZ, do_round_convert, gen_helper_FCTIDZ, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCTIDUZ, do_round_convert, gen_helper_FCTIDUZ, false);

TRANS_FLAGS2(FP_CVT_S64, FCFID, do_round_convert, gen_helper_FCFID, true);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDS, do_round_convert, gen_helper_FCFIDS, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDU, do_round_convert, gen_helper_FCFIDU, false);
TRANS_FLAGS2(FP_CVT_ISA206, FCFIDUS, do_round_convert, gen_helper_FCFIDUS, false);

static bool trans_FTDIV(DisasContext *ctx, arg_X_bf *a)
{
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS2(ctx, FP_TST_ISA206);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    gen_helper_FTDIV(cpu_crf[a->bf], t0, t1);
    return true;
}

static bool trans_FTSQRT(DisasContext *ctx, arg_X_bf_b *a)
{
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS2(ctx, FP_TST_ISA206);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    gen_helper_FTSQRT(cpu_crf[a->bf], t0);
    return true;
}

/***                         Floating-Point compare                        ***/
static bool do_helper_cmp(DisasContext *ctx, arg_X_bf *a,
                          void (*helper)(TCGv_env, TCGv_i64, TCGv_i64,
                                         TCGv_i32))
{
    TCGv_i32 crf;
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if ((void *)helper == (void *)gen_helper_FCMPU && native_fcmpu(ctx, a)) {
        return true;
    }
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    gen_reset_fpstatus();
    crf = tcg_constant_i32(a->bf);
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    helper(tcg_env, t0, t1, crf);
    gen_helper_float_check_status(tcg_env);
    return true;
}

TRANS(FCMPU, do_helper_cmp, gen_helper_FCMPU);
TRANS(FCMPO, do_helper_cmp, gen_helper_FCMPO);

/***                         Floating-point move                           ***/

/* fmr  - fmr. */
/* XXX: beware that fmr never checks for NaNs nor update FPSCR */
static bool trans_FMR(DisasContext *ctx, arg_FMR *a)
{
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    set_fpr(a->rt, t0);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

/* XXX: beware that f{neg, abs, nabs} never checks for NaNs nor update FPSCR */
static bool do_move_b(DisasContext *ctx, arg_X_tb_rc *a, int64_t val,
                      void (*tcg_op)(TCGv_i64, TCGv_i64, int64_t))
{
    TCGv_i64 t0, t1;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    tcg_op(t1, t0, val);
    set_fpr(a->rt, t1);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

TRANS(FNEG, do_move_b, 1ULL << 63, tcg_gen_xori_i64);
TRANS(FABS, do_move_b, ~(1ULL << 63), tcg_gen_andi_i64);
TRANS(FNABS, do_move_b, 1ULL << 63, tcg_gen_ori_i64);

/* fcpsgn: PowerPC 2.05 specification */
/* XXX: beware that fcpsgn never checks for NaNs nor update FPSCR */
static bool trans_FCPSGN(DisasContext *ctx, arg_FCPSGN *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, ISA205);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t0, a->ra);
    get_fpr(t1, a->rb);
    tcg_gen_deposit_i64(t2, t0, t1, 0, 63);
    set_fpr(a->rt, t2);
    if (unlikely(a->rc)) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_FMRGEW(DisasContext *ctx, arg_FMRGEW *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, VSX207);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_shri_i64(t0, t1, 32);
    get_fpr(t1, a->ra);
    tcg_gen_deposit_i64(t2, t1, t0, 0, 32);
    set_fpr(a->rt, t2);
    return true;
}

static bool trans_FMRGOW(DisasContext *ctx, arg_FMRGOW *a)
{
    TCGv_i64 t0, t1, t2;
    REQUIRE_INSNS_FLAGS2(ctx, VSX207);
    REQUIRE_FPU(ctx);
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    get_fpr(t0, a->rb);
    get_fpr(t1, a->ra);
    tcg_gen_deposit_i64(t2, t0, t1, 32, 32);
    set_fpr(a->rt, t2);
    return true;
}

/***                  Floating-Point status & ctrl register                ***/

/* mcrfs */
static void gen_mcrfs(DisasContext *ctx)
{
    TCGv tmp = tcg_temp_new();
    TCGv_i32 tmask;
    TCGv_i64 tnew_fpscr = tcg_temp_new_i64();
    int bfa;
    int nibble;
    int shift;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    bfa = crfS(ctx->opcode);
    nibble = 7 - bfa;
    shift = 4 * nibble;
    tcg_gen_shri_tl(tmp, cpu_fpscr, shift);
    tcg_gen_trunc_tl_i32(cpu_crf[crfD(ctx->opcode)], tmp);
    tcg_gen_andi_i32(cpu_crf[crfD(ctx->opcode)], cpu_crf[crfD(ctx->opcode)],
                     0xf);
    tcg_gen_extu_tl_i64(tnew_fpscr, cpu_fpscr);
    /* Only the exception bits (including FX) should be cleared if read */
    tcg_gen_andi_i64(tnew_fpscr, tnew_fpscr,
                     ~(MAKE_64BIT_MASK(shift, 4) & FP_EX_CLEAR_BITS));
    /* FEX and VX need to be updated, so don't set fpscr directly */
    tmask = tcg_constant_i32(1 << nibble);
    gen_helper_store_fpscr(tcg_env, tnew_fpscr, tmask);
}

static TCGv_i64 place_from_fpscr(int rt, uint64_t mask)
{
    TCGv_i64 fpscr = tcg_temp_new_i64();
    TCGv_i64 fpscr_masked = tcg_temp_new_i64();

    tcg_gen_extu_tl_i64(fpscr, cpu_fpscr);
    tcg_gen_andi_i64(fpscr_masked, fpscr, mask);
    set_fpr(rt, fpscr_masked);

    return fpscr;
}

static void store_fpscr_masked(TCGv_i64 fpscr, uint64_t clear_mask,
                               TCGv_i64 set_mask, uint32_t store_mask)
{
    TCGv_i64 fpscr_masked = tcg_temp_new_i64();
    TCGv_i32 st_mask = tcg_constant_i32(store_mask);

    tcg_gen_andi_i64(fpscr_masked, fpscr, ~clear_mask);
    tcg_gen_or_i64(fpscr_masked, fpscr_masked, set_mask);
    gen_helper_store_fpscr(tcg_env, fpscr_masked, st_mask);
}

static bool trans_MFFS_ISA207(DisasContext *ctx, arg_X_t_rc *a)
{
    if (!(ctx->insns_flags2 & PPC2_ISA300)) {
        /*
         * Before Power ISA v3.0, MFFS bits 11~15 were reserved, any instruction
         * with OPCD=63 and XO=583 should be decoded as MFFS.
         */
        return trans_MFFS(ctx, a);
    }
    /*
     * For Power ISA v3.0+, return false and let the pattern group
     * select the correct instruction.
     */
    return false;
}

static bool trans_MFFS(DisasContext *ctx, arg_X_t_rc *a)
{
    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    place_from_fpscr(a->rt, UINT64_MAX);
    if (a->rc) {
        gen_set_cr1_from_fpscr(ctx);
    }
    return true;
}

static bool trans_MFFSCE(DisasContext *ctx, arg_X_t *a)
{
    TCGv_i64 fpscr;

    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, UINT64_MAX);
    store_fpscr_masked(fpscr, FP_ENABLES, tcg_constant_i64(0), 0x0003);
    native_fp_end_tb(ctx, 0x0003);
    return true;
}

static bool trans_MFFSCRN(DisasContext *ctx, arg_X_tb *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_andi_i64(t1, t1, FP_RN);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_RN, t1, 0x0001);
    native_fp_end_tb(ctx, 0x0001);
    return true;
}

static bool trans_MFFSCDRN(DisasContext *ctx, arg_X_tb *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    get_fpr(t1, a->rb);
    tcg_gen_andi_i64(t1, t1, FP_DRN);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_DRN, t1, 0x0100);
    return true;
}

static bool trans_MFFSCRNI(DisasContext *ctx, arg_X_imm2 *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    tcg_gen_movi_i64(t1, a->imm);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_RN, t1, 0x0001);
    native_fp_end_tb(ctx, 0x0001);
    return true;
}

static bool trans_MFFSCDRNI(DisasContext *ctx, arg_X_imm3 *a)
{
    TCGv_i64 t1, fpscr;

    REQUIRE_FPU(ctx);

    t1 = tcg_temp_new_i64();
    tcg_gen_movi_i64(t1, (uint64_t)a->imm << FPSCR_DRN0);

    gen_reset_fpstatus();
    fpscr = place_from_fpscr(a->rt, FP_DRN | FP_ENABLES | FP_NI | FP_RN);
    store_fpscr_masked(fpscr, FP_DRN, t1, 0x0100);
    return true;
}

static bool trans_MFFSL(DisasContext *ctx, arg_X_t *a)
{
    REQUIRE_FPU(ctx);

    gen_reset_fpstatus();
    place_from_fpscr(a->rt, FP_DRN | FP_STATUS | FP_ENABLES | FP_NI | FP_RN);
    return true;
}

/* mtfsb0 */
static void gen_mtfsb0(DisasContext *ctx)
{
    uint8_t crb;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    crb = 31 - crbD(ctx->opcode);
    gen_reset_fpstatus();
    if (likely(crb != FPSCR_FEX && crb != FPSCR_VX)) {
        gen_helper_fpscr_clrbit(tcg_env, tcg_constant_i32(crb));
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    native_fp_end_tb(ctx, 1 << (crb / 4));
}

/* mtfsb1 */
static void gen_mtfsb1(DisasContext *ctx)
{
    uint8_t crb;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    crb = 31 - crbD(ctx->opcode);
    /* XXX: we pretend we can only do IEEE floating-point computations */
    if (likely(crb != FPSCR_FEX && crb != FPSCR_VX && crb != FPSCR_NI)) {
        gen_helper_fpscr_setbit(tcg_env, tcg_constant_i32(crb));
    }
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
    native_fp_end_tb(ctx, 1 << (crb / 4));
}

/* mtfsf */
static void gen_mtfsf(DisasContext *ctx)
{
    TCGv_i32 t0;
    TCGv_i64 t1;
    int flm, l, w;
    uint32_t nibbles;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    flm = FPFLM(ctx->opcode);
    l = FPL(ctx->opcode);
    w = FPW(ctx->opcode);
    if (unlikely(w & !(ctx->insns_flags2 & PPC2_ISA205))) {
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);
        return;
    }
    if (!l) {
        nibbles = flm << (w * 8);
    } else if (ctx->insns_flags2 & PPC2_ISA205) {
        nibbles = 0xffff;
    } else {
        nibbles = 0xff;
    }
    t0 = tcg_constant_i32(nibbles);
    t1 = tcg_temp_new_i64();
    get_fpr(t1, rB(ctx->opcode));
    gen_helper_store_fpscr(tcg_env, t1, t0);
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
    native_fp_end_tb(ctx, nibbles);
}

/* mtfsfi */
static void gen_mtfsfi(DisasContext *ctx)
{
    int bf, sh, w;
    TCGv_i64 t0;
    TCGv_i32 t1;

    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    w = FPW(ctx->opcode);
    bf = FPBF(ctx->opcode);
    if (unlikely(w & !(ctx->insns_flags2 & PPC2_ISA205))) {
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);
        return;
    }
    sh = (8 * w) + 7 - bf;
    t0 = tcg_constant_i64(((uint64_t)FPIMM(ctx->opcode)) << (4 * sh));
    t1 = tcg_constant_i32(1 << sh);
    gen_helper_store_fpscr(tcg_env, t0, t1);
    if (unlikely(Rc(ctx->opcode) != 0)) {
        tcg_gen_trunc_tl_i32(cpu_crf[1], cpu_fpscr);
        tcg_gen_shri_i32(cpu_crf[1], cpu_crf[1], FPSCR_OX);
    }
    /* We can raise a deferred exception */
    gen_helper_fpscr_check_status(tcg_env);
    native_fp_end_tb(ctx, 1 << sh);
}

static void gen_qemu_ld32fs(DisasContext *ctx, TCGv_i64 dest, TCGv addr)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    tcg_gen_qemu_ld_i32(tmp, addr, ctx->mem_idx, DEF_MEMOP(MO_UL));
    gen_helper_todouble(dest, tmp);
}

/* lfdepx (external PID lfdx) */
static void gen_lfdepx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    CHK_SV(ctx);
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    tcg_gen_qemu_ld_i64(t0, EA, PPC_TLB_EPID_LOAD, DEF_MEMOP(MO_UQ));
    set_fpr(rD(ctx->opcode), t0);
}

/* lfdp */
static void gen_lfdp(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    gen_addr_imm_index(ctx, EA, 0);
    t0 = tcg_temp_new_i64();
    /*
     * We only need to swap high and low halves. gen_qemu_ld64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
    } else {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
    }
}

/* lfdpx */
static void gen_lfdpx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    gen_addr_reg_index(ctx, EA);
    t0 = tcg_temp_new_i64();
    /*
     * We only need to swap high and low halves. gen_qemu_ld64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
    } else {
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode), t0);
        tcg_gen_addi_tl(EA, EA, 8);
        gen_qemu_ld64_i64(ctx, t0, EA);
        set_fpr(rD(ctx->opcode) + 1, t0);
    }
}

/* lfiwax */
static void gen_lfiwax(DisasContext *ctx)
{
    TCGv EA;
    TCGv t0;
    TCGv_i64 t1;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new();
    t1 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    gen_qemu_ld32s(ctx, t0, EA);
    tcg_gen_ext_tl_i64(t1, t0);
    set_fpr(rD(ctx->opcode), t1);
}

/* lfiwzx */
static void gen_lfiwzx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    gen_qemu_ld32u_i64(ctx, t0, EA);
    set_fpr(rD(ctx->opcode), t0);
}

#define GEN_STXF(name, stop, opc2, opc3, type)                                \
static void glue(gen_, name##x)(DisasContext *ctx)                            \
{                                                                             \
    TCGv EA;                                                                  \
    TCGv_i64 t0;                                                              \
    if (unlikely(!ctx->fpu_enabled)) {                                        \
        gen_exception(ctx, POWERPC_EXCP_FPU);                                 \
        return;                                                               \
    }                                                                         \
    gen_set_access_type(ctx, ACCESS_FLOAT);                                   \
    EA = tcg_temp_new();                                                      \
    t0 = tcg_temp_new_i64();                                                  \
    gen_addr_reg_index(ctx, EA);                                              \
    get_fpr(t0, rS(ctx->opcode));                                             \
    gen_qemu_##stop(ctx, t0, EA);                                             \
}

static void gen_qemu_st32fs(DisasContext *ctx, TCGv_i64 src, TCGv addr)
{
    TCGv_i32 tmp = tcg_temp_new_i32();
    gen_helper_tosingle(tmp, src);
    tcg_gen_qemu_st_i32(tmp, addr, ctx->mem_idx, DEF_MEMOP(MO_UL));
}

/* stfdepx (external PID lfdx) */
static void gen_stfdepx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    CHK_SV(ctx);
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    get_fpr(t0, rD(ctx->opcode));
    tcg_gen_qemu_st_i64(t0, EA, PPC_TLB_EPID_STORE, DEF_MEMOP(MO_UQ));
}

/* stfdp */
static void gen_stfdp(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_imm_index(ctx, EA, 0);
    /*
     * We only need to swap high and low halves. gen_qemu_st64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
    } else {
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
    }
}

/* stfdpx */
static void gen_stfdpx(DisasContext *ctx)
{
    TCGv EA;
    TCGv_i64 t0;
    if (unlikely(!ctx->fpu_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_FPU);
        return;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    EA = tcg_temp_new();
    t0 = tcg_temp_new_i64();
    gen_addr_reg_index(ctx, EA);
    /*
     * We only need to swap high and low halves. gen_qemu_st64_i64
     * does necessary 64-bit byteswap already.
     */
    if (unlikely(ctx->le_mode)) {
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
    } else {
        get_fpr(t0, rD(ctx->opcode));
        gen_qemu_st64_i64(ctx, t0, EA);
        tcg_gen_addi_tl(EA, EA, 8);
        get_fpr(t0, rD(ctx->opcode) + 1);
        gen_qemu_st64_i64(ctx, t0, EA);
    }
}

/* Optional: */
static inline void gen_qemu_st32fiw(DisasContext *ctx, TCGv_i64 arg1, TCGv arg2)
{
    TCGv t0 = tcg_temp_new();
    tcg_gen_trunc_i64_tl(t0, arg1),
    gen_qemu_st32(ctx, t0, arg2);
}
/* stfiwx */
GEN_STXF(stfiw, st32fiw, 0x17, 0x1E, PPC_FLOAT_STFIWX);

/*            Floating-point Load/Store Instructions                         */
static bool do_lsfpsd(DisasContext *ctx, int rt, int ra, TCGv displ,
                      bool update, bool store, bool single)
{
    TCGv ea;
    TCGv_i64 t0;
    REQUIRE_INSNS_FLAGS(ctx, FLOAT);
    REQUIRE_FPU(ctx);
    if (update && ra == 0) {
        gen_invalid(ctx);
        return true;
    }
    gen_set_access_type(ctx, ACCESS_FLOAT);
    t0 = tcg_temp_new_i64();
    ea = do_ea_calc(ctx, ra, displ);
    if (store) {
        get_fpr(t0, rt);
        if (single) {
            gen_qemu_st32fs(ctx, t0, ea);
        } else {
            gen_qemu_st64_i64(ctx, t0, ea);
        }
    } else {
        if (single) {
            gen_qemu_ld32fs(ctx, t0, ea);
        } else {
            gen_qemu_ld64_i64(ctx, t0, ea);
        }
        set_fpr(rt, t0);
    }
    if (update) {
        tcg_gen_mov_tl(cpu_gpr[ra], ea);
    }
    return true;
}

static bool do_lsfp_D(DisasContext *ctx, arg_D *a, bool update, bool store,
                      bool single)
{
    return do_lsfpsd(ctx, a->rt, a->ra, tcg_constant_tl(a->si), update, store,
                     single);
}

static bool do_lsfp_PLS_D(DisasContext *ctx, arg_PLS_D *a, bool update,
                          bool store, bool single)
{
    arg_D d;
    if (!resolve_PLS_D(ctx, &d, a)) {
        return true;
    }
    return do_lsfp_D(ctx, &d, update, store, single);
}

static bool do_lsfp_X(DisasContext *ctx, arg_X *a, bool update,
                      bool store, bool single)
{
    return do_lsfpsd(ctx, a->rt, a->ra, cpu_gpr[a->rb], update, store, single);
}

TRANS(LFS, do_lsfp_D, false, false, true)
TRANS(LFSU, do_lsfp_D, true, false, true)
TRANS(LFSX, do_lsfp_X, false, false, true)
TRANS(LFSUX, do_lsfp_X, true, false, true)
TRANS(PLFS, do_lsfp_PLS_D, false, false, true)

TRANS(LFD, do_lsfp_D, false, false, false)
TRANS(LFDU, do_lsfp_D, true, false, false)
TRANS(LFDX, do_lsfp_X, false, false, false)
TRANS(LFDUX, do_lsfp_X, true, false, false)
TRANS(PLFD, do_lsfp_PLS_D, false, false, false)

TRANS(STFS, do_lsfp_D, false, true, true)
TRANS(STFSU, do_lsfp_D, true, true, true)
TRANS(STFSX, do_lsfp_X, false, true, true)
TRANS(STFSUX, do_lsfp_X, true, true, true)
TRANS(PSTFS, do_lsfp_PLS_D, false, true, true)

TRANS(STFD, do_lsfp_D, false, true, false)
TRANS(STFDU, do_lsfp_D, true, true, false)
TRANS(STFDX, do_lsfp_X, false, true, false)
TRANS(STFDUX, do_lsfp_X, true, true, false)
TRANS(PSTFD, do_lsfp_PLS_D, false, true, false)

#undef GEN_LDF
#undef GEN_LDUF
#undef GEN_LDUXF
#undef GEN_LDXF
#undef GEN_LDFS

#undef GEN_STF
#undef GEN_STUF
#undef GEN_STUXF
#undef GEN_STXF
#undef GEN_STFS
