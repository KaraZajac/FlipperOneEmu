/*
 * Raspberry Pi RP2350 (the Flipper One's MCU)
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HW_ARM_FLIPPER_ONE_RP2350_H
#define HW_ARM_FLIPPER_ONE_RP2350_H

#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_RP2350 "rp2350"
OBJECT_DECLARE_SIMPLE_TYPE(RP2350State, RP2350)

#define RP2350_NUM_IRQ      52
#define RP2350_ROM_SIZE     (32 * KiB)
#define RP2350_SRAM_SIZE    (520 * KiB)
#define RP2350_FLASH_SIZE   (16 * MiB)       /* the Flipper One's QSPI flash */
#define RP2350_XIP_SRAM_SIZE (16 * KiB)
#define RP2350_BOOTRAM_SIZE (1 * KiB)

#define RP2350_ROM_BASE         0x00000000
#define RP2350_XIP_BASE         0x10000000
#define RP2350_XIP_SRAM_BASE    0x13ffc000
#define RP2350_XIP_NOCACHE_BASE 0x14000000
#define RP2350_XIP_MAINT_BASE   0x18000000
#define RP2350_XIP_NOTRANS_BASE 0x1c000000
#define RP2350_SRAM_BASE        0x20000000
#define RP2350_BOOTRAM_BASE     0x400e0000
#define RP2350_SIO_BASE         0xd0000000

/*
 * A peripheral's registers, with the RP2350's aliases: its 4 KiB of
 * registers is followed by an XOR, a set and a clear alias of them
 * (+0x1000, +0x2000, +0x3000), which the peripheral applies itself.  A
 * narrow write is the byte or half-word copied across the word, as the
 * APB bridge does.
 */
#define RP2_REGS 256                    /* 0x400 bytes of registers */
typedef struct RP2Block RP2Block;
/* a register's value (offset @reg): NULL reads regs[] */
typedef uint32_t RP2ReadFn(RP2Block *b, hwaddr reg);
/* a write, after the alias: NULL stores it in regs[] */
typedef void RP2WriteFn(RP2Block *b, hwaddr reg, uint32_t value);
struct RP2Block {
    MemoryRegion mr;
    struct RP2350State *soc;
    uint32_t regs[RP2_REGS];
    RP2ReadFn *read;
    RP2WriteFn *write;
};

/* SIO: as each core sees it (CPUID, its FIFO) */
#define RP2_FIFO_DEPTH 8
typedef struct RP2SIO {
    MemoryRegion mr;
    uint32_t out[2], oe[2];             /* GPIO: [0] pins 0-31, [1] the rest */
    uint32_t ext[2];                    /* what drives the inputs from outside */
    uint32_t fifo[2][RP2_FIFO_DEPTH];   /* fifo[c]: what core c reads */
    int fifo_n[2], fifo_head[2];
    uint32_t fifo_err[2];               /* each core's WOF and ROE */
    uint32_t spinlock;
    uint32_t doorbell[2];
    uint32_t peri_nonsec;
} RP2SIO;

/* SHA-256 accelerator */
typedef struct RP2SHA {
    uint32_t h[8];
    uint32_t w[16];
    int n;                              /* words of this block so far */
} RP2SHA;

/* the redundancy coprocessor (CP7): one block, a salt for each core */
typedef struct RP2350RCP {
    uint64_t salt[2];
    bool salted[2];
    uint8_t count[2];           /* each core's sequence counter */
    uint64_t prng[2];           /* random bytes and delays, from the salt */
} RP2350RCP;

typedef struct RP2350Core {
    struct RP2350State *soc;
    int index;
} RP2350Core;

struct RP2350State {
    SysBusDevice parent_obj;

    ARMv7MState cpu[1];         /* core 0; core 1 comes with SIO's FIFOs */
    RP2350Core core[1];
    Clock *sysclk;

    MemoryRegion bus;           /* what the cores see, the PPB aside */
    MemoryRegion rom;
    MemoryRegion sram;
    MemoryRegion xip;           /* the flash, as XIP reads it */
    MemoryRegion xip_nocache, xip_notrans;
    MemoryRegion xip_sram;
    MemoryRegion bootram;

    RP2350RCP rcp;

    /* rp2350_sys.c */
    RP2Block resets, rosc, trng, sha256, powman, watchdog;
    RP2Block xosc, pll_sys, pll_usb, clocks, psm;
    RP2SHA sha;

    /* rp2350_sio.c */
    RP2SIO sio;
    MemoryRegion usb_dpram;     /* the USB controller's RAM, 4 KiB */
    bool bootsel;               /* the BOOTSEL button: QSPI CS held low */

    char *bootrom;              /* the boot ROM's image, 32 KiB */
    char *flash;                /* the flash's image, up to 16 MiB */
};

/* rp2350_coproc.c */
void rp2350_coproc_init(RP2350State *s, int core);
void rp2350_coproc_reset(RP2350State *s);

/* rp2350_sys.c */
void rp2_block_init(RP2350State *s, RP2Block *b, const char *name,
                    hwaddr base, RP2ReadFn *read, RP2WriteFn *write);
void rp2350_sys_realize(RP2350State *s);
void rp2350_sys_reset(RP2350State *s);

/* rp2350_sio.c */
int rp2350_current_core(RP2350State *s);
void rp2350_sio_realize(RP2350State *s);
void rp2350_sio_reset(RP2350State *s);

#endif
