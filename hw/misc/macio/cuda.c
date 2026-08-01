/*
 * QEMU PowerMac CUDA device support
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "hw/misc/macio/cuda.h"
#include "qemu/timer.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

/* Bits in B data register: all active low */
#define TREQ            0x08    /* Transfer request (input) */
#define TACK            0x10    /* Transfer acknowledge (output) */
#define TIP             0x20    /* Transfer in progress (output) */

/* commands (1st byte) */
#define ADB_PACKET      0
#define CUDA_PACKET     1
#define ERROR_PACKET    2
#define TIMER_PACKET    3
#define POWER_PACKET    4
#define MACIIC_PACKET   5
#define PMU_PACKET      6

/* status byte (2nd byte) of an ADB_PACKET reply */
#define ADB_STAT_RESPONSE 0x80

#define CUDA_TIMER_FREQ (4700000 / 6)

/*
 * SR_INT delay, by triggering event -- confirmed against DingusPPC's
 * ViaCuda::write()/schedule_sr_int() call sites, which model these three
 * cases with distinct, specific microsecond values (not a single generic
 * delay): a dummy idle-acknowledge/attention pulse whenever TIP rises
 * (regardless of which of the two TIP-rising sub-cases caused it), and
 * separate per-byte "I've clocked this one in/out" acks that differ
 * depending on transfer direction.
 */
#define CUDA_SR_DELAY_TIP_PULSE     (61 * SCALE_US)
#define CUDA_SR_DELAY_HOST_TO_CUDA  (71 * SCALE_US)
#define CUDA_SR_DELAY_CUDA_TO_HOST  (88 * SCALE_US)
#define CUDA_SR_DELAY_ATTENTION     (30 * SCALE_US)
/*
 * Not itself a modeled hardware timing -- just how soon to check again
 * after a one-shot SR_INT pulse was missed (see cuda_set_sr_int()).
 */
#define CUDA_SR_DELAY_RETRY         (20 * SCALE_US)

/* CUDA returns time_t's offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET                      2082844800

static void cuda_receive_packet_from_host(CUDAState *s,
                                          const uint8_t *data, int len);

/* MacOS uses timer 1 for calibration on startup, so we use
 * the timebase frequency and cuda_get_counter_value() with
 * cuda_get_load_time() to steer MacOS to calculate calibrate its timers
 * correctly for both TCG and KVM (see commit b981289c49 "PPC: Cuda: Use cuda
 * timer to expose tbfreq to guest" for more information) */

static uint64_t cuda_get_counter_value(MOS6522State *s, MOS6522Timer *ti)
{
    MOS6522CUDAState *mcs = container_of(s, MOS6522CUDAState, parent_obj);
    CUDAState *cs = container_of(mcs, CUDAState, mos6522_cuda);

    /* Reverse of the tb calculation algorithm that Mac OS X uses on bootup */
    uint64_t tb_diff = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                cs->tb_frequency, NANOSECONDS_PER_SECOND) -
                           ti->load_time;

    return (tb_diff * 0xBF401675E5DULL) / (cs->tb_frequency << 24);
}

static uint64_t cuda_get_load_time(MOS6522State *s, MOS6522Timer *ti)
{
    MOS6522CUDAState *mcs = container_of(s, MOS6522CUDAState, parent_obj);
    CUDAState *cs = container_of(mcs, CUDAState, mos6522_cuda);

    uint64_t load_time = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  cs->tb_frequency, NANOSECONDS_PER_SECOND);
    return load_time;
}

static void cuda_delay_set_sr_int(CUDAState *s, uint64_t delay_ns);

static void cuda_set_sr_int(void *opaque)
{
    CUDAState *s = opaque;
    MOS6522CUDAState *mcs = &s->mos6522_cuda;
    MOS6522State *ms = MOS6522(mcs);
    qemu_irq irq = qdev_get_gpio_in(DEVICE(ms), SR_INT_BIT);

    qemu_set_irq(irq, 1);

    /*
     * This SR_INT pulse is the only signal telling the host "a queued
     * host-to-CUDA-to-host response is ready to be clocked out" -- it
     * fires exactly once, 20us after the response was queued. If the
     * ROM's polling loop doesn't happen to sample IFR's SR flag inside
     * that narrow window (e.g. because it's masking interrupts to poll
     * TREQ directly instead, as this ROM's ADB-reset-completion code
     * does), the one-shot pulse is missed forever and TREQ/data_in
     * never get drained, even though real hardware's single-threaded
     * CUDA firmware would keep representing the same pending condition
     * on every one of its own idle-loop passes. Keep re-pulsing while
     * a response is still undrained so a later poll gets another
     * chance, instead of relying on catching this one exact instant.
     */
    if (s->data_in_index < s->data_in_size) {
        cuda_delay_set_sr_int(s, CUDA_SR_DELAY_RETRY);
    }
}

static void cuda_delay_set_sr_int(CUDAState *s, uint64_t delay_ns)
{
    int64_t expire;

    trace_cuda_delay_set_sr_int();

    expire = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;
    timer_mod(s->sr_delay_timer, expire);
}

/* NOTE: TIP and TREQ are negated */
static void cuda_update(CUDAState *s)
{
    MOS6522CUDAState *mcs = &s->mos6522_cuda;
    MOS6522State *ms = MOS6522(mcs);
    ADBBusState *adb_bus = &s->adb_bus;
    int packet_received, len;

    packet_received = 0;
    if (!(ms->b & TIP)) {
        /* transfer requested from host */

        if (ms->acr & SR_OUT) {
            /* data output */
            if ((ms->b & (TACK | TIP)) != (s->last_b & (TACK | TIP))) {
                if (s->data_out_index < sizeof(s->data_out)) {
                    if (s->data_out_index == 0) {
                        adb_autopoll_block(adb_bus);
                    }
                    trace_cuda_data_send(ms->sr);
                    s->data_out[s->data_out_index++] = ms->sr;
                    cuda_delay_set_sr_int(s, CUDA_SR_DELAY_HOST_TO_CUDA);
                }
            }
        } else {
            if (s->data_in_index < s->data_in_size) {
                /* data input */
                if ((ms->b & (TACK | TIP)) != (s->last_b & (TACK | TIP))) {
                    ms->sr = s->data_in[s->data_in_index++];
                    trace_cuda_data_recv(ms->sr);
                    /* indicate end of transfer */
                    if (s->data_in_index >= s->data_in_size) {
                        ms->b = (ms->b | TREQ);
                        adb_autopoll_unblock(adb_bus);
                    }
                    cuda_delay_set_sr_int(s, CUDA_SR_DELAY_CUDA_TO_HOST);
                }
            }
        }
    } else {
        /* no transfer requested: handle sync case */
        if ((s->last_b & TIP) && (ms->b & TACK) != (s->last_b & TACK)) {
            /*
             * Update TREQ state each time TACK changes state. TACK
             * clear means the host is entering a fresh sync/attention
             * cycle: always assert TREQ. TACK set means the host is
             * acknowledging end-of-cycle: negate TREQ *unless* the host
             * has already started reading a queued response and left it
             * partway through (data_in_index between 0 and
             * data_in_size), in which case we must keep asserting TREQ
             * to invite the host to finish reading it -- otherwise an
             * in-progress response never gets signaled and the host's
             * idle TACK-toggle probe (which never raises TIP on its
             * own) spins forever. A response that is queued but was
             * never touched by this cycle (data_in_index == 0) belongs
             * to a separate, unrelated exchange (e.g. an ADB autopoll
             * reply queued behind the scenes) and must not block this
             * sync probe's own handshake.
             */
            if ((ms->b & TACK) &&
                (s->data_in_index == 0 || s->data_in_index >= s->data_in_size)) {
                ms->b = (ms->b | TREQ);
            } else {
                ms->b = (ms->b & ~TREQ);
            }
            cuda_delay_set_sr_int(s, CUDA_SR_DELAY_TIP_PULSE);
        } else {
            if (!(s->last_b & TIP)) {
                /* handle end of host to cuda transfer */
                packet_received = (s->data_out_index > 0);

                /*
                 * The host may stop clocking a cuda-to-host transfer (e.g.
                 * an open-ended I2C/RTAS-style read) before fully draining
                 * data_in: it simply knows it has enough bytes and drops
                 * back to idle. Without this, data_in_index stays stuck
                 * below data_in_size forever, autopoll stays blocked, and
                 * subsequent idle-state TACK toggling spins indefinitely
                 * trying to re-signal "more data available" via TREQ.
                 */
                if (s->data_in_index > 0 && s->data_in_index < s->data_in_size) {
                    s->data_in_size = s->data_in_index;
                    adb_autopoll_unblock(adb_bus);
                }

                /* always an IRQ at the end of transfer */
                cuda_delay_set_sr_int(s, CUDA_SR_DELAY_TIP_PULSE);
            }
            /* signal if there is data to read */
            if (s->data_in_index < s->data_in_size) {
                ms->b = (ms->b & ~TREQ);
            }
        }
    }

    s->last_acr = ms->acr;
    s->last_b = ms->b;

    /* NOTE: cuda_receive_packet_from_host() can call cuda_update()
       recursively */
    if (packet_received) {
        len = s->data_out_index;
        s->data_out_index = 0;
        cuda_receive_packet_from_host(s, s->data_out, len);
    }
}

static void cuda_send_packet_to_host(CUDAState *s,
                                     const uint8_t *data, int len)
{
    int i;

    trace_cuda_packet_send(len);
    for (i = 0; i < len; i++) {
        trace_cuda_packet_send_data(i, data[i]);
    }

    memcpy(s->data_in, data, len);
    s->data_in_size = len;
    s->data_in_index = 0;
    cuda_update(s);
    /*
     * Draw the guest's attention to a freshly-queued response -- matches
     * DingusPPC's ViaCuda::autopoll_handler(), which uses this same 30us
     * value specifically for unsolicited announcements (autopoll/one-second
     * packets); direct command responses get their own pulse from
     * cuda_update()'s TIP-transition paths instead, same as there.
     */
    cuda_delay_set_sr_int(s, CUDA_SR_DELAY_ATTENTION);
}

/*
 * True while the host is inside a byte transfer (TIP asserted) or a
 * queued CUDA->host response has not been fully read out. Unsolicited
 * packets (ADB autopoll data, one-second RTC ticks) must not be sent
 * in either state: the real Cuda is a single-threaded microcontroller
 * that only emits them from its idle loop, never mid-transaction --
 * DingusPPC's ViaCuda::autopoll_handler() implements exactly this
 * gate ("Don't send async packets while the host has TIP asserted",
 * "time packets are not urgent enough to preempt"). Sending anyway
 * overwrote data_in underneath the guest's in-progress read, slipping
 * its byte framing: motion deltas got consumed as button/status bytes
 * and packet boundaries smeared -- the long-standing "phantom clicks
 * and cursor jumps under load" input corruption, worst on progress
 * screens where the busy guest drains packets slowly. Skipping is
 * safe: mouse/keyboard deltas keep accumulating in the ADB device for
 * the next poll, and a missed RTC tick is superseded by the next.
 */
static bool cuda_unsolicited_busy(CUDAState *s)
{
    MOS6522State *ms = MOS6522(&s->mos6522_cuda);

    return !(ms->b & TIP) || s->data_in_index < s->data_in_size;
}

static void cuda_adb_poll(void *opaque)
{
    CUDAState *s = opaque;
    ADBBusState *adb_bus = &s->adb_bus;
    uint8_t obuf[ADB_MAX_OUT_LEN + 2];
    int olen;

    if (cuda_unsolicited_busy(s)) {
        return;
    }
    olen = adb_poll(adb_bus, obuf + 2, adb_bus->autopoll_mask);
    if (olen > 0) {
        obuf[0] = ADB_PACKET;
        obuf[1] = 0x40; /* polled data */
        cuda_send_packet_to_host(s, obuf, olen + 2);
    }
}

/*
 * CUDA_SET_ONE_SECOND_MODE (see cuda_cmd_set_one_second_mode below) asks
 * for an unsolicited real-time-clock packet once per real second,
 * independent of ADB autopoll -- confirmed against DingusPPC's
 * ViaCuda::autopoll_handler() one_sec_mode branch. Implements the
 * simplest (mode 1 / "full packet") variant unconditionally rather than
 * DingusPPC's full mode 1/2/3 distinction, since mode 1 is always a
 * valid response regardless of which mode the guest actually asked for.
 */
static void cuda_one_sec_tick(void *opaque)
{
    CUDAState *s = opaque;
    uint8_t obuf[7];
    uint32_t real_time;

    if (s->one_sec_mode == 0) {
        return;
    }

    /*
     * Never preempt an in-progress or unread exchange (see
     * cuda_unsolicited_busy()); this tick's time value is superseded
     * by the next one anyway.
     */
    if (!cuda_unsolicited_busy(s)) {
        real_time = s->tick_offset + (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                      / NANOSECONDS_PER_SECOND);

        obuf[0] = CUDA_PACKET;
        obuf[1] = 0;
        obuf[2] = CUDA_GET_TIME;
        obuf[3] = real_time >> 24;
        obuf[4] = real_time >> 16;
        obuf[5] = real_time >> 8;
        obuf[6] = real_time;
        cuda_send_packet_to_host(s, obuf, sizeof(obuf));
    }

    timer_mod(s->one_sec_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                 + NANOSECONDS_PER_SECOND);
}

/* description of commands */
typedef struct CudaCommand {
    uint8_t command;
    const char *name;
    bool (*handler)(CUDAState *s,
                    const uint8_t *in_args, int in_len,
                    uint8_t *out_args, int *out_len);
} CudaCommand;

static bool cuda_cmd_autopoll(CUDAState *s,
                              const uint8_t *in_data, int in_len,
                              uint8_t *out_data, int *out_len)
{
    ADBBusState *adb_bus = &s->adb_bus;
    bool autopoll;

    if (in_len != 1) {
        return false;
    }

    autopoll = (in_data[0] != 0) ? true : false;

    adb_set_autopoll_enabled(adb_bus, autopoll);
    return true;
}

static bool cuda_cmd_set_autorate(CUDAState *s,
                                  const uint8_t *in_data, int in_len,
                                  uint8_t *out_data, int *out_len)
{
    ADBBusState *adb_bus = &s->adb_bus;

    if (in_len != 1) {
        return false;
    }

    /* we don't want a period of 0 ms */
    /* FIXME: check what real hardware does */
    if (in_data[0] == 0) {
        return false;
    }

    adb_set_autopoll_rate_ms(adb_bus, in_data[0]);
    return true;
}

static bool cuda_cmd_set_device_list(CUDAState *s,
                                     const uint8_t *in_data, int in_len,
                                     uint8_t *out_data, int *out_len)
{
    ADBBusState *adb_bus = &s->adb_bus;
    uint16_t mask;

    if (in_len != 2) {
        return false;
    }

    mask = (((uint16_t)in_data[0]) << 8) | in_data[1];

    adb_set_autopoll_mask(adb_bus, mask);
    return true;
}

static bool cuda_cmd_powerdown(CUDAState *s,
                               const uint8_t *in_data, int in_len,
                               uint8_t *out_data, int *out_len)
{
    if (in_len != 0) {
        return false;
    }

    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    return true;
}

static bool cuda_cmd_reset_system(CUDAState *s,
                                  const uint8_t *in_data, int in_len,
                                  uint8_t *out_data, int *out_len)
{
    if (in_len != 0) {
        return false;
    }

    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    return true;
}

static bool cuda_cmd_set_file_server_flag(CUDAState *s,
                                          const uint8_t *in_data, int in_len,
                                          uint8_t *out_data, int *out_len)
{
    if (in_len != 1) {
        return false;
    }

    qemu_log_mask(LOG_UNIMP,
                  "CUDA: unimplemented command FILE_SERVER_FLAG %d\n",
                  in_data[0]);
    return true;
}

static bool cuda_cmd_set_power_message(CUDAState *s,
                                       const uint8_t *in_data, int in_len,
                                       uint8_t *out_data, int *out_len)
{
    if (in_len != 1) {
        return false;
    }

    qemu_log_mask(LOG_UNIMP,
                  "CUDA: unimplemented command SET_POWER_MESSAGE %d\n",
                  in_data[0]);
    return true;
}

static bool cuda_cmd_get_time(CUDAState *s,
                              const uint8_t *in_data, int in_len,
                              uint8_t *out_data, int *out_len)
{
    uint32_t ti;

    if (in_len != 0) {
        return false;
    }

    ti = s->tick_offset + (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           / NANOSECONDS_PER_SECOND);
    out_data[0] = ti >> 24;
    out_data[1] = ti >> 16;
    out_data[2] = ti >> 8;
    out_data[3] = ti;
    *out_len = 4;
    return true;
}

static bool cuda_cmd_set_time(CUDAState *s,
                              const uint8_t *in_data, int in_len,
                              uint8_t *out_data, int *out_len)
{
    uint32_t ti;

    if (in_len != 4) {
        return false;
    }

    ti = (((uint32_t)in_data[0]) << 24) + (((uint32_t)in_data[1]) << 16)
         + (((uint32_t)in_data[2]) << 8) + in_data[3];
    s->tick_offset = ti - (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           / NANOSECONDS_PER_SECOND);
    return true;
}

/*
 * Real hardware never reads more than a handful of I2C bytes back in a
 * single CUDA pseudo-command (drivers loop, one subaddress per call, for
 * bulk reads like SPD). Cap our synchronous reply to a small fixed size;
 * the guest simply stops clocking once it has what it wants.
 */
#define CUDA_IIC_MAX_LEN 8

static bool cuda_cmd_get_set_iic(CUDAState *s,
                                 const uint8_t *in_data, int in_len,
                                 uint8_t *out_data, int *out_len)
{
    uint8_t addr;
    bool is_recv;
    int i;

    if (in_len < 1) {
        return false;
    }

    addr = in_data[0] >> 1;
    is_recv = in_data[0] & 1;

    if (i2c_start_transfer(s->i2c_bus, addr, is_recv)) {
        trace_cuda_iic_nack(addr, is_recv);
        return false;
    }

    if (is_recv) {
        for (i = 0; i < CUDA_IIC_MAX_LEN; i++) {
            out_data[i] = i2c_recv(s->i2c_bus);
        }
        *out_len = CUDA_IIC_MAX_LEN;
    } else {
        for (i = 1; i < in_len; i++) {
            if (i2c_send(s->i2c_bus, in_data[i])) {
                i2c_end_transfer(s->i2c_bus);
                trace_cuda_iic_nack(addr, is_recv);
                return false;
            }
        }
        *out_len = 0;
    }

    i2c_end_transfer(s->i2c_bus);
    return true;
}

static bool cuda_cmd_combined_format_iic(CUDAState *s,
                                         const uint8_t *in_data, int in_len,
                                         uint8_t *out_data, int *out_len)
{
    uint8_t addr, addr1, sub_addr;
    bool is_recv;
    int i;

    if (in_len < 3) {
        return false;
    }

    addr = in_data[0] >> 1;
    sub_addr = in_data[1];
    addr1 = in_data[2] >> 1;
    is_recv = in_data[2] & 1;

    if ((in_data[0] & 0xfe) != (in_data[2] & 0xfe)) {
        return false;
    }

    if (i2c_start_send(s->i2c_bus, addr)) {
        trace_cuda_iic_nack(addr, is_recv);
        return false;
    }
    if (i2c_send(s->i2c_bus, sub_addr)) {
        i2c_end_transfer(s->i2c_bus);
        trace_cuda_iic_nack(addr, is_recv);
        return false;
    }

    if (is_recv) {
        /*
         * Repeated start: do NOT end the transfer here. QEMU's I2C core
         * treats a start_transfer while devices are already selected as a
         * repeated start (see i2c_do_start_transfer()), which is exactly
         * the SMBus "write subaddress, restart, read" protocol real SPD
         * EEPROM drivers use.
         */
        if (i2c_start_recv(s->i2c_bus, addr1)) {
            i2c_end_transfer(s->i2c_bus);
            trace_cuda_iic_nack(addr1, is_recv);
            return false;
        }
        for (i = 0; i < CUDA_IIC_MAX_LEN; i++) {
            out_data[i] = i2c_recv(s->i2c_bus);
        }
        *out_len = CUDA_IIC_MAX_LEN;
    } else {
        for (i = 3; i < in_len; i++) {
            if (i2c_send(s->i2c_bus, in_data[i])) {
                i2c_end_transfer(s->i2c_bus);
                return false;
            }
        }
        *out_len = 0;
    }

    i2c_end_transfer(s->i2c_bus);
    return true;
}

/*
 * Real hardware wires one CUDA VIA port-B bit (PB0) out to a board-specific
 * general-purpose line; nothing on a Beige G3 desktop reads it back. Real
 * firmware issues this as a fire-and-forget pseudo-command expecting a
 * trivial ack -- matches DingusPPC's ViaCuda handling of the same command,
 * which just logs and acks. Without an ack, the caller gets an ERROR_PACKET
 * it isn't expecting instead.
 */
static bool cuda_cmd_out_pb0(CUDAState *s,
                             const uint8_t *in_data, int in_len,
                             uint8_t *out_data, int *out_len)
{
    if (in_len != 1) {
        return false;
    }

    *out_len = 0;
    return true;
}

/*
 * Enables/disables the periodic real-time-clock "tick" packet handled by
 * cuda_one_sec_tick() above. Real classic Mac OS issues this during boot
 * (confirmed via -d guest_errors on this exact ROM: "CUDA: unknown
 * command 0x1b" fired before this was implemented) and DingusPPC's own
 * ViaCuda handles it the same minimal way -- store the mode, ack, and
 * let the timer pick it up.
 */
static bool cuda_cmd_set_one_second_mode(CUDAState *s,
                                         const uint8_t *in_data, int in_len,
                                         uint8_t *out_data, int *out_len)
{
    if (in_len != 1) {
        return false;
    }

    s->one_sec_mode = in_data[0];
    if (s->one_sec_mode != 0) {
        timer_mod(s->one_sec_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                     + NANOSECONDS_PER_SECOND);
    } else {
        timer_del(s->one_sec_timer);
    }

    *out_len = 0;
    return true;
}

/*
 * CUDA_TIMER_TICKLE (0x24): real firmware/OS use is undocumented and
 * DingusPPC's own ViaCuda logs it as an "unsupported pseudo command"
 * too -- but it still sends a trivial ack rather than rejecting it.
 * Before this, our lack of any handler meant an ERROR_PACKET response,
 * and this exact ROM was observed retrying the command 19 times in a
 * row waiting for a real acknowledgement it never got.
 */
static bool cuda_cmd_timer_tickle(CUDAState *s,
                                  const uint8_t *in_data, int in_len,
                                  uint8_t *out_data, int *out_len)
{
    if (in_len != 1) {
        return false;
    }

    *out_len = 0;
    return true;
}

/*
 * CUDA_READ_MCU_MEM / CUDA_WRITE_MCU_MEM (a.k.a GET/SET_6805_ADDR):
 * real hardware exposes the Cuda MCU's own internal memory (including a
 * second path into PRAM) through these; DingusPPC implements the PRAM
 * aliasing and a small fake ROM-header reply for other addresses. We
 * only need the trivial ack here -- this exact ROM was observed issuing
 * these but only to probe/initialize, not to stream real data through
 * this path (PRAM is already reachable via CUDA_GET_PRAM/SET_PRAM).
 */
static bool cuda_cmd_read_mcu_mem(CUDAState *s,
                                  const uint8_t *in_data, int in_len,
                                  uint8_t *out_data, int *out_len)
{
    if (in_len != 2) {
        return false;
    }

    *out_len = 0;
    return true;
}

static bool cuda_cmd_write_mcu_mem(CUDAState *s,
                                   const uint8_t *in_data, int in_len,
                                   uint8_t *out_data, int *out_len)
{
    if (in_len < 2) {
        return false;
    }

    *out_len = 0;
    return true;
}

/*
 * Extended Parameter RAM access. Real Old World ROMs read/write PRAM
 * through the Cuda chip rather than a memory-mapped register: the
 * request carries a 2-byte big-endian offset, and GET_PRAM replies with
 * the single byte at that offset while SET_PRAM takes a 3rd byte to
 * store there.
 */
static bool cuda_cmd_get_pram(CUDAState *s,
                              const uint8_t *in_data, int in_len,
                              uint8_t *out_data, int *out_len)
{
    uint16_t addr;

    if (in_len != 2) {
        return false;
    }

    addr = (((uint16_t)in_data[0]) << 8) | in_data[1];
    out_data[0] = s->pram[addr % sizeof(s->pram)];
    *out_len = 1;
    return true;
}

/*
 * Auto-manage a default PRAM backing file, "pram.img", when the user
 * hasn't attached one explicitly via -global cuda.drive=... -- mirrors
 * mac_oldworld.c's mac_oldworld_default_nvram_blk() for "nvram.img"
 * exactly (same auto-create-if-missing, auto-size-up semantics), so
 * PRAM-resident guest settings persist across runs by default just
 * like real hardware's battery-backed PRAM, without the user needing
 * to pre-create an empty file by hand.
 */
static BlockBackend *cuda_default_pram_blk(void)
{
    static const char filename[] = "pram.img";
    struct stat st;
    BlockBackend *blk;
    Error *local_err = NULL;
    int fd;

    fd = qemu_open_old(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        warn_report("could not open/create default PRAM image '%s': %s",
                    filename, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) == 0 && st.st_size < (int64_t)sizeof(((CUDAState *)0)->pram)) {
        if (ftruncate(fd, sizeof(((CUDAState *)0)->pram)) != 0) {
            warn_report("could not size default PRAM image '%s': %s",
                        filename, strerror(errno));
        }
    }
    close(fd);

    QDict *options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(filename, NULL, options, BDRV_O_RDWR, &local_err);
    if (!blk) {
        warn_report_err(local_err);
        return NULL;
    }
    return blk;
}

/* Write PRAM back out to the optional backing file (see the "drive"
 * property), matching q800's mac_via pram_update() -- without this,
 * PRAM-resident settings (e.g. Memory control panel's virtual memory
 * on/off flag) reset to zero on every fresh QEMU invocation. */
static void cuda_pram_update(CUDAState *s)
{
    if (s->pram_blk) {
        if (blk_pwrite(s->pram_blk, 0, sizeof(s->pram), s->pram, 0) < 0) {
            qemu_log("cuda_pram_update: cannot write to file\n");
        }
    }
}

static bool cuda_cmd_set_pram(CUDAState *s,
                              const uint8_t *in_data, int in_len,
                              uint8_t *out_data, int *out_len)
{
    uint16_t addr;

    if (in_len != 3) {
        return false;
    }

    addr = (((uint16_t)in_data[0]) << 8) | in_data[1];
    s->pram[addr % sizeof(s->pram)] = in_data[2];
    cuda_pram_update(s);
    *out_len = 0;
    return true;
}

static const CudaCommand handlers[] = {
    { CUDA_AUTOPOLL, "AUTOPOLL", cuda_cmd_autopoll },
    { CUDA_SET_AUTO_RATE, "SET_AUTO_RATE",  cuda_cmd_set_autorate },
    { CUDA_SET_DEVICE_LIST, "SET_DEVICE_LIST", cuda_cmd_set_device_list },
    { CUDA_POWERDOWN, "POWERDOWN", cuda_cmd_powerdown },
    { CUDA_RESET_SYSTEM, "RESET_SYSTEM", cuda_cmd_reset_system },
    { CUDA_FILE_SERVER_FLAG, "FILE_SERVER_FLAG",
      cuda_cmd_set_file_server_flag },
    { CUDA_SET_POWER_MESSAGES, "SET_POWER_MESSAGES",
      cuda_cmd_set_power_message },
    { CUDA_GET_TIME, "GET_TIME", cuda_cmd_get_time },
    { CUDA_SET_TIME, "SET_TIME", cuda_cmd_set_time },
    { CUDA_GET_PRAM, "GET_PRAM", cuda_cmd_get_pram },
    { CUDA_SET_PRAM, "SET_PRAM", cuda_cmd_set_pram },
    { CUDA_GET_SET_IIC, "GET_SET_IIC", cuda_cmd_get_set_iic },
    { CUDA_COMBINED_FORMAT_IIC, "COMBINED_FORMAT_IIC",
      cuda_cmd_combined_format_iic },
    { CUDA_OUT_PB0, "OUT_PB0", cuda_cmd_out_pb0 },
    { CUDA_SET_ONE_SECOND_MODE, "SET_ONE_SECOND_MODE",
      cuda_cmd_set_one_second_mode },
    { CUDA_TIMER_TICKLE, "TIMER_TICKLE", cuda_cmd_timer_tickle },
    { CUDA_GET_6805_ADDR, "READ_MCU_MEM", cuda_cmd_read_mcu_mem },
    { CUDA_SET_6805_ADDR, "WRITE_MCU_MEM", cuda_cmd_write_mcu_mem },
};

static void cuda_receive_packet(CUDAState *s,
                                const uint8_t *data, int len)
{
    uint8_t obuf[32] = { CUDA_PACKET, 0, data[0] };
    int i, out_len = 0;

    for (i = 0; i < ARRAY_SIZE(handlers); i++) {
        const CudaCommand *desc = &handlers[i];
        if (desc->command == data[0]) {
            trace_cuda_receive_packet_cmd(desc->name);
            out_len = 0;
            if (desc->handler(s, data + 1, len - 1, obuf + 3, &out_len)) {
                cuda_send_packet_to_host(s, obuf, 3 + out_len);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "CUDA: %s: wrong parameters %d\n",
                              desc->name, len);
                obuf[0] = ERROR_PACKET;
                obuf[1] = 0x5; /* bad parameters */
                obuf[2] = CUDA_PACKET;
                obuf[3] = data[0];
                cuda_send_packet_to_host(s, obuf, 4);
            }
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR, "CUDA: unknown command 0x%02x\n", data[0]);
    obuf[0] = ERROR_PACKET;
    obuf[1] = 0x2; /* unknown command */
    obuf[2] = CUDA_PACKET;
    obuf[3] = data[0];
    cuda_send_packet_to_host(s, obuf, 4);
}

static void cuda_receive_packet_from_host(CUDAState *s,
                                          const uint8_t *data, int len)
{
    int i;

    trace_cuda_packet_receive(len);
    for (i = 0; i < len; i++) {
        trace_cuda_packet_receive_data(i, data[i]);
    }

    switch(data[0]) {
    case ADB_PACKET:
        {
            uint8_t obuf[ADB_MAX_OUT_LEN + 3];
            int olen;
            olen = adb_request(&s->adb_bus, obuf + 3, data + 1, len - 1);
            obuf[0] = ADB_PACKET;
            /*
             * Bit 0x80 of the status byte (ADB_STAT_RESPONSE) marks this
             * packet as a real ADB response the host must read back --
             * confirmed against DingusPPC's ViaCuda::process_adb_command(),
             * which always ORs this bit in regardless of success/error.
             * Previously this bit was never set (status was 0x00 on
             * success, or a bare error code on failure), so the ROM's
             * ADB-reset code never recognized the packet as needing to
             * be drained and left TREQ asserted forever. olen == 0 is a
             * legitimate non-error outcome (e.g. a SendReset broadcast,
             * which every device acknowledges but none of them talk back
             * on), not just "no data".
             */
            if (olen >= 0) {
                obuf[1] = ADB_STAT_RESPONSE;
            } else {
                obuf[1] = ADB_STAT_RESPONSE | -olen;
                olen = 0;
            }
            obuf[2] = data[1];
            cuda_send_packet_to_host(s, obuf, olen + 3);
        }
        break;
    case CUDA_PACKET:
        cuda_receive_packet(s, data + 1, len - 1);
        break;
    }
}

static uint64_t mos6522_cuda_read(void *opaque, hwaddr addr, unsigned size)
{
    CUDAState *s = opaque;
    MOS6522CUDAState *mcs = &s->mos6522_cuda;
    MOS6522State *ms = MOS6522(mcs);

    addr = (addr >> 9) & 0xf;
    return mos6522_read(ms, addr, size);
}

static void mos6522_cuda_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    CUDAState *s = opaque;
    MOS6522CUDAState *mcs = &s->mos6522_cuda;
    MOS6522State *ms = MOS6522(mcs);

    addr = (addr >> 9) & 0xf;
    mos6522_write(ms, addr, val, size);
}

static const MemoryRegionOps mos6522_cuda_ops = {
    .read = mos6522_cuda_read,
    .write = mos6522_cuda_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const VMStateDescription vmstate_cuda = {
    .name = "cuda",
    .version_id = 8,
    .minimum_version_id = 8,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(mos6522_cuda.parent_obj, CUDAState, 0, vmstate_mos6522,
                       MOS6522State),
        VMSTATE_UINT8(last_b, CUDAState),
        VMSTATE_UINT8(last_acr, CUDAState),
        VMSTATE_INT32(data_in_size, CUDAState),
        VMSTATE_INT32(data_in_index, CUDAState),
        VMSTATE_INT32(data_out_index, CUDAState),
        VMSTATE_BUFFER(data_in, CUDAState),
        VMSTATE_BUFFER(data_out, CUDAState),
        VMSTATE_UINT32(tick_offset, CUDAState),
        VMSTATE_TIMER_PTR(sr_delay_timer, CUDAState),
        VMSTATE_BUFFER(pram, CUDAState),
        VMSTATE_TIMER_PTR(one_sec_timer, CUDAState),
        VMSTATE_UINT8(one_sec_mode, CUDAState),
        VMSTATE_END_OF_LIST()
    }
};

static void cuda_reset(DeviceState *dev)
{
    CUDAState *s = CUDA(dev);
    ADBBusState *adb_bus = &s->adb_bus;

    s->data_in_size = 0;
    s->data_in_index = 0;
    s->data_out_index = 0;

    s->one_sec_mode = 0;
    timer_del(s->one_sec_timer);

    adb_set_autopoll_enabled(adb_bus, false);
}

static void cuda_realize(DeviceState *dev, Error **errp)
{
    CUDAState *s = CUDA(dev);
    SysBusDevice *sbd;
    ADBBusState *adb_bus = &s->adb_bus;
    struct tm tm;

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mos6522_cuda), errp)) {
        return;
    }

    /* Pass IRQ from 6522 */
    sbd = SYS_BUS_DEVICE(s);
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->mos6522_cuda));

    qemu_get_timedate(&tm, 0);
    s->tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;

    s->sr_delay_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cuda_set_sr_int, s);
    s->one_sec_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cuda_one_sec_tick, s);

    adb_register_autopoll_callback(adb_bus, cuda_adb_poll, s);

    if (!s->pram_blk) {
        s->pram_blk = cuda_default_pram_blk();
    }

    if (s->pram_blk) {
        int64_t len = blk_getlength(s->pram_blk);
        int ret;

        if (len < 0) {
            error_setg_errno(errp, -len,
                             "could not get length of PRAM backing image");
            return;
        }
        ret = blk_set_perm(s->pram_blk,
                           BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                           BLK_PERM_ALL, errp);
        if (ret < 0) {
            return;
        }

        ret = blk_pread(s->pram_blk, 0, sizeof(s->pram), s->pram, 0);
        if (ret < 0) {
            error_setg(errp, "can't read PRAM contents");
            return;
        }
    }
}

static void cuda_init(Object *obj)
{
    CUDAState *s = CUDA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "mos6522-cuda", &s->mos6522_cuda,
                            TYPE_MOS6522_CUDA);

    memory_region_init_io(&s->mem, obj, &mos6522_cuda_ops, s, "cuda", 0x2000);
    sysbus_init_mmio(sbd, &s->mem);

    qbus_init(&s->adb_bus, sizeof(s->adb_bus), TYPE_ADB_BUS,
              DEVICE(obj), "adb.0");

    s->i2c_bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static const Property cuda_properties[] = {
    DEFINE_PROP_UINT64("timebase-frequency", CUDAState, tb_frequency, 0),
    DEFINE_PROP_DRIVE("drive", CUDAState, pram_blk),
};

static void cuda_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = cuda_realize;
    device_class_set_legacy_reset(dc, cuda_reset);
    dc->vmsd = &vmstate_cuda;
    device_class_set_props(dc, cuda_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo cuda_type_info = {
    .name = TYPE_CUDA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CUDAState),
    .instance_init = cuda_init,
    .class_init = cuda_class_init,
};

static void mos6522_cuda_portB_write(MOS6522State *s)
{
    MOS6522CUDAState *mcs = container_of(s, MOS6522CUDAState, parent_obj);
    CUDAState *cs = container_of(mcs, CUDAState, mos6522_cuda);

    cuda_update(cs);
}

static void mos6522_cuda_reset_hold(Object *obj, ResetType type)
{
    MOS6522State *ms = MOS6522(obj);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /*
     * Both VIA timers share a single real hardware clock -- there is only
     * one Phi2 input feeding the whole 6522, confirmed against DingusPPC's
     * ViaCuda (a single VIA_CLOCK_HZ = 783360, used unscaled for both
     * timers; our own CUDA_TIMER_FREQ = 4700000/6 = 783333.33 is the same
     * real value modulo rounding). Timer2 here previously used a wildly
     * different, much smaller rate ((SCALE_US*6000)/4700 = ~1276.6, over
     * 600x slower) dating back to a 2015 MacOS-9-hang fix (commit
     * a53cfdcca2) -- whatever that value compensated for, it leaves Timer2
     * decrementing far slower than real hardware. This ROM's own boot-time
     * hardware-timing-calibration loop (reads Timer2's live counter twice,
     * subtracts, compares against a threshold to measure real elapsed
     * time -- see project notes on the 0xffc34766-347a2 loop) depends on
     * Timer2 actually ticking at the real VIA rate; at 1/600th speed the
     * loop's exit condition is essentially never satisfied.
     */
    ms->timers[0].frequency = CUDA_TIMER_FREQ;
    ms->timers[1].frequency = CUDA_TIMER_FREQ;

    /*
     * Generic MOS6522 reset zeroes port B, which leaves TREQ (active low)
     * asserted. Real hardware powers up with TREQ negated (nothing
     * pending) -- confirmed against DingusPPC's ViaCuda::cuda_init(),
     * which explicitly sets treq=1 at reset. Without this, real Beige G3
     * ROM's early ADB bus reset/enumeration code -- which watches TREQ
     * going idle as its loop-exit condition -- waits forever, since
     * nothing else in the idle-state TACK/TREQ handling corrects a
     * bogus initial assertion that was never a real transfer.
     */
    ms->b |= TREQ;
}

static void mos6522_cuda_class_init(ObjectClass *oc, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);

    resettable_class_set_parent_phases(rc, NULL, mos6522_cuda_reset_hold,
                                       NULL, &mdc->parent_phases);
    mdc->portB_write = mos6522_cuda_portB_write;
    /*
     * Mac OS only uses Timer 1 for its own PowerPC-timebase calibration
     * on startup (see the comment on cuda_get_counter_value() above) --
     * these overrides return load_time/counter_value scaled into
     * PowerPC-timebase-tick units instead of plain nanoseconds, which is
     * only correct for that specific Timer 1 calibration use. Timer 2 is
     * used generically for ordinary real-time-based interrupt scheduling
     * (ADB protocol delays, misc timeouts) by hw/misc/mos6522.c's shared
     * get_next_irq_time()/mos6522_read(), which always treats
     * ti->load_time as plain nanoseconds -- wiring Timer 2 to these same
     * timebase-scaled overrides (as this code previously did) corrupts
     * every such computation by a factor of NANOSECONDS_PER_SECOND /
     * tb_frequency (~59.87x at this board's real 16,705,000 Hz
     * timebase), confirmed this session via direct instrumentation of
     * get_next_irq_time(): ti->load_time (in tick units) was being
     * subtracted directly from qemu_clock_get_ns() (in ns), producing
     * wildly wrong elapsed-time and next-fire computations and a
     * genuine, confirmed livelock in this ROM's VIA-Timer2-driven ADB/
     * boot code. Leaving Timer 2 on the base class's plain, correct,
     * nanosecond-based defaults (MOS6522_CLASS's own class_init already
     * sets both timers to those; simply not overriding Timer 2 here is
     * sufficient) fixes this without touching Timer 1's own, genuinely
     * correct and still-needed calibration behavior.
     */
    mdc->get_timer1_counter_value = cuda_get_counter_value;
    mdc->get_timer1_load_time = cuda_get_load_time;
}

static const TypeInfo mos6522_cuda_type_info = {
    .name = TYPE_MOS6522_CUDA,
    .parent = TYPE_MOS6522,
    .instance_size = sizeof(MOS6522CUDAState),
    .class_init = mos6522_cuda_class_init,
};

static void cuda_register_types(void)
{
    type_register_static(&mos6522_cuda_type_info);
    type_register_static(&cuda_type_info);
}

type_init(cuda_register_types)
