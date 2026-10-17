/*
 * 9p utilities (Windows Implementation)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <dirent.h>
#include "9p-util.h"

/*
 * On Windows, the dirfd-based xattr functions are not used.
 * The macos_compat layer in 9p-local-win32.c handles Apple xattrs
 * using NTFS Alternate Data Streams via path-based APIs.
 *
 * These stubs satisfy the linker for code paths that reference them.
 */

ssize_t fgetxattrat_nofollow(int dirfd, const char *filename, const char *name,
                             void *value, size_t size)
{
    errno = ENOTSUP;
    return -1;
}

ssize_t flistxattrat_nofollow(int dirfd, const char *filename,
                              char *list, size_t size)
{
    return 0;
}

ssize_t fremovexattrat_nofollow(int dirfd, const char *filename,
                                const char *name)
{
    errno = ENOTSUP;
    return -1;
}

int fsetxattrat_nofollow(int dirfd, const char *filename, const char *name,
                         void *value, size_t size, int flags)
{
    errno = ENOTSUP;
    return -1;
}

int qemu_mknodat(int dirfd, const char *filename, mode_t mode, dev_t dev)
{
    errno = ENOTSUP;
    return -1;
}
