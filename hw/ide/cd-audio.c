/*
 * ATAPI CD-DA play through the drive's analog output
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/audio.h"
#include "system/block-backend.h"
#include "ide-internal.h"

#define CDA_FRAMES_PER_SEC  75
#define CDA_RING_SECTORS    75
#define CDA_READ_SECTORS    15

typedef struct IDECDAudio {
    IDEState *s;
    AudioBackend *be;
    SWVoiceOut *voice;
    QEMUTimer *timer;       /* end of play when there is no voice */

    uint8_t status;
    bool playing;
    bool paused;
    bool draining;
    uint32_t pos;           /* sector being output */
    uint32_t end;           /* first sector not played */
    uint32_t off;           /* bytes of @pos already output */

    /* no voice: position follows the clock */
    int64_t clock0;
    uint32_t pos0;

    /* read-ahead of the sectors from @pos */
    uint8_t *ring;
    uint32_t ring_head;
    uint32_t ring_count;
    uint32_t fetch;         /* next sector to read */
    uint32_t reading;       /* sectors in flight */
    uint32_t generation;
    uint32_t read_gen;
    uint8_t *rbuf;
    QEMUIOVector qiov;
} IDECDAudio;

static void cda_fill(IDECDAudio *a);

static void cda_voice_off(IDECDAudio *a)
{
    a->draining = false;
    audio_be_set_active_out(a->be, a->voice, false);
    timer_del(a->timer);
}

static void cda_finish(IDECDAudio *a, uint8_t status)
{
    a->playing = false;
    a->paused = false;
    a->status = status;
    a->generation++;
    if (a->voice) {
        cda_voice_off(a);
    }
    timer_del(a->timer);
}

/* Play reached its end; the voice runs until its buffer has played out. */
static void cda_complete(IDECDAudio *a)
{
    a->playing = false;
    a->status = CD_AUDIO_STATUS_COMPLETED;
    a->generation++;
    if (a->voice) {
        a->draining = true;
        timer_mod(a->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                            NANOSECONDS_PER_SECOND);
    }
}

static void cda_read_done(void *opaque, int ret)
{
    IDECDAudio *a = opaque;
    uint32_t n = a->reading;
    uint32_t i;

    a->reading = 0;
    if (!a->playing || a->read_gen != a->generation) {
        cda_fill(a);
        return;
    }
    if (ret < 0) {
        cda_finish(a, CD_AUDIO_STATUS_ERROR);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t slot = (a->ring_head + a->ring_count) % CDA_RING_SECTORS;

        memcpy(a->ring + slot * CD_RAW_SECTOR_SIZE,
               a->rbuf + i * CD_RAW_SECTOR_SIZE, CD_RAW_SECTOR_SIZE);
        a->ring_count++;
    }
    a->fetch += n;
    cda_fill(a);
}

static void cda_fill(IDECDAudio *a)
{
    uint32_t n;

    if (!a->playing || a->reading || !a->voice) {
        return;
    }
    n = MIN(CDA_READ_SECTORS, a->end - a->fetch);
    if (n == 0 || CDA_RING_SECTORS - a->ring_count < n) {
        return;
    }
    a->reading = n;
    a->read_gen = a->generation;
    qemu_iovec_init_buf(&a->qiov, a->rbuf, n * CD_RAW_SECTOR_SIZE);
    blk_aio_cd_read_raw(a->s->blk, a->fetch, n, &a->qiov, cda_read_done, a);
}

static int16_t cda_mix(const uint8_t *frame, uint8_t sel, uint8_t vol)
{
    int l = (int16_t)lduw_le_p(frame);
    int r = (int16_t)lduw_le_p(frame + 2);
    int v = 0;

    if (sel & 1) {
        v += l;
    }
    if (sel & 2) {
        v += r;
    }
    v = v * vol / 255;
    return MAX(MIN(v, INT16_MAX), INT16_MIN);
}

static void cda_out(void *opaque, int free)
{
    IDECDAudio *a = opaque;
    const uint8_t *page = a->s->cd_audio_page;
    uint8_t tmp[CD_RAW_SECTOR_SIZE];

    if (a->draining) {
        if (free >= audio_be_get_buffer_size_out(a->be, a->voice)) {
            cda_voice_off(a);
        }
        return;
    }

    while (a->playing && !a->paused && free >= 4 && a->ring_count > 0) {
        const uint8_t *src = a->ring + a->ring_head * CD_RAW_SECTOR_SIZE +
                             a->off;
        int len = MIN(free, CD_RAW_SECTOR_SIZE - a->off) & ~3;
        int i;
        size_t n;

        for (i = 0; i < len; i += 4) {
            stw_le_p(tmp + i, cda_mix(src + i, page[8] & 0x0f, page[9]));
            stw_le_p(tmp + i + 2, cda_mix(src + i, page[10] & 0x0f, page[11]));
        }
        n = audio_be_write(a->be, a->voice, tmp, len) & ~3;
        if (n == 0) {
            break;
        }
        free -= n;
        a->off += n;
        if (a->off == CD_RAW_SECTOR_SIZE) {
            a->off = 0;
            a->ring_head = (a->ring_head + 1) % CDA_RING_SECTORS;
            a->ring_count--;
            if (++a->pos == a->end) {
                cda_complete(a);
                return;
            }
        }
    }
    cda_fill(a);
}

static void cda_timer(void *opaque)
{
    IDECDAudio *a = opaque;

    if (a->voice) {
        cda_voice_off(a);
        return;
    }
    a->pos = a->end;
    cda_complete(a);
}

static void cda_clock_start(IDECDAudio *a)
{
    a->clock0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    a->pos0 = a->pos;
    timer_mod(a->timer, a->clock0 + muldiv64(a->end - a->pos,
                                             NANOSECONDS_PER_SECOND,
                                             CDA_FRAMES_PER_SEC));
}

static uint32_t cda_clock_pos(IDECDAudio *a)
{
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - a->clock0;
    uint64_t n = muldiv64(t, CDA_FRAMES_PER_SEC, NANOSECONDS_PER_SECOND);

    return MIN(a->pos0 + n, a->end);
}

void ide_cd_audio_init(IDEState *s, AudioBackend *be)
{
    IDECDAudio *a = g_new0(IDECDAudio, 1);

    a->s = s;
    a->status = CD_AUDIO_STATUS_NONE;
    a->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cda_timer, a);
    if (be) {
        struct audsettings as = {
            .freq = 44100,
            .nchannels = 2,
            .fmt = AUDIO_FORMAT_S16,
            .big_endian = false,
        };

        a->be = be;
        a->voice = audio_be_open_out(be, NULL, "ide-cd", a, cda_out, &as);
        a->ring = g_malloc(CDA_RING_SECTORS * CD_RAW_SECTOR_SIZE);
        a->rbuf = blk_blockalign(s->blk, CDA_READ_SECTORS * CD_RAW_SECTOR_SIZE);
    }
    s->cd_audio = a;
}

bool ide_cd_audio_has_output(IDEState *s)
{
    return s->cd_audio && s->cd_audio->voice;
}

void ide_cd_audio_play(IDEState *s, uint32_t start, uint32_t end)
{
    IDECDAudio *a = s->cd_audio;

    cda_finish(a, CD_AUDIO_STATUS_NONE);
    a->pos = start;
    a->end = end;
    if (start >= end) {
        return;
    }
    a->playing = true;
    a->status = CD_AUDIO_STATUS_PLAYING;
    if (a->voice) {
        a->off = 0;
        a->ring_head = 0;
        a->ring_count = 0;
        a->fetch = start;
        cda_fill(a);
        audio_be_set_active_out(a->be, a->voice, true);
    } else {
        cda_clock_start(a);
    }
}

bool ide_cd_audio_pause(IDEState *s, bool pause)
{
    IDECDAudio *a = s->cd_audio;

    if (!a->playing) {
        return false;
    }
    if (pause == a->paused) {
        return true;
    }
    a->paused = pause;
    a->status = pause ? CD_AUDIO_STATUS_PAUSED : CD_AUDIO_STATUS_PLAYING;
    if (a->voice) {
        audio_be_set_active_out(a->be, a->voice, !pause);
    } else if (pause) {
        a->pos = cda_clock_pos(a);
        timer_del(a->timer);
    } else {
        cda_clock_start(a);
    }
    return true;
}

void ide_cd_audio_stop(IDEState *s)
{
    IDECDAudio *a = s->cd_audio;

    if (!a) {
        return;
    }
    if (a->playing) {
        a->pos = ide_cd_audio_position(s);
        cda_finish(a, CD_AUDIO_STATUS_NONE);
    }
}

uint32_t ide_cd_audio_position(IDEState *s)
{
    IDECDAudio *a = s->cd_audio;

    if (a->playing && !a->paused && !a->voice) {
        return cda_clock_pos(a);
    }
    return a->pos;
}

uint8_t ide_cd_audio_status(IDEState *s)
{
    IDECDAudio *a = s->cd_audio;
    uint8_t st = a->status;

    if (st == CD_AUDIO_STATUS_COMPLETED || st == CD_AUDIO_STATUS_ERROR) {
        a->status = CD_AUDIO_STATUS_NONE;
    }
    return st;
}
