#ifndef HW_ESCC_H
#define HW_ESCC_H

#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "hw/core/sysbus.h"
#include "ui/input.h"
#include "qom/object.h"

/* escc.c */
#define TYPE_ESCC "escc"
#define ESCC_SIZE 4

OBJECT_DECLARE_SIMPLE_TYPE(ESCCState, ESCC)

typedef enum {
    escc_chn_a, escc_chn_b,
} ESCCChnID;

typedef enum {
    escc_serial, escc_kbd, escc_mouse,
} ESCCChnType;

#define ESCC_SERIO_QUEUE_SIZE 256

typedef struct {
    uint8_t data[ESCC_SERIO_QUEUE_SIZE];
    int rptr, wptr, count;
} ESCCSERIOQueue;

#define ESCC_SERIAL_REGS 16
typedef struct ESCCChannelState {
    qemu_irq irq;
    uint32_t rxint, txint, rxint_under_svc, txint_under_svc;
    struct ESCCChannelState *otherchn;
    uint32_t reg;
    uint8_t wregs[ESCC_SERIAL_REGS], rregs[ESCC_SERIAL_REGS];
    ESCCSERIOQueue queue;
    CharFrontend chr;
    int e0_mode, led_mode, caps_lock_mode, num_lock_mode;
    int disabled;
    int clock;
    uint32_t vmstate_dummy;
    ESCCChnID chn; /* this channel, A (base+4) or B (base+0) */
    ESCCChnType type;
    uint8_t rx, tx;
    QemuInputHandlerState *hs;
    char *sunkbd_layout;
    int sunmouse_dx;
    int sunmouse_dy;
    int sunmouse_buttons;

    /*
     * DBDMA hookup (PowerMac oldworld only -- see escc_register_dma() in
     * escc.c and macio_oldworld_realize() in hw/misc/macio/macio.c). Kept
     * as plain fields here rather than pulling in hw/ppc/mac_dbdma.h from
     * this shared, non-PPC-specific header, matching how hw/scsi/mesh.h
     * and hw/net/bmac.h store their own DBDMA_io pointers as void *.
     */
    qemu_irq dma_tx_irq;
    qemu_irq dma_rx_irq;
    void *rx_dma_io; /* DBDMA_io *, saved to complete RX DMA on byte arrival */
    bool rx_dma_waiting;

    /*
     * External/Status interrupt latch. The 8530 latches a set of RR0
     * condition bits and raises one interrupt for them; the driver clears
     * the latch with the WR0 "Reset External/Status Interrupts" command.
     * Only the Break condition was ever modelled here.
     */
    uint32_t extint;
} ESCCChannelState;

struct ESCCState {
    SysBusDevice parent_obj;

    struct ESCCChannelState chn[2];
    uint32_t it_shift;
    bool bit_swap;
    MemoryRegion mmio;
    uint32_t disabled;
    uint32_t frequency;
};

/*
 * PowerMac oldworld only -- see hw/misc/macio/macio.c's
 * macio_oldworld_realize(). dbdma is a DBDMAState *, left as void * here
 * to avoid pulling hw/ppc/mac_dbdma.h into this shared, non-PPC-specific
 * header (matching mesh_register_dma()/bmac_register_dma()'s signatures).
 */
void escc_register_dma(ESCCState *s, void *dbdma);

#endif
