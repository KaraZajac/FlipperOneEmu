/*
 * TI BQ25792: the battery's charger, on I2C
 *
 * The Flipper One's main bus has it at 0x6B (the firmware's driver takes a
 * BQ25792 or a BQ25798; this is a BQ25792, revision 1). Registers are a
 * byte map, read and written from the address a transfer starts at; the
 * 16-bit ones (the limits, the ADC) most significant byte first. The
 * settings start as the datasheet has them for two cells and keep what is
 * written; REG_RST puts them back, WD_RST clears itself. The status, flag
 * and ADC registers are read-only, the flags cleared by reading them.
 *
 * No USB input is modelled yet: VBUS is absent, nothing charges, and the
 * ADC reads the battery's side (battery.c): VBAT and VSYS its voltage, IBAT
 * the board's load (negative: out of the battery), TS the 10 kΩ NTC at its
 * temperature through the board's divider (as the firmware works it out:
 * 5.23 kΩ and 31.6 kΩ), the die at 30 °C. With nothing to flag, INT stays
 * high.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_BQ2579X "bq2579x"
OBJECT_DECLARE_SIMPLE_TYPE(Bq2579xState, BQ2579X)

enum {
    R_TERMINATION = 0x09, R_CTRL1 = 0x10, R_ICO_LIMIT = 0x19,
    R_STATUS0 = 0x1b, R_STATUS2 = 0x1d, R_FLAG0 = 0x22, R_FLAG_END = 0x28,
    R_IBUS = 0x31, R_IBAT = 0x33, R_VBUS = 0x35, R_VAC1 = 0x37, R_VAC2 = 0x39,
    R_VBAT = 0x3b, R_VSYS = 0x3d, R_TS = 0x3f, R_TDIE = 0x41,
    R_DPLUS = 0x43, R_DMINUS = 0x45, R_PART = 0x48, R_NUM
};

#define TERM_REG_RST    0x40
#define CTRL1_WD_RST    0x08
#define STATUS2_VBAT_PRESENT 0x01

/* the settings as the chip starts, for two cells */
static const uint8_t bq_reset_values[R_NUM] = {
    [0x00] = 0x12,                  /* VSYSMIN 7.0 V */
    [0x01] = 0x03, [0x02] = 0x48,   /* VREG 8.40 V */
    [0x03] = 0x00, [0x04] = 0x64,   /* ICHG 1.00 A */
    [0x05] = 0x24,                  /* VINDPM 3.6 V */
    [0x06] = 0x01, [0x07] = 0x2c,   /* IINDPM 3.00 A */
    [0x08] = 0xc3, [0x09] = 0x05, [0x0a] = 0x63,
    [0x0b] = 0x00, [0x0c] = 0xdc,   /* VOTG 5.0 V */
    [0x0d] = 0x4b, [0x0e] = 0x3d, [0x0f] = 0xa2, [0x10] = 0x85,
    [0x11] = 0x40, [0x14] = 0x16, [0x15] = 0xaa, [0x16] = 0xc0,
    [0x17] = 0x7a, [0x18] = 0x54, [0x2e] = 0x30,
    [R_PART] = 0x09,                /* BQ25792, revision 1 */
};

struct Bq2579xState {
    I2CSlave parent_obj;

    FoBattery *bat;
    uint8_t r[R_NUM];
    uint8_t ptr;
    bool have_ptr;
};

/* TS: the NTC (10 kΩ at 25 °C, β 3380) with 31.6 kΩ across it, below
 * 5.23 kΩ from REGN: its part of REGN, in 1/1024 */
static uint16_t bq_ts(const FoBattery *b)
{
    double t = b->temp_dk / 10.0;
    double ntc = 10000.0 * exp(3380.0 * (1.0 / t - 1.0 / 298.15));
    double below = 1.0 / (1.0 / ntc + 1.0 / 31600.0);

    return lround(below / (below + 5230.0) * 1024.0);
}

/* a 16-bit ADC register (at its even address) */
static uint16_t bq_adc(Bq2579xState *s, uint8_t reg)
{
    static const FoBattery none = { .percent = 0, .temp_dk = 2982 };
    const FoBattery *b = s->bat ? s->bat : &none;

    switch (reg) {
    case R_IBAT: return -b->load_ma;
    case R_VBAT: case R_VSYS: return fo_battery_mv(b);
    case R_TS: return bq_ts(b);
    case R_TDIE: return 60;                     /* 30 °C, 0.5 °C a step */
    default: return 0;      /* IBUS, VBUS, VAC1, VAC2, D+, D-: no input */
    }
}

static uint8_t bq_byte(Bq2579xState *s, uint8_t a)
{
    if (a >= R_IBUS && a < R_IBUS + 22) {
        uint16_t v = bq_adc(s, a - ((a - R_IBUS) & 1));

        return (a - R_IBUS) & 1 ? v & 0xff : v >> 8;
    }
    switch (a) {
    case R_ICO_LIMIT:                   /* ICO: the input current limit */
        return s->r[0x06];
    case R_ICO_LIMIT + 1:
        return s->r[0x07];
    case R_STATUS2:
        return STATUS2_VBAT_PRESENT;
    }
    return a < R_NUM ? s->r[a] : 0;
}

static void bq_write(Bq2579xState *s, uint8_t a, uint8_t v)
{
    if (a >= R_ICO_LIMIT && a < R_FLAG_END) {
        return;                                 /* status, flags */
    }
    if ((a >= R_IBUS && a < R_IBUS + 22) || a >= R_PART) {
        return;                                 /* ADC, part */
    }
    if (a == R_TERMINATION && (v & TERM_REG_RST)) {
        memcpy(s->r, bq_reset_values, sizeof(s->r));
        return;
    }
    if (a == R_CTRL1) {
        v &= ~CTRL1_WD_RST;
    }
    s->r[a] = v;
}

static uint8_t bq_recv(I2CSlave *i2c)
{
    Bq2579xState *s = BQ2579X(i2c);
    uint8_t a = s->ptr++;
    uint8_t v = bq_byte(s, a);

    if (a >= R_FLAG0 && a < R_FLAG_END) {
        s->r[a] = 0;                            /* read: cleared */
    }
    return v;
}

static int bq_send(I2CSlave *i2c, uint8_t data)
{
    Bq2579xState *s = BQ2579X(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        bq_write(s, s->ptr++, data);
    }
    return 0;
}

static int bq_event(I2CSlave *i2c, enum i2c_event event)
{
    Bq2579xState *s = BQ2579X(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

void bq2579x_attach(I2CSlave *dev, FoBattery *bat)
{
    BQ2579X(dev)->bat = bat;
}

static void bq_reset(DeviceState *dev)
{
    Bq2579xState *s = BQ2579X(dev);

    memcpy(s->r, bq_reset_values, sizeof(s->r));
    s->ptr = 0;
    s->have_ptr = false;
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
    .name = TYPE_BQ2579X,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Bq2579xState),
    .class_init = bq_class_init,
};

static void bq_register(void)
{
    type_register_static(&bq_info);
}

type_init(bq_register)
