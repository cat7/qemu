/*
 * PowerMac3,6 (Mirrored Drive Doors) machine-specific constants and device
 * wiring
 *
 * Stub for now (see the mac99 PowerMac3,6 support plan) -- comparable to
 * mac_newworld_pm34.c, filled in as each piece of real PowerMac3,6 hardware
 * fidelity lands.
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
#include "hw/ppc/mac_newworld_pm36.h"

uint32_t pm36_tbfreq(void)
{
    g_assert_not_reached();
}

uint32_t pm36_clockfreq(void)
{
    g_assert_not_reached();
}

uint32_t pm36_busfreq(void)
{
    g_assert_not_reached();
}

void pm36_cpu_defaults(PowerPCCPU *cpu)
{
    g_assert_not_reached();
}

void pm36_add_config_eeprom(I2CBus *bus)
{
    g_assert_not_reached();
}

void pm36_add_spd_dimms(I2CBus *bus, uint64_t ram_size)
{
    g_assert_not_reached();
}

void pm36_add_i2c_peripherals(I2CBus *bus)
{
    g_assert_not_reached();
}

void pm36_place_gmac(PCIBus *internal_bus, const char *default_nic)
{
    g_assert_not_reached();
}

void pm36_internal_bus_irq_map(DeviceState *uninorth_internal_dev,
                               DeviceState *pic_dev)
{
    g_assert_not_reached();
}

void pm36_pci_irq_map(DeviceState *uninorth_pci_dev, DeviceState *pic_dev)
{
    g_assert_not_reached();
}

int pm36_macio_devfn(void)
{
    g_assert_not_reached();
}

int pm36_usb_devfn(void)
{
    g_assert_not_reached();
}
