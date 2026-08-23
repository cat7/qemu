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
#include "trace.h"

/*
 * UniNorth's AGP GART. The three registers below live in the AGP host
 * bridge's config space directly after its AGP capability block (which
 * ends at 0x8b), and were read off a live Mac OS 9 boot programming
 * them in this order: GART_BASE, then AGP_BASE, then a 2xRESET pulse,
 * then ENABLE|INVALIDATE, then ENABLE.
 *
 * GART_BASE holds the physical address of the translation table in its
 * top 20 bits, with an aperture-size code in the low bits: the table
 * has code * 1024 entries, each mapping one 4KB page, so the aperture
 * spans code * 4MB (observed: code 8 = 8192 entries = 32MB).
 *
 * AGP_BASE is the aperture's base address on the AGP bus. Real
 * UniNorth cannot place the aperture anywhere but bus address 0, and
 * Mac OS duly writes 0 -- but honour whatever it writes rather than
 * assuming, so a guest that does something else still works.
 */
#define TYPE_UNIN_AGP_IOMMU_MEMORY_REGION "unin-agp-iommu-memory-region"

#define UNIN_CFG_GART_BASE      0x8c
#define UNIN_CFG_AGP_BASE       0x90
#define UNIN_CFG_GART_CTRL      0x94

#define UNIN_GART_CTRL_INVAL    0x00000001
#define UNIN_GART_CTRL_ENABLE   0x00000100
#define UNIN_GART_CTRL_2XRESET  0x00010000

#define UNIN_GART_PAGE_SIZE     4096
#define UNIN_GART_PAGE_MASK     (UNIN_GART_PAGE_SIZE - 1)

static IOMMUTLBEntry unin_agp_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                        IOMMUAccessFlags flag, int iommu_idx)
{
    UNINHostState *s = container_of(iommu, UNINHostState, agp_iommu);
    uint32_t entries = (s->gart_base & UNIN_GART_PAGE_MASK) * 1024;
    uint64_t apsize = (uint64_t)entries * UNIN_GART_PAGE_SIZE;
    hwaddr table, offset;
    uint32_t entry = 0;
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~(hwaddr)UNIN_GART_PAGE_MASK,
        .translated_addr = addr & ~(hwaddr)UNIN_GART_PAGE_MASK,
        .addr_mask = UNIN_GART_PAGE_MASK,
        .perm = IOMMU_RW,
    };

    /*
     * Anything outside a live aperture is a plain system-memory access:
     * an AGP master's ordinary DMA is not translated, only the window
     * the GART describes is.
     */
    if (!(s->gart_ctrl & UNIN_GART_CTRL_ENABLE) || !entries ||
        addr < s->agp_base || addr - s->agp_base >= apsize) {
        return ret;
    }

    table = s->gart_base & ~(hwaddr)UNIN_GART_PAGE_MASK;
    offset = (addr - s->agp_base) >> 12;
    address_space_read(&address_space_memory, table + offset * 4,
                       MEMTXATTRS_UNSPECIFIED, &entry, sizeof(entry));
    /*
     * A GART entry is a little-endian word holding the mapped page's
     * physical address with bit 0 as its valid flag -- read off a live
     * Mac OS 9 table, whose entries ran 0x015c8001, 0x015c9001,
     * 0x015ca001 ... , i.e. consecutive 4KB pages each tagged valid.
     * An unmapped page translates nowhere; returning IOMMU_NONE fails
     * the access instead of silently landing on page 0.
     */
    entry = le32_to_cpu(entry);
    trace_unin_agp_gart_translate(addr, table + offset * 4, entry);
    if (!(entry & 1)) {
        ret.perm = IOMMU_NONE;
        return ret;
    }
    ret.translated_addr = entry & ~(hwaddr)UNIN_GART_PAGE_MASK;
    return ret;
}

static AddressSpace *unin_agp_dma_address_space(PCIBus *bus, void *opaque,
                                                int devfn)
{
    UNINHostState *s = opaque;

    return &s->agp_dma_as;
}

static const PCIIOMMUOps unin_agp_iommu_ops = {
    .get_address_space = unin_agp_dma_address_space,
};

static int pci_unin_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return (irq_num + (pci_dev->devfn >> 3)) & 3;
}

/*
 * Slot-to-line mapping of the real PowerMac3,4's 33MHz PCI bus
 * (pci@f2000000), from a real machine's interrupt-map property:
 *
 *   slot 0x12 -> 0x34    slot 0x15 -> 0x3a    slot 0x1a -> 0x3f
 *   slot 0x13 -> 0x35    slot 0x18 -> 0x1b
 *   slot 0x14 -> 0x36    slot 0x19 -> 0x1c
 *
 * The Apple ROM hands the guest exactly that table, so when it provides
 * the device tree the devices must interrupt on those OpenPIC inputs --
 * the driver unmasks what the tree says, not what our default hash
 * picks. (Observed live: Mac OS X's Rage 128 driver at slot 0x15
 * enabled its VBLANK interrupt and waited forever while we pulsed
 * 0x1c.) The returned index is the position in the machine's wiring
 * array, which connects them in the order listed above; unknown slots
 * get the unwired spare line 7.
 */
static int pci_unin_main_real_map_irq(PCIDevice *pci_dev, int irq_num)
{
    switch (pci_dev->devfn >> 3) {
    case 0x12: return 0;
    case 0x13: return 1;
    case 0x14: return 2;
    case 0x15: return 3;
    case 0x18: return 4;
    case 0x19: return 5;
    case 0x1a: return 6;
    default:   return 7;
    }
}

/*
 * Real pci@f4000000 interrupt-map: slot 0x0e -> 0x28, slot 0x0f (the
 * on-board GMAC) -> 0x29; nothing else lives on this bus.
 */
static int pci_unin_internal_real_map_irq(PCIDevice *pci_dev, int irq_num)
{
    switch (pci_dev->devfn >> 3) {
    case 0x0e: return 0;
    case 0x0f: return 1;
    default:   return 7;
    }
}

/*
 * Real pci@f0000000 interrupt-map: the AGP slot is device 0x10 and its
 * INTA lands on 0x30. That is the whole table -- the mask only matches
 * the device field, and the bridge at device 0x0b interrupts through
 * its own "interrupts" property instead.
 *
 * A card here must interrupt where that table says, because the Apple
 * ROM hands the guest the table and its driver unmasks whatever it
 * names: Mac OS 9's Rage 128 driver enables the VBLANK interrupt and
 * then waits for source 0x30, so pulsing the default-swizzle line
 * instead left the cursor's VBL task never running -- the pointer sat
 * where it started while the mouse's reports arrived and were ignored.
 */
static int pci_unin_agp_real_map_irq(PCIDevice *pci_dev, int irq_num)
{
    switch (pci_dev->devfn >> 3) {
    case 0x10: return 0;
    default:   return 7;
    }
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
    uint32_t config_addr = unin_get_config_reg(phb->config_reg, addr);
    uint32_t val;

    /*
     * Real PowerMac PCI bridges legitimately have some genuinely
     * unpopulated device numbers -- e.g. device 11 is empty on both the
     * main and internal buses of a real PowerMac3,4 (confirmed against
     * this project's own real-hardware device-tree dump: pci@f4000000's
     * only two children are ethernet@f and firewire@e, nothing at
     * device 11), the same device number AppleMacRiscPCI::configure()
     * checks on every bridge (main/internal/AGP alike) for an AGP
     * capability. Its capability-chain walker, findPCICapability(), has
     * no defense against a genuine "no device" response: an all-1s
     * PCI_STATUS falsely looks like "capability list present" (its bit
     * 0x10), and the chain it then tries to walk never finds the byte-0
     * terminator its only loop-exit test looks for, spinning forever.
     * This is presumably a dormant kernel bug real hardware never
     * exercises for reasons specific to that silicon this emulation
     * doesn't replicate -- confirmed via live kernel-symbol tracing
     * during a genuine OS X 10.0.3/10.1.3 boot hang, see the mac99
     * project notes. Report a clean, capability-free response instead of
     * the generic empty-slot sentinel whenever nothing is actually at
     * the target devfn, letting the capability walker's own early-exit
     * path do the right thing.
     */
    if (!pci_find_device(phb->bus, (config_addr >> 16) & 0xff,
                         (config_addr >> 8) & 0xff)) {
        trace_unin_data_read(addr, len, 0);
        return 0;
    }

    val = pci_data_read(phb->bus, config_addr, len);
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
                                   pci_unin_set_irq,
                                   s->real_irq_map ?
                                       pci_unin_main_real_map_irq :
                                       pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0),
                                   s->real_irq_map ? 8 : 4, TYPE_PCI_BUS);

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
                                   pci_unin_set_irq,
                                   s->real_irq_map ?
                                       pci_unin_agp_real_map_irq :
                                       pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(11, 0),
                                   s->real_irq_map ? 8 : 4, TYPE_PCI_BUS);

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
                             0x80000000ULL, 0x70000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
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

    /*
     * Route this bus's DMA through the GART (see unin_agp_translate()).
     * With the GART disabled every access passes straight through, so
     * this is transparent until a guest programs it.
     */
    memory_region_init_iommu(&s->agp_iommu, sizeof(s->agp_iommu),
                             TYPE_UNIN_AGP_IOMMU_MEMORY_REGION, OBJECT(s),
                             "unin-agp-iommu", UINT64_MAX);
    address_space_init(&s->agp_dma_as, MEMORY_REGION(&s->agp_iommu),
                       "unin-agp-dma");
    pci_setup_iommu(h->bus, &unin_agp_iommu_ops, s);

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
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops,
                          obj, "unin-agp-conf-data", 0x1000);

    /*
     * The AGP bus has its own MMIO and I/O address spaces. A real
     * PowerMac3,4 /pci@f0000000 carries an 8 MB I/O window at 0xf0000000
     * (see its "ranges"); without it, 68K display/AGP drivers loaded from
     * the Mac OS 9 CD touch AGP I/O ports that hit unmapped memory,
     * spinning the boot in an unhandled-DSI storm at "Starting Up...".
     * Mirror the main bus so those accesses resolve (empty => reads -1).
     * pci_mmio also backs the bus's memory space handed to
     * pci_register_root_bus() below, which was previously left
     * uninitialised.
     */
    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-agp-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-agp-isa-mmio", 0x00800000);

    /*
     * The AGP bus's memory window. Without it pci_mmio was a private
     * address space nothing ever aliased into system memory, so a BAR
     * on this bus was assigned an address the CPU could not reach: the
     * card answered config cycles and got real BAR values, but every
     * access to its framebuffer or registers went to unmapped memory,
     * leaving an AGP-slot display permanently dark.
     *
     * The main bridge's hole occupies 0x80000000-0x8fffffff, and real
     * UniNorth2 systems place the AGP slot's window immediately after
     * it at 0x90000000 -- which is also where this machine's Apple ROM
     * assigns BARs on this bus (observed: 0x90000000 and 0x94000000).
     * Reserve 512 MiB, covering the real PowerMac3,4 tree's own AGP
     * prefetchable range at 0xa0000000 as well.
     */
    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-agp-hole", &s->pci_mmio,
                             0x90000000ULL, 0x20000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);
    sysbus_init_mmio(sbd, &s->pci_io);

    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static void pci_unin_internal_realize(DeviceState *dev, Error **errp)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);

    h->bus = pci_register_root_bus(dev, NULL,
                                   pci_unin_set_irq,
                                   s->real_irq_map ?
                                       pci_unin_internal_real_map_irq :
                                       pci_unin_map_irq,
                                   s,
                                   &s->pci_mmio,
                                   &s->pci_io,
                                   PCI_DEVFN(14, 0),
                                   s->real_irq_map ? 8 : 4, TYPE_PCI_BUS);

    /*
     * On a real PowerMac3,4, device 14 function 0 of this bus is not a
     * generic bridge self-function -- it's the onboard Lucent/Agere
     * FireWire OHCI controller (/pci@f4000000/firewire@e, vendor:device
     * 11c1:5811, confirmed against a real machine's device-tree dump).
     * Leave that slot free for it there instead of shadowing it with this
     * placeholder.
     */
    if (!s->no_self_func) {
        pci_create_simple(h->bus, PCI_DEVFN(14, 0), "uni-north-internal-pci");
    }
}

static void pci_unin_internal_init(Object *obj)
{
    UNINHostState *s = UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    /* Uninorth internal bus */
    memory_region_init_io(&h->conf_mem, OBJECT(h), &pci_host_conf_le_ops,
                          obj, "unin-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, OBJECT(h), &unin_data_ops,
                          obj, "unin-pci-conf-data", 0x1000);

    /*
     * The internal bus previously handed pci_register_root_bus()
     * pointers to never-initialized regions -- harmless while nothing
     * lived on the bus, fatal the moment a device (GMAC) does. The real
     * PowerMac3,4's pci@f4000000 forwards one 16MB memory window at
     * 0xf5000000 (its own `ranges` property); expose exactly that as
     * mmio region 2 for the machine to map.
     */
    memory_region_init(&s->pci_mmio, OBJECT(s), "unin-internal-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, obj,
                          "unin-internal-pci-isa-mmio", 0x00800000);
    memory_region_init_alias(&s->pci_hole, OBJECT(s),
                             "unin-internal-pci-hole", &s->pci_mmio,
                             0xf5000000ULL, 0x01000000ULL);

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->pci_hole);

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

    /*
     * AppleMacRiscAGP walks the PCI capability list on this bridge's own
     * config space looking for the standard AGP capability (status/command
     * registers describing rate, sideband addressing, queue depth) during
     * its own bring-up -- independent of whatever card sits in the AGP
     * slot. With no capability advertised at all, the driver has nothing
     * to find and spins forever. Values: AGP 2.0, 1x/2x/4x + SBA
     * supported, RQ depth 32 -- unremarkable, real-hardware-typical
     * settings a driver's sanity checks accept; AGPCMD starts at 0
     * (nothing enabled yet, same as real post-reset state).
     */
    d->config[PCI_STATUS] |= PCI_STATUS_CAP_LIST;
    d->config[PCI_CAPABILITY_LIST] = 0x80;
    d->config[0x80] = PCI_CAP_ID_AGP;
    d->config[0x81] = 0x00;         /* next capability pointer: end of list */
    d->config[0x82] = 0x20;         /* AGP revision: major 2, minor 0 */
    d->config[0x83] = 0x00;         /* reserved */
    d->config[0x84] = 0x07;         /* AGPSTAT: RATE1x | RATE2x | RATE4x */
    d->config[0x85] = 0x02;         /* AGPSTAT: SBA supported (bit 9) */
    d->config[0x86] = 0x00;
    d->config[0x87] = 0x20;         /* AGPSTAT: RQ = 32 (bits 31:24) */
    d->config[0x88] = 0x00;         /* AGPCMD: nothing enabled yet */
    d->config[0x89] = 0x00;
    d->config[0x8a] = 0x00;
    d->config[0x8b] = 0x00;

    /*
     * The GART registers past the capability are guest-writable; PCI
     * config bytes default to read-only, so open them explicitly.
     */
    memset(d->wmask + UNIN_CFG_GART_BASE, 0xff,
           UNIN_CFG_GART_CTRL + 4 - UNIN_CFG_GART_BASE);
}

static void unin_agp_pci_host_config_write(PCIDevice *d, uint32_t addr,
                                           uint32_t val, int len)
{
    UNINHostState *s;

    pci_default_write_config(d, addr, val, len);

    if (!ranges_overlap(addr, len, UNIN_CFG_GART_BASE,
                        UNIN_CFG_GART_CTRL + 4 - UNIN_CFG_GART_BASE)) {
        return;
    }

    s = UNI_NORTH_AGP_HOST_BRIDGE(qdev_get_parent_bus(DEVICE(d))->parent);
    s->gart_base = pci_get_long(d->config + UNIN_CFG_GART_BASE);
    s->agp_base = pci_get_long(d->config + UNIN_CFG_AGP_BASE);
    s->gart_ctrl = pci_get_long(d->config + UNIN_CFG_GART_CTRL);
    trace_unin_agp_gart_cfg(s->gart_base, s->agp_base, s->gart_ctrl);

    /*
     * An INVALIDATE is self-clearing and needs no work here: nothing
     * caches a translation across accesses -- unin_agp_translate() re-reads
     * the table entry every time -- so a flushed GART is already visible.
     */
    d->config[UNIN_CFG_GART_CTRL] &= ~UNIN_GART_CTRL_INVAL;
}

static void u3_agp_pci_host_realize(PCIDevice *d, Error **errp)
{
    d->config[PCI_CACHE_LINE_SIZE] = 0x08;
    d->config[PCI_LATENCY_TIMER] = 0x10;
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

static void unin_agp_pci_host_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = unin_agp_pci_host_realize;
    k->config_write = unin_agp_pci_host_config_write;
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
    /* Route slots per the real PowerMac3,4 interrupt-map (Apple ROM mode) */
    DEFINE_PROP_BOOL("real-irq-map", UNINHostState, real_irq_map, false),
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

static const TypeInfo pci_u3_agp_info = {
    .name          = TYPE_U3_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_u3_agp_init,
    .class_init    = pci_u3_agp_class_init,
};

static const Property pci_unin_agp_props[] = {
    /* Route the slot per the real PowerMac3,4 interrupt-map (Apple ROM) */
    DEFINE_PROP_BOOL("real-irq-map", UNINHostState, real_irq_map, false),
};

static void pci_unin_agp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_agp_realize;
    device_class_set_props(dc, pci_unin_agp_props);
}

static const TypeInfo pci_unin_agp_info = {
    .name          = TYPE_UNI_NORTH_AGP_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(UNINHostState),
    .instance_init = pci_unin_agp_init,
    .class_init    = pci_unin_agp_class_init,
};

static const Property pci_unin_internal_props[] = {
    /* Route slots per the real PowerMac3,4 interrupt-map (Apple ROM mode) */
    DEFINE_PROP_BOOL("real-irq-map", UNINHostState, real_irq_map, false),
    /* Leave device 14 function 0 free for a real onboard device */
    DEFINE_PROP_BOOL("no-self-func", UNINHostState, no_self_func, false),
};

static void pci_unin_internal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = pci_unin_internal_realize;
    device_class_set_props(dc, pci_unin_internal_props);
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
    UNINState *s = UNI_NORTH(opaque);
    unsigned i;

    trace_unin_write(addr, value);

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;

        if (a < UNIN_REGS_SIZE) {
            s->regs[a] = value >> (8 * (size - 1 - i));
        }
    }
}

static uint64_t unin_read(void *opaque, hwaddr addr, unsigned size)
{
    UNINState *s = UNI_NORTH(opaque);
    uint32_t value;
    unsigned i;

    if (addr == 0) {
        value = UNINORTH_VERSION_10A;
    } else {
        /*
         * The rest of this window is state the firmware writes and reads
         * back: HWINIT_STATE at 0x70, and -- the reason this matters -- a
         * block of memory-controller registers around 0x2000 that a real
         * Apple ROM programs per chip select while sizing RAM. Reading zero
         * to all of it leaves the ROM unable to see its own configuration.
         */
        value = 0;
        for (i = 0; i < size; i++) {
            hwaddr a = addr + i;

            value <<= 8;
            if (a < UNIN_REGS_SIZE) {
                value |= s->regs[a];
            }
        }
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

    /*
     * The real uni-n decodes 16MB (its device-tree node reads
     * "reg f8000000 01000000"), not 4KB -- the memory controller registers
     * the firmware uses sit above the first page.
     */
    memory_region_init_io(&s->mem, obj, &unin_ops, obj, "unin", 0x1000000);
    keywest_i2c_init(&s->i2c, DEVICE(obj), "unin-i2c", UNINORTH_I2C_SIZE);

    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_mmio(sbd, &s->i2c.mem);
}

static ResettablePhases unin_parent_phases;

static void unin_reset_hold(Object *obj, ResetType type)
{
    UNINState *s = UNI_NORTH(obj);

    if (unin_parent_phases.hold) {
        unin_parent_phases.hold(obj, type);
    }

    /*
     * s->regs[] is where the firmware's own state lives -- HWINIT_STATE at
     * offset 0x70 among it (see the comment in unin_read()). A guest-
     * triggered restart (PMU/CUDA -> qemu_system_reset_request()) re-runs
     * the ROM's reset-vector code from scratch, but without this, this
     * same device object's regs[] carries over whatever the PREVIOUS boot
     * session's firmware already wrote there. The ROM's own early sanity
     * check on HWINIT_STATE reads that stale "already initialized" value,
     * takes its fatal-error branch, and hangs forever in a `b .` loop --
     * confirmed live, NIP parked at that exact instruction after a
     * guest-initiated restart, on an OS X 10.0 desktop that had booted
     * and run normally beforehand. Real hardware's electrical reset would
     * clear this register on every reset, cold or warm; this array
     * previously had no reset() handler at all, so it never did here.
     */
    memset(s->regs, 0, sizeof(s->regs));
    /* the Keywest cell too: idle, nothing pending (it has no IRQ line) */
    keywest_i2c_reset(&s->i2c);
}

static void unin_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    resettable_class_set_parent_phases(rc, NULL, unin_reset_hold, NULL,
                                       &unin_parent_phases);
}

static const TypeInfo unin_info = {
    .name          = TYPE_UNI_NORTH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UNINState),
    .instance_init = unin_init,
    .class_init    = unin_class_init,
};

static void unin_agp_iommu_memory_region_class_init(ObjectClass *klass,
                                                    const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = unin_agp_translate;
}

static const TypeInfo unin_agp_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_UNIN_AGP_IOMMU_MEMORY_REGION,
    .class_init = unin_agp_iommu_memory_region_class_init,
};

static void unin_register_types(void)
{
    type_register_static(&unin_agp_iommu_memory_region_info);

    type_register_static(&unin_main_pci_host_info);
    type_register_static(&u3_agp_pci_host_info);
    type_register_static(&unin_agp_pci_host_info);
    type_register_static(&unin_internal_pci_host_info);

    type_register_static(&pci_unin_main_info);
    type_register_static(&pci_u3_agp_info);
    type_register_static(&pci_unin_agp_info);
    type_register_static(&pci_unin_internal_info);

    type_register_static(&unin_info);
}

type_init(unin_register_types)
