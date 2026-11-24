/*
 * The Flipper One's 17 WS2812 LEDs, on three chains
 *
 * The firmware drives each chain with the Pico SDK's ws2812 PIO program at
 * 800 kHz (furi_bsp_led.c): GPIO29 the network lights (4), GPIO30 Power,
 * the battery's outline (2) and its watts (4), GPIO31 USB charging, its
 * watts (4) and the battery's centre: the panel's LEDs in that order. Each
 * chain's supply is an output of the control bus's expander (pins 15, 14,
 * 13 for chains 1, 2, 3), which the firmware turns on before it writes and
 * off when the chain is all dark.
 *
 * A bit is a high pulse, long for a 1 (0.75 µs at the program's timing),
 * short for a 0 (0.375 µs): longer than 0.56 µs is a 1. Each LED takes 24
 * bits, green, red, blue, most significant first, and passes the rest on;
 * the line low for more than 50 µs starts the chain again. An LED shows its
 * colour once it has its 24 bits; one without power is dark, and starts
 * dark when its power comes back.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "flipper_one.h"
#include "rp2350.h"

#define BIT_ONE_NS      560
#define RESET_NS        50000

static const struct {
    int pin, first, count;
} chains[FO_LED_CHAINS] = {
    { 29, 0, 4 }, { 30, 4, 7 }, { 31, 11, 6 },
};

static void leds_dark(FoLeds *l, int c)
{
    for (int i = 0; i < chains[c].count; i++) {
        fo_panel_led(l->panel, chains[c].first + i, 0);
    }
}

static void leds_pin(void *opaque, int pin, bool level, int64_t when)
{
    FoLeds *l = opaque;
    int c = pin - chains[0].pin;
    FoLedChain *ch = &l->chain[c];

    if (!ch->powered) {
        return;
    }
    if (level) {
        if (when - ch->fall_ns > RESET_NS) {
            ch->bits = 0;                       /* the chain starts again */
            ch->shift = 0;
        }
        ch->rise_ns = when;
        return;
    }
    ch->fall_ns = when;
    ch->shift = ch->shift << 1 | (when - ch->rise_ns > BIT_ONE_NS);
    if (++ch->bits % 24 == 0) {
        int k = ch->bits / 24 - 1;
        uint32_t grb = ch->shift & 0xffffff;

        if (k < chains[c].count) {
            fo_panel_led(l->panel, chains[c].first + k,
                         (grb & 0xff00) << 8 | (grb & 0xff0000) >> 8 |
                         (grb & 0xff));
        }
        ch->shift = 0;
    }
}

void fo_leds_power(FoLeds *l, int c, bool on)
{
    FoLedChain *ch = &l->chain[c];

    if (on == ch->powered) {
        return;
    }
    ch->powered = on;
    ch->bits = 0;
    ch->shift = 0;
    leds_dark(l, c);
}

void fo_leds_init(FoLeds *l, struct RP2350State *soc,
                  FlipperOnePanelState *panel)
{
    l->panel = panel;
    for (int c = 0; c < FO_LED_CHAINS; c++) {
        rp2350_pin_listen(soc, chains[c].pin, leds_pin, l);
    }
}
