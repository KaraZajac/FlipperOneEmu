/*
 * TI DRV2605L: the haptic driver, on I2C
 *
 * The Flipper One's control bus has it at 0x5A, its EN on the MCU's GPIO7
 * (its PWM/trigger input, GPIO8, the firmware leaves alone). 8-bit
 * registers; the address goes up a register a byte, read or written.
 *
 * Its motor runs (the "motor" output, 1: running) while EN is high, the
 * chip is out of standby and it plays: in internal-trigger mode, from GO
 * until the waveform sequencer's last entry (WAV_FRM_SEQ1-8: an effect from
 * the ROM's library, or bit 7 and a wait of 10 ms a step, ending at 0);
 * in real-time playback, while RTP_INPUT is not 0; in auto-calibration or
 * diagnostics, from GO until they end (AUTO_CAL_TIME's middle; 100 ms). GO
 * reads 1 until then, and the firmware's start waits for that. Both
 * succeed (DIAG_RESULT 0) and leave A_CAL_COMP, A_CAL_BEMF and BEMF_GAIN
 * as they were. MODE's DEV_RESET puts every register back as it starts.
 *
 * How long each library effect lasts is not published: here a click, tick
 * or bump 60 ms, a double 150 ms, a triple or long double 250 ms, a buzz
 * or hum 400 ms, pulsing 500 ms, the alerts as named (750 ms, 1000 ms),
 * the ramps by their length (1000, 500, 250 ms), and effect 118 until GO
 * is cleared. Not modelled: the external triggers, PWM and audio inputs,
 * the supply's measure (VBAT reads 0) and the LRA's resonance period.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_DRV2605L "drv2605l"
OBJECT_DECLARE_SIMPLE_TYPE(Drv2605lState, DRV2605L)

enum {
    R_STATUS = 0x00, R_MODE = 0x01, R_RTP_INPUT = 0x02, R_WAV_FRM_SEQ1 = 0x04,
    R_GO = 0x0c, R_CONTROL4 = 0x1e, R_VBAT = 0x21, R_LRA_PERIOD = 0x22,
    R_NUM
};

#define MODE_DEV_RESET      0x80
#define MODE_STANDBY        0x40
#define MODE_INTERNAL       0
#define MODE_RTP            5
#define MODE_DIAGNOSTICS    6
#define MODE_AUTO_CAL       7
#define STATUS_DIAG_RESULT  0x08

/* the registers as the chip starts (the datasheet's reset values) */
static const uint8_t drv_reset_values[R_NUM] = {
    [0x00] = 0xe0, [0x01] = 0x40, [0x03] = 0x01, [0x04] = 0x01,
    [0x11] = 0x05, [0x12] = 0x19, [0x13] = 0xff, [0x14] = 0x19, [0x15] = 0xff,
    [0x16] = 0x3e, [0x17] = 0x8c, [0x18] = 0x0c, [0x19] = 0x6c, [0x1a] = 0x36,
    [0x1b] = 0x93, [0x1c] = 0xf5, [0x1d] = 0xa0, [0x1e] = 0x20, [0x1f] = 0x80,
    [0x20] = 0x33,
};

typedef enum { DRV_IDLE, DRV_SEQUENCE, DRV_WAIT, DRV_CALIBRATING } DrvRun;

struct Drv2605lState {
    I2CSlave parent_obj;

    uint8_t r[R_NUM];
    uint8_t ptr;
    bool have_ptr;
    bool en;                        /* the EN pin */
    DrvRun run;                     /* what GO started, now */
    int step;                       /* the sequencer's next entry */
    bool motor;                     /* as last told */
    QEMUTimer *timer;               /* the end of this effect, wait or run */
    qemu_irq motor_out;
};

/* how long a library effect lasts, in ms (-1: until GO is cleared) */
static int drv_effect_ms(uint8_t e)
{
    static const int ramps[6] = { 1000, 1000, 500, 500, 250, 250 };

    if (e == 118) {
        return -1;
    }
    if (e == 15 || e == 16) {
        return e == 15 ? 750 : 1000;
    }
    if (e >= 70 && e <= 117) {
        return ramps[(e - 70) % 6];
    }
    if (e == 10 || e == 11 || e == 13 || (e >= 27 && e <= 36)) {
        return 150;
    }
    if (e == 12 || (e >= 37 && e <= 46)) {
        return 250;
    }
    if (e >= 52 && e <= 57) {
        return 500;
    }
    if (e == 14 || (e >= 47 && e <= 51) || (e >= 64 && e <= 69) || e >= 119) {
        return 400;
    }
    return 60;                      /* the clicks, ticks and bumps */
}

static void drv_motor(Drv2605lState *s)
{
    uint8_t mode = s->r[R_MODE];
    bool on = s->en && !(mode & MODE_STANDBY) &&
              (s->run == DRV_SEQUENCE || s->run == DRV_CALIBRATING ||
               ((mode & 7) == MODE_RTP && s->r[R_RTP_INPUT]));

    if (on != s->motor) {
        s->motor = on;
        qemu_set_irq(s->motor_out, on);
    }
}

static void drv_after(Drv2605lState *s, int ms)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                        (int64_t)ms * SCALE_MS);
}

static void drv_stop(Drv2605lState *s)
{
    timer_del(s->timer);
    s->run = DRV_IDLE;
    s->r[R_GO] = 0;
    drv_motor(s);
}

/* the sequencer's next entry, or its end */
static void drv_next(Drv2605lState *s)
{
    while (s->step < 8) {
        uint8_t e = s->r[R_WAV_FRM_SEQ1 + s->step++];
        int ms;

        if (!e) {
            break;
        }
        if (e & 0x80) {
            s->run = DRV_WAIT;
            drv_after(s, (e & 0x7f) * 10);
        } else {
            s->run = DRV_SEQUENCE;
            ms = drv_effect_ms(e);
            if (ms < 0) {
                timer_del(s->timer);
            } else {
                drv_after(s, ms);
            }
        }
        drv_motor(s);
        return;
    }
    drv_stop(s);
}

static void drv_timer(void *opaque)
{
    Drv2605lState *s = opaque;

    if (s->run == DRV_CALIBRATING) {
        s->r[R_STATUS] &= ~STATUS_DIAG_RESULT;  /* it passed */
        drv_stop(s);
    } else {
        drv_next(s);
    }
}

static void drv_go(Drv2605lState *s)
{
    static const int cal_ms[4] = { 250, 350, 600, 1100 };
    uint8_t mode = s->r[R_MODE];

    if (mode & MODE_STANDBY) {
        return;                                 /* GO is not taken */
    }
    s->r[R_GO] = 1;
    switch (mode & 7) {
    case MODE_INTERNAL:
        s->step = 0;
        drv_next(s);
        break;
    case MODE_AUTO_CAL:
    case MODE_DIAGNOSTICS:
        s->run = DRV_CALIBRATING;
        drv_after(s, (mode & 7) == MODE_AUTO_CAL ?
                     cal_ms[(s->r[R_CONTROL4] >> 4) & 3] : 100);
        drv_motor(s);
        break;
    default:                                    /* the external triggers */
        break;
    }
}

static void drv_chip_reset(Drv2605lState *s)
{
    memcpy(s->r, drv_reset_values, sizeof(s->r));
    drv_stop(s);
}

static void drv_write(Drv2605lState *s, uint8_t reg, uint8_t v)
{
    switch (reg) {
    case R_STATUS: case R_VBAT: case R_LRA_PERIOD:
        return;                                 /* read-only */
    case R_MODE:
        if (v & MODE_DEV_RESET) {
            drv_chip_reset(s);
            return;
        }
        s->r[R_MODE] = v;
        if (v & MODE_STANDBY) {
            drv_stop(s);
        }
        break;
    case R_GO:
        if ((v & 1) && !s->r[R_GO]) {
            drv_go(s);
        } else if (!(v & 1)) {
            drv_stop(s);
        }
        return;
    default:
        if (reg < R_NUM) {
            s->r[reg] = v;
        }
    }
    drv_motor(s);
}

static uint8_t drv_recv(I2CSlave *i2c)
{
    Drv2605lState *s = DRV2605L(i2c);
    uint8_t v = s->ptr < R_NUM ? s->r[s->ptr] : 0;

    s->ptr++;
    return v;
}

static int drv_send(I2CSlave *i2c, uint8_t data)
{
    Drv2605lState *s = DRV2605L(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        drv_write(s, s->ptr++, data);
    }
    return 0;
}

static int drv_event(I2CSlave *i2c, enum i2c_event event)
{
    Drv2605lState *s = DRV2605L(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static void drv_en(void *opaque, int n, int level)
{
    Drv2605lState *s = opaque;

    s->en = level;
    drv_motor(s);
}

static void drv_reset(DeviceState *dev)
{
    Drv2605lState *s = DRV2605L(dev);

    s->ptr = 0;
    s->have_ptr = false;
    drv_chip_reset(s);
}

static void drv_realize(DeviceState *dev, Error **errp)
{
    Drv2605lState *s = DRV2605L(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, drv_timer, s);
}

static void drv_init(Object *obj)
{
    Drv2605lState *s = DRV2605L(obj);

    qdev_init_gpio_in(DEVICE(obj), drv_en, 1);
    qdev_init_gpio_out(DEVICE(obj), &s->motor_out, 1);
}

static void drv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = drv_event;
    k->recv = drv_recv;
    k->send = drv_send;
    dc->realize = drv_realize;
    device_class_set_legacy_reset(dc, drv_reset);
}

static const TypeInfo drv_info = {
    .name = TYPE_DRV2605L,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Drv2605lState),
    .instance_init = drv_init,
    .class_init = drv_class_init,
};

static void drv_register(void)
{
    type_register_static(&drv_info);
}

type_init(drv_register)
