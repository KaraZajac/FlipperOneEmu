/*
 * What each Cortex-M33 has in its PPB that QEMU's armv7m does not keep
 *
 *   0xe0001000  DWT: CYCCNT, clk_sys's cycles on the virtual clock while
 *               CTRL.CYCCNTENA and DEMCR.TRCENA are set (the firmware's
 *               furi_delay_us() counts them); 4 comparators kept, unused
 *   0xe000e008  ACTLR, kept
 *   0xe000edfc  DEMCR, kept (TRCENA starts the DWT)
 *   0xe0080000  the RP2350's EPPB: NMI_MASK0/1, SLEEPCTRL, kept
 *
 * Over QEMU's own PPB in each core's container: the NVIC's registers and its
 * RAZ/WI default.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "rp2350.h"

#define DWT_CYCCNTENA       0x1
#define DEMCR_TRCENA        (1u << 24)

static bool dwt_counting(RP2Ppb *b)
{
    return (b->dwt_ctrl & DWT_CYCCNTENA) && (b->demcr & DEMCR_TRCENA);
}

static uint64_t dwt_cycles(RP2Ppb *b)
{
    uint64_t hz = clock_get_hz(b->soc->sysclk) ?: 150000000;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!dwt_counting(b)) {
        return b->base_cycles;
    }
    return b->base_cycles + muldiv64(now - b->base_ns, hz,
                                     NANOSECONDS_PER_SECOND);
}

/* before what makes it count or stop changes: the count so far */
static void dwt_rebase(RP2Ppb *b)
{
    b->base_cycles = dwt_cycles(b);
    b->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t dwt_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2Ppb *b = opaque;

    switch (addr) {
    case 0x000:
        return b->dwt_ctrl | 0x40000000;            /* NUMCOMP 4 */
    case 0x004:
        return (uint32_t)dwt_cycles(b);
    }
    if (addr >= 0x020 && addr < 0x060) {
        return b->comp[(addr - 0x020) / 4];
    }
    return 0;
}

static void dwt_write(void *opaque, hwaddr addr, uint64_t value,
                      unsigned size)
{
    RP2Ppb *b = opaque;

    switch (addr) {
    case 0x000:
        dwt_rebase(b);
        b->dwt_ctrl = value & 0x0fffffff;
        return;
    case 0x004:
        b->base_cycles = (uint32_t)value;
        b->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    }
    if (addr >= 0x020 && addr < 0x060) {
        b->comp[(addr - 0x020) / 4] = value;
    }
}

static const MemoryRegionOps dwt_ops = {
    .read = dwt_read,
    .write = dwt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

/* the single words: ACTLR, DEMCR; the EPPB's three */
static uint64_t word_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *w = opaque;

    return w[addr / 4];
}

static void word_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    uint32_t *w = opaque;

    w[addr / 4] = value;
}

static const MemoryRegionOps word_ops = {
    .read = word_read,
    .write = word_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void demcr_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    RP2Ppb *b = opaque;

    dwt_rebase(b);
    b->demcr = value;
}

static uint64_t demcr_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((RP2Ppb *)opaque)->demcr;
}

static const MemoryRegionOps demcr_ops = {
    .read = demcr_read,
    .write = demcr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

void rp2350_ppb_realize(RP2350State *s, int core)
{
    RP2Ppb *b = &s->ppb[core];
    MemoryRegion *c = &s->cpu[core].container;
    Object *obj = OBJECT(s);

    b->soc = s;
    b->core = core;
    memory_region_init_io(&b->dwt, obj, &dwt_ops, b, "rp2350.dwt", 0x1000);
    memory_region_add_subregion_overlap(c, 0xe0001000, &b->dwt, 1);
    memory_region_init_io(&b->actlr, obj, &word_ops, &b->actlr_value,
                          "rp2350.actlr", 4);
    memory_region_add_subregion_overlap(c, 0xe000e008, &b->actlr, 1);
    memory_region_init_io(&b->demcr_mr, obj, &demcr_ops, b, "rp2350.demcr", 4);
    memory_region_add_subregion_overlap(c, 0xe000edfc, &b->demcr_mr, 1);
    memory_region_init_io(&b->eppb, obj, &word_ops, b->eppb_regs,
                          "rp2350.eppb", sizeof(b->eppb_regs));
    memory_region_add_subregion_overlap(c, 0xe0080000, &b->eppb, 1);
}

void rp2350_ppb_reset(RP2350State *s)
{
    for (int core = 0; core < 2; core++) {
        RP2Ppb *b = &s->ppb[core];

        b->dwt_ctrl = 0;
        b->demcr = 0;
        b->actlr_value = 0;
        b->base_cycles = 0;
        b->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        memset(b->comp, 0, sizeof(b->comp));
        memset(b->eppb_regs, 0, sizeof(b->eppb_regs));
    }
}
