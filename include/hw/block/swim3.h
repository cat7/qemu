/*
 * SWIM3 (Sander-Wozniak Integrated Machine 3) floppy controller
 *
 * The floppy controller found in Old World PowerMacs' mac-io (Heathrow
 * and friends), driving one internal Apple Superdrive (auto-detect
 * MFM 720K/1440K media here). Register-level behaviour follows the
 * MESS/Linux swim3 driver's expectations and DingusPPC's
 * register-compatible model; sector data moves through DBDMA channel 1.
 *
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SWIM3_H
#define SWIM3_H

#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_SWIM3 "swim3"
OBJECT_DECLARE_SIMPLE_TYPE(Swim3State, SWIM3)

#define SWIM3_MMIO_SIZE   0x1000
#define SWIM3_NUM_REGS    16
#define SWIM3_MAX_TRACKS  80

typedef struct Swim3State {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq dma_irq;
    void *dbdma;

    BlockBackend *blk;

    /* controller registers / state */
    uint8_t setup_reg;
    uint8_t mode_reg;
    uint8_t error;
    uint8_t phase_lines;
    uint8_t int_flags;
    uint8_t int_mask;
    uint8_t pram;
    uint8_t step_count;
    uint8_t cur_track_reg;   /* register 10 image: side<<7 | track */
    uint8_t cur_sector_reg;  /* register 11 image: crc_ok<<7 | sector */
    uint8_t format;
    uint8_t first_sec;
    uint8_t xfer_cnt;
    uint8_t gap_size;
    bool irq_level;

    /* 1 us countdown timer (register 1) */
    uint8_t timer_val;
    int64_t timer_start_ns;
    QEMUTimer *usec_timer;

    /* drive state (single internal Superdrive) */
    bool disk_in;
    bool wr_protect;
    bool motor_on;
    bool is_ready;
    bool eject_latch;
    int step_dir;
    int cur_track;
    int cur_head;
    int cur_sector;          /* rotational position, 0-based */
    int drive_mode;          /* 0 = GCR, 1 = MFM */

    /* media geometry (MFM only) */
    bool media_hd;
    int num_tracks;
    int sectors_per_track;

    /* disk access state machine */
    int access_state;
    uint8_t target_sect;
    QEMUTimer *step_timer;
    QEMUTimer *access_timer;

    /* parked DBDMA descriptor awaiting sector data */
    void *pending_io;
    uint32_t pending_off;
} Swim3State;

void swim3_register_dma(Swim3State *s, void *dbdma);

#endif /* SWIM3_H */
