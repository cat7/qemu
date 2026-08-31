/*
 * viastretch: model 6522 VIA bus-cycle stretching.
 *
 * Real Mac hardware synchronizes every VIA register access to the VIA
 * clock (4.7MHz/6 = ~783kHz): each access occupies ~1.276us of bus
 * time, regardless of CPU speed. Classic Mac ROM code depends on this
 * as a *timing primitive* -- the Mac OS ROM Trampoline calibrates the
 * timebase/bus frequencies by executing a fixed number of VIA reads
 * (~780 = one nominal millisecond of bus time) and counting timebase
 * ticks across them (the "TimeVIADB" idiom). QEMU's instantaneous MMIO
 * makes that window collapse ~700x, poisoning the calibration.
 *
 * This plugin busy-waits the balance of `ns` nanoseconds of host wall
 * time on every guest access to the given physical range. Under TCG
 * with the default wall-clock-driven virtual clock, that makes each
 * access consume the same virtual time it would on hardware.
 *
 * Args: base=<phys>,len=<bytes>[,ns=<per-access ns, default 1276>]
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t base;
static uint64_t len;
static uint64_t stretch_ns = 1276;

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);
    uint64_t pa, end;

    if (!hw || qemu_plugin_hwaddr_is_io(hw) == false) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    {
        static int dbg;
        if (dbg < 8) {
            dbg++;
            fprintf(stderr, "viastretch dbg: io access vaddr=%llx pa=%llx\n",
                    (unsigned long long)vaddr, (unsigned long long)pa);
        }
    }
    if (pa < base || pa >= base + len) {
        return;
    }

    /* Busy-wait: precise at the ~1us scale where nanosleep overshoots. */
    end = now_ns() + stretch_ns;
    while (now_ns() < end) {
        /* spin */
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, NULL);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        char **tokens = g_strsplit(argv[i], "=", 2);

        if (g_strcmp0(tokens[0], "base") == 0) {
            base = g_ascii_strtoull(tokens[1], NULL, 0);
        } else if (g_strcmp0(tokens[0], "len") == 0) {
            len = g_ascii_strtoull(tokens[1], NULL, 0);
        } else if (g_strcmp0(tokens[0], "ns") == 0) {
            stretch_ns = g_ascii_strtoull(tokens[1], NULL, 0);
        } else {
            fprintf(stderr, "viastretch: bad arg %s\n", argv[i]);
            g_strfreev(tokens);
            return -1;
        }
        g_strfreev(tokens);
    }
    if (!len) {
        fprintf(stderr, "viastretch: base=/len= required\n");
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
