/*
 * divwatch: log every execution of a PowerPC divw/divwu (and OE-enabled
 * divwo/divwuo) instruction whose divisor register is zero at the
 * moment of execution -- used to find the real call site of the
 * "divide by zero" System Error on classic Mac OS, since PowerPC
 * hardware division does not itself trap and this project's earlier
 * attempt to find an explicit tw/twi zero-check trap came up empty
 * (see excwatch.c) -- the crash may instead come from an unguarded
 * divw/divwu whose undefined result gets used or explicitly checked by
 * ordinary branch code afterward, not from a hardware trap at all.
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
static struct qemu_plugin_register *gpr_handle[32];
static struct qemu_plugin_register *lr_handle;
static GHashTable *seen;
static GMutex seen_lock;

#define CODE_BEFORE 48
#define CODE_AFTER 16

typedef struct {
    uint64_t pc;
    int ra, rb, rt;
    bool is_unsigned;
} DivInsn;

static bool read_gpr(int n, uint32_t *val_out)
{
    GByteArray *buf;
    bool ok = false;

    if (n < 0 || n > 31 || !gpr_handle[n]) {
        return false;
    }
    buf = g_byte_array_new();
    if (qemu_plugin_read_register(gpr_handle[n], buf) && buf->len >= 4) {
        *val_out = ((uint32_t)buf->data[buf->len - 4] << 24) |
                   ((uint32_t)buf->data[buf->len - 3] << 16) |
                   ((uint32_t)buf->data[buf->len - 2] << 8) |
                   (uint32_t)buf->data[buf->len - 1];
        ok = true;
    }
    g_byte_array_free(buf, TRUE);
    return ok;
}

static uint64_t read_lr(void)
{
    GByteArray *buf;
    uint64_t lr = 0;

    if (!lr_handle) {
        return 0;
    }
    buf = g_byte_array_new();
    if (qemu_plugin_read_register(lr_handle, buf) && buf->len >= 4) {
        lr = ((uint64_t)buf->data[buf->len - 4] << 24) |
             ((uint64_t)buf->data[buf->len - 3] << 16) |
             ((uint64_t)buf->data[buf->len - 2] << 8) |
             (uint64_t)buf->data[buf->len - 1];
    }
    g_byte_array_free(buf, TRUE);
    return lr;
}

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    DivInsn *d = (DivInsn *)udata;
    uint32_t ra_val = 0, rb_val = 0;
    bool have_ra, have_rb;

    have_ra = read_gpr(d->ra, &ra_val);
    have_rb = read_gpr(d->rb, &rb_val);

    if (out_fp && have_rb && rb_val == 0) {
        uint64_t lr = read_lr();
        gboolean first_time;
        gpointer key = (gpointer)(uintptr_t)d->pc;

        g_mutex_lock(&seen_lock);
        first_time = !g_hash_table_contains(seen, key);
        if (first_time) {
            g_hash_table_add(seen, key);
        }
        g_mutex_unlock(&seen_lock);

        fprintf(out_fp, "DIV_BY_ZERO pc=0x%" PRIx64 " %s rt=%d ra=%d rb=%d"
               " ra_val=0x%x rb_val=0x%x lr=0x%" PRIx64,
               d->pc, d->is_unsigned ? "divwu" : "divw", d->rt, d->ra, d->rb,
               ra_val, rb_val, lr);

        if (first_time) {
            GByteArray *code = g_byte_array_new();
            uint64_t code_start = d->pc - CODE_BEFORE;
            size_t i;

            g_byte_array_set_size(code, CODE_BEFORE + CODE_AFTER);
            if (qemu_plugin_read_memory_vaddr(code_start, code,
                                              CODE_BEFORE + CODE_AFTER)) {
                fprintf(out_fp, " code_start=0x%" PRIx64 " code=", code_start);
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
        uint32_t word, opcode, xo10;

        if (sz != 4 || qemu_plugin_insn_data(insn, data, sizeof(data)) != 4) {
            continue;
        }
        word = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
               ((uint32_t)data[2] << 8) | (uint32_t)data[3];
        opcode = (word >> 26) & 0x3F;
        if (opcode != 31) {
            continue;
        }
        xo10 = (word >> 1) & 0x3FF;
        /* divwu=459, divw=491; the OE-enabled forms (divwuo/divwo) set
         * bit 21, i.e. add 512 to the plain 10-bit XO value. */
        if (xo10 == 459 || xo10 == 459 + 512 ||
            xo10 == 491 || xo10 == 491 + 512) {
            DivInsn *d = g_new0(DivInsn, 1);
            d->pc = qemu_plugin_insn_vaddr(insn);
            d->rt = (word >> 21) & 0x1F;
            d->ra = (word >> 16) & 0x1F;
            d->rb = (word >> 11) & 0x1F;
            d->is_unsigned = (xo10 == 459 || xo10 == 459 + 512);
            qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                                   QEMU_PLUGIN_CB_R_REGS,
                                                   (void *)d);
        }
    }
}

static void vcpu_init(unsigned int cpu_index, void *udata)
{
    GArray *regs;
    guint i;
    char name[8];

    if (gpr_handle[0]) {
        return;
    }
    regs = qemu_plugin_get_registers();
    for (i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *dsc =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        int n;
        if (g_strcmp0(dsc->name, "lr") == 0) {
            lr_handle = dsc->handle;
            continue;
        }
        for (n = 0; n < 32; n++) {
            g_snprintf(name, sizeof(name), "r%d", n);
            if (g_strcmp0(dsc->name, name) == 0) {
                gpr_handle[n] = dsc->handle;
                break;
            }
        }
    }
    g_array_free(regs, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    seen = g_hash_table_new(NULL, NULL);
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
