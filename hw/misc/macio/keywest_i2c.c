/*
 * Apple "KeyWest" I2C controller cell, and the Pulsar clock chip of the
 * PowerMac7,3 whose registers Mac OS X writes to freeze the timebase
 * while starting a second CPU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/macio/keywest_i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

/*
 * Registers sit on a 16-byte stride and are accessed as bytes (the Apple ROM
 * uses lbz/stb). The transfer modes mirror Linux's low_i2c.c: "dumb" drives
 * the bus by hand, "standard" does address + data, "standard sub" inserts a
 * sub-address byte, and "combined" does a write of the sub-address followed
 * by a repeated start for the read.
 */
static void keywest_i2c_update_irq(KeyWestI2CState *c)
{
    if (c->irq) {
        qemu_set_irq(c->irq, (c->isr & c->ier & KW_I2C_IRQ_MASK) != 0);
    }
}

/* System reset: idle, nothing pending or enabled, line low */
void keywest_i2c_reset(KeyWestI2CState *c)
{
    c->xfer_active = false;
    c->manual_addr_pending = false;
    c->manual_byte_delivered = false;
    c->read_pending = false;
    c->mode = 0;
    c->control = 0;
    c->status = 0;
    c->isr = 0;
    c->ier = 0;
    c->addr = 0;
    c->subaddr = 0;
    c->data = 0;
    keywest_i2c_update_irq(c);
}

static void keywest_i2c_set_irq(KeyWestI2CState *c, uint8_t bits)
{
    c->isr |= bits;
    keywest_i2c_update_irq(c);
}

/* Drop the bus without disturbing the control shadow. */
static void keywest_i2c_abort(KeyWestI2CState *c)
{
    if (c->xfer_active) {
        i2c_end_transfer(c->bus);
        c->xfer_active = false;
    }
    c->status &= ~KW_I2C_STAT_BUSY;
    c->manual_addr_pending = false;
    c->manual_byte_delivered = false;
}

static void keywest_i2c_stop(KeyWestI2CState *c)
{
    keywest_i2c_abort(c);
    /* Let the next transfer's XADDR register as a rising edge again. */
    c->control = 0;
}

/* Start the addressing phase. Returns true if a device acknowledged. */
static bool keywest_i2c_start(KeyWestI2CState *c)
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
    KeyWestI2CState *c = opaque;
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

    trace_keywest_i2c_read(c->name, addr & ~0xfULL, val);
    return val;
}

static void keywest_i2c_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    KeyWestI2CState *c = opaque;
    uint8_t val = value;

    trace_keywest_i2c_write(c->name, addr & ~0xfULL, val);

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
             * Fetch the data byte and raise IRQ_DATA only when the driver
             * acks IRQ_ADDR, as the hardware clocks it after the address
             * phase; see read_pending.
             */
            c->read_pending = ack && (c->addr & 1);
            keywest_i2c_set_irq(c, KW_I2C_IRQ_ADDR);

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

        /*
         * DUMB mode's START: a bare start condition, address unknown to the
         * controller yet (real hardware calls this "manual" mode -- Linux's
         * kw_i2c_xfer() refuses it outright, it's an Apple-firmware-only
         * path). Unlike XADDR there is no ADDR/SUBADDR to address the bus
         * with here, so keywest_i2c_start() doesn't apply; just open the
         * transfer and wait for the driver to hand the address byte to
         * KW_I2C_REG_DATA itself, the same way every later byte of a manual
         * transfer already arrives.
         */
        if ((rising & KW_I2C_CTL_START) &&
            (c->mode & KW_I2C_MODE_MODE_MASK) == KW_I2C_MODE_DUMB) {
            keywest_i2c_abort(c);
            c->manual_addr_pending = true;
            keywest_i2c_set_irq(c, KW_I2C_IRQ_START);
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
        keywest_i2c_update_irq(c);

        /*
         * The driver acking IRQ_ADDR is what lets the deferred data byte
         * land (see read_pending's doc comment) -- fetch it and raise
         * IRQ_DATA as a genuinely new event now, not before, so the driver
         * never observes ADDR and DATA set simultaneously on its very
         * first poll.
         */
        if ((val & KW_I2C_IRQ_ADDR) && c->xfer_active && c->read_pending) {
            c->read_pending = false;
            c->data = i2c_recv(c->bus);
            c->status |= KW_I2C_STAT_LAST_AAK;
            keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA);
        }

        /*
         * Acknowledging the data interrupt is what lets the byte engine run
         * again, so the controller is ready (or the next read byte has
         * landed) essentially immediately afterwards. Raising it during the
         * data-register access instead would be wrong: the driver clears the
         * interrupt *after* touching the data register, which would throw
         * the fresh notification away and stall the transfer.
         */
        if ((val & KW_I2C_IRQ_DATA) && c->xfer_active) {
            int mode = c->mode & KW_I2C_MODE_MODE_MASK;
            bool manual_read = mode == KW_I2C_MODE_DUMB && (c->addr & 1);

            if ((mode == KW_I2C_MODE_COMBINED && (c->addr & 1)) ||
                (mode == KW_I2C_MODE_STANDARD && (c->addr & 1)) ||
                (manual_read && c->manual_byte_delivered)) {
                /*
                 * Combined mode is the register-read form: sub-address write,
                 * repeated start, one data byte, stop. The controller closes
                 * the transfer out by itself once the driver has taken that
                 * byte -- the Apple ROM reads it and then waits for the stop
                 * interrupt without ever writing a STOP of its own.
                 *
                 * Plain STANDARD mode never sends a sub-address at all (see
                 * keywest_i2c_start()) -- it's a bare "current address read"
                 * of whatever byte the target device's own internal pointer
                 * is sitting on. The PowerMac3,6 ROM's ADM1030 probe
                 * addresses for read with mode=STANDARD and never writes a
                 * STOP itself either, so without this the
                 * transfer free-runs forever, reading device memory
                 * sequentially -- same one-byte-then-stop contract as
                 * COMBINED's read leg, just without the write leg first.
                 *
                 * DUMB (manual) mode reads do the same, just one ack later: the address-ack's IRQ_DATA is "empty" (see
                 * the DATA-register case), so it's the driver's *second* ack
                 * -- after actually consuming the byte this first ack
                 * delivers below -- that should auto-stop.
                 */
                keywest_i2c_stop(c);
                keywest_i2c_set_irq(c, KW_I2C_IRQ_STOP);
            } else {
                if (c->addr & 1) {
                    c->data = i2c_recv(c->bus);
                    c->status |= KW_I2C_STAT_LAST_AAK;
                }
                if (manual_read) {
                    c->manual_byte_delivered = true;
                }
                keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA);
            }
        }
        break;

    case KW_I2C_REG_IER:
        c->ier = val;
        keywest_i2c_update_irq(c);
        break;

    case KW_I2C_REG_ADDR:
        c->addr = val;
        break;

    case KW_I2C_REG_SUBADDR:
        c->subaddr = val;
        break;

    case KW_I2C_REG_DATA:
        c->data = val;
        if (c->manual_addr_pending) {
            /*
             * The byte the driver hands over right after START is the
             * manual-mode address+R/W byte, not payload -- open the bus
             * with it now, the same ack/nak and auto-abort-on-NAK contract
             * as every other addressing path here.
             */
            c->manual_addr_pending = false;
            c->addr = val;
            if (i2c_start_transfer(c->bus, val >> 1, val & 1) == 0) {
                c->xfer_active = true;
                c->status |= KW_I2C_STAT_BUSY | KW_I2C_STAT_LAST_AAK;
                if (val & 1) {
                    c->status |= KW_I2C_STAT_LAST_RW;
                } else {
                    c->status &= ~KW_I2C_STAT_LAST_RW;
                }
                c->manual_byte_delivered = false;
                keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA);
            } else {
                c->status &= ~KW_I2C_STAT_LAST_AAK;
                keywest_i2c_stop(c);
                keywest_i2c_set_irq(c, KW_I2C_IRQ_DATA | KW_I2C_IRQ_STOP);
            }
        } else if (c->xfer_active && !(c->addr & 1) &&
                  i2c_send(c->bus, val) == 0) {
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


void keywest_i2c_init(KeyWestI2CState *c, DeviceState *owner,
                      const char *name, uint64_t size)
{
    g_autofree char *busname = g_strdup_printf("%s.bus", name);

    c->name = name;
    c->bus = i2c_init_bus(owner, busname);
    memory_region_init_io(&c->mem, OBJECT(owner), &keywest_i2c_ops, c,
                          name, size);
}

/* Pulsar: a register file with an auto-incrementing pointer */
OBJECT_DECLARE_SIMPLE_TYPE(PulsarState, PULSAR_CLOCK)

struct PulsarState {
    I2CSlave parent_obj;

    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_set;
};

static int pulsar_event(I2CSlave *i2c, enum i2c_event event)
{
    PulsarState *s = PULSAR_CLOCK(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_set = false;
    }
    return 0;
}

static int pulsar_send(I2CSlave *i2c, uint8_t data)
{
    PulsarState *s = PULSAR_CLOCK(i2c);

    if (!s->ptr_set) {
        s->ptr = data;
        s->ptr_set = true;
    } else {
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t pulsar_recv(I2CSlave *i2c)
{
    PulsarState *s = PULSAR_CLOCK(i2c);

    return s->regs[s->ptr++];
}

static const VMStateDescription vmstate_pulsar = {
    .name = TYPE_PULSAR_CLOCK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PulsarState),
        VMSTATE_UINT8_ARRAY(regs, PulsarState, 256),
        VMSTATE_UINT8(ptr, PulsarState),
        VMSTATE_BOOL(ptr_set, PulsarState),
        VMSTATE_END_OF_LIST()
    }
};

static void pulsar_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = pulsar_event;
    k->send = pulsar_send;
    k->recv = pulsar_recv;
    dc->vmsd = &vmstate_pulsar;
}

static const TypeInfo pulsar_info = {
    .name          = TYPE_PULSAR_CLOCK,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PulsarState),
    .class_init    = pulsar_class_init,
};

static void keywest_i2c_register_types(void)
{
    type_register_static(&pulsar_info);
}

type_init(keywest_i2c_register_types)
