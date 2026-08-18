/*
 * QEMU ATI Rage 128 Pro emulation
 *
 * See ati_rage128.h for background. Milestone scope: correct PCI
 * identity and BAR layout for the real OEM Mac FCode ROM (which the
 * Beige G3's own Open Firmware loads from the expansion ROM BAR and
 * executes), enough CRTC/PLL/DAC/config register modeling for that
 * FCode's chip bring-up and mode-set, a linear framebuffer with the
 * per-aperture endian swapping the Mac driver uses, EDID over the
 * hardware I2C engine, and the VBLANK interrupt. No 2D/3D acceleration
 * yet -- every unmodeled register access is traced
 * (ati_rage128_unk_read/ati_rage128_unk_write) so the FCode's and
 * NDRV's real demands drive what gets implemented next, the same
 * trace-first method used to bring up ati_mach64.c.
 *
 * Register semantics per the official "RAGE 128 PRO Register Reference
 * Guide" RRG-G04500-C Rev 1.01 -- see ati_rage128_regs.h.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/i2c/i2c.h"

#include "ati_rage128_int.h"
#include "ati_rage128_regs.h"
#include "trace.h"

#define ATI_RAGE128_VBLANK_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)
#define ATI_RAGE128_VBLANK_LEN_NS    (ATI_RAGE128_VBLANK_PERIOD_NS / 8)

/* ---------------------------------------------------------------- */
/* Display                                                          */

static uint32_t ati_rage128_pixel_bytes(uint32_t pix_width)
{
    switch (pix_width) {
    case R128_PIX_WIDTH_8BPP:
        return 1;
    case R128_PIX_WIDTH_15BPP:
    case R128_PIX_WIDTH_16BPP:
        return 2;
    case R128_PIX_WIDTH_24BPP:
        return 3;
    case R128_PIX_WIDTH_32BPP:
        return 4;
    default:
        return 0; /* 4bpp and reserved codes: no linear fb path */
    }
}

static void ati_rage128_get_mode(ATIRage128State *s, ATIRage128Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[R128_CRTC_GEN_CNTL >> 2];
    uint32_t h_total_disp = s->regs[R128_CRTC_H_TOTAL_DISP >> 2];
    uint32_t v_total_disp = s->regs[R128_CRTC_V_TOTAL_DISP >> 2];
    uint32_t pix_width = (crtc_gen_cntl >> R128_CRTC_PIX_WIDTH_SHIFT) &
                         R128_CRTC_PIX_WIDTH_MASK;

    memset(mode, 0, sizeof(*mode));

    mode->width = (((h_total_disp >> R128_CRTC_H_DISP_SHIFT) &
                    R128_CRTC_H_DISP_MASK) + 1) * 8;
    mode->height = ((v_total_disp >> R128_CRTC_V_DISP_SHIFT) &
                    R128_CRTC_V_DISP_MASK) + 1;
    mode->bpp = ati_rage128_pixel_bytes(pix_width);
    /*
     * CRTC_PITCH is in units of 8 *pixels* for the display path (the
     * 24bpp bytes*8 exception applies to the render engine, not here
     * -- RRG-G04500-C 3.7 CRTC_PITCH note).
     */
    mode->pitch = ((s->regs[R128_CRTC_PITCH >> 2] & R128_CRTC_PITCH_MASK) * 8) *
                  (mode->bpp ? mode->bpp : 1);
    mode->fb_offset = s->regs[R128_CRTC_OFFSET >> 2] & R128_CRTC_OFFSET_MASK;
    mode->pix_width = pix_width;
}

static bool ati_rage128_mode_valid(ATIRage128State *s,
                                   const ATIRage128Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[R128_CRTC_GEN_CNTL >> 2];
    uint32_t crtc_ext_cntl = s->regs[R128_CRTC_EXT_CNTL >> 2];

    if (!(crtc_gen_cntl & R128_CRTC_EN) ||
        !(crtc_gen_cntl & R128_CRTC_EXT_DISP_EN)) {
        return false;
    }
    if (crtc_ext_cntl & R128_CRTC_DISPLAY_DIS) {
        return false;
    }
    if (mode->width < 16 || mode->height < 16 || mode->bpp == 0) {
        return false;
    }
    /* An undersized stride would corrupt the whole surface view. */
    if ((uint64_t)mode->pitch < (uint64_t)mode->width * mode->bpp) {
        return false;
    }
    if ((uint64_t)mode->fb_offset + (uint64_t)mode->pitch * mode->height >
        ATI_RAGE128_VRAM_SIZE) {
        return false;
    }
    return true;
}

/*
 * Snapshot the mode the instant a CRTC1 register write makes it valid,
 * rather than waiting for the next periodic gfx_update poll. Open
 * Firmware's own boot-time mode-set (e.g. picking a bigger console for
 * BootX) can assert a real, valid mode only fleetingly -- set it,
 * possibly draw through it once via some other internal path, then
 * move on -- entirely between two of QEMU's display-refresh ticks.
 * Without this, ati_rage128_update_display()'s own poll-based check
 * can miss that window completely and never learn the real
 * offset/pitch/dimensions the guest is actually using, even though
 * real content keeps landing there.
 */
static void ati_rage128_maybe_capture_mode(ATIRage128State *s)
{
    ATIRage128Mode mode;

    ati_rage128_get_mode(s, &mode);
    if (ati_rage128_mode_valid(s, &mode)) {
        s->mode = mode;
        s->have_valid_mode = true;
        s->mode_dirty = true;
    }
}

/*
 * All drawing converts through an allocated 32bpp surface. VRAM bytes
 * are decoded CHIP-NATIVE LITTLE-ENDIAN: with the aperture-1 byte
 * swapper actually modeled (see ati_rage128_aper1_ops), a big-endian
 * Mac guest's pixels land in VRAM in the chip's own layout, exactly
 * as on real hardware (verified live against Mac OS 9's lavender
 * desktop: bytes 9c 63 63 00 = LE B,G,R,X -- decoding them big-endian
 * was what tinted the desktop olive-green once the swapper existed;
 * the earlier BE decode had only ever looked right because the
 * missing swapper and the wrong decode cancelled out).
 */
static void ati_rage128_draw_8bpp(ATIRage128State *s, DisplaySurface *ds,
                                  const ATIRage128Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            uint8_t idx = src[x];
            dst[x] = 0xff000000u |
                     ((uint32_t)s->palette[idx][0] << 16) |
                     ((uint32_t)s->palette[idx][1] << 8) |
                     s->palette[idx][2];
        }
        src += mode->pitch;
    }
}

static void ati_rage128_draw_16bpp(ATIRage128State *s, DisplaySurface *ds,
                                   const ATIRage128Mode *mode, bool rgb565)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            uint16_t pixel = ((uint16_t)src[2 * x + 1] << 8) | src[2 * x];
            uint8_t r, g, b;

            if (rgb565) {
                r = ((pixel >> 11) & 0x1f) << 3;
                g = ((pixel >> 5) & 0x3f) << 2;
                b = (pixel & 0x1f) << 3;
                g |= g >> 6;
            } else {                        /* RGB555 */
                r = ((pixel >> 10) & 0x1f) << 3;
                g = ((pixel >> 5) & 0x1f) << 3;
                b = (pixel & 0x1f) << 3;
                g |= g >> 5;
            }
            r |= r >> 5;
            b |= b >> 5;
            dst[x] = 0xff000000u | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
        src += mode->pitch;
    }
}

static void ati_rage128_draw_32bpp(ATIRage128State *s, DisplaySurface *ds,
                                   const ATIRage128Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            /* chip-native little-endian: B,G,R,X in VRAM */
            dst[x] = 0xff000000u | ((uint32_t)src[4 * x + 2] << 16) |
                     ((uint32_t)src[4 * x + 1] << 8) | src[4 * x];
        }
        src += mode->pitch;
    }
}

static void ati_rage128_draw_24bpp(ATIRage128State *s, DisplaySurface *ds,
                                   const ATIRage128Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            /* chip-native little-endian: B,G,R in VRAM */
            dst[x] = 0xff000000u | ((uint32_t)src[3 * x + 2] << 16) |
                     ((uint32_t)src[3 * x + 1] << 8) | src[3 * x];
        }
        src += mode->pitch;
    }
}

/*
 * Auto-detect the real, currently-live framebuffer via VRAM write
 * activity rather than any register. Live testing (2026-08-02, both
 * Mac OS X 10.2 and Mac OS 9.2) found CRTC1's "Extended" mode-set
 * registers are never programmed by either guest OS at all -- not
 * through direct MMIO, not through MM_INDEX/MM_DATA, not through PM4
 * -- so ati_rage128_maybe_capture_mode() only ever captures whatever
 * mode Open Firmware's own boot console happened to establish once at
 * power-on. That's sufficient as long as the guest keeps using that
 * exact mode (confirmed live: OS X mirroring at 256 colors/8bpp
 * renders cleanly, matching that original boot mode) -- but switching
 * to a higher color depth makes the guest draw its real desktop
 * somewhere else in VRAM instead, through a mechanism that still
 * isn't understood, and the last-known-good CRTC1 mode has no way to
 * find it.
 *
 * What IS observable is that switching to a real live framebuffer
 * writes a large, contiguous block of VRAM at least once -- so scan
 * VRAM periodically (via QEMU's own per-MemoryRegion dirty-bitmap
 * tracking, the same DIRTY_MEMORY_VGA mechanism every other display
 * device uses to skip redrawing unchanged scanlines -- reused here
 * for discovery instead) for the largest contiguous span that was
 * written at all recently, and treat it as the real framebuffer once
 * it's been confirmed against a plausible resolution. A single write
 * is trusted immediately (a mostly-static desktop -- no cursor blink,
 * no clock tick landing in the scanned region -- may only ever paint
 * a freshly-switched mode once and then sit idle, so waiting for
 * *repeated* activity would miss it entirely); the size-vs-known-
 * resolution match already filters out noise from small, unrelated
 * writes, so a low per-block bar is safe here. The decay counter
 * still ages a region out over several idle scans once superseded,
 * so switching back to an earlier mode is eventually noticed too.
 */
/*
 * ACTIVITY_HIT is the per-block credit granted on a hit (decaying 1
 * per scan), i.e. how many scans a write stays "recent". It must
 * comfortably exceed the duration of a slow, multi-scan-straddling
 * canvas paint: the completed region only becomes visible as ONE run
 * once the last slice lands, and it must then survive un-shrunk for
 * at least the 2 scans the stability gate needs -- with credit N, a
 * paint spread over up to N-2 scans still adopts correctly (verified
 * in the qtest harness with a 6-scan paint).
 */
#define ATI_RAGE128_FB_SCAN_PERIOD      30
#define ATI_RAGE128_FB_ACTIVITY_HIT     12
#define ATI_RAGE128_FB_ACTIVITY_THRESH  1

/*
 * Real Mac resolutions this hardware/driver combination has actually
 * been observed offering or using (see the mach64 Monitors panel's
 * own Resolution list, live-tested this session) -- dirty tracking
 * alone can find a byte span but can't recover its 2D geometry, so a
 * detected span gets matched against whichever of these implies the
 * closest total size.
 */
static const struct { uint32_t width, height; } ati_rage128_known_modes[] = {
    { 640, 480 }, { 800, 600 }, { 832, 624 }, { 1024, 768 },
    { 1152, 870 }, { 1280, 960 }, { 1280, 1024 },
};

static uint32_t ati_rage128_pix_width_from_bpp(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return R128_PIX_WIDTH_8BPP;
    case 2:
        /*
         * 2 bytes/pixel is ambiguous by size alone -- RGB555 (15bpp)
         * and RGB565 (16bpp) are byte-identical in length, differing
         * only in bit packing. Classic Mac OS/Mac OS X's "Thousands
         * of colors" is conventionally 15bpp (unlike Windows, which
         * defaults to 16bpp), so guess that -- guessing 16bpp here
         * produces a real, visible, structured color-channel
         * corruption from the mismatched bit layout (reported live as
         * looking "endianness-troubled" -- it isn't a byte-order bug,
         * this device's 32bpp path was already confirmed correct
         * byte-for-byte against real content earlier this session,
         * but a 15-vs-16bpp mismatch looks similar enough to read
         * that way).
         */
        return R128_PIX_WIDTH_15BPP;
    case 4:
        return R128_PIX_WIDTH_32BPP;
    default:
        return 0;
    }
}

static void ati_rage128_pick_auto_fb(ATIRage128State *s, int nblocks)
{
    int i, run_start = -1, best_start = -1, best_len = 0, cur_len = 0;
    uint32_t best_size, best_score = UINT32_MAX;
    static const uint32_t bpp_candidates[] = { 1, 2, 4 };
    unsigned bi, mi;
    ATIRage128Mode candidate;
    bool found = false;

    for (i = 0; i <= nblocks; i++) {
        bool active = i < nblocks &&
                     s->fb_scan_activity[i] >= ATI_RAGE128_FB_ACTIVITY_THRESH;

        if (active) {
            if (run_start < 0) {
                run_start = i;
            }
            cur_len++;
        } else {
            if (cur_len > best_len) {
                best_len = cur_len;
                best_start = run_start;
            }
            run_start = -1;
            cur_len = 0;
        }
    }

    if (best_len == 0) {
        trace_ati_rage128_auto_fb(0, 0, 0, 0, s->auto_fb_valid, 0);
        s->auto_fb_pending_valid = false;
        return;
    }
    best_size = (uint32_t)best_len * ATI_RAGE128_FB_SCAN_BLOCK;

    memset(&candidate, 0, sizeof(candidate));
    for (mi = 0; mi < ARRAY_SIZE(ati_rage128_known_modes); mi++) {
        for (bi = 0; bi < ARRAY_SIZE(bpp_candidates); bi++) {
            uint32_t w = ati_rage128_known_modes[mi].width;
            uint32_t h = ati_rage128_known_modes[mi].height;
            uint32_t bpp = bpp_candidates[bi];
            uint32_t size = w * bpp * h;
            uint32_t diff = size > best_size ? size - best_size
                                             : best_size - size;

            if (diff < best_score) {
                best_score = diff;
                candidate.width = w;
                candidate.height = h;
                candidate.bpp = bpp;
                candidate.pitch = w * bpp;
                found = true;
            }
        }
    }

    /*
     * No plausible resolution matches within 10% of the detected
     * span -- don't guess.
     */
    if (!found || best_score > best_size / 10) {
        trace_ati_rage128_auto_fb((uint32_t)best_start *
                                  ATI_RAGE128_FB_SCAN_BLOCK,
                                  0, 0, best_size, s->auto_fb_valid,
                                  best_score);
        s->auto_fb_pending_valid = false;
        return;
    }

    candidate.fb_offset = (uint32_t)best_start * ATI_RAGE128_FB_SCAN_BLOCK;
    candidate.pix_width = ati_rage128_pix_width_from_bpp(candidate.bpp);
    if ((uint64_t)candidate.fb_offset +
        (uint64_t)candidate.pitch * candidate.height > ATI_RAGE128_VRAM_SIZE) {
        trace_ati_rage128_auto_fb(candidate.fb_offset, candidate.width,
                                  candidate.height, candidate.bpp,
                                  s->auto_fb_valid, best_score);
        s->auto_fb_pending_valid = false;
        return;
    }

    /*
     * Stability gate: only expose a candidate for adoption once the
     * same one has come out of two consecutive scans -- see the field
     * comment on auto_fb_pending in ati_rage128.h for the two churn
     * states this suppresses.
     *
     * Adoption is deliberately one-way: a scan that disagrees replaces
     * the *pending* candidate but never clears an already-adopted
     * framebuffer, and neither do the three "can't tell" early returns
     * above. Reverting to "none" on a single dissenting scan is what
     * this heuristic must not do, because the display then snaps back
     * to CRTC1's stale last-known-good mode for a frame and snaps
     * forward again the moment the run settles -- visible as a flicker
     * synchronised to whatever is writing VRAM. Mouse motion and an
     * animating progress bar both do exactly that: they add a second
     * competing write-run, so the largest-run pick alternates between
     * it and the real desktop. A static desktop hits the same edge
     * from the other side, since its activity credit eventually decays
     * to nothing and best_len falls to 0.
     *
     * Keeping the last adopted mode costs nothing when the guess was
     * right and is no worse than the stale CRTC1 mode when it wasn't;
     * a genuinely new framebuffer still takes over as soon as it has
     * been stable for the same two scans any first adoption needs.
     */
    if (s->auto_fb_pending_valid &&
        memcmp(&candidate, &s->auto_fb_pending, sizeof(candidate)) == 0) {
        s->auto_fb_mode = candidate;
        s->auto_fb_valid = true;
    }
    s->auto_fb_pending = candidate;
    s->auto_fb_pending_valid = true;
    trace_ati_rage128_auto_fb(candidate.fb_offset, candidate.width,
                              candidate.height, candidate.bpp,
                              s->auto_fb_valid, best_score);
}

static void ati_rage128_scan_vram_activity(ATIRage128State *s)
{
    int nblocks = ATI_RAGE128_VRAM_SIZE / ATI_RAGE128_FB_SCAN_BLOCK;
    DirtyBitmapSnapshot *snap;
    int i;

    if (++s->fb_scan_counter < ATI_RAGE128_FB_SCAN_PERIOD) {
        return;
    }
    s->fb_scan_counter = 0;

    snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0,
                                                   ATI_RAGE128_VRAM_SIZE,
                                                   DIRTY_MEMORY_VGA);
    for (i = 0; i < nblocks; i++) {
        bool dirty = memory_region_snapshot_get_dirty(
            &s->vram, snap, (hwaddr)i * ATI_RAGE128_FB_SCAN_BLOCK,
            ATI_RAGE128_FB_SCAN_BLOCK);

        if (dirty) {
            /*
             * Jump most of the way to the cap on a single hit rather
             * than incrementing by 1: a large canvas-filling write
             * can straddle a scan-period boundary, landing in two
             * separate snapshots. With a slow +1/-1 pace the earlier
             * half decays back to 0 right as the later half turns on,
             * so the two halves are never seen as one contiguous
             * region -- jumping to near-cap on any hit gives a hit
             * several scans (~seconds) of "recent" credit, long
             * enough for a split write's other half to show up too.
             */
            s->fb_scan_activity[i] = ATI_RAGE128_FB_ACTIVITY_HIT;
        } else if (s->fb_scan_activity[i] > 0) {
            s->fb_scan_activity[i]--;
        }
    }
    g_free(snap);

    ati_rage128_pick_auto_fb(s, nblocks);
}

static void ati_rage128_cursor_update(ATIRage128State *s);

static bool ati_rage128_update_display(void *opaque)
{
    ATIRage128State *s = opaque;
    ATIRage128Mode mode;
    DisplaySurface *ds;
    bool valid;

    ati_rage128_get_mode(s, &mode);
    valid = ati_rage128_mode_valid(s, &mode);
    trace_ati_rage128_update(mode.width, mode.height, mode.bpp, valid,
                             mode.fb_offset);
    if (!valid) {
        if (!s->have_valid_mode) {
            return true;
        }
        /*
         * CRTC_EN/CRTC_EXT_DISP_EN going away doesn't necessarily mean
         * the guest stopped drawing. Real Old World boot flow has
         * Open Firmware itself switch to a bigger console mode via
         * these same registers (e.g. BootX asking for a nicer startup
         * resolution); once classic Mac OS/Mac OS X's own generic
         * framebuffer driver takes over, it can go on drawing into
         * that same framebuffer indefinitely without ever restoring
         * these bits (observed live: real desktop content keeps
         * landing at a fixed offset/pitch while CRTC_EN reads 0 the
         * entire time). Once a real mode has been seen, keep
         * rendering it instead of going blank on a bit that's
         * advisory in practice -- actual blanking is DISPLAY_DIS in
         * CRTC_EXT_CNTL, already checked above.
         */
        mode = s->mode;
    } else {
        s->have_valid_mode = true;
    }

    ati_rage128_scan_vram_activity(s);
    /*
     * A CRTC mode that is valid *right now* is authoritative and is never
     * second-guessed. The heuristic only gets a say while the registers
     * are not currently describing a usable mode, i.e. when we are
     * falling back on the remembered one above. Letting a guess outrank
     * a live, correctly-programmed CRTC is how a guest-initiated
     * resolution switch got overridden right back (seen on mac99 as a
     * garbled 1024x768 over a valid 800x600, and here as the Monitors
     * panel switch snapping back).
     */
    if (!valid && s->auto_fb_valid &&
        (s->auto_fb_mode.fb_offset != mode.fb_offset ||
         s->auto_fb_mode.width != mode.width ||
         s->auto_fb_mode.height != mode.height ||
         s->auto_fb_mode.bpp != mode.bpp)) {
        /*
         * FALLBACK, activity-gated: the auto-detected region only
         * overrides the CRTC-derived mode when the CRTC's own
         * framebuffer region shows no recent write activity at all --
         * i.e. the CRTC registers describe a stale mode (typically
         * the boot console) while the guest is really painting
         * somewhere else, which is exactly the pre-CCE-support wedge
         * signature. A CRTC region that IS being written wins
         * unconditionally: with the CCE/GART path implemented the
         * real driver programs CRTC1 with fully valid modes
         * (confirmed live 2026-08-02: GEN_CNTL=0x03090600, an
         * 800x600@32bpp mode at offset 0x8000 -- earlier "CRTC is
         * never programmed" conclusions were an artifact of
         * byte-order-confused pmemsave analysis on my side, not of
         * guest behavior).
         */
        uint64_t crtc_len = (uint64_t)mode.pitch * mode.height;
        int first = mode.fb_offset / ATI_RAGE128_FB_SCAN_BLOCK;
        int last = (mode.fb_offset + crtc_len - 1) / ATI_RAGE128_FB_SCAN_BLOCK;
        bool crtc_region_live = false;
        int i;

        /*
         * Test the region of whichever mode we are actually about to
         * draw -- including the remembered one when the CRTC enable
         * bits are momentarily clear. Gating this on `valid` was
         * wrong: a mode-set is a disable/reprogram/re-enable sequence,
         * so CRTC_GEN_CNTL spends much of its life with the enables
         * down mid-switch. Skipping the check there left
         * crtc_region_live false, so the heuristic overrode a
         * perfectly good, actively-painted framebuffer every time.
         */
        for (i = first; i <= last &&
             i < (int)(ATI_RAGE128_VRAM_SIZE /
                       ATI_RAGE128_FB_SCAN_BLOCK); i++) {
            if (s->fb_scan_activity[i] >= ATI_RAGE128_FB_ACTIVITY_THRESH) {
                crtc_region_live = true;
                break;
            }
        }
        if (!crtc_region_live) {
            mode = s->auto_fb_mode;
        }
        if (!crtc_region_live != s->auto_fb_overriding) {
            s->auto_fb_overriding = !crtc_region_live;
            trace_ati_rage128_mode_override(s->auto_fb_overriding,
                                            mode.width, mode.height,
                                            mode.bpp, mode.fb_offset,
                                            mode.pitch);
        }
    } else if (s->auto_fb_overriding) {
        s->auto_fb_overriding = false;
        trace_ati_rage128_mode_override(false, mode.width, mode.height,
                                        mode.bpp, mode.fb_offset,
                                        mode.pitch);
    }

    /*
     * Only reallocate the DisplaySurface when its geometry actually
     * changes. It used to be recreated whenever *any* part of the mode
     * differed or mode_dirty was set -- but mode_dirty is raised by every
     * CRTC register write, and CRTC_OFFSET is written on every buffer
     * flip ("Updated for buffer flips", RRG-G04500-C 3.9), so a
     * double-buffering guest tore the surface down several times a
     * second. A changed offset or pitch only changes where we read from.
     *
     * Compare against the surface's own dimensions rather than against
     * the previously recorded mode. Those two can drift apart -- the
     * console can end up holding a surface this function did not create
     * -- and a stale s->mode that already "matches" then suppresses the
     * reallocation forever, leaving the guest drawing a larger desktop
     * into a smaller surface (mac99 saw an 800x600 desktop in a 640x480
     * surface). Asking the surface is self-correcting; asking our own
     * bookkeeping is not.
     */
    ds = qemu_console_surface(s->con);
    if (!ds || surface_width(ds) != (int)mode.width ||
        surface_height(ds) != (int)mode.height) {
        trace_ati_rage128_surface_realloc(ds ? surface_width(ds) : 0,
                                          ds ? surface_height(ds) : 0,
                                          mode.width, mode.height, mode.bpp,
                                          mode.fb_offset, mode.pitch);
        ds = qemu_create_displaysurface(mode.width, mode.height);
        qemu_console_set_surface(s->con, ds);
    }
    s->mode = mode;
    s->mode_dirty = false;
    ds = qemu_console_surface(s->con);
    switch (mode.pix_width) {
    case R128_PIX_WIDTH_8BPP:
        ati_rage128_draw_8bpp(s, ds, &mode);
        break;
    case R128_PIX_WIDTH_15BPP:
        ati_rage128_draw_16bpp(s, ds, &mode, false);
        break;
    case R128_PIX_WIDTH_16BPP:
        ati_rage128_draw_16bpp(s, ds, &mode, true);
        break;
    case R128_PIX_WIDTH_24BPP:
        ati_rage128_draw_24bpp(s, ds, &mode);
        break;
    case R128_PIX_WIDTH_32BPP:
        ati_rage128_draw_32bpp(s, ds, &mode);
        break;
    default:
        break;
    }
    ati_rage128_cursor_update(s);
    qemu_console_update_full(s->con);

    return true;
}

static const GraphicHwOps ati_rage128_gfx_ops = {
    .gfx_update = ati_rage128_update_display,
};

/* ---------------------------------------------------------------- */
/* VBLANK interrupt: gated pulse, as validated on the mach64 device */

static void ati_rage128_update_irq(ATIRage128State *s)
{
    uint32_t pending = s->regs[R128_GEN_INT_STATUS >> 2] &
                       s->regs[R128_GEN_INT_CNTL >> 2] &
                       R128_GEN_INT_ACK_MASK;

    pci_set_irq(PCI_DEVICE(s), pending != 0);
}

static void ati_rage128_vblank_end_tick(void *opaque)
{
    ATIRage128State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), 0);
}

static void ati_rage128_vblank_timer_tick(void *opaque)
{
    ATIRage128State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next_blank;

    if (s->regs[R128_CRTC_GEN_CNTL >> 2] & R128_CRTC_EN) {
        s->regs[R128_GEN_INT_STATUS >> 2] |= R128_CRTC_VBLANK_INT |
                                             R128_CRTC_VSYNC_INT;
        /* CRTC_STATUS.CRTC_VBLANK_SAVE latches until cleared */
        s->regs[R128_CRTC_STATUS >> 2] |= R128_CRTC_VBLANK_SAVE;
        if (s->regs[R128_GEN_INT_CNTL >> 2] &
            (R128_CRTC_VBLANK_INT | R128_CRTC_VSYNC_INT)) {
            trace_ati_rage128_vblank_irq(1,
                s->regs[R128_GEN_INT_CNTL >> 2]);
            pci_set_irq(PCI_DEVICE(s), 1);
            timer_mod(s->vblank_end_timer, now + ATI_RAGE128_VBLANK_LEN_NS);
        }
    }

    /* Tick at each frame's blank-start phase (last 1/8 of the period),
     * agreeing with the phase-computed CRTC_VBLANK_CUR status bit. */
    next_blank = (now / ATI_RAGE128_VBLANK_PERIOD_NS + 1) *
                 ATI_RAGE128_VBLANK_PERIOD_NS - ATI_RAGE128_VBLANK_LEN_NS;
    if (next_blank <= now) {
        next_blank += ATI_RAGE128_VBLANK_PERIOD_NS;
    }
    timer_mod(s->vblank_timer, next_blank);
}

/* ---------------------------------------------------------------- */
/*
 * Minimal DDC EEPROM slave for the GPIO_MONID bit-banged bus. Serves
 * the device's own 128-byte EDID (the same image the hardware I2C
 * engine answers with): a written byte sets the read offset, reads
 * return successive EDID bytes.
 */
#define TYPE_ATI_RAGE128_DDC "ati-rage128-ddc"
OBJECT_DECLARE_SIMPLE_TYPE(ATIRage128DDCState, ATI_RAGE128_DDC)

struct ATIRage128DDCState {
    I2CSlave parent_obj;
    uint8_t reg;
    const uint8_t *edid;
};

static int ati_rage128_ddc_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t ati_rage128_ddc_recv(I2CSlave *i2c)
{
    ATIRage128DDCState *d = ATI_RAGE128_DDC(i2c);

    return d->edid ? d->edid[d->reg++ & 0x7f] : 0xff;
}

static int ati_rage128_ddc_send(I2CSlave *i2c, uint8_t data)
{
    ATI_RAGE128_DDC(i2c)->reg = data;
    return 0;
}

static void ati_rage128_ddc_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = ati_rage128_ddc_event;
    k->recv = ati_rage128_ddc_recv;
    k->send = ati_rage128_ddc_send;
}

static const TypeInfo ati_rage128_ddc_info = {
    .name = TYPE_ATI_RAGE128_DDC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ATIRage128DDCState),
    .class_init = ati_rage128_ddc_class_init,
};

/* ---------------------------------------------------------------- */
/* Hardware I2C engine serving EDID (DDC addresses 0xA0/0xA1)       */

static void ati_rage128_i2c_go(ATIRage128State *s)
{
    uint32_t cntl0 = s->regs[R128_I2C_CNTL_0 >> 2];
    uint32_t cntl1 = s->regs[R128_I2C_CNTL_1 >> 2];
    uint8_t addr = (cntl1 >> R128_I2C_ADDR_SHIFT) & R128_I2C_ADDR_MASK;
    uint8_t count = (cntl1 >> R128_I2C_DATA_COUNT_SHIFT) &
                    R128_I2C_DATA_COUNT_MASK;
    int i;

    cntl0 &= ~(R128_I2C_DONE | R128_I2C_NACK | R128_I2C_GO);

    if ((addr & 0xfe) == 0xa0) {                /* DDC EDID slave */
        if (cntl0 & R128_I2C_RECEIVE) {
            s->i2c_data_len = 0;
            s->i2c_data_pos = 0;
            for (i = 0; i < count && i < (int)sizeof(s->i2c_data_fifo); i++) {
                s->i2c_data_fifo[i] = s->edid[s->i2c_offset++ & 0x7f];
                s->i2c_data_len++;
            }
        } else {
            /* the written byte(s) set the EDID word offset */
            if (s->i2c_data_len > 0) {
                s->i2c_offset = s->i2c_data_fifo[s->i2c_data_len - 1];
            }
            s->i2c_data_len = 0;
            s->i2c_data_pos = 0;
        }
        cntl0 |= R128_I2C_DONE;
    } else {
        cntl0 |= R128_I2C_DONE | R128_I2C_NACK;
    }
    trace_ati_rage128_i2c(addr, count, !!(cntl0 & R128_I2C_RECEIVE),
                          !!(cntl0 & R128_I2C_NACK), s->i2c_offset);

    s->regs[R128_I2C_CNTL_0 >> 2] = cntl0;
}

/* ---------------------------------------------------------------- */
/* Register file                                                    */

static int64_t ati_rage128_beam_phase_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) %
           ATI_RAGE128_VBLANK_PERIOD_NS;
}

static uint32_t ati_rage128_reg_read32(ATIRage128State *s, uint32_t base)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint32_t val = s->regs[base >> 2];

    switch (base) {
    case R128_MM_DATA:
        val = 0;
        if ((s->regs[R128_MM_INDEX >> 2] & 0x3ffc) != R128_MM_DATA) {
            val = ati_rage128_reg_read32(s,
                s->regs[R128_MM_INDEX >> 2] & 0x3ffc);
        }
        break;
    case R128_CLOCK_CNTL_DATA:
    {
        unsigned idx = s->regs[R128_CLOCK_CNTL_INDEX >> 2] &
                       R128_PLL_ADDR_MASK;

        val = s->plls[idx];
        /*
         * The PLL update is instant here, so the atomic-update
         * handshake bit always reads back clear (see
         * R128_PPLL_ATOMIC_UPDATE).
         */
        if (idx >= R128_PLL_PPLL_REF_DIV && idx <= R128_PLL_PPLL_DIV_3) {
            val &= ~R128_PPLL_ATOMIC_UPDATE;
        }
        trace_ati_rage128_pll_read(idx, val);
        break;
    }
    case R128_CRTC_STATUS:
    {
        bool in_vblank = ati_rage128_beam_phase_ns() >=
                         (ATI_RAGE128_VBLANK_PERIOD_NS -
                          ATI_RAGE128_VBLANK_LEN_NS);

        val = (val & ~(uint32_t)R128_CRTC_VBLANK_CUR) |
              (in_vblank ? R128_CRTC_VBLANK_CUR : 0) |
              R128_FIX_VSYNC_TIMING;
        break;
    }
    case R128_CRTC_VLINE_CRNT_VLINE:
    {
        /* current scanline in [31:16], trigger vline in [10:0] */
        uint32_t height = ((s->regs[R128_CRTC_V_TOTAL_DISP >> 2] >>
                            R128_CRTC_V_DISP_SHIFT) &
                           R128_CRTC_V_DISP_MASK) + 1;
        uint32_t line = ati_rage128_beam_phase_ns() * height /
                        ATI_RAGE128_VBLANK_PERIOD_NS;

        val = (val & 0x7ff) | ((line & 0x7ff) << 16);
        break;
    }
    case R128_CRTC_CRNT_FRAME:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
              ATI_RAGE128_VBLANK_PERIOD_NS;
        break;
    case R128_DAC_CNTL:
        /*
         * DAC comparator always reports the sensed levels of a
         * connected color monitor (all three RGB lines terminated) --
         * the FCode's CRT detection drives test colors and polls this.
         */
        val |= R128_DAC_CMP_OUTPUT;
        break;
    case R128_GPIO_MONID:
        /*
         * MONID_Y (read-only pad-level readback, bits 11:8) was
         * previously left as whatever raw value happened to be stored,
         * so the FCode's monitor-sense bit-bang (drive one pad low via
         * MONID_A/MONID_EN, then poll MONID_Y for the other pads) never
         * saw the "released pad floats high" response and spun -- live
         * trace showed 265k+ back-to-back reads of this exact byte
         * stuck at 0. Real open-drain GPIO pads with an internal weak
         * pull-up read back their own driven value while in output
         * mode, and read high while floating in input mode with
         * nothing externally pulling them low (no monitor attached in
         * this model) -- there is no dedicated DDC engine wired to
         * these 4 pads (unlike the hardware I2C_CNTL_0/1/DATA engine
         * above, which already serves EDID), so this is the physically
         * correct default response absent one.
         */
    {
        uint32_t en = (val >> 16) & 0xf;
        uint32_t a = val & 0xf;
        uint32_t y = (a & en) | (~en & 0xf);

        /*
         * Apple Monitor Sense (Technical Note HW30) on MONID0-2, same
         * connected display the onboard mach64 presents (a MultiScan
         * Band-3: standard code 6, extended code 0x23 -- the exact
         * monitor whose codes are already proven against this ROM and
         * OS on the mach64 side). Without a sense response the classic
         * Mac OS driver labels the port a generic "VGA Display" and
         * offers era-absurd refresh lists (observed live: 120Hz).
         * Probe patterns: all three lines floating = standard code on
         * Y[2:0]; exactly one line driven low = the extended code's
         * two-bit answer on the other two lines.
         */
        if (((val >> 24) & 0xf) == 0xf) {
            /*
             * DDC readback (see the write handler): SDA on Y0, SCL on
             * Y1, upper pads float high. The Apple-sense answers below
             * must NOT fire here -- their Y0-clearing ext-code replies
             * made the driver's DDC check read SDA as stuck low, so it
             * abandoned DDC and fell back to the generic "VGA
             * Display" (observed identically for Mac OS X's ndrv on
             * mac99 and, live on this machine, for the classic Mac OS
             * driver's boot-time probe).
             *
             * While the guest merely samples (no SCL clocking), a
             * floating SDA carries the VESA DDC1 stream: the monitor
             * shifts the 128-byte EDID out continuously, one bit per
             * VSYNC, in 9-bit frames (8 data bits MSB first + one
             * high null bit). The driver holds this state and samples
             * across frames looking for a moving bitstream; a static
             * level reads as "no DDC monitor". The bit position
             * advances in the VBLANK timer.
             */
            /*
             * Driven pads read their own level and floating pads pull
             * up, exactly as outside the DDC session -- the FCode's
             * post-handshake pin survey pulses EVERY pad low in turn
             * and requires the readback to match, so the session must
             * not repaint the port wholesale. Only two pads carry
             * extra signals:
             *
             * - Pad 0 (SDA): a floating pad reads the DDC2 slave's
             *   output while a transaction holds the line, and the
             *   VESA DDC1 EDID bitstream otherwise -- the monitor
             *   shifts its EDID out continuously, one bit per VSYNC
             *   (advanced in the vblank tick), in 9-bit frames: 8
             *   data bits MSB first plus a high null bit. The
             *   FCode's bulk reader (word 0x95a) is a DDC1 sampler:
             *   it reads VSYNC-synced bytes until the value CHANGES
             *   (its stream-alive check, up to 54 reads), then
             *   captures 128 x 9 x 2 samples and decodes/checksums
             *   them into the "EDID" property.
             *
             * - Pad 3: the VSYNC loopback sense. The FCode flips
             *   CRTC_V_SYNC_STRT_WID's V_SYNC_POL (bit 23) and
             *   requires this pad to follow (polled every ~1ms, up to
             *   32 tries per level) before it proceeds at all -- the
             *   "is a DDC-wired monitor cable physically present"
             *   check. A pad stuck high fails it, and the FCode then
             *   never publishes the "EDID" property, which is what
             *   left classic Mac OS naming the display a generic
             *   "VGA Display".
             */
            if (!(en & 1)) {
                if (!s->monid_sda) {
                    y &= ~1u; /* DDC2 slave holding SDA low */
                } else {
                    uint32_t frame = s->ddc1_pos / 9;
                    uint32_t bit = s->ddc1_pos % 9;
                    int sda = (bit == 8) ? 1 :
                        (s->edid[frame % sizeof(s->edid)] >> (7 - bit)) & 1;

                    y = (y & ~1u) | sda;
                }
            }
            if (!(en & 8) && s->monitor_connected) {
                y = (y & ~8u) |
                    (((s->regs[R128_CRTC_V_SYNC_STRT_WID >> 2] >> 23) & 1)
                     << 3);
            }
        } else if (s->monitor_connected) {
            uint32_t drv = en & 7 & ~a;   /* lines driven low */

            if ((en & 7) == 0) {
                /* standard sense: code 6 = 0b110 on Y2..Y0 */
                y = (y & ~7u) | 6;
            } else if (drv == 4 && (en & 7) == 4) {
                /* sense2 low: ext[5:4] -> (Y1,Y0) = 1,0 */
                y = (y & ~3u) | 2;
            } else if (drv == 2 && (en & 7) == 2) {
                /* sense1 low: ext[3:2] -> (Y2,Y0) = 0,0 */
                y &= ~5u;
            } else if (drv == 1 && (en & 7) == 1) {
                /* sense0 low: ext[1:0] -> (Y2,Y1) = 1,1 */
                y |= 6;
            }
        }

        val = (val & ~(0xfu << 8)) | (y << 8);
        trace_ati_rage128_monid_t(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  s->regs[base >> 2], y);
        break;
    }
    case R128_GPIO_MONIDB: {
        /*
         * Second GPIO port (see the regs.h comment): pull-up physics
         * like GPIO_MONID, plus the FCode's DDC-presence handshake --
         * its word 0x918 toggles CRTC_OFFSET bit 23 and requires pad 3
         * to follow within ~32ms before it will run the bulk EDID
         * read, so a floating pad 3 mirrors that bit.
         */
        uint32_t en = (val >> 16) & 0xf;
        uint32_t a = val & 0xf;
        uint32_t y = (a & en) | (~en & 0xf);

        if (!(en & 8)) {
            y = (y & ~8u) |
                (((s->regs[R128_CRTC_OFFSET >> 2] >> 23) & 1) << 3);
        }
        val = (val & ~(0xfu << 8)) | (y << 8);
        trace_ati_rage128_monidb(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                 s->regs[base >> 2], y);
        break;
    }
    case R128_PALETTE_INDEX:
        val = s->dac_wr_index | ((uint32_t)s->dac_rd_index << 16);
        break;
    case R128_PALETTE_DATA:
        val = ((uint32_t)s->palette[s->dac_rd_index][0] << 16) |
              ((uint32_t)s->palette[s->dac_rd_index][1] << 8) |
              s->palette[s->dac_rd_index][2];
        s->dac_rd_index++;
        break;
    case R128_I2C_DATA:
        val = 0;
        if (s->i2c_data_pos < s->i2c_data_len) {
            val = s->i2c_data_fifo[s->i2c_data_pos++];
        }
        break;
    case R128_AMCGPIO_Y_MIR:
        /*
         * Same floating-pin pull-up default as GPIO_MONID above; the
         * FCode drives this mirror's byte lane [23:16] alongside
         * GPIO_MONID during the same probe.
         */
        val = (s->regs[R128_AMCGPIO_A_MIR >> 2] &
               s->regs[R128_AMCGPIO_EN_MIR >> 2]) |
              ~s->regs[R128_AMCGPIO_EN_MIR >> 2];
        trace_ati_rage128_amcgpio(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  'Y', s->regs[R128_AMCGPIO_A_MIR >> 2],
                                  s->regs[R128_AMCGPIO_EN_MIR >> 2], val);
        break;
    case R128_PM4_STAT:
        /*
         * Engine idle: all FIFO entries free (192 covers every
         * partitioning mode's fifo size), BUSY/GUI_ACTIVE clear --
         * matches the Linux driver's r128_do_cce_idle() check.
         */
        val = 192;
        break;
    case R128_PM4_BUFFER_OFFSET:
        val = s->pm4_buffer_addr;
        break;
    case R128_PM4_BUFFER_CNTL:
        val = s->pm4_buffer_cntl;
        break;
    case R128_PM4_BUFFER_DL_RPTR:
        val = s->pm4_rptr;
        break;
    case R128_PM4_BUFFER_DL_WPTR:
        val = s->pm4_wptr;
        break;
    case R128_PM4_MICROCODE_DATAH:
        val = s->pm4_microcode[s->pm4_ucode_raddr][0];
        break;
    case R128_PM4_MICROCODE_DATAL:
        val = s->pm4_microcode[s->pm4_ucode_raddr][1];
        s->pm4_ucode_raddr = (s->pm4_ucode_raddr + 1) &
                             (R128_PM4_MICROCODE_WORDS - 1);
        break;
    case R128_PM4_BUFFER_ADDR:
        /* read as a fence by init code ("as per the sample code") */
        val = 0;
        break;
    case R128_CONFIG_MEMSIZE:
        val = ATI_RAGE128_VRAM_SIZE;
        break;
    case R128_CONFIG_APER_0_BASE:
        val = pci_get_long(dev->config + PCI_BASE_ADDRESS_0) &
              PCI_BASE_ADDRESS_MEM_MASK;
        break;
    case R128_CONFIG_APER_1_BASE:
        val = (pci_get_long(dev->config + PCI_BASE_ADDRESS_0) &
               PCI_BASE_ADDRESS_MEM_MASK) + ATI_RAGE128_APER_SIZE / 2;
        break;
    case R128_CONFIG_APER_SIZE:
        /* size of one aperture image; the BAR holds two of them */
        val = ATI_RAGE128_APER_SIZE / 2;
        break;
    case R128_CONFIG_REG_1_BASE:
        val = pci_get_long(dev->config + PCI_BASE_ADDRESS_2) &
              PCI_BASE_ADDRESS_MEM_MASK;
        break;
    case R128_CONFIG_REG_APER_SIZE:
        val = ATI_RAGE128_MMIO_SIZE;
        break;
    case R128_CONFIG_XSTRAP:
        val = R128_XSTRAP_ADDIN_CARD;
        break;
    case R128_GEN_STATUS:
    case R128_CONFIG_BONDS:
        val = 0;
        break;
    case R128_SW_SEMAPHORE:
        /* report all 8 semaphores free (bit reads as acquired-ok) */
        val = 0xff;
        break;
    case R128_MEM_STR_CNTL:
        val = s->regs[base >> 2];
        break;
    case R128_PC_NGUI_CTLSTAT:
        /* pixel cache idle: BUSY (bit 31) clear */
        val = s->regs[base >> 2] & 0x3fffffff;
        break;
    case R128_GUI_STAT:
        /* engine idle, all 64 command FIFO entries free */
        val = 0x40;
        break;
    case R128_DST_OFFSET:
        val = s->dst_offset_reg;
        break;
    case R128_DST_PITCH:
        val = s->dst_pitch_reg | (s->dst_tile_reg << 16);
        break;
    case R128_DST_WIDTH:
        val = s->dst_width;
        break;
    case R128_DST_HEIGHT:
        val = s->dst_height;
        break;
    case R128_SRC_X:
        val = s->src_x;
        break;
    case R128_SRC_Y:
        val = s->src_y;
        break;
    case R128_DST_X:
        val = s->dst_x;
        break;
    case R128_DST_Y:
        val = s->dst_y;
        break;
    case R128_DP_GUI_MASTER_CNTL:
        /* aliases fields from DP_MIX and DP_DATATYPE -- see the write case */
        val = s->dp_gui_master_cntl |
              ((s->dp_datatype & R128_DP_BRUSH_DATATYPE) >> 4) |
              ((s->dp_datatype & R128_DP_DST_DATATYPE) << 8) |
              ((s->dp_datatype & R128_DP_SRC_DATATYPE) >> 4) |
              (s->dp_mix & R128_DP_ROP3) |
              ((s->dp_mix & R128_DP_SRC_SOURCE) << 16);
        break;
    /*
     * The packed PITCH_OFFSET form and the separate PITCH/OFFSET
     * registers are two views of one piece of state on real silicon, so
     * a read through either has to see what was written through the
     * other. Mac OS X programs the separate registers exclusively, which
     * left the packed ones reading a permanent zero.
     */
    case R128_SRC_PITCH_OFFSET:
        val = (s->src_offset_reg >> 5) |
              (s->src_pitch_reg << R128_PITCH_OFFSET_PITCH_SHIFT) |
              (s->src_tile_reg << 31);
        break;
    case R128_DST_PITCH_OFFSET:
    case R128_DST_PITCH_OFFSET_C:
        val = (s->dst_offset_reg >> 5) |
              (s->dst_pitch_reg << R128_PITCH_OFFSET_PITCH_SHIFT) |
              (s->dst_tile_reg << 31);
        break;
    case R128_SRC_OFFSET:
        val = s->src_offset_reg;
        break;
    case R128_SRC_PITCH:
        val = s->src_pitch_reg | (s->src_tile_reg << 16);
        break;
    case R128_DP_BRUSH_BKGD_CLR:
        val = s->dp_brush_bkgd_clr;
        break;
    case R128_DP_BRUSH_FRGD_CLR:
        val = s->dp_brush_frgd_clr;
        break;
    case R128_DP_SRC_FRGD_CLR:
        val = s->dp_src_frgd_clr;
        break;
    case R128_DP_SRC_BKGD_CLR:
        val = s->dp_src_bkgd_clr;
        break;
    case R128_DP_CNTL_XDIR_YDIR_YMAJOR:
        /*
         * The same two directions as DP_CNTL, in different bit
         * positions. Keep DP_CNTL as the canonical copy so the 2D engine
         * has one place to look.
         */
        s->dp_cntl &= ~(R128_DST_X_LEFT_TO_RIGHT | R128_DST_Y_TOP_TO_BOTTOM);
        if (val & R128_DST_X_DIR_LEFT_TO_RIGHT) {
            s->dp_cntl |= R128_DST_X_LEFT_TO_RIGHT;
        }
        if (val & R128_DST_Y_DIR_TOP_TO_BOTTOM) {
            s->dp_cntl |= R128_DST_Y_TOP_TO_BOTTOM;
        }
        break;
    case R128_DP_CNTL:
        val = s->dp_cntl;
        break;
    case R128_DP_DATATYPE:
        val = s->dp_datatype;
        break;
    case R128_DP_MIX:
        val = s->dp_mix;
        break;
    case R128_DP_WRITE_MASK:
        val = s->dp_write_mask;
        break;
    case R128_DEFAULT_OFFSET:
        val = s->default_offset;
        break;
    case R128_DEFAULT_PITCH:
        val = s->default_pitch;
        break;
    case R128_DEFAULT_SC_BOTTOM_RIGHT:
        val = s->default_sc_right | (s->default_sc_bottom << 16);
        break;
    case R128_SC_TOP:
        val = s->sc_top_reg;
        break;
    case R128_SC_LEFT:
        val = s->sc_left_reg;
        break;
    case R128_SC_BOTTOM:
        val = s->sc_bottom_reg;
        break;
    case R128_SC_RIGHT:
        val = s->sc_right_reg;
        break;
    case R128_SRC_SC_BOTTOM:
        val = s->src_sc_bottom_reg;
        break;
    case R128_SRC_SC_RIGHT:
        val = s->src_sc_right_reg;
        break;
    case R128_CFG_MIRROR_BASE ... R128_CFG_MIRROR_END:
        val = pci_default_read_config(dev, base - R128_CFG_MIRROR_BASE, 4);
        break;
    default:
        break;
    }
    return val;
}

static void ati_rage128_bm_gui_run(ATIRage128State *s, uint32_t table);
static void ati_rage128_pm4_run(ATIRage128State *s);
static void ati_rage128_pm4_fifo_push(ATIRage128State *s, uint32_t val);
static void ati_rage128_pm4_indirect(ATIRage128State *s, uint32_t offset,
                                     uint32_t dwords);

/*
 * Re-derive the effective 2D pitch/offset and scissors from the register
 * values, honouring DP_GUI_MASTER_CNTL's per-operation source selects.
 *
 * Those GMC bits pick where each value comes from; they do not move it.
 * Applying them destructively (writing DEFAULT_* into the effective field
 * on a GMC write) loses the register underneath, so whether a blit saw the
 * right pitch came down to whether the driver wrote SRC_PITCH before or
 * after the GMC dword. Recomputing instead means either order works, and a
 * surface's pitch can no longer leak into the next, differently sized one.
 */
static void ati_rage128_resolve_gui_context(ATIRage128State *s)
{
    uint32_t gmc = s->dp_gui_master_cntl;

    if (gmc & R128_GMC_SRC_PITCH_OFFSET_CNTL) {
        s->src_offset = s->src_offset_reg;
        s->src_pitch = s->src_pitch_reg;
        s->src_tile = s->src_tile_reg;
    } else {
        s->src_offset = s->default_offset;
        s->src_pitch = s->default_pitch;
        s->src_tile = 0;
    }

    if (gmc & R128_GMC_DST_PITCH_OFFSET_CNTL) {
        s->dst_offset = s->dst_offset_reg;
        s->dst_pitch = s->dst_pitch_reg;
        s->dst_tile = s->dst_tile_reg;
    } else {
        s->dst_offset = s->default_offset;
        s->dst_pitch = s->default_pitch;
        s->dst_tile = 0;
    }

    if (gmc & R128_GMC_SRC_CLIPPING) {
        s->src_sc_right = s->src_sc_right_reg;
        s->src_sc_bottom = s->src_sc_bottom_reg;
    } else {
        s->src_sc_right = s->default_sc_right;
        s->src_sc_bottom = s->default_sc_bottom;
    }

    if (gmc & R128_GMC_DST_CLIPPING) {
        s->sc_top = s->sc_top_reg;
        s->sc_left = s->sc_left_reg;
        s->sc_right = s->sc_right_reg;
        s->sc_bottom = s->sc_bottom_reg;
    } else {
        s->sc_top = 0;
        s->sc_left = 0;
        s->sc_right = s->default_sc_right;
        s->sc_bottom = s->default_sc_bottom;
    }
}

static void ati_rage128_reg_write32(ATIRage128State *s, uint32_t base,
                                    uint32_t val)
{
    /*
     * Diagnostic: the 2D source/destination context. Mac OS X programs
     * essentially all of this through the CCE (a whole window drag issues
     * ~180 MMIO register writes, nearly all of them CUR_OFFSET), and PM4
     * type-0 packets reach the register file through this function rather
     * than through the MMIO ops -- so the MMIO trace never sees them.
     * Tracing here catches both paths.
     */
    switch (base) {
    case R128_SRC_OFFSET:
    case R128_SRC_PITCH:
    case R128_SRC_PITCH_OFFSET:
    case R128_DST_OFFSET:
    case R128_DST_PITCH:
    case R128_DST_PITCH_OFFSET:
    case R128_DST_PITCH_OFFSET_C:
    case R128_DEFAULT_OFFSET:
    case R128_DEFAULT_PITCH:
    case R128_DP_GUI_MASTER_CNTL:
    case R128_DP_GUI_MASTER_CNTL_C:
        trace_ati_rage128_ctx_write(ati_rage128_reg_name(base), base, val);
        break;
    default:
        break;
    }

    switch (base) {
    case R128_MM_INDEX:
        s->regs[base >> 2] = val;
        break;
    case R128_MM_DATA:
        if ((s->regs[R128_MM_INDEX >> 2] & 0x3ffc) != R128_MM_DATA) {
            ati_rage128_reg_write32(s, s->regs[R128_MM_INDEX >> 2] & 0x3ffc,
                                    val);
        }
        break;
    case R128_CLOCK_CNTL_INDEX:
        s->regs[base >> 2] = val;
        break;
    case R128_CLOCK_CNTL_DATA:
    {
        unsigned idx = s->regs[R128_CLOCK_CNTL_INDEX >> 2] &
                       R128_PLL_ADDR_MASK;

        if (s->regs[R128_CLOCK_CNTL_INDEX >> 2] & R128_PLL_WR_EN) {
            s->plls[idx] = val;
            trace_ati_rage128_pll_write(idx, val);
        }
        break;
    }
    case R128_GEN_INT_CNTL:
        s->regs[base >> 2] = val;
        ati_rage128_update_irq(s);
        break;
    case R128_GEN_INT_STATUS:
        /* write-1-to-acknowledge */
        s->regs[base >> 2] &= ~(val & R128_GEN_INT_ACK_MASK);
        trace_ati_rage128_int_ack(val, s->regs[base >> 2]);
        ati_rage128_update_irq(s);
        break;
    case R128_CRTC_GEN_CNTL:
        s->regs[base >> 2] = val;
        s->mode_dirty = true;
        trace_ati_rage128_mode_reg(ati_rage128_reg_name(base), val);
        ati_rage128_maybe_capture_mode(s);
        ati_rage128_cursor_update(s);   /* carries CRTC_CUR_EN */
        break;
    case R128_CRTC_EXT_CNTL:
    case R128_CRTC_H_TOTAL_DISP:
    case R128_CRTC_V_TOTAL_DISP:
    case R128_CRTC_PITCH:
        s->regs[base >> 2] = val;
        s->mode_dirty = true;
        trace_ati_rage128_mode_reg(ati_rage128_reg_name(base), val);
        ati_rage128_maybe_capture_mode(s);
        break;
    case R128_CUR_OFFSET:
    case R128_CUR_HORZ_VERT_POSN:
    case R128_CUR_HORZ_VERT_OFF:
    case R128_CUR_CLR0:
    case R128_CUR_CLR1:
        s->regs[base >> 2] = val;
        ati_rage128_cursor_update(s);
        break;
    case R128_CRTC_OFFSET:
        s->regs[base >> 2] = val & (R128_CRTC_OFFSET_MASK |
                                    R128_CRTC_OFFSET_LOCK);
        s->mode_dirty = true;
        ati_rage128_maybe_capture_mode(s);
        break;
    case R128_CRTC_STATUS:
        /* write 1 to bit 1 clears CRTC_VBLANK_SAVE */
        if (val & R128_CRTC_VBLANK_SAVE) {
            s->regs[base >> 2] &= ~(uint32_t)R128_CRTC_VBLANK_SAVE;
        }
        break;
    case R128_CRTC_V_SYNC_STRT_WID: {
        /*
         * V_SYNC_POL (bit 23) doubles as the FCode's manual DDC1
         * clock: during the GPIO_MONID DDC session it pulses this bit
         * once per SAMPLE, two samples per stream bit (its capture
         * buffer is exactly 128 bytes x 9 bits x 2 samples), so the
         * EDID bitstream advances every second rising edge. The same
         * toggles also feed the pad-3 loopback handshake in the
         * GPIO_MONID read handler.
         */
        uint32_t old = s->regs[base >> 2];

        s->regs[base >> 2] = val;
        if (((s->regs[R128_GPIO_MONID >> 2] >> 24) & 0xf) == 0xf &&
            !(old & (1u << 23)) && (val & (1u << 23))) {
            s->ddc1_half ^= 1;
            if (!s->ddc1_half) {
                s->ddc1_pos = (s->ddc1_pos + 1) % (sizeof(s->edid) * 9);
            }
        }
        break;
    }
    case R128_PALETTE_INDEX:
        s->dac_wr_index = val & 0xff;
        s->dac_rd_index = (val >> 16) & 0xff;
        break;
    case R128_PALETTE_DATA:
        s->palette[s->dac_wr_index][0] = (val >> 16) & 0xff;  /* R */
        s->palette[s->dac_wr_index][1] = (val >> 8) & 0xff;   /* G */
        s->palette[s->dac_wr_index][2] = val & 0xff;          /* B */
        s->dac_wr_index++;
        break;
    case R128_I2C_CNTL_0:
        s->regs[base >> 2] = val;
        if (val & R128_I2C_SOFT_RST) {
            s->i2c_data_len = 0;
            s->i2c_data_pos = 0;
            s->regs[base >> 2] = (val & ~(R128_I2C_SOFT_RST | R128_I2C_GO)) |
                                 R128_I2C_DONE;
        } else if (val & R128_I2C_GO) {
            ati_rage128_i2c_go(s);
        }
        break;
    case R128_I2C_DATA:
        if (s->i2c_data_len < (int)sizeof(s->i2c_data_fifo)) {
            s->i2c_data_fifo[s->i2c_data_len++] = val & 0xff;
        }
        break;
    case R128_CONFIG_CNTL:
        s->regs[base >> 2] = val;
        trace_ati_rage128_aper_endian(val & R128_APER_0_ENDIAN_MASK,
            (val >> R128_APER_1_ENDIAN_SHIFT) & R128_APER_0_ENDIAN_MASK,
            !!(val & R128_APER_REG_ENDIAN));
        break;
    case R128_GEN_RESET_CNTL:
        s->regs[base >> 2] = val;
        if (val & R128_SOFT_RESET_GUI) {
            /* engine soft reset also aborts any half-parsed command
             * stream state (matches the driver's reset-then-restart
             * expectation) */
            s->pm4_fifo.remaining = 0;
            s->pm4_rptr = 0;
            s->pm4_wptr = 0;
        }
        break;
    case R128_BM_GUI_TABLE:
        s->regs[base >> 2] = val;
        ati_rage128_bm_gui_run(s, val);
        break;
    case R128_PM4_BUFFER_OFFSET:
        s->pm4_buffer_addr = val;
        s->regs[base >> 2] = val;
        break;
    case R128_PM4_BUFFER_CNTL:
    {
        uint32_t l2qw = R128_PM4_BUFFER_SIZE_L2QW(val);
        uint32_t mode = val & R128_PM4_MODE_MASK;

        s->pm4_buffer_cntl = val;
        s->regs[base >> 2] = val;
        /*
         * Only the even "BM" modes give the primary command stream a
         * bus-master ring; the PIO modes (incl. mode 7, which Mac OS
         * X uses) deliver the primary stream through the FIFO
         * registers, handled separately below.
         */
        if ((mode == R128_PM4_192BM || mode == R128_PM4_128BM_64INDBM ||
             mode == R128_PM4_64BM_128INDBM ||
             mode == R128_PM4_64BM_64VCBM_64INDBM) && l2qw > 0) {
            s->pm4_ring_dwords = 2u << l2qw;
        } else {
            s->pm4_ring_dwords = 0;
        }
        break;
    }
    case R128_PM4_BUFFER_DL_RPTR:
        s->pm4_rptr = val & ~R128_PM4_BUFFER_DL_DONE;
        break;
    case R128_PM4_BUFFER_DL_WPTR:
        s->pm4_wptr = val & ~R128_PM4_BUFFER_DL_DONE;
        /* bit31 (DL_DONE) is a flush marker -- either way, consume */
        ati_rage128_pm4_run(s);
        break;
    case R128_PM4_IW_INDOFF:
        s->regs[base >> 2] = val;
        break;
    case R128_PM4_IW_INDSIZE:
        s->regs[base >> 2] = val;
        ati_rage128_pm4_indirect(s, s->regs[R128_PM4_IW_INDOFF >> 2], val);
        break;
    case R128_PM4_MICROCODE_ADDR:
        s->pm4_ucode_waddr = val & (R128_PM4_MICROCODE_WORDS - 1);
        break;
    case R128_PM4_MICROCODE_RADDR:
        s->pm4_ucode_raddr = val & (R128_PM4_MICROCODE_WORDS - 1);
        break;
    case R128_PM4_MICROCODE_DATAH:
        s->pm4_microcode[s->pm4_ucode_waddr][0] = val;
        break;
    case R128_PM4_MICROCODE_DATAL:
        s->pm4_microcode[s->pm4_ucode_waddr][1] = val;
        s->pm4_ucode_waddr = (s->pm4_ucode_waddr + 1) &
                             (R128_PM4_MICROCODE_WORDS - 1);
        break;
    case R128_PM4_MICRO_CNTL:
    case R128_PM4_BUFFER_WM_CNTL:
    case R128_PM4_BUFFER_DL_RPTR_ADDR:
        s->regs[base >> 2] = val;
        break;
    case R128_PM4_FIFO_DATA_EVEN:
    case R128_PM4_FIFO_DATA_ODD:
        ati_rage128_pm4_fifo_push(s, val);
        break;
    case R128_DST_OFFSET:
        s->dst_offset_reg = val & 0xfffffff0;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DST_PITCH:
        s->dst_pitch_reg = val & 0x3fff;
        s->dst_tile_reg = (val >> 16) & 1;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DST_WIDTH:
        s->dst_width = val & 0x3fff;
        ati_rage128_2d_blt(s);
        break;
    case R128_DST_HEIGHT:
        s->dst_height = val & 0x3fff;
        break;
    case R128_SRC_X:
        s->src_x = val & 0x3fff;
        break;
    case R128_SRC_Y:
        s->src_y = val & 0x3fff;
        break;
    case R128_DST_X:
        s->dst_x = val & 0x3fff;
        break;
    case R128_DST_Y:
        s->dst_y = val & 0x3fff;
        break;
    case R128_SRC_PITCH_OFFSET:
        s->src_offset_reg = (val & 0x1fffff) << 5;
        s->src_pitch_reg = (val & 0x7fe00000) >> 21;
        s->src_tile_reg = val >> 31;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DST_PITCH_OFFSET:
    case R128_DST_PITCH_OFFSET_C:
        s->dst_offset_reg = (val & 0x1fffff) << 5;
        s->dst_pitch_reg = (val & 0x7fe00000) >> 21;
        s->dst_tile_reg = val >> 31;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SRC_Y_X:
        s->src_x = val & 0x3fff;
        s->src_y = (val >> 16) & 0x3fff;
        break;
    case R128_DST_Y_X:
        s->dst_x = val & 0x3fff;
        s->dst_y = (val >> 16) & 0x3fff;
        break;
    case R128_DST_HEIGHT_WIDTH:
        s->dst_width = val & 0x3fff;
        s->dst_height = (val >> 16) & 0x3fff;
        ati_rage128_2d_blt(s);
        break;
    case R128_DP_GUI_MASTER_CNTL:
    case R128_DP_GUI_MASTER_CNTL_C:
        s->dp_gui_master_cntl = val & 0xf800000f;
        /*
         * Only the aliased fields come from GUI_MASTER_CNTL. Rebuilding
         * the whole of DP_DATATYPE here wiped HOST_BIG_ENDIAN_EN, which
         * the driver programs separately (and, on this card, through a
         * PM4 type-0 packet rather than an MMIO write) -- and since every
         * PAINT/PAINT_MULTI/HOSTDATA_BLT packet carries its own GMC
         * dword, the bit was destroyed again before every single blit.
         * Host-supplied pixels then went into VRAM byte-reversed: on a
         * 32bpp desktop the unused byte landed in the blue slot, so
         * anything white came out yellow and red/green swapped places.
         */
        s->dp_datatype = (s->dp_datatype & ~R128_DP_DATATYPE_GMC_ALIAS) |
                         (val & 0x0f00) >> 8 | (val & 0x30f0) << 4 |
                         (val & 0x4000) << 16;
        s->dp_mix = (val & R128_GMC_ROP3_MASK) | (val & 0x7000000) >> 16;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DST_WIDTH_X:
        s->dst_x = val & 0x3fff;
        s->dst_width = (val >> 16) & 0x3fff;
        ati_rage128_2d_blt(s);
        break;
    case R128_SRC_X_Y:
        s->src_y = val & 0x3fff;
        s->src_x = (val >> 16) & 0x3fff;
        break;
    case R128_DST_X_Y:
        s->dst_y = val & 0x3fff;
        s->dst_x = (val >> 16) & 0x3fff;
        break;
    case R128_DST_WIDTH_HEIGHT:
        s->dst_height = val & 0x3fff;
        s->dst_width = (val >> 16) & 0x3fff;
        ati_rage128_2d_blt(s);
        break;
    case R128_DST_HEIGHT_Y:
        s->dst_y = val & 0x3fff;
        s->dst_height = (val >> 16) & 0x3fff;
        break;
    case R128_SRC_OFFSET:
        s->src_offset_reg = val & 0xfffffff0;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SRC_PITCH:
        s->src_pitch_reg = val & 0x3fff;
        s->src_tile_reg = (val >> 16) & 1;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DP_BRUSH_BKGD_CLR:
        s->dp_brush_bkgd_clr = val;
        break;
    case R128_DP_BRUSH_FRGD_CLR:
    case R128_CONSTANT_COLOR_C:
        s->dp_brush_frgd_clr = val;
        break;
    case R128_DP_CNTL:
        s->dp_cntl = val;
        break;
    case R128_DP_SRC_FRGD_CLR:
        s->dp_src_frgd_clr = val;
        break;
    case R128_DP_SRC_BKGD_CLR:
        s->dp_src_bkgd_clr = val;
        break;
    case R128_DP_DATATYPE:
        s->dp_datatype = val & 0xe0070f0f;
        trace_ati_rage128_datatype(val, !!(val & R128_HOST_BIG_ENDIAN_EN));
        break;
    case R128_DP_MIX:
        s->dp_mix = val & 0x00ff0700;
        break;
    case R128_DP_WRITE_MASK:
    case R128_PLANE_3D_MASK_C:
        s->dp_write_mask = val;
        break;
    case R128_DEFAULT_OFFSET:
        s->default_offset = val & 0xfffffff0;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DEFAULT_PITCH:
        s->default_pitch = val & 0x3fff;
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_DEFAULT_SC_BOTTOM_RIGHT:
        s->default_sc_right = sextract32(val, 0, 14);
        s->default_sc_bottom = sextract32(val, 16, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_TOP_LEFT:
    case R128_SC_TOP_LEFT_C:
        s->sc_left_reg = sextract32(val, 0, 14);
        s->sc_top_reg = sextract32(val, 16, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_LEFT:
        s->sc_left_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_TOP:
        s->sc_top_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_BOTTOM_RIGHT:
    case R128_SC_BOTTOM_RIGHT_C:
        s->sc_right_reg = sextract32(val, 0, 14);
        s->sc_bottom_reg = sextract32(val, 16, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_RIGHT:
        s->sc_right_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SC_BOTTOM:
        s->sc_bottom_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SRC_SC_BOTTOM_RIGHT:
        s->src_sc_right_reg = sextract32(val, 0, 14);
        s->src_sc_bottom_reg = sextract32(val, 16, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SRC_SC_RIGHT:
        s->src_sc_right_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_SRC_SC_BOTTOM:
        s->src_sc_bottom_reg = sextract32(val, 0, 14);
        ati_rage128_resolve_gui_context(s);
        break;
    case R128_HOST_DATA0:
    case R128_HOST_DATA1:
    case R128_HOST_DATA2:
    case R128_HOST_DATA3:
    case R128_HOST_DATA4:
    case R128_HOST_DATA5:
    case R128_HOST_DATA6:
    case R128_HOST_DATA7:
    case R128_HOST_DATA_LAST:
        if (!s->host_data_active) {
            break;
        }
        s->host_data_acc[s->host_data_next++] = val;
        if (base == R128_HOST_DATA_LAST) {
            ati_rage128_host_data_flush(s);
            s->host_data_active = false;
            s->host_data_next = 0;
        } else if (s->host_data_next >= 4) {
            ati_rage128_host_data_flush(s);
            s->host_data_next = 0;
        }
        break;
    case R128_CFG_MIRROR_BASE ... R128_CFG_MIRROR_END:
        /* read-only mirror of PCI config space */
        break;
    case R128_GPIO_MONID: {
        /*
         * The FCode's DDC session (MASK nibble 0xf, distinct from the
         * Apple-sense probes' 0x7): SDA on pad 0, SCL on pad 1, open
         * drain -- a pad drives its A-bit level while its EN bit is
         * set, floats high otherwise. Every edge feeds the DDC2
         * bit-bang core, whose resulting SDA level the read handler
         * feeds back on a floating pad 0.
         */
        uint32_t oldmask = (s->regs[base >> 2] >> 24) & 0xf;

        s->regs[base >> 2] = val;
        if (((val >> 24) & 0xf) == 0xf) {
            uint32_t en = (val >> 16) & 0xf;
            uint32_t a = val & 0xf;
            int scl = (en & 2) ? !!(a & 2) : 1;
            int sda = (en & 1) ? !!(a & 1) : 1;

            if (oldmask != 0xf) {
                /* session entry rewinds the DDC1 stream to byte 0 */
                s->ddc1_pos = 0;
                s->ddc1_half = 0;
            }
            bitbang_i2c_set(&s->monid_i2c, BITBANG_I2C_SCL, scl);
            s->monid_sda = bitbang_i2c_set(&s->monid_i2c,
                                           BITBANG_I2C_SDA, sda);
            trace_ati_rage128_monid_wr(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                       val, scl, sda, s->monid_sda);
        }
        break;
    }
    case R128_GPIO_MONIDB:
        s->regs[base >> 2] = val;
        trace_ati_rage128_monidb_wr(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                    val);
        break;
    case R128_AMCGPIO_A_MIR:
    case R128_AMCGPIO_EN_MIR:
    case R128_AMCGPIO_MASK_MIR:
        s->regs[base >> 2] = val;
        trace_ati_rage128_amcgpio(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  base == R128_AMCGPIO_A_MIR ? 'A' :
                                  base == R128_AMCGPIO_EN_MIR ? 'E' : 'M',
                                  s->regs[R128_AMCGPIO_A_MIR >> 2],
                                  s->regs[R128_AMCGPIO_EN_MIR >> 2], val);
        break;
    default:
        s->regs[base >> 2] = val;
        break;
    }
}

/*
 * GUI bus master: walk the descriptor table at the given guest-physical
 * address. Each 12-byte, little-endian entry is {dest register offset,
 * source system-memory address, control}; control's low 16 bits are a
 * byte count and bit 31 is END_OF_LIST (both inferred from the smoke
 * test -- see the comment on R128_BM_GUI_TABLE in ati_rage128_regs.h).
 * The transfer completes synchronously, one dword at a time through the
 * normal register-write path so a descriptor can target any register
 * exactly as if the driver had written it directly via MM_INDEX/DATA.
 */
static void ati_rage128_bm_gui_run(ATIRage128State *s, uint32_t table)
{
    PCIDevice *pci = PCI_DEVICE(s);
    dma_addr_t desc = table;
    int entry;

    for (entry = 0; entry < 4096; entry++) {
        uint32_t d[3];
        uint32_t reg_off, sysaddr, ctrl, count;

        if (pci_dma_read(pci, desc, d, sizeof(d)) != MEMTX_OK) {
            break;
        }
        reg_off = le32_to_cpu(d[0]);
        sysaddr = le32_to_cpu(d[1]);
        ctrl = le32_to_cpu(d[2]);
        count = ctrl & 0xffff;
        trace_ati_rage128_bm_desc(reg_off, sysaddr, ctrl);

        while (count >= 4) {
            uint32_t word;

            if (pci_dma_read(pci, sysaddr, &word, 4) != MEMTX_OK) {
                break;
            }
            ati_rage128_reg_write32(s, reg_off & 0x3ffc, le32_to_cpu(word));
            sysaddr += 4;
            reg_off += 4;
            count -= 4;
        }

        if (ctrl & (1u << 31)) {
            break;
        }
        desc += 12;
    }

    s->regs[R128_GEN_INT_STATUS >> 2] |= R128_BUSMASTER_EOL_INT;
    ati_rage128_update_irq(s);
}

/*
 * Read one dword from the PM4 ring at the current read pointer and
 * advance it, wrapping at the ring size. The ring lives in VRAM (see
 * the comment on R128_PM4_BUFFER_OFFSET) so this is a direct pointer
 * read, not a DMA -- consistent with every other VRAM access in this
 * file.
 */
/*
 * Read one little-endian dword from card address space: local VRAM
 * below ATI_RAGE128_VRAM_SIZE, otherwise the 32MB GART-translated
 * "VM" window (see the R128_PCIGART_TABLE_ENTRIES comment in
 * ati_rage128_regs.h). Command streams are little-endian regardless
 * of guest CPU endianness -- big-endian Mac drivers byte-swap their
 * command buffers on the way out, exactly like the Linux driver's
 * cpu_to_le32() (verified live: PIO-FIFO dwords, which arrive
 * pre-swapped through the LE register aperture, decode with the
 * identical packet layout).
 *
 * The GART window's card-space base isn't modeled explicitly: the
 * window is exactly 32MB and its base is 32MB-aligned, so masking the
 * page index into the 8192-entry table is base-agnostic.
 */
static uint32_t ati_rage128_card_read32(ATIRage128State *s, uint32_t addr,
                                        bool gart)
{
    if (!gart && addr + 4 <= ATI_RAGE128_VRAM_SIZE) {
        uint8_t *vram = memory_region_get_ram_ptr(&s->vram);

        return ldl_le_p(vram + addr);
    } else {
        uint32_t gart_base = s->regs[R128_PCI_GART_PAGE >> 2] & ~0xfffu;
        uint32_t idx = (addr >> 12) & (R128_PCIGART_TABLE_ENTRIES - 1);
        uint32_t entry, page, val;

        if (!gart_base) {
            return 0;
        }
        pci_dma_read(PCI_DEVICE(s), gart_base + idx * 4, &entry,
                     sizeof(entry));
        page = le32_to_cpu(entry) & ~0xfffu;
        if (!page) {
            return 0;
        }
        pci_dma_read(PCI_DEVICE(s), page | (addr & 0xfff), &val,
                     sizeof(val));
        return le32_to_cpu(val);
    }
}

static uint32_t ati_rage128_pm4_read_ring(ATIRage128State *s)
{
    bool gart = s->pm4_buffer_addr & R128_AGP_OFFSET_FLAG;
    uint32_t base = s->pm4_buffer_addr & ~R128_AGP_OFFSET_FLAG;
    uint32_t val;

    val = ati_rage128_card_read32(s, base + s->pm4_rptr * 4, gart);
    s->pm4_rptr = (s->pm4_rptr + 1) & (s->pm4_ring_dwords - 1);
    return val;
}

/*
 * Consume PM4 ring entries from the read pointer up to the write
 * pointer, synchronously (matching the BM engine's own synchronous
 * model). Packet format: see the comment on R128_PM4_BUFFER_OFFSET in
 * ati_rage128_regs.h.
 */
static void ati_rage128_pm4_run(ATIRage128State *s)
{
    int guard;

    if (!s->pm4_ring_dwords) {
        return;
    }

    for (guard = 0; guard < 100000 && s->pm4_rptr != s->pm4_wptr; guard++) {
        uint32_t header = ati_rage128_pm4_read_ring(s);
        uint32_t type = R128_PM4_PACKET_TYPE(header);
        uint32_t count = R128_PM4_PACKET_COUNT(header);
        uint32_t i;

        switch (type) {
        case 0:
        {
            uint32_t reg = R128_PM4_PACKET0_REG(header);
            bool one_reg = R128_PM4_PACKET0_ONE_REG(header);

            for (i = 0; i < count; i++) {
                uint32_t data = ati_rage128_pm4_read_ring(s);

                ati_rage128_reg_write32(s, reg & 0x3ffc, data);
                if (!one_reg) {
                    reg += 4;
                }
            }
            break;
        }
        case 1:
        {
            uint32_t reg1 = R128_PM4_PACKET1_REG1(header);
            uint32_t reg2 = R128_PM4_PACKET1_REG2(header);
            uint32_t data1 = ati_rage128_pm4_read_ring(s);
            uint32_t data2 = ati_rage128_pm4_read_ring(s);

            ati_rage128_reg_write32(s, reg1 & 0x3ffc, data1);
            ati_rage128_reg_write32(s, reg2 & 0x3ffc, data2);
            break;
        }
        case 2:
            /* NOP/padding, no data words */
            break;
        case 3:
        {
            uint32_t opcode = R128_PM4_PACKET3_OPCODE(header);

            switch (opcode) {
            case R128_PM4_OPCODE_PAINT:
                if (count >= 4) {
                    /*
                     * The drawing context and colour travel inside the
                     * packet, and the rectangle is given as two CORNERS,
                     * not as position + size. Decoding the corners as
                     * DST_Y_X/DST_HEIGHT_WIDTH (the two-dword form
                     * below) yielded wildly out-of-range rectangles --
                     * captured live: a 233x144 window panel came out
                     * as 7645x221 at (13040,14032) -- so every fill
                     * fell outside the scissors and vanished, leaving
                     * only text and lines on screen.
                     *
                     * When the context has GMC_LD_BRUSH_Y_X set the
                     * packet is three dwords longer, carrying the 8x8
                     * brush pattern and its origin inline before the
                     * corners. See the streamed parser's copy of this
                     * case for the capture that established it.
                     */
                    uint32_t gmc = ati_rage128_pm4_read_ring(s);
                    uint32_t color = ati_rage128_pm4_read_ring(s);
                    bool ld_brush = (gmc & R128_GMC_LD_BRUSH_Y_X) &&
                                    count >= 7;
                    uint32_t tl, br;
                    int x1, y1, x2, y2;

                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, gmc);
                    ati_rage128_reg_write32(s, R128_DP_BRUSH_FRGD_CLR, color);
                    if (ld_brush) {
                        ati_rage128_reg_write32(s, R128_BRUSH_DATA0,
                                        ati_rage128_pm4_read_ring(s));
                        ati_rage128_reg_write32(s, R128_BRUSH_DATA0 + 4,
                                        ati_rage128_pm4_read_ring(s));
                        ati_rage128_reg_write32(s, R128_BRUSH_Y_X,
                                        ati_rage128_pm4_read_ring(s));
                    }
                    tl = ati_rage128_pm4_read_ring(s);
                    br = ati_rage128_pm4_read_ring(s);
                    x1 = tl & 0x3fff; y1 = (tl >> 16) & 0x3fff;
                    x2 = br & 0x3fff; y2 = (br >> 16) & 0x3fff;
                    for (i = ld_brush ? 7 : 4; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                    if (x2 > x1 && y2 > y1) {
                        s->dst_x = x1;
                        s->dst_y = y1;
                        s->dst_width = x2 - x1;
                        s->dst_height = y2 - y1;
                        trace_ati_rage128_paint_multi(0, tl, br, s->dst_x,
                                                      s->dst_y, s->dst_width,
                                                      s->dst_height);
                        ati_rage128_2d_blt(s);
                    }
                } else if (count >= 2) {
                    uint32_t dst_y_x = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_h_w = ati_rage128_pm4_read_ring(s);

                    s->dst_x = dst_y_x & 0x3fff;
                    s->dst_y = (dst_y_x >> 16) & 0x3fff;
                    s->dst_width = dst_h_w & 0x3fff;
                    s->dst_height = (dst_h_w >> 16) & 0x3fff;
                    for (i = 2; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                    ati_rage128_2d_blt(s);
                } else {
                    for (i = 0; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                }
                break;
            case R128_PM4_OPCODE_PAINT_MULTI:
            {
                /*
                 * Context and colour, then one or more rectangles as
                 * (DST_X_Y, DST_WIDTH_HEIGHT) pairs -- note the field
                 * order differs from PAINT above, which carries two
                 * corners instead: here X and WIDTH sit in the HIGH
                 * half and Y and HEIGHT in the low one, matching the
                 * registers of the same names. Established from a live
                 * capture of Mac OS drawing a dialog: successive
                 * packets walk y = 0,1,2,3 with widths 5,3,2,1 at
                 * x = 0 and x = 1019..1023 on a 1024-wide screen --
                 * a window's rounded corners, pixel row by pixel row.
                 * Reading the halves the other way round made every
                 * one of them zero-sized, which is why button frames
                 * never appeared.
                 */
                uint32_t gmc, color;

                if (count < 4) {
                    for (i = 0; i < (int)count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                    break;
                }
                gmc = ati_rage128_pm4_read_ring(s);
                ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, gmc);
                i = 1;
                /*
                 * A context with DST_PITCH_OFFSET_CNTL set carries the
                 * destination pitch/offset dword before the colour
                 * (Linux's r128 DRM clear packet); Mac OS 9's context
                 * (0x72f036d0) has the bit clear and goes straight to
                 * the colour.
                 */
                if ((gmc & R128_GMC_DST_PITCH_OFFSET_CNTL) &&
                    i + 1 < (int)count) {
                    ati_rage128_reg_write32(s, R128_DST_PITCH_OFFSET,
                                            ati_rage128_pm4_read_ring(s));
                    i++;
                }
                color = ati_rage128_pm4_read_ring(s);
                ati_rage128_reg_write32(s, R128_DP_BRUSH_FRGD_CLR, color);
                i++;
                for (; i + 1 < (int)count; i += 2) {
                    uint32_t dst_x_y = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_w_h = ati_rage128_pm4_read_ring(s);

                    s->dst_y = dst_x_y & 0x3fff;
                    s->dst_x = (dst_x_y >> 16) & 0x3fff;
                    s->dst_height = dst_w_h & 0x3fff;
                    s->dst_width = (dst_w_h >> 16) & 0x3fff;
                    trace_ati_rage128_paint_multi(i, dst_x_y, dst_w_h,
                                                  s->dst_x, s->dst_y,
                                                  s->dst_width,
                                                  s->dst_height);
                    if (s->dst_width && s->dst_height) {
                        ati_rage128_2d_blt(s);
                    }
                }
                for (; i < (int)count; i++) {
                    ati_rage128_pm4_read_ring(s);
                }
                break;
            }
            case R128_PM4_OPCODE_BITBLT_MULTI:
                /*
                 * The save-under half of a window drag: a context
                 * dword, the pitch/offset dwords the context itself
                 * announces, and then a RUN of three-dword rectangles
                 * -- the MULTI is not decoration. See
                 * ati_rage128_regs.h for the layout. iTunes sends nine
                 * rectangles in one 29-dword packet, tiling a
                 * rounded-corner window, so stopping after the first
                 * copied a 1-pixel strip and dropped the 600x390 body:
                 * chrome came out right, contents did not.
                 *
                 * How many pitch/offset dwords follow the context is
                 * decided by that context's own SRC/DST_PITCH_OFFSET_CNTL
                 * bits, in SRC-then-DST order (Linux's r128 DRM builds
                 * its swap packet exactly so). Mac OS 9 sets only the
                 * SRC bit: one dword, the screen, with DEFAULT_* as the
                 * offscreen destination. Mac OS X 10.4 sets BOTH for
                 * its 16x16 pointer save/restore: source, destination,
                 * then the rectangle. Reading that as one dword plus a
                 * rectangle turned the restore into a copy of
                 * (pointer x) by (pointer y) pixels from a 64-pixel-wide
                 * sprite buffer to the screen's top-left corner -- the
                 * "corruption growing from the top-left towards the
                 * mouse" on OS X 10.2/10.4.
                 */
                if (count >= R128_BITBLT_MULTI_MIN_DWORDS) {
                    uint32_t gmc = ati_rage128_pm4_read_ring(s);

                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, gmc);
                    i = 1;
                    if ((gmc & R128_GMC_SRC_PITCH_OFFSET_CNTL) &&
                        i < (int)count) {
                        ati_rage128_reg_write32(s, R128_SRC_PITCH_OFFSET,
                                                ati_rage128_pm4_read_ring(s));
                        i++;
                    }
                    if ((gmc & R128_GMC_DST_PITCH_OFFSET_CNTL) &&
                        i < (int)count) {
                        ati_rage128_reg_write32(s, R128_DST_PITCH_OFFSET,
                                                ati_rage128_pm4_read_ring(s));
                        i++;
                    }
                    for (; i + 3 <= (int)count; i += 3) {
                        uint32_t src_x_y = ati_rage128_pm4_read_ring(s);
                        uint32_t dst_x_y = ati_rage128_pm4_read_ring(s);
                        uint32_t dst_w_h = ati_rage128_pm4_read_ring(s);

                        s->src_y = src_x_y & 0x3fff;
                        s->src_x = (src_x_y >> 16) & 0x3fff;
                        s->dst_y = dst_x_y & 0x3fff;
                        s->dst_x = (dst_x_y >> 16) & 0x3fff;
                        s->dst_height = dst_w_h & 0x3fff;
                        s->dst_width = (dst_w_h >> 16) & 0x3fff;
                        ati_rage128_2d_blt(s);
                    }
                    for (; i < (int)count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                } else {
                    for (i = 0; i < (int)count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                }
                break;

            case R128_PM4_OPCODE_BITBLT:
                /*
                 * Four dwords: context, SRC_X_Y, DST_X_Y,
                 * DST_WIDTH_HEIGHT -- and, like the registers of those
                 * names (and unlike PAINT's corners), X and WIDTH sit in
                 * the HIGH half with Y and HEIGHT in the low one.
                 *
                 * Established from a live capture of Mac OS dragging a
                 * window between screens, where each move issues three
                 * blits that must tile the window exactly; only this
                 * reading makes them do so:
                 *   src(175,162) -> dst(195,156) 507x2
                 *   src(175,164) -> dst(195,158) 508x258   162+2   = 164
                 *   src(177,422) -> dst(197,416) 506x1     164+258 = 422
                 * (the narrower first and last rows are the window's
                 * rounded corners). Reading three dwords from index 0
                 * took the CONTEXT dword for the source point, so every
                 * window copy fetched its pixels from a nonsense
                 * position -- corruption that then travelled with the
                 * window, since this is the path that moves its bits.
                 */
                if (count >= 4) {
                    uint32_t gmc = ati_rage128_pm4_read_ring(s);
                    uint32_t src_x_y = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_x_y = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_w_h = ati_rage128_pm4_read_ring(s);

                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, gmc);
                    s->src_y = src_x_y & 0x3fff;
                    s->src_x = (src_x_y >> 16) & 0x3fff;
                    s->dst_y = dst_x_y & 0x3fff;
                    s->dst_x = (dst_x_y >> 16) & 0x3fff;
                    s->dst_height = dst_w_h & 0x3fff;
                    s->dst_width = (dst_w_h >> 16) & 0x3fff;
                    for (i = 4; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                    ati_rage128_2d_blt(s);
                } else {
                    for (i = 0; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                }
                break;
            case R128_PM4_OPCODE_SCALING:
                if (count >= R128_SCALE_PKT_DWORDS) {
                    uint32_t pkt[R128_SCALE_PKT_DWORDS];

                    for (i = 0; i < R128_SCALE_PKT_DWORDS; i++) {
                        pkt[i] = ati_rage128_pm4_read_ring(s);
                    }
                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                            pkt[R128_SCALE_PKT_GMC]);
                    ati_rage128_reg_write32(s, R128_SC_TOP_LEFT,
                                            pkt[R128_SCALE_PKT_SC_TL]);
                    ati_rage128_reg_write32(s, R128_SC_BOTTOM_RIGHT,
                                            pkt[R128_SCALE_PKT_SC_BR]);
                    ati_rage128_2d_scale(s, pkt);
                }
                for (i = R128_SCALE_PKT_DWORDS; i < count; i++) {
                    ati_rage128_pm4_read_ring(s);
                }
                break;
            case R128_PM4_OPCODE_HOSTDATA_BLT:
                if (count >= 8) {
                    /*
                     * Eight-dword header, then the pixel dwords. Only
                     * three fields are needed: [0] the drawing context
                     * (rop 0xCC, SRCCOPY, in every captured packet),
                     * [5] DST_Y_X and [6] DST_HEIGHT_WIDTH; [7] is the
                     * pixel dword count, which equals width x height in
                     * every capture (72x14 -> 0x3f0, 80x12 -> 0x3c0),
                     * and that identity is what pins this layout down.
                     * [1]-[4] carry clip/state the driver has already
                     * programmed through registers, so they are
                     * skipped. Reading [0]/[1] as the rectangle -- the
                     * short form below -- put every glyph and icon at a
                     * nonsense position, which is why text and icons
                     * were missing on this card.
                     */
                    uint32_t hdr[8];
                    int nhdr;

                    for (i = 0; i < 8; i++) {
                        hdr[i] = ati_rage128_pm4_read_ring(s);
                    }
                    /*
                     * The last header dword is the pixel-dword count,
                     * so it must equal what is left of the packet --
                     * that identity tells the two known header lengths
                     * apart without guessing: Linux's r128 driver emits
                     * seven (context, pitch/offset, write mask, clip,
                     * position, size, count) while the Mac driver emits
                     * eight, with one extra dword before the position.
                     */
                    nhdr = (hdr[7] == count - 8) ? 8 :
                           (hdr[6] == count - 7) ? 7 : 8;
                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                            hdr[0]);
                    /*
                     * hdr[1]/hdr[2] are the destination scissors, and
                     * they are not decoration: the driver pads the blit
                     * width up to a 4-pixel boundary and expects the
                     * clip to discard the surplus. They must be
                     * programmed AFTER the context dword, since a GMC
                     * write with DST_CLIPPING clear resets them.
                     */
                    if (nhdr == 8) {
                        ati_rage128_reg_write32(s, R128_SC_TOP_LEFT, hdr[1]);
                        ati_rage128_reg_write32(s, R128_SC_BOTTOM_RIGHT,
                                                hdr[2]);
                    }
                    s->dst_x = hdr[nhdr - 3] & 0x3fff;
                    s->dst_y = (hdr[nhdr - 3] >> 16) & 0x3fff;
                    s->dst_width = hdr[nhdr - 2] & 0x3fff;
                    s->dst_height = (hdr[nhdr - 2] >> 16) & 0x3fff;
                    if (nhdr == 7) {
                        /* the 8th dword we already read is pixel data */
                        s->host_data_acc[0] = hdr[7];
                    }
                    ati_rage128_2d_blt(s); /* enters host-data mode */
                    if (nhdr == 7 && s->host_data_active) {
                        s->host_data_next = 1;
                    }
                    for (i = 8; i < count; i++) {
                        uint32_t hdata = ati_rage128_pm4_read_ring(s);

                        if (s->host_data_active) {
                            s->host_data_acc[s->host_data_next++] = hdata;
                            if (s->host_data_next >= 4) {
                                ati_rage128_host_data_flush(s);
                                s->host_data_next = 0;
                            }
                        }
                    }
                    if (s->host_data_active) {
                        ati_rage128_host_data_flush(s);
                        s->host_data_active = false;
                        s->host_data_next = 0;
                    }
                } else if (count >= 2) {
                    uint32_t dst_y_x = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_h_w = ati_rage128_pm4_read_ring(s);

                    s->dst_x = dst_y_x & 0x3fff;
                    s->dst_y = (dst_y_x >> 16) & 0x3fff;
                    s->dst_width = dst_h_w & 0x3fff;
                    s->dst_height = (dst_h_w >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s); /* enters host-data mode */
                    for (i = 2; i < count; i++) {
                        uint32_t hdata = ati_rage128_pm4_read_ring(s);

                        if (s->host_data_active) {
                            s->host_data_acc[s->host_data_next++] = hdata;
                            if (s->host_data_next >= 4) {
                                ati_rage128_host_data_flush(s);
                                s->host_data_next = 0;
                            }
                        }
                    }
                    if (s->host_data_active) {
                        ati_rage128_host_data_flush(s);
                        s->host_data_active = false;
                        s->host_data_next = 0;
                    }
                } else {
                    for (i = 0; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                }
                break;
            default:
                trace_ati_rage128_pm4_unimp(opcode, count);
                for (i = 0; i < count; i++) {
                    ati_rage128_pm4_read_ring(s);
                }
                break;
            }
            break;
        }
        }
    }
}

/*
 * PIO alternative to the ring above: the real driver actually pushes
 * its command stream straight through PM4_FIFO_DATA_EVEN/ODD (see the
 * comment on those in ati_rage128_regs.h), one dword per write. Same
 * packet format as the ring, just delivered by MMIO write instead of
 * being pulled from VRAM, so the state (in-flight packet type/count/
 * running register) has to live across calls instead of a loop index.
 */
static void ati_rage128_pm4_parse(ATIRage128State *s,
                                  ATIRage128PM4Parser *p, uint32_t val)
{
    if (p->remaining == 0) {
        /* val is a new packet header */
        p->type = R128_PM4_PACKET_TYPE(val);

        switch (p->type) {
        case 0:
            p->reg = R128_PM4_PACKET0_REG(val);
            p->one_reg = R128_PM4_PACKET0_ONE_REG(val);
            p->remaining = R128_PM4_PACKET_COUNT(val);
            break;
        case 1:
            p->p1_reg1 = R128_PM4_PACKET1_REG1(val);
            p->p1_reg2 = R128_PM4_PACKET1_REG2(val);
            p->remaining = 2;
            break;
        case 2:
            /* NOP/padding, no data words */
            break;
        case 3:
            p->remaining = R128_PM4_PACKET_COUNT(val);
            p->p3_opcode = R128_PM4_PACKET3_OPCODE(val);
            p->p3_param_idx = 0;
            p->p3_total = p->remaining;
            trace_ati_rage128_pm4_p3_hdr(p->p3_opcode, p->remaining);
            if (p->p3_opcode != R128_PM4_OPCODE_PAINT &&
                p->p3_opcode != R128_PM4_OPCODE_PAINT_MULTI &&
                p->p3_opcode != R128_PM4_OPCODE_BITBLT &&
                p->p3_opcode != R128_PM4_OPCODE_BITBLT_MULTI &&
                p->p3_opcode != R128_PM4_OPCODE_HOSTDATA_BLT &&
                p->p3_opcode != R128_PM4_OPCODE_SCALING) {
                trace_ati_rage128_pm4_unimp(p->p3_opcode, p->remaining);
            }
            break;
        }
        return;
    }

    /* val is a data dword for the packet currently in flight */
    switch (p->type) {
    case 0:
        ati_rage128_reg_write32(s, p->reg & 0x3ffc, val);
        if (!p->one_reg) {
            p->reg += 4;
        }
        break;
    case 1:
        if (p->remaining == 2) {
            ati_rage128_reg_write32(s, p->p1_reg1 & 0x3ffc, val);
        } else {
            ati_rage128_reg_write32(s, p->p1_reg2 & 0x3ffc, val);
        }
        break;
    case 3:
        trace_ati_rage128_p3_payload(p->p3_opcode, p->p3_param_idx, val);
        switch (p->p3_opcode) {
        case R128_PM4_OPCODE_PAINT:
        {
            /*
             * Two forms, told apart by GMC_LD_BRUSH_Y_X (bit 31 of the
             * context dword, which is always the first payload dword):
             *
             *   clear, 4 dwords: context, colour, top-left corner,
             *                    bottom-right corner
             *   set,   7 dwords: context, colour, BRUSH_DATA0,
             *                    BRUSH_DATA1, BRUSH_Y_X, top-left,
             *                    bottom-right
             *
             * "Load brush Y/X" means the packet carries the pattern
             * inline instead of the driver pre-loading the BRUSH_DATA
             * registers (which is how Linux's DRM r128 does it, via a
             * type-0 write to BRUSH_DATA0). Mac OS uses the inline form
             * for every marquee segment: 4668 consecutive packets in one
             * live capture, all
             *   f2550610 00ffffff aa55aa55 aa55aa55 00000000 <tl> <br>
             * i.e. DSTINVERT through a 50% checkerboard -- marching
             * ants. Reading only four dwords took BRUSH_DATA0/1 for the
             * two corners, which decode to a degenerate rectangle, so
             * every segment was silently dropped and no selection
             * rectangle was ever drawn.
             */
            unsigned want = (p->p3_params[0] & R128_GMC_LD_BRUSH_Y_X) &&
                            p->p3_total >= 7 ? 7 : 4;

            if (p->p3_param_idx < 7) {
                p->p3_params[p->p3_param_idx++] = val;
            }
            if (p->p3_total >= 4) {
                if (p->p3_param_idx == want) {
                    uint32_t tl = p->p3_params[want - 2];
                    uint32_t br = p->p3_params[want - 1];
                    int x1 = tl & 0x3fff, y1 = (tl >> 16) & 0x3fff;
                    int x2 = br & 0x3fff, y2 = (br >> 16) & 0x3fff;

                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                            p->p3_params[0]);
                    ati_rage128_reg_write32(s, R128_DP_BRUSH_FRGD_CLR,
                                            p->p3_params[1]);
                    if (want == 7) {
                        ati_rage128_reg_write32(s, R128_BRUSH_DATA0,
                                                p->p3_params[2]);
                        ati_rage128_reg_write32(s, R128_BRUSH_DATA0 + 4,
                                                p->p3_params[3]);
                        ati_rage128_reg_write32(s, R128_BRUSH_Y_X,
                                                p->p3_params[4]);
                    }
                    if (x2 > x1 && y2 > y1) {
                        s->dst_x = x1;
                        s->dst_y = y1;
                        s->dst_width = x2 - x1;
                        s->dst_height = y2 - y1;
                        trace_ati_rage128_paint_multi(0, tl, br, s->dst_x,
                                                      s->dst_y, s->dst_width,
                                                      s->dst_height);
                        ati_rage128_2d_blt(s);
                    }
                }
            } else if (p->p3_param_idx == 2) {
                s->dst_x = p->p3_params[0] & 0x3fff;
                s->dst_y = (p->p3_params[0] >> 16) & 0x3fff;
                s->dst_width = p->p3_params[1] & 0x3fff;
                s->dst_height = (p->p3_params[1] >> 16) & 0x3fff;
                ati_rage128_2d_blt(s);
            }
            break;
        }
        case R128_PM4_OPCODE_PAINT_MULTI:
            /*
             * [0] context, [1] colour, then (DST_X_Y, DST_WIDTH_HEIGHT)
             * pairs -- see the ring parser's copy of this case for the
             * live capture that established the field order.
             */
            if (p->p3_param_idx == 0) {
                /*
                 * see the ring parser: a context with
                 * DST_PITCH_OFFSET_CNTL set is followed by the
                 * destination pitch/offset dword, then the colour
                 */
                ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, val);
                p->p3_params[3] = (val & R128_GMC_DST_PITCH_OFFSET_CNTL) ?
                                  1 : 0;
                p->p3_param_idx = 1;
            } else if (p->p3_param_idx == 1 && p->p3_params[3]) {
                ati_rage128_reg_write32(s, R128_DST_PITCH_OFFSET, val);
                p->p3_params[3] = 0;
            } else if (p->p3_param_idx == 1) {
                ati_rage128_reg_write32(s, R128_DP_BRUSH_FRGD_CLR, val);
                p->p3_param_idx = 2;
            } else if (p->p3_param_idx == 2) {
                p->p3_params[2] = val;
                p->p3_param_idx = 3;
            } else {
                uint32_t dst_x_y = p->p3_params[2];

                s->dst_y = dst_x_y & 0x3fff;
                s->dst_x = (dst_x_y >> 16) & 0x3fff;
                s->dst_height = val & 0x3fff;
                s->dst_width = (val >> 16) & 0x3fff;
                trace_ati_rage128_paint_multi(0, dst_x_y, val, s->dst_x,
                                              s->dst_y, s->dst_width,
                                              s->dst_height);
                if (s->dst_width && s->dst_height) {
                    ati_rage128_2d_blt(s);
                }
                p->p3_param_idx = 2;   /* next rectangle pair */
            }
            break;
        case R128_PM4_OPCODE_BITBLT_MULTI:
            /*
             * Same packet as the ring parser's copy; see there for the
             * layout. The rectangle run is longer than p3_params, so each
             * three-dword rectangle is gathered in place and issued as
             * soon as it completes rather than buffering the packet.
             * p3_params[3] counts the pitch/offset dwords the context
             * announced, so the rectangle run is known to start at
             * index 1 + that count.
             */
            if (p->p3_param_idx == 0) {
                ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL, val);
                p->p3_params[3] = 0;
                if (val & R128_GMC_SRC_PITCH_OFFSET_CNTL) {
                    p->p3_params[3]++;
                }
                if (val & R128_GMC_DST_PITCH_OFFSET_CNTL) {
                    p->p3_params[3]++;
                }
                p->p3_params[4] = val;
            } else if (p->p3_param_idx <= p->p3_params[3]) {
                bool src_first = p->p3_params[4] &
                                 R128_GMC_SRC_PITCH_OFFSET_CNTL;

                if (p->p3_param_idx == 1 && src_first) {
                    ati_rage128_reg_write32(s, R128_SRC_PITCH_OFFSET, val);
                } else {
                    ati_rage128_reg_write32(s, R128_DST_PITCH_OFFSET, val);
                }
            } else {
                unsigned slot = (p->p3_param_idx - 1 - p->p3_params[3]) % 3;

                p->p3_params[slot] = val;
                if (slot == 2) {
                    s->src_y = p->p3_params[0] & 0x3fff;
                    s->src_x = (p->p3_params[0] >> 16) & 0x3fff;
                    s->dst_y = p->p3_params[1] & 0x3fff;
                    s->dst_x = (p->p3_params[1] >> 16) & 0x3fff;
                    s->dst_height = p->p3_params[2] & 0x3fff;
                    s->dst_width = (p->p3_params[2] >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s);
                }
            }
            p->p3_param_idx++;
            break;

        case R128_PM4_OPCODE_BITBLT:
            /* see the ring parser's copy of this case for the layout */
            if (p->p3_param_idx < 4) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 4) {
                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                            p->p3_params[0]);
                    s->src_y = p->p3_params[1] & 0x3fff;
                    s->src_x = (p->p3_params[1] >> 16) & 0x3fff;
                    s->dst_y = p->p3_params[2] & 0x3fff;
                    s->dst_x = (p->p3_params[2] >> 16) & 0x3fff;
                    s->dst_height = p->p3_params[3] & 0x3fff;
                    s->dst_width = (p->p3_params[3] >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s);
                }
            }
            break;
        case R128_PM4_OPCODE_SCALING:
            /* see the ring parser's copy for the packet layout */
            if (p->p3_param_idx < R128_SCALE_PKT_DWORDS) {
                p->p3_scale[p->p3_param_idx++] = val;
                if (p->p3_param_idx == R128_SCALE_PKT_DWORDS) {
                    ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                    p->p3_scale[R128_SCALE_PKT_GMC]);
                    ati_rage128_reg_write32(s, R128_SC_TOP_LEFT,
                                    p->p3_scale[R128_SCALE_PKT_SC_TL]);
                    ati_rage128_reg_write32(s, R128_SC_BOTTOM_RIGHT,
                                    p->p3_scale[R128_SCALE_PKT_SC_BR]);
                    ati_rage128_2d_scale(s, p->p3_scale);
                }
            }
            break;
        case R128_PM4_OPCODE_HOSTDATA_BLT:
        {
            /* header dwords before the pixel data -- see the ring parser */
            uint32_t nhdr = p->p3_total >= 8 ? 8 : 2;

            if (p->p3_param_idx < nhdr) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == nhdr) {
                    uint32_t yx = nhdr == 8 ? p->p3_params[5]
                                            : p->p3_params[0];
                    uint32_t hw = nhdr == 8 ? p->p3_params[6]
                                            : p->p3_params[1];

                    if (nhdr == 8) {
                        /*
                         * Context first, then the clip -- see the ring
                         * parser's copy of this case.
                         */
                        ati_rage128_reg_write32(s, R128_DP_GUI_MASTER_CNTL,
                                                p->p3_params[0]);
                        ati_rage128_reg_write32(s, R128_SC_TOP_LEFT,
                                                p->p3_params[1]);
                        ati_rage128_reg_write32(s, R128_SC_BOTTOM_RIGHT,
                                                p->p3_params[2]);
                    }
                    s->dst_x = yx & 0x3fff;
                    s->dst_y = (yx >> 16) & 0x3fff;
                    s->dst_width = hw & 0x3fff;
                    s->dst_height = (hw >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s); /* enters host-data mode */
                }
            } else if (s->host_data_active) {
                s->host_data_acc[s->host_data_next++] = val;
                if (s->host_data_next >= 4) {
                    ati_rage128_host_data_flush(s);
                    s->host_data_next = 0;
                }
            }
            break;
        }
        default:
            /*
             * Payload of an opcode we do not model. Still advance the
             * parameter index: it is what the payload trace reports, and
             * leaving it pinned at zero made every dword of such a packet
             * look like a fresh one-dword packet, which turned a single
             * 960-dword CNTL_SCALING into 960 phantom packets in the
             * offline analyser. The data itself is discarded as before.
             */
            if (p->p3_param_idx < 0xffff) {
                p->p3_param_idx++;
            }
            break;
        }
        break;
    default:
        break;
    }
    p->remaining--;
    if (p->remaining == 0 && p->type == 3 &&
        p->p3_opcode == R128_PM4_OPCODE_HOSTDATA_BLT &&
        s->host_data_active) {
        ati_rage128_host_data_flush(s);
        s->host_data_active = false;
        s->host_data_next = 0;
    }
}

static void ati_rage128_pm4_fifo_push(ATIRage128State *s, uint32_t val)
{
    ati_rage128_pm4_parse(s, &s->pm4_fifo, val);
}

/*
 * Indirect-buffer dispatch (PM4_IW_INDOFF/INDSIZE): the engine
 * fetches `dwords` command dwords from card address `offset` --
 * bus-master command submission from GART-translated system memory
 * (or VRAM). This is how Mac OS X's driver, running the CCE in mode
 * 7 (64PIO_64VCBM_64INDBM), submits its actual drawing/mode-set
 * command buffers; the Linux driver does the same via
 * r128_cce_dispatch_indirect(). Each fetched dword goes through the
 * same packet state machine as the PIO FIFO, so every packet type the
 * FIFO path understands works identically here.
 */
static void ati_rage128_pm4_indirect(ATIRage128State *s, uint32_t offset,
                                     uint32_t dwords)
{
    /*
     * The IND queue is a bus-master fetcher by definition (the CNTL
     * mode names literally call it INDBM): with a GART configured,
     * IW_INDOFF addresses are offsets into the GART-translated VM
     * space (guest system memory), even small ones -- confirmed live:
     * treating small offsets as VRAM read all-zero/stale bytes, while
     * the guest's real command buffers sit in the GART pages.
     */
    bool gart = (s->regs[R128_PCI_GART_PAGE >> 2] & ~0xfffu) != 0;
    ATIRage128PM4Parser parser = { 0 };
    uint32_t i;

    trace_ati_rage128_pm4_indirect(offset, dwords,
        dwords ? ati_rage128_card_read32(s, offset, gart) : 0);
    if (dwords > 0x10000) {
        /* bogus size -- a real IB is at most a few KB */
        return;
    }
    for (i = 0; i < dwords; i++) {
        uint32_t val = ati_rage128_card_read32(s, offset + i * 4, gart);

        trace_ati_rage128_pm4_ib_dword(i, val);
        ati_rage128_pm4_parse(s, &parser, val);
    }
}

/*
 * All register apertures allow 1/2/4-byte access at any offset
 * (RRG-G04500-C: "access: 8/16/32"). Sub-dword accesses are folded
 * onto the 32-bit register via read-modify-write against the raw
 * stored value.
 */
static uint64_t ati_rage128_mmio_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    ATIRage128State *s = opaque;
    uint32_t base = addr & 0x3ffc;
    uint32_t val = ati_rage128_reg_read32(s, base);

    val = extract32(val, (addr & 3) * 8, size * 8);
    if (ati_rage128_reg_name(base)[0] == '?') {
        trace_ati_rage128_unk_read(size, addr, val);
    } else {
        trace_ati_rage128_reg_read(size, addr, ati_rage128_reg_name(base),
                                   val);
    }
    return val;
}

static void ati_rage128_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
    ATIRage128State *s = opaque;
    uint32_t base = addr & 0x3ffc;
    uint32_t val = data;

    if (ati_rage128_reg_name(base)[0] == '?') {
        trace_ati_rage128_unk_write(size, addr, data);
    } else {
        trace_ati_rage128_reg_write(size, addr, ati_rage128_reg_name(base),
                                    data);
    }
    if (size != 4 || (addr & 3)) {
        /*
         * Merge the written lanes onto the register's current value.
         * For the MM_DATA and CLOCK_CNTL_DATA pass-throughs the
         * current value lives behind the indirection, not in the raw
         * regs[] slot -- the FCode does byte writes through both (e.g.
         * single-byte PLL data writes), and merging against the
         * always-zero raw slot would clobber the target's other lanes.
         */
        uint32_t mbase = base;
        uint32_t merged;

        if (mbase == R128_MM_DATA) {
            mbase = s->regs[R128_MM_INDEX >> 2] & 0x3ffc;
        }
        if (mbase == R128_CLOCK_CNTL_DATA) {
            merged = s->plls[s->regs[R128_CLOCK_CNTL_INDEX >> 2] &
                             R128_PLL_ADDR_MASK];
        } else {
            /* raw slot; auto-increment/FIFO registers keep their own
             * state elsewhere, so this stays side-effect free */
            merged = s->regs[mbase >> 2];
        }
        merged = deposit32(merged, (addr & 3) * 8, size * 8, data);
        val = merged;
    }
    ati_rage128_reg_write32(s, base, val);
}

static const MemoryRegionOps ati_rage128_mmio_ops = {
    .read = ati_rage128_mmio_read,
    .write = ati_rage128_mmio_write,
    /*
     * Native little-endian PCI device, exactly like the mach64 (whose
     * BE declaration was a long-lived bring-up bug -- see the comment
     * on ati_mach64_mmio_ops). The chip's big-endian support is the
     * separate aperture/register endian swappers in CONFIG_CNTL.
     */
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Aperture 1: the second 32MB framebuffer image, with the CONFIG_CNTL
 * APER_1_ENDIAN byte swapper actually applied (RRG-G04500-C: "For the
 * PowerMac environment, this allows each [aperture] to be
 * independently marked as big-endian or little-endian"). Mac OS X's
 * driver programs mode 2 (32-bit swap) and stages its CCE indirect
 * buffers through this half; the swap is what turns its native
 * big-endian stores into the little-endian bytes the command
 * processor expects to fetch.
 *
 * The swap is modeled as byte-lane XOR addressing (how the real
 * silicon implements it): mode 1 (16-bit swap) = XOR 1, mode 2
 * (32-bit swap) = XOR 3, mode 3 (half-dword swap) = XOR 2. With the
 * region declared guest-native big-endian, an unswapped mapping would
 * put an access's most-significant byte at the lowest address, so
 * lane i of an N-byte access carries byte N-1-i of the value.
 */
static uint64_t ati_rage128_aper1_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    ATIRage128State *s = opaque;
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    static const uint32_t xor_map[4] = { 0, 1, 3, 2 };
    uint32_t mode = (s->regs[R128_CONFIG_CNTL >> 2] >>
                     R128_APER_1_ENDIAN_SHIFT) & R128_APER_0_ENDIAN_MASK;
    uint32_t lane_xor = xor_map[mode];
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint64_t off = (addr + i) ^ lane_xor;

        if (off < ATI_RAGE128_VRAM_SIZE) {
            val |= (uint64_t)vram[off] << (8 * (size - 1 - i));
        }
    }
    return val;
}

static void ati_rage128_aper1_write(void *opaque, hwaddr addr, uint64_t data,
                                    unsigned size)
{
    ATIRage128State *s = opaque;
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    static const uint32_t xor_map[4] = { 0, 1, 3, 2 };
    uint32_t mode = (s->regs[R128_CONFIG_CNTL >> 2] >>
                     R128_APER_1_ENDIAN_SHIFT) & R128_APER_0_ENDIAN_MASK;
    uint32_t lane_xor = xor_map[mode];
    unsigned i;

    for (i = 0; i < size; i++) {
        uint64_t off = (addr + i) ^ lane_xor;

        if (off < ATI_RAGE128_VRAM_SIZE) {
            vram[off] = (data >> (8 * (size - 1 - i))) & 0xff;
        }
    }
    /*
     * Diagnostic: CPU stores into the frame buffer bypass the drawing
     * engine entirely, so nothing else in this device can see them. Some
     * guest text is drawn this way.
     */
    if (trace_event_get_state_backends(TRACE_ATI_RAGE128_APER_WR)) {
        uint32_t crtc = s->regs[R128_CRTC_OFFSET >> 2] & 0x7ffffff;
        uint32_t pitch = (s->regs[R128_CRTC_PITCH >> 2] & 0x7ff) * 8;
        int px = -1, py = -1;

        if (pitch && addr >= crtc) {
            uint32_t rel = addr - crtc;

            py = rel / (pitch * 4);
            px = (rel % (pitch * 4)) / 4;
        }
        trace_ati_rage128_aper_wr(1, (uint32_t)addr, size, data, px, py);
    }
    /* keep the dirty-bitmap framebuffer scanner seeing these writes */
    memory_region_set_dirty(&s->vram, addr & ~7ull, 8);
}

/*
 * Diagnostic window over part of aperture 0 (see the fillwatch fields).
 * Aperture 0 applies no endian swap, so this has to behave exactly like
 * the RAM alias it replaces: on a big-endian guest a store's most
 * significant byte lands at the lowest address, which is what writing
 * lane i as byte (size-1-i) of a DEVICE_BIG_ENDIAN access does.
 */
static uint64_t ati_rage128_fillwatch_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    ATIRage128State *s = opaque;
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t base = s->fillwatch_off + (uint32_t)addr;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (base + i < ATI_RAGE128_VRAM_SIZE) {
            val |= (uint64_t)vram[base + i] << (8 * (size - 1 - i));
        }
    }
    return val;
}

static void ati_rage128_fillwatch_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    ATIRage128State *s = opaque;
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t base = s->fillwatch_off + (uint32_t)addr;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (base + i < ATI_RAGE128_VRAM_SIZE) {
            vram[base + i] = (data >> (8 * (size - 1 - i))) & 0xff;
        }
    }
    memory_region_set_dirty(&s->vram, base & ~7ull, 8);

    /*
     * Coalesce. A strided fill writes each row as one contiguous run and
     * then jumps, so the run length and the jump to the next run's start
     * are exactly the geometry we are trying to recover: the delta
     * between successive run starts IS the stride the CPU is filling at.
     */
    if (s->fw_active && base == s->fw_run_end) {
        s->fw_run_end = base + size;
        return;
    }
    if (s->fw_active) {
        trace_ati_rage128_fillwatch(s->fw_run_start,
                                    s->fw_run_end - s->fw_run_start,
                                    base - s->fw_run_start);
    }
    s->fw_run_start = base;
    s->fw_run_end = base + size;
    s->fw_active = true;
}

static const MemoryRegionOps ati_rage128_fillwatch_ops = {
    .read = ati_rage128_fillwatch_read,
    .write = ati_rage128_fillwatch_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static const MemoryRegionOps ati_rage128_aper1_ops = {
    .read = ati_rage128_aper1_read,
    .write = ati_rage128_aper1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* ---------------------------------------------------------------- */

static void ati_rage128_reset_hold(Object *obj, ResetType type)
{
    ATIRage128State *s = ATI_RAGE128(obj);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->plls, 0, sizeof(s->plls));
    memset(s->palette, 0, sizeof(s->palette));
    s->dac_wr_index = 0;
    s->dac_rd_index = 0;
    s->i2c_offset = 0;
    s->i2c_data_len = 0;
    s->i2c_data_pos = 0;
    s->mode_dirty = true;

    /* Documented non-zero reset defaults (RRG-G04500-C) */
    s->regs[R128_DAC_CNTL >> 2] = 0x2 |                 /* PS2 output level */
                                  R128_DAC_CMP_EN |
                                  (R128_DAC_MASK_DEFAULT <<
                                   R128_DAC_MASK_SHIFT);
    s->regs[R128_CRTC_EXT_CNTL >> 2] = R128_DFIFO_EXTSENSE |
                                       R128_CRTC_DISPLAY_DIS;
    s->regs[R128_CRTC_STATUS >> 2] = R128_FIX_VSYNC_TIMING;
    s->regs[R128_CRTC_GEN_CNTL >> 2] = R128_CRTC_DISP_REQ_EN_B;
    s->plls[R128_PLL_CLK_PIN_CNTL] = 0xf7; /* all clock outputs enabled */
}

/*
 * Hardware cursor. CRTC_CUR_MODE 0 -- the only mode the chip defines --
 * is a 64x64 two-colour image with transparent and inverting codes
 * (RRG-G04500-C 3.13). The image sits at CUR_OFFSET in the frame buffer
 * as 16 bytes per row: eight bytes of AND mask followed by eight of XOR
 * mask, most significant bit leftmost. (AND,XOR) selects CUR_CLR0,
 * CUR_CLR1, "leave the destination alone" or "invert it".
 *
 * That layout was confirmed against the live image Mac OS X programs on
 * this card rather than assumed: decoded any other plausible way -- as
 * separate AND/XOR planes, or as the mach64's packed 2bpp -- the same
 * 1024 bytes come out as noise, and this way they come out as the Mac
 * arrow. Note also that CUR_CLR0/1 are plain 0x00RRGGBB here, NOT the
 * mach64's colour-in-bits-31:8 layout; the two decodes are not
 * interchangeable. QEMUCursor data is RGBA byte order, i.e. a dword of
 * (a << 24) | (b << 16) | (g << 8) | r (ui/cursor.c).
 *
 * "Invert the destination" has no QEMUCursor equivalent, so it becomes
 * 50%-alpha black -- the same approximation the mach64 uses, and the
 * classic Mac cursors only use that code to soften edges.
 */
static void ati_rage128_cursor_update(ATIRage128State *s)
{
    uint32_t posn = s->regs[R128_CUR_HORZ_VERT_POSN >> 2];
    uint32_t coff = s->regs[R128_CUR_HORZ_VERT_OFF >> 2];
    uint32_t offs = s->regs[R128_CUR_OFFSET >> 2];
    uint32_t raw0 = s->regs[R128_CUR_CLR0 >> 2];
    uint32_t raw1 = s->regs[R128_CUR_CLR1 >> 2];
    uint32_t clr0 = 0xff000000u | ((raw0 & 0xff) << 16) |
                    (raw0 & 0xff00) | ((raw0 >> 16) & 0xff);
    uint32_t clr1 = 0xff000000u | ((raw1 & 0xff) << 16) |
                    (raw1 & 0xff00) | ((raw1 >> 16) & 0xff);
    uint32_t vram_off = offs & R128_CUR_OFFSET_MASK;
    unsigned horz_off = (coff >> R128_CUR_HORZ_OFF_SHIFT) &
                        R128_CUR_HORZ_OFF_MASK;
    unsigned vert_off = coff & R128_CUR_VERT_OFF_MASK;
    bool on = (s->regs[R128_CRTC_GEN_CNTL >> 2] & R128_CRTC_CUR_EN) != 0;
    const uint8_t *src;
    uint32_t sum = 0;
    QEMUCursor *c;
    unsigned row, px;
    int x, y;

    if (!s->con) {
        return;
    }
    /*
     * Host-side tracking owns the pointer when it is running (see the
     * host_cursor_last_ns comment): let it, rather than fighting it with a
     * hardware cursor the guest may not be keeping up to date.
     */
    if (s->host_cursor_active) {
        return;
    }
    /*
     * CUR_LOCK freezes all three of CUR_OFFSET/POSN/OFF so the driver can
     * change shape and position together; publishing a half-updated
     * cursor is exactly the tearing the bit exists to prevent.
     */
    if ((offs | posn | coff) & R128_CUR_LOCK) {
        return;
    }
    if (!on) {
        if (s->hw_cursor_on) {
            trace_ati_rage128_cursor_off(s->hw_cursor_x, s->hw_cursor_y);
            s->hw_cursor_on = false;
            s->hw_cursor_sum = 0;
            qemu_console_set_mouse(s->con, 0, 0, false);
        }
        return;
    }
    if ((uint64_t)vram_off + R128_CUR_IMAGE_BYTES > ATI_RAGE128_VRAM_SIZE) {
        return;
    }
    src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram) + vram_off;

    /*
     * CUR_HORZ_OFF says which column of the map is drawn at CUR_HORZ_POSN,
     * so the image's own left edge belongs at POSN - OFF. Vertically the
     * driver does it differently: to push the cursor off the top it
     * advances CUR_OFFSET by 16 bytes per hidden row *as well as* raising
     * CUR_VERT_OFF, so the data already begins at the first visible row.
     * The top edge is therefore CUR_VERT_POSN with no correction, and the
     * final CUR_VERT_OFF rows of the map are simply not part of the
     * cursor ("Height of cursor is (64-CUR_VERT_OFF)").
     */
    x = (int)((posn >> R128_CUR_HORZ_POSN_SHIFT) & R128_CUR_HORZ_POSN_MASK) -
        (int)horz_off;
    y = (int)(posn & R128_CUR_VERT_POSN_MASK);

    for (row = 0; row < R128_CUR_IMAGE_BYTES; row++) {
        sum = (sum << 1 | sum >> 31) ^ src[row];
    }
    sum ^= clr0 ^ clr1 ^ (uint32_t)horz_off ^ ((uint32_t)vert_off << 8);

    if (s->hw_cursor_on && sum == s->hw_cursor_sum) {
        /* Same image: a move only, so don't re-upload it -- and if it
         * has not moved either (this runs on every refresh tick), do
         * nothing at all. */
        if (x != s->hw_cursor_x || y != s->hw_cursor_y) {
            trace_ati_rage128_cursor_move(vram_off, x, y, sum);
            s->hw_cursor_x = x;
            s->hw_cursor_y = y;
            qemu_console_set_mouse(s->con, x, y, true);
        }
        return;
    }

    c = cursor_alloc(R128_CUR_WIDTH, R128_CUR_HEIGHT);
    for (row = 0; row < R128_CUR_HEIGHT; row++) {
        for (px = 0; px < R128_CUR_WIDTH; px++) {
            const uint8_t *line = src + row * R128_CUR_ROW_BYTES;
            unsigned bit = 7 - (px & 7);
            unsigned and_bit = (line[px >> 3] >> bit) & 1;
            unsigned xor_bit = (line[8 + (px >> 3)] >> bit) & 1;
            uint32_t val;

            if (row >= R128_CUR_HEIGHT - vert_off || px < horz_off) {
                val = 0;                        /* outside the cursor */
            } else if (!and_bit) {
                val = xor_bit ? clr1 : clr0;
            } else {
                val = xor_bit ? 0x80000000u : 0;    /* invert : transparent */
            }
            c->data[row * R128_CUR_WIDTH + px] = val;
        }
    }
    trace_ati_rage128_cursor_upload(vram_off, horz_off, vert_off, x, y,
                                    c->data[0], c->data[64 * 4],
                                    c->data[64 * 4 + 4]);
    qemu_console_set_cursor(s->con, c);
    cursor_unref(c);
    s->hw_cursor_on = true;
    s->hw_cursor_sum = sum;
    s->hw_cursor_x = x;
    s->hw_cursor_y = y;
    qemu_console_set_mouse(s->con, x, y, true);
}

/*
 * Host-driven pointer for this display (see the header), used only while
 * the guest's own hardware cursor is switched off -- Mac OS X does drive
 * these registers, so once it has, the real cursor wins and this stands
 * aside rather than fighting it for the console cursor.
 */
void ati_rage128_host_cursor(int x, int y, bool on)
{
    /* classic 16x16 arrow: data = black pixels, mask = opaque area */
    static const uint16_t arrow_data[16] = {
        0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00,
        0x7f80, 0x7c00, 0x6c00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
    };
    static const uint16_t arrow_mask[16] = {
        0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00, 0xff80,
        0xffc0, 0xffe0, 0xfe00, 0xef00, 0xcf00, 0x8780, 0x0780, 0x0380,
    };
    ATIRage128State *s;
    Object *o = object_resolve_path_type("", TYPE_ATI_RAGE128, NULL);

    if (!o) {
        return;
    }
    s = ATI_RAGE128(o);
    if (!s->con) {
        return;
    }
    s->host_cursor_active = true;
    s->hw_cursor_on = false;

    if (on && !s->host_cursor_published) {
        QEMUCursor *c = cursor_alloc(16, 16);
        int row, col;

        for (row = 0; row < 16; row++) {
            for (col = 0; col < 16; col++) {
                uint16_t bit = 0x8000 >> col;

                if (!(arrow_mask[row] & bit)) {
                    c->data[row * 16 + col] = 0;            /* transparent */
                } else if (arrow_data[row] & bit) {
                    c->data[row * 16 + col] = 0xff000000u;  /* black */
                } else {
                    c->data[row * 16 + col] = 0xffffffffu;  /* white edge */
                }
            }
        }
        qemu_console_set_cursor(s->con, c);
        cursor_unref(c);
        s->host_cursor_published = true;
    }
    qemu_console_set_mouse(s->con, x, y, on);
}

static void ati_rage128_realize(PCIDevice *dev, Error **errp)
{
    ATIRage128State *s = ATI_RAGE128(dev);
    Object *obj = OBJECT(dev);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0,
                                         &ati_rage128_gfx_ops, s);

    /*
     * BAR0: the 64MB linear aperture, holding two 32MB "images" of the
     * frame buffer (CONFIG_APER_0_BASE / CONFIG_APER_1_BASE). Each
     * image can be given its own endian-swap mode via CONFIG_CNTL.
     * Aperture 0 stays a plain RAM mapping (fast path; a guest
     * enabling a swap mode on it is only traced) -- aperture 1 is a
     * real swapping IO view, which is the half Mac drivers configure
     * as their byte-swapped window (observed live: Mac OS X sets
     * CONFIG_CNTL=0x8 = 32-bit swap on aperture 1 and stages its CCE
     * indirect buffers through it; without the swap those buffers
     * land byte-reversed and every command mis-parses).
     */
    memory_region_init(&s->aper, obj, "ati-rage128-aper",
                       ATI_RAGE128_APER_SIZE);
    memory_region_init_ram(&s->vram, obj, "ati-rage128-vram",
                           ATI_RAGE128_VRAM_SIZE, &error_fatal);
    /*
     * Needed by ati_rage128_scan_vram_activity() to auto-detect the
     * real live framebuffer when CRTC1 never describes it (see that
     * function's comment) -- reuses the same DIRTY_MEMORY_VGA client
     * every other display device's dirty-scanline tracking already
     * uses, since this device has no dirty-tracking use of its own to
     * conflict with.
     */
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    memory_region_add_subregion(&s->aper, 0, &s->vram);
    memory_region_init_io(&s->vram_aper1, obj, &ati_rage128_aper1_ops, s,
                          "ati-rage128-aper1", ATI_RAGE128_VRAM_SIZE);
    memory_region_add_subregion(&s->aper, ATI_RAGE128_APER_SIZE / 2,
                                &s->vram_aper1);
    /*
     * Optional diagnostic overlay on aperture 0 (see the fillwatch fields
     * in the header). Higher priority than the RAM alias underneath, so
     * stores in this range come to us instead of landing silently.
     */
    if (s->fillwatch_size &&
        (uint64_t)s->fillwatch_off + s->fillwatch_size <=
        ATI_RAGE128_VRAM_SIZE) {
        memory_region_init_io(&s->vram_watch, obj,
                              &ati_rage128_fillwatch_ops, s,
                              "ati-rage128-fillwatch", s->fillwatch_size);
        memory_region_add_subregion_overlap(&s->aper, s->fillwatch_off,
                                            &s->vram_watch, 1);
    }

    memory_region_init_io(&s->mmio, obj, &ati_rage128_mmio_ops, s,
                          "ati-rage128-mmio", ATI_RAGE128_MMIO_SIZE);
    /*
     * BAR1: the 256-byte I/O register window. Registers above 0xFF are
     * reachable through it via the MM_INDEX/MM_DATA pair at 0x00/0x04
     * -- which the shared mmio ops already implement, so the window is
     * simply the low 256 bytes of the same register file.
     */
    memory_region_init_io(&s->io, obj, &ati_rage128_mmio_ops, s,
                          "ati-rage128-io", ATI_RAGE128_IO_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->aper);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /*
     * PCI interrupt pin A. The FCode publishes an "interrupts"
     * property for the NDRV from this (same lesson as the mach64:
     * pin 0 = "no interrupt" makes OF omit the property and the
     * native driver then fails its interrupt lookup).
     */
    pci_config_set_interrupt_pin(dev->config, 1);

    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ati_rage128_vblank_timer_tick, s);
    s->vblank_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ati_rage128_vblank_end_tick, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ATI_RAGE128_VBLANK_PERIOD_NS);

    /*
     * Period-correct EDID: an Apple-style 17" multiscan CRT limited to
     * the classic Mac timing set (640x480@60/67 through 1280x1024@75,
     * preferred 1152x870@75, vertical range capped at 75Hz). QEMU's
     * generated default EDID advertises modern high-refresh modes,
     * which a guest driver that builds its mode list from DDC turns
     * into era-absurd offerings (observed live under Mac OS 9: 120Hz
     * refresh choices on a "VGA Display").
     */
    static const uint8_t rage128_default_edid[128] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
        0x06, 0x10, 0x75, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x0c, 0x09, 0x01, 0x01, 0x08, 0x20, 0x18, 0x78,
        0x0a, 0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26,
        0x0f, 0x50, 0x54, 0x31, 0x6b, 0x80, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x10, 0x27,
        0x80, 0x30, 0x41, 0x66, 0x2d, 0x30, 0x40, 0x80,
        0x13, 0x00, 0x40, 0xf0, 0x10, 0x00, 0x00, 0x1e,
        0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e,
        0x46, 0x0a, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x4d,
        0x75, 0x6c, 0x74, 0x69, 0x70, 0x6c, 0x65, 0x20,
        0x53, 0x63, 0x61, 0x6e, 0x00, 0x00, 0x00, 0x10,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76,
    };

    memcpy(s->edid, rage128_default_edid, sizeof(s->edid));

    /*
     * The MONID bit-banged DDC bus (see the R128_GPIO_MONID write
     * handler): the ATI Mac drivers read EDID here, not through the
     * hardware I2C engine.
     */
    {
        I2CBus *ddcbus = i2c_init_bus(DEVICE(dev), "ati-rage128.monid-ddc");
        I2CSlave *slv = i2c_slave_create_simple(ddcbus,
                                                TYPE_ATI_RAGE128_DDC, 0x50);

        ATI_RAGE128_DDC(slv)->edid = s->edid;
        bitbang_i2c_init(&s->monid_i2c, ddcbus);
        s->monid_sda = 1;
    }
}

static void ati_rage128_exit(PCIDevice *dev)
{
    ATIRage128State *s = ATI_RAGE128(dev);

    timer_free(s->vblank_timer);
    timer_free(s->vblank_end_timer);
    qemu_graphic_console_close(s->con);
}

static const VMStateDescription vmstate_ati_rage128 = {
    .name = "ati-rage128-pro",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, ATIRage128State),
        VMSTATE_UINT32_ARRAY(regs, ATIRage128State, ATI_RAGE128_NUM_REGS),
        VMSTATE_UINT32_ARRAY(plls, ATIRage128State, ATI_RAGE128_NUM_PLLS),
        VMSTATE_UINT8_2DARRAY(palette, ATIRage128State, 256, 3),
        VMSTATE_UINT8(dac_wr_index, ATIRage128State),
        VMSTATE_UINT8(dac_rd_index, ATIRage128State),
        VMSTATE_UINT8(i2c_offset, ATIRage128State),
        VMSTATE_UINT8_ARRAY(i2c_data_fifo, ATIRage128State, 16),
        VMSTATE_INT32(i2c_data_len, ATIRage128State),
        VMSTATE_INT32(i2c_data_pos, ATIRage128State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ati_rage128_properties[] = {
    /*
     * Diagnostic only: lay an instrumented window over part of aperture 0
     * so CPU fills of that VRAM range become traceable. Off by default.
     */
    DEFINE_PROP_UINT32("fillwatch", ATIRage128State, fillwatch_off, 0),
    DEFINE_PROP_UINT32("fillwatch-size", ATIRage128State, fillwatch_size, 0),
    DEFINE_PROP_BOOL("monitor-connected", ATIRage128State,
                     monitor_connected, true),
    DEFINE_EDID_PROPERTIES(ATIRage128State, edid_info),
};

static void ati_rage128_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    /*
     * Must match the PCIR vendor/device of the OEM Mac ROM image
     * exactly, or Open Firmware refuses to bind the FCode to the
     * card. 0x5046 "PF" is an AGP-only part on real silicon; QEMU has
     * no AGP bus and the electrical difference is invisible to
     * software beyond the (absent) AGP capability block.
     */
    k->device_id = PCI_DEVICE_ID_ATI_RAGE128PRO;
    k->revision  = 0x00;
    /*
     * Add-in card: the FCode driver comes from the PCI expansion ROM,
     * which the Beige G3 ROM's own Open Firmware probes and executes
     * (unlike the onboard mach64, whose FCode lives inside the system
     * ROM -- see the deliberate no-romfile comment in ati_mach64.c).
     * No default romfile name: pass romfile=<path> on the -device
     * option, pointing at the real card ROM dump.
     */
    k->realize   = ati_rage128_realize;
    k->exit      = ati_rage128_exit;
    dc->vmsd     = &vmstate_ati_rage128;
    device_class_set_props(dc, ati_rage128_properties);
    rc->phases.hold = ati_rage128_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ati_rage128_type_info = {
    .name           = TYPE_ATI_RAGE128,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(ATIRage128State),
    .class_init     = ati_rage128_class_init,
    .interfaces     = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ati_rage128_register_types(void)
{
    type_register_static(&ati_rage128_type_info);
    type_register_static(&ati_rage128_ddc_info);
}

type_init(ati_rage128_register_types)
