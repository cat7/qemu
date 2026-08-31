/*
 * Always-on MMU/TB instrumentation counters -- storage and formatting.
 *
 * See include/exec/tcg-mmu-stats.h for what each counter means.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/tcg-mmu-stats.h"

TCGMMUStats tcg_mmu_stats[TCG_MMU_STATS_MAX_CPUS];

static const char *const org_name[TCG_MMU_ORG_COUNT] = {
    "sr", "tlbie", "tlbia", "bat", "sdr1", "other",
};

char *tcg_mmu_stats_dump(void)
{
    TCGMMUStats t;
    GString *out = g_string_new(NULL);
    int i, j;

    memset(&t, 0, sizeof(t));
    for (i = 0; i < TCG_MMU_STATS_MAX_CPUS; i++) {
        const TCGMMUStats *s = &tcg_mmu_stats[i];

        for (j = 0; j < TCG_MMU_ORG_COUNT; j++) {
            t.req[j] += s->req[j];
            t.perf_by[j] += s->perf_by[j];
            t.perf_sole[j] += s->perf_sole[j];
        }
        for (j = 0; j < TCG_MMU_STATS_MMU_IDX; j++) {
            t.fill_idx[j] += s->fill_idx[j];
        }
        t.sr_same_value += s->sr_same_value;
        t.sr_writes += s->sr_writes;
        t.flush_local += s->flush_local;
        t.flush_global += s->flush_global;
        t.flush_direct += s->flush_direct;
        t.perf_unattributed += s->perf_unattributed;
        t.bat_full_flush += s->bat_full_flush;
        t.bat_page_ranges += s->bat_page_ranges;
        t.bat_pages += s->bat_pages;
        t.fill += s->fill;
        t.fill_probe += s->fill_probe;
        t.fill_fail += s->fill_fail;
        t.hash32_xlate += s->hash32_xlate;
        t.hash32_real += s->hash32_real;
        t.hash32_bat_hit += s->hash32_bat_hit;
        t.hash32_htab += s->hash32_htab;
        t.hash32_primary += s->hash32_primary;
        t.hash32_secondary += s->hash32_secondary;
        t.hash32_notfound += s->hash32_notfound;
        t.hash32_pteg_probe += s->hash32_pteg_probe;
        t.hash32_pte_slot += s->hash32_pte_slot;
        t.jc_hit += s->jc_hit;
        t.jc_miss_found += s->jc_miss_found;
        t.jc_miss_none += s->jc_miss_none;
        t.tb_flush += s->tb_flush;
    }

    for (j = 0; j < TCG_MMU_ORG_COUNT; j++) {
        g_string_append_printf(out, "req_%s=%" PRIu64 "\n",
                               org_name[j], t.req[j]);
    }
    g_string_append_printf(out, "sr_writes=%" PRIu64 "\n", t.sr_writes);
    g_string_append_printf(out, "sr_same_value=%" PRIu64 "\n",
                           t.sr_same_value);
    g_string_append_printf(out, "flush_local=%" PRIu64 "\n", t.flush_local);
    g_string_append_printf(out, "flush_global=%" PRIu64 "\n", t.flush_global);
    g_string_append_printf(out, "flush_direct=%" PRIu64 "\n", t.flush_direct);
    g_string_append_printf(out, "flush_total=%" PRIu64 "\n",
                           t.flush_local + t.flush_global + t.flush_direct);
    for (j = 0; j < TCG_MMU_ORG_COUNT; j++) {
        g_string_append_printf(out, "perf_by_%s=%" PRIu64 "\n",
                               org_name[j], t.perf_by[j]);
    }
    for (j = 0; j < TCG_MMU_ORG_COUNT; j++) {
        g_string_append_printf(out, "perf_sole_%s=%" PRIu64 "\n",
                               org_name[j], t.perf_sole[j]);
    }
    g_string_append_printf(out, "perf_unattributed=%" PRIu64 "\n",
                           t.perf_unattributed);
    g_string_append_printf(out, "bat_full_flush=%" PRIu64 "\n",
                           t.bat_full_flush);
    g_string_append_printf(out, "bat_page_ranges=%" PRIu64 "\n",
                           t.bat_page_ranges);
    g_string_append_printf(out, "bat_pages=%" PRIu64 "\n", t.bat_pages);
    g_string_append_printf(out, "fill=%" PRIu64 "\n", t.fill);
    g_string_append_printf(out, "fill_probe=%" PRIu64 "\n", t.fill_probe);
    g_string_append_printf(out, "fill_fail=%" PRIu64 "\n", t.fill_fail);
    for (j = 0; j < TCG_MMU_STATS_MMU_IDX; j++) {
        if (t.fill_idx[j]) {
            g_string_append_printf(out, "fill_idx%d=%" PRIu64 "\n",
                                   j, t.fill_idx[j]);
        }
    }
    g_string_append_printf(out, "hash32_xlate=%" PRIu64 "\n", t.hash32_xlate);
    g_string_append_printf(out, "hash32_real=%" PRIu64 "\n", t.hash32_real);
    g_string_append_printf(out, "hash32_bat_hit=%" PRIu64 "\n",
                           t.hash32_bat_hit);
    g_string_append_printf(out, "hash32_htab=%" PRIu64 "\n", t.hash32_htab);
    g_string_append_printf(out, "hash32_primary=%" PRIu64 "\n",
                           t.hash32_primary);
    g_string_append_printf(out, "hash32_secondary=%" PRIu64 "\n",
                           t.hash32_secondary);
    g_string_append_printf(out, "hash32_notfound=%" PRIu64 "\n",
                           t.hash32_notfound);
    g_string_append_printf(out, "hash32_pteg_probe=%" PRIu64 "\n",
                           t.hash32_pteg_probe);
    g_string_append_printf(out, "hash32_pte_slot=%" PRIu64 "\n",
                           t.hash32_pte_slot);
    g_string_append_printf(out, "jc_hit=%" PRIu64 "\n", t.jc_hit);
    g_string_append_printf(out, "jc_miss_found=%" PRIu64 "\n",
                           t.jc_miss_found);
    g_string_append_printf(out, "jc_miss_none=%" PRIu64 "\n", t.jc_miss_none);
    g_string_append_printf(out, "tb_flush=%" PRIu64 "\n", t.tb_flush);

    return g_string_free(out, FALSE);
}
