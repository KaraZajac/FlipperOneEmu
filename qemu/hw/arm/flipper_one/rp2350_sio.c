/*
 * The RP2350's SIO (single-cycle IO), on each core's own port
 *
 *   CPUID          the core that reads it
 *   GPIO           OUT and OE, with their SET, CLR and XOR registers, for
 *                  pins 0-31 and for the rest (32-47, QSPI, USB); IN is
 *                  what SIO drives where it drives, what the board does
 *                  elsewhere.  QSPI's CS reads high (the flash's pull-up),
 *                  low with the BOOTSEL button held
 *   FIFOs          one each way between the cores, 8 words deep
 *   spinlocks      32; doorbells; PERI_NONSEC
 *
 * The NonSecure copy at 0xd0020000 is the same registers, as yet.  The
 * interpolators, the RISC-V timer and the TMDS encoder are not modelled:
 * logged.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "rp2350.h"

#define SIO_HI_QSPI_SD      0xf0000000u
#define SIO_HI_QSPI_CSN     0x08000000u
#define FIFO_VLD            0x1
#define FIFO_RDY            0x2
#define FIFO_WOF            0x4
#define FIFO_ROE            0x8

int rp2350_current_core(RP2350State *s)
{
    for (int i = 0; i < ARRAY_SIZE(s->cpu); i++) {
        if (s->cpu[i].cpu && current_cpu == CPU(s->cpu[i].cpu)) {
            return i;
        }
    }
    return 0;
}

uint32_t rp2350_sio_in(RP2350State *s, int hi)
{
    RP2SIO *o = &s->sio;
    uint32_t qspi = o->ext[1] & 0xfc000000u;

    if (!hi) {
        return s->pin_level;
    }
    if (s->bootsel) {
        qspi &= ~SIO_HI_QSPI_CSN;
    }
    return ((s->pin_level >> 32) & 0xffff) | qspi;
}

static uint64_t sio_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2350State *s = opaque;
    RP2SIO *o = &s->sio;
    int core = rp2350_current_core(s);
    hwaddr reg = addr & 0x1fffc;
    uint32_t v;

    switch (reg) {
    case 0x00:                                      /* CPUID */
        return core;
    case 0x04:                                      /* GPIO_IN */
        return rp2350_sio_in(s, 0);
    case 0x08:                                      /* GPIO_HI_IN */
        return rp2350_sio_in(s, 1);
    case 0x10:
        return o->out[0];
    case 0x14:
        return o->out[1];
    case 0x30:
        return o->oe[0];
    case 0x34:
        return o->oe[1];
    case 0x50:                                      /* FIFO_ST */
        return (o->fifo_n[core] ? FIFO_VLD : 0) |
               (o->fifo_n[!core] < RP2_FIFO_DEPTH ? FIFO_RDY : 0) |
               o->fifo_err[core];
    case 0x58:                                      /* FIFO_RD */
        if (!o->fifo_n[core]) {
            o->fifo_err[core] |= FIFO_ROE;
            return 0;
        }
        v = o->fifo[core][o->fifo_head[core]];
        o->fifo_head[core] = (o->fifo_head[core] + 1) % RP2_FIFO_DEPTH;
        o->fifo_n[core]--;
        return v;
    case 0x5c:                                      /* SPINLOCK_ST */
        return o->spinlock;
    case 0x100 ... 0x17c:                           /* SPINLOCKn: claim */
        v = 1u << ((reg - 0x100) / 4);
        if (o->spinlock & v) {
            return 0;
        }
        o->spinlock |= v;
        return v;
    case 0x188:                                     /* DOORBELL_IN_SET */
    case 0x18c:                                     /* DOORBELL_IN_CLR */
        return o->doorbell[core];
    case 0x190:
        return o->peri_nonsec;
    default:
        qemu_log_mask(LOG_UNIMP, "rp2350: SIO: read of 0x%03x\n",
                      (unsigned)reg);
        return 0;
    }
}

static void sio_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size)
{
    RP2350State *s = opaque;
    RP2SIO *o = &s->sio;
    int core = rp2350_current_core(s);
    hwaddr reg = addr & 0x1fffc;
    uint32_t v = value;
    int hi = (reg >> 2) & 1;

    switch (reg) {
    case 0x10: case 0x14:                           /* GPIO_OUT */
        o->out[hi] = v;
        break;
    case 0x18: case 0x1c:                           /* ..._SET */
        o->out[hi] |= v;
        break;
    case 0x20: case 0x24:                           /* ..._CLR */
        o->out[hi] &= ~v;
        break;
    case 0x28: case 0x2c:                           /* ..._XOR */
        o->out[hi] ^= v;
        break;
    case 0x30: case 0x34:                           /* GPIO_OE */
        o->oe[hi] = v;
        break;
    case 0x38: case 0x3c:
        o->oe[hi] |= v;
        break;
    case 0x40: case 0x44:
        o->oe[hi] &= ~v;
        break;
    case 0x48: case 0x4c:
        o->oe[hi] ^= v;
        break;
    case 0x50:                                      /* FIFO_ST: W1C */
        o->fifo_err[core] &= ~(v & (FIFO_WOF | FIFO_ROE));
        break;
    case 0x54:                                      /* FIFO_WR: the other's */
        if (o->fifo_n[!core] == RP2_FIFO_DEPTH) {
            o->fifo_err[core] |= FIFO_WOF;
            break;
        }
        o->fifo[!core][(o->fifo_head[!core] + o->fifo_n[!core]) %
                       RP2_FIFO_DEPTH] = v;
        o->fifo_n[!core]++;
        break;
    case 0x100 ... 0x17c:                           /* SPINLOCKn: release */
        o->spinlock &= ~(1u << ((reg - 0x100) / 4));
        break;
    case 0x180:                                     /* DOORBELL_OUT_SET */
        o->doorbell[!core] |= v;
        break;
    case 0x184:                                     /* DOORBELL_OUT_CLR */
        o->doorbell[!core] &= ~v;
        break;
    case 0x188:                                     /* DOORBELL_IN_SET */
        o->doorbell[core] |= v;
        break;
    case 0x18c:                                     /* DOORBELL_IN_CLR */
        o->doorbell[core] &= ~v;
        break;
    case 0x190:
        o->peri_nonsec = v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rp2350: SIO: write of 0x%08x to 0x%03x\n",
                      v, (unsigned)reg);
    }
    if (reg >= 0x10 && reg <= 0x4c) {                /* OUT, OE: the pins */
        rp2350_pins_update(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static const MemoryRegionOps sio_ops = {
    .read = sio_read,
    .write = sio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void rp2350_sio_realize(RP2350State *s)
{
    /* the Secure registers and their NonSecure copy */
    memory_region_init_io(&s->sio.mr, OBJECT(s), &sio_ops, s, "rp2350.sio",
                          0x40000);
    s->sio.mr.disable_reentrancy_guard = true;
    memory_region_add_subregion(&s->bus, RP2350_SIO_BASE, &s->sio.mr);
}

void rp2350_sio_reset(RP2350State *s)
{
    RP2SIO *o = &s->sio;

    memset(o->out, 0, sizeof(o->out));
    memset(o->oe, 0, sizeof(o->oe));
    memset(o->fifo_n, 0, sizeof(o->fifo_n));
    memset(o->fifo_head, 0, sizeof(o->fifo_head));
    memset(o->fifo_err, 0, sizeof(o->fifo_err));
    memset(o->doorbell, 0, sizeof(o->doorbell));
    o->spinlock = 0;
    o->peri_nonsec = 0;
    /* the flash's CS and data lines pulled up */
    o->ext[0] = 0;
    o->ext[1] = SIO_HI_QSPI_CSN | SIO_HI_QSPI_SD;
}
