/*
 * 9p Win32 compatibility definitions
 *
 * Provides missing POSIX definitions for building 9pfs on Windows/MinGW.
 *
 * Copyright (c) 2025
 * License: GNU GPL v2 or later
 */

#ifndef QEMU_9P_WIN32_COMPAT_H
#define QEMU_9P_WIN32_COMPAT_H

#ifdef _WIN32

#include <sys/types.h>
#include <sys/stat.h>

/*
 * POSIX open flags not available on Windows.
 * Define as zero so they compile but have no effect.
 */
#ifndef O_NOCTTY
#define O_NOCTTY    0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0
#endif
#ifndef O_DSYNC
#define O_DSYNC     0
#endif
#ifndef O_DIRECT
#define O_DIRECT    0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW  0
#endif
#ifndef O_NOATIME
#define O_NOATIME   0
#endif
#ifndef O_SYNC
#define O_SYNC      0
#endif
#ifndef O_ASYNC
#define O_ASYNC     0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_PATH
#define O_PATH      0
#endif
#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0
#endif

/*
 * POSIX types not available on Windows
 */
#ifndef uid_t
typedef unsigned int uid_t;
#endif
#ifndef gid_t
typedef unsigned int gid_t;
#endif

/*
 * AT_* constants for unlinkat/renameat
 */
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_FDCWD
#define AT_FDCWD            -100
#endif

/*
 * POSIX symlink mode bits
 */
#ifndef S_IFLNK
#define S_IFLNK     0xA000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK     0xC000
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif

/*
 * Xattr-related constants
 */
#ifndef ENOATTR
#define ENOATTR     ENODATA
#endif
#ifndef ENODATA
#define ENODATA     61
#endif
#ifndef ENOTSUP
#define ENOTSUP     95
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP  95
#endif

/*
 * dirent d_type constants (MinGW does not define these).
 */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN  0
#endif
#ifndef DT_DIR
#define DT_DIR      4
#endif
#ifndef DT_REG
#define DT_REG      8
#endif
#ifndef DT_LNK
#define DT_LNK     10
#endif

/*
 * statfs structure for Windows.
 * Only the fields that the 9P protocol needs are populated.
 */
struct statfs {
    long f_type;
    long f_bsize;
    long long f_blocks;
    long long f_bfree;
    long long f_bavail;
    long long f_files;
    long long f_ffree;
    long f_namelen;
};

/*
 * timespec may or may not be defined in MinGW.
 * The QEMU osdep.h usually handles this.
 */

/*
 * iovec for scatter/gather I/O
 * MinGW should have this via QEMU's osdep.h
 */

/*
 * Errno translation: Windows errno values differ from Linux.
 * The 9P protocol sends Linux errno values to guests.
 * NOTE: errno_to_dotl() is defined in 9p-util.h, which is the
 * canonical location used throughout the 9pfs codebase.
 */

#endif /* _WIN32 */
#endif /* QEMU_9P_WIN32_COMPAT_H */
