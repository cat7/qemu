/*
 * Host optical drive media tracking on macOS
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef BLOCK_CDROM_DARWIN_H
#define BLOCK_CDROM_DARWIN_H

typedef struct HostCDWatch HostCDWatch;

typedef enum HostCDEvent {
    HOST_CD_MEDIUM_IN,      /* @bsd: whole-disc BSD name */
    HOST_CD_MEDIUM_OUT,
    HOST_CD_EJECT_REQUEST,  /* the host wants the disc; close it */
} HostCDEvent;

/* Called in the main loop. */
typedef void HostCDWatchFn(void *opaque, HostCDEvent ev, const char *bsd);

/*
 * Watch the drive named @id: "" is the first optical drive, otherwise
 * "<vendor> <product>" of the drive, or "image:<path prefix>" for disk
 * images. The disc present now is returned in @bsd (empty if none).
 * Discs of the drive are kept unmounted while the watch exists.
 */
HostCDWatch *host_cd_watch_new(const char *id, HostCDWatchFn *fn,
                               void *opaque, char *bsd, size_t bsd_len,
                               Error **errp);
void host_cd_watch_free(HostCDWatch *w);
void host_cd_watch_set_locked(HostCDWatch *w, bool locked);
/* Call when the disc is closed after HOST_CD_EJECT_REQUEST. */
void host_cd_watch_released(HostCDWatch *w);
/* Eject disc @bsd; the descriptor must be closed. */
void host_cd_watch_eject(HostCDWatch *w, const char *bsd);
/* The disc of the drive, as last seen (empty if none). */
void host_cd_watch_present(HostCDWatch *w, char *bsd, size_t bsd_len);

#endif
