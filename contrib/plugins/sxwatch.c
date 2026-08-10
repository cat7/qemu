/*
 * sxwatch: trace XNU 10.3 (xnu-517) kernel-pmap shared/exclusive search
 * lock traffic and mapSearch calls, to catch the transient mapSearch miss
 * behind the -smp 2 "Unresolved kernel trap" panic. All guest addresses
 * are the fixed V=P addresses of this specific mach_kernel build:
 *
 *   0x8c080 sxlkConvert    (success isync at 0x8c09c)
 *   0x8c0c0 sxlkPromote    (success isync at 0x8c0d8)
 *   0x8c100 sxlkExclusive  (success isync at 0x8c118)
 *   0x8c160 sxlkShared     (success isync at 0x8c178)
 *   0x8c1c0 sxlkUnlock     (entry eieio)
 *   0x8c3a0 mapSearch      (entry; r3 = pmap, r5 = VA low half)
 *   0x8ad00 hpfNotFound    (handlePF punts fault to trap.c/vm_fault)
 *   0x8ace0 hpfBadLock     (handlePF lock failure -> Choke)
 *
 * Lock events are filtered to the kernel pmap (struct at 0x2e4000,
 * pmapSXlk at +0x10) so user-pmap traffic stays out of the log.
 * Attach with: -plugin libsxwatch.dylib
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define KPMAP    0x2e4000
#define KPMAP_SX 0x2e4010
#define KMAP     0x8eef6c   /* kernel_map (vm_map_t value, stable across boots) */

enum ev {
    EV_SH, EV_EX, EV_CV, EV_PM, EV_UN, EV_MS, EV_NF, EV_BL, EV_VF, EV_VO,
    EV_AD, EV_RM,
};

static const struct { uint64_t pc; enum ev ev; const char *tag; } watch[] = {
    { 0x8c178, EV_SH, "SH+" },
    { 0x8c118, EV_EX, "EX+" },
    { 0x8c09c, EV_CV, "CV+" },
    { 0x8c0d8, EV_PM, "PM+" },
    { 0x8c1c0, EV_UN, "UN " },
    { 0x8c3a0, EV_MS, "MS " },
    { 0x8ad00, EV_NF, "NF " },
    { 0x8ace0, EV_BL, "BAD" },
    { 0x58c30, EV_VF, "VF " },  /* vm_fault preemption cmpwi: r25=map r4=va r3=level */
    { 0x5a104, EV_VO, "VFO" },  /* vm_fault shared epilogue: r25=map r21=va r3=retcode */
    { 0x88880, EV_AD, "ADD" },  /* hw_add_map entry: r3=pmap r4=mapping (VA at +0x14) */
    { 0x88bc0, EV_RM, "REM" },  /* hw_rem_map entry: r3=pmap r5=va low */
};

static struct qemu_plugin_register *r3_handle;
static struct qemu_plugin_register *r4_handle;
static struct qemu_plugin_register *r5_handle;
static struct qemu_plugin_register *r21_handle;
static struct qemu_plugin_register *r25_handle;
static struct qemu_plugin_register *lr_handle;

static void find_regs(void)
{
    GArray *regs = qemu_plugin_get_registers();

    if (!regs) {
        return;
    }
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (d->name && !g_strcmp0(d->name, "r3")) {
            r3_handle = d->handle;
        } else if (d->name && !g_strcmp0(d->name, "r4")) {
            r4_handle = d->handle;
        } else if (d->name && !g_strcmp0(d->name, "r5")) {
            r5_handle = d->handle;
        } else if (d->name && !g_strcmp0(d->name, "r21")) {
            r21_handle = d->handle;
        } else if (d->name && !g_strcmp0(d->name, "r25")) {
            r25_handle = d->handle;
        } else if (d->name && !g_strcmp0(d->name, "lr")) {
            lr_handle = d->handle;
        }
    }
}

static uint64_t read_reg(struct qemu_plugin_register *h)
{
    uint64_t v = 0;

    if (!h) {
        return 0;
    }
    GByteArray *buf = g_byte_array_new();
    if (qemu_plugin_read_register(h, buf) > 0) {
        for (guint i = 0; i < buf->len; i++) {
            v = (v << 8) | buf->data[i];
        }
    }
    g_byte_array_free(buf, TRUE);
    return v;
}

static void vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    uintptr_t idx = (uintptr_t)udata;
    uint64_t r3;

    if (!r3_handle) {
        find_regs();
    }

    switch (watch[idx].ev) {
    case EV_SH: case EV_EX: case EV_CV: case EV_PM: case EV_UN:
        r3 = read_reg(r3_handle);
        if (r3 != KPMAP_SX) {
            return;
        }
        fprintf(stderr, "SX c%u %s\n", cpu_index, watch[idx].tag);
        break;
    case EV_MS:
        r3 = read_reg(r3_handle);
        if (r3 != KPMAP) {
            return;
        }
        fprintf(stderr, "SX c%u MS  va=%08" PRIx64 "\n",
                cpu_index, read_reg(r5_handle));
        break;
    case EV_NF:
    case EV_BL:
        fprintf(stderr, "SX c%u %s lr=%08" PRIx64 "\n",
                cpu_index, watch[idx].tag, read_reg(lr_handle));
        break;
    case EV_VF: {
        uint64_t lvl = read_reg(r3_handle);
        uint64_t map = read_reg(r25_handle);
        if (lvl == 0 && map != KMAP) {
            return;
        }
        fprintf(stderr, "SX c%u VF  va=%08" PRIx64 " lvl=%" PRIu64
                " map=%08" PRIx64 "\n",
                cpu_index, read_reg(r4_handle), lvl, map);
        break;
    }
    case EV_VO:
        r3 = read_reg(r3_handle);
        if (r3 == 0) {
            return;
        }
        fprintf(stderr, "SX c%u VFAIL va=%08" PRIx64 " rc=%" PRIu64
                " map=%08" PRIx64 "\n",
                cpu_index, read_reg(r21_handle), r3, read_reg(r25_handle));
        break;
    case EV_AD: {
        if (read_reg(r3_handle) != KPMAP) {
            return;
        }
        uint64_t mp = read_reg(r4_handle);
        uint32_t va = 0;
        GByteArray *buf = g_byte_array_new();
        if (qemu_plugin_read_memory_vaddr(mp + 0x14, buf, 4) && buf->len == 4) {
            va = ((uint32_t)buf->data[0] << 24) | (buf->data[1] << 16) |
                 (buf->data[2] << 8) | buf->data[3];
        }
        g_byte_array_free(buf, TRUE);
        fprintf(stderr, "SX c%u ADD va=%08x\n", cpu_index, va);
        break;
    }
    case EV_RM:
        if (read_reg(r3_handle) != KPMAP) {
            return;
        }
        fprintf(stderr, "SX c%u REM va=%08" PRIx64 "\n",
                cpu_index, read_reg(r5_handle));
        break;
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);

        for (size_t t = 0; t < G_N_ELEMENTS(watch); t++) {
            if (watch[t].pc == vaddr) {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_insn_exec, QEMU_PLUGIN_CB_R_REGS,
                    (void *)(uintptr_t)t);
                break;
            }
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
