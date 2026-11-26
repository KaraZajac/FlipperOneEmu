/*
 * The RK3576's boot ROM in its USB download mode ("MaskROM"): its USB0 a
 * device, 2207:350e, as the computer on the cable drives it (fo_usb.h)
 *
 * The boot ROM is here when BOOT0 was low at the reset, or when no medium
 * had an image to boot. It takes Rockchip's maskrom protocol, as rockusb's
 * and rkdeveloptool's download-boot speak it: vendor requests (0x40, 0x0c)
 * on the control endpoint, whose index says what the data is: 0x471, code
 * for SRAM that returns to the boot ROM (the DDR blob), or 0x472, code for
 * DRAM that does not (SPL). A request brings up to 4096 bytes, and one
 * that brings fewer ends the payload; its last two bytes were its CRC-16
 * (CCITT, from 0xffff), so that all of it comes to 0. Two zero bytes there
 * are taken without a look, as the RK3576's boot ROM does. A payload that
 * fails its CRC is dropped. One that passes runs, the board's to say how
 * (run(): the boot ROM itself only receives). Its descriptors: a Rockusb
 * device in its MaskROM (bcdUSB 2.00, its bit 0 clear, which is what tells
 * a tool so), a vendor interface (ff/06/05) with a bulk endpoint each way
 * that it does not use, no strings.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/crc-ccitt.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/units.h"
#include "rk3576.h"
#include "fo_usb.h"

#define CHUNK           4096            /* a request's most */
#define SRAM_MAX        (256 * KiB)     /* 0x471: what SRAM takes */
#define DRAM_MAX        (256 * MiB)     /* 0x472: up to 0x50000000 */

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02,                  /* USB 2.00 */
    0, 0, 0, 64,                        /* (the interface's class), ep0 64 */
    0x07, 0x22, 0x0e, 0x35,             /* 2207:350e */
    0x00, 0x01,                         /* bcdDevice 1.00 */
    0, 0, 0, 1,                         /* no strings; one configuration */
};

static const uint8_t config_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,     /* bus powered, 100 mA */
    9, 4, 0, 0, 2, 0xff, 0x06, 0x05, 0, /* vendor: Rockusb */
    7, 5, 0x81, 2, 0x00, 0x02, 0,       /* bulk IN 1, 512 */
    7, 5, 0x01, 2, 0x00, 0x02, 0,       /* bulk OUT 1, 512 */
};

enum { AFTER_NONE, AFTER_ADDRESS, AFTER_CHUNK };

static void maskrom_drop(RkMaskrom *m)
{
    g_byte_array_set_size(m->payload, 0);
    m->code = 0;
    m->too_big = false;
}

/* the payload's end: checked, then run once the host has its status */
static void maskrom_end(RkMaskrom *m)
{
    const uint8_t *p = m->payload->data;
    size_t len = m->payload->len;
    bool unchecked = len >= 2 && !p[len - 2] && !p[len - 1];

    if (m->too_big) {
        qemu_log_mask(LOG_GUEST_ERROR, "rk3576: MaskROM: a 0x%03x payload "
                      "larger than its memory: dropped\n", m->code);
    } else if (len < 2 || (!unchecked && crc_ccitt_false(0xffff, p, len))) {
        qemu_log_mask(LOG_GUEST_ERROR, "rk3576: MaskROM: a 0x%03x payload of "
                      "%zu bytes failed its CRC: dropped\n", m->code, len);
    } else {
        m->run_unchecked = unchecked;
        m->run_code = m->code;
        g_byte_array_set_size(m->run_payload, 0);
        g_byte_array_append(m->run_payload, p, len - 2);
        qemu_bh_schedule(m->bh);
    }
    maskrom_drop(m);
}

static void maskrom_run(void *opaque)
{
    RkMaskrom *m = opaque;

    if (!m->on) {
        return;
    }
    qemu_log("rk3576: MaskROM: a 0x%03x payload of %u bytes (%s): it "
             "runs\n", m->run_code, m->run_payload->len,
             m->run_unchecked ? "no CRC" : "its CRC good");
    if (m->run) {
        m->run(m->run_opaque, m->run_code, m->run_payload->data,
               m->run_payload->len);
    }
}

/* -------------------------------------------- as the computer drives it */

static bool maskrom_pulled_up(void *opaque)
{
    return ((RkMaskrom *)opaque)->on;
}

static bool maskrom_irq_pending(void *opaque)
{
    return false;
}

static void maskrom_bus_reset(void *opaque)
{
    RkMaskrom *m = opaque;

    m->addr = 0;
    m->config = 0;
    m->ctl = false;
    maskrom_drop(m);
}

static void maskrom_detach(void *opaque)
{
    maskrom_bus_reset(opaque);
}

/* the SETUP of a control transfer: what it asks, what its stages will do */
static FoUsbResult maskrom_setup(void *opaque, uint8_t addr,
                                 const uint8_t pkt[8])
{
    RkMaskrom *m = opaque;
    uint8_t type = pkt[0], req = pkt[1];
    uint16_t val = lduw_le_p(pkt + 2), idx = lduw_le_p(pkt + 4);
    uint16_t len = lduw_le_p(pkt + 6);
    const uint8_t *reply = NULL;
    uint8_t two[2] = { 0, 0 };
    int n = 0;

    if (!m->on || addr != m->addr) {
        return FO_USB_TIMEOUT;
    }
    m->ctl = false;
    m->ctl_in = type & 0x80;
    m->after = AFTER_NONE;
    m->out_len = m->out_got = 0;
    m->reply_len = m->reply_pos = 0;
    switch (type << 8 | req) {
    case 0x8006:                                /* GET_DESCRIPTOR */
        if (val >> 8 == 1) {
            reply = dev_desc;
            n = sizeof(dev_desc);
        } else if (val >> 8 == 2) {
            reply = config_desc;
            n = sizeof(config_desc);
        } else {
            return FO_USB_STALL;                /* no strings, qualifier */
        }
        break;
    case 0x8000: case 0x8100: case 0x8200:      /* GET_STATUS */
        reply = two;
        n = 2;
        break;
    case 0x8008:                                /* GET_CONFIGURATION */
        reply = &m->config;
        n = 1;
        break;
    case 0x810a:                                /* GET_INTERFACE */
        reply = two;
        n = 1;
        break;
    case 0x0005:                                /* SET_ADDRESS */
        m->new_addr = val & 0x7f;
        m->after = AFTER_ADDRESS;
        break;
    case 0x0009:                                /* SET_CONFIGURATION */
        if (val > 1) {
            return FO_USB_STALL;
        }
        m->config = val;
        break;
    case 0x010b:                                /* SET_INTERFACE */
        if (val || idx) {
            return FO_USB_STALL;
        }
        break;
    case 0x0001: case 0x0003:                   /* (CLEAR_|SET_)FEATURE */
    case 0x0201: case 0x0203:
        break;
    case 0x400c:                                /* the maskrom protocol */
        if (idx != 0x471 && idx != 0x472) {
            return FO_USB_STALL;
        }
        if (m->code != idx) {                   /* (another's: a new one) */
            maskrom_drop(m);
            m->code = idx;
        }
        m->out_len = len;
        if (len < CHUNK) {
            m->after = AFTER_CHUNK;             /* its last request */
        }
        break;
    default:
        return FO_USB_STALL;
    }
    if (reply) {
        m->reply_len = MIN(n, len);
        memcpy(m->reply, reply, m->reply_len);
    }
    m->ctl = true;
    return FO_USB_ACK;
}

/* the status stage done: what the transfer asked for takes effect */
static void maskrom_status_done(RkMaskrom *m)
{
    m->ctl = false;
    if (m->after == AFTER_ADDRESS) {
        m->addr = m->new_addr;
    } else if (m->after == AFTER_CHUNK) {
        maskrom_end(m);
    }
}

static FoUsbResult maskrom_out(void *opaque, uint8_t addr, int ep,
                               const uint8_t *data, int len)
{
    RkMaskrom *m = opaque;
    size_t max;

    if (!m->on || addr != m->addr) {
        return FO_USB_TIMEOUT;
    }
    if (ep || !m->ctl) {
        return FO_USB_NAK;                      /* (nothing armed there) */
    }
    if (m->ctl_in) {                            /* an IN transfer's status */
        maskrom_status_done(m);
        return FO_USB_ACK;
    }
    if (len > (int)(m->out_len - m->out_got)) {
        return FO_USB_STALL;
    }
    m->out_got += len;
    if (m->code) {
        max = m->code == 0x471 ? SRAM_MAX : DRAM_MAX;
        if (m->payload->len + len > max) {
            m->too_big = true;
        } else {
            g_byte_array_append(m->payload, data, len);
        }
    }
    return FO_USB_ACK;
}

static FoUsbResult maskrom_in(void *opaque, uint8_t addr, int ep,
                              uint8_t *data, int *len)
{
    RkMaskrom *m = opaque;
    int n;

    *len = 0;
    if (!m->on || addr != m->addr) {
        return FO_USB_TIMEOUT;
    }
    if (ep || !m->ctl) {
        return FO_USB_NAK;
    }
    if (!m->ctl_in) {                           /* an OUT transfer's status */
        if (m->out_got < m->out_len) {
            return FO_USB_NAK;                  /* (its data still to come) */
        }
        maskrom_status_done(m);
        return FO_USB_ACK;
    }
    n = MIN(64, m->reply_len - m->reply_pos);   /* (0 at the end: a ZLP) */
    memcpy(data, m->reply + m->reply_pos, n);
    m->reply_pos += n;
    *len = n;
    return FO_USB_ACK;
}

const FoUsbDevOps rk_maskrom_usb_ops = {
    .speed = 3,                                 /* high speed */
    .pulled_up = maskrom_pulled_up,
    .irq_pending = maskrom_irq_pending,
    .bus_reset = maskrom_bus_reset,
    .detach = maskrom_detach,
    .setup = maskrom_setup,
    .out = maskrom_out,
    .in = maskrom_in,
};

/* ------------------------------------------------------------ the board */

void rk_maskrom_init(RkMaskrom *m)
{
    m->payload = g_byte_array_new();
    m->run_payload = g_byte_array_new();
    m->bh = qemu_bh_new(maskrom_run, m);
}

void rk_maskrom_set(RkMaskrom *m, bool on)
{
    if (on == m->on) {
        return;
    }
    m->on = on;
    maskrom_bus_reset(m);
    if (!on) {
        qemu_bh_cancel(m->bh);
    }
    if (m->pullup_fn) {
        m->pullup_fn(m->pullup_opaque, on);
    }
}
