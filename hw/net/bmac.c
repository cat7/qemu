/*
 * QEMU Apple BMAC (Big Mac) Ethernet Controller
 *
 * As found in Beige Power Mac G3 and other OldWorld PowerMacs,
 * inside the Heathrow (macio) chip. The BMAC is a 10Mbit Ethernet MAC
 * that transfers packet data via the macio DBDMA controller.
 *
 * Register layout based on the Linux kernel's
 * drivers/net/ethernet/apple/bmac.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ppc/mac_dbdma.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "net/net.h"
#include "net/eth.h"
#include "hw/net/bmac.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "trace.h"
#include <zlib.h>

/* Convert register byte offset to our 16-bit register array index */
#define REG_INDEX(offset)   ((offset) >> 4)

/* MIFCSR bits (canonical/driver order, i.e. post-bswap16) */
#define MIFCSR_CLOCK     0x0001  /* Clock bit */
#define MIFCSR_DATAOUT   0x0002  /* Data out (to PHY) */
#define MIFCSR_DATAIN    0x0008  /* Data in (from PHY) */

/*
 * SROMCSR (serial EEPROM control) bits. Guest drivers disagree on which
 * byte of the register they use: Linux and Mac OS X 10.2 use the low byte
 * (bits 0-3), Mac OS Server 1.2v3 uses the high byte (bits 8-11). We
 * support both, detecting which one is in use from what the guest writes.
 */
#define SROMCSR_CS_LO       0x0001
#define SROMCSR_CLK_LO      0x0002
#define SROMCSR_DOUT_LO     0x0004
#define SROMCSR_DIN_LO      0x0008

#define SROMCSR_CS_HI       0x0100
#define SROMCSR_CLK_HI      0x0200
#define SROMCSR_DOUT_HI     0x0400
#define SROMCSR_DIN_HI      0x0800

#define SROMCSR_ANY_LO      (SROMCSR_CS_LO | SROMCSR_CLK_LO | SROMCSR_DIN_LO)

/* SROM (93C46-style) opcodes */
#define SROM_CMD_READ       0x6

/* SROM state machine states */
#define SROM_STATE_IDLE     0
#define SROM_STATE_OPCODE   1
#define SROM_STATE_ADDRESS  2
#define SROM_STATE_DATA     3

static void bmac_update_irq(BMACState *s)
{
    bool raise = s->status & ~s->int_mask;

    trace_bmac_irq_update(raise, s->status, s->int_mask);
    if (raise) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void bmac_set_status(BMACState *s, uint32_t bits)
{
    s->status |= bits;
    bmac_update_irq(s);
}

/*
 * MII PHY interface
 *
 * The BMAC uses a bit-banged MII interface through the MIFCSR register.
 * We model just enough of a generic 10BASE-T PHY for guest link/speed
 * detection to succeed.
 */
static void bmac_phy_reset(BMACState *s)
{
    memset(s->phy_regs, 0, sizeof(s->phy_regs));

    s->phy_regs[MII_BMCR] = 0x1000; /* Auto-negotiation enabled */
    s->phy_regs[MII_BMSR] = BMSR_10T | BMSR_10TFD |
                             BMSR_ANEGCOMPLETE | BMSR_ANEGCAPABLE;
    if (s->link_up) {
        s->phy_regs[MII_BMSR] |= BMSR_LINK;
    }

    /* Generic PHY ID; BMAC hardware never had a fixed PHY vendor */
    s->phy_regs[MII_PHYID1] = 0x2000;
    s->phy_regs[MII_PHYID2] = 0x5c10;

    s->phy_regs[MII_ANAR] = 0x0021;   /* 10BASE-T half duplex only */
    s->phy_regs[MII_ANLPAR] = 0x0021;
}

static uint16_t bmac_phy_read(BMACState *s, int phy_addr, int reg)
{
    uint16_t val;

    /* BMAC always addresses its PHY at 0 or 31 */
    if ((phy_addr != 0 && phy_addr != 31) || reg >= 32) {
        return 0xffff;
    }

    val = s->phy_regs[reg];
    if (reg == MII_BMSR) {
        val = s->link_up ? (val | BMSR_LINK) : (val & ~BMSR_LINK);
    }
    return val;
}

static void bmac_phy_write(BMACState *s, int phy_addr, int reg, uint16_t val)
{
    if ((phy_addr != 0 && phy_addr != 31) || reg >= 32) {
        return;
    }

    switch (reg) {
    case MII_BMCR:
        if (val & 0x8000) {
            bmac_phy_reset(s);
        } else {
            s->phy_regs[reg] = val & ~0x8000;
        }
        break;
    case MII_BMSR:
        break; /* read-only */
    default:
        s->phy_regs[reg] = val;
        break;
    }
}

/*
 * MDIO bit-bang state machine.
 *
 * MII frame format: 32 bits preamble, 2 bits start (01), 2 bits opcode
 * (10=read, 01=write), 5 bits PHY address, 5 bits register address,
 * 2 bits turnaround, 16 bits data.
 */
static void bmac_mii_clock_edge(BMACState *s, bool data_out)
{
    s->mii_word = (s->mii_word << 1) | (data_out ? 1 : 0);
    s->mii_bit_count++;

    if (s->mii_bit_count >= 32 && !s->mii_in_frame) {
        if ((s->mii_word & 0x3) == 0x1) {
            s->mii_in_frame = true;
            s->mii_frame_bits = 0;
            s->mii_frame_data = 0;
            return; /* start bit consumed, not part of the frame data */
        }
    }

    if (!s->mii_in_frame) {
        return;
    }

    s->mii_frame_data = (s->mii_frame_data << 1) | (data_out ? 1 : 0);
    s->mii_frame_bits++;

    if (s->mii_frame_bits == 12) {
        s->mii_opcode = (s->mii_frame_data >> 10) & 0x3;
        s->mii_phy_addr = (s->mii_frame_data >> 5) & 0x1f;
        s->mii_reg_addr = s->mii_frame_data & 0x1f;

        if (s->mii_opcode == 2) { /* read: prepare reply bits now */
            s->mii_read_data = bmac_phy_read(s, s->mii_phy_addr,
                                              s->mii_reg_addr);
        }
    }

    if (s->mii_frame_bits >= 30) { /* 14-bit header + 16-bit data */
        if (s->mii_opcode == 1) { /* write */
            bmac_phy_write(s, s->mii_phy_addr, s->mii_reg_addr,
                          s->mii_frame_data & 0xffff);
        }
        s->mii_in_frame = false;
        s->mii_frame_bits = 0;
        s->mii_bit_count = 0;
        s->mii_word = 0;
    }
}

static uint16_t bmac_mii_get_data_in(BMACState *s)
{
    if (s->mii_in_frame && s->mii_opcode == 2 && s->mii_frame_bits >= 14) {
        int bit_pos = s->mii_frame_bits - 14;
        if (bit_pos < 16) {
            int bit = (s->mii_read_data >> (15 - bit_pos)) & 1;
            return bit ? MIFCSR_DATAIN : 0;
        }
    }
    if (s->mii_in_frame && s->mii_opcode == 2 &&
        s->mii_frame_bits >= 12 && s->mii_frame_bits < 14) {
        return 0; /* turnaround */
    }
    return MIFCSR_DATAIN; /* idle */
}

/*
 * SROM (93C46-style serial EEPROM) interface, used by guest drivers to
 * read the burned-in MAC address at boot.
 */
static uint8_t bitrev8(uint8_t byte)
{
    byte = ((byte & 0xF0) >> 4) | ((byte & 0x0F) << 4);
    byte = ((byte & 0xCC) >> 2) | ((byte & 0x33) << 2);
    byte = ((byte & 0xAA) >> 1) | ((byte & 0x55) << 1);
    return byte;
}

/* MAC address lives at SROM word offset 10 (byte offset 20), 3 words */
#define SROM_ENET_ADDR_OFFSET 10

static void bmac_srom_init(BMACState *s)
{
    memset(s->srom_data, 0, sizeof(s->srom_data));

    /*
     * The driver bit-reverses each byte it reads back
     * (ea[i] = bitrev8(word_byte)), so store bit-reversed bytes here.
     */
    s->srom_data[SROM_ENET_ADDR_OFFSET + 0] =
        (bitrev8(s->conf.macaddr.a[1]) << 8) | bitrev8(s->conf.macaddr.a[0]);
    s->srom_data[SROM_ENET_ADDR_OFFSET + 1] =
        (bitrev8(s->conf.macaddr.a[3]) << 8) | bitrev8(s->conf.macaddr.a[2]);
    s->srom_data[SROM_ENET_ADDR_OFFSET + 2] =
        (bitrev8(s->conf.macaddr.a[5]) << 8) | bitrev8(s->conf.macaddr.a[4]);
}

static void bmac_srom_reset(BMACState *s)
{
    s->srom_bit_count = 0;
    s->srom_shift_reg = 0;
    s->srom_state = SROM_STATE_IDLE;
    s->srom_addr = 0;
    s->srom_read_data = 0;
    s->srom_read_bit = 0;
    s->srom_data_edge_skip = 0;
    s->srom_dummy_pending = false;
    s->srom_cs_prev = false;
    s->srom_clk_prev = false;
}

static void bmac_srom_clock_edge(BMACState *s, bool cs, bool clk, bool di)
{
    trace_bmac_srom_edge(cs, clk, di, s->srom_state);

    if (cs && !s->srom_cs_prev) {
        bmac_srom_reset(s);
        s->srom_state = SROM_STATE_OPCODE;
    }
    s->srom_cs_prev = cs;

    if (!cs) {
        s->srom_clk_prev = clk;
        return;
    }

    if (clk && !s->srom_clk_prev) { /* rising edge: shift in a bit */
        s->srom_shift_reg = (s->srom_shift_reg << 1) | (di ? 1 : 0);
        s->srom_bit_count++;

        switch (s->srom_state) {
        case SROM_STATE_OPCODE:
            if (s->srom_bit_count >= 3) {
                int opcode = s->srom_shift_reg & 0x7;
                s->srom_shift_reg = 0;
                s->srom_bit_count = 0;
                s->srom_state = (opcode == SROM_CMD_READ) ?
                    SROM_STATE_ADDRESS : SROM_STATE_IDLE;
                trace_bmac_srom_opcode(opcode, s->srom_state);
            }
            break;
        case SROM_STATE_ADDRESS:
            if (s->srom_bit_count >= 6) { /* 93C46: 6-bit address */
                s->srom_addr = s->srom_shift_reg & 0x3f;
                s->srom_read_data = s->srom_data[s->srom_addr];
                trace_bmac_srom_address(s->srom_addr, s->srom_read_data);
                /*
                 * A real 93C46 outputs a leading dummy/start bit (always
                 * 0) before the 16 real data bits, MSB first. An earlier
                 * pass removed this dummy bit to match DingusPPC's
                 * BigMac model (no dummy-bit special case) -- but
                 * disassembling Mac OS 8.5's actual native bmac driver
                 * proved it genuinely peeks this exact bit and aborts
                 * the whole read if it's nonzero, which only a real
                 * dummy 0 bit satisfies (DingusPPC's own driver stack
                 * apparently never exercises that check, so its model
                 * not needing one doesn't mean real hardware doesn't
                 * have one).
                 *
                 * The dummy is tracked as a one-shot flag
                 * (srom_dummy_pending), completely separate from
                 * srom_read_bit, rather than as an extra numeric bit
                 * position (read_bit=17): srom_read_bit is initialized
                 * straight to 16 (the true MSB's position) and is what
                 * the *next real* data read will use once the dummy is
                 * consumed. Folding the dummy into the numeric countdown
                 * (17..1) was tried first and seemed to work for the
                 * abort-check alone, but corrupted every real data bit
                 * read afterward: whichever single edge's decrement got
                 * skipped to keep the dummy read correct also left the
                 * main 16-bit read loop exactly one edge short by the
                 * end, so its capture came out silently doubled (a
                 * clean value << 1) instead of the real 16 bits --
                 * confirmed bit-by-bit against a live register trace on
                 * both a scratch instance and the user's real disk
                 * (00:05:02:12:34:56 read back as 00:0a:04:24:68:ac,
                 * i.e. exactly the expected bytes each shifted left one
                 * bit). Decoupling the dummy from the numeric position
                 * entirely avoids that -- read_bit never needs an
                 * out-of-range placeholder value at all.
                 */
                s->srom_dummy_pending = true;
                s->srom_read_bit = 16;
                s->srom_state = SROM_STATE_DATA;
                s->srom_bit_count = 0;
                s->srom_shift_reg = 0;
                /*
                 * The falling edge that completes this same 6th address
                 * bit's clock cycle (handled just below, in this exact
                 * function call for a rising edge, or the very next call
                 * for a falling edge) is part of finishing the ADDRESS
                 * phase, not a real data-clock cycle -- it must not
                 * advance srom_read_bit, or the dummy-bit read (which
                 * must not itself consume a real bit position) and the
                 * first real data bit after it both end up reading the
                 * same, wrong position.
                 */
                s->srom_data_edge_skip = 1;
            }
            break;
        case SROM_STATE_DATA:
            /*
             * The rising edge right after the dummy-bit read (itself
             * riding on the address-completing falling edge, protected
             * from decrementing by srom_data_edge_skip above) is what
             * ends the dummy phase -- clearing the flag here, rather
             * than as part of that same falling edge, is what keeps
             * srom_read_bit sitting at the true MSB's position (16) for
             * both the dummy read and the first real data read that
             * immediately follows it.
             */
            s->srom_dummy_pending = false;
            break;
        }
    }

    if (!clk && s->srom_clk_prev) { /* falling edge: advance output bit */
        if (s->srom_data_edge_skip > 0) {
            s->srom_data_edge_skip--;
        } else if (s->srom_state == SROM_STATE_DATA && s->srom_read_bit > 0) {
            s->srom_read_bit--;
        }
    }

    s->srom_clk_prev = clk;
}

static uint16_t bmac_srom_get_data(BMACState *s)
{
    if (s->srom_state != SROM_STATE_DATA) {
        return 0;
    }
    if (s->srom_dummy_pending) {
        trace_bmac_srom_getdata(s->srom_read_bit, 0);
        return 0; /* leading dummy/start bit, always 0 */
    }
    if (s->srom_read_bit >= 1 && s->srom_read_bit <= 16) {
        int bit = (s->srom_read_data >> (s->srom_read_bit - 1)) & 1;
        trace_bmac_srom_getdata(s->srom_read_bit, bit);
        /* Set the data-out bit in both low and high byte positions */
        return bit ? (SROMCSR_DOUT_LO | SROMCSR_DOUT_HI) : 0;
    }
    return 0;
}

/*
 * The Mac OS driver writes the MAC address into MADD0/1/2 at init time;
 * mirror that back into mac_addr[] which we use for RX filtering.
 * After bswap16, each register holds (high_byte << 8) | low_byte in the
 * canonical order below.
 */
static void bmac_update_mac_from_regs(BMACState *s)
{
    uint16_t madd0 = s->regs[REG_INDEX(BMAC_MADD0)];
    uint16_t madd1 = s->regs[REG_INDEX(BMAC_MADD1)];
    uint16_t madd2 = s->regs[REG_INDEX(BMAC_MADD2)];

    s->mac_addr[0] = (madd0 >> 8) & 0xff;
    s->mac_addr[1] = madd0 & 0xff;
    s->mac_addr[2] = (madd1 >> 8) & 0xff;
    s->mac_addr[3] = madd1 & 0xff;
    s->mac_addr[4] = (madd2 >> 8) & 0xff;
    s->mac_addr[5] = madd2 & 0xff;
}

static int bmac_hash_index(const uint8_t *mac)
{
    uint32_t crc = crc32(~0, mac, 6);
    return (~crc >> 26) & 0x3f; /* top 6 bits of inverted CRC */
}

static bool bmac_hash_filter_match(BMACState *s, const uint8_t *mac)
{
    int idx = bmac_hash_index(mac);
    return (s->hash_table[idx >> 4] >> (idx & 0xf)) & 1;
}

static bool bmac_can_receive_packet(BMACState *s, const uint8_t *buf)
{
    uint16_t rxcfg = s->regs[REG_INDEX(BMAC_RXCFG)];

    if (!(rxcfg & RXCFG_ENABLE)) {
        return false;
    }
    if (rxcfg & RXCFG_PROMISC) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return true;
    }
    if (buf[0] & 0x01) { /* multicast */
        if (rxcfg & RXCFG_GRPPROM) {
            return true;
        }
        if (rxcfg & RXCFG_HASHFILT) {
            return bmac_hash_filter_match(s, buf);
        }
        return false;
    }
    return memcmp(buf, s->mac_addr, 6) == 0;
}

static bool bmac_can_receive(NetClientState *nc)
{
    BMACState *s = qemu_get_nic_opaque(nc);

    /*
     * Must also gate on rx_dma_waiting: bmac_receive() silently drops
     * any packet that arrives before the driver has armed the next RX
     * descriptor (returns size without ever writing/completing it).
     * Reporting "can receive" from RXCFG_ENABLE alone told the net
     * layer it was always safe to deliver immediately, so any packet
     * landing in the gap between one RX completion and the driver
     * re-arming the ring (e.g. an ARP reply or DHCP offer during
     * negotiation) was lost with no retry -- qemu_flush_queued_packets()
     * is already called from bmac_rx_dma_rw() once a descriptor is
     * armed, so gating here lets the net layer's normal queuing take
     * over instead.
     */
    bool enabled = (s->regs[REG_INDEX(BMAC_RXCFG)] & RXCFG_ENABLE) != 0;
    trace_bmac_can_receive(enabled, s->rx_dma_waiting);
    return enabled && s->rx_dma_waiting;
}

static ssize_t bmac_receive(NetClientState *nc, const uint8_t *buf,
                            size_t size)
{
    BMACState *s = qemu_get_nic_opaque(nc);
    DBDMA_io *io = s->rx_dma_io;
    uint8_t frame_buf[BMAC_MAX_PACKET + 4 + 2];
    size_t total_size;
    uint16_t rxcfg = s->regs[REG_INDEX(BMAC_RXCFG)];
    uint16_t frame_len, rx_pkt_status;

    trace_bmac_rx_receive(size, bmac_can_receive_packet(s, buf),
                          s->rx_dma_waiting, io != NULL);
    if (!bmac_can_receive_packet(s, buf) || !s->rx_dma_waiting || !io) {
        return size;
    }

    /*
     * frame_buf is sized for a maximum-length frame plus CRC and status
     * trailer; a real BMAC rejects frames beyond RXMAX (1518) anyway.
     * Without this bound an oversized frame from the net layer overruns
     * the stack buffer -- host memory corruption, not just a guest bug.
     */
    if (size > BMAC_MAX_PACKET) {
        return size;
    }

    /*
     * Frame layout the driver expects in the DMA buffer:
     *   [ethernet frame] [4-byte CRC, if RXCFG_CRCNOSTRIP] [2-byte
     *   rxPktStatus, big-endian, low bits = frame length]
     */
    memcpy(frame_buf, buf, size);
    total_size = size;

    if (rxcfg & RXCFG_CRCNOSTRIP) {
        uint32_t crc = crc32(~0, buf, size) ^ ~0;
        frame_buf[total_size++] = crc & 0xff;
        frame_buf[total_size++] = (crc >> 8) & 0xff;
        frame_buf[total_size++] = (crc >> 16) & 0xff;
        frame_buf[total_size++] = (crc >> 24) & 0xff;
        frame_len = (uint16_t)(size + 4);
    } else {
        frame_len = (uint16_t)size;
    }
    rx_pkt_status = frame_len; /* no error bits (abort = 0) */
    frame_buf[total_size++] = (rx_pkt_status >> 8) & 0xff;
    frame_buf[total_size++] = rx_pkt_status & 0xff;

    if (total_size > s->rx_dma_len) {
        total_size = s->rx_dma_len;
    }
    dma_memory_write(&address_space_memory, s->rx_dma_addr, frame_buf,
                      total_size, MEMTXATTRS_UNSPECIFIED);

    s->rx_dma_waiting = false;
    s->rx_dma_io = NULL;

    if (io->dma_end) {
        io->len = s->rx_dma_len - total_size; /* residual */
        if (io->channel) {
            DBDMA_channel *ch = io->channel;
            ch->regs[DBDMA_STATUS] |= 0x01; /* DEVSTAT bit 0 */
        }
        io->dma_end(io);
    }

    bmac_set_status(s, BMAC_INT_RXDONE);
    s->regs[REG_INDEX(BMAC_FRCNT)]++;

    return size;
}

static void bmac_set_link_status(NetClientState *nc)
{
    BMACState *s = qemu_get_nic_opaque(nc);

    s->link_up = !nc->link_down;
    /*
     * Real link status is exposed to the guest driver via TWO independent
     * hardware paths, not one:
     *
     *  1. The MII PHY's standard BMSR_LINK bit, read via BMAC_MIFCSR's
     *     bit-banged MII interface (MDIO/MDC).
     *  2. XCVRIF_LINK (BMAC_XCVRIF bit 0x0100). The real Beige G3 (Gossamer)
     *     board schematic (main logic board, sheet 12 "Heathrow I/O ASIC"
     *     and sheet 13 "Ethernet: Transceiver, Magnetics, Connector,
     *     Address PROM") shows Heathrow's MDC pin is NOT CONNECTED on this
     *     board -- the MDIO management interface is electrically disabled,
     *     so path (1) can never actually be used by any real driver on
     *     real hardware. Instead, the LXT907 PHY's dedicated link-status
     *     output (pin 20, "LEDL"/ETH10BT_LINK) is wired directly into
     *     Heathrow's RX_ER input pin, which Heathrow's BMAC block reflects
     *     through XCVR_IF -- confirming this bit is a real, load-bearing
     *     hardware signal, not a fabrication.
     *
     * An EARLIER version of this fix forced XCVRIF_LINK permanently on
     * (and re-forced it on every register write), which was wrong for a
     * different reason: it stomped the bit back to 1 immediately even
     * when the guest tried to write/poll for it clearing, breaking the
     * driver's real init-time write-then-poll sequence. The correct model
     * (matching how BMSR_LINK is already handled below) is to compute
     * this bit fresh at *read* time from live s->link_up, never storing
     * or re-forcing it on writes -- see bmac_read()'s BMAC_XCVRIF case,
     * which also confirms (via live testing) that this bit reads
     * active-low (0 = link present), not active-high.
     */
    if (s->link_up) {
        s->phy_regs[MII_BMSR] |= BMSR_LINK;
    } else {
        s->phy_regs[MII_BMSR] &= ~BMSR_LINK;
    }
}

static uint64_t bmac_read(void *opaque, hwaddr addr, unsigned size)
{
    BMACState *s = BMAC(opaque);
    uint64_t val = 0;

    switch (addr) {
    case BMAC_STATUS:
        val = s->status;
        s->status = 0; /* reading status clears it */
        bmac_update_irq(s);
        break;

    case BMAC_INTDISABLE:
        val = s->int_mask;
        break;

    case BMAC_MIFCSR:
        val = s->regs[REG_INDEX(addr)] & ~MIFCSR_DATAIN;
        val |= bmac_mii_get_data_in(s);
        break;

    case BMAC_SROMCSR:
        val = s->regs[REG_INDEX(addr)] & ~(SROMCSR_DOUT_LO | SROMCSR_DOUT_HI);
        val |= bmac_srom_get_data(s);
        break;

    case BMAC_XCVRIF:
        /*
         * XCVRIF_LINK reflects the real board's PHY-link-status wire (see
         * bmac_set_link_status() for the schematic-derived reasoning) --
         * computed fresh from live link state on every read, never stored,
         * so a guest poll sees a real value instead of a permanently-forced
         * bit. Active-low (0 = link present), confirmed by live testing:
         * with this bit active-high the guest driver reads it exactly 7
         * times then permanently aborts its hardware-init sequence
         * (TXCFG/RXCFG/the hash table never get programmed, matching a
         * previously-documented regression); with it active-low the driver
         * reads it exactly once, is satisfied, and completes full
         * hardware init every time thereafter -- consistent with
         * XCVRIF_COLLOW (this same register's other status bit) also being
         * documented as active-low.
         */
        val = s->regs[REG_INDEX(addr)] & ~XCVRIF_LINK;
        if (!s->link_up) {
            val |= XCVRIF_LINK;
        }
        break;

    default:
        if (addr < BMAC_REG_SIZE) {
            int idx = REG_INDEX(addr);
            if (idx < BMAC_NUM_REGS) {
                val = s->regs[idx];
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bmac: read from unknown register 0x%"HWADDR_PRIx"\n",
                          addr);
        }
        break;
    }

    /*
     * BMAC registers are accessed by Mac OS drivers using byte-reversed
     * half-word loads/stores (sthbrx/lhbrx) because the hardware is
     * natively little-endian; DEVICE_BIG_ENDIAN MMIO plus this bswap16
     * reproduces that swap for the bus.
     */
    val = bswap16(val & 0xffff);
    trace_bmac_reg_read(addr, val, size);
    return val;
}

static void bmac_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    BMACState *s = BMAC(opaque);

    val = bswap16(val & 0xffff);
    trace_bmac_reg_write(addr, val, size);

    switch (addr) {
    case BMAC_STATUS:
        s->status &= ~val; /* writing 1 bits clears them */
        bmac_update_irq(s);
        return;

    case BMAC_INTDISABLE:
        s->int_mask = val;
        bmac_update_irq(s);
        return;

    case BMAC_TXRST:
        s->regs[REG_INDEX(BMAC_TXRST)] = 0; /* reset self-clears */
        return;

    case BMAC_RXRST:
        /*
         * An RX-engine reset also invalidates any armed-but-unfilled RX
         * DMA arm. The OS driver's init-time RXRST is the only thing
         * standing between whatever the ROM (or a previous session) left
         * armed and the new system's memory layout: without this, the
         * next packet slirp delivers is DMA-written through the STALE
         * address saved at arm time -- silent memory corruption in
         * whatever the new boot placed there.
         */
        s->rx_dma_waiting = false;
        s->rx_dma_io = NULL;
        s->regs[REG_INDEX(BMAC_RXRST)] = 0;
        return;

    case BMAC_MADD0:
    case BMAC_MADD1:
    case BMAC_MADD2:
        s->regs[REG_INDEX(addr)] = val;
        bmac_update_mac_from_regs(s);
        return;

    case BMAC_BHASH0:
    case BMAC_BHASH1:
    case BMAC_BHASH2:
    case BMAC_BHASH3:
        /*
         * BHASH0 (bits 15-0) sits at the HIGHEST address (0x730) and
         * BHASH3 (bits 63-48) at the LOWEST (0x700) -- addresses count
         * down as significance goes up. hash_table[] is indexed the
         * other way (index 0 = bits 15-0, per bmac_hash_index()'s
         * hash_table[idx>>4]), so the reference point for this
         * computation must be BMAC_BHASH0 (the highest address, mapping
         * to the lowest index), not BMAC_BHASH3. Using BMAC_BHASH3 here
         * only produced a correct (zero) index for BHASH3 itself --
         * addr is unsigned (hwaddr), so BMAC_BHASH3 - addr underflows
         * for BHASH0/1/2 into a huge wrapped index, corrupting memory
         * far outside the 4-entry hash_table[] array. This meant only
         * 16 of the real 64 hash-filter bits (BHASH3's) were ever
         * actually landing in hash_table[] correctly -- a likely cause
         * of multicast traffic silently not matching the filter.
         */
        s->hash_table[(BMAC_BHASH0 - addr) / 0x10] = val;
        s->regs[REG_INDEX(addr)] = val;
        return;

    case BMAC_MIFCSR:
        {
            uint16_t old_val = s->regs[REG_INDEX(addr)];
            bool old_clock = (old_val & MIFCSR_CLOCK) != 0;
            bool new_clock = (val & MIFCSR_CLOCK) != 0;

            s->regs[REG_INDEX(addr)] = val;
            if (!old_clock && new_clock) {
                bmac_mii_clock_edge(s, (val & MIFCSR_DATAOUT) != 0);
            }
        }
        return;

    case BMAC_SROMCSR:
        {
            bool cs, clk, di;

            if (val & SROMCSR_ANY_LO) {
                cs = (val & SROMCSR_CS_LO) != 0;
                clk = (val & SROMCSR_CLK_LO) != 0;
                di = (val & SROMCSR_DIN_LO) != 0;
            } else {
                cs = (val & SROMCSR_CS_HI) != 0;
                clk = (val & SROMCSR_CLK_HI) != 0;
                di = (val & SROMCSR_DIN_HI) != 0;
            }
            s->regs[REG_INDEX(addr)] = val;
            bmac_srom_clock_edge(s, cs, clk, di);
        }
        return;

    case BMAC_XCVRIF:
        /*
         * Store whatever the guest writes; XCVRIF_LINK is recomputed
         * fresh from live link state on every READ instead (see
         * bmac_read()'s case and bmac_set_link_status()), so it doesn't
         * matter whether the guest's write included that bit or not.
         */
        s->regs[REG_INDEX(addr)] = val;
        return;

    default:
        if (addr < BMAC_REG_SIZE) {
            int idx = REG_INDEX(addr);
            if (idx < BMAC_NUM_REGS) {
                s->regs[idx] = val;
            }
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bmac: write to unknown register 0x%"HWADDR_PRIx"\n",
                          addr);
        }
        break;
    }
}

static const MemoryRegionOps bmac_ops = {
    .read = bmac_read,
    .write = bmac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
};

static void bmac_reset(DeviceState *dev)
{
    BMACState *s = BMAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->status = 0;
    s->int_mask = BMAC_INT_DISABLEALL;

    s->mii_bit_count = 0;
    s->mii_word = 0;
    s->mii_in_frame = false;
    s->mii_frame_bits = 0;
    s->mii_frame_data = 0;
    s->mii_opcode = 0;
    s->mii_phy_addr = 0;
    s->mii_reg_addr = 0;
    s->mii_read_data = 0;

    bmac_srom_reset(s);

    /*
     * Beige G3 (Gossamer) is a Heathrow-based machine (this project's own
     * hw/intc/heathrow_pic.c) -- Paddington is a later macio revision
     * (iMac rev B, Lombard PowerBook) that didn't exist yet in 1997.
     * Real chip IDs confirmed against DingusPPC's real-hardware-derived
     * BigMac model (devices/ethernet/bigmac.h): Heathrow=0xB1,
     * Paddington=0xC7 -- the previous 0xC4 here was wrong even for
     * Paddington (transcription error), and Paddington was the wrong
     * identity for this machine regardless. Real native BigMac drivers
     * branch PHY/init handling on this exact ID (see DingusPPC's own
     * bigmac.cpp: "if (chip_id == Paddington) {...} else {// assume
     * Heathrow with LXT907 PHY}") -- reporting the wrong family here is
     * a very plausible reason a real driver would silently take a wrong
     * init path and never fully attach, independent of any individual
     * register being otherwise correct.
     */
    s->regs[REG_INDEX(BMAC_CHIPID)] = 0x00b1; /* Heathrow BMAC */
    s->regs[REG_INDEX(BMAC_IPG1)] = 0x0008;
    s->regs[REG_INDEX(BMAC_IPG2)] = 0x0004;
    s->regs[REG_INDEX(BMAC_ALIMIT)] = 0x0010;
    s->regs[REG_INDEX(BMAC_SLOT)] = 0x0040;
    s->regs[REG_INDEX(BMAC_PALEN)] = 0x0007;
    s->regs[REG_INDEX(BMAC_PAPAT)] = 0x00aa;
    s->regs[REG_INDEX(BMAC_TXSFD)] = 0x00ab;
    s->regs[REG_INDEX(BMAC_JAM)] = 0x0004;
    s->regs[REG_INDEX(BMAC_TXMAX)] = 0x05ee; /* 1518 bytes */
    s->regs[REG_INDEX(BMAC_TXMIN)] = 0x0040;
    s->regs[REG_INDEX(BMAC_RXMAX)] = 0x05ee;
    s->regs[REG_INDEX(BMAC_RXMIN)] = 0x0040;

    s->regs[REG_INDEX(BMAC_MADD0)] =
        (s->conf.macaddr.a[0] << 8) | s->conf.macaddr.a[1];
    s->regs[REG_INDEX(BMAC_MADD1)] =
        (s->conf.macaddr.a[2] << 8) | s->conf.macaddr.a[3];
    s->regs[REG_INDEX(BMAC_MADD2)] =
        (s->conf.macaddr.a[4] << 8) | s->conf.macaddr.a[5];
    bmac_update_mac_from_regs(s);

    s->link_up = true;
    bmac_phy_reset(s);
    s->regs[REG_INDEX(BMAC_XCVRIF)] = XCVRIF_CLK | XCVRIF_SERIAL;
}

static NetClientInfo net_bmac_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = bmac_can_receive,
    .receive = bmac_receive,
    .link_status_changed = bmac_set_link_status,
};

static void bmac_realize(DeviceState *dev, Error **errp)
{
    BMACState *s = BMAC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &bmac_ops, s,
                          "bmac", BMAC_REG_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->irq_tx_dma);
    sysbus_init_irq(sbd, &s->irq_rx_dma);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_bmac_info, &s->conf,
                          object_get_typename(OBJECT(dev)),
                          dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    bmac_srom_init(s);
}

static void bmac_unrealize(DeviceState *dev)
{
    BMACState *s = BMAC(dev);

    qemu_del_nic(s->nic);
}

/* TX DMA callback: DBDMA has packet data ready for us to transmit */
static void bmac_tx_dma_rw(DBDMA_io *io)
{
    BMACState *s = io->opaque;
    uint8_t buf[BMAC_MAX_PACKET];

    if (io->is_dma_out && io->len > 0 && io->len <= BMAC_MAX_PACKET) {
        dma_memory_read(&address_space_memory, io->addr, buf, io->len,
                        MEMTXATTRS_UNSPECIFIED);

        if (memcmp(buf, s->mac_addr, 6) == 0) {
            /* Destination is our own MAC: loop back to the RX path */
            trace_bmac_tx_send(io->len, true);
            bmac_receive(qemu_get_queue(s->nic), buf, io->len);
        } else {
            trace_bmac_tx_send(io->len, false);
            qemu_send_packet(qemu_get_queue(s->nic), buf, io->len);
        }

        io->len = 0;
        bmac_set_status(s, BMAC_INT_TXDONE);
    } else {
        io->len = 0;
    }

    if (io->dma_end) {
        io->dma_end(io);
    }
}

static void bmac_tx_dma_flush(DBDMA_io *io)
{
}

/* RX DMA callback: DBDMA has a buffer ready to receive an incoming packet */
static void bmac_rx_dma_rw(DBDMA_io *io)
{
    BMACState *s = io->opaque;

    if (io->is_dma_out) {
        io->len = 0;
        if (io->dma_end) {
            io->dma_end(io);
        }
        return;
    }

    /*
     * Save the DMA parameters; bmac_receive() completes this transfer
     * (and calls io->dma_end) once a packet actually arrives.
     */
    s->rx_dma_waiting = true;
    s->rx_dma_addr = io->addr;
    s->rx_dma_len = io->len;
    trace_bmac_rx_dma_armed(io->addr, io->len);
    s->rx_dma_io = io;

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static void bmac_rx_dma_flush(DBDMA_io *io)
{
    BMACState *s = io->opaque;

    s->rx_dma_waiting = false;
    s->rx_dma_io = NULL;
}

/*
 * Real hardware wires BMAC's TX/RX DBDMA channels at fixed, ROM-known
 * addresses -- confirmed directly from this exact machine's device-tree
 * dump (SourceFiles/G3/PowerMacG3-device-tree.txt, pci/mac-io/bmac's
 * "reg" property: 0x8200/0x8300, i.e. channels (addr-0x8000)/0x100 = 2
 * and 3). Since mac_oldworld.c builds no QEMU-side device-tree at all
 * (the real ROM's own Open Firmware supplies one from its own hardcoded
 * knowledge, not a dynamically-read property), the guest's driver
 * expects these exact channel numbers, not merely "some unused slot" --
 * channels 4/6 actually belong to escc's ch-a (modem, channel 4) and
 * ch-b (printer, channel 6) per the same device-tree dump, so the
 * previous choice collided with the wrong real device's real address.
 */
#define BMAC_TX_DMA_CHANNEL 2
#define BMAC_RX_DMA_CHANNEL 3

void bmac_register_dma(BMACState *s, DBDMAState *dbdma)
{
    DBDMA_register_channel(dbdma, BMAC_TX_DMA_CHANNEL, s->irq_tx_dma,
                           bmac_tx_dma_rw, bmac_tx_dma_flush, s);
    DBDMA_register_channel(dbdma, BMAC_RX_DMA_CHANNEL, s->irq_rx_dma,
                           bmac_rx_dma_rw, bmac_rx_dma_flush, s);
}

static const Property bmac_properties[] = {
    DEFINE_NIC_PROPERTIES(BMACState, conf),
};

static const VMStateDescription vmstate_bmac = {
    .name = "bmac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, BMACState, BMAC_NUM_REGS),
        VMSTATE_UINT32(status, BMACState),
        VMSTATE_UINT32(int_mask, BMACState),
        VMSTATE_UINT16_ARRAY(phy_regs, BMACState, 32),
        VMSTATE_UINT16_ARRAY(hash_table, BMACState, 4),
        VMSTATE_BOOL(link_up, BMACState),
        VMSTATE_MACADDR(conf.macaddr, BMACState),
        VMSTATE_END_OF_LIST()
    }
};

static void bmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = bmac_realize;
    dc->unrealize = bmac_unrealize;
    device_class_set_legacy_reset(dc, bmac_reset);
    dc->vmsd = &vmstate_bmac;
    device_class_set_props(dc, bmac_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bmac_info = {
    .name          = TYPE_BMAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BMACState),
    .class_init    = bmac_class_init,
};

static void bmac_register_types(void)
{
    type_register_static(&bmac_info);
}

type_init(bmac_register_types)
