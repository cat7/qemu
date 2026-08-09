/*
 * KeyLargo MacIO cells: Feature Control Registers, I2S and the "Keywest"
 * I2C controller.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef KEYLARGO_H
#define KEYLARGO_H

#include "system/memory.h"
#include "hw/i2c/i2c.h"
#include "qemu/audio.h"

/* FCR1 bits (Linux keylargo.h, Apple AudioI2SHardwareConstants.h) */
#define KL1_AUDIO_CELL_ENABLE       0x00000040
#define KL1_AUDIO_CHOOSE            0x00000080
#define KL1_I2S0_CHOOSE             0x00000200
#define KL1_I2S0_CELL_ENABLE        0x00000400
#define KL1_I2S0_RESET              0x00000800  /* active high */
#define KL1_I2S0_CLK_ENABLE_BIT     0x00001000
#define KL1_I2S0_ENABLE             0x00002000
#define KL1_I2S1_CELL_ENABLE        0x00020000
#define KL1_I2S1_RESET              0x00040000  /* active high */
#define KL1_I2S1_CLK_ENABLE_BIT     0x00080000
#define KL1_I2S1_ENABLE             0x00100000

/* I2S cells */
#define KEYLARGO_I2S0_BASE          0x10000
#define KEYLARGO_I2S_STRIDE         0x01000
#define KEYLARGO_I2S_SIZE           0x00100

#define I2S_REG_INT_CTL             0x00
#define I2S_REG_SERIAL_FORMAT       0x10
#define I2S_REG_CODEC_MSG_OUT       0x20
#define I2S_REG_CODEC_MSG_IN        0x30
#define I2S_REG_FRAME_COUNT         0x40
#define I2S_REG_FRAME_MATCH         0x50
#define I2S_REG_DATA_WORD_SIZES     0x60
#define I2S_REG_PEAK_LEVEL_SEL      0x70

/*
 * Interrupt Control is pairs of (enable, pending) bits. Bit 24 is
 * "clocks stopped pending" -- the one firmware waits on before it may
 * reprogram the serial format. Pending bits are write-1-to-clear.
 */
#define I2S_INT_CLOCKS_STOPPED_PENDING  0x01000000
#define I2S_INT_PENDING_MASK            0x55550000

/* Keywest I2C (Linux arch/powerpc/platforms/powermac/low_i2c.c) */
#define KEYLARGO_I2C_BASE           0x18000
#define KEYLARGO_I2C_SIZE           0x01000

#define KW_I2C_REG_MODE             0x00
#define KW_I2C_REG_CONTROL          0x10
#define KW_I2C_REG_STATUS           0x20
#define KW_I2C_REG_ISR              0x30
#define KW_I2C_REG_IER              0x40
#define KW_I2C_REG_ADDR             0x50
#define KW_I2C_REG_SUBADDR          0x60
#define KW_I2C_REG_DATA             0x70

#define KW_I2C_MODE_DUMB            0x00
#define KW_I2C_MODE_STANDARD        0x04
#define KW_I2C_MODE_STANDARDSUB     0x08
#define KW_I2C_MODE_COMBINED        0x0c
#define KW_I2C_MODE_MODE_MASK       0x0c

#define KW_I2C_CTL_AAK              0x01
#define KW_I2C_CTL_XADDR            0x02
#define KW_I2C_CTL_STOP             0x04
#define KW_I2C_CTL_START            0x08

#define KW_I2C_STAT_BUSY            0x01
#define KW_I2C_STAT_LAST_AAK        0x02
#define KW_I2C_STAT_LAST_RW         0x04
#define KW_I2C_STAT_SDA             0x08
#define KW_I2C_STAT_SCL             0x10

#define KW_I2C_IRQ_DATA             0x01
#define KW_I2C_IRQ_ADDR             0x02
#define KW_I2C_IRQ_STOP             0x04
#define KW_I2C_IRQ_START            0x08
#define KW_I2C_IRQ_MASK             0x0f

typedef struct KeyLargoI2SState {
    MemoryRegion mem;
    int cell;
    uint32_t intr_ctl;
    uint32_t serial_format;
    uint32_t data_word_sizes;
    uint32_t frame_count;

    /*
     * Sound DMA has to be paced to real playback time. Completing a
     * descriptor the instant it is handed over makes a looping audio ring
     * -- which is exactly what the boot chime is -- run as fast as the host
     * can spin, burning CPU forever instead of playing once.
     */
    QEMUTimer *out_complete_timer;
    struct DBDMA_io *pending_out_io;
    int64_t play_deadline_ns;

    /*
     * Host playback: the paced DMA above already throttles the guest to
     * real time, so the samples only need tapping into a FIFO that the
     * audio backend's pull callback drains (same shape as awacs.c, which
     * needed the FIFO for the same reason: the backend pulls on its own
     * schedule).
     */
    AudioBackend *audio_be;      /* borrowed from the owning device */
    SWVoiceOut *voice;
    int voice_rate;
    uint8_t out_fifo[0x80000];
    uint32_t fifo_rptr;
    uint32_t fifo_wptr;
    uint32_t fifo_count;
    /*
     * Playout cushion (same shape as awacs.c): a freshly (re)started
     * stream only begins draining once the FIFO holds a real cushion of
     * audio, so the backend never underruns between the guest's bursty
     * DMA pushes -- an underrun makes CoreAudio loop its stale ring
     * buffer (heard as the last fraction of a second repeating) and
     * every restart pops.
     */
    bool prebuffering;
    int64_t last_push_ns;
} KeyLargoI2SState;

typedef struct KeyLargoI2CState {
    MemoryRegion mem;
    const char *name;
    I2CBus *bus;
    /*
     * Interrupt output, asserted while (ISR & IER) is nonzero. May be
     * left unwired (NULL) on cells nothing listens to. Open Firmware
     * drives the controller purely by polling ISR, but Mac OS X's
     * AppleI2C does interrupt-mode transfers -- without this line every
     * transfer it attempts times out ("APPLE I2C WRITE SEMAPHORE
     * EXCEEDED TIMEOUT" on the kernel console).
     */
    qemu_irq irq;
    bool xfer_active;
    /*
     * The vCPU whose transfer is currently on the bus (valid only while
     * xfer_active). Open Firmware's I2C probing is bare-metal register
     * pokes with no software semaphore, so on a cold SMP boot both CPUs
     * can address this single physical controller within microseconds
     * of each other; this is what lets keywest_i2c_write() tell "my own
     * follow-up write" apart from "the other CPU is still waiting its
     * turn" instead of one tearing down the other's in-flight transfer.
     * See the pending_* fields and keywest_i2c_promote_pending().
     */
    CPUState *owner;
    bool pending_valid;
    CPUState *pending_owner;
    uint8_t pending_mode;
    uint8_t pending_addr;
    uint8_t pending_subaddr;
    /*
     * DUMB ("manual") mode's START control bit asserts a bare I2C start
     * condition with no address yet -- unlike XADDR, which addresses the
     * bus itself from the ADDR/SUBADDR registers. This is set between a
     * START and the driver's first DATA write, which supplies the address
     * byte by hand; see keywest_i2c_write()'s CONTROL and DATA cases.
     */
    bool manual_addr_pending;
    /*
     * DUMB mode's read completion is one ISR-ack later than every other
     * mode's: the address-ack fires IRQ_DATA "empty" first (no byte pre-
     * fetched, unlike XADDR-triggered reads), the driver's first ack of
     * that is what actually clocks the byte in, and only its *second* ack
     * (once the driver has consumed it) should auto-stop the bus. This
     * distinguishes those two acks; see keywest_i2c_write()'s ISR case.
     */
    bool manual_byte_delivered;
    uint8_t mode;
    uint8_t control;
    uint8_t status;
    uint8_t isr;
    uint8_t ier;
    uint8_t addr;
    uint8_t subaddr;
    uint8_t data;
} KeyLargoI2CState;

typedef struct KeyLargoState {
    MemoryRegion fcr_mem;
    uint32_t fcr[7];               /* MBCR 0x34, FCR0..FCR5 0x38..0x4c */
    KeyLargoI2SState i2s[2];
    KeyLargoI2CState i2c;
} KeyLargoState;

#define UNINORTH_I2C_BASE           0xf8001000
#define UNINORTH_I2C_SIZE           0x00001000

void keywest_i2c_init(KeyLargoI2CState *c, DeviceState *owner,
                      const char *name, uint64_t size);
/* i2s-a owns DBDMA channels 0 (out) and 1 (in); i2s-b owns 2 and 3. */
#define KEYLARGO_I2S_DMA_OUT(cell)  ((cell) * 2)
#define KEYLARGO_I2S_DMA_IN(cell)   ((cell) * 2 + 1)

void keylargo_i2s_register_dma(KeyLargoState *s, void *dbdma);
void keylargo_i2s_update_clocks(KeyLargoState *s);
KeyLargoState *keylargo_cells_init(DeviceState *owner, MemoryRegion *bar);

#endif /* KEYLARGO_H */
