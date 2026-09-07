/*
 * pulse: one CSV line per second describing what the guest is doing --
 * instructions executed, translation blocks translated, code-cache
 * flushes, and exceptions/interrupts by PowerPC vector -- so bursts of
 * guest speed and slowness can be lined up with their cause.
 *
 * Args: out=<path>   (default: stderr)
 *
 * Columns: time, insns, tbs, flushes, dsi(0x300), isi(0x400), align(0x600),
 *          prog(0x700), fpu(0x800), dec(0x900), ext(0x500), sc(0xc00),
 *          trace(0xd00), other_exc, interrupts_total
 * All counts are deltas for that second (insns summed over vCPUs).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_scoreboard *sb;
static qemu_plugin_u64 insns;
static uint64_t tbs, flushes, ints;
static uint64_t vec[32];
static FILE *out_fp, *pcs_fp;
static volatile int stopping;
static GThread *thread;

/* Faulting-PC histograms for the vectors that matter most on classic Mac
 * OS: 0x300 DSI, 0x700 program, 0xc00 syscall. Keyed by from_pc; the
 * value also remembers the instruction word seen there first. */
typedef struct { uint64_t count; uint32_t insn; } pcrec;
static GHashTable *pcs[3];      /* [0]=0x300 [1]=0x700 [2]=0xc00 */
/* PCs whose instruction word could not be read at exception time (the
 * CPU is already in real mode then); resolved from the next translation
 * of a block containing them. */
static GHashTable *unresolved;  /* pc -> pcrec* (not owned) */
static const char *pcs_name[3] = { "0x300 DSI", "0x700 program", "0xc00 syscall" };
static GMutex pcs_lock;

static void note_pc(int which, uint64_t from_pc)
{
    pcrec *r;

    g_mutex_lock(&pcs_lock);
    r = g_hash_table_lookup(pcs[which], GUINT_TO_POINTER((guint)from_pc));
    if (!r) {
        GByteArray *b = g_byte_array_new();

        r = g_new0(pcrec, 1);
        if (qemu_plugin_read_memory_vaddr(from_pc, b, 4) && b->len == 4) {
            r->insn = (b->data[0] << 24) | (b->data[1] << 16) |
                      (b->data[2] << 8) | b->data[3];
        } else {
            g_hash_table_insert(unresolved, GUINT_TO_POINTER((guint)from_pc), r);
        }
        g_byte_array_free(b, TRUE);
        g_hash_table_insert(pcs[which], GUINT_TO_POINTER((guint)from_pc), r);
    }
    r->count++;
    g_mutex_unlock(&pcs_lock);
}

static gint cmp_desc(gconstpointer a, gconstpointer b, gpointer d)
{
    GHashTable *t = d;
    pcrec *ra = g_hash_table_lookup(t, a), *rb = g_hash_table_lookup(t, b);
    return (rb->count > ra->count) - (rb->count < ra->count);
}

static void report_pcs(void)
{
    int w;
    GDateTime *dt = g_date_time_new_now_utc();
    gchar *ts = g_date_time_format_iso8601(dt);

    if (!pcs_fp) {
        return;
    }
    g_mutex_lock(&pcs_lock);
    fprintf(pcs_fp, "=== %s (cumulative)\n", ts);
    for (w = 0; w < 3; w++) {
        GList *keys = g_hash_table_get_keys(pcs[w]), *l;
        int n = 0;

        keys = g_list_sort_with_data(keys, cmp_desc, pcs[w]);
        fprintf(pcs_fp, "%s: %u distinct PCs\n", pcs_name[w], g_hash_table_size(pcs[w]));
        for (l = keys; l && n < 12; l = l->next, n++) {
            pcrec *r = g_hash_table_lookup(pcs[w], l->data);
            fprintf(pcs_fp, "   pc=0x%08x count=%" PRIu64 " insn=0x%08x\n",
                    GPOINTER_TO_UINT(l->data), r->count, r->insn);
        }
        g_list_free(keys);
    }
    g_mutex_unlock(&pcs_lock);
    fflush(pcs_fp);
    g_free(ts);
    g_date_time_unref(dt);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    __atomic_fetch_add(&tbs, 1, __ATOMIC_RELAXED);
    if (g_hash_table_size(unresolved)) {
        size_t n = qemu_plugin_tb_n_insns(tb), i;

        g_mutex_lock(&pcs_lock);
        for (i = 0; i < n; i++) {
            struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
            uint64_t va = qemu_plugin_insn_vaddr(in);
            pcrec *r = g_hash_table_lookup(unresolved, GUINT_TO_POINTER((guint)va));

            if (r) {
                uint8_t b[4] = { 0 };

                qemu_plugin_insn_data(in, b, 4);
                r->insn = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
                g_hash_table_remove(unresolved, GUINT_TO_POINTER((guint)va));
            }
        }
        g_mutex_unlock(&pcs_lock);
    }
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, insns, qemu_plugin_tb_n_insns(tb));
}

static void vcpu_discon(unsigned int vcpu_index,
                        enum qemu_plugin_discon_type type,
                        uint64_t from_pc, uint64_t to_pc, void *udata)
{
    int slot = (to_pc >> 8) & 31;

    if (type == QEMU_PLUGIN_DISCON_INTERRUPT) {
        __atomic_fetch_add(&ints, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&vec[slot], 1, __ATOMIC_RELAXED);
    if (slot == 3) {
        note_pc(0, from_pc);
    } else if (slot == 7) {
        note_pc(1, from_pc);
    } else if (slot == 0xc) {
        note_pc(2, from_pc);
    }
}

static void flush_cb(void *udata)
{
    __atomic_fetch_add(&flushes, 1, __ATOMIC_RELAXED);
}

static uint64_t rd(uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

static void emit(uint64_t *prev, gboolean header)
{
    uint64_t cur[16];
    int i;
    GDateTime *dt = g_date_time_new_now_utc();
    gchar *ts = g_date_time_format_iso8601(dt);

    if (header) {
        fprintf(out_fp, "time,insns,tbs,flushes,dsi,isi,align,prog,fpu,dec,ext,sc,trace,other_exc,interrupts\n");
    }
    cur[0] = qemu_plugin_u64_sum(insns);
    cur[1] = rd(&tbs); cur[2] = rd(&flushes);
    cur[3] = rd(&vec[3]); cur[4] = rd(&vec[4]); cur[5] = rd(&vec[6]);
    cur[6] = rd(&vec[7]); cur[7] = rd(&vec[8]); cur[8] = rd(&vec[9]);
    cur[9] = rd(&vec[5]); cur[10] = rd(&vec[0xc]); cur[11] = rd(&vec[0xd]);
    cur[12] = 0;
    for (i = 0; i < 32; i++) {
        if (i != 3 && i != 4 && i != 6 && i != 7 && i != 8 && i != 9 &&
            i != 5 && i != 0xc && i != 0xd) {
            cur[12] += rd(&vec[i]);
        }
    }
    cur[13] = rd(&ints);
    fprintf(out_fp, "%s", ts);
    for (i = 0; i < 14; i++) {
        fprintf(out_fp, ",%" PRIu64, cur[i] - prev[i]);
        prev[i] = cur[i];
    }
    fputc('\n', out_fp);
    fflush(out_fp);
    g_free(ts);
    g_date_time_unref(dt);
}

static gpointer reporter(gpointer data)
{
    uint64_t prev[16] = { 0 };
    gboolean first = TRUE;

    int n = 0;

    while (!stopping) {
        g_usleep(1000000);
        emit(prev, first);
        first = FALSE;
        if (++n % 10 == 0) {
            report_pcs();
        }
    }
    report_pcs();
    return NULL;
}

static void plugin_exit(void *p)
{
    stopping = 1;
    if (thread) {
        g_thread_join(thread);
    }
    if (out_fp && out_fp != stderr) {
        fclose(out_fp);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int i;

    out_fp = stderr;
    for (i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "out=")) {
            gchar *pcs_path = g_strconcat(argv[i] + 4, ".pcs.txt", NULL);

            out_fp = fopen(argv[i] + 4, "w");
            if (!out_fp) {
                fprintf(stderr, "pulse: cannot open %s\n", argv[i] + 4);
                return -1;
            }
            pcs_fp = fopen(pcs_path, "w");
            g_free(pcs_path);
        }
    }
    for (i = 0; i < 3; i++) {
        pcs[i] = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    }
    unresolved = g_hash_table_new(g_direct_hash, g_direct_equal);
    sb = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(sb);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_vcpu_discon_cb(id, QEMU_PLUGIN_DISCON_ALL,
                                        vcpu_discon, NULL);
    qemu_plugin_register_flush_cb(id, flush_cb, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    thread = g_thread_new("pulse", reporter, NULL);
    return 0;
}
