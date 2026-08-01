/*
 * excburst: group guest exceptions into bursts so they can be
 * correlated with a single user action.
 *
 * Every exception is counted per PowerPC vector offset (0x300 DSI,
 * 0x400 ISI, 0x600 alignment, 0x700 program, 0x800 FP unavailable,
 * 0x900 decrementer, ...). A run of exceptions separated by more than
 * `gap` milliseconds of quiet is reported as one burst, listing the
 * per-vector counts. Idle-time interrupts (decrementer, external) are
 * counted but do not by themselves start or extend a burst, so a burst
 * line corresponds to real work.
 *
 * Args: gap=<ms, default 150>, outfile=<path>
 */
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define NVEC 32                 /* vector >> 8, covers 0x000-0x1F00 */

static GMutex lock;
static uint64_t cnt[NVEC];      /* counts in the current burst */
static uint64_t total[NVEC];    /* counts over the whole run */
static uint64_t burst_events;
static int64_t last_event;
static int64_t gap_us = 150000;
static FILE *out_fp;

static bool is_background(uint64_t vec)
{
    /* decrementer and external interrupts tick constantly */
    return vec == 0x900 || vec == 0x500;
}

static void emit_burst(void)
{
    GString *s = g_string_new("excburst: ");
    int i;

    g_string_append_printf(s, "%" PRIu64 " exceptions [", burst_events);
    for (i = 0; i < NVEC; i++) {
        if (cnt[i]) {
            g_string_append_printf(s, " 0x%03x:%" PRIu64, i << 8, cnt[i]);
        }
    }
    g_string_append(s, " ]\n");
    if (out_fp) {
        fputs(s->str, out_fp);
        fflush(out_fp);
    } else {
        qemu_plugin_outs(s->str);
    }
    g_string_free(s, TRUE);
    memset(cnt, 0, sizeof(cnt));
    burst_events = 0;
}

static void vcpu_discon(unsigned int vcpu_index,
                        enum qemu_plugin_discon_type type,
                        uint64_t from_pc, uint64_t to_pc, void *udata)
{
    uint64_t vec = to_pc & 0x1ff00;
    int slot = (vec >> 8) & (NVEC - 1);
    int64_t now = g_get_monotonic_time();

    g_mutex_lock(&lock);
    total[slot]++;
    if (!is_background(vec)) {
        if (burst_events && now - last_event > gap_us) {
            emit_burst();
        }
        burst_events++;
        cnt[slot]++;
        last_event = now;
    } else if (burst_events) {
        cnt[slot]++;
    }
    g_mutex_unlock(&lock);
}

static void plugin_exit(void *udata)
{
    GString *s = g_string_new("excburst TOTALS:");
    int i;

    if (burst_events) {
        emit_burst();
    }
    for (i = 0; i < NVEC; i++) {
        if (total[i]) {
            g_string_append_printf(s, " 0x%03x:%" PRIu64, i << 8, total[i]);
        }
    }
    g_string_append_c(s, '\n');
    if (out_fp) {
        fputs(s->str, out_fp);
        fclose(out_fp);
    } else {
        qemu_plugin_outs(s->str);
    }
    g_string_free(s, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tok = g_strsplit(argv[i], "=", 2);
        if (!g_strcmp0(tok[0], "outfile") && tok[1]) {
            out_fp = fopen(tok[1], "w");
        } else if (!g_strcmp0(tok[0], "gap") && tok[1]) {
            gap_us = strtoull(tok[1], NULL, 0) * 1000;
        }
    }
    qemu_plugin_register_vcpu_discon_cb(id, QEMU_PLUGIN_DISCON_EXCEPTION,
                                        vcpu_discon, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
