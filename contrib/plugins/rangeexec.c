/*
 * rangeexec: log the first execution of each translation block whose
 * start address falls within a configurable [lo, hi) range.
 *
 * Args: lo=<hex>,hi=<hex>
 */
#include <inttypes.h>
#include <stdlib.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t range_lo = 0;
static uint64_t range_hi = 0;
static GMutex lock;
static GHashTable *seen;

static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    uint64_t addr = (uint64_t)(uintptr_t)udata;
    g_mutex_lock(&lock);
    if (!g_hash_table_contains(seen, (gpointer)addr)) {
        g_hash_table_add(seen, (gpointer)addr);
        g_mutex_unlock(&lock);
        qemu_plugin_outs(g_strdup_printf("FIRSTEXEC 0x%" PRIx64 "\n", addr));
    } else {
        g_mutex_unlock(&lock);
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    uint64_t addr = qemu_plugin_tb_vaddr(tb);
    if (addr >= range_lo && addr < range_hi) {
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                              QEMU_PLUGIN_CB_NO_REGS,
                                              (void *)(uintptr_t)addr);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    seen = g_hash_table_new(NULL, NULL);
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "lo") == 0) {
            range_lo = g_ascii_strtoull(tokens[1], NULL, 16);
        } else if (g_strcmp0(tokens[0], "hi") == 0) {
            range_hi = g_ascii_strtoull(tokens[1], NULL, 16);
        }
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
