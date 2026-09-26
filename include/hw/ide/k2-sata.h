/*
 * ServerWorks/Broadcom K2 SATA controller
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_IDE_K2_SATA_H
#define HW_IDE_K2_SATA_H

#include "hw/ide/pci.h"
#include "qom/object.h"

#define TYPE_K2_SATA "k2-sata"
OBJECT_DECLARE_SIMPLE_TYPE(K2SATAState, K2_SATA)

/* Ports with a SATA connector */
#define K2_SATA_NUM_PORTS 2
/* Ports with a register block */
#define K2_SATA_NUM_REGS 4

struct K2SATAState {
    PCIIDEState parent_obj;

    MemoryRegion mmio;
    uint32_t serror[K2_SATA_NUM_REGS];
    uint32_t scontrol[K2_SATA_NUM_REGS];
    uint32_t sicr1[K2_SATA_NUM_REGS];
    uint32_t sicr2[K2_SATA_NUM_REGS];
    uint32_t sim[K2_SATA_NUM_REGS];
    uint32_t mdio[K2_SATA_NUM_REGS];
    uint32_t irq_pending;
};

/* hd_table holds one drive per port */
void k2_sata_init_drives(K2SATAState *s, DriveInfo **hd_table);

#endif
