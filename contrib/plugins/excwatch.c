/*
 * excwatch: log every PowerPC Program Exception (vector offset 0x700).
 * On this target, "divide by zero" surfaces as an explicit tw/twi trap
 * instruction (TO field with the "equal" bit set) that the compiler
 * emits before a divw/divwu, since hardware divide doesn't trap on its
 * own. Most Program Exceptions are unrelated (illegal-instruction-based
 * Mixed-Mode/CFM dispatch, FP emulation, etc.), so this decodes the
 * faulting instruction itself and only flags REAL_ZERO_TRAP when it's a
 * tw/twi whose "equal" condition holds against literal zero.
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

static void vcpu_discon(unsigned int vcpu_index,
                        enum qemu_plugin_discon_type type,
                        uint64_t from_pc, uint64_t to_pc, void *udata)
{
    GByteArray *insn_bytes;
    uint32_t word = 0;
    bool have_word;
    bool is_trap = false;
    bool zero_trap = false;
    uint32_t opcode, to, ra = 0, rb = 0;
    int32_t simm;
    uint32_t ra_val = 0, rb_val = 0;
    bool have_ra = false, have_rb = false;

    if (type != QEMU_PLUGIN_DISCON_EXCEPTION) {
        return;
    }
    if ((to_pc & 0xFFFF) != 0x700) {
        return;
    }

    insn_bytes = g_byte_array_new();
    g_byte_array_set_size(insn_bytes, 4);
    have_word = qemu_plugin_read_memory_vaddr(from_pc, insn_bytes, 4);
    if (have_word) {
        word = ((uint32_t)insn_bytes->data[0] << 24) |
               ((uint32_t)insn_bytes->data[1] << 16) |
               ((uint32_t)insn_bytes->data[2] << 8) |
               (uint32_t)insn_bytes->data[3];
    }
    g_byte_array_free(insn_bytes, TRUE);

    if (have_word) {
        opcode = (word >> 26) & 0x3F;
        to = (word >> 21) & 0x1F;
        ra = (word >> 16) & 0x1F;
        rb = (word >> 11) & 0x1F;
        simm = (int16_t)(word & 0xFFFF);

        if (opcode == 31 && ((word >> 1) & 0x3FF) == 4) {
            /* tw RA,RB form */
            is_trap = true;
            have_ra = read_gpr(ra, &ra_val);
            have_rb = read_gpr(rb, &rb_val);
            if ((to & 0x4) && have_ra && have_rb && ra_val == rb_val) {
                zero_trap = (ra_val == 0);
            }
        } else if (opcode == 3) {
            /* twi RA,SIMM form */
            is_trap = true;
            have_ra = read_gpr(ra, &ra_val);
            if ((to & 0x4) && have_ra && (int32_t)ra_val == simm) {
                zero_trap = (simm == 0);
            }
        }
    }

    if (out_fp) {
        GByteArray *code = g_byte_array_new();
        uint64_t code_start = from_pc - CODE_BEFORE;
        size_t i;

        fprintf(out_fp, "%s from_pc=0x%" PRIx64 " to_pc=0x%" PRIx64
               " word=0x%08x is_trap=%d ra=%u rb=%u ra_val=0x%x rb_val=0x%x",
               zero_trap ? "REAL_ZERO_TRAP" : "exc",
               from_pc, to_pc, word, is_trap, ra, rb, ra_val, rb_val);

        if (zero_trap) {
            g_byte_array_set_size(code, CODE_BEFORE + CODE_AFTER);
            if (qemu_plugin_read_memory_vaddr(code_start, code,
                                              CODE_BEFORE + CODE_AFTER)) {
                fprintf(out_fp, " code_start=0x%" PRIx64 " code=", code_start);
                for (i = 0; i < code->len; i++) {
                    fprintf(out_fp, "%02x", code->data[i]);
                }
            }
        }
        g_byte_array_free(code, TRUE);
        fprintf(out_fp, "\n");
        fflush(out_fp);
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
        }
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_discon_cb(id, QEMU_PLUGIN_DISCON_EXCEPTION,
                                        vcpu_discon, NULL);
    return 0;
}
