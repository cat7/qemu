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
#include "hw/audio/awacs.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"
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

    /* the codec whose output this processor sits behind (may be NULL) */
    AWACSState *codec;
};

/*
 * Speaker attenuator code (bits 0-4) in dB: 1 dB steps to -24 dB, then a
 * non-linear tail up to -37.5 dB (ST TDA7433 datasheet, table for
 * sub-addresses 3-6). Bit 5 mutes the channel outright.
 */
static double tda7433_atten_db(uint8_t code)
{
    static const double tail[] = { 25.5, 27, 28.5, 30, 32, 34.5, 37.5 };

    code &= 0x1f;
    return code <= 24 ? code : tail[code - 25];
}

/*
 * Translate the register file into what the codec's backend voice needs.
 * Register map (ST TDA7433 datasheet; Apple's awacs_OWhw.h names in
 * brackets): 0 = input selector [kInFuncReg] -- bits 1:0 = 01 selects
 * IN1, the only input wired to the codec (Apple's "mute bit" 0x02 turns
 * this into 11, "no input"); 1 = master gain [kVolReg], +32 dB minus the
 * register value, 0x20 = 0 dB, 0x6f = -79 dB; 2 = bass/treble (flat at
 * 0xff, not modelled); 3/5 = left/right internal speaker attenuators
 * [kLFAttnReg/kRFAttnReg]; 4/6 = left/right rear jack, which this machine
 * never senses anything on. Mac OS 9 drives the slider through register
 * 1, Mac OS X through registers 3-6; the ROM sets register 1 from the
 * PRAM volume for the chime.
 */
static void tda7433_apply(TDA7433State *s)
{
    bool mute = (s->regs[0] & 0x3) != 0x1;
    double master_db = (int)(s->regs[1] & 0x7f) - 32;   /* attenuation */
    double left_db = master_db + tda7433_atten_db(s->regs[3]);
    double right_db = master_db + tda7433_atten_db(s->regs[5]);

    if ((s->regs[3] & 0x20) && (s->regs[5] & 0x20)) {
        mute = true;
    } else if (s->regs[3] & 0x20) {
        left_db = 120;
    } else if (s->regs[5] & 0x20) {
        right_db = 120;
    }

    trace_tda7433_apply((int)(left_db * 10), (int)(right_db * 10), mute);
    if (s->codec) {
        awacs_set_processor(s->codec, left_db, right_db, mute);
    }
}

static bool tda7433_send_subaddress(TDA7433State *s, uint8_t data)
{
    if ((data & 0xf) > 6) {
        return false;
    }
    s->sub_addr = data & 0xf;
    s->auto_inc = !!(data & 0x10);
    trace_tda7433_subaddr(s->sub_addr, s->auto_inc);
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

    trace_tda7433_read(s->sub_addr, s->regs[s->sub_addr]);
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

    trace_tda7433_write(s->sub_addr, data);
    s->regs[s->sub_addr] = data;
    tda7433_apply(s);
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
    /* input selector 0 = IN2, which nothing drives: silent until programmed */
    tda7433_apply(s);
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

static const Property tda7433_properties[] = {
    DEFINE_PROP_LINK("codec", TDA7433State, codec, TYPE_AWACS, AWACSState *),
};

static void tda7433_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);

    device_class_set_props(dc, tda7433_properties);
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
