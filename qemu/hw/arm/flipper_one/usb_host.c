/*
 * A USB host on the other end of the Flipper One's cable: the PC its USB-C
 * port is plugged into
 *
 * QEMU has no USB wire that leaves the machine, so this is the host. The
 * cable's data lines reach one device controller (fo_usb.h), as the board
 * says: the MCU's (rp2350_usb.c), or, when the main board's USB mux has
 * them on the Linux side, the RK3576's USB0 (rk_dwc3.c). When the device
 * pulls D+ up, it waits 100 ms, resets the bus for 10 ms, and enumerates
 * what it finds as a PC would: the device descriptor at address 0,
 * SET_ADDRESS (1), the device descriptor again, the configuration, the
 * strings, SET_CONFIGURATION; and reports it in QEMU's log. A CDC ACM
 * function it bridges to a character device (the launcher's
 * devices/NAME/usb.sock): DTR and RTS up while a program is connected there
 * (the firmware's CLI starts its session then), what the program writes to
 * the bulk OUT endpoint, what the bulk IN endpoint gives to the program.
 *
 * It can also give the device to the PC's own kernel, over USB/IP (Linux's
 * usbip protocol, on the usbip chardev: the launcher's --usbip PORT): there
 * `usbip attach` (or FlipperZeroEmu's scripts/usbip_attach.py) makes it a
 * real USB device, cdc_acm binds /dev/ttyACM*, the boot ROM's drive mounts,
 * picotool finds it. The computer here keeps what only the bus does (the
 * reset, the address: vhci-hcd answers SET_ADDRESS on its side); each URB
 * that comes becomes transactions, a control one through the control
 * transfer above, a bulk or interrupt one on its endpoint, done when the
 * firmware gives or takes the data. Meanwhile the CDC bridge stands aside.
 *
 * It drives the controller a transaction at a time, on a timer of the
 * virtual clock. The firmware relies on being fast next to the bus: on the
 * chip its interrupt handler has taken a packet (and, say, a control
 * transfer's end) long before the host sends the next; an emulated CPU, on
 * its own thread, can be slower than the bus's time. So the host sends
 * nothing while the device's CPU still has events to take (10 ms at most),
 * and otherwise one about every 20 µs while they are taken, a NAKed one
 * again after 50 µs, and a bulk IN endpoint that has nothing looked at less
 * and less often, down to every 2 ms, until something comes from the
 * program or the endpoint gives something again.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "flipper_one.h"
#include "fo_usb.h"

#define STEP_NS         20000
#define NAK_NS          50000
#define IDLE_MAX_NS     2000000
#define ATTACH_NS       100000000       /* the pull-up to the bus reset */
#define RESET_NS        10000000
#define SET_ADDRESS_NS  2000000         /* the device's recovery */
#define PATIENCE_NS     10000000        /* the most it waits for the CPU */
#define ADDRESS         1
#define CTL_MAX         1024
#define TO_DEV_MAX      4096

enum { H_OFF, H_ATTACHING, H_RESETTING, H_ENUM, H_RUN, H_FAILED };
enum { C_IDLE, C_SETUP, C_DATA, C_STATUS };

/* the enumeration's steps */
enum { E_DEV8, E_ADDRESS, E_DEV, E_CONFIG9, E_CONFIG, E_LANG, E_STR1,
       E_STR2, E_STR3, E_SET_CONFIG, E_LINE_CODING, E_DONE };

typedef struct FoUsbUrb {
    uint32_t seqnum, flags, len, done;
    uint32_t start_frame, npackets;
    int ep, dir;                        /* dir 1: IN */
    uint8_t setup[8];
    uint8_t *data;
    bool started, unlinked, zlp_done;
    QTAILQ_ENTRY(FoUsbUrb) next;
} FoUsbUrb;

typedef struct FoUsbHost {
    DeviceState parent_obj;

    CharFrontend cdc_chr, usbip_chr;
    const FoUsbDevOps *ops;             /* the device the cable's data lines */
    void *dev;                          /* reach (NULL: none) */
    QEMUTimer *timer;
    int state;
    uint8_t addr;                       /* the device's, as the host uses it */

    /* the control transfer under way */
    int ctl;
    uint8_t setup[8];
    uint8_t ctl_buf[CTL_MAX];
    int ctl_len, ctl_got;
    bool ctl_ok;
    int ctl_failed, ctl_result;         /* where it ended badly, and how */
    char trail[16][48];                 /* the last transactions, for when
                                           the enumeration fails */
    int trail_n;

    int step;                           /* enumeration */
    int ep0_size;
    uint8_t dev_desc[18];
    uint8_t config[CTL_MAX];
    int config_len;
    char strings[3][64];

    /* the CDC ACM function, if there is one */
    int cdc_comm, cdc_in, cdc_out;      /* -1: none */
    bool open;                          /* a program is on the chardev */
    int dtr_sent;                       /* -1: not yet */
    GByteArray *to_dev, *to_chr;
    int64_t idle_ns;                    /* how long the IN endpoint naps */
    uint16_t ep_mps[2][16];             /* [IN][ep]: max packet sizes */
    uint8_t itf_class[16][3];           /* each interface's class */
    int n_itf;

    /* USB/IP */
    bool usbip_open, imported;
    GByteArray *usbip_rx, *usbip_tx;
    guint usbip_watch;
    QTAILQ_HEAD(, FoUsbUrb) urbs;
    FoUsbUrb *ctl_urb;                  /* the URB the control transfer is */
    int64_t waiting_since;              /* for the CPU's handler, since */
    guint watch;
} FoUsbHost;

OBJECT_DECLARE_SIMPLE_TYPE(FoUsbHost, FO_USB_HOST)

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void host_after(FoUsbHost *h, int64_t ns)
{
    timer_mod(h->timer, now_ns() + ns);
}

/* the transaction just done, kept for a report */
static void trail(FoUsbHost *h, const char *what, int ep, FoUsbResult r,
                  int len)
{
    static const char *const how[] = { "ACK", "NAK", "STALL", "-" };

    snprintf(h->trail[h->trail_n++ % 16], 48, "%6" PRId64 " us %s %d: %s %d",
             (now_ns() / 1000) % 1000000, what, ep, how[r & 3], len);
}

/* ---------------------------------------------------- control transfers */

static void ctl_start(FoUsbHost *h, uint8_t type, uint8_t req, uint16_t val,
                      uint16_t idx, uint16_t len, const uint8_t *data)
{
    uint8_t s[8] = { type, req, val, val >> 8, idx, idx >> 8, len, len >> 8 };

    memcpy(h->setup, s, 8);
    h->ctl_len = MIN(len, CTL_MAX);
    h->ctl_got = 0;
    if (data) {
        memcpy(h->ctl_buf, data, h->ctl_len);
    }
    h->ctl = C_SETUP;
}

/* a step of the control transfer: false while it goes on (call again after
 * the time it set), true when it ended (ctl_ok: how) */
static bool ctl_step(FoUsbHost *h, int64_t *wait)
{
    bool in = h->setup[0] & 0x80;
    uint8_t pkt[FO_USB_MAX_PACKET];
    FoUsbResult r = FO_USB_TIMEOUT;
    int n;

    *wait = STEP_NS;
    switch (h->ctl) {
    case C_SETUP:
        r = h->ops->setup(h->dev, h->addr, h->setup);
        trail(h, "SETUP", 0, r, 8);
        if (r != FO_USB_ACK) {
            break;
        }
        h->ctl = h->ctl_len ? C_DATA : C_STATUS;
        return false;
    case C_DATA:
        if (in) {
            n = 0;
            r = h->ops->in(h->dev, h->addr, 0, pkt, &n);
            trail(h, "IN", 0, r, n);
            if (r == FO_USB_ACK) {
                int take = MIN(n, h->ctl_len - h->ctl_got);

                memcpy(h->ctl_buf + h->ctl_got, pkt, take);
                h->ctl_got += take;
                if (n < h->ep0_size || h->ctl_got >= h->ctl_len) {
                    h->ctl = C_STATUS;
                }
                return false;
            }
        } else {
            n = MIN(h->ep0_size, h->ctl_len - h->ctl_got);
            r = h->ops->out(h->dev, h->addr, 0, h->ctl_buf + h->ctl_got, n);
            trail(h, "OUT", 0, r, n);
            if (r == FO_USB_ACK) {
                h->ctl_got += n;
                if (h->ctl_got >= h->ctl_len) {
                    h->ctl = C_STATUS;
                }
                return false;
            }
        }
        if (r == FO_USB_NAK) {
            *wait = NAK_NS;
            return false;
        }
        break;
    case C_STATUS:
        if (in) {
            r = h->ops->out(h->dev, h->addr, 0, NULL, 0);
            trail(h, "status OUT", 0, r, 0);
        } else {
            n = 0;
            r = h->ops->in(h->dev, h->addr, 0, pkt, &n);
            trail(h, "status IN", 0, r, n);
        }
        if (r == FO_USB_NAK) {
            *wait = NAK_NS;
            return false;
        }
        if (r != FO_USB_ACK) {
            break;
        }
        h->ctl = C_IDLE;
        h->ctl_ok = true;
        return true;
    }
    h->ctl_failed = h->ctl;             /* a stall, or nobody answered */
    h->ctl_result = r;
    h->ctl = C_IDLE;
    h->ctl_ok = false;
    return true;
}

/* ------------------------------------------------------------- the CDC */

static void cdc_flush(FoUsbHost *h);

static gboolean cdc_writable(void *do_not_use, GIOCondition cond, void *opaque)
{
    FoUsbHost *h = opaque;

    h->watch = 0;
    cdc_flush(h);
    return G_SOURCE_REMOVE;
}

static void cdc_flush(FoUsbHost *h)
{
    while (h->to_chr->len) {
        int w = qemu_chr_fe_write(&h->cdc_chr, h->to_chr->data,
                                  h->to_chr->len);

        if (w <= 0) {
            if (!h->open) {
                g_byte_array_set_size(h->to_chr, 0);
            } else if (!h->watch) {
                h->watch = qemu_chr_fe_add_watch(&h->cdc_chr,
                                                 G_IO_OUT | G_IO_HUP,
                                                 cdc_writable, h);
            }
            return;
        }
        g_byte_array_remove_range(h->to_chr, 0, w);
    }
}

/* the configuration's descriptors, one at a time (@i: 0 to start) */
static const uint8_t *config_next(FoUsbHost *h, int *i)
{
    const uint8_t *d;

    if (*i + 2 > h->config_len || h->config[*i] < 2 ||
        *i + h->config[*i] > h->config_len) {
        return NULL;
    }
    d = h->config + *i;
    *i += d[0];
    return d;
}

/* the configuration's CDC ACM function: its comm interface (the first
 * ACM one), and the bulk endpoints of the data interface its Union
 * descriptor names (without one, the interface after it: a composite
 * device can have other data interfaces, NCM's for one); and each
 * endpoint's packet size */
static void cdc_find(FoUsbHost *h)
{
    const uint8_t *d;
    int cls = -1, itf = -1, data = -1, i = 0;

    h->cdc_comm = h->cdc_in = h->cdc_out = -1;
    h->n_itf = 0;
    memset(h->ep_mps, 0, sizeof(h->ep_mps));
    h->ep_mps[0][0] = h->ep_mps[1][0] = h->ep0_size;
    while ((d = config_next(h, &i))) {
        if (d[1] == 5 && d[0] >= 7) {           /* ENDPOINT: its size */
            h->ep_mps[d[2] >> 7][d[2] & 0x0f] = MAX(d[4] | (d[5] & 7) << 8, 8);
        } else if (d[1] == 4 && d[0] >= 9) {    /* INTERFACE */
            itf = d[2];
            if (d[3] == 0 && h->n_itf < 16) {   /* (not an alternate) */
                memcpy(h->itf_class[h->n_itf++], d + 5, 3);
            }
            if (d[5] == 0x02 && d[6] == 0x02 && h->cdc_comm < 0) {
                h->cdc_comm = itf;              /* ACM */
                data = itf + 1;
            }
        } else if (d[1] == 0x24 && d[0] >= 5 && d[2] == 0x06 &&
                   h->cdc_comm >= 0 && itf == h->cdc_comm) {
            data = d[4];                        /* its Union */
        }
    }
    i = 0;
    while ((d = config_next(h, &i))) {
        if (d[1] == 4 && d[0] >= 9) {
            itf = d[2];
            cls = d[5];
        } else if (d[1] == 5 && d[0] >= 7 && itf == data && cls == 0x0a &&
                   (d[3] & 3) == 2) {           /* its data interface's bulk */
            if (d[2] & 0x80) {
                h->cdc_in = d[2] & 0x0f;
            } else {
                h->cdc_out = d[2] & 0x0f;
            }
        }
    }
    if (h->cdc_in < 0 || h->cdc_out < 0) {
        h->cdc_comm = h->cdc_in = h->cdc_out = -1;
    }
}

static void cdc_kick(FoUsbHost *h)
{
    h->idle_ns = 0;
    if (h->state == H_RUN && (!timer_pending(h->timer) ||
                              timer_expire_time_ns(h->timer) >
                              now_ns() + STEP_NS)) {
        host_after(h, STEP_NS);
    }
}

/* ------------------------------------------------------------ USB/IP */

#define USBIP_VERSION       0x0111
#define USBIP_BUSID         "1-1"
#define USBIP_CMD_SUBMIT    1
#define USBIP_CMD_UNLINK    2
#define USBIP_RET_SUBMIT    3
#define USBIP_RET_UNLINK    4
#define URB_ZERO_PACKET     0x0040
#define ERR_EPIPE           (-32)       /* Linux's errno: the client is Linux */
#define ERR_EOVERFLOW       (-75)
#define ERR_ECONNRESET      (-104)

static void usbip_flush(FoUsbHost *h);

static gboolean usbip_writable(void *do_not_use, GIOCondition cond,
                               void *opaque)
{
    FoUsbHost *h = opaque;

    h->usbip_watch = 0;
    usbip_flush(h);
    return G_SOURCE_REMOVE;
}

static void usbip_flush(FoUsbHost *h)
{
    while (h->usbip_tx->len) {
        int w = qemu_chr_fe_write(&h->usbip_chr, h->usbip_tx->data,
                                  h->usbip_tx->len);

        if (w <= 0) {
            if (!h->usbip_open) {
                g_byte_array_set_size(h->usbip_tx, 0);
            } else if (!h->usbip_watch) {
                h->usbip_watch = qemu_chr_fe_add_watch(&h->usbip_chr,
                                     G_IO_OUT | G_IO_HUP, usbip_writable, h);
            }
            return;
        }
        g_byte_array_remove_range(h->usbip_tx, 0, w);
    }
}

static void put_be16(GByteArray *b, uint16_t v)
{
    uint8_t x[2];

    stw_be_p(x, v);
    g_byte_array_append(b, x, 2);
}

static void put_be32(GByteArray *b, uint32_t v)
{
    uint8_t x[4];

    stl_be_p(x, v);
    g_byte_array_append(b, x, 4);
}

static void put_zero(GByteArray *b, int n)
{
    static const uint8_t z[256];

    g_byte_array_append(b, z, n);
}

/* the 0x138-byte device block of OP_REP_DEVLIST and OP_REP_IMPORT */
static void usbip_put_device(FoUsbHost *h, GByteArray *b)
{
    char path[256] = "/sys/devices/platform/flipperoneemu/usb1/" USBIP_BUSID;
    char busid[32] = USBIP_BUSID;
    uint8_t n_itf = h->n_itf;

    g_byte_array_append(b, (uint8_t *)path, sizeof(path));
    g_byte_array_append(b, (uint8_t *)busid, sizeof(busid));
    put_be32(b, 1);                                 /* busnum */
    put_be32(b, 2);                                 /* devnum */
    put_be32(b, h->ops ? h->ops->speed : 2);        /* USB_SPEED_FULL, _HIGH */
    put_be16(b, lduw_le_p(&h->dev_desc[8]));        /* idVendor */
    put_be16(b, lduw_le_p(&h->dev_desc[10]));       /* idProduct */
    put_be16(b, lduw_le_p(&h->dev_desc[12]));       /* bcdDevice */
    g_byte_array_append(b, &h->dev_desc[4], 3);     /* class, sub, proto */
    g_byte_array_append(b, &h->config[5], 1);       /* bConfigurationValue */
    g_byte_array_append(b, &h->dev_desc[17], 1);    /* bNumConfigurations */
    g_byte_array_append(b, &n_itf, 1);
}

static bool usbip_ready(FoUsbHost *h)
{
    return h->state == H_RUN && h->config_len >= 9;
}

static void usbip_free_urb(FoUsbHost *h, FoUsbUrb *u)
{
    QTAILQ_REMOVE(&h->urbs, u, next);
    g_free(u->data);
    g_free(u);
}

static void usbip_complete(FoUsbHost *h, FoUsbUrb *u, int32_t status)
{
    GByteArray *b = h->usbip_tx;
    uint32_t actual = u->dir ? u->done : (status ? u->done : u->len);

    if (!u->unlinked && h->imported) {
        put_be32(b, USBIP_RET_SUBMIT);
        put_be32(b, u->seqnum);
        put_be32(b, 0);                         /* devid, direction, ep: 0 */
        put_be32(b, 0);
        put_be32(b, 0);
        put_be32(b, status);
        put_be32(b, actual);
        put_be32(b, u->start_frame);            /* what the client sent */
        put_be32(b, u->npackets);
        put_be32(b, 0);                         /* error_count */
        put_zero(b, 8);
        if (u->dir) {
            g_byte_array_append(b, u->data, u->done);
        }
        usbip_flush(h);
    }
    if (h->ctl_urb == u) {
        h->ctl_urb = NULL;
    }
    usbip_free_urb(h, u);
}

/* the control transfer that was a URB's ended */
static void usbip_ctl_done(FoUsbHost *h)
{
    FoUsbUrb *u = h->ctl_urb;

    if (u->dir) {
        u->done = MIN(h->ctl_got, u->len);
        memcpy(u->data, h->ctl_buf, u->done);
    } else {
        u->done = h->ctl_ok ? u->len : 0;
    }
    usbip_complete(h, u, h->ctl_ok ? 0 : ERR_EPIPE);
}

static void usbip_drop_urbs(FoUsbHost *h)
{
    FoUsbUrb *u, *tmp;

    QTAILQ_FOREACH_SAFE(u, &h->urbs, next, tmp) {
        if (u == h->ctl_urb) {
            h->ctl = C_IDLE;
            h->ctl_urb = NULL;
        }
        usbip_free_urb(h, u);
    }
}

/* a bulk or interrupt transaction for the URB first on its endpoint:
 * whether it moved */
static bool usbip_transact(FoUsbHost *h, FoUsbUrb *u)
{
    int mps = h->ep_mps[u->dir][u->ep] ?: 64;
    uint8_t pkt[FO_USB_MAX_PACKET];
    FoUsbResult r;
    int n = 0;

    if (u->dir) {
        r = h->ops->in(h->dev, h->addr, u->ep, pkt, &n);
        if (r == FO_USB_ACK) {
            int take = MIN(n, (int)(u->len - u->done));

            memcpy(u->data + u->done, pkt, take);
            u->done += take;
            if (n > take) {
                usbip_complete(h, u, ERR_EOVERFLOW);        /* babble */
            } else if (n < mps || u->done >= u->len) {
                usbip_complete(h, u, 0);
            }
            return true;
        }
    } else {
        bool want_zlp = (u->flags & URB_ZERO_PACKET) && u->len &&
                        !(u->len % mps);

        n = MIN(mps, (int)(u->len - u->done));
        r = h->ops->out(h->dev, h->addr, u->ep, u->data + u->done, n);
        if (r == FO_USB_ACK) {
            u->done += n;
            if (!n) {
                u->zlp_done = true;
            }
            if (u->done >= u->len && (!want_zlp || u->zlp_done || !n) &&
                (u->len || u->zlp_done)) {
                usbip_complete(h, u, 0);
            }
            return true;
        }
    }
    if (r == FO_USB_STALL) {
        usbip_complete(h, u, ERR_EPIPE);
        return true;
    }
    return false;
}

/* the imported device's next transactions; the time to the next look */
static int64_t usbip_step(FoUsbHost *h)
{
    FoUsbUrb *u, *tmp;
    uint32_t busy = 0;
    bool moved = false, waiting = false;
    int64_t wait;

    if (h->ctl != C_IDLE) {
        if (ctl_step(h, &wait) && h->ctl_urb) {
            usbip_ctl_done(h);
        }
        return wait;
    }
    QTAILQ_FOREACH(u, &h->urbs, next) {     /* control: in order */
        if (u->ep == 0 && !u->started) {
            u->started = true;
            h->ctl_urb = u;
            ctl_start(h, u->setup[0], u->setup[1], lduw_le_p(&u->setup[2]),
                      lduw_le_p(&u->setup[4]),
                      MIN(lduw_le_p(&u->setup[6]), u->len),
                      u->dir ? NULL : u->data);
            return STEP_NS;
        }
    }
    QTAILQ_FOREACH_SAFE(u, &h->urbs, next, tmp) {
        uint32_t key = 1u << ((u->dir << 4) | u->ep);

        if (u->ep == 0 || (busy & key)) {
            continue;
        }
        busy |= key;
        moved |= usbip_transact(h, u);
        waiting = true;
    }
    return moved ? STEP_NS : waiting ? NAK_NS : -1;
}

static void usbip_reply_import(FoUsbHost *h, bool ok)
{
    GByteArray *b = h->usbip_tx;

    put_be16(b, USBIP_VERSION);
    put_be16(b, 0x0003);                    /* OP_REP_IMPORT */
    put_be32(b, ok ? 0 : 1);
    if (ok) {
        usbip_put_device(h, b);
    }
    usbip_flush(h);
}

static void usbip_import(FoUsbHost *h, const char *busid)
{
    if (strcmp(busid, USBIP_BUSID) || !usbip_ready(h)) {
        usbip_reply_import(h, false);
        return;
    }
    usbip_drop_urbs(h);
    h->imported = true;
    usbip_reply_import(h, true);
    info_report("flipper-one: USB: %04x:%04x attached to the PC over USB/IP",
                lduw_le_p(&h->dev_desc[8]), lduw_le_p(&h->dev_desc[10]));
}

static void usbip_reply_devlist(FoUsbHost *h)
{
    GByteArray *b = h->usbip_tx;
    bool ready = usbip_ready(h);

    put_be16(b, USBIP_VERSION);
    put_be16(b, 0x0005);                    /* OP_REP_DEVLIST */
    put_be32(b, 0);
    put_be32(b, ready ? 1 : 0);
    if (ready) {
        usbip_put_device(h, b);
        for (int i = 0; i < h->n_itf; i++) {
            g_byte_array_append(b, h->itf_class[i], 3);
            put_zero(b, 1);
        }
    }
    usbip_flush(h);
}

static void usbip_unlink(FoUsbHost *h, uint32_t seqnum, uint32_t target)
{
    GByteArray *b = h->usbip_tx;
    int32_t status = 0;
    FoUsbUrb *u;

    QTAILQ_FOREACH(u, &h->urbs, next) {
        if (u->seqnum == target && !u->unlinked) {
            status = ERR_ECONNRESET;
            if (u == h->ctl_urb) {
                u->unlinked = true;         /* it ends on the bus, untold */
            } else {
                usbip_free_urb(h, u);
            }
            break;
        }
    }
    put_be32(b, USBIP_RET_UNLINK);
    put_be32(b, seqnum);
    put_zero(b, 12);
    put_be32(b, status);
    put_zero(b, 24);
    usbip_flush(h);
}

/* what the client sent; false on a protocol error */
static bool usbip_parse(FoUsbHost *h)
{
    GByteArray *in = h->usbip_rx;

    while (in->len >= 8) {
        const uint8_t *p = in->data;
        uint32_t cmd, seqnum;

        if (!h->imported) {
            uint16_t code = lduw_be_p(p + 2);

            if (code == 0x8005) {           /* OP_REQ_DEVLIST */
                g_byte_array_remove_range(in, 0, 8);
                usbip_reply_devlist(h);
            } else if (code == 0x8003) {    /* OP_REQ_IMPORT */
                char busid[33];

                if (in->len < 40) {
                    return true;
                }
                memcpy(busid, p + 8, 32);
                busid[32] = 0;
                g_byte_array_remove_range(in, 0, 40);
                usbip_import(h, busid);
            } else {
                return false;
            }
            continue;
        }
        if (in->len < 48) {
            return true;
        }
        cmd = ldl_be_p(p);
        seqnum = ldl_be_p(p + 4);
        if (cmd == USBIP_CMD_SUBMIT) {
            uint32_t dir = ldl_be_p(p + 12), len = ldl_be_p(p + 24);
            FoUsbUrb *u;

            if (len > 1024 * 1024) {
                return false;
            }
            if (!dir && in->len < 48 + len) {
                return true;                /* the OUT data, still to come */
            }
            u = g_new0(FoUsbUrb, 1);
            u->seqnum = seqnum;
            u->dir = dir ? 1 : 0;
            u->ep = ldl_be_p(p + 16) & 0xf;
            u->flags = ldl_be_p(p + 20);
            u->len = len;
            u->start_frame = ldl_be_p(p + 28);
            u->npackets = ldl_be_p(p + 32);
            memcpy(u->setup, p + 40, 8);
            u->data = g_malloc0(MAX(len, 1));
            if (!dir) {
                memcpy(u->data, p + 48, len);
            }
            g_byte_array_remove_range(in, 0, 48 + (dir ? 0 : len));
            QTAILQ_INSERT_TAIL(&h->urbs, u, next);
            cdc_kick(h);
        } else if (cmd == USBIP_CMD_UNLINK) {
            uint32_t target = ldl_be_p(p + 20);

            g_byte_array_remove_range(in, 0, 48);
            usbip_unlink(h, seqnum, target);
        } else {
            return false;
        }
    }
    return true;
}

static int usbip_can_receive(void *opaque)
{
    FoUsbHost *h = opaque;

    return h->usbip_rx->len < 2 * 1024 * 1024 ? 64 * 1024 : 0;
}

static void usbip_receive(void *opaque, const uint8_t *buf, int size)
{
    FoUsbHost *h = opaque;

    g_byte_array_append(h->usbip_rx, buf, size);
    if (!usbip_parse(h)) {
        warn_report("flipper-one: USB/IP: a protocol error; closing");
        qemu_chr_fe_disconnect(&h->usbip_chr);
    }
}

static void host_look(FoUsbHost *h);

static void usbip_end(FoUsbHost *h)
{
    bool was = h->imported;

    h->imported = false;
    usbip_drop_urbs(h);
    g_byte_array_set_size(h->usbip_rx, 0);
    if (was) {
        info_report("flipper-one: USB: the PC let go of it (USB/IP)");
        if (h->state != H_OFF) {            /* back to this computer: a reset */
            h->state = H_OFF;
            timer_del(h->timer);
            h->ops->detach(h->dev);
            host_look(h);
        }
    }
}

static void usbip_event(void *opaque, QEMUChrEvent event)
{
    FoUsbHost *h = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        h->usbip_open = true;
        g_byte_array_set_size(h->usbip_rx, 0);
        g_byte_array_set_size(h->usbip_tx, 0);
        break;
    case CHR_EVENT_CLOSED:
        h->usbip_open = false;
        g_byte_array_set_size(h->usbip_tx, 0);
        usbip_end(h);
        break;
    default:
        break;
    }
}

/* the running device: a control transfer (DTR), else bulk OUT, else IN */
static int64_t run_step(FoUsbHost *h)
{
    uint8_t buf[FO_USB_MAX_PACKET];
    int64_t wait;
    int n;

    if (h->imported) {
        return usbip_step(h);           /* the PC's kernel has it */
    }
    if (h->ctl != C_IDLE) {
        ctl_step(h, &wait);
        return wait;
    }
    if (h->cdc_in < 0) {
        return -1;                      /* nothing to do for it */
    }
    if (h->dtr_sent != h->open) {
        ctl_start(h, 0x21, 0x22, h->open ? 3 : 0, h->cdc_comm, 0, NULL);
        h->dtr_sent = h->open;          /* SET_CONTROL_LINE_STATE */
        return STEP_NS;
    }
    if (h->to_dev->len) {
        n = MIN(h->ep_mps[0][h->cdc_out] ?: 64, h->to_dev->len);
        if (h->ops->out(h->dev, h->addr, h->cdc_out, h->to_dev->data, n) ==
            FO_USB_ACK) {
            g_byte_array_remove_range(h->to_dev, 0, n);
            qemu_chr_fe_accept_input(&h->cdc_chr);
            return STEP_NS;
        }
    }
    if (h->open && h->to_chr->len < 4096) {
        n = 0;
        if (h->ops->in(h->dev, h->addr, h->cdc_in, buf, &n) == FO_USB_ACK) {
            g_byte_array_append(h->to_chr, buf, n);
            cdc_flush(h);
            h->idle_ns = 0;
            return STEP_NS;
        }
    }
    if (h->to_dev->len) {
        return NAK_NS;
    }
    h->idle_ns = MIN(MAX(h->idle_ns * 2, 100000), IDLE_MAX_NS);
    return h->open ? h->idle_ns : -1;
}

/* ------------------------------------------------------ the enumeration */

static void decode_string(FoUsbHost *h, int which)
{
    char *out = h->strings[which];
    int len = MIN(h->ctl_got, h->ctl_buf[0]), j = 0;

    for (int i = 2; i + 1 < len && j < 63; i += 2) {
        uint16_t c = h->ctl_buf[i] | h->ctl_buf[i + 1] << 8;

        out[j++] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    out[j] = 0;
}

static void enum_report(FoUsbHost *h)
{
    const uint8_t *d = h->dev_desc;

    info_report("flipper-one: USB: %04x:%04x \"%s\" \"%s\" (serial %s), "
                "configured%s",
                d[8] | d[9] << 8, d[10] | d[11] << 8, h->strings[0],
                h->strings[1], h->strings[2][0] ? h->strings[2] : "none",
                h->cdc_in >= 0 ? "; its CDC ACM serial port on the host's "
                "chardev" : "");
}

/* the next request of the enumeration, or the last one's answer taken */
static void enum_next(FoUsbHost *h)
{
    const uint8_t *d = h->dev_desc;
    int idx;

    switch (h->step) {
    case E_DEV8:
        ctl_start(h, 0x80, 6, 0x0100, 0, 64, NULL);
        break;
    case E_ADDRESS:
        ctl_start(h, 0x00, 5, ADDRESS, 0, 0, NULL);
        break;
    case E_DEV:
        ctl_start(h, 0x80, 6, 0x0100, 0, 18, NULL);
        break;
    case E_CONFIG9:
        ctl_start(h, 0x80, 6, 0x0200, 0, 9, NULL);
        break;
    case E_CONFIG:
        ctl_start(h, 0x80, 6, 0x0200, 0, h->config_len, NULL);
        break;
    case E_LANG:
        ctl_start(h, 0x80, 6, 0x0300, 0, 255, NULL);
        break;
    case E_STR1: case E_STR2: case E_STR3:
        idx = d[14 + (h->step - E_STR1)];
        if (!idx) {
            h->step++;
            enum_next(h);
            return;
        }
        ctl_start(h, 0x80, 6, 0x0300 | idx, 0x0409, 255, NULL);
        break;
    case E_SET_CONFIG:
        ctl_start(h, 0x00, 9, h->config[5], 0, 0, NULL);
        break;
    case E_LINE_CODING: {
        static const uint8_t coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };

        if (h->cdc_comm < 0) {
            h->step++;
            enum_next(h);
            return;
        }
        ctl_start(h, 0x21, 0x20, 0, h->cdc_comm, 7, coding);  /* 115200 8N1 */
        break;
    }
    default:
        enum_report(h);
        h->state = H_RUN;
        h->dtr_sent = -1;
        h->idle_ns = 0;
        qemu_chr_fe_accept_input(&h->cdc_chr);
        break;
    }
}

/* the enumeration's last request ended: what it said */
static bool enum_took(FoUsbHost *h)
{
    if (!h->ctl_ok && h->step != E_LANG && h->step != E_LINE_CODING &&
        !(h->step >= E_STR1 && h->step <= E_STR3)) {
        static const char *const stage[] = { "", "SETUP", "data", "status" };
        static const char *const how[] = { "ACK", "NAK", "a STALL",
                                           "no answer" };

        warn_report("flipper-one: USB: the device did not answer the "
                    "enumeration: request %02x %02x %02x%02x, at its %s "
                    "stage, %s (step %d)", h->setup[0], h->setup[1],
                    h->setup[3], h->setup[2], stage[h->ctl_failed & 3],
                    how[h->ctl_result & 3], h->step);
        for (int i = MAX(h->trail_n - 16, 0); i < h->trail_n; i++) {
            error_printf("  %s\n", h->trail[i % 16]);
        }
        return false;
    }
    switch (h->step) {
    case E_DEV8:
        if (h->ctl_got < 8) {
            return false;
        }
        h->ep0_size = MAX(h->ctl_buf[7], 8);
        break;
    case E_ADDRESS:
        h->addr = ADDRESS;
        break;
    case E_DEV:
        memcpy(h->dev_desc, h->ctl_buf, MIN(h->ctl_got, 18));
        break;
    case E_CONFIG9:
        h->config_len = MIN(h->ctl_buf[2] | h->ctl_buf[3] << 8, CTL_MAX);
        break;
    case E_CONFIG:
        memcpy(h->config, h->ctl_buf, h->ctl_got);
        h->config_len = h->ctl_got;
        cdc_find(h);
        break;
    case E_STR1: case E_STR2: case E_STR3:
        if (h->ctl_ok) {
            decode_string(h, h->step - E_STR1);
        }
        break;
    }
    h->step++;
    return true;
}

/* ------------------------------------------------------------ the host */

/* the CPU has not dealt with the last transaction yet: wait for it */
static bool host_waits(FoUsbHost *h)
{
    int64_t now = now_ns();

    if (h->ops->irq_pending(h->dev)) {
        if (!h->waiting_since) {
            h->waiting_since = now;
        }
        if (now - h->waiting_since < PATIENCE_NS) {
            host_after(h, STEP_NS);
            return true;
        }
    }
    h->waiting_since = 0;
    return false;
}

static void host_step(void *opaque)
{
    FoUsbHost *h = opaque;
    int64_t wait = STEP_NS;

    if ((h->state == H_ENUM || h->state == H_RUN) && host_waits(h)) {
        return;
    }

    switch (h->state) {
    case H_ATTACHING:
        h->ops->bus_reset(h->dev);
        h->state = H_RESETTING;
        host_after(h, RESET_NS);
        return;
    case H_RESETTING:
        h->state = H_ENUM;
        h->addr = 0;
        h->ep0_size = 64;               /* until it says: a first read of
                                           64 takes one packet, as a PC's */
        h->step = E_DEV8;
        memset(h->strings, 0, sizeof(h->strings));
        enum_next(h);
        host_after(h, STEP_NS);
        return;
    case H_ENUM:
        if (!ctl_step(h, &wait)) {
            host_after(h, wait);
            return;
        }
        if (!enum_took(h)) {
            h->state = H_FAILED;
            return;
        }
        enum_next(h);
        host_after(h, h->step == E_DEV ? SET_ADDRESS_NS : STEP_NS);
        return;
    case H_RUN:
        wait = run_step(h);
        if (wait >= 0) {
            host_after(h, wait);
        }
        return;
    default:
        return;
    }
}

/* the device has gone from the bus, or the bus from it */
static void host_detach(FoUsbHost *h)
{
    h->state = H_OFF;
    h->ctl = C_IDLE;
    timer_del(h->timer);
    h->ops->detach(h->dev);
    g_byte_array_set_size(h->to_dev, 0);
    if (h->imported) {                      /* the device left: the PC's
                                               client has nothing more */
        h->imported = false;
        usbip_drop_urbs(h);
        qemu_chr_fe_disconnect(&h->usbip_chr);
    }
}

/* the cable's data lines and the device's pull-up: attached or not */
static void host_look(FoUsbHost *h)
{
    bool attached = h->ops && h->ops->pulled_up(h->dev);

    if (attached && h->state == H_OFF) {
        h->state = H_ATTACHING;
        host_after(h, ATTACH_NS);
    } else if (!attached && h->state != H_OFF) {
        host_detach(h);
    }
}

void fo_usb_host_pullup(void *opaque, bool on)
{
    host_look(FO_USB_HOST(opaque));
}

void fo_usb_host_connect(DeviceState *dev, const FoUsbDevOps *ops,
                         void *opaque)
{
    FoUsbHost *h = FO_USB_HOST(dev);

    if (ops == h->ops && opaque == h->dev) {
        return;
    }
    if (h->state != H_OFF) {
        host_detach(h);                     /* (from the one it had) */
    }
    h->ops = ops;
    h->dev = ops ? opaque : NULL;
    host_look(h);
}

/* ------------------------------------------------- the program's side */

static int cdc_can_receive(void *opaque)
{
    FoUsbHost *h = opaque;

    return h->cdc_in >= 0 && h->state == H_RUN ?
           TO_DEV_MAX - (int)h->to_dev->len : 0;
}

static void cdc_receive(void *opaque, const uint8_t *buf, int size)
{
    FoUsbHost *h = opaque;

    g_byte_array_append(h->to_dev, buf, size);
    cdc_kick(h);
}

static void cdc_event(void *opaque, QEMUChrEvent event)
{
    FoUsbHost *h = opaque;

    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        h->open = event == CHR_EVENT_OPENED;
        g_byte_array_set_size(h->to_dev, 0);
        g_byte_array_set_size(h->to_chr, 0);
        cdc_kick(h);                    /* DTR, and what comes */
    }
}

static void usb_host_realize(DeviceState *dev, Error **errp)
{
    FoUsbHost *h = FO_USB_HOST(dev);

    h->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, host_step, h);
    h->to_dev = g_byte_array_new();
    h->to_chr = g_byte_array_new();
    h->usbip_rx = g_byte_array_new();
    h->usbip_tx = g_byte_array_new();
    QTAILQ_INIT(&h->urbs);
    h->cdc_comm = h->cdc_in = h->cdc_out = -1;
    if (qemu_chr_fe_backend_connected(&h->cdc_chr)) {
        qemu_chr_fe_set_handlers(&h->cdc_chr, cdc_can_receive, cdc_receive,
                                 cdc_event, NULL, h, NULL, true);
    }
    if (qemu_chr_fe_backend_connected(&h->usbip_chr)) {
        qemu_chr_fe_set_handlers(&h->usbip_chr, usbip_can_receive,
                                 usbip_receive, usbip_event, NULL, h, NULL,
                                 true);
    }
}

static const Property usb_host_properties[] = {
    DEFINE_PROP_CHR("cdc", FoUsbHost, cdc_chr),
    DEFINE_PROP_CHR("usbip", FoUsbHost, usbip_chr),
};

static void usb_host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usb_host_realize;
    dc->desc = "Flipper One: the computer at the other end of its USB "
               "cable, with the device's CDC serial port on a chardev";
    device_class_set_props(dc, usb_host_properties);
}

static const TypeInfo usb_host_types[] = {
    {
        .name = TYPE_FO_USB_HOST,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(FoUsbHost),
        .class_init = usb_host_class_init,
    },
};
DEFINE_TYPES(usb_host_types)
