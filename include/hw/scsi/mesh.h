/*
 * QEMU MESH (Macintosh Enhanced SCSI Hardware) Controller
 *
 * As found integrated into the Heathrow (macio) chip on the Beige G3
 * and other OldWorld PowerMacs.
 *
 * Register layout and command/exception/interrupt bit encodings are
 * taken from a real, previously-verified-working implementation of
 * this exact chip (DingusPPC's devices/common/scsi/mesh.{h,cpp}),
 * not guessed -- notably the chip ID byte (0x4 for the Heathrow-
 * integrated cell, as opposed to 0xE2 for the standalone TNT ASIC).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SCSI_MESH_H
#define HW_SCSI_MESH_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/scsi/scsi.h"

#define TYPE_MESH "mesh"
OBJECT_DECLARE_SIMPLE_TYPE(MESHState, MESH)

/* MESH register space is 0x100 bytes: 16 registers, 0x10 apart */
#define MESH_REG_SIZE     0x100
#define MESH_NUM_REGS     0x10
#define MESH_FIFO_SIZE    16

/* Register offsets (byte address / 0x10 = index) */
#define MESH_XFER_COUNT0  0x00
#define MESH_XFER_COUNT1  0x10
#define MESH_FIFO         0x20
#define MESH_SEQUENCE     0x30
#define MESH_BUS_STATUS0  0x40
#define MESH_BUS_STATUS1  0x50
#define MESH_FIFO_COUNT   0x60
#define MESH_EXCEPTION    0x70
#define MESH_ERROR        0x80
#define MESH_INTMASK      0x90
#define MESH_INTERRUPT    0xa0
#define MESH_SOURCE_ID    0xb0
#define MESH_DEST_ID      0xc0
#define MESH_SYNC_PARAMS  0xd0
#define MESH_MESH_ID      0xe0
#define MESH_SEL_TIMEOUT  0xf0

/* Chip ID reported by the MESH cell integrated into Heathrow */
#define MESH_ID_HEATHROW  4

/* Sequencer commands (low nibble of the Sequence register) */
#define SEQ_NOOP          0x0
#define SEQ_ARBITRATE     0x1
#define SEQ_SELECT        0x2
#define SEQ_COMMAND       0x3
#define SEQ_STATUS        0x4
#define SEQ_DATA_OUT      0x5
#define SEQ_DATA_IN       0x6
#define SEQ_MSG_OUT       0x7
#define SEQ_MSG_IN        0x8
#define SEQ_BUS_FREE      0x9
#define SEQ_ENA_PARITY    0xa
#define SEQ_DIS_PARITY    0xb
#define SEQ_ENA_RESEL     0xc
#define SEQ_DIS_RESEL     0xd
#define SEQ_RESET_MESH    0xe
#define SEQ_FLUSH_FIFO    0xf
#define SEQ_CMD_MASK      0xf
/* High bits of a Sequence write, combined with the base command */
#define SEQ_ATN           0x20  /* assert ATN during SELECT */
#define SEQ_DMA           0x80  /* transfer is DMA-driven */

/* Exception register bits */
#define EXC_SEL_TIMEOUT   (1 << 0)
#define EXC_PHASE_MM      (1 << 1)
#define EXC_ARB_LOST      (1 << 2)

/* Interrupt register bits */
#define INT_CMD_DONE      (1 << 0)
#define INT_EXCEPTION     (1 << 1)
#define INT_ERROR         (1 << 2)
#define INT_ALL           (INT_CMD_DONE | INT_EXCEPTION | INT_ERROR)

struct MESHState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq dma_irq;

    SCSIBus bus;
    SCSIDevice *current_dev;
    SCSIRequest *current_req;

    /* SELECT completion (success or selection-timeout exception) is
     * deferred by a real elapsed delay rather than resolved instantly --
     * see mesh_select()/mesh_select_timeout_cb() in mesh.c. */
    QEMUTimer *sel_timer;
    int sel_pending_target;

    uint8_t fifo[MESH_FIFO_SIZE];
    int fifo_pos;

    /* Real hardware's SEQ_COMMAND arms the Command phase and only actually
     * dispatches the request once the driver has pushed the full CDB
     * (xfer_count bytes) into the FIFO one byte at a time -- the two
     * events are NOT simultaneous, unlike this model's earlier assumption.
     * See mesh_perform_command()/mesh_write() in mesh.c. */
    bool awaiting_cdb;

    uint16_t xfer_count;
    uint8_t cur_cmd;
    uint8_t exception;
    uint8_t int_mask;
    uint8_t int_stat;
    uint8_t src_id;
    uint8_t dst_id;
    uint8_t sync_params;
    uint8_t status;
    uint8_t lun;

    /* Bridges the DBDMA-side and SCSI-backend-side of a data transfer,
     * following the same pattern as bmac's rx path: whichever side
     * becomes ready first waits for the other. */
    uint8_t *async_buf;
    int async_len;
    bool data_ready;
    bool dma_waiting;
    bool to_device;
    hwaddr dma_addr;
    int dma_len;
    void *dma_io; /* DBDMA_io *, saved to complete the transfer later */
};

/* Called by macio once its DBDMA controller is realized */
void mesh_register_dma(MESHState *s, void *dbdma);

#endif /* HW_SCSI_MESH_H */
