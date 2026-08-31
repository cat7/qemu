/*
 * msgwatch - report which strings in a table the guest actually reads.
 *
 * The Mac OS ROM carries a table of "MacOS: ..." diagnostics and prints
 * them through Open Firmware's console. When that console is the screen
 * there is nothing to capture, so instead watch the table itself: the only
 * reason to read one of those bytes is to print it. Whichever message gets
 * touched is the one the boot is complaining about.
 *
 * Usage: -plugin ./libmsgwatch.dylib,base=0xf0000,len=0x30000
 *
 * This file is licensed under the GPL v2 or later.
 */
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t base;
static uint64_t len;
static GHashTable *seen;       /* start-of-string hwaddr -> reported */
static uint64_t nreads;        /* positive control: total reads seen */
static uint64_t nhits;

/*
 * Walk back to the start of the printable run containing @pa, so a read
 * landing mid-message still identifies the whole message.
 */
static uint64_t string_start(uint64_t pa)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint64_t p = pa;
    int back = 0;

    while (p > base && back < 200) {
        g_byte_array_set_size(b, 0);
        if (!qemu_plugin_read_memory_hwaddr(p - 1, b, 1) || b->len < 1) {
            break;
        }
        if (b->data[0] < 0x20 || b->data[0] > 0x7e) {
            break;
        }
        p--;
        back++;
    }
    return p;
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa, start;
    g_autoptr(GByteArray) b = NULL;
    char txt[160];
    unsigned i;

    nreads++;

    if (qemu_plugin_mem_is_store(info)) {
        return;
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    if (pa < base || pa >= base + len) {
        return;
    }

    start = string_start(pa);
    if (g_hash_table_contains(seen, GUINT_TO_POINTER((guint)start))) {
        return;
    }
    g_hash_table_add(seen, GUINT_TO_POINTER((guint)start));

    b = g_byte_array_new();
    if (!qemu_plugin_read_memory_hwaddr(start, b, 140)) {
        return;
    }
    for (i = 0; i < b->len && i < sizeof(txt) - 1; i++) {
        if (b->data[i] < 0x20 || b->data[i] > 0x7e) {
            break;
        }
        txt[i] = (char)b->data[i];
    }
    txt[i] = '\0';
    if (i < 6) {
        return;                /* not a message, just stray data */
    }

    nhits++;
    fprintf(stderr, "msgwatch: READ @0x%08" PRIx64 " (vaddr 0x%08" PRIx64
            ") : %s\n", start, vaddr, txt);
    fflush(stderr);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_R, NULL);
    }
}

static void plugin_exit(void *p)
{
    fprintf(stderr, "msgwatch: %" PRIu64 " reads observed (control), "
            "%" PRIu64 " distinct strings in range\n", nreads, nhits);
    fflush(stderr);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int i;

    for (i = 0; i < argc; i++) {
        g_auto(GStrv) tok = g_strsplit(argv[i], "=", 2);

        if (g_strcmp0(tok[0], "base") == 0) {
            base = g_ascii_strtoull(tok[1], NULL, 0);
        } else if (g_strcmp0(tok[0], "len") == 0) {
            len = g_ascii_strtoull(tok[1], NULL, 0);
        } else {
            fprintf(stderr, "msgwatch: bad arg %s\n", argv[i]);
            return -1;
        }
    }
    if (!len) {
        fprintf(stderr, "msgwatch: base= and len= are required\n");
        return -1;
    }

    seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
