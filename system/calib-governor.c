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
 * 3. A SPIN DETECTOR, because the arm gate alone is not selective
 *    enough. "T2 armed as a one-shot of 100 us to 2 ms" still admits
 *    around a thousand ordinary Time Manager delays over a 9.2 boot,
 *    and paced end to end those add up to a second or more of throttle
 *    -- a full-session governor again, just a thinner one. But a
 *    calibration spin does not merely arm a short timer, it *cycles a
 *    handful of tiny blocks and nothing else* for the whole window,
 *    while an ordinary delay runs ordinary varied code. So the window
 *    is opened optimistically and then continuously checked against
 *    that profile: over each CALIB_GOV_EPOCH_BLOCKS-block epoch, count
 *    the distinct block addresses executed. A window that does not look
 *    like a spin un-governs itself -- the pacing global is cleared,
 *    which is exactly the live one_insn_per_tb toggle in curr_cflags()
 *    (blocks are cflags-keyed, so the next exec-loop iteration simply
 *    looks up the ordinary chained copy; nothing has to be flushed) --
 *    but the window stays open, so a re-arm inside it starts looking
 *    again. A window does not open *on* its spin, it opens on the arm's
 *    own return path, so a contaminated sub-epoch first spends a
 *    re-sync (see CALIB_GOV_RESYNC_TRIES) rather than condemning the
 *    window.
 *
 * A guest that arms a one-shot timer permanently must not throttle
 * forever, so a single window is capped at CALIB_GOV_MAX_WINDOW_NS and
 * every window is counted. The counters are readable as QOM properties
 * on the machine and in "info via". A runtime warning fires if the
 * *share of wall time actually spent pacing* stays above
 * CALIB_GOV_WARN_PCT over a CALIB_GOV_WARN_SPAN_NS interval -- a rate,
 * not a total, because a total large enough to be alarming is reached
 * by any long enough session no matter how benign the pacing is.
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

/*
 * Define CALIB_GOV_PROFILE to rebuild the per-window measurement
 * instrumentation that the detector thresholds below were set from: it
 * prints one CGPROF line per window with the distribution of "distinct
 * blocks per 256-block epoch", and it makes the detector observe-only
 * so the pacing being measured is the undetected baseline.
 */
#undef CALIB_GOV_PROFILE

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

/*
 * Spin detector, measured on a 9.2 boot with the per-window profiling
 * above (doc/wingov-2026-08-31/REFINE-HANDOFF.md).
 *
 * The decision unit is a 256-block epoch and the statistic is the
 * number of DISTINCT block addresses executed in it. The measurement is
 * emphatic: the ROM's own calibration windows spend 97-98% of their
 * epochs executing 4 distinct blocks (the TimeDBRA spin -- the 68K
 * emulator's dbra fast path is a four-block cycle, not the single
 * self-branching block that was assumed) or 7-8 (the TimeSCCDB /
 * TimeSCSIDB spins, which touch MMIO each iteration), while 1700 of the
 * boot's 2230 windows contain not one single epoch under 9 distinct
 * blocks, and the median epoch of ordinary code overflows even a
 * 64-entry table -- roughly 200 distinct blocks per 256 executed.
 *
 * So 12 sits with about 50% headroom over the widest genuine spin and
 * an order of magnitude below ordinary code. Rejection is also cheap:
 * varied code fills the table within a few dozen blocks, long before
 * the epoch is up, and the window un-governs itself there and then.
 *
 * A "busiest block takes >= X% of the epoch" test was tried and
 * rejected: the seven-block sibling spin puts its top block at 1/7 of
 * the epoch, so any threshold loose enough to admit it admits ordinary
 * code too. Distinct-block count alone does the whole job.
 *
 * RE-SYNC, and why the recovery cannot be a timer. A window does not
 * begin with the spin: it begins with the arm's own return path and the
 * handful of 68K instructions that set the loop counter up, which under
 * the 68K emulator is a few dozen distinct PPC blocks. An epoch that
 * straddles that setup fails no matter how clean the spin after it is --
 * in the profiling boot the ROM's TimeDBRA window had 127 of its 129
 * epochs at exactly 4 distinct blocks and the other two at 17-32 and 48,
 * i.e. ~44 contaminating blocks concentrated at the edges. So a failed
 * sub-epoch RE-SYNCS -- wipe the table, keep pacing, try again -- up to
 * CALIB_GOV_RESYNC_TRIES times per arm, and the budget is restored by
 * every clean full epoch. Ordinary code fails each retry within ~13
 * blocks, so it still un-governs after ~120 blocks (~350 guest
 * instructions, well under one pacing quantum: a rejected window never
 * even reaches the accounting, let alone sleeps), while a genuine spin
 * walks past its setup without losing any of the window.
 *
 * It has to work this way because gov_probe_timer cannot be relied on to
 * do it. QEMU_CLOCK_REALTIME timers fire from the main loop, whose
 * resolution is qemu_poll_ns()'s -- and on any host without ppoll()
 * (macOS, where this is developed) that is g_poll() in whole
 * milliseconds, rounded up. A 50 us probe therefore lands ~1 ms late,
 * which is *after* the ~1.3 ms window it was meant to rescue. The probe
 * timer is kept because it is exact on hosts that do have ppoll() and
 * because it still covers long windows, but the detector must not depend
 * on it, and the arm path re-probes synchronously for the same reason.
 */
#define CALIB_GOV_EPOCH_BLOCKS  256
#define CALIB_GOV_PROBE_NS      (50 * 1000)
#define CALIB_GOV_DET_SLOTS     32      /* > SPIN_DISTINCT: overflow rejects */
#define CALIB_GOV_SPIN_DISTINCT 12
#define CALIB_GOV_RESYNC_TRIES  8

/*
 * Warn when the share of wall time spent pacing stays high, not when a
 * total is reached: over a long enough session any benign per-boot
 * residual crosses any fixed total. Two buckets give a rolling span of
 * between one and two times CALIB_GOV_WARN_BUCKET_NS.
 */
#define CALIB_GOV_WARN_BUCKET_NS (15LL * 1000 * 1000 * 1000)
#define CALIB_GOV_WARN_SPAN_NS   (2 * CALIB_GOV_WARN_BUCKET_NS)
#define CALIB_GOV_WARN_PCT       5

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
static QEMUTimer *gov_probe_timer;
static bool gov_window_active;          /* a window is open (maybe unpaced) */
static int64_t gov_window_open_ns;      /* host monotonic, window start */
static int64_t gov_window_deadline_ns;  /* host monotonic, window end */
static int64_t gov_paced_since_ns;      /* host monotonic, pacing armed at */
static bool gov_warned;
static unsigned gov_warn_pct = CALIB_GOV_WARN_PCT;

/* Rolling pacing-share buckets for the warning. */
static int64_t gov_warn_bucket_index = -1;
static uint64_t gov_warn_bucket_ns[2];

/*
 * Spin detector state. Written only by the vCPU thread inside
 * calib_governor_account(), i.e. only while pacing is armed; the probe
 * timer resets it before arming, which the qatomic_set() of
 * calib_gov_window_open publishes.
 */
typedef struct CalibGovDetSlot {
    uint64_t pc;
    uint32_t count;
    uint32_t icount;
} CalibGovDetSlot;

static CalibGovDetSlot gov_det[CALIB_GOV_DET_SLOTS];
static uint32_t gov_det_used;
static uint32_t gov_det_blocks;
static uint32_t gov_det_tries;          /* re-syncs left before giving up */
static bool gov_det_overflow;

static CalibGovStats gov_stats;

/* ------------------------------------------------------------------ */
/* TEMPORARY per-window execution profiling (step 1 measurement only). */
/* ------------------------------------------------------------------ */
#ifdef CALIB_GOV_PROFILE

/*
 * Profile each window the way the detector will actually see it: in
 * fixed-size epochs, through a small fixed table that overflows on
 * varied code. Per window we report the distribution of "distinct
 * blocks per epoch" and "busiest block's share of the epoch", which is
 * the statistic the detector thresholds are set from.
 */
#define CGP_EPOCH   256
#define CGP_SLOTS   64           /* profiling table: wider than the detector */
#define CGP_MAXEP   4096

typedef struct CgpSlot {
    uint64_t pc;
    uint32_t count;
    uint32_t icount;
} CgpSlot;

static CgpSlot cgp[CGP_SLOTS];
static uint32_t cgp_used;
static bool cgp_ovf;
static uint32_t cgp_ep_blocks;

/* per-epoch results for the current window */
static uint16_t cgp_ep_distinct[CGP_MAXEP];
static uint16_t cgp_ep_topcnt[CGP_MAXEP];
static uint16_t cgp_ep_topic[CGP_MAXEP];
static uint32_t cgp_neps;

/* whole-window */
static uint64_t cgp_blocks;
static uint64_t cgp_insns;
static int64_t cgp_countdown;
static int64_t cgp_cd_min;
static int64_t cgp_cd_max;
static uint32_t cgp_arms;

static void cgp_epoch_reset(void)
{
    memset(cgp, 0, sizeof(cgp));
    cgp_used = 0;
    cgp_ovf = false;
    cgp_ep_blocks = 0;
}

static void cgp_reset(int64_t countdown_ns)
{
    cgp_epoch_reset();
    cgp_neps = 0;
    cgp_blocks = 0;
    cgp_insns = 0;
    cgp_countdown = countdown_ns;
    cgp_cd_min = countdown_ns;
    cgp_cd_max = countdown_ns;
    cgp_arms = 1;
}

static void cgp_rearm(int64_t countdown_ns)
{
    cgp_arms++;
    if (countdown_ns < cgp_cd_min) {
        cgp_cd_min = countdown_ns;
    }
    if (countdown_ns > cgp_cd_max) {
        cgp_cd_max = countdown_ns;
    }
}

static void cgp_close_epoch(void)
{
    uint32_t i, top = 0, topic = 0;

    for (i = 0; i < CGP_SLOTS; i++) {
        if (cgp[i].count > top) {
            top = cgp[i].count;
            topic = cgp[i].icount;
        }
    }
    if (cgp_neps < CGP_MAXEP) {
        cgp_ep_distinct[cgp_neps] = cgp_ovf ? 0xffff : cgp_used;
        cgp_ep_topcnt[cgp_neps] = top;
        cgp_ep_topic[cgp_neps] = topic;
        cgp_neps++;
    }
    cgp_epoch_reset();
}

static void cgp_record(uint64_t pc, unsigned int n_insns)
{
    uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) % CGP_SLOTS;
    uint32_t i;

    cgp_blocks++;
    cgp_insns += n_insns;
    cgp_ep_blocks++;

    if (!cgp_ovf) {
        for (i = 0; i < CGP_SLOTS; i++) {
            CgpSlot *s = &cgp[(h + i) % CGP_SLOTS];

            if (s->count == 0) {
                s->pc = pc;
                s->count = 1;
                s->icount = n_insns;
                cgp_used++;
                break;
            }
            if (s->pc == pc) {
                s->count++;
                s->icount = n_insns;
                break;
            }
        }
        if (i == CGP_SLOTS) {
            cgp_ovf = true;
        }
    }

    if (cgp_ep_blocks >= CGP_EPOCH) {
        cgp_close_epoch();
    }
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static void cgp_dump(uint64_t window, int64_t governed_ns)
{
    g_autofree uint16_t *d = NULL;
    uint32_t i, n, pass4 = 0, pass8 = 0, pass16 = 0, pass32 = 0;
    uint32_t rule = 0;

    if (cgp_ep_blocks) {
        cgp_close_epoch();
    }
    n = cgp_neps;
    if (n == 0) {
        return;
    }
    d = g_new(uint16_t, n);
    for (i = 0; i < n; i++) {
        uint16_t dist = cgp_ep_distinct[i];

        d[i] = dist;
        if (dist <= 4) {
            pass4++;
        }
        if (dist <= 8) {
            pass8++;
        }
        if (dist <= 16) {
            pass16++;
        }
        if (dist <= 32) {
            pass32++;
        }
        /* candidate rule: few distinct blocks, one of them dominant, tiny */
        if (dist <= 12 && cgp_ep_topcnt[i] * 100 >= CGP_EPOCH * 15 &&
            cgp_ep_topic[i] <= 16) {
            rule++;
        }
    }
    qsort(d, n, sizeof(*d), cmp_u16);

    fprintf(stderr,
            "CGPROF win=%" PRIu64 " cd_us=%" PRId64 " cdmin_us=%" PRId64
            " cdmax_us=%" PRId64 " arms=%u gov_us=%" PRId64
            " blocks=%" PRIu64 " insns=%" PRIu64
            " eps=%u dmin=%u dp10=%u dmed=%u dp90=%u dmax=%u"
            " le4=%0.3f le8=%0.3f le16=%0.3f le32=%0.3f rule=%0.3f\n",
            window, cgp_countdown / 1000, cgp_cd_min / 1000,
            cgp_cd_max / 1000, cgp_arms, governed_ns / 1000,
            cgp_blocks, cgp_insns, n,
            d[0], d[n / 10], d[n / 2], d[(n * 9) / 10], d[n - 1],
            (double)pass4 / n, (double)pass8 / n, (double)pass16 / n,
            (double)pass32 / n, (double)rule / n);
    fflush(stderr);
}
#endif /* CALIB_GOV_PROFILE */

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

/*
 * Fold @paced_ns of pacing at @now into the rolling share buckets and
 * warn if the share over the rolling span is above the threshold. This
 * measures what the warning claims to measure: a *rate* of throttling.
 * The old absolute-total form fired on every long enough session --
 * the benign ~1.7 s per boot this governor's own evidence recorded --
 * and so said "full-session throttle" about its own normal behaviour.
 */
static void calib_governor_note_paced(int64_t now, int64_t paced_ns)
{
    int64_t bucket = now / CALIB_GOV_WARN_BUCKET_NS;
    uint64_t sum;

    if (bucket != gov_warn_bucket_index) {
        if (bucket == gov_warn_bucket_index + 1) {
            gov_warn_bucket_ns[1] = gov_warn_bucket_ns[0];
        } else {
            gov_warn_bucket_ns[1] = 0;
        }
        gov_warn_bucket_ns[0] = 0;
        gov_warn_bucket_index = bucket;
    }
    gov_warn_bucket_ns[0] += (uint64_t)paced_ns;

    if (gov_warned) {
        return;
    }
    sum = gov_warn_bucket_ns[0] + gov_warn_bucket_ns[1];
    if (sum * 100 > (uint64_t)CALIB_GOV_WARN_SPAN_NS * gov_warn_pct) {
        gov_warned = true;
        warn_report("calibration-governor: paced %" PRIu64 " ms of the last "
                    "%" PRId64 " s (over %u%%) across %" PRIu64 " windows -- "
                    "the guest is arming the VIA one-shot timer continuously "
                    "and this has become a general throttle rather than a "
                    "calibration window",
                    sum / 1000000, CALIB_GOV_WARN_SPAN_NS / 1000000000,
                    gov_warn_pct, qatomic_read(&gov_stats.windows));
    }
}

/*
 * Stop pacing. Clearing the global is all it takes: translation blocks
 * are keyed on cflags, so the next cpu_exec_loop() iteration looks up
 * the ordinary chained copy of whatever runs next, exactly as toggling
 * one_insn_per_tb does at runtime. Nothing is flushed and no vCPU needs
 * kicking -- the vCPU is between blocks by construction, since the
 * non-chaining cflags are what brought it back here.
 */
static void calib_governor_unpace(int64_t now)
{
    int64_t paced;

    if (!qatomic_xchg(&calib_gov_window_open, false)) {
        return;
    }

    paced = now - gov_paced_since_ns;
    if (paced < 0) {
        paced = 0;
    }
    qatomic_add(&gov_stats.governed_ns, (uint64_t)paced);
    calib_governor_note_paced(now, paced);
}

/* Arm (or re-arm) pacing for the open window, starting a fresh epoch. */
static void calib_governor_pace(int64_t now)
{
    int i;

    for (i = 0; i < CALIB_GOV_MAX_CPUS; i++) {
        gov_vcpu[i].quantum_insn = 0;
        gov_vcpu[i].window_insn = 0;
        gov_vcpu[i].window_start_ns = now;
    }
    memset(gov_det, 0, sizeof(gov_det));
    gov_det_used = 0;
    gov_det_blocks = 0;
    gov_det_tries = CALIB_GOV_RESYNC_TRIES;
    gov_det_overflow = false;
    gov_paced_since_ns = now;

    qatomic_set(&calib_gov_window_open, true);

    /*
     * Break any translation-block chain that is already running, so the
     * vCPU returns to the execution loop and picks up the non-chaining
     * cflags. Without this the self-chained `dbra d0,self` block would
     * simply never come back and the window would do nothing at all.
     */
    calib_governor_kick_all();
}

static void calib_governor_close(void)
{
    int64_t now = now_ns();

    calib_governor_unpace(now);
    if (!gov_window_active) {
        return;
    }
    gov_window_active = false;
    if (gov_probe_timer) {
        timer_del(gov_probe_timer);
    }

#ifdef CALIB_GOV_PROFILE
    cgp_dump(qatomic_read(&gov_stats.windows), now - gov_window_open_ns);
#endif
}

static void calib_governor_close_timer_cb(void *opaque)
{
    calib_governor_close();
}

/*
 * Re-probe: the window is still open but the last epoch did not look
 * like a spin. Turn pacing back on for one more epoch so that a spin
 * starting part-way through the window is still caught.
 */
static void calib_governor_probe_timer_cb(void *opaque)
{
    int64_t now = now_ns();

    if (!gov_window_active || qatomic_read(&calib_gov_window_open)) {
        return;
    }
    if (now >= gov_window_deadline_ns) {
        calib_governor_close();
        return;
    }
    qatomic_inc(&gov_stats.probes);
    calib_governor_pace(now);
}

bool calib_governor_arm(int64_t countdown_ns)
{
    int64_t now, deadline;

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

    if (gov_window_active) {
        /*
         * Already inside a window: extend it rather than counting a new
         * one, but never past the hard cap measured from the ORIGINAL
         * open, so back-to-back re-arming cannot ratchet a window open
         * indefinitely.
         */
        int64_t hard = gov_window_open_ns + CALIB_GOV_MAX_WINDOW_NS;

        qatomic_inc(&gov_stats.rearms);
#ifdef CALIB_GOV_PROFILE
        cgp_rearm(countdown_ns);
#endif
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
        /*
         * A fresh arm is fresh evidence: re-probe at once rather than
         * waiting out the probe interval, so the ROM's own arm-then-spin
         * sequence is paced from its very first block even if the
         * preceding window had already un-governed itself.
         */
        if (!qatomic_read(&calib_gov_window_open)) {
            qatomic_inc(&gov_stats.probes);
            calib_governor_pace(now);
        }
        return true;
    }

    gov_window_active = true;
    gov_window_open_ns = now;
    gov_window_deadline_ns = deadline;
    qatomic_inc(&gov_stats.windows);

#ifdef CALIB_GOV_PROFILE
    cgp_reset(countdown_ns);
#endif

    if (!gov_close_timer) {
        gov_close_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                       calib_governor_close_timer_cb, NULL);
        gov_probe_timer = timer_new_ns(QEMU_CLOCK_REALTIME,
                                       calib_governor_probe_timer_cb, NULL);
    }
    timer_mod_ns(gov_close_timer,
                 qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + (deadline - now));

    /*
     * Open optimistically: a genuine calibration spin starts on the
     * window's very first block, so pacing has to be on before the guest
     * runs anything. The detector in calib_governor_account() turns it
     * straight back off if what actually executes is ordinary code.
     */
    calib_governor_pace(now);
    return true;
}

/*
 * Spin detector. Fold one executed block into the current epoch and,
 * once the epoch is complete, say whether what ran looks like the ROM's
 * dbra-to-self calibration spin: a tiny block, taken essentially
 * exclusively, with almost no other block addresses beside it.
 *
 * Returns false only once the re-sync budget for this arm is spent. The
 * table doubles as the distinct-block counter: exceeding the bound is
 * already a verdict on the current sub-epoch, so ordinary code is judged
 * within a few blocks rather than after a full epoch, and a sub-epoch
 * contaminated by the window's pre-spin setup costs a re-sync rather
 * than the whole window.
 */
static bool calib_governor_looks_like_spin(uint64_t pc, unsigned int n_insns)
{
    uint32_t h = (uint32_t)((pc >> 2) * 2654435761u) % CALIB_GOV_DET_SLOTS;
    uint32_t i;

    gov_det_blocks++;

    if (!gov_det_overflow) {
        for (i = 0; i < CALIB_GOV_DET_SLOTS; i++) {
            CalibGovDetSlot *s = &gov_det[(h + i) % CALIB_GOV_DET_SLOTS];

            if (s->count == 0) {
                s->pc = pc;
                s->count = 1;
                s->icount = n_insns;
                gov_det_used++;
                break;
            }
            if (s->pc == pc) {
                s->count++;
                s->icount = n_insns;
                break;
            }
        }
        if (i == CALIB_GOV_DET_SLOTS) {
            gov_det_overflow = true;
        }
    }

    /*
     * Too many distinct blocks: this sub-epoch is not a spin. Spend a
     * re-sync and look again from here -- the contamination may just be
     * the setup code this window opened on, with the spin behind it.
     * Only when the budget is gone is the window itself given up.
     */
    if (gov_det_overflow || gov_det_used > CALIB_GOV_SPIN_DISTINCT) {
        if (gov_det_tries == 0) {
            return false;
        }
        gov_det_tries--;
        goto resync;
    }
    if (gov_det_blocks < CALIB_GOV_EPOCH_BLOCKS) {
        return true;                    /* not enough evidence yet */
    }

    /*
     * A full epoch inside the bound: this really is a spin, so restore
     * the re-sync budget -- a later interrupt or the loop's exit path may
     * contaminate an epoch without meaning the window has stopped being
     * a calibration -- and start a fresh epoch.
     */
    gov_det_tries = CALIB_GOV_RESYNC_TRIES;

resync:
    memset(gov_det, 0, sizeof(gov_det));
    gov_det_used = 0;
    gov_det_blocks = 0;
    gov_det_overflow = false;
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

void calib_governor_account(CPUState *cpu, uint64_t pc, unsigned int n_insns)
{
    CalibGovVCPU *v;
    int64_t now, due_ns, elapsed_ns;
    unsigned index = cpu->cpu_index;

    if (unlikely(index >= CALIB_GOV_MAX_CPUS)) {
        return;
    }
    v = &gov_vcpu[index];

#ifdef CALIB_GOV_PROFILE
    cgp_record(pc, n_insns);
#endif

    if (unlikely(!calib_governor_looks_like_spin(pc, n_insns))) {
#ifdef CALIB_GOV_PROFILE
        /*
         * Measurement build: observe the detector's verdict but do not
         * act on it, so pacing behaviour stays identical to the
         * un-detected baseline being measured against.
         */
        memset(gov_det, 0, sizeof(gov_det));
        gov_det_used = 0;
        gov_det_blocks = 0;
        gov_det_tries = CALIB_GOV_RESYNC_TRIES;
        gov_det_overflow = false;
        qatomic_inc(&gov_stats.ungoverned);
#else
        /*
         * Ordinary code inside the window. Stop pacing it right now; the
         * window stays open and the probe timer will look again shortly,
         * so a spin that starts later still gets caught.
         */
        qatomic_inc(&gov_stats.ungoverned);
        calib_governor_unpace(now_ns());
        if (gov_probe_timer) {
            timer_mod_ns(gov_probe_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                         CALIB_GOV_PROBE_NS);
        }
        return;
#endif
    }

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
    out->ungoverned = qatomic_read(&gov_stats.ungoverned);
    out->probes = qatomic_read(&gov_stats.probes);
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
    if (!g_strcmp0(value, "on")) {
        gov_explicit = true;
        gov_enabled = true;
    } else if (!g_strcmp0(value, "off")) {
        gov_explicit = true;
        gov_enabled = false;
        calib_governor_close();
    } else if (g_str_has_prefix(value, "mips=")) {
        gov_explicit = true;
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
    } else if (g_str_has_prefix(value, "warn-pct=")) {
        /*
         * Tuning knob for the "this has become a general throttle"
         * warning, and the control that proves the warning can still
         * fire: warn-pct=0 makes any pacing at all trip it.
         */
        const char *arg = value + strlen("warn-pct=");
        uint64_t pct;

        if (qemu_strtou64(arg, NULL, 10, &pct) < 0 || pct > 100) {
            error_setg(errp, "calibration-governor: bad warn-pct value '%s'",
                       arg);
            return false;
        }
        gov_warn_pct = pct;
        gov_warned = false;
    } else {
        error_setg(errp, "calibration-governor: expected 'on', 'off', "
                         "'mips=<n>' or 'warn-pct=<n>', got '%s'", value);
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
