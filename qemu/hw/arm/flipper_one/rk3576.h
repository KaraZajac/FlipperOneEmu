/*
 * Rockchip RK3576: the Flipper One's application processor
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HW_ARM_FLIPPER_ONE_RK3576_H
#define HW_ARM_FLIPPER_ONE_RK3576_H

#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gic.h"
#include "target/arm/cpu.h"

#define TYPE_RK3576 "rk3576"
OBJECT_DECLARE_SIMPLE_TYPE(RK3576State, RK3576)

/*
 * A block of Rockchip registers: what is written is kept, and most take
 * the bits they change as a mask in their top half (HIWORD_UPDATE: the
 * CRU's, the GRFs'); a block's own hooks add what its registers do.
 */
typedef struct RkBlock RkBlock;
struct RkBlock {
    MemoryRegion mr;
    struct RK3576State *soc;
    uint32_t *regs;
    bool hiword;
    uint32_t (*read)(RkBlock *b, hwaddr off);   /* NULL: the register */
    /* after a write: the register was @old, @v was written */
    void (*wrote)(RkBlock *b, hwaddr off, uint32_t old, uint32_t v);
};

/* a Rockchip timer (rk3288's kind): a 64-bit count down from LOAD */
typedef struct RkTimer {
    MemoryRegion mr;
    struct RK3576State *soc;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t load[2], ctrl, status;
    bool done;                          /* a one-shot count ran out */
    int64_t start_ns;                   /* its count began then */
    uint64_t hz;                        /* ... at this rate */
} RkTimer;

#define RK3576_NCPUS        8           /* 4 Cortex-A53, then 4 Cortex-A72 */
#define RK3576_DRAM_BASE    0x40000000ULL
#define RK3576_GIC_SPIS     448         /* the GIC-400's, as the SoC has it */

struct RK3576State {
    SysBusDevice parent_obj;

    CPUClusterState cluster[2];         /* little (A53s), big (A72s) */
    ARMCPU cpu[RK3576_NCPUS];
    GICState gic;
    RkBlock cru;                        /* clocks and resets */
    RkBlock pmu;                        /* power domains */
    RkBlock rng;                        /* the TRNG */
    RkTimer timer0;                     /* the broadcast clockevent */
    bool start_on;                      /* CPU 0 runs from the start (a
                                           kernel given), else all are off */
};

/* an SPI of the GIC, as the device tree numbers them */
qemu_irq rk3576_spi(RK3576State *s, int spi);

#endif
