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
 * Deferring the interrupt is right for a driver that waits for the IRQ
 * line (see the int_timer comment in mesh.h), but the Interrupt register
 * itself must never lie to a driver that POLLS it. Mac OS X Server 1.2v3
 * (Rhapsody) drives the MESH by polling: it writes a sequencer command
 * and reads Interrupt on the very next instruction, expecting the
 * command-done bit to be there. With delivery parked on a 10us timer it
 * read 0, decided the sequencer had not responded, and gave up before
 * ever issuing the SELECT -- the boot stopped on the "Starting Mac OS X
 * Server" splash with the wheel spinning.
 *
 * So collapse the delay whenever the guest reads the register: by then
 * the store that armed the interrupt has long retired, which is the
 * whole hazard the deferral exists to avoid. A driver that takes the
 * IRQ instead is unaffected -- by the time its ISR reads Interrupt the
 * timer has already fired and there is nothing pending to flush.
 */
static void mesh_int_flush(MESHState *s)
{
    if (s->int_pending) {
        timer_del(s->int_timer);
        s->int_stat |= s->int_pending;
        s->int_pending = 0;
        mesh_update_irq(s);
    }
}

/*
 * Called when a SCSI request completes (status phase reached). Only
 * RECORD the status here -- real MESH delivers the status byte into the
 * FIFO when the driver runs SEQ_STATUS, and the COMMAND COMPLETE
 * message when it runs SEQ_MSG_IN. Pre-pushing both at completion time
 * (as this used to) contaminated the tail of a PIO Data In transfer:
 * the ROM's drain loop suddenly saw FIFO_COUNT=2 of bytes it never
 * asked for and stalled polling (live-traced: INQUIRY data delivered
 * fine, then an endless FIFO_COUNT=2 poll at the Data->Status
 * boundary).
 */
static void mesh_command_complete(SCSIRequest *req, size_t resid)
{
    MESHState *s = req->hba_private;

    s->status = req->status;
    s->data_ready = false;
    s->async_len = 0;

    mesh_fifo_reset(s);

    mesh_set_phase(s, MESH_PH_STATUS);

    if ((s->pio_active || s->dma_active) && s->to_xfer > 0) {
        /*
         * The target left Data phase while a data sequencer step still
         * had bytes to move: real MESH ends that step with a PHASE
         * MISMATCH exception (xfer_count holding the residual), not a
         * command-done. Linux's mesh.c relies on exactly this on real
         * hardware -- it programs the full length and lets the
         * exception mark the end of the data phase.
         */
        trace_mesh_phase_mismatch(s->cur_cmd, s->phase, s->to_xfer);
        s->exception |= EXC_PHASE_MM;
        mesh_raise_int(s, INT_EXCEPTION);
    } else {
        mesh_raise_int(s, INT_CMD_DONE);
    }
    s->pio_active = false;
    s->dma_active = false;
    s->to_xfer = 0;
    s->step_done_pending = false;

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
static void mesh_pio_out_drain(MESHState *s);
static void mesh_dma_pump(MESHState *s);

/* Called by the SCSI backend when it has data ready to transfer */
static void mesh_transfer_data(SCSIRequest *req, uint32_t len)
{
    MESHState *s = req->hba_private;

    s->async_len = len;
    s->async_buf = scsi_req_get_buf(req);
    s->data_ready = true;

    if (s->step_done_pending) {
        /*
         * The previous step consumed the last byte of the previous
         * chunk; the target has more, so that step is now complete
         * and the phase stays Data -- the driver will read it from
         * BUS_STATUS0 and issue the next step.
         */
        s->step_done_pending = false;
        mesh_raise_int(s, INT_CMD_DONE);
    }

    if (s->dma_active) {
        mesh_dma_pump(s);
    } else if (s->pio_active) {
        if (s->to_device) {
            mesh_pio_out_drain(s);
        } else {
            mesh_pio_fill_fifo(s);
        }
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
    s->dma_active = false;
    s->to_xfer = 0;
    s->step_done_pending = false;
    s->connected = false;
    mesh_set_phase(s, MESH_PH_BUS_FREE);
    s->int_mask = 0;
    s->int_stat = 0;
    s->exception = 0;
    s->xfer_count = 0;
    s->src_id = 7;
    s->data_ready = false;
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
    s->pio_active = false;
    s->dma_active = false;
    s->to_xfer = 0;
    s->step_done_pending = false;
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
    while (s->fifo_pos < MESH_FIFO_SIZE && s->async_len > 0 &&
           s->to_xfer > 0) {
        mesh_fifo_push(s, *s->async_buf++);
        s->async_len--;
        s->to_xfer--;
        s->xfer_count--;
    }
    if (s->pio_active && s->to_xfer == 0) {
        /*
         * The step's last byte is in the FIFO: the transfer counter
         * hit zero, so the sequencer command is done. The driver
         * drains what is left by FIFO_COUNT (live-traced: it polls
         * INTERRUPT and FIFO_COUNT alternately and pops whatever is
         * there). Raising this at the FIRST fill instead, as the
         * model used to, made the driver treat every step longer
         * than the 16-byte FIFO as a short transfer and re-issue it
         * with the residual (0x24 -> 0x14, 0x200 -> 0x1f0 -> ...).
         * Request completion for Data In comes later, from the FIFO
         * read path, once the driver has popped the last byte --
         * completion resets the FIFO.
         */
        s->pio_active = false;
        mesh_raise_int(s, INT_CMD_DONE);
    }
}

/*
 * A data-phase sequencer step (PIO Data Out, or DMA either way) has
 * moved all of its xfer_count bytes. If the SCSI backend's chunk still
 * has bytes, the target is simply ready for the next step; if the step
 * ended exactly where the chunk did, ask the backend for what follows
 * -- the next chunk (mesh_transfer_data) or completion
 * (mesh_command_complete) raises the command-done, so the driver
 * never sees a Data phase it cannot continue nor a Status phase before
 * the backend has actually finished the I/O.
 */
static void mesh_step_done(MESHState *s)
{
    s->pio_active = false;
    s->dma_active = false;
    if (s->async_len == 0 && s->data_ready && s->current_req) {
        s->data_ready = false;
        s->step_done_pending = true;
        scsi_req_continue(s->current_req);
    } else {
        mesh_raise_int(s, INT_CMD_DONE);
    }
}

/* PIO Data Out bookkeeping after bytes were moved onto the bus */
static void mesh_pio_out_advance(MESHState *s)
{
    if (!s->pio_active || !s->to_device) {
        return;
    }
    if (s->to_xfer == 0) {
        mesh_step_done(s);
    } else if (s->async_len == 0 && s->data_ready && s->current_req) {
        /* the step continues into the backend's next chunk */
        s->data_ready = false;
        scsi_req_continue(s->current_req);
    }
}

/*
 * Move bytes the driver has pushed into the FIFO onto the bus. Real
 * MESH hands each byte to the target as it takes it, so the driver
 * watches FIFO_COUNT fall and keeps pushing (it sends xfer_count bytes
 * THROUGH the 16-byte FIFO); bytes that arrive before the backend has
 * a buffer simply wait in the FIFO.
 */
static void mesh_pio_out_drain(MESHState *s)
{
    while (s->pio_active && s->to_device && s->fifo_pos > 0 &&
           s->data_ready && s->async_len > 0 && s->to_xfer > 0) {
        *s->async_buf++ = mesh_fifo_pop(s);
        s->async_len--;
        s->to_xfer--;
        s->xfer_count--;
    }
    mesh_pio_out_advance(s);
}

/* The bus phase a transfer-type sequencer command expects, or -1 */
static int mesh_cmd_phase(uint8_t cmd)
{
    switch (cmd & SEQ_CMD_MASK) {
    case SEQ_COMMAND:
        return MESH_PH_COMMAND;
    case SEQ_STATUS:
        return MESH_PH_STATUS;
    case SEQ_DATA_OUT:
        return MESH_PH_DATA_OUT;
    case SEQ_DATA_IN:
        return MESH_PH_DATA_IN;
    case SEQ_MSG_OUT:
        return MESH_PH_MSG_OUT;
    case SEQ_MSG_IN:
        return MESH_PH_MSG_IN;
    default:
        return -1;
    }
}

static void mesh_perform_command(MESHState *s, uint8_t cmd)
{
    int expected;

    trace_mesh_seq_cmd(cmd);
    s->cur_cmd = cmd;
    s->int_stat &= ~INT_CMD_DONE;
    s->int_pending &= ~INT_CMD_DONE;

    /*
     * A transfer command whose phase is not the one the target is in
     * ends at once with a PHASE MISMATCH exception -- that is how the
     * chip tells the initiator the target moved on, and the .MESH
     * driver uses it as a probe: live-traced after a DMA READ(10),
     * having drained the last PIO bytes and read BUS_STATUS0 = Status
     * without REQ, it issued SEQ_DATA_IN xfer_count=1 and polled
     * INTERRUPT/FIFO_COUNT for the answer; a model that silently
     * accepted the command left it spinning there forever ("Installing
     * device driver..." hang). Every transfer command in the working
     * enumeration/ROM-scan traces was issued in its matching phase, so
     * this only fires where real hardware would. (SEQ_BUS_FREE has its
     * own probe semantics below.)
     */
    expected = mesh_cmd_phase(cmd);
    if (expected >= 0 && s->connected && s->phase != expected) {
        trace_mesh_phase_mismatch(cmd, s->phase, s->xfer_count);
        s->exception |= EXC_PHASE_MM;
        mesh_raise_int(s, INT_EXCEPTION);
        return;
    }

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
         * Deliver the recorded status byte now -- this sequencer command
         * is what latches it into the FIFO on real hardware. The
         * target's next stop is Message In (COMMAND COMPLETE).
         */
        mesh_fifo_push(s, s->status);
        mesh_set_phase(s, MESH_PH_MSG_IN);
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_DATA_OUT:
    case SEQ_DATA_IN:
        /*
         * One data-phase STEP of xfer_count bytes (0 = 65536), PIO
         * through the FIFO or DMA through the DBDMA channel. The step
         * completes -- INT_CMD_DONE -- when that many bytes have
         * moved, whatever the SCSI request's total: the .MESH driver
         * moves a 128000-byte WRITE(6) as 250 steps of 0x200 and
         * waits for a command-done after each (live-traced; the old
         * model only signalled when the backend's whole chunk was
         * consumed, so the driver waited forever after step one --
         * the Drive Setup "Initialize" hang). DingusPPC keeps the
         * same counter (ScsiBusController::to_xfer).
         */
        s->to_device = (cmd & SEQ_CMD_MASK) == SEQ_DATA_OUT;
        s->to_xfer = s->xfer_count ? s->xfer_count : 0x10000;
        s->step_done_pending = false;
        if (cmd & SEQ_DMA) {
            s->pio_active = false;
            s->dma_active = true;
            mesh_dma_pump(s);
        } else {
            s->dma_active = false;
            s->pio_active = true;
            if (s->to_device) {
                /* bytes the driver may already have pushed */
                mesh_pio_out_drain(s);
            } else if (s->data_ready) {
                mesh_pio_fill_fifo(s);
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
        /* deliver the COMMAND COMPLETE message byte */
        mesh_fifo_push(s, 0x00);
        mesh_raise_int(s, INT_CMD_DONE);
        break;
    case SEQ_BUS_FREE:
        /*
         * SEQ_BUS_FREE is the Mac driver's DISPATCH PROBE, not just a
         * disconnect: after every completed step it asks "is the
         * transaction over?", and real MESH answers with a
         * PHASE-MISMATCH exception whenever the target is still
         * connected in some further phase. The driver then reads
         * BUS_STATUS0 for the new phase and issues the matching
         * transfer command (SEQ_DATA_IN/SEQ_STATUS/...). Answering
         * INT_CMD_DONE unconditionally -- as this did -- told the
         * driver the transaction had ended right after the CDB, which
         * is exactly the "driver issues SEQ_BUS_FREE and never
         * transfers" dead end the bring-up traces showed. Semantics
         * verified against DingusPPC's SeqCmd::BusFree (mesh.cpp): in
         * MESSAGE_IN the probe ACCEPTS the final message and the
         * target really does go bus-free; connected in any other phase
         * raises EXC_PHASE_MM + INT_EXCEPTION; genuinely free answers
         * INT_CMD_DONE.
         */
        if (s->connected && s->phase != MESH_PH_MSG_IN &&
            s->phase != MESH_PH_BUS_FREE) {
            s->exception |= EXC_PHASE_MM;
            mesh_raise_int(s, INT_EXCEPTION);
            break;
        }
        s->connected = false;
        s->pio_active = false;
        s->dma_active = false;
        s->to_xfer = 0;
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
        /*
         * "Reset MESH" resets the sequencer and its transfer state. It is
         * NOT a chip reset: the interrupt-mask register is a CPU-visible
         * register of its own, and the driver's copy of it has to survive.
         * Clearing it here silently masked off the INT_CMD_DONE this very
         * command then raises, and every interrupt after it.
         *
         * Mac OS X Server 1.2v3 (Rhapsody) is the guest that shows it: its
         * SCSI driver writes IntMask = 0x6 and only then issues Reset MESH,
         * so from that point on no MESH interrupt could ever be delivered
         * and the boot stopped on the "Starting Mac OS X Server" splash
         * with the wheel still spinning. Mac OS 9 programs the mask after
         * the reset instead, which is why it never noticed.
         */
        {
            uint8_t saved_mask = s->int_mask;

            mesh_reset_state(s);
            s->int_mask = saved_mask;
        }
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
    uint64_t val;

    if (addr == MESH_INTERRUPT) {
        mesh_int_flush(s);
    }
    val = mesh_read_one(s, addr);

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
        if (!s->to_device && s->phase == MESH_PH_DATA_IN &&
            s->fifo_pos == 0 && s->data_ready && s->current_req) {
            if (s->pio_active && s->async_len > 0) {
                mesh_pio_fill_fifo(s);
            } else if (s->async_len == 0) {
                /*
                 * Chunk fully drained (the step may already have
                 * ended -- its command-done was raised when its last
                 * byte entered the FIFO): fetch the next chunk, or
                 * let the request complete into Status phase.
                 */
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
        if (s->pio_active && s->to_device) {
            /*
             * PIO Data Out drains CONTINUOUSLY: real MESH moves each
             * byte from the FIFO onto the bus as the target takes it,
             * so the driver watches FIFO_COUNT fall and keeps pushing
             * -- it sends xfer_count bytes THROUGH the 16-byte FIFO
             * (a bulk handoff could never fire for a step larger than
             * the FIFO; live-traced as FIFO_COUNT pegged at 16). The
             * step's command-done comes from the transfer counter
             * reaching zero (mesh_pio_out_advance()).
             */
            mesh_fifo_push(s, val);
            mesh_pio_out_drain(s);
            return;
        }
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
/*
 * Move as much as all three sides allow: the armed DBDMA descriptor
 * (io->len), the SCSI backend's current chunk (async_len) and the
 * sequencer step's transfer counter (to_xfer). A descriptor is
 * completed only when ITS byte count is satisfied -- the DBDMA engine
 * does not know about SCSI chunk boundaries -- and ending it may
 * synchronously start the next one (mesh_dma_rw() re-enters with the
 * new io), so all state lives in `s` and the loop re-reads it.
 */
static void mesh_dma_pump(MESHState *s)
{
    DBDMA_io *io;
    int len;

    while (s->dma_active && (io = s->dma_io) != NULL && s->data_ready &&
           s->async_len > 0 && s->to_xfer > 0 && io->len > 0) {
        len = MIN(io->len, MIN(s->async_len, (int)s->to_xfer));

        if (s->to_device) {
            dma_memory_read(&address_space_memory, io->addr, s->async_buf,
                            len, MEMTXATTRS_UNSPECIFIED);
        } else {
            dma_memory_write(&address_space_memory, io->addr, s->async_buf,
                             len, MEMTXATTRS_UNSPECIFIED);
        }

        s->async_buf += len;
        s->async_len -= len;
        s->to_xfer -= len;
        s->xfer_count -= len;
        io->addr += len;
        io->len -= len;
        trace_mesh_dma_xfer(s->to_device, io->addr - len, len, io->len,
                            s->to_xfer, s->async_len);

        if (io->len == 0) {
            s->dma_io = NULL;
            io->dma_end(io);
        }
    }

    if (s->dma_active && s->to_xfer == 0) {
        mesh_step_done(s);
    } else if (s->dma_active && s->async_len == 0 && s->data_ready &&
               s->current_req) {
        /* the step continues into the backend's next chunk */
        s->data_ready = false;
        scsi_req_continue(s->current_req);
    }
}

static void mesh_dma_rw(DBDMA_io *io)
{
    MESHState *s = io->opaque;

    /*
     * The descriptor may be armed before or after the sequencer
     * command; the direction is the sequencer command's, and nothing
     * moves until one is active (real MESH only asserts DMA request
     * while a DMA sequencer step is running).
     */
    s->dma_io = io;
    mesh_dma_pump(s);
}

static void mesh_dma_flush(DBDMA_io *io)
{
    MESHState *s = io->opaque;

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
    .version_id = 3,
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
        VMSTATE_UINT32_V(to_xfer, MESHState, 3),
        VMSTATE_BOOL_V(dma_active, MESHState, 3),
        VMSTATE_BOOL_V(step_done_pending, MESHState, 3),
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
