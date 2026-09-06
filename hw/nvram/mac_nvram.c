/*
 * PowerMac NVRAM emulation
 *
 * Copyright (c) 2005-2007 Fabrice Bellard
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
#include "qapi/error.h"
#include "hw/nvram/chrp_nvram.h"
#include "hw/nvram/mac_nvram.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

/* macio style NVRAM device */
static void macio_nvram_writeb(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    MacIONVRAMState *s = opaque;

    addr = (addr >> s->it_shift) & (s->size - 1);
    trace_macio_nvram_write(addr, value);
    s->data[addr] = value;
    if (s->blk) {
        if (blk_pwrite(s->blk, addr, 1, &s->data[addr], 0) < 0) {
            error_report("%s: write of NVRAM data to backing store failed",
                         blk_name(s->blk));
        }
    }
}

static uint64_t macio_nvram_readb(void *opaque, hwaddr addr,
                                  unsigned size)
{
    MacIONVRAMState *s = opaque;
    uint32_t value;

    addr = (addr >> s->it_shift) & (s->size - 1);
    value = s->data[addr];
    trace_macio_nvram_read(addr, value);

    return value;
}

static const MemoryRegionOps macio_nvram_ops = {
    .read = macio_nvram_readb,
    .write = macio_nvram_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_BIG_ENDIAN,
};

static const VMStateDescription vmstate_macio_nvram = {
    .name = "macio_nvram",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(data, MacIONVRAMState, 0, NULL, size),
        VMSTATE_END_OF_LIST()
    }
};


static void macio_nvram_reset(DeviceState *dev)
{
}

static void macio_nvram_realizefn(DeviceState *dev, Error **errp)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    s->data = g_malloc0(s->size);

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg_errno(errp, -len,
                             "could not get length of nvram backing image");
            return;
        } else if (len != s->size) {
            error_setg_errno(errp, -len,
                             "invalid size nvram backing image");
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, s->size, s->data, 0) < 0) {
            error_setg(errp, "can't read-nvram contents");
            return;
        }
    }

    memory_region_init_io(&s->mem, OBJECT(s), &macio_nvram_ops, s,
                          "macio-nvram", s->size << s->it_shift);
    sysbus_init_mmio(d, &s->mem);
}

static void macio_nvram_unrealizefn(DeviceState *dev)
{
    MacIONVRAMState *s = MACIO_NVRAM(dev);

    g_free(s->data);
}

static const Property macio_nvram_properties[] = {
    DEFINE_PROP_UINT32("size", MacIONVRAMState, size, 0),
    DEFINE_PROP_UINT32("it_shift", MacIONVRAMState, it_shift, 0),
    DEFINE_PROP_DRIVE("drive", MacIONVRAMState, blk),
};

static void macio_nvram_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = macio_nvram_realizefn;
    dc->unrealize = macio_nvram_unrealizefn;
    device_class_set_legacy_reset(dc, macio_nvram_reset);
    dc->vmsd = &vmstate_macio_nvram;
    device_class_set_props(dc, macio_nvram_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo macio_nvram_type_info = {
    .name = TYPE_MACIO_NVRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MacIONVRAMState),
    .class_init = macio_nvram_class_init,
};

static void macio_nvram_register_types(void)
{
    type_register_static(&macio_nvram_type_info);
}

/* Set up a system OpenBIOS NVRAM partition */
static void pmac_format_nvram_partition_of(MacIONVRAMState *nvr, int off,
                                           int len)
{
    int sysp_end;

    /* OpenBIOS nvram variables partition */
    sysp_end = chrp_nvram_create_system_partition(&nvr->data[off],
                                                  DEF_SYSTEM_SIZE, len) + off;

    /* Free space partition */
    chrp_nvram_create_free_partition(&nvr->data[sysp_end], len - sysp_end);
}

/*
 * Old World Macs' genuine Open Firmware NVRAM partition: signature 0x1275,
 * version 5, at a fixed offset (0x1800) -- a completely different layout
 * from the CHRP/OpenBIOS text partition above (which is what New World
 * ROMs use instead; see DingusPPC's devices/common/ofnvram.cpp, whose own
 * comments label OfConfigAppl "Old World" vs OfConfigChrp "New World").
 * A real Beige G3 ROM's NanoKernel-level NVRAM validation looks for this
 * exact structure; without it, the boot never leaves the 68K low-memory
 * init code (confirmed by comparing against a live DingusPPC capture of
 * the identical ROM, whose own NVRAM has this partition populated and
 * has nothing at all at the offsets the CHRP partition above uses).
 *
 * The two chunks below are copied verbatim from a real, checksum-valid
 * partition captured from a successful DingusPPC boot of this same ROM
 * (header + int vars, then the string heap grown down from the partition
 * end); everything in between is zero, matching the captured data.
 */
#define OLDWORLD_OF_OFFSET      0x1800
#define OLDWORLD_OF_SIZE        0x800

static const uint8_t oldworld_of_partition_head[] = {
    /* sig=0x1275 version=5 num_pages=8 checksum=0x78fb here=0x185c top=0x1fe2 */
    0x12, 0x75, 0x05, 0x08, 0x78, 0xfb, 0x18, 0x5c, 0x1f, 0xe2, 0x00, 0x00,
    /* flags (auto-boot? only) */
    0x20, 0x00, 0x00, 0x00,
    /* real-base, real-size, virt-base, virt-size, load-base */
    0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    /* pci-probe-list, screen-#columns, screen-#rows */
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x28,
    /* selftest-#megs */
    0x00, 0x00, 0x00, 0x00,
    /* boot-device, boot-file, diag-device, diag-file, input-device,
     * output-device, oem-banner, oem-logo, nvramrc, boot-command
     * (offset,length) pairs into the string heap below */
    0x1f, 0xf7, 0x00, 0x09, 0x1f, 0xf7, 0x00, 0x00,
    0x1f, 0xef, 0x00, 0x08, 0x1f, 0xef, 0x00, 0x00, 0x1f, 0xec, 0x00, 0x03,
    0x1f, 0xe6, 0x00, 0x06, 0x1f, 0xe6, 0x00, 0x00, 0x1f, 0xe6, 0x00, 0x00,
    0x1f, 0xe6, 0x00, 0x00, 0x1f, 0xe2, 0x00, 0x04,
};

/* "/AAPL,ROM" (boot-device), "fd:diags" (diag-device), "kbd"
 * (input-device), "screen" (output-device), "boot" (boot-command) */
static const uint8_t oldworld_of_partition_strings[] = {
    'b', 'o', 'o', 't', 's', 'c', 'r', 'e', 'e', 'n', 'k', 'b', 'd',
    'f', 'd', ':', 'd', 'i', 'a', 'g', 's',
    '/', 'A', 'A', 'P', 'L', ',', 'R', 'O', 'M',
};

/* Checks the same signature/version/checksum a real Old World ROM's own
 * NVRAM validation would (see DingusPPC's OfConfigAppl::validate()) --
 * used to tell an already-valid, persisted partition (loaded from an
 * attached backing file, e.g. a prior boot's saved boot-device) apart
 * from a fresh/blank one that still needs our defaults. */
static uint16_t oldworld_of_checksum(const uint8_t *buf)
{
    uint8_t tmp[OLDWORLD_OF_SIZE];
    uint32_t acc = 0;
    int i;

    memcpy(tmp, buf, OLDWORLD_OF_SIZE);
    tmp[4] = tmp[5] = 0;
    for (i = 0; i < OLDWORLD_OF_SIZE; i += 2) {
        acc += lduw_be_p(tmp + i);
    }
    acc = (acc + (acc >> 16)) & 0xffff;

    return (~acc) & 0xffff;
}

static bool oldworld_of_partition_valid(const uint8_t *buf)
{
    if (lduw_be_p(buf) != 0x1275 || buf[2] != 5) {
        return false;
    }

    return oldworld_of_checksum(buf) == lduw_be_p(buf + 4);
}

/*
 * The partition's string variables. Each is an (offset, length) pair of
 * big-endian 16-bit words at OLDWORLD_OF_VARS, in this fixed order; the
 * offsets are absolute NVRAM offsets into a string heap that grows DOWN
 * from the end of the partition. "top" in the header tracks the lowest
 * byte the heap has reached. Cross-checked against DingusPPC's
 * devices/common/ofnvram.cpp, which exposes the same table to its
 * debugger's setenv.
 */
#define OLDWORLD_OF_TOP         0x08
#define OLDWORLD_OF_FLAGS       0x0c
#define OLDWORLD_OF_VARS        0x34

enum {
    OF_VAR_BOOT_DEVICE, OF_VAR_BOOT_FILE, OF_VAR_DIAG_DEVICE,
    OF_VAR_DIAG_FILE, OF_VAR_INPUT_DEVICE, OF_VAR_OUTPUT_DEVICE,
    OF_VAR_OEM_BANNER, OF_VAR_OEM_LOGO, OF_VAR_NVRAMRC,
    OF_VAR_BOOT_COMMAND, OF_VAR__COUNT
};

/* Flag bits are numbered from the MSB of the flags word, in the order
 * little-endian?, real-mode?, auto-boot?, diag-switch?, fcode-debug?,
 * oem-banner?, oem-logo?, use-nvramrc?, f-segment? -- same source. */
#define OLDWORLD_OF_AUTO_BOOT       0x20000000
#define OLDWORLD_OF_USE_NVRAMRC     0x01000000

static const char *oldworld_of_get_var(const uint8_t *buf, int var,
                                       unsigned *len)
{
    unsigned off = lduw_be_p(buf + OLDWORLD_OF_VARS + 4 * var);

    *len = lduw_be_p(buf + OLDWORLD_OF_VARS + 4 * var + 2);
    if (off < OLDWORLD_OF_OFFSET ||
        off + *len > OLDWORLD_OF_OFFSET + OLDWORLD_OF_SIZE) {
        *len = 0;
        return NULL;
    }
    return (const char *)buf + (off - OLDWORLD_OF_OFFSET);
}

/*
 * Rewrite the partition's variables, rebuilding the string heap. Returns
 * false (leaving the partition untouched) if the strings do not fit.
 */
static bool oldworld_of_set_vars(uint8_t *buf,
                                 const char *const value[OF_VAR__COUNT],
                                 uint32_t flags)
{
    unsigned heap = OLDWORLD_OF_SIZE, off[OF_VAR__COUNT], len[OF_VAR__COUNT];
    uint8_t new_part[OLDWORLD_OF_SIZE];
    unsigned table_end = lduw_be_p(buf + 0x06) - OLDWORLD_OF_OFFSET;
    int i;

    memcpy(new_part, buf, OLDWORLD_OF_SIZE);

    for (i = 0; i < OF_VAR__COUNT; i++) {
        len[i] = value[i] ? strlen(value[i]) : 0;
        if (len[i] > heap || heap - len[i] < table_end) {
            return false;
        }
        heap -= len[i];
        memcpy(new_part + heap, value[i], len[i]);
        off[i] = heap;
    }

    memset(new_part + table_end, 0, heap - table_end);
    for (i = 0; i < OF_VAR__COUNT; i++) {
        /* an empty variable still needs a plausible offset: the heap top */
        stw_be_p(new_part + OLDWORLD_OF_VARS + 4 * i,
                 OLDWORLD_OF_OFFSET + (len[i] ? off[i] : heap));
        stw_be_p(new_part + OLDWORLD_OF_VARS + 4 * i + 2, len[i]);
    }
    stw_be_p(new_part + OLDWORLD_OF_TOP, OLDWORLD_OF_OFFSET + heap);
    stl_be_p(new_part + OLDWORLD_OF_FLAGS, flags);
    stw_be_p(new_part + 4, 0);
    stw_be_p(new_part + 4, oldworld_of_checksum(new_part));

    memcpy(buf, new_part, OLDWORLD_OF_SIZE);
    return true;
}

/* Set up the Old World genuine-OF NVRAM partition (not the CHRP/OpenBIOS
 * one below, which real Old World ROMs never look at). If a backing file
 * (nvr->blk) already holds a valid partition -- persisted from an earlier
 * run -- it's left untouched instead of being clobbered with defaults. */
/*
 * Old World "cold boot" POST-request flag inside the ROM's own settings
 * area of NVRAM. Mac OS sets it (value 9 observed, Mac OS 8.1) during
 * Shut Down so the ROM treats the next power-on as a cold boot and runs
 * its FULL destructive RAM test -- CONCURRENTLY with the second (RAM-
 * resident) half of the startup chime. The test sweeps all of RAM,
 * including the chime's live DBDMA descriptor chain at phys 0x20-0xDF;
 * the engine then fetches test-pattern words as descriptors, hits a
 * reserved command, and goes DEAD with the command pointer short of the
 * terminating STOP -- which the ROM's completion loop re-kicks forever
 * (poll at 0xfff046fc/0xfff04720: retry until *cmdptr == 0x70000000).
 * Result: half a chime, then a permanent hang, on every boot after a
 * clean Shut Down. Real silicon evidently survives this sequence;
 * neither we nor DingusPPC do (its project workflow deletes nvram
 * before every run for the same reason). Until the surviving-silicon
 * behavior is understood, scrub the flag at machine init so every
 * emulated boot takes the (proven-good) warm-boot path -- exactly what
 * deleting nvram.img achieved, minus losing the rest of NVRAM.
 */
#define OLDWORLD_POST_REQ_OFFSET 0x1043

void pmac_format_nvram_partition_oldworld(MacIONVRAMState *nvr)
{
    uint8_t *buf = &nvr->data[OLDWORLD_OF_OFFSET];

    if (nvr->data[OLDWORLD_POST_REQ_OFFSET]) {
        nvr->data[OLDWORLD_POST_REQ_OFFSET] = 0;
        if (nvr->blk) {
            blk_pwrite(nvr->blk, OLDWORLD_POST_REQ_OFFSET, 1,
                       &nvr->data[OLDWORLD_POST_REQ_OFFSET], 0);
        }
    }

    if (oldworld_of_partition_valid(buf)) {
        return;
    }

    memset(buf, 0, OLDWORLD_OF_SIZE);
    memcpy(buf, oldworld_of_partition_head, sizeof(oldworld_of_partition_head));
    memcpy(buf + OLDWORLD_OF_SIZE - sizeof(oldworld_of_partition_strings),
           oldworld_of_partition_strings, sizeof(oldworld_of_partition_strings));

    if (nvr->blk) {
        if (blk_pwrite(nvr->blk, OLDWORLD_OF_OFFSET, OLDWORLD_OF_SIZE, buf,
                       0) < 0) {
            error_report("%s: failed to write default Old World NVRAM "
                        "partition", blk_name(nvr->blk));
        }
    }
}

/*
 * Point a pristine Old World NVRAM at a Mac OS X startup device.
 *
 * A machine whose only disk holds Mac OS X reaches the flashing question
 * mark and stops: with the default boot-device "/AAPL,ROM" the ROM's own
 * boot scan finds nothing (measured: a fresh NVRAM, and a fresh NVRAM
 * plus a working nvramrc with boot-device left at /AAPL,ROM, both sit
 * at the flashing floppy), and an explicit boot-device alone gets as far as
 * the video-mode switch and then dies with the CPU at address 0. On real
 * hardware you escape that by booting Mac OS 9 once: its Startup Disk
 * control panel writes an explicit boot-device AND an nvramrc, and that
 * nvramrc is what this shim stands in for.
 *
 * A guest that has never run Mac OS 9 has no way to get that written,
 * so do for it what the control panel would: when the partition is
 * still exactly our untouched default, install the shim and the path.
 * Anything the guest itself has since written is left alone.
 */
/*
 * The word is called "bootr", and boot-command invokes it exactly as
 * Apple's does, because Mac OS X rewrites boot-command to "0 bootr "
 * itself during its first boot while leaving nvramrc alone (observed
 * 2026-09-06). Under any other name our shim would define a word that
 * nothing calls afterwards, and the second boot would fail. The leading
 * 0 is an argument Apple's version reads; ours ignores it.
 */
static const char oldworld_osx_boot_command[] = "0 bootr ";

/*
 * The shim itself. Two jobs, each shown necessary by removing it from
 * the working shim and watching the boot fail on a Mac OS X 10.0 volume
 * (2026-09-06, with the full shim booting to the desktop on the same
 * binary as the control):
 *
 *  - mac-io decode-unit: redefined to parse one hex number. Without it
 *    (arm F5) the boot reaches the video-mode switch and ends with the
 *    CPU at address 0. The reading that this is what lets the "@0" in a
 *    boot path like "ide0/@0:6" resolve comes from the word's name and
 *    from published OF documentation, not from watching OF.
 *  - qmem/qargs: release two low memory ranges, make OF's
 *    install-interrupt-vectors a no-op, and give /chosen an empty
 *    "machargs" property. Without them (arm G2, ": bootr boot ;") the
 *    screen never leaves 640x480 black and the CPU idles in real mode
 *    (MSR 0x40) around 0x4095xx. Which of the three parts matters, and
 *    why, was not separated.
 *
 * Not in the shim: Apple's Startup Disk script also branch-patches the
 * /packages/mac-parts package. Removing that block from our working shim
 * (arm G1) still boots the 10.0 volume to the desktop in ~90 s, the same
 * as with it, so it is left out. The 10.0 disk's HFS wrapper holds no
 * copy of BootX, so how OF loads BootX from the wrapped HFS+ volume
 * without it is not known; the earlier claim that "Old World OF cannot
 * read HFS+" came from web documentation and is not supported by this
 * measurement. Only 10.0 was tried.
 *
 * This is our own Forth, tested on a Mac OS X 10.0 volume, which it
 * boots to the desktop in ~90 s. Apple's script is longer: key-map
 * polling for the boot-time modifier keys and a retry loop; none of
 * that is reproduced here.
 */
static const char oldworld_osx_boot_shim[] =
    "hex\r"
    ": qE device-end ;\r"
    ": qR BRpatch ;\r"
    ": qprop 0 to my-self property ;\r"
    ": qargs \" \" encode-string \" machargs\" \" /chosen\" find-device "
        "qprop qE ;\r"
    ": qmem ['] install-interrupt-vectors ['] noop qR\r"
    "0 4000 release-mem 8000 2000 release-mem ;\r"
    "dev mac-io\r"
    ": decode-unit parse-1hex ;\r"
    "qE\r"
    ": bootr qargs qmem boot ;\r";

bool pmac_oldworld_nvram_is_default(MacIONVRAMState *nvr)
{
    const uint8_t *buf = &nvr->data[OLDWORLD_OF_OFFSET];
    const char *dev;
    unsigned dev_len, rc_len;

    if (!oldworld_of_partition_valid(buf)) {
        return false;
    }
    dev = oldworld_of_get_var(buf, OF_VAR_BOOT_DEVICE, &dev_len);
    oldworld_of_get_var(buf, OF_VAR_NVRAMRC, &rc_len);

    return rc_len == 0 && dev_len == strlen("/AAPL,ROM") &&
           !memcmp(dev, "/AAPL,ROM", dev_len);
}

void pmac_oldworld_nvram_set_osx_startup(MacIONVRAMState *nvr,
                                         const char *device)
{
    uint8_t *buf = &nvr->data[OLDWORLD_OF_OFFSET];
    const char *value[OF_VAR__COUNT] = { NULL };
    unsigned len;
    int i;

    for (i = 0; i < OF_VAR__COUNT; i++) {
        value[i] = oldworld_of_get_var(buf, i, &len);
        value[i] = len ? g_strndup(value[i], len) : NULL;
    }
    g_free((char *)value[OF_VAR_BOOT_DEVICE]);
    value[OF_VAR_BOOT_DEVICE] = g_strdup(device);
    g_free((char *)value[OF_VAR_NVRAMRC]);
    value[OF_VAR_NVRAMRC] = g_strdup(oldworld_osx_boot_shim);
    g_free((char *)value[OF_VAR_BOOT_COMMAND]);
    value[OF_VAR_BOOT_COMMAND] = g_strdup(oldworld_osx_boot_command);

    if (!oldworld_of_set_vars(buf, value,
                              ldl_be_p(buf + OLDWORLD_OF_FLAGS) |
                              OLDWORLD_OF_AUTO_BOOT |
                              OLDWORLD_OF_USE_NVRAMRC)) {
        warn_report("NVRAM: Mac OS X startup shim does not fit, leaving "
                    "the default partition alone");
    } else if (nvr->blk &&
               blk_pwrite(nvr->blk, OLDWORLD_OF_OFFSET, OLDWORLD_OF_SIZE,
                          buf, 0) < 0) {
        error_report("%s: failed to write the Mac OS X startup NVRAM "
                     "partition", blk_name(nvr->blk));
    }

    for (i = 0; i < OF_VAR__COUNT; i++) {
        g_free((char *)value[i]);
    }
}

#define OSX_NVRAM_SIGNATURE     (0x5A)

/* Set up a Mac OS X NVRAM partition */
static void pmac_format_nvram_partition_osx(MacIONVRAMState *nvr, int off,
                                            int len)
{
    uint32_t start = off;
    ChrpNvramPartHdr *part_header;
    unsigned char *data = &nvr->data[start];

    /* empty partition */
    part_header = (ChrpNvramPartHdr *)data;
    part_header->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(part_header->name, sizeof(part_header->name), "wwwwwwwwwwww");

    chrp_nvram_finish_partition(part_header, len);

    /* Generation */
    stl_be_p(&data[20], 2);

    /* Adler32 checksum */
    stl_be_p(&data[16], adler32(0, &data[20], len - 20));
}

/* Set up NVRAM with OF and OSX partitions */
void pmac_format_nvram_partition(MacIONVRAMState *nvr, int len)
{
    /*
     * Mac OS X expects side "B" of the flash at the second half of NVRAM,
     * so we use half of the chip for OF and the other half for a free OSX
     * partition.
     */
    pmac_format_nvram_partition_of(nvr, 0, len / 2);
    pmac_format_nvram_partition_osx(nvr, len / 2, len / 2);
}
type_init(macio_nvram_register_types)
