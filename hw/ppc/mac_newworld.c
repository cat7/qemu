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
#include "hw/ppc/mac_newworld_pm34.h"
#include "hw/ppc/mac_newworld_pm36.h"
#include "hw/ppc/openpic.h"
#include "hw/core/loader.h"
#include "hw/core/fw-path-provider.h"
#include "elf.h"
#include "qemu/error-report.h"
#include <zlib.h>                /* adler32() for the CHRP nvram outer checksum */
#include "system/kvm.h"
#include "system/reset.h"
#include "kvm_ppc.h"
#include "hw/usb/usb.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "trace.h"

#define MAX_IDE_BUS 3
#define CFG_ADDR 0xf0000510

#define NDRV_VGA_FILENAME "qemu_vga.ndrv"

#define PROM_FILENAME "openbios-ppc"
#define PROM_BASE 0xfff00000

static uint64_t core99_rom_read(void *opaque, hwaddr addr, unsigned size)
{
    /* never called: reads are served from the ROM device's RAM backing */
    return 0;
}

static void core99_rom_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    trace_mac99_rom_write(addr, size, val);
}

static const MemoryRegionOps core99_rom_ops = {
    .read = core99_rom_read,
    .write = core99_rom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
};
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
#define ROM_WINDOW_BASE 0xff000000
#define ROM_WINDOW_SIZE (16 * MiB)

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

typedef enum {
    CORE99_MODEL_PM34 = 0,
    CORE99_MODEL_PM36
} Core99Model;

struct Core99MachineState {
    /*< private >*/
    MachineState parent;

    Core99ViaConfig via_config;
    Core99Model model;
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
     * XNU's secondary-CPU start protocol (osfmk/ppc/cpu.c cpu_start(),
     * unchanged across the PowerPC era -- decoded from the 10.2 kernel's
     * own disassembly and verified live on a 10.4 boot): the kernel
     * installs a ResetHandler at physical 0x100 that reads a 3-word start
     * block in low memory --
     *
     *   0xF0: type       1 = start request (2/3 = other reset kinds)
     *   0xF4: entry      physical address to jump to
     *   0xF8: argument    per_proc pointer, wanted in r3
     *
     * -- clears the type word, and branches to [0xF4] with r3 = [0xF8],
     * translation off. Whoever answers the physical reset is expected to
     * enter that handler at 0x100.
     *
     * The catch is timing: the kernel pulses this soft-reset line as an
     * assert/deassert pair, and the FIRST release edge fires before it
     * has written the block (observed live: block still 0 on the first
     * kick, valid 1/entry on the next). So a release with no valid block
     * yet is not a signal to run -- it means "not ready, keep waiting."
     * Leave the CPU halted on that edge; the real start edge, once the
     * block is populated, releases it into the kernel's own ResetHandler
     * at 0x100, which then does the [0xF4]/r3 dispatch itself.
     *
     * (Re-entering the ROM reset vector at kernel time, as this function
     * used to, dead-ends every way: the hard-reset path trips UniNorth's
     * HWINIT_STATE sanity check and hangs at a `b .`, and the soft-reset
     * path lands in the ROM's SCC serial command monitor no kernel talks
     * to -- both observed live.)
     */
    if (ldl_phys(cs->as, 0xf0) != 1 || ldl_phys(cs->as, 0xf4) == 0) {
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
     * Enter at the low-memory reset vector with translation off and
     * exceptions vectored low (excp_prefix 0, i.e. MSR[IP]=0): the kernel
     * has installed both its ResetHandler at 0x100 and its real exception
     * handlers at the low vectors (0x300 DSI, 0x400 ISI, ...), and
     * _start_cpu takes faults before its MMU is up. Leaving excp_prefix
     * at the ROM alias a previous reset/probe left behind sent those
     * early faults into the ROM's SCC monitor instead (observed live:
     * CPU1 dispatched correctly but immediately bounced to 0xfff0ad6c).
     */
    cpu->env.excp_prefix = 0;
    cpu->env.nip = 0x100;
    cpu->env.msr = 0;

    cs->halted = 0;
    cs->exception_index = -1;
    qemu_cpu_kick(cs);
}

typedef struct CpuProbeWakeState {
    PowerPCCPU *cpu;
    QEMUTimer *rehalt_timer;
} CpuProbeWakeState;

static void cpu_probe_wake_rehalt(void *opaque)
{
    CPUState *cs = opaque;

    /*
     * Force the probed CPU back to halted regardless of what the ROM's
     * trampoline (see cpu_probe_wake() below) went on to do -- it has no
     * business still running once the real ROM's own 64us probe window
     * (see cpu-probe? in the ROM) has elapsed.
     */
    cs->halted = 1;
    qemu_cpu_kick(cs);
}

/*
 * Called when the KeyLargo GPIO line the real Apple ROM's cpu-probe? word
 * pulses low is toggled (see gpio.c KL_GPIO_CPU_PROBE handling; the exact
 * address is inferred from the ROM's own code, not from Apple/Linux
 * documentation). Real hardware presumably has some way for a genuinely
 * power-on-but-parked secondary CPU to answer this probe without racing
 * the primary CPU's own concurrent, un-arbitrated cold-init ROM code --
 * two independent races were found running that shared path on both CPUs
 * at once (Keywest I2C, and a callback/vector-table setup routine).
 *
 * Two things were tried and rejected before this:
 *
 * 1) Jump straight to the trampoline the ROM has already copied to
 *    physical 0x500 (see the cpu-probe? decompile:
 *    "cpu-vector 500 h#10 move"), leaving SRR0 untouched. This avoids
 *    both known races (verified live: the CPU parks in a clean, stable,
 *    non-oscillating wait loop, not either race's address) and runs
 *    genuinely correct ROM code -- but cpu-probe? still always saw a
 *    false ack, because the trampoline's first action is
 *    `mfspr r0, SRR0; stw r0, 0x10(cpu-info)`: SRR0 itself *is* the ack
 *    value, and a raw NIP poke leaves it at its power-on value of zero.
 *
 * 2) Model this as a real HID0[NHR] soft-reset exception (see the
 *    MPC7410 manual's system-reset chapter) into the ROM's own reset
 *    vector, so SRR0 lands on the CPU's pre-reset NIP the way real
 *    hardware's SRESET would. This reaches the real soft-reset dispatch
 *    correctly (confirmed: HID0 reads back with NHR set, and the CPU
 *    heads toward the trampoline via the ROM's own code) but the shared
 *    prologue every reset runs through first (`bl 0xfff0060c`, common to
 *    both hard- and soft-reset) checks UniNorth HWINIT_STATE and hangs
 *    (`b .` at 0xfff03600) whenever it's already progressed past its own
 *    early boot phase -- true by the time anything triggers this pulse
 *    (confirmed live: HWINIT_STATE reads 0x2, its steady-state value,
 *    well under a second after boot starts).
 *
 * This combines what worked from each: jump straight to the trampoline
 * (skips the HWINIT_STATE-sensitive shared prologue entirely, same as
 * attempt 1) but with SRR0 forced nonzero first (fixes attempt 1's
 * always-false ack, without needing the real reset-vector plumbing
 * attempt 2 went through to get there). Force the CPU back to halted
 * shortly after either way -- nothing here can safely let it keep
 * running the ROM's shared init state indefinitely.
 */
static void cpu_probe_wake(void *opaque, int n, int level)
{
    CpuProbeWakeState *pw = opaque;
    PowerPCCPU *cpu = pw->cpu;
    CPUState *cs = CPU(cpu);

    if (level || !cs->halted) {
        return;
    }

    cpu->env.spr[SPR_SRR0] = 0x500;
    cpu->env.nip = 0x500;
    cpu->env.msr = 0;
    cs->halted = 0;
    cs->exception_index = -1;
    qemu_cpu_kick(cs);

    /*
     * The ack write (SRR0 -> cpu-info+0x10) happens within the trampoline's
     * first ~10 instructions, so this only needs to outlast that, not the
     * real ROM's full 64us probe window. 500us gives generous headroom
     * over both without leaving the CPU free-running through the rest of
     * the shared init state for long.
     */
    timer_mod(pw->rehalt_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000);
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

/*
 * Dispatch to the matching per-model file (mac_newworld_pm34.c /
 * mac_newworld_pm36.c) for whatever piece of setup needs to match real
 * hardware for the selected model.
 */
static uint32_t core99_tbfreq(Core99Model model)
{
    return model == CORE99_MODEL_PM36 ? pm36_tbfreq() : pm34_tbfreq();
}

static uint32_t core99_clockfreq(Core99Model model)
{
    return model == CORE99_MODEL_PM36 ? pm36_clockfreq() : pm34_clockfreq();
}

static uint32_t core99_busfreq(Core99Model model)
{
    return model == CORE99_MODEL_PM36 ? pm36_busfreq() : pm34_busfreq();
}

static void core99_cpu_defaults(Core99Model model, PowerPCCPU *cpu)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_cpu_defaults(cpu);
    } else {
        pm34_cpu_defaults(cpu);
    }
}

static void core99_add_config_eeprom(Core99Model model, I2CBus *bus)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_add_config_eeprom(bus);
    } else {
        pm34_add_config_eeprom(bus);
    }
}

static void core99_add_spd_dimms(Core99Model model, I2CBus *bus,
                                 uint64_t ram_size)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_add_spd_dimms(bus, ram_size);
    } else {
        pm34_add_spd_dimms(bus, ram_size);
    }
}

static void core99_place_gmac(Core99Model model, PCIBus *internal_bus,
                              const char *default_nic)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_place_gmac(internal_bus, default_nic);
    } else {
        pm34_place_gmac(internal_bus, default_nic);
    }
}

static void core99_internal_bus_irq_map(Core99Model model,
                                        DeviceState *uninorth_internal_dev,
                                        DeviceState *pic_dev)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_internal_bus_irq_map(uninorth_internal_dev, pic_dev);
    } else {
        pm34_internal_bus_irq_map(uninorth_internal_dev, pic_dev);
    }
}

static void core99_pci_irq_map(Core99Model model,
                               DeviceState *uninorth_pci_dev,
                               DeviceState *pic_dev)
{
    if (model == CORE99_MODEL_PM36) {
        pm36_pci_irq_map(uninorth_pci_dev, pic_dev);
    } else {
        pm34_pci_irq_map(uninorth_pci_dev, pic_dev);
    }
}

static int core99_macio_devfn(Core99Model model)
{
    return model == CORE99_MODEL_PM36 ? pm36_macio_devfn() : pm34_macio_devfn();
}

static int core99_usb_devfn(Core99Model model)
{
    return model == CORE99_MODEL_PM36 ? pm36_usb_devfn() : pm34_usb_devfn();
}

/* CHRP nvram partition-header checksum (see include/hw/nvram/chrp_nvram.h). */
static uint8_t core99_chrp_cksum(const uint8_t *hdr)
{
    unsigned int sum = hdr[0];
    int i;

    for (i = 0; i < 14; i++) {
        sum += hdr[2 + i];
        sum = (sum + ((sum & 0xff00) >> 8)) & 0xff;
    }
    return sum & 0xff;
}

/*
 * Make the built-in gmac usable under Mac OS by publishing its MAC on the
 * Open Firmware ethernet node, and graft the missing L2-cache device-tree
 * data our ROM never produces so Mac OS stops reporting a cache fault.
 *
 * Mac OS (classic AppleTalk/OT and OS X's AppleGMACEthernet) reads the
 * Ethernet MAC solely from the OF "local-mac-address" property and will not
 * create en0 without it; a "built-in" property additionally makes it show as
 * "Built-in Ethernet" rather than a PCI slot. The real Apple ROM's own gmac
 * driver never publishes these -- its (open) aborts on an internal ALLOC-MEM
 * cap before it gets there -- so we inject them ourselves via the OF
 * "boot-command" variable, honouring whatever MAC the gmac was given on the
 * command line (or QEMU's default when none was specified).
 *
 * A real PowerMac3,4 publishes /cpus/PowerPC,G4@0/l2cr = 0xb9000000 and a
 * child "l2-cache" node (1MB unified, 64-byte lines, 0x2000 sets, 233MHz --
 * verbatim from a real lsprop dump). Our 7450-family L2CR write path was
 * already fixed (commit 33231ae62a), but the ROM's own L2 bring-up/POST
 * still never runs on QEMU's transparent memory model, so the device tree
 * ends up with neither property and /diagnostics/post-results (the POST
 * failure bitmask a real healthy Mac reads back as 0x00000000) is left
 * non-zero -- which is what Mac OS surfaces as "a problem with cache
 * memory". Graft both: the l2cr/l2-cache data a real machine has, and a
 * zeroed post-results, so Mac OS sees exactly what a passing POST would
 * have produced.
 *
 * boot-command (not nvramrc) is used deliberately: nvramrc runs before
 * probe-all, when /pci@f4000000/ethernet@f does not yet exist; boot-command
 * runs at the end of start-up, after the PCI ethernet node has been probed
 * and after /diagnostics/post-results has already been written by the ROM's
 * own (failing) POST, so it is the right point to overwrite both.
 *
 * The nvram is Apple/CHRP format: an 8KB image mirrored inside the 16KB
 * flash, OF variables stored as name=value NUL strings in the "common"
 * partition. Every edit must fix both the per-partition header checksum and
 * the outer Adler32 (at copy+0x10, over [copy+0x14 : copy+0x2000]); without
 * the latter the ROM rejects the image ("NVRAM corrupted") and wipes it.
 */
/*
 * A blank/never-written flash NVRAM (all bytes 0xff, matching real erased-
 * flash semantics) has no valid CHRP partition structure at all for
 * core99_nvram_graft()/core99_nvram_set_bootcmd() to patch into -- their
 * search loop for the "common" (0x70) partition finds nothing and silently
 * no-ops. The real Apple ROM detects this as "NVRAM corrupted" and rebuilds
 * its own default structure, but only in its own in-guest working copy;
 * that repair is never observed to persist back to the emulated flash's
 * backing file (confirmed live: even a full, successful boot leaves a
 * freshly-created backing file completely unchanged). Net effect: every
 * boot-command graft (gmac MAC, FireWire GUID, cache-POST) silently never
 * applies on a genuinely fresh nvram-flash-*.img, only on one a real ROM
 * has already formatted at some point in the past.
 *
 * Seed a real, byte-for-byte, ROM-validated default structure instead of
 * hand-constructing one -- extracted directly from a real PowerMac3,4
 * nvram-flash-34.img's second 8KB copy (the first copy in that same real
 * file is itself blank/unused, matching typical flash wear-leveling; only
 * one live copy is needed for the ROM's own scan to find). This is the
 * exact same "copy a real captured byte blob rather than reconstruct the
 * format from a spec" approach already used for the Old World OF partition
 * template above (see oldworld_of_partition_head/_strings) -- safer than
 * hand-rolling the "system" (0x5f, Apple-specific, not the generic CHRP
 * tag) and "APL,MacOS75" partitions this real capture also contains, which
 * this code has no need to understand the contents of, only to reproduce
 * faithfully enough that the real ROM's own validation accepts the whole
 * copy as uncorrupted.
 */
#define CORE99_NVRAM_TEMPLATE_SIZE 0x2000
static const uint8_t core99_nvram_flash_template[CORE99_NVRAM_TEMPLATE_SIZE] = {
    [0]=0x5a, [1]=0x82, [3]=0x02, [4]=0x6e, [5]=0x76, [6]=0x72, [7]=0x61, [8]=0x6d,
    [16]=0x7c, [17]=0x26, [18]=0x12, [19]=0x0a, [32]=0x5f, [33]=0x25, [35]=0x1e, [36]=0x73,
    [37]=0x79, [38]=0x73, [39]=0x74, [40]=0x65, [41]=0x6d, [512]=0x70, [513]=0xfd, [514]=0x01,
    [515]=0x01, [516]=0x63, [517]=0x6f, [518]=0x6d, [519]=0x6d, [520]=0x6f, [521]=0x6e, [528]=0x6c,
    [529]=0x69, [530]=0x74, [531]=0x74, [532]=0x6c, [533]=0x65, [534]=0x2d, [535]=0x65, [536]=0x6e,
    [537]=0x64, [538]=0x69, [539]=0x61, [540]=0x6e, [541]=0x3f, [542]=0x3d, [543]=0x66, [544]=0x61,
    [545]=0x6c, [546]=0x73, [547]=0x65, [549]=0x72, [550]=0x65, [551]=0x61, [552]=0x6c, [553]=0x2d,
    [554]=0x6d, [555]=0x6f, [556]=0x64, [557]=0x65, [558]=0x3f, [559]=0x3d, [560]=0x66, [561]=0x61,
    [562]=0x6c, [563]=0x73, [564]=0x65, [566]=0x61, [567]=0x75, [568]=0x74, [569]=0x6f, [570]=0x2d,
    [571]=0x62, [572]=0x6f, [573]=0x6f, [574]=0x74, [575]=0x3f, [576]=0x3d, [577]=0x74, [578]=0x72,
    [579]=0x75, [580]=0x65, [582]=0x64, [583]=0x69, [584]=0x61, [585]=0x67, [586]=0x2d, [587]=0x73,
    [588]=0x77, [589]=0x69, [590]=0x74, [591]=0x63, [592]=0x68, [593]=0x3f, [594]=0x3d, [595]=0x66,
    [596]=0x61, [597]=0x6c, [598]=0x73, [599]=0x65, [601]=0x66, [602]=0x63, [603]=0x6f, [604]=0x64,
    [605]=0x65, [606]=0x2d, [607]=0x64, [608]=0x65, [609]=0x62, [610]=0x75, [611]=0x67, [612]=0x3f,
    [613]=0x3d, [614]=0x66, [615]=0x61, [616]=0x6c, [617]=0x73, [618]=0x65, [620]=0x6f, [621]=0x65,
    [622]=0x6d, [623]=0x2d, [624]=0x62, [625]=0x61, [626]=0x6e, [627]=0x6e, [628]=0x65, [629]=0x72,
    [630]=0x3f, [631]=0x3d, [632]=0x66, [633]=0x61, [634]=0x6c, [635]=0x73, [636]=0x65, [638]=0x6f,
    [639]=0x65, [640]=0x6d, [641]=0x2d, [642]=0x6c, [643]=0x6f, [644]=0x67, [645]=0x6f, [646]=0x3f,
    [647]=0x3d, [648]=0x66, [649]=0x61, [650]=0x6c, [651]=0x73, [652]=0x65, [654]=0x75, [655]=0x73,
    [656]=0x65, [657]=0x2d, [658]=0x6e, [659]=0x76, [660]=0x72, [661]=0x61, [662]=0x6d, [663]=0x72,
    [664]=0x63, [665]=0x3f, [666]=0x3d, [667]=0x66, [668]=0x61, [669]=0x6c, [670]=0x73, [671]=0x65,
    [673]=0x75, [674]=0x73, [675]=0x65, [676]=0x2d, [677]=0x67, [678]=0x65, [679]=0x6e, [680]=0x65,
    [681]=0x72, [682]=0x69, [683]=0x63, [684]=0x3f, [685]=0x3d, [686]=0x66, [687]=0x61, [688]=0x6c,
    [689]=0x73, [690]=0x65, [692]=0x64, [693]=0x65, [694]=0x66, [695]=0x61, [696]=0x75, [697]=0x6c,
    [698]=0x74, [699]=0x2d, [700]=0x6d, [701]=0x61, [702]=0x63, [703]=0x2d, [704]=0x61, [705]=0x64,
    [706]=0x64, [707]=0x72, [708]=0x65, [709]=0x73, [710]=0x73, [711]=0x3f, [712]=0x3d, [713]=0x66,
    [714]=0x61, [715]=0x6c, [716]=0x73, [717]=0x65, [719]=0x72, [720]=0x65, [721]=0x61, [722]=0x6c,
    [723]=0x2d, [724]=0x62, [725]=0x61, [726]=0x73, [727]=0x65, [728]=0x3d, [729]=0x2d, [730]=0x31,
    [732]=0x72, [733]=0x65, [734]=0x61, [735]=0x6c, [736]=0x2d, [737]=0x73, [738]=0x69, [739]=0x7a,
    [740]=0x65, [741]=0x3d, [742]=0x2d, [743]=0x31, [745]=0x6c, [746]=0x6f, [747]=0x61, [748]=0x64,
    [749]=0x2d, [750]=0x62, [751]=0x61, [752]=0x73, [753]=0x65, [754]=0x3d, [755]=0x30, [756]=0x78,
    [757]=0x38, [758]=0x30, [759]=0x30, [760]=0x30, [761]=0x30, [762]=0x30, [764]=0x76, [765]=0x69,
    [766]=0x72, [767]=0x74, [768]=0x2d, [769]=0x62, [770]=0x61, [771]=0x73, [772]=0x65, [773]=0x3d,
    [774]=0x2d, [775]=0x31, [777]=0x76, [778]=0x69, [779]=0x72, [780]=0x74, [781]=0x2d, [782]=0x73,
    [783]=0x69, [784]=0x7a, [785]=0x65, [786]=0x3d, [787]=0x2d, [788]=0x31, [790]=0x70, [791]=0x63,
    [792]=0x69, [793]=0x2d, [794]=0x70, [795]=0x72, [796]=0x6f, [797]=0x62, [798]=0x65, [799]=0x2d,
    [800]=0x6d, [801]=0x61, [802]=0x73, [803]=0x6b, [804]=0x3d, [805]=0x2d, [806]=0x31, [808]=0x73,
    [809]=0x63, [810]=0x72, [811]=0x65, [812]=0x65, [813]=0x6e, [814]=0x2d, [815]=0x23, [816]=0x63,
    [817]=0x6f, [818]=0x6c, [819]=0x75, [820]=0x6d, [821]=0x6e, [822]=0x73, [823]=0x3d, [824]=0x31,
    [825]=0x30, [826]=0x30, [828]=0x73, [829]=0x63, [830]=0x72, [831]=0x65, [832]=0x65, [833]=0x6e,
    [834]=0x2d, [835]=0x23, [836]=0x72, [837]=0x6f, [838]=0x77, [839]=0x73, [840]=0x3d, [841]=0x34,
    [842]=0x30, [844]=0x73, [845]=0x65, [846]=0x6c, [847]=0x66, [848]=0x74, [849]=0x65, [850]=0x73,
    [851]=0x74, [852]=0x2d, [853]=0x23, [854]=0x6d, [855]=0x65, [856]=0x67, [857]=0x73, [858]=0x3d,
    [859]=0x30, [861]=0x62, [862]=0x6f, [863]=0x6f, [864]=0x74, [865]=0x2d, [866]=0x64, [867]=0x65,
    [868]=0x76, [869]=0x69, [870]=0x63, [871]=0x65, [872]=0x3d, [873]=0x68, [874]=0x64, [875]=0x3a,
    [876]=0x2c, [877]=0x5c, [878]=0x5c, [879]=0x3a, [880]=0x74, [881]=0x62, [882]=0x78, [883]=0x69,
    [885]=0x62, [886]=0x6f, [887]=0x6f, [888]=0x74, [889]=0x2d, [890]=0x66, [891]=0x69, [892]=0x6c,
    [893]=0x65, [894]=0x3d, [896]=0x62, [897]=0x6f, [898]=0x6f, [899]=0x74, [900]=0x2d, [901]=0x73,
    [902]=0x63, [903]=0x72, [904]=0x65, [905]=0x65, [906]=0x6e, [907]=0x3d, [909]=0x63, [910]=0x6f,
    [911]=0x6e, [912]=0x73, [913]=0x6f, [914]=0x6c, [915]=0x65, [916]=0x2d, [917]=0x73, [918]=0x63,
    [919]=0x72, [920]=0x65, [921]=0x65, [922]=0x6e, [923]=0x3d, [925]=0x64, [926]=0x69, [927]=0x61,
    [928]=0x67, [929]=0x2d, [930]=0x64, [931]=0x65, [932]=0x76, [933]=0x69, [934]=0x63, [935]=0x65,
    [936]=0x3d, [937]=0x65, [938]=0x6e, [939]=0x65, [940]=0x74, [942]=0x64, [943]=0x69, [944]=0x61,
    [945]=0x67, [946]=0x2d, [947]=0x66, [948]=0x69, [949]=0x6c, [950]=0x65, [951]=0x3d, [952]=0x2c,
    [953]=0x64, [954]=0x69, [955]=0x61, [956]=0x67, [957]=0x73, [959]=0x69, [960]=0x6e, [961]=0x70,
    [962]=0x75, [963]=0x74, [964]=0x2d, [965]=0x64, [966]=0x65, [967]=0x76, [968]=0x69, [969]=0x63,
    [970]=0x65, [971]=0x3d, [972]=0x6b, [973]=0x65, [974]=0x79, [975]=0x62, [976]=0x6f, [977]=0x61,
    [978]=0x72, [979]=0x64, [981]=0x6f, [982]=0x75, [983]=0x74, [984]=0x70, [985]=0x75, [986]=0x74,
    [987]=0x2d, [988]=0x64, [989]=0x65, [990]=0x76, [991]=0x69, [992]=0x63, [993]=0x65, [994]=0x3d,
    [995]=0x73, [996]=0x63, [997]=0x72, [998]=0x65, [999]=0x65, [1000]=0x6e, [1002]=0x69, [1003]=0x6e,
    [1004]=0x70, [1005]=0x75, [1006]=0x74, [1007]=0x2d, [1008]=0x64, [1009]=0x65, [1010]=0x76, [1011]=0x69,
    [1012]=0x63, [1013]=0x65, [1014]=0x2d, [1015]=0x31, [1016]=0x3d, [1017]=0x73, [1018]=0x63, [1019]=0x63,
    [1020]=0x61, [1022]=0x6f, [1023]=0x75, [1024]=0x74, [1025]=0x70, [1026]=0x75, [1027]=0x74, [1028]=0x2d,
    [1029]=0x64, [1030]=0x65, [1031]=0x76, [1032]=0x69, [1033]=0x63, [1034]=0x65, [1035]=0x2d, [1036]=0x31,
    [1037]=0x3d, [1038]=0x73, [1039]=0x63, [1040]=0x63, [1041]=0x61, [1043]=0x6d, [1044]=0x6f, [1045]=0x75,
    [1046]=0x73, [1047]=0x65, [1048]=0x2d, [1049]=0x64, [1050]=0x65, [1051]=0x76, [1052]=0x69, [1053]=0x63,
    [1054]=0x65, [1055]=0x3d, [1056]=0x6d, [1057]=0x6f, [1058]=0x75, [1059]=0x73, [1060]=0x65, [1062]=0x6f,
    [1063]=0x65, [1064]=0x6d, [1065]=0x2d, [1066]=0x62, [1067]=0x61, [1068]=0x6e, [1069]=0x6e, [1070]=0x65,
    [1071]=0x72, [1072]=0x3d, [1074]=0x6f, [1075]=0x65, [1076]=0x6d, [1077]=0x2d, [1078]=0x6c, [1079]=0x6f,
    [1080]=0x67, [1081]=0x6f, [1082]=0x3d, [1084]=0x6e, [1085]=0x76, [1086]=0x72, [1087]=0x61, [1088]=0x6d,
    [1089]=0x72, [1090]=0x63, [1091]=0x3d, [1093]=0x62, [1094]=0x6f, [1095]=0x6f, [1096]=0x74, [1097]=0x2d,
    [1098]=0x63, [1099]=0x6f, [1100]=0x6d, [1101]=0x6d, [1102]=0x61, [1103]=0x6e, [1104]=0x64, [1105]=0x3d,
    [1106]=0x6d, [1107]=0x61, [1108]=0x63, [1109]=0x2d, [1110]=0x62, [1111]=0x6f, [1112]=0x6f, [1113]=0x74,
    [1115]=0x64, [1116]=0x65, [1117]=0x66, [1118]=0x61, [1119]=0x75, [1120]=0x6c, [1121]=0x74, [1122]=0x2d,
    [1123]=0x63, [1124]=0x6c, [1125]=0x69, [1126]=0x65, [1127]=0x6e, [1128]=0x74, [1129]=0x2d, [1130]=0x69,
    [1131]=0x70, [1132]=0x3d, [1134]=0x64, [1135]=0x65, [1136]=0x66, [1137]=0x61, [1138]=0x75, [1139]=0x6c,
    [1140]=0x74, [1141]=0x2d, [1142]=0x73, [1143]=0x65, [1144]=0x72, [1145]=0x76, [1146]=0x65, [1147]=0x72,
    [1148]=0x2d, [1149]=0x69, [1150]=0x70, [1151]=0x3d, [1153]=0x64, [1154]=0x65, [1155]=0x66, [1156]=0x61,
    [1157]=0x75, [1158]=0x6c, [1159]=0x74, [1160]=0x2d, [1161]=0x67, [1162]=0x61, [1163]=0x74, [1164]=0x65,
    [1165]=0x77, [1166]=0x61, [1167]=0x79, [1168]=0x2d, [1169]=0x69, [1170]=0x70, [1171]=0x3d, [1173]=0x64,
    [1174]=0x65, [1175]=0x66, [1176]=0x61, [1177]=0x75, [1178]=0x6c, [1179]=0x74, [1180]=0x2d, [1181]=0x73,
    [1182]=0x75, [1183]=0x62, [1184]=0x6e, [1185]=0x65, [1186]=0x74, [1187]=0x2d, [1188]=0x6d, [1189]=0x61,
    [1190]=0x73, [1191]=0x6b, [1192]=0x3d, [1194]=0x64, [1195]=0x65, [1196]=0x66, [1197]=0x61, [1198]=0x75,
    [1199]=0x6c, [1200]=0x74, [1201]=0x2d, [1202]=0x72, [1203]=0x6f, [1204]=0x75, [1205]=0x74, [1206]=0x65,
    [1207]=0x72, [1208]=0x2d, [1209]=0x69, [1210]=0x70, [1211]=0x3d, [1213]=0x62, [1214]=0x6f, [1215]=0x6f,
    [1216]=0x74, [1217]=0x2d, [1218]=0x73, [1219]=0x63, [1220]=0x72, [1221]=0x69, [1222]=0x70, [1223]=0x74,
    [1224]=0x3d, [1226]=0x61, [1227]=0x61, [1228]=0x70, [1229]=0x6c, [1230]=0x2c, [1231]=0x70, [1232]=0x63,
    [1233]=0x69, [1234]=0x3d, [1236]=0x72, [1237]=0x61, [1238]=0x6d, [1239]=0x2d, [1240]=0x73, [1241]=0x69,
    [1242]=0x7a, [1243]=0x65, [1244]=0x3d, [1245]=0x30, [1246]=0x78, [1247]=0x34, [1248]=0x30, [1249]=0x30,
    [1250]=0x30, [1251]=0x30, [1252]=0x30, [1253]=0x30, [1254]=0x30, [4624]=0xa0, [4625]=0x1d, [4627]=0x51,
    [4628]=0x41, [4629]=0x50, [4630]=0x4c, [4631]=0x2c, [4632]=0x4d, [4633]=0x61, [4634]=0x63, [4635]=0x4f,
    [4636]=0x53, [4637]=0x37, [4638]=0x35, [5920]=0x7f, [5921]=0xa7, [5923]=0x8e, [5924]=0x77, [5925]=0x77,
    [5926]=0x77, [5927]=0x77, [5928]=0x77, [5929]=0x77, [5930]=0x77, [5931]=0x77, [5932]=0x77, [5933]=0x77,
    [5934]=0x77, [5935]=0x77,
};

/*
 * True if the flash is genuinely blank (all-0xff, real erased-flash state --
 * see macio_nvram_default_blk()'s own "must start out erased" comment).
 * Deliberately not macio_nvram_is_blank(), which checks for all-*zero*
 * bytes -- the correct blank state for the *non*-flash nvram.img path this
 * function is not used for.
 */
static bool core99_nvram_flash_is_blank(const uint8_t *data, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size; i++) {
        if (data[i] != 0xff) {
            return false;
        }
    }
    return true;
}

/*
 * Seed the real default structure into a blank flash nvram so the grafts
 * below always have a valid "common" partition to patch into, even on the
 * very first boot against a brand-new nvram-flash-*.img.
 */
static void core99_nvram_seed_if_blank(uint8_t *data, uint32_t size)
{
    if (size < 2 * CORE99_NVRAM_TEMPLATE_SIZE ||
        !core99_nvram_flash_is_blank(data, size)) {
        return;
    }
    memcpy(&data[CORE99_NVRAM_TEMPLATE_SIZE], core99_nvram_flash_template,
          CORE99_NVRAM_TEMPLATE_SIZE);
}

static void core99_nvram_set_bootcmd(uint8_t *data, uint32_t size,
                                     const uint8_t *mac, const uint8_t *guid)
{
    const uint32_t COPY = 0x2000;
    /*
     * gmac graft (only when a MAC is supplied, i.e. a sungem is present):
     * publishes local-mac-address + "built-in" so Mac OS creates en0.
     */
    g_autofree char *gmac = mac ? g_strdup_printf(
        "dev /pci@f4000000/ethernet@f "
        "here h# %02x over c! 1+ h# %02x over c! 1+ h# %02x over c! 1+ "
        "h# %02x over c! 1+ h# %02x over c! 1+ h# %02x swap c! "
        "here 6 encode-bytes \" local-mac-address\" property "
        "0 0 encode-bytes \" built-in\" property device-end ",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]) : g_strdup("");
    /*
     * FireWire GUID graft (only when a GUID is supplied, PM34 only):
     * publishes an 8-byte GUID property on the built-in firewire@e node.
     * The real ROM's own vendor-specific bring-up for the real 11c1:5811
     * identity crashes (a CPU/MMU translation gap in the ROM's own code,
     * unfixable device-side) before it ever reaches a config-ROM/GUID
     * readback, and no separate GUID/serial EEPROM exists on the real
     * board to model a read protocol against in the first place --
     * confirmed against the real PowerMac3,4 schematic, which shows the
     * FireWire link integrated into UniNorth 1.5 with only a PHY (no GUID
     * EEPROM) as a separate part. So this is grafted directly, the same
     * way local-mac-address is above; see mac99 gmac/firewire
     * investigation memory.
     */
    g_autofree char *fwguid = guid ? g_strdup_printf(
        "dev /pci@f4000000/firewire@e "
        "here h# %02x over c! 1+ h# %02x over c! 1+ h# %02x over c! 1+ "
        "h# %02x over c! 1+ h# %02x over c! 1+ h# %02x over c! 1+ "
        "h# %02x over c! 1+ h# %02x swap c! "
        "here 8 encode-bytes \" GUID\" property device-end ",
        guid[0], guid[1], guid[2], guid[3], guid[4], guid[5], guid[6],
        guid[7]) : g_strdup("");
    /*
     * AGP-slot enable graft (PM34 only): PowerMac3,4 always shipped with an
     * AGP graphics card, but this ROM's native PCI config-space access code
     * is hardcoded to the main bus only -- a Ghidra disassembly of the whole
     * 1MB image found literal references to the main bus's config-space
     * MMIO addresses at 4 sites, and zero anywhere to the AGP or internal
     * bus's -- so nothing ever flips PCI_COMMAND's memory-space-enable bit
     * for a card sitting on the AGP bus. The generic BAR-sizing walk still
     * runs and assigns real addresses (device-identity-independent,
     * confirmed with an unrelated rtl8139 in the same slot), it just never
     * gets turned on. See mac99 AGP investigation memory for the full trail.
     *
     * Set the bit ourselves, operating purely from the AGP host bridge's own
     * node (always present, unconditionally created for every PM34 machine)
     * via its generic config-b@/config-b! methods with an explicit config
     * address -- device 0x10 (16) is the real AGP slot's device number on
     * this bus (confirmed via the disassembly-derived `unin_get_config_reg`
     * geographic-addressing scheme), offset 4 is PCI_COMMAND.
     *
     * Deliberately does NOT navigate to the card's own auto-generated child
     * node by path (`dev /pci@.../pci1af4,1100@10`, needed for e.g.
     * `my-space`-relative access) -- `my-space` is an INSTANCE word that
     * plain `dev` doesn't open (confirmed live: throws "no current
     * instance"), and wrapping a `dev <path>` in a colon-definition +
     * `catch` to make a possibly-absent child safe to reference doesn't
     * work either: `dev` is a PARSING word that reads its path from the
     * current input stream at *run* time, and by the time a deferred
     * colon-word executes via `' ... catch`, the input pointer has already
     * moved past that text during the original top-level scan (confirmed
     * live as a garbled "path, unknown word" error). Operating on the
     * parent bridge with an explicit config address sidesteps all of this:
     * poking a devfn with nothing present is a harmless no-op on real
     * hardware (and on this emulation, per `unin_data_read`/`write`'s own
     * not-found handling in hw/pci-host/uninorth.c), so it needs no
     * presence check or `catch` at all, and safely does nothing when no AGP
     * card is attached -- confirmed live, no regression on the normal
     * main-bus-only config.
     */
    const char *agp_enable = guid ?
        "dev /pci@f0000000 h# 8004 config-b@ 2 or h# 8004 config-b! "
        "device-end "
        : "";
    /*
     * cache-POST graft (always): the l2cr/l2-cache device-tree data a real
     * PowerMac3,4 publishes plus a zeroed /diagnostics/post-results, so Mac OS
     * never shows the "problem with cache memory" alert. Independent of the
     * gmac/fwguid grafts above.
     */
    g_autofree char *bootcmd = g_strconcat("boot-command=", gmac, fwguid,
        agp_enable,
        "dev /cpus/PowerPC,G4@0 "
        "h# b9000000 encode-int \" l2cr\" property "
        "new-device \" l2-cache\" device-name \" cache\" device-type "
        "h# de86254 encode-int \" clock-frequency\" property "
        "0 0 encode-bytes \" cache-unified\" property "
        "h# 40 encode-int \" d-cache-line-size\" property "
        "h# 40 encode-int \" i-cache-line-size\" property "
        "h# 2000 encode-int \" d-cache-sets\" property "
        "h# 2000 encode-int \" i-cache-sets\" property "
        "h# 100000 encode-int \" d-cache-size\" property "
        "h# 100000 encode-int \" i-cache-size\" property "
        "finish-device device-end "
        "dev /diagnostics 0 encode-int \" post-results\" property "
        "device-end mac-boot", NULL);
    uint32_t base;

    for (base = 0; base + COPY <= size; base += COPY) {
        uint32_t off;

        /* Replace boot-command in this copy's "common" partition. */
        for (off = base; off + 16 <= base + COPY; ) {
            uint32_t plen = (((data[off + 2] << 8) | data[off + 3]) * 16);

            if (plen == 0) {
                break;
            }
            if (data[off] == 0x70 &&
                memcmp(&data[off + 4], "common", 6) == 0) {
                uint32_t dstart = off + 16, dlen = plen - 16, i;
                GString *vars = g_string_new(NULL);
                bool seen = false;

                for (i = dstart; i < dstart + dlen && data[i]; ) {
                    const char *v = (const char *)&data[i];
                    size_t vl = strnlen(v, dstart + dlen - i);

                    if (!strncmp(v, "boot-command=", 13)) {
                        g_string_append(vars, bootcmd);
                        seen = true;
                    } else {
                        g_string_append_len(vars, v, vl);
                    }
                    g_string_append_c(vars, '\0');
                    i += vl + 1;
                }
                if (!seen) {
                    g_string_append(vars, bootcmd);
                    g_string_append_c(vars, '\0');
                }
                g_string_append_c(vars, '\0');   /* end-of-list marker */

                if (vars->len <= dlen) {
                    memset(&data[dstart], 0, dlen);
                    memcpy(&data[dstart], vars->str, vars->len);
                    data[off + 1] = core99_chrp_cksum(&data[off]);
                } else {
                    warn_report("nvram: vars too large, "
                                "boot-command graft not applied");
                }
                g_string_free(vars, TRUE);
                break;
            }
            off += plen;
        }

        /* Fix the outer Adler32 in this copy's "nvram" (0x5a) header. */
        if (data[base] == 0x5a &&
            !memcmp(&data[base + 4], "nvram", 5)) {
            /* Standard Adler-32 (initial value 1), as the real Apple ROM uses. */
            uint32_t a = adler32(1, &data[base + 0x14], COPY - 0x14);
            stl_be_p(&data[base + 0x10], a);
        }
    }
}

/*
 * Return a NUL-terminated copy of an OF variable's value from the nvram
 * "common" (0x70) partition, or NULL if not present. Caller frees.
 *
 * The 16 KB flash is two mirrored 8 KB copies and the active data can live in
 * either one (the other is left erased, 0xff, as the next write target), so
 * both copies are scanned.
 */
static char *core99_nvram_get_var(const uint8_t *data, uint32_t size,
                                  const char *name)
{
    const uint32_t COPY = 0x2000;
    size_t nlen = strlen(name);
    uint32_t base, best = 0, best_gen = 0;
    bool have_best = false;

    /*
     * Two 8KB banks; the ROM (and Mac OS's nvram,flash driver) use the one
     * with the higher generation count (core99 header: CHRP header, adler32
     * at +0x10, generation at +0x14). Report from that one, not the first.
     */
    for (base = 0; base + COPY <= size; base += COPY) {
        uint32_t gen;

        if (data[base] != 0x5a || memcmp(&data[base + 4], "nvram", 5) != 0) {
            continue;
        }
        gen = ldl_be_p(&data[base + 0x14]);
        if (!have_best || gen > best_gen) {
            best = base;
            best_gen = gen;
            have_best = true;
        }
    }
    for (base = have_best ? best : 0; base + COPY <= size; base += COPY) {
        uint32_t off;

        for (off = base; off + 16 <= base + COPY; ) {
            uint32_t plen = (((data[off + 2] << 8) | data[off + 3]) * 16);
            uint32_t dstart, dend, i;

            if (plen == 0) {
                break;
            }
            if (data[off] != 0x70 || memcmp(&data[off + 4], "common", 6) != 0) {
                off += plen;
                continue;
            }
            dstart = off + 16;
            dend = (plen > 16 && off + plen <= base + COPY) ? off + plen
                                                            : base + COPY;
            for (i = dstart; i < dend && data[i]; ) {
                const char *v = (const char *)&data[i];
                size_t vl = strnlen(v, dend - i);

                if (vl > nlen && v[nlen] == '=' && !memcmp(v, name, nlen)) {
                    return g_strndup(v + nlen + 1, vl - nlen - 1);
                }
                i += vl + 1;
            }
            break;
        }
    }
    return NULL;
}

/*
 * Trace which device the machine is configured to boot from -- the OF
 * "boot-device" nvram variable (from the NVRAM bank the ROM will use).
 * This is what the guest is *set* to boot from; the Apple ROM may still fall
 * back to scanning other devices if it is absent.
 */
static void core99_report_boot_device(MacIONVRAMState *nvr)
{
    g_autofree char *dev = nvr->data ?
        core99_nvram_get_var(nvr->data, nvr->size, "boot-device") : NULL;

    trace_mac99_boot_device(dev ? dev : "(unset)");
}

/*
 * Graft the OF boot-command: always the cache-POST data (so no "cache memory"
 * alert), and the built-in gmac's MAC so Mac OS brings up en0 (skipped
 * whenever no sungem is present; the cache graft applies regardless).
 * On PM34, also grafts a synthesized FireWire GUID (see
 * core99_nvram_set_bootcmd's fwguid comment for why this can't be read from
 * real hardware); its serial portion borrows the gmac's last 3 bytes when
 * available, purely to keep the two synthesized identities consistent with
 * each other, not because real hardware ties them together.
 */
static void core99_nvram_graft(MacIONVRAMState *nvr, bool inject_gmac,
                               bool is_pm34)
{
    Object *o = object_resolve_path_type("", "sungem", NULL);
    g_autofree char *macstr = o ? object_property_get_str(o, "mac", NULL)
                                : NULL;
    uint8_t mac[6];
    uint8_t guid[8];
    bool have_mac = false;

    if (!nvr->data) {
        return;
    }
    if (macstr && sscanf(macstr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
                         &mac[5]) == 6) {
        have_mac = true;
    }
    if (is_pm34) {
        guid[0] = 0x00; guid[1] = 0x30; guid[2] = 0x65; /* Apple OUI */
        guid[3] = 0xff; guid[4] = 0xfe;                 /* EUI-64 padding */
        guid[5] = have_mac ? mac[3] : 0x00;
        guid[6] = have_mac ? mac[4] : 0x00;
        guid[7] = have_mac ? mac[5] : 0x01;
    }
    core99_nvram_seed_if_blank(nvr->data, nvr->size);
    core99_nvram_set_bootcmd(nvr->data, nvr->size,
                             (inject_gmac && have_mac) ? mac : NULL,
                             is_pm34 ? guid : NULL);
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
    MemoryRegion *rom_chip = g_new(MemoryRegion, 1);
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
    uint64_t tbfreq = kvm_enabled() ? kvmppc_get_tbfreq()
                                    : core99_tbfreq(core99_machine->model);

    /* init CPUs */
    cpus = g_new0(PowerPCCPU *, machine->smp.cpus);
    for (i = 0; i < machine->smp.cpus; i++) {
        cpus[i] = POWERPC_CPU(cpu_create(machine->cpu_type));

        cpu_ppc_tb_init(&cpus[i]->env, core99_tbfreq(core99_machine->model));

        if (PPC_INPUT(&cpus[i]->env) != PPC_FLAGS_INPUT_970) {
            core99_cpu_defaults(core99_machine->model, cpus[i]);
        }

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

    /*
     * A ROM *device* rather than plain ROM so that guest stores into the
     * boot-flash range are visible (traced) instead of silently dropped:
     * the real part is one flash chip, and a guest flash driver may issue
     * its command/ID cycles anywhere in it, not only inside the 16KB
     * NVRAM window that mac_nvram.c models. Reads still come straight
     * from the RAM backing.
     */
    memory_region_init_rom_device(bios, NULL, &core99_rom_ops, NULL,
                                  "ppc_core99.bios", PROM_SIZE,
                                  &error_fatal);
    /*
     * "rom_chip" is the whole 1MB flash part: the ROM image with, on the
     * Apple ROM machines, the 16KB NVRAM window overlaid at 0x4000 (added
     * below once the nvram device exists). Everything that decodes the
     * part -- the primary slot at PROM_BASE and every alias -- goes
     * through this container, so the NVRAM shows up in every alias too.
     */
    memory_region_init(rom_chip, NULL, "ppc_core99.rom-chip", PROM_SIZE);
    memory_region_add_subregion(rom_chip, 0, bios);
    memory_region_add_subregion(get_system_memory(), PROM_BASE, rom_chip);

    /*
     * The ROM socket decodes ROM_WINDOW_SIZE at ROM_WINDOW_BASE, but only
     * the top 1MB is populated, so the part aliases through the rest of the
     * window. The real Apple ROM makes use of that: after its low-level init
     * it jumps to code via a window-relative address such as 0xff80b644,
     * which on hardware is simply offset 0xb644 of the same flash part --
     * and Mac OS 9's "nvram,flash" native driver (Mac OS ROM 8.x parcel
     * nvram,flash-1.0d1) reads and programs the two NVRAM banks at
     * 0xff004000/0xff006000: the same 0x4000/0x6000 offsets of the part,
     * decoded 16MB below the top. With only 8MB mirrored it read zeros
     * there, found no 0x5a bank signature, and silently never wrote --
     * Startup Disk / PRAM changes were lost on every restart.
     */
    for (i = 0; i < ROM_WINDOW_SIZE / PROM_SIZE - 1; i++) {
        MemoryRegion *mirror = g_new(MemoryRegion, 1);
        g_autofree char *name = g_strdup_printf("ppc_core99.bios.mirror%d", i);

        memory_region_init_alias(mirror, NULL, name, rom_chip, 0, PROM_SIZE);
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

    unin_i2c = UNI_NORTH(s)->i2c.bus;
    core99_add_config_eeprom(core99_machine->model, unin_i2c);
    core99_add_spd_dimms(core99_machine->model, unin_i2c, machine->ram_size);

    if (core99_machine->model == CORE99_MODEL_PM36) {
        pm36_add_i2c_peripherals(unin_i2c);
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
        /*
         * AGP bus I/O space, 8 MB at 0xf0000000, matching the real
         * PowerMac3,4 /pci@f0000000 "ranges". Low overlap priority so the
         * fw_cfg registers mapped inside it (at 0xf0000510) keep winning
         * their bytes. Without this window, guest accesses to AGP I/O
         * ports hit unmapped memory instead of the empty-bus 0xff.
         */
        memory_region_add_subregion_overlap(get_system_memory(), 0xf0000000,
                                             sysbus_mmio_get_region(s, 2), -1);

        /* Uninorth internal bus */
        uninorth_internal_dev = qdev_new(
                                TYPE_UNI_NORTH_INTERNAL_PCI_HOST_BRIDGE);
        qdev_prop_set_bit(uninorth_internal_dev, "real-irq-map",
                          rom_is_flash);
        qdev_prop_set_bit(uninorth_internal_dev, "no-self-func",
                          core99_machine->model == CORE99_MODEL_PM34);
        s = SYS_BUS_DEVICE(uninorth_internal_dev);
        sysbus_realize_and_unref(s, &error_fatal);
        sysbus_mmio_map(s, 0, 0xf4800000);
        sysbus_mmio_map(s, 1, 0xf4c00000);
        /* The real pci@f4000000's one memory window (its ranges prop) */
        sysbus_mmio_map(s, 2, 0xf5000000);

        /* Uninorth main bus - this must be last to make it the default */
        uninorth_pci_dev = qdev_new(TYPE_UNI_NORTH_PCI_HOST_BRIDGE);
        qdev_prop_set_uint32(uninorth_pci_dev, "ofw-addr", 0xf2000000);
        /*
         * The Apple ROM's device tree carries the real machine's
         * interrupt-map, so in that mode the slots must interrupt where
         * that table says they do (see pci_unin_main_real_map_irq()).
         * OpenBIOS builds its own tree around the legacy 4-line hash,
         * which stays the default.
         */
        qdev_prop_set_bit(uninorth_pci_dev, "real-irq-map", rom_is_flash);
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
    macio = OBJECT(pci_new(core99_macio_devfn(core99_machine->model),
                          TYPE_NEWWORLD_MACIO));
    dev = DEVICE(macio);
    qdev_prop_set_uint64(dev, "frequency", tbfreq);
    qdev_prop_set_bit(dev, "has-pmu", has_pmu);
    qdev_prop_set_bit(dev, "has-adb", has_adb);
    /*
     * The real KeyLargo ATA layout only when the Apple ROM provides the
     * device tree; OpenBIOS hardcodes the legacy channels/interrupts.
     */
    qdev_prop_set_bit(dev, "real-ata", rom_is_flash);
    if (has_pmu && rom_is_flash) {
        /*
         * Per-model PRAM file, like nvram-flash-34/36.img: PRAM contents
         * are model specific too, so models run from one directory must
         * not share a file.
         */
        qdev_prop_set_string(dev, "pram-file",
                             core99_machine->model == CORE99_MODEL_PM36 ?
                             "pram-36.img" : "pram-34.img");
    }

    dev = DEVICE(object_resolve_path_component(macio, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));

    pic_dev = DEVICE(object_resolve_path_component(macio, "pic"));
    qdev_prop_set_uint32(pic_dev, "nb_cpus", machine->smp.cpus);

    qdev_prop_set_uint32(DEVICE(object_resolve_path_component(macio, "gpio")),
                         "nb-cpus", machine->smp.cpus);

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
    /*
     * Apple ROM only. This is NOT harmless under OpenBIOS: the notifier arms a
     * *reset* handler, so the BAR is forced back to the hardware base on every
     * subsequent reset too, behind the back of firmware that had legitimately
     * assigned it somewhere else. Classic Mac OS then loads over PIO and wedges
     * the moment it touches MacIO again. Bisected to this commit; gating it
     * here restores the OpenBIOS boot.
     */
    if (rom_is_flash) {
        macio_notifier = g_new0(MacIOHwBaseNotifier, 1);
        macio_notifier->notifier.notify = macio_arm_hw_base_reset;
        macio_notifier->macio = PCI_DEVICE(macio);
        qemu_add_machine_init_done_notifier(&macio_notifier->notifier);
    }

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
    if (rom_is_flash) {
        core99_pci_irq_map(core99_machine->model, uninorth_pci_dev, pic_dev);
    } else {
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_pci_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }
    }

    /* TODO: additional PCI buses only wired up for 32-bit machines */
    if (PPC_INPUT(env) != PPC_FLAGS_INPUT_970) {
        /* Uninorth AGP bus */
        for (i = 0; i < 4; i++) {
            qdev_connect_gpio_out(uninorth_agp_dev, i,
                                  qdev_get_gpio_in(pic_dev, 0x1b + i));
        }

        /* Uninorth internal bus */
        if (rom_is_flash) {
            core99_internal_bus_irq_map(core99_machine->model,
                                        uninorth_internal_dev, pic_dev);
        } else {
            for (i = 0; i < 4; i++) {
                qdev_connect_gpio_out(uninorth_internal_dev, i,
                                      qdev_get_gpio_in(pic_dev, 0x1b + i));
            }
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
    if (machine->smp.cpus > 1) {
        CpuProbeWakeState *pw = g_new0(CpuProbeWakeState, 1);
        qemu_irq cpu_probe_irq;

        pw->cpu = cpus[1];
        pw->rehalt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        cpu_probe_wake_rehalt, CPU(cpus[1]));
        cpu_probe_irq = qemu_allocate_irq(cpu_probe_wake, pw, 0);
        sysbus_connect_irq(s, KL_GPIO_CPU_PROBE - KEYLARGO_GPIO_EXTINT_0,
                           cpu_probe_irq);
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
        pci_create_simple(pci_bus, core99_usb_devfn(core99_machine->model),
                          "pci-ohci");

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

    /* OpenBIOS machines keep the default placement its own tree describes. */
    if (rom_is_flash && uninorth_internal_dev) {
        PCIBus *internal_bus =
            PCI_HOST_BRIDGE(uninorth_internal_dev)->bus;

        core99_place_gmac(core99_machine->model, internal_bus,
                         mc->default_nic);

        /* Real PM34 device tree: /pci@f4000000/firewire@e, same internal
         * bus as ethernet@f -- not the external pci@f2000000 bus. */
        if (core99_machine->model == CORE99_MODEL_PM34) {
            pci_create_simple(internal_bus, pm34_firewire_devfn(),
                              "mac99-firewire-stub");
        }
    } else {
        pci_init_nic_devices(pci_bus, mc->default_nic);
    }

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
        /*
         * Per-model default filenames: PowerMac3,4 and PowerMac3,6 have
         * different NVRAM contents (different device trees, SPD data,
         * etc), so sharing one file between models run from the same
         * directory would silently cross-contaminate their PRAM/NVRAM.
         */
        const char *flash_name = core99_machine->model == CORE99_MODEL_PM36 ?
            "nvram-flash-36.img" : "nvram-flash-34.img";
        BlockBackend *nvram_blk = rom_is_flash ?
            macio_nvram_default_blk(flash_name, NVRAM_FLASH_SIZE, 0xff) :
            macio_nvram_default_blk("nvram.img", MACIO_NVRAM_SIZE, 0);

        if (nvram_blk) {
            qdev_prop_set_drive(dev, "drive", nvram_blk);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    if (rom_is_flash && nvram_addr == 0xFFF04000) {
        /* Part of the flash chip: overlay it inside every ROM alias. */
        memory_region_add_subregion_overlap(rom_chip, nvram_addr - PROM_BASE,
                                            sysbus_mmio_get_region(
                                                SYS_BUS_DEVICE(dev), 0), 1);
    } else {
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, nvram_addr);
    }
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
    /*
     * Real Apple ROM: graft the OF boot-command -- the cache-POST data always,
     * and the built-in gmac's MAC when a sungem is present. Patches the in-RAM
     * nvram image only; the backing file is left untouched.
     */
    if (rom_is_flash) {
        core99_nvram_graft(nvr, true,
                          core99_machine->model == CORE99_MODEL_PM34);
    }
    core99_report_boot_device(nvr);
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
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_CLOCKFREQ,
                  core99_clockfreq(core99_machine->model));
    fw_cfg_add_i32(fw_cfg, FW_CFG_PPC_BUSFREQ,
                  core99_busfreq(core99_machine->model));
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

#ifndef TARGET_PPC64
/*
 * PowerMac3,6's real CPU is a 7455, not the 7400 every other mac99 config
 * defaults to. Hooking get_default_cpu_type (rather than setting cpu_type
 * directly somewhere in ppc_core99_init) means an explicit "-cpu" still
 * wins, the same as it does for every other machine -- QEMU only calls this
 * when the user didn't ask for a specific CPU.
 */
static const char *core99_get_default_cpu_type(const MachineState *ms)
{
    Core99MachineState *cms = CORE99_MACHINE((MachineState *)ms);

    if (cms->model == CORE99_MODEL_PM36) {
        return POWERPC_CPU_TYPE_NAME("7455_v2.1");
    }
    return POWERPC_CPU_TYPE_NAME("7400_v2.9");
}
#endif

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
    mc->get_default_cpu_type = core99_get_default_cpu_type;
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

static char *core99_get_model(Object *obj, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    switch (cms->model) {
    default:
    case CORE99_MODEL_PM34:
        return g_strdup("pm34");

    case CORE99_MODEL_PM36:
        return g_strdup("pm36");
    }
}

/*
 * "pm34"/"pm36" rather than the real "3,4"/"3,6" model numbers: -M's option
 * string is comma-separated, so a literal comma in a property value needs
 * doubling (-M mac99,model=3,,6) to parse at all -- not what anyone would
 * type unprompted. Avoid the footgun entirely instead of documenting it.
 */
static void core99_set_model(Object *obj, const char *value, Error **errp)
{
    Core99MachineState *cms = CORE99_MACHINE(obj);

    if (!strcmp(value, "pm34")) {
        cms->model = CORE99_MODEL_PM34;
    } else if (!strcmp(value, "pm36")) {
        cms->model = CORE99_MODEL_PM36;
    } else {
        error_setg(errp, "Invalid model value");
        error_append_hint(errp, "Valid values are pm34 and pm36.\n");
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

    /* Default model is PowerMac3,4 (Digital Audio) */
    cms->model = CORE99_MODEL_PM34;
    object_property_add_str(obj, "model", core99_get_model,
                            core99_set_model);
    object_property_set_description(obj, "model",
                                    "Set the real PowerMac model to emulate "
                                    "(pm34 = PowerMac3,4 Digital Audio, "
                                    "pm36 = PowerMac3,6 Mirrored Drive "
                                    "Doors). Valid values are pm34 and pm36");
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
