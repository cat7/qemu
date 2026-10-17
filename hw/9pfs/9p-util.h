/*
 * 9p utilities
 *
 * Copyright IBM, Corp. 2017
 *
 * Authors:
 *  Greg Kurz <groug@kaod.org>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_9P_UTIL_H
#define QEMU_9P_UTIL_H

#include "qemu/error-report.h"
#include <dirent.h>

#ifdef _WIN32
/*
 * Windows/MinGW compatibility: define POSIX constants and types
 * that are missing from the MinGW headers.
 */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW  0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0
#endif
#ifndef O_NOATIME
#define O_NOATIME   0
#endif
#ifndef O_PATH
#define O_PATH      0
#endif
#ifndef O_DSYNC
#define O_DSYNC     0
#endif
#ifndef O_SYNC
#define O_SYNC      0
#endif
#ifndef O_ASYNC
#define O_ASYNC     0
#endif
#ifndef FASYNC
#define FASYNC      0
#endif
#ifndef O_DIRECT
#define O_DIRECT    0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0
#endif
#ifndef XATTR_CREATE
#define XATTR_CREATE    0x1
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE   0x2
#endif
#ifndef S_IFLNK
#define S_IFLNK  0xA000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif
/* setuid/setgid/sticky bits */
#ifndef S_ISUID
#define S_ISUID 04000
#endif
#ifndef S_ISGID
#define S_ISGID 02000
#endif
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif
/* AT_* constants */
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
/* utimensat constants */
#ifndef UTIME_NOW
#define UTIME_NOW  ((1L << 30) - 1L)
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((1L << 30) - 2L)
#endif
/* major/minor/makedev - Windows has no device number encoding */
#ifndef major
#define major(dev) 0
#endif
#ifndef minor
#define minor(dev) ((unsigned int)(dev))
#endif
#ifndef makedev
#define makedev(ma, mi) ((dev_t)(mi))
#endif
/* xattr size limit */
#ifndef XATTR_SIZE_MAX
#define XATTR_SIZE_MAX 65536
#endif
#endif /* _WIN32 */

#ifdef O_PATH
#define O_PATH_9P_UTIL O_PATH
#else
#define O_PATH_9P_UTIL 0
#endif

#ifdef CONFIG_FREEBSD
/*
 * FreeBSD does not have these flags, so we can only emulate their intended
 * behaviour (racily).
 */
#ifndef XATTR_CREATE
#define XATTR_CREATE    0x1
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE   0x2
#endif
#endif

#if !defined(CONFIG_LINUX)

/*
 * Generates a Linux device number (a.k.a. dev_t) for given device major
 * and minor numbers.
 *
 * To be more precise: it generates a device number in glibc's format
 * (MMMM_Mmmm_mmmM_MMmm, 64 bits) actually, which is compatible with
 * Linux's format (mmmM_MMmm, 32 bits), as described in <bits/sysmacros.h>.
 */
static inline uint64_t makedev_dotl(uint32_t dev_major, uint32_t dev_minor)
{
    uint64_t dev;

    // from glibc sysmacros.h:
    dev  = (((uint64_t) (dev_major & 0x00000fffu)) <<  8);
    dev |= (((uint64_t) (dev_major & 0xfffff000u)) << 32);
    dev |= (((uint64_t) (dev_minor & 0x000000ffu)) <<  0);
    dev |= (((uint64_t) (dev_minor & 0xffffff00u)) << 12);
    return dev;
}

#endif

/*
 * Converts given device number from host's device number format to Linux
 * device number format. As both the size of type dev_t and encoding of
 * dev_t is system dependent, we have to convert them for Linux guests if
 * host is not running Linux.
 */
static inline uint64_t host_dev_to_dotl_dev(dev_t dev)
{
#ifdef CONFIG_LINUX
    return dev;
#elif defined(_WIN32)
    /*
     * Windows dev_t is simple (no major/minor encoding).
     * Return it via makedev_dotl with dev as minor, 0 as major.
     */
    return makedev_dotl(0, (uint32_t)dev);
#else
    return makedev_dotl(major(dev), minor(dev));
#endif
}

/* Translates errno from host -> Linux if needed */
static inline int errno_to_dotl(int err) {
#if defined(CONFIG_LINUX)
    /* nothing to translate (Linux -> Linux) */
#elif defined(CONFIG_DARWIN) || defined(CONFIG_FREEBSD)
    /*
     * translation mandatory for non-Linux hosts
     *
     * FIXME: Only most important errnos translated here yet, this should be
     * extended to as many errnos being translated as possible in future.
     */
    if (err == ENAMETOOLONG) {
        err = 36; /* ==ENAMETOOLONG on Linux */
    } else if (err == ENOTEMPTY) {
        err = 39; /* ==ENOTEMPTY on Linux */
    } else if (err == ELOOP) {
        err = 40; /* ==ELOOP on Linux */
    } else if (err == ENOATTR) {
        err = 61; /* ==ENODATA on Linux */
    } else if (err == ENODATA) {
        err = 61; /* ==ENODATA on Linux */
    } else if (err == ENOTSUP) {
        err = 95; /* ==EOPNOTSUPP on Linux */
    } else if (err == EOPNOTSUPP) {
        err = 95; /* ==EOPNOTSUPP on Linux */
    }
#elif defined(_WIN32)
    /*
     * Windows/MinGW errno values mostly match Linux for common errors.
     * Translate the ones that differ.
     */
    switch (err) {
    case EPERM:         err = 1; break;
    case ENOENT:        err = 2; break;
    case EIO:           err = 5; break;
    case ENXIO:         err = 6; break;
    case EBADF:         err = 9; break;
    case ENOMEM:        err = 12; break;
    case EACCES:        err = 13; break;
    case EBUSY:         err = 16; break;
    case EEXIST:        err = 17; break;
    case ENODEV:        err = 19; break;
    case ENOTDIR:       err = 20; break;
    case EISDIR:        err = 21; break;
    case EINVAL:        err = 22; break;
    case EMFILE:        err = 24; break;
    case ENOSPC:        err = 28; break;
    case ENAMETOOLONG:  err = 36; break;
    case ENOTEMPTY:     err = 39; break;
#ifdef ENODATA
    case ENODATA:       err = 61; break; /* UCRT defines as 120 */
#endif
#ifdef ENOTSUP
    case ENOTSUP:       err = 95; break;
#endif
    default: break; /* hope for the best */
    }
#else
#error Missing errno translation to Linux for this host system
#endif
    return err;
}

#ifdef CONFIG_DARWIN
#define qemu_fgetxattr(...) fgetxattr(__VA_ARGS__, 0, 0)
#elif defined(_WIN32)
/* No fgetxattr on Windows; the macos_compat layer handles xattrs */
#define qemu_fgetxattr(fd, name, value, size) (-1)
#else
#define qemu_fgetxattr fgetxattr
#endif

#ifndef _WIN32
/* POSIX *at() functions - not available on Windows */
#define qemu_openat     openat
#define qemu_fstat      fstat
#define qemu_fstatat    fstatat
#define qemu_mkdirat    mkdirat
#define qemu_renameat   renameat
#define qemu_utimensat  utimensat
#define qemu_unlinkat   unlinkat
#define qemu_futimens   futimens
#endif

static inline void close_preserve_errno(int fd)
{
    int serrno = errno;
    close(fd);
    errno = serrno;
}

#ifndef _WIN32
/**
 * close_if_special_file() - Close @fd if neither regular file nor directory.
 *
 * @fd: file descriptor of open file
 * Return: 0 on regular file or directory, -1 otherwise
 *
 * CVE-2023-2861: Prohibit opening any special file directly on host
 * (especially device files), as a compromised client could potentially gain
 * access outside exported tree under certain, unsafe setups. We expect
 * client to handle I/O on special files exclusively on guest side.
 */
static inline int close_if_special_file(int fd)
{
    struct stat stbuf;

    if (qemu_fstat(fd, &stbuf) < 0) {
        close_preserve_errno(fd);
        return -1;
    }
    if (!S_ISREG(stbuf.st_mode) && !S_ISDIR(stbuf.st_mode)) {
        error_report_once(
            "9p: broken or compromised client detected; attempt to open "
            "special file (i.e. neither regular file, nor directory)"
        );
        close(fd);
        errno = ENXIO;
        return -1;
    }

    return 0;
}

static inline int openat_dir(int dirfd, const char *name)
{
    return qemu_openat(dirfd, name,
                       O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_PATH_9P_UTIL);
}

static inline int openat_file(int dirfd, const char *name, int flags,
                              mode_t mode)
{
    int fd, serrno, ret;

#if !defined(CONFIG_DARWIN) && !defined(CONFIG_FREEBSD)
again:
#endif
    fd = qemu_openat(dirfd, name, flags | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK,
                     mode);
    if (fd == -1) {
#if !defined(CONFIG_DARWIN) && !defined(CONFIG_FREEBSD)
        if (errno == EPERM && (flags & O_NOATIME)) {
            /*
             * The client passed O_NOATIME but we lack permissions to honor it.
             * Rather than failing the open, fall back without O_NOATIME. This
             * doesn't break the semantics on the client side, as the Linux
             * open(2) man page notes that O_NOATIME "may not be effective on
             * all filesystems". In particular, NFS and other network
             * filesystems ignore it entirely.
             */
            flags &= ~O_NOATIME;
            goto again;
        }
#endif
        return -1;
    }

    /* Only if O_PATH is not set ... */
    if (!(flags & O_PATH_9P_UTIL)) {
        /*
         * Prevent I/O on special files (device files, etc.) on host side,
         * however it is safe and required to allow opening them with O_PATH,
         * as this is limited to (required) path based operations only.
         */
        if (close_if_special_file(fd) < 0) {
            return -1;
        }

        serrno = errno;
        /*
         * O_NONBLOCK was only needed to open the file. Let's drop it. We don't
         * do that with O_PATH since fcntl(F_SETFL) isn't supported, and
         * openat() ignored it anyway.
         */
        ret = fcntl(fd, F_SETFL, flags);
        assert(!ret);
        errno = serrno;
    }
    return fd;
}
#endif /* !_WIN32 */

#ifdef CONFIG_FREEBSD
ssize_t fgetxattr(int dirfd, const char *name, void *value, size_t size);
#endif
ssize_t fgetxattrat_nofollow(int dirfd, const char *path, const char *name,
                             void *value, size_t size);
int fsetxattrat_nofollow(int dirfd, const char *path, const char *name,
                         void *value, size_t size, int flags);
ssize_t flistxattrat_nofollow(int dirfd, const char *filename,
                              char *list, size_t size);
ssize_t fremovexattrat_nofollow(int dirfd, const char *filename,
                                const char *name);

/*
 * Darwin has d_seekoff, which appears to function similarly to d_off.
 * However, it does not appear to be supported on all file systems,
 * so ensure it is manually injected earlier and call here when
 * needed.
 */

/* Ensure DT_* constants are available on all platforms */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif

static inline off_t qemu_dirent_off(struct dirent *dent)
{
#ifdef CONFIG_DARWIN
    return dent->d_seekoff;
#elif defined(_WIN32)
    /*
     * Windows/MinGW dirent has no d_off or d_seekoff.
     * We store the telldir() value (snapshot position) in d_namlen,
     * which is unused by QEMU on Windows (we use strlen(d_name) instead).
     * d_ino is reserved for the real NTFS file index (inode equivalent)
     * which is needed for correct 9P QID path generation.
     * See local_readdir in 9p-local-win32.c.
     */
    return (off_t)dent->d_namlen;
#else
    return dent->d_off;
#endif
}

/**
 * qemu_dirent_type() - Return the d_type for a directory entry.
 *
 * On most POSIX systems, struct dirent has a d_type member.
 * On Windows/MinGW, struct dirent lacks d_type.  Our directory
 * snapshot code (9p-local-win32.c) stores the type in d_reclen
 * instead, since that field is unused on Windows.
 *
 * Returns DT_DIR, DT_REG, or DT_UNKNOWN.
 */
static inline unsigned char qemu_dirent_type(struct dirent *dent)
{
#ifdef _WIN32
    /*
     * MinGW struct dirent has no d_type.  Our Win32 snapshot code
     * stores the file type (DT_DIR / DT_REG) in d_reclen.
     */
    return (unsigned char)dent->d_reclen;
#elif defined(_DIRENT_HAVE_D_TYPE) || defined(CONFIG_DARWIN)
    return dent->d_type;
#else
    return DT_UNKNOWN;
#endif
}

/**
 * qemu_dirent_dup() - Duplicate directory entry @dent.
 *
 * @dent: original directory entry to be duplicated
 * Return: duplicated directory entry which should be freed with g_free()
 *
 * It is highly recommended to use this function instead of open coding
 * duplication of dirent objects, because the actual struct dirent
 * size may be bigger or shorter than sizeof(struct dirent) and correct
 * handling is platform specific (see gitlab issue #841).
 */
static inline struct dirent *qemu_dirent_dup(struct dirent *dent)
{
    size_t sz = 0;
#if defined _DIRENT_HAVE_D_RECLEN
    /* Avoid use of strlen() if platform supports d_reclen. */
    sz = dent->d_reclen;
#endif
    /*
     * Test sz for zero even if d_reclen is available
     * because some drivers may set d_reclen to zero.
     */
    if (sz == 0) {
        /* Fallback to the most portable way. */
        sz = offsetof(struct dirent, d_name) +
                      strlen(dent->d_name) + 1;
    }
    return g_memdup(dent, sz);
}

/*
 * As long as mknodat is not available on macOS, this workaround
 * using pthread_fchdir_np is needed. qemu_mknodat is defined in
 * os-posix.c. pthread_fchdir_np is weakly linked here as a guard
 * in case it disappears in future macOS versions, because it is
 * is a private API.
 */
#if defined CONFIG_DARWIN && defined CONFIG_PTHREAD_FCHDIR_NP
int pthread_fchdir_np(int fd) __attribute__((weak_import));
#endif
int qemu_mknodat(int dirfd, const char *filename, mode_t mode, dev_t dev);

/*
 * Returns a newly allocated string presentation of open() flags, intended
 * for debugging (tracing) purposes only.
 */
char *qemu_open_flags_tostr(int flags);

#endif
