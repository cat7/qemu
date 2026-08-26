/*
 * QEMU ATI Radeon 9800 Pro (R350) emulation
 *
 * Models the ATI Radeon 9800 Pro Mac Edition (PCI vendor 0x1002, device
 * 0x4E48) as an AGP add-in graphics card for the mac99 (PowerMac3,4)
 * machine. Forked from our ati_rage128 device: the Radeon keeps the Rage
 * 128 register lineage for the display controller (CRTC/DAC/palette/
 * cursor/I2C/GPIO), the 2D GUI engine (0x14xx), the scratch registers
 * and the PM4 command-stream format, so all of that is inherited; what
 * changes is the identity, the aperture layout (128MB frame buffer
 * BAR, 64KB register BAR), the memory-controller address windows
 * (MC_FB_LOCATION / MC_AGP_LOCATION -- engine and CP addresses are
 * card addresses, not frame-buffer offsets), the CP ring (CP_RB_*,
 * read-pointer and scratch write-back), RBBM_STATUS, the SURFACE_CNTL
 * byte swappers that replace CONFIG_CNTL's aperture endian modes, and
 * the R300 3D engine (not modeled: packets are parsed and skipped).
 *
 * Ground truth: the retail 9800 Pro Mac SE / OEM 9800 XT FCode ROMs
 * (detokenized 2026-08-24), Mac OS X 10.4.11's ATIRadeon9700.kext, the
 * Linux radeon DRM and X.org radeon drivers, and AMD's public R3xx
 * register references.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_R350_INT_H
#define ATI_R350_INT_H

#include "hw/pci/pci_device.h"
#include "hw/display/edid.h"
#include "qemu/bitmap.h"
#include "hw/i2c/bitbang_i2c.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ati_r350_pvs.h"
#include "ati_r350_us.h"
#include "ati_r350_cap.h"
#include "ati_r350_gl.h"

#define PCI_VENDOR_ID_ATI              0x1002
/*
 * 0x4E48 = R350 "Radeon 9800 Pro". The PCIR header of every Mac Edition
 * 9800 ROM we have (retail 9800 Pro SE 1.03/1.26 and Apple's OEM 9800
 * XT 1.18/1.23) declares this ID -- even the XT, whose silicon is an
 * R360 (0x4E4A): Mac OS X's ATIRadeon9700.kext matches 0x4E48 but not
 * 0x4E4A, so Apple's cards are strapped to the Pro's identity. Open
 * Firmware binds the ROM's FCode only on an exact vendor:device match.
 */
#define PCI_DEVICE_ID_ATI_R350         0x4e48

#define TYPE_ATI_R350 "ati-radeon9800"
OBJECT_DECLARE_SIMPLE_TYPE(ATIR350State, ATI_R350)

/*
 * Real Rage 128 BAR layout (confirmed by both the FCode's own mapping
 * words and real-hardware lspci dumps of Rage 128 cards):
 *   BAR0: 64MB memory, framebuffer aperture (prefetchable on real hw)
 *   BAR1: 256 bytes I/O, register access
 *   BAR2: 16KB memory, register aperture
 *   Expansion ROM: 128KB
 */
#define ATI_R350_APER_SIZE   (128 * 1024 * 1024)
/*
 * 128MB: the retail 9800 Pro Mac Edition's memory (the "SE" and the
 * OEM XT carry 256MB). The whole BAR0 aperture is the frame buffer --
 * unlike the Rage 128 there is no second aperture image; the Radeon's
 * byte swappers are SURFACE_CNTL's non-surface/surface swap bits.
 * Quartz Extreme needs >= 16MB, Quartz 2D Extreme >= 64MB.
 */
#define ATI_R350_VRAM_SIZE   (128 * 1024 * 1024)
#define ATI_R350_MMIO_SIZE   (64 * 1024)
#define ATI_R350_IO_SIZE     256
#define ATI_R350_NUM_REGS    (ATI_R350_MMIO_SIZE / 4)
#define ATI_R350_NUM_PLLS    64
#define ATI_R350_FB_SCAN_BLOCK (64 * 1024)

/*
 * Kinds of "the hardware does this, we do not" gap the engines report
 * through ati_r350_note_gap(). Each kind indexes its counters by the
 * field value itself (an opcode, a format code), so the tally names
 * exactly what a guest asked for and never got.
 */
typedef enum ATIR350GapKind {
    R350_GAP_P3_OPCODE,      /* packet3 opcode the parser discards */
    R350_GAP_PRIM,           /* VAP_VF_CNTL primitive type */
    R350_GAP_VTX_WALK,       /* VAP_VF_CNTL vertex walk mode */
    R350_GAP_TEX_FORMAT,     /* TX_FORMAT1 texel format code */
    R350_GAP_BLEND_FACTOR,   /* RB3D_BLENDCNTL src/dst factor code */
    R350_GAP_VTX_PROGRAM,    /* draw needs a vertex program we don't run */
    R350_GAP_DEST_OFF_VRAM,  /* work discarded: destination outside VRAM */
    R350_GAP_VS_VECTOR_OP,   /* PVS vector-engine opcode not implemented */
    R350_GAP_VS_MATH_OP,     /* PVS math-engine opcode not implemented */
    R350_GAP_VS_DST_FILE,    /* PVS destination register file not modelled */
    R350_GAP_TEX_SWIZZLE,    /* TX_FORMAT1 component select not modelled */
    R350_GAP_AOS_ARRAYS,     /* more vertex arrays bound than we fetch */
    R350_GAP_VTE_FMT,        /* VAP_VTE_CNTL vertex format bit not modelled */
    R350_GAP_FS_RGB_OP,      /* US colour-side opcode not implemented */
    R350_GAP_FS_ALPHA_OP,    /* US alpha-side opcode not implemented */
    R350_GAP_FS_TEX_OP,      /* US texture instruction not implemented */
    R350_GAP_FS_INDIRECT,    /* US_CONFIG names an indirection level */
    R350_GAP_FS_RS_ROUTE,    /* rasterizer routes something we do not emit */
    R350_GAP_FS_OUT_FMT,     /* US_OUT_FMT_0 component select not modelled */
    R350_GAP_MAX
} ATIR350GapKind;

#define R350_GAP_SLOTS 256

/*
 * How the "gl" property is set: whether the host-GPU backend renders a
 * draw, and whether the software rasterizer runs alongside it as a
 * check. `off` is the default and is a provable no-op -- the draw path
 * tests one NULL pointer and calls exactly what it called before.
 */
typedef enum ATIR350GlMode {
    R350_GL_OFF = 0,
    R350_GL_ON,             /* GL renders what it can; the rest falls back */
    R350_GL_VERIFY          /* both run, they are diffed, software wins */
} ATIR350GlMode;

/*
 * Why a draw the GL backend was offered went to the software rasterizer
 * instead. Counted per primitive type, the same shape as the gap
 * tracker, so `qom-get <device> gl` says how much of a session the
 * offload actually covered rather than leaving it to be inferred.
 */
typedef enum ATIR350GlFallback {
    R350_GLF_PRIM,          /* point sprites, lines: not assembled here */
    R350_GLF_RESOLVE,       /* AA resolve: the colour buffer is the source */
    R350_GLF_RECT,          /* no drawable destination rectangle */
    R350_GLF_ALIGN,         /* destination offset or pitch not dword-aligned */
    R350_GLF_VRAMEND,       /* rectangle runs off the end of VRAM */
    R350_GLF_XOR,           /* aperture swapper not uniform over the rect */
    R350_GLF_CLIPRULE,      /* a genuine 4-rect cliprect truth table */
    R350_GLF_TEXTURE,       /* texture too large to decode per draw */
    R350_GLF_SELFBLEND,     /* self-overlapping blend needing too many passes */
    R350_GLF_SURFACE,       /* target too large, or it would not resize */
    R350_GLF_BACKEND,       /* the backend itself declined the request */
    R350_GLF_FSPROG,        /* fragment program the translator refused */
    R350_GLF_MAX
} ATIR350GlFallback;

/*
 * Which coherency hook gave the resident render target back. The rules
 * are stated at "GL-OWNED RENDER TARGET" in ati_r350_3d.c; this counts
 * how often each of them actually fires and how many pixels it moved,
 * because "the target is not staying resident" is a symptom whose cure
 * depends entirely on WHICH rule is ending it. Read the tally from
 * `gl-stats`.
 */
typedef enum ATIR350GlRel {
    R350_GLR_SCANOUT,       /* the display refresh, and the cursor's timer */
    R350_GLR_RING,          /* the end of a command-processor ring run */
    R350_GLR_IB,            /* the end of an indirect buffer */
    R350_GLR_FIFO,          /* a CP FIFO push through the register aperture */
    R350_GLR_READ,          /* a named-range reader, ati_r350_gl_sync() */
    R350_GLR_2D,            /* the 2D scaler, and host-data pushes */
    R350_GLR_TARGET,        /* a draw bound a different colour buffer */
    R350_GLR_FALLBACK,      /* a draw the offload handed back */
    R350_GLR_BACKEND,       /* the backend declined, mid-draw */
    R350_GLR_RESET,         /* reset, unrealize */
    R350_GLR_MAX
} ATIR350GlRel;

/*
 * How long a DECODED TEXTURE may live. The default states the rule the
 * decode actually depends on -- see r300_gl_tex_current() -- and the
 * other two exist to measure it:
 *
 *   burst   the M3 lifetime: every release drops the whole cache. Safe,
 *           and on Flurry it left a 0.6% hit rate. Kept as the A/B arm.
 *   dirty   the default: an entry lives while the VRAM it was decoded
 *           from is unwritten, which is what correctness requires.
 *   never   deliberately WRONG -- entries are never invalidated at all.
 *           It exists so the guard can be shown to bite: a gl=verify
 *           run in this mode must FAIL. Never a shipping configuration.
 */
typedef enum ATIR350GlTexLife {
    R350_TEXLIFE_DIRTY,
    R350_TEXLIFE_BURST,
    R350_TEXLIFE_NEVER,
} ATIR350GlTexLife;

/*
 * A blended draw whose own primitives overlap is rendered in several
 * ordered passes rather than handed back to the software rasterizer
 * (milestone M3; see r300_gl_passes()). These bound the partition: a
 * draw needing more triangles or more passes than this falls back and
 * is counted, so the limits are visible in `gl-stats` rather than
 * silently shaping what the offload covers.
 */
#define R300_GL_TRI_MAX     512
#define R300_GL_PASS_MAX    128

/*
 * How many decoded textures are kept, and the largest one kept. Eight
 * entries of at most a megabyte of RGBA each: enough for a compositor
 * frame's window tiles, and bounded so a guest cannot make the device
 * allocate without limit.
 */
#define R300_GL_TEXCACHE     R350_GL_TEXSLOTS
#define R300_GL_TEXCACHE_MAX (256 * 1024)

/*
 * The guard on a cached entry (see r300_gl_tex_current()) walks the
 * host pages its VRAM range covers. A texture spanning more pages than
 * this is decoded into the scratch buffer and not cached, so that the
 * walk stays a bounded cost per draw -- 4 MB at a 4 KB page, where the
 * cache's own texel limit is a quarter of that.
 */
#define R300_GL_DIRTY_PAGES  1024

typedef struct ATIR350PM4Parser {
    uint32_t remaining;      /* data dwords still expected */
    uint32_t type;           /* packet type of the in-flight packet */
    uint32_t reg;            /* running register offset, packet0 */
    bool one_reg;
    uint32_t p1_reg1;        /* packet1's two register offsets */
    uint32_t p1_reg2;
    uint32_t p3_opcode;      /* packet3 2D-draw sub-state */
    uint32_t p3_params[8];
    uint32_t p3_scale[16];      /* R350_SCALE_PKT_DWORDS */
    uint32_t p3_param_idx;
    uint32_t p3_total;       /* payload dwords the packet3 declared */
} ATIR350PM4Parser;

typedef struct ATIR350Mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bytes per pixel */
    uint32_t fb_offset;  /* byte offset into VRAM */
    uint32_t pix_width;  /* raw CRTC_PIX_WIDTH field, for draw dispatch */
} ATIR350Mode;

struct ATIR350State {
    PCIDevice parent_obj;

    MemoryRegion aper;        /* BAR0: 64MB aperture container */
    MemoryRegion vram;        /* 16MB of real VRAM at aperture offset 0 */
    MemoryRegion mmio;        /* BAR2: 16KB register file */
    MemoryRegion io;          /* BAR1: 256-byte I/O register window */
    QemuConsole *con;

    uint32_t regs[ATI_R350_NUM_REGS];
    uint32_t plls[ATI_R350_NUM_PLLS];
    /*
     * Memo for ati_r350_vram_xor(). Resolving the swapper means walking
     * eight surface descriptors, three registers each, and the software
     * rasterizer asks two or three times for every pixel it touches --
     * which is why the surface walk sat second and third in a profile of
     * a stalled guest. `swap_lo`..`swap_hi` is the offset range over
     * which the walk provably cannot give a different answer than
     * `swap_val`, so a hit costs two comparisons. Any write to a surface
     * register clears `swap_valid`.
     */
    uint32_t swap_lo, swap_hi;
    unsigned swap_val;
    bool swap_valid;
    /* R300 memory-controller indirect register file (MC_IND_INDEX/DATA) */
    uint32_t mc_ind[256];
    /*
     * Bit-banged DDC on the Radeon's dedicated GPIO_VGA_DDC / GPIO_DVI_DDC
     * pads (same A/Y/EN lane layout as GPIO_MONID), each serving the EDID.
     */
    bitbang_i2c_interface vga_ddc_i2c;
    bitbang_i2c_interface dvi_ddc_i2c;
    int vga_ddc_sda;
    int dvi_ddc_sda;

    /* DAC palette state (PALETTE_INDEX/PALETTE_DATA) */
    uint8_t dac_wr_index;
    uint8_t dac_rd_index;
    uint8_t palette[256][3];

    ATIR350Mode mode;      /* what the last refresh actually drew */
    /*
     * The last mode CRTC1 itself described while valid. Kept apart from
     * `mode`: when the auto-detected framebuffer overrides the CRTC, `mode`
     * holds the guess, and using it as the "remembered CRTC mode" fallback
     * made the override compare the guess against itself and stick for
     * good (seen live: OS X blanked the display for sleep, the heuristic
     * swapped in a stale 8bpp 800x600 buffer, and it never let go).
     */
    ATIR350Mode crtc_mode;
    bool mode_dirty;
    bool have_valid_mode; /* has `crtc_mode` ever held a real, valid mode? */

    /*
     * Auto-detected framebuffer, tracked via VRAM write activity
     * rather than any register: neither real guest OS this device has
     * been tested against (Mac OS X 10.2, Mac OS 9.2) ever programs
     * CRTC1's "Extended" mode-set registers at all -- confirmed live,
     * see the comment on ati_r350_scan_vram_activity() -- so the
     * *actual* live framebuffer, whenever it isn't the one CRTC1's
     * last-known-good mode already correctly describes, has to be
     * found some other way. `fb_scan_activity` is a decayed
     * hit-counter per ATI_R350_FB_SCAN_BLOCK-sized block of VRAM;
     * a sustained run of "recently written every scan" blocks is
     * treated as the real, currently-live framebuffer.
     */
    uint8_t fb_scan_activity[ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK];
    uint32_t fb_scan_counter;
    /*
     * Dirty VRAM blocks seen by the per-refresh dirty snapshot since the
     * activity scan last consumed them (the snapshot clears the bitmap, so
     * there is exactly one consumer of it -- ati_r350_update_display --
     * and the slower activity scan reads this instead).
     */
    bool fb_block_pending[ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK];
    /* redraw the whole surface next pass regardless of dirty state */
    bool force_redraw;
    bool auto_fb_valid;
    ATIR350Mode auto_fb_mode;
    /*
     * Adoption is gated on a candidate staying identical across two
     * consecutive scans: while a slow canvas paint is still in
     * progress the partial region matches a different (wrong)
     * resolution each scan, and after painting stops the region's
     * activity credit drains slice-by-slice in write order, shrinking
     * the run through more wrong intermediate shapes -- both churn
     * states never repeat the same candidate twice in a row, so the
     * stability gate suppresses them and only the settled, fully
     * painted region is ever adopted (verified in the qtest harness:
     * without this gate a 6-slice gradual paint visibly cycles the
     * display through 4+ wrong modes and can END on one).
     */
    bool auto_fb_pending_valid;

    /*
     * Apple Monitor Sense: report a connected MultiScan display on
     * the MONID sense pins (default on); off = nothing plugged in
     * (all sense lines float high).
     */
    bool monitor_connected;
    /*
     * Dual-head card (DVI-I + ADC). The DVI head always has the display;
     * this says whether the second head (ADC/VGA connector: its DDC pad,
     * the primary DAC's comparator and the Apple-sense lines) reports a
     * connected display too -- off by default so Mac OS sees one screen.
     */
    bool second_display;
    ATIR350Mode auto_fb_pending;

    /*
     * Same VBL model as the mach64 device: a gated pulse -- raise the
     * VBLANK status bit (and the PCI interrupt, when enabled) at each
     * frame's blank phase, drop the line a blank-length later. See
     * ati_mach64.c for why the free-running and held-until-ack
     * variants both wedged real ROM boots on that device.
     */
    QEMUTimer *vblank_timer;
    QEMUTimer *vblank_end_timer;
    /* coalesces a burst of cursor-register writes into one host update */
    QEMUTimer *cursor_timer;

    /*
     * Hardware I2C engine (I2C_CNTL_0/1 + I2C_DATA) serving a
     * generated EDID at DDC address 0xA0/0xA1 -- the Rage 128 has a
     * real I2C controller (unlike the mach64's bit-banged GP_IO pins)
     * and ATI's Mac FCode/NDRV use it for monitor detection.
     */
    qemu_edid_info edid_info;
    uint8_t edid[128];
    uint8_t i2c_offset;      /* current EDID read offset */
    uint8_t i2c_data_fifo[16];
    int i2c_data_len;
    int i2c_data_pos;

    /*
     * Second DDC path: the ATI Mac drivers (Mac OS X's ndrv on mac99,
     * and -- observed live on g3beige -- the classic Mac OS 9 driver
     * and the card FCode too) bit-bang I2C on the GPIO_MONID pads
     * (SDA = pad 0, SCL = pad 1), marking them with MASK nibble 0xf --
     * distinct from the Apple-sense probes (MASK 0x7) and the FCode's
     * EN-only convention (MASK 0). Serves the same 128-byte EDID as
     * the hardware engine above.
     */
    bool host_cursor_published;   /* synthetic arrow handed to the UI */
    bitbang_i2c_interface monid_i2c;
    int monid_sda;           /* live SDA level fed back into MONID_Y */
    /*
     * A bit-banged I2C session on pads 1 (SDA) / 2 (SCL) under the
     * Apple-sense MASK nibble 0x7 -- what Mac OS 9's "ATI Resource
     * Manager" does. Told apart from a sense probe by its clock: a
     * START (SDA falling while SCL is released) followed by SCL being
     * driven low opens the session; SDA rising while SCL is released
     * (STOP) closes it. A sense probe pulses one pad and never clocks.
     */
    bool monid7_i2c;         /* session open: pads answer as an I2C bus */
    bool monid7_start;       /* START seen, waiting for the first clock */
    bool monid7_sda_low;     /* pad 1 currently driven low */
    bool monid7_scl_low;     /* pad 2 currently driven low */
    uint32_t ddc1_pos;       /* DDC1 EDID bitstream position (in bits) */
    uint32_t ddc1_half;      /* half-bit phase: 2 manual VSYNC pulses/bit */

    /*
     * PM4/CCE GUI command-FIFO ring buffer -- the mechanism the real
     * Mac driver actually uses for register/2D submission on this
     * card (distinct from, and used after, the older one-shot
     * BM_GUI_TABLE descriptor engine). Register offsets and the ring
     * living in VRAM (this PCI, non-AGP variant has no GART/system-
     * memory command access) are cross-verified between a live trace
     * of the real driver and independent reference source; see the
     * comment on R350_PM4_BUFFER_OFFSET in ati_r350_regs.h.
     * Consumed synchronously on each PM4_BUFFER_DL_WPTR write, same
     * style as the BM engine.
     */
    uint32_t pm4_rptr;         /* ring read pointer, in dwords */
    uint32_t pm4_wptr;         /* ring write pointer, in dwords */
    /* ring base: VRAM offset, or GART offset when AGP_OFFSET_FLAG set */
    uint32_t pm4_buffer_addr;
    uint32_t pm4_buffer_cntl;
    uint32_t pm4_ring_dwords;  /* ring size decoded from CNTL, 0 = no ring */

    /*
     * CCE microcode store (PM4_MICROCODE_ADDR/RADDR/DATAH/DATAL):
     * 256 x 64-bit words, streamed as high/low pairs with the address
     * auto-incrementing after each DATAL access -- kept so a driver
     * that reads its upload back to verify sees exactly what it wrote.
     */
    uint32_t pm4_microcode[256][2];
    uint16_t pm4_ucode_waddr;
    uint16_t pm4_ucode_raddr;

    /*
     * PIO alternative to the ring: the real Mac driver actually
     * submits its command stream by writing raw dwords straight to
     * PM4_FIFO_DATA_EVEN/ODD (0x1000/0x1004, undocumented in the OEM
     * manual but confirmed live: consecutive writes there decode as
     * an ordinary PM4 packet stream, ending in a packet0 write of the
     * exact 64-bit fence value the driver then polls for at
     * GUI_SCRATCH_REG0/1). Both addresses behave identically -- they
     * just feed the next dword into this same parser state machine.
     */
    /*
     * One packet-parser state per independent command stream: the PIO
     * FIFO stream gets a persistent one (writes arrive one dword at a
     * time across many MMIO accesses), while each indirect-buffer
     * dispatch runs a fresh private instance -- an IB dispatch is
     * triggered from *inside* a FIFO packet (packet0 hitting
     * IW_INDOFF/INDSIZE), so sharing the parser state would corrupt
     * the framing of both streams (a real bug: it desynced the FIFO
     * stream after the first IB and wedged the guest driver).
     */
    ATIR350PM4Parser pm4_fifo;
    ATIR350PM4Parser pm4_ring;

    /* unimplemented-command tally -- see ati_r350_note_gap() */
    uint32_t gap_count[R350_GAP_MAX][R350_GAP_SLOTS];

    /*
     * 2D GUI (destination datapath) engine state -- ported from the
     * real upstream `ati-vga` device (hw/display/ati.c/ati_2d.c), not
     * generic/free-standing; register semantics are documented on the
     * offsets themselves in ati_r350_regs.h. These mirror upstream's
     * ATIVGARegs 2D fields one-to-one so the ported blt logic needs no
     * renaming.
     */
    uint32_t dst_offset, dst_pitch, dst_tile, dst_width, dst_height;
    uint32_t dst_x, dst_y;
    uint32_t src_offset, src_pitch, src_tile, src_x, src_y;
    uint32_t dp_gui_master_cntl;
    uint32_t dp_brush_bkgd_clr, dp_brush_frgd_clr;
    uint32_t dp_src_frgd_clr, dp_src_bkgd_clr;
    /*
     * Scissors are SIGNED 14-bit fields -- the register guide gives the
     * range as -8192..8191 ("Destination left scissor", RAGE 128 VR/GL
     * Register Reference Manual 7.6). Holding them unsigned turned a
     * legitimately negative left/top edge -- what the driver programs
     * for a window hanging off the left or top of the screen -- into a
     * huge positive one, which clips the whole drawing away.
     */
    int32_t sc_top, sc_left, sc_bottom, sc_right;
    int32_t src_sc_bottom, src_sc_right;
    uint32_t dp_cntl, dp_datatype, dp_mix, dp_write_mask;
    uint32_t default_offset, default_pitch;
    int32_t default_sc_bottom, default_sc_right;

    /*
     * The pitch/offset and scissor fields above are the EFFECTIVE values
     * the drawing code uses; these are the register values they are
     * derived from. DP_GUI_MASTER_CNTL's SRC/DST_PITCH_OFFSET_CNTL and
     * SRC/DST_CLIPPING bits select, per operation, whether the effective
     * value comes from the source/destination registers or from the
     * DEFAULT_* ones -- a selection, not a transfer. Folding it straight
     * into the effective field on every GMC write (which is what this
     * used to do) destroys the register behind it, so the result depends
     * on the order the driver happens to write things in. Mac OS X hits
     * exactly that: it programs the separate SRC_PITCH/SRC_OFFSET
     * registers rather than the packed SRC_PITCH_OFFSET, and every
     * PAINT/BITBLT packet carries its own GMC dword, so a blit could run
     * with a pitch left over from an earlier, differently sized surface.
     * One 8-pixel pitch unit of staleness shears the copy by 8 pixels per
     * row -- the diagonally streaked window contents on the Rage.
     */
    uint32_t src_offset_reg, src_pitch_reg, src_tile_reg;
    uint32_t dst_offset_reg, dst_pitch_reg, dst_tile_reg;
    /*
     * Radeon's standalone SRC_PITCH/DST_PITCH registers count BYTES;
     * the packed *_PITCH_OFFSET registers keep the Rage 128 8-pixel
     * unit. Track which flavour each side's live value came from so
     * the engine applies the right stride (OS X pages textures in
     * through byte-pitch blits; treating those as 8-pixel units
     * shredded every window into 32x-spaced slices).
     */
    bool src_pitch_bytes_reg, dst_pitch_bytes_reg;
    bool src_pitch_bytes, dst_pitch_bytes;
    int32_t sc_top_reg, sc_left_reg, sc_bottom_reg, sc_right_reg;
    int32_t src_sc_bottom_reg, src_sc_right_reg;

    /*
     * Diagnostic: CPU stores into the frame buffer go through aperture 0,
     * which is a plain RAM alias, so nothing in this device can see them
     * -- and that is exactly the path that fills the driver's offscreen
     * staging surface. Setting the "fillwatch"/"fillwatch-size" properties
     * lays an instrumented IO window over that range of aperture 0 so the
     * fills become visible. Writes are coalesced into contiguous runs and
     * traced one line per run, so a full surface fill costs a line per row
     * rather than one per store.
     */
    MemoryRegion vram_watch;
    uint32_t fillwatch_off, fillwatch_size;
    uint32_t fw_run_start, fw_run_end;
    bool fw_active;

    /*
     * Diagnostic: 3D draw capture. When the "draw-capture" property names
     * a file, every draw the rasterizer runs is also written there as a
     * self-contained record -- the resolved draw state, the transformed
     * vertices, the texture, and the destination rectangle before and
     * after the draw. `cap_fp` is NULL unless the property was given, and
     * that pointer is the only thing the draw path tests, so an unarmed
     * capture costs one predictable branch per draw and changes nothing.
     * See ati_r350_cap.h for the record format.
     */
    char *cap_path;
    FILE *cap_fp;
    bool cap_arm;               /* record at all: a settable QOM property */
    int draw_xr;                /* swapper xor the LAST redraw used */
    uint32_t cap_max;           /* records to take before closing the file */
    uint32_t cap_max_px;        /* skip a draw whose rectangle exceeds this */
    uint32_t cap_index;         /* records written so far */
    uint32_t cap_skipped;
    struct {
        uint32_t off, len, xr, hash, rec;
    } cap_tex[R350_CAP_TEX_CACHE];
    unsigned cap_tex_n;

    /*
     * Host-GPU offload (phase 2, milestone M2). `gl_path` is the "gl"
     * property as given; `gl_mode` is it parsed, and `gl_ctx` is NULL
     * unless a backend was actually opened -- that pointer is the only
     * thing the draw path tests, so a device left at the default is
     * untouched by any of this. See ati_r350_gl.h for the backend
     * interface and ati_r350_gl.c for the GL implementation of it.
     */
    char *gl_path;
    ATIR350GlMode gl_mode;
    struct R350GlCtx *gl_ctx;
    uint64_t gl_drawn;          /* draws the backend rendered */
    uint64_t gl_fb[R350_GLF_MAX][R350_GAP_SLOTS];   /* fallbacks, by prim */
    /* gl=verify: the per-pixel agreement between the two paths */
    uint64_t gl_v_px, gl_v_hist[4];     /* delta 0, 1, 2-4, above 4 */
    uint64_t gl_v_draws, gl_v_bad;      /* draws compared / with delta > 1 */
    uint64_t gl_v_cover_px, gl_v_cover;  /* coverage-class pixels / differing */
    uint64_t gl_v_mesh;         /* ... of those, from a multi-triangle draw */
    unsigned gl_v_max, gl_v_vmax;
    /*
     * Scratch for one request, grown on demand and never shrunk: a draw
     * needs the destination rectangle twice over in RGBA, the decoded
     * texture, and the expanded triangle list. Milestone M3 hands these
     * to a queue instead, which is why they are sized per request rather
     * than allocated inside the backend.
     */
    uint8_t *gl_before, *gl_out, *gl_sw, *gl_texbuf;
    size_t gl_rect_sz, gl_texbuf_sz;
    float *gl_verts;
    size_t gl_verts_sz;
    /*
     * The self-overlap partition's working set, sized once rather than
     * allocated per draw: which pass each triangle landed in, the
     * triangles in pass order, where each pass begins in the vertex
     * array, and the bounding boxes the pairwise test screens with.
     */
    uint8_t gl_pass[R300_GL_TRI_MAX];
    unsigned gl_order[R300_GL_TRI_MAX];
    unsigned gl_pass_first[R300_GL_PASS_MAX + 1];
    float gl_bbox[R300_GL_TRI_MAX][4];
    bool gl_fast;               /* gl=fast: allow the additive one-pass blend */
    uint64_t gl_addblend;       /* draws GL's own blender ordered */
    uint64_t gl_multipass;      /* draws that needed more than one pass */
    uint64_t gl_passes;         /* ... and how many passes in total */
    /*
     * The GL-owned render target (milestone M3). The rules that keep it
     * coherent with emulated VRAM are stated in full at "GL-OWNED RENDER
     * TARGET" in ati_r350_3d.c; these are the four facts they need.
     * `gl_res` says a target is resident at all and is the ONLY thing
     * the hot hooks test.
     */
    bool gl_res;
    uint32_t gl_res_off, gl_res_pitch;
    unsigned gl_res_xr;
    int gl_tex_w, gl_tex_h;             /* the backend texture's extent */
    int gl_vx0, gl_vy0, gl_vx1, gl_vy1; /* seeded: the GPU copy is good */
    int gl_dx0, gl_dy0, gl_dx1, gl_dy1; /* drawn: the GPU copy is NEWER */
    uint64_t gl_flushes;                /* fetches back into VRAM */
    uint64_t gl_flush_px, gl_seed_px;   /* ... and pixels moved each way */
    uint64_t gl_rel[R350_GLR_MAX];      /* which hook ended a residency */
    uint64_t gl_rel_px[R350_GLR_MAX];   /* ... and what it cost to */
    /*
     * Decoded textures, keyed on everything the decode depends on.
     *
     * An entry is valid while the VRAM range it was decoded from is
     * UNWRITTEN -- that, and not the render target's residency, is what
     * the decode depends on. `epoch` is the dirty-bitmap generation the
     * entry was last known clean in; see r300_gl_tex_current() for the
     * rule and its two enforcement points.
     */
    struct {
        uint32_t off, len, pitch;
        unsigned bpp, code, xr;
        unsigned sel[4];
        int w, h;
        uint8_t *rgba;
        size_t sz;
        uint64_t used;
        uint64_t epoch;             /* the bitmap generation it is clean in */
        unsigned npg;               /* host pages the range spans */
        bool live;                  /* the decoded bytes are current */
        bool up;                    /* ... and the backend has them too */
    } gl_tex[R300_GL_TEXCACHE];
    uint64_t gl_tex_seq, gl_tex_hit, gl_tex_miss;
    uint64_t gl_tex_stale;      /* entries the dirty guard killed */
    /*
     * Why the other lookups missed. A hit rate is only actionable with
     * these beside it: an entry refused admission, one a writer killed,
     * one a draw rendered over and one the LRU evicted are four
     * different problems.
     */
    uint64_t gl_tex_noadmit;    /* range was dirty: cannot be guarded */
    uint64_t gl_tex_wrote;      /* a writer hook killed it */
    uint64_t gl_tex_over;       /* a draw rendered into its range */
    uint64_t gl_tex_evict;      /* the LRU gave its slot away */
    uint64_t gl_epoch;          /* bumped whenever the VGA bitmap is cleared */
    unsigned gl_pgbits;         /* qemu_target_page_bits(), resolved once */
    ATIR350GlTexLife gl_texlife;
    char *gl_texlife_path;
    bool gl_tex_any;            /* any entry live: the hot hooks test this */

    /*
     * Hardware cursor (CUR_* registers). hw_cursor_on tracks whether the
     * guest's own cursor is live, so the host-driven fallback pointer
     * stands aside rather than fighting it for the console cursor.
     * hw_cursor_sum is a checksum of the published image, so a shape
     * change made by writing VRAM alone is still picked up without
     * re-uploading an unchanged cursor on every frame.
     */
    bool hw_cursor_on;
    uint32_t hw_cursor_sum;
    /*
     * CUR_LOCK is a single bit that merely appears in bit 31 of all three
     * of CUR_OFFSET / CUR_HORZ_VERT_POSN / CUR_HORZ_VERT_OFF (RRG 3-80):
     * the most recent write to any of them sets or clears it. Kept here
     * rather than in the regs[] copies, which are stored without it.
     */
    bool cur_lock;
    /* last position published to the console, so an unchanged cursor is
     * not re-published (and re-traced) on every refresh tick */
    int hw_cursor_x, hw_cursor_y;
    /* the auto-detected framebuffer is currently overriding the CRTC mode
     * (tracked so the transition can be traced, not every frame) */
    bool auto_fb_overriding;
    /*
     * When the mach64's host-side pointer tracking is driving this display
     * (its host-cursor-tracking property, which the Mac OS 9 launcher turns
     * on), it owns the console cursor for good and the guest's own hardware
     * cursor must stand aside -- under Mac OS 9 the guest barely updates the
     * CUR_* registers on this card, so publishing them leaves a pointer
     * frozen at a stale position. Under Mac OS X, where tracking is off and
     * the guest drives the registers properly, the hardware cursor wins.
     *
     * Ownership is a latch, NOT a timeout. Expiring it after a second of no
     * host updates meant that whenever the pointer sat still on the other
     * display the hardware cursor took the console back and redisplayed its
     * stale position -- a ghost pointer left behind on this screen, plus an
     * artefact as the handover happened on the way across.
     */
    bool host_cursor_active;

    /* HOST_DATA0-7/LAST accumulator, same protocol as upstream */
    bool host_data_active;
    uint32_t host_data_row, host_data_col, host_data_next;
    uint32_t host_data_acc[4];

    /*
     * Vertex-program RAM as uploaded through VAP_PVS_UPLOAD_ADDRESS/DATA.
     * It is one flat vector-indexed address space: vectors below
     * R300_PVS_CONST_START are program instructions, four dwords each,
     * and from there up they are the constant file. Neither is ever
     * cleared, so `pvs_code_slot_valid` remembers which instruction slots
     * the guest has actually written -- the control registers alone would
     * happily point at whatever a previous program left behind.
     */
    uint32_t pvs_upload_addr;
    uint32_t pvs_upload_cnt;
    uint32_t pvs_const[R300_PVS_CONST_SLOTS * 4];
    uint32_t pvs_const_dwords;
    uint32_t pvs_code[R300_PVS_CODE_SLOTS * 4];
    uint32_t pvs_code_slot_valid[R300_PVS_CODE_SLOTS / 32];
    /* dwords ever uploaded to the code region: is there a program at all */
    uint32_t pvs_code_dwords;

    /*
     * Phase 2, milestone M4: how much of what real guests upload the
     * GLSL translation in ati_r350_pvs_glsl.c can express. Armed by the
     * "pvs-glsl" property and otherwise never entered, because the
     * translation is not on any pixel's path -- the offline three-way
     * harness measures what it COMPUTES, and this measures what it
     * COVERS, which no corpus of seven programs can answer.
     *
     * A program is translated once, when the control registers first
     * name it: `pvs_tr_sig` is the range and constant base the last
     * attempt was made for, so a thousand draws of one program cost one
     * translation.
     */
    bool pvs_glsl;
    uint64_t pvs_tr_sig;
    uint64_t pvs_tr_ok, pvs_tr_refused;
    uint64_t pvs_tr_by_reason[3];       /* vector op, math op, dst file */
    uint32_t pvs_tr_last_bytes, pvs_tr_last_nconst;
    uint32_t pvs_tr_last_in, pvs_tr_last_out;

    /*
     * Phase 2, milestone M5: the fragment program in force, decoded once
     * per draw out of the US register banks. It lives here rather than in
     * the per-draw state because it is three kilobytes of decoded
     * instruction and because the GL backend keys its shader cache on it.
     * `us_sig` is the signature of the control words and instruction
     * dwords the decode was made from, so a thousand draws of one program
     * cost one decode.
     */
    R300UsProgram us_prog;
    uint64_t us_sig;
    uint64_t us_draws, us_refused;
    /*
     * The same program as GLSL, for the host-GPU backend, translated
     * whenever the decode is. `us_glsl_key` is what the backend caches
     * the linked shader under and is never zero for a usable program.
     * The constants are flattened per draw because they change without
     * the program changing.
     */
    char us_glsl[16 * 1024];
    bool us_glsl_ok;
    uint64_t us_glsl_key;
    uint64_t us_glsl_ok_n, us_glsl_refused_n;
    float us_konst_flat[R300_US_CONSTS * 4];

    /*
     * Staging buffer for an in-flight R300 3D_DRAW_IMMD_2 payload
     * (VAP_VF_CNTL + inline vertices). Scratch state only: a packet
     * split across a migration is lost, like the 2D host-data
     * accumulator above.
     */
    uint32_t r300_immd[16384];
};


/* ati_r350_dbg.c */
const char *ati_r350_reg_name(uint32_t base);

/*
 * Report a command the hardware understands and this model does not.
 * Warns on the first sight of each distinct value and counts every
 * one, so a silently dropped packet type shows up in the log the
 * first time a guest uses it instead of years later as a rendering
 * mystery. Packet3 opcode 0x1b -- the rectangle-only blit that moves
 * a window's body during a drag -- went unnoticed exactly that way:
 * its trace event existed but was never armed in any capture.
 * Read the running tally with `qom-get <device> gaps`.
 */
void ati_r350_note_gap(ATIR350State *s, ATIR350GapKind kind, unsigned idx);

/* sizes of the private draw-capture payload structs (ati_r350_cap.h) */
uint32_t ati_r350_cap_state_bytes(void);
uint32_t ati_r350_cap_vtx_bytes(void);

/* why a draw went to the software rasterizer instead of the GL backend */
const char *ati_r350_gl_fb_name(ATIR350GlFallback why);

/*
 * GL-OWNED RENDER TARGET -- the coherency hooks. The rules are stated
 * in full where they are implemented, at "GL-OWNED RENDER TARGET" in
 * ati_r350_3d.c; what follows is how the rest of the device obeys them.
 *
 * ati_r350_gl_release() gives the target back: anything the host GPU
 * holds is fetched into VRAM first, and the GPU copy stops being
 * trusted. Call it before ANY code that reads or writes VRAM outside
 * the 3D draw path, and at every boundary where the guest CPU might run
 * -- which in this device means the end of a command-processor run,
 * because a whole ring or indirect buffer is drained inside one guest
 * store and nothing else can interleave with it.
 *
 * ati_r350_gl_touch() is the same thing for code that names a range and
 * is hot enough to care: it costs one predictable branch when no target
 * is resident, which is always the case with the default gl=off.
 */
void ati_r350_gl_release(ATIR350State *s, ATIR350GlRel why);
void ati_r350_gl_sync(ATIR350State *s, uint32_t off, uint32_t len);
void ati_r350_gl_wrote(ATIR350State *s, uint32_t off, uint32_t len);
const char *ati_r350_gl_rel_name(ATIR350GlRel why);

/*
 * The display refresh has just snapshotted and CLEARED the VGA dirty
 * bitmap, which is the one thing the decoded-texture cache reads to
 * know whether the guest CPU wrote over a texture. Hand the snapshot
 * over before it is discarded: entries whose range it marks are killed,
 * and the rest are carried into the new generation.
 */
void ati_r350_gl_epoch(ATIR350State *s, DirtyBitmapSnapshot *snap);

/*
 * Take responsibility for the VGA dirty bits over a VRAM range the
 * decoded-texture cache wants to guard, feeding the scanout's own
 * accumulator first so nothing it uses is lost. False when the range
 * may not be claimed. Implemented in ati_r350.c, beside the scanout
 * code whose bits these are.
 */
bool ati_r350_gl_admit(ATIR350State *s, uint32_t off, uint32_t len);

/* something is about to READ this range of VRAM */
static inline void ati_r350_gl_touch(ATIR350State *s, uint32_t off,
                                     uint32_t len)
{
    if (unlikely(s->gl_res)) {
        ati_r350_gl_sync(s, off, len);
    }
}

/* ... and about to WRITE it, which also stales anything decoded from it */
static inline void ati_r350_gl_dirty(ATIR350State *s, uint32_t off,
                                     uint32_t len)
{
    if (unlikely(s->gl_res || s->gl_tex_any)) {
        ati_r350_gl_wrote(s, off, len);
    }
}

/* ati_r350_3d.c */
void ati_r350_r300_draw_immd(ATIR350State *s, const uint32_t *dw, unsigned n);
void ati_r350_r300_draw_vbuf(ATIR350State *s, uint32_t vf);

/* ati_r350.c MC-window translation, shared with the engines */
bool ati_r350_mc_to_vram(ATIR350State *s, uint32_t addr, uint32_t *off);
uint32_t ati_r350_mc_read32(ATIR350State *s, uint32_t addr);
/*
 * Trace helper: name the window a card address resolves through and
 * return the address it resolves to ("vram" -> a VRAM byte offset,
 * "agp" -> a bus address after AGP_BASE, "bus" -> passed through
 * untranslated). Read-only; it exists so a trace can record where a
 * fetch really went instead of leaving it to be inferred from
 * post-hoc register reads.
 */
const char *ati_r350_mc_describe(ATIR350State *s, uint32_t addr,
                                 uint64_t *target);

/* ati_r350_2d.c */
void ati_r350_2d_blt(ATIR350State *s);
void ati_r350_2d_scale(ATIR350State *s, const uint32_t *pkt);
void ati_r350_2d_scale_regs(ATIR350State *s);
bool ati_r350_host_data_flush(ATIR350State *s);

/*
 * Show/position a host-driven pointer on this card's console. Used by
 * the mach64's host-cursor-tracking workaround once the pointer crosses
 * onto this display: this device has no hardware-cursor emulation and
 * the guest never drives one here, so without it the pointer would
 * simply vanish on the second screen.
 */
void ati_r350_host_cursor(int x, int y, bool on);
/*
 * Byte-lane XOR that turns a raw VRAM byte offset into the byte a
 * little-endian consumer (CRTC, 2D engine, CP) sees there, given the
 * SURFACE_CNTL / SURFACEn swappers in force at that address: 0 = no
 * swap, 1 = 16-bit swap, 3 = 32-bit swap. See ati_r350_vram_xor().
 */
unsigned ati_r350_vram_xor(ATIR350State *s, uint32_t off);
uint32_t ati_r350_vram_ld32(ATIR350State *s, uint32_t off);

#endif /* ATI_R350_INT_H */
