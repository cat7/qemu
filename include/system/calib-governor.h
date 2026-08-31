/*
 * Windowed calibration governor -- pace guest execution only while a
 * one-shot VIA timer calibration window is open.
 *
 * See system/calib-governor.c for the rationale.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef SYSTEM_CALIB_GOVERNOR_H
#define SYSTEM_CALIB_GOVERNOR_H

#include "qemu/atomic.h"

typedef struct CPUState CPUState;

/*
 * True while a calibration window is open. Read on the hot path (once
 * per TCG execution-loop iteration, i.e. NOT once per translation block
 * when blocks are chained), so it is a plain global rather than anything
 * that needs a lock. Never write it directly: use calib_governor_arm()
 * and the internal close path.
 */
extern bool calib_gov_window_open;

static inline bool calib_governor_active(void)
{
    return unlikely(qatomic_read(&calib_gov_window_open));
}

/*
 * Device side. Called (with the BQL held, from an MMIO write) when a
 * one-shot timer has just been armed to expire in @countdown_ns.
 * Opening or extending a window kicks every vCPU out of its translation
 * block chain so that the new cflags take effect immediately. Returns
 * true if a window was opened or extended -- a one-shot too long to be
 * a calibration spin is ignored.
 */
bool calib_governor_arm(int64_t countdown_ns);

/*
 * Accelerator side. Called from the TCG execution loop after each
 * translation block, but only while calib_governor_active().
 */
void calib_governor_account(CPUState *cpu, unsigned int n_insns);

/*
 * Opt a board in. The T2 hook is generic to every mos6522, but the
 * governor stays inert until the machine that needs it asks, from its
 * own init function. An explicit "calibration-governor=" always wins.
 */
void calib_governor_default_on(void);

/* Machine-property plumbing: "on" | "off" | "mips=<n>". */
bool calib_governor_configure(const char *value, Error **errp);
char *calib_governor_get_config(void);

/* Statistics, for QOM properties and "info via". */
typedef struct CalibGovStats {
    uint64_t windows;       /* windows opened */
    uint64_t rearms;        /* re-arms that extended an open window */
    uint64_t capped;        /* windows truncated by CALIB_GOV_MAX_WINDOW_NS */
    uint64_t ignored;       /* one-shots too long to be a calibration */
    uint64_t governed_ns;   /* total real time spent inside windows */
    uint64_t insns;         /* guest instructions executed inside windows */
    uint64_t slept_ns;      /* real time spent asleep enforcing the cap */
} CalibGovStats;

void calib_governor_get_stats(CalibGovStats *out);

#endif /* SYSTEM_CALIB_GOVERNOR_H */
