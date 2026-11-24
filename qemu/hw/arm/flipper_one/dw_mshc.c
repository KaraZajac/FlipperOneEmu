/*
 * Synopsys DesignWare Mobile Storage Host (dw-mshc): the RK3576's SD host,
 * in front of QEMU's SD card, as Linux's dw_mmc drives it
 *
 * A command (CMD with START) goes to the card at once: its response in
 * RESP0-3 (a long one as its 128 bits, RESP3 the top), CMD_DONE in RINTSTS,
 * or RTO if the card gave none; "update clock" commands only clear START.
 * Its data (DAT_EXP) follows at once:
 *   IDMAC     (CTRL USE_IDMAC, BMOD ENABLE): the descriptor chain from
 *             DBADDR, 32-bit descriptors (OWN, chained or ring, two buffers
 *             of up to 8 KiB), each given back; RI or TI with NI in IDSTS
 *   the FIFO  (the driver's own, for a few bytes): a read's bytes wait in
 *             the FIFO (the data port at 0x200, VERID 0x270a) with RXDR, a
 *             write's are taken from it until BYTCNT have come (TXDR)
 * then DATA_OVER, and with SEND_STOP an automatic CMD12 (ACD). RINTSTS and
 * IDSTS are cleared by writing 1s; the interrupt is RINTSTS & INTMASK with
 * CTRL's INT_ENABLE, or IDSTS & IDINTEN. CTRL's and BMOD's resets clear
 * themselves. HCON: one slot, 32-bit FIFO, the internal DMA, 32-bit
 * addresses. CDETECT says a card is in; the board's card-detect GPIO is
 * what the kernel looks at. The bus's timing is not modelled.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "rk3576.h"

#define R_CTRL          0x000
#define R_PWREN         0x004
#define R_CLKDIV        0x008
#define R_CLKSRC        0x00c
#define R_CLKENA        0x010
#define R_TMOUT         0x014
#define R_CTYPE         0x018
#define R_BLKSIZ        0x01c
#define R_BYTCNT        0x020
#define R_INTMASK       0x024
#define R_CMDARG        0x028
#define R_CMD           0x02c
#define R_RESP0         0x030
#define R_MINTSTS       0x040
#define R_RINTSTS       0x044
#define R_STATUS        0x048
#define R_FIFOTH        0x04c
#define R_CDETECT       0x050
#define R_WRTPRT        0x054
#define R_TCBCNT        0x05c
#define R_TBBCNT        0x060
#define R_USRID         0x068
#define R_VERID         0x06c
#define R_HCON          0x070
#define R_UHS_REG       0x074
#define R_BMOD          0x080
#define R_PLDMND        0x084
#define R_DBADDR        0x088
#define R_IDSTS         0x08c
#define R_IDINTEN       0x090
#define R_DSCADDR       0x094
#define R_BUFADDR       0x098
#define R_DATA          0x200

#define CTRL_RESET      (1 << 0)
#define CTRL_FIFO_RESET (1 << 1)
#define CTRL_DMA_RESET  (1 << 2)
#define CTRL_INT_ENABLE (1 << 4)
#define CTRL_USE_IDMAC  (1 << 25)

#define CMD_START       (1u << 31)
#define CMD_UPD_CLK     (1 << 21)
#define CMD_SEND_STOP   (1 << 12)
#define CMD_DAT_WR      (1 << 10)
#define CMD_DAT_EXP     (1 << 9)
#define CMD_RESP_LONG   (1 << 7)
#define CMD_RESP_EXP    (1 << 6)

#define INT_ACD         (1 << 14)
#define INT_RTO         (1 << 8)
#define INT_RXDR        (1 << 5)
#define INT_TXDR        (1 << 4)
#define INT_DATA_OVER   (1 << 3)
#define INT_CMD_DONE    (1 << 2)

#define BMOD_SWRESET    (1 << 0)
#define BMOD_ENABLE     (1 << 7)
#define IDSTS_TI        (1 << 0)
#define IDSTS_RI        (1 << 1)
#define IDSTS_DU        (1 << 4)
#define IDSTS_NI        (1 << 8)
#define IDSTS_AI        (1 << 9)

#define DES0_CH         (1 << 4)
#define DES0_ER         (1 << 5)
#define DES0_LD         (1 << 2)
#define DES0_OWN        (1u << 31)

#define VERID           0x5342270a
#define HCON            (1 << 7)                /* a 32-bit FIFO, IDMAC */

static bool dw_idmac(DwMshcState *s)
{
    return (s->ctrl & CTRL_USE_IDMAC) && (s->bmod & BMOD_ENABLE);
}

static void dw_irq(DwMshcState *s)
{
    bool on = ((s->ctrl & CTRL_INT_ENABLE) && (s->rintsts & s->intmask)) ||
              (s->idsts & s->idinten & 0x3ff);

    qemu_set_irq(s->irq, on);
}

/* the bytes of a transfer between the card and memory, through the
 * descriptors: how many were moved */
static uint32_t dw_idmac_run(DwMshcState *s, bool write, uint32_t left)
{
    hwaddr desc = s->dbaddr;
    uint32_t moved = 0;
    uint8_t buf[8192];

    for (int n = 0; left && n < 4096; n++) {
        uint32_t d[4];

        dma_memory_read(&address_space_memory, desc, d, sizeof(d),
                        MEMTXATTRS_UNSPECIFIED);
        for (int i = 0; i < 4; i++) {
            d[i] = le32_to_cpu(d[i]);
        }
        if (!(d[0] & DES0_OWN)) {
            s->idsts |= IDSTS_DU | IDSTS_AI;    /* descriptor unavailable */
            break;
        }
        s->dscaddr = desc;
        for (int b = 0; b < 2 && left; b++) {
            uint32_t size = b ? (d[1] >> 13) & 0x1fff : d[1] & 0x1fff;
            uint32_t addr = b ? d[3] : d[2];
            uint32_t len = MIN(size, left);

            if (b && (d[0] & DES0_CH)) {
                break;                          /* des3: the next one */
            }
            if (!len) {
                continue;
            }
            if (write) {
                dma_memory_read(&address_space_memory, addr, buf, len,
                                MEMTXATTRS_UNSPECIFIED);
                sdbus_write_data(&s->sdbus, buf, len);
            } else {
                sdbus_read_data(&s->sdbus, buf, len);
                dma_memory_write(&address_space_memory, addr, buf, len,
                                 MEMTXATTRS_UNSPECIFIED);
            }
            s->bufaddr = addr;
            left -= len;
            moved += len;
        }
        d[0] = cpu_to_le32(d[0] & ~DES0_OWN);   /* given back */
        dma_memory_write(&address_space_memory, desc, &d[0], 4,
                         MEMTXATTRS_UNSPECIFIED);
        if (le32_to_cpu(d[0]) & DES0_LD) {
            break;
        }
        if (le32_to_cpu(d[0]) & DES0_CH) {
            desc = d[3];
        } else if (le32_to_cpu(d[0]) & DES0_ER) {
            desc = s->dbaddr;
        } else {
            desc += 16;
        }
    }
    s->idsts |= (write ? IDSTS_TI : IDSTS_RI) | IDSTS_NI;
    return moved;
}

static void dw_stop(DwMshcState *s)
{
    SDRequest req = { .cmd = 12, .arg = 0 };
    uint8_t resp[16];

    sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    s->rintsts |= INT_ACD;
}

static void dw_data_done(DwMshcState *s, uint32_t moved)
{
    s->tcbcnt = s->tbbcnt = moved;
    s->rintsts |= INT_DATA_OVER;
    if (s->cmd & CMD_SEND_STOP) {
        dw_stop(s);
    }
}

static void dw_data(DwMshcState *s, bool write)
{
    uint32_t len = s->bytcnt;

    s->fifo_len = s->fifo_pos = 0;
    s->writing = 0;
    if (dw_idmac(s)) {
        dw_data_done(s, dw_idmac_run(s, write, len));
        return;
    }
    if (len > sizeof(s->fifo)) {
        len = sizeof(s->fifo);
    }
    if (write) {                                /* the driver fills it */
        s->writing = len;
        s->rintsts |= INT_TXDR;
        return;
    }
    sdbus_read_data(&s->sdbus, s->fifo, len);
    s->fifo_len = len;
    s->rintsts |= INT_RXDR;
    dw_data_done(s, len);
}

static void dw_command(DwMshcState *s)
{
    SDRequest req = { .cmd = s->cmd & 0x3f, .arg = s->cmdarg };
    uint8_t resp[16];
    size_t n;

    if (s->cmd & CMD_UPD_CLK) {                 /* the card's clock: no
                                                   command goes out */
        s->cmd &= ~CMD_START;
        return;
    }
    n = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    s->cmd &= ~CMD_START;
    if (s->cmd & CMD_RESP_EXP) {
        if (!n) {
            s->rintsts |= INT_RTO | INT_CMD_DONE;
            dw_irq(s);
            return;
        }
        if (s->cmd & CMD_RESP_LONG) {
            for (int i = 0; i < 4; i++) {
                s->resp[3 - i] = ldl_be_p(&resp[4 * i]);
            }
        } else {
            s->resp[0] = ldl_be_p(resp);
        }
    }
    s->rintsts |= INT_CMD_DONE;
    if (s->cmd & CMD_DAT_EXP) {
        dw_data(s, s->cmd & CMD_DAT_WR);
    }
    dw_irq(s);
}

static uint32_t dw_fifo_pop(DwMshcState *s)
{
    uint32_t v = 0;

    for (int i = 0; i < 4 && s->fifo_pos < s->fifo_len; i++) {
        v |= s->fifo[s->fifo_pos++] << (8 * i);
    }
    return v;
}

static void dw_fifo_push(DwMshcState *s, uint64_t v, unsigned size)
{
    for (int i = 0; i < size && s->fifo_len < s->writing; i++) {
        s->fifo[s->fifo_len++] = v >> (8 * i);
    }
    if (s->writing && s->fifo_len == s->writing) {
        sdbus_write_data(&s->sdbus, s->fifo, s->fifo_len);
        s->writing = 0;
        s->rintsts &= ~INT_TXDR;
        dw_data_done(s, s->fifo_len);
        s->fifo_len = 0;
    }
}

static uint64_t dw_read(void *opaque, hwaddr off, unsigned size)
{
    DwMshcState *s = opaque;
    uint32_t words;

    if (off >= R_DATA && off < R_DATA + 0x200) {
        return dw_fifo_pop(s);
    }
    switch (off) {
    case R_CTRL: return s->ctrl;
    case R_PWREN: return s->pwren;
    case R_CLKDIV: return s->clkdiv;
    case R_CLKSRC: return s->clksrc;
    case R_CLKENA: return s->clkena;
    case R_TMOUT: return s->tmout;
    case R_CTYPE: return s->ctype;
    case R_BLKSIZ: return s->blksiz;
    case R_BYTCNT: return s->bytcnt;
    case R_INTMASK: return s->intmask;
    case R_CMDARG: return s->cmdarg;
    case R_CMD: return s->cmd;
    case R_RESP0 ... R_RESP0 + 0xc: return s->resp[(off - R_RESP0) / 4];
    case R_MINTSTS: return s->rintsts & s->intmask;
    case R_RINTSTS: return s->rintsts;
    case R_STATUS:
        words = (s->fifo_len - s->fifo_pos + 3) / 4;
        return words << 17 | (words ? 0 : 1 << 2);
    case R_FIFOTH: return s->fifoth;
    case R_CDETECT: return sdbus_get_inserted(&s->sdbus) ? 0 : 1;
    case R_WRTPRT: return 0;
    case R_TCBCNT: return s->tcbcnt;
    case R_TBBCNT: return s->tbbcnt;
    case R_USRID: return s->usrid;
    case R_VERID: return VERID;
    case R_HCON: return HCON;
    case R_UHS_REG: return s->uhs;
    case R_BMOD: return s->bmod;
    case R_DBADDR: return s->dbaddr;
    case R_IDSTS: return s->idsts;
    case R_IDINTEN: return s->idinten;
    case R_DSCADDR: return s->dscaddr;
    case R_BUFADDR: return s->bufaddr;
    }
    return off / 4 < ARRAY_SIZE(s->other) ? s->other[off / 4] : 0;
}

static void dw_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    DwMshcState *s = opaque;

    if (off >= R_DATA && off < R_DATA + 0x200) {
        dw_fifo_push(s, v, size);
        dw_irq(s);
        return;
    }
    switch (off) {
    case R_CTRL:
        if (v & (CTRL_RESET | CTRL_FIFO_RESET)) {
            s->fifo_len = s->fifo_pos = 0;
            s->writing = 0;
        }
        s->ctrl = v & ~(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        break;
    case R_PWREN: s->pwren = v; break;
    case R_CLKDIV: s->clkdiv = v; break;
    case R_CLKSRC: s->clksrc = v; break;
    case R_CLKENA: s->clkena = v; break;
    case R_TMOUT: s->tmout = v; break;
    case R_CTYPE: s->ctype = v; break;
    case R_BLKSIZ: s->blksiz = v; break;
    case R_BYTCNT: s->bytcnt = v; break;
    case R_INTMASK: s->intmask = v; break;
    case R_CMDARG: s->cmdarg = v; break;
    case R_CMD:
        s->cmd = v;
        if (v & CMD_START) {
            dw_command(s);
        }
        break;
    case R_RINTSTS: s->rintsts &= ~v; break;
    case R_FIFOTH: s->fifoth = v; break;
    case R_USRID: s->usrid = v; break;
    case R_UHS_REG:
        s->uhs = v;
        sdbus_set_voltage(&s->sdbus, v & 1 ? 1800 : 3300);
        break;
    case R_BMOD:
        if (v & BMOD_SWRESET) {
            s->dscaddr = s->dbaddr;
        }
        s->bmod = v & ~BMOD_SWRESET;
        break;
    case R_PLDMND: break;                       /* (runs with its command) */
    case R_DBADDR: s->dbaddr = s->dscaddr = v; break;
    case R_IDSTS:
        s->idsts &= ~(v & 0x3ff);
        if (!(s->idsts & 0xff)) {
            s->idsts &= ~(IDSTS_NI | IDSTS_AI);
        }
        break;
    case R_IDINTEN: s->idinten = v; break;
    default:
        if (off / 4 < ARRAY_SIZE(s->other)) {
            s->other[off / 4] = v;              /* (timing, phases) */
        }
    }
    dw_irq(s);
}

static const MemoryRegionOps dw_ops = {
    .read = dw_read,
    .write = dw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

SDBus *dw_mshc_bus(DwMshcState *s)
{
    return &s->sdbus;
}

static void dw_reset(DeviceState *dev)
{
    DwMshcState *s = DW_MSHC(dev);

    s->ctrl = s->pwren = s->clkdiv = s->clksrc = s->clkena = 0;
    s->tmout = 0xffffff40;
    s->ctype = s->blksiz = 0;
    s->bytcnt = 0x200;
    s->intmask = s->cmdarg = s->cmd = s->rintsts = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->fifoth = s->uhs = s->bmod = s->dbaddr = s->idsts = s->idinten = 0;
    s->dscaddr = s->bufaddr = s->tcbcnt = s->tbbcnt = 0;
    s->fifo_len = s->fifo_pos = 0;
    s->writing = 0;
    memset(s->other, 0, sizeof(s->other));
    qemu_set_irq(s->irq, 0);
}

static void dw_init(Object *obj)
{
    DwMshcState *s = DW_MSHC(obj);

    memory_region_init_io(&s->mr, obj, &dw_ops, s, "dw-mshc", 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(obj),
              "sd-bus");
}

static void dw_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), dw_reset);
}

static const TypeInfo dw_types[] = {
    {
        .name = TYPE_DW_MSHC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DwMshcState),
        .instance_init = dw_init,
        .class_init = dw_class_init,
    },
};

DEFINE_TYPES(dw_types)
