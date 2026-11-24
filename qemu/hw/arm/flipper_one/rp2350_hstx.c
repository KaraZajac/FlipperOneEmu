/*
 * The RP2350's HSTX: words from its FIFO shifted out on GPIO12-19
 *
 * A word written to the FIFO goes out at once, N_SHIFTS cycles of it: in
 * each, BITn's SEL_P bit of the shift register (inverted with INV) is what
 * GPIO12+n shows, then the register rotates right by SHIFT.  The cycles go
 * to what the board attached (rp2350_hstx_attach()), with the pins whose
 * function is HSTX; the FIFO is then empty again.  CLK pins, the expander
 * and TMDS are not modelled.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "rp2350.h"

#define CSR_EN          0x1
#define FIFO_EMPTY      0x200
#define FIFO_WOF        0x400

/* which of GPIO12-19 have HSTX (function 0) */
static uint8_t hstx_pins(RP2350State *s)
{
    uint8_t mask = 0;

    for (int i = 0; i < 8; i++) {
        if ((s->io_bank0.regs[(12 + i) * 2 + 1] & 0x1f) == 0) {
            mask |= 1u << i;
        }
    }
    return mask;
}

static void hstx_shift(RP2350State *s, uint32_t word)
{
    uint32_t csr = s->hstx_ctrl.regs[0];
    int n = (csr >> 16) & 0x1f ?: 32;
    int shift = (csr >> 8) & 0x1f;
    uint8_t cycles[32];

    if (!(csr & CSR_EN) || !s->hstx_fn) {
        return;
    }
    for (int k = 0; k < n; k++) {
        uint8_t out = 0;

        for (int b = 0; b < 8; b++) {
            uint32_t bit = s->hstx_ctrl.regs[1 + b];
            int level = (word >> (bit & 0x1f)) & 1;

            out |= (level ^ ((bit >> 16) & 1)) << b;
        }
        cycles[k] = out;
        word = ror32(word, shift);
    }
    s->hstx_fn(s->hstx_opaque, cycles, n, hstx_pins(s));
}

static uint32_t hstx_fifo_read(RP2Block *b, hwaddr reg)
{
    return reg == 0 ? FIFO_EMPTY | (b->regs[0] & FIFO_WOF) : 0;
}

static void hstx_fifo_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    switch (reg) {
    case 0x0:                                       /* STAT: WOF is W1C */
        b->regs[0] &= ~(value & FIFO_WOF);
        break;
    case 0x4:                                       /* FIFO */
        hstx_shift(b->soc, value);
        break;
    }
}

void rp2350_hstx_attach(RP2350State *s, RP2HstxFn *fn, void *opaque)
{
    s->hstx_fn = fn;
    s->hstx_opaque = opaque;
}

void rp2350_hstx_realize(RP2350State *s)
{
    rp2_block_init(s, &s->hstx_ctrl, "rp2350.hstx_ctrl", 0x400c0000, NULL,
                   NULL);
    rp2_block_init(s, &s->hstx_fifo, "rp2350.hstx_fifo", 0x50600000,
                   hstx_fifo_read, hstx_fifo_write);
}

void rp2350_hstx_reset(RP2350State *s)
{
    memset(s->hstx_ctrl.regs, 0, sizeof(s->hstx_ctrl.regs));
    s->hstx_ctrl.regs[0] = 0x10050600;              /* CSR's reset value */
    for (int b = 0; b < 8; b++) {
        s->hstx_ctrl.regs[1 + b] = b | b << 8;
    }
    memset(s->hstx_fifo.regs, 0, sizeof(s->hstx_fifo.regs));
}
