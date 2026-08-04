/*
 * PowerMac NewWorld MacIO GPIO emulation
 *
 * Copyright (c) 2016 Benjamin Herrenschmidt
 * Copyright (c) 2018 Mark Cave-Ayland
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
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/misc/macio/macio.h"
#include "hw/misc/macio/gpio.h"
#include "hw/core/irq.h"
#include "hw/core/nmi.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

enum MacioGPIORegisterBits {
    OUT_DATA   = 1,
    IN_DATA    = 2,
    OUT_ENABLE = 4,
};

void macio_set_gpio(MacIOGPIOState *s, uint32_t gpio, bool state)
{
    uint8_t new_reg;

    trace_macio_set_gpio(gpio, state);

    if (s->gpio_regs[gpio] & OUT_ENABLE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "GPIO: Setting GPIO %d while it's an output\n", gpio);
    }

    new_reg = s->gpio_regs[gpio] & ~IN_DATA;
    if (state) {
        new_reg |= IN_DATA;
    }

    /*
     * CPU reset lines (GPIO 3, 4, 15, 16) must always propagate, even if the
     * register value doesn't change, so that repeated assert/deassert
     * writes from the guest reliably kick the reset IRQ line.
     */
    if (gpio != 3 && gpio != 4 && gpio != 15 && gpio != 16 &&
        new_reg == s->gpio_regs[gpio]) {
        return;
    }

    s->gpio_regs[gpio] = new_reg;

    /*
     * Note that we probably need to get access to the MPIC config to
     * decode polarity since qemu always use "raise" regardless.
     *
     * For now, we hard wire known GPIOs
     */

    switch (gpio) {
    case 1:
    case 3:
    case 4:
    case 15:
    case 16:
        /*
         * Level low: GPIO 1 is the machine's own reset/interrupt line;
         * GPIO 3/4/15/16 are the KeyLargo CPU0-3 soft-reset lines used to
         * hold secondary CPUs in reset and release them for SMP.
         */
        if (!state) {
            trace_macio_gpio_irq_assert(gpio);
            qemu_irq_raise(s->gpio_extirqs[gpio]);
        } else {
            trace_macio_gpio_irq_deassert(gpio);
            qemu_irq_lower(s->gpio_extirqs[gpio]);
        }
        break;

    case 9:
        /* Edge, triggered by NMI below */
        if (state) {
            trace_macio_gpio_irq_assert(gpio);
            qemu_irq_raise(s->gpio_extirqs[gpio]);
        } else {
            trace_macio_gpio_irq_deassert(gpio);
            qemu_irq_lower(s->gpio_extirqs[gpio]);
        }
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "GPIO: setting unimplemented GPIO %d", gpio);
    }
}

static void macio_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    MacIOGPIOState *s = opaque;
    uint8_t ibit;

    trace_macio_gpio_write(addr, value);

    /* Levels regs are read-only */
    if (addr < 8) {
        return;
    }

    addr -= 8;
    if (addr < 36) {
        value &= ~IN_DATA;

        if (value & OUT_ENABLE) {
            ibit = (value & OUT_DATA) << 1;
        } else {
            ibit = s->gpio_regs[addr] & IN_DATA;
        }

        /*
         * The CPU1-3 soft-reset aliases only exist on boards that have
         * those CPUs. On boards that do not, the same pins carry real,
         * unrelated functions -- on the PowerMac3,4 extint-gpio16 is the
         * speaker-id Dallas one-wire bus, and routing its writes through
         * the CPU3-reset alias turned AppleDallasDriver's tristate into
         * "input reads high": a phantom always-present speaker ROM whose
         * presence hunt then failed forever ("Sound assertion
         * \"++errorCount > 10\" ... AppleDallasDriver.cpp at line 152"
         * storming the OS X installer until ApplePMU shut the machine
         * down). An undriven pin with no device attached reads low (the
         * one-wire pull-up lives in the speaker pod), which is also the
         * plain-storage behavior below.
         */
        if (s->nb_cpus > 1 &&
            addr == (KL_GPIO_RESET_CPU1 - KEYLARGO_GPIO_EXTINT_0)) {
            macio_set_gpio(s, 4, !(value & OUT_ENABLE) || (ibit != 0));
        } else if (s->nb_cpus > 2 &&
                   addr == (KL_GPIO_RESET_CPU2 - KEYLARGO_GPIO_EXTINT_0)) {
            macio_set_gpio(s, 15, !(value & OUT_ENABLE) || (ibit != 0));
        } else if (s->nb_cpus > 3 &&
                   addr == (KL_GPIO_RESET_CPU3 - KEYLARGO_GPIO_EXTINT_0)) {
            macio_set_gpio(s, 16, !(value & OUT_ENABLE) || (ibit != 0));
        } else {
            s->gpio_regs[addr] = value | ibit;
        }
    }
}

static uint64_t macio_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    MacIOGPIOState *s = opaque;
    uint64_t val = 0;

    /* Levels regs */
    if (addr < 8) {
        val = s->gpio_levels[addr];
    } else {
        addr -= 8;

        if (addr < 36) {
            val = s->gpio_regs[addr];
        }
    }

    trace_macio_gpio_read(addr, val);
    return val;
}

static const MemoryRegionOps macio_gpio_ops = {
    .read = macio_gpio_read,
    .write = macio_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void macio_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MacIOGPIOState *s = MACIO_GPIO(obj);
    int i;

    for (i = 0; i < KEYLARGO_GPIO_EXTINT_CNT; i++) {
        sysbus_init_irq(sbd, &s->gpio_extirqs[i]);
    }

    memory_region_init_io(&s->gpiomem, OBJECT(s), &macio_gpio_ops, obj,
                          "gpio", 0x30);
    sysbus_init_mmio(sbd, &s->gpiomem);
}

static const Property macio_gpio_properties[] = {
    /* CPUs actually present: gates the CPU1-3 soft-reset pin aliases */
    DEFINE_PROP_UINT32("nb-cpus", MacIOGPIOState, nb_cpus, 1),
};

static const VMStateDescription vmstate_macio_gpio = {
    .name = "macio_gpio",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(gpio_levels, MacIOGPIOState, 8),
        VMSTATE_UINT8_ARRAY(gpio_regs, MacIOGPIOState, 36),
        VMSTATE_END_OF_LIST()
    }
};

static void macio_gpio_reset(DeviceState *dev)
{
    MacIOGPIOState *s = MACIO_GPIO(dev);

    /* GPIO 1 is up by default */
    macio_set_gpio(s, 1, true);

    /*
     * GPIO 3 (CPU0 soft-reset) is up by default. GPIO 4/15/16 (CPU1-3
     * soft-reset) are intentionally left low/asserted here: secondary
     * CPUs stay held in reset until the guest OS deasserts them to
     * start each one up (see hw/ppc/mac_newworld.c cpu_kick()).
     */
    macio_set_gpio(s, 3, true);

    /*
     * GPIO 9 is the programmer's switch (the real PowerMac3,4 device tree
     * gives its programmer-switch node interrupt 0x37, which is this line).
     * The button is pulled up and reads high when nobody is holding it in,
     * so it has to come out of reset high: a real Apple ROM polls it during
     * POST and, finding it low, concludes the switch is stuck down, spins
     * for a five second timeout and then reports the failure -- costing five
     * seconds of every boot and flagging an error the machine does not have.
     *
     * Set the input level directly rather than through macio_set_gpio():
     * this line's interrupt is edge-triggered on the level going high, which
     * is how the NMI below injects a press, so going through there would
     * assert a spurious NMI on every reset.
     */
    s->gpio_regs[9] |= IN_DATA;
}

static void macio_gpio_nmi(NMIState *n)
{
    macio_set_gpio(MACIO_GPIO(n), 9, true);
    macio_set_gpio(MACIO_GPIO(n), 9, false);
}

static void macio_gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    NMIClass *nc = NMI_CLASS(oc);

    device_class_set_legacy_reset(dc, macio_gpio_reset);
    dc->vmsd = &vmstate_macio_gpio;
    device_class_set_props(dc, macio_gpio_properties);
    nc->raise_nmi = macio_gpio_nmi;
}

static const TypeInfo macio_gpio_init_info = {
    .name          = TYPE_MACIO_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MacIOGPIOState),
    .instance_init = macio_gpio_init,
    .class_init    = macio_gpio_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_NMI },
        { }
    },
};

static void macio_gpio_register_types(void)
{
    type_register_static(&macio_gpio_init_info);
}

type_init(macio_gpio_register_types)
