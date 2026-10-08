/*
 * Host optical drive media tracking on macOS
 *
 * DiskArbitration runs on a private dispatch queue. Disc arrival and
 * removal are queued and handed to the block driver in the main loop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "cdrom-darwin.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOMedia.h>
#include <DiskArbitration/DiskArbitration.h>

#define CD_DEVICE_CLASS     "IOCDBlockStorageDevice"
#define IMAGE_PREFIX        "image:"
#define IMAGE_DEVICE_CLASS  "IOHDIXHDDriveOutKernel"
#define RELEASE_TIMEOUT_MS  3000
#define DA_TIMEOUT_MS       5000

typedef struct WatchEvent {
    HostCDEvent ev;
    char bsd[32];
} WatchEvent;

struct HostCDWatch {
    char *id;                   /* drive name, or NULL */
    char *image;                /* disk image path prefix, or NULL */
    HostCDWatchFn *fn;
    void *opaque;
    QEMUBH *bh;

    QemuMutex lock;
    GQueue events;
    char bsd[32];               /* disc present, as DiskArbitration sees it */
    bool held;                  /* unmounted by us; mount again at the end */

    bool locked;
    bool self_eject;
    bool want_release;
    QemuSemaphore released;
    char eject_bsd[32];
    QemuSemaphore da_done;      /* unmount at start / mount at the end */

    DASessionRef session;
    dispatch_queue_t queue;
};

static void cfstr_to_utf8(CFStringRef str, char *buf, size_t len)
{
    buf[0] = '\0';
    if (str && CFGetTypeID(str) == CFStringGetTypeID()) {
        CFStringGetCString(str, buf, len, kCFStringEncodingUTF8);
    }
}

/* "<vendor> <product>" with runs of white space folded. */
static void drive_name(io_registry_entry_t dev, char *buf, size_t len)
{
    CFDictionaryRef dc;
    char vendor[64] = "", product[64] = "";
    g_autofree char *raw = NULL;
    g_auto(GStrv) words = NULL;

    buf[0] = '\0';
    dc = IORegistryEntryCreateCFProperty(dev, CFSTR("Device Characteristics"),
                                         kCFAllocatorDefault, 0);
    if (!dc) {
        return;
    }
    if (CFGetTypeID(dc) == CFDictionaryGetTypeID()) {
        cfstr_to_utf8(CFDictionaryGetValue(dc, CFSTR("Vendor Name")),
                      vendor, sizeof(vendor));
        cfstr_to_utf8(CFDictionaryGetValue(dc, CFSTR("Product Name")),
                      product, sizeof(product));
    }
    CFRelease(dc);

    raw = g_strdup_printf("%s %s", vendor, product);
    words = g_strsplit_set(g_strstrip(raw), " \t", -1);
    buf[0] = '\0';
    for (char **w = words; *w; w++) {
        if (**w) {
            if (buf[0]) {
                g_strlcat(buf, " ", len);
            }
            g_strlcat(buf, *w, len);
        }
    }
}

static bool image_path(io_registry_entry_t e, char *buf, size_t len)
{
    CFTypeRef p = IORegistryEntryCreateCFProperty(e, CFSTR("image-path"),
                                                  kCFAllocatorDefault, 0);
    bool ok = false;

    if (!p) {
        return false;
    }
    if (CFGetTypeID(p) == CFDataGetTypeID()) {
        size_t n = MIN(CFDataGetLength(p), len - 1);

        memcpy(buf, CFDataGetBytePtr(p), n);
        buf[n] = '\0';
        ok = true;
    } else if (CFGetTypeID(p) == CFStringGetTypeID()) {
        cfstr_to_utf8(p, buf, len);
        ok = true;
    }
    CFRelease(p);
    return ok;
}

/* Is @e the drive (or disk image device) the watch names? -1: not a drive */
static int device_matches(HostCDWatch *w, io_registry_entry_t e)
{
    char buf[PATH_MAX];

    if (w->image) {
        if (!image_path(e, buf, sizeof(buf))) {
            return -1;
        }
        return g_str_has_prefix(buf, w->image);
    }
    if (!IOObjectConformsTo(e, CD_DEVICE_CLASS)) {
        return -1;
    }
    drive_name(e, buf, sizeof(buf));
    return g_ascii_strcasecmp(buf, w->id) == 0;
}

/* Does IOMedia @media sit on the watched drive? */
static bool media_matches(HostCDWatch *w, io_service_t media)
{
    io_registry_entry_t e = media, parent;
    int m = -1;

    IOObjectRetain(e);
    while (m < 0 &&
           IORegistryEntryGetParentEntry(e, kIOServicePlane, &parent) ==
           KERN_SUCCESS) {
        IOObjectRelease(e);
        e = parent;
        m = device_matches(w, e);
    }
    IOObjectRelease(e);
    return m > 0;
}

static bool disk_matches(HostCDWatch *w, DADiskRef disk)
{
    io_service_t media = DADiskCopyIOMedia(disk);
    bool m;

    if (!media) {
        return false;
    }
    m = media_matches(w, media);
    IOObjectRelease(media);
    return m;
}

/* The whole-disc media below drive @dev, if any. */
static bool find_disc(io_registry_entry_t dev, char *bsd, size_t len)
{
    io_iterator_t it;
    io_registry_entry_t e;
    bool found = false;

    if (IORegistryEntryCreateIterator(dev, kIOServicePlane,
                                      kIORegistryIterateRecursively,
                                      &it) != KERN_SUCCESS) {
        return false;
    }
    while (!found && (e = IOIteratorNext(it))) {
        if (IOObjectConformsTo(e, kIOMediaClass)) {
            CFTypeRef whole = IORegistryEntryCreateCFProperty(
                e, CFSTR(kIOMediaWholeKey), kCFAllocatorDefault, 0);
            CFTypeRef name = IORegistryEntryCreateCFProperty(
                e, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);

            if (whole == kCFBooleanTrue && name) {
                cfstr_to_utf8(name, bsd, len);
                found = bsd[0];
            }
            if (whole) {
                CFRelease(whole);
            }
            if (name) {
                CFRelease(name);
            }
        }
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

/*
 * Resolve the first optical drive for an empty id and report the disc
 * present now. Returns false if no such drive exists at the moment.
 */
static bool scan_drives(HostCDWatch *w, char *bsd, size_t len)
{
    io_iterator_t it;
    io_registry_entry_t e;
    bool found = false;
    CFMutableDictionaryRef match;

    bsd[0] = '\0';
    match = IOServiceMatching(w->image ? IMAGE_DEVICE_CLASS : CD_DEVICE_CLASS);
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) !=
        KERN_SUCCESS) {
        return false;
    }
    while (!found && (e = IOIteratorNext(it))) {
        if (!w->image && !w->id[0]) {
            char name[128];

            drive_name(e, name, sizeof(name));
            g_free(w->id);
            w->id = g_strdup(name);
        }
        if (device_matches(w, e) > 0) {
            found = true;
            find_disc(e, bsd, len);
        }
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
    return found;
}

static void watch_post(HostCDWatch *w, HostCDEvent ev, const char *bsd)
{
    WatchEvent *e = g_new0(WatchEvent, 1);

    e->ev = ev;
    pstrcpy(e->bsd, sizeof(e->bsd), bsd ? bsd : "");
    qemu_mutex_lock(&w->lock);
    g_queue_push_tail(&w->events, e);
    qemu_mutex_unlock(&w->lock);
    qemu_bh_schedule(w->bh);
}

static void watch_bh(void *opaque)
{
    HostCDWatch *w = opaque;
    WatchEvent *e;

    for (;;) {
        qemu_mutex_lock(&w->lock);
        e = g_queue_pop_head(&w->events);
        qemu_mutex_unlock(&w->lock);
        if (!e) {
            break;
        }
        w->fn(w->opaque, e->ev, e->bsd);
        g_free(e);
    }
}

static void unmount_done(DADiskRef disk, DADissenterRef dissenter,
                         void *context)
{
    HostCDWatch *w = context;

    if (dissenter) {
        warn_report("host_cdrom: could not unmount %s (0x%x)",
                    DADiskGetBSDName(disk), DADissenterGetStatus(dissenter));
    }
    /* the disc cannot be opened while a volume of it is mounted */
    watch_post(w, HOST_CD_MEDIUM_IN, DADiskGetBSDName(disk));
}

static void unmount_sync_done(DADiskRef disk, DADissenterRef dissenter,
                              void *context)
{
    HostCDWatch *w = context;

    if (dissenter) {
        warn_report("host_cdrom: could not unmount %s (0x%x)",
                    DADiskGetBSDName(disk), DADissenterGetStatus(dissenter));
    }
    qemu_sem_post(&w->da_done);
}

static void do_unmount_sync(void *opaque)
{
    HostCDWatch *w = opaque;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, w->session,
                                             w->bsd);

    if (!disk) {
        qemu_sem_post(&w->da_done);
        return;
    }
    DADiskUnmount(disk, kDADiskUnmountOptionWhole, unmount_sync_done, w);
    CFRelease(disk);
}

static void mount_done(DADiskRef disk, DADissenterRef dissenter,
                       void *context)
{
    HostCDWatch *w = context;

    qemu_sem_post(&w->da_done);
}

static void disk_appeared(DADiskRef disk, void *context)
{
    HostCDWatch *w = context;
    const char *name = DADiskGetBSDName(disk);

    if (!name || !disk_matches(w, disk)) {
        return;
    }
    qemu_mutex_lock(&w->lock);
    pstrcpy(w->bsd, sizeof(w->bsd), name);
    w->held = true;
    qemu_mutex_unlock(&w->lock);
    DADiskUnmount(disk, kDADiskUnmountOptionWhole, unmount_done, w);
}

static void disk_disappeared(DADiskRef disk, void *context)
{
    HostCDWatch *w = context;
    const char *name = DADiskGetBSDName(disk);
    bool ours;

    qemu_mutex_lock(&w->lock);
    ours = name && w->bsd[0] && !strcmp(w->bsd, name);
    if (ours) {
        w->bsd[0] = '\0';
        w->held = false;
    }
    qemu_mutex_unlock(&w->lock);
    if (ours) {
        watch_post(w, HOST_CD_MEDIUM_OUT, name);
    }
}

static DADissenterRef mount_approval(DADiskRef disk, void *context)
{
    if (!disk_matches(context, disk)) {
        return NULL;
    }
    return DADissenterCreate(kCFAllocatorDefault, kDAReturnExclusiveAccess,
                             CFSTR("The disc is in use by QEMU."));
}

static DADissenterRef eject_approval(DADiskRef disk, void *context)
{
    HostCDWatch *w = context;

    if (qatomic_read(&w->self_eject) || !disk_matches(w, disk)) {
        return NULL;
    }
    if (qatomic_read(&w->locked)) {
        return DADissenterCreate(kCFAllocatorDefault, kDAReturnExclusiveAccess,
                                 CFSTR("The guest has locked the disc."));
    }
    qatomic_set(&w->want_release, true);
    watch_post(w, HOST_CD_EJECT_REQUEST, DADiskGetBSDName(disk));
    if (qemu_sem_timedwait(&w->released, RELEASE_TIMEOUT_MS) == 0) {
        return NULL;
    }
    if (!qatomic_xchg(&w->want_release, false)) {
        qemu_sem_wait(&w->released);
        return NULL;
    }
    return DADissenterCreate(kCFAllocatorDefault, kDAReturnBusy,
                             CFSTR("The disc is in use by QEMU."));
}

static void eject_done(DADiskRef disk, DADissenterRef dissenter,
                       void *context)
{
    HostCDWatch *w = context;

    qatomic_set(&w->self_eject, false);
    if (dissenter) {
        warn_report("host_cdrom: could not eject %s (0x%x)",
                    DADiskGetBSDName(disk), DADissenterGetStatus(dissenter));
    }
}

static void do_eject(void *opaque)
{
    HostCDWatch *w = opaque;
    DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, w->session,
                                             w->eject_bsd);

    if (!disk) {
        qatomic_set(&w->self_eject, false);
        return;
    }
    DADiskEject(disk, kDADiskEjectOptionDefault, eject_done, w);
    CFRelease(disk);
}

static void do_teardown(void *opaque)
{
    HostCDWatch *w = opaque;

    DAUnregisterApprovalCallback(w->session, mount_approval, w);
    DAUnregisterApprovalCallback(w->session, eject_approval, w);
    DAUnregisterCallback(w->session, disk_appeared, w);
    DAUnregisterCallback(w->session, disk_disappeared, w);
    if (w->held && w->bsd[0]) {
        DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault,
                                                 w->session, w->bsd);
        if (disk) {
            DADiskMount(disk, NULL, kDADiskMountOptionWhole, mount_done, w);
            CFRelease(disk);
            return;
        }
    }
    qemu_sem_post(&w->da_done);
}

static void do_detach(void *opaque)
{
    HostCDWatch *w = opaque;

    DASessionSetDispatchQueue(w->session, NULL);
}

HostCDWatch *host_cd_watch_new(const char *id, HostCDWatchFn *fn,
                               void *opaque, char *bsd, size_t bsd_len,
                               Error **errp)
{
    HostCDWatch *w = g_new0(HostCDWatch, 1);
    CFMutableDictionaryRef whole;
    const char *image;

    if (strstart(id, IMAGE_PREFIX, &image)) {
        if (!*image) {
            error_setg(errp, "host_cdrom: empty disk image path prefix");
            g_free(w);
            return NULL;
        }
        w->image = g_strdup(image);
    } else {
        w->id = g_strdup(id);
    }
    if (!scan_drives(w, bsd, bsd_len)) {
        if (!w->image && !w->id[0]) {
            error_setg(errp, "host_cdrom: no optical drive found");
            g_free(w->id);
            g_free(w);
            return NULL;
        }
        warn_report("host_cdrom: drive '%s' not found, waiting for it", id);
    }

    w->fn = fn;
    w->opaque = opaque;
    w->bh = qemu_bh_new(watch_bh, w);
    qemu_mutex_init(&w->lock);
    qemu_sem_init(&w->released, 0);
    qemu_sem_init(&w->da_done, 0);
    g_queue_init(&w->events);
    pstrcpy(w->bsd, sizeof(w->bsd), bsd);

    w->session = DASessionCreate(kCFAllocatorDefault);
    w->queue = dispatch_queue_create("org.qemu.host-cdrom", NULL);
    whole = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(whole, kDADiskDescriptionMediaWholeKey,
                         kCFBooleanTrue);
    DARegisterDiskAppearedCallback(w->session, whole, disk_appeared, w);
    DARegisterDiskDisappearedCallback(w->session, whole, disk_disappeared, w);
    DARegisterDiskMountApprovalCallback(w->session, NULL, mount_approval, w);
    DARegisterDiskEjectApprovalCallback(w->session, whole, eject_approval, w);
    CFRelease(whole);
    DASessionSetDispatchQueue(w->session, w->queue);
    return w;
}

void host_cd_watch_free(HostCDWatch *w)
{
    WatchEvent *e;

    if (!w) {
        return;
    }
    dispatch_sync_f(w->queue, w, do_teardown);
    /* the disc goes back to the host before the session ends */
    qemu_sem_timedwait(&w->da_done, DA_TIMEOUT_MS);
    dispatch_sync_f(w->queue, w, do_detach);
    CFRelease(w->session);
    dispatch_release(w->queue);
    qemu_bh_delete(w->bh);
    while ((e = g_queue_pop_head(&w->events))) {
        g_free(e);
    }
    qemu_sem_destroy(&w->released);
    qemu_sem_destroy(&w->da_done);
    qemu_mutex_destroy(&w->lock);
    g_free(w->id);
    g_free(w->image);
    g_free(w);
}

void host_cd_watch_set_locked(HostCDWatch *w, bool locked)
{
    qatomic_set(&w->locked, locked);
}

void host_cd_watch_released(HostCDWatch *w)
{
    if (qatomic_xchg(&w->want_release, false)) {
        qemu_sem_post(&w->released);
    }
}

void host_cd_watch_eject(HostCDWatch *w, const char *bsd)
{
    if (qatomic_xchg(&w->self_eject, true)) {
        return;
    }
    pstrcpy(w->eject_bsd, sizeof(w->eject_bsd), bsd);
    dispatch_async_f(w->queue, w, do_eject);
}

void host_cd_watch_present(HostCDWatch *w, char *bsd, size_t bsd_len)
{
    qemu_mutex_lock(&w->lock);
    pstrcpy(bsd, bsd_len, w->bsd);
    qemu_mutex_unlock(&w->lock);
}

void host_cd_watch_unmount(HostCDWatch *w)
{
    if (!w->bsd[0]) {
        return;
    }
    dispatch_async_f(w->queue, w, do_unmount_sync);
    qemu_sem_timedwait(&w->da_done, DA_TIMEOUT_MS);
}
