/*
 * QEMU ATI Mach64 GT (Rage Pro) emulation -- register-name table for
 * tracing, generated from ati_mach64_regs.h.
 *
 * Split out following the layout of the upstream ati-vga device
 * (ati_dbg.c).
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/display/ati_mach64_int.h"
#include "ati_mach64_regs.h"

const char *ati_mach64_reg_name(uint32_t base)
{
    switch (base) {
    case ATI_CRTC_H_TOTAL_DISP:       return "CRTC_H_TOTAL_DISP";
    case ATI_BKGD_SRC_PATTERN:        return "BKGD_SRC_PATTERN";
    case ATI_CRTC_V_TOTAL_DISP:       return "CRTC_V_TOTAL_DISP";
    case ATI_MIX_AND:                 return "MIX_AND";
    case ATI_SRC_LINE_X_L2R:          return "SRC_LINE_X_L2R";
    case ATI_CRTC_OFF_PITCH:          return "CRTC_OFF_PITCH";
    case ATI_CRTC_INT_CNTL:           return "CRTC_INT_CNTL";
    case ATI_CRTC_GEN_CNTL:           return "CRTC_GEN_CNTL";
    case ATI_CUR_CLR0:                return "CUR_CLR0";
    case ATI_CUR_CLR1:                return "CUR_CLR1";
    case ATI_CUR_OFFSET:              return "CUR_OFFSET";
    case ATI_CUR_HORZ_VERT_POSN:      return "CUR_HORZ_VERT_POSN";
    case ATI_CUR_HORZ_VERT_OFF:       return "CUR_HORZ_VERT_OFF";
    case ATI_GP_IO:                   return "GP_IO";
    case ATI_CLOCK_CNTL:              return "CLOCK_CNTL";
    case ATI_BUS_CNTL:                return "BUS_CNTL";
    case ATI_EXT_MEM_CNTL:            return "EXT_MEM_CNTL";
    case ATI_MEM_CNTL:                return "MEM_CNTL";
    case ATI_DAC_REGS:                return "DAC_REGS";
    case ATI_DAC_CNTL:                return "DAC_CNTL";
    case ATI_GEN_TEST_CNTL:           return "GEN_TEST_CNTL";
    case ATI_CONFIG_CNTL:             return "CONFIG_CNTL";
    case ATI_CONFIG_CHIP_ID:          return "CONFIG_CHIP_ID";
    case ATI_CONFIG_STAT0:            return "CONFIG_STAT0";
    case ATI_DST_OFF_PITCH:           return "DST_OFF_PITCH";
    case ATI_DST_X:                   return "DST_X";
    case ATI_DST_Y:                   return "DST_Y";
    case ATI_DST_Y_X:                 return "DST_Y_X";
    case ATI_DST_WIDTH:               return "DST_WIDTH";
    case ATI_DST_HEIGHT:              return "DST_HEIGHT";
    case ATI_DST_HEIGHT_WIDTH:        return "DST_HEIGHT_WIDTH";
    case ATI_DST_X_WIDTH:             return "DST_X_WIDTH";
    case ATI_DST_BRES_LNTH:           return "DST_BRES_LNTH";
    case ATI_DST_BRES_ERR:            return "DST_BRES_ERR";
    case ATI_DST_BRES_INC:            return "DST_BRES_INC";
    case ATI_DST_BRES_DEC:            return "DST_BRES_DEC";
    case ATI_DST_CNTL:                return "DST_CNTL";
    case ATI_Z_OFF_PITCH:             return "Z_OFF_PITCH";
    case ATI_Z_CNTL:                  return "Z_CNTL";
    case ATI_SRC_OFF_PITCH:           return "SRC_OFF_PITCH";
    case ATI_SRC_X:                   return "SRC_X";
    case ATI_SRC_Y:                   return "SRC_Y";
    case ATI_SRC_Y_X:                 return "SRC_Y_X";
    case ATI_SRC_WIDTH1:              return "SRC_WIDTH1";
    case ATI_SRC_HEIGHT1:             return "SRC_HEIGHT1";
    case ATI_SRC_HEIGHT1_WIDTH1:      return "SRC_HEIGHT1_WIDTH1";
    case ATI_SRC_X_START:             return "SRC_X_START";
    case ATI_SRC_Y_START:             return "SRC_Y_START";
    case ATI_SRC_Y_X_START:           return "SRC_Y_X_START";
    case ATI_SRC_CNTL:                return "SRC_CNTL";
    case ATI_SCALE_3D_CNTL:           return "SCALE_3D_CNTL";
    case ATI_FRGD_SRC_HOST:           return "FRGD_SRC_HOST";
    case ATI_HOST_DATAF:              return "HOST_DATAF";
    case ATI_HOST_CNTL:               return "HOST_CNTL";
    case ATI_BM_HOSTDATA:             return "BM_HOSTDATA";
    case ATI_BM_ADDR_DATA:            return "BM_ADDR_DATA";
    case ATI_BM_GUI_TABLE_CMD:        return "BM_GUI_TABLE_CMD";
    case ATI_PAT_REG0:                return "PAT_REG0";
    case ATI_PAT_REG1:                return "PAT_REG1";
    case ATI_PAT_CNTL:                return "PAT_CNTL";
    case ATI_SC_LEFT:                 return "SC_LEFT";
    case ATI_SC_RIGHT:                return "SC_RIGHT";
    case ATI_SC_LEFT_RIGHT:           return "SC_LEFT_RIGHT";
    case ATI_SC_TOP:                  return "SC_TOP";
    case ATI_SC_BOTTOM:               return "SC_BOTTOM";
    case ATI_SC_TOP_BOTTOM:           return "SC_TOP_BOTTOM";
    case ATI_DP_BKGD_CLR:             return "DP_BKGD_CLR";
    case ATI_DP_FRGD_CLR:             return "DP_FRGD_CLR";
    case ATI_DP_WRITE_MSK:            return "DP_WRITE_MSK";
    case ATI_DP_PIX_WIDTH:            return "DP_PIX_WIDTH";
    case ATI_DP_MIX:                  return "DP_MIX";
    case ATI_DP_SRC:                  return "DP_SRC";
    case ATI_DST_X_Y:                 return "DST_X_Y";
    case ATI_DST_WIDTH_HEIGHT:        return "DST_WIDTH_HEIGHT";
    case ATI_USR_DST_PITCH:           return "USR_DST_PITCH";
    case ATI_DP_SET_GUI_ENGINE2:      return "DP_SET_GUI_ENGINE2";
    case ATI_DP_SET_GUI_ENGINE:       return "DP_SET_GUI_ENGINE";
    case ATI_CLR_CMP_CLR:             return "CLR_CMP_CLR";
    case ATI_CLR_CMP_MSK:             return "CLR_CMP_MSK";
    case ATI_CLR_CMP_CNTL:            return "CLR_CMP_CNTL";
    case ATI_FIFO_STAT:               return "FIFO_STAT";
    case ATI_GUI_TRAJ_CNTL:           return "GUI_TRAJ_CNTL";
    case ATI_GUI_STAT:                return "GUI_STAT";
    case ATI_FRGD_SRC_PATTERN:        return "FRGD_SRC_PATTERN";
    case ATI_BM_FRAME_BUF_OFFSET:     return "BM_FRAME_BUF_OFFSET";
    case ATI_BM_SYSTEM_MEM_ADDR:      return "BM_SYSTEM_MEM_ADDR";
    case ATI_BM_COMMAND:              return "BM_COMMAND";
    case ATI_BM_STATUS:               return "BM_STATUS";
    case ATI_BM_GUI_TABLE:            return "BM_GUI_TABLE";
    case ATI_BM_SYSTEM_TABLE:         return "BM_SYSTEM_TABLE";
    case ATI_VERTEX_1_S:              return "VERTEX_1_S";
    case ATI_VERTEX_1_T:              return "VERTEX_1_T";
    case ATI_VERTEX_1_W:              return "VERTEX_1_W";
    case ATI_VERTEX_1_SPEC_ARGB:      return "VERTEX_1_SPEC_ARGB";
    case ATI_VERTEX_1_Z:              return "VERTEX_1_Z";
    case ATI_VERTEX_1_ARGB:           return "VERTEX_1_ARGB";
    case ATI_VERTEX_1_X_Y:            return "VERTEX_1_X_Y";
    case ATI_ONE_OVER_AREA:           return "ONE_OVER_AREA";
    case ATI_VERTEX_2_S:              return "VERTEX_2_S";
    case ATI_VERTEX_2_T:              return "VERTEX_2_T";
    case ATI_VERTEX_2_W:              return "VERTEX_2_W";
    case ATI_VERTEX_2_SPEC_ARGB:      return "VERTEX_2_SPEC_ARGB";
    case ATI_VERTEX_2_Z:              return "VERTEX_2_Z";
    case ATI_VERTEX_2_ARGB:           return "VERTEX_2_ARGB";
    case ATI_VERTEX_2_X_Y:            return "VERTEX_2_X_Y";
    case ATI_VERTEX_3_S:              return "VERTEX_3_S";
    case ATI_VERTEX_3_T:              return "VERTEX_3_T";
    case ATI_VERTEX_3_W:              return "VERTEX_3_W";
    case ATI_VERTEX_3_SPEC_ARGB:      return "VERTEX_3_SPEC_ARGB";
    case ATI_VERTEX_3_Z:              return "VERTEX_3_Z";
    case ATI_VERTEX_3_ARGB:           return "VERTEX_3_ARGB";
    case ATI_VERTEX_3_X_Y:            return "VERTEX_3_X_Y";
    case ATI_FRGD_SRC_MASK:           return "FRGD_SRC_MASK";
    case ATI_SETUP_CNTL:              return "SETUP_CNTL";
    default:                          return "?";
    }
}

/*
 * Silent-register audit: the register-level half of the coverage
 * question this file's name table serves.
 *
 * A trace tells you a register was written; it does not tell you
 * whether anything in the model then read it. A register this device
 * stores and never consults is invisible from inside every implemented
 * path -- no unimplemented-operation warning fires, and the screen is
 * simply wrong somewhere else. That silent class is where the R350's
 * RBBM_GUICNTL/GUI_HOST_SWAP_CNTL root causes lived, and this device
 * has the same shape of blind spot.
 *
 * The generated bitmap in ati_mach64_audit.h says which registers some
 * model code consumes; every write to any other register is tallied
 * here, one counter per register, and reported by the `silent-regs`
 * property. Regenerate the bitmap (doc/radeon9800/regaudit2.py
 * mach64-g3 --emit-table) whenever the model learns to read a new
 * register, or silent-regs will keep reporting it.
 */
#include "ati_mach64_audit.h"

QEMU_BUILD_BUG_ON(MACH64_REG_AUDIT_LIMIT / 4 != MACH64_SILENT_REG_WORDS);

void ati_mach64_audit_reg_write(ATIMach64State *s, uint32_t base)
{
    unsigned bit;

    if (base >= MACH64_REG_AUDIT_LIMIT) {
        return;
    }
    bit = base >> 2;
    if (ati_mach64_reg_consumed[bit / 32] & (1u << (bit % 32))) {
        return;
    }
    if (s->silent_reg_count[bit] != UINT32_MAX) {
        s->silent_reg_count[bit]++;
    }
}
