/*
 * QEMU Uninorth PCI host (for all Mac99 and newer machines)
 *
 * Copyright (c) 2006 Fabrice Bellard
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

#ifndef UNINORTH_H
#define UNINORTH_H

#include "hw/pci/pci_host.h"
#include "hw/pci/pci_bridge.h"
#include "hw/misc/macio/keywest_i2c.h"
#include "qom/object.h"

/* UniNorth version */
#define UNINORTH_VERSION_10A    0x7
#define U3_VERSION_23           0x33

#define TYPE_UNI_NORTH_PCI_HOST_BRIDGE "uni-north-pci-pcihost"
#define TYPE_UNI_NORTH_AGP_HOST_BRIDGE "uni-north-agp-pcihost"
#define TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE "uni-north-internal-pci-pcihost"
#define TYPE_U3_AGP_HOST_BRIDGE "u3-agp-pcihost"
#define TYPE_U3_HT_HOST_BRIDGE "u3-ht-pcihost"

typedef struct UNINHostState UNINHostState;
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_PCI_HOST_BRIDGE,
                         TYPE_UNI_NORTH_PCI_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_AGP_HOST_BRIDGE,
                         TYPE_UNI_NORTH_AGP_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE,
                         TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE)
DECLARE_INSTANCE_CHECKER(UNINHostState, U3_AGP_HOST_BRIDGE,
                         TYPE_U3_AGP_HOST_BRIDGE)

struct UNINHostState {
    PCIHostState parent_obj;

    uint32_t ofw_addr;
    qemu_irq irqs[4];
    MemoryRegion pci_mmio;
    MemoryRegion pci_hole;
    MemoryRegion pci_io;
};

struct UNINState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    uint32_t version;
    KeyWestI2CState i2c;        /* at +0x1000 on the U3 */
};

#define TYPE_UNI_NORTH "uni-north"
OBJECT_DECLARE_SIMPLE_TYPE(UNINState, UNI_NORTH)

/* U3 HyperTransport host */
#define U3_HT_CFG_BASE      0xf2000000
#define U3_HT_CFG_SIZE      0x02000000
#define U3_HT_IO_BASE       0xf4000000
#define U3_HT_IO_SIZE       0x00400000
#define U3_HT_SELF_BASE     0xf8070000

/* U3 MPIC and its input on the K2 MPIC */
#define U3_MPIC_BASE        0xf8040000
#define U3_MPIC_CASCADE_IRQ 0x38
#define U3_HT_SELF_SIZE     0x1000
#define U3_HT_NUM_IRQS      64

/* AMD-8131 PCI-X bridges at HT devices 1-2, K2 HT-PCI bridges at 3-7 */
#define AMD8131_FIRST_SLOT  1
#define AMD8131_NUM         2
#define K2_HT_PCI_FIRST_SLOT 3
#define K2_HT_PCI_NUM       5
#define TYPE_U3_HT_PCI_BRIDGE "u3-ht-pci-bridge"

OBJECT_DECLARE_SIMPLE_TYPE(U3HTHostState, U3_HT_HOST_BRIDGE)

struct U3HTHostState {
    PCIHostState parent_obj;

    qemu_irq irqs[U3_HT_NUM_IRQS];
    MemoryRegion cfg;
    MemoryRegion self;
    MemoryRegion pci_mmio;
    MemoryRegion pci_io;
    MemoryRegion mem_win[3];
    PCIBridge *pcix[AMD8131_NUM];
    PCIBridge *k2[K2_HT_PCI_NUM];
};

void u3_ht_map(SysBusDevice *sbd);

#endif /* UNINORTH_H */
