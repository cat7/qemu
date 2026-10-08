/*
 * CD track layout reported by block drivers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BLOCK_CDROM_H
#define BLOCK_CDROM_H

/* Bytes in a raw CD sector: CD-DA frame or sync+header+data+EDC/ECC */
#define CD_RAW_SECTOR_SIZE  2352
#define CD_DATA_SECTOR_SIZE 2048
#define CD_MAX_TRACKS       99

/* Q sub-channel CONTROL bits */
#define CD_CTRL_PREEMPHASIS 0x1
#define CD_CTRL_COPY        0x2
#define CD_CTRL_DATA        0x4
#define CD_CTRL_4CH         0x8

typedef enum CDTrackMode {
    CD_TRACK_AUDIO,
    CD_TRACK_MODE1,
    CD_TRACK_MODE2,
} CDTrackMode;

typedef struct CDTrack {
    uint8_t mode;       /* CDTrackMode */
    uint8_t control;    /* Q sub-channel CONTROL nibble */
    uint32_t index0;    /* LBA of index 0 (equals start without a pregap) */
    uint32_t start;     /* LBA of index 1 */
    uint32_t end;       /* first LBA after the track */
} CDTrack;

/*
 * Single-session layout. Track i has number first + i.
 * LBA 0 is MSF 00:02:00.
 */
typedef struct CDToc {
    uint8_t first;
    uint8_t nb_tracks;
    uint32_t leadout;
    CDTrack tracks[CD_MAX_TRACKS];
} CDToc;

/* Index into toc->tracks of the track holding @lba, or -1. */
static inline int cd_toc_find(const CDToc *toc, uint32_t lba)
{
    int i;

    for (i = 0; i < toc->nb_tracks; i++) {
        if (lba >= toc->tracks[i].index0 && lba < toc->tracks[i].end) {
            return i;
        }
    }
    return -1;
}

#endif
