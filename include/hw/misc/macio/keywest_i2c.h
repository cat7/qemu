/*
 * Apple "KeyWest" I2C controller cell (UniNorth, U3, KeyLargo, K2)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MACIO_KEYWEST_I2C_H
#define HW_MISC_MACIO_KEYWEST_I2C_H

#include "hw/i2c/i2c.h"
#include "system/memory.h"
#include "qemu/timer.h"

#define KW_I2C_REG_MODE             0x00
#define KW_I2C_REG_CONTROL          0x10
#define KW_I2C_REG_STATUS           0x20
#define KW_I2C_REG_ISR              0x30
#define KW_I2C_REG_IER              0x40
#define KW_I2C_REG_ADDR             0x50
#define KW_I2C_REG_SUBADDR          0x60
#define KW_I2C_REG_DATA             0x70

#define KW_I2C_MODE_DUMB            0x00
#define KW_I2C_MODE_STANDARD        0x04
#define KW_I2C_MODE_STANDARDSUB     0x08
#define KW_I2C_MODE_COMBINED        0x0c
#define KW_I2C_MODE_MODE_MASK       0x0c

#define KW_I2C_CTL_AAK              0x01
#define KW_I2C_CTL_XADDR            0x02
#define KW_I2C_CTL_STOP             0x04
#define KW_I2C_CTL_START            0x08

#define KW_I2C_STAT_BUSY            0x01
#define KW_I2C_STAT_LAST_AAK        0x02
#define KW_I2C_STAT_LAST_RW         0x04
#define KW_I2C_STAT_SDA             0x08
#define KW_I2C_STAT_SCL             0x10

#define KW_I2C_IRQ_DATA             0x01
#define KW_I2C_IRQ_ADDR             0x02
#define KW_I2C_IRQ_STOP             0x04
#define KW_I2C_IRQ_START            0x08
#define KW_I2C_IRQ_MASK             0x0f

/* One byte at 100 kHz */
#define KW_I2C_BYTE_US              90

typedef struct KeyWestI2CState {
    MemoryRegion mem;
    const char *name;
    I2CBus *bus;
    qemu_irq irq;               /* (ISR & IER) != 0; may be unwired */
    bool xfer_active;
    /* DUMB mode: between START and the address byte written to DATA */
    bool manual_addr_pending;
    /* DUMB mode reads: the first IRQ_DATA ack delivers the byte */
    bool manual_byte_delivered;
    /* XADDR reads: the data byte is fetched when IRQ_ADDR is acked */
    bool read_pending;
    /* XADDR reads: the byte just delivered was NAK'd (AAK clear) */
    bool last_nak;
    /* XADDR reads: the next byte clocks in one byte time after an ack */
    QEMUTimer *byte_timer;
    uint8_t mode;
    uint8_t control;
    uint8_t status;
    uint8_t isr;
    uint8_t ier;
    uint8_t addr;
    uint8_t subaddr;
    uint8_t data;
} KeyWestI2CState;

void keywest_i2c_init(KeyWestI2CState *c, DeviceState *owner,
                      const char *name, uint64_t size);
void keywest_i2c_reset(KeyWestI2CState *c);

/* A clock chip on the U3 I2C bus that only needs to answer */
#define TYPE_PULSAR_CLOCK "pulsar-clock"

#endif
