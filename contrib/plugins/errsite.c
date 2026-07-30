/*
 * errsite: watch guest memory reads that fall inside the classic Mac OS
 * "divide by zero" entry of the low-level System Error string table
 * (error-code byte 0x04, length-prefixed Pascal string "divide by
 * zero") -- this table was located live in RAM at physical/virtual
 * 0x294e61..0x294e78 by inspecting a paused, crashed guest. Unlike
 * watching the whole string table (which is read during ordinary
 * resource caching on every boot, crashing or not), reads of this
 * SPECIFIC entry only happen when the "divide by zero" error is being
 * displayed, so this is a crash-specific trigger.
 *
 * On a hit, captures the accessing instruction's PC, LR, and walks the
 * PowerPC standard stack back-chain (via r1) a few frames to recover
 * the call chain that led here -- all synchronously, since delayed
 * re-inspection of guest memory/registers is unreliable on this target
 * (code/data at a given address gets dynamically reused).
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
static struct qemu_plugin_register *r1_handle;

#define TARGET_LO 0x294e60ULL
#define TARGET_HI 0x294e80ULL
#define BACKTRACE_DEPTH 6

static bool read_reg64(struct qemu_plugin_register *h, uint64_t *val_out)
{
    GByteArray *buf;
    bool ok = false;

    if (!h) {
        return false;
    }
    buf = g_byte_array_new();
    if (qemu_plugin_read_register(h, buf) && buf->len >= 4) {
        if (buf->len >= 8) {
            *val_out = ((uint64_t)buf->data[buf->len - 8] << 56) |
                       ((uint64_t)buf->data[buf->len - 7] << 48) |
                       ((uint64_t)buf->data[buf->len - 6] << 40) |
                       ((uint64_t)buf->data[buf->len - 5] << 32) |
                       ((uint64_t)buf->data[buf->len - 4] << 24) |
                       ((uint64_t)buf->data[buf->len - 3] << 16) |
                       ((uint64_t)buf->data[buf->len - 2] << 8) |
                       (uint64_t)buf->data[buf->len - 1];
        } else {
            *val_out = ((uint64_t)buf->data[buf->len - 4] << 24) |
                       ((uint64_t)buf->data[buf->len - 3] << 16) |
                       ((uint64_t)buf->data[buf->len - 2] << 8) |
                       (uint64_t)buf->data[buf->len - 1];
        }
        ok = true;
    }
    g_byte_array_free(buf, TRUE);
    return ok;
}

static bool read_u32_at(uint64_t addr, uint32_t *val_out)
{
    GByteArray *buf = g_byte_array_new();
    bool ok = false;

    g_byte_array_set_size(buf, 4);
    if (qemu_plugin_read_memory_vaddr(addr, buf, 4)) {
        *val_out = ((uint32_t)buf->data[0] << 24) |
                   ((uint32_t)buf->data[1] << 16) |
                   ((uint32_t)buf->data[2] << 8) |
                   (uint32_t)buf->data[3];
        ok = true;
    }
    g_byte_array_free(buf, TRUE);
    return ok;
}

static void mem_cb(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    uint64_t pc = (uint64_t)(uintptr_t)udata;
    uint64_t lr = 0, sp = 0;
    int i;

    if (vaddr < TARGET_LO || vaddr >= TARGET_HI) {
        return;
    }
    if (!out_fp) {
        return;
    }

    read_reg64(lr_handle, &lr);
    read_reg64(r1_handle, &sp);

    fprintf(out_fp, "ERRSITE_HIT pc=0x%" PRIx64 " vaddr=0x%" PRIx64
           " lr=0x%" PRIx64 " sp=0x%" PRIx64 " backtrace=", pc, vaddr, lr, sp);

    /* Standard PowerPC back-chain: [SP] = caller's SP, [SP+8] = the
     * caller's saved LR (i.e. one level further up the call chain). */
    fprintf(out_fp, "0x%" PRIx64, lr);
    for (i = 0; i < BACKTRACE_DEPTH && sp != 0; i++) {
        uint32_t next_sp, saved_lr;

        if (!read_u32_at(sp, &next_sp) || next_sp == 0 || next_sp <= sp) {
            break;
        }
        if (!read_u32_at((uint64_t)next_sp + 8, &saved_lr)) {
            break;
        }
        fprintf(out_fp, ",0x%x", saved_lr);
        sp = next_sp;
    }
    fprintf(out_fp, "\n");
    fflush(out_fp);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);

        qemu_plugin_register_vcpu_mem_cb(insn, mem_cb,
                                         QEMU_PLUGIN_CB_R_REGS,
                                         QEMU_PLUGIN_MEM_R,
                                         (void *)(uintptr_t)pc);
    }
}

static void vcpu_init(unsigned int cpu_index, void *udata)
{
    GArray *regs;
    guint i;

    if (lr_handle) {
        return;
    }
    regs = qemu_plugin_get_registers();
    for (i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (g_strcmp0(d->name, "lr") == 0) {
            lr_handle = d->handle;
        } else if (g_strcmp0(d->name, "r1") == 0) {
            r1_handle = d->handle;
        }
    }
    g_array_free(regs, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
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
