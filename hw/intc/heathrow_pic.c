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

#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/intc/heathrow_pic.h"
#include "hw/core/irq.h"
#include "trace.h"

/*
 * Bit 31 of the mask register doubles as a mode flag on real Heathrow/
 * Grand Central silicon: 0 selects "native" PowerPC interrupt handling
 * (events latch on a 0->1 transition only, cleared bit-by-bit by
 * whatever the ack write specifies), 1 selects "68k-style" handling
 * used by the classic Mac nanokernel/68K emulator during early boot.
 * In 68k-style mode, an ack write that itself has bit 31 set is a
 * "clear everything" acknowledgement, not a request to clear bit 31
 * specifically (there is no real IRQ source at bit 31 -- confirmed
 * against DingusPPC's register-compatible grandcentral.cpp, which
 * models this exact convention as MACIO_INT_MODE/MACIO_INT_CLR).
 * Without this, an edge-latched source acked this way (e.g. CUDA's
 * VIA IRQ) never actually clears and the native interrupt-dispatch
 * handler re-enters forever.
 */
#define HEATHROW_INT_MODE_68K 0x80000000

static inline int heathrow_check_irq(HeathrowPICState *pic)
{
    return (pic->events | (pic->levels & pic->level_triggered)) & pic->mask;
}

/* update the CPU irq state */
static void heathrow_update_irq(HeathrowState *s)
{
    if (heathrow_check_irq(&s->pics[0]) ||
            heathrow_check_irq(&s->pics[1])) {
        qemu_irq_raise(s->irqs[0]);
    } else {
        qemu_irq_lower(s->irqs[0]);
    }
}

static void heathrow_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    HeathrowState *s = opaque;
    HeathrowPICState *pic;
    unsigned int n;

    if ((addr & 0xfff) == 0x38) {
        trace_heathrow_feat_ctrl_write(value);
        s->feat_ctrl = value;
        return;
    }

    if ((addr & 0xfff) == 0x34) {
        /*
         * Read-only board-strapped ID register on real silicon; writes
         * are ignored (matches DingusPPC's MacIoTwo, which only logs
         * such writes without ever changing cpu_id/mb_id/mon_id/fp_id).
         */
        trace_heathrow_ohare_id_write(value);
        return;
    }

    n = ((addr & 0xfff) - 0x10) >> 4;
    trace_heathrow_write(addr, n, value);
    if (n >= 2)
        return;
    pic = &s->pics[n];
    switch(addr & 0xf) {
    case 0x04:
        pic->mask = value;
        heathrow_update_irq(s);
        break;
    case 0x08:
        /*
         * The 68k-style-acknowledge mode flag is chip-global, selected
         * by bit 31 of the PRIMARY bank's mask register (IRQs 0-31,
         * register block 0x20 = pics[1]); there is no separate mode bit
         * for the auxiliary bank. Mac OS sets the flag once in the
         * primary mask (observed: mask1=0x845ff80c, mask2=0x402) and
         * then acks BOTH banks with 0x80000000 clear-all writes --
         * gating per-bank on the bank's own mask left every aux-bank
         * ack a silent no-op, so an edge-latched aux event (e.g. the
         * bmac ethernet IRQ 0x2a) could never be acknowledged at all.
         */
        if ((s->pics[1].mask & HEATHROW_INT_MODE_68K) &&
            (value & HEATHROW_INT_MODE_68K)) {
            /* 68k-style "clear everything" acknowledgement */
            pic->events = 0;
        } else {
            /*
             * DMA-channel completion interrupts (e.g. IDE0/IDE1 DBDMA,
             * MESH SCSI DMA) have no acknowledge bit of their own in
             * the DBDMA channel's own registers -- real silicon (and
             * DingusPPC's register-compatible MacIoBase::clear_dma_int())
             * has the guest's normal interrupt ack write here also drop
             * the raw level bits for these sources. Without this, such
             * a source's level latches permanently once asserted (there
             * is nothing else that ever lowers it), and a native
             * dispatch loop waiting for that level to clear spins
             * forever even though the DMA transfer itself completed.
             *
             * The GPU line (0x16, bit 22) is included for a related
             * reason: the ATI holds its request until its INT_CNTL is
             * acked (as real silicon does), but Mac OS X Server 1.x's
             * Rhapsody-era driver never acks the device at all
             * (measured: 6 device acks in 26k VBL interrupts) -- with
             * the line then held for the whole blanking window, its
             * level-triggered ISR re-entered in a storm that burned
             * ~12.5% of guest CPU and halved its wall clock. Dropping
             * the level at the guest's own PIC acknowledgment cannot
             * race dispatch visibility (the guest has, by definition,
             * already seen the interrupt it is acking), and guests
             * that do ack the device (Mac OS X 10.2's NDRV path)
             * deassert through the device first anyway.
             */
            pic->levels &= ~(value & (0x7ff | (1 << 0x16)));
            /*
             * Acks clear latched events for every source, matching the
             * latch change above (DingusPPC clears int_events by the
             * written mask with no level-triggered exclusion). A
             * still-asserted level keeps the interrupt pending anyway
             * through the levels term in heathrow_check_irq().
             */
            pic->events &= ~value;
        }
        heathrow_update_irq(s);
        break;
    default:
        break;
    }
}

static uint64_t heathrow_read(void *opaque, hwaddr addr,
                              unsigned size)
{
    HeathrowState *s = opaque;
    HeathrowPICState *pic;
    unsigned int n;
    uint32_t value;

    if ((addr & 0xfff) == 0x38) {
        trace_heathrow_feat_ctrl_read(s->feat_ctrl);
        return s->feat_ctrl;
    }

    if ((addr & 0xfff) == 0x34) {
        trace_heathrow_ohare_id_read(s->ohare_id);
        return s->ohare_id;
    }

    n = ((addr & 0xfff) - 0x10) >> 4;
    if (n >= 2) {
        value = 0;
    } else {
        pic = &s->pics[n];
        switch(addr & 0xf) {
        case 0x0:
            value = pic->events;
            break;
        case 0x4:
            value = pic->mask;
            break;
        case 0xc:
            /*
             * NOTE (2026-07-30): masking this down to
             * (levels & level_triggered) was tried as an interrupt-storm
             * fix and REVERTED: early ROM boot legitimately polls raw
             * input lines here (Ticks never started, gray-screen hang).
             * The storm was fully explained by the bank-0 acknowledge
             * bug fixed in the write handler below instead.
             */
            value = pic->levels;
            break;
        default:
            value = 0;
            break;
        }
    }
    trace_heathrow_read(addr, n, value);
    return value;
}

static const MemoryRegionOps heathrow_ops = {
    .read = heathrow_read,
    .write = heathrow_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void heathrow_set_irq(void *opaque, int num, int level)
{
    HeathrowState *s = opaque;
    HeathrowPICState *pic;
    unsigned int irq_bit;
    int last_level;

    pic = &s->pics[1 - (num >> 5)];
    irq_bit = 1 << (num & 0x1f);
    last_level = (pic->levels & irq_bit) ? 1 : 0;

    if (level) {
        /*
         * Edge-latch on a genuine 0->1 transition only (see the
         * HEATHROW_INT_MODE_68K comment above) -- confirmed against
         * DingusPPC's register-compatible MacIoBase::ack_int_common(),
         * which gates its own int_events latching the same way
         * (irq_line_state && !(int_levels & irq_id)). Without this
         * gate, a source whose model re-calls qemu_irq_raise() on
         * every relevant register access while its line is already
         * held high (e.g. mos6522_update_irq(), called from many
         * unrelated VIA register accesses whenever IFR&IER is still
         * nonzero) re-latches events on every such redundant call --
         * including while the guest's own ISR is still in the middle
         * of servicing/acking the very same interrupt, making it look
         * permanently pending and causing an infinite re-entry storm.
         */
        if (!last_level) {
            /*
             * Latch an event for EVERY source on a rising edge --
             * including level-triggered ones. Upstream QEMU excluded
             * level-triggered lines here (events |= bit &
             * ~level_triggered), leaving them visible ONLY while the
             * line is physically high; DingusPPC's real-hardware-
             * verified GrandCentral::ack_int_common() latches
             * int_events on a 0-to-1 transition for all sources with
             * no such exclusion. The exclusion made the ATI VBL
             * interrupt (level-triggered GPU line 0x16) invisible to
             * any guest whose dispatcher reads the events register or
             * that samples after the line dropped -- the root cause
             * under a whole family of VBL-starvation bugs (classic
             * Mac OS CrsrVBLTask stalls, Mac OS X frozen-idle-cursor,
             * Rhapsody screen-refresh/clock problems).
             */
            pic->events |= irq_bit;
        }
        pic->levels |= irq_bit;
    } else {
        /*
         * NOTE (2026-07-29): re-tested both-edges latching (DingusPPC's
         * MacIoBase::ack_int_common() approach) again, this time with an
         * objective, quantified A/B test -- live CPU-level External
         * Interrupt rate via a TCG plugin, not a screenshot judgment call.
         * Result: no measurable change (~2-5 interrupts per 2M
         * instructions either way) and the idle-loop hang this was meant
         * to fix still consumed ~25% of all instructions continuously
         * with the change in place. Confirms the original 2026-07-28
         * revert's conclusion was correct. Do not re-add without new
         * evidence this actually changes the interrupt rate.
         */
        pic->levels &= ~irq_bit;
    }

    if (last_level != level) {
        trace_heathrow_set_irq(num, level);
    }

    heathrow_update_irq(s);
}

static const VMStateDescription vmstate_heathrow_pic_one = {
    .name = "heathrow_pic_one",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(events, HeathrowPICState),
        VMSTATE_UINT32(mask, HeathrowPICState),
        VMSTATE_UINT32(levels, HeathrowPICState),
        VMSTATE_UINT32(level_triggered, HeathrowPICState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_heathrow = {
    .name = "heathrow_pic",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(pics, HeathrowState, 2, 1,
                             vmstate_heathrow_pic_one, HeathrowPICState),
        VMSTATE_UINT32(feat_ctrl, HeathrowState),
        VMSTATE_UINT32(ohare_id, HeathrowState),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Board-strapped O'Hare/Heathrow ID register reset value for the g3beige
 * (Gossamer desktop) board: fp_id=0x70 (no flat panel), mon_id=0x10,
 * mb_id=0x70 (no media bay), cpu_id=0xF0 -- matches DingusPPC's real,
 * working MacIoTwo defaults for this exact machine.
 */
#define HEATHROW_OHARE_ID_GOSSAMER_DESKTOP 0x701070f0

static void heathrow_reset(DeviceState *d)
{
    HeathrowState *s = HEATHROW(d);

    s->pics[0].level_triggered = 0;
    s->pics[1].level_triggered = 0x1ff00000;
    s->feat_ctrl = 0;
    s->ohare_id = HEATHROW_OHARE_ID_GOSSAMER_DESKTOP;
}

static void heathrow_init(Object *obj)
{
    HeathrowState *s = HEATHROW(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* only 1 CPU */
    qdev_init_gpio_out(DEVICE(obj), s->irqs, 1);

    qdev_init_gpio_in(DEVICE(obj), heathrow_set_irq, HEATHROW_NUM_IRQS);

    memory_region_init_io(&s->mem, OBJECT(s), &heathrow_ops, s,
                          "heathrow-pic", 0x1000);
    sysbus_init_mmio(sbd, &s->mem);
}

static void heathrow_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, heathrow_reset);
    dc->vmsd = &vmstate_heathrow;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo heathrow_type_info = {
    .name = TYPE_HEATHROW,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HeathrowState),
    .instance_init = heathrow_init,
    .class_init = heathrow_class_init,
};

static void heathrow_register_types(void)
{
    type_register_static(&heathrow_type_info);
}

type_init(heathrow_register_types)
