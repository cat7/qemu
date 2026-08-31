
/*
 * QEMU OldWorld PowerMac (currently ~G3 Beige) hardware System Emulator
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
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "exec/target_page.h"
#include "hw/ppc/ppc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/boards.h"
#include "hw/input/adb.h"
#include "system/system.h"
#include "net/net.h"
#include "hw/isa/isa.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/grackle.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/misc/macio/cuda.h"
#include "hw/display/ati_mach64_int.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/core/loader.h"
#include "hw/core/fw-path-provider.h"
#include "elf.h"
#include "qemu/error-report.h"
#include "system/block-backend.h"
#include "qobject/qdict.h"
#include "system/kvm.h"
#include "system/reset.h"
#include "system/calib-governor.h"
#include "qapi/visitor.h"
#include "kvm_ppc.h"

#define MAX_IDE_BUS 2
#define CFG_ADDR 0xf0000510
/*
 * Real hardware's actual clock-frequency values, confirmed against this
 * exact ROM's own device-tree dump (SourceFiles/G3/PowerMacG3-device-tree.txt:
 * root node "clock-frequency"=0x03fb97a0=66,820,000 Hz; cpus/PowerPC,750
 * "clock-frequency"=0x11ec2a50=300,690,000 Hz). The previous rounded
 * 66,000,000/266,000,000 values are close but not exact -- the ROM reads
 * these via fw_cfg (FW_CFG_PPC_BUSFREQ/CLOCKFREQ) for its own boot-time
 * timing calibration (confirmed this session: a ROM routine reads a VIA
 * Timer 2 counter byte in a tight loop specifically to measure real
 * elapsed time against an expected CPU-cycles-per-tick ratio derived
 * from these reported frequencies), so an inexact value skews that
 * calibration systematically, independent of any TCG/icount timing
 * variance. 66,820,000 Hz also matches DingusPPC's own
 * "BUS_FREQ_66P82" constant name in machines/machinegossamer.cpp,
 * confirming this is the real, intended value for this board.
 */
#define CLOCKFREQ 300690000UL
#define BUSFREQ 66820000UL
/*
 * Same device-tree dump, cpus/PowerPC,750 "timebase-frequency" =
 * 0x00fee5e8 = 16,705,000 Hz -- exactly BUSFREQ/4, the standard 60x-bus
 * PowerPC convention (timebase increments once per 4 bus clocks). The
 * previous hardcoded 16,600,000 UL was a separate, never-updated rounded
 * value left over from before BUSFREQ/CLOCKFREQ were corrected to their
 * exact real-hardware figures above -- fixed here to derive from the
 * same, now-accurate BUSFREQ constant instead of drifting independently.
 */
#define TBFREQ (BUSFREQ / 4)

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xffc00000
#define PROM_SIZE (4 * MiB)

#define KERNEL_LOAD_ADDR 0x01000000
#define KERNEL_GAP       0x00100000

#define GRACKLE_BASE 0xfec00000

/*
 * Real Gossamer-board (Beige G3) hardware exposes a 16-bit "machine ID"
 * register at physical address 0xFF000004, readable by ROM boot code to
 * identify board capabilities (floppy controller type, ROM burst-read
 * support, which PCI slots are physically present, CPU/cache clock
 * ratio, bus speed, and whether this is an All-in-One-style board).
 * QEMU previously left this address entirely unimplemented, so reads
 * fell through to Grackle's PCI-hole "no target responds" default --
 * not the real machine-identification value the ROM expects. Confirmed
 * against DingusPPC's GossamerID device (devices/common/machineid.h,
 * machines/machinegossamer.cpp): a real Beige G3 desktop's value is
 * built from FDC_TYPE_SWIM3(0x8000) | BURST_ROM_TRUE(0) |
 * PCI_A_PRSNT(0x3F<<8) | PCM_PID(1<<5) | AIO_PRSNT_FALSE(1<<4) |
 * BUS_FREQ_66P82(6<<1) | BOSE_PRSNT_FALSE(1) = 0xBF3D.
 */
#define MACHINE_ID_BASE 0xff000000
#define MACHINE_ID_SIZE 4096
#define MACHINE_ID_VALUE 0xbf3d

static uint64_t machine_id_read(void *opaque, hwaddr addr, unsigned size)
{
    if (addr == 4 && size == 2) {
        return MACHINE_ID_VALUE;
    }
    return 0;
}

static void machine_id_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    /* read-only */
}

static const MemoryRegionOps machine_id_ops = {
    .read = machine_id_read,
    .write = machine_id_write,
    .endianness = DEVICE_BIG_ENDIAN,
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

static void ppc_heathrow_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;

    cpu_ppc_tb_reset(&cpu->env);
    cpu_reset(CPU(cpu));

    /*
     * HID1's upper PLL_CFG bits reflect the bus-to-core clock multiplier
     * board straps on real hardware (sampled at power-on, not something
     * the CPU or firmware sets), so cpu_reset() zeroing them leaves an
     * unrecognized/invalid multiplier encoding. A real Apple ROM's
     * boot-time hardware-init code reads this to identify the CPU speed
     * and aborts if it doesn't recognize the value -- matches the
     * "AAPL,PowerMac G3" Beige G3 board strap used by DingusPPC's
     * Gossamer machine, and the same pattern already used for the
     * Pegasos machine below (see pegasos_cpu_reset()).
     */
    cpu->env.spr[SPR_HID1] = 0xEULL << 28;
}

/*
 * Build a minimal SPD EEPROM image for a 168-pin SDRAM DIMM.
 *
 * QEMU's generic spd_data_generate() (hw/i2c/smbus_eeprom.c) is designed
 * for x86 SMBIOS reporting, where the actual usable RAM size comes from
 * elsewhere (e820) and the SPD content only needs to look plausible. Real
 * Beige G3 ROM firmware, by contrast, *derives* the installed RAM size
 * from the row/column/bank fields it reads back over I2C, so the encoded
 * capacity must multiply out exactly or the ROM's memory-sizing code
 * miscomputes and corrupts later boot state (observed as a completely
 * different crash -- a stack pointer landing in unmapped memory -- at
 * RAM sizes other than 256 MiB).
 *
 * This mirrors DingusPPC's SpdSdram168::set_capacity() table exactly
 * (verified against real Beige G3 ROM boot behavior): fixed 12 row
 * address bits, with column count and bank count varying by capacity.
 */
static uint8_t *g3beige_spd_data_generate(uint64_t ram_size)
{
    static const struct { uint32_t megs; uint8_t cols, banks; } table[] = {
        { 8,   6,  1 },
        { 16,  7,  1 },
        { 32,  8,  1 },
        { 64,  9,  1 },
        { 128, 10, 1 },
        { 256, 10, 2 },
        { 512, 11, 2 },
    };
    uint32_t megs = ram_size / MiB;
    int i;

    /* pick the largest supported capacity not exceeding what's configured */
    for (i = ARRAY_SIZE(table) - 1; i > 0 && table[i].megs > megs; i--) {
    }

    uint8_t *spd = g_malloc0(256);
    spd[0] = 128; /* number of bytes present */
    spd[1] = 8;   /* log2(EEPROM size) */
    spd[2] = 4;   /* memory type: SDRAM */
    spd[3] = 0xC; /* number of row addresses (12, fixed) */
    spd[4] = table[i].cols;
    spd[5] = table[i].banks;
    return spd;
}

/*
 * Auto-manage a default NVRAM backing file, "nvram.img", when the user
 * hasn't attached one explicitly via -drive if=mtd,... -- creates it
 * (empty) on first use and mac_nvram.c's Old World formatter fills it
 * with a valid default OF partition; on later runs, its already-valid
 * persisted content is read back and left alone instead of being
 * overwritten again (see pmac_format_nvram_partition_oldworld()).
 */
static BlockBackend *mac_oldworld_default_nvram_blk(void)
{
    static const char filename[] = "nvram.img";
    struct stat st;
    BlockBackend *blk;
    Error *local_err = NULL;
    int fd;

    fd = qemu_open_old(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        warn_report("could not open/create default NVRAM image '%s': %s",
                    filename, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) == 0 && st.st_size < MACIO_NVRAM_SIZE) {
        if (ftruncate(fd, MACIO_NVRAM_SIZE) != 0) {
            warn_report("could not size default NVRAM image '%s': %s",
                        filename, strerror(errno));
        }
    }
    close(fd);

    QDict *options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(filename, NULL, options, BDRV_O_RDWR, &local_err);
    if (!blk) {
        warn_report_err(local_err);
        return NULL;
    }
    return blk;
}

static void ppc_heathrow_init(MachineState *machine)
{
    const char *bios_name = machine->firmware ?: PROM_FILENAME;
    PowerPCCPU *cpu = NULL;
    CPUPPCState *env = NULL;
    char *filename;
    int i, bios_size = -1;
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    uint64_t bios_addr;
    uint32_t kernel_base = 0, initrd_base = 0, cmdline_base = 0;
    int32_t kernel_size = 0, initrd_size = 0;
    PCIBus *pci_bus;
    Object *macio;
    MACIOIDEState *macio_ide;
    SysBusDevice *s;
    DeviceState *dev, *pic_dev, *grackle_dev;
    BusState *adb_bus;
    uint16_t ppc_boot_device;
    DriveInfo *dinfo, *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    void *fw_cfg;
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq() : TBFREQ;

    /*
     * This board's ROM is the one that needs the windowed calibration
     * governor (see system/calib-governor.c), so this board is what
     * turns it on -- the mos6522 hook itself is generic and stays inert
     * on every other machine. An explicit calibration-governor= on the
     * command line has already been applied and wins over this default.
     */
    calib_governor_default_on();

    /* init CPUs */
    for (i = 0; i < machine->smp.cpus; i++) {
        cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
        env = &cpu->env;

        /* Set time-base frequency to 16.6 Mhz */
        cpu_ppc_tb_init(env,  TBFREQ);
        qemu_register_reset(ppc_heathrow_reset, cpu);
    }

    /* allocate RAM */
    if (machine->ram_size > 2047 * MiB) {
        error_report("Too much memory for this machine: %" PRId64 " MB, "
                     "maximum 2047 MB", machine->ram_size / MiB);
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    /* allocate and load firmware ROM */
    memory_region_init_rom(bios, NULL, "ppc_heathrow.bios", PROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, bios);

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        /* Load OpenBIOS (ELF) */
        bios_size = load_elf(filename, NULL, NULL, NULL, NULL, &bios_addr,
                             NULL, NULL, ELFDATA2MSB, PPC_ELF_MACHINE, 0, 0);
        /* Unfortunately, load_elf sign-extends reading elf32 */
        bios_addr = (uint32_t)bios_addr;

        if (bios_size <= 0) {
            /* or if could not load ELF try loading a binary ROM image */
            bios_size = load_image_targphys(filename, PROM_BASE, PROM_SIZE,
                                            &error_fatal);
            bios_addr = PROM_BASE;
        }
        g_free(filename);
    }
    if (bios_size < 0 || bios_addr - PROM_BASE + bios_size > PROM_SIZE) {
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
            initrd_base = TARGET_PAGE_ALIGN(kernel_base + kernel_size +
                                            KERNEL_GAP);
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
        for (i = 0; machine->boot_config.order[i] != '\0'; i++) {
            /*
             * TOFIX: for now, the second IDE channel is not properly
             *        used by OHW. The Mac floppy disk are not emulated.
             *        For now, OHW cannot boot from the network.
             */
#if 0
            if (machine->boot_config.order[i] >= 'a' &&
                machine->boot_config.order[i] <= 'f') {
                ppc_boot_device = machine->boot_config.order[i];
                break;
            }
#else
            if (machine->boot_config.order[i] >= 'c' &&
                machine->boot_config.order[i] <= 'd') {
                ppc_boot_device = machine->boot_config.order[i];
                break;
            }
#endif
        }
        if (ppc_boot_device == '\0') {
            error_report("No valid boot device for G3 Beige machine");
            exit(1);
        }
    }

    /* Grackle PCI host bridge */
    grackle_dev = qdev_new(TYPE_GRACKLE_PCI_HOST_BRIDGE);
    qdev_prop_set_uint32(grackle_dev, "ofw-addr", 0x80000000);
    s = SYS_BUS_DEVICE(grackle_dev);
    sysbus_realize_and_unref(s, &error_fatal);

    sysbus_mmio_map(s, 0, GRACKLE_BASE);
    sysbus_mmio_map(s, 1, GRACKLE_BASE + 0x200000);
    /* PCI hole */
    memory_region_add_subregion(get_system_memory(), 0x80000000ULL,
                                sysbus_mmio_get_region(s, 2));
    /* Register 2 MB of ISA IO space */
    memory_region_add_subregion(get_system_memory(), 0xfe000000,
                                sysbus_mmio_get_region(s, 3));

    /* Gossamer-board machine ID register, punched through the PCI hole */
    MemoryRegion *machine_id = g_new(MemoryRegion, 1);
    memory_region_init_io(machine_id, NULL, &machine_id_ops, NULL,
                          "machine-id", MACHINE_ID_SIZE);
    memory_region_add_subregion_overlap(get_system_memory(), MACHINE_ID_BASE,
                                        machine_id, 1);

    pci_bus = PCI_HOST_BRIDGE(grackle_dev)->bus;

    /* MacIO */
    macio = OBJECT(pci_new(PCI_DEVFN(16, 0), TYPE_OLDWORLD_MACIO));
    qdev_prop_set_uint64(DEVICE(macio), "frequency", tbfreq);

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        dev = DEVICE(object_resolve_path_component(macio, "nvram"));
        qdev_prop_set_drive(dev, "drive", blk_by_legacy_dinfo(dinfo));
    } else {
        /*
         * No user-supplied NVRAM image (-drive if=mtd,...) -- manage a
         * default one ourselves so NVRAM contents (the Old World OF
         * partition mac_nvram.c seeds on first use) persist across runs
         * instead of being re-stamped from scratch on every boot.
         */
        BlockBackend *nvram_blk = mac_oldworld_default_nvram_blk();
        if (nvram_blk) {
            dev = DEVICE(object_resolve_path_component(macio, "nvram"));
            qdev_prop_set_drive(dev, "drive", nvram_blk);
        }
    }

    pci_realize_and_unref(PCI_DEVICE(macio), pci_bus, &error_fatal);

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    /*
     * Heathrow lines 0x16-0x19: onboard GPU, PCI slots A1/B1/C1 --
     * see pci_grackle_map_irq() for the real-hardware wiring this
     * index order encodes. Requires the ATI model's pulse-style VBL
     * interrupt; see the note there.
     */
    for (i = 0; i < 4; i++) {
        qdev_connect_gpio_out(grackle_dev, i,
                              qdev_get_gpio_in(pic_dev, 0x16 + i));
    }

    /* Connect the heathrow PIC outputs to the 6xx bus */
    for (i = 0; i < machine->smp.cpus; i++) {
        switch (PPC_INPUT(env)) {
        case PPC_FLAGS_INPUT_6xx:
            /* XXX: we register only 1 output pin for heathrow PIC */
            qdev_connect_gpio_out(pic_dev, 0,
                              qdev_get_gpio_in(DEVICE(cpu), PPC6xx_INPUT_INT));
            break;
        default:
            error_report("Bus model not supported on OldWorld Mac machine");
            exit(1);
        }
    }

    /*
     * Real hardware wires the ATI card at PCI device 18 (OF slot name
     * "F1", confirmed via this machine's own device-tree dump) and the
     * native ROM's PCI probe scans that specific slot rather than
     * discovering devices generically -- auto-assigning devfn here
     * left the card undetected and triggered a stack-pointer corruption
     * in the ROM's native code once it found nothing at slot 18.
     */
    pci_create_simple(pci_bus, PCI_DEVFN(18, 0), TYPE_ATI_MACH64);

    /* MacIO IDE */
    ide_drive_get(hd, ARRAY_SIZE(hd));
    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[0]"));
    macio_ide_init_drives(macio_ide, hd);

    macio_ide = MACIO_IDE(object_resolve_path_component(macio, "ide[1]"));
    macio_ide_init_drives(macio_ide, &hd[MAX_IDE_DEVS]);

    /* MacIO CUDA/ADB */
    dev = DEVICE(object_resolve_path_component(macio, "cuda"));
    adb_bus = qdev_get_child_bus(dev, "adb.0");

    /*
     * Real hardware determines its RAM configuration by reading an I2C
     * SPD EEPROM on each populated DIMM slot via CUDA's I2C pass-through.
     * Without it, real Beige G3 ROM/Toolbox RAM-sizing code walks off
     * into the weeds (this was found to precede a later stack-pointer
     * corruption during early boot).
     *
     * The Gossamer board has 3 physical DIMM slots, addressed 0x57/0x56/
     * 0x55 for DIMM_1/2/3 respectively (confirmed against DingusPPC's
     * real, working `machines/machinegossamer.cpp`: `setup_ram_slot(
     * "RAM_DIMM_1", 0x57, ...)` etc. -- NOT the generic 168-pin-SPD
     * 0x50-based sequential scheme `spdram.h`'s own doc comment
     * describes, which is a different board's SA0-2 strapping, not this
     * one). This machine only ever configures a single RAM bank, so only
     * DIMM_1 (0x57) is populated; DIMM_2/3 (0x56/0x55) are correctly left
     * unregistered, matching a real board with only the first slot
     * filled (the I2C bus already NAKs unclaimed addresses, exactly the
     * "empty slot" behavior real hardware would show there).
     *
     * A prior version of this code used a nonstandard 0x50 (not a real
     * address on this board at all) after switching to 0x57 once caused
     * a regression -- but `g3beige_spd_data_generate()`'s content was
     * never actually wrong: checked byte-for-byte against DingusPPC's
     * own `SpdSdram168::eeprom_data`/`set_capacity()` (the reference this
     * project's own real-Finder-boot milestone was achieved against) and
     * it matches exactly, field for field, for every capacity DingusPPC
     * supports. The earlier regression was most likely from registering
     * TWO devices (0x50 and 0x57) simultaneously, not from bad content --
     * fixed by registering only the one real address.
     */
    smbus_eeprom_init_one(CUDA(dev)->i2c_bus, 0x57,
                          g3beige_spd_data_generate(machine->ram_size));

    /*
     * Real hardware also has a small I2C EEPROM at 0x53 identifying the
     * "Whisper" personality card (confirmed via the device tree's
     * "perch" node, iic-address 0xa6 = 0x53 << 1, compatible="Whisper"),
     * which DingusPPC's Gossamer machine likewise creates
     * (machines/machinegossamer.cpp's "Perch" I2CProm). Content matches
     * DingusPPC's WhisperID byte-for-byte: 16-byte ID string, 16 zero
     * bytes, then 0xFF padding for the rest of the 256-byte part except
     * the very last byte (left at the buffer's default zero-fill).
     */
    {
        static const uint8_t whisper_id[16] = {
            0x0f, 0xaa, 0x55, 0xaa, 0x57, 0x68, 0x69, 0x73,
            0x70, 0x65, 0x72, 0x00, 0x00, 0x00, 0x00, 0x02,
        };
        uint8_t *perch_buf = g_malloc0(256);
        memset(perch_buf + 32, 0xff, 223);
        memcpy(perch_buf, whisper_id, sizeof(whisper_id));
        smbus_eeprom_init_one(CUDA(dev)->i2c_bus, 0x53, perch_buf);
    }

    /*
     * Real hardware also has an Athens (Apple part# 343S1191) clock
     * generator ASIC on this same I2C bus at address 0x28, confirmed via
     * DingusPPC's machinegossamer.cpp. Without it, this address's
     * CUDA_COMBINED_FORMAT_IIC presence probe NAKs; native ROM code uses
     * exactly this kind of onboard-device probe (alongside the SPD/Perch
     * probes above) to decide whether to fall into a factory-diagnostics
     * serial console instead of continuing normal boot -- with every
     * probed address NAK'ing (this device was entirely missing), it
     * always took that path and then hung forever waiting for serial
     * input that headless boot can never provide.
     */
    i2c_slave_create_simple(CUDA(dev)->i2c_bus, "athens", 0x28);

    /*
     * Real hardware also has a TDA7433 audio tone/volume control chip on
     * this same I2C bus at address 0x45, alongside the AWACS/Screamer
     * sound codec's own MMIO register block (confirmed via DingusPPC's
     * devices/sound/awacs.cpp, which registers its AudioProcessor at this
     * same address). Without it, this address's CUDA_COMBINED_FORMAT_IIC
     * presence probe NAKs, contributing to the factory-diagnostics
     * serial-console fallback described above.
     */
    i2c_slave_create_simple(CUDA(dev)->i2c_bus, "tda7433", 0x45);

    dev = qdev_new(TYPE_ADB_KEYBOARD);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);
    dev = qdev_new(TYPE_ADB_MOUSE);
    qdev_realize_and_unref(dev, adb_bus, &error_fatal);

    if (machine_usb(machine)) {
        pci_create_simple(pci_bus, -1, "pci-ohci");
    }

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
    fw_cfg_add_i16(fw_cfg, FW_CFG_MACHINE_ID, ARCH_HEATHROW);
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
static char *heathrow_fw_dev_path(FWPathProvider *p, BusState *bus,
                                  DeviceState *dev)
{
    PCIDevice *pci;
    MACIOIDEState *macio_ide;

    if (!strcmp(object_get_typename(OBJECT(dev)), "macio-oldworld")) {
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

static int heathrow_kvm_type(MachineState *machine, const char *arg)
{
    /* Always force PR KVM */
    return 2;
}

static char *heathrow_get_calib_governor(Object *obj, Error **errp)
{
    return calib_governor_get_config();
}

static void heathrow_set_calib_governor(Object *obj, const char *value,
                                        Error **errp)
{
    calib_governor_configure(value, errp);
}

static void heathrow_get_gov_stat(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    CalibGovStats gov;
    uint64_t value;

    calib_governor_get_stats(&gov);
    switch ((uintptr_t)opaque) {
    case 0:
        value = gov.windows;
        break;
    case 1:
        value = gov.governed_ns / 1000000;
        break;
    case 2:
        value = gov.insns;
        break;
    case 3:
        value = gov.slept_ns / 1000000;
        break;
    case 4:
        value = gov.capped;
        break;
    default:
        value = gov.ignored;
        break;
    }
    visit_type_uint64(v, name, &value, errp);
}

static void heathrow_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    FWPathProviderClass *fwc = FW_PATH_PROVIDER_CLASS(oc);

    mc->desc = "Heathrow based PowerMac";
    mc->init = ppc_heathrow_init;
    mc->block_default_type = IF_IDE;
    /* SMP is not supported currently */
    mc->max_cpus = 1;
#ifndef TARGET_PPC64
    mc->is_default = true;
#endif
    /* TOFIX "cad" when Mac floppy is implemented */
    mc->default_boot_order = "cd";
    mc->kvm_type = heathrow_kvm_type;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("750_v2.2");
    mc->ignore_boot_device_suffixes = true;
    mc->default_ram_id = "ppc_heathrow.ram";
    /*
     * Real Beige G3 ROM boot-time Memory Manager init reads a low-memory
     * heap/zone-size global that is derived from installed RAM; below a
     * certain threshold this negotiation tends to come up short compared
     * to a real 256 MiB-equipped machine, and a later stage's linked-list
     * resource walk (Slot Manager / device list) terminates early,
     * leaving the stack in a state that can lead to a wild jump into RAM
     * data during the escc self-test dispatch. QEMU's generic 128 MiB
     * default (hw/core/machine.c) makes this the only observed outcome;
     * matching the real hardware's common 256 MiB configuration (also
     * DingusPPC's default for this same ROM) makes it much less likely,
     * though boot timing is not fully deterministic here (VIA/CUDA
     * real-time counters), so this mitigates rather than guarantees.
     */
    mc->default_ram_size = 256 * MiB;
    fwc->get_dev_path = heathrow_fw_dev_path;

    /*
     * This machine's ROM calibrates its CPU-speed constants by spinning
     * a pure register loop inside a ~1ms VIA T2 one-shot; unthrottled
     * TCG finishes the loop first and stores zero, which later divides
     * by zero in the ATAPI driver and Open Transport. The governor
     * paces the vCPU only while such a one-shot is armed. On by default
     * because the failure it prevents is a boot-blocker, not a nicety.
     */
    object_class_property_add_str(oc, "calibration-governor",
                                  heathrow_get_calib_governor,
                                  heathrow_set_calib_governor);
    object_class_property_set_description(oc, "calibration-governor",
        "Pace the CPU while a VIA T2 one-shot calibration window is open: "
        "'on' (the default, 100 MIPS), 'off', or 'mips=<n>'");

    object_class_property_add(oc, "calibration-governor-windows", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)0);
    object_class_property_add(oc, "calibration-governor-governed-ms", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)1);
    object_class_property_add(oc, "calibration-governor-insns", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)2);
    object_class_property_add(oc, "calibration-governor-slept-ms", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)3);
    object_class_property_add(oc, "calibration-governor-capped", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)4);
    object_class_property_add(oc, "calibration-governor-ignored", "uint64",
                              heathrow_get_gov_stat, NULL, NULL,
                              (void *)(uintptr_t)5);
}

static const TypeInfo ppc_heathrow_machine_info = {
    .name          = MACHINE_TYPE_NAME("g3beige"),
    .parent        = TYPE_MACHINE,
    .class_init    = heathrow_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_FW_PATH_PROVIDER },
        { }
    },
};

static void ppc_heathrow_register_types(void)
{
    type_register_static(&ppc_heathrow_machine_info);
}

type_init(ppc_heathrow_register_types);
