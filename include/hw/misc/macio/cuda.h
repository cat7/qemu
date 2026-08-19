/*
 * QEMU PowerMac CUDA device support
 *
 * Copyright (c) 2004-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
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

#ifndef CUDA_H
#define CUDA_H

#include "hw/input/adb.h"
#include "hw/misc/mos6522.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

/* CUDA commands (2nd byte) */
#define CUDA_WARM_START                0x0
#define CUDA_AUTOPOLL                  0x1
#define CUDA_GET_6805_ADDR             0x2
#define CUDA_GET_TIME                  0x3
#define CUDA_GET_PRAM                  0x7
#define CUDA_SET_6805_ADDR             0x8
#define CUDA_SET_TIME                  0x9
#define CUDA_POWERDOWN                 0xa
#define CUDA_POWERUP_TIME              0xb
#define CUDA_SET_PRAM                  0xc
#define CUDA_MS_RESET                  0xd
#define CUDA_SEND_DFAC                 0xe
#define CUDA_BATTERY_SWAP_SENSE        0x10
#define CUDA_RESET_SYSTEM              0x11
#define CUDA_SET_IPL                   0x12
#define CUDA_FILE_SERVER_FLAG          0x13
#define CUDA_SET_AUTO_RATE             0x14
#define CUDA_GET_AUTO_RATE             0x16
#define CUDA_SET_DEVICE_LIST           0x19
#define CUDA_GET_DEVICE_LIST           0x1a
#define CUDA_SET_ONE_SECOND_MODE       0x1b
#define CUDA_SET_POWER_MESSAGES        0x21
#define CUDA_GET_SET_IIC               0x22
#define CUDA_WAKEUP                    0x23
#define CUDA_TIMER_TICKLE              0x24
#define CUDA_COMBINED_FORMAT_IIC       0x25
#define CUDA_OUT_PB0                   0x26


/* MOS6522 CUDA */
struct MOS6522CUDAState {
    /*< private >*/
    MOS6522State parent_obj;
};

#define TYPE_MOS6522_CUDA "mos6522-cuda"
OBJECT_DECLARE_SIMPLE_TYPE(MOS6522CUDAState, MOS6522_CUDA)

/* Cuda */
#define TYPE_CUDA "cuda"
OBJECT_DECLARE_SIMPLE_TYPE(CUDAState, CUDA)

struct CUDAState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion mem;

    ADBBusState adb_bus;
    MOS6522CUDAState mos6522_cuda;
    I2CBus *i2c_bus;

    uint32_t tick_offset;
    uint64_t tb_frequency;

    uint8_t last_b;
    uint8_t last_acr;

    /*
     * MacOS 9 is racy and requires a delay upon setting the SR_INT bit.
     * The actual delay is context-dependent -- see the CUDA_SR_DELAY_*
     * constants in cuda.c -- so this is just the shared one-shot timer,
     * armed with a different duration depending on what triggered it.
     */
    QEMUTimer *sr_delay_timer;
    /* virtual-time deadline of the pending sr_delay_timer, for the
     * guest-access catch-up path (see cuda_sr_int_catch_up()) */
    int64_t sr_int_due_ns;

    /*
     * CUDA_SET_ONE_SECOND_MODE support: once enabled, an unsolicited
     * real-time-clock packet is pushed to the host once per real
     * second (independent of, and in addition to, ADB autopoll). Real
     * classic Mac OS enables this during boot; previously unimplemented
     * here (guest got ERROR_PACKET/"unknown command" and never
     * received the periodic tick it was expecting).
     */
    QEMUTimer *one_sec_timer;
    uint8_t one_sec_mode;

    int data_in_size;
    int data_in_index;
    int data_out_index;

    qemu_irq irq;
    uint8_t data_in[128];
    uint8_t data_out[16];

    /* Extended Parameter RAM, accessed via CUDA_GET_PRAM/CUDA_SET_PRAM.
     * Optionally backed by a "-drive" file (like q800's mac_via PRAM) so
     * settings the guest OS stores in PRAM (e.g. the Memory control
     * panel's virtual memory on/off flag) persist across separate QEMU
     * invocations instead of resetting to zero every boot. */
    uint8_t pram[256];
    BlockBackend *pram_blk;
};

#endif /* CUDA_H */
