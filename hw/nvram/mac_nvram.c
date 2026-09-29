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
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "system/system.h"
#include "qobject/qdict.h"
#include "trace.h"
#include <zlib.h> /* for adler32 */

#define DEF_SYSTEM_SIZE 0xc10

/*
 * Flash NVRAM, Intel command set as used by Apple firmware and by the
 * Sharp/Micron parts Linux drives:
 *   erase: 20, d0 to the block, poll status bit 0x80, ff
 *   write: 40, data to the byte, poll status, ff
 * Erase granularity is 8 KB.
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
            error_report("%s: write of NVRAM data to backing store failed",
                         blk_name(s->blk));
        }
    }
}

/* Returns true if the write was a command rather than plain data */
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
        /* programming can only clear bits */
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
        break;
    case NVRAM_FLASH_CMD_READ_ARRAY:
        s->flash_cmd = NVRAM_FLASH_CMD_NONE;
        s->flash_status = 0;
        break;
    default:
        /* status and other commands leave the array untouched */
        break;
    }
    return true;
}

/* macio style NVRAM device */
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
    /* while a command is in flight the part answers with its status */
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
    .valid.max_access_size = 8,
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
 * A G5's flash NVRAM, as its firmware, Mac OS X and Linux keep it: two
 * 8 KB banks, the live one being the valid bank with the higher
 * generation. Each starts with a header partition (0x5a "nvram": Adler-32
 * of the bank from byte 20, then the generation), followed by the Open
 * Firmware variables in "common" and free space. Bank A is written,
 * bank B left erased.
 */
void pmac_format_nvram_core99(MacIONVRAMState *nvr)
{
    uint8_t *bank = nvr->data;
    ChrpNvramPartHdr *hdr = (ChrpNvramPartHdr *)bank;
    ChrpNvramPartHdr *common;
    int end;

    memset(nvr->data, 0xff, nvr->size);

    memset(bank, 0, 32);
    hdr->signature = OSX_NVRAM_SIGNATURE;
    pstrcpy(hdr->name, sizeof(hdr->name), "nvram");
    chrp_nvram_finish_partition(hdr, 32);

    common = (ChrpNvramPartHdr *)&bank[32];
    end = chrp_nvram_create_system_partition(&bank[32], DEF_SYSTEM_SIZE,
                                             MACIO_NVRAM_SIZE - 32);
    pstrcpy(common->name, sizeof(common->name), "common");
    chrp_nvram_finish_partition(common, end);
    chrp_nvram_create_free_partition(&bank[32 + end],
                                     MACIO_NVRAM_SIZE - 32 - end);

    stl_be_p(&bank[20], 1);
    stl_be_p(&bank[16], adler32(1, &bank[20], MACIO_NVRAM_SIZE - 20));

    if (nvr->blk &&
        blk_pwrite(nvr->blk, 0, nvr->size, nvr->data, 0) < 0) {
        error_report("%s: write of default NVRAM failed", blk_name(nvr->blk));
    }
}

/* Length of the CHRP partition at p, 0 if its header is not valid */
static uint32_t chrp_partition_len(const uint8_t *p)
{
    ChrpNvramPartHdr hdr;
    uint32_t len;

    memcpy(&hdr, p, sizeof(hdr));
    len = be16_to_cpu(hdr.len) << 4;
    chrp_nvram_finish_partition(&hdr, len);
    return hdr.checksum == p[1] ? len : 0;
}

/* Generation of a valid Core99 bank, 0 if it is not one */
static uint32_t core99_bank_generation(const uint8_t *bank)
{
    if (bank[0] != OSX_NVRAM_SIGNATURE || chrp_partition_len(bank) == 0 ||
        (uint32_t)ldl_be_p(&bank[16]) !=
        adler32(1, &bank[20], MACIO_NVRAM_SIZE - 20)) {
        return 0;
    }
    return ldl_be_p(&bank[20]);
}

/*
 * True if the bank the firmware will pick (the valid one with the higher
 * generation, B on a tie) is a valid bank whose partitions tile it and
 * include the Open Firmware "common" partition.
 */
bool pmac_nvram_core99_valid(MacIONVRAMState *nvr)
{
    uint32_t gen_a, gen_b, off, len;
    const uint8_t *bank;
    bool common = false;

    if (nvr->size != MACIO_NVRAM_FLASH_SIZE) {
        return false;
    }
    gen_a = core99_bank_generation(nvr->data);
    gen_b = core99_bank_generation(nvr->data + MACIO_NVRAM_SIZE);
    if (MAX(gen_a, gen_b) == 0) {
        return false;
    }
    bank = nvr->data + (gen_a > gen_b ? 0 : MACIO_NVRAM_SIZE);
    for (off = 0; off < MACIO_NVRAM_SIZE; off += len) {
        len = chrp_partition_len(&bank[off]);
        if (len == 0 || len > MACIO_NVRAM_SIZE - off) {
            return false;
        }
        if (bank[off] == CHRP_NVPART_SYSTEM &&
            !strncmp((const char *)&bank[off + 4], "common", 12)) {
            common = true;
        }
    }
    return common;
}

/* True if the -prom-env list sets the variable of the "name=value" var */
static bool prom_env_sets(const char *var)
{
    const char *eq = strchr(var, '=');
    unsigned int i;

    for (i = 0; eq && i < nb_prom_envs; i++) {
        if (!strncmp(prom_envs[i], var, eq - var + 1)) {
            return true;
        }
    }
    return false;
}

/*
 * Set the -prom-env variables in the "common" partition of the live bank
 * of a valid Core99 NVRAM, in place of any of the same name.
 */
void pmac_nvram_core99_set_prom_env(MacIONVRAMState *nvr)
{
    g_autofree uint8_t *buf = NULL;
    uint32_t gen_a, gen_b, off, len, size, pos, p, n;
    uint8_t *bank, *vars;
    unsigned int i;

    if (!nb_prom_envs || !pmac_nvram_core99_valid(nvr)) {
        return;
    }
    gen_a = core99_bank_generation(nvr->data);
    gen_b = core99_bank_generation(nvr->data + MACIO_NVRAM_SIZE);
    bank = nvr->data + (gen_a > gen_b ? 0 : MACIO_NVRAM_SIZE);
    for (off = 0; ; off += len) {
        len = chrp_partition_len(&bank[off]);
        if (bank[off] == CHRP_NVPART_SYSTEM &&
            !strncmp((const char *)&bank[off + 4], "common", 12)) {
            break;
        }
    }
    vars = &bank[off + sizeof(ChrpNvramPartHdr)];
    size = len - sizeof(ChrpNvramPartHdr);
    buf = g_malloc0(size);

    pos = 0;
    for (p = 0; p < size && vars[p]; p += n + 1) {
        n = strnlen((const char *)&vars[p], size - p);
        if (n == size - p) {
            break;
        }
        if (prom_env_sets((const char *)&vars[p])) {
            continue;
        }
        if (pos + n + 1 >= size) {
            goto full;
        }
        memcpy(&buf[pos], &vars[p], n + 1);
        pos += n + 1;
    }
    for (i = 0; i < nb_prom_envs; i++) {
        n = strlen(prom_envs[i]);
        if (pos + n + 1 >= size) {
            goto full;
        }
        memcpy(&buf[pos], prom_envs[i], n + 1);
        pos += n + 1;
    }

    memcpy(vars, buf, size);
    stl_be_p(&bank[16], adler32(1, &bank[20], MACIO_NVRAM_SIZE - 20));
    if (nvr->blk && blk_pwrite(nvr->blk, bank - nvr->data, MACIO_NVRAM_SIZE,
                               bank, 0) < 0) {
        error_report("%s: write of NVRAM failed", blk_name(nvr->blk));
    }
    return;

full:
    warn_report("NVRAM has no room for the -prom-env variables");
}

/*
 * Open, creating it erased if absent or empty, a default NVRAM backing
 * file. A file of any other size is left alone and NULL returned.
 */
BlockBackend *macio_nvram_default_blk(const char *filename, uint32_t size,
                                      uint8_t fill)
{
    BlockBackend *blk;
    Error *local_err = NULL;
    QDict *options;
    struct stat st;
    int fd;

    fd = qemu_open_old(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        warn_report("could not open NVRAM image '%s': %s",
                    filename, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) < 0) {
        st.st_size = -1;
    }
    if (st.st_size == 0) {
        g_autofree uint8_t *blank = g_malloc(size);

        memset(blank, fill, size);
        if (write(fd, blank, size) != size) {
            st.st_size = -1;
        } else {
            st.st_size = size;
        }
    }
    close(fd);
    if (st.st_size != size) {
        warn_report("NVRAM image '%s' is not %u bytes, not using it",
                    filename, size);
        return NULL;
    }

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(filename, NULL, options, BDRV_O_RDWR, &local_err);
    if (!blk) {
        warn_report_err(local_err);
        return NULL;
    }
    return blk;
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
