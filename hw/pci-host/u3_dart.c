/*
 * Apple U3 DART: the DMA address relocation table in front of memory
 *
 * While disabled, bus addresses are physical addresses. Enabled, bus page
 * n is looked up in a table of 32-bit big-endian entries in memory:
 * bit 31 valid, bits 23:0 the physical page.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/pci-host/u3_dart.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "trace.h"

#define DART_CNTL               0x00
#define DART_EXCP               0x10

#define DART_CNTL_BASE_SHIFT    12
#define DART_CNTL_BASE_MASK     0xfffff
#define DART_CNTL_FLUSHTLB      0x400
#define DART_CNTL_ENABLE        0x200
#define DART_CNTL_SIZE_MASK     0x1ff

#define DART_ENTRY_VALID        0x80000000
#define DART_ENTRY_RPN_MASK     0x00ffffff

#define DART_PAGE_SHIFT         12
#define DART_PAGE_MASK          ((1ULL << DART_PAGE_SHIFT) - 1)
/* Table size is in pages of entries; 0 means the 512-page maximum */
#define DART_ENTRIES_PER_PAGE   1024
#define DART_MAX_TABLE_PAGES    512

#define TYPE_U3_DART_IOMMU_MEMORY_REGION "u3-dart-iommu-memory-region"

static IOMMUTLBEntry u3_dart_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                       IOMMUAccessFlags flag, int iommu_idx)
{
    U3DARTState *s = container_of(iommu, U3DARTState, iommu);
    IOMMUTLBEntry ret = {
        .target_as = &address_space_memory,
        .iova = addr & ~DART_PAGE_MASK,
        .translated_addr = addr & ~DART_PAGE_MASK,
        .addr_mask = DART_PAGE_MASK,
        .perm = IOMMU_RW,
    };
    uint64_t page = addr >> DART_PAGE_SHIFT;
    uint64_t pages, base;
    uint32_t entry;

    if (!(s->cntl & DART_CNTL_ENABLE)) {
        return ret;
    }

    pages = s->cntl & DART_CNTL_SIZE_MASK;
    if (!pages) {
        pages = DART_MAX_TABLE_PAGES;
    }
    base = (uint64_t)((s->cntl >> DART_CNTL_BASE_SHIFT) &
                      DART_CNTL_BASE_MASK) << DART_PAGE_SHIFT;

    if (page >= pages * DART_ENTRIES_PER_PAGE) {
        entry = 0;
    } else {
        entry = ldl_be_phys(&address_space_memory, base + page * 4);
    }
    if (!(entry & DART_ENTRY_VALID)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "u3-dart: no mapping for bus address 0x%" HWADDR_PRIx
                      "\n", addr);
        trace_u3_dart_fault(addr, entry);
        s->excp = page;
        ret.perm = IOMMU_NONE;
        return ret;
    }
    ret.translated_addr = (uint64_t)(entry & DART_ENTRY_RPN_MASK)
                          << DART_PAGE_SHIFT;
    return ret;
}

static uint64_t u3_dart_read(void *opaque, hwaddr addr, unsigned size)
{
    U3DARTState *s = opaque;
    uint32_t val;

    switch (addr) {
    case DART_CNTL:
        val = s->cntl;
        break;
    case DART_EXCP:
        val = s->excp;
        break;
    default:
        val = 0;
        break;
    }
    trace_u3_dart_read(addr, val);
    return val;
}

static void u3_dart_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    U3DARTState *s = opaque;

    trace_u3_dart_write(addr, val);
    switch (addr) {
    case DART_CNTL:
        /* Translations are not cached, so a TLB flush completes at once */
        s->cntl = val & ~DART_CNTL_FLUSHTLB;
        break;
    case DART_EXCP:
        s->excp = 0;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps u3_dart_ops = {
    .read = u3_dart_read,
    .write = u3_dart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static AddressSpace *u3_dart_get_address_space(PCIBus *bus, void *opaque,
                                               int devfn)
{
    U3DARTState *s = opaque;

    return &s->as;
}

static const PCIIOMMUOps u3_dart_iommu_ops = {
    .get_address_space = u3_dart_get_address_space,
};

void u3_dart_attach(U3DARTState *s, PCIBus *bus)
{
    pci_setup_iommu(bus, &u3_dart_iommu_ops, s);
}

static void u3_dart_reset(DeviceState *dev)
{
    U3DARTState *s = U3_DART(dev);

    s->cntl = 0;
    s->excp = 0;
}

static void u3_dart_init(Object *obj)
{
    U3DARTState *s = U3_DART(obj);

    memory_region_init_io(&s->mem, obj, &u3_dart_ops, s, "u3-dart",
                          U3_DART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
    memory_region_init_iommu(&s->iommu, sizeof(s->iommu),
                             TYPE_U3_DART_IOMMU_MEMORY_REGION, obj,
                             "u3-dart-iommu", UINT64_MAX);
    address_space_init(&s->as, MEMORY_REGION(&s->iommu), "u3-dart");
}

static const VMStateDescription vmstate_u3_dart = {
    .name = TYPE_U3_DART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cntl, U3DARTState),
        VMSTATE_UINT32(excp, U3DARTState),
        VMSTATE_END_OF_LIST()
    }
};

static void u3_dart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, u3_dart_reset);
    dc->vmsd = &vmstate_u3_dart;
}

static void u3_dart_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = u3_dart_translate;
}

static const TypeInfo u3_dart_types[] = {
    {
        .name          = TYPE_U3_DART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(U3DARTState),
        .instance_init = u3_dart_init,
        .class_init    = u3_dart_class_init,
    }, {
        .name          = TYPE_U3_DART_IOMMU_MEMORY_REGION,
        .parent        = TYPE_IOMMU_MEMORY_REGION,
        .class_init    = u3_dart_iommu_class_init,
    },
};

DEFINE_TYPES(u3_dart_types)
