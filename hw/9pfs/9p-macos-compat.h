/*
 * 9p-macos-compat.h - Mac OS 9 compatibility layer for QEMU 9pfs
 *
 * Provides transparent handling of Mac OS filesystem metadata
 * (FinderInfo, resource forks, type/creator codes) on host filesystems.
 *
 * Designed to work with classicvirtio guest driver for Mac OS 9.
 *
 * Platform support:
 *   Linux/macOS: xattrs (user.com.apple.FinderInfo, user.com.apple.ResourceFork)
 *   Windows:     NTFS Alternate Data Streams (file:com.apple.FinderInfo, etc.)
 *   All:         Sidecar files (.finf/<n>, .rsrc/<n>) as fallback
 *   All:         Synthesis from file extension (FinderInfo only, read-only)
 *
 * Copyright (c) 2025
 * License: GNU GPL v2 or later
 */

#ifndef QEMU_9P_MACOS_COMPAT_H
#define QEMU_9P_MACOS_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

/*
 * Mac OS FinderInfo is 32 bytes (FInfo 16 bytes + FXInfo 16 bytes).
 *
 * File FInfo layout:
 *   [0..3]   fdType      - file type code (e.g. 'TEXT')
 *   [4..7]   fdCreator   - creator code (e.g. 'ttxt')
 *   [8..9]   fdFlags     - Finder flags
 *   [10..13] fdLocation  - icon position in window
 *   [14..15] fdFldr      - folder ID
 *
 * Bytes 16..31 are FXInfo (extended finder info), usually zeroed.
 */
#define FINDERINFO_SIZE             32

/* Offsets within FinderInfo for files */
#define FINFO_FDTYPE_OFF            0
#define FINFO_FDCREATOR_OFF         4
#define FINFO_FDFLAGS_OFF           8
#define FINFO_FDLOCATION_OFF        10
#define FINFO_FDFLDR_OFF            14

/* Default Finder flags: kHasBeenInited (0x0100) */
#define FINDER_FLAG_HAS_BEEN_INITED 0x0100

/*
 * Xattr names. On macOS, these are native system xattrs without prefix.
 * On Linux, the "user." prefix makes them accessible to unprivileged users
 * on filesystems that support xattrs (ext4, xfs, btrfs).
 * On Windows, the "user." prefix is part of the 9P protocol name but the
 * NTFS ADS stream uses the bare name (com.apple.FinderInfo).
 */
#ifdef __APPLE__
#define XATTR_FINDERINFO_NAME       "com.apple.FinderInfo"
#define XATTR_RESOURCEFORK_NAME     "com.apple.ResourceFork"
#else
#define XATTR_FINDERINFO_NAME       "user.com.apple.FinderInfo"
#define XATTR_RESOURCEFORK_NAME     "user.com.apple.ResourceFork"
#endif

/*
 * Sidecar directory names (Basilisk II / SheepShaver convention).
 * Created as subdirectories alongside the actual files.
 * Used as fallback on all platforms when primary storage fails.
 */
#define SIDECAR_FINF_DIR            ".finf"
#define SIDECAR_RSRC_DIR            ".rsrc"

/*
 * Maximum resource fork size to store in xattrs/ADS-as-primary.
 * ext4 limits xattr value size to ~64KB by default.
 * NTFS ADS has no practical size limit, but we keep the same
 * threshold for consistency with sidecar fallback behavior.
 */
#define RSRC_XATTR_MAX_SIZE         (60 * 1024)

/*
 * Extension-to-type/creator mapping.
 * Derived from Basilisk II's e2t_translation table (extfs_windows.cpp).
 */
typedef struct MacExtMapping {
    const char *ext;        /* file extension including dot, e.g. ".jpg" */
    uint32_t type;          /* Mac OS file type, big-endian fourcc */
    uint32_t creator;       /* Mac OS creator code, big-endian fourcc */
} MacExtMapping;

extern const MacExtMapping mac_ext_table[];
extern const size_t mac_ext_table_size;

/* Build a big-endian FourCC from four characters (macro for use in initializers) */
#define MAC_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8)  | \
     ((uint32_t)(unsigned char)(d)))

/* Write big-endian uint32 into a byte buffer */
static inline void mac_put_be32(void *buf, uint32_t val)
{
    uint8_t *p = (uint8_t *)buf;
    p[0] = (val >> 24) & 0xff;
    p[1] = (val >> 16) & 0xff;
    p[2] = (val >> 8) & 0xff;
    p[3] = val & 0xff;
}

/* Write big-endian uint16 into a byte buffer */
static inline void mac_put_be16(void *buf, uint16_t val)
{
    uint8_t *p = (uint8_t *)buf;
    p[0] = (val >> 8) & 0xff;
    p[1] = val & 0xff;
}

/* Read big-endian uint32 from a byte buffer */
static inline uint32_t mac_get_be32(const void *buf)
{
    const uint8_t *p = (const uint8_t *)buf;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}


/*
 * =========================================================================
 *  Platform-conditional API
 *
 *  On POSIX (Linux/macOS): functions take (int dirfd, const char *name)
 *  On Windows:             functions take (const char *dirpath, const char *name)
 *
 *  The dirfd/dirpath identifies the parent directory of the target file.
 *  The name is the filename within that directory.
 * =========================================================================
 */

#ifdef _WIN32

/* ========================= FinderInfo ================================= */

int macos_get_finderinfo(const char *dirpath, const char *name, bool is_dir,
                         void *buf, size_t buflen);

int macos_set_finderinfo(const char *dirpath, const char *name,
                         const void *buf, size_t buflen);

/* ========================= Resource fork ============================== */

ssize_t macos_get_resourcefork(const char *dirpath, const char *name,
                               void *buf, size_t buflen, off_t offset);

ssize_t macos_set_resourcefork(const char *dirpath, const char *name,
                               const void *buf, size_t buflen, off_t offset);

ssize_t macos_resourcefork_size(const char *dirpath, const char *name);

/* ========================= Sidecar management ========================= */

void macos_remove_sidecars(const char *dirpath, const char *name);

void macos_rename_sidecars(const char *old_parent, const char *oldname,
                           const char *new_parent, const char *newname);

/* ========================= Xattr helpers ============================== */

ssize_t macos_list_xattrs(const char *dirpath, const char *name,
                          bool is_dir, void *buf, size_t buflen);

void macos_remove_apple_xattr(const char *dirpath, const char *name,
                              bool fork);

ssize_t win32_get_xattr(const char *dirpath, const char *name,
                        const char *xname, void *buf, size_t buflen);
int win32_set_xattr(const char *dirpath, const char *name, const char *xname,
                    const void *buf, size_t size, int flags);
int win32_remove_xattr(const char *dirpath, const char *name,
                       const char *xname);

#else /* POSIX */

/* ========================= FinderInfo ================================= */

/*
 * Retrieve FinderInfo for a file or directory.
 *
 * Search order:
 *   1. xattr "user.com.apple.FinderInfo" on the host file
 *   2. Sidecar file  <parentdir>/.finf/<n>
 *   3. Synthesize from file extension using mac_ext_table (files only)
 *   4. Return default (zeroed with kHasBeenInited flag set)
 *
 * @dirfd:  open fd of the file's parent directory
 * @name:   filename within that directory
 * @is_dir: true if the entry is a directory
 * @buf:    output buffer (must be >= FINDERINFO_SIZE bytes)
 * @buflen: size of buf
 *
 * Returns FINDERINFO_SIZE on success, -errno on error.
 */
int macos_get_finderinfo(int dirfd, const char *name, bool is_dir,
                         void *buf, size_t buflen);

/*
 * Store FinderInfo for a file.
 * Tries xattr first, falls back to sidecar .finf/<n>.
 * Returns 0 on success, -errno on error.
 */
int macos_set_finderinfo(int dirfd, const char *name,
                         const void *buf, size_t buflen);

/* ========================= Resource fork ============================== */

/*
 * Read resource fork data starting at @offset.
 * Tries xattr, then sidecar .rsrc/<n>.
 * Returns bytes read (0 if no resource fork), or -errno.
 */
ssize_t macos_get_resourcefork(int dirfd, const char *name,
                               void *buf, size_t buflen, off_t offset);

/*
 * Write resource fork data starting at @offset.
 * Small forks (<=RSRC_XATTR_MAX_SIZE): tries xattr first.
 * Larger forks: sidecar file.
 * Returns bytes written, or -errno.
 */
ssize_t macos_set_resourcefork(int dirfd, const char *name,
                               const void *buf, size_t buflen, off_t offset);

/*
 * Get total resource fork size without reading it.
 * Returns size (0 if none), or -errno.
 */
ssize_t macos_resourcefork_size(int dirfd, const char *name);

/* ========================= Sidecar management ========================= */

/* Remove .finf/<n> and .rsrc/<n> when a file is deleted. */
void macos_remove_sidecars(int dirfd, const char *name);

/* Remove FinderInfo (@fork false) or the resource fork from all stores. */
void macos_remove_apple_xattr(int dirfd, const char *name, bool fork);

/* Rename sidecar files when a file is renamed/moved. */
void macos_rename_sidecars(int olddirfd, const char *oldname,
                           int newdirfd, const char *newname);

/* ========================= Xattr helpers ============================== */

/*
 * Append virtual Apple xattr names to a listing buffer.
 * Always reports FinderInfo; reports ResourceFork only if one exists.
 * If @buf is NULL, returns required buffer size.
 * Returns bytes written/needed, or -errno.
 */
ssize_t macos_list_xattrs(int dirfd, const char *name, bool is_dir,
                          void *buf, size_t buflen);

#endif /* _WIN32 */


/* ======== Platform-independent helpers (always available) ============= */

/* Returns true if @name is ".finf" or ".rsrc" (should be hidden from guest). */
bool macos_is_sidecar_dir(const char *name);

/* Returns true if xattr name is FinderInfo or ResourceFork (with or without "user." prefix). */
bool macos_is_apple_xattr(const char *name);

/* Returns true if xattr name is FinderInfo (with or without "user." prefix). */
bool macos_is_finderinfo_xattr(const char *name);

/* Returns true if xattr name is ResourceFork (with or without "user." prefix). */
bool macos_is_resourcefork_xattr(const char *name);

#endif /* QEMU_9P_MACOS_COMPAT_H */
