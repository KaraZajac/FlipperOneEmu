/*
 * The RP2350's PWM: twelve slices, kept, and a pin's duty from them
 *
 * The counters do not count and wrap no interrupt: what is modelled is
 * what a pin shows on average, CC over TOP + 1 while the slice is enabled,
 * which the board reads (the screen's backlight on GPIO6) and is told of
 * when a slice changes.  GPIO0-31 are slices 0-7 (twice), GPIO32-47 slices
 * 8-11 (twice); an even pin is channel A.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "rp2350.h"

#define FUNC_PWM 4

static int pwm_slice(int pin)
{
    return pin < 32 ? (pin >> 1) & 7 : 8 + (((pin - 32) >> 1) & 3);
}

int rp2350_pwm_duty(RP2350State *s, int pin)
{
    uint32_t *r = &s->pwm.regs[pwm_slice(pin) * 5];
    uint32_t top = (r[4] & 0xffff) + 1;
    uint32_t cc = pin & 1 ? r[3] >> 16 : r[3] & 0xffff;
    bool inv = (r[0] >> (pin & 1 ? 3 : 2)) & 1;     /* CSR.A_INV, B_INV */
    uint32_t duty;

    if ((s->io_bank0.regs[pin * 2 + 1] & 0x1f) != FUNC_PWM) {
        return -1;
    }
    if (!(r[0] & 1)) {                              /* CSR.EN */
        return inv ? 65535 : 0;
    }
    duty = MIN((uint64_t)cc * 65536 / top, 65535);
    return inv ? 65535 - duty : duty;
}

static void pwm_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2350State *s = b->soc;

    if (reg == 0xf0) {                              /* EN: every CSR.EN */
        for (int i = 0; i < 12; i++) {
            b->regs[i * 5] = deposit32(b->regs[i * 5], 0, 1, (value >> i) & 1);
        }
    } else if (reg < 0xf0 && (reg % 0x14) != 0x8) {
        b->regs[reg / 4] = value;
    } else if (reg > 0xf0 && reg < RP2_REGS * 4) {
        b->regs[reg / 4] = value;
    }
    if (s->pwm_fn) {
        s->pwm_fn(s->pwm_opaque);
    }
}

static uint32_t pwm_read(RP2Block *b, hwaddr reg)
{
    if (reg == 0xf0) {
        uint32_t en = 0;

        for (int i = 0; i < 12; i++) {
            en |= (b->regs[i * 5] & 1) << i;
        }
        return en;
    }
    return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
}

void rp2350_pwm_realize(RP2350State *s)
{
    rp2_block_init(s, &s->pwm, "rp2350.pwm", 0x400a8000, pwm_read, pwm_write);
}

void rp2350_pwm_reset(RP2350State *s)
{
    memset(s->pwm.regs, 0, sizeof(s->pwm.regs));
    for (int i = 0; i < 12; i++) {
        s->pwm.regs[i * 5 + 1] = 0x10;              /* DIV: 1.0 */
        s->pwm.regs[i * 5 + 4] = 0xffff;            /* TOP */
    }
}
