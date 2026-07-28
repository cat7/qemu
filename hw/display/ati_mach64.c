/*
 * QEMU ATI Mach64 "3D Rage Pro" emulation
 *
 * See ati_mach64.h for background: this models the ATI Mach64 "3D Rage
 * Pro" chip (PCI vendor 0x1002, device 0x4750, revision 0x5c) -- the
 * identity directly confirmed by this machine's own real Open Firmware
 * device-tree dump. Milestone scope: correct PCI identity/BAR layout and
 * enough CRTC/DAC/config register modeling for native ROM boot-time
 * probing and a basic mode-set to work, rendered through a plain linear
 * framebuffer. The 2D BitBLT engine, hardware cursor, overlay/video-in
 * registers and true DDC/I2C are deliberately not modeled yet -- classic
 * Mac OS QuickDraw always has a software fallback per pixel depth, so
 * none of that is boot-blocking.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

#include "ati_mach64.h"
#include "ati_mach64_regs.h"
#include "trace.h"

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
        return PIXMAN_x8r8g8b8;
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
 * Composite the hardware cursor directly into the surface ("software
 * cursor" style). Only possible on the palettized path, which repaints
 * the whole surface every refresh anyway; it makes the visible cursor
 * pixel-exact with the guest's own idea of its position, sidestepping
 * every display-backend cursor-layer geometry quirk.
 */
static void ati_mach64_composite_cursor(ATIMach64State *s, DisplaySurface *ds,
                                        const ATIMach64Mode *mode)
{
    uint32_t posn = s->regs[ATI_CUR_HORZ_VERT_POSN >> 2];
    uint32_t offs = s->regs[ATI_CUR_HORZ_VERT_OFF >> 2];
    uint32_t raw0 = s->regs[ATI_CUR_CLR0 >> 2];
    uint32_t raw1 = s->regs[ATI_CUR_CLR1 >> 2];
    uint32_t clr0 = 0xff000000u | (((raw0 >> 8) & 0xff) << 16) |
                    (((raw0 >> 16) & 0xff) << 8) | ((raw0 >> 24) & 0xff);
    uint32_t clr1 = 0xff000000u | (((raw1 >> 8) & 0xff) << 16) |
                    (((raw1 >> 16) & 0xff) << 8) | ((raw1 >> 24) & 0xff);
    uint32_t vram_off = (s->regs[ATI_CUR_OFFSET >> 2] & 0xfffff) * 8;
    int x0 = (int)(posn & ATI_CUR_POSN_MASK) -
             (int)(offs & ATI_CUR_OFF_MASK);
    int y0 = (int)((posn >> 16) & ATI_CUR_POSN_MASK) -
             (int)((offs >> 16) & ATI_CUR_OFF_MASK);
    const uint8_t *src;
    uint32_t *dst;
    int row, px, x, y;

    if (!(s->regs[ATI_GEN_TEST_CNTL >> 2] & ATI_GEN_CUR_ENABLE)) {
        return;
    }
    if (vram_off + 64 * 64 / 4 > ATI_MACH64_VRAM_SIZE) {
        return;
    }
    src = (const uint8_t *)memory_region_get_ram_ptr(&s->vram) + vram_off;

    for (row = 0; row < 64; row++) {
        y = y0 + row;
        if (y < 0 || y >= mode->height) {
            continue;
        }
        dst = (uint32_t *)((uint8_t *)surface_data(ds) +
                           y * surface_stride(ds));
        for (px = 0; px < 64; px++) {
            x = x0 + px;
            if (x < 0 || x >= mode->width) {
                continue;
            }
            switch ((src[row * 16 + px / 4] >> ((px % 4) * 2)) & 3) {
            case 0:
                dst[x] = clr0;
                break;
            case 1:
                dst[x] = clr1;
                break;
            case 3:
                dst[x] ^= 0x00ffffffu; /* complement of display pixel */
                break;
            default:
                break; /* transparent */
            }
        }
    }
}

static bool ati_mach64_update_display(void *opaque)
{
    ATIMach64State *s = opaque;
    ATIMach64Mode mode;
    DisplaySurface *ds;
    uint8_t *ptr;
    bool palettized;

    ati_mach64_get_mode(s, &mode);
    if (!ati_mach64_mode_valid(s, &mode)) {
        return true;
    }
    palettized = (mode.bpp == 1);

    if (memcmp(&s->mode, &mode, sizeof(mode)) != 0 || s->mode_dirty) {
        s->mode = mode;
        s->mode_dirty = false;
        if (palettized) {
            ds = qemu_create_displaysurface(mode.width, mode.height);
        } else {
            ptr = memory_region_get_ram_ptr(&s->vram);
            ds = qemu_create_displaysurface_from(mode.width, mode.height,
                                                 ati_mach64_pixman_format(
                                                     (s->regs[ATI_CRTC_GEN_CNTL >> 2]
                                                      >> ATI_CRTC_PIX_WIDTH_SHIFT) &
                                                     ATI_CRTC_PIX_WIDTH_MASK),
                                                 mode.pitch,
                                                 ptr + mode.fb_offset);
        }
        qemu_console_set_surface(s->con, ds);
    }
    if (palettized) {
        ds = qemu_console_surface(s->con);
        ati_mach64_draw_8bpp(s, ds, &mode);
        ati_mach64_composite_cursor(s, ds, &mode);
    }
    s->cursor_composited = palettized;
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
    bool on = (s->regs[ATI_GEN_TEST_CNTL >> 2] & ATI_GEN_CUR_ENABLE) != 0;
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
    /*
     * When the cursor is being composited straight into the surface
     * (palettized modes), suppress the backend overlay cursor so it
     * doesn't appear twice; the composited image is the pixel-exact
     * one.
     */
    qemu_console_set_mouse(s->con, x, y, on && !s->cursor_composited);
}

#define ATI_MACH64_VBLANK_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)
/* Blank interval = last 1/8 of the frame, matching the phase-computed
 * live VBLANK status bit in the INT_CNTL read path. */
#define ATI_MACH64_VBLANK_LEN_NS    (ATI_MACH64_VBLANK_PERIOD_NS / 8)
/*
 * How long the PCI IRQ line itself stays physically asserted each
 * frame -- deliberately much shorter than ATI_MACH64_VBLANK_LEN_NS
 * above (which models the CRT's real, visually-accurate ~12.5% duty
 * cycle for the read-only status bit, a completely separate concern).
 * Heathrow's mask marks this GPU line level_triggered
 * (level_triggered = 0x1ff00000, see heathrow_pic.c), so
 * heathrow_check_irq() ORs the raw physical level into the trigger
 * condition regardless of any "clear events" ack the guest issues --
 * acking does not lower the CPU-visible interrupt while the physical
 * line is still held high. Holding it for the full 1/8-frame (~2.08ms)
 * duration let the guest's level-triggered exception handler be
 * re-taken continuously for the whole window (confirmed empirically:
 * a live MMIO trace at the Finder desktop showed a tight repeating
 * ack/read spin on the heathrow PIC registers, thousands of times per
 * frame, GPR-visible CPU PC alternating between a fixed ROM interrupt
 * stub and a fixed 68K-interpreter return address -- i.e. a genuine
 * interrupt-storm livelock, not a dispatcher decision, explaining why
 * Mac OS's ATI ISR only ever completed twice). Real Mach64 silicon
 * would have its request line dropped by software acking the device's
 * own INT_CNTL status bits almost immediately (microseconds, not
 * milliseconds); this short, fixed pulse is a stand-in for that until
 * ack-driven deassertion is modeled, chosen long enough to guarantee
 * the CPU sees at least one exception even under coarse icount
 * granularity, but far too short to storm.
 */
#define ATI_MACH64_VBLANK_IRQ_LEN_NS 2000

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
    uint8_t gpio_levels = (rword >> 8) & 0xff;
    uint8_t gpio_dirs = (rword >> 24) & 0xff;
    uint8_t result;

    gpio_levels = ((gpio_levels & 0x30) >> 3) | (gpio_levels & 1);
    gpio_levels ^= 7;
    gpio_dirs = ((gpio_dirs & 0x30) >> 3) | (gpio_dirs & 1);
    gpio_levels &= ~gpio_dirs;

    switch ((gpio_dirs << 3) | gpio_levels) {
    case 0043: /* sense line 2 pulled low; read sense line 1 and 0 */
        result = (ATI_APPLESENSE_EXT_CODE & 0060) >> 4;
        break;
    case 0025: /* sense line 1 pulled low; read sense line 2 and 0 */
        result = ((ATI_APPLESENSE_EXT_CODE & 0010) >> 1) |
                 ((ATI_APPLESENSE_EXT_CODE & 0004) >> 2);
        break;
    case 0016: /* sense line 0 pulled low; read sense line 2 and 1 */
        result = (ATI_APPLESENSE_EXT_CODE & 0003) << 1;
        break;
    default:
        result = ATI_APPLESENSE_STD_CODE;
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
         * a plausible real-CRT vblank duty cycle.
         */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t phase = now % ATI_MACH64_VBLANK_PERIOD_NS;
        bool in_vblank = phase >= (ATI_MACH64_VBLANK_PERIOD_NS * 7 / 8);

        result = (result & ~ATI_CRTC_VBLANK) | (in_vblank ? ATI_CRTC_VBLANK : 0);
        break;
    }
    default:
        break;
    }

    if (size == 4 && byte_off == 0) {
        trace_ati_mach64_mmio_read(size, addr, result);
        return result;
    }
    /* Reverted along with the write path -- see the shift comment
     * there for why byte_off*8 (not top-down) is the real convention. */
    result = (result >> (byte_off * 8)) & ((1ull << (size * 8)) - 1);
    trace_ati_mach64_mmio_read(size, addr, result);
    return result;
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

    trace_ati_mach64_mmio_write(size, addr, data);
    word = (word & ~mask) | ((uint32_t)data << shift);

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
         * pulse driven purely by the vblank timers (see above). */
        uint32_t pending = s->regs[reg_num] &
                           (ATI_CRTC_VBLANK_INT | ATI_CRTC_VLINE_INT);
        if (word & ATI_CRTC_VBLANK_INT_AK) {
            pending &= ~ATI_CRTC_VBLANK_INT;
        }
        if (word & ATI_CRTC_VLINE_INT_AK) {
            pending &= ~ATI_CRTC_VLINE_INT;
        }
        s->regs[reg_num] = (word & ~(ATI_CRTC_VBLANK_INT |
                                     ATI_CRTC_VLINE_INT)) | pending;
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
    default:
        s->regs[reg_num] = word;
        return;
    }
}

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

    /*
     * Expose the same register file within the VRAM (BAR0) aperture,
     * matching real Mach64/Rage Pro hardware -- see the comment on
     * ATI_MACH64_REGS_IN_VRAM_OFFSET.
     */
    memory_region_init_alias(&s->mmio_in_vram, obj, "ati-mach64-mmio-in-vram",
                             &s->mmio, 0, ATI_MACH64_MMIO_SIZE);
    memory_region_add_subregion_overlap(&s->vram, ATI_MACH64_REGS_IN_VRAM_OFFSET,
                                        &s->mmio_in_vram, 1);

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
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   ati_mach64_vblank_timer_tick, s);
    s->vblank_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ati_mach64_vblank_end_tick, s);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ATI_MACH64_VBLANK_PERIOD_NS);
}

static void ati_mach64_exit(PCIDevice *dev)
{
    ATIMach64State *s = ATI_MACH64(dev);

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
    device_class_set_legacy_reset(dc, ati_mach64_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ati_mach64_type_info = {
    .name           = TYPE_ATI_MACH64,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(ATIMach64State),
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
