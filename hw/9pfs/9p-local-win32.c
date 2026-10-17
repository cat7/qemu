/*
 * 9p local backend for Windows
 *
 * Implements the FileOperations interface using Win32 / MSVC runtime APIs.
 * Designed for use with classicvirtio and Mac OS 9 guests.
 *
 * Only security_model=none is supported on Windows.
 * macOS compatibility mode (macos_compat) uses NTFS Alternate Data Streams
 * for FinderInfo and resource fork storage.
 *
 * Copyright (c) 2025
 * License: GNU GPL v2 or later
 *
 * Based on the POSIX 9p-local.c by Anthony Liguori (IBM, Corp. 2010)
 * and the Win32 9pfs patches by Guohuai Shi / Bin Meng (Wind River, 2022)
 */

#ifdef _WIN32

#include "qemu/osdep.h"
#include "9p.h"
#include "9p-local.h"
#include "9p-xattr.h"
#include "9p-util.h"
#include "9p-macos-compat.h"
#include "9p-macroman.h"

#include "fsdev/qemu-fsdev.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qapi/error.h"

#include <windows.h>
#include <io.h>
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

/*
 * On Windows, we don't have dirfd-based operations. Instead, we build
 * full paths from fs_root + relative path. This is safe because Windows
 * doesn't have the symlink attack surface that POSIX systems do (creating
 * symlinks requires elevated privileges on Windows).
 */

/*
 * =========================================================================
 *  Snapshot-based directory enumeration
 *
 *  On Windows, MinGW's opendir()/readdir() holds a FindFirstFile search
 *  handle on the directory for the entire lifetime of the DIR* stream.
 *  This prevents Windows from deleting, moving, or renaming the directory
 *  while the guest is browsing it (e.g., the Finder has it open).
 *
 *  Instead, we snapshot all directory entries at opendir time, close the
 *  native handle immediately, and serve readdir/seekdir/telldir from the
 *  in-memory snapshot.  On rewinddir we re-snapshot to pick up changes.
 *
 *  The snapshot is stored in a Win32DirSnap structure.  We cast this to
 *  DIR* when storing it in fs->dir.stream — the 9P core only checks for
 *  NULL, stores, and passes it back to our local_* functions.
 * =========================================================================
 */

/* Forward declarations */
static void normalize_path(char *path);
static char *win32_native_path(const char *path);
static wchar_t *utf8_to_wstr(const char *utf8);
static wchar_t *win32_wide_path(const char *path);
static wchar_t *win32_wide_native(const char *path);

typedef struct Win32DirEntry {
    struct dirent d;     /* cached dirent — d_name filled in */
    uint64_t ino;        /* NTFS file index (inode equivalent) */
} Win32DirEntry;

typedef struct Win32DirSnap {
    Win32DirEntry *entries;    /* array of cached entries */
    int count;                 /* number of entries */
    int pos;                   /* current read position (0-based) */
    char *hostpath;            /* full host path for re-snapshotting */
} Win32DirSnap;

/*
 * Read all directory entries from path into a newly allocated snapshot.
 * Returns NULL on failure (errno set).
 */
/*
 * Build a snapshot of directory entries from the given wide path.
 * Each entry gets its d_name (UTF-8), d_reclen (d_type: DT_DIR/DT_REG),
 * and ino (NTFS file index) populated.
 *
 * Returns a GArray of Win32DirEntry on success, NULL on failure.
 * The caller must close the _WDIR after using this function if needed.
 */
static GArray *win32_snap_build_entries(const wchar_t *wpath, _WDIR *d)
{
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(Win32DirEntry));
    struct _wdirent *went;

    while ((went = _wreaddir(d)) != NULL) {
        Win32DirEntry we;
        memset(&we, 0, sizeof(we));
        /* Convert wide d_name back to UTF-8 for our snapshot */
        WideCharToMultiByte(CP_UTF8, 0, went->d_name, -1,
                            we.d.d_name, sizeof(we.d.d_name), NULL, NULL);
        /*
         * Get file attributes AND NTFS file index for each child.
         * We open a handle with FILE_FLAG_BACKUP_SEMANTICS (needed for
         * directories) to get the file index, which serves as our inode
         * number for unique QID path generation.
         */
        {
            wchar_t child[MAX_PATH + 1];
            _snwprintf(child, MAX_PATH, L"%s\\%s", wpath, went->d_name);
            child[MAX_PATH] = L'\0';

            HANDLE h = CreateFileW(
                child,
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                NULL);
            if (h != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION info;
                if (GetFileInformationByHandle(h, &info)) {
                    we.ino = ((uint64_t)info.nFileIndexHigh << 32) |
                             info.nFileIndexLow;
                    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        we.d.d_reclen = DT_DIR;
                    } else {
                        we.d.d_reclen = DT_REG;
                    }
                } else {
                    we.d.d_reclen = DT_REG;
                }
                CloseHandle(h);
            } else {
                DWORD attrs = GetFileAttributesW(child);
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    we.d.d_reclen = DT_DIR;
                } else {
                    we.d.d_reclen = DT_REG;
                }
            }
        }
        g_array_append_val(arr, we);
    }

    return arr;
}

static Win32DirSnap *win32_snap_opendir(const char *path)
{
    wchar_t *wpath = win32_wide_path(path);

    if (!wpath) {
        errno = EINVAL;
        return NULL;
    }

    _WDIR *d = _wopendir(wpath);

    if (!d) {
        g_free(wpath);
        return NULL;
    }

    GArray *arr = win32_snap_build_entries(wpath, d);

    /* Close the native handle immediately — this releases the lock */
    _wclosedir(d);
    g_free(wpath);

    Win32DirSnap *snap = g_new0(Win32DirSnap, 1);
    snap->count = arr->len;
    snap->entries = (Win32DirEntry *)g_array_free(arr, FALSE);
    snap->pos = 0;
    snap->hostpath = g_strdup(path);
    return snap;
}

static void win32_snap_closedir(Win32DirSnap *snap)
{
    if (snap) {
        g_free(snap->entries);
        g_free(snap->hostpath);
        g_free(snap);
    }
}

static struct dirent *win32_snap_readdir(Win32DirSnap *snap)
{
    if (snap->pos >= snap->count) {
        return NULL;
    }
    return &snap->entries[snap->pos++].d;
}

/*
 * telldir: return current position.  We use 1-based indices so that
 * 0 can mean "start from beginning" (consistent with seekdir(0) below).
 */
static off_t win32_snap_telldir(Win32DirSnap *snap)
{
    return (off_t)snap->pos;  /* after readdir increments, this is 1-based */
}

static void win32_snap_seekdir(Win32DirSnap *snap, off_t pos)
{
    if (pos <= 0) {
        snap->pos = 0;
    } else if (pos >= snap->count) {
        snap->pos = snap->count;
    } else {
        snap->pos = (int)pos;
    }
}

static void win32_snap_rewinddir(Win32DirSnap *snap)
{
    /* Re-snapshot to pick up filesystem changes */
    wchar_t *wpath = win32_wide_path(snap->hostpath);

    if (wpath) {
        _WDIR *d = _wopendir(wpath);

        if (d) {
            GArray *arr = win32_snap_build_entries(wpath, d);
            _wclosedir(d);

            g_free(snap->entries);
            snap->count = arr->len;
            snap->entries = (Win32DirEntry *)g_array_free(arr, FALSE);
        }
        g_free(wpath);
    }
    snap->pos = 0;
}

/* Build a full host path from the fs context and a relative 9p path */
static char *win32_full_path(FsContext *ctx, const char *path)
{
    char *result;
    if (!path || !path[0] || (path[0] == '.' && path[1] == '\0')) {
        return g_strdup(ctx->fs_root);
    }
    /* Skip leading slash to avoid double-slash in joined path */
    while (*path == '/') {
        path++;
    }
    if (!*path) {
        return g_strdup(ctx->fs_root);
    }
    result = g_strdup_printf("%s/%s", ctx->fs_root, path);
    normalize_path(result);
    return result;
}

/* Build a full host path from a directory path and a name */
static char *win32_full_path_name(FsContext *ctx, const char *dir_path,
                                  const char *name)
{
    char *result;
    g_autofree char *converted_name = NULL;

    /* Mac names may hold characters NTFS rejects (e.g. "Icon\r") */
    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        converted_name = win32_name_to_host(name);
        name = converted_name;
    }

    if (!dir_path || !dir_path[0] ||
        (dir_path[0] == '.' && dir_path[1] == '\0')) {
        result = g_strdup_printf("%s/%s", ctx->fs_root, name);
        normalize_path(result);
        return result;
    }
    /* Skip leading slash from dir_path */
    while (*dir_path == '/') {
        dir_path++;
    }
    if (!*dir_path) {
        result = g_strdup_printf("%s/%s", ctx->fs_root, name);
        normalize_path(result);
        return result;
    }
    result = g_strdup_printf("%s/%s/%s", ctx->fs_root, dir_path, name);
    normalize_path(result);
    return result;
}

/*
 * Normalize path separators: convert backslashes to forward slashes.
 * QEMU/9P uses forward slashes internally.
 */
static void normalize_path(char *path)
{
    for (char *p = path; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

/*
 * Resolve '.' and '..' components in a path in-place.
 * Works with forward slashes. Does not touch the filesystem.
 * Example: "C:/qemu-shared/Trash/foo/.." -> "C:/qemu-shared/Trash"
 */
static void resolve_dotdot(char *path)
{
    char *parts[256];
    int n = 0;
    char *tmp = g_strdup(path);
    char *tok, *saveptr;

    /* Tokenize by '/' */
    for (tok = strtok_r(tmp, "/", &saveptr); tok;
         tok = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(tok, ".") == 0) {
            continue;
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0) {
                n--;
            }
        } else {
            if (n < 256) {
                parts[n++] = tok;
            }
        }
    }

    /* Rebuild */
    path[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            strcat(path, "/");
        }
        strcat(path, parts[i]);
    }
    g_free(tmp);
}

/*
 * Convert a QEMU internal path (forward slashes, e.g. "C:/qemu-shared/dir")
 * to a Windows native path with \\?\ prefix and backslashes for Win32 APIs.
 *
 * The \\?\ prefix tells Windows to skip Win32 filename parsing, which:
 *  - Allows any Unicode character in filenames (including '…', trailing dots/spaces)
 *  - Raises the MAX_PATH limit to ~32767 characters
 *  - Avoids misinterpretation of special device names (CON, PRN, etc.)
 *
 * IMPORTANT: \\?\ disables all path parsing including '.' and '..'
 * resolution, so we must resolve those before adding the prefix.
 *
 * The caller must g_free() the returned string.
 */
static char *win32_native_path(const char *path)
{
    char *native;
    char *p;

    if (!path || !path[0]) {
        return g_strdup("");
    }

    /*
     * If already prefixed (shouldn't happen, but be safe), just convert
     * slashes.
     */
    if (path[0] == '\\' && path[1] == '\\' &&
        path[2] == '?' && path[3] == '\\') {
        native = g_strdup(path);
    } else {
        /*
         * Resolve any '.' or '..' components first — the \\?\ prefix
         * disables Windows' own path canonicalization.
         */
        char *resolved = g_strdup(path);
        resolve_dotdot(resolved);
        native = g_strdup_printf("\\\\?\\%s", resolved);
        g_free(resolved);
    }

    /* Convert forward slashes to backslashes (required for \\?\ paths) */
    for (p = native + 4; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    return native;
}

/*
 * Wrappers around CRT/Win32 filesystem functions.
 *
 * These convert UTF-8 paths to UTF-16 wide strings and call the wide ("W")
 * API variants.  The narrow ("A") CRT functions (stat, open, mkdir, etc.)
 * interpret bytes through the system ANSI codepage (Windows-1252), which
 * corrupts multi-byte UTF-8 filenames.  The wide APIs handle Unicode
 * correctly.
 *
 * IMPORTANT: MinGW CRT functions (_wopen, _wmkdir, _wrmdir, _wunlink,
 * _wopendir, etc.) do NOT support the \\?\ extended path prefix — they
 * internally call _wfullpath() which fails on \\?\ paths.  Only native
 * Win32 APIs (CreateFileW, GetFileAttributesExW, MoveFileExW, etc.)
 * support the prefix.  We therefore use two helpers:
 *
 *   win32_wide_path()   — plain UTF-16 path for CRT functions
 *   win32_wide_native() — \\?\ prefixed UTF-16 path for Win32 APIs
 */

/*
 * Convert a UTF-8 string to a newly allocated wide-char (UTF-16) string.
 * Caller must g_free() the result.  Returns NULL on failure.
 */
static wchar_t *utf8_to_wstr(const char *utf8)
{
    int wlen;
    wchar_t *wstr;

    if (!utf8) {
        return NULL;
    }
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) {
        return NULL;
    }
    wstr = g_new(wchar_t, wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, wlen) <= 0) {
        g_free(wstr);
        return NULL;
    }
    return wstr;
}

/*
 * Convert a UTF-8 path to a wide string suitable for CRT functions.
 * No \\?\ prefix — just normalize slashes to backslashes.
 * Caller must g_free() the result.
 */
static wchar_t *win32_wide_path(const char *path)
{
    char *copy = g_strdup(path);
    char *p;
    wchar_t *wpath;

    /* Normalize to backslashes for Windows CRT */
    for (p = copy; *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
    wpath = utf8_to_wstr(copy);
    g_free(copy);
    return wpath;
}

/*
 * Convert a UTF-8 path to a wide string with \\?\ prefix for Win32 APIs.
 * Only use with native Win32 functions (CreateFileW, GetFileAttributesExW,
 * MoveFileExW, etc.) — never with CRT functions.
 * Caller must g_free() the result.
 */
static wchar_t *win32_wide_native(const char *path)
{
    char *native = win32_native_path(path);
    wchar_t *wpath = utf8_to_wstr(native);
    g_free(native);
    return wpath;
}

static int win32_stat(const char *path, struct stat *stbuf)
{
    wchar_t *wpath = win32_wide_native(path);
    WIN32_FILE_ATTRIBUTE_DATA fdata;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }

    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fdata)) {
        DWORD err = GetLastError();
        g_free(wpath);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            errno = ENOENT;
        } else if (err == ERROR_ACCESS_DENIED) {
            errno = EACCES;
        } else {
            errno = EIO;
        }
        return -1;
    }

    memset(stbuf, 0, sizeof(*stbuf));

    /* File size */
    stbuf->st_size = ((int64_t)fdata.nFileSizeHigh << 32) | fdata.nFileSizeLow;

    /* Mode: directory vs regular file */
    if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
            stbuf->st_mode &= ~0222;
        }
    }

    /*
     * Populate st_ino, st_dev, and st_rdev from NTFS metadata.
     *
     * st_dev gets the volume serial number (same for all files on the
     * same volume), needed for QEMU's multi-device detection.
     *
     * st_ino gets the low 16 bits of the NTFS file index (that's all
     * MinGW's 'unsigned short' st_ino can hold).
     *
     * st_rdev gets the low 32 bits of the NTFS file index.  This is
     * used by stat_to_qid() on Windows to generate a 32-bit qid.path,
     * bypassing the too-small st_ino.  st_rdev is unused on Windows
     * (no block/char devices) so this is safe.
     *
     * We open a handle with FILE_FLAG_BACKUP_SEMANTICS (required for
     * directories) and minimal access rights.
     */
    {
        HANDLE h = CreateFileW(
            wpath,
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,  /* needed to open directories */
            NULL);
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(h, &info)) {
                uint64_t ntfs_idx = ((uint64_t)info.nFileIndexHigh << 32) |
                                    info.nFileIndexLow;
                stbuf->st_ino = (unsigned short)(ntfs_idx & 0xFFFF);
                stbuf->st_rdev = (unsigned int)(ntfs_idx & 0xFFFFFFFF);
                stbuf->st_dev = info.dwVolumeSerialNumber;
                stbuf->st_nlink = info.nNumberOfLinks;
            }
            CloseHandle(h);
        }
    }

    g_free(wpath);

    /* Time conversion: FILETIME (100ns since 1601) to time_t (seconds since 1970) */
#define FILETIME_TO_TIMET(ft) \
    ((time_t)((((uint64_t)(ft).dwHighDateTime << 32) | \
               (ft).dwLowDateTime) / 10000000ULL - 11644473600ULL))

    stbuf->st_atime = FILETIME_TO_TIMET(fdata.ftLastAccessTime);
    stbuf->st_mtime = FILETIME_TO_TIMET(fdata.ftLastWriteTime);
    stbuf->st_ctime = FILETIME_TO_TIMET(fdata.ftCreationTime);

#undef FILETIME_TO_TIMET

    return 0;
}

static int win32_open(const char *path, int flags, int mode)
{
    wchar_t *wpath = win32_wide_path(path);
    int ret;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }
    ret = _wopen(wpath, flags, mode);
    g_free(wpath);
    return ret;
}

static int win32_mkdir(const char *path)
{
    wchar_t *wpath = win32_wide_path(path);
    int ret;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }
    ret = _wmkdir(wpath);
    g_free(wpath);
    return ret;
}

static int win32_rmdir(const char *path)
{
    wchar_t *wpath = win32_wide_path(path);
    int ret;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }
    ret = _wrmdir(wpath);
    g_free(wpath);
    return ret;
}

static int win32_unlink(const char *path)
{
    wchar_t *wpath = win32_wide_path(path);
    int ret;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }
    ret = _wunlink(wpath);
    g_free(wpath);
    return ret;
}

static int win32_utime(const char *path, struct utimbuf *times)
{
    wchar_t *wpath = win32_wide_native(path);
    HANDLE h;
    FILETIME atime, mtime;
    int64_t ticks;

    if (!wpath) {
        errno = EINVAL;
        return -1;
    }

    h = CreateFileW(wpath, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, /* needed for directories */
                    NULL);
    g_free(wpath);

    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }

    /* Convert time_t to FILETIME (100ns intervals since 1601-01-01) */
#define TIMET_TO_FILETIME(t, ft) do { \
    ticks = ((int64_t)(t) + 11644473600LL) * 10000000LL; \
    (ft).dwLowDateTime = (DWORD)ticks; \
    (ft).dwHighDateTime = (DWORD)(ticks >> 32); \
} while (0)

    TIMET_TO_FILETIME(times->actime, atime);
    TIMET_TO_FILETIME(times->modtime, mtime);

#undef TIMET_TO_FILETIME

    if (!SetFileTime(h, NULL, &atime, &mtime)) {
        CloseHandle(h);
        errno = EIO;
        return -1;
    }

    CloseHandle(h);
    return 0;
}

static DWORD win32_GetFileAttributes(const char *path)
{
    wchar_t *wpath = win32_wide_native(path);
    DWORD ret;

    if (!wpath) {
        return INVALID_FILE_ATTRIBUTES;
    }
    ret = GetFileAttributesW(wpath);
    g_free(wpath);
    return ret;
}

static BOOL win32_MoveFileEx(const char *old_path, const char *new_path,
                             DWORD flags)
{
    wchar_t *wold = win32_wide_native(old_path);
    wchar_t *wnew = win32_wide_native(new_path);
    BOOL ret;

    if (!wold || !wnew) {
        g_free(wold);
        g_free(wnew);
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    ret = MoveFileExW(wold, wnew, flags);
    g_free(wold);
    g_free(wnew);
    return ret;
}

static BOOL win32_CreateHardLink(const char *new_path, const char *old_path)
{
    wchar_t *wnew = win32_wide_native(new_path);
    wchar_t *wold = win32_wide_native(old_path);
    BOOL ret;

    if (!wnew || !wold) {
        g_free(wnew);
        g_free(wold);
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    ret = CreateHardLinkW(wnew, wold, NULL);
    g_free(wnew);
    g_free(wold);
    return ret;
}

static BOOL win32_GetDiskFreeSpaceEx(const char *path,
                                      PULARGE_INTEGER freeBytesAvailable,
                                      PULARGE_INTEGER totalBytes,
                                      PULARGE_INTEGER totalFreeBytes)
{
    wchar_t *wpath = win32_wide_native(path);
    BOOL ret;

    if (!wpath) {
        return FALSE;
    }
    ret = GetDiskFreeSpaceExW(wpath, freeBytesAvailable,
                               totalBytes, totalFreeBytes);
    g_free(wpath);
    return ret;
}

/*
 * Windows stat doesn't populate st_ino (always 0). The 9P protocol
 * needs unique inode numbers to generate distinct QIDs for each file.
 * We hash the full path to produce a synthetic inode number.
 */
static ino_t win32_inode_from_path(const char *path)
{
    /*
     * FNV-1a 64-bit hash. Simple, fast, good distribution.
     * Collisions are theoretically possible but extremely unlikely
     * for typical directory trees.
     */
    uint64_t hash = 14695981039346656037ULL;
    for (const char *p = path; *p; p++) {
        char c = *p;
        /* Normalize path separators for consistent hashing */
        if (c == '\\') {
            c = '/';
        }
        hash ^= (uint64_t)(unsigned char)c;
        hash *= 1099511628211ULL;
    }
    /* Avoid 0 since that means "no inode" */
    if (hash == 0) {
        hash = 1;
    }
    return (ino_t)hash;
}

static void fixup_stat(struct stat *stbuf, const char *path)
{
    /* Ensure uid/gid are 0 (root) for security_model=none */
    stbuf->st_uid = 0;
    stbuf->st_gid = 0;
    /* Windows doesn't track hard link count for regular files */
    if (stbuf->st_nlink == 0) {
        stbuf->st_nlink = 1;
    }
    /* Generate a synthetic inode number from the path */
    if (path) {
        stbuf->st_ino = win32_inode_from_path(path);
    }
    /* Use a fixed device number to avoid multi-device warnings */
    stbuf->st_dev = 0x5039;
}

/*
 * Convert a Windows error code to an errno value that Linux guests expect.
 */
static void win32_set_errno(void)
{
    DWORD err = GetLastError();
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_DIRECTORY:
        errno = ENOTDIR;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        errno = EINVAL;
        break;
    case ERROR_TOO_MANY_OPEN_FILES:
        errno = EMFILE;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EBUSY;
        break;
    default:
        errno = EIO;
        break;
    }
}

/* ================================================================
 * FileOperations implementation
 * ================================================================ */

static int local_parse_opts(QemuOpts *opts, FsDriverEntry *fse, Error **errp)
{
    ERRP_GUARD();
    const char *sec_model = qemu_opt_get(opts, "security_model");
    const char *path = qemu_opt_get(opts, "path");

    uint64_t val = qemu_opt_get_number(opts, "max_xattr",
                                       V9FS_MAX_XATTR_DEFAULT);
    if (val > UINT32_MAX) {
        error_setg(errp, "max_xattr value '%s' too large",
                   qemu_opt_get(opts, "max_xattr"));
        return -1;
    }
    fse->max_xattr = val;

    if (!sec_model) {
        error_setg(errp, "security_model property not set");
        return -1;
    }

    if (!strcmp(sec_model, "none")) {
        fse->export_flags |= V9FS_SM_NONE;
    } else if (!strcmp(sec_model, "mapped") ||
               !strcmp(sec_model, "mapped-xattr")) {
        fse->export_flags |= V9FS_SM_MAPPED;
    } else {
        error_setg(errp,
                   "On Windows, only security_model=none or mapped-xattr "
                   "is supported (got '%s')", sec_model);
        return -1;
    }

    if (qemu_opt_get_bool(opts, "macos_compat", false)) {
        fse->export_flags |= V9FS_SM_MACOS_COMPAT;
        if (strcmp(sec_model, "none") != 0) {
            warn_report("macos_compat works best with security_model=none");
        }
    }

    if (!path) {
        error_setg(errp, "path property not set");
        return -1;
    }

    if (fsdev_throttle_parse_opts(opts, &fse->fst, errp)) {
        error_prepend(errp, "invalid throttle configuration: ");
        return -1;
    }

    fse->path = g_strdup(path);
    return 0;
}

static int local_init(FsContext *ctx, Error **errp)
{
    DWORD attrs;

    /* Normalize backslashes in fs_root to forward slashes */
    normalize_path(ctx->fs_root);

    /* Strip trailing slash from fs_root (unless it's just "C:/") */
    {
        size_t len = strlen(ctx->fs_root);
        while (len > 1 && ctx->fs_root[len - 1] == '/') {
            /* Keep the slash after drive letter, e.g. "C:/" */
            if (len == 3 && ctx->fs_root[1] == ':') {
                break;
            }
            ctx->fs_root[--len] = '\0';
        }
    }

    /* Verify the export path exists and is a directory */
    attrs = win32_GetFileAttributes(ctx->fs_root);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        error_setg_win32(errp, GetLastError(),
                         "Cannot access 9p export path '%s'", ctx->fs_root);
        return -1;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error_setg(errp, "9p export path '%s' is not a directory",
                   ctx->fs_root);
        return -1;
    }

    ctx->xops = none_xattr_ops;
    ctx->export_flags |= V9FS_PATHNAME_FSCONTEXT;

    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        info_report("9pfs: macOS compatibility mode enabled for '%s'",
                    ctx->fs_root);
    }

    return 0;
}

static void local_cleanup(FsContext *ctx)
{
    /* Nothing to clean up on Windows */
}

static int local_lstat(FsContext *fs_ctx, V9fsPath *fs_path, struct stat *stbuf)
{
    char *path = win32_full_path(fs_ctx, fs_path->data);
    int ret;

    ret = win32_stat(path, stbuf);
    if (ret == 0) {
        fixup_stat(stbuf, path);
    }
    g_free(path);
    return ret;
}

static ssize_t local_readlink(FsContext *fs_ctx, V9fsPath *fs_path,
                              char *buf, size_t bufsz)
{
    /* Windows doesn't support symlinks in the traditional POSIX sense */
    errno = ENOTSUP;
    return -1;
}

static int local_close(FsContext *ctx, V9fsFidOpenState *fs)
{
    return close(fs->fd);
}

static int local_closedir(FsContext *ctx, V9fsFidOpenState *fs)
{
    Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
    win32_snap_closedir(snap);
    fs->dir.stream = NULL;
    return 0;
}

static int local_open(FsContext *ctx, V9fsPath *fs_path,
                      int flags, V9fsFidOpenState *fs)
{
    char *path = win32_full_path(ctx, fs_path->data);
    int fd;

    /* Strip POSIX-only flags that don't exist on Windows */
    flags &= ~(O_NOCTTY | O_NONBLOCK);

    fd = win32_open(path, flags | O_BINARY, 0666);
    g_free(path);

    if (fd == -1) {
        return -1;
    }
    fs->fd = fd;
    return fs->fd;
}

static int local_opendir(FsContext *ctx, V9fsPath *fs_path,
                         V9fsFidOpenState *fs)
{
    char *path = win32_full_path(ctx, fs_path->data);
    Win32DirSnap *snap;

    snap = win32_snap_opendir(path);
    g_free(path);

    if (!snap) {
        return -1;
    }
    /* Cast to DIR* — the 9P core only stores/checks for NULL */
    fs->dir.stream = (DIR *)snap;
    return 0;
}

static void local_rewinddir(FsContext *ctx, V9fsFidOpenState *fs)
{
    Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
    win32_snap_rewinddir(snap);
}

static off_t local_telldir(FsContext *ctx, V9fsFidOpenState *fs)
{
    Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
    return win32_snap_telldir(snap);
}

static struct dirent *local_readdir(FsContext *ctx, V9fsFidOpenState *fs)
{
    Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
    struct dirent *entry;
    Win32DirEntry *we;

again:
    if (snap->pos >= snap->count) {
        return NULL;
    }
    /*
     * Get Win32DirEntry BEFORE readdir increments pos, so we can
     * access the cached NTFS inode.
     */
    we = &snap->entries[snap->pos];
    entry = win32_snap_readdir(snap);  /* increments snap->pos */
    if (!entry) {
        return NULL;
    }

    /* Skip . and .. */
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
        goto again;
    }

    /* If macos_compat is enabled, hide sidecar directories */
    if ((ctx->export_flags & V9FS_SM_MACOS_COMPAT) &&
        macos_is_sidecar_dir(entry->d_name)) {
        goto again;
    }

    /* undo the SFM mapping of win32_name_to_host() */
    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        g_autofree char *guest_name = win32_name_to_guest(entry->d_name);
        g_strlcpy(entry->d_name, guest_name, sizeof(entry->d_name));
    }

    /*
     * Store the low 32 bits of the NTFS file index in d_ino.
     * stat_to_qid() on Windows produces qid.path from st_rdev (also
     * the low 32 bits).  v9fs_do_readdir() does memset(&qid.path,0,8)
     * then memcpy(&qid.path, &d_ino, 4), producing the same value.
     *
     * Store the snapshot position in d_namlen for qemu_dirent_off().
     */
    entry->d_ino = (long)(we->ino & 0xFFFFFFFF);
    entry->d_namlen = (unsigned short)win32_snap_telldir(snap);

    return entry;
}

static void local_seekdir(FsContext *ctx, V9fsFidOpenState *fs, off_t off)
{
    Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
    /*
     * Seeking to offset 0 means "restart from the beginning".
     * Re-snapshot to pick up any new entries.
     */
    if (off == 0) {
        win32_snap_rewinddir(snap);
        return;
    }
    win32_snap_seekdir(snap, off);
}

static ssize_t local_preadv(FsContext *ctx, V9fsFidOpenState *fs,
                            const struct iovec *iov, int iovcnt, off_t offset)
{
    ssize_t total = 0;

    if (lseek(fs->fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    for (int i = 0; i < iovcnt; i++) {
        int bytes = read(fs->fd, iov[i].iov_base, iov[i].iov_len);
        if (bytes < 0) {
            return total > 0 ? total : -1;
        }
        total += bytes;
        if ((size_t)bytes < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

static ssize_t local_pwritev(FsContext *ctx, V9fsFidOpenState *fs,
                             const struct iovec *iov, int iovcnt, off_t offset)
{
    ssize_t total = 0;

    if (lseek(fs->fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }

    for (int i = 0; i < iovcnt; i++) {
        int bytes = write(fs->fd, iov[i].iov_base, iov[i].iov_len);
        if (bytes < 0) {
            return total > 0 ? total : -1;
        }
        total += bytes;
        if ((size_t)bytes < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

static int local_chmod(FsContext *fs_ctx, V9fsPath *fs_path, FsCred *credp)
{
    /* Windows doesn't support POSIX permissions; silently succeed */
    return 0;
}

static int local_mknod(FsContext *fs_ctx, V9fsPath *dir_path,
                       const char *name, FsCred *credp)
{
    char *path = win32_full_path_name(fs_ctx, dir_path->data, name);
    int fd;

    /* On Windows, we can only create regular files */
    fd = win32_open(path, O_CREAT | O_WRONLY | O_EXCL | O_BINARY, 0666);
    g_free(path);

    if (fd == -1) {
        return -1;
    }
    close(fd);
    return 0;
}

static int local_mkdir(FsContext *fs_ctx, V9fsPath *dir_path,
                       const char *name, FsCred *credp)
{
    char *path = win32_full_path_name(fs_ctx, dir_path->data, name);
    int ret;

    ret = win32_mkdir(path);
    g_free(path);
    return ret;
}

static int local_fstat(FsContext *fs_ctx, int fid_type,
                       V9fsFidOpenState *fs, struct stat *stbuf)
{
    int ret;

    if (fid_type == P9_FID_DIR) {
        /*
         * Windows cannot fstat() a DIR stream.
         * Use the snapshot's stored host path to stat() instead.
         */
        const char *dirpath = NULL;
        if (fs->dir.stream) {
            Win32DirSnap *snap = (Win32DirSnap *)fs->dir.stream;
            dirpath = snap->hostpath;
        }
        if (dirpath) {
            ret = win32_stat(dirpath, stbuf);
            if (ret == 0) {
                fixup_stat(stbuf, dirpath);
            }
        } else {
            /* Fallback: return synthetic directory stat */
            memset(stbuf, 0, sizeof(*stbuf));
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            fixup_stat(stbuf, NULL);
            ret = 0;
        }
        return ret;
    }

    if (fid_type != P9_FID_FILE) {
        errno = EBADF;
        return -1;
    }
    ret = fstat(fs->fd, stbuf);
    if (ret == 0) {
        fixup_stat(stbuf, NULL);
    }
    return ret;
}

static int local_open2(FsContext *fs_ctx, V9fsPath *dir_path, const char *name,
                       int flags, FsCred *credp, V9fsFidOpenState *fs)
{
    char *path = win32_full_path_name(fs_ctx, dir_path->data, name);
    int fd;

    /* Strip POSIX-only flags */
    flags &= ~(O_NOCTTY | O_NONBLOCK);

    fd = win32_open(path, flags | O_BINARY, 0666);
    if (fd == -1) {
        g_free(path);
        return -1;
    }
    g_free(path);
    fs->fd = fd;
    return fs->fd;
}

static int local_symlink(FsContext *fs_ctx, const char *oldpath,
                         V9fsPath *dir_path, const char *name, FsCred *credp)
{
    /* Symlinks not supported on Windows */
    errno = ENOTSUP;
    return -1;
}

static int local_link(FsContext *ctx, V9fsPath *oldpath,
                      V9fsPath *dir_path, const char *name)
{
    /* Hard links: possible on NTFS but not commonly used */
    char *old_full = win32_full_path(ctx, oldpath->data);
    char *new_full = win32_full_path_name(ctx, dir_path->data, name);
    int ret;

    if (win32_CreateHardLink(new_full, old_full)) {
        ret = 0;
    } else {
        win32_set_errno();
        ret = -1;
    }
    g_free(old_full);
    g_free(new_full);
    return ret;
}

static int local_truncate(FsContext *ctx, V9fsPath *fs_path, off_t size)
{
    char *path = win32_full_path(ctx, fs_path->data);
    int fd, ret;

    fd = win32_open(path, O_WRONLY | O_BINARY, 0);
    g_free(path);
    if (fd == -1) {
        return -1;
    }
    ret = ftruncate(fd, size);
    close(fd);
    return ret;
}

static int local_ftruncate(FsContext *ctx, int fid_type, V9fsFidOpenState *fs,
                           off_t size)
{
    if (fid_type == P9_FID_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (fid_type != P9_FID_FILE) {
        errno = EBADF;
        return -1;
    }
    return ftruncate(fs->fd, size);
}

static int local_chown(FsContext *fs_ctx, V9fsPath *fs_path, FsCred *credp)
{
    /* Windows doesn't support POSIX chown; silently succeed */
    return 0;
}

static int local_utimensat(FsContext *s, V9fsPath *fs_path,
                           const struct timespec *buf)
{
    char *path = win32_full_path(s, fs_path->data);
    struct utimbuf times;
    int ret;

    times.actime = buf[0].tv_sec;
    times.modtime = buf[1].tv_sec;
    ret = win32_utime(path, &times);
    g_free(path);
    return ret;
}

static int local_futimens(FsContext *s, int fid_type, V9fsFidOpenState *fs,
                          const struct timespec *buf)
{
    /*
     * Windows doesn't have futimens. Use _futime or just succeed.
     * The guest doesn't rely heavily on exact timestamps.
     */
    return 0;
}

static int local_remove(FsContext *ctx, const char *path)
{
    char *full = win32_full_path(ctx, path);
    DWORD attrs;
    int ret;

    attrs = win32_GetFileAttributes(full);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        g_free(full);
        errno = ENOENT;
        return -1;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        ret = win32_rmdir(full) == 0 ? 0 : -1;
    } else {
        ret = win32_unlink(full) == 0 ? 0 : -1;
    }
    g_free(full);
    return ret;
}

static int local_fsync(FsContext *ctx, int fid_type,
                       V9fsFidOpenState *fs, int datasync)
{
    if (fid_type == P9_FID_DIR) {
        return 0; /* Can't fsync a dir on Windows */
    }
    if (fid_type != P9_FID_FILE) {
        errno = EBADF;
        return -1;
    }
    return _commit(fs->fd);
}

static int local_statfs(FsContext *s, V9fsPath *fs_path,
                        struct statfs *stbuf)
{
    char *path = win32_full_path(s, fs_path->data);
    ULARGE_INTEGER freeBytesAvailable, totalBytes, totalFreeBytes;

    memset(stbuf, 0, sizeof(*stbuf));

    if (win32_GetDiskFreeSpaceEx(path, &freeBytesAvailable,
                            &totalBytes, &totalFreeBytes)) {
        stbuf->f_type = 0x65735546; /* "Fuse" magic - arbitrary */
        stbuf->f_bsize = 4096;
        stbuf->f_blocks = totalBytes.QuadPart / 4096;
        stbuf->f_bfree = totalFreeBytes.QuadPart / 4096;
        stbuf->f_bavail = freeBytesAvailable.QuadPart / 4096;
        stbuf->f_files = 0;
        stbuf->f_ffree = 0;
        stbuf->f_namelen = MAX_PATH;
    } else {
        win32_set_errno();
        g_free(path);
        return -1;
    }

    g_free(path);
    return 0;
}

/* ================================================================
 * Xattr operations - intercepted by macos_compat when enabled
 * ================================================================ */

static ssize_t local_lgetxattr(FsContext *ctx, V9fsPath *fs_path,
                               const char *name, void *value, size_t size)
{
    /*
     * macOS compat: intercept Apple xattr requests.
     * Uses NTFS Alternate Data Streams via 9p-macos-compat.c.
     */
    if ((ctx->export_flags & V9FS_SM_MACOS_COMPAT) &&
        macos_is_apple_xattr(name)) {
        char *path = win32_full_path(ctx, fs_path->data);
        char *dirpath = g_path_get_dirname(path);
        char *fname = g_path_get_basename(path);
        struct stat stbuf;
        ssize_t ret;

        normalize_path(dirpath);
        bool is_dir = false;
        if (win32_stat(path, &stbuf) == 0) {
            is_dir = S_ISDIR(stbuf.st_mode);
        }

        if (macos_is_finderinfo_xattr(name)) {
            ret = macos_get_finderinfo(dirpath, fname,
                                       is_dir, value, size);
        } else {
            ret = macos_get_resourcefork(dirpath, fname, value, size, 0);
            if (ret == 0) {
                ret = -ENODATA;     /* no fork */
            }
        }

        g_free(fname);
        g_free(dirpath);
        g_free(path);
        if (ret < 0) {
            errno = -ret;
            return -1;
        }
        return ret;
    }

    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        g_autofree char *path = win32_full_path(ctx, fs_path->data);
        g_autofree char *dirpath = g_path_get_dirname(path);
        g_autofree char *fname = g_path_get_basename(path);

        normalize_path(dirpath);
        return win32_get_xattr(dirpath, fname, name, value, size);
    }
    errno = ENODATA;
    return -1;
}

static ssize_t local_llistxattr(FsContext *ctx, V9fsPath *fs_path,
                                void *value, size_t size)
{
    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        char *path = win32_full_path(ctx, fs_path->data);
        char *dirpath = g_path_get_dirname(path);
        char *fname = g_path_get_basename(path);
        struct stat stbuf;
        ssize_t ret;

        normalize_path(dirpath);
        bool is_dir = false;
        if (win32_stat(path, &stbuf) == 0) {
            is_dir = S_ISDIR(stbuf.st_mode);
        }

        ret = macos_list_xattrs(dirpath, fname,
                                is_dir, value, size);
        g_free(fname);
        g_free(dirpath);
        g_free(path);
        return ret;
    }

    /* No xattr support without macos_compat on Windows */
    if (value && size > 0) {
        memset(value, 0, 1);
    }
    return 0;
}

static int local_lsetxattr(FsContext *ctx, V9fsPath *fs_path,
                           const char *name, void *value,
                           size_t size, int flags)
{
    if ((ctx->export_flags & V9FS_SM_MACOS_COMPAT) &&
        macos_is_apple_xattr(name)) {
        char *path = win32_full_path(ctx, fs_path->data);
        char *dirpath = g_path_get_dirname(path);
        char *fname = g_path_get_basename(path);
        int ret;

        normalize_path(dirpath);

        if (macos_is_finderinfo_xattr(name)) {
            ret = macos_set_finderinfo(dirpath, fname, value, size);
        } else {
            ret = macos_set_resourcefork(dirpath, fname, value, size, 0);
        }

        g_free(fname);
        g_free(dirpath);
        g_free(path);
        if (ret < 0) {
            errno = -ret;
            return -1;
        }
        return 0;
    }

    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        g_autofree char *path = win32_full_path(ctx, fs_path->data);
        g_autofree char *dirpath = g_path_get_dirname(path);
        g_autofree char *fname = g_path_get_basename(path);

        normalize_path(dirpath);
        return win32_set_xattr(dirpath, fname, name, value, size, flags);
    }
    errno = ENOTSUP;
    return -1;
}

static int local_lremovexattr(FsContext *ctx, V9fsPath *fs_path,
                              const char *name)
{
    g_autofree char *path = NULL;
    g_autofree char *dirpath = NULL;
    g_autofree char *fname = NULL;

    if (!(ctx->export_flags & V9FS_SM_MACOS_COMPAT)) {
        errno = ENOTSUP;
        return -1;
    }
    path = win32_full_path(ctx, fs_path->data);
    dirpath = g_path_get_dirname(path);
    fname = g_path_get_basename(path);
    normalize_path(dirpath);
    if (macos_is_apple_xattr(name)) {
        macos_remove_apple_xattr(dirpath, fname,
                                 !macos_is_finderinfo_xattr(name));
        return 0;
    }
    return win32_remove_xattr(dirpath, fname, name);
}

/* ================================================================
 * Path and rename operations
 * ================================================================ */

static int local_name_to_path(FsContext *ctx, V9fsPath *dir_path,
                              const char *name, V9fsPath *target)
{
    g_autofree char *converted_name = NULL;

    /* the stored 9P path must match the name on the Windows filesystem */
    if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
        converted_name = win32_name_to_host(name);
        name = converted_name;
    }

    if (dir_path) {
        const char *d = dir_path->data;
        size_t dlen = strlen(d);
        /* Strip trailing slashes from dir to avoid "dir//name" */
        while (dlen > 1 && d[dlen - 1] == '/') {
            dlen--;
        }
        /* Skip leading slashes from name */
        while (*name == '/') {
            name++;
        }
        if (v9fs_path_sprintf(target, "%.*s/%s", (int)dlen, d, name) < 0) {
            return -1;
        }
    } else if (name[0] == '/') {
        /* Absolute path from attach */
        if (v9fs_path_sprintf(target, "%s", name + 1) < 0) {
            return -1;
        }
    } else {
        if (v9fs_path_sprintf(target, "%s", name) < 0) {
            return -1;
        }
    }

    /* Resolve any '.' or '..' components in the 9P path */
    resolve_dotdot(target->data);

    /* Normalize: remove trailing slashes */
    char *p = target->data;
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/') {
        p[--len] = '\0';
    }
    target->size = len + 1;

    return 0;
}

static int local_rename(FsContext *ctx, const char *oldpath,
                        const char *newpath)
{
    char *old_full = win32_full_path(ctx, oldpath);
    char *new_full = win32_full_path(ctx, newpath);
    int ret;

    /*
     * On Windows, rename() fails if the target exists.
     * Use MoveFileEx with MOVEFILE_REPLACE_EXISTING.
     */
    if (win32_MoveFileEx(old_full, new_full, MOVEFILE_REPLACE_EXISTING)) {
        ret = 0;
    } else {
        win32_set_errno();
        ret = -1;
    }
    g_free(old_full);
    g_free(new_full);
    return ret;
}

static int local_renameat(FsContext *ctx, V9fsPath *olddir,
                          const char *old_name, V9fsPath *newdir,
                          const char *new_name)
{
    char *old_full = win32_full_path_name(ctx, olddir->data, old_name);
    char *new_full = win32_full_path_name(ctx, newdir->data, new_name);
    int ret;

    if (win32_MoveFileEx(old_full, new_full, MOVEFILE_REPLACE_EXISTING)) {
        ret = 0;

        /* If macos_compat, also rename sidecars (use converted names) */
        if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
            char *old_dir = g_path_get_dirname(old_full);
            char *new_dir = g_path_get_dirname(new_full);
            char *old_base = g_path_get_basename(old_full);
            char *new_base = g_path_get_basename(new_full);
            normalize_path(old_dir);
            normalize_path(new_dir);
            macos_rename_sidecars(old_dir, old_base, new_dir, new_base);
            g_free(old_base);
            g_free(new_base);
            g_free(old_dir);
            g_free(new_dir);
        }
    } else {
        win32_set_errno();
        ret = -1;
    }
    g_free(old_full);
    g_free(new_full);
    return ret;
}

static int local_unlinkat(FsContext *ctx, V9fsPath *dir,
                          const char *name, int flags)
{
    char *full = win32_full_path_name(ctx, dir->data, name);
    int ret;

    if (flags & P9_DOTL_AT_REMOVEDIR) {
        /*
         * When macos_compat is enabled, directories may contain hidden
         * sidecar files/dirs that the guest doesn't know about (e.g.
         * ._Icon\r for custom folder icons).  Clean these up before
         * rmdir, otherwise Windows will fail with ENOTEMPTY.
         * Also remove the directory's own sidecar in the parent.
         */
        if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
            /*
             * Remove sidecar directories (.finf, .rsrc) inside the
             * directory being deleted.  These are created transparently
             * by the macos_compat layer to store Finder info and resource
             * forks.  The guest doesn't see them (they're filtered from
             * readdir) so it can't delete them, but they prevent rmdir
             * from succeeding on Windows.
             *
             * Each sidecar dir may itself contain leftover entries, so
             * we do a shallow recursive clean: delete all files inside
             * each sidecar dir, then rmdir the sidecar dir itself.
             */
            wchar_t *wfull = win32_wide_path(full);
            if (wfull) {
                _WDIR *d = _wopendir(wfull);
                if (d) {
                    struct _wdirent *went;
                    while ((went = _wreaddir(d)) != NULL) {
                        char child_utf8[256];
                        WideCharToMultiByte(CP_UTF8, 0, went->d_name, -1,
                                            child_utf8, sizeof(child_utf8),
                                            NULL, NULL);
                        if (!macos_is_sidecar_dir(child_utf8)) {
                            continue;
                        }
                        /* This is a .finf or .rsrc dir — clean it out */
                        wchar_t subdir[MAX_PATH + 1];
                        _snwprintf(subdir, MAX_PATH, L"%s\\%s",
                                   wfull, went->d_name);
                        subdir[MAX_PATH] = L'\0';
                        _WDIR *sd = _wopendir(subdir);
                        if (sd) {
                            struct _wdirent *se;
                            while ((se = _wreaddir(sd)) != NULL) {
                                if (se->d_name[0] == L'.' &&
                                    (se->d_name[1] == L'\0' ||
                                     (se->d_name[1] == L'.' &&
                                      se->d_name[2] == L'\0'))) {
                                    continue;
                                }
                                wchar_t child[MAX_PATH + 1];
                                _snwprintf(child, MAX_PATH, L"%s\\%s",
                                           subdir, se->d_name);
                                child[MAX_PATH] = L'\0';
                                SetFileAttributesW(child,
                                    FILE_ATTRIBUTE_NORMAL);
                                _wunlink(child);
                            }
                            _wclosedir(sd);
                        }
                        _wrmdir(subdir);
                    }
                    _wclosedir(d);
                }
                g_free(wfull);
            }

            /* Remove the directory's own sidecar in the parent */
            char *dirpath = g_path_get_dirname(full);
            char *base = g_path_get_basename(full);
            normalize_path(dirpath);
            macos_remove_sidecars(dirpath, base);
            g_free(base);
            g_free(dirpath);
        }
        ret = win32_rmdir(full) == 0 ? 0 : -1;
    } else {
        /* If macos_compat, remove sidecars first (use converted name) */
        if (ctx->export_flags & V9FS_SM_MACOS_COMPAT) {
            char *dirpath = g_path_get_dirname(full);
            char *base = g_path_get_basename(full);
            normalize_path(dirpath);
            macos_remove_sidecars(dirpath, base);
            g_free(base);
            g_free(dirpath);
        }
        ret = win32_unlink(full) == 0 ? 0 : -1;
    }
    g_free(full);

    if (ret < 0) {
        /* Convert Windows errno to what the guest expects */
        DWORD err = GetLastError();
        if (err == ERROR_DIR_NOT_EMPTY) {
            errno = ENOTEMPTY;
        }
        ret = -1;
    }
    return ret;
}

static bool local_has_valid_file_handle(int fid_type, V9fsFidOpenState *fs)
{
    return
        (fid_type == P9_FID_FILE && fs->fd != -1) ||
        (fid_type == P9_FID_DIR && fs->dir.stream != NULL);
}

/* ================================================================
 * Export the FileOperations table
 * ================================================================ */

FileOperations local_ops = {
    .parse_opts = local_parse_opts,
    .init  = local_init,
    .cleanup = local_cleanup,
    .lstat = local_lstat,
    .readlink = local_readlink,
    .close = local_close,
    .closedir = local_closedir,
    .open = local_open,
    .opendir = local_opendir,
    .rewinddir = local_rewinddir,
    .telldir = local_telldir,
    .readdir = local_readdir,
    .seekdir = local_seekdir,
    .preadv = local_preadv,
    .pwritev = local_pwritev,
    .chmod = local_chmod,
    .mknod = local_mknod,
    .mkdir = local_mkdir,
    .fstat = local_fstat,
    .open2 = local_open2,
    .symlink = local_symlink,
    .link = local_link,
    .truncate = local_truncate,
    .rename = local_rename,
    .chown = local_chown,
    .utimensat = local_utimensat,
    .remove = local_remove,
    .fsync = local_fsync,
    .statfs = local_statfs,
    .lgetxattr = local_lgetxattr,
    .llistxattr = local_llistxattr,
    .lsetxattr = local_lsetxattr,
    .lremovexattr = local_lremovexattr,
    .name_to_path = local_name_to_path,
    .renameat  = local_renameat,
    .unlinkat = local_unlinkat,
    .has_valid_file_handle = local_has_valid_file_handle,
    .ftruncate = local_ftruncate,
    .futimens = local_futimens,
};

#endif /* _WIN32 */
