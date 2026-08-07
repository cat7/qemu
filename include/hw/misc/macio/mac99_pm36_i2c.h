/*
 * PowerMac3,6 UniNorth-I2C peripherals: ADM1030 fan controller, CY2213
 * clock synthesizer, DS1775 thermal sensor
 *
 * Minimum-viable models: real PowerMac3,6 hardware, per its own device
 * tree (~/Downloads/device-tree-powermac3,6-smp.txt), only appears to have
 * these chips probed for identity during Open Firmware bring-up, not
 * deeply programmed -- same "accept writes, answer plausible reads"
 * treatment already used for the PowerMac3,4 TAS3001 stub in keylargo.c.
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

#ifndef HW_MISC_MACIO_MAC99_PM36_I2C_H
#define HW_MISC_MACIO_MAC99_PM36_I2C_H

#define TYPE_ADM1030 "adm1030"
#define TYPE_CY2213  "cy2213"
#define TYPE_DS1775  "ds1775"

#endif
