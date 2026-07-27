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

    s->int_stat |= INT_CMD_DONE;
    mesh_update_irq(s);

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

/* Called by the SCSI backend when it has data ready to transfer */
static void mesh_transfer_data(SCSIRequest *req, uint32_t len)
{
    MESHState *s = req->hba_private;

    s->async_len = len;
    s->async_buf = scsi_req_get_buf(req);
    s->data_ready = true;

    if (s->dma_waiting) {
        mesh_do_dma(s);
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

static void mesh_reset_state(MESHState *s)
{
    s->cur_cmd = SEQ_NOOP;
    mesh_fifo_reset(s);
    s->awaiting_cdb = false;
    s->int_mask = 0;
    s->int_stat = 0;
    s->exception = 0;
    s->xfer_count = 0;
    s->src_id = 7;
    s->data_ready = false;
    s->dma_waiting = false;
    s->async_len = 0;
    timer_del(s->sel_timer);
}

/*
 * Fires MESH_SEL_TIMEOUT_NS after mesh_select() was asked to select
 * `s->sel_pending_target`. Resolves to success or a real selection-timeout
 * exception depending on whether a device answered -- see the comment on
 * MESH_SEL_TIMEOUT_NS above for why this can't just resolve synchronously.
 */
static void mesh_select_timeout_cb(void *opaque)
{
    MESHState *s = MESH(opaque);

    s->current_dev = scsi_device_find(&s->bus, 0, s->sel_pending_target, 0);
    trace_mesh_select_timeout_fire(s->sel_pending_target, s->current_dev != NULL);
    if (!s->current_dev) {
        s->exception |= EXC_SEL_TIMEOUT;
        s->int_stat |= INT_EXCEPTION | INT_CMD_DONE;
    } else {
        s->int_stat |= INT_CMD_DONE;
    }
    mesh_update_irq(s);
}

static void mesh_select(MESHState *s)
{
    int target = s->dst_id & 7;

    trace_mesh_select_start(target);

    s->lun = 0;
    if (s->current_req) {
        scsi_req_cancel(s->current_req);
    }
    s->current_dev = NULL;

    s->sel_pending_target = target;
    timer_mod(s->sel_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MESH_SEL_TIMEOUT_NS);
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

    s->int_stat |= INT_CMD_DONE;
    mesh_update_irq(s);

    if (datalen != 0) {
        s->to_device = datalen < 0;
        scsi_req_continue(s->current_req);
    }
}

static void mesh_perform_command(MESHState *s, uint8_t cmd)
{
    trace_mesh_seq_cmd(cmd);
    s->cur_cmd = cmd;
    s->int_stat &= ~INT_CMD_DONE;

    switch (cmd & SEQ_CMD_MASK) {
    case SEQ_ARBITRATE:
        s->exception &= EXC_ARB_LOST;
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
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
        /* Status byte is already sitting in the FIFO from completion */
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
        break;
    case SEQ_DATA_OUT:
        s->to_device = true;
        if (s->current_req) {
            scsi_req_continue(s->current_req);
        }
        break;
    case SEQ_DATA_IN:
        s->to_device = false;
        if (s->data_ready) {
            mesh_do_dma(s);
        }
        break;
    case SEQ_MSG_OUT:
        /* IDENTIFY etc.; QEMU's scsi_req_new already took the LUN
         * directly, so there's nothing more to act on here. */
        mesh_fifo_reset(s);
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
        break;
    case SEQ_MSG_IN:
        /* COMMAND COMPLETE message byte was already queued */
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
        break;
    case SEQ_BUS_FREE:
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
        break;
    case SEQ_ENA_PARITY:
    case SEQ_DIS_PARITY:
    case SEQ_ENA_RESEL:
    case SEQ_DIS_RESEL:
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
        break;
    case SEQ_RESET_MESH:
        mesh_reset_state(s);
        s->int_stat |= INT_CMD_DONE;
        mesh_update_irq(s);
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

static uint64_t mesh_read(void *opaque, hwaddr addr, unsigned size)
{
    MESHState *s = MESH(opaque);

    switch (addr) {
    case MESH_XFER_COUNT0:
        return s->xfer_count & 0xff;
    case MESH_XFER_COUNT1:
        return (s->xfer_count >> 8) & 0xff;
    case MESH_FIFO:
        return mesh_fifo_pop(s);
    case MESH_SEQUENCE:
        return s->cur_cmd;
    case MESH_BUS_STATUS0:
    case MESH_BUS_STATUS1:
        /* Real bus signal lines aren't modelled; QEMU's SCSIBus already
         * abstracts arbitration/selection/phase transitions. */
        return 0;
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

    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &mesh_scsi_info);
}

static const VMStateDescription vmstate_mesh = {
    .name = "mesh",
    .version_id = 1,
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
