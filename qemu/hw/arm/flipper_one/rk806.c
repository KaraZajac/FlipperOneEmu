/*
 * Rockchip RK806: the RK3576's PMIC, on its I2C1 at 0x23
 *
 * Registers are bytes, read and written from the address a transfer
 * starts at, onwards. As Linux's rk8xx driver has them:
 *   0x00-0x05  POWER_EN0-5: each regulator's enable in the low nibble,
 *              changed only where the high nibble says (a write mask); the
 *              high nibble reads as 1s, so an enabled regulator reads with
 *              both of its bits set, as the driver checks
 *   0x5a 0x5b  CHIP_NAME and CHIP_VER: 0x80, 0x61 (the ID 0x806x)
 *   0x77 0x79  INT_STS0/1, cleared by writing 1s (nothing raises them yet:
 *              its INT line, GPIO0_A6, stays high)
 * Everything else keeps what is written: the voltages the kernel sets (each
 * regulator's ON_VSEL), the sleep settings, the PMIC's GPIOs and options.
 * At reset every regulator is off and at its lowest voltage: the kernel
 * turns on and sets what its device tree asks for.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "rk3576.h"

#define TYPE_RK806 "rk806"
OBJECT_DECLARE_SIMPLE_TYPE(Rk806State, RK806)

#define R_POWER_EN0     0x00
#define R_POWER_EN5     0x05
#define R_CHIP_NAME     0x5a
#define R_CHIP_VER      0x5b
#define R_INT_STS0      0x77
#define R_INT_STS1      0x79

struct Rk806State {
    I2CSlave parent_obj;

    uint8_t r[256];
    uint8_t ptr;
    bool have_ptr;
};

static uint8_t rk806_byte(Rk806State *s, uint8_t a)
{
    switch (a) {
    case R_POWER_EN0 ... R_POWER_EN5:
        return 0xf0 | (s->r[a] & 0x0f);
    case R_CHIP_NAME:
        return 0x80;
    case R_CHIP_VER:
        return 0x61;
    }
    return s->r[a];
}

static void rk806_write(Rk806State *s, uint8_t a, uint8_t v)
{
    switch (a) {
    case R_POWER_EN0 ... R_POWER_EN5: {
        uint8_t mask = v >> 4;

        s->r[a] = (s->r[a] & ~mask) | (v & mask);
        break;
    }
    case R_CHIP_NAME: case R_CHIP_VER:
        break;
    case R_INT_STS0: case R_INT_STS1:
        s->r[a] &= ~v;
        break;
    default:
        s->r[a] = v;
    }
}

static int rk806_event(I2CSlave *i2c, enum i2c_event event)
{
    Rk806State *s = RK806(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static uint8_t rk806_recv(I2CSlave *i2c)
{
    Rk806State *s = RK806(i2c);

    return rk806_byte(s, s->ptr++);
}

static int rk806_send(I2CSlave *i2c, uint8_t data)
{
    Rk806State *s = RK806(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        rk806_write(s, s->ptr++, data);
    }
    return 0;
}

static void rk806_reset(DeviceState *dev)
{
    Rk806State *s = RK806(dev);

    memset(s->r, 0, sizeof(s->r));
    s->ptr = 0;
    s->have_ptr = false;
}

static void rk806_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = rk806_event;
    k->recv = rk806_recv;
    k->send = rk806_send;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), rk806_reset);
}

static const TypeInfo rk806_types[] = {
    {
        .name = TYPE_RK806,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Rk806State),
        .class_init = rk806_class_init,
    },
};

DEFINE_TYPES(rk806_types)
