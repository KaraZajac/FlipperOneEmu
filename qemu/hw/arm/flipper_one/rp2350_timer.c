/*
 * The RP2350's TIMER0 and TIMER1
 *
 * A 64-bit count of microseconds: the virtual clock's, while the TICKS
 * block's tick for it runs (1 µs, as the boot ROM and the SDK start it:
 * from a reset the count stands still until then).  TIMEHW then TIMELW
 * set it; reading TIMELR latches the high
 * word for TIMEHR; TIMERAWH and TIMERAWL read it as it is.  Four alarms
 * fire when the count's low word reaches them: INTR, the interrupt
 * (TIMER0_IRQ_0..3 = 0..3, TIMER1's = 4..7), ARMED cleared.  PAUSE stops
 * the count; SOURCE (clk_sys instead of the tick) is kept, not acted on.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "rp2350.h"

#define T_ARMED     (0x20 / 4)
#define T_PAUSE     (0x30 / 4)
#define T_INTR      (0x3c / 4)
#define T_INTE      (0x40 / 4)
#define T_INTF      (0x44 / 4)

/* the count standing still: PAUSE, or its tick not running */
static bool timer_stopped(RP2Timer *t)
{
    return (t->regs.regs[T_PAUSE] & 1) || !t->ticking;
}

static uint64_t timer_now(RP2Timer *t)
{
    if (timer_stopped(t)) {
        return t->paused_at;
    }
    return qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + t->offset;
}

static void timer_irqs(RP2Timer *t)
{
    uint32_t ints = (t->regs.regs[T_INTR] | t->regs.regs[T_INTF]) &
                    t->regs.regs[T_INTE];

    for (int i = 0; i < 4; i++) {
        qemu_set_irq(t->irq[i], (ints >> i) & 1);
    }
}

/* when the low word next equals alarm @n's: its virtual-clock time */
static void timer_arm(RP2Timer *t, int n)
{
    uint32_t delta;

    if (!(t->regs.regs[T_ARMED] & (1u << n)) || timer_stopped(t)) {
        timer_del(t->alarm[n]);
        return;
    }
    delta = t->regs.regs[0x10 / 4 + n] - (uint32_t)timer_now(t);
    timer_mod(t->alarm[n], (qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + delta) *
                           1000);
}

static void timer_fire(RP2Timer *t, int n)
{
    t->regs.regs[T_ARMED] &= ~(1u << n);
    t->regs.regs[T_INTR] |= 1u << n;
    timer_irqs(t);
}

static void timer_fire0(void *opaque) { timer_fire(opaque, 0); }
static void timer_fire1(void *opaque) { timer_fire(opaque, 1); }
static void timer_fire2(void *opaque) { timer_fire(opaque, 2); }
static void timer_fire3(void *opaque) { timer_fire(opaque, 3); }

static uint32_t timer_read(RP2Block *b, hwaddr reg)
{
    RP2Timer *t = container_of(b, RP2Timer, regs);
    uint64_t now;

    switch (reg) {
    case 0x08:                                      /* TIMEHR */
        return t->latched_hi;
    case 0x0c:                                      /* TIMELR: latch */
        now = timer_now(t);
        t->latched_hi = now >> 32;
        return now;
    case 0x24:                                      /* TIMERAWH */
        return timer_now(t) >> 32;
    case 0x28:                                      /* TIMERAWL */
        return timer_now(t);
    case 0x48:                                      /* INTS */
        return (b->regs[T_INTR] | b->regs[T_INTF]) & b->regs[T_INTE];
    default:
        return reg < 0x4c ? b->regs[reg / 4] : 0;
    }
}

static void timer_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2Timer *t = container_of(b, RP2Timer, regs);
    uint64_t now;

    switch (reg) {
    case 0x00:                                      /* TIMEHW */
        t->write_hi = value;
        break;
    case 0x04:                                      /* TIMELW: set it */
        now = (uint64_t)t->write_hi << 32 | value;
        t->offset = now - qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
        t->paused_at = now;
        for (int i = 0; i < 4; i++) {
            timer_arm(t, i);
        }
        break;
    case 0x10 ... 0x1c:                             /* ALARMn: arms it */
        b->regs[reg / 4] = value;
        b->regs[T_ARMED] |= 1u << ((reg - 0x10) / 4);
        timer_arm(t, (reg - 0x10) / 4);
        break;
    case 0x20:                                      /* ARMED: W1C disarms */
        b->regs[T_ARMED] &= ~(value & 0xf);
        for (int i = 0; i < 4; i++) {
            timer_arm(t, i);
        }
        break;
    case 0x30:                                      /* PAUSE */
        now = timer_now(t);
        b->regs[T_PAUSE] = value & 1;
        t->paused_at = now;
        t->offset = now - qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
        for (int i = 0; i < 4; i++) {
            timer_arm(t, i);
        }
        break;
    case 0x3c:                                      /* INTR: W1C */
        b->regs[T_INTR] &= ~(value & 0xf);
        timer_irqs(t);
        break;
    case 0x40: case 0x44:                           /* INTE, INTF */
        b->regs[reg / 4] = value & 0xf;
        timer_irqs(t);
        break;
    case 0x2c: case 0x34: case 0x38:                /* DBGPAUSE, LOCKED, SOURCE */
        b->regs[reg / 4] = value;
        break;
    }
}

/* TICKS: the timer's tick (TIMER@i's) started or stopped */
void rp2350_timer_tick(RP2350State *s, int i, bool running)
{
    RP2Timer *t = &s->timer[i];
    uint64_t now = timer_now(t);

    t->ticking = running;
    t->paused_at = now;
    t->offset = now - qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    for (int n = 0; n < 4; n++) {
        timer_arm(t, n);
    }
}

void rp2350_timer_realize(RP2350State *s)
{
    static QEMUTimerCB *const fire[4] = {
        timer_fire0, timer_fire1, timer_fire2, timer_fire3,
    };

    for (int i = 0; i < 2; i++) {
        RP2Timer *t = &s->timer[i];

        t->index = i;
        rp2_block_init(s, &t->regs, i ? "rp2350.timer1" : "rp2350.timer0",
                       i ? 0x400b8000 : 0x400b0000, timer_read, timer_write);
        for (int n = 0; n < 4; n++) {
            t->alarm[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL, fire[n], t);
            t->irq[n] = rp2350_irq(s, i * 4 + n);
        }
    }
}

void rp2350_timer_reset(RP2350State *s)
{
    for (int i = 0; i < 2; i++) {
        RP2Timer *t = &s->timer[i];

        memset(t->regs.regs, 0, sizeof(t->regs.regs));
        t->regs.regs[0x2c / 4] = 0x6;               /* DBGPAUSE */
        t->offset = -qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
        t->write_hi = t->latched_hi = 0;
        t->paused_at = 0;
        t->ticking = false;                         /* (TICKS: stopped) */
        for (int n = 0; n < 4; n++) {
            timer_del(t->alarm[n]);
        }
        timer_irqs(t);
    }
}
