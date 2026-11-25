/*
 * The RK3576's display link to the MCU, for the host link: frames as Flipper
 * OS sends them (docs/HOST_LINK.md)
 *
 * Flipper OS's kernel (its flipper-one-display driver) sends a frame thus:
 * "active" asserted (its GPIO0_D1, low; the MCU's GPIO25), the whole frame
 * in one SPI write (mode 3: the clock idles high, the data changes on its
 * falling edge and is taken on its rising one; 33 MHz, MSB first) on the
 * MCU's GPIO26 (SCK) and GPIO28 (MOSI), then "active" let go.  The MCU
 * firmware samples it with a PIO program (pio_get_frame.c) into a DMA
 * buffer, and takes the frame when "active" goes up again.
 *
 * The lines change as they would, edge by edge, but in bursts: a timer
 * callback clocks out up to BURST_BITS bits at 33 MHz from the virtual
 * clock's present on, ahead of it, and the next burst goes on from where
 * that one ended, or from the present if the clock went past it.  A burst's
 * bits go to the MCU as a wave (rp2350_pins_wave()): the PIO's state
 * machines see SCK and MOSI change at each edge's time, and one that waits
 * for an edge goes on at it, without a call for each of a frame's 600,000
 * changes; where the MCU has more than its PIO on those pins, edge by edge
 * (rp2350_pins_drive_at() runs the PIO up to each change before making it).
 * A frame (258 x 144 bytes) takes about 9 ms of the device's time.  The
 * lines stay driven, as they idle, until the program goes.
 *
 * A burst is the MCU's PIO run through a thousand clocks, which costs the
 * host more than their 30 us, with the BQL held.  So the next one waits as
 * long as that one took: the CPUs get the BQL meanwhile (a register, an
 * interrupt taken), which they hardly did with the bursts back to back.  On
 * the wire it is a clock that pauses between bursts, as a DMA-fed one may.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "flipper_one.h"
#include "rp2350.h"

#define PIN_ACTIVE      25
#define PIN_SCK         26
#define PIN_MOSI        28
#define HALF_NS         15                  /* 33 MHz */
#define HALF_PS         (HALF_NS * 1000ll)
#define BIT_PS          (2 * HALF_PS)
#define SETUP_NS        2000                /* "active" to the first clock */
#define HOLD_NS         1000                /* the last clock to "active" up */
#define GAP_NS          1000000             /* after a frame, at least */
#define BURST_BITS      1024

#define LINE(pin)       (1ull << (pin))
#define LINES           (LINE(PIN_ACTIVE) | LINE(PIN_SCK) | LINE(PIN_MOSI))

enum { SPI_IDLE, SPI_BITS, SPI_GAP };

/* a burst's bits as a wave: at each bit's start SCK goes low and the bit
 * onto MOSI, half way SCK goes up */
typedef struct SpiWave {
    RP2Wave w;
    const uint8_t *data;
    size_t bit0;                        /* the frame's bit at the start */
    int64_t n;                          /* bits in it */
} SpiWave;

static int wave_bit(const SpiWave *sw, int64_t i)
{
    size_t b = sw->bit0 + i;

    return (sw->data[b / 8] >> (7 - b % 8)) & 1;
}

static uint64_t wave_levels(const RP2Wave *w, int64_t t_ps)
{
    const SpiWave *sw = container_of(w, SpiWave, w);
    int64_t rel = t_ps - w->from_ps, i = rel / BIT_PS;

    if (i >= sw->n) {                               /* after its last bit */
        return LINE(PIN_SCK) | (wave_bit(sw, sw->n - 1) ? LINE(PIN_MOSI) : 0);
    }
    return (rel % BIT_PS >= HALF_PS ? LINE(PIN_SCK) : 0) |
           (wave_bit(sw, i) ? LINE(PIN_MOSI) : 0);
}

static int64_t wave_until(const RP2Wave *w, int pin, bool level, int64_t t_ps)
{
    const SpiWave *sw = container_of(w, SpiWave, w);
    int64_t rel = t_ps - w->from_ps, i = rel / BIT_PS;
    bool high = rel - i * BIT_PS >= HALF_PS;

    if (i >= sw->n) {                               /* as its last bit left */
        bool now = pin == PIN_SCK || wave_bit(sw, sw->n - 1);

        return now == level ? t_ps : INT64_MAX;
    }
    if (pin == PIN_SCK) {
        if (high == level) {
            return t_ps;
        }
        if (level) {                                /* this bit's rising edge */
            return w->from_ps + i * BIT_PS + HALF_PS;
        }
        return i + 1 < sw->n ? w->from_ps + (i + 1) * BIT_PS : INT64_MAX;
    }
    for (int64_t j = i; j < sw->n; j++) {           /* MOSI: at a bit's start */
        if (wave_bit(sw, j) == level) {
            return j == i ? t_ps : w->from_ps + j * BIT_PS;
        }
    }
    return INT64_MAX;
}

/* @n more bits of the frame as a wave; false: the MCU wants them edge by
 * edge */
static bool spi_wave(FoHostSpi *f, int64_t n)
{
    SpiWave sw;
    int prev = 1;                                   /* MOSI idles high */

    /* (a wave starts at the virtual clock or later) */
    f->t = MAX(f->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    sw = (SpiWave) {
        .w = {
            .mask = LINE(PIN_SCK) | LINE(PIN_MOSI),
            .from_ps = f->t * 1000,
            .to_ps = f->t * 1000 + n * BIT_PS,
            .rose = LINE(PIN_SCK),
            .fell = LINE(PIN_SCK),
            .levels = wave_levels,
            .until = wave_until,
        },
        .data = f->cur->data,
        .bit0 = f->bit,
        .n = n,
    };
    if (f->bit) {
        prev = (f->cur->data[(f->bit - 1) / 8] >> (7 - (f->bit - 1) % 8)) & 1;
    }
    for (int64_t i = 0; i < n; i++) {
        int b = wave_bit(&sw, i);

        if (b != prev) {
            if (b) {
                sw.w.rose |= LINE(PIN_MOSI);
            } else {
                sw.w.fell |= LINE(PIN_MOSI);
            }
        }
        prev = b;
    }
    if (!rp2350_pins_wave(f->soc, &sw.w)) {
        return false;
    }
    f->t += n * 2 * HALF_NS;
    f->bit += n;
    return true;
}

static void spi_let_go(FoHostSpi *f)
{
    if (f->driving) {
        f->driving = false;
        rp2350_pin_drive(f->soc, PIN_ACTIVE, -1);
        rp2350_pin_drive(f->soc, PIN_SCK, -1);
        rp2350_pin_drive(f->soc, PIN_MOSI, -1);
    }
}

/* up to BURST_BITS of the frame; true: it is all out */
static bool spi_bits(FoHostSpi *f)
{
    RP2350State *s = f->soc;
    size_t bits = f->cur->len * 8;
    int budget = BURST_BITS;

    if (f->phase != SPI_BITS) {
        /* the lines as they idle, then "active" */
        rp2350_pins_drive_at(s, LINES, LINES, f->t);
        f->driving = true;
        rp2350_pins_drive_at(s, LINE(PIN_ACTIVE), 0, f->t + 1);
        /* the clock from SETUP_NS after "active" went down: at the virtual
         * clock's present, if that had passed f->t + 1 (a change behind the
         * clock is made at its present, and the PIO's machines saw it then) */
        f->t = MAX(f->t + 1, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) + SETUP_NS;
        f->phase = SPI_BITS;
    }
    if (spi_wave(f, MIN(bits - f->bit, BURST_BITS))) {
        budget = 0;
    }
    while (f->bit < bits && budget-- > 0) {
        int b = (f->cur->data[f->bit / 8] >> (7 - f->bit % 8)) & 1;

        rp2350_pins_drive_at(s, LINE(PIN_SCK) | LINE(PIN_MOSI),
                             b ? LINE(PIN_MOSI) : 0, f->t);
        rp2350_pins_drive_at(s, LINE(PIN_SCK), LINE(PIN_SCK), f->t + HALF_NS);
        f->t += 2 * HALF_NS;
        f->bit++;
    }
    if (f->bit < bits) {
        return false;
    }
    /* all of it: "active" let go, the data line as it idles */
    f->t += HOLD_NS;
    rp2350_pins_drive_at(s, LINE(PIN_ACTIVE) | LINE(PIN_MOSI), LINES, f->t);
    return true;
}

static void spi_burst(void *opaque)
{
    FoHostSpi *f = opaque;
    int64_t took;
    bool out;

    f->t = MAX(f->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (f->phase != SPI_BITS) {
        f->cur = g_queue_pop_head(f->frames);
        if (!f->cur) {
            f->phase = SPI_IDLE;
            if (f->closing) {
                spi_let_go(f);
            }
            return;
        }
        f->bit = 0;
    }
    took = get_clock();
    rp2350_pio_burst(f->soc, true);
    out = spi_bits(f);
    rp2350_pio_burst(f->soc, false);
    took = get_clock() - took;
    if (!out) {
        /* the next burst, after as long again as this one took the host:
         * meanwhile the rest of the machine has its turn at the BQL (its
         * CPUs at their devices, an interrupt taken) */
        timer_mod(f->timer, MAX(f->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                      took));
        return;
    }
    g_byte_array_unref(f->cur);
    f->cur = NULL;
    f->phase = SPI_GAP;
    f->t = MAX(f->t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) + GAP_NS;
    timer_mod(f->timer, f->t);
    f->sent(f->opaque);
}

void fo_host_spi_send(FoHostSpi *f, GByteArray *frame)
{
    g_queue_push_tail(f->frames, frame);
    f->closing = false;
    if (f->phase == SPI_IDLE) {
        f->phase = SPI_GAP;                         /* (starting) */
        timer_mod(f->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

int fo_host_spi_waiting(FoHostSpi *f)
{
    return g_queue_get_length(f->frames);
}

void fo_host_spi_close(FoHostSpi *f)
{
    GByteArray *frame;

    while ((frame = g_queue_pop_head(f->frames))) {
        g_byte_array_unref(frame);
    }
    f->closing = true;
    if (f->phase == SPI_IDLE) {
        spi_let_go(f);
    }
}

void fo_host_spi_init(FoHostSpi *f, struct RP2350State *soc,
                      void (*sent)(void *opaque), void *opaque)
{
    f->soc = soc;
    f->frames = g_queue_new();
    f->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, spi_burst, f);
    f->sent = sent;
    f->opaque = opaque;
}
