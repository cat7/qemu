/*
 * Athens (Apple part# 343S1191) programmable clock generator emulation
 *
 * Real hardware: an I2C-addressable clock synthesizer ASIC used on PCI
 * Power Macintosh boards (including the Beige G3/"Gossamer") to derive the
 * system and video dot clocks. Firmware also uses its I2C interface as a
 * simple presence probe during early boot -- on the Beige G3 ROM this is
 * one of several onboard I2C devices whose CUDA_COMBINED_FORMAT_IIC probe
 * must succeed for native boot to avoid falling into a factory-diagnostics
 * serial console (this device was entirely absent from g3beige before,
 * meaning every one of those onboard-device probes NAK'd).
 *
 * This model only implements the I2C register file and the fixed ID byte
 * returned on read (confirmed against DingusPPC's register-compatible
 * AthensClocks, whose real, working Gossamer boot relies on exactly this:
 * an unconditional 0x41 "my ID" response to any read, regardless of the
 * register pointer). Actual clock-frequency synthesis from the D2/N2/MUX
 * registers is not needed for boot and is not modeled.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_ATHENS_CLOCKS "athens"
OBJECT_DECLARE_SIMPLE_TYPE(AthensState, ATHENS_CLOCKS)

#define ATHENS_NUM_REGS 16
#define ATHENS_ID_BYTE  0x41

struct AthensState {
    I2CSlave i2c;

    uint8_t regs[ATHENS_NUM_REGS];
    uint8_t reg_num;
    uint8_t pos;
};

static int athens_event(I2CSlave *i2c, enum i2c_event event)
{
    AthensState *s = ATHENS_CLOCKS(i2c);

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        s->pos = 0;
    }
    return 0;
}

static uint8_t athens_recv(I2CSlave *i2c)
{
    return ATHENS_ID_BYTE;
}

static int athens_send(I2CSlave *i2c, uint8_t data)
{
    AthensState *s = ATHENS_CLOCKS(i2c);

    if (s->pos == 0) {
        s->reg_num = data;
    } else if (s->pos == 1 && s->reg_num < ATHENS_NUM_REGS) {
        s->regs[s->reg_num] = data;
    }
    s->pos++;
    return 0;
}

static void athens_reset(DeviceState *dev)
{
    AthensState *s = ATHENS_CLOCKS(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->reg_num = 0;
    s->pos = 0;
}

static const VMStateDescription vmstate_athens = {
    .name = TYPE_ATHENS_CLOCKS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, AthensState, ATHENS_NUM_REGS),
        VMSTATE_UINT8(reg_num, AthensState),
        VMSTATE_UINT8(pos, AthensState),
        VMSTATE_I2C_SLAVE(i2c, AthensState),
        VMSTATE_END_OF_LIST()
    }
};

static void athens_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);

    dc->vmsd = &vmstate_athens;
    device_class_set_legacy_reset(dc, athens_reset);
    isc->event = athens_event;
    isc->recv = athens_recv;
    isc->send = athens_send;
}

static const TypeInfo athens_info = {
    .name = TYPE_ATHENS_CLOCKS,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AthensState),
    .class_init = athens_class_init,
};

static void athens_register_types(void)
{
    type_register_static(&athens_info);
}

type_init(athens_register_types)
