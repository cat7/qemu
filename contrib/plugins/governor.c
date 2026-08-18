/*
 * governor: cap guest execution speed to a target MIPS without touching
 * the virtual clock.
 *
 * Unlike contrib/plugins/ips.c (which drives QEMU_CLOCK_VIRTUAL from
 * the instruction count via the plugin time-control API), this plugin
 * leaves time entirely alone and simply sleeps the vCPU whenever it
 * runs ahead of the target rate measured against the host's real
 * clock. Device models, timers, and the boot path all behave exactly
 * as at native speed -- the guest just executes fewer instructions per
 * real millisecond.
 *
 * Why this exists: classic Mac OS ROMs calibrate the TimeDBRA family
 * of low-memory globals by counting dbra-loop iterations (counter
 * seeded with 0xFFFF) inside a ~1 ms VIA T2 one-shot window. If the
 * CPU completes all 65536 iterations before the timer fires, the loop
 * falls through and the calibration stores not.w(0xFFFF) == 0 --
 * poisoning every later delay computation and crashing code that
 * divides by these globals ("divide by zero" system errors in the
 * ATAPI driver and Open Transport). Real hardware could never run the
 * loop that fast; capping the emulated CPU to a period-plausible MIPS
 * restores the invariant. ~65536 iterations/ms is the breaking point;
 * a few hundred MIPS leaves a comfortable margin while still being far
 * faster than any real Beige G3.
 *
 * Plugin arg: mips=<millions of instructions per second> (default 250).
 */
#include <stdio.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NSEC_IN_ONE_SEC (1000LL * 1000 * 1000)
/*
 * Rate-check granularity: sleep debts settle every quantum. This MUST
 * be much finer than the ~1 ms calibration windows we exist to protect
 * -- with a coarse quantum the vCPU bursts at full host speed through
 * the whole window and the cap never engages (verified: a 12500-insn
 * quantum at mips=100 still produced TimeDBRA 0/65535 extremes, while
 * fine quanta produce properly calibrated mid values). MAX_QUANTUM
 * bounds the burst directly; host-usleep overshoot is self-correcting
 * because the due-vs-elapsed accounting is cumulative over the window.
 */
#define NUM_TIME_UPDATE_PER_SEC 8000
#define MAX_QUANTUM_INSNS 2000

static uint64_t max_insn_per_second = 250 * 1000 * 1000;
static uint64_t max_insn_per_quantum;

struct qemu_plugin_scoreboard *vcpus;

typedef struct {
    uint64_t quantum_insn;
    int64_t window_start_ns;
    uint64_t window_insn;
} vCPUGov;

static int64_t now_ns(void)
{
    /*
     * Monotonic: clock_gettime(CLOCK_MONOTONIC) / mach_absolute_time on
     * POSIX hosts, QueryPerformanceCounter on Windows. g_get_real_time()
     * would be GetSystemTimeAsFileTime there, whose granularity is the
     * system timer tick (~1 ms at best) -- useless for pacing quanta that
     * are due in a few microseconds -- and a wall clock can step anyway.
     */
    return g_get_monotonic_time() * 1000;
}

/*
 * Maximum "credit" a vCPU may bank while running below target. Without
 * this cap, any idle or MMIO-heavy stretch banks enough budget that a
 * subsequent pure-compute burst (exactly the dbra calibration loop we
 * exist to constrain -- it only needs ~1-3 ms of unthrottled execution)
 * runs entirely un-throttled on credit. Verified empirically: with a
 * 0.25 s-granularity window and no cap, the effective rate inside the
 * calibration loop exceeded the target more than tenfold.
 */
#ifdef _WIN32
/*
 * Windows sleeps in whole milliseconds: glib's g_usleep() is Sleep()
 * rounded UP to the next millisecond, and even Sleep(1) (QEMU already
 * calls timeBeginPeriod(1)) returns 1-2 ms later. Quantum debts here
 * are a few microseconds, so settling each one with a sleep would
 * over-throttle by ~10x -- the overshoot beyond MAX_CREDIT_NS is
 * forgiven, not banked. Instead: let sub-millisecond debt ride (the
 * window accounting is cumulative, so it stays owed), sleep only the
 * whole milliseconds Sleep() can honour, and widen the credit cap to
 * one millisecond so Sleep()'s own overshoot is banked rather than
 * lost. Worst-case burst is then ~1 ms of debt plus ~1 ms of credit of
 * target-time -- ~400k instructions at 200 MIPS, still far short of the
 * 65536-iteration emulated-68K dbra loop this plugin exists to slow
 * down (each 68K iteration costs on the order of ten-plus PPC
 * instructions in the emulator).
 */
#define SLEEP_GRANULARITY_NS (1000 * 1000)
#define MAX_CREDIT_NS        (1000 * 1000)
#else
#define MAX_CREDIT_NS        (100 * 1000)
#endif

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

static void settle_quantum(vCPUGov *vcpu)
{
    int64_t now = now_ns();

    vcpu->window_insn += vcpu->quantum_insn;
    vcpu->quantum_insn = 0;

    /* how long SHOULD this window's instructions have taken? */
    int64_t due_ns = (int64_t)((double)vcpu->window_insn /
                               (double)max_insn_per_second *
                               (double)NSEC_IN_ONE_SEC);
    int64_t elapsed_ns = now - vcpu->window_start_ns;

    if (due_ns > elapsed_ns) {
        if (throttle_sleep(due_ns - elapsed_ns)) {
            now = now_ns();
        }
    } else if (elapsed_ns - due_ns > MAX_CREDIT_NS) {
        /* running below target (idle / MMIO-heavy): forgive the excess
         * beyond the cap instead of banking it as burst budget */
        vcpu->window_start_ns = now - due_ns - MAX_CREDIT_NS;
    }

    /* bound the window so the accumulators can't drift or overflow */
    if (now - vcpu->window_start_ns > NSEC_IN_ONE_SEC / 4) {
        vcpu->window_start_ns = now;
        vcpu->window_insn = 0;
    }
}

static void vcpu_init(unsigned int cpu_index, void *userdata)
{
    vCPUGov *vcpu = qemu_plugin_scoreboard_find(vcpus, cpu_index);
    vcpu->quantum_insn = 0;
    vcpu->window_insn = 0;
    vcpu->window_start_ns = now_ns();
}

static void every_quantum_insn(unsigned int cpu_index, void *udata)
{
    vCPUGov *vcpu = qemu_plugin_scoreboard_find(vcpus, cpu_index);
    settle_quantum(vcpu);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n_insns = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_u64 quantum_insn =
        qemu_plugin_scoreboard_u64_in_struct(vcpus, vCPUGov, quantum_insn);

    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, quantum_insn, n_insns);
    qemu_plugin_register_vcpu_tb_exec_cond_cb(
        tb, every_quantum_insn,
        QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_COND_GE,
        quantum_insn, max_insn_per_quantum, NULL);
}

static void plugin_exit(void *udata)
{
    qemu_plugin_scoreboard_free(vcpus);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "mips") == 0) {
            uint64_t mips = g_ascii_strtoull(tokens[1], NULL, 10);
            if (!mips) {
                fprintf(stderr, "bad mips value: %s\n", tokens[1]);
                return -1;
            }
            max_insn_per_second = mips * 1000 * 1000;
        } else {
            fprintf(stderr, "option parsing failed: %s\n", opt);
            return -1;
        }
    }

    max_insn_per_quantum = max_insn_per_second / NUM_TIME_UPDATE_PER_SEC;
    if (max_insn_per_quantum > MAX_QUANTUM_INSNS) {
        max_insn_per_quantum = MAX_QUANTUM_INSNS;
    }
    vcpus = qemu_plugin_scoreboard_new(sizeof(vCPUGov));

    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
