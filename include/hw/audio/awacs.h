/*
 * QEMU Apple AWACS/Screamer sound codec (macio "davbus")
 *
 * As found in Beige Power Mac G3 and other Old/New World PowerMacs,
 * inside the Heathrow (macio) chip.
 *
 * Register layout confirmed against DingusPPC's AwacsScreamer model
 * (devices/sound/awacs.{h,cpp}). Six 32-bit registers, addressed
 * directly (no scaling) at byte offsets 0x00/0x10/0x20/0x30/0x40/0x50 --
 * confirmed via the actual dispatch code for *this* machine specifically
 * (DingusPPC's Gossamer machine backs its "Heathrow" device with
 * MacIoTwo (devices/ioctrl/maciotwo.cpp), whose sound-codec read/write
 * pass `offset & 0xFF` straight through with no shift; grandcentral.cpp,
 * which does use a different convention, is a separate IO controller for
 * a different machine generation and doesn't apply here). Real hardware's
 * own device-tree ("davbus" node) declares this register block as 0x1000
 * bytes at macio offset 0x14000, immediately followed by swim3 at
 * 0x15000 -- a stray shift here previously made this region size 0x2000,
 * silently overlapping swim3's real address range.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_AUDIO_AWACS_H
#define HW_AUDIO_AWACS_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/ppc/mac_dbdma.h"
#include "qemu/audio.h"
#include "qemu/timer.h"

#define TYPE_AWACS "awacs"
OBJECT_DECLARE_SIMPLE_TYPE(AWACSState, AWACS)

/* Matches real hardware's device-tree "reg" size for davbus (0x1000),
 * ending exactly where swim3 begins (0x15000) -- see comment above. */
#define AWACS_REG_SIZE      0x1000

/* Register byte offsets, used directly (no scaling) -- see comment above. */
#define AWACS_SOUND_CTRL    0x00
#define AWACS_CODEC_CTRL    0x10
#define AWACS_CODEC_STATUS  0x20
#define AWACS_CLIP_COUNT    0x30
#define AWACS_BYTE_SWAP     0x40
#define AWACS_FRAME_COUNT   0x50

/* Codec Status Register fields (matches DingusPPC's AwacsScreamer) */
#define AWACS_STATUS_AVAILABLE   0x40  /* codec present/ready, bits 8-15 */
#define AWACS_MAKER_CRYSTAL      1     /* manufacturer id, bits 16-19 */
#define AWACS_REV_SCREAMER       3     /* chip revision, bits 20-23 */

/*
 * SOUND_CTRL sample-rate field. These registers are little-endian on
 * the bus (the guest driver accesses them with byte-reversed
 * load/stores through this DEVICE_BIG_ENDIAN-declared region), so the
 * true register value is bswap32() of the raw MMIO value; the rate
 * field is bits 8-10 of that -- kHWRateMask/kHWRateShift in Apple's
 * own awacs_OWhw.h (apple-oss-distributions/AppleOnboardAudio,
 * AppleLegacyAudio/AppleOWScreamerAudio -- the Old World Screamer
 * driver for exactly this hardware), same field Linux's
 * sound/ppc/awacs.h calls MASK_RATE. Codes index the 44.1 kHz-family
 * table (awacs_sample_rates): 0=44100, 2=22050, 5=11025, ...
 *
 * Behavior confirmed live from Mac OS 8.5: the classic driver writes
 * the stream's rate here when a sound starts (e.g. 0x4211 = code 2 for
 * a 22.05 kHz stream) and parks the codec at code 5 (11025) when
 * output goes idle -- so rate writes arrive both at stream start and
 * right after a sound finishes, while our FIFO can still hold that
 * sound's tail (we buffer ahead of real playback; real hardware holds
 * only a few samples). The voice reopen is therefore deferred until
 * the FIFO drains (see pending_rate).
 *
 * (Codec register 1 bits 3-5 look like a rate field too -- Linux calls
 * them MASK_SAMPLERATE -- but the classic driver leaves stale values
 * there that don't track the output rate, and Apple's own awacs_OWhw.h
 * comments them "!!!Do these bits do anything???". Not used.)
 */
#define AWACS_CTRL_RATE_SHIFT    8
#define AWACS_CTRL_RATE_MASK     0x7

struct AWACSState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion mem;
    qemu_irq irq;
    qemu_irq dma_irq;

    uint32_t sound_ctrl;
    uint32_t codec_ctrl;
    uint32_t clip_count;
    uint32_t byte_swap;

    /*
     * Free-running frame counter (FRAME_COUNT register). Real hardware
     * counts serial-bus frames continuously once the clocks run; the
     * classic Mac OS driver polls it (a handful of reads with a retry
     * cap) to confirm the codec is alive before acting on a sample-rate
     * change -- with a counter stuck at 0 those polls time out and the
     * driver silently abandons rate reprogramming (confirmed live: only
     * the first post-boot quality switch was ever written to the
     * hardware). Modeled as time-based: value = base_val + elapsed
     * virtual time * cur_sample_rate.
     */
    uint32_t frame_count_base_val;
    int64_t frame_count_base_ns;

    /*
     * Rotating single-bit "device status" value the audio-in DBDMA
     * channel ORs into the low byte of the channel's DBDMA status
     * register on each serviced transfer, mirroring DingusPPC's
     * AwacsBase::dma_in_data() (devices/sound/awacs.cpp): a probe's
     * BR_IFSET/BR_IFCLR descriptor condition tests this byte, and with
     * nothing ever changing it the condition is static forever and the
     * probe's descriptor ring (a real, by-design infinite loop otherwise)
     * never takes its exit branch. Starts at 0x10 and left-shifts each
     * transfer, wrapping 0x80 back to 0x01, so bit 0 genuinely becomes
     * set every 8 transfers -- same cadence DingusPPC uses.
     */
    uint8_t dma_in_status;

    /*
     * Real audio output (see hw/audio/awacs.c). DMA hands us samples
     * in a single synchronous burst (all descriptors in one guest
     * "kick" are serviced back-to-back with no return to QEMU's main
     * loop in between), much faster than real playback time -- writing
     * straight to the audio backend from awacs_dma_rw overflows its own
     * small internal buffer after the first chunk and silently drops
     * the rest. Buffering here and draining through the backend's own
     * pull callback (awacs_audio_callback), which fires from QEMU's
     * real-time-paced audio timer, avoids that.
     */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    int cur_sample_rate;
    /* Rate written by the guest but not yet applied to the voice --
     * applied only when DMA data actually arrives while the voice runs
     * at a different rate. Applying at write time is wrong twice over:
     * it would retime audio still buffered from the previous stream,
     * and the classic driver parks the codec at 11025 whenever output
     * goes idle and unparks at the next sound, so eager application
     * would tear down and reopen the host voice around every single
     * sound (audibly glitchy on coreaudio). 0 = none pending. */
    int pending_rate;

    /*
     * Playout cushion. The Sound Manager streams through a small
     * ping-pong (observed: 2 x 2048 bytes = ~23 ms total), so the
     * guest can never run more than that ahead and the FIFO trough
     * between refills is a few ms -- less than one audio-timer period.
     * Real hardware doesn't care (no host in the loop); for us any
     * host callback jitter empties the FIFO and playback stutters.
     * When a stream starts (or restarts after the FIFO emptied), hold
     * the backend off until ~30 ms is buffered (or the guest stops
     * pushing, so short one-shot sounds still play) -- a one-time,
     * inaudible added latency that keeps the steady-state trough above
     * the jitter floor.
     */
    bool prebuffering;
    int64_t last_push_ns;
    /*
     * A single DBDMA descriptor chain for one chime is ~404KB on real
     * hardware (confirmed against DingusPPC's own captured chime
     * audio: 12 descriptors of 0x8000 bytes + one of 0x2b60), and
     * awacs_dma_rw's fix to actually consume a descriptor's full
     * length (rather than silently truncating it, see that function's
     * comment) means all of it can arrive in this FIFO in one
     * synchronous burst before the backend's pull callback gets a
     * chance to drain any of it. Sized with real headroom above that.
     */
#define AWACS_OUT_FIFO_SIZE (512 * 1024)
    uint8_t out_fifo[AWACS_OUT_FIFO_SIZE];

    /*
     * Diagnostic: when the "dumpfile" property is set, every byte actually
     * handed to the audio backend is appended here. Unlike running with
     * `-audiodev wav`, this records the stream in the REAL configuration --
     * with coreaudio attached and pacing us -- so an artefact that only
     * appears against a live device is still captured.
     */
    char *dump_path;
    FILE *dump_fp;
    uint32_t out_fifo_rptr;
    uint32_t out_fifo_wptr;
    uint32_t out_fifo_count;

    /*
     * Real-time pacing of DBDMA-out completion. On real hardware the
     * channel's completion interrupt fires when the buffer has actually
     * been clocked out to the codec, not the instant the data is read
     * from memory. Classic Mac OS's Sound Manager double-buffers short
     * sounds and swaps buffers on that interrupt, so completing a
     * descriptor instantly makes it mispace the swaps and the sound
     * comes out chopped into fragments. Hold each descriptor's
     * completion until its samples would have finished playing (minus a
     * small lookahead so the next buffer arrives before the FIFO
     * underruns).
     */
    QEMUTimer *out_complete_timer;
    DBDMA_io *pending_out_io;
    int64_t play_deadline_ns;
};

void awacs_register_dma(AWACSState *s, void *dbdma);

#endif /* HW_AUDIO_AWACS_H */
