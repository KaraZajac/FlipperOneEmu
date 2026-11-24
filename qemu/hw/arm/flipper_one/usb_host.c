/*
 * A USB host on the other end of the Flipper One's cable: the PC the MCU's
 * USB device is plugged into
 *
 * QEMU has no USB wire that leaves the machine, so this is the host: when
 * the cable's data lines reach the MCU (a computer's port is plugged in and
 * the board's USB mux has them on the MCU) and the device pulls D+ up, it
 * waits 100 ms, resets the bus for 10 ms, and enumerates what it finds as a
 * PC would: the device descriptor at address 0, SET_ADDRESS (1), the device
 * descriptor again, the configuration, the strings, SET_CONFIGURATION; and
 * reports it in QEMU's log. A CDC ACM function it bridges to a character
 * device (the launcher's devices/NAME/usb.sock): DTR and RTS up while a
 * program is connected there (the firmware's CLI starts its session then),
 * what the program writes to the bulk OUT endpoint, what the bulk IN
 * endpoint gives to the program.
 *
 * It drives the controller (rp2350_usb.c) a transaction at a time, on a
 * timer of the virtual clock. The firmware relies on being fast next to the
 * bus: on the chip its interrupt handler has taken a packet (and, say, a
 * control transfer's end) long before the host sends the next; an emulated
 * CPU, on its own thread, can be slower than the bus's time. So the host
 * sends nothing while the controller's interrupt is still up (10 ms at
 * most), and otherwise one about every 20 µs while they are taken,
 * a NAKed one again after 50 µs, and a bulk IN endpoint that has nothing
 * looked at less and less often, down to every 2 ms, until something comes
 * from the program or the endpoint gives something again.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "flipper_one.h"
#include "rp2350.h"

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

typedef struct FoUsbHost {
    DeviceState parent_obj;

    CharFrontend cdc_chr;
    RP2350State *soc;
    QEMUTimer *timer;
    bool cable;                         /* a computer's data lines are on it */
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
static void trail(FoUsbHost *h, const char *what, int ep, RP2UsbResult r,
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
    RP2350State *s = h->soc;
    bool in = h->setup[0] & 0x80;
    uint8_t zlp[64];
    RP2UsbResult r;
    int n;

    *wait = STEP_NS;
    switch (h->ctl) {
    case C_SETUP:
        r = rp2350_usb_setup(s, h->addr, h->setup);
        trail(h, "SETUP", 0, r, 8);
        if (r != RP2_USB_ACK) {
            break;
        }
        h->ctl = h->ctl_len ? C_DATA : C_STATUS;
        return false;
    case C_DATA:
        if (in) {
            n = 0;
            r = rp2350_usb_in(s, h->addr, 0, h->ctl_buf + h->ctl_got, &n);
            trail(h, "IN", 0, r, n);
            if (r == RP2_USB_ACK) {
                h->ctl_got = MIN(h->ctl_got + n, h->ctl_len);
                if (n < h->ep0_size || h->ctl_got >= h->ctl_len) {
                    h->ctl = C_STATUS;
                }
                return false;
            }
        } else {
            n = MIN(h->ep0_size, h->ctl_len - h->ctl_got);
            r = rp2350_usb_out(s, h->addr, 0, h->ctl_buf + h->ctl_got, n);
            trail(h, "OUT", 0, r, n);
            if (r == RP2_USB_ACK) {
                h->ctl_got += n;
                if (h->ctl_got >= h->ctl_len) {
                    h->ctl = C_STATUS;
                }
                return false;
            }
        }
        if (r == RP2_USB_NAK) {
            *wait = NAK_NS;
            return false;
        }
        break;
    case C_STATUS:
        if (in) {
            r = rp2350_usb_out(s, h->addr, 0, NULL, 0);
            trail(h, "status OUT", 0, r, 0);
        } else {
            n = 0;
            r = rp2350_usb_in(s, h->addr, 0, zlp, &n);
            trail(h, "status IN", 0, r, n);
        }
        if (r == RP2_USB_NAK) {
            *wait = NAK_NS;
            return false;
        }
        if (r != RP2_USB_ACK) {
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

/* the configuration's CDC ACM function: its comm interface, its data
 * interface's bulk endpoints */
static void cdc_find(FoUsbHost *h)
{
    int cls = -1, itf = -1;

    h->cdc_comm = h->cdc_in = h->cdc_out = -1;
    for (int i = 0; i + 2 <= h->config_len && h->config[i] >= 2;
         i += h->config[i]) {
        const uint8_t *d = h->config + i;

        if (d[1] == 4 && d[0] >= 9) {           /* INTERFACE */
            itf = d[2];
            cls = d[5];
            if (cls == 0x02 && d[6] == 0x02 && h->cdc_comm < 0) {
                h->cdc_comm = itf;              /* ACM */
            }
        } else if (d[1] == 5 && d[0] >= 7 && cls == 0x0a &&
                   (d[3] & 3) == 2) {           /* a data interface's bulk */
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

/* the running device: a control transfer (DTR), else bulk OUT, else IN */
static int64_t run_step(FoUsbHost *h)
{
    RP2350State *s = h->soc;
    uint8_t buf[64];
    int64_t wait;
    int n;

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
        n = MIN(64, h->to_dev->len);
        if (rp2350_usb_out(s, h->addr, h->cdc_out, h->to_dev->data, n) ==
            RP2_USB_ACK) {
            g_byte_array_remove_range(h->to_dev, 0, n);
            qemu_chr_fe_accept_input(&h->cdc_chr);
            return STEP_NS;
        }
    }
    if (h->open && h->to_chr->len < 4096) {
        n = 0;
        if (rp2350_usb_in(s, h->addr, h->cdc_in, buf, &n) == RP2_USB_ACK) {
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

    if (rp2350_usb_irq_pending(h->soc)) {
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
        rp2350_usb_bus_reset(h->soc);
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

/* the cable's data lines and the device's pull-up: attached or not */
static void host_look(FoUsbHost *h)
{
    bool attached = h->cable && rp2350_usb_pulled_up(h->soc);

    if (attached && h->state == H_OFF) {
        h->state = H_ATTACHING;
        host_after(h, ATTACH_NS);
    } else if (!attached && h->state != H_OFF) {
        h->state = H_OFF;
        h->ctl = C_IDLE;
        timer_del(h->timer);
        rp2350_usb_detach(h->soc);
        g_byte_array_set_size(h->to_dev, 0);
    }
}

static void host_pullup(void *opaque, bool on)
{
    host_look(opaque);
}

void fo_usb_host_cable(DeviceState *dev, bool data)
{
    FoUsbHost *h = FO_USB_HOST(dev);

    h->cable = data;
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
    h->cdc_comm = h->cdc_in = h->cdc_out = -1;
    if (qemu_chr_fe_backend_connected(&h->cdc_chr)) {
        qemu_chr_fe_set_handlers(&h->cdc_chr, cdc_can_receive, cdc_receive,
                                 cdc_event, NULL, h, NULL, true);
    }
}

void fo_usb_host_attach(DeviceState *dev, struct RP2350State *soc)
{
    FoUsbHost *h = FO_USB_HOST(dev);

    h->soc = soc;
    soc->usb.pullup_fn = host_pullup;
    soc->usb.pullup_opaque = h;
}

static const Property usb_host_properties[] = {
    DEFINE_PROP_CHR("cdc", FoUsbHost, cdc_chr),
};

static void usb_host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usb_host_realize;
    dc->desc = "Flipper One: the computer at the other end of its USB "
               "cable, with the MCU's CDC serial port on a chardev";
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
