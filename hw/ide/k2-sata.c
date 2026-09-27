/*
 * ServerWorks/Broadcom K2 SATA controller, as found on the PowerMac G5
 *
 * BAR5 is 8 KB of MMIO: one 256-byte block per port, the taskfile
 * registers 4 bytes apart, a bus-master IDE block at 0x30, the SATA
 * status/error/control registers at 0x40 and interrupt control at 0x80.
 * Four blocks answer; only the first two have a connector.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/pci/pci.h"
#include "hw/ide/k2-sata.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "ide-internal.h"
#include "trace.h"

#define K2_SATA_PORT_SHIFT      8

#define K2_SATA_TF_DATA         0x00
#define K2_SATA_TF_CMDSTAT      0x1c
#define K2_SATA_TF_CTL          0x20
#define K2_SATA_BMDMA_CMD       0x30
#define K2_SATA_BMDMA_STATUS    0x31    /* also read and written at 0x32 */
#define K2_SATA_BMDMA_PRD       0x34
#define K2_SATA_SSTATUS         0x40
#define K2_SATA_SERROR          0x44
#define K2_SATA_SCONTROL        0x48
#define K2_SATA_SICR1           0x80
#define K2_SATA_SICR2           0x84
#define K2_SATA_SIM             0x88
#define K2_SATA_MDIO            0x8c
#define K2_SATA_MDIO_DONE       0x8000

/* One bit per port: its drive's interrupt line is asserted */
#define K2_SATA_INT_STATUS      0x1f80

#define K2_SATA_SSTATUS_LINK_UP 0x113   /* device present, Gen1, active */
#define K2_SATA_SSTATUS_OFFLINE 0x4     /* no device: the phy stays offline */
#define K2_SATA_SCONTROL_DET    0xf
#define K2_SATA_DET_RESET       1

static bool k2_sata_port_present(K2SATAState *s, int port)
{
    return port < K2_SATA_NUM_PORTS &&
           s->parent_obj.bus[port].ifs[0].blk != NULL;
}

/*
 * Drivers write the LBA48 taskfile as 16-bit values, the previous
 * (HOB) byte in the upper half, and read it back the same way.
 */
static uint32_t k2_sata_tf_read(IDEBus *bus, int reg, unsigned size)
{
    uint8_t cmd = bus->cmd;
    uint32_t val;

    if (size == 1) {
        return ide_ioport_read(bus, reg);
    }
    bus->cmd = cmd & ~IDE_CTRL_HOB;
    val = ide_ioport_read(bus, reg);
    bus->cmd = cmd | IDE_CTRL_HOB;
    val |= ide_ioport_read(bus, reg) << 8;
    bus->cmd = cmd;
    return val;
}

static void k2_sata_tf_write(IDEBus *bus, int reg, uint64_t val,
                             unsigned size)
{
    if (size > 1) {
        ide_ioport_write(bus, reg, (val >> 8) & 0xff);
    }
    ide_ioport_write(bus, reg, val & 0xff);
}

/* A software reset drops the drive's interrupt line */
static void k2_sata_ctl_write(IDEBus *bus, uint8_t val)
{
    if (!(bus->cmd & IDE_CTRL_RESET) && (val & IDE_CTRL_RESET)) {
        qemu_irq_lower(bus->irq);
    }
    ide_ctrl_write(bus, 0, val);
}

static uint64_t k2_sata_bmdma_read(BMDMAState *bm, hwaddr off, unsigned size)
{
    uint64_t val = 0;
    int i;

    for (i = size - 1; i >= 0; i--) {
        uint8_t b;

        switch (off + i) {
        case 0:
            b = bm->cmd;
            break;
        case 1:
        case 2:
            b = bm->status;
            break;
        default:
            b = 0;
        }
        val = (val << 8) | b;
    }
    return val;
}

static void k2_sata_bmdma_write(BMDMAState *bm, hwaddr off, uint64_t val,
                                unsigned size)
{
    int i;

    for (i = 0; i < size; i++, val >>= 8) {
        switch (off + i) {
        case 0:
            bmdma_cmd_writeb(bm, val & 0xff);
            break;
        case 1:
        case 2:
            bmdma_status_writeb(bm, val & 0xff);
            break;
        }
    }
}

static uint64_t k2_sata_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    K2SATAState *s = opaque;
    int port = addr >> K2_SATA_PORT_SHIFT;
    hwaddr off = addr & ((1 << K2_SATA_PORT_SHIFT) - 1);
    IDEBus *bus;
    uint64_t val = 0;

    if (addr == K2_SATA_INT_STATUS) {
        val = s->irq_pending;
        goto out;
    }
    if (port >= K2_SATA_NUM_REGS) {
        goto out;
    }
    bus = port < K2_SATA_NUM_PORTS ? &s->parent_obj.bus[port] : NULL;

    if (off < K2_SATA_SSTATUS && !bus) {
        goto out;
    }
    switch (off) {
    case K2_SATA_TF_DATA:
        if (size == 1) {
            val = ide_ioport_read(bus, 0);
        } else if (size == 2) {
            val = ide_data_readw(bus, 0);
        } else {
            val = ide_data_readl(bus, 0);
        }
        break;
    case 0x04 ... 0x18:
        val = k2_sata_tf_read(bus, off >> 2, size);
        break;
    case K2_SATA_TF_CMDSTAT:
        val = ide_ioport_read(bus, 7);
        break;
    case K2_SATA_TF_CTL:
        val = ide_status_read(bus, 0);
        break;
    case K2_SATA_BMDMA_CMD ... K2_SATA_BMDMA_CMD + 3:
        val = k2_sata_bmdma_read(&s->parent_obj.bmdma[port],
                                 off - K2_SATA_BMDMA_CMD, size);
        break;
    case K2_SATA_BMDMA_PRD ... K2_SATA_BMDMA_PRD + 3:
        val = bmdma_addr_ioport_ops.read(&s->parent_obj.bmdma[port],
                                         off - K2_SATA_BMDMA_PRD, size);
        break;
    case K2_SATA_SSTATUS:
        val = k2_sata_port_present(s, port) ? K2_SATA_SSTATUS_LINK_UP
                                            : K2_SATA_SSTATUS_OFFLINE;
        break;
    case K2_SATA_SERROR:
        val = s->serror[port];
        break;
    case K2_SATA_SCONTROL:
        val = s->scontrol[port];
        break;
    case K2_SATA_SICR1:
        val = s->sicr1[port];
        break;
    case K2_SATA_SICR2:
        val = s->sicr2[port];
        break;
    case K2_SATA_SIM:
        val = s->sim[port];
        break;
    case K2_SATA_MDIO:
        val = s->mdio[port] | K2_SATA_MDIO_DONE;
        break;
    }
out:
    trace_k2_sata_mmio_read(addr, size, val);
    return val;
}

static void k2_sata_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    K2SATAState *s = opaque;
    int port = addr >> K2_SATA_PORT_SHIFT;
    hwaddr off = addr & ((1 << K2_SATA_PORT_SHIFT) - 1);
    IDEBus *bus;

    trace_k2_sata_mmio_write(addr, size, val);
    if (port >= K2_SATA_NUM_REGS) {
        return;
    }
    bus = port < K2_SATA_NUM_PORTS ? &s->parent_obj.bus[port] : NULL;

    if (off < K2_SATA_SSTATUS && !bus) {
        return;
    }
    switch (off) {
    case K2_SATA_TF_DATA:
        if (size == 2) {
            ide_data_writew(bus, 0, val);
        } else if (size == 4) {
            ide_data_writel(bus, 0, val);
        }
        break;
    case 0x04 ... 0x18:
        k2_sata_tf_write(bus, off >> 2, val, size);
        break;
    case K2_SATA_TF_CMDSTAT:
        ide_ioport_write(bus, 7, val & 0xff);
        break;
    case K2_SATA_TF_CTL:
        k2_sata_ctl_write(bus, val & 0xff);
        break;
    case K2_SATA_BMDMA_CMD ... K2_SATA_BMDMA_CMD + 3:
        k2_sata_bmdma_write(&s->parent_obj.bmdma[port],
                            off - K2_SATA_BMDMA_CMD, val, size);
        break;
    case K2_SATA_BMDMA_PRD ... K2_SATA_BMDMA_PRD + 3:
        bmdma_addr_ioport_ops.write(&s->parent_obj.bmdma[port],
                                    off - K2_SATA_BMDMA_PRD, val, size);
        break;
    case K2_SATA_SERROR:
        s->serror[port] &= ~val;
        break;
    case K2_SATA_SCONTROL:
        s->scontrol[port] = val;
        /* COMRESET: the drive resets and runs its diagnostic */
        if (bus && (val & K2_SATA_SCONTROL_DET) == K2_SATA_DET_RESET) {
            k2_sata_ctl_write(bus, bus->cmd | IDE_CTRL_RESET);
        }
        break;
    case K2_SATA_SICR1:
        s->sicr1[port] = val;
        break;
    case K2_SATA_SICR2:
        s->sicr2[port] = val;
        break;
    case K2_SATA_SIM:
        s->sim[port] = val;
        break;
    case K2_SATA_MDIO:
        s->mdio[port] = val;
        break;
    }
}

static const MemoryRegionOps k2_sata_mmio_ops = {
    .read = k2_sata_mmio_read,
    .write = k2_sata_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void k2_sata_set_irq(void *opaque, int port, int level)
{
    K2SATAState *s = opaque;
    IDEBus *bus = &s->parent_obj.bus[port];

    /* The FIS that ends a software reset has its interrupt bit clear */
    if (level && (bus->cmd & IDE_CTRL_RESET)) {
        return;
    }
    s->irq_pending = deposit32(s->irq_pending, port, 1, level != 0);
    trace_k2_sata_irq(port, level, bus->cmd, s->irq_pending);
    pci_set_irq(PCI_DEVICE(s), s->irq_pending != 0);
}

static void k2_sata_reset(DeviceState *dev)
{
    K2SATAState *s = K2_SATA(dev);
    int i;

    for (i = 0; i < K2_SATA_NUM_PORTS; i++) {
        ide_bus_reset(&s->parent_obj.bus[i], IDE_RESET_HARDWARE);
    }
    for (i = 0; i < K2_SATA_NUM_REGS; i++) {
        s->serror[i] = 0;
        s->scontrol[i] = 0;
        s->sicr1[i] = 0;
        s->sicr2[i] = 0;
        s->sim[i] = 0;
        s->mdio[i] = 0;
    }
}

static void k2_sata_realize(PCIDevice *dev, Error **errp)
{
    K2SATAState *s = K2_SATA(dev);
    PCIIDEState *d = PCI_IDE(dev);
    DeviceState *ds = DEVICE(dev);
    int i;

    pci_config_set_prog_interface(dev->config, 0x8f);
    pci_config_set_interrupt_pin(dev->config, 1);

    memory_region_init_io(&s->mmio, OBJECT(s), &k2_sata_mmio_ops, s,
                          "k2-sata", 0x2000);
    pci_register_bar(dev, 5, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    qdev_init_gpio_in(ds, k2_sata_set_irq, K2_SATA_NUM_PORTS);
    for (i = 0; i < K2_SATA_NUM_PORTS; i++) {
        g_autofree char *name = g_strdup_printf("sata.%d", i);

        qbus_init(&d->bus[i], sizeof(d->bus[i]), TYPE_IDE_BUS, ds, name);
        d->bus[i].bus_id = i;
        d->bus[i].max_units = 1;
        ide_bus_init_output_irq(&d->bus[i], qdev_get_gpio_in(ds, i));
        bmdma_init(&d->bus[i], &d->bmdma[i], d);
        ide_bus_register_restart_cb(&d->bus[i]);
    }
}

void k2_sata_init_drives(K2SATAState *s, DriveInfo **hd_table)
{
    int i;

    for (i = 0; i < K2_SATA_NUM_PORTS; i++) {
        if (hd_table[i]) {
            ide_bus_create_drive(&s->parent_obj.bus[i], 0, hd_table[i]);
        }
    }
}

static const VMStateDescription vmstate_k2_sata = {
    .name = "k2-sata",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, K2SATAState, 0, vmstate_ide_pci,
                       PCIIDEState),
        VMSTATE_UINT32_ARRAY(serror, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32_ARRAY(scontrol, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32_ARRAY(sicr1, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32_ARRAY(sicr2, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32_ARRAY(sim, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32_ARRAY(mdio, K2SATAState, K2_SATA_NUM_REGS),
        VMSTATE_UINT32(irq_pending, K2SATAState),
        VMSTATE_END_OF_LIST()
    }
};

static void k2_sata_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);

    k->realize = k2_sata_realize;
    k->vendor_id = PCI_VENDOR_ID_SERVERWORKS;
    k->device_id = PCI_DEVICE_ID_SERVERWORKS_K2_SATA;
    k->subsystem_vendor_id = PCI_VENDOR_ID_SERVERWORKS;
    k->subsystem_id = PCI_DEVICE_ID_SERVERWORKS_K2_SATA;
    k->class_id = PCI_CLASS_STORAGE_IDE;
    device_class_set_legacy_reset(dc, k2_sata_reset);
    dc->vmsd = &vmstate_k2_sata;
    dc->desc = "K2 SATA controller";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo k2_sata_type_info = {
    .name = TYPE_K2_SATA,
    .parent = TYPE_PCI_IDE,
    .instance_size = sizeof(K2SATAState),
    .class_init = k2_sata_class_init,
};

static void k2_sata_register_types(void)
{
    type_register_static(&k2_sata_type_info);
}

type_init(k2_sata_register_types)
