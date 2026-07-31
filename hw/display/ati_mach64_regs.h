/*
 * ATI Mach64 register offsets (byte offsets into the BAR2 MMIO block).
 *
 * Offsets and bitfield layouts cross-checked directly against the
 * publicly-documented Mach64 register map (word-index * 4 == byte
 * offset shown here).
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_MACH64_REGS_H
#define ATI_MACH64_REGS_H

/* CRTC block */
#define ATI_CRTC_H_TOTAL_DISP     0x000
#define ATI_CRTC_V_TOTAL_DISP     0x008
#define ATI_CRTC_INT_CNTL         0x018
#define ATI_CRTC_OFF_PITCH        0x014
#define ATI_CRTC_GEN_CNTL         0x01C

/* CRTC_INT_CNTL fields -- bit 2 (VBLANK_INT) is both the "interrupt
 * pending" status when read and "write 1 to acknowledge" when written,
 * the standard Mach64 convention. */
#define ATI_CRTC_VBLANK           (1u << 0)
#define ATI_CRTC_VBLANK_INT_EN    (1u << 1)
#define ATI_CRTC_VBLANK_INT       (1u << 2)
#define ATI_CRTC_VBLANK_INT_AK    (1u << 2)
#define ATI_CRTC_VLINE_INT_EN     (1u << 3)
#define ATI_CRTC_VLINE_INT        (1u << 4)
#define ATI_CRTC_VLINE_INT_AK     (1u << 4)
/* CRTC_GEN_CNTL bit 25: master CRTC enable (bit 6 is DISPLAY_DIS) */
#define ATI_CRTC_ENABLE           (1u << 25)

/* CRTC_OFF_PITCH fields */
#define ATI_CRTC_OFFSET_SHIFT     0
#define ATI_CRTC_OFFSET_MASK      0x000fffff
#define ATI_CRTC_PITCH_SHIFT      22
#define ATI_CRTC_PITCH_MASK       0x3ff

/* CRTC_GEN_CNTL fields */
#define ATI_CRTC_DISPLAY_DIS      (1u << 6)
#define ATI_CRTC_PIX_WIDTH_SHIFT  8
#define ATI_CRTC_PIX_WIDTH_MASK   0x7

/* CRTC_H_TOTAL_DISP / V_TOTAL_DISP fields */
#define ATI_CRTC_H_DISP_SHIFT     16
#define ATI_CRTC_H_DISP_MASK      0xff
#define ATI_CRTC_V_DISP_SHIFT     16
#define ATI_CRTC_V_DISP_MASK      0x7ff

/* Pixel-width encodings for CRTC_GEN_CNTL's PIX_WIDTH field */
#define ATI_PIX_FMT_4BPP          1
#define ATI_PIX_FMT_8BPP          2
#define ATI_PIX_FMT_RGB555        3
#define ATI_PIX_FMT_RGB565        4
#define ATI_PIX_FMT_RGB888        5
#define ATI_PIX_FMT_ARGB8888      6

/*
 * Hardware-cursor block. 64x64, 2 bits per pixel, image fetched from
 * VRAM at CUR_OFFSET*8; per-pixel codes: 0 = CUR_CLR0, 1 = CUR_CLR1,
 * 2 = transparent, 3 = complement of the underlying pixel. Colors sit
 * in bits 8-31 of the CLR registers. HORZ/VERT_OFF clip the image's
 * top-left when the cursor overlaps the screen edge. Enable is
 * GEN_TEST_CNTL bit 7 (matching DingusPPC's ATI_GEN_CUR_ENABLE).
 */
#define ATI_CUR_CLR0              0x060
#define ATI_CUR_CLR1              0x064
#define ATI_CUR_OFFSET            0x068
#define ATI_CUR_HORZ_VERT_POSN    0x06C
#define ATI_CUR_HORZ_VERT_OFF     0x070
#define ATI_CUR_POSN_MASK         0x7ff
#define ATI_CUR_OFF_MASK          0x3f
#define ATI_GEN_CUR_ENABLE        (1u << 7)   /* in GEN_TEST_CNTL */

/* Misc/config block */
#define ATI_GP_IO                 0x078
#define ATI_CLOCK_CNTL            0x090
#define ATI_BUS_CNTL              0x0A0
#define ATI_EXT_MEM_CNTL          0x0AC
#define ATI_MEM_CNTL              0x0B0

/* DAC block: one 32-bit-aligned word at 0xC0 packs four byte-wide
 * sub-registers (write-index, data, mask, read-index), matching real
 * Mach64 hardware's own packing -- NOT standard IBM-VGA DAC ordering. */
#define ATI_DAC_REGS              0x0C0
#define ATI_DAC_W_INDEX           0x0C0
#define ATI_DAC_DATA              0x0C1
#define ATI_DAC_MASK              0x0C2
#define ATI_DAC_R_INDEX           0x0C3
#define ATI_DAC_CNTL              0x0C4

#define ATI_GEN_TEST_CNTL         0x0D0
#define ATI_CONFIG_CNTL           0x0DC
#define ATI_CONFIG_CHIP_ID        0x0E0
#define ATI_CONFIG_STAT0          0x0E4

/* CLOCK_CNTL fields (internal-PLL addressing, matches "CT"-family
 * layout also used by GT/Pro for the low-speed internal PLL path) */
#define ATI_PLL_ADDR_SHIFT        10
#define ATI_PLL_ADDR_MASK         0x3f
#define ATI_PLL_DATA_SHIFT        16
#define ATI_PLL_DATA_MASK         0xff

/* CONFIG_CHIP_ID fields */
#define ATI_CFG_CHIP_TYPE_SHIFT   0
#define ATI_CFG_CHIP_MAJOR_SHIFT  24

/*
 * GUI (2D drawing) engine block -- byte offsets and field layouts per
 * Linux's include/video/mach64.h (derived from ATI's Mach64 register
 * reference) and drivers/video/fbdev/aty/mach64_accel.c usage:
 * DST_Y_X = (x << 16) | y, DST_HEIGHT_WIDTH = (width << 16) | height
 * (writing it triggers the operation), OFF_PITCH = offset/8 in bits
 * 0-19 (units of 8 bytes) and pitch/8 in bits 22-31 (units of 8
 * pixels).
 */
#define ATI_DST_OFF_PITCH         0x100
#define ATI_DST_X                 0x104
#define ATI_DST_Y                 0x108
#define ATI_DST_Y_X               0x10C
#define ATI_DST_WIDTH             0x110
#define ATI_DST_HEIGHT            0x114
#define ATI_DST_HEIGHT_WIDTH      0x118
#define ATI_DST_X_WIDTH           0x11C
#define ATI_DST_CNTL              0x130
#define ATI_SRC_OFF_PITCH         0x180
#define ATI_SRC_X                 0x184
#define ATI_SRC_Y                 0x188
#define ATI_SRC_Y_X               0x18C
#define ATI_SRC_WIDTH1            0x190
#define ATI_SRC_HEIGHT1           0x194
#define ATI_SRC_HEIGHT1_WIDTH1    0x198
#define ATI_SRC_CNTL              0x1B4
#define ATI_HOST_CNTL             0x240
#define ATI_PAT_REG0              0x280
#define ATI_PAT_REG1              0x284
#define ATI_PAT_CNTL              0x288
#define ATI_SC_LEFT               0x2A0
#define ATI_SC_RIGHT              0x2A4
#define ATI_SC_LEFT_RIGHT         0x2A8
#define ATI_SC_TOP                0x2AC
#define ATI_SC_BOTTOM             0x2B0
#define ATI_SC_TOP_BOTTOM         0x2B4
#define ATI_DP_BKGD_CLR           0x2C0
#define ATI_DP_FRGD_CLR           0x2C4
#define ATI_DP_WRITE_MSK          0x2C8
#define ATI_DP_PIX_WIDTH          0x2D0
#define ATI_DP_MIX                0x2D4
#define ATI_DP_SRC                0x2D8
#define ATI_CLR_CMP_CLR           0x300
#define ATI_CLR_CMP_MSK           0x304
#define ATI_CLR_CMP_CNTL          0x308
#define ATI_FIFO_STAT             0x310
#define ATI_GUI_TRAJ_CNTL         0x330
#define ATI_GUI_STAT              0x338
/* Rage Pro (GT) command-FIFO depth, reported free in GUI_STAT bits
 * 16-23 (matches DingusPPC's ATIRage cmd_fifo_size for the GT). */
#define ATI_MACH64_GUI_FIFO_SIZE  48

/* DST_CNTL fields */
#define ATI_DST_X_LEFT_TO_RIGHT   (1u << 0)
#define ATI_DST_Y_TOP_TO_BOTTOM   (1u << 1)
#define ATI_DST_LAST_PEL          (1u << 5)

/* DP_SRC fields (frgd source bits 8-10, bkgd 0-2, mono 16-18) */
#define ATI_FRGD_SRC_MASK         0x700
#define ATI_FRGD_SRC_BKGD_CLR     0x000
#define ATI_FRGD_SRC_FRGD_CLR     0x100
#define ATI_FRGD_SRC_HOST         0x200
#define ATI_FRGD_SRC_BLIT         0x300
#define ATI_FRGD_SRC_PATTERN      0x400
#define ATI_MONO_SRC_MASK         0x70000
#define ATI_MONO_SRC_ONE          0x00000

/* DP_MIX foreground mix (bits 16-20) and background mix (bits 0-4);
 * ROP-style codes. Foreground applies to mono 1-bits and to
 * foreground/blit sources; background applies to mono 0-bits. */
#define ATI_FRGD_MIX_SHIFT        16
#define ATI_FRGD_MIX_MASK         0x1f
#define ATI_BKGD_MIX_MASK         0x1f
#define ATI_MIX_NOT_DST           0x0   /* dst = ~dst (source ignored) */
#define ATI_MIX_D                 0x3   /* leave destination */
#define ATI_MIX_XOR               0x5   /* dst ^= src */
#define ATI_MIX_S                 0x7   /* copy source */

/* PAT_CNTL: enable of the 8x8 monochrome pattern (PAT_REG0 = rows 0-3,
 * PAT_REG1 = rows 4-7, 8 bits/row MSB-left; a set bit picks the
 * foreground colour/mix, a clear bit the background). */
#define ATI_PAT_MONO_8x8_ENABLE   0x01000000

/* DP_SRC foreground/mono source selectors (see ATI_FRGD_SRC_* /
 * ATI_MONO_SRC_* above); HOST means the CPU streams the pixels. */
#define ATI_FRGD_SRC_HOST         0x200
#define ATI_MONO_SRC_HOST         0x20000

/* DP_PIX_WIDTH destination pixel-width codes (bits 0-3), host source
 * pixel-width codes (bits 16-19), and byte-order (bit 24). */
#define ATI_PIX_WIDTH_DST_MASK    0xf
#define ATI_PIX_WIDTH_HOST_SHIFT  16
#define ATI_PIX_WIDTH_HOST_MASK   0xf
#define ATI_PIX_WIDTH_BYTE_ORDER  (1u << 24)   /* set = LSB-to-MSB */

/* HOST_CNTL bit 0: source rows are aligned to a whole host word. */
#define ATI_HOST_BYTE_ALIGN       0x1

/* Host-data register block (CPU streams pixel/mask words here). */
#define ATI_HOST_DATA0            0x200
#define ATI_HOST_DATAF            0x23C

/*
 * 3D setup/raster engine (Rage 3D/Rage Pro) -- block 0 control
 * registers plus the block 1 (byte offsets 0x400-0x7FF) vertex and
 * setup registers, per Linux's include/video/mach64.h and Mesa's
 * mach64 DRI driver. A triangle is drawn by loading the three
 * vertices' state and writing ONE_OVER_AREA (strip continuation
 * layouts alias further vertex loads followed by another
 * ONE_OVER_AREA per new triangle). Exact operand fixed-point formats
 * are to be confirmed against a live classic-Mac RAVE driver capture
 * before the rasterizer lands -- see ati_mach64_3d_trigger().
 */
#define ATI_Z_OFF_PITCH           0x148
#define ATI_Z_CNTL                0x14C
#define ATI_SCALE_3D_CNTL         0x1FC
#define ATI_VERTEX_1_S            0x640
#define ATI_VERTEX_1_T            0x644
#define ATI_VERTEX_1_W            0x648
#define ATI_VERTEX_1_SPEC_ARGB    0x64C
#define ATI_VERTEX_1_Z            0x650
#define ATI_VERTEX_1_ARGB         0x654
#define ATI_VERTEX_1_X_Y          0x658
#define ATI_ONE_OVER_AREA         0x65C
#define ATI_VERTEX_2_S            0x660
#define ATI_VERTEX_2_T            0x664
#define ATI_VERTEX_2_W            0x668
#define ATI_VERTEX_2_SPEC_ARGB    0x66C
#define ATI_VERTEX_2_Z            0x670
#define ATI_VERTEX_2_ARGB         0x674
#define ATI_VERTEX_2_X_Y          0x678
#define ATI_VERTEX_3_S            0x680
#define ATI_VERTEX_3_T            0x684
#define ATI_VERTEX_3_W            0x688
#define ATI_VERTEX_3_SPEC_ARGB    0x68C
#define ATI_VERTEX_3_Z            0x690
#define ATI_VERTEX_3_ARGB         0x694
#define ATI_VERTEX_3_X_Y          0x698
#define ATI_ONE_OVER_AREA_UC      0x700
#define ATI_SETUP_CNTL            0x704

#endif /* ATI_MACH64_REGS_H */
