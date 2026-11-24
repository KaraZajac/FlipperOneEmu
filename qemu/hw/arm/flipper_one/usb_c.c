/*
 * The USB-C port's chips, on I2C
 *
 * The Flipper One's main bus has its USB-C power delivery PHY, an onsemi
 * FUSB302B, at 0x22, and its port controller and mux, a TI HD3SS3220, at
 * 0x47. Both keep what is written to their settings, the FUSB302 as its
 * datasheet starts them (SW_RES puts them back; its interrupts clear when
 * read; the FIFO address does not advance), the HD3SS3220 likewise
 * (I2C_SOFT_RESET; INTERRUPT_STATUS is cleared by writing 1).
 *
 * The partner is nothing, or a source on CC1 (fusb302_attach(),
 * hd3ss3220_attach()): a charger, its Rp offering 0.9, 1.5 or 3 A. Then
 * the FUSB302 sees VBUS (VBUSOK), the Rp's level on the CC it measures
 * (BC_LVL, CC1's), and its toggling, if it ran, ends as a sink on CC1
 * (TOGSS, I_TOGDONE); I_VBUSOK and I_BC_LVL come at each change. Its INT_N
 * is low while an interrupt is set that MASK, MASKA, MASKB and CONTROL0's
 * INT_MASK let through, to the main expander. No PD message is sent or
 * received: its FIFOs stay empty. The HD3SS3220 attaches as a sink (UFP)
 * with the cable on CC1, unless it is set to be a source only, and says
 * what current it detected; it raises INTERRUPT_STATUS at each change (its
 * INT_N goes nowhere the MCU reads). Its identification registers are
 * assumed: its name in ASCII from the top address down.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
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
#define F_SWITCHES0     0x02
#define F_MEAS_CC1      0x04
#define F_CONTROL0      0x06
#define F_INT_MASK      0x20            /* CONTROL0: every interrupt off */
#define F_CONTROL2      0x08
#define F_TOGGLE        0x01
#define F_MASK          0x0a
#define F_MASKA         0x0e
#define F_MASKB         0x0f
#define F_VBUSOK        0x80            /* STATUS0, and I_VBUSOK */
#define F_I_BC_LVL      0x01
#define F_I_TOGDONE     0x40            /* INTERRUPTA */
#define F_TOGSS_SNK1    (5 << 3)        /* STATUS1A: toggling found a source
                                           on CC1, as a sink */

static const uint8_t fusb_reset_values[F_SETTINGS_END] = {
    [0x01] = 0x91, [0x02] = 0x03, [0x03] = 0x20, [0x04] = 0x31, [0x05] = 0x60,
    [0x06] = 0x24, [0x08] = 0x02, [0x09] = 0x06, [0x0b] = 0x01, [0x0d] = 0x0f,
};

struct Fusb302State {
    I2CSlave parent_obj;
    uint8_t r[F_SETTINGS_END];
    uint8_t ptr;
    bool have_ptr;
    int rp_ma;                      /* the partner's Rp: none, 0 */
    bool toggled;                   /* toggling ended on it */
    uint8_t status0;                /* VBUSOK, BC_LVL as last seen */
    uint8_t interrupt, interrupta;  /* set until read */
    qemu_irq int_n;                 /* 0: asserted */
};

/* STATUS0: VBUS, and the Rp's level on the CC measured (CC1's) */
static uint8_t fusb_status0(Fusb302State *s)
{
    int bc_lvl = !s->rp_ma ? 0 : s->rp_ma >= 3000 ? 3 :
                 s->rp_ma >= 1500 ? 2 : 1;

    if (!(s->r[F_SWITCHES0] & F_MEAS_CC1)) {
        bc_lvl = 0;                 /* CC2: open */
    }
    return (s->rp_ma ? F_VBUSOK : 0) | bc_lvl;
}

static void fusb_int(Fusb302State *s)
{
    bool on = !(s->r[F_CONTROL0] & F_INT_MASK) &&
              ((s->interrupt & ~s->r[F_MASK]) ||
               (s->interrupta & ~s->r[F_MASKA]));

    qemu_set_irq(s->int_n, !on);
}

/* the partner changed, or what is measured: the interrupts of it */
static void fusb_look(Fusb302State *s)
{
    uint8_t st = fusb_status0(s), ch = st ^ s->status0;

    s->interrupt |= (ch & F_VBUSOK ? F_VBUSOK : 0) |
                    (ch & 3 ? F_I_BC_LVL : 0);
    s->status0 = st;
    if (s->rp_ma && !s->toggled && (s->r[F_CONTROL2] & F_TOGGLE)) {
        s->toggled = true;
        s->interrupta |= F_I_TOGDONE;
    }
    fusb_int(s);
}

void fusb302_attach(I2CSlave *dev, int rp_ma)
{
    Fusb302State *s = FUSB302(dev);

    s->rp_ma = rp_ma;
    s->toggled = false;
    fusb_look(s);
}

static uint8_t fusb_read(Fusb302State *s, uint8_t a)
{
    uint8_t v;

    if (a < F_SETTINGS_END) {
        return a == F_RESET ? 0 : s->r[a];
    }
    switch (a) {
    case F_STATUS1A:
        return s->toggled ? F_TOGSS_SNK1 : 0;
    case F_INTERRUPTA:
        v = s->interrupta;
        s->interrupta = 0;
        fusb_int(s);
        return v;
    case F_STATUS0:
        return fusb_status0(s);
    case F_STATUS1:
        return F_STATUS1_EMPTY;     /* no PD message either way */
    case F_INTERRUPT:
        v = s->interrupt;
        s->interrupt = 0;
        fusb_int(s);
        return v;
    }
    return 0;
}

static void fusb_write(Fusb302State *s, uint8_t a, uint8_t v)
{
    if (a == F_RESET) {
        if (v & F_SW_RES) {
            memcpy(s->r, fusb_reset_values, sizeof(s->r));
            s->toggled = false;
            s->interrupt = s->interrupta = 0;
        }                           /* PD_RESET: its FIFOs are empty anyway */
    } else if (a > F_DEVICE_ID && a < F_SETTINGS_END) {
        if (a == F_CONTROL2 && !(v & F_TOGGLE)) {
            s->toggled = false;     /* toggling stopped: it starts again */
        }
        s->r[a] = v;
    } else {
        return;                     /* the TX FIFO: nobody to send to */
    }
    fusb_look(s);                   /* what it measures, how it toggles */
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
    s->toggled = false;
    s->interrupt = s->interrupta = 0;
    s->status0 = fusb_status0(s);
    fusb_int(s);
}

static void fusb_init(Object *obj)
{
    qdev_init_gpio_out(DEVICE(obj), &FUSB302(obj)->int_n, 1);
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
#define H_ATTACHED_SNK      (2 << 6)    /* CONTROL: attached as a UFP */
#define H_MODE_DFP          (2 << 4)    /* GENERAL: a source only */

static const uint8_t hd_id[8] = "0223BSU";      /* "USB3220", reversed */

struct Hd3ss3220State {
    I2CSlave parent_obj;
    uint8_t status, control, general;
    uint8_t ptr;
    bool have_ptr;
    int rp_ma;                      /* the partner's Rp: none, 0 */
};

/* attached or not, as the partner and its mode have it */
static void hd_look(Hd3ss3220State *s)
{
    bool attached = s->rp_ma && (s->general & 0x30) != H_MODE_DFP;
    uint8_t detect = !s->rp_ma ? 0 : s->rp_ma >= 3000 ? 3 :
                     s->rp_ma >= 1500 ? 1 : 0;

    if (attached != !!(s->control & H_ATTACHED_SNK)) {
        s->control = (s->control & ~0xe0) | H_INTERRUPT_STATUS |
                     (attached ? H_ATTACHED_SNK : 0);   /* CC1: CABLE_DIR 0 */
    }
    s->status = (s->status & ~0x30) | (attached ? detect << 4 : 0);
}

void hd3ss3220_attach(I2CSlave *dev, int rp_ma)
{
    Hd3ss3220State *s = HD3SS3220(dev);

    s->rp_ma = rp_ma;
    hd_look(s);
}

static uint8_t hd_read(Hd3ss3220State *s, uint8_t a)
{
    if (a < 8) {
        return hd_id[a];
    }
    switch (a) {
    case H_STATUS: return s->status;
    case H_CONTROL: return s->control;
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
            hd_look(s);
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
    hd_look(s);
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
        .instance_init = fusb_init,
        .class_init = fusb_class_init,
    }, {
        .name = TYPE_HD3SS3220,
        .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Hd3ss3220State),
        .class_init = hd_class_init,
    },
};
DEFINE_TYPES(usb_c_types)
