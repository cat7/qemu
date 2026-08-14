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

#ifndef ATI_RAGE128_H
#define ATI_RAGE128_H

#include "hw/pci/pci_device.h"
#include "hw/display/edid.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define PCI_VENDOR_ID_ATI              0x1002
/* "PF" = Rage 128 Pro (the AGP 4x TMDS variant the OEM Mac ROM is for) */
#define PCI_DEVICE_ID_ATI_RAGE128PRO   0x5046

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

typedef struct ATIRage128Mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint32_t bpp;        /* bytes per pixel */
    uint32_t fb_offset;  /* byte offset into VRAM */
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
};

#endif /* ATI_RAGE128_H */
