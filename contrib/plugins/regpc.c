/*
 * regpc: report the guest PC (and instruction bytes) of every store into
 * a physical-address window. Built to find which code in an opaque
 * guest driver programs a device register: attach with e.g.
 *   -plugin libregpc.dylib,base=0x80084900,len=0x400
 * and every store landing in the window logs the storing instruction's
 * virtual PC, its opcode bytes and the value written. Each unique PC is
 * reported only the first few times so a per-frame handler does not
 * flood the log.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t win_base;
static uint64_t win_size = 0x100;
static uint64_t code_lo;
static uint64_t code_hi;
static GHashTable *seen;
static GMutex lock;

struct insn_rec {
    uint64_t vaddr;
    uint32_t bytes;
};

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
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    if (pa < win_base || pa >= win_base + win_size) {
        return;
    }

    g_mutex_lock(&lock);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(seen,
                                                 (gpointer)(rec->vaddr ^ pa)));
    if (count < 4) {
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        uint64_t data = 0;

        switch (v.type) {
        case QEMU_PLUGIN_MEM_VALUE_U8:  data = v.data.u8;  break;
        case QEMU_PLUGIN_MEM_VALUE_U16: data = v.data.u16; break;
        case QEMU_PLUGIN_MEM_VALUE_U32: data = v.data.u32; break;
        case QEMU_PLUGIN_MEM_VALUE_U64: data = v.data.u64; break;
        default: break;
        }
        qemu_plugin_outs(g_strdup_printf(
            "regpc: pc=0x%" PRIx64 " insn=0x%08x pa=0x%" PRIx64
            " val=0x%" PRIx64 "\n",
            rec->vaddr, rec->bytes, pa, data));
        g_hash_table_insert(seen, (gpointer)(rec->vaddr ^ pa),
                            GUINT_TO_POINTER(count + 1));
    }
    g_mutex_unlock(&lock);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        struct insn_rec *rec = g_new0(struct insn_rec, 1);
        uint32_t raw = 0;

        rec->vaddr = qemu_plugin_insn_vaddr(insn);
        if (qemu_plugin_insn_data(insn, &raw, sizeof(raw)) == sizeof(raw)) {
            rec->bytes = raw;
        }
        if (code_lo && rec->vaddr >= code_lo && rec->vaddr < code_hi) {
            qemu_plugin_outs(g_strdup_printf("code: 0x%" PRIx64 " 0x%08x\n",
                                             rec->vaddr, rec->bytes));
        }
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
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
        } else if (!g_strcmp0(tokens[0], "code_lo")) {
            code_lo = g_ascii_strtoull(tokens[1], NULL, 0);
        } else if (!g_strcmp0(tokens[0], "code_hi")) {
            code_hi = g_ascii_strtoull(tokens[1], NULL, 0);
        } else {
            fprintf(stderr, "regpc: bad option %s\n", opt);
            return -1;
        }
    }
    if (!win_base) {
        fprintf(stderr, "regpc: base=<phys addr> required\n");
        return -1;
    }
    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
