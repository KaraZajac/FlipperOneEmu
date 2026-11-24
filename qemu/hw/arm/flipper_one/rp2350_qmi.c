/*
 * The RP2350's QMI (QSPI memory interface) and the flash on its CS0
 *
 * XIP: CS0's 16 MiB of virtual addresses (0x10000000 cached, 0x14000000
 * uncached) are four 4 MiB windows that ATRANS0-3 map onto the flash
 * (BASE and SIZE in 4 KiB sectors; past SIZE is a bus error), as the boot
 * ROM sets them to run a partition in place.  0x1c000000 is the flash
 * untranslated.  A read is the flash's bytes; QMI's read formats (M0_RFMT,
 * M0_RCMD) are kept, not acted out.  CS1 has nothing on it.
 *
 * Direct mode: each TX entry is 8 or 16 bits on 1, 2 or 4 lanes; the flash
 * takes the bytes the host drives and gives back what it drives itself,
 * which goes to the RX FIFO unless NOPUSH.  CS0 is asserted by ASSERT_CS0N,
 * or by AUTO_CS0N for as long as the host keeps writing TX (until it next
 * looks at the CSR or RX).
 *
 * The flash answers as a Winbond W25Q128JV does, byte by byte: reads (03h,
 * 0Bh, 3Bh, 6Bh, BBh, EBh with its continuous mode), SFDP, JEDEC and
 * unique IDs, status registers, write enable, page program, 4/32/64 KiB
 * and chip erase (done at once: never busy).  Programs and erases are
 * written back to the image file.  QPI is not modelled: a command on
 * more than one lane is ignored (the boot ROM's F5h/FFh "QPI exit").
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "rp2350.h"

#define CSR_EN              0x1
#define CSR_ASSERT_CS0N     0x4
#define CSR_ASSERT_CS1N     0x8
#define CSR_AUTO_CS0N       0x40
#define CSR_AUTO_CS1N       0x80
#define CSR_TXEMPTY         0x800
#define CSR_RXEMPTY         0x10000
#define CSR_RXFULL          0x20000
#define CSR_KEEP            0xffc000cfu         /* RXDELAY, CLKDIV, CS bits */
#define TX_NOPUSH           0x100000
#define TX_DWIDTH           0x40000

#define REG_ATRANS0         (0x34 / 4)

enum { F_CMD, F_ADDR, F_MODE, F_DUMMY, F_DATA, F_IGNORE };

static const uint8_t flash_jedec[3] = { 0xef, 0x40, 0x18 };

/* SFDP: the header and the JEDEC basic flash parameters of a 128 Mbit part */
static const uint8_t flash_sfdp[0xa4] = {
    [0x00] = 'S', 'F', 'D', 'P', 0x06, 0x01, 0x00, 0xff,
    [0x08] = 0x00, 0x06, 0x01, 0x09, 0x80, 0x00, 0x00, 0xff,
    [0x80] = 0xe5, 0x20, 0xf9, 0xff,            /* 4 KiB erase 20h, fast reads */
    [0x84] = 0xff, 0xff, 0xff, 0x07,            /* density: 128 Mbit */
    [0x88] = 0x44, 0xeb, 0x08, 0x6b,            /* 1-4-4 EBh, 1-1-4 6Bh */
    [0x8c] = 0x08, 0x3b, 0x42, 0xbb,            /* 1-1-2 3Bh, 1-2-2 BBh */
    [0x90] = 0xfe, 0xff, 0xff, 0xff,
    [0x94] = 0xff, 0xff, 0x00, 0x00,
    [0x98] = 0xff, 0xff, 0x00, 0x00,
    [0x9c] = 0x0c, 0x20, 0x0f, 0x52,            /* 4 KiB 20h, 32 KiB 52h */
    [0xa0] = 0x10, 0xd8, 0x00, 0x00,            /* 64 KiB D8h */
};

static void flash_sync(RP2350State *s, uint32_t off, uint32_t len)
{
    RP2Flash *f = &s->qmi.flash;

    memory_region_flush_rom_device(&s->qmi.mem, off, len);
    if (f->fd >= 0 &&
        pwrite(f->fd, f->mem + off, len, off) != (ssize_t)len) {
        qemu_log_mask(LOG_GUEST_ERROR, "rp2350: flash: could not write the "
                      "image back at 0x%06x\n", off);
    }
}

static void flash_command(RP2350State *s, uint8_t cmd);

static void flash_select(RP2350State *s)
{
    RP2Flash *f = &s->qmi.flash;

    f->selected = true;
    f->got = f->count = 0;
    f->addr = 0;
    f->dirty_lo = UINT32_MAX;
    f->dirty_hi = 0;
    if (f->continuous) {                /* EBh/BBh again, without the command */
        flash_command(s, f->cont_cmd);
    } else {
        f->phase = F_CMD;
        f->cmd = 0;
    }
}

static void flash_deselect(RP2350State *s)
{
    RP2Flash *f = &s->qmi.flash;
    bool wel = f->sr[0] & 0x02;
    uint32_t size = 0;

    if (!f->selected) {
        return;
    }
    f->selected = false;
    if (f->continuous && f->phase != F_DATA) {
        f->continuous = false;          /* a broken sequence ends the mode */
    }
    if (f->dirty_lo < f->dirty_hi) {    /* what a page program changed */
        flash_sync(s, f->dirty_lo, f->dirty_hi - f->dirty_lo);
    }
    switch (f->cmd) {
    case 0x20:
        size = 4 * KiB;
        break;
    case 0x52:
        size = 32 * KiB;
        break;
    case 0xd8:
        size = 64 * KiB;
        break;
    case 0x60: case 0xc7:
        if (wel) {
            memset(f->mem, 0xff, RP2350_FLASH_SIZE);
            flash_sync(s, 0, RP2350_FLASH_SIZE);
        }
        break;
    }
    if (size && wel && f->got == 3) {
        uint32_t at = f->addr & (RP2350_FLASH_SIZE - 1) & ~(size - 1);

        memset(f->mem + at, 0xff, size);
        flash_sync(s, at, size);
    }
    switch (f->cmd) {                   /* these end a write enable */
    case 0x01: case 0x31: case 0x11: case 0x02: case 0x32:
    case 0x20: case 0x52: case 0xd8: case 0x60: case 0xc7:
        f->sr[0] &= ~0x02;
        break;
    }
    f->cmd = 0;
}

/* a read command's address, mode and dummy phases */
static void flash_read_cmd(RP2Flash *f, int dummy)
{
    f->phase = F_ADDR;
    f->dummy = dummy;
}

static void flash_command(RP2350State *s, uint8_t cmd)
{
    RP2Flash *f = &s->qmi.flash;

    f->cmd = cmd;
    f->phase = F_DATA;
    switch (cmd) {
    case 0x03:                                      /* read */
    case 0x02: case 0x32:                           /* page program */
    case 0x20: case 0x52: case 0xd8:                /* erase */
    case 0x90:                                      /* manufacturer/device */
        flash_read_cmd(f, 0);
        break;
    case 0x0b: case 0x3b: case 0x6b:                /* fast reads */
    case 0x5a:                                      /* SFDP */
        flash_read_cmd(f, 8);
        break;
    case 0xbb:                                      /* dual I/O: mode byte */
        flash_read_cmd(f, 0);
        break;
    case 0xeb:                                      /* quad I/O: mode, 4 */
        flash_read_cmd(f, 4);
        break;
    case 0xab:                                      /* 3 dummy bytes, ID */
        f->phase = F_DUMMY;
        f->dummy = 24;
        break;
    case 0x4b:                                      /* 4 dummy bytes, UID */
        f->phase = F_DUMMY;
        f->dummy = 32;
        break;
    case 0x06:
        f->sr[0] |= 0x02;
        break;
    case 0x04:
        f->sr[0] &= ~0x02;
        break;
    case 0x9f: case 0x05: case 0x35: case 0x15:
    case 0x01: case 0x31: case 0x11: case 0x50:
    case 0x60: case 0xc7:
    case 0x66: case 0x99: case 0xb9: case 0xff: case 0xa3:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rp2350: flash: command %02xh is not "
                      "modelled\n", cmd);
        f->phase = F_IGNORE;
    }
}

/* the data phase: what the flash drives, or what it takes */
static uint8_t flash_data(RP2350State *s, uint8_t b)
{
    RP2Flash *f = &s->qmi.flash;
    int i = f->count++;
    uint32_t a;

    switch (f->cmd) {
    case 0x03: case 0x0b: case 0x3b: case 0x6b: case 0xbb: case 0xeb:
        return f->mem[(f->addr + i) & (RP2350_FLASH_SIZE - 1)];
    case 0x5a:
        a = f->addr + i;
        return a < sizeof(flash_sfdp) ? flash_sfdp[a] : 0xff;
    case 0x9f:
        return flash_jedec[i % 3];
    case 0x90:
        return (f->addr + i) & 1 ? 0x17 : 0xef;
    case 0xab:
        return 0x17;
    case 0x4b:                                      /* unique ID */
        return i < 8 ? 0xe6 ^ (0x31 * (i + 1)) : 0xff;
    case 0x05:
        return f->sr[0];
    case 0x35:
        return f->sr[1];
    case 0x15:
        return f->sr[2];
    case 0x01:                                      /* SR1, then SR2 */
        if (i < 2 && (f->sr[0] & 0x02)) {
            f->sr[i] = i ? b & 0x7f : b & 0xfc;
        }
        return 0xff;
    case 0x31: case 0x11:
        if (i == 0 && (f->sr[0] & 0x02)) {
            f->sr[f->cmd == 0x31 ? 1 : 2] = b;
        }
        return 0xff;
    case 0x02: case 0x32:                           /* within its page */
        if (f->sr[0] & 0x02) {
            a = (f->addr & (RP2350_FLASH_SIZE - 1) & ~0xffu) |
                ((f->addr + i) & 0xff);
            f->mem[a] &= b;
            f->dirty_lo = MIN(f->dirty_lo, a);
            f->dirty_hi = MAX(f->dirty_hi, a + 1);
        }
        return 0xff;
    default:
        return 0xff;
    }
}

/* one byte on the bus, on @lanes lanes: what the flash drives back */
static uint8_t flash_byte(RP2350State *s, uint8_t b, int lanes)
{
    RP2Flash *f = &s->qmi.flash;

    switch (f->phase) {
    case F_CMD:
        if (lanes != 1) {               /* a QPI command: not for us */
            f->phase = F_IGNORE;
            return 0xff;
        }
        flash_command(s, b);
        return 0xff;
    case F_ADDR:
        f->addr = (f->addr << 8) | b;
        if (++f->got == 3) {
            if (f->cmd == 0xeb || f->cmd == 0xbb) {
                f->phase = F_MODE;
            } else {
                f->phase = f->dummy ? F_DUMMY : F_DATA;
            }
        }
        return 0xff;
    case F_MODE:                        /* Axh: the next read has no command */
        f->mode = b;
        f->continuous = (b & 0x30) == 0x20;
        f->cont_cmd = f->cmd;
        f->phase = f->dummy ? F_DUMMY : F_DATA;
        return 0xff;
    case F_DUMMY:
        f->dummy -= 8 / lanes;
        if (f->dummy <= 0) {
            f->phase = F_DATA;
        }
        return 0xff;
    case F_DATA:
        return flash_data(s, b);
    default:
        return 0xff;
    }
}

/* ------------------------------------------------------------- QMI */

static void qmi_rx_push(RP2QMI *q, uint16_t v)
{
    if (q->rx_n == RP2_QMI_RX) {
        return;
    }
    q->rx[(q->rx_head + q->rx_n++) % RP2_QMI_RX] = v;
}

static void qmi_auto_end(RP2350State *s)
{
    if (s->qmi.auto_open) {
        s->qmi.auto_open = false;
        flash_deselect(s);
    }
}

/* what TX holds goes out once direct mode is enabled, with CS or without */
static void qmi_pump(RP2350State *s)
{
    RP2QMI *q = &s->qmi;
    uint32_t csr = q->regs.regs[0];

    if (!(csr & CSR_EN)) {
        return;
    }
    while (q->tx_n) {
        uint32_t v = q->tx[q->tx_head];
        int lanes = 1 << ((v >> 16) & 3);
        int bits = v & TX_DWIDTH ? 16 : 8;
        uint32_t in = 0;

        q->tx_head = (q->tx_head + 1) % RP2_QMI_RX;
        q->tx_n--;
        if ((csr & CSR_AUTO_CS0N) && !(csr & CSR_ASSERT_CS0N) &&
            !q->auto_open) {
            q->auto_open = true;
            flash_select(s);
        }
        for (int i = bits - 8; i >= 0; i -= 8) {
            uint8_t b = (v >> i) & 0xff;

            in = in << 8 |
                 (q->flash.selected ? flash_byte(s, b, lanes) : 0xff);
        }
        if (!(v & TX_NOPUSH)) {
            qmi_rx_push(q, in);
        }
    }
}

static void qmi_tx(RP2350State *s, uint32_t v)
{
    RP2QMI *q = &s->qmi;

    if (q->tx_n == RP2_QMI_RX) {
        qemu_log_mask(LOG_GUEST_ERROR, "rp2350: QMI: TX is full\n");
        return;
    }
    q->tx[(q->tx_head + q->tx_n++) % RP2_QMI_RX] = v;
    qmi_pump(s);
}

/* ATRANS0-3: CS0's four 4 MiB windows onto the flash */
static void qmi_map(RP2350State *s)
{
    RP2QMI *q = &s->qmi;

    memory_region_transaction_begin();
    for (int w = 0; w < 4; w++) {
        uint32_t at = q->regs.regs[REG_ATRANS0 + w];
        uint64_t base = (uint64_t)(at & 0xfff) * 4 * KiB;
        uint64_t size = MIN((uint64_t)((at >> 16) & 0x7ff) * 4 * KiB, 4 * MiB);

        if (base >= RP2350_FLASH_SIZE) {
            size = 0;
        }
        size = MIN(size, RP2350_FLASH_SIZE - base);
        for (int c = 0; c < 2; c++) {
            MemoryRegion *mr = &q->win[c][w];

            memory_region_set_enabled(mr, size != 0);
            if (size) {
                memory_region_set_alias_offset(mr, base);
                memory_region_set_size(mr, size);
            }
        }
    }
    memory_region_transaction_commit();
}

static uint32_t qmi_read(RP2Block *b, hwaddr reg)
{
    RP2350State *s = b->soc;
    RP2QMI *q = &s->qmi;
    uint16_t v;

    switch (reg) {
    case 0x0:                                       /* DIRECT_CSR */
        qmi_auto_end(s);
        return (b->regs[0] & CSR_KEEP) | (q->tx_n ? 0 : CSR_TXEMPTY) |
               (MIN(q->tx_n, 7) << 12) |
               (q->rx_n ? 0 : CSR_RXEMPTY) |
               (q->rx_n == RP2_QMI_RX ? CSR_RXFULL : 0) |
               (MIN(q->rx_n, 7) << 18);
    case 0x4:
        return 0;
    case 0x8:                                       /* DIRECT_RX */
        qmi_auto_end(s);
        if (!q->rx_n) {
            return 0;
        }
        v = q->rx[q->rx_head];
        q->rx_head = (q->rx_head + 1) % RP2_QMI_RX;
        q->rx_n--;
        return v;
    default:
        return reg <= 0x50 ? b->regs[reg / 4] : 0;
    }
}

static void qmi_write(RP2Block *b, hwaddr reg, uint32_t value)
{
    RP2350State *s = b->soc;
    uint32_t old = b->regs[0];

    switch (reg) {
    case 0x0:                                       /* DIRECT_CSR */
        qmi_auto_end(s);
        b->regs[0] = value & CSR_KEEP;
        if (!(value & CSR_EN)) {
            b->regs[0] &= ~(CSR_ASSERT_CS0N | CSR_ASSERT_CS1N);
        }
        if (!(old & CSR_ASSERT_CS0N) && (b->regs[0] & CSR_ASSERT_CS0N)) {
            flash_select(s);
        } else if ((old & CSR_ASSERT_CS0N) &&
                   !(b->regs[0] & CSR_ASSERT_CS0N)) {
            flash_deselect(s);
        }
        qmi_pump(s);
        break;
    case 0x4:                                       /* DIRECT_TX */
        qmi_tx(s, value);
        break;
    case 0x8:
        break;
    case 0x34 ... 0x50:                             /* ATRANS0-7 */
        b->regs[reg / 4] = value & 0x07ff0fff;
        if (reg < 0x44) {
            qmi_map(s);
        }
        break;
    default:
        if (reg < 0x34) {
            b->regs[reg / 4] = value;
        }
    }
}

/* the CPU's own writes to the flash's array (XIP has no write format) */
static void flash_array_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "rp2350: a write to the XIP flash at "
                  "0x%" HWADDR_PRIx " is ignored\n", addr);
}

static const MemoryRegionOps flash_array_ops = {
    .write = flash_array_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

bool rp2350_qmi_realize(RP2350State *s, Error **errp)
{
    RP2QMI *q = &s->qmi;
    Object *obj = OBJECT(s);
    RP2Flash *f = &q->flash;

    if (!memory_region_init_rom_device(&q->mem, obj, &flash_array_ops, s,
                                       "rp2350.flash", RP2350_FLASH_SIZE,
                                       errp)) {
        return false;
    }
    f->mem = memory_region_get_ram_ptr(&q->mem);
    memset(f->mem, 0xff, RP2350_FLASH_SIZE);
    f->fd = -1;
    if (s->flash) {
        ssize_t n;

        f->fd = open(s->flash, O_RDWR);
        if (f->fd < 0) {
            f->fd = open(s->flash, O_RDONLY);   /* then not written back */
        }
        if (f->fd < 0) {
            error_setg_errno(errp, errno, "rp2350: flash %s", s->flash);
            return false;
        }
        n = pread(f->fd, f->mem, RP2350_FLASH_SIZE, 0);
        if (n < 0) {
            error_setg_errno(errp, errno, "rp2350: flash %s", s->flash);
            return false;
        }
        if ((fcntl(f->fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
            close(f->fd);               /* read only: not written back */
            f->fd = -1;
        }
    }
    for (int c = 0; c < 2; c++) {
        for (int w = 0; w < 4; w++) {
            g_autofree char *name = g_strdup_printf("rp2350.xip%s.%d",
                                                    c ? "-nocache" : "", w);

            memory_region_init_alias(&q->win[c][w], obj, name, &q->mem,
                                     w * 4 * MiB, 4 * MiB);
            memory_region_add_subregion(&s->bus,
                (c ? RP2350_XIP_NOCACHE_BASE : RP2350_XIP_BASE) + w * 4 * MiB,
                &q->win[c][w]);
        }
    }
    memory_region_init_alias(&q->untranslated, obj, "rp2350.xip-untranslated",
                             &q->mem, 0, RP2350_FLASH_SIZE);
    memory_region_add_subregion(&s->bus, RP2350_XIP_NOTRANS_BASE,
                                &q->untranslated);
    rp2_block_init(s, &q->regs, "rp2350.qmi", 0x400d0000, qmi_read,
                   qmi_write);
    return true;
}

void rp2350_qmi_reset(RP2350State *s)
{
    RP2QMI *q = &s->qmi;
    RP2Flash *f = &q->flash;

    memset(q->regs.regs, 0, sizeof(q->regs.regs));
    q->regs.regs[0] = 0x01800000;                   /* DIRECT_CSR: CLKDIV 6 */
    q->regs.regs[0xc / 4] = 0x40000004;             /* M0_TIMING */
    q->regs.regs[0x10 / 4] = 0x00001000;            /* M0_RFMT: 03h, 8-bit */
    q->regs.regs[0x14 / 4] = 0x000000a003;          /* M0_RCMD */
    for (int i = 0; i < 8; i++) {
        q->regs.regs[REG_ATRANS0 + i] = 0x04000000 | (i & 3) * 0x400;
    }
    q->rx_n = q->rx_head = 0;
    q->tx_n = q->tx_head = 0;
    q->auto_open = false;
    f->selected = f->continuous = false;
    f->phase = F_CMD;
    f->cmd = 0;
    f->sr[0] = 0;
    f->sr[1] = 0x02;                                /* QE */
    f->sr[2] = 0x60;
    qmi_map(s);
}
