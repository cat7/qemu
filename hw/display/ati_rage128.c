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

#include "ati_rage128.h"
#include "ati_rage128_regs.h"
#include "trace.h"

#define ATI_RAGE128_VBLANK_PERIOD_NS (NANOSECONDS_PER_SECOND / 60)
#define ATI_RAGE128_VBLANK_LEN_NS    (ATI_RAGE128_VBLANK_PERIOD_NS / 8)

/* ---------------------------------------------------------------- */
/* Register names for tracing                                       */

static const char *ati_rage128_reg_name(uint32_t base)
{
    switch (base) {
    case R128_MM_INDEX:              return "MM_INDEX";
    case R128_MM_DATA:               return "MM_DATA";
    case R128_CLOCK_CNTL_INDEX:      return "CLOCK_CNTL_INDEX";
    case R128_CLOCK_CNTL_DATA:       return "CLOCK_CNTL_DATA";
    case R128_BIOS_0_SCRATCH:        return "BIOS_0_SCRATCH";
    case R128_BIOS_1_SCRATCH:        return "BIOS_1_SCRATCH";
    case R128_BIOS_2_SCRATCH:        return "BIOS_2_SCRATCH";
    case R128_BIOS_3_SCRATCH:        return "BIOS_3_SCRATCH";
    case R128_BUS_CNTL:              return "BUS_CNTL";
    case R128_BUS_CNTL1:             return "BUS_CNTL1";
    case R128_GEN_INT_CNTL:          return "GEN_INT_CNTL";
    case R128_GEN_INT_STATUS:        return "GEN_INT_STATUS";
    case R128_CRTC_GEN_CNTL:         return "CRTC_GEN_CNTL";
    case R128_CRTC_EXT_CNTL:         return "CRTC_EXT_CNTL";
    case R128_DAC_CNTL:              return "DAC_CNTL";
    case R128_CRTC_STATUS:           return "CRTC_STATUS";
    case R128_GPIO_MONID:            return "GPIO_MONID";
    case R128_SEPROM_CNTL:           return "SEPROM_CNTL";
    case R128_I2C_CNTL_0:            return "I2C_CNTL_0";
    case R128_I2C_CNTL_1:            return "I2C_CNTL_1";
    case R128_I2C_DATA:              return "I2C_DATA";
    case R128_AMCGPIO_MASK_MIR:      return "AMCGPIO_MASK_MIR";
    case R128_AMCGPIO_A_MIR:         return "AMCGPIO_A_MIR";
    case R128_AMCGPIO_Y_MIR:         return "AMCGPIO_Y_MIR";
    case R128_AMCGPIO_EN_MIR:        return "AMCGPIO_EN_MIR";
    case R128_PALETTE_INDEX:         return "PALETTE_INDEX";
    case R128_PALETTE_DATA:          return "PALETTE_DATA";
    case R128_CONFIG_CNTL:           return "CONFIG_CNTL";
    case R128_CONFIG_XSTRAP:         return "CONFIG_XSTRAP";
    case R128_CONFIG_BONDS:          return "CONFIG_BONDS";
    case R128_GEN_RESET_CNTL:        return "GEN_RESET_CNTL";
    case R128_GEN_STATUS:            return "GEN_STATUS";
    case R128_CONFIG_MEMSIZE:        return "CONFIG_MEMSIZE";
    case R128_CONFIG_APER_0_BASE:    return "CONFIG_APER_0_BASE";
    case R128_CONFIG_APER_1_BASE:    return "CONFIG_APER_1_BASE";
    case R128_CONFIG_APER_SIZE:      return "CONFIG_APER_SIZE";
    case R128_CONFIG_REG_1_BASE:     return "CONFIG_REG_1_BASE";
    case R128_CONFIG_REG_APER_SIZE:  return "CONFIG_REG_APER_SIZE";
    case R128_HOST_PATH_CNTL:        return "HOST_PATH_CNTL";
    case R128_SW_SEMAPHORE:          return "SW_SEMAPHORE";
    case R128_MEM_CNTL:              return "MEM_CNTL";
    case R128_EXT_MEM_CNTL:          return "EXT_MEM_CNTL";
    case R128_MEM_ADDR_CONFIG:       return "MEM_ADDR_CONFIG";
    case R128_MEM_INTF_CNTL:         return "MEM_INTF_CNTL";
    case R128_MEM_STR_CNTL:          return "MEM_STR_CNTL";
    case R128_MEM_INIT_LAT_TIMER:    return "MEM_INIT_LAT_TIMER";
    case R128_MEM_SDRAM_MODE_REG:    return "MEM_SDRAM_MODE_REG";
    case R128_AGP_BASE:              return "AGP_BASE";
    case R128_AGP_CNTL:              return "AGP_CNTL";
    case R128_AGP_APER_OFFSET:       return "AGP_APER_OFFSET";
    case R128_PCI_GART_PAGE:         return "PCI_GART_PAGE";
    case R128_PC_NGUI_MODE:          return "PC_NGUI_MODE";
    case R128_PC_NGUI_CTLSTAT:       return "PC_NGUI_CTLSTAT";
    case R128_CRTC_H_TOTAL_DISP:     return "CRTC_H_TOTAL_DISP";
    case R128_CRTC_H_SYNC_STRT_WID:  return "CRTC_H_SYNC_STRT_WID";
    case R128_CRTC_V_TOTAL_DISP:     return "CRTC_V_TOTAL_DISP";
    case R128_CRTC_V_SYNC_STRT_WID:  return "CRTC_V_SYNC_STRT_WID";
    case R128_CRTC_VLINE_CRNT_VLINE: return "CRTC_VLINE_CRNT_VLINE";
    case R128_CRTC_CRNT_FRAME:       return "CRTC_CRNT_FRAME";
    case R128_CRTC_GUI_TRIG_VLINE:   return "CRTC_GUI_TRIG_VLINE";
    case R128_CRTC_OFFSET:           return "CRTC_OFFSET";
    case R128_CRTC_OFFSET_CNTL:      return "CRTC_OFFSET_CNTL";
    case R128_CRTC_PITCH:            return "CRTC_PITCH";
    case R128_OVR_CLR:               return "OVR_CLR";
    case R128_OVR_WID_LEFT_RIGHT:    return "OVR_WID_LEFT_RIGHT";
    case R128_OVR_WID_TOP_BOTTOM:    return "OVR_WID_TOP_BOTTOM";
    case R128_CUR_OFFSET:            return "CUR_OFFSET";
    case R128_CUR_HORZ_VERT_POSN:    return "CUR_HORZ_VERT_POSN";
    case R128_CUR_HORZ_VERT_OFF:     return "CUR_HORZ_VERT_OFF";
    case R128_CUR_CLR0:              return "CUR_CLR0";
    case R128_CUR_CLR1:              return "CUR_CLR1";
    case R128_DAC_EXT_CNTL:          return "DAC_EXT_CNTL";
    case R128_DDA_CONFIG:            return "DDA_CONFIG";
    case R128_DDA_ON_OFF:            return "DDA_ON_OFF";
    case R128_GUI_DEBUG0:            return "GUI_DEBUG0";
    case R128_WAIT_UNTIL:            return "WAIT_UNTIL";
    case R128_GUI_STAT:              return "GUI_STAT";
    default:                         return "?";
    }
}

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
 * All drawing converts through an allocated 32bpp surface: 8bpp is
 * palettized, and the direct-color modes hold big-endian pixels (Mac
 * guest; same reasoning as ati_mach64.c's draw helpers, which these
 * follow). Rage 128 adds the per-aperture byte-swap configured in
 * CONFIG_CNTL -- APER_0_ENDIAN swaps CPU *accesses* on their way to
 * VRAM, so a Mac driver using a swapped aperture still produces
 * little-endian bytes in VRAM. The swap mode therefore selects how we
 * decode VRAM bytes here. Until a guest is seen actually enabling the
 * swappers (trace ati_rage128_aper_endian), VRAM bytes are decoded
 * big-endian like every other classic Mac framebuffer.
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
            /* big-endian X,R,G,B in VRAM */
            dst[x] = 0xff000000u | ((uint32_t)src[4 * x + 1] << 16) |
                     ((uint32_t)src[4 * x + 2] << 8) | src[4 * x + 3];
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
            dst[x] = 0xff000000u | ((uint32_t)src[3 * x] << 16) |
                     ((uint32_t)src[3 * x + 1] << 8) | src[3 * x + 2];
        }
        src += mode->pitch;
    }
}

static bool ati_rage128_update_display(void *opaque)
{
    ATIRage128State *s = opaque;
    ATIRage128Mode mode;
    DisplaySurface *ds;
    uint32_t pix_width;

    ati_rage128_get_mode(s, &mode);
    trace_ati_rage128_update(mode.width, mode.height, mode.bpp,
                             ati_rage128_mode_valid(s, &mode),
                             mode.fb_offset);
    if (!ati_rage128_mode_valid(s, &mode)) {
        return true;
    }
    pix_width = (s->regs[R128_CRTC_GEN_CNTL >> 2] >>
                 R128_CRTC_PIX_WIDTH_SHIFT) & R128_CRTC_PIX_WIDTH_MASK;

    if (memcmp(&s->mode, &mode, sizeof(mode)) != 0 || s->mode_dirty) {
        s->mode = mode;
        s->mode_dirty = false;
        ds = qemu_create_displaysurface(mode.width, mode.height);
        qemu_console_set_surface(s->con, ds);
    }
    ds = qemu_console_surface(s->con);
    switch (pix_width) {
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
    case R128_CFG_MIRROR_BASE ... R128_CFG_MIRROR_END:
        val = pci_default_read_config(dev, base - R128_CFG_MIRROR_BASE, 4);
        break;
    default:
        break;
    }
    return val;
}

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
    case R128_CRTC_EXT_CNTL:
    case R128_CRTC_H_TOTAL_DISP:
    case R128_CRTC_V_TOTAL_DISP:
    case R128_CRTC_PITCH:
        s->regs[base >> 2] = val;
        s->mode_dirty = true;
        trace_ati_rage128_mode_reg(ati_rage128_reg_name(base), val);
        break;
    case R128_CRTC_OFFSET:
        s->regs[base >> 2] = val & (R128_CRTC_OFFSET_MASK |
                                    R128_CRTC_OFFSET_LOCK);
        s->mode_dirty = true;
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
        /* engine soft reset: accept and report done by storing 0 */
        s->regs[base >> 2] = val;
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
     * image can be given its own endian-swap mode via CONFIG_CNTL;
     * the swappers themselves are not modeled yet (traced instead) so
     * both halves currently alias the same raw VRAM bytes.
     */
    memory_region_init(&s->aper, obj, "ati-rage128-aper",
                       ATI_RAGE128_APER_SIZE);
    memory_region_init_ram(&s->vram, obj, "ati-rage128-vram",
                           ATI_RAGE128_VRAM_SIZE, &error_fatal);
    memory_region_add_subregion(&s->aper, 0, &s->vram);
    memory_region_init_alias(&s->vram_aper1, obj, "ati-rage128-aper1",
                             &s->vram, 0, ATI_RAGE128_VRAM_SIZE);
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

    qemu_edid_generate(s->edid, sizeof(s->edid), &s->edid_info);
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
