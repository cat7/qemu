/*
 * QEMU Apple BMAC (Big Mac) Ethernet Controller
 *
 * As found in Beige Power Mac G3 and other OldWorld PowerMacs,
 * inside the Heathrow (macio) chip.
 *
 * Based on the register layout documented in the Linux kernel's
 * drivers/net/ethernet/apple/bmac.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_BMAC_H
#define HW_NET_BMAC_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "net/net.h"

typedef struct DBDMAState DBDMAState;

#define TYPE_BMAC "bmac"
OBJECT_DECLARE_SIMPLE_TYPE(BMACState, BMAC)

/* BMAC register space is 0x1000 bytes */
#define BMAC_REG_SIZE       0x1000
#define BMAC_MAX_PACKET     1518

/*
 * Register offsets, from the Linux driver's bmac.h.
 * Registers are 16-bit, spaced at 0x10 byte intervals.
 */

/* Global status and control */
#define BMAC_XIFC           0x000   /* XIF Configuration */
#define   XIFC_TXOE         0x0001  /* TX Output Enable */
#define   XIFC_LBCK         0x0002  /* Loopback enable */
#define   XIFC_MIILB        0x0004  /* MII Loopback */
#define   XIFC_MIIDIS       0x0008  /* MII Buffer Disable */
#define   XIFC_SQEEN        0x0010  /* SQE Test Enable */

#define BMAC_TXFIFOCSR      0x100   /* TX FIFO Control */
#define   TXFIFO_ENABLE     0x0001

#define BMAC_TXTH           0x110   /* TX Threshold */

#define BMAC_RXFIFOCSR      0x120   /* RX FIFO Control */
#define   RXFIFO_ENABLE     0x0001

#define BMAC_MEMADD         0x130   /* Memory Address */
#define BMAC_MEMDATAHI      0x140   /* Memory Data High */
#define BMAC_MEMDATALO      0x150   /* Memory Data Low */

#define BMAC_XCVRIF         0x160   /* Transceiver Interface Control */
#define   XCVRIF_COLLOW     0x0002  /* COL Active Low */
#define   XCVRIF_SERIAL     0x0004  /* Serial Mode */
#define   XCVRIF_CLK        0x0008  /* Clock Bit */
#define   XCVRIF_LINK       0x0100  /* Link Status */

#define BMAC_CHIPID         0x170   /* Chip ID */
#define BMAC_MIFCSR         0x180   /* MII Frame Interface CSR */
#define BMAC_SROMCSR        0x190   /* SROM Control */

#define BMAC_TXPNTR         0x1a0   /* TX Pointer */
#define BMAC_RXPNTR         0x1b0   /* RX Pointer */

#define BMAC_STATUS         0x200   /* Status - read clears */
#define BMAC_INTDISABLE     0x210   /* Interrupt Enable/Disable */

/* Status/Interrupt bits */
#define BMAC_INT_RXDONE     0x00000001  /* Frame Received */
#define BMAC_INT_RXFRMCNT   0x00000002  /* RX Frame Counter Expired */
#define BMAC_INT_RXALIGN    0x00000004  /* RX Align Error Counter Expired */
#define BMAC_INT_RXCRC      0x00000008  /* RX CRC Error Counter Expired */
#define BMAC_INT_RXLEN      0x00000010  /* RX Length Error Counter Expired */
#define BMAC_INT_RXOFLOW    0x00000020  /* RX FIFO Overflow */
#define BMAC_INT_RXCODEVIOL 0x00000040  /* RX Code Violation Counter Expired */
#define BMAC_INT_SQEERR     0x00000080  /* SQE Test Error */
#define BMAC_INT_TXDONE     0x00000100  /* Frame Transmitted */
#define BMAC_INT_TXURUN     0x00000200  /* TX FIFO Underrun */
#define BMAC_INT_TXMAXERR   0x00000400  /* TX Max Packet Size Error */
#define BMAC_INT_TXNORMCOLL 0x00000800  /* TX Normal Collision Counter Expired */
#define BMAC_INT_TXEXCOLL   0x00001000  /* TX Excess Collision Counter Expired */
#define BMAC_INT_TXLATECOLL 0x00002000  /* TX Late Collision Counter Expired */
#define BMAC_INT_TXNETCOLL  0x00004000  /* TX First Collision Counter Expired */
#define BMAC_INT_TXDEFER    0x00008000  /* TX Defer Timer Expired */
#define BMAC_INT_RXTOHOST   0x00010000  /* RX FIFO to Host */

#define BMAC_INT_DISABLEALL 0xffffffff

/* TX Control Registers */
#define BMAC_TXRST          0x420   /* TX Reset */
#define   TXRST_RESET       0x0001

#define BMAC_TXCFG          0x430   /* TX Configuration */
#define   TXCFG_ENABLE      0x0001  /* TX MAC Enable */
#define   TXCFG_SLOWMODE    0x0020  /* Slow Mode */
#define   TXCFG_IGNCOLL     0x0040  /* Ignore Collisions */
#define   TXCFG_NOFCS       0x0080  /* No FCS */
#define   TXCFG_NOBACKOFF   0x0100  /* No Backoff */
#define   TXCFG_FULLDPX     0x0200  /* Full Duplex */
#define   TXCFG_NEVER       0x0400  /* Never Give Up */

#define BMAC_IPG1           0x440   /* Inter-Packet Gap 1 */
#define BMAC_IPG2           0x450   /* Inter-Packet Gap 2 */
#define BMAC_ALIMIT         0x460   /* TX Attempt Limit */
#define BMAC_SLOT           0x470   /* TX Slot Time */
#define BMAC_PALEN          0x480   /* Preamble Length */
#define BMAC_PAPAT          0x490   /* Preamble Pattern */
#define BMAC_TXSFD          0x4a0   /* TX Start Frame Delimiter */
#define BMAC_JAM            0x4b0   /* Jam Size */
#define BMAC_TXMAX          0x4c0   /* TX Max Packet Size */
#define BMAC_TXMIN          0x4d0   /* TX Min Packet Size */
#define BMAC_PAREG          0x4e0   /* TX Peak Attempts */
#define BMAC_DCNT           0x4f0   /* TX Defer Timer */
#define BMAC_NCCNT          0x500   /* TX Normal Collision Counter */
#define BMAC_NTCNT          0x510   /* TX First Collision Counter */
#define BMAC_EXCNT          0x520   /* TX Excess Collision Counter */
#define BMAC_LTCNT          0x530   /* TX Late Collision Counter */
#define BMAC_RSEED          0x540   /* TX Random Seed */
#define BMAC_TXSM           0x550   /* TX State Machine */

/* RX Control Registers */
#define BMAC_RXRST          0x620   /* RX Reset */
#define BMAC_RXCFG          0x630   /* RX Configuration */
#define   RXCFG_ENABLE      0x0001  /* RX MAC Enable */
#define   RXCFG_PADSTRIP    0x0020  /* Strip Pad Bytes */
#define   RXCFG_PROMISC     0x0040  /* Promiscuous Mode */
#define   RXCFG_NOERR       0x0080  /* No Error Check */
#define   RXCFG_CRCNOSTRIP  0x0100  /* Don't Strip CRC */
#define   RXCFG_REJOWN      0x0200  /* Reject Own Packets */
#define   RXCFG_GRPPROM     0x0400  /* Group Promiscuous */
#define   RXCFG_HASHFILT    0x0800  /* Hash Filter Enable */
#define   RXCFG_ADDRFILT    0x1000  /* Address Filter Enable */

#define BMAC_RXMAX          0x640   /* RX Max Packet Size */
#define BMAC_RXMIN          0x650   /* RX Min Packet Size */

#define BMAC_MADD2          0x660   /* MAC Address 2 (high) */
#define BMAC_MADD1          0x670   /* MAC Address 1 (middle) */
#define BMAC_MADD0          0x680   /* MAC Address 0 (low) */

#define BMAC_FRCNT          0x690   /* RX Frame Counter */
#define BMAC_LECNT          0x6a0   /* RX Length Error Counter */
#define BMAC_AECNT          0x6b0   /* RX Align Error Counter */
#define BMAC_FECNT          0x6c0   /* RX CRC Error Counter */
#define BMAC_RXSM           0x6d0   /* RX State Machine */
#define BMAC_RXCV           0x6e0   /* RX Code Violation */

/* Hash Table Registers */
#define BMAC_BHASH3         0x700   /* Hash bits 63-48 */
#define BMAC_BHASH2         0x710   /* Hash bits 47-32 */
#define BMAC_BHASH1         0x720   /* Hash bits 31-16 */
#define BMAC_BHASH0         0x730   /* Hash bits 15-0 */

/* Number of 16-bit registers (space / 0x10) */
#define BMAC_NUM_REGS       0x80

/* MII PHY registers (basic set) */
#define MII_BMCR            0x00    /* Basic Mode Control */
#define MII_BMSR            0x01    /* Basic Mode Status */
#define MII_PHYID1          0x02    /* PHY ID 1 */
#define MII_PHYID2          0x03    /* PHY ID 2 */
#define MII_ANAR            0x04    /* Auto-Neg Advertisement */
#define MII_ANLPAR          0x05    /* Auto-Neg Link Partner Ability */

/* MII BMSR bits */
#define BMSR_LINK           0x0004  /* Link Status */
#define BMSR_ANEGCAPABLE    0x0008  /* Auto-Neg Capable */
#define BMSR_ANEGCOMPLETE   0x0020  /* Auto-Neg Complete */
#define BMSR_10TFD          0x1000  /* 10BASE-T Full Duplex */
#define BMSR_10T            0x0800  /* 10BASE-T */

struct BMACState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    /* Network interface */
    NICState *nic;
    NICConf conf;

    /* Interrupt outputs: main device status, TX DMA, RX DMA */
    qemu_irq irq;
    qemu_irq irq_tx_dma;
    qemu_irq irq_rx_dma;

    /* Registers, stored in canonical (driver-view) 16-bit format */
    uint16_t regs[BMAC_NUM_REGS];

    uint32_t status;
    uint32_t int_mask;

    /* MII PHY state */
    uint16_t phy_regs[32];

    /* MII bit-bang state machine */
    int mii_bit_count;
    uint32_t mii_word;
    bool mii_in_frame;
    int mii_frame_bits;
    uint32_t mii_frame_data;
    int mii_opcode;
    int mii_phy_addr;
    int mii_reg_addr;
    uint16_t mii_read_data;

    /* MAC address, cached from the MADD0/1/2 registers */
    uint8_t mac_addr[6];

    /* Multicast hash filter */
    uint16_t hash_table[4];

    bool link_up;

    /* RX DMA state: parameters saved while waiting for a packet */
    bool rx_dma_waiting;
    uint64_t rx_dma_addr;
    int rx_dma_len;
    void *rx_dma_io; /* DBDMA_io *, saved to complete RX DMA on packet arrival */

    /* SROM (93C46-style serial EEPROM) state */
    uint16_t srom_data[64];
    int srom_state;
    int srom_bit_count;
    uint32_t srom_shift_reg;
    int srom_addr;
    uint16_t srom_read_data;
    int srom_read_bit;
    bool srom_cs_prev;
    bool srom_clk_prev;
    int srom_data_edge_skip;
    bool srom_dummy_pending;
};

/* Called by macio once its DBDMA controller is realized */
void bmac_register_dma(BMACState *s, DBDMAState *dbdma);

#endif /* HW_NET_BMAC_H */
