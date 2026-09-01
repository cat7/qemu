/*
 * QEMU Grackle PCI host (heathrow OldWorld PowerMac)
 *
 * Copyright (c) 2006-2007 Fabrice Bellard
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
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "trace.h"
#include "qom/object.h"
#include "hw/pci-host/grackle.h"

/*
 * Real Gossamer PCI interrupt wiring (matching DingusPPC's Heathrow
 * IntSrc table, itself derived from the real board): each slot has all
 * its INTx pins tied to one dedicated Heathrow line -- onboard ATI GPU
 * (devfn 0x12) -> line 0x16, slot A1 (0x0D) -> 0x17, B1 (0x0E) -> 0x18,
 * C1 (0x0F) -> 0x19. The four outputs here are wired to Heathrow lines
 * 0x16+i by mac_oldworld.c. The original generic (pin+slot)&3 rotation
 * over lines 0x15-0x18 put every device on the wrong line -- found via
 * the GPU's VBLANK interrupt asserting a line Mac OS never listens on
 * (VBL task queue never ran; frozen mouse pointer while everything
 * else worked). NOTE: enabling this wiring requires the ATI model's
 * pulse-style VBL interrupt (see ati_mach64.c) -- with the older
 * latched-line model, an unhandled early-boot VBL stayed asserted
 * forever and wedged the ROM.
 */
static int pci_grackle_map_irq(PCIDevice *pci_dev, int irq_num)
{
    switch (pci_dev->devfn >> 3) {
    case 0x12:                          /* onboard ATI GPU  -> 0x16 */
        return 0;
    case 0x0d:                          /* PCI slot A1      -> 0x17 */
        return 1;
    case 0x0e:                          /* PCI slot B1      -> 0x18 */
        return 2;
    case 0x0f:                          /* PCI slot C1      -> 0x19 */
        return 3;
    default:
        return (irq_num + (pci_dev->devfn >> 3)) & 3;
    }
}

static void pci_grackle_set_irq(void *opaque, int irq_num, int level)
{
    GrackleState *s = opaque;

    trace_grackle_set_irq(irq_num, level);
    qemu_set_irq(s->irqs[irq_num], level);
}

static void grackle_realize(DeviceState *dev, Error **errp)
{
    GrackleState *s = GRACKLE_PCI_HOST_BRIDGE(dev);
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);

    phb->bus = pci_register_root_bus(dev, NULL,
                                     pci_grackle_set_irq,
                                     pci_grackle_map_irq,
                                     s,
                                     &s->pci_mmio,
                                     &s->pci_io,
                                     0, 4, TYPE_PCI_BUS);

    pci_create_simple(phb->bus, 0, "grackle");
}

static void grackle_init(Object *obj)
{
    GrackleState *s = GRACKLE_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *phb = PCI_HOST_BRIDGE(obj);

    memory_region_init(&s->pci_mmio, OBJECT(s), "pci-mmio", 0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "pci-isa-mmio", 0x00200000);

    memory_region_init_alias(&s->pci_hole, OBJECT(s), "pci-hole", &s->pci_mmio,
                             0x80000000ULL, 0x7e000000ULL);

    memory_region_init_io(&phb->conf_mem, obj, &pci_host_conf_le_ops,
                          DEVICE(obj), "pci-conf-idx", 0x1000);
    memory_region_init_io(&phb->data_mem, obj, &pci_host_data_le_ops,
                          DEVICE(obj), "pci-data-idx", 0x1000);

    sysbus_init_mmio(sbd, &phb->conf_mem);
    sysbus_init_mmio(sbd, &phb->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

/*
 * Real MPC106 ("Grackle") memory-controller bank-decode registers
 * (NXP MPC106UM §5.1). The Beige G3 ROM programs these from the RAM
 * geometry it reads via SPD, then sets MEMGO in MCCR1 to activate the
 * bank(s). DingusPPC (a reference PowerMac emulator that boots this
 * exact ROM successfully) derives its actual usable-RAM range from
 * this register programming rather than trusting the nominal
 * configured RAM size verbatim; QEMU's grackle model has never
 * implemented these registers at all. Track them here so the RAM
 * range implied by the ROM's own programming can be observed via
 * trace, ahead of deciding whether/how it should affect the RAM
 * mapping itself.
 */
enum {
    GRACKLE_MSAR1  = 0x80,
    GRACKLE_MSAR2  = 0x84,
    GRACKLE_EMSAR1 = 0x88,
    GRACKLE_EMSAR2 = 0x8c,
    GRACKLE_MEAR1  = 0x90,
    GRACKLE_MEAR2  = 0x94,
    GRACKLE_EMEAR1 = 0x98,
    GRACKLE_EMEAR2 = 0x9c,
    GRACKLE_MBER   = 0xa0,
    GRACKLE_MCCR1  = 0xf0,
    GRACKLE_MEMGO  = 1 << 19,
};

typedef struct GrackleFunState {
    PCIDevice parent_obj;

    uint32_t mem_start[2];
    uint32_t ext_mem_start[2];
    uint32_t mem_end[2];
    uint32_t ext_mem_end[2];
    uint8_t mem_bank_en;
    uint32_t mccr1;
} GrackleFunState;

/* Mirrors DingusPPC's MPC106::setup_ram() bank-decode arithmetic exactly. */
static void grackle_log_ram_range(GrackleFunState *s)
{
    uint32_t bank_start[8], bank_end[8];
    int bank_order[8];
    int bank_count = 0;
    uint32_t lo, hi;

    for (int bank = 0; bank < 8; bank++) {
        int word, shift;

        if (!(s->mem_bank_en & (1 << bank))) {
            continue;
        }
        word = bank >> 2;
        shift = (bank & 3) * 8;
        bank_start[bank_count] =
            (((s->ext_mem_start[word] >> shift) & 3) << 28) |
            (((s->mem_start[word] >> shift) & 0xff) << 20);
        bank_end[bank_count] =
            (((s->ext_mem_end[word] >> shift) & 3) << 28) |
            (((s->mem_end[word] >> shift) & 0xff) << 20) | 0xfffff;
        bank_order[bank_count] = bank_count;
        bank_count++;
    }
    if (bank_count == 0) {
        trace_grackle_ram_setup(0, 0, 0);
        return;
    }

    for (int i = 0; i < bank_count; i++) {
        for (int j = i + 1; j < bank_count; j++) {
            if (bank_start[bank_order[j]] < bank_start[bank_order[i]]) {
                int tmp = bank_order[i];
                bank_order[i] = bank_order[j];
                bank_order[j] = tmp;
            }
        }
    }

    lo = bank_start[bank_order[0]];
    hi = bank_end[bank_order[0]];
    for (int i = 1; i < bank_count; i++) {
        if (bank_end[bank_order[i]] > hi) {
            hi = bank_end[bank_order[i]];
        }
    }
    trace_grackle_ram_setup(lo, hi, (uint64_t)hi - lo + 1);
}

static void grackle_pci_config_write(PCIDevice *d, uint32_t addr,
                                      uint32_t val, int len)
{
    GrackleFunState *s = (GrackleFunState *)d;

    pci_default_write_config(d, addr, val, len);

    switch (addr & ~3u) {
    case GRACKLE_MSAR1:
        s->mem_start[0] = pci_get_long(d->config + GRACKLE_MSAR1);
        break;
    case GRACKLE_MSAR2:
        s->mem_start[1] = pci_get_long(d->config + GRACKLE_MSAR2);
        break;
    case GRACKLE_EMSAR1:
        s->ext_mem_start[0] = pci_get_long(d->config + GRACKLE_EMSAR1);
        break;
    case GRACKLE_EMSAR2:
        s->ext_mem_start[1] = pci_get_long(d->config + GRACKLE_EMSAR2);
        break;
    case GRACKLE_MEAR1:
        s->mem_end[0] = pci_get_long(d->config + GRACKLE_MEAR1);
        break;
    case GRACKLE_MEAR2:
        s->mem_end[1] = pci_get_long(d->config + GRACKLE_MEAR2);
        break;
    case GRACKLE_EMEAR1:
        s->ext_mem_end[0] = pci_get_long(d->config + GRACKLE_EMEAR1);
        break;
    case GRACKLE_EMEAR2:
        s->ext_mem_end[1] = pci_get_long(d->config + GRACKLE_EMEAR2);
        break;
    case GRACKLE_MBER:
        s->mem_bank_en = pci_get_long(d->config + GRACKLE_MBER) & 0xff;
        break;
    case GRACKLE_MCCR1:
        s->mccr1 = pci_get_long(d->config + GRACKLE_MCCR1);
        if (s->mccr1 & GRACKLE_MEMGO) {
            grackle_log_ram_range(s);
        }
        break;
    default:
        break;
    }
}

static void grackle_pci_realize(PCIDevice *d, Error **errp)
{
    GrackleFunState *s = (GrackleFunState *)d;
    int i;

    d->config[PCI_CLASS_PROG] = 0x01;

    /*
     * The MPC106 memory bank-decode registers must be WRITABLE, and
     * read back what the firmware programs. PCI config space defaults
     * every non-standard offset's wmask to 0 (read-only), so the
     * tracking added for these registers never saw a byte change: the
     * Beige ROM's bank programming was silently dropped and every
     * read-back returned zero -- one reason guest-visible RAM stuck at
     * 512MB regardless of -m (DingusPPC's MPC106 stores and returns
     * all of these; verified against its devices/memctrl/mpc106.cpp).
     */
    for (i = GRACKLE_MSAR1; i <= GRACKLE_EMEAR2; i += 4) {
        pci_set_long(d->wmask + i, 0xffffffff);
    }
    d->wmask[GRACKLE_MBER] = 0xff;          /* 8-bit bank-enable */
    pci_set_long(d->wmask + GRACKLE_MCCR1, 0xffffffff);

    /* Real MPC106 reset default (NXP MPC106UM §5.1.11): 64-bit ROM bus,
     * MEMGO already set. Present it in config space too, so a read
     * before the first write sees the real reset value. */
    s->mccr1 = 0xff820000;
    pci_set_long(d->config + GRACKLE_MCCR1, s->mccr1);
}

static void grackle_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = grackle_pci_realize;
    k->config_write = grackle_pci_config_write;
    k->vendor_id = PCI_VENDOR_ID_MOTOROLA;
    k->device_id = PCI_DEVICE_ID_MOTOROLA_MPC106;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo grackle_pci_info = {
    .name          = "grackle",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GrackleFunState),
    .class_init = grackle_pci_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static char *grackle_ofw_unit_address(const SysBusDevice *dev)
{
    GrackleState *s = GRACKLE_PCI_HOST_BRIDGE(dev);

    return g_strdup_printf("%x", s->ofw_addr);
}

static const Property grackle_properties[] = {
    DEFINE_PROP_UINT32("ofw-addr", GrackleState, ofw_addr, -1),
};

static void grackle_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(klass);

    dc->realize = grackle_realize;
    device_class_set_props(dc, grackle_properties);
    dc->fw_name = "pci";
    sbc->explicit_ofw_unit_address = grackle_ofw_unit_address;
}

static const TypeInfo grackle_host_info = {
    .name          = TYPE_GRACKLE_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(GrackleState),
    .instance_init = grackle_init,
    .class_init    = grackle_class_init,
};

static void grackle_register_types(void)
{
    type_register_static(&grackle_pci_info);
    type_register_static(&grackle_host_info);
}

type_init(grackle_register_types)
