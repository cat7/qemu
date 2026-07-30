/*
 * SWIM3 (Sander-Wozniak Integrated Machine 3) floppy controller
 *
 * The MMIO floppy controller in Old World PowerMac mac-io ASICs
 * (Heathrow on the Beige G3), fronting one internal Apple Superdrive.
 * Sixteen byte-wide registers at a 16-byte stride (matching Linux's
 * drivers/block/swim3.c layout); sector data moves through DBDMA
 * channel 1. Register semantics and the drive-side status/command
 * protocol follow DingusPPC's register-compatible swim3/superdrive
 * model, which boots real Mac OS; unlike that model, writing is
 * implemented here too.
 *
 * Only raw MFM images are supported (1474560 bytes = HD 1440K,
 * 737280 bytes = DD 720K). GCR 400K/800K images are rejected.
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/block/swim3.h"
#include "hw/ppc/mac_dbdma.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "trace.h"

/* register indexes (MMIO offset = index * 16) */
enum {
    SWIM3_REG_DATA        = 0,
    SWIM3_REG_TIMER       = 1,
    SWIM3_REG_ERROR       = 2,
    SWIM3_REG_PARAM       = 3,
    SWIM3_REG_PHASE       = 4,
    SWIM3_REG_SETUP       = 5,
    SWIM3_REG_STATUS      = 6,  /* read: status; write: clear mode bits */
    SWIM3_REG_HANDSHAKE   = 7,  /* read: handshake; write: set mode bits */
    SWIM3_REG_INT_FLAGS   = 8,
    SWIM3_REG_STEP        = 9,
    SWIM3_REG_CUR_TRACK   = 10,
    SWIM3_REG_CUR_SECTOR  = 11,
    SWIM3_REG_GAP_FORMAT  = 12,
    SWIM3_REG_FIRST_SEC   = 13,
    SWIM3_REG_XFER_CNT    = 14,
    SWIM3_REG_INT_MASK    = 15,
};

/* mode register bits */
#define SWIM3_INT_ENA     0x01
#define SWIM3_DRIVE_1     0x02
#define SWIM3_DRIVE_2     0x04
#define SWIM3_GO          0x08
#define SWIM3_WR_MODE     0x10
#define SWIM3_HEAD_SELECT 0x20
#define SWIM3_FORMAT_MODE 0x40
#define SWIM3_GO_STEP     0x80

/* interrupt flag bits */
#define INT_TIMER_DONE 0x01
#define INT_STEP_DONE  0x02
#define INT_ID_READ    0x04
#define INT_SECT_DONE  0x08
#define INT_SENSE      0x10
#define INT_ERROR      0x20

/* drive status addresses (Apple Superdrive interface) */
enum {
    STA_STEP_STATUS   = 1,
    STA_MOTOR_STATUS  = 2,
    STA_EJECT_LATCH   = 3,
    STA_SELECT_HEAD_0 = 4,
    STA_MFM_SUPPORT   = 5,
    STA_DOUBLE_SIDED  = 6,
    STA_DRIVE_EXISTS  = 7,
    STA_DISK_IN_DRIVE = 8,
    STA_WRITE_PROTECT = 9,
    STA_TRACK_ZERO    = 0xA,
    STA_SELECT_HEAD_1 = 0xC,
    STA_DRIVE_MODE    = 0xD,
    STA_DRIVE_READY   = 0xE,
    STA_MEDIA_KIND    = 0xF,
};

/* drive command addresses */
enum {
    CMD_STEP_DIRECTION    = 0,
    CMD_DO_STEP           = 1,
    CMD_MOTOR_ON_OFF      = 2,
    CMD_EJECT_DISK        = 3,
    CMD_RESET_EJECT_LATCH = 4,
    CMD_SWITCH_DRIVE_MODE = 5,
};

/* disk access state machine */
enum {
    ACCESS_IDLE,
    ACCESS_MARK_SEARCH,
    ACCESS_DATA_XFER,
};

/*
 * MFM rotational timing: reading a byte off the disk takes 8 bits at
 * 2 us/bit. Grossly realistic pacing keeps the guest driver's state
 * machine honest without being slow: an HD track holds 18 sectors at
 * 300 rpm (200 ms/rev -> ~11 ms/sector including gaps).
 */
#define MFM_BYTES_TO_NS(bytes) ((int64_t)(bytes) * 8 * 2 * 1000)
#define MFM_ADDR_MARK_NS  MFM_BYTES_TO_NS(22)
#define MFM_SECT_DATA_NS  MFM_BYTES_TO_NS(514)
#define MFM_HD_SECTOR_NS  MFM_BYTES_TO_NS(675)
#define MFM_DD_SECTOR_NS  MFM_BYTES_TO_NS(658)
#define SECTOR_SIZE 512

static void swim3_update_irq(Swim3State *s)
{
    bool level = (s->mode_reg & SWIM3_INT_ENA) &&
                 (s->int_flags & s->int_mask);

    if (level != s->irq_level) {
        s->irq_level = level;
        trace_swim3_irq(level, s->int_flags, s->int_mask);
        qemu_set_irq(s->irq, level);
    }
}

static void swim3_raise_flag(Swim3State *s, uint8_t flag)
{
    s->int_flags |= flag;
    swim3_update_irq(s);
}

/* ---------------- internal Superdrive ---------------- */

static bool swim3_media_valid(Swim3State *s)
{
    return s->blk && s->disk_in;
}

static uint8_t drive_status(Swim3State *s, uint8_t addr)
{
    uint8_t value;

    switch (addr) {
    case STA_STEP_STATUS:
        value = 1;                       /* step complete (active low idle) */
        break;
    case STA_MOTOR_STATUS:
        value = s->motor_on ? 0 : 1;     /* active low */
        break;
    case STA_EJECT_LATCH:
        value = s->eject_latch;
        break;
    case STA_SELECT_HEAD_0:
        s->cur_head = 0;
        value = 0;
        break;
    case STA_MFM_SUPPORT:
        value = 1;                       /* Superdrive: MFM capable */
        break;
    case STA_DOUBLE_SIDED:
        value = 1;
        break;
    case STA_DRIVE_EXISTS:
        value = 0;                       /* active low: drive present */
        break;
    case STA_DISK_IN_DRIVE:
        value = s->disk_in ? 0 : 1;      /* active low */
        break;
    case STA_WRITE_PROTECT:
        value = s->wr_protect ? 0 : 1;   /* active low */
        break;
    case STA_TRACK_ZERO:
        value = s->cur_track != 0;       /* active low */
        break;
    case STA_SELECT_HEAD_1:
        s->cur_head = 1;
        value = 1;
        break;
    case STA_DRIVE_MODE:
        value = s->drive_mode;           /* 1 = MFM */
        break;
    case STA_DRIVE_READY:
        value = s->is_ready ? 0 : 1;     /* active low */
        break;
    case STA_MEDIA_KIND:
        value = (s->media_hd ? 1 : 0) ^ 1; /* active low: HD reads 0 */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "swim3: status addr 0x%x unimplemented\n",
                      addr);
        value = 0;
        break;
    }
    trace_swim3_drive_status(addr, value);
    return value;
}

static void drive_command(Swim3State *s, uint8_t addr, uint8_t value)
{
    trace_swim3_drive_command(addr, value);

    switch (addr) {
    case CMD_STEP_DIRECTION:
        s->step_dir = value ? -1 : 1;
        break;
    case CMD_DO_STEP:
        if (!value) {
            s->cur_track += s->step_dir;
            if (s->cur_track < 0) {
                s->cur_track = 0;
            } else if (s->cur_track >= SWIM3_MAX_TRACKS) {
                s->cur_track = SWIM3_MAX_TRACKS - 1;
            }
        }
        break;
    case CMD_MOTOR_ON_OFF:
        {
            bool on = !value;            /* active low */
            if (on != s->motor_on) {
                s->motor_on = on;
                if (on) {
                    /*
                     * Only assert ready when media is present: a real
                     * drive with no disk spins but never locks onto a
                     * track. Don't clear ready on motor-off (it means
                     * "no seek in progress", not motor state).
                     */
                    s->is_ready = s->disk_in;
                }
                trace_swim3_motor(on);
            }
        }
        break;
    case CMD_EJECT_DISK:
        if (value && s->disk_in) {
            s->eject_latch = 1;
            s->disk_in = false;
            s->is_ready = false;
        }
        break;
    case CMD_RESET_EJECT_LATCH:
        if (value) {
            s->eject_latch = 0;
        }
        break;
    case CMD_SWITCH_DRIVE_MODE:
        s->drive_mode = value ^ 1;       /* reverse logic */
        s->is_ready = true;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "swim3: command addr 0x%x unimplemented\n",
                      addr);
        break;
    }
}

/* byte offset of a (track, head, 1-based sector) in a raw MFM image */
static int64_t swim3_sector_offset(Swim3State *s, int sector_1based)
{
    return ((int64_t)(s->cur_track * 2 + s->cur_head) * s->sectors_per_track +
            (sector_1based - 1)) * SECTOR_SIZE;
}

static int64_t swim3_sector_time_ns(Swim3State *s)
{
    return s->media_hd ? MFM_HD_SECTOR_NS : MFM_DD_SECTOR_NS;
}

/* ---------------- disk access state machine ---------------- */

static void swim3_access_stop(Swim3State *s)
{
    timer_del(s->access_timer);
    s->access_state = ACCESS_IDLE;
}

static void swim3_complete_pending_io(Swim3State *s)
{
    DBDMA_io *io = s->pending_io;

    if (!io) {
        return;
    }
    s->pending_io = NULL;
    s->pending_off = 0;
    io->len = 0;
    if (io->dma_end) {
        io->dma_end(io);
    }
}

static void swim3_access_advance(Swim3State *s, int64_t delay_ns)
{
    timer_mod(s->access_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void swim3_access_tick(void *opaque)
{
    Swim3State *s = opaque;
    DBDMA_io *io = s->pending_io;
    uint8_t buf[SECTOR_SIZE];
    int sect;

    if (!(s->mode_reg & SWIM3_GO) || !swim3_media_valid(s)) {
        swim3_access_stop(s);
        return;
    }

    switch (s->access_state) {
    case ACCESS_MARK_SEARCH:
        /* rotate to the next address mark */
        s->cur_sector = (s->cur_sector + 1) % s->sectors_per_track;
        sect = s->cur_sector + 1;   /* MFM sector numbering is 1-based */
        s->cur_track_reg = ((s->cur_head & 1) << 7) | (s->cur_track & 0x7F);
        s->cur_sector_reg = 0x80 | (sect & 0x7F);
        s->format = 0x02;  /* MFM address-field format byte, both densities */
        swim3_raise_flag(s, INT_ID_READ);

        if (sect == s->target_sect) {
            s->access_state = ACCESS_DATA_XFER;
            swim3_access_advance(s, MFM_SECT_DATA_NS);
        } else {
            swim3_access_advance(s, swim3_sector_time_ns(s));
        }
        return;

    case ACCESS_DATA_XFER:
        sect = s->cur_sector + 1;

        if (s->mode_reg & SWIM3_WR_MODE) {
            /*
             * WRITE: the guest DMAs a raw MFM byte stream, not bare
             * sector data: gap bytes (0x4E), sync zeros, then
             * escape pairs (0x99,<symbol>) covering the A1 A1 A1 sync
             * marks and the FB data-address-mark, then the 512 data
             * bytes, then a CRC/postamble. (Verified live: the Mac OS
             * .Sony driver's preamble is 10x4E, 12x00, then five
             * escape pairs = a 32-byte prefix; Linux's swim3 driver
             * uses the same structure with a shorter gap.) Skip the
             * framing to find the real data.
             */
            uint8_t hdr[64];
            int skip = 0;

            if (!io || io->is_dma_out == 0 ||
                io->len - (int)s->pending_off < SECTOR_SIZE) {
                /* no (or exhausted) buffer yet: wait for the next one */
                swim3_access_advance(s, swim3_sector_time_ns(s));
                return;
            }
            if (io->len - (int)s->pending_off >= (int)sizeof(hdr)) {
                dma_memory_read(&address_space_memory,
                                io->addr + s->pending_off, hdr, sizeof(hdr),
                                MEMTXATTRS_UNSPECIFIED);
                while (skip < (int)sizeof(hdr) &&
                       (hdr[skip] == 0x4E || hdr[skip] == 0x00)) {
                    skip++;
                }
                while (skip < (int)sizeof(hdr) - 1 && hdr[skip] == 0x99) {
                    skip += 2;   /* escape pair: raw MFM symbol follows */
                }
                if (skip >= (int)sizeof(hdr) ||
                    io->len - (int)(s->pending_off + skip) < SECTOR_SIZE) {
                    skip = 0;    /* unrecognized framing: take data raw */
                }
            }
            dma_memory_read(&address_space_memory,
                            io->addr + s->pending_off + skip,
                            buf, SECTOR_SIZE, MEMTXATTRS_UNSPECIFIED);
            s->pending_off += skip;
            if (!s->wr_protect &&
                blk_pwrite(s->blk, swim3_sector_offset(s, sect),
                           SECTOR_SIZE, buf, 0) < 0) {
                s->error |= 0x40;
                trace_swim3_sector_io(false, s->cur_track, s->cur_head, sect,
                                      false);
            } else {
                trace_swim3_sector_io(false, s->cur_track, s->cur_head, sect,
                                      true);
            }
        } else {
            /* READ: push one sector into the armed DBDMA input buffer */
            if (!io || io->is_dma_out ||
                io->len - (int)s->pending_off < SECTOR_SIZE) {
                swim3_access_advance(s, swim3_sector_time_ns(s));
                return;
            }
            if (blk_pread(s->blk, swim3_sector_offset(s, sect),
                          SECTOR_SIZE, buf, 0) < 0) {
                memset(buf, 0, SECTOR_SIZE);
                s->error |= 0x40;
            }
            dma_memory_write(&address_space_memory, io->addr + s->pending_off,
                             buf, SECTOR_SIZE, MEMTXATTRS_UNSPECIFIED);
            trace_swim3_sector_io(true, s->cur_track, s->cur_head, sect, true);
        }

        s->pending_off += SECTOR_SIZE;
        if (io->len - (int)s->pending_off < SECTOR_SIZE) {
            /* armed buffer consumed: complete this DBDMA command */
            swim3_complete_pending_io(s);
        }

        if (s->xfer_cnt) {
            s->xfer_cnt--;
        }
        if (s->xfer_cnt == 0) {
            swim3_access_stop(s);
            s->mode_reg &= ~SWIM3_GO;
            swim3_raise_flag(s, INT_SECT_DONE);
            return;
        }

        /* advance to the next sector's address mark */
        s->target_sect = (sect % s->sectors_per_track) + 1;
        s->access_state = ACCESS_MARK_SEARCH;
        swim3_access_advance(s, MFM_ADDR_MARK_NS);
        return;

    default:
        swim3_access_stop(s);
        return;
    }
}

static void swim3_access_start(Swim3State *s)
{
    if (s->access_state != ACCESS_IDLE) {
        return;
    }
    if (!swim3_media_valid(s)) {
        /* no disk: raise an error so the driver doesn't wait forever */
        s->error |= 0x10;
        swim3_raise_flag(s, INT_ERROR | INT_SECT_DONE);
        s->mode_reg &= ~SWIM3_GO;
        return;
    }

    s->target_sect = s->first_sec;
    s->access_state = ACCESS_MARK_SEARCH;
    trace_swim3_access_start((s->mode_reg & SWIM3_WR_MODE) != 0,
                             s->cur_track, s->cur_head, s->target_sect,
                             s->xfer_cnt);
    swim3_access_advance(s, MFM_ADDR_MARK_NS);
}

/* ---------------- stepping ---------------- */

static void swim3_step_tick(void *opaque)
{
    Swim3State *s = opaque;

    if (!(s->mode_reg & SWIM3_GO_STEP) || !s->step_count) {
        return;
    }
    drive_command(s, CMD_DO_STEP, 0);
    if (--s->step_count == 0) {
        s->mode_reg &= ~SWIM3_GO_STEP;
        swim3_raise_flag(s, INT_STEP_DONE);
        return;
    }
    timer_mod(s->step_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 80 * 1000); /* 80 us */
}

/* ---------------- 1 us countdown timer ---------------- */

static void swim3_usec_timer_tick(void *opaque)
{
    Swim3State *s = opaque;

    s->timer_val = 0;
    swim3_raise_flag(s, INT_TIMER_DONE);
}

static uint8_t swim3_timer_read(Swim3State *s)
{
    int64_t elapsed_us;

    if (!s->timer_val) {
        return 0;
    }
    elapsed_us = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->timer_start_ns)
                 / 1000;
    if (elapsed_us >= s->timer_val) {
        return 0;
    }
    return (s->timer_val - elapsed_us) & 0xFF;
}

/* ---------------- MMIO ---------------- */

static uint64_t swim3_read(void *opaque, hwaddr addr, unsigned size)
{
    Swim3State *s = opaque;
    unsigned reg = addr >> 4;
    uint8_t status_addr;
    uint64_t val = 0;

    switch (reg) {
    case SWIM3_REG_TIMER:
        val = swim3_timer_read(s);
        break;
    case SWIM3_REG_ERROR:
        val = s->error;
        s->error = 0;
        break;
    case SWIM3_REG_PHASE:
        val = s->phase_lines;
        break;
    case SWIM3_REG_SETUP:
        val = s->setup_reg;
        break;
    case SWIM3_REG_STATUS:
        val = s->mode_reg;
        break;
    case SWIM3_REG_HANDSHAKE:
        if (s->mode_reg & SWIM3_DRIVE_1) {
            status_addr = ((s->mode_reg & SWIM3_HEAD_SELECT) >> 2) |
                          (s->phase_lines & 7);
            uint8_t sense = drive_status(s, status_addr) & 1;
            /* RDDATA (bit 2) and SENSE (bit 3) are wired together */
            val = (sense << 2) | (sense << 3);
        } else {
            val = 0x0C;
        }
        break;
    case SWIM3_REG_INT_FLAGS:
        val = s->int_flags;
        s->int_flags = 0;    /* reading clears all flags */
        swim3_update_irq(s);
        break;
    case SWIM3_REG_STEP:
        val = s->step_count;
        break;
    case SWIM3_REG_CUR_TRACK:
        val = s->cur_track_reg;
        break;
    case SWIM3_REG_CUR_SECTOR:
        val = s->cur_sector_reg;
        break;
    case SWIM3_REG_GAP_FORMAT:
        val = s->format;
        break;
    case SWIM3_REG_FIRST_SEC:
        val = s->first_sec;
        break;
    case SWIM3_REG_XFER_CNT:
        val = s->xfer_cnt;
        break;
    case SWIM3_REG_INT_MASK:
        val = s->int_mask;
        break;
    default:
        break;
    }
    trace_swim3_reg_read(reg, val);
    return val;
}

static void swim3_write(void *opaque, hwaddr addr, uint64_t val64,
                        unsigned size)
{
    Swim3State *s = opaque;
    unsigned reg = addr >> 4;
    uint8_t value = val64 & 0xFF;

    trace_swim3_reg_write(reg, value);

    switch (reg) {
    case SWIM3_REG_TIMER:
        s->timer_val = value;
        if (value) {
            s->timer_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(s->usec_timer,
                      s->timer_start_ns + (int64_t)value * 1000);
        } else {
            timer_del(s->usec_timer);
        }
        break;
    case SWIM3_REG_PARAM:
        s->pram = value;
        break;
    case SWIM3_REG_PHASE:
        s->phase_lines = value & 0xF;
        if (s->phase_lines & 8) {
            /* LSTRB high: command strobe to the selected drive */
            if (s->mode_reg & SWIM3_DRIVE_1) {
                drive_command(s,
                              ((s->mode_reg & SWIM3_HEAD_SELECT) >> 3) |
                              (s->phase_lines & 3),
                              (value >> 2) & 1);
            }
        }
        break;
    case SWIM3_REG_SETUP:
        s->setup_reg = value;
        break;
    case SWIM3_REG_STATUS:
        /* ones clear mode bits */
        if ((s->mode_reg & value) & SWIM3_GO_STEP) {
            timer_del(s->step_timer);
            s->step_count = 0;
        }
        if ((s->mode_reg & value) & SWIM3_GO) {
            swim3_access_stop(s);
            swim3_complete_pending_io(s);
        }
        s->mode_reg &= ~value;
        swim3_update_irq(s);
        break;
    case SWIM3_REG_HANDSHAKE:
        /* ones set mode bits */
        if ((s->mode_reg ^ value) & value & SWIM3_GO_STEP) {
            s->mode_reg |= value;
            if (s->step_count) {
                swim3_step_tick(s);   /* first step immediately */
            } else {
                s->mode_reg &= ~SWIM3_GO_STEP;
                swim3_raise_flag(s, INT_STEP_DONE);
            }
        } else if ((s->mode_reg ^ value) & value & SWIM3_GO) {
            s->mode_reg |= value;
            swim3_access_start(s);
        } else {
            s->mode_reg |= value;
        }
        swim3_update_irq(s);
        break;
    case SWIM3_REG_STEP:
        s->step_count = value;
        break;
    case SWIM3_REG_GAP_FORMAT:
        s->gap_size = value;
        break;
    case SWIM3_REG_FIRST_SEC:
        s->first_sec = value;
        break;
    case SWIM3_REG_XFER_CNT:
        s->xfer_cnt = value;
        break;
    case SWIM3_REG_INT_MASK:
        s->int_mask = value;
        swim3_update_irq(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps swim3_ops = {
    .read = swim3_read,
    .write = swim3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* ---------------- DBDMA channel 1 ---------------- */

static void swim3_dma_rw(DBDMA_io *io)
{
    Swim3State *s = io->opaque;

    /*
     * Park the armed descriptor; the rotational access state machine
     * consumes it sector by sector and completes it (dma_end) when the
     * buffer is exhausted, like bmac's deferred RX path.
     */
    s->pending_io = io;
    s->pending_off = 0;
    trace_swim3_dma_armed(io->is_dma_out, (uint64_t)io->addr, io->len);
}

static void swim3_dma_flush(DBDMA_io *io)
{
    Swim3State *s = io->opaque;

    /*
     * The .Sony driver reads one sector per GO cycle into a large
     * (32K) DBDMA descriptor, then STOPs/flushes the channel and
     * expects the descriptor to complete with a residual count. A
     * flush must therefore COMPLETE the parked descriptor (reporting
     * how much was left), not just forget it -- dropping it leaves
     * ch->io.processing set forever and permanently wedges the
     * channel (observed: one armed descriptor, then every subsequent
     * kick ignored).
     */
    if (s->pending_io == io) {
        s->pending_io = NULL;
        io->len -= s->pending_off;   /* residual */
        s->pending_off = 0;
        if (io->dma_end) {
            io->dma_end(io);
        }
    }
}

void swim3_register_dma(Swim3State *s, void *dbdma)
{
    s->dbdma = dbdma;
    DBDMA_register_channel(dbdma, 1, s->dma_irq,
                           swim3_dma_rw, swim3_dma_flush, s);
}

/* ---------------- media / lifecycle ---------------- */

static void swim3_media_probe(Swim3State *s, Error **errp)
{
    int64_t len;

    s->disk_in = false;
    if (!s->blk) {
        return;
    }
    len = blk_getlength(s->blk);
    if (len == 1474560) {
        s->media_hd = true;
        s->num_tracks = 80;
        s->sectors_per_track = 18;
    } else if (len == 737280) {
        s->media_hd = false;
        s->num_tracks = 80;
        s->sectors_per_track = 9;
    } else {
        error_setg(errp, "swim3: unsupported floppy image size %" PRId64
                   " (supported: raw 1440K/720K MFM)", len);
        return;
    }
    s->disk_in = true;
    s->wr_protect = !blk_supports_write_perm(s->blk);
    s->drive_mode = 1; /* MFM */
}

static void swim3_reset_hold(Object *obj, ResetType type)
{
    Swim3State *s = SWIM3(obj);

    s->setup_reg = 0;
    s->mode_reg = 0;
    s->error = 0;
    s->phase_lines = 0;
    s->int_flags = 0;
    s->int_mask = 0;
    s->step_count = 0;
    s->xfer_cnt = 0;
    s->first_sec = 0xFF;
    s->cur_track_reg = 0xFF;
    s->cur_sector_reg = 0x7F;
    s->timer_val = 0;
    s->irq_level = false;
    s->motor_on = false;
    s->is_ready = false;
    s->eject_latch = 0;
    s->step_dir = 1;
    s->cur_track = 0;
    s->cur_head = 0;
    s->cur_sector = 0;
    s->access_state = ACCESS_IDLE;
    s->pending_io = NULL;
    s->pending_off = 0;
    timer_del(s->usec_timer);
    timer_del(s->step_timer);
    timer_del(s->access_timer);
}

static void swim3_realize(DeviceState *dev, Error **errp)
{
    Swim3State *s = SWIM3(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &swim3_ops, s,
                          "swim3", SWIM3_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->dma_irq);

    s->usec_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                 swim3_usec_timer_tick, s);
    s->step_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, swim3_step_tick, s);
    s->access_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, swim3_access_tick, s);

    if (s->blk) {
        if (!blk_supports_write_perm(s->blk)) {
            blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ, BLK_PERM_ALL,
                         &error_abort);
        } else {
            blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, &error_abort);
        }
        swim3_media_probe(s, errp);
    }
}

static const Property swim3_properties[] = {
    DEFINE_PROP_DRIVE("drive", Swim3State, blk),
};

static const VMStateDescription vmstate_swim3 = {
    .name = "swim3",
    .unmigratable = 1,
};

static void swim3_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = swim3_realize;
    dc->vmsd = &vmstate_swim3;
    device_class_set_props(dc, swim3_properties);
    rc->phases.hold = swim3_reset_hold;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo swim3_type_info = {
    .name = TYPE_SWIM3,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Swim3State),
    .class_init = swim3_class_init,
};

static void swim3_register_types(void)
{
    type_register_static(&swim3_type_info);
}

type_init(swim3_register_types)
