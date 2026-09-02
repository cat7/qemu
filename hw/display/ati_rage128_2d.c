/*
 * QEMU ATI Rage 128 Pro emulation -- 2D (destination datapath) engine.
 *
 * Split out of ati_rage128.c following the layout of the upstream
 * ati-vga device (ati_2d.c).
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "system/memory.h"
#include "ui/console.h"

#include "ati_rage128_int.h"
#include "ati_rage128_regs.h"
#include "trace.h"

/*
 * 2D GUI (destination datapath) engine. Ported from the real, shipped
 * upstream `ati-vga` device (hw/display/ati.c/ati_2d.c) rather than
 * written from scratch or from the abandoned SourceFiles/ATI/qemu
 * clone -- see the comment on the register block in
 * ati_rage128_regs.h for why. Adapted for this device's standalone
 * VRAM MemoryRegion (no VGACommonState/vbe here) and for a bigger ROP3
 * repertoire: upstream only implements SRCCOPY/PATCOPY/BLACKNESS/
 * WHITENESS and no-ops everything else; this adds a general bit-level
 * ROP3 fallback (all 16 codes) so a ROP this driver actually uses
 * doesn't silently vanish.
 */
static int ati_rage128_bpp_from_datatype(uint32_t datatype)
{
    switch (datatype & 0xf) {
    case 2:
        return 8;
    case 3:
    case 4:
        return 16;
    case 5:
        return 24;
    case 6:
        return 32;
    default:
        return 0;
    }
}

static int ati_rage128_bpp_from_dp_datatype(ATIRage128State *s)
{
    return ati_rage128_bpp_from_datatype(s->dp_datatype);
}

static uint32_t ati_rage128_2d_read_pixel(ATIRage128State *s, uint32_t offset,
                                          uint32_t stride, int x, int y,
                                          int bpp)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t addr = offset + (uint32_t)y * stride + (uint32_t)x * (bpp / 8);

    if (x < 0 || y < 0 || addr + bpp / 8 > ATI_RAGE128_VRAM_SIZE) {
        return 0;
    }
    switch (bpp) {
    case 8:
        return vram[addr];
    case 16:
        return lduw_le_p(vram + addr);
    case 24:
        return ((uint32_t)vram[addr + 2] << 16) |
               ((uint32_t)vram[addr + 1] << 8) | vram[addr];
    case 32:
        return ldl_le_p(vram + addr);
    default:
        return 0;
    }
}

static void ati_rage128_2d_write_pixel(ATIRage128State *s, uint32_t offset,
                                       uint32_t stride, int x, int y, int bpp,
                                       uint32_t color)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t addr = offset + (uint32_t)y * stride + (uint32_t)x * (bpp / 8);

    if (x < 0 || y < 0 || addr + bpp / 8 > ATI_RAGE128_VRAM_SIZE) {
        return;
    }
    switch (bpp) {
    case 8:
        vram[addr] = color;
        break;
    case 16:
        stw_le_p(vram + addr, color);
        break;
    case 24:
        vram[addr] = color & 0xff;
        vram[addr + 1] = (color >> 8) & 0xff;
        vram[addr + 2] = (color >> 16) & 0xff;
        break;
    case 32:
        stl_le_p(vram + addr, color);
        break;
    default:
        break;
    }
    /*
     * Keep the dirty-bitmap framebuffer scanner seeing engine-drawn
     * pixels, exactly as the CPU aperture write path does. Without
     * this, blitted content (menu bar, window interiors, host-data
     * icons) sits correct in VRAM but the display surface never
     * refreshes it until an unrelated CPU store happens to dirty the
     * same scan block -- observed live as white Finder windows whose
     * icons only appear when clicked, and a Mac OS 9 menu bar that is
     * never painted.
     */
    memory_region_set_dirty(&s->vram, addr & ~7ull, 8);
}

static uint32_t ati_rage128_apply_rop3(uint8_t rop, uint32_t src, uint32_t dst,
                                       uint32_t pat)
{
    uint32_t result = 0;
    int bit;

    /* Fast paths for the common cases */
    switch (rop) {
    case 0x00:
        return 0;
    case 0xff:
        return 0xffffffffu;
    case 0xcc: /* SRCCOPY */
        return src;
    case 0xf0: /* PATCOPY */
        return pat;
    case 0x55: /* DSTINVERT */
        return ~dst;
    case 0x66: /* SRCINVERT (XOR) */
        return src ^ dst;
    case 0x88: /* SRCAND */
        return src & dst;
    case 0xee: /* SRCPAINT (OR) */
        return src | dst;
    case 0x33: /* NOTSRCCOPY */
        return ~src;
    case 0x5a: /* PATINVERT */
        return pat ^ dst;
    case 0xc0: /* MERGECOPY */
        return pat & src;
    default:
        break;
    }

    /* General bit-level ROP3: each of the 8 bits of `rop` selects the
     * output for one of the 8 (S,D,P) input combinations. */
    for (bit = 0; bit < 32; bit++) {
        uint32_t mask = 1u << bit;
        int sb = (src & mask) ? 1 : 0;
        int db = (dst & mask) ? 1 : 0;
        int pb = (pat & mask) ? 1 : 0;
        int idx = (sb << 2) | (db << 1) | pb;

        if (rop & (1 << idx)) {
            result |= mask;
        }
    }
    return result;
}


/*
 * Pattern ("brush") lookup for one destination pixel.
 *
 * Returns false when the pixel must be left untouched -- that is what
 * the transparent "_LA" (leave alone) brush types mean, and it is what
 * turns a rectangle stamped with a 50% dither into a dotted outline
 * rather than a solid block.
 *
 * The mono pattern lives in BRUSH_DATA0.. as a bitmap, one row per
 * byte for the 8-wide forms and one row per dword for the 32-wide ones,
 * MSB first within a byte (the same order the mono host-data expander
 * uses). BRUSH_Y_X gives the pattern origin.
 */
static bool ati_rage128_2d_brush(ATIRage128State *s, int x, int y, int bpp,
                                 uint32_t *pat)
{
    unsigned type = (s->dp_datatype & R128_DP_BRUSH_DATATYPE) >>
                    R128_DP_BRUSH_DATATYPE_SHIFT;
    uint32_t yx = s->regs[R128_BRUSH_Y_X >> 2];
    int bx = x - (int)(yx & 0xffff);
    int by = y - (int)((yx >> 16) & 0xffff);
    const uint32_t *data = &s->regs[R128_BRUSH_DATA0 >> 2];
    bool transparent = false;
    int pw, ph, bit;

    switch (type) {
    case R128_BRUSH_SOLID_COLOR:
    case R128_BRUSH_NONE:
    default:
        *pat = s->dp_brush_frgd_clr;
        return true;

    case R128_BRUSH_8X8_COLOR:
        *pat = data[((by & 7) * 8 + (bx & 7)) & 63];
        return true;
    case R128_BRUSH_1X8_COLOR:
        *pat = data[by & 7];
        return true;

    case R128_BRUSH_8X8_MONO_FG_LA:
        transparent = true;
        /* fall through */
    case R128_BRUSH_8X8_MONO_FG_BG:
        pw = 8; ph = 8;
        break;
    case R128_BRUSH_1X8_MONO_FG_LA:
        transparent = true;
        /* fall through */
    case R128_BRUSH_1X8_MONO_FG_BG:
        pw = 1; ph = 8;
        break;
    case R128_BRUSH_32X1_MONO_FG_LA:
        transparent = true;
        /* fall through */
    case R128_BRUSH_32X1_MONO_FG_BG:
        pw = 32; ph = 1;
        break;
    case R128_BRUSH_32X32_MONO_FG_LA:
        transparent = true;
        /* fall through */
    case R128_BRUSH_32X32_MONO_FG_BG:
        pw = 32; ph = 32;
        break;
    }

    bx &= pw - 1;
    by &= ph - 1;
    if (pw == 32) {
        bit = (data[by] >> (31 - bx)) & 1;
    } else {
        /* eight rows of eight bits: four rows to a dword, low byte first */
        bit = (data[by >> 2] >> ((by & 3) * 8 + (7 - bx))) & 1;
    }

    if (bit) {
        *pat = s->dp_brush_frgd_clr;
        return true;
    }
    if (transparent) {
        return false;
    }
    *pat = s->dp_brush_bkgd_clr;
    return true;
}

/*
 * Colour compare (CLR_CMP_CNTL functions 0/1/4/5/7, RRG 3-178): does a
 * pixel that compares as `eq` against the reference get drawn?
 */
static inline bool ati_rage128_clr_cmp_draw(unsigned fn, bool eq)
{
    switch (fn) {
    case 1:
        return false;               /* CMP_TRUE: never draw */
    case 4:
    case 7:
        return eq;                  /* draw when equal (7: flip on eq) */
    case 5:
        return !eq;                 /* draw when not equal */
    default:
        return true;                /* CMP_FALSE: always draw */
    }
}

static void ati_rage128_2d_do_blt(ATIRage128State *s)
{
    int bpp = ati_rage128_bpp_from_dp_datatype(s);
    uint8_t rop = (s->dp_mix >> 16) & 0xff;
    uint32_t pixmask;
    uint32_t cmp_cntl = s->regs[R128_CLR_CMP_CNTL >> 2];
    unsigned cmp_fn_src = cmp_cntl & 7;
    unsigned cmp_fn_dst = (cmp_cntl >> 8) & 7;
    unsigned cmp_sel = (cmp_cntl >> 24) & 3;
    uint32_t cmp_mask, cmp_src, cmp_dst, wmask;
    bool cmp_on_dst, cmp_on_src;
    bool left_to_right = s->dp_cntl & R128_DST_X_LEFT_TO_RIGHT;
    bool top_to_bottom = s->dp_cntl & R128_DST_Y_TOP_TO_BOTTOM;
    bool overlaps;
    int width = s->dst_width;
    int height = s->dst_height;
    uint32_t dst_stride, src_stride;
    int sc_left, sc_top, sc_right, sc_bottom;
    int x, y;

    if (!bpp || width == 0 || height == 0) {
        return;
    }

    /*
     * Rage 128 destination/source pitch registers count in units of 8
     * PIXELS (SRC/DST_PITCH_OFFSET pack pitch/8; a live Mac OS X 10.3
     * shadow->screen blit carried pitch 0x64/0x68 for its 800px/832px
     * surfaces). Upstream ati_2d.c encodes the same rule as
     * "dst_stride *= bpp" for the Rage 128 Pro. Treating them as plain
     * pixels compressed every blit 8x vertically into a self-overlapping
     * smear -- the striped-band garbled desktop.
     */
    dst_stride = s->dst_pitch * bpp;
    src_stride = s->src_pitch * bpp;
    if (!dst_stride) {
        return;
    }

    /*
     * Colour compare and write mask. CLR_CMP_SRC selects which side is
     * keyed (0 = destination, 1 = source, 2 = both; 3 "HILITE" treated
     * as destination), CLR_CMP_MSK picks the compared bits, DP_WRITE_MSK
     * the written ones. Mac OS's QuickDraw hilite is a four-fill dance
     * on exactly these: clear the aRGB1555 alpha bits, white -> hilite
     * colour + alpha flag, unflagged hilite (the previous selection) ->
     * white, flagged -> hilite. Ignoring them made every pass a plain
     * solid fill: the highlighted list row lost its text and the old
     * highlight was never removed.
     */
    pixmask = bpp >= 32 ? 0xffffffffu : (1u << bpp) - 1;   /* bpp in bits */
    cmp_mask = s->regs[R128_CLR_CMP_MASK >> 2] & pixmask;
    cmp_src = s->regs[R128_CLR_CMP_CLR_SRC >> 2] & cmp_mask;
    cmp_dst = s->regs[R128_CLR_CMP_CLR_DST >> 2] & cmp_mask;
    cmp_on_dst = (cmp_sel != 1) && cmp_fn_dst != 0;
    cmp_on_src = (cmp_sel == 1 || cmp_sel == 2) && cmp_fn_src != 0 &&
                 rop != 0xf0;
    wmask = s->dp_write_mask & pixmask;
    trace_ati_rage128_2d_cmp(cmp_cntl, cmp_mask, cmp_src, cmp_dst, wmask,
                             s->dp_brush_frgd_clr);

    sc_left = s->sc_left;
    sc_top = s->sc_top;
    sc_right = s->sc_right;
    sc_bottom = s->sc_bottom;
    if (sc_right == 0 && sc_bottom == 0) {
        sc_right = 0x3fff;
        sc_bottom = 0x3fff;
    }

    /*
     * Pick a non-destructive direction when a copy overlaps itself.
     *
     * Mac OS never varies the direction bits -- DP_CNTL reads 0x0107,
     * left-to-right and top-to-bottom, for every blit, including the six
     * overlapping down-and-right copies a single diagonal window drag
     * produces. Real hardware clearly does not corrupt those, so the
     * engine must order the copy itself rather than trusting the driver.
     * Walking forward regardless re-read rows that had already been
     * overwritten, smearing repeated fragments across a window whenever
     * it was dragged anything other than exactly horizontally.
     */
    overlaps = s->src_offset == s->dst_offset &&
               (int)s->dst_x < (int)s->src_x + width &&
               (int)s->src_x < (int)s->dst_x + width &&
               (int)s->dst_y < (int)s->src_y + height &&
               (int)s->src_y < (int)s->dst_y + height;
    if (overlaps) {
        left_to_right = s->dst_x <= s->src_x;
        top_to_bottom = s->dst_y <= s->src_y;
    }

    for (y = 0; y < height; y++) {
        int dy = top_to_bottom ? (int)s->dst_y + y
                               : (int)s->dst_y + height - 1 - y;
        int sy = top_to_bottom ? (int)s->src_y + y
                               : (int)s->src_y + height - 1 - y;

        if (dy < sc_top || dy > sc_bottom) {
            continue;
        }
        for (x = 0; x < width; x++) {
            int dx = left_to_right ? (int)s->dst_x + x
                                   : (int)s->dst_x + width - 1 - x;
            int sx = left_to_right ? (int)s->src_x + x
                                   : (int)s->src_x + width - 1 - x;
            uint32_t src_pixel = 0;
            uint32_t dst_pixel;
            uint32_t pat_pixel;
            uint32_t result;

            if (dx < sc_left || dx > sc_right) {
                continue;
            }
            if (!ati_rage128_2d_brush(s, dx, dy, bpp, &pat_pixel)) {
                continue;
            }
            if (rop != 0xf0) {
                src_pixel = ati_rage128_2d_read_pixel(s, s->src_offset,
                                                      src_stride, sx, sy,
                                                      bpp);
            }
            dst_pixel = ati_rage128_2d_read_pixel(s, s->dst_offset,
                                                  dst_stride, dx, dy, bpp);
            if (cmp_on_dst &&
                !ati_rage128_clr_cmp_draw(cmp_fn_dst,
                                          (dst_pixel & cmp_mask) == cmp_dst)) {
                continue;
            }
            if (cmp_on_src &&
                !ati_rage128_clr_cmp_draw(cmp_fn_src,
                                          (src_pixel & cmp_mask) == cmp_src)) {
                continue;
            }
            result = ati_rage128_apply_rop3(rop, src_pixel, dst_pixel,
                                            pat_pixel);
            if (wmask != pixmask) {
                result = (result & wmask) | (dst_pixel & ~wmask);
            }
            ati_rage128_2d_write_pixel(s, s->dst_offset, dst_stride, dx, dy,
                                       bpp, result);
        }
    }
}


static uint32_t ati_rage128_scale_texel(const uint8_t *row, int sx,
                                        unsigned dt)
{
    int y, u, v, r, g, b;

    switch (dt) {
    case R128_SCALE_DT_YUYV422:
    case R128_SCALE_DT_UYVY422:
    {
        const uint8_t *p = row + (sx & ~1) * 2;

        if (dt == R128_SCALE_DT_YUYV422) {      /* Y0 U Y1 V */
            y = p[(sx & 1) ? 2 : 0];
            u = p[1];
            v = p[3];
        } else {                                /* U Y0 V Y1 ('2vuy') */
            y = p[(sx & 1) ? 3 : 1];
            u = p[0];
            v = p[2];
        }
        break;
    }
    case R128_SCALE_DT_AYUV444:
        y = row[sx * 4 + 1];
        u = row[sx * 4 + 2];
        v = row[sx * 4 + 3];
        break;
    case R128_SCALE_DT_Y8:
        y = row[sx];
        u = v = 128;
        break;
    case R128_SCALE_DT_ARGB8888:
        /*
         * Chip-native little-endian, alpha in the top byte, kept for the
         * caller (the blended path needs it; the plain copy ignores it).
         * Verified against the ARGB pointer sprite OS X's driver stages
         * for its cursor-in-VRAM path (bytes ff ff ff 55 = white at
         * alpha 0x55, 00 00 00 ff = opaque black -- the I-beam).
         */
        return ldl_le_p(row + sx * 4);
    case R128_SCALE_DT_RGB565:
    {
        uint16_t px = lduw_be_p(row + sx * 2);

        return (((px >> 11) & 0x1f) << 19) | (((px >> 5) & 0x3f) << 10) |
               ((px & 0x1f) << 3);
    }
    case R128_SCALE_DT_ARGB1555:
    {
        uint16_t px = lduw_be_p(row + sx * 2);

        return (((px >> 10) & 0x1f) << 19) | (((px >> 5) & 0x1f) << 11) |
               ((px & 0x1f) << 3);
    }
    default:
        return 0;
    }

    y = (y - 16) * 298;
    u -= 128;
    v -= 128;
    r = (y + 409 * v + 128) >> 8;
    g = (y - 100 * u - 208 * v + 128) >> 8;
    b = (y + 516 * u + 128) >> 8;
    r = MIN(MAX(r, 0), 255);
    g = MIN(MAX(g, 0), 255);
    b = MIN(MAX(b, 0), 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/*
 * One alpha-blend factor of MISC_3D_STATE_CNTL_REG / SCALE_3D_CNTL,
 * evaluated per channel (0..255). Rage 128 blends
 * dst = src * src_factor + dst * dst_factor.
 */
static int ati_rage128_blend_factor(unsigned f, int sc, int sa, int dc,
                                    int da)
{
    switch (f) {
    case R128_ALPHA_BLEND_ZERO:        return 0;
    case R128_ALPHA_BLEND_ONE:         return 255;
    case R128_ALPHA_BLEND_SRCCOLOR:    return sc;
    case R128_ALPHA_BLEND_INVSRCCOLOR: return 255 - sc;
    case R128_ALPHA_BLEND_SRCALPHA:    return sa;
    case R128_ALPHA_BLEND_INVSRCALPHA: return 255 - sa;
    case R128_ALPHA_BLEND_DSTALPHA:    return da;
    case R128_ALPHA_BLEND_INVDSTALPHA: return 255 - da;
    case R128_ALPHA_BLEND_DSTCOLOR:    return dc;
    case R128_ALPHA_BLEND_INVDSTCOLOR: return 255 - dc;
    case R128_ALPHA_BLEND_SAT:         return MIN(sa, 255 - da);
    default:                           return 255;
    }
}

/* Read a destination pixel as 8-bit ARGB regardless of surface depth. */
static uint32_t ati_rage128_dst_to_argb(uint32_t px, int bpp)
{
    switch (bpp) {
    case 16:
        return 0xff000000 | (((px >> 11) & 0x1f) << 19) |
               (((px >> 5) & 0x3f) << 10) | ((px & 0x1f) << 3);
    case 24:
        return 0xff000000 | (px & 0xffffff);
    case 32:
        return px;
    default:
        return 0xff000000 | px;
    }
}

static uint32_t ati_rage128_argb_to_dst(uint32_t argb, int bpp)
{
    switch (bpp) {
    case 16:
        return (((argb >> 19) & 0x1f) << 11) | (((argb >> 10) & 0x3f) << 5) |
               ((argb >> 3) & 0x1f);
    default:
        return argb;
    }
}

typedef struct ATIRage128ScaleOp {
    int dst_x, dst_y, w, h;
    uint32_t src_off, src_pitch;      /* pitch in pixels */
    uint32_t x_inc, y_inc;            /* 4.12 fixed point */
    unsigned dt;
    uint32_t dst_off, dst_stride;     /* stride in bytes */
    int bpp;
    int sc_left, sc_top, sc_right, sc_bottom;
    unsigned src_factor, dst_factor;  /* R128_ALPHA_BLEND_* */
} ATIRage128ScaleOp;

/*
 * The scaler proper: a scaled, optionally YUV-converting, optionally
 * alpha-blended copy from a source image in VRAM into the destination
 * rectangle. Nearest-neighbour: the increments are DDA accumulator
 * steps with 12 fractional bits.
 */
static void ati_rage128_2d_scale_run(ATIRage128State *s,
                                     const ATIRage128ScaleOp *op)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    bool blend = !(op->src_factor == R128_ALPHA_BLEND_ONE &&
                   op->dst_factor == R128_ALPHA_BLEND_ZERO);
    int src_bpp, x, y;

    trace_ati_rage128_scale(op->dst_x, op->dst_y, op->w, op->h, op->src_off,
                            op->src_pitch, op->x_inc, op->y_inc, op->dt);
    if (!op->bpp || !op->dst_stride || !op->x_inc || !op->y_inc ||
        !op->src_pitch || op->w <= 0 || op->h <= 0) {
        return;
    }

    switch (op->dt) {
    case R128_SCALE_DT_Y8:
        src_bpp = 1;
        break;
    case R128_SCALE_DT_ARGB1555:
    case R128_SCALE_DT_RGB565:
    case R128_SCALE_DT_YUYV422:
    case R128_SCALE_DT_UYVY422:
        src_bpp = 2;
        break;
    case R128_SCALE_DT_ARGB8888:
    case R128_SCALE_DT_AYUV444:
        src_bpp = 4;
        break;
    default:
        trace_ati_rage128_scale_unimp(op->dt);
        return;
    }
    if (blend) {
        trace_ati_rage128_scale_blend(op->src_factor, op->dst_factor);
    }

    for (y = 0; y < op->h; y++) {
        int dy = op->dst_y + y;
        uint32_t sy = ((uint32_t)y * op->y_inc) >> 12;
        const uint8_t *row;

        if (dy < op->sc_top || dy > op->sc_bottom) {
            continue;
        }
        row = vram + op->src_off + sy * op->src_pitch * (uint32_t)src_bpp;
        if (op->src_off + (sy + 1) * op->src_pitch * (uint32_t)src_bpp >
            ATI_RAGE128_VRAM_SIZE) {
            break;
        }
        for (x = 0; x < op->w; x++) {
            int dx = op->dst_x + x;
            uint32_t sx = ((uint32_t)x * op->x_inc) >> 12;
            uint32_t src, out;

            if (dx < op->sc_left || dx > op->sc_right) {
                continue;
            }
            if ((sx + 1) * (uint32_t)src_bpp >
                op->src_pitch * (uint32_t)src_bpp) {
                break;
            }
            src = ati_rage128_scale_texel(row, sx, op->dt);
            if (op->dt != R128_SCALE_DT_ARGB8888) {
                src |= 0xff000000;      /* no alpha in the source */
            }
            if (!blend) {
                out = ati_rage128_argb_to_dst(src, op->bpp);
            } else {
                uint32_t dst = ati_rage128_dst_to_argb(
                    ati_rage128_2d_read_pixel(s, op->dst_off, op->dst_stride,
                                              dx, dy, op->bpp), op->bpp);
                int sa = src >> 24, da = dst >> 24;
                int c, shift;

                out = 0;
                for (c = 0, shift = 0; c < 4; c++, shift += 8) {
                    int sc = (src >> shift) & 0xff, dc = (dst >> shift) & 0xff;
                    int v = (sc * ati_rage128_blend_factor(op->src_factor, sc,
                                                           sa, dc, da) +
                             dc * ati_rage128_blend_factor(op->dst_factor, sc,
                                                           sa, dc, da) +
                             127) / 255;

                    out |= (uint32_t)MIN(v, 255) << shift;
                }
                out = ati_rage128_argb_to_dst(out, op->bpp);
            }
            ati_rage128_2d_write_pixel(s, op->dst_off, op->dst_stride, dx, dy,
                                       op->bpp, out);
        }
    }
}

/*
 * CNTL_SCALING packet: this is how Mac OS plays video on this card --
 * the counterpart of the mach64's scaler pipe, and it carries the same
 * parameters for the same movie. Reading the increments unshifted gives
 * a sixteen-fold downscale, which cannot be right when the source and
 * destination are the same size: they sit at bits 19:4 exactly as the
 * mach64's do.
 */
void ati_rage128_2d_scale(ATIRage128State *s, const uint32_t *pkt)
{
    ATIRage128ScaleOp op = { 0 };
    uint32_t dst_xy = pkt[R128_SCALE_PKT_DST_X_Y];
    uint32_t dst_hw = pkt[R128_SCALE_PKT_DST_H_W];
    uint32_t sc_tl = pkt[R128_SCALE_PKT_SC_TL];
    uint32_t sc_br = pkt[R128_SCALE_PKT_SC_BR];
    /*
     * The packet carries its own pitch/offset for both source and
     * destination -- its context dword sets both PITCH_OFFSET_CNTL bits,
     * which is precisely what those bits mean. Using the engine's
     * left-over state instead sent every scaled frame to wherever the
     * previous operation happened to be pointing.
     */
    uint32_t dpo = pkt[R128_SCALE_PKT_DST_PITCH_OFF];

    op.bpp = ati_rage128_bpp_from_dp_datatype(s);
    op.dst_x = (dst_xy >> 16) & 0x3fff;
    op.dst_y = dst_xy & 0x3fff;
    op.w = dst_hw & 0x3fff;
    op.h = (dst_hw >> 16) & 0x3fff;
    op.sc_left = sc_tl & 0x3fff;
    op.sc_top = (sc_tl >> 16) & 0x3fff;
    op.sc_right = sc_br & 0x3fff;
    op.sc_bottom = (sc_br >> 16) & 0x3fff;
    op.dt = pkt[R128_SCALE_PKT_DATATYPE] & 0xf;
    op.src_off = pkt[R128_SCALE_PKT_OFFSET] & ~7u;
    op.src_pitch = (pkt[R128_SCALE_PKT_PITCH] & 0x3fff) * 8;
    op.x_inc = pkt[R128_SCALE_PKT_X_INC] >> 4;
    op.y_inc = pkt[R128_SCALE_PKT_Y_INC] >> 4;
    op.dst_off = (dpo & R128_PITCH_OFFSET_OFF_MASK) <<
                 R128_PITCH_OFFSET_OFF_SHIFT;
    op.dst_stride = (dpo >> R128_PITCH_OFFSET_PITCH_SHIFT) * op.bpp;
    op.src_factor = R128_ALPHA_BLEND_ONE;
    op.dst_factor = R128_ALPHA_BLEND_ZERO;
    ati_rage128_2d_scale_run(s, &op);
}

/*
 * The scaler kicked through its registers (SCALE_DST_HEIGHT_WIDTH is the
 * trigger, written last), drawing with the resolved 2D/3D context: the
 * destination from DST_PITCH_OFFSET, the scissor from SC_*_C, and the
 * blend factors and scale-function select from MISC_3D_STATE_CNTL_REG.
 * This is Mac OS X's pointer whenever it does not fit the two-colour
 * hardware cursor -- see the register block's comment in the header.
 */
void ati_rage128_2d_scale_regs(ATIRage128State *s)
{
    ATIRage128ScaleOp op = { 0 };
    uint32_t misc = s->regs[R128_MISC_3D_STATE_CNTL_REG >> 2];
    uint32_t dst_xy = s->regs[R128_SCALE_DST_X_Y >> 2];
    uint32_t dst_hw = s->regs[R128_SCALE_DST_HEIGHT_WIDTH >> 2];
    unsigned fcn = (misc >> R128_MISC_SCALE_3D_FCN_SHIFT) &
                   R128_MISC_SCALE_3D_FCN_MASK;

    if (fcn != R128_MISC_SCALE_3D_SCALE ||
        !(s->dp_gui_master_cntl & R128_GMC_3D_FCN_EN)) {
        trace_ati_rage128_scale_regs_skip(fcn, s->dp_gui_master_cntl);
        return;
    }
    op.bpp = ati_rage128_bpp_from_dp_datatype(s);
    op.dst_x = (dst_xy >> 16) & 0x3fff;
    op.dst_y = dst_xy & 0x3fff;
    op.w = dst_hw & 0x3fff;
    op.h = (dst_hw >> 16) & 0x3fff;
    op.sc_left = s->sc_left;
    op.sc_top = s->sc_top;
    op.sc_right = s->sc_right;
    op.sc_bottom = s->sc_bottom;
    if (op.sc_right == 0 && op.sc_bottom == 0) {
        op.sc_right = 0x3fff;
        op.sc_bottom = 0x3fff;
    }
    op.dt = s->regs[R128_SCALE_3D_DATATYPE >> 2] & 0xf;
    op.src_off = s->regs[R128_SCALE_OFFSET_0 >> 2] & ~7u;
    op.src_pitch = (s->regs[R128_SCALE_PITCH >> 2] & 0x3fff) * 8;
    op.x_inc = s->regs[R128_SCALE_X_INC >> 2] >> 4;
    op.y_inc = s->regs[R128_SCALE_Y_INC >> 2] >> 4;
    op.dst_off = s->dst_offset;
    op.dst_stride = s->dst_pitch * op.bpp;
    op.src_factor = (misc >> R128_ALPHA_BLEND_SRC_SHIFT) & R128_ALPHA_BLEND_MASK;
    op.dst_factor = (misc >> R128_ALPHA_BLEND_DST_SHIFT) & R128_ALPHA_BLEND_MASK;
    ati_rage128_2d_scale_run(s, &op);
}

void ati_rage128_2d_blt(ATIRage128State *s)
{
    uint32_t src_source = s->dp_mix & R128_DP_SRC_SOURCE;

    trace_ati_rage128_2d_blt((s->src_x << 16) | s->src_y,
                             (s->dst_x << 16) | s->dst_y,
                             s->dst_width, s->dst_height,
                             (s->dp_mix >> 16) & 0xff, s->dp_datatype,
                             src_source >> 8, s->src_offset, s->dst_offset,
                             (s->src_pitch << 16) | s->dst_pitch);

    if (s->host_data_active) {
        /* A new blt implicitly ends any still-in-progress HOST_DATA
         * transfer, matching upstream's ati_host_data_finish(). */
        ati_rage128_host_data_flush(s);
        s->host_data_active = false;
    }

    if (src_source == R128_DP_SRC_HOST ||
        src_source == R128_DP_SRC_HOST_BYTEALIGN) {
        s->host_data_active = true;
        s->host_data_next = 0;
        s->host_data_col = 0;
        s->host_data_row = 0;
        /* the transfer takes a copy of the context it starts with --
         * see the hd comment in ati_rage128_int.h */
        s->hd.dst_x = s->dst_x;
        s->hd.dst_y = s->dst_y;
        s->hd.dst_width = s->dst_width;
        s->hd.dst_height = s->dst_height;
        s->hd.dst_offset = s->dst_offset;
        s->hd.dst_pitch = s->dst_pitch;
        s->hd.datatype = s->dp_datatype;
        s->hd.src_frgd_clr = s->dp_src_frgd_clr;
        s->hd.src_bkgd_clr = s->dp_src_bkgd_clr;
        s->hd.sc_left = s->sc_left;
        s->hd.sc_top = s->sc_top;
        s->hd.sc_right = s->sc_right;
        s->hd.sc_bottom = s->sc_bottom;
        return;
    }
    ati_rage128_2d_do_blt(s);
}

/*
 * Flush one HOST_DATA_ACC_BITS (128-bit / 4-dword) accumulator's worth
 * of pixels, pushed via the HOST_DATA0-7/LAST registers (direct MMIO
 * path) or the equivalent PM4 HOSTDATA_BLT payload dwords, into VRAM
 * at the current scanline/column position -- continuing a
 * possibly-multi-flush transfer across (s->dst_width, s->dst_height).
 * Same chunked-flush protocol as upstream's ati_host_data_flush().
 */
bool ati_rage128_host_data_flush(ATIRage128State *s)
{
    int bpp = ati_rage128_bpp_from_datatype(s->hd.datatype);
    uint32_t src_datatype = s->hd.datatype & R128_DP_SRC_DATATYPE;
    uint32_t dst_stride;
    /*
     * One accumulator holds 128 bits. As COLOUR data that is at most 16
     * pixels (8bpp); expanded from MONOCHROME it is 128 pixels of up to
     * four bytes each. Sizing this for the colour case only -- as it was
     * -- let the mono expander below run 128 pixels into a 16-byte
     * buffer, smashing the stack: a guest-triggered abort, seen live
     * (SIGABRT via __stack_chk_fail) the first time Mac OS issued a mono
     * host-data blit on this card.
     */
    uint8_t pix_buf[128 * 4];
    /*
     * Which expanded pixels must not be written at all. Source datatype
     * MONO_FRGD ("foreground / leave alone") paints only the set bits and
     * leaves the destination untouched everywhere else -- the same
     * transparency the "_LA" brush types have. Painting the clear bits
     * with the background colour instead turned every submenu arrow into
     * a solid black square, the glyph's cell filled in rather than
     * masked.
     */
    bool pix_skip[128];
    uint32_t acc[4];
    int sc_left, sc_top, sc_right, sc_bottom;
    unsigned bypp, pix_count, idx, row, col;

    if (!s->host_data_active) {
        return false;
    }
    if (!bpp || bpp == 24) {
        s->host_data_active = false;
        return false;
    }

    bypp = bpp / 8;
    dst_stride = s->hd.dst_pitch * bpp; /* pitch is in 8-pixel units */
    if (!dst_stride) {
        s->host_data_active = false;
        return false;
    }

    /*
     * HOST_BIG_ENDIAN_EN: the payload was written by a big-endian host
     * and has to be converted, by PIXEL size -- a full dword swap for
     * 32bpp, a swap within each halfword for 16bpp, nothing for 8bpp
     * (where byte order inside a dword is already the pixel order).
     * The Mac driver relies on this: it byte-swaps its COMMAND dwords in
     * software so the little-endian command fetch reads them correctly,
     * then ships bitmap payload verbatim and leaves the conversion to
     * the chip. Without it every host-supplied pixel lands reversed.
     */
    if ((s->hd.datatype & R128_HOST_BIG_ENDIAN_EN) &&
        src_datatype == R128_SRC_COLOR) {
        unsigned w;

        /*
         * Colour payload only. A monochrome source is a bitmask, not
         * pixels -- there is no pixel size to swap by, and its bit order
         * is already spelled out by BYTE_PIX_ORDER below, so leave it
         * alone rather than guess.
         */
        for (w = 0; w < ARRAY_SIZE(acc); w++) {
            uint32_t v = s->host_data_acc[w];

            if (bpp == 32) {
                acc[w] = bswap32(v);
            } else if (bpp == 16) {
                acc[w] = ((v & 0x00ff00ffu) << 8) | ((v & 0xff00ff00u) >> 8);
            } else {
                acc[w] = v;
            }
        }
    } else {
        memcpy(acc, s->host_data_acc, sizeof(acc));
    }

    memset(pix_skip, 0, sizeof(pix_skip));

    if (src_datatype == R128_SRC_COLOR) {
        pix_count = sizeof(acc) / bypp;
        memcpy(pix_buf, acc, sizeof(acc));
    } else {
        uint32_t byte_pix_order = s->hd.datatype & R128_DP_BYTE_PIX_ORDER;
        uint32_t fg = s->hd.src_frgd_clr;
        uint32_t bg = s->hd.src_bkgd_clr;
        unsigned word, byte, bit, pidx = 0;

        /* Expand the 128 accumulated monochrome bits to bypp-sized
         * foreground/background pixels. */
        bool transparent = src_datatype == R128_SRC_MONO_FRGD;

        for (word = 0; word < 4; word++) {
            for (byte = 0; byte < 4; byte++) {
                uint8_t byte_val = acc[word] >> (byte * 8);

                for (bit = 0; bit < 8; bit++) {
                    bool is_fg = byte_val &
                                 (1u << (byte_pix_order ? bit : 7 - bit));
                    uint32_t color = is_fg ? fg : bg;

                    pix_skip[pidx / bypp] = !is_fg && transparent;

                    switch (bypp) {
                    case 1:
                        pix_buf[pidx] = color;
                        break;
                    case 2:
                        stw_le_p(pix_buf + pidx, color);
                        break;
                    case 4:
                        stl_le_p(pix_buf + pidx, color);
                        break;
                    }
                    pidx += bypp;
                }
            }
        }
        /*
         * 128 bits in, one pixel out per bit. This used to be recomputed
         * as sizeof(pix_buf) / bypp, i.e. the COLOUR pixel count, so all
         * but the first few expanded pixels of every chunk were silently
         * dropped -- monochrome text and icons came out mangled.
         */
        pix_count = 128;
    }

    /*
     * The destination scissors apply here just as they do to an ordinary
     * blit. That matters more than it sounds: the Mac driver pads every
     * host-data blit's WIDTH up to a multiple of four pixels -- one
     * 128-bit accumulator chunk -- and relies on the scissors to throw
     * the padding away. Captured live, 1061 of 1613 host-data blits
     * overhang their clip rectangle, including a 1028-pixel-wide blit on
     * a 1024-pixel screen. The padding dwords are junk, so drawing them
     * put one to four columns of speckled garbage down the right-hand
     * edge of every icon, button, glyph run and menu.
     */
    sc_left = s->hd.sc_left;
    sc_top = s->hd.sc_top;
    sc_right = s->hd.sc_right;
    sc_bottom = s->hd.sc_bottom;
    if (sc_right == 0 && sc_bottom == 0) {
        sc_right = 0x3fff;
        sc_bottom = 0x3fff;
    }

    row = s->host_data_row;
    col = s->host_data_col;
    idx = 0;
    while (idx < pix_count && row < s->hd.dst_height) {
        unsigned n = MIN(pix_count - idx, s->hd.dst_width - col);
        unsigned i;

        for (i = 0; i < n; i++) {
            int dx = (int)(s->hd.dst_x + col + i);
            int dy = (int)(s->hd.dst_y + row);
            uint32_t color;

            if (dx < sc_left || dx > sc_right ||
                dy < sc_top || dy > sc_bottom) {
                continue;
            }
            if (pix_skip[idx + i]) {
                continue;       /* mask bit clear: leave the destination */
            }
            switch (bypp) {
            case 1:
                color = pix_buf[(idx + i) * bypp];
                break;
            case 2:
                color = lduw_le_p(pix_buf + (idx + i) * bypp);
                break;
            case 4:
                color = ldl_le_p(pix_buf + (idx + i) * bypp);
                break;
            default:
                color = 0;
                break;
            }
            ati_rage128_2d_write_pixel(s, s->hd.dst_offset, dst_stride,
                                       s->hd.dst_x + col + i,
                                       s->hd.dst_y + row, bpp, color);
        }
        idx += n;
        col += n;
        if (col >= s->hd.dst_width) {
            col = 0;
            row++;
        }
    }
    s->host_data_row = row;
    s->host_data_col = col;
    if (s->host_data_row >= s->hd.dst_height) {
        s->host_data_active = false;
    }
    return s->host_data_active;
}

