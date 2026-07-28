/*
 * QEMU Apple AWACS/Screamer sound codec (macio "davbus")
 *
 * As found in Beige Power Mac G3 and other OldWorld PowerMacs, inside
 * the Heathrow (macio) chip.
 *
 * Makes the codec register block behave plausibly enough for firmware/
 * OS probing and the ROM's startup chime routine (which polls the Codec
 * Status register for "codec available" before writing samples) to
 * proceed. Real Beige G3 ROM boot hangs indefinitely without this -- the
 * ROM writes waveform samples to Sound Control/Codec Control and polls
 * Codec Status, and with nothing mapped there at all the access faults
 * instead of ever seeing the "ready" bits it expects. DMA-out samples
 * are forwarded to a real QEMU audio backend (see awacs_dma_rw) once
 * the DBDMA channel actually reaches this code -- this depended on a
 * real DBDMA channel-addressing bug (DBDMA_CHANNEL_SHIFT, see
 * include/hw/ppc/mac_dbdma.h) that silently doubled every real channel
 * number, including this device's own AWACS_DMA_CHANNEL/
 * AWACS_DMA_IN_CHANNEL, until fixed.
 *
 * Register layout and PCM format (16-bit signed stereo, big-endian in
 * the DMA buffer) confirmed against DingusPPC's AwacsScreamer model
 * (devices/sound/awacs.cpp) and its soundserver_cubeb.cpp playback
 * callback (READ_WORD_BE_A per sample). Sample-rate table
 * (screamer_freqs below) copied from the same source.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/audio/awacs.h"
#include "hw/core/qdev-properties.h"
#include "hw/ppc/mac_dbdma.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "system/dma.h"

/* Real Screamer sample rates, indexed by SOUND_CTRL bits 8-10 (see
 * DingusPPC's AwacsScreamer::AwacsScreamer, devices/sound/awacs.cpp). */
static const int awacs_sample_rates[8] = {
    44100, 29400, 22050, 17640, 14700, 11025, 8820, 7350
};

/* Queue converted samples for awacs_audio_callback to drain; drops
 * data on overflow (a real overrun, matching how a real FIFO behaves
 * under sustained backpressure -- 128KB is generous headroom for a
 * short chime, so this should only ever bite under pathological
 * conditions). */
static void awacs_fifo_push(AWACSState *s, const uint8_t *data, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (s->out_fifo_count >= AWACS_OUT_FIFO_SIZE) {
            break;
        }
        s->out_fifo[s->out_fifo_wptr] = data[i];
        s->out_fifo_wptr = (s->out_fifo_wptr + 1) % AWACS_OUT_FIFO_SIZE;
        s->out_fifo_count++;
    }
}

/*
 * Drains our own FIFO into the audio backend. This must be pull-driven
 * (invoked by the backend's own real-time-paced timer via `avail`, not
 * called directly from awacs_dma_rw) -- the guest hands us an entire
 * DBDMA descriptor chain's worth of samples in one synchronous burst,
 * far faster than real playback time, and writing straight to the
 * backend from there overflows its own small internal buffer after the
 * first chunk with nothing ever draining it in between (confirmed
 * empirically: audio_be_write's own buffer, ~4100 bytes, accepted
 * exactly one 4096-byte chunk then returned 0 for every further call).
 */
static void awacs_audio_callback(void *opaque, int avail)
{
    AWACSState *s = AWACS(opaque);

    while (avail > 0 && s->out_fifo_count > 0) {
        uint8_t staging[4096];
        int chunk = MIN(avail, (int)sizeof(staging));
        int i;
        size_t written;

        chunk = MIN(chunk, (int)s->out_fifo_count);
        for (i = 0; i < chunk; i++) {
            staging[i] = s->out_fifo[s->out_fifo_rptr];
            s->out_fifo_rptr = (s->out_fifo_rptr + 1) % AWACS_OUT_FIFO_SIZE;
        }
        s->out_fifo_count -= chunk;

        written = audio_be_write(s->audio_be, s->voice, staging, chunk);
        avail -= written;
        if (written < (size_t)chunk) {
            /* Backend couldn't take it all either -- put the unwritten
             * tail back at the front of the FIFO and stop for now. */
            s->out_fifo_rptr = (s->out_fifo_rptr + AWACS_OUT_FIFO_SIZE -
                               (chunk - written)) % AWACS_OUT_FIFO_SIZE;
            s->out_fifo_count += chunk - written;
            break;
        }
    }
}

static void awacs_open_voice(AWACSState *s, int sample_rate)
{
    struct audsettings as = {
        .freq = sample_rate,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };

    s->cur_sample_rate = sample_rate;
    s->voice = audio_be_open_out(s->audio_be, s->voice, "awacs.out", s,
                                 awacs_audio_callback, &as);
    audio_be_set_active_out(s->audio_be, s->voice, true);
}

/*
 * The real DBDMA channel-address bug behind an earlier pass's "channel
 * 18, not 9" empirical finding (davbus's device-tree "reg" property is
 * 0x8800/0x8900 -> channels 8/9, matching real hardware) has now been
 * found and fixed at its source: QEMU's DBDMA_CHANNEL_SHIFT was wrong
 * (see include/hw/ppc/mac_dbdma.h), silently doubling every real
 * channel number. With that fixed, the real, davbus-device-tree-
 * correct channel number (9) is used directly here again.
 */
#define AWACS_DMA_CHANNEL 8
#define AWACS_DMA_IN_CHANNEL 9

/*
 * DBDMA channel callback: the ROM's startup chime (and any other sound
 * playback) primes a DBDMA descriptor chain and expects it to actually
 * complete -- with no channel registered at all, the transfer just sat
 * unserviced forever. Beyond just consuming the samples and signaling
 * completion the same way DingusPPC's own working AwacsBase DMA path
 * does (it never uses the AWACS IRQ for this either -- only the generic
 * DMA channel's own completion/interrupt logic matters), the samples
 * are forwarded to a real audio backend so playback is actually
 * audible (or verifiable via e.g. `-audiodev wav,...`).
 *
 * There is no separate audio-input channel registration here (DingusPPC's
 * own AwacsScreamer model doesn't implement one either): a guest probe of
 * the audio-in DBDMA channel now falls through to the generic
 * dbdma_unassigned_rw() path in hw/misc/macio/mac_dbdma.c, which properly
 * completes the command list (advancing it via the normal
 * interrupt/branch handling) instead of leaving it stuck -- fixing this
 * generically for any unregistered channel, not just this one.
 */
static void awacs_dma_rw(DBDMA_io *io)
{
    AWACSState *s = AWACS(io->opaque);
    uint8_t buf[4096];
    hwaddr addr = io->addr;
    int remaining = io->len;

    /*
     * A single descriptor's req_count can be up to 0x8000+ bytes (a
     * real chime's descriptors are ~32KB each, confirmed live) -- far
     * more than fits in one `buf`. An earlier version of this function
     * only ever read the first sizeof(buf) bytes and then completed
     * the WHOLE descriptor regardless, silently discarding the rest:
     * verified to cut the real chime down to ~53KB of the ~404KB a
     * real ROM actually requests (confirmed byte-for-byte against
     * DingusPPC's own captured chime audio, which is the real,
     * complete-duration reference). Loop over the full transfer here
     * instead, only completing the descriptor once all of it has
     * actually been consumed.
     */
    while (remaining > 0) {
        int len = MIN(remaining, (int)sizeof(buf));

        if (io->is_dma_out) {
            int i;

            dma_memory_read(&address_space_memory, addr, buf, len,
                            MEMTXATTRS_UNSPECIFIED);

            /* Raw digital samples are 16-bit big-endian on real
             * hardware (DingusPPC's sound_out_callback reads each one
             * via READ_WORD_BE_A); the backend was opened with
             * big_endian = false, so convert to host order before
             * writing. */
            for (i = 0; i + 1 < len; i += 2) {
                uint16_t sample;
                memcpy(&sample, &buf[i], 2);
                sample = be16_to_cpu(sample);
                memcpy(&buf[i], &sample, 2);
            }
            awacs_fifo_push(s, buf, len & ~1);
        }

        addr += len;
        remaining -= len;
    }

    io->dma_end(io);
}

static void awacs_dma_flush(DBDMA_io *io)
{
}

/*
 * Real audio-in DMA channel: the ROM's AWACS audio-input probe primes
 * a descriptor ring (two INPUT_MORE/INPUT_LAST commands, each guarded
 * by a BR_IFSET testing bit 0 of the DBDMA channel's status byte) that
 * is a genuine infinite loop on its "condition not met" path -- real
 * hardware (and DingusPPC's AwacsBase::dma_in_data(), which does the
 * exact same thing via DMAChannel::set_stat()) breaks out of it by
 * having the audio-in device itself periodically flip that status
 * byte as it services transfers, until the BR_IFSET's condition is
 * finally satisfied and the ring takes its other, terminating branch
 * (which ends in a real DBDMA_STOP command). Without this, nothing
 * ever changes that byte and the ring spins forever -- confirmed via
 * live tracing that our own generic dbdma_unassigned_rw() completion
 * path (correct for a channel nothing services at all) cannot fix
 * this specific case, since the loop's exit depends on a *content*
 * signal from the device, not just the transfer completing.
 */
static void awacs_dma_in_rw(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    AWACSState *s = AWACS(io->opaque);
    uint8_t buf[4096];
    int len = MIN(io->len, (int)sizeof(buf));

    memset(buf, 0, len);
    if (!io->is_dma_out) {
        dma_memory_write(&address_space_memory, io->addr, buf, len,
                         MEMTXATTRS_UNSPECIFIED);
    }

    ch->regs[DBDMA_STATUS] = (ch->regs[DBDMA_STATUS] & ~DEVSTAT) |
                             s->dma_in_status;
    s->dma_in_status <<= 1;
    if (!s->dma_in_status) {
        s->dma_in_status = 1;
    }

    io->dma_end(io);
}

static void awacs_dma_in_flush(DBDMA_io *io)
{
}

void awacs_register_dma(AWACSState *s, void *dbdma)
{
    DBDMA_register_channel(dbdma, AWACS_DMA_CHANNEL, s->dma_irq,
                           awacs_dma_rw, awacs_dma_flush, s);
    DBDMA_register_channel(dbdma, AWACS_DMA_IN_CHANNEL, s->dma_irq,
                           awacs_dma_in_rw, awacs_dma_in_flush, s);
}

static uint64_t awacs_read(void *opaque, hwaddr addr, unsigned size)
{
    AWACSState *s = AWACS(opaque);
    uint32_t reg = addr & 0xff;

    switch (reg) {
    case AWACS_SOUND_CTRL:
        return s->sound_ctrl;
    case AWACS_CODEC_CTRL:
        return s->codec_ctrl;
    case AWACS_CODEC_STATUS:
        return (AWACS_STATUS_AVAILABLE << 8) |
               (AWACS_MAKER_CRYSTAL << 16) |
               (AWACS_REV_SCREAMER << 20);
    case AWACS_CLIP_COUNT:
        return s->clip_count;
    case AWACS_BYTE_SWAP:
        return s->byte_swap;
    case AWACS_FRAME_COUNT:
        return s->frame_count;
    default:
        qemu_log_mask(LOG_UNIMP, "awacs: read from unknown register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void awacs_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    AWACSState *s = AWACS(opaque);
    uint32_t reg = addr & 0xff;

    switch (reg) {
    case AWACS_SOUND_CTRL:
        s->sound_ctrl = val;
        {
            int sr_id = (val >> 8) & 7;
            int rate = awacs_sample_rates[sr_id];

            if (rate != s->cur_sample_rate) {
                awacs_open_voice(s, rate);
            }
        }
        break;
    case AWACS_CODEC_CTRL:
        s->codec_ctrl = val;
        break;
    case AWACS_CODEC_STATUS:
        /* read-only on real hardware */
        break;
    case AWACS_CLIP_COUNT:
        s->clip_count = val;
        break;
    case AWACS_BYTE_SWAP:
        s->byte_swap = val;
        break;
    case AWACS_FRAME_COUNT:
        s->frame_count = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "awacs: write to unknown register 0x%"
                      HWADDR_PRIx "\n", addr);
        break;
    }
}

static const MemoryRegionOps awacs_ops = {
    .read = awacs_read,
    .write = awacs_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void awacs_reset(DeviceState *dev)
{
    AWACSState *s = AWACS(dev);

    s->sound_ctrl = 0;
    s->codec_ctrl = 0;
    s->clip_count = 0;
    s->byte_swap = 0;
    s->frame_count = 0;
    s->dma_in_status = 0x10;
    s->out_fifo_rptr = 0;
    s->out_fifo_wptr = 0;
    s->out_fifo_count = 0;
}

static void awacs_init(Object *obj)
{
    AWACSState *s = AWACS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &awacs_ops, s, "awacs",
                          AWACS_REG_SIZE);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->dma_irq);
}

static void awacs_realize(DeviceState *dev, Error **errp)
{
    AWACSState *s = AWACS(dev);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
}

static void awacs_unrealize(DeviceState *dev)
{
    AWACSState *s = AWACS(dev);

    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
    }
}

static const VMStateDescription vmstate_awacs = {
    .name = "awacs",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sound_ctrl, AWACSState),
        VMSTATE_UINT32(codec_ctrl, AWACSState),
        VMSTATE_UINT32(clip_count, AWACSState),
        VMSTATE_UINT32(byte_swap, AWACSState),
        VMSTATE_UINT32(frame_count, AWACSState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property awacs_properties[] = {
    DEFINE_AUDIO_PROPERTIES(AWACSState, audio_be),
};

static void awacs_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, awacs_reset);
    dc->vmsd = &vmstate_awacs;
    dc->realize = awacs_realize;
    dc->unrealize = awacs_unrealize;
    device_class_set_props(dc, awacs_properties);
}

static const TypeInfo awacs_type_info = {
    .name = TYPE_AWACS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AWACSState),
    .instance_init = awacs_init,
    .class_init = awacs_class_init,
};

static void awacs_register_types(void)
{
    type_register_static(&awacs_type_info);
}

type_init(awacs_register_types)
