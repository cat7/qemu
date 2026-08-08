/*
 * sprwatch: log the guest PC of every mfspr/mtspr targeting a small set
 * of cache-control SPRs (HID0, HID1, MSSCR0, L2CR), to correlate their
 * access pattern/timing against a guest-visible event (e.g. a boot-time
 * self-test failure dialog). Also dumps the few instructions around each
 * match, captured at translation time (not re-read later, since guest
 * code at a given address can be replaced/reused between the match and
 * any later inspection). Attach with:
 *   -plugin libsprwatch.dylib
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define SPR_HID0    1008
#define SPR_HID1    1009
#define SPR_MSSCR0  1014
#define SPR_L2CR    1017
#define CTX_WINDOW  6

static GHashTable *seen;
static GMutex lock;

static const char *spr_name(unsigned spr)
{
    switch (spr) {
    case SPR_HID0: return "HID0";
    case SPR_HID1: return "HID1";
    case SPR_MSSCR0: return "MSSCR0";
    case SPR_L2CR: return "L2CR";
    default: return "?";
    }
}

struct ctx_insn {
    uint64_t vaddr;
    uint32_t bytes;
};

struct insn_rec {
    uint64_t vaddr;
    unsigned spr;
    int is_write;
    int n_ctx;
    struct ctx_insn ctx[CTX_WINDOW];
};

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    struct insn_rec *rec = udata;
    unsigned count;
    GString *out;

    g_mutex_lock(&lock);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(seen,
                                                 (gpointer)rec->vaddr));
    if (count >= 3) {
        g_mutex_unlock(&lock);
        return;
    }
    g_hash_table_insert(seen, (gpointer)rec->vaddr,
                        GUINT_TO_POINTER(count + 1));
    g_mutex_unlock(&lock);

    out = g_string_new(NULL);
    g_string_append_printf(out,
        "sprwatch: pc=0x%" PRIx64 " %cfspr %s(%u) ctx:",
        rec->vaddr, rec->is_write ? 't' : 'f', spr_name(rec->spr), rec->spr);
    for (int i = 0; i < rec->n_ctx; i++) {
        g_string_append_printf(out, " [0x%" PRIx64 "]=0x%08x",
                               rec->ctx[i].vaddr, rec->ctx[i].bytes);
    }
    g_string_append(out, "\n");
    qemu_plugin_outs(out->str);
    g_string_free(out, TRUE);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint32_t raw = 0;
        unsigned opcode, xo, spr_lo, spr_hi, spr;
        int is_write;

        if (qemu_plugin_insn_data(insn, &raw, sizeof(raw)) != sizeof(raw)) {
            continue;
        }
        /* insn_data copies raw guest bytes (big-endian on PPC); the host
         * here is little-endian, so the naive uint32_t is byte-swapped. */
        raw = GUINT32_FROM_BE(raw);
        opcode = (raw >> 26) & 0x3F;
        if (opcode != 31) {
            continue;
        }
        xo = (raw >> 1) & 0x3FF;
        if (xo == 339) {
            is_write = 0;
        } else if (xo == 467) {
            is_write = 1;
        } else {
            continue;
        }
        spr_lo = (raw >> 16) & 0x1F;
        spr_hi = (raw >> 11) & 0x1F;
        spr = (spr_hi << 5) | spr_lo;
        if (spr != SPR_HID0 && spr != SPR_HID1 &&
            spr != SPR_MSSCR0 && spr != SPR_L2CR) {
            continue;
        }

        struct insn_rec *rec = g_new0(struct insn_rec, 1);
        rec->vaddr = qemu_plugin_insn_vaddr(insn);
        rec->spr = spr;
        rec->is_write = is_write;

        /* Snapshot a window of neighboring instructions from this same
         * TB, captured now (translation time) rather than re-read later:
         * the address range this PC lives in can be repurposed for
         * different code by the time any later inspection would run. */
        size_t lo = (i >= 2) ? i - 2 : 0;
        size_t hi = (i + 4 <= n) ? i + 4 : n;
        rec->n_ctx = 0;
        for (size_t j = lo; j < hi && rec->n_ctx < CTX_WINDOW; j++) {
            struct qemu_plugin_insn *ci = qemu_plugin_tb_get_insn(tb, j);
            uint32_t craw = 0;

            if (qemu_plugin_insn_data(ci, &craw, sizeof(craw)) == sizeof(craw)) {
                rec->ctx[rec->n_ctx].vaddr = qemu_plugin_insn_vaddr(ci);
                rec->ctx[rec->n_ctx].bytes = GUINT32_FROM_BE(craw);
                rec->n_ctx++;
            }
        }

        qemu_plugin_register_vcpu_insn_exec_cb(insn, vcpu_insn_exec,
                                               QEMU_PLUGIN_CB_NO_REGS, rec);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_plugin_outs("sprwatch: installed\n");
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
