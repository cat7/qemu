/*
 * ATI R300/R350 programmable vertex shader (PVS) -- instruction encoding
 * and interpreter.
 *
 * Deliberately free of any device state: everything it needs arrives as
 * plain arrays, so the same code that runs inside the model can be driven
 * from a host test harness against programs lifted out of a capture.
 *
 * Field names and opcode semantics are transcribed from the R5xx
 * Acceleration guide (chapter 7.5, "Vertex Shader"), whose PVS description
 * covers R300 as well; the register addresses are R3xx.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ATI_R350_PVS_H
#define ATI_R350_PVS_H

/* program RAM: 256 instruction slots of four dwords, 256 constant vectors */
#define R300_PVS_CODE_SLOTS           256
#define R300_PVS_CONST_SLOTS          256

#define R300_PVS_IN_REGS              16
#define R300_PVS_OUT_REGS             16
#define R300_PVS_TMP_REGS             32
#define R300_PVS_ATMP_REGS            4

/* word 0: opcode and destination operand */
#define R300_PVS_DST_OPCODE_MASK      0x3f
#define R300_PVS_DST_MATH_INST        (1u << 6)
#define R300_PVS_DST_MACRO_INST       (1u << 7)
#define R300_PVS_DST_REG_TYPE_SHIFT   8
#define R300_PVS_DST_REG_TYPE_MASK    0xf
#define R300_PVS_DST_ADDR_MODE_1      (1u << 12)
#define R300_PVS_DST_OFFSET_SHIFT     13
#define R300_PVS_DST_OFFSET_MASK      0x7f
#define R300_PVS_DST_WE_SHIFT         20      /* four write-enable bits */
#define R300_PVS_DST_WE_MASK          0xf
#define R300_PVS_DST_VE_SAT           (1u << 24)
#define R300_PVS_DST_ME_SAT           (1u << 25)
#define R300_PVS_DST_PRED_ENABLE      (1u << 26)
#define R300_PVS_DST_DUAL_MATH_OP     (1u << 28)
#define R300_PVS_DST_ADDR_MODE_0      (1u << 31)

/* words 1-3: source operands */
#define R300_PVS_SRC_REG_TYPE_MASK    0x3
#define R300_PVS_SRC_ABS_XYZW         (1u << 3)
#define R300_PVS_SRC_ADDR_MODE_0      (1u << 4)
#define R300_PVS_SRC_OFFSET_SHIFT     5
#define R300_PVS_SRC_OFFSET_MASK      0xff
#define R300_PVS_SRC_SWIZZLE_SHIFT    13      /* four 3-bit selectors */
#define R300_PVS_SRC_SWIZZLE_MASK     0x7
#define R300_PVS_SRC_MODIFIER_SHIFT   25      /* four per-channel negates */
#define R300_PVS_SRC_ADDR_MODE_1      (1u << 31)

/*
 * Word 3 when R300_PVS_DST_DUAL_MATH_OP is set: it stops being a third
 * source operand and describes a math-engine instruction issued alongside
 * the vector one, reading two swizzled components of a single vector and
 * writing one channel of the alternate temporary file.
 */
#define R300_PVS_DUAL_OPCODE_MSB      (1u << 2)
#define R300_PVS_DUAL_DST_OFF_SHIFT   19
#define R300_PVS_DUAL_DST_OFF_MASK    0x3
#define R300_PVS_DUAL_OPCODE_SHIFT    21
#define R300_PVS_DUAL_OPCODE_MASK     0xf
#define R300_PVS_DUAL_WE_SEL_SHIFT    27
#define R300_PVS_DUAL_WE_SEL_MASK     0x3

/* swizzle selectors */
#define R300_PVS_SRC_SELECT_FORCE_0   4
#define R300_PVS_SRC_SELECT_FORCE_1   5

/* destination register files */
#define R300_PVS_DST_REG_TEMPORARY    0
#define R300_PVS_DST_REG_A0           1
#define R300_PVS_DST_REG_OUT          2
#define R300_PVS_DST_REG_OUT_REPL_X   3
#define R300_PVS_DST_REG_ALT_TEMP     4
#define R300_PVS_DST_REG_INPUT        5

/* source register files */
#define R300_PVS_SRC_REG_TEMPORARY    0
#define R300_PVS_SRC_REG_INPUT        1
#define R300_PVS_SRC_REG_CONSTANT     2
#define R300_PVS_SRC_REG_ALT_TEMP     3

/* vector-engine opcodes */
#define R300_VE_NO_OP                 0
#define R300_VE_DOT_PRODUCT           1
#define R300_VE_MULTIPLY              2
#define R300_VE_ADD                   3
#define R300_VE_MULTIPLY_ADD          4
#define R300_VE_DISTANCE_VECTOR       5
#define R300_VE_FRACTION              6
#define R300_VE_MAXIMUM               7
#define R300_VE_MINIMUM               8
#define R300_VE_SET_GREATER_THAN_EQUAL 9
#define R300_VE_SET_LESS_THAN         10
#define R300_VE_MULTIPLYX2_ADD        11
#define R300_VE_MULTIPLY_CLAMP        12

/* math-engine opcodes (one scalar per source, always its w channel) */
#define R300_ME_NO_OP                 0
#define R300_ME_EXP_BASE2_DX          1
#define R300_ME_LOG_BASE2_DX          2
#define R300_ME_LIGHT_COEFF_DX        4
#define R300_ME_RECIP_DX              6
#define R300_ME_RECIP_FF              7
#define R300_ME_RECIP_SQRT_DX         8
#define R300_ME_RECIP_SQRT_FF         9
#define R300_ME_MULTIPLY              10
#define R300_ME_EXP_BASE2_FULL_DX     11
#define R300_ME_LOG_BASE2_FULL_DX     12

/*
 * A program as the control registers describe it, plus what a static
 * scan of its instructions found. Holds no copy of the RAM: `code` and
 * `cnst` point at the caller's, which is what the guest has uploaded.
 */
typedef struct R300PvsProgram {
    const uint32_t *code;       /* four dwords per instruction slot */
    const uint32_t *cnst;       /* four dwords per constant vector */
    unsigned code_slots, const_slots;
    unsigned first, last;       /* inclusive instruction range */
    unsigned cbase;             /* PVS_CONST_BASE_OFFSET, in vectors */
    unsigned cmax;              /* PVS_MAX_CONST_ADDR */
    bool bounded;               /* honour cmax (the register was written) */
    bool valid;                 /* the whole range has really been uploaded */
    /*
     * The program computes out[0] as the four dot products of input 0
     * against constants cbase+0..3 and nothing else that this model
     * consumes -- i.e. it is the 4x4 matrix the fixed path already
     * applies, and running it per vertex would only spend time.
     */
    bool plain_matrix;
    /* out[n] is a straight copy of in[out_src[n]], or -1 if it is not */
    int8_t out_src[R300_PVS_OUT_REGS];
    uint32_t out_mask;          /* outputs the program writes at all */
} R300PvsProgram;

/* opcodes met that this interpreter does not implement */
typedef struct R300PvsGaps {
    uint8_t vec_op, math_op, dst_file;
    bool has_vec_op, has_math_op, has_dst_file;
} R300PvsGaps;

typedef struct R300PvsRegs {
    float in[R300_PVS_IN_REGS][4];
    float out[R300_PVS_OUT_REGS][4];
    float tmp[R300_PVS_TMP_REGS][4];
    float atmp[R300_PVS_ATMP_REGS][4];
    uint32_t out_written;       /* bit per output register actually written */
} R300PvsRegs;

/*
 * Where the vertex stage's outputs land. VAP_OUTPUT_VTX_FMT_0 bit 0 says
 * the position is emitted and bits 1-4 which of the four colours are, and
 * the outputs are packed in that order, so the first colour follows the
 * position and the texture coordinates follow the colours.
 */
void r300_pvs_out_layout(uint32_t fmt0, unsigned *first_color,
                         unsigned *ncolor, unsigned *first_texcoord);

void r300_pvs_analyse(R300PvsProgram *p, const uint32_t *code,
                      const uint32_t *slot_valid, unsigned code_slots,
                      const uint32_t *cnst, unsigned const_slots,
                      uint32_t code_cntl, uint32_t const_cntl,
                      unsigned first_texcoord);

/*
 * Run the program over the inputs already in `r`, leaving the outputs
 * there. `gaps` accumulates opcodes met and not implemented; pass NULL
 * to ignore them.
 */
void r300_pvs_run(const R300PvsProgram *p, R300PvsRegs *r, R300PvsGaps *gaps);

/* one constant vector as the program addresses it, cmax applied */
void r300_pvs_const(const R300PvsProgram *p, unsigned off, float v[4]);

#endif /* ATI_R350_PVS_H */
