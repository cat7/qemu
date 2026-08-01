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
/*
 * Bresenham line engine (ATI-264VT/3D RAGE Register Reference,
 * RRG-G02700, 4-43..4-46): writing DST_BRES_LNTH triggers a line draw
 * of LNTH pixels from (DST_X, DST_Y). ERR/INC/DEC are signed 18-bit;
 * per pixel: error < 0 takes an axial step (major axis only) and adds
 * INC, error >= 0 takes a diagonal step and adds DEC (which must be
 * negative). Setup for a line of raw deltas dx, dy:
 *   ERR = 2*min - max,  INC = 2*min,  DEC = 2*(min - max),
 *   LNTH = max + 1  (pixels drawn when DST_LAST_PEL is set).
 */
#define ATI_DST_BRES_LNTH         0x120
#define ATI_DST_BRES_ERR          0x124
#define ATI_DST_BRES_INC          0x128
#define ATI_DST_BRES_DEC          0x12C
/*
 * DST_BRES_LNTH write gating (RRG 4-46): bit 15 set = trapezoid draw
 * (3D RAGE), bit 31 set with bit 15 clear = load registers only.
 */
#define ATI_BRES_LNTH_MASK        0x7fff
#define ATI_BRES_DRAW_TRAP        (1u << 15)
#define ATI_BRES_LINE_DIS         (1u << 31)
#define ATI_DST_CNTL              0x130
#define ATI_SRC_OFF_PITCH         0x180
#define ATI_SRC_X                 0x184
#define ATI_SRC_Y                 0x188
#define ATI_SRC_Y_X               0x18C
#define ATI_SRC_WIDTH1            0x190
#define ATI_SRC_HEIGHT1           0x194
#define ATI_SRC_HEIGHT1_WIDTH1    0x198
#define ATI_SRC_X_START           0x19C
#define ATI_SRC_Y_START           0x1A0
#define ATI_SRC_Y_X_START         0x1A4
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
#define ATI_USR_DST_PITCH         0x2F0
/*
 * Rage Pro macro-context registers (3D RAGE LT PRO Register
 * Reference, LT3REGRE 5-53..5-57): one write programs a whole
 * DP_MIX/DP_SRC/pix-width/direction/pitch drawing context and zeroes
 * the position/compare state. Apple's Mac OS accelerated NDRV sets up
 * EVERY 2D operation through DP_SET_GUI_ENGINE2 (confirmed by a live
 * register trace of the 9.2.2 driver; it never touches the component
 * registers per-op), so ignoring these leaves the engine running on
 * stale state -- the root cause of the previously-garbled accelerated
 * desktop at 15 bpp.
 */
#define ATI_DP_SET_GUI_ENGINE2    0x2F8
#define ATI_DP_SET_GUI_ENGINE     0x2FC
#define ATI_CLR_CMP_CLR           0x300
#define ATI_CLR_CMP_MSK           0x304
#define ATI_CLR_CMP_CNTL          0x308
#define ATI_FIFO_STAT             0x310
#define ATI_GUI_TRAJ_CNTL         0x330
#define ATI_GUI_STAT              0x338

/*
 * CLR_CMP_CNTL (LT3REGRE 5-61): bits 0-2 select the compare function;
 * when the comparison is TRUE the destination pixel is left unchanged
 * (the color source is suppressed). Bits 24-25 select what is
 * compared against CLR_CMP_CLR (under CLR_CMP_MSK): the destination
 * pixel or the 2D (blit/host/fill) source color.
 */
#define ATI_CLR_CMP_FN_MASK       0x7
#define ATI_CLR_CMP_FN_FALSE      0
#define ATI_CLR_CMP_FN_TRUE       1
#define ATI_CLR_CMP_FN_NOT_EQUAL  4
#define ATI_CLR_CMP_FN_EQUAL      5
#define ATI_CLR_CMP_SRC_SHIFT     24
#define ATI_CLR_CMP_SRC_MASK      0x3
#define ATI_CLR_CMP_SRC_DST       0
#define ATI_CLR_CMP_SRC_2D        1
/* Rage Pro (GT) command-FIFO depth, reported free in GUI_STAT bits
 * 16-23 (matches DingusPPC's ATIRage cmd_fifo_size for the GT). */
#define ATI_MACH64_GUI_FIFO_SIZE  48

/* DST_CNTL fields (LT3REGRE 5-64 GUI_TRAJ_CNTL bit list, low half) */
#define ATI_DST_X_LEFT_TO_RIGHT   (1u << 0)
#define ATI_DST_Y_TOP_TO_BOTTOM   (1u << 1)
#define ATI_DST_Y_MAJOR           (1u << 2)   /* lines: major axis is Y */
#define ATI_DST_X_TILE            (1u << 3)
#define ATI_DST_Y_TILE            (1u << 4)
#define ATI_DST_LAST_PEL          (1u << 5)   /* lines: draw the last pixel */
#define ATI_DST_POLYGON_EN        (1u << 6)
#define ATI_DST_24_ROT_EN         (1u << 7)   /* packed 24bpp (8bpp mode) */
#define ATI_DST_24_ROT_SHIFT      8
#define ATI_DST_24_ROT_MASK       0x7
#define ATI_DST_BRES_SIGN         (1u << 11)  /* lines: err == 0 is negative */

/* DP_SRC fields (frgd source bits 8-10, bkgd 0-2, mono 16-18) */
#define ATI_FRGD_SRC_MASK         0x700
#define ATI_FRGD_SRC_BKGD_CLR     0x000
#define ATI_FRGD_SRC_FRGD_CLR     0x100
#define ATI_FRGD_SRC_HOST         0x200
#define ATI_FRGD_SRC_BLIT         0x300
#define ATI_FRGD_SRC_PATTERN      0x400
#define ATI_BKGD_SRC_MASK         0x7
#define ATI_BKGD_SRC_BKGD_CLR     0x0
#define ATI_BKGD_SRC_FRGD_CLR     0x1
#define ATI_BKGD_SRC_HOST         0x2
#define ATI_BKGD_SRC_BLIT         0x3
#define ATI_BKGD_SRC_PATTERN      0x4
#define ATI_MONO_SRC_MASK         0x70000
#define ATI_MONO_SRC_ONE          0x00000
#define ATI_MONO_SRC_PATTERN      0x10000
#define ATI_MONO_SRC_HOST         0x20000
#define ATI_MONO_SRC_BLIT         0x30000

/*
 * DP_MIX foreground mix (bits 16-20) and background mix (bits 0-4).
 * The monochrome channel picks per pixel: 1-bit = foreground source +
 * foreground mix, 0-bit = background source + background mix. Full
 * bitwise function table per LT3REGRE Table 5-10; codes 0x10-0x1F are
 * documented Reserved on the Rage Pro generation (the older
 * arithmetic mixes), so only 0x0-0xF exist here.
 */
#define ATI_FRGD_MIX_SHIFT        16
#define ATI_FRGD_MIX_MASK         0x1f
#define ATI_BKGD_MIX_MASK         0x1f
#define ATI_MIX_NOT_DST           0x0   /* dst = ~dst (source ignored) */
#define ATI_MIX_0                 0x1   /* all zero bits */
#define ATI_MIX_1                 0x2   /* all one bits */
#define ATI_MIX_D                 0x3   /* leave destination */
#define ATI_MIX_NOT_SRC           0x4
#define ATI_MIX_XOR               0x5
#define ATI_MIX_XNOR              0x6
#define ATI_MIX_S                 0x7   /* copy source */
#define ATI_MIX_NAND              0x8
#define ATI_MIX_NOT_SRC_OR_DST    0x9
#define ATI_MIX_SRC_OR_NOT_DST    0xa
#define ATI_MIX_OR                0xb
#define ATI_MIX_AND               0xc
#define ATI_MIX_SRC_AND_NOT_DST   0xd
#define ATI_MIX_NOT_SRC_AND_DST   0xe
#define ATI_MIX_NOR               0xf

/*
 * PAT_CNTL enables one fixed pattern type (mutually exclusive): the
 * 8x8 monochrome pattern, or the 8bpp-only 4x2 / 8x1 color patterns.
 * The documented component-register bit for mono is bit 0 (LT3REGRE
 * 5-38); Apple's NDRV instead writes 0x01000000, which is the mono
 * enable's position within the composite GUI_TRAJ_CNTL layout (bit
 * 24), so both are accepted.
 *
 * Bit-to-pixel mapping (mach64 Programmer's Guide, "Pattern
 * Consumption", PRG888GX0-01 2-36): row (DST_Y mod 8) 0-3 = bytes 0-3
 * (low byte first) of PAT_REG0, rows 4-7 the same in PAT_REG1;
 * within each row byte the leftmost pixel is the MSB when
 * DP_BYTE_PIX_ORDER = 0, the LSB when 1.
 */
#define ATI_PAT_MONO_EN           0x00000001
#define ATI_PAT_CLR_4x2_EN        0x00000002
#define ATI_PAT_CLR_8x1_EN        0x00000004
#define ATI_PAT_MONO_EN_TRAJ      0x01000000

/*
 * DP_PIX_WIDTH destination pixel-width codes (bits 0-3), source codes
 * (bits 8-11), host source codes (bits 16-19), host mono triplication
 * for packed 24bpp (bit 13), and the sub-byte pixel order (bit 24:
 * set = LSB first within each byte; only affects 1bpp/4bpp data).
 */
#define ATI_PIX_WIDTH_DST_MASK    0xf
#define ATI_PIX_WIDTH_SRC_SHIFT   8
#define ATI_PIX_WIDTH_SRC_MASK    0xf
#define ATI_PIX_WIDTH_HOST_SHIFT  16
#define ATI_PIX_WIDTH_HOST_MASK   0xf
#define ATI_DP_HOST_TRIPLE_EN     (1u << 13)
#define ATI_PIX_WIDTH_BYTE_ORDER  (1u << 24)   /* set = LSB-to-MSB */

/* SRC_CNTL fields (component-register layout, Linux mach64.h) */
#define ATI_SRC_PATTERN_EN        0x01
#define ATI_SRC_ROTATION_EN       0x02
#define ATI_SRC_LINEAR_EN         0x04
#define ATI_SRC_BYTE_ALIGN        0x08
#define ATI_SRC_LINE_X_L2R        0x10

/*
 * HOST_CNTL (LT3REGRE 5-34): bit 0 re-aligns host consumption to the
 * next byte boundary when the destination advances a row; bit 1
 * byte-swaps each incoming HOST_DATA word (big-endian translation --
 * what a big-endian guest driver enables instead of swapping its own
 * stores; Apple's NDRV sets exactly 0x2, verified by live trace).
 */
#define ATI_HOST_BYTE_ALIGN       0x1
#define ATI_HOST_BIG_ENDIAN_EN    0x2

/* Host-data register block (CPU streams pixel/mask words here). */
#define ATI_HOST_DATA0            0x200
#define ATI_HOST_DATAF            0x23C

/*
 * Bus mastering (LT3REGRE chapter 6): the GUI bus master walks a
 * descriptor table in system memory; each 16-byte entry is
 * {FRAME_BUF_OFFSET, SYSTEM_MEM_ADDR, COMMAND, reserved}, where
 * COMMAND holds the byte count plus FRAME_OFFSET_HOLD (bit 30, "keep
 * writing the same aperture offset" -- how register ports and
 * HOST_DATA are fed) and END_OF_LIST (bit 31). BM_GUI_TABLE (block 1)
 * and its command-FIFO alias BM_GUI_TABLE_CMD (block 0) hold the
 * table's physical address in bits 31:4 and kick the walk.
 * BM_HOSTDATA is the DMA port into the draw engine's host-data
 * consumer; BM_ADDR/BM_DATA is a dual-purpose port for register-list
 * programming (first write = block-0 register index [7:0] and count
 * field [21:8] (value+1 registers), following writes = data with the
 * index auto-incrementing; any other register write resets it to
 * address mode). The 1_6x registers shadow the current descriptor.
 */
#define ATI_BM_HOSTDATA           0x244
#define ATI_BM_ADDR_DATA          0x248
#define ATI_BM_GUI_TABLE_CMD      0x24C
#define ATI_BM_FRAME_BUF_OFFSET   0x580
#define ATI_BM_SYSTEM_MEM_ADDR    0x584
#define ATI_BM_COMMAND            0x588
#define ATI_BM_STATUS             0x58C
#define ATI_BM_GUI_TABLE          0x5B8
#define ATI_BM_SYSTEM_TABLE       0x5BC
#define ATI_BM_FRAME_OFFSET_HOLD  (1u << 30)
#define ATI_BM_END_OF_LIST        (1u << 31)
/* CRTC_INT_CNTL bus-master-list-complete interrupt (Rage Pro) */
#define ATI_BUSMASTER_EOL_INT_EN  (1u << 24)
#define ATI_BUSMASTER_EOL_INT     (1u << 25)
#define ATI_BUSMASTER_EOL_INT_AK  (1u << 25)

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
