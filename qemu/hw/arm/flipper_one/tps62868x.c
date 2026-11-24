/*
 * TI TPS62868x: the screen's VCI supply, a step-down converter on I2C
 *
 * The Flipper One's control bus has it at 0x47, enabled by the MCU's
 * GPIO24. 8-bit registers, the address going up a register a byte: VOUT1
 * and VOUT2 (the output's setting, 5 mV a step from 0.4 V before the
 * board's divider), CONTROL (its RESET bit puts them back as they start)
 * and STATUS (no undervoltage, hiccup or heat here). The firmware sets
 * forced PWM and 3.3 V. Its output powers nothing in the model: the
 * screen's controller works without it. The values the registers start
 * with are assumed (VOUT 0x64, CONTROL 0x2B: enabled, output discharge,
 * the slowest ramp).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_TPS62868X "tps62868x"
OBJECT_DECLARE_SIMPLE_TYPE(Tps62868xState, TPS62868X)

enum { R_VOUT1 = 0x01, R_VOUT2 = 0x02, R_CONTROL = 0x03, R_STATUS = 0x05,
       R_NUM };

#define CONTROL_RESET   0x80

struct Tps62868xState {
    I2CSlave parent_obj;

    uint8_t r[R_NUM];
    uint8_t ptr;
    bool have_ptr;
};

static void tps_chip_reset(Tps62868xState *s)
{
    memset(s->r, 0, sizeof(s->r));
    s->r[R_VOUT1] = s->r[R_VOUT2] = 0x64;
    s->r[R_CONTROL] = 0x2b;
}

static uint8_t tps_recv(I2CSlave *i2c)
{
    Tps62868xState *s = TPS62868X(i2c);
    uint8_t v = s->ptr < R_NUM ? s->r[s->ptr] : 0;

    s->ptr++;
    return v;
}

static int tps_send(I2CSlave *i2c, uint8_t data)
{
    Tps62868xState *s = TPS62868X(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        return 0;
    }
    if (s->ptr == R_CONTROL && (data & CONTROL_RESET)) {
        tps_chip_reset(s);
    } else if (s->ptr >= R_VOUT1 && s->ptr <= R_CONTROL) {
        s->r[s->ptr] = data;
    }
    s->ptr++;
    return 0;
}

static int tps_event(I2CSlave *i2c, enum i2c_event event)
{
    Tps62868xState *s = TPS62868X(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static void tps_reset(DeviceState *dev)
{
    Tps62868xState *s = TPS62868X(dev);

    s->ptr = 0;
    s->have_ptr = false;
    tps_chip_reset(s);
}

static void tps_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = tps_event;
    k->recv = tps_recv;
    k->send = tps_send;
    device_class_set_legacy_reset(dc, tps_reset);
}

static const TypeInfo tps_info = {
    .name = TYPE_TPS62868X,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Tps62868xState),
    .class_init = tps_class_init,
};

static void tps_register(void)
{
    type_register_static(&tps_info);
}

type_init(tps_register)
