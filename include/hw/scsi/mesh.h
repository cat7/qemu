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

/*
 * Bus status line bits, as read back from BUS_STATUS0/1. Bit positions
 * follow the real chip (and DingusPPC's scsi.h SCSI_CTRL_* constants,
 * with STATUS1 holding the upper byte's bits 5-7).
 */
#define BUS0_IO           (1 << 0)
#define BUS0_CD           (1 << 1)
#define BUS0_MSG          (1 << 2)
#define BUS0_ATN          (1 << 3)
#define BUS0_ACK          (1 << 4)
#define BUS0_REQ          (1 << 5)
#define BUS1_SEL          (1 << 5)
#define BUS1_BSY          (1 << 6)
#define BUS1_RST          (1 << 7)

/*
 * Synthesized SCSI bus phase. The classic Mac OS .MESH driver POLLS the
 * bus-status registers between sequencer commands and takes the phase
 * encoded in MSG/CD/IO as ground truth about what the target wants next
 * -- reading constant zeros made it abandon every exchange right after
 * delivering the CDB (live-traced: INQUIRY pushed, BUS_STATUS0 read,
 * SEQ_BUS_FREE issued). Values chosen so the low three bits ARE the
 * MSG/CD/IO encoding.
 */
#define MESH_PH_DATA_OUT  0x0
#define MESH_PH_DATA_IN   (BUS0_IO)
#define MESH_PH_COMMAND   (BUS0_CD)
#define MESH_PH_STATUS    (BUS0_CD | BUS0_IO)
#define MESH_PH_MSG_OUT   (BUS0_MSG | BUS0_CD)
#define MESH_PH_MSG_IN    (BUS0_MSG | BUS0_CD | BUS0_IO)
#define MESH_PH_BUS_FREE  0xff  /* not a line encoding: nothing driven */

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

    /*
     * All command-done/exception interrupts are raised a few
     * microseconds AFTER the register access that caused them, via
     * this timer -- never synchronously inside the guest's own store
     * instruction. Real silicon works on the bus after the CPU moves
     * on; DingusPPC defers every sequencer step through one-shot
     * timers for the same reason. Raising inline made the .MESH
     * driver's ISR re-enter against half-advanced state (live-traced
     * as INTMASK 7->0->7 toggle storms) and abort healthy exchanges.
     */
    QEMUTimer *int_timer;
    uint8_t int_pending;

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

    /* Synthesized bus state the driver polls via BUS_STATUS0/1 */
    uint8_t phase;          /* MESH_PH_* */
    bool connected;         /* target selected and BSY */

    /*
     * The .MESH driver moves small transfers (INQUIRY and friends) by
     * PIO through the FIFO: SEQ_DATA_IN/OUT *without* the DMA bit, with
     * xfer_count programmed and the FIFO read/written byte-wise. The
     * DBDMA path is only used when the sequencer command carries
     * SEQ_DMA.
     */
    bool pio_active;
    bool awaiting_msg_out;

    /*
     * Bytes still to move in the CURRENT data-phase sequencer step
     * (SEQ_DATA_IN/OUT, PIO or DMA): loaded from xfer_count when the
     * command is issued (0 means 65536, as on the chip), decremented
     * per byte moved. The step -- and its INT_CMD_DONE -- ends when
     * this reaches zero, NOT when the SCSI request runs out of data:
     * the .MESH driver moves a 250-block WRITE(6) as 250 separate
     * 512-byte steps and waits for a command-done after each one
     * (live-traced; DingusPPC's ScsiBusController::to_xfer is the same
     * counter). dma_active marks a DMA step in progress the way
     * pio_active marks a PIO one.
     */
    uint32_t to_xfer;
    bool dma_active;
    /*
     * The step ended exactly where the SCSI backend's current chunk
     * ended, so "step done" is only true once the backend has said
     * what comes next (another chunk -> the target is ready again; or
     * completion -> Status phase). Set by mesh_step_done(), consumed
     * by mesh_transfer_data()/mesh_command_complete().
     */
    bool step_done_pending;

    /* Bridges the DBDMA-side and SCSI-backend-side of a data transfer,
     * following the same pattern as bmac's rx path: whichever side
     * becomes ready first waits for the other. */
    uint8_t *async_buf;
    int async_len;
    bool data_ready;
    bool to_device;
    void *dma_io; /* DBDMA_io *, saved to complete the transfer later */
};

/* Called by macio once its DBDMA controller is realized */
void mesh_register_dma(MESHState *s, void *dbdma);

#endif /* HW_SCSI_MESH_H */
