/*
 * The RK3576's USB controllers: Synopsys DWC3 cores (DWC_usb3 3.30a),
 * dual-role, as Linux's dwc3 drives them, with QEMU's xHCI for the host
 * role
 *
 * A controller's 4 MiB:
 *   0x0000  QEMU's xHCI (16 KiB: capability, operational, port, runtime
 *           and doorbell registers): one USB 2 and one USB 3 root port,
 *           one interrupter; ERSTBA taking effect on its low dword, as on
 *           DWC3, where Linux writes it high dword first (QEMU patch 0011)
 *   0xc100  the DWC3's global registers: GSNPSID 0x5533330a; GHWPARAMS of
 *           a dual-role core (MODE 2) with 64-bit addresses, a UTMI PHY,
 *           16 endpoints and FIFOs for a gadget; event buffer 0 (GEVNT*);
 *           the rest kept as written
 *   0xc700  its device registers (below)
 *   the rest reads 0
 * Its interrupt is the xHCI's, ORed with the device role's.
 *
 * The device role, as the computer at the cable's end drives it a
 * transaction at a time (usb_host.c, through rk_dwc3_usb_ops), at high
 * speed. RUN_STOP pulls D+ up (with GCTL's port capability on the device).
 * The host's bus reset gives the Reset and Connect Done events (DSTS: high
 * speed). The events go into event buffer 0 as the core writes them, word
 * by word, GEVNTCOUNT counting their bytes and the interrupt up while it
 * is not 0 and not masked (GEVNTSIZ's INTMASK); an event the buffer has no
 * room for waits until the driver has taken some. Endpoint commands
 * (DEPCMD) are done as they are written: Set Endpoint Config (its packet
 * size and the events it gives), Set and Clear Stall, Start Transfer (the
 * TRB it starts at; its resource index is the endpoint's own number), End
 * Transfer; with CMDIOC each gives its Command Complete event. A
 * transaction moves data through the endpoint's TRBs in memory: read from
 * their buffers for an IN, written for an OUT, through link and chained
 * TRBs, a short packet ending a chain; a TRB done gets its remaining size
 * and HWO cleared written back, and gives its event as it says (IOC):
 * Transfer Complete on the control endpoint, Transfer In Progress on the
 * others. With no TRB of the hardware's to take or give a packet, a token
 * is NAKed. The control endpoint's SETUP is always ACKed and goes into its
 * SETUP TRB (held until one is started); a data or status stage token that
 * finds no TRB of its kind gives one Transfer Not Ready (data or status),
 * which is how the driver knows to start the status stage; Set Stall on it
 * stalls the control pipe until the next SETUP. SET_ADDRESS's address
 * takes effect once its status stage is done. The host going away gives a
 * Disconnect.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "rk3576.h"
#include "fo_usb.h"

#define REGS_BASE       0xc100
#define R(a)            (((a) - REGS_BASE) / 4)

#define GCTL            0xc110
#define GCTL_PRTCAPDIR(v) (((v) >> 12) & 3)
#define PRTCAP_DEVICE   2
#define GSNPSID         0xc120
#define GHWPARAMS0      0xc140          /* ... GHWPARAMS7 at 0xc15c */
#define GUSB2PHYCFG     0xc200
#define GUSB3PIPECTL    0xc2c0
#define GTXFIFOSIZ(n)   (0xc300 + 4 * (n))
#define GRXFIFOSIZ0     0xc380
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTSIZ_INTMASK BIT(31)
#define GEVNTCOUNT      0xc40c
#define GHWPARAMS8      0xc600
#define DCFG            0xc700
#define DCFG_DEVADDR(v) (((v) >> 3) & 0x7f)
#define DCTL            0xc704
#define DCTL_RUN_STOP   BIT(31)
#define DCTL_CSFTRST    BIT(30)
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DSTS_RXFIFOEMPTY BIT(17)
#define DSTS_USBLNKST(v) ((v) << 18)
#define DSTS_DEVCTRLHLT BIT(22)
#define DGCMD           0xc714
#define DALEPENA        0xc720
#define DEPCMD_BASE     0xc800          /* each endpoint's 16 bytes: */
#define DEPCMDPAR1(n)   (DEPCMD_BASE + 0x10 * (n) + 0x4)
#define DEPCMDPAR0(n)   (DEPCMD_BASE + 0x10 * (n) + 0x8)
#define DEPCMD(n)       (DEPCMD_BASE + 0x10 * (n) + 0xc)
#define CMDIOC          BIT(8)
#define CMDACT          BIT(10)
#define CMDSTATUS       0xf000
#define CMDRSCIDX       0x7f0000

enum {                                  /* endpoint commands */
    EPCMD_SET_CONFIG = 1, EPCMD_SET_STALL = 4, EPCMD_CLEAR_STALL = 5,
    EPCMD_START = 6, EPCMD_END = 8,
};

/* the device's events, and an endpoint's */
enum { DEVT_DISCONNECT = 0, DEVT_RESET = 1, DEVT_CONNECT_DONE = 2 };
enum {
    EPEVT_XFER_COMPLETE = 1, EPEVT_XFER_IN_PROGRESS = 2,
    EPEVT_XFER_NOT_READY = 3, EPEVT_CMD_COMPLETE = 7,
};
#define EPST_SHORT      BIT(1)
#define EPST_IOC        BIT(2)
#define EPST_LST        BIT(3)
enum { NRDY_DATA = 1, NRDY_STATUS = 2 };

/* a TRB: 16 bytes in memory, its buffer's address, its size, its control */
#define TRB_HWO         BIT(0)
#define TRB_LST         BIT(1)
#define TRB_CHN         BIT(2)
#define TRB_ISP         BIT(10)
#define TRB_IOC         BIT(11)
#define TRB_TYPE(c)     (((c) >> 4) & 0x3f)
#define TRB_SIZE(sz)    ((sz) & 0xffffff)
enum {
    TRB_SETUP = 2, TRB_STATUS2 = 3, TRB_STATUS3 = 4, TRB_DATA = 5,
    TRB_LINK = 8,
};

typedef struct Trb {
    uint64_t addr;                      /* where it is */
    uint64_t buf;
    uint32_t size, ctrl;
} Trb;

static const uint32_t ghwparams[8] = {
    0x4020400a,     /* DRD, AXI, 64-bit addresses */
    0x01008000,     /* EN_PWROPT 1, one interrupt */
    0,
    0x08210045,     /* 16 endpoints (8 IN), UTMI 16-bit, SuperSpeed Gen1 */
    0, 0,
    0,              /* (not an FPGA) */
    0x02850800,
};

static void dma_rd(uint64_t a, void *buf, int len)
{
    dma_memory_read(&address_space_memory, a, buf, len,
                    MEMTXATTRS_UNSPECIFIED);
}

static void dma_wr(uint64_t a, const void *buf, int len)
{
    if (len) {
        dma_memory_write(&address_space_memory, a, buf, len,
                         MEMTXATTRS_UNSPECIFIED);
    }
}

/* ------------------------------------------------ interrupt and events */

static bool dwc3_device_role(RkDwc3State *s)
{
    return GCTL_PRTCAPDIR(s->regs[R(GCTL)]) == PRTCAP_DEVICE;
}

static void dwc3_update_irq(RkDwc3State *s)
{
    bool dev = dwc3_device_role(s) && s->ev_count &&
               !(s->regs[R(GEVNTSIZ)] & GEVNTSIZ_INTMASK);

    qemu_set_irq(s->irq, s->xhci_irq || dev);
}

static void dwc3_xhci_irq(void *opaque, int n, int level)
{
    RkDwc3State *s = opaque;

    s->xhci_irq = level;
    dwc3_update_irq(s);
}

/* the events waiting, into the event buffer as far as it has room */
static void dwc3_events_flush(RkDwc3State *s)
{
    uint64_t base = (uint64_t)s->regs[R(GEVNTADRHI)] << 32 |
                    s->regs[R(GEVNTADRLO)];
    uint32_t size = s->regs[R(GEVNTSIZ)] & 0xfffc;
    int i = 0;

    while (i < s->ev_nwait && size && s->ev_count + 4 <= size) {
        uint32_t w = cpu_to_le32(s->ev_wait[i++]);

        if (s->ev_off >= size) {
            s->ev_off = 0;
        }
        dma_wr(base + s->ev_off, &w, 4);
        s->ev_off = (s->ev_off + 4) % size;
        s->ev_count += 4;
    }
    if (i) {
        s->ev_nwait -= i;
        memmove(s->ev_wait, s->ev_wait + i, s->ev_nwait * sizeof(uint32_t));
    }
    dwc3_update_irq(s);
}

static void dwc3_event(RkDwc3State *s, uint32_t ev)
{
    if (s->ev_nwait == (int)ARRAY_SIZE(s->ev_wait)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rk-dwc3: an event lost (%08x): the "
                      "event buffer is not being emptied\n", ev);
        return;
    }
    s->ev_wait[s->ev_nwait++] = ev;
    dwc3_events_flush(s);
}

static void dwc3_dev_event(RkDwc3State *s, int type)
{
    if (s->regs[R(DEVTEN)] & BIT(type)) {
        dwc3_event(s, 1 | type << 8);
    }
}

/* endpoint n's event, if its configuration asks for that kind (a command's
 * completion: if the command did) */
static void dwc3_ep_event(RkDwc3State *s, int n, int type, int status,
                          int param)
{
    if (type == EPEVT_CMD_COMPLETE || (s->ep[n].events & BIT(7 + type))) {
        dwc3_event(s, n << 1 | type << 6 | status << 12 | param << 16);
    }
}

/* ------------------------------------------------- the device's state */

static bool dwc3_pulled_up(RkDwc3State *s)
{
    return dwc3_device_role(s) && (s->regs[R(DCTL)] & DCTL_RUN_STOP);
}

static void dwc3_ep0_idle(RkDwc3State *s)
{
    s->ctl = s->setup_held = s->ep0_stalled = false;
    s->ep[0].nrdy = s->ep[1].nrdy = false;
}

/* D+ may have been pulled up or let go: the host is told */
static void dwc3_pullup_check(RkDwc3State *s)
{
    bool up = dwc3_pulled_up(s);

    if (up == s->pulled) {
        return;
    }
    s->pulled = up;
    if (!up) {                          /* off the bus: no event for it */
        s->connected = false;
        s->addr = 0;
        dwc3_ep0_idle(s);
    }
    if (s->pullup_fn) {
        s->pullup_fn(s->pullup_opaque, up);
    }
}

/* DCTL's soft reset: the device role as new */
static void dwc3_device_reset(RkDwc3State *s)
{
    memset(s->ep, 0, sizeof(s->ep));
    s->regs[R(DALEPENA)] = 0;
    s->ev_off = s->ev_count = 0;
    s->ev_nwait = 0;
    s->connected = false;
    s->addr = 0;
    dwc3_ep0_idle(s);
}

/* ------------------------------------------------------------- TRBs */

/* the TRB at *@a, past any link TRBs (*@a: where it is); false if it is
 * not the hardware's (HWO clear) */
static bool trb_fetch(uint64_t *a, Trb *t)
{
    for (int links = 0; links < 4; links++) {
        uint32_t w[3], ctrl;

        dma_rd(*a + 12, &ctrl, 4);      /* (the driver writes it last) */
        ctrl = le32_to_cpu(ctrl);
        smp_rmb();
        if (TRB_TYPE(ctrl) == TRB_LINK) {
            dma_rd(*a, w, 8);
            *a = ((uint64_t)le32_to_cpu(w[1]) << 32 | le32_to_cpu(w[0])) &
                 ~0xfull;
            continue;
        }
        if (!(ctrl & TRB_HWO)) {
            return false;
        }
        dma_rd(*a, w, 12);
        t->addr = *a;
        t->buf = (uint64_t)le32_to_cpu(w[1]) << 32 | le32_to_cpu(w[0]);
        t->size = le32_to_cpu(w[2]);
        t->ctrl = ctrl;
        return true;
    }
    return false;
}

/* TRB t done with, @left of its bytes not moved: its size and control
 * written back (never its buffer's address: a SETUP packet goes there) */
static void trb_writeback(Trb *t, uint32_t left)
{
    uint32_t w;

    w = cpu_to_le32((t->size & 0x0f000000) | left);     /* TRBSTS: OK */
    dma_wr(t->addr + 8, &w, 4);
    smp_wmb();
    w = cpu_to_le32(t->ctrl & ~TRB_HWO);
    dma_wr(t->addr + 12, &w, 4);
}

/* endpoint n is done with TRB t, @left of its bytes not moved; @shrt: a
 * short packet ended it, and with it the rest of its chain, which is
 * skipped untouched (the driver clears those TRBs itself) */
static void ep_trb_finish(RkDwc3State *s, int n, Trb *t, uint32_t left,
                          bool shrt)
{
    RkDwc3Ep *ep = &s->ep[n];
    uint32_t last = t->ctrl;            /* the chain's last TRB's control */

    trb_writeback(t, left);
    ep->cur = t->addr + 16;
    ep->done = 0;
    if (shrt) {
        uint64_t a = ep->cur;
        Trb c = *t;

        while ((c.ctrl & TRB_CHN) && trb_fetch(&a, &c)) {
            last = c.ctrl;
            a = c.addr + 16;
        }
        ep->cur = a;
    }
    if (n < 2) {                        /* control: the chain's end ends the
                                           transfer */
        if (shrt || !(t->ctrl & TRB_CHN)) {
            ep->started = false;
            dwc3_ep_event(s, n, EPEVT_XFER_COMPLETE,
                          (shrt ? EPST_SHORT : 0) |
                          (last & TRB_IOC ? EPST_IOC : 0) |
                          (last & TRB_LST ? EPST_LST : 0), 0);
        }
    } else if ((shrt || !(t->ctrl & TRB_CHN)) && (last & TRB_LST)) {
        /* the transfer's last TRB (LST): the transfer ends, as U-Boot's
         * driver has it, a transfer for each request (Linux's sets LST on
         * a stream's only, and keeps a transfer going) */
        ep->started = false;
        dwc3_ep_event(s, n, EPEVT_XFER_COMPLETE,
                      (shrt ? EPST_SHORT : 0) |
                      ((last | t->ctrl) & TRB_IOC ? EPST_IOC : 0) | EPST_LST,
                      0);
    } else if (shrt) {
        if ((last | t->ctrl) & (TRB_IOC | TRB_ISP)) {
            dwc3_ep_event(s, n, EPEVT_XFER_IN_PROGRESS, EPST_SHORT |
                          ((last | t->ctrl) & TRB_IOC ? EPST_IOC : 0), 0);
        }
    } else if (t->ctrl & TRB_IOC) {
        dwc3_ep_event(s, n, EPEVT_XFER_IN_PROGRESS, EPST_IOC, 0);
    }
}

/* an OUT packet into endpoint n's transfer: ACK, or NAK while it has no
 * TRB for it */
static FoUsbResult ep_rx(RkDwc3State *s, int n, const uint8_t *data, int len)
{
    RkDwc3Ep *ep = &s->ep[n];
    bool shrt = len < (ep->mps ?: 64);
    uint64_t a = ep->cur;
    uint32_t size = 0;
    int off = 0;
    Trb t;

    if (!trb_fetch(&a, &t)) {
        return FO_USB_NAK;
    }
    for (;;) {
        uint32_t from, take;

        ep->cur = t.addr;
        size = TRB_SIZE(t.size);
        from = MIN(ep->done, size);
        take = MIN(size - from, (uint32_t)(len - off));
        dma_wr(t.buf + from, data + off, take);
        ep->done = from + take;
        off += take;
        if (off < len || (!shrt && ep->done == size)) {
            bool chn = t.ctrl & TRB_CHN;

            ep_trb_finish(s, n, &t, 0, false);      /* it is full */
            if (off == len || !chn) {
                return FO_USB_ACK;      /* (!chn: what is more than its
                                           transfer takes is lost) */
            }
            a = ep->cur;
            if (!trb_fetch(&a, &t)) {
                return FO_USB_ACK;      /* (lost too) */
            }
            continue;
        }
        break;
    }
    if (shrt) {
        ep_trb_finish(s, n, &t, size - ep->done, true);
    }
    return FO_USB_ACK;
}

/* an IN packet from endpoint n's transfer, into @data (*@len bytes): ACK,
 * or NAK while it has none ready. A packet is the endpoint's packet size,
 * through chained TRBs, or less where a chain ends; a TRB of 0 bytes is a
 * zero-length packet. */
static FoUsbResult ep_tx(RkDwc3State *s, int n, uint8_t *data, int *len)
{
    RkDwc3Ep *ep = &s->ep[n];
    uint32_t mps = MIN(ep->mps ?: 64, FO_USB_MAX_PACKET);
    struct {
        Trb t;
        uint32_t from, take;
    } seg[8];
    uint64_t a = ep->cur;
    uint32_t done = ep->done, pkt = 0;
    int nseg = 0, off = 0;

    while (nseg < (int)ARRAY_SIZE(seg)) {
        Trb *t = &seg[nseg].t;
        uint32_t size, from, take;

        if (!trb_fetch(&a, t)) {
            return FO_USB_NAK;          /* nothing yet, or not all of it */
        }
        size = TRB_SIZE(t->size);
        from = MIN(done, size);
        take = MIN(size - from, mps - pkt);
        seg[nseg].from = from;
        seg[nseg++].take = take;
        pkt += take;
        if (from + take < size || pkt == mps || !(t->ctrl & TRB_CHN)) {
            break;
        }
        a = t->addr + 16;
        done = 0;
    }
    for (int i = 0; i < nseg; i++) {
        dma_rd(seg[i].t.buf + seg[i].from, data + off, seg[i].take);
        off += seg[i].take;
    }
    *len = pkt;
    for (int i = 0; i < nseg; i++) {
        Trb *t = &seg[i].t;

        if (seg[i].from + seg[i].take == TRB_SIZE(t->size)) {
            ep_trb_finish(s, n, t, 0, false);
        } else {
            ep->cur = t->addr;
            ep->done = seg[i].from + seg[i].take;
        }
    }
    return FO_USB_ACK;
}

/* ------------------------------------------------- the control endpoint */

/* the SETUP held, into the control endpoint's SETUP TRB (the first TRB of
 * its last transfer), if it has one */
static void ep0_give_setup(RkDwc3State *s)
{
    RkDwc3Ep *ep = &s->ep[0];
    uint64_t a = ep->start;
    Trb t;

    /* (the TRB is looked at even if the transfer has ended: after a stall
     * in an OUT data or status stage, the driver writes a SETUP TRB where
     * its transfer started, and starts none) */
    if (!s->setup_held || !a || !(s->regs[R(DALEPENA)] & BIT(0)) ||
        !trb_fetch(&a, &t) || TRB_TYPE(t.ctrl) != TRB_SETUP) {
        return;
    }
    s->setup_held = false;
    s->ep0_stalled = false;             /* (a stall before it was the last
                                           transfer's) */
    dma_wr(t.buf, s->setup, 8);
    ep_trb_finish(s, 0, &t, TRB_SIZE(t.size) - MIN(TRB_SIZE(t.size), 8),
                  false);
}

/* an IN or OUT token on endpoint 0: the control transfer's data stage if
 * it goes the SETUP's way and the SETUP asked for data, else its status
 * stage */
static FoUsbResult ep0_token(RkDwc3State *s, bool in, const uint8_t *out,
                             int len, uint8_t *data, int *got)
{
    int n = in;
    RkDwc3Ep *ep = &s->ep[n];
    bool data_stage = s->ctl_len && in == s->ctl_in;
    uint64_t a = ep->cur;
    FoUsbResult r;
    Trb t;

    if (s->setup_held) {
        return FO_USB_NAK;              /* the driver has not seen the SETUP
                                           yet: nothing to tell it */
    }
    if (s->ep0_stalled) {
        return FO_USB_STALL;
    }
    if (!s->ctl) {
        return FO_USB_NAK;
    }
    if (ep->started && (s->regs[R(DALEPENA)] & BIT(n)) &&
        trb_fetch(&a, &t) &&
        (data_stage ? TRB_TYPE(t.ctrl) == TRB_DATA :
         (TRB_TYPE(t.ctrl) == TRB_STATUS2 ||
          TRB_TYPE(t.ctrl) == TRB_STATUS3))) {
        r = in ? ep_tx(s, n, data, got) : ep_rx(s, n, out, len);
        if (r == FO_USB_ACK && !ep->started) {
            if (data_stage) {
                s->ctl_data_done = true;
            } else {
                s->ctl = false;         /* its status stage is done */
                if (s->ctl_set_address) {
                    s->addr = DCFG_DEVADDR(s->regs[R(DCFG)]);
                }
            }
        }
        return r;
    }
    if (!ep->nrdy) {                    /* the driver is told, once */
        ep->nrdy = true;
        dwc3_ep_event(s, n, EPEVT_XFER_NOT_READY,
                      data_stage ? NRDY_DATA : NRDY_STATUS, 0);
    }
    return FO_USB_NAK;
}

/* an IN or OUT token on another endpoint (n: the physical one) */
static FoUsbResult ep_token(RkDwc3State *s, int n, const uint8_t *out,
                            int len, uint8_t *data, int *got)
{
    RkDwc3Ep *ep = &s->ep[n];

    if (!(s->regs[R(DALEPENA)] & BIT(n))) {
        return FO_USB_NAK;
    }
    if (ep->stalled) {
        return FO_USB_STALL;
    }
    if (!ep->started) {
        /* no transfer for it: the driver is told, once, if it asked for
         * that (DEPCFG). Linux's starts a transfer as its gadget queues a
         * request, and does not ask; U-Boot's starts one only then, and
         * would leave the host NAKed for good */
        if (!ep->nrdy) {
            ep->nrdy = true;
            dwc3_ep_event(s, n, EPEVT_XFER_NOT_READY, 0, 0);
        }
        return FO_USB_NAK;
    }
    return n & 1 ? ep_tx(s, n, data, got) : ep_rx(s, n, out, len);
}

/* ------------------------------------------------ as the host drives it */

static bool dwc3_op_pulled_up(void *opaque)
{
    return dwc3_pulled_up(opaque);
}

static bool dwc3_op_irq_pending(void *opaque)
{
    RkDwc3State *s = opaque;

    /* (the driver's interrupt handler masks the interrupt and takes the
     * events at once; its thread then deals with them, and unmasks it. A
     * SETUP held waits for the driver to give it a TRB.) */
    return s->ev_count || s->ev_nwait || s->setup_held ||
           (s->regs[R(GEVNTSIZ)] & GEVNTSIZ_INTMASK);
}

static void dwc3_op_bus_reset(void *opaque)
{
    RkDwc3State *s = opaque;

    if (!dwc3_pulled_up(s)) {
        return;
    }
    s->connected = true;
    s->addr = 0;
    dwc3_ep0_idle(s);
    dwc3_dev_event(s, DEVT_RESET);
    dwc3_dev_event(s, DEVT_CONNECT_DONE);
}

static void dwc3_op_detach(void *opaque)
{
    RkDwc3State *s = opaque;
    int st = s->ctl_len && s->ctl_in ? 0 : 1;   /* the status stage's way */

    if (!s->connected) {
        return;
    }
    /* a control transfer left before its status stage: as if the host had
     * asked for it before it went, so that the driver, which waits for
     * that, ends the transfer and is ready for a SETUP again (else it
     * never is: it gives the control endpoint no SETUP TRB) */
    if (s->ctl && !s->setup_held && (!s->ctl_len || s->ctl_data_done) &&
        !s->ep[st].nrdy) {
        dwc3_ep_event(s, st, EPEVT_XFER_NOT_READY, NRDY_STATUS, 0);
    }
    s->connected = false;
    s->addr = 0;
    dwc3_ep0_idle(s);
    dwc3_dev_event(s, DEVT_DISCONNECT);
}

static FoUsbResult dwc3_op_setup(void *opaque, uint8_t addr,
                                 const uint8_t pkt[8])
{
    RkDwc3State *s = opaque;

    if (!s->connected || addr != s->addr) {
        return FO_USB_TIMEOUT;
    }
    memcpy(s->setup, pkt, 8);
    s->ctl = true;
    s->ctl_data_done = false;
    s->ctl_in = pkt[0] & 0x80;
    s->ctl_len = lduw_le_p(pkt + 6);
    s->ctl_set_address = pkt[0] == 0x00 && pkt[1] == 5;
    s->ep0_stalled = false;
    s->ep[0].nrdy = s->ep[1].nrdy = false;
    s->setup_held = true;
    ep0_give_setup(s);
    return FO_USB_ACK;
}

static FoUsbResult dwc3_op_out(void *opaque, uint8_t addr, int ep,
                               const uint8_t *data, int len)
{
    RkDwc3State *s = opaque;

    if (!s->connected || addr != s->addr || ep < 0 || ep > 15) {
        return FO_USB_TIMEOUT;
    }
    return ep ? ep_token(s, 2 * ep, data, len, NULL, NULL) :
                ep0_token(s, false, data, len, NULL, NULL);
}

static FoUsbResult dwc3_op_in(void *opaque, uint8_t addr, int ep,
                              uint8_t *data, int *len)
{
    RkDwc3State *s = opaque;

    *len = 0;
    if (!s->connected || addr != s->addr || ep < 0 || ep > 15) {
        return FO_USB_TIMEOUT;
    }
    return ep ? ep_token(s, 2 * ep + 1, NULL, 0, data, len) :
                ep0_token(s, true, NULL, 0, data, len);
}

const FoUsbDevOps rk_dwc3_usb_ops = {
    .speed = 3,                         /* high speed */
    .pulled_up = dwc3_op_pulled_up,
    .irq_pending = dwc3_op_irq_pending,
    .bus_reset = dwc3_op_bus_reset,
    .detach = dwc3_op_detach,
    .setup = dwc3_op_setup,
    .out = dwc3_op_out,
    .in = dwc3_op_in,
};

/* -------------------------------------------------------- the registers */

/* endpoint n's command, done as it is written */
static void dwc3_depcmd(RkDwc3State *s, int n, uint32_t cmd)
{
    RkDwc3Ep *ep = &s->ep[n];
    uint32_t p0 = s->regs[R(DEPCMDPAR0(n))], p1 = s->regs[R(DEPCMDPAR1(n))];

    switch (cmd & 0xf) {
    case EPCMD_SET_CONFIG:
        ep->mps = (p0 >> 3) & 0x7ff;
        if (p0 >> 30 != 2) {            /* (not a MODIFY: from the start) */
            ep->events = p1;
            ep->stalled = false;
            ep->nrdy = false;
        }
        break;
    case EPCMD_SET_STALL:
        ep->stalled = true;
        if (n < 2) {
            s->ep0_stalled = true;      /* the control pipe, until a SETUP */
        }
        break;
    case EPCMD_CLEAR_STALL:
        ep->stalled = false;
        break;
    case EPCMD_START:
        ep->start = ep->cur = (uint64_t)p0 << 32 | p1;
        ep->done = 0;
        ep->started = true;
        ep->nrdy = false;
        if (n == 0) {
            ep0_give_setup(s);
        }
        break;
    case EPCMD_END:
        ep->started = false;
        ep->nrdy = false;
        break;
    }
    /* done: its resource index, the endpoint's number */
    s->regs[R(DEPCMD(n))] = (cmd & ~(CMDACT | CMDSTATUS | CMDRSCIDX)) |
                            n << 16;
    if (cmd & CMDIOC) {
        dwc3_ep_event(s, n, EPEVT_CMD_COMPLETE, 0, (cmd & 0xf) << 8);
    }
}

static uint64_t dwc3_read(void *opaque, hwaddr off, unsigned size)
{
    RkDwc3State *s = opaque;
    hwaddr a = off + REGS_BASE;

    switch (a) {
    case GSNPSID:
        return 0x5533330a;
    case GHWPARAMS8:
        return 0;
    case GEVNTCOUNT:
        return s->ev_count;
    case DSTS:                          /* (CONNECTSPD 0: high speed) */
        return DSTS_RXFIFOEMPTY | DSTS_USBLNKST(s->connected ? 0 : 4) |
               (s->regs[R(DCTL)] & DCTL_RUN_STOP ? 0 : DSTS_DEVCTRLHLT);
    case DGCMD:
        return s->regs[R(a)] & ~(CMDACT | CMDSTATUS);
    }
    if (a >= GHWPARAMS0 && a < GHWPARAMS0 + sizeof(ghwparams)) {
        return ghwparams[(a - GHWPARAMS0) / 4];
    }
    return s->regs[R(a)];
}

static void dwc3_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkDwc3State *s = opaque;
    hwaddr a = off + REGS_BASE;

    if (a == GSNPSID || a == GHWPARAMS8 || a == DSTS ||
        (a >= GHWPARAMS0 && a < GHWPARAMS0 + sizeof(ghwparams))) {
        return;                                 /* (read-only) */
    }
    switch (a) {
    case GEVNTCOUNT:                    /* the bytes the driver has taken */
        s->ev_count -= MIN(s->ev_count, (uint32_t)v & 0xfffc);
        dwc3_events_flush(s);
        return;
    case GEVNTADRLO:
        s->regs[R(a)] = v;
        s->ev_off = 0;
        return;
    case GEVNTSIZ:
        s->regs[R(a)] = v;
        dwc3_events_flush(s);           /* (and the interrupt's mask) */
        return;
    case GCTL:
        s->regs[R(a)] = v;
        dwc3_pullup_check(s);
        dwc3_update_irq(s);
        return;
    case DCTL:
        if (v & DCTL_CSFTRST) {         /* done at once */
            dwc3_device_reset(s);
            v &= ~(DCTL_CSFTRST | DCTL_RUN_STOP);
        }
        s->regs[R(a)] = v;
        dwc3_pullup_check(s);
        dwc3_update_irq(s);
        return;
    case DGCMD:
        s->regs[R(a)] = v;              /* (done at once) */
        return;
    }
    s->regs[R(a)] = v;
    if (a >= DEPCMD_BASE && a < DEPCMD_BASE + 32 * 0x10 && (a & 0xf) == 0xc) {
        dwc3_depcmd(s, (a - DEPCMD_BASE) / 0x10, v);
    }
}

static const MemoryRegionOps dwc3_ops = {
    .read = dwc3_read,
    .write = dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* what nothing is at: reads 0, takes writes */
static uint64_t quiet_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static void quiet_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps quiet_ops = {
    .read = quiet_read,
    .write = quiet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void dwc3_reset(DeviceState *dev)
{
    RkDwc3State *s = RK_DWC3(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(GUSB2PHYCFG)] = 0x40102400;
    s->regs[R(GUSB3PIPECTL)] = 0x010c0002;
    s->regs[R(GTXFIFOSIZ(0))] = 0x0042;
    for (int n = 1; n < 8; n++) {
        s->regs[R(GTXFIFOSIZ(n))] = 0x0184;     /* (max packet 1024) */
    }
    s->regs[R(GRXFIFOSIZ0)] = 0x0285;
    dwc3_device_reset(s);
    dwc3_pullup_check(s);
    dwc3_update_irq(s);
}

static void dwc3_init(Object *obj)
{
    RkDwc3State *s = RK_DWC3(obj);

    object_initialize_child(obj, "xhci", &s->xhci, TYPE_XHCI_SYSBUS);
}

static void dwc3_realize(DeviceState *dev, Error **errp)
{
    RkDwc3State *s = RK_DWC3(dev);
    SysBusDevice *xhci = SYS_BUS_DEVICE(&s->xhci);

    /* (sysbus-xhci's defaults for intrs and slots do not survive its
     * core's initialisation, which zeroes them: both are set here) */
    qdev_prop_set_uint32(DEVICE(xhci), "p2", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "p3", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "intrs", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "slots", 64);
    qdev_prop_set_bit(DEVICE(xhci), "erstba-hi-lo", true);    /* (0011) */
    if (!sysbus_realize(xhci, errp)) {
        return;
    }
    memory_region_init(&s->mr, OBJECT(s), "rk-dwc3", 4 * MiB);
    memory_region_init_io(&s->quiet, OBJECT(s), &quiet_ops, s,
                          "rk-dwc3.none", 4 * MiB);
    memory_region_add_subregion_overlap(&s->mr, 0, &s->quiet, -1);
    memory_region_add_subregion(&s->mr, 0, sysbus_mmio_get_region(xhci, 0));
    memory_region_init_io(&s->regs_mr, OBJECT(s), &dwc3_ops, s,
                          "rk-dwc3.regs", sizeof(s->regs));
    memory_region_add_subregion(&s->mr, REGS_BASE, &s->regs_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_in_named(dev, dwc3_xhci_irq, "xhci-irq", 1);
    sysbus_connect_irq(xhci, 0, qdev_get_gpio_in_named(dev, "xhci-irq", 0));
}

static void dwc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = dwc3_realize;
    device_class_set_legacy_reset(dc, dwc3_reset);
}

static const TypeInfo dwc3_types[] = {
    {
        .name = TYPE_RK_DWC3,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkDwc3State),
        .instance_init = dwc3_init,
        .class_init = dwc3_class_init,
    },
};

DEFINE_TYPES(dwc3_types)
