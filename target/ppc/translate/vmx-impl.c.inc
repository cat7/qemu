/*
 * translate/vmx-impl.c
 *
 * Altivec/VMX translation
 */

/***                      Altivec vector extension                         ***/
/* Altivec registers moves */

static inline TCGv_ptr gen_avr_ptr(int reg)
{
    TCGv_ptr r = tcg_temp_new_ptr();
    tcg_gen_addi_ptr(r, tcg_env, avr_full_offset(reg));
    return r;
}

static inline void get_avr64(TCGv_i64 dst, int regno, bool high)
{
    tcg_gen_ld_i64(dst, tcg_env, avr64_offset(regno, high));
}

static inline void set_avr64(int regno, TCGv_i64 src, bool high)
{
    tcg_gen_st_i64(src, tcg_env, avr64_offset(regno, high));
}

static inline void get_avr_full(TCGv_i128 dst, int regno)
{
    tcg_gen_ld_i128(dst, tcg_env, avr_full_offset(regno));
}

static inline void set_avr_full(int regno, TCGv_i128 src)
{
    tcg_gen_st_i128(src, tcg_env, avr_full_offset(regno));
}

static bool trans_LVX(DisasContext *ctx, arg_X *a)
{
    TCGv EA;
    TCGv_i128 avr;
    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);
    gen_set_access_type(ctx, ACCESS_INT);
    avr = tcg_temp_new_i128();
    EA = do_ea_calc(ctx, a->ra, cpu_gpr[a->rb]);
    tcg_gen_andi_tl(EA, EA, ~0xf);
    tcg_gen_qemu_ld_i128(avr, EA, ctx->mem_idx,
                         DEF_MEMOP(MO_128 | MO_ATOM_IFALIGN_PAIR));
    set_avr_full(a->rt, avr);
    return true;
}

/* As we don't emulate the cache, lvxl is strictly equivalent to lvx */
QEMU_FLATTEN
static bool trans_LVXL(DisasContext *ctx, arg_LVXL *a)
{
    return trans_LVX(ctx, a);
}

static bool trans_STVX(DisasContext *ctx, arg_STVX *a)
{
    TCGv EA;
    TCGv_i128 avr;
    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);
    gen_set_access_type(ctx, ACCESS_INT);
    avr = tcg_temp_new_i128();
    EA = do_ea_calc(ctx, a->ra, cpu_gpr[a->rb]);
    tcg_gen_andi_tl(EA, EA, ~0xf);
    get_avr_full(avr, a->rt);
    tcg_gen_qemu_st_i128(avr, EA, ctx->mem_idx,
                         DEF_MEMOP(MO_128 | MO_ATOM_IFALIGN_PAIR));
    return true;
}

/* As we don't emulate the cache, stvxl is strictly equivalent to stvx */
QEMU_FLATTEN
static bool trans_STVXL(DisasContext *ctx, arg_STVXL *a)
{
    return trans_STVX(ctx, a);
}

static bool do_ldst_ve_X(DisasContext *ctx, arg_X *a, int size,
                   void (*helper)(TCGv_env, TCGv_ptr, TCGv))
{
    TCGv EA;
    TCGv_ptr vrt;
    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);
    gen_set_access_type(ctx, ACCESS_INT);
    EA = do_ea_calc(ctx, a->ra, cpu_gpr[a->rb]);
    if (size > 1) {
        tcg_gen_andi_tl(EA, EA, ~(size - 1));
    }
    vrt = gen_avr_ptr(a->rt);
    helper(tcg_env, vrt, EA);
    return true;
}

TRANS(LVEBX, do_ldst_ve_X, 1, gen_helper_LVEBX);
TRANS(LVEHX, do_ldst_ve_X, 2, gen_helper_LVEHX);
TRANS(LVEWX, do_ldst_ve_X, 4, gen_helper_LVEWX);

TRANS(STVEBX, do_ldst_ve_X, 1, gen_helper_STVEBX);
TRANS(STVEHX, do_ldst_ve_X, 2, gen_helper_STVEHX);
TRANS(STVEWX, do_ldst_ve_X, 4, gen_helper_STVEWX);

static void gen_mfvscr(DisasContext *ctx)
{
    TCGv_i32 t;
    TCGv_i64 avr;
    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }
    avr = tcg_temp_new_i64();
    tcg_gen_movi_i64(avr, 0);
    set_avr64(rD(ctx->opcode), avr, true);
    t = tcg_temp_new_i32();
    gen_helper_mfvscr(t, tcg_env);
    tcg_gen_extu_i32_i64(avr, t);
    set_avr64(rD(ctx->opcode), avr, false);
}

static void gen_mtvscr(DisasContext *ctx)
{
    TCGv_i32 val;
    int bofs;

    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }

    val = tcg_temp_new_i32();
    bofs = avr_full_offset(rB(ctx->opcode));
#if HOST_BIG_ENDIAN
    bofs += 3 * 4;
#endif

    tcg_gen_ld_i32(val, tcg_env, bofs);
    gen_helper_mtvscr(tcg_env, val);
}

static bool do_vx_vmul10(DisasContext *ctx, arg_VX *a,
                         bool add_cin, bool ret_carry)
{
    TCGv_i64 t0;
    TCGv_i64 t1;
    TCGv_i64 t2;
    TCGv_i64 avr;
    TCGv_i64 ten, z;

    REQUIRE_VECTOR(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();
    avr = tcg_temp_new_i64();
    ten = tcg_constant_i64(10);
    z = tcg_constant_i64(0);

    if (add_cin) {
        get_avr64(avr, a->vra, false);
        tcg_gen_mulu2_i64(t0, t1, avr, ten);
        get_avr64(avr, a->vrb, false);
        tcg_gen_andi_i64(t2, avr, 0xF);
        tcg_gen_add2_i64(avr, t2, t0, t1, t2, z);
        set_avr64(a->vrt, avr, false);
    } else {
        get_avr64(avr, a->vra, false);
        tcg_gen_mulu2_i64(avr, t2, avr, ten);
        set_avr64(a->vrt, avr, false);
    }

    if (ret_carry) {
        get_avr64(avr, a->vra, true);
        tcg_gen_mulu2_i64(t0, t1, avr, ten);
        tcg_gen_add2_i64(t0, avr, t0, t1, t2, z);
        set_avr64(a->vrt, avr, false);
        set_avr64(a->vrt, z, true);
    } else {
        get_avr64(avr, a->vra, true);
        tcg_gen_mul_i64(t0, avr, ten);
        tcg_gen_add_i64(avr, t0, t2);
        set_avr64(a->vrt, avr, true);
    }

    return true;
}

#define GEN_VXFORM(name, opc2, opc3)                                    \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    TCGv_ptr ra, rb, rd;                                                \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    ra = gen_avr_ptr(rA(ctx->opcode));                                  \
    rb = gen_avr_ptr(rB(ctx->opcode));                                  \
    rd = gen_avr_ptr(rD(ctx->opcode));                                  \
    gen_helper_##name(rd, ra, rb);                                      \
}

#define GEN_VXFORM_TRANS(name, opc2, opc3)                              \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    trans_##name(ctx);                                                  \
}

#define GEN_VXFORM_ENV(name, opc2, opc3)                                \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    TCGv_ptr ra, rb, rd;                                                \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    ra = gen_avr_ptr(rA(ctx->opcode));                                  \
    rb = gen_avr_ptr(rB(ctx->opcode));                                  \
    rd = gen_avr_ptr(rD(ctx->opcode));                                  \
    gen_helper_##name(tcg_env, rd, ra, rb);                             \
}

#define GEN_VXFORM3(name, opc2, opc3)                                   \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    TCGv_ptr ra, rb, rc, rd;                                            \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    ra = gen_avr_ptr(rA(ctx->opcode));                                  \
    rb = gen_avr_ptr(rB(ctx->opcode));                                  \
    rc = gen_avr_ptr(rC(ctx->opcode));                                  \
    rd = gen_avr_ptr(rD(ctx->opcode));                                  \
    gen_helper_##name(rd, ra, rb, rc);                                  \
}

/*
 * Support for Altivec instruction pairs that use bit 31 (Rc) as
 * an opcode bit.  In general, these pairs come from different
 * versions of the ISA, so we must also support a pair of flags for
 * each instruction.
 */
#define GEN_VXFORM_DUAL(name0, flg0, flg2_0, name1, flg1, flg2_1)          \
static void glue(gen_, name0##_##name1)(DisasContext *ctx)             \
{                                                                      \
    if ((Rc(ctx->opcode) == 0) &&                                      \
        ((ctx->insns_flags & flg0) || (ctx->insns_flags2 & flg2_0))) { \
        gen_##name0(ctx);                                              \
    } else if ((Rc(ctx->opcode) == 1) &&                               \
        ((ctx->insns_flags & flg1) || (ctx->insns_flags2 & flg2_1))) { \
        gen_##name1(ctx);                                              \
    } else {                                                           \
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);            \
    }                                                                  \
}

/*
 * We use this macro if one instruction is realized with direct
 * translation, and second one with helper.
 */
#define GEN_VXFORM_TRANS_DUAL(name0, flg0, flg2_0, name1, flg1, flg2_1)\
static void glue(gen_, name0##_##name1)(DisasContext *ctx)             \
{                                                                      \
    if ((Rc(ctx->opcode) == 0) &&                                      \
        ((ctx->insns_flags & flg0) || (ctx->insns_flags2 & flg2_0))) { \
        if (unlikely(!ctx->altivec_enabled)) {                         \
            gen_exception(ctx, POWERPC_EXCP_VPU);                      \
            return;                                                    \
        }                                                              \
        trans_##name0(ctx);                                            \
    } else if ((Rc(ctx->opcode) == 1) &&                               \
        ((ctx->insns_flags & flg1) || (ctx->insns_flags2 & flg2_1))) { \
        gen_##name1(ctx);                                              \
    } else {                                                           \
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);            \
    }                                                                  \
}

/* Adds support to provide invalid mask */
#define GEN_VXFORM_DUAL_EXT(name0, flg0, flg2_0, inval0,                \
                            name1, flg1, flg2_1, inval1)                \
static void glue(gen_, name0##_##name1)(DisasContext *ctx)              \
{                                                                       \
    if ((Rc(ctx->opcode) == 0) &&                                       \
        ((ctx->insns_flags & flg0) || (ctx->insns_flags2 & flg2_0)) &&  \
        !(ctx->opcode & inval0)) {                                      \
        gen_##name0(ctx);                                               \
    } else if ((Rc(ctx->opcode) == 1) &&                                \
               ((ctx->insns_flags & flg1) || (ctx->insns_flags2 & flg2_1)) && \
               !(ctx->opcode & inval1)) {                               \
        gen_##name1(ctx);                                               \
    } else {                                                            \
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);             \
    }                                                                   \
}

#define GEN_VXFORM_HETRO(name, opc2, opc3)                              \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    TCGv_ptr rb;                                                        \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    rb = gen_avr_ptr(rB(ctx->opcode));                                  \
    gen_helper_##name(cpu_gpr[rD(ctx->opcode)], cpu_gpr[rA(ctx->opcode)], rb); \
}

enum { VPK_US, VPK_SU, VPK_SS, VPK_PX };
enum { VSUM_UB, VSUM_SB, VSUM_SH };
enum {
    VP_MRGB, VP_MRGH, VP_MRGW, VP_UPKH, VP_UPKPX, VP_PKM, VP_PKS,
    VP_PKUWUS, VP_PKPX, VP_SUM4, VP_SUMS, VP_SO, VP_N
};

static bool vmxperm(int op);
static void gen_vmrg_i64(int vrt, int vra, int vrb, unsigned vece, bool hi);
static void gen_vpkum_i64(int vrt, int vra, int vrb, unsigned vece);
static void gen_vpk_vec(int vrt, int vra, int vrb, unsigned vece, int kind);
static void gen_vsum4_vec(int vrt, int vra, int vrb, int kind);
static void gen_vsum2sws_i64(int vrt, int vra, int vrb);
static void gen_vsumsws_i64(int vrt, int vra, int vrb);
static void gen_vso_i64(int vrt, int vra, int vrb, bool left);

static bool do_vmrg(DisasContext *ctx, arg_VX *a, unsigned vece, bool hi,
                    void (*helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr ra, rb, rd;
    REQUIRE_VECTOR(ctx);
    if (vmxperm(vece == MO_8 ? VP_MRGB : vece == MO_16 ? VP_MRGH : VP_MRGW)) {
        gen_vmrg_i64(a->vrt, a->vra, a->vrb, vece, hi);
        return true;
    }
    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    helper(rd, ra, rb);
    return true;
}

TRANS_FLAGS(ALTIVEC, VMRGHB, do_vmrg, MO_8, true, gen_helper_VMRGHB);
TRANS_FLAGS(ALTIVEC, VMRGHH, do_vmrg, MO_16, true, gen_helper_VMRGHH);
TRANS_FLAGS(ALTIVEC, VMRGHW, do_vmrg, MO_32, true, gen_helper_VMRGHW);
TRANS_FLAGS(ALTIVEC, VMRGLB, do_vmrg, MO_8, false, gen_helper_VMRGLB);
TRANS_FLAGS(ALTIVEC, VMRGLH, do_vmrg, MO_16, false, gen_helper_VMRGLH);
TRANS_FLAGS(ALTIVEC, VMRGLW, do_vmrg, MO_32, false, gen_helper_VMRGLW);

static bool do_vx_vaddsubm(DisasContext *ctx, arg_VX *a, MemOp vece,
                           void (*gen_op)(unsigned, uint32_t, uint32_t,
                                          uint32_t, uint32_t, uint32_t))
{
    REQUIRE_VECTOR(ctx);

    gen_op(vece,
           avr_full_offset(a->vrt),
           avr_full_offset(a->vra),
           avr_full_offset(a->vrb),
           16, 16);

    return true;
}

TRANS_FLAGS(ALTIVEC, VADDUBM, do_vx_vaddsubm, MO_8, tcg_gen_gvec_add)
TRANS_FLAGS(ALTIVEC, VADDUHM, do_vx_vaddsubm, MO_16, tcg_gen_gvec_add)
TRANS_FLAGS(ALTIVEC, VADDUWM, do_vx_vaddsubm, MO_32, tcg_gen_gvec_add)
TRANS_FLAGS2(ISA207, VADDUDM, do_vx_vaddsubm, MO_64, tcg_gen_gvec_add)

TRANS_FLAGS(ALTIVEC, VSUBUBM, do_vx_vaddsubm, MO_8, tcg_gen_gvec_sub)
TRANS_FLAGS(ALTIVEC, VSUBUHM, do_vx_vaddsubm, MO_16, tcg_gen_gvec_sub)
TRANS_FLAGS(ALTIVEC, VSUBUWM, do_vx_vaddsubm, MO_32, tcg_gen_gvec_sub)
TRANS_FLAGS2(ISA207, VSUBUDM, do_vx_vaddsubm, MO_64, tcg_gen_gvec_sub)

TRANS_FLAGS2(ISA207, VMULUWM, do_vx_vaddsubm, MO_32, tcg_gen_gvec_mul)

TRANS_FLAGS2(ISA300, VMUL10CUQ, do_vx_vmul10, false, true)
TRANS_FLAGS2(ISA300, VMUL10ECUQ, do_vx_vmul10, true, true)
TRANS_FLAGS2(ISA300, VMUL10UQ, do_vx_vmul10, false, false)
TRANS_FLAGS2(ISA300, VMUL10EUQ, do_vx_vmul10, true, false)

static void trans_vmrgew(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VA = rA(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv_i64 avr = tcg_temp_new_i64();

    get_avr64(avr, VB, true);
    tcg_gen_shri_i64(tmp, avr, 32);
    get_avr64(avr, VA, true);
    tcg_gen_deposit_i64(avr, avr, tmp, 0, 32);
    set_avr64(VT, avr, true);

    get_avr64(avr, VB, false);
    tcg_gen_shri_i64(tmp, avr, 32);
    get_avr64(avr, VA, false);
    tcg_gen_deposit_i64(avr, avr, tmp, 0, 32);
    set_avr64(VT, avr, false);
}

static void trans_vmrgow(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VA = rA(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 t0 = tcg_temp_new_i64();
    TCGv_i64 t1 = tcg_temp_new_i64();
    TCGv_i64 avr = tcg_temp_new_i64();

    get_avr64(t0, VB, true);
    get_avr64(t1, VA, true);
    tcg_gen_deposit_i64(avr, t0, t1, 32, 32);
    set_avr64(VT, avr, true);

    get_avr64(t0, VB, false);
    get_avr64(t1, VA, false);
    tcg_gen_deposit_i64(avr, t0, t1, 32, 32);
    set_avr64(VT, avr, false);
}

/*
 * lvsl VRT,RA,RB - Load Vector for Shift Left
 *
 * Let the EA be the sum (rA|0)+(rB). Let sh=EA[28–31].
 * Let X be the 32-byte value 0x00 || 0x01 || 0x02 || ... || 0x1E || 0x1F.
 * Bytes sh:sh+15 of X are placed into vD.
 */
static bool trans_LVSL(DisasContext *ctx, arg_LVSL *a)
{
    TCGv_i64 result = tcg_temp_new_i64();
    TCGv_i64 sh = tcg_temp_new_i64();
    TCGv EA = tcg_temp_new();

    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);

    /* Get sh(from description) by anding EA with 0xf. */
    EA = do_ea_calc(ctx, a->ra, cpu_gpr[a->rb]);
    tcg_gen_extu_tl_i64(sh, EA);
    tcg_gen_andi_i64(sh, sh, 0xfULL);

    /*
     * Create bytes sh:sh+7 of X(from description) and place them in
     * higher doubleword of vD.
     */
    tcg_gen_muli_i64(sh, sh, 0x0101010101010101ULL);
    tcg_gen_addi_i64(result, sh, 0x0001020304050607ull);
    set_avr64(a->rt, result, true);
    /*
     * Create bytes sh+8:sh+15 of X(from description) and place them in
     * lower doubleword of vD.
     */
    tcg_gen_addi_i64(result, sh, 0x08090a0b0c0d0e0fULL);
    set_avr64(a->rt, result, false);
    return true;
}

/*
 * lvsr VRT,RA,RB - Load Vector for Shift Right
 *
 * Let the EA be the sum (rA|0)+(rB). Let sh=EA[28–31].
 * Let X be the 32-byte value 0x00 || 0x01 || 0x02 || ... || 0x1E || 0x1F.
 * Bytes (16-sh):(31-sh) of X are placed into vD.
 */
static bool trans_LVSR(DisasContext *ctx, arg_LVSR *a)
{
    TCGv_i64 result = tcg_temp_new_i64();
    TCGv_i64 sh = tcg_temp_new_i64();
    TCGv EA = tcg_temp_new();

    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);

    /* Get sh(from description) by anding EA with 0xf. */
    EA = do_ea_calc(ctx, a->ra, cpu_gpr[a->rb]);
    tcg_gen_extu_tl_i64(sh, EA);
    tcg_gen_andi_i64(sh, sh, 0xfULL);

    /*
     * Create bytes (16-sh):(23-sh) of X(from description) and place them in
     * higher doubleword of vD.
     */
    tcg_gen_muli_i64(sh, sh, 0x0101010101010101ULL);
    tcg_gen_subfi_i64(result, 0x1011121314151617ULL, sh);
    set_avr64(a->rt, result, true);
    /*
     * Create bytes (24-sh):(32-sh) of X(from description) and place them in
     * lower doubleword of vD.
     */
    tcg_gen_subfi_i64(result, 0x18191a1b1c1d1e1fULL, sh);
    set_avr64(a->rt, result, false);
    return true;
}

/*
 * vsl VRT,VRA,VRB - Vector Shift Left
 *
 * Shifting left 128 bit value of vA by value specified in bits 125-127 of vB.
 * Lowest 3 bits in each byte element of register vB must be identical or
 * result is undefined.
 */
static void trans_vsl(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VA = rA(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 avr = tcg_temp_new_i64();
    TCGv_i64 sh = tcg_temp_new_i64();
    TCGv_i64 carry = tcg_temp_new_i64();
    TCGv_i64 tmp = tcg_temp_new_i64();

    /* Place bits 125-127 of vB in 'sh'. */
    get_avr64(avr, VB, false);
    tcg_gen_andi_i64(sh, avr, 0x07ULL);

    /*
     * Save highest 'sh' bits of lower doubleword element of vA in variable
     * 'carry' and perform shift on lower doubleword.
     */
    get_avr64(avr, VA, false);
    tcg_gen_subfi_i64(tmp, 32, sh);
    tcg_gen_shri_i64(carry, avr, 32);
    tcg_gen_shr_i64(carry, carry, tmp);
    tcg_gen_shl_i64(avr, avr, sh);
    set_avr64(VT, avr, false);

    /*
     * Perform shift on higher doubleword element of vA and replace lowest
     * 'sh' bits with 'carry'.
     */
    get_avr64(avr, VA, true);
    tcg_gen_shl_i64(avr, avr, sh);
    tcg_gen_or_i64(avr, avr, carry);
    set_avr64(VT, avr, true);
}

/*
 * vsr VRT,VRA,VRB - Vector Shift Right
 *
 * Shifting right 128 bit value of vA by value specified in bits 125-127 of vB.
 * Lowest 3 bits in each byte element of register vB must be identical or
 * result is undefined.
 */
static void trans_vsr(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VA = rA(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 avr = tcg_temp_new_i64();
    TCGv_i64 sh = tcg_temp_new_i64();
    TCGv_i64 carry = tcg_temp_new_i64();
    TCGv_i64 tmp = tcg_temp_new_i64();

    /* Place bits 125-127 of vB in 'sh'. */
    get_avr64(avr, VB, false);
    tcg_gen_andi_i64(sh, avr, 0x07ULL);

    /*
     * Save lowest 'sh' bits of higher doubleword element of vA in variable
     * 'carry' and perform shift on higher doubleword.
     */
    get_avr64(avr, VA, true);
    tcg_gen_subfi_i64(tmp, 32, sh);
    tcg_gen_shli_i64(carry, avr, 32);
    tcg_gen_shl_i64(carry, carry, tmp);
    tcg_gen_shr_i64(avr, avr, sh);
    set_avr64(VT, avr, true);
    /*
     * Perform shift on lower doubleword element of vA and replace highest
     * 'sh' bits with 'carry'.
     */
    get_avr64(avr, VA, false);
    tcg_gen_shr_i64(avr, avr, sh);
    tcg_gen_or_i64(avr, avr, carry);
    set_avr64(VT, avr, false);
}

/*
 * vgbbd VRT,VRB - Vector Gather Bits by Bytes by Doubleword
 *
 * All ith bits (i in range 1 to 8) of each byte of doubleword element in source
 * register are concatenated and placed into ith byte of appropriate doubleword
 * element in destination register.
 *
 * Following solution is done for both doubleword elements of source register
 * in parallel, in order to reduce the number of instructions needed(that's why
 * arrays are used):
 * First, both doubleword elements of source register vB are placed in
 * appropriate element of array avr. Bits are gathered in 2x8 iterations(2 for
 * loops). In first iteration bit 1 of byte 1, bit 2 of byte 2,... bit 8 of
 * byte 8 are in their final spots so avr[i], i={0,1} can be and-ed with
 * tcg_mask. For every following iteration, both avr[i] and tcg_mask variables
 * have to be shifted right for 7 and 8 places, respectively, in order to get
 * bit 1 of byte 2, bit 2 of byte 3.. bit 7 of byte 8 in their final spots so
 * shifted avr values(saved in tmp) can be and-ed with new value of tcg_mask...
 * After first 8 iteration(first loop), all the first bits are in their final
 * places, all second bits but second bit from eight byte are in their places...
 * only 1 eight bit from eight byte is in it's place). In second loop we do all
 * operations symmetrically, in order to get other half of bits in their final
 * spots. Results for first and second doubleword elements are saved in
 * result[0] and result[1] respectively. In the end those results are saved in
 * appropriate doubleword element of destination register vD.
 */
static void trans_vgbbd(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 tmp = tcg_temp_new_i64();
    uint64_t mask = 0x8040201008040201ULL;
    int i, j;

    TCGv_i64 result[2];
    result[0] = tcg_temp_new_i64();
    result[1] = tcg_temp_new_i64();
    TCGv_i64 avr[2];
    avr[0] = tcg_temp_new_i64();
    avr[1] = tcg_temp_new_i64();
    TCGv_i64 tcg_mask = tcg_temp_new_i64();

    tcg_gen_movi_i64(tcg_mask, mask);
    for (j = 0; j < 2; j++) {
        get_avr64(avr[j], VB, j);
        tcg_gen_and_i64(result[j], avr[j], tcg_mask);
    }
    for (i = 1; i < 8; i++) {
        tcg_gen_movi_i64(tcg_mask, mask >> (i * 8));
        for (j = 0; j < 2; j++) {
            tcg_gen_shri_i64(tmp, avr[j], i * 7);
            tcg_gen_and_i64(tmp, tmp, tcg_mask);
            tcg_gen_or_i64(result[j], result[j], tmp);
        }
    }
    for (i = 1; i < 8; i++) {
        tcg_gen_movi_i64(tcg_mask, mask << (i * 8));
        for (j = 0; j < 2; j++) {
            tcg_gen_shli_i64(tmp, avr[j], i * 7);
            tcg_gen_and_i64(tmp, tmp, tcg_mask);
            tcg_gen_or_i64(result[j], result[j], tmp);
        }
    }
    for (j = 0; j < 2; j++) {
        set_avr64(VT, result[j], j);
    }
}

/*
 * vclzw VRT,VRB - Vector Count Leading Zeros Word
 *
 * Counting the number of leading zero bits of each word element in source
 * register and placing result in appropriate word element of destination
 * register.
 */
static void trans_vclzw(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i32 tmp = tcg_temp_new_i32();
    int i;

    /* Perform count for every word element using tcg_gen_clzi_i32. */
    for (i = 0; i < 4; i++) {
        tcg_gen_ld_i32(tmp, tcg_env,
            offsetof(CPUPPCState, vsr[32 + VB].u64[0]) + i * 4);
        tcg_gen_clzi_i32(tmp, tmp, 32);
        tcg_gen_st_i32(tmp, tcg_env,
            offsetof(CPUPPCState, vsr[32 + VT].u64[0]) + i * 4);
    }
}

/*
 * vclzd VRT,VRB - Vector Count Leading Zeros Doubleword
 *
 * Counting the number of leading zero bits of each doubleword element in source
 * register and placing result in appropriate doubleword element of destination
 * register.
 */
static void trans_vclzd(DisasContext *ctx)
{
    int VT = rD(ctx->opcode);
    int VB = rB(ctx->opcode);
    TCGv_i64 avr = tcg_temp_new_i64();

    /* high doubleword */
    get_avr64(avr, VB, true);
    tcg_gen_clzi_i64(avr, avr, 64);
    set_avr64(VT, avr, true);

    /* low doubleword */
    get_avr64(avr, VB, false);
    tcg_gen_clzi_i64(avr, avr, 64);
    set_avr64(VT, avr, false);
}

GEN_VXFORM(vsrv, 2, 28);
GEN_VXFORM(vslv, 2, 29);

#define GEN_VXFORM_VSO(name, left)                                      \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (vmxperm(VP_SO)) {                                               \
        gen_vso_i64(rD(ctx->opcode), rA(ctx->opcode), rB(ctx->opcode),  \
                    left);                                              \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(gen_avr_ptr(rD(ctx->opcode)),                     \
                      gen_avr_ptr(rA(ctx->opcode)),                     \
                      gen_avr_ptr(rB(ctx->opcode)));                    \
}
GEN_VXFORM_VSO(vslo, true);
GEN_VXFORM_VSO(vsro, false);

static bool do_vector_gvec3_VX(DisasContext *ctx, arg_VX *a, int vece,
                               void (*gen_gvec)(unsigned, uint32_t, uint32_t,
                                                uint32_t, uint32_t, uint32_t))
{
    REQUIRE_VECTOR(ctx);

    gen_gvec(vece, avr_full_offset(a->vrt), avr_full_offset(a->vra),
             avr_full_offset(a->vrb), 16, 16);

    return true;
}

TRANS_FLAGS(ALTIVEC, VSLB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_shlv);
TRANS_FLAGS(ALTIVEC, VSLH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_shlv);
TRANS_FLAGS(ALTIVEC, VSLW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_shlv);
TRANS_FLAGS2(ISA207, VSLD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_shlv);

TRANS_FLAGS(ALTIVEC, VSRB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_shrv);
TRANS_FLAGS(ALTIVEC, VSRH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_shrv);
TRANS_FLAGS(ALTIVEC, VSRW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_shrv);
TRANS_FLAGS2(ISA207, VSRD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_shrv);

TRANS_FLAGS(ALTIVEC, VSRAB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_sarv);
TRANS_FLAGS(ALTIVEC, VSRAH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_sarv);
TRANS_FLAGS(ALTIVEC, VSRAW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_sarv);
TRANS_FLAGS2(ISA207, VSRAD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_sarv);

TRANS_FLAGS(ALTIVEC, VRLB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_rotlv)
TRANS_FLAGS(ALTIVEC, VRLH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_rotlv)
TRANS_FLAGS(ALTIVEC, VRLW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_rotlv)
TRANS_FLAGS2(ISA207, VRLD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_rotlv)

/* Logical operations */
TRANS_FLAGS(ALTIVEC, VAND, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_and);
TRANS_FLAGS(ALTIVEC, VANDC, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_andc);
TRANS_FLAGS(ALTIVEC, VOR, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_or);
TRANS_FLAGS(ALTIVEC, VXOR, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_xor);
TRANS_FLAGS(ALTIVEC, VNOR, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_nor);
TRANS_FLAGS2(ISA207, VEQV, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_eqv);
TRANS_FLAGS2(ISA207, VNAND, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_nand);
TRANS_FLAGS2(ISA207, VORC, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_orc);

/* Integer Max/Min operations */
TRANS_FLAGS(ALTIVEC, VMAXUB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_umax);
TRANS_FLAGS(ALTIVEC, VMAXUH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_umax);
TRANS_FLAGS(ALTIVEC, VMAXUW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_umax);
TRANS_FLAGS2(ISA207, VMAXUD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_umax);

TRANS_FLAGS(ALTIVEC, VMAXSB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_smax);
TRANS_FLAGS(ALTIVEC, VMAXSH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_smax);
TRANS_FLAGS(ALTIVEC, VMAXSW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_smax);
TRANS_FLAGS2(ISA207, VMAXSD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_smax);

TRANS_FLAGS(ALTIVEC, VMINUB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_umin);
TRANS_FLAGS(ALTIVEC, VMINUH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_umin);
TRANS_FLAGS(ALTIVEC, VMINUW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_umin);
TRANS_FLAGS2(ISA207, VMINUD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_umin);

TRANS_FLAGS(ALTIVEC, VMINSB, do_vector_gvec3_VX, MO_8, tcg_gen_gvec_smin);
TRANS_FLAGS(ALTIVEC, VMINSH, do_vector_gvec3_VX, MO_16, tcg_gen_gvec_smin);
TRANS_FLAGS(ALTIVEC, VMINSW, do_vector_gvec3_VX, MO_32, tcg_gen_gvec_smin);
TRANS_FLAGS2(ISA207, VMINSD, do_vector_gvec3_VX, MO_64, tcg_gen_gvec_smin);

static TCGv_vec do_vrl_mask_vec(unsigned vece, TCGv_vec vrb)
{
    TCGv_vec t0 = tcg_temp_new_vec_matching(vrb),
             t1 = tcg_temp_new_vec_matching(vrb),
             t2 = tcg_temp_new_vec_matching(vrb),
             ones = tcg_constant_vec_matching(vrb, vece, -1);

    /* Extract b and e */
    tcg_gen_dupi_vec(vece, t2, (8 << vece) - 1);

    tcg_gen_shri_vec(vece, t0, vrb, 16);
    tcg_gen_and_vec(vece, t0, t0, t2);

    tcg_gen_shri_vec(vece, t1, vrb, 8);
    tcg_gen_and_vec(vece, t1, t1, t2);

    /* Compare b and e to negate the mask where begin > end */
    tcg_gen_cmp_vec(TCG_COND_GT, vece, t2, t0, t1);

    /* Create the mask with (~0 >> b) ^ ((~0 >> e) >> 1) */
    tcg_gen_shrv_vec(vece, t0, ones, t0);
    tcg_gen_shrv_vec(vece, t1, ones, t1);
    tcg_gen_shri_vec(vece, t1, t1, 1);
    tcg_gen_xor_vec(vece, t0, t0, t1);

    /* negate the mask */
    tcg_gen_xor_vec(vece, t0, t0, t2);

    return t0;
}

static void gen_vrlnm_vec(unsigned vece, TCGv_vec vrt, TCGv_vec vra,
                          TCGv_vec vrb)
{
    TCGv_vec mask, n = tcg_temp_new_vec_matching(vrt);

    /* Create the mask */
    mask = do_vrl_mask_vec(vece, vrb);

    /* Extract n */
    tcg_gen_dupi_vec(vece, n, (8 << vece) - 1);
    tcg_gen_and_vec(vece, n, vrb, n);

    /* Rotate and mask */
    tcg_gen_rotlv_vec(vece, vrt, vra, n);
    tcg_gen_and_vec(vece, vrt, vrt, mask);
}

static bool do_vrlnm(DisasContext *ctx, arg_VX *a, int vece)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_cmp_vec, INDEX_op_rotlv_vec, INDEX_op_sari_vec,
        INDEX_op_shli_vec, INDEX_op_shri_vec, INDEX_op_shrv_vec, 0
    };
    static const GVecGen3 ops[2] = {
        {
            .fniv = gen_vrlnm_vec,
            .fno = gen_helper_VRLWNM,
            .opt_opc = vecop_list,
            .load_dest = true,
            .vece = MO_32
        },
        {
            .fniv = gen_vrlnm_vec,
            .fno = gen_helper_VRLDNM,
            .opt_opc = vecop_list,
            .load_dest = true,
            .vece = MO_64
        }
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VSX(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &ops[vece - 2]);

    return true;
}

TRANS(VRLWNM, do_vrlnm, MO_32)
TRANS(VRLDNM, do_vrlnm, MO_64)

static void gen_vrlmi_vec(unsigned vece, TCGv_vec vrt, TCGv_vec vra,
                          TCGv_vec vrb)
{
    TCGv_vec mask, n = tcg_temp_new_vec_matching(vrt),
             tmp = tcg_temp_new_vec_matching(vrt);

    /* Create the mask */
    mask = do_vrl_mask_vec(vece, vrb);

    /* Extract n */
    tcg_gen_dupi_vec(vece, n, (8 << vece) - 1);
    tcg_gen_and_vec(vece, n, vrb, n);

    /* Rotate and insert */
    tcg_gen_rotlv_vec(vece, tmp, vra, n);
    tcg_gen_bitsel_vec(vece, vrt, mask, tmp, vrt);
}

static bool do_vrlmi(DisasContext *ctx, arg_VX *a, int vece)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_cmp_vec, INDEX_op_rotlv_vec, INDEX_op_sari_vec,
        INDEX_op_shli_vec, INDEX_op_shri_vec, INDEX_op_shrv_vec, 0
    };
    static const GVecGen3 ops[2] = {
        {
            .fniv = gen_vrlmi_vec,
            .fno = gen_helper_VRLWMI,
            .opt_opc = vecop_list,
            .load_dest = true,
            .vece = MO_32
        },
        {
            .fniv = gen_vrlnm_vec,
            .fno = gen_helper_VRLDMI,
            .opt_opc = vecop_list,
            .load_dest = true,
            .vece = MO_64
        }
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VSX(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &ops[vece - 2]);

    return true;
}

TRANS(VRLWMI, do_vrlmi, MO_32)
TRANS(VRLDMI, do_vrlmi, MO_64)

static bool do_vector_shift_quad(DisasContext *ctx, arg_VX *a, bool right,
                                 bool alg)
{
    TCGv_i64 hi, lo, t0, t1, n, zero = tcg_constant_i64(0);

    REQUIRE_VECTOR(ctx);

    n = tcg_temp_new_i64();
    hi = tcg_temp_new_i64();
    lo = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();

    get_avr64(lo, a->vra, false);
    get_avr64(hi, a->vra, true);

    get_avr64(n, a->vrb, true);

    tcg_gen_andi_i64(t0, n, 64);
    if (right) {
        tcg_gen_movcond_i64(TCG_COND_NE, lo, t0, zero, hi, lo);
        if (alg) {
            t1 = tcg_temp_new_i64();
            tcg_gen_sari_i64(t1, lo, 63);
        } else {
            t1 = zero;
        }
        tcg_gen_movcond_i64(TCG_COND_NE, hi, t0, zero, t1, hi);
    } else {
        tcg_gen_movcond_i64(TCG_COND_NE, hi, t0, zero, lo, hi);
        tcg_gen_movcond_i64(TCG_COND_NE, lo, t0, zero, zero, lo);
    }
    tcg_gen_andi_i64(n, n, 0x3F);

    if (right) {
        if (alg) {
            tcg_gen_sar_i64(t0, hi, n);
        } else {
            tcg_gen_shr_i64(t0, hi, n);
        }
    } else {
        tcg_gen_shl_i64(t0, lo, n);
    }
    set_avr64(a->vrt, t0, right);

    if (right) {
        tcg_gen_shr_i64(lo, lo, n);
    } else {
        tcg_gen_shl_i64(hi, hi, n);
    }
    tcg_gen_xori_i64(n, n, 63);
    if (right) {
        tcg_gen_shl_i64(hi, hi, n);
        tcg_gen_shli_i64(hi, hi, 1);
    } else {
        tcg_gen_shr_i64(lo, lo, n);
        tcg_gen_shri_i64(lo, lo, 1);
    }
    tcg_gen_or_i64(hi, hi, lo);
    set_avr64(a->vrt, hi, !right);
    return true;
}

TRANS_FLAGS2(ISA310, VSLQ, do_vector_shift_quad, false, false);
TRANS_FLAGS2(ISA310, VSRQ, do_vector_shift_quad, true, false);
TRANS_FLAGS2(ISA310, VSRAQ, do_vector_shift_quad, true, true);

static void do_vrlq_mask(TCGv_i64 mh, TCGv_i64 ml, TCGv_i64 b, TCGv_i64 e)
{
    TCGv_i64 th, tl, t0, t1, zero = tcg_constant_i64(0),
             ones = tcg_constant_i64(-1);

    th = tcg_temp_new_i64();
    tl = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    /* m = ~0 >> b */
    tcg_gen_andi_i64(t0, b, 64);
    tcg_gen_movcond_i64(TCG_COND_NE, t1, t0, zero, zero, ones);
    tcg_gen_andi_i64(t0, b, 0x3F);
    tcg_gen_shr_i64(mh, t1, t0);
    tcg_gen_shr_i64(ml, ones, t0);
    tcg_gen_xori_i64(t0, t0, 63);
    tcg_gen_shl_i64(t1, t1, t0);
    tcg_gen_shli_i64(t1, t1, 1);
    tcg_gen_or_i64(ml, t1, ml);

    /* t = ~0 >> e */
    tcg_gen_andi_i64(t0, e, 64);
    tcg_gen_movcond_i64(TCG_COND_NE, t1, t0, zero, zero, ones);
    tcg_gen_andi_i64(t0, e, 0x3F);
    tcg_gen_shr_i64(th, t1, t0);
    tcg_gen_shr_i64(tl, ones, t0);
    tcg_gen_xori_i64(t0, t0, 63);
    tcg_gen_shl_i64(t1, t1, t0);
    tcg_gen_shli_i64(t1, t1, 1);
    tcg_gen_or_i64(tl, t1, tl);

    /* t = t >> 1 */
    tcg_gen_extract2_i64(tl, tl, th, 1);
    tcg_gen_shri_i64(th, th, 1);

    /* m = m ^ t */
    tcg_gen_xor_i64(mh, mh, th);
    tcg_gen_xor_i64(ml, ml, tl);

    /* Negate the mask if begin > end */
    tcg_gen_movcond_i64(TCG_COND_GT, t0, b, e, ones, zero);

    tcg_gen_xor_i64(mh, mh, t0);
    tcg_gen_xor_i64(ml, ml, t0);
}

static bool do_vector_rotl_quad(DisasContext *ctx, arg_VX *a, bool mask,
                                bool insert)
{
    TCGv_i64 ah, al, vrb, n, t0, t1, zero = tcg_constant_i64(0);

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    ah = tcg_temp_new_i64();
    al = tcg_temp_new_i64();
    vrb = tcg_temp_new_i64();
    n = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    get_avr64(ah, a->vra, true);
    get_avr64(al, a->vra, false);
    get_avr64(vrb, a->vrb, true);

    tcg_gen_mov_i64(t0, ah);
    tcg_gen_andi_i64(t1, vrb, 64);
    tcg_gen_movcond_i64(TCG_COND_NE, ah, t1, zero, al, ah);
    tcg_gen_movcond_i64(TCG_COND_NE, al, t1, zero, t0, al);
    tcg_gen_andi_i64(n, vrb, 0x3F);

    tcg_gen_shl_i64(t0, ah, n);
    tcg_gen_shl_i64(t1, al, n);

    tcg_gen_xori_i64(n, n, 63);

    tcg_gen_shr_i64(al, al, n);
    tcg_gen_shri_i64(al, al, 1);
    tcg_gen_or_i64(t0, al, t0);

    tcg_gen_shr_i64(ah, ah, n);
    tcg_gen_shri_i64(ah, ah, 1);
    tcg_gen_or_i64(t1, ah, t1);

    if (mask || insert) {
        tcg_gen_extract_i64(n, vrb, 8, 7);
        tcg_gen_extract_i64(vrb, vrb, 16, 7);

        do_vrlq_mask(ah, al, vrb, n);

        tcg_gen_and_i64(t0, t0, ah);
        tcg_gen_and_i64(t1, t1, al);

        if (insert) {
            get_avr64(n, a->vrt, true);
            get_avr64(vrb, a->vrt, false);
            tcg_gen_andc_i64(n, n, ah);
            tcg_gen_andc_i64(vrb, vrb, al);
            tcg_gen_or_i64(t0, t0, n);
            tcg_gen_or_i64(t1, t1, vrb);
        }
    }

    set_avr64(a->vrt, t0, true);
    set_avr64(a->vrt, t1, false);
    return true;
}

TRANS(VRLQ, do_vector_rotl_quad, false, false)
TRANS(VRLQNM, do_vector_rotl_quad, true, false)
TRANS(VRLQMI, do_vector_rotl_quad, false, true)

GEN_VXFORM_TRANS(vsl, 2, 7);
GEN_VXFORM_TRANS(vsr, 2, 11);
static bool do_vpk_env(DisasContext *ctx, arg_VX *a,
                       void (*gen_helper)(TCGv_ptr, TCGv_ptr,
                                          TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr ra, rb, rd;
    REQUIRE_VECTOR(ctx);
    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    gen_helper(tcg_env, rd, ra, rb);
    return true;
}

static bool do_vpkum(DisasContext *ctx, arg_VX *a, unsigned vece,
                     void (*gen_helper)(TCGv_ptr, TCGv_ptr,
                                        TCGv_ptr, TCGv_ptr))
{
    REQUIRE_VECTOR(ctx);
    if (vmxperm(VP_PKM)) {
        gen_vpkum_i64(a->vrt, a->vra, a->vrb, vece);
        return true;
    }
    return do_vpk_env(ctx, a, gen_helper);
}

static bool do_vpks(DisasContext *ctx, arg_VX *a, unsigned vece, int kind,
                    void (*gen_helper)(TCGv_ptr, TCGv_ptr,
                                       TCGv_ptr, TCGv_ptr))
{
    REQUIRE_VECTOR(ctx);
    if (vmxperm(kind == VPK_US && vece == MO_32 ? VP_PKUWUS : VP_PKS)) {
        gen_vpk_vec(a->vrt, a->vra, a->vrb, vece, kind);
        return true;
    }
    return do_vpk_env(ctx, a, gen_helper);
}

static bool do_vpk(DisasContext *ctx, arg_VX *a,
                   void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr ra, rb, rd;
    REQUIRE_VECTOR(ctx);
    if (vmxperm(VP_PKPX)) {
        gen_vpk_vec(a->vrt, a->vra, a->vrb, MO_32, VPK_PX);
        return true;
    }
    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    gen_helper(rd, ra, rb);
    return true;
}

TRANS_FLAGS(ALTIVEC, VPKUHUM, do_vpkum, MO_16, gen_helper_VPKUHUM)
TRANS_FLAGS(ALTIVEC, VPKUWUM, do_vpkum, MO_32, gen_helper_VPKUWUM)
TRANS_FLAGS2(ISA207, VPKUDUM, do_vpk_env, gen_helper_VPKUDUM)
TRANS_FLAGS(ALTIVEC, VPKUHUS, do_vpks, MO_16, VPK_US, gen_helper_VPKUHUS)
TRANS_FLAGS(ALTIVEC, VPKUWUS, do_vpks, MO_32, VPK_US, gen_helper_VPKUWUS)
TRANS_FLAGS2(ISA207, VPKUDUS, do_vpk_env, gen_helper_VPKUDUS)
TRANS_FLAGS(ALTIVEC, VPKSHUS, do_vpks, MO_16, VPK_SU, gen_helper_VPKSHUS)
TRANS_FLAGS(ALTIVEC, VPKSWUS, do_vpks, MO_32, VPK_SU, gen_helper_VPKSWUS)
TRANS_FLAGS2(ISA207, VPKSDUS, do_vpk_env, gen_helper_VPKSDUS)
TRANS_FLAGS(ALTIVEC, VPKSHSS, do_vpks, MO_16, VPK_SS, gen_helper_VPKSHSS)
TRANS_FLAGS(ALTIVEC, VPKSWSS, do_vpks, MO_32, VPK_SS, gen_helper_VPKSWSS)
TRANS_FLAGS2(ISA207, VPKSDSS, do_vpk_env, gen_helper_VPKSDSS)
TRANS_FLAGS(ALTIVEC, VPKPX, do_vpk, gen_helper_VPKPX)

#define GEN_VXFORM_VSUM(name, cond, native)                             \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    int vrt = rD(ctx->opcode), vra = rA(ctx->opcode);                   \
    int vrb = rB(ctx->opcode);                                          \
                                                                        \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (cond) {                                                         \
        native;                                                         \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(tcg_env, gen_avr_ptr(vrt), gen_avr_ptr(vra),      \
                      gen_avr_ptr(vrb));                                \
}
GEN_VXFORM_VSUM(vsum4ubs, vmxperm(VP_SUM4),
                gen_vsum4_vec(vrt, vra, vrb, VSUM_UB));
GEN_VXFORM_VSUM(vsum4sbs, vmxperm(VP_SUM4),
                gen_vsum4_vec(vrt, vra, vrb, VSUM_SB));
GEN_VXFORM_VSUM(vsum4shs, vmxperm(VP_SUM4),
                gen_vsum4_vec(vrt, vra, vrb, VSUM_SH));
GEN_VXFORM_VSUM(vsum2sws, vmxperm(VP_SUMS), gen_vsum2sws_i64(vrt, vra, vrb));
GEN_VXFORM_VSUM(vsumsws, vmxperm(VP_SUMS), gen_vsumsws_i64(vrt, vra, vrb));
/*
 * Single precision arithmetic on the host (vfparith_vec).  Vectors whose
 * host result might differ from the helper's for either VSCR[NJ] (NaNs,
 * subnormal operands, results below 2^-125) get the helper's result
 * from ppc_vfparith_ref().  PPC_NATIVE_VMX=0 keeps the helpers.
 */
static TCGv_vec native_avr(int regno)
{
    TCGv_vec v = tcg_temp_new_vec(TCG_TYPE_V128);

    tcg_gen_ld_vec(v, tcg_env, avr_full_offset(regno));
    return v;
}

/* vrt = desc(vra, vrb, vrc) */
static void native_vmx_op(unsigned desc, int vrt, int vra, int vrb, int vrc)
{
    TCGv_vec r = tcg_temp_new_vec(TCG_TYPE_V128);
    TCGv_vec vb = native_avr(vrb);
    TCGv_vec va = vra >= 0 ? native_avr(vra) : vb;
    TCGv_vec vc = vrc >= 0 ? native_avr(vrc) : vb;

    tcg_gen_vfparith_vec(desc, r, va, vb, vc);
    tcg_gen_st_vec(r, tcg_env, avr_full_offset(vrt));
}

/*
 * Integer multiply and multiply-sum ops as host vector ops, with the
 * helpers' results and VSCR[SAT].  PPC_NATIVE_VMX=0 keeps the helpers.
 * An even element is the high half of the wider host lane.
 */
static const TCGOpcode vmxint_list[] = {
    INDEX_op_add_vec, INDEX_op_mul_vec, INDEX_op_shli_vec,
    INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_cmp_vec,
    INDEX_op_smin_vec, INDEX_op_smax_vec, 0
};

static const TCGOpcode vmxint_list_ss[] = {
    INDEX_op_add_vec, INDEX_op_mul_vec, INDEX_op_shli_vec,
    INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_cmp_vec,
    INDEX_op_ssadd_vec, 0
};

/* the multiply-sums by host pair products */
static const TCGOpcode vmxint_list_dot[] = {
    INDEX_op_add_vec, INDEX_op_dot2s_vec, INDEX_op_dot2u_vec, 0
};

static const TCGOpcode vmxint_list_dot_ss[] = {
    INDEX_op_add_vec, INDEX_op_dot2s_vec, INDEX_op_shri_vec,
    INDEX_op_cmp_vec, INDEX_op_ssadd_vec, 0
};

static bool vmxint_ok(const TCGOpcode *list)
{
    return native_vmxint_on
           && tcg_op_supported(INDEX_op_add_vec, TCG_TYPE_V128, 0)
           && tcg_can_emit_vecop_list(list, TCG_TYPE_V128, MO_16)
           && tcg_can_emit_vecop_list(list, TCG_TYPE_V128, MO_32);
}

/* vmh[r]addshs by host rounded high products */
static const TCGOpcode vmxint_list_mh[] = {
    INDEX_op_add_vec, INDEX_op_sub_vec, INDEX_op_neg_vec, INDEX_op_mul_vec,
    INDEX_op_shli_vec, INDEX_op_sari_vec, INDEX_op_sssub_vec,
    INDEX_op_mulhrs_vec, 0
};

static bool vmxint_ok_at(const TCGOpcode *list, unsigned vece)
{
    return native_vmxint_on
           && tcg_op_supported(INDEX_op_add_vec, TCG_TYPE_V128, 0)
           && tcg_can_emit_vecop_list(list, TCG_TYPE_V128, vece);
}

typedef void VmxIntFn(TCGv_vec t, TCGv_vec a, TCGv_vec b, TCGv_vec c,
                      TCGv_vec sat);

static TCGv_vec vi_new(void)
{
    return tcg_temp_new_vec(TCG_TYPE_V128);
}

static TCGv_vec vi_k(unsigned vece, int64_t v)
{
    return tcg_constant_vec(TCG_TYPE_V128, vece, v);
}

/* half of each vece lane, as a vece lane: high half if hi */
static TCGv_vec vi_half(unsigned vece, TCGv_vec a, bool hi, bool sign)
{
    int n = 4 << vece;
    TCGv_vec r = vi_new();

    if (hi) {
        (sign ? tcg_gen_sari_vec : tcg_gen_shri_vec)(vece, r, a, n);
    } else {
        tcg_gen_shli_vec(vece, r, a, n);
        (sign ? tcg_gen_sari_vec : tcg_gen_shri_vec)(vece, r, r, n);
    }
    return r;
}

/* products of the even (hi) or odd halves */
static TCGv_vec vi_mul(unsigned vece, TCGv_vec a, TCGv_vec b, bool hi,
                       bool sa, bool sb)
{
    TCGv_vec r = vi_half(vece, a, hi, sa);

    tcg_gen_mul_vec(vece, r, r, vi_half(vece, b, hi, sb));
    return r;
}

/* r = a + b signed saturating; sat |= overflow */
static void vi_ssadd32(TCGv_vec r, TCGv_vec a, TCGv_vec b, TCGv_vec sat)
{
    TCGv_vec s = vi_new();

    tcg_gen_add_vec(MO_32, s, a, b);
    tcg_gen_ssadd_vec(MO_32, r, a, b);
    tcg_gen_cmp_vec(TCG_COND_NE, MO_32, s, s, r);
    tcg_gen_or_vec(MO_32, sat, sat, s);
}

/* r = a + b unsigned saturating; sat |= overflow */
static void vi_usadd32(TCGv_vec r, TCGv_vec a, TCGv_vec b, TCGv_vec sat)
{
    TCGv_vec s = vi_new(), o = vi_new();

    tcg_gen_add_vec(MO_32, s, a, b);
    tcg_gen_cmp_vec(TCG_COND_LTU, MO_32, o, s, a);
    tcg_gen_or_vec(MO_32, r, s, o);
    tcg_gen_or_vec(MO_32, sat, sat, o);
}

static void gen_vmsumuhm_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_add_vec(MO_32, t, vi_mul(MO_32, a, b, true, false, false), c);
    tcg_gen_add_vec(MO_32, t, t, vi_mul(MO_32, a, b, false, false, false));
}

static void gen_vmsumshm_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_add_vec(MO_32, t, vi_mul(MO_32, a, b, true, true, true), c);
    tcg_gen_add_vec(MO_32, t, t, vi_mul(MO_32, a, b, false, true, true));
}

static void gen_vmsumuhm_dot(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_dot2u_vec(MO_32, t, a, b);
    tcg_gen_add_vec(MO_32, t, t, c);
}

static void gen_vmsumshm_dot(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_dot2s_vec(MO_32, t, a, b);
    tcg_gen_add_vec(MO_32, t, t, c);
}

/* four byte products per word; sa: a signed */
static void gen_vmsumb(TCGv_vec t, TCGv_vec a, TCGv_vec b, TCGv_vec c,
                       bool sa)
{
    TCGv_vec pe = vi_mul(MO_16, a, b, true, sa, false);
    TCGv_vec po = vi_mul(MO_16, a, b, false, sa, false);

    tcg_gen_add_vec(MO_32, t, vi_half(MO_32, pe, true, sa), c);
    tcg_gen_add_vec(MO_32, t, t, vi_half(MO_32, pe, false, sa));
    tcg_gen_add_vec(MO_32, t, t, vi_half(MO_32, po, true, sa));
    tcg_gen_add_vec(MO_32, t, t, vi_half(MO_32, po, false, sa));
}

static void gen_vmsumubm_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    gen_vmsumb(t, a, b, c, false);
}

static void gen_vmsummbm_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    gen_vmsumb(t, a, b, c, true);
}

static void gen_vmsumuhs_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    vi_usadd32(t, vi_mul(MO_32, a, b, true, false, false),
               vi_mul(MO_32, a, b, false, false, false), sat);
    vi_usadd32(t, t, c, sat);
}

/* t = pair sum t + c, signed saturating */
static void vi_msumshs(TCGv_vec t, TCGv_vec c, TCGv_vec sat)
{
    TCGv_vec m = vi_new();

    /* the pair sum wraps only for 2^31: take 2^31 - 1, add 1 later */
    tcg_gen_cmp_vec(TCG_COND_EQ, MO_32, m, t, vi_k(MO_32, INT32_MIN));
    tcg_gen_add_vec(MO_32, t, t, m);
    vi_ssadd32(t, t, c, sat);
    tcg_gen_shri_vec(MO_32, m, m, 31);
    vi_ssadd32(t, t, m, sat);
}

static void gen_vmsumshs_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_add_vec(MO_32, t, vi_mul(MO_32, a, b, true, true, true),
                    vi_mul(MO_32, a, b, false, true, true));
    vi_msumshs(t, c, sat);
}

static void gen_vmsumshs_dot(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    tcg_gen_dot2s_vec(MO_32, t, a, b);
    vi_msumshs(t, c, sat);
}

static void gen_vmhaddshs(TCGv_vec t, TCGv_vec a, TCGv_vec b, TCGv_vec c,
                          TCGv_vec sat, bool round)
{
    TCGv_vec h[2];
    TCGv_vec o = vi_new();

    for (int i = 0; i < 2; i++) {
        TCGv_vec p = vi_mul(MO_32, a, b, !i, true, true);
        TCGv_vec r = vi_half(MO_32, c, !i, true);

        if (round) {
            tcg_gen_add_vec(MO_32, p, p, vi_k(MO_32, 0x4000));
        }
        tcg_gen_sari_vec(MO_32, p, p, 15);
        tcg_gen_add_vec(MO_32, r, r, p);
        tcg_gen_smin_vec(MO_32, p, r, vi_k(MO_32, INT16_MAX));
        tcg_gen_smax_vec(MO_32, p, p, vi_k(MO_32, INT16_MIN));
        tcg_gen_cmp_vec(TCG_COND_NE, MO_32, o, p, r);
        tcg_gen_or_vec(MO_32, sat, sat, o);
        h[i] = p;
    }
    tcg_gen_shli_vec(MO_32, t, h[0], 16);
    tcg_gen_shli_vec(MO_32, h[1], h[1], 16);
    tcg_gen_shri_vec(MO_32, h[1], h[1], 16);
    tcg_gen_or_vec(MO_32, t, t, h[1]);
}

static void gen_vmhaddshs_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                              TCGv_vec c, TCGv_vec sat)
{
    gen_vmhaddshs(t, a, b, c, sat, false);
}

static void gen_vmhraddshs_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                               TCGv_vec c, TCGv_vec sat)
{
    gen_vmhaddshs(t, a, b, c, sat, true);
}

/* the same from the rounded high product, exact for 0x8000 * 0x8000 */
static void gen_vmhaddshs_mh(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat, bool round)
{
    TCGv_vec m = vi_new(), w = vi_new();

    tcg_gen_mulhrs_vec(MO_16, m, a, b);
    if (!round) {
        /* less 1 where bit 14 of the product is set */
        tcg_gen_mul_vec(MO_16, w, a, b);
        tcg_gen_shli_vec(MO_16, w, w, 1);
        tcg_gen_sari_vec(MO_16, w, w, 15);
        tcg_gen_add_vec(MO_16, m, m, w);
    }
    /* m wraps only for 2^15, -m is exact */
    tcg_gen_neg_vec(MO_16, m, m);
    tcg_gen_sssub_vec(MO_16, t, c, m);
    tcg_gen_sub_vec(MO_16, w, c, m);
    tcg_gen_xor_vec(MO_16, w, w, t);
    tcg_gen_or_vec(MO_16, sat, sat, w);
}

static void gen_vmhaddshs_rs(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                             TCGv_vec c, TCGv_vec sat)
{
    gen_vmhaddshs_mh(t, a, b, c, sat, false);
}

static void gen_vmhraddshs_rs(TCGv_vec t, TCGv_vec a, TCGv_vec b,
                              TCGv_vec c, TCGv_vec sat)
{
    gen_vmhaddshs_mh(t, a, b, c, sat, true);
}

/* vrt = fn(vra, vrb, vrc); vrc < 0: none */
static void vmxint_gen(const TCGOpcode *list, VmxIntFn *fn, bool sat,
                       int vrt, int vra, int vrb, int vrc)
{
    const TCGOpcode *hold = tcg_swap_vecop_list(list);
    TCGv_vec t = vi_new();
    TCGv_vec a = native_avr(vra);
    TCGv_vec b = native_avr(vrb);
    TCGv_vec c = vrc >= 0 ? native_avr(vrc) : NULL;
    TCGv_vec s = NULL;

    if (sat) {
        s = vi_new();
        tcg_gen_dupi_vec(MO_32, s, 0);
    }
    fn(t, a, b, c, s);
    tcg_gen_st_vec(t, tcg_env, avr_full_offset(vrt));
    if (sat) {
        TCGv_vec v = vi_new();

        tcg_gen_ld_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat));
        tcg_gen_or_vec(MO_32, v, v, s);
        tcg_gen_st_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat));
    }
    tcg_swap_vecop_list(hold);
}

/*
 * Merges, packs, unpacks and sums on the host, with the helpers' results
 * and VSCR[SAT]: as 64-bit halves (VP_I) or host vector ops (VP_V).  The
 * vector ops move data between halves by a dup load of one doubleword
 * and a select with vmx_lanes.  Per host the faster of these and the
 * helper; other hosts take the x86 choice.  PPC_NATIVE_VMX=0 keeps the
 * helpers.
 */
enum { VP_H, VP_V, VP_I };

static const uint8_t vmxperm_how[VP_N] = {
#ifdef __aarch64__
    [VP_MRGB] = VP_H, [VP_MRGH] = VP_I, [VP_MRGW] = VP_I,
    [VP_UPKH] = VP_I, [VP_UPKPX] = VP_H,
    [VP_PKM] = VP_H, [VP_PKS] = VP_H, [VP_PKUWUS] = VP_H, [VP_PKPX] = VP_H,
    [VP_SUM4] = VP_V, [VP_SUMS] = VP_I, [VP_SO] = VP_I,
#else
    [VP_MRGB] = VP_I, [VP_MRGH] = VP_I, [VP_MRGW] = VP_I,
    [VP_UPKH] = VP_I, [VP_UPKPX] = VP_V,
    [VP_PKM] = VP_I, [VP_PKS] = VP_V, [VP_PKUWUS] = VP_H, [VP_PKPX] = VP_V,
    [VP_SUM4] = VP_V, [VP_SUMS] = VP_I, [VP_SO] = VP_I,
#endif
};

static const TCGOpcode vmxperm_req[] = {
    INDEX_op_add_vec, INDEX_op_sub_vec, INDEX_op_shli_vec,
    INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_cmp_vec,
    INDEX_op_umin_vec, INDEX_op_smin_vec, INDEX_op_smax_vec, 0
};

static const TCGOpcode vmxperm_list[] = {
    INDEX_op_add_vec, INDEX_op_sub_vec, INDEX_op_shli_vec,
    INDEX_op_shri_vec, INDEX_op_sari_vec, INDEX_op_cmp_vec,
    INDEX_op_umin_vec, INDEX_op_smin_vec, INDEX_op_smax_vec,
    INDEX_op_ssadd_vec, 0
};

static const TCGOpcode vmxperm_list64[] = {
    INDEX_op_shli_vec, INDEX_op_shri_vec, 0
};

static bool vmxperm(int op)
{
    if (!native_vmxint_on || vmxperm_how[op] == VP_H) {
        return false;
    }
    return vmxperm_how[op] == VP_I
           || (tcg_op_supported(INDEX_op_add_vec, TCG_TYPE_V128, 0)
               && tcg_can_emit_vecop_list(vmxperm_req, TCG_TYPE_V128, MO_16)
               && tcg_can_emit_vecop_list(vmxperm_req, TCG_TYPE_V128, MO_32)
               && tcg_can_emit_vecop_list(vmxperm_list64, TCG_TYPE_V128,
                                          MO_64));
}

/* both lanes: doubleword hi/lo of reg */
static TCGv_vec vi_dupd(int reg, bool hi)
{
    TCGv_vec r = vi_new();

    tcg_gen_dup_mem_vec(MO_64, r, tcg_env, avr64_offset(reg, hi));
    return r;
}

static TCGv_vec vi_lanes(void)
{
    TCGv_vec m = vi_new();

    tcg_gen_ld_vec(m, tcg_env, offsetof(CPUPPCState, vmx_lanes));
    return m;
}

/* high lane of h, low lane of l */
static TCGv_vec vi_sel(TCGv_vec m, TCGv_vec h, TCGv_vec l)
{
    TCGv_vec r = vi_new();

    tcg_gen_bitsel_vec(MO_64, r, m, h, l);
    return r;
}

static void vi_or_sat(TCGv_vec sat)
{
    TCGv_vec v = vi_new();

    tcg_gen_ld_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat));
    tcg_gen_or_vec(MO_32, v, v, sat);
    tcg_gen_st_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat));
}

static void vi_or_sat_i64(TCGv_i64 sat)
{
    TCGv_i64 v = tcg_temp_new_i64();
    int ofs = offsetof(CPUPPCState, vscr_sat.u64[0]);

    tcg_gen_ld_i64(v, tcg_env, ofs);
    tcg_gen_or_i64(v, v, sat);
    tcg_gen_st_i64(v, tcg_env, ofs);
}

/* swap the bits under m with those n above them */
static void vi_swap_i64(TCGv_i64 x, int n, uint64_t m)
{
    TCGv_i64 t = tcg_temp_new_i64();

    tcg_gen_shri_i64(t, x, n);
    tcg_gen_xor_i64(t, t, x);
    tcg_gen_andi_i64(t, t, m);
    tcg_gen_xor_i64(x, x, t);
    tcg_gen_shli_i64(t, t, n);
    tcg_gen_xor_i64(x, x, t);
}

/* vmrgh (hi) / vmrgl of vece elements */
static void gen_vmrg_i64(int vrt, int vra, int vrb, unsigned vece, bool hi)
{
    TCGv_i64 a = tcg_temp_new_i64(), b = tcg_temp_new_i64();
    TCGv_i64 x = tcg_temp_new_i64(), y = tcg_temp_new_i64();

    get_avr64(a, vra, hi);
    get_avr64(b, vrb, hi);
    /* a.w0:b.w0, a.w1:b.w1 */
    tcg_gen_shri_i64(x, b, 32);
    tcg_gen_deposit_i64(x, a, x, 0, 32);
    tcg_gen_deposit_i64(y, b, a, 32, 32);
    if (vece <= MO_16) {
        vi_swap_i64(x, 16, 0x00000000ffff0000ull);
        vi_swap_i64(y, 16, 0x00000000ffff0000ull);
    }
    if (vece == MO_8) {
        vi_swap_i64(x, 8, 0x0000ff000000ff00ull);
        vi_swap_i64(y, 8, 0x0000ff000000ff00ull);
    }
    set_avr64(vrt, x, true);
    set_avr64(vrt, y, false);
}

/* vupkhsh (hi) / vupklsh */
static void gen_vupkh_i64(int vrt, int vrb, bool hi)
{
    TCGv_i64 x = tcg_temp_new_i64(), t = tcg_temp_new_i64();
    TCGv_i64 r = tcg_temp_new_i64(), s = tcg_temp_new_i64();

    get_avr64(x, vrb, hi);
    tcg_gen_sextract_i64(r, x, 32, 16);
    tcg_gen_sari_i64(t, x, 48);
    tcg_gen_deposit_i64(r, r, t, 32, 32);
    tcg_gen_ext16s_i64(s, x);
    tcg_gen_sextract_i64(t, x, 16, 16);
    tcg_gen_deposit_i64(s, s, t, 32, 32);
    set_avr64(vrt, r, true);
    set_avr64(vrt, s, false);
}

/* vupkhpx (hi) / vupklpx */
static void gen_vupkpx_vec(int vrt, int vrb, bool hi)
{
    const TCGOpcode *hold = tcg_swap_vecop_list(vmxperm_list);
    TCGv_vec a = vi_dupd(vrb, hi), x = vi_new(), t = vi_new();
    TCGv_vec r = vi_new();

    /* high lane: word 0 of the doubleword, low lane: word 1, at the top */
    tcg_gen_and_vec(MO_64, x, a, vi_k(MO_64, 0xffffffff00000000ull));
    tcg_gen_shli_vec(MO_64, t, a, 32);
    x = vi_sel(vi_lanes(), x, t);
    /* each pixel to the top of a word */
    tcg_gen_shri_vec(MO_64, t, x, 16);
    tcg_gen_or_vec(MO_64, x, x, t);
    tcg_gen_sari_vec(MO_32, r, x, 8);
    tcg_gen_and_vec(MO_32, r, r, vi_k(MO_32, 0xff000000));
    tcg_gen_shri_vec(MO_32, t, x, 10);
    tcg_gen_and_vec(MO_32, t, t, vi_k(MO_32, 0x001f0000));
    tcg_gen_or_vec(MO_32, r, r, t);
    tcg_gen_shri_vec(MO_32, t, x, 13);
    tcg_gen_and_vec(MO_32, t, t, vi_k(MO_32, 0x00001f00));
    tcg_gen_or_vec(MO_32, r, r, t);
    tcg_gen_shri_vec(MO_32, t, x, 16);
    tcg_gen_and_vec(MO_32, t, t, vi_k(MO_32, 0x0000001f));
    tcg_gen_or_vec(MO_32, r, r, t);
    tcg_gen_st_vec(r, tcg_env, avr_full_offset(vrt));
    tcg_swap_vecop_list(hold);
}

/* low halves of the vece elements of x to the low 32 bits */
static void vi_pk_i64(TCGv_i64 x, unsigned vece)
{
    TCGv_i64 t = tcg_temp_new_i64();

    if (vece == MO_16) {
        tcg_gen_andi_i64(x, x, 0x00ff00ff00ff00ffull);
        tcg_gen_shri_i64(t, x, 8);
        tcg_gen_or_i64(x, x, t);
    }
    tcg_gen_shri_i64(t, x, 32);
    tcg_gen_deposit_i64(x, x, t, 16, 16);
}

/* vpkuhum / vpkuwum */
static void gen_vpkum_i64(int vrt, int vra, int vrb, unsigned vece)
{
    TCGv_i64 v[4];

    for (int i = 0; i < 4; i++) {
        v[i] = tcg_temp_new_i64();
        get_avr64(v[i], i < 2 ? vra : vrb, !(i & 1));
        vi_pk_i64(v[i], vece);
    }
    tcg_gen_deposit_i64(v[1], v[1], v[0], 32, 32);
    tcg_gen_deposit_i64(v[3], v[3], v[2], 32, 32);
    set_avr64(vrt, v[1], true);
    set_avr64(vrt, v[3], false);
}

/*
 * Narrow the vece elements of x, saturating, to the low 32 bits of each
 * lane; sat |= saturated.
 */
static void vi_pk(TCGv_vec x, unsigned vece, int kind, TCGv_vec sat)
{
    unsigned n = 4 << vece;
    uint64_t lo = (1ull << n) - 1;
    TCGv_vec t = vi_new(), u = vi_new();

    switch (kind) {
    case VPK_US:
        tcg_gen_umin_vec(vece, t, x, vi_k(vece, lo));
        break;
    case VPK_SU:
        tcg_gen_smax_vec(vece, t, x, vi_k(vece, 0));
        tcg_gen_smin_vec(vece, t, t, vi_k(vece, lo));
        break;
    case VPK_SS:
        tcg_gen_smin_vec(vece, t, x, vi_k(vece, lo >> 1));
        tcg_gen_smax_vec(vece, t, t, vi_k(vece, -(lo >> 1) - 1));
        break;
    case VPK_PX:
        tcg_gen_shri_vec(MO_32, t, x, 9);
        tcg_gen_and_vec(MO_32, t, t, vi_k(MO_32, 0xfc00));
        tcg_gen_shri_vec(MO_32, u, x, 6);
        tcg_gen_and_vec(MO_32, u, u, vi_k(MO_32, 0x3e0));
        tcg_gen_or_vec(MO_32, t, t, u);
        tcg_gen_shri_vec(MO_32, u, x, 3);
        tcg_gen_and_vec(MO_32, u, u, vi_k(MO_32, 0x1f));
        tcg_gen_or_vec(MO_32, x, t, u);
        break;
    }
    if (kind != VPK_PX) {
        tcg_gen_xor_vec(vece, x, x, t);
        tcg_gen_or_vec(vece, sat, sat, x);
        if (kind == VPK_SS) {
            tcg_gen_and_vec(vece, x, t, vi_k(vece, lo));
        } else {
            tcg_gen_mov_vec(x, t);
        }
    }
    /* elements now zero-extended */
    if (vece == MO_16) {
        tcg_gen_shri_vec(MO_64, t, x, 8);
        tcg_gen_or_vec(MO_64, x, x, t);
        tcg_gen_and_vec(MO_64, x, x, vi_k(MO_64, 0x0000ffff0000ffffull));
    }
    tcg_gen_shri_vec(MO_64, t, x, 16);
    tcg_gen_or_vec(MO_64, x, x, t);
}

/* saturating packs and vpkpx */
static void gen_vpk_vec(int vrt, int vra, int vrb, unsigned vece, int kind)
{
    const TCGOpcode *hold = tcg_swap_vecop_list(vmxperm_list);
    TCGv_vec m = vi_lanes();
    TCGv_vec p = vi_sel(m, native_avr(vra), vi_dupd(vrb, true));
    TCGv_vec q = vi_sel(m, vi_dupd(vra, false), native_avr(vrb));
    TCGv_vec sat = vi_new();

    /* p: a.hi, b.hi; q: a.lo, b.lo */
    tcg_gen_dupi_vec(MO_32, sat, 0);
    vi_pk(p, vece, kind, sat);
    vi_pk(q, vece, kind, sat);
    tcg_gen_shli_vec(MO_64, p, p, 32);
    tcg_gen_and_vec(MO_64, q, q, vi_k(MO_64, 0xffffffff));
    tcg_gen_or_vec(MO_64, p, p, q);
    tcg_gen_st_vec(p, tcg_env, avr_full_offset(vrt));
    if (kind != VPK_PX) {
        vi_or_sat(sat);
    }
    tcg_swap_vecop_list(hold);
}

/* r = b + x signed saturating, |x| <= 2^16; sat |= overflow */
static void vi_ssadd32x(TCGv_vec r, TCGv_vec b, TCGv_vec x, TCGv_vec sat)
{
    TCGv_vec o, t;

    if (tcg_can_emit_vec_op(INDEX_op_ssadd_vec, TCG_TYPE_V128, MO_32)) {
        vi_ssadd32(r, b, x, sat);
        return;
    }
    /* b clamped to [INT32_MIN - min(x, 0), INT32_MAX - max(x, 0)] */
    o = vi_new();
    t = vi_new();
    tcg_gen_smax_vec(MO_32, o, x, vi_k(MO_32, 0));
    tcg_gen_sub_vec(MO_32, o, vi_k(MO_32, INT32_MAX), o);
    tcg_gen_smin_vec(MO_32, t, x, vi_k(MO_32, 0));
    tcg_gen_sub_vec(MO_32, t, vi_k(MO_32, INT32_MIN), t);
    tcg_gen_smax_vec(MO_32, t, b, t);
    tcg_gen_smin_vec(MO_32, t, t, o);
    tcg_gen_xor_vec(MO_32, o, t, b);
    tcg_gen_or_vec(MO_32, sat, sat, o);
    tcg_gen_add_vec(MO_32, r, t, x);
}

static void gen_vsum4_vec(int vrt, int vra, int vrb, int kind)
{
    const TCGOpcode *hold = tcg_swap_vecop_list(vmxperm_list);
    TCGv_vec a = native_avr(vra), b = native_avr(vrb);
    TCGv_vec x = vi_new(), t = vi_new(), sat = vi_new();

    tcg_gen_dupi_vec(MO_32, sat, 0);
    switch (kind) {
    case VSUM_UB:
        tcg_gen_shri_vec(MO_16, x, a, 8);
        tcg_gen_and_vec(MO_16, t, a, vi_k(MO_16, 0xff));
        tcg_gen_add_vec(MO_16, x, x, t);
        tcg_gen_shri_vec(MO_32, t, x, 16);
        tcg_gen_and_vec(MO_32, x, x, vi_k(MO_32, 0xffff));
        tcg_gen_add_vec(MO_32, x, x, t);
        vi_usadd32(x, x, b, sat);
        break;
    case VSUM_SB:
        tcg_gen_sari_vec(MO_16, x, a, 8);
        tcg_gen_shli_vec(MO_16, t, a, 8);
        tcg_gen_sari_vec(MO_16, t, t, 8);
        tcg_gen_add_vec(MO_16, a, x, t);
        /* fall through */
    case VSUM_SH:
        tcg_gen_sari_vec(MO_32, x, a, 16);
        tcg_gen_shli_vec(MO_32, t, a, 16);
        tcg_gen_sari_vec(MO_32, t, t, 16);
        tcg_gen_add_vec(MO_32, x, x, t);
        vi_ssadd32x(x, b, x, sat);
        break;
    }
    tcg_gen_st_vec(x, tcg_env, avr_full_offset(vrt));
    vi_or_sat(sat);
    tcg_swap_vecop_list(hold);
}

/* t = t saturated to int32, zero-extended; sat |= overflow */
static void vi_sat32_i64(TCGv_i64 t, TCGv_i64 sat)
{
    TCGv_i64 r = tcg_temp_new_i64(), m = tcg_temp_new_i64();

    tcg_gen_ext32s_i64(r, t);
    tcg_gen_sari_i64(m, t, 63);
    tcg_gen_xori_i64(m, m, INT32_MAX);
    tcg_gen_movcond_i64(TCG_COND_NE, m, t, r, m, r);
    tcg_gen_setcond_i64(TCG_COND_NE, r, t, r);
    tcg_gen_or_i64(sat, sat, r);
    tcg_gen_ext32u_i64(t, m);
}

/* sum of both words of x, sign-extended */
static TCGv_i64 vi_addw_i64(TCGv_i64 x)
{
    TCGv_i64 r = tcg_temp_new_i64(), w = tcg_temp_new_i64();

    tcg_gen_sari_i64(r, x, 32);
    tcg_gen_ext32s_i64(w, x);
    tcg_gen_add_i64(r, r, w);
    return r;
}

static void gen_vsumsws_i64(int vrt, int vra, int vrb)
{
    TCGv_i64 hi = tcg_temp_new_i64(), lo = tcg_temp_new_i64();
    TCGv_i64 t = tcg_temp_new_i64(), sat = tcg_temp_new_i64();

    get_avr64(hi, vra, true);
    get_avr64(lo, vra, false);
    get_avr64(t, vrb, false);
    tcg_gen_ext32s_i64(t, t);
    hi = vi_addw_i64(hi);
    tcg_gen_add_i64(t, t, vi_addw_i64(lo));
    tcg_gen_add_i64(t, t, hi);
    tcg_gen_movi_i64(sat, 0);
    vi_sat32_i64(t, sat);
    set_avr64(vrt, t, false);
    set_avr64(vrt, tcg_constant_i64(0), true);
    vi_or_sat_i64(sat);
}

static void gen_vsum2sws_i64(int vrt, int vra, int vrb)
{
    TCGv_i64 a[2], t[2], sat = tcg_temp_new_i64();

    for (int i = 0; i < 2; i++) {
        a[i] = tcg_temp_new_i64();
        t[i] = tcg_temp_new_i64();
        get_avr64(a[i], vra, i);
        get_avr64(t[i], vrb, i);
    }
    tcg_gen_movi_i64(sat, 0);
    for (int i = 0; i < 2; i++) {
        tcg_gen_ext32s_i64(t[i], t[i]);
        tcg_gen_add_i64(t[i], t[i], vi_addw_i64(a[i]));
        vi_sat32_i64(t[i], sat);
        set_avr64(vrt, t[i], i);
    }
    vi_or_sat_i64(sat);
}

/* vslo (left) / vsro: shift by octets, count from vB bits 121:124 */
static void gen_vso_i64(int vrt, int vra, int vrb, bool left)
{
    TCGv_i64 hi = tcg_temp_new_i64(), lo = tcg_temp_new_i64();
    TCGv_i64 m = tcg_temp_new_i64(), b = tcg_temp_new_i64();
    TCGv_i64 x = tcg_temp_new_i64(), y = tcg_temp_new_i64();
    TCGv_i64 z = tcg_constant_i64(0), k = tcg_constant_i64(0x40);
    TCGv_i64 in = left ? lo : hi, out = left ? hi : lo;

    get_avr64(hi, vra, true);
    get_avr64(lo, vra, false);
    get_avr64(b, vrb, false);
    tcg_gen_andi_i64(m, b, 0x38);
    /* x: the half shifted into, y: the half shifted out of */
    if (left) {
        tcg_gen_shl_i64(x, out, m);
        tcg_gen_shl_i64(y, in, m);
        tcg_gen_shri_i64(in, in, 1);
        tcg_gen_xori_i64(m, m, 63);
        tcg_gen_shr_i64(in, in, m);
    } else {
        tcg_gen_shr_i64(x, out, m);
        tcg_gen_shr_i64(y, in, m);
        tcg_gen_shli_i64(in, in, 1);
        tcg_gen_xori_i64(m, m, 63);
        tcg_gen_shl_i64(in, in, m);
    }
    tcg_gen_or_i64(x, x, in);
    tcg_gen_movcond_i64(TCG_COND_TSTNE, x, b, k, y, x);
    tcg_gen_movcond_i64(TCG_COND_TSTNE, y, b, k, z, y);
    set_avr64(vrt, x, left);
    set_avr64(vrt, y, !left);
}

#define GEN_VXFORM_VFP(name, opc2, opc3, desc)                          \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (native_vmx_on) {                                                \
        native_vmx_op(desc, rD(ctx->opcode), rA(ctx->opcode),           \
                      rB(ctx->opcode), -1);                             \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(tcg_env, gen_avr_ptr(rD(ctx->opcode)),            \
                      gen_avr_ptr(rA(ctx->opcode)),                     \
                      gen_avr_ptr(rB(ctx->opcode)));                    \
}

#define GEN_VXFORM_NOA_VFP(name, opc2, opc3, desc)                      \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (native_vmx_on) {                                                \
        native_vmx_op(desc, rD(ctx->opcode), -1, rB(ctx->opcode), -1);  \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(tcg_env, gen_avr_ptr(rD(ctx->opcode)),            \
                      gen_avr_ptr(rB(ctx->opcode)));                    \
}

GEN_VXFORM_VFP(vaddfp, 5, 0, VFPARITH_ADD);
GEN_VXFORM_VFP(vsubfp, 5, 1, VFPARITH_SUB);
GEN_VXFORM_VFP(vmaxfp, 5, 16, VFPARITH_MAX);
GEN_VXFORM_VFP(vminfp, 5, 17, VFPARITH_MIN);
GEN_VXFORM_HETRO(vextublx, 6, 24)
GEN_VXFORM_HETRO(vextuhlx, 6, 25)
GEN_VXFORM_HETRO(vextuwlx, 6, 26)
GEN_VXFORM_TRANS_DUAL(vmrgow, PPC_NONE, PPC2_ISA207,
                vextuwlx, PPC_NONE, PPC2_ISA300)
GEN_VXFORM_HETRO(vextubrx, 6, 28)
GEN_VXFORM_HETRO(vextuhrx, 6, 29)
GEN_VXFORM_HETRO(vextuwrx, 6, 30)
GEN_VXFORM_TRANS_DUAL(vmrgew, PPC_NONE, PPC2_ISA207,
                vextuwrx, PPC_NONE, PPC2_ISA300)

#define GEN_VXRFORM1(opname, name, str, opc2, opc3)                     \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr ra, rb, rd;                                            \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        ra = gen_avr_ptr(rA(ctx->opcode));                              \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##opname(tcg_env, rd, ra, rb);                       \
    }

#define GEN_VXRFORM(name, opc2, opc3)                                \
    GEN_VXRFORM1(name, name, #name, opc2, opc3)                      \
    GEN_VXRFORM1(name##_dot, name##_, #name ".", opc2, (opc3 | (0x1 << 4)))

/*
 * Support for Altivec instructions that use bit 31 (Rc) as an opcode
 * bit but also use bit 21 as an actual Rc bit.  In general, these pairs
 * come from different versions of the ISA, so we must also support a
 * pair of flags for each instruction.
 */
#define GEN_VXRFORM_DUAL(name0, flg0, flg2_0, name1, flg1, flg2_1)     \
static void glue(gen_, name0##_##name1)(DisasContext *ctx)             \
{                                                                      \
    if ((Rc(ctx->opcode) == 0) &&                                      \
        ((ctx->insns_flags & flg0) || (ctx->insns_flags2 & flg2_0))) { \
        if (Rc21(ctx->opcode) == 0) {                                  \
            gen_##name0(ctx);                                          \
        } else {                                                       \
            gen_##name0##_(ctx);                                       \
        }                                                              \
    } else if ((Rc(ctx->opcode) == 1) &&                               \
        ((ctx->insns_flags & flg1) || (ctx->insns_flags2 & flg2_1))) { \
        if (Rc21(ctx->opcode) == 0) {                                  \
            gen_##name1(ctx);                                          \
        } else {                                                       \
            gen_##name1##_(ctx);                                       \
        }                                                              \
    } else {                                                           \
        gen_inval_exception(ctx, POWERPC_EXCP_INVAL_INVAL);            \
    }                                                                  \
}

static void do_vcmp_rc(int vrt)
{
    TCGv_i64 tmp, set, clr;

    tmp = tcg_temp_new_i64();
    set = tcg_temp_new_i64();
    clr = tcg_temp_new_i64();

    get_avr64(tmp, vrt, true);
    tcg_gen_mov_i64(set, tmp);
    get_avr64(tmp, vrt, false);
    tcg_gen_or_i64(clr, set, tmp);
    tcg_gen_and_i64(set, set, tmp);

    tcg_gen_setcondi_i64(TCG_COND_EQ, clr, clr, 0);
    tcg_gen_shli_i64(clr, clr, 1);

    tcg_gen_setcondi_i64(TCG_COND_EQ, set, set, -1);
    tcg_gen_shli_i64(set, set, 3);

    tcg_gen_or_i64(tmp, set, clr);
    tcg_gen_extrl_i64_i32(cpu_crf[6], tmp);
}

static bool do_vcmp(DisasContext *ctx, arg_VC *a, TCGCond cond, int vece)
{
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_cmp(cond, vece, avr_full_offset(a->vrt),
                     avr_full_offset(a->vra), avr_full_offset(a->vrb), 16, 16);

    if (a->rc) {
        do_vcmp_rc(a->vrt);
    }

    return true;
}

TRANS_FLAGS(ALTIVEC, VCMPEQUB, do_vcmp, TCG_COND_EQ, MO_8)
TRANS_FLAGS(ALTIVEC, VCMPEQUH, do_vcmp, TCG_COND_EQ, MO_16)
TRANS_FLAGS(ALTIVEC, VCMPEQUW, do_vcmp, TCG_COND_EQ, MO_32)
TRANS_FLAGS2(ISA207, VCMPEQUD, do_vcmp, TCG_COND_EQ, MO_64)

TRANS_FLAGS(ALTIVEC, VCMPGTSB, do_vcmp, TCG_COND_GT, MO_8)
TRANS_FLAGS(ALTIVEC, VCMPGTSH, do_vcmp, TCG_COND_GT, MO_16)
TRANS_FLAGS(ALTIVEC, VCMPGTSW, do_vcmp, TCG_COND_GT, MO_32)
TRANS_FLAGS2(ISA207, VCMPGTSD, do_vcmp, TCG_COND_GT, MO_64)
TRANS_FLAGS(ALTIVEC, VCMPGTUB, do_vcmp, TCG_COND_GTU, MO_8)
TRANS_FLAGS(ALTIVEC, VCMPGTUH, do_vcmp, TCG_COND_GTU, MO_16)
TRANS_FLAGS(ALTIVEC, VCMPGTUW, do_vcmp, TCG_COND_GTU, MO_32)
TRANS_FLAGS2(ISA207, VCMPGTUD, do_vcmp, TCG_COND_GTU, MO_64)

TRANS_FLAGS2(ISA300, VCMPNEB, do_vcmp, TCG_COND_NE, MO_8)
TRANS_FLAGS2(ISA300, VCMPNEH, do_vcmp, TCG_COND_NE, MO_16)
TRANS_FLAGS2(ISA300, VCMPNEW, do_vcmp, TCG_COND_NE, MO_32)

static void gen_vcmpnez_vec(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    TCGv_vec t0, t1, zero;

    t0 = tcg_temp_new_vec_matching(t);
    t1 = tcg_temp_new_vec_matching(t);
    zero = tcg_constant_vec_matching(t, vece, 0);

    tcg_gen_cmp_vec(TCG_COND_EQ, vece, t0, a, zero);
    tcg_gen_cmp_vec(TCG_COND_EQ, vece, t1, b, zero);
    tcg_gen_cmp_vec(TCG_COND_NE, vece, t, a, b);

    tcg_gen_or_vec(vece, t, t, t0);
    tcg_gen_or_vec(vece, t, t, t1);
}

static bool do_vcmpnez(DisasContext *ctx, arg_VC *a, int vece)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_cmp_vec, 0
    };
    static const GVecGen3 ops[3] = {
        {
            .fniv = gen_vcmpnez_vec,
            .fno = gen_helper_VCMPNEZB,
            .opt_opc = vecop_list,
            .vece = MO_8
        },
        {
            .fniv = gen_vcmpnez_vec,
            .fno = gen_helper_VCMPNEZH,
            .opt_opc = vecop_list,
            .vece = MO_16
        },
        {
            .fniv = gen_vcmpnez_vec,
            .fno = gen_helper_VCMPNEZW,
            .opt_opc = vecop_list,
            .vece = MO_32
        }
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &ops[vece]);

    if (a->rc) {
        do_vcmp_rc(a->vrt);
    }

    return true;
}

TRANS(VCMPNEZB, do_vcmpnez, MO_8)
TRANS(VCMPNEZH, do_vcmpnez, MO_16)
TRANS(VCMPNEZW, do_vcmpnez, MO_32)

static bool trans_VCMPEQUQ(DisasContext *ctx, arg_VC *a)
{
    TCGv_i64 t0, t1, t2;

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();

    get_avr64(t0, a->vra, true);
    get_avr64(t1, a->vrb, true);
    tcg_gen_xor_i64(t2, t0, t1);

    get_avr64(t0, a->vra, false);
    get_avr64(t1, a->vrb, false);
    tcg_gen_xor_i64(t1, t0, t1);

    tcg_gen_or_i64(t1, t1, t2);
    tcg_gen_negsetcond_i64(TCG_COND_EQ, t1, t1, tcg_constant_i64(0));

    set_avr64(a->vrt, t1, true);
    set_avr64(a->vrt, t1, false);

    if (a->rc) {
        tcg_gen_extrl_i64_i32(cpu_crf[6], t1);
        tcg_gen_andi_i32(cpu_crf[6], cpu_crf[6], 0xa);
        tcg_gen_xori_i32(cpu_crf[6], cpu_crf[6], 0x2);
    }
    return true;
}

static bool do_vcmpgtq(DisasContext *ctx, arg_VC *a, bool sign)
{
    TCGv_i64 t0, t1, t2;

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();
    t2 = tcg_temp_new_i64();

    get_avr64(t0, a->vra, false);
    get_avr64(t1, a->vrb, false);
    tcg_gen_negsetcond_i64(TCG_COND_GTU, t2, t0, t1);

    get_avr64(t0, a->vra, true);
    get_avr64(t1, a->vrb, true);
    tcg_gen_movcond_i64(TCG_COND_EQ, t2, t0, t1, t2, tcg_constant_i64(0));
    tcg_gen_negsetcond_i64(sign ? TCG_COND_GT : TCG_COND_GTU, t1, t0, t1);

    tcg_gen_or_i64(t1, t1, t2);

    set_avr64(a->vrt, t1, true);
    set_avr64(a->vrt, t1, false);

    if (a->rc) {
        tcg_gen_extrl_i64_i32(cpu_crf[6], t1);
        tcg_gen_andi_i32(cpu_crf[6], cpu_crf[6], 0xa);
        tcg_gen_xori_i32(cpu_crf[6], cpu_crf[6], 0x2);
    }
    return true;
}

TRANS(VCMPGTSQ, do_vcmpgtq, true)
TRANS(VCMPGTUQ, do_vcmpgtq, false)

static bool do_vcmpq(DisasContext *ctx, arg_VX_bf *a, bool sign)
{
    TCGv_i64 vra, vrb;
    TCGLabel *gt, *lt, *done;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    vra = tcg_temp_new_i64();
    vrb = tcg_temp_new_i64();
    gt = gen_new_label();
    lt = gen_new_label();
    done = gen_new_label();

    get_avr64(vra, a->vra, true);
    get_avr64(vrb, a->vrb, true);
    tcg_gen_brcond_i64((sign ? TCG_COND_GT : TCG_COND_GTU), vra, vrb, gt);
    tcg_gen_brcond_i64((sign ? TCG_COND_LT : TCG_COND_LTU), vra, vrb, lt);

    get_avr64(vra, a->vra, false);
    get_avr64(vrb, a->vrb, false);
    tcg_gen_brcond_i64(TCG_COND_GTU, vra, vrb, gt);
    tcg_gen_brcond_i64(TCG_COND_LTU, vra, vrb, lt);

    tcg_gen_movi_i32(cpu_crf[a->bf], CRF_EQ);
    tcg_gen_br(done);

    gen_set_label(gt);
    tcg_gen_movi_i32(cpu_crf[a->bf], CRF_GT);
    tcg_gen_br(done);

    gen_set_label(lt);
    tcg_gen_movi_i32(cpu_crf[a->bf], CRF_LT);
    tcg_gen_br(done);

    gen_set_label(done);
    return true;
}

TRANS(VCMPSQ, do_vcmpq, true)
TRANS(VCMPUQ, do_vcmpq, false)

static void gen_vcmpfp(DisasContext *ctx, unsigned desc, bool rc,
                       void (*helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    int vrt = rD(ctx->opcode), vra = rA(ctx->opcode), vrb = rB(ctx->opcode);

    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }
    if (native_vmx_on) {
        native_vmx_op(desc, vrt, vra, vrb, -1);
        if (rc) {
            do_vcmp_rc(vrt);
        }
        return;
    }
    helper(tcg_env, gen_avr_ptr(vrt), gen_avr_ptr(vra), gen_avr_ptr(vrb));
}

#define GEN_VXRFORM_VFP(name, opc2, opc3, desc)                         \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    gen_vcmpfp(ctx, desc, false, gen_helper_##name);                    \
}                                                                       \
static void glue(gen_, name##_)(DisasContext *ctx)                      \
{                                                                       \
    gen_vcmpfp(ctx, desc, true, gen_helper_##name##_dot);               \
}

GEN_VXRFORM_VFP(vcmpeqfp, 3, 3, VFPARITH_CMPEQ)
GEN_VXRFORM_VFP(vcmpgefp, 3, 7, VFPARITH_CMPGE)
GEN_VXRFORM_VFP(vcmpgtfp, 3, 11, VFPARITH_CMPGT)
GEN_VXRFORM_VFP(vcmpbfp, 3, 15, VFPARITH_CMPB)

static void gen_vsplti(DisasContext *ctx, int vece)
{
    int simm;

    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }

    simm = SIMM5(ctx->opcode);
    tcg_gen_gvec_dup_imm(vece, avr_full_offset(rD(ctx->opcode)), 16, 16, simm);
}

#define GEN_VXFORM_VSPLTI(name, vece, opc2, opc3) \
static void glue(gen_, name)(DisasContext *ctx) { gen_vsplti(ctx, vece); }

GEN_VXFORM_VSPLTI(vspltisb, MO_8, 6, 12);
GEN_VXFORM_VSPLTI(vspltish, MO_16, 6, 13);
GEN_VXFORM_VSPLTI(vspltisw, MO_32, 6, 14);

#define GEN_VXFORM_NOA(name, opc2, opc3)                                \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb, rd;                                                \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##name(rd, rb);                                      \
    }

#define GEN_VXFORM_NOA_ENV(name, opc2, opc3)                            \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb, rd;                                                \
                                                                        \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##name(tcg_env, rd, rb);                             \
    }

#define GEN_VXFORM_NOA_2(name, opc2, opc3, opc4)                        \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb, rd;                                                \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##name(rd, rb);                                      \
    }

#define GEN_VXFORM_NOA_3(name, opc2, opc3, opc4)                        \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb;                                                    \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        gen_helper_##name(cpu_gpr[rD(ctx->opcode)], rb);                \
    }

#define GEN_VXFORM_VUPK(name, op, native)                               \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    int vrt = rD(ctx->opcode), vrb = rB(ctx->opcode);                   \
                                                                        \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (vmxperm(op)) {                                                  \
        native;                                                         \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(gen_avr_ptr(vrt), gen_avr_ptr(vrb));              \
}
GEN_VXFORM_NOA(vupkhsb, 7, 8);
GEN_VXFORM_VUPK(vupkhsh, VP_UPKH, gen_vupkh_i64(vrt, vrb, true));
GEN_VXFORM_NOA(vupkhsw, 7, 25);
GEN_VXFORM_NOA(vupklsb, 7, 10);
GEN_VXFORM_VUPK(vupklsh, VP_UPKH, gen_vupkh_i64(vrt, vrb, false));
GEN_VXFORM_NOA(vupklsw, 7, 27);
GEN_VXFORM_VUPK(vupkhpx, VP_UPKPX, gen_vupkpx_vec(vrt, vrb, true));
GEN_VXFORM_VUPK(vupklpx, VP_UPKPX, gen_vupkpx_vec(vrt, vrb, false));
GEN_VXFORM_NOA_VFP(vrefp, 5, 4, VFPARITH_RE);
GEN_VXFORM_NOA_VFP(vrsqrtefp, 5, 5, VFPARITH_RSQRT);
GEN_VXFORM_NOA_ENV(vexptefp, 5, 6);
GEN_VXFORM_NOA_ENV(vlogefp, 5, 7);
GEN_VXFORM_NOA_VFP(vrfim, 5, 11, VFPARITH_RIM);
GEN_VXFORM_NOA_VFP(vrfin, 5, 8, VFPARITH_RIN);
GEN_VXFORM_NOA_VFP(vrfip, 5, 10, VFPARITH_RIP);
GEN_VXFORM_NOA_VFP(vrfiz, 5, 9, VFPARITH_RIZ);

static void gen_vprtyb_vec(unsigned vece, TCGv_vec t, TCGv_vec b)
{
    int i;
    TCGv_vec tmp = tcg_temp_new_vec_matching(b);
    /* MO_32 is 2, so 2 iterations for MO_32 and 3 for MO_64 */
    for (i = 0; i < vece; i++) {
        tcg_gen_shri_vec(vece, tmp, b, (4 << (vece - i)));
        tcg_gen_xor_vec(vece, b, tmp, b);
    }
    tcg_gen_and_vec(vece, t, b, tcg_constant_vec_matching(t, vece, 1));
}

/* vprtybw */
static void gen_vprtyb_i32(TCGv_i32 t, TCGv_i32 b)
{
    tcg_gen_ctpop_i32(t, b);
    tcg_gen_and_i32(t, t, tcg_constant_i32(1));
}

/* vprtybd */
static void gen_vprtyb_i64(TCGv_i64 t, TCGv_i64 b)
{
    tcg_gen_ctpop_i64(t, b);
    tcg_gen_and_i64(t, t, tcg_constant_i64(1));
}

static bool do_vx_vprtyb(DisasContext *ctx, arg_VX_tb *a, unsigned vece)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shri_vec, 0
    };

    static const GVecGen2 op[] = {
        {
            .fniv = gen_vprtyb_vec,
            .fni4 = gen_vprtyb_i32,
            .opt_opc = vecop_list,
            .vece = MO_32
        },
        {
            .fniv = gen_vprtyb_vec,
            .fni8 = gen_vprtyb_i64,
            .opt_opc = vecop_list,
            .vece = MO_64
        },
        {
            .fno = gen_helper_VPRTYBQ,
            .vece = MO_128
        },
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_2(avr_full_offset(a->vrt), avr_full_offset(a->vrb),
                   16, 16, &op[vece - MO_32]);

    return true;
}

TRANS(VPRTYBW, do_vx_vprtyb, MO_32)
TRANS(VPRTYBD, do_vx_vprtyb, MO_64)
TRANS(VPRTYBQ, do_vx_vprtyb, MO_128)

static void gen_vsplt(DisasContext *ctx, int vece)
{
    int uimm, dofs, bofs;

    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }

    uimm = UIMM5(ctx->opcode);
    bofs = avr_full_offset(rB(ctx->opcode));
    dofs = avr_full_offset(rD(ctx->opcode));

    /* Experimental testing shows that hardware masks the immediate.  */
    bofs += (uimm << vece) & 15;
#if !HOST_BIG_ENDIAN
    bofs ^= 15;
    bofs &= ~((1 << vece) - 1);
#endif

    tcg_gen_gvec_dup_mem(vece, dofs, bofs, 16, 16);
}

#define GEN_VXFORM_VSPLT(name, vece, opc2, opc3) \
static void glue(gen_, name)(DisasContext *ctx) { gen_vsplt(ctx, vece); }

#define GEN_VXFORM_UIMM_ENV(name, opc2, opc3)                           \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb, rd;                                                \
        TCGv_i32 uimm;                                                  \
                                                                        \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        uimm = tcg_constant_i32(UIMM5(ctx->opcode));                    \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##name(tcg_env, rd, rb, uimm);                       \
    }

#define GEN_VXFORM_UIMM_SPLAT(name, opc2, opc3, splat_max)              \
static void glue(gen_, name)(DisasContext *ctx)                         \
    {                                                                   \
        TCGv_ptr rb, rd;                                                \
        uint8_t uimm = UIMM4(ctx->opcode);                              \
        TCGv_i32 t0;                                                    \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        if (uimm > splat_max) {                                         \
            uimm = 0;                                                   \
        }                                                               \
        t0 = tcg_temp_new_i32();                                        \
        tcg_gen_movi_i32(t0, uimm);                                     \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        gen_helper_##name(rd, rb, t0);                                  \
    }

GEN_VXFORM_VSPLT(vspltb, MO_8, 6, 8);
GEN_VXFORM_VSPLT(vsplth, MO_16, 6, 9);
GEN_VXFORM_VSPLT(vspltw, MO_32, 6, 10);
GEN_VXFORM_UIMM_SPLAT(vextractub, 6, 8, 15);
GEN_VXFORM_UIMM_SPLAT(vextractuh, 6, 9, 14);
GEN_VXFORM_UIMM_SPLAT(vextractuw, 6, 10, 12);
GEN_VXFORM_UIMM_SPLAT(vextractd, 6, 11, 8);
/* vcf*, vct*: exact on the host; vct* OR the saturated lanes into SAT */
#define GEN_VXFORM_UIMM_VFP(name, opc2, opc3, desc, sat)                \
static void glue(gen_, name)(DisasContext *ctx)                         \
{                                                                       \
    unsigned uim = UIMM5(ctx->opcode);                                  \
    int vrt = rD(ctx->opcode), vrb = rB(ctx->opcode);                   \
                                                                        \
    if (unlikely(!ctx->altivec_enabled)) {                              \
        gen_exception(ctx, POWERPC_EXCP_VPU);                           \
        return;                                                         \
    }                                                                   \
    if (native_vmx_on) {                                                \
        if (sat >= 0) {                                                 \
            TCGv_vec b = native_avr(vrb);                               \
            TCGv_vec m = tcg_temp_new_vec(TCG_TYPE_V128);               \
            TCGv_vec v = tcg_temp_new_vec(TCG_TYPE_V128);               \
                                                                        \
            tcg_gen_vfparith_vec(sat | uim << VFPARITH_SCALE, m, b, b, b); \
            tcg_gen_ld_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat)); \
            tcg_gen_or_vec(MO_32, v, v, m);                             \
            tcg_gen_st_vec(v, tcg_env, offsetof(CPUPPCState, vscr_sat)); \
        }                                                               \
        native_vmx_op(desc | uim << VFPARITH_SCALE, vrt, -1, vrb, -1);  \
        return;                                                         \
    }                                                                   \
    gen_helper_##name(tcg_env, gen_avr_ptr(vrt), gen_avr_ptr(vrb),      \
                      tcg_constant_i32(uim));                           \
}

GEN_VXFORM_UIMM_VFP(vcfux, 5, 12, VFPARITH_CFU, -1);
GEN_VXFORM_UIMM_VFP(vcfsx, 5, 13, VFPARITH_CFS, -1);
GEN_VXFORM_UIMM_VFP(vctuxs, 5, 14, VFPARITH_CTU, VFPARITH_CTU_SAT);
GEN_VXFORM_UIMM_VFP(vctsxs, 5, 15, VFPARITH_CTS, VFPARITH_CTS_SAT);
GEN_VXFORM_DUAL(vspltb, PPC_ALTIVEC, PPC_NONE,
                vextractub, PPC_NONE, PPC2_ISA300);
GEN_VXFORM_DUAL(vsplth, PPC_ALTIVEC, PPC_NONE,
                vextractuh, PPC_NONE, PPC2_ISA300);
GEN_VXFORM_DUAL(vspltw, PPC_ALTIVEC, PPC_NONE,
                vextractuw, PPC_NONE, PPC2_ISA300);

static bool trans_VGNB(DisasContext *ctx, arg_VX_n *a)
{
    /*
     * Similar to do_vextractm, we'll use a sequence of mask-shift-or operations
     * to gather the bits. The masks can be created with
     *
     * uint64_t mask(uint64_t n, uint64_t step)
     * {
     *     uint64_t p = ((1UL << (1UL << step)) - 1UL) << ((n - 1UL) << step),
     *                  plen = n << step, m = 0;
     *     for(int i = 0; i < 64/plen; i++) {
     *         m |= p;
     *         m = ror64(m, plen);
     *     }
     *     p >>= plen * DIV_ROUND_UP(64, plen) - 64;
     *     return m | p;
     * }
     *
     * But since there are few values of N, we'll use a lookup table to avoid
     * these calculations at runtime.
     */
    static const uint64_t mask[6][5] = {
        {
            0xAAAAAAAAAAAAAAAAULL, 0xccccccccccccccccULL, 0xf0f0f0f0f0f0f0f0ULL,
            0xff00ff00ff00ff00ULL, 0xffff0000ffff0000ULL
        },
        {
            0x9249249249249249ULL, 0xC30C30C30C30C30CULL, 0xF00F00F00F00F00FULL,
            0xFF0000FF0000FF00ULL, 0xFFFF00000000FFFFULL
        },
        {
            /* For N >= 4, some mask operations can be elided */
            0x8888888888888888ULL, 0, 0xf000f000f000f000ULL, 0,
            0xFFFF000000000000ULL
        },
        {
            0x8421084210842108ULL, 0, 0xF0000F0000F0000FULL, 0, 0
        },
        {
            0x8208208208208208ULL, 0, 0xF00000F00000F000ULL, 0, 0
        },
        {
            0x8102040810204081ULL, 0, 0xF000000F000000F0ULL, 0, 0
        }
    };
    uint64_t m;
    int i, sh, nbits = DIV_ROUND_UP(64, a->n);
    TCGv_i64 hi, lo, t0, t1;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    if (a->n < 2) {
        /*
         * "N can be any value between 2 and 7, inclusive." Otherwise, the
         * result is undefined, so we don't need to change RT. Also, N > 7 is
         * impossible since the immediate field is 3 bits only.
         */
        return true;
    }

    hi = tcg_temp_new_i64();
    lo = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    get_avr64(hi, a->vrb, true);
    get_avr64(lo, a->vrb, false);

    /* Align the lower doubleword so we can use the same mask */
    tcg_gen_shli_i64(lo, lo, a->n * nbits - 64);

    /*
     * Starting from the most significant bit, gather every Nth bit with a
     * sequence of mask-shift-or operation. E.g.: for N=3
     * AxxBxxCxxDxxExxFxxGxxHxxIxxJxxKxxLxxMxxNxxOxxPxxQxxRxxSxxTxxUxxV
     *     & rep(0b100)
     * A..B..C..D..E..F..G..H..I..J..K..L..M..N..O..P..Q..R..S..T..U..V
     *     << 2
     * .B..C..D..E..F..G..H..I..J..K..L..M..N..O..P..Q..R..S..T..U..V..
     *     |
     * AB.BC.CD.DE.EF.FG.GH.HI.IJ.JK.KL.LM.MN.NO.OP.PQ.QR.RS.ST.TU.UV.V
     *  & rep(0b110000)
     * AB....CD....EF....GH....IJ....KL....MN....OP....QR....ST....UV..
     *     << 4
     * ..CD....EF....GH....IJ....KL....MN....OP....QR....ST....UV......
     *     |
     * ABCD..CDEF..EFGH..GHIJ..IJKL..KLMN..MNOP..OPQR..QRST..STUV..UV..
     *     & rep(0b111100000000)
     * ABCD........EFGH........IJKL........MNOP........QRST........UV..
     *     << 8
     * ....EFGH........IJKL........MNOP........QRST........UV..........
     *     |
     * ABCDEFGH....EFGHIJKL....IJKLMNOP....MNOPQRST....QRSTUV......UV..
     *  & rep(0b111111110000000000000000)
     * ABCDEFGH................IJKLMNOP................QRSTUV..........
     *     << 16
     * ........IJKLMNOP................QRSTUV..........................
     *     |
     * ABCDEFGHIJKLMNOP........IJKLMNOPQRSTUV..........QRSTUV..........
     *     & rep(0b111111111111111100000000000000000000000000000000)
     * ABCDEFGHIJKLMNOP................................QRSTUV..........
     *     << 32
     * ................QRSTUV..........................................
     *     |
     * ABCDEFGHIJKLMNOPQRSTUV..........................QRSTUV..........
     */
    for (i = 0, sh = a->n - 1; i < 5; i++, sh <<= 1) {
        m = mask[a->n - 2][i];
        if (m) {
            tcg_gen_andi_i64(hi, hi, m);
            tcg_gen_andi_i64(lo, lo, m);
        }
        if (sh < 64) {
            tcg_gen_shli_i64(t0, hi, sh);
            tcg_gen_shli_i64(t1, lo, sh);
            tcg_gen_or_i64(hi, t0, hi);
            tcg_gen_or_i64(lo, t1, lo);
        }
    }

    tcg_gen_andi_i64(hi, hi, ~(~0ULL >> nbits));
    tcg_gen_andi_i64(lo, lo, ~(~0ULL >> nbits));
    tcg_gen_shri_i64(lo, lo, nbits);
    tcg_gen_or_i64(hi, hi, lo);
    tcg_gen_trunc_i64_tl(cpu_gpr[a->rt], hi);
    return true;
}

static bool do_vextdx(DisasContext *ctx, arg_VA *a, int size, bool right,
               void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv))
{
    TCGv_ptr vrt, vra, vrb;
    TCGv rc;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    vrt = gen_avr_ptr(a->vrt);
    vra = gen_avr_ptr(a->vra);
    vrb = gen_avr_ptr(a->vrb);
    rc = tcg_temp_new();

    tcg_gen_andi_tl(rc, cpu_gpr[a->rc], 0x1F);
    if (right) {
        tcg_gen_subfi_tl(rc, 32 - size, rc);
    }
    gen_helper(tcg_env, vrt, vra, vrb, rc);
    return true;
}

TRANS(VEXTDUBVLX, do_vextdx, 1, false, gen_helper_VEXTDUBVLX)
TRANS(VEXTDUHVLX, do_vextdx, 2, false, gen_helper_VEXTDUHVLX)
TRANS(VEXTDUWVLX, do_vextdx, 4, false, gen_helper_VEXTDUWVLX)
TRANS(VEXTDDVLX, do_vextdx, 8, false, gen_helper_VEXTDDVLX)

TRANS(VEXTDUBVRX, do_vextdx, 1, true, gen_helper_VEXTDUBVLX)
TRANS(VEXTDUHVRX, do_vextdx, 2, true, gen_helper_VEXTDUHVLX)
TRANS(VEXTDUWVRX, do_vextdx, 4, true, gen_helper_VEXTDUWVLX)
TRANS(VEXTDDVRX, do_vextdx, 8, true, gen_helper_VEXTDDVLX)

static bool do_vinsx(DisasContext *ctx, int vrt, int size, bool right, TCGv ra,
            TCGv_i64 rb, void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    TCGv_ptr t;
    TCGv idx;

    t = gen_avr_ptr(vrt);
    idx = tcg_temp_new();

    tcg_gen_andi_tl(idx, ra, 0xF);
    if (right) {
        tcg_gen_subfi_tl(idx, 16 - size, idx);
    }

    gen_helper(tcg_env, t, rb, idx);
    return true;
}

static bool do_vinsvx(DisasContext *ctx, int vrt, int size, bool right, TCGv ra,
                int vrb, void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    TCGv_i64 val;

    val = tcg_temp_new_i64();
    get_avr64(val, vrb, true);
    return do_vinsx(ctx, vrt, size, right, ra, val, gen_helper);
}

static bool do_vinsx_VX(DisasContext *ctx, arg_VX *a, int size, bool right,
                        void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    TCGv_i64 val;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    val = tcg_temp_new_i64();
    tcg_gen_extu_tl_i64(val, cpu_gpr[a->vrb]);

    return do_vinsx(ctx, a->vrt, size, right, cpu_gpr[a->vra], val, gen_helper);
}

static bool do_vinsvx_VX(DisasContext *ctx, arg_VX *a, int size, bool right,
                        void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    return do_vinsvx(ctx, a->vrt, size, right, cpu_gpr[a->vra], a->vrb,
                     gen_helper);
}

static bool do_vins_VX_uim4(DisasContext *ctx, arg_VX_uim4 *a, int size,
                        void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    TCGv_i64 val;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    if (a->uim > (16 - size)) {
        /*
         * PowerISA v3.1 says that the resulting value is undefined in this
         * case, so just log a guest error and leave VRT unchanged. The
         * real hardware would do a partial insert, e.g. if VRT is zeroed and
         * RB is 0x12345678, executing "vinsw VRT,RB,14" results in
         * VRT = 0x0000...00001234, but we don't bother to reproduce this
         * behavior as software shouldn't rely on it.
         */
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid index for VINS* at"
            " 0x" TARGET_FMT_lx ", UIM = %d > %d\n", ctx->cia, a->uim,
            16 - size);
        return true;
    }

    val = tcg_temp_new_i64();
    tcg_gen_extu_tl_i64(val, cpu_gpr[a->vrb]);

    return do_vinsx(ctx, a->vrt, size, false, tcg_constant_tl(a->uim), val,
                    gen_helper);
}

static bool do_vinsert_VX_uim4(DisasContext *ctx, arg_VX_uim4 *a, int size,
                        void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv))
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    if (a->uim > (16 - size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Invalid index for VINSERT* at"
            " 0x" TARGET_FMT_lx ", UIM = %d > %d\n", ctx->cia, a->uim,
            16 - size);
        return true;
    }

    return do_vinsvx(ctx, a->vrt, size, false, tcg_constant_tl(a->uim), a->vrb,
                     gen_helper);
}

TRANS(VINSBLX, do_vinsx_VX, 1, false, gen_helper_VINSBLX)
TRANS(VINSHLX, do_vinsx_VX, 2, false, gen_helper_VINSHLX)
TRANS(VINSWLX, do_vinsx_VX, 4, false, gen_helper_VINSWLX)
TRANS(VINSDLX, do_vinsx_VX, 8, false, gen_helper_VINSDLX)

TRANS(VINSBRX, do_vinsx_VX, 1, true, gen_helper_VINSBLX)
TRANS(VINSHRX, do_vinsx_VX, 2, true, gen_helper_VINSHLX)
TRANS(VINSWRX, do_vinsx_VX, 4, true, gen_helper_VINSWLX)
TRANS(VINSDRX, do_vinsx_VX, 8, true, gen_helper_VINSDLX)

TRANS(VINSW, do_vins_VX_uim4, 4, gen_helper_VINSWLX)
TRANS(VINSD, do_vins_VX_uim4, 8, gen_helper_VINSDLX)

TRANS(VINSBVLX, do_vinsvx_VX, 1, false, gen_helper_VINSBLX)
TRANS(VINSHVLX, do_vinsvx_VX, 2, false, gen_helper_VINSHLX)
TRANS(VINSWVLX, do_vinsvx_VX, 4, false, gen_helper_VINSWLX)

TRANS(VINSBVRX, do_vinsvx_VX, 1, true, gen_helper_VINSBLX)
TRANS(VINSHVRX, do_vinsvx_VX, 2, true, gen_helper_VINSHLX)
TRANS(VINSWVRX, do_vinsvx_VX, 4, true, gen_helper_VINSWLX)

TRANS(VINSERTB, do_vinsert_VX_uim4, 1, gen_helper_VINSBLX)
TRANS(VINSERTH, do_vinsert_VX_uim4, 2, gen_helper_VINSHLX)
TRANS(VINSERTW, do_vinsert_VX_uim4, 4, gen_helper_VINSWLX)
TRANS(VINSERTD, do_vinsert_VX_uim4, 8, gen_helper_VINSDLX)

static void gen_vsldoi(DisasContext *ctx)
{
    TCGv_ptr ra, rb, rd;
    TCGv_i32 sh;
    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }
    if (native_vmxint_on) {
        /* high 128 bits of (vA:vB) << 8 * SH */
        int n = VSH(ctx->opcode), q = n >> 3, s = (n & 7) * 8;
        TCGv_i64 w[4];

        for (int i = q; i < q + 2 + !!s; i++) {
            w[i] = tcg_temp_new_i64();
            get_avr64(w[i], i < 2 ? rA(ctx->opcode) : rB(ctx->opcode),
                      !(i & 1));
        }
        if (s) {
            tcg_gen_extract2_i64(w[q], w[q + 1], w[q], 64 - s);
            tcg_gen_extract2_i64(w[q + 1], w[q + 2], w[q + 1], 64 - s);
        }
        set_avr64(rD(ctx->opcode), w[q], true);
        set_avr64(rD(ctx->opcode), w[q + 1], false);
        return;
    }
    ra = gen_avr_ptr(rA(ctx->opcode));
    rb = gen_avr_ptr(rB(ctx->opcode));
    rd = gen_avr_ptr(rD(ctx->opcode));
    sh = tcg_constant_i32(VSH(ctx->opcode));
    gen_helper_vsldoi(rd, ra, rb, sh);
}

static bool trans_VSLDBI(DisasContext *ctx, arg_VN *a)
{
    TCGv_i64 t0, t1, t2;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    get_avr64(t0, a->vra, true);
    get_avr64(t1, a->vra, false);

    if (a->sh != 0) {
        t2 = tcg_temp_new_i64();

        get_avr64(t2, a->vrb, true);

        tcg_gen_extract2_i64(t0, t1, t0, 64 - a->sh);
        tcg_gen_extract2_i64(t1, t2, t1, 64 - a->sh);
    }

    set_avr64(a->vrt, t0, true);
    set_avr64(a->vrt, t1, false);
    return true;
}

static bool trans_VSRDBI(DisasContext *ctx, arg_VN *a)
{
    TCGv_i64 t2, t1, t0;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    get_avr64(t0, a->vrb, false);
    get_avr64(t1, a->vrb, true);

    if (a->sh != 0) {
        t2 = tcg_temp_new_i64();

        get_avr64(t2, a->vra, false);

        tcg_gen_extract2_i64(t0, t0, t1, a->sh);
        tcg_gen_extract2_i64(t1, t1, t2, a->sh);
    }

    set_avr64(a->vrt, t0, false);
    set_avr64(a->vrt, t1, true);
    return true;
}

static bool do_vexpand(DisasContext *ctx, arg_VX_tb *a, unsigned vece)
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_sari(vece, avr_full_offset(a->vrt), avr_full_offset(a->vrb),
                      (8 << vece) - 1, 16, 16);

    return true;
}

TRANS(VEXPANDBM, do_vexpand, MO_8)
TRANS(VEXPANDHM, do_vexpand, MO_16)
TRANS(VEXPANDWM, do_vexpand, MO_32)
TRANS(VEXPANDDM, do_vexpand, MO_64)

static bool trans_VEXPANDQM(DisasContext *ctx, arg_VX_tb *a)
{
    TCGv_i64 tmp;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tmp = tcg_temp_new_i64();

    get_avr64(tmp, a->vrb, true);
    tcg_gen_sari_i64(tmp, tmp, 63);
    set_avr64(a->vrt, tmp, false);
    set_avr64(a->vrt, tmp, true);
    return true;
}

static bool do_vextractm(DisasContext *ctx, arg_VX_tb *a, unsigned vece)
{
    const uint64_t elem_width = 8 << vece, elem_count_half = 8 >> vece,
                   mask = dup_const(vece, 1ULL << (elem_width - 1));
    uint64_t i, j;
    TCGv_i64 lo, hi, t0, t1;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    hi = tcg_temp_new_i64();
    lo = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    get_avr64(lo, a->vrb, false);
    get_avr64(hi, a->vrb, true);

    tcg_gen_andi_i64(lo, lo, mask);
    tcg_gen_andi_i64(hi, hi, mask);

    /*
     * Gather the most significant bit of each element in the highest element
     * element. E.g. for bytes:
     * aXXXXXXXbXXXXXXXcXXXXXXXdXXXXXXXeXXXXXXXfXXXXXXXgXXXXXXXhXXXXXXX
     *     & dup(1 << (elem_width - 1))
     * a0000000b0000000c0000000d0000000e0000000f0000000g0000000h0000000
     *     << 32 - 4
     * 0000e0000000f0000000g0000000h00000000000000000000000000000000000
     *     |
     * a000e000b000f000c000g000d000h000e0000000f0000000g0000000h0000000
     *     << 16 - 2
     * 00c000g000d000h000e0000000f0000000g0000000h000000000000000000000
     *     |
     * a0c0e0g0b0d0f0h0c0e0g000d0f0h000e0g00000f0h00000g0000000h0000000
     *     << 8 - 1
     * 0b0d0f0h0c0e0g000d0f0h000e0g00000f0h00000g0000000h00000000000000
     *     |
     * abcdefghbcdefgh0cdefgh00defgh000efgh0000fgh00000gh000000h0000000
     */
    for (i = elem_count_half / 2, j = 32; i > 0; i >>= 1, j >>= 1) {
        tcg_gen_shli_i64(t0, hi, j - i);
        tcg_gen_shli_i64(t1, lo, j - i);
        tcg_gen_or_i64(hi, hi, t0);
        tcg_gen_or_i64(lo, lo, t1);
    }

    tcg_gen_shri_i64(hi, hi, 64 - elem_count_half);
    tcg_gen_extract2_i64(lo, lo, hi, 64 - elem_count_half);
    tcg_gen_trunc_i64_tl(cpu_gpr[a->vrt], lo);
    return true;
}

TRANS(VEXTRACTBM, do_vextractm, MO_8)
TRANS(VEXTRACTHM, do_vextractm, MO_16)
TRANS(VEXTRACTWM, do_vextractm, MO_32)
TRANS(VEXTRACTDM, do_vextractm, MO_64)

static bool trans_VEXTRACTQM(DisasContext *ctx, arg_VX_tb *a)
{
    TCGv_i64 tmp;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tmp = tcg_temp_new_i64();

    get_avr64(tmp, a->vrb, true);
    tcg_gen_shri_i64(tmp, tmp, 63);
    tcg_gen_trunc_i64_tl(cpu_gpr[a->vrt], tmp);
    return true;
}

static bool do_mtvsrm(DisasContext *ctx, arg_VX_tb *a, unsigned vece)
{
    const uint64_t elem_width = 8 << vece, elem_count_half = 8 >> vece;
    uint64_t c;
    int i, j;
    TCGv_i64 hi, lo, t0, t1;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    hi = tcg_temp_new_i64();
    lo = tcg_temp_new_i64();
    t0 = tcg_temp_new_i64();
    t1 = tcg_temp_new_i64();

    tcg_gen_extu_tl_i64(t0, cpu_gpr[a->vrb]);
    tcg_gen_extract_i64(hi, t0, elem_count_half, elem_count_half);
    tcg_gen_extract_i64(lo, t0, 0, elem_count_half);

    /*
     * Spread the bits into their respective elements.
     * E.g. for bytes:
     * 00000000000000000000000000000000000000000000000000000000abcdefgh
     *   << 32 - 4
     * 0000000000000000000000000000abcdefgh0000000000000000000000000000
     *   |
     * 0000000000000000000000000000abcdefgh00000000000000000000abcdefgh
     *   << 16 - 2
     * 00000000000000abcdefgh00000000000000000000abcdefgh00000000000000
     *   |
     * 00000000000000abcdefgh000000abcdefgh000000abcdefgh000000abcdefgh
     *   << 8 - 1
     * 0000000abcdefgh000000abcdefgh000000abcdefgh000000abcdefgh0000000
     *   |
     * 0000000abcdefgXbcdefgXbcdefgXbcdefgXbcdefgXbcdefgXbcdefgXbcdefgh
     *   & dup(1)
     * 0000000a0000000b0000000c0000000d0000000e0000000f0000000g0000000h
     *   * 0xff
     * aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffffgggggggghhhhhhhh
     */
    for (i = elem_count_half / 2, j = 32; i > 0; i >>= 1, j >>= 1) {
        tcg_gen_shli_i64(t0, hi, j - i);
        tcg_gen_shli_i64(t1, lo, j - i);
        tcg_gen_or_i64(hi, hi, t0);
        tcg_gen_or_i64(lo, lo, t1);
    }

    c = dup_const(vece, 1);
    tcg_gen_andi_i64(hi, hi, c);
    tcg_gen_andi_i64(lo, lo, c);

    c = MAKE_64BIT_MASK(0, elem_width);
    tcg_gen_muli_i64(hi, hi, c);
    tcg_gen_muli_i64(lo, lo, c);

    set_avr64(a->vrt, lo, false);
    set_avr64(a->vrt, hi, true);
    return true;
}

TRANS(MTVSRBM, do_mtvsrm, MO_8)
TRANS(MTVSRHM, do_mtvsrm, MO_16)
TRANS(MTVSRWM, do_mtvsrm, MO_32)
TRANS(MTVSRDM, do_mtvsrm, MO_64)

static bool trans_MTVSRQM(DisasContext *ctx, arg_VX_tb *a)
{
    TCGv_i64 tmp;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tmp = tcg_temp_new_i64();

    tcg_gen_ext_tl_i64(tmp, cpu_gpr[a->vrb]);
    tcg_gen_sextract_i64(tmp, tmp, 0, 1);
    set_avr64(a->vrt, tmp, false);
    set_avr64(a->vrt, tmp, true);
    return true;
}

static bool trans_MTVSRBMI(DisasContext *ctx, arg_DX_b *a)
{
    const uint64_t mask = dup_const(MO_8, 1);
    uint64_t hi, lo;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    hi = extract16(a->b, 8, 8);
    lo = extract16(a->b, 0, 8);

    for (int i = 4, j = 32; i > 0; i >>= 1, j >>= 1) {
        hi |= hi << (j - i);
        lo |= lo << (j - i);
    }

    hi = (hi & mask) * 0xFF;
    lo = (lo & mask) * 0xFF;

    set_avr64(a->vrt, tcg_constant_i64(hi), true);
    set_avr64(a->vrt, tcg_constant_i64(lo), false);

    return true;
}

static bool do_vcntmb(DisasContext *ctx, arg_VX_mp *a, int vece)
{
    TCGv_i64 r[2], mask;

    r[0] = tcg_temp_new_i64();
    r[1] = tcg_temp_new_i64();
    mask = tcg_constant_i64(dup_const(vece, 1ULL << ((8 << vece) - 1)));

    for (int i = 0; i < 2; i++) {
        get_avr64(r[i], a->vrb, i);
        if (a->mp) {
            tcg_gen_and_i64(r[i], mask, r[i]);
        } else {
            tcg_gen_andc_i64(r[i], mask, r[i]);
        }
        tcg_gen_ctpop_i64(r[i], r[i]);
    }

    tcg_gen_add_i64(r[0], r[0], r[1]);
    tcg_gen_shli_i64(r[0], r[0], TARGET_LONG_BITS - 8 + vece);
    tcg_gen_trunc_i64_tl(cpu_gpr[a->rt], r[0]);
    return true;
}

TRANS(VCNTMBB, do_vcntmb, MO_8)
TRANS(VCNTMBH, do_vcntmb, MO_16)
TRANS(VCNTMBW, do_vcntmb, MO_32)
TRANS(VCNTMBD, do_vcntmb, MO_64)

static bool do_vstri(DisasContext *ctx, arg_VX_tb_rc *a,
                     void (*gen_helper)(TCGv_i32, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr vrt, vrb;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    vrt = gen_avr_ptr(a->vrt);
    vrb = gen_avr_ptr(a->vrb);

    if (a->rc) {
        gen_helper(cpu_crf[6], vrt, vrb);
    } else {
        TCGv_i32 discard = tcg_temp_new_i32();
        gen_helper(discard, vrt, vrb);
    }
    return true;
}

TRANS(VSTRIBL, do_vstri, gen_helper_VSTRIBL)
TRANS(VSTRIBR, do_vstri, gen_helper_VSTRIBR)
TRANS(VSTRIHL, do_vstri, gen_helper_VSTRIHL)
TRANS(VSTRIHR, do_vstri, gen_helper_VSTRIHR)

static bool do_vclrb(DisasContext *ctx, arg_VX *a, bool right)
{
    TCGv_i64 rb, mh, ml, tmp,
             ones = tcg_constant_i64(-1),
             zero = tcg_constant_i64(0);

    rb = tcg_temp_new_i64();
    mh = tcg_temp_new_i64();
    ml = tcg_temp_new_i64();
    tmp = tcg_temp_new_i64();

    tcg_gen_extu_tl_i64(rb, cpu_gpr[a->vrb]);
    tcg_gen_andi_i64(tmp, rb, 7);
    tcg_gen_shli_i64(tmp, tmp, 3);
    if (right) {
        tcg_gen_shr_i64(tmp, ones, tmp);
    } else {
        tcg_gen_shl_i64(tmp, ones, tmp);
    }
    tcg_gen_not_i64(tmp, tmp);

    if (right) {
        tcg_gen_movcond_i64(TCG_COND_LTU, mh, rb, tcg_constant_i64(8),
                            tmp, ones);
        tcg_gen_movcond_i64(TCG_COND_LTU, ml, rb, tcg_constant_i64(8),
                            zero, tmp);
        tcg_gen_movcond_i64(TCG_COND_LTU, ml, rb, tcg_constant_i64(16),
                            ml, ones);
    } else {
        tcg_gen_movcond_i64(TCG_COND_LTU, ml, rb, tcg_constant_i64(8),
                            tmp, ones);
        tcg_gen_movcond_i64(TCG_COND_LTU, mh, rb, tcg_constant_i64(8),
                            zero, tmp);
        tcg_gen_movcond_i64(TCG_COND_LTU, mh, rb, tcg_constant_i64(16),
                            mh, ones);
    }

    get_avr64(tmp, a->vra, true);
    tcg_gen_and_i64(tmp, tmp, mh);
    set_avr64(a->vrt, tmp, true);

    get_avr64(tmp, a->vra, false);
    tcg_gen_and_i64(tmp, tmp, ml);
    set_avr64(a->vrt, tmp, false);
    return true;
}

TRANS(VCLRLB, do_vclrb, false)
TRANS(VCLRRB, do_vclrb, true)

#define GEN_VAFORM_PAIRED(name0, name1, opc2)                           \
static void glue(gen_, name0##_##name1)(DisasContext *ctx)              \
    {                                                                   \
        TCGv_ptr ra, rb, rc, rd;                                        \
        if (unlikely(!ctx->altivec_enabled)) {                          \
            gen_exception(ctx, POWERPC_EXCP_VPU);                       \
            return;                                                     \
        }                                                               \
        ra = gen_avr_ptr(rA(ctx->opcode));                              \
        rb = gen_avr_ptr(rB(ctx->opcode));                              \
        rc = gen_avr_ptr(rC(ctx->opcode));                              \
        rd = gen_avr_ptr(rD(ctx->opcode));                              \
        if (Rc(ctx->opcode)) {                                          \
            gen_helper_##name1(tcg_env, rd, ra, rb, rc);                \
        } else {                                                        \
            gen_helper_##name0(tcg_env, rd, ra, rb, rc);                \
        }                                                               \
    }

static void gen_vmaddfp_vnmsubfp(DisasContext *ctx)
{
    bool neg = Rc(ctx->opcode);
    void (*helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr) =
        neg ? gen_helper_vnmsubfp : gen_helper_vmaddfp;
    int vrt = rD(ctx->opcode), vra = rA(ctx->opcode);
    int vrb = rB(ctx->opcode), vrc = rC(ctx->opcode);

    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }
    if (native_vmx_on) {
        native_vmx_op(neg ? VFPARITH_NMSUB : VFPARITH_MADD,
                      vrt, vra, vrb, vrc);
        return;
    }
    helper(tcg_env, gen_avr_ptr(vrt), gen_avr_ptr(vra), gen_avr_ptr(vrb),
           gen_avr_ptr(vrc));
}

static bool do_va_helper(DisasContext *ctx, arg_VA *a,
    void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr vrt, vra, vrb, vrc;
    REQUIRE_VECTOR(ctx);

    vrt = gen_avr_ptr(a->vrt);
    vra = gen_avr_ptr(a->vra);
    vrb = gen_avr_ptr(a->vrb);
    vrc = gen_avr_ptr(a->rc);
    gen_helper(vrt, vra, vrb, vrc);
    return true;
}

TRANS_FLAGS2(ISA207, VADDECUQ, do_va_helper, gen_helper_VADDECUQ)
TRANS_FLAGS2(ISA207, VADDEUQM, do_va_helper, gen_helper_VADDEUQM)

TRANS_FLAGS2(ISA207, VSUBEUQM, do_va_helper, gen_helper_VSUBEUQM)
TRANS_FLAGS2(ISA207, VSUBECUQ, do_va_helper, gen_helper_VSUBECUQ)

/* host order: table vB:vA, index 31 - (vC & 31) */
static bool trans_VPERM(DisasContext *ctx, arg_VA *a)
{
    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    if (native_vmxint_on && !HOST_BIG_ENDIAN && tcg_can_emit_perm2()) {
        TCGv_vec i, r;

        REQUIRE_VECTOR(ctx);
        i = tcg_temp_new_vec(TCG_TYPE_V128);
        r = tcg_temp_new_vec(TCG_TYPE_V128);
        tcg_gen_andc_vec(MO_8, i, tcg_constant_vec(TCG_TYPE_V128, MO_8, 31),
                         native_avr(a->rc));
        tcg_gen_perm2_vec(r, native_avr(a->vrb), native_avr(a->vra), i);
        tcg_gen_st_vec(r, tcg_env, avr_full_offset(a->vrt));
        return true;
    }
    return do_va_helper(ctx, a, native_vmxint_on ? gen_helper_VPERMN
                                                 : gen_helper_VPERM);
}
TRANS_FLAGS2(ISA300, VPERMR, do_va_helper, gen_helper_VPERMR)

static void gen_vmladduhm_vec(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b,
                              TCGv_vec c)
{
    tcg_gen_mul_vec(vece, t, a, b);
    tcg_gen_add_vec(vece, t, t, c);
}

static bool trans_VMLADDUHM(DisasContext *ctx, arg_VA *a)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_add_vec, INDEX_op_mul_vec, 0
    };

    static const GVecGen4 op = {
        .fno = gen_helper_VMLADDUHM,
        .fniv = gen_vmladduhm_vec,
        .opt_opc = vecop_list,
        .vece = MO_16
    };

    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_4(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), avr_full_offset(a->rc),
                   16, 16, &op);

    return true;
}

static bool trans_VSEL(DisasContext *ctx, arg_VA *a)
{
    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_bitsel(MO_64, avr_full_offset(a->vrt), avr_full_offset(a->rc),
                        avr_full_offset(a->vrb), avr_full_offset(a->vra),
                        16, 16);

    return true;
}

static bool do_va_int(DisasContext *ctx, arg_VA *a, VmxIntFn *fn,
    VmxIntFn *dfn, void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    REQUIRE_VECTOR(ctx);
    if (dfn && vmxint_ok_at(vmxint_list_dot, MO_32)) {
        vmxint_gen(vmxint_list_dot, dfn, false, a->vrt, a->vra, a->vrb, a->rc);
        return true;
    }
    if (vmxint_ok(vmxint_list)) {
        vmxint_gen(vmxint_list, fn, false, a->vrt, a->vra, a->vrb, a->rc);
        return true;
    }
    return do_va_helper(ctx, a, gen_helper);
}

TRANS_FLAGS(ALTIVEC, VMSUMUBM, do_va_int, gen_vmsumubm_vec, NULL,
            gen_helper_VMSUMUBM)
TRANS_FLAGS(ALTIVEC, VMSUMMBM, do_va_int, gen_vmsummbm_vec, NULL,
            gen_helper_VMSUMMBM)
TRANS_FLAGS(ALTIVEC, VMSUMSHM, do_va_int, gen_vmsumshm_vec,
            gen_vmsumshm_dot, gen_helper_VMSUMSHM)
TRANS_FLAGS(ALTIVEC, VMSUMUHM, do_va_int, gen_vmsumuhm_vec,
            gen_vmsumuhm_dot, gen_helper_VMSUMUHM)

static bool do_va_env_helper(DisasContext *ctx, arg_VA *a,
    void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr vrt, vra, vrb, vrc;
    REQUIRE_VECTOR(ctx);

    vrt = gen_avr_ptr(a->vrt);
    vra = gen_avr_ptr(a->vra);
    vrb = gen_avr_ptr(a->vrb);
    vrc = gen_avr_ptr(a->rc);
    gen_helper(tcg_env, vrt, vra, vrb, vrc);
    return true;
}

static bool do_va_int_sat(DisasContext *ctx, arg_VA *a,
    const TCGOpcode *list, VmxIntFn *fn,
    const TCGOpcode *dlist, unsigned dvece, VmxIntFn *dfn,
    void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    REQUIRE_VECTOR(ctx);
    if (dfn && vmxint_ok_at(dlist, dvece)) {
        vmxint_gen(dlist, dfn, true, a->vrt, a->vra, a->vrb, a->rc);
        return true;
    }
    if (vmxint_ok(list)) {
        vmxint_gen(list, fn, true, a->vrt, a->vra, a->vrb, a->rc);
        return true;
    }
    return do_va_env_helper(ctx, a, gen_helper);
}

TRANS_FLAGS(ALTIVEC, VMSUMUHS, do_va_int_sat, vmxint_list,
            gen_vmsumuhs_vec, NULL, 0, NULL, gen_helper_VMSUMUHS)
TRANS_FLAGS(ALTIVEC, VMSUMSHS, do_va_int_sat, vmxint_list_ss,
            gen_vmsumshs_vec, vmxint_list_dot_ss, MO_32, gen_vmsumshs_dot,
            gen_helper_VMSUMSHS)

TRANS_FLAGS(ALTIVEC, VMHADDSHS, do_va_int_sat, vmxint_list,
            gen_vmhaddshs_vec, vmxint_list_mh, MO_16, gen_vmhaddshs_rs,
            gen_helper_VMHADDSHS)
TRANS_FLAGS(ALTIVEC, VMHRADDSHS, do_va_int_sat, vmxint_list,
            gen_vmhraddshs_vec, vmxint_list_mh, MO_16, gen_vmhraddshs_rs,
            gen_helper_VMHRADDSHS)

GEN_VXFORM_NOA(vclzb, 1, 28)
GEN_VXFORM_NOA(vclzh, 1, 29)
GEN_VXFORM_TRANS(vclzw, 1, 30)
GEN_VXFORM_TRANS(vclzd, 1, 31)

static bool do_vneg(DisasContext *ctx, arg_VX_tb *a, unsigned vece)
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_neg(vece, avr_full_offset(a->vrt), avr_full_offset(a->vrb),
                     16, 16);
    return true;
}

TRANS(VNEGW, do_vneg, MO_32)
TRANS(VNEGD, do_vneg, MO_64)

static void gen_vexts_i64(TCGv_i64 t, TCGv_i64 b, int64_t s)
{
    tcg_gen_sextract_i64(t, b, 0, 64 - s);
}

static void gen_vexts_i32(TCGv_i32 t, TCGv_i32 b, int32_t s)
{
    tcg_gen_sextract_i32(t, b, 0, 32 - s);
}

static void gen_vexts_vec(unsigned vece, TCGv_vec t, TCGv_vec b, int64_t s)
{
    tcg_gen_shli_vec(vece, t, b, s);
    tcg_gen_sari_vec(vece, t, t, s);
}

static bool do_vexts(DisasContext *ctx, arg_VX_tb *a, unsigned vece, int64_t s)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_shli_vec, INDEX_op_sari_vec, 0
    };

    static const GVecGen2i op[2] = {
        {
            .fni4 = gen_vexts_i32,
            .fniv = gen_vexts_vec,
            .opt_opc = vecop_list,
            .vece = MO_32
        },
        {
            .fni8 = gen_vexts_i64,
            .fniv = gen_vexts_vec,
            .opt_opc = vecop_list,
            .vece = MO_64
        },
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_2i(avr_full_offset(a->vrt), avr_full_offset(a->vrb),
                    16, 16, s, &op[vece - MO_32]);

    return true;
}

TRANS(VEXTSB2W, do_vexts, MO_32, 24);
TRANS(VEXTSH2W, do_vexts, MO_32, 16);
TRANS(VEXTSB2D, do_vexts, MO_64, 56);
TRANS(VEXTSH2D, do_vexts, MO_64, 48);
TRANS(VEXTSW2D, do_vexts, MO_64, 32);

static bool trans_VEXTSD2Q(DisasContext *ctx, arg_VX_tb *a)
{
    TCGv_i64 tmp;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tmp = tcg_temp_new_i64();

    get_avr64(tmp, a->vrb, false);
    set_avr64(a->vrt, tmp, false);
    tcg_gen_sari_i64(tmp, tmp, 63);
    set_avr64(a->vrt, tmp, true);
    return true;
}

GEN_VXFORM_NOA_2(vctzb, 1, 24, 28)
GEN_VXFORM_NOA_2(vctzh, 1, 24, 29)
GEN_VXFORM_NOA_2(vctzw, 1, 24, 30)
GEN_VXFORM_NOA_2(vctzd, 1, 24, 31)
GEN_VXFORM_NOA_3(vclzlsbb, 1, 24, 0)
GEN_VXFORM_NOA_3(vctzlsbb, 1, 24, 1)
GEN_VXFORM_NOA(vpopcntb, 1, 28)
GEN_VXFORM_NOA(vpopcnth, 1, 29)
GEN_VXFORM_NOA(vpopcntw, 1, 30)
GEN_VXFORM_NOA(vpopcntd, 1, 31)
GEN_VXFORM_DUAL(vclzb, PPC_NONE, PPC2_ISA207, \
                vpopcntb, PPC_NONE, PPC2_ISA207)
GEN_VXFORM_DUAL(vclzh, PPC_NONE, PPC2_ISA207, \
                vpopcnth, PPC_NONE, PPC2_ISA207)
GEN_VXFORM_DUAL(vclzw, PPC_NONE, PPC2_ISA207, \
                vpopcntw, PPC_NONE, PPC2_ISA207)
GEN_VXFORM_DUAL(vclzd, PPC_NONE, PPC2_ISA207, \
                vpopcntd, PPC_NONE, PPC2_ISA207)
GEN_VXFORM(vbpermd, 6, 23);
GEN_VXFORM(vbpermq, 6, 21);
GEN_VXFORM_TRANS(vgbbd, 6, 20);
GEN_VXFORM(vpmsumb, 4, 16)
GEN_VXFORM(vpmsumh, 4, 17)
GEN_VXFORM(vpmsumw, 4, 18)

static bool do_bcd_ps(DisasContext *ctx, arg_VX_ps *a,
                   void (*helper)(TCGv_i32, TCGv_ptr, TCGv_ptr,
                                  TCGv_ptr, TCGv_i32))
{
    TCGv_ptr ra, rb, rd;
    TCGv_i32 ps;

    REQUIRE_VECTOR(ctx);

    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    ps = tcg_constant_i32(a->ps);

    helper(cpu_crf[6], rd, ra, rb, ps);
    return true;
}

static bool do_bcd(DisasContext *ctx, arg_VX *a,
                   void (*helper)(TCGv_i32, TCGv_ptr, TCGv_ptr,
                                  TCGv_ptr, TCGv_i32))
{
    TCGv_ptr ra, rb, rd;
    TCGv_i32 ps;

    REQUIRE_VECTOR(ctx);

    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    ps = tcg_constant_i32(0);

    helper(cpu_crf[6], rd, ra, rb, ps);
    return true;
}

static bool do_bcd_tb(DisasContext *ctx, arg_VX_tb *a,
                    void (*helper)(TCGv_i32, TCGv_ptr, TCGv_ptr, TCGv_i32))
{
    TCGv_ptr rd, rb;
    TCGv_i32 ps;

    REQUIRE_VECTOR(ctx);

    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);

    ps = tcg_constant_i32(0);

    helper(cpu_crf[6], rd, rb, ps);

    return true;
}

static bool do_bcd_tb_ps(DisasContext *ctx, arg_VX_tb_ps *a,
                    void (*helper)(TCGv_i32, TCGv_ptr, TCGv_ptr, TCGv_i32))
{
    TCGv_ptr rd, rb;
    TCGv_i32 ps;

    REQUIRE_VECTOR(ctx);

    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);

    ps = tcg_constant_i32(a->ps);

    helper(cpu_crf[6], rd, rb, ps);

    return true;
}

TRANS_FLAGS2(ISA207, BCDADD, do_bcd_ps, gen_helper_BCDADD)
TRANS_FLAGS2(ISA207, BCDSUB, do_bcd_ps, gen_helper_BCDSUB)
TRANS_FLAGS2(ISA300, BCDUS, do_bcd, gen_helper_BCDUS)
TRANS_FLAGS2(ISA300, BCDS, do_bcd_ps, gen_helper_BCDS)
TRANS_FLAGS2(ISA300, BCDCFN, do_bcd_tb_ps, gen_helper_BCDCFN)
TRANS_FLAGS2(ISA300, BCDCTN, do_bcd_tb, gen_helper_BCDCTN)
TRANS_FLAGS2(ISA300, BCDCFZ, do_bcd_tb_ps, gen_helper_BCDCFZ)
TRANS_FLAGS2(ISA300, BCDCTZ, do_bcd_tb_ps, gen_helper_BCDCTZ)
TRANS_FLAGS2(ISA300, BCDCFSQ, do_bcd_tb_ps, gen_helper_BCDCFSQ)
TRANS_FLAGS2(ISA300, BCDCTSQ, do_bcd_tb, gen_helper_BCDCTSQ)
TRANS_FLAGS2(ISA300, BCDSETSGN, do_bcd_tb_ps, gen_helper_BCDSETSGN)
TRANS_FLAGS2(ISA300, BCDCPSGN, do_bcd, gen_helper_BCDCPSGN)
TRANS_FLAGS2(ISA300, BCDTRUNC, do_bcd_ps, gen_helper_BCDTRUNC)
TRANS_FLAGS2(ISA300, BCDUTRUNC, do_bcd, gen_helper_BCDUTRUNC)
TRANS_FLAGS2(ISA300, BCDSR, do_bcd_ps, gen_helper_BCDSR)

static void gen_vsbox(DisasContext *ctx)
{
    TCGv_ptr ra, rd;
    if (unlikely(!ctx->altivec_enabled)) {
        gen_exception(ctx, POWERPC_EXCP_VPU);
        return;
    }
    ra = gen_avr_ptr(rA(ctx->opcode));
    rd = gen_avr_ptr(rD(ctx->opcode));
    gen_helper_vsbox(rd, ra);
}

GEN_VXFORM(vcipher, 4, 20)
GEN_VXFORM(vcipherlast, 4, 20)
GEN_VXFORM(vncipher, 4, 21)
GEN_VXFORM(vncipherlast, 4, 21)

GEN_VXFORM_DUAL(vcipher, PPC_NONE, PPC2_ISA207,
                vcipherlast, PPC_NONE, PPC2_ISA207)
GEN_VXFORM_DUAL(vncipher, PPC_NONE, PPC2_ISA207,
                vncipherlast, PPC_NONE, PPC2_ISA207)

#define VSHASIGMA(op)                         \
static void gen_##op(DisasContext *ctx)       \
{                                             \
    TCGv_ptr ra, rd;                          \
    TCGv_i32 st_six;                          \
    if (unlikely(!ctx->altivec_enabled)) {    \
        gen_exception(ctx, POWERPC_EXCP_VPU); \
        return;                               \
    }                                         \
    ra = gen_avr_ptr(rA(ctx->opcode));        \
    rd = gen_avr_ptr(rD(ctx->opcode));        \
    st_six = tcg_constant_i32(rB(ctx->opcode));  \
    gen_helper_##op(rd, ra, st_six);          \
}

VSHASIGMA(vshasigmaw)
VSHASIGMA(vshasigmad)

GEN_VXFORM3(vpermxor, 22, 0xFF)
GEN_VXFORM_DUAL(vsldoi, PPC_ALTIVEC, PPC_NONE,
                vpermxor, PPC_NONE, PPC2_ISA207)

static bool trans_VCFUGED(DisasContext *ctx, arg_VX *a)
{
    static const GVecGen3 g = {
        .fni8 = gen_helper_CFUGED,
        .vece = MO_64,
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &g);

    return true;
}

static bool trans_VCLZDM(DisasContext *ctx, arg_VX *a)
{
    static const GVecGen3i g = {
        .fni8 = do_cntzdm,
        .vece = MO_64,
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3i(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                    avr_full_offset(a->vrb), 16, 16, false, &g);

    return true;
}

static bool trans_VCTZDM(DisasContext *ctx, arg_VX *a)
{
    static const GVecGen3i g = {
        .fni8 = do_cntzdm,
        .vece = MO_64,
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3i(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                    avr_full_offset(a->vrb), 16, 16, true, &g);

    return true;
}

static bool trans_VPDEPD(DisasContext *ctx, arg_VX *a)
{
    static const GVecGen3 g = {
        .fni8 = gen_helper_PDEPD,
        .vece = MO_64,
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &g);

    return true;
}

static bool trans_VPEXTD(DisasContext *ctx, arg_VX *a)
{
    static const GVecGen3 g = {
        .fni8 = gen_helper_PEXTD,
        .vece = MO_64,
    };

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &g);

    return true;
}

static bool trans_VMSUMUDM(DisasContext *ctx, arg_VA *a)
{
    TCGv_i64 rl, rh, src1, src2;
    int dw;

    REQUIRE_INSNS_FLAGS2(ctx, ISA300);
    REQUIRE_VECTOR(ctx);

    rh = tcg_temp_new_i64();
    rl = tcg_temp_new_i64();
    src1 = tcg_temp_new_i64();
    src2 = tcg_temp_new_i64();

    get_avr64(rl, a->rc, false);
    get_avr64(rh, a->rc, true);

    for (dw = 0; dw < 2; dw++) {
        get_avr64(src1, a->vra, dw);
        get_avr64(src2, a->vrb, dw);
        tcg_gen_mulu2_i64(src1, src2, src1, src2);
        tcg_gen_add2_i64(rl, rh, rl, rh, src1, src2);
    }

    set_avr64(a->vrt, rl, false);
    set_avr64(a->vrt, rh, true);
    return true;
}

static bool trans_VMSUMCUD(DisasContext *ctx, arg_VA *a)
{
    TCGv_i64 tmp0, tmp1, prod1h, prod1l, prod0h, prod0l, zero;

    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tmp0 = tcg_temp_new_i64();
    tmp1 = tcg_temp_new_i64();
    prod1h = tcg_temp_new_i64();
    prod1l = tcg_temp_new_i64();
    prod0h = tcg_temp_new_i64();
    prod0l = tcg_temp_new_i64();
    zero = tcg_constant_i64(0);

    /* prod1 = vsr[vra+32].dw[1] * vsr[vrb+32].dw[1] */
    get_avr64(tmp0, a->vra, false);
    get_avr64(tmp1, a->vrb, false);
    tcg_gen_mulu2_i64(prod1l, prod1h, tmp0, tmp1);

    /* prod0 = vsr[vra+32].dw[0] * vsr[vrb+32].dw[0] */
    get_avr64(tmp0, a->vra, true);
    get_avr64(tmp1, a->vrb, true);
    tcg_gen_mulu2_i64(prod0l, prod0h, tmp0, tmp1);

    /* Sum lower 64-bits elements */
    get_avr64(tmp1, a->rc, false);
    tcg_gen_add2_i64(tmp1, tmp0, tmp1, zero, prod1l, zero);
    tcg_gen_add2_i64(tmp1, tmp0, tmp1, tmp0, prod0l, zero);

    /*
     * Discard lower 64-bits, leaving the carry into bit 64.
     * Then sum the higher 64-bit elements.
     */
    get_avr64(tmp1, a->rc, true);
    tcg_gen_add2_i64(tmp1, tmp0, tmp0, zero, tmp1, zero);
    tcg_gen_add2_i64(tmp1, tmp0, tmp1, tmp0, prod1h, zero);
    tcg_gen_add2_i64(tmp1, tmp0, tmp1, tmp0, prod0h, zero);

    /* Discard 64 more bits to complete the CHOP128(temp >> 128) */
    set_avr64(a->vrt, tmp0, false);
    set_avr64(a->vrt, zero, true);
    return true;
}

static bool do_vx_helper(DisasContext *ctx, arg_VX *a,
                         void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    TCGv_ptr ra, rb, rd;
    REQUIRE_VECTOR(ctx);

    ra = gen_avr_ptr(a->vra);
    rb = gen_avr_ptr(a->vrb);
    rd = gen_avr_ptr(a->vrt);
    gen_helper(rd, ra, rb);
    return true;
}

TRANS_FLAGS2(ISA207, VADDCUQ, do_vx_helper, gen_helper_VADDCUQ)
TRANS_FLAGS2(ISA207, VADDUQM, do_vx_helper, gen_helper_VADDUQM)

TRANS_FLAGS2(ISA207, VPMSUMD, do_vx_helper, gen_helper_VPMSUMD)

TRANS_FLAGS2(ISA207, VSUBCUQ, do_vx_helper, gen_helper_VSUBCUQ)
TRANS_FLAGS2(ISA207, VSUBUQM, do_vx_helper, gen_helper_VSUBUQM)

static void gen_VADDCUW_vec(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_not_vec(vece, a, a);
    tcg_gen_cmp_vec(TCG_COND_LTU, vece, t, a, b);
    tcg_gen_and_vec(vece, t, t, tcg_constant_vec_matching(t, vece, 1));
}

static void gen_VADDCUW_i32(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)
{
    tcg_gen_not_i32(a, a);
    tcg_gen_setcond_i32(TCG_COND_LTU, t, a, b);
}

static void gen_VSUBCUW_vec(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_cmp_vec(TCG_COND_GEU, vece, t, a, b);
    tcg_gen_and_vec(vece, t, t, tcg_constant_vec_matching(t, vece, 1));
}

static void gen_VSUBCUW_i32(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)
{
    tcg_gen_setcond_i32(TCG_COND_GEU, t, a, b);
}

static bool do_vx_vaddsubcuw(DisasContext *ctx, arg_VX *a, int add)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_cmp_vec, 0
    };

    static const GVecGen3 op[] = {
        {
            .fniv = gen_VSUBCUW_vec,
            .fni4 = gen_VSUBCUW_i32,
            .opt_opc = vecop_list,
            .vece = MO_32
        },
        {
            .fniv = gen_VADDCUW_vec,
            .fni4 = gen_VADDCUW_i32,
            .opt_opc = vecop_list,
            .vece = MO_32
        },
    };

    REQUIRE_INSNS_FLAGS(ctx, ALTIVEC);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &op[add]);

    return true;
}

TRANS(VSUBCUW, do_vx_vaddsubcuw, 0)
TRANS(VADDCUW, do_vx_vaddsubcuw, 1)

/* Integer Add/Sub Saturate Instructions */
static inline void do_vadd_vsub_sat
(
    unsigned vece, TCGv_vec t, TCGv_vec qc, TCGv_vec a, TCGv_vec b,
    void (*norm_op)(unsigned, TCGv_vec, TCGv_vec, TCGv_vec),
    void (*sat_op)(unsigned, TCGv_vec, TCGv_vec, TCGv_vec))
{
    TCGv_vec x = tcg_temp_new_vec_matching(t);
    norm_op(vece, x, a, b);
    sat_op(vece, t, a, b);
    tcg_gen_xor_vec(vece, x, x, t);
    tcg_gen_or_vec(vece, qc, qc, x);
}

static void gen_vadd_sat_u(unsigned vece, TCGv_vec t, TCGv_vec sat,
                           TCGv_vec a, TCGv_vec b)
{
    do_vadd_vsub_sat(vece, t, sat, a, b, tcg_gen_add_vec, tcg_gen_usadd_vec);
}

static void gen_vadd_sat_s(unsigned vece, TCGv_vec t, TCGv_vec sat,
                           TCGv_vec a, TCGv_vec b)
{
    do_vadd_vsub_sat(vece, t, sat, a, b, tcg_gen_add_vec, tcg_gen_ssadd_vec);
}

static void gen_vsub_sat_u(unsigned vece, TCGv_vec t, TCGv_vec sat,
                           TCGv_vec a, TCGv_vec b)
{
    do_vadd_vsub_sat(vece, t, sat, a, b, tcg_gen_sub_vec, tcg_gen_ussub_vec);
}

static void gen_vsub_sat_s(unsigned vece, TCGv_vec t, TCGv_vec sat,
                           TCGv_vec a, TCGv_vec b)
{
    do_vadd_vsub_sat(vece, t, sat, a, b, tcg_gen_sub_vec, tcg_gen_sssub_vec);
}

/*
 * Signed/Unsigned add/sub helper ops for byte/halfword/word
 * GVecGen4 struct variants.
 */
static const TCGOpcode vecop_list_sub_u[] = {
    INDEX_op_sub_vec, INDEX_op_ussub_vec, 0
};
static const TCGOpcode vecop_list_sub_s[] = {
    INDEX_op_sub_vec, INDEX_op_sssub_vec, 0
};
static const TCGOpcode vecop_list_add_u[] = {
    INDEX_op_add_vec, INDEX_op_usadd_vec, 0
};
static const TCGOpcode vecop_list_add_s[] = {
    INDEX_op_add_vec, INDEX_op_ssadd_vec, 0
};

static const GVecGen4 op_vsububs = {
    .fniv = gen_vsub_sat_u,
    .fno = gen_helper_VSUBUBS,
    .opt_opc = vecop_list_sub_u,
    .write_aofs = true,
    .vece = MO_8
};

static const GVecGen4 op_vaddubs = {
    .fniv = gen_vadd_sat_u,
    .fno = gen_helper_VADDUBS,
    .opt_opc = vecop_list_add_u,
    .write_aofs = true,
    .vece = MO_8
};

static const GVecGen4 op_vsubuhs = {
    .fniv = gen_vsub_sat_u,
    .fno = gen_helper_VSUBUHS,
    .opt_opc = vecop_list_sub_u,
    .write_aofs = true,
    .vece = MO_16
};

static const GVecGen4 op_vadduhs = {
    .fniv = gen_vadd_sat_u,
    .fno = gen_helper_VADDUHS,
    .opt_opc = vecop_list_add_u,
    .write_aofs = true,
    .vece = MO_16
};

static const GVecGen4 op_vsubuws = {
    .fniv = gen_vsub_sat_u,
    .fno = gen_helper_VSUBUWS,
    .opt_opc = vecop_list_sub_u,
    .write_aofs = true,
    .vece = MO_32
};

static const GVecGen4 op_vadduws = {
    .fniv = gen_vadd_sat_u,
    .fno = gen_helper_VADDUWS,
    .opt_opc = vecop_list_add_u,
    .write_aofs = true,
    .vece = MO_32
};

static const GVecGen4 op_vsubsbs = {
    .fniv = gen_vsub_sat_s,
    .fno = gen_helper_VSUBSBS,
    .opt_opc = vecop_list_sub_s,
    .write_aofs = true,
    .vece = MO_8
};

static const GVecGen4 op_vaddsbs = {
    .fniv = gen_vadd_sat_s,
    .fno = gen_helper_VADDSBS,
    .opt_opc = vecop_list_add_s,
    .write_aofs = true,
    .vece = MO_8
};

static const GVecGen4 op_vsubshs = {
    .fniv = gen_vsub_sat_s,
    .fno = gen_helper_VSUBSHS,
    .opt_opc = vecop_list_sub_s,
    .write_aofs = true,
    .vece = MO_16
};

static const GVecGen4 op_vaddshs = {
    .fniv = gen_vadd_sat_s,
    .fno = gen_helper_VADDSHS,
    .opt_opc = vecop_list_add_s,
    .write_aofs = true,
    .vece = MO_16
};

static const GVecGen4 op_vsubsws = {
    .fniv = gen_vsub_sat_s,
    .fno = gen_helper_VSUBSWS,
    .opt_opc = vecop_list_sub_s,
    .write_aofs = true,
    .vece = MO_32
};

static const GVecGen4 op_vaddsws = {
    .fniv = gen_vadd_sat_s,
    .fno = gen_helper_VADDSWS,
    .opt_opc = vecop_list_add_s,
    .write_aofs = true,
    .vece = MO_32
};

static bool do_vx_vadd_vsub_sat(DisasContext *ctx, arg_VX *a, const GVecGen4 *op)
{
    REQUIRE_VECTOR(ctx);
    tcg_gen_gvec_4(avr_full_offset(a->vrt), offsetof(CPUPPCState, vscr_sat),
                   avr_full_offset(a->vra), avr_full_offset(a->vrb),
                   16, 16, op);

    return true;
}

TRANS_FLAGS(ALTIVEC, VSUBUBS, do_vx_vadd_vsub_sat, &op_vsububs)
TRANS_FLAGS(ALTIVEC, VSUBUHS, do_vx_vadd_vsub_sat, &op_vsubuhs)
TRANS_FLAGS(ALTIVEC, VSUBUWS, do_vx_vadd_vsub_sat, &op_vsubuws)
TRANS_FLAGS(ALTIVEC, VSUBSBS, do_vx_vadd_vsub_sat, &op_vsubsbs)
TRANS_FLAGS(ALTIVEC, VSUBSHS, do_vx_vadd_vsub_sat, &op_vsubshs)
TRANS_FLAGS(ALTIVEC, VSUBSWS, do_vx_vadd_vsub_sat, &op_vsubsws)
TRANS_FLAGS(ALTIVEC, VADDUBS, do_vx_vadd_vsub_sat, &op_vaddubs)
TRANS_FLAGS(ALTIVEC, VADDUHS, do_vx_vadd_vsub_sat, &op_vadduhs)
TRANS_FLAGS(ALTIVEC, VADDUWS, do_vx_vadd_vsub_sat, &op_vadduws)
TRANS_FLAGS(ALTIVEC, VADDSBS, do_vx_vadd_vsub_sat, &op_vaddsbs)
TRANS_FLAGS(ALTIVEC, VADDSHS, do_vx_vadd_vsub_sat, &op_vaddshs)
TRANS_FLAGS(ALTIVEC, VADDSWS, do_vx_vadd_vsub_sat, &op_vaddsws)

static bool do_vx_vmuleo(DisasContext *ctx, arg_VX *a, bool even,
                         void (*gen_mul)(TCGv_i64, TCGv_i64, TCGv_i64, TCGv_i64))
{
    TCGv_i64 vra, vrb, vrt0, vrt1;
    REQUIRE_VECTOR(ctx);

    vra = tcg_temp_new_i64();
    vrb = tcg_temp_new_i64();
    vrt0 = tcg_temp_new_i64();
    vrt1 = tcg_temp_new_i64();

    get_avr64(vra, a->vra, even);
    get_avr64(vrb, a->vrb, even);
    gen_mul(vrt0, vrt1, vra, vrb);
    set_avr64(a->vrt, vrt0, false);
    set_avr64(a->vrt, vrt1, true);
    return true;
}

static bool trans_VMULLD(DisasContext *ctx, arg_VX *a)
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_mul(MO_64, avr_full_offset(a->vrt), avr_full_offset(a->vra),
                     avr_full_offset(a->vrb), 16, 16);

    return true;
}

static void gen_vmul_vec(unsigned vece, bool even, bool sign, TCGv_vec t,
                         TCGv_vec a, TCGv_vec b)
{
    tcg_gen_mov_vec(t, vi_mul(vece, a, b, even, sign, sign));
}

#define VMUL_VEC(name, vece, even, sign)                                \
static void gen_##name##_vec(TCGv_vec t, TCGv_vec a, TCGv_vec b,        \
                             TCGv_vec c, TCGv_vec sat)                  \
{                                                                       \
    gen_vmul_vec(vece, even, sign, t, a, b);                            \
}
VMUL_VEC(vmulesb, MO_16, true, true)
VMUL_VEC(vmulosb, MO_16, false, true)
VMUL_VEC(vmuleub, MO_16, true, false)
VMUL_VEC(vmuloub, MO_16, false, false)
VMUL_VEC(vmulesh, MO_32, true, true)
VMUL_VEC(vmulosh, MO_32, false, true)
VMUL_VEC(vmuleuh, MO_32, true, false)
VMUL_VEC(vmulouh, MO_32, false, false)
#undef VMUL_VEC

static bool do_vx_int(DisasContext *ctx, arg_VX *a, VmxIntFn *fn,
                      void (*gen_helper)(TCGv_ptr, TCGv_ptr, TCGv_ptr))
{
    REQUIRE_VECTOR(ctx);
    if (vmxint_ok(vmxint_list)) {
        vmxint_gen(vmxint_list, fn, false, a->vrt, a->vra, a->vrb, -1);
        return true;
    }
    return do_vx_helper(ctx, a, gen_helper);
}

TRANS_FLAGS(ALTIVEC, VMULESB, do_vx_int, gen_vmulesb_vec, gen_helper_VMULESB)
TRANS_FLAGS(ALTIVEC, VMULOSB, do_vx_int, gen_vmulosb_vec, gen_helper_VMULOSB)
TRANS_FLAGS(ALTIVEC, VMULEUB, do_vx_int, gen_vmuleub_vec, gen_helper_VMULEUB)
TRANS_FLAGS(ALTIVEC, VMULOUB, do_vx_int, gen_vmuloub_vec, gen_helper_VMULOUB)
TRANS_FLAGS(ALTIVEC, VMULESH, do_vx_int, gen_vmulesh_vec, gen_helper_VMULESH)
TRANS_FLAGS(ALTIVEC, VMULOSH, do_vx_int, gen_vmulosh_vec, gen_helper_VMULOSH)
TRANS_FLAGS(ALTIVEC, VMULEUH, do_vx_int, gen_vmuleuh_vec, gen_helper_VMULEUH)
TRANS_FLAGS(ALTIVEC, VMULOUH, do_vx_int, gen_vmulouh_vec, gen_helper_VMULOUH)
TRANS_FLAGS2(ISA207, VMULESW, do_vx_helper, gen_helper_VMULESW)
TRANS_FLAGS2(ISA207, VMULOSW, do_vx_helper, gen_helper_VMULOSW)
TRANS_FLAGS2(ISA207, VMULEUW, do_vx_helper, gen_helper_VMULEUW)
TRANS_FLAGS2(ISA207, VMULOUW, do_vx_helper, gen_helper_VMULOUW)
TRANS_FLAGS2(ISA310, VMULESD, do_vx_vmuleo, true , tcg_gen_muls2_i64)
TRANS_FLAGS2(ISA310, VMULOSD, do_vx_vmuleo, false, tcg_gen_muls2_i64)
TRANS_FLAGS2(ISA310, VMULEUD, do_vx_vmuleo, true , tcg_gen_mulu2_i64)
TRANS_FLAGS2(ISA310, VMULOUD, do_vx_vmuleo, false, tcg_gen_mulu2_i64)

static void do_vx_vmulhw_i64(TCGv_i64 t, TCGv_i64 a, TCGv_i64 b, bool sign)
{
    TCGv_i64 hh, lh, temp;

    hh = tcg_temp_new_i64();
    lh = tcg_temp_new_i64();
    temp = tcg_temp_new_i64();

    if (sign) {
        tcg_gen_ext32s_i64(lh, a);
        tcg_gen_ext32s_i64(temp, b);
    } else {
        tcg_gen_ext32u_i64(lh, a);
        tcg_gen_ext32u_i64(temp, b);
    }
    tcg_gen_mul_i64(lh, lh, temp);

    if (sign) {
        tcg_gen_sari_i64(hh, a, 32);
        tcg_gen_sari_i64(temp, b, 32);
    } else {
        tcg_gen_shri_i64(hh, a, 32);
        tcg_gen_shri_i64(temp, b, 32);
    }
    tcg_gen_mul_i64(hh, hh, temp);

    tcg_gen_shri_i64(lh, lh, 32);
    tcg_gen_deposit_i64(t, hh, lh, 0, 32);
}

static void do_vx_vmulhd_i64(TCGv_i64 t, TCGv_i64 a, TCGv_i64 b, bool sign)
{
    TCGv_i64 tlow;

    tlow  = tcg_temp_new_i64();
    if (sign) {
        tcg_gen_muls2_i64(tlow, t, a, b);
    } else {
        tcg_gen_mulu2_i64(tlow, t, a, b);
    }
}

static bool do_vx_mulh(DisasContext *ctx, arg_VX *a, bool sign,
                       void (*func)(TCGv_i64, TCGv_i64, TCGv_i64, bool))
{
    REQUIRE_INSNS_FLAGS2(ctx, ISA310);
    REQUIRE_VECTOR(ctx);

    TCGv_i64 vra, vrb, vrt;
    int i;

    vra = tcg_temp_new_i64();
    vrb = tcg_temp_new_i64();
    vrt = tcg_temp_new_i64();

    for (i = 0; i < 2; i++) {
        get_avr64(vra, a->vra, i);
        get_avr64(vrb, a->vrb, i);
        get_avr64(vrt, a->vrt, i);

        func(vrt, vra, vrb, sign);

        set_avr64(a->vrt, vrt, i);
    }
    return true;
}

TRANS(VMULHSW, do_vx_mulh, true , do_vx_vmulhw_i64)
TRANS(VMULHSD, do_vx_mulh, true , do_vx_vmulhd_i64)
TRANS(VMULHUW, do_vx_mulh, false, do_vx_vmulhw_i64)
TRANS(VMULHUD, do_vx_mulh, false, do_vx_vmulhd_i64)

static void do_vavg(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b,
                    void (*gen_shr_vec)(unsigned, TCGv_vec, TCGv_vec, int64_t))
{
    TCGv_vec tmp = tcg_temp_new_vec_matching(t);
    tcg_gen_or_vec(vece, tmp, a, b);
    tcg_gen_and_vec(vece, tmp, tmp, tcg_constant_vec_matching(t, vece, 1));
    gen_shr_vec(vece, a, a, 1);
    gen_shr_vec(vece, b, b, 1);
    tcg_gen_add_vec(vece, t, a, b);
    tcg_gen_add_vec(vece, t, t, tmp);
}

QEMU_FLATTEN
static void gen_vavgu(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    do_vavg(vece, t, a, b, tcg_gen_shri_vec);
}

QEMU_FLATTEN
static void gen_vavgs(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    do_vavg(vece, t, a, b, tcg_gen_sari_vec);
}

static bool do_vx_vavg(DisasContext *ctx, arg_VX *a, int sign, int vece)
{
    static const TCGOpcode vecop_list_s[] = {
        INDEX_op_add_vec, INDEX_op_sari_vec, 0
    };
    static const TCGOpcode vecop_list_u[] = {
        INDEX_op_add_vec, INDEX_op_shri_vec, 0
    };

    static const GVecGen3 op[2][3] = {
        {
            {
                .fniv = gen_vavgu,
                .fno = gen_helper_VAVGUB,
                .opt_opc = vecop_list_u,
                .vece = MO_8
            },
            {
                .fniv = gen_vavgu,
                .fno = gen_helper_VAVGUH,
                .opt_opc = vecop_list_u,
                .vece = MO_16
            },
            {
                .fniv = gen_vavgu,
                .fno = gen_helper_VAVGUW,
                .opt_opc = vecop_list_u,
                .vece = MO_32
            },
        },
        {
            {
                .fniv = gen_vavgs,
                .fno = gen_helper_VAVGSB,
                .opt_opc = vecop_list_s,
                .vece = MO_8
            },
            {
                .fniv = gen_vavgs,
                .fno = gen_helper_VAVGSH,
                .opt_opc = vecop_list_s,
                .vece = MO_16
            },
            {
                .fniv = gen_vavgs,
                .fno = gen_helper_VAVGSW,
                .opt_opc = vecop_list_s,
                .vece = MO_32
            },
        },
    };

    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &op[sign][vece]);


    return true;
}


TRANS_FLAGS(ALTIVEC, VAVGSB, do_vx_vavg, 1, MO_8)
TRANS_FLAGS(ALTIVEC, VAVGSH, do_vx_vavg, 1, MO_16)
TRANS_FLAGS(ALTIVEC, VAVGSW, do_vx_vavg, 1, MO_32)
TRANS_FLAGS(ALTIVEC, VAVGUB, do_vx_vavg, 0, MO_8)
TRANS_FLAGS(ALTIVEC, VAVGUH, do_vx_vavg, 0, MO_16)
TRANS_FLAGS(ALTIVEC, VAVGUW, do_vx_vavg, 0, MO_32)

static void gen_vabsdu(unsigned vece, TCGv_vec t, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_umax_vec(vece, t, a, b);
    tcg_gen_umin_vec(vece, a, a, b);
    tcg_gen_sub_vec(vece, t, t, a);
}

static bool do_vabsdu(DisasContext *ctx, arg_VX *a, const int vece)
{
    static const TCGOpcode vecop_list[] = {
        INDEX_op_umax_vec, INDEX_op_umin_vec, INDEX_op_sub_vec, 0
    };

    static const GVecGen3 op[] = {
        {
            .fniv = gen_vabsdu,
            .fno = gen_helper_VABSDUB,
            .opt_opc = vecop_list,
            .vece = MO_8
        },
        {
            .fniv = gen_vabsdu,
            .fno = gen_helper_VABSDUH,
            .opt_opc = vecop_list,
            .vece = MO_16
        },
        {
            .fniv = gen_vabsdu,
            .fno = gen_helper_VABSDUW,
            .opt_opc = vecop_list,
            .vece = MO_32
        },
    };

    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &op[vece]);

    return true;
}

TRANS_FLAGS2(ISA300, VABSDUB, do_vabsdu, MO_8)
TRANS_FLAGS2(ISA300, VABSDUH, do_vabsdu, MO_16)
TRANS_FLAGS2(ISA300, VABSDUW, do_vabsdu, MO_32)

static bool do_vdiv_vmod(DisasContext *ctx, arg_VX *a, const int vece,
                         void (*func_32)(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b),
                         void (*func_64)(TCGv_i64 t, TCGv_i64 a, TCGv_i64 b))
{
    const GVecGen3 op = {
        .fni4 = func_32,
        .fni8 = func_64,
        .vece = vece
    };

    REQUIRE_VECTOR(ctx);

    tcg_gen_gvec_3(avr_full_offset(a->vrt), avr_full_offset(a->vra),
                   avr_full_offset(a->vrb), 16, 16, &op);

    return true;
}

#define DIVU32(NAME, DIV)                                               \
static void NAME(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)                    \
{                                                                       \
    TCGv_i32 zero = tcg_constant_i32(0);                                \
    TCGv_i32 one = tcg_constant_i32(1);                                 \
    tcg_gen_movcond_i32(TCG_COND_EQ, b, b, zero, one, b);               \
    DIV(t, a, b);                                                       \
}

#define DIVS32(NAME, DIV)                                               \
static void NAME(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)                    \
{                                                                       \
    TCGv_i32 t0 = tcg_temp_new_i32();                                   \
    TCGv_i32 t1 = tcg_temp_new_i32();                                   \
    tcg_gen_setcondi_i32(TCG_COND_EQ, t0, a, INT32_MIN);                \
    tcg_gen_setcondi_i32(TCG_COND_EQ, t1, b, -1);                       \
    tcg_gen_and_i32(t0, t0, t1);                                        \
    tcg_gen_setcondi_i32(TCG_COND_EQ, t1, b, 0);                        \
    tcg_gen_or_i32(t0, t0, t1);                                         \
    tcg_gen_movi_i32(t1, 0);                                            \
    tcg_gen_movcond_i32(TCG_COND_NE, b, t0, t1, t0, b);                 \
    DIV(t, a, b);                                                       \
}

#define DIVU64(NAME, DIV)                                               \
static void NAME(TCGv_i64 t, TCGv_i64 a, TCGv_i64 b)                    \
{                                                                       \
    TCGv_i64 zero = tcg_constant_i64(0);                                \
    TCGv_i64 one = tcg_constant_i64(1);                                 \
    tcg_gen_movcond_i64(TCG_COND_EQ, b, b, zero, one, b);               \
    DIV(t, a, b);                                                       \
}

#define DIVS64(NAME, DIV)                                               \
static void NAME(TCGv_i64 t, TCGv_i64 a, TCGv_i64 b)                    \
{                                                                       \
    TCGv_i64 t0 = tcg_temp_new_i64();                                   \
    TCGv_i64 t1 = tcg_temp_new_i64();                                   \
    tcg_gen_setcondi_i64(TCG_COND_EQ, t0, a, INT64_MIN);                \
    tcg_gen_setcondi_i64(TCG_COND_EQ, t1, b, -1);                       \
    tcg_gen_and_i64(t0, t0, t1);                                        \
    tcg_gen_setcondi_i64(TCG_COND_EQ, t1, b, 0);                        \
    tcg_gen_or_i64(t0, t0, t1);                                         \
    tcg_gen_movi_i64(t1, 0);                                            \
    tcg_gen_movcond_i64(TCG_COND_NE, b, t0, t1, t0, b);                 \
    DIV(t, a, b);                                                       \
}

DIVS32(do_divsw, tcg_gen_div_i32)
DIVU32(do_divuw, tcg_gen_divu_i32)
DIVS64(do_divsd, tcg_gen_div_i64)
DIVU64(do_divud, tcg_gen_divu_i64)

TRANS_FLAGS2(ISA310, VDIVSW, do_vdiv_vmod, MO_32, do_divsw, NULL)
TRANS_FLAGS2(ISA310, VDIVUW, do_vdiv_vmod, MO_32, do_divuw, NULL)
TRANS_FLAGS2(ISA310, VDIVSD, do_vdiv_vmod, MO_64, NULL, do_divsd)
TRANS_FLAGS2(ISA310, VDIVUD, do_vdiv_vmod, MO_64, NULL, do_divud)
TRANS_FLAGS2(ISA310, VDIVSQ, do_vx_helper, gen_helper_VDIVSQ)
TRANS_FLAGS2(ISA310, VDIVUQ, do_vx_helper, gen_helper_VDIVUQ)

static void do_dives_i32(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i64 val1, val2;

    val1 = tcg_temp_new_i64();
    val2 = tcg_temp_new_i64();

    tcg_gen_ext_i32_i64(val1, a);
    tcg_gen_ext_i32_i64(val2, b);

    /* (a << 32)/b */
    tcg_gen_shli_i64(val1, val1, 32);
    tcg_gen_div_i64(val1, val1, val2);

    /* if quotient doesn't fit in 32 bits the result is undefined */
    tcg_gen_extrl_i64_i32(t, val1);
}

static void do_diveu_i32(TCGv_i32 t, TCGv_i32 a, TCGv_i32 b)
{
    TCGv_i64 val1, val2;

    val1 = tcg_temp_new_i64();
    val2 = tcg_temp_new_i64();

    tcg_gen_extu_i32_i64(val1, a);
    tcg_gen_extu_i32_i64(val2, b);

    /* (a << 32)/b */
    tcg_gen_shli_i64(val1, val1, 32);
    tcg_gen_divu_i64(val1, val1, val2);

    /* if quotient doesn't fit in 32 bits the result is undefined */
    tcg_gen_extrl_i64_i32(t, val1);
}

DIVS32(do_divesw, do_dives_i32)
DIVU32(do_diveuw, do_diveu_i32)

DIVS32(do_modsw, tcg_gen_rem_i32)
DIVU32(do_moduw, tcg_gen_remu_i32)
DIVS64(do_modsd, tcg_gen_rem_i64)
DIVU64(do_modud, tcg_gen_remu_i64)

TRANS_FLAGS2(ISA310, VDIVESW, do_vdiv_vmod, MO_32, do_divesw, NULL)
TRANS_FLAGS2(ISA310, VDIVEUW, do_vdiv_vmod, MO_32, do_diveuw, NULL)
TRANS_FLAGS2(ISA310, VDIVESD, do_vx_helper, gen_helper_VDIVESD)
TRANS_FLAGS2(ISA310, VDIVEUD, do_vx_helper, gen_helper_VDIVEUD)
TRANS_FLAGS2(ISA310, VDIVESQ, do_vx_helper, gen_helper_VDIVESQ)
TRANS_FLAGS2(ISA310, VDIVEUQ, do_vx_helper, gen_helper_VDIVEUQ)

TRANS_FLAGS2(ISA310, VMODSW, do_vdiv_vmod, MO_32, do_modsw , NULL)
TRANS_FLAGS2(ISA310, VMODUW, do_vdiv_vmod, MO_32, do_moduw, NULL)
TRANS_FLAGS2(ISA310, VMODSD, do_vdiv_vmod, MO_64, NULL, do_modsd)
TRANS_FLAGS2(ISA310, VMODUD, do_vdiv_vmod, MO_64, NULL, do_modud)
TRANS_FLAGS2(ISA310, VMODSQ, do_vx_helper, gen_helper_VMODSQ)
TRANS_FLAGS2(ISA310, VMODUQ, do_vx_helper, gen_helper_VMODUQ)

#undef DIVS32
#undef DIVU32
#undef DIVS64
#undef DIVU64

#undef GEN_VXFORM
#undef GEN_VXFORM_207
#undef GEN_VXFORM_DUAL
#undef GEN_VXRFORM_DUAL
#undef GEN_VXRFORM1
#undef GEN_VXRFORM
#undef GEN_VXFORM_VSPLTI
#undef GEN_VXFORM_NOA
#undef GEN_VXFORM_UIMM
#undef GEN_VAFORM_PAIRED
