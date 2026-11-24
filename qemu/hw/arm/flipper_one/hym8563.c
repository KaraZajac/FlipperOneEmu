/*
 * Haoyu HYM8563: the RK3576's real-time clock, on its I2C8 at 0x51
 *
 * A PCF8563's registers, read and written from the address a transfer
 * starts at, onwards: control 1 and 2 (0x00, 0x01), the time in BCD
 * (seconds 0x02, its bit 7 VL, clear: the time is good; minutes, hours,
 * day, weekday, month with the century bit, year), the alarm (0x09-0x0c),
 * CLKOUT (0x0d), the timer (0x0e, 0x0f). The time is QEMU's (-rtc base=
 * sets it) and what the kernel sets is kept as a difference from it, taken
 * when a write that changed the time ends. Neither the alarm nor the timer
 * fires: its INT line, GPIO0_A0, stays high.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "hw/i2c/i2c.h"
#include "system/rtc.h"
#include "rk3576.h"

#define TYPE_HYM8563 "hym8563"
OBJECT_DECLARE_SIMPLE_TYPE(Hym8563State, HYM8563)

#define R_SEC           0x02
#define R_YEAR          0x08

struct Hym8563State {
    I2CSlave parent_obj;

    uint8_t r[16];
    uint8_t ptr;
    bool have_ptr;
    bool time_set;                      /* a write changed the time */
    time_t offset;                      /* the kernel's time, from QEMU's */
};

/* the time registers, as of now */
static void hym_latch(Hym8563State *s)
{
    struct tm tm;

    qemu_get_timedate(&tm, s->offset);
    s->r[0x02] = to_bcd(tm.tm_sec);             /* VL clear */
    s->r[0x03] = to_bcd(tm.tm_min);
    s->r[0x04] = to_bcd(tm.tm_hour);
    s->r[0x05] = to_bcd(tm.tm_mday);
    s->r[0x06] = tm.tm_wday;
    s->r[0x07] = to_bcd(tm.tm_mon + 1) | (tm.tm_year >= 200 ? 0x80 : 0);
    s->r[0x08] = to_bcd(tm.tm_year % 100);
}

/* the time written: from now on, that far from QEMU's */
static void hym_set(Hym8563State *s)
{
    struct tm tm = {
        .tm_sec = from_bcd(s->r[0x02] & 0x7f),
        .tm_min = from_bcd(s->r[0x03] & 0x7f),
        .tm_hour = from_bcd(s->r[0x04] & 0x3f),
        .tm_mday = from_bcd(s->r[0x05] & 0x3f),
        .tm_wday = s->r[0x06] & 7,
        .tm_mon = from_bcd(s->r[0x07] & 0x1f) - 1,
        .tm_year = from_bcd(s->r[0x08]) + (s->r[0x07] & 0x80 ? 200 : 100),
    };

    s->offset = qemu_timedate_diff(&tm);
}

static int hym_event(I2CSlave *i2c, enum i2c_event event)
{
    Hym8563State *s = HYM8563(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->have_ptr = false;
        break;
    case I2C_START_RECV:
        hym_latch(s);                           /* read as one moment */
        break;
    case I2C_FINISH:
        if (s->time_set) {
            s->time_set = false;
            hym_set(s);
        }
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t hym_recv(I2CSlave *i2c)
{
    Hym8563State *s = HYM8563(i2c);

    return s->r[s->ptr++ & 0xf];
}

static int hym_send(I2CSlave *i2c, uint8_t data)
{
    Hym8563State *s = HYM8563(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        if (s->ptr <= R_YEAR) {
            hym_latch(s);                       /* the rest stays as it is */
        }
        return 0;
    }
    if ((s->ptr & 0xf) >= R_SEC && (s->ptr & 0xf) <= R_YEAR) {
        s->time_set = true;
    }
    s->r[s->ptr++ & 0xf] = data;
    return 0;
}

static void hym_reset(DeviceState *dev)
{
    Hym8563State *s = HYM8563(dev);

    memset(s->r, 0, sizeof(s->r));
    s->ptr = 0;
    s->have_ptr = s->time_set = false;
}

static void hym_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    k->event = hym_event;
    k->recv = hym_recv;
    k->send = hym_send;
    device_class_set_legacy_reset(DEVICE_CLASS(oc), hym_reset);
}

static const TypeInfo hym_types[] = {
    {
        .name = TYPE_HYM8563,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Hym8563State),
        .class_init = hym_class_init,
    },
};

DEFINE_TYPES(hym_types)
