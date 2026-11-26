/*
 * The RK3576's SARADC: eight channels of 12 bits over 1.8 V (the RK3588's
 * second version, which its device tree falls back to)
 *
 * A conversion is a write of CONV_CON with START, SINGLE_MODE and the
 * channel (bits 3-0), the bits it changes as a mask in its top half, as
 * both U-Boot and Linux write it. 20 us later the channel's DATA has its
 * code, START has cleared itself, END_INT_ST's bit 0 is set (a 1 written
 * clears it) and, while END_INT_EN's bit 0 is set (masked the same way),
 * SPI 124 is high. U-Boot polls END_INT_ST; Linux takes the interrupt.
 * STATUS's bit 0 is set while a conversion runs. What a channel reads is
 * the board's to say (the SoC's saradc_in); without it, 0. The comparators,
 * their interrupts, the debounce and the auto mode keep what is written and
 * do nothing.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "rk3576.h"

#define CONV_CON        0x000
#define END_INT_EN      0x104
#define STATUS          0x10c
#define END_INT_ST      0x110
#define DATA(n)         (0x120 + (n) * 4)
#define DATA_END        DATA(16)

#define CONV_START      BIT(4)
#define CONV_CHANNEL    0xf
#define END_INT         BIT(0)
#define BUSY            BIT(0)
#define CONVERSION_NS   20000

static void saradc_irq(RkSaradc *a)
{
    qemu_set_irq(a->irq, (a->regs[END_INT_ST / 4] & END_INT) &&
                         (a->regs[END_INT_EN / 4] & END_INT));
}

static void saradc_done(void *opaque)
{
    RkSaradc *a = opaque;
    RK3576State *soc = a->soc;
    int ch = a->regs[CONV_CON / 4] & CONV_CHANNEL;
    int code = 0;

    if (ch < RK3576_SARADC_CHANNELS && soc->saradc_in) {
        code = soc->saradc_in(soc->saradc_in_opaque, ch);
    }
    a->regs[DATA(ch) / 4] = MIN(MAX(code, 0), 0xfff);
    a->regs[CONV_CON / 4] &= ~CONV_START;
    a->regs[STATUS / 4] &= ~BUSY;
    a->regs[END_INT_ST / 4] |= END_INT;
    saradc_irq(a);
}

static uint64_t saradc_read(void *opaque, hwaddr addr, unsigned size)
{
    RkSaradc *a = opaque;

    return addr < sizeof(a->regs) ? a->regs[addr / 4] : 0;
}

static void saradc_write(void *opaque, hwaddr addr, uint64_t v,
                         unsigned size)
{
    RkSaradc *a = opaque;
    uint32_t mask = v >> 16;

    if (addr >= sizeof(a->regs) || addr == STATUS ||
        (addr >= DATA(0) && addr < DATA_END)) {
        return;
    }
    switch (addr) {
    case CONV_CON:
    case END_INT_EN:
        a->regs[addr / 4] = (a->regs[addr / 4] & ~mask) | (v & mask);
        if (addr == CONV_CON && (v & mask & CONV_START)) {
            a->regs[STATUS / 4] |= BUSY;
            timer_mod(a->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                CONVERSION_NS);
        }
        break;
    case END_INT_ST:
        a->regs[addr / 4] &= ~v;
        break;
    default:
        a->regs[addr / 4] = v;
        break;
    }
    saradc_irq(a);
}

static const MemoryRegionOps saradc_ops = {
    .read = saradc_read,
    .write = saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void rk_saradc_reset(RkSaradc *a)
{
    timer_del(a->timer);
    memset(a->regs, 0, sizeof(a->regs));
    saradc_irq(a);
}

void rk_saradc_init(RK3576State *s, RkSaradc *a, hwaddr base, qemu_irq irq)
{
    a->soc = s;
    a->irq = irq;
    a->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, saradc_done, a);
    memory_region_init_io(&a->mr, OBJECT(s), &saradc_ops, a, "rk3576.saradc",
                          0x10000);
    memory_region_add_subregion(get_system_memory(), base, &a->mr);
}
