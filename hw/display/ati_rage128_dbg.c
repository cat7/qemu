/*
 * QEMU ATI Rage 128 Pro emulation -- register-name table for tracing.
 *
 * Split out of ati_rage128.c following the layout of the upstream
 * ati-vga device (ati_dbg.c).
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "ati_rage128_int.h"
#include "ati_rage128_regs.h"

const char *ati_rage128_reg_name(uint32_t base)
{
    switch (base) {
    case R128_MM_INDEX:              return "MM_INDEX";
    case R128_MM_DATA:               return "MM_DATA";
    case R128_CLOCK_CNTL_INDEX:      return "CLOCK_CNTL_INDEX";
    case R128_CLOCK_CNTL_DATA:       return "CLOCK_CNTL_DATA";
    case R128_BIOS_0_SCRATCH:        return "BIOS_0_SCRATCH";
    case R128_BIOS_1_SCRATCH:        return "BIOS_1_SCRATCH";
    case R128_BIOS_2_SCRATCH:        return "BIOS_2_SCRATCH";
    case R128_BIOS_3_SCRATCH:        return "BIOS_3_SCRATCH";
    case R128_BUS_CNTL:              return "BUS_CNTL";
    case R128_BUS_CNTL1:             return "BUS_CNTL1";
    case R128_GEN_INT_CNTL:          return "GEN_INT_CNTL";
    case R128_GEN_INT_STATUS:        return "GEN_INT_STATUS";
    case R128_CRTC_GEN_CNTL:         return "CRTC_GEN_CNTL";
    case R128_CRTC_EXT_CNTL:         return "CRTC_EXT_CNTL";
    case R128_DAC_CNTL:              return "DAC_CNTL";
    case R128_CRTC_STATUS:           return "CRTC_STATUS";
    case R128_GPIO_MONID:            return "GPIO_MONID";
    case R128_SEPROM_CNTL:           return "SEPROM_CNTL";
    case R128_I2C_CNTL_0:            return "I2C_CNTL_0";
    case R128_I2C_CNTL_1:            return "I2C_CNTL_1";
    case R128_I2C_DATA:              return "I2C_DATA";
    case R128_AMCGPIO_MASK_MIR:      return "AMCGPIO_MASK_MIR";
    case R128_AMCGPIO_A_MIR:         return "AMCGPIO_A_MIR";
    case R128_AMCGPIO_Y_MIR:         return "AMCGPIO_Y_MIR";
    case R128_AMCGPIO_EN_MIR:        return "AMCGPIO_EN_MIR";
    case R128_PALETTE_INDEX:         return "PALETTE_INDEX";
    case R128_PALETTE_DATA:          return "PALETTE_DATA";
    case R128_CONFIG_CNTL:           return "CONFIG_CNTL";
    case R128_CONFIG_XSTRAP:         return "CONFIG_XSTRAP";
    case R128_CONFIG_BONDS:          return "CONFIG_BONDS";
    case R128_GEN_RESET_CNTL:        return "GEN_RESET_CNTL";
    case R128_GEN_STATUS:            return "GEN_STATUS";
    case R128_CONFIG_MEMSIZE:        return "CONFIG_MEMSIZE";
    case R128_CONFIG_APER_0_BASE:    return "CONFIG_APER_0_BASE";
    case R128_CONFIG_APER_1_BASE:    return "CONFIG_APER_1_BASE";
    case R128_CONFIG_APER_SIZE:      return "CONFIG_APER_SIZE";
    case R128_CONFIG_REG_1_BASE:     return "CONFIG_REG_1_BASE";
    case R128_CONFIG_REG_APER_SIZE:  return "CONFIG_REG_APER_SIZE";
    case R128_HOST_PATH_CNTL:        return "HOST_PATH_CNTL";
    case R128_SW_SEMAPHORE:          return "SW_SEMAPHORE";
    case R128_MEM_CNTL:              return "MEM_CNTL";
    case R128_EXT_MEM_CNTL:          return "EXT_MEM_CNTL";
    case R128_MEM_ADDR_CONFIG:       return "MEM_ADDR_CONFIG";
    case R128_MEM_INTF_CNTL:         return "MEM_INTF_CNTL";
    case R128_MEM_STR_CNTL:          return "MEM_STR_CNTL";
    case R128_MEM_INIT_LAT_TIMER:    return "MEM_INIT_LAT_TIMER";
    case R128_MEM_SDRAM_MODE_REG:    return "MEM_SDRAM_MODE_REG";
    case R128_AGP_BASE:              return "AGP_BASE";
    case R128_AGP_CNTL:              return "AGP_CNTL";
    case R128_AGP_APER_OFFSET:       return "AGP_APER_OFFSET";
    case R128_PCI_GART_PAGE:         return "PCI_GART_PAGE";
    case R128_PC_NGUI_MODE:          return "PC_NGUI_MODE";
    case R128_PC_NGUI_CTLSTAT:       return "PC_NGUI_CTLSTAT";
    case R128_CRTC_H_TOTAL_DISP:     return "CRTC_H_TOTAL_DISP";
    case R128_CRTC_H_SYNC_STRT_WID:  return "CRTC_H_SYNC_STRT_WID";
    case R128_CRTC_V_TOTAL_DISP:     return "CRTC_V_TOTAL_DISP";
    case R128_CRTC_V_SYNC_STRT_WID:  return "CRTC_V_SYNC_STRT_WID";
    case R128_CRTC_VLINE_CRNT_VLINE: return "CRTC_VLINE_CRNT_VLINE";
    case R128_CRTC_CRNT_FRAME:       return "CRTC_CRNT_FRAME";
    case R128_CRTC_GUI_TRIG_VLINE:   return "CRTC_GUI_TRIG_VLINE";
    case R128_CRTC_OFFSET:           return "CRTC_OFFSET";
    case R128_CRTC_OFFSET_CNTL:      return "CRTC_OFFSET_CNTL";
    case R128_CRTC_PITCH:            return "CRTC_PITCH";
    case R128_OVR_CLR:               return "OVR_CLR";
    case R128_OVR_WID_LEFT_RIGHT:    return "OVR_WID_LEFT_RIGHT";
    case R128_OVR_WID_TOP_BOTTOM:    return "OVR_WID_TOP_BOTTOM";
    case R128_CUR_OFFSET:            return "CUR_OFFSET";
    case R128_CUR_HORZ_VERT_POSN:    return "CUR_HORZ_VERT_POSN";
    case R128_CUR_HORZ_VERT_OFF:     return "CUR_HORZ_VERT_OFF";
    case R128_CUR_CLR0:              return "CUR_CLR0";
    case R128_CUR_CLR1:              return "CUR_CLR1";
    case R128_DAC_EXT_CNTL:          return "DAC_EXT_CNTL";
    case R128_DDA_CONFIG:            return "DDA_CONFIG";
    case R128_DDA_ON_OFF:            return "DDA_ON_OFF";
    case R128_GUI_DEBUG0:            return "GUI_DEBUG0";
    case R128_WAIT_UNTIL:            return "WAIT_UNTIL";
    case R128_GUI_STAT:              return "GUI_STAT";
    case R128_GUI_SCRATCH_REG0:      return "GUI_SCRATCH_REG0";
    case R128_GUI_SCRATCH_REG1:      return "GUI_SCRATCH_REG1";
    case R128_BM_GUI_TABLE:          return "BM_GUI_TABLE";
    case R128_BM_CHUNK_0_VAL:        return "BM_CHUNK_0_VAL";
    case R128_PM4_BUFFER_OFFSET:     return "PM4_BUFFER_OFFSET";
    case R128_PM4_BUFFER_CNTL:       return "PM4_BUFFER_CNTL";
    case R128_PM4_BUFFER_WM_CNTL:    return "PM4_BUFFER_WM_CNTL";
    case R128_PM4_BUFFER_DL_RPTR_ADDR: return "PM4_BUFFER_DL_RPTR_ADDR";
    case R128_PM4_BUFFER_DL_RPTR:    return "PM4_BUFFER_DL_RPTR";
    case R128_PM4_BUFFER_DL_WPTR:    return "PM4_BUFFER_DL_WPTR";
    case R128_PM4_STAT:              return "PM4_STAT";
    case R128_PM4_IW_INDOFF:         return "PM4_IW_INDOFF";
    case R128_PM4_IW_INDSIZE:        return "PM4_IW_INDSIZE";
    case R128_PM4_MICROCODE_ADDR:    return "PM4_MICROCODE_ADDR";
    case R128_PM4_MICROCODE_RADDR:   return "PM4_MICROCODE_RADDR";
    case R128_PM4_MICROCODE_DATAH:   return "PM4_MICROCODE_DATAH";
    case R128_PM4_MICROCODE_DATAL:   return "PM4_MICROCODE_DATAL";
    case R128_PM4_BUFFER_ADDR:       return "PM4_BUFFER_ADDR";
    case R128_PM4_MICRO_CNTL:        return "PM4_MICRO_CNTL";
    case R128_PM4_FIFO_DATA_EVEN:    return "PM4_FIFO_DATA_EVEN";
    case R128_PM4_FIFO_DATA_ODD:     return "PM4_FIFO_DATA_ODD";
    default:                         return "?";
    }
}

