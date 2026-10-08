/*
 * CD track layout helpers shared by host CD drivers and images
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "block/cdrom.h"

static uint32_t cd_msf_to_lba(const uint8_t *msf)
{
    return (msf[0] * 60 + msf[1]) * 75 + msf[2] - 150;
}

static uint8_t cd_to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

int cd_toc_parse_full(const uint8_t *buf, int len, CDToc *toc)
{
    uint8_t disc_type = 0;
    uint32_t leadout = 0;
    int i, first = 0, last = 0;
    CDTrack tracks[CD_MAX_TRACKS + 1] = { 0 };
    bool seen[CD_MAX_TRACKS + 1] = { false };

    if (len < 4) {
        return -EINVAL;
    }
    len = MIN(lduw_be_p(buf) + 2, len);
    for (i = 4; i + 11 <= len; i += 11) {
        const uint8_t *d = buf + i;
        uint8_t adr = d[1] >> 4, point = d[3];

        if (adr != 1) {
            continue;
        }
        if (point == 0xa0 && d[0] == 1) {
            disc_type = d[9];
        } else if (point == 0xa2) {
            leadout = cd_msf_to_lba(d + 8);
        } else if (point >= 1 && point <= CD_MAX_TRACKS) {
            tracks[point].control = d[1] & 0x0f;
            tracks[point].start = cd_msf_to_lba(d + 8);
            seen[point] = true;
            first = first ? MIN(first, point) : point;
            last = MAX(last, point);
        }
    }
    if (!first || !leadout) {
        return -EINVAL;
    }

    memset(toc, 0, sizeof(*toc));
    toc->first = first;
    toc->leadout = leadout;
    for (i = first; i <= last; i++) {
        CDTrack *t = &toc->tracks[toc->nb_tracks];

        if (!seen[i]) {
            return -EINVAL;
        }
        *t = tracks[i];
        t->index0 = toc->nb_tracks ? t->start : 0;
        if (!(t->control & CD_CTRL_DATA)) {
            t->mode = CD_TRACK_AUDIO;
        } else {
            t->mode = disc_type == 0x20 ? CD_TRACK_MODE2 : CD_TRACK_MODE1;
        }
        if (toc->nb_tracks) {
            toc->tracks[toc->nb_tracks - 1].end = t->start;
        }
        toc->nb_tracks++;
    }
    toc->tracks[toc->nb_tracks - 1].end = leadout;
    return 0;
}

bool cd_toc_has_audio(const CDToc *toc)
{
    int i;

    for (i = 0; i < toc->nb_tracks; i++) {
        if (toc->tracks[i].mode == CD_TRACK_AUDIO) {
            return true;
        }
    }
    return false;
}

void cd_raw_header(uint8_t *buf, uint32_t lba, int mode)
{
    uint32_t a = lba + 150;

    buf[0] = 0x00;
    memset(buf + 1, 0xff, 10);
    buf[11] = 0x00;
    buf[12] = cd_to_bcd(a / 75 / 60);
    buf[13] = cd_to_bcd((a / 75) % 60);
    buf[14] = cd_to_bcd(a % 75);
    buf[15] = mode;
}
