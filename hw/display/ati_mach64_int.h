/*
 * QEMU ATI Mach64 "3D Rage Pro" emulation
 *
 * Models the ATI Mach64 "3D Rage Pro" (device 0x4750, ASIC/revision ID
 * 0x5c) PCI graphics card. This is the identity directly and
 * unambiguously stated by this exact machine's own real Open Firmware
 * device-tree dump (SourceFiles/G3/PowerMacG3-device-tree.txt, node
 * ATY,mach64_3DUPro: "device-id"=0x4750, "revision-id"=0x5c).
 *
 * A prior revision of this file switched to modeling "3D Rage GT"
 * (device 0x4754, ASIC ID 0x9a) instead, reasoning from DingusPPC (an
 * independent, working PowerMac emulator) using `AtiRageGT` for its
 * Desktop-form-factor Beige G3 profile ("pmg3dt"). That reasoning
 * subordinated our own authoritative real-hardware ground truth to a
 * third party's own profile choice -- and empirically, the GT identity
 * was separately confirmed NOT to unblock CRTC mode-setting in our own
 * QEMU boot anyway (H_TOTAL_DISP/V_TOTAL_DISP/OFF_PITCH stayed zero even
 * after switching). Reverted back to the device-tree-confirmed Rage Pro
 * identity. This is a distinct chip generation from the Rage 128 Pro /
 * Radeon devices modeled by ati.c -- it has an entirely different
 * register layout, so it is implemented as its own device rather than
 * an extension of ati.c.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#ifndef ATI_MACH64_INT_H
#define ATI_MACH64_INT_H

#include "hw/pci/pci_device.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "ui/input.h"

#define PCI_VENDOR_ID_ATI            0x1002
/*
 * Tested switching this to the Rage II GT identity (0x4754/0x9A) to
 * match DingusPPC's own default "pmg3dt" GPU choice (machinegossamer.cpp
 * gossamer_desktop_settings uses "AtiRageGT") -- confirmed via a live
 * -trace comparison that the ROM's FCode DOES take a measurably
 * different PLL/register path for each chip ID, but the change did NOT
 * fix the CRTC-never-programmed issue (same give-up point either way).
 * Reverted to the real hardware device-tree's actual values (0x4750/0x5c,
 * SourceFiles/G3/PowerMacG3-device-tree.txt), which remain the
 * authoritative ground truth for the physical unit that was dumped.
 */
#define PCI_DEVICE_ID_ATI_RAGE_PRO   0x4750
#define ATI_RAGE_PRO_ASIC_ID         0x5c

#define TYPE_ATI_MACH64 "ati-mach64-gt"
OBJECT_DECLARE_SIMPLE_TYPE(ATIMach64State, ATI_MACH64)

/* Real BAR0 aperture size (only the low 6MB is actually backed on real
 * hardware, per that machine's "ATY,memsize" property, but the whole
 * declared aperture is backed with RAM here for simplicity). */
#define ATI_MACH64_VRAM_SIZE   (16 * 1024 * 1024)
#define ATI_MACH64_MMIO_SIZE   (4 * 1024)
#define ATI_MACH64_NUM_REGS    (ATI_MACH64_MMIO_SIZE / 4)
#define ATI_MACH64_NUM_PLLS    64

/*
 * Real Mach64/Rage hardware also exposes its register file through a
 * fixed offset within the framebuffer aperture (BAR0) itself, 1KB
 * below the 8MB mark (confirmed against DingusPPC's ATIRage model,
 * MM_REGS_0_OFF in atimach64defs.h) -- unconditionally, for GT/Rage
 * Pro-class chips (the BUS_CNTL bits that would gate this only apply
 * to older CT-class Mach64). This matters because this machine's own
 * Open Firmware only ever probes/assigns BAR0 for this device (traced
 * via -trace pci_cfg_write: BAR0 gets written to a real address, BAR2
 * is written to 0 once during the initial clear-all-BARs sweep and
 * never touched again) -- so the native ATI driver almost certainly
 * relies on this aperture-relative access path and may never actually
 * need BAR2 mapped at all.
 */
#define ATI_MACH64_REGS_IN_VRAM_OFFSET (8 * 1024 * 1024 - 1024)

/*
 * Real Mach64/Rage hardware mirrors the SAME physical VRAM bytes a
 * second time starting at this offset within the BAR0 aperture --
 * confirmed against DingusPPC's own ATIRage model (BE_FB_OFFSET in
 * atimach64defs.h, comment: "Offset to the big-endian frame buffer";
 * its own read()/write() dispatch for this upper mirror computes the
 * SAME underlying vram_ptr[] index as the plain low aperture, just
 * reached via a different address -- i.e. this is a second address
 * range for the identical bytes, not a separate buffer). Software can
 * address either alias interchangeably; this only matters if a guest
 * driver picks a CRTC_OFF_PITCH framebuffer base >= this offset, which
 * would previously have read/written uninitialized upper-VRAM instead
 * of the real framebuffer content.
 */
#define ATI_MACH64_BE_FB_OFFSET (8 * 1024 * 1024)

typedef struct ATIMach64Mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;
    uint32_t fb_offset;  /* byte offset into VRAM */
} ATIMach64Mode;

struct ATIMach64State {
    PCIDevice parent_obj;

    MemoryRegion vram;
    MemoryRegion mmio;
    MemoryRegion mmio_bar2;
    MemoryRegion io;
    MemoryRegion mmio_in_vram;
    MemoryRegion mmio_in_vram_blk1;
    MemoryRegion vram_be_mirror;
    QemuConsole *con;

    /* BM_ADDR/BM_DATA register-list port state (see ati_mach64_regs.h) */
    bool bm_data_mode;
    uint8_t bm_reg;
    int bm_reg_count;

    uint32_t regs[ATI_MACH64_NUM_REGS];
    uint8_t plls[ATI_MACH64_NUM_PLLS];

    uint8_t dac_wr_index;
    uint8_t dac_rd_index;
    uint8_t dac_mask;
    uint8_t dac_comp_index;
    uint8_t dac_comp_buf[3];
    uint8_t palette[256][3];

    ATIMach64Mode mode;
    bool mode_dirty;

    /*
     * Real hardware fires a VBLANK interrupt from the CRTC continuously
     * once powered, independent of whether a valid display mode has
     * been set yet. Classic Mac OS's own VBL task-queue init code
     * waits for this interrupt as confirmation the video hardware is
     * alive before proceeding -- confirmed against this exact ROM: it
     * spins forever on a low-memory flag byte ($172.w) that only a
     * real VBLANK interrupt's handler ever clears. Without this timer
     * the guest never gets past that wait at all, regardless of any
     * CRTC/DAC register correctness.
     */
    QEMUTimer *vblank_timer;
    QEMUTimer *vblank_end_timer;

    /*
     * WORKAROUND, not real hardware behavior: real Mach64 silicon never
     * autonomously tracks the host pointer -- the hardware cursor only
     * ever moves when software writes CUR_HORZ_VERT_POSN. This exists
     * because classic Mac OS's own VBL-queue-driven cursor task
     * (CrsrVBLTask) stops being serviced by the guest's interrupt
     * dispatcher partway through boot (see the project's investigation
     * notes), so the guest itself never issues those writes again. Since
     * the guest's own cursor-tracking logic never runs, we track the
     * real host pointer ourselves here and drive CUR_HORZ_VERT_POSN
     * directly, entirely independent of guest software -- letting the
     * cursor's on-screen position work even though the guest's own
     * dispatch is stuck. This does NOT address keyboard input or any
     * other guest-side symptom of the same stall.
     */
    QemuInputHandlerState *cursor_hs;
    bool host_cursor_tracking;
    int host_cursor_x;
    int host_cursor_y;
    /*
     * The tracking workaround above is specific to Classic Mac OS, so it
     * is armed only while a live Cursor Device Manager record is visible
     * in the guest's low memory -- see ati_mach64_sync_cursor_handler().
     * cursor_env_probe paces the check, cursor_env_miss debounces it.
     */
    int cursor_env_probe;
    int cursor_env_miss;
    bool monitor_connected;

    /*
     * DDC/I2C over the GP_IO sense pins: an EDID-serving I2C slave the
     * guest bit-bangs to read monitor identity (used by AppleVision and
     * the Monitors control panel). Independent of the Apple Monitor
     * Sense response on the same pins, which the ROM's boot-time
     * mode-set still relies on.
     */
    bitbang_i2c_interface bbi2c;
    I2CDDCState i2cddc;
    uint8_t i2c_sense;   /* last I2C SCL/SDA readback, 3-bit logical */

    /*
     * Host-data blit state (GUI engine, CPU-sourced pixels). Set up by
     * a DST_HEIGHT_WIDTH trigger whose DP_SRC selects HOST as the
     * foreground (or the mono) source; the guest then streams pixel or
     * 1bpp-mask words to the HOST_DATA registers, which this consumes
     * left-to-right, top-to-bottom into the destination rectangle.
     * Used for text/glyph drawing, which the accelerated NDRV offloads
     * to the engine. hb_active gates the HOST_DATA write path.
     */
    bool hb_active;
    bool hb_mono;          /* 1bpp mask expansion vs packed color */
    bool hb_byte_align;    /* re-align to a byte boundary on row wrap */
    bool hb_lsb_first;     /* DP_BYTE_PIX_ORDER: LSB-first within a byte */
    bool hb_bigendian;     /* HOST_CNTL: byte-swap each incoming word */
    bool hb_triple;        /* packed-24bpp: one mono bit = 3 dst bytes */
    bool hb_rot24;         /* packed-24bpp color component per dst byte */
    int hb_host_bpp;       /* bytes/pixel of the host color data */
    int hb_dst_bpp;
    int hb_x, hb_y;        /* current write position within the rect */
    int hb_x0, hb_w, hb_h, hb_h0; /* rect origin-x, width, height (rows left = h - y) */
    int hb_bit;            /* mono: consumed bits of the current word */
    int hb_words;          /* HOST_DATA words consumed by this blit */
    uint32_t hb_dst_base;
    int hb_dst_pitch;      /* pixels */
    uint32_t hb_frgd_clr, hb_bkgd_clr;
    uint8_t hb_frgd_mix, hb_bkgd_mix;
};


/* ati_mach64_dbg.c */
const char *ati_mach64_reg_name(uint32_t base);

/* ati_mach64_2d.c */
void ati_mach64_2d_op(ATIMach64State *s);
void ati_mach64_2d_line(ATIMach64State *s, uint32_t lnth);
void ati_mach64_set_gui_engine2(ATIMach64State *s, uint32_t w);
void ati_mach64_3d_trigger(ATIMach64State *s, uint32_t one_over_area);
void ati_mach64_host_data(ATIMach64State *s, uint32_t word);

#endif /* ATI_MACH64_H */
