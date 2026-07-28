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

#ifndef ATI_MACH64_H
#define ATI_MACH64_H

#include "hw/pci/pci_device.h"
#include "qemu/timer.h"
#include "qom/object.h"

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
    MemoryRegion io;
    MemoryRegion mmio_in_vram;
    MemoryRegion vram_be_mirror;
    QemuConsole *con;

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
    bool cursor_composited;

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
};

#endif /* ATI_MACH64_H */
