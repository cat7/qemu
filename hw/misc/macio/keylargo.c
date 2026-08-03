/*
 * KeyLargo MacIO cells that the New World machine did not previously model:
 * the Feature Control Registers, the two I2S sound cells and the "Keywest"
 * I2C controller.
 *
 * OpenBIOS never touches any of these, which is why mac99 got by without
 * them. A real Apple boot ROM drives all three during early hardware
 * bring-up and spins forever if they do not answer.
 *
 * Register layout and bit meanings come from Linux
 * (arch/powerpc/include/asm/keylargo.h, sound/aoa/soundbus/i2sbus/,
 * arch/powerpc/platforms/powermac/low_i2c.c), Apple's own AppleOnboardAudio
 * sources (AudioI2SHardwareConstants.h, I2STransportInterface.cpp) and the
 * OpenBSD/NetBSD macppc i2s drivers.
 *
 * All KeyLargo registers are little-endian: Linux uses in_le32()/out_le32(),
 * Apple's driver wraps every access in OSRead/WriteLittleInt32(), and the
 * Apple ROM reaches them with lwbrx/stwbrx.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/macio/macio.h"
#include "hw/misc/macio/keylargo.h"
#include "hw/i2c/i2c.h"
#include "trace.h"

/* ---------------------------------------------------------------------- */
/* Feature Control Registers                                              */
/* ---------------------------------------------------------------------- */

/*
 * MBCR at 0x34, then FCR0..FCR5 at 0x38..0x4c. The Apple ROM writes every
 * one of them before reading any back, so plain storage is faithful enough;
 * what matters is that FCR1's I2S clock-gating bits are visible to the I2S
 * cells below.
 */
#define KEYLARGO_FCR_BASE       0x34
#define KEYLARGO_FCR_SIZE       0x1c

static uint64_t keylargo_fcr_read(void *opaque, hwaddr addr, unsigned size)
{
    KeyLargoState *s = opaque;
    uint32_t val = s->fcr[addr >> 2];

    trace_keylargo_fcr_read(KEYLARGO_FCR_BASE + addr, val);
    return val;
}

static void keylargo_fcr_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    KeyLargoState *s = opaque;

    trace_keylargo_fcr_write(KEYLARGO_FCR_BASE + addr, (uint32_t)value);
    s->fcr[addr >> 2] = value;

    /*
     * Gating an I2S cell's clock is what makes its "clocks stopped" status
     * latch fire; firmware polls for that before reprogramming the cell.
     */
    keylargo_i2s_update_clocks(s);
}

static const MemoryRegionOps keylargo_fcr_ops = {
    .read = keylargo_fcr_read,
    .write = keylargo_fcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* I2S cells                                                              */
/* ---------------------------------------------------------------------- */

/*
 * An I2S cell is considered to be running only when its FCR1 bits say the
 * cell is enabled, not held in software reset, and its clock is un-gated.
 * The moment that stops being true the "clocks stopped" interrupt latches,
 * and it stays latched until firmware writes a 1 to it.
 *
 * This is the handshake the Apple ROM performs: pulse I2S0's reset bit, wait
 * for clocks-stopped, then reprogram the serial format. Nothing in this
 * register reports "cell ready" -- getting that backwards means firmware
 * waits forever.
 */
static bool keylargo_i2s_clocks_running(KeyLargoState *s, int cell)
{
    uint32_t fcr1 = s->fcr[(0x3c - KEYLARGO_FCR_BASE) >> 2];
    uint32_t enable, reset, clk;

    if (cell == 0) {
        enable = KL1_I2S0_CELL_ENABLE;
        reset = KL1_I2S0_RESET;
        clk = KL1_I2S0_CLK_ENABLE_BIT;
    } else {
        enable = KL1_I2S1_CELL_ENABLE;
        reset = KL1_I2S1_RESET;
        clk = KL1_I2S1_CLK_ENABLE_BIT;
    }

    return (fcr1 & enable) && (fcr1 & clk) && !(fcr1 & reset);
}

void keylargo_i2s_update_clocks(KeyLargoState *s)
{
    int cell;

    for (cell = 0; cell < 2; cell++) {
        if (!keylargo_i2s_clocks_running(s, cell)) {
            s->i2s[cell].intr_ctl |= I2S_INT_CLOCKS_STOPPED_PENDING;
        }
    }
}

static uint64_t keylargo_i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    KeyLargoI2SState *c = opaque;
    uint32_t val;

    switch (addr) {
    case I2S_REG_INT_CTL:
        val = c->intr_ctl;
        break;
    case I2S_REG_SERIAL_FORMAT:
        val = c->serial_format;
        break;
    case I2S_REG_DATA_WORD_SIZES:
        val = c->data_word_sizes;
        break;
    case I2S_REG_FRAME_COUNT:
        val = c->frame_count;
        break;
    default:
        val = 0;
        break;
    }

    trace_keylargo_i2s_read(c->cell, addr, val);
    return val;
}

static void keylargo_i2s_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    KeyLargoI2SState *c = opaque;

    trace_keylargo_i2s_write(c->cell, addr, (uint32_t)value);

    switch (addr) {
    case I2S_REG_INT_CTL:
        /* Pending bits are write-1-to-clear; enable bits are read/write. */
        c->intr_ctl &= ~(value & I2S_INT_PENDING_MASK);
        c->intr_ctl = (c->intr_ctl & I2S_INT_PENDING_MASK) |
                      (value & ~I2S_INT_PENDING_MASK);
        break;
    case I2S_REG_SERIAL_FORMAT:
        c->serial_format = value;
        break;
    case I2S_REG_DATA_WORD_SIZES:
        c->data_word_sizes = value;
        break;
    case I2S_REG_FRAME_COUNT:
        c->frame_count = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps keylargo_i2s_ops = {
    .read = keylargo_i2s_read,
    .write = keylargo_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* "Keywest" I2C controller                                               */
/* ---------------------------------------------------------------------- */

/*
 * Registers sit on a 16-byte stride and are accessed as bytes (the Apple ROM
 * uses lbz/stb). The transfer modes mirror Linux's low_i2c.c: "dumb" drives
 * the bus by hand, "standard" does address + data, "standard sub" inserts a
 * sub-address byte, and "combined" does a write of the sub-address followed
 * by a repeated start for the read.
 */
static void keywest_i2c_set_irq(KeyLargoI2CState *c, uint8_t bits)
{
    c->isr |= bits;
}

/* Drop the bus without disturbing the control shadow. */
static void keywest_i2c_abort(KeyLargoI2CState *c)
{
    if (c->xfer_active) {
        i2c_end_transfer(c->bus);
        c->xfer_active = false;
    }
    c->status &= ~KW_I2C_STAT_BUSY;
}

static void keywest_i2c_stop(KeyLargoI2CState *c)
{
    keywest_i2c_abort(c);
    /* Let the next transfer's XADDR register as a rising edge again. */
    c->control = 0;
}

/*
 * Start the addressing phase. Returns true if a device acknowledged.
 *
 * With nothing on the bus -- which is the stock Tangent configuration, where
 * KeyLargo's I2C only reaches the modem and CardBus slots -- this correctly
 * reports a NAK rather than pretending a device answered.
 */
static bool keywest_i2c_start(KeyLargoI2CState *c)
{
    int mode = c->mode & KW_I2C_MODE_MODE_MASK;
    bool recv = c->addr & 1;
    bool ack;

    keywest_i2c_abort(c);

    /*
     * A combined transfer addresses the device for writing first so the
     * sub-address can be sent, then repeats the start for the read.
     */
    ack = i2c_start_transfer(c->bus, c->addr >> 1,
                             (mode == KW_I2C_MODE_COMBINED) ? false : recv) == 0;
    if (!ack) {
        i2c_end_transfer(c->bus);
        return false;
    }
    c->xfer_active = true;
    c->status |= KW_I2C_STAT_BUSY;

    if (mode == KW_I2C_MODE_STANDARDSUB || mode == KW_I2C_MODE_COMBINED) {
        if (i2c_send(c->bus, c->subaddr) < 0) {
            i2c_end_transfer(c->bus);
            c->xfer_active = false;
            return false;
        }
    }

    if (mode == KW_I2C_MODE_COMBINED && recv) {
        if (i2c_start_transfer(c->bus, c->addr >> 1, true) != 0) {
            i2c_end_transfer(c->bus);
            c->xfer_active = false;
            return false;
        }
    }

    return true;
}

static uint64_t keywest_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    KeyLargoI2CState *c = opaque;
    uint32_t val = 0;

    switch (addr & ~0xfULL) {
    case KW_I2C_REG_MODE:
        val = c->mode;
        break;
    case KW_I2C_REG_CONTROL:
        val = c->control;
        break;
    case KW_I2C_REG_STATUS:
        /* SDA/SCL read back high whenever the bus is idle. */
        val = c->status | KW_I2C_STAT_SDA | KW_I2C_STAT_SCL;
        break;
    case KW_I2C_REG_ISR:
        val = c->isr;
        break;
    case KW_I2C_REG_IER:
        val = c->ier;
        break;
    case KW_I2C_REG_ADDR:
        val = c->addr;
        break;
    case KW_I2C_REG_SUBADDR:
        val = c->subaddr;
        break;
    case KW_I2C_REG_DATA:
        /* Already clocked in; the next one is fetched on the ack below. */
        val = c->data;
        break;
    default:
        break;
    }

    trace_keylargo_i2c_read(c->name, addr & ~0xfULL, val);
    return val;
}

static void keywest_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    KeyLargoI2CState *c = opaque;
    uint8_t val = value;

    trace_keylargo_i2c_write(c->name, addr & ~0xfULL, val);

    switch (addr & ~0xfULL) {
    case KW_I2C_REG_MODE:
        c->mode = val;
        break;

    case KW_I2C_REG_CONTROL: {
        /*
         * XADDR and STOP are edge-triggered commands, not levels. Drivers
         * read-modify-write this register mid-transfer -- both Linux and the
         * Apple ROM OR in the AAK bit while XADDR is still set -- so acting
         * on the level would restart the addressing phase every time.
         * keywest_i2c_stop() clears the shadow so the next transfer's XADDR
         * is an edge again.
         */
        uint8_t rising = val & ~c->control;

        c->control = val;

        if (rising & KW_I2C_CTL_XADDR) {
            bool ack = keywest_i2c_start(c);

            if (ack) {
                c->status |= KW_I2C_STAT_LAST_AAK;
            } else {
                c->status &= ~KW_I2C_STAT_LAST_AAK;
            }
            if (c->addr & 1) {
                c->status |= KW_I2C_STAT_LAST_RW;
            } else {
                c->status &= ~KW_I2C_STAT_LAST_RW;
            }
            /*
             * A read transfer starts clocking the first byte in as soon as
             * the device has acknowledged its address, so the data interrupt
             * is already pending by the time the driver looks.
             */
            if (ack && (c->addr & 1)) {
                c->data = i2c_recv(c->bus);
            }
            keywest_i2c_set_irq(c, KW_I2C_IRQ_ADDR |
                                   (ack ? KW_I2C_IRQ_DATA : 0));

            /*
             * On a NAK the controller aborts the transfer by itself and
             * puts a stop condition on the bus. Firmware relies on this:
             * the Apple ROM's error path clears the address interrupt and
             * then waits for the stop interrupt without ever asking for
             * one. (Linux writes an explicit STOP as well, which is
             * harmlessly redundant.)
             */
            if (!ack) {
                keywest_i2c_stop(c);
                keywest_i2c_set_irq(c, KW_I2C_IRQ_STOP);
            }
        }

        if (rising & KW_I2C_CTL_STOP) {
            keywest_i2c_stop(c);
            keywest_i2c_set_irq(c, KW_I2C_IRQ_STOP);
        }
        break;
    }

    case KW_I2C_REG_STATUS:
        c->status = val;
        break;

    case KW_I2C_REG_ISR:
        /* Write-1-to-clear. */
        c->isr &= ~(val & KW_I2C_IRQ_MASK);

        /*
         * Acknowledging the data interrupt is what lets the byte engine run
         * again, so the controller is ready (or the next read byte has
         * landed) essentially immediately afterwards. Raising it during the
         * data-register access instead would be wrong: the driver clears the
         * interrupt *after* touching the data register, which would throw
         * the fresh notification away and stall the transfer.
         */
        if ((val & KW_I2C_IRQ_DATA) && c->xfer_active) {
            if ((c->mode & KW_I2C_MODE_MODE_MASK) == KW_I2C_MODE_COMBINED &&
                (c->addr & 1)) {
                /*
                 * Combined mode is the register-read form: sub-address write,
                 * repeated start, one data byte, stop. The controller closes
                 * the transfer out by itself once the driver has taken that
                 * byte -- the Apple ROM reads it and then waits for the stop
                 * interrupt without ever writing a STOP of its own.
                 */
                keywest_i2c_stop(c);
                keywest_i2c_set_irq(c, KW_I2C_IRQ_STOP);
            } else {
                if (c->addr & 1) {
                    c->data = i2c_recv(c->bus);
                    c->status |= KW_I2C_STAT_LAST_AAK;
                }
                keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA);
            }
        }
        break;

    case KW_I2C_REG_IER:
        c->ier = val;
        break;

    case KW_I2C_REG_ADDR:
        c->addr = val;
        break;

    case KW_I2C_REG_SUBADDR:
        c->subaddr = val;
        break;

    case KW_I2C_REG_DATA:
        c->data = val;
        if (c->xfer_active && !(c->addr & 1) && i2c_send(c->bus, val) == 0) {
            c->status |= KW_I2C_STAT_LAST_AAK;
            keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA);
        } else {
            /* Same auto-abort as above, for a byte that goes unacknowledged. */
            c->status &= ~KW_I2C_STAT_LAST_AAK;
            keywest_i2c_stop(c);
            keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA | KW_I2C_IRQ_STOP);
        }
        break;

    default:
        break;
    }
}

static const MemoryRegionOps keywest_i2c_ops = {
    .read = keywest_i2c_read,
    .write = keywest_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */

KeyLargoState *keylargo_cells_init(DeviceState *owner, MemoryRegion *bar)
{
    KeyLargoState *s = g_new0(KeyLargoState, 1);
    int cell;

    memory_region_init_io(&s->fcr_mem, OBJECT(owner), &keylargo_fcr_ops, s,
                          "keylargo-fcr", KEYLARGO_FCR_SIZE);
    memory_region_add_subregion(bar, KEYLARGO_FCR_BASE, &s->fcr_mem);

    for (cell = 0; cell < 2; cell++) {
        g_autofree char *name = g_strdup_printf("keylargo-i2s%d", cell);

        s->i2s[cell].cell = cell;
        memory_region_init_io(&s->i2s[cell].mem, OBJECT(owner),
                              &keylargo_i2s_ops, &s->i2s[cell], name,
                              KEYLARGO_I2S_SIZE);
        memory_region_add_subregion(bar, KEYLARGO_I2S0_BASE +
                                    cell * KEYLARGO_I2S_STRIDE,
                                    &s->i2s[cell].mem);
    }

    /* Both cells come out of reset with their clocks stopped. */
    keylargo_i2s_update_clocks(s);

    keywest_i2c_init(&s->i2c, owner, "keylargo-i2c", KEYLARGO_I2C_SIZE);
    memory_region_add_subregion(bar, KEYLARGO_I2C_BASE, &s->i2c.mem);

    return s;
}

/*
 * UniNorth carries a second, identical Keywest cell at 0xf8001000, on the
 * bus that reaches the DIMM SPD EEPROMs and the processor module's
 * configuration EEPROM. Same registers, same 16-byte stride -- firmware
 * addresses it at +3 within each slot, which the register decode ignores.
 */
void keywest_i2c_init(KeyLargoI2CState *c, DeviceState *owner,
                      const char *name, uint64_t size)
{
    g_autofree char *busname = g_strdup_printf("%s.bus", name);

    c->name = name;
    c->bus = i2c_init_bus(owner, busname);
    memory_region_init_io(&c->mem, OBJECT(owner), &keywest_i2c_ops, c,
                          name, size);
}
