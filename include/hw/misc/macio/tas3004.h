/*
 * TI TAS3004 digital audio equalizer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MACIO_TAS3004_H
#define HW_MISC_MACIO_TAS3004_H

#define TYPE_TAS3004 "tas3004"

/* 7-bit I2C address on the K2 bus */
#define TAS3004_I2C_ADDR 0x35

#include "hw/i2c/i2c.h"

/* Output silenced: volume programmed to zero, or analog power-down */
bool tas3004_muted(I2CSlave *i2c);

#endif
