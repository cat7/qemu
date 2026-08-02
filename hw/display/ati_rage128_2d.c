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

static void ati_rage128_2d_do_blt(ATIRage128State *s)
{
    int bpp = ati_rage128_bpp_from_dp_datatype(s);
    uint8_t rop = (s->dp_mix >> 16) & 0xff;
    bool left_to_right = s->dp_cntl & R128_DST_X_LEFT_TO_RIGHT;
    bool top_to_bottom = s->dp_cntl & R128_DST_Y_TOP_TO_BOTTOM;
    int width = s->dst_width;
    int height = s->dst_height;
    uint32_t dst_stride, src_stride;
    int sc_left, sc_top, sc_right, sc_bottom;
    int x, y;

    if (!bpp || width == 0 || height == 0) {
        return;
    }

    dst_stride = s->dst_pitch * (bpp / 8);
    src_stride = s->src_pitch * (bpp / 8);
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
            uint32_t pat_pixel = s->dp_brush_frgd_clr;
            uint32_t result;

            if (dx < sc_left || dx > sc_right) {
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

void ati_rage128_2d_blt(ATIRage128State *s)
{
    uint32_t src_source = s->dp_mix & R128_DP_SRC_SOURCE;

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
    uint8_t pix_buf[16]; /* 128 bits */
    unsigned bypp, pix_count, idx, row, col;

    if (!s->host_data_active) {
        return false;
    }
    if (!bpp || bpp == 24) {
        s->host_data_active = false;
        return false;
    }

    bypp = bpp / 8;
    dst_stride = s->dst_pitch * bypp;
    if (!dst_stride) {
        s->host_data_active = false;
        return false;
    }

    if (src_datatype == R128_SRC_COLOR) {
        pix_count = sizeof(pix_buf) / bypp;
        memcpy(pix_buf, s->host_data_acc, sizeof(s->host_data_acc));
    } else {
        uint32_t byte_pix_order = s->dp_datatype & R128_DP_BYTE_PIX_ORDER;
        uint32_t fg = s->dp_src_frgd_clr;
        uint32_t bg = s->dp_src_bkgd_clr;
        unsigned word, byte, bit, pidx = 0;

        /* Expand the 128 accumulated monochrome bits to bypp-sized
         * foreground/background pixels. */
        for (word = 0; word < 4; word++) {
            for (byte = 0; byte < 4; byte++) {
                uint8_t byte_val = s->host_data_acc[word] >> (byte * 8);

                for (bit = 0; bit < 8; bit++) {
                    bool is_fg = byte_val &
                                 (1u << (byte_pix_order ? bit : 7 - bit));
                    uint32_t color = is_fg ? fg : bg;

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
        pix_count = sizeof(pix_buf) / bypp;
    }

    row = s->host_data_row;
    col = s->host_data_col;
    idx = 0;
    while (idx < pix_count && row < s->dst_height) {
        unsigned n = MIN(pix_count - idx, s->dst_width - col);
        unsigned i;

        for (i = 0; i < n; i++) {
            uint32_t color;

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

