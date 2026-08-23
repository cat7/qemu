/*
 * PowerMac3,4 (Digital Audio) machine-specific constants and device wiring
 *
 * Split out of hw/ppc/mac_newworld.c: this file holds everything that ties
 * the "mac99" QEMU machine to the exact real PowerMac3,4 "Digital Audio"
 * hardware it emulates, so a second real machine (PowerMac3,6) can have its
 * own comparable file without the two being tangled together.
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "hw/ppc/mac_newworld_pm34.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/nvram/mac_spd.h"
#include "hw/pci/pci_device.h"
#include "net/net.h"
#include "target/ppc/cpu.h"

/*
 * The frequencies a real PowerMac3,4 (Digital Audio) publishes in its own
 * device tree, taken verbatim from an lsprop dump of /cpus/PowerPC,G4@0:
 *
 *   timebase-frequency  0x01fbf711    33,290,001 Hz
 *   clock-frequency     0x1bd0c4a9   466,666,665 Hz
 *   bus-frequency       0x07efdc44   133,160,004 Hz
 *
 * These are what the real firmware measured on real silicon, so they are not
 * quite the nominal 33.33/466.67/133.33MHz round numbers -- but the exact
 * timebase = bus/4 relationship holds, as on every 60x-bus PowerPC, and
 * guests derive one frequency from the other, so keep the set consistent.
 */
#define PM34_TBFREQ    33290001UL
#define PM34_CLOCKFREQ 466666665UL
#define PM34_BUSFREQ   133160004UL

#define PM34_SPD_NUM_DIMMS 3

uint32_t pm34_tbfreq(void)
{
    return PM34_TBFREQ;
}

uint32_t pm34_clockfreq(void)
{
    return PM34_CLOCKFREQ;
}

uint32_t pm34_busfreq(void)
{
    return PM34_BUSFREQ;
}

void pm34_cpu_defaults(PowerPCCPU *cpu)
{
    /*
     * The Apple ROM derives the CPU clock itself: it measures the bus
     * clock against a timer and multiplies by the PLL ratio it reads
     * from HID1[0:3] (PLL_CFG). QEMU's 74xx resets HID1 to 0, whose
     * decode made Open Firmware report a 1.2GHz processor. A 466MHz
     * Digital Audio runs 3.5x on a 133MHz bus, and MPC7400EC Table 14
     * gives PLL_CFG 0b1110 for 3.5x -- with this in place OF computes
     * ~465MHz on its own. Set the default too: SPRs snap back to
     * their registered defaults on every machine reset.
     */
    cpu->env.spr_cb[SPR_HID1].default_value = 0xE0000000;
    cpu->env.spr[SPR_HID1] = 0xE0000000;
}

/*
 * The processor module carries an SPD-format configuration EEPROM. The
 * Tangent schematic labels it "ADDRESS = 6 (AC/AD)" next to the module
 * connector's I2C pins, i.e. 8-bit 0xac/0xad, 7-bit 0x56. A real Apple
 * ROM reads it during bring-up and refuses to continue without it.
 */
void pm34_add_config_eeprom(I2CBus *bus)
{
    at24c_eeprom_init_rom(bus, 0x56, 256, NULL, 0);
}

/*
 * SPD EEPROMs for the memory slots, at the usual 0x50 + slot. The ROM
 * sizes RAM from these alone -- it never asks QEMU -- so the sticks
 * must add up to the configured memory or the guest sees the donor
 * machine's RAM instead of -m (observed: -m 1024 booting as 768MB,
 * the donor's 3x256MB). Synthesize an SPD image per stick from the
 * donor 256MB module, patching the geometry bytes (3 rows, 4 cols,
 * 5 ranks, 31 bank density) and re-summing the byte-63 checksum.
 */
void pm34_add_spd_dimms(I2CBus *bus, uint64_t ram_size)
{
    static const uint8_t pm34_spd_dimm0[MAC_SPD_SIZE] = {
        0x80, 0x08, 0x04, 0x0d, 0x0a, 0x01, 0x40, 0x00, 0x01, 0x75, 0x54, 0x00,
        0x82, 0x08, 0x00, 0x01, 0x8f, 0x04, 0x04, 0x01, 0x01, 0x00, 0x0e, 0x00,
        0x00, 0x00, 0x00, 0x14, 0x0f, 0x14, 0x2d, 0x40, 0x15, 0x08, 0x15, 0x08,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x12, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x52, 0x4d, 0x35, 0x36, 0x53, 0x32, 0x38, 0x31, 0x54, 0x41, 0x2d,
        0x31, 0x33, 0x41, 0x43, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x40, 0xff,
        0xff, 0xff, 0xff, 0xff, 0x52, 0x30, 0x31, 0x4a, 0x34, 0x30, 0x43, 0x4d,
        0x35, 0x31, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x64, 0xad,
    };
    static const struct {
        unsigned mb, rows, cols, ranks, density;
    } spd_geom[] = {
        { 512, 13, 10, 2, 0x40 },
        { 256, 13, 10, 1, 0x40 },
        { 128, 12, 10, 1, 0x20 },
        {  64, 12,  9, 1, 0x10 },
        {  32, 11,  9, 1, 0x08 },
    };
    uint64_t left = ram_size;
    int slot = 0;
    unsigned g = 0;

    while (left && slot < PM34_SPD_NUM_DIMMS &&
           g < ARRAY_SIZE(spd_geom)) {
        uint8_t spd[MAC_SPD_SIZE];
        unsigned sum, b;

        if (left < (uint64_t)spd_geom[g].mb * MiB) {
            g++;
            continue;
        }
        memcpy(spd, pm34_spd_dimm0, MAC_SPD_SIZE);
        spd[3] = spd_geom[g].rows;
        spd[4] = spd_geom[g].cols;
        spd[5] = spd_geom[g].ranks;
        spd[31] = spd_geom[g].density;
        for (sum = 0, b = 0; b < 63; b++) {
            sum += spd[b];
        }
        spd[63] = sum & 0xff;
        at24c_eeprom_init_rom(bus, 0x50 + slot, MAC_SPD_SIZE, spd, MAC_SPD_SIZE);
        left -= (uint64_t)spd_geom[g].mb * MiB;
        slot++;
    }
    if (left) {
        warn_report("mac99: %" PRIu64 "MB of RAM not representable as "
                    "1-3 PowerMac3,4 DIMMs; the firmware will see %"
                    PRIu64 "MB", left / MiB, (ram_size - left) / MiB);
    }
}

/*
 * With the Apple ROM providing the device tree, the on-board GMAC must sit
 * where that tree says it does: pci@f4000000, slot 0x0f (verified against a
 * real PowerMac3,4's ethernet@f node; our sungem is the same 106b:0021
 * part).
 */
void pm34_place_gmac(PCIBus *internal_bus, const char *default_nic)
{
    pci_init_nic_in_slot(internal_bus, default_nic, NULL, "f");
}

/*
 * Real pci@f0000000 interrupt-map: the AGP slot (device 0x10) -> 0x30.
 * Index order matches pci_unin_agp_real_map_irq(); index 7 is the
 * unwired spare, as on the real machine.
 */
void pm34_agp_bus_irq_map(DeviceState *uninorth_agp_dev, DeviceState *pic_dev)
{
    qdev_connect_gpio_out(uninorth_agp_dev, 0,
                          qdev_get_gpio_in(pic_dev, 0x30));
}

/* Real pci@f4000000 interrupt-map: slots 0x0e/0x0f */
void pm34_internal_bus_irq_map(DeviceState *uninorth_internal_dev,
                               DeviceState *pic_dev)
{
    qdev_connect_gpio_out(uninorth_internal_dev, 0,
                          qdev_get_gpio_in(pic_dev, 0x28));
    qdev_connect_gpio_out(uninorth_internal_dev, 1,
                          qdev_get_gpio_in(pic_dev, 0x29));
}

/*
 * Real PowerMac3,4 pci@f2000000 interrupt-map, in
 * pci_unin_main_real_map_irq() index order (slots 0x12-0x15,
 * 0x18-0x1a); index 7 is the spare for off-map slots and stays
 * unwired on the real machine too.
 */
void pm34_pci_irq_map(DeviceState *uninorth_pci_dev, DeviceState *pic_dev)
{
    static const int real_pci_irqs[7] = {
        0x34, 0x35, 0x36, 0x3a, 0x1b, 0x1c, 0x3f
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(real_pci_irqs); i++) {
        qdev_connect_gpio_out(uninorth_pci_dev, i,
                              qdev_get_gpio_in(pic_dev, real_pci_irqs[i]));
    }
}

/*
 * Slot numbers matter to a real Apple ROM, which expects the built-in
 * devices where the hardware puts them: the PowerMac3,4 device tree has
 * mac-io@17 and usb@18 / usb@19 on this bus.
 */
int pm34_macio_devfn(void)
{
    return PCI_DEVFN(0x17, 0);
}

int pm34_usb_devfn(void)
{
    return PCI_DEVFN(0x18, 0);
}

/* Real PM34 device tree: /pci@f4000000/firewire@e, device 14 function 0. */
int pm34_firewire_devfn(void)
{
    return PCI_DEVFN(0x0e, 0);
}
