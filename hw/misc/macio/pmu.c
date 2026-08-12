/*
 * QEMU PowerMac PMU device support
 *
 * Copyright (c) 2016 Benjamin Herrenschmidt, IBM Corp.
 * Copyright (c) 2018 Mark Cave-Ayland
 *
 * Based on the CUDA device by:
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

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/misc/macio/pmu.h"
#include "qemu/timer.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "system/rtc.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"


/* Bits in B data register: all active low */
#define TACK    0x08    /* Transfer request (input) */
#define TREQ    0x10    /* Transfer acknowledge (output) */

/* PMU returns time_t's offset from Jan 1, 1904, not 1970 */
#define RTC_OFFSET                      2082844800

#define VIA_TIMER_FREQ (4700000 / 6)

static void via_set_sr_int(void *opaque)
{
    PMUState *s = opaque;
    MOS6522PMUState *mps = MOS6522_PMU(&s->mos6522_pmu);
    MOS6522State *ms = MOS6522(mps);
    qemu_irq irq = qdev_get_gpio_in(DEVICE(ms), SR_INT_BIT);

    qemu_set_irq(irq, 1);
}

static void pmu_update_extirq(PMUState *s)
{
    if ((s->intbits & s->intmask) != 0) {
        macio_set_gpio(s->gpio, 1, false);
    } else {
        macio_set_gpio(s->gpio, 1, true);
    }
}

static void pmu_adb_poll(void *opaque)
{
    PMUState *s = opaque;
    ADBBusState *adb_bus = &s->adb_bus;
    int olen;

    if (!(s->intbits & PMU_INT_ADB)) {
        olen = adb_poll(adb_bus, s->adb_reply, adb_bus->autopoll_mask);
        trace_pmu_adb_poll(olen);

        if (olen > 0) {
            s->adb_reply_size = olen;
            s->intbits |= PMU_INT_ADB | PMU_INT_ADB_AUTO;
            pmu_update_extirq(s);
        }
    }
}

static void pmu_one_sec_timer(void *opaque)
{
    PMUState *s = opaque;

    trace_pmu_one_sec_timer();

    s->intbits |= PMU_INT_TICK;
    pmu_update_extirq(s);
    s->one_sec_target += 1000;

    timer_mod(s->one_sec_timer, s->one_sec_target);
}

static void pmu_cmd_int_ack(PMUState *s,
                            const uint8_t *in_data, uint8_t in_len,
                            uint8_t *out_data, uint8_t *out_len)
{
    if (in_len != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: INT_ACK command, invalid len: %d want: 0\n",
                      in_len);
        return;
    }

    /* Make appropriate reply packet */
    if (s->intbits & PMU_INT_ADB) {
        if (!s->adb_reply_size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "Odd, PMU_INT_ADB set with no reply in buffer\n");
        }

        memcpy(out_data + 1, s->adb_reply, s->adb_reply_size);
        out_data[0] = s->intbits & (PMU_INT_ADB | PMU_INT_ADB_AUTO);
        *out_len = s->adb_reply_size + 1;
        s->intbits &= ~(PMU_INT_ADB | PMU_INT_ADB_AUTO);
        s->adb_reply_size = 0;
    } else {
        out_data[0] = s->intbits;
        s->intbits = 0;
        *out_len = 1;
    }

    pmu_update_extirq(s);
}

static void pmu_cmd_set_int_mask(PMUState *s,
                                 const uint8_t *in_data, uint8_t in_len,
                                 uint8_t *out_data, uint8_t *out_len)
{
    if (in_len != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: SET_INT_MASK command, invalid len: %d want: 1\n",
                      in_len);
        return;
    }

    trace_pmu_cmd_set_int_mask(s->intmask);
    s->intmask = in_data[0];

    pmu_update_extirq(s);
}

static void pmu_cmd_set_adb_autopoll(PMUState *s, uint16_t mask)
{
    ADBBusState *adb_bus = &s->adb_bus;

    trace_pmu_cmd_set_adb_autopoll(mask);

    if (mask) {
        adb_set_autopoll_mask(adb_bus, mask);
        adb_set_autopoll_enabled(adb_bus, true);
    } else {
        adb_set_autopoll_enabled(adb_bus, false);
    }
}

static void pmu_cmd_adb(PMUState *s,
                        const uint8_t *in_data, uint8_t in_len,
                        uint8_t *out_data, uint8_t *out_len)
{
    int len, adblen;
    uint8_t adb_cmd[255];

    if (in_len < 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: ADB PACKET, invalid len: %d want at least 2\n",
                      in_len);
        return;
    }

    *out_len = 0;

    if (!s->has_adb) {
        trace_pmu_cmd_adb_nobus();
        return;
    }

    /* Set autopoll is a special form of the command */
    if (in_data[0] == 0 && in_data[1] == 0x86) {
        uint16_t mask = in_data[2];
        mask = (mask << 8) | in_data[3];
        if (in_len != 4) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "PMU: ADB Autopoll requires 4 bytes, got %d\n",
                          in_len);
            return;
        }

        pmu_cmd_set_adb_autopoll(s, mask);
        return;
    }

    trace_pmu_cmd_adb_request(in_len, in_data[0], in_data[1], in_data[2],
                              in_data[3], in_data[4]);

    *out_len = 0;

    /* Check ADB len */
    adblen = in_data[2];
    if (adblen > (in_len - 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: ADB len is %d > %d (in_len -3)...erroring\n",
                      adblen, in_len - 3);
        len = -1;
    } else if (adblen > 252) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU: ADB command too big!\n");
        len = -1;
    } else {
        /* Format command */
        adb_cmd[0] = in_data[0];
        memcpy(&adb_cmd[1], &in_data[3], in_len - 3);
        len = adb_request(&s->adb_bus, s->adb_reply + 2, adb_cmd, in_len - 2);

        trace_pmu_cmd_adb_reply(len);
    }

    if (len > 0) {
        /* XXX Check this */
        s->adb_reply_size = len + 2;
        s->adb_reply[0] = 0x01;
        s->adb_reply[1] = len;
    } else {
        /* XXX Check this */
        s->adb_reply_size = 1;
        s->adb_reply[0] = 0x00;
    }

    s->intbits |= PMU_INT_ADB;
    pmu_update_extirq(s);
}

static void pmu_cmd_adb_poll_off(PMUState *s,
                                 const uint8_t *in_data, uint8_t in_len,
                                 uint8_t *out_data, uint8_t *out_len)
{
    ADBBusState *adb_bus = &s->adb_bus;

    if (in_len != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: ADB POLL OFF command, invalid len: %d want: 0\n",
                      in_len);
        return;
    }

    if (s->has_adb) {
        adb_set_autopoll_enabled(adb_bus, false);
    }
}

static void pmu_cmd_shutdown(PMUState *s,
                             const uint8_t *in_data, uint8_t in_len,
                             uint8_t *out_data, uint8_t *out_len)
{
    if (in_len != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: SHUTDOWN command, invalid len: %d want: 4\n",
                      in_len);
        return;
    }

    *out_len = 1;
    out_data[0] = 0;

    if (in_data[0] != 'M' || in_data[1] != 'A' || in_data[2] != 'T' ||
        in_data[3] != 'T') {

        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: SHUTDOWN command, Bad MATT signature\n");
        return;
    }

    qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
}

static void pmu_cmd_reset(PMUState *s,
                          const uint8_t *in_data, uint8_t in_len,
                          uint8_t *out_data, uint8_t *out_len)
{
    if (in_len != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: RESET command, invalid len: %d want: 0\n",
                      in_len);
        return;
    }

    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void pmu_cmd_get_rtc(PMUState *s,
                            const uint8_t *in_data, uint8_t in_len,
                            uint8_t *out_data, uint8_t *out_len)
{
    uint32_t ti;

    if (in_len != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: GET_RTC command, invalid len: %d want: 0\n",
                      in_len);
        return;
    }

    ti = s->tick_offset + (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           / NANOSECONDS_PER_SECOND);
    out_data[0] = ti >> 24;
    out_data[1] = ti >> 16;
    out_data[2] = ti >> 8;
    out_data[3] = ti;
    *out_len = 4;
}

static void pmu_cmd_set_rtc(PMUState *s,
                            const uint8_t *in_data, uint8_t in_len,
                            uint8_t *out_data, uint8_t *out_len)
{
    uint32_t ti;

    if (in_len != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: SET_RTC command, invalid len: %d want: 4\n",
                      in_len);
        return;
    }

    ti = (((uint32_t)in_data[0]) << 24) + (((uint32_t)in_data[1]) << 16)
         + (((uint32_t)in_data[2]) << 8) + in_data[3];

    s->tick_offset = ti - (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                           / NANOSECONDS_PER_SECOND);
}

static void pmu_cmd_system_ready(PMUState *s,
                                 const uint8_t *in_data, uint8_t in_len,
                                 uint8_t *out_data, uint8_t *out_len)
{
    /* Do nothing */
}

static void pmu_cmd_get_version(PMUState *s,
                                const uint8_t *in_data, uint8_t in_len,
                                uint8_t *out_data, uint8_t *out_len)
{
    /*
     * One byte: Linux's pmu_data_len table gives GET_VERSION (0xea) a
     * fixed 1-byte reply, and Mac OS X's ApplePMU agrees -- its
     * getPMUversion() (disassembled from the 10.4 kext) reads exactly one
     * byte here, and only on pre-KeyLargo PMUs; on this machine it
     * fetches the full version from PMU RAM instead (see
     * pmu_cmd_read_pmu_ram below).
     *
     * The value: a real PowerMac3,4 publishes pmu-version = 0x00d07e0c in
     * its device tree, whose low byte -- the part ApplePMU treats as the
     * PMU type -- is 0x0c. Upstream's 1 here was a placeholder (its own
     * comment read "??? Check what Apple does").
     */
    *out_len = 1;
    *out_data = 0x0c;
}

static void pmu_cmd_power_events(PMUState *s,
                                 const uint8_t *in_data, uint8_t in_len,
                                 uint8_t *out_data, uint8_t *out_len)
{
    if (in_len < 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: POWER EVENTS command, invalid len %d, want at least 1\n",
                      in_len);
        return;
    }

    switch (in_data[0]) {
    /* Dummies for now */
    case PMU_PWR_GET_POWERUP_EVENTS:
        *out_len = 2;
        out_data[0] = 0;
        out_data[1] = 0;
        break;
    case PMU_PWR_SET_POWERUP_EVENTS:
    case PMU_PWR_CLR_POWERUP_EVENTS:
        break;
    case PMU_PWR_GET_WAKEUP_EVENTS:
        *out_len = 2;
        out_data[0] = 0;
        out_data[1] = 0;
        break;
    case PMU_PWR_SET_WAKEUP_EVENTS:
    case PMU_PWR_CLR_WAKEUP_EVENTS:
        break;
    /*
     * The two below are absent from Linux's driver; ApplePMU (10.4,
     * disassembled) sends both during power-management bring-up.
     */
    case PMU_PWR_LAST_SHUTDOWN_CAUSE:
        /*
         * whatCausedLastShutdown() expects one cause byte (bit 0x80 =
         * "the PMU forced the shutdown", which makes it dig further).
         * We never force one, so answer 0: last shutdown was clean.
         */
        *out_len = 1;
        out_data[0] = 0;
        break;
    case PMU_PWR_SERVER_ID:
        /* With a payload byte it is a set, without one a get;
         * either way the current ID is returned. */
        if (in_len >= 2) {
            s->server_id = in_data[1];
        }
        *out_len = 1;
        out_data[0] = s->server_id;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: POWER EVENTS unknown subcommand 0x%02x\n",
                      in_data[0]);
    }
}

/*
 * PRAM lives in the PMU on PMU-equipped machines. Apple's Power Manager
 * equates (PowerPrivEqu.a) name these pramWrite (0x31) / pramRead (0x39)
 * for the legacy 20-byte clock-chip block and xPramWrite (0x32) /
 * xPramRead (0x3a) for the extended 256 bytes. Classic Mac OS reads
 * them during early boot and write-verifies whatever it initializes, so
 * the storage must round-trip.
 */
/*
 * The PMU's battery-backed PRAM (the 256-byte extended block plus the 20-byte
 * legacy clock block) holds per-user settings the guest expects to survive a
 * power cycle: the startup disk, sound volume, mouse/keyboard timings,
 * AppleTalk port, time zone, etc. Persist it to a file, mirroring cuda.c's
 * cuda_default_pram_blk()/cuda_pram_update(). The image layout is xpram[256]
 * followed by pram[20].
 */
#define PMU_PRAM_FILE_SIZE (sizeof(((PMUState *)0)->xpram) + \
                            sizeof(((PMUState *)0)->pram))

static BlockBackend *pmu_default_pram_blk(void)
{
    static const char filename[] = "pram.img";
    struct stat st;
    BlockBackend *blk;
    Error *local_err = NULL;
    QDict *options;
    int fd;

    fd = qemu_open_old(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        warn_report("could not open/create default PRAM image '%s': %s",
                    filename, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) == 0 && st.st_size < (int64_t)PMU_PRAM_FILE_SIZE) {
        if (ftruncate(fd, PMU_PRAM_FILE_SIZE) != 0) {
            warn_report("could not size default PRAM image '%s': %s",
                        filename, strerror(errno));
        }
    }
    close(fd);

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(filename, NULL, options, BDRV_O_RDWR, &local_err);
    if (!blk) {
        warn_report_err(local_err);
        return NULL;
    }
    return blk;
}

static void pmu_pram_update(PMUState *s)
{
    if (!s->pram_blk) {
        return;
    }
    if (blk_pwrite(s->pram_blk, 0, sizeof(s->xpram), s->xpram, 0) < 0 ||
        blk_pwrite(s->pram_blk, sizeof(s->xpram), sizeof(s->pram),
                   s->pram, 0) < 0) {
        qemu_log("pmu_pram_update: cannot write PRAM backing file\n");
    }
}

static void pmu_cmd_pram_write(PMUState *s,
                               const uint8_t *in_data, uint8_t in_len,
                               uint8_t *out_data, uint8_t *out_len)
{
    if (in_len != sizeof(s->pram)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: pramWrite command, invalid len: %d want: 20\n",
                      in_len);
        return;
    }
    memcpy(s->pram, in_data, sizeof(s->pram));
    pmu_pram_update(s);
}

static void pmu_cmd_pram_read(PMUState *s,
                              const uint8_t *in_data, uint8_t in_len,
                              uint8_t *out_data, uint8_t *out_len)
{
    memcpy(out_data, s->pram, sizeof(s->pram));
    *out_len = sizeof(s->pram);
}

static void pmu_cmd_xpram_write(PMUState *s,
                                const uint8_t *in_data, uint8_t in_len,
                                uint8_t *out_data, uint8_t *out_len)
{
    unsigned int addr, count;

    /* variable-length command: offset, count, data bytes */
    if (in_len < 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: xPramWrite command, invalid len: %d\n", in_len);
        return;
    }
    addr = in_data[0];
    count = in_data[1];
    if (count != in_len - 2u || addr + count > sizeof(s->xpram)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: xPramWrite bad range %u+%u (len %d)\n",
                      addr, count, in_len);
        return;
    }
    memcpy(&s->xpram[addr], &in_data[2], count);
    pmu_pram_update(s);
}

static void pmu_cmd_xpram_read(PMUState *s,
                               const uint8_t *in_data, uint8_t in_len,
                               uint8_t *out_data, uint8_t *out_len)
{
    unsigned int addr, count;

    if (in_len != 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: xPramRead command, invalid len: %d want: 2\n",
                      in_len);
        return;
    }
    addr = in_data[0];
    count = in_data[1];
    if (addr + count > sizeof(s->xpram) || count > 128) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: xPramRead bad range %u+%u\n", addr, count);
        return;
    }
    memcpy(out_data, &s->xpram[addr], count);
    *out_len = count;
}

/*
 * Subsystem power control (Apple: powerCntl 0x10 / power1Cntl 0x11) and
 * the volume/brightness pair (0x40/0x41): accepted and remembered
 * nowhere -- none of the controlled subsystems (screen, charger, media
 * bay, backlight) has an equivalent to switch in this machine model.
 * Accepting them silences the unknown-command path for commands classic
 * Mac OS sends during every boot.
 */
static void pmu_cmd_accept(PMUState *s,
                           const uint8_t *in_data, uint8_t in_len,
                           uint8_t *out_data, uint8_t *out_len)
{
}

static void pmu_cmd_get_cover(PMUState *s,
                              const uint8_t *in_data, uint8_t in_len,
                              uint8_t *out_data, uint8_t *out_len)
{
    /* Not 100% sure here, will have to check what a real Mac
     * returns other than byte 0 bit 0 is LID closed on laptops
     */
    *out_len = 1;
    *out_data = 0x00;
}

static void pmu_cmd_download_status(PMUState *s,
                                    const uint8_t *in_data, uint8_t in_len,
                                    uint8_t *out_data, uint8_t *out_len)
{
    /* This has to do with PMU firmware updates as far as I can tell.
     *
     * We return 0x62 which is what OpenPMU expects
     */
    *out_len = 1;
    *out_data = 0x62;
}

static void pmu_cmd_read_pmu_ram(PMUState *s,
                                 const uint8_t *in_data, uint8_t in_len,
                                 uint8_t *out_data, uint8_t *out_len)
{
    if (in_len < 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMU: READ_PMU_RAM command, invalid len %d, expected 3\n",
                      in_len);
        return;
    }

    /*
     * The full PMU version lives in PMU RAM: Mac OS X's ApplePMU
     * getPMUversion() (10.4 kext, disassembled) reads 3 bytes at
     * address 0x80 0x02 on KeyLargo-based machines instead of ever
     * sending GET_VERSION, stores byte 2 as the PMU type, and firmware
     * packs the three bytes into the /via-pmu "pmu-version" device tree
     * property. A real PowerMac3,4's property is 0x00d07e0c, so these
     * three bytes are d0 7e 0c.
     */
    if (in_data[0] == 0x80 && in_data[1] == 0x02 && in_data[2] == 3) {
        *out_len = 3;
        out_data[0] = 0xd0;
        out_data[1] = 0x7e;
        out_data[2] = 0x0c;
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "PMU: Unsupported READ_PMU_RAM, args: %02x %02x %02x\n",
                  in_data[0], in_data[1], in_data[2]);

    *out_len = 0;
}

/* description of commands */
typedef struct PMUCmdHandler {
    uint8_t command;
    const char *name;
    void (*handler)(PMUState *s,
                    const uint8_t *in_args, uint8_t in_len,
                    uint8_t *out_args, uint8_t *out_len);
} PMUCmdHandler;

static const PMUCmdHandler PMUCmdHandlers[] = {
    { PMU_INT_ACK, "INT ACK", pmu_cmd_int_ack },
    { PMU_SET_INTR_MASK, "SET INT MASK", pmu_cmd_set_int_mask },
    { PMU_ADB_CMD, "ADB COMMAND", pmu_cmd_adb },
    { PMU_ADB_POLL_OFF, "ADB POLL OFF", pmu_cmd_adb_poll_off },
    { PMU_RESET, "REBOOT", pmu_cmd_reset },
    { PMU_SHUTDOWN, "SHUTDOWN", pmu_cmd_shutdown },
    { PMU_READ_RTC, "GET RTC", pmu_cmd_get_rtc },
    { PMU_SET_RTC, "SET RTC", pmu_cmd_set_rtc },
    { PMU_PRAM_WRITE, "PRAM WRITE", pmu_cmd_pram_write },
    { PMU_PRAM_READ, "PRAM READ", pmu_cmd_pram_read },
    { PMU_XPRAM_WRITE, "XPRAM WRITE", pmu_cmd_xpram_write },
    { PMU_XPRAM_READ, "XPRAM READ", pmu_cmd_xpram_read },
    { PMU_POWER_CTRL0, "POWER CTRL0", pmu_cmd_accept },
    { PMU_POWER_CTRL, "POWER CTRL", pmu_cmd_accept },
    { PMU_SET_VOLBUTTON, "SET VOLBUTTON", pmu_cmd_accept },
    { PMU_BACKLIGHT_BRIGHT, "SET BACKLIGHT", pmu_cmd_accept },
    { PMU_SYSTEM_READY, "SYSTEM READY", pmu_cmd_system_ready },
    { PMU_GET_VERSION, "GET VERSION", pmu_cmd_get_version },
    { PMU_POWER_EVENTS, "POWER EVENTS", pmu_cmd_power_events },
    { PMU_GET_COVER, "GET_COVER", pmu_cmd_get_cover },
    { PMU_DOWNLOAD_STATUS, "DOWNLOAD STATUS", pmu_cmd_download_status },
    { PMU_READ_PMU_RAM, "READ PMGR RAM", pmu_cmd_read_pmu_ram },
};

static void pmu_dispatch_cmd(PMUState *s)
{
    unsigned int i;

    /* No response by default */
    s->cmd_rsp_sz = 0;

    for (i = 0; i < ARRAY_SIZE(PMUCmdHandlers); i++) {
        const PMUCmdHandler *desc = &PMUCmdHandlers[i];

        if (desc->command != s->cmd) {
            continue;
        }

        trace_pmu_dispatch_cmd(desc->name);
        desc->handler(s, s->cmd_buf, s->cmd_buf_pos,
                      s->cmd_rsp, &s->cmd_rsp_sz);

        if (s->rsplen != -1 && s->rsplen != s->cmd_rsp_sz) {
            trace_pmu_debug_protocol_string("QEMU internal cmd resp mismatch!");
        } else {
            trace_pmu_debug_protocol_resp_size(s->cmd_rsp_sz);
        }

        return;
    }

    trace_pmu_dispatch_unknown_cmd(s->cmd);

    /* Manufacture fake response with 0's */
    if (s->rsplen == -1) {
        s->cmd_rsp_sz = 0;
    } else {
        s->cmd_rsp_sz = s->rsplen;
        memset(s->cmd_rsp, 0, s->rsplen);
    }
}

static void pmu_update(PMUState *s)
{
    MOS6522PMUState *mps = &s->mos6522_pmu;
    MOS6522State *ms = MOS6522(mps);
    ADBBusState *adb_bus = &s->adb_bus;

    /* Only react to changes in reg B */
    if (ms->b == s->last_b) {
        return;
    }
    s->last_b = ms->b;

    /* Check the TREQ / TACK state */
    switch (ms->b & (TREQ | TACK)) {
    case TREQ:
        /* This is an ack release, handle it and bail out */
        ms->b |= TACK;
        s->last_b = ms->b;

        trace_pmu_debug_protocol_string("handshake: TREQ high, setting TACK");
        return;
    case TACK:
        /* This is a valid request, handle below */
        break;
    case TREQ | TACK:
        /* This is an idle state */
        return;
    default:
        /* Invalid state, log and ignore */
        trace_pmu_debug_protocol_error(ms->b);
        return;
    }

    /* If we wanted to handle commands asynchronously, this is where
     * we would delay the clearing of TACK until we are ready to send
     * the response
     */

    /* We have a request, handshake TACK so we don't stay in
     * an invalid state. If we were concurrent with the OS we
     * should only do this after we grabbed the SR but that isn't
     * a problem here.
     */

    trace_pmu_debug_protocol_clear_treq(s->cmd_state);

    ms->b &= ~TACK;
    s->last_b = ms->b;

    /* Act according to state */
    switch (s->cmd_state) {
    case pmu_state_idle:
        if (!(ms->acr & SR_OUT)) {
            trace_pmu_debug_protocol_string("protocol error! "
                                            "state idle, ACR reading");
            break;
        }

        s->cmd = ms->sr;
        via_set_sr_int(s);
        s->cmdlen = pmu_data_len[s->cmd][0];
        s->rsplen = pmu_data_len[s->cmd][1];
        s->cmd_buf_pos = 0;
        s->cmd_rsp_pos = 0;
        s->cmd_state = pmu_state_cmd;

        adb_autopoll_block(adb_bus);
        trace_pmu_debug_protocol_cmd(s->cmd, s->cmdlen, s->rsplen);
        break;

    case pmu_state_cmd:
        if (!(ms->acr & SR_OUT)) {
            trace_pmu_debug_protocol_string("protocol error! "
                                            "state cmd, ACR reading");
            break;
        }

        if (s->cmdlen == -1) {
            trace_pmu_debug_protocol_cmdlen(ms->sr);

            s->cmdlen = ms->sr;
            if (s->cmdlen > sizeof(s->cmd_buf)) {
                trace_pmu_debug_protocol_cmd_toobig(s->cmdlen);
            }
        } else if (s->cmd_buf_pos < sizeof(s->cmd_buf)) {
            s->cmd_buf[s->cmd_buf_pos++] = ms->sr;
        }

        via_set_sr_int(s);
        break;

    case pmu_state_rsp:
        if (ms->acr & SR_OUT) {
            trace_pmu_debug_protocol_string("protocol error! "
                                            "state resp, ACR writing");
            break;
        }

        if (s->rsplen == -1) {
            trace_pmu_debug_protocol_cmd_send_resp_size(s->cmd_rsp_sz);

            ms->sr = s->cmd_rsp_sz;
            s->rsplen = s->cmd_rsp_sz;
        } else if (s->cmd_rsp_pos < s->cmd_rsp_sz) {
            trace_pmu_debug_protocol_cmd_send_resp(s->cmd_rsp_pos, s->rsplen);

            ms->sr = s->cmd_rsp[s->cmd_rsp_pos++];
        }

        via_set_sr_int(s);
        break;
    }

    /* Check for state completion */
    if (s->cmd_state == pmu_state_cmd && s->cmdlen == s->cmd_buf_pos) {
        trace_pmu_debug_protocol_string("Command reception complete, "
                                        "dispatching...");

        pmu_dispatch_cmd(s);
        s->cmd_state = pmu_state_rsp;
    }

    if (s->cmd_state == pmu_state_rsp && s->rsplen == s->cmd_rsp_pos) {
        trace_pmu_debug_protocol_cmd_resp_complete(ms->ier);

        adb_autopoll_unblock(adb_bus);
        s->cmd_state = pmu_state_idle;
    }
}

static uint64_t mos6522_pmu_read(void *opaque, hwaddr addr, unsigned size)
{
    PMUState *s = opaque;
    MOS6522PMUState *mps = &s->mos6522_pmu;
    MOS6522State *ms = MOS6522(mps);

    addr = (addr >> 9) & 0xf;
    return mos6522_read(ms, addr, size);
}

static void mos6522_pmu_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    PMUState *s = opaque;
    MOS6522PMUState *mps = &s->mos6522_pmu;
    MOS6522State *ms = MOS6522(mps);

    addr = (addr >> 9) & 0xf;
    mos6522_write(ms, addr, val, size);
}

static const MemoryRegionOps mos6522_pmu_ops = {
    .read = mos6522_pmu_read,
    .write = mos6522_pmu_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static bool pmu_adb_state_needed(void *opaque)
{
    PMUState *s = opaque;

    return s->has_adb;
}

static const VMStateDescription vmstate_pmu_adb = {
    .name = "pmu/adb",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pmu_adb_state_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(adb_reply_size, PMUState),
        VMSTATE_BUFFER(adb_reply, PMUState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pmu = {
    .name = "pmu",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(mos6522_pmu.parent_obj, PMUState, 0, vmstate_mos6522,
                       MOS6522State),
        VMSTATE_UINT8(last_b, PMUState),
        VMSTATE_UINT8(cmd, PMUState),
        VMSTATE_UINT32(cmdlen, PMUState),
        VMSTATE_UINT32(rsplen, PMUState),
        VMSTATE_UINT8(cmd_buf_pos, PMUState),
        VMSTATE_BUFFER(cmd_buf, PMUState),
        VMSTATE_UINT8(cmd_rsp_pos, PMUState),
        VMSTATE_UINT8(cmd_rsp_sz, PMUState),
        VMSTATE_BUFFER(cmd_rsp, PMUState),
        VMSTATE_UINT8(intbits, PMUState),
        VMSTATE_UINT8(intmask, PMUState),
        VMSTATE_UINT32(tick_offset, PMUState),
        VMSTATE_TIMER_PTR(one_sec_timer, PMUState),
        VMSTATE_INT64(one_sec_target, PMUState),
        VMSTATE_BUFFER_V(pram, PMUState, 2),
        VMSTATE_BUFFER_V(xpram, PMUState, 2),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_pmu_adb,
        NULL
    }
};

static void pmu_reset(DeviceState *dev)
{
    PMUState *s = VIA_PMU(dev);

    /* OpenBIOS needs to do this? MacOS 9 needs it */
    s->intmask = PMU_INT_ADB | PMU_INT_TICK;
    s->intbits = 0;

    s->cmd_state = pmu_state_idle;
}

static void pmu_realize(DeviceState *dev, Error **errp)
{
    PMUState *s = VIA_PMU(dev);
    SysBusDevice *sbd;
    ADBBusState *adb_bus = &s->adb_bus;
    struct tm tm;

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mos6522_pmu), errp)) {
        return;
    }

    /* Pass IRQ from 6522 */
    sbd = SYS_BUS_DEVICE(s);
    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(&s->mos6522_pmu));

    qemu_get_timedate(&tm, 0);
    s->tick_offset = (uint32_t)mktimegm(&tm) + RTC_OFFSET;
    s->one_sec_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, pmu_one_sec_timer, s);
    s->one_sec_target = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000;
    timer_mod(s->one_sec_timer, s->one_sec_target);

    if (s->has_adb) {
        qbus_init(adb_bus, sizeof(*adb_bus), TYPE_ADB_BUS, dev, "adb.0");
        adb_register_autopoll_callback(adb_bus, pmu_adb_poll, s);
    }

    /* Persistent PRAM: attach a default backing file if none was given. */
    if (!s->pram_blk) {
        s->pram_blk = pmu_default_pram_blk();
    }
    if (s->pram_blk) {
        int64_t len = blk_getlength(s->pram_blk);

        if (len < 0) {
            error_setg_errno(errp, -len,
                             "could not get length of PRAM backing image");
            return;
        }
        if (blk_set_perm(s->pram_blk,
                         BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (len >= (int64_t)PMU_PRAM_FILE_SIZE) {
            if (blk_pread(s->pram_blk, 0, sizeof(s->xpram), s->xpram, 0) < 0 ||
                blk_pread(s->pram_blk, sizeof(s->xpram), sizeof(s->pram),
                          s->pram, 0) < 0) {
                error_setg(errp, "could not read PRAM backing image");
                return;
            }
        }
    }
}

static void pmu_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    PMUState *s = VIA_PMU(obj);

    object_property_add_link(obj, "gpio", TYPE_MACIO_GPIO,
                             (Object **) &s->gpio,
                             qdev_prop_allow_set_link_before_realize,
                             0);

    object_initialize_child(obj, "mos6522-pmu", &s->mos6522_pmu,
                            TYPE_MOS6522_PMU);

    memory_region_init_io(&s->mem, obj, &mos6522_pmu_ops, s, "via-pmu",
                          0x2000);
    sysbus_init_mmio(d, &s->mem);
}

static const Property pmu_properties[] = {
    DEFINE_PROP_BOOL("has-adb", PMUState, has_adb, true),
    DEFINE_PROP_DRIVE("drive", PMUState, pram_blk),
};

static void pmu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pmu_realize;
    device_class_set_legacy_reset(dc, pmu_reset);
    dc->vmsd = &vmstate_pmu;
    device_class_set_props(dc, pmu_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo pmu_type_info = {
    .name = TYPE_VIA_PMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PMUState),
    .instance_init = pmu_init,
    .class_init = pmu_class_init,
};

static void mos6522_pmu_portB_write(MOS6522State *s)
{
    MOS6522PMUState *mps = container_of(s, MOS6522PMUState, parent_obj);
    PMUState *ps = container_of(mps, PMUState, mos6522_pmu);

    pmu_update(ps);
}

static void mos6522_pmu_reset_hold(Object *obj, ResetType type)
{
    MOS6522State *ms = MOS6522(obj);
    MOS6522PMUState *mps = container_of(ms, MOS6522PMUState, parent_obj);
    PMUState *s = container_of(mps, PMUState, mos6522_pmu);
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(ms);

    if (mdc->parent_phases.hold) {
        mdc->parent_phases.hold(obj, type);
    }

    /*
     * Both timers count at the VIA clock (4.7MHz/6 = ~783kHz). The old
     * T2 value here, (SCALE_US * 6000) / 4700 = 1276, is the tick
     * PERIOD in nanoseconds stored into a field consumed as a frequency
     * in Hz -- making T2 run ~613x too slow. The Mac OS ROM's
     * Trampoline calibrates the timebase/bus frequencies by counting TB
     * inside ~1ms T2 one-shot windows; with T2 this slow the window
     * never closes within its poll budget and the measured frequencies
     * come out ~700x too small, poisoning the nanokernel's ProcInfo and
     * storming the decrementer (Mac OS 9's grey-screen boot hang).
     * cuda.c received the equivalent fix earlier; this is the PMU copy
     * of the same upstream units mismatch.
     */
    ms->timers[0].frequency = VIA_TIMER_FREQ;
    ms->timers[1].frequency = VIA_TIMER_FREQ;

    s->last_b = ms->b = TACK | TREQ;
}

static void mos6522_pmu_class_init(ObjectClass *oc, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);

    resettable_class_set_parent_phases(rc, NULL, mos6522_pmu_reset_hold,
                                       NULL, &mdc->parent_phases);
    mdc->portB_write = mos6522_pmu_portB_write;
}

static const TypeInfo mos6522_pmu_type_info = {
    .name = TYPE_MOS6522_PMU,
    .parent = TYPE_MOS6522,
    .instance_size = sizeof(MOS6522PMUState),
    .class_init = mos6522_pmu_class_init,
};

static void pmu_register_types(void)
{
    type_register_static(&pmu_type_info);
    type_register_static(&mos6522_pmu_type_info);
}

type_init(pmu_register_types)
