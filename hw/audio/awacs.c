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
#include "qemu/error-report.h"
#include "system/dma.h"
#include "trace.h"

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

    s->last_push_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (i = 0; i < len; i++) {
        if (s->out_fifo_count >= AWACS_OUT_FIFO_SIZE) {
            trace_awacs_fifo_overflow(len - i);
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
/*
 * One audio frame is stereo 16-bit = 4 bytes. Every write to the
 * backend MUST be a whole number of frames: a partial-frame write
 * desynchronises the interleaved L/R stream for everything that
 * follows (each subsequent 16-bit sample is split across two output
 * samples and the channels swap), which manifests as harsh, noisy
 * playback -- most audible on the short, intermittent system alert
 * sounds, and not on a single continuous stream like the boot chime
 * that happens to stay aligned throughout. Neither the backend's
 * reported `avail` nor its accepted `written` count is guaranteed
 * frame-aligned, so we round both down here.
 */
#define AWACS_FRAME_BYTES 4

/* Playout cushion (see the field comment in awacs.h): target depth the
 * FIFO must reach before a freshly (re)started stream begins draining,
 * and how long to keep waiting for it once the guest stops pushing. */
#define AWACS_PREBUF_NS         (60 * 1000 * 1000)
#define AWACS_PREBUF_GIVEUP_NS  (100 * 1000 * 1000)

/*
 * Fill the backend with silence while we have no samples to give it.
 * Backends with a looping ring (DirectSound most visibly) otherwise
 * keep replaying whatever the ring last held once the stream ends --
 * heard as the tail of the boot chime repeating forever. Real AWACS
 * hardware's DAC keeps emitting silence between streams; do the same.
 * (coreaudio solves this inside its own render callback; this covers
 * every other backend generically.)
 */
static void awacs_write_silence(AWACSState *s, int avail)
{
    static const uint8_t zeros[1024];

    avail -= avail % AWACS_FRAME_BYTES;
    while (avail >= AWACS_FRAME_BYTES) {
        int chunk = MIN(avail, (int)sizeof(zeros));
        size_t written = audio_be_write(s->audio_be, s->voice,
                                        (void *)zeros, chunk);

        written -= written % AWACS_FRAME_BYTES;
        if (!written) {
            break;
        }
        avail -= written;
    }
}

static void awacs_pull_pending(AWACSState *s, int64_t now);

static void awacs_audio_callback(void *opaque, int avail)
{
    AWACSState *s = AWACS(opaque);

    awacs_pull_pending(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    trace_awacs_cb(avail, s->out_fifo_count,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    if (s->out_fifo_count == 0) {
        /* Stream drained (or never started): next data prebuffers. */
        s->prebuffering = true;
        awacs_write_silence(s, avail);
        return;
    }
    if (s->prebuffering) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int rate = s->cur_sample_rate ? s->cur_sample_rate : 44100;
        uint32_t want = (uint64_t)rate * AWACS_FRAME_BYTES *
                        AWACS_PREBUF_NS / NANOSECONDS_PER_SECOND;

        if (s->out_fifo_count < want &&
            now - s->last_push_ns < AWACS_PREBUF_GIVEUP_NS) {
            /* No silence-fill here: zeros written while a stream is
             * gathering would queue ahead of its real samples and add
             * ring-depth latency to every stream start. The backend
             * ring already holds our own trailing silence from the
             * drained state above, so there is nothing stale to loop. */
            return;
        }
        s->prebuffering = false;
    }

    avail -= avail % AWACS_FRAME_BYTES;

    while (avail >= AWACS_FRAME_BYTES &&
           s->out_fifo_count >= AWACS_FRAME_BYTES) {
        uint8_t staging[4096];
        int chunk = MIN(avail, (int)sizeof(staging));
        int i;
        size_t written;

        chunk = MIN(chunk, (int)s->out_fifo_count);
        chunk -= chunk % AWACS_FRAME_BYTES;
        for (i = 0; i < chunk; i++) {
            staging[i] = s->out_fifo[s->out_fifo_rptr];
            s->out_fifo_rptr = (s->out_fifo_rptr + 1) % AWACS_OUT_FIFO_SIZE;
        }
        s->out_fifo_count -= chunk;

        written = audio_be_write(s->audio_be, s->voice, staging, chunk);
        if (s->dump_fp && written) {
            fwrite(staging, 1, written, s->dump_fp);
        }
        trace_awacs_cb_write(chunk, written, s->out_fifo_count);
        /* Only advance by whole frames; hold back any partial-frame
         * tail the backend accepted so the stream never desyncs. */
        written -= written % AWACS_FRAME_BYTES;
        avail -= written;
        if (written < (size_t)chunk) {
            /* Backend couldn't take it all -- put the unwritten tail
             * back at the front of the FIFO and stop for now. */
            s->out_fifo_rptr = (s->out_fifo_rptr + AWACS_OUT_FIFO_SIZE -
                               (chunk - written)) % AWACS_OUT_FIFO_SIZE;
            s->out_fifo_count += chunk - written;
            break;
        }
    }
}

/* Apply the codec's speaker path (attenuation + mute) to the host voice. */
static void awacs_update_volume(AWACSState *s)
{
    uint32_t att = s->codec_regs[4];
    int left = 0xf - ((att >> 6) & 0xf);
    int right = 0xf - (att & 0xf);
    /*
     * Register 1 bit 9 mutes the speaker path, bit 7 the headphone path.
     * Determined from the guests themselves: the ROM sets both only after
     * the chime has finished, while Mac OS 9.0.4 and 9.2 keep bit 7 set
     * for as long as they play music through the speaker.
     */
    bool mute = s->codec_regs[1] & 0x200;

    trace_awacs_volume(mute, left, right);
    if (s->voice) {
        audio_be_set_volume_out_lr(s->audio_be, s->voice, mute,
                                   left * 255 / 15, right * 255 / 15);
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

    trace_awacs_open_voice(sample_rate);
    s->cur_sample_rate = sample_rate;
    s->voice = audio_be_open_out(s->audio_be, s->voice, "awacs.out", s,
                                 awacs_audio_callback, &as);
    audio_be_set_active_out(s->audio_be, s->voice, true);
    awacs_update_volume(s);
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
 * DMA-out model: each descriptor gets a read clock. Its data is read
 * from guest memory at the sample rate from t_start -- the position a
 * real DBDMA engine feeding a real codec would have reached -- and it
 * completes when that clock reaches its end. Reading at playback pace
 * is not an optimisation, it is the contract classic Mac OS relies on:
 * Mac OS 9.2's Sound Manager restarts the channel on a 16 KB buffer and
 * only then mixes into it, a few ms ahead of where the DMA pointer can
 * be. Copying the buffer at arm time (as an earlier version did) read
 * 16 KB of zeros every cycle; the stream was gapless, real-time and
 * completely silent (measured: 185 s of zeros after the chime).
 *
 * The ROM's boot chime used to look like a counter-example -- the ROM
 * appeared to zero the head of the chime's second descriptor ~12 ms
 * after arming the chain, so that descriptor had to be copied early.
 * It was not: the ROM spins on the channel's STATUS register until
 * ACTIVE drops (810 k reads during one chime), and mac_dbdma reported
 * ACTIVE clear while a device transfer was in flight, so the ROM saw
 * the chime "finish" 12 ms in and reused the RAM. With ACTIVE reported
 * truthfully for a busy transfer (io->device_busy) the ROM waits for
 * the real end, and a purely lazy read yields the byte-identical chime.
 *
 * A stop (RUN cleared) halts in place: nothing the clock has not
 * reached is ever read. A restart within AWACS_RESUME_GRACE_NS
 * continues the clock where the previous command's audio ended, so the
 * guest's interrupt-to-restart latency (~1 ms here, tens of us on
 * hardware) does not open a hole in the audio every buffer.
 */
#define AWACS_RESUME_GRACE_NS (5 * 1000 * 1000)
/*
 * A descriptor armed from inside our own completion is the chain's next
 * link and inherits the clock no matter how late the host delivered
 * that completion: the DMA engine it models never loses time. Without
 * this, every main-loop stall beyond the grace (15-50 ms spikes, about
 * one a second on this host) re-based the clock to "now" and the stream
 * fell behind real time for good -- measured 0.984x on Mac OS X's
 * free-running ring, draining the cushion within ~40 s and then
 * underrunning every 90 ms (heard as garbling). The catch-up after a
 * stall is bounded so a long pause (a debugger, a suspended host) does
 * not turn into a burst.
 */
#define AWACS_MAX_CATCHUP_NS (200 * 1000 * 1000)

/*
 * How often the pending descriptor's data is pulled from guest memory.
 * The backend callback alone runs every ~10 ms with host jitter to
 * ~20 ms, so a pull there reads bytes up to that long after the read
 * clock passed them. Mac OS X's audio engine erases its ring behind
 * the play head with only a small margin: a late read lands on freshly
 * erased zeros (heard as garbling that sets in once the engine's clock
 * model has settled, ~30 s into a song). Real hardware reads every byte
 * at its moment; 2 ms is the closest cheap approximation.
 */
#define AWACS_PULL_PERIOD_NS (2 * 1000 * 1000)

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
static void awacs_out_complete(void *opaque);
static int64_t awacs_byte_rate(AWACSState *s);
static void awacs_read_guest(AWACSState *s, hwaddr addr, uint32_t len);

static void awacs_dma_rw(DBDMA_io *io)
{
    AWACSState *s = AWACS(io->opaque);

    /* Deferred rate changes (see pending_rate in awacs.h) apply when
     * stream data actually arrives -- any leftover previous-stream
     * tail is small (the completion-pacing margin). Also bootstraps
     * the voice if data ever arrives before any rate write. */
    if (s->pending_rate && s->pending_rate != s->cur_sample_rate) {
        awacs_open_voice(s, s->pending_rate);
    } else if (!s->voice) {
        awacs_open_voice(s, s->cur_sample_rate ? s->cur_sample_rate : 44100);
    }
    s->pending_rate = 0;

    trace_awacs_dma_out(io->addr, io->len, s->cur_sample_rate,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

    if (!io->is_dma_out) {
        io->len = 0;
        io->dma_end(io);
        return;
    }

    {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t byte_rate = awacs_byte_rate(s);
        int64_t dur_ns = (int64_t)io->len * NANOSECONDS_PER_SECOND / byte_rate;
        int64_t fire_ns;

        s->pending_out_io = io;
        io->device_busy = true;      /* STATUS shows ACTIVE while paced */
        s->pending_out_len = io->len;
        s->pending_addr = io->addr;
        s->pending_ppos = 0;
        /* Continue the previous command's clock: a chained descriptor
         * starts when its predecessor ends, and a restart within the
         * grace period picks up where the stopped command's audio
         * ended. Anything later is a fresh stream starting now. */
        if (s->last_end_ns &&
            (s->in_complete || now - s->last_end_ns < AWACS_RESUME_GRACE_NS)) {
            s->pending_t_start_ns = MAX(s->last_end_ns, now - AWACS_MAX_CATCHUP_NS);
        } else {
            s->pending_t_start_ns = now;
        }
        fire_ns = s->pending_t_start_ns + dur_ns;
        trace_awacs_dma_pace(io->len, dur_ns,
                             s->pending_t_start_ns + dur_ns - now,
                             MAX(fire_ns - now, 0), s->out_fifo_count);
        if (fire_ns <= now) {
            awacs_out_complete(s);
            return;
        }
        timer_mod(s->out_complete_timer, fire_ns);
        timer_mod(s->pull_timer, now + AWACS_PULL_PERIOD_NS);
    }
}

static int64_t awacs_byte_rate(AWACSState *s)
{
    int rate = s->cur_sample_rate ? s->cur_sample_rate : 44100;

    return (int64_t)rate * AWACS_FRAME_BYTES;
}

/* Bytes of the in-flight descriptor its read clock has reached by now. */
static uint32_t awacs_pending_vpos(AWACSState *s, int64_t now)
{
    int64_t elapsed = now - s->pending_t_start_ns;
    int64_t bytes;

    if (elapsed <= 0) {
        return 0;
    }
    bytes = elapsed * awacs_byte_rate(s) / NANOSECONDS_PER_SECOND;
    bytes -= bytes % AWACS_FRAME_BYTES;
    return MIN(bytes, s->pending_out_len);
}

/*
 * Bytes of the in-flight descriptor not yet transferred. This is what a
 * DBDMA FLUSH writes back as resCount -- classic Mac OS's Sound Manager
 * polls playback position exactly that way (set Flush, read the
 * descriptor's resCount), and Mac OS X's driver does the same once per
 * ring lap.
 */
static uint32_t awacs_pending_remaining(AWACSState *s, int64_t now)
{
    return s->pending_out_len - awacs_pending_vpos(s, now);
}

/* Read [addr, addr+len) of guest memory as big-endian samples into the FIFO. */
static void awacs_read_guest(AWACSState *s, hwaddr addr, uint32_t len)
{
    uint8_t buf[4096];

    while (len > 0) {
        int chunk = MIN(len, sizeof(buf));
        int i;

        dma_memory_read(&address_space_memory, addr, buf, chunk,
                        MEMTXATTRS_UNSPECIFIED);
        /* Samples are 16-bit big-endian in memory; the voice was opened
         * host-endian, so convert. */
        for (i = 0; i + 1 < chunk; i += 2) {
            uint16_t v;
            memcpy(&v, &buf[i], 2);
            v = be16_to_cpu(v);
            memcpy(&buf[i], &v, 2);
        }
        awacs_fifo_push(s, buf, chunk & ~1);
        addr += chunk;
        len -= chunk;
    }
}

/* Bring a lazily-read descriptor's FIFO contribution up to its clock. */
static void awacs_pull_pending(AWACSState *s, int64_t now)
{
    uint32_t vpos;

    if (!s->pending_out_io) {
        return;
    }
    vpos = awacs_pending_vpos(s, now);
    if (vpos > s->pending_ppos) {
        awacs_read_guest(s, s->pending_addr + s->pending_ppos,
                         vpos - s->pending_ppos);
        s->pending_ppos = vpos;
    }
}

static void awacs_pull_tick(void *opaque)
{
    AWACSState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    awacs_pull_pending(s, now);
    if (s->pending_out_io) {
        timer_mod(s->pull_timer, now + AWACS_PULL_PERIOD_NS);
    }
}

static void awacs_out_complete(void *opaque)
{
    AWACSState *s = opaque;
    DBDMA_io *io = s->pending_out_io;
    DBDMA_channel *ch;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!io) {
        return;
    }
    s->pending_out_io = NULL;
    ch = io->channel;
    /*
     * The guest may have stopped, reset or reprogrammed the channel
     * since this descriptor was paced (a stop clears io.processing; a
     * reset clears the whole register block). Completing into that
     * would write descriptor status wherever the command pointer now
     * points and could chain into the guest's fresh program.
     */
    if (!(ch->regs[DBDMA_STATUS] & RUN) || !ch->io.processing) {
        trace_awacs_out_complete_drop(ch->regs[DBDMA_STATUS],
                                      ch->io.processing);
        return;
    }
    if (s->pending_ppos < s->pending_out_len) {
        /* Completion at its clock's end: take the tail now. */
        awacs_read_guest(s, s->pending_addr + s->pending_ppos,
                         s->pending_out_len - s->pending_ppos);
        s->pending_ppos = s->pending_out_len;
    }
    s->last_end_ns = s->pending_t_start_ns +
                     (int64_t)s->pending_out_len * NANOSECONDS_PER_SECOND /
                     awacs_byte_rate(s);
    trace_awacs_out_complete_fire(now);
    io->len = 0;                     /* fully transferred: resCount 0 */
    s->in_complete = true;
    io->dma_end(io);
    s->in_complete = false;
}

/*
 * DBDMA calls this from a CONTROL write that sets FLUSH, clears RUN or
 * sets PAUSE (and from channel reset). What the guest is asking for
 * differs, and the distinction is audible:
 *
 * FLUSH (and PAUSE) with the channel still running is a position
 * query, not an abort. Real hardware writes the current command's
 * xferStatus/resCount back and carries on transferring. Mac OS 9.2's
 * Sound Manager issues one ~10 ms after arming every 16 KB buffer;
 * Mac OS X's driver issues one per ring lap. An earlier version of this
 * function answered every flush by completing the in-flight descriptor
 * instead -- so 9.2 saw each buffer "finish" instantly, queued the
 * next, and streamed a whole song into the FIFO in a tenth of a second
 * (measured: 48 s of audio in 0.13 s, 31 MB dropped on FIFO overflow,
 * only sporadic fragments audible), while OS X replayed 6-12 ms of
 * ring every lap. Here the descriptor's residual is snapshotted into
 * io->len, which mac_dbdma writes back as resCount, and the paced
 * completion stays armed.
 *
 * RUN cleared is a stop, and a DBDMA stop halts the channel IN PLACE:
 * the current command stays current (CMDPTR still names it) and is
 * neither completed nor advanced -- no interrupt, no branch, no
 * write-back unless FLUSH was set too. Mac OS 9.2 depends on that: its
 * interrupt handler stops the channel while buffer B is under way,
 * works out from the channel where the DMA got to and re-arms from
 * there. An earlier version completed the stopped command and advanced
 * past it, so the pointer named the command after B and the handler
 * re-armed the previous pair -- buffer A, which the guest never fills --
 * on every cycle (measured: A read lazily over its full 93 ms was still
 * all zeros; 100+ s of zeros delivered while the song sat in B).
 */
static void awacs_dma_flush(DBDMA_io *io)
{
    AWACSState *s = AWACS(io->opaque);
    DBDMA_channel *ch = io->channel;
    uint16_t mask = ch->regs[DBDMA_CONTROL] >> 16;
    uint16_t value = ch->regs[DBDMA_CONTROL] & 0xffff;
    bool stopping = (mask & RUN) && !(value & RUN);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t remaining = 0;

    if (s->pending_out_io) {
        remaining = awacs_pending_remaining(s, now);
        io->len = remaining;
    }
    trace_awacs_dma_flush(!!s->pending_out_io, s->out_fifo_count, stopping,
                          remaining, now);
    if (!stopping) {
        return;
    }
    if (s->pending_out_io) {
        /* The audio that did go out ends here; a prompt restart
         * continues from this point. Halted in place: mac_dbdma clears
         * io.processing for a stop itself; the command is not completed
         * and nothing beyond the clock was ever read. */
        s->last_end_ns = s->pending_t_start_ns +
                         (int64_t)s->pending_ppos * NANOSECONDS_PER_SECOND /
                         awacs_byte_rate(s);
        timer_del(s->out_complete_timer);
        s->pending_out_io = NULL;
    }
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

/*
 * Free-running frame counter -- see the field comment in awacs.h. The
 * FRAME_COUNT register value is little-endian on the bus like the rest
 * of this block (the guest accesses it byte-reversed), so the computed
 * value is bswapped on read and the written value un-bswapped, keeping
 * the raw-value convention the other registers use.
 */
static uint32_t awacs_frame_count(AWACSState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int rate = s->cur_sample_rate ? s->cur_sample_rate : 44100;
    int64_t elapsed = now - s->frame_count_base_ns;

    /* Report the counter a little behind the read clock so a guest that
     * erases its ring behind the counter (Mac OS X's audio engine) never
     * erases what the DMA has not read yet. */
    elapsed -= (int64_t)s->frame_count_lag_us * 1000;
    if (elapsed < 0) {
        elapsed = 0;
    }

    /* Experiment knob (frame-count-divisor): 0 freezes the counter at
     * its written value (DingusPPC behaviour), N counts at rate/N. */
    if (s->frame_count_divisor == 0) {
        return s->frame_count_base_val;
    }
    return s->frame_count_base_val +
           (uint32_t)(elapsed * rate * s->frame_count_multiplier /
                      s->frame_count_divisor / NANOSECONDS_PER_SECOND);
}

static uint64_t awacs_read_internal(AWACSState *s, uint32_t reg, hwaddr addr);

static uint64_t awacs_read(void *opaque, hwaddr addr, unsigned size)
{
    AWACSState *s = AWACS(opaque);
    uint32_t reg = addr & 0xff;
    uint64_t ret = awacs_read_internal(s, reg, addr);

    trace_awacs_mmio_read(addr, ret, size);
    return ret;
}

static uint64_t awacs_read_internal(AWACSState *s, uint32_t reg, hwaddr addr)
{
    switch (reg) {
    case AWACS_SOUND_CTRL:
        return s->sound_ctrl;
    case AWACS_CODEC_CTRL:
        return s->codec_ctrl;
    case AWACS_CODEC_STATUS:
        /*
         * Screamer readback: with register 7 bit 0 set, the status
         * register echoes the codec register selected by bits 1-3
         * instead of the status word. Mac OS 9.0.4 probes registers
         * 0,1,2,4,5,6 this way around every alert and every volume
         * change, and gives up on the codec if the echo is missing.
         * Same raw (byte-reversed) view as the status word below.
         */
        if (s->codec_regs[7] & 1) {
            unsigned sel = (s->codec_regs[7] >> 1) & 7;

            return bswap32(s->codec_regs[sel] & 0xfff);
        }
        return (AWACS_STATUS_AVAILABLE << 8) |
               (AWACS_MAKER_CRYSTAL << 16) |
               (AWACS_REV_SCREAMER << 20);
    case AWACS_CLIP_COUNT:
        return s->clip_count;
    case AWACS_BYTE_SWAP:
        return s->byte_swap;
    case AWACS_FRAME_COUNT:
        return bswap32(awacs_frame_count(s));
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

    trace_awacs_mmio_write(addr, val, size);
    switch (reg) {
    case AWACS_SOUND_CTRL:
        s->sound_ctrl = val;
        {
            /* Rate field of the true (byteswapped) register value --
             * see the comment block in awacs.h. */
            uint32_t ctrl = bswap32((uint32_t)val);
            int sr_id = (ctrl >> AWACS_CTRL_RATE_SHIFT) &
                        AWACS_CTRL_RATE_MASK;
            int rate = awacs_sample_rates[sr_id];

            trace_awacs_set_rate(sr_id, rate);
            if (rate != s->cur_sample_rate) {
                /* Keep the free-running frame counter continuous
                 * across the rate change. */
                s->frame_count_base_val = awacs_frame_count(s);
                s->frame_count_base_ns =
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                /* Never touch the voice here -- see pending_rate in
                 * awacs.h. Applied when data arrives at this rate. */
                s->pending_rate = rate;
            } else {
                s->pending_rate = 0;
            }
        }
        break;
    case AWACS_CODEC_CTRL:
        s->codec_ctrl = val;
        {
            uint32_t ctrl = bswap32((uint32_t)val);
            unsigned reg = (ctrl >> 12) & 7;

            /* register 1 bit 2 (recalibrate) is self-clearing */
            s->codec_regs[reg] = ctrl & (reg == 1 ? 0xffb : 0xfff);
            trace_awacs_codec_write(reg, ctrl & 0xfff);
            if (reg == 1 || reg == 4) {
                awacs_update_volume(s);
            }
        }
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
        s->frame_count_base_val = bswap32((uint32_t)val);
        s->frame_count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
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
    memset(s->codec_regs, 0, sizeof(s->codec_regs));
    s->clip_count = 0;
    s->byte_swap = 0;
    s->frame_count_base_val = 0;
    s->frame_count_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->pending_rate = 0;
    s->prebuffering = true;
    s->last_push_ns = 0;
    s->dma_in_status = 0x10;
    s->out_fifo_rptr = 0;
    s->out_fifo_wptr = 0;
    s->out_fifo_count = 0;
    s->pending_out_len = 0;
    s->pending_ppos = 0;
    s->pending_addr = 0;
    s->pending_t_start_ns = 0;
    s->last_end_ns = 0;
    s->in_complete = false;
    if (s->out_complete_timer) {
        timer_del(s->out_complete_timer);
    }
    if (s->pull_timer) {
        timer_del(s->pull_timer);
    }
    s->pending_out_io = NULL;
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

static void awacs_dump_open(AWACSState *s)
{
    if (s->dump_path && *s->dump_path) {
        s->dump_fp = fopen(s->dump_path, "wb");
        if (!s->dump_fp) {
            warn_report("awacs: cannot open dumpfile %s", s->dump_path);
        } else {
            setvbuf(s->dump_fp, NULL, _IOFBF, 1 << 16);
        }
    }
}

static void awacs_realize(DeviceState *dev, Error **errp)
{
    AWACSState *s = AWACS(dev);

    awacs_dump_open(s);

    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    s->out_complete_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         awacs_out_complete, s);
    s->pull_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, awacs_pull_tick, s);
}

static void awacs_unrealize(DeviceState *dev)
{
    AWACSState *s = AWACS(dev);

    if (s->out_complete_timer) {
        timer_free(s->out_complete_timer);
        s->out_complete_timer = NULL;
    }
    if (s->pull_timer) {
        timer_free(s->pull_timer);
        s->pull_timer = NULL;
    }
    if (s->voice) {
        audio_be_close_out(s->audio_be, s->voice);
    }
}

static const VMStateDescription vmstate_awacs = {
    .name = "awacs",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sound_ctrl, AWACSState),
        VMSTATE_UINT32(codec_ctrl, AWACSState),
        VMSTATE_UINT32(clip_count, AWACSState),
        VMSTATE_UINT32(byte_swap, AWACSState),
        VMSTATE_UINT32(frame_count_base_val, AWACSState),
        VMSTATE_INT64(frame_count_base_ns, AWACSState),
        VMSTATE_INT32(pending_rate, AWACSState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property awacs_properties[] = {
    DEFINE_PROP_STRING("dumpfile", AWACSState, dump_path),
    DEFINE_PROP_UINT32("frame-count-divisor", AWACSState, frame_count_divisor, 1),
    DEFINE_PROP_UINT32("frame-count-multiplier", AWACSState, frame_count_multiplier, 1),
    /*
     * Default 0: Mac OS 9.0.4's Sound Manager plays alerts through 2 KB
     * (11.6 ms) ping-pong buffers and samples FRAME_COUNT at each buffer
     * completion; a 5 ms lag made it read ~300 frames where 516 had
     * played and it aborted every alert after the second buffer.
     */
    DEFINE_PROP_UINT32("frame-count-lag-us", AWACSState, frame_count_lag_us, 0),
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
