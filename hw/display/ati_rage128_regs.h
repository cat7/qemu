/*
 * QEMU ATI Rage 128 Pro emulation -- register definitions
 *
 * All offsets, field positions and reset defaults below are taken from
 * the official "RAGE 128 PRO Register Reference Guide" RRG-G04500-C
 * Rev 1.01 (ATI, January 2000) unless a comment says otherwise. The
 * register file is accessible identically through the BAR2 memory
 * aperture (MMR), the BAR1 I/O window (IOR, low 256 bytes + the
 * MM_INDEX/MM_DATA indirection for the rest) and the VGA index port
 * (IND, unmodeled). PCI configuration space is additionally mirrored
 * read-only at register offsets 0x0F00-0x0FFF.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_RAGE128_REGS_H
#define ATI_RAGE128_REGS_H

#define R128_MM_INDEX                0x0000
#define R128_MM_DATA                 0x0004
#define R128_CLOCK_CNTL_INDEX        0x0008
#define R128_CLOCK_CNTL_DATA         0x000c
#define R128_BIOS_0_SCRATCH          0x0010
#define R128_BIOS_1_SCRATCH          0x0014
#define R128_BIOS_2_SCRATCH          0x0018
#define R128_BIOS_3_SCRATCH          0x001c
#define R128_BUS_CNTL                0x0030
#define R128_BUS_CNTL1               0x0034
#define R128_MEM_VGA_WP_SEL          0x0038
#define R128_MEM_VGA_RP_SEL          0x003c
#define R128_GEN_INT_CNTL            0x0040
#define R128_GEN_INT_STATUS          0x0044
#define R128_CRTC_GEN_CNTL           0x0050
#define R128_CRTC_EXT_CNTL           0x0054
#define R128_DAC_CNTL                0x0058
#define R128_CRTC_STATUS             0x005c
#define R128_GPIO_MONID              0x0068
#define R128_SEPROM_CNTL             0x006c
/*
 * The hardware I2C engine (0x0090/0x0094/0x0098) is NOT documented in
 * RRG-G04500-C at all, but the OEM Mac FCode ROM's constant table
 * includes all three offsets and XFree86's r128_reg.h names them
 * I2C_CNTL_0/I2C_CNTL_1/I2C_DATA. Bit layout below is from XFree86 /
 * the abandoned SourceFiles/ATI reference, to be validated against
 * live FCode traces.
 */
#define R128_I2C_CNTL_0              0x0090
#define R128_I2C_CNTL_1              0x0094
#define R128_I2C_DATA                0x0098
#define R128_AMCGPIO_MASK_MIR        0x009c
#define R128_AMCGPIO_A_MIR           0x00a0
#define R128_AMCGPIO_Y_MIR           0x00a4
#define R128_AMCGPIO_EN_MIR          0x00a8
#define R128_PALETTE_INDEX           0x00b0
#define R128_PALETTE_DATA            0x00b4
#define R128_CONFIG_CNTL             0x00e0
#define R128_CONFIG_XSTRAP           0x00e4
#define R128_CONFIG_BONDS            0x00e8
#define R128_GEN_RESET_CNTL          0x00f0
#define R128_GEN_STATUS              0x00f4
#define R128_CONFIG_MEMSIZE          0x00f8
#define R128_CONFIG_APER_0_BASE      0x0100
#define R128_CONFIG_APER_1_BASE      0x0104
#define R128_CONFIG_APER_SIZE        0x0108
#define R128_CONFIG_REG_1_BASE       0x010c
#define R128_CONFIG_REG_APER_SIZE    0x0110
#define R128_CONFIG_MEMSIZE_EMBEDDED 0x0114
#define R128_TEST_DEBUG_CNTL         0x0120
#define R128_HOST_PATH_CNTL          0x0130
#define R128_SW_SEMAPHORE            0x013c
#define R128_MEM_CNTL                0x0140
#define R128_EXT_MEM_CNTL            0x0144
#define R128_MEM_ADDR_CONFIG         0x0148
#define R128_MEM_INTF_CNTL           0x014c
#define R128_MEM_STR_CNTL            0x0150
#define R128_MEM_INIT_LAT_TIMER      0x0154
#define R128_MEM_SDRAM_MODE_REG      0x0158
#define R128_AGP_BASE                0x0170
#define R128_AGP_CNTL                0x0174
#define R128_AGP_APER_OFFSET         0x0178
#define R128_PCI_GART_PAGE           0x017c
#define R128_PC_NGUI_MODE            0x0180
#define R128_PC_NGUI_CTLSTAT         0x0184
#define R128_PC_MISC_CTL             0x0188
#define R128_CRTC_H_TOTAL_DISP       0x0200
#define R128_CRTC_H_SYNC_STRT_WID    0x0204
#define R128_CRTC_V_TOTAL_DISP       0x0208
#define R128_CRTC_V_SYNC_STRT_WID    0x020c
#define R128_CRTC_VLINE_CRNT_VLINE   0x0210
#define R128_CRTC_CRNT_FRAME         0x0214
#define R128_CRTC_GUI_TRIG_VLINE     0x0218
#define R128_CRTC_OFFSET             0x0224
#define R128_CRTC_OFFSET_CNTL        0x0228
#define R128_CRTC_PITCH              0x022c
#define R128_OVR_CLR                 0x0230
#define R128_OVR_WID_LEFT_RIGHT      0x0234
#define R128_OVR_WID_TOP_BOTTOM      0x0238
#define R128_CUR_OFFSET              0x0260
#define R128_CUR_HORZ_VERT_POSN      0x0264
#define R128_CUR_HORZ_VERT_OFF       0x0268
#define R128_CUR_CLR0                0x026c
#define R128_CUR_CLR1                0x0270
#define R128_DAC_EXT_CNTL            0x0280
#define R128_DDA_CONFIG              0x02e0
#define R128_DDA_ON_OFF              0x02e4
#define R128_VGA_DDA_CONFIG          0x02e8
#define R128_VGA_DDA_ON_OFF          0x02ec
#define R128_GUI_DEBUG0              0x16a0
#define R128_WAIT_UNTIL              0x1720
#define R128_GUI_STAT                0x1740

/* PCI config space read-only mirror */
#define R128_CFG_MIRROR_BASE         0x0f00
#define R128_CFG_MIRROR_END          0x0fff

/* GEN_INT_CNTL / GEN_INT_STATUS (status bits ack by writing 1) */
#define R128_CRTC_VBLANK_INT         (1 << 0)
#define R128_CRTC_VLINE_INT          (1 << 1)
#define R128_CRTC_VSYNC_INT          (1 << 2)
#define R128_SNAPSHOT_INT            (1 << 3)
#define R128_FP_DETECT_INT           (1 << 10)
#define R128_BUSMASTER_EOL_INT       (1 << 16)
#define R128_I2C_INT                 (1 << 17)
#define R128_MPP_GP_INT              (1 << 18)
#define R128_GUI_IDLE_INT            (1 << 19)
#define R128_VIPH_INT                (1 << 24)
#define R128_GEN_INT_ACK_MASK        (R128_CRTC_VBLANK_INT | \
                                      R128_CRTC_VLINE_INT | \
                                      R128_CRTC_VSYNC_INT | \
                                      R128_SNAPSHOT_INT | \
                                      R128_FP_DETECT_INT | \
                                      R128_BUSMASTER_EOL_INT | \
                                      R128_I2C_INT | R128_MPP_GP_INT | \
                                      R128_GUI_IDLE_INT | R128_VIPH_INT)

/* CRTC_GEN_CNTL */
#define R128_CRTC_DBL_SCAN_EN        (1 << 0)
#define R128_CRTC_INTERLACE_EN       (1 << 1)
#define R128_CRTC_C_SYNC_EN          (1 << 4)
#define R128_CRTC_PIX_WIDTH_SHIFT    8
#define R128_CRTC_PIX_WIDTH_MASK     7
#define R128_PIX_WIDTH_4BPP          1
#define R128_PIX_WIDTH_8BPP          2
#define R128_PIX_WIDTH_15BPP         3
#define R128_PIX_WIDTH_16BPP         4
#define R128_PIX_WIDTH_24BPP         5
#define R128_PIX_WIDTH_32BPP         6
#define R128_CRTC_CUR_EN             (1 << 16)
#define R128_CRTC_EXT_DISP_EN        (1 << 24)
#define R128_CRTC_EN                 (1 << 25)
#define R128_CRTC_DISP_REQ_EN_B      (1 << 26)

/* CRTC_EXT_CNTL */
#define R128_VGA_ATI_LINEAR          (1 << 3)
#define R128_VGA_XCRT_CNT_EN         (1 << 6)
#define R128_CRTC_HSYNC_DIS          (1 << 8)
#define R128_CRTC_VSYNC_DIS          (1 << 9)
#define R128_CRTC_DISPLAY_DIS        (1 << 10)
#define R128_CRTC_SYNC_TRISTATE      (1 << 11)
#define R128_DFIFO_EXTSENSE          (1 << 21)  /* default 1 */

/* CRTC_STATUS */
#define R128_CRTC_VBLANK_CUR         (1 << 0)
#define R128_CRTC_VBLANK_SAVE        (1 << 1)  /* write 1 clears */
#define R128_CRTC_VLINE_SYNC         (1 << 2)
#define R128_CRTC_FRAME_ODD          (1 << 3)
#define R128_FIX_VSYNC_TIMING        (1u << 31) /* default 1 */

/* DAC_CNTL */
#define R128_DAC_RANGE_CNTL_MASK     3          /* default 2 (PS2 level) */
#define R128_DAC_BLANKING            (1 << 2)
#define R128_DAC_CMP_EN              (1 << 3)   /* default 1 */
#define R128_DAC_CMP_OUTPUT          (1 << 7)   /* RO: comparator/monitor sense */
#define R128_DAC_8BIT_EN             (1 << 8)
#define R128_DAC_MASK_SHIFT          24
#define R128_DAC_MASK_DEFAULT        0xffu

/* CRTC timing field extraction */
#define R128_CRTC_H_TOTAL_MASK       0x1ff      /* [8:0], chars */
#define R128_CRTC_H_DISP_SHIFT       16         /* [23:16], chars - 1 */
#define R128_CRTC_H_DISP_MASK        0xff
#define R128_CRTC_V_TOTAL_MASK       0x7ff      /* [10:0], lines */
#define R128_CRTC_V_DISP_SHIFT       16         /* [26:16], lines - 1 */
#define R128_CRTC_V_DISP_MASK        0x7ff
#define R128_CRTC_OFFSET_MASK        0x01fffff8 /* [24:0], bits 2:0 wired 0 */
#define R128_CRTC_OFFSET_LOCK        (1u << 31)
#define R128_CRTC_PITCH_MASK         0x3ff      /* [9:0], pixels * 8 */

/* CLOCK_CNTL_INDEX */
#define R128_PLL_ADDR_MASK           0x1f
#define R128_PLL_WR_EN               (1 << 7)
#define R128_PPLL_DIV_SEL_SHIFT      8
#define R128_PPLL_DIV_SEL_MASK       3

/* PLL register indices */
#define R128_PLL_CLK_PIN_CNTL        0x01
#define R128_PLL_PPLL_CNTL           0x02
#define R128_PLL_PPLL_REF_DIV        0x03
#define R128_PLL_PPLL_DIV_0          0x04
#define R128_PLL_PPLL_DIV_1          0x05
#define R128_PLL_PPLL_DIV_2          0x06
#define R128_PLL_PPLL_DIV_3          0x07
#define R128_PLL_VCLK_ECP_CNTL       0x08
#define R128_PLL_HTOTAL_CNTL         0x09
#define R128_PLL_X_MPLL_REF_FB_DIV   0x0a
#define R128_PLL_XPLL_CNTL           0x0b
#define R128_PLL_XDLL_CNTL           0x0c
#define R128_PLL_XCLK_CNTL           0x0d
#define R128_PLL_MPLL_CNTL           0x0e
#define R128_PLL_MCLK_CNTL           0x0f
/*
 * PPLL_REF_DIV and PPLL_DIV_0..3 all carry the atomic-update handshake
 * in bit 15: writing 1 (ATOMIC_UPDATE_W) requests the PLL update,
 * reading (ATOMIC_UPDATE_R) polls for completion -- real hardware
 * clears it when the new dividers have taken effect; we emulate an
 * instant PLL, so reads always see it clear.
 */
#define R128_PPLL_ATOMIC_UPDATE      (1 << 15)

/* CONFIG_CNTL */
#define R128_APER_0_ENDIAN_MASK      3          /* [1:0] */
#define R128_APER_1_ENDIAN_SHIFT     2          /* [3:2] */
#define R128_APER_ENDIAN_LE          0
#define R128_APER_ENDIAN_BE16        1
#define R128_APER_ENDIAN_BE32        2
#define R128_APER_REG_ENDIAN         (1 << 4)
#define R128_CFG_VGA_RAM_EN          (1 << 8)
#define R128_CFG_VGA_IO_DIS          (1 << 9)

/* CONFIG_XSTRAP (read-only strap reflections) */
#define R128_XSTRAP_ADDIN_CARD       (1 << 13)

/* GUI_STAT */
#define R128_GUI_FIFOCNT_MASK        0xfff      /* [11:0], default 0x40 free */
#define R128_GUI_ACTIVE              (1u << 31)

/* I2C_CNTL_0 (undocumented; XFree86 r128_reg.h layout) */
#define R128_I2C_DONE                (1 << 0)
#define R128_I2C_NACK                (1 << 1)
#define R128_I2C_HALT                (1 << 2)
#define R128_I2C_SOFT_RST            (1 << 5)
#define R128_I2C_DRIVE_EN            (1 << 6)
#define R128_I2C_DRIVE_SEL           (1 << 7)
#define R128_I2C_START               (1 << 8)
#define R128_I2C_STOP                (1 << 9)
#define R128_I2C_RECEIVE             (1 << 10)
#define R128_I2C_ABORT               (1 << 11)
#define R128_I2C_GO                  (1 << 12)
/* I2C_CNTL_1 */
#define R128_I2C_DATA_COUNT_SHIFT    0
#define R128_I2C_DATA_COUNT_MASK     0xff
#define R128_I2C_ADDR_SHIFT          8
#define R128_I2C_ADDR_MASK           0xff
#define R128_I2C_SEL                 (1 << 16)
#define R128_I2C_EN                  (1 << 17)
#define R128_I2C_TIME_LIMIT_SHIFT    24

#endif /* ATI_RAGE128_REGS_H */
