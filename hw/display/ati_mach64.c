/*
 * QEMU ATI Mach64 "3D Rage Pro" emulation
 *
 * See ati_mach64.h for background: this models the ATI Mach64 "3D Rage
 * Pro" chip (PCI vendor 0x1002, device 0x4750, revision 0x5c) -- the
 * identity directly confirmed by this machine's own real Open Firmware
 * device-tree dump. Milestone scope: correct PCI identity/BAR layout,
 * enough CRTC/DAC/config register modeling for native ROM boot-time
 * probing and a basic mode-set, a linear framebuffer, and the hardware
 * cursor. The 2D BitBLT engine, 3D pipeline/CCE, overlay/video-in
 * registers and true DDC/I2C are deliberately not modeled -- classic
 * Mac OS QuickDraw always has a software fallback per pixel depth, so
 * none of that is boot-blocking. There is currently no 2D or 3D
 * acceleration of any kind; all drawing goes through QuickDraw's
 * software rasterizer into plain VRAM.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "exec/icount.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/i2c-ddc.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

#include "ati_mach64_int.h"
#include "ati_rage128_int.h"
#include "ati_mach64_regs.h"
#include "trace.h"

#define ATI_MACH64_VBLANK_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)
/* Blank interval = last 1/8 of the frame, matching the phase-computed
 * live VBLANK status bit in the INT_CNTL read path. */
#define ATI_MACH64_VBLANK_LEN_NS    (ATI_MACH64_VBLANK_PERIOD_NS / 8)
/*
 * icount-only compensation for the live VBLANK status bit (see its read
 * handler below): under -icount, guest instructions advance QEMU_CLOCK_VIRTUAL
 * by a fixed amount each, decoupled from real host speed. A bounded-retry
 * guest busy-wait poll (Mac OS's boot-time "wait for the CRT timing
 * generator" check is exactly such a loop, confirmed live: ~10000 iterations,
 * each burning a fixed slice of virtual time) can therefore cover far less
 * virtual time per call than on real hardware, where the same poll finishes
 * in real microseconds regardless of VBLANK phase. At the project's
 * established icount shift=4, that budget is ~1.9ms -- smaller than this
 * bit's normal ~2.08ms "on" window, so under fully deterministic icount
 * pacing a poll that starts out of phase can miss forever, not just
 * occasionally (confirmed live: 3 early hits, then permanent timeout for the
 * rest of boot, reproducible across shift=4/7/auto). Shrinking the "off" gap
 * to a fixed, small slice of virtual time under icount -- regardless of
 * shift -- keeps any poll with a reasonable instruction budget from ever
 * landing in it, without touching the realistic ~12.5% duty cycle used in
 * normal (non-icount) operation. Precedented pattern: icount_enabled()-gated
 * device/DMA timing compensation for guest determinism assumptions that only
 * break under icount, e.g. system/dma-helpers.c's overlapping-SG splitting.
 */
#define ATI_MACH64_VBLANK_ICOUNT_GAP_NS 50000
/*
 * How long the PCI IRQ line stays asserted each frame if the guest
 * does NOT acknowledge the device (INT_CNTL ack writes deassert it
 * early -- see the INT_CNTL write handler). Heathrow marks this GPU
 * line level_triggered (0x1ff00000, heathrow_pic.c) and its event
 * latch deliberately EXCLUDES level-triggered lines, so the interrupt
 * is guest-visible only while the line is physically high. The old
 * 2us pulse here was unobservably short for any handler that takes
 * more than a few hundred instructions to read the PIC (measured
 * live: Mac OS X 10.2 acked only 16 of 1017 VBL interrupts, its
 * dispatcher usually finding nothing pending by the time it looked --
 * the direct cause of the VBL-starved frozen-cursor-when-idle bug,
 * and the same mechanism as classic Mac OS's long-stalled
 * CrsrVBLTask). Real silicon holds the line until software acks the
 * device; we now do the same, with this full-blanking-interval cap as
 * the fallback for boot phases that never ack. (The 2026-07-28
 * experiment that rejected held lines as "storming" predates the
 * MMIO endianness fix -- guest acks were byte-swap-corrupted then and
 * could never land.)
 */
#define ATI_MACH64_VBLANK_IRQ_LEN_NS ATI_MACH64_VBLANK_LEN_NS


static uint32_t ati_mach64_pixel_bpp(uint32_t pix_fmt)
{
    switch (pix_fmt) {
    case ATI_PIX_FMT_4BPP:
        return 0; /* not supported for a linear framebuffer path */
    case ATI_PIX_FMT_8BPP:
        return 1;
    case ATI_PIX_FMT_RGB555:
    case ATI_PIX_FMT_RGB565:
        return 2;
    case ATI_PIX_FMT_RGB888:
        return 3;
    case ATI_PIX_FMT_ARGB8888:
        return 4;
    default:
        return 0;
    }
}

static pixman_format_code_t ati_mach64_pixman_format(uint32_t pix_fmt)
{
    switch (pix_fmt) {
    case ATI_PIX_FMT_8BPP:
        return PIXMAN_r3g3b2; /* placeholder direct mapping; palette
                                * indexed 8bpp is handled via the DAC
                                * loaded palette in ati_mach64_update_mode */
    case ATI_PIX_FMT_RGB555:
        return PIXMAN_x1r5g5b5;
    case ATI_PIX_FMT_RGB565:
        return PIXMAN_r5g6b5;
    case ATI_PIX_FMT_RGB888:
        return PIXMAN_r8g8b8;
    case ATI_PIX_FMT_ARGB8888:
        /*
         * Guest is PowerPC (big-endian) -- each 32-bit pixel is stored
         * to VRAM MSB-first (byte order X,R,G,B), not host-native
         * little-endian. PIXMAN_x8r8g8b8 assumes a little-endian-native
         * word and reads those same four bytes as B,G,R,X, scrambling
         * every pixel (confirmed via a live VRAM read: bytes
         * "00 63 63 9c" at a known blue-purple desktop-pattern pixel --
         * only reproduces RGB(99,99,156) when read as X,R,G,B --
         * PIXMAN_x8r8g8b8 turns those same bytes into a dark-olive
         * RGB(99,99,0) instead, matching the wrong colors seen live).
         * PIXMAN_b8g8r8x8 is the format whose native-word bit layout
         * (B,G,R,X from MSB) matches those bytes when loaded on a
         * little-endian host.
         */
        return PIXMAN_b8g8r8x8;
    default:
        return PIXMAN_x8r8g8b8;
    }
}

static void ati_mach64_get_mode(ATIMach64State *s, ATIMach64Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[ATI_CRTC_GEN_CNTL >> 2];
    uint32_t h_total_disp = s->regs[ATI_CRTC_H_TOTAL_DISP >> 2];
    uint32_t v_total_disp = s->regs[ATI_CRTC_V_TOTAL_DISP >> 2];
    uint32_t off_pitch = s->regs[ATI_CRTC_OFF_PITCH >> 2];
    uint32_t pix_fmt = (crtc_gen_cntl >> ATI_CRTC_PIX_WIDTH_SHIFT) &
                       ATI_CRTC_PIX_WIDTH_MASK;

    memset(mode, 0, sizeof(*mode));

    mode->width = (((h_total_disp >> ATI_CRTC_H_DISP_SHIFT) &
                    ATI_CRTC_H_DISP_MASK) + 1) * 8;
    mode->height = ((v_total_disp >> ATI_CRTC_V_DISP_SHIFT) &
                    ATI_CRTC_V_DISP_MASK) + 1;
    mode->bpp = ati_mach64_pixel_bpp(pix_fmt);
    mode->pitch = (((off_pitch >> ATI_CRTC_PITCH_SHIFT) &
                    ATI_CRTC_PITCH_MASK) * 8) * (mode->bpp ? mode->bpp : 1);
    mode->fb_offset = (off_pitch & ATI_CRTC_OFFSET_MASK) << 3;
}

static bool ati_mach64_mode_valid(ATIMach64State *s, const ATIMach64Mode *mode)
{
    uint32_t crtc_gen_cntl = s->regs[ATI_CRTC_GEN_CNTL >> 2];

    if (crtc_gen_cntl & ATI_CRTC_DISPLAY_DIS) {
        return false;
    }
    if (mode->width < 16 || mode->height < 16 || mode->bpp == 0) {
        return false;
    }
    /*
     * A depth switch that leaves OFF_PITCH's pitch field stale/zero
     * relative to the new bpp (e.g. still sized for the previous depth,
     * momentarily during a live mode-change register sequence) must not
     * be accepted -- an undersized stride fed into
     * qemu_create_displaysurface_from() corrupts the whole framebuffer
     * view rather than just clipping, since pixman reads full rows at
     * the given stride regardless of the true per-pixel width.
     */
    if ((uint64_t)mode->pitch < (uint64_t)mode->width * mode->bpp) {
        return false;
    }
    if ((uint64_t)mode->fb_offset + (uint64_t)mode->pitch * mode->height >
        ATI_MACH64_VRAM_SIZE) {
        return false;
    }
    return true;
}

/*
 * 8bpp is a palettized (CLUT) mode on this hardware: pixel bytes index
 * the DAC palette loaded via the DAC registers (kept in s->palette).
 * There is no pixman format for that, so render via an allocated 32bpp
 * surface, converting through the palette every refresh (this also
 * picks up palette animation without extra dirty tracking). The
 * previous PIXMAN_r3g3b2 direct mapping treated palette indices as raw
 * RGB332 and produced wildly wrong colors (Mac OS platinum grey came
 * out dark red).
 */
static void ati_mach64_draw_8bpp(ATIMach64State *s, DisplaySurface *ds,
                                 const ATIMach64Mode *mode)
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

/*
 * 15/16bpp direct-color modes hold BIG-endian pixels: this is a Mac
 * card, the guest's framebuffer is big-endian (its CPU stores pixels
 * MSB-first and the 2D engine writes them the same way), so handing
 * the raw VRAM to pixman with a host-native 16-bit format shows every
 * pixel byte-swapped -- the long-standing "thousands of colours comes
 * out green" bug. Convert into an allocated 32bpp surface instead,
 * exactly as QEMU's own big-endian Mac framebuffer does
 * (hw/display/macfb.c macfb_draw_line16).
 */
static void ati_mach64_draw_16bpp(ATIMach64State *s, DisplaySurface *ds,
                                  const ATIMach64Mode *mode, bool rgb565)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            uint16_t pixel = ((uint16_t)src[2 * x] << 8) | src[2 * x + 1];
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

/* Packed 24bpp: three big-endian bytes per pixel, R first. */
static void ati_mach64_draw_24bpp(ATIMach64State *s, DisplaySurface *ds,
                                  const ATIMach64Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int x, y;

    for (y = 0; y < mode->height; y++) {
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            dst[x] = 0xff000000u | ((uint32_t)src[3 * x] << 16) |
                     ((uint32_t)src[3 * x + 1] << 8) | src[3 * x + 2];
        }
        src += mode->pitch;
    }
}


/*
 * Hardware video overlay (the "scaler").
 *
 * Mac OS plays QuickTime through this rather than through the draw
 * engine: it points the scaler at a YUV 4:2:2 buffer in VRAM, gives a
 * destination window on screen and a scale factor, and the CRTC
 * substitutes scaled video for graphics inside that window. With none of
 * it modelled the guest's movie was simply invisible -- video played
 * black while the transport controls animated normally.
 *
 * Only the parts a player actually programs are implemented: a single
 * buffer, nearest-neighbour scaling from the 4.12 accumulator steps, the
 * packed 4:2:2 and direct-colour source formats, and the graphics colour
 * key. Interpolation coefficients, the second buffer, planar YUV and the
 * capture path are ignored.
 */
/*
 * ARGB8888 into a shadow surface. Only needed when the overlay is up:
 * without it this mode is mapped straight out of VRAM.
 */
static void ati_mach64_draw_32bpp(ATIMach64State *s, DisplaySurface *ds,
                                  const ATIMach64Mode *mode)
{
    uint8_t *src = (uint8_t *)memory_region_get_ram_ptr(&s->vram) +
                   mode->fb_offset;
    uint32_t *dst;
    int y;

    for (y = 0; y < mode->height; y++) {
        int x;

        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (x = 0; x < mode->width; x++) {
            /*
             * VRAM holds big-endian xRGB -- bytes X,R,G,B, which is why
             * the zero-copy path can hand it to pixman as b8g8r8x8. The
             * shadow surface is host-native, so the pixels need the same
             * conversion every other depth here does. A plain memcpy
             * (what this was) put the unused X byte in the blue channel
             * and swapped red with green: the whole screen turned olive
             * the instant an overlay switched this path on.
             */
            dst[x] = 0xff000000u | (ldl_be_p(src + x * 4) & 0xffffff);
        }
        src += mode->pitch;
    }
}

static bool ati_mach64_overlay_active(ATIMach64State *s)
{
    uint32_t cntl = s->regs[ATI_OVERLAY_SCALE_CNTL >> 2];

    if ((cntl & (ATI_SCALE_EN | ATI_OVERLAY_EN)) !=
        (ATI_SCALE_EN | ATI_OVERLAY_EN)) {
        return false;
    }
    /*
     * Require a configuration that could actually produce pixels before
     * believing the enable bits. Engaging the overlay switches the whole
     * display to a composited shadow surface, so a stray value in these
     * registers must not be able to change how the screen is rendered --
     * and stray values are reachable here, because a bus-master
     * descriptor aimed at the block-1 register window writes straight
     * into this same array.
     */
    return s->regs[ATI_OVERLAY_SCALE_INC >> 2] != 0 &&
           (s->regs[ATI_SCALER_BUF_PITCH >> 2] & 0xfff) != 0 &&
           (s->regs[ATI_SCALER_HEIGHT_WIDTH >> 2] & 0x07ff07ff) != 0;
}

static uint32_t ati_mach64_overlay_texel(ATIMach64State *s, unsigned fmt,
                                         const uint8_t *row, int sx)
{
    int y, u, v, r, g, b;

    switch (fmt) {
    case ATI_SCALER_IN_VYUY422:   /* Y0 U Y1 V in memory order (YUY2) */
    case ATI_SCALER_IN_YVYU422:   /* U Y0 V Y1 in memory order (UYVY) */
    {
        const uint8_t *pair = row + (sx & ~1) * 2;
        bool odd = sx & 1;

        if (fmt == ATI_SCALER_IN_VYUY422) {
            y = pair[odd ? 2 : 0];
            u = pair[1];
            v = pair[3];
        } else {
            y = pair[odd ? 3 : 1];
            u = pair[0];
            v = pair[2];
        }
        /* BT.601, the conversion the scaler's colour-space unit does */
        y = (y - 16) * 298;
        u -= 128;
        v -= 128;
        r = (y + 409 * v + 128) >> 8;
        g = (y - 100 * u - 208 * v + 128) >> 8;
        b = (y + 516 * u + 128) >> 8;
        break;
    }
    case ATI_SCALER_IN_32BPP:
        return ldl_be_p(row + sx * 4) & 0xffffff;
    case ATI_SCALER_IN_16BPP:
    case ATI_SCALER_IN_15BPP:
    {
        uint16_t px = lduw_be_p(row + sx * 2);

        if (fmt == ATI_SCALER_IN_16BPP) {
            r = ((px >> 11) & 0x1f) << 3;
            g = ((px >> 5) & 0x3f) << 2;
            b = (px & 0x1f) << 3;
        } else {
            r = ((px >> 10) & 0x1f) << 3;
            g = ((px >> 5) & 0x1f) << 3;
            b = (px & 0x1f) << 3;
        }
        break;
    }
    default:
        return 0;
    }

    r = MIN(MAX(r, 0), 255);
    g = MIN(MAX(g, 0), 255);
    b = MIN(MAX(b, 0), 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void ati_mach64_overlay_composite(ATIMach64State *s, DisplaySurface *ds,
                                         const ATIMach64Mode *mode)
{
    uint32_t start = s->regs[ATI_OVERLAY_Y_X_START >> 2];
    uint32_t end = s->regs[ATI_OVERLAY_Y_X_END >> 2];
    uint32_t inc = s->regs[ATI_OVERLAY_SCALE_INC >> 2];
    uint32_t hw = s->regs[ATI_SCALER_HEIGHT_WIDTH >> 2];
    uint32_t keyc = s->regs[ATI_OVERLAY_GRAPHICS_KEY_CLR >> 2] & 0xffffff;
    uint32_t keym = s->regs[ATI_OVERLAY_GRAPHICS_KEY_MSK >> 2] & 0xffffff;
    uint32_t keyfn = s->regs[ATI_OVERLAY_KEY_CNTL >> 2] &
                     ATI_GRAPHIC_KEY_FN_MASK;
    unsigned fmt = (s->regs[ATI_VIDEO_FORMAT >> 2] & ATI_SCALER_IN_MASK) >>
                   ATI_SCALER_IN_SHIFT;
    uint32_t offset = s->regs[ATI_SCALER_BUF0_OFFSET >> 2];
    uint32_t pitch = s->regs[ATI_SCALER_BUF_PITCH >> 2] & 0xfff;
    const uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    /* both ends inclusive, X in the high half */
    int x0 = (start >> 16) & 0x7ff, y0 = start & 0x7ff;
    int x1 = (end >> 16) & 0x7ff, y1 = end & 0x7ff;
    int src_w = (hw >> 16) & 0x7ff, src_h = hw & 0x7ff;
    uint32_t h_inc = (inc >> 16) & 0xffff, v_inc = inc & 0xffff;
    int bypp = (fmt == ATI_SCALER_IN_32BPP) ? 4 :
               (fmt == ATI_SCALER_IN_VYUY422 ||
                fmt == ATI_SCALER_IN_YVYU422) ? 2 : 2;
    int x, y;

    trace_ati_mach64_overlay(x0, y0, x1, y1, src_w, src_h, h_inc, v_inc,
                             offset, pitch);
    trace_ati_mach64_overlay_cfg(s->regs[ATI_OVERLAY_SCALE_CNTL >> 2], fmt,
                                 keyfn);
    if (!h_inc || !v_inc) {
        trace_ati_mach64_overlay_skip("zero scale increment");
        return;
    }
    if (!pitch) {
        trace_ati_mach64_overlay_skip("zero source pitch");
        return;
    }
    if (src_w <= 0 || src_h <= 0) {
        trace_ati_mach64_overlay_skip("empty source rectangle");
        return;
    }
    if (x1 < x0 || y1 < y0) {
        trace_ati_mach64_overlay_skip("empty destination rectangle");
        return;
    }
    x1 = MIN(x1, (int)mode->width - 1);
    y1 = MIN(y1, (int)mode->height - 1);

    for (y = y0; y <= y1; y++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                                     y * surface_stride(ds));
        /* 4.12 accumulator: twelve fractional bits */
        int sy = ((y - y0) * v_inc) >> 12;
        const uint8_t *row;

        if (sy >= src_h) {
            break;
        }
        row = vram + offset + (uint32_t)sy * pitch;
        if (offset + (uint32_t)(sy + 1) * pitch > ATI_MACH64_VRAM_SIZE) {
            break;
        }
        for (x = x0; x <= x1; x++) {
            int sx = ((x - x0) * h_inc) >> 12;

            if (sx >= src_w) {
                break;
            }
            if (keyfn == ATI_GRAPHIC_KEY_FN_EQ &&
                (dst[x] & keym & 0xffffff) != (keyc & keym)) {
                continue;   /* graphics wins here */
            }
            if (keyfn == ATI_GRAPHIC_KEY_FN_NE &&
                (dst[x] & keym & 0xffffff) == (keyc & keym)) {
                continue;
            }
            if ((uint32_t)(sx + 1) * bypp > pitch) {
                break;
            }
            dst[x] = 0xff000000u |
                     ati_mach64_overlay_texel(s, fmt, row, sx);
        }
    }
}

static bool ati_mach64_update_display(void *opaque)
{
    ATIMach64State *s = opaque;
    ATIMach64Mode mode;
    DisplaySurface *ds;
    uint8_t *ptr;
    uint32_t pix_fmt;
    bool shadow, overlay;

    ati_mach64_get_mode(s, &mode);
    trace_ati_mach64_update(mode.width, mode.height, mode.bpp,
                            ati_mach64_mode_valid(s, &mode),
                            mode.fb_offset);
    if (!ati_mach64_mode_valid(s, &mode)) {
        return true;
    }
    pix_fmt = (s->regs[ATI_CRTC_GEN_CNTL >> 2] >> ATI_CRTC_PIX_WIDTH_SHIFT) &
              ATI_CRTC_PIX_WIDTH_MASK;
    /*
     * Every depth except ARGB8888 needs per-pixel conversion into an
     * allocated surface: 8bpp is palettized, and the 15/16/24bpp modes
     * hold big-endian pixels (see ati_mach64_draw_16bpp). ARGB8888 is
     * the one layout whose in-memory byte order maps directly to a
     * pixman format (PIXMAN_b8g8r8x8 on a little-endian host), so it
     * still renders straight out of VRAM with no copy.
     */
    shadow = (pix_fmt != ATI_PIX_FMT_ARGB8888);
    /*
     * ARGB8888 normally renders straight out of VRAM with no copy, but
     * the overlay has to be composited over the graphics, so while it is
     * running that mode needs a shadow surface too. Switching the
     * overlay on or off therefore changes the surface type, which means
     * the surface must be rebuilt at the transition.
     */
    overlay = ati_mach64_overlay_active(s);
    if (overlay) {
        shadow = true;
    }
    if (overlay != s->overlay_shown) {
        s->overlay_shown = overlay;
        s->mode_dirty = true;
    }

    if (memcmp(&s->mode, &mode, sizeof(mode)) != 0 || s->mode_dirty) {
        s->mode = mode;
        s->mode_dirty = false;
        if (shadow) {
            ds = qemu_create_displaysurface(mode.width, mode.height);
        } else {
            ptr = memory_region_get_ram_ptr(&s->vram);
            ds = qemu_create_displaysurface_from(mode.width, mode.height,
                                                 ati_mach64_pixman_format(
                                                     pix_fmt),
                                                 mode.pitch,
                                                 ptr + mode.fb_offset);
        }
        qemu_console_set_surface(s->con, ds);
    }
    if (shadow) {
        ds = qemu_console_surface(s->con);
        switch (pix_fmt) {
        case ATI_PIX_FMT_8BPP:
            ati_mach64_draw_8bpp(s, ds, &mode);
            break;
        case ATI_PIX_FMT_RGB555:
            ati_mach64_draw_16bpp(s, ds, &mode, false);
            break;
        case ATI_PIX_FMT_RGB565:
            ati_mach64_draw_16bpp(s, ds, &mode, true);
            break;
        case ATI_PIX_FMT_RGB888:
            ati_mach64_draw_24bpp(s, ds, &mode);
            break;
        case ATI_PIX_FMT_ARGB8888:
            /* only reached with the overlay up -- see `shadow` above */
            ati_mach64_draw_32bpp(s, ds, &mode);
            break;
        default:
            break;
        }
        if (overlay) {
            ati_mach64_overlay_composite(s, ds, &mode);
        }
    }
    qemu_console_update_full(s->con);

    return true;
}

static const GraphicHwOps ati_mach64_gfx_ops = {
    .gfx_update = ati_mach64_update_display,
};

/*
 * Rebuild and publish the hardware cursor from the CUR_* registers and
 * the 2bpp image at CUR_OFFSET*8 in VRAM (see ati_mach64_regs.h for
 * the format). The display backend (cocoa/gtk/vnc) composites the
 * cursor for us via qemu_console_set_cursor/set_mouse, so nothing is
 * ever drawn into the guest-visible framebuffer. Code 3 ("complement
 * of the display pixel") has no QEMUCursor equivalent; approximate it
 * with 50%-alpha black, which reads fine for the classic Mac cursors
 * that use it for anti-aliasing edges.
 */
static void ati_mach64_cursor_update(ATIMach64State *s)
{
    uint32_t posn = s->regs[ATI_CUR_HORZ_VERT_POSN >> 2];
    uint32_t offs = s->regs[ATI_CUR_HORZ_VERT_OFF >> 2];
    /*
     * CLR registers hold the color in bits 31-8 as R,G,B (high to
     * low, same decoding as DingusPPC's draw_hw_cursor); QEMUCursor
     * data is RGBA byte order, i.e. dword (a<<24)|(b<<16)|(g<<8)|r
     * per ui/cursor.c.
     */
    uint32_t raw0 = s->regs[ATI_CUR_CLR0 >> 2];
    uint32_t raw1 = s->regs[ATI_CUR_CLR1 >> 2];
    uint32_t clr0 = 0xff000000u | (((raw0 >> 8) & 0xff) << 16) |
                    (((raw0 >> 16) & 0xff) << 8) | ((raw0 >> 24) & 0xff);
    uint32_t clr1 = 0xff000000u | (((raw1 >> 8) & 0xff) << 16) |
                    (((raw1 >> 16) & 0xff) << 8) | ((raw1 >> 24) & 0xff);
    uint32_t vram_off = (s->regs[ATI_CUR_OFFSET >> 2] & 0xfffff) * 8;
    bool on = (s->regs[ATI_GEN_TEST_CNTL >> 2] & ATI_GEN_CUR_ENABLE) != 0 &&
              !s->host_cursor_elsewhere;
    /*
     * The guest positions the cursor IMAGE at POSN minus the
     * HORZ/VERT_OFF clip offsets -- i.e. OFF acts as a hotspot-style
     * correction, not just edge clipping (DingusPPC's
     * get_cursor_position computes the draw position the same way).
     * Drawing at raw POSN instead put the visible arrow up to 63px
     * away from the guest's true click position, making the UI
     * un-aimable. The display backends ignore QEMUCursor hotspots, so
     * apply the correction here and report hotspot 0.
     */
    int x = (int)(posn & ATI_CUR_POSN_MASK) -
            (int)(offs & ATI_CUR_OFF_MASK);
    int y = (int)((posn >> 16) & ATI_CUR_POSN_MASK) -
            (int)((offs >> 16) & ATI_CUR_OFF_MASK);
    const uint8_t *src;
    QEMUCursor *c;
    int px, row;

    if (on) {
        if (vram_off + 64 * 64 / 4 > ATI_MACH64_VRAM_SIZE) {
            return;
        }
        src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram) + vram_off;
        c = cursor_alloc(64, 64);
        /* Hotspot left at 0 -- the OFF correction is applied to the
         * reported position above, since backends ignore hotspots. */
        for (row = 0; row < 64; row++) {
            for (px = 0; px < 64; px++) {
                uint8_t code = (src[row * 16 + px / 4] >> ((px % 4) * 2)) & 3;
                uint32_t val;

                switch (code) {
                case 0:
                    val = clr0;
                    break;
                case 1:
                    val = clr1;
                    break;
                case 3:
                    val = 0x80000000u; /* approximate "invert" */
                    break;
                default:
                    val = 0; /* transparent */
                    break;
                }
                c->data[row * 64 + px] = val;
            }
        }
        qemu_console_set_cursor(s->con, c);
        cursor_unref(c);
    }
    qemu_console_set_mouse(s->con, x, y, on);
}

/*
 * Classic Mac OS low-memory globals for mouse position and button
 * state (Guide to Macintosh Family Hardware / SysEqu.a -- fixed
 * addresses, stable across boots and OS versions, physical/identity-
 * mapped in guest RAM). MTemp/RawMouse/Mouse are each a packed
 * QuickDraw Point: high 16 bits = v (Y), low 16 bits = h (X); normally
 * kept in sync by the guest's own ADB-interrupt-time completion
 * handler (MTemp, updated first) and CrsrVBLTask (RawMouse/Mouse,
 * copied from MTemp each cycle, pinned to CrsrPin). MBState is a
 * single byte, bit 7 clear (0x00) = button down, set (0xff in
 * practice) = up -- also normally updated directly by the ADB
 * completion handler, matching real ADB's own active-low convention.
 * This is exactly the historic "$172" address this project's own
 * investigation spent many passes characterizing as "the wait" --
 * $172 is literally MBState. See the WORKAROUND comment below for why
 * we write these directly instead of leaving it to the guest.
 */
#define MAC_LOWMEM_MTEMP     0x828
#define MAC_LOWMEM_RAWMOUSE  0x82c
#define MAC_LOWMEM_MOUSE     0x830
#define MAC_LOWMEM_MBSTATE   0x172

/*
 * WORKAROUND -- see the field comment on host_cursor_x/y in
 * ati_mach64.h for why this exists: real hardware never does this,
 * software always drives CUR_HORZ_VERT_POSN, and it certainly never
 * pokes the guest OS's own low-memory globals directly. Tracks the
 * real host pointer/button state and writes it both into the hardware
 * cursor position registers (so the on-screen pointer is visible and
 * usable) and directly into the guest's MTemp/RawMouse/Mouse/MBState
 * low-memory globals, standing in for the guest's own stalled
 * ADB-completion handler and CrsrVBLTask. Without the low-memory
 * writes, the visible cursor moves but the guest's own click-location
 * and button-state bookkeeping stays frozen whenever its dispatch is
 * stuck, so clicks land wherever it was last stalled (or don't
 * register at all) rather than where the pointer visibly is -- worse
 * than doing nothing. This only affects the *guest's own bookkeeping*
 * globals; ADB's own existing path is unaffected and still separately
 * delivers real button transitions to the ADB device model.
 */

/*
 * Classic Mac OS keeps every attached display in a linked list of
 * GDevice records, each carrying its rectangle in GLOBAL desktop
 * coordinates -- so the guest's own arrangement (which screen is left
 * of which, set in the Monitors control panel) can be read straight out
 * of guest memory rather than assumed. DeviceList (low memory 0x8A8) is
 * a handle to the first GDevice; gdRect sits at offset 0x22 (top, left,
 * bottom, right as big-endian int16) and gdNextGD, itself a handle, at
 * 0x1E.
 */
#define MAC_LOWMEM_DEVICELIST 0x8a8
#define MAC_LOWMEM_CRSRPIN    0x834
#define MAC_GDEVICE_GDPMAP    0x16
#define MAC_GDEVICE_GDNEXTGD  0x1e
#define MAC_GDEVICE_GDRECT    0x22
#define MAC_MAX_SCREENS       4

typedef struct MacScreen {
    int x0, y0, x1, y1;
    uint32_t base;      /* PixMap baseAddr, i.e. which card owns it */
} MacScreen;

static bool mac_read_be32(uint32_t addr, uint32_t *out)
{
    uint32_t v;

    if (!addr || addr >= 0x10000000) {
        return false;
    }
    if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                           &v, 4) != MEMTX_OK) {
        return false;
    }
    *out = be32_to_cpu(v);
    return true;
}

static int mac_read_screens(MacScreen *scr, int max)
{
    uint32_t handle, gd;
    int n = 0;

    if (!mac_read_be32(MAC_LOWMEM_DEVICELIST, &handle) ||
        !mac_read_be32(handle, &gd)) {
        return 0;
    }
    while (gd && n < max) {
        uint16_t raw[4];
        uint32_t nexth, pmh, pm;
        int16_t r[4];
        int i;

        if (address_space_read(&address_space_memory, gd + MAC_GDEVICE_GDRECT,
                               MEMTXATTRS_UNSPECIFIED, raw, 8) != MEMTX_OK) {
            break;
        }
        for (i = 0; i < 4; i++) {
            r[i] = (int16_t)be16_to_cpu(raw[i]);
        }
        /* top, left, bottom, right -- reject anything implausible */
        if (r[2] <= r[0] || r[3] <= r[1] ||
            r[2] - r[0] > 4096 || r[3] - r[1] > 4096) {
            break;
        }
        scr[n].y0 = r[0];
        scr[n].x0 = r[1];
        scr[n].y1 = r[2];
        scr[n].x1 = r[3];
        scr[n].base = 0;
        if (mac_read_be32(gd + MAC_GDEVICE_GDPMAP, &pmh) &&
            mac_read_be32(pmh, &pm)) {
            mac_read_be32(pm, &scr[n].base);
        }
        n++;

        if (!mac_read_be32(gd + MAC_GDEVICE_GDNEXTGD, &nexth) ||
            !mac_read_be32(nexth, &gd)) {
            break;
        }
    }
    return n;
}


/*
 * Which of the guest's screens is this card driving? Matching the
 * GDevice's PixMap baseAddr against our own VRAM aperture is exact; the
 * mode-size comparison is only a fallback for when the PixMap could not
 * be read, and it prefers the screen at the desktop origin since two
 * heads may well be running the same resolution.
 */
static int mac_find_own_screen(ATIMach64State *s, const MacScreen *scr, int n,
                               const ATIMach64Mode *mode)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t vbase = pdev->io_regions[0].addr;
    uint64_t vsize = pdev->io_regions[0].size;
    int i;

    for (i = 0; i < n; i++) {
        if (vbase != PCI_BAR_UNMAPPED && scr[i].base >= vbase &&
            scr[i].base < vbase + vsize) {
            return i;
        }
    }
    for (i = 0; i < n; i++) {
        if (scr[i].x0 == 0 && scr[i].y0 == 0 &&
            scr[i].x1 == (int)mode->width && scr[i].y1 == (int)mode->height) {
            return i;
        }
    }
    return -1;
}

static void ati_mach64_host_cursor_event(DeviceState *dev, QemuConsole *src,
                                         QemuInputEvent *evt)
{
    ATIMach64State *s = ATI_MACH64(dev);
    ATIMach64Mode mode;
    uint32_t point_be;
    int old_x, old_y;
    bool was_elsewhere;

    ati_mach64_get_mode(s, &mode);
    if (!ati_mach64_mode_valid(s, &mode)) {
        return;
    }

    /*
     * Button/MBState handling deliberately removed (2026-07-29): once
     * virtual memory is disabled in the guest, ADB click delivery works
     * correctly through the guest's own native path -- confirmed via
     * live testing with this host-side button write disabled. Writing
     * MBState here as well would race the guest's own now-working
     * update and risks corrupting it. Only cursor *position* remains
     * host-tracked, since CrsrVBLTask (the VBL-driven cursor redraw
     * task) still doesn't get serviced by the guest on its own.
     */
    if (evt->type != INPUT_EVENT_KIND_REL) {
        return;
    }

    /*
     * Ignore implausibly large single-event deltas: the SDL backend
     * synthesizes window-sized relative jumps on grab/ungrab and on
     * the pointer entering the window (warp compensation), which are
     * not real hand motion -- integrating one slams the tracked
     * position into a screen corner. Real pointing devices deliver
     * far smaller per-event deltas.
     */
    if (evt->rel.value > 256 || evt->rel.value < -256) {
        return;
    }

    old_x = s->host_cursor_x;
    old_y = s->host_cursor_y;
    was_elsewhere = s->host_cursor_elsewhere;
    if (evt->rel.axis == INPUT_AXIS_X) {
        s->host_cursor_x += evt->rel.value;
    } else if (evt->rel.axis == INPUT_AXIS_Y) {
        s->host_cursor_y += evt->rel.value;
    } else {
        return;
    }

    /*
     * Track across the WHOLE desktop, not just this card's screen.
     * Clamping to our own mode (as this used to) silently discarded any
     * motion past our edge, so on a multi-head setup the pointer could
     * never reach the second display at all. The guest's own GDevice
     * list gives the real arrangement, so the position published here
     * stays in the global coordinates Mac OS hit-tests clicks against.
     */
    {
        MacScreen scr[MAC_MAX_SCREENS];
        int n = mac_read_screens(scr, MAC_MAX_SCREENS);
        int mine = mac_find_own_screen(s, scr, n, &mode);
        int here = -1, i;

        if (mine < 0) {
            /* No usable desktop map -- behave as the single-screen code did */
            s->host_cursor_elsewhere = false;
            s->host_cursor_x = MIN(MAX(s->host_cursor_x, 0),
                                   (int)mode.width - 1);
            s->host_cursor_y = MIN(MAX(s->host_cursor_y, 0),
                                   (int)mode.height - 1);
            s->regs[ATI_CUR_HORZ_VERT_POSN >> 2] =
                ((uint32_t)s->host_cursor_y << 16) | (uint32_t)s->host_cursor_x;
        } else {
            /*
             * Keep CrsrPin -- the rectangle the Cursor Manager pins the
             * mouse to -- covering the whole desktop. Normally the
             * guest's own cursor task widens it when a second screen
             * appears, but this workaround exists precisely because that
             * task does not run, so it was left describing the main
             * screen alone (measured live: GrayRgn spanned 2048 px while
             * CrsrPin still stopped at 1024). Toolbox paths that clamp a
             * reported point to CrsrPin then fold anything happening on
             * the second screen back onto the first.
             */
            int px0 = scr[0].x0, py0 = scr[0].y0;
            int px1 = scr[0].x1, py1 = scr[0].y1;
            uint16_t pin[4];

            for (i = 1; i < n; i++) {
                px0 = MIN(px0, scr[i].x0);
                py0 = MIN(py0, scr[i].y0);
                px1 = MAX(px1, scr[i].x1);
                py1 = MAX(py1, scr[i].y1);
            }
            pin[0] = cpu_to_be16(py0);
            pin[1] = cpu_to_be16(px0);
            pin[2] = cpu_to_be16(py1);
            pin[3] = cpu_to_be16(px1);
            address_space_write(&address_space_memory, MAC_LOWMEM_CRSRPIN,
                                MEMTXATTRS_UNSPECIFIED, pin, sizeof(pin));

            for (i = 0; i < n; i++) {
                if (s->host_cursor_x >= scr[i].x0 &&
                    s->host_cursor_x <  scr[i].x1 &&
                    s->host_cursor_y >= scr[i].y0 &&
                    s->host_cursor_y <  scr[i].y1) {
                    here = i;
                    break;
                }
            }
            /*
             * The union of the screens is not necessarily a rectangle, so
             * rather than clamp to a bounding box that may contain dead
             * space, simply refuse a step that would leave the desktop --
             * the same pinning behaviour Mac OS applies via CrsrPin.
             */
            if (here < 0) {
                s->host_cursor_x = old_x;
                s->host_cursor_y = old_y;
                return;
            }

            s->host_cursor_elsewhere = (here != mine);
            if (here == mine) {
                s->regs[ATI_CUR_HORZ_VERT_POSN >> 2] =
                    ((uint32_t)(s->host_cursor_y - scr[mine].y0) << 16) |
                    (uint32_t)(s->host_cursor_x - scr[mine].x0);
                if (was_elsewhere) {
                    ati_rage128_host_cursor(0, 0, false);
                }
            } else {
                /* Off our head: park our sprite and light up the other card */
                s->regs[ATI_CUR_HORZ_VERT_POSN >> 2] = 0x07ff07ff;
                ati_rage128_host_cursor(s->host_cursor_x - scr[here].x0,
                                        s->host_cursor_y - scr[here].y0, true);
            }
        }
    }

    point_be = cpu_to_be32(((uint32_t)(uint16_t)s->host_cursor_y << 16) |
                           (uint32_t)(uint16_t)s->host_cursor_x);
    address_space_write(&address_space_memory, MAC_LOWMEM_MTEMP,
                        MEMTXATTRS_UNSPECIFIED, &point_be, 4);
    address_space_write(&address_space_memory, MAC_LOWMEM_RAWMOUSE,
                        MEMTXATTRS_UNSPECIFIED, &point_be, 4);
    address_space_write(&address_space_memory, MAC_LOWMEM_MOUSE,
                        MEMTXATTRS_UNSPECIFIED, &point_be, 4);
    ati_mach64_cursor_update(s);
}

static const QemuInputHandler ati_mach64_cursor_handler = {
    .name  = "ATI Mach64 hardware cursor (host-tracking workaround)",
    .mask  = INPUT_EVENT_MASK_REL,
    .event = ati_mach64_host_cursor_event,
};

/*
 * Interrupt semantics -- gated pulse (empirically the only shape both
 * boot phases accept; all four model variants were tested against the
 * real ROM on 2026-07-28, see the investigation notes' 140th pass):
 *
 * - INT status bits are set ONLY at a vertical-blank boundary while
 *   the CRTC is actually running (GEN_CNTL: CRTC_ENABLE set,
 *   DISPLAY_DIS clear) -- matching DingusPPC. A free-running timer
 *   from t=0 plus line-recompute-on-EN-write wedged early boot (the
 *   FCode's mid-bring-up INT_EN write met stale pending state).
 * - The line PULSES: asserted at blank start when an INT source is
 *   enabled, deasserted at blank end unconditionally. The early ROM
 *   never acks the device -- it consumes one PIC EDGE per frame (a
 *   VBL event counter), so any held-until-acked variant starves it
 *   and hangs the bring-up; with the pulse the whole system boots to
 *   the desktop under the REAL Heathrow routing (GPU on line 0x16).
 * - Known residual gap: at the desktop, Mac OS's per-source interrupt
 *   dispatch services this source only twice and then stops (vs
 *   DingusPPC's ndrv acking continuously), leaving the VBL task queue
 *   -- and thus mouse-pointer motion -- stalled. The latched INT bits
 *   stay readable/ackable; the discriminating dispatcher behavior is
 *   still under investigation.
 */
static void ati_mach64_vblank_end_tick(void *opaque)
{
    ATIMach64State *s = opaque;

    pci_set_irq(PCI_DEVICE(s), 0);
}

static void ati_mach64_vblank_timer_tick(void *opaque)
{
    ATIMach64State *s = opaque;
    uint32_t gen_cntl = s->regs[ATI_CRTC_GEN_CNTL >> 2];
    uint32_t int_cntl;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next_blank;

    if ((gen_cntl & ATI_CRTC_ENABLE) && !(gen_cntl & ATI_CRTC_DISPLAY_DIS)) {
        s->regs[ATI_CRTC_INT_CNTL >> 2] |= ATI_CRTC_VBLANK_INT |
                                           ATI_CRTC_VLINE_INT;
        int_cntl = s->regs[ATI_CRTC_INT_CNTL >> 2];
        if (int_cntl & (ATI_CRTC_VBLANK_INT_EN | ATI_CRTC_VLINE_INT_EN)) {
            trace_ati_mach64_vblank_irq(1, int_cntl);
            pci_set_irq(PCI_DEVICE(s), 1);
            timer_mod(s->vblank_end_timer, now + ATI_MACH64_VBLANK_IRQ_LEN_NS);
        }
    }

    /* Tick at each frame's blank-start phase, agreeing with the
     * phase-computed live VBLANK status bit (last 1/8 of each period). */
    next_blank = (now / ATI_MACH64_VBLANK_PERIOD_NS + 1) *
                 ATI_MACH64_VBLANK_PERIOD_NS - ATI_MACH64_VBLANK_LEN_NS;
    if (next_blank <= now) {
        next_blank += ATI_MACH64_VBLANK_PERIOD_NS;
    }
    timer_mod(s->vblank_timer, next_blank);
}

static void ati_mach64_set_palette_color(ATIMach64State *s, uint8_t index,
                                         uint8_t r, uint8_t g, uint8_t b)
{
    s->palette[index][0] = r;
    s->palette[index][1] = g;
    s->palette[index][2] = b;
}

/*
 * Classic 3-pin "Apple Monitor Sense" identification code for a common,
 * real Mac display type (std=6/ext=0x2B, "13"/14" RGB or 12" Monochrome"
 * -- DingusPPC's "HiRes12-14in", also its own alias target for the
 * commonly-used AppleVision1710). Any valid, real code works equally
 * well for boot purposes; this one is a safe, ordinary default.
 *
 * Tested DingusPPC's real "NotConnected" code (std=7/ext=0x3F,
 * "kESCSevenNoDisplay") here instead: confirmed a real, visible
 * behavioral difference (unlike the chip-identity tests, which had zero
 * effect) -- with this code, GP_IO (0x78) is never polled at all, and
 * the ROM's early boot-time splash-screen setup skips video entirely.
 * That's the *correct* behavior for a genuinely disconnected monitor, so
 * it confirms the sense-code mechanism works and that a valid, connected
 * code (kept here) is the right choice for testing the actual boot path.
 */
/*
 * 2026-07-28 update: the "any valid code works equally well" assumption
 * above is under test. An instrumented DingusPPC boot (which passes
 * --mon_id=Multiscan20in, std=6/ext=0x23, "MultiScan Band-3") showed
 * Open Firmware itself performing the full 640x480 CRTC mode-set
 * (H_TOTAL_DISP=0x004F006B) at ~2.9s, before any 68K activity -- while
 * our boot, presenting as the fixed-frequency HiRes12-14in (ext=0x2B),
 * never gets those dimension writes. The ROM FCode's mode-table lookup
 * (word 0x8da/table 0x876, see investigation notes) plausibly maps the
 * two codes to different slots, including zero/skip entries. Presenting
 * the same monitor as the known-working reference boot to test this.
 */
#define ATI_APPLESENSE_STD_CODE 6
#define ATI_APPLESENSE_EXT_CODE 0x23

/*
 * GP_IO models the Mach64's bit-banged monitor-sense GPIO lines (3
 * sense pins, each independently switchable between output-low and
 * input-pulled-up). Confirmed via -trace ati_mach64_mmio_write against
 * this exact ROM's real boot-time ATI bring-up sequence -- once decoded
 * with the correct byte order (see below), it pulls each of the 3
 * sense lines low in turn while reading the other two, exactly the
 * classic Apple Monitor Sense probe (Technical Note HW30), not DDC/I2C
 * as originally assumed -- and that without a real response here, ROM
 * bring-up never gets far enough to touch any CRTC mode register
 * (H_TOTAL_DISP/V_TOTAL_DISP/OFF_PITCH/GEN_CNTL) at all.
 *
 * Byte order: real Mach64 silicon is native little-endian and this
 * whole MMIO block is declared DEVICE_BIG_ENDIAN (see the endianness
 * fix elsewher in this file/session for the CRTC registers), but this
 * specific register's guest accesses are apparently byte-reversed
 * relative to that -- confirmed empirically: reversing the bytes of
 * `word` before decoding turns an otherwise-always-zero "direction"
 * byte into the expected one-hot 4/2/1 sequence (sense line 2, then 1,
 * then 0, pulled low in turn) that DingusPPC's own AppleSense switch
 * (ported below from DisplayID::read_monitor_sense) expects.
 */
static uint32_t ati_mach64_gp_io_write(ATIMach64State *s, uint32_t word)
{
    /*
     * 2026-07-28: the bswap32 in/out pair that used to live here was
     * compensating for the region's wrong DEVICE_BIG_ENDIAN declaration
     * (see ati_mach64_mmio_ops); with the region now correctly
     * little-endian, guest dwords arrive unswapped and this matches
     * DingusPPC's ATIRage GP_IO handling directly (levels byte at bits
     * 8-15, dirs byte at bits 24-31).
     */
    uint32_t rword = word;
    uint8_t raw_levels = (rword >> 8) & 0xff;
    uint8_t raw_dirs = (rword >> 24) & 0xff;
    uint8_t lvl3, dir3;         /* 3-bit logical sense code: bit0, SCL, SDA */
    uint8_t appsense_lvl, appsense_dir;
    uint8_t result;

    /*
     * The three physical GPIO sense pins map to a 3-bit logical code:
     * physical bit 0 -> logical bit 0, physical bit 4 -> logical bit 1
     * (I2C SCL), physical bit 5 -> logical bit 2 (I2C SDA).
     */
    lvl3 = ((raw_levels & 0x30) >> 3) | (raw_levels & 1);
    dir3 = ((raw_dirs & 0x30) >> 3) | (raw_dirs & 1);

    /*
     * Always drive the DDC/I2C engine from the SCL/SDA lines so the
     * EDID slave stays in sync: a line configured as output presents
     * its level, an input line reads high (pulled up). This coexists
     * with the Apple Monitor Sense response below because the ROM's
     * sense probe uses one-hot direction patterns (exactly one line
     * driven low) that never occur during real I2C signalling.
     */
    {
        int scl = (dir3 & 2) ? !!(lvl3 & 2) : 1;
        int sda = (dir3 & 4) ? !!(lvl3 & 4) : 1;

        bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SCL, scl);
        sda = bitbang_i2c_set(&s->bbi2c, BITBANG_I2C_SDA, sda);
        /* I2C readback: SCL -> logical bit 1, SDA -> logical bit 2 */
        s->i2c_sense = (scl ? 2 : 0) | (sda ? 4 : 0);
    }

    /* Apple Monitor Sense probe decode (unchanged; boot mode-set relies
     * on this). Uses its own munged view of the same pins. */
    appsense_lvl = (lvl3 ^ 7) & ~dir3;
    appsense_dir = dir3;

    switch ((appsense_dir << 3) | appsense_lvl) {
    case 0043: /* sense line 2 pulled low; read sense line 1 and 0 */
        /*
         * With no monitor plugged in there's no pull-down resistor
         * network at all: every sense line just floats to its
         * pulled-up high default regardless of which one we're
         * driving low, i.e. every probe reads back "both other lines
         * high" (3) -- the real "no monitor connected" pattern
         * (overall code 7), as opposed to a specific monitor's fixed
         * resistor-encoded ID. Real Old World Macs pick their
         * boot/console display by which port actually senses a
         * monitor; the monitor-connected property lets that same
         * real mechanism be tested here.
         */
        result = s->monitor_connected ? (ATI_APPLESENSE_EXT_CODE & 0060) >> 4
                                       : 3;
        break;
    case 0025: /* sense line 1 pulled low; read sense line 2 and 0 */
        result = s->monitor_connected
                     ? ((ATI_APPLESENSE_EXT_CODE & 0010) >> 1) |
                       ((ATI_APPLESENSE_EXT_CODE & 0004) >> 2)
                     : 3;
        break;
    case 0016: /* sense line 0 pulled low; read sense line 2 and 1 */
        result = s->monitor_connected ? (ATI_APPLESENSE_EXT_CODE & 0003) << 1
                                       : 3;
        break;
    default:
        /*
         * Not an Apple-sense probe: the guest is either bit-banging
         * I2C or reading the bus at rest. Return the live I2C line
         * state so DDC EDID reads work.
         */
        result = s->i2c_sense;
        break;
    }

    rword &= ~(0xffU << 8);
    rword |= (((result & 6) << 3) | (result & 1)) << 8;
    return rword;
}

static uint64_t ati_mach64_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    ATIMach64State *s = opaque;
    uint32_t reg_num = (addr & ~3U) >> 2;
    uint32_t byte_off = addr & 3;
    uint32_t result;

    if (reg_num >= ATI_MACH64_NUM_REGS) {
        return 0;
    }
    result = s->regs[reg_num];

    switch (addr & ~3U) {
    case ATI_GUI_STAT:
        /*
         * Report the GUI (2D) engine as idle with a completely free
         * command FIFO. Bit 0 (GUI_ACTIVE) = 0 means idle; bits 16-23
         * (FIFO_CNT) hold the number of free FIFO entries. Our engine
         * completes every operation synchronously, so it is always
         * idle with a fully-available FIFO. The Rage Pro (GT) has a
         * 48-entry command FIFO; a driver that waits for FIFO space
         * before writing engine registers (Mac OS 9's ATI Graphics
         * Accelerator does, polling here) spins forever if this field
         * reads 0. Matches DingusPPC's ATIRage GUI_STAT "pretend empty
         * FIFO" (cmd_fifo_size << 16).
         */
        result = ATI_MACH64_GUI_FIFO_SIZE << 16;
        break;
    case ATI_FIFO_STAT:
        /* Per-slot "in use" bitmask (low 16 bits) + error (bit 31);
         * an idle engine has an empty FIFO and no error. */
        result = 0;
        break;
    case ATI_CLOCK_CNTL:
    {
        uint8_t pll_addr = (result >> ATI_PLL_ADDR_SHIFT) & ATI_PLL_ADDR_MASK;
        result &= ~(ATI_PLL_DATA_MASK << ATI_PLL_DATA_SHIFT);
        result |= ((uint32_t)s->plls[pll_addr] & ATI_PLL_DATA_MASK) <<
                  ATI_PLL_DATA_SHIFT;
        break;
    }
    case ATI_DAC_REGS:
        switch (addr) {
        case ATI_DAC_W_INDEX:
            result = (result & ~0xffu) | s->dac_wr_index;
            break;
        case ATI_DAC_MASK:
            result = (result & ~0xffu) | s->dac_mask;
            break;
        case ATI_DAC_R_INDEX:
            result = (result & ~0xffu) | s->dac_rd_index;
            break;
        case ATI_DAC_DATA:
        {
            uint8_t comp = s->palette[s->dac_rd_index][s->dac_comp_index];
            result = (result & ~0xffu) | comp;
            if (++s->dac_comp_index >= 3) {
                s->dac_comp_index = 0;
                s->dac_rd_index++;
            }
            break;
        }
        default:
            break;
        }
        break;
    case ATI_CONFIG_CHIP_ID:
        /* fixed: asic id 0x5c, dev id 0x4750 (see ati_mach64.h) */
        result = (ATI_RAGE_PRO_ASIC_ID << ATI_CFG_CHIP_MAJOR_SHIFT) |
                 (PCI_DEVICE_ID_ATI_RAGE_PRO << ATI_CFG_CHIP_TYPE_SHIFT);
        break;
    case ATI_CRTC_INT_CNTL:
    {
        /*
         * Bit 0 (VBLANK) is a LIVE, read-only status bit reflecting
         * whether the CRTC's timing generator is currently within its
         * vertical blanking interval -- distinct from bit 2
         * (VBLANK_INT), the latched/acknowledgeable interrupt flag.
         * Confirmed against DingusPPC's register-compatible atirage.cpp
         * (ATI_CRTC_VBLANK = bit 0, updated via insert_bits() from the
         * live "irq_line_state", separately from the latched
         * VBLANK_INT). Real firmware polls this bit as a hardware
         * self-test ("is the CRT timing generator actually running")
         * before committing to a full mode-set; without it ever
         * toggling, our own boot never got past this exact check to
         * program CRTC_GEN_CNTL/H_TOTAL_DISP/V_TOTAL_DISP/OFF_PITCH at
         * all. Modeled as the last ~12.5% of each 60Hz frame period,
         * a plausible real-CRT vblank duty cycle -- except under icount,
         * where the "off" gap is instead pinned to a small fixed slice of
         * virtual time (see ATI_MACH64_VBLANK_ICOUNT_GAP_NS above).
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t phase = now % ATI_MACH64_VBLANK_PERIOD_NS;
        int64_t vblank_start = icount_enabled()
            ? ATI_MACH64_VBLANK_ICOUNT_GAP_NS
            : (ATI_MACH64_VBLANK_PERIOD_NS * 7 / 8);
        bool in_vblank = phase >= vblank_start;

        result = (result & ~ATI_CRTC_VBLANK) | (in_vblank ? ATI_CRTC_VBLANK : 0);
        break;
    }
    default:
        break;
    }

    if (size == 4 && byte_off == 0) {
        trace_ati_mach64_mmio_read(size, addr,
                                   ati_mach64_reg_name(addr & ~3ull),
                                   result);
        return result;
    }
    /* Reverted along with the write path -- see the shift comment
     * there for why byte_off*8 (not top-down) is the real convention. */
    result = (result >> (byte_off * 8)) & ((1ull << (size * 8)) - 1);
    trace_ati_mach64_mmio_read(size, addr,
                                   ati_mach64_reg_name(addr & ~3ull),
                                   result);
    return result;
}

static void ati_mach64_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size);

/*
 * Deliver one DMA'd dword to a frame-buffer-aperture offset. The
 * aperture's register windows are part of that address space, so a
 * descriptor's FRAME_BUF_OFFSET may target plain VRAM, a block-0
 * register (0x7FFC00 window -- HOST_DATA and BM_HOSTDATA are the
 * ports drivers stream draw-engine pixel data through) or a block-1
 * register (0x7FF800 window).
 */
static void ati_mach64_bm_deliver(ATIMach64State *s, uint32_t fbo,
                                  uint32_t word)
{
    if (fbo >= ATI_MACH64_REGS_IN_VRAM_OFFSET &&
        fbo < ATI_MACH64_REGS_IN_VRAM_OFFSET + 0x400) {
        ati_mach64_mmio_write(s, fbo & 0x3ff, word, 4);
    } else if (fbo >= ATI_MACH64_REGS_IN_VRAM_OFFSET - 0x400 &&
               fbo < ATI_MACH64_REGS_IN_VRAM_OFFSET) {
        ati_mach64_mmio_write(s, 0x400 + (fbo & 0x3ff), word, 4);
    } else if (fbo + 4 <= ATI_MACH64_VRAM_SIZE) {
        uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
        stl_le_p(vram + fbo, word);
        memory_region_set_dirty(&s->vram, fbo, 4);
    }
}

/*
 * GUI bus master (LT3REGRE 6.2.2): walk the descriptor table at the
 * given physical address, DMAing each entry's system-memory data into
 * the frame-buffer aperture -- with FRAME_OFFSET_HOLD, every dword
 * goes to the same offset (a register port such as HOST_DATA), which
 * is how the accelerated Mac OS NDRV feeds image data to the draw
 * engine without a single MMIO data write. Transfers complete
 * synchronously; END_OF_LIST finishes the walk and latches the
 * bus-master-complete interrupt.
 */
static void ati_mach64_bm_gui_run(ATIMach64State *s, uint32_t table)
{
    PCIDevice *pci = PCI_DEVICE(s);
    dma_addr_t desc = table & ~0xfu;
    int entry;

    for (entry = 0; entry < 4096; entry++) {
        uint32_t d[4];
        uint32_t fbo, sysaddr, cmd, count;
        bool hold;

        if (pci_dma_read(pci, desc, d, sizeof(d)) != MEMTX_OK) {
            break;
        }
        fbo = le32_to_cpu(d[0]);
        sysaddr = le32_to_cpu(d[1]);
        cmd = le32_to_cpu(d[2]);
        count = cmd & 0xffff;
        hold = cmd & ATI_BM_FRAME_OFFSET_HOLD;
        s->regs[ATI_BM_FRAME_BUF_OFFSET >> 2] = fbo;
        s->regs[ATI_BM_SYSTEM_MEM_ADDR >> 2] = sysaddr;
        s->regs[ATI_BM_COMMAND >> 2] = cmd;
        trace_ati_mach64_bm_desc(fbo, sysaddr, cmd);

        while (count >= 4) {
            uint32_t word;

            if (pci_dma_read(pci, sysaddr, &word, 4) != MEMTX_OK) {
                break;
            }
            ati_mach64_bm_deliver(s, fbo, le32_to_cpu(word));
            sysaddr += 4;
            count -= 4;
            if (!hold) {
                fbo += 4;
            }
        }

        if (cmd & ATI_BM_END_OF_LIST) {
            break;
        }
        desc += 16;
    }

    s->regs[ATI_CRTC_INT_CNTL >> 2] |= ATI_BUSMASTER_EOL_INT;
    if (s->regs[ATI_CRTC_INT_CNTL >> 2] & ATI_BUSMASTER_EOL_INT_EN) {
        pci_set_irq(PCI_DEVICE(s), 1);
    }
}

static void ati_mach64_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    ATIMach64State *s = opaque;
    uint32_t reg_num = (addr & ~3U) >> 2;
    uint32_t byte_off = addr & 3;
    uint32_t word = s->regs[reg_num];
    /*
     * REVERTED: an earlier "endianness fix" here (shift counting down
     * from the top, i.e. byte_off=0 = MSB) was based on a mistaken
     * theoretical assumption about DEVICE_BIG_ENDIAN byte addressing,
     * and was never cross-checked against a real working trace before
     * being applied. DingusPPC's own verbose log of a genuine successful
     * boot disproves it directly: "write CRTC_GEN_CNTL 001f.b = 01"
     * immediately following a full-word write of 0x01000240 results in
     * the register staying "= 01000240" (unchanged) -- only possible if
     * byte_off=3 (address+3) maps to bits 31-24 (this original
     * byte_off*8 formula), matching DingusPPC's own write_reg()
     * (insert_bits(val, value, offset * 8, size * 8)). The "fixed"
     * top-down formula would have produced 0x01000201 instead,
     * contradicting the real observed behavior. Kept as byte_off*8.
     */
    uint32_t shift = byte_off * 8;
    uint32_t mask = (size >= 4) ? 0xffffffffu : (((1u << (size * 8)) - 1) << shift);

    if (reg_num >= ATI_MACH64_NUM_REGS) {
        return;
    }

    trace_ati_mach64_mmio_write(size, addr,
                                ati_mach64_reg_name(addr & ~3ull), data);
    word = (word & ~mask) | ((uint32_t)data << shift);

    /*
     * Writing any register except BM_ADDR resets the BM_ADDR/BM_DATA
     * port to address mode (LT3REGRE 6-12, explicit note).
     */
    if ((addr & ~3U) != ATI_BM_ADDR_DATA) {
        s->bm_data_mode = false;
    }

    switch (addr & ~3U) {
    case ATI_CONFIG_CHIP_ID:
        /* read-only on real hardware */
        return;
    case ATI_GP_IO:
        s->regs[reg_num] = ati_mach64_gp_io_write(s, word);
        return;
    case ATI_CLOCK_CNTL:
    {
        uint8_t pll_addr = (word >> ATI_PLL_ADDR_SHIFT) & ATI_PLL_ADDR_MASK;
        uint8_t pll_data = (word >> ATI_PLL_DATA_SHIFT) & ATI_PLL_DATA_MASK;
        s->plls[pll_addr] = pll_data;
        s->regs[reg_num] = word;
        return;
    }
    case ATI_DAC_REGS:
        switch (addr) {
        case ATI_DAC_W_INDEX:
            s->dac_wr_index = (uint8_t)data;
            break;
        case ATI_DAC_MASK:
            s->dac_mask = (uint8_t)data;
            break;
        case ATI_DAC_R_INDEX:
            s->dac_rd_index = (uint8_t)data;
            s->dac_comp_index = 0;
            break;
        case ATI_DAC_DATA:
            s->dac_comp_buf[s->dac_comp_index] = (uint8_t)data;
            if (++s->dac_comp_index >= 3) {
                ati_mach64_set_palette_color(s, s->dac_wr_index,
                                             s->dac_comp_buf[0],
                                             s->dac_comp_buf[1],
                                             s->dac_comp_buf[2]);
                s->dac_comp_index = 0;
                s->dac_wr_index++;
            }
            break;
        default:
            break;
        }
        s->regs[reg_num] = word;
        return;
    case ATI_CRTC_H_TOTAL_DISP:
    case ATI_CRTC_V_TOTAL_DISP:
    case ATI_CRTC_OFF_PITCH:
    case ATI_CRTC_GEN_CNTL:
        s->regs[reg_num] = word;
        s->mode_dirty = true;
        return;
    case ATI_CRTC_INT_CNTL:
    {
        /* VBLANK_INT (bit 2) and VLINE_INT (bit 4) are
         * write-1-to-acknowledge status bits; the EN bits are plain
         * read/write. Acks do not touch the interrupt line -- it is a
         * pulse driven purely by the vblank timers (see above); this
         * matches DingusPPC's own reference atirage.cpp exactly (its
         * write_reg() ack path only clears status bits and never calls
         * pci_interrupt() -- the line is driven solely by its vbl_cb
         * timer callback). Confirmed by direct source comparison. */
        uint32_t pending = s->regs[reg_num] &
                           (ATI_CRTC_VBLANK_INT | ATI_CRTC_VLINE_INT |
                            ATI_BUSMASTER_EOL_INT);
        if (word & ATI_CRTC_VBLANK_INT_AK) {
            pending &= ~ATI_CRTC_VBLANK_INT;
        }
        if (word & ATI_CRTC_VLINE_INT_AK) {
            pending &= ~ATI_CRTC_VLINE_INT;
        }
        if (word & ATI_BUSMASTER_EOL_INT_AK) {
            pending &= ~ATI_BUSMASTER_EOL_INT;
        }
        s->regs[reg_num] = (word & ~(ATI_CRTC_VBLANK_INT |
                                     ATI_CRTC_VLINE_INT |
                                     ATI_BUSMASTER_EOL_INT)) | pending;
        trace_ati_mach64_int_ack(word, pending);
        /*
         * Ack-driven deassertion, matching real silicon: once no
         * enabled interrupt status remains pending, drop the request
         * line immediately instead of waiting out the blanking-window
         * fallback timer. Heathrow's event latch excludes
         * level-triggered lines, so guests only ever see this
         * interrupt while the line is high -- see the
         * ATI_MACH64_VBLANK_IRQ_LEN_NS comment for the measured
         * dispatch-starvation this fixes.
         */
        if (!pending) {
            timer_del(s->vblank_end_timer);
            pci_set_irq(PCI_DEVICE(s), 0);
        }
        return;
    }
    case ATI_CUR_CLR0:
    case ATI_CUR_CLR1:
    case ATI_CUR_OFFSET:
    case ATI_CUR_HORZ_VERT_POSN:
    case ATI_CUR_HORZ_VERT_OFF:
    case ATI_GEN_TEST_CNTL:
        s->regs[reg_num] = word;
        ati_mach64_cursor_update(s);
        return;
    /*
     * GUI (2D) engine: composite registers mirror into their component
     * registers (the engine itself only reads the components), and a
     * DST_HEIGHT_WIDTH write triggers the operation -- the standard
     * Mach64 programming model (mach64_accel.c's draw_rect()).
     */
    case ATI_DST_Y_X:
        s->regs[reg_num] = word;
        s->regs[ATI_DST_X >> 2] = word >> 16;
        s->regs[ATI_DST_Y >> 2] = word & 0xffff;
        return;
    case ATI_SRC_Y_X:
        s->regs[reg_num] = word;
        s->regs[ATI_SRC_X >> 2] = word >> 16;
        s->regs[ATI_SRC_Y >> 2] = word & 0xffff;
        return;
    case ATI_SRC_HEIGHT1_WIDTH1:
        s->regs[reg_num] = word;
        s->regs[ATI_SRC_WIDTH1 >> 2] = word >> 16;
        s->regs[ATI_SRC_HEIGHT1 >> 2] = word & 0xffff;
        return;
    case ATI_SRC_Y_X_START:
        s->regs[reg_num] = word;
        s->regs[ATI_SRC_X_START >> 2] = word >> 16;
        s->regs[ATI_SRC_Y_START >> 2] = word & 0xffff;
        return;
    case ATI_SC_LEFT_RIGHT:
        s->regs[reg_num] = word;
        s->regs[ATI_SC_LEFT >> 2] = word & 0xffff;
        s->regs[ATI_SC_RIGHT >> 2] = word >> 16;
        return;
    case ATI_SC_TOP_BOTTOM:
        s->regs[reg_num] = word;
        s->regs[ATI_SC_TOP >> 2] = word & 0xffff;
        s->regs[ATI_SC_BOTTOM >> 2] = word >> 16;
        return;
    case ATI_DST_HEIGHT_WIDTH:
        s->regs[reg_num] = word;
        s->regs[ATI_DST_WIDTH >> 2] = word >> 16;
        s->regs[ATI_DST_HEIGHT >> 2] = word & 0xffff;
        ati_mach64_2d_op(s);
        return;
    case ATI_DST_X_Y:
        /* Same fields as DST_Y_X, opposite halves (see the header). */
        s->regs[reg_num] = word;
        s->regs[ATI_DST_Y >> 2] = word >> 16;
        s->regs[ATI_DST_X >> 2] = word & 0xffff;
        s->regs[ATI_DST_Y_X >> 2] = (word << 16) | (word >> 16);
        return;
    case ATI_DST_WIDTH_HEIGHT:
        s->regs[reg_num] = word;
        s->regs[ATI_DST_HEIGHT >> 2] = word >> 16;
        s->regs[ATI_DST_WIDTH >> 2] = word & 0xffff;
        s->regs[ATI_DST_HEIGHT_WIDTH >> 2] = (word << 16) | (word >> 16);
        ati_mach64_2d_op(s);
        return;
    case ATI_DST_BRES_LNTH:
        /*
         * Loading the line length starts a line draw unless the
         * disable / trapezoid gate bits say otherwise (RRG 4-46);
         * the length also mirrors into DST_WIDTH.
         */
        s->regs[reg_num] = word;
        s->regs[ATI_DST_WIDTH >> 2] = word & ATI_BRES_LNTH_MASK;
        if (word & ATI_BRES_DRAW_TRAP) {
            trace_ati_mach64_2d_unimp(s->regs[ATI_DP_SRC >> 2],
                                      s->regs[ATI_DP_MIX >> 2],
                                      s->regs[ATI_DP_PIX_WIDTH >> 2]);
        } else if (!(word & ATI_BRES_LINE_DIS)) {
            ati_mach64_2d_line(s, word);
        }
        return;
    case ATI_GUI_TRAJ_CNTL:
        /*
         * Composite of DST_CNTL, SRC_CNTL, PAT_CNTL and HOST_CNTL
         * (LT3REGRE 5-64, Rage Pro bit layout): [15:0] DST_CNTL,
         * [16] SRC_PATT_EN, [17] SRC_PATT_ROT_EN, [18] SRC_LINEAR_EN,
         * [19] SRC_BYTE_ALIGN, [20] SRC_LINE_X_DIR, [26:24] the
         * PAT_CNTL enables, [27] HOST_BYTE_ALIGN, [28]
         * HOST_BIG_ENDIAN_EN. Linux's atyfb programs its engine
         * defaults only through this register.
         */
        s->regs[reg_num] = word;
        s->regs[ATI_DST_CNTL >> 2] = word & 0xffff;
        s->regs[ATI_SRC_CNTL >> 2] = (word >> 16) & 0x1f;
        s->regs[ATI_PAT_CNTL >> 2] = (word >> 24) & 0x7;
        s->regs[ATI_HOST_CNTL >> 2] =
            ((word & (1u << 27)) ? ATI_HOST_BYTE_ALIGN : 0) |
            ((word & (1u << 28)) ? ATI_HOST_BIG_ENDIAN_EN : 0);
        return;
    case ATI_DP_SET_GUI_ENGINE2:
        s->regs[reg_num] = word;
        ati_mach64_set_gui_engine2(s, word);
        return;
    case ATI_DP_SET_GUI_ENGINE:
        /*
         * The DRAWING_COMBO-table macro variant; no observed guest
         * uses it (Apple's NDRV uses ENGINE2, Linux neither), so it
         * traces loudly instead of guessing at its packing.
         */
        s->regs[reg_num] = word;
        trace_ati_mach64_2d_unimp(s->regs[ATI_DP_SRC >> 2],
                                  s->regs[ATI_DP_MIX >> 2],
                                  s->regs[ATI_DP_PIX_WIDTH >> 2]);
        return;
    case ATI_ONE_OVER_AREA:
    case ATI_ONE_OVER_AREA_UC:
        s->regs[reg_num] = word;
        ati_mach64_3d_trigger(s, word);
        return;
    case ATI_HOST_DATA0 ... ATI_HOST_DATAF:
    case ATI_BM_HOSTDATA:
        /*
         * Pixel/mask words streamed (by the CPU or the bus master)
         * for a host-source blit set up in ati_mach64_2d_op().
         */
        s->regs[reg_num] = word;
        if (s->hb_active) {
            ati_mach64_host_data(s, word);
        }
        return;
    case ATI_BM_ADDR_DATA:
        /*
         * Dual-purpose port: address+count first, then data words for
         * consecutive block-0 registers (see ati_mach64_regs.h).
         */
        s->regs[reg_num] = word;
        if (!s->bm_data_mode) {
            s->bm_reg = word & 0xff;
            s->bm_reg_count = ((word >> 8) & 0x3fff) + 1;
            s->bm_data_mode = true;
        } else {
            ati_mach64_mmio_write(s, (uint32_t)s->bm_reg << 2, word, 4);
            s->bm_data_mode = true;     /* survive the reset-on-write rule */
            s->bm_reg++;
            if (--s->bm_reg_count <= 0) {
                s->bm_data_mode = false;
            }
        }
        return;
    case ATI_BM_GUI_TABLE:
    case ATI_BM_GUI_TABLE_CMD:
        s->regs[ATI_BM_GUI_TABLE >> 2] = word;
        s->regs[ATI_BM_GUI_TABLE_CMD >> 2] = word;
        ati_mach64_bm_gui_run(s, word);
        return;
    case ATI_BM_SYSTEM_TABLE:
        /*
         * The generic system bus master (fb<->sysmem block moves) is
         * not implemented yet; trace loudly so its first real user
         * shows up immediately.
         */
        s->regs[reg_num] = word;
        qemu_log_mask(LOG_UNIMP,
                      "ati-mach64: BM_SYSTEM_TABLE 0x%x unimplemented\n",
                      word);
        return;
    default:
        s->regs[reg_num] = word;
        return;
    }
}

/*
 * BAR2 ("auxiliary") aperture layout. This 4KB window is NOT a flat
 * view of the register file: per the Rage Pro programmer's guide the
 * lower 1KB is register block 1 and the second 1KB is block 0, and
 * the upper 2KB -- documented "reserved" -- is used by Mac OS anyway
 * and simply wraps onto the lower 2KB. DingusPPC's ATIRage models
 * exactly this ("marks the upper 2KB ... reserved, but it's used by
 * Mac OS anyway ... Make it wrap around the 2KB boundary instead").
 *
 * This is how the accelerated Mac OS NDRV streams host-blit pixel
 * data: it writes HOST_DATA (block 0, 0x200) at BAR2 offset 0xE00
 * (0xE00 & 0x7FF = 0x600, minus the 0x400 block-0 base = 0x200).
 * Decoding BAR2 linearly sent every one of those words to a
 * nonexistent register, so the menu bar, window contents and dialog
 * contents -- everything the driver draws with a colour host blit --
 * silently never arrived.
 */
static hwaddr ati_mach64_bar2_reg(hwaddr addr)
{
    addr &= 0x7ff;
    return (addr >= 0x400) ? (addr & 0x3ff) : ((addr & 0x3ff) + 0x400);
}

static uint64_t ati_mach64_bar2_read(void *opaque, hwaddr addr, unsigned size)
{
    return ati_mach64_mmio_read(opaque, ati_mach64_bar2_reg(addr), size);
}

static void ati_mach64_bar2_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    ati_mach64_mmio_write(opaque, ati_mach64_bar2_reg(addr), data, size);
}

static const MemoryRegionOps ati_mach64_bar2_ops = {
    .read = ati_mach64_bar2_read,
    .write = ati_mach64_bar2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps ati_mach64_mmio_ops = {
    .read = ati_mach64_mmio_read,
    .write = ati_mach64_mmio_write,
    /*
     * 2026-07-28: was DEVICE_BIG_ENDIAN, which byte-swapped every dword
     * access relative to real Mach64 silicon (a native little-endian PCI
     * device -- DingusPPC models the whole aperture LE; the chip's own
     * big-endian support is the separate BE framebuffer aperture, modeled
     * here as the vram-be-mirror alias). Direct evidence the BE
     * declaration was wrong: (1) an instrumented DingusPPC boot of the
     * same ROM shows its OF FCode writing CRTC_H_TOTAL_DISP=0x004F006B
     * (sane 640-wide timing) while our device received the byte-swapped
     * 0x6b004f00 for the same logical write; (2) our boot's ATI access
     * stream diverges from DingusPPC's from the very first register
     * touch, consistent with the FCode's first dword readbacks (chip
     * ID/config) arriving swapped and steering it down a different init
     * path that never reaches the mode-set.
     */
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void ati_mach64_reset(DeviceState *dev)
{
    ATIMach64State *s = ATI_MACH64(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->plls, 0, sizeof(s->plls));
    memset(s->palette, 0, sizeof(s->palette));
    s->dac_wr_index = 0;
    s->dac_rd_index = 0;
    s->dac_mask = 0xff;
    s->dac_comp_index = 0;
    memset(s->dac_comp_buf, 0, sizeof(s->dac_comp_buf));
    s->mode_dirty = true;
    s->hb_active = false;
    s->bm_data_mode = false;

    /*
     * GUI engine power-on defaults: scissors fully open (the scissor
     * fields are SIGNED -- right max is 0x1fff, bottom max 0x3fff per
     * LT3REGRE 5.2.3 / Table 5-11's "OPEN completely" values), drawing
     * direction left-to-right / top-to-bottom.
     */
    s->regs[ATI_SC_RIGHT >> 2] = 0x1fff;
    s->regs[ATI_SC_BOTTOM >> 2] = 0x3fff;
    s->regs[ATI_SC_LEFT_RIGHT >> 2] = 0x1fff0000;
    s->regs[ATI_SC_TOP_BOTTOM >> 2] = 0x3fff0000;
    s->regs[ATI_DST_CNTL >> 2] = ATI_DST_X_LEFT_TO_RIGHT |
                                 ATI_DST_Y_TOP_TO_BOTTOM;

    s->regs[ATI_CONFIG_CHIP_ID >> 2] =
        (ATI_RAGE_PRO_ASIC_ID << ATI_CFG_CHIP_MAJOR_SHIFT) |
        (PCI_DEVICE_ID_ATI_RAGE_PRO << ATI_CFG_CHIP_TYPE_SHIFT);

    /*
     * Real hardware's GP_IO sense pins default to input mode at power-on
     * (no direction bits driven), which means the register already
     * reflects the passively-sensed monitor code the instant the guest
     * reads it -- even before any guest write. Confirmed against
     * DingusPPC's real, working AtiRage constructor
     * (`regs[ATI_GP_IO] = ((mon_code & 6) << 11) | ((mon_code & 1) << 8)`,
     * using `read_monitor_sense(0, 0)` i.e. all-input, which resolves to
     * the plain std_sense_code). Our own reset previously left GP_IO at
     * a hard zero until the guest's first write -- if the ROM's FCode
     * does an early passive read of GP_IO to decide whether a monitor is
     * already sensed before committing to full CRTC bring-up, a zero
     * read (vs. DingusPPC's real nonzero default) would send it down a
     * different, minimal-init path -- consistent with this session's
     * traced divergence (DingusPPC does an immediate BUS_CNTL+CRTC_
     * GEN_CNTL bring-up; our own boot instead starts with a memory-
     * controller-only sequence and never fully programs the CRTC).
     * Reuse our own already-validated gp_io write-path encoding (called
     * with word=0, i.e. "no direction bits set") for consistency rather
     * than duplicating the bit-packing logic here.
     */
    s->regs[ATI_GP_IO >> 2] = ati_mach64_gp_io_write(s, 0);

    /*
     * Real hardware's CRTC also comes up at power-on with its display
     * output blanked (DISPLAY_DIS set) until firmware explicitly
     * programs a mode and re-enables it -- confirmed against
     * DingusPPC's real, working AtiRage constructor, which sets exactly
     * this bit at construction time (`set_bit(regs[ATI_CRTC_GEN_CNTL],
     * ATI_CRTC_DISPLAY_DIS)`). Our own reset previously left
     * CRTC_GEN_CNTL at a hard zero (DISPLAY_DIS clear, i.e. "already
     * displaying") until the guest's first write -- if the ROM's FCode
     * does an early passive read of CRTC_GEN_CNTL to check whether the
     * display is already active/blanked before deciding whether a full
     * cold-boot CRTC bring-up is needed, this mismatch could plausibly
     * steer it down a different path than a real, freshly-reset chip
     * would take.
     */
    s->regs[ATI_CRTC_GEN_CNTL >> 2] |= ATI_CRTC_DISPLAY_DIS;
}

static void ati_mach64_realize(PCIDevice *dev, Error **errp)
{
    ATIMach64State *s = ATI_MACH64(dev);
    Object *obj = OBJECT(dev);

    s->con = qemu_graphic_console_create(DEVICE(dev), 0, &ati_mach64_gfx_ops, s);

    memory_region_init_ram(&s->vram, obj, "ati-mach64-vram",
                           ATI_MACH64_VRAM_SIZE, &error_fatal);
    memory_region_init_io(&s->mmio, obj, &ati_mach64_mmio_ops, s,
                          "ati-mach64-mmio", ATI_MACH64_MMIO_SIZE);
    memory_region_init_io(&s->mmio_bar2, obj, &ati_mach64_bar2_ops, s,
                          "ati-mach64-mmio-bar2", ATI_MACH64_MMIO_SIZE);

    /*
     * Expose the same register file within the VRAM (BAR0) aperture,
     * matching real Mach64/Rage Pro hardware -- see the comment on
     * ATI_MACH64_REGS_IN_VRAM_OFFSET.
     */
    memory_region_init_alias(&s->mmio_in_vram, obj, "ati-mach64-mmio-in-vram",
                             &s->mmio, 0, 0x400);
    memory_region_add_subregion_overlap(&s->vram, ATI_MACH64_REGS_IN_VRAM_OFFSET,
                                        &s->mmio_in_vram, 1);

    /*
     * Register block 1 (registers 0x400-0x7FF: 3D setup, overlay and
     * the bus-master control set) sits in the 1KB directly BELOW the
     * block-0 window in the aperture -- confirmed against DingusPPC's
     * ATIRage (MM_REGS_1_OFF = 0x7FF800). This window is how the
     * accelerated Mac OS NDRV reaches BM_GUI_TABLE to kick bus-master
     * transfers: Open Firmware only assigns BAR0, so every register
     * access goes through the aperture windows. Without this mapping
     * those writes landed in raw VRAM and the driver's DMA-fed image
     * uploads (menu bar, desktop pattern tiles, menu save/restore)
     * silently never happened.
     */
    memory_region_init_alias(&s->mmio_in_vram_blk1, obj,
                             "ati-mach64-mmio-in-vram-blk1",
                             &s->mmio, 0x400, 0x400);
    memory_region_add_subregion_overlap(&s->vram,
                                        ATI_MACH64_REGS_IN_VRAM_OFFSET - 0x400,
                                        &s->mmio_in_vram_blk1, 1);

    /*
     * Real hardware mirrors the low ATI_MACH64_BE_FB_OFFSET bytes of
     * VRAM a second time starting at that same offset -- see the
     * comment on ATI_MACH64_BE_FB_OFFSET. A guest driver that picks a
     * CRTC framebuffer base at or beyond this offset must reach the
     * same physical bytes as the low aperture, not uninitialized VRAM.
     */
    memory_region_init_alias(&s->vram_be_mirror, obj, "ati-mach64-vram-be-mirror",
                             &s->vram, 0, ATI_MACH64_BE_FB_OFFSET);
    memory_region_add_subregion_overlap(&s->vram, ATI_MACH64_BE_FB_OFFSET,
                                        &s->vram_be_mirror, 0);

    pci_set_byte(&dev->config[PCI_REVISION_ID], ATI_RAGE_PRO_ASIC_ID);
    /*
     * Real hardware's device tree ("interrupts" = 0x00000001 under
     * ATY,mach64_3DUPro) shows this card uses PCI interrupt pin A.
     * Without this, PCI_INTERRUPT_PIN defaults to 0 ("uses no
     * interrupt"), so Open Firmware's generic PCI-node property
     * generator omits the "interrupts" property entirely, and the
     * card's own native driver (ndrv) fails its "Cannot Get
     * interrupts property" property lookup during init.
     */
    pci_config_set_interrupt_pin(dev->config, 1);

    /*
     * BAR1: the Mach64's 256-byte "Block I/O" register aperture in PCI
     * I/O space, exposing register offsets 0x00-0xFF (identity-mapped
     * onto the same register file as the memory aperture -- same
     * convention as DingusPPC's ATIRage aperture_size[1]=0x100/flag=IO
     * and its pci_io_read/write, which dispatch straight to
     * read_reg/write_reg). This BAR was previously missing entirely,
     * which broke the real ROM's Open Firmware ATY FCode driver: an
     * instrumented DingusPPC boot shows the FCode assigns THIS BAR
     * first (config @0x14 <- 0xc00), enables I/O space, and performs
     * its ENTIRE chip bring-up -- PLL setup, monitor sense, and the
     * initial 640x480 CRTC mode-set (every register it touches is
     * below 0x100) -- through this I/O window, before the memory BARs
     * are assigned at all. Without it, all of that traffic went to
     * unclaimed PCI I/O space and the boot display never initialized.
     */
    memory_region_init_io(&s->io, obj, &ati_mach64_mmio_ops, s,
                          "ati-mach64-io", 0x100);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio_bar2);

    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ati_mach64_vblank_timer_tick, s);
    s->vblank_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ati_mach64_vblank_end_tick, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ATI_MACH64_VBLANK_PERIOD_NS);

    /*
     * With virtual memory off in the guest, live testing confirmed ADB
     * click and keyboard delivery now work through the guest's own
     * native path (previously masked/confused by this same handler also
     * writing MBState). Cursor *position* tracking (CrsrVBLTask, the
     * VBL-driven cursor redraw task) still isn't serviced by the guest
     * on its own, so this handler stays registered for REL events only
     * -- see the button-handling removal note in
     * ati_mach64_host_cursor_event().
     */
    if (s->host_cursor_tracking) {
        s->cursor_hs = qemu_input_handler_register(DEVICE(dev),
                                                   &ati_mach64_cursor_handler);
    }

    /*
     * DDC/I2C EDID slave on the GP_IO sense pins (I2C address 0x50),
     * bit-banged by the guest -- lets AppleVision and the Monitors
     * control panel read a valid monitor descriptor. The EDID payload
     * is synthesized by QEMU's generic generator (as for every emulated
     * GPU); the real chip would read it from the attached monitor's ROM.
     */
    I2CBus *i2cbus = i2c_init_bus(DEVICE(dev), "ati-mach64.ddc");
    bitbang_i2c_init(&s->bbi2c, i2cbus);
    i2c_slave_set_address(I2C_SLAVE(&s->i2cddc), 0x50);
    qdev_realize(DEVICE(&s->i2cddc), BUS(i2cbus), &error_abort);
}

static void ati_mach64_exit(PCIDevice *dev)
{
    ATIMach64State *s = ATI_MACH64(dev);

    if (s->cursor_hs) {
        qemu_input_handler_unregister(s->cursor_hs);
    }
    timer_free(s->vblank_timer);
    qemu_graphic_console_close(s->con);
}

static const VMStateDescription vmstate_ati_mach64 = {
    .name = "ati-mach64",
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, ATIMach64State),
        VMSTATE_UINT32_ARRAY(regs, ATIMach64State, ATI_MACH64_NUM_REGS),
        VMSTATE_UINT8_ARRAY(plls, ATIMach64State, ATI_MACH64_NUM_PLLS),
        VMSTATE_UINT8_2DARRAY(palette, ATIMach64State, 256, 3),
        VMSTATE_UINT8(dac_wr_index, ATIMach64State),
        VMSTATE_UINT8(dac_rd_index, ATIMach64State),
        VMSTATE_UINT8(dac_mask, ATIMach64State),
        VMSTATE_UINT8(dac_comp_index, ATIMach64State),
        VMSTATE_UINT8_ARRAY(dac_comp_buf, ATIMach64State, 3),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * host-cursor-tracking gates the host-pointer-driven cursor workaround
 * (see ati_mach64_host_cursor_event()). Default on. Turn off with
 * -global ati-mach64-gt.host-cursor-tracking=off to test whether the
 * guest's own CrsrVBLTask-driven cursor path works -- the workaround
 * was justified on boots that all predated the TimeDBRA-calibration
 * governor and heathrow acknowledge fixes, so the underlying guest
 * stall may no longer occur.
 */
static const Property ati_mach64_properties[] = {
    DEFINE_PROP_BOOL("host-cursor-tracking", ATIMach64State,
                     host_cursor_tracking, true),
    /*
     * Real Old World Macs pick which video port is the boot/console
     * display by physical monitor-sense detection, not a stored
     * preference -- set to off (e.g. -global
     * ati-mach64-gt.monitor-connected=off) to simulate nothing being
     * plugged into the onboard port, the same way a real user would
     * by connecting their monitor to an add-in card's port instead.
     */
    DEFINE_PROP_BOOL("monitor-connected", ATIMach64State,
                     monitor_connected, true),
    DEFINE_EDID_PROPERTIES(ATIMach64State, i2cddc.edid_info),
};

static void ati_mach64_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    k->vendor_id = PCI_VENDOR_ID_ATI;
    k->device_id = PCI_DEVICE_ID_ATI_RAGE_PRO;
    /*
     * Real Apple-fitted Rage Pro boards report subsystem vendor/device
     * 0x1002/0x6987 (confirmed against DingusPPC's own ATIRage model).
     * NOTE: checked directly against a real hardware OF device-tree dump
     * (SourceFiles/G3/PowerMacG3-device-tree.txt) -- there is NO
     * subsystem-vendor-id/subsystem-id property on the real
     * ATY,mach64_3DUPro node at all, so this is NOT the mechanism real
     * OF uses to decide whether to bind its built-in
     * ".Display_ATImach64_3DR3" NDRV (confirmed empirically too: adding
     * this alone did not change CRTC register activity). Kept anyway
     * since it's still a real fidelity fix matching actual hardware.
     */
    k->subsystem_vendor_id = PCI_VENDOR_ID_ATI;
    k->subsystem_id = 0x6987;
    /*
     * 2026-07-28: PCI revision was previously left at the default 0.
     * Real 3D Rage Pro silicon reports its ASIC revision here -- 0x5C
     * ("R3B/D/P-A4", per DingusPPC's ATIRage model, which also mirrors
     * the same value into CONFIG_CHIP_ID's ASIC-rev field like we do).
     * The ROM's OF FCode selects among chip-revision-specific bring-up
     * paths; a revision of 0 does not correspond to any real shipped
     * chip.
     */
    k->revision = 0x5c;
    /*
     * Deliberately NO k->romfile / PCI Expansion ROM BAR. Two different
     * real third-party Mach64 add-in-card expansion ROMs (a GX-family
     * 0x4758 dump and a correctly-chip-matched GT-family 0x4754 dump)
     * were each tried and reverted -- both are genuinely for the wrong
     * chip (this device is 0x4750 "Rage Pro"), and more fundamentally,
     * this machine's real onboard chip's own FCode ("ATY,Fcode"="1.53"
     * per the real device-tree dump) was found embedded directly inside
     * the system ROM file itself at fixed offsets (~0x33fe80, matching
     * "name"="ATY,mach64_3DUPro"/"model"="ATY,GT-C" exactly), NOT behind
     * any PCI Expansion ROM BAR -- real Old World OF for onboard/
     * motherboard devices apparently looks this up via its own internal,
     * vendor/device-ID-keyed table baked into the ROM's native code,
     * independent of the generic PCI Expansion ROM BAR scan mechanism
     * add-in cards use. Since the real ROM's own OF code is what's
     * actually running here (this is a real ROM dump, not reimplemented
     * firmware), getting our PCI identity/config-space bits right should
     * be sufficient for that internal lookup to succeed on its own --
     * no QEMU-side romfile is the right way to model an onboard chip.
     */

    k->realize   = ati_mach64_realize;
    k->exit      = ati_mach64_exit;
    dc->vmsd     = &vmstate_ati_mach64;
    device_class_set_props(dc, ati_mach64_properties);
    device_class_set_legacy_reset(dc, ati_mach64_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static void ati_mach64_init(Object *obj)
{
    ATIMach64State *s = ATI_MACH64(obj);

    object_initialize_child(obj, "edid", &s->i2cddc, TYPE_I2CDDC);
}

static const TypeInfo ati_mach64_type_info = {
    .name           = TYPE_ATI_MACH64,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(ATIMach64State),
    .instance_init  = ati_mach64_init,
    .class_init     = ati_mach64_class_init,
    .interfaces     = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ati_mach64_register_types(void)
{
    type_register_static(&ati_mach64_type_info);
}

type_init(ati_mach64_register_types)
