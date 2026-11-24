/*
 * The Flipper One's display controller: a Jadard JD9853 on QSPI
 *
 * Wiring (the MCU firmware's display_jd9853_qspi.c): IO0 GPIO19, IO1 GPIO16,
 * IO2 GPIO15, IO3 GPIO14, SCK GPIO18 (HSTX), CS GPIO17 (HSTX, or SIO while
 * pixels go out), RESET GPIO12, TE GPIO5.  Bytes come over HSTX's cycles:
 * a transaction is CS low; its first byte the instruction (02h: write, on
 * IO0; 32h: write with the data on four lines), three address bytes (the
 * middle one the command), then the data, on IO0 a bit a cycle or on IO3-0
 * a nibble.  A command acts when CS goes up.
 *
 * The glass is monochrome behind a colour controller: each byte of RGB666
 * memory is one grey pixel, 86 columns of three a row.  The firmware writes
 * the whole window (258 x 144 bytes, MADCTL and the window set so that it
 * shows upright), and the glass shows 256 of each row, from the second on:
 * the firmware's GUI kept its first column empty for that ("the pixel shift
 * bug", from March 2026), and since September it puts a frame from the
 * RK3576 one pixel on, so that the frame's leftmost 256 are what shows, as
 * the kernel's binding (flipper,one-display.yaml) has it.  RAMWR writes from
 * the window's start, RAMWRC goes on.
 *
 * TE (tearing effect, GPIO5) pulses at the panel's 80 Hz while TEON is set
 * and the controller is out of reset: the firmware starts each frame there,
 * and takes a TE as the sign that the panel is connected.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "flipper_one.h"
#include "rp2350.h"

#define PIN_TE          5
#define PIN_RESET       12
#define PIN_CS          17
#define HSTX_CS         (PIN_CS - 12)
#define HSTX_IO0        (19 - 12)
#define HSTX_IO1        (16 - 12)
#define HSTX_IO2        (15 - 12)
#define HSTX_IO3        (14 - 12)
#define ROW             258
#define GLASS_X0        1                       /* its first pixel shown */
#define TE_PERIOD_NS    12500000                /* 80 Hz */
#define TE_HIGH_NS      2000000

enum { PH_INSTR, PH_ADDR, PH_DATA };

static void jd_reset(FoJd9853 *d)
{
    d->phase = PH_INSTR;
    d->bits = 0;
    d->nparams = 0;
    d->te_on = false;
    d->sleep = true;
    d->disp_on = false;
    d->pos = 0;
    fo_panel_lcd_on(d->panel, false);
}

static void jd_lcd(FoJd9853 *d)
{
    fo_panel_lcd_on(d->panel, d->disp_on && !d->sleep && !d->in_reset);
}

/* a command, with its parameters, when its transaction ends */
static void jd_command(FoJd9853 *d)
{
    switch (d->cmd) {
    case 0x01:                                      /* SWRESET */
        jd_reset(d);
        break;
    case 0x10:                                      /* SLPIN */
        d->sleep = true;
        break;
    case 0x11:                                      /* SLPOUT */
        d->sleep = false;
        break;
    case 0x28:                                      /* DISPOFF */
        d->disp_on = false;
        break;
    case 0x29:                                      /* DISPON */
        d->disp_on = true;
        break;
    case 0x34:                                      /* TEOFF */
        d->te_on = false;
        break;
    case 0x35:                                      /* TEON */
        d->te_on = true;
        break;
    }
    jd_lcd(d);
}

static void jd_byte(FoJd9853 *d, uint8_t b)
{
    switch (d->phase) {
    case PH_INSTR:
        d->instr = b;
        d->naddr = 0;
        d->phase = PH_ADDR;
        break;
    case PH_ADDR:
        if (d->naddr++ == 1) {
            d->cmd = b;
        }
        if (d->naddr == 3) {
            d->phase = PH_DATA;
            d->nparams = 0;
            if (d->cmd == 0x2c) {                   /* RAMWR: from the start */
                d->pos = 0;
            }
        }
        break;
    default:
        if (d->cmd == 0x2c || d->cmd == 0x3c) {     /* RAMWR, RAMWRC */
            uint32_t y = d->pos / ROW, x = d->pos % ROW;

            if (y < FO_LCD_H && x >= GLASS_X0 && x - GLASS_X0 < FO_LCD_W) {
                d->panel->fb[y][x - GLASS_X0] = b;
            }
            d->pos++;
        } else if (d->nparams < sizeof(d->params)) {
            d->params[d->nparams++] = b;
        }
        break;
    }
}

static void jd_select(FoJd9853 *d, bool selected)
{
    if (selected == d->selected) {
        return;
    }
    d->selected = selected;
    if (selected) {
        d->phase = PH_INSTR;
        d->bits = 0;
        d->cur = 0;
    } else if (d->phase == PH_DATA || d->naddr == 3) {
        jd_command(d);
    }
}

/* HSTX's cycles: CS from HSTX when it has the pin, else the pin's level */
static void jd_cycles(void *opaque, const uint8_t *cycles, int n, uint8_t mask)
{
    FoJd9853 *d = opaque;
    for (int i = 0; i < n; i++) {
        uint8_t c = cycles[i];
        bool quad;

        if (mask & (1u << HSTX_CS)) {
            jd_select(d, !((c >> HSTX_CS) & 1));
        }
        if (!d->selected || d->in_reset) {
            continue;
        }
        quad = d->phase == PH_DATA && d->instr == 0x32;
        if (quad) {
            uint8_t nib = ((c >> HSTX_IO3) & 1) << 3 | ((c >> HSTX_IO2) & 1) << 2 |
                          ((c >> HSTX_IO1) & 1) << 1 | ((c >> HSTX_IO0) & 1);

            d->cur = d->cur << 4 | nib;
            d->bits += 4;
        } else {
            d->cur = d->cur << 1 | ((c >> HSTX_IO0) & 1);
            d->bits += 1;
        }
        if (d->bits >= 8) {
            jd_byte(d, d->cur);
            d->bits = 0;
            d->cur = 0;
        }
    }
}

static void jd_pin(void *opaque, int pin, bool level, int64_t when_ns)
{
    FoJd9853 *d = opaque;

    if (pin == PIN_RESET) {
        d->in_reset = !level;
        if (d->in_reset) {
            jd_reset(d);
        }
        jd_lcd(d);
    } else if (pin == PIN_CS &&
               (d->soc->io_bank0.regs[PIN_CS * 2 + 1] & 0x1f) != 0) {
        jd_select(d, !level);                       /* SIO has CS */
    }
}

static void jd_te(void *opaque)
{
    FoJd9853 *d = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (d->te_level) {
        d->te_level = false;
        rp2350_pin_drive(d->soc, PIN_TE, 0);
        timer_mod(d->te, now + TE_PERIOD_NS - TE_HIGH_NS);
    } else {
        if (d->te_on && !d->in_reset) {
            d->te_level = true;
            rp2350_pin_drive(d->soc, PIN_TE, 1);
        }
        timer_mod(d->te, now + (d->te_level ? TE_HIGH_NS
                                            : TE_PERIOD_NS - TE_HIGH_NS));
    }
}

void fo_jd9853_unpowered(FoJd9853 *d)
{
    d->in_reset = true;
    jd_reset(d);
    jd_lcd(d);
}

void fo_jd9853_init(FoJd9853 *d, RP2350State *soc, FlipperOnePanelState *panel)
{
    d->soc = soc;
    d->panel = panel;
    d->in_reset = true;
    jd_reset(d);
    rp2350_hstx_attach(soc, jd_cycles, d);
    rp2350_pin_listen(soc, PIN_RESET, jd_pin, d);
    rp2350_pin_listen(soc, PIN_CS, jd_pin, d);
    d->te = timer_new_ns(QEMU_CLOCK_VIRTUAL, jd_te, d);
    timer_mod(d->te, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TE_PERIOD_NS);
}
