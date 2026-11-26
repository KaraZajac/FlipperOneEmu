/*
 * The RP2350's I2C0 and I2C1: Synopsys DW_apb_i2c, master and slave
 *
 * As a master (CON.MASTER_MODE), each has a QEMU I2C bus (rp2350_i2c_bus())
 * that the board's chips are put on. A command written to DATA_CMD is done
 * at once on it: a START (or a repeated one, for RESTART or a change of
 * direction) to TAR's address, then the byte sent or a byte received into
 * the RX FIFO, then a STOP if the command asks for one. So the TX FIFO is
 * always empty (TX_EMPTY while enabled). An address or a byte that no chip
 * ACKs is an abort, as the controller does it: TX_ABRT with its source, a
 * STOP, the commands after it flushed until CLR_TX_ABRT is read.
 *
 * Disabling the controller does not end a transfer that has had no STOP:
 * the Pico SDK disables it, sets TAR and enables it again between a
 * register's address (written without a STOP) and the read after it,
 * which comes with RESTART.
 *
 * As a slave (MASTER_MODE and SLAVE_DISABLE clear), it answers SAR for a
 * master that reaches it through rp2350_i2c_target_*() (the RK3576's link,
 * on I2C1): START_DET at each START, RESTART_DET too at a repeated one; the
 * bytes written to it into the RX FIFO, the first of each transfer flagged
 * (DATA_CMD's FIRST_DATA_BYTE, bit 11, which the firmware frames its
 * transfers by), SCL held while the FIFO is full; a read takes bytes the CPU
 * wrote to DATA_CMD, and with none there raises RD_REQ (once a byte) and
 * holds SCL until it writes one; the master's NACK of its last byte is
 * RX_DONE; its STOP is STOP_DET. Bytes left over from a read are flushed
 * when the next read starts, with TX_ABRT (ABRT_SLVFLUSH_TXFIFO). In master
 * receive too the first byte after the address is flagged.
 *
 * A master that SCL was held on is told when it is let go (a byte read from
 * the RX FIFO, one written for it, the FIFOs flushed): the bus goes on then,
 * without the master having to look again and again.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "rp2350.h"

#define IRQ_I2C0            36

enum {
    R_CON = 0x00, R_TAR = 0x04, R_SAR = 0x08, R_DATA_CMD = 0x10,
    R_INTR_STAT = 0x2c, R_INTR_MASK = 0x30, R_RAW_INTR_STAT = 0x34,
    R_RX_TL = 0x38, R_TX_TL = 0x3c, R_CLR_INTR = 0x40, R_CLR_RX_UNDER = 0x44,
    R_CLR_RX_OVER = 0x48, R_CLR_TX_OVER = 0x4c, R_CLR_RD_REQ = 0x50,
    R_CLR_TX_ABRT = 0x54, R_CLR_RX_DONE = 0x58, R_CLR_ACTIVITY = 0x5c,
    R_CLR_STOP_DET = 0x60, R_CLR_START_DET = 0x64, R_CLR_GEN_CALL = 0x68,
    R_ENABLE = 0x6c, R_STATUS = 0x70, R_TXFLR = 0x74, R_RXFLR = 0x78,
    R_TX_ABRT_SOURCE = 0x80, R_ENABLE_STATUS = 0x9c, R_CLR_RESTART_DET = 0xa8,
    R_COMP_PARAM_1 = 0xf4, R_COMP_VERSION = 0xf8, R_COMP_TYPE = 0xfc,
};

/* CON */
#define CON_MASTER_MODE     (1u << 0)
#define CON_SLAVE_DISABLE   (1u << 6)

/* RAW_INTR_STAT */
#define RX_UNDER        (1u << 0)
#define RX_OVER         (1u << 1)
#define RX_FULL         (1u << 2)
#define TX_EMPTY        (1u << 4)
#define RD_REQ          (1u << 5)
#define TX_ABRT         (1u << 6)
#define RX_DONE         (1u << 7)
#define ACTIVITY        (1u << 8)
#define STOP_DET        (1u << 9)
#define START_DET       (1u << 10)
#define RESTART_DET     (1u << 12)
#define CLEARABLE       (RX_UNDER | RX_OVER | RD_REQ | TX_ABRT | RX_DONE | \
                         ACTIVITY | STOP_DET | START_DET | RESTART_DET)

/* TX_ABRT_SOURCE */
#define ABRT_7B_ADDR_NOACK  (1u << 0)
#define ABRT_TXDATA_NOACK   (1u << 3)
#define ABRT_SLVFLUSH_TXFIFO (1u << 13)
#define ABRT_USER_ABRT      (1u << 16)

/* DATA_CMD */
#define CMD_READ        (1u << 8)
#define CMD_STOP        (1u << 9)
#define CMD_RESTART     (1u << 10)
#define FIRST_DATA_BYTE (1u << 11)

#define FIFO_DEPTH      16

static RP2I2c *i2c_of(RP2Block *b)
{
    return container_of(b, RP2I2c, regs);
}

static uint32_t reg(RP2I2c *c, hwaddr offset)
{
    return c->regs.regs[offset / 4];
}

static bool enabled(RP2I2c *c)
{
    return reg(c, R_ENABLE) & 1;
}

static bool slave_mode(RP2I2c *c)
{
    return enabled(c) &&
           !(reg(c, R_CON) & (CON_MASTER_MODE | CON_SLAVE_DISABLE));
}

static uint32_t raw_intr(RP2I2c *c)
{
    uint32_t raw = c->raw;

    if (slave_mode(c)) {
        if (c->tx_n <= (int)(reg(c, R_TX_TL) & 0xff)) {
            raw |= TX_EMPTY;
        }
    } else if (enabled(c) || c->active) {
        raw |= TX_EMPTY;                /* its commands are done at once */
    }
    if (c->rx_n > (int)(reg(c, R_RX_TL) & 0xff)) {
        raw |= RX_FULL;
    }
    return raw;
}

static void i2c_irq(RP2I2c *c)
{
    qemu_set_irq(c->irq, (raw_intr(c) & reg(c, R_INTR_MASK)) != 0);
}

static bool rx_push(RP2I2c *c, uint16_t v)
{
    if (c->rx_n == FIFO_DEPTH) {
        return false;
    }
    c->rx[(c->rx_head + c->rx_n++) % FIFO_DEPTH] = v;
    return true;
}

/* ------------------------------------------------------- as a master */

static void i2c_stop(RP2I2c *c)
{
    if (c->active) {
        i2c_end_transfer(c->bus);
        c->active = false;
    }
    c->raw |= STOP_DET;
}

static void i2c_abort(RP2I2c *c, uint32_t source)
{
    c->abrt |= source;
    c->raw |= TX_ABRT;
    i2c_stop(c);
}

static void i2c_command(RP2I2c *c, uint32_t v)
{
    bool read = v & CMD_READ;
    uint8_t addr = reg(c, R_TAR) & 0x7f;

    if (!enabled(c) || (c->raw & TX_ABRT)) {
        return;                         /* flushed: held until CLR_TX_ABRT */
    }
    if (!c->active || (v & CMD_RESTART) || read != c->recv) {
        if (c->active && addr != c->addr) {
            i2c_end_transfer(c->bus);
            c->active = false;
        }
        c->raw |= c->active ? RESTART_DET : START_DET;
        if (i2c_start_transfer(c->bus, addr, read)) {
            c->active = false;
            i2c_abort(c, ABRT_7B_ADDR_NOACK);
            return;
        }
        c->active = true;
        c->recv = read;
        c->addr = addr;
        c->first = true;
        c->raw |= ACTIVITY;
    }
    if (read) {
        uint8_t byte = i2c_recv(c->bus);

        if (!rx_push(c, byte | (c->first ? FIRST_DATA_BYTE : 0))) {
            c->raw |= RX_OVER;
        }
        c->first = false;
    } else if (i2c_send(c->bus, v & 0xff)) {
        i2c_abort(c, ABRT_TXDATA_NOACK);
        return;
    }
    if (v & CMD_STOP) {
        if (read) {
            i2c_nack(c->bus);           /* the last byte read: NACKed */
        }
        i2c_stop(c);
    }
}

/* -------------------------------------------------------- as a slave */

/* SCL let go: the master held on it goes on */
static void i2c_release(RP2I2c *c)
{
    if (c->master_held) {
        c->master_held = false;
        if (c->release) {
            c->release(c->release_opaque);
        }
    }
}

void rp2350_i2c_target_on_release(RP2350State *s, int n,
                                  void (*release)(void *opaque), void *opaque)
{
    s->i2c[n].release = release;
    s->i2c[n].release_opaque = opaque;
}

bool rp2350_i2c_target_start(RP2350State *s, int n, uint8_t addr, bool read)
{
    RP2I2c *c = &s->i2c[n];

    if (!slave_mode(c) || addr != (reg(c, R_SAR) & 0x7f)) {
        return false;                   /* no ACK */
    }
    c->raw |= START_DET | ACTIVITY | (c->addressed ? RESTART_DET : 0);
    c->addressed = true;
    c->recv = read;
    c->first = true;
    if (read) {
        if (c->tx_n) {                  /* left from a read before: flushed */
            c->tx_n = 0;
            c->abrt |= ABRT_SLVFLUSH_TXFIFO;
            c->raw |= TX_ABRT;
        }
        c->raw |= RD_REQ;
        c->rd_requested = true;
    }
    i2c_irq(c);
    return true;
}

int rp2350_i2c_target_write(RP2350State *s, int n, uint8_t byte)
{
    RP2I2c *c = &s->i2c[n];

    if (!c->addressed || c->recv) {
        return 1;                       /* not ours to take: let it go */
    }
    if (!rx_push(c, byte | (c->first ? FIRST_DATA_BYTE : 0))) {
        c->master_held = true;          /* full: SCL held */
        return 0;
    }
    c->first = false;
    i2c_irq(c);
    return 1;
}

int rp2350_i2c_target_read(RP2350State *s, int n, uint8_t *byte)
{
    RP2I2c *c = &s->i2c[n];

    if (!c->addressed || !c->recv) {
        *byte = 0xff;
        return 1;
    }
    if (!c->tx_n) {
        if (!c->rd_requested) {         /* once a byte: SCL held meanwhile */
            c->raw |= RD_REQ;
            c->rd_requested = true;
            i2c_irq(c);
        }
        c->master_held = true;
        return 0;
    }
    c->rd_requested = false;
    *byte = c->tx[c->tx_head];
    c->tx_head = (c->tx_head + 1) % FIFO_DEPTH;
    c->tx_n--;
    i2c_irq(c);
    return 1;
}

void rp2350_i2c_target_nack(RP2350State *s, int n)
{
    RP2I2c *c = &s->i2c[n];

    if (c->addressed && c->recv) {
        c->raw |= RX_DONE;
        i2c_irq(c);
    }
}

void rp2350_i2c_target_stop(RP2350State *s, int n)
{
    RP2I2c *c = &s->i2c[n];

    if (c->addressed) {
        c->addressed = false;
        c->rd_requested = false;
        c->raw |= STOP_DET;
        i2c_irq(c);
    }
}

/* -------------------------------------------------------- registers */

static uint32_t i2c_read(RP2Block *b, hwaddr offset)
{
    RP2I2c *c = i2c_of(b);
    uint32_t v = 0;

    switch (offset) {
    case R_DATA_CMD:
        if (c->rx_n) {
            v = c->rx[c->rx_head];
            c->rx_head = (c->rx_head + 1) % FIFO_DEPTH;
            c->rx_n--;
            i2c_release(c);
        } else {
            c->raw |= RX_UNDER;
        }
        break;
    case R_INTR_STAT:
        v = raw_intr(c) & reg(c, R_INTR_MASK);
        break;
    case R_RAW_INTR_STAT:
        v = raw_intr(c);
        break;
    case R_CLR_INTR:                    /* all that can be cleared */
        v = (c->raw & CLEARABLE) != 0;
        c->raw &= ~CLEARABLE | (c->active || c->addressed ? ACTIVITY : 0);
        c->abrt = 0;
        break;
    case R_CLR_RX_UNDER:
        v = !!(c->raw & RX_UNDER);
        c->raw &= ~RX_UNDER;
        break;
    case R_CLR_RX_OVER:
        v = !!(c->raw & RX_OVER);
        c->raw &= ~RX_OVER;
        break;
    case R_CLR_RD_REQ:
        v = !!(c->raw & RD_REQ);
        c->raw &= ~RD_REQ;
        break;
    case R_CLR_TX_ABRT:
        v = !!(c->raw & TX_ABRT);
        c->raw &= ~TX_ABRT;
        c->abrt = 0;
        break;
    case R_CLR_RX_DONE:
        v = !!(c->raw & RX_DONE);
        c->raw &= ~RX_DONE;
        break;
    case R_CLR_ACTIVITY:
        v = !!(c->raw & ACTIVITY);
        if (!c->active && !c->addressed) {
            c->raw &= ~ACTIVITY;
        }
        break;
    case R_CLR_STOP_DET:
        v = !!(c->raw & STOP_DET);
        c->raw &= ~STOP_DET;
        break;
    case R_CLR_START_DET:
        v = !!(c->raw & START_DET);
        c->raw &= ~START_DET;
        break;
    case R_CLR_RESTART_DET:
        v = !!(c->raw & RESTART_DET);
        c->raw &= ~RESTART_DET;
        break;
    case R_CLR_TX_OVER: case R_CLR_GEN_CALL:
        break;
    case R_STATUS:
        v = (c->active ? 1 | (1 << 5) : 0) | (c->addressed ? 1 | (1 << 6) : 0) |
            (c->tx_n < FIFO_DEPTH ? 1 << 1 : 0) | (c->tx_n ? 0 : 1 << 2) |
            (c->rx_n ? 1 << 3 : 0) | (c->rx_n == FIFO_DEPTH ? 1 << 4 : 0);
        break;
    case R_TXFLR:
        v = c->tx_n;
        break;
    case R_RXFLR:
        v = c->rx_n;
        break;
    case R_TX_ABRT_SOURCE:
        v = c->abrt;
        break;
    case R_ENABLE_STATUS:
        v = reg(c, R_ENABLE) & 1;
        break;
    case R_COMP_PARAM_1:
        v = 0;
        break;
    case R_COMP_VERSION:
        v = 0x3230312a;
        break;
    case R_COMP_TYPE:
        v = 0x44570140;
        break;
    default:
        v = offset < RP2_REGS * 4 ? b->regs[offset / 4] : 0;
    }
    i2c_irq(c);
    return v;
}

static void i2c_write(RP2Block *b, hwaddr offset, uint32_t value)
{
    RP2I2c *c = i2c_of(b);

    switch (offset) {
    case R_DATA_CMD:
        if (slave_mode(c)) {
            if (!(value & CMD_READ) && c->tx_n < FIFO_DEPTH) {
                c->tx[(c->tx_head + c->tx_n++) % FIFO_DEPTH] = value;
                i2c_release(c);
            }
        } else {
            i2c_command(c, value);
        }
        break;
    case R_ENABLE:
        if ((value & 2) && c->active) {         /* ABORT */
            i2c_abort(c, ABRT_USER_ABRT);
        }
        b->regs[R_ENABLE / 4] = value & 5;
        if (!(value & 1)) {
            c->rx_n = 0;                        /* the FIFOs flushed */
            c->tx_n = 0;
            i2c_release(c);
        }
        break;
    case R_INTR_STAT: case R_RAW_INTR_STAT: case R_STATUS: case R_TXFLR:
    case R_RXFLR: case R_TX_ABRT_SOURCE: case R_ENABLE_STATUS:
        break;                                  /* read-only */
    default:
        if (offset < RP2_REGS * 4) {
            b->regs[offset / 4] = value;
        }
    }
    i2c_irq(c);
}

I2CBus *rp2350_i2c_bus(RP2350State *s, int n)
{
    return s->i2c[n].bus;
}

void rp2350_i2c_realize(RP2350State *s)
{
    for (int n = 0; n < 2; n++) {
        RP2I2c *c = &s->i2c[n];
        g_autofree char *name = g_strdup_printf("rp2350.i2c%d", n);

        c->bus = i2c_init_bus(DEVICE(s), name);
        c->irq = rp2350_irq(s, IRQ_I2C0 + n);
        rp2_block_init(s, &c->regs, name, 0x40090000 + n * 0x8000, i2c_read,
                       i2c_write);
    }
}

void rp2350_i2c_reset(RP2350State *s)
{
    for (int n = 0; n < 2; n++) {
        RP2I2c *c = &s->i2c[n];
        uint32_t *r = c->regs.regs;

        if (c->active) {
            i2c_end_transfer(c->bus);
        }
        memset(r, 0, sizeof(c->regs.regs));
        r[R_CON / 4] = 0x65;            /* master, fast, restarts, no slave */
        r[R_TAR / 4] = 0x55;
        r[R_SAR / 4] = 0x55;
        r[0x14 / 4] = 0x28;             /* SS_SCL_HCNT, LCNT */
        r[0x18 / 4] = 0x2f;
        r[0x1c / 4] = 0x06;             /* FS_SCL_HCNT, LCNT */
        r[0x20 / 4] = 0x0d;
        r[R_INTR_MASK / 4] = 0x8ff;
        r[0x7c / 4] = 1;                /* SDA_HOLD */
        r[0x94 / 4] = 0x64;             /* SDA_SETUP */
        r[0x98 / 4] = 1;                /* ACK_GENERAL_CALL */
        r[0xa0 / 4] = 7;                /* FS_SPKLEN */
        c->rx_n = c->rx_head = 0;
        c->tx_n = c->tx_head = 0;
        c->active = c->recv = c->first = c->addressed = false;
        c->rd_requested = false;
        c->raw = 0;
        c->abrt = 0;
        i2c_irq(c);
        i2c_release(c);
    }
}
