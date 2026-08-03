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
#include "qobject/qdict.h"
#include "qapi/error.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

/* macio style NVRAM device */
/*
 * A real Apple ROM keeps its Open Firmware variables in the boot flash and
 * maintains them with the Intel command set. Its /nvram package decompiles to
 *
 *   erase: 20 -> block, d0 -> block, poll status bit 0x80, ff -> block,
 *          then verify the block reads all ones
 *   write: 40 -> byte, data -> byte, poll status, ff -> block, verify
 *
 * so the window has to answer those commands. Treated as plain RAM every
 * erase fails its verify and the firmware reports "ERASE failure on NVRAM",
 * losing every setting. Erase granularity is 8KB: the ROM's own info property
 * describes fff04000 as 0x4000 long and its package alternates between two
 * blocks.
 */
#define NVRAM_FLASH_SECTOR   0x2000
#define NVRAM_FLASH_READY    0x80

enum {
    NVRAM_FLASH_CMD_NONE        = 0x00,
    NVRAM_FLASH_CMD_ERASE_SETUP = 0x20,
    NVRAM_FLASH_CMD_PROGRAM     = 0x40,
    NVRAM_FLASH_CMD_ERASE_CONF  = 0xd0,
    NVRAM_FLASH_CMD_READ_ARRAY  = 0xff,
};

static void macio_nvram_flush(MacIONVRAMState *s, hwaddr addr, int len)
{
    if (s->blk) {
        if (blk_pwrite(s->blk, addr, len, &s->data[addr], 0) < 0) {
            error_report("%s: cannot write to NVRAM", blk_name(s->blk));
        }
    }
}

/* Returns true if the access was a command rather than plain data. */
static bool macio_nvram_flash_write(MacIONVRAMState *s, hwaddr addr,
                                    uint8_t value)
{
    if (!s->flash) {
        return false;
    }

    switch (s->flash_cmd) {
    case NVRAM_FLASH_CMD_ERASE_SETUP:
        s->flash_cmd = NVRAM_FLASH_CMD_NONE;
        if (value == NVRAM_FLASH_CMD_ERASE_CONF) {
            hwaddr base = addr & ~(hwaddr)(NVRAM_FLASH_SECTOR - 1);

            memset(&s->data[base], 0xff, NVRAM_FLASH_SECTOR);
            macio_nvram_flush(s, base, NVRAM_FLASH_SECTOR);
            s->flash_status = NVRAM_FLASH_READY;
        }
        return true;

    case NVRAM_FLASH_CMD_PROGRAM:
        /* Programming can only clear bits, exactly like the real part. */
        s->flash_cmd = NVRAM_FLASH_CMD_NONE;
        s->data[addr] &= value;
        macio_nvram_flush(s, addr, 1);
        s->flash_status = NVRAM_FLASH_READY;
        return true;

    default:
        break;
    }

    switch (value) {
    case NVRAM_FLASH_CMD_ERASE_SETUP:
    case NVRAM_FLASH_CMD_PROGRAM:
        s->flash_cmd = value;
        s->flash_status = 0;
        return true;
    case NVRAM_FLASH_CMD_READ_ARRAY:
        s->flash_cmd = NVRAM_FLASH_CMD_NONE;
        s->flash_status = 0;
        return true;
    default:
        /* Anything else in the array state is not data: ignore it. */
        return true;
    }
}

static void macio_nvram_writeb(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    MacIONVRAMState *s = opaque;

    addr = (addr >> s->it_shift) & (s->size - 1);
    trace_macio_nvram_write(addr, value);

    if (macio_nvram_flash_write(s, addr, value)) {
        return;
    }
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
    /* While a command is in flight the part reports its status register. */
    if (s->flash && s->flash_status) {
        value = s->flash_status;
    }

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
    DEFINE_PROP_BOOL("flash", MacIONVRAMState, flash, false),
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
static bool oldworld_of_partition_valid(const uint8_t *buf)
{
    uint8_t tmp[OLDWORLD_OF_SIZE];
    uint32_t acc = 0;
    uint16_t stored;
    int i;

    if (lduw_be_p(buf) != 0x1275 || buf[2] != 5) {
        return false;
    }

    stored = lduw_be_p(buf + 4);
    memcpy(tmp, buf, OLDWORLD_OF_SIZE);
    tmp[4] = tmp[5] = 0;
    for (i = 0; i < OLDWORLD_OF_SIZE; i += 2) {
        acc += lduw_be_p(tmp + i);
    }
    acc = (acc + (acc >> 16)) & 0xffff;

    return ((~acc) & 0xffff) == stored;
}

/* Set up the Old World genuine-OF NVRAM partition (not the CHRP/OpenBIOS
 * one below, which real Old World ROMs never look at). If a backing file
 * (nvr->blk) already holds a valid partition -- persisted from an earlier
 * run -- it's left untouched instead of being clobbered with defaults. */
void pmac_format_nvram_partition_oldworld(MacIONVRAMState *nvr)
{
    uint8_t *buf = &nvr->data[OLDWORLD_OF_OFFSET];

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

/*
 * Manage a default NVRAM backing file when the user has not attached one
 * with -drive if=mtd,... Without this the contents are re-stamped from
 * scratch on every run, so nothing a firmware writes -- boot-device,
 * auto-boot?, the lot -- ever survives a restart.
 */
BlockBackend *macio_nvram_default_blk(const char *filename, uint32_t size,
                                     uint8_t fill)
{
    struct stat st;
    BlockBackend *blk;
    Error *local_err = NULL;
    QDict *options;
    int fd;

    fd = qemu_open_old(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        warn_report("could not open/create default NVRAM image '%s': %s",
                    filename, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) == 0 && st.st_size < size) {
        /*
         * Flash-backed NVRAM must start out erased (all ones), not zeroed:
         * firmware tells "never written" from "written and invalid", and a
         * block of zeroes is neither.
         */
        g_autofree uint8_t *blank = g_malloc(size);

        memset(blank, fill, size);
        if (ftruncate(fd, 0) != 0 || write(fd, blank, size) != size) {
            warn_report("could not size default NVRAM image '%s': %s",
                        filename, strerror(errno));
        }
    }
    close(fd);

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(filename, NULL, options, BDRV_O_RDWR, &local_err);
    if (!blk) {
        warn_report_err(local_err);
        return NULL;
    }
    return blk;
}

/* True if nothing has ever been stored here. */
bool macio_nvram_is_blank(MacIONVRAMState *nvr, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (nvr->data[i]) {
            return false;
        }
    }
    return true;
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

    if (nvr->blk) {
        if (blk_pwrite(nvr->blk, 0, len, nvr->data, 0) < 0) {
            error_report("%s: failed to write default NVRAM partitions",
                         blk_name(nvr->blk));
        }
    }
}
type_init(macio_nvram_register_types)
