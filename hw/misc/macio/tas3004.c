/*
 * TI TAS3004 digital audio equalizer ("deq") on the K2 I2C bus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/macio/tas3004.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

/* The longest register: one biquad, five 3-byte coefficients */
#define TAS3004_MAX_REG_LEN 15

#define TAS3004_VOL             0x04
#define TAS3004_ACR             0x40
#define TAS3004_ACR_APD         0x01

OBJECT_DECLARE_SIMPLE_TYPE(TAS3004State, TAS3004)

struct TAS3004State {
    I2CSlave parent_obj;

    uint8_t subaddr;
    uint8_t pos;
    uint8_t regs[0x100 * TAS3004_MAX_REG_LEN];
    bool vol_written;
};

/* A write is a subaddress followed by that register's bytes. */
static int tas3004_event(I2CSlave *i2c, enum i2c_event event)
{
    TAS3004State *s = TAS3004(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->pos = 0;
        break;
    case I2C_START_RECV:
        s->pos = 1;
        break;
    default:
        break;
    }
    return 0;
}

static int tas3004_send(I2CSlave *i2c, uint8_t data)
{
    TAS3004State *s = TAS3004(i2c);

    if (s->pos == 0) {
        s->subaddr = data;
    } else if (s->pos <= TAS3004_MAX_REG_LEN) {
        s->regs[s->subaddr * TAS3004_MAX_REG_LEN + s->pos - 1] = data;
        if (s->subaddr == TAS3004_VOL) {
            s->vol_written = true;
        }
        trace_tas3004_write(s->subaddr, s->pos - 1, data);
    }
    s->pos++;
    return 0;
}

static uint8_t tas3004_recv(I2CSlave *i2c)
{
    TAS3004State *s = TAS3004(i2c);
    uint8_t val = 0;

    if (s->pos <= TAS3004_MAX_REG_LEN) {
        val = s->regs[s->subaddr * TAS3004_MAX_REG_LEN + s->pos - 1];
    }
    s->pos++;
    return val;
}

/* Before a driver sets the volume, the reset value must not mute. */
bool tas3004_muted(I2CSlave *i2c)
{
    TAS3004State *s = TAS3004(i2c);
    const uint8_t *vol = &s->regs[TAS3004_VOL * TAS3004_MAX_REG_LEN];
    int i;

    if (s->regs[TAS3004_ACR * TAS3004_MAX_REG_LEN] & TAS3004_ACR_APD) {
        return true;
    }
    if (!s->vol_written) {
        return false;
    }
    for (i = 0; i < 6; i++) {
        if (vol[i]) {
            return false;
        }
    }
    return true;
}

static void tas3004_reset(DeviceState *dev)
{
    TAS3004State *s = TAS3004(dev);

    s->subaddr = 0;
    s->pos = 0;
    memset(s->regs, 0, sizeof(s->regs));
    s->vol_written = false;
}

static const VMStateDescription vmstate_tas3004 = {
    .name = TYPE_TAS3004,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, TAS3004State),
        VMSTATE_UINT8(subaddr, TAS3004State),
        VMSTATE_UINT8(pos, TAS3004State),
        VMSTATE_BUFFER(regs, TAS3004State),
        VMSTATE_BOOL(vol_written, TAS3004State),
        VMSTATE_END_OF_LIST()
    }
};

static void tas3004_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = tas3004_event;
    k->send = tas3004_send;
    k->recv = tas3004_recv;
    device_class_set_legacy_reset(dc, tas3004_reset);
    dc->vmsd = &vmstate_tas3004;
}

static const TypeInfo tas3004_info = {
    .name          = TYPE_TAS3004,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TAS3004State),
    .class_init    = tas3004_class_init,
};

static void tas3004_register_types(void)
{
    type_register_static(&tas3004_info);
}

type_init(tas3004_register_types)
