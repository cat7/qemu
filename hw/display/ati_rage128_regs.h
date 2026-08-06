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
 * The retail card's FCode drives 0x6C as a second GPIO port ("GPIO
 * MONID B": A [3:0], Y [11:8] read-only, EN [19:16], MASK [27:24] --
 * same lane layout as GPIO_MONID) for its DDC/EDID path: its word
 * 0x918 sets MASK=0xf, toggles CRTC_OFFSET bit 23 and expects pad 3
 * (Y bit 3) to follow -- a cable/DDC presence handshake -- before the
 * bulk EDID read. The RRG names 0x6C SEPROM_CNTL; both uses share the
 * pads on real silicon.
 */
#define R128_GPIO_MONIDB             0x006c
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
/*
 * The ATI PCI GART table (whose guest-physical base page the driver
 * writes into PCI_GART_PAGE): 8192 little-endian 32-bit entries, one
 * per 4KB page of a 32MB card-address "VM" window, each entry the
 * bus/physical address of the backing page (ati_pcigart.c,
 * DRM_ATI_GART_PCI flavor: plain LE32 page address, no flag bits).
 */
#define R128_PCIGART_TABLE_ENTRIES   8192
#define R128_SOFT_RESET_GUI          (1u << 0)
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
#define R128_GUI_SCRATCH_REG0        0x15e0
#define R128_GUI_SCRATCH_REG1        0x15e4

/*
 * PM4/CCE (Concurrent Command Engine) GUI command-FIFO ring buffer --
 * the mechanism the real classic Mac driver actually uses for register
 * and 2D command submission on this card, reached after the older
 * one-shot BM_GUI_TABLE descriptor engine's own smoke test succeeds.
 * RRG-G04500-C's coverage of this whole area is minimal (only
 * PM4_VC_TIMESTAMP0/1 at 0x7B0/0x7B4 and PM4_BUFFER_DL_WPTR_DELAY at
 * 0x718 have real register pages; the manual's PM4 vertex-engine
 * block simply stops there). These offsets are cross-verified two
 * ways: live-traced (2026-08-02, retail PCI card ROM/NDRV) against a
 * prior, independent reference implementation of this same engine
 * (SourceFiles/ATI/qemu/ati_cce.c, read-only project reference
 * material) -- R128_PM4_STAT's offset in particular was *predicted*
 * by that reference and then found to match a live guest poll byte-
 * for-byte, and PM4_BUFFER_DL_WPTR (0x714) sits exactly 4 bytes before
 * the manual-documented PM4_BUFFER_DL_WPTR_DELAY (0x718), which is
 * exactly the layout that register's own name implies.
 *
 * The ring buffer lives in VRAM (byte offset given by
 * R128_PM4_BUFFER_OFFSET), not system memory -- this PCI, non-AGP
 * variant has no GART/system-memory command access, unlike later
 * AGP/PCIe parts. Packet format (from the same reference, itself
 * textbook ATI PM4: type in bits[31:30] of each ring dword): type 0 =
 * sequential register writes starting at ((header&0x1FFF)<<2), count
 * ((header>>16)&0x3FFF)+1, one-register-repeated if bit15 set; type 1
 * = two register writes (rare, not modeled); type 2 = NOP/padding;
 * type 3 = a 2D/3D draw command (opcode in bits[15:8], not modeled
 * yet -- consumed and traced as unimplemented so the ring position
 * stays correct).
 */
#define R128_PM4_BUFFER_OFFSET       0x0700
#define R128_PM4_BUFFER_CNTL         0x0704
#define R128_PM4_BUFFER_WM_CNTL      0x0708
#define R128_PM4_BUFFER_DL_RPTR_ADDR 0x070c
#define R128_PM4_BUFFER_DL_RPTR      0x0710
#define R128_PM4_BUFFER_DL_WPTR      0x0714
#define R128_PM4_IW_INDOFF           0x0738
#define R128_PM4_IW_INDSIZE          0x073c
#define R128_PM4_STAT                0x07b8
#define R128_PM4_MICROCODE_ADDR      0x07d4
#define R128_PM4_MICROCODE_RADDR     0x07d8
#define R128_PM4_MICROCODE_DATAH     0x07dc
#define R128_PM4_MICROCODE_DATAL     0x07e0
#define R128_PM4_BUFFER_ADDR         0x07f0
#define R128_PM4_MICRO_CNTL          0x07fc

/*
 * PM4_BUFFER_CNTL semantics per the Linux r128 DRM driver
 * (r128_drv.h, the authoritative public reference for this engine --
 * our previous START/RESET bit-0/bit-4 interpretation, inherited from
 * the SourceFiles reference implementation, was wrong): bits [31:28]
 * select the command-FIFO partitioning MODE (0 = NONPM4/none, odd
 * values are PIO variants, even values 2-8 give the primary stream a
 * bus-master ring; mode 7 "64PIO_64VCBM_64INDBM" -- PIO primary
 * stream + bus-master vertex/indirect buffers -- is what Mac OS X
 * 10.2's driver uses, value 0x78000000 with NOUPDATE), bit 27 =
 * NOUPDATE (don't DMA the read pointer to memory), and the low bits
 * hold log2 of the ring size in qwords for the ring modes.
 */
#define R128_PM4_MODE_MASK           (15u << 28)
#define R128_PM4_NONPM4              (0u << 28)
#define R128_PM4_192BM               (2u << 28)
#define R128_PM4_128BM_64INDBM       (4u << 28)
#define R128_PM4_64BM_128INDBM       (6u << 28)
#define R128_PM4_64BM_64VCBM_64INDBM (8u << 28)
#define R128_PM4_BUFFER_CNTL_NOUPDATE (1u << 27)
#define R128_PM4_BUFFER_SIZE_L2QW(c) ((c) & 0xff)
#define R128_PM4_BUFFER_DL_DONE      (1u << 31)
#define R128_PM4_MICRO_FREERUN       (1u << 30)
#define R128_PM4_MICROCODE_WORDS     256

/*
 * PM4_BUFFER_OFFSET flag: ring lives in AGP/"VM" (GART-translated)
 * space rather than local VRAM; the rest of the value is the offset
 * within that space (Linux: "ring_start | R128_AGP_OFFSET").
 */
#define R128_AGP_OFFSET_FLAG         0x02000000

#define R128_PM4_PACKET_TYPE(h)      (((h) >> 30) & 3)
#define R128_PM4_PACKET0_REG(h)      (((h) & 0x1fff) << 2)
#define R128_PM4_PACKET0_ONE_REG(h)  (((h) >> 15) & 1)
#define R128_PM4_PACKET_COUNT(h)     ((((h) >> 16) & 0x3fff) + 1)
#define R128_PM4_PACKET3_OPCODE(h)   (((h) >> 8) & 0xff)
#define R128_PM4_PACKET1_REG1(h)     (((h) & 0x7ff) << 2)
#define R128_PM4_PACKET1_REG2(h)     ((((h) >> 11) & 0x7ff) << 2)

/*
 * PM4 packet3 2D draw opcodes actually needed to make PAINT (solid
 * fill) and BITBLT (screen-to-screen copy, including cross-card
 * copies staged through HOSTDATA_BLT) work -- offsets/semantics from
 * SourceFiles/ATI/qemu/ati_int.h, cross-checked against real ATI
 * driver conventions (same opcode values used by every Rage/Radeon
 * generation's PM4 parser).
 */
#define R128_PM4_OPCODE_PAINT         0x91
/*
 * PAINT_MULTI: the same solid-fill operation as PAINT, but carrying
 * several rectangles in one packet -- the payload is consecutive
 * (DST_Y_X, DST_HEIGHT_WIDTH) pairs, with the drawing context set up
 * beforehand through ordinary register writes (the Mac driver programs
 * it via the GUI context "_C" aliases). Classic Mac OS paints every
 * window panel, button and dialog background with these, so dropping
 * them leaves only text and lines on screen -- the long-standing
 * "ghost window" rendering on this card.
 */
#define R128_PM4_OPCODE_PAINT_MULTI   0x9a
#define R128_PM4_OPCODE_BITBLT        0x92
#define R128_PM4_OPCODE_HOSTDATA_BLT  0x94

/*
 * PIO alternative submission path for the same PM4 stream: undocumented
 * in RRG-G04500-C (whose own "GUI Bus Mastering Registers" section is a
 * stub -- see above), but live-traced 2026-08-02 against the retail PCI
 * card's real NDRV, and named/offset-confirmed against the independent
 * reference (SourceFiles/ATI/qemu/ati_regs.h). Both addresses are the
 * same push port -- consecutive writes there (regardless of which of
 * the two addresses each individual write lands on) feed the next dword
 * of an ordinary PM4 packet stream, identical in format to the ring's.
 * Confirmed live: a 6-dword stream across 3 writes decoded as two
 * type-0 packets, the second of which wrote the exact 64-bit fence
 * value the driver polls for at GUI_SCRATCH_REG0/1 -- i.e. this IS the
 * real driver's actual submission path, not a secondary/optional one.
 */
#define R128_PM4_FIFO_DATA_EVEN      0x1000
#define R128_PM4_FIFO_DATA_ODD       0x1004

/*
 * 2D GUI (destination datapath) engine. Offsets cross-verified directly
 * against RRG-G04500-C (unlike the PM4/CCE block above, this whole area
 * IS fully documented in the manual, chapter "Destination GUI
 * Registers"/"Datapath Registers") AND against the real, shipped,
 * upstream QEMU `ati-vga` device (hw/display/ati.c/ati_regs.h/ati_2d.c)
 * -- both independent sources agree on every offset here byte-for-byte.
 * Semantics (which combined register triggers a blit on write, the
 * DP_GUI_MASTER_CNTL field-aliasing behavior, the HOST_DATA accumulator
 * protocol) are ported from that same upstream ati_2d.c/ati.c, which is
 * real production code -- not the abandoned SourceFiles/ATI/qemu clone
 * (see project memory: that tree was already tried once and its
 * accelerator never loaded).
 */
#define R128_DST_OFFSET              0x1404
#define R128_DST_PITCH               0x1408
#define R128_DST_WIDTH               0x140c
#define R128_DST_HEIGHT              0x1410
#define R128_SRC_X                   0x1414
#define R128_SRC_Y                   0x1418
#define R128_DST_X                   0x141c
#define R128_DST_Y                   0x1420
#define R128_SRC_PITCH_OFFSET        0x1428
#define R128_DST_PITCH_OFFSET        0x142c
#define R128_SRC_Y_X                 0x1434
#define R128_DST_Y_X                 0x1438
#define R128_DST_HEIGHT_WIDTH        0x143c
#define R128_DP_GUI_MASTER_CNTL      0x146c
/*
 * GUI_MASTER_CNTL bit 31: the packet carries the brush pattern and
 * origin inline, rather than the driver having pre-loaded the
 * BRUSH_DATA registers. Mac OS sets it on every patterned PAINT.
 */
#define R128_GMC_LD_BRUSH_Y_X        0x80000000
#define R128_BRUSH_Y_X               0x1474
#define R128_BRUSH_DATA0             0x1480   /* .. BRUSH_DATA63 at 0x157c */
#define R128_BRUSH_DATA63            0x157c
#define R128_DP_BRUSH_BKGD_CLR       0x1478
#define R128_DP_BRUSH_FRGD_CLR       0x147c
#define R128_DST_WIDTH_X             0x1588
#define R128_SRC_X_Y                 0x1590
#define R128_DST_X_Y                 0x1594
#define R128_DST_WIDTH_HEIGHT        0x1598
#define R128_DST_HEIGHT_Y            0x15a0
#define R128_SRC_OFFSET              0x15ac
#define R128_SRC_PITCH               0x15b0
/*
 * Colour compare: a per-pixel test that suppresses the write. Mac OS
 * uses it to repaint a text field's background without disturbing the
 * glyphs already in it, so leaving it unimplemented erased the text.
 */
#define R128_CLR_CMP_CNTL            0x15c0
#define R128_CLR_CMP_CLR_SRC         0x15c4
#define R128_CLR_CMP_CLR_DST         0x15c8
#define R128_CLR_CMP_MASK            0x15cc
#define R128_CLR_CMP_FN_MASK         0x00000007
#define R128_CLR_CMP_FN_FALSE        0
#define R128_CLR_CMP_FN_TRUE         1
#define R128_CLR_CMP_FN_NOT_EQUAL    4
#define R128_CLR_CMP_FN_EQUAL        5
#define R128_CLR_CMP_SRC_SOURCE      0x01000000
#define R128_DP_SRC_FRGD_CLR         0x15d8
#define R128_DP_SRC_BKGD_CLR         0x15dc
#define R128_SC_LEFT                 0x1640
#define R128_SC_RIGHT                0x1644
#define R128_SC_TOP                  0x1648
#define R128_SC_BOTTOM               0x164c
#define R128_SRC_SC_RIGHT            0x1654
#define R128_SRC_SC_BOTTOM           0x165c
#define R128_DP_CNTL                 0x16c0
#define R128_DP_DATATYPE             0x16c4
#define R128_DP_MIX                  0x16c8
#define R128_DP_WRITE_MASK           0x16cc
#define R128_DEFAULT_OFFSET          0x16e0
#define R128_DEFAULT_PITCH           0x16e4
#define R128_DEFAULT_SC_BOTTOM_RIGHT 0x16e8
#define R128_SC_TOP_LEFT             0x16ec
#define R128_SC_BOTTOM_RIGHT         0x16f0
#define R128_SRC_SC_BOTTOM_RIGHT     0x16f4
/*
 * GUI context ("_C") registers, RRG-G04500-C: write-only aliases of the
 * corresponding base registers. XFree86's r128 accel and Mac OS X's
 * driver program per-operation state through these (the OS X driver's
 * full-screen presentation batches use ONLY this block for GMC/scissor,
 * so dropping them executes those blits with stale rop/datatype).
 */
#define R128_DST_PITCH_OFFSET_C      0x1c80
#define R128_DP_GUI_MASTER_CNTL_C    0x1c84
#define R128_SC_TOP_LEFT_C           0x1c88
#define R128_SC_BOTTOM_RIGHT_C       0x1c8c
#define R128_CONSTANT_COLOR_C        0x1d34
#define R128_PLANE_3D_MASK_C         0x1d44
#define R128_HOST_DATA0              0x17c0
#define R128_HOST_DATA1              0x17c4
#define R128_HOST_DATA2              0x17c8
#define R128_HOST_DATA3              0x17cc
#define R128_HOST_DATA4              0x17d0
#define R128_HOST_DATA5              0x17d4
#define R128_HOST_DATA6              0x17d8
#define R128_HOST_DATA7              0x17dc
#define R128_HOST_DATA_LAST          0x17e0

#define R128_ATI_HOST_DATA_ACC_BITS  128

#define R128_DP_DST_DATATYPE         0x0000000f
#define R128_DP_BRUSH_DATATYPE       0x00000f00
#define R128_DP_BRUSH_DATATYPE_SHIFT 8
/*
 * Brush (pattern) types, DP_DATATYPE bits 11:8 -- the same codes
 * GUI_MASTER_CNTL carries in bits 7:4. The "_LA" ("leave alone")
 * variants are transparent: where the pattern bit is 0 the destination
 * is not touched at all. Mac OS draws its drag-selection marquee as one
 * big DSTINVERT rectangle stamped through an 8x8 MONO_FG_LA brush, so
 * treating every brush as solid inverted the WHOLE rectangle instead of
 * a dotted outline -- leaving olive-green (inverted desktop purple)
 * blocks behind on screen.
 */
#define R128_BRUSH_8X8_MONO_FG_BG    0
#define R128_BRUSH_8X8_MONO_FG_LA    1
#define R128_BRUSH_1X8_MONO_FG_BG    4
#define R128_BRUSH_1X8_MONO_FG_LA    5
#define R128_BRUSH_32X1_MONO_FG_BG   6
#define R128_BRUSH_32X1_MONO_FG_LA   7
#define R128_BRUSH_32X32_MONO_FG_BG  8
#define R128_BRUSH_32X32_MONO_FG_LA  9
#define R128_BRUSH_8X8_COLOR         10
#define R128_BRUSH_1X8_COLOR         12
#define R128_BRUSH_SOLID_COLOR       13
#define R128_BRUSH_NONE              15
#define R128_DP_SRC_DATATYPE         0x00030000
#define R128_DP_ROP3                 0x00ff0000
#define R128_DP_SRC_SOURCE           0x00000700
#define R128_DP_SRC_HOST             0x00000300
#define R128_DP_SRC_HOST_BYTEALIGN   0x00000400
#define R128_DP_BYTE_PIX_ORDER       0x40000000
/*
 * "Host data is big endian" -- the chip byte-swaps every pixel the host
 * feeds it through the HOST_DATA registers or a HOSTDATA_BLT payload,
 * by pixel size. A big-endian driver that byte-swaps its COMMAND dwords
 * in software (so the little-endian command fetch reads them right) can
 * then ship bitmap payload verbatim and let the chip convert it.
 * Confirmed against xf86-video-r128's r128_reg.h.
 */
#define R128_HOST_BIG_ENDIAN_EN      0x20000000
/*
 * The fields DP_GUI_MASTER_CNTL aliases into DP_DATATYPE. Everything
 * outside this mask -- HOST_BIG_ENDIAN_EN above, in particular -- has no
 * counterpart in GUI_MASTER_CNTL and must survive a write to it.
 */
#define R128_DP_DATATYPE_GMC_ALIAS   (R128_DP_DST_DATATYPE | \
                                      R128_DP_BRUSH_DATATYPE | \
                                      R128_DP_SRC_DATATYPE | \
                                      R128_DP_BYTE_PIX_ORDER)
#define R128_SRC_MONO_FRGD_BKGD      0x00000000
#define R128_SRC_MONO_FRGD           0x00010000
#define R128_SRC_COLOR                0x00030000
#define R128_DST_X_LEFT_TO_RIGHT     0x00000001
#define R128_DST_Y_TOP_TO_BOTTOM     0x00000002
#define R128_GMC_SRC_PITCH_OFFSET_CNTL 0x00000001
#define R128_GMC_DST_PITCH_OFFSET_CNTL 0x00000002
#define R128_GMC_SRC_CLIPPING        0x00000004
#define R128_GMC_DST_CLIPPING        0x00000008
#define R128_GMC_ROP3_MASK           0x00ff0000
#define R128_ROP3_BLACKNESS          0x00000000
#define R128_ROP3_SRCCOPY            0x00cc0000
#define R128_ROP3_PATCOPY            0x00f00000
#define R128_ROP3_WHITENESS          0x00ff0000

/*
 * GUI bus mastering (RRG-G04500-C 3.34 "GUI Bus Mastering Registers" is
 * a stub in the manual itself -- literally "<No description>" with no
 * register table, confirmed against the actual PDF page, not a text
 * extraction gap). Only BM_QUEUE_FREE_STATUS (0xA14), BM_ABORT (0xA88)
 * and the BM_CHUNK_0_VAL name (revision-history mention only) are
 * documented anywhere in it, and this smoke test doesn't touch any of
 * them. BM_GUI_TABLE's offset and the descriptor format are
 * reverse-engineered from a live capture of the real OEM Mac FCode's
 * post-CRTC-bringup bus-master smoke test (2026-08-02): it writes an 8
 * byte sentinel to system RAM, points a one-entry descriptor table at
 * it via this register, then reads back GUI_SCRATCH_REG0/1 expecting
 * the sentinel to have landed there -- see ati_rage128_bm_gui_run().
 */
#define R128_BM_GUI_TABLE            0x0a50
#define R128_BM_CHUNK_0_VAL          0x0a18

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


/*
 * CNTL_SCALING (packet-3 opcode 0x96): the scaled blit Mac OS uses for
 * video on this card, the counterpart of the mach64's scaler pipe. A
 * 16-dword packet; the layout below was established from a live capture
 * of QuickTime playback and cross-checked against the mach64, which
 * drives the same movie through its own scaler with identical
 * parameters (the X/Y DDA increments are literally the same values).
 *
 *   [0]  GUI_MASTER_CNTL         [3]  SC_TOP_LEFT
 *   [4]  SC_BOTTOM_RIGHT         [8]  source datatype
 *   [9]  source offset (bytes)   [10] source pitch, in 8-pixel units
 *   [12] X increment             [13] Y increment
 *   [14] DST_X_Y   (X high)      [15] DST_HEIGHT_WIDTH (height high)
 *
 * Self-consistency check that pins four of those at once: the scissors
 * in [3]/[4] exactly bound the rectangle that [14] and [15] describe.
 */
#define R128_PM4_OPCODE_SCALING       0x96
#define R128_SCALE_PKT_DWORDS         16
#define R128_SCALE_PKT_GMC            0
#define R128_SCALE_PKT_SRC_PITCH_OFF  1
#define R128_SCALE_PKT_DST_PITCH_OFF  2
#define R128_SCALE_PKT_SC_TL          3
#define R128_SCALE_PKT_SC_BR          4
#define R128_SCALE_PKT_DATATYPE       8
#define R128_SCALE_PKT_OFFSET         9
#define R128_SCALE_PKT_PITCH          10
#define R128_SCALE_PKT_X_INC          12
#define R128_SCALE_PKT_Y_INC          13
#define R128_SCALE_PKT_DST_X_Y        14
#define R128_SCALE_PKT_DST_H_W        15

/*
 * Scaler source datatypes. The two 4:2:2 codes are named inconsistently
 * between the tables in xf86-video-r128's own header, so trust the
 * behaviour instead: the mach64 uses code 12 for this same movie and
 * renders correctly as UYVY ('2vuy', the classic Mac 4:2:2 layout).
 */
/*
 * PITCH_OFFSET packing, as Linux's r128 driver builds it:
 * (pitch << 21) | (offset >> 5), pitch counting 8-pixel units. Verified
 * against the captured packet, where the source form decodes to pitch
 * 192 and offset 0x1fda000 -- the same offset the packet also carries
 * separately, and the same pitch.
 */
#define R128_PITCH_OFFSET_PITCH_SHIFT 21
#define R128_PITCH_OFFSET_OFF_MASK    0x001fffff
#define R128_PITCH_OFFSET_OFF_SHIFT   5

/*
 * Blit directions also have a second, differently packed home. Leaving
 * it undecoded left the direction stale, so an overlapping copy with a
 * vertical component duplicated rows -- visible as repeated fragments
 * when a window is dragged anything other than exactly horizontally.
 */
#define R128_DP_CNTL_XDIR_YDIR_YMAJOR 0x16d0
#define R128_DST_Y_DIR_TOP_TO_BOTTOM  0x00008000
#define R128_DST_X_DIR_LEFT_TO_RIGHT  0x80000000

#define R128_SCALE_DT_ARGB1555        3
#define R128_SCALE_DT_RGB565          4
#define R128_SCALE_DT_ARGB8888        6
#define R128_SCALE_DT_Y8              8
#define R128_SCALE_DT_YUYV422         11
#define R128_SCALE_DT_UYVY422         12
#define R128_SCALE_DT_AYUV444         14

#endif /* ATI_RAGE128_REGS_H */
