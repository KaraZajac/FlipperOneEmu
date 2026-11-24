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
#include "chardev/char-fe.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
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
#define RP2_REGS 1024                   /* 4 KiB of registers */
typedef struct RP2Block RP2Block;
/* a register's value (offset @reg): NULL reads regs[] */
typedef uint32_t RP2ReadFn(RP2Block *b, hwaddr reg);
/* a write, after the alias: NULL stores it in regs[] */
typedef void RP2WriteFn(RP2Block *b, hwaddr reg, uint32_t value);
/* a plain write as it came, size and all: true if it was taken */
typedef bool RP2RawFn(RP2Block *b, hwaddr addr, uint32_t value,
                      unsigned size);
struct RP2Block {
    MemoryRegion mr;
    struct RP2350State *soc;
    uint32_t regs[RP2_REGS];
    RP2ReadFn *read;
    RP2WriteFn *write;
    RP2RawFn *raw;
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

/* the QSPI NOR flash on QMI's CS0 (a 16 MiB part, as a W25Q128JV answers) */
typedef struct RP2Flash {
    uint8_t *mem;
    int fd;                             /* the image, written back; or -1 */
    uint32_t dirty_lo, dirty_hi;        /* programmed, to write back */
    uint8_t cmd, mode;
    uint8_t cont_cmd;                   /* the read of continuous mode */
    int phase;
    uint32_t addr;
    int got;                            /* address bytes so far */
    int dummy;                          /* dummy clocks still to come */
    int count;                          /* data bytes so far */
    uint8_t sr[3];                      /* status registers 1-3 */
    bool continuous;                    /* EBh/BBh read without a command */
    bool selected;
} RP2Flash;

/* QMI: direct mode, and the XIP windows of CS0 through ATRANS0-3 */
#define RP2_QMI_RX 16
typedef struct RP2QMI {
    RP2Block regs;
    uint32_t tx[RP2_QMI_RX];            /* waits for direct mode's EN */
    int tx_n, tx_head;
    uint16_t rx[RP2_QMI_RX];
    int rx_n, rx_head;
    bool auto_open;                     /* a transfer with AUTO_CS0N, open */
    RP2Flash flash;
    MemoryRegion mem;                   /* the flash's array */
    MemoryRegion win[2][4];             /* cached, uncached; 4 MiB each */
    MemoryRegion untranslated;
} RP2QMI;

/* TIMER0 and TIMER1: microseconds of the virtual clock, four alarms each */
typedef struct RP2Timer {
    RP2Block regs;                      /* first: its functions get this */
    int index;
    QEMUTimer *alarm[4];
    int64_t offset;                     /* TIME = virtual clock's us + this */
    uint32_t write_hi, latched_hi;
    uint64_t paused_at;
    qemu_irq irq[4];
} RP2Timer;

/* the pins (rp2350_gpio.c): a listener is told of a pin's every change */
#define RP2_PINS 48
typedef void RP2PinFn(void *opaque, int pin, bool level, int64_t when_ns);
typedef struct RP2PinListener {
    RP2PinFn *fn;
    void *opaque;
} RP2PinListener;

/* a UART's bytes from a pin's changes (the firmware's log, on GPIO41) */
typedef struct RP2PinUart {
    struct RP2350State *soc;
    int pin;
    uint32_t baud;
    CharFrontend *chr;
    bool level, busy;
    int64_t last_ns, start_ns;
    int bit;
    uint16_t bits;
    QEMUTimer *done;
} RP2PinUart;

/* PIO: four state machines on 32 instructions */
typedef struct RP2PioSM {
    uint32_t clkdiv, execctrl, shiftctrl, pinctrl;
    uint8_t pc;
    uint32_t x, y, isr, osr;
    uint8_t isr_count, osr_count;       /* bits shifted in; bits shifted out */
    uint32_t txf[8], rxf[8];
    int tx_n, tx_head, rx_n, rx_head;
    uint16_t exec;                      /* an instruction to run next ... */
    bool has_exec;                      /* ... (INSTR, OUT/MOV EXEC) */
    bool stalled;
    int64_t next_ps;                    /* when its next cycle is (ps) */
    bool irq_waiting;                   /* an IRQ WAIT set its flag */
} RP2PioSM;

typedef struct RP2Pio {
    RP2Block regs;                      /* first: its functions get this */
    int index;
    uint16_t imem[32];
    RP2PioSM sm[4];
    uint32_t ctrl;                      /* SM_ENABLE */
    uint8_t irq;                        /* its eight IRQ flags */
    uint32_t pad_out, pad_oe;           /* what it drives, pin 0 at GPIOBASE */
    uint32_t gpiobase;
    uint32_t inte[2], intf[2];
    uint32_t fdebug;
    QEMUTimer *timer;
    bool running;
    qemu_irq irq_line[2];
} RP2Pio;

/* SHA-256 accelerator */
typedef struct RP2SHA {
    uint32_t h[8];
    uint32_t w[16];
    int n;                              /* words of this block so far */
    uint32_t acc;                       /* bytes of a word so far, LSB first */
    int nbytes;
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

    ARMv7MState cpu[2];         /* the two Cortex-M33s */
    RP2350Core core[2];
    qemu_irq *irqs;             /* a peripheral's interrupt: to both NVICs */
    Clock *sysclk;

    MemoryRegion bus;           /* what the cores see, the PPB aside */
    MemoryRegion bus_alias;     /* core 1's way to it: a region has one home */
    MemoryRegion rom;
    MemoryRegion sram;
    MemoryRegion xip_sram;
    MemoryRegion bootram;

    RP2350RCP rcp;

    /* rp2350_sys.c */
    RP2Block resets, rosc, trng, sha256, powman, watchdog;
    RP2Block xosc, pll_sys, pll_usb, clocks, psm, bootram_regs;
    RP2SHA sha;

    /* rp2350_qmi.c */
    RP2QMI qmi;

    /* rp2350_timer.c; rp2350_sys.c keeps the pads' blocks as registers */
    RP2Timer timer[2];
    RP2Block io_bank0, pads_bank0, io_qspi, pads_qspi;

    /* rp2350_gpio.c: the pins as they are, and who drives them from outside */
    uint64_t pin_level;
    uint64_t ext_drive, ext_level;      /* the board: pins it drives, to what */
    uint64_t ext_pullup;                /* ... and pulls up (I2C's resistors) */
    uint32_t gpio_edges[6];             /* INTR's latched edges, 8 pins each */
    RP2PinListener pin_listener[RP2_PINS][2];
    RP2PinUart log_uart;

    /* rp2350_pio.c */
    RP2Pio pio[3];

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

/* rp2350_qmi.c */
bool rp2350_qmi_realize(RP2350State *s, Error **errp);
void rp2350_qmi_reset(RP2350State *s);

/* rp2350_sio.c */
int rp2350_current_core(RP2350State *s);
uint32_t rp2350_sio_in(RP2350State *s, int hi);
void rp2350_sio_realize(RP2350State *s);
void rp2350_sio_reset(RP2350State *s);

/* rp2350_timer.c */
void rp2350_timer_realize(RP2350State *s);
void rp2350_timer_reset(RP2350State *s);

/* rp2350_gpio.c */
void rp2350_gpio_realize(RP2350State *s);
void rp2350_gpio_reset(RP2350State *s);
/* work out every pin again after what drives them changed (at @when_ns) */
void rp2350_pins_update(RP2350State *s, int64_t when_ns);
/* the board drives @pin: 0 low, 1 high, -1 lets go */
void rp2350_pin_drive(RP2350State *s, int pin, int level);
void rp2350_pin_listen(RP2350State *s, int pin, RP2PinFn *fn, void *opaque);
/* decode @pin's changes as a UART at @baud, to @chr */
void rp2350_pin_uart(RP2350State *s, int pin, uint32_t baud,
                     CharFrontend *chr);

/* rp2350_pio.c */
void rp2350_pio_realize(RP2350State *s);
void rp2350_pio_reset(RP2350State *s);
/* a pin's input changed: state machines that wait on pins look again */
void rp2350_pio_kick(RP2350State *s);

/* the RP2350's interrupt @n, which both cores' NVICs get */
qemu_irq rp2350_irq(RP2350State *s, int n);
/* core 1 held off (PSM FRCE_OFF.PROC1) or let go: it starts in its ROM */
void rp2350_core1_power(RP2350State *s, bool on);

#endif
