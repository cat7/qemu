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

#endif /* ATI_MACH64_REGS_H */
