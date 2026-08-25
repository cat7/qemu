/*
 * QEMU ATI Radeon 9800 Pro (R350) emulation -- see ati_r350_int.h.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
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

#include "ati_r350_int.h"
#include "ati_r350_regs.h"
#include "trace.h"


#define ATI_R350_VBLANK_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)
#define ATI_R350_VBLANK_LEN_NS    (ATI_R350_VBLANK_PERIOD_NS / 8)
/*
 * How long the PCI IRQ line stays asserted after a VBLANK/VSYNC raise
 * if the guest never acknowledges GEN_INT_STATUS. Acks deassert the
 * line early (see ati_r350_update_irq()). Real silicon holds the
 * line until software acks; this cap only exists so a bring-up phase
 * that enables the interrupt without servicing it cannot storm -- the
 * line always drops before the next blank start so every frame still
 * yields exactly one fresh edge. Measured live (OS 9.1, OpenBIOS
 * mac99, OpenPIC level source): with the old 1/8-frame pulse the guest
 * missed ~23% of VBLs (its ISR/other interrupt work often exceeded
 * 2 ms, and a level source that goes low before it is taken is simply
 * lost), which was the direct cause of jerky mouse motion -- Mac OS
 * moves the hardware cursor from its VBL task.
 */
#define ATI_R350_VBLANK_IRQ_LEN_NS (ATI_R350_VBLANK_PERIOD_NS - \
                                       ATI_R350_VBLANK_LEN_NS)

/* ---------------------------------------------------------------- */
/* Display                                                          */

static uint32_t ati_r350_pixel_bytes(uint32_t pix_width)
{
    switch (pix_width) {
    case R350_PIX_WIDTH_8BPP:
        return 1;
    case R350_PIX_WIDTH_15BPP:
    case R350_PIX_WIDTH_16BPP:
        return 2;
    case R350_PIX_WIDTH_24BPP:
        return 3;
    case R350_PIX_WIDTH_32BPP:
        return 4;
    default:
        return 0; /* 4bpp and reserved codes: no linear fb path */
    }
}

static void ati_r350_get_mode(ATIR350State *s, ATIR350Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[R350_CRTC_GEN_CNTL >> 2];
    uint32_t h_total_disp = s->regs[R350_CRTC_H_TOTAL_DISP >> 2];
    uint32_t v_total_disp = s->regs[R350_CRTC_V_TOTAL_DISP >> 2];
    uint32_t pix_width = (crtc_gen_cntl >> R350_CRTC_PIX_WIDTH_SHIFT) &
                         R350_CRTC_PIX_WIDTH_MASK;

    memset(mode, 0, sizeof(*mode));

    mode->width = (((h_total_disp >> R350_CRTC_H_DISP_SHIFT) &
                    R350_CRTC_H_DISP_MASK) + 1) * 8;
    mode->height = ((v_total_disp >> R350_CRTC_V_DISP_SHIFT) &
                    R350_CRTC_V_DISP_MASK) + 1;
    mode->bpp = ati_r350_pixel_bytes(pix_width);
    /*
     * CRTC_PITCH is in units of 8 *pixels* for the display path (the
     * 24bpp bytes*8 exception applies to the render engine, not here
     * -- RRG-G04500-C 3.7 CRTC_PITCH note).
     */
    mode->pitch = ((s->regs[R350_CRTC_PITCH >> 2] & R350_CRTC_PITCH_MASK) * 8) *
                  (mode->bpp ? mode->bpp : 1);
    mode->fb_offset = s->regs[R350_CRTC_OFFSET >> 2] & R350_CRTC_OFFSET_MASK;
    /*
     * R300: the scanout address is DISPLAY_BASE_ADDR (a card address --
     * the Mac FCode sets it to the frame buffer's own aperture address)
     * plus CRTC_OFFSET. Fold the base into a frame-buffer offset when it
     * points inside the MC_FB_LOCATION window; a base of 0 (never
     * programmed) means "frame buffer start".
     */
    {
        uint32_t base = s->regs[R350_DISPLAY_BASE_ADDR >> 2];
        uint32_t off;

        if (base && ati_r350_mc_to_vram(s, base, &off)) {
            mode->fb_offset += off;
        }
    }
    mode->pix_width = pix_width;
}

static bool ati_r350_mode_valid(ATIR350State *s,
                                   const ATIR350Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[R350_CRTC_GEN_CNTL >> 2];
    uint32_t crtc_ext_cntl = s->regs[R350_CRTC_EXT_CNTL >> 2];

    if (!(crtc_gen_cntl & R350_CRTC_EN) ||
        !(crtc_gen_cntl & R350_CRTC_EXT_DISP_EN)) {
        return false;
    }
    if (crtc_ext_cntl & R350_CRTC_DISPLAY_DIS) {
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
        ATI_R350_VRAM_SIZE) {
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
 * Without this, ati_r350_update_display()'s own poll-based check
 * can miss that window completely and never learn the real
 * offset/pitch/dimensions the guest is actually using, even though
 * real content keeps landing there.
 */
static void ati_r350_maybe_capture_mode(ATIR350State *s)
{
    ATIR350Mode mode;

    ati_r350_get_mode(s, &mode);
    if (ati_r350_mode_valid(s, &mode)) {
        s->crtc_mode = mode;
        s->have_valid_mode = true;
        s->mode_dirty = true;
    }
}

/*
 * All drawing converts through an allocated 32bpp surface. VRAM bytes
 * are decoded CHIP-NATIVE LITTLE-ENDIAN: with the aperture-1 byte
 * swapper actually modeled (see ati_r350_aper1_ops), a big-endian
 * Mac guest's pixels land in VRAM in the chip's own layout, exactly
 * as on real hardware (verified live against Mac OS 9's lavender
 * desktop: bytes 9c 63 63 00 = LE B,G,R,X -- decoding them big-endian
 * was what tinted the desktop olive-green once the swapper existed;
 * the earlier BE decode had only ever looked right because the
 * missing swapper and the wrong decode cancelled out).
 */
/*
 * SURFACE_CNTL byte swappers. On real silicon a big-endian CPU store
 * through the aperture is byte-swapped on its way into VRAM (16- or
 * 32-bit wide, per the NONSURF_AP0_SWP bits, or per surface for the
 * eight SURFACEn ranges), so that the chip's little-endian consumers --
 * CRTC, 2D engine, CP -- see little-endian data. VRAM here is a plain
 * RAM region (stores land unswapped, for speed), so the swap is applied
 * on the consumer side instead: every reader/writer of VRAM bytes XORs
 * the byte address with the lane mask returned here, which is exactly
 * how the hardware's swapper is built (byte-lane steering). Mode 1 =
 * 16-bit swap = XOR 1, mode 3 = 32-bit swap = XOR 3.
 */
unsigned ati_r350_vram_xor(ATIR350State *s, uint32_t off)
{
    uint32_t cntl = s->regs[R350_SURFACE_CNTL >> 2];
    /*
     * The surfaces' bounds are FRAME-BUFFER APERTURE OFFSETS, not card
     * addresses: the swapper sits on the host-access side of the
     * aperture. Observed live under Mac OS 9: the ndrv declares its
     * 640x480x32 frame buffer as SURFACE7 = 0x10000..0x13bfff while
     * DISPLAY_BASE_ADDR / MC_FB_LOCATION put the same buffer at card
     * address 0x90010000 -- comparing against the card address matched
     * nothing, so no swap was applied and white came out yellow.
     */
    uint32_t mc = off;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t lo = s->regs[(R350_SURFACE0_LOWER_BOUND +
                               i * R350_SURFACE_STRIDE) >> 2];
        uint32_t hi = s->regs[(R350_SURFACE0_UPPER_BOUND +
                               i * R350_SURFACE_STRIDE) >> 2];
        uint32_t info = s->regs[(R350_SURFACE0_INFO +
                                 i * R350_SURFACE_STRIDE) >> 2];

        if (hi > lo && mc >= lo && mc <= hi) {
            return (info & R350_NONSURF_AP0_SWP_32BPP) ? 3 :
                   (info & R350_NONSURF_AP0_SWP_16BPP) ? 1 : 0;
        }
    }
    return (cntl & R350_NONSURF_AP0_SWP_32BPP) ? 3 :
           (cntl & R350_NONSURF_AP0_SWP_16BPP) ? 1 : 0;
}

uint32_t ati_r350_vram_ld32(ATIR350State *s, uint32_t off)
{
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    unsigned xr;

    if (off + 4 > ATI_R350_VRAM_SIZE) {
        return 0;
    }
    xr = ati_r350_vram_xor(s, off);
    return (uint32_t)vram[off ^ xr] |
           ((uint32_t)vram[(off + 1) ^ xr] << 8) |
           ((uint32_t)vram[(off + 2) ^ xr] << 16) |
           ((uint32_t)vram[(off + 3) ^ xr] << 24);
}

static void ati_r350_draw_8bpp(ATIR350State *s, DisplaySurface *ds,
                                  const ATIR350Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    unsigned xr = ati_r350_vram_xor(s, mode->fb_offset);
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            uint8_t idx = src[x ^ xr];
            dst[x] = 0xff000000u |
                     ((uint32_t)s->palette[idx][0] << 16) |
                     ((uint32_t)s->palette[idx][1] << 8) |
                     s->palette[idx][2];
        }
        src += mode->pitch;
    }
}

static void ati_r350_draw_16bpp(ATIR350State *s, DisplaySurface *ds,
                                   const ATIR350Mode *mode, bool rgb565)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    unsigned xr = ati_r350_vram_xor(s, mode->fb_offset);
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            uint16_t pixel = ((uint16_t)src[(2 * x + 1) ^ xr] << 8) |
                             src[(2 * x) ^ xr];
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

static void ati_r350_draw_32bpp(ATIR350State *s, DisplaySurface *ds,
                                   const ATIR350Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    unsigned xr = ati_r350_vram_xor(s, mode->fb_offset);
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            /*
             * Chip-native little-endian: B,G,R,X in VRAM. The DAC's
             * palette RAM acts as a per-channel gamma LUT in direct-
             * colour modes too -- OS X renders in linear light and
             * relies on the ramp it loads via PALETTE_30_DATA (the
             * accelerated desktop backdrop arrives as (8,21,55) and
             * only becomes the Aqua blue through the LUT).
             */
            dst[x] = 0xff000000u |
                     ((uint32_t)s->palette[src[(4 * x + 2) ^ xr]][0] << 16) |
                     ((uint32_t)s->palette[src[(4 * x + 1) ^ xr]][1] << 8) |
                     s->palette[src[(4 * x) ^ xr]][2];
        }
        src += mode->pitch;
    }
}

static void ati_r350_draw_24bpp(ATIR350State *s, DisplaySurface *ds,
                                   const ATIR350Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    unsigned xr = ati_r350_vram_xor(s, mode->fb_offset);
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            /* chip-native little-endian: B,G,R in VRAM; palette RAM
             * doubles as the per-channel gamma LUT, as in 32bpp */
            dst[x] = 0xff000000u |
                     ((uint32_t)s->palette[src[(3 * x + 2) ^ xr]][0] << 16) |
                     ((uint32_t)s->palette[src[(3 * x + 1) ^ xr]][1] << 8) |
                     s->palette[src[(3 * x) ^ xr]][2];
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
 * -- so ati_r350_maybe_capture_mode() only ever captures whatever
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
#define ATI_R350_FB_SCAN_PERIOD      30
#define ATI_R350_FB_ACTIVITY_HIT     12
#define ATI_R350_FB_ACTIVITY_THRESH  1

/*
 * Real Mac resolutions this hardware/driver combination has actually
 * been observed offering or using (see the mach64 Monitors panel's
 * own Resolution list, live-tested this session) -- dirty tracking
 * alone can find a byte span but can't recover its 2D geometry, so a
 * detected span gets matched against whichever of these implies the
 * closest total size.
 */
static const struct { uint32_t width, height; } ati_r350_known_modes[] = {
    { 640, 480 }, { 800, 600 }, { 832, 624 }, { 1024, 768 },
    { 1152, 870 }, { 1280, 960 }, { 1280, 1024 },
};

static uint32_t ati_r350_pix_width_from_bpp(uint32_t bpp)
{
    switch (bpp) {
    case 1:
        return R350_PIX_WIDTH_8BPP;
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
        return R350_PIX_WIDTH_15BPP;
    case 4:
        return R350_PIX_WIDTH_32BPP;
    default:
        return 0;
    }
}

static void ati_r350_pick_auto_fb(ATIR350State *s, int nblocks)
{
    int i, run_start = -1, best_start = -1, best_len = 0, cur_len = 0;
    uint32_t best_size, best_score = UINT32_MAX;
    static const uint32_t bpp_candidates[] = { 1, 2, 4 };
    unsigned bi, mi;
    ATIR350Mode candidate;
    bool found = false;

    for (i = 0; i <= nblocks; i++) {
        bool active = i < nblocks &&
                     s->fb_scan_activity[i] >= ATI_R350_FB_ACTIVITY_THRESH;

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
        trace_ati_r350_auto_fb(0, 0, 0, 0, s->auto_fb_valid, 0);
        s->auto_fb_pending_valid = false;
        return;
    }
    best_size = (uint32_t)best_len * ATI_R350_FB_SCAN_BLOCK;

    memset(&candidate, 0, sizeof(candidate));
    for (mi = 0; mi < ARRAY_SIZE(ati_r350_known_modes); mi++) {
        for (bi = 0; bi < ARRAY_SIZE(bpp_candidates); bi++) {
            uint32_t w = ati_r350_known_modes[mi].width;
            uint32_t h = ati_r350_known_modes[mi].height;
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
        trace_ati_r350_auto_fb((uint32_t)best_start *
                                  ATI_R350_FB_SCAN_BLOCK,
                                  0, 0, best_size, s->auto_fb_valid,
                                  best_score);
        s->auto_fb_pending_valid = false;
        return;
    }

    candidate.fb_offset = (uint32_t)best_start * ATI_R350_FB_SCAN_BLOCK;
    candidate.pix_width = ati_r350_pix_width_from_bpp(candidate.bpp);
    if ((uint64_t)candidate.fb_offset +
        (uint64_t)candidate.pitch * candidate.height > ATI_R350_VRAM_SIZE) {
        trace_ati_r350_auto_fb(candidate.fb_offset, candidate.width,
                                  candidate.height, candidate.bpp,
                                  s->auto_fb_valid, best_score);
        s->auto_fb_pending_valid = false;
        return;
    }

    /*
     * Stability gate: only expose a candidate for adoption once the
     * same one has come out of two consecutive scans -- see the field
     * comment on auto_fb_pending in ati_r350.h for the two churn
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
    trace_ati_r350_auto_fb(candidate.fb_offset, candidate.width,
                              candidate.height, candidate.bpp,
                              s->auto_fb_valid, best_score);
}

static void ati_r350_scan_vram_activity(ATIR350State *s)
{
    int nblocks = ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK;
    int i;

    if (++s->fb_scan_counter < ATI_R350_FB_SCAN_PERIOD) {
        return;
    }
    s->fb_scan_counter = 0;

    for (i = 0; i < nblocks; i++) {
        bool dirty = s->fb_block_pending[i];

        s->fb_block_pending[i] = false;
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
            s->fb_scan_activity[i] = ATI_R350_FB_ACTIVITY_HIT;
        } else if (s->fb_scan_activity[i] > 0) {
            s->fb_scan_activity[i]--;
        }
    }

    ati_r350_pick_auto_fb(s, nblocks);
}

/*
 * Consume the VRAM dirty bitmap once per refresh: note which scan blocks
 * were written (for the activity heuristic above) and return the snapshot
 * so the caller can ask whether the range it is about to display changed.
 */
static DirtyBitmapSnapshot *ati_r350_take_dirty(ATIR350State *s)
{
    int nblocks = ATI_R350_VRAM_SIZE / ATI_R350_FB_SCAN_BLOCK;
    DirtyBitmapSnapshot *snap;
    int i;

    snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0,
                                                   ATI_R350_VRAM_SIZE,
                                                   DIRTY_MEMORY_VGA);
    for (i = 0; i < nblocks; i++) {
        if (!s->fb_block_pending[i] &&
            memory_region_snapshot_get_dirty(&s->vram, snap,
                                             (hwaddr)i * ATI_R350_FB_SCAN_BLOCK,
                                             ATI_R350_FB_SCAN_BLOCK)) {
            s->fb_block_pending[i] = true;
        }
    }
    return snap;
}

static void ati_r350_cursor_update(ATIR350State *s);
static void ati_r350_cursor_apply(ATIR350State *s);

static bool ati_r350_update_display(void *opaque)
{
    ATIR350State *s = opaque;
    ATIR350Mode mode;
    DisplaySurface *ds;
    DirtyBitmapSnapshot *snap;
    bool valid, blanked, redraw;
    uint64_t fb_len;

    snap = ati_r350_take_dirty(s);
    ati_r350_get_mode(s, &mode);
    valid = ati_r350_mode_valid(s, &mode);
    /*
     * DISPLAY_DIS is the guest deliberately blanking the output (display
     * sleep, the blank phase of a mode-set), not a sign that CRTC1 was
     * never programmed. Real hardware shows nothing; we keep showing the
     * last frame -- but the framebuffer-guess heuristic below must not
     * get a say, because an idle, asleep guest paints nothing and its
     * framebuffer region looks exactly like the "dead CRTC" the
     * heuristic exists to route around (seen live: 5 min of idle under
     * OS X 10.3 -> Energy Saver blanks -> a stale 8bpp 800x600 buffer
     * replaced the desktop).
     */
    blanked = (s->regs[R350_CRTC_EXT_CNTL >> 2] & R350_CRTC_DISPLAY_DIS) != 0;
    trace_ati_r350_update(mode.width, mode.height, mode.bpp, valid,
                             mode.fb_offset);
    if (!valid) {
        if (!s->have_valid_mode) {
            g_free(snap);
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
        mode = s->crtc_mode;
    } else {
        s->crtc_mode = mode;
        s->have_valid_mode = true;
    }

    ati_r350_scan_vram_activity(s);
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
    if (!valid && !blanked && s->auto_fb_valid &&
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
        int first = mode.fb_offset / ATI_R350_FB_SCAN_BLOCK;
        int last = (mode.fb_offset + crtc_len - 1) / ATI_R350_FB_SCAN_BLOCK;
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
             i < (int)(ATI_R350_VRAM_SIZE /
                       ATI_R350_FB_SCAN_BLOCK); i++) {
            if (s->fb_scan_activity[i] >= ATI_R350_FB_ACTIVITY_THRESH) {
                crtc_region_live = true;
                break;
            }
        }
        if (!crtc_region_live) {
            mode = s->auto_fb_mode;
        }
        if (!crtc_region_live != s->auto_fb_overriding) {
            s->auto_fb_overriding = !crtc_region_live;
            trace_ati_r350_mode_override(s->auto_fb_overriding,
                                            mode.width, mode.height,
                                            mode.bpp, mode.fb_offset,
                                            mode.pitch);
        }
    } else if (s->auto_fb_overriding) {
        s->auto_fb_overriding = false;
        trace_ati_r350_mode_override(false, mode.width, mode.height,
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
    redraw = s->force_redraw;
    if (!ds || surface_width(ds) != (int)mode.width ||
        surface_height(ds) != (int)mode.height) {
        trace_ati_r350_surface_realloc(ds ? surface_width(ds) : 0,
                                          ds ? surface_height(ds) : 0,
                                          mode.width, mode.height, mode.bpp,
                                          mode.fb_offset, mode.pitch);
        ds = qemu_create_displaysurface(mode.width, mode.height);
        qemu_console_set_surface(s->con, ds);
        redraw = true;
    }
    /*
     * Only redraw -- and only hand the UI a new frame -- when something
     * that shapes the picture changed: the mode/framebuffer geometry, the
     * palette (force_redraw), or the displayed VRAM range itself (CPU
     * stores through either aperture, engine-drawn pixels and DMA all
     * land in the dirty bitmap). Re-pushing an identical 1024x768 frame
     * on every refresh tick was pure host load, and on macOS the stream
     * of redundant full-window redraws is what the intermittent darker
     * frames ("screen flicker") were made of.
     */
    if (memcmp(&s->mode, &mode, sizeof(mode)) != 0) {
        redraw = true;
    }
    fb_len = (uint64_t)mode.pitch * mode.height;
    if (fb_len && (uint64_t)mode.fb_offset + fb_len <= ATI_R350_VRAM_SIZE &&
        memory_region_snapshot_get_dirty(&s->vram, snap, mode.fb_offset,
                                         fb_len)) {
        redraw = true;
    }
    g_free(snap);
    s->mode = mode;
    s->mode_dirty = false;
    if (!redraw) {
        ati_r350_cursor_update(s);
        return true;
    }
    s->force_redraw = false;
    if (trace_event_get_state_backends(TRACE_ATI_R350_VRAM_PEEK)) {
        uint8_t *vp = (uint8_t *)memory_region_get_ram_ptr(&s->vram);
        uint32_t o5 = mode.fb_offset + 5 * mode.pitch;
        uint32_t o30 = mode.fb_offset + 30 * mode.pitch;
        trace_ati_r350_vram_peek(5, o5, ldl_le_p(vp + o5),
                                    ldl_le_p(vp + o5 + 4),
                                    ldl_le_p(vp + o5 + 8),
                                    ldl_le_p(vp + o5 + 12));
        trace_ati_r350_vram_peek(30, o30, ldl_le_p(vp + o30),
                                    ldl_le_p(vp + o30 + 4),
                                    ldl_le_p(vp + o30 + 8),
                                    ldl_le_p(vp + o30 + 12));
    }
    ds = qemu_console_surface(s->con);
    switch (mode.pix_width) {
    case R350_PIX_WIDTH_8BPP:
        ati_r350_draw_8bpp(s, ds, &mode);
        break;
    case R350_PIX_WIDTH_15BPP:
        ati_r350_draw_16bpp(s, ds, &mode, false);
        break;
    case R350_PIX_WIDTH_16BPP:
        ati_r350_draw_16bpp(s, ds, &mode, true);
        break;
    case R350_PIX_WIDTH_24BPP:
        ati_r350_draw_24bpp(s, ds, &mode);
        break;
    case R350_PIX_WIDTH_32BPP:
        ati_r350_draw_32bpp(s, ds, &mode);
        break;
    default:
        break;
    }
    ati_r350_cursor_apply(s);
    qemu_console_update_full(s->con);

    return true;
}

static const GraphicHwOps ati_r350_gfx_ops = {
    .gfx_update = ati_r350_update_display,
};

/* ---------------------------------------------------------------- */
/*
 * VBLANK interrupt: level held until acked (GEN_INT_STATUS is
 * write-1-to-clear and the line follows STATUS & CNTL), with a
 * per-frame fallback cap so a never-acking guest cannot storm -- see
 * ATI_R350_VBLANK_IRQ_LEN_NS.
 */

static void ati_r350_update_irq(ATIR350State *s)
{
    uint32_t pending = s->regs[R350_GEN_INT_STATUS >> 2] &
                       s->regs[R350_GEN_INT_CNTL >> 2] &
                       R350_GEN_INT_ACK_MASK;

    pci_set_irq(PCI_DEVICE(s), pending != 0);
}

static void ati_r350_vblank_end_tick(void *opaque)
{
    ATIR350State *s = opaque;

    /* Fallback only: the guest normally lowered the line long ago by
     * acking GEN_INT_STATUS (ati_r350_update_irq()). */
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void ati_r350_vblank_timer_tick(void *opaque)
{
    ATIR350State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next_blank;

    if (s->regs[R350_CRTC_GEN_CNTL >> 2] & R350_CRTC_EN) {
        s->regs[R350_GEN_INT_STATUS >> 2] |= R350_CRTC_VBLANK_INT |
                                             R350_CRTC_VSYNC_INT;
        /* CRTC_STATUS.CRTC_VBLANK_SAVE latches until cleared */
        s->regs[R350_CRTC_STATUS >> 2] |= R350_CRTC_VBLANK_SAVE;
        if (s->regs[R350_GEN_INT_CNTL >> 2] &
            (R350_CRTC_VBLANK_INT | R350_CRTC_VSYNC_INT)) {
            trace_ati_r350_vblank_irq(1,
                s->regs[R350_GEN_INT_CNTL >> 2]);
            pci_set_irq(PCI_DEVICE(s), 1);
            timer_mod(s->vblank_end_timer,
                      now + ATI_R350_VBLANK_IRQ_LEN_NS);
        }
    }

    /* Tick at each frame's blank-start phase (last 1/8 of the period),
     * agreeing with the phase-computed CRTC_VBLANK_CUR status bit. */
    next_blank = (now / ATI_R350_VBLANK_PERIOD_NS + 1) *
                 ATI_R350_VBLANK_PERIOD_NS - ATI_R350_VBLANK_LEN_NS;
    if (next_blank <= now) {
        next_blank += ATI_R350_VBLANK_PERIOD_NS;
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
#define TYPE_ATI_R350_DDC "ati-r350-ddc"
OBJECT_DECLARE_SIMPLE_TYPE(ATIR350DDCState, ATI_R350_DDC)

struct ATIR350DDCState {
    I2CSlave parent_obj;
    uint8_t reg;
    const uint8_t *edid;
};

static int ati_r350_ddc_event(I2CSlave *i2c, enum i2c_event event)
{
    return 0;
}

static uint8_t ati_r350_ddc_recv(I2CSlave *i2c)
{
    ATIR350DDCState *d = ATI_R350_DDC(i2c);

    return d->edid ? d->edid[d->reg++ & 0x7f] : 0xff;
}

static int ati_r350_ddc_send(I2CSlave *i2c, uint8_t data)
{
    ATI_R350_DDC(i2c)->reg = data;
    return 0;
}

static void ati_r350_ddc_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = ati_r350_ddc_event;
    k->recv = ati_r350_ddc_recv;
    k->send = ati_r350_ddc_send;
}

static const TypeInfo ati_r350_ddc_info = {
    .name = TYPE_ATI_R350_DDC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ATIR350DDCState),
    .class_init = ati_r350_ddc_class_init,
};

/* ---------------------------------------------------------------- */
/* Hardware I2C engine serving EDID (DDC addresses 0xA0/0xA1)       */

static void ati_r350_i2c_go(ATIR350State *s)
{
    uint32_t cntl0 = s->regs[R350_I2C_CNTL_0 >> 2];
    uint32_t cntl1 = s->regs[R350_I2C_CNTL_1 >> 2];
    uint8_t addr = (cntl1 >> R350_I2C_ADDR_SHIFT) & R350_I2C_ADDR_MASK;
    uint8_t count = (cntl1 >> R350_I2C_DATA_COUNT_SHIFT) &
                    R350_I2C_DATA_COUNT_MASK;
    int i;

    cntl0 &= ~(R350_I2C_DONE | R350_I2C_NACK | R350_I2C_GO);

    if ((addr & 0xfe) == 0xa0) {                /* DDC EDID slave */
        if (cntl0 & R350_I2C_RECEIVE) {
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
        cntl0 |= R350_I2C_DONE;
    } else {
        cntl0 |= R350_I2C_DONE | R350_I2C_NACK;
    }
    trace_ati_r350_i2c(addr, count, !!(cntl0 & R350_I2C_RECEIVE),
                          !!(cntl0 & R350_I2C_NACK), s->i2c_offset);

    s->regs[R350_I2C_CNTL_0 >> 2] = cntl0;
}

/* ---------------------------------------------------------------- */
/* Register file                                                    */

static int64_t ati_r350_beam_phase_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) %
           ATI_R350_VBLANK_PERIOD_NS;
}

static uint32_t ati_r350_reg_read32(ATIR350State *s, uint32_t base)
{
    PCIDevice *dev = PCI_DEVICE(s);
    uint32_t val = s->regs[base >> 2];

    switch (base) {
    case R350_CUR_OFFSET:
    case R350_CUR_HORZ_VERT_POSN:
    case R350_CUR_HORZ_VERT_OFF:
        /* the shared CUR_LOCK bit reads back through all three */
        if (s->cur_lock) {
            val |= R350_CUR_LOCK;
        }
        break;
    case R350_MM_DATA:
        val = 0;
        if (s->regs[R350_MM_INDEX >> 2] & R350_MM_APER) {
            uint32_t off = s->regs[R350_MM_INDEX >> 2] & 0x7ffffffc;

            if (off + 4 <= ATI_R350_VRAM_SIZE) {
                val = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->vram)
                               + off);
            }
        } else if ((s->regs[R350_MM_INDEX >> 2] & 0xfffc) != R350_MM_DATA) {
            val = ati_r350_reg_read32(s,
                s->regs[R350_MM_INDEX >> 2] & 0xfffc);
        }
        break;
    case R350_CLOCK_CNTL_DATA:
    {
        unsigned idx = s->regs[R350_CLOCK_CNTL_INDEX >> 2] &
                       R350_PLL_ADDR_MASK;

        val = s->plls[idx];
        /*
         * The PLL update is instant here, so the atomic-update
         * handshake bit always reads back clear (see
         * R350_PPLL_ATOMIC_UPDATE).
         */
        if ((idx >= R350_PLL_PPLL_REF_DIV && idx <= R350_PLL_PPLL_DIV_3) ||
            (idx >= R350_PLL_P2PLL_REF_DIV && idx <= R350_PLL_P2PLL_DIV_0 + 3)) {
            val &= ~R350_PPLL_ATOMIC_UPDATE;
        }
        trace_ati_r350_pll_read(idx, val);
        break;
    }
    case R350_CRTC_STATUS:
    {
        bool in_vblank = ati_r350_beam_phase_ns() >=
                         (ATI_R350_VBLANK_PERIOD_NS -
                          ATI_R350_VBLANK_LEN_NS);

        val = (val & ~(uint32_t)R350_CRTC_VBLANK_CUR) |
              (in_vblank ? R350_CRTC_VBLANK_CUR : 0) |
              R350_FIX_VSYNC_TIMING;
        break;
    }
    case R350_CRTC_VLINE_CRNT_VLINE:
    {
        /* current scanline in [31:16], trigger vline in [10:0] */
        uint32_t height = ((s->regs[R350_CRTC_V_TOTAL_DISP >> 2] >>
                            R350_CRTC_V_DISP_SHIFT) &
                           R350_CRTC_V_DISP_MASK) + 1;
        uint32_t line = ati_r350_beam_phase_ns() * height /
                        ATI_R350_VBLANK_PERIOD_NS;

        val = (val & 0x7ff) | ((line & 0x7ff) << 16);
        break;
    }
    case R350_CRTC_CRNT_FRAME:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
              ATI_R350_VBLANK_PERIOD_NS;
        break;
    case R350_DAC_CNTL:
        /*
         * DAC comparator: reports the sensed levels of a connected color
         * monitor (all three RGB lines terminated) on the primary DAC,
         * which feeds the ADC/VGA connector -- the second head here.
         */
        if (s->second_display) {
            val |= R350_DAC_CMP_OUTPUT;
        }
        break;
    case R350_GPIO_MONID:
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
                    (((s->regs[R350_CRTC_V_SYNC_STRT_WID >> 2] >> 23) & 1)
                     << 3);
            }
        } else if (s->monid7_i2c) {
            /*
             * ATIMM's I2C session on pads 1/2 under MASK 0x7 (see the
             * write handler): an open-drain bus, not a sense probe.
             * Released pads pull up (already in y); a released pad 1
             * carries the DDC slave's SDA. Pad 2 (SCL) must read high
             * when released -- the sense answer for "pad 1 driven low"
             * cleared it and stalled the master's clock-stretch wait.
             */
            if (!(en & 2)) {
                y = (y & ~2u) | (s->monid_sda ? 2u : 0);
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
        trace_ati_r350_monid_t(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  s->regs[base >> 2], y);
        break;
    }
    case R350_GPIO_VGA_DDC:
    case R350_GPIO_DVI_DDC: {
        /* driven pads read their own level, floating pads pull up, a
         * floating SDA carries the DDC slave's output */
        uint32_t en = (val >> 16) & 0xf;
        uint32_t a = val & 0xf;
        uint32_t y = (a & en) | (~en & 0xf);
        int sda = base == R350_GPIO_VGA_DDC ? s->vga_ddc_sda : s->dvi_ddc_sda;

        if (!(en & 1) && !sda) {
            y &= ~1u;
        }
        val = (val & ~(0xfu << 8)) | (y << 8);
        break;
    }
    case R350_GPIO_MONIDB: {
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
                (((s->regs[R350_CRTC_OFFSET >> 2] >> 23) & 1) << 3);
        }
        val = (val & ~(0xfu << 8)) | (y << 8);
        trace_ati_r350_monidb(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                 s->regs[base >> 2], y);
        break;
    }
    case R350_PALETTE_INDEX:
        val = s->dac_wr_index | ((uint32_t)s->dac_rd_index << 16);
        break;
    case R350_PALETTE_DATA:
        val = ((uint32_t)s->palette[s->dac_rd_index][0] << 16) |
              ((uint32_t)s->palette[s->dac_rd_index][1] << 8) |
              s->palette[s->dac_rd_index][2];
        s->dac_rd_index++;
        break;
    case R350_I2C_DATA:
        val = 0;
        if (s->i2c_data_pos < s->i2c_data_len) {
            val = s->i2c_data_fifo[s->i2c_data_pos++];
        }
        break;
    case R350_AMCGPIO_Y_MIR:
        /*
         * Same floating-pin pull-up default as GPIO_MONID above; the
         * FCode drives this mirror's byte lane [23:16] alongside
         * GPIO_MONID during the same probe.
         */
        val = (s->regs[R350_AMCGPIO_A_MIR >> 2] &
               s->regs[R350_AMCGPIO_EN_MIR >> 2]) |
              ~s->regs[R350_AMCGPIO_EN_MIR >> 2];
        trace_ati_r350_amcgpio(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  'Y', s->regs[R350_AMCGPIO_A_MIR >> 2],
                                  s->regs[R350_AMCGPIO_EN_MIR >> 2], val);
        break;
    case R350_CP_RB_BASE:
        val = s->pm4_buffer_addr;
        break;
    case R350_CP_RB_CNTL:
        val = s->pm4_buffer_cntl;
        break;
    case R350_CP_RB_RPTR:
        val = s->pm4_rptr;
        break;
    case R350_CP_RB_WPTR:
        val = s->pm4_wptr;
        break;
    case R350_PM4_MICROCODE_DATAH:
        val = s->pm4_microcode[s->pm4_ucode_raddr][0];
        break;
    case R350_PM4_MICROCODE_DATAL:
        val = s->pm4_microcode[s->pm4_ucode_raddr][1];
        s->pm4_ucode_raddr = (s->pm4_ucode_raddr + 1) &
                             (R350_PM4_MICROCODE_WORDS - 1);
        break;
    case R350_CP_STAT:
    case R350_CP_CSQ_STAT:
        /* command processor idle, queues empty */
        val = 0;
        break;
    case R350_RBBM_STATUS:
        /* engine idle: FIFO fully free, RBBM_ACTIVE and all busy bits clear */
        val = 0x40 & R350_RBBM_FIFOCNT_MASK;
        break;
    case R350_RB2D_DSTCACHE_CTLSTAT:
    case R350_RB3D_DSTCACHE_CTLSTAT:
    case R350_RB3D_ZCACHE_CTLSTAT:
        /* cache flush/idle handshakes: never busy (bit 31) */
        val = s->regs[base >> 2] & 0x7fffffff;
        break;
    case R350_MC_IND_DATA:
        val = s->mc_ind[s->regs[R350_MC_IND_INDEX >> 2] & 0xff];
        break;
    case R350_PALETTE_30_DATA:
        val = ((uint32_t)s->palette[s->dac_rd_index][0] << 22) |
              ((uint32_t)s->palette[s->dac_rd_index][1] << 12) |
              ((uint32_t)s->palette[s->dac_rd_index][2] << 2);
        s->dac_rd_index++;
        break;
    case R350_CONFIG_MEMSIZE:
        val = ATI_R350_VRAM_SIZE;
        break;
    case R350_CONFIG_APER_0_BASE:
        val = pci_get_long(dev->config + PCI_BASE_ADDRESS_0) &
              PCI_BASE_ADDRESS_MEM_MASK;
        break;
    case R350_CONFIG_APER_1_BASE:
        val = 0;
        break;
    case R350_CONFIG_APER_SIZE:
        val = ATI_R350_APER_SIZE;
        break;
    case R350_CONFIG_REG_1_BASE:
        val = pci_get_long(dev->config + PCI_BASE_ADDRESS_2) &
              PCI_BASE_ADDRESS_MEM_MASK;
        break;
    case R350_CONFIG_REG_APER_SIZE:
        val = ATI_R350_MMIO_SIZE;
        break;
    case R350_CONFIG_XSTRAP:
        val = R350_XSTRAP_ADDIN_CARD;
        break;
    case R350_GEN_STATUS:
    case R350_CONFIG_BONDS:
        val = 0;
        break;
    case R350_SW_SEMAPHORE:
        /* report all 8 semaphores free (bit reads as acquired-ok) */
        val = 0xff;
        break;
    case R350_MEM_STR_CNTL:
        val = s->regs[base >> 2];
        break;
    case R350_PC_NGUI_CTLSTAT:
        /* pixel cache idle: BUSY (bit 31) clear */
        val = s->regs[base >> 2] & 0x3fffffff;
        break;
    case R350_GUI_STAT:
        /* engine idle, all 64 command FIFO entries free */
        val = 0x40;
        break;
    case R350_DST_OFFSET:
        val = s->dst_offset_reg;
        break;
    case R350_DST_PITCH:
        val = s->dst_pitch_reg | (s->dst_tile_reg << 16);
        break;
    case R350_DST_WIDTH:
        val = s->dst_width;
        break;
    case R350_DST_HEIGHT:
        val = s->dst_height;
        break;
    case R350_SRC_X:
        val = s->src_x;
        break;
    case R350_SRC_Y:
        val = s->src_y;
        break;
    case R350_DST_X:
        val = s->dst_x;
        break;
    case R350_DST_Y:
        val = s->dst_y;
        break;
    case R350_DP_GUI_MASTER_CNTL:
        /* aliases fields from DP_MIX and DP_DATATYPE -- see the write case */
        val = s->dp_gui_master_cntl |
              ((s->dp_datatype & R350_DP_BRUSH_DATATYPE) >> 4) |
              ((s->dp_datatype & R350_DP_DST_DATATYPE) << 8) |
              ((s->dp_datatype & R350_DP_SRC_DATATYPE) >> 4) |
              (s->dp_mix & R350_DP_ROP3) |
              ((s->dp_mix & R350_DP_SRC_SOURCE) << 16);
        break;
    /*
     * The packed PITCH_OFFSET form and the separate PITCH/OFFSET
     * registers are two views of one piece of state on real silicon, so
     * a read through either has to see what was written through the
     * other. Mac OS X programs the separate registers exclusively, which
     * left the packed ones reading a permanent zero.
     */
    case R350_SRC_PITCH_OFFSET:
        val = (s->src_offset_reg >> 5) |
              (s->src_pitch_reg << R350_PITCH_OFFSET_PITCH_SHIFT) |
              (s->src_tile_reg << 31);
        break;
    case R350_DST_PITCH_OFFSET:
    case R350_DST_PITCH_OFFSET_C:
        val = (s->dst_offset_reg >> 5) |
              (s->dst_pitch_reg << R350_PITCH_OFFSET_PITCH_SHIFT) |
              (s->dst_tile_reg << 31);
        break;
    case R350_SRC_OFFSET:
        val = s->src_offset_reg;
        break;
    case R350_SRC_PITCH:
        val = s->src_pitch_reg | (s->src_tile_reg << 16);
        break;
    case R350_DP_BRUSH_BKGD_CLR:
        val = s->dp_brush_bkgd_clr;
        break;
    case R350_DP_BRUSH_FRGD_CLR:
        val = s->dp_brush_frgd_clr;
        break;
    case R350_DP_SRC_FRGD_CLR:
        val = s->dp_src_frgd_clr;
        break;
    case R350_DP_SRC_BKGD_CLR:
        val = s->dp_src_bkgd_clr;
        break;
    case R350_DP_CNTL_XDIR_YDIR_YMAJOR:
        /*
         * The same two directions as DP_CNTL, in different bit
         * positions. Keep DP_CNTL as the canonical copy so the 2D engine
         * has one place to look.
         */
        s->dp_cntl &= ~(R350_DST_X_LEFT_TO_RIGHT | R350_DST_Y_TOP_TO_BOTTOM);
        if (val & R350_DST_X_DIR_LEFT_TO_RIGHT) {
            s->dp_cntl |= R350_DST_X_LEFT_TO_RIGHT;
        }
        if (val & R350_DST_Y_DIR_TOP_TO_BOTTOM) {
            s->dp_cntl |= R350_DST_Y_TOP_TO_BOTTOM;
        }
        break;
    case R350_DP_CNTL:
        val = s->dp_cntl;
        break;
    case R350_DP_DATATYPE:
        val = s->dp_datatype;
        break;
    case R350_DP_MIX:
        val = s->dp_mix;
        break;
    case R350_DP_WRITE_MASK:
        val = s->dp_write_mask;
        break;
    case R350_DEFAULT_OFFSET:
        val = s->default_offset;
        break;
    case R350_DEFAULT_PITCH:
        val = s->default_pitch;
        break;
    case R350_DEFAULT_SC_BOTTOM_RIGHT:
        val = s->default_sc_right | (s->default_sc_bottom << 16);
        break;
    case R350_SC_TOP:
        val = s->sc_top_reg;
        break;
    case R350_SC_LEFT:
        val = s->sc_left_reg;
        break;
    case R350_SC_BOTTOM:
        val = s->sc_bottom_reg;
        break;
    case R350_SC_RIGHT:
        val = s->sc_right_reg;
        break;
    case R350_SRC_SC_BOTTOM:
        val = s->src_sc_bottom_reg;
        break;
    case R350_SRC_SC_RIGHT:
        val = s->src_sc_right_reg;
        break;
    case R350_CFG_MIRROR_BASE ... R350_CFG_MIRROR_END:
        val = pci_default_read_config(dev, base - R350_CFG_MIRROR_BASE, 4);
        break;
    default:
        break;
    }
    return val;
}

static void ati_r350_bm_gui_run(ATIR350State *s, uint32_t table);
static void ati_r350_pm4_run(ATIR350State *s);
static void ati_r350_pm4_run_ring(ATIR350State *s);
static void ati_r350_scratch_writeback(ATIR350State *s, unsigned n);
static void ati_r350_pm4_fifo_push(ATIR350State *s, uint32_t val);
static void ati_r350_pm4_indirect(ATIR350State *s, uint32_t offset,
                                     uint32_t dwords);
static void ati_r350_pm4_parse(ATIR350State *s,
                                  ATIR350PM4Parser *p, uint32_t val);

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
static void ati_r350_resolve_gui_context(ATIR350State *s)
{
    uint32_t gmc = s->dp_gui_master_cntl;

    if (gmc & R350_GMC_SRC_PITCH_OFFSET_CNTL) {
        s->src_offset = s->src_offset_reg;
        s->src_pitch = s->src_pitch_reg;
        s->src_pitch_bytes = s->src_pitch_bytes_reg;
        s->src_tile = s->src_tile_reg;
    } else {
        s->src_offset = s->default_offset;
        s->src_pitch = s->default_pitch;
        s->src_pitch_bytes = true;
        s->src_tile = 0;
    }

    if (gmc & R350_GMC_DST_PITCH_OFFSET_CNTL) {
        s->dst_offset = s->dst_offset_reg;
        s->dst_pitch = s->dst_pitch_reg;
        s->dst_pitch_bytes = s->dst_pitch_bytes_reg;
        s->dst_tile = s->dst_tile_reg;
    } else {
        s->dst_offset = s->default_offset;
        s->dst_pitch = s->default_pitch;
        s->dst_pitch_bytes = true;
        s->dst_tile = 0;
    }

    if (gmc & R350_GMC_SRC_CLIPPING) {
        s->src_sc_right = s->src_sc_right_reg;
        s->src_sc_bottom = s->src_sc_bottom_reg;
    } else {
        s->src_sc_right = s->default_sc_right;
        s->src_sc_bottom = s->default_sc_bottom;
    }

    if (gmc & R350_GMC_DST_CLIPPING) {
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

static void ati_r350_reg_write32(ATIR350State *s, uint32_t base,
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
    case R350_SRC_OFFSET:
    case R350_SRC_PITCH:
    case R350_SRC_PITCH_OFFSET:
    case R350_DST_OFFSET:
    case R350_DST_PITCH:
    case R350_DST_PITCH_OFFSET:
    case R350_DST_PITCH_OFFSET_C:
    case R350_DEFAULT_OFFSET:
    case R350_DEFAULT_PITCH:
    case R350_DP_GUI_MASTER_CNTL:
    case R350_DP_GUI_MASTER_CNTL_C:
        trace_ati_r350_ctx_write(ati_r350_reg_name(base), base, val);
        break;
    default:
        break;
    }

    switch (base) {
    case R350_MM_INDEX:
        s->regs[base >> 2] = val;
        break;
    case R350_MM_DATA:
        if (s->regs[R350_MM_INDEX >> 2] & R350_MM_APER) {
            uint32_t off = s->regs[R350_MM_INDEX >> 2] & 0x7ffffffc;
            uint8_t *vram = memory_region_get_ram_ptr(&s->vram);

            if (off + 4 <= ATI_R350_VRAM_SIZE) {
                stl_le_p(vram + off, val);
                memory_region_set_dirty(&s->vram, off, 4);
            }
        } else if ((s->regs[R350_MM_INDEX >> 2] & 0xfffc) != R350_MM_DATA) {
            ati_r350_reg_write32(s, s->regs[R350_MM_INDEX >> 2] & 0xfffc,
                                    val);
        }
        break;
    case R350_CLOCK_CNTL_INDEX:
        s->regs[base >> 2] = val;
        break;
    case R350_CLOCK_CNTL_DATA:
    {
        unsigned idx = s->regs[R350_CLOCK_CNTL_INDEX >> 2] &
                       R350_PLL_ADDR_MASK;

        if (s->regs[R350_CLOCK_CNTL_INDEX >> 2] & R350_PLL_WR_EN) {
            s->plls[idx] = val;
            trace_ati_r350_pll_write(idx, val);
        }
        break;
    }
    case R350_GEN_INT_CNTL:
        s->regs[base >> 2] = val;
        ati_r350_update_irq(s);
        break;
    case R350_GEN_INT_STATUS:
        /* write-1-to-acknowledge */
        s->regs[base >> 2] &= ~(val & R350_GEN_INT_ACK_MASK);
        trace_ati_r350_int_ack(val, s->regs[base >> 2]);
        ati_r350_update_irq(s);
        break;
    case R350_CRTC_GEN_CNTL:
        s->regs[base >> 2] = val;
        s->mode_dirty = true;
        trace_ati_r350_mode_reg(ati_r350_reg_name(base), val);
        ati_r350_maybe_capture_mode(s);
        ati_r350_cursor_update(s);   /* carries CRTC_CUR_EN */
        break;
    case R350_CRTC_EXT_CNTL:
    case R350_CRTC_H_TOTAL_DISP:
    case R350_CRTC_V_TOTAL_DISP:
    case R350_CRTC_PITCH:
        s->regs[base >> 2] = val;
        s->mode_dirty = true;
        trace_ati_r350_mode_reg(ati_r350_reg_name(base), val);
        ati_r350_maybe_capture_mode(s);
        break;
    case R350_CUR_OFFSET:
    case R350_CUR_HORZ_VERT_POSN:
    case R350_CUR_HORZ_VERT_OFF:
        /*
         * CUR_LOCK is ONE bit aliased into bit 31 of all three registers
         * (RRG 3-80/81); the last write to any of them sets or clears it,
         * and unlocking is what applies a locked shape/position update
         * atomically. OR-ing bit 31 of the three stored copies together
         * (the old model) stayed locked for good whenever a driver locked
         * through one register and unlocked through another -- and a
         * locked cursor here silently drops every later update, which is
         * how OS X 10.3's cursor vanished after a shape change.
         */
        s->cur_lock = (val & R350_CUR_LOCK) != 0;
        s->regs[base >> 2] = val & ~R350_CUR_LOCK;
        trace_ati_r350_cur_reg(ati_r350_reg_name(base), val,
                                  s->cur_lock);
        ati_r350_cursor_update(s);
        break;
    case R350_CUR_CLR0:
    case R350_CUR_CLR1:
        s->regs[base >> 2] = val;
        trace_ati_r350_cur_reg(ati_r350_reg_name(base), val,
                                  s->cur_lock);
        ati_r350_cursor_update(s);
        break;
    case R350_CRTC_OFFSET:
        s->regs[base >> 2] = val & (R350_CRTC_OFFSET_MASK |
                                    R350_CRTC_OFFSET_LOCK);
        s->mode_dirty = true;
        ati_r350_maybe_capture_mode(s);
        break;
    case R350_CRTC_STATUS:
        /* write 1 to bit 1 clears CRTC_VBLANK_SAVE */
        if (val & R350_CRTC_VBLANK_SAVE) {
            s->regs[base >> 2] &= ~(uint32_t)R350_CRTC_VBLANK_SAVE;
        }
        break;
    case R350_CRTC_V_SYNC_STRT_WID: {
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
        if (((s->regs[R350_GPIO_MONID >> 2] >> 24) & 0xf) == 0xf &&
            !(old & (1u << 23)) && (val & (1u << 23))) {
            s->ddc1_half ^= 1;
            if (!s->ddc1_half) {
                s->ddc1_pos = (s->ddc1_pos + 1) % (sizeof(s->edid) * 9);
            }
        }
        break;
    }
    case R350_PALETTE_INDEX:
        s->dac_wr_index = val & 0xff;
        s->dac_rd_index = (val >> 16) & 0xff;
        break;
    case R350_PALETTE_DATA:
        s->palette[s->dac_wr_index][0] = (val >> 16) & 0xff;  /* R */
        s->palette[s->dac_wr_index][1] = (val >> 8) & 0xff;   /* G */
        s->palette[s->dac_wr_index][2] = val & 0xff;          /* B */
        s->force_redraw = true;
        s->dac_wr_index++;
        break;
    case R350_I2C_CNTL_0:
        s->regs[base >> 2] = val;
        if (val & R350_I2C_SOFT_RST) {
            s->i2c_data_len = 0;
            s->i2c_data_pos = 0;
            s->regs[base >> 2] = (val & ~(R350_I2C_SOFT_RST | R350_I2C_GO)) |
                                 R350_I2C_DONE;
        } else if (val & R350_I2C_GO) {
            ati_r350_i2c_go(s);
        }
        break;
    case R350_I2C_DATA:
        if (s->i2c_data_len < (int)sizeof(s->i2c_data_fifo)) {
            s->i2c_data_fifo[s->i2c_data_len++] = val & 0xff;
        }
        break;
    case R350_GEN_RESET_CNTL:
        s->regs[base >> 2] = val;
        if (val & R350_SOFT_RESET_GUI) {
            /* engine soft reset also aborts any half-parsed command
             * stream state (matches the driver's reset-then-restart
             * expectation) */
            s->pm4_fifo.remaining = 0;
            s->pm4_ring.remaining = 0;
            s->pm4_rptr = 0;
            s->pm4_wptr = 0;
        }
        break;
    case R350_BM_GUI_TABLE:
        s->regs[base >> 2] = val;
        ati_r350_bm_gui_run(s, val);
        break;
    case R350_CP_RB_BASE:
        s->pm4_buffer_addr = val;
        s->regs[base >> 2] = val;
        break;
    case R350_CP_RB_CNTL:
    {
        /* RB_BUFSZ = log2 of the ring size in qwords */
        uint32_t l2qw = val & R350_RB_BUFSZ_MASK;

        s->pm4_buffer_cntl = val;
        s->regs[base >> 2] = val;
        s->pm4_ring_dwords = l2qw ? 2u << l2qw : 0;
        /* reconfiguring the ring aborts any half-parsed packet */
        s->pm4_ring.remaining = 0;
        trace_ati_r350_cp_rb_cntl(val, s->pm4_ring_dwords,
                                  !!(val & R350_RB_NO_UPDATE),
                                  (val & R350_BUF_SWAP_MASK) >> 16);
        break;
    }
    case R350_CP_RB_RPTR:
    case R350_CP_RB_RPTR_WR:
        s->pm4_rptr = s->pm4_ring_dwords ?
                      val & (s->pm4_ring_dwords - 1) : val;
        break;
    case R350_CP_RB_WPTR:
        s->pm4_wptr = s->pm4_ring_dwords ?
                      val & (s->pm4_ring_dwords - 1) : val;
        s->regs[base >> 2] = val;
        ati_r350_pm4_run(s);
        break;
    case R350_CP_IB_BASE:
        s->regs[base >> 2] = val;
        break;
    case R350_CP_IB_BUFSZ:
        s->regs[base >> 2] = val;
        ati_r350_pm4_indirect(s, s->regs[R350_CP_IB_BASE >> 2] & ~3u,
                              val & 0x1ffff);
        break;
    case R350_PM4_MICROCODE_ADDR:
        s->pm4_ucode_waddr = val & (R350_PM4_MICROCODE_WORDS - 1);
        break;
    case R350_PM4_MICROCODE_RADDR:
        s->pm4_ucode_raddr = val & (R350_PM4_MICROCODE_WORDS - 1);
        break;
    case R350_PM4_MICROCODE_DATAH:
        s->pm4_microcode[s->pm4_ucode_waddr][0] = val;
        break;
    case R350_PM4_MICROCODE_DATAL:
        s->pm4_microcode[s->pm4_ucode_waddr][1] = val;
        s->pm4_ucode_waddr = (s->pm4_ucode_waddr + 1) &
                             (R350_PM4_MICROCODE_WORDS - 1);
        break;
    case R350_CP_CSQ_CNTL:
    case R350_CP_CSQ_MODE:
    case R350_CP_ME_CNTL:
    case R350_CP_RB_WPTR_DELAY:
    case R350_CP_RB_RPTR_ADDR:
    case R350_SCRATCH_UMSK:
    case R350_SCRATCH_ADDR:
        s->regs[base >> 2] = val;
        trace_ati_r350_cp_reg(ati_r350_reg_name(base), val);
        break;
    case R350_SCRATCH_REG_BASE ... R350_SCRATCH_REG_LAST:
        s->regs[base >> 2] = val;
        ati_r350_scratch_writeback(s, (base - R350_SCRATCH_REG_BASE) >> 2);
        break;
    case R300_VAP_PVS_UPLOAD_ADDRESS:
        s->regs[base >> 2] = val;
        s->pvs_upload_addr = val;
        s->pvs_upload_cnt = 0;
        break;
    case R300_VAP_PVS_UPLOAD_DATA:
    {
        /*
         * Dwords land at (selected vec4 slot * 4 + how many have
         * arrived since the address was set), in the one flat space
         * that holds instructions below R300_PVS_CONST_START and the
         * constant file from there up.
         */
        uint32_t off = s->pvs_upload_addr * 4 + s->pvs_upload_cnt;

        s->regs[base >> 2] = val;
        s->pvs_upload_cnt++;
        if (s->pvs_upload_addr < R300_PVS_CONST_START) {
            if (off < ARRAY_SIZE(s->pvs_code)) {
                s->pvs_code[off] = val;
                if (off + 1 > s->pvs_code_dwords) {
                    s->pvs_code_dwords = off + 1;
                }
            }
        } else {
            off -= R300_PVS_CONST_START * 4;
            if (off < ARRAY_SIZE(s->pvs_const)) {
                s->pvs_const[off] = val;
                if (off + 1 > s->pvs_const_dwords) {
                    s->pvs_const_dwords = off + 1;
                }
            }
        }
        break;
    }
    case R350_MC_IND_INDEX:
        s->regs[base >> 2] = val;
        break;
    case R350_MC_IND_DATA:
        s->mc_ind[s->regs[R350_MC_IND_INDEX >> 2] & 0xff] = val;
        trace_ati_r350_mc_ind(s->regs[R350_MC_IND_INDEX >> 2] & 0xff, val);
        break;
    case R350_PALETTE_30_DATA:
        /* 10 bits per component, B[9:0] G[19:10] R[29:20] */
        s->palette[s->dac_wr_index][0] = (val >> 22) & 0xff;
        s->palette[s->dac_wr_index][1] = (val >> 12) & 0xff;
        s->palette[s->dac_wr_index][2] = (val >> 2) & 0xff;
        s->force_redraw = true;
        s->dac_wr_index++;
        break;
    case R350_SURFACE_CNTL:
    case R350_SURFACE0_LOWER_BOUND ... R350_SURFACE7_INFO:
        s->regs[base >> 2] = val;
        s->force_redraw = true;
        trace_ati_r350_surface(ati_r350_reg_name(base), val);
        break;
    case R350_PM4_FIFO_DATA_EVEN ... R350_PM4_FIFO_APER_END:
        /*
         * Any dword in the CCE FIFO aperture is a push. Mac OS's driver
         * writes packets as bursts across 0x1000..0x101c; taking only
         * EVEN/ODD lost the tail of every burst -- typically the
         * DST_Y_X/DST_HEIGHT_WIDTH pair that arms a host-data blit, so
         * the HOST_DATA stream that followed (menu bar restore, window
         * icons, drag save-behind) was thrown away.
         */
        ati_r350_pm4_fifo_push(s, val);
        break;
    case R350_DST_OFFSET:
        s->dst_offset_reg = val & 0xfffffff0;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DST_PITCH:
        s->dst_pitch_reg = val & 0x3fff;
        s->dst_tile_reg = (val >> 16) & 1;
        s->dst_pitch_bytes_reg = true;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DST_WIDTH:
        s->dst_width = val & 0x3fff;
        ati_r350_2d_blt(s);
        break;
    case R350_DST_HEIGHT:
        s->dst_height = val & 0x3fff;
        break;
    case R350_SRC_X:
        s->src_x = val & 0x3fff;
        break;
    case R350_SRC_Y:
        s->src_y = val & 0x3fff;
        break;
    case R350_DST_X:
        s->dst_x = val & 0x3fff;
        break;
    case R350_DST_Y:
        s->dst_y = val & 0x3fff;
        break;
    case R350_SRC_PITCH_OFFSET:
        /*
         * Radeon packed layout (R5xx accel guide / DRM): offset in
         * [21:0] as multiples of 1KB, pitch in [29:22] as multiples
         * of 64 bytes, tiling flags on top. (The Rage 128 packed
         * fields this device started from were offset/32 and pitch in
         * 8-pixel units -- live captures of OS X's scroll copies,
         * e.g. DEFAULT_OFFSET 0x4b002658 for the 704px surface at
         * 0x996000, decode only under the Radeon rule.)
         */
        s->src_offset_reg = (val & 0x3fffff) << 10;
        s->src_pitch_reg = ((val >> 22) & 0xff) * 64;
        s->src_pitch_bytes_reg = true;
        s->src_tile_reg = val >> 30;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DST_PITCH_OFFSET:
    case R350_DST_PITCH_OFFSET_C:
        /* Radeon packed layout, as for SRC_PITCH_OFFSET above */
        s->dst_offset_reg = (val & 0x3fffff) << 10;
        s->dst_pitch_reg = ((val >> 22) & 0xff) * 64;
        s->dst_pitch_bytes_reg = true;
        s->dst_tile_reg = val >> 30;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SRC_Y_X:
        s->src_x = val & 0x3fff;
        s->src_y = (val >> 16) & 0x3fff;
        break;
    case R350_DST_Y_X:
        s->dst_x = val & 0x3fff;
        s->dst_y = (val >> 16) & 0x3fff;
        break;
    case R350_DST_HEIGHT_WIDTH:
        s->dst_width = val & 0x3fff;
        s->dst_height = (val >> 16) & 0x3fff;
        ati_r350_2d_blt(s);
        break;
    case R350_SCALE_DST_HEIGHT_WIDTH:
        /* the register-programmed scaler's kick: parameters were stored
         * by the writes before it */
        s->regs[base >> 2] = val;
        ati_r350_2d_scale_regs(s);
        break;
    case R350_DP_GUI_MASTER_CNTL:
    case R350_DP_GUI_MASTER_CNTL_C:
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
        s->dp_datatype = (s->dp_datatype & ~R350_DP_DATATYPE_GMC_ALIAS) |
                         (val & 0x0f00) >> 8 | (val & 0x30f0) << 4 |
                         (val & 0x4000) << 16;
        s->dp_mix = (val & R350_GMC_ROP3_MASK) | (val & 0x7000000) >> 16;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DST_WIDTH_X:
        s->dst_x = val & 0x3fff;
        s->dst_width = (val >> 16) & 0x3fff;
        ati_r350_2d_blt(s);
        break;
    case R350_SRC_X_Y:
        s->src_y = val & 0x3fff;
        s->src_x = (val >> 16) & 0x3fff;
        break;
    case R350_DST_X_Y:
        s->dst_y = val & 0x3fff;
        s->dst_x = (val >> 16) & 0x3fff;
        break;
    case R350_DST_WIDTH_HEIGHT:
        s->dst_height = val & 0x3fff;
        s->dst_width = (val >> 16) & 0x3fff;
        ati_r350_2d_blt(s);
        break;
    case R350_DST_HEIGHT_Y:
        s->dst_y = val & 0x3fff;
        s->dst_height = (val >> 16) & 0x3fff;
        break;
    case R350_SRC_OFFSET:
        s->src_offset_reg = val & 0xfffffff0;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SRC_PITCH:
        s->src_pitch_reg = val & 0x3fff;
        s->src_tile_reg = (val >> 16) & 1;
        s->src_pitch_bytes_reg = true;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DP_BRUSH_BKGD_CLR:
        s->dp_brush_bkgd_clr = val;
        break;
    case R350_DP_BRUSH_FRGD_CLR:
    case R350_CONSTANT_COLOR_C:
        s->dp_brush_frgd_clr = val;
        trace_ati_r350_brush_clr(base, val);
        break;
    case R350_DP_CNTL:
        s->dp_cntl = val;
        break;
    case R350_DP_SRC_FRGD_CLR:
        s->dp_src_frgd_clr = val;
        break;
    case R350_DP_SRC_BKGD_CLR:
        s->dp_src_bkgd_clr = val;
        break;
    case R350_DP_DATATYPE:
        s->dp_datatype = val & 0xe0070f0f;
        trace_ati_r350_datatype(val, !!(val & R350_HOST_BIG_ENDIAN_EN));
        break;
    case R350_DP_MIX:
        s->dp_mix = val & 0x00ff0700;
        break;
    case R350_DP_WRITE_MASK:
    case R350_PLANE_3D_MASK_C:
        s->dp_write_mask = val;
        break;
    case R350_DEFAULT_OFFSET:
        /*
         * On Radeon this is DEFAULT_PITCH_OFFSET, packed like
         * SRC/DST_PITCH_OFFSET: OS X's scroll copies run with the GMC
         * defaults selected and only this register armed.
         */
        s->default_offset = (val & 0x3fffff) << 10;
        s->default_pitch = ((val >> 22) & 0xff) * 64;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DEFAULT_PITCH:
        s->default_pitch = val & 0x3fff;
        ati_r350_resolve_gui_context(s);
        break;
    case R350_DEFAULT_SC_BOTTOM_RIGHT:
        s->default_sc_right = sextract32(val, 0, 14);
        s->default_sc_bottom = sextract32(val, 16, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_TOP_LEFT:
    case R350_SC_TOP_LEFT_C:
        s->sc_left_reg = sextract32(val, 0, 14);
        s->sc_top_reg = sextract32(val, 16, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_LEFT:
        s->sc_left_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_TOP:
        s->sc_top_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_BOTTOM_RIGHT:
    case R350_SC_BOTTOM_RIGHT_C:
        s->sc_right_reg = sextract32(val, 0, 14);
        s->sc_bottom_reg = sextract32(val, 16, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_RIGHT:
        s->sc_right_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SC_BOTTOM:
        s->sc_bottom_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SRC_SC_BOTTOM_RIGHT:
        s->src_sc_right_reg = sextract32(val, 0, 14);
        s->src_sc_bottom_reg = sextract32(val, 16, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SRC_SC_RIGHT:
        s->src_sc_right_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_SRC_SC_BOTTOM:
        s->src_sc_bottom_reg = sextract32(val, 0, 14);
        ati_r350_resolve_gui_context(s);
        break;
    case R350_HOST_DATA0:
    case R350_HOST_DATA1:
    case R350_HOST_DATA2:
    case R350_HOST_DATA3:
    case R350_HOST_DATA4:
    case R350_HOST_DATA5:
    case R350_HOST_DATA6:
    case R350_HOST_DATA7:
    case R350_HOST_DATA_LAST:
        if (!s->host_data_active) {
            break;
        }
        s->host_data_acc[s->host_data_next++] = val;
        if (base == R350_HOST_DATA_LAST) {
            ati_r350_host_data_flush(s);
            s->host_data_active = false;
            s->host_data_next = 0;
        } else if (s->host_data_next >= 4) {
            ati_r350_host_data_flush(s);
            s->host_data_next = 0;
        }
        break;
    case R350_CFG_MIRROR_BASE ... R350_CFG_MIRROR_END:
        /* read-only mirror of PCI config space */
        break;
    case R350_GPIO_MONID: {
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
            trace_ati_r350_monid_wr(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                       val, scl, sda, s->monid_sda);
        } else if (((val >> 24) & 0xf) == 0x7) {
            /*
             * Mac OS 9's ATI Resource Manager (ATIMM, the memory manager
             * shared library the ATI suite loads at boot) bit-bangs I2C
             * on pads 1 (SDA) and 2 (SCL) with the MASK nibble at 0x7 --
             * the same value the Apple-sense probes use. Answering its
             * START with the sense codes pulled the released SCL pad low
             * ("sense1 low" -> Y2 = 0), so its clock-stretch wait spun
             * ~74k reads per attempt; the long timeouts left the library
             * wedged and the Finder's event loop blocked behind it (mouse
             * moved, nothing else landed). Track the session here and let
             * the read handler answer as an open-drain bus while it is
             * open. A sense probe pulses a single pad and never clocks,
             * so it keeps its sense answers.
             */
            uint32_t en = (val >> 16) & 0xf;
            uint32_t a = val & 0xf;
            bool sda_low = (en & 2) && !(a & 2);
            bool scl_low = (en & 4) && !(a & 4);

            if (oldmask != 0x7) {
                s->monid7_i2c = false;
                s->monid7_start = false;
            }
            if (sda_low && !s->monid7_sda_low && !scl_low) {
                /* SDA fell with SCL released: START, or a sense1 pulse */
                s->monid7_start = true;
            } else if (!sda_low && s->monid7_sda_low && !scl_low) {
                /* SDA rose with SCL released: STOP, or the pulse ending */
                s->monid7_i2c = false;
                s->monid7_start = false;
            } else if (scl_low && !s->monid7_scl_low && s->monid7_start) {
                /* the clock follows the START: a real I2C session */
                s->monid7_i2c = true;
                s->monid7_start = false;
            }
            s->monid7_sda_low = sda_low;
            s->monid7_scl_low = scl_low;
            bitbang_i2c_set(&s->monid_i2c, BITBANG_I2C_SCL, !scl_low);
            s->monid_sda = bitbang_i2c_set(&s->monid_i2c, BITBANG_I2C_SDA,
                                           !sda_low);
            trace_ati_r350_monid_wr(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                       val, !scl_low, !sda_low, s->monid_sda);
        }
        break;
    }
    case R350_GPIO_VGA_DDC:
    case R350_GPIO_DVI_DDC: {
        /*
         * Open-drain DDC pads: SDA = pad 0, SCL = pad 1; a pad drives its
         * A level while EN is set and floats high otherwise.
         */
        bitbang_i2c_interface *i2c = base == R350_GPIO_VGA_DDC ?
                                     &s->vga_ddc_i2c : &s->dvi_ddc_i2c;
        int *sda_out = base == R350_GPIO_VGA_DDC ?
                       &s->vga_ddc_sda : &s->dvi_ddc_sda;
        uint32_t en = (val >> 16) & 0xf;
        uint32_t a = val & 0xf;
        int scl = (en & 2) ? !!(a & 2) : 1;
        int sda = (en & 1) ? !!(a & 1) : 1;

        s->regs[base >> 2] = val;
        bitbang_i2c_set(i2c, BITBANG_I2C_SCL, scl);
        *sda_out = bitbang_i2c_set(i2c, BITBANG_I2C_SDA, sda);
        trace_ati_r350_ddc_pad(base, val, scl, sda, *sda_out);
        break;
    }
    case R350_GPIO_MONIDB:
        s->regs[base >> 2] = val;
        trace_ati_r350_monidb_wr(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                    val);
        break;
    case R350_AMCGPIO_A_MIR:
    case R350_AMCGPIO_EN_MIR:
    case R350_AMCGPIO_MASK_MIR:
        s->regs[base >> 2] = val;
        trace_ati_r350_amcgpio(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                  base == R350_AMCGPIO_A_MIR ? 'A' :
                                  base == R350_AMCGPIO_EN_MIR ? 'E' : 'M',
                                  s->regs[R350_AMCGPIO_A_MIR >> 2],
                                  s->regs[R350_AMCGPIO_EN_MIR >> 2], val);
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
 * test -- see the comment on R350_BM_GUI_TABLE in ati_r350_regs.h).
 * The transfer completes synchronously, one dword at a time through the
 * normal register-write path so a descriptor can target any register
 * exactly as if the driver had written it directly via MM_INDEX/DATA.
 */
static void ati_r350_bm_gui_run(ATIR350State *s, uint32_t table)
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
        trace_ati_r350_bm_desc(reg_off, sysaddr, ctrl);

        while (count >= 4) {
            uint32_t word;

            if (pci_dma_read(pci, sysaddr, &word, 4) != MEMTX_OK) {
                break;
            }
            ati_r350_reg_write32(s, reg_off & 0xfffc, le32_to_cpu(word));
            sysaddr += 4;
            reg_off += 4;
            count -= 4;
        }

        if (ctrl & (1u << 31)) {
            break;
        }
        desc += 12;
    }

    s->regs[R350_GEN_INT_STATUS >> 2] |= R350_BUSMASTER_EOL_INT;
    ati_r350_update_irq(s);
}

/*
 * Read one dword from the PM4 ring at the current read pointer and
 * advance it, wrapping at the ring size. The ring lives in VRAM (see
 * the comment on R350_PM4_BUFFER_OFFSET) so this is a direct pointer
 * read, not a DMA -- consistent with every other VRAM access in this
 * file.
 */
/*
 * Read one little-endian dword from card address space: local VRAM
 * below ATI_R350_VRAM_SIZE, otherwise the 32MB GART-translated
 * "VM" window (see the R350_PCIGART_TABLE_ENTRIES comment in
 * ati_r350_regs.h). Command streams are little-endian regardless
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
/*
 * Card ("MC") address space. Everything the engine and the command
 * processor fetch or draw to is a 32-bit card address that the memory
 * controller routes by window: MC_FB_LOCATION [31:16] = top, [15:0] =
 * base of the frame buffer window (in 64KB units), MC_AGP_LOCATION
 * likewise for the AGP window (translated by AGP_BASE + the host
 * bridge's GART), and anything outside both goes straight to the PCI
 * bus as a host address. The Mac ROMs put the frame buffer window at
 * the card's PCI aperture address, so a frame-buffer card address is
 * numerically the same as its PCI bus address -- either way the window
 * test below resolves it locally first.
 */
bool ati_r350_mc_to_vram(ATIR350State *s, uint32_t addr,
                                uint32_t *off)
{
    uint32_t loc = s->regs[R350_MC_FB_LOCATION >> 2];
    uint32_t lo = (loc & 0xffff) << 16;
    uint32_t hi = (loc & 0xffff0000) | 0xffff;

    if (addr >= lo && addr <= hi && addr - lo < ATI_R350_VRAM_SIZE) {
        *off = addr - lo;
        return true;
    }
    return false;
}

static bool ati_r350_mc_to_agp(ATIR350State *s, uint32_t addr,
                               dma_addr_t *bus)
{
    uint32_t loc = s->regs[R350_MC_AGP_LOCATION >> 2];
    uint32_t lo = (loc & 0xffff) << 16;
    uint32_t hi = (loc & 0xffff0000) | 0xffff;

    if (lo <= hi && addr >= lo && addr <= hi) {
        *bus = (s->regs[R350_AGP_BASE >> 2] & ~0xfffu) + (addr - lo);
        return true;
    }
    return false;
}

/*
 * Read one little-endian dword from card address space. Command streams
 * are little-endian regardless of guest CPU endianness (big-endian Mac
 * drivers byte-swap on the way out, or stage them through a swapped
 * surface -- which ati_r350_vram_ld32() honours).
 */
uint32_t ati_r350_mc_read32(ATIR350State *s, uint32_t addr)
{
    uint32_t off, val = 0;
    dma_addr_t bus;

    if (ati_r350_mc_to_vram(s, addr, &off)) {
        return ati_r350_vram_ld32(s, off);
    }
    if (!ati_r350_mc_to_agp(s, addr, &bus)) {
        bus = addr;
    }
    pci_dma_read(PCI_DEVICE(s), bus, &val, sizeof(val));
    return le32_to_cpu(val);
}

static void ati_r350_mc_write32(ATIR350State *s, uint32_t addr, uint32_t val)
{
    uint32_t off;
    dma_addr_t bus;

    if (ati_r350_mc_to_vram(s, addr, &off)) {
        uint8_t *vram = memory_region_get_ram_ptr(&s->vram);

        if (off + 4 <= ATI_R350_VRAM_SIZE) {
            stl_le_p(vram + off, val);
            memory_region_set_dirty(&s->vram, off, 4);
        }
        return;
    }
    if (!ati_r350_mc_to_agp(s, addr, &bus)) {
        bus = addr;
    }
    val = cpu_to_le32(val);
    pci_dma_write(PCI_DEVICE(s), bus, &val, sizeof(val));
}

/*
 * The CP's memory write-backs: the ring read pointer (CP_RB_RPTR_ADDR,
 * unless RB_NO_UPDATE) and any scratch register whose SCRATCH_UMSK bit
 * is set (to SCRATCH_ADDR + 4*n). Drivers poll these in memory rather
 * than reading the registers, so a missing write-back is a silent
 * driver hang.
 */
/* classic 16x16 arrow: data = black pixels, mask = opaque area */
static const uint16_t ati_r350_arrow_data[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00,
    0x7f80, 0x7c00, 0x6c00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
};
static const uint16_t ati_r350_arrow_mask[16] = {
    0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00, 0xff80,
    0xffc0, 0xffe0, 0xfe00, 0xef00, 0xcf00, 0x8780, 0x0780, 0x0380,
};

static QEMUCursor *ati_r350_builtin_arrow(void)
{
    QEMUCursor *c = cursor_alloc(16, 16);
    int row, col;

    for (row = 0; row < 16; row++) {
        for (col = 0; col < 16; col++) {
            uint16_t bit = 0x8000 >> col;

            if (!(ati_r350_arrow_mask[row] & bit)) {
                c->data[row * 16 + col] = 0;            /* transparent */
            } else if (ati_r350_arrow_data[row] & bit) {
                c->data[row * 16 + col] = 0xff000000u;  /* black */
            } else {
                c->data[row * 16 + col] = 0xffffffffu;  /* white edge */
            }
        }
    }
    return c;
}

static void ati_r350_cp_rptr_writeback(ATIR350State *s)
{
    uint32_t cntl = s->regs[R350_CP_RB_CNTL >> 2];
    uint32_t addr = s->regs[R350_CP_RB_RPTR_ADDR >> 2] & ~3u;

    if (!(cntl & R350_RB_NO_UPDATE) && addr) {
        ati_r350_mc_write32(s, addr, s->pm4_rptr);
    }
}

static void ati_r350_scratch_writeback(ATIR350State *s, unsigned n)
{
    uint32_t umsk = s->regs[R350_SCRATCH_UMSK >> 2];
    uint32_t addr = s->regs[R350_SCRATCH_ADDR >> 2] & ~3u;

    if ((umsk & (1u << n)) && addr) {
        ati_r350_mc_write32(s, addr + n * 4,
                            s->regs[(R350_SCRATCH_REG_BASE >> 2) + n]);
    }
}

static uint32_t ati_r350_pm4_read_ring(ATIR350State *s)
{
    uint32_t base = s->regs[R350_CP_RB_BASE >> 2] & ~3u;
    uint32_t val;

    val = ati_r350_mc_read32(s, base + s->pm4_rptr * 4);
    /*
     * BUF_SWAP_32BIT: the driver writes the ring in its own (big-endian)
     * byte order and the CP swaps each dword on fetch.
     */
    if ((s->regs[R350_CP_RB_CNTL >> 2] & R350_BUF_SWAP_MASK) ==
        R350_BUF_SWAP_32BIT) {
        val = bswap32(val);
    }
    s->pm4_rptr = (s->pm4_rptr + 1) & (s->pm4_ring_dwords - 1);
    return val;
}

/*
 * Consume PM4 ring entries from the read pointer up to the write
 * pointer, synchronously (matching the BM engine's own synchronous
 * model). Packet format: see the comment on R350_PM4_BUFFER_OFFSET in
 * ati_r350_regs.h.
 */
static void ati_r350_pm4_run(ATIR350State *s)
{
    if (!s->pm4_ring_dwords) {
        return;
    }
    ati_r350_pm4_run_ring(s);
    ati_r350_cp_rptr_writeback(s);
}

/*
 * Every ring dword goes through the same packet state machine as the
 * PIO FIFO and the indirect buffers, so a packet behaves identically
 * no matter how it was submitted. The parser state persists in
 * s->pm4_ring because the driver can bump the write pointer
 * mid-packet: the remainder arrives with a later WPTR write. (The
 * previous inline ring parser instead read payload dwords straight
 * past the write pointer, consuming ring slots the driver had not
 * written yet.)
 */
static void ati_r350_pm4_run_ring(ATIR350State *s)
{
    int guard;

    for (guard = 0; guard < 1000000 && s->pm4_rptr != s->pm4_wptr;
         guard++) {
        uint32_t pos = s->pm4_rptr;
        uint32_t val = ati_r350_pm4_read_ring(s);

        trace_ati_r350_pm4_ring_dword(pos, val);
        ati_r350_pm4_parse(s, &s->pm4_ring, val);
    }
}

/*
 * PIO alternative to the ring above: the real driver actually pushes
 * its command stream straight through PM4_FIFO_DATA_EVEN/ODD (see the
 * comment on those in ati_r350_regs.h), one dword per write. Same
 * packet format as the ring, just delivered by MMIO write instead of
 * being pulled from VRAM, so the state (in-flight packet type/count/
 * running register) has to live across calls instead of a loop index.
 */
static void ati_r350_pm4_parse(ATIR350State *s,
                                  ATIR350PM4Parser *p, uint32_t val)
{
    if (p->remaining == 0) {
        /* val is a new packet header */
        p->type = R350_PM4_PACKET_TYPE(val);

        switch (p->type) {
        case 0:
            p->reg = R350_PM4_PACKET0_REG(val);
            p->one_reg = R350_PM4_PACKET0_ONE_REG(val);
            p->remaining = R350_PM4_PACKET_COUNT(val);
            break;
        case 1:
            p->p1_reg1 = R350_PM4_PACKET1_REG1(val);
            p->p1_reg2 = R350_PM4_PACKET1_REG2(val);
            p->remaining = 2;
            break;
        case 2:
            /* NOP/padding, no data words */
            break;
        case 3:
            p->remaining = R350_PM4_PACKET_COUNT(val);
            p->p3_opcode = R350_PM4_PACKET3_OPCODE(val);
            p->p3_param_idx = 0;
            p->p3_total = p->remaining;
            trace_ati_r350_pm4_p3_hdr(p->p3_opcode, p->remaining);
            if (p->p3_opcode != R350_PM4_OPCODE_PAINT &&
                p->p3_opcode != R350_PM4_OPCODE_PAINT_MULTI &&
                p->p3_opcode != R350_PM4_OPCODE_BITBLT &&
                p->p3_opcode != R350_PM4_OPCODE_BITBLT_RECT &&
                p->p3_opcode != R350_PM4_OPCODE_BITBLT_MULTI &&
                p->p3_opcode != R350_PM4_OPCODE_HOSTDATA_BLT &&
                p->p3_opcode != R350_PM4_OPCODE_SCALING &&
                p->p3_opcode != R300_PM4_OPCODE_NOP3 &&
                p->p3_opcode != R300_PM4_OPCODE_CLEAR_ZMASK &&
                p->p3_opcode != R300_PM4_OPCODE_CLEAR_HIZ &&
                p->p3_opcode != R300_PM4_OPCODE_CLEAR_CMASK &&
                p->p3_opcode != R300_PM4_OPCODE_LOAD_VBPNTR &&
                p->p3_opcode != R300_PM4_OPCODE_DRAW_IMMD_2 &&
                p->p3_opcode != R300_PM4_OPCODE_DRAW_VBUF_2) {
                trace_ati_r350_pm4_unimp(p->p3_opcode, p->remaining);
                ati_r350_note_gap(s, R350_GAP_P3_OPCODE, p->p3_opcode);
            }
            break;
        }
        return;
    }

    /* val is a data dword for the packet currently in flight */
    switch (p->type) {
    case 0:
        trace_ati_r350_pm4_reg(p->reg & 0xfffc,
                                  ati_r350_reg_name(p->reg & 0xfffc), val);
        ati_r350_reg_write32(s, p->reg & 0xfffc, val);
        if (!p->one_reg) {
            p->reg += 4;
        }
        break;
    case 1:
        if (p->remaining == 2) {
            trace_ati_r350_pm4_reg(p->p1_reg1 & 0xfffc,
                ati_r350_reg_name(p->p1_reg1 & 0xfffc), val);
            ati_r350_reg_write32(s, p->p1_reg1 & 0xfffc, val);
        } else {
            trace_ati_r350_pm4_reg(p->p1_reg2 & 0xfffc,
                ati_r350_reg_name(p->p1_reg2 & 0xfffc), val);
            ati_r350_reg_write32(s, p->p1_reg2 & 0xfffc, val);
        }
        break;
    case 3:
        trace_ati_r350_p3_payload(p->p3_opcode, p->p3_param_idx, val);
        switch (p->p3_opcode) {
        case R350_PM4_OPCODE_PAINT:
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
            unsigned want = (p->p3_params[0] & R350_GMC_LD_BRUSH_Y_X) &&
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

                    ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL,
                                            p->p3_params[0]);
                    ati_r350_reg_write32(s, R350_DP_BRUSH_FRGD_CLR,
                                            p->p3_params[1]);
                    if (want == 7) {
                        ati_r350_reg_write32(s, R350_BRUSH_DATA0,
                                                p->p3_params[2]);
                        ati_r350_reg_write32(s, R350_BRUSH_DATA0 + 4,
                                                p->p3_params[3]);
                        ati_r350_reg_write32(s, R350_BRUSH_Y_X,
                                                p->p3_params[4]);
                    }
                    if (x2 > x1 && y2 > y1) {
                        s->dst_x = x1;
                        s->dst_y = y1;
                        s->dst_width = x2 - x1;
                        s->dst_height = y2 - y1;
                        trace_ati_r350_paint_multi(0, tl, br, s->dst_x,
                                                      s->dst_y, s->dst_width,
                                                      s->dst_height);
                        ati_r350_2d_blt(s);
                    }
                }
            } else if (p->p3_param_idx == 2) {
                s->dst_x = p->p3_params[0] & 0x3fff;
                s->dst_y = (p->p3_params[0] >> 16) & 0x3fff;
                s->dst_width = p->p3_params[1] & 0x3fff;
                s->dst_height = (p->p3_params[1] >> 16) & 0x3fff;
                ati_r350_2d_blt(s);
            }
            break;
        }
        case R350_PM4_OPCODE_PAINT_MULTI:
            /*
             * [0] context, [1] colour, then (DST_X_Y, DST_WIDTH_HEIGHT)
             * pairs -- and unlike PAINT's corners, X and WIDTH sit in
             * the HIGH half with Y and HEIGHT in the low one, matching
             * the registers of the same names. Established from a live
             * capture of Mac OS drawing a dialog: successive packets
             * walk y = 0,1,2,3 with widths 5,3,2,1 at x = 0 and
             * x = 1019..1023 on a 1024-wide screen -- a window's
             * rounded corners, pixel row by pixel row. Reading the
             * halves the other way round made every one of them
             * zero-sized, which is why button frames never appeared.
             */
            if (p->p3_param_idx == 0) {
                /*
                 * A context with DST_PITCH_OFFSET_CNTL set is followed
                 * by the destination pitch/offset dword, then the
                 * colour (Linux's r128 DRM clear packet); Mac OS 9's
                 * context (0x72f036d0) has the bit clear and goes
                 * straight to the colour.
                 */
                ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL, val);
                p->p3_params[3] = (val & R350_GMC_DST_PITCH_OFFSET_CNTL) ?
                                  1 : 0;
                p->p3_param_idx = 1;
            } else if (p->p3_param_idx == 1 && p->p3_params[3]) {
                ati_r350_reg_write32(s, R350_DST_PITCH_OFFSET, val);
                p->p3_params[3] = 0;
            } else if (p->p3_param_idx == 1) {
                ati_r350_reg_write32(s, R350_DP_BRUSH_FRGD_CLR, val);
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
                trace_ati_r350_paint_multi(0, dst_x_y, val, s->dst_x,
                                              s->dst_y, s->dst_width,
                                              s->dst_height);
                if (s->dst_width && s->dst_height) {
                    ati_r350_2d_blt(s);
                }
                p->p3_param_idx = 2;   /* next rectangle pair */
            }
            break;
        case R350_PM4_OPCODE_BITBLT_MULTI:
            /*
             * The save-under half of a window drag: a context dword,
             * the pitch/offset dwords the context itself announces
             * (its SRC/DST_PITCH_OFFSET_CNTL bits, in SRC-then-DST
             * order, as Linux's r128 DRM builds its swap packet), and
             * then a RUN of three-dword rectangles -- the MULTI is
             * not decoration: iTunes tiles a rounded-corner window
             * with nine rectangles in one 29-dword packet, and
             * stopping after the first copied a 1-pixel strip and
             * dropped the 600x390 body. Mac OS X 10.4 sets both
             * pitch/offset bits for its 16x16 pointer save/restore;
             * reading that as one dword plus a rectangle turned the
             * restore into "corruption growing from the top-left
             * towards the mouse".
             *
             * The rectangle run is longer than p3_params, so each
             * three-dword rectangle is gathered in place and issued as
             * soon as it completes rather than buffering the packet.
             * p3_params[3] counts the pitch/offset dwords the context
             * announced, so the rectangle run is known to start at
             * index 1 + that count.
             */
            if (p->p3_param_idx == 0) {
                ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL, val);
                p->p3_params[3] = 0;
                if (val & R350_GMC_SRC_PITCH_OFFSET_CNTL) {
                    p->p3_params[3]++;
                }
                if (val & R350_GMC_DST_PITCH_OFFSET_CNTL) {
                    p->p3_params[3]++;
                }
                p->p3_params[4] = val;
            } else if (p->p3_param_idx <= p->p3_params[3]) {
                bool src_first = p->p3_params[4] &
                                 R350_GMC_SRC_PITCH_OFFSET_CNTL;

                if (p->p3_param_idx == 1 && src_first) {
                    ati_r350_reg_write32(s, R350_SRC_PITCH_OFFSET, val);
                } else {
                    ati_r350_reg_write32(s, R350_DST_PITCH_OFFSET, val);
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
                    ati_r350_2d_blt(s);
                }
            }
            p->p3_param_idx++;
            break;

        case R350_PM4_OPCODE_BITBLT:
            /*
             * Four dwords: context, SRC_X_Y, DST_X_Y,
             * DST_WIDTH_HEIGHT -- and, like the registers of those
             * names (and unlike PAINT's corners), X and WIDTH sit in
             * the HIGH half with Y and HEIGHT in the low one.
             * Established from a live capture of Mac OS dragging a
             * window between screens, where each move issues three
             * blits that must tile the window exactly; only this
             * reading makes them do so. Reading three dwords from
             * index 0 took the CONTEXT dword for the source point, so
             * every window copy fetched its pixels from a nonsense
             * position -- corruption that then travelled with the
             * window, since this is the path that moves its bits.
             */
            if (p->p3_param_idx < 4) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 4) {
                    ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL,
                                            p->p3_params[0]);
                    s->src_y = p->p3_params[1] & 0x3fff;
                    s->src_x = (p->p3_params[1] >> 16) & 0x3fff;
                    s->dst_y = p->p3_params[2] & 0x3fff;
                    s->dst_x = (p->p3_params[2] >> 16) & 0x3fff;
                    s->dst_height = p->p3_params[3] & 0x3fff;
                    s->dst_width = (p->p3_params[3] >> 16) & 0x3fff;
                    ati_r350_2d_blt(s);
                }
            }
            break;
        case R350_PM4_OPCODE_BITBLT_RECT:
            /*
             * One SRC_X_Y/DST_X_Y/DST_WIDTH_HEIGHT rectangle in
             * BITBLT's field layout, drawing context inherited from
             * the registers -- see the define for the capture that
             * established it. No GMC dword, so no register writes
             * here.
             */
            if (p->p3_param_idx < 3) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == 3) {
                    s->src_y = p->p3_params[0] & 0x3fff;
                    s->src_x = (p->p3_params[0] >> 16) & 0x3fff;
                    s->dst_y = p->p3_params[1] & 0x3fff;
                    s->dst_x = (p->p3_params[1] >> 16) & 0x3fff;
                    s->dst_height = p->p3_params[2] & 0x3fff;
                    s->dst_width = (p->p3_params[2] >> 16) & 0x3fff;
                    ati_r350_2d_blt(s);
                }
            }
            break;
        case R350_PM4_OPCODE_SCALING:
            /* fixed-length packet: see R350_SCALE_PKT_* in the header */
            if (p->p3_param_idx < R350_SCALE_PKT_DWORDS) {
                p->p3_scale[p->p3_param_idx++] = val;
                if (p->p3_param_idx == R350_SCALE_PKT_DWORDS) {
                    ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL,
                                    p->p3_scale[R350_SCALE_PKT_GMC]);
                    ati_r350_reg_write32(s, R350_SC_TOP_LEFT,
                                    p->p3_scale[R350_SCALE_PKT_SC_TL]);
                    ati_r350_reg_write32(s, R350_SC_BOTTOM_RIGHT,
                                    p->p3_scale[R350_SCALE_PKT_SC_BR]);
                    ati_r350_2d_scale(s, p->p3_scale);
                }
            }
            break;
        case R350_PM4_OPCODE_HOSTDATA_BLT:
        {
            /*
             * Header dwords before the pixel data. Two long layouts
             * exist -- Linux's r128 DRM emits seven header dwords
             * (context, pitch/offset, write mask, clip, position,
             * size, count), the Mac driver eight, with one extra
             * dword before the position -- and the last header dword
             * is the pixel-dword count, which must equal what is left
             * of the packet: that identity tells the two apart
             * without guessing.
             */
            uint32_t nhdr = p->p3_total >= 8 ? 8 : 2;

            if (p->p3_param_idx < nhdr) {
                p->p3_params[p->p3_param_idx++] = val;
                if (p->p3_param_idx == nhdr) {
                    uint32_t real = nhdr;
                    uint32_t yx, hw;

                    if (nhdr == 8) {
                        real = p->p3_params[7] == p->p3_total - 8 ? 8 :
                               p->p3_params[6] == p->p3_total - 7 ? 7 : 8;
                        ati_r350_reg_write32(s, R350_DP_GUI_MASTER_CNTL,
                                                p->p3_params[0]);
                    }
                    if (real == 8) {
                        /*
                         * Context first, then the clip: the driver
                         * pads the blit width up to a 4-pixel boundary
                         * and expects the clip to discard the surplus,
                         * and a GMC write with DST_CLIPPING clear
                         * resets the scissors.
                         */
                        ati_r350_reg_write32(s, R350_SC_TOP_LEFT,
                                                p->p3_params[1]);
                        ati_r350_reg_write32(s, R350_SC_BOTTOM_RIGHT,
                                                p->p3_params[2]);
                    }
                    yx = nhdr == 2 ? p->p3_params[0]
                                   : p->p3_params[real - 3];
                    hw = nhdr == 2 ? p->p3_params[1]
                                   : p->p3_params[real - 2];
                    s->dst_x = yx & 0x3fff;
                    s->dst_y = (yx >> 16) & 0x3fff;
                    s->dst_width = hw & 0x3fff;
                    s->dst_height = (hw >> 16) & 0x3fff;
                    ati_r350_2d_blt(s); /* enters host-data mode */
                    if (nhdr == 8 && real == 7 && s->host_data_active) {
                        /* the 8th dword already read is pixel data */
                        s->host_data_acc[0] = p->p3_params[7];
                        s->host_data_next = 1;
                    }
                }
            } else if (s->host_data_active) {
                s->host_data_acc[s->host_data_next++] = val;
                if (s->host_data_next >= 4) {
                    ati_r350_host_data_flush(s);
                    s->host_data_next = 0;
                }
            }
            break;
        }
        case R300_PM4_OPCODE_DRAW_IMMD_2:
            if (p->p3_param_idx < ARRAY_SIZE(s->r300_immd)) {
                s->r300_immd[p->p3_param_idx] = val;
            }
            p->p3_param_idx++;
            break;
        case R300_PM4_OPCODE_DRAW_VBUF_2:
            /* single payload dword: VAP_VF_CNTL for an AOS-array draw */
            if (p->p3_param_idx++ == 0) {
                ati_r350_r300_draw_vbuf(s, val);
            }
            break;
        case R300_PM4_OPCODE_LOAD_VBPNTR:
            /*
             * Same data in the same order as type-0 writes to
             * VAP_VTX_AOS_CNT/CTL/ADDR0/ADDR1: the array count, the
             * packed size|stride control dword, then the array
             * addresses (Linux r100_packet3_load_vbpntr). Only the
             * first pair of arrays is captured -- that is all the
             * draw engine models, and all the count field of every
             * captured OS X submission declares.
             */
            if (p->p3_param_idx < 4) {
                ati_r350_reg_write32(s, R300_VAP_VTX_AOS_CNT +
                                        p->p3_param_idx * 4, val);
            }
            p->p3_param_idx++;
            break;
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
        p->p3_opcode == R350_PM4_OPCODE_HOSTDATA_BLT &&
        s->host_data_active) {
        ati_r350_host_data_flush(s);
        s->host_data_active = false;
        s->host_data_next = 0;
    }
    if (p->remaining == 0 && p->type == 3 &&
        p->p3_opcode == R300_PM4_OPCODE_DRAW_IMMD_2 &&
        p->p3_total >= 1 && p->p3_total <= ARRAY_SIZE(s->r300_immd)) {
        ati_r350_r300_draw_immd(s, s->r300_immd, p->p3_total);
    }
}

static void ati_r350_pm4_fifo_push(ATIR350State *s, uint32_t val)
{
    ati_r350_pm4_parse(s, &s->pm4_fifo, val);
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
static void ati_r350_pm4_indirect(ATIR350State *s, uint32_t offset,
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
    ATIR350PM4Parser parser = { 0 };
    bool swap = (s->regs[R350_CP_RB_CNTL >> 2] & R350_BUF_SWAP_MASK) ==
                R350_BUF_SWAP_32BIT;
    uint32_t i;

    trace_ati_r350_pm4_indirect(offset, dwords,
        dwords ? ati_r350_mc_read32(s, offset) : 0);
    if (dwords > 0x10000) {
        /* bogus size -- a real IB is at most a few KB */
        return;
    }
    for (i = 0; i < dwords; i++) {
        uint32_t val = ati_r350_mc_read32(s, offset + i * 4);

        /*
         * CP_RB_CNTL's BUF_SWAP swapper sits on the CP's fetch port,
         * not on the ring specifically: indirect-buffer fetches go
         * through the same dword swapper as ring fetches. OS X's
         * driver stores IBs with native big-endian stores and relies
         * on this (an unswapped fetch reads its 0x80000000 NOP
         * padding as 0x00000080 and the whole IB parses as garbage).
         */
        if (swap) {
            val = bswap32(val);
        }
        trace_ati_r350_pm4_ib_dword(i, val);
        ati_r350_pm4_parse(s, &parser, val);
    }
}

/*
 * All register apertures allow 1/2/4-byte access at any offset
 * (RRG-G04500-C: "access: 8/16/32"). Sub-dword accesses are folded
 * onto the 32-bit register via read-modify-write against the raw
 * stored value.
 */
static uint64_t ati_r350_mmio_read(void *opaque, hwaddr addr,
                                      unsigned size)
{
    ATIR350State *s = opaque;
    uint32_t base = addr & 0xfffc;
    uint32_t val = ati_r350_reg_read32(s, base);

    val = extract32(val, (addr & 3) * 8, size * 8);
    if (ati_r350_reg_name(base)[0] == '?') {
        trace_ati_r350_unk_read(size, addr, val);
    } else {
        trace_ati_r350_reg_read(size, addr, ati_r350_reg_name(base),
                                   val);
    }
    return val;
}

static void ati_r350_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
    ATIR350State *s = opaque;
    uint32_t base = addr & 0xfffc;
    uint32_t val = data;

    if (ati_r350_reg_name(base)[0] == '?') {
        trace_ati_r350_unk_write(size, addr, data);
    } else {
        trace_ati_r350_reg_write(size, addr, ati_r350_reg_name(base),
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

        if (mbase == R350_MM_DATA) {
            mbase = s->regs[R350_MM_INDEX >> 2] & 0xfffc;
        }
        if (mbase == R350_CLOCK_CNTL_DATA) {
            merged = s->plls[s->regs[R350_CLOCK_CNTL_INDEX >> 2] &
                             R350_PLL_ADDR_MASK];
        } else {
            /* raw slot; auto-increment/FIFO registers keep their own
             * state elsewhere, so this stays side-effect free */
            merged = s->regs[mbase >> 2];
        }
        merged = deposit32(merged, (addr & 3) * 8, size * 8, data);
        val = merged;
        /*
         * The cursor registers' stored copies carry no CUR_LOCK bit (it
         * lives in s->cur_lock); a partial write that does not cover the
         * top byte must leave the lock as it is, so put it back before
         * the full-register write path re-derives it from bit 31.
         */
        if ((base == R350_CUR_OFFSET || base == R350_CUR_HORZ_VERT_POSN ||
             base == R350_CUR_HORZ_VERT_OFF) &&
            (addr & 3) + size < 4 && s->cur_lock) {
            val |= R350_CUR_LOCK;
        }
    }
    ati_r350_reg_write32(s, base, val);
}

static const MemoryRegionOps ati_r350_mmio_ops = {
    .read = ati_r350_mmio_read,
    .write = ati_r350_mmio_write,
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
 * Diagnostic window over part of aperture 0 (see the fillwatch fields).
 * Aperture 0 applies no endian swap, so this has to behave exactly like
 * the RAM alias it replaces: on a big-endian guest a store's most
 * significant byte lands at the lowest address, which is what writing
 * lane i as byte (size-1-i) of a DEVICE_BIG_ENDIAN access does.
 */
static uint64_t ati_r350_fillwatch_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    ATIR350State *s = opaque;
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t base = s->fillwatch_off + (uint32_t)addr;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (base + i < ATI_R350_VRAM_SIZE) {
            val |= (uint64_t)vram[base + i] << (8 * (size - 1 - i));
        }
    }
    return val;
}

static void ati_r350_fillwatch_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    ATIR350State *s = opaque;
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t base = s->fillwatch_off + (uint32_t)addr;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (base + i < ATI_R350_VRAM_SIZE) {
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
        trace_ati_r350_fillwatch(s->fw_run_start,
                                    s->fw_run_end - s->fw_run_start,
                                    base - s->fw_run_start);
    }
    s->fw_run_start = base;
    s->fw_run_end = base + size;
    s->fw_active = true;
}

static const MemoryRegionOps ati_r350_fillwatch_ops = {
    .read = ati_r350_fillwatch_read,
    .write = ati_r350_fillwatch_write,
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

/* ---------------------------------------------------------------- */

static void ati_r350_reset_hold(Object *obj, ResetType type)
{
    ATIR350State *s = ATI_R350(obj);

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
    s->regs[R350_DAC_CNTL >> 2] = 0x2 |                 /* PS2 output level */
                                  R350_DAC_CMP_EN |
                                  (R350_DAC_MASK_DEFAULT <<
                                   R350_DAC_MASK_SHIFT);
    s->regs[R350_CRTC_EXT_CNTL >> 2] = R350_DFIFO_EXTSENSE |
                                       R350_CRTC_DISPLAY_DIS;
    s->regs[R350_CRTC_STATUS >> 2] = R350_FIX_VSYNC_TIMING;
    s->regs[R350_CRTC_GEN_CNTL >> 2] = R350_CRTC_DISP_REQ_EN_B;
    s->plls[R350_PLL_CLK_PIN_CNTL] = 0xf7; /* all clock outputs enabled */

    /* Radeon memory-controller / config defaults */
    memset(s->mc_ind, 0, sizeof(s->mc_ind));
    /* frame buffer window at card address 0, AGP window disabled */
    s->regs[R350_MC_FB_LOCATION >> 2] =
        ((ATI_R350_VRAM_SIZE - 1) >> 16) << 16;
    s->regs[R350_MC_AGP_LOCATION >> 2] = 0xffffffc0;
    s->regs[R350_MEM_CNTL >> 2] = R350_MEM_NUM_CHANNELS_256;
    s->regs[R350_CP_RB_CNTL >> 2] = R350_RB_NO_UPDATE;
    s->pm4_buffer_cntl = R350_RB_NO_UPDATE;
    s->pm4_ring_dwords = 0;
    s->pm4_rptr = 0;
    s->pm4_wptr = 0;
    s->pm4_buffer_addr = 0;
    s->vga_ddc_sda = 1;
    s->dvi_ddc_sda = 1;
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
/*
 * Apply the cursor registers to the host pointer. Called from the
 * coalescing timer (see ati_r350_cursor_update) and from the display
 * refresh path.
 */
static void ati_r350_cursor_apply(ATIR350State *s)
{
    uint32_t posn = s->regs[R350_CUR_HORZ_VERT_POSN >> 2];
    uint32_t coff = s->regs[R350_CUR_HORZ_VERT_OFF >> 2];
    uint32_t offs = s->regs[R350_CUR_OFFSET >> 2];
    uint32_t raw0 = s->regs[R350_CUR_CLR0 >> 2];
    uint32_t raw1 = s->regs[R350_CUR_CLR1 >> 2];
    uint32_t clr0 = 0xff000000u | ((raw0 & 0xff) << 16) |
                    (raw0 & 0xff00) | ((raw0 >> 16) & 0xff);
    uint32_t clr1 = 0xff000000u | ((raw1 & 0xff) << 16) |
                    (raw1 & 0xff00) | ((raw1 >> 16) & 0xff);
    uint32_t vram_off = offs & R350_CUR_OFFSET_MASK;
    unsigned horz_off = (coff >> R350_CUR_HORZ_OFF_SHIFT) &
                        R350_CUR_HORZ_OFF_MASK;
    unsigned vert_off = coff & R350_CUR_VERT_OFF_MASK;
    bool on = (s->regs[R350_CRTC_GEN_CNTL >> 2] & R350_CRTC_CUR_EN) != 0;
    const uint8_t *src;
    uint32_t sum = 0;
    QEMUCursor *c;
    unsigned row, px, xr;
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
    if (s->cur_lock) {
        return;
    }
    if (!on) {
        if (s->hw_cursor_on) {
            trace_ati_r350_cursor_off(s->hw_cursor_x, s->hw_cursor_y);
            s->hw_cursor_on = false;
            s->hw_cursor_sum = 0;
            qemu_console_set_mouse(s->con, 0, 0, false);
        }
        return;
    }
    if ((uint64_t)vram_off + R350_CUR_IMAGE_BYTES > ATI_R350_VRAM_SIZE) {
        return;
    }
    src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram) + vram_off;
    xr = ati_r350_vram_xor(s, vram_off);

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
    x = (int)((posn >> R350_CUR_HORZ_POSN_SHIFT) & R350_CUR_HORZ_POSN_MASK) -
        (int)horz_off;
    y = (int)(posn & R350_CUR_VERT_POSN_MASK);

    /*
     * Order-sensitive hash of the image plus everything that shapes it.
     * The previous rotate-left-1/xor was blind to a 64-byte shift of the
     * data (two full 32-bit rotations): OS X 10.2 first shows its arrow
     * with CUR_OFFSET=0 (4 zero rows -- opaque white -- above the image
     * at 0x40) and then moves CUR_OFFSET to 0x40; both views hashed the
     * same (verified: 0x90627cdc for either offset), so the white-topped,
     * 4-rows-late image was kept for good -- a 64x4 white bar above the
     * arrow tip. Mix the offset in as well.
     */
    sum = 0x811c9dc5u ^ vram_off;
    for (row = 0; row < R350_CUR_IMAGE_BYTES; row++) {
        sum = (sum ^ src[row ^ xr]) * 0x01000193u;
    }
    sum ^= clr0 ^ clr1 ^ (uint32_t)horz_off ^ ((uint32_t)vert_off << 8);

    if (s->hw_cursor_on && sum == s->hw_cursor_sum) {
        /* Same image: a move only, so don't re-upload it -- and if it
         * has not moved either (this runs on every refresh tick), do
         * nothing at all. */
        if (x != s->hw_cursor_x || y != s->hw_cursor_y) {
            trace_ati_r350_cursor_move(vram_off, x, y, sum);
            s->hw_cursor_x = x;
            s->hw_cursor_y = y;
            qemu_console_set_mouse(s->con, x, y, true);
        }
        return;
    }

    /*
     * Stopgap for the OS X/OS 9 Radeon ndrv, which positions and
     * colour-programs the hardware cursor but never uploads an image
     * (open investigation): the bits at CUR_OFFSET are then leftover
     * boot junk that decodes to an opaque box. A real mono cursor is
     * mostly transparent (AND plane mostly ones); when almost no AND
     * bits are set the "image" cannot be a cursor, so hide the guest
     * sprite entirely -- the UI then shows the host pointer at the
     * (correctly tracked) guest position instead of the box.
     */
    {
        unsigned and_ones = 0, byte;

        for (row = 0; row < R350_CUR_HEIGHT; row++) {
            for (byte = 0; byte < 8; byte++) {
                and_ones += ctpop8(src[(row * R350_CUR_ROW_BYTES + byte)
                                       ^ xr]);
            }
        }
        if (and_ones < R350_CUR_WIDTH * R350_CUR_HEIGHT / 8) {
            /* substitute the built-in arrow: anything defined earlier
             * (or the box the junk decodes to) would persist otherwise,
             * since a console cursor cannot be undefined */
            c = ati_r350_builtin_arrow();
            qemu_console_set_cursor(s->con, c);
            cursor_unref(c);
            s->hw_cursor_on = true;
            s->hw_cursor_sum = sum;
            s->hw_cursor_x = x;
            s->hw_cursor_y = y;
            qemu_console_set_mouse(s->con, x, y, true);
            return;
        }
    }

    c = cursor_alloc(R350_CUR_WIDTH, R350_CUR_HEIGHT);
    for (row = 0; row < R350_CUR_HEIGHT; row++) {
        for (px = 0; px < R350_CUR_WIDTH; px++) {
            const uint8_t *line = src + row * R350_CUR_ROW_BYTES;
            unsigned bit = 7 - (px & 7);
            unsigned and_bit = (line[(px >> 3) ^ xr] >> bit) & 1;
            unsigned xor_bit = (line[(8 + (px >> 3)) ^ xr] >> bit) & 1;
            uint32_t val;

            if (row >= R350_CUR_HEIGHT - vert_off || px < horz_off) {
                val = 0;                        /* outside the cursor */
            } else if (!and_bit) {
                val = xor_bit ? clr1 : clr0;
            } else {
                val = xor_bit ? 0x80000000u : 0;    /* invert : transparent */
            }
            c->data[row * R350_CUR_WIDTH + px] = val;
        }
    }
    trace_ati_r350_cursor_upload(vram_off, horz_off, vert_off, x, y,
                                    c->data[0], c->data[64 * 4],
                                    c->data[64 * 4 + 4]);
    if (trace_event_get_state_backends(TRACE_ATI_R350_CURSOR_DUMP)) {
        /* the first 2KB of VRAM: covers every sprite offset seen so far */
        const uint8_t *v = (const uint8_t *)memory_region_get_ram_ptr(&s->vram);
        for (row = 0; row < 128; row++) {
            trace_ati_r350_cursor_dump(offs, row * 16,
                                          ldq_be_p(v + row * 16),
                                          ldq_be_p(v + row * 16 + 8));
        }
    }
    qemu_console_set_cursor(s->con, c);
    cursor_unref(c);
    s->hw_cursor_on = true;
    s->hw_cursor_sum = sum;
    s->hw_cursor_x = x;
    s->hw_cursor_y = y;
    qemu_console_set_mouse(s->con, x, y, true);
}

static void ati_r350_cursor_timer(void *opaque)
{
    ati_r350_cursor_apply(opaque);
}

/*
 * A guest moves the hardware cursor with a burst of separate register
 * writes -- Mac OS 9's Rage 128 driver does six per move: both 16-bit
 * halves of CUR_HORZ_VERT_POSN, both halves of CUR_HORZ_VERT_OFF,
 * CUR_OFFSET and the CRTC_GEN_CNTL enable byte, none of them under
 * CUR_LOCK. Publishing after every one of those turned each move into
 * several host-cursor updates, the first with the new x but the old y (a
 * visible L-shaped hitch), and re-hashed the 1KB image every time. On
 * real hardware the whole burst is done long before the beam next
 * reaches the cursor; model that by applying the registers once, a
 * moment after the first write, so the burst is seen whole. Bounded
 * latency: the timer is not re-armed by later writes in the burst.
 */
static void ati_r350_cursor_update(ATIR350State *s)
{
    if (!s->cursor_timer) {
        ati_r350_cursor_apply(s);
        return;
    }
    if (!timer_pending(s->cursor_timer)) {
        timer_mod(s->cursor_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCALE_MS);
    }
}

/*
 * Host-driven pointer for this display (see the header), used only while
 * the guest's own hardware cursor is switched off -- Mac OS X does drive
 * these registers, so once it has, the real cursor wins and this stands
 * aside rather than fighting it for the console cursor.
 */
void ati_r350_host_cursor(int x, int y, bool on)
{
    ATIR350State *s;
    Object *o = object_resolve_path_type("", TYPE_ATI_R350, NULL);

    if (!o) {
        return;
    }
    s = ATI_R350(o);
    if (!s->con) {
        return;
    }
    s->host_cursor_active = true;
    s->hw_cursor_on = false;

    if (on && !s->host_cursor_published) {
        QEMUCursor *c = ati_r350_builtin_arrow();

        qemu_console_set_cursor(s->con, c);
        cursor_unref(c);
        s->host_cursor_published = true;
    }
    qemu_console_set_mouse(s->con, x, y, on);
}

static void ati_r350_realize(PCIDevice *dev, Error **errp)
{
    ATIR350State *s = ATI_R350(dev);
    Object *obj = OBJECT(dev);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0,
                                         &ati_r350_gfx_ops, s);

    /*
     * BAR0: the 128MB linear frame-buffer aperture, all of it VRAM. The
     * Radeon's big-endian support is the SURFACE_CNTL byte swappers,
     * honoured by every consumer of VRAM bytes (ati_r350_vram_xor())
     * rather than by trapping CPU stores.
     */
    memory_region_init(&s->aper, obj, "ati-r350-aper",
                       ATI_R350_APER_SIZE);
    memory_region_init_ram(&s->vram, obj, "ati-r350-vram",
                           ATI_R350_VRAM_SIZE, &error_fatal);
    /*
     * Needed by ati_r350_scan_vram_activity() to auto-detect the
     * real live framebuffer when CRTC1 never describes it (see that
     * function's comment) -- reuses the same DIRTY_MEMORY_VGA client
     * every other display device's dirty-scanline tracking already
     * uses, since this device has no dirty-tracking use of its own to
     * conflict with.
     */
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    memory_region_add_subregion(&s->aper, 0, &s->vram);
    /*
     * Optional diagnostic overlay on aperture 0 (see the fillwatch fields
     * in the header). Higher priority than the RAM alias underneath, so
     * stores in this range come to us instead of landing silently.
     */
    if (s->fillwatch_size &&
        (uint64_t)s->fillwatch_off + s->fillwatch_size <=
        ATI_R350_VRAM_SIZE) {
        memory_region_init_io(&s->vram_watch, obj,
                              &ati_r350_fillwatch_ops, s,
                              "ati-r350-fillwatch", s->fillwatch_size);
        memory_region_add_subregion_overlap(&s->aper, s->fillwatch_off,
                                            &s->vram_watch, 1);
    }

    memory_region_init_io(&s->mmio, obj, &ati_r350_mmio_ops, s,
                          "ati-r350-mmio", ATI_R350_MMIO_SIZE);
    /*
     * BAR1: the 256-byte I/O register window. Registers above 0xFF are
     * reachable through it via the MM_INDEX/MM_DATA pair at 0x00/0x04
     * -- which the shared mmio ops already implement, so the window is
     * simply the low 256 bytes of the same register file.
     */
    memory_region_init_io(&s->io, obj, &ati_r350_mmio_ops, s,
                          "ati-r350-io", ATI_R350_IO_SIZE);

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

    /*
     * AGP capability block: this is an AGP-only part and the Mac ROMs'
     * FCode walks the capability chain for it (see the Rage 128 device
     * for the observed behaviour when it is missing). AGP 2.0, 4x/2x/1x,
     * sideband addressing, fast writes -- what the PM34's UniNorth AGP
     * bridge can negotiate; the command register is guest-writable so
     * the driver's enable handshake completes.
     */
    {
        int cap = pci_add_capability(dev, PCI_CAP_ID_AGP, 0, 0x0c, errp);
        if (cap < 0) {
            return;
        }
        dev->config[cap + PCI_AGP_VERSION] = 0x20; /* AGP 2.0 */
        pci_set_long(dev->config + cap + PCI_AGP_STATUS,
                     PCI_AGP_STATUS_RQ_MASK | PCI_AGP_STATUS_SBA |
                     PCI_AGP_STATUS_FW | PCI_AGP_STATUS_RATE4 |
                     PCI_AGP_STATUS_RATE2 | PCI_AGP_STATUS_RATE1);
        pci_set_long(dev->wmask + cap + PCI_AGP_COMMAND, 0xffffffffu);
    }

    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ati_r350_vblank_timer_tick, s);
    s->vblank_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ati_r350_vblank_end_tick, s);
    s->cursor_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ati_r350_cursor_timer, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ATI_R350_VBLANK_PERIOD_NS);

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
     * The MONID bit-banged DDC bus (see the R350_GPIO_MONID write
     * handler): the ATI Mac drivers read EDID here, not through the
     * hardware I2C engine.
     */
    {
        I2CBus *ddcbus = i2c_init_bus(DEVICE(dev), "ati-r350.monid-ddc");
        I2CSlave *slv = i2c_slave_create_simple(ddcbus,
                                                TYPE_ATI_R350_DDC, 0x50);

        ATI_R350_DDC(slv)->edid = s->edid;
        bitbang_i2c_init(&s->monid_i2c, ddcbus);
        s->monid_sda = 1;
    }
    /*
     * The Radeon's dedicated DDC pads: GPIO_VGA_DDC (CRT / ADC head) and
     * GPIO_DVI_DDC (DVI head), each an independent bit-banged bus with
     * the same EDID EEPROM behind it.
     */
    {
        I2CBus *bus = i2c_init_bus(DEVICE(dev), "ati-r350.vga-ddc");

        /* the ADC/VGA head: an EDID EEPROM only when a display is there */
        if (s->second_display) {
            I2CSlave *slv = i2c_slave_create_simple(bus, TYPE_ATI_R350_DDC,
                                                    0x50);

            ATI_R350_DDC(slv)->edid = s->edid;
        }
        bitbang_i2c_init(&s->vga_ddc_i2c, bus);
        s->vga_ddc_sda = 1;
    }
    /* Apple-sense on MONID belongs to the same second head */
    s->monitor_connected = s->second_display;
    {
        I2CBus *bus = i2c_init_bus(DEVICE(dev), "ati-r350.dvi-ddc");
        I2CSlave *slv = i2c_slave_create_simple(bus, TYPE_ATI_R350_DDC, 0x50);

        ATI_R350_DDC(slv)->edid = s->edid;
        bitbang_i2c_init(&s->dvi_ddc_i2c, bus);
        s->dvi_ddc_sda = 1;
    }
}

static void ati_r350_exit(PCIDevice *dev)
{
    ATIR350State *s = ATI_R350(dev);

    timer_free(s->vblank_timer);
    timer_free(s->vblank_end_timer);
    timer_free(s->cursor_timer);
    qemu_graphic_console_close(s->con);
}

static const VMStateDescription vmstate_ati_r350 = {
    .name = "ati-radeon9800",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, ATIR350State),
        VMSTATE_UINT32_ARRAY(regs, ATIR350State, ATI_R350_NUM_REGS),
        VMSTATE_UINT32_ARRAY(plls, ATIR350State, ATI_R350_NUM_PLLS),
        VMSTATE_UINT8_2DARRAY(palette, ATIR350State, 256, 3),
        VMSTATE_UINT8(dac_wr_index, ATIR350State),
        VMSTATE_UINT8(dac_rd_index, ATIR350State),
        VMSTATE_UINT8(i2c_offset, ATIR350State),
        VMSTATE_UINT8_ARRAY(i2c_data_fifo, ATIR350State, 16),
        VMSTATE_INT32(i2c_data_len, ATIR350State),
        VMSTATE_INT32(i2c_data_pos, ATIR350State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ati_r350_properties[] = {
    /*
     * Diagnostic only: lay an instrumented window over part of aperture 0
     * so CPU fills of that VRAM range become traceable. Off by default.
     */
    DEFINE_PROP_UINT32("fillwatch", ATIR350State, fillwatch_off, 0),
    DEFINE_PROP_UINT32("fillwatch-size", ATIR350State, fillwatch_size, 0),
    DEFINE_PROP_BOOL("second-display", ATIR350State, second_display, false),
    DEFINE_EDID_PROPERTIES(ATIR350State, edid_info),
};

static const char *const ati_r350_gap_names[R350_GAP_MAX] = {
    [R350_GAP_P3_OPCODE]    = "packet3 opcode",
    [R350_GAP_PRIM]         = "primitive type",
    [R350_GAP_VTX_WALK]     = "vertex walk mode",
    [R350_GAP_TEX_FORMAT]   = "texture format",
    [R350_GAP_BLEND_FACTOR] = "blend factor",
    [R350_GAP_VS_VECTOR_OP] = "vertex program vector opcode",
    [R350_GAP_VS_MATH_OP]   = "vertex program math opcode",
    [R350_GAP_VS_DST_FILE]  = "vertex program destination file",
};

void ati_r350_note_gap(ATIR350State *s, ATIR350GapKind kind, unsigned idx)
{
    if (kind >= R350_GAP_MAX || idx >= R350_GAP_SLOTS) {
        return;
    }
    if (!s->gap_count[kind][idx]) {
        warn_report("ati-radeon9800: unimplemented %s 0x%x -- the guest "
                    "asked for it and the command was dropped",
                    ati_r350_gap_names[kind], idx);
    }
    if (s->gap_count[kind][idx] != UINT32_MAX) {
        s->gap_count[kind][idx]++;
    }
}

/*
 * `gaps` property: everything ati_r350_note_gap() has seen this run,
 * with use counts. Read it from the monitor with
 *   qom-get /machine/peripheral-anon/device[N] gaps
 * after exercising a guest, and anything the model quietly ignores is
 * named rather than left to be inferred from a wrong-looking screen.
 *
 * Registers are deliberately not counted here: a register this model
 * stores but never reads back is invisible from inside the device, so
 * that half of the coverage question stays with the offline audit
 * script, which diffs guest writes against our own source.
 */
static char *ati_r350_get_gaps(Object *obj, Error **errp)
{
    ATIR350State *s = ATI_R350(obj);
    GString *out = g_string_new(NULL);
    unsigned k, i;

    for (k = 0; k < R350_GAP_MAX; k++) {
        for (i = 0; i < R350_GAP_SLOTS; i++) {
            if (s->gap_count[k][i]) {
                g_string_append_printf(out, "%s%s 0x%x: %u",
                                       out->len ? "\n" : "",
                                       ati_r350_gap_names[k], i,
                                       s->gap_count[k][i]);
            }
        }
    }
    if (!out->len) {
        g_string_append(out, "none");
    }
    return g_string_free(out, FALSE);
}

static void ati_r350_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    object_class_property_add_str(klass, "gaps", ati_r350_get_gaps, NULL);

    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    /* must match the Mac Edition ROMs' PCIR exactly (see the define) */
    k->device_id = PCI_DEVICE_ID_ATI_R350;
    /* ATI reference-board subsystem identity, as real R350 cards report */
    k->subsystem_vendor_id = PCI_VENDOR_ID_ATI;
    k->subsystem_id = 0x0002;
    k->revision  = 0x00;
    /*
     * Add-in card: the FCode driver comes from the PCI expansion ROM,
     * which the Beige G3 ROM's own Open Firmware probes and executes
     * (unlike the onboard mach64, whose FCode lives inside the system
     * ROM -- see the deliberate no-romfile comment in ati_mach64.c).
     * No default romfile name: pass romfile=<path> on the -device
     * option, pointing at the real card ROM dump.
     */
    k->realize   = ati_r350_realize;
    k->exit      = ati_r350_exit;
    dc->vmsd     = &vmstate_ati_r350;
    device_class_set_props(dc, ati_r350_properties);
    rc->phases.hold = ati_r350_reset_hold;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ati_r350_type_info = {
    .name           = TYPE_ATI_R350,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(ATIR350State),
    .class_init     = ati_r350_class_init,
    .interfaces     = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ati_r350_register_types(void)
{
    type_register_static(&ati_r350_type_info);
    type_register_static(&ati_r350_ddc_info);
}

type_init(ati_r350_register_types)
