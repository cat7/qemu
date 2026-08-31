/*
 * Windowed calibration governor.
 *
 * Classic Mac OS ROMs calibrate the TimeDBRA family of low-memory
 * globals by arming VIA Timer 2 as a ~1 ms one-shot (780 ticks at
 * CUDA_TIMER_FREQ) and then spinning `dbra d0,self` with d0 = 0xFFFF,
 * expecting the T2 interrupt to break the loop part-way through. That
 * spin is a *pure register* loop -- it touches no device on any
 * iteration -- so under TCG it runs at host speed and completes all
 * 65536 iterations before the timer expires. The loop then falls
 * through and the calibrator stores not.w(0xFFFF) == 0 into TimeDBRA
 * (low memory 0x0D00), poisoning every later consumer that divides by
 * it (the ATAPI driver's own recalibration, Open Transport) with a
 * "divide by zero" system error. The sibling globals (TimeSCCDB,
 * TimeSCSIDB) survive only because their loops touch emulated MMIO
 * every iteration and are slow by accident.
 *
 * contrib/plugins/governor.c fixes this by capping the *whole session*
 * to a period-plausible MIPS figure against the host monotonic clock.
 * That works, but it is a global tax: the guest runs slowly forever to
 * protect a few milliseconds of ROM code.
 *
 * This is the same pacing, applied only while a calibration window is
 * open. The window is detected by the device itself -- mos6522.c calls
 * calib_governor_arm() when the guest arms T2 as a one-shot -- and runs
 * until the timer's expiry plus a short guard tail. Everywhere else the
 * guest runs completely unthrottled.
 *
 * Two things are needed to make that work, and both are load-bearing:
 *
 * 1. THE PACING ACCOUNTING, ported faithfully from the plugin. Its
 *    three constants were each established empirically there and are
 *    reproduced here for the same reasons:
 *      - a fine settle quantum (MAX_QUANTUM_INSNS, ~8 kHz), because a
 *        coarse quantum lets the vCPU burst at full host speed through
 *        the entire ~1 ms window and the cap never engages;
 *      - a banked-credit cap (MAX_CREDIT_NS), because otherwise an idle
 *        or MMIO-heavy stretch banks enough budget that the whole
 *        calibration burst runs on credit (a tenfold effective-rate
 *        overshoot was measured without it);
 *      - the host MONOTONIC clock, not a wall clock.
 *    Here the credit question is largely moot because each window
 *    starts with the accumulators reset, but the cap is kept so that a
 *    long or repeatedly-extended window behaves like the plugin.
 *
 * 2. FORCED TRANSLATION-BLOCK EXITS. `dbra d0,self` compiles to a tiny
 *    translation block that chains to *itself* via goto_tb, so under
 *    normal TCG it never returns to the execution loop at all and no
 *    per-block hook of ours would ever run. (The plugin escapes this
 *    only because plugin instrumentation is generated *inside* the
 *    chained block.) So while a window is open, curr_cflags() adds
 *    CF_NO_GOTO_TB | CF_NO_GOTO_PTR -- CF_NO_GOTO_TB alone is not
 *    enough, since goto_ptr would still chain through the jump cache
 *    and bypass the loop -- and arming the window kicks every vCPU with
 *    cpu_exit() so that a block chain already running is broken out of
 *    immediately (the exit-request test that gen_tb_start() emits at
 *    the top of every block catches the chained jump). Blocks are
 *    keyed on cflags, so the non-chaining copies are translated once
 *    and reused by every later window; when the window closes,
 *    curr_cflags() goes back to its normal value and the guest resumes
 *    executing the ordinary chained copies.
 *
 * A guest that arms a one-shot timer permanently must not throttle
 * forever, so a single window is capped at CALIB_GOV_MAX_WINDOW_NS and
 * every window is counted. The counters are readable as QOM properties
 * on the machine and in "info via"; if the governed total is more than
 * a few milliseconds across a boot, something is arming T2 constantly
 * and this has quietly become a full-session governor.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "system/calib-governor.h"

bool calib_gov_window_open;

#define NSEC_IN_ONE_SEC (1000LL * 1000 * 1000)

/*
 * Rate-check granularity, and the hard bound on a single burst. See the
 * file comment: both matter, and MAX_QUANTUM_INSNS is what actually
 * bounds the burst at low target rates.
 */
#define NUM_TIME_UPDATE_PER_SEC 8000
#define MAX_QUANTUM_INSNS       2000

#ifdef _WIN32
/*
 * Windows sleeps in whole milliseconds (g_usleep() is Sleep() rounded
 * up), so settling a few-microsecond debt with a sleep would
 * over-throttle by ~10x. Let sub-millisecond debt ride -- the window
 * accounting is cumulative so it stays owed -- and widen the credit cap
 * to one millisecond so Sleep()'s own overshoot is banked rather than
 * lost. Same reasoning as contrib/plugins/governor.c.
 */
#define SLEEP_GRANULARITY_NS (1000 * 1000)
#define MAX_CREDIT_NS        (1000 * 1000)
#else
#define MAX_CREDIT_NS        (100 * 1000)
#endif

/*
 * Guard tail: keep pacing this long past the timer's expiry, so the
 * interrupt delivery and the handful of instructions the calibrator
 * runs immediately after the loop breaks are still covered.
 */
#define CALIB_GOV_GUARD_NS (300 * 1000)

/*
 * Longest one-shot that can plausibly be a CPU-speed calibration.
 *
 * "T2 armed as a one-shot" on its own is far too broad a signal, and
 * this was measured, not assumed: over a 9.2 boot the guest arms T2 in
 * timed mode roughly 500 times a second -- the Time Manager's ordinary
 * scheduled delays -- almost all of them with counts around 7600 ticks
 * (~9.7 ms). Governing all of those covered 64% of the whole session:
 * a full-session governor wearing a window's clothes.
 *
 * A calibration spin is necessarily short. The ROM's SetUpTimeK arms
 * 780 ticks (~995 us) and counts a 16-bit dbra down inside it: the
 * whole point is that the loop must not run out before the timer does
 * at any period-plausible CPU speed, which bounds the window at the
 * millisecond scale. So accept only one-shots up to a couple of
 * milliseconds -- twice the ROM's own figure, and five times below the
 * Time Manager traffic that dominates the rest of the boot.
 */
#define CALIB_GOV_MAX_ARM_NS (2 * 1000 * 1000)

/*
 * ...and the shortest. Measured over the same boot: about 1700 of the
 * accepted one-shots were 4-23 ticks (5-30 us), the CUDA/ADB driver's
 * hardware handshake timeouts, and they accounted for 40% of the
 * governed time on their own. Nothing that short can be the loop this
 * exists for: a 65536-iteration dbra spin costs on the order of a
 * quarter of a million host-emulated instructions, so it needs hundreds
 * of microseconds even on a host far faster than any this will run on.
 * A window shorter than a couple of pacing quanta could not be paced
 * meaningfully anyway.
 */
#define CALIB_GOV_MIN_ARM_NS (100 * 1000)

/*
 * Hard cap on one window. The real windows are ~1 ms; this only exists
 * so that a guest which re-arms back-to-back forever cannot turn this
 * into a permanent global throttle without it showing up in the
 * "capped" counter.
 */
#define CALIB_GOV_MAX_WINDOW_NS (10 * 1000 * 1000)

/* Warn once if the governed total gets implausibly large. */
#define CALIB_GOV_WARN_TOTAL_NS (1000LL * 1000 * 1000)

#define CALIB_GOV_MAX_CPUS 16

typedef struct CalibGovVCPU {
    uint64_t quantum_insn;
    uint64_t window_insn;
    int64_t window_start_ns;
} CalibGovVCPU;

static CalibGovVCPU gov_vcpu[CALIB_GOV_MAX_CPUS];

/*
 * Configuration. The T2 hook in mos6522.c is generic, but only boards
 * that ask for it get the governor: it stays off until a machine calls
 * calib_governor_default_on() (g3beige does, from its own init), and an
 * explicit "calibration-governor=..." always wins over that default.
 */
static bool gov_enabled;
static bool gov_explicit;
static uint64_t gov_insn_per_second = 100 * 1000 * 1000;
static uint64_t gov_insn_per_quantum = MAX_QUANTUM_INSNS;

/*
 * Window state. gov_window_open_ns/gov_window_deadline_ns are written
 * only by calib_governor_arm() (on a vCPU thread, from an MMIO write,
 * with the BQL held) and are published by the release implied by the
 * qatomic_set() of calib_gov_window_open that follows them; readers only
 * ever look at them while that flag is set.
 */
static QEMUTimer *gov_close_timer;
static int64_t gov_window_open_ns;      /* host monotonic, window start */
static int64_t gov_window_deadline_ns;  /* host monotonic, window end */
static bool gov_warned;

static CalibGovStats gov_stats;

static int64_t now_ns(void)
{
    /*
     * Monotonic: clock_gettime(CLOCK_MONOTONIC)/mach_absolute_time on
     * POSIX, QueryPerformanceCounter on Windows. A wall clock can step,
     * and on Windows its granularity is the system tick -- useless for
     * pacing quanta due in a few microseconds.
     */
    return g_get_monotonic_time() * 1000;
}

static void calib_governor_kick_all(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_exit(cpu);
    }
}

static void calib_governor_close(void)
{
    int64_t governed;

    if (!qatomic_xchg(&calib_gov_window_open, false)) {
        return;
    }

    governed = now_ns() - gov_window_open_ns;
    if (governed < 0) {
        governed = 0;
    }
    qatomic_add(&gov_stats.governed_ns, (uint64_t)governed);

    if (!gov_warned &&
        qatomic_read(&gov_stats.governed_ns) > CALIB_GOV_WARN_TOTAL_NS) {
        gov_warned = true;
        warn_report("calibration-governor: more than 1s of governed time "
                    "across %" PRIu64 " windows -- the guest is arming the "
                    "VIA one-shot timer continuously and this has become a "
                    "full-session throttle",
                    qatomic_read(&gov_stats.windows));
    }
}

static void calib_governor_close_timer_cb(void *opaque)
{
    calib_governor_close();
}

bool calib_governor_arm(int64_t countdown_ns)
{
    int64_t now, deadline;
    int i;

    if (!gov_enabled) {
        return false;
    }
    if (countdown_ns < 0) {
        countdown_ns = 0;
    }
    /*
     * Too long to be a calibration spin: this is an ordinary scheduled
     * delay, and governing it would quietly throttle the session. Note
     * the comparison also disposes of the INT64_MAX ("never") that a
     * stopped timer produces, before any arithmetic on it.
     */
    if (countdown_ns > CALIB_GOV_MAX_ARM_NS ||
        countdown_ns < CALIB_GOV_MIN_ARM_NS) {
        qatomic_inc(&gov_stats.ignored);
        return false;
    }

    now = now_ns();
    deadline = now + countdown_ns + CALIB_GOV_GUARD_NS;

    if (qatomic_read(&calib_gov_window_open)) {
        /*
         * Already inside a window: extend it rather than counting a new
         * one, but never past the hard cap measured from the ORIGINAL
         * open, so back-to-back re-arming cannot ratchet a window open
         * indefinitely.
         */
        int64_t hard = gov_window_open_ns + CALIB_GOV_MAX_WINDOW_NS;

        qatomic_inc(&gov_stats.rearms);
        if (deadline > hard) {
            deadline = hard;
            qatomic_inc(&gov_stats.capped);
        }
        if (deadline > gov_window_deadline_ns) {
            gov_window_deadline_ns = deadline;
            timer_mod_ns(gov_close_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                         (deadline - now));
        }
        return true;
    }

    /*
     * Start every window with the pacing accumulators cleared: no
     * credit banked from before the window can be spent inside it.
     */
    for (i = 0; i < CALIB_GOV_MAX_CPUS; i++) {
        gov_vcpu[i].quantum_insn = 0;
        gov_vcpu[i].window_insn = 0;
        gov_vcpu[i].window_start_ns = now;
    }

    gov_window_open_ns = now;
    gov_window_deadline_ns = deadline;
    qatomic_inc(&gov_stats.windows);

    if (!gov_close_timer) {
        gov_close_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                       calib_governor_close_timer_cb, NULL);
    }
    timer_mod_ns(gov_close_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + (deadline - now));

    qatomic_set(&calib_gov_window_open, true);

    /*
     * Break any translation-block chain that is already running, so the
     * vCPU returns to the execution loop and picks up the non-chaining
     * cflags. Without this the self-chained `dbra d0,self` block would
     * simply never come back and the window would do nothing at all.
     */
    calib_governor_kick_all();
    return true;
}

/* Sleep off a debt; returns false if the debt is too small to sleep on. */
static bool throttle_sleep(int64_t debt_ns)
{
#ifdef _WIN32
    if (debt_ns < SLEEP_GRANULARITY_NS) {
        return false;
    }
    /* whole milliseconds only: g_usleep(N * 1000) is exactly Sleep(N) */
    g_usleep((debt_ns / SLEEP_GRANULARITY_NS) * 1000);
#else
    g_usleep(debt_ns / 1000);
#endif
    return true;
}

void calib_governor_account(CPUState *cpu, unsigned int n_insns)
{
    CalibGovVCPU *v;
    int64_t now, due_ns, elapsed_ns;
    unsigned index = cpu->cpu_index;

    if (unlikely(index >= CALIB_GOV_MAX_CPUS)) {
        return;
    }
    v = &gov_vcpu[index];

    v->quantum_insn += n_insns;
    if (v->quantum_insn < gov_insn_per_quantum) {
        return;
    }

    now = now_ns();
    v->window_insn += v->quantum_insn;
    qatomic_add(&gov_stats.insns, v->quantum_insn);
    v->quantum_insn = 0;

    /* how long SHOULD this window's instructions have taken? */
    due_ns = (int64_t)((double)v->window_insn /
                       (double)gov_insn_per_second *
                       (double)NSEC_IN_ONE_SEC);
    elapsed_ns = now - v->window_start_ns;

    if (due_ns > elapsed_ns) {
        if (throttle_sleep(due_ns - elapsed_ns)) {
            int64_t after = now_ns();
            qatomic_add(&gov_stats.slept_ns, (uint64_t)(after - now));
            now = after;
        }
    } else if (elapsed_ns - due_ns > MAX_CREDIT_NS) {
        /*
         * Running below target: forgive the excess beyond the cap
         * instead of banking it as burst budget.
         */
        v->window_start_ns = now - due_ns - MAX_CREDIT_NS;
    }

    /* bound the accounting window so the accumulators can't drift */
    if (now - v->window_start_ns > NSEC_IN_ONE_SEC / 4) {
        v->window_start_ns = now;
        v->window_insn = 0;
    }

    /*
     * Close the window as soon as the vCPU itself notices the deadline
     * has passed; gov_close_timer is the backstop for a guest that
     * stops executing before then.
     */
    if (now >= gov_window_deadline_ns) {
        calib_governor_close();
    }
}

void calib_governor_get_stats(CalibGovStats *out)
{
    out->windows = qatomic_read(&gov_stats.windows);
    out->rearms = qatomic_read(&gov_stats.rearms);
    out->capped = qatomic_read(&gov_stats.capped);
    out->ignored = qatomic_read(&gov_stats.ignored);
    out->governed_ns = qatomic_read(&gov_stats.governed_ns);
    out->insns = qatomic_read(&gov_stats.insns);
    out->slept_ns = qatomic_read(&gov_stats.slept_ns);
}

static void calib_governor_recompute(void)
{
    gov_insn_per_quantum = gov_insn_per_second / NUM_TIME_UPDATE_PER_SEC;
    if (gov_insn_per_quantum > MAX_QUANTUM_INSNS) {
        gov_insn_per_quantum = MAX_QUANTUM_INSNS;
    }
    if (gov_insn_per_quantum < 1) {
        gov_insn_per_quantum = 1;
    }
}

void calib_governor_default_on(void)
{
    if (!gov_explicit) {
        gov_enabled = true;
    }
}

bool calib_governor_configure(const char *value, Error **errp)
{
    gov_explicit = true;
    if (!g_strcmp0(value, "on")) {
        gov_enabled = true;
    } else if (!g_strcmp0(value, "off")) {
        gov_enabled = false;
        calib_governor_close();
    } else if (g_str_has_prefix(value, "mips=")) {
        const char *arg = value + strlen("mips=");
        uint64_t mips;

        if (qemu_strtou64(arg, NULL, 10, &mips) < 0 || mips == 0 ||
            mips > 100000) {
            error_setg(errp, "calibration-governor: bad mips value '%s'", arg);
            return false;
        }
        gov_enabled = true;
        gov_insn_per_second = mips * 1000 * 1000;
        calib_governor_recompute();
    } else {
        error_setg(errp, "calibration-governor: expected 'on', 'off' or "
                         "'mips=<n>', got '%s'", value);
        return false;
    }
    return true;
}

char *calib_governor_get_config(void)
{
    if (!gov_enabled) {
        return g_strdup("off");
    }
    return g_strdup_printf("mips=%" PRIu64, gov_insn_per_second / 1000000);
}
