/*
 * Heathrow PIC support (OldWorld PowerMac)
 *
 * Copyright (c) 2005-2007 Fabrice Bellard
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

#ifndef HW_INTC_HEATHROW_PIC_H
#define HW_INTC_HEATHROW_PIC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_HEATHROW "heathrow"
OBJECT_DECLARE_SIMPLE_TYPE(HeathrowState, HEATHROW)

typedef struct HeathrowPICState {
    uint32_t events;
    uint32_t mask;
    uint32_t levels;
    uint32_t level_triggered;
} HeathrowPICState;

struct HeathrowState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    HeathrowPICState pics[2];
    qemu_irq irqs[1];

    /*
     * O'Hare/Heathrow Feature Control Register (offset 0x38): gates
     * clock/enable lines to onboard peripherals (SCC A/B, sound, SCSI,
     * SWIM, IDE...). Real ROMs write this during early native hardware
     * bring-up and some read it back to confirm state.
     */
    uint32_t feat_ctrl;

    /*
     * O'Hare/Heathrow ID register (offset 0x34): a packed, board-strapped
     * identification word -- fp_id (flat-panel ID, bits 31:24), mon_id
     * (monitor ID, bits 23:16), mb_id (media-bay ID, bits 15:8), cpu_id
     * (bits 7:0). These are hardwired pull-up/pull-down straps on real
     * silicon, not something the ROM computes -- confirmed against
     * DingusPPC's register-compatible MacIoTwo (its real, working
     * Gossamer-desktop defaults are cpu_id=0xF0, mb_id=0x70, mon_id=0x10,
     * fp_id=0x70). Leaving this at the reset-default all-zero value (as
     * this register was previously entirely unimplemented) misreports
     * "media bay present, ID 0" and "flat panel present, ID 0" to any ROM
     * code that probes it, instead of the real desktop board's actual
     * "no media bay / no flat panel" strapping.
     */
    uint32_t ohare_id;
};

#define HEATHROW_NUM_IRQS 64

#endif /* HW_INTC_HEATHROW_PIC_H */
