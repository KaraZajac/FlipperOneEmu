/*
 * The USB-C port's chips, on I2C, with nothing plugged in
 *
 * The Flipper One's main bus has its USB-C power delivery PHY, an onsemi
 * FUSB302B, at 0x22, and its port controller and mux, a TI HD3SS3220, at
 * 0x47. No cable is modelled yet, so neither ever sees a partner: the
 * FUSB302's toggling (which the firmware starts as a dual-role port) never
 * ends, its comparators and VBUS read nothing, its FIFOs are empty; the
 * HD3SS3220 reports nothing attached. Both keep what is written to their
 * settings, the FUSB302 as its datasheet starts them (SW_RES puts them
 * back; interrupts clear when read; the FIFO address does not advance), the
 * HD3SS3220 likewise (I2C_SOFT_RESET; INTERRUPT_STATUS is cleared by
 * writing 1). Their interrupts would reach the MCU through the main
 * expander; with nothing to report they stay quiet. The HD3SS3220's
 * identification registers are assumed: its name in ASCII from the top
 * address down.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

/* ---------------------------------------------------------- FUSB302B */

#define TYPE_FUSB302 "fusb302"
OBJECT_DECLARE_SIMPLE_TYPE(Fusb302State, FUSB302)

enum {
    F_DEVICE_ID = 0x01, F_RESET = 0x0c, F_SETTINGS_END = 0x11,
    F_STATUS0A = 0x3c, F_STATUS1A, F_INTERRUPTA, F_INTERRUPTB,
    F_STATUS0, F_STATUS1, F_INTERRUPT, F_FIFOS, F_NUM
};

#define F_SW_RES        0x01
#define F_STATUS1_EMPTY 0x28            /* RX_EMPTY, TX_EMPTY */

static const uint8_t fusb_reset_values[F_SETTINGS_END] = {
    [0x01] = 0x91, [0x02] = 0x03, [0x03] = 0x20, [0x04] = 0x31, [0x05] = 0x60,
    [0x06] = 0x24, [0x08] = 0x02, [0x09] = 0x06, [0x0b] = 0x01, [0x0d] = 0x0f,
};

struct Fusb302State {
    I2CSlave parent_obj;
    uint8_t r[F_SETTINGS_END];
    uint8_t ptr;
    bool have_ptr;
};

static uint8_t fusb_read(Fusb302State *s, uint8_t a)
{
    if (a < F_SETTINGS_END) {
        return a == F_RESET ? 0 : s->r[a];
    }
    return a == F_STATUS1 ? F_STATUS1_EMPTY : 0;    /* nothing seen, empty */
}

static void fusb_write(Fusb302State *s, uint8_t a, uint8_t v)
{
    if (a == F_RESET) {
        if (v & F_SW_RES) {
            memcpy(s->r, fusb_reset_values, sizeof(s->r));
        }                           /* PD_RESET: its FIFOs are empty anyway */
    } else if (a > F_DEVICE_ID && a < F_SETTINGS_END) {
        s->r[a] = v;
    }                               /* the TX FIFO: nobody to send to */
}

static uint8_t fusb_recv(I2CSlave *i2c)
{
    Fusb302State *s = FUSB302(i2c);
    uint8_t v = fusb_read(s, s->ptr);

    if (s->ptr != F_FIFOS) {
        s->ptr++;
    }
    return v;
}

static int fusb_send(I2CSlave *i2c, uint8_t data)
{
    Fusb302State *s = FUSB302(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        return 0;
    }
    fusb_write(s, s->ptr, data);
    if (s->ptr != F_FIFOS) {
        s->ptr++;
    }
    return 0;
}

static int fusb_event(I2CSlave *i2c, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        FUSB302(i2c)->have_ptr = false;
    }
    return 0;
}

static void fusb_reset(DeviceState *dev)
{
    Fusb302State *s = FUSB302(dev);

    memcpy(s->r, fusb_reset_values, sizeof(s->r));
    s->ptr = 0;
    s->have_ptr = false;
}

static void fusb_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = fusb_event;
    k->recv = fusb_recv;
    k->send = fusb_send;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), fusb_reset);
}

/* -------------------------------------------------------- HD3SS3220 */

#define TYPE_HD3SS3220 "hd3ss3220"
OBJECT_DECLARE_SIMPLE_TYPE(Hd3ss3220State, HD3SS3220)

enum { H_STATUS = 0x08, H_CONTROL = 0x09, H_GENERAL = 0x0a,
       H_REVISION = 0xa0 };

#define H_INTERRUPT_STATUS  0x10
#define H_SOFT_RESET        0x08

static const uint8_t hd_id[8] = "0223BSU";      /* "USB3220", reversed */

struct Hd3ss3220State {
    I2CSlave parent_obj;
    uint8_t status, control, general;
    uint8_t ptr;
    bool have_ptr;
};

static uint8_t hd_read(Hd3ss3220State *s, uint8_t a)
{
    if (a < 8) {
        return hd_id[a];
    }
    switch (a) {
    case H_STATUS: return s->status;
    case H_CONTROL: return s->control;     /* ATTACHED_STATE 0: nothing */
    case H_GENERAL: return s->general;
    case H_REVISION: return 0x02;
    }
    return 0;
}

static void hd_reset(DeviceState *dev);

static void hd_write(Hd3ss3220State *s, uint8_t a, uint8_t v)
{
    switch (a) {
    case H_STATUS:
        s->status = (s->status & 0x3f) | (v & 0xc0);    /* the advertised */
        break;
    case H_CONTROL:
        s->control = (s->control & ~(0x06 | (v & H_INTERRUPT_STATUS))) |
                     (v & 0x06);                        /* DRP duty cycle */
        break;
    case H_GENERAL:
        if (v & H_SOFT_RESET) {
            hd_reset(DEVICE(s));
        } else {
            s->general = v & ~H_SOFT_RESET;
        }
        break;
    }
}

static uint8_t hd_recv(I2CSlave *i2c)
{
    Hd3ss3220State *s = HD3SS3220(i2c);

    return hd_read(s, s->ptr++);
}

static int hd_send(I2CSlave *i2c, uint8_t data)
{
    Hd3ss3220State *s = HD3SS3220(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        hd_write(s, s->ptr++, data);
    }
    return 0;
}

static int hd_event(I2CSlave *i2c, enum i2c_event event)
{
    if (event == I2C_START_SEND) {
        HD3SS3220(i2c)->have_ptr = false;
    }
    return 0;
}

static void hd_reset(DeviceState *dev)
{
    Hd3ss3220State *s = HD3SS3220(dev);

    s->status = s->control = s->general = 0;
}

static void hd_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = hd_event;
    k->recv = hd_recv;
    k->send = hd_send;
    device_class_set_legacy_reset(DEVICE_CLASS(klass), hd_reset);
}

static const TypeInfo usb_c_types[] = {
    {
        .name = TYPE_FUSB302,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Fusb302State),
        .class_init = fusb_class_init,
    }, {
        .name = TYPE_HD3SS3220,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Hd3ss3220State),
        .class_init = hd_class_init,
    },
};
DEFINE_TYPES(usb_c_types)
