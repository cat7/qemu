/*
 * QEMU ATAPI Emulation
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
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
#include "qemu/cutils.h"
#include "hw/scsi/scsi.h"
#include "system/block-backend.h"
#include "scsi/constants.h"
#include "ide-internal.h"
#include "trace.h"

#define ATAPI_SECTOR_BITS (2 + BDRV_SECTOR_BITS)
#define ATAPI_SECTOR_SIZE (1 << ATAPI_SECTOR_BITS)

static void ide_atapi_cmd_read_dma_cb(void *opaque, int ret);

static void padstr8(uint8_t *buf, int buf_size, const char *src)
{
    int i;
    for(i = 0; i < buf_size; i++) {
        if (*src)
            buf[i] = *src++;
        else
            buf[i] = ' ';
    }
}

static void lba_to_msf(uint8_t *buf, int lba)
{
    lba += 150;
    buf[0] = (lba / 75) / 60;
    buf[1] = (lba / 75) % 60;
    buf[2] = lba % 75;
}

static inline int media_present(IDEState *s)
{
    return !s->tray_open && s->nb_sectors > 0;
}

/* XXX: DVDs that could fit on a CD will be reported as a CD */
static inline int media_is_dvd(IDEState *s)
{
    return (media_present(s) && s->nb_sectors > CD_MAX_SECTORS);
}

static inline int media_is_cd(IDEState *s)
{
    return (media_present(s) && s->nb_sectors <= CD_MAX_SECTORS);
}

/* The medium has a backend-reported track layout. */
static inline bool media_has_toc(IDEState *s)
{
    return media_present(s) && s->cd_toc;
}

/* The drive reports CD-DA play and read capabilities. */
static bool atapi_audio_capable(IDEState *s)
{
    return s->cd_toc || ide_cd_audio_has_output(s);
}

static const uint8_t cd_audio_page_default[16] = {
    MODE_PAGE_AUDIO_CTL, 14, 0x04, 0, 0, 0, 0, 0,
    0x01, 0xff, 0x02, 0xff, 0, 0, 0, 0,
};

void ide_atapi_reset(IDEState *s)
{
    ide_cd_audio_stop(s);
    memcpy(s->cd_audio_page, cd_audio_page_default,
           sizeof(s->cd_audio_page));
    s->cd_raw_read = false;
    s->atapi_out_end = NULL;
}

void ide_atapi_media_changed(IDEState *s, bool load)
{
    CDToc toc;

    ide_cd_audio_stop(s);
    g_free(s->cd_toc);
    s->cd_toc = NULL;
    if (load && blk_get_cd_toc(s->blk, &toc) == 0 && toc.nb_tracks > 0) {
        s->cd_toc = g_memdup2(&toc, sizeof(toc));
    }
}

static const CDTrack *cd_track_at(IDEState *s, uint32_t lba, int *num)
{
    int i = cd_toc_find(s->cd_toc, lba);

    if (i < 0) {
        return NULL;
    }
    if (num) {
        *num = s->cd_toc->first + i;
    }
    return &s->cd_toc->tracks[i];
}

/* All sectors of [lba, lba + n) have track mode @mode. */
static bool cd_range_mode(IDEState *s, uint32_t lba, uint32_t n, int *mode)
{
    const CDToc *toc = s->cd_toc;
    int i = cd_toc_find(toc, lba);

    if (i < 0) {
        return false;
    }
    *mode = toc->tracks[i].mode;
    for (; i < toc->nb_tracks && toc->tracks[i].index0 < lba + n; i++) {
        if (toc->tracks[i].mode != *mode) {
            return false;
        }
    }
    return true;
}

static void cd_data_to_raw(uint8_t *buf, int lba)
{
    /* sync bytes */
    buf[0] = 0x00;
    memset(buf + 1, 0xff, 10);
    buf[11] = 0x00;
    buf += 12;
    /* MSF */
    lba_to_msf(buf, lba);
    buf[3] = 0x01; /* mode 1 data */
    buf += 4;
    /* data */
    buf += 2048;
    /* XXX: ECC not computed */
    memset(buf, 0, 288);
}

/* READ CD main channel selection (CDB byte 9) */
#define RCD_SYNC        0x80
#define RCD_SUBHEADER   0x40
#define RCD_HEADER      0x20
#define RCD_USER        0x10
#define RCD_EDC         0x08
#define RCD_C2          0x06
#define RCD_MAIN        0xf8

/* READ CD expected sector type (CDB byte 1) */
enum {
    RCD_TYPE_ANY,
    RCD_TYPE_CDDA,
    RCD_TYPE_MODE1,
    RCD_TYPE_MODE2,
    RCD_TYPE_FORM1,
    RCD_TYPE_FORM2,
};

/* Sector layouts */
enum {
    CD_KIND_CDDA,
    CD_KIND_MODE1,
    CD_KIND_MODE2,
    CD_KIND_FORM1,
    CD_KIND_FORM2,
};

/* Main channel fields of each sector layout, in order: start and end. */
static const struct {
    uint8_t field;
    uint16_t range[5][2];
} cd_fields[] = {
    { RCD_SYNC,      { {0, 0}, {0, 12}, {0, 12}, {0, 12}, {0, 12} } },
    { RCD_HEADER,    { {0, 0}, {12, 16}, {12, 16}, {12, 16}, {12, 16} } },
    { RCD_SUBHEADER, { {0, 0}, {16, 16}, {16, 16}, {16, 24}, {16, 24} } },
    { RCD_USER,      { {0, 2352}, {16, 2064}, {16, 2352}, {24, 2072},
                       {24, 2348} } },
    { RCD_EDC,       { {0, 0}, {2064, 2352}, {2352, 2352}, {2072, 2352},
                       {2348, 2352} } },
};

/* Bytes returned per sector of layout @kind, or -1 if invalid. */
static int cd_sector_bytes(int kind, uint8_t fields, uint8_t subch)
{
    int i, n = 0;

    if (kind == CD_KIND_CDDA) {
        n = (fields & RCD_MAIN) ? CD_RAW_SECTOR_SIZE : 0;
    } else {
        for (i = 0; i < ARRAY_SIZE(cd_fields); i++) {
            if (fields & cd_fields[i].field) {
                n += cd_fields[i].range[kind][1] - cd_fields[i].range[kind][0];
            }
        }
    }
    switch (fields & RCD_C2) {
    case 0x02:
        n += 294;
        break;
    case 0x04:
        n += 296;
        break;
    case 0x06:
        return -1;
    }
    switch (subch) {
    case 0:
        break;
    case 2:
        n += 16;
        break;
    default:
        return -1;
    }
    return n;
}

static uint8_t to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

/* Formatted Q sub-channel for @lba */
static void cd_subchannel_q(IDEState *s, uint32_t lba, uint8_t *q)
{
    int num = 1;
    const CDTrack *t = cd_track_at(s, lba, &num);
    uint32_t a = lba + 150;
    int32_t rel;

    memset(q, 0, 16);
    if (!t) {
        return;
    }
    rel = (int32_t)lba - (int32_t)t->start;
    if (rel < 0) {
        rel = -rel;
    }
    q[0] = 0x10 | t->control;
    q[1] = to_bcd(num);
    q[2] = lba < t->start ? 0 : 1;
    q[3] = to_bcd(rel / 75 / 60);
    q[4] = to_bcd((rel / 75) % 60);
    q[5] = to_bcd(rel % 75);
    q[7] = to_bcd(a / 75 / 60);
    q[8] = to_bcd((a / 75) % 60);
    q[9] = to_bcd(a % 75);
}

static int cd_sector_kind(IDEState *s, uint32_t lba, const uint8_t *raw)
{
    const CDTrack *t = cd_track_at(s, lba, NULL);

    if (!t || t->mode == CD_TRACK_AUDIO) {
        return CD_KIND_CDDA;
    }
    if (t->mode == CD_TRACK_MODE1) {
        return CD_KIND_MODE1;
    }
    switch (s->cd_read_type) {
    case RCD_TYPE_MODE2:
        return CD_KIND_MODE2;
    case RCD_TYPE_FORM1:
        return CD_KIND_FORM1;
    case RCD_TYPE_FORM2:
        return CD_KIND_FORM2;
    default:
        return raw && (raw[18] & 0x20) ? CD_KIND_FORM2 : CD_KIND_FORM1;
    }
}

/* Build the READ CD answer for one raw sector. */
static void cd_format_sector(IDEState *s, uint32_t lba, const uint8_t *raw,
                             uint8_t *out)
{
    int kind = cd_sector_kind(s, lba, raw);
    uint8_t fields = s->cd_read_fields;
    uint8_t *p = out;
    int i;

    if (kind == CD_KIND_CDDA) {
        if (fields & RCD_MAIN) {
            memcpy(p, raw, CD_RAW_SECTOR_SIZE);
            p += CD_RAW_SECTOR_SIZE;
        }
    } else {
        for (i = 0; i < ARRAY_SIZE(cd_fields); i++) {
            int a = cd_fields[i].range[kind][0];
            int b = cd_fields[i].range[kind][1];

            if (fields & cd_fields[i].field) {
                memcpy(p, raw + a, b - a);
                p += b - a;
            }
        }
    }
    switch (fields & RCD_C2) {
    case 0x02:
        memset(p, 0, 294);
        p += 294;
        break;
    case 0x04:
        memset(p, 0, 296);
        p += 296;
        break;
    }
    if (s->cd_read_subch == 2) {
        cd_subchannel_q(s, lba, p);
        p += 16;
    }
    /* sectors of a run share one size; pad or cut a stray layout */
    if (p - out < s->cd_sector_size) {
        memset(p, 0, s->cd_sector_size - (p - out));
    }
}

static void cd_read_formatted_cb(void *opaque, int ret)
{
    IDEState *s = opaque;
    uint8_t tmp[CD_RAW_SECTOR_SIZE + 296 + 16];
    int i;

    if (ret >= 0) {
        for (i = 0; i < s->cd_raw_n; i++) {
            cd_format_sector(s, s->cd_raw_lba + i,
                             s->cd_raw_buf + i * CD_RAW_SECTOR_SIZE, tmp);
            memcpy(s->io_buffer + i * s->cd_sector_size, tmp,
                   s->cd_sector_size);
        }
    }
    s->cd_raw_cb(s->cd_raw_opaque, ret);
}

/*
 * Read @nb_sectors raw sectors from @lba and format them for READ CD into
 * s->io_buffer, cd_sector_size bytes each.
 */
BlockAIOCB *ide_atapi_read_formatted(IDEState *s, int lba, int nb_sectors,
                                     BlockCompletionFunc *cb, void *opaque)
{
    assert(nb_sectors > 0 && nb_sectors <= CD_RAW_MAX_SECTORS);
    assert(nb_sectors * s->cd_sector_size <= s->io_buffer_total_len);

    if (!s->cd_raw_buf) {
        s->cd_raw_buf = blk_blockalign(s->blk,
                                       CD_RAW_MAX_SECTORS * CD_RAW_SECTOR_SIZE);
    }
    s->cd_raw_lba = lba;
    s->cd_raw_n = nb_sectors;
    s->cd_raw_cb = cb;
    s->cd_raw_opaque = opaque;
    qemu_iovec_init_buf(&s->cd_raw_qiov, s->cd_raw_buf,
                        nb_sectors * CD_RAW_SECTOR_SIZE);
    return ide_buffered_cd_read_raw(s, lba, &s->cd_raw_qiov, nb_sectors,
                                    cd_read_formatted_cb, s);
}

static void cd_read_sector_cb(void *opaque, int ret)
{
    IDEState *s = opaque;
    int et = s->elementary_transfer_size;
    int skip = s->io_buffer_index;
    int nsec = DIV_ROUND_UP(skip + et, s->cd_sector_size);
    uint8_t *buf;
    int i;

    trace_cd_read_sector_cb(s->lba, ret);

    if (ret < 0) {
        block_acct_failed(blk_get_stats(s->blk), &s->acct);
        ide_atapi_io_error(s, ret);
        return;
    }

    block_acct_done(blk_get_stats(s->blk), &s->acct);

    if (!s->cd_raw_read && s->cd_sector_size == 2352) {
        /* unpack back-to-front so a sector never clobbers an unmoved one */
        for (i = nsec - 1; i >= 0; i--) {
            memmove(s->io_buffer + i * 2352 + 16, s->io_buffer + i * 2048,
                    ATAPI_SECTOR_SIZE);
            cd_data_to_raw(s->io_buffer + i * 2352, s->lba + i);
        }
    }

    s->status &= ~BUSY_STAT;

    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
    s->lcyl = et & 0xff;
    s->hcyl = (et >> 8) & 0xff;
    ide_bus_set_irq(s->bus);

    /* a boundary sector shared with the next burst is re-read there */
    buf = s->io_buffer + skip;
    s->packet_transfer_size -= et;
    s->lba += (skip + et) / s->cd_sector_size;
    s->io_buffer_index = (skip + et) % s->cd_sector_size;
    s->elementary_transfer_size = 0;

    if (ide_transfer_start_norecurse(s, buf, et, ide_atapi_cmd_reply_end)) {
        ide_atapi_cmd_reply_end(s);
    }
}

/*
 * Read the whole elementary transfer (one DRQ burst) in a single async
 * request. No read is issued mid-burst, so unlike the old synchronous
 * rebuffer it cannot deadlock against a concurrent drain.
 */
static int cd_read_sector(IDEState *s)
{
    int et = s->elementary_transfer_size;
    int skip = s->io_buffer_index;
    int nsec = DIV_ROUND_UP(skip + et, s->cd_sector_size);

    if (s->cd_raw_read) {
        trace_cd_read_sector(s->lba);
        block_acct_start(blk_get_stats(s->blk), &s->acct,
                         nsec * CD_RAW_SECTOR_SIZE, BLOCK_ACCT_READ);
        ide_atapi_read_formatted(s, s->lba, nsec, cd_read_sector_cb, s);
        s->status |= BUSY_STAT;
        return 0;
    }

    if (s->cd_sector_size != 2048 && s->cd_sector_size != 2352) {
        block_acct_invalid(blk_get_stats(s->blk), BLOCK_ACCT_READ);
        return -EINVAL;
    }

    /* a burst is bounded by the byte count limit, so it fits io_buffer */
    assert(nsec * s->cd_sector_size <= s->io_buffer_total_len);

    /*
     * Read the payload packed at the front of io_buffer; the 2352 raw case is
     * unpacked into place on completion.
     */
    qemu_iovec_init_buf(&s->qiov, s->io_buffer, nsec * ATAPI_SECTOR_SIZE);

    trace_cd_read_sector(s->lba);

    block_acct_start(blk_get_stats(s->blk), &s->acct,
                     nsec * ATAPI_SECTOR_SIZE, BLOCK_ACCT_READ);

    ide_buffered_readv(s, (int64_t)s->lba << 2, &s->qiov, nsec * 4,
                       cd_read_sector_cb, s);

    s->status |= BUSY_STAT;
    return 0;
}

void ide_atapi_cmd_ok(IDEState *s)
{
    s->error = 0;
    s->status = READY_STAT | SEEK_STAT;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    ide_transfer_stop(s);
    ide_bus_set_irq(s->bus);
}

void ide_atapi_cmd_error(IDEState *s, int sense_key, int asc)
{
    trace_ide_atapi_cmd_error(s, sense_key, asc);
    s->error = sense_key << 4;
    s->status = READY_STAT | ERR_STAT;
    s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
    s->sense_key = sense_key;
    s->asc = asc;
    ide_transfer_stop(s);
    ide_bus_set_irq(s->bus);
}

void ide_atapi_io_error(IDEState *s, int ret)
{
    /* XXX: handle more errors */
    if (ret == -ENOMEDIUM) {
        ide_atapi_cmd_error(s, NOT_READY,
                            ASC_MEDIUM_NOT_PRESENT);
    } else {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_LOGICAL_BLOCK_OOR);
    }
}

static uint16_t atapi_byte_count_limit(IDEState *s)
{
    uint16_t bcl;

    bcl = s->lcyl | (s->hcyl << 8);
    if (bcl == 0xffff) {
        return 0xfffe;
    }
    return bcl;
}

/* The whole ATAPI transfer logic is handled in this function */
void ide_atapi_cmd_reply_end(IDEState *s)
{
    int byte_count_limit, size, ret;

    trace_ide_atapi_cmd_reply_end(s, s->packet_transfer_size,
                                  s->elementary_transfer_size,
                                  s->io_buffer_index);

    if (s->lba != -1 && s->packet_transfer_size > 0) {
        byte_count_limit = atapi_byte_count_limit(s);
        trace_ide_atapi_cmd_reply_end_bcl(s, byte_count_limit);
        size = s->packet_transfer_size;
        if (size > byte_count_limit) {
            /* byte count limit must be even if this case */
            if (byte_count_limit & 1) {
                byte_count_limit--;
            }
            size = byte_count_limit;
        }
        if (s->cd_raw_read) {
            int max = CD_RAW_MAX_SECTORS * s->cd_sector_size -
                      s->io_buffer_index;

            if (size > max) {
                size = max & ~1;
            }
        }
        s->elementary_transfer_size = size;
        ret = cd_read_sector(s);
        if (ret < 0) {
            ide_atapi_io_error(s, ret);
        }
        return;
    }

    while (s->packet_transfer_size > 0) {
        /* a new transfer is needed */
        s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO;
        ide_bus_set_irq(s->bus);
        byte_count_limit = atapi_byte_count_limit(s);
        trace_ide_atapi_cmd_reply_end_bcl(s, byte_count_limit);
        size = s->packet_transfer_size;
        if (size > byte_count_limit) {
            /* byte count limit must be even if this case */
            if (byte_count_limit & 1) {
                byte_count_limit--;
            }
            size = byte_count_limit;
        }
        s->lcyl = size & 0xff;
        s->hcyl = size >> 8;
        s->elementary_transfer_size = size;
        trace_ide_atapi_cmd_reply_end_new(s, s->status);

        s->packet_transfer_size -= size;
        s->elementary_transfer_size -= size;
        s->io_buffer_index += size;
        assert(size <= s->io_buffer_total_len);
        assert(s->io_buffer_index <= s->io_buffer_total_len);

        /* Some adapters process PIO data right away.  In that case, we need
         * to avoid mutual recursion between ide_transfer_start
         * and ide_atapi_cmd_reply_end.
         */
        if (!ide_transfer_start_norecurse(s,
                                          s->io_buffer + s->io_buffer_index - size,
                                          size, ide_atapi_cmd_reply_end)) {
            return;
        }
    }

    /* end of transfer */
    trace_ide_atapi_cmd_reply_end_eot(s, s->status);
    ide_atapi_cmd_ok(s);
    ide_bus_set_irq(s->bus);
}

/* send a reply of 'size' bytes in s->io_buffer to an ATAPI command */
static void ide_atapi_cmd_reply(IDEState *s, int size, int max_size)
{
    if (size > max_size)
        size = max_size;
    s->lba = -1; /* no sector read */
    s->packet_transfer_size = size;
    s->io_buffer_size = size;    /* dma: send the reply data as one chunk */
    s->elementary_transfer_size = 0;

    if (s->atapi_dma) {
        block_acct_start(blk_get_stats(s->blk), &s->acct, size,
                         BLOCK_ACCT_READ);
        s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
        ide_start_dma(s, ide_atapi_cmd_read_dma_cb);
    } else {
        s->status = READY_STAT | SEEK_STAT;
        s->io_buffer_index = 0;
        ide_atapi_cmd_reply_end(s);
    }
}

static void ide_atapi_cmd_write_dma_cb(void *opaque, int ret)
{
    IDEState *s = opaque;
    EndTransferFunc *end = s->atapi_out_end;

    s->atapi_out_end = NULL;
    s->io_buffer_index = 0;
    if (ret < 0 || s->bus->dma->ops->rw_buf(s->bus->dma, 0) == 0) {
        ide_set_inactive(s, false);
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_PARAMETER_LIST_LENGTH_ERROR);
        return;
    }
    ide_set_inactive(s, false);
    end(s);
}

static void ide_atapi_data_out(IDEState *s, int len, EndTransferFunc *end)
{
    s->lba = -1;
    s->packet_transfer_size = len;
    s->elementary_transfer_size = 0;
    s->io_buffer_index = 0;
    s->io_buffer_size = len;

    if (s->atapi_dma) {
        s->atapi_out_end = end;
        s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
        ide_start_dma(s, ide_atapi_cmd_write_dma_cb);
    } else {
        s->status = READY_STAT | SEEK_STAT;
        s->nsector = s->nsector & ~7;   /* data to the device */
        s->lcyl = len & 0xff;
        s->hcyl = len >> 8;
        ide_transfer_start(s, s->io_buffer, len, end);
        ide_bus_set_irq(s->bus);
    }
}

/* start a CD-ROM read command */
static void ide_atapi_cmd_read_pio(IDEState *s, int lba, int nb_sectors,
                                   int sector_size)
{
    assert(0 <= lba && lba < (s->nb_sectors >> 2));

    s->lba = lba;
    s->packet_transfer_size = nb_sectors * sector_size;
    s->elementary_transfer_size = 0;
    s->io_buffer_index = 0;
    s->cd_sector_size = sector_size;

    ide_atapi_cmd_reply_end(s);
}

static void ide_atapi_cmd_check_status(IDEState *s)
{
    trace_ide_atapi_cmd_check_status(s);
    s->error = MC_ERR | (UNIT_ATTENTION << 4);
    s->status = ERR_STAT;
    s->nsector = 0;
    ide_bus_set_irq(s->bus);
}
/* ATAPI DMA support */

static void ide_atapi_cmd_read_dma_cb(void *opaque, int ret)
{
    IDEState *s = opaque;
    int data_offset, n;

    if (ret < 0) {
        if (ide_handle_rw_error(s, -ret, ide_dma_cmd_to_retry(s->dma_cmd))) {
            if (s->bus->error_status) {
                s->bus->dma->aiocb = NULL;
                return;
            }
            goto eot;
        }
    }

    if (s->io_buffer_size > 0) {
        /*
         * For a cdrom read sector command (s->lba != -1),
         * adjust the lba for the next s->io_buffer_size chunk
         * and dma the current chunk.
         * For a command != read (s->lba == -1), just transfer
         * the reply data.
         */
        if (s->lba != -1) {
            if (s->cd_raw_read) {
                n = s->io_buffer_size / s->cd_sector_size;
            } else if (s->cd_sector_size == 2352) {
                n = 1;
                cd_data_to_raw(s->io_buffer, s->lba);
            } else {
                n = s->io_buffer_size >> 11;
            }
            s->lba += n;
        }
        s->packet_transfer_size -= s->io_buffer_size;
        if (s->bus->dma->ops->rw_buf(s->bus->dma, 1) == 0)
            goto eot;
    }

    if (s->packet_transfer_size <= 0) {
        s->status = READY_STAT | SEEK_STAT;
        s->nsector = (s->nsector & ~7) | ATAPI_INT_REASON_IO | ATAPI_INT_REASON_CD;
        ide_bus_set_irq(s->bus);
        goto eot;
    }

    s->io_buffer_index = 0;
    if (s->cd_raw_read) {
        n = MIN(s->packet_transfer_size / s->cd_sector_size,
                CD_RAW_MAX_SECTORS);
        s->io_buffer_size = n * s->cd_sector_size;
        trace_ide_atapi_cmd_read_dma_cb_aio(s, s->lba, n);
        s->bus->dma->aiocb = ide_atapi_read_formatted(s, s->lba, n,
                                                      ide_atapi_cmd_read_dma_cb,
                                                      s);
        return;
    } else if (s->cd_sector_size == 2352) {
        n = 1;
        s->io_buffer_size = s->cd_sector_size;
        data_offset = 16;
    } else {
        n = s->packet_transfer_size >> 11;
        if (n > (IDE_DMA_BUF_SECTORS / 4))
            n = (IDE_DMA_BUF_SECTORS / 4);
        s->io_buffer_size = n * 2048;
        data_offset = 0;
    }
    trace_ide_atapi_cmd_read_dma_cb_aio(s, s->lba, n);
    qemu_iovec_init_buf(&s->bus->dma->qiov, s->io_buffer + data_offset,
                        n * ATAPI_SECTOR_SIZE);

    s->bus->dma->aiocb = ide_buffered_readv(s, (int64_t)s->lba << 2,
                                            &s->bus->dma->qiov, n * 4,
                                            ide_atapi_cmd_read_dma_cb, s);
    return;

eot:
    if (ret < 0) {
        block_acct_failed(blk_get_stats(s->blk), &s->acct);
    } else {
        block_acct_done(blk_get_stats(s->blk), &s->acct);
    }
    ide_set_inactive(s, false);
}

/* start a CD-ROM read command with DMA */
/* XXX: test if DMA is available */
static void ide_atapi_cmd_read_dma(IDEState *s, int lba, int nb_sectors,
                                   int sector_size)
{
    assert(0 <= lba && lba < (s->nb_sectors >> 2));

    s->lba = lba;
    s->packet_transfer_size = nb_sectors * sector_size;
    s->io_buffer_size = 0;
    s->cd_sector_size = sector_size;

    block_acct_start(blk_get_stats(s->blk), &s->acct, s->packet_transfer_size,
                     BLOCK_ACCT_READ);

    /* XXX: check if BUSY_STAT should be set */
    s->status = READY_STAT | SEEK_STAT | DRQ_STAT | BUSY_STAT;
    ide_start_dma(s, ide_atapi_cmd_read_dma_cb);
}

static void ide_atapi_cmd_read(IDEState *s, int lba, int nb_sectors,
                               int sector_size)
{
    trace_ide_atapi_cmd_read(s, s->atapi_dma ? "dma" : "pio",
                             lba, nb_sectors);
    if (s->atapi_dma) {
        ide_atapi_cmd_read_dma(s, lba, nb_sectors, sector_size);
    } else {
        ide_atapi_cmd_read_pio(s, lba, nb_sectors, sector_size);
    }
}

void ide_atapi_dma_restart(IDEState *s)
{
    /*
     * At this point we can just re-evaluate the packet command and start over.
     * The presence of ->dma_cb callback in the pre_save ensures that the packet
     * command has been completely sent and we can safely restart command.
     */
    s->unit = s->bus->retry_unit;
    s->bus->dma->ops->restart_dma(s->bus->dma);
    ide_atapi_cmd(s);
}

static int ide_dvd_read_structure(IDEState *s, int format,
                                  const uint8_t *packet, uint8_t *buf)
{
    switch (format) {
        case 0x0: /* Physical format information */
            {
                int layer = packet[6];
                uint64_t total_sectors;

                if (layer != 0)
                    return -ASC_INV_FIELD_IN_CMD_PACKET;

                total_sectors = s->nb_sectors >> 2;
                if (total_sectors == 0) {
                    return -ASC_MEDIUM_NOT_PRESENT;
                }

                buf[4] = 1;   /* DVD-ROM, part version 1 */
                buf[5] = 0xf; /* 120mm disc, minimum rate unspecified */
                buf[6] = 1;   /* one layer, read-only (per MMC-2 spec) */
                buf[7] = 0;   /* default densities */

                /* FIXME: 0x30000 per spec? */
                stl_be_p(buf + 8, 0); /* start sector */
                stl_be_p(buf + 12, total_sectors - 1); /* end sector */
                stl_be_p(buf + 16, total_sectors - 1); /* l0 end sector */

                /* Size of buffer, not including 2 byte size field */
                stw_be_p(buf, 2048 + 2);

                /* 2k data + 4 byte header */
                return (2048 + 4);
            }

        case 0x01: /* DVD copyright information */
            buf[4] = 0; /* no copyright data */
            buf[5] = 0; /* no region restrictions */

            /* Size of buffer, not including 2 byte size field */
            stw_be_p(buf, 4 + 2);

            /* 4 byte header + 4 byte data */
            return (4 + 4);

        case 0x03: /* BCA information - invalid field for no BCA info */
            return -ASC_INV_FIELD_IN_CMD_PACKET;

        case 0x04: /* DVD disc manufacturing information */
            /* Size of buffer, not including 2 byte size field */
            stw_be_p(buf, 2048 + 2);

            /* 2k data + 4 byte header */
            return (2048 + 4);

        case 0xff:
            /*
             * This lists all the command capabilities above.  Add new ones
             * in order and update the length and buffer return values.
             */

            buf[4] = 0x00; /* Physical format */
            buf[5] = 0x40; /* Not writable, is readable */
            stw_be_p(buf + 6, 2048 + 4);

            buf[8] = 0x01; /* Copyright info */
            buf[9] = 0x40; /* Not writable, is readable */
            stw_be_p(buf + 10, 4 + 4);

            buf[12] = 0x03; /* BCA info */
            buf[13] = 0x40; /* Not writable, is readable */
            stw_be_p(buf + 14, 188 + 4);

            buf[16] = 0x04; /* Manufacturing info */
            buf[17] = 0x40; /* Not writable, is readable */
            stw_be_p(buf + 18, 2048 + 4);

            /* Size of buffer, not including 2 byte size field */
            stw_be_p(buf, 16 + 2);

            /* data written + 4 byte header */
            return (16 + 4);

        default: /* TODO: formats beyond DVD-ROM requires */
            return -ASC_INV_FIELD_IN_CMD_PACKET;
    }
}

static unsigned int event_status_media(IDEState *s,
                                       uint8_t *buf)
{
    uint8_t event_code, media_status;

    media_status = 0;
    if (s->tray_open) {
        media_status = MS_TRAY_OPEN;
    } else if (blk_is_inserted(s->blk)) {
        media_status = MS_MEDIA_PRESENT;
    }

    /* Event notification descriptor */
    event_code = MEC_NO_CHANGE;
    if (media_status != MS_TRAY_OPEN) {
        if (s->events.new_media) {
            event_code = MEC_NEW_MEDIA;
            s->events.new_media = false;
        } else if (s->events.eject_request) {
            event_code = MEC_EJECT_REQUESTED;
            s->events.eject_request = false;
        }
    }

    buf[4] = event_code;
    buf[5] = media_status;

    /* These fields are reserved, just clear them. */
    buf[6] = 0;
    buf[7] = 0;

    return 8; /* We wrote to 4 extra bytes from the header */
}

/*
 * Before transferring data or otherwise signalling acceptance of a command
 * marked CONDDATA, we must check the validity of the byte_count_limit.
 */
static bool validate_bcl(IDEState *s)
{
    /* TODO: Check IDENTIFY data word 125 for defacult BCL (currently 0) */
    if (s->atapi_dma || atapi_byte_count_limit(s)) {
        return true;
    }

    /* TODO: Move abort back into core.c and introduce proper error flow between
     *       ATAPI layer and IDE core layer */
    ide_abort_command(s);
    return false;
}

static void cmd_get_event_status_notification(IDEState *s,
                                              uint8_t *buf)
{
    const uint8_t *packet = buf;

    struct {
        uint8_t opcode;
        uint8_t polled;        /* lsb bit is polled; others are reserved */
        uint8_t reserved2[2];
        uint8_t class;
        uint8_t reserved3[2];
        uint16_t len;
        uint8_t control;
    } QEMU_PACKED *gesn_cdb;

    struct {
        uint16_t len;
        uint8_t notification_class;
        uint8_t supported_events;
    } QEMU_PACKED *gesn_event_header;
    unsigned int max_len, used_len;

    gesn_cdb = (void *)packet;
    gesn_event_header = (void *)buf;

    max_len = be16_to_cpu(gesn_cdb->len);

    /* It is fine by the MMC spec to not support async mode operations */
    if (!(gesn_cdb->polled & 0x01)) { /* asynchronous mode */
        /* Only polling is supported, asynchronous mode is not. */
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    /* polling mode operation */

    /*
     * These are the supported events.
     *
     * We currently only support requests of the 'media' type.
     * Notification class requests and supported event classes are bitmasks,
     * but they are build from the same values as the "notification class"
     * field.
     */
    gesn_event_header->supported_events = 1 << GESN_MEDIA;

    /*
     * We use |= below to set the class field; other bits in this byte
     * are reserved now but this is useful to do if we have to use the
     * reserved fields later.
     */
    gesn_event_header->notification_class = 0;

    /*
     * Responses to requests are to be based on request priority.  The
     * notification_class_request_type enum above specifies the
     * priority: upper elements are higher prio than lower ones.
     */
    if (gesn_cdb->class & (1 << GESN_MEDIA)) {
        gesn_event_header->notification_class |= GESN_MEDIA;
        used_len = event_status_media(s, buf);
    } else {
        gesn_event_header->notification_class = 0x80; /* No event available */
        used_len = sizeof(*gesn_event_header);
    }
    gesn_event_header->len = cpu_to_be16(used_len
                                         - sizeof(*gesn_event_header));
    ide_atapi_cmd_reply(s, used_len, max_len);
}

static void cmd_request_sense(IDEState *s, uint8_t *buf)
{
    int max_len = buf[4];

    memset(buf, 0, 18);
    buf[0] = 0x70 | (1 << 7);
    buf[2] = s->sense_key;
    buf[7] = 10;
    buf[12] = s->asc;

    if (s->sense_key == UNIT_ATTENTION) {
        s->sense_key = NO_SENSE;
    }

    ide_atapi_cmd_reply(s, 18, max_len);
}

static void cmd_inquiry(IDEState *s, uint8_t *buf)
{
    uint8_t page_code = buf[2];
    int max_len = buf[4];

    unsigned idx = 0;
    unsigned size_idx;
    unsigned preamble_len;
    unsigned pad_to = 0;

    /* If the EVPD (Enable Vital Product Data) bit is set in byte 1,
     * we are being asked for a specific page of info indicated by byte 2. */
    if (buf[1] & 0x01) {
        preamble_len = 4;
        size_idx = 3;

        buf[idx++] = 0x05;      /* CD-ROM */
        buf[idx++] = page_code; /* Page Code */
        buf[idx++] = 0x00;      /* reserved */
        idx++;                  /* length (set later) */

        switch (page_code) {
        case 0x00:
            /* Supported Pages: List of supported VPD responses. */
            buf[idx++] = 0x00; /* 0x00: Supported Pages, and: */
            buf[idx++] = 0x83; /* 0x83: Device Identification. */
            break;

        case 0x83:
            /* Device Identification. Each entry is optional, but the entries
             * included here are modeled after libata's VPD responses.
             * If the response is given, at least one entry must be present. */

            /* Entry 1: Serial */
            if (idx + 24 > max_len) {
                /* Not enough room for even the first entry: */
                /* 4 byte header + 20 byte string */
                ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                    ASC_DATA_PHASE_ERROR);
                return;
            }
            buf[idx++] = 0x02; /* Ascii */
            buf[idx++] = 0x00; /* Vendor Specific */
            buf[idx++] = 0x00;
            buf[idx++] = 20;   /* Remaining length */
            padstr8(buf + idx, 20, s->drive_serial_str);
            idx += 20;

            /* Entry 2: Drive Model and Serial */
            if (idx + 72 > max_len) {
                /* 4 (header) + 8 (vendor) + 60 (model & serial) */
                goto out;
            }
            buf[idx++] = 0x02; /* Ascii */
            buf[idx++] = 0x01; /* T10 Vendor */
            buf[idx++] = 0x00;
            buf[idx++] = 68;
            padstr8(buf + idx, 8, "ATA"); /* Generic T10 vendor */
            idx += 8;
            padstr8(buf + idx, 40, s->drive_model_str);
            idx += 40;
            padstr8(buf + idx, 20, s->drive_serial_str);
            idx += 20;

            /* Entry 3: WWN */
            if (s->wwn && (idx + 12 <= max_len)) {
                /* 4 byte header + 8 byte wwn */
                buf[idx++] = 0x01; /* Binary */
                buf[idx++] = 0x03; /* NAA */
                buf[idx++] = 0x00;
                buf[idx++] = 0x08;
                stq_be_p(&buf[idx], s->wwn);
                idx += 8;
            }
            break;

        default:
            /* SPC-3, revision 23 sec. 6.4 */
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_INV_FIELD_IN_CMD_PACKET);
            return;
        }
    } else {
        preamble_len = 5;
        size_idx = 4;

        buf[0] = 0x05; /* CD-ROM */
        buf[1] = 0x80; /* removable */
        buf[2] = 0x00; /* ISO */
        buf[3] = 0x21; /* ATAPI-2 (XXX: put ATAPI-4 ?) */
        /* buf[size_idx] set below. */
        buf[5] = 0;    /* reserved */
        buf[6] = 0;    /* reserved */
        buf[7] = 0;    /* reserved */
        /*
         * Keep the stock QEMU identity. An earlier Apple-compatibility
         * quirk impersonated a real 1993 Sony CDU-8003A CD-ROM here
         * (mirroring DingusPPC), but Mac OS X 10.4 responds to that
         * identity by treating the drive as vintage CD hardware: paging
         * in files that live beyond the CD-sized region of a DVD-sized
         * install image fails without the device ever being asked, which
         * killed the installer's WindowServer in _PEFExamineFile with
         * KERN_MEMORY_ERROR on every boot (verified A/B against the
         * 10.4.6 install image: Sony identity crashes, stock identity
         * reaches the installer GUI). Classic Mac OS's drive
         * qualification is satisfied by the Apple vendor mode pages
         * (0x30/0x31) below, which it requests explicitly and OS X
         * never touches.
         */
        padstr8(buf + 8, 8, "QEMU");
        padstr8(buf + 16, 16, "QEMU DVD-ROM");
        padstr8(buf + 32, 4, s->version);
        idx = 36;
        /*
         * Return the full allocation length, zero-padded past the
         * 36-byte standard page, as real drives do. Transferring only
         * the bytes we populate is spec-legal, but a driver that sized
         * its buffer from the allocation length can treat the short
         * data phase as an error after the fact: the command completes
         * normally, then the transferred count is compared against the
         * requested one and rewritten to an underrun status, so the
         * device is dropped with nothing logged. buf[size_idx] still
         * reports the true additional length.
         */
        pad_to = max_len;
    }

 out:
    buf[size_idx] = idx - preamble_len;
    if (pad_to > idx) {
        memset(buf + idx, 0, pad_to - idx);
        idx = pad_to;
    }
    ide_atapi_cmd_reply(s, idx, max_len);
}

/*
 * Write the descriptor for @feature at @p and return its length, or 0 if
 * the drive does not have that feature. *@current is set when the feature
 * applies to the loaded medium.
 */
static int atapi_feature(IDEState *s, uint16_t feature, uint8_t *p,
                         bool *current)
{
    switch (feature) {
    case 0x0000: /* Profile List */
        stw_be_p(p, 0x0000);
        p[2] = 0x03; /* version 0, persistent, current */
        p[3] = 8;
        stw_be_p(p + 4, MMC_PROFILE_DVD_ROM);
        p[6] = media_is_dvd(s);
        p[7] = 0;
        stw_be_p(p + 8, MMC_PROFILE_CD_ROM);
        p[10] = media_is_cd(s);
        p[11] = 0;
        *current = true;
        return 12;

    case 0x0001: /* Core */
        stw_be_p(p, 0x0001);
        p[2] = 0x07; /* version 1, persistent, current */
        p[3] = 8;
        stl_be_p(p + 4, 0x00000002); /* ATAPI */
        p[8] = 0;
        p[9] = p[10] = p[11] = 0;
        *current = true;
        return 12;

    case 0x0003: /* Removable Medium */
        stw_be_p(p, 0x0003);
        p[2] = 0x03;
        p[3] = 4;
        p[4] = (1 << 5) | (1 << 3) | (1 << 0); /* tray, eject, lock */
        p[5] = p[6] = p[7] = 0;
        *current = true;
        return 8;

    case 0x001e: /* CD Read */
        if (!s->cd_toc) {
            return 0;
        }
        stw_be_p(p, 0x001e);
        p[2] = (2 << 2) | media_is_cd(s);
        p[3] = 4;
        p[4] = p[5] = p[6] = p[7] = 0;
        *current = media_is_cd(s);
        return 8;

    case 0x0103: /* CD External Audio Play */
        if (!atapi_audio_capable(s)) {
            return 0;
        }
        stw_be_p(p, 0x0103);
        p[2] = media_is_cd(s);
        p[3] = 4;
        p[4] = 0x03;    /* separate channel mute, separate volume */
        p[5] = 0;
        stw_be_p(p + 6, 256);
        *current = media_is_cd(s);
        return 8;

    case 0x0010: /* Random Readable */
        stw_be_p(p, 0x0010);
        p[2] = 0x03;
        p[3] = 8;
        stl_be_p(p + 4, 2048); /* logical block size */
        stw_be_p(p + 8, 1);    /* blocking */
        p[10] = 0;
        p[11] = 0;
        *current = media_present(s);
        return 12;

    default:
        return 0;
    }
}

static void cmd_get_configuration(IDEState *s, uint8_t *buf)
{
    static const uint16_t features[] = {
        0x0000, 0x0001, 0x0003, 0x0010, 0x001e, 0x0103,
    };
    uint16_t start = lduw_be_p(buf + 2);
    int rt = buf[1] & 0x03;
    int max_len = lduw_be_p(buf + 7);
    uint32_t len = 8;
    unsigned i;

    if (rt == 3) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    /* XXX: assume one sector */
    if (max_len > BDRV_SECTOR_SIZE) {
        max_len = BDRV_SECTOR_SIZE;
    }
    memset(buf, 0, BDRV_SECTOR_SIZE);

    /*
     * the number of sectors from the media tells us which profile
     * to use as current.  0 means there is no media
     */
    if (media_is_dvd(s)) {
        stw_be_p(buf + 6, MMC_PROFILE_DVD_ROM);
    } else if (media_is_cd(s)) {
        stw_be_p(buf + 6, MMC_PROFILE_CD_ROM);
    }

    /*
     * RT 0: all features from @start; RT 1: current features from
     * @start; RT 2: only @start. The answer is built in full and cut to
     * the allocation length on reply, so a short first request still
     * reports the total length.
     */
    for (i = 0; i < ARRAY_SIZE(features); i++) {
        bool current = false;
        int n;

        if (rt == 2 ? features[i] != start : features[i] < start) {
            continue;
        }
        n = atapi_feature(s, features[i], buf + len, &current);
        if (n == 0 || (rt == 1 && !current)) {
            continue;
        }
        len += n;
    }

    stl_be_p(buf, len - 4); /* data length */
    ide_atapi_cmd_reply(s, len, max_len);
}

/*
 * Write mode page @code at @p and return its length, or 0 if unknown.
 * @action is the page control field: current, changeable or default.
 */
static int atapi_mode_page(IDEState *s, int code, int action, uint8_t *p)
{
    switch (code) {
    case MODE_PAGE_R_W_ERROR: /* error recovery */
        p[0] = MODE_PAGE_R_W_ERROR;
        p[1] = 6;
        p[2] = 0x00;
        p[3] = 0x05;
        p[4] = p[5] = p[6] = p[7] = 0x00;
        return 8;

    case MODE_PAGE_AUDIO_CTL:
        memset(p, 0, 16);
        p[0] = MODE_PAGE_AUDIO_CTL;
        p[1] = 14;
        if (!atapi_audio_capable(s)) {
            return 16;
        }
        switch (action) {
        case 1:
            p[2] = 0x02;    /* SOTC */
            p[8] = 0x0f;
            p[9] = 0xff;
            p[10] = 0x0f;
            p[11] = 0xff;
            break;
        case 2:
            memcpy(p, cd_audio_page_default, 16);
            break;
        default:
            memcpy(p, s->cd_audio_page, 16);
            break;
        }
        return 16;

    case MODE_PAGE_CAPABILITIES:
        memset(p, 0, 22);
        p[0] = MODE_PAGE_CAPABILITIES;
        p[1] = 20;
        p[2] = 0x3b; /* read CDR/CDRW/DVDROM/DVDR/DVDRAM */
        p[3] = 0x00;
        /*
         * Claim PLAY_AUDIO capability (0x01) since some Linux
         * code checks for this to automount media.
         */
        p[4] = 0x71;
        p[5] = 3 << 5;
        p[6] = (1 << 0) | (1 << 3) | (1 << 5);
        if (s->tray_locked) {
            p[6] |= 1 << 1;
        }
        p[7] = 0x00; /* No volume & mute control, no changer */
        stw_be_p(&p[8], 704);  /* 4x read speed */
        p[10] = 0;             /* Two volume levels */
        p[11] = 2;
        if (atapi_audio_capable(s)) {
            p[5] |= 0x03;      /* CD-DA commands, stream accurate */
            p[7] |= 0x03;      /* separate volume and channel mute */
            stw_be_p(&p[10], 256);
        }
        stw_be_p(&p[12], 512); /* 512k buffer */
        stw_be_p(&p[14], 704); /* 4x read speed current */
        return 22;

    case MODE_PAGE_APPLE_VENDOR:
        /* Classic Mac OS drive qualification checks this signature. */
        memset(p, 0, 32);
        p[0] = MODE_PAGE_APPLE_VENDOR;
        p[1] = 30;
        memcpy(&p[10], "APPLE COMPUTER, INC   ", 22);
        return 32;

    case 0x31: /* Apple features */
        p[0] = 0x31;
        p[1] = 6;
        p[2] = '.';
        p[3] = 'A';
        p[4] = 'p';
        p[5] = 'p';
        p[6] = 0x00;
        p[7] = 0x00;
        return 8;

    default:
        return 0;
    }
}

static void atapi_mode_sense(IDEState *s, uint8_t *buf, bool ten)
{
    static const uint8_t all_pages[] = {
        MODE_PAGE_R_W_ERROR, MODE_PAGE_AUDIO_CTL, MODE_PAGE_CAPABILITIES,
        MODE_PAGE_APPLE_VENDOR, 0x31,
    };
    int action = buf[2] >> 6;
    int code = buf[2] & 0x3f;
    int head = ten ? 8 : 4;
    int max_len = ten ? lduw_be_p(buf + 7) : buf[4];
    int page = 0;
    int i;

    if (action == 3) { /* saved values */
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_SAVING_PARAMETERS_NOT_SUPPORTED);
        return;
    }

    memset(buf, 0, head);
    if (code == 0x3f) {
        for (i = 0; i < ARRAY_SIZE(all_pages); i++) {
            page += atapi_mode_page(s, all_pages[i], action,
                                    buf + head + page);
        }
    } else {
        page = atapi_mode_page(s, code, action, buf + head);
    }
    if (page == 0) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    if (action == 1) {
        /* Changeable values: only the audio control page has any. */
        for (i = 0; i < page;) {
            int len = buf[head + i + 1] + 2;

            if ((buf[head + i] & 0x3f) != MODE_PAGE_AUDIO_CTL) {
                memset(buf + head + i + 2, 0, len - 2);
            }
            i += len;
        }
    }

    if (ten) {
        stw_be_p(&buf[0], head + page - 2);
        buf[2] = 0x70; /* medium type */
    } else {
        buf[0] = head + page - 1;
        buf[1] = 0x70; /* medium type */
    }
    ide_atapi_cmd_reply(s, head + page, max_len);
}

static void cmd_mode_sense(IDEState *s, uint8_t *buf)
{
    atapi_mode_sense(s, buf, true);
}

static void cmd_mode_sense_6(IDEState *s, uint8_t *buf)
{
    atapi_mode_sense(s, buf, false);
}

static void atapi_mode_select_end(IDEState *s, bool ten)
{
    uint8_t *buf = s->io_buffer;
    int len = s->packet_transfer_size;
    int head = ten ? 8 : 4;
    int p;

    if (len < head) {
        goto bad_param;
    }
    p = head + (ten ? lduw_be_p(buf + 6) : buf[3]);
    while (p + 2 <= len) {
        int code = buf[p] & 0x3f;
        int plen = buf[p + 1] + 2;

        if (p + plen > len) {
            goto bad_param;
        }
        if (code == MODE_PAGE_AUDIO_CTL) {
            if (plen < 16) {
                goto bad_param;
            }
            s->cd_audio_page[2] = (s->cd_audio_page[2] & ~0x02) |
                                  (buf[p + 2] & 0x02);
            s->cd_audio_page[8] = buf[p + 8] & 0x0f;
            s->cd_audio_page[9] = buf[p + 9];
            s->cd_audio_page[10] = buf[p + 10] & 0x0f;
            s->cd_audio_page[11] = buf[p + 11];
        }
        p += plen;
    }
    ide_atapi_cmd_ok(s);
    return;

bad_param:
    ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_PARAMETER_LIST);
}

void ide_atapi_mode_select6_end(IDEState *s)
{
    atapi_mode_select_end(s, false);
}

void ide_atapi_mode_select10_end(IDEState *s)
{
    atapi_mode_select_end(s, true);
}

static void atapi_mode_select(IDEState *s, uint8_t *buf, bool ten)
{
    int len = ten ? lduw_be_p(buf + 7) : buf[4];

    if (len == 0) {
        ide_atapi_cmd_ok(s);
        return;
    }
    if (len > 512) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_PARAMETER_LIST_LENGTH_ERROR);
        return;
    }
    ide_atapi_data_out(s, len, ten ? ide_atapi_mode_select10_end
                                   : ide_atapi_mode_select6_end);
}

static void cmd_mode_select(IDEState *s, uint8_t *buf)
{
    atapi_mode_select(s, buf, true);
}

static void cmd_mode_select_6(IDEState *s, uint8_t *buf)
{
    atapi_mode_select(s, buf, false);
}

static void cmd_test_unit_ready(IDEState *s, uint8_t *buf)
{
    /* Not Ready Conditions are already handled in ide_atapi_cmd(), so if we
     * come here, we know that it's ready. */
    ide_atapi_cmd_ok(s);
}

static void cmd_prevent_allow_medium_removal(IDEState *s, uint8_t* buf)
{
    s->tray_locked = buf[4] & 1;
    blk_lock_medium(s->blk, buf[4] & 1);
    ide_atapi_cmd_ok(s);
}

static void cmd_read(IDEState *s, uint8_t* buf)
{
    unsigned int nb_sectors, lba;

    /* Total logical sectors of ATAPI_SECTOR_SIZE(=2048) bytes */
    uint64_t total_sectors = s->nb_sectors >> 2;

    if (buf[0] == GPCMD_READ_10) {
        nb_sectors = lduw_be_p(buf + 7);
    } else {
        nb_sectors = ldl_be_p(buf + 6);
    }
    if (nb_sectors == 0) {
        ide_atapi_cmd_ok(s);
        return;
    }

    lba = ldl_be_p(buf + 2);
    if (lba >= total_sectors || lba + nb_sectors - 1 >= total_sectors) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
        return;
    }

    if (media_has_toc(s)) {
        int mode;

        if (!cd_range_mode(s, lba, nb_sectors, &mode) ||
            mode == CD_TRACK_AUDIO) {
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_ILLEGAL_MODE_FOR_THIS_TRACK);
            return;
        }
    }

    ide_atapi_cmd_read(s, lba, nb_sectors, 2048);
}

/* READ CD of a medium with a track layout */
static void cmd_read_cd_toc(IDEState *s, uint8_t *buf, uint32_t lba,
                            uint32_t nb_sectors)
{
    int type = (buf[1] >> 2) & 7;
    uint8_t fields = buf[9] & 0xfe;
    uint8_t subch = buf[10] & 7;
    int mode, kind, size;

    if (!cd_range_mode(s, lba, nb_sectors, &mode)) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_ILLEGAL_MODE_FOR_THIS_TRACK);
        return;
    }
    switch (type) {
    case RCD_TYPE_ANY:
        break;
    case RCD_TYPE_CDDA:
        if (mode != CD_TRACK_AUDIO) {
            goto bad_mode;
        }
        break;
    case RCD_TYPE_MODE1:
        if (mode != CD_TRACK_MODE1) {
            goto bad_mode;
        }
        break;
    case RCD_TYPE_MODE2:
    case RCD_TYPE_FORM1:
    case RCD_TYPE_FORM2:
        if (mode != CD_TRACK_MODE2) {
            goto bad_mode;
        }
        break;
    default:
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    s->cd_read_type = type;
    s->cd_read_fields = fields;
    s->cd_read_subch = subch;
    kind = cd_sector_kind(s, lba, NULL);
    size = cd_sector_bytes(kind, fields, subch);
    if (size < 0) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }
    if (size == 0) {
        ide_atapi_cmd_ok(s);
        return;
    }
    if (!validate_bcl(s)) {
        return;
    }
    s->cd_raw_read = true;
    ide_atapi_cmd_read(s, lba, nb_sectors, size);
    return;

bad_mode:
    ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_ILLEGAL_MODE_FOR_THIS_TRACK);
}

static void cmd_read_cd(IDEState *s, uint8_t* buf)
{
    unsigned int nb_sectors, lba, transfer_request;

    /* Total logical sectors of ATAPI_SECTOR_SIZE(=2048) bytes */
    uint64_t total_sectors = s->nb_sectors >> 2;

    nb_sectors = (buf[6] << 16) | (buf[7] << 8) | buf[8];
    if (nb_sectors == 0) {
        ide_atapi_cmd_ok(s);
        return;
    }

    lba = ldl_be_p(buf + 2);
    if (lba >= total_sectors || lba + nb_sectors - 1 >= total_sectors) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
        return;
    }

    if (media_has_toc(s)) {
        cmd_read_cd_toc(s, buf, lba, nb_sectors);
        return;
    }

    transfer_request = buf[9] & 0xf8;
    if (transfer_request == 0x00) {
        /* nothing */
        ide_atapi_cmd_ok(s);
        return;
    }

    /* Check validity of BCL before transferring data */
    if (!validate_bcl(s)) {
        return;
    }

    switch (transfer_request) {
    case 0x10:
        /* normal read */
        ide_atapi_cmd_read(s, lba, nb_sectors, 2048);
        break;
    case 0xf8:
        /* read all data */
        ide_atapi_cmd_read(s, lba, nb_sectors, 2352);
        break;
    default:
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_INV_FIELD_IN_CMD_PACKET);
        break;
    }
}

static int msf_to_lba(const uint8_t *msf)
{
    return (msf[0] * 60 + msf[1]) * 75 + msf[2] - 150;
}

static void cmd_read_cd_msf(IDEState *s, uint8_t *buf)
{
    int start = msf_to_lba(buf + 3);
    int end = msf_to_lba(buf + 6);
    uint32_t n;

    if (start < 0 || end < start) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }
    n = end - start;
    /* rewrite as READ CD */
    stl_be_p(buf + 2, start);
    buf[6] = n >> 16;
    buf[7] = n >> 8;
    buf[8] = n;
    cmd_read_cd(s, buf);
}

static void cmd_seek(IDEState *s, uint8_t* buf)
{
    unsigned int lba;
    uint64_t total_sectors = s->nb_sectors >> 2;

    lba = ldl_be_p(buf + 2);
    if (lba >= total_sectors) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
        return;
    }

    ide_cd_audio_stop(s);
    ide_atapi_cmd_ok(s);
}

static void cmd_start_stop_unit(IDEState *s, uint8_t* buf)
{
    int sense;
    bool start = buf[4] & 1;
    bool loej = buf[4] & 2;     /* load on start, eject on !start */
    int pwrcnd = buf[4] & 0xf0;

    if (pwrcnd) {
        /* eject/load only happens for power condition == 0 */
        ide_atapi_cmd_ok(s);
        return;
    }

    if (!start) {
        ide_cd_audio_stop(s);
    }

    if (loej) {
        if (!start && !s->tray_open && s->tray_locked) {
            sense = blk_is_inserted(s->blk)
                ? NOT_READY : ILLEGAL_REQUEST;
            ide_atapi_cmd_error(s, sense, ASC_MEDIA_REMOVAL_PREVENTED);
            return;
        }

        if (s->tray_open != !start) {
            blk_eject(s->blk, !start);
            s->tray_open = !start;
        }
    }

    ide_atapi_cmd_ok(s);
}

static void cmd_mechanism_status(IDEState *s, uint8_t* buf)
{
    int max_len = lduw_be_p(buf + 8);

    stw_be_p(buf, 0);
    /* no current LBA */
    buf[2] = 0;
    buf[3] = 0;
    buf[4] = 0;
    buf[5] = 1;
    stw_be_p(buf + 6, 0);
    ide_atapi_cmd_reply(s, 8, max_len);
}

static uint8_t *cd_toc_addr(uint8_t *q, uint32_t lba, bool msf)
{
    if (msf) {
        q[0] = 0;
        lba_to_msf(q + 1, lba);
    } else {
        stl_be_p(q, lba);
    }
    return q + 4;
}

static uint8_t *cd_full_toc_entry(uint8_t *q, uint8_t control, uint8_t point,
                                  uint8_t pmin, uint8_t psec, uint8_t pframe)
{
    *q++ = 1;                   /* session */
    *q++ = 0x10 | control;      /* ADR 1 */
    *q++ = 0;                   /* TNO */
    *q++ = point;
    *q++ = 0;
    *q++ = 0;
    *q++ = 0;
    *q++ = 0;
    *q++ = pmin;
    *q++ = psec;
    *q++ = pframe;
    return q;
}

static int cd_read_toc(IDEState *s, int format, bool msf, int start,
                       uint8_t *buf)
{
    const CDToc *toc = s->cd_toc;
    int last = toc->first + toc->nb_tracks - 1;
    const CDTrack *lt = &toc->tracks[toc->nb_tracks - 1];
    uint8_t *q = buf + 4;
    uint8_t m[3];
    bool xa = false;
    int i, len;

    switch (format) {
    case 0:
        if (start > last && start != 0xaa) {
            return -1;
        }
        buf[2] = toc->first;
        buf[3] = last;
        for (i = 0; i < toc->nb_tracks; i++) {
            if (toc->first + i < start) {
                continue;
            }
            *q++ = 0;
            *q++ = 0x10 | toc->tracks[i].control;
            *q++ = toc->first + i;
            *q++ = 0;
            q = cd_toc_addr(q, toc->tracks[i].start, msf);
        }
        *q++ = 0;
        *q++ = 0x10 | lt->control;
        *q++ = 0xaa;
        *q++ = 0;
        q = cd_toc_addr(q, toc->leadout, msf);
        break;
    case 1:
        buf[2] = 1;
        buf[3] = 1;
        *q++ = 0;
        *q++ = 0x10 | toc->tracks[0].control;
        *q++ = toc->first;
        *q++ = 0;
        q = cd_toc_addr(q, toc->tracks[0].start, msf);
        break;
    case 2:
        for (i = 0; i < toc->nb_tracks; i++) {
            xa |= toc->tracks[i].mode == CD_TRACK_MODE2;
        }
        buf[2] = 1;
        buf[3] = 1;
        q = cd_full_toc_entry(q, toc->tracks[0].control, 0xa0,
                              toc->first, xa ? 0x20 : 0x00, 0);
        q = cd_full_toc_entry(q, lt->control, 0xa1, last, 0, 0);
        lba_to_msf(m, toc->leadout);
        q = cd_full_toc_entry(q, lt->control, 0xa2, m[0], m[1], m[2]);
        for (i = 0; i < toc->nb_tracks; i++) {
            lba_to_msf(m, toc->tracks[i].start);
            q = cd_full_toc_entry(q, toc->tracks[i].control, toc->first + i,
                                  m[0], m[1], m[2]);
        }
        break;
    default:
        return -1;
    }
    len = q - buf;
    stw_be_p(buf, len - 2);
    return len;
}

static void cmd_read_toc_pma_atip(IDEState *s, uint8_t* buf)
{
    int format, msf, start_track, len;
    int max_len;
    uint64_t total_sectors = s->nb_sectors >> 2;

    max_len = lduw_be_p(buf + 7);
    format = buf[9] >> 6;
    msf = (buf[1] >> 1) & 1;
    start_track = buf[6];

    if (media_has_toc(s)) {
        if (buf[2] & 0x0f) {
            format = buf[2] & 0x0f;
        }
        len = cd_read_toc(s, format, msf, start_track, buf);
        if (len < 0) {
            goto error_cmd;
        }
        ide_atapi_cmd_reply(s, len, max_len);
        return;
    }

    switch(format) {
    case 0:
        len = cdrom_read_toc(total_sectors, buf, msf, start_track);
        if (len < 0)
            goto error_cmd;
        ide_atapi_cmd_reply(s, len, max_len);
        break;
    case 1:
        /* multi session : only a single session defined */
        memset(buf, 0, 12);
        buf[1] = 0x0a;
        buf[2] = 0x01;
        buf[3] = 0x01;
        ide_atapi_cmd_reply(s, 12, max_len);
        break;
    case 2:
        len = cdrom_read_toc_raw(total_sectors, buf, msf, start_track);
        if (len < 0)
            goto error_cmd;
        ide_atapi_cmd_reply(s, len, max_len);
        break;
    default:
    error_cmd:
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_INV_FIELD_IN_CMD_PACKET);
    }
}

static void cmd_read_cdvd_capacity(IDEState *s, uint8_t* buf)
{
    uint64_t total_sectors = s->nb_sectors >> 2;

    /* NOTE: it is really the number of sectors minus 1 */
    stl_be_p(buf, total_sectors - 1);
    stl_be_p(buf + 4, 2048);
    ide_atapi_cmd_reply(s, 8, 8);
}

static void cmd_read_disc_information(IDEState *s, uint8_t* buf)
{
    uint8_t type = buf[1] & 7;
    uint32_t max_len = lduw_be_p(buf + 7);

    /* Types 1/2 are only defined for Blu-Ray.  */
    if (type != 0) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    memset(buf, 0, 34);
    buf[1] = 32;
    buf[2] = 0xe; /* last session complete, disc finalized */
    buf[3] = 1;   /* first track on disc */
    buf[4] = 1;   /* # of sessions */
    buf[5] = 1;   /* first track of last session */
    buf[6] = 1;   /* last track of last session */
    if (media_has_toc(s)) {
        buf[3] = s->cd_toc->first;
        buf[5] = s->cd_toc->first;
        buf[6] = s->cd_toc->first + s->cd_toc->nb_tracks - 1;
    }
    buf[7] = 0x20; /* unrestricted use */
    buf[8] = 0x00; /* CD-ROM or DVD-ROM */
    /* 9-10-11: most significant byte corresponding bytes 4-5-6 */
    /* 12-23: not meaningful for CD-ROM or DVD-ROM */
    /* 24-31: disc bar code */
    /* 32: disc application code */
    /* 33: number of OPC tables */

    ide_atapi_cmd_reply(s, 34, max_len);
}

static void cmd_read_dvd_structure(IDEState *s, uint8_t* buf)
{
    int max_len;
    int media = buf[1];
    int format = buf[7];
    int ret;

    max_len = lduw_be_p(buf + 8);

    if (format < 0xff) {
        if (media_is_cd(s)) {
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_INCOMPATIBLE_FORMAT);
            return;
        } else if (!media_present(s)) {
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_INV_FIELD_IN_CMD_PACKET);
            return;
        }
    }

    memset(buf, 0, max_len > IDE_DMA_BUF_SECTORS * BDRV_SECTOR_SIZE + 4 ?
           IDE_DMA_BUF_SECTORS * BDRV_SECTOR_SIZE + 4 : max_len);

    switch (format) {
        case 0x00 ... 0x7f:
        case 0xff:
            if (media == 0) {
                ret = ide_dvd_read_structure(s, format, buf, buf);

                if (ret < 0) {
                    ide_atapi_cmd_error(s, ILLEGAL_REQUEST, -ret);
                } else {
                    ide_atapi_cmd_reply(s, ret, max_len);
                }

                break;
            }
            /* TODO: BD support, fall through for now */

        /* Generic disk structures */
        case 0x80: /* TODO: AACS volume identifier */
        case 0x81: /* TODO: AACS media serial number */
        case 0x82: /* TODO: AACS media identifier */
        case 0x83: /* TODO: AACS media key block */
        case 0x90: /* TODO: List of recognized format layers */
        case 0xc0: /* TODO: Write protection status */
        default:
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_INV_FIELD_IN_CMD_PACKET);
            break;
    }
}

static void cmd_read_subchannel(IDEState *s, uint8_t *buf)
{
    int max_len = lduw_be_p(buf + 7);
    bool msf = buf[1] & 2;
    bool subq = buf[2] & 0x40;
    int format = buf[3];
    uint32_t pos = ide_cd_audio_position(s);
    const CDTrack *t;
    int num = 1, len = 4;

    if (subq && (format < 1 || format > 3)) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }

    memset(buf, 0, 24);
    buf[1] = ide_cd_audio_status(s);
    if (subq) {
        buf[4] = format;
        switch (format) {
        case 1: /* current position */
            t = s->cd_toc ? cd_track_at(s, pos, &num) : NULL;
            buf[5] = 0x10 | (t ? t->control : CD_CTRL_DATA);
            buf[6] = num;
            buf[7] = t && pos < t->start ? 0 : 1;
            cd_toc_addr(buf + 8, pos, msf);
            if (msf) {
                uint32_t rel = t ? abs((int32_t)(pos - t->start)) : pos;

                buf[12] = 0;
                buf[13] = rel / 75 / 60;
                buf[14] = (rel / 75) % 60;
                buf[15] = rel % 75;
            } else {
                stl_be_p(buf + 12, t ? pos - t->start : pos);
            }
            len = 16;
            break;
        case 3: /* ISRC */
            buf[5] = 0x10 | (s->cd_toc && buf[6] ? 0 : CD_CTRL_DATA);
            /* fall through */
        case 2: /* media catalog number: none */
            len = 24;
            break;
        }
        stw_be_p(buf + 2, len - 4);
    }
    ide_atapi_cmd_reply(s, len, max_len);
}

/* Start audio play of [start, end) */
static void atapi_play(IDEState *s, uint32_t start, uint32_t end)
{
    const CDTrack *t;
    int mode;

    if (end == start) {
        ide_atapi_cmd_ok(s);
        return;
    }
    if (end < start) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }
    if (!media_has_toc(s) || start >= s->cd_toc->leadout) {
        if (media_has_toc(s)) {
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
        } else {
            ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                                ASC_ILLEGAL_MODE_FOR_THIS_TRACK);
        }
        return;
    }
    if (end > s->cd_toc->leadout) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_LOGICAL_BLOCK_OOR);
        return;
    }
    t = cd_track_at(s, start, NULL);
    if (s->cd_audio_page[2] & 0x02) { /* SOTC */
        end = MIN(end, t->end);
    }
    if (!cd_range_mode(s, start, end - start, &mode) ||
        mode != CD_TRACK_AUDIO) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST,
                            ASC_ILLEGAL_MODE_FOR_THIS_TRACK);
        return;
    }
    ide_cd_audio_play(s, start, end);
    ide_atapi_cmd_ok(s);
}

static void cmd_play_audio(IDEState *s, uint8_t *buf)
{
    uint32_t lba = ldl_be_p(buf + 2);
    uint32_t len;

    if (buf[0] == GPCMD_PLAY_AUDIO_10) {
        len = lduw_be_p(buf + 7);
    } else {
        len = ldl_be_p(buf + 6);
    }
    if (lba == 0xffffffff) {
        lba = ide_cd_audio_position(s);
    }
    if (len == 0) {
        ide_atapi_cmd_ok(s);
        return;
    }
    atapi_play(s, lba, (uint64_t)lba + len > UINT32_MAX ? UINT32_MAX
                                                         : lba + len);
}

static void cmd_play_audio_msf(IDEState *s, uint8_t *buf)
{
    int start, end;

    if (buf[3] == 0xff && buf[4] == 0xff && buf[5] == 0xff) {
        start = ide_cd_audio_position(s);
    } else {
        start = msf_to_lba(buf + 3);
    }
    end = msf_to_lba(buf + 6);
    if (start < 0 || end < 0) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET);
        return;
    }
    atapi_play(s, start, end);
}

static void cmd_pause_resume(IDEState *s, uint8_t *buf)
{
    if (!ide_cd_audio_pause(s, !(buf[8] & 1))) {
        ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_COMMAND_SEQUENCE_ERROR);
        return;
    }
    ide_atapi_cmd_ok(s);
}

static void cmd_stop_play_scan(IDEState *s, uint8_t *buf)
{
    ide_cd_audio_stop(s);
    ide_atapi_cmd_ok(s);
}

static void cmd_set_speed(IDEState *s, uint8_t* buf)
{
    ide_atapi_cmd_ok(s);
}

enum {
    /*
     * Only commands flagged as ALLOW_UA are allowed to run under a
     * unit attention condition. (See MMC-5, section 4.1.6.1)
     */
    ALLOW_UA = 0x01,

    /*
     * Commands flagged with CHECK_READY can only execute if a medium is present.
     * Otherwise they report the Not Ready Condition. (See MMC-5, section
     * 4.1.8)
     */
    CHECK_READY = 0x02,

    /*
     * Commands flagged with NONDATA do not in any circumstances return
     * any data via ide_atapi_cmd_reply. These commands are exempt from
     * the normal byte_count_limit constraints.
     * See ATA8-ACS3 "7.21.5 Byte Count Limit"
     */
    NONDATA = 0x04,

    /*
     * CONDDATA implies a command that transfers data only conditionally based
     * on the presence of suboptions. It should be exempt from the BCL check at
     * command validation time, but it needs to be checked at the command
     * handler level instead.
     */
    CONDDATA = 0x08,
};

static const struct AtapiCmd {
    void (*handler)(IDEState *s, uint8_t *buf);
    int flags;
} atapi_cmd_table[0x100] = {
    [ 0x00 ] = { cmd_test_unit_ready,               CHECK_READY | NONDATA },
    [ 0x03 ] = { cmd_request_sense,                 ALLOW_UA },
    [ 0x12 ] = { cmd_inquiry,                       ALLOW_UA },
    [ 0x15 ] = { cmd_mode_select_6,                 0 },
    [ 0x1a ] = { cmd_mode_sense_6,                  0 },
    [ 0x1b ] = { cmd_start_stop_unit,               NONDATA }, /* [1] */
    [ 0x1e ] = { cmd_prevent_allow_medium_removal,  NONDATA },
    [ 0x25 ] = { cmd_read_cdvd_capacity,            CHECK_READY },
    [ 0x28 ] = { cmd_read, /* (10) */               CHECK_READY },
    [ 0x2b ] = { cmd_seek,                          CHECK_READY | NONDATA },
    [ 0x42 ] = { cmd_read_subchannel,               CHECK_READY },
    [ 0x43 ] = { cmd_read_toc_pma_atip,             CHECK_READY },
    [ 0x45 ] = { cmd_play_audio, /* (10) */         CHECK_READY | NONDATA },
    [ 0x47 ] = { cmd_play_audio_msf,                CHECK_READY | NONDATA },
    [ 0x4b ] = { cmd_pause_resume,                  CHECK_READY | NONDATA },
    [ 0x4e ] = { cmd_stop_play_scan,                CHECK_READY | NONDATA },
    [ 0x46 ] = { cmd_get_configuration,             ALLOW_UA },
    [ 0x4a ] = { cmd_get_event_status_notification, ALLOW_UA },
    [ 0x51 ] = { cmd_read_disc_information,         CHECK_READY },
    [ 0x55 ] = { cmd_mode_select, /* (10) */        0 },
    [ 0x5a ] = { cmd_mode_sense, /* (10) */         0 },
    [ 0xa5 ] = { cmd_play_audio, /* (12) */         CHECK_READY | NONDATA },
    [ 0xa8 ] = { cmd_read, /* (12) */               CHECK_READY },
    [ 0xad ] = { cmd_read_dvd_structure,            CHECK_READY },
    [ 0xb9 ] = { cmd_read_cd_msf,                   CHECK_READY | CONDDATA },
    [ 0xbb ] = { cmd_set_speed,                     NONDATA },
    [ 0xbd ] = { cmd_mechanism_status,              0 },
    [ 0xbe ] = { cmd_read_cd,                       CHECK_READY | CONDDATA },
    /* [1] handler detects and reports not ready condition itself */
};

void ide_atapi_cmd(IDEState *s)
{
    uint8_t *buf = s->io_buffer;
    const struct AtapiCmd *cmd = &atapi_cmd_table[s->io_buffer[0]];

    trace_ide_atapi_cmd(s, s->io_buffer[0]);

    if (trace_event_get_state_backends(TRACE_IDE_ATAPI_CMD_PACKET)) {
        g_autoptr(GString) str =
            qemu_hexdump_line(NULL, buf, ATAPI_PACKET_SIZE, 1, 0);
        trace_ide_atapi_cmd_packet(s, s->lcyl | (s->hcyl << 8), str->str);
    }

    /*
     * If there's a UNIT_ATTENTION condition pending, only command flagged with
     * ALLOW_UA are allowed to complete. with other commands getting a CHECK
     * condition response unless a higher priority status, defined by the drive
     * here, is pending.
     */
    if (s->sense_key == UNIT_ATTENTION && !(cmd->flags & ALLOW_UA)) {
        ide_atapi_cmd_check_status(s);
        return;
    }
    /*
     * When a CD gets changed, we have to report an ejected state and
     * then a loaded state to guests so that they detect tray
     * open/close and media change events.  Guests that do not use
     * GET_EVENT_STATUS_NOTIFICATION to detect such tray open/close
     * states rely on this behavior.
     */
    if (!(cmd->flags & ALLOW_UA) &&
        !s->tray_open && blk_is_inserted(s->blk) && s->cdrom_changed) {

        if (s->cdrom_changed == 1) {
            ide_atapi_cmd_error(s, NOT_READY, ASC_MEDIUM_NOT_PRESENT);
            s->cdrom_changed = 2;
        } else {
            ide_atapi_cmd_error(s, UNIT_ATTENTION, ASC_MEDIUM_MAY_HAVE_CHANGED);
            s->cdrom_changed = 0;
        }

        return;
    }

    /* Report a Not Ready condition if appropriate for the command */
    if ((cmd->flags & CHECK_READY) &&
        (!media_present(s) || !blk_is_inserted(s->blk)))
    {
        ide_atapi_cmd_error(s, NOT_READY, ASC_MEDIUM_NOT_PRESENT);
        return;
    }

    /* Commands that don't transfer DATA permit the byte_count_limit to be 0.
     * If this is a data-transferring PIO command and BCL is 0,
     * we abort at the /ATA/ level, not the ATAPI level.
     * See ATA8 ACS3 section 7.17.6.49 and 7.21.5 */
    if (cmd->handler && !(cmd->flags & (NONDATA | CONDDATA))) {
        if (!validate_bcl(s)) {
            return;
        }
    }

    /* Execute the command */
    if (cmd->handler) {
        s->cd_raw_read = false;
        cmd->handler(s, buf);
        return;
    }

    ide_atapi_cmd_error(s, ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE);
}
