/*
 * fbwatch2: like regpc, but also reads the LR register at each matching
 * store, to find the caller of a hot fill/blit routine without needing to
 * race a live breakpoint against a one-shot draw that already happened.
 * Attach with:
 *   -plugin libfbwatch2.dylib,base=<phys>,len=<len>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t win_base;
static uint64_t win_size = 0x100;
static GHashTable *seen;
static GMutex lock;
static struct qemu_plugin_register *lr_handle;

struct insn_rec {
    uint64_t vaddr;
};

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
            qemu_plugin_outs("fbwatch2: found lr register\n");
            return;
        }
    }
    /* Dump all register names once so we can pick the right one if "lr"
     * isn't the exact name this build uses. */
    GString *names = g_string_new("fbwatch2: registers:");
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        g_string_append_printf(names, " %s", d->name ? d->name : "?");
    }
    g_string_append(names, "\n");
    qemu_plugin_outs(names->str);
    g_string_free(names, TRUE);
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct insn_rec *rec = udata;
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa;
    unsigned count;

    if (!qemu_plugin_mem_is_store(info)) {
        return;
    }
    if (!lr_handle) {
        find_lr();
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    if (pa < win_base || pa >= win_base + win_size) {
        return;
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
        "fbwatch2: pc=0x%" PRIx64 " lr=0x%" PRIx64 " pa=0x%" PRIx64 "\n",
        rec->vaddr, lr, pa));
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        struct insn_rec *rec = g_new0(struct insn_rec, 1);

        rec->vaddr = qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_R_REGS,
                                         QEMU_PLUGIN_MEM_W, rec);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);

        if (!g_strcmp0(tokens[0], "base")) {
            win_base = g_ascii_strtoull(tokens[1], NULL, 0);
        } else if (!g_strcmp0(tokens[0], "len")) {
            win_size = g_ascii_strtoull(tokens[1], NULL, 0);
        } else {
            fprintf(stderr, "fbwatch2: bad option %s\n", opt);
            return -1;
        }
    }
    if (!win_base) {
        fprintf(stderr, "fbwatch2: base=<phys addr> required\n");
        return -1;
    }
    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
