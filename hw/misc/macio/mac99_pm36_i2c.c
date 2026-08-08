/*
 * PowerMac3,6 UniNorth-I2C peripherals: ADM1030 fan controller, CY2213
 * clock synthesizer, DS1775 thermal sensor
 *
 * See mac99_pm36_i2c.h for scope. All three are simple "register pointer,
 * then read/write the addressed register" SMBus-style devices, the same
 * access pattern hw/sensor/tmp105.c already models for a closely related
 * chip (DS1775 is itself near-identical to TMP105's temperature/config/
 * limit register layout).
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
#include "hw/misc/macio/mac99_pm36_i2c.h"
#include "qemu/module.h"
#include "qom/object.h"

/* ---------------------------------------------------------------------- */
/* Generic "register pointer, then read/write bytes at it" register file   */
/* ---------------------------------------------------------------------- */

typedef struct RegFileI2CState {
    I2CSlave i2c;
    uint8_t regs[256];
    uint8_t pointer;
    bool have_pointer;
} RegFileI2CState;

#define TYPE_REGFILE_I2C_BASE "mac99-pm36-regfile-i2c-base"
OBJECT_DECLARE_SIMPLE_TYPE(RegFileI2CState, REGFILE_I2C_BASE)

static int regfile_i2c_event(I2CSlave *i2c, enum i2c_event event)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(i2c);

    /*
     * Reset on I2C_FINISH too, not just I2C_START_SEND: a targeted read
     * (STANDARDSUB mode, "write the register pointer, repeated start, read
     * the byte") opens with I2C_START_RECV, which never touches this flag.
     * Without also clearing it at the end of the PREVIOUS transaction, the
     * pointer-setting byte of every read after the first gets treated as a
     * data write into whatever register the pointer was last left at,
     * instead of repositioning it -- the same reset-on-both-events pattern
     * hw/nvram/eeprom_at24c.c already uses for the identical reason.
     */
    if (event == I2C_START_SEND || event == I2C_FINISH) {
        s->have_pointer = false;
    }
    return 0;
}

static uint8_t regfile_i2c_recv(I2CSlave *i2c)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(i2c);
    uint8_t ret = s->regs[s->pointer];

    s->pointer++;
    return ret;
}

static int regfile_i2c_send(I2CSlave *i2c, uint8_t data)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(i2c);

    if (!s->have_pointer) {
        s->pointer = data;
        s->have_pointer = true;
    } else {
        s->regs[s->pointer] = data;
        s->pointer++;
    }
    return 0;
}

static const VMStateDescription vmstate_regfile_i2c = {
    .name = "mac99-pm36-regfile-i2c",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RegFileI2CState, 256),
        VMSTATE_UINT8(pointer, RegFileI2CState),
        VMSTATE_BOOL(have_pointer, RegFileI2CState),
        VMSTATE_I2C_SLAVE(i2c, RegFileI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void regfile_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = regfile_i2c_event;
    k->recv = regfile_i2c_recv;
    k->send = regfile_i2c_send;
    dc->vmsd = &vmstate_regfile_i2c;
}

static const TypeInfo regfile_i2c_base_info = {
    .name          = TYPE_REGFILE_I2C_BASE,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(RegFileI2CState),
    .class_init    = regfile_i2c_class_init,
    .abstract      = true,
};

/* ---------------------------------------------------------------------- */
/* ADM1030 fan controller                                                  */
/* ---------------------------------------------------------------------- */

/*
 * A live boot trace (real PowerMac3,6 ROM) shows the full sequence: write
 * real ADM1030 config registers (0x00 Config1, 0x01 Config2, 0x16/0x1a
 * THERM limits, 0x22 fan speed config, 0x23 filter, 0x24/0x25 Tmin/Trange
 * -- matches the datasheet's own "Automatic Fan Speed Control Mode" setup
 * sequence and the real Linux adm1031 driver's init path, which does the
 * same read-modify-write dance on 0x00/0x01), then set the read pointer to
 * 0x3e and read it -- with the STANDARD-mode auto-stop fix in keylargo.c,
 * this is a normal single-byte "current address read" of the Company ID,
 * exactly as the real ADM1030 datasheet (Table 16) describes it: reads are
 * always one byte. Power-on defaults below are transcribed directly from
 * that same table (Rev 3, April 2012) rather than guessed.
 */
#define ADM1030_REG_DEVICE_ID  0x3d
#define ADM1030_REG_COMPANY_ID 0x3e
#define ADM1030_REG_THERM_REV  0x3f
static void adm1030_reset(DeviceState *dev)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(dev);

    memset(s->regs, 0xff, sizeof(s->regs));
    s->regs[0x00] = 0x90; /* Configuration Register 1 */
    s->regs[0x01] = 0x7f; /* Configuration Register 2 */
    s->regs[0x02] = 0x00; /* Status Register 1 */
    s->regs[0x03] = 0x00; /* Status Register 2 */
    s->regs[0x06] = 0x00; /* Extended Temperature Resolution */
    s->regs[0x10] = 0xff; /* Fan Tach High Limit */
    s->regs[0x14] = 0x3c; /* Local Temp High Limit, 60C */
    s->regs[0x15] = 0x00; /* Local Temp Low Limit, 0C */
    s->regs[0x16] = 0x46; /* Local Temp Therm Limit, 70C */
    s->regs[0x18] = 0x50; /* Remote Temp High Limit, 80C */
    s->regs[0x19] = 0x00; /* Remote Temp Low Limit, 0C */
    s->regs[0x1a] = 0x64; /* Remote Temp Therm Limit, 100C */
    s->regs[0x20] = 0x5d; /* Fan Characteristics Register 1 */
    s->regs[0x22] = 0x05; /* Fan Speed Config Register */
    s->regs[0x23] = 0x50; /* Fan Filter Register */
    s->regs[0x24] = 0x41; /* Local Temp Tmin/Trange */
    s->regs[0x25] = 0x61; /* Remote Temp Tmin/Trange */
    s->regs[ADM1030_REG_DEVICE_ID] = 0x30;
    s->regs[ADM1030_REG_COMPANY_ID] = 0x41;
    s->regs[ADM1030_REG_THERM_REV] = 0x80;
    s->pointer = 0;
    s->have_pointer = false;
}

static void adm1030_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, adm1030_reset);
}

static const TypeInfo adm1030_info = {
    .name       = TYPE_ADM1030,
    .parent     = TYPE_REGFILE_I2C_BASE,
    .class_init = adm1030_class_init,
};

/* ---------------------------------------------------------------------- */
/* CY2213 clock synthesizer                                                */
/* ---------------------------------------------------------------------- */

/*
 * Real PowerMac3,6 hardware only appears to use this for OS-level CPU
 * speed-stepping (the device tree's "platform-do-clockspeed" method),
 * not during Open Firmware bring-up -- plain register storage, no
 * identity bytes confirmed or expected to be needed yet.
 */
static void cy2213_reset(DeviceState *dev)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->pointer = 0;
    s->have_pointer = false;
}

static void cy2213_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, cy2213_reset);
}

static const TypeInfo cy2213_info = {
    .name       = TYPE_CY2213,
    .parent     = TYPE_REGFILE_I2C_BASE,
    .class_init = cy2213_class_init,
};

/* ---------------------------------------------------------------------- */
/* DS1775 thermal sensor                                                   */
/* ---------------------------------------------------------------------- */

/*
 * Same register layout family as TMP105 (hw/sensor/tmp105.c): pointer 0 =
 * temperature (16-bit, MSB first), 1 = config (8-bit), 2 = T_hyst, 3 =
 * T_os -- the device tree's own method list (write-high-limit/write-low-
 * limit/write-config/read-temp etc.) matches this shape exactly. Reusing
 * the generic byte-addressed register file above rather than TMP105State
 * directly keeps this file self-contained; a fixed, plausible room
 * temperature is enough for the "answer plausible reads" scope here, no
 * conversion/alarm modeling needed since nothing in real PowerMac3,6
 * bring-up appears to act on it.
 */
#define DS1775_REG_TEMPERATURE 0x00

static void ds1775_reset(DeviceState *dev)
{
    RegFileI2CState *s = REGFILE_I2C_BASE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* ~25C in DS1775's 9-bit-in-16, MSB-first, 0.5C/LSB-at-bit7 format. */
    s->regs[DS1775_REG_TEMPERATURE] = 0x19;
    s->regs[DS1775_REG_TEMPERATURE + 1] = 0x00;
    s->pointer = 0;
    s->have_pointer = false;
}

static void ds1775_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, ds1775_reset);
}

static const TypeInfo ds1775_info = {
    .name       = TYPE_DS1775,
    .parent     = TYPE_REGFILE_I2C_BASE,
    .class_init = ds1775_class_init,
};

static void mac99_pm36_i2c_register_types(void)
{
    type_register_static(&regfile_i2c_base_info);
    type_register_static(&adm1030_info);
    type_register_static(&cy2213_info);
    type_register_static(&ds1775_info);
}

type_init(mac99_pm36_i2c_register_types)
