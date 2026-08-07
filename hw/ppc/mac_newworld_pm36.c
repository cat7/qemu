/*
 * PowerMac3,6 (Mirrored Drive Doors) machine-specific constants and device
 * wiring
 *
 * Comparable to mac_newworld_pm34.c: mac_newworld.c stays the generic New
 * World Mac platform file and calls into this interface wherever behavior
 * needs to match real PowerMac3,6 "Mirrored Drive Doors" hardware
 * specifically (a real dual-CPU G4 tower).
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
#include "hw/ppc/mac_newworld_pm34.h"
#include "hw/ppc/mac_newworld_pm36.h"
#include "hw/misc/macio/mac99_pm36_i2c.h"
#include "target/ppc/cpu.h"

/*
 * The frequencies a real PowerMac3,6 (Mirrored Drive Doors, dual 867MHz)
 * publishes in its own device tree, taken verbatim from an lsprop dump of
 * /cpus/PowerPC,G4@0 (~/Downloads/device-tree-powermac3,6-smp.txt):
 *
 *   timebase-frequency  0x01fc36a7    33,306,279 Hz
 *   clock-frequency     0x33a848a8   866,666,664 Hz
 *   bus-frequency       0x07f0da9f   133,225,119 Hz
 *
 * As with PowerMac3,4, these are what the real firmware measured on real
 * silicon, not round numbers -- the timebase = bus/4 relationship still
 * holds. bus-frequency/timebase-frequency confirms it (133225119/4 =
 * 33306279.75, matching within rounding).
 */
#define PM36_TBFREQ    33306279UL
#define PM36_CLOCKFREQ 866666664UL
#define PM36_BUSFREQ   133225119UL

uint32_t pm36_tbfreq(void)
{
    return PM36_TBFREQ;
}

uint32_t pm36_clockfreq(void)
{
    return PM36_CLOCKFREQ;
}

uint32_t pm36_busfreq(void)
{
    return PM36_BUSFREQ;
}

void pm36_cpu_defaults(PowerPCCPU *cpu)
{
    /*
     * Same mechanism as PowerMac3,4 (see pm34_cpu_defaults()), but the 7455
     * ("7450 family") PLL_CFG field is 5 bits wide (HID1 bits 15-19, PC0-
     * PC4), one bit wider than the 7400's 4-bit field -- confirmed against
     * the MPC7455 RISC Microprocessor Hardware Specifications Rev 4.1,
     * Table 17: PLL_CFG 0b01010 gives a 6.5x bus-to-core multiplier, which
     * at this machine's ~133MHz bus lands on ~866MHz -- an exact match for
     * the real dual-867MHz Mirrored Drive Doors. PC1 (bit 16, 0x00008000)
     * and PC3 (bit 18, 0x00002000) are the set bits in 0b01010.
     */
    cpu->env.spr_cb[SPR_HID1].default_value = 0x0000A000;
    cpu->env.spr[SPR_HID1] = 0x0000A000;
}

/*
 * Unconfirmed: the real PowerMac3,6 device-tree dump doesn't publish a
 * device-tree node for this EEPROM (same as PowerMac3,4 -- OF reads it via
 * raw I2C without ever creating a node for it, so its presence can't be
 * confirmed or denied from the dump alone). Reuse PowerMac3,4's EEPROM as a
 * safe placeholder: harmless if unused, and a real Apple ROM is documented
 * to refuse booting without answering an I2C transaction at this address at
 * all, which matters more than what the EEPROM contains.
 */
void pm36_add_config_eeprom(I2CBus *bus)
{
    pm34_add_config_eeprom(bus);
}

/*
 * Unconfirmed/placeholder: real SPD data transcription is Stage 6 of the
 * mac99 PowerMac3,6 plan. Reuse PowerMac3,4's synthesis so machine init
 * doesn't crash before then; this needs replacing with real PowerMac3,6
 * SPD geometry (4 DIMM slots, not 3) once that stage lands.
 */
void pm36_add_spd_dimms(I2CBus *bus, uint64_t ram_size)
{
    pm34_add_spd_dimms(bus, ram_size);
}

/*
 * Confirmed live on real PowerMac3,6 hardware's UniNorth I2C bus, addresses
 * converted from the device tree's 8-bit convention to 7-bit the same way
 * as the existing PowerMac3,4 config EEPROM comment does.
 */
void pm36_add_i2c_peripherals(I2CBus *bus)
{
    i2c_slave_create_simple(bus, TYPE_ADM1030, 0x2c);
    i2c_slave_create_simple(bus, TYPE_CY2213, 0x65);
    i2c_slave_create_simple(bus, TYPE_DS1775, 0x49);
}

/*
 * Confirmed identical to PowerMac3,4 from the real device tree: GMAC sits
 * at the same pci@f4000000 slot 0x0f (built-in-names lists "Ethernet" at
 * the same position, ethernet@f node present) on both real machines.
 */
void pm36_place_gmac(PCIBus *internal_bus, const char *default_nic)
{
    pm34_place_gmac(internal_bus, default_nic);
}

/*
 * Confirmed identical to PowerMac3,4 from the real device tree's
 * interrupt-map (0x28/0x29 for the same two slots). The dump also shows a
 * third/fourth line for the "Kauai ATA" PCI controller at slot 0x0d (IRQs
 * 0x27/0x26) -- deliberately not wired here, since that whole controller
 * (PowerMac3,6's real hard-disk path, distinct from PowerMac3,4's) is
 * deferred; PowerMac3,6 boots from CD via KeyLargo's built-in ATA only
 * until that's implemented.
 */
void pm36_internal_bus_irq_map(DeviceState *uninorth_internal_dev,
                               DeviceState *pic_dev)
{
    pm34_internal_bus_irq_map(uninorth_internal_dev, pic_dev);
}

/*
 * Confirmed identical to PowerMac3,4 from the real device tree: the same
 * seven (slot, irq) pairs appear in pci@f2000000's interrupt-map on both
 * real machines.
 */
void pm36_pci_irq_map(DeviceState *uninorth_pci_dev, DeviceState *pic_dev)
{
    pm34_pci_irq_map(uninorth_pci_dev, pic_dev);
}

/* Confirmed identical to PowerMac3,4 from the real device tree: mac-io@17. */
int pm36_macio_devfn(void)
{
    return pm34_macio_devfn();
}

/* Confirmed identical to PowerMac3,4 from the real device tree: usb@18. */
int pm36_usb_devfn(void)
{
    return pm34_usb_devfn();
}
