/*
 * Azoteq IQS7211E: the touchpad's controller, on I2C
 *
 * The Flipper One's control bus has it at 0x56, its RDY on the MCU's GPIO9
 * (open drain; the board pulls it up).  Its registers are 16-bit words,
 * little-endian: a write's first byte is the address of the first, and the
 * address goes up a word every two bytes, read or written.
 *
 * The chip senses the pad a cycle at a time (ACTIVE_MODE_RR ms), puts what
 * it found in its registers and opens a communication window: RDY low
 * until a STOP ends it, or until I2C_TIMEOUT ms go by with no byte from the
 * master.  In stream mode a window comes every cycle; in event mode
 * (CONFIG_SETTINGS' Event Mode) only when the finger came, went or moved
 * (TP Event; TP Touch Event for coming and going).  Through a window the
 * registers keep what it opened with.  The MCU firmware's driver does one
 * step of its start a window (the product number, Show Reset, its
 * settings, Ack Reset, a re-ATI, the data, event mode), then reads the data
 * block (GESTURES ... FINGER_2_AREA) in each.  SYS_CONTROL's SW Reset
 * resets the chip when the window ends; Ack Reset clears Show Reset.
 *
 * The finger comes from the front panel, 0 ... 1000 across and down the
 * pad: scaled to X_RESOLUTION and Y_RESOLUTION, switched and flipped as
 * TP_RX_SETTINGS asks; its strength and area follow the panel's pressure.
 * Without one, its place reads 0xFFFF.  Not modelled: a second finger (the
 * panel has one), gestures, the low-power modes (it stays in Active), the
 * ALP channel, ATI (done at once), and the settings the chip starts with
 * (0 here; a report rate of 0 counts as 10 ms, a timeout of 0 as 100 ms).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_IQS7211E "iqs7211e"
OBJECT_DECLARE_SIMPLE_TYPE(Iqs7211eState, IQS7211E)

enum {
    R_PRODUCT_NUM = 0x00, R_MAJOR_VERSION = 0x01, R_MINOR_VERSION = 0x02,
    R_LIBRARY_NUM = 0x05, R_MAJOR_VERSION_LIBRARY = 0x06,
    R_RELATIVE_X = 0x0a, R_RELATIVE_Y = 0x0b,
    R_GESTURES = 0x0e, R_INFO_FLAGS = 0x0f,
    R_FINGER_1 = 0x10,              /* X, Y, touch strength, area */
    R_FINGER_2 = 0x14,
    R_SETTINGS = 0x1f,              /* the first that can be written */
    R_ACTIVE_MODE_RR = 0x28,
    R_I2C_TIMEOUT = 0x32,
    R_SYS_CONTROL = 0x33,
    R_CONFIG_SETTINGS = 0x34,
    R_TP_RX_SETTINGS = 0x41,
    R_X_RESOLUTION = 0x43, R_Y_RESOLUTION = 0x44,
    R_NUM = 0x7d
};

/* INFO_FLAGS */
#define INFO_SHOW_RESET     (1u << 7)
#define INFO_FINGERS        8               /* bits 9:8 */
#define INFO_TP_MOVEMENT    (1u << 10)

/* SYS_CONTROL: what acts, and does not stay */
#define SYS_COMMANDS        0x02f8          /* reseeds, re-ATIs, resets */
#define SYS_ACK_RESET       (1u << 7)
#define SYS_SW_RESET        (1u << 9)

/* CONFIG_SETTINGS */
#define CFG_EVENT_MODE      (1u << 8)
#define CFG_TP_EVENT        (1u << 10)
#define CFG_TP_TOUCH_EVENT  (1u << 14)

/* TP_RX_SETTINGS */
#define RX_FLIP_X           (1u << 0)
#define RX_FLIP_Y           (1u << 1)
#define RX_SWITCH_XY        (1u << 2)

#define NO_FINGER           0xffff

struct Iqs7211eState {
    I2CSlave parent_obj;

    uint16_t r[R_NUM];
    uint8_t ptr;                    /* the register the next byte is of */
    bool high;                      /* ... and it is the high byte */
    bool have_ptr;                  /* this write's first byte came */
    uint8_t low;                    /* a word's low byte, written */
    bool window;                    /* RDY low */
    bool show_reset;
    bool reset_after;               /* SW Reset: when the window ends */

    /* the finger, as the panel has it (0 ... 1000), and as last reported */
    bool touching;
    int x, y, p;
    bool reported;
    uint16_t rx, ry;

    QEMUTimer *timer;               /* the next window, or the open one's end */
    qemu_irq rdy;                   /* 0: pulled low */
};

static int64_t iqs_after(Iqs7211eState *s, int reg, int otherwise_ms)
{
    int ms = s->r[reg] ? s->r[reg] : otherwise_ms;

    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)ms * SCALE_MS;
}

/* where the finger is, in the chip's coordinates */
static void iqs_place(Iqs7211eState *s, uint16_t *x, uint16_t *y)
{
    uint16_t rx = s->r[R_TP_RX_SETTINGS];
    int xres = s->r[R_X_RESOLUTION], yres = s->r[R_Y_RESOLUTION];
    int across = s->x, down = s->y;

    if (rx & RX_SWITCH_XY) {
        across = s->y;
        down = s->x;
    }
    across = across * xres / 1000;
    down = down * yres / 1000;
    *x = rx & RX_FLIP_X ? xres - across : across;
    *y = rx & RX_FLIP_Y ? yres - down : down;
}

/* whether the pad has something the registers do not say, that the chip
 * would open a window for */
static bool iqs_due(Iqs7211eState *s)
{
    uint16_t cfg = s->r[R_CONFIG_SETTINGS];
    uint16_t x = NO_FINGER, y = NO_FINGER;

    if (!(cfg & CFG_EVENT_MODE)) {
        return true;                            /* stream mode: every cycle */
    }
    if (s->touching) {
        iqs_place(s, &x, &y);
    }
    if (s->touching != s->reported) {
        return cfg & (CFG_TP_EVENT | CFG_TP_TOUCH_EVENT);
    }
    return (cfg & CFG_TP_EVENT) && (x != s->rx || y != s->ry);
}

/* a cycle's end: what the pad has, into the registers */
static void iqs_capture(Iqs7211eState *s)
{
    uint16_t x = NO_FINGER, y = NO_FINGER, info;
    bool moved;

    if (s->touching) {
        iqs_place(s, &x, &y);
    }
    moved = s->touching != s->reported || x != s->rx || y != s->ry;
    s->r[R_RELATIVE_X] = s->touching && s->reported ? x - s->rx : 0;
    s->r[R_RELATIVE_Y] = s->touching && s->reported ? y - s->ry : 0;
    s->r[R_GESTURES] = 0;
    info = (s->show_reset ? INFO_SHOW_RESET : 0) |
           (s->touching ? 1 : 0) << INFO_FINGERS |
           (moved ? INFO_TP_MOVEMENT : 0);
    s->r[R_INFO_FLAGS] = info;
    s->r[R_FINGER_1] = x;
    s->r[R_FINGER_1 + 1] = y;
    s->r[R_FINGER_1 + 2] = s->touching ? MAX(s->p, 1) * 16 : 0;
    s->r[R_FINGER_1 + 3] = s->touching ? 1 + s->p / 32 : 0;
    s->r[R_FINGER_2] = s->r[R_FINGER_2 + 1] = NO_FINGER;
    s->r[R_FINGER_2 + 2] = s->r[R_FINGER_2 + 3] = 0;
    s->reported = s->touching;
    s->rx = x;
    s->ry = y;
}

static void iqs_rdy(Iqs7211eState *s, bool low)
{
    s->window = low;
    qemu_set_irq(s->rdy, !low);
}

/* power on, or SW Reset: Show Reset, the settings anew, stream mode */
static void iqs_chip_reset(Iqs7211eState *s)
{
    memset(s->r, 0, sizeof(s->r));
    s->r[R_PRODUCT_NUM] = 0x0458;
    s->r[R_MAJOR_VERSION] = 1;
    s->r[R_MINOR_VERSION] = 1;
    s->r[R_LIBRARY_NUM] = 206;
    s->r[R_MAJOR_VERSION_LIBRARY] = 4;
    s->show_reset = true;
    s->reset_after = false;
    s->reported = false;
    s->rx = s->ry = NO_FINGER;
    iqs_capture(s);
    iqs_rdy(s, false);
    timer_mod(s->timer, iqs_after(s, R_ACTIVE_MODE_RR, 10));
}

static void iqs_open(Iqs7211eState *s)
{
    iqs_capture(s);
    iqs_rdy(s, true);
    timer_mod(s->timer, iqs_after(s, R_I2C_TIMEOUT, 100));
}

static void iqs_close(Iqs7211eState *s)
{
    iqs_rdy(s, false);
    if (s->reset_after) {
        iqs_chip_reset(s);
    } else if (iqs_due(s)) {
        timer_mod(s->timer, iqs_after(s, R_ACTIVE_MODE_RR, 10));
    } else {
        timer_del(s->timer);
    }
}

/* the next cycle's end, or the master did not come in time */
static void iqs_timer(void *opaque)
{
    Iqs7211eState *s = opaque;

    if (s->window) {
        iqs_close(s);
    } else {
        iqs_open(s);
    }
}

/* the master is talking: the window waits for it again */
static void iqs_busy(Iqs7211eState *s)
{
    if (s->window) {
        timer_mod(s->timer, iqs_after(s, R_I2C_TIMEOUT, 100));
    }
}

static void iqs_write(Iqs7211eState *s, uint8_t reg, uint16_t v)
{
    if (reg < R_SETTINGS || reg >= R_NUM) {
        return;                                 /* read-only, or none */
    }
    if (reg == R_SYS_CONTROL) {
        if (v & SYS_ACK_RESET) {
            s->show_reset = false;
            s->r[R_INFO_FLAGS] &= ~INFO_SHOW_RESET;
        }
        if (v & SYS_SW_RESET) {
            s->reset_after = true;
        }
        v &= ~SYS_COMMANDS;
    }
    s->r[reg] = v;
}

/* a write that ended with half a word: its low byte */
static void iqs_flush(Iqs7211eState *s)
{
    if (s->have_ptr && s->high) {
        iqs_write(s, s->ptr, (s->ptr < R_NUM ? s->r[s->ptr] & 0xff00 : 0) |
                             s->low);
    }
    s->high = false;
}

static uint8_t iqs_recv(I2CSlave *i2c)
{
    Iqs7211eState *s = IQS7211E(i2c);
    uint16_t v = s->ptr < R_NUM ? s->r[s->ptr] : 0;

    iqs_busy(s);
    if (!s->high) {
        s->high = true;
        return v & 0xff;
    }
    s->high = false;
    s->ptr++;
    return v >> 8;
}

static int iqs_send(I2CSlave *i2c, uint8_t data)
{
    Iqs7211eState *s = IQS7211E(i2c);

    iqs_busy(s);
    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
        s->high = false;
    } else if (!s->high) {
        s->low = data;
        s->high = true;
    } else {
        iqs_write(s, s->ptr++, s->low | data << 8);
        s->high = false;
    }
    return 0;
}

static int iqs_event(I2CSlave *i2c, enum i2c_event event)
{
    Iqs7211eState *s = IQS7211E(i2c);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        iqs_flush(s);
        s->have_ptr = event == I2C_START_RECV && s->have_ptr;
        iqs_busy(s);
        break;
    case I2C_FINISH:                            /* STOP: the window ends */
        iqs_flush(s);
        s->have_ptr = false;
        if (s->window) {
            iqs_close(s);
        } else if (s->reset_after) {
            iqs_chip_reset(s);
        }
        break;
    default:
        break;
    }
    return 0;
}

void iqs7211e_touch(I2CSlave *dev, bool touching, int x, int y, int p)
{
    Iqs7211eState *s = IQS7211E(dev);

    s->touching = touching;
    s->x = x;
    s->y = y;
    s->p = p;
    /* in event mode the cycle after a change opens a window */
    if (!s->window && !timer_pending(s->timer) && iqs_due(s)) {
        timer_mod(s->timer, iqs_after(s, R_ACTIVE_MODE_RR, 10));
    }
}

static void iqs_reset(DeviceState *dev)
{
    Iqs7211eState *s = IQS7211E(dev);

    s->ptr = 0;
    s->high = s->have_ptr = false;
    iqs_chip_reset(s);
}

static void iqs_realize(DeviceState *dev, Error **errp)
{
    Iqs7211eState *s = IQS7211E(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, iqs_timer, s);
}

static void iqs_init(Object *obj)
{
    Iqs7211eState *s = IQS7211E(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->rdy, 1);
}

static void iqs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = iqs_event;
    k->recv = iqs_recv;
    k->send = iqs_send;
    dc->realize = iqs_realize;
    device_class_set_legacy_reset(dc, iqs_reset);
}

static const TypeInfo iqs_info = {
    .name = TYPE_IQS7211E,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Iqs7211eState),
    .instance_init = iqs_init,
    .class_init = iqs_class_init,
};

static void iqs_register(void)
{
    type_register_static(&iqs_info);
}

type_init(iqs_register)
