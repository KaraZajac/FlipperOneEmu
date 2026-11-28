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
#include "hw/cpu/cluster.h"
#include "chardev/char-fe.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "fo_usb.h"

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

/* the USB controller (rp2350_usb.c) */
typedef struct RP2Usb {
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t addr, main_ctrl, sie_ctrl, sie_status, buff_status, buff_cpu;
    uint32_t ep_abort, ep_abort_done, ep_stall_arm, nak_poll;
    uint32_t ep_status_stall_nak, muxing, pwr, inte, intf, sof;
    bool connected, suspended, vbus;    /* what the bus and the cable do */
    bool conn_changed, susp_changed, vbus_changed;      /* latched */
    bool nak_int_ep0;
    bool pulled_up;                     /* as the host was last told */
    uint8_t next_buf[32];               /* each endpoint's buffer selector */
    /* each endpoint's buffers done that the CPU has not yet cleared from
     * BUFF_STATUS, oldest first: how many (0 ... 2), and their halves */
    uint8_t done_n[32], done_half[32];
    void (*pullup_fn)(void *opaque, bool on);
    void *pullup_opaque;
} RP2Usb;

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
/* the TICKS block's tick generators (the SDK's tick_gen_num_t) */
enum {
    RP2_TICK_PROC0, RP2_TICK_PROC1, RP2_TICK_TIMER0, RP2_TICK_TIMER1,
    RP2_TICK_WATCHDOG, RP2_TICK_RISCV, RP2_TICKS
};

typedef struct RP2Timer {
    RP2Block regs;                      /* first: its functions get this */
    int index;
    bool ticking;                       /* its tick (TICKS') runs */
    QEMUTimer *alarm[4];
    int64_t offset;                     /* TIME = virtual clock's us + this */
    uint32_t write_hi, latched_hi;
    uint64_t paused_at;
    qemu_irq irq[4];
} RP2Timer;

/* a device of QEMU's own behind the RP2350's aliases (+0x1000 XOR, +0x2000
 * set, +0x3000 clear), which it does not know: read, changed, written */
typedef struct RP2Wrap {
    MemoryRegion mr;
    MemoryRegion *target;
    /* a register the device cannot read (the PL011's ICR), read as 0 here as
     * on the chip: the Pico SDK reads it to clear its bits through XOR */
    hwaddr write_only;
} RP2Wrap;

/* the pins (rp2350_gpio.c): a listener is told of a pin's every change */
#define RP2_PINS 48
typedef void RP2PinFn(void *opaque, int pin, bool level, int64_t when_ns);
typedef struct RP2PinListener {
    RP2PinFn *fn;
    void *opaque;
} RP2PinListener;

/*
 * Pins the board drives over a window of time as it knows ahead
 * (rp2350_pins_wave()): their levels at any moment of it, and when one of
 * them next has a level.  Times are picoseconds of the virtual clock.
 */
typedef struct RP2Wave RP2Wave;
struct RP2Wave {
    uint64_t mask;                      /* the pins */
    int64_t from_ps, to_ps;             /* the window */
    uint64_t rose, fell;                /* pins that went up, down in it */
    /* the pins' levels (in @mask's bits) at @t_ps, from_ps or later: from
     * to_ps on, as the window leaves them */
    uint64_t (*levels)(const RP2Wave *w, int64_t t_ps);
    /* the first moment from @t_ps on that @pin has @level, if it is in the
     * window; INT64_MAX if not */
    int64_t (*until)(const RP2Wave *w, int pin, bool level, int64_t t_ps);
};

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
    bool on_pins;                       /* ... on a WAIT for a pin */
    int64_t next_ps;                    /* when its next cycle is (ps) */
    bool irq_waiting;                   /* an IRQ WAIT set its flag */
    /* its cycle's length, worked out for this CLKDIV and clk_sys period */
    uint32_t period_div;
    uint64_t period_clk;
    int64_t period_ps;
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
    bool irq_level[2];          /* ... as last set */
} RP2Pio;

/* what a core's PPB has beyond QEMU's (rp2350_ppb.c) */
typedef struct RP2Ppb {
    struct RP2350State *soc;
    int core;
    MemoryRegion dwt, actlr, demcr_mr, eppb;
    uint32_t dwt_ctrl, demcr, actlr_value;
    uint64_t base_cycles;
    int64_t base_ns;
    uint32_t comp[16];
    uint32_t eppb_regs[3];
} RP2Ppb;

/* DMA: 16 channels */
typedef struct RP2DmaChan {
    uint32_t read_addr, write_addr;
    uint32_t count;                     /* live: transfers left, MODE on top */
    uint32_t reload;                    /* what TRANS_COUNT was written */
    uint32_t ctrl;
} RP2DmaChan;

typedef struct RP2Dma {
    RP2Block regs;
    AddressSpace as;
    RP2DmaChan ch[16];
    uint32_t intr, inte[4], intf[4], timer[4];
    uint32_t sniff_ctrl, sniff_data;
    uint32_t pending;                   /* triggered, not yet run */
    bool running, kicked;
    QEMUTimer *later;                   /* the rest of a long transfer */
    qemu_irq irq[4];
} RP2Dma;

/* HSTX's cycles on GPIO12-19, to the board: bit n of a cycle is GPIO12+n;
 * @mask: the pins that HSTX has (their function is HSTX) */
typedef void RP2HstxFn(void *opaque, const uint8_t *cycles, int n,
                       uint8_t mask);

/* I2C0, I2C1 (rp2350_i2c.c) */
typedef struct RP2I2c {
    RP2Block regs;
    struct I2CBus *bus;
    qemu_irq irq;
    uint16_t rx[16];                    /* the RX FIFO: a byte, its flags */
    int rx_n, rx_head;
    uint8_t tx[16];                     /* slave mode: bytes for a master */
    int tx_n, tx_head;
    bool active, recv;                  /* a transfer is open; it reads */
    uint8_t addr;                       /* ... to this address */
    bool first;                         /* the next byte in comes first */
    bool addressed;                     /* slave mode: a master's at SAR */
    bool rd_requested;                  /* ... RD_REQ raised for this byte */
    bool master_held;                   /* ... its SCL held: the master waits */
    void (*release)(void *opaque);      /* ... and is told when it's let go */
    void *release_opaque;
    uint32_t raw;                       /* RAW_INTR_STAT's latched bits */
    uint32_t abrt;                      /* TX_ABRT_SOURCE */
} RP2I2c;

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

/* the double-precision coprocessor (rp2350_dcp.c): a sequence's operands */
typedef struct RP2Dcp {
    uint64_t x, y;              /* as IEEE doubles */
    uint32_t status;            /* ADD0's comparison: Z, C, V (bits 30..28) */
    bool nearest;               /* NRDC: to an integer, rounded */
    bool engaged;               /* in a sequence */
} RP2Dcp;

typedef struct RP2350Core {
    struct RP2350State *soc;
    int index;
    RP2Dcp dcp[2];              /* the Secure DCP, the Non-secure one */
} RP2350Core;

struct RP2350State {
    SysBusDevice parent_obj;

    CPUClusterState cluster;    /* ... cluster 0: QEMU's gdb stub has a
                                   process a cluster, and this is its
                                   first (pid 1), attached as gdb comes */
    ARMv7MState cpu[2];         /* the two Cortex-M33s, in it */
    RP2350Core core[2];
    qemu_irq *irqs;             /* a peripheral's interrupt: to both NVICs */
    qemu_irq cpu_irq[2][RP2350_NUM_IRQ];    /* ... their inputs */
    Clock *sysclk;

    MemoryRegion bus;           /* what the cores see, the PPB aside */
    MemoryRegion bus_alias;     /* core 1's way to it: a region has one home */
    MemoryRegion rom;
    MemoryRegion sram;
    MemoryRegion xip_sram;
    MemoryRegion bootram;

    RP2350RCP rcp;

    /* rp2350_sys.c */
    RP2Block resets, rosc, trng, sha256, powman, watchdog, otp, accessctrl;
    RP2Block ticks;
    RP2Block xosc, pll_sys, pll_usb, clocks, psm, bootram_regs, adc;
    uint16_t adc_mv[9];         /* AIN0-7 (GPIO40-47), the temperature sensor */
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
    bool pio_burst;                     /* a burst of pins runs them */
    const RP2Wave *wave;                /* ... or a wave, while they run */

    /* rp2350_ppb.c */
    RP2Ppb ppb[2];

    /* rp2350_dma.c, rp2350_hstx.c, rp2350_pwm.c */
    RP2Dma dma;
    RP2Block hstx_ctrl, hstx_fifo, pwm;
    RP2HstxFn *hstx_fn;
    void *hstx_opaque;
    void (*pwm_fn)(void *opaque);
    void *pwm_opaque;

    /* rp2350_i2c.c */
    RP2I2c i2c[2];

    /* UART0, UART1: QEMU's PL011, behind the aliases */
    DeviceState *uart[2];
    RP2Wrap uart_wrap[2];
    Chardev *uart0_chr;                 /* UART0's line: the board's
                                           (uart_link.c), else serial 1 */

    /* rp2350_sio.c */
    RP2SIO sio;
    MemoryRegion usb_dpram;     /* the USB controller's RAM, 4 KiB */
    RP2Usb usb;
    bool bootsel;               /* the BOOTSEL button: QSPI CS held low,
                                   through the first start only (then let
                                   go, as a hand does) */
    bool started;               /* ... it has been */
    bool unpowered;             /* no supply: the cores stay off through
                                   its resets */
    bool watchdog_fired;        /* the reset to come is the watchdog's */
    uint32_t watchdog_reason;   /* ... TIMER (1) or FORCE (2) */
    QEMUTimer *watchdog_timer;  /* its count running down */
    int64_t watchdog_due;       /* ... to 0 then (ns) */
    uint32_t watchdog_left;     /* ... where it stands while it does not */
    bool tick_running[RP2_TICKS];   /* TICKS' generators that run */
    bool core1_held;            /* PSM FRCE_OFF.PROC1: core 1 held off */
    bool core1_cycled;          /* ... since it last started: restart it */
    bool core1_work;            /* its power work queued, not yet run */

    char *bootrom;              /* the boot ROM's image, 32 KiB */
    char *flash;                /* the flash's image, up to 16 MiB */
    char *otp_path;             /* the OTP's image: its 4096 rows, raw */
    int otp_fd;                 /* ... written back (-1: not) */
    uint32_t otp_rows[4096];    /* ... each 24 bits: data, ECC, BRP */
    MemoryRegion otp_data;      /* ... its four read windows */
    uint8_t otp_dap[64];        /* ... its SBPI targets' registers: the */
    uint8_t otp_pmc[64];        /*     DAP's, the PMC's (programming) */
};

/* rp2350_coproc.c */
void rp2350_coproc_init(RP2350State *s, int core);
void rp2350_coproc_reset(RP2350State *s);
/* rp2350_dcp.c: CP4's and CP5's instructions, and their reset */
struct ARMMCoprocInsn;
void rp2350_dcp(void *opaque, ARMCPU *cpu, struct ARMMCoprocInsn *in);
void rp2350_dcp_reset(RP2350State *s);

/* rp2350_sys.c */
void rp2_block_init(RP2350State *s, RP2Block *b, const char *name,
                    hwaddr base, RP2ReadFn *read, RP2WriteFn *write);
void rp2350_sys_realize(RP2350State *s);
void rp2350_sys_reset(RP2350State *s);

/* rp2350_qmi.c */
bool rp2350_qmi_realize(RP2350State *s, Error **errp);
bool rp2350_otp_realize(RP2350State *s, Error **errp);
void rp2350_qmi_reset(RP2350State *s);

/* rp2350_sio.c */
int rp2350_current_core(RP2350State *s);
uint32_t rp2350_sio_in(RP2350State *s, int hi);
void rp2350_sio_realize(RP2350State *s);
void rp2350_sio_reset(RP2350State *s);

/* rp2350_timer.c */
void rp2350_timer_realize(RP2350State *s);
void rp2350_timer_reset(RP2350State *s);
void rp2350_timer_tick(RP2350State *s, int i, bool running);

/* rp2350_gpio.c */
void rp2350_gpio_realize(RP2350State *s);
void rp2350_gpio_reset(RP2350State *s);
/* work out every pin again after what drives them changed (at @when_ns) */
void rp2350_pins_update(RP2350State *s, int64_t when_ns);
/* the board drives @pin: 0 low, 1 high, -1 lets go */
void rp2350_pin_drive(RP2350State *s, int pin, int level);
/*
 * ... or drives @mask's pins to @levels at @when_ns, which may be ahead of
 * the virtual clock: a fast bus clocked out in bursts (host_spi.c).  The PIO
 * runs up to that moment with the levels as they were, then sees the change
 * at it, so a state machine samples what it would have on the chip.  Its
 * times must not go back: a burst starts at the virtual clock or later.
 */
void rp2350_pins_drive_at(RP2350State *s, uint64_t mask, uint64_t levels,
                          int64_t when_ns);
/*
 * ... or drives @w's pins over its window as it says, which the PIO sees as
 * it would see each change through rp2350_pins_drive_at(), but without a
 * call for each: a state machine that waits on one of them goes on at the
 * moment it comes.  The pins are left as the window leaves them, driven.
 * Only for pins nothing but the PIO looks at meanwhile (no listener, no
 * interrupt, nothing on the chip driving them): false, and nothing done,
 * otherwise.  The window starts at the virtual clock or later, and after
 * any change made before it (rp2350_pins_drive_at() makes one that is
 * behind the clock at its present): a machine already past the window's
 * start misses what the wave did before its time.
 */
bool rp2350_pins_wave(RP2350State *s, const RP2Wave *w);
void rp2350_pin_listen(RP2350State *s, int pin, RP2PinFn *fn, void *opaque);
/* the level @pin has from the chip and the pulls alone, whatever the board
 * drives: what an open-drain master on it lets the line do */
int rp2350_pin_own(RP2350State *s, int pin);
/* decode @pin's changes as a UART at @baud, to @chr */
void rp2350_pin_uart(RP2350State *s, int pin, uint32_t baud,
                     CharFrontend *chr);

/* rp2350_dma.c */
void rp2350_dma_realize(RP2350State *s);
void rp2350_dma_reset(RP2350State *s);
/* a DREQ may have come: busy channels go on */
void rp2350_dma_kick(RP2350State *s);

/* rp2350_hstx.c */
void rp2350_hstx_realize(RP2350State *s);
void rp2350_hstx_reset(RP2350State *s);
void rp2350_hstx_attach(RP2350State *s, RP2HstxFn *fn, void *opaque);

/* rp2350_i2c.c: the board puts its chips on rp2350_i2c_bus(s, 0 or 1) */
void rp2350_i2c_realize(RP2350State *s);
void rp2350_i2c_reset(RP2350State *s);
struct I2CBus *rp2350_i2c_bus(RP2350State *s, int n);
/*
 * ... and a master that reaches I2Cn in slave mode (the RK3576 on I2C1), a
 * step at a time. start: whether SAR answers @addr. write: 1 the byte is
 * taken, 0 not yet (the RX FIFO is full, SCL held: try again). read: 1 a
 * byte, 0 not yet (the CPU has not given one: SCL held). nack: the master
 * read its last byte.
 */
bool rp2350_i2c_target_start(RP2350State *s, int n, uint8_t addr, bool read);
int rp2350_i2c_target_write(RP2350State *s, int n, uint8_t byte);
int rp2350_i2c_target_read(RP2350State *s, int n, uint8_t *byte);
void rp2350_i2c_target_nack(RP2350State *s, int n);
void rp2350_i2c_target_stop(RP2350State *s, int n);
/* @release is called when I2C@n lets go of the SCL it held on its master
 * (a write or a read returned 0): the master goes on then */
void rp2350_i2c_target_on_release(RP2350State *s, int n,
                                  void (*release)(void *opaque), void *opaque);

/*
 * rp2350_usb.c: the USB controller as a device; and for a host on the
 * other end of the cable: whether the device pulls D+ up (pullup_fn tells
 * when it changes), VBUS, a bus reset, suspend and resume, a detach; a
 * SETUP, OUT or IN transaction to an endpoint at an address (IN: up to 64
 * bytes into @data, *@len of them)
 */
void rp2350_usb_realize(RP2350State *s);
void rp2350_usb_reset(RP2350State *s);
bool rp2350_usb_pulled_up(RP2350State *s);
/* its interrupt is up: the CPU has not yet dealt with what came */
bool rp2350_usb_irq_pending(RP2350State *s);
void rp2350_usb_vbus(RP2350State *s, bool vbus);
void rp2350_usb_bus_reset(RP2350State *s);
void rp2350_usb_suspend(RP2350State *s, bool suspended);
void rp2350_usb_detach(RP2350State *s);
FoUsbResult rp2350_usb_setup(RP2350State *s, uint8_t addr,
                             const uint8_t pkt[8]);
FoUsbResult rp2350_usb_out(RP2350State *s, uint8_t addr, int ep,
                           const uint8_t *data, int len);
FoUsbResult rp2350_usb_in(RP2350State *s, uint8_t addr, int ep,
                          uint8_t *data, int *len);
/* ... all of it as the computer's side drives a device (fo_usb.h), with
 * the RP2350State as its opaque */
extern const FoUsbDevOps rp2350_usb_ops;

/* rp2350_pwm.c: a pin's duty, 0 to 65535 (-1: the pin is not PWM's) */
void rp2350_pwm_realize(RP2350State *s);
void rp2350_pwm_reset(RP2350State *s);
int rp2350_pwm_duty(RP2350State *s, int pin);

/* rp2350_ppb.c */
void rp2350_ppb_realize(RP2350State *s, int core);
void rp2350_ppb_reset(RP2350State *s);

/* rp2350_pio.c */
void rp2350_pio_realize(RP2350State *s);
void rp2350_pio_reset(RP2350State *s);
/* a pin's input changed: state machines that wait on pins look again */
void rp2350_pio_kick(RP2350State *s);
/* the state machines run up to @when_ns (ahead of the clock, it may be);
 * @pins: a pin changed then, and those that wait on pins look again */
void rp2350_pio_run_to(RP2350State *s, int64_t when_ns, bool pins);
/* the state machines run through @w's window (rp2350_pins_wave()) */
void rp2350_pio_wave(RP2350State *s, const RP2Wave *w);
/* a burst of rp2350_pins_drive_at() begins (its changes run the machines;
 * their timers wait), or ends */
void rp2350_pio_burst(RP2350State *s, bool on);

/* the RP2350's interrupt @n, which both cores' NVICs get */
qemu_irq rp2350_irq(RP2350State *s, int n);
/* core 1 held off (PSM FRCE_OFF.PROC1) or let go: it starts in its ROM */
void rp2350_core1_power(RP2350State *s, bool on);

#endif
