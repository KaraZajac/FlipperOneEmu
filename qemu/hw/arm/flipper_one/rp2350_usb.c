/*
 * The RP2350's USB controller (USBCTRL), as a device, and what a host on the
 * other end of its cable can do with it (usb_host.c)
 *
 * The registers (0x50110000; the atomic aliases as the chip's, the
 * write-clear ones taking the written mask whichever alias it came by) and
 * the 4 KiB DPRAM (0x50100000, RAM that the CPU and the controller share):
 * the SETUP packet in its first 8 bytes; each endpoint's control
 * (EPx_IN/OUT_CONTROL: enabled, double-buffered, an interrupt a buffer or a
 * double buffer, its buffer's offset); each endpoint's buffer control, two
 * halves for buffers 0 and 1 (FULL, LAST, DATA1, AVAILABLE, the length;
 * STALL and RESET, which puts the buffer selector back to 0, in the low
 * half); EP0's buffer at 0x100. The CPU writes the DPRAM as memory, so the
 * controller sees a buffer control when the endpoint's next transaction
 * comes: RESET (TinyUSB's SEL, at every arming) is taken then.
 *
 * In device mode (MAIN_CTRL: CONTROLLER_EN, not HOST_NDEVICE) a host's
 * transaction on an endpoint at the device's address takes or gives the
 * buffer that the endpoint's selector points at, if the CPU made it
 * AVAILABLE (an IN one FULL with its data, an OUT one empty): it clears
 * AVAILABLE, sets the length and FULL (OUT) or clears FULL (IN), moves the
 * selector on if the endpoint is double-buffered, and sets the endpoint's
 * BUFF_STATUS bit (EP0's if SIE_CTRL asks for it). A buffer not available
 * NAKs (EP_STATUS_STALL_NAK, if asked for); STALL set (on EP0, armed in
 * EP_STALL_ARM too) stalls. A SETUP is always taken: into the DPRAM,
 * SETUP_REC, EP0's stall disarmed. A bus reset sets BUS_RESET and clears
 * the address. EP_ABORT is done at once (EP_ABORT_DONE). USBCTRL_IRQ (14)
 * is INTR & INTE | INTF; INTR comes from SIE_STATUS, BUFF_STATUS,
 * EP_ABORT_DONE, EP_STATUS_STALL_NAK, and the changes of CONNECTED and
 * SUSPENDED, latched until those bits of SIE_STATUS are written.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "rp2350.h"

#define IRQ_USBCTRL         14
#define USB_REGS_BASE       0x50110000

enum {
    R_ADDR_ENDP = 0x00, R_MAIN_CTRL = 0x40, R_SOF_WR = 0x44, R_SOF_RD = 0x48,
    R_SIE_CTRL = 0x4c, R_SIE_STATUS = 0x50, R_INT_EP_CTRL = 0x54,
    R_BUFF_STATUS = 0x58, R_BUFF_CPU_SHOULD_HANDLE = 0x5c, R_EP_ABORT = 0x60,
    R_EP_ABORT_DONE = 0x64, R_EP_STALL_ARM = 0x68, R_NAK_POLL = 0x6c,
    R_EP_STATUS_STALL_NAK = 0x70, R_USB_MUXING = 0x74, R_USB_PWR = 0x78,
    R_INTR = 0x8c, R_INTE = 0x90, R_INTF = 0x94, R_INTS = 0x98,
};

#define MAIN_CONTROLLER_EN  0x01
#define MAIN_HOST_NDEVICE   0x02
#define SIE_CTRL_EP0_INT_1BUF 0x20000000
#define SIE_CTRL_EP0_DOUBLE_BUF 0x40000000
#define SIE_CTRL_PULLUP_EN  0x00010000
#define SIE_CTRL_RESUME     0x00001000
#define ST_SETUP_REC        0x00020000
#define ST_BUS_RESET        0x00080000
#define ST_TRANS_COMPLETE   0x00040000
#define ST_CONNECTED        0x00010000
#define ST_RESUME           0x00000800
#define ST_SUSPENDED        0x00000010
#define ST_VBUS_DETECTED    0x00000001
#define ST_SPEED_FULL       0x00000200
#define ST_WC               0xff8e1800      /* the write-clear bits */
#define MUX_TO_PHY          0x01
#define PWR_VBUS_DETECT     0x04
#define PWR_VBUS_DETECT_OVR 0x08

#define INTR_TRANS_COMPLETE 0x00000008
#define INTR_BUFF_STATUS    0x00000010
#define INTR_STALL          0x00000400
#define INTR_VBUS_DETECT    0x00000800
#define INTR_BUS_RESET      0x00001000
#define INTR_DEV_CONN_DIS   0x00002000
#define INTR_DEV_SUSPEND    0x00004000
#define INTR_DEV_RESUME     0x00008000
#define INTR_SETUP_REQ      0x00010000
#define INTR_ABORT_DONE     0x00040000
#define INTR_EP_STALL_NAK   0x00080000

/* EPx_IN/OUT_CONTROL */
#define EPC_ENABLE          0x80000000
#define EPC_DOUBLE          0x40000000
#define EPC_INT_PER_BUFF    0x20000000
#define EPC_INT_PER_DOUBLE  0x10000000
#define EPC_INT_ON_NAK      0x00010000
#define EPC_INT_ON_STALL    0x00020000
#define EPC_ADDR            0x0000ffc0

/* a half of a buffer control */
#define BUF_LEN             0x03ff
#define BUF_AVAIL           0x0400
#define BUF_STALL           0x0800          /* the low half's */
#define BUF_RESET           0x1000          /* the low half's */
#define BUF_DATA1           0x2000
#define BUF_LAST            0x4000
#define BUF_FULL            0x8000

#define DP_EP_CTRL(ep, out) (0x08 * (ep) + ((out) ? 4 : 0))     /* ep >= 1 */
#define DP_BUF_CTRL(ep, out) (0x80 + 0x08 * (ep) + ((out) ? 4 : 0))
#define DP_EP0_BUF          0x100

static uint8_t *usb_dp(RP2350State *s)
{
    return memory_region_get_ram_ptr(&s->usb_dpram);
}

static uint32_t dp_read32(RP2350State *s, uint32_t off)
{
    return ldl_le_p(usb_dp(s) + off);
}

static void dp_write32(RP2350State *s, uint32_t off, uint32_t v)
{
    stl_le_p(usb_dp(s) + off, v);
    memory_region_set_dirty(&s->usb_dpram, off, 4);
}

static uint32_t usb_intr(RP2350State *s)
{
    RP2Usb *u = &s->usb;
    uint32_t st = u->sie_status, v = 0;

    v |= st & ST_TRANS_COMPLETE ? INTR_TRANS_COMPLETE : 0;
    v |= u->buff_status ? INTR_BUFF_STATUS : 0;
    v |= u->ep_status_stall_nak ? INTR_EP_STALL_NAK : 0;
    v |= u->ep_abort_done ? INTR_ABORT_DONE : 0;
    v |= st & ST_BUS_RESET ? INTR_BUS_RESET : 0;
    v |= st & ST_SETUP_REC ? INTR_SETUP_REQ : 0;
    v |= st & ST_RESUME ? INTR_DEV_RESUME : 0;
    v |= u->conn_changed ? INTR_DEV_CONN_DIS : 0;
    v |= u->susp_changed ? INTR_DEV_SUSPEND : 0;
    v |= u->vbus_changed ? INTR_VBUS_DETECT : 0;
    return v;
}

static void usb_irq(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    qemu_set_irq(u->irq, ((usb_intr(s) & u->inte) | u->intf) != 0);
}

static uint32_t usb_sie_status(RP2350State *s)
{
    RP2Usb *u = &s->usb;
    bool vbus = u->vbus || ((u->pwr & PWR_VBUS_DETECT_OVR) &&
                            (u->pwr & PWR_VBUS_DETECT));

    return (u->sie_status & ~(ST_CONNECTED | ST_SUSPENDED | ST_VBUS_DETECTED)) |
           (u->connected ? ST_CONNECTED | ST_SPEED_FULL : 0) |
           (u->suspended ? ST_SUSPENDED : 0) | (vbus ? ST_VBUS_DETECTED : 0);
}

bool rp2350_usb_irq_pending(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    return ((usb_intr(s) & u->inte) | u->intf) != 0;
}

bool rp2350_usb_pulled_up(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    return (u->main_ctrl & (MAIN_CONTROLLER_EN | MAIN_HOST_NDEVICE)) ==
           MAIN_CONTROLLER_EN && (u->sie_ctrl & SIE_CTRL_PULLUP_EN) &&
           (u->muxing & MUX_TO_PHY);
}

/* the pull-up may have come or gone: the host is told */
static void usb_pullup_check(RP2350State *s)
{
    RP2Usb *u = &s->usb;
    bool up = rp2350_usb_pulled_up(s);

    if (up != u->pulled_up) {
        u->pulled_up = up;
        if (u->pullup_fn) {
            u->pullup_fn(u->pullup_opaque, up);
        }
    }
}

void rp2350_usb_vbus(RP2350State *s, bool vbus)
{
    RP2Usb *u = &s->usb;

    if (vbus != u->vbus) {
        u->vbus = vbus;
        u->vbus_changed = true;
        usb_irq(s);
    }
}

void rp2350_usb_bus_reset(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    u->addr = 0;
    u->sie_status |= ST_BUS_RESET;
    if (!u->connected) {
        u->connected = true;
        u->conn_changed = true;
    }
    if (u->suspended) {
        u->suspended = false;
        u->susp_changed = true;
    }
    memset(u->next_buf, 0, sizeof(u->next_buf));
    usb_irq(s);
}

void rp2350_usb_detach(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    if (u->connected) {
        u->connected = false;
        u->conn_changed = true;
        usb_irq(s);
    }
}

void rp2350_usb_suspend(RP2350State *s, bool suspended)
{
    RP2Usb *u = &s->usb;

    if (suspended != u->suspended) {
        u->suspended = suspended;
        u->susp_changed = true;
        if (!suspended) {
            u->sie_status |= ST_RESUME;
        }
        usb_irq(s);
    }
}

static bool usb_device_mode(RP2350State *s, uint8_t addr)
{
    RP2Usb *u = &s->usb;

    return rp2350_usb_pulled_up(s) && u->connected && addr == (u->addr & 0x7f);
}

/* an endpoint's buffer, for a transaction: where its data is, and which
 * half of its buffer control */
typedef struct UsbBuf {
    uint32_t ctrl_off;              /* the buffer control's DPRAM offset */
    uint32_t data_off;
    int half;                       /* 0, 1 */
    bool double_buf;
    uint32_t epc;                   /* its endpoint control (EP0: made up) */
} UsbBuf;

static bool usb_buf(RP2350State *s, int ep, bool out, UsbBuf *b)
{
    RP2Usb *u = &s->usb;
    int sel = ep * 2 + out;

    if (ep == 0) {
        b->double_buf = u->sie_ctrl & SIE_CTRL_EP0_DOUBLE_BUF;
        b->epc = EPC_ENABLE | (u->sie_ctrl & SIE_CTRL_EP0_INT_1BUF ?
                               EPC_INT_PER_BUFF : 0);
        b->data_off = DP_EP0_BUF;
    } else {
        b->epc = dp_read32(s, DP_EP_CTRL(ep, out));
        if (!(b->epc & EPC_ENABLE)) {
            return false;
        }
        b->double_buf = b->epc & EPC_DOUBLE;
        b->data_off = b->epc & EPC_ADDR;
    }
    b->ctrl_off = DP_BUF_CTRL(ep, out);
    if (dp_read32(s, b->ctrl_off) & BUF_RESET) {
        /* armed again (SEL): from buffer 0; the bit is a command */
        u->next_buf[sel] = 0;
        dp_write32(s, b->ctrl_off, dp_read32(s, b->ctrl_off) & ~BUF_RESET);
    }
    b->half = b->double_buf ? u->next_buf[sel] : 0;
    if (b->half) {
        b->data_off += 64;
    }
    return true;
}

/* a buffer went: its BUFF_STATUS bit, the selector on */
static void usb_buf_done(RP2350State *s, int ep, bool out, UsbBuf *b)
{
    RP2Usb *u = &s->usb;
    int sel = ep * 2 + out;
    bool tell = b->epc & EPC_INT_PER_BUFF ||
                (b->epc & EPC_INT_PER_DOUBLE && (!b->double_buf || b->half));

    if (b->double_buf) {
        u->next_buf[sel] ^= 1;
    }
    if (tell) {
        u->buff_status |= 1u << sel;
    }
    usb_irq(s);
}

static uint16_t buf_half(uint32_t ctrl, int half)
{
    return half ? ctrl >> 16 : ctrl & 0xffff;
}

static uint32_t buf_set_half(uint32_t ctrl, int half, uint16_t v)
{
    return half ? (ctrl & 0xffff) | (uint32_t)v << 16
                : (ctrl & 0xffff0000) | v;
}

/* the endpoint stalls, or NAKs (and says so, if asked) */
static RP2UsbResult usb_refuse(RP2350State *s, int ep, bool out, UsbBuf *b,
                               uint32_t ctrl)
{
    RP2Usb *u = &s->usb;
    bool stall = ctrl & BUF_STALL;

    if (ep == 0) {
        stall = stall && (u->ep_stall_arm >> out) & 1;
    }
    if (stall) {
        if (ep == 0 || b->epc & EPC_INT_ON_STALL) {
            u->ep_status_stall_nak |= 1u << (ep * 2 + out);
            usb_irq(s);
        }
        return RP2_USB_STALL;
    }
    if ((ep == 0 && u->nak_int_ep0) || (ep && (b->epc & EPC_INT_ON_NAK))) {
        u->ep_status_stall_nak |= 1u << (ep * 2 + out);
        usb_irq(s);
    }
    return RP2_USB_NAK;
}

RP2UsbResult rp2350_usb_setup(RP2350State *s, uint8_t addr,
                              const uint8_t pkt[8])
{
    RP2Usb *u = &s->usb;

    if (!usb_device_mode(s, addr)) {
        return RP2_USB_TIMEOUT;
    }
    memcpy(usb_dp(s), pkt, 8);
    memory_region_set_dirty(&s->usb_dpram, 0, 8);
    u->sie_status |= ST_SETUP_REC;
    u->ep_stall_arm = 0;            /* a SETUP disarms EP0's stall */
    u->next_buf[0] = u->next_buf[1] = 0;
    usb_irq(s);
    return RP2_USB_ACK;
}

RP2UsbResult rp2350_usb_out(RP2350State *s, uint8_t addr, int ep,
                            const uint8_t *data, int len)
{
    UsbBuf b;
    uint32_t ctrl;
    uint16_t half;

    if (!usb_device_mode(s, addr) || ep < 0 || ep > 15) {
        return RP2_USB_TIMEOUT;
    }
    if (!usb_buf(s, ep, true, &b)) {
        return RP2_USB_TIMEOUT;
    }
    ctrl = dp_read32(s, b.ctrl_off);
    half = buf_half(ctrl, b.half);
    if ((ctrl & BUF_STALL) || !(half & BUF_AVAIL) || (half & BUF_FULL)) {
        return usb_refuse(s, ep, true, &b, ctrl);
    }
    len = MIN(len, MIN(half & BUF_LEN, 64));
    memcpy(usb_dp(s) + b.data_off, data, len);
    memory_region_set_dirty(&s->usb_dpram, b.data_off, MAX(len, 1));
    half = (half & ~(BUF_AVAIL | BUF_LEN)) | BUF_FULL | len;
    dp_write32(s, b.ctrl_off, buf_set_half(ctrl, b.half, half));
    usb_buf_done(s, ep, true, &b);
    return RP2_USB_ACK;
}

RP2UsbResult rp2350_usb_in(RP2350State *s, uint8_t addr, int ep,
                           uint8_t *data, int *len)
{
    UsbBuf b;
    uint32_t ctrl;
    uint16_t half;

    if (!usb_device_mode(s, addr) || ep < 0 || ep > 15) {
        return RP2_USB_TIMEOUT;
    }
    if (!usb_buf(s, ep, false, &b)) {
        return RP2_USB_TIMEOUT;
    }
    ctrl = dp_read32(s, b.ctrl_off);
    half = buf_half(ctrl, b.half);
    if ((ctrl & BUF_STALL) || !(half & BUF_AVAIL) || !(half & BUF_FULL)) {
        return usb_refuse(s, ep, false, &b, ctrl);
    }
    *len = MIN(half & BUF_LEN, 64);
    memcpy(data, usb_dp(s) + b.data_off, *len);
    half &= ~(BUF_AVAIL | BUF_FULL);
    dp_write32(s, b.ctrl_off, buf_set_half(ctrl, b.half, half));
    usb_buf_done(s, ep, false, &b);
    return RP2_USB_ACK;
}

/* ------------------------------------------------------------ registers */

static uint32_t usb_read(RP2Usb *u, RP2350State *s, hwaddr reg)
{
    switch (reg) {
    case R_ADDR_ENDP: return u->addr;
    case R_MAIN_CTRL: return u->main_ctrl;
    case R_SOF_RD: return u->sof & 0x7ff;
    case R_SIE_CTRL: return u->sie_ctrl;
    case R_SIE_STATUS: return usb_sie_status(s);
    case R_BUFF_STATUS: return u->buff_status;
    case R_BUFF_CPU_SHOULD_HANDLE: return 0;
    case R_EP_ABORT: return u->ep_abort;
    case R_EP_ABORT_DONE: return u->ep_abort_done;
    case R_EP_STALL_ARM: return u->ep_stall_arm;
    case R_NAK_POLL: return u->nak_poll;
    case R_EP_STATUS_STALL_NAK: return u->ep_status_stall_nak;
    case R_USB_MUXING: return u->muxing;
    case R_USB_PWR: return u->pwr;
    case R_INTR: return usb_intr(s);
    case R_INTE: return u->inte;
    case R_INTF: return u->intf;
    case R_INTS: return (usb_intr(s) & u->inte) | u->intf;
    default:
        return 0;
    }
}

/* a register's new value; @mask: what was written (for write-clear bits) */
static void usb_write(RP2Usb *u, RP2350State *s, hwaddr reg, uint32_t v,
                      uint32_t mask)
{
    switch (reg) {
    case R_ADDR_ENDP:
        u->addr = v & 0x000f007f;
        break;
    case R_MAIN_CTRL:
        u->main_ctrl = v & 0x80000007;
        break;
    case R_SIE_CTRL:
        u->sie_ctrl = v & ~SIE_CTRL_RESUME;     /* (remote wakeup: a pulse) */
        break;
    case R_SIE_STATUS:
        u->sie_status &= ~(mask & ST_WC);
        if (mask & ST_CONNECTED) {
            u->conn_changed = false;
        }
        if (mask & ST_SUSPENDED) {
            u->susp_changed = false;
        }
        if (mask & ST_VBUS_DETECTED) {
            u->vbus_changed = false;
        }
        break;
    case R_BUFF_STATUS:
        u->buff_status &= ~mask;
        break;
    case R_EP_ABORT:
        u->ep_abort = v;
        u->ep_abort_done |= v;                  /* nothing in flight */
        break;
    case R_EP_ABORT_DONE:
        u->ep_abort_done &= ~mask;
        break;
    case R_EP_STALL_ARM:
        u->ep_stall_arm = v & 3;
        break;
    case R_NAK_POLL:
        u->nak_poll = v;
        break;
    case R_EP_STATUS_STALL_NAK:
        u->ep_status_stall_nak &= ~mask;
        break;
    case R_USB_MUXING:
        u->muxing = v & 0x1f;
        break;
    case R_USB_PWR:
        u->pwr = v & 0x3f;
        break;
    case R_INTE:
        u->inte = v & 0x00ffffff;
        break;
    case R_INTF:
        u->intf = v & 0x00ffffff;
        break;
    case R_SOF_WR:
    default:
        break;
    }
    u->nak_int_ep0 = u->sie_ctrl & 0x08000000;  /* EP0_INT_NAK */
    usb_irq(s);
    usb_pullup_check(s);
}

static uint64_t usb_mr_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2350State *s = opaque;
    uint32_t v = usb_read(&s->usb, s, addr & 0xffc);

    return extract32(v, (addr & 3) * 8, size * 8);
}

static void usb_mr_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    RP2350State *s = opaque;
    RP2Usb *u = &s->usb;
    hwaddr reg = addr & 0xffc;
    uint32_t mask = value, v;

    if (size == 1) {
        mask = (mask & 0xff) * 0x01010101u;
    } else if (size == 2) {
        mask = (mask & 0xffff) * 0x00010001u;
    }
    v = mask;
    switch (addr >> 12) {
    case 1: v ^= usb_read(u, s, reg); break;    /* XOR */
    case 2: v |= usb_read(u, s, reg); break;    /* SET */
    case 3: v = usb_read(u, s, reg) & ~v; break;        /* CLR */
    }
    usb_write(u, s, reg, v, mask);
}

static const MemoryRegionOps usb_ops = {
    .read = usb_mr_read,
    .write = usb_mr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void rp2350_usb_realize(RP2350State *s)
{
    RP2Usb *u = &s->usb;

    memory_region_init_io(&u->mr, OBJECT(s), &usb_ops, s, "rp2350.usbctrl",
                          0x4000);
    u->mr.disable_reentrancy_guard = true;
    memory_region_add_subregion_overlap(&s->bus, USB_REGS_BASE, &u->mr, 1);
    u->irq = rp2350_irq(s, IRQ_USBCTRL);
}

void rp2350_usb_reset(RP2350State *s)
{
    RP2Usb *u = &s->usb;
    bool vbus = u->vbus;

    u->addr = u->main_ctrl = u->sie_ctrl = u->sie_status = 0;
    u->buff_status = u->ep_abort = u->ep_abort_done = 0;
    u->ep_stall_arm = u->ep_status_stall_nak = u->muxing = u->pwr = 0;
    u->nak_poll = 0x00100010;
    u->inte = u->intf = 0;
    u->connected = u->suspended = false;
    u->conn_changed = u->susp_changed = u->vbus_changed = false;
    u->nak_int_ep0 = false;
    u->vbus = vbus;
    memset(u->next_buf, 0, sizeof(u->next_buf));
    usb_pullup_check(s);
    qemu_set_irq(u->irq, 0);
}
