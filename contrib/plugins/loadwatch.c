/*
 * loadwatch: print PC, physical address, and value of every guest CPU
 * LOAD from [base, base+size), up to a hit cap. The load-side twin of
 * storewatch.c -- diagnostic for "does anything ever read this memory
 * back" questions.
 *
 * Args: base=<hex>,size=<hex>,max=<dec>
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t win_base;
static uint64_t win_size = 0x100;
static uint64_t max_hits = 400;
static uint64_t nhits;
static GMutex lock;

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa;

    if (qemu_plugin_mem_is_store(info)) {
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
    if (nhits++ < max_hits) {
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        fprintf(stderr, "loadwatch: pc=%" PRIx64 " pa=%" PRIx64
                " size=%u val=%" PRIx64 "\n",
                (uint64_t)(uintptr_t)udata, pa,
                1u << qemu_plugin_mem_size_shift(info),
                v.type == QEMU_PLUGIN_MEM_VALUE_U64 ? v.data.u64 :
                (uint64_t)v.data.u32);
    }
    g_mutex_unlock(&lock);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_R,
                                         (void *)(uintptr_t)
                                         qemu_plugin_insn_vaddr(insn));
    }
}

static void plugin_exit(void *p)
{
    fprintf(stderr, "loadwatch: %" PRIu64 " hits total\n", nhits);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tok = g_strsplit(argv[i], "=", 2);
        if (g_strcmp0(tok[0], "base") == 0) {
            win_base = g_ascii_strtoull(tok[1], NULL, 16);
        } else if (g_strcmp0(tok[0], "size") == 0) {
            win_size = g_ascii_strtoull(tok[1], NULL, 16);
        } else if (g_strcmp0(tok[0], "max") == 0) {
            max_hits = g_ascii_strtoull(tok[1], NULL, 10);
        } else {
            fprintf(stderr, "loadwatch: bad option %s\n", argv[i]);
            return -1;
        }
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
