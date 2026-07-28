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
#include "qemu/audio.h"

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
    uint32_t frame_count;

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
    uint32_t out_fifo_rptr;
    uint32_t out_fifo_wptr;
    uint32_t out_fifo_count;
};

void awacs_register_dma(AWACSState *s, void *dbdma);

#endif /* HW_AUDIO_AWACS_H */
