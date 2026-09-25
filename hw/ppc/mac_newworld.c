/*
 * QEMU PowerPC CHRP (currently NewWorld PowerMac) hardware System Emulator
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
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
 *
 * PCI bus layout on a real G5 (U3 based):
 *
 * 0000:f0:0b.0 Host bridge [0600]: Apple Computer Inc. U3 AGP [106b:004b]
 * 0000:f0:10.0 VGA compatible controller [0300]: ATI Technologies Inc RV350 AP [Radeon 9600] [1002:4150]
 * 0001:00:00.0 Host bridge [0600]: Apple Computer Inc. CPC945 HT Bridge [106b:004a]
 * 0001:00:01.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:02.0 PCI bridge [0604]: Advanced Micro Devices [AMD] AMD-8131 PCI-X Bridge [1022:7450] (rev 12)
 * 0001:00:03.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0045]
 * 0001:00:04.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0046]
 * 0001:00:05.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0047]
 * 0001:00:06.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0048]
 * 0001:00:07.0 PCI bridge [0604]: Apple Computer Inc. K2 HT-PCI Bridge [106b:0049]
 * 0001:01:07.0 Class [ff00]: Apple Computer Inc. K2 KeyLargo Mac/IO [106b:0041] (rev 20)
 * 0001:01:08.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:01:09.0 USB Controller [0c03]: Apple Computer Inc. K2 KeyLargo USB [106b:0040]
 * 0001:02:0b.0 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.1 USB Controller [0c03]: NEC Corporation USB [1033:0035] (rev 43)
 * 0001:02:0b.2 USB Controller [0c03]: NEC Corporation USB 2.0 [1033:00e0] (rev 04)
 * 0001:03:0d.0 Class [ff00]: Apple Computer Inc. K2 ATA/100 [106b:0043]
 * 0001:03:0e.0 FireWire (IEEE 1394) [0c00]: Apple Computer Inc. K2 FireWire [106b:0042]
 * 0001:04:0f.0 Ethernet controller [0200]: Apple Computer Inc. K2 GMAC (Sun GEM) [106b:004c]
 * 0001:05:0c.0 IDE interface [0101]: Broadcom K2 SATA [1166:0240]
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/target_page.h"
#include "hw/ppc/ppc.h"
#include "hw/core/qdev-properties.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/core/boards.h"
#include "hw/pci-host/uninorth.h"
#include "hw/pci-host/u3_dart.h"
#include "hw/i2c/i2c.h"
#include "hw/input/adb.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/pci/pci.h"
#include "net/net.h"
#include "system/system.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/ppc/openpic.h"
#include "hw/core/loader.h"
#include "hw/core/fw-path-provider.h"
#include "elf.h"
#include "qemu/error-report.h"
#include "system/kvm.h"
#include "system/reset.h"
#include "kvm_ppc.h"
#include "hw/usb/usb.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "trace.h"

#define MAX_IDE_BUS 2
#define CFG_ADDR 0xf0000510
#define TBFREQ (25UL * 1000UL * 1000UL)
#define CLOCKFREQ (900UL * 1000UL * 1000UL)
#define BUSFREQ (100UL * 1000UL * 1000UL)

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xfff00000
#define PROM_SIZE (1 * MiB)

#define KERNEL_LOAD_ADDR 0x01000000
#define KERNEL_GAP       0x00100000

#define TYPE_CORE99_MACHINE MACHINE_TYPE_NAME("mac99")
typedef struct Core99MachineState Core99MachineState;
DECLARE_INSTANCE_CHECKER(Core99MachineState, CORE99_MACHINE,
                         TYPE_CORE99_MACHINE)

typedef enum {
    CORE99_VIA_CONFIG_CUDA = 0,
    CORE99_VIA_CONFIG_PMU,
    CORE99_VIA_CONFIG_PMU_ADB
} Core99ViaConfig;

struct Core99MachineState {
    /*< private >*/
    MachineState parent;

    Core99ViaConfig via_config;
};

static void fw_cfg_boot_set(void *opaque, const char *boot_device,
                            Error **errp)
{
    fw_cfg_modify_i16(opaque, FW_CFG_BOOT_DEVICE, boot_device[0]);
}

static uint64_t translate_kernel_address(void *opaque, uint64_t addr)
{
    return (addr & 0x0fffffff) + KERNEL_LOAD_ADDR;
}

static void ppc_core99_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    /* 970 CPUs want to get their initial IP as part of their boot protocol */
    cpu->env.nip = PROM_BASE + 0x100;

    /* Secondary CPUs wait for their GPIO soft-reset release */
    if (cs->cpu_index > 0) {
        cs->halted = 1;
    }
}

/* KeyLargo GPIO soft-reset line of a secondary CPU; level 0 = released */
static void cpu_kick(void *opaque, int n, int level)
{
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUState *first_cs = first_cpu;

    if (level || !cs->halted) {
        return;
    }

    /* One timebase is shared by all CPUs */
    if (first_cs && first_cs != cs) {
        PowerPCCPU *first = POWERPC_CPU(first_cs);

        cpu->env.tb_env->tb_offset = first->env.tb_env->tb_offset;
        cpu->env.tb_env->atb_offset = first->env.tb_env->atb_offset;
    }

    /* System reset: the guest has placed its entry at 0x100 */
    cpu->env.excp_prefix = 0;
    cpu->env.nip = 0x100;
    cpu->env.msr = 0;

    cs->halted = 0;
    cs->exception_index = -1;
    qemu_cpu_kick(cs);
}

/* PowerPC Mac99 hardware initialisation */
static void ppc_core99_init(MachineState *machine)
{
    Core99MachineState *core99_machine = CORE99_MACHINE(machine);
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    PowerPCCPU **cpus;
    CPUPPCState *env = NULL;
    char *filename;
    IrqLines *openpic_irqs;
    qemu_irq cpu_kick_irq;
    int i, j, k, ppc_boot_device, machine_arch, bios_size = -1;
    const char *bios_name = machine->firmware ?: PROM_FILENAME;
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    hwaddr kernel_base = 0, initrd_base = 0, cmdline_base = 0;
    long kernel_size = 0, initrd_size = 0;
    PCIBus *pci_bus;
    bool has_pmu, has_adb;
    Object *macio;
    MACIOIDEState *macio_ide;
    BusState *adb_bus;
    MacIONVRAMState *nvr;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    void *fw_cfg;
    SysBusDevice *s;
    DeviceState *dev, *pic_dev, *uninorth_pci_dev;
    DeviceState *uninorth_internal_dev = NULL, *uninorth_agp_dev = NULL;
    DeviceState *ht_dev = NULL;
    uint64_t low_ram_size;
    DeviceState *dart;
    SysBusDevice *unin_dev;
    PCIBus *macio_bus;
    PCIDevice *usb0, *usb1;
    int macio_devfn;
    hwaddr nvram_addr = 0xFFF04000;
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TBFREQ;

    /* init CPUs */
    cpus = g_new0(PowerPCCPU *, machine->smp.cpus);
    for (i = 0; i < machine->smp.cpus; i++) {
        cpus[i] = POWERPC_CPU(cpu_create(machine->cpu_type));

        /* Set time-base frequency to 100 Mhz */
        cpu_ppc_tb_init(&cpus[i]->env, TBFREQ);

        qemu_register_reset(ppc_core99_reset, cpus[i]);

        if (i > 0) {
            cpus[i]->env.tb_env->tb_offset = cpus[0]->env.tb_env->tb_offset;
            cpus[i]->env.tb_env->atb_offset = cpus[0]->env.tb_env->atb_offset;
            CPU(cpus[i])->halted = 1;
        }
    }
    env = &cpus[0]->env;

    /* allocate RAM; on U3, RAM beyond 2 GiB continues at 4 GiB */
    low_ram_size = machine->ram_size;
    if (machine->ram_size > 2 * GiB) {
        MemoryRegion *low, *high;

        if (PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
            error_report("RAM size more than 2 GiB is not supported");
            exit(1);
        }
        low_ram_size = 2 * GiB;
        low = g_new(MemoryRegion, 1);
        high = g_new(MemoryRegion, 1);
        memory_region_init_alias(low, NULL, "ram-low", machine->ram, 0,
                                 low_ram_size);
        memory_region_init_alias(high, NULL, "ram-high", machine->ram,
                                 low_ram_size,
                                 machine->ram_size - low_ram_size);
        memory_region_add_subregion(get_system_memory(), 0, low);
        memory_region_add_subregion(get_system_memory(), 4 * GiB, high);
    } else {
        memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    }

    /* allocate and load firmware ROM */
    memory_region_init_rom(bios, NULL, "ppc_core99.bios", PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, bios);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        /* Load OpenBIOS (ELF) */
        bios_size = load_elf(filename, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL,
                             ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);

        if (bios_size <= 0) {
            /* or load binary ROM image */
            bios_size = load_image_targphys(filename, PROM_BASE, PROM_SIZE,
                                            &error_fatal);
        }
        g_free(filename);
    }
    if (bios_size < 0 || bios_size > PROM_SIZE) {
        error_report("could not load PowerPC bios '%s'", bios_name);
        exit(1);
    }

    if (machine->kernel_filename) {
        kernel_base = KERNEL_LOAD_ADDR;
        kernel_size = load_elf(machine->kernel_filename, NULL,
                               translate_kernel_address, NULL, NULL, NULL,
                               NULL, NULL, ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);
        if (kernel_size < 0) {
            kernel_size = load_aout(machine->kernel_filename, kernel_base,
                                    low_ram_size - kernel_base,
                                    true, TARGET_PAGE_SIZE);
        }
        if (kernel_size < 0) {
            kernel_size = load_image_targphys(machine->kernel_filename,
                                              kernel_base,
                                              low_ram_size - kernel_base,
                                              &error_fatal);
        }
        /* load initrd */
        if (machine->initrd_filename) {
            initrd_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
            initrd_size = load_image_targphys(machine->initrd_filename,
                                              initrd_base,
                                              low_ram_size - initrd_base,
                                              &error_fatal);
            cmdline_base = TARGET_PAGE_ALIGN(initrd_base + initrd_size);
        } else {
            cmdline_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
        }
        ppc_boot_device = 'm';
    } else {
        ppc_boot_device = '\0';
        /* We consider that NewWorld PowerMac never have any floppy drive
         * For now, OHW cannot boot from the network.
         */
        for (i = 0; machine->boot_config.order[i] != '\0'; i++) {
            if (machine->boot_config.order[i] >= 'c' &&
                machine->boot_config.order[i] <= 'f') {
                ppc_boot_device = machine->boot_config.order[i];
                break;
            }
        }
        if (ppc_boot_device == '\0') {
            error_report("No valid boot device for Mac99 machine");
            exit(1);
        }
    }

    openpic_irqs = g_new0(IrqLines, machine->smp.cpus);
    for (i = 0; i < machine->smp.cpus; i++) {
        dev = DEVICE(cpus[i]);
        /* Mac99 IRQ connection between OpenPIC outputs pins
         * and PowerPC input pins
         */
        switch (PPC_INPUT(env)) {
        case PPC_FLAGS_INPUT_6xx:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                 qdev_get_gpio_in(dev, PPC6xx_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC6xx_INPUT_HRESET);
            break;
#if defined(TARGET_PPC64)
        case PPC_FLAGS_INPUT_970:
            openpic_irqs[i].irq[OPENPIC_OUTPUT_INT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_CINT] =
                qdev_get_gpio_in(dev, PPC970_INPUT_INT);
            openpic_irqs[i].irq[OPENPIC_OUTPUT_MCK] =
                qdev_get_gpio_in(dev, PPC970_INPUT_MCP);
            /* Not connected ? */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_DEBUG] = NULL;
            /* Check this */
            openpic_irqs[i].irq[OPENPIC_OUTPUT_RESET] =
                qdev_get_gpio_in(dev, PPC970_INPUT_HRESET);
            break;
#endif /* defined(TARGET_PPC64) */
        default:
            error_report("Bus model not supported on mac99 machine");
            exit(1);
        }
    }

    /* UniN init */
    s = SYS_BUS_DEVICE(qdev_new(TYPE_UNI_NORTH));
    if (PPC_INPUT(env) == PPC_FLAGS_INPUT_970) {
        qdev_prop_set_uint32(DEVICE(s), "version", U3_VERSION_23);
    }
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xf8000000,
                                sysbus_mmio_get_region(s, 0));
    unin_dev = s;

    if (PPC_INPUT(env) == PPC_FLAGS_INPUT_970) {
        machine_arch = ARCH_MAC99_U3;
        /* 970 gets a U3 bus */
        /* Uninorth AGP bus */
        uninorth_pci_dev = qdev_new(TYPE_U3_AGP_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);
        /* PCI hole */
        memory_region_add_subregion(get_system_memory(), 0x90000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf0000000,
                                    sysbus_mmio_get_region(s, 3));

        /* U3 I2C, with the Pulsar clock chip at 0xd2 */
        sysbus_mmio_map(SYS_BUS_DEVICE(unin_dev), 1, 0xf8001000);
        i2c_slave_create_simple(UNI_NORTH(unin_dev)->i2c.bus,
                                TYPE_PULSAR_CLOCK, 0xd2 >> 1);

        /* HyperTransport */
        ht_dev = qdev_new(TYPE_U3_HT_HOST_BRIDGE);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(ht_dev), &error_fatal);
        u3_ht_map(SYS_BUS_DEVICE(ht_dev));

        /* DMA from both host buses goes through the DART */
        dart = qdev_new(TYPE_U3_DART);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dart), 0, U3_DART_BASE);
        u3_dart_attach(U3_DART(dart),
                       PCI_HOST_BRIDGE(uninorth_pci_dev)->bus);
        u3_agp_set_dma_as(U3_AGP_HOST_BRIDGE(uninorth_pci_dev),
                          &U3_DART(dart)->as);
        u3_dart_attach(U3_DART(dart), PCI_HOST_BRIDGE(ht_dev)->bus);
    } else {
        machine_arch = ARCH_MAC99;
        /* Use values found on a real PowerMac */
        /* Uninorth AGP bus */
        uninorth_agp_dev = qdev_new(TYPE_UNI_NORTH_AGP_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_agp_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf0800000);
        sysbus_mmio_map(s, 1, 0xf0c00000);

        /* Uninorth internal bus */
        uninorth_internal_dev = qdev_new(
                                TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE);
        s = SYS_BUS_DEVICE(uninorth_internal_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf4800000);
        sysbus_mmio_map(s, 1, 0xf4c00000);

        /* Uninorth main bus - this must be last to make it the default */
        uninorth_pci_dev = qdev_new(TYPE_UNI_NORTH_PCI_HOST_BRIDGE);
        qdev_prop_set_uint32(uninorth_pci_dev, "ofw-addr", 0xf2000000);
        s = SYS_BUS_DEVICE(uninorth_pci_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf2800000);
        sysbus_mmio_map(s, 1, 0xf2c00000);
        /* PCI hole */
        memory_region_add_subregion(get_system_memory(), 0x80000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf2000000,
                                    sysbus_mmio_get_region(s, 3));
    }

    machine->usb |= defaults_enabled() && !machine->usb_disabled;
    has_pmu = (core99_machine->via_config != CORE99_VIA_CONFIG_CUDA);
    has_adb = (core99_machine->via_config == CORE99_VIA_CONFIG_CUDA ||
               core99_machine->via_config == CORE99_VIA_CONFIG_PMU_ADB);

    /* Secondary CPUs are released through the KeyLargo GPIOs, PMU only */
    if (machine->smp.cpus > 1 && !has_pmu) {
        error_report("mac99: -smp %u needs via=pmu or via=pmu-adb",
                     machine->smp.cpus);
        exit(1);
    }
    /* The K2 has soft-reset lines for two CPUs */
    if (machine_arch == ARCH_MAC99_U3 && machine->smp.cpus > 2) {
        error_report("mac99: a 970 machine takes at most 2 CPUs");
        exit(1);
    }

    /* init basic PC hardware */
    pci_bus = PCI_HOST_BRIDGE(uninorth_pci_dev)->bus;

    /* MacIO */
    /* The K2 is behind the first HT-PCI bridge, the KeyLargo on the PCI bus */
    if (machine_arch == ARCH_MAC99_U3) {
        macio_bus = pci_bridge_get_sec_bus(U3_HT_HOST_BRIDGE(ht_dev)->k2[0]);
        macio_devfn = PCI_DEVFN(7, 0);
    } else {
        macio_bus = pci_bus;
        macio_devfn = -1;
    }
    macio = OBJECT(pci_new(macio_devfn, TYPE_NEWWORLD_MACIO));
    dev = DEVICE(macio);
    qdev_prop_set_uint64(dev, "frequency", tbfreq);
    qdev_prop_set_bit(dev, "has-pmu", has_pmu);
    qdev_prop_set_bit(dev, "has-adb", has_adb);
    qdev_prop_set_bit(dev, "k2", machine_arch == ARCH_MAC99_U3);

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    qdev_prop_set_uint32(pic_dev, "nb_cpus", machine->smp.cpus);
    dev = DEVICE(object_resolve_path_component(macio, "gpio"));
    qdev_prop_set_uint32(dev, "nb-cpus", machine->smp.cpus);
    qdev_prop_set_bit(dev, "k2", machine_arch == ARCH_MAC99_U3);

    pci_realize_and_unref(PCI_DEVICE(macio), macio_bus, &error_fatal);

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    for (i = 0; i < 4; i++) {
        qdev_connect_gpio_out(uninorth_pci_dev, i,
                              qdev_get_gpio_in(pic_dev, 0x1b + i));
    }
    if (machine_arch == ARCH_MAC99_U3) {
        qdev_connect_gpio_out(uninorth_pci_dev, U3_AGP_SLOT_IRQ_LINE,
                              qdev_get_gpio_in(pic_dev, U3_AGP_SLOT_IRQ));
        for (i = 0; i < U3_HT_NUM_IRQS; i++) {
            qdev_connect_gpio_out(ht_dev, i, qdev_get_gpio_in(pic_dev, i));
        }

        /*
         * U3 MPIC, cascaded into the K2 MPIC. Its window also spans the
         * HT self registers, which take precedence.
         */
        dev = qdev_new(TYPE_OPENPIC);
        qdev_prop_set_uint32(dev, "model", OPENPIC_MODEL_KEYLARGO);
        qdev_prop_set_bit(dev, "big-endian", true);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, U3_MPIC_BASE, -1);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), OPENPIC_OUTPUT_INT,
                           qdev_get_gpio_in(pic_dev, U3_MPIC_CASCADE_IRQ));
    }

    /* TODO: additional PCI buses only wired up for 32-bit machines */
    if (PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
        /* Uninorth AGP bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_agp_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }

        /* Uninorth internal bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_internal_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }
    }

    /* OpenPIC */
    s = SYS_BUS_DEVICE(pic_dev);
    k = 0;
    for (i = 0; i < machine->smp.cpus; i++) {
        for (j = 0; j < OPENPIC_OUTPUT_NB; j++) {
            sysbus_connect_irq(s, k++, openpic_irqs[i].irq[j]);
        }
    }
    g_free(openpic_irqs);

    /* CPU1-3 soft-reset lines */
    dev = DEVICE(object_resolve_path_component(macio, "gpio"));
    for (i = 1; i < machine->smp.cpus; i++) {
        cpu_kick_irq = qemu_allocate_irq(cpu_kick, cpus[i], 0);
        qdev_connect_gpio_out_named(dev, "cpu-reset", i, cpu_kick_irq);
    }
    g_free(cpus);

    ide_drive_get(hd, ARRAY_SIZE(hd));

    if (machine_arch == ARCH_MAC99_U3) {
        /*
         * The K2 ATA-100, behind the third HT-PCI bridge, takes the first
         * two IDE drives as master and slave, as a G5's optical drive
         * shares it with a disk.
         */
        DriveInfo *uata_hd[MAX_IDE_DEVS] = {};
        PCIDevice *uata;

        for (i = 0, j = 0; i < ARRAY_SIZE(hd); i++) {
            if (!hd[i]) {
                continue;
            }
            if (j == MAX_IDE_DEVS) {
                error_report("mac99: the K2 ATA-100 takes two IDE drives");
                exit(1);
            }
            uata_hd[j++] = hd[i];
        }
        uata = pci_new(PCI_DEVFN(13, 0), TYPE_K2_UATA);
        pci_realize_and_unref(uata, pci_bridge_get_sec_bus(
                              U3_HT_HOST_BRIDGE(ht_dev)->k2[2]),
                              &error_fatal);
        qdev_connect_gpio_out_named(DEVICE(uata), "dma", 0,
                                    qdev_get_gpio_in(pic_dev,
                                                     K2_UATA_DMA_IRQ));
        k2_uata_init_drives(K2_UATA(uata), uata_hd);
    } else {
        /* We only emulate 2 out of 3 IDE controllers for now */
        macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[0]"));
        macio_ide_init_drives(macio_ide, hd);

        macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[1]"));
        macio_ide_init_drives(macio_ide, &hd[MAX_IDE_DEVS]);
    }

    if (has_adb) {
        if (has_pmu) {
            dev = DEVICE(object_resolve_path_component(macio, "pmu"));
        } else {
            dev = DEVICE(object_resolve_path_component(macio, "cuda"));
        }

        adb_bus = qdev_get_child_bus(dev, "adb.0");
        dev = qdev_new(TYPE_ADB_KEYBOARD);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);

        dev = qdev_new(TYPE_ADB_MOUSE);
        qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    }

    if (machine->usb) {
        if (machine_arch == ARCH_MAC99_U3) {
            usb0 = pci_create_simple(macio_bus, PCI_DEVFN(8, 0), "pci-ohci");
            pci_config_set_device_id(usb0->config, PCI_DEVICE_ID_APPLE_K2_USB);
        } else {
            pci_create_simple(pci_bus, -1, "pci-ohci");
        }

        /* U3 needs to use USB for input because Linux doesn't support via-cuda
        on PPC64 */
        if (!has_adb || machine_arch == ARCH_MAC99_U3) {
            USBBus *usb_bus;

            usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                              &error_abort));
            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
        }

        /* The K2 has a second USB controller */
        if (machine_arch == ARCH_MAC99_U3) {
            usb1 = pci_create_simple(macio_bus, PCI_DEVFN(9, 0), "pci-ohci");
            pci_config_set_device_id(usb1->config, PCI_DEVICE_ID_APPLE_K2_USB);
        }
    }

    pci_vga_init(pci_bus);

    if (!graphic_width) {
        graphic_width = 800;
    }
    if (!graphic_height) {
        graphic_height = 600;
    }
    if (!graphic_depth) {
        graphic_depth = 32;
    }
    if (graphic_depth != 15 && graphic_depth != 32 && graphic_depth != 8) {
        graphic_depth = 15;
    }

    if (machine_arch == ARCH_MAC99_U3) {
        /* The K2 GMAC is behind the fourth HT-PCI bridge */
        PCIDevice *gmac = pci_new(PCI_DEVFN(15, 0), mc->default_nic);

        /* The K2 GMAC's PHY is at MII address 1 */
        qdev_prop_set_uint32(DEVICE(gmac), "phy_addr", 1);
        if (qemu_configure_nic_device(DEVICE(gmac), true, NULL)) {
            pci_realize_and_unref(gmac, pci_bridge_get_sec_bus(
                                  U3_HT_HOST_BRIDGE(ht_dev)->k2[3]),
                                  &error_fatal);
            pci_config_set_device_id(gmac->config,
                                     PCI_DEVICE_ID_APPLE_K2_GMAC);
        } else {
            object_unref(OBJECT(gmac));
        }
    }
    pci_init_nic_devices(pci_bus, mc->default_nic);

    /* The NewWorld NVRAM is not located in the MacIO device */
    if (kvm_enabled() && qemu_real_host_page_size() > 4096) {
        /* We can't combine read-write and read-only in a single page, so
           move the NVRAM out of ROM again for KVM */
        nvram_addr = 0xFFE00000;
    }
    dev = qdev_new(TYPE_MACIO_NVRAM);
    if (machine_arch == ARCH_MAC99_U3) {
        /* The G5's NVRAM is flat, two 8 KB flash banks */
        qdev_prop_set_uint32(dev, "size", MACIO_NVRAM_FLASH_SIZE);
        qdev_prop_set_uint32(dev, "it_shift", 0);
        qdev_prop_set_bit(dev, "flash", true);
    } else {
        qdev_prop_set_uint32(dev, "size", MACIO_NVRAM_SIZE);
        qdev_prop_set_uint32(dev, "it_shift", 1);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, nvram_addr);
    nvr = MACIO_NVRAM(dev);
    if (machine_arch == ARCH_MAC99_U3) {
        pmac_format_nvram_core99(nvr);
    } else {
        pmac_format_nvram_partition(nvr, MACIO_NVRAM_SIZE);
    }
    /* No PCI init: the BIOS will do it */

    dev = qdev_new(TYPE_FW_CFG_MEM);
    fw_cfg = FW_CFG(dev);
    qdev_prop_set_uint32(dev, "data_width", 1);
    qdev_prop_set_bit(dev, "dma_enabled", false);
    object_property_add_child(OBJECT(machine), TYPE_FW_CFG, OBJECT(fw_cfg));
    s = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(s, &error_fatal);
    sysbus_mmio_map(s, 0, CFG_ADDR);
    sysbus_mmio_map(s, 1, CFG_ADDR + 2);

    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, (uint16_t)machine->smp.cpus);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)machine->smp.max_cpus);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, low_ram_size);
    fw_cfg_add_i64(fw_cfg, FW_CFG_PPC_HIGH_RAM_SIZE,
                   machine->ram_size - low_ram_size);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MACHINE_ID, machine_arch);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, kernel_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    if (machine->kernel_cmdline) {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, cmdline_base);
        pstrcpy_targphys("cmdline", cmdline_base, TARGET_PAGE_SIZE,
                         machine->kernel_cmdline);
    } else {
        fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_CMDLINE, 0);
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_base);
    fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
    fw_cfg_add_i16(fw_cfg, FW_CFG_BOOT_DEVICE, ppc_boot_device);

    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_WIDTH, graphic_width);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_HEIGHT, graphic_height);
    fw_cfg_add_i16(fw_cfg, FW_CFG_PPC_DEPTH, graphic_depth);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_VIACONFIG, core99_machine->via_config);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_NVRAM_FLAT,
                   machine_arch == ARCH_MAC99_U3);

    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_IS_KVM, kvm_enabled());
    if (kvm_enabled()) {
        uint8_t *hypercall;

        hypercall = g_malloc(16);
        kvmppc_get_hypercall(env, hypercall, 16);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_PPC_KVM_HC, hypercall, 16);
        fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_KVM_PID, getpid());
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_TBFREQ, tbfreq);
    /* Mac OS X requires a "known good" clock-frequency value; pass it one. */
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_CLOCKFREQ, CLOCKFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_BUSFREQ, BUSFREQ);
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_NVRAM_ADDR, nvram_addr);

    /* MacOS NDRV VGA driver */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, NDRV_VGA_FILENAME);
    if (filename) {
        gchar *ndrv_file;
        gsize ndrv_size;

        if (g_file_get_contents(filename, &ndrv_file, &ndrv_size, NULL)) {
            fw_cfg_add_file(fw_cfg, "ndrv/qemu_vga.ndrv", ndrv_file, ndrv_size);
        }
        g_free(filename);
    }

    qemu_register_boot_set(fw_cfg_boot_set, fw_cfg);
}

/*
 * Implementation of an interface to adjust firmware path
 * for the bootindex property handling.
 */
static char *core99_fw_dev_path(FWPathProvider *p, BusState *bus,
                                DeviceState *dev)
{
    PCIDevice *pci;
    MACIOIDEState *macio_ide;

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-newworld")) {
        pci = PCI_DEVICE(dev);
        return g_strdup_printf("mac-io@%x", PCI_SLOT(pci->devfn));
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-ide")) {
        macio_ide = MACIO_IDE(dev);
        return g_strdup_printf("ata-3@%x", macio_ide->addr);
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-hd")) {
        return g_strdup("disk");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "ide-cd")) {
        return g_strdup("cdrom");
    }

    if (!strcmp(object_get_typename(OBJECT(dev)), "virtio-blk-device")) {
        return g_strdup("disk");
    }

    return NULL;
}
static int core99_kvm_type(MachineState *machine, const char *arg)
{
    /* Always force PR KVM */
    return 2;
}

static void core99_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);

    mc->desc = "Mac99 based PowerMac";
    mc->init = ppc_core99_init;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = KEYLARGO_MAX_CPU;
    mc->default_boot_order = "cd";
    mc->default_display = "std";
    mc->default_nic = "sungem";
    mc->kvm_type = core99_kvm_type;
#ifdef TARGET_PPC64
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("970fx_v3.1");
#else
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("7400_v2.9");
#endif
    mc->default_ram_id = "ppc_core99.ram";
    mc->ignore_boot_device_suffixes = true;
    fwc->get_dev_path = core99_fw_dev_path;
}

static char *core99_get_via_config(Object *obj, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    switch (cms->via_config) {
    default:
    case CORE99_VIA_CONFIG_CUDA:
        return g_strdup("cuda");

    case CORE99_VIA_CONFIG_PMU:
        return g_strdup("pmu");

    case CORE99_VIA_CONFIG_PMU_ADB:
        return g_strdup("pmu-adb");
    }
}

static void core99_set_via_config(Object *obj, const char *value, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    if (!strcmp(value, "cuda")) {
        cms->via_config = CORE99_VIA_CONFIG_CUDA;
    } else if (!strcmp(value, "pmu")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU;
    } else if (!strcmp(value, "pmu-adb")) {
        cms->via_config = CORE99_VIA_CONFIG_PMU_ADB;
    } else {
        error_setg(errp, "Invalid via value");
        error_append_hint(errp, "Valid values are cuda, pmu, pmu-adb.\n");
    }
}

static void core99_instance_init(Object *obj)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    /* Default via_config is CORE99_VIA_CONFIG_CUDA */
    cms->via_config = CORE99_VIA_CONFIG_CUDA;
    object_property_add_str(obj, "via", core99_get_via_config,
                            core99_set_via_config);
    object_property_set_description(obj, "via",
                                    "Set VIA configuration. "
                                    "Valid values are cuda, pmu and pmu-adb");
}

static const TypeInfo core99_machine_info = {
    .name          = MACHINE_TYPE_NAME("mac99"),
    .parent        = TYPE_MACHINE,
    .class_init    = core99_machine_class_init,
    .instance_init = core99_instance_init,
    .instance_size = sizeof(Core99MachineState),
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_FW_PATH_PROVIDER },
        { }
    },
};

static void mac_machine_register_types(void)
{
    type_register_static(&core99_machine_info);
}

type_init(mac_machine_register_types)
