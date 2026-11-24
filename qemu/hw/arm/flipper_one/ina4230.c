/*
 * TI INA4230: four channels of current, voltage, power and energy, on I2C
 *
 * The Flipper One's main bus has five, at 0x40, 0x41, 0x43, 0x44 and 0x46,
 * on the RK3576's rails and the control board's 3.3 V: the MCU firmware's
 * `power consumption` and Linux's ina4230 driver read them. 16-bit
 * registers, most significant byte first; a write's first byte points at
 * one, which a read repeats from its start (the pointer does not move on:
 * a 32-bit ENERGY register is read as four bytes). What a channel measures
 * is the board's (ina4230_attach(): its rail's voltage and current, and its
 * shunt resistor). As the datasheet has them, for channel n at 8n:
 *   SHUNT        the shunt's voltage in 2.5 µV steps, or 625 nV with the
 *                channel's CONFIG2 RANGE bit (±81.92 or ±20.48 mV)
 *   BUS          the rail's voltage in 1.6 mV steps
 *   CURRENT      SHUNT × CALIBRATION / 2048
 *   POWER        |CURRENT| × BUS / 20000
 *   ENERGY       POWER summed over seconds, 32 bits (overflow: a FLAGS bit)
 *   CALIBRATION  as written (15 bits; 0 at reset, so no current or power)
 *   ALERT_LIMIT, ALERT_CONFIG  as written; the ALERT pin is not modelled
 * CONFIG1 (0x20) chooses the active channels, the conversion times, the
 * averaging and the mode: a continuous mode measures as it is read, a
 * single-shot one at the CONFIG1 write, shutdown keeps the last results.
 * FLAGS' CVRF is set at the end of each conversion cycle (the times and
 * the averaging of the active channels) and cleared by reading FLAGS.
 * CONFIG2's RST puts every register back as it starts, its ACC_RST bits
 * clear a channel's energy and overflow flag; both clear themselves.
 * MANUFACTURER_ID (0x7E) is 0x5449, "TI"; the register map has no device
 * ID, and 0x7F reads 0 as the registers it does not have do.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_INA4230 "ina4230"
OBJECT_DECLARE_SIMPLE_TYPE(Ina4230State, INA4230)

#define NCH             4
enum { R_SHUNT, R_BUS, R_CURRENT, R_POWER, R_ENERGY, R_CALIBRATION,
       R_ALERT_LIMIT, R_ALERT_CONFIG };
#define R_CONFIG1       0x20
#define R_CONFIG2       0x21
#define R_FLAGS         0x22
#define R_MANUFACTURER  0x7e

#define CONFIG1_RESET   0xf127      /* all channels, 1100 µs, 1, continuous */
#define CONFIG2_RST     0x8000
#define CONFIG2_ACC_RST 0x0f00
#define FLAGS_CVRF      0x0080
#define FLAGS_ENERGYOF(n) (0x0100u << (n))

struct Ina4230State {
    I2CSlave parent_obj;

    FoRailFn *rail_fn;                  /* what the channels measure */
    void *rail_opaque;
    int rail[NCH];
    uint32_t shunt_uohm[NCH];

    uint16_t config1, config2, flags;   /* (FLAGS: its overflow bits) */
    uint16_t cal[NCH], alert_limit[NCH], alert_config[NCH];
    int16_t shunt[NCH];                 /* the last results */
    uint16_t bus[NCH];
    double energy[NCH];                 /* POWER × seconds */
    int64_t energy_ns;                  /* ... summed up to here */
    int64_t start_ns, cycle_ns;         /* conversions: since, each cycle */
    int64_t flags_read_ns;              /* FLAGS read: CVRF clear until a
                                           cycle ends after it */

    uint8_t ptr;
    int got;                            /* bytes of this write */
    uint8_t msb;
    uint8_t out[4];                     /* a read's bytes */
    int out_n, out_pos;
};

static const uint16_t conv_us[8] = { 140, 204, 332, 588, 1100, 2116, 4156,
                                     8244 };
static const uint16_t avg_n[8] = { 1, 4, 16, 64, 128, 256, 512, 1024 };

static int ina_mode(Ina4230State *s)
{
    return s->config1 & 7;      /* bit 0 shunt, bit 1 bus, bit 2 continuous */
}

static bool ina_active(Ina4230State *s, int n)
{
    return (s->config1 >> (12 + n)) & 1;
}

static int64_t ina_cycle_ns(Ina4230State *s)
{
    int mode = ina_mode(s);
    int64_t one = 0, all = 0;

    if (mode & 1) {
        one += conv_us[(s->config1 >> 3) & 7];
    }
    if (mode & 2) {
        one += conv_us[(s->config1 >> 6) & 7];
    }
    for (int n = 0; n < NCH; n++) {
        if (ina_active(s, n)) {
            all += one;
        }
    }
    return all * avg_n[(s->config1 >> 9) & 7] * SCALE_US;
}

/* channel @n's results as of now */
static void ina_sample(Ina4230State *s, int n)
{
    int mode = ina_mode(s);
    int32_t mv = 0, ua = 0;

    if (!ina_active(s, n) || !(mode & 3)) {
        return;
    }
    if (s->rail_fn) {
        s->rail_fn(s->rail_opaque, s->rail[n], &mv, &ua);
    }
    if (mode & 1) {
        int64_t nv = (int64_t)ua * s->shunt_uohm[n] / 1000;
        int lsb = (s->config2 >> n) & 1 ? 625 : 2500;          /* nV */
        int64_t steps = (nv + (nv < 0 ? -lsb : lsb) / 2) / lsb;

        s->shunt[n] = MIN(MAX(steps, -32768), 32767);
    }
    if (mode & 2) {
        int64_t steps = ((int64_t)mv * 10 + 8) / 16;           /* 1.6 mV */

        s->bus[n] = MIN(MAX(steps, 0), 32767);
    }
}

static int16_t ina_current(Ina4230State *s, int n)
{
    int64_t c = (int64_t)s->shunt[n] * s->cal[n] / 2048;

    return MIN(MAX(c, -32768), 32767);
}

static uint16_t ina_power(Ina4230State *s, int n)
{
    return MIN((int64_t)abs(ina_current(s, n)) * s->bus[n] / 20000, 65535);
}

/* the energy up to now, at the power as it is (a continuous mode's) */
static void ina_energy(Ina4230State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double dt = (now - s->energy_ns) / 1e9;

    s->energy_ns = now;
    if (!(ina_mode(s) & 4) || (ina_mode(s) & 3) != 3) {
        return;
    }
    for (int n = 0; n < NCH; n++) {
        if (ina_active(s, n)) {
            ina_sample(s, n);
            s->energy[n] += ina_power(s, n) * dt;
            if (s->energy[n] >= 4294967296.0) {
                s->energy[n] -= 4294967296.0;
                s->flags |= FLAGS_ENERGYOF(n);
            }
        }
    }
}

static bool ina_cvrf(Ina4230State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), end;

    if (!(ina_mode(s) & 3) || !s->cycle_ns || now < s->start_ns +
        s->cycle_ns) {
        return false;
    }
    if (ina_mode(s) & 4) {                      /* the last cycle's end */
        end = s->start_ns + (now - s->start_ns) / s->cycle_ns * s->cycle_ns;
    } else {                                    /* the one cycle's */
        end = s->start_ns + s->cycle_ns;
    }
    return end > s->flags_read_ns;
}

/* a register's bytes, for a read */
static void ina_load(Ina4230State *s, uint8_t r)
{
    uint32_t v = 0;
    int bytes = 2;

    if (r < 8 * NCH) {
        int n = r / 8;

        if ((r % 8) <= R_ENERGY) {
            ina_energy(s);
            if (ina_mode(s) & 4) {
                ina_sample(s, n);
            }
        }
        switch (r % 8) {
        case R_SHUNT: v = (uint16_t)s->shunt[n]; break;
        case R_BUS: v = s->bus[n]; break;
        case R_CURRENT: v = (uint16_t)ina_current(s, n); break;
        case R_POWER: v = ina_power(s, n); break;
        case R_ENERGY: v = (uint32_t)s->energy[n]; bytes = 4; break;
        case R_CALIBRATION: v = s->cal[n]; break;
        case R_ALERT_LIMIT: v = s->alert_limit[n]; break;
        case R_ALERT_CONFIG: v = s->alert_config[n]; break;
        }
    } else if (r == R_CONFIG1) {
        v = s->config1;
    } else if (r == R_CONFIG2) {
        v = s->config2;
    } else if (r == R_FLAGS) {
        ina_energy(s);
        v = s->flags | (ina_cvrf(s) ? FLAGS_CVRF : 0);
        s->flags_read_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else if (r == R_MANUFACTURER) {
        v = 0x5449;
    }
    for (int i = 0; i < bytes; i++) {
        s->out[i] = v >> (8 * (bytes - 1 - i));
    }
    s->out_n = bytes;
    s->out_pos = 0;
}

static void ina_defaults(Ina4230State *s)
{
    s->config1 = CONFIG1_RESET;
    s->config2 = s->flags = 0;
    memset(s->cal, 0, sizeof(s->cal));
    memset(s->alert_limit, 0, sizeof(s->alert_limit));
    memset(s->alert_config, 0, sizeof(s->alert_config));
    memset(s->shunt, 0, sizeof(s->shunt));
    memset(s->bus, 0, sizeof(s->bus));
    memset(s->energy, 0, sizeof(s->energy));
    s->energy_ns = s->start_ns = s->flags_read_ns =
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->cycle_ns = ina_cycle_ns(s);
}

static void ina_write(Ina4230State *s, uint8_t r, uint16_t v)
{
    if (r < 8 * NCH) {
        int n = r / 8;

        switch (r % 8) {
        case R_CALIBRATION: s->cal[n] = v & 0x7fff; break;
        case R_ALERT_LIMIT: s->alert_limit[n] = v; break;
        case R_ALERT_CONFIG: s->alert_config[n] = v & 0x1f; break;
        }
    } else if (r == R_CONFIG1) {
        ina_energy(s);                          /* (as configured so far) */
        s->config1 = v;
        s->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->cycle_ns = ina_cycle_ns(s);
        if ((ina_mode(s) & 3) && !(ina_mode(s) & 4)) {
            for (int n = 0; n < NCH; n++) {     /* a single shot: now */
                ina_sample(s, n);
            }
        }
    } else if (r == R_CONFIG2) {
        ina_energy(s);
        if (v & CONFIG2_RST) {
            ina_defaults(s);
            return;
        }
        for (int n = 0; n < NCH; n++) {
            if ((v >> (8 + n)) & 1) {
                s->energy[n] = 0;
                s->flags &= ~FLAGS_ENERGYOF(n);
            }
        }
        s->config2 = v & 0x00ff;
    }
}

static uint8_t ina_recv(I2CSlave *i2c)
{
    Ina4230State *s = INA4230(i2c);
    uint8_t b = s->out[s->out_pos];

    s->out_pos = (s->out_pos + 1) % s->out_n;
    return b;
}

static int ina_send(I2CSlave *i2c, uint8_t data)
{
    Ina4230State *s = INA4230(i2c);

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
    Ina4230State *s = INA4230(i2c);

    s->got = 0;
    if (event == I2C_START_RECV) {
        ina_load(s, s->ptr);                    /* as it is now */
    }
    return 0;
}

void ina4230_attach(I2CSlave *dev, FoRailFn *fn, void *opaque,
                    const int rail[4], const uint32_t shunt_uohm[4])
{
    Ina4230State *s = INA4230(dev);

    s->rail_fn = fn;
    s->rail_opaque = opaque;
    for (int n = 0; n < NCH; n++) {
        s->rail[n] = rail[n];
        s->shunt_uohm[n] = shunt_uohm[n];
    }
}

static void ina_reset(DeviceState *dev)
{
    Ina4230State *s = INA4230(dev);

    ina_defaults(s);
    s->ptr = 0;
    s->got = 0;
    s->out_n = 2;
    s->out_pos = 0;
    memset(s->out, 0, sizeof(s->out));
}

static void ina_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = ina_event;
    k->recv = ina_recv;
    k->send = ina_send;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), ina_reset);
}

static const TypeInfo ina_types[] = {
    {
        .name = TYPE_INA4230,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Ina4230State),
        .class_init = ina_class_init,
    },
};

DEFINE_TYPES(ina_types)
