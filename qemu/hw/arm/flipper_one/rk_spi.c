/*
 * Rockchip SPI controller (the RK3576's, as Linux's spi-rockchip drives it,
 * as a host)
 *
 * Each word written to TXDR while the controller is on (SSIENR) goes out at
 * once, to the board (rk_spi_set_out()); the TX FIFO is never full nor
 * busy (TXFLR 0, SR: TF_EMPTY, not BUSY), so that the PL330 can feed it
 * without waiting, as the driver's DMA does for a long transfer (transmit
 * only). Transmit and receive (CTRLR0's XFM 0: the driver without DMA)
 * takes a word in for each one out: 0, nothing driving MISO; RF_FULL is
 * raised when the RX FIFO holds more than RXFTLR. The interrupts are RISR &
 * IMR, TF_EMPTY and RF_FULL as the FIFOs' levels say; VERSION is the
 * newer kind's (a 64-word FIFO). The bus's timing is not modelled.
 *
 * QEMU's PL330 writes a burst to a fixed address as one write, which the
 * memory core splits into accesses of up to 8 bytes: with TX DMA on
 * (DMACR), each byte of an access is a byte of data; otherwise a write is
 * the driver's, one word.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "rk3576.h"

#define R_CTRLR0        0x000
#define R_CTRLR1        0x004
#define R_SSIENR        0x008
#define R_SER           0x00c
#define R_BAUDR         0x010
#define R_TXFTLR        0x014
#define R_RXFTLR        0x018
#define R_TXFLR         0x01c
#define R_RXFLR         0x020
#define R_SR            0x024
#define R_IPR           0x028
#define R_IMR           0x02c
#define R_ISR           0x030
#define R_RISR          0x034
#define R_ICR           0x038
#define R_DMACR         0x03c
#define R_DMATDLR       0x040
#define R_DMARDLR       0x044
#define R_VERSION       0x048
#define R_TXDR          0x400
#define R_RXDR          0x800

#define TF_DMA_EN       (1 << 1)
#define SR_TF_EMPTY     (1 << 2)
#define SR_RF_EMPTY     (1 << 3)
#define SR_RF_FULL      (1 << 4)
#define INT_TF_EMPTY    (1 << 0)
#define INT_RF_FULL     (1 << 4)
#define XFM(cr0)        (((cr0) >> 18) & 3)
#define XFM_TR          0
#define VERSION_V2      0x00110002          /* VER2_TYPE2 */

static uint32_t rkspi_risr(RkSpiState *s)
{
    uint32_t r = INT_TF_EMPTY;              /* (never anything waiting) */

    if (s->rx_n > s->rxftlr) {
        r |= INT_RF_FULL;
    }
    return r;
}

static void rkspi_irq(RkSpiState *s)
{
    qemu_set_irq(s->irq, (rkspi_risr(s) & s->imr) != 0);
}

static uint64_t rkspi_read(void *opaque, hwaddr off, unsigned size)
{
    RkSpiState *s = opaque;
    uint32_t v;

    switch (off) {
    case R_CTRLR0: return s->ctrlr0;
    case R_CTRLR1: return s->ctrlr1;
    case R_SSIENR: return s->ssienr;
    case R_SER: return s->ser;
    case R_BAUDR: return s->baudr;
    case R_TXFTLR: return s->txftlr;
    case R_RXFTLR: return s->rxftlr;
    case R_TXFLR: return 0;
    case R_RXFLR: return s->rx_n;
    case R_SR:
        return SR_TF_EMPTY | (s->rx_n ? 0 : SR_RF_EMPTY) |
               (s->rx_n == RKSPI_FIFO ? SR_RF_FULL : 0);
    case R_IMR: return s->imr;
    case R_ISR: return rkspi_risr(s) & s->imr;
    case R_RISR: return rkspi_risr(s);
    case R_DMACR: return s->dmacr;
    case R_DMATDLR: return s->dmatdlr;
    case R_DMARDLR: return s->dmardlr;
    case R_VERSION: return VERSION_V2;
    case R_RXDR ... R_RXDR + 0x3ff:
        if (!s->rx_n) {
            return 0;
        }
        v = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % RKSPI_FIFO;
        s->rx_n--;
        rkspi_irq(s);
        return v;
    }
    return 0;
}

static void rkspi_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkSpiState *s = opaque;

    switch (off) {
    case R_CTRLR0: s->ctrlr0 = v; break;
    case R_CTRLR1: s->ctrlr1 = v; break;
    case R_SSIENR:
        s->ssienr = v & 1;
        if (!s->ssienr) {
            s->rx_n = s->rx_head = 0;       /* off: the FIFOs empty */
        }
        break;
    case R_SER: s->ser = v & 3; break;
    case R_BAUDR: s->baudr = v; break;
    case R_TXFTLR: s->txftlr = v; break;
    case R_RXFTLR: s->rxftlr = v; break;
    case R_IMR: s->imr = v; break;
    case R_DMACR: s->dmacr = v; break;
    case R_DMATDLR: s->dmatdlr = v; break;
    case R_DMARDLR: s->dmardlr = v; break;
    case R_TXDR ... R_TXDR + 0x3ff: {
        int word = (s->ctrlr0 & 3) == 2 ? 2 : 1;    /* DFS: 16 or 8 bits */
        int len = s->dmacr & TF_DMA_EN ? size : word;
        uint8_t w[8];

        if (!s->ssienr) {
            break;
        }
        for (int i = 0; i < len; i++) {
            w[i] = v >> (8 * i);
        }
        if (s->out) {
            s->out(s->out_opaque, s->ser, w, len);
        }
        for (int i = 0; i < len / word; i++) {
            if (XFM(s->ctrlr0) == XFM_TR && s->rx_n < RKSPI_FIFO) {
                s->rx[(s->rx_head + s->rx_n) % RKSPI_FIFO] = 0;
                s->rx_n++;
            }
        }
        break;
    }
    }
    rkspi_irq(s);
}

static const MemoryRegionOps rkspi_ops = {
    .read = rkspi_read,
    .write = rkspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

void rk_spi_set_out(RkSpiState *s,
                    void (*out)(void *opaque, int cs, const uint8_t *data,
                                int len), void *opaque)
{
    s->out = out;
    s->out_opaque = opaque;
}

static void rkspi_reset(DeviceState *dev)
{
    RkSpiState *s = RK_SPI(dev);

    s->ctrlr0 = s->ctrlr1 = s->ssienr = s->ser = s->baudr = 0;
    s->txftlr = s->rxftlr = s->imr = s->dmacr = s->dmatdlr = 0;
    s->dmardlr = 0;
    s->rx_n = s->rx_head = 0;
    qemu_set_irq(s->irq, 0);
}

static void rkspi_init(Object *obj)
{
    RkSpiState *s = RK_SPI(obj);

    memory_region_init_io(&s->mr, obj, &rkspi_ops, s, "rockchip-spi",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void rkspi_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), rkspi_reset);
}

static const TypeInfo rkspi_types[] = {
    {
        .name = TYPE_RK_SPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkSpiState),
        .instance_init = rkspi_init,
        .class_init = rkspi_class_init,
    },
};

DEFINE_TYPES(rkspi_types)
