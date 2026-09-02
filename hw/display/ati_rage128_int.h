/*
 * QEMU ATI Rage 128 Pro emulation
 *
 * Models the ATI Rage 128 Pro (PCI vendor 0x1002, device 0x5046 "PF")
 * as an add-in graphics card for the Beige Power Mac G3 (g3beige)
 * machine. This is a NEW device, deliberately separate from both
 * upstream's ati-vga (ati.c, which also targets Rage 128 Pro but is
 * built around the VGA core for Linux/PC-style guests) and our own
 * ati-mach64-gt (the machine's onboard chip) -- following the same
 * "own device, own register file, matched against Mac ground truth"
 * approach that made the mach64 bring-up succeed.
 *
 * The card identity (0x5046) is chosen to match the PCIR header of the
 * real ATI OEM Rage 128 Pro ROM (an AGP card's ROM -- the Beige G3 is
 * PCI-only, but QEMU has no AGP bus and models AGP devices as PCI
 * devices; the ROM's FCode must find a device whose vendor/device IDs
 * match its PCIR or Open Firmware will not bind/run it). The FCode in
 * that ROM (detokenized 2026-08-01) is the ground truth for the BAR
 * layout below: it looks up config offset 0x10 (memory, framebuffer
 * aperture), 0x18 (memory, 16KB register aperture) and 0x14 (I/O
 * register access, 256 bytes) in assigned-addresses and maps them with
 * "map-in", then flips the PCI command register bits itself via
 * config-b@/config-b!.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_RAGE128_INT_H
#define ATI_RAGE128_INT_H

#include "hw/pci/pci_device.h"
#include "hw/display/edid.h"
#include "hw/i2c/bitbang_i2c.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define PCI_VENDOR_ID_ATI              0x1002
/*
 * 0x5245 ("RE") is ATI's ID for the original, non-Pro Rage 128
 * (GL/SG family), not the Rage 128 Pro -- in ATI's own naming scheme
 * the "P"-prefixed IDs (0x5041-0x5052, "PA".."PR") are Pro parts and
 * the "R"-prefixed IDs (0x5245-0x524C, "RE".."RL") are the earlier
 * non-Pro chip. This device models the Pro feature set but currently
 * advertises the non-Pro ID because it matches a genuine retail
 * PCI-slot card ROM dump (ati_ret_nexus128_103_pci_full.rom,
 * PCIR-confirmed; note the filename itself lacks "pro") rather than
 * the AGP 4x TMDS Pro variant (0x5046, "PF") this device previously
 * used. The Beige G3 (Desktop/Minitower/AIO) never had an AGP slot at
 * all -- AGP debuted with the later Blue & White G3 -- so a card in
 * one of its real PCI slots is always the PCI variant; 0x5046 was
 * only ever a stand-in used because it was the only Rage 128 Pro ROM
 * dump available at the time. The ID/feature-set mismatch hasn't
 * mattered functionally so far; a genuine Rage 128 Pro PCI ROM would
 * resolve it if one turns up.
 */
#define PCI_DEVICE_ID_ATI_RAGE128PRO   0x5245

#define TYPE_ATI_RAGE128 "ati-rage128-pro"
OBJECT_DECLARE_SIMPLE_TYPE(ATIRage128State, ATI_RAGE128)

/*
 * Real Rage 128 BAR layout (confirmed by both the FCode's own mapping
 * words and real-hardware lspci dumps of Rage 128 cards):
 *   BAR0: 64MB memory, framebuffer aperture (prefetchable on real hw)
 *   BAR1: 256 bytes I/O, register access
 *   BAR2: 16KB memory, register aperture
 *   Expansion ROM: 128KB
 */
#define ATI_RAGE128_APER_SIZE   (64 * 1024 * 1024)
/*
 * 32MB, which is what the retail Rage 128 Pro this device models carries
 * and what the rest of the code already assumes: the 64MB aperture is
 * described, and mapped, as two 32MB images of the frame buffer.
 *
 * With only 16MB the upper half of the guest's frame-buffer memory did
 * not exist. Mac OS allocates its offscreen surfaces from the top, so
 * everything it staged there was silently lost -- captured live, the
 * video scaler's source sat at 0x1fda000 (31.85MB) and the blits around
 * it addressed 0x1fd9500 and 0x1ff3f00, all beyond the end of VRAM.
 */
#define ATI_RAGE128_VRAM_SIZE   (32 * 1024 * 1024)
#define ATI_RAGE128_MMIO_SIZE   (16 * 1024)
#define ATI_RAGE128_IO_SIZE     256
#define ATI_RAGE128_NUM_REGS    (ATI_RAGE128_MMIO_SIZE / 4)
#define ATI_RAGE128_NUM_PLLS    64
#define ATI_RAGE128_FB_SCAN_BLOCK (64 * 1024)

/*
 * One decoded GEN_PRIM vertex: pre-transformed screen-space position
 * (x/y in pixels on the render target, z in 0..1) and the diffuse
 * colour as 0..1 floats -- the CCE FPU path's native form. rhw, fog
 * and s/t are parsed for stride but not carried: perspective, fog and
 * texturing are later steps.
 */
typedef struct ATIRage128Vertex {
    float x, y, z;
    float rhw;               /* 1/w, 1.0 when the format carries none */
    float b, g, r, a;
    float s, t;              /* primary texture coordinates, 0 if absent */
} ATIRage128Vertex;

#define R128_HOSTDATA_HDR_MAX   (1 + 5 + 5)

typedef struct ATIRage128PM4Parser {
    uint32_t remaining;      /* data dwords still expected */
    uint32_t type;           /* packet type of the in-flight packet */
    uint32_t reg;            /* running register offset, packet0 */
    bool one_reg;
    uint32_t p1_reg1;        /* packet1's two register offsets */
    uint32_t p1_reg2;
    uint32_t p3_opcode;      /* packet3 2D-draw sub-state */
    /*
     * HOSTDATA_BLT's full header is the context dword, up to five
     * GMC-announced prefix dwords, and five fixed ones (see the ring
     * parser); PAINT's inline-brush form needs seven.
     */
    uint32_t p3_params[R128_HOSTDATA_HDR_MAX];
    uint32_t p3_scale[16];      /* R128_SCALE_PKT_DWORDS */
    uint32_t p3_param_idx;
    uint32_t p3_total;       /* payload dwords the packet3 declared */
    uint32_t p3_vc_format;   /* GEN_PRIM: VC_FORMAT dword */
    uint32_t p3_vc_cntl;     /* GEN_PRIM: VC_CNTL dword */
    uint32_t p3_vtx_stride;  /* dwords per vertex, from VC_FORMAT */
    uint32_t p3_vtx[16];     /* the vertex currently being gathered */
    uint32_t p3_vtx_count;   /* completed vertices in this packet */
    ATIRage128Vertex p3_tri[3]; /* triangle accumulator (list/fan/strip) */
} ATIRage128PM4Parser;

typedef struct ATIRage128Mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bytes per pixel */
    uint32_t fb_offset;  /* byte offset into VRAM */
    uint32_t pix_width;  /* raw CRTC_PIX_WIDTH field, for draw dispatch */
} ATIRage128Mode;

struct ATIRage128State {
    PCIDevice parent_obj;

    MemoryRegion aper;        /* BAR0: 64MB aperture container */
    MemoryRegion vram;        /* 16MB of real VRAM at aperture offset 0 */
    MemoryRegion vram_aper1;  /* alias of VRAM in the aperture's top half */
    MemoryRegion mmio;        /* BAR2: 16KB register file */
    MemoryRegion io;          /* BAR1: 256-byte I/O register window */
    QemuConsole *con;

    uint32_t regs[ATI_RAGE128_NUM_REGS];
    uint32_t plls[ATI_RAGE128_NUM_PLLS];

    /* DAC palette state (PALETTE_INDEX/PALETTE_DATA) */
    uint8_t dac_wr_index;
    uint8_t dac_rd_index;
    uint8_t palette[256][3];

    ATIRage128Mode mode;      /* what the last refresh actually drew */
    /*
     * The last mode CRTC1 itself described while valid. Kept apart from
     * `mode`: when the auto-detected framebuffer overrides the CRTC, `mode`
     * holds the guess, and using it as the "remembered CRTC mode" fallback
     * made the override compare the guess against itself and stick for
     * good (seen live: OS X blanked the display for sleep, the heuristic
     * swapped in a stale 8bpp 800x600 buffer, and it never let go).
     */
    ATIRage128Mode crtc_mode;
    bool mode_dirty;
    bool have_valid_mode; /* has `crtc_mode` ever held a real, valid mode? */

    /*
     * Auto-detected framebuffer, tracked via VRAM write activity
     * rather than any register: neither real guest OS this device has
     * been tested against (Mac OS X 10.2, Mac OS 9.2) ever programs
     * CRTC1's "Extended" mode-set registers at all -- confirmed live,
     * see the comment on ati_rage128_scan_vram_activity() -- so the
     * *actual* live framebuffer, whenever it isn't the one CRTC1's
     * last-known-good mode already correctly describes, has to be
     * found some other way. `fb_scan_activity` is a decayed
     * hit-counter per ATI_RAGE128_FB_SCAN_BLOCK-sized block of VRAM;
     * a sustained run of "recently written every scan" blocks is
     * treated as the real, currently-live framebuffer.
     */
    uint8_t fb_scan_activity[ATI_RAGE128_VRAM_SIZE / ATI_RAGE128_FB_SCAN_BLOCK];
    uint32_t fb_scan_counter;
    /*
     * Dirty VRAM blocks seen by the per-refresh dirty snapshot since the
     * activity scan last consumed them (the snapshot clears the bitmap, so
     * there is exactly one consumer of it -- ati_rage128_update_display --
     * and the slower activity scan reads this instead).
     */
    bool fb_block_pending[ATI_RAGE128_VRAM_SIZE / ATI_RAGE128_FB_SCAN_BLOCK];
    /* redraw the whole surface next pass regardless of dirty state */
    bool force_redraw;
    bool auto_fb_valid;
    ATIRage128Mode auto_fb_mode;
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
    ATIRage128Mode auto_fb_pending;

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
    bool monid_pads12;       /* DDC session uses SDA=pad1/SCL=pad2 (FCode) */
    bool monid_ddc2;         /* host clocked SCL: DDC1 stream silenced */
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
     * comment on R128_PM4_BUFFER_OFFSET in ati_rage128_regs.h.
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
    ATIRage128PM4Parser pm4_fifo;

    /*
     * 2D GUI (destination datapath) engine state -- ported from the
     * real upstream `ati-vga` device (hw/display/ati.c/ati_2d.c), not
     * generic/free-standing; register semantics are documented on the
     * offsets themselves in ati_rage128_regs.h. These mirror upstream's
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
     * The drawing context the transfer STARTED with. A host-data
     * transfer spans many writes and its last partial accumulator is
     * not flushed when the data stops arriving: it is flushed when the
     * next blit turns up, from ati_rage128_2d_blt()'s implicit finish.
     * By then that blit's caller has already installed its own
     * destination rectangle -- every one of the packet handlers in
     * ati_rage128.c assigns dst_x/dst_y/dst_width/dst_height and only
     * then calls in -- so the tail would be written into the next
     * operation's rectangle, and where the stale column index runs past
     * the new width the unsigned `dst_width - col` underflows to a huge
     * span and paints straight across its neighbours.
     *
     * The CCE packet handlers close their own transfer before returning,
     * so they never reach that; the paths that can are the MMIO/type-0
     * HOST_DATA0-7 register stream that ends without HOST_DATA_LAST, and
     * a GEN_RESET_CNTL SOFT_RESET_GUI landing mid-packet (which drops
     * the parser's packet state but not host_data_active). A transfer
     * owns its context until it ends, which is what the FIFO semantics
     * say, and it makes the ordering question disappear rather than
     * adding a rule to each of the five callers.
     */
    struct {
        uint32_t dst_x, dst_y, dst_width, dst_height;
        uint32_t dst_offset, dst_pitch;
        uint32_t datatype;
        uint32_t src_frgd_clr, src_bkgd_clr;
        int32_t sc_left, sc_top, sc_right, sc_bottom;
    } hd;

    /*
     * Silent-register tally -- see ati_rage128_audit_reg_write(). One
     * counter per register in the audited window, indexed by
     * offset >> 2. A flat array rather than a first-seen slot list,
     * following the R350's measurement: a 160-slot list saturated on a
     * Mac OS X session and a truncated census reads as a complete one.
     * The word count mirrors R128_REG_AUDIT_LIMIT in the generated
     * ati_rage128_audit.h; ati_rage128_dbg.c build-checks the pairing.
     * Deliberately not in vmstate: a diagnostic counter, not state a
     * migrated guest can notice.
     */
#define R128_SILENT_REG_WORDS 4096
    uint32_t silent_reg_count[R128_SILENT_REG_WORDS];
};


/* ati_rage128_dbg.c */
const char *ati_rage128_reg_name(uint32_t base);

/*
 * Tally a write to a register no model code reads -- one stored into
 * s->regs[] (or dropped) and consulted by no logic, so nothing else in
 * the device can notice it. Called from the ati_rage128_reg_write32()
 * funnel, which every register write passes through: the MMIO and I/O
 * BAR ops, the MM_INDEX/MM_DATA indirection, PM4 type-0/1 packets from
 * both the ring and the PIO FIFO/indirect buffers, the packet-3 blit
 * decoders, and GUI bus-master descriptors. Read the tally with
 * `qom-get <device> silent-regs`.
 */
void ati_rage128_audit_reg_write(ATIRage128State *s, uint32_t base);

/* ati_rage128_2d.c */
void ati_rage128_2d_blt(ATIRage128State *s);
void ati_rage128_3d_triangle(ATIRage128State *s, const ATIRage128Vertex *v);
void ati_rage128_2d_scale(ATIRage128State *s, const uint32_t *pkt);
void ati_rage128_2d_scale_regs(ATIRage128State *s);
bool ati_rage128_host_data_flush(ATIRage128State *s);

/*
 * Show/position a host-driven pointer on this card's console. Used by
 * the mach64's host-cursor-tracking workaround once the pointer crosses
 * onto this display: this device has no hardware-cursor emulation and
 * the guest never drives one here, so without it the pointer would
 * simply vanish on the second screen.
 */
void ati_rage128_host_cursor(int x, int y, bool on);

#endif /* ATI_RAGE128_INT_H */
