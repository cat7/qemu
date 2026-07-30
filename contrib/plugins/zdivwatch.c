/*
 * zdivwatch: catch the Mac OS ROM 68K emulator's zero-divide raise path.
 *
 * The emulator (mapped at 0x68000000 by the nanokernel on this ROM)
 * checks a 68K DIVU/DIVS divisor for zero BEFORE dividing and branches
 * to a single raise stub at 0x68066e64 ("li r6, 0x2014" -- the 68020
 * format-2 zero-divide vector word -- then a jump to the generic
 * exception builder). Instrumenting that ONE instruction gives a
 * zero-overhead trap that fires exactly when an emulated 68K divide by
 * zero occurs, with the full 68K state still live in the emulator's
 * PPC register file (r24 = 68K instruction fetch pointer, r8-r15 =
 * D0-D7, r16-r23 = A0-A7, r4 = the zero divisor's source register
 * value, r3 = operand EA scratch).
 *
 * On each hit: logs all 32 GPRs and synchronously captures the 68K
 * code bytes around the faulting instruction (r24) -- guest memory
 * content at a given address is not stable over time on this target,
 * so only an in-callback capture is trustworthy.
 *
 * Plugin arg: outfile=<path> (required for output), also accepts
 * raise=<hex vaddr> to override the raise-stub address.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static FILE *out_fp;
static uint64_t raise_vaddr = 0x68066e64;
static struct qemu_plugin_register *gpr_handle[32];

#define CODE_BEFORE 32
#define CODE_AFTER 16

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

static void dump_mem(const char *tag, uint64_t addr, size_t len)
{
    GByteArray *buf = g_byte_array_new();
    size_t i;

    g_byte_array_set_size(buf, len);
    if (qemu_plugin_read_memory_vaddr(addr, buf, len)) {
        fprintf(out_fp, " %s@0x%" PRIx64 "=", tag, addr);
        for (i = 0; i < buf->len; i++) {
            fprintf(out_fp, "%02x", buf->data[i]);
        }
    }
    g_byte_array_free(buf, TRUE);
}

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    uint32_t gpr[32] = { 0 };
    int i;

    if (!out_fp) {
        return;
    }
    for (i = 0; i < 32; i++) {
        read_gpr(i, &gpr[i]);
    }

    fprintf(out_fp, "ZERODIV_RAISE");
    for (i = 0; i < 32; i++) {
        fprintf(out_fp, " r%d=0x%x", i, gpr[i]);
    }
    /* r24 = 68K instruction fetch pointer (points just past the opcode) */
    dump_mem("m68kcode", gpr[24] - CODE_BEFORE, CODE_BEFORE + CODE_AFTER);
    /* r20 = A4 under the documented D0-D7/A0-A7 = r8-r23 mapping;
     * the suspect driver globals live at small positive offsets. */
    dump_mem("a4_e70", gpr[20] + 0xe70, 0x40);
    /* r22/r23 = A6/A7: capture the argument frame / stack top */
    dump_mem("a6frame", gpr[22], 0x40);
    dump_mem("stack", gpr[23], 0x40);
    fprintf(out_fp, "\n");
    fflush(out_fp);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        if (qemu_plugin_insn_vaddr(insn) == raise_vaddr) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                                   QEMU_PLUGIN_CB_R_REGS,
                                                   NULL);
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
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        int n;
        for (n = 0; n < 32; n++) {
            g_snprintf(name, sizeof(name), "r%d", n);
            if (g_strcmp0(d->name, name) == 0) {
                gpr_handle[n] = d->handle;
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
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "outfile") == 0) {
            out_fp = fopen(tokens[1], "w");
        } else if (g_strcmp0(tokens[0], "raise") == 0) {
            raise_vaddr = g_ascii_strtoull(tokens[1], NULL, 16);
        }
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
