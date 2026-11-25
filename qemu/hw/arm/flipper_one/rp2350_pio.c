/*
 * The RP2350's three PIO blocks, four state machines each
 *
 * An interpreter of the PIO's instructions, the RP2350's included (WAIT on
 * the JMP pin, IRQ to the previous and next block and relative to the state
 * machine, MOV to PINDIRS, MOV between the ISR/OSR and an RX FIFO entry, the
 * STATUS of an IRQ flag), with side-set, delays, wrap, autopush and
 * autopull, joined FIFOs, forced and EXEC'd instructions.
 *
 * Time: each state machine has the time of its next cycle, in picoseconds of
 * the virtual clock, from its clock divider and clk_sys.  A block runs its
 * machines up to the present when a timer of its own comes due, and when
 * anything may have changed for them: the CPU at its registers, a pin's
 * level.  A machine that stalls (a FIFO, a WAIT, an IRQ) costs nothing until
 * then, and goes on at that moment; so does one that jumps to itself, always,
 * which nothing but the CPU or the DMA can move on (a program that ran off
 * its end, into empty instruction memory, is such a loop: JMP 0 at 0).
 * Pins it drives change at the time of the instruction that drove them; the
 * pins (rp2350_gpio.c) take them from pad_out and pad_oe, PIO pin 0 being
 * GPIOBASE.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/clock.h"
#include "rp2350.h"

#define IRQ_PIO0_0          15
#define BUDGET              200000          /* instructions to a run, at most */

#define EXEC_SIDE_EN        (1u << 30)
#define EXEC_SIDE_PINDIR    (1u << 29)
#define EXEC_INLINE_OUT_EN  (1u << 18)
#define EXEC_OUT_STICKY     (1u << 17)
#define SHIFT_FJOIN_RX      (1u << 31)
#define SHIFT_FJOIN_TX      (1u << 30)
#define SHIFT_OUT_RIGHT     (1u << 19)
#define SHIFT_IN_RIGHT      (1u << 18)
#define SHIFT_AUTOPULL      (1u << 17)
#define SHIFT_AUTOPUSH      (1u << 16)

/* ------------------------------------------------------------- the FIFOs */

static int tx_depth(RP2PioSM *sm)
{
    return sm->shiftctrl & SHIFT_FJOIN_TX ? 8 :
           sm->shiftctrl & SHIFT_FJOIN_RX ? 0 : 4;
}

static int rx_depth(RP2PioSM *sm)
{
    return sm->shiftctrl & SHIFT_FJOIN_RX ? 8 :
           sm->shiftctrl & SHIFT_FJOIN_TX ? 0 : 4;
}

static bool tx_push(RP2PioSM *sm, uint32_t v)
{
    if (sm->tx_n >= tx_depth(sm)) {
        return false;
    }
    sm->txf[(sm->tx_head + sm->tx_n++) % 8] = v;
    return true;
}

static bool fifo_moved;                     /* a DREQ may have come */

static uint32_t tx_pop(RP2PioSM *sm)
{
    uint32_t v = sm->txf[sm->tx_head];

    sm->tx_head = (sm->tx_head + 1) % 8;
    sm->tx_n--;
    fifo_moved = true;
    return v;
}

static bool rx_push(RP2PioSM *sm, uint32_t v)
{
    if (sm->rx_n >= rx_depth(sm)) {
        return false;
    }
    sm->rxf[(sm->rx_head + sm->rx_n++) % 8] = v;
    fifo_moved = true;
    return true;
}

static uint32_t rx_pop(RP2PioSM *sm)
{
    uint32_t v = sm->rxf[sm->rx_head];

    sm->rx_head = (sm->rx_head + 1) % 8;
    sm->rx_n--;
    return v;
}

/* --------------------------------------------------------- pins, IRQs */

static int64_t sm_period_ps(RP2350State *s, RP2PioSM *sm)
{
    uint64_t hz = clock_get_hz(s->sysclk) ?: 150000000;
    uint32_t div = sm->clkdiv >> 8;                 /* INT.FRAC, 16.8 */

    if (!(div >> 8)) {
        div |= 0x10000 << 8;                        /* INT 0: 65536 */
    }
    return (int64_t)((uint64_t)div * 1000000000000ull / 256 / hz);
}

static uint32_t pio_pins(RP2350State *s, RP2Pio *p)
{
    uint64_t all = s->pin_level | ((uint64_t)s->pin_level << RP2_PINS);

    return all >> p->gpiobase;                      /* 32 pins from GPIOBASE */
}

static uint32_t sm_in_pins(RP2350State *s, RP2Pio *p, RP2PioSM *sm)
{
    int base = (sm->pinctrl >> 15) & 31;
    uint32_t v = ror32(pio_pins(s, p), base);
    int count = sm->shiftctrl & 31;

    return count ? v & ((1u << count) - 1) : v;
}

/* @count pins from @base (wrapping at 32) to @value's low bits */
static void pins_write(uint32_t *reg, int base, int count, uint32_t value)
{
    for (int i = 0; i < count; i++) {
        *reg = deposit32(*reg, (base + i) & 31, 1, (value >> i) & 1);
    }
}

static void pio_pins_changed(RP2350State *s, int64_t when_ps)
{
    rp2350_pins_update(s, when_ps / 1000);
}

static void pio_irqs(RP2Pio *p)
{
    uint32_t intr = 0;

    for (int i = 0; i < 4; i++) {
        intr |= (p->sm[i].rx_n ? 1u : 0) << i;
        intr |= (p->sm[i].tx_n < tx_depth(&p->sm[i]) ? 1u : 0) << (4 + i);
    }
    intr |= (uint32_t)p->irq << 8;
    for (int i = 0; i < 2; i++) {               /* (it runs at every step:
                                                   a line set only when it
                                                   changes) */
        bool level = ((intr | p->intf[i]) & p->inte[i]) != 0;

        if (level != p->irq_level[i]) {
            p->irq_level[i] = level;
            qemu_set_irq(p->irq_line[i], level);
        }
    }
}

static uint32_t pio_intr(RP2Pio *p)
{
    uint32_t intr = 0;

    for (int i = 0; i < 4; i++) {
        intr |= (p->sm[i].rx_n ? 1u : 0) << i;
        intr |= (p->sm[i].tx_n < tx_depth(&p->sm[i]) ? 1u : 0) << (4 + i);
    }
    return intr | (uint32_t)p->irq << 8;
}

/* an IRQ index of an instruction: the block it is in, and the flag */
static RP2Pio *irq_target(RP2350State *s, RP2Pio *p, int sm, int index,
                          int *flag)
{
    int mode = (index >> 3) & 3;

    *flag = index & 7;
    switch (mode) {
    case 1:                                         /* PREV */
        return &s->pio[(p->index + 2) % 3];
    case 2:                                         /* REL */
        *flag = (index & 4) | ((index + sm) & 3);
        return p;
    case 3:                                         /* NEXT */
        return &s->pio[(p->index + 1) % 3];
    default:
        return p;
    }
}

/* ------------------------------------------------------ the instructions */

/*
 * One instruction of state machine @n at its time: false if it stalled (its
 * side-set took effect anyway, as on the chip).
 */
static bool sm_step(RP2350State *s, RP2Pio *p, int n)
{
    RP2PioSM *sm = &p->sm[n];
    bool exec = sm->has_exec;
    uint8_t pc0 = sm->pc;
    uint16_t insn = exec ? sm->exec : p->imem[sm->pc];
    int ss_count = (sm->pinctrl >> 29) & 7;
    int field = (insn >> 8) & 0x1f;
    int delay = field & ((1 << (5 - ss_count)) - 1);
    int side = field >> (5 - ss_count), side_bits = ss_count;
    bool do_side = ss_count > 0;
    int arg = insn & 0xff;
    int op = insn >> 13;
    bool jumped = false, pins_changed = false, on_pins = false;
    uint32_t v = 0;
    int count, flag;
    RP2Pio *tp;

    if (sm->execctrl & EXEC_SIDE_EN) {
        do_side = (side >> (ss_count - 1)) & 1;
        side_bits = ss_count - 1;
        side &= (1 << side_bits) - 1;
    }
    if (do_side && side_bits) {
        int base = (sm->pinctrl >> 10) & 31;

        pins_write(sm->execctrl & EXEC_SIDE_PINDIR ? &p->pad_oe : &p->pad_out,
                   base, side_bits, side);
        pins_changed = true;
    }

    switch (op) {
    case 0: {                                       /* JMP */
        bool take;

        switch ((arg >> 5) & 7) {
        case 0: take = true; break;
        case 1: take = !sm->x; break;
        case 2: take = sm->x != 0; sm->x--; break;
        case 3: take = !sm->y; break;
        case 4: take = sm->y != 0; sm->y--; break;
        case 5: take = sm->x != sm->y; break;
        case 6:
            take = (pio_pins(s, p) >> ((sm->execctrl >> 24) & 31)) & 1;
            break;
        default:                                    /* !OSRE */
            take = sm->osr_count < (((sm->shiftctrl >> 25) & 31) ?: 32);
            break;
        }
        if (take) {
            sm->pc = arg & 31;
            jumped = true;
            if (!exec && !((arg >> 5) & 7) && sm->pc == pc0) {
                /* a JMP to itself, always: nothing more happens until
                 * something from outside does (the CPU, the DMA), so it
                 * waits for that here rather than running each cycle */
                sm->next_ps += (1 + delay) * sm_period_ps(s, sm);
                on_pins = false;
                goto stall;
            }
        }
        break;
    }
    case 1: {                                       /* WAIT */
        bool pol = arg & 0x80, ok;
        int index = arg & 31;

        switch ((arg >> 5) & 3) {
        case 0:                                     /* GPIO */
            ok = ((pio_pins(s, p) >> index) & 1) == pol;
            break;
        case 1:                                     /* PIN */
            ok = ((sm_in_pins(s, p, sm) >> index) & 1) == pol;
            break;
        case 2:                                     /* IRQ */
            tp = irq_target(s, p, n, index, &flag);
            ok = ((tp->irq >> flag) & 1) == pol;
            if (ok && pol) {
                tp->irq &= ~(1u << flag);
                pio_irqs(tp);
            }
            break;
        default:                                    /* JMPPIN */
            ok = ((pio_pins(s, p) >> (((sm->execctrl >> 24) + (index & 3))
                                      & 31)) & 1) == pol;
            break;
        }
        if (!ok) {
            on_pins = ((arg >> 5) & 3) != 2;        /* GPIO, PIN, JMPPIN */
            goto stall;
        }
        break;
    }
    case 2: {                                       /* IN */
        uint32_t thresh = ((sm->shiftctrl >> 20) & 31) ?: 32;

        count = (arg & 31) ?: 32;
        if ((sm->shiftctrl & SHIFT_AUTOPUSH) &&
            MIN(sm->isr_count + count, 32) >= thresh &&
            sm->rx_n >= rx_depth(sm)) {
            p->fdebug |= 1u << n;                   /* RXSTALL */
            goto stall;
        }
        switch ((arg >> 5) & 7) {
        case 0: v = sm_in_pins(s, p, sm); break;
        case 1: v = sm->x; break;
        case 2: v = sm->y; break;
        case 6: v = sm->isr; break;
        case 7: v = sm->osr; break;
        default: v = 0; break;
        }
        if (count < 32) {
            v &= (1u << count) - 1;
        }
        if (sm->shiftctrl & SHIFT_IN_RIGHT) {
            sm->isr = count == 32 ? v : (sm->isr >> count) | (v << (32 - count));
        } else {
            sm->isr = count == 32 ? v : (sm->isr << count) | v;
        }
        sm->isr_count = MIN(sm->isr_count + count, 32);
        if ((sm->shiftctrl & SHIFT_AUTOPUSH) && sm->isr_count >= thresh) {
            rx_push(sm, sm->isr);
            sm->isr = 0;
            sm->isr_count = 0;
        }
        break;
    }
    case 3: {                                       /* OUT */
        uint32_t thresh = ((sm->shiftctrl >> 25) & 31) ?: 32;
        int dest = (arg >> 5) & 7;

        count = (arg & 31) ?: 32;
        if ((sm->shiftctrl & SHIFT_AUTOPULL) && sm->osr_count >= thresh) {
            if (!sm->tx_n) {
                p->fdebug |= 1u << (24 + n);        /* TXSTALL */
                goto stall;
            }
            sm->osr = tx_pop(sm);
            sm->osr_count = 0;
        }
        if (sm->shiftctrl & SHIFT_OUT_RIGHT) {
            v = count == 32 ? sm->osr : sm->osr & ((1u << count) - 1);
            sm->osr = count == 32 ? 0 : sm->osr >> count;
        } else {
            v = count == 32 ? sm->osr : sm->osr >> (32 - count);
            sm->osr = count == 32 ? 0 : sm->osr << count;
        }
        sm->osr_count = MIN(sm->osr_count + count, 32);
        switch (dest) {
        case 0:                                     /* PINS */
            pins_write(&p->pad_out, sm->pinctrl & 31,
                       (sm->pinctrl >> 20) & 63, v);
            pins_changed = true;
            break;
        case 1: sm->x = v; break;
        case 2: sm->y = v; break;
        case 4:                                     /* PINDIRS */
            pins_write(&p->pad_oe, sm->pinctrl & 31,
                       (sm->pinctrl >> 20) & 63, v);
            pins_changed = true;
            break;
        case 5: sm->pc = v & 31; jumped = true; break;
        case 6: sm->isr = v; sm->isr_count = count; break;
        case 7:                                     /* EXEC */
            sm->exec = v;
            sm->has_exec = true;
            delay = 0;
            break;
        default: break;
        }
        if ((sm->shiftctrl & SHIFT_AUTOPULL) && sm->osr_count >= thresh &&
            sm->tx_n) {
            sm->osr = tx_pop(sm);                   /* refilled at once */
            sm->osr_count = 0;
        }
        break;
    }
    case 4:                                         /* PUSH, PULL, MOV RX */
        if (arg & 0x10) {                           /* RP2350: RX FIFO entry */
            int idx = arg & 0x08 ? arg & 3 : sm->y & 3;

            if (arg & 0x80) {
                sm->osr = sm->rxf[idx];
                sm->osr_count = 0;
            } else {
                sm->rxf[idx] = sm->isr;
            }
            break;
        }
        if (!(arg & 0x80)) {                        /* PUSH */
            uint32_t thresh = ((sm->shiftctrl >> 20) & 31) ?: 32;

            if ((arg & 0x40) && sm->isr_count < thresh) {
                break;                              /* IfFull, not full */
            }
            if (sm->rx_n >= rx_depth(sm)) {
                if (arg & 0x20) {
                    p->fdebug |= 1u << n;
                    goto stall;
                }
            } else {
                rx_push(sm, sm->isr);
            }
            sm->isr = 0;
            sm->isr_count = 0;
        } else {                                    /* PULL */
            uint32_t thresh = ((sm->shiftctrl >> 25) & 31) ?: 32;

            if ((arg & 0x40) && sm->osr_count < thresh) {
                break;                              /* IfEmpty, not empty */
            }
            if (!sm->tx_n) {
                if (arg & 0x20) {
                    p->fdebug |= 1u << (24 + n);
                    goto stall;
                }
                sm->osr = sm->x;                    /* no block: MOV OSR, X */
            } else {
                sm->osr = tx_pop(sm);
            }
            sm->osr_count = 0;
        }
        break;
    case 5: {                                       /* MOV */
        int dest = (arg >> 5) & 7;

        switch (arg & 7) {
        case 0: v = sm_in_pins(s, p, sm); break;
        case 1: v = sm->x; break;
        case 2: v = sm->y; break;
        case 5: {                                   /* STATUS */
            int nn = sm->execctrl & 31;

            switch ((sm->execctrl >> 5) & 3) {
            case 0: v = sm->tx_n < nn ? ~0u : 0; break;
            case 1: v = sm->rx_n < nn ? ~0u : 0; break;
            default:
                tp = irq_target(s, p, n, nn, &flag);
                v = (tp->irq >> flag) & 1 ? ~0u : 0;
                break;
            }
            break;
        }
        case 6: v = sm->isr; break;
        case 7: v = sm->osr; break;
        default: v = 0; break;
        }
        switch ((arg >> 3) & 3) {
        case 1: v = ~v; break;
        case 2: v = revbit32(v); break;
        }
        switch (dest) {
        case 0:
            pins_write(&p->pad_out, sm->pinctrl & 31,
                       (sm->pinctrl >> 20) & 63, v);
            pins_changed = true;
            break;
        case 1: sm->x = v; break;
        case 2: sm->y = v; break;
        case 3:
            pins_write(&p->pad_oe, sm->pinctrl & 31,
                       (sm->pinctrl >> 20) & 63, v);
            pins_changed = true;
            break;
        case 4:                                     /* EXEC */
            sm->exec = v;
            sm->has_exec = true;
            delay = 0;
            break;
        case 5: sm->pc = v & 31; jumped = true; break;
        case 6: sm->isr = v; sm->isr_count = 0; break;
        case 7: sm->osr = v; sm->osr_count = 0; break;
        }
        break;
    }
    case 6: {                                       /* IRQ */
        tp = irq_target(s, p, n, arg & 31, &flag);
        if (arg & 0x40) {                           /* clear */
            tp->irq &= ~(1u << flag);
        } else if (!sm->irq_waiting) {
            tp->irq |= 1u << flag;
            if (arg & 0x20) {
                sm->irq_waiting = true;
                pio_irqs(tp);
                goto stall;
            }
        } else if ((tp->irq >> flag) & 1) {
            goto stall;                             /* still set: wait on */
        } else {
            sm->irq_waiting = false;
        }
        pio_irqs(tp);
        break;
    }
    default: {                                      /* SET */
        int base = (sm->pinctrl >> 5) & 31, cnt = (sm->pinctrl >> 26) & 7;

        v = arg & 31;
        switch ((arg >> 5) & 7) {
        case 0: pins_write(&p->pad_out, base, cnt, v); pins_changed = true; break;
        case 1: sm->x = v; break;
        case 2: sm->y = v; break;
        case 4: pins_write(&p->pad_oe, base, cnt, v); pins_changed = true; break;
        }
        break;
    }
    }

    if (exec && !(op == 3 && ((arg >> 5) & 7) == 7) &&
        !(op == 5 && ((arg >> 5) & 7) == 4)) {
        sm->has_exec = false;                       /* (unless it EXECs on) */
    } else if (exec) {
        /* it put another instruction in its own place */
    }
    if (!jumped && !exec) {
        uint8_t top = (sm->execctrl >> 12) & 31;

        sm->pc = sm->pc == top ? (sm->execctrl >> 7) & 31 : (sm->pc + 1) & 31;
    }
    if (pins_changed) {
        pio_pins_changed(s, sm->next_ps);
    }
    sm->stalled = false;
    sm->next_ps += (1 + delay) * sm_period_ps(s, sm);
    pio_irqs(p);
    return true;

stall:
    if (pins_changed) {
        pio_pins_changed(s, sm->next_ps);
    }
    sm->stalled = true;
    sm->on_pins = on_pins;
    pio_irqs(p);
    return false;
}

/* --------------------------------------------------------------- running */

enum { WAKE_NONE, WAKE_PINS, WAKE_ALL };

/*
 * Run @p's machines up to @now_ps.  Those that stalled look again first:
 * every one (WAKE_ALL: the CPU, the DMA or the timer came), those that wait
 * on a pin (WAKE_PINS: a pin changed), or none (WAKE_NONE: only the time
 * went on); and when one goes on, the others, which it may have freed.  A
 * machine's time never goes back: one that ran ahead of the clock with a
 * burst of pin changes (rp2350_pins_drive_at()) looks again at its own time.
 */
static void pio_run_at(RP2350State *s, RP2Pio *p, int64_t now_ps, int wake)
{
    int64_t next = INT64_MAX;
    int budget = BUDGET;

    if (p->running) {
        return;
    }
    p->running = true;
    for (int i = 0; i < 4; i++) {
        RP2PioSM *sm = &p->sm[i];

        if (sm->stalled &&
            (wake == WAKE_ALL || (wake == WAKE_PINS && sm->on_pins))) {
            sm->next_ps = MAX(sm->next_ps, now_ps);
            sm->stalled = false;
        }
    }
    while (budget-- > 0) {
        int best = -1;
        int64_t at;
        bool went_on;

        for (int i = 0; i < 4; i++) {
            RP2PioSM *sm = &p->sm[i];

            if (((p->ctrl >> i) & 1 || sm->has_exec) && !sm->stalled &&
                sm->next_ps <= now_ps &&
                (best < 0 || sm->next_ps < p->sm[best].next_ps)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        at = p->sm[best].next_ps;
        went_on = sm_step(s, p, best);
        if (fifo_moved) {
            /* a DREQ: the DMA answers at once, as on the chip, so that a
             * FIFO it feeds (or drains) does not run dry (or fill) here */
            fifo_moved = false;
            rp2350_dma_kick(s);
            went_on = true;
        }
        if (went_on) {
            for (int i = 0; i < 4; i++) {           /* it may free another */
                RP2PioSM *sm = &p->sm[i];

                if (sm->stalled) {
                    sm->next_ps = MAX(sm->next_ps, at);
                    sm->stalled = false;
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        RP2PioSM *sm = &p->sm[i];

        if ((p->ctrl >> i) & 1 && !sm->stalled) {
            next = MIN(next, sm->next_ps);
        }
    }
    if (s->pio_burst) {
        /* the burst's next change runs it; its end, the timer */
    } else if (next != INT64_MAX) {
        timer_mod(p->timer, MAX(next, now_ps + 1) / 1000 + 1);
    } else {
        timer_del(p->timer);
    }
    p->running = false;
    if (fifo_moved) {
        fifo_moved = false;
        rp2350_dma_kick(s);
    }
}

static void pio_run(RP2350State *s, RP2Pio *p)
{
    pio_run_at(s, p, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 1000, WAKE_ALL);
}

static void pio_timer(void *opaque)
{
    RP2Pio *p = opaque;

    pio_run(p->regs.soc, p);
}

void rp2350_pio_kick(RP2350State *s)
{
    for (int i = 0; i < 3; i++) {
        if (s->pio[i].ctrl & 0xf) {
            pio_run(s, &s->pio[i]);
        }
    }
}

void rp2350_pio_burst(RP2350State *s, bool on)
{
    s->pio_burst = on;
    if (!on) {
        int64_t now_ps = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 1000;

        for (int i = 0; i < 3; i++) {
            if (s->pio[i].ctrl & 0xf) {
                pio_run_at(s, &s->pio[i], now_ps, WAKE_NONE);  /* timers */
            }
        }
    }
}

void rp2350_pio_run_to(RP2350State *s, int64_t when_ns, bool pins)
{
    int64_t at_ps = MAX(when_ns, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) * 1000;

    for (int i = 0; i < 3; i++) {
        if (s->pio[i].ctrl & 0xf) {
            pio_run_at(s, &s->pio[i], at_ps, pins ? WAKE_PINS : WAKE_NONE);
        }
    }
}

/* ------------------------------------------------------------ registers */

#define SM_REG(reg)     (((reg) - 0xc8) / 0x18)
#define SM_FIELD(reg)   (((reg) - 0xc8) % 0x18)

static void sm_restart(RP2PioSM *sm)
{
    sm->isr = 0;
    sm->isr_count = 0;
    sm->osr_count = 32;                             /* empty */
    sm->has_exec = false;
    sm->stalled = false;
    sm->irq_waiting = false;
}

static uint32_t pio_read(RP2Block *b, hwaddr reg)
{
    RP2Pio *p = container_of(b, RP2Pio, regs);
    RP2350State *s = b->soc;
    uint32_t v = 0;
    int n;

    pio_run(s, p);
    switch (reg) {
    case 0x000:
        return p->ctrl;
    case 0x004:                                     /* FSTAT */
        for (int i = 0; i < 4; i++) {
            RP2PioSM *sm = &p->sm[i];

            v |= (sm->rx_n >= rx_depth(sm)) << i;
            v |= (sm->rx_n == 0) << (8 + i);
            v |= (sm->tx_n >= tx_depth(sm)) << (16 + i);
            v |= (sm->tx_n == 0) << (24 + i);
        }
        return v;
    case 0x008:
        return p->fdebug;
    case 0x00c:                                     /* FLEVEL */
        for (int i = 0; i < 4; i++) {
            v |= (p->sm[i].tx_n & 0xf) << (i * 8);
            v |= (p->sm[i].rx_n & 0xf) << (i * 8 + 4);
        }
        return v;
    case 0x020 ... 0x02c:                           /* RXFn */
        n = (reg - 0x20) / 4;
        if (!p->sm[n].rx_n) {
            p->fdebug |= 1u << (8 + n);             /* RXUNDER */
            return 0;
        }
        v = rx_pop(&p->sm[n]);
        pio_irqs(p);
        pio_run(s, p);                              /* a stalled push goes on */
        return v;
    case 0x030:
        return p->irq;
    case 0x03c:
        return p->pad_out;
    case 0x040:
        return p->pad_oe;
    case 0x044:                                     /* DBG_CFGINFO */
        return 1u << 28 | 32 << 16 | 4 << 8 | 4;
    case 0x048 ... 0x0c4:
        return p->imem[(reg - 0x48) / 4];
    case 0x0c8 ... 0x124:
        n = SM_REG(reg);
        switch (SM_FIELD(reg)) {
        case 0x00: return p->sm[n].clkdiv;
        case 0x04: return p->sm[n].execctrl | (p->sm[n].stalled ? 1u << 31 : 0);
        case 0x08: return p->sm[n].shiftctrl;
        case 0x0c: return p->sm[n].pc;
        case 0x10: return p->imem[p->sm[n].pc];
        default: return p->sm[n].pinctrl;
        }
    case 0x128 ... 0x164:                           /* RXFn_PUTGETm */
        return p->sm[(reg - 0x128) / 16].rxf[((reg - 0x128) / 4) & 3];
    case 0x168:
        return p->gpiobase;
    case 0x16c:
        return pio_intr(p);
    case 0x170: return p->inte[0];
    case 0x174: return p->intf[0];
    case 0x178: return (pio_intr(p) | p->intf[0]) & p->inte[0];
    case 0x17c: return p->inte[1];
    case 0x180: return p->intf[1];
    case 0x184: return (pio_intr(p) | p->intf[1]) & p->inte[1];
    default:
        return 0;
    }
}

static void pio_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2Pio *p = container_of(b, RP2Pio, regs);
    RP2350State *s = b->soc;
    int64_t now_ps = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 1000;
    int n;

    if (s->dma.running && reg >= 0x010 && reg <= 0x01c) {
        /*
         * The DMA filling a TX FIFO: on the chip it fills it far faster than
         * a state machine empties it, so it is not run word by word here
         * (emulating its pins costs more than the time it takes); the block
         * looks at it next.
         */
        n = (reg - 0x10) / 4;
        if (!tx_push(&p->sm[n], value)) {
            p->fdebug |= 1u << (16 + n);            /* TXOVER */
        }
        if (p->sm[n].stalled) {                     /* it looks again */
            p->sm[n].next_ps = MAX(p->sm[n].next_ps, now_ps);
            p->sm[n].stalled = false;
        }
        pio_irqs(p);
        if (!p->running) {
            timer_mod_anticipate(p->timer, now_ps / 1000);
        }
        return;
    }
    pio_run(s, p);
    switch (reg) {
    case 0x000: {                                   /* CTRL */
        uint32_t starting = value & ~p->ctrl & 0xf;

        for (int i = 0; i < 4; i++) {
            if ((value >> (4 + i)) & 1) {
                sm_restart(&p->sm[i]);
            }
            if ((starting >> i) & 1 || (value >> (8 + i)) & 1) {
                p->sm[i].next_ps = now_ps;          /* CLKDIV_RESTART too */
            }
        }
        p->ctrl = value & 0xf;
        break;
    }
    case 0x008:                                     /* FDEBUG: W1C */
        p->fdebug &= ~value;
        break;
    case 0x010 ... 0x01c:                           /* TXFn */
        n = (reg - 0x10) / 4;
        if (!tx_push(&p->sm[n], value)) {
            p->fdebug |= 1u << (16 + n);            /* TXOVER */
        }
        break;
    case 0x030:                                     /* IRQ: W1C */
        p->irq &= ~value;
        break;
    case 0x034:                                     /* IRQ_FORCE */
        p->irq |= value;
        break;
    case 0x048 ... 0x0c4:
        p->imem[(reg - 0x48) / 4] = value;
        break;
    case 0x0c8 ... 0x124:
        n = SM_REG(reg);
        switch (SM_FIELD(reg)) {
        case 0x00: p->sm[n].clkdiv = value & 0xffffff00; break;
        case 0x04: p->sm[n].execctrl = value & 0x7fffffff; break;
        case 0x08: p->sm[n].shiftctrl = value; break;
        case 0x10:                                  /* INSTR: run it now */
            p->sm[n].exec = value;
            p->sm[n].has_exec = true;
            p->sm[n].stalled = false;
            /* in place of the one it stalled on: an IRQ WAIT is done with
             * (else the next IRQ WAIT would pass without setting its flag) */
            p->sm[n].irq_waiting = false;
            p->sm[n].next_ps = now_ps;
            break;
        case 0x14: p->sm[n].pinctrl = value; break;
        }
        break;
    case 0x128 ... 0x164:
        p->sm[(reg - 0x128) / 16].rxf[((reg - 0x128) / 4) & 3] = value;
        break;
    case 0x168:                                     /* GPIOBASE: 0 or 16 */
        p->gpiobase = value & 0x10;
        rp2350_pins_update(s, now_ps / 1000);
        break;
    case 0x170: p->inte[0] = value; break;
    case 0x174: p->intf[0] = value; break;
    case 0x17c: p->inte[1] = value; break;
    case 0x180: p->intf[1] = value; break;
    }
    pio_irqs(p);
    pio_run(s, p);
}

void rp2350_pio_realize(RP2350State *s)
{
    for (int i = 0; i < 3; i++) {
        RP2Pio *p = &s->pio[i];
        g_autofree char *name = g_strdup_printf("rp2350.pio%d", i);

        p->index = i;
        rp2_block_init(s, &p->regs, name, 0x50200000 + i * 0x100000,
                       pio_read, pio_write);
        p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pio_timer, p);
        p->irq_line[0] = rp2350_irq(s, IRQ_PIO0_0 + i * 2);
        p->irq_line[1] = rp2350_irq(s, IRQ_PIO0_0 + i * 2 + 1);
    }
}

void rp2350_pio_reset(RP2350State *s)
{
    for (int i = 0; i < 3; i++) {
        RP2Pio *p = &s->pio[i];

        timer_del(p->timer);
        memset(p->imem, 0, sizeof(p->imem));
        memset(p->sm, 0, sizeof(p->sm));
        for (int n = 0; n < 4; n++) {
            p->sm[n].clkdiv = 0x10000;              /* 1.0 */
            p->sm[n].execctrl = 0x1f << 12;         /* WRAP_TOP 31 */
            p->sm[n].shiftctrl = 0x000c0000;        /* both shifts right */
            p->sm[n].pinctrl = 5u << 26;            /* SET_COUNT 5 */
            p->sm[n].osr_count = 32;
        }
        p->ctrl = 0;
        p->irq = 0;
        p->pad_out = p->pad_oe = 0;
        p->gpiobase = 0;
        p->inte[0] = p->inte[1] = p->intf[0] = p->intf[1] = 0;
        p->fdebug = 0;
        p->running = false;
        for (int n = 0; n < 2; n++) {
            p->irq_level[n] = false;
            qemu_set_irq(p->irq_line[n], 0);
        }
    }
}
