/*
 * The RK3576's temperature sensor (TSADC), and how warm the chip is
 *
 * Six channels, as the kernel numbers them: the SoC (by its centre), the
 * big cores, the little cores, DDR, the NPU, the GPU. In auto mode
 * (AUTO_CON bit 0) a channel's code (DATA, 9 bits) follows its temperature
 * as the kernel's table has it: 215 at -40 °C, 285 at 25 °C, 350 at 85 °C,
 * 395 at 125 °C, straight between. Each sensor reads a code or two off, as
 * its trim in the OTP says (the code it read at 30 °C, against the 290 of
 * a true one), and the kernel takes it off again (rk_tsadc_otp_trims gives
 * the OTP them).
 *
 * A code over a channel's COMP_INT ("higher than": at it, the kernel's
 * rounding reads a temperature under the trip it set the alarm for, and
 * sets the same alarm again), its HT_INT_EN bit set, is pending in INT_PD
 * (write 1 to clear) and raises SPI 123; over its COMP_SHUT, its
 * AUTO_SRC_CON bit set, it is pending in HSHUT_PD and, by
 * HSHUT_CRU_INT_EN, resets the chip as its CRU does (the board's to do); by
 * HSHUT_GPIO_INT_EN it would drive the TSHUT pin, which the board does not
 * wire: logged. The control registers take the bits they change as a mask
 * in their top halves, as the kernel writes them. Samples are ten a
 * second, not the hardware's 400, and none is debounced.
 *
 * How warm: 25 °C around it unless the board says otherwise (the panel's
 * `ambient`); the chip's own 0.4 W at rest, and for each core that is
 * running (not waiting for an interrupt) as a sample is taken, 0.26 W for
 * a Cortex-A72 and 0.09 W for an A53 at its cluster's top clock (the CPU
 * rails' currents in the board's table), less at a lower one, as
 * rk3576_cluster_power has it: cpufreq's cooling cools. 20 °C a watt over
 * the ambient for the whole chip, 8 more a watt of its own for a cluster's
 * sensor, reached with a time constant of 10 s: about 34 °C at rest,
 * 60-70 °C with every core busy at its top clock. Nominal, not measured.
 * Off, the chip cools to the ambient.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "rk3576.h"

#define AUTO_CON        0x004
#define AUTO_SRC_CON    0x00c
#define HT_INT_EN       0x014
#define HSHUT_GPIO_EN   0x018
#define HSHUT_CRU_EN    0x01c
#define INT_PD          0x024
#define HSHUT_PD        0x028
#define DATA(n)         (0x02c + (n) * 4)
#define COMP_INT(n)     (0x06c + (n) * 4)
#define COMP_SHUT(n)    (0x10c + (n) * 4)

#define AUTO_EN         BIT(0)
#define CODE_MAX        0x1ff
#define N               RK3576_TSADC_CHANNELS
#define CH_BIG          1
#define CH_LITTLE       2

#define SAMPLE_NS       (100 * SCALE_MS)
#define AMBIENT_C       25.0
#define REST_W          0.4
#define A72_W           0.2625          /* 350 mA at 0.75 V */
#define A53_W           0.09            /* 120 mA at 0.75 V */
#define CHIP_C_PER_W    20.0
#define CLUSTER_C_PER_W 8.0
#define TAU_S           10.0

/* the kernel's table (rk3588_code_table): a code at a temperature */
static const struct {
    int code;
    double c;
} tsadc_table[] = {
    { 215, -40.0 }, { 285, 25.0 }, { 350, 85.0 }, { 395, 125.0 },
};

/* each sensor's error, in codes, and where its trim is in the OTP's
 * non-secure part (the device tree's cells) */
static const struct {
    int8_t off;
    uint8_t cell;
} tsadc_sensors[N] = {
    { 1, 0x64 }, { -1, 0x24 }, { 2, 0x26 }, { 0, 0x28 }, { -2, 0x2a },
    { 1, 0x2c },
};
#define TRUE_30C        290             /* a true sensor's code at 30 °C */

void rk_tsadc_otp_trims(uint8_t *otp)
{
    for (int ch = 0; ch < N; ch++) {
        stw_le_p(otp + tsadc_sensors[ch].cell,
                 TRUE_30C + tsadc_sensors[ch].off);
    }
}

/* what channel @ch reads now */
static uint32_t tsadc_code(RkTsadc *t, int ch)
{
    double c = t->temp[ch];
    int last = ARRAY_SIZE(tsadc_table) - 1, code = tsadc_table[last].code;

    if (c <= tsadc_table[0].c) {
        code = tsadc_table[0].code;
    }
    for (int i = 1; i <= last && c > tsadc_table[0].c; i++) {
        if (c <= tsadc_table[i].c) {
            code = tsadc_table[i - 1].code +
                   lround((c - tsadc_table[i - 1].c) *
                          (tsadc_table[i].code - tsadc_table[i - 1].code) /
                          (tsadc_table[i].c - tsadc_table[i - 1].c));
            break;
        }
    }
    return MIN(MAX(code + tsadc_sensors[ch].off, 0), CODE_MAX);
}

static void tsadc_irq(RkTsadc *t)
{
    qemu_set_irq(t->irq, (t->regs[INT_PD / 4] & MAKE_64BIT_MASK(0, N)) != 0);
}

/* a sample of every channel: their codes, an alarm, a shutdown */
static void tsadc_sample(RkTsadc *t)
{
    RK3576State *s = t->soc;
    bool reset = false;

    for (int ch = 0; ch < N; ch++) {
        uint32_t code = tsadc_code(t, ch);

        t->regs[DATA(ch) / 4] = code;
        if (t->regs[HT_INT_EN / 4] & BIT(ch) &&
            code > t->regs[COMP_INT(ch) / 4]) {
            t->regs[INT_PD / 4] |= BIT(ch);
        }
        if (t->regs[AUTO_SRC_CON / 4] & BIT(ch) &&
            code > t->regs[COMP_SHUT(ch) / 4] &&
            !(t->regs[HSHUT_PD / 4] & BIT(ch))) {
            t->regs[HSHUT_PD / 4] |= BIT(ch);
            if (t->regs[HSHUT_CRU_EN / 4] & BIT(ch)) {
                qemu_log("rk3576: TSHUT: channel %d at %.1f °C: the CRU "
                         "resets the chip\n", ch, t->temp[ch]);
                reset = true;
            } else if (t->regs[HSHUT_GPIO_EN / 4] & BIT(ch)) {
                qemu_log_mask(LOG_UNIMP, "rk3576: TSHUT: channel %d at "
                              "%.1f °C: the TSHUT pin (not wired)\n", ch,
                              t->temp[ch]);
            }
        }
    }
    tsadc_irq(t);
    if (reset && s->cru_reset) {
        s->cru_reset(s->cru_reset_opaque);
    }
}

/* the chip warms or cools toward what it draws now */
static void tsadc_tick(void *opaque)
{
    RkTsadc *t = opaque;
    RK3576State *s = t->soc;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double k = 1.0 - exp(-(now - t->last_ns) / 1e9 / TAU_S);
    double big = 0, little = 0, chip = 0;
    bool on = false;

    t->last_ns = now;
    for (int i = 0; i < RK3576_NCPUS; i++) {
        on |= s->cpu[i].power_state != PSCI_OFF;
        if (!qatomic_read(&CPU(&s->cpu[i])->halted)) {
            if (i < 4) {
                little += A53_W * rk3576_cluster_power(s, 0);
            } else {
                big += A72_W * rk3576_cluster_power(s, 1);
            }
        }
    }
    if (on) {
        chip = REST_W + big + little;
    } else {
        big = little = 0;
    }
    for (int ch = 0; ch < N; ch++) {
        double target = t->ambient + CHIP_C_PER_W * chip +
                        CLUSTER_C_PER_W * (ch == CH_BIG ? big :
                                           ch == CH_LITTLE ? little : 0);

        t->temp[ch] += (target - t->temp[ch]) * k;
    }
    if (t->regs[AUTO_CON / 4] & AUTO_EN) {
        tsadc_sample(t);
    }
    timer_mod(t->timer, now + SAMPLE_NS);
}

static uint64_t tsadc_read(void *opaque, hwaddr off, unsigned size)
{
    RkTsadc *t = opaque;

    if (off >= DATA(0) && off < DATA(N) &&
        t->regs[AUTO_CON / 4] & AUTO_EN) {
        return tsadc_code(t, (off - DATA(0)) / 4);  /* (as it is now) */
    }
    return t->regs[off / 4];
}

static void tsadc_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkTsadc *t = opaque;
    uint32_t *r = &t->regs[off / 4];
    bool was_on = t->regs[AUTO_CON / 4] & AUTO_EN;

    switch (off) {
    case AUTO_CON:
    case AUTO_SRC_CON:
    case HT_INT_EN:
    case HSHUT_GPIO_EN:
    case HSHUT_CRU_EN:
        *r = (*r & ~(v >> 16)) | (v & (v >> 16));
        break;
    case INT_PD:
    case HSHUT_PD:
        *r &= ~v;                                   /* write 1 to clear */
        break;
    default:
        if (off < DATA(0) || off >= DATA(N)) {      /* (DATA: read-only) */
            *r = v;
        }
    }
    if (!was_on && t->regs[AUTO_CON / 4] & AUTO_EN) {
        tsadc_sample(t);                            /* the first at once */
    } else {
        tsadc_irq(t);
    }
}

static const MemoryRegionOps tsadc_ops = {
    .read = tsadc_read,
    .write = tsadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void rk_tsadc_reset(RkTsadc *t)
{
    memset(t->regs, 0, sizeof(t->regs));
    tsadc_irq(t);
}

void rk_tsadc_init(RK3576State *s, RkTsadc *t, hwaddr base, qemu_irq irq)
{
    t->soc = s;
    t->irq = irq;
    t->ambient = AMBIENT_C;
    for (int ch = 0; ch < N; ch++) {
        t->temp[ch] = t->ambient;
    }
    memory_region_init_io(&t->mr, OBJECT(s), &tsadc_ops, t, "rk3576.tsadc",
                          sizeof(t->regs));
    memory_region_add_subregion(get_system_memory(), base, &t->mr);
    t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tsadc_tick, t);
    t->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(t->timer, t->last_ns + SAMPLE_NS);
}
