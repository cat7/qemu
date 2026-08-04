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

#ifndef MACIO_GPIO_H
#define MACIO_GPIO_H

#include "hw/ppc/openpic.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define KEYLARGO_GPIO_EXTINT_CNT 18

/* KeyLargo GPIO register offsets used for SMP CPU reset control */
#define KEYLARGO_GPIO_EXTINT_0        0x58

#define KL_GPIO_RESET_CPU0             (KEYLARGO_GPIO_EXTINT_0 + 0x03)
#define KL_GPIO_RESET_CPU1             (KEYLARGO_GPIO_EXTINT_0 + 0x04)
#define KL_GPIO_RESET_CPU2             (KEYLARGO_GPIO_EXTINT_0 + 0x0f)
#define KL_GPIO_RESET_CPU3             (KEYLARGO_GPIO_EXTINT_0 + 0x10)

#define TYPE_MACIO_GPIO "macio-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MacIOGPIOState, MACIO_GPIO)

struct MacIOGPIOState {
    /*< private >*/
    SysBusDevice parent;
    /*< public >*/

    MemoryRegion gpiomem;
    qemu_irq gpio_extirqs[KEYLARGO_GPIO_EXTINT_CNT];
    uint8_t gpio_levels[8];
    uint8_t gpio_regs[36];
    uint32_t nb_cpus;
};

void macio_set_gpio(MacIOGPIOState *s, uint32_t gpio, bool state);

#endif
