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
 * 0x472 code that the board does not run as SPL is a usbplug, rkdeveloptool's
 * download-boot's second part, and the board plays it (rk_maskrom_loader()):
 * the device goes from the bus a moment, as the usbplug starts USB0 again,
 * and comes back in Rockusb's Loader mode (bcdUSB 2.01, its bit 0 set: what
 * tells a tool so). That is bulk-only transport: a 31-byte command block
 * (USBC) on the bulk OUT endpoint, its data, a 13-byte status (USBS) on the
 * bulk IN endpoint. Its commands, Rockchip's: test unit ready, the flash's
 * ID and size (flash info), the chip's info, the capabilities (direct LBA,
 * a switch of storage), sectors of 512 bytes read, written and erased, the
 * storage got and switched (the UFS's LU 0, rockusb's 12, from the start;
 * the microSD card, 2), and a reset, the board's to do (its subcode: 3 into
 * the MaskROM, 2 off). Others fail.
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
#include "qemu/timer.h"
#include "system/block-backend.h"
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

/* the Loader mode: a command's phase */
enum { LD_CBW, LD_IN, LD_OUT, LD_CSW };

#define SECTOR          512
#define BULK_MPS        512
#define LD_DATA_MAX     (32 * MiB)      /* a command's data at most */
#define LD_BACK_NS      (100 * SCALE_MS)   /* the usbplug's start */
#define LD_RESET_NS     (20 * SCALE_MS)    /* ... its reset, after a status */
#define MEDIA_UFS       12              /* (rockusb's storage indexes) */
#define MEDIA_SD        2

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
    RkMaskrom *m = opaque;

    return m->on && !m->away;
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
    m->phase = LD_CBW;
    m->reset_due = false;
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

    if (!m->on || m->away || addr != m->addr) {
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
            if (m->loader) {
                memcpy(m->reply, dev_desc, n);
                m->reply[2] = 0x01;             /* bcdUSB 2.01: a Loader */
                reply = m->reply;
            }
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
        if (m->loader || (idx != 0x471 && idx != 0x472)) {
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
        memmove(m->reply, reply, m->reply_len);
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

/* ------------------------------------------------- the Loader mode */

static BlockBackend *loader_blk(RkMaskrom *m)
{
    return m->storage[m->media == MEDIA_SD];
}

static int64_t loader_sectors(RkMaskrom *m)
{
    BlockBackend *blk = loader_blk(m);
    int64_t len = blk ? blk_getlength(blk) : 0;

    return len > 0 ? len / SECTOR : 0;
}

/* the command done: its status, and the data it leaves unmoved */
static void loader_status(RkMaskrom *m, bool ok, uint32_t residue)
{
    memcpy(m->csw, "USBS", 4);                  /* (the tag stays: 4-7) */
    stl_le_p(m->csw + 8, residue);
    m->csw[12] = ok ? 0 : 1;
    m->phase = LD_CSW;
}

/* data to give: what the command put in m->buf, @len bytes of it, as much
 * as the host asked for (@want) */
static void loader_give(RkMaskrom *m, uint32_t want, uint32_t len)
{
    m->buf_len = MIN(want, len);
    m->buf_pos = 0;
    m->residue = want - m->buf_len;
    m->phase = LD_IN;
    if (!m->buf_len) {
        loader_status(m, true, m->residue);
    }
}

/* data to take: @want bytes, for m->at (-1: to drop, the command failed) */
static void loader_take(RkMaskrom *m, uint32_t want, int64_t at)
{
    m->at = at;
    m->buf_len = want;
    m->buf_pos = 0;
    m->phase = LD_OUT;
    if (!want) {
        loader_status(m, at >= 0, 0);
    }
}

static bool loader_buf(RkMaskrom *m, uint32_t len)
{
    if (len > LD_DATA_MAX) {
        return false;
    }
    g_free(m->buf);
    m->buf = g_malloc0(MAX(len, 16));
    return true;
}

/* a command block: what it asks, done (or its data to come first) */
static FoUsbResult loader_command(RkMaskrom *m, const uint8_t *cbw, int len)
{
    uint32_t want, lba;
    uint16_t count;
    uint8_t code, op;
    int64_t sectors;
    BlockBackend *blk;
    bool ok;

    if (len != 31 || memcmp(cbw, "USBC", 4)) {
        return FO_USB_STALL;
    }
    memcpy(m->csw + 4, cbw + 4, 4);             /* its tag, as it came */
    want = ldl_le_p(cbw + 8);
    code = cbw[15];
    op = cbw[16];
    lba = ldl_be_p(cbw + 17);
    count = lduw_be_p(cbw + 22);
    blk = loader_blk(m);
    sectors = loader_sectors(m);

    switch (code) {
    case 0x00:                                  /* TEST UNIT READY */
        loader_status(m, blk != NULL, 0);
        break;
    case 0x01:                                  /* the flash's ID: the */
        loader_buf(m, 5);                       /* usbplug's names (the UFS
                                                   a SCSI disk to its U-Boot,
                                                   which says SATA; the card
                                                   an MMC one) */
        memcpy(m->buf, m->media == MEDIA_SD ? "EMMC " : "SATA ", 5);
        loader_give(m, want, 5);
        break;
    case 0x1a:                                  /* the flash's info */
        loader_buf(m, 11);
        stl_le_p(m->buf, sectors);
        stw_le_p(m->buf + 4, 0x400);            /* blocks of 512 KiB */
        m->buf[6] = 4;                          /* pages of 2 KiB */
        m->buf[8] = 40;                         /* (access time) */
        m->buf[10] = 1;                         /* (its chip select) */
        loader_give(m, want, 11);
        break;
    case 0x1b:                                  /* the chip's info */
        loader_buf(m, 16);
        memcpy(m->buf, "6753", 4);              /* RK3576, as words are */
        loader_give(m, want, 16);
        break;
    case 0xaa:                                  /* the capabilities */
        loader_buf(m, 8);
        m->buf[0] = 0x01 | 0x08;                /* direct LBA, read LBA */
        m->buf[1] = 0x02;                       /* a switch of storage */
        loader_give(m, want, 8);
        break;
    case 0x2b:                                  /* the storage, got */
        loader_buf(m, 4);
        stl_le_p(m->buf, 1u << m->media);
        loader_give(m, want, 4);
        break;
    case 0x2a:                                  /* ... switched */
        if ((op == MEDIA_UFS && m->storage[0]) ||
            (op == MEDIA_SD && m->storage[1])) {
            m->media = op;
            loader_status(m, true, 0);
        } else {
            loader_status(m, false, 0);
        }
        break;
    case 0x14:                                  /* sectors read */
        if (!blk || (uint64_t)lba + count > (uint64_t)sectors ||
            want != (uint32_t)count * SECTOR || !loader_buf(m, want) ||
            blk_pread(blk, (int64_t)lba * SECTOR, want, m->buf, 0) < 0) {
            loader_status(m, false, want);
            break;
        }
        loader_give(m, want, want);
        break;
    case 0x15:                                  /* ... written */
        ok = blk && (uint64_t)lba + count <= (uint64_t)sectors &&
             want == (uint32_t)count * SECTOR && loader_buf(m, want);
        loader_take(m, want, ok ? (int64_t)lba * SECTOR : -1);
        break;
    case 0x25:                                  /* ... erased */
        loader_status(m, blk && (uint64_t)lba + count <= (uint64_t)sectors &&
                      blk_pwrite_zeroes(blk, (int64_t)lba * SECTOR,
                                        (int64_t)count * SECTOR,
                                        BDRV_REQ_MAY_UNMAP) >= 0, 0);
        break;
    case 0xff:                                  /* a reset: the board's, */
        loader_status(m, true, 0);              /* as its subcode asks */
        m->reset_due = true;
        m->reset_op = op;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rk3576: Loader mode: command 0x%02x "
                      "is not played\n", code);
        if (cbw[12] & 0x80) {
            loader_status(m, false, want);      /* (its status at once) */
        } else {
            loader_take(m, want, -1);           /* (its data dropped) */
        }
        break;
    }
    return FO_USB_ACK;
}

static FoUsbResult loader_out(RkMaskrom *m, const uint8_t *data, int len)
{
    uint32_t take;
    bool ok;

    if (m->phase == LD_CBW) {
        return loader_command(m, data, len);
    }
    if (m->phase != LD_OUT) {
        return FO_USB_NAK;                      /* (its status first) */
    }
    take = MIN((uint32_t)len, m->buf_len - m->buf_pos);
    if (m->at >= 0) {
        memcpy(m->buf + m->buf_pos, data, take);
    }
    m->buf_pos += take;
    if (m->buf_pos == m->buf_len) {
        ok = m->at >= 0 && blk_pwrite(loader_blk(m), m->at, m->buf_len,
                                      m->buf, 0) >= 0;
        loader_status(m, ok, ok ? 0 : m->buf_len);
    }
    return FO_USB_ACK;
}

static FoUsbResult loader_in(RkMaskrom *m, uint8_t *data, int *len)
{
    uint32_t n;

    if (m->phase == LD_IN) {
        n = MIN(BULK_MPS, m->buf_len - m->buf_pos);
        memcpy(data, m->buf + m->buf_pos, n);
        m->buf_pos += n;
        *len = n;
        if (m->buf_pos == m->buf_len) {
            loader_status(m, true, m->residue);
        }
        return FO_USB_ACK;
    }
    if (m->phase == LD_CSW) {
        memcpy(data, m->csw, sizeof(m->csw));
        *len = sizeof(m->csw);
        m->phase = LD_CBW;
        if (m->reset_due) {                     /* (its status out first) */
            m->reset_due = false;
            timer_mod(m->reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      LD_RESET_NS);
        }
        return FO_USB_ACK;
    }
    return FO_USB_NAK;                          /* (a command first) */
}

static void loader_back(void *opaque)
{
    RkMaskrom *m = opaque;

    m->away = false;
    if (m->pullup_fn) {
        m->pullup_fn(m->pullup_opaque, true);
    }
}

static void loader_reset(void *opaque)
{
    RkMaskrom *m = opaque;

    if (m->on && m->loader && m->reset) {
        m->reset(m->reset_opaque, m->reset_op);
    }
}

static FoUsbResult maskrom_out(void *opaque, uint8_t addr, int ep,
                               const uint8_t *data, int len)
{
    RkMaskrom *m = opaque;
    size_t max;

    if (!m->on || m->away || addr != m->addr) {
        return FO_USB_TIMEOUT;
    }
    if (ep == 1 && m->loader && m->config) {
        return loader_out(m, data, len);
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
    if (!m->on || m->away || addr != m->addr) {
        return FO_USB_TIMEOUT;
    }
    if (ep == 1 && m->loader && m->config) {
        return loader_in(m, data, len);
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
    m->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, loader_reset, m);
    m->back = timer_new_ns(QEMU_CLOCK_VIRTUAL, loader_back, m);
}

void rk_maskrom_loader(RkMaskrom *m, BlockBackend *ufs, BlockBackend *sd)
{
    m->storage[0] = ufs;
    m->storage[1] = sd;
    m->media = ufs ? MEDIA_UFS : MEDIA_SD;
    m->loader = true;
    m->away = true;                             /* (gone a moment) */
    maskrom_bus_reset(m);
    if (m->pullup_fn) {
        m->pullup_fn(m->pullup_opaque, false);
    }
    timer_mod(m->back, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LD_BACK_NS);
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
        timer_del(m->reset_timer);
        timer_del(m->back);
        m->loader = m->away = m->reset_due = false;
        g_free(m->buf);
        m->buf = NULL;
    }
    if (m->pullup_fn) {
        m->pullup_fn(m->pullup_opaque, on);
    }
}
