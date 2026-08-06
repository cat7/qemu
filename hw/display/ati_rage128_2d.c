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
static int ati_rage128_bpp_from_dp_datatype(ATIRage128State *s)
{
    switch (s->dp_datatype & 0xf) {
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

static void ati_rage128_2d_do_blt(ATIRage128State *s)
{
    int bpp = ati_rage128_bpp_from_dp_datatype(s);
    uint8_t rop = (s->dp_mix >> 16) & 0xff;
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
            result = ati_rage128_apply_rop3(rop, src_pixel, dst_pixel,
                                            pat_pixel);
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
        return ldl_be_p(row + sx * 4) & 0xffffff;
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
 * CNTL_SCALING: a scaled, optionally YUV-converting copy from a source
 * image in VRAM into the destination rectangle. This is how Mac OS plays
 * video on this card -- the counterpart of the mach64's scaler pipe, and
 * it carries the same parameters for the same movie.
 *
 * Nearest-neighbour: the increments are DDA accumulator steps with 12
 * fractional bits, sitting at bits 19:4 exactly as the mach64's do.
 * Reading them unshifted gives a sixteen-fold downscale, which cannot be
 * right when the source and destination are the same size.
 */
void ati_rage128_2d_scale(ATIRage128State *s, const uint32_t *pkt)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t dst_xy = pkt[R128_SCALE_PKT_DST_X_Y];
    uint32_t dst_hw = pkt[R128_SCALE_PKT_DST_H_W];
    uint32_t sc_tl = pkt[R128_SCALE_PKT_SC_TL];
    uint32_t sc_br = pkt[R128_SCALE_PKT_SC_BR];
    unsigned dt = pkt[R128_SCALE_PKT_DATATYPE] & 0xf;
    uint32_t src_off = pkt[R128_SCALE_PKT_OFFSET] & ~7u;
    uint32_t src_pitch = (pkt[R128_SCALE_PKT_PITCH] & 0x3fff) * 8;
    uint32_t x_inc = pkt[R128_SCALE_PKT_X_INC] >> 4;
    uint32_t y_inc = pkt[R128_SCALE_PKT_Y_INC] >> 4;
    int dst_x = (dst_xy >> 16) & 0x3fff, dst_y = dst_xy & 0x3fff;
    int w = dst_hw & 0x3fff, h = (dst_hw >> 16) & 0x3fff;
    int sc_left = sc_tl & 0x3fff, sc_top = (sc_tl >> 16) & 0x3fff;
    int sc_right = sc_br & 0x3fff, sc_bottom = (sc_br >> 16) & 0x3fff;
    int bpp = ati_rage128_bpp_from_dp_datatype(s);
    /*
     * The packet carries its own pitch/offset for both source and
     * destination -- its context dword sets both PITCH_OFFSET_CNTL bits,
     * which is precisely what those bits mean. Using the engine's
     * left-over state instead sent every scaled frame to wherever the
     * previous operation happened to be pointing.
     */
    uint32_t dpo = pkt[R128_SCALE_PKT_DST_PITCH_OFF];
    uint32_t dst_off = (dpo & R128_PITCH_OFFSET_OFF_MASK) <<
                       R128_PITCH_OFFSET_OFF_SHIFT;
    uint32_t dst_stride = (dpo >> R128_PITCH_OFFSET_PITCH_SHIFT) * bpp;
    int src_bpp, x, y;

    trace_ati_rage128_scale(dst_x, dst_y, w, h, src_off, src_pitch,
                            x_inc, y_inc, dt);
    if (!bpp || !dst_stride || !x_inc || !y_inc || !src_pitch ||
        w <= 0 || h <= 0) {
        return;
    }

    switch (dt) {
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
        trace_ati_rage128_scale_unimp(dt);
        return;
    }

    for (y = 0; y < h; y++) {
        int dy = dst_y + y;
        uint32_t sy = ((uint32_t)y * y_inc) >> 12;
        const uint8_t *row;

        if (dy < sc_top || dy > sc_bottom) {
            continue;
        }
        row = vram + src_off + sy * src_pitch * (uint32_t)src_bpp;
        if (src_off + (sy + 1) * src_pitch * (uint32_t)src_bpp >
            ATI_RAGE128_VRAM_SIZE) {
            break;
        }
        for (x = 0; x < w; x++) {
            int dx = dst_x + x;
            uint32_t sx = ((uint32_t)x * x_inc) >> 12;

            if (dx < sc_left || dx > sc_right) {
                continue;
            }
            if ((sx + 1) * (uint32_t)src_bpp > src_pitch * (uint32_t)src_bpp) {
                break;
            }
            ati_rage128_2d_write_pixel(s, dst_off, dst_stride, dx, dy, bpp,
                                       ati_rage128_scale_texel(row, sx, dt));
        }
    }
}

void ati_rage128_2d_blt(ATIRage128State *s)
{
    uint32_t src_source = s->dp_mix & R128_DP_SRC_SOURCE;

    trace_ati_rage128_2d_blt(s->src_offset, (s->src_x << 16) | s->src_y,
                             s->dst_offset, (s->dst_x << 16) | s->dst_y,
                             s->dst_width, s->dst_height,
                             (s->dp_mix >> 16) & 0xff, s->dp_datatype,
                             src_source >> 8, s->dp_cntl);

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
    int bpp = ati_rage128_bpp_from_dp_datatype(s);
    uint32_t src_datatype = s->dp_datatype & R128_DP_SRC_DATATYPE;
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
    dst_stride = s->dst_pitch * bpp; /* pitch is in 8-pixel units */
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
    if ((s->dp_datatype & R128_HOST_BIG_ENDIAN_EN) &&
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
        uint32_t byte_pix_order = s->dp_datatype & R128_DP_BYTE_PIX_ORDER;
        uint32_t fg = s->dp_src_frgd_clr;
        uint32_t bg = s->dp_src_bkgd_clr;
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
    sc_left = s->sc_left;
    sc_top = s->sc_top;
    sc_right = s->sc_right;
    sc_bottom = s->sc_bottom;
    if (sc_right == 0 && sc_bottom == 0) {
        sc_right = 0x3fff;
        sc_bottom = 0x3fff;
    }

    row = s->host_data_row;
    col = s->host_data_col;
    idx = 0;
    while (idx < pix_count && row < s->dst_height) {
        unsigned n = MIN(pix_count - idx, s->dst_width - col);
        unsigned i;

        for (i = 0; i < n; i++) {
            int dx = (int)(s->dst_x + col + i);
            int dy = (int)(s->dst_y + row);
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
            ati_rage128_2d_write_pixel(s, s->dst_offset, dst_stride,
                                       s->dst_x + col + i, s->dst_y + row,
                                       bpp, color);
        }
        idx += n;
        col += n;
        if (col >= s->dst_width) {
            col = 0;
            row++;
        }
    }
    s->host_data_row = row;
    s->host_data_col = col;
    if (s->host_data_row >= s->dst_height) {
        s->host_data_active = false;
    }
    return s->host_data_active;
}

