/*
 * Cirrus Logic CS8420 digital audio sample rate converter / S/PDIF
 * transceiver ("Topaz") on the K2 I2C bus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/macio/cs8420.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "trace.h"

#define CS8420_MAP_INCR     0x80
#define CS8420_MAP_MASK     0x7f
#define CS8420_ID_VERSION   0x7f
/* ID 0001, revision 1 */
#define CS8420_ID           0x11

OBJECT_DECLARE_SIMPLE_TYPE(CS8420State, CS8420)

struct CS8420State {
    I2CSlave parent_obj;

    uint8_t map;
    bool map_set;
    uint8_t regs[0x80];
};

/* The first byte of a write sets the memory address pointer */
static int cs8420_event(I2CSlave *i2c, enum i2c_event event)
{
    CS8420State *s = CS8420(i2c);

    if (event == I2C_START_SEND) {
        s->map_set = false;
    }
    return 0;
}

static void cs8420_advance(CS8420State *s)
{
    s->map = (s->map & CS8420_MAP_INCR) |
             ((s->map + 1) & CS8420_MAP_MASK);
}

static int cs8420_send(I2CSlave *i2c, uint8_t data)
{
    CS8420State *s = CS8420(i2c);
    uint8_t reg;

    if (!s->map_set) {
        s->map = data;
        s->map_set = true;
        return 0;
    }
    reg = s->map & CS8420_MAP_MASK;
    trace_cs8420_write(reg, data);
    if (reg != CS8420_ID_VERSION) {
        s->regs[reg] = data;
    }
    if (s->map & CS8420_MAP_INCR) {
        cs8420_advance(s);
    }
    return 0;
}

static uint8_t cs8420_recv(I2CSlave *i2c)
{
    CS8420State *s = CS8420(i2c);
    uint8_t val = s->regs[s->map & CS8420_MAP_MASK];

    if (s->map & CS8420_MAP_INCR) {
        cs8420_advance(s);
    }
    return val;
}

static void cs8420_reset(DeviceState *dev)
{
    CS8420State *s = CS8420(dev);

    s->map = 0;
    s->map_set = false;
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[CS8420_ID_VERSION] = CS8420_ID;
}

static const VMStateDescription vmstate_cs8420 = {
    .name = TYPE_CS8420,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, CS8420State),
        VMSTATE_UINT8(map, CS8420State),
        VMSTATE_BOOL(map_set, CS8420State),
        VMSTATE_BUFFER(regs, CS8420State),
        VMSTATE_END_OF_LIST()
    }
};

static void cs8420_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = cs8420_event;
    k->send = cs8420_send;
    k->recv = cs8420_recv;
    device_class_set_legacy_reset(dc, cs8420_reset);
    dc->vmsd = &vmstate_cs8420;
}

static const TypeInfo cs8420_info = {
    .name          = TYPE_CS8420,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(CS8420State),
    .class_init    = cs8420_class_init,
};

static void cs8420_register_types(void)
{
    type_register_static(&cs8420_info);
}

type_init(cs8420_register_types)
