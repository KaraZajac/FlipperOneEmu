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
 * Its input is VBUS (VAC1 tied to it, no ACFET): a source plugged in
 * (bq2579x_input()) is present at once, good 30 ms later, when its D+/D-
 * detection starts, and known 300 ms after that: a dedicated charger (DCP,
 * D+ and D- shorted). The input current limit is the register's, as the
 * firmware writes it (the detection's own setting is not modelled).
 *
 * The power flow (the battery's "flow", battery.c): with no input, the
 * battery feeds the system. With one, the converter (90 % efficient, as
 * assumed) feeds the system from it, and charges the battery with what
 * is left, up to what the charge cycle asks for: the precharge current
 * below 71.4 % of VREG, then ICHG (fast charge, CC), until the pack's
 * resistance (0.15 Ω) at VREG holds the current below that (taper, CV),
 * then done, below ITERM or full; again when the voltage falls VRECHG below
 * VREG. Not enough input for the system and the battery: the input
 * current is limited (IINDPM) and the battery gets less, or makes up the
 * difference. EN_CHG off: no charge; EN_HIZ: no input.
 *
 * The status registers follow; each change of a status sets its flag, as
 * the datasheet has them (on any change, or on the rising edge), and a flag
 * set that its mask lets through pulses INT low for 256 µs (it goes to the
 * main board's expander, which latches it). The ADC reads the input (VBUS,
 * VAC1, IBUS), the battery's side (VBAT, IBAT + into it, VSYS), TS from the
 * 10 kΩ NTC at the battery's temperature through the board's divider (as
 * the firmware works it out: 5.23 kΩ and 31.6 kΩ), the die at 30 °C.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "flipper_one.h"

#define TYPE_BQ2579X "bq2579x"
OBJECT_DECLARE_SIMPLE_TYPE(Bq2579xState, BQ2579X)

enum {
    R_VSYSMIN = 0x00, R_VREG = 0x01, R_ICHG = 0x03, R_IINDPM = 0x06,
    R_PRECHG = 0x08, R_TERMINATION = 0x09, R_RECHG = 0x0a,
    R_CTRL0 = 0x0f, R_CTRL1 = 0x10, R_ICO_LIMIT = 0x19,
    R_STATUS0 = 0x1b, R_FLAG0 = 0x22, R_FLAG_END = 0x28, R_MASK0 = 0x28,
    R_IBUS = 0x31, R_IBAT = 0x33, R_VBUS = 0x35, R_VAC1 = 0x37, R_VAC2 = 0x39,
    R_VBAT = 0x3b, R_VSYS = 0x3d, R_TS = 0x3f, R_TDIE = 0x41,
    R_DPLUS = 0x43, R_DMINUS = 0x45, R_PART = 0x48, R_NUM
};

#define TERM_REG_RST    0x40
#define CTRL0_EN_CHG    0x20
#define CTRL0_EN_HIZ    0x04
#define CTRL1_WD_RST    0x08

/* STATUS0 ... 4 (0x1b ... 0x1f) */
#define S0_VBUS_PRESENT 0x01
#define S0_AC1_PRESENT  0x02
#define S0_PG           0x08
#define S0_IINDPM       0x80
#define S1_BC12_DONE    0x01
#define S1_VBUS_DCP     (3 << 1)
#define S2_VBAT_PRESENT 0x01
#define S2_DPDM         0x02
#define S3_VSYS         0x10

enum { CHG_NONE, CHG_TRICKLE, CHG_PRE, CHG_FAST, CHG_TAPER, CHG_DONE = 7 };
enum { IN_NONE, IN_PRESENT, IN_DETECTING, IN_DONE };

#define PG_NS           30000000                /* present to good */
#define DPDM_NS         300000000               /* D+/D- detection */
#define INT_NS          256000                  /* INT's pulse */
#define EFFICIENCY      0.9

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

    int vbus_mv, src_ma;            /* the source on VBUS: none, 0 */
    int input;                      /* IN_...: how far it is known */
    int chg;                        /* CHG_...: the charge cycle */
    bool iindpm;                    /* the input current is the limit */
    bool done;                      /* terminated, until a recharge */
    int ibus_ma;
    uint8_t seen[5];                /* the status as last flagged */
    QEMUTimer *input_timer, *int_timer;
    qemu_irq int_out;               /* 0: asserted */
};

static int bq_reg16(Bq2579xState *s, uint8_t a)
{
    return s->r[a] << 8 | s->r[a + 1];
}

/* TS: the NTC (10 kΩ at 25 °C, β 3380) with 31.6 kΩ across it, below
 * 5.23 kΩ from REGN: its part of REGN, in 1/1024 */
static uint16_t bq_ts(const FoBattery *b)
{
    double t = b->temp_dk / 10.0;
    double ntc = 10000.0 * exp(3380.0 * (1.0 / t - 1.0 / 298.15));
    double below = 1.0 / (1.0 / ntc + 1.0 / 31600.0);

    return lround(below / (below + 5230.0) * 1024.0);
}

static bool bq_powered(Bq2579xState *s)
{
    return s->input == IN_DONE && !(s->r[R_CTRL0] & CTRL0_EN_HIZ);
}

static void bq_status(Bq2579xState *s, uint8_t st[5])
{
    bool present = s->input != IN_NONE;
    int vbat = s->bat ? fo_battery_mv(s->bat) : 0;

    st[0] = (present ? S0_VBUS_PRESENT | S0_AC1_PRESENT : 0) |
            (s->input >= IN_DETECTING ? S0_PG : 0) |
            (s->iindpm ? S0_IINDPM : 0);
    st[1] = s->chg << 5 |
            (s->input == IN_DONE ? S1_VBUS_DCP | S1_BC12_DONE : 0);
    st[2] = S2_VBAT_PRESENT | (s->input == IN_DETECTING ? S2_DPDM : 0);
    st[3] = present && vbat < (s->r[R_VSYSMIN] & 0x3f) * 250 + 2500 ?
            S3_VSYS : 0;
    st[4] = 0;                                  /* TS: normal */
}

static void bq_int_end(void *opaque)
{
    Bq2579xState *s = opaque;

    qemu_set_irq(s->int_out, 1);
}

/* the statuses' changes to their flags; a flag let through pulses INT */
static void bq_flag(Bq2579xState *s)
{
    uint8_t st[5], ch[5], fl[4];
    bool pulse = false;

    bq_status(s, st);
    for (int i = 0; i < 5; i++) {
        ch[i] = st[i] ^ s->seen[i];
    }
    fl[0] = (ch[0] & 0x0f) | (ch[0] & st[0] & 0xf0);     /* any; rising */
    fl[1] = (ch[1] & S1_BC12_DONE ? 0x01 : 0) |          /* BC1.2_DONE */
            (ch[2] & S2_VBAT_PRESENT ? 0x02 : 0) |       /* VBAT_PRESENT */
            (ch[1] & 0x1e ? 0x10 : 0) |                  /* VBUS_STAT */
            (ch[1] & 0xe0 ? 0x80 : 0);                   /* CHG_STAT */
    fl[2] = (ch[2] & s->seen[2] & S2_DPDM ? 0x40 : 0) |  /* DPDM done */
            (ch[3] & S3_VSYS ? 0x10 : 0);                /* VSYS */
    fl[3] = 0;
    memcpy(s->seen, st, sizeof(s->seen));
    for (int i = 0; i < 4; i++) {
        s->r[R_FLAG0 + i] |= fl[i];
        pulse |= fl[i] & ~s->r[R_MASK0 + i];
    }
    if (pulse) {
        qemu_set_irq(s->int_out, 0);
        timer_mod(s->int_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + INT_NS);
    }
}

/* the battery's current (+ into it), as the charger has things now */
static int bq_flow(void *opaque, FoBattery *b)
{
    Bq2579xState *s = opaque;
    int ocv = fo_battery_ocv_mv(b);
    int vreg = (bq_reg16(s, R_VREG) & 0x7ff) * 10;
    int ibat = -b->load_ma, want = 0, chg = CHG_NONE;
    bool iindpm = false;

    s->ibus_ma = 0;
    if (bq_powered(s)) {
        int iin = MIN((bq_reg16(s, R_IINDPM) & 0x1ff) * 10, s->src_ma);
        double pin = s->vbus_mv * iin * EFFICIENCY / 1000.0;    /* mW */
        double psys = ocv * b->load_ma / 1000.0;
        double room = (pin - psys) * 1000.0 / ocv;              /* mA */
        int ichg = (bq_reg16(s, R_ICHG) & 0x1ff) * 10;
        int iterm = (s->r[R_TERMINATION] & 0x1f) * 40;
        int cv = (vreg - ocv) * 1000 / 150;     /* what VREG lets in */

        if (s->done && ocv < vreg - ((s->r[R_RECHG] & 0x0f) * 50 + 50)) {
            s->done = false;                    /* recharge */
        }
        if (!(s->r[R_CTRL0] & CTRL0_EN_CHG)) {
            chg = CHG_NONE;
        } else if (s->done) {
            chg = CHG_DONE;
        } else if (ocv < vreg * 714 / 1000) {
            chg = CHG_PRE;
            want = (s->r[R_PRECHG] & 0x3f) * 40;
        } else if (cv >= ichg) {
            chg = CHG_FAST;
            want = ichg;
        } else if (cv >= iterm && b->mah < b->design_mah) {
            chg = CHG_TAPER;
            want = cv;
        } else {
            s->done = true;
            chg = CHG_DONE;
        }
        ibat = MIN(want, (int)room);    /* short of input: less, or out */
        iindpm = room < want || room < 0;
        s->ibus_ma = MIN(lround((psys + MAX(ibat, 0) * ocv / 1000.0) /
                                EFFICIENCY / s->vbus_mv * 1000), iin);
    }
    s->chg = chg;
    s->iindpm = iindpm;
    bq_flag(s);
    return ibat;
}

/* the input's next step: good, then known */
static void bq_input_step(void *opaque)
{
    Bq2579xState *s = opaque;

    if (s->input == IN_PRESENT) {
        s->input = IN_DETECTING;
        timer_mod(s->input_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DPDM_NS);
    } else if (s->input == IN_DETECTING) {
        s->input = IN_DONE;
    }
    if (s->bat) {
        fo_battery_update(s->bat);              /* it flags the change */
    } else {
        bq_flag(s);
    }
}

void bq2579x_input(I2CSlave *dev, int mv, int ma)
{
    Bq2579xState *s = BQ2579X(dev);

    if (s->bat) {
        fo_battery_update(s->bat);              /* the charge up to now */
    }
    s->vbus_mv = mv;
    s->src_ma = ma;
    timer_del(s->input_timer);
    if (mv > 0) {
        s->input = IN_PRESENT;
        timer_mod(s->input_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PG_NS);
    } else {
        s->input = IN_NONE;
    }
    s->done = false;
    if (s->bat) {
        fo_battery_update(s->bat);
    } else {
        bq_flag(s);
    }
}

/* a 16-bit ADC register (at its even address) */
static uint16_t bq_adc(Bq2579xState *s, uint8_t reg)
{
    static const FoBattery none = { .temp_dk = 2982 };
    const FoBattery *b = s->bat ? s->bat : &none;
    bool present = s->input != IN_NONE;
    int vbat = fo_battery_mv(b);
    int vsysmin = (s->r[R_VSYSMIN] & 0x3f) * 250 + 2500;

    switch (reg) {
    case R_IBUS: return s->ibus_ma;
    case R_IBAT: return b->ibat_ma;
    case R_VBUS: case R_VAC1: return present ? s->vbus_mv : 0;
    case R_VBAT: return vbat;
    case R_VSYS: return bq_powered(s) ? MAX(vbat, vsysmin) : vbat;
    case R_TS: return bq_ts(b);
    case R_TDIE: return 60;                     /* 30 °C, 0.5 °C a step */
    default: return 0;                          /* VAC2, D+, D- */
    }
}

static uint8_t bq_byte(Bq2579xState *s, uint8_t a)
{
    uint8_t st[5];

    if (a >= R_IBUS && a < R_IBUS + 22) {
        uint16_t v = bq_adc(s, a - ((a - R_IBUS) & 1));

        return (a - R_IBUS) & 1 ? v & 0xff : v >> 8;
    }
    switch (a) {
    case R_ICO_LIMIT:                   /* ICO: the input current limit */
        return s->r[R_IINDPM];
    case R_ICO_LIMIT + 1:
        return s->r[R_IINDPM + 1];
    case R_STATUS0 ... R_STATUS0 + 4:
        bq_status(s, st);
        return st[a - R_STATUS0];
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

    switch (event) {
    case I2C_START_SEND:
        s->have_ptr = false;
        break;
    case I2C_START_RECV:
    case I2C_FINISH:                    /* as it is now; after a write, the
                                           settings it changed taken */
        if (s->bat) {
            fo_battery_update(s->bat);
        }
        break;
    default:
        break;
    }
    return 0;
}

void bq2579x_attach(I2CSlave *dev, FoBattery *bat)
{
    Bq2579xState *s = BQ2579X(dev);

    s->bat = bat;
    bat->flow = bq_flow;
    bat->flow_opaque = s;
}

static void bq_reset(DeviceState *dev)
{
    Bq2579xState *s = BQ2579X(dev);

    memcpy(s->r, bq_reset_values, sizeof(s->r));
    s->ptr = 0;
    s->have_ptr = false;
    s->chg = CHG_NONE;
    s->iindpm = s->done = false;
    s->ibus_ma = 0;
    bq_status(s, s->seen);
    timer_del(s->int_timer);
    qemu_set_irq(s->int_out, 1);
}

static void bq_init(Object *obj)
{
    Bq2579xState *s = BQ2579X(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->int_out, 1);
    s->input_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bq_input_step, s);
    s->int_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bq_int_end, s);
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
    .instance_init = bq_init,
    .class_init = bq_class_init,
};

static void bq_register(void)
{
    type_register_static(&bq_info);
}

type_init(bq_register)
