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

#ifndef ATI_R350_REGS_H
#define ATI_R350_REGS_H

#define R350_MM_INDEX                0x0000
#define R350_MM_DATA                 0x0004
#define R350_CLOCK_CNTL_INDEX        0x0008
#define R350_CLOCK_CNTL_DATA         0x000c
#define R350_BIOS_0_SCRATCH          0x0010
#define R350_BIOS_1_SCRATCH          0x0014
#define R350_BIOS_2_SCRATCH          0x0018
#define R350_BIOS_3_SCRATCH          0x001c
#define R350_BUS_CNTL                0x0030
#define R350_BUS_CNTL1               0x0034
#define R350_MEM_VGA_WP_SEL          0x0038
#define R350_MEM_VGA_RP_SEL          0x003c
#define R350_GEN_INT_CNTL            0x0040
#define R350_GEN_INT_STATUS          0x0044
#define R350_CRTC_GEN_CNTL           0x0050
#define R350_CRTC_EXT_CNTL           0x0054
#define R350_DAC_CNTL                0x0058
#define R350_CRTC_STATUS             0x005c
#define R350_GPIO_MONID              0x0068
#define R350_SEPROM_CNTL             0x006c
/*
 * The retail card's FCode drives 0x6C as a second GPIO port ("GPIO
 * MONID B": A [3:0], Y [11:8] read-only, EN [19:16], MASK [27:24] --
 * same lane layout as GPIO_MONID) for its DDC/EDID path: its word
 * 0x918 sets MASK=0xf, toggles CRTC_OFFSET bit 23 and expects pad 3
 * (Y bit 3) to follow -- a cable/DDC presence handshake -- before the
 * bulk EDID read. The RRG names 0x6C SEPROM_CNTL; both uses share the
 * pads on real silicon.
 */
#define R350_GPIO_MONIDB             0x006c
/*
 * The hardware I2C engine (0x0090/0x0094/0x0098) is NOT documented in
 * RRG-G04500-C at all, but the OEM Mac FCode ROM's constant table
 * includes all three offsets and XFree86's r128_reg.h names them
 * I2C_CNTL_0/I2C_CNTL_1/I2C_DATA. Bit layout below is from XFree86 /
 * the abandoned SourceFiles/ATI reference, to be validated against
 * live FCode traces.
 */
#define R350_I2C_CNTL_0              0x0090
#define R350_I2C_CNTL_1              0x0094
#define R350_I2C_DATA                0x0098
#define R350_AMCGPIO_MASK_MIR        0x009c
#define R350_AMCGPIO_A_MIR           0x00a0
#define R350_AMCGPIO_Y_MIR           0x00a4
#define R350_AMCGPIO_EN_MIR          0x00a8
#define R350_PALETTE_INDEX           0x00b0
#define R350_PALETTE_DATA            0x00b4
#define R350_CONFIG_CNTL             0x00e0
#define R350_CONFIG_XSTRAP           0x00e4
#define R350_CONFIG_BONDS            0x00e8
#define R350_GEN_RESET_CNTL          0x00f0
#define R350_GEN_STATUS              0x00f4
#define R350_CONFIG_MEMSIZE          0x00f8
#define R350_CONFIG_APER_0_BASE      0x0100
#define R350_CONFIG_APER_1_BASE      0x0104
#define R350_CONFIG_APER_SIZE        0x0108
#define R350_CONFIG_REG_1_BASE       0x010c
#define R350_CONFIG_REG_APER_SIZE    0x0110
#define R350_CONFIG_MEMSIZE_EMBEDDED 0x0114
#define R350_TEST_DEBUG_CNTL         0x0120
#define R350_HOST_PATH_CNTL          0x0130
#define R350_SW_SEMAPHORE            0x013c
#define R350_MEM_CNTL                0x0140
#define R350_EXT_MEM_CNTL            0x0144
#define R350_MEM_ADDR_CONFIG         0x0148
#define R350_MEM_INTF_CNTL           0x014c
#define R350_MEM_STR_CNTL            0x0150
#define R350_MEM_INIT_LAT_TIMER      0x0154
#define R350_MEM_SDRAM_MODE_REG      0x0158
#define R350_AGP_BASE                0x0170
#define R350_AGP_CNTL                0x0174
#define R350_AGP_APER_OFFSET         0x0178
#define R350_PCI_GART_PAGE           0x017c
/*
 * The ATI PCI GART table (whose guest-physical base page the driver
 * writes into PCI_GART_PAGE): 8192 little-endian 32-bit entries, one
 * per 4KB page of a 32MB card-address "VM" window, each entry the
 * bus/physical address of the backing page (ati_pcigart.c,
 * DRM_ATI_GART_PCI flavor: plain LE32 page address, no flag bits).
 */
#define R350_PCIGART_TABLE_ENTRIES   8192
#define R350_SOFT_RESET_GUI          (1u << 0)
#define R350_PC_NGUI_MODE            0x0180
#define R350_PC_NGUI_CTLSTAT         0x0184
#define R350_PC_MISC_CTL             0x0188
#define R350_CRTC_H_TOTAL_DISP       0x0200
#define R350_CRTC_H_SYNC_STRT_WID    0x0204
#define R350_CRTC_V_TOTAL_DISP       0x0208
#define R350_CRTC_V_SYNC_STRT_WID    0x020c
#define R350_CRTC_VLINE_CRNT_VLINE   0x0210
#define R350_CRTC_CRNT_FRAME         0x0214
#define R350_CRTC_GUI_TRIG_VLINE     0x0218
#define R350_CRTC_OFFSET             0x0224
#define R350_CRTC_OFFSET_CNTL        0x0228
#define R350_CRTC_PITCH              0x022c
#define R350_OVR_CLR                 0x0230
#define R350_OVR_WID_LEFT_RIGHT      0x0234
#define R350_OVR_WID_TOP_BOTTOM      0x0238
#define R350_CUR_OFFSET              0x0260
#define R350_CUR_HORZ_VERT_POSN      0x0264
#define R350_CUR_HORZ_VERT_OFF       0x0268
#define R350_CUR_CLR0                0x026c
#define R350_CUR_CLR1                0x0270

/* Hardware cursor field layout (RRG-G04500-C 3.13) */
#define R350_CUR_OFFSET_MASK         0x01fffff0 /* [24:0], [3:0] hardwired 0 */
#define R350_CUR_LOCK                (1u << 31) /* atomic shape/move update */
#define R350_CUR_VERT_POSN_MASK      0x7ff      /* [10:0] */
#define R350_CUR_HORZ_POSN_SHIFT     16         /* [26:16] */
#define R350_CUR_HORZ_POSN_MASK      0x7ff
#define R350_CUR_VERT_OFF_MASK       0x3f       /* [5:0] */
#define R350_CUR_HORZ_OFF_SHIFT      16         /* [21:16] */
#define R350_CUR_HORZ_OFF_MASK       0x3f
/* 64x64 pixels, 16 bytes per row: 8 of AND mask then 8 of XOR mask */
#define R350_CUR_WIDTH               64
#define R350_CUR_HEIGHT              64
#define R350_CUR_ROW_BYTES           16
#define R350_CUR_IMAGE_BYTES         (R350_CUR_HEIGHT * R350_CUR_ROW_BYTES)
#define R350_DAC_EXT_CNTL            0x0280
#define R350_DDA_CONFIG              0x02e0
#define R350_DDA_ON_OFF              0x02e4
#define R350_VGA_DDA_CONFIG          0x02e8
#define R350_VGA_DDA_ON_OFF          0x02ec
#define R350_GUI_DEBUG0              0x16a0
#define R350_WAIT_UNTIL              0x1720
#define R350_GUI_STAT                0x1740
#define R350_GUI_SCRATCH_REG0        0x15e0
#define R350_GUI_SCRATCH_REG1        0x15e4

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
 * material) -- R350_PM4_STAT's offset in particular was *predicted*
 * by that reference and then found to match a live guest poll byte-
 * for-byte, and PM4_BUFFER_DL_WPTR (0x714) sits exactly 4 bytes before
 * the manual-documented PM4_BUFFER_DL_WPTR_DELAY (0x718), which is
 * exactly the layout that register's own name implies.
 *
 * The ring buffer lives in VRAM (byte offset given by
 * R350_PM4_BUFFER_OFFSET), not system memory -- this PCI, non-AGP
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
#define R350_PM4_BUFFER_OFFSET       0x0700
#define R350_PM4_BUFFER_CNTL         0x0704
#define R350_PM4_BUFFER_WM_CNTL      0x0708
#define R350_PM4_BUFFER_DL_RPTR_ADDR 0x070c
#define R350_PM4_BUFFER_DL_RPTR      0x0710
#define R350_PM4_BUFFER_DL_WPTR      0x0714
#define R350_PM4_IW_INDOFF           0x0738
#define R350_PM4_IW_INDSIZE          0x073c
#define R350_PM4_STAT                0x07b8
#define R350_PM4_MICROCODE_ADDR      0x07d4
#define R350_PM4_MICROCODE_RADDR     0x07d8
#define R350_PM4_MICROCODE_DATAH     0x07dc
#define R350_PM4_MICROCODE_DATAL     0x07e0
#define R350_PM4_BUFFER_ADDR         0x07f0
#define R350_PM4_MICRO_CNTL          0x07fc

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
#define R350_PM4_MODE_MASK           (15u << 28)
#define R350_PM4_NONPM4              (0u << 28)
#define R350_PM4_192BM               (2u << 28)
#define R350_PM4_128BM_64INDBM       (4u << 28)
#define R350_PM4_64BM_128INDBM       (6u << 28)
#define R350_PM4_64BM_64VCBM_64INDBM (8u << 28)
#define R350_PM4_BUFFER_CNTL_NOUPDATE (1u << 27)
#define R350_PM4_BUFFER_SIZE_L2QW(c) ((c) & 0xff)
#define R350_PM4_BUFFER_DL_DONE      (1u << 31)
#define R350_PM4_MICRO_FREERUN       (1u << 30)
#define R350_PM4_MICROCODE_WORDS     256

/*
 * PM4_BUFFER_OFFSET flag: ring lives in AGP/"VM" (GART-translated)
 * space rather than local VRAM; the rest of the value is the offset
 * within that space (Linux: "ring_start | R350_AGP_OFFSET").
 */
#define R350_AGP_OFFSET_FLAG         0x02000000

#define R350_PM4_PACKET_TYPE(h)      (((h) >> 30) & 3)
#define R350_PM4_PACKET0_REG(h)      (((h) & 0x1fff) << 2)
#define R350_PM4_PACKET0_ONE_REG(h)  (((h) >> 15) & 1)
#define R350_PM4_PACKET_COUNT(h)     ((((h) >> 16) & 0x3fff) + 1)
#define R350_PM4_PACKET3_OPCODE(h)   (((h) >> 8) & 0xff)
#define R350_PM4_PACKET1_REG1(h)     (((h) & 0x7ff) << 2)
#define R350_PM4_PACKET1_REG2(h)     ((((h) >> 11) & 0x7ff) << 2)

/*
 * PM4 packet3 2D draw opcodes actually needed to make PAINT (solid
 * fill) and BITBLT (screen-to-screen copy, including cross-card
 * copies staged through HOSTDATA_BLT) work -- offsets/semantics from
 * SourceFiles/ATI/qemu/ati_int.h, cross-checked against real ATI
 * driver conventions (same opcode values used by every Rage/Radeon
 * generation's PM4 parser).
 */
#define R350_PM4_OPCODE_PAINT         0x91
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
#define R350_PM4_OPCODE_PAINT_MULTI   0x9a
#define R350_PM4_OPCODE_BITBLT        0x92
/*
 * CNTL_BITBLT_MULTI: BITBLT carrying its own destination pitch/offset, so
 * a run of copies can share one context dword. Mac OS X issues exactly one
 * of these per frame of a window drag -- while it went unimplemented the
 * copy simply never happened, and every later blit out of the driver's
 * offscreen surface propagated whatever stale content was left there.
 * That was the garbled window contents on this card.
 *
 * Two header dwords and then a RUN of rectangles, three dwords each -- the
 * MULTI is not decoration:
 *   [0]      GMC (DP_GUI_MASTER_CNTL)
 *   [1]      SRC_PITCH_OFFSET (observed 0x10000400 = offset 0x8000, pitch
 *            128 -- the screen; see the ring parser for why this is the
 *            source and not the destination)
 *   [2+3k]   SRC_X_Y
 *   [3+3k]   DST_X_Y
 *   [4+3k]   DST_WIDTH_HEIGHT
 * X/WIDTH live in the HIGH half and Y/HEIGHT in the low half, the same way
 * round as PAINT_MULTI and plain BITBLT on this driver.
 *
 * Captured live, iTunes sends 29 dwords = 2 + NINE rectangles, and they
 * tile one window exactly: 590x1, 594x1, 596x1, 598x2, then 600x390, then
 * 598x2, 596x1, 594x1, 590x1, with the destination Y running 10, 11, 12,
 * 13, 15, 405, 407, 408, 409 -- contiguous, narrow at top and bottom and
 * wide in between. That is a rounded-corner window, the same shape the
 * BITBLT and PAINT_MULTI comments describe. Handling only the first
 * rectangle copied a single 1-pixel-high strip and threw away the 600x390
 * body, which is why window CHROME came out right while the CONTENTS were
 * garbage.
 */
#define R350_PM4_OPCODE_BITBLT_MULTI  0x9b
/* header dwords plus at least one 3-dword rectangle */
#define R350_BITBLT_MULTI_MIN_DWORDS  5
/*
 * Rectangle-only blit continuation (not in any public register guide;
 * established from live OS X 10.4 window-drag captures). Three dwords
 * -- SRC_X_Y, DST_X_Y, DST_WIDTH_HEIGHT in BITBLT's trajectory layout
 * -- with the whole drawing context (GMC, pitches, offsets) inherited
 * from the registers as left by the preceding packet. Every drag step
 * pairs one with the BITBLT_MULTI title strip: a window-body-sized
 * screen-to-layer copy (e.g. 785x421) plus a bottom-edge strip
 * (763x11), re-homing the window image inside the re-anchored drag
 * layer. Dropping them leaves the body at its old layer rows: one
 * ghost title bar per drag step, shredding on fast drags.
 */
#define R350_PM4_OPCODE_BITBLT_RECT   0x1b
#define R350_PM4_OPCODE_HOSTDATA_BLT  0x94

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
#define R350_PM4_FIFO_DATA_EVEN      0x1000
#define R350_PM4_FIFO_DATA_ODD       0x1004
/*
 * The whole 0x1000-0x13ff range is the Concurrent Command Engine's
 * FIFO aperture (RRG-G04500-C, 2.2 "Memory Mapping": "Concurrent
 * Command Engine registers 1000h - 13FFh"): a dword written anywhere in
 * it is a command-FIFO push. FIFO_DATA_EVEN/ODD are merely its first
 * two names -- the DRM driver only ever uses those two, but Mac OS's
 * Rage 128 driver bursts up to eight dwords at 0x1000..0x101c.
 */
#define R350_PM4_FIFO_APER_END       0x13fc

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
#define R350_DST_OFFSET              0x1404
#define R350_DST_PITCH               0x1408
#define R350_DST_WIDTH               0x140c
#define R350_DST_HEIGHT              0x1410
#define R350_SRC_X                   0x1414
#define R350_SRC_Y                   0x1418
#define R350_DST_X                   0x141c
#define R350_DST_Y                   0x1420
#define R350_SRC_PITCH_OFFSET        0x1428
#define R350_DST_PITCH_OFFSET        0x142c
#define R350_SRC_Y_X                 0x1434
#define R350_DST_Y_X                 0x1438
#define R350_DST_HEIGHT_WIDTH        0x143c
#define R350_DP_GUI_MASTER_CNTL      0x146c
/*
 * GUI_MASTER_CNTL bit 31: the packet carries the brush pattern and
 * origin inline, rather than the driver having pre-loaded the
 * BRUSH_DATA registers. Mac OS sets it on every patterned PAINT.
 */
#define R350_GMC_LD_BRUSH_Y_X        0x80000000
#define R350_BRUSH_Y_X               0x1474
#define R350_BRUSH_DATA0             0x1480   /* .. BRUSH_DATA63 at 0x157c */
#define R350_BRUSH_DATA63            0x157c
#define R350_DP_BRUSH_BKGD_CLR       0x1478
#define R350_DP_BRUSH_FRGD_CLR       0x147c
#define R350_DST_WIDTH_X             0x1588
#define R350_SRC_X_Y                 0x1590
#define R350_DST_X_Y                 0x1594
#define R350_DST_WIDTH_HEIGHT        0x1598
#define R350_DST_HEIGHT_Y            0x15a0
#define R350_SRC_OFFSET              0x15ac
#define R350_SRC_PITCH               0x15b0
/*
 * Colour compare: a per-pixel test that suppresses the write. Mac OS
 * uses it to repaint a text field's background without disturbing the
 * glyphs already in it, so leaving it unimplemented erased the text.
 */
#define R350_CLR_CMP_CNTL            0x15c0
#define R350_CLR_CMP_CLR_SRC         0x15c4
#define R350_CLR_CMP_CLR_DST         0x15c8
#define R350_CLR_CMP_MASK            0x15cc
#define R350_CLR_CMP_FN_MASK         0x00000007
#define R350_CLR_CMP_FN_FALSE        0
#define R350_CLR_CMP_FN_TRUE         1
#define R350_CLR_CMP_FN_NOT_EQUAL    4
#define R350_CLR_CMP_FN_EQUAL        5
#define R350_CLR_CMP_SRC_SOURCE      0x01000000
#define R350_DP_SRC_FRGD_CLR         0x15d8
#define R350_DP_SRC_BKGD_CLR         0x15dc
#define R350_SC_LEFT                 0x1640
#define R350_SC_RIGHT                0x1644
#define R350_SC_TOP                  0x1648
#define R350_SC_BOTTOM               0x164c
#define R350_SRC_SC_RIGHT            0x1654
#define R350_SRC_SC_BOTTOM           0x165c
#define R350_DP_CNTL                 0x16c0
#define R350_DP_DATATYPE             0x16c4
#define R350_DP_MIX                  0x16c8
#define R350_DP_WRITE_MASK           0x16cc
#define R350_DEFAULT_OFFSET          0x16e0
#define R350_DEFAULT_PITCH           0x16e4
#define R350_DEFAULT_SC_BOTTOM_RIGHT 0x16e8
#define R350_SC_TOP_LEFT             0x16ec
#define R350_SC_BOTTOM_RIGHT         0x16f0
#define R350_SRC_SC_BOTTOM_RIGHT     0x16f4
/*
 * GUI context ("_C") registers, RRG-G04500-C: write-only aliases of the
 * corresponding base registers. XFree86's r128 accel and Mac OS X's
 * driver program per-operation state through these (the OS X driver's
 * full-screen presentation batches use ONLY this block for GMC/scissor,
 * so dropping them executes those blits with stale rop/datatype).
 */
#define R350_DST_PITCH_OFFSET_C      0x1c80
#define R350_DP_GUI_MASTER_CNTL_C    0x1c84
#define R350_SC_TOP_LEFT_C           0x1c88
#define R350_SC_BOTTOM_RIGHT_C       0x1c8c
#define R350_CONSTANT_COLOR_C        0x1d34
#define R350_PLANE_3D_MASK_C         0x1d44
#define R350_HOST_DATA0              0x17c0
#define R350_HOST_DATA1              0x17c4
#define R350_HOST_DATA2              0x17c8
#define R350_HOST_DATA3              0x17cc
#define R350_HOST_DATA4              0x17d0
#define R350_HOST_DATA5              0x17d4
#define R350_HOST_DATA6              0x17d8
#define R350_HOST_DATA7              0x17dc
#define R350_HOST_DATA_LAST          0x17e0

#define R350_ATI_HOST_DATA_ACC_BITS  128

#define R350_DP_DST_DATATYPE         0x0000000f
#define R350_DP_BRUSH_DATATYPE       0x00000f00
#define R350_DP_BRUSH_DATATYPE_SHIFT 8
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
#define R350_BRUSH_8X8_MONO_FG_BG    0
#define R350_BRUSH_8X8_MONO_FG_LA    1
#define R350_BRUSH_1X8_MONO_FG_BG    4
#define R350_BRUSH_1X8_MONO_FG_LA    5
#define R350_BRUSH_32X1_MONO_FG_BG   6
#define R350_BRUSH_32X1_MONO_FG_LA   7
#define R350_BRUSH_32X32_MONO_FG_BG  8
#define R350_BRUSH_32X32_MONO_FG_LA  9
#define R350_BRUSH_8X8_COLOR         10
#define R350_BRUSH_1X8_COLOR         12
#define R350_BRUSH_SOLID_COLOR       13
#define R350_BRUSH_NONE              15
#define R350_DP_SRC_DATATYPE         0x00030000
#define R350_DP_ROP3                 0x00ff0000
#define R350_DP_SRC_SOURCE           0x00000700
#define R350_DP_SRC_HOST             0x00000300
#define R350_DP_SRC_HOST_BYTEALIGN   0x00000400
#define R350_DP_BYTE_PIX_ORDER       0x40000000
/*
 * "Host data is big endian" -- the chip byte-swaps every pixel the host
 * feeds it through the HOST_DATA registers or a HOSTDATA_BLT payload,
 * by pixel size. A big-endian driver that byte-swaps its COMMAND dwords
 * in software (so the little-endian command fetch reads them right) can
 * then ship bitmap payload verbatim and let the chip convert it.
 * Confirmed against xf86-video-r128's r128_reg.h.
 */
#define R350_HOST_BIG_ENDIAN_EN      0x20000000
/*
 * The fields DP_GUI_MASTER_CNTL aliases into DP_DATATYPE. Everything
 * outside this mask -- HOST_BIG_ENDIAN_EN above, in particular -- has no
 * counterpart in GUI_MASTER_CNTL and must survive a write to it.
 */
#define R350_DP_DATATYPE_GMC_ALIAS   (R350_DP_DST_DATATYPE | \
                                      R350_DP_BRUSH_DATATYPE | \
                                      R350_DP_SRC_DATATYPE | \
                                      R350_DP_BYTE_PIX_ORDER)
#define R350_SRC_MONO_FRGD_BKGD      0x00000000
#define R350_SRC_MONO_FRGD           0x00010000
#define R350_SRC_COLOR                0x00030000
#define R350_DST_X_LEFT_TO_RIGHT     0x00000001
#define R350_DST_Y_TOP_TO_BOTTOM     0x00000002
#define R350_GMC_SRC_PITCH_OFFSET_CNTL 0x00000001
#define R350_GMC_DST_PITCH_OFFSET_CNTL 0x00000002
#define R350_GMC_SRC_CLIPPING        0x00000004
#define R350_GMC_DST_CLIPPING        0x00000008
#define R350_GMC_ROP3_MASK           0x00ff0000
#define R350_ROP3_BLACKNESS          0x00000000
#define R350_ROP3_SRCCOPY            0x00cc0000
#define R350_ROP3_PATCOPY            0x00f00000
#define R350_ROP3_WHITENESS          0x00ff0000

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
 * the sentinel to have landed there -- see ati_r350_bm_gui_run().
 */
#define R350_BM_GUI_TABLE            0x0a50
#define R350_BM_CHUNK_0_VAL          0x0a18

/* PCI config space read-only mirror */
#define R350_CFG_MIRROR_BASE         0x0f00
#define R350_CFG_MIRROR_END          0x0fff

/* GEN_INT_CNTL / GEN_INT_STATUS (status bits ack by writing 1) */
#define R350_CRTC_VBLANK_INT         (1 << 0)
#define R350_CRTC_VLINE_INT          (1 << 1)
#define R350_CRTC_VSYNC_INT          (1 << 2)
#define R350_SNAPSHOT_INT            (1 << 3)
#define R350_FP_DETECT_INT           (1 << 10)
#define R350_BUSMASTER_EOL_INT       (1 << 16)
#define R350_I2C_INT                 (1 << 17)
#define R350_MPP_GP_INT              (1 << 18)
#define R350_GUI_IDLE_INT            (1 << 19)
#define R350_VIPH_INT                (1 << 24)
/*
 * The software interrupt, which is how the driver learns that work it
 * submitted has finished: it enables SW_INT in GEN_INT_CNTL, the
 * command stream raises the interrupt by writing SW_INT_FIRE to
 * GEN_INT_STATUS, and the handler acknowledges by writing SW_INT back.
 *
 * Mac OS X's driver sleeps on this. With it unimplemented the ring
 * drains correctly and nothing looks wrong from the device's side --
 * read pointer equal to write pointer, no unimplemented commands --
 * while the guest waits forever for a completion signal that never
 * arrives, at low CPU with a dead user interface.
 */
#define R350_SW_INT                  (1 << 25)   /* status: pending/ack */
#define R350_SW_INT_FIRE             (1 << 26)   /* status: write to raise */
#define R350_GEN_INT_ACK_MASK        (R350_CRTC_VBLANK_INT | \
                                      R350_CRTC_VLINE_INT | \
                                      R350_CRTC_VSYNC_INT | \
                                      R350_SNAPSHOT_INT | \
                                      R350_FP_DETECT_INT | \
                                      R350_BUSMASTER_EOL_INT | \
                                      R350_I2C_INT | R350_MPP_GP_INT | \
                                      R350_GUI_IDLE_INT | R350_VIPH_INT | \
                                      R350_SW_INT)

/* CRTC_GEN_CNTL */
#define R350_CRTC_DBL_SCAN_EN        (1 << 0)
#define R350_CRTC_INTERLACE_EN       (1 << 1)
#define R350_CRTC_C_SYNC_EN          (1 << 4)
#define R350_CRTC_PIX_WIDTH_SHIFT    8
#define R350_CRTC_PIX_WIDTH_MASK     7
#define R350_PIX_WIDTH_4BPP          1
#define R350_PIX_WIDTH_8BPP          2
#define R350_PIX_WIDTH_15BPP         3
#define R350_PIX_WIDTH_16BPP         4
#define R350_PIX_WIDTH_24BPP         5
#define R350_PIX_WIDTH_32BPP         6
#define R350_CRTC_CUR_EN             (1 << 16)
#define R350_CRTC_EXT_DISP_EN        (1 << 24)
#define R350_CRTC_EN                 (1 << 25)
#define R350_CRTC_DISP_REQ_EN_B      (1 << 26)

/* CRTC_EXT_CNTL */
#define R350_VGA_ATI_LINEAR          (1 << 3)
#define R350_VGA_XCRT_CNT_EN         (1 << 6)
#define R350_CRTC_HSYNC_DIS          (1 << 8)
#define R350_CRTC_VSYNC_DIS          (1 << 9)
#define R350_CRTC_DISPLAY_DIS        (1 << 10)
#define R350_CRTC_SYNC_TRISTATE      (1 << 11)
#define R350_DFIFO_EXTSENSE          (1 << 21)  /* default 1 */

/* CRTC_STATUS */
#define R350_CRTC_VBLANK_CUR         (1 << 0)
#define R350_CRTC_VBLANK_SAVE        (1 << 1)  /* write 1 clears */
#define R350_CRTC_VLINE_SYNC         (1 << 2)
#define R350_CRTC_FRAME_ODD          (1 << 3)
#define R350_FIX_VSYNC_TIMING        (1u << 31) /* default 1 */

/* DAC_CNTL */
#define R350_DAC_RANGE_CNTL_MASK     3          /* default 2 (PS2 level) */
#define R350_DAC_BLANKING            (1 << 2)
#define R350_DAC_CMP_EN              (1 << 3)   /* default 1 */
#define R350_DAC_CMP_OUTPUT          (1 << 7)   /* RO: comparator/monitor sense */
#define R350_DAC_8BIT_EN             (1 << 8)
#define R350_DAC_MASK_SHIFT          24
#define R350_DAC_MASK_DEFAULT        0xffu

/* CRTC timing field extraction */
#define R350_CRTC_H_TOTAL_MASK       0x1ff      /* [8:0], chars */
#define R350_CRTC_H_DISP_SHIFT       16         /* [23:16], chars - 1 */
#define R350_CRTC_H_DISP_MASK        0xff
#define R350_CRTC_V_TOTAL_MASK       0x7ff      /* [10:0], lines */
#define R350_CRTC_V_DISP_SHIFT       16         /* [26:16], lines - 1 */
#define R350_CRTC_V_DISP_MASK        0x7ff
#define R350_CRTC_OFFSET_MASK        0x01fffff8 /* [24:0], bits 2:0 wired 0 */
#define R350_CRTC_OFFSET_LOCK        (1u << 31)
#define R350_CRTC_PITCH_MASK         0x3ff      /* [9:0], pixels * 8 */

/* CLOCK_CNTL_INDEX */
#define R350_PLL_ADDR_MASK           0x3f /* Radeon: 64 PLL registers */
#define R350_PLL_WR_EN               (1 << 7)
#define R350_PPLL_DIV_SEL_SHIFT      8
#define R350_PPLL_DIV_SEL_MASK       3

/* PLL register indices */
#define R350_PLL_CLK_PIN_CNTL        0x01
#define R350_PLL_PPLL_CNTL           0x02
#define R350_PLL_PPLL_REF_DIV        0x03
#define R350_PLL_PPLL_DIV_0          0x04
#define R350_PLL_PPLL_DIV_1          0x05
#define R350_PLL_PPLL_DIV_2          0x06
#define R350_PLL_PPLL_DIV_3          0x07
#define R350_PLL_VCLK_ECP_CNTL       0x08
#define R350_PLL_HTOTAL_CNTL         0x09
#define R350_PLL_X_MPLL_REF_FB_DIV   0x0a
#define R350_PLL_XPLL_CNTL           0x0b
#define R350_PLL_XDLL_CNTL           0x0c
#define R350_PLL_XCLK_CNTL           0x0d
#define R350_PLL_MPLL_CNTL           0x0e
#define R350_PLL_MCLK_CNTL           0x0f
/*
 * PPLL_REF_DIV and PPLL_DIV_0..3 all carry the atomic-update handshake
 * in bit 15: writing 1 (ATOMIC_UPDATE_W) requests the PLL update,
 * reading (ATOMIC_UPDATE_R) polls for completion -- real hardware
 * clears it when the new dividers have taken effect; we emulate an
 * instant PLL, so reads always see it clear.
 */
#define R350_PPLL_ATOMIC_UPDATE      (1 << 15)

/* CONFIG_CNTL */
#define R350_APER_0_ENDIAN_MASK      3          /* [1:0] */
#define R350_APER_1_ENDIAN_SHIFT     2          /* [3:2] */
#define R350_APER_ENDIAN_LE          0
#define R350_APER_ENDIAN_BE16        1
#define R350_APER_ENDIAN_BE32        2
#define R350_APER_REG_ENDIAN         (1 << 4)
#define R350_CFG_VGA_RAM_EN          (1 << 8)
#define R350_CFG_VGA_IO_DIS          (1 << 9)

/* CONFIG_XSTRAP (read-only strap reflections) */
#define R350_XSTRAP_ADDIN_CARD       (1 << 13)

/* GUI_STAT */
#define R350_GUI_FIFOCNT_MASK        0xfff      /* [11:0], default 0x40 free */
#define R350_GUI_ACTIVE              (1u << 31)

/* I2C_CNTL_0 (undocumented; XFree86 r128_reg.h layout) */
#define R350_I2C_DONE                (1 << 0)
#define R350_I2C_NACK                (1 << 1)
#define R350_I2C_HALT                (1 << 2)
#define R350_I2C_SOFT_RST            (1 << 5)
#define R350_I2C_DRIVE_EN            (1 << 6)
#define R350_I2C_DRIVE_SEL           (1 << 7)
#define R350_I2C_START               (1 << 8)
#define R350_I2C_STOP                (1 << 9)
#define R350_I2C_RECEIVE             (1 << 10)
#define R350_I2C_ABORT               (1 << 11)
#define R350_I2C_GO                  (1 << 12)
/* I2C_CNTL_1 */
#define R350_I2C_DATA_COUNT_SHIFT    0
#define R350_I2C_DATA_COUNT_MASK     0xff
#define R350_I2C_ADDR_SHIFT          8
#define R350_I2C_ADDR_MASK           0xff
#define R350_I2C_SEL                 (1 << 16)
#define R350_I2C_EN                  (1 << 17)
#define R350_I2C_TIME_LIMIT_SHIFT    24


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
#define R350_PM4_OPCODE_SCALING       0x96
#define R350_SCALE_PKT_DWORDS         16
#define R350_SCALE_PKT_GMC            0
#define R350_SCALE_PKT_SRC_PITCH_OFF  1
#define R350_SCALE_PKT_DST_PITCH_OFF  2
#define R350_SCALE_PKT_SC_TL          3
#define R350_SCALE_PKT_SC_BR          4
#define R350_SCALE_PKT_DATATYPE       8
#define R350_SCALE_PKT_OFFSET         9
#define R350_SCALE_PKT_PITCH          10
#define R350_SCALE_PKT_X_INC          12
#define R350_SCALE_PKT_Y_INC          13
#define R350_SCALE_PKT_DST_X_Y        14
#define R350_SCALE_PKT_DST_H_W        15

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
#define R350_PITCH_OFFSET_PITCH_SHIFT 21
#define R350_PITCH_OFFSET_OFF_MASK    0x001fffff
#define R350_PITCH_OFFSET_OFF_SHIFT   5

/*
 * Blit directions also have a second, differently packed home. Leaving
 * it undecoded left the direction stale, so an overlapping copy with a
 * vertical component duplicated rows -- visible as repeated fragments
 * when a window is dragged anything other than exactly horizontally.
 */
#define R350_DP_CNTL_XDIR_YDIR_YMAJOR 0x16d0
#define R350_DST_Y_DIR_TOP_TO_BOTTOM  0x00008000
#define R350_DST_X_DIR_LEFT_TO_RIGHT  0x80000000

/*
 * The same scaler, programmed through its own registers instead of a
 * CNTL_SCALING packet. Mac OS X's ATI driver draws the pointer this way
 * whenever the shape does not fit the two-colour hardware cursor (the
 * I-beam, with its alpha halo): IOGraphics' "cursor in VRAM" callout
 * hands the driver a 32-bit ARGB sprite, and the driver save-unders with
 * BITBLT_MULTI, then blends the sprite in with an alpha-blended scale
 * of it -- SRC_ALPHA / INV_SRC_ALPHA from MISC_3D_STATE_CNTL_REG, the
 * scale function selected there too (bits 9:8), GMC_3D_FCN_EN in
 * DP_GUI_MASTER_CNTL_C, and SCALE_DST_HEIGHT_WIDTH as the kick (the last
 * register written, as for the mach64's scaler). Captured live from
 * OS X 10.3 on 2026-08-18. Bit layouts as in xf86-video-r128's
 * r128_reg.h; the RRG lists these registers but documents no fields.
 */
#define R350_SCALE_SRC_HEIGHT_WIDTH   0x1994
#define R350_SCALE_OFFSET_0           0x1998
#define R350_SCALE_PITCH              0x199c
#define R350_SCALE_X_INC              0x19a0
#define R350_SCALE_Y_INC              0x19a4
#define R350_SCALE_HACC               0x19a8
#define R350_SCALE_VACC               0x19ac
#define R350_SCALE_DST_X_Y            0x19b0
#define R350_SCALE_DST_HEIGHT_WIDTH   0x19b4
#define R350_SCALE_3D_CNTL            0x1a00
#define R350_PRIM_TEXTURE_COMBINE_CNTL 0x1a08
#define R350_SCALE_3D_DATATYPE        0x1a20
#define R350_TEX_CNTL                 0x1800
#define R350_TEX_CNTL_C               0x1c9c
#define R350_MISC_3D_STATE_CNTL_REG   0x1ca0
#define R350_PRIM_TEX_CNTL_C          0x1cb0
#define R350_MISC_SCALE_3D_FCN_SHIFT  8       /* 0 noop, 1 scale, 2 texmap */
#define R350_MISC_SCALE_3D_FCN_MASK   0x3
#define R350_MISC_SCALE_3D_SCALE      1
#define R350_ALPHA_BLEND_SRC_SHIFT    16
#define R350_ALPHA_BLEND_DST_SHIFT    20
#define R350_ALPHA_BLEND_MASK         0xf
#define R350_ALPHA_BLEND_ZERO         0
#define R350_ALPHA_BLEND_ONE          1
#define R350_ALPHA_BLEND_SRCCOLOR     2
#define R350_ALPHA_BLEND_INVSRCCOLOR  3
#define R350_ALPHA_BLEND_SRCALPHA     4
#define R350_ALPHA_BLEND_INVSRCALPHA  5
#define R350_ALPHA_BLEND_DSTALPHA     6
#define R350_ALPHA_BLEND_INVDSTALPHA  7
#define R350_ALPHA_BLEND_DSTCOLOR     8
#define R350_ALPHA_BLEND_INVDSTCOLOR  9
#define R350_ALPHA_BLEND_SAT          10
#define R350_GMC_3D_FCN_EN            (1u << 28)

#define R350_SCALE_DT_ARGB1555        3
#define R350_SCALE_DT_RGB565          4
#define R350_SCALE_DT_ARGB8888        6
#define R350_SCALE_DT_Y8              8
#define R350_SCALE_DT_YUYV422         11
#define R350_SCALE_DT_UYVY422         12
#define R350_SCALE_DT_AYUV444         14


/* ---------------------------------------------------------------- */
/*
 * Radeon (R3xx) additions -- offsets and bits from the Linux radeon DRM
 * radeon_reg.h / r300_reg.h and the X.org radeon driver.
 */
#define R350_GPIO_VGA_DDC            0x0060
#define R350_GPIO_DVI_DDC            0x0064
#define R350_PALETTE_30_DATA         0x00b8
#define R350_MC_FB_LOCATION          0x0148
#define R350_MC_AGP_LOCATION         0x014c
#define R350_MC_IND_INDEX            0x01f8
#define R350_MC_IND_DATA             0x01fc
#define R350_DISPLAY_BASE_ADDR       0x023c
#define R350_CRTC2_GEN_CNTL          0x03f8
#define R350_CP_RB_BASE              0x0700
#define R350_CP_RB_CNTL              0x0704
#define R350_CP_RB_RPTR_ADDR         0x070c
#define R350_CP_RB_RPTR              0x0710
#define R350_CP_RB_WPTR              0x0714
#define R350_CP_RB_WPTR_DELAY        0x0718
#define R350_CP_RB_RPTR_WR           0x071c
#define R350_CP_IB_BASE              0x0738
#define R350_CP_IB_BUFSZ             0x073c
#define R350_CP_CSQ_CNTL             0x0740
#define R350_CP_CSQ_MODE             0x0744
#define R350_SCRATCH_UMSK            0x0770
#define R350_SCRATCH_ADDR            0x0774
#define R350_CP_STAT                 0x07c0
#define R350_CP_ME_CNTL              0x07d0
#define R350_CP_CSQ_ADDR             0x07f0
#define R350_CP_CSQ_DATA             0x07f4
#define R350_CP_CSQ_STAT             0x07f8
#define R350_SURFACE_CNTL            0x0b00
#define R350_SURFACE0_LOWER_BOUND    0x0b04
#define R350_SURFACE0_UPPER_BOUND    0x0b08
#define R350_SURFACE0_INFO           0x0b0c
#define R350_SURFACE_STRIDE          0x10
#define R350_SURFACE7_INFO           0x0b7c
#define R350_RBBM_STATUS             0x0e40
#define R350_SCRATCH_REG_BASE        0x15e0
#define R350_SCRATCH_REG_LAST        0x15f4
#define R350_ISYNC_CNTL              0x1724
#define R350_RB3D_ZCACHE_CTLSTAT     0x3254
#define R350_RB3D_DSTCACHE_CTLSTAT   0x325c
#define R350_RB2D_DSTCACHE_CTLSTAT   0x342c
/* MM_INDEX: bit 31 routes MM_DATA to the frame buffer aperture */
#define R350_MM_APER                 (1u << 31)
/* SURFACE_CNTL / SURFACEn_INFO byte swappers */
#define R350_NONSURF_AP0_SWP_16BPP   (1u << 20)
#define R350_NONSURF_AP0_SWP_32BPP   (1u << 21)
#define R350_NONSURF_AP1_SWP_16BPP   (1u << 22)
#define R350_NONSURF_AP1_SWP_32BPP   (1u << 23)
/* CP_RB_CNTL */
#define R350_RB_BUFSZ_MASK           0x3f       /* log2(ring size in qwords) */
#define R350_BUF_SWAP_MASK           (3u << 16)
#define R350_BUF_SWAP_32BIT          (2u << 16)
#define R350_RB_NO_UPDATE            (1u << 27)
#define R350_RB_RPTR_WR_ENA          (1u << 31)
/* RBBM_STATUS */
#define R350_RBBM_FIFOCNT_MASK       0x7f
#define R350_RBBM_ACTIVE             (1u << 31)
/* MEM_CNTL: R300_MEM_NUM_CHANNELS: 0 = 64-bit, 1 = 128-bit, 2 = 256-bit */
#define R350_MEM_NUM_CHANNELS_256    2
/* P2PLL (CRTC2 pixel PLL) indices, same ATOMIC_UPDATE handshake as PPLL */
#define R350_PLL_P2PLL_REF_DIV       0x2b
#define R350_PLL_P2PLL_DIV_0         0x2c

#endif /* ATI_R350_REGS_H */

/* R300 3D engine (used by OS X's accelerator for all its blits) */
#define R300_PM4_OPCODE_NOP3          0x10
/*
 * Clears of the compression side-structures: the Z mask, the
 * hierarchical-Z buffer and the colour compression mask. This model
 * stores colour and depth uncompressed, so the structures they reset
 * do not exist here and discarding the packets is the whole of the
 * correct behaviour -- but they are named rather than left to the
 * unknown-opcode arm so they do not show up as gaps. Chess.app clears
 * CMASK twice per new game.
 */
#define R300_PM4_OPCODE_CLEAR_ZMASK   0x32
#define R300_PM4_OPCODE_CLEAR_HIZ     0x37
#define R300_PM4_OPCODE_CLEAR_CMASK   0x38
#define R300_PM4_OPCODE_LOAD_VBPNTR   0x2f
#define R300_PM4_OPCODE_INDX_BUFFER   0x33
#define R300_PM4_OPCODE_DRAW_VBUF_2   0x34
#define R300_PM4_OPCODE_DRAW_IMMD_2   0x35
#define R300_PM4_OPCODE_DRAW_INDX_2   0x36
#define R300_VAP_VTX_SIZE             0x20b4
#define R300_TX_ENABLE                0x4104
#define R300_TX_FORMAT0_0             0x4480
#define R300_TX_FORMAT1_0             0x44c0
#define R300_TX_FORMAT2_0             0x4500
/*
 * TX_FORMAT0 bit 31 says whether TX_FORMAT2 carries the row pitch at
 * all. With it clear the pitch register is not in use and the rows are
 * as wide as the texture, whatever TX_FORMAT2 happens to hold -- and
 * what it holds is then stale: Chess.app leaves 16383 in it while
 * sampling a 128-pixel-wide texture, which taken literally puts every
 * row 64KB apart and samples each one from unrelated memory. That is
 * the horizontal banding its window renders as.
 */
#define R300_TX_PITCH_EN              (1u << 31)
#define R300_TX_OFFSET_0              0x4540
/*
 * Colour and alpha blend control. The two registers share the factor
 * and combine fields; only CBLEND carries the enables. Mac OS X sets
 * SEPARATE_ALPHA on every blended draw, so ABLEND -- not CBLEND --
 * decides what lands in the destination's alpha byte.
 */
#define R300_RB3D_BLENDCNTL           0x4e04
#define R300_RB3D_ABLENDCNTL          0x4e08
#define R300_BLEND_ENABLE             (1u << 0)
#define R300_BLEND_SEPARATE_ALPHA     (1u << 1)
#define R300_BLEND_READ_ENABLE        (1u << 2)
#define R300_BLEND_DISCARD_SHIFT      3     /* [5:3], see the enum below */
#define R300_BLEND_COMB_FCN_SHIFT     12    /* [14:12] add/sub/min/max */
#define R300_BLEND_SRC_SHIFT          16    /* [21:16] 6-bit factor code */
#define R300_BLEND_DST_SHIFT          24    /* [29:24] */
#define R300_BLEND_FACTOR_MASK        0x3f
/* constant operand for factor codes 43-46 */
#define R300_RB3D_BLEND_COLOR         0x4e10
#define R300_RB3D_COLOROFFSET0        0x4e28
#define R300_RB3D_COLORPITCH0         0x4e38
#define R300_PFS_PARAM_0_X            0x4c00
#define R300_GA_POINT_S0              0x4200
#define R300_GA_POINT_T0              0x4204
#define R300_GA_POINT_S1              0x4208
#define R300_GA_POINT_T1              0x420c
#define R300_RE_POINTSIZE             0x421c
#define R300_VAP_VTX_AOS_CNT          0x20c0
#define R300_VAP_VTX_AOS_CTL          0x20c4
#define R300_VAP_VTX_AOS_ADDR0        0x20c8
#define R300_VAP_VTX_AOS_ADDR1        0x20cc
/*
 * Vertex-program RAM is one flat vec4-indexed address space selected
 * by UPLOAD_ADDRESS, with UPLOAD_DATA writing dwords that follow on:
 * slots below CONST_START are program instructions, four dwords each,
 * and slots from CONST_START up are the constant file. Address 1030
 * (POINT_VPORT_SCALE_OFFSET) is a separate point-size vector, not part
 * of either.
 */
#define R300_VAP_PVS_UPLOAD_ADDRESS   0x2200
#define R300_VAP_PVS_UPLOAD_DATA      0x2208
#define R300_PVS_CONST_START          512
#define R300_PVS_MAX_CODE_DWORDS      (256 * 4)
#define R300_PVS_MAX_CONST_DWORDS     (256 * 4)
#define R300_VAP_CNTL_STATUS          0x2140
#define R300_VAP_PVS_BYPASS           0x00000100
/*
 * What the vertex stage emits, and therefore which program output
 * index carries what: out[0] is the position, then one output per
 * colour the format declares, then the texture coordinates. So the
 * first texcoord output is 1 + (number of colours present), and the
 * rasterizer's texture interpolator reads that one.
 */
#define R300_VAP_OUTPUT_VTX_FMT_0     0x2090
#define R300_VAP_OUT_POS_PRESENT      (1u << 0)
#define R300_VAP_OUT_COLOR_SHIFT      1      /* four colour-present bits */
#define R300_VAP_OUT_COLOR_COUNT      4
#define R300_VAP_OUTPUT_VTX_FMT_1     0x2094
#define R300_VAP_OUT_TEX0_COMP_MASK   0x7    /* components of texcoord 0 */
/* program bounds: first/last instruction slot, 10 bits each */
#define R300_VAP_PVS_CODE_CNTL_0      0x22d0
#define R300_PVS_FIRST_INST_SHIFT     0
#define R300_PVS_LAST_INST_SHIFT      20
#define R300_PVS_INST_MASK            0x3ff
/* constant file base, added to every const[] index a program names */
#define R300_VAP_PVS_CONST_CNTL       0x22d4
#define R300_PVS_CONST_BASE_MASK      0xffff

/*
 * PVS instruction encoding (4 dwords). Word 0 is the opcode and
 * destination; words 1-3 are the three source operands.
 */
#define R300_PVS_DST_OPCODE_MASK      0x3f
#define R300_PVS_DST_MATH_INST        (1u << 6)
#define R300_PVS_DST_MACRO_INST       (1u << 7)
#define R300_PVS_DST_REG_TYPE_SHIFT   8
#define R300_PVS_DST_REG_TYPE_MASK    0xf
#define R300_PVS_DST_OFFSET_SHIFT     13
#define R300_PVS_DST_OFFSET_MASK      0x7f
#define R300_PVS_DST_WE_SHIFT         20      /* four write-enable bits */
#define R300_PVS_DST_VE_SAT           (1u << 24)
#define R300_PVS_DST_ME_SAT           (1u << 25)

#define R300_PVS_SRC_REG_TYPE_MASK    0x3
#define R300_PVS_SRC_ABS              (1u << 3)
#define R300_PVS_SRC_OFFSET_SHIFT     5
#define R300_PVS_SRC_OFFSET_MASK      0xff
#define R300_PVS_SRC_SWIZZLE_SHIFT    13      /* four 3-bit selectors */
#define R300_PVS_SRC_SWIZZLE_MASK     0x7
#define R300_PVS_SRC_MODIFIER_SHIFT   25      /* four per-channel negates */

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

/* vector-engine opcodes (word 0 without R300_PVS_DST_MATH_INST) */
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

/* math-engine opcodes (scalar, replicated across the write mask) */
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
#define R300_ME_SIN                   16
#define R300_ME_COS                   17
#define R300_SE_VPORT_XSCALE          0x1d98
#define R300_SC_SCISSOR0              0x43e0
#define R300_SC_SCISSOR1              0x43e4
#define R300_SCISSOR_OFFSET           1440
#define R300_FG_ALPHA_FUNC            0x4bd4
#define R300_RE_CLIPRECT_TL_0         0x43b0
#define R300_RE_CLIPRECT_CNTL         0x43d0
#define R300_TX_FILTER0_0             0x4400
