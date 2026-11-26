/*
 * Rockchip I2C controller (rk3x: the RK3399's, which the RK3576 has nine of)
 *
 * As Linux's i2c-rk3x drives it: CON's mode says what the next count does.
 *   TX           MTXCNT bytes from TXBUFFER go out; the first after a START
 *                is the address byte (the driver puts it there)
 *   REGISTER_TX  MRXCNT bytes come in from MRXADDR's target, after the
 *                register bytes MRXRADDR has valid (if its address is a
 *                write: then a repeated START, as a read)
 *   RX           MRXCNT more bytes of the read under way
 * A START (CON) is START in IPD at once, its addressing done with the first
 * count; a STOP ends the transfer, STOP in IPD. A target that does not
 * acknowledge its address or a byte is NAKRCV, and the count goes no
 * further; the driver stops. MBTF and MBRF say a count is done (FCNT how
 * many bytes); LASTACK has the last byte of a read not acknowledged. The
 * interrupt is IPD & IEN; IPD is cleared by writing 1s.
 *
 * A count is a few steps (a START with its address byte, bytes written,
 * bytes read), each done as soon as the target takes or gives it: QEMU's
 * I2C devices at once; a target that holds SCL (the MCU, whose firmware
 * answers in its interrupt handler) has its step tried again when it says
 * it has let go (rk_i2c_release()), from the main loop, or 100 µs later.
 * The bus's own timing is not modelled.
 *
 * ST (0x220) says SDA and SCL are high: an idle bus (the driver looks
 * before it starts, and recovers a bus it finds held low).
 *
 * The bus is the controller's own, or one given (the Flipper One's main
 * bus has the RK3576's I2C2 and the MCU's PIO I2C on it, as two masters).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qemu/timer.h"
#include "rk3576.h"

#define R_CON           0x00
#define R_CLKDIV        0x04
#define R_MRXADDR       0x08
#define R_MRXRADDR      0x0c
#define R_MTXCNT        0x10
#define R_MRXCNT        0x14
#define R_IEN           0x18
#define R_IPD           0x1c
#define R_FCNT          0x20
#define TXBUF           0x100
#define RXBUF           0x200
#define R_ST            0x220
#define ST_SDA_SCL      0x3

#define CON_EN          0x01
#define CON_MOD(con)    (((con) >> 1) & 3)
#define CON_START       0x08
#define CON_STOP        0x10
#define CON_LASTACK     0x20
#define MOD_TX          0
#define MOD_REGISTER_TX 1
#define MOD_RX          2

#define INT_MBTF        0x04
#define INT_MBRF        0x08
#define INT_START       0x10
#define INT_STOP        0x20
#define INT_NAKRCV      0x40
#define INT_ALL         0x7f

#define HELD_NS         100000          /* SCL held: try again then */
#define BUSY_NS         20000           /* the other master's transfer */

static void rki2c_irq(RkI2cState *s)
{
    qemu_set_irq(s->irq, (s->ipd & s->ien & INT_ALL) != 0);
}

static uint8_t buf_byte(const uint32_t *buf, int i)
{
    return buf[i / 4] >> (i % 4 * 8);
}

/* ---- the bus's other end: QEMU's I2C devices, or a target */

static int port_start(RkI2cState *s, uint8_t byte)
{
    s->busy = false;
    if (s->arb && !s->claimed) {
        if (!s->arb(s->arb_opaque, true)) {
            s->busy = true;
            return 0;                           /* later */
        }
        s->claimed = true;
    }
    if (s->target) {
        return s->target->start(s->target->opaque, byte >> 1, byte & 1);
    }
    return (byte & 1 ? i2c_start_recv(s->bus, byte >> 1)
                     : i2c_start_send(s->bus, byte >> 1)) ? -1 : 1;
}

static int port_write(RkI2cState *s, uint8_t byte)
{
    if (s->target) {
        return s->target->write(s->target->opaque, byte);
    }
    return i2c_send(s->bus, byte) ? -1 : 1;
}

static int port_read(RkI2cState *s, uint8_t *byte)
{
    if (s->target) {
        return s->target->read(s->target->opaque, byte);
    }
    *byte = i2c_recv(s->bus);
    return 1;
}

static void port_nack(RkI2cState *s)
{
    if (s->target) {
        s->target->nack(s->target->opaque);
    } else {
        i2c_nack(s->bus);
    }
}

static void port_stop(RkI2cState *s)
{
    if (s->target) {
        s->target->stop(s->target->opaque);
    } else if (!s->arb || s->claimed) {         /* (not the other's) */
        i2c_end_transfer(s->bus);
    }
    if (s->claimed) {
        s->claimed = false;
        s->arb(s->arb_opaque, false);
    }
}

/* ---- a count: its steps, as far as the target lets them go */

static void rki2c_step(RkI2cState *s, int op, uint8_t byte, bool last)
{
    s->steps[s->nsteps++] = (RkI2cStep){ .op = op, .byte = byte,
                                         .last = last };
}

static void rki2c_run(RkI2cState *s)
{
    while (s->step < s->nsteps) {
        RkI2cStep *st = &s->steps[s->step];
        uint8_t b = 0;
        int r;

        switch (st->op) {
        case RKI2C_START:
            r = port_start(s, st->byte);
            s->active = true;
            break;
        case RKI2C_WRITE:
            r = port_write(s, st->byte);
            break;
        default:
            r = port_read(s, &b);
            if (r > 0) {
                s->rxbuf[s->nread / 4] |= (uint32_t)b << (s->nread % 4 * 8);
                s->nread++;
                if (st->last) {
                    port_nack(s);
                }
            }
        }
        if (r == 0) {                           /* held: again, later */
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                (s->busy ? BUSY_NS : HELD_NS));
            return;
        }
        if (r < 0) {
            s->ipd |= INT_NAKRCV;
            s->nsteps = 0;
            rki2c_irq(s);
            return;
        }
        s->step++;
        s->fcnt = s->reading ? s->nread : s->step;
    }
    s->ipd |= s->reading ? INT_MBRF : INT_MBTF;
    s->nsteps = 0;
    rki2c_irq(s);
}

static void rki2c_held(void *opaque)
{
    rki2c_run(opaque);
}

void rk_i2c_release(RkI2cState *s)
{
    if (s->step < s->nsteps) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static void rki2c_begin(RkI2cState *s, bool reading)
{
    s->reading = reading;
    s->step = s->nread = 0;
    s->need_addr = false;
    timer_del(s->timer);
    rki2c_run(s);
}

static void rki2c_transmit(RkI2cState *s, int n)
{
    s->nsteps = 0;
    for (int i = 0; i < n; i++) {
        rki2c_step(s, s->need_addr && !i ? RKI2C_START : RKI2C_WRITE,
                   buf_byte(s->txbuf, i), false);
    }
    rki2c_begin(s, false);
}

static void rki2c_receive(RkI2cState *s, int n)
{
    s->nsteps = 0;
    if (CON_MOD(s->con) == MOD_REGISTER_TX && s->need_addr) {
        uint8_t addr = s->mrxaddr;

        rki2c_step(s, RKI2C_START, addr, false);
        if (!(addr & 1)) {                      /* the register, then read */
            for (int i = 0; i < 3; i++) {
                if (s->mrxraddr >> (24 + i) & 1) {
                    rki2c_step(s, RKI2C_WRITE, s->mrxraddr >> (8 * i), false);
                }
            }
            rki2c_step(s, RKI2C_START, addr | 1, false);
        }
    }
    for (int i = 0; i < n; i++) {
        rki2c_step(s, RKI2C_READ, 0, i == n - 1 && (s->con & CON_LASTACK));
    }
    memset(s->rxbuf, 0, sizeof(s->rxbuf));
    rki2c_begin(s, true);
}

static uint64_t rki2c_read(void *opaque, hwaddr off, unsigned size)
{
    RkI2cState *s = opaque;

    switch (off) {
    case R_CON: return s->con;
    case R_CLKDIV: return s->clkdiv;
    case R_MRXADDR: return s->mrxaddr;
    case R_MRXRADDR: return s->mrxraddr;
    case R_MTXCNT: return s->mtxcnt;
    case R_MRXCNT: return s->mrxcnt;
    case R_IEN: return s->ien;
    case R_IPD: return s->ipd;
    case R_FCNT: return s->fcnt;
    case TXBUF ... TXBUF + 0x1f: return s->txbuf[(off - TXBUF) / 4];
    case RXBUF ... RXBUF + 0x1f: return s->rxbuf[(off - RXBUF) / 4];
    case R_ST: return ST_SDA_SCL;
    }
    return 0;
}

static void rki2c_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkI2cState *s = opaque;
    uint32_t old;

    switch (off) {
    case R_CON:
        old = s->con;
        s->con = v;
        if ((v & (CON_EN | CON_START)) == (CON_EN | CON_START) &&
            !(old & CON_START)) {
            s->need_addr = true;                /* (a repeated one too) */
            s->ipd |= INT_START;
        }
        if ((v & (CON_EN | CON_STOP)) == (CON_EN | CON_STOP) &&
            !(old & CON_STOP)) {
            s->nsteps = 0;                      /* (a count left undone) */
            timer_del(s->timer);
            if (s->active) {
                port_stop(s);
                s->active = false;
            }
            s->ipd |= INT_STOP;
        }
        break;
    case R_CLKDIV:
        s->clkdiv = v;
        break;
    case R_MRXADDR:
        s->mrxaddr = v;
        break;
    case R_MRXRADDR:
        s->mrxraddr = v;
        break;
    case R_MTXCNT:
        s->mtxcnt = v & 0x3f;
        if (s->con & CON_EN && CON_MOD(s->con) == MOD_TX) {
            rki2c_transmit(s, MIN(s->mtxcnt, 32));
        }
        break;
    case R_MRXCNT:
        s->mrxcnt = v & 0x3f;
        if (s->con & CON_EN && CON_MOD(s->con) != MOD_TX) {
            rki2c_receive(s, MIN(s->mrxcnt, 32));
        }
        break;
    case R_IEN:
        s->ien = v;
        break;
    case R_IPD:
        s->ipd &= ~v;
        break;
    case TXBUF ... TXBUF + 0x1f:
        s->txbuf[(off - TXBUF) / 4] = v;
        break;
    }
    rki2c_irq(s);
}

static const MemoryRegionOps rki2c_ops = {
    .read = rki2c_read,
    .write = rki2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rki2c_reset(DeviceState *dev)
{
    RkI2cState *s = RK_I2C(dev);

    timer_del(s->timer);
    s->nsteps = 0;
    if (s->active || s->claimed) {
        port_stop(s);
    }
    s->busy = false;
    s->con = s->clkdiv = s->mrxaddr = s->mrxraddr = 0;
    s->mtxcnt = s->mrxcnt = s->ien = s->ipd = s->fcnt = 0;
    memset(s->txbuf, 0, sizeof(s->txbuf));
    memset(s->rxbuf, 0, sizeof(s->rxbuf));
    s->active = s->need_addr = false;
    qemu_set_irq(s->irq, 0);
}

static void rki2c_init(Object *obj)
{
    RkI2cState *s = RK_I2C(obj);

    memory_region_init_io(&s->mr, obj, &rki2c_ops, s, "rk3x-i2c", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rki2c_held, s);
}

static void rki2c_realize(DeviceState *dev, Error **errp)
{
    RkI2cState *s = RK_I2C(dev);

    s->bus = s->shared_bus ?: i2c_init_bus(dev, "i2c");
}

static const Property rki2c_props[] = {
    DEFINE_PROP_LINK("bus", RkI2cState, shared_bus, TYPE_I2C_BUS, I2CBus *),
};

static void rki2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rki2c_realize;
    device_class_set_legacy_reset(dc, rki2c_reset);
    device_class_set_props(dc, rki2c_props);
}

static const TypeInfo rki2c_types[] = {
    {
        .name = TYPE_RK_I2C,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkI2cState),
        .instance_init = rki2c_init,
        .class_init = rki2c_class_init,
    },
};

DEFINE_TYPES(rki2c_types)
