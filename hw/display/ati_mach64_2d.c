/*
 * QEMU ATI Mach64 GT (Rage Pro) emulation -- GUI (2D) engine.
 *
 * Split out of ati_mach64.c following the layout of the upstream
 * ati-vga device (ati_2d.c). See the block comment below for the
 * engine's provenance and semantics.
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "system/memory.h"
#include "ui/console.h"

#include "hw/display/ati_mach64_int.h"
#include "ati_mach64_regs.h"
#include "trace.h"

/*
 * GUI (2D) engine -- the Mach64 BitBLT block. Register semantics per
 * Linux's include/video/mach64.h / mach64_accel.c, ATI's own register
 * references (RRG-G02700 for the VT/GT, LT3REGRE for the Rage Pro
 * generation this device models) and the mach64 Programmer's Guide
 * (PRG888GX0-01) consumption tables, cross-checked against a live
 * register trace of Apple's accelerated Mac OS 9 NDRV.
 *
 * The data path is the documented one: every operation color-expands
 * a monochrome channel (all-ones / fixed 8x8 pattern / host stream /
 * 1bpp blit source) into a foreground or background color source
 * (color registers, blit source, host stream, fixed color patterns),
 * applies the foreground/background bitwise mix, the color-compare
 * suppression and the plane write mask, under rectangular scissors.
 * Rectangles trigger on a DST_HEIGHT_WIDTH write, Bresenham lines on
 * a DST_BRES_LNTH write. Operations complete synchronously, so
 * GUI_STAT always reads idle and FIFO_STAT always reads empty.
 *
 * Multi-byte pixel values (colors, blit data, compare values) are
 * byte-ordered little-endian in VRAM, matching the LE register
 * aperture; big-endian guests compensate exactly as on real hardware
 * (swapped stores, HOST_CNTL's big-endian translation, the BE
 * framebuffer aperture).
 *
 * Still unimplemented, traced loudly via ati_mach64_2d_unimp: 1bpp
 * and 4bpp destination modes, polygon/trapezoid fills, the
 * generalized (VRAM-sourced) rotating pattern, and host-sourced
 * background channels.
 *
 * The guest's direction bits (DST_CNTL) are used only to interpret
 * the start coordinates (right-to-left/bottom-to-top ops give the
 * right/bottom corner); execution order for overlapping copies is
 * chosen internally (row order by overlap direction, a temporary row
 * buffer within a row), which is always safe.
 */
static int ati_mach64_2d_bytes_pp(uint32_t pix_width_code)
{
    switch (pix_width_code & ATI_PIX_WIDTH_DST_MASK) {
    case 2:
        return 1;           /* 8 bpp (also packed 24bpp via 24_ROT) */
    case 3:                 /* 15 bpp */
    case 4:                 /* 16 bpp */
        return 2;
    case 6:
        return 4;           /* 32 bpp */
    default:
        return 0;           /* 1/4 bpp engine modes: not implemented */
    }
}

/*
 * One byte of the bitwise ALU (LT3REGRE Table 5-10). Every mix code
 * on this chip generation is a pure bitwise function, so byte-wise
 * application is exact at any pixel depth (codes 0x10-0x1F are
 * documented Reserved on the Rage Pro).
 */
static inline uint8_t ati_mach64_2d_mix_byte(uint8_t d, uint8_t sr, uint8_t mix)
{
    switch (mix & 0x1f) {
    case ATI_MIX_NOT_DST:         return ~d;
    case ATI_MIX_0:               return 0x00;
    case ATI_MIX_1:               return 0xff;
    case ATI_MIX_D:               return d;
    case ATI_MIX_NOT_SRC:         return ~sr;
    case ATI_MIX_XOR:             return d ^ sr;
    case ATI_MIX_XNOR:            return ~(d ^ sr);
    case ATI_MIX_S:               return sr;
    case ATI_MIX_NAND:            return ~(d & sr);
    case ATI_MIX_NOT_SRC_OR_DST:  return ~sr | d;
    case ATI_MIX_SRC_OR_NOT_DST:  return sr | ~d;
    case ATI_MIX_OR:              return d | sr;
    case ATI_MIX_AND:             return d & sr;
    case ATI_MIX_SRC_AND_NOT_DST: return sr & ~d;
    case ATI_MIX_NOT_SRC_AND_DST: return ~sr & d;
    case ATI_MIX_NOR:             return ~(d | sr);
    default:                      return d;
    }
}

/*
 * Multi-byte pixel values are stored BIG-endian in VRAM by the
 * engine. Every guest this Mac card serves runs a big-endian
 * framebuffer (CPU pixels land via swapped stores or the BE
 * aperture), and the drivers program NATURAL color values: the live
 * NDRV trace shows 15bpp fills of e.g. 0x4210 (mid gray), which only
 * displays as gray if the engine stores the high byte first --
 * confirmed against DingusPPC's ATIRage, whose 15bpp engine fill does
 * BYTESWAP_16. A little-endian store here was the root cause of the
 * previously wrong-colored/never-erased 15bpp and 32bpp accelerated
 * desktops (Mac OS 9 at thousands-of-colors, OS X's boot screen).
 * Screen-to-screen copies are raw byte moves and don't care.
 */
static inline uint32_t ati_mach64_2d_get(const uint8_t *p, int bpp)
{
    uint32_t v = 0;
    int b;

    for (b = 0; b < bpp; b++) {
        v |= (uint32_t)p[bpp - 1 - b] << (8 * b);
    }
    return v;
}

/*
 * Color-compare suppression (LT3REGRE 5-61): when the selected
 * compare function is TRUE for this pixel, the destination is left
 * unchanged. The compared value is the destination pixel or the 2D
 * source color, masked by CLR_CMP_MSK.
 */
static inline bool ati_mach64_2d_cmp_reject(const ATIMach64State *s,
                                            uint32_t dstv, uint32_t srcv)
{
    uint32_t cntl = s->regs[ATI_CLR_CMP_CNTL >> 2];
    uint32_t fn = cntl & ATI_CLR_CMP_FN_MASK;
    uint32_t msk, v;
    bool match;

    if (fn == ATI_CLR_CMP_FN_FALSE) {
        return false;
    }
    if (fn == ATI_CLR_CMP_FN_TRUE) {
        return true;
    }
    msk = s->regs[ATI_CLR_CMP_MSK >> 2];
    v = ((cntl >> ATI_CLR_CMP_SRC_SHIFT) & ATI_CLR_CMP_SRC_MASK)
        == ATI_CLR_CMP_SRC_2D ? srcv : dstv;
    match = ((v ^ s->regs[ATI_CLR_CMP_CLR >> 2]) & msk) == 0;
    switch (fn) {
    case ATI_CLR_CMP_FN_EQUAL:
        return match;
    case ATI_CLR_CMP_FN_NOT_EQUAL:
        return !match;
    default:
        return false;       /* reserved functions: draw normally */
    }
}

/*
 * Write one destination pixel through the full back end of the data
 * path: color compare, mix, plane write mask. mask_phase selects
 * which byte lane of DP_WRITE_MSK the pixel's first byte uses --
 * normally 0 (so an 8bpp pixel always uses mask bits 7:0, matching
 * planemask semantics), but the absolute byte position under packed
 * 24bpp rotation.
 */
static void ati_mach64_2d_put(const ATIMach64State *s, uint8_t *p, int bpp,
                              uint32_t color, uint8_t mix, int mask_phase)
{
    uint32_t wmsk = s->regs[ATI_DP_WRITE_MSK >> 2];
    int b;

    if (ati_mach64_2d_cmp_reject(s, ati_mach64_2d_get(p, bpp), color)) {
        return;
    }
    for (b = 0; b < bpp; b++) {
        uint8_t *pb = &p[bpp - 1 - b];      /* value byte b, stored BE */
        uint8_t m = wmsk >> (8 * ((mask_phase + b) & 3));
        uint8_t n = ati_mach64_2d_mix_byte(*pb, (color >> (8 * b)) & 0xff,
                                           mix);
        *pb = (n & m) | (*pb & ~m);
    }
}

/*
 * Fixed-pattern lookups (mach64 Programmer's Guide "Pattern
 * Consumption", PRG888GX0-01 2-36), indexed by absolute destination
 * coordinates. Mono 8x8: row (y mod 8) is byte (row & 3) of
 * PAT_REG0/1 (low byte first, PAT_REG1 for rows 4-7); the leftmost
 * pixel is the row byte's MSB unless DP_BYTE_PIX_ORDER selects
 * LSB-first. Color patterns (8bpp only): 4x2 uses byte (x mod 4) of
 * PAT_REG0 (row 0) / PAT_REG1 (row 1); 8x1 runs the 8 bytes of
 * PAT_REG0 then PAT_REG1 across x.
 */
static bool ati_mach64_2d_pat_mono(const ATIMach64State *s, int x, int y)
{
    uint32_t reg = s->regs[((y & 4) ? ATI_PAT_REG1 : ATI_PAT_REG0) >> 2];
    uint8_t row = reg >> (8 * (y & 3));
    bool lsb_first = s->regs[ATI_DP_PIX_WIDTH >> 2] & ATI_PIX_WIDTH_BYTE_ORDER;

    return (row >> (lsb_first ? (x & 7) : 7 - (x & 7))) & 1;
}

static uint8_t ati_mach64_2d_pat_color(const ATIMach64State *s, int x, int y)
{
    if (s->regs[ATI_PAT_CNTL >> 2] & ATI_PAT_CLR_8x1_EN) {
        uint32_t reg = s->regs[((x & 4) ? ATI_PAT_REG1 : ATI_PAT_REG0) >> 2];
        return reg >> (8 * (x & 3));
    }
    /* 4x2 */
    return s->regs[((y & 1) ? ATI_PAT_REG1 : ATI_PAT_REG0) >> 2]
           >> (8 * (x & 3));
}

/*
 * Scissor accessors: left/right are signed 14-bit, top/bottom signed
 * 15-bit (LT3REGRE 5.2.3: X range -8192..8191, Y range -16384..16383;
 * negative scissors are real usage -- XFree86's left-edge clipping
 * programs SC_LEFT below zero).
 */
static inline int ati_mach64_sc_left(const ATIMach64State *s)
{
    return sextract32(s->regs[ATI_SC_LEFT >> 2], 0, 14);
}
static inline int ati_mach64_sc_right(const ATIMach64State *s)
{
    return sextract32(s->regs[ATI_SC_RIGHT >> 2], 0, 14);
}
static inline int ati_mach64_sc_top(const ATIMach64State *s)
{
    return sextract32(s->regs[ATI_SC_TOP >> 2], 0, 15);
}
static inline int ati_mach64_sc_bottom(const ATIMach64State *s)
{
    return sextract32(s->regs[ATI_SC_BOTTOM >> 2], 0, 15);
}

/*
 * Consume one 32-bit HOST_DATA word for an in-progress host-source
 * blit (see the hb_* fields in ati_mach64.h). Consumption order per
 * the mach64 Programmer's Guide "Host Data Consumption" tables
 * (PRG888GX0-01 2-35): bytes of the word are consumed low to high;
 * within a byte, mono bits go MSB-first, or LSB-first when
 * DP_BYTE_PIX_ORDER is set. HOST_CNTL's big-endian translation
 * byte-swaps the whole incoming word first (how Apple's swapped-store
 * NDRV lands its glyph bitmaps in the documented order -- HOST_CNTL
 * reads 0x2 in its live trace). Packed color pixels are consumed low
 * byte first.
 *
 * Two modes:
 *  - Mono expansion (hb_mono): each bit is one destination pixel (a
 *    1 uses the foreground colour/mix, a 0 the background) -- the
 *    glyph path. With hb_triple (packed 24bpp), each bit covers the
 *    pixel's three destination bytes.
 *  - Packed colour (host bpp 8/16/32): whole pixels through the
 *    foreground mix.
 *
 * At the row's right edge the cursor wraps to the next row; if
 * HOST_BYTE_ALIGN is set, consumption re-aligns to the next byte
 * boundary (not a fresh word -- the guide is explicit that alignment
 * is to the nearest byte).
 */
void ati_mach64_host_data(ATIMach64State *s, uint32_t word)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    int sc_left = ati_mach64_sc_left(s);
    int sc_right = ati_mach64_sc_right(s);
    int sc_top = ati_mach64_sc_top(s);
    int sc_bottom = ati_mach64_sc_bottom(s);
    int bpp = s->hb_dst_bpp;
    int i;

    s->hb_words++;
    /*
     * HOST_CNTL's big-endian translation, per pixel width (LT3REGRE
     * 5-34): "In 15 bpp and 16 bpp modes, the bytes within each word
     * are swapped. In 32 bpp mode, the order of the four bytes within
     * each DWORD is reversed." It does NOT apply to 1bpp masks or
     * 8bpp pixels, whose bytes already arrive in stream order.
     *
     * Swapping the whole dword for 16bpp (as this used to) exchanges
     * the two pixels as well as their bytes, so every adjacent pixel
     * pair came out reversed: invisible on solid fills, but it doubles
     * and shreds one-pixel-wide glyph strokes -- the unreadable menu
     * and window text. Byte-swapping a 1bpp mask reverses the glyph's
     * bytes within each word for the same reason.
     */
    if (s->hb_bigendian) {
        if (s->hb_host_bpp == 2 && !s->hb_mono) {
            word = ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
        } else if (s->hb_host_bpp == 4 && !s->hb_mono) {
            word = bswap32(word);
        }
    }

    if (!s->hb_mono) {
        int npix = 4 / s->hb_host_bpp;

        for (i = 0; i < npix && s->hb_h > 0; i++) {
            uint32_t color = extract32(word, 8 * s->hb_host_bpp * i,
                                       8 * s->hb_host_bpp);

            if (s->hb_x >= sc_left && s->hb_x <= sc_right &&
                s->hb_y >= sc_top && s->hb_y <= sc_bottom &&
                s->hb_x >= 0 && s->hb_y >= 0) {
                uint64_t off = s->hb_dst_base +
                    ((uint64_t)s->hb_y * s->hb_dst_pitch + s->hb_x) * bpp;
                if (off + bpp <= ATI_MACH64_VRAM_SIZE) {
                    ati_mach64_2d_put(s, vram + off, bpp, color,
                                      s->hb_frgd_mix, 0);
                }
            }
            if (++s->hb_x >= s->hb_x0 + s->hb_w) {
                s->hb_x = s->hb_x0;
                s->hb_y++;
                s->hb_h--;
                /* color rows re-align to a pixel boundary naturally */
            }
        }
    } else {
        for (s->hb_bit = 0; s->hb_bit < 32 && s->hb_h > 0; s->hb_bit++) {
            int byte = s->hb_bit >> 3;
            int bitpos = s->hb_lsb_first ? (s->hb_bit & 7)
                                         : 7 - (s->hb_bit & 7);
            int bit = (word >> (8 * byte + bitpos)) & 1;
            uint32_t color = bit ? s->hb_frgd_clr : s->hb_bkgd_clr;
            uint8_t mix = bit ? s->hb_frgd_mix : s->hb_bkgd_mix;
            int nbytes = s->hb_triple ? 3 : 1;
            int b;

            for (b = 0; b < nbytes; b++) {
                /*
                 * scissors are in engine-mode pixel units, which at
                 * packed 24bpp are destination bytes already
                 */
                int x = s->hb_triple ? s->hb_x * 3 + b : s->hb_x;

                if (x >= sc_left && x <= sc_right && x >= 0 &&
                    s->hb_y >= sc_top && s->hb_y <= sc_bottom &&
                    s->hb_y >= 0) {
                    uint64_t off = s->hb_dst_base +
                        ((uint64_t)s->hb_y * s->hb_dst_pitch + x) * bpp;
                    uint32_t c = s->hb_rot24
                        ? (color >> (8 * (2 - (x % 3)))) & 0xff : color;
                    if (off + bpp <= ATI_MACH64_VRAM_SIZE) {
                        ati_mach64_2d_put(s, vram + off, bpp, c, mix,
                                          s->hb_rot24 ? (x & 3) : 0);
                    }
                }
            }

            if (++s->hb_x >= s->hb_x0 + s->hb_w) {
                s->hb_x = s->hb_x0;
                s->hb_y++;
                s->hb_h--;
                if (s->hb_byte_align) {
                    s->hb_bit |= 7;    /* skip to the next byte boundary */
                }
            }
        }
    }

    if (s->hb_h <= 0) {
        int per_word = s->hb_mono ? 32 : (4 / s->hb_host_bpp);
        int expect = s->hb_byte_align && s->hb_mono ?
            ((s->hb_w + 31) / 32) * s->hb_h0 :
            (s->hb_w * s->hb_h0 + per_word - 1) / per_word;

        trace_ati_mach64_hb_done(s->hb_w, s->hb_h0, s->hb_host_bpp,
                                 s->hb_mono, s->hb_words, expect);
        s->hb_active = false;
        memory_region_set_dirty(&s->vram, 0, ATI_MACH64_VRAM_SIZE);
    }
}

/*
 * Resolve the color for one destination pixel from a foreground or
 * background source selector. sp points at this pixel's blit source
 * bytes (already resolved through the source trajectory, and
 * overlap-safe), or NULL when no blit source is in use. abs_x/abs_y
 * are absolute destination coordinates (pattern lookups are
 * screen-anchored).
 */
static inline uint32_t ati_mach64_2d_src_color(const ATIMach64State *s,
                                               uint32_t sel,
                                               const uint8_t *sp,
                                               int bpp,
                                               int abs_x, int abs_y,
                                               bool rot24)
{
    switch (sel) {
    case ATI_BKGD_SRC_BKGD_CLR:
    case ATI_BKGD_SRC_FRGD_CLR: {
        uint32_t c = s->regs[(sel == ATI_BKGD_SRC_FRGD_CLR ?
                              ATI_DP_FRGD_CLR : ATI_DP_BKGD_CLR) >> 2];
        /*
         * packed 24bpp: each 8bpp "pixel" is one component byte of
         * the 24-bit color, high byte first in memory
         */
        return rot24 ? (c >> (8 * (2 - (abs_x % 3)))) & 0xff : c;
    }
    case ATI_BKGD_SRC_BLIT:
        return sp ? ati_mach64_2d_get(sp, bpp) : 0;
    case ATI_BKGD_SRC_PATTERN:
        return ati_mach64_2d_pat_color(s, abs_x, abs_y);
    default:
        return 0;
    }
}

void ati_mach64_2d_op(ATIMach64State *s)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t dp_src = s->regs[ATI_DP_SRC >> 2];
    uint32_t dp_mix = s->regs[ATI_DP_MIX >> 2];
    uint8_t frgd_mix = (dp_mix >> ATI_FRGD_MIX_SHIFT) & ATI_FRGD_MIX_MASK;
    uint8_t bkgd_mix = dp_mix & ATI_BKGD_MIX_MASK;
    uint32_t dst_cntl = s->regs[ATI_DST_CNTL >> 2];
    uint32_t dst_op = s->regs[ATI_DST_OFF_PITCH >> 2];
    uint32_t src_op = s->regs[ATI_SRC_OFF_PITCH >> 2];
    uint32_t pix_width = s->regs[ATI_DP_PIX_WIDTH >> 2];
    int bpp = ati_mach64_2d_bytes_pp(pix_width);
    int dst_x = s->regs[ATI_DST_X >> 2] & 0x1fff;
    int dst_y = s->regs[ATI_DST_Y >> 2] & 0x7fff;
    int src_x = s->regs[ATI_SRC_X >> 2] & 0x1fff;
    int src_y = s->regs[ATI_SRC_Y >> 2] & 0x7fff;
    int w = s->regs[ATI_DST_WIDTH >> 2] & 0x3fff;
    int h = s->regs[ATI_DST_HEIGHT >> 2] & 0x7fff;
    int sc_left = ati_mach64_sc_left(s);
    int sc_right = ati_mach64_sc_right(s);
    int sc_top = ati_mach64_sc_top(s);
    int sc_bottom = ati_mach64_sc_bottom(s);
    uint32_t dst_base = (dst_op & 0xfffff) * 8;
    int dst_pitch = ((dst_op >> 22) & 0x3ff) * 8;   /* pixels */
    uint32_t src_base = (src_op & 0xfffff) * 8;
    int src_pitch = ((src_op >> 22) & 0x3ff) * 8;
    uint32_t frgd_src = (dp_src & ATI_FRGD_SRC_MASK) >> 8;
    uint32_t bkgd_src = dp_src & ATI_BKGD_SRC_MASK;
    uint32_t mono_src = dp_src & ATI_MONO_SRC_MASK;
    uint32_t pat_cntl = s->regs[ATI_PAT_CNTL >> 2];
    uint32_t src_cntl = s->regs[ATI_SRC_CNTL >> 2];
    /*
     * Source trajectories (mach64 Programmer's Guide 2-40..2-42). The
     * source is NOT simply the destination rectangle read linearly:
     *   1 linear  (SRC_LINEAR_EN): consumed as a flat byte stream
     *   2 default: X wraps at SRC_WIDTH1, Y unbounded
     *   3 pattern (SRC_PATTERN_EN): X and Y wrap (SRC_WIDTH1/HEIGHT1)
     *   4 rotated (SRC_ROTATION_EN): as 3, starting at SRC_X/Y_START
     * Trajectories 3/4 are how a small tile is repeated over a large
     * destination -- the accelerated Mac OS NDRV paints the whole
     * desktop pattern with one such blit (the register reference calls
     * this combo out as "pattern ROPs for Apple"). Ignoring the wrap
     * made that blit read straight past the tile, so the desktop got
     * one tile band and then whatever followed it in VRAM.
     */
    bool src_linear = src_cntl & ATI_SRC_LINEAR_EN;
    bool src_patt = src_cntl & ATI_SRC_PATTERN_EN;
    bool src_rot = src_cntl & ATI_SRC_ROTATION_EN;
    int src_w1 = s->regs[ATI_SRC_WIDTH1 >> 2] & 0x3fff;
    int src_h1 = s->regs[ATI_SRC_HEIGHT1 >> 2] & 0x3fff;
    int src_xs = src_rot ? (s->regs[ATI_SRC_X_START >> 2] & 0x1fff) : 0;
    int src_ys = src_rot ? (s->regs[ATI_SRC_Y_START >> 2] & 0x3fff) : 0;
    /*
     * Only walk a wrapping trajectory when the guest explicitly asked
     * for one. The default trajectory nominally wraps X at SRC_WIDTH1
     * too, but honouring that on a SRC_WIDTH1 the driver did not set
     * for this blit (it is left over from the previous, e.g. the
     * desktop's 128-pixel tile) repeats a narrow strip across every
     * ordinary copy -- which smears window text and breaks scrolling,
     * since the wrapping path also gives up the overlap-safe staged
     * row used for same-buffer copies.
     */
    bool src_wraps = src_linear || src_patt || src_rot;
    bool rot24 = (dst_cntl & ATI_DST_24_ROT_EN) && bpp == 1;
    bool mono_one = mono_src == ATI_MONO_SRC_ONE;
    bool mono_pat = mono_src == ATI_MONO_SRC_PATTERN ||
                    /*
                     * Apple's NDRV selects the fixed mono pattern via
                     * FRGD_SRC_PATTERN with the mono channel at ONE
                     */
                    (mono_one && frgd_src == ATI_BKGD_SRC_PATTERN &&
                     (pat_cntl & (ATI_PAT_MONO_EN | ATI_PAT_MONO_EN_TRAJ)));
    bool mono_blit = mono_src == ATI_MONO_SRC_BLIT;
    bool mono_host = mono_src == ATI_MONO_SRC_HOST;
    bool blit_used = frgd_src == ATI_BKGD_SRC_BLIT ||
                     (!mono_one && bkgd_src == ATI_BKGD_SRC_BLIT) ||
                     mono_blit;
    bool host_color = frgd_src == ATI_BKGD_SRC_HOST;
    uint8_t *rowbuf = NULL;
    int y, x, clip;

    /*
     * The NDRV's FRGD_SRC_PATTERN mono-pattern idiom: the effective
     * per-bit foreground is the foreground color register.
     */
    uint32_t eff_frgd_src = (mono_pat && frgd_src == ATI_BKGD_SRC_PATTERN &&
                             !(pat_cntl & (ATI_PAT_CLR_4x2_EN |
                                           ATI_PAT_CLR_8x1_EN)))
                            ? ATI_BKGD_SRC_FRGD_CLR : frgd_src;

    if (w == 0 || h == 0) {
        trace_ati_mach64_2d_drop("zero-size", dst_x, dst_y, w, h,
                                 sc_right, sc_bottom, dp_src, dst_base);
        return;
    }

    if (bpp == 0 || dst_pitch == 0 ||
        (dst_cntl & ATI_DST_POLYGON_EN) ||
        bkgd_src == ATI_BKGD_SRC_HOST ||
        (host_color && !mono_one)) {
        trace_ati_mach64_2d_unimp(dp_src, dp_mix, pix_width);
        return;
    }
    /*
     * Fixed color patterns are 8bpp-only and need their PAT_CNTL
     * enable; a PATTERN source selector outside the mono-pattern
     * idiom without one is the (unimplemented) generalized source
     * pattern.
     */
    if (((eff_frgd_src == ATI_BKGD_SRC_PATTERN && !mono_pat) ||
         (!mono_one && bkgd_src == ATI_BKGD_SRC_PATTERN)) &&
        (bpp != 1 || !(pat_cntl & (ATI_PAT_CLR_4x2_EN | ATI_PAT_CLR_8x1_EN)))) {
        trace_ati_mach64_2d_unimp(dp_src, dp_mix, pix_width);
        return;
    }

    /*
     * Host-source blit: the CPU will stream pixel/mask words to the
     * HOST_DATA registers next. Latch the destination rectangle and
     * expansion state; drawing happens in ati_mach64_host_data() as
     * the words arrive.
     */
    if (mono_host || host_color) {
        int host_code = (pix_width >> ATI_PIX_WIDTH_HOST_SHIFT) &
                        ATI_PIX_WIDTH_HOST_MASK;
        int host_bpp = ati_mach64_2d_bytes_pp(host_code);
        uint32_t host_cntl = s->regs[ATI_HOST_CNTL >> 2];

        if (host_color && host_bpp == 0) {
            trace_ati_mach64_2d_unimp(dp_src, dp_mix, pix_width);
            return;
        }
        if (!(dst_cntl & ATI_DST_X_LEFT_TO_RIGHT)) {
            dst_x -= w - 1;
        }
        if (!(dst_cntl & ATI_DST_Y_TOP_TO_BOTTOM)) {
            dst_y -= h - 1;
        }
        if (s->hb_active && s->hb_h > 0) {
            trace_ati_mach64_hb_stale(s->hb_h, s->hb_w, s->hb_h0);
        }
        s->hb_words = 0;
        s->hb_h0 = h;
        s->hb_active = true;
        s->hb_mono = mono_host;
        s->hb_host_bpp = host_bpp;
        s->hb_dst_bpp = bpp;
        s->hb_byte_align = (host_cntl & ATI_HOST_BYTE_ALIGN) != 0;
        s->hb_bigendian = (host_cntl & ATI_HOST_BIG_ENDIAN_EN) != 0;
        s->hb_lsb_first = (pix_width & ATI_PIX_WIDTH_BYTE_ORDER) != 0;
        s->hb_triple = mono_host && rot24 &&
                       (pix_width & ATI_DP_HOST_TRIPLE_EN);
        s->hb_rot24 = rot24;
        s->hb_x0 = dst_x;
        s->hb_x = dst_x;
        s->hb_y = dst_y;
        s->hb_w = w;
        s->hb_h = h;
        s->hb_dst_base = dst_base;
        s->hb_dst_pitch = dst_pitch;
        s->hb_frgd_clr = s->regs[ATI_DP_FRGD_CLR >> 2];
        s->hb_bkgd_clr = s->regs[ATI_DP_BKGD_CLR >> 2];
        s->hb_frgd_mix = frgd_mix;
        s->hb_bkgd_mix = bkgd_mix;
        trace_ati_mach64_2d_host(dst_x, dst_y, w, h, mono_host, host_bpp,
                                 sc_left, sc_right, sc_top, sc_bottom);
        return;
    }

    if (blit_used && src_pitch == 0) {
        trace_ati_mach64_2d_unimp(dp_src, dp_mix, pix_width);
        return;
    }

    /* Direction bits locate the start corner; normalize to top-left. */
    if (!(dst_cntl & ATI_DST_X_LEFT_TO_RIGHT)) {
        dst_x -= w - 1;
        src_x -= w - 1;
    }
    if (!(dst_cntl & ATI_DST_Y_TOP_TO_BOTTOM)) {
        dst_y -= h - 1;
        src_y -= h - 1;
    }

    /* Rectangular scissors (inclusive bounds). */
    clip = sc_left - dst_x;
    if (clip > 0) {
        dst_x += clip;
        src_x += clip;
        w -= clip;
    }
    clip = sc_top - dst_y;
    if (clip > 0) {
        dst_y += clip;
        src_y += clip;
        h -= clip;
    }
    if (dst_x + w - 1 > sc_right) {
        w = sc_right - dst_x + 1;
    }
    if (dst_y + h - 1 > sc_bottom) {
        h = sc_bottom - dst_y + 1;
    }
    if (w <= 0 || h <= 0 || dst_x < 0 || dst_y < 0 ||
        (blit_used && (src_x < 0 || src_y < 0))) {
        trace_ati_mach64_2d_drop("clipped-away", dst_x, dst_y, w, h,
                                 sc_right, sc_bottom, dp_src, dst_base);
        return;
    }

    /*
     * Bounds: reject anything that could touch outside VRAM. The
     * mono-blit source is 1bpp, so its byte extent is x+w bits.
     */
    if ((uint64_t)dst_base +
        ((uint64_t)(dst_y + h - 1) * dst_pitch + dst_x + w) * bpp >
        ATI_MACH64_VRAM_SIZE ||
        (blit_used &&
         (uint64_t)src_base +
         ((uint64_t)(src_y + h - 1) * src_pitch + src_x + w) *
         (mono_blit ? 1 : bpp) / (mono_blit ? 8 : 1) + (mono_blit ? 1 : 0) >
         ATI_MACH64_VRAM_SIZE)) {
        trace_ati_mach64_2d_drop("vram-bounds", dst_x, dst_y, w, h,
                                 sc_right, sc_bottom, dp_src, dst_base);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ati-mach64: 2D op exceeds VRAM, dropped\n");
        return;
    }

    /*
     * Fast paths for the two dominant trivial cases -- opaque solid
     * fill and straight copy with no compare, full write mask and no
     * mono expansion -- keeping full-screen operations cheap.
     */
    if (mono_one && !mono_pat && frgd_mix == ATI_MIX_S && !rot24 &&
        (s->regs[ATI_CLR_CMP_CNTL >> 2] & ATI_CLR_CMP_FN_MASK) == 0 &&
        s->regs[ATI_DP_WRITE_MSK >> 2] == 0xffffffff) {
        if (eff_frgd_src == ATI_BKGD_SRC_FRGD_CLR ||
            eff_frgd_src == ATI_BKGD_SRC_BKGD_CLR) {
            uint32_t color = s->regs[(eff_frgd_src == ATI_BKGD_SRC_FRGD_CLR ?
                                      ATI_DP_FRGD_CLR : ATI_DP_BKGD_CLR) >> 2];
            uint8_t pix[4];
            int b;

            trace_ati_mach64_2d_fill(dst_x, dst_y, w, h, bpp, color,
                                     frgd_mix, dst_base, dst_pitch);
            for (b = 0; b < bpp; b++) {
                pix[bpp - 1 - b] = (color >> (8 * b)) & 0xff;   /* BE */
            }
            for (y = 0; y < h; y++) {
                uint8_t *row = vram + dst_base +
                    ((uint64_t)(dst_y + y) * dst_pitch + dst_x) * bpp;
                if (bpp == 1) {
                    memset(row, pix[0], w);
                } else {
                    for (x = 0; x < w; x++) {
                        memcpy(row + (uint64_t)x * bpp, pix, bpp);
                    }
                }
            }
            goto done;
        }
        if (eff_frgd_src == ATI_BKGD_SRC_BLIT && !src_wraps) {
            bool reverse_y = (dst_base == src_base) && (dst_y > src_y);

            trace_ati_mach64_2d_copy(src_x, src_y, dst_x, dst_y, w, h, bpp,
                                     src_base, dst_base, dst_pitch);
            for (y = 0; y < h; y++) {
                int ry = reverse_y ? h - 1 - y : y;
                uint8_t *drow = vram + dst_base +
                    ((uint64_t)(dst_y + ry) * dst_pitch + dst_x) * bpp;
                const uint8_t *srow = vram + src_base +
                    ((uint64_t)(src_y + ry) * src_pitch + src_x) * bpp;
                memmove(drow, srow, (size_t)w * bpp);
            }
            goto done;
        }
    }

    /*
     * General path: per pixel, resolve the mono channel, pick the
     * foreground or background source and mix, and push the result
     * through compare/mix/write-mask. A blit source row is staged
     * into a buffer first so overlapping copies stay safe in any
     * direction.
     */
    trace_ati_mach64_2d_fill(dst_x, dst_y, w, h, bpp,
                             s->regs[ATI_DP_FRGD_CLR >> 2], frgd_mix,
                             dst_base, dst_pitch);
    if (blit_used && !mono_blit && !src_wraps) {
        rowbuf = g_malloc((size_t)w * bpp);
    }
    for (y = 0; y < h; y++) {
        int ry = ((dst_base == src_base) && (dst_y > src_y)) ? h - 1 - y : y;
        uint8_t *drow = vram + dst_base +
            ((uint64_t)(dst_y + ry) * dst_pitch + dst_x) * bpp;
        const uint8_t *srow = NULL;

        if (rowbuf) {
            memcpy(rowbuf, vram + src_base +
                   ((uint64_t)(src_y + ry) * src_pitch + src_x) * bpp,
                   (size_t)w * bpp);
            srow = rowbuf;
        }
        for (x = 0; x < w; x++) {
            int ax = dst_x + x;
            int ay = dst_y + ry;
            const uint8_t *sp = srow ? srow + (uint64_t)x * bpp : NULL;
            bool m = true;

            /*
             * Walk the source trajectory when it is not a plain
             * rectangle (see src_wraps above). Out-of-VRAM source
             * pixels read as zero rather than faulting.
             */
            if (blit_used && !mono_blit && src_wraps) {
                uint64_t soff;

                if (src_linear) {
                    soff = src_base + ((uint64_t)ry * w + x) * bpp;
                } else {
                    int sx = src_x + (src_w1 > 0 ? (x + src_xs) % src_w1 : x);
                    int sy = src_y + ((src_patt || src_rot) && src_h1 > 0 ?
                                      (ry + src_ys) % src_h1 : ry);

                    soff = src_base +
                           ((uint64_t)sy * src_pitch + sx) * bpp;
                }
                sp = (soff + bpp <= ATI_MACH64_VRAM_SIZE) ? vram + soff : NULL;
            }

            if (mono_pat) {
                m = ati_mach64_2d_pat_mono(s, rot24 ? ax / 3 : ax, ay);
            } else if (mono_blit) {
                uint32_t bitpos = (uint64_t)(src_y + ry) * src_pitch +
                                  src_x + x;
                uint8_t byte = vram[src_base + (bitpos >> 3)];
                bool lsb = pix_width & ATI_PIX_WIDTH_BYTE_ORDER;

                m = (byte >> (lsb ? (bitpos & 7) : 7 - (bitpos & 7))) & 1;
            }

            ati_mach64_2d_put(s, drow + (uint64_t)x * bpp, bpp,
                              ati_mach64_2d_src_color(s,
                                  m ? eff_frgd_src : bkgd_src,
                                  sp, bpp, ax, ay, rot24),
                              m ? frgd_mix : bkgd_mix,
                              rot24 ? (ax & 3) : 0);
        }
    }
    g_free(rowbuf);

done:
    memory_region_set_dirty(&s->vram,
                            dst_base + (uint64_t)dst_y * dst_pitch * bpp,
                            (uint64_t)h * dst_pitch * bpp);
}

/*
 * Bresenham line engine (RRG-G02700 4-43..4-46), triggered by a
 * DST_BRES_LNTH write: LNTH pixels from (DST_X, DST_Y), the last one
 * omitted unless DST_LAST_PEL. Per pixel: negative error takes an
 * axial step (major axis only, direction per DST_CNTL) and adds
 * DST_BRES_INC; otherwise a diagonal step adds DST_BRES_DEC.
 * DST_BRES_SIGN makes a zero error count as negative. The foreground
 * color/mix draws every pixel (the mono channel is all-ones for
 * lines), through the same compare/mask back end and scissors.
 */
void ati_mach64_2d_line(ATIMach64State *s, uint32_t lnth)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vram);
    uint32_t dp_src = s->regs[ATI_DP_SRC >> 2];
    uint32_t dp_mix = s->regs[ATI_DP_MIX >> 2];
    uint8_t frgd_mix = (dp_mix >> ATI_FRGD_MIX_SHIFT) & ATI_FRGD_MIX_MASK;
    uint32_t dst_cntl = s->regs[ATI_DST_CNTL >> 2];
    uint32_t dst_op = s->regs[ATI_DST_OFF_PITCH >> 2];
    uint32_t pix_width = s->regs[ATI_DP_PIX_WIDTH >> 2];
    int bpp = ati_mach64_2d_bytes_pp(pix_width);
    uint32_t dst_base = (dst_op & 0xfffff) * 8;
    int dst_pitch = ((dst_op >> 22) & 0x3ff) * 8;
    int x = s->regs[ATI_DST_X >> 2] & 0x1fff;
    int y = s->regs[ATI_DST_Y >> 2] & 0x7fff;
    int sc_left = ati_mach64_sc_left(s);
    int sc_right = ati_mach64_sc_right(s);
    int sc_top = ati_mach64_sc_top(s);
    int sc_bottom = ati_mach64_sc_bottom(s);
    int err = sextract32(s->regs[ATI_DST_BRES_ERR >> 2], 0, 18);
    int inc = sextract32(s->regs[ATI_DST_BRES_INC >> 2], 0, 18);
    int dec = sextract32(s->regs[ATI_DST_BRES_DEC >> 2], 0, 18);
    int xd = (dst_cntl & ATI_DST_X_LEFT_TO_RIGHT) ? 1 : -1;
    int yd = (dst_cntl & ATI_DST_Y_TOP_TO_BOTTOM) ? 1 : -1;
    bool ymajor = dst_cntl & ATI_DST_Y_MAJOR;
    uint32_t frgd_src = (dp_src & ATI_FRGD_SRC_MASK) >> 8;
    uint32_t color;
    int pixels = lnth & ATI_BRES_LNTH_MASK;
    int i;

    if (!(dst_cntl & ATI_DST_LAST_PEL)) {
        pixels--;
    }
    if (pixels <= 0) {
        return;
    }
    if (bpp == 0 || dst_pitch == 0 ||
        (dp_src & ATI_MONO_SRC_MASK) != ATI_MONO_SRC_ONE ||
        (frgd_src != ATI_BKGD_SRC_FRGD_CLR &&
         frgd_src != ATI_BKGD_SRC_BKGD_CLR) ||
        (dst_cntl & (ATI_DST_POLYGON_EN | ATI_DST_24_ROT_EN))) {
        trace_ati_mach64_2d_unimp(dp_src, dp_mix, pix_width);
        return;
    }
    color = s->regs[(frgd_src == ATI_BKGD_SRC_FRGD_CLR ?
                     ATI_DP_FRGD_CLR : ATI_DP_BKGD_CLR) >> 2];

    trace_ati_mach64_2d_line(x, y, pixels, err, inc, dec,
                             (int)(dst_cntl & 0xfff));
    for (i = 0; i < pixels; i++) {
        if (x >= sc_left && x <= sc_right && x >= 0 &&
            y >= sc_top && y <= sc_bottom && y >= 0) {
            uint64_t off = dst_base + ((uint64_t)y * dst_pitch + x) * bpp;

            if (off + bpp <= ATI_MACH64_VRAM_SIZE) {
                ati_mach64_2d_put(s, vram + off, bpp, color, frgd_mix, 0);
            }
        }
        if (err < 0 || (err == 0 && (dst_cntl & ATI_DST_BRES_SIGN))) {
            err += inc;                 /* axial: major axis only */
        } else {
            err += dec;                 /* diagonal: minor too */
            if (ymajor) {
                x += xd;
            } else {
                y += yd;
            }
        }
        if (ymajor) {
            y += yd;
        } else {
            x += xd;
        }
    }
    memory_region_set_dirty(&s->vram, 0, ATI_MACH64_VRAM_SIZE);
}

/*
 * DP_SET_GUI_ENGINE2 (LT3REGRE 5-56): the Rage Pro's one-write
 * drawing-context macro. Field layout verified by decoding the live
 * values Apple's accelerated NDRV writes before every operation
 * (e.g. 0xcd430877 = solid 8bpp fill context at pitch 1152 with S/S
 * mixes and SRC_OFF_PITCH copied from DST_OFF_PITCH):
 *   [3:0] bkgd mix, [7:4] frgd mix, [10:8] bkgd src, [13:11] frgd
 *   src, [15:14] mono src, [16] X dir, [17] Y dir, [18] mono-pattern
 *   enable, [19] source pattern rotation, [22] reset DP_WRITE_MSK to
 *   all-ones, [25:23] dst pix width code, [26] src width = dst width,
 *   [30:27] pitch code, [31] copy DST_OFF_PITCH to SRC_OFF_PITCH.
 * A write also zeroes the position registers and CLR_CMP_CNTL and
 * clears the host width / byte-order fields (Table 5-13).
 */
void ati_mach64_set_gui_engine2(ATIMach64State *s, uint32_t w)
{
    static const uint16_t pitch_codes[16] = {
        0, 320, 352, 384, 640, 800, 896, 512,
        1024, 1152, 1280, 400, 832, 1600, 448, 2048,
    };
    uint32_t dst_code = (w >> 23) & 7;
    uint32_t pitch_px = pitch_codes[(w >> 27) & 0xf];
    uint32_t dst_cntl = s->regs[ATI_DST_CNTL >> 2];

    s->regs[ATI_DP_MIX >> 2] = (((w >> 4) & 0xf) << ATI_FRGD_MIX_SHIFT) |
                               (w & 0xf);
    s->regs[ATI_DP_SRC >> 2] = (((w >> 14) & 3) << 16) |
                               (((w >> 11) & 7) << 8) | ((w >> 8) & 7);
    dst_cntl &= ~(ATI_DST_X_LEFT_TO_RIGHT | ATI_DST_Y_TOP_TO_BOTTOM);
    dst_cntl |= (w & (1u << 16)) ? ATI_DST_X_LEFT_TO_RIGHT : 0;
    dst_cntl |= (w & (1u << 17)) ? ATI_DST_Y_TOP_TO_BOTTOM : 0;
    s->regs[ATI_DST_CNTL >> 2] = dst_cntl;
    s->regs[ATI_PAT_CNTL >> 2] = (w & (1u << 18)) ? ATI_PAT_MONO_EN : 0;
    if (w & (1u << 19)) {
        s->regs[ATI_SRC_CNTL >> 2] |= ATI_SRC_ROTATION_EN;
    } else {
        s->regs[ATI_SRC_CNTL >> 2] &= ~ATI_SRC_ROTATION_EN;
    }
    if (w & (1u << 22)) {
        s->regs[ATI_DP_WRITE_MSK >> 2] = 0xffffffff;
    }
    s->regs[ATI_DP_PIX_WIDTH >> 2] =
        (((w & (1u << 26)) ? dst_code : 0) << ATI_PIX_WIDTH_SRC_SHIFT) |
        dst_code;
    if (pitch_px) {
        s->regs[ATI_DST_OFF_PITCH >> 2] =
            (s->regs[ATI_DST_OFF_PITCH >> 2] & 0x3fffff) |
            ((pitch_px / 8) << 22);
    } else if (s->regs[ATI_USR_DST_PITCH >> 2]) {
        s->regs[ATI_DST_OFF_PITCH >> 2] =
            (s->regs[ATI_DST_OFF_PITCH >> 2] & 0x3fffff) |
            ((s->regs[ATI_USR_DST_PITCH >> 2] & 0x3ff) << 22);
    }
    if (w & (1u << 31)) {
        s->regs[ATI_SRC_OFF_PITCH >> 2] = s->regs[ATI_DST_OFF_PITCH >> 2];
    }

    s->regs[ATI_DST_X >> 2] = 0;
    s->regs[ATI_DST_Y >> 2] = 0;
    s->regs[ATI_DST_Y_X >> 2] = 0;
    s->regs[ATI_DST_WIDTH >> 2] = 0;
    s->regs[ATI_DST_HEIGHT >> 2] = 0;
    s->regs[ATI_DST_HEIGHT_WIDTH >> 2] = 0;
    s->regs[ATI_SRC_X >> 2] = 0;
    s->regs[ATI_SRC_Y >> 2] = 0;
    s->regs[ATI_SRC_Y_X >> 2] = 0;
    s->regs[ATI_CLR_CMP_CNTL >> 2] = 0;
    trace_ati_mach64_2d_set_engine(w, s->regs[ATI_DP_SRC >> 2],
                                   s->regs[ATI_DP_MIX >> 2],
                                   s->regs[ATI_DP_PIX_WIDTH >> 2]);
}

/*
 * 3D setup-engine trigger scaffolding: no rasterizer yet -- this
 * traces the complete decoded vertex/state snapshot every time a
 * guest driver fires a triangle (ONE_OVER_AREA write after loading
 * vertex state), so a live capture against ATI's own classic-Mac RAVE
 * driver can pin down the operand fixed-point formats before the
 * rasterizer is written. Loud by design.
 */
void ati_mach64_3d_trigger(ATIMach64State *s, uint32_t one_over_area)
{
    trace_ati_mach64_3d_tri(one_over_area,
                            s->regs[ATI_SETUP_CNTL >> 2],
                            s->regs[ATI_SCALE_3D_CNTL >> 2],
                            s->regs[ATI_Z_CNTL >> 2],
                            s->regs[ATI_VERTEX_1_X_Y >> 2],
                            s->regs[ATI_VERTEX_2_X_Y >> 2],
                            s->regs[ATI_VERTEX_3_X_Y >> 2]);
    trace_ati_mach64_3d_vtx(1, s->regs[ATI_VERTEX_1_ARGB >> 2],
                            s->regs[ATI_VERTEX_1_Z >> 2],
                            s->regs[ATI_VERTEX_1_S >> 2],
                            s->regs[ATI_VERTEX_1_T >> 2],
                            s->regs[ATI_VERTEX_1_W >> 2]);
    trace_ati_mach64_3d_vtx(2, s->regs[ATI_VERTEX_2_ARGB >> 2],
                            s->regs[ATI_VERTEX_2_Z >> 2],
                            s->regs[ATI_VERTEX_2_S >> 2],
                            s->regs[ATI_VERTEX_2_T >> 2],
                            s->regs[ATI_VERTEX_2_W >> 2]);
    trace_ati_mach64_3d_vtx(3, s->regs[ATI_VERTEX_3_ARGB >> 2],
                            s->regs[ATI_VERTEX_3_Z >> 2],
                            s->regs[ATI_VERTEX_3_S >> 2],
                            s->regs[ATI_VERTEX_3_T >> 2],
                            s->regs[ATI_VERTEX_3_W >> 2]);
}
