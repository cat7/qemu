/*
 * QEMU MESH (Macintosh Enhanced SCSI Hardware) Controller
 *
 * As found in Beige Power Mac G3 and other OldWorld PowerMacs, integrated
 * into the Heathrow (macio) chip. Bulk data transfer happens over a
 * macio DBDMA channel; command/status/message bytes go through a small
 * FIFO. The register-level model (offsets, sequencer command encoding,
 * exception/interrupt bits, and the Heathrow-specific chip ID) is based
 * on a previously-verified-working implementation of this exact chip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/scsi/mesh.h"
#include "hw/scsi/scsi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "trace.h"

/*
 * Real SCSI selection is not instantaneous: the initiator asserts SEL and
 * waits a real bus-timeout interval for the target to respond before
 * concluding no device is present. Real classic-Mac-OS-era SCSI HBAs (and
 * DingusPPC's own, independently-implemented, confirmed-working MESH model,
 * devices/common/scsi/mesh.cpp's `SEL_TIME_OUT` constant) use the classic
 * ~250ms SCSI selection-timeout convention here. Resolving SELECT
 * synchronously within the same register write (as this model previously
 * did) makes an all-targets bus scan complete many orders of magnitude
 * faster than real hardware -- exactly the kind of timing divergence this
 * project has repeatedly found to change which native ROM code path gets
 * taken.
 */
#define MESH_SEL_TIMEOUT_NS (250 * 1000 * 1000)

static void mesh_update_irq(MESHState *s)
{
    int level = !!(s->int_stat & s->int_mask);
    trace_mesh_irq_update(s->int_stat, s->int_mask, level);
    if (level) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void mesh_fifo_reset(MESHState *s)
{
    s->fifo_pos = 0;
}

static void mesh_fifo_push(MESHState *s, uint8_t val)
{
    if (s->fifo_pos < MESH_FIFO_SIZE) {
        s->fifo[s->fifo_pos++] = val;
    }
}

/* FIFO reads pop from the front, shifting the rest down */
static uint8_t mesh_fifo_pop(MESHState *s)
{
    uint8_t val;
    int i;

    if (s->fifo_pos == 0) {
        return 0;
    }
    val = s->fifo[0];
    for (i = 1; i < s->fifo_pos; i++) {
        s->fifo[i - 1] = s->fifo[i];
    }
    s->fifo_pos--;
    return val;
}

static void mesh_do_dma(MESHState *s);
static void mesh_set_phase(MESHState *s, uint8_t phase);

/*
 * Deferred-interrupt delivery: see the int_timer comment in mesh.h.
 * 10us is comfortably longer than the guest's store instruction and
 * far shorter than any driver timeout.
 */
#define MESH_INT_DELAY_NS (10 * 1000)

static void mesh_int_timer_cb(void *opaque)
{
    MESHState *s = MESH(opaque);

    s->int_stat |= s->int_pending;
    s->int_pending = 0;
    mesh_update_irq(s);
}

static void mesh_raise_int(MESHState *s, uint8_t bits)
{
    s->int_pending |= bits;
    timer_mod(s->int_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MESH_INT_DELAY_NS);
}

/*
 * Called when a SCSI request completes (status phase reached). Push the
 * status byte and a COMMAND COMPLETE message byte into the FIFO for the
 * driver's Status/MessageIn sequencer commands to read back, and let it
 * know via the command-done interrupt.
 */
static void mesh_command_complete(SCSIRequest *req, size_t resid)
{
    MESHState *s = req->hba_private;

    s->status = req->status;
    s->data_ready = false;
    s->async_len = 0;

    mesh_fifo_reset(s);
    mesh_fifo_push(s, s->status);
    mesh_fifo_push(s, 0x00); /* COMMAND COMPLETE message */

    s->pio_active = false;
    mesh_set_phase(s, MESH_PH_STATUS);

    mesh_raise_int(s, INT_CMD_DONE);

    if (s->current_req) {
        scsi_req_unref(s->current_req);
        s->current_req = NULL;
    }
}

static void mesh_request_cancelled(SCSIRequest *req)
{
    MESHState *s = req->hba_private;

    if (s->current_req == req) {
        scsi_req_unref(s->current_req);
        s->current_req = NULL;
        s->current_dev = NULL;
    }
}

static void mesh_pio_fill_fifo(MESHState *s);

/* Called by the SCSI backend when it has data ready to transfer */
static void mesh_transfer_data(SCSIRequest *req, uint32_t len)
{
    MESHState *s = req->hba_private;

    s->async_len = len;
    s->async_buf = scsi_req_get_buf(req);
    s->data_ready = true;

    if (s->dma_waiting) {
        mesh_do_dma(s);
    } else if (s->pio_active && !s->to_device) {
        /* a PIO Data In was already armed and waiting for this data */
        mesh_pio_fill_fifo(s);
        mesh_raise_int(s, INT_CMD_DONE);
    }
}

static const struct SCSIBusInfo mesh_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 0,

    .transfer_data = mesh_transfer_data,
    .complete = mesh_command_complete,
    .cancel = mesh_request_cancelled,
};

static void mesh_set_phase(MESHState *s, uint8_t phase)
{
    trace_mesh_phase(phase);
    s->phase = phase;
}

static void mesh_reset_state(MESHState *s)
{
    s->cur_cmd = SEQ_NOOP;
    mesh_fifo_reset(s);
    s->awaiting_cdb = false;
    s->awaiting_msg_out = false;
    s->pio_active = false;
    s->connected = false;
    mesh_set_phase(s, MESH_PH_BUS_FREE);
    s->int_mask = 0;
    s->int_stat = 0;
    s->exception = 0;
    s->xfer_count = 0;
    s->src_id = 7;
    s->data_ready = false;
    s->dma_waiting = false;
    s->async_len = 0;
    s->int_pending = 0;
    timer_del(s->sel_timer);
    if (s->int_timer) {
        timer_del(s->int_timer);
    }
}

/*
 * Fires MESH_SEL_TIMEOUT_NS after mesh_select() was asked to select
 * `s->sel_pending_target`. Resolves to success or a real selection-timeout
 * exception depending on whether a device answered -- see the comment on
 * MESH_SEL_TIMEOUT_NS above for why this can't just resolve synchronously.
 */
/*
 * A short, realistic completion delay for selecting a target that IS on
 * the bus: on real hardware a present target asserts BSY within
 * microseconds of recognizing its ID (DingusPPC models this as the
 * target's CONFIRM_SEL notification cancelling the timeout timer). Only
 * an ABSENT target costs the full 250ms timeout. The previous model
 * charged 250ms to every selection, present or not -- and during that
 * window the driver's bus-status polls saw a dead bus.
 */
#define MESH_SEL_PRESENT_NS (100 * 1000)

static void mesh_select_timeout_cb(void *opaque)
{
    MESHState *s = MESH(opaque);

    s->current_dev = scsi_device_find(&s->bus, 0, s->sel_pending_target, 0);
    trace_mesh_select_timeout_fire(s->sel_pending_target, s->current_dev != NULL);
    if (!s->current_dev) {
        s->exception |= EXC_SEL_TIMEOUT;
        s->int_stat |= INT_EXCEPTION | INT_CMD_DONE;
    } else {
        s->connected = true;
        /*
         * Selection with ATN tells the target the initiator has a
         * message (IDENTIFY) to send first; without it, the target
         * proceeds straight to the Command phase.
         */
        mesh_set_phase(s, (s->cur_cmd & SEQ_ATN) ? MESH_PH_MSG_OUT
                                                 : MESH_PH_COMMAND);
        s->int_stat |= INT_CMD_DONE;
    }
    mesh_update_irq(s);
}

static void mesh_select(MESHState *s)
{
    int target = s->dst_id & 7;
    bool present = scsi_device_find(&s->bus, 0, target, 0) != NULL;

    trace_mesh_select_start(target);

    s->lun = 0;
    if (s->current_req) {
        scsi_req_cancel(s->current_req);
    }
    s->current_dev = NULL;
    s->connected = false;
    mesh_set_phase(s, MESH_PH_BUS_FREE);

    s->sel_pending_target = target;
    timer_mod(s->sel_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (present ? MESH_SEL_PRESENT_NS : MESH_SEL_TIMEOUT_NS));
}

static void mesh_do_command(MESHState *s)
{
    uint8_t cdb[MESH_FIFO_SIZE];
    int cdblen = s->fifo_pos;
    int32_t datalen;

    if (!s->current_dev || cdblen == 0) {
        return;
    }
    memcpy(cdb, s->fifo, cdblen);
    mesh_fifo_reset(s);

    s->current_req = scsi_req_new(s->current_dev, 0, s->lun, cdb, cdblen, s);
    datalen = scsi_req_enqueue(s->current_req);

    /*
     * Advertise the phase the target moves to for this command's data
     * direction; the driver polls BUS_STATUS0 right after the CDB and
     * picks its next sequencer command from these bits. A command with
     * no data goes straight to Status (mesh_command_complete() sets
     * that, possibly synchronously inside scsi_req_enqueue()).
     */
    if (datalen > 0) {
        mesh_set_phase(s, MESH_PH_DATA_IN);
    } else if (datalen < 0) {
        mesh_set_phase(s, MESH_PH_DATA_OUT);
    }

    mesh_raise_int(s, INT_CMD_DONE);

    if (datalen != 0) {
        s->to_device = datalen < 0;
        scsi_req_continue(s->current_req);
    }
}

/*
 * Feed pending SCSI data into the FIFO for a PIO (non-DMA) Data In
 * transfer. The driver reads FIFO_COUNT and pops bytes; larger-than-FIFO
 * transfers are refilled as it drains (see the FIFO read path).
 */
static void mesh_pio_fill_fifo(MESHState *s)
{
    while (s->fifo_pos < MESH_FIFO_SIZE && s->async_len > 0) {
        mesh_fifo_push(s, *s->async_buf++);
        s->async_len--;
    }
}

static void mesh_perform_command(MESHState *s, uint8_t cmd)
{
    trace_mesh_seq_cmd(cmd);
    s->cur_cmd = cmd;
    s->int_stat &= ~INT_CMD_DONE;
    s->int_pending &= ~INT_CMD_DONE;

    switch (cmd & SEQ_CMD_MASK) {
    case SEQ_ARBITRATE:
        s->exception &= EXC_ARB_LOST;
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_SELECT:
        s->exception &= EXC_SEL_TIMEOUT;
        mesh_select(s);
        break;
    case SEQ_COMMAND:
        /*
         * Real hardware (and DingusPPC's own confirmed-working model)
         * arms the Command phase here but only actually dispatches once
         * the full CDB has arrived in the FIFO -- the driver commonly
         * writes SEQUENCE=SEQ_COMMAND *before* pushing any CDB bytes
         * (confirmed via live trace against the real V3 ROM), so trying
         * to consume the FIFO synchronously right here sees it empty and
         * silently drops the whole command. Dispatch immediately only if
         * the bytes already happen to be present; otherwise wait for
         * mesh_write()'s MESH_FIFO case to notice the CDB is complete.
         */
        if (s->fifo_pos > 0) {
            mesh_do_command(s);
        } else {
            s->awaiting_cdb = true;
        }
        break;
    case SEQ_STATUS:
        /*
         * Status byte is already sitting in the FIFO from completion.
         * Once the driver has collected it, the target's next stop is
         * Message In (COMMAND COMPLETE, also pre-queued).
         */
        mesh_set_phase(s, MESH_PH_MSG_IN);
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_DATA_OUT:
        s->to_device = true;
        if (cmd & SEQ_DMA) {
            if (s->current_req) {
                scsi_req_continue(s->current_req);
            }
        } else {
            /* PIO: bytes will arrive through the FIFO write path */
            s->pio_active = true;
            if (s->current_req && !s->data_ready) {
                scsi_req_continue(s->current_req);
            }
        }
        break;
    case SEQ_DATA_IN:
        s->to_device = false;
        if (cmd & SEQ_DMA) {
            if (s->data_ready) {
                mesh_do_dma(s);
            }
        } else {
            /*
             * PIO through the FIFO -- how the .MESH driver moves small
             * transfers like INQUIRY (live-traced: xfer_count=5,
             * SEQ_DATA_IN without the DMA bit, then FIFO pops).
             */
            s->pio_active = true;
            if (s->data_ready) {
                mesh_pio_fill_fifo(s);
                mesh_raise_int(s, INT_CMD_DONE);
            }
        }
        break;
    case SEQ_MSG_OUT:
        /*
         * The initiator's message is IDENTIFY (0x80 | LUN), pushed into
         * the FIFO either before or after this command is issued. QEMU's
         * scsi layer takes the LUN at scsi_req_new() time, so parse it
         * out for the upcoming command; then advance to Command phase.
         */
        if (s->fifo_pos > 0) {
            if (s->fifo[0] & 0x80) {
                s->lun = s->fifo[0] & 0x07;
            }
            mesh_fifo_reset(s);
            mesh_set_phase(s, MESH_PH_COMMAND);
            mesh_raise_int(s, INT_CMD_DONE);
        } else {
            s->awaiting_msg_out = true;
        }
        break;
    case SEQ_MSG_IN:
        /* COMMAND COMPLETE message byte was already queued */
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_BUS_FREE:
        s->connected = false;
        s->pio_active = false;
        mesh_set_phase(s, MESH_PH_BUS_FREE);
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_ENA_PARITY:
    case SEQ_DIS_PARITY:
    case SEQ_ENA_RESEL:
    case SEQ_DIS_RESEL:
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_RESET_MESH:
        mesh_reset_state(s);
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_FLUSH_FIFO:
        mesh_fifo_reset(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "mesh: unsupported sequencer command 0x%x\n",
                      cmd);
        break;
    }
}

static uint64_t mesh_read_one(MESHState *s, hwaddr addr);

static uint64_t mesh_read(void *opaque, hwaddr addr, unsigned size)
{
    MESHState *s = MESH(opaque);
    uint64_t val = mesh_read_one(s, addr);

    trace_mesh_reg_read(addr, val);
    return val;
}

static uint64_t mesh_read_one(MESHState *s, hwaddr addr)
{
    switch (addr) {
    case MESH_XFER_COUNT0:
        return s->xfer_count & 0xff;
    case MESH_XFER_COUNT1:
        return (s->xfer_count >> 8) & 0xff;
    case MESH_FIFO:
    {
        uint8_t val = mesh_fifo_pop(s);

        /*
         * PIO Data In refill: transfers larger than the 16-byte FIFO
         * are streamed as the driver drains it, and the SCSI request is
         * advanced only once BOTH the backend chunk and the FIFO are
         * empty -- scsi_req_continue() may complete the command, and
         * completion overwrites the FIFO with status bytes, so it must
         * never fire while data bytes are still queued.
         */
        if (s->pio_active && !s->to_device && s->fifo_pos == 0) {
            if (s->async_len > 0) {
                mesh_pio_fill_fifo(s);
            } else if (s->data_ready && s->current_req) {
                s->data_ready = false;
                scsi_req_continue(s->current_req);
            }
        }
        return val;
    }
    case MESH_SEQUENCE:
        return s->cur_cmd;
    case MESH_BUS_STATUS0:
        /*
         * The .MESH driver treats the MSG/CD/IO lines read here as
         * ground truth for the target's current phase -- a constant 0
         * made it abandon every exchange right after the CDB
         * (live-traced). REQ semantics matter just as much, the other
         * way: REQ means "the target has an UNANSWERED byte handshake
         * pending", and the driver's step-completion logic (read out of
         * the live guest and disassembled: its disconnect helper polls
         * for ACK/REQ to settle, and its state machine treats REQ still
         * asserted at a sequencer-step boundary as a phase anomaly and
         * aborts to Bus Free). So REQ is reported only while an armed
         * step is genuinely waiting on the driver; at step boundaries
         * the lines show the phase alone.
         */
        if (!s->connected || s->phase == MESH_PH_BUS_FREE) {
            return 0;
        }
        {
            uint8_t val = s->phase & (BUS0_MSG | BUS0_CD | BUS0_IO);
            bool req = false;

            switch (s->phase) {
            case MESH_PH_DATA_IN:
                /* target has bytes to hand over */
                req = s->data_ready || s->fifo_pos > 0;
                break;
            case MESH_PH_DATA_OUT:
            case MESH_PH_COMMAND:
            case MESH_PH_MSG_OUT:
                /* target is ready to accept the next byte from us */
                req = true;
                break;
            case MESH_PH_STATUS:
            case MESH_PH_MSG_IN:
                /* status / message byte is queued in the FIFO */
                req = s->fifo_pos > 0;
                break;
            }
            if (req) {
                val |= BUS0_REQ;
            }
            return val;
        }
    case MESH_BUS_STATUS1:
        return s->connected ? BUS1_BSY : 0;
    case MESH_FIFO_COUNT:
        return s->fifo_pos;
    case MESH_EXCEPTION:
        return s->exception;
    case MESH_ERROR:
        return 0;
    case MESH_INTMASK:
        return s->int_mask;
    case MESH_INTERRUPT:
        return s->int_stat;
    case MESH_SOURCE_ID:
        return s->src_id;
    case MESH_DEST_ID:
        return s->dst_id;
    case MESH_SYNC_PARAMS:
        return s->sync_params;
    case MESH_MESH_ID:
        return MESH_ID_HEATHROW;
    case MESH_SEL_TIMEOUT:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mesh: read from unknown register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }
}

static void mesh_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MESHState *s = MESH(opaque);

    trace_mesh_reg_write(addr, val);

    switch (addr) {
    case MESH_XFER_COUNT0:
        s->xfer_count = (s->xfer_count & 0xff00) | (val & 0xff);
        return;
    case MESH_XFER_COUNT1:
        s->xfer_count = (s->xfer_count & 0x00ff) | ((val & 0xff) << 8);
        return;
    case MESH_FIFO:
        mesh_fifo_push(s, val);
        if (s->awaiting_cdb && s->xfer_count > 0 &&
            s->fifo_pos >= s->xfer_count) {
            s->awaiting_cdb = false;
            mesh_do_command(s);
        } else if (s->awaiting_msg_out) {
            /* IDENTIFY (0x80 | LUN) delivered after SEQ_MSG_OUT */
            s->awaiting_msg_out = false;
            if (s->fifo[0] & 0x80) {
                s->lun = s->fifo[0] & 0x07;
            }
            mesh_fifo_reset(s);
            mesh_set_phase(s, MESH_PH_COMMAND);
            mesh_raise_int(s, INT_CMD_DONE);
        } else if (s->pio_active && s->to_device && s->data_ready &&
                   (s->fifo_pos >= s->async_len ||
                    (s->xfer_count > 0 && s->fifo_pos >= s->xfer_count))) {
            /* PIO Data Out: hand the accumulated bytes to the backend */
            int n = MIN(s->fifo_pos, s->async_len);

            memcpy(s->async_buf, s->fifo, n);
            s->async_buf += n;
            s->async_len -= n;
            mesh_fifo_reset(s);
            if (s->async_len == 0 && s->current_req) {
                s->data_ready = false;
                scsi_req_continue(s->current_req);
            }
            mesh_raise_int(s, INT_CMD_DONE);
        }
        return;
    case MESH_SEQUENCE:
        mesh_perform_command(s, val);
        return;
    case MESH_BUS_STATUS0:
    case MESH_BUS_STATUS1:
        return; /* no-op: no real bus signals to drive */
    case MESH_INTMASK:
        s->int_mask = val;
        mesh_update_irq(s);
        return;
    case MESH_INTERRUPT:
        s->int_stat &= ~(val & INT_ALL);
        mesh_update_irq(s);
        return;
    case MESH_SOURCE_ID:
        s->src_id = val;
        return;
    case MESH_DEST_ID:
        s->dst_id = val;
        return;
    case MESH_SYNC_PARAMS:
        s->sync_params = val;
        return;
    case MESH_SEL_TIMEOUT:
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mesh: write to unknown register 0x%"HWADDR_PRIx"\n",
                      addr);
        return;
    }
}

static const MemoryRegionOps mesh_ops = {
    .read = mesh_read,
    .write = mesh_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/*
 * DBDMA integration. One channel carries both directions (only one is
 * active at a time, selected by the last Data In/Data Out sequencer
 * command), following the same deferred-completion pattern bmac uses
 * for its rx path: whichever side (DBDMA memory, or the SCSI backend)
 * becomes ready first waits for the other.
 */
static void mesh_do_dma(MESHState *s)
{
    DBDMA_io *io = s->dma_io;
    int len;

    if (!io || !s->data_ready) {
        return;
    }

    len = MIN((int)io->len, s->async_len);

    if (s->to_device) {
        dma_memory_read(&address_space_memory, io->addr, s->async_buf, len,
                        MEMTXATTRS_UNSPECIFIED);
    } else {
        dma_memory_write(&address_space_memory, io->addr, s->async_buf, len,
                         MEMTXATTRS_UNSPECIFIED);
    }

    s->async_buf += len;
    s->async_len -= len;
    io->len -= len;

    s->dma_waiting = false;
    s->dma_io = NULL;
    io->dma_end(io);

    if (s->async_len == 0 && s->current_req) {
        scsi_req_continue(s->current_req);
    }
}

static void mesh_dma_rw(DBDMA_io *io)
{
    MESHState *s = io->opaque;

    s->dma_addr = io->addr;
    s->dma_len = io->len;
    s->dma_io = io;
    s->to_device = io->is_dma_out;

    if (s->data_ready) {
        mesh_do_dma(s);
    } else {
        s->dma_waiting = true;
    }
}

static void mesh_dma_flush(DBDMA_io *io)
{
    MESHState *s = io->opaque;

    s->dma_waiting = false;
    s->dma_io = NULL;
}

#define MESH_DMA_CHANNEL 0

void mesh_register_dma(MESHState *s, void *dbdma)
{
    DBDMA_register_channel(dbdma, MESH_DMA_CHANNEL, s->dma_irq,
                           mesh_dma_rw, mesh_dma_flush, s);
}

static void mesh_reset(DeviceState *dev)
{
    MESHState *s = MESH(dev);

    mesh_reset_state(s);
    s->dst_id = 0;
    s->sync_params = 2;
    s->status = 0;
}

static void mesh_realize(DeviceState *dev, Error **errp)
{
    MESHState *s = MESH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &mesh_ops, s,
                          "mesh", MESH_REG_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->dma_irq);

    s->sel_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mesh_select_timeout_cb, s);
    s->int_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mesh_int_timer_cb, s);

    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &mesh_scsi_info);
}

static const VMStateDescription vmstate_mesh = {
    .name = "mesh",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(fifo, MESHState, MESH_FIFO_SIZE),
        VMSTATE_INT32(fifo_pos, MESHState),
        VMSTATE_UINT16(xfer_count, MESHState),
        VMSTATE_UINT8(cur_cmd, MESHState),
        VMSTATE_UINT8(exception, MESHState),
        VMSTATE_UINT8(int_mask, MESHState),
        VMSTATE_UINT8(int_stat, MESHState),
        VMSTATE_UINT8(src_id, MESHState),
        VMSTATE_UINT8(dst_id, MESHState),
        VMSTATE_UINT8(sync_params, MESHState),
        VMSTATE_UINT8(status, MESHState),
        VMSTATE_UINT8_V(phase, MESHState, 2),
        VMSTATE_BOOL_V(connected, MESHState, 2),
        VMSTATE_UINT8_V(lun, MESHState, 2),
        VMSTATE_BOOL_V(awaiting_cdb, MESHState, 2),
        VMSTATE_BOOL_V(awaiting_msg_out, MESHState, 2),
        VMSTATE_BOOL_V(pio_active, MESHState, 2),
        VMSTATE_UINT8_V(int_pending, MESHState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void mesh_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mesh_realize;
    device_class_set_legacy_reset(dc, mesh_reset);
    dc->vmsd = &vmstate_mesh;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo mesh_info = {
    .name          = TYPE_MESH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MESHState),
    .class_init    = mesh_class_init,
};

static void mesh_register_types(void)
{
    type_register_static(&mesh_info);
}

type_init(mesh_register_types)
