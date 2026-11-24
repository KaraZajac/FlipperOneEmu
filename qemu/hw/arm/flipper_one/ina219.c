/*
 * TI INA219: the system rail's current and voltage monitor, on I2C
 *
 * The Flipper One's main bus has it at 0x45, on VSYS with a 4 mΩ shunt (the
 * f1 board's; "shunt-uohm"). 16-bit registers, most significant byte first;
 * a write's first byte points at one, which reads repeat. What it measures
 * is the battery's side (battery.c): VSYS is the battery's voltage, its
 * current the board's load. As the datasheet has them: the shunt voltage in
 * 10 µV steps, clipped at the gain's range (CONFIG's PG: 40 to 320 mV); the
 * bus voltage in 4 mV steps (bits 15:3), CNVR set; the current register
 * the shunt voltage times CALIBRATION over 4096; the power register the
 * current times the bus voltage over 5000; both 0 while CALIBRATION is.
 * Conversions are done at once, whatever CONFIG's mode; RST puts the
 * registers back as they start.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "flipper_one.h"

#define TYPE_INA219 "ina219"
OBJECT_DECLARE_SIMPLE_TYPE(Ina219State, INA219)

enum { R_CONFIG, R_SHUNT, R_BUS, R_POWER, R_CURRENT, R_CALIBRATION };

#define CONFIG_RST      0x8000

struct Ina219State {
    I2CSlave parent_obj;

    FoBattery *bat;
    uint32_t shunt_uohm;
    uint16_t config, calibration;
    uint8_t ptr;
    int got;                        /* bytes of this write */
    uint8_t msb;
    bool low;                       /* a read's next byte is the low one */
};

static int16_t ina_shunt(Ina219State *s)
{
    int load = s->bat ? s->bat->load_ma : 0;
    int64_t steps = (int64_t)load * s->shunt_uohm / 10000;  /* 10 µV each */
    int range = 4000 << ((s->config >> 11) & 3);            /* 40 mV ... */

    return MIN(MAX(steps, -range), range);
}

static uint16_t ina_bus_steps(Ina219State *s)
{
    return s->bat ? MAX(fo_battery_mv(s->bat), 0) / 4 : 0;  /* 4 mV each */
}

static uint16_t ina_reg(Ina219State *s, uint8_t r)
{
    int32_t current = (int32_t)ina_shunt(s) * s->calibration / 4096;

    switch (r) {
    case R_CONFIG: return s->config;
    case R_SHUNT: return ina_shunt(s);
    case R_BUS: return ina_bus_steps(s) << 3 | 2;           /* CNVR */
    case R_POWER: return abs(current) * ina_bus_steps(s) / 5000;
    case R_CURRENT: return current;
    case R_CALIBRATION: return s->calibration;
    }
    return 0;
}

static void ina_write(Ina219State *s, uint8_t r, uint16_t v)
{
    if (r == R_CONFIG) {
        if (v & CONFIG_RST) {
            s->config = 0x399f;
            s->calibration = 0;
        } else {
            s->config = v;
        }
    } else if (r == R_CALIBRATION) {
        s->calibration = v & 0xfffe;
    }
}

static uint8_t ina_recv(I2CSlave *i2c)
{
    Ina219State *s = INA219(i2c);
    uint16_t v = ina_reg(s, s->ptr);

    s->low = !s->low;
    return s->low ? v >> 8 : v & 0xff;
}

static int ina_send(I2CSlave *i2c, uint8_t data)
{
    Ina219State *s = INA219(i2c);

    switch (s->got++) {
    case 0:
        s->ptr = data;
        break;
    case 1:
        s->msb = data;
        break;
    case 2:
        ina_write(s, s->ptr, s->msb << 8 | data);
        break;
    }
    return 0;
}

static int ina_event(I2CSlave *i2c, enum i2c_event event)
{
    Ina219State *s = INA219(i2c);

    s->got = 0;
    s->low = false;
    return 0;
}

void ina219_attach(I2CSlave *dev, FoBattery *bat)
{
    INA219(dev)->bat = bat;
}

static void ina_reset(DeviceState *dev)
{
    Ina219State *s = INA219(dev);

    s->config = 0x399f;
    s->calibration = 0;
    s->ptr = 0;
    s->got = 0;
    s->low = false;
}

static const Property ina_properties[] = {
    DEFINE_PROP_UINT32("shunt-uohm", Ina219State, shunt_uohm, 4000),
};

static void ina_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = ina_event;
    k->recv = ina_recv;
    k->send = ina_send;
    device_class_set_legacy_reset(dc, ina_reset);
    device_class_set_props(dc, ina_properties);
}

static const TypeInfo ina_info = {
    .name = TYPE_INA219,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Ina219State),
    .class_init = ina_class_init,
};

static void ina_register(void)
{
    type_register_static(&ina_info);
}

type_init(ina_register)
