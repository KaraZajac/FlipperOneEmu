/*
 * The RK3576's SAIs (serial audio interfaces), as Linux's rockchip_sai
 * drives them
 *
 * A SAI sits between its DMAC and an I2S link: what the DMAC writes to
 * TXDR goes out on the link, what comes in is read from RXDR. Here:
 * - Its registers as the driver uses them. VERSION is 0x23073576, the
 *   RK3576's, so the driver looks for its idle flags in STATUS (they are
 *   in XFER too, where older SAIs had them): the frame clock is idle while
 *   the SAI is master with its clock and frame sync off, or slave with
 *   neither direction on; a direction is idle while it is off. CLR's
 *   clears are done at once. Its interrupts (FIFO underrun and overrun)
 *   are never raised: an emulated CPU can be slow to feed it, and a short
 *   gap is a gap of silence here, not an error the driver would stop the
 *   stream for.
 * - Its FIFOs, of 1024 words each way (the chip's are shallower: the
 *   frames are taken a millisecond at a time here, or more when the timer
 *   is late, not one at a time). Its DMA requests go to QEMU's PL330, whose
 *   request inputs hold a channel at DMAWFP while high: TX asks while
 *   DMACR's TDE is set and 256 words are free, RX while RDE is set and 144
 *   words are there, since that PL330 queues up to 16 bursts' loads and
 *   stores (128 words) before they land. They are driven again after every
 *   reset, since the PL330's own reset forgets them.
 * - Its frame clock: as master (SAI6, HDMI's), MCLK / (MDIV + 1) / (FW +
 *   1) frames a second, MCLK a property (12.288 MHz); as slave (SAI2, the
 *   codec's), the codec's. While TXS or RXS is on and the clock runs, it
 *   is looked at every millisecond of virtual time: the frames due since
 *   are taken from the TX FIFO, as TXCR says (16-bit samples two to a
 *   word, low half first; wider ones one to a word), and given to its
 *   sink as S16 stereo (the codec's DAC; with none, they go nowhere). A
 *   frame the FIFO does not have yet goes when it comes, up to 20 ms late:
 *   the DMAC's ring always has frames, so a short FIFO means only that the
 *   emulation is behind. RX gets silence at the same rate.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "rk3576.h"

#define SAI_TXCR        0x00
#define SAI_FSCR        0x04
#define SAI_RXCR        0x08
#define SAI_XFER        0x10
#define XFER_CLK        BIT(0)
#define XFER_FSS        BIT(1)
#define XFER_TXS        BIT(2)
#define XFER_RXS        BIT(3)
#define SAI_CLR         0x14
#define CLR_TXC         BIT(0)
#define CLR_RXC         BIT(1)
#define CLR_FSC         BIT(2)
#define CLR_FCR         BIT(3)
#define SAI_CKR         0x18
#define CKR_MSS         BIT(2)          /* slave */
#define SAI_TXFIFOLR    0x1c
#define SAI_RXFIFOLR    0x20
#define SAI_DMACR       0x24
#define DMACR_TDE       BIT(8)
#define DMACR_RDE       BIT(24)
#define SAI_INTCR       0x28
#define INTCR_CLEARS    (BIT(28) | BIT(26) | BIT(18) | BIT(2))
#define SAI_INTSR       0x2c
#define SAI_TXDR        0x30
#define SAI_RXDR        0x34
#define SAI_PATH_SEL    0x38
#define SAI_TX_DATA_CNT 0x5c
#define SAI_RX_DATA_CNT 0x60
#define SAI_STATUS      0x6c
#define STATUS_FS_IDLE  BIT(1)
#define STATUS_TX_IDLE  BIT(2)
#define STATUS_RX_IDLE  BIT(3)
#define SAI_VERSION     0x70
#define SAI_SIZE        0x80

#define SAI_VERSION_RK3576  0x23073576
#define TICK_NS         1000000         /* the frame clock: every 1 ms */
/* the words free for a TX request, and there for an RX one: QEMU's PL330
 * queues up to 16 bursts' loads and stores (of 8 words) before they land,
 * past the requests that let them go */
#define TX_ROOM         256
#define RX_LEVEL        144
#define CHUNK           256             /* frames to the sink at a time */

#define R(a)            ((a) / 4)

/* -------------------------------------------------------- the clock */

static bool sai_master(RkSaiState *s)
{
    return !(s->regs[R(SAI_CKR)] & CKR_MSS);
}

/* frames a second on the link now (0: no clock) */
static int sai_rate(RkSaiState *s)
{
    uint32_t xfer = s->regs[R(SAI_XFER)];

    if (sai_master(s)) {
        uint32_t mdiv = (s->regs[R(SAI_CKR)] >> 3) & 0xfff;
        uint32_t fw = s->regs[R(SAI_FSCR)] & 0xfff;

        if ((xfer & (XFER_CLK | XFER_FSS)) != (XFER_CLK | XFER_FSS)) {
            return 0;
        }
        return s->mclk / (mdiv + 1) / (fw + 1);
    }
    return s->lrck_hz;
}

/* STATUS's idle flags */
static uint32_t sai_idle(RkSaiState *s)
{
    uint32_t xfer = s->regs[R(SAI_XFER)], st = 0;
    bool fs = sai_master(s) ?
              (xfer & (XFER_CLK | XFER_FSS)) == (XFER_CLK | XFER_FSS) :
              (xfer & (XFER_TXS | XFER_RXS));

    if (!fs) {
        st |= STATUS_FS_IDLE;
    }
    if (!(xfer & XFER_TXS)) {
        st |= STATUS_TX_IDLE;
    }
    if (!(xfer & XFER_RXS)) {
        st |= STATUS_RX_IDLE;
    }
    return st;
}

/* ------------------------------------------------------- the FIFOs */

static void sai_dma_update(RkSaiState *s)
{
    uint32_t dmacr = s->regs[R(SAI_DMACR)];
    bool tx = (dmacr & DMACR_TDE) && RK_SAI_FIFO - s->tx_n >= TX_ROOM;
    bool rx = (dmacr & DMACR_RDE) && s->rx_n >= RX_LEVEL;

    qemu_set_irq(s->dma_busy[0], !tx);
    qemu_set_irq(s->dma_busy[1], !rx);
}

static void sai_tx_flush(RkSaiState *s)
{
    s->tx_head = s->tx_n = 0;
    s->tx_half = false;
    s->tx_late = 0;
}

static void sai_rx_flush(RkSaiState *s)
{
    s->rx_head = s->rx_n = 0;
    s->rx_half = false;
}

/* samples the TX FIFO holds, @vdw bits each */
static int sai_tx_samples(RkSaiState *s, int vdw)
{
    return vdw <= 16 ? 2 * s->tx_n - s->tx_half : s->tx_n;
}

/* the TX FIFO's next sample, as S16 (it has one) */
static int16_t sai_tx_sample(RkSaiState *s, int vdw)
{
    uint32_t w = s->tx[s->tx_head];
    int16_t v;

    if (vdw <= 16) {
        v = s->tx_half ? (int16_t)(w >> 16) : (int16_t)w;
        s->tx_half = !s->tx_half;
        if (s->tx_half) {
            return v;                   /* (the word's high half next) */
        }
    } else {
        v = (int16_t)(w >> (vdw - 16));
    }
    s->tx_head = (s->tx_head + 1) % RK_SAI_FIFO;
    s->tx_n--;
    return v;
}

/* @frames go out on the link, to the sink, as far as the TX FIFO has them:
 * those it has not, still to go. (The FIFO runs short only when the
 * emulation does: the DMAC's ring always has frames to give, old ones if
 * the program is late, so a frame missing here is late, not silent.) */
static int64_t sai_send(RkSaiState *s, int64_t frames)
{
    uint32_t cr = s->regs[R(SAI_TXCR)];
    int slots = ((cr >> 11) & 0x7f) + 1, vdw = (cr & 0x1f) + 1;
    int16_t lr[2 * CHUNK];
    int n = 0;

    while (frames > 0 && sai_tx_samples(s, vdw) >= slots) {
        int16_t l = 0, r = 0;

        for (int i = 0; i < slots; i++) {
            int16_t v = sai_tx_sample(s, vdw);

            if (i == 0) {
                l = r = v;
            } else if (i == 1) {
                r = v;
            }
        }
        lr[2 * n] = l;
        lr[2 * n + 1] = r;
        frames--;
        if (++n == CHUNK) {
            if (s->sink) {
                s->sink(s->sink_opaque, lr, n);
            }
            n = 0;
        }
    }
    if (n && s->sink) {
        s->sink(s->sink_opaque, lr, n);
    }
    return frames;
}

/* @frames come in on the link: silence, into the RX FIFO while it has
 * room */
static void sai_receive(RkSaiState *s, int frames)
{
    uint32_t cr = s->regs[R(SAI_RXCR)];
    int slots = ((cr >> 11) & 0x7f) + 1, vdw = (cr & 0x1f) + 1;
    int words = 0;

    for (int i = 0; i < frames * slots; i++) {
        if (vdw <= 16) {
            s->rx_half = !s->rx_half;
            if (s->rx_half) {
                continue;               /* (a word holds two) */
            }
        }
        words++;
    }
    words = MIN(words, RK_SAI_FIFO - s->rx_n);
    for (int i = 0; i < words; i++) {
        s->rx[(s->rx_head + s->rx_n++) % RK_SAI_FIFO] = 0;
    }
}

static void sai_tick(void *opaque)
{
    RkSaiState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t xfer = s->regs[R(SAI_XFER)];
    int rate = sai_rate(s);
    int64_t frames;

    if (!rate || !(xfer & (XFER_TXS | XFER_RXS))) {
        s->last_ns = 0;                 /* stopped */
        return;
    }
    s->owed += (now - s->last_ns) * rate;
    s->last_ns = now;
    frames = s->owed / NANOSECONDS_PER_SECOND;
    s->owed -= frames * NANOSECONDS_PER_SECOND;
    frames = MIN(frames, rate / 10);    /* (a late tick: 100 ms at most) */
    if (s->verbose) {
        qemu_log("rk-sai %p: %.3f ms: %" PRId64 " frames due, %" PRId64
                 " late; TX FIFO %d, %d words came since\n", s, now / 1e6,
                 frames, s->tx_late, s->tx_n, s->tx_in);
    }
    s->tx_in = 0;
    if (xfer & XFER_TXS) {              /* (late ones wait 20 ms at most) */
        s->tx_late = MIN(sai_send(s, MIN(frames + s->tx_late, rate / 10)),
                         rate / 50);
    } else {
        s->tx_late = 0;
    }
    if (xfer & XFER_RXS) {
        sai_receive(s, frames);
    }
    sai_dma_update(s);
    timer_mod(s->timer, now + TICK_NS);
}

/* the frame clock may have started (it stops by itself) */
static void sai_clock_check(RkSaiState *s)
{
    if (sai_rate(s) && (s->regs[R(SAI_XFER)] & (XFER_TXS | XFER_RXS)) &&
        !s->last_ns) {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->owed = 0;
        s->tx_late = 0;
        timer_mod(s->timer, s->last_ns + TICK_NS);
    }
}

void rk_sai_lrck(void *sai, int hz)
{
    RkSaiState *s = RK_SAI(sai);

    s->lrck_hz = hz;
    sai_clock_check(s);
}

void rk_sai_set_sink(RkSaiState *s, RkSaiSink *sink, void *opaque)
{
    s->sink = sink;
    s->sink_opaque = opaque;
}

/* ---------------------------------------------------- the registers */

static uint64_t sai_read(void *opaque, hwaddr a, unsigned size)
{
    RkSaiState *s = opaque;
    uint32_t v;

    switch (a) {
    case SAI_XFER:                      /* (older SAIs' idle flags, 8:6) */
        return (s->regs[R(a)] & 0x3f) | sai_idle(s) << 5;
    case SAI_STATUS:
        return sai_idle(s);
    case SAI_CLR:
    case SAI_INTSR:
    case SAI_TX_DATA_CNT:
    case SAI_RX_DATA_CNT:
        return 0;
    case SAI_VERSION:
        return SAI_VERSION_RK3576;
    case SAI_TXFIFOLR:                  /* (lane 0's, as the chip counts) */
        return MIN(s->tx_n, 32);
    case SAI_RXFIFOLR:
        return MIN(s->rx_n, 32);
    case SAI_RXDR:
        if (!s->rx_n) {
            return 0;
        }
        v = s->rx[s->rx_head];
        s->rx_head = (s->rx_head + 1) % RK_SAI_FIFO;
        s->rx_n--;
        sai_dma_update(s);
        return v;
    case SAI_TXDR:
        return 0;
    }
    return a < SAI_SIZE ? s->regs[R(a)] : 0;
}

static void sai_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RkSaiState *s = opaque;

    if (s->verbose && a != SAI_TXDR) {
        qemu_log("rk-sai %p: %.3f ms: [%02" HWADDR_PRIx "] <- %08" PRIx64
                 "\n", s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6, a, v);
    }
    switch (a) {
    case SAI_TXDR:
        if (s->tx_n < RK_SAI_FIFO) {
            s->tx[(s->tx_head + s->tx_n++) % RK_SAI_FIFO] = v;
        }
        s->tx_in++;
        sai_dma_update(s);
        return;
    case SAI_XFER:
        s->regs[R(a)] = v & 0x3f;       /* (the idle flags are its own) */
        sai_clock_check(s);
        return;
    case SAI_CLR:                       /* done at once */
        if (v & (CLR_TXC | CLR_FCR)) {
            sai_tx_flush(s);
        }
        if (v & (CLR_RXC | CLR_FCR)) {
            sai_rx_flush(s);
        }
        if (v & (CLR_FSC | CLR_FCR)) {
            s->owed = 0;
        }
        sai_dma_update(s);
        return;
    case SAI_INTCR:                     /* (nothing to clear: no status) */
        s->regs[R(a)] = v & ~INTCR_CLEARS;
        return;
    case SAI_DMACR:
        s->regs[R(a)] = v;
        sai_dma_update(s);
        return;
    case SAI_CKR:
    case SAI_FSCR:
        s->regs[R(a)] = v;
        sai_clock_check(s);
        return;
    case SAI_TXFIFOLR:
    case SAI_RXFIFOLR:
    case SAI_INTSR:
    case SAI_RXDR:
    case SAI_TX_DATA_CNT:
    case SAI_RX_DATA_CNT:
    case SAI_STATUS:
    case SAI_VERSION:
        return;                         /* (read-only) */
    }
    if (a < SAI_SIZE) {
        s->regs[R(a)] = v;
    }
}

static const MemoryRegionOps sai_ops = {
    .read = sai_read,
    .write = sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sai_reset_hold(Object *obj, ResetType type)
{
    RkSaiState *s = RK_SAI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(SAI_TXCR)] = 0x00000bff;
    s->regs[R(SAI_FSCR)] = 0x0001f03f;
    s->regs[R(SAI_RXCR)] = 0x00000bff;
    s->regs[R(SAI_PATH_SEL)] = 0x0000e4e4;
    sai_tx_flush(s);
    sai_rx_flush(s);
    timer_del(s->timer);
    s->last_ns = 0;
    s->owed = 0;
}

static void sai_reset_exit(Object *obj, ResetType type)
{
    RkSaiState *s = RK_SAI(obj);

    sai_dma_update(s);                  /* (after the DMAC's own reset) */
    qemu_set_irq(s->irq, 0);
}

static bool sai_get_verbose(Object *obj, Error **errp)
{
    return RK_SAI(obj)->verbose;
}

static void sai_set_verbose(Object *obj, bool on, Error **errp)
{
    RK_SAI(obj)->verbose = on;
}

static void sai_init(Object *obj)
{
    RkSaiState *s = RK_SAI(obj);

    memory_region_init_io(&s->mr, obj, &sai_ops, s, "rk-sai", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), s->dma_busy, "dma", 2);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sai_tick, s);
    object_property_add_bool(obj, "verbose", sai_get_verbose,
                             sai_set_verbose);
}

static const Property sai_properties[] = {
    DEFINE_PROP_UINT32("mclk", RkSaiState, mclk, 12288000),
};

static void sai_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "RK3576 SAI (serial audio interface)";
    device_class_set_props(dc, sai_properties);
    rc->phases.hold = sai_reset_hold;
    rc->phases.exit = sai_reset_exit;
}

static const TypeInfo sai_types[] = {
    {
        .name = TYPE_RK_SAI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkSaiState),
        .instance_init = sai_init,
        .class_init = sai_class_init,
    },
};

DEFINE_TYPES(sai_types)
