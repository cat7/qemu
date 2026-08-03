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
#include "qemu/timer.h"
#include "qom/object.h"

#define PCI_VENDOR_ID_ATI              0x1002
/*
 * 0x5245 = Rage 128 Pro, PCI (non-AGP) variant -- matches a genuine
 * retail PCI-slot card ROM (ati_ret_nexus128_103_pci_full.rom,
 * PCIR-confirmed) rather than the AGP 4x TMDS variant (0x5046) this
 * device previously used. The Beige G3 (Desktop/Minitower/AIO) never
 * had an AGP slot at all -- AGP debuted with the later Blue & White
 * G3 -- so a card in one of its real PCI slots is always the PCI
 * variant; 0x5046 was only ever a stand-in used because it was the
 * only Rage 128 Pro ROM dump available at the time.
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
#define ATI_RAGE128_VRAM_SIZE   (16 * 1024 * 1024)
#define ATI_RAGE128_MMIO_SIZE   (16 * 1024)
#define ATI_RAGE128_IO_SIZE     256
#define ATI_RAGE128_NUM_REGS    (ATI_RAGE128_MMIO_SIZE / 4)
#define ATI_RAGE128_NUM_PLLS    64
#define ATI_RAGE128_FB_SCAN_BLOCK (64 * 1024)

typedef struct ATIRage128PM4Parser {
    uint32_t remaining;      /* data dwords still expected */
    uint32_t type;           /* packet type of the in-flight packet */
    uint32_t reg;            /* running register offset, packet0 */
    bool one_reg;
    uint32_t p1_reg1;        /* packet1's two register offsets */
    uint32_t p1_reg2;
    uint32_t p3_opcode;      /* packet3 2D-draw sub-state */
    uint32_t p3_params[3];
    uint32_t p3_param_idx;
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

    ATIRage128Mode mode;
    bool mode_dirty;
    /*
     * Cursor updates are coalesced to one per displayed frame. Moving the
     * pointer past the top edge takes two writes that must agree --
     * CUR_OFFSET advances 16 bytes per hidden line while CUR_VERT_OFF
     * counts the same lines -- and Mac OS X does NOT bracket them with
     * CUR_LOCK (confirmed live: lock read 0 in every sample). Rebuilding
     * the sprite on each individual write therefore renders the
     * intermediate state, where the offset has moved but the line count
     * has not, and the tail of the 64-row read runs off the end of the
     * 1KB image into unrelated VRAM -- drawn as a large opaque block.
     * Real hardware has the same window but only latches the cursor once
     * a frame, so it is never visible; do the same.
     */
    bool cursor_dirty;
    bool have_valid_mode; /* has `mode` ever held a real, valid mode? */

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
    uint32_t sc_top, sc_left, sc_bottom, sc_right;
    uint32_t src_sc_bottom, src_sc_right;
    uint32_t dp_cntl, dp_datatype, dp_mix, dp_write_mask;
    uint32_t default_offset, default_pitch;
    uint32_t default_sc_bottom, default_sc_right;

    /* HOST_DATA0-7/LAST accumulator, same protocol as upstream */
    bool host_data_active;
    uint32_t host_data_row, host_data_col, host_data_next;
    uint32_t host_data_acc[4];
};


/* ati_rage128_dbg.c */
const char *ati_rage128_reg_name(uint32_t base);

/* ati_rage128_2d.c */
void ati_rage128_2d_blt(ATIRage128State *s);
bool ati_rage128_host_data_flush(ATIRage128State *s);

#endif /* ATI_RAGE128_INT_H */
