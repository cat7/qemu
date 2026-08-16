/*
 * PowerMac3,4 built-in FireWire (IEEE 1394) controller stub
 *
 * Real PM34 hardware is a Lucent/Agere FW322/FW323 OHCI 1394 controller
 * (PCI 11c1:5811, real device tree at /pci@f4000000/firewire@e). No OHCI
 * 1394 link-layer/bus functionality is modeled here -- this exists so the
 * PCI device enumerates and publishes the real device-tree node (name,
 * reg, assigned-addresses, compatible) that Apple Hardware Test's Core99
 * FireWire TCM (TCMFirewire) needs to find via finddevice()/getprop()
 * before it can even attempt a register test -- without a device present
 * at this slot at all, AHT's GetBaseAddress() has nothing to find and the
 * Logic Board sequence hard-fails (see mac99 CD-boot investigation
 * memory). The one MMIO BAR is present and traced but returns a fixed,
 * empirically-tuned register image, not real controller behavior.
 *
 * Deliberately reports a fictional vendor:device (0x1234:0x5678) instead
 * of the real 11c1:5811. The real Apple ROM has vendor-specific bring-up
 * for the real Lucent identity that does a raw, unmapped MMIO read
 * (BusID at BAR offset 0x1c) before the CPU's own BAT/MMU setup covers
 * that address range -- a genuine CPU-level translation fault (DSI),
 * gdb-confirmed at ROM address 0x2b12ec, independent of anything this
 * device answers. On real hardware this path is safe only because the
 * real chip carries onboard PCI Expansion ROM/FCode that Open Firmware
 * runs in its own sandboxed interpreter first; modeling that is a much
 * larger feature (see mac99 CD-boot investigation memory). A non-
 * matching vendor:device sidesteps the ROM's vendor-specific bring-up
 * entirely -- confirmed live: no BusID read ever occurs, boot proceeds
 * straight through to AHT's own well-behaved, OF-map-in-mediated
 * TCMFirewire probe, and AHT's menu loads normally (Logic Board's
 * FireWire subtest still reports "no device", the honest outcome for an
 * unmodeled link layer, but nothing hangs or crashes).
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "trace.h"

#define TYPE_MAC99_FIREWIRE_STUB "mac99-firewire-stub"
OBJECT_DECLARE_SIMPLE_TYPE(Mac99FirewireStubState, MAC99_FIREWIRE_STUB)

struct Mac99FirewireStubState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t phy_control;
    uint32_t guid_hi;
    uint32_t guid_lo;
};

/*
 * OHCI 1.1 spec register offsets referenced by the read handler below.
 * Real Lucent FW322/FW323 values are not available to this project (no
 * real-hardware register dump on file); the values here are placeholders
 * tuned against what Apple Hardware Test's TCMFirewire actually reads,
 * per the mac99 CD-boot investigation memory -- adjust here if a probe
 * turns up a different expectation.
 */
#define OHCI_REG_VERSION      0x00
#define OHCI_REG_GUID_ROM     0x04
#define OHCI_REG_BUS_ID       0x1c
#define OHCI_REG_GUID_HIGH    0x24
#define OHCI_REG_GUID_LOW     0x28
#define OHCI_REG_VENDOR_ID    0x40
#define OHCI_REG_PHY_CONTROL  0xec

/*
 * Real FW323 (see FW323061394.PDF datasheet, PCI Configuration Registers /
 * "GUID High Register" and "GUID Low Register" sections): with no serial
 * EEPROM detected (the GUID_ROM version bit clear, as this stub always
 * reports), the chip defines an EEPROM-free path -- a single PCI
 * config-space write to CONFIG_GUID_HIGH/LOW (or the FW323-05 back-compat
 * aliases) latches the GUID, which then becomes visible read-only through
 * the normal memory-mapped OHCI GUID High/Low registers above to any real
 * driver. This is a genuine spec-defined mechanism, not a QEMU invention --
 * and unlike the vendor-specific ROM bring-up (which crashes for this
 * device, see mac99_firewire_stub_realize()), any real class-code-matched
 * OHCI driver reads the GUID this same way regardless of vendor identity.
 */
#define CONFIG_GUID_HIGH       0x70
#define CONFIG_GUID_LOW        0x74
#define CONFIG_GUID_HIGH_COMPAT 0x80
#define CONFIG_GUID_LOW_COMPAT  0x84

/*
 * BusID is a read-only, hardwired constant on real OHCI 1.1 hardware --
 * always the ASCII bytes "1394", identifying the controller as compliant
 * with the IEEE 1394 OHCI spec. Confirmed via gdb as the actual register
 * the Apple ROM's own bring-up code reads (a plain `lwz` at a fixed
 * offset from this BAR's base, not a handshake) immediately after this
 * BAR becomes accessible -- see mac99 CD-boot investigation memory.
 */
#define OHCI_BUS_ID_MAGIC 0x31333934

/*
 * PhyControl's real bit layout, for reference: reverse-engineered from
 * Apple Hardware Test's own TCMFirewire subtest (FUN_10001524/FUN_1000170c
 * in the AHT 1.1 PM34 disc's TCMFirewire.bin, decompiled via Ghidra -- see
 * mac99 CD-boot investigation memory). AHT drives the real OHCI indirect
 * PHY-register access protocol: a read request writes (reg<<8)|0x8000
 * (RDREG), then polls for RDREG to clear and RDDONE (bit 31) to set, with
 * the completed word echoing the register number and carrying the data
 * byte; a write request writes (reg<<8)|0x4000|data (WRREG), then polls
 * for WRREG to clear. This stub deliberately does NOT implement that
 * handshake -- see the write handler below for why.
 */

static uint64_t mac99_firewire_stub_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    uint64_t val = 0;

    Mac99FirewireStubState *s = MAC99_FIREWIRE_STUB(opaque);

    switch (addr) {
    case OHCI_REG_VERSION:
        /*
         * OHCI version 1.0, GUID_ROM bit (24) left clear: claiming a
         * config ROM is present would send real firmware into the 1394
         * Config ROM's own offset-0x04 indirect-read protocol (bus_info
         * block, GUID, vendor/unit directories), a different and more
         * involved handshake than PhyControl's, not modeled here. This
         * was suspected (wrongly, ruled out by gdb) of being the cause
         * of the real crash tracked down below at OHCI_REG_BUS_ID --
         * left cleared anyway since that protocol still isn't modeled.
         */
        val = 0x00010000;
        break;
    case OHCI_REG_BUS_ID:
        val = OHCI_BUS_ID_MAGIC;
        break;
    case OHCI_REG_GUID_HIGH:
        val = s->guid_hi;
        break;
    case OHCI_REG_GUID_LOW:
        val = s->guid_lo;
        break;
    case OHCI_REG_VENDOR_ID:
        /* Real PM34 vendor-id property: 0x000011c1 (Lucent/Agere). */
        val = 0x000011c1;
        break;
    case OHCI_REG_PHY_CONTROL:
        val = s->phy_control;
        break;
    default:
        val = 0;
        break;
    }

    trace_mac99_firewire_stub_read(addr, size, val);
    return val;
}

static void mac99_firewire_stub_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    Mac99FirewireStubState *s = MAC99_FIREWIRE_STUB(opaque);

    trace_mac99_firewire_stub_write(addr, size, value);

    switch (addr) {
    case OHCI_REG_PHY_CONTROL:
        /*
         * Deliberately never report RDDONE/success here. Making this look
         * like a real PHY chip answered (RDREG -> RDDONE with echoed data)
         * was tried and made things worse: it convinces the Apple ROM's
         * own PHY bring-up code a real 1394 PHY is present, which then
         * walks further into real-hardware-only initialization this stub
         * can't follow and crashes the whole boot (DSI at a fixed ROM
         * address, reproduced with and without the BAR pre-map below --
         * see mac99 CD-boot investigation memory). Always reading back 0
         * here, for every command, is what let both the Apple ROM's own
         * early boot-time PHY probe AND AHT's own TCMFirewire RDREG/WRREG
         * polls (FUN_10001524/FUN_1000170c, both bounded-retry loops) time
         * out and move on cleanly -- AHT reports a Logic Board FireWire
         * error, but the whole Quick Test suite completes instead of
         * hanging or crashing, which is what actually matters here.
         */
        s->phy_control = 0;
        break;
    default:
        break;
    }
}

static void mac99_firewire_stub_config_write(PCIDevice *dev, uint32_t addr,
                                             uint32_t val, int len)
{
    Mac99FirewireStubState *s = MAC99_FIREWIRE_STUB(dev);

    pci_default_write_config(dev, addr, val, len);

    /* See CONFIG_GUID_HIGH's comment above: the real EEPROM-free GUID load
     * mechanism. Not expected to actually fire (the ROM never reaches this
     * device's config space, and we preload a synthesized GUID at realize()
     * time already), but honored for spec fidelity in case anything does. */
    if (addr == CONFIG_GUID_HIGH || addr == CONFIG_GUID_HIGH_COMPAT) {
        s->guid_hi = val;
    } else if (addr == CONFIG_GUID_LOW || addr == CONFIG_GUID_LOW_COMPAT) {
        s->guid_lo = val;
    }
}

static const MemoryRegionOps mac99_firewire_stub_ops = {
    .read = mac99_firewire_stub_read,
    .write = mac99_firewire_stub_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void mac99_firewire_stub_realize(PCIDevice *dev, Error **errp)
{
    Mac99FirewireStubState *s = MAC99_FIREWIRE_STUB(dev);

    /* Fictional vendor:device, deliberately not the real 11c1:5811 --
     * see the file-header comment for why. */
    pci_set_word(dev->config + PCI_VENDOR_ID, 0x1234);
    pci_set_word(dev->config + PCI_DEVICE_ID, 0x5678);
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, 0x1234);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, 0x5678);
    pci_config_set_class(dev->config, PCI_CLASS_SERIAL_FIREWIRE);
    dev->config[PCI_CLASS_PROG] = 0x10; /* OHCI programming interface */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    /*
     * Preload a synthesized GUID (Apple OUI 00:30:65 + a per-machine serial
     * borrowed from the gmac's MAC when available, matching the identical
     * bytes core99_nvram_set_bootcmd's fwguid graft publishes as an OF
     * property) so any real driver reading the GUID High/Low registers
     * directly gets a valid, consistent answer from boot, without needing
     * the crash-prone ROM bring-up or a real config-space write to ever
     * happen -- see CONFIG_GUID_HIGH's comment above for why that write
     * can't come from this project's ROM.
     */
    {
        Object *o = object_resolve_path_type("", "sungem", NULL);
        g_autofree char *macstr = o ?
            object_property_get_str(o, "mac", NULL) : NULL;
        uint8_t mac[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };

        if (macstr) {
            sscanf(macstr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
        }
        s->guid_hi = 0x003065ff; /* node_vendor_id=00:30:65, chip_id_hi=ff */
        s->guid_lo = (0xfeu << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &mac99_firewire_stub_ops, s,
                          "mac99-firewire-stub", 0x1000);

    /*
     * Deliberately not pci_register_bar(): this ROM never eagerly assigns
     * this BAR during its own boot-time PCI scan the way it does for GMAC
     * -- confirmed live at the OF prompt (raw reads to the documented
     * assigned-addresses fault until the node is explicitly opened) and
     * confirmed again via gdb: the actual crashing instruction is a
     * firmware-internal `lwz r3,28(r3)` with r3 == this BAR's real
     * address (0xf5000000), reading OHCI's BusID register (offset 0x1c)
     * as part of normal bring-up, well before anything would have opened
     * the node. Pinning the BAR through normal PCI config-space writes
     * wasn't enough either -- the ROM's own device-specific bring-up
     * never writes BAR0 at all for this vendor:device, so a
     * config_write override forcing it back in had nothing to intercept.
     *
     * Map the region directly into the global address space at the real
     * fixed address instead, entirely bypassing PCI BAR negotiation. This
     * matches how this project's own real-hardware research treats other
     * onboard, non-relocatable devices on this same internal bus (see
     * UNINORTH_I2C_BASE's identical direct overlay in mac_newworld.c) and
     * is arguably more accurate anyway: a real onboard chip's address is
     * hardwired by board design, not soft-configured by firmware. AHT's
     * own device-tree lookup already gets this same address from the
     * ROM's statically-published assigned-addresses property regardless
     * of live BAR/config-space state, so nothing needs pci_register_bar's
     * bookkeeping to find it.
     */
    memory_region_add_subregion_overlap(get_system_memory(), 0xf5000000,
                                        &s->mmio, 1);
}

static void mac99_firewire_stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = mac99_firewire_stub_realize;
    k->config_write = mac99_firewire_stub_config_write;
    k->vendor_id = 0x1234;
    k->device_id = 0x5678;
    k->class_id = PCI_CLASS_SERIAL_FIREWIRE;
    dc->desc = "PowerMac3,4 built-in FireWire (stub, no 1394 bus)";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mac99_firewire_stub_type_info = {
    .name = TYPE_MAC99_FIREWIRE_STUB,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Mac99FirewireStubState),
    .class_init = mac99_firewire_stub_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void mac99_firewire_stub_register_types(void)
{
    type_register_static(&mac99_firewire_stub_type_info);
}

type_init(mac99_firewire_stub_register_types)
