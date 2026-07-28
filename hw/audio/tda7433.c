/*
 * TDA7433 audio tone/volume control emulation
 *
 * Real hardware: a Philips/NXP TDA7433 audio processor IC, I2C-addressable,
 * used on PCI Power Macintosh boards (including the Beige G3/"Gossamer") as
 * a sideband tone/volume control chip alongside the AWACS/Screamer sound
 * codec's own MMIO register block. Firmware also uses its I2C interface as
 * a simple presence probe during early boot -- on the Beige G3 ROM this is
 * one of several onboard I2C devices whose CUDA_COMBINED_FORMAT_IIC probe
 * must succeed for native boot to avoid falling into a factory-diagnostics
 * serial console (this device was entirely absent from g3beige before,
 * meaning this onboard-device probe NAK'd).
 *
 * This model only implements the I2C register file protocol (subaddress
 * select with optional auto-increment, per the TDA7433 datasheet), not
 * actual tone/volume signal processing (confirmed sufficient against
 * DingusPPC's register-compatible AudioProcessor, whose real, working
 * Gossamer boot relies on exactly this transaction protocol succeeding).
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

#define TYPE_TDA7433 "tda7433"
OBJECT_DECLARE_SIMPLE_TYPE(TDA7433State, TDA7433)

#define TDA7433_NUM_REGS 7

struct TDA7433State {
    I2CSlave i2c;

    uint8_t regs[TDA7433_NUM_REGS];
    uint8_t sub_addr;
    bool auto_inc;
    int pos;
};

static bool tda7433_send_subaddress(TDA7433State *s, uint8_t data)
{
    if ((data & 0xf) > 6) {
        return false;
    }
    s->sub_addr = data & 0xf;
    s->auto_inc = !!(data & 0x10);
    return true;
}

static int tda7433_event(I2CSlave *i2c, enum i2c_event event)
{
    TDA7433State *s = TDA7433(i2c);

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        s->pos = 0;
    }
    return 0;
}

static uint8_t tda7433_recv(I2CSlave *i2c)
{
    TDA7433State *s = TDA7433(i2c);

    return s->regs[s->sub_addr];
}

static int tda7433_send(I2CSlave *i2c, uint8_t data)
{
    TDA7433State *s = TDA7433(i2c);

    if (s->pos == 0) {
        s->pos++;
        return tda7433_send_subaddress(s, data) ? 0 : -1;
    }

    if (s->sub_addr > 6) {
        return -1;
    }

    s->regs[s->sub_addr] = data;
    if (s->auto_inc) {
        s->sub_addr++;
    }
    return 0;
}

static void tda7433_reset(DeviceState *dev)
{
    TDA7433State *s = TDA7433(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->sub_addr = 0;
    s->auto_inc = false;
    s->pos = 0;
}

static const VMStateDescription vmstate_tda7433 = {
    .name = TYPE_TDA7433,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, TDA7433State, TDA7433_NUM_REGS),
        VMSTATE_UINT8(sub_addr, TDA7433State),
        VMSTATE_BOOL(auto_inc, TDA7433State),
        VMSTATE_I2C_SLAVE(i2c, TDA7433State),
        VMSTATE_END_OF_LIST()
    }
};

static void tda7433_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);

    dc->vmsd = &vmstate_tda7433;
    device_class_set_legacy_reset(dc, tda7433_reset);
    isc->event = tda7433_event;
    isc->recv = tda7433_recv;
    isc->send = tda7433_send;
}

static const TypeInfo tda7433_info = {
    .name = TYPE_TDA7433,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TDA7433State),
    .class_init = tda7433_class_init,
};

static void tda7433_register_types(void)
{
    type_register_static(&tda7433_info);
}

type_init(tda7433_register_types)
