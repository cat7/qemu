/*
 * Always-on, cheap MMU/TB instrumentation counters (stage 0 of the
 * TCG MMU speed-up plan).
 *
 * These are the regression instrument for every future MMU change: they
 * answer "how many FULL TLB flushes does this workload cost, and WHO asked
 * for them", "how expensive is a hash-table fill", and "is the TB jump cache
 * doing its job".  Nothing here changes emulation behaviour.
 *
 * Concurrency: the counters are per-vCPU (indexed by cpu_index, cache-line
 * padded) so plain non-atomic increments neither race nor share a line.  The
 * reader sums the slots; a sum taken while the vCPUs run can miss the last
 * few increments, which is irrelevant at the rates involved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EXEC_TCG_MMU_STATS_H
#define EXEC_TCG_MMU_STATS_H

/* Origins of a request for a FULL TLB flush. */
enum {
    TCG_MMU_ORG_SR = 0,     /* mtsr / mtsrin -- segment register write   */
    TCG_MMU_ORG_TLBIE,      /* tlbie (32-bit: degenerates to a full flush)*/
    TCG_MMU_ORG_TLBIA,      /* tlbia                                     */
    TCG_MMU_ORG_BAT,        /* BAT register write                        */
    TCG_MMU_ORG_SDR1,       /* hash-table base/size write                */
    TCG_MMU_ORG_OTHER,      /* anything else in the target               */
    TCG_MMU_ORG_COUNT
};

#define TCG_MMU_STATS_MAX_CPUS 16
#define TCG_MMU_STATS_MMU_IDX  16

typedef struct TCGMMUStats {
    /* --- full-flush REQUESTS, by origin ------------------------------- */
    uint64_t req[TCG_MMU_ORG_COUNT];
    /* mtsr calls that changed nothing (early-out: no flush requested) */
    uint64_t sr_same_value;
    /* every 32-bit mtsr/mtsrin, changed or not */
    uint64_t sr_writes;

    /* --- full flushes actually PERFORMED ------------------------------ */
    uint64_t flush_local;       /* deferred, check_tlb_flush -> tlb_flush   */
    uint64_t flush_global;      /* deferred, ..._all_cpus_synced            */
    uint64_t flush_direct;      /* immediate tlb_flush() from the target    */
    /*
     * Attribution of each PERFORMED deferred flush.  by[] counts an origin
     * whenever it contributed; sole[] counts it only when it was the single
     * contributor.  sole[SR] is the stage-1 go/no-go number.
     */
    uint64_t perf_by[TCG_MMU_ORG_COUNT];
    uint64_t perf_sole[TCG_MMU_ORG_COUNT];
    /* a flush was performed with no origin recorded -> instrumentation gap */
    uint64_t perf_unattributed;
    /* live pending-origin mask, cleared when the flush is performed */
    uint32_t pending;

    /* --- BAT flushes -------------------------------------------------- */
    uint64_t bat_full_flush;    /* >1024 pages: took the full-flush path    */
    uint64_t bat_page_ranges;   /* per-page invalidation loops              */
    uint64_t bat_pages;         /* pages invalidated by those loops         */

    /* --- softmmu fills ------------------------------------------------ */
    uint64_t fill;              /* ppc_cpu_tlb_fill entries (probe or not)  */
    uint64_t fill_probe;        /* ... of which probe-only                  */
    uint64_t fill_fail;         /* ... which raised a guest fault           */
    uint64_t fill_idx[TCG_MMU_STATS_MMU_IDX];

    /* --- 32-bit hash walk --------------------------------------------- */
    uint64_t hash32_xlate;      /* ppc_hash32_xlate calls                   */
    uint64_t hash32_real;       /* ... resolved by real-mode short circuit  */
    uint64_t hash32_bat_hit;    /* ... resolved by a BAT                    */
    uint64_t hash32_htab;       /* ... which reached the hash table         */
    uint64_t hash32_primary;    /* htab lookups satisfied by the primary PTEG */
    uint64_t hash32_secondary;  /* ... by the secondary PTEG                */
    uint64_t hash32_notfound;   /* ... by neither (guest page fault)        */
    uint64_t hash32_pteg_probe; /* PTEG searches (1 or 2 per htab lookup)   */
    uint64_t hash32_pte_slot;   /* PTE slots actually loaded (2 ldl each)   */

    /* --- TB jump cache ------------------------------------------------ */
    uint64_t jc_hit;            /* tb_lookup satisfied from the jump cache  */
    uint64_t jc_miss_found;     /* ... missed, found in the global htable   */
    uint64_t jc_miss_none;      /* ... missed, no TB at all (needs codegen) */

    /* --- code cache --------------------------------------------------- */
    uint64_t tb_flush;          /* whole-code-cache flushes (slot 0 only)   */

    char pad[128];
} TCGMMUStats;

extern TCGMMUStats tcg_mmu_stats[TCG_MMU_STATS_MAX_CPUS];

/*
 * Slot accessor.  cpu_index is masked rather than bounds-checked: with more
 * vCPUs than slots the counters would alias, which is harmless here and
 * cheaper than a branch on the hot path.
 */
static inline TCGMMUStats *tcg_mmu_slot(int cpu_index)
{
    return &tcg_mmu_stats[cpu_index & (TCG_MMU_STATS_MAX_CPUS - 1)];
}

/*
 * Called when a deferred full flush is actually performed: charges it to
 * every origin that had asked for one since the last flush, and -- when a
 * single origin asked -- to that origin alone.  perf_sole[TCG_MMU_ORG_SR]
 * is the number of full flushes a segment-register-only optimisation could
 * have removed, i.e. the stage-1 go/no-go.
 */
static inline void tcg_mmu_flush_attribute(TCGMMUStats *s)
{
    uint32_t m = s->pending;
    int i, n = 0, last = -1;

    if (!m) {
        s->perf_unattributed++;
        return;
    }
    for (i = 0; i < TCG_MMU_ORG_COUNT; i++) {
        if (m & (1u << i)) {
            s->perf_by[i]++;
            n++;
            last = i;
        }
    }
    if (n == 1) {
        s->perf_sole[last]++;
    }
    s->pending = 0;
}

/* Formats the aggregate as "key=value" lines; caller frees. */
char *tcg_mmu_stats_dump(void);

#endif /* EXEC_TCG_MMU_STATS_H */
