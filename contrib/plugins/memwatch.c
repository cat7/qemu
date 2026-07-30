/*
 * memwatch: log every execution of a PowerPC mftb/mftbu (read timebase)
 * instruction, together with the PC and the link register (LR) at that
 * point -- used to find real callers of timebase-based elapsed-time
 * calibration code without the overhead (and resulting timing
 * perturbation) of instrumenting every single instruction.
 *
 * No args needed; always active once loaded.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static FILE *out_fp;
static struct qemu_plugin_register *lr_handle;
static GHashTable *seen_lr;
static GMutex seen_lock;

static bool is_mftb(uint32_t word, int *spr_out)
{
    uint32_t opcode = (word >> 26) & 0x3F;
    uint32_t xo = (word >> 1) & 0x3FF;
    /* the two 5-bit TBR/SPR sub-fields are transmitted low-order-first */
    uint32_t tbr_lo = (word >> 16) & 0x1F;
    uint32_t tbr_hi = (word >> 11) & 0x1F;
    uint32_t tbr = (tbr_hi << 5) | tbr_lo;

    /*
     * mftb is its own dedicated X-form instruction (XO=371), not an
     * mfspr(SPR=268/269) alias (XO=339) -- verified against the known
     * real-world encoding of "mftb r3" (0x7c6c42e6).
     */
    if (opcode == 31 && xo == 371 && (tbr == 268 || tbr == 269)) {
        *spr_out = (int)tbr;
        return true;
    }
    return false;
}

#define LR_CODE_BEFORE 64
#define LR_CODE_AFTER 16

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    uint64_t pc = (uint64_t)(uintptr_t)udata;
    GByteArray *buf = g_byte_array_new();
    uint64_t lr = 0;

    if (lr_handle && qemu_plugin_read_register(lr_handle, buf)) {
        if (buf->len >= 4) {
            /* target byte order == big endian for this guest */
            lr = ((uint64_t)buf->data[buf->len - 4] << 24) |
                 ((uint64_t)buf->data[buf->len - 3] << 16) |
                 ((uint64_t)buf->data[buf->len - 2] << 8) |
                 (uint64_t)buf->data[buf->len - 1];
        }
    }
    g_byte_array_free(buf, TRUE);

    if (out_fp) {
        /* dedup: only dump code bytes the first time this (pc,lr) pair is
         * seen -- the same call site repeats identically on every
         * iteration of a hot loop, so capturing it once is enough and
         * avoids an enormous, mostly-redundant log. */
        uint64_t key = pc ^ (lr * 0x9E3779B97F4A7C15ULL);
        bool first_time;

        g_mutex_lock(&seen_lock);
        first_time = !g_hash_table_contains(seen_lr, (gpointer)key);
        if (first_time) {
            g_hash_table_add(seen_lr, (gpointer)key);
        }
        g_mutex_unlock(&seen_lock);

        fprintf(out_fp, "MFTB pc=0x%" PRIx64 " lr=0x%" PRIx64, pc, lr);
        if (lr && first_time) {
            GByteArray *code = g_byte_array_new();
            uint64_t code_start = lr - LR_CODE_BEFORE;
            size_t i;

            g_byte_array_set_size(code, LR_CODE_BEFORE + LR_CODE_AFTER);
            if (qemu_plugin_read_memory_vaddr(code_start, code,
                                              LR_CODE_BEFORE + LR_CODE_AFTER)) {
                fprintf(out_fp, " lrcode_start=0x%" PRIx64 " lrcode=", code_start);
                for (i = 0; i < code->len; i++) {
                    fprintf(out_fp, "%02x", code->data[i]);
                }
            }
            g_byte_array_free(code, TRUE);
        }
        fprintf(out_fp, "\n");
        fflush(out_fp);
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint8_t data[4];
        size_t sz = qemu_plugin_insn_size(insn);
        uint32_t word;
        int spr;

        if (sz == 4 && qemu_plugin_insn_data(insn, data, sizeof(data)) == 4) {
            /* guest is big-endian */
            word = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8) | (uint32_t)data[3];
            if (is_mftb(word, &spr)) {
                uint64_t pc = qemu_plugin_insn_vaddr(insn);
                qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                                       QEMU_PLUGIN_CB_R_REGS,
                                                       (void *)(uintptr_t)pc);
            }
        }
    }
}

static void vcpu_init(unsigned int cpu_index, void *udata)
{
    GArray *regs = qemu_plugin_get_registers();
    guint i;

    if (lr_handle) {
        return;
    }
    for (i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (g_strcmp0(d->name, "lr") == 0) {
            lr_handle = d->handle;
            break;
        }
    }
    g_array_free(regs, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    seen_lr = g_hash_table_new(NULL, NULL);
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "outfile") == 0) {
            out_fp = fopen(tokens[1], "w");
        }
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
