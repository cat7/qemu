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

/*
 * Rebuild and publish the hardware cursor from the CUR_* registers and
 * the 2bpp image in VRAM. The display backend composites it for us via
 * qemu_console_set_cursor()/set_mouse(), so nothing is ever drawn into
 * the guest-visible framebuffer.
 *
 * Register semantics are from ATI's own RAGE 128 PRO Register Reference
 * Guide (RRG-G04500-C rev 1.01), section 3.13 "Hardware Cursor". The
 * Rage 128 is close to the mach64 here but differs in two ways that
 * matter, so this is not a straight copy of ati_mach64_cursor_update():
 *
 *  - CUR_OFFSET is a plain byte offset into the frame buffer (bits 24:0,
 *    "16 byte (128 bit) aligned... bits 3:0 of this field are hardwired
 *    to ZERO"). The mach64's equivalent counts in 8-byte units and its
 *    code multiplies by 8; doing that here would land 8x too far in.
 *  - The CLR registers put the colour in the LOW 24 bits -- B at 7:0,
 *    G at 15:8, R at 23:16 -- whereas the mach64 carries R,G,B up in
 *    bits 31-8. Decoding the mach64 way swaps and shifts the channels.
 *
 * The image is 64 rows of 16 bytes, which CRTC_GEN_CNTL.CRTC_CUR_MODE=0
 * (the only defined mode) calls "2bpp monochrome 64x64. 2 color,
 * transparent, inverse". That "2bpp" is the per-pixel bit budget, NOT
 * the interleaving: unlike the mach64's packed 2-bit pairs, each row
 * here is two 1-bit planes -- 8 bytes of AND mask, then 8 bytes of XOR
 * mask, MSB first. Decoding it the mach64 way turns a normal, mostly
 * transparent arrow into a solid 64x64 block (an all-ones AND byte
 * reads back as four "invert" pairs, an all-zero XOR byte as four
 * "colour 0" pairs), which is exactly what it looked like on screen.
 *
 * Established by dumping the live sprite out of a Mac OS X 10.3 guest
 * and rendering it under each candidate layout: this one yields the
 * standard Mac arrow with 97 of 4096 pixels opaque, the packed reading
 * yields 4070 of 4096. The register guide does not document the layout
 * either way, so that experiment is the authority here.
 *
 * AND=1 XOR=0 transparent, AND=1 XOR=1 invert, AND=0 XOR=0 colour 0,
 * AND=0 XOR=1 colour 1. "Invert" (complement the display pixel) has no
 * QEMUCursor equivalent; approximate it with 50%-alpha black.
 */
/*
 * Publishing the pointer *position* is separate from rebuilding the
 * sprite, and deliberately is not coalesced to a frame. Position is one
 * cheap call; the sprite rebuild reads 1KB of VRAM and allocates a
 * QEMUCursor. Making motion wait for the once-per-frame rebuild ties
 * pointer responsiveness to a full 800x600x32bpp redraw and shows up as
 * the pointer stalling for a moment under load. Only the image can tear
 * (CUR_OFFSET moving before CUR_VERT_OFF catches up), so only the image
 * needs the frame latch.
 */
static void ati_rage128_cursor_set_pos(ATIRage128State *s)
{
    uint32_t posn = s->regs[R128_CUR_HORZ_VERT_POSN >> 2];
    uint32_t offs = s->regs[R128_CUR_HORZ_VERT_OFF >> 2];
    bool on = (s->regs[R128_CRTC_GEN_CNTL >> 2] & R128_CRTC_CUR_EN) != 0;
    int x = (int)((posn >> 16) & 0x7ff) - (int)((offs >> 16) & 0x3f);
    int y = (int)(posn & 0x7ff);

    qemu_console_set_mouse(s->con, x, y, on);
}

static void ati_rage128_cursor_update(ATIRage128State *s)
{
    uint32_t offs = s->regs[R128_CUR_HORZ_VERT_OFF >> 2];
    uint32_t raw0 = s->regs[R128_CUR_CLR0 >> 2];
    uint32_t raw1 = s->regs[R128_CUR_CLR1 >> 2];
    /* QEMUCursor dwords are RGBA byte order, i.e. (a<<24)|(b<<16)|(g<<8)|r. */
    uint32_t clr0 = 0xff000000u | ((raw0 & 0xff) << 16) |
                    (((raw0 >> 8) & 0xff) << 8) | ((raw0 >> 16) & 0xff);
    uint32_t clr1 = 0xff000000u | ((raw1 & 0xff) << 16) |
                    (((raw1 >> 8) & 0xff) << 8) | ((raw1 >> 16) & 0xff);
    uint32_t vram_off = s->regs[R128_CUR_OFFSET >> 2] & 0x01fffff0;
    bool on = (s->regs[R128_CRTC_GEN_CNTL >> 2] & R128_CRTC_CUR_EN) != 0;
    uint32_t vert_off = offs & 0x3f;
    uint32_t horz_off = (offs >> 16) & 0x3f;
    /*
     * POSN gives the screen position of the *visible* top-left pixel,
     * and the OFF fields say how far into the 64x64 map that pixel is.
     * Horizontally the guest moves off the left edge by holding
     * CUR_HORZ_POSN at 0 and raising CUR_HORZ_OFF, so the map's own
     * column 0 belongs at POSN - OFF. Vertically it instead advances
     * CUR_OFFSET by 16 bytes per hidden line and raises CUR_VERT_OFF,
     * so the data already starts at the first visible row: the sprite
     * goes at plain CUR_VERT_POSN and is (64 - CUR_VERT_OFF) tall.
     * Subtracting vert_off here as well would double-compensate. The
     * screen position itself is applied by ati_rage128_cursor_set_pos().
     */
    uint32_t height = 64 - vert_off;
    const uint8_t *src;
    QEMUCursor *c;
    uint32_t px, row;

    /*
     * Deliberately NOT gated on CUR_LOCK. Skipping the update while the
     * lock bit is set looks reasonable -- that is what the bit is for --
     * but it can wedge the pointer permanently: this function is the only
     * thing that calls qemu_console_set_mouse(), so a skipped update
     * leaves the last published position on screen, and if the guest then
     * parks the cursor and stops writing these registers altogether
     * nothing ever re-publishes. Observed exactly that: registers reading
     * a settled top-left position while the visible pointer stayed in the
     * bottom-right corner indefinitely.
     *
     * There is nothing to gain in exchange, because Mac OS X never sets
     * CUR_LOCK (it read 0 in every sampled state). Tearing between the
     * CUR_OFFSET and CUR_VERT_OFF halves of an update is instead handled
     * by latching the cursor once per frame, the way the hardware does --
     * see the cursor_dirty field comment.
     */
    if (on) {
        if ((uint64_t)vram_off + (uint64_t)height * 16 >
            ATI_RAGE128_VRAM_SIZE) {
            return;
        }
        src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram) + vram_off;
        c = cursor_alloc(64, 64);
        for (row = 0; row < height; row++) {
            for (px = horz_off; px < 64; px++) {
                uint32_t byte = px / 8, bit = 7 - (px % 8);
                uint8_t and_bit = (src[row * 16 + byte] >> bit) & 1;
                uint8_t xor_bit = (src[row * 16 + 8 + byte] >> bit) & 1;
                uint32_t val;

                if (and_bit) {
                    val = xor_bit ? 0x80000000u /* invert */ : 0 /* clear */;
                } else {
                    val = xor_bit ? clr1 : clr0;
                }
                c->data[row * 64 + px] = val;
            }
        }
        qemu_console_set_cursor(s->con, c);
        cursor_unref(c);
    }
    ati_rage128_cursor_set_pos(s);
}

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

    if (s->cursor_dirty) {
        s->cursor_dirty = false;
        ati_rage128_cursor_update(s);
    }

    ati_rage128_scan_vram_activity(s);
    /*
     * A CRTC mode that is valid *right now* is authoritative and is never
     * second-guessed. The heuristic only gets a say while the registers
     * are not currently describing a usable mode, i.e. when we are
     * falling back on the remembered one above.
     *
     * The old "CRTC1 is never programmed properly" premise this override
     * was built on does not hold: a live Mac OS X 10.3 session reads back
     * CRTC_GEN_CNTL=0x03090600, H_DISP->800, V_DISP->600, CRTC_OFFSET
     * 0x8000, CRTC_PITCH 0x68 -- a complete and correct 800x600x32bpp
     * mode -- and the guest paints into exactly that. Letting a guess
     * outrank that is how the display ended up showing a garbled
     * 1024x768.
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
         * so CRTC_GEN_CNTL spends much of its life with CRTC_EN and
         * CRTC_EXT_DISP_EN down (Mac OS X 10.3 writes it as 0x1, 0x3,
         * 0x200 and 0x01000200 in the course of one mode change).
         * Skipping the check there left crtc_region_live false, so the
         * heuristic overrode a perfectly good, actively-painted
         * framebuffer every time -- visible as the display churning
         * between resolutions while the guest is already up, and as a
         * correct mode appearing briefly and then being lost again.
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
    }

    /*
     * Only reallocate the DisplaySurface when its geometry actually
     * changes. It used to be recreated whenever *any* part of the mode
     * differed or mode_dirty was set -- but mode_dirty is raised by every
     * CRTC register write, and CRTC_OFFSET is written on every buffer
     * flip ("Updated for buffer flips", RRG-G04500-C 3.9). Mac OS X
     * double-buffers, so that tore down and recreated the whole surface
     * several times a second, which the UI shows as flicker. A changed
     * offset or pitch only changes where we read from; the surface is
     * still the same size and format, so it can stay.
     *
     * Compare against the surface's own dimensions rather than against
     * the previously recorded mode. Those two can drift apart -- the
     * console can end up holding a surface this function did not create
     * -- and a stale s->mode that already "matches" then suppresses the
     * reallocation forever, leaving the guest drawing an 800x600 desktop
     * into a 640x480 surface (seen live: a correctly rendered but
     * off-centre, clipped installer window). Asking the surface is
     * self-correcting; asking our own bookkeeping is not.
     */
    ds = qemu_console_surface(s->con);
    if (!ds || surface_width(ds) != mode.width ||
        surface_height(ds) != mode.height) {
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
        if (s->monitor_connected) {
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
        trace_ati_rage128_monid(s->regs[base >> 2], y);
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
        val = s->dst_offset;
        break;
    case R128_DST_PITCH:
        val = s->dst_pitch | (s->dst_tile << 16);
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
    case R128_SRC_OFFSET:
        val = s->src_offset;
        break;
    case R128_SRC_PITCH:
        val = s->src_pitch | (s->src_tile << 16);
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
        val = s->sc_top;
        break;
    case R128_SC_LEFT:
        val = s->sc_left;
        break;
    case R128_SC_BOTTOM:
        val = s->sc_bottom;
        break;
    case R128_SC_RIGHT:
        val = s->sc_right;
        break;
    case R128_SRC_SC_BOTTOM:
        val = s->src_sc_bottom;
        break;
    case R128_SRC_SC_RIGHT:
        val = s->src_sc_right;
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

static void ati_rage128_reg_write32(ATIRage128State *s, uint32_t base,
                                    uint32_t val)
{
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
        /* CRTC_CUR_EN lives here, so the sprite goes on and off with it. */
        s->cursor_dirty = true;
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
        /*
         * CUR_LOCK is one physical lock bit, readable and writable
         * through any of these three addresses (RRG-G04500-C 3.13
         * documents it identically in all three). We keep the registers
         * as separate words, so mirror bit 31 across them by hand --
         * otherwise a guest that raises the lock through one register
         * and drops it through another leaves a stale set copy behind,
         * and the cursor stays frozen at its last published position
         * for good.
         */
        s->regs[base >> 2] = val;
        if (base == R128_CUR_HORZ_VERT_POSN) {
            /* motion only -- no sprite change, so publish straight away */
            ati_rage128_cursor_set_pos(s);
        } else {
            s->cursor_dirty = true;
        }
        if (val & R128_CUR_LOCK) {
            s->regs[R128_CUR_OFFSET >> 2] |= R128_CUR_LOCK;
            s->regs[R128_CUR_HORZ_VERT_POSN >> 2] |= R128_CUR_LOCK;
            s->regs[R128_CUR_HORZ_VERT_OFF >> 2] |= R128_CUR_LOCK;
        } else {
            s->regs[R128_CUR_OFFSET >> 2] &= ~R128_CUR_LOCK;
            s->regs[R128_CUR_HORZ_VERT_POSN >> 2] &= ~R128_CUR_LOCK;
            s->regs[R128_CUR_HORZ_VERT_OFF >> 2] &= ~R128_CUR_LOCK;
        }
        break;
    case R128_CUR_CLR0:
    case R128_CUR_CLR1:
        s->regs[base >> 2] = val;
        s->cursor_dirty = true;
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
        s->dst_offset = val & 0xfffffff0;
        break;
    case R128_DST_PITCH:
        s->dst_pitch = val & 0x3fff;
        s->dst_tile = (val >> 16) & 1;
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
        s->src_offset = (val & 0x1fffff) << 5;
        s->src_pitch = (val & 0x7fe00000) >> 21;
        s->src_tile = val >> 31;
        break;
    case R128_DST_PITCH_OFFSET:
        s->dst_offset = (val & 0x1fffff) << 5;
        s->dst_pitch = (val & 0x7fe00000) >> 21;
        s->dst_tile = val >> 31;
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
        s->dp_gui_master_cntl = val & 0xf800000f;
        s->dp_datatype = (val & 0x0f00) >> 8 | (val & 0x30f0) << 4 |
                         (val & 0x4000) << 16;
        s->dp_mix = (val & R128_GMC_ROP3_MASK) | (val & 0x7000000) >> 16;
        if (!(val & R128_GMC_SRC_PITCH_OFFSET_CNTL)) {
            s->src_offset = s->default_offset;
            s->src_pitch = s->default_pitch;
        }
        if (!(val & R128_GMC_DST_PITCH_OFFSET_CNTL)) {
            s->dst_offset = s->default_offset;
            s->dst_pitch = s->default_pitch;
        }
        if (!(val & R128_GMC_SRC_CLIPPING)) {
            s->src_sc_right = s->default_sc_right;
            s->src_sc_bottom = s->default_sc_bottom;
        }
        if (!(val & R128_GMC_DST_CLIPPING)) {
            s->sc_top = 0;
            s->sc_left = 0;
            s->sc_right = s->default_sc_right;
            s->sc_bottom = s->default_sc_bottom;
        }
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
        s->src_offset = val & 0xfffffff0;
        break;
    case R128_SRC_PITCH:
        s->src_pitch = val & 0x3fff;
        s->src_tile = (val >> 16) & 1;
        break;
    case R128_DP_BRUSH_BKGD_CLR:
        s->dp_brush_bkgd_clr = val;
        break;
    case R128_DP_BRUSH_FRGD_CLR:
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
        break;
    case R128_DP_MIX:
        s->dp_mix = val & 0x00ff0700;
        break;
    case R128_DP_WRITE_MASK:
        s->dp_write_mask = val;
        break;
    case R128_DEFAULT_OFFSET:
        s->default_offset = val & 0xfffffff0;
        break;
    case R128_DEFAULT_PITCH:
        s->default_pitch = val & 0x3fff;
        break;
    case R128_DEFAULT_SC_BOTTOM_RIGHT:
        s->default_sc_right = val & 0x3fff;
        s->default_sc_bottom = (val >> 16) & 0x3fff;
        break;
    case R128_SC_TOP_LEFT:
        s->sc_left = val & 0x3fff;
        s->sc_top = (val >> 16) & 0x3fff;
        break;
    case R128_SC_LEFT:
        s->sc_left = val & 0x3fff;
        break;
    case R128_SC_TOP:
        s->sc_top = val & 0x3fff;
        break;
    case R128_SC_BOTTOM_RIGHT:
        s->sc_right = val & 0x3fff;
        s->sc_bottom = (val >> 16) & 0x3fff;
        break;
    case R128_SC_RIGHT:
        s->sc_right = val & 0x3fff;
        break;
    case R128_SC_BOTTOM:
        s->sc_bottom = val & 0x3fff;
        break;
    case R128_SRC_SC_BOTTOM_RIGHT:
        s->src_sc_right = val & 0x3fff;
        s->src_sc_bottom = (val >> 16) & 0x3fff;
        break;
    case R128_SRC_SC_RIGHT:
        s->src_sc_right = val & 0x3fff;
        break;
    case R128_SRC_SC_BOTTOM:
        s->src_sc_bottom = val & 0x3fff;
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
                if (count >= 2) {
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
            case R128_PM4_OPCODE_BITBLT:
                if (count >= 3) {
                    uint32_t src_y_x = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_y_x = ati_rage128_pm4_read_ring(s);
                    uint32_t dst_h_w = ati_rage128_pm4_read_ring(s);

                    s->src_x = src_y_x & 0x3fff;
                    s->src_y = (src_y_x >> 16) & 0x3fff;
                    s->dst_x = dst_y_x & 0x3fff;
                    s->dst_y = (dst_y_x >> 16) & 0x3fff;
                    s->dst_width = dst_h_w & 0x3fff;
                    s->dst_height = (dst_h_w >> 16) & 0x3fff;
                    for (i = 3; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                    ati_rage128_2d_blt(s);
                } else {
                    for (i = 0; i < count; i++) {
                        ati_rage128_pm4_read_ring(s);
                    }
                }
                break;
            case R128_PM4_OPCODE_HOSTDATA_BLT:
                if (count >= 2) {
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
            if (p->p3_opcode != R128_PM4_OPCODE_PAINT &&
                p->p3_opcode != R128_PM4_OPCODE_BITBLT &&
                p->p3_opcode != R128_PM4_OPCODE_HOSTDATA_BLT) {
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
        switch (p->p3_opcode) {
        case R128_PM4_OPCODE_PAINT:
            if (p->p3_param_idx < 2) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 2) {
                    s->dst_x = p->p3_params[0] & 0x3fff;
                    s->dst_y = (p->p3_params[0] >> 16) & 0x3fff;
                    s->dst_width = p->p3_params[1] & 0x3fff;
                    s->dst_height = (p->p3_params[1] >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s);
                }
            }
            break;
        case R128_PM4_OPCODE_BITBLT:
            if (p->p3_param_idx < 3) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 3) {
                    s->src_x = p->p3_params[0] & 0x3fff;
                    s->src_y = (p->p3_params[0] >> 16) & 0x3fff;
                    s->dst_x = p->p3_params[1] & 0x3fff;
                    s->dst_y = (p->p3_params[1] >> 16) & 0x3fff;
                    s->dst_width = p->p3_params[2] & 0x3fff;
                    s->dst_height = (p->p3_params[2] >> 16) & 0x3fff;
                    ati_rage128_2d_blt(s);
                }
            }
            break;
        case R128_PM4_OPCODE_HOSTDATA_BLT:
            if (p->p3_param_idx < 2) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 2) {
                    s->dst_x = p->p3_params[0] & 0x3fff;
                    s->dst_y = (p->p3_params[0] >> 16) & 0x3fff;
                    s->dst_width = p->p3_params[1] & 0x3fff;
                    s->dst_height = (p->p3_params[1] >> 16) & 0x3fff;
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
        default:
            /* draw opcode payload / NOP overrun: discarded, not modeled */
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
        ati_rage128_pm4_parse(s, &parser,
            ati_rage128_card_read32(s, offset + i * 4, gart));
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
    /* keep the dirty-bitmap framebuffer scanner seeing these writes */
    memory_region_set_dirty(&s->vram, addr & ~7ull, 8);
}

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
}

type_init(ati_rage128_register_types)
