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
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "hw/core/boards.h"
#include "hw/pci-host/uninorth.h"
#include "hw/input/adb.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/pci/pci.h"
#include "net/net.h"
#include "system/system.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/nvram/mac_spd.h"
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

#define MAX_IDE_BUS 3
#define CFG_ADDR 0xf0000510
#define TBFREQ (25UL * 1000UL * 1000UL)
#define CLOCKFREQ (900UL * 1000UL * 1000UL)
#define BUSFREQ (100UL * 1000UL * 1000UL)

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xfff00000
/* KeyLargo/MacIO base decoded by real hardware (and by real Apple ROMs) */
#define MACIO_BASE 0xf3000000
#define MACIO_WIN_SIZE 0x01000000
#define PROM_SIZE (1 * MiB)
/*
 * A real Apple ROM stores its Open Firmware variables in the boot flash --
 * the ROM's own info property lists fff04000 as a 0x4000 nvram section and
 * its write-characteristic as "flash" -- and reaches it as flat bytes rather
 * than through the Old World one-byte-per-halfword window.
 */
#define NVRAM_FLASH_SIZE 0x4000
/* The ROM socket's full decode window; only the top PROM_SIZE is populated */
#define ROM_WINDOW_BASE 0xff800000
#define ROM_WINDOW_SIZE (8 * MiB)

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

    /*
     * Secondary CPUs are held in reset until the guest OS releases them
     * via the KeyLargo GPIO soft-reset lines (see cpu_kick() below).
     */
    if (cs->cpu_index > 0) {
        cs->halted = 1;
    }
}

/*
 * Called when a KeyLargo GPIO soft-reset line for a secondary CPU changes
 * level. The lines are active low: level == 0 means the reset is
 * deasserted and the CPU should start running.
 */
static void cpu_kick(void *opaque, int n, int level)
{
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUState *first_cs = first_cpu;

    if (level || !cs->halted) {
        return;
    }

    /*
     * Re-sync this CPU's timebase offset with the primary CPU's before
     * starting it, in case of any drift while it was halted.
     */
    if (first_cs && first_cs != cs) {
        PowerPCCPU *first = POWERPC_CPU(first_cs);

        cpu->env.tb_env->tb_offset = first->env.tb_env->tb_offset;
        cpu->env.tb_env->atb_offset = first->env.tb_env->atb_offset;
    }

    /*
     * The reset vector spin loop the firmware places at 0x100 reads a
     * mailbox to find its real entry point, matching real hardware's
     * secondary-CPU boot protocol.
     */
    cpu->env.excp_prefix = 0;
    cpu->env.nip = 0x100;
    cpu->env.msr = 0;

    cs->halted = 0;
    cs->exception_index = -1;
    qemu_cpu_kick(cs);
}

/* OpenBIOS is an ELF; a real Apple ROM is a flat binary image. */
static bool firmware_is_elf(const char *filename)
{
    uint8_t magic[4];
    bool is_elf = false;
    FILE *f = fopen(filename, "rb");

    if (f) {
        is_elf = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                 !memcmp(magic, ELFMAG, sizeof(magic));
        fclose(f);
    }
    return is_elf;
}

static void macio_map_at_hw_base(void *opaque)
{
    PCIDevice *macio = opaque;

    pci_default_write_config(macio, PCI_BASE_ADDRESS_0, MACIO_BASE, 4);
    pci_default_write_config(macio, PCI_COMMAND,
                             PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER, 2);
}

typedef struct MacIOHwBaseNotifier {
    Notifier notifier;
    PCIDevice *macio;
} MacIOHwBaseNotifier;

static void macio_arm_hw_base_reset(Notifier *n, void *opaque)
{
    MacIOHwBaseNotifier *mn = container_of(n, MacIOHwBaseNotifier, notifier);

    /*
     * Deferred to machine-init-done on purpose: the sysbus (and with it the
     * whole device tree) only becomes resettable at the end of machine
     * creation, so a handler registered during ppc_core99_init() would run
     * *before* MacIO's own reset and have its config write thrown away.
     */
    qemu_register_reset(macio_map_at_hw_base, mn->macio);
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
    bool rom_is_flash;
    hwaddr kernel_base = 0, initrd_base = 0, cmdline_base = 0;
    long kernel_size = 0, initrd_size = 0;
    PCIBus *pci_bus;
    bool has_pmu, has_adb;
    Object *macio;
    MACIOIDEState *macio_ide;
    BusState *adb_bus;
    MacIONVRAMState *nvr;
    DriveInfo *dinfo, *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    void *fw_cfg;
    SysBusDevice *s;
    DeviceState *dev, *pic_dev, *uninorth_pci_dev;
    DeviceState *uninorth_internal_dev = NULL, *uninorth_agp_dev = NULL;
    MemoryRegion *macio_win;
    I2CBus *unin_i2c;
    MacIOHwBaseNotifier *macio_notifier;
    hwaddr nvram_addr = 0xFFF04000;
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TBFREQ;

    /* init CPUs */
    cpus = g_new0(PowerPCCPU *, machine->smp.cpus);
    for (i = 0; i < machine->smp.cpus; i++) {
        cpus[i] = POWERPC_CPU(cpu_create(machine->cpu_type));

        /* Set time-base frequency to 100 Mhz */
        cpu_ppc_tb_init(&cpus[i]->env, TBFREQ);

        /*
         * Secondary CPUs share the primary's timebase offset: real
         * hardware has one timebase counter shared by all CPUs, each
         * with its own decrementer.
         */
        if (i > 0) {
            cpus[i]->env.tb_env->tb_offset = cpus[0]->env.tb_env->tb_offset;
            cpus[i]->env.tb_env->atb_offset = cpus[0]->env.tb_env->atb_offset;
        }

        qemu_register_reset(ppc_core99_reset, cpus[i]);

        /* Secondary CPUs start halted; they are kicked via GPIO */
        if (i > 0) {
            CPU(cpus[i])->halted = 1;
        }
    }
    env = &cpus[0]->env;

    /* allocate RAM */
    if (machine->ram_size > 2 * GiB) {
        error_report("RAM size more than 2 GiB is not supported");
        exit(1);
    }
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* allocate and load firmware ROM */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    rom_is_flash = filename && !firmware_is_elf(filename);

    memory_region_init_rom(bios, NULL, "ppc_core99.bios", PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, bios);

    /*
     * The ROM socket decodes 8MB at ROM_WINDOW_BASE (the real machine's
     * /rom@ff800000 node has ranges ff800000 00800000), but only the top 1MB
     * is populated, so the part aliases through the rest of the window. A
     * real Apple ROM makes use of that: after its low-level init it jumps to
     * code via a window-relative address such as 0xff80b644, which on
     * hardware is simply offset 0xb644 of the same flash part.
     */
    for (i = 0; i < ROM_WINDOW_SIZE / PROM_SIZE - 1; i++) {
        MemoryRegion *mirror = g_new(MemoryRegion, 1);
        g_autofree char *name = g_strdup_printf("ppc_core99.bios.mirror%d", i);

        memory_region_init_alias(mirror, NULL, name, bios, 0, PROM_SIZE);
        memory_region_add_subregion(get_system_memory(),
                                    ROM_WINDOW_BASE + (hwaddr)i * PROM_SIZE,
                                    mirror);
    }

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
                                    machine->ram_size - kernel_base,
                                    true, TARGET_PAGE_SIZE);
        }
        if (kernel_size < 0) {
            kernel_size = load_image_targphys(machine->kernel_filename,
                                              kernel_base,
                                              machine->ram_size - kernel_base,
                                              &error_fatal);
        }
        /* load initrd */
        if (machine->initrd_filename) {
            initrd_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size + KERNEL_GAP);
            initrd_size = load_image_targphys(machine->initrd_filename,
                                              initrd_base,
                                              machine->ram_size - initrd_base,
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
    sysbus_realize_and_unref(s, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0xf8000000,
                                sysbus_mmio_get_region(s, 0));
    /*
     * UniNorth's own I2C controller. Its bus reaches the DIMM SPD EEPROMs and
     * the processor module's configuration EEPROM, which a real Apple ROM
     * reads during early bring-up; OpenBIOS never touches it.
     */
    /* Sits inside the uni-n window above, so it needs to win the overlap. */
    memory_region_add_subregion_overlap(get_system_memory(), UNINORTH_I2C_BASE,
                                        sysbus_mmio_get_region(s, 1), 1);

    /*
     * The processor module carries an SPD-format configuration EEPROM. The
     * Tangent schematic labels it "ADDRESS = 6 (AC/AD)" next to the module
     * connector's I2C pins, i.e. 8-bit 0xac/0xad, 7-bit 0x56. A real Apple
     * ROM reads it during bring-up and refuses to continue without it.
     */
    unin_i2c = UNI_NORTH(s)->i2c.bus;
    at24c_eeprom_init_rom(unin_i2c, 0x56, 256, NULL, 0);

    /*
     * SPD EEPROMs for the memory slots, at the usual 0x50 + slot. The ROM
     * walks these to size RAM, so populate one per 256MB of configured
     * memory using the real machine's DIMM data; -m 768 reproduces the
     * donor PowerMac3,4 exactly.
     */
    for (i = 0; i < MAC_SPD_NUM_DIMMS; i++) {
        if (machine->ram_size <= (uint64_t)i * MAC_SPD_DIMM_BYTES) {
            break;
        }
        at24c_eeprom_init_rom(unin_i2c, 0x50 + i, MAC_SPD_SIZE,
                              mac_spd_dimms[i], MAC_SPD_SIZE);
    }

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
        memory_region_add_subregion(get_system_memory(), 0x80000000,
                                    sysbus_mmio_get_region(s, 2));
        /* Register 8 MB of ISA IO space */
        memory_region_add_subregion(get_system_memory(), 0xf2000000,
                                    sysbus_mmio_get_region(s, 3));
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

    /* init basic PC hardware */
    pci_bus = PCI_HOST_BRIDGE(uninorth_pci_dev)->bus;

    /* MacIO */
    /*
     * Slot numbers matter to a real Apple ROM, which expects the built-in
     * devices where the hardware puts them: the PowerMac3,4 device tree has
     * mac-io@17 and usb@18 / usb@19 on this bus.
     */
    macio = OBJECT(pci_new(PCI_DEVFN(0x17, 0), TYPE_NEWWORLD_MACIO));
    dev = DEVICE(macio);
    qdev_prop_set_uint64(dev, "frequency", tbfreq);
    qdev_prop_set_bit(dev, "has-pmu", has_pmu);
    qdev_prop_set_bit(dev, "has-adb", has_adb);

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    qdev_prop_set_uint32(pic_dev, "nb_cpus", machine->smp.cpus);

    pci_realize_and_unref(PCI_DEVICE(macio), pci_bus, &error_fatal);

    /*
     * Have MacIO come out of reset already decoding at the address real
     * hardware uses, instead of leaving it to firmware. Registered as a
     * reset handler because PCI config space (including BARs) is cleared
     * when the device is reset, which happens after machine init.
     *
     * OpenBIOS enumerates the PCI bus and assigns BARs itself, so it never
     * needed this. A real Apple boot ROM does not: its early hardware-init
     * code reaches straight for KeyLargo at the hardwired 0xf3000000 (e.g.
     * the 4.2.8 Tangent ROM polls a status bit at 0xf3010000 long before
     * any PCI configuration happens) and spins forever if nothing answers.
     * Firmware that does enumerate simply reassigns the BAR afterwards, so
     * this is harmless for the OpenBIOS path.
     */
    macio_notifier = g_new0(MacIOHwBaseNotifier, 1);
    macio_notifier->notifier.notify = macio_arm_hw_base_reset;
    macio_notifier->macio = PCI_DEVICE(macio);
    qemu_add_machine_init_done_notifier(&macio_notifier->notifier);

    /*
     * UniNorth's only CPU-visible window into PCI memory space is the
     * 256MB "PCI hole" at 0x80000000, so a BAR at 0xf3000000 would decode
     * on the bus but be unreachable from the CPU. Real UniNorth also
     * forwards the 0xf3000000 range, which is where MacIO lives; add that
     * window so the BAR above is actually usable.
     */
    macio_win = g_new(MemoryRegion, 1);
    memory_region_init_alias(macio_win, macio, "unin-pci-macio-hole",
                             pci_address_space(PCI_DEVICE(macio)),
                             MACIO_BASE, MACIO_WIN_SIZE);
    memory_region_add_subregion(get_system_memory(), MACIO_BASE, macio_win);

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    for (i = 0; i < 4; i++) {
        qdev_connect_gpio_out(uninorth_pci_dev, i,
                              qdev_get_gpio_in(pic_dev, 0x1b + i));
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

    /*
     * Wire the KeyLargo GPIO soft-reset lines for secondary CPUs (1-3) to
     * cpu_kick(), so the guest OS can release each one from reset to
     * start it running (see gpio.c GPIO 4/15/16 handling).
     */
    s = SYS_BUS_DEVICE(object_resolve_path_component(macio, "gpio"));
    if (machine->smp.cpus > 1) {
        cpu_kick_irq = qemu_allocate_irq(cpu_kick, cpus[1], 0);
        sysbus_connect_irq(s, 4, cpu_kick_irq);
    }
    if (machine->smp.cpus > 2) {
        cpu_kick_irq = qemu_allocate_irq(cpu_kick, cpus[2], 0);
        sysbus_connect_irq(s, 15, cpu_kick_irq);
    }
    if (machine->smp.cpus > 3) {
        cpu_kick_irq = qemu_allocate_irq(cpu_kick, cpus[3], 0);
        sysbus_connect_irq(s, 16, cpu_kick_irq);
    }
    g_free(cpus);

    /*
     * KeyLargo's three ATA buses: ata-4@1f000 carries the hard disks (Open
     * Firmware's "hd" and "ultra0"/"ultra1" aliases), ata-3@20000 is
     * "ide0"/"ide1"/"cd" and ata-3@21000 is "zip".
     */
    ide_drive_get(hd, ARRAY_SIZE(hd));

    for (i = 0; i < MAX_IDE_BUS; i++) {
        g_autofree char *name = g_strdup_printf("ide[%d]", i);

        macio_ide = MACIO_IDE(object_resolve_path_component(macio, name));
        macio_ide_init_drives(macio_ide, &hd[i * MAX_IDE_DEVS]);
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
        pci_create_simple(pci_bus, PCI_DEVFN(0x18, 0), "pci-ohci");

        /* U3 needs to use USB for input because Linux doesn't support via-cuda
        on PPC64 */
        if (!has_adb || machine_arch == ARCH_MAC99_U3) {
            USBBus *usb_bus;

            usb_bus = USB_BUS(object_resolve_type_unambiguous(TYPE_USB_BUS,
                                                              &error_abort));
            usb_create_simple(usb_bus, "usb-kbd");
            usb_create_simple(usb_bus, "usb-mouse");
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

    pci_init_nic_devices(pci_bus, mc->default_nic);

    /* The NewWorld NVRAM is not located in the MacIO device */
    if (kvm_enabled() && qemu_real_host_page_size() > 4096) {
        /* We can't combine read-write and read-only in a single page, so
           move the NVRAM out of ROM again for KVM */
        nvram_addr = 0xFFE00000;
    }
    dev = qdev_new(TYPE_MACIO_NVRAM);
    if (rom_is_flash) {
        /* Flat bytes, flash command set, and erased rather than zeroed. */
        qdev_prop_set_uint32(dev, "size", NVRAM_FLASH_SIZE);
        qdev_prop_set_uint32(dev, "it_shift", 0);
        qdev_prop_set_bit(dev, "flash", true);
    } else {
        qdev_prop_set_uint32(dev, "size", MACIO_NVRAM_SIZE);
        qdev_prop_set_uint32(dev, "it_shift", 1);
    }

    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive(dev, "drive", blk_by_legacy_dinfo(dinfo));
    } else {
        BlockBackend *nvram_blk = rom_is_flash ?
            macio_nvram_default_blk("nvram-flash.img", NVRAM_FLASH_SIZE, 0xff) :
            macio_nvram_default_blk("nvram.img", MACIO_NVRAM_SIZE, 0);

        if (nvram_blk) {
            qdev_prop_set_drive(dev, "drive", nvram_blk);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, nvram_addr);
    nvr = MACIO_NVRAM(dev);
    /*
     * Only seed the default partitions into a genuinely fresh image --
     * otherwise the firmware's own persisted variables get overwritten on
     * every boot, which is exactly what we are trying to stop.
     */
    /*
     * The Apple ROM lays out and validates its own partitions; only the
     * OpenBIOS-style NVRAM wants the CHRP ones seeded, and even then only
     * into a genuinely fresh image.
     */
    if (!rom_is_flash && macio_nvram_is_blank(nvr, MACIO_NVRAM_SIZE)) {
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
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)machine->ram_size);
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
        /* The Ultra ATA/66 bus is an ata-4 node; the other two are ata-3. */
        return g_strdup_printf("ata-%d@%x", macio_ide->addr == 0x1f000 ? 4 : 3,
                               macio_ide->addr);
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
    /* SMP supported via KeyLargo GPIO-based secondary CPU reset control */
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
