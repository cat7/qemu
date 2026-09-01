/*
 * QEMU MOS6522 VIA emulation
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 * Copyright (c) 2018 Mark Cave-Ayland
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
#include "hw/misc/mos6522.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"
#include "monitor/hmp.h"
#include "qapi/type-helpers.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/calib-governor.h"
#include "trace.h"


static const char *mos6522_reg_names[MOS6522_NUM_REGS] = {
    "ORB", "ORA", "DDRB", "DDRA", "T1CL", "T1CH", "T1LL", "T1LH",
    "T2CL", "T2CH", "SR", "ACR", "PCR", "IFR", "IER", "ANH"
};


/* XXX: implement all timer modes */

static void mos6522_timer1_update(MOS6522State *s, MOS6522Timer *ti,
                                  int64_t current_time);
static void mos6522_timer2_update(MOS6522State *s, MOS6522Timer *ti,
                                  int64_t current_time);

static void mos6522_update_irq(MOS6522State *s)
{
    if (s->ifr & s->ier) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void mos6522_set_irq(void *opaque, int n, int level)
{
    MOS6522State *s = MOS6522(opaque);
    int last_level = !!(s->last_irq_levels & (1 << n));
    uint8_t last_ifr = s->ifr;
    bool positive_edge = true;
    int ctrl;

    /*
     * SR_INT is managed by mos6522 instances and cleared upon SR
     * read. It is only the external CA1/2 and CB1/2 lines that
     * are edge-triggered and latched in IFR
     */
    if (n != SR_INT_BIT && level == last_level) {
        return;
    }

    /* Detect negative edge trigger */
    if (last_level == 1 && level == 0) {
        positive_edge = false;
    }

    switch (n) {
    case CA2_INT_BIT:
        ctrl = (s->pcr & CA2_CTRL_MASK) >> CA2_CTRL_SHIFT;
        if ((positive_edge && (ctrl & C2_POS)) ||
             (!positive_edge && !(ctrl & C2_POS))) {
            s->ifr |= 1 << n;
        }
        break;
    case CA1_INT_BIT:
        ctrl = (s->pcr & CA1_CTRL_MASK) >> CA1_CTRL_SHIFT;
        if ((positive_edge && (ctrl & C1_POS)) ||
             (!positive_edge && !(ctrl & C1_POS))) {
            s->ifr |= 1 << n;
        }
        break;
    case SR_INT_BIT:
        s->ifr |= 1 << n;
        break;
    case CB2_INT_BIT:
        ctrl = (s->pcr & CB2_CTRL_MASK) >> CB2_CTRL_SHIFT;
        if ((positive_edge && (ctrl & C2_POS)) ||
             (!positive_edge && !(ctrl & C2_POS))) {
            s->ifr |= 1 << n;
        }
        break;
    case CB1_INT_BIT:
        ctrl = (s->pcr & CB1_CTRL_MASK) >> CB1_CTRL_SHIFT;
        if ((positive_edge && (ctrl & C1_POS)) ||
             (!positive_edge && !(ctrl & C1_POS))) {
            s->ifr |= 1 << n;
        }
        break;
    }

    if (s->ifr != last_ifr) {
        mos6522_update_irq(s);
    }

    if (level) {
        s->last_irq_levels |= 1 << n;
    } else {
        s->last_irq_levels &= ~(1 << n);
    }
}

static uint64_t get_counter_value(MOS6522State *s, MOS6522Timer *ti)
{
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);

    if (ti->index == 0) {
        return mdc->get_timer1_counter_value(s, ti);
    } else {
        return mdc->get_timer2_counter_value(s, ti);
    }
}

static uint64_t get_load_time(MOS6522State *s, MOS6522Timer *ti)
{
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);

    if (ti->index == 0) {
        return mdc->get_timer1_load_time(s, ti);
    } else {
        return mdc->get_timer2_load_time(s, ti);
    }
}

/*
 * Real-time window (ns) within which two get_counter() reads of the same
 * timer are treated as part of one back-to-back measurement pair -- see
 * the comment in get_counter() for what this guards against. Kept tight
 * (a couple of this timer's own ~1.276us ticks, CUDA_TIMER_FREQ) rather
 * than generous: a wider window (10us was tried and measured insufficient
 * -- still ~80Hz on a Ticks counter that should read 60.15Hz) risks
 * treating genuinely frequent-but-unrelated legitimate polling as part of
 * the same pair, re-anchoring the window on every such call and never
 * actually triggering the reset below.
 */
#define TIMER_MONOTONIC_WINDOW_NS 2500

/*
 * Bound on how far get_counter()'s forced advancement may run ahead of
 * the honestly-computed value even *within* an active monotonic window
 * -- belt-and-suspenders alongside the window reset above, for a busy
 * loop tight enough to make several back-to-back calls within a single
 * window. At this timer's ~1.276us tick period, 16 ticks is ~20us of
 * maximum induced drift.
 */
#define TIMER_MAX_AHEAD_OF_REAL 16

static unsigned int get_counter(MOS6522State *s, MOS6522Timer *ti)
{
    int64_t d;
    unsigned int counter;
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    d = get_counter_value(s, ti);

    /*
     * get_counter_value() derives elapsed ticks from real host wall-clock
     * time (QEMU_CLOCK_VIRTUAL), scaled per-timer (~783kHz/1.276us ticks
     * for the generic VIA path, a different CUDA-specific transform for
     * Timer1 -- see cuda_get_counter_value()) -- genuinely correct and
     * hardware-accurate on its own. But real classic Mac ROM code (the
     * Time Manager's CPU-speed calibration, backing _InsTime/_RmvTime/
     * _PrimeTime) measures elapsed time by arming Timer1 with _PrimeTime,
     * then almost immediately reading it back via _RmvTime -- twice in a
     * row, once with negligible intervening work and once after a short
     * busy-loop -- and unconditionally divides by the difference between
     * the two measurements. Confirmed via live ROM disassembly (the
     * calibration routine around 0x4f6bde) and register capture: each
     * _PrimeTime reloads the timer (a fresh load_time epoch), so the two
     * measurements are independent elapsed-since-reload values, and on a
     * host fast enough to execute the ROM's own trap-dispatch and Prime/
     * Remove sequence itself in under one tick period, *both* independent
     * measurements can honestly compute to the same value -- not just a
     * single read landing at d=0, but two separate reload-then-read
     * cycles each doing so, which a fix scoped to a single load epoch
     * (an earlier version of this fix, keyed only off qemu_clock_get_ns()
     * vs this load's ti->load_time) cannot catch.
     *
     * Real silicon never hits this: a 68K/PPC750-era CPU takes measurable,
     * non-negligible real time to execute the ROM's own trap dispatch
     * between two such measurements, comfortably more than one VIA tick
     * period, because CPU and VIA timer share the same physical clock
     * domain by design -- so two independent measurements always differ
     * by at least a little, even when the "real work" between them is
     * itself negligible. TCG on a modern host can execute that same
     * sequence far faster than 233-300MHz silicon ever could, decoupling
     * that inherent pairing.
     *
     * Guarantee the same real-hardware invariant this ROM code implicitly
     * (if unknowingly) depends on -- each read of this timer observes
     * strictly more progress than the previous one did -- by tracking the
     * last value ANY read of this timer returned. This CANNOT persist
     * indefinitely across every future reload the way an earlier version
     * of this fix did, though: Timer1 gets reloaded constantly during
     * ordinary Time Manager operation (deferred tasks, scheduling), each
     * with its own fresh load_time epoch -- so a watermark that's never
     * reset ends up comparing entirely unrelated, genuinely-separated-in-
     * real-time reload+read cycles against a stale high-water mark from
     * some earlier, unconnected access, forcing them to "advance" too.
     * Measured empirically: that version made the guest's whole notion of
     * elapsed time (anything timed off this counter, including the
     * VBL-driven Ticks counter backing double-click/key-repeat timing)
     * run at 4-16x real speed depending on the exact bound used, not just
     * a negligible one-time offset. Scope the guarantee to real wall-
     * clock proximity instead: only force advancement if this read is
     * happening within TIMER_MONOTONIC_WINDOW_NS of the previous one,
     * matching the actual narrow case this exists for (two measurements
     * from the same calibration sequence, genuinely close together in
     * real host time) -- if real time has clearly moved on since the
     * last read, trust the honestly-computed value and let the watermark
     * reset, since that's a fresh, unrelated access, not a continuation
     * of the same measurement pair. This doesn't affect IRQ scheduling
     * (computed independently by get_next_irq_time()); it only changes
     * the guest-visible value in the narrow, real-hardware-unreachable
     * case where two back-to-back reads would otherwise regress or
     * repeat.
     */
    if (now_ns - ti->last_read_real_ns >= TIMER_MONOTONIC_WINDOW_NS) {
        ti->last_read_d = -1;
    }
    if (d <= ti->last_read_d) {
        if (ti->last_read_d - d < TIMER_MAX_AHEAD_OF_REAL) {
            d = ti->last_read_d + 1;
        } else {
            d = ti->last_read_d;
        }
    } else {
        /*
         * Genuine, honest advance: this is a real sync point with real
         * time, so anchor the window here. Deliberately NOT done on a
         * forced/held read above -- if it were, a continuous run of
         * back-to-back-in-real-time calls (each one individually within
         * the window of the previous) would keep pushing the deadline
         * forward forever and the reset above would never trigger,
         * reintroducing unbounded growth by another route. Anchoring
         * only on genuine advances means the window measures "how long
         * since we last KNEW we were caught up to real time", so a
         * sustained forcing streak gets cut off after one window's worth
         * of real time has elapsed, however many calls happened in it.
         */
        ti->last_read_real_ns = now_ns;
    }
    ti->last_read_d = d;

    if (ti->index == 0) {
        /* the timer goes down from latch to -1 (period of latch + 2) */
        if (d <= (ti->counter_value + 1)) {
            counter = (ti->counter_value - d) & 0xffff;
        } else {
            counter = (d - (ti->counter_value + 1)) % (ti->latch + 2);
            counter = (ti->latch - counter) & 0xffff;
        }
    } else {
        counter = (ti->counter_value - d) & 0xffff;
    }
    return counter;
}

static void set_counter(MOS6522State *s, MOS6522Timer *ti, unsigned int val)
{
    trace_mos6522_set_counter(1 + ti->index, val);
    ti->load_time = get_load_time(s, ti);
    ti->counter_value = val;
    if (ti->index == 0) {
        mos6522_timer1_update(s, ti, ti->load_time);
    } else {
        mos6522_timer2_update(s, ti, ti->load_time);
    }
}

static int64_t get_next_irq_time(MOS6522State *s, MOS6522Timer *ti,
                                 int64_t current_time)
{
    int64_t d, next_time;
    unsigned int counter;

    if (ti->frequency == 0) {
        return INT64_MAX;
    }

    /* current counter value */
    d = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - ti->load_time,
                 ti->frequency, NANOSECONDS_PER_SECOND);

    /* the timer goes down from latch to -1 (period of latch + 2) */
    if (d <= (ti->counter_value + 1)) {
        counter = (ti->counter_value - d) & 0xffff;
    } else {
        counter = (d - (ti->counter_value + 1)) % (ti->latch + 2);
        counter = (ti->latch - counter) & 0xffff;
    }

    /* Note: we consider the irq is raised on 0 */
    if (counter == 0xffff) {
        next_time = d + ti->latch + 1;
    } else if (counter == 0) {
        next_time = d + ti->latch + 2;
    } else {
        next_time = d + counter;
    }
    trace_mos6522_get_next_irq_time(ti->latch, d, next_time - d);
    next_time = muldiv64(next_time, NANOSECONDS_PER_SECOND, ti->frequency) +
                         ti->load_time;

    if (next_time <= current_time) {
        next_time = current_time + 1;
    }
    return next_time;
}

static void mos6522_timer1_update(MOS6522State *s, MOS6522Timer *ti,
                                 int64_t current_time)
{
    if (!ti->timer) {
        return;
    }
    ti->next_irq_time = get_next_irq_time(s, ti, current_time);
    if ((s->ier & T1_INT) == 0 || (s->acr & T1MODE) != T1MODE_CONT) {
        timer_del(ti->timer);
    } else {
        timer_mod(ti->timer, ti->next_irq_time);
    }
}

static void mos6522_timer2_update(MOS6522State *s, MOS6522Timer *ti,
                                 int64_t current_time)
{
    if (!ti->timer) {
        return;
    }
    ti->next_irq_time = get_next_irq_time(s, ti, current_time);
    if ((s->ier & T2_INT) == 0) {
        timer_del(ti->timer);
    } else {
        timer_mod(ti->timer, ti->next_irq_time);
    }
}

static void mos6522_timer1(void *opaque)
{
    MOS6522State *s = opaque;
    MOS6522Timer *ti = &s->timers[0];

    mos6522_timer1_update(s, ti, ti->next_irq_time);
    s->ifr |= T1_INT;
    mos6522_update_irq(s);
}

static void mos6522_timer2(void *opaque)
{
    MOS6522State *s = opaque;

    /*
     * Unlike T1, real 6522 hardware's T2 (in its only mode relevant here,
     * timed-interrupt mode) is one-shot: it counts down once, fires, and
     * then stops -- it does not automatically reload and keep firing.
     * Software must explicitly rewrite T2C-H to arm it again (see
     * set_counter(), called from the VIA_REG_T2CH write case). Confirmed
     * against DingusPPC's ViaCuda, which arms T2 with an explicit
     * add_oneshot_timer() rather than any repeating/continuous timer.
     * Re-arming here unconditionally (as this used to do, mirroring T1)
     * turned T2 into a free-running periodic interrupt source once
     * enabled via IER, which native ROM code relying on T2's real
     * one-shot semantics never expects and can spin on forever.
     */
    s->ifr |= T2_INT;
    mos6522_update_irq(s);
}

/*
 * Deliver an overdue T2 one-shot interrupt inline. Invoked (BQL held)
 * by the calibration governor from the paced vCPU when the one-shot's
 * expiry has passed on the host clock: the calibration spin is a pure
 * register loop, so the lazy catch-up in mos6522_read() can never fire
 * for it, and on hosts whose main loop services QEMU timers later than
 * the window's guard tail (win32) the spin would otherwise outlive the
 * paced window and run out at raw host speed -- making the calibrated
 * value host-dependent however exact the pacing was. Never early: the
 * expiry is rechecked against the virtual clock here. The pending
 * main-loop timer is cancelled so the interrupt is not delivered a
 * second time after the guest's ISR has already cleared it.
 */
static void mos6522_t2_irq_catch_up(void *opaque)
{
    MOS6522State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now < s->timers[1].next_irq_time || (s->ifr & T2_INT)) {
        return;
    }
    if (s->timers[1].timer) {
        timer_del(s->timers[1].timer);
    }
    trace_mos6522_t2_irq_catch_up(now - s->timers[1].next_irq_time);
    s->ifr |= T2_INT;
    mos6522_update_irq(s);
}

static uint64_t mos6522_get_counter_value(MOS6522State *s, MOS6522Timer *ti)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - ti->load_time,
                    ti->frequency, NANOSECONDS_PER_SECOND);
}

static uint64_t mos6522_get_load_time(MOS6522State *s, MOS6522Timer *ti)
{
    uint64_t load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return load_time;
}

static void mos6522_portA_write(MOS6522State *s)
{
    qemu_log_mask(LOG_UNIMP, "portA_write unimplemented\n");
}

static void mos6522_portB_write(MOS6522State *s)
{
    qemu_log_mask(LOG_UNIMP, "portB_write unimplemented\n");
}

uint64_t mos6522_read(void *opaque, hwaddr addr, unsigned size)
{
    MOS6522State *s = opaque;
    uint32_t val;
    int ctrl;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now >= s->timers[0].next_irq_time) {
        mos6522_timer1_update(s, &s->timers[0], now);
        s->ifr |= T1_INT;
    }
    if (now >= s->timers[1].next_irq_time) {
        /*
         * T2 is one-shot (see mos6522_timer2()) -- unlike T1, do not
         * call mos6522_timer2_update() here, or this lazy catch-up
         * check re-arms T2 on every single register read past its
         * expiry, turning it back into a free-running periodic
         * interrupt exactly like the bug fixed in the timer callback.
         */
        s->ifr |= T2_INT;
    }
    switch (addr) {
    case VIA_REG_B:
        val = s->b;
        ctrl = (s->pcr & CB2_CTRL_MASK) >> CB2_CTRL_SHIFT;
        if (!(ctrl & C2_IND)) {
            s->ifr &= ~CB2_INT;
        }
        s->ifr &= ~CB1_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_A:
       qemu_log_mask(LOG_UNIMP, "Read access to register A with handshake");
       /* fall through */
    case VIA_REG_ANH:
        val = s->a;
        ctrl = (s->pcr & CA2_CTRL_MASK) >> CA2_CTRL_SHIFT;
        if (!(ctrl & C2_IND)) {
            s->ifr &= ~CA2_INT;
        }
        s->ifr &= ~CA1_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_DIRB:
        val = s->dirb;
        break;
    case VIA_REG_DIRA:
        val = s->dira;
        break;
    case VIA_REG_T1CL:
        val = get_counter(s, &s->timers[0]) & 0xff;
        s->ifr &= ~T1_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_T1CH:
        /*
         * Reading the counter is the last thing pe_run_clock_test()
         * does, so the measured loop is over: end any CPU-speed probe
         * window now rather than pacing whatever runs next.
         */
        calib_governor_end_cpu_probe();
        val = get_counter(s, &s->timers[0]) >> 8;
        mos6522_update_irq(s);
        break;
    case VIA_REG_T1LL:
        val = s->timers[0].latch & 0xff;
        break;
    case VIA_REG_T1LH:
        /* XXX: check this */
        val = (s->timers[0].latch >> 8) & 0xff;
        break;
    case VIA_REG_T2CL:
        val = get_counter(s, &s->timers[1]) & 0xff;
        s->ifr &= ~T2_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_T2CH:
        val = get_counter(s, &s->timers[1]) >> 8;
        break;
    case VIA_REG_SR:
        val = s->sr;
        s->ifr &= ~SR_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_ACR:
        val = s->acr;
        break;
    case VIA_REG_PCR:
        val = s->pcr;
        break;
    case VIA_REG_IFR:
        val = s->ifr;
        if (s->ifr & s->ier) {
            val |= 0x80;
        }
        break;
    case VIA_REG_IER:
        val = s->ier | 0x80;
        break;
    default:
        g_assert_not_reached();
    }

    trace_mos6522_read(addr, mos6522_reg_names[addr], val);

    return val;
}

void mos6522_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MOS6522State *s = opaque;
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);
    int ctrl;

    trace_mos6522_write(addr, mos6522_reg_names[addr], val);

    switch (addr) {
    case VIA_REG_B:
        s->b = (s->b & ~s->dirb) | (val & s->dirb);
        mdc->portB_write(s);
        ctrl = (s->pcr & CB2_CTRL_MASK) >> CB2_CTRL_SHIFT;
        if (!(ctrl & C2_IND)) {
            s->ifr &= ~CB2_INT;
        }
        s->ifr &= ~CB1_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_A:
       qemu_log_mask(LOG_UNIMP, "Write access to register A with handshake");
       /* fall through */
    case VIA_REG_ANH:
        s->a = (s->a & ~s->dira) | (val & s->dira);
        mdc->portA_write(s);
        ctrl = (s->pcr & CA2_CTRL_MASK) >> CA2_CTRL_SHIFT;
        if (!(ctrl & C2_IND)) {
            s->ifr &= ~CA2_INT;
        }
        s->ifr &= ~CA1_INT;
        mos6522_update_irq(s);
        break;
    case VIA_REG_DIRB:
        s->dirb = val;
        break;
    case VIA_REG_DIRA:
        s->dira = val;
        break;
    case VIA_REG_T1CL:
        s->timers[0].latch = (s->timers[0].latch & 0xff00) | val;
        mos6522_timer1_update(s, &s->timers[0],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        break;
    case VIA_REG_T1CH:
        s->timers[0].latch = (s->timers[0].latch & 0xff) | (val << 8);
        s->ifr &= ~T1_INT;
        set_counter(s, &s->timers[0], s->timers[0].latch);
        /*
         * Loading T1's counter with 0xffff -- the full range, so the
         * timer cannot expire during whatever comes next -- is the
         * signature of XNU's pe_run_clock_test(), which uses T1 purely
         * as a stopwatch across a fixed 10,000,000-CPU-clock loop and
         * derives the CPU PLL multiplier from how long that took. Under
         * TCG the loop finishes ~10x too fast; open a CPU-speed probe
         * window so it is paced to this board's real clock. See
         * system/calib-governor.c.
         */
        if (s->timers[0].latch == 0xffff) {
            trace_mos6522_t1_cpu_probe(s->acr,
                                       calib_governor_arm_cpu_probe());
        }
        break;
    case VIA_REG_T1LL:
        s->timers[0].latch = (s->timers[0].latch & 0xff00) | val;
        mos6522_timer1_update(s, &s->timers[0],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        break;
    case VIA_REG_T1LH:
        s->timers[0].latch = (s->timers[0].latch & 0xff) | (val << 8);
        s->ifr &= ~T1_INT;
        mos6522_timer1_update(s, &s->timers[0],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        break;
    case VIA_REG_T2CL:
        s->timers[1].latch = (s->timers[1].latch & 0xff00) | val;
        break;
    case VIA_REG_T2CH:
        /* To ensure T2 generates an interrupt on zero crossing with the
           common timer code, write the value directly from the latch to
           the counter */
        s->timers[1].latch = (s->timers[1].latch & 0xff) | (val << 8);
        s->ifr &= ~T2_INT;
        set_counter(s, &s->timers[1], s->timers[1].latch);
        /*
         * Writing T2C-H is the only way to arm T2, and in timed-interrupt
         * mode (ACR bit 5 clear) it is a one-shot: the guest has just
         * started a bounded wait it intends to notice the end of. Classic
         * Mac OS ROMs use exactly this to time their TimeDBRA family of
         * CPU-speed calibration spins, which are pure register loops that
         * a modern host runs to completion long before ~1ms of real time
         * has passed -- storing 0 and poisoning every later consumer that
         * divides by them. Open a calibration window so the vCPU is paced
         * to a period-plausible rate for the duration of this one-shot,
         * and only for that: see system/calib-governor.c.
         */
        if (!(s->acr & T2MODE_COUNT)) {
            int64_t countdown = s->timers[1].next_irq_time -
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            bool governed = calib_governor_arm(countdown,
                                               mos6522_t2_irq_catch_up, s);

            trace_mos6522_t2_oneshot(s->timers[1].latch, s->ier, s->acr,
                                     countdown, governed);
        }
        break;
    case VIA_REG_SR:
        s->sr = val;
        break;
    case VIA_REG_ACR:
        s->acr = val;
        mos6522_timer1_update(s, &s->timers[0],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        break;
    case VIA_REG_PCR:
        s->pcr = val;
        break;
    case VIA_REG_IFR:
        /* reset bits */
        s->ifr &= ~val;
        mos6522_update_irq(s);
        break;
    case VIA_REG_IER:
        if (val & IER_SET) {
            /* set bits */
            s->ier |= val & 0x7f;
        } else {
            /* reset bits */
            s->ier &= ~val;
        }
        mos6522_update_irq(s);
        /* if IER is modified starts needed timers */
        mos6522_timer1_update(s, &s->timers[0],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        mos6522_timer2_update(s, &s->timers[1],
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        break;
    default:
        g_assert_not_reached();
    }
}

static int qmp_x_query_via_foreach(Object *obj, void *opaque)
{
    GString *buf = opaque;

    if (object_dynamic_cast(obj, TYPE_MOS6522)) {
        MOS6522State *s = MOS6522(obj);
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint16_t t1counter = get_counter(s, &s->timers[0]);
        uint16_t t2counter = get_counter(s, &s->timers[1]);

        g_string_append_printf(buf, "%s:\n", object_get_typename(obj));

        g_string_append_printf(buf, "  Registers:\n");
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[0], s->b);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[1], s->a);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[2], s->dirb);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[3], s->dira);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[4], t1counter & 0xff);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[5], t1counter >> 8);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[6],
                               s->timers[0].latch & 0xff);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[7],
                               s->timers[0].latch >> 8);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[8], t2counter & 0xff);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[9], t2counter >> 8);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[10], s->sr);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[11], s->acr);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[12], s->pcr);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[13], s->ifr);
        g_string_append_printf(buf, "    %-*s:    0x%x\n", 4,
                               mos6522_reg_names[14], s->ier);

        g_string_append_printf(buf, "  Timers:\n");
        g_string_append_printf(buf, "    Using current time now(ns)=%"PRId64
                                    "\n", now);
        g_string_append_printf(buf, "    T1 freq(hz)=%"PRId64
                               " mode=%s"
                               " counter=0x%x"
                               " latch=0x%x\n"
                               "       load_time(ns)=%"PRId64
                               " next_irq_time(ns)=%"PRId64 "\n",
                               s->timers[0].frequency,
                               ((s->acr & T1MODE) == T1MODE_CONT) ? "continuous"
                                                                  : "one-shot",
                               t1counter,
                               s->timers[0].latch,
                               s->timers[0].load_time,
                               get_next_irq_time(s, &s->timers[0], now));
        g_string_append_printf(buf, "    T2 freq(hz)=%"PRId64
                               " mode=%s"
                               " counter=0x%x"
                               " latch=0x%x\n"
                               "       load_time(ns)=%"PRId64
                               " next_irq_time(ns)=%"PRId64 "\n",
                               s->timers[1].frequency,
                               "one-shot",
                               t2counter,
                               s->timers[1].latch,
                               s->timers[1].load_time,
                               get_next_irq_time(s, &s->timers[1], now));
    }

    return 0;
}

static HumanReadableText *qmp_x_query_via(Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");
    g_autofree char *govcfg = calib_governor_get_config();
    CalibGovStats gov;

    object_child_foreach_recursive(object_get_root(),
                                   qmp_x_query_via_foreach, buf);

    calib_governor_get_stats(&gov);
    g_string_append_printf(buf, "calibration-governor (%s):\n", govcfg);
    g_string_append_printf(buf, "    windows=%" PRIu64 " (cpu-probe %" PRIu64
                                ") re-arms=%" PRIu64
                                " capped=%" PRIu64 " ignored=%" PRIu64 "\n",
                           gov.windows, gov.cpu_probes, gov.rearms, gov.capped,
                           gov.ignored);
    g_string_append_printf(buf, "    ungoverned=%" PRIu64 " probes=%" PRIu64
                                "\n", gov.ungoverned, gov.probes);
    g_string_append_printf(buf, "    governed=%.3f ms slept=%.3f ms"
                                " insns=%" PRIu64 " irq-catch-ups=%" PRIu64
                                "\n",
                           gov.governed_ns / 1.0e6, gov.slept_ns / 1.0e6,
                           gov.insns, gov.irq_catch_ups);

    return human_readable_text_from_str(buf);
}

void hmp_info_via(Monitor *mon, const QDict *qdict)
{
    Error *err = NULL;
    g_autoptr(HumanReadableText) info = qmp_x_query_via(&err);

    if (hmp_handle_error(mon, err)) {
        return;
    }
    monitor_puts(mon, info->human_readable_text);
}

static const MemoryRegionOps mos6522_ops = {
    .read = mos6522_read,
    .write = mos6522_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static const VMStateDescription vmstate_mos6522_timer = {
    .name = "mos6522_timer",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(latch, MOS6522Timer),
        VMSTATE_UINT16(counter_value, MOS6522Timer),
        VMSTATE_INT64(load_time, MOS6522Timer),
        VMSTATE_INT64(next_irq_time, MOS6522Timer),
        VMSTATE_TIMER_PTR(timer, MOS6522Timer),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_mos6522 = {
    .name = "mos6522",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(a, MOS6522State),
        VMSTATE_UINT8(b, MOS6522State),
        VMSTATE_UINT8(dira, MOS6522State),
        VMSTATE_UINT8(dirb, MOS6522State),
        VMSTATE_UINT8(sr, MOS6522State),
        VMSTATE_UINT8(acr, MOS6522State),
        VMSTATE_UINT8(pcr, MOS6522State),
        VMSTATE_UINT8(ifr, MOS6522State),
        VMSTATE_UINT8(ier, MOS6522State),
        VMSTATE_UINT8(last_irq_levels, MOS6522State),
        VMSTATE_STRUCT_ARRAY(timers, MOS6522State, 2, 0,
                             vmstate_mos6522_timer, MOS6522Timer),
        VMSTATE_END_OF_LIST()
    }
};

static void mos6522_reset_hold(Object *obj, ResetType type)
{
    MOS6522State *s = MOS6522(obj);

    s->b = 0;
    s->a = 0;
    /*
     * A bare 6522 resets with port B as all-inputs, but 0xff here is
     * de-facto ABI for QEMU's shipped OpenBIOS: its CUDA driver
     * (drivers/cuda.c) starts bit-banging TIP/TACK on port B without
     * ever programming DIRB, so an all-inputs reset masks those writes
     * to nothing and the very first CUDA request hangs firmware boot
     * (found by bisect -- every -kernel/CD boot through OpenBIOS died).
     * Real Mac ROMs program DDRB themselves and don't care either way.
     */
    s->dirb = 0xff;
    s->dira = 0;
    s->sr = 0;
    s->acr = 0;
    s->pcr = 0;
    s->ifr = 0;
    s->ier = 0;
    /* s->ier = T1_INT | SR_INT; */

    s->timers[0].frequency = s->frequency;
    s->timers[0].latch = 0xffff;
    s->timers[0].last_read_d = -1;
    s->timers[0].last_read_real_ns = -1;
    set_counter(s, &s->timers[0], 0xffff);
    timer_del(s->timers[0].timer);

    s->timers[1].frequency = s->frequency;
    s->timers[1].latch = 0xffff;
    s->timers[1].last_read_d = -1;
    s->timers[1].last_read_real_ns = -1;
    timer_del(s->timers[1].timer);
}

static void mos6522_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MOS6522State *s = MOS6522(obj);
    int i;

    memory_region_init_io(&s->mem, obj, &mos6522_ops, s, "mos6522",
                          MOS6522_NUM_REGS);
    sysbus_init_mmio(sbd, &s->mem);
    sysbus_init_irq(sbd, &s->irq);

    for (i = 0; i < ARRAY_SIZE(s->timers); i++) {
        s->timers[i].index = i;
        s->timers[i].last_read_d = -1;
        s->timers[i].last_read_real_ns = -1;
    }

    s->timers[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mos6522_timer1, s);
    s->timers[1].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mos6522_timer2, s);

    qdev_init_gpio_in(DEVICE(obj), mos6522_set_irq, VIA_NUM_INTS);
}

static void mos6522_finalize(Object *obj)
{
    MOS6522State *s = MOS6522(obj);

    timer_free(s->timers[0].timer);
    timer_free(s->timers[1].timer);
}

static const Property mos6522_properties[] = {
    DEFINE_PROP_UINT64("frequency", MOS6522State, frequency, 0),
};

static void mos6522_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);

    rc->phases.hold = mos6522_reset_hold;
    dc->vmsd = &vmstate_mos6522;
    device_class_set_props(dc, mos6522_properties);
    mdc->portB_write = mos6522_portB_write;
    mdc->portA_write = mos6522_portA_write;
    mdc->get_timer1_counter_value = mos6522_get_counter_value;
    mdc->get_timer2_counter_value = mos6522_get_counter_value;
    mdc->get_timer1_load_time = mos6522_get_load_time;
    mdc->get_timer2_load_time = mos6522_get_load_time;
}

static const TypeInfo mos6522_type_info = {
    .name = TYPE_MOS6522,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MOS6522State),
    .instance_init = mos6522_init,
    .instance_finalize = mos6522_finalize,
    .abstract = true,
    .class_size = sizeof(MOS6522DeviceClass),
    .class_init = mos6522_class_init,
};

static void mos6522_register_types(void)
{
    type_register_static(&mos6522_type_info);
}

type_init(mos6522_register_types)
