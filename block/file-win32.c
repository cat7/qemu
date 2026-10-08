/*
 * Block driver for RAW files (win32)
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/ctype.h"
#include "qemu/error-report.h"
#include "qemu/memalign.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "block/raw-aio.h"
#include "trace.h"
#include "block/thread-pool.h"
#include "qemu/iov.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "block/cdrom.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>

#define FTYPE_FILE 0
#define FTYPE_CD     1
#define FTYPE_HARDDISK 2

typedef struct RawWin32AIOData {
    BlockDriverState *bs;
    HANDLE hfile;
    struct iovec *aio_iov;
    int aio_niov;
    size_t aio_nbytes;
    off64_t aio_offset;
    int aio_type;
} RawWin32AIOData;

typedef struct BDRVRawState {
    HANDLE hfile;
    int type;
    char drive_path[16]; /* format: "d:\" */
    QEMUWin32AIOState *aio;

    /* host_cdrom */
    char cd_path[16];
    HANDLE cd_poll;
    QEMUTimer *cd_timer;
    ULONG cd_changes;
    bool cd_present;
    bool cd_locked;
    CDToc *cd_toc;
} BDRVRawState;

typedef struct BDRVRawReopenState {
    HANDLE hfile;
} BDRVRawReopenState;

/*
 * Read/writes the data to/from a given linear buffer.
 *
 * Returns the number of bytes handles or -errno in case of an error. Short
 * reads are only returned if the end of the file is reached.
 */
static size_t handle_aiocb_rw(RawWin32AIOData *aiocb)
{
    size_t offset = 0;
    int i;

    for (i = 0; i < aiocb->aio_niov; i++) {
        OVERLAPPED ov;
        DWORD ret, ret_count, len;

        memset(&ov, 0, sizeof(ov));
        ov.Offset = (aiocb->aio_offset + offset);
        ov.OffsetHigh = (aiocb->aio_offset + offset) >> 32;
        len = aiocb->aio_iov[i].iov_len;
        if (aiocb->aio_type & QEMU_AIO_WRITE) {
            ret = WriteFile(aiocb->hfile, aiocb->aio_iov[i].iov_base,
                            len, &ret_count, &ov);
        } else {
            ret = ReadFile(aiocb->hfile, aiocb->aio_iov[i].iov_base,
                           len, &ret_count, &ov);
        }
        if (!ret) {
            ret_count = 0;
        }
        if (ret_count != len) {
            offset += ret_count;
            break;
        }
        offset += len;
    }

    return offset;
}

static int aio_worker(void *arg)
{
    RawWin32AIOData *aiocb = arg;
    ssize_t ret = 0;
    size_t count;

    switch (aiocb->aio_type & QEMU_AIO_TYPE_MASK) {
    case QEMU_AIO_READ:
        count = handle_aiocb_rw(aiocb);
        if (count < aiocb->aio_nbytes) {
            /* A short read means that we have reached EOF. Pad the buffer
             * with zeros for bytes after EOF. */
            iov_memset(aiocb->aio_iov, aiocb->aio_niov, count,
                      0, aiocb->aio_nbytes - count);

            count = aiocb->aio_nbytes;
        }
        if (count == aiocb->aio_nbytes) {
            ret = 0;
        } else {
            ret = -EINVAL;
        }
        break;
    case QEMU_AIO_WRITE:
        count = handle_aiocb_rw(aiocb);
        if (count == aiocb->aio_nbytes) {
            ret = 0;
        } else {
            ret = -EINVAL;
        }
        break;
    case QEMU_AIO_FLUSH:
        if (!FlushFileBuffers(aiocb->hfile)) {
            return -EIO;
        }
        break;
    default:
        fprintf(stderr, "invalid aio request (0x%x)\n", aiocb->aio_type);
        ret = -EINVAL;
        break;
    }

    g_free(aiocb);
    return ret;
}

static BlockAIOCB *paio_submit(BlockDriverState *bs, HANDLE hfile,
        int64_t offset, QEMUIOVector *qiov, int count,
        BlockCompletionFunc *cb, void *opaque, int type)
{
    RawWin32AIOData *acb = g_new(RawWin32AIOData, 1);

    acb->bs = bs;
    acb->hfile = hfile;
    acb->aio_type = type;

    if (qiov) {
        acb->aio_iov = qiov->iov;
        acb->aio_niov = qiov->niov;
        assert(qiov->size == count);
    }
    acb->aio_nbytes = count;
    acb->aio_offset = offset;

    trace_file_paio_submit(acb, opaque, offset, count, type);
    return thread_pool_submit_aio(aio_worker, acb, cb, opaque);
}

static int set_sparse(int fd)
{
    DWORD returned;
    return (int) DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE,
                                 NULL, 0, NULL, 0, &returned, NULL);
}

static void raw_detach_aio_context(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;

    if (s->aio) {
        win32_aio_detach_aio_context(s->aio, bdrv_get_aio_context(bs));
    }
}

static void raw_attach_aio_context(BlockDriverState *bs,
                                   AioContext *new_context)
{
    BDRVRawState *s = bs->opaque;

    if (s->aio) {
        win32_aio_attach_aio_context(s->aio, new_context);
    }
}

static void raw_probe_alignment(BlockDriverState *bs, Error **errp)
{
    BDRVRawState *s = bs->opaque;
    DWORD sectorsPerCluster, freeClusters, totalClusters, count;
    DISK_GEOMETRY_EX dg;
    BOOL status;

    if (s->type == FTYPE_CD) {
        bs->bl.request_alignment = 2048;
        return;
    }
    if (s->type == FTYPE_HARDDISK) {
        status = DeviceIoControl(s->hfile, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                 NULL, 0, &dg, sizeof(dg), &count, NULL);
        if (status != 0) {
            bs->bl.request_alignment = dg.Geometry.BytesPerSector;
            return;
        }
        /* try GetDiskFreeSpace too */
    }

    if (s->drive_path[0]) {
        GetDiskFreeSpace(s->drive_path, &sectorsPerCluster,
                         &dg.Geometry.BytesPerSector,
                         &freeClusters, &totalClusters);
        bs->bl.request_alignment = dg.Geometry.BytesPerSector;
        return;
    }

    /* XXX Does Windows support AIO on less than 512-byte alignment? */
    bs->bl.request_alignment = 512;
}

static void raw_parse_flags(int flags, bool use_aio, int *access_flags,
                            DWORD *overlapped)
{
    assert(access_flags != NULL);
    assert(overlapped != NULL);

    if (flags & BDRV_O_RDWR) {
        *access_flags = GENERIC_READ | GENERIC_WRITE;
    } else {
        *access_flags = GENERIC_READ;
    }

    *overlapped = FILE_ATTRIBUTE_NORMAL;
    if (use_aio) {
        *overlapped |= FILE_FLAG_OVERLAPPED;
    }
    if (flags & BDRV_O_NOCACHE) {
        *overlapped |= FILE_FLAG_NO_BUFFERING;
    }
}

static void raw_parse_filename(const char *filename, QDict *options,
                               Error **errp)
{
    bdrv_parse_filename_strip_prefix(filename, "file:", options);
}

static QemuOptsList raw_runtime_opts = {
    .name = "raw",
    .head = QTAILQ_HEAD_INITIALIZER(raw_runtime_opts.head),
    .desc = {
        {
            .name = "filename",
            .type = QEMU_OPT_STRING,
            .help = "File name of the image",
        },
        {
            .name = "aio",
            .type = QEMU_OPT_STRING,
            .help = "host AIO implementation (threads, native)",
        },
        {
            .name = "locking",
            .type = QEMU_OPT_STRING,
            .help = "file locking mode (on/off/auto, default: auto)",
        },
        { /* end of list */ }
    },
};

static bool get_aio_option(QemuOpts *opts, int flags, Error **errp)
{
    BlockdevAioOptions aio, aio_default;

    aio_default = (flags & BDRV_O_NATIVE_AIO) ? BLOCKDEV_AIO_OPTIONS_NATIVE
                                              : BLOCKDEV_AIO_OPTIONS_THREADS;
    aio = qapi_enum_parse(&BlockdevAioOptions_lookup, qemu_opt_get(opts, "aio"),
                          aio_default, errp);

    switch (aio) {
    case BLOCKDEV_AIO_OPTIONS_NATIVE:
        return true;
    case BLOCKDEV_AIO_OPTIONS_THREADS:
        return false;
    default:
        error_setg(errp, "Invalid AIO option");
    }
    return false;
}

static int raw_open(BlockDriverState *bs, QDict *options, int flags,
                    Error **errp)
{
    BDRVRawState *s = bs->opaque;
    int access_flags;
    DWORD overlapped;
    QemuOpts *opts;
    Error *local_err = NULL;
    const char *filename;
    bool use_aio;
    OnOffAuto locking;
    int ret;

    s->type = FTYPE_FILE;

    opts = qemu_opts_create(&raw_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto fail;
    }

    locking = qapi_enum_parse(&OnOffAuto_lookup,
                              qemu_opt_get(opts, "locking"),
                              ON_OFF_AUTO_AUTO, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto fail;
    }
    switch (locking) {
    case ON_OFF_AUTO_ON:
        error_setg(errp, "locking=on is not supported on Windows");
        ret = -EINVAL;
        goto fail;
    case ON_OFF_AUTO_OFF:
    case ON_OFF_AUTO_AUTO:
        break;
    default:
        g_assert_not_reached();
    }

    filename = qemu_opt_get(opts, "filename");

    use_aio = get_aio_option(opts, flags, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto fail;
    }

    raw_parse_flags(flags, use_aio, &access_flags, &overlapped);

    if (filename[0] && filename[1] == ':') {
        snprintf(s->drive_path, sizeof(s->drive_path), "%c:\\", filename[0]);
    } else if (filename[0] == '\\' && filename[1] == '\\') {
        s->drive_path[0] = 0;
    } else {
        /* Relative path.  */
        char buf[MAX_PATH];
        GetCurrentDirectory(MAX_PATH, buf);
        snprintf(s->drive_path, sizeof(s->drive_path), "%c:\\", buf[0]);
    }

    s->hfile = CreateFile(filename, access_flags,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, overlapped, NULL);
    if (s->hfile == INVALID_HANDLE_VALUE) {
        int err = GetLastError();

        error_setg_win32(errp, err, "Could not open '%s'", filename);
        if (err == ERROR_ACCESS_DENIED) {
            ret = -EACCES;
        } else {
            ret = -EINVAL;
        }
        goto fail;
    }

    if (use_aio) {
        s->aio = win32_aio_init();
        if (s->aio == NULL) {
            CloseHandle(s->hfile);
            error_setg(errp, "Could not initialize AIO");
            ret = -EINVAL;
            goto fail;
        }

        ret = win32_aio_attach(s->aio, s->hfile);
        if (ret < 0) {
            win32_aio_cleanup(s->aio);
            CloseHandle(s->hfile);
            error_setg_errno(errp, -ret, "Could not enable AIO");
            goto fail;
        }

        win32_aio_attach_aio_context(s->aio, bdrv_get_aio_context(bs));
    }

    /* When extending regular files, we get zeros from the OS */
    bs->supported_truncate_flags = BDRV_REQ_ZERO_WRITE;

    ret = 0;
fail:
    qemu_opts_del(opts);
    return ret;
}

static BlockAIOCB *raw_aio_preadv(BlockDriverState *bs,
                                  int64_t offset, int64_t bytes,
                                  QEMUIOVector *qiov, BdrvRequestFlags flags,
                                  BlockCompletionFunc *cb, void *opaque)
{
    BDRVRawState *s = bs->opaque;
    if (s->aio) {
        return win32_aio_submit(bs, s->aio, s->hfile, offset, bytes, qiov,
                                cb, opaque, QEMU_AIO_READ);
    } else {
        return paio_submit(bs, s->hfile, offset, qiov, bytes,
                           cb, opaque, QEMU_AIO_READ);
    }
}

static BlockAIOCB *raw_aio_pwritev(BlockDriverState *bs,
                                   int64_t offset, int64_t bytes,
                                   QEMUIOVector *qiov, BdrvRequestFlags flags,
                                   BlockCompletionFunc *cb, void *opaque)
{
    BDRVRawState *s = bs->opaque;
    if (s->aio) {
        return win32_aio_submit(bs, s->aio, s->hfile, offset, bytes, qiov,
                                cb, opaque, QEMU_AIO_WRITE);
    } else {
        return paio_submit(bs, s->hfile, offset, qiov, bytes,
                           cb, opaque, QEMU_AIO_WRITE);
    }
}

static BlockAIOCB *raw_aio_flush(BlockDriverState *bs,
                         BlockCompletionFunc *cb, void *opaque)
{
    BDRVRawState *s = bs->opaque;
    return paio_submit(bs, s->hfile, 0, NULL, 0, cb, opaque, QEMU_AIO_FLUSH);
}

static void raw_close(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;

    if (s->aio) {
        win32_aio_detach_aio_context(s->aio, bdrv_get_aio_context(bs));
        win32_aio_cleanup(s->aio);
        s->aio = NULL;
    }

    CloseHandle(s->hfile);
    if (bs->open_flags & BDRV_O_TEMPORARY) {
        unlink(bs->filename);
    }
}

static int coroutine_fn raw_co_truncate(BlockDriverState *bs, int64_t offset,
                                        bool exact, PreallocMode prealloc,
                                        BdrvRequestFlags flags, Error **errp)
{
    BDRVRawState *s = bs->opaque;
    LONG low, high;
    DWORD dwPtrLow;

    if (prealloc != PREALLOC_MODE_OFF) {
        error_setg(errp, "Unsupported preallocation mode '%s'",
                   PreallocMode_str(prealloc));
        return -ENOTSUP;
    }

    low = offset;
    high = offset >> 32;

    /*
     * An error has occurred if the return value is INVALID_SET_FILE_POINTER
     * and GetLastError doesn't return NO_ERROR.
     */
    dwPtrLow = SetFilePointer(s->hfile, low, &high, FILE_BEGIN);
    if (dwPtrLow == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        error_setg_win32(errp, GetLastError(), "SetFilePointer error");
        return -EIO;
    }
    if (SetEndOfFile(s->hfile) == 0) {
        error_setg_win32(errp, GetLastError(), "SetEndOfFile error");
        return -EIO;
    }
    return 0;
}

static int64_t coroutine_fn raw_co_getlength(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    LARGE_INTEGER l;
    ULARGE_INTEGER available, total, total_free;
    DISK_GEOMETRY_EX dg;
    DWORD count;
    BOOL status;

    switch(s->type) {
    case FTYPE_FILE:
        l.LowPart = GetFileSize(s->hfile, (PDWORD)&l.HighPart);
        if (l.LowPart == 0xffffffffUL && GetLastError() != NO_ERROR)
            return -EIO;
        break;
    case FTYPE_CD:
        if (!GetDiskFreeSpaceEx(s->drive_path, &available, &total, &total_free))
            return -EIO;
        l.QuadPart = total.QuadPart;
        break;
    case FTYPE_HARDDISK:
        status = DeviceIoControl(s->hfile, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                 NULL, 0, &dg, sizeof(dg), &count, NULL);
        if (status != 0) {
            l = dg.DiskSize;
        }
        break;
    default:
        return -EIO;
    }
    return l.QuadPart;
}

static int64_t coroutine_fn raw_co_get_allocated_file_size(BlockDriverState *bs)
{
    typedef DWORD (WINAPI * get_compressed_t)(const char *filename,
                                              DWORD * high);
    get_compressed_t get_compressed;
    struct _stati64 st;
    const char *filename = bs->filename;
    /* WinNT support GetCompressedFileSize to determine allocate size */
    get_compressed =
        (get_compressed_t) GetProcAddress(GetModuleHandle("kernel32"),
                                            "GetCompressedFileSizeA");
    if (get_compressed) {
        DWORD high, low;
        low = get_compressed(filename, &high);
        if (low != 0xFFFFFFFFlu || GetLastError() == NO_ERROR) {
            return (((int64_t) high) << 32) + low;
        }
    }

    if (_stati64(filename, &st) < 0) {
        return -1;
    }
    return st.st_size;
}

static int raw_co_create(BlockdevCreateOptions *options, Error **errp)
{
    BlockdevCreateOptionsFile *file_opts;
    int fd;

    assert(options->driver == BLOCKDEV_DRIVER_FILE);
    file_opts = &options->u.file;

    if (file_opts->has_preallocation) {
        error_setg(errp, "Preallocation is not supported on Windows");
        return -EINVAL;
    }
    if (file_opts->has_nocow) {
        error_setg(errp, "nocow is not supported on Windows");
        return -EINVAL;
    }

    fd = qemu_create(file_opts->filename, O_WRONLY | O_TRUNC | O_BINARY,
                     0644, errp);
    if (fd < 0) {
        return -EIO;
    }
    set_sparse(fd);
    ftruncate(fd, file_opts->size);
    qemu_close(fd);

    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
raw_co_create_opts(BlockDriver *drv, const char *filename,
                   QemuOpts *opts, Error **errp)
{
    BlockdevCreateOptions options;
    int64_t total_size = 0;

    strstart(filename, "file:", &filename);

    /* Read out options */
    total_size = ROUND_UP(qemu_opt_get_size_del(opts, BLOCK_OPT_SIZE, 0),
                          BDRV_SECTOR_SIZE);

    options = (BlockdevCreateOptions) {
        .driver     = BLOCKDEV_DRIVER_FILE,
        .u.file     = {
            .filename           = (char *) filename,
            .size               = total_size,
            .has_preallocation  = false,
            .has_nocow          = false,
        },
    };
    return raw_co_create(&options, errp);
}

static int raw_reopen_prepare(BDRVReopenState *state,
                              BlockReopenQueue *queue, Error **errp)
{
    BDRVRawState *s = state->bs->opaque;
    BDRVRawReopenState *rs;
    int access_flags;
    DWORD overlapped;
    int ret = 0;

    if (s->type != FTYPE_FILE) {
        error_setg(errp, "Can only reopen files");
        return -EINVAL;
    }

    rs = g_new0(BDRVRawReopenState, 1);

    /*
     * We do not support changing any options (only flags). By leaving
     * all options in state->options, we tell the generic reopen code
     * that we do not support changing any of them, so it will verify
     * that their values did not change.
     */

    raw_parse_flags(state->flags, s->aio != NULL, &access_flags, &overlapped);
    rs->hfile = CreateFile(state->bs->filename, access_flags,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, overlapped, NULL);

    if (rs->hfile == INVALID_HANDLE_VALUE) {
        int err = GetLastError();

        error_setg_win32(errp, err, "Could not reopen '%s'",
                         state->bs->filename);
        if (err == ERROR_ACCESS_DENIED) {
            ret = -EACCES;
        } else {
            ret = -EINVAL;
        }
        goto fail;
    }

    if (s->aio) {
        ret = win32_aio_attach(s->aio, rs->hfile);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "Could not enable AIO");
            CloseHandle(rs->hfile);
            goto fail;
        }
    }

    state->opaque = rs;

    return 0;

fail:
    g_free(rs);
    state->opaque = NULL;

    return ret;
}

static void raw_reopen_commit(BDRVReopenState *state)
{
    BDRVRawState *s = state->bs->opaque;
    BDRVRawReopenState *rs = state->opaque;

    assert(rs != NULL);

    CloseHandle(s->hfile);
    s->hfile = rs->hfile;

    g_free(rs);
    state->opaque = NULL;
}

static void raw_reopen_abort(BDRVReopenState *state)
{
    BDRVRawReopenState *rs = state->opaque;

    if (!rs) {
        return;
    }

    if (rs->hfile != INVALID_HANDLE_VALUE) {
        CloseHandle(rs->hfile);
    }

    g_free(rs);
    state->opaque = NULL;
}

static QemuOptsList raw_create_opts = {
    .name = "raw-create-opts",
    .head = QTAILQ_HEAD_INITIALIZER(raw_create_opts.head),
    .desc = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Virtual disk size"
        },
        { /* end of list */ }
    }
};

BlockDriver bdrv_file = {
    .format_name            = "file",
    .protocol_name          = "file",
    .instance_size          = sizeof(BDRVRawState),
    .bdrv_needs_filename    = true,
    .bdrv_parse_filename    = raw_parse_filename,
    .bdrv_open              = raw_open,
    .bdrv_refresh_limits    = raw_probe_alignment,
    .bdrv_close             = raw_close,
    .bdrv_co_create_opts    = raw_co_create_opts,
    .bdrv_has_zero_init     = bdrv_has_zero_init_1,

    .bdrv_reopen_prepare = raw_reopen_prepare,
    .bdrv_reopen_commit  = raw_reopen_commit,
    .bdrv_reopen_abort   = raw_reopen_abort,

    .bdrv_aio_preadv    = raw_aio_preadv,
    .bdrv_aio_pwritev   = raw_aio_pwritev,
    .bdrv_aio_flush     = raw_aio_flush,

    .bdrv_co_truncate   = raw_co_truncate,
    .bdrv_co_getlength  = raw_co_getlength,
    .bdrv_co_get_allocated_file_size
                        = raw_co_get_allocated_file_size,

    .create_opts        = &raw_create_opts,
};

/***********************************************/
/* host device */

static int find_cdrom(char *cdrom_name, int cdrom_name_size)
{
    char drives[256], *pdrv = drives;
    UINT type;

    memset(drives, 0, sizeof(drives));
    GetLogicalDriveStrings(sizeof(drives), drives);
    while(pdrv[0] != '\0') {
        type = GetDriveType(pdrv);
        switch(type) {
        case DRIVE_CDROM:
            snprintf(cdrom_name, cdrom_name_size, "\\\\.\\%c:", pdrv[0]);
            return 0;
            break;
        }
        pdrv += lstrlen(pdrv) + 1;
    }
    return -1;
}

static int find_device_type(BlockDriverState *bs, const char *filename)
{
    BDRVRawState *s = bs->opaque;
    UINT type;
    const char *p;

    if (strstart(filename, "\\\\.\\", &p) ||
        strstart(filename, "//./", &p)) {
        if (stristart(p, "PhysicalDrive", NULL))
            return FTYPE_HARDDISK;
        snprintf(s->drive_path, sizeof(s->drive_path), "%c:\\", p[0]);
        type = GetDriveType(s->drive_path);
        switch (type) {
        case DRIVE_REMOVABLE:
        case DRIVE_FIXED:
            return FTYPE_HARDDISK;
        case DRIVE_CDROM:
            return FTYPE_CD;
        default:
            return FTYPE_FILE;
        }
    } else {
        return FTYPE_FILE;
    }
}

static int hdev_probe_device(const char *filename)
{
    /* allow host_cdrom to match CD drives with a higher priority */
    if (strstart(filename, "/dev/cdrom", NULL))
        return 50;
    if (is_windows_drive(filename))
        return 50;
    return 0;
}

static void hdev_parse_filename(const char *filename, QDict *options,
                                Error **errp)
{
    bdrv_parse_filename_strip_prefix(filename, "host_device:", options);
}

static void hdev_refresh_limits(BlockDriverState *bs, Error **errp)
{
    /* XXX Does Windows support AIO on less than 512-byte alignment? */
    bs->bl.request_alignment = 512;
    bs->bl.has_variable_length = true;
}

static int hdev_open(BlockDriverState *bs, QDict *options, int flags,
                     Error **errp)
{
    BDRVRawState *s = bs->opaque;
    int access_flags, create_flags;
    int ret = 0;
    DWORD overlapped;
    char device_name[64];

    Error *local_err = NULL;
    const char *filename;
    bool use_aio;

    QemuOpts *opts = qemu_opts_create(&raw_runtime_opts, NULL, 0,
                                      &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        ret = -EINVAL;
        goto done;
    }

    filename = qemu_opt_get(opts, "filename");

    use_aio = get_aio_option(opts, flags, &local_err);
    if (!local_err && use_aio) {
        error_setg(&local_err, "AIO is not supported on Windows host devices");
    }
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto done;
    }

    if (strstart(filename, "/dev/cdrom", NULL)) {
        if (find_cdrom(device_name, sizeof(device_name)) < 0) {
            error_setg(errp, "Could not open CD-ROM drive");
            ret = -ENOENT;
            goto done;
        }
        filename = device_name;
    } else {
        /* transform drive letters into device name */
        if (((filename[0] >= 'a' && filename[0] <= 'z') ||
             (filename[0] >= 'A' && filename[0] <= 'Z')) &&
            filename[1] == ':' && filename[2] == '\0') {
            snprintf(device_name, sizeof(device_name), "\\\\.\\%c:", filename[0]);
            filename = device_name;
        }
    }
    s->type = find_device_type(bs, filename);

    raw_parse_flags(flags, use_aio, &access_flags, &overlapped);

    create_flags = OPEN_EXISTING;

    s->hfile = CreateFile(filename, access_flags,
                          FILE_SHARE_READ, NULL,
                          create_flags, overlapped, NULL);
    if (s->hfile == INVALID_HANDLE_VALUE) {
        int err = GetLastError();

        if (err == ERROR_ACCESS_DENIED) {
            ret = -EACCES;
        } else {
            ret = -EINVAL;
        }
        error_setg_win32(errp, err, "Could not open device");
        goto done;
    }

done:
    qemu_opts_del(opts);
    return ret;
}

static BlockDriver bdrv_host_device = {
    .format_name            = "host_device",
    .protocol_name          = "host_device",
    .instance_size          = sizeof(BDRVRawState),
    .bdrv_needs_filename    = true,
    .bdrv_parse_filename    = hdev_parse_filename,
    .bdrv_probe_device      = hdev_probe_device,
    .bdrv_open              = hdev_open,
    .bdrv_close             = raw_close,
    .bdrv_refresh_limits    = hdev_refresh_limits,

    .bdrv_aio_preadv    = raw_aio_preadv,
    .bdrv_aio_pwritev   = raw_aio_pwritev,
    .bdrv_aio_flush     = raw_aio_flush,

    .bdrv_detach_aio_context = raw_detach_aio_context,
    .bdrv_attach_aio_context = raw_attach_aio_context,

    .bdrv_co_getlength                = raw_co_getlength,
    .bdrv_co_get_allocated_file_size  = raw_co_get_allocated_file_size,
};

/***********************************************/
/* host CD drive */

/*
 * The drive is polled for media changes through an attribute-only handle,
 * which stays valid while discs come and go. The read handle is opened per
 * disc.
 */

#define CD_POLL_MS          1000
#define CD_CHUNK            24      /* sectors per raw read */

static void cdrom_parse_filename(const char *filename, QDict *options,
                                 Error **errp)
{
    bdrv_parse_filename_strip_prefix(filename, "host_cdrom:", options);
}

static bool cdrom_letter_is_cd(char letter)
{
    char root[4] = { letter, ':', '\\', '\0' };

    return GetDriveType(root) == DRIVE_CDROM;
}

/* Drive letter of "D", "D:", "D:\", "\\.\D:" or "/dev/cdrom"; 0 if none. */
static char cdrom_drive_letter(const char *name)
{
    const char *p = name;
    char path[64];

    if (strstart(name, "/dev/cdrom", NULL)) {
        if (find_cdrom(path, sizeof(path)) < 0) {
            return 0;
        }
        return path[4];
    }
    if (!strstart(name, "\\\\.\\", &p)) {
        strstart(name, "//./", &p);
    }
    if (!qemu_isalpha(p[0]) ||
        !(p[1] == '\0' || (p[1] == ':' && (p[2] == '\0' ||
                                          (p[2] == '\\' && p[3] == '\0'))))) {
        return 0;
    }
    return qemu_toupper(p[0]);
}

static int cdrom_probe_device(const char *filename)
{
    char letter = cdrom_drive_letter(filename);

    return letter && cdrom_letter_is_cd(letter) ? 100 : 0;
}

static bool cdrom_check(BDRVRawState *s, ULONG *changes)
{
    DWORD n = 0;

    *changes = 0;
    return DeviceIoControl(s->cd_poll, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0,
                           changes, sizeof(*changes), &n, NULL);
}

static void cdrom_load_toc(BDRVRawState *s)
{
    uint8_t buf[4 + 128 * 11];
    CDROM_READ_TOC_EX req = {
        .Format = CDROM_READ_TOC_EX_FORMAT_FULL_TOC,
        .Msf = 1,
        .SessionTrack = 1,
    };
    CDToc toc;
    DWORD n = 0;

    g_free(s->cd_toc);
    s->cd_toc = NULL;
    if (s->hfile == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(s->hfile, IOCTL_CDROM_READ_TOC_EX, &req, sizeof(req),
                         buf, sizeof(buf), &n, NULL) ||
        cd_toc_parse_full(buf, n, &toc) < 0) {
        return;
    }
    s->cd_toc = g_memdup2(&toc, sizeof(toc));
}

static void cdrom_apply_lock(BDRVRawState *s)
{
    PREVENT_MEDIA_REMOVAL pmr = { .PreventMediaRemoval = s->cd_locked };
    DWORD n;

    if (s->hfile != INVALID_HANDLE_VALUE &&
        !DeviceIoControl(s->hfile, IOCTL_STORAGE_MEDIA_REMOVAL, &pmr,
                         sizeof(pmr), NULL, 0, &n, NULL)) {
        warn_report("host_cdrom: could not %s the drive",
                    s->cd_locked ? "lock" : "unlock");
    }
}

/* Open the read handle on the disc now in the drive, or close it. */
static void cdrom_set_medium(BDRVRawState *s, bool present)
{
    if (s->hfile != INVALID_HANDLE_VALUE) {
        CloseHandle(s->hfile);
        s->hfile = INVALID_HANDLE_VALUE;
    }
    g_free(s->cd_toc);
    s->cd_toc = NULL;
    if (!present) {
        return;
    }
    s->hfile = CreateFile(s->cd_path, GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL);
    if (s->hfile == INVALID_HANDLE_VALUE) {
        warn_report("host_cdrom: cannot open %s (error %lu)", s->cd_path,
                    GetLastError());
        return;
    }
    cdrom_load_toc(s);
    if (s->cd_locked) {
        cdrom_apply_lock(s);
    }
}

static void cdrom_poll(void *opaque)
{
    BlockDriverState *bs = opaque;
    BDRVRawState *s = bs->opaque;
    ULONG changes;
    bool present = cdrom_check(s, &changes);
    bool was = s->cd_present;

    if (present != was || (present && changes != s->cd_changes)) {
        bdrv_drained_begin(bs);
        s->cd_present = present;
        s->cd_changes = changes;
        cdrom_set_medium(s, present);
        bdrv_drained_end(bs);

        GRAPH_RDLOCK_GUARD_MAINLOOP();
        if (was) {
            bdrv_media_changed(bs, false);
        }
        if (present) {
            bdrv_media_changed(bs, true);
        }
    }
    timer_mod(s->cd_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CD_POLL_MS);
}

/*
 * drive=<letter> or filename=\\.\D:, D: or /dev/cdrom (the first CD
 * drive) follow the discs of that drive.
 */
static int cdrom_open(BlockDriverState *bs, QDict *options, int flags,
                      Error **errp)
{
    BDRVRawState *s = bs->opaque;
    QemuOpts *opts;
    const char *name;
    char letter;
    ULONG changes;
    int ret;

    name = qdict_get_try_str(options, "drive");
    if (!name) {
        name = qdict_get_try_str(options, "filename");
    }
    if (!name) {
        error_setg(errp, "host_cdrom needs 'filename' or 'drive'");
        return -EINVAL;
    }
    letter = cdrom_drive_letter(name);
    if (!letter || !cdrom_letter_is_cd(letter)) {
        error_setg(errp, "host_cdrom: '%s' is not a CD drive", name);
        return -ENOENT;
    }
    qdict_del(options, "drive");

    ret = bdrv_apply_auto_read_only(bs, "host_cdrom is read-only", errp);
    if (ret < 0) {
        return ret;
    }

    opts = qemu_opts_create(&raw_runtime_opts, NULL, 0, &error_abort);
    qdict_del(options, "filename");
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        qemu_opts_del(opts);
        return -EINVAL;
    }
    qemu_opts_del(opts);

    s->type = FTYPE_CD;
    s->hfile = INVALID_HANDLE_VALUE;
    snprintf(s->drive_path, sizeof(s->drive_path), "%c:\\", letter);
    snprintf(s->cd_path, sizeof(s->cd_path), "\\\\.\\%c:", letter);
    s->cd_poll = CreateFile(s->cd_path, FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (s->cd_poll == INVALID_HANDLE_VALUE) {
        error_setg_win32(errp, GetLastError(), "Could not open %s",
                         s->cd_path);
        return -EIO;
    }
    s->cd_present = cdrom_check(s, &changes);
    s->cd_changes = changes;
    cdrom_set_medium(s, s->cd_present);
    s->cd_timer = timer_new_ms(QEMU_CLOCK_REALTIME, cdrom_poll, bs);
    timer_mod(s->cd_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CD_POLL_MS);
    return 0;
}

static void cdrom_close(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;

    timer_free(s->cd_timer);
    s->cd_timer = NULL;
    cdrom_set_medium(s, false);
    CloseHandle(s->cd_poll);
    s->cd_poll = INVALID_HANDLE_VALUE;
}

static void cdrom_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = CD_DATA_SECTOR_SIZE;
    bs->bl.has_variable_length = true;
}

typedef struct CDWin32Req {
    HANDLE h;
    DWORD ioctl;            /* 0: ReadFile */
    RAW_READ_INFO info;
    uint64_t offset;
    uint8_t *buf;
    DWORD len;
} CDWin32Req;

static int cdrom_worker(void *opaque)
{
    CDWin32Req *r = opaque;
    DWORD n = 0;
    BOOL ok;

    if (r->ioctl) {
        ok = DeviceIoControl(r->h, r->ioctl, &r->info, sizeof(r->info),
                             r->buf, r->len, &n, NULL);
    } else {
        OVERLAPPED ov = {
            .Offset = r->offset,
            .OffsetHigh = r->offset >> 32,
        };

        ok = ReadFile(r->h, r->buf, r->len, &n, &ov);
    }
    return ok && n == r->len ? 0 : -EIO;
}

/* @n sectors of track mode @mode from @lba, as 2048-byte user data or raw. */
static int coroutine_fn cdrom_read_sectors(BlockDriverState *bs,
                                           uint32_t lba, uint32_t n,
                                           int mode, bool raw, uint8_t *buf,
                                           uint8_t *tmp)
{
    BDRVRawState *s = bs->opaque;
    CDWin32Req r = { .h = s->hfile };
    uint32_t i;
    int ret;

    if (mode == CD_TRACK_AUDIO) {
        if (!raw) {
            memset(buf, 0, n * CD_DATA_SECTOR_SIZE);
            return 0;
        }
        r.ioctl = IOCTL_CDROM_RAW_READ;
        r.info.DiskOffset.QuadPart = (uint64_t)lba * CD_DATA_SECTOR_SIZE;
        r.info.SectorCount = n;
        r.info.TrackMode = CDDA;
        r.buf = buf;
        r.len = n * CD_RAW_SECTOR_SIZE;
        return thread_pool_submit_co(cdrom_worker, &r);
    }
    if (!raw) {
        r.offset = (uint64_t)lba * CD_DATA_SECTOR_SIZE;
        r.buf = buf;
        r.len = n * CD_DATA_SECTOR_SIZE;
        return thread_pool_submit_co(cdrom_worker, &r);
    }
    if (mode == CD_TRACK_MODE2) {
        r.ioctl = IOCTL_CDROM_RAW_READ;
        r.info.DiskOffset.QuadPart = (uint64_t)lba * CD_DATA_SECTOR_SIZE;
        r.info.SectorCount = n;
        r.info.TrackMode = YellowMode2;
        r.buf = tmp;
        r.len = n * 2336;
    } else {
        r.offset = (uint64_t)lba * CD_DATA_SECTOR_SIZE;
        r.buf = tmp;
        r.len = n * CD_DATA_SECTOR_SIZE;
    }
    ret = thread_pool_submit_co(cdrom_worker, &r);
    if (ret < 0) {
        return ret;
    }
    for (i = 0; i < n; i++) {
        uint8_t *dst = buf + i * CD_RAW_SECTOR_SIZE;

        memset(dst, 0, CD_RAW_SECTOR_SIZE);
        cd_raw_header(dst, lba + i, mode == CD_TRACK_MODE2 ? 2 : 1);
        if (mode == CD_TRACK_MODE2) {
            memcpy(dst + 16, tmp + i * 2336, 2336);
        } else {
            memcpy(dst + 16, tmp + i * CD_DATA_SECTOR_SIZE,
                   CD_DATA_SECTOR_SIZE);
        }
    }
    return 0;
}

/* Read [lba, lba + nb) split at track mode changes. */
static int coroutine_fn cdrom_read_runs(BlockDriverState *bs, uint32_t lba,
                                        uint32_t nb, bool raw,
                                        QEMUIOVector *qiov)
{
    BDRVRawState *s = bs->opaque;
    uint32_t osize = raw ? CD_RAW_SECTOR_SIZE : CD_DATA_SECTOR_SIZE;
    uint8_t *buf = qemu_try_blockalign(bs, CD_CHUNK * CD_RAW_SECTOR_SIZE);
    uint8_t *tmp = qemu_try_blockalign(bs, CD_CHUNK * CD_RAW_SECTOR_SIZE);
    size_t done = 0;
    int ret = 0;

    if (!buf || !tmp) {
        ret = -ENOMEM;
        goto out;
    }
    while (nb > 0) {
        int i = s->cd_toc ? cd_toc_find(s->cd_toc, lba) : -1;
        uint32_t n = MIN(nb, CD_CHUNK);

        if (!s->cd_toc) {
            ret = cdrom_read_sectors(bs, lba, n, CD_TRACK_MODE1, raw, buf,
                                     tmp);
        } else if (i < 0) {
            memset(buf, 0, n * osize);
        } else {
            const CDTrack *t = &s->cd_toc->tracks[i];

            n = MIN(n, t->end - lba);
            ret = cdrom_read_sectors(bs, lba, n, t->mode, raw, buf, tmp);
        }
        if (ret < 0) {
            break;
        }
        qemu_iovec_from_buf(qiov, done, buf, n * osize);
        done += n * osize;
        lba += n;
        nb -= n;
    }
out:
    qemu_vfree(buf);
    qemu_vfree(tmp);
    return ret;
}

static int coroutine_fn cdrom_co_preadv(BlockDriverState *bs, int64_t offset,
                                        int64_t bytes, QEMUIOVector *qiov,
                                        BdrvRequestFlags flags)
{
    BDRVRawState *s = bs->opaque;

    if (s->hfile == INVALID_HANDLE_VALUE) {
        return -ENOMEDIUM;
    }
    assert(QEMU_IS_ALIGNED(offset | bytes, CD_DATA_SECTOR_SIZE));
    return cdrom_read_runs(bs, offset / CD_DATA_SECTOR_SIZE,
                           bytes / CD_DATA_SECTOR_SIZE, false, qiov);
}

static int coroutine_fn cdrom_co_cd_read_raw(BlockDriverState *bs,
                                             int64_t lba, int nb_sectors,
                                             QEMUIOVector *qiov)
{
    BDRVRawState *s = bs->opaque;

    if (s->hfile == INVALID_HANDLE_VALUE || !s->cd_toc) {
        return -ENOTSUP;
    }
    if (lba + nb_sectors > s->cd_toc->leadout) {
        return -EINVAL;
    }
    return cdrom_read_runs(bs, lba, nb_sectors, true, qiov);
}

static int cdrom_get_cd_toc(BlockDriverState *bs, CDToc *toc)
{
    BDRVRawState *s = bs->opaque;

    if (!s->cd_toc) {
        return -ENOTSUP;
    }
    *toc = *s->cd_toc;
    return 0;
}

static int64_t coroutine_fn cdrom_co_getlength(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;
    GET_LENGTH_INFORMATION li;
    DWORD n;

    if (s->hfile == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (s->cd_toc) {
        return (int64_t)s->cd_toc->leadout * CD_DATA_SECTOR_SIZE;
    }
    if (DeviceIoControl(s->hfile, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &li, sizeof(li), &n, NULL)) {
        return li.Length.QuadPart;
    }
    return raw_co_getlength(bs);
}

static bool coroutine_fn cdrom_co_is_inserted(BlockDriverState *bs)
{
    BDRVRawState *s = bs->opaque;

    return s->cd_present && s->hfile != INVALID_HANDLE_VALUE;
}

static void coroutine_fn cdrom_co_eject(BlockDriverState *bs, bool eject_flag)
{
    BDRVRawState *s = bs->opaque;
    HANDLE h = s->hfile;
    DWORD n;

    if (eject_flag && s->cd_locked) {
        s->cd_locked = false;
        cdrom_apply_lock(s);
    }
    if (h == INVALID_HANDLE_VALUE) {
        /* no disc: a handle on the drive itself */
        h = CreateFile(s->cd_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
    }
    if (!DeviceIoControl(h, eject_flag ? IOCTL_STORAGE_EJECT_MEDIA
                                       : IOCTL_STORAGE_LOAD_MEDIA,
                         NULL, 0, NULL, 0, &n, NULL)) {
        warn_report("host_cdrom: %s failed (error %lu)",
                    eject_flag ? "eject" : "load", GetLastError());
    }
    if (h != s->hfile) {
        CloseHandle(h);
    }
}

static void coroutine_fn cdrom_co_lock_medium(BlockDriverState *bs,
                                              bool locked)
{
    BDRVRawState *s = bs->opaque;

    s->cd_locked = locked;
    cdrom_apply_lock(s);
}

static BlockDriver bdrv_host_cdrom = {
    .format_name            = "host_cdrom",
    .protocol_name          = "host_cdrom",
    .instance_size          = sizeof(BDRVRawState),
    .bdrv_parse_filename    = cdrom_parse_filename,
    .bdrv_probe_device      = cdrom_probe_device,
    .bdrv_open              = cdrom_open,
    .bdrv_close             = cdrom_close,
    .bdrv_refresh_limits    = cdrom_refresh_limits,

    .bdrv_co_preadv         = cdrom_co_preadv,

    .bdrv_co_getlength      = cdrom_co_getlength,
    .bdrv_co_is_inserted    = cdrom_co_is_inserted,
    .bdrv_co_eject          = cdrom_co_eject,
    .bdrv_co_lock_medium    = cdrom_co_lock_medium,

    .bdrv_get_cd_toc        = cdrom_get_cd_toc,
    .bdrv_co_cd_read_raw    = cdrom_co_cd_read_raw,
};

static void bdrv_file_init(void)
{
    bdrv_register(&bdrv_file);
    bdrv_register(&bdrv_host_device);
    bdrv_register(&bdrv_host_cdrom);
}

block_init(bdrv_file_init);
