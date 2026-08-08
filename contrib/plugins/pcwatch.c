/*
 * pcwatch: log the LR (return address / caller) whenever execution
 * reaches one of a small list of target guest virtual addresses. Armed
 * from process start via translation-time instrumentation, so it can't
 * miss a one-shot event the way a live-attached breakpoint can. Attach
 * with:
 *   -plugin libpcwatch.dylib,pc0=<vaddr>[,pc1=<vaddr>...] (up to 4)
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_TARGETS 4
static uint64_t targets[MAX_TARGETS];
static int n_targets;
static GHashTable *seen;
static GMutex lock;
static struct qemu_plugin_register *lr_handle;

static void find_lr(void)
{
    GArray *regs = qemu_plugin_get_registers();

    if (!regs) {
        return;
    }
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (d->name && !g_strcmp0(d->name, "lr")) {
            lr_handle = d->handle;
            return;
        }
    }
}

struct insn_rec {
    uint64_t vaddr;
};

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    struct insn_rec *rec = udata;
    unsigned count;

    if (!lr_handle) {
        find_lr();
    }

    g_mutex_lock(&lock);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(seen, (gpointer)rec->vaddr));
    if (count >= 8) {
        g_mutex_unlock(&lock);
        return;
    }
    g_hash_table_insert(seen, (gpointer)rec->vaddr, GUINT_TO_POINTER(count + 1));
    g_mutex_unlock(&lock);

    uint64_t lr = 0;
    if (lr_handle) {
        GByteArray *buf = g_byte_array_new();
        if (qemu_plugin_read_register(lr_handle, buf) > 0) {
            for (guint i = 0; i < buf->len; i++) {
                lr = (lr << 8) | buf->data[i];
            }
        }
        g_byte_array_free(buf, TRUE);
    }

    qemu_plugin_outs(g_strdup_printf(
        "pcwatch: pc=0x%" PRIx64 " lr=0x%" PRIx64 "\n", rec->vaddr, lr));
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
        int match = 0;

        for (int t = 0; t < n_targets; t++) {
            if (targets[t] == vaddr) {
                match = 1;
                break;
            }
        }
        if (!match) {
            continue;
        }

        struct insn_rec *rec = g_new0(struct insn_rec, 1);
        rec->vaddr = vaddr;
        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                               QEMU_PLUGIN_CB_R_REGS, rec);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);

        if (g_str_has_prefix(tokens[0], "pc") && n_targets < MAX_TARGETS) {
            targets[n_targets++] = g_ascii_strtoull(tokens[1], NULL, 0);
        } else {
            fprintf(stderr, "pcwatch: bad option %s\n", opt);
            return -1;
        }
    }
    if (n_targets == 0) {
        fprintf(stderr, "pcwatch: pc0=<vaddr> required\n");
        return -1;
    }
    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
