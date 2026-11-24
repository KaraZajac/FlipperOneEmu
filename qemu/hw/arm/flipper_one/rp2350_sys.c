/*
 * The RP2350's system blocks, and the register blocks they are made of
 *
 *   RESETS    a peripheral is out of reset as soon as it is let go
 *   ROSC      the ring oscillator: running and stable; its random bit
 *   TRNG      its entropy registers always hold fresh random words (QEMU's
 *             guest random source: -seed makes them repeat)
 *   SHA256    the accelerator: SHA-256 a 64-byte block at a time, as the
 *             words come, with its byte swap of what is written (the SUM
 *             registers are the hash's words as they are: the boot ROM and
 *             the SDK swap them themselves)
 *   POWMAN    registers that want the password 0x5afe in their top half
 *             (scratch and boot registers aside); a power-on reset
 *   WATCHDOG  its scratch and reason registers (no timer yet)
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "rp2350.h"

/* ------------------------------------------------------ register blocks */

static uint32_t rp2_block_get(RP2Block *b, hwaddr reg)
{
    if (b->read) {
        return b->read(b, reg);
    }
    return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
}

static uint64_t rp2_block_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2Block *b = opaque;
    uint32_t v = rp2_block_get(b, addr & 0xffc);

    return extract32(v, (addr & 3) * 8, size * 8);
}

static void rp2_block_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    RP2Block *b = opaque;
    hwaddr reg = addr & 0xffc;
    uint32_t v = value;

    if (b->raw && !(addr >> 12) && b->raw(b, addr, value, size)) {
        return;
    }
    if (size == 1) {
        v = (v & 0xff) * 0x01010101u;
    } else if (size == 2) {
        v = (v & 0xffff) * 0x00010001u;
    }
    switch (addr >> 12) {
    case 1:
        v ^= rp2_block_get(b, reg);
        break;
    case 2:
        v |= rp2_block_get(b, reg);
        break;
    case 3:
        v = rp2_block_get(b, reg) & ~v;
        break;
    }
    if (b->write) {
        b->write(b, reg, v);
    } else if (reg < RP2_REGS * 4) {
        b->regs[reg / 4] = v;
    }
}

static const MemoryRegionOps rp2_block_ops = {
    .read = rp2_block_read,
    .write = rp2_block_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void rp2_block_init(RP2350State *s, RP2Block *b, const char *name,
                    hwaddr base, RP2ReadFn *read, RP2WriteFn *write)
{
    b->soc = s;
    b->read = read;
    b->write = write;
    memory_region_init_io(&b->mr, OBJECT(s), &rp2_block_ops, b, name,
                          0x4000);
    /*
     * All of the SoC's blocks are one QEMU device, whose re-entrancy guard
     * would refuse the DMA (its register write starting a transfer) the
     * block it writes to: as on the chip, a block may be reached from
     * another's access.
     */
    b->mr.disable_reentrancy_guard = true;
    memory_region_add_subregion(&s->bus, base, &b->mr);
}

#define RS(b) ((b)->soc)

/* ---------------------------------------------------------------- RESETS */

#define RESETS_ALL 0x1fffffffu

static uint32_t resets_read(RP2Block *b, hwaddr reg)
{
    switch (reg) {
    case 0x8:                                       /* RESET_DONE */
        return ~b->regs[0] & RESETS_ALL;
    default:
        return reg < 0x8 ? b->regs[reg / 4] : 0;
    }
}

static void resets_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    switch (reg) {
    case 0x0:                                       /* RESET */
    case 0x4:                                       /* WDSEL */
        b->regs[reg / 4] = value & RESETS_ALL;
        break;
    }
}

/* ------------------------------------------------------------------ ROSC */

static uint32_t rosc_read(RP2Block *b, hwaddr reg)
{
    uint8_t bit;

    switch (reg) {
    case 0x1c:                                      /* STATUS */
        return 0x80000000u | 0x10000 | 0x1000;      /* stable, running */
    case 0x20:                                      /* RANDOMBIT */
        qemu_guest_getrandom_nofail(&bit, 1);
        return bit & 1;
    case 0x24:                                      /* COUNT: done */
        return 0;
    default:
        return reg < 0x28 ? b->regs[reg / 4] : 0;
    }
}

/* ------------------------------------------------------------------- OTP */

/*
 * OTP's registers: the 64 pages' software locks (SW_LOCKn: SEC in bits 1:0,
 * NSEC in 3:2; a write is ORed in, a lock only tightens), which the boot
 * ROM sets from the OTP's page settings and reads back before its USB boot.
 * The rest keep what is written. The OTP itself reads as blank
 * (otp_data, unimplemented).
 */
static void otp_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    if (reg < 0x100) {
        b->regs[reg / 4] |= value & 0xf;            /* SW_LOCK0 ... 63 */
    } else if (reg < RP2_REGS * 4) {
        b->regs[reg / 4] = value;
    }
}

/* ------------------------------------------------------------------ TRNG */

static uint32_t trng_read(RP2Block *b, hwaddr reg)
{
    uint32_t v;

    switch (reg) {
    case 0x104:                                     /* RNG_ISR */
    case 0x110:                                     /* TRNG_VALID */
        return 1;                                   /* EHR_VALID */
    case 0x114 ... 0x128:                           /* EHR_DATA0..5 */
        qemu_guest_getrandom_nofail(&v, sizeof(v));
        return v;
    case 0x1b8:                                     /* TRNG_BUSY */
    case 0x140:                                     /* TRNG_SW_RESET */
        return 0;
    case 0x1c0:                                     /* RNG_VERSION */
        return 0x000000ef;
    default:
        return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
    }
}

/* ---------------------------------------------------------------- SHA256 */

#define SHA_CSR_START       0x1
#define SHA_CSR_WDATA_RDY   0x2
#define SHA_CSR_SUM_VLD     0x4
#define SHA_CSR_BSWAP       0x1000
#define SHA_CSR_KEEP        (0x0300 | SHA_CSR_BSWAP)    /* DMA_SIZE, BSWAP */

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static const uint32_t sha_h0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static void sha_block(RP2SHA *h)
{
    uint32_t w[64], v[8], t1, t2;

    memcpy(w, h->w, sizeof(h->w));
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h->h, sizeof(v));
    for (int i = 0; i < 64; i++) {
        t1 = v[7] + (ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25)) +
             ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
        t2 = (ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22)) +
             ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(&v[1], &v[0], 7 * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) {
        h->h[i] += v[i];
    }
}

static uint32_t sha256_read(RP2Block *b, hwaddr reg)
{
    RP2SHA *h = &RS(b)->sha;
    uint32_t csr = b->regs[0];

    switch (reg) {
    case 0x0:                                       /* CSR */
        return (csr & SHA_CSR_KEEP) | SHA_CSR_WDATA_RDY |
               (h->n == 0 && h->nbytes == 0 ? SHA_CSR_SUM_VLD : 0);
    case 0x8 ... 0x24:          /* SUM0..7, as computed: BSWAP is input's */
        return h->h[(reg - 0x8) / 4];
    default:
        return 0;
    }
}

static void sha256_word(RP2Block *b, uint32_t value)
{
    RP2SHA *h = &RS(b)->sha;

    h->w[h->n++] = b->regs[0] & SHA_CSR_BSWAP ? bswap32(value) : value;
    if (h->n == 16) {
        sha_block(h);
        h->n = 0;
    }
}

static void sha256_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2SHA *h = &RS(b)->sha;

    switch (reg) {
    case 0x0:                                       /* CSR */
        b->regs[0] = value & SHA_CSR_KEEP;
        if (value & SHA_CSR_START) {
            memcpy(h->h, sha_h0, sizeof(h->h));
            h->n = h->nbytes = 0;
            h->acc = 0;
        }
        break;
    case 0x4:                                       /* WDATA: a whole word */
        sha256_word(b, value);
        break;
    }
}

/*
 * WDATA takes bytes and half-words too, packed into a word in the order
 * they come (as from consecutive addresses of a little-endian word), which
 * is then as a word written whole: the boot ROM pads with bytes and ends
 * with the length's words.
 */
static bool sha256_raw(RP2Block *b, hwaddr addr, uint32_t value,
                       unsigned size)
{
    RP2SHA *h = &RS(b)->sha;

    if ((addr & 0xffc) != 0x4 || (size == 4 && h->nbytes == 0)) {
        return false;
    }
    for (unsigned i = 0; i < size; i++) {
        h->acc |= ((value >> (i * 8)) & 0xff) << (h->nbytes * 8);
        if (++h->nbytes == 4) {
            sha256_word(b, h->acc);
            h->acc = 0;
            h->nbytes = 0;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- POWMAN */

#define POWMAN_PASSWORD     0x5afe0000u
#define POWMAN_CHIP_RESET   (0x2c / 4)
#define POWMAN_HAD_POR      0x00010000u

static uint32_t powman_read(RP2Block *b, hwaddr reg)
{
    switch (reg) {
    case 0x8:                                       /* VREG_STS: ok */
        return 0x10;
    default:
        return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
    }
}

static void powman_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    if (reg >= 0xb0 && reg <= 0xdc) {               /* SCRATCH0-7, BOOT0-3 */
        b->regs[reg / 4] = value;
        return;
    }
    if ((value & 0xffff0000u) != POWMAN_PASSWORD) {
        b->regs[0] = 1;                             /* BADPASSWD */
        return;
    }
    value &= 0xffff;
    switch (reg) {
    case 0x0:                                       /* BADPASSWD: W1C */
        b->regs[0] &= ~value;
        break;
    case 0x2c:                                      /* CHIP_RESET */
        b->regs[POWMAN_CHIP_RESET] =
            (b->regs[POWMAN_CHIP_RESET] & ~0x11u) | (value & 0x11u);
        break;
    default:
        if (reg < RP2_REGS * 4) {
            b->regs[reg / 4] = value;
        }
    }
}

/* -------------------------------------------------------------- WATCHDOG */

static void watchdog_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    switch (reg) {
    case 0x0:                                       /* CTRL */
        if (value & 0x80000000u) {
            qemu_log_mask(LOG_UNIMP, "rp2350: watchdog: TRIGGER (a reset) "
                          "is not modelled yet\n");
        }
        b->regs[0] = value & ~0x80000000u;
        break;
    case 0x8:                                       /* REASON: read only */
        break;
    default:
        if (reg <= 0x28) {
            b->regs[reg / 4] = value;
        }
    }
}

/* ------------------------------------------------------------------ XOSC */

#define XOSC_ENABLE         0xfab000u               /* CTRL.ENABLE = ENABLE */

static uint32_t xosc_read(RP2Block *b, hwaddr reg)
{
    uint32_t ctrl = b->regs[0];

    switch (reg) {
    case 0x4:                                       /* STATUS */
        return (ctrl & 3) | ((ctrl & 0xfff000) == XOSC_ENABLE
                             ? 0x80000000u | 0x1000 : 0);
    case 0x10:                                      /* COUNT: done */
        return 0;
    default:
        return reg < 0x14 ? b->regs[reg / 4] : 0;
    }
}

/* -------------------------------------------------------------------- PLL */

static uint32_t pll_read(RP2Block *b, hwaddr reg)
{
    switch (reg) {
    case 0x0:                                       /* CS: locked */
        return b->regs[0] | 0x80000000u;
    default:
        return reg < 0x20 ? b->regs[reg / 4] : 0;
    }
}

/* ---------------------------------------------------------------- CLOCKS */

static uint32_t clocks_read(RP2Block *b, hwaddr reg)
{
    switch (reg) {
    case 0x38:                              /* CLK_REF_SELECTED: its SRC */
        return 1u << (b->regs[0x30 / 4] & 3);
    case 0x44:                              /* CLK_SYS_SELECTED: its SRC */
        return 1u << (b->regs[0x3c / 4] & 1);
    case 0x08: case 0x14: case 0x20: case 0x2c:     /* no glitchless mux */
    case 0x50: case 0x5c: case 0x68: case 0x74:
        return 1;
    case 0xa4:                              /* FC0_STATUS: DONE */
        return 0x10;
    default:
        return reg < RP2_REGS * 4 ? b->regs[reg / 4] : 0;
    }
}

/* ------------------------------------------------------------------- PSM */

#define PSM_ALL     0x01ffffffu
#define PSM_PROC1   0x01000000u

static uint32_t psm_read(RP2Block *b, hwaddr reg)
{
    switch (reg) {
    case 0xc:                                       /* DONE */
        return ~b->regs[1] & PSM_ALL;
    default:
        return reg < 0xc ? b->regs[reg / 4] : 0;
    }
}

/* FRCE_OFF.PROC1 holds core 1 off; letting go restarts it in its ROM */
static void psm_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    uint32_t old = b->regs[1];

    switch (reg) {
    case 0x0: case 0x8:                             /* FRCE_ON, WDSEL */
        b->regs[reg / 4] = value & PSM_ALL;
        break;
    case 0x4:                                       /* FRCE_OFF */
        b->regs[1] = value & PSM_ALL;
        if ((old ^ b->regs[1]) & PSM_PROC1) {
            rp2350_core1_power(b->soc, !(b->regs[1] & PSM_PROC1));
        }
        break;
    }
}

/* ---------------------------------------------------------------- BOOTRAM */

/*
 * Its 1 KiB of RAM is a RAM region over this block; the registers: two
 * write-once words (bits are only set), and eight locks that work as SIO's
 * spinlocks do (a read claims, a write lets go; BOOTLOCK_STAT has 1 for
 * unclaimed).
 */
#define BOOTRAM_STAT    (0x808 / 4)

static uint32_t bootram_read(RP2Block *b, hwaddr reg)
{
    uint32_t bit;

    switch (reg) {
    case 0x80c ... 0x828:                           /* BOOTLOCKn: claim */
        bit = 1u << ((reg - 0x80c) / 4);
        if (!(b->regs[BOOTRAM_STAT] & bit)) {
            return 0;
        }
        b->regs[BOOTRAM_STAT] &= ~bit;
        return bit;
    default:
        return reg >= 0x800 && reg < 0x80c ? b->regs[reg / 4] : 0;
    }
}

static void bootram_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    switch (reg) {
    case 0x800: case 0x804:                         /* WRITE_ONCE0/1 */
        b->regs[reg / 4] |= value;
        break;
    case 0x808:                                     /* BOOTLOCK_STAT */
        b->regs[BOOTRAM_STAT] = value & 0xff;
        break;
    case 0x80c ... 0x828:                           /* BOOTLOCKn: let go */
        b->regs[BOOTRAM_STAT] |= 1u << ((reg - 0x80c) / 4);
        break;
    }
}

/* -------------------------------------------------------------------- ADC */

/*
 * One conversion at a time, done at once: READY is EN.  The inputs are
 * GPIO40-47 (AIN0-7) and the temperature sensor (AIN8), in adc_mv[]: what
 * the board puts on them.  RESULT and the FIFO have the 12 bits of the
 * selected one; ROBIN steps through the inputs it names.
 */
#define ADC_CS_EN           0x1
#define ADC_CS_START_ONCE   0x4
#define ADC_CS_START_MANY   0x8
#define ADC_CS_READY        0x100

static uint32_t adc_convert(RP2350State *s)
{
    int ain = (s->adc.regs[0] >> 12) & 0xf;
    uint32_t mv = ain < 9 ? s->adc_mv[ain] : 0;
    uint32_t robin = (s->adc.regs[0] >> 16) & 0x1ff;

    if (robin) {                                    /* the next one it names */
        for (int i = 1; i <= 9; i++) {
            int next = (ain + i) % 9;

            if ((robin >> next) & 1) {
                s->adc.regs[0] = deposit32(s->adc.regs[0], 12, 4, next);
                break;
            }
        }
    }
    return MIN(mv * 4096 / 3300, 4095);
}

static uint32_t adc_read(RP2Block *b, hwaddr reg)
{
    RP2350State *s = b->soc;

    switch (reg) {
    case 0x0:                                       /* CS */
        return (b->regs[0] & ~ADC_CS_READY) |
               (b->regs[0] & ADC_CS_EN ? ADC_CS_READY : 0);
    case 0x8:                                       /* FCS: never empty */
        return b->regs[2] | (b->regs[0] & ADC_CS_START_MANY ? 1u << 16 : 0x100);
    case 0xc:                                       /* FIFO */
        return adc_convert(s);
    default:
        return reg <= 0x20 ? b->regs[reg / 4] : 0;
    }
}

static void adc_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2350State *s = b->soc;

    switch (reg) {
    case 0x0:                                       /* CS */
        b->regs[0] = value & ~(ADC_CS_START_ONCE | ADC_CS_READY | 0x600);
        if ((value & ADC_CS_START_ONCE) && (value & ADC_CS_EN)) {
            b->regs[1] = adc_convert(s);            /* RESULT */
        }
        break;
    case 0x4:                                       /* RESULT: read only */
        break;
    default:
        if (reg <= 0x20) {
            b->regs[reg / 4] = value;
        }
    }
}

/* ------------------------------------------------------------------------ */

void rp2350_sys_realize(RP2350State *s)
{
    rp2_block_init(s, &s->resets, "rp2350.resets", 0x40020000,
                   resets_read, resets_write);
    rp2_block_init(s, &s->rosc, "rp2350.rosc", 0x400e8000, rosc_read, NULL);
    rp2_block_init(s, &s->trng, "rp2350.trng", 0x400f0000, trng_read, NULL);
    rp2_block_init(s, &s->otp, "rp2350.otp", 0x40120000, NULL, otp_write);
    rp2_block_init(s, &s->sha256, "rp2350.sha256", 0x400f8000,
                   sha256_read, sha256_write);
    s->sha256.raw = sha256_raw;
    rp2_block_init(s, &s->powman, "rp2350.powman", 0x40100000,
                   powman_read, powman_write);
    rp2_block_init(s, &s->watchdog, "rp2350.watchdog", 0x400d8000,
                   NULL, watchdog_write);
    rp2_block_init(s, &s->xosc, "rp2350.xosc", 0x40048000, xosc_read, NULL);
    rp2_block_init(s, &s->pll_sys, "rp2350.pll_sys", 0x40050000,
                   pll_read, NULL);
    rp2_block_init(s, &s->pll_usb, "rp2350.pll_usb", 0x40058000,
                   pll_read, NULL);
    rp2_block_init(s, &s->clocks, "rp2350.clocks", 0x40010000,
                   clocks_read, NULL);
    rp2_block_init(s, &s->psm, "rp2350.psm", 0x40018000, psm_read, psm_write);
    rp2_block_init(s, &s->bootram_regs, "rp2350.bootram-regs",
                   RP2350_BOOTRAM_BASE, bootram_read, bootram_write);
    rp2_block_init(s, &s->adc, "rp2350.adc", 0x400a0000, adc_read, adc_write);
    for (int i = 0; i < 8; i++) {
        s->adc_mv[i] = 3300;                        /* the GPIOs: pulled up */
    }
    s->adc_mv[8] = 706;                             /* the sensor: 27 C */
    /* the pads: kept (rp2350_gpio.c reads them); the QSPI pins' blocks too */
    rp2_block_init(s, &s->io_qspi, "rp2350.io_qspi", 0x40030000, NULL, NULL);
    rp2_block_init(s, &s->pads_bank0, "rp2350.pads_bank0", 0x40038000,
                   NULL, NULL);
    rp2_block_init(s, &s->pads_qspi, "rp2350.pads_qspi", 0x40040000,
                   NULL, NULL);
}

void rp2350_sys_reset(RP2350State *s)
{
    memset(s->resets.regs, 0, sizeof(s->resets.regs));
    s->resets.regs[0] = RESETS_ALL;
    memset(s->rosc.regs, 0, sizeof(s->rosc.regs));
    s->rosc.regs[0] = 0xaa0;                        /* CTRL */
    memset(s->trng.regs, 0, sizeof(s->trng.regs));
    memset(s->otp.regs, 0, sizeof(s->otp.regs));
    memset(s->sha256.regs, 0, sizeof(s->sha256.regs));
    s->sha256.regs[0] = SHA_CSR_KEEP;               /* BSWAP, DMA_SIZE 2 */
    s->sha256.regs[0] &= ~0x0100u;
    memcpy(s->sha.h, sha_h0, sizeof(s->sha.h));
    s->sha.n = s->sha.nbytes = 0;
    s->sha.acc = 0;
    memset(s->powman.regs, 0, sizeof(s->powman.regs));
    s->powman.regs[POWMAN_CHIP_RESET] = POWMAN_HAD_POR;
    s->powman.regs[0x38 / 4] = 0xf;                 /* STATE */
    memset(s->watchdog.regs, 0, sizeof(s->watchdog.regs));
    memset(s->xosc.regs, 0, sizeof(s->xosc.regs));
    s->xosc.regs[0xc / 4] = 0xc4;                   /* STARTUP */
    for (int i = 0; i < 2; i++) {
        RP2Block *pll = i ? &s->pll_usb : &s->pll_sys;

        memset(pll->regs, 0, sizeof(pll->regs));
        pll->regs[0] = 1;                           /* CS: REFDIV 1 */
        pll->regs[1] = 0x2d;                        /* PWR: all down */
        pll->regs[3] = 0x77000;                     /* PRIM: 7, 7 */
    }
    memset(s->clocks.regs, 0, sizeof(s->clocks.regs));
    for (hwaddr div = 0x04; div <= 0x70; div += 0xc) {
        s->clocks.regs[div / 4] = 0x10000;          /* CLK_x_DIV: 1 */
    }
    s->clocks.regs[0xbc / 4] = 0xffffffff;          /* ENABLED0/1 */
    s->clocks.regs[0xc0 / 4] = 0xffffffff;
    memset(s->psm.regs, 0, sizeof(s->psm.regs));
    memset(s->adc.regs, 0, sizeof(s->adc.regs));
    memset(s->bootram_regs.regs, 0, sizeof(s->bootram_regs.regs));
    s->bootram_regs.regs[BOOTRAM_STAT] = 0xff;
    memset(s->io_qspi.regs, 0, sizeof(s->io_qspi.regs));
    memset(s->pads_bank0.regs, 0, sizeof(s->pads_bank0.regs));
    for (int pin = 0; pin < 50; pin++) {            /* GPIOs, SWCLK, SWD */
        s->pads_bank0.regs[1 + pin] = 0x116;        /* ISO, PDE, SCHMITT */
    }
    memset(s->pads_qspi.regs, 0, sizeof(s->pads_qspi.regs));
}
