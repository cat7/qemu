/*
 * Block driver for CUE sheets with BINARY track files
 *
 * The image is the 2048-byte user data of every sector, in LBA order;
 * audio and gap sectors read as zeros. Raw 2352-byte sectors and the
 * track layout are available through bdrv_co_cd_read_raw() and
 * bdrv_get_cd_toc().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/memalign.h"
#include "qemu/bswap.h"
#include "qemu/units.h"

#define CUE_MAX_SIZE        (1 * MiB)
#define CUE_MAX_FILES       CD_MAX_TRACKS
#define CUE_BOUNCE_SECTORS  32

typedef struct CueExtent {
    uint32_t lba;
    uint32_t count;
    int file;           /* index into files[], -1 for a gap */
    uint64_t offset;    /* file offset of the first sector */
    uint16_t ssize;     /* bytes per sector in the file */
    uint8_t mode;       /* CDTrackMode */
} CueExtent;

typedef struct BDRVCueState {
    CDToc toc;
    int nb_files;
    BdrvChild *files[CUE_MAX_FILES];
    bool swap[CUE_MAX_FILES];   /* big-endian audio samples */
    int nb_ext;
    CueExtent *ext;
} BDRVCueState;

typedef struct CueTrackInfo {
    uint16_t ssize;
    uint32_t pregap;
    uint32_t postgap;
    bool started;
    bool has_index1;
} CueTrackInfo;

typedef struct CueParser {
    BlockDriverState *bs;
    BDRVCueState *s;
    QDict *options;
    char *dir;
    int file;
    int64_t file_len;
    uint64_t file_byte;     /* file offset of seg_frame */
    uint32_t seg_frame;     /* file frame where the open segment starts */
    int seg_track;          /* track owning the open segment */
    int track;              /* current track */
    uint32_t lba;
    CueTrackInfo info[CD_MAX_TRACKS];
} CueParser;

static int cue_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    size_t len;
    g_autofree char *text = NULL;

    if (!filename) {
        return 0;
    }
    len = strlen(filename);
    if (len < 4 || g_ascii_strcasecmp(filename + len - 4, ".cue")) {
        return 0;
    }
    text = g_ascii_strup((const char *)buf, buf_size);
    if (strstr(text, "FILE") && strstr(text, "TRACK")) {
        return 100;
    }
    return 0;
}

static void cue_add_extent(CueParser *p, int file, uint64_t offset,
                           uint32_t count, uint16_t ssize, uint8_t mode)
{
    BDRVCueState *s = p->s;
    CueExtent *e;

    if (!count) {
        return;
    }
    s->ext = g_renew(CueExtent, s->ext, s->nb_ext + 1);
    e = &s->ext[s->nb_ext++];
    *e = (CueExtent) {
        .lba = p->lba, .count = count, .file = file, .offset = offset,
        .ssize = ssize, .mode = mode,
    };
    p->lba += count;
}

/* Map file frames [seg_frame, frame) to the disc; -1 means to end of file. */
static int cue_flush(CueParser *p, int64_t frame, Error **errp)
{
    int owner = p->seg_track >= 0 ? p->seg_track : p->track;
    uint16_t ssize;
    uint64_t count;

    if (p->file < 0 || owner < 0) {
        return 0;
    }
    ssize = p->info[owner].ssize;
    if (frame < 0) {
        count = (p->file_len - p->file_byte) / ssize;
    } else {
        if (frame < p->seg_frame) {
            error_setg(errp, "cue: INDEX positions go backwards");
            return -EINVAL;
        }
        count = frame - p->seg_frame;
        if (p->file_byte + count * ssize > p->file_len) {
            error_setg(errp, "cue: INDEX beyond the end of the file");
            return -EINVAL;
        }
    }
    if (p->lba + count > 450000) {
        error_setg(errp, "cue: image too large");
        return -EINVAL;
    }
    cue_add_extent(p, p->file, p->file_byte, count, ssize,
                   p->s->toc.tracks[owner].mode);
    p->file_byte += count * ssize;
    p->seg_frame += count;
    return 0;
}

static void cue_end_track(CueParser *p, int t)
{
    CDTrack *tr = &p->s->toc.tracks[t];

    p->seg_track = -1;
    cue_add_extent(p, -1, 0, p->info[t].postgap, CD_RAW_SECTOR_SIZE, tr->mode);
    tr->end = p->lba;
}

static bool cue_parse_msf(const char *str, uint32_t *frames)
{
    unsigned m, sec, f;
    char tail;

    if (sscanf(str, "%u:%u:%u%c", &m, &sec, &f, &tail) != 3 ||
        sec >= 60 || f >= 75 || m > 99) {
        return false;
    }
    *frames = (m * 60 + sec) * 75 + f;
    return true;
}

/* Split a line into whitespace separated words; quotes group words. */
static int cue_split(char *line, char **argv, int max)
{
    int argc = 0;
    char *q = line;

    while (*q && argc < max) {
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (!*q) {
            break;
        }
        if (*q == '"') {
            argv[argc++] = ++q;
            while (*q && *q != '"') {
                q++;
            }
        } else {
            argv[argc++] = q;
            while (*q && *q != ' ' && *q != '\t') {
                q++;
            }
        }
        if (*q) {
            *q++ = '\0';
        }
    }
    return argc;
}

static int GRAPH_UNLOCKED cue_cmd_file(CueParser *p, int argc, char **argv,
                                       Error **errp)
{
    BDRVCueState *s = p->s;
    g_autofree char *path = NULL;
    char prefix[16];
    BdrvChild *child;
    int ret;

    if (argc < 3) {
        error_setg(errp, "cue: FILE needs a name and a type");
        return -EINVAL;
    }
    if (g_ascii_strcasecmp(argv[2], "BINARY") &&
        g_ascii_strcasecmp(argv[2], "MOTOROLA")) {
        error_setg(errp, "cue: unsupported file type '%s'", argv[2]);
        return -ENOTSUP;
    }
    if (s->nb_files == CUE_MAX_FILES) {
        error_setg(errp, "cue: too many files");
        return -EINVAL;
    }

    ret = cue_flush(p, -1, errp);
    if (ret < 0) {
        return ret;
    }

    if (path_is_absolute(argv[1])) {
        path = g_strdup(argv[1]);
    } else {
        if (!p->dir) {
            bdrv_graph_rdlock_main_loop();
            p->dir = bdrv_dirname(p->bs->file->bs, errp);
            bdrv_graph_rdunlock_main_loop();
            if (!p->dir) {
                return -EINVAL;
            }
        }
        path = g_strconcat(p->dir, argv[1], NULL);
    }

    snprintf(prefix, sizeof(prefix), "bin%d", s->nb_files);
    child = bdrv_open_child(path, p->options, prefix, p->bs, &child_of_bds,
                            BDRV_CHILD_DATA, false, errp);
    if (!child) {
        return -EINVAL;
    }
    s->swap[s->nb_files] = !g_ascii_strcasecmp(argv[2], "MOTOROLA");
    s->files[s->nb_files] = child;

    bdrv_graph_rdlock_main_loop();
    p->file_len = bdrv_getlength(child->bs);
    bdrv_graph_rdunlock_main_loop();
    if (p->file_len < 0) {
        error_setg_errno(errp, -p->file_len, "cue: cannot size '%s'", path);
        return p->file_len;
    }
    p->file = s->nb_files++;
    p->file_byte = 0;
    p->seg_frame = 0;
    p->seg_track = p->track;
    return 0;
}

static int cue_cmd_track(CueParser *p, int argc, char **argv, Error **errp)
{
    CDToc *toc = &p->s->toc;
    static const struct {
        const char *name;
        uint8_t mode;
        uint16_t ssize;
    } types[] = {
        { "AUDIO",      CD_TRACK_AUDIO, 2352 },
        { "MODE1/2048", CD_TRACK_MODE1, 2048 },
        { "MODE1/2352", CD_TRACK_MODE1, 2352 },
        { "MODE2/2336", CD_TRACK_MODE2, 2336 },
        { "MODE2/2352", CD_TRACK_MODE2, 2352 },
    };
    unsigned long num;
    CDTrack *tr;
    int i;

    if (argc < 3 || qemu_strtoul(argv[1], NULL, 10, &num) < 0 ||
        num < 1 || num > CD_MAX_TRACKS) {
        error_setg(errp, "cue: bad TRACK line");
        return -EINVAL;
    }
    if (p->file < 0) {
        error_setg(errp, "cue: TRACK before FILE");
        return -EINVAL;
    }
    if (toc->nb_tracks == 0) {
        toc->first = num;
    } else if (num != toc->first + toc->nb_tracks) {
        error_setg(errp, "cue: track numbers are not consecutive");
        return -EINVAL;
    } else if (!p->info[p->track].has_index1) {
        error_setg(errp, "cue: track %d has no INDEX 01",
                   toc->first + p->track);
        return -EINVAL;
    }
    for (i = 0; i < ARRAY_SIZE(types); i++) {
        if (!g_ascii_strcasecmp(argv[2], types[i].name)) {
            break;
        }
    }
    if (i == ARRAY_SIZE(types)) {
        error_setg(errp, "cue: unsupported track type '%s'", argv[2]);
        return -ENOTSUP;
    }

    p->track = toc->nb_tracks++;
    tr = &toc->tracks[p->track];
    tr->mode = types[i].mode;
    tr->control = tr->mode == CD_TRACK_AUDIO ? 0 : CD_CTRL_DATA;
    p->info[p->track].ssize = types[i].ssize;
    if (p->seg_track < 0) {
        p->seg_track = p->track;
    }
    return 0;
}

static int cue_cmd_index(CueParser *p, int argc, char **argv, Error **errp)
{
    CDTrack *tr;
    CueTrackInfo *ti;
    unsigned long idx;
    uint32_t frame;
    int ret;

    if (p->track < 0) {
        error_setg(errp, "cue: INDEX before TRACK");
        return -EINVAL;
    }
    if (argc < 3 || qemu_strtoul(argv[1], NULL, 10, &idx) < 0 || idx > 99 ||
        !cue_parse_msf(argv[2], &frame)) {
        error_setg(errp, "cue: bad INDEX line");
        return -EINVAL;
    }

    ret = cue_flush(p, frame, errp);
    if (ret < 0) {
        return ret;
    }

    tr = &p->s->toc.tracks[p->track];
    ti = &p->info[p->track];
    if (!ti->started) {
        ti->started = true;
        if (p->track > 0) {
            cue_end_track(p, p->track - 1);
            tr->index0 = p->lba;
            /* track 1's pregap lies before LBA 0 */
            cue_add_extent(p, -1, 0, ti->pregap, CD_RAW_SECTOR_SIZE,
                           tr->mode);
        } else {
            tr->index0 = 0;
        }
    }
    if (idx == 1 && !ti->has_index1) {
        tr->start = p->lba;
        ti->has_index1 = true;
    } else if (idx == 0 && ti->has_index1) {
        error_setg(errp, "cue: INDEX 00 after INDEX 01");
        return -EINVAL;
    }
    p->seg_track = p->track;
    return 0;
}

static int cue_cmd_gap(CueParser *p, int argc, char **argv, bool pre,
                       Error **errp)
{
    uint32_t frames;

    if (p->track < 0 || argc < 2 || !cue_parse_msf(argv[1], &frames)) {
        error_setg(errp, "cue: bad %s line", pre ? "PREGAP" : "POSTGAP");
        return -EINVAL;
    }
    if (pre) {
        if (p->info[p->track].started) {
            error_setg(errp, "cue: PREGAP after INDEX");
            return -EINVAL;
        }
        p->info[p->track].pregap = frames;
    } else {
        p->info[p->track].postgap = frames;
    }
    return 0;
}

static void cue_cmd_flags(CueParser *p, int argc, char **argv)
{
    CDTrack *tr;
    int i;

    if (p->track < 0) {
        return;
    }
    tr = &p->s->toc.tracks[p->track];
    for (i = 1; i < argc; i++) {
        if (!g_ascii_strcasecmp(argv[i], "DCP")) {
            tr->control |= CD_CTRL_COPY;
        } else if (tr->mode == CD_TRACK_AUDIO &&
                   !g_ascii_strcasecmp(argv[i], "4CH")) {
            tr->control |= CD_CTRL_4CH;
        } else if (tr->mode == CD_TRACK_AUDIO &&
                   !g_ascii_strcasecmp(argv[i], "PRE")) {
            tr->control |= CD_CTRL_PREEMPHASIS;
        }
    }
}

static int GRAPH_UNLOCKED cue_parse(CueParser *p, char *text, Error **errp)
{
    char *line, *next;
    char *argv[8];
    int argc, ret = 0;

    if (!strncmp(text, "\xef\xbb\xbf", 3)) {
        text += 3;
    }
    for (line = text; line && ret == 0; line = next) {
        next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
        }
        argc = cue_split(line, argv, ARRAY_SIZE(argv));
        if (argc == 0) {
            continue;
        }
        if (!g_ascii_strcasecmp(argv[0], "FILE")) {
            ret = cue_cmd_file(p, argc, argv, errp);
        } else if (!g_ascii_strcasecmp(argv[0], "TRACK")) {
            ret = cue_cmd_track(p, argc, argv, errp);
        } else if (!g_ascii_strcasecmp(argv[0], "INDEX")) {
            ret = cue_cmd_index(p, argc, argv, errp);
        } else if (!g_ascii_strcasecmp(argv[0], "PREGAP")) {
            ret = cue_cmd_gap(p, argc, argv, true, errp);
        } else if (!g_ascii_strcasecmp(argv[0], "POSTGAP")) {
            ret = cue_cmd_gap(p, argc, argv, false, errp);
        } else if (!g_ascii_strcasecmp(argv[0], "FLAGS")) {
            cue_cmd_flags(p, argc, argv);
        }
        /* REM, CATALOG, CDTEXTFILE, TITLE, PERFORMER, ISRC...: ignored */
    }
    if (ret < 0) {
        return ret;
    }

    if (p->s->toc.nb_tracks == 0) {
        error_setg(errp, "cue: no tracks");
        return -EINVAL;
    }
    if (!p->info[p->track].has_index1) {
        error_setg(errp, "cue: track %d has no INDEX 01",
                   p->s->toc.first + p->track);
        return -EINVAL;
    }
    ret = cue_flush(p, -1, errp);
    if (ret < 0) {
        return ret;
    }
    cue_end_track(p, p->track);
    p->s->toc.leadout = p->lba;
    if (p->lba == 0) {
        error_setg(errp, "cue: empty image");
        return -EINVAL;
    }
    return 0;
}

static void cue_free(BlockDriverState *bs)
{
    BDRVCueState *s = bs->opaque;
    int i;

    bdrv_graph_wrlock_drained();
    for (i = 0; i < s->nb_files; i++) {
        bdrv_unref_child(bs, s->files[i]);
    }
    bdrv_graph_wrunlock();
    s->nb_files = 0;
    g_free(s->ext);
    s->ext = NULL;
    s->nb_ext = 0;
}

static int cue_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVCueState *s = bs->opaque;
    CueParser *p;
    g_autofree char *text = NULL;
    int64_t len;
    int ret;

    GLOBAL_STATE_CODE();

    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, NULL, errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    bdrv_graph_rdlock_main_loop();
    len = bdrv_getlength(bs->file->bs);
    if (len < 0) {
        ret = len;
    } else if (len > CUE_MAX_SIZE) {
        error_setg(errp, "cue: sheet too large");
        ret = -EFBIG;
    } else {
        text = g_malloc0(len + 1);
        ret = bdrv_pread(bs->file, 0, len, text, 0);
    }
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    p = g_new0(CueParser, 1);
    p->bs = bs;
    p->s = s;
    p->options = options;
    p->file = -1;
    p->seg_track = -1;
    p->track = -1;
    ret = cue_parse(p, text, errp);
    g_free(p->dir);
    g_free(p);
    if (ret < 0) {
        cue_free(bs);
        return ret;
    }

    bs->total_sectors = (int64_t)s->toc.leadout *
                        (CD_DATA_SECTOR_SIZE / BDRV_SECTOR_SIZE);
    return 0;
}

static void cue_close(BlockDriverState *bs)
{
    BDRVCueState *s = bs->opaque;

    /* the block layer drops the children */
    g_free(s->ext);
}

static void cue_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = CD_DATA_SECTOR_SIZE;
}

static const CueExtent *cue_find(BDRVCueState *s, uint32_t lba)
{
    int lo = 0, hi = s->nb_ext - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const CueExtent *e = &s->ext[mid];

        if (lba < e->lba) {
            hi = mid - 1;
        } else if (lba >= e->lba + e->count) {
            lo = mid + 1;
        } else {
            return e;
        }
    }
    return NULL;
}

static uint8_t to_bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static void cue_raw_header(uint8_t *buf, uint32_t lba, int mode)
{
    uint32_t a = lba + 150;

    buf[0] = 0x00;
    memset(buf + 1, 0xff, 10);
    buf[11] = 0x00;
    buf[12] = to_bcd(a / 75 / 60);
    buf[13] = to_bcd((a / 75) % 60);
    buf[14] = to_bcd(a % 75);
    buf[15] = mode;
}

/*
 * Read @n sectors of extent @e starting at @lba as raw 2352-byte sectors
 * (@raw) or as 2048-byte user data into @out.
 */
static int coroutine_fn GRAPH_RDLOCK
cue_read_extent(BDRVCueState *s, const CueExtent *e, uint32_t lba, int n,
                bool raw, uint8_t *bounce, uint8_t *out)
{
    int osize = raw ? CD_RAW_SECTOR_SIZE : CD_DATA_SECTOR_SIZE;
    int i, ret;

    if (e->file < 0 || (!raw && e->mode == CD_TRACK_AUDIO)) {
        memset(out, 0, n * osize);
        if (raw && e->mode != CD_TRACK_AUDIO) {
            for (i = 0; i < n; i++) {
                cue_raw_header(out + i * osize, lba + i,
                               e->mode == CD_TRACK_MODE1 ? 1 : 2);
            }
        }
        return 0;
    }

    ret = bdrv_co_pread(s->files[e->file],
                        e->offset + (uint64_t)(lba - e->lba) * e->ssize,
                        n * e->ssize, bounce, 0);
    if (ret < 0) {
        return ret;
    }

    for (i = 0; i < n; i++) {
        const uint8_t *src = bounce + i * e->ssize;
        uint8_t *dst = out + i * osize;

        if (e->mode == CD_TRACK_AUDIO) {
            memcpy(dst, src, CD_RAW_SECTOR_SIZE);
            if (s->swap[e->file]) {
                uint16_t *w = (uint16_t *)dst;
                int k;

                for (k = 0; k < CD_RAW_SECTOR_SIZE / 2; k++) {
                    w[k] = bswap16(w[k]);
                }
            }
        } else if (raw) {
            switch (e->ssize) {
            case 2352:
                memcpy(dst, src, CD_RAW_SECTOR_SIZE);
                break;
            case 2336:
                cue_raw_header(dst, lba + i, 2);
                memcpy(dst + 16, src, 2336);
                break;
            default:
                cue_raw_header(dst, lba + i, 1);
                memcpy(dst + 16, src, CD_DATA_SECTOR_SIZE);
                memset(dst + 16 + CD_DATA_SECTOR_SIZE, 0, 288);
                break;
            }
        } else {
            int off;

            switch (e->ssize) {
            case 2352:
                off = e->mode == CD_TRACK_MODE1 ? 16 : 24;
                break;
            case 2336:
                off = 8;
                break;
            default:
                off = 0;
                break;
            }
            memcpy(dst, src + off, CD_DATA_SECTOR_SIZE);
        }
    }
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
cue_read(BlockDriverState *bs, uint32_t lba, int nb, bool raw,
         QEMUIOVector *qiov)
{
    BDRVCueState *s = bs->opaque;
    int osize = raw ? CD_RAW_SECTOR_SIZE : CD_DATA_SECTOR_SIZE;
    uint8_t *bounce, *out;
    size_t done = 0;
    int ret = 0;

    bounce = qemu_try_blockalign(bs->file->bs,
                                 CUE_BOUNCE_SECTORS * CD_RAW_SECTOR_SIZE);
    out = qemu_try_blockalign(bs->file->bs,
                              CUE_BOUNCE_SECTORS * CD_RAW_SECTOR_SIZE);
    if (!bounce || !out) {
        ret = -ENOMEM;
        goto out;
    }

    while (nb > 0) {
        const CueExtent *e = cue_find(s, lba);
        int n = MIN(nb, CUE_BOUNCE_SECTORS);

        if (!e) {
            memset(out, 0, n * osize);
        } else {
            n = MIN(n, e->lba + e->count - lba);
            ret = cue_read_extent(s, e, lba, n, raw, bounce, out);
            if (ret < 0) {
                goto out;
            }
        }
        qemu_iovec_from_buf(qiov, done, out, n * osize);
        done += n * osize;
        lba += n;
        nb -= n;
    }

out:
    qemu_vfree(bounce);
    qemu_vfree(out);
    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
cue_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
              QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    assert(QEMU_IS_ALIGNED(offset | bytes, CD_DATA_SECTOR_SIZE));
    return cue_read(bs, offset / CD_DATA_SECTOR_SIZE,
                    bytes / CD_DATA_SECTOR_SIZE, false, qiov);
}

static int coroutine_fn GRAPH_RDLOCK
cue_co_cd_read_raw(BlockDriverState *bs, int64_t lba, int nb_sectors,
                   QEMUIOVector *qiov)
{
    BDRVCueState *s = bs->opaque;

    if (lba + nb_sectors > s->toc.leadout) {
        return -EINVAL;
    }
    return cue_read(bs, lba, nb_sectors, true, qiov);
}

static int GRAPH_RDLOCK cue_get_cd_toc(BlockDriverState *bs, CDToc *toc)
{
    BDRVCueState *s = bs->opaque;

    *toc = s->toc;
    return 0;
}

static BlockDriver bdrv_cue = {
    .format_name            = "cue",
    .instance_size          = sizeof(BDRVCueState),
    .bdrv_probe             = cue_probe,
    .bdrv_open              = cue_open,
    .bdrv_close             = cue_close,
    .bdrv_refresh_limits    = cue_refresh_limits,
    .bdrv_child_perm        = bdrv_default_perms,
    .bdrv_co_preadv         = cue_co_preadv,
    .bdrv_get_cd_toc        = cue_get_cd_toc,
    .bdrv_co_cd_read_raw    = cue_co_cd_read_raw,
    .is_format              = true,
};

static void bdrv_cue_init(void)
{
    bdrv_register(&bdrv_cue);
}

block_init(bdrv_cue_init);
