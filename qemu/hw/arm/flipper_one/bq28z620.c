/*
 * TI BQ28Z620: the battery's fuel gauge, on I2C
 *
 * The Flipper One's main bus has it at 0x55. Its standard commands are
 * 16-bit words, little-endian, at even addresses; a read goes on from the
 * command's address a byte at a time. What it reports comes from the
 * battery (battery.c): its charge, voltage, the current (positive into the
 * battery), the capacities and times from them, the temperature, the
 * average power in 10 mW steps (as the firmware's driver found it). It is
 * sealed, and has done no charge cycle; AtRate can be written (and its
 * time to empty follows), the ManufacturerAccess/MAC commands are taken
 * and answer nothing.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_BQ28Z620 "bq28z620"
OBJECT_DECLARE_SIMPLE_TYPE(Bq28z620State, BQ28Z620)

/* BatteryStatus */
#define BS_FD           (1u << 4)       /* fully discharged */
#define BS_FC           (1u << 5)       /* fully charged */
#define BS_DSG          (1u << 6)       /* discharging (not charging) */
#define BS_INIT         (1u << 7)

#define FOREVER         65535           /* a time that does not run out */

struct Bq28z620State {
    I2CSlave parent_obj;

    FoBattery *bat;
    uint8_t cmd;                    /* the next byte's address */
    bool have_cmd;
    uint8_t low;                    /* a written word's low byte */
    int16_t at_rate;
};

static uint16_t bq_minutes(int mah, int ma)
{
    return ma > 0 ? MIN((int64_t)mah * 60 / ma, FOREVER - 1) : FOREVER;
}

static uint16_t bq_word(Bq28z620State *s, uint8_t cmd)
{
    static const FoBattery none = { .design_mah = 1 };
    const FoBattery *b = s->bat ? s->bat : &none;
    int fcc = b->design_mah;
    int rem = lround(b->mah);
    int percent = fo_battery_percent(b);
    int current = b->ibat_ma;                   /* + into the battery */
    int mv = fo_battery_mv(b);

    switch (cmd) {
    case 0x00: return 0x6200;                   /* sealed; flash writes ok */
    case 0x02: return s->at_rate;
    case 0x04: return bq_minutes(rem, -s->at_rate);
    case 0x06: return b->temp_dk;
    case 0x08: return mv;
    case 0x0a:
        return BS_INIT | (current > 0 ? 0 : BS_DSG) |
               (percent >= 100 ? BS_FC : 0) | (percent <= 0 ? BS_FD : 0);
    case 0x0c: case 0x14: return current;       /* now, and on average */
    case 0x10: return rem;
    case 0x12: return fcc;
    case 0x16: return bq_minutes(rem, -current);
    case 0x18: return bq_minutes(fcc - rem, current);
    case 0x1a: return (uint16_t)-10;            /* standby: 10 mA */
    case 0x1c: return bq_minutes(rem, 10);
    case 0x1e: return (uint16_t)-2000;          /* the heaviest load: 2 A */
    case 0x20: return bq_minutes(rem, 2000);
    case 0x22:                                  /* 10 mW steps */
        return (int)((int64_t)mv * current / 10000);
    case 0x28: return b->temp_dk + 20;          /* the chip, a little warmer */
    case 0x2a: return 0;                        /* cycles */
    case 0x2c: return percent;
    case 0x2e: return 100;                      /* state of health */
    case 0x30: return 8650;                     /* the charge voltage, mV */
    case 0x32: return percent >= 100 ? 0 : 3000;        /* ... current, mA */
    case 0x3c: return b->design_mah;
    default: return 0;
    }
}

static uint8_t bq_recv(I2CSlave *i2c)
{
    Bq28z620State *s = BQ28Z620(i2c);
    uint16_t w = bq_word(s, s->cmd & ~1);
    uint8_t v = s->cmd & 1 ? w >> 8 : w & 0xff;

    s->cmd++;
    return v;
}

static int bq_send(I2CSlave *i2c, uint8_t data)
{
    Bq28z620State *s = BQ28Z620(i2c);

    if (!s->have_cmd) {
        s->cmd = data;
        s->have_cmd = true;
        return 0;
    }
    if (!(s->cmd & 1)) {
        s->low = data;
    } else if ((s->cmd & ~1) == 0x02) {
        s->at_rate = s->low | data << 8;
    }                                   /* ManufacturerAccess, MAC: taken */
    s->cmd++;
    return 0;
}

static int bq_event(I2CSlave *i2c, enum i2c_event event)
{
    Bq28z620State *s = BQ28Z620(i2c);

    if (event == I2C_START_SEND) {
        s->have_cmd = false;
    } else if (event == I2C_START_RECV && s->bat) {
        fo_battery_update(s->bat);              /* as it is now */
    }
    return 0;
}

void bq28z620_attach(I2CSlave *dev, FoBattery *bat)
{
    BQ28Z620(dev)->bat = bat;
}

static void bq_reset(DeviceState *dev)
{
    Bq28z620State *s = BQ28Z620(dev);

    s->cmd = 0;
    s->have_cmd = false;
    s->at_rate = 0;
}

static void bq_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = bq_event;
    k->recv = bq_recv;
    k->send = bq_send;
    device_class_set_legacy_reset(dc, bq_reset);
}

static const TypeInfo bq_info = {
    .name = TYPE_BQ28Z620,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Bq28z620State),
    .class_init = bq_class_init,
};

static void bq_register(void)
{
    type_register_static(&bq_info);
}

type_init(bq_register)
