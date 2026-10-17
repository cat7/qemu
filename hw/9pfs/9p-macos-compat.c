/*
 * 9p-macos-compat.c - Mac OS 9 compatibility layer for QEMU 9pfs
 *
 * Provides transparent Mac OS metadata handling on host filesystems.
 * Supports Linux (xattrs), macOS (native xattrs), and Windows (NTFS ADS).
 *
 * Extension-to-type/creator table derived from Basilisk II (Christian Bauer,
 * GNU GPL v2). See extfs_windows.cpp in the Basilisk II source tree.
 *
 * Copyright (c) 2025
 * License: GNU GPL v2 or later
 */

#include "qemu/osdep.h"
#include "9p-macos-compat.h"

#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#endif

/*
 * =========================================================================
 *  Extension-to-Type/Creator mapping table
 *
 *  Derived from Basilisk II's e2t_translation[] in extfs_windows.cpp.
 *  All FourCC values are stored big-endian (network byte order).
 * =========================================================================
 */

#ifndef _WIN32
#ifndef O_PATH
#define O_PATH 010000000
#endif
#endif

#define F(a, b, c, d) MAC_FOURCC(a, b, c, d)

const MacExtMapping mac_ext_table[] = {
    /* Archives / compression */
    { ".Z",     F('Z','I','V','M'), F('L','Z','I','V') },
    { ".gz",    F('G','z','i','p'), F('G','z','i','p') },
    { ".hqx",   F('T','E','X','T'), F('S','I','T','x') },
    { ".bin",   F('T','E','X','T'), F('S','I','T','x') },
    { ".sit",   F('S','I','T','!'), F('S','I','T','x') },
    { ".tar",   F('T','A','R','F'), F('T','A','R',' ') },
    { ".uu",    F('T','E','X','T'), F('S','I','T','x') },
    { ".uue",   F('T','E','X','T'), F('S','I','T','x') },
    { ".zip",   F('Z','I','P',' '), F('Z','I','P',' ') },

    /* Documents */
    { ".pdf",   F('P','D','F',' '), F('C','A','R','O') },
    { ".ps",    F('T','E','X','T'), F('t','t','x','t') },
    { ".htm",   F('T','E','X','T'), F('M','O','S','S') },
    { ".html",  F('T','E','X','T'), F('M','O','S','S') },
    { ".txt",   F('T','E','X','T'), F('t','t','x','t') },
    { ".rtf",   F('T','E','X','T'), F('M','S','W','D') },

    /* Source code */
    { ".c",     F('T','E','X','T'), F('R','*','c','h') },
    { ".C",     F('T','E','X','T'), F('R','*','c','h') },
    { ".cc",    F('T','E','X','T'), F('R','*','c','h') },
    { ".cpp",   F('T','E','X','T'), F('R','*','c','h') },
    { ".cxx",   F('T','E','X','T'), F('R','*','c','h') },
    { ".h",     F('T','E','X','T'), F('R','*','c','h') },
    { ".hh",    F('T','E','X','T'), F('R','*','c','h') },
    { ".hpp",   F('T','E','X','T'), F('R','*','c','h') },
    { ".hxx",   F('T','E','X','T'), F('R','*','c','h') },
    { ".s",     F('T','E','X','T'), F('R','*','c','h') },
    { ".S",     F('T','E','X','T'), F('R','*','c','h') },
    { ".asm",   F('T','E','X','T'), F('R','*','c','h') },
    { ".p",     F('T','E','X','T'), F('R','*','c','h') },
    { ".pas",   F('T','E','X','T'), F('R','*','c','h') },
    { ".java",  F('T','E','X','T'), F('R','*','c','h') },
    { ".py",    F('T','E','X','T'), F('R','*','c','h') },
    { ".pl",    F('T','E','X','T'), F('R','*','c','h') },
    { ".rb",    F('T','E','X','T'), F('R','*','c','h') },
    { ".sh",    F('T','E','X','T'), F('R','*','c','h') },
    { ".r",     F('T','E','X','T'), F('R','*','c','h') },

    /* Config/data */
    { ".xml",   F('T','E','X','T'), F('R','*','c','h') },
    { ".json",  F('T','E','X','T'), F('R','*','c','h') },
    { ".csv",   F('T','E','X','T'), F('R','*','c','h') },
    { ".cfg",   F('T','E','X','T'), F('t','t','x','t') },
    { ".ini",   F('T','E','X','T'), F('t','t','x','t') },
    { ".log",   F('T','E','X','T'), F('t','t','x','t') },

    /* Images */
    { ".jpg",   F('J','P','E','G'), F('o','g','l','e') },
    { ".jpeg",  F('J','P','E','G'), F('o','g','l','e') },
    { ".gif",   F('G','I','F','f'), F('o','g','l','e') },
    { ".png",   F('P','N','G','f'), F('o','g','l','e') },
    { ".tif",   F('T','I','F','F'), F('o','g','l','e') },
    { ".tiff",  F('T','I','F','F'), F('o','g','l','e') },
    { ".bmp",   F('B','M','P','f'), F('o','g','l','e') },
    { ".psd",   F('8','B','P','S'), F('8','B','I','M') },

    /* Audio */
    { ".wav",   F('W','A','V','E'), F('T','V','O','D') },
    { ".aif",   F('A','I','F','F'), F('T','V','O','D') },
    { ".aiff",  F('A','I','F','F'), F('T','V','O','D') },
    { ".mp3",   F('M','P','G','3'), F('T','V','O','D') },

    /* Video */
    { ".mov",   F('M','o','o','V'), F('T','V','O','D') },
    { ".mpg",   F('M','P','E','G'), F('T','V','O','D') },
    { ".mpeg",  F('M','P','E','G'), F('T','V','O','D') },
    { ".avi",   F('V','f','W',' '), F('T','V','O','D') },
};

const size_t mac_ext_table_size =
    sizeof(mac_ext_table) / sizeof(mac_ext_table[0]);

#undef F

/*
 * =========================================================================
 *  Shared helpers (platform-independent)
 * =========================================================================
 */

/*
 * Case-insensitive extension match.
 * Returns true if @name ends with @ext (case-insensitive).
 */
static bool ext_match(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);

    if (nlen < elen) {
        return false;
    }
    return strcasecmp(name + nlen - elen, ext) == 0;
}

/*
 * Look up type/creator by file extension.
 * Returns pointer to matching table entry, or NULL if no match.
 */
static const MacExtMapping *lookup_ext(const char *name)
{
    size_t i;

    for (i = 0; i < mac_ext_table_size; i++) {
        if (ext_match(name, mac_ext_table[i].ext)) {
            return &mac_ext_table[i];
        }
    }
    return NULL;
}

/*
 * Synthesize a default FinderInfo block.
 * Sets kHasBeenInited flag, location = -1 (no position).
 * For files, fills type/creator from extension table if possible.
 */
static void synth_finderinfo(const char *name, bool is_dir, uint8_t *buf)
{
    memset(buf, 0, FINDERINFO_SIZE);

    /* Set default Finder flags: kHasBeenInited */
    mac_put_be16(buf + FINFO_FDFLAGS_OFF, FINDER_FLAG_HAS_BEEN_INITED);

    /* Set fdLocation to (0xFFFF, 0xFFFF) = "no position" */
    mac_put_be32(buf + FINFO_FDLOCATION_OFF, 0xFFFFFFFF);

    if (!is_dir) {
        const MacExtMapping *m = lookup_ext(name);
        if (m) {
            mac_put_be32(buf + FINFO_FDTYPE_OFF, m->type);
            mac_put_be32(buf + FINFO_FDCREATOR_OFF, m->creator);
        }
        /* If no extension match, type/creator remain 0x00000000 which is
         * valid — Finder will show a generic document icon. */
    }
}


/*
 * =========================================================================
 *  Platform-specific internal helpers
 * =========================================================================
 */

#ifdef _WIN32

/*
 * ===== WINDOWS: NTFS Alternate Data Streams =====
 *
 * On Windows/NTFS, xattr-like metadata is stored as ADS:
 *   file.txt:com.apple.FinderInfo         → 32-byte FinderInfo
 *   file.txt:com.apple.ResourceFork       → binary resource fork
 *
 * We also support Basilisk II sidecar dirs (.finf, .rsrc) as fallback,
 * using standard Win32 file operations.
 *
 * Note: ADS names use the bare Apple names (no "user." prefix) since
 * NTFS stream names are arbitrary strings, not namespaced like Linux xattrs.
 * The "user." prefix is stripped when mapping from the 9P xattr name.
 */

/* ADS stream names (without the "user." prefix used on Linux) */
#define ADS_FINDERINFO    "com.apple.FinderInfo"
#define ADS_RESOURCEFORK  "com.apple.ResourceFork"

/*
 * =========================================================================
 *  UTF-8 → UTF-16 wide-char helpers
 *
 *  All Win32 file operations must use the wide ("W") APIs so that UTF-8
 *  encoded filenames (e.g. from Mac Roman conversion) are handled correctly.
 *  The ANSI ("A") APIs interpret bytes using the system codepage
 *  (typically Windows-1252), which silently corrupts multi-byte UTF-8.
 * =========================================================================
 */

/*
 * Convert a UTF-8 string to a newly allocated wide-char (UTF-16) string.
 * The caller must g_free() the returned pointer.
 * Returns NULL on failure.
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
 * Wide-char wrappers around Win32 APIs.
 * Each takes a UTF-8 path, converts to UTF-16, and calls the W variant.
 */
static HANDLE compat_CreateFile(const char *path, DWORD access,
                                DWORD share, DWORD creation, DWORD flags)
{
    wchar_t *wpath = utf8_to_wstr(path);
    HANDLE h;

    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }

    h = CreateFileW(wpath, access, share, NULL, creation, flags, NULL);
    g_free(wpath);
    return h;
}

static BOOL compat_DeleteFile(const char *path)
{
    wchar_t *wpath = utf8_to_wstr(path);
    BOOL ret;

    if (!wpath) {
        return FALSE;
    }

    ret = DeleteFileW(wpath);
    g_free(wpath);
    return ret;
}

static BOOL compat_CreateDirectory(const char *path)
{
    wchar_t *wpath = utf8_to_wstr(path);
    BOOL ret;

    if (!wpath) {
        return FALSE;
    }

    ret = CreateDirectoryW(wpath, NULL);
    g_free(wpath);
    return ret;
}

static DWORD compat_GetFileAttributes(const char *path)
{
    wchar_t *wpath = utf8_to_wstr(path);
    DWORD ret;

    if (!wpath) {
        return INVALID_FILE_ATTRIBUTES;
    }

    ret = GetFileAttributesW(wpath);
    g_free(wpath);
    return ret;
}

static BOOL compat_MoveFileEx(const char *old_path, const char *new_path,
                              DWORD flags)
{
    wchar_t *wold = utf8_to_wstr(old_path);
    wchar_t *wnew = utf8_to_wstr(new_path);
    BOOL ret;

    if (!wold || !wnew) {
        g_free(wold);
        g_free(wnew);
        return FALSE;
    }

    ret = MoveFileExW(wold, wnew, flags);
    g_free(wold);
    g_free(wnew);
    return ret;
}

/*
 * Build a full path to a file's ADS.
 * Given parent_path="C:\share\subdir", name="file.txt",
 * stream="com.apple.FinderInfo", produces:
 *   "C:\share\subdir\file.txt:com.apple.FinderInfo"
 *
 * Returns a g_malloc'd string, or NULL if allocation fails.
 * The caller must g_free() the result.
 */
static char *build_ads_path(const char *parent_path, const char *name,
                            const char *stream)
{
    return g_strdup_printf("%s/%s:%s", parent_path, name, stream);
}

/*
 * Build a regular file path (no ADS).
 * Returns a g_malloc'd string. Caller must g_free().
 */
static char *build_file_path(const char *parent_path, const char *name)
{
    return g_strdup_printf("%s/%s", parent_path, name);
}

/*
 * Build a sidecar file path: parent_path/sidecar_dir/name
 * Returns a g_malloc'd string. Caller must g_free().
 */
static char *build_sidecar_path(const char *parent_path,
                                const char *sidecar_dir,
                                const char *name)
{
    return g_strdup_printf("%s/%s/%s", parent_path, sidecar_dir, name);
}

/*
 * Get the full filesystem path from a dirfd.
 *
 * On Windows, our "dirfd" is actually an opaque handle or a path index.
 * QEMU's Windows 9pfs backend uses get_full_path_win32() internally.
 * We replicate a simplified version: the caller passes the parent path
 * directly through our API (see the _win32 public function signatures).
 *
 * For Windows, we change the internal API to accept (const char *dirpath)
 * instead of (int dirfd). The public functions below handle the mapping.
 */

/*
 * Read an NTFS ADS.
 * Returns bytes read, or -1 on error (sets errno).
 */
static ssize_t ads_read(const char *parent_path, const char *name,
                        const char *stream, void *buf, size_t bufsz)
{
    g_autofree char *ads_path = build_ads_path(parent_path, name, stream);
    HANDLE h;
    DWORD bytes_read = 0;

    h = compat_CreateFile(ads_path, GENERIC_READ, FILE_SHARE_READ,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            errno = ENODATA;
        } else {
            errno = EIO;
        }
        return -1;
    }

    if (!ReadFile(h, buf, (DWORD)bufsz, &bytes_read, NULL)) {
        CloseHandle(h);
        errno = EIO;
        return -1;
    }

    CloseHandle(h);
    return (ssize_t)bytes_read;
}

/*
 * Write an NTFS ADS.
 * Returns 0 on success, -1 on error (sets errno).
 */
static int ads_write(const char *parent_path, const char *name,
                     const char *stream, const void *buf, size_t size)
{
    g_autofree char *ads_path = build_ads_path(parent_path, name, stream);
    HANDLE h;
    DWORD bytes_written = 0;

    h = compat_CreateFile(ads_path, GENERIC_WRITE, 0,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EIO;
        return -1;
    }

    if (size > 0) {
        if (!WriteFile(h, buf, (DWORD)size, &bytes_written, NULL) ||
            bytes_written != (DWORD)size) {
            CloseHandle(h);
            errno = EIO;
            return -1;
        }
    }

    CloseHandle(h);
    return 0;
}

/*
 * Delete an NTFS ADS by opening it and calling DeleteFile.
 * Ignores "not found" errors.
 */
static void ads_delete(const char *parent_path, const char *name,
                       const char *stream)
{
    g_autofree char *ads_path = build_ads_path(parent_path, name, stream);
    compat_DeleteFile(ads_path); /* ignore errors */
}

/*
 * Get the size of an ADS.
 * Returns size >= 0, or -1 if the stream doesn't exist.
 */
static ssize_t ads_size(const char *parent_path, const char *name,
                        const char *stream)
{
    g_autofree char *ads_path = build_ads_path(parent_path, name, stream);
    HANDLE h;
    LARGE_INTEGER size;

    h = compat_CreateFile(ads_path, GENERIC_READ, FILE_SHARE_READ,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return -1;
    }

    CloseHandle(h);
    return (ssize_t)size.QuadPart;
}

/*
 * Sidecar helpers for Windows using standard Win32 API.
 */

static ssize_t sidecar_read_win32(const char *parent_path,
                                  const char *sidecar_dir,
                                  const char *name,
                                  void *buf, size_t bufsz, off_t offset)
{
    g_autofree char *path = build_sidecar_path(parent_path, sidecar_dir, name);
    HANDLE h;
    DWORD bytes_read = 0;
    LARGE_INTEGER li;

    h = compat_CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (offset > 0) {
        li.QuadPart = offset;
        if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            return -1;
        }
    }

    if (!ReadFile(h, buf, (DWORD)bufsz, &bytes_read, NULL)) {
        CloseHandle(h);
        return -1;
    }

    CloseHandle(h);
    return (ssize_t)bytes_read;
}

static ssize_t sidecar_write_win32(const char *parent_path,
                                   const char *sidecar_dir,
                                   const char *name,
                                   const void *buf, size_t size,
                                   off_t offset, bool truncate)
{
    g_autofree char *dirpath = build_file_path(parent_path, sidecar_dir);
    g_autofree char *path = build_sidecar_path(parent_path, sidecar_dir, name);
    HANDLE h;
    DWORD bytes_written = 0;
    LARGE_INTEGER li;
    DWORD create_flag;

    /* Ensure sidecar directory exists */
    compat_CreateDirectory(dirpath); /* ignore EEXIST */

    create_flag = truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
    h = compat_CreateFile(path, GENERIC_WRITE, 0,
                          create_flag, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (offset > 0) {
        li.QuadPart = offset;
        if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            return -1;
        }
    }

    if (size > 0) {
        if (!WriteFile(h, buf, (DWORD)size, &bytes_written, NULL)) {
            CloseHandle(h);
            return -1;
        }
    }

    CloseHandle(h);
    return (ssize_t)bytes_written;
}

static void sidecar_unlink_win32(const char *parent_path,
                                 const char *sidecar_dir,
                                 const char *name)
{
    g_autofree char *path = build_sidecar_path(parent_path, sidecar_dir, name);
    compat_DeleteFile(path); /* ignore errors */
}

static void sidecar_rename_win32(const char *old_parent, const char *oldname,
                                 const char *new_parent, const char *newname,
                                 const char *sidecar_dir)
{
    g_autofree char *old_path =
        build_sidecar_path(old_parent, sidecar_dir, oldname);
    g_autofree char *new_dirpath = build_file_path(new_parent, sidecar_dir);
    g_autofree char *new_path =
        build_sidecar_path(new_parent, sidecar_dir, newname);

    /* Check that source exists */
    if (compat_GetFileAttributes(old_path) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    /* Ensure target sidecar dir exists */
    compat_CreateDirectory(new_dirpath);

    /* MOVEFILE_REPLACE_EXISTING handles overwrites */
    compat_MoveFileEx(old_path, new_path, MOVEFILE_REPLACE_EXISTING);
}

static ssize_t sidecar_size_win32(const char *parent_path,
                                  const char *sidecar_dir,
                                  const char *name)
{
    g_autofree char *path = build_sidecar_path(parent_path, sidecar_dir, name);
    HANDLE h;
    LARGE_INTEGER size;

    h = compat_CreateFile(path, GENERIC_READ, FILE_SHARE_READ,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return -1;
    }

    CloseHandle(h);
    return (ssize_t)size.QuadPart;
}


#else /* !_WIN32  ===== POSIX / Linux / macOS ===== */

/*
 * Open a sidecar directory entry.
 * Given @dirfd (parent of the actual file) and @name (the filename),
 * open <dirfd>/<sidecar_dir>/<name> with @flags.
 *
 * Returns fd on success, -1 on failure (errno set).
 */
static int open_sidecar(int dirfd, const char *sidecar_dir,
                        const char *name, int flags, mode_t mode)
{
    int scdir_fd, fd;

    scdir_fd = openat(dirfd, sidecar_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (scdir_fd < 0) {
        return -1;
    }

    fd = openat(scdir_fd, name, flags | O_NOFOLLOW, mode);
    close(scdir_fd);
    return fd;
}

/*
 * Ensure a sidecar directory exists, then open/create the sidecar file.
 * Creates the sidecar directory if needed.
 */
static int open_sidecar_create(int dirfd, const char *sidecar_dir,
                               const char *name, int flags, mode_t mode)
{
    int scdir_fd, fd, ret;

    /* Create the sidecar directory if it doesn't exist */
    ret = mkdirat(dirfd, sidecar_dir, 0755);
    if (ret < 0 && errno != EEXIST) {
        return -1;
    }

    scdir_fd = openat(dirfd, sidecar_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (scdir_fd < 0) {
        return -1;
    }

    fd = openat(scdir_fd, name, flags | O_NOFOLLOW, mode);
    close(scdir_fd);
    return fd;
}

/*
 * Try to get an xattr via fgetxattr on a file opened relative to dirfd.
 * Returns xattr size on success, -1 on error (errno set).
 */
static ssize_t getxattr_at(int dirfd, const char *name,
                           const char *xattr_name,
                           void *value, size_t size)
{
    int fd;
    ssize_t ret;

    fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
#ifdef __linux__
        /* Try O_PATH for special files or permission issues */
        char proc_path[64];
        fd = openat(dirfd, name, O_PATH | O_NOFOLLOW);
        if (fd < 0) {
            return -1;
        }
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        ret = getxattr(proc_path, xattr_name, value, size);
        close(fd);
        return ret;
#else
        return -1;
#endif
    }

#ifdef __APPLE__
    ret = fgetxattr(fd, xattr_name, value, size, 0, 0);
#else
    ret = fgetxattr(fd, xattr_name, value, size);
#endif
    close(fd);
    return ret;
}

/*
 * Try to set an xattr on a file relative to dirfd.
 */
static int setxattr_at(int dirfd, const char *name,
                       const char *xattr_name,
                       const void *value, size_t size, int flags)
{
    int fd, ret;

    fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
#ifdef __linux__
        char proc_path[64];
        fd = openat(dirfd, name, O_PATH | O_NOFOLLOW);
        if (fd < 0) {
            return -1;
        }
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        ret = setxattr(proc_path, xattr_name, value, size, flags);
        close(fd);
        return ret;
#else
        return -1;
#endif
    }

#ifdef __APPLE__
    ret = fsetxattr(fd, xattr_name, value, size, 0, flags);
#else
    ret = fsetxattr(fd, xattr_name, value, size, flags);
#endif
    close(fd);
    return ret;
}

/*
 * Remove an xattr from a file relative to dirfd, ignoring errors.
 */
static void removexattr_at(int dirfd, const char *name,
                           const char *xattr_name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW);

    if (fd < 0) {
        return;
    }
#ifdef __APPLE__
    fremovexattr(fd, xattr_name, 0);
#else
    fremovexattr(fd, xattr_name);
#endif
    close(fd);
}

/*
 * Unlink a sidecar file, ignoring ENOENT.
 */
static void unlink_sidecar(int dirfd, const char *sidecar_dir,
                           const char *name)
{
    int scdir_fd;

    scdir_fd = openat(dirfd, sidecar_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (scdir_fd < 0) {
        return;
    }
    unlinkat(scdir_fd, name, 0);  /* ignore errors */
    close(scdir_fd);
}

/*
 * Rename a sidecar file. Both source and target sidecar dirs must be
 * under their respective parent dirs. Creates target sidecar dir if needed.
 */
static void rename_sidecar(int olddirfd, const char *oldname,
                           int newdirfd, const char *newname,
                           const char *sidecar_dir)
{
    int old_scdir, new_scdir;

    old_scdir = openat(olddirfd, sidecar_dir,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (old_scdir < 0) {
        return;  /* no sidecar dir = nothing to rename */
    }

    /* Ensure target sidecar dir exists */
    mkdirat(newdirfd, sidecar_dir, 0755);  /* ignore EEXIST */

    new_scdir = openat(newdirfd, sidecar_dir,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (new_scdir < 0) {
        close(old_scdir);
        return;
    }

    renameat(old_scdir, oldname, new_scdir, newname);  /* ignore errors */
    close(new_scdir);
    close(old_scdir);
}

#endif /* _WIN32 */


/*
 * =========================================================================
 *  Public API: FinderInfo
 * =========================================================================
 */

#ifdef _WIN32

int macos_get_finderinfo(const char *dirpath, const char *name, bool is_dir,
                         void *buf, size_t buflen)
{
    ssize_t ret;

    /* Size query: return how many bytes we would produce */
    if (buf == NULL || buflen == 0) {
        return FINDERINFO_SIZE;
    }

    if (buflen < FINDERINFO_SIZE) {
        return -EINVAL;
    }

    /* Strategy 1: NTFS ADS */
    ret = ads_read(dirpath, name, ADS_FINDERINFO, buf, FINDERINFO_SIZE);
    if (ret == FINDERINFO_SIZE) {
        return FINDERINFO_SIZE;
    }

    /* Strategy 2: Basilisk II sidecar .finf/<name> */
    ret = sidecar_read_win32(dirpath, SIDECAR_FINF_DIR, name,
                             buf, FINDERINFO_SIZE, 0);
    if (ret >= 16) {
        if (ret < FINDERINFO_SIZE) {
            memset((uint8_t *)buf + ret, 0, FINDERINFO_SIZE - ret);
        }
        return FINDERINFO_SIZE;
    }

    /* Strategy 3: Synthesize from extension */
    synth_finderinfo(name, is_dir, (uint8_t *)buf);
    return FINDERINFO_SIZE;
}

int macos_set_finderinfo(const char *dirpath, const char *name,
                         const void *buf, size_t buflen)
{
    if (buflen < FINDERINFO_SIZE) {
        return -EINVAL;
    }

    /* Strategy 1: NTFS ADS */
    if (ads_write(dirpath, name, ADS_FINDERINFO,
                  buf, FINDERINFO_SIZE) == 0) {
        sidecar_unlink_win32(dirpath, SIDECAR_FINF_DIR, name);
        return 0;
    }

    /* Strategy 2: Sidecar .finf/<name> */
    if (sidecar_write_win32(dirpath, SIDECAR_FINF_DIR, name,
                            buf, FINDERINFO_SIZE, 0, true) ==
        FINDERINFO_SIZE) {
        return 0;
    }

    return -EIO;
}

#else /* POSIX */

int macos_get_finderinfo(int dirfd, const char *name, bool is_dir,
                         void *buf, size_t buflen)
{
    ssize_t ret;
    int fd;

    /* Size query: return how many bytes we would produce */
    if (buf == NULL || buflen == 0) {
        return FINDERINFO_SIZE;
    }

    if (buflen < FINDERINFO_SIZE) {
        return -EINVAL;
    }

    /*
     * Strategy 1: Try native xattr on the actual file.
     */
    ret = getxattr_at(dirfd, name, XATTR_FINDERINFO_NAME,
                      buf, FINDERINFO_SIZE);
    if (ret == FINDERINFO_SIZE) {
        return FINDERINFO_SIZE;
    }
    /* If ret > 0 but != FINDERINFO_SIZE, data is corrupt — fall through */

    /*
     * Strategy 2: Try Basilisk II sidecar file .finf/<n>
     */
    fd = open_sidecar(dirfd, SIDECAR_FINF_DIR, name, O_RDONLY, 0);
    if (fd >= 0) {
        ret = read(fd, buf, FINDERINFO_SIZE);
        close(fd);
        if (ret >= 16) {
            /* Pad remainder with zeros if sidecar was short */
            if (ret < FINDERINFO_SIZE) {
                memset((uint8_t *)buf + ret, 0, FINDERINFO_SIZE - ret);
            }
            return FINDERINFO_SIZE;
        }
        /* Sidecar exists but is too small/empty — fall through */
    }

    /*
     * Strategy 3: Synthesize from file extension + defaults.
     */
    synth_finderinfo(name, is_dir, (uint8_t *)buf);
    return FINDERINFO_SIZE;
}

int macos_set_finderinfo(int dirfd, const char *name,
                         const void *buf, size_t buflen)
{
    int ret, fd;

    if (buflen < FINDERINFO_SIZE) {
        return -EINVAL;
    }

    /*
     * Strategy 1: Try to store as xattr.
     */
    ret = setxattr_at(dirfd, name, XATTR_FINDERINFO_NAME,
                      buf, FINDERINFO_SIZE, 0);
    if (ret == 0) {
        return 0;
    }

    /*
     * Strategy 2: Fall back to sidecar file .finf/<n>
     * This handles filesystems that don't support xattrs (e.g. FAT, tmpfs).
     */
    fd = open_sidecar_create(dirfd, SIDECAR_FINF_DIR, name,
                             O_WRONLY | O_CREAT | O_TRUNC,
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        return -errno;
    }

    ret = write(fd, buf, FINDERINFO_SIZE);
    close(fd);

    if (ret != FINDERINFO_SIZE) {
        return (ret < 0) ? -errno : -EIO;
    }
    return 0;
}

#endif /* _WIN32 */

/*
 * =========================================================================
 *  Public API: Resource fork
 * =========================================================================
 */

#ifdef _WIN32

ssize_t macos_get_resourcefork(const char *dirpath, const char *name,
                               void *buf, size_t buflen, off_t offset)
{
    /*
     * Size query: if buf is NULL or buflen is 0, just return the total size.
     */
    if (buf == NULL || buflen == 0) {
        return macos_resourcefork_size(dirpath, name);
    }

    /*
     * Strategy 1: NTFS ADS.
     * ADS supports random access, but we use a simple size-check + read.
     */
    ssize_t total = ads_size(dirpath, name, ADS_RESOURCEFORK);
    if (total > 0) {
        if (offset >= total) {
            return 0;
        }
        /*
         * For offset reads from ADS, we need to open with seek support.
         */
        g_autofree char *ads_path =
            build_ads_path(dirpath, name, ADS_RESOURCEFORK);
        HANDLE h = compat_CreateFile(ads_path, GENERIC_READ,
                                     FILE_SHARE_READ,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            li.QuadPart = offset;
            SetFilePointerEx(h, li, NULL, FILE_BEGIN);
            DWORD bytes_read = 0;
            DWORD to_read = (DWORD)buflen;
            if ((ssize_t)(offset + to_read) > total) {
                to_read = (DWORD)(total - offset);
            }
            ReadFile(h, buf, to_read, &bytes_read, NULL);
            CloseHandle(h);
            return (ssize_t)bytes_read;
        }
    }

    /* Strategy 2: Sidecar .rsrc/<name> */
    ssize_t ret = sidecar_read_win32(dirpath, SIDECAR_RSRC_DIR, name,
                                     buf, buflen, offset);
    if (ret >= 0) {
        return ret;
    }

    return 0; /* no resource fork */
}

ssize_t macos_set_resourcefork(const char *dirpath, const char *name,
                               const void *buf, size_t buflen, off_t offset)
{
    ssize_t ret;

    /* A whole fork goes to the ADS (any size); the sidecar copy goes. */
    if (offset == 0 &&
        ads_write(dirpath, name, ADS_RESOURCEFORK, buf, buflen) == 0) {
        sidecar_unlink_win32(dirpath, SIDECAR_RSRC_DIR, name);
        return buflen;
    }

    /* No ADS (e.g. FAT): sidecar .rsrc/<name> */
    ret = sidecar_write_win32(dirpath, SIDECAR_RSRC_DIR, name,
                              buf, buflen, offset, (offset == 0));
    if (ret >= 0) {
        if (offset == 0) {
            ads_delete(dirpath, name, ADS_RESOURCEFORK);
        }
        return ret;
    }

    return -EIO;
}

ssize_t macos_resourcefork_size(const char *dirpath, const char *name)
{
    /* Try ADS first */
    ssize_t ret = ads_size(dirpath, name, ADS_RESOURCEFORK);
    if (ret > 0) {
        return ret;
    }

    /* Try sidecar */
    ret = sidecar_size_win32(dirpath, SIDECAR_RSRC_DIR, name);
    if (ret > 0) {
        return ret;
    }

    /* No resource fork found — that's not an error, just empty */
    return 0;
}

#else /* POSIX */

ssize_t macos_get_resourcefork(int dirfd, const char *name,
                               void *buf, size_t buflen, off_t offset)
{
    ssize_t total_size, ret;
    uint8_t *xattr_buf;
    int fd;

    /*
     * Size query: if buf is NULL, just return the total size.
     */
    if (buf == NULL || buflen == 0) {
        return macos_resourcefork_size(dirfd, name);
    }

    /*
     * Strategy 1: xattr. We must read the whole thing to extract a range.
     * First, query the size.
     */
    total_size = getxattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME, NULL, 0);
    if (total_size > 0) {
        if (offset >= total_size) {
            return 0;  /* past end */
        }
        xattr_buf = malloc(total_size);
        if (!xattr_buf) {
            return -ENOMEM;
        }
        ret = getxattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME,
                          xattr_buf, total_size);
        if (ret > 0) {
            ssize_t avail = ret - offset;
            if (avail <= 0) {
                free(xattr_buf);
                return 0;
            }
            if ((size_t)avail > buflen) {
                avail = buflen;
            }
            memcpy(buf, xattr_buf + offset, avail);
            free(xattr_buf);
            return avail;
        }
        free(xattr_buf);
        /* xattr disappeared between size query and read — fall through */
    }

    /*
     * Strategy 2: Sidecar file .rsrc/<n>
     */
    fd = open_sidecar(dirfd, SIDECAR_RSRC_DIR, name, O_RDONLY, 0);
    if (fd >= 0) {
        ret = pread(fd, buf, buflen, offset);
        close(fd);
        return (ret < 0) ? -errno : ret;
    }

    /* No resource fork */
    return 0;
}

ssize_t macos_set_resourcefork(int dirfd, const char *name,
                               const void *buf, size_t buflen, off_t offset)
{
    ssize_t ret;
    int fd;

    /*
     * A whole fork replaces both stores, so a stale copy in the other one
     * can never shadow it. macOS keeps forks of any size natively.
     */
    if (offset == 0) {
        if (buflen == 0) {
            macos_remove_apple_xattr(dirfd, name, true);
            return 0;
        }
#ifndef __APPLE__
        if (buflen <= RSRC_XATTR_MAX_SIZE)
#endif
        {
#ifdef __APPLE__
            /* macOS writes a fork in place: a longer old one keeps its tail */
            removexattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME);
#endif
            if (setxattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME,
                            buf, buflen, 0) == 0) {
                unlink_sidecar(dirfd, SIDECAR_RSRC_DIR, name);
                return buflen;
            }
        }
        removexattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME);
    }

    fd = open_sidecar_create(dirfd, SIDECAR_RSRC_DIR, name,
                             O_WRONLY | O_CREAT | (offset == 0 ? O_TRUNC : 0),
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        return -errno;
    }

    ret = pwrite(fd, buf, buflen, offset);
    if (ret < 0) {
        ret = -errno;
    }
    close(fd);
    return ret;
}

void macos_remove_apple_xattr(int dirfd, const char *name, bool fork)
{
    if (fork) {
        removexattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME);
        unlink_sidecar(dirfd, SIDECAR_RSRC_DIR, name);
    } else {
        removexattr_at(dirfd, name, XATTR_FINDERINFO_NAME);
        unlink_sidecar(dirfd, SIDECAR_FINF_DIR, name);
    }
}

ssize_t macos_resourcefork_size(int dirfd, const char *name)
{
    ssize_t ret;
    struct stat st;
    int fd;

    /* Try xattr first */
    ret = getxattr_at(dirfd, name, XATTR_RESOURCEFORK_NAME, NULL, 0);
    if (ret > 0) {
        return ret;
    }

    /* Try sidecar */
    fd = open_sidecar(dirfd, SIDECAR_RSRC_DIR, name, O_RDONLY, 0);
    if (fd >= 0) {
        if (fstat(fd, &st) == 0) {
            close(fd);
            return st.st_size;
        }
        close(fd);
    }

    return 0;
}

#endif /* _WIN32 */


/*
 * =========================================================================
 *  Public API: Sidecar management
 * =========================================================================
 */

#ifdef _WIN32

void macos_remove_sidecars(const char *dirpath, const char *name)
{
    sidecar_unlink_win32(dirpath, SIDECAR_FINF_DIR, name);
    sidecar_unlink_win32(dirpath, SIDECAR_RSRC_DIR, name);

    /* Also remove ADS streams (they travel with the file on rename,
     * but need explicit cleanup on delete) */
    ads_delete(dirpath, name, ADS_FINDERINFO);
    ads_delete(dirpath, name, ADS_RESOURCEFORK);
}

void macos_rename_sidecars(const char *old_parent, const char *oldname,
                           const char *new_parent, const char *newname)
{
    sidecar_rename_win32(old_parent, oldname, new_parent, newname,
                         SIDECAR_FINF_DIR);
    sidecar_rename_win32(old_parent, oldname, new_parent, newname,
                         SIDECAR_RSRC_DIR);
    /* Note: ADS streams travel with the file on NTFS rename, so
     * no explicit ADS rename is needed. */
}

#else /* POSIX */

void macos_remove_sidecars(int dirfd, const char *name)
{
    unlink_sidecar(dirfd, SIDECAR_FINF_DIR, name);
    unlink_sidecar(dirfd, SIDECAR_RSRC_DIR, name);
}

void macos_rename_sidecars(int olddirfd, const char *oldname,
                           int newdirfd, const char *newname)
{
    rename_sidecar(olddirfd, oldname, newdirfd, newname, SIDECAR_FINF_DIR);
    rename_sidecar(olddirfd, oldname, newdirfd, newname, SIDECAR_RSRC_DIR);
}

#endif /* _WIN32 */


/*
 * =========================================================================
 *  Public API: Directory filtering (platform-independent)
 * =========================================================================
 */

bool macos_is_sidecar_dir(const char *name)
{
    return strcmp(name, SIDECAR_FINF_DIR) == 0 ||
           strcmp(name, SIDECAR_RSRC_DIR) == 0;
}


/*
 * =========================================================================
 *  Public API: Xattr namespace helpers (platform-independent)
 * =========================================================================
 */

bool macos_is_apple_xattr(const char *name)
{
    return strcmp(name, XATTR_FINDERINFO_NAME) == 0 ||
           strcmp(name, XATTR_RESOURCEFORK_NAME) == 0 ||
           strcmp(name, "com.apple.FinderInfo") == 0 ||
           strcmp(name, "com.apple.ResourceFork") == 0;
}

bool macos_is_finderinfo_xattr(const char *name)
{
    return strcmp(name, XATTR_FINDERINFO_NAME) == 0 ||
           strcmp(name, "com.apple.FinderInfo") == 0;
}

bool macos_is_resourcefork_xattr(const char *name)
{
    return strcmp(name, XATTR_RESOURCEFORK_NAME) == 0 ||
           strcmp(name, "com.apple.ResourceFork") == 0;
}

/*
 * This function needs platform-specific resource fork size check.
 * We use a macro to call the correct resourcefork_size variant.
 */
#ifdef _WIN32

ssize_t macos_list_xattrs(const char *dirpath, const char *name,
                          bool is_dir, void *buf, size_t buflen)
{
    g_autoptr(GString) out = g_string_new(NULL);
    g_autofree char *path = build_file_path(dirpath, name);
    g_autofree wchar_t *wpath = utf8_to_wstr(path);
    WIN32_FIND_STREAM_DATA sd;
    HANDLE h;

    g_string_append_len(out, XATTR_FINDERINFO_NAME,
                        sizeof(XATTR_FINDERINFO_NAME));
    if (macos_resourcefork_size(dirpath, name) > 0) {
        g_string_append_len(out, XATTR_RESOURCEFORK_NAME,
                            sizeof(XATTR_RESOURCEFORK_NAME));
    }

    /* other xattrs are streams ":<name>:$DATA" */
    h = wpath ? FindFirstStreamW(wpath, FindStreamInfoStandard, &sd, 0)
              : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char s[MAX_PATH + 36];
            char *end;

            if (!WideCharToMultiByte(CP_UTF8, 0, sd.cStreamName, -1,
                                     s, sizeof(s), NULL, NULL) ||
                s[0] != ':' || !(end = strstr(s + 1, ":$DATA")) ||
                end == s + 1) {
                continue;
            }
            *end = '\0';
            if (macos_is_apple_xattr(s + 1)) {
                continue;
            }
            g_string_append_len(out, s + 1, end - s);
        } while (FindNextStreamW(h, &sd));
        FindClose(h);
    }

    if (!buf) {
        return out->len;
    }
    if (buflen < out->len) {
        errno = ERANGE;
        return -1;
    }
    memcpy(buf, out->str, out->len);
    return out->len;
}

/* Drop FinderInfo or the resource fork from both stores. */
void macos_remove_apple_xattr(const char *dirpath, const char *name,
                              bool fork)
{
    if (fork) {
        ads_delete(dirpath, name, ADS_RESOURCEFORK);
        sidecar_unlink_win32(dirpath, SIDECAR_RSRC_DIR, name);
    } else {
        ads_delete(dirpath, name, ADS_FINDERINFO);
        sidecar_unlink_win32(dirpath, SIDECAR_FINF_DIR, name);
    }
}

/* Other xattrs live in a stream of the same name. */
#define P9_XATTR_CREATE     1
#define P9_XATTR_REPLACE    2

static bool stream_name_ok(const char *xname)
{
    return xname[0] && !strpbrk(xname, ":/\\");
}

ssize_t win32_get_xattr(const char *dirpath, const char *name,
                        const char *xname, void *buf, size_t buflen)
{
    ssize_t size;

    if (!stream_name_ok(xname)) {
        errno = ENODATA;
        return -1;
    }
    size = ads_size(dirpath, name, xname);
    if (size < 0) {
        errno = ENODATA;
        return -1;
    }
    if (!buf || !buflen) {
        return size;
    }
    if (buflen < (size_t)size) {
        errno = ERANGE;
        return -1;
    }
    return ads_read(dirpath, name, xname, buf, size);
}

int win32_set_xattr(const char *dirpath, const char *name, const char *xname,
                    const void *buf, size_t size, int flags)
{
    bool exists;

    if (!stream_name_ok(xname)) {
        errno = ENOTSUP;
        return -1;
    }
    exists = ads_size(dirpath, name, xname) >= 0;
    if ((flags & P9_XATTR_CREATE) && exists) {
        errno = EEXIST;
        return -1;
    }
    if ((flags & P9_XATTR_REPLACE) && !exists) {
        errno = ENODATA;
        return -1;
    }
    return ads_write(dirpath, name, xname, buf, size);
}

int win32_remove_xattr(const char *dirpath, const char *name,
                       const char *xname)
{
    if (!stream_name_ok(xname) || ads_size(dirpath, name, xname) < 0) {
        errno = ENODATA;
        return -1;
    }
    ads_delete(dirpath, name, xname);
    return 0;
}

#else /* POSIX */

ssize_t macos_list_xattrs(int dirfd, const char *name, bool is_dir,
                          void *buf, size_t buflen)
{
    /*
     * We always advertise FinderInfo (it can always be synthesized).
     * We only advertise ResourceFork if one actually exists.
     */
    size_t fi_len = strlen(XATTR_FINDERINFO_NAME) + 1;  /* incl. NUL */
    size_t rf_len = strlen(XATTR_RESOURCEFORK_NAME) + 1;
    size_t total = fi_len;
    bool has_rsrc = false;

    if (macos_resourcefork_size(dirfd, name) > 0) {
        has_rsrc = true;
        total += rf_len;
    }

    if (!buf) {
        return total;
    }

    if (buflen < total) {
        errno = ERANGE;
        return -1;
    }

    memcpy(buf, XATTR_FINDERINFO_NAME, fi_len);
    if (has_rsrc) {
        memcpy((char *)buf + fi_len, XATTR_RESOURCEFORK_NAME, rf_len);
    }

    return total;
}

#endif /* _WIN32 */
