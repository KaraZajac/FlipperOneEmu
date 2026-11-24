/*
 * NXP PCAL6416A: a 16-bit I/O expander on I2C
 *
 * The Flipper One's control bus has one at 0x20 with the 13 buttons on its
 * inputs (pressed: low) and its INT on the MCU's GPIO46; the main bus has
 * another. Registers come in pairs, one a port: the command byte picks
 * one, and each byte after it goes to the other of the pair. The input
 * port reads the pins (an output reads back its level) through the
 * polarity inversion. An input whose interrupt is not masked and that
 * changes pulls INT low, and sets its bit in the interrupt status; with
 * its input latch set, the input port keeps the level it changed to until
 * it is read. Reading a port of the input register lets INT go for that
 * port. Drive strength, pull-ups and the output's configuration are kept.
 * RESET low puts every register back as it starts, and the chip does not
 * answer on the bus until it goes high again.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_PCAL6416 "pcal6416"
OBJECT_DECLARE_SIMPLE_TYPE(Pcal6416State, PCAL6416)

enum {
    R_INPUT, R_OUTPUT, R_POLARITY, R_CONFIG,        /* 0x00 ... 0x07 */
    R_DRIVE0, R_DRIVE1, R_LATCH, R_PULL_EN, R_PULL_SEL, R_INT_MASK,
    R_INT_STATUS,                                   /* 0x40 ... 0x4d */
    R_NUM
};

struct Pcal6416State {
    I2CSlave parent_obj;

    uint8_t command;                /* the register byte last selected */
    bool have_command;              /* this write's first byte was it */
    uint16_t r[R_NUM];
    uint8_t out_conf;               /* 0x4f */
    uint16_t pins;                  /* the levels from outside: 1 high */
    uint16_t seen;                  /* the inputs' levels when last read */
    uint16_t latched;               /* latched inputs changed since read */
    uint16_t latched_level;         /* ... and the level each changed to */
    bool in_reset;                  /* RESET is low */
    qemu_irq int_out;               /* 0: asserted */
};

/* a command byte's register and port (byte), or -1 */
static int pcal_reg(uint8_t command, int *port)
{
    *port = command & 1;
    if (command < 0x08) {
        return command >> 1;
    }
    if (command >= 0x40 && command < 0x4e) {
        return R_DRIVE0 + ((command - 0x40) >> 1);
    }
    return -1;
}

static uint16_t pcal_levels(Pcal6416State *s)
{
    uint16_t in = s->r[R_CONFIG];       /* 1: an input */

    return (s->pins & in) | (s->r[R_OUTPUT] & ~in);
}

static void pcal_int(Pcal6416State *s)
{
    qemu_set_irq(s->int_out, s->r[R_INT_STATUS] ? 0 : 1);
}

/* the inputs changed (or how they are watched did) */
static void pcal_update(Pcal6416State *s)
{
    uint16_t levels = pcal_levels(s);
    uint16_t watched = s->r[R_CONFIG] & ~s->r[R_INT_MASK];
    uint16_t changed = (levels ^ s->seen) & s->r[R_CONFIG];
    uint16_t to_latch = changed & s->r[R_LATCH] & ~s->latched;

    s->latched_level = (s->latched_level & ~to_latch) | (levels & to_latch);
    s->latched |= to_latch;
    s->r[R_INT_STATUS] |= changed & watched;
    pcal_int(s);
}

static uint8_t pcal_read_byte(Pcal6416State *s, int reg, int port)
{
    uint16_t v;

    if (reg == R_INPUT) {
        uint16_t mask = port ? 0xff00 : 0x00ff;
        uint16_t levels = pcal_levels(s);

        v = (levels & ~(s->latched & mask)) | (s->latched_level & s->latched
                                                & mask);
        v ^= s->r[R_POLARITY];
        /* the read lets INT go for this port */
        s->seen = (s->seen & ~mask) | (levels & mask);
        s->latched &= ~mask;
        s->r[R_INT_STATUS] &= ~mask;
        pcal_int(s);
    } else {
        v = s->r[reg];
    }
    return port ? v >> 8 : v & 0xff;
}

static void pcal_write_byte(Pcal6416State *s, int reg, int port, uint8_t v)
{
    if (reg == R_INPUT || reg == R_INT_STATUS) {
        return;                                 /* read-only */
    }
    s->r[reg] = port ? deposit32(s->r[reg], 8, 8, v)
                     : deposit32(s->r[reg], 0, 8, v);
    pcal_update(s);
}

static uint8_t pcal_recv(I2CSlave *i2c)
{
    Pcal6416State *s = PCAL6416(i2c);
    int port, reg = pcal_reg(s->command, &port);
    uint8_t v;

    if (s->command == 0x4f) {
        return s->out_conf;
    }
    if (reg < 0) {
        return 0xff;
    }
    v = pcal_read_byte(s, reg, port);
    s->command ^= 1;                            /* the other of the pair */
    return v;
}

static int pcal_send(I2CSlave *i2c, uint8_t data)
{
    Pcal6416State *s = PCAL6416(i2c);
    int port, reg;

    if (!s->have_command) {
        s->command = data;
        s->have_command = true;
        return 0;
    }
    if (s->command == 0x4f) {
        s->out_conf = data & 3;
        return 0;
    }
    reg = pcal_reg(s->command, &port);
    if (reg >= 0) {
        pcal_write_byte(s, reg, port, data);
        s->command ^= 1;
    }
    return 0;
}

static int pcal_event(I2CSlave *i2c, enum i2c_event event)
{
    Pcal6416State *s = PCAL6416(i2c);

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        if (s->in_reset) {
            return -1;                          /* no ACK */
        }
        s->have_command = false;
    }
    return 0;
}

void pcal6416_set_pins(I2CSlave *dev, uint16_t levels)
{
    Pcal6416State *s = PCAL6416(dev);

    s->pins = levels;
    pcal_update(s);
}

static void pcal_reset(DeviceState *dev)
{
    Pcal6416State *s = PCAL6416(dev);

    memset(s->r, 0, sizeof(s->r));
    s->r[R_OUTPUT] = 0xffff;
    s->r[R_CONFIG] = 0xffff;                    /* all inputs */
    s->r[R_DRIVE0] = s->r[R_DRIVE1] = 0xffff;
    s->r[R_PULL_SEL] = 0xffff;
    s->r[R_INT_MASK] = 0xffff;                  /* all masked */
    s->out_conf = 0;
    s->command = 0;
    s->have_command = false;
    s->seen = pcal_levels(s);
    s->latched = s->latched_level = 0;
    pcal_int(s);
}

static void pcal_reset_pin(void *opaque, int n, int level)
{
    Pcal6416State *s = opaque;

    s->in_reset = !level;
    if (s->in_reset) {
        pcal_reset(DEVICE(s));
    }
}

static void pcal_init(Object *obj)
{
    Pcal6416State *s = PCAL6416(obj);

    s->pins = 0xffff;                           /* pulled up */
    qdev_init_gpio_in(DEVICE(obj), pcal_reset_pin, 1);
    qdev_init_gpio_out(DEVICE(obj), &s->int_out, 1);
}

static void pcal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = pcal_event;
    k->recv = pcal_recv;
    k->send = pcal_send;
    device_class_set_legacy_reset(dc, pcal_reset);
}

static const TypeInfo pcal_info = {
    .name = TYPE_PCAL6416,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Pcal6416State),
    .instance_init = pcal_init,
    .class_init = pcal_class_init,
};

static void pcal_register(void)
{
    type_register_static(&pcal_info);
}

type_init(pcal_register)
