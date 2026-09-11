/*
 * QEMU PowerMac Awacs Screamer device support
 *
 * Copyright (c) 2016 Mark Cave-Ayland
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
#include "qemu/audio.h"
#include "hw/core/irq.h"
#include "hw/audio/screamer.h"
#include "hw/core/qdev-properties.h"
#include "qemu/timer.h"
#include "hw/ppc/mac_dbdma.h"
#include "system/system.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

/* debug screamer */
//#define DEBUG_SCREAMER

#ifdef DEBUG_SCREAMER
#define SCREAMER_DPRINTF(fmt, ...)                                  \
    do { printf("SCREAMER: " fmt , ## __VA_ARGS__); } while (0)
#else
#define SCREAMER_DPRINTF(fmt, ...)
#endif

/* chip registers */
#define SND_CTRL_REG   0x0
#define CODEC_CTRL_REG 0x1
#define CODEC_STAT_REG 0x2
#define CLIP_CNT_REG   0x3
#define BYTE_SWAP_REG  0x4
#define FRAME_CNT_REG  0x5

#define CODEC_CTRL_MASKECMD        (0x1 << 24)
#define CODEC_CTRL1_RECALIBRATE    0x4

#define CODEC_STAT_MANUFACTURER_CRYSTAL    0x100
#define CODEC_STAT_AWACS_REVISION          0x3000
#define CODEC_STAT_MASK_VALID              (0x1 << 22)

/*
 * The reported count trails real time, so the guest's eraser cannot
 * overtake what has actually been handed to the backend.
 */
#define SCREAMER_COUNT_LAG_NS   (20 * 1000 * 1000)

/*
 * Frame counter behaviour, selected by the frame-count property:
 *   legacy  advances in the output callback by what the backend consumed
 *   gated   zero until the first DMA transfer, then counts at the sample rate
 *   clock   free-running from reset at the sample rate
 * Only legacy is known to boot Mac OS 9.
 */
#define SCREAMER_FC_LEGACY  0
#define SCREAMER_FC_GATED   1
#define SCREAMER_FC_CLOCK   2

static uint32_t screamer_frame_count(ScreamerState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int rate = s->rate ? s->rate : 44100;
    int64_t elapsed;

    if (s->frame_count_mode == SCREAMER_FC_LEGACY) {
        return s->regs[FRAME_CNT_REG];
    }

    if (s->frame_count_mode == SCREAMER_FC_GATED && !s->frame_count_running) {
        return s->frame_count_base_val;
    }

    elapsed = now - s->frame_count_base_ns - SCREAMER_COUNT_LAG_NS;
    if (elapsed < 0) {
        elapsed = 0;
    }

    return s->frame_count_base_val +
           (uint32_t)(elapsed * rate / NANOSECONDS_PER_SECOND);
}

/* Start/stop the gated counter, banking what it has counted so far. */
static void screamer_frame_count_run(ScreamerState *s, bool running)
{
    if (s->frame_count_mode != SCREAMER_FC_GATED ||
        running == s->frame_count_running) {
        return;
    }

    if (running) {
        s->frame_count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else {
        s->frame_count_base_val = screamer_frame_count(s);
    }
    s->frame_count_running = running;
}

static void screamer_frame_count_rebase(ScreamerState *s, uint32_t val)
{
    s->frame_count_base_val = val;
    s->frame_count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->regs[FRAME_CNT_REG] = val;
}

/* Audio */
static const char *s_spk = "screamer";

/* Fetch ring, in frames; a power of two. */
#define SCREAMER_RING_FRAMES    16384
#define SCREAMER_FRAME_BYTES    4

/* Fetch granularity, and how far ahead of the sample clock a fetch runs. */
#define SCREAMER_FETCH_TICK_NS  (1000 * 1000)
#define SCREAMER_FETCH_LEAD_NS  (1000 * 1000)

/* A fetch further behind the sample clock than this restarts the clock. */
#define SCREAMER_FETCH_SLIP_NS  (10 * 1000 * 1000)

/* Output latency added at stream start. */
#define SCREAMER_PREROLL_NS     (40 * 1000 * 1000)

/* Idle time after which the next descriptor starts a new stream. */
#define SCREAMER_IDLE_NS        (50 * 1000 * 1000)

static uint32_t screamer_ring_level(ScreamerState *s)
{
    return s->ring_w - s->ring_r;
}

static void screamer_ring_put(ScreamerState *s, hwaddr addr, uint32_t frames)
{
    uint32_t pos = s->ring_w & (SCREAMER_RING_FRAMES - 1);
    uint32_t n = MIN(frames, SCREAMER_RING_FRAMES - pos);

    dma_memory_read(&address_space_memory, addr,
                    s->ring + pos * SCREAMER_FRAME_BYTES,
                    n * SCREAMER_FRAME_BYTES, MEMTXATTRS_UNSPECIFIED);
    if (frames > n) {
        dma_memory_read(&address_space_memory, addr + n * SCREAMER_FRAME_BYTES,
                        s->ring, (frames - n) * SCREAMER_FRAME_BYTES,
                        MEMTXATTRS_UNSPECIFIED);
    }
    s->ring_w += frames;
}

/*
 * Fetch descriptor data no earlier than the codec consumes it and
 * complete each descriptor once its last frame is fetched, as the DBDMA
 * engine does. Output latency is added after the fetch, never by
 * reading guest buffers ahead of time.
 */
static void screamer_fetch(ScreamerState *s)
{
    int64_t rate = s->rate ? s->rate : 44100;
    int64_t now, since;
    uint64_t due;
    uint32_t frames;

    if (s->fetching) {
        return;
    }
    s->fetching = true;

    while (s->io_busy) {
        now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        since = now + SCREAMER_FETCH_LEAD_NS - s->fetch_t0_ns;
        due = since > 0 ? since * rate / NANOSECONDS_PER_SECOND : 0;

        if (due > s->fetched + SCREAMER_FETCH_SLIP_NS * rate /
                               NANOSECONDS_PER_SECOND) {
            /* Behind the clock: restart it here rather than burst. */
            s->fetch_t0_ns = now + SCREAMER_FETCH_LEAD_NS -
                             (int64_t)(s->fetched * NANOSECONDS_PER_SECOND / rate);
            due = s->fetched;
        }

        frames = MIN(due - MIN(due, s->fetched), s->io.len / SCREAMER_FRAME_BYTES);
        frames = MIN(frames, SCREAMER_RING_FRAMES - screamer_ring_level(s));
        if (frames) {
            screamer_ring_put(s, s->io.addr, frames);
            s->io.addr += frames * SCREAMER_FRAME_BYTES;
            s->io.len -= frames * SCREAMER_FRAME_BYTES;
            s->fetched += frames;
        }

        if (s->io.len >= SCREAMER_FRAME_BYTES) {
            break;
        }

        s->io_busy = false;
        s->io.dma_end(&s->io);
    }

    if (s->io_busy) {
        timer_mod(s->fetch_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  SCREAMER_FETCH_TICK_NS);
    }
    s->fetching = false;
}

static void screamer_fetch_timer(void *opaque)
{
    screamer_fetch(opaque);
}

static void pmac_screamer_tx(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Gated mode starts counting here. */
    screamer_frame_count_run(s, true);

    SCREAMER_DPRINTF("DMA TX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    if (!s->out_running && !screamer_ring_level(s) &&
        now - s->drained_ns >= SCREAMER_IDLE_NS) {
        s->fetch_t0_ns = now;
        s->fetched = 0;
        s->out_start_ns = now + SCREAMER_PREROLL_NS;
    }

    memcpy(&s->io, io, sizeof(DBDMA_io));
    s->io_busy = true;
    screamer_fetch(s);
}

static void pmac_screamer_tx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA TX flush!\n");

#if 0
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == OUTPUT_MORE || cmd == OUTPUT_LAST ||
        cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif

    ch->io.processing = false;

    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

            dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }

}

static void pmac_screamer_rx(DBDMA_io *io)
{
    SCREAMER_DPRINTF("DMA RX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    //ScreamerState *s = io->opaque;
    DBDMA_channel *ch = io->channel;

    /* FIXME: stop channel after updating with status to stop MacOS 9 freezing */
    ch->regs[DBDMA_STATUS] &= ~RUN;

    io->dma_end(io);
}

static void pmac_screamer_rx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA RX flush!\n");
#if 1
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif
}

void macio_screamer_register_dma(ScreamerState *s, void *dbdma, int txchannel, int rxchannel)
{
    s->dbdma = dbdma;
    DBDMA_register_channel(dbdma, txchannel, s->dma_tx_irq,
                           pmac_screamer_tx, pmac_screamer_tx_flush, s);
    DBDMA_register_channel(dbdma, rxchannel, s->dma_rx_irq,
                           pmac_screamer_rx, pmac_screamer_rx_flush, s);
}

static void screamerspk_callback(void *opaque, int free_b)
{
    ScreamerState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t level, pos, n, generated = 0;
    size_t written;

    level = screamer_ring_level(s);
    if (!s->out_running) {
        if (!level || now < s->out_start_ns) {
            return;
        }
        s->out_running = true;
    }

    /*
     * The backend may accept less than it is offered. Advance only by
     * what it took, and only by whole frames: a partial-frame advance
     * desynchronises the interleaved stream.
     */
    while (level && free_b >= SCREAMER_FRAME_BYTES) {
        pos = s->ring_r & (SCREAMER_RING_FRAMES - 1);
        n = MIN(level, SCREAMER_RING_FRAMES - pos);
        n = MIN(n, free_b / SCREAMER_FRAME_BYTES);
        written = audio_be_write(s->be, s->voice,
                                 s->ring + pos * SCREAMER_FRAME_BYTES,
                                 n * SCREAMER_FRAME_BYTES);
        written /= SCREAMER_FRAME_BYTES;
        s->ring_r += written;
        generated += written;
        level -= written;
        free_b -= written * SCREAMER_FRAME_BYTES;
        if (written < n) {
            break;
        }
    }

    /* Reported by legacy mode only. */
    s->regs[FRAME_CNT_REG] += generated;

    if (!level && !s->io_busy) {
        s->out_running = false;
        s->drained_ns = now;
    }
}

static void screamer_update_settings(ScreamerState *s)
{
    struct audsettings as = { s->rate, 2, AUDIO_FORMAT_S16,
        1 };

    s->voice = audio_be_open_out(s->be, s->voice, s_spk, s, screamerspk_callback, &as);
    if (!s->voice) {
        error_report("Could not open voice");
        exit(1);
    }

    s->shift = 2;

    audio_be_set_active_out(s->be, s->voice, true);
}

static void screamer_update_volume(ScreamerState *s)
{
    uint8_t muted = s->codec_ctrl_regs[0x1] & 0x80 ? 1 : 0;
    /* Bits 6-9 attenuate the left channel, bits 0-3 the right. */
    uint8_t att_left = (s->codec_ctrl_regs[0x4] & 0x3c0) >> 6;
    uint8_t att_right = (s->codec_ctrl_regs[0x4] & 0xf);

    SCREAMER_DPRINTF("setting mute: %d, attenuation L: %d R: %d\n",
                     muted, att_left, att_right);

    audio_be_set_volume_out_lr(s->be, s->voice, muted, (0xf - att_left) << 4,
                          (0xf - att_right) << 4);
}

static void screamer_reset(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->codec_ctrl_regs, 0, sizeof(s->codec_ctrl_regs));
    memset(&s->io, 0, sizeof(DBDMA_io));

    s->rate = 44100;
    screamer_frame_count_rebase(s, 0);
    screamer_update_settings(s);

    if (s->fetch_timer) {
        timer_del(s->fetch_timer);
    }
    s->io_busy = false;
    s->fetching = false;
    s->fetched = 0;
    s->ring_r = 0;
    s->ring_w = 0;
    s->out_running = false;
    s->drained_ns = INT64_MIN / 2;

    s->bpos = 0;
    s->ppos = 0;

    return;
}

static void screamer_realizefn(DeviceState *dev, Error **errp)
{
    ScreamerState *s = SCREAMER(dev);

    if (!audio_be_check(&s->be, errp)) {
        return;
    }

    s->fetch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, screamer_fetch_timer, s);
    s->ring = g_malloc0(SCREAMER_RING_FRAMES * SCREAMER_FRAME_BYTES);
    s->drained_ns = INT64_MIN / 2;

    s->frame_count_mode = SCREAMER_FC_LEGACY;
    if (s->frame_count_mode_str) {
        if (!strcmp(s->frame_count_mode_str, "gated")) {
            s->frame_count_mode = SCREAMER_FC_GATED;
        } else if (!strcmp(s->frame_count_mode_str, "clock")) {
            s->frame_count_mode = SCREAMER_FC_CLOCK;
        } else if (strcmp(s->frame_count_mode_str, "legacy")) {
            error_setg(errp, "screamer: frame-count must be "
                       "legacy, gated or clock");
            return;
        }
    }

    s->rate = 44100;
    screamer_frame_count_rebase(s, 0);
    screamer_update_settings(s);
}

static void screamer_control_write(ScreamerState *s, uint32_t val)
{
    uint32_t old_rate = s->rate;
    uint32_t count_now = screamer_frame_count(s);

    SCREAMER_DPRINTF("%s: val %" PRId32 "\n", __func__, val);

    /* Basic rate selection */
    switch ((val & 0x700) >> 8) {
    case 0x00:
        s->rate = 44100;
        break;
    case 0x1:
        s->rate = 29400;
        break;
    case 0x2:
        s->rate = 22050;
        break;
    case 0x3:
        s->rate = 17640;
        break;
    case 0x4:
        s->rate = 14700;
        break;
    case 0x5:
        s->rate = 11025;
        break;
    case 0x6:
        s->rate = 8820;
        break;
    case 0x7:
        s->rate = 7350;
        break;
    }

    SCREAMER_DPRINTF("basic rate: %d\n", s->rate);
    if (s->rate != old_rate) {
        /* Keep the counter continuous across a rate change. */
        screamer_frame_count_rebase(s, count_now);
    }
    screamer_update_settings(s);

    s->regs[0] = val;
}

static void screamer_codec_write(ScreamerState *s, hwaddr addr, uint64_t val)
{
    //SCREAMER_DPRINTF("%s: addr " HWADDR_PRIx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case 0x1:
        /* Clear recalibrate if set */
        val = val & ~CODEC_CTRL1_RECALIBRATE;

        /* Update volume in case mute set */
        screamer_update_volume(s);
        break;

    case 0x4:
        /* Speaker attenuation */
        screamer_update_volume(s);
        break;
    }

    s->codec_ctrl_regs[addr] = val;
}

static uint64_t screamer_read(void *opaque, hwaddr addr, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t val;

    addr = addr >> 4;
    switch (addr) {
    case SND_CTRL_REG:
        val = s->regs[addr];
        break;
    case CODEC_CTRL_REG:
        val = s->regs[addr] & ~CODEC_CTRL_MASKECMD;
        break;
    case CODEC_STAT_REG:
        if (s->codec_ctrl_regs[7] & 1) {
            /* Read back mode */
            val = s->codec_ctrl_regs[(s->codec_ctrl_regs[7] >> 1) & 0xe];
        } else {
            /* Return status register */
            val = s->regs[addr] & ~0xff00;
            val |= CODEC_STAT_MANUFACTURER_CRYSTAL | CODEC_STAT_AWACS_REVISION |
                   CODEC_STAT_MASK_VALID;
        }
        break;
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
        val = s->regs[addr];
        break;
    case FRAME_CNT_REG:
        val = screamer_frame_count(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register read "
                  "reg 0x%" HWADDR_PRIx " size 0x%x\n",
                  addr, size);
        val = 0;
        break;
    }

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " -> %x\n", __func__, addr, val);

    return val;
}

static void screamer_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t codec_addr;

    addr = addr >> 4;

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case SND_CTRL_REG:
        screamer_control_write(s, val & 0xffffffff);
        break;
    case CODEC_CTRL_REG:
        s->regs[addr] = val & 0xffffffff;
        codec_addr = (val & 0x7fff) >> 12;
        screamer_codec_write(s, codec_addr, val & 0xfff);
        break;
    case CODEC_STAT_REG:
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
        s->regs[addr] = val & 0xffffffff;
        break;
    case FRAME_CNT_REG:
        /* Writing it sets the count; it keeps running from there. */
        screamer_frame_count_rebase(s, val & 0xffffffff);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register write "
                  "reg 0x%" HWADDR_PRIx " size 0x%x value 0x%" PRIx64 "\n",
                  addr, size, val);
        break;
    }

    return;
}

static const MemoryRegionOps screamer_ops = {
    .read = screamer_read,
    .write = screamer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    }
};

static void screamer_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    ScreamerState *s = SCREAMER(obj);

    memory_region_init_io(&s->mem, obj, &screamer_ops, s, "screamer", 0x1000);
    sysbus_init_mmio(d, &s->mem);
    sysbus_init_irq(d, &s->irq);
    sysbus_init_irq(d, &s->dma_tx_irq);
    sysbus_init_irq(d, &s->dma_rx_irq);
}

static const Property screamer_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ScreamerState, be),
    DEFINE_PROP_STRING("frame-count", ScreamerState, frame_count_mode_str),
};

static void screamer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = screamer_realizefn;
    dc->legacy_reset = screamer_reset;
    device_class_set_props(dc, screamer_properties);
}

static const TypeInfo screamer_type_info = {
    .name = TYPE_SCREAMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScreamerState),
    .instance_init = screamer_initfn,
    .class_init = screamer_class_init,
};

static void screamer_register_types(void)
{
    type_register_static(&screamer_type_info);
}

type_init(screamer_register_types)
