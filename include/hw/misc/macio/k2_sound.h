/*
 * K2 mac-io feature control registers and I2S-a sound cell
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MACIO_K2_SOUND_H
#define HW_MISC_MACIO_K2_SOUND_H

#include "hw/i2c/i2c.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "system/memory.h"

/* FCR10..FCR6 at 0x24..0x34, FCR0..FCR5 at 0x38..0x4c */
#define K2_FCR_BASE                 0x24
#define K2_FCR_COUNT                11
#define K2_FCR1                     0x3c

#define K2_FCR1_I2S0_CELL_ENABLE    0x00000400
#define K2_FCR1_I2S0_RESET          0x00000800
#define K2_FCR1_I2S0_CLK_ENABLE     0x00001000
#define K2_FCR1_I2S0_ENABLE         0x00002000

#define K2_I2S_BASE                 0x10000
#define K2_I2S_SIZE                 0x100

/* DBDMA register blocks of i2s-a, relative to the mac-io DBDMA base */
#define K2_I2S_DMA_OUT_OFFSET       0x000
#define K2_I2S_DMA_IN_OFFSET        0x100

#define K2_I2S_TX_DMA_IRQ           0x01
#define K2_I2S_RX_DMA_IRQ           0x02

struct DBDMA_io;

typedef struct K2I2SDir {
    QEMUTimer *timer;
    struct DBDMA_io *pending;
    int64_t deadline_ns;
} K2I2SDir;

typedef struct K2SoundState {
    MemoryRegion fcr_mem;
    MemoryRegion i2s_mem;
    uint32_t fcr[K2_FCR_COUNT];

    uint32_t intr_ctl;
    uint32_t serial_format;
    uint32_t data_word_sizes;
    uint64_t walk_frames;
    uint32_t frame_count_bias;
    K2I2SDir dir[2];            /* 0 out, 1 in */

    AudioBackend *audio_be;
    I2CSlave *codec;
    SWVoiceOut *voice;
    int voice_rate;
    int voice_frame_bytes;
    uint8_t out_fifo[0x80000];
    uint32_t fifo_rptr;
    uint32_t fifo_wptr;
    uint32_t fifo_count;
    bool prebuffering;
    int64_t last_push_ns;
} K2SoundState;

void k2_sound_init(K2SoundState *s, DeviceState *owner, MemoryRegion *bar);
void k2_sound_register_dma(K2SoundState *s, void *dbdma,
                           qemu_irq tx_irq, qemu_irq rx_irq);
void k2_sound_reset(K2SoundState *s);

#endif
