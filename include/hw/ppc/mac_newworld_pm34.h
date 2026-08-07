/*
 * PowerMac3,4 (Digital Audio) machine-specific constants and device wiring
 *
 * Split out of hw/ppc/mac_newworld.c so a second real machine
 * (PowerMac3,6) can live alongside it without the two being tangled
 * together: mac_newworld.c stays the generic New World Mac platform file
 * and calls into this interface wherever behavior needs to match real
 * PowerMac3,4 "Digital Audio" hardware specifically.
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

#ifndef HW_PPC_MAC_NEWWORLD_PM34_H
#define HW_PPC_MAC_NEWWORLD_PM34_H

#include "hw/ppc/ppc.h"
#include "hw/i2c/i2c.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev.h"

uint32_t pm34_tbfreq(void);
uint32_t pm34_clockfreq(void);
uint32_t pm34_busfreq(void);
void pm34_cpu_defaults(PowerPCCPU *cpu);

void pm34_add_config_eeprom(I2CBus *bus);
void pm34_add_spd_dimms(I2CBus *bus, uint64_t ram_size);

void pm34_place_gmac(PCIBus *internal_bus, const char *default_nic);
void pm34_internal_bus_irq_map(DeviceState *uninorth_internal_dev,
                               DeviceState *pic_dev);
void pm34_pci_irq_map(DeviceState *uninorth_pci_dev, DeviceState *pic_dev);

int pm34_macio_devfn(void);
int pm34_usb_devfn(void);

#endif
