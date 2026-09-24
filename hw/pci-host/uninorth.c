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

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/uninorth.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "trace.h"

static int pci_unin_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 3;
}

static void pci_unin_set_irq(void *opaque, int irq_num, int level)
{
    UNINHostState *s = opaque;

    trace_unin_set_irq(irq_num, level);
    qemu_set_irq(s->irqs[irq_num], level);
}

static uint32_t unin_get_config_reg(uint32_t reg, uint32_t addr)
{
    uint32_t retval;

    if (reg & (1u << 31)) {
        /* XXX OpenBIOS compatibility hack */
        retval = reg | (addr & 3);
    } else if (reg & 1) {
        /* CFA1 style */
        retval = (reg & ~7u) | (addr & 7);
    } else {
        uint32_t slot, func;

        /* Grab CFA0 style values */
        slot = ctz32(reg & 0xfffff800);
        if (slot == 32) {
            slot = -1; /* XXX: should this be 0? */
        }
        func = PCI_FUNC(reg >> 8);

        /* ... and then convert them to x86 format */
        /* config pointer */
        retval = (reg & (0xff - 7)) | (addr & 7);
        /* slot, fn */
        retval |= PCI_DEVFN(slot, func) << 8;
    }

    trace_unin_get_config_reg(reg, addr, retval);

    return retval;
}

static void unin_data_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned len)
{
    UNINHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    trace_unin_data_write(addr, len, val);
    pci_data_write(phb->bus,
                   unin_get_config_reg(phb->config_reg, addr),
                   val, len);
}

static uint64_t unin_data_read(void *opaque, hwaddr addr,
                               unsigned len)
{
    UNINHostState *s = opaque;
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    uint32_t val;

    val = pci_data_read(phb->bus,
                        unin_get_config_reg(phb->config_reg, addr),
                        len);
    trace_unin_data_read(addr, len, val);
    return val;
}

static const MemoryRegionOps unin_data_ops = {
    .read = unin_data_read,
    .write = unin_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static char *pci_unin_main_ofw_unit_address(const SysBusDevice *dev)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(dev);

    return g_strdup_printf("%x", s->ofw_addr);
}

static void pci_unin_main_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "uni-north-pci");

    /*
     * DEC 21154 bridge was unused for many years, this comment is
     * a placeholder for whoever wishes to resurrect it
     */
}

static void pci_unin_main_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Use values found on a real PowerMac */
    /* Uninorth main bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops, obj,
                          "unin-pci-conf-data", 0x1000);

    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-pci-isa-mmio", 0x00800000);

    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-pci-hole", &s->pci_mmio,
                             0x80000000ULL, 0x10000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_u3_agp_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = U3_AGP_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "u3-agp");
}

static void pci_u3_agp_init(Object *obj)
{
    UNINHostState *s = U3_AGP_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth U3 AGP bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops, obj,
                          "unin-pci-conf-data", 0x1000);

    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-pci-isa-mmio", 0x00800000);

    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-pci-hole", &s->pci_mmio,
                             0x90000000ULL, 0x10000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

/*
 * HyperTransport configuration window: type 0 cycles at devfn << 8 | reg,
 * type 1 cycles at 0x01000000 + (bus << 16 | devfn << 8 | reg).
 */
static uint32_t u3_ht_cfg_addr(hwaddr addr)
{
    if (addr & 0x01000000) {
        return addr & 0x00ffffff;
    }
    return addr & 0xffff;
}

static uint64_t u3_ht_cfg_read(void *opaque, hwaddr addr, unsigned len)
{
    PCIHostState *h = opaque;

    return pci_data_read(h->bus, u3_ht_cfg_addr(addr), len);
}

static void u3_ht_cfg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned len)
{
    PCIHostState *h = opaque;

    pci_data_write(h->bus, u3_ht_cfg_addr(addr), val, len);
}

static const MemoryRegionOps u3_ht_cfg_ops = {
    .read = u3_ht_cfg_read,
    .write = u3_ht_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* The host's own configuration space, one register every four bytes */
static uint64_t u3_ht_self_read(void *opaque, hwaddr addr, unsigned len)
{
    PCIDevice *d = opaque;

    return pci_host_config_read_common(d, addr >> 2, PCI_CONFIG_SPACE_SIZE,
                                       len);
}

static void u3_ht_self_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned len)
{
    PCIDevice *d = opaque;

    pci_host_config_write_common(d, addr >> 2, PCI_CONFIG_SPACE_SIZE,
                                 val, len);
}

static const MemoryRegionOps u3_ht_self_ops = {
    .read = u3_ht_self_read,
    .write = u3_ht_self_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * HyperTransport interrupts reach the MPIC input named by the device's
 * bridge; a root bus interrupt number is an MPIC input.
 */
static int u3_ht_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return irq_num;
}

static void u3_ht_set_irq(void *opaque, int irq_num, int level)
{
    U3HTHostState *s = opaque;

    qemu_set_irq(s->irqs[irq_num], level);
}

/* MPIC inputs of the slots behind each HT bridge */
static const struct {
    int ht_slot;
    int slot;
    int irq;
} u3_ht_pci_irqs[] = {
    { 1, 2, 0x34 }, { 1, 3, 0x35 },         /* PCI-X SLOT-2, SLOT-3 */
    { 2, 4, 0x36 },                         /* PCI-X SLOT-4 */
    { 3, 8, 0x1b }, { 3, 9, 0x1c },         /* USB0, USB1 */
    { 4, 11, 0x3f },                        /* USB2 */
    { 5, 13, 0x27 }, { 5, 14, 0x28 },       /* ATA-100, FireWire */
    { 6, 15, 0x29 },                        /* GMAC */
    { 7, 12, 0x11 },                        /* SATA */
};

static int u3_ht_pci_map_irq(PCIDevice *pci_dev, int irq_num)
{
    PCIDevice *br = pci_bridge_get_device(pci_get_bus(pci_dev));
    int ht_slot = PCI_SLOT(br->devfn);
    int slot = PCI_SLOT(pci_dev->devfn);
    int i;

    for (i = 0; i < ARRAY_SIZE(u3_ht_pci_irqs); i++) {
        if (u3_ht_pci_irqs[i].ht_slot == ht_slot &&
            u3_ht_pci_irqs[i].slot == slot) {
            return u3_ht_pci_irqs[i].irq;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "U3 HT: no interrupt for %d:%d\n",
                  ht_slot, slot);
    return 0;
}

static PCIBridge *u3_ht_add_bridge(PCIBus *bus, int ht_slot, uint16_t vendor,
                                   uint16_t device, uint8_t revision,
                                   const char *name)
{
    PCIDevice *d = pci_new(PCI_DEVFN(ht_slot, 0), TYPE_U3_HT_PCI_BRIDGE);
    PCIBridge *br = PCI_BRIDGE(d);

    qdev_prop_set_uint16(DEVICE(d), "vendor-id", vendor);
    qdev_prop_set_uint16(DEVICE(d), "device-id", device);
    qdev_prop_set_uint8(DEVICE(d), "revision", revision);
    pci_bridge_map_irq(br, name, u3_ht_pci_map_irq);
    pci_realize_and_unref(d, bus, &error_fatal);
    return br;
}

/* Memory decoded to HyperTransport, as in the host's decode register */
static const struct {
    hwaddr base;
    hwaddr size;
} u3_ht_mem_windows[3] = {
    { 0x80000000, 0x10000000 },
    { 0xa0000000, 0x50000000 },
    { 0xfa000000, 0x05000000 },
};

static void pci_u3_ht_realize(DeviceState *dev, Error **errp)
{
    U3HTHostState *s = U3_HT_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);
    PCIDevice *d;
    int i;

    h->bus = pci_register_root_bus(dev, NULL,
                                   u3_ht_set_irq, u3_ht_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(0, 0), U3_HT_NUM_IRQS,
                                   TYPE_PCI_BUS);

    d = pci_create_simple(h->bus, PCI_DEVFN(0, 0), "u3-ht");
    memory_region_init_io(&s->self, OBJECT(s), &u3_ht_self_ops, d,
                          "u3-ht-self", U3_HT_SELF_SIZE);

    for (i = 0; i < AMD8131_NUM; i++) {
        g_autofree char *name = g_strdup_printf("pcix%d", i + 1);

        s->pcix[i] = u3_ht_add_bridge(h->bus, AMD8131_FIRST_SLOT + i,
                                      PCI_VENDOR_ID_AMD,
                                      PCI_DEVICE_ID_AMD_8131_BRIDGE, 0x12,
                                      name);
    }
    for (i = 0; i < K2_HT_PCI_NUM; i++) {
        g_autofree char *name = g_strdup_printf("k2-pci%d", i + 1);

        s->k2[i] = u3_ht_add_bridge(h->bus, K2_HT_PCI_FIRST_SLOT + i,
                                    PCI_VENDOR_ID_APPLE,
                                    PCI_DEVICE_ID_APPLE_K2_HT_PCI_1 + i, 0,
                                    name);
    }
}

static void pci_u3_ht_init(Object *obj)
{
    U3HTHostState *s = U3_HT_HOST_BRIDGE(obj);
    int i;

    memory_region_init_io(&s->cfg, obj, &u3_ht_cfg_ops, obj,
                          "u3-ht-cfg", U3_HT_CFG_SIZE);
    memory_region_init(&s->pci_mmio, obj, "u3-ht-mmio", 0x100000000ULL);
    memory_region_init_io(&s->pci_io, obj, &unassigned_io_ops, obj,
                          "u3-ht-io", U3_HT_IO_SIZE);
    for (i = 0; i < ARRAY_SIZE(u3_ht_mem_windows); i++) {
        memory_region_init_alias(&s->mem_win[i], obj, "u3-ht-mem",
                                 &s->pci_mmio, u3_ht_mem_windows[i].base,
                                 u3_ht_mem_windows[i].size);
    }

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

void u3_ht_map(SysBusDevice *sbd)
{
    U3HTHostState *s = U3_HT_HOST_BRIDGE(sbd);
    MemoryRegion *sysmem = get_system_memory();
    int i;

    memory_region_add_subregion(sysmem, U3_HT_CFG_BASE, &s->cfg);
    memory_region_add_subregion(sysmem, U3_HT_SELF_BASE, &s->self);
    memory_region_add_subregion(sysmem, U3_HT_IO_BASE, &s->pci_io);
    for (i = 0; i < ARRAY_SIZE(u3_ht_mem_windows); i++) {
        memory_region_add_subregion(sysmem, u3_ht_mem_windows[i].base,
                                    &s->mem_win[i]);
    }
}

static void pci_unin_agp_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_AGP_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(11, 0), "uni-north-agp");
}

static void pci_unin_agp_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_AGP_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth AGP bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-agp-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &pci_host_data_le_ops,
                          obj, "unin-agp-conf-data", 0x1000);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_unin_internal_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq, pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(14, 0), 4, TYPE_PCI_BUS);

    pci_create_simple(h->bus, PCI_DEVFN(14, 0), "uni-north-internal-pci");
}

static void pci_unin_internal_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth internal bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &pci_host_data_le_ops,
                          obj, "unin-pci-conf-data", 0x1000);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void unin_main_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    d->config[PCI_CAPABILITY_LIST] = 0x00;

    /*
     * Set kMacRISCPCIAddressSelect (0x48) register to indicate PCI
     * memory space with base 0x80000000, size 0x10000000 for Apple's
     * AppleMacRiscPCI driver
     */
    d->config[0x48] = 0x0;
    d->config[0x49] = 0x0;
    d->config[0x4a] = 0x0;
    d->config[0x4b] = 0x1;
}

static void unin_agp_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    /* d->config[PCI_CAPABILITY_LIST] = 0x80; */
}

static void u3_agp_pci_host_realize(PCIDevice *d, Error **errp)
{
    int cap;

    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;

    /* AGP 3.0 at 0x80, as the U3 reports; Mac OS X's AGP driver needs it */
    cap = pci_add_capability(d, PCI_CAP_ID_AGP, 0x80, PCI_AGP_SIZEOF, errp);
    if (cap < 0) {
        return;
    }
    d->config[cap + PCI_AGP_VERSION] = 0x30;
    /* 32 requests, sideband, AGP 3.0 mode (bit 3) where RATE1/2 are 4x/8x */
    pci_set_long(d->config + cap + PCI_AGP_STATUS,
                 0x1f000000 | PCI_AGP_STATUS_SBA | (1 << 3) |
                 PCI_AGP_STATUS_RATE2 | PCI_AGP_STATUS_RATE1);

    /* address select: one bit per 256 MB region decoded to AGP */
    pci_set_long(d->config + 0x48, 1u << (16 + 9));
    pci_set_long(d->wmask + 0x48, 0);
}

/* Decode register: 16M at 0xfa000000-0xfeffffff, 256M at 0x8 and 0xa-0xe */
#define U3_HT_DECODE    0x003f00be

static void u3_ht_pci_host_realize(PCIDevice *d, Error **errp)
{
    pci_set_long(d->config + 0x80, U3_HT_DECODE);
    pci_set_long(d->wmask + 0x80, 0);
}

static void unin_internal_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
    d->config[PCI_CAPABILITY_LIST] = 0x00;
}

static void unin_main_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_main_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_PCI;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_main_pci_host_info = {
    .name = "uni-north-pci",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_main_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void u3_agp_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = u3_agp_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_U3_AGP;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo u3_agp_pci_host_info = {
    .name = "u3-agp",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = u3_agp_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void u3_ht_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = u3_ht_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_U3_HT;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    dc->user_creatable = false;
}

static const TypeInfo u3_ht_pci_host_info = {
    .name = "u3-ht",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = u3_ht_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

/* A PCI-PCI bridge on the U3's HyperTransport: K2 HT-PCI or AMD-8131 */
typedef struct U3HTPCIBridge {
    PCIBridge parent_obj;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
} U3HTPCIBridge;

OBJECT_DECLARE_SIMPLE_TYPE(U3HTPCIBridge, U3_HT_PCI_BRIDGE)

static void u3_ht_pci_bridge_realize(PCIDevice *d, Error **errp)
{
    U3HTPCIBridge *br = U3_HT_PCI_BRIDGE(d);

    pci_bridge_initfn(d, TYPE_PCI_BUS);
    pci_config_set_vendor_id(d->config, br->vendor_id);
    pci_config_set_device_id(d->config, br->device_id);
    pci_config_set_revision(d->config, br->revision);
}

static const Property u3_ht_pci_bridge_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", U3HTPCIBridge, vendor_id,
                       PCI_VENDOR_ID_APPLE),
    DEFINE_PROP_UINT16("device-id", U3HTPCIBridge, device_id,
                       PCI_DEVICE_ID_APPLE_K2_HT_PCI_1),
    DEFINE_PROP_UINT8("revision", U3HTPCIBridge, revision, 0),
};

static void u3_ht_pci_bridge_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = u3_ht_pci_bridge_realize;
    k->exit = pci_bridge_exitfn;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_K2_HT_PCI_1;
    k->config_write = pci_bridge_write_config;
    device_class_set_legacy_reset(dc, pci_bridge_reset);
    device_class_set_props(dc, u3_ht_pci_bridge_properties);
    dc->vmsd = &vmstate_pci_device;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo u3_ht_pci_bridge_info = {
    .name          = TYPE_U3_HT_PCI_BRIDGE,
    .parent        = TYPE_PCI_BRIDGE,
    .instance_size = sizeof(U3HTPCIBridge),
    .class_init    = u3_ht_pci_bridge_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void unin_agp_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_agp_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_AGP;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_agp_pci_host_info = {
    .name = "uni-north-agp",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_agp_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void unin_internal_pci_host_class_init(ObjectClass *klass,
                                              const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_internal_pci_host_realize;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_UNI_N_I_PCI;
    k->revision  = 0x00;
    k->class_id  = PCI_CLASS_BRIDGE_HOST;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo unin_internal_pci_host_info = {
    .name = "uni-north-internal-pci",
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIDevice),
    .class_init = unin_internal_pci_host_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const Property pci_unin_main_pci_host_props[] = {
    DEFINE_PROP_UINT32("ofw-addr", UNINHostState, ofw_addr, -1),
};

static void pci_unin_main_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(klass);

    dc->realize = pci_unin_main_realize;
    device_class_set_props(dc, pci_unin_main_pci_host_props);
    dc->fw_name = "pci";
    sbc->explicit_ofw_unit_address = pci_unin_main_ofw_unit_address;
}

static const TypeInfo pci_unin_main_info = {
    .name          = TYPE_UNI_NORTH_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_main_init,
    .class_init    = pci_unin_main_class_init,
};

static void pci_u3_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_u3_agp_realize;
}

static void pci_u3_ht_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_u3_ht_realize;
}

static const TypeInfo pci_u3_ht_info = {
    .name          = TYPE_U3_HT_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(U3HTHostState),
    .instance_init = pci_u3_ht_init,
    .class_init    = pci_u3_ht_class_init,
};

static const TypeInfo pci_u3_agp_info = {
    .name          = TYPE_U3_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_u3_agp_init,
    .class_init    = pci_u3_agp_class_init,
};

static void pci_unin_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_agp_realize;
}

static const TypeInfo pci_unin_agp_info = {
    .name          = TYPE_UNI_NORTH_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_agp_init,
    .class_init    = pci_unin_agp_class_init,
};

static void pci_unin_internal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_internal_realize;
}

static const TypeInfo pci_unin_internal_info = {
    .name          = TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_internal_init,
    .class_init    = pci_unin_internal_class_init,
};

/* UniN device */
static void unin_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    trace_unin_write(addr, value);
}

static uint64_t unin_read(void *opaque, hwaddr addr, unsigned size)
{
    UNINState *s = opaque;
    uint32_t value;

    switch (addr) {
    case 0:
        value = s->version;
        break;
    default:
        value = 0;
    }

    trace_unin_read(addr, value);

    return value;
}

static const MemoryRegionOps unin_ops = {
    .read = unin_read,
    .write = unin_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void unin_init(Object *obj)
{
    UNINState *s = UNI_NORTH(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &unin_ops, s, "unin", 0x1000);
    keywest_i2c_init(&s->i2c, DEVICE(obj), "unin-i2c", 0x1000);

    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_mmio(sbd, &s->i2c.mem);
}

static const Property unin_properties[] = {
    DEFINE_PROP_UINT32("version", UNINState, version, UNINORTH_VERSION_10A),
};

static void unin_reset(DeviceState *dev)
{
    keywest_i2c_reset(&UNI_NORTH(dev)->i2c);
}

static void unin_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, unin_reset);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    device_class_set_props(dc, unin_properties);
}

static const TypeInfo unin_info = {
    .name          = TYPE_UNI_NORTH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UNINState),
    .instance_init = unin_init,
    .class_init    = unin_class_init,
};

static void unin_register_types(void)
{
    type_register_static(&unin_main_pci_host_info);
    type_register_static(&u3_agp_pci_host_info);
    type_register_static(&u3_ht_pci_host_info);
    type_register_static(&u3_ht_pci_bridge_info);
    type_register_static(&unin_agp_pci_host_info);
    type_register_static(&unin_internal_pci_host_info);

    type_register_static(&pci_unin_main_info);
    type_register_static(&pci_u3_agp_info);
    type_register_static(&pci_u3_ht_info);
    type_register_static(&pci_unin_agp_info);
    type_register_static(&pci_unin_internal_info);

    type_register_static(&unin_info);
}

type_init(unin_register_types)
