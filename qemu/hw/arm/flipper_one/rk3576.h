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
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
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

/* rk3x_i2c.c: a Rockchip I2C controller */
#define TYPE_RK_I2C "rk3x-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(RkI2cState, RK_I2C)

/*
 * What a controller's bus reaches, a step at a time, if not QEMU I2C
 * devices: each step 1 done, 0 not yet (the target holds SCL: the step is
 * tried again), -1 not acknowledged
 */
typedef struct RkI2cTarget {
    int (*start)(void *opaque, uint8_t addr, bool read);
    int (*write)(void *opaque, uint8_t byte);
    int (*read)(void *opaque, uint8_t *byte);
    void (*nack)(void *opaque);
    void (*stop)(void *opaque);
    void *opaque;
} RkI2cTarget;

enum { RKI2C_START, RKI2C_WRITE, RKI2C_READ };

typedef struct RkI2cStep {
    uint8_t op, byte;
    bool last;                          /* a read: the last, not ACKed */
} RkI2cStep;

struct RkI2cState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    I2CBus *bus;                        /* its own, or ... */
    I2CBus *shared_bus;                 /* ... this one, given */
    uint32_t con, clkdiv, mrxaddr, mrxraddr, mtxcnt, mrxcnt, ien, ipd, fcnt;
    uint32_t txbuf[8], rxbuf[8];
    bool active;                        /* a transfer is under way */
    bool need_addr;                     /* ... a START waits for its target */
    const RkI2cTarget *target;          /* NULL: QEMU's I2C devices */
    bool (*arb)(void *opaque, bool claim);  /* another master on the bus */
    void *arb_opaque;
    bool claimed;                       /* ... this one has it */
    bool busy;                          /* ... it has it: try again then */
    RkI2cStep steps[40];                /* the count under way */
    int nsteps, step, nread;
    bool reading;
    QEMUTimer *timer;                   /* ... a step the target held */
};

/* @t is what I2C@n's bus reaches */
void rk3576_i2c_target(RK3576State *s, int n, const RkI2cTarget *t);
/* I2C@n's bus has another master: @claim takes the bus before a START
 * (false: the other has a transfer under way) and lets it go after the
 * STOP */
void rk3576_i2c_arbiter(RK3576State *s, int n,
                        bool (*claim)(void *opaque, bool claim),
                        void *opaque);

/* a GPIO bank (Rockchip's v2: 32 pins, each register split in halves) */
typedef struct RkGpio {
    MemoryRegion mr;
    struct RK3576State *soc;
    int bank;
    qemu_irq irq;
    uint32_t dr, ddr;                   /* output levels, outputs */
    uint32_t en, mask, type, pol, both; /* interrupts: on, masked, edge,
                                           high or rising, either edge */
    uint32_t raw;                       /* ... pending (levels as they are,
                                           edges until EOI) */
    uint32_t in;                        /* the levels from outside */
    uint32_t level;                     /* the pins' levels, as last seen */
    uint32_t debounce, dbclk_en, dbclk_con;
} RkGpio;

/* rk_spi.c: a Rockchip SPI controller, as a host */
#define TYPE_RK_SPI "rockchip-spi"
OBJECT_DECLARE_SIMPLE_TYPE(RkSpiState, RK_SPI)
#define RKSPI_FIFO          64

struct RkSpiState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t ctrlr0, ctrlr1, ssienr, ser, baudr, txftlr, rxftlr, imr;
    uint32_t dmacr, dmatdlr, dmardlr;
    uint16_t rx[RKSPI_FIFO];            /* what came in (nothing drives
                                           MISO: 0s) */
    int rx_n, rx_head;
    /* the board gets what goes out, with SER's chip selects */
    void (*out)(void *opaque, int cs, const uint8_t *data, int len);
    void *out_opaque;
};

void rk_spi_set_out(RkSpiState *s,
                    void (*out)(void *opaque, int cs, const uint8_t *data,
                                int len), void *opaque);

/* dw_mshc.c: a DesignWare SD host, with an SD bus for a card */
#define TYPE_DW_MSHC "dw-mshc"
OBJECT_DECLARE_SIMPLE_TYPE(DwMshcState, DW_MSHC)

struct DwMshcState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    SDBus sdbus;
    uint32_t ctrl, pwren, clkdiv, clksrc, clkena, tmout, ctype, blksiz;
    uint32_t bytcnt, intmask, cmdarg, cmd, resp[4], rintsts, fifoth, usrid;
    uint32_t uhs, bmod, dbaddr, idsts, idinten, dscaddr, bufaddr;
    uint32_t tcbcnt, tbbcnt;
    uint32_t other[128];                /* the rest, kept (timing, phases) */
    uint8_t fifo[4096];                 /* the driver's few bytes */
    uint32_t fifo_len, fifo_pos;
    uint32_t writing;                   /* ... a write's, to come */
};

SDBus *dw_mshc_bus(DwMshcState *s);

/*
 * rk3576_scmi.c: the SCMI server that TF-A is (the CPU and GPU clocks), on
 * SMC 0x82000010, its shared memory in DRAM's first 2 MiB, which are
 * TF-A's: the kernel's memory starts after them
 */
#define RK3576_SCMI_SMC         0x82000010u
#define RK3576_SCMI_SHMEM       0x4010f000
#define RK3576_TFA_SIZE         0x200000
typedef struct RkScmi {
    uint64_t clk_hz[3];
    bool clk_on[3];
} RkScmi;
/* at the RK3576's start: the channel free, the clocks as TF-A leaves them */
void rk_scmi_reset(RkScmi *s);
/* SMC 0x82000010: the message in the shared memory, answered there */
void rk_scmi_call(RkScmi *s);

/*
 * rk3576_tsadc.c: the temperature sensor (six channels: the SoC, the big
 * cores, the little cores, DDR, the NPU, the GPU), and how warm the chip
 * is, from the cores that run
 */
#define RK3576_TSADC_CHANNELS   6
typedef struct RkTsadc {
    MemoryRegion mr;
    struct RK3576State *soc;
    qemu_irq irq;
    QEMUTimer *timer;                   /* the samples, the chip's warmth */
    uint32_t regs[0x400 / 4];
    double ambient;                     /* °C, around it */
    double temp[RK3576_TSADC_CHANNELS]; /* °C, at each sensor */
    int64_t last_ns;                    /* ... as of then */
} RkTsadc;
void rk_tsadc_init(struct RK3576State *s, RkTsadc *t, hwaddr base,
                   qemu_irq irq);
/* its registers as at power-on (the chip's warmth stays) */
void rk_tsadc_reset(RkTsadc *t);
/* the sensors' trims, into the OTP's non-secure part */
void rk_tsadc_otp_trims(uint8_t *otp);

#define RK3576_NCPUS        8
#define RK3576_NI2C         9           /* I2C0 ... I2C8 */
#define RK3576_NGPIO        5           /* GPIO0 ... GPIO4 */           /* 4 Cortex-A53, then 4 Cortex-A72 */
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
    RkI2cState i2c[RK3576_NI2C];
    RkGpio gpio[RK3576_NGPIO];
    RkSpiState spi0;                    /* the display link */
    DwMshcState sdmmc;                  /* the microSD slot */
    RkScmi scmi;                        /* TF-A's SCMI server */
    RkBlock otp;                        /* the OTP's controller */
    uint8_t otp_ns[0x100];              /* ... the part it reads */
    RkTsadc tsadc;                      /* the temperature sensor */
    DeviceState *uart[2];               /* UART0, UART4 */
    DeviceState *dmac[3];
    MemoryRegion dmac_id[3];            /* the PL330s' IDs, where AMBA
                                           looks: their regions' ends */
    /* the board is told of a GPIO pin's new level */
    void (*gpio_out)(void *opaque, int bank, int pin, bool level);
    void *gpio_out_opaque;
    /* the CRU resets the chip (the TSADC's TSHUT): the board's to do, as
     * PSCI's SYSTEM_RESET is */
    void (*cru_reset)(void *opaque);
    void *cru_reset_opaque;
    I2CBus *i2c2_bus;                   /* I2C2's bus, if it is shared */
    bool start_on;                      /* CPU 0 runs from the start (a
                                           kernel given), else all are off */
};

/* an SPI of the GIC, as the device tree numbers them */
qemu_irq rk3576_spi(RK3576State *s, int spi);
/* its devices as at power-on (not the cores, which are the board's to
 * start) */
void rk3576_reset_devices(RK3576State *s);
/* the bus of I2C@n, for what the board has on it */
I2CBus *rk3576_i2c_bus(RK3576State *s, int n);
/* a level from outside on GPIO@bank's @pin (0 ... 31: A0 ... D7) */
void rk3576_gpio_in(RK3576State *s, int bank, int pin, bool level);

#endif
