/*
 * Rockchip RK3576: the Flipper One's application processor, so far as its
 * kernel needs it to start and to speak (the first step of its bring-up)
 *
 *   CPUs      4 Cortex-A53 (MPIDR 0x000-0x003) and 4 Cortex-A72 (0x100-
 *             0x103), each four a cluster of their own; EL2 and no EL3:
 *             PSCI is QEMU's, over SMC, as TF-A's BL31 gives it on the
 *             device (an SMC that PSCI does not know, as SCMI's, answers
 *             NOT_SUPPORTED); the generic timer at 24 MHz, as the SoC's
 *   GIC-400   GICD 0x2a701000, GICC 0x2a702000, GICH 0x2a704000, GICV
 *             0x2a706000; 448 SPIs; the timers on PPIs 13, 14, 11 and 10,
 *             the maintenance interrupt on 9 (the device tree's)
 *   CRU       0x27200000: what is written is kept (HIWORD_UPDATE); each
 *             PLL says it is locked (PLLCON6 bit 15). At reset every PLL
 *             is in slow mode: the clocks are the 24 MHz crystal's until
 *             the kernel sets them (U-Boot and TF-A have, on a device)
 *   TIMER0    0x2acc0000, SPI 45: the kernel's broadcast clockevent (the
 *             cores' own timers stop in their deep idle: local-timer-stop):
 *             a count down from LOAD at clk_timer0's rate, worked out from
 *             the CRU as the kernel works it out (CPLL or GPLL / n, or the
 *             crystal); one-shot or reloading; INT_STATUS, write 1 to clear
 *   PMU       0x27380000: the power domains (the kernel's table for the
 *             RK3576): PWR_CON (0x210, 0x214) asks each on or off, and the
 *             repair and chain status say it is (0x570, 0x248; memory
 *             0x250; the NPU's 0x230); an idle request (0x110, 0x114) is
 *             acknowledged and idle at once (0x120, 0x128)
 *   RNG       0x2a410000: the TRNG: a request (CTRL bit 4) has 32 bytes
 *             ready at once (STATE bit 4, write 1 to clear), from QEMU's
 *             guest random source (-seed makes them the same each run)
 *   GPIO0-4   0x27320000, 0x2ae10000-0x2ae40000; SPIs 153, 157, 161, 165,
 *             169: Rockchip's v2 banks (VER_ID 0x010219c8; each register in
 *             two halves with write masks): outputs, inputs from the board
 *             (pulled up unless it says otherwise), interrupts by level or
 *             edge (both edges with BOTHEDGE, whatever INT_TYPE says: a
 *             level there was an interrupt for ever from a card-detect pin
 *             held low), masked, cleared by EOI
 *   DMAC0-2   0x2ab90000, 0x2abb0000, 0x2abd0000: ARM PL330s (QEMU's), their
 *             event and abort interrupts on the first of their two SPIs
 *             (32, 34, 36); peripherals never hold them back. AMBA reads a
 *             PrimeCell's IDs at the end of its region, 16 KiB here: QEMU's
 *             4 KiB of registers are there, and their IDs again at 0x3fe0
 *   SPI0      rk_spi.c, 0x2acf0000, SPI 116: the display link (the board
 *             takes what it sends)
 *   SDMMC     dw_mshc.c, 0x2a310000, SPI 251: the microSD slot's host
 *   I2C0-8    rk3x_i2c.c: I2C0 at 0x27300000, I2C1-8 from 0x2ac40000 a
 *             0x10000 apart; SPIs 88-96
 *   UART0, UART4  DesignWare APB UARTs, 16550s with 32-bit registers
 *             (reg-shift 2) on a 24 MHz clock: UART0 is the debug port's
 *             console (serial0, the kernel's stdout), UART4 the line to the
 *             MCU (ttyS4); the machine gives them their chardevs
 *   the rest  0x20000000-0x3fffffff: unimplemented, logged (-d unimp),
 *             read as 0
 *
 * DRAM is the machine's, at 0x40000000. With no kernel given the cores
 * stay off (the board's rail for them is the MCU's to switch: not yet).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/guest-random.h"
#include "hw/core/or-irq.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "rk3576.h"

#define PERIPH_BASE     0x20000000
#define PERIPH_SIZE     0x20000000
#define GIC_BASE        0x2a700000      /* CBAR: the GIC-400's block */
#define GIC_DIST        0x2a701000
#define GIC_CPU         0x2a702000
#define GIC_VIFACE      0x2a704000
#define GIC_VCPU        0x2a706000
#define CRU_BASE        0x27200000
#define CRU_SIZE        0x50000
#define PMU_BASE        0x27380000
#define PMU_SIZE        0x800
#define RNG_BASE        0x2a410000
#define RNG_SIZE        0x200
#define TIMER0_BASE     0x2acc0000
#define TIMER0_SPI      45
#define UART0_BASE      0x2ad40000
#define UART0_SPI       76
#define UART4_BASE      0x2ad70000
#define UART4_SPI       80
#define UART_BAUDBASE   (24000000 / 16)
#define CNTFRQ_HZ       24000000

/* interrupt IDs, as the GIC numbers its private ones */
#define PPI_MAINTENANCE 25
#define PPI_HYP_TIMER   26
#define PPI_VIRT_TIMER  27
#define PPI_SEC_TIMER   29
#define PPI_PHYS_TIMER  30

static const uint64_t rk3576_mpidr[RK3576_NCPUS] = {
    0x000, 0x001, 0x002, 0x003,         /* the A53s */
    0x100, 0x101, 0x102, 0x103,         /* the A72s */
};

/* ------------------------------------------------- register blocks */

static uint64_t rk_read(void *opaque, hwaddr off, unsigned size)
{
    RkBlock *b = opaque;

    off &= ~3;
    return b->read ? b->read(b, off) : b->regs[off / 4];
}

static void rk_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkBlock *b = opaque;
    uint32_t *r = &b->regs[(off & ~3) / 4];
    uint32_t old = *r;

    if (b->hiword) {
        uint32_t mask = v >> 16;

        *r = (*r & ~mask) | (v & mask);
    } else {
        *r = v;
    }
    if (b->wrote) {
        b->wrote(b, off & ~3, old, v);
    }
}

static const MemoryRegionOps rk_ops = {
    .read = rk_read,
    .write = rk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk_block_init(RK3576State *s, RkBlock *b, const char *name,
                          hwaddr base, uint32_t size, bool hiword)
{
    b->soc = s;
    b->hiword = hiword;
    b->regs = g_new0(uint32_t, size / 4);
    memory_region_init_io(&b->mr, OBJECT(s), &rk_ops, b, name, size);
    memory_region_add_subregion(get_system_memory(), base, &b->mr);
}

/* -------------------------------------------------------------- CRU */

/* the PLLs' CON0 (RK3588-type: CON6 bit 15 is LOCK_STATUS): BPLL as the
 * kernel names it and as the big cores' CRU has it, LPLL (the little
 * cores', in the CCI's CRU), VPLL, AUPLL, CPLL, GPLL, PPLL */
static const uint32_t cru_plls[] = {
    0x00000, 0x38000, 0x48040, 0x00160, 0x00180, 0x001a0, 0x001c0, 0x08200,
};

static uint32_t cru_read(RkBlock *b, hwaddr off)
{
    uint32_t v = b->regs[off / 4];

    for (int i = 0; i < ARRAY_SIZE(cru_plls); i++) {
        if (off == cru_plls[i] + 0x18) {
            v |= 0x8000;                        /* locked */
        }
    }
    return v;
}

/* a PLL's output (RK3588-type: (24 MHz * (m + k / 65536) / p) >> s in
 * normal mode, the crystal in slow mode); @mode_shift: its MODE_CON0 bits */
static uint64_t cru_pll_hz(RK3576State *s, uint32_t con0, int mode_shift)
{
    uint32_t *r = &s->cru.regs[con0 / 4];
    int mode = (s->cru.regs[0x280 / 4] >> mode_shift) & 3;
    uint64_t m = r[0] & 0x3ff, p = r[1] & 0x3f, k = r[2] & 0xffff;

    if (mode != 1) {
        return mode ? 32768 : 24000000;     /* deep slow, slow */
    }
    if (!p) {
        return 0;
    }
    return ((24000000ULL * m + (24000000ULL * k >> 16)) / p) >>
           ((r[1] >> 6) & 7);
}

/* clk_timer0_root: clk_cpll_div10 (CLKSEL_CON(0): GPLL or CPLL / n) or the
 * crystal (CLKSEL_CON(71) bit 14) */
static uint64_t cru_timer0_hz(RK3576State *s)
{
    uint32_t sel0 = s->cru.regs[0x300 / 4];
    uint64_t pll;

    if (s->cru.regs[0x41c / 4] & (1 << 14)) {
        return 24000000;
    }
    pll = sel0 & (1 << 11) ? cru_pll_hz(s, 0x1a0, 8) : cru_pll_hz(s, 0x1c0, 2);
    return pll / (((sel0 >> 6) & 0x1f) + 1);
}

/* -------------------------------------------------------------- PMU */

/* the power domains: PWR_CON's offset and bit (1: off), and the bit that
 * says it is on in the repair and chain status (the NPU: STATUS, 1: off) */
static const struct {
    uint8_t pwr_off;
    uint32_t pwr, status, on;
} pmu_domains[] = {
    { 0x0, BIT(0),  BIT(0), 0 },            /* npu */
    { 0x0, BIT(6),  0, BIT(6) },            /* nvm */
    { 0x0, BIT(7),  0, BIT(7) },            /* sdgmac */
    { 0x0, BIT(8),  0, BIT(8) },            /* audio */
    { 0x0, BIT(9),  0, BIT(9) },            /* php */
    { 0x0, BIT(10), 0, BIT(10) },           /* subphp */
    { 0x0, BIT(11), 0, BIT(11) },           /* vop */
    { 0x0, BIT(14), 0, BIT(14) },           /* vo1 */
    { 0x0, BIT(15), 0, BIT(15) },           /* vo0 */
    { 0x4, BIT(0),  0, BIT(16) },           /* usb */
    { 0x4, BIT(1),  0, BIT(17) },           /* vi */
    { 0x4, BIT(2),  0, BIT(18) },           /* vepu0 */
    { 0x4, BIT(3),  0, BIT(19) },           /* vepu1 */
    { 0x4, BIT(4),  0, BIT(20) },           /* vdec */
    { 0x4, BIT(5),  0, BIT(21) },           /* vpu */
    { 0x4, BIT(6),  0, BIT(22) },           /* nputop */
    { 0x4, BIT(7),  0, BIT(23) },           /* npu0 */
    { 0x4, BIT(8),  0, BIT(24) },           /* npu1 */
    { 0x4, BIT(9),  0, BIT(25) },           /* gpu */
};

static uint32_t pmu_read(RkBlock *b, hwaddr off)
{
    uint32_t on = 0, off_status = 0;

    for (int i = 0; i < ARRAY_SIZE(pmu_domains); i++) {
        bool is_off = b->regs[(0x210 + pmu_domains[i].pwr_off) / 4] &
                      pmu_domains[i].pwr;

        if (is_off) {
            off_status |= pmu_domains[i].status;
        } else {
            on |= pmu_domains[i].on;
        }
    }
    switch (off) {
    case 0x230: return off_status;                  /* STATUS: 1, off */
    case 0x248: case 0x570: return on;              /* chain, repair */
    case 0x250: return ~on & 0x3ffffc0;             /* memory: 1, off */
    case 0x120: case 0x128:                         /* ACK, IDLE */
        return (b->regs[0x110 / 4] & 0xffff) |
               (b->regs[0x114 / 4] & 0xffff) << 16;
    }
    return b->regs[off / 4];
}

/* -------------------------------------------------------------- RNG */

static void rng_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    if (off == 0x10) {                              /* CTRL (hiword) */
        uint32_t mask = v >> 16;

        b->regs[off / 4] = (old & ~mask) | (v & mask);
        if (v & mask & BIT(4)) {                    /* REQ_TRNG */
            qemu_guest_getrandom_nofail(&b->regs[0x50 / 4], 32);
            b->regs[0x14 / 4] |= BIT(4);            /* TRNG_RDY */
        }
    } else if (off == 0x14) {                       /* STATE: W1C */
        b->regs[off / 4] = old & ~v;
    }
}

/* ------------------------------------------------------------ TIMER0 */

#define TMR_ENABLE      0x1
#define TMR_ONESHOT     0x2                     /* USER_DEFINED_COUNT */
#define TMR_INT_UNMASK  0x4

static uint64_t tmr_load(RkTimer *t)
{
    return (uint64_t)t->load[1] << 32 | t->load[0];
}

static int64_t tmr_period_ns(RkTimer *t)
{
    return muldiv64(tmr_load(t) + 1, NANOSECONDS_PER_SECOND, t->hz);
}

static void tmr_irq(RkTimer *t)
{
    qemu_set_irq(t->irq, (t->status & 1) && (t->ctrl & TMR_INT_UNMASK));
}

static void tmr_expired(void *opaque)
{
    RkTimer *t = opaque;

    t->status |= 1;
    tmr_irq(t);
    if (t->ctrl & TMR_ONESHOT) {
        t->done = true;
    } else {
        t->start_ns += tmr_period_ns(t);        /* reloaded */
        timer_mod(t->timer, t->start_ns + tmr_period_ns(t));
    }
}

/* what is left of the count */
static uint64_t tmr_value(RkTimer *t)
{
    uint64_t gone;

    if (!(t->ctrl & TMR_ENABLE)) {
        return tmr_load(t);
    }
    if (t->done) {
        return 0;
    }
    gone = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns,
                    t->hz, NANOSECONDS_PER_SECOND);
    return gone > tmr_load(t) ? 0 : tmr_load(t) - gone;
}

static uint64_t tmr_read(void *opaque, hwaddr off, unsigned size)
{
    RkTimer *t = opaque;

    switch (off) {
    case 0x00: case 0x04: return t->load[off / 4];
    case 0x08: return tmr_value(t);
    case 0x0c: return tmr_value(t) >> 32;
    case 0x10: return t->ctrl;
    case 0x18: return t->status;
    }
    return 0;
}

static void tmr_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkTimer *t = opaque;

    switch (off) {
    case 0x00: case 0x04:
        t->load[off / 4] = v;
        break;
    case 0x10:
        t->ctrl = v & 7;
        timer_del(t->timer);
        t->done = false;
        if (t->ctrl & TMR_ENABLE) {
            t->hz = cru_timer0_hz(t->soc) ?: 24000000;
            t->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(t->timer, t->start_ns + tmr_period_ns(t));
        }
        tmr_irq(t);
        break;
    case 0x18:
        t->status &= ~v;
        tmr_irq(t);
        break;
    }
}

static const MemoryRegionOps tmr_ops = {
    .read = tmr_read,
    .write = tmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------ GPIO */

static const hwaddr gpio_base[RK3576_NGPIO] = {
    0x27320000, 0x2ae10000, 0x2ae20000, 0x2ae30000, 0x2ae40000,
};
static const int gpio_spi[RK3576_NGPIO] = { 153, 157, 161, 165, 169 };

/* the pins' levels now: the latched edges, the levels as they are, the
 * interrupt */
static void gpio_update(RkGpio *g)
{
    uint32_t level = (g->dr & g->ddr) | (g->in & ~g->ddr);
    uint32_t changed = level ^ g->level;
    uint32_t rose = level & ~g->level, fell = ~level & g->level;
    uint32_t edges = (rose & (g->pol | g->both)) | (fell & (~g->pol | g->both));
    uint32_t levels = ~(level ^ g->pol);        /* at its active level */
    uint32_t edge = g->type | g->both;          /* BOTHEDGE: edges, whatever
                                                   INT_TYPE says */

    g->level = level;
    g->raw = (g->raw & edge) | (edges & edge) | (levels & ~edge);
    g->raw &= g->en;
    qemu_set_irq(g->irq, (g->raw & ~g->mask) != 0);
    for (int pin = 0; changed && g->soc && g->soc->gpio_out; pin++) {
        if (changed & BIT(pin)) {
            changed &= ~BIT(pin);
            g->soc->gpio_out(g->soc->gpio_out_opaque, g->bank, pin,
                             level & BIT(pin));
        }
    }
}

static uint32_t *gpio_reg(RkGpio *g, hwaddr off)
{
    switch (off & ~7) {
    case 0x00: return &g->dr;
    case 0x08: return &g->ddr;
    case 0x10: return &g->en;
    case 0x18: return &g->mask;
    case 0x20: return &g->type;
    case 0x28: return &g->pol;
    case 0x30: return &g->both;
    case 0x38: return &g->debounce;
    case 0x40: return &g->dbclk_en;
    }
    return NULL;
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    RkGpio *g = opaque;
    uint32_t *r = gpio_reg(g, off);

    if (r) {                                    /* a half */
        return off & 4 ? *r >> 16 : *r & 0xffff;
    }
    switch (off) {
    case 0x48: return g->dbclk_con;
    case 0x50: return g->raw & ~g->mask;        /* INT_STATUS */
    case 0x58: return g->raw;                   /* INT_RAWSTATUS */
    case 0x70: return g->level;                 /* EXT_PORT */
    case 0x78: return 0x010219c8;               /* VER_ID: v2 */
    }
    return 0;
}

static void gpio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkGpio *g = opaque;
    uint32_t *r = gpio_reg(g, off);
    int shift = off & 4 ? 16 : 0;
    uint32_t mask = (v >> 16) << shift;

    if (r) {
        *r = (*r & ~mask) | ((v & 0xffff) << shift & mask);
    } else if ((off & ~4) == 0x60) {            /* PORT_EOI: edges done */
        g->raw &= ~((v & 0xffff) << shift & mask);
    } else if (off == 0x48) {
        g->dbclk_con = v;
    }
    gpio_update(g);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void rk3576_gpio_in(RK3576State *s, int bank, int pin, bool level)
{
    RkGpio *g = &s->gpio[bank];

    g->in = deposit32(g->in, pin, 1, level);
    gpio_update(g);
}

/* ------------------------------------------------------------ the SoC */

I2CBus *rk3576_i2c_bus(RK3576State *s, int n)
{
    return s->i2c[n].bus;
}

void rk3576_i2c_target(RK3576State *s, int n, const RkI2cTarget *t)
{
    s->i2c[n].target = t;
}

qemu_irq rk3576_spi(RK3576State *s, int spi)
{
    return qdev_get_gpio_in(DEVICE(&s->gic), spi);
}

static qemu_irq rk3576_ppi(RK3576State *s, int cpu, int id)
{
    return qdev_get_gpio_in(DEVICE(&s->gic),
                            RK3576_GIC_SPIS + cpu * GIC_INTERNAL + id);
}

static void rk3576_init(Object *obj)
{
    RK3576State *s = RK3576(obj);

    for (int c = 0; c < 2; c++) {
        object_initialize_child(obj, c ? "cluster-big" : "cluster-little",
                                &s->cluster[c], TYPE_CPU_CLUSTER);
        /* the RP2350's cores are cluster 0 */
        qdev_prop_set_uint32(DEVICE(&s->cluster[c]), "cluster-id", 1 + c);
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        int c = i / 4;

        object_initialize_child(OBJECT(&s->cluster[c]), "cpu[*]", &s->cpu[i],
                                c ? ARM_CPU_TYPE_NAME("cortex-a72")
                                  : ARM_CPU_TYPE_NAME("cortex-a53"));
    }
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "spi0", &s->spi0, TYPE_RK_SPI);
    object_initialize_child(obj, "sdmmc", &s->sdmmc, TYPE_DW_MSHC);
    for (int n = 0; n < RK3576_NI2C; n++) {
        g_autofree char *name = g_strdup_printf("i2c%d", n);

        object_initialize_child(obj, name, &s->i2c[n], TYPE_RK_I2C);
    }
}

static void rk3576_realize(DeviceState *dev, Error **errp)
{
    RK3576State *s = RK3576(dev);
    SysBusDevice *gic = SYS_BUS_DEVICE(&s->gic);
    MemoryRegion *sysmem = get_system_memory();

    for (int c = 0; c < 2; c++) {
        if (!qdev_realize(DEVICE(&s->cluster[c]), NULL, errp)) {
            return;
        }
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        Object *cpu = OBJECT(&s->cpu[i]);

        object_property_set_int(cpu, "mp-affinity", rk3576_mpidr[i],
                                &error_abort);
        object_property_set_bool(cpu, "has_el3", false, &error_abort);
        object_property_set_bool(cpu, "has_el2", true, &error_abort);
        object_property_set_int(cpu, "reset-cbar", GIC_BASE, &error_abort);
        object_property_set_int(cpu, "core-count", 4, &error_abort);
        object_property_set_uint(cpu, "cntfrq", CNTFRQ_HZ, &error_abort);
        object_property_set_bool(cpu, "start-powered-off",
                                 i || !s->start_on, &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    /* the GIC-400: its CPU interfaces are these eight, from the first */
    qdev_prop_set_uint32(DEVICE(gic), "revision", 2);
    qdev_prop_set_uint32(DEVICE(gic), "num-cpu", RK3576_NCPUS);
    qdev_prop_set_uint32(DEVICE(gic), "first-cpu-index",
                         CPU(&s->cpu[0])->cpu_index);
    qdev_prop_set_uint32(DEVICE(gic), "num-irq",
                         RK3576_GIC_SPIS + GIC_INTERNAL);
    qdev_prop_set_bit(DEVICE(gic), "has-security-extensions", false);
    qdev_prop_set_bit(DEVICE(gic), "has-virtualization-extensions", true);
    if (!sysbus_realize(gic, errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, GIC_DIST,
                                sysbus_mmio_get_region(gic, 0));
    memory_region_add_subregion(sysmem, GIC_CPU,
                                sysbus_mmio_get_region(gic, 1));
    memory_region_add_subregion(sysmem, GIC_VIFACE,
                                sysbus_mmio_get_region(gic, 2));
    memory_region_add_subregion(sysmem, GIC_VCPU,
                                sysbus_mmio_get_region(gic, 3));
    for (int i = 0; i < RK3576_NCPUS; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);

        sysbus_connect_irq(gic, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gic, i + RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gic, i + 2 * RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gic, i + 3 * RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
        sysbus_connect_irq(gic, i + 4 * RK3576_NCPUS,
                           rk3576_ppi(s, i, PPI_MAINTENANCE));
        qdev_connect_gpio_out(cpu, GTIMER_PHYS,
                              rk3576_ppi(s, i, PPI_PHYS_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_VIRT,
                              rk3576_ppi(s, i, PPI_VIRT_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_HYP,
                              rk3576_ppi(s, i, PPI_HYP_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_SEC,
                              rk3576_ppi(s, i, PPI_SEC_TIMER));
    }

    /* the console (serial port 2) and the MCU's line (serial port 3) */
    serial_mm_init(sysmem, UART0_BASE, 2, rk3576_spi(s, UART0_SPI),
                   UART_BAUDBASE, serial_hd(2), DEVICE_LITTLE_ENDIAN);
    serial_mm_init(sysmem, UART4_BASE, 2, rk3576_spi(s, UART4_SPI),
                   UART_BAUDBASE, serial_hd(3), DEVICE_LITTLE_ENDIAN);

    rk_block_init(s, &s->cru, "rk3576.cru", CRU_BASE, CRU_SIZE, true);
    s->cru.read = cru_read;

    rk_block_init(s, &s->pmu, "rk3576.pmu", PMU_BASE, PMU_SIZE, true);
    s->pmu.read = pmu_read;
    rk_block_init(s, &s->rng, "rk3576.rng", RNG_BASE, RNG_SIZE, false);
    s->rng.wrote = rng_wrote;

    s->timer0.soc = s;
    s->timer0.irq = rk3576_spi(s, TIMER0_SPI);
    s->timer0.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tmr_expired,
                                   &s->timer0);
    memory_region_init_io(&s->timer0.mr, OBJECT(s), &tmr_ops, &s->timer0,
                          "rk3576.timer0", 0x20);
    memory_region_add_subregion(sysmem, TIMER0_BASE, &s->timer0.mr);

    for (int b = 0; b < RK3576_NGPIO; b++) {
        RkGpio *g = &s->gpio[b];
        g_autofree char *name = g_strdup_printf("rk3576.gpio%d", b);

        g->soc = s;
        g->bank = b;
        g->irq = rk3576_spi(s, gpio_spi[b]);
        g->in = g->level = 0xffffffff;          /* pulled up */
        memory_region_init_io(&g->mr, OBJECT(s), &gpio_ops, g, name, 0x200);
        memory_region_add_subregion(sysmem, gpio_base[b], &g->mr);
    }

    for (int n = 0; n < RK3576_NI2C; n++) {
        SysBusDevice *i2c = SYS_BUS_DEVICE(&s->i2c[n]);

        if (n == 2 && s->i2c2_bus) {
            object_property_set_link(OBJECT(i2c), "bus",
                                     OBJECT(s->i2c2_bus), &error_abort);
        }
        if (!sysbus_realize(i2c, errp)) {
            return;
        }
        memory_region_add_subregion(sysmem, n ? 0x2ac40000 + (n - 1) * 0x10000
                                              : 0x27300000,
                                    sysbus_mmio_get_region(i2c, 0));
        sysbus_connect_irq(i2c, 0, rk3576_spi(s, 88 + n));
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi0), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, 0x2acf0000,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->spi0),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi0), 0, rk3576_spi(s, 116));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdmmc), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, 0x2a310000,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->sdmmc), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdmmc), 0, rk3576_spi(s, 251));

    for (int d = 0; d < 3; d++) {
        DeviceState *dma = qdev_new("pl330");
        DeviceState *any = qdev_new(TYPE_OR_IRQ);

        object_property_set_link(OBJECT(dma), "memory", OBJECT(sysmem),
                                 &error_abort);
        qdev_prop_set_uint32(dma, "num_chnls", 8);
        qdev_prop_set_uint8(dma, "num_periph_req", 32);
        qdev_prop_set_uint8(dma, "num_events", 16);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        memory_region_add_subregion(sysmem, 0x2ab90000 + d * 0x20000,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(dma), 0));
        memory_region_init_alias(&s->dmac_id[d], OBJECT(s), "rk3576.dmac-id",
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(dma),
                                                        0), 0xfe0, 0x20);
        memory_region_add_subregion(sysmem, 0x2ab90000 + d * 0x20000 + 0x3fe0,
                                    &s->dmac_id[d]);
        qdev_prop_set_uint16(any, "num-lines", 17);
        qdev_realize_and_unref(any, NULL, &error_fatal);
        for (int i = 0; i < 17; i++) {          /* abort, then events */
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), i,
                               qdev_get_gpio_in(any, i));
        }
        qdev_connect_gpio_out(any, 0, rk3576_spi(s, 32 + 2 * d));
    }

    create_unimplemented_device("rk3576", PERIPH_BASE, PERIPH_SIZE);
}

static const Property rk3576_props[] = {
    DEFINE_PROP_BOOL("start-on", RK3576State, start_on, false),
    DEFINE_PROP_LINK("i2c2-bus", RK3576State, i2c2_bus, TYPE_I2C_BUS,
                     I2CBus *),
};

static void rk3576_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3576_realize;
    device_class_set_props(dc, rk3576_props);
    dc->user_creatable = false;
}

static const TypeInfo rk3576_types[] = {
    {
        .name = TYPE_RK3576,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RK3576State),
        .instance_init = rk3576_init,
        .class_init = rk3576_class_init,
    },
};

DEFINE_TYPES(rk3576_types)
