/*
 * The RP2350's DMA: 16 channels
 *
 * A channel moves its bytes, half-words or words as fast as its DREQ lets
 * it: at once where the other side is always ready (HSTX, which drains at
 * once; a UART's TX; the pacing timers; FORCE), and otherwise as a PIO
 * state machine's FIFO makes room or has data (the PIO kicks the DMA when
 * one of its FIFOs changes).  Reads and writes go over the RP2350's bus,
 * with the increments, ring and byte swap of its CTRL.  The trigger
 * aliases, null triggers, MULTI_CHAN_TRIGGER, CHAIN_TO, TRANS_COUNT's
 * self-trigger and endless modes, IRQ_QUIET, CHAN_ABORT and the four IRQs
 * (DMA_IRQ_0..3 = 10..13) are modelled; the sniffer and the DMA's MPU are
 * kept, not acted on.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "rp2350.h"

#define IRQ_DMA_0           10
#define CTRL_EN             0x1
#define CTRL_INCR_READ      0x10
#define CTRL_INCR_READ_REV  0x20
#define CTRL_INCR_WRITE     0x40
#define CTRL_INCR_WRITE_REV 0x80
#define CTRL_RING_SEL       0x1000
#define CTRL_IRQ_QUIET      (1u << 23)
#define CTRL_BSWAP          (1u << 24)
#define CTRL_BUSY           (1u << 26)
#define CTRL_KEEP           0x03ffffffu         /* BUSY, the errors: not */
#define MODE_TRIGGER_SELF   0x1
#define MODE_ENDLESS        0xf
/*
 * Transfers a kick makes before the CPUs run again: the rest go on a
 * moment later, at about the chip's one transfer a cycle (150 MHz).
 */
#define DMA_BUDGET          65536
#define DMA_BUDGET_NS       (DMA_BUDGET * 20 / 3)

static bool dreq_ready(RP2350State *s, int treq)
{
    if (treq < 24) {                                /* PIOn: TX 0-3, RX 4-7 */
        RP2Pio *p = &s->pio[treq / 8];
        RP2PioSM *sm = &p->sm[treq % 4];
        int depth;

        if ((treq % 8) < 4) {
            depth = sm->shiftctrl & (1u << 30) ? 8 :
                    sm->shiftctrl & (1u << 31) ? 0 : 4;
            return sm->tx_n < depth;
        }
        return sm->rx_n > 0;
    }
    return true;                    /* HSTX, UART TX, timers, FORCE: ready */
}

static void dma_irqs(RP2350State *s)
{
    RP2Dma *d = &s->dma;

    for (int i = 0; i < 4; i++) {
        qemu_set_irq(d->irq[i], ((d->intr | d->intf[i]) & d->inte[i]) != 0);
    }
}

static void dma_trigger(RP2350State *s, int n)
{
    RP2DmaChan *c = &s->dma.ch[n];

    if (!(c->ctrl & CTRL_EN)) {
        return;
    }
    c->count = c->reload;
    c->ctrl |= CTRL_BUSY;
    s->dma.pending |= 1u << n;
}

static uint32_t ring(uint32_t addr, uint32_t next, int size)
{
    uint32_t mask = size ? (1u << size) - 1 : 0xffffffffu;

    return (addr & ~mask) | (next & mask);
}

/* one transfer of channel @n; false when its count is done */
static bool dma_move(RP2350State *s, int n)
{
    RP2DmaChan *c = &s->dma.ch[n];
    int size = 1 << ((c->ctrl >> 2) & 3);
    int ring_size = (c->ctrl >> 8) & 0xf;
    uint32_t v = 0, step;

    address_space_read(&s->dma.as, c->read_addr, MEMTXATTRS_UNSPECIFIED,
                       &v, size);
    v = le32_to_cpu(v);
    if (c->ctrl & CTRL_BSWAP) {
        v = size == 4 ? bswap32(v) : size == 2 ? bswap16(v) : v;
    }
    v = cpu_to_le32(v);
    address_space_write(&s->dma.as, c->write_addr, MEMTXATTRS_UNSPECIFIED,
                        &v, size);
    if (c->ctrl & CTRL_INCR_READ) {
        step = c->ctrl & CTRL_INCR_READ_REV ? -size : size;
        c->read_addr = ring_size && !(c->ctrl & CTRL_RING_SEL)
                       ? ring(c->read_addr, c->read_addr + step, ring_size)
                       : c->read_addr + step;
    }
    if (c->ctrl & CTRL_INCR_WRITE) {
        step = c->ctrl & CTRL_INCR_WRITE_REV ? -size : size;
        c->write_addr = ring_size && (c->ctrl & CTRL_RING_SEL)
                        ? ring(c->write_addr, c->write_addr + step, ring_size)
                        : c->write_addr + step;
    }
    if ((c->count >> 28) == MODE_ENDLESS) {
        return true;
    }
    c->count = deposit32(c->count, 0, 28, (c->count & 0x0fffffff) - 1);
    return (c->count & 0x0fffffff) != 0;
}

static void dma_complete(RP2350State *s, int n)
{
    RP2DmaChan *c = &s->dma.ch[n];
    int chain = (c->ctrl >> 13) & 0xf;

    c->ctrl &= ~CTRL_BUSY;
    if (!(c->ctrl & CTRL_IRQ_QUIET)) {
        s->dma.intr |= 1u << n;
        dma_irqs(s);
    }
    if ((c->reload >> 28) == MODE_TRIGGER_SELF) {
        dma_trigger(s, n);
    }
    if (chain != n) {
        dma_trigger(s, chain);
    }
}

/* every busy channel, as far as its DREQ lets it */
void rp2350_dma_kick(RP2350State *s)
{
    RP2Dma *d = &s->dma;
    int budget = DMA_BUDGET;
    bool moved;

    if (d->running) {
        d->kicked = true;
        return;
    }
    d->running = true;
    do {
        d->kicked = false;
        moved = false;
        for (int n = 0; n < 16; n++) {
            RP2DmaChan *c = &d->ch[n];
            int treq = (c->ctrl >> 17) & 0x3f;

            d->pending &= ~(1u << n);
            if (!(c->ctrl & CTRL_BUSY)) {
                continue;
            }
            if ((c->count & 0x0fffffff) == 0 && (c->count >> 28) != MODE_ENDLESS) {
                dma_complete(s, n);                 /* triggered with none */
                moved = true;
                continue;
            }
            while ((c->ctrl & CTRL_BUSY) && dreq_ready(s, treq)) {
                if (budget-- == 0) {
                    timer_mod(d->later, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              DMA_BUDGET_NS);
                    d->running = false;
                    return;
                }
                moved = true;
                if (!dma_move(s, n)) {
                    dma_complete(s, n);
                    break;
                }
            }
        }
    } while (moved || d->kicked || d->pending);
    d->running = false;
}

/* --------------------------------------------------------- registers */

static uint32_t dma_read(RP2Block *b, hwaddr reg)
{
    RP2350State *s = b->soc;
    RP2Dma *d = &s->dma;
    RP2DmaChan *c;
    uint32_t ints = 0;

    if (reg < 0x400) {
        c = &d->ch[reg / 0x40];
        switch (reg & 0x3f) {
        case 0x00: case 0x14: case 0x28: case 0x3c: return c->read_addr;
        case 0x04: case 0x18: case 0x2c: case 0x34: return c->write_addr;
        case 0x08: case 0x1c: case 0x24: case 0x38: return c->count;
        default: return c->ctrl;                    /* CTRL and its aliases */
        }
    }
    switch (reg) {
    case 0x400:
        return d->intr;
    case 0x404: case 0x414: case 0x424: case 0x434:
        return d->inte[(reg - 0x404) / 0x10];
    case 0x408: case 0x418: case 0x428: case 0x438:
        return d->intf[(reg - 0x408) / 0x10];
    case 0x40c: case 0x41c: case 0x42c: case 0x43c:
        ints = (reg - 0x40c) / 0x10;
        return (d->intr | d->intf[ints]) & d->inte[ints];
    case 0x440 ... 0x44c:
        return d->timer[(reg - 0x440) / 4];
    case 0x454:
        return d->sniff_ctrl;
    case 0x458:
        return d->sniff_data;
    case 0x468:                                     /* N_CHANNELS */
        return 16;
    default:
        return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
    }
}

static void dma_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2350State *s = b->soc;
    RP2Dma *d = &s->dma;
    RP2DmaChan *c;
    int n, field;

    if (reg < 0x400) {
        n = reg / 0x40;
        c = &d->ch[n];
        field = reg & 0x3f;
        switch (field) {
        case 0x00: case 0x14: case 0x28: case 0x3c: c->read_addr = value; break;
        case 0x04: case 0x18: case 0x2c: case 0x34: c->write_addr = value; break;
        case 0x08: case 0x1c: case 0x24: case 0x38: c->reload = value; break;
        default:
            c->ctrl = (c->ctrl & CTRL_BUSY) | (value & CTRL_KEEP & ~CTRL_BUSY);
            break;
        }
        /* the last of each row triggers: CTRL always, the others unless 0 */
        if (field == 0x0c || (value && (field == 0x1c || field == 0x2c ||
                                        field == 0x3c))) {
            dma_trigger(s, n);
            rp2350_dma_kick(s);
        }
        return;
    }
    switch (reg) {
    case 0x400:                                     /* INTR: W1C */
        d->intr &= ~value;
        break;
    case 0x404: case 0x414: case 0x424: case 0x434:
        d->inte[(reg - 0x404) / 0x10] = value & 0xffff;
        break;
    case 0x408: case 0x418: case 0x428: case 0x438:
        d->intf[(reg - 0x408) / 0x10] = value & 0xffff;
        break;
    case 0x40c: case 0x41c: case 0x42c: case 0x43c: /* INTSn: W1C INTR */
        d->intr &= ~value;
        break;
    case 0x440 ... 0x44c:
        d->timer[(reg - 0x440) / 4] = value;
        break;
    case 0x450:                                     /* MULTI_CHAN_TRIGGER */
        for (n = 0; n < 16; n++) {
            if ((value >> n) & 1) {
                dma_trigger(s, n);
            }
        }
        rp2350_dma_kick(s);
        break;
    case 0x454:
        d->sniff_ctrl = value;
        break;
    case 0x458:
        d->sniff_data = value;
        break;
    case 0x464:                                     /* CHAN_ABORT */
        for (n = 0; n < 16; n++) {
            if ((value >> n) & 1) {
                d->ch[n].ctrl &= ~CTRL_BUSY;
            }
        }
        break;
    default:
        if (reg < RP2_REGS * 4) {
            b->regs[reg / 4] = value;
        }
    }
    dma_irqs(s);
}

static void dma_later(void *opaque)
{
    rp2350_dma_kick(opaque);
}

void rp2350_dma_realize(RP2350State *s)
{
    s->dma.later = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_later, s);
    address_space_init(&s->dma.as, &s->bus, "rp2350-dma");
    rp2_block_init(s, &s->dma.regs, "rp2350.dma", 0x50000000, dma_read,
                   dma_write);
    for (int i = 0; i < 4; i++) {
        s->dma.irq[i] = rp2350_irq(s, IRQ_DMA_0 + i);
    }
}

void rp2350_dma_reset(RP2350State *s)
{
    RP2Dma *d = &s->dma;

    memset(d->ch, 0, sizeof(d->ch));
    for (int n = 0; n < 16; n++) {
        d->ch[n].ctrl = n << 13;                    /* CHAIN_TO: itself */
    }
    d->intr = 0;
    memset(d->inte, 0, sizeof(d->inte));
    memset(d->intf, 0, sizeof(d->intf));
    memset(d->timer, 0, sizeof(d->timer));
    d->sniff_ctrl = d->sniff_data = 0;
    d->pending = 0;
    d->running = d->kicked = false;
    timer_del(d->later);
    memset(d->regs.regs, 0, sizeof(d->regs.regs));
}
