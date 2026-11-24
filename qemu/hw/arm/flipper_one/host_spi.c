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
 * clock's present on, ahead of it (rp2350_pins_drive_at() runs the PIO up
 * to each change before making it), and the next burst goes on from where
 * that one ended, or from the present if the clock went past it.  A frame
 * (258 x 144 bytes) takes about 9 ms of the device's time, and of the host's
 * what its 600,000 pin changes cost.  The lines stay driven, as they idle,
 * until the program goes.
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
#define SETUP_NS        2000                /* "active" to the first clock */
#define HOLD_NS         1000                /* the last clock to "active" up */
#define GAP_NS          1000000             /* after a frame, at least */
#define BURST_BITS      1024

#define LINE(pin)       (1ull << (pin))
#define LINES           (LINE(PIN_ACTIVE) | LINE(PIN_SCK) | LINE(PIN_MOSI))

enum { SPI_IDLE, SPI_BITS, SPI_GAP };

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
        f->t += SETUP_NS;
        f->phase = SPI_BITS;
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
    rp2350_pio_burst(f->soc, true);
    out = spi_bits(f);
    rp2350_pio_burst(f->soc, false);
    if (!out) {
        timer_mod(f->timer, f->t);                  /* the next burst */
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
