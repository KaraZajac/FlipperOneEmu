/*
 * The RP2350B's 48 pins, and IO_BANK0
 *
 * A pin is driven by the function its GPIOn_CTRL selects (SIO, a PIO block;
 * the rest as they come), through CTRL's output and enable overrides and
 * its pad (OD and ISO let go of it).  Where nothing on the chip drives it,
 * the board may (rp2350_pin_drive()), and otherwise its pad's pull, or the
 * board's pull-up, decides; with none it reads low.  Every change of a pin
 * goes to its listeners with the virtual time it came at, and to IO_BANK0's
 * interrupts: INTRn's edges latched, its levels live, PROCn_INTE/INTF/INTS,
 * IO_IRQ_BANK0 to each core.  INOVER (what a peripheral sees) is kept, not
 * applied.
 *
 * RP2PinUart decodes 8N1 bytes from a pin's changes, for the firmware's log
 * UART, which a PIO program sends on GPIO41.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "rp2350.h"

#define IRQ_IO_BANK0        21
#define FUNC_SIO            5
#define FUNC_PIO0           6
#define CTRL(s, pin)        ((s)->io_bank0.regs[(pin) * 2 + 1])
#define PAD(s, pin)         ((s)->pads_bank0.regs[1 + (pin)])
#define PAD_OD              0x80
#define PAD_PUE             0x08
#define PAD_PDE             0x04
#define PAD_ISO             0x100

/* what @pin's function drives: its level and whether it drives at all */
static void pin_function(RP2350State *s, int pin, bool *out, bool *oe)
{
    uint32_t ctrl = CTRL(s, pin);
    int func = ctrl & 0x1f;
    int hi = pin >> 5, bit = pin & 31;

    *out = *oe = false;
    if (func == FUNC_SIO) {
        *out = (s->sio.out[hi] >> bit) & 1;
        *oe = (s->sio.oe[hi] >> bit) & 1;
    } else if (func >= FUNC_PIO0 && func <= FUNC_PIO0 + 2) {
        RP2Pio *p = &s->pio[func - FUNC_PIO0];
        int rel = pin - p->gpiobase;

        if (rel >= 0 && rel < 32) {
            *out = (p->pad_out >> rel) & 1;
            *oe = (p->pad_oe >> rel) & 1;
        }
    }
    switch ((ctrl >> 12) & 3) {                     /* OUTOVER */
    case 1: *out = !*out; break;
    case 2: *out = false; break;
    case 3: *out = true; break;
    }
    switch ((ctrl >> 14) & 3) {                     /* OEOVER */
    case 1: *oe = !*oe; break;
    case 2: *oe = false; break;
    case 3: *oe = true; break;
    }
    if (PAD(s, pin) & (PAD_OD | PAD_ISO)) {
        *oe = false;
    }
}

static bool pin_compute(RP2350State *s, int pin)
{
    bool out, oe;
    uint32_t pad = PAD(s, pin);

    pin_function(s, pin, &out, &oe);
    if (oe) {
        return out;
    }
    if ((s->ext_drive >> pin) & 1) {
        return (s->ext_level >> pin) & 1;
    }
    if ((s->ext_pullup >> pin) & 1) {
        return true;
    }
    if (pad & PAD_PUE) {
        return true;
    }
    return false;
}

/* INTRn: 8 pins' LEVEL_LOW, LEVEL_HIGH (as they are), EDGE_LOW, EDGE_HIGH */
static uint32_t gpio_raw(RP2350State *s, int reg)
{
    uint32_t raw = s->gpio_edges[reg];

    for (int i = 0; i < 8; i++) {
        int pin = reg * 8 + i;

        if (pin < RP2_PINS) {
            raw |= ((s->pin_level >> pin) & 1 ? 2u : 1u) << (i * 4);
        }
    }
    return raw;
}

/* IO_BANK0's interrupt status for a core (0, 1) or dormant wake (2) */
static uint32_t gpio_ints(RP2350State *s, int who, int reg)
{
    uint32_t base = (0x248 + who * 0x48) / 4;       /* PROCn_INTE0 */

    return (gpio_raw(s, reg) | s->io_bank0.regs[base + 6 + reg]) &
           s->io_bank0.regs[base + reg];
}

static void gpio_irqs(RP2350State *s)
{
    for (int core = 0; core < 2; core++) {
        bool any = false;

        for (int reg = 0; reg < 6; reg++) {
            any |= gpio_ints(s, core, reg) != 0;
        }
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu[core]), IRQ_IO_BANK0),
                     any);
    }
}

void rp2350_pins_update(RP2350State *s, int64_t when_ns)
{
    uint64_t now = 0, changed;

    for (int pin = 0; pin < RP2_PINS; pin++) {
        now |= (uint64_t)pin_compute(s, pin) << pin;
    }
    changed = now ^ s->pin_level;
    s->pin_level = now;
    if (!changed) {
        return;
    }
    for (int pin = 0; pin < RP2_PINS; pin++) {
        bool level;

        if (!((changed >> pin) & 1)) {
            continue;
        }
        level = (now >> pin) & 1;
        /* EDGE_HIGH (bit 3) or EDGE_LOW (bit 2) of the pin's nibble */
        s->gpio_edges[pin / 8] |= (level ? 8u : 4u) << ((pin % 8) * 4);
        for (int i = 0; i < 2; i++) {
            if (s->pin_listener[pin][i].fn) {
                s->pin_listener[pin][i].fn(s->pin_listener[pin][i].opaque, pin,
                                           level, when_ns);
            }
        }
    }
    gpio_irqs(s);
    rp2350_pio_kick(s);
}

void rp2350_pin_drive(RP2350State *s, int pin, int level)
{
    if (level < 0) {
        s->ext_drive &= ~(1ull << pin);
    } else {
        s->ext_drive |= 1ull << pin;
        s->ext_level = deposit64(s->ext_level, pin, 1, level);
    }
    rp2350_pins_update(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

int rp2350_pin_own(RP2350State *s, int pin)
{
    bool out, oe;

    pin_function(s, pin, &out, &oe);
    if (oe) {
        return out;
    }
    return ((s->ext_pullup >> pin) & 1) || (PAD(s, pin) & PAD_PUE);
}

void rp2350_pin_listen(RP2350State *s, int pin, RP2PinFn *fn, void *opaque)
{
    for (int i = 0; i < 2; i++) {
        if (!s->pin_listener[pin][i].fn) {
            s->pin_listener[pin][i].fn = fn;
            s->pin_listener[pin][i].opaque = opaque;
            return;
        }
    }
}

/* ------------------------------------------------------------- IO_BANK0 */

static uint32_t io_bank0_read(RP2Block *b, hwaddr reg)
{
    RP2350State *s = b->soc;
    int pin, who;

    if (reg < RP2_PINS * 8) {
        pin = reg / 8;
        if (reg & 4) {
            return b->regs[reg / 4];                /* GPIOn_CTRL */
        } else {                                    /* GPIOn_STATUS */
            bool out, oe;

            pin_function(s, pin, &out, &oe);
            return (out ? 0x200 : 0) | (oe ? 0x2000 : 0) |
                   ((s->pin_level >> pin) & 1 ? 0x20000 : 0);
        }
    }
    switch (reg) {
    case 0x200 ... 0x22c: {                         /* IRQSUMMARY_* */
        uint32_t sum = 0;
        int half = (reg >> 2) & 1;                  /* pins 0-31, 32-47 */

        who = (reg - 0x200) / 16;                   /* proc0, proc1, dormant */
        if (reg & 8) {
            return 0;                               /* NonSecure: none */
        }
        for (int r = 0; r < 6; r++) {
            uint32_t ints = gpio_ints(s, who > 2 ? 2 : who, r);

            for (int i = 0; i < 8; i++) {
                pin = r * 8 + i;
                if (pin / 32 == half && ((ints >> (i * 4)) & 0xf)) {
                    sum |= 1u << (pin & 31);
                }
            }
        }
        return sum;
    }
    case 0x230 ... 0x244:                           /* INTRn */
        return gpio_raw(s, (reg - 0x230) / 4);
    case 0x278 ... 0x28c:                           /* PROC0_INTSn */
        return gpio_ints(s, 0, (reg - 0x278) / 4);
    case 0x2c0 ... 0x2d4:                           /* PROC1_INTSn */
        return gpio_ints(s, 1, (reg - 0x2c0) / 4);
    case 0x308 ... 0x31c:                           /* DORMANT_WAKE_INTSn */
        return gpio_ints(s, 2, (reg - 0x308) / 4);
    default:
        return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
    }
}

static void io_bank0_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2350State *s = b->soc;

    if (reg < RP2_PINS * 8) {
        if (reg & 4) {                              /* GPIOn_CTRL */
            b->regs[reg / 4] = value & 0x3003f01f;
            rp2350_pins_update(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        return;
    }
    switch (reg) {
    case 0x230 ... 0x244:                           /* INTRn: W1C edges */
        s->gpio_edges[(reg - 0x230) / 4] &= ~(value & 0xcccccccc);
        gpio_irqs(s);
        break;
    case 0x248 ... 0x274:                           /* PROC0_INTE, INTF */
    case 0x290 ... 0x2bc:                           /* PROC1_INTE, INTF */
    case 0x2d8 ... 0x304:                           /* DORMANT_WAKE_* */
        b->regs[reg / 4] = value;
        gpio_irqs(s);
        break;
    }
}

/* ------------------------------------------------------ a UART on a pin */

static void pin_uart_byte(RP2PinUart *u)
{
    uint8_t byte = (u->bits >> 1) & 0xff;

    if ((u->bits & 1) == 0 && (u->bits >> 9) & 1 && u->chr) {
        qemu_chr_fe_write(u->chr, &byte, 1);        /* a start and a stop */
    }
    u->busy = false;
}

/* the bits from the last change up to @when_ns had the level before it */
static void pin_uart_fill(RP2PinUart *u, int64_t when_ns)
{
    int64_t period = NANOSECONDS_PER_SECOND / u->baud;

    while (u->busy && u->bit < 10) {
        int64_t at = u->start_ns + period * u->bit + period / 2;

        if (at > when_ns) {
            break;
        }
        u->bits |= (uint16_t)u->level << u->bit;
        u->bit++;
    }
    if (u->busy && u->bit == 10) {
        pin_uart_byte(u);
    }
}

static void pin_uart_done(void *opaque)
{
    RP2PinUart *u = opaque;

    pin_uart_fill(u, INT64_MAX);
}

static void pin_uart_change(void *opaque, int pin, bool level, int64_t when_ns)
{
    RP2PinUart *u = opaque;

    pin_uart_fill(u, when_ns);
    if (!u->busy && u->level && !level) {           /* a start bit */
        u->busy = true;
        u->start_ns = when_ns;
        u->bit = 0;
        u->bits = 0;
        timer_mod(u->done, when_ns + 10 * NANOSECONDS_PER_SECOND / u->baud +
                           NANOSECONDS_PER_SECOND / u->baud / 2);
    }
    u->level = level;
    u->last_ns = when_ns;
}

void rp2350_pin_uart(RP2350State *s, int pin, uint32_t baud,
                     CharFrontend *chr)
{
    RP2PinUart *u = &s->log_uart;

    u->soc = s;
    u->pin = pin;
    u->baud = baud;
    u->chr = chr;
    u->level = true;
    u->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, pin_uart_done, u);
    rp2350_pin_listen(s, pin, pin_uart_change, u);
}

/* ------------------------------------------------------------------------ */

void rp2350_gpio_realize(RP2350State *s)
{
    rp2_block_init(s, &s->io_bank0, "rp2350.io_bank0", 0x40028000,
                   io_bank0_read, io_bank0_write);
}

void rp2350_gpio_reset(RP2350State *s)
{
    memset(s->io_bank0.regs, 0, sizeof(s->io_bank0.regs));
    for (int pin = 0; pin < RP2_PINS; pin++) {
        CTRL(s, pin) = 0x1f;                        /* FUNCSEL: NULL */
    }
    memset(s->gpio_edges, 0, sizeof(s->gpio_edges));
    s->ext_drive = s->ext_level = 0;
    s->pin_level = 0;
    s->log_uart.busy = false;
    s->log_uart.level = true;
}
