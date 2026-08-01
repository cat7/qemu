/*
 * fbwatch: record which bytes of a physical address window the guest
 * CPU actually writes, to distinguish "the guest never painted this"
 * from "the guest painted it and the device model lost it".
 *
 * Builds a byte-granular coverage bitmap over [base, base+size) and,
 * on exit, prints per-row run lengths of written vs untouched bytes
 * for a caller-chosen framebuffer geometry.
 *
 * Args: base=<hex>,size=<dec>,pitch=<dec>,rows=<dec>,row=<dec>
 */
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t win_base;
static uint64_t win_size = 4 * 1024 * 1024;
static uint64_t pitch = 2304;
static uint64_t rows = 200;
static uint64_t report_row = 40;
static uint8_t *covered;
static uint8_t *shadow;
static const char *dumpfile;
static uint64_t nwrites;
static uint64_t burst;
static int64_t burst_start;
static int64_t last_write;
static GMutex lock;

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa, off;
    unsigned size;

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
    off = pa - win_base;
    size = 1u << qemu_plugin_mem_size_shift(info);

    {
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        uint64_t data = 0;
        unsigned n = size;

        switch (v.type) {
        case QEMU_PLUGIN_MEM_VALUE_U8:  data = v.data.u8;  break;
        case QEMU_PLUGIN_MEM_VALUE_U16: data = v.data.u16; break;
        case QEMU_PLUGIN_MEM_VALUE_U32: data = v.data.u32; break;
        case QEMU_PLUGIN_MEM_VALUE_U64: data = v.data.u64; break;
        default: n = 0; break;
        }

        g_mutex_lock(&lock);
        nwrites++;
        {
            /*
             * Report bursts: a run of framebuffer stores separated by
             * >150ms of quiet. A guest-side scroll copy shows up as one
             * large burst; if a scroll produces no burst, the guest
             * never attempted to move the pixels.
             */
            int64_t now = g_get_monotonic_time();
            if (last_write && now - last_write > 150000 && burst) {
                qemu_plugin_outs(g_strdup_printf(
                    "fbwatch: burst of %" PRIu64 " stores over %" PRId64 " ms\n",
                    burst, (last_write - burst_start) / 1000));
                burst = 0;
            }
            if (!burst) {
                burst_start = now;
            }
            burst++;
            last_write = now;
        }
        /* guest is big-endian: byte 0 of the store is the MSB */
        for (unsigned i = 0; i < n && off + i < win_size; i++) {
            covered[off + i] = 1;
            shadow[off + i] = (data >> (8 * (n - 1 - i))) & 0xff;
        }
        g_mutex_unlock(&lock);
    }
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_W, NULL);
    }
}

static void plugin_exit(void *udata)
{
    GString *out = g_string_new(NULL);
    uint64_t r, off, total = 0;

    for (off = 0; off < win_size; off++) {
        total += covered[off];
    }
    g_string_append_printf(out, "fbwatch: %" PRIu64 " CPU stores, %" PRIu64
                           " of %" PRIu64 " bytes written\n",
                           nwrites, total, win_size);

    /* coarse map: which 64KB blocks of the window were written */
    {
        uint64_t blk = 64 * 1024, i, j;
        g_string_append_printf(out, "  written 64KB blocks (offset : bytes):\n");
        for (i = 0; i < win_size; i += blk) {
            uint64_t c = 0;
            for (j = i; j < i + blk && j < win_size; j++) {
                c += covered[j];
            }
            if (c) {
                g_string_append_printf(out, "    0x%06" PRIx64 " : %" PRIu64 "\n",
                                       i, c);
            }
        }
    }

    /* per-row written-byte counts */
    for (r = 0; r < rows; r++) {
        uint64_t c = 0, i;
        for (i = 0; i < pitch && r * pitch + i < win_size; i++) {
            c += covered[r * pitch + i];
        }
        if (r < 8 || (r % 10) == 0 || (c == 0 && r < 200)) {
            g_string_append_printf(out, "  row %4" PRIu64 ": %" PRIu64
                                   " bytes written\n", r, c);
        }
    }

    /* run-length map of one row: were the on-screen gaps ever written? */
    g_string_append_printf(out, "  row %" PRIu64 " runs (state,startbyte,len):\n",
                           report_row);
    {
        uint64_t i, start = 0;
        int cur = -1;
        for (i = 0; i <= pitch; i++) {
            int v = (i < pitch) ? covered[report_row * pitch + i] : -2;
            if (v != cur) {
                if (cur >= 0) {
                    g_string_append_printf(out, "    %s %5" PRIu64 " %5" PRIu64 "\n",
                                           cur ? "WRITTEN" : "untouched",
                                           start, i - start);
                }
                cur = v; start = i;
            }
        }
    }
    if (dumpfile) {
        FILE *fp = fopen(dumpfile, "wb");
        if (fp) {
            fwrite(shadow, 1, win_size, fp);
            fclose(fp);
            g_string_append_printf(out, "  shadow written to %s\n", dumpfile);
        }
    }
    qemu_plugin_outs(out->str);
    g_string_free(out, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *p = strchr(argv[i], '=');
        if (!p) {
            continue;
        }
        *p++ = 0;
        if (!strcmp(argv[i], "base")) {
            win_base = strtoull(p, NULL, 0);
        } else if (!strcmp(argv[i], "size")) {
            win_size = strtoull(p, NULL, 0);
        } else if (!strcmp(argv[i], "pitch")) {
            pitch = strtoull(p, NULL, 0);
        } else if (!strcmp(argv[i], "rows")) {
            rows = strtoull(p, NULL, 0);
        } else if (!strcmp(argv[i], "row")) {
            report_row = strtoull(p, NULL, 0);
        } else if (!strcmp(argv[i], "dump")) {
            dumpfile = g_strdup(p);
        }
    }
    covered = g_malloc0(win_size);
    shadow = g_malloc0(win_size);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
