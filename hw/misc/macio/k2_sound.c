/*
 * K2 mac-io feature control registers and the I2S-a sound cell
 *
 * All registers are little-endian. The I2S cell's register block, the
 * clocks-stopped handshake and the frame counter follow the KeyLargo
 * I2S cells; K2 has one cell in use (i2s-a), fed by the DBDMA channels at
 * mac-io DBDMA +0x000 (out) and +0x100 (in).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/macio/k2_sound.h"
#include "hw/misc/macio/tas3004.h"
#include "hw/ppc/mac_dbdma.h"
#include "system/dma.h"
#include "trace.h"

#define I2S_REG_INT_CTL             0x00
#define I2S_REG_SERIAL_FORMAT       0x10
#define I2S_REG_FRAME_COUNT         0x40
#define I2S_REG_DATA_WORD_SIZES     0x60

/* Interrupt control: (enable, pending) bit pairs; pending is W1C */
#define I2S_INT_CLOCKS_STOPPED      0x01000000
#define I2S_INT_PENDING_MASK        0x55550000

/* Reported frame count trails the DMA walk by this much */
#define K2_I2S_COUNT_LAG_NS         (20 * 1000 * 1000)
/* FIFO depth a (re)started stream waits for, and how long it waits */
#define K2_I2S_PREBUF_NS            (60 * 1000 * 1000)
#define K2_I2S_PREBUF_GIVEUP_NS     (100 * 1000 * 1000)
/* How far descriptor retirement may fall behind real time */
#define K2_I2S_MAX_DEBT_NS          (10 * 1000 * 1000)

static uint32_t k2_fcr1(K2SoundState *s)
{
    return s->fcr[(K2_FCR1 - K2_FCR_BASE) >> 2];
}

/* Enabled, clocked and out of reset; anything else latches clocks-stopped */
static void k2_i2s_update_clocks(K2SoundState *s)
{
    uint32_t fcr1 = k2_fcr1(s);

    if (!(fcr1 & K2_FCR1_I2S0_CELL_ENABLE) ||
        !(fcr1 & K2_FCR1_I2S0_CLK_ENABLE) ||
        (fcr1 & K2_FCR1_I2S0_RESET)) {
        s->intr_ctl |= I2S_INT_CLOCKS_STOPPED;
    }
}

static uint64_t k2_fcr_read(void *opaque, hwaddr addr, unsigned size)
{
    K2SoundState *s = opaque;
    uint32_t val = s->fcr[addr >> 2];

    trace_k2_fcr_read(K2_FCR_BASE + addr, val);
    return val;
}

static void k2_fcr_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    K2SoundState *s = opaque;

    trace_k2_fcr_write(K2_FCR_BASE + addr, value);
    s->fcr[addr >> 2] = value;
    k2_i2s_update_clocks(s);
}

static const MemoryRegionOps k2_fcr_ops = {
    .read = k2_fcr_read,
    .write = k2_fcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Serial format: bits 31:30 clock source (18.432, 45.1584, 49.152 MHz),
 * 28:24 MClk divisor, 23:20 SClk divisor; a frame is 64 SClks.
 */
static int k2_i2s_mclk_div(uint32_t field)
{
    switch (field) {
    case 0x14: return 1;
    case 0x13: return 3;
    case 0x12: return 5;
    default:   return (field + 1) * 2;
    }
}

static int k2_i2s_sclk_div(uint32_t field)
{
    switch (field) {
    case 8:  return 1;
    case 9:  return 3;
    default: return (field + 1) * 2;
    }
}

static int k2_i2s_rate(K2SoundState *s)
{
    static const int clock_hz[4] = { 18432000, 45158400, 49152000, 0 };
    uint32_t fmt = s->serial_format;
    int src = clock_hz[(fmt >> 30) & 3];
    int rate;

    if (!src) {
        return 44100;
    }
    rate = src / (k2_i2s_mclk_div((fmt >> 24) & 0x1f) *
                  k2_i2s_sclk_div((fmt >> 20) & 0x0f) * 64);
    return rate > 0 ? rate : 44100;
}

/* Stereo 16-bit, or 24-bit in 32-bit slots */
static int k2_i2s_frame_bytes(K2SoundState *s)
{
    return (s->data_word_sizes & 3) == 3 ? 8 : 4;
}

static uint32_t k2_i2s_frame_count(K2SoundState *s)
{
    uint64_t lag = (uint64_t)k2_i2s_rate(s) * K2_I2S_COUNT_LAG_NS /
                   NANOSECONDS_PER_SECOND;
    uint64_t counted = s->walk_frames > lag ? s->walk_frames - lag : 0;

    return (uint32_t)counted + s->frame_count_bias;
}

static uint64_t k2_i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    K2SoundState *s = opaque;
    uint32_t val;

    switch (addr) {
    case I2S_REG_INT_CTL:
        val = s->intr_ctl;
        break;
    case I2S_REG_SERIAL_FORMAT:
        val = s->serial_format;
        break;
    case I2S_REG_DATA_WORD_SIZES:
        val = s->data_word_sizes;
        break;
    case I2S_REG_FRAME_COUNT:
        val = k2_i2s_frame_count(s);
        break;
    default:
        val = 0;
        break;
    }
    trace_k2_i2s_read(addr, val);
    return val;
}

static void k2_i2s_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    K2SoundState *s = opaque;

    trace_k2_i2s_write(addr, value);
    switch (addr) {
    case I2S_REG_INT_CTL:
        s->intr_ctl &= ~(value & I2S_INT_PENDING_MASK);
        s->intr_ctl = (s->intr_ctl & I2S_INT_PENDING_MASK) |
                      (value & ~I2S_INT_PENDING_MASK);
        break;
    case I2S_REG_SERIAL_FORMAT:
        s->serial_format = value;
        break;
    case I2S_REG_DATA_WORD_SIZES:
        s->data_word_sizes = value;
        break;
    case I2S_REG_FRAME_COUNT:
        /* Bias against the raw walk, so the lag survives the write */
        s->frame_count_bias = value - (uint32_t)s->walk_frames;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps k2_i2s_ops = {
    .read = k2_i2s_read,
    .write = k2_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* An active voice that writes nothing stalls the shared mixer */
static void k2_i2s_write_silence(K2SoundState *s, int avail)
{
    static const uint8_t zeros[4096];
    int fb = s->voice_frame_bytes;

    avail -= avail % fb;
    while (avail >= fb) {
        size_t chunk = MIN(avail, (int)sizeof(zeros));
        size_t written = audio_be_write(s->audio_be, s->voice,
                                        (void *)zeros, chunk);

        written -= written % fb;
        avail -= written;
        if (written < chunk) {
            break;
        }
    }
}

static void k2_i2s_audio_cb(void *opaque, int avail)
{
    K2SoundState *s = opaque;
    int fb = s->voice_frame_bytes;
    bool muted = s->codec && tas3004_muted(s->codec);

    if (s->fifo_count == 0) {
        s->prebuffering = true;
        k2_i2s_write_silence(s, avail);
        return;
    }
    if (s->prebuffering) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint32_t want = (uint64_t)s->voice_rate * fb * K2_I2S_PREBUF_NS /
                        NANOSECONDS_PER_SECOND;

        if (s->fifo_count < want &&
            now - s->last_push_ns < K2_I2S_PREBUF_GIVEUP_NS) {
            k2_i2s_write_silence(s, avail);
            return;
        }
        s->prebuffering = false;
    }

    avail -= avail % fb;
    while (avail >= fb && s->fifo_count >= fb) {
        uint8_t staging[4096];
        int chunk = MIN(MIN(avail, (int)sizeof(staging)), (int)s->fifo_count);
        size_t written;
        int i;

        chunk -= chunk % fb;
        for (i = 0; i < chunk; i++) {
            staging[i] = s->out_fifo[s->fifo_rptr];
            s->fifo_rptr = (s->fifo_rptr + 1) % sizeof(s->out_fifo);
        }
        s->fifo_count -= chunk;
        if (muted) {
            memset(staging, 0, chunk);
        }
        written = audio_be_write(s->audio_be, s->voice, staging, chunk);
        written -= written % fb;
        avail -= written;
        if (written < (size_t)chunk) {
            s->fifo_rptr = (s->fifo_rptr + sizeof(s->out_fifo) -
                            (chunk - written)) % sizeof(s->out_fifo);
            s->fifo_count += chunk - written;
            break;
        }
    }
}

static void k2_i2s_tap_out(K2SoundState *s, DBDMA_io *io, int rate)
{
    int fb = k2_i2s_frame_bytes(s);
    uint8_t buf[4096];
    hwaddr addr = io->addr;
    int remaining = io->len;

    if (!s->audio_be) {
        return;
    }
    if (!s->voice || s->voice_rate != rate || s->voice_frame_bytes != fb) {
        struct audsettings as = {
            .freq = rate,
            .nchannels = 2,
            .fmt = fb == 8 ? AUDIO_FORMAT_S32 : AUDIO_FORMAT_S16,
            .big_endian = true,
        };

        s->voice_rate = rate;
        s->voice_frame_bytes = fb;
        s->fifo_rptr = s->fifo_wptr = s->fifo_count = 0;
        s->voice = audio_be_open_out(s->audio_be, s->voice, "tas3004.out",
                                     s, k2_i2s_audio_cb, &as);
        audio_be_set_active_out(s->audio_be, s->voice, true);
        s->prebuffering = true;
    }

    s->last_push_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    while (remaining > 0) {
        int len = MIN(remaining, (int)sizeof(buf));
        int i;

        dma_memory_read(&address_space_memory, addr, buf, len,
                        MEMTXATTRS_UNSPECIFIED);
        for (i = 0; i < len && s->fifo_count < sizeof(s->out_fifo); i++) {
            s->out_fifo[s->fifo_wptr] = buf[i];
            s->fifo_wptr = (s->fifo_wptr + 1) % sizeof(s->out_fifo);
            s->fifo_count++;
        }
        addr += len;
        remaining -= len;
    }
}

static void k2_i2s_dir_complete(void *opaque)
{
    K2I2SDir *d = opaque;
    DBDMA_io *io = d->pending;

    d->pending = NULL;
    if (io) {
        io->dma_end(io);
    }
}

/* Descriptors retire at the pace the audio would play */
static void k2_i2s_dma_rw(DBDMA_io *io)
{
    K2SoundState *s = io->opaque;
    K2I2SDir *d = &s->dir[io->is_dma_out ? 0 : 1];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int rate = k2_i2s_rate(s);
    int frames = io->len / k2_i2s_frame_bytes(s);

    trace_k2_i2s_dma(io->is_dma_out, io->addr, io->len, rate);

    if (io->is_dma_out) {
        s->walk_frames += frames;
        k2_i2s_tap_out(s, io, rate);
    } else {
        dma_memory_set(&address_space_memory, io->addr, 0, io->len,
                       MEMTXATTRS_UNSPECIFIED);
    }

    if (d->deadline_ns < now - K2_I2S_MAX_DEBT_NS) {
        d->deadline_ns = now - K2_I2S_MAX_DEBT_NS;
    }
    d->deadline_ns += (int64_t)frames * NANOSECONDS_PER_SECOND / rate;
    if (d->deadline_ns <= now) {
        io->dma_end(io);
        return;
    }
    d->pending = io;
    timer_mod(d->timer, d->deadline_ns);
}

/* The outstanding command completes before the channel reports stopped */
static void k2_i2s_dma_flush(DBDMA_io *io)
{
    K2SoundState *s = io->opaque;
    K2I2SDir *d = &s->dir[io->is_dma_out ? 0 : 1];

    if (d->pending) {
        DBDMA_io *pending = d->pending;

        d->pending = NULL;
        timer_del(d->timer);
        pending->dma_end(pending);
    }
    d->deadline_ns = 0;
}

void k2_sound_register_dma(K2SoundState *s, void *dbdma,
                           qemu_irq tx_irq, qemu_irq rx_irq)
{
    DBDMA_register_channel(dbdma, K2_I2S_DMA_OUT_OFFSET >> DBDMA_CHANNEL_SHIFT,
                           tx_irq, k2_i2s_dma_rw, k2_i2s_dma_flush, s);
    DBDMA_register_channel(dbdma, K2_I2S_DMA_IN_OFFSET >> DBDMA_CHANNEL_SHIFT,
                           rx_irq, k2_i2s_dma_rw, k2_i2s_dma_flush, s);
}

void k2_sound_reset(K2SoundState *s)
{
    int i;

    memset(s->fcr, 0, sizeof(s->fcr));
    s->intr_ctl = 0;
    s->serial_format = 0;
    s->data_word_sizes = 0;
    s->walk_frames = 0;
    s->frame_count_bias = 0;
    for (i = 0; i < 2; i++) {
        timer_del(s->dir[i].timer);
        s->dir[i].pending = NULL;
        s->dir[i].deadline_ns = 0;
    }
    s->fifo_rptr = s->fifo_wptr = s->fifo_count = 0;
    s->prebuffering = true;
    k2_i2s_update_clocks(s);
}

void k2_sound_init(K2SoundState *s, DeviceState *owner, MemoryRegion *bar)
{
    int i;

    memory_region_init_io(&s->fcr_mem, OBJECT(owner), &k2_fcr_ops, s,
                          "k2-fcr", K2_FCR_COUNT * 4);
    memory_region_add_subregion(bar, K2_FCR_BASE, &s->fcr_mem);

    memory_region_init_io(&s->i2s_mem, OBJECT(owner), &k2_i2s_ops, s,
                          "k2-i2s-a", K2_I2S_SIZE);
    memory_region_add_subregion(bar, K2_I2S_BASE, &s->i2s_mem);

    for (i = 0; i < 2; i++) {
        s->dir[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       k2_i2s_dir_complete, &s->dir[i]);
    }
    k2_sound_reset(s);
}
