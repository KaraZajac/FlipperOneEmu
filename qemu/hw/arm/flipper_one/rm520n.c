/*
 * A 5G modem in the Flipper One's M.2 slot: a Quectel RM520N-GL as Linux
 * sees one, its QMI interface on USB (the board's hub, port 4)
 *
 * Its one interface is vendor-specific (ff/ff/ff), as the RM520N's QMI one
 * is, which Linux's qmi_wwan takes (2c7c:0801): an interrupt endpoint for
 * CDC-WDM's notifications, and bulk endpoints for its data. Its serial
 * ports (diagnostics, NMEA, AT) are not modelled.
 *
 * QMI, as CDC-WDM carries it: a message comes as SEND_ENCAPSULATED_COMMAND
 * (a QMUX frame: its service and client, then the message: a transaction,
 * its ID, TLVs); each answer, and each indication, is queued, announced
 * with a RESPONSE_AVAILABLE notification, and taken with
 * GET_ENCAPSULATED_RESPONSE. A message it does not know is answered with
 * failure, "invalid QMI command", as a modem's firmware does.
 *
 * Its data: WDS's network interface, raw IP on its bulk endpoints (WDA's
 * data format), through its netdev (a -nic of model rm520n: passt) as
 * Ethernet frames. Its addresses are the netdev's: the modem asks passt's
 * DHCP for them as it powers on (from a MAC of its own), and gives them as
 * WDS's current settings; it answers passt's ARP for that address. Without
 * a netdev, or before passt has answered, a call fails. IPv4 only: an IPv6
 * call is refused, as on a network without IPv6.
 *
 * The M.2 slot's power and reset are the board's (FULL_CARD_POWER_OFF#
 * and RESET#, which Linux's tree holds released): the modem is on USB
 * while both are.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "net/net.h"
#include "net/eth.h"
#include "flipper_one.h"

OBJECT_DECLARE_SIMPLE_TYPE(RM520NState, RM520N)

#define QMI_IFACE       0
#define INTR_MPS        8
#define RESP_MAX        64              /* answers waiting, at most */

/* its services */
enum {
    SVC_CTL = 0x00, SVC_WDS = 0x01, SVC_DMS = 0x02, SVC_NAS = 0x03,
    SVC_UIM = 0x0b, SVC_WDA = 0x1a,
};

/* QMI's errors (libqmi's QmiProtocolError) */
#define QMI_ERR_NONE            0x0000
#define QMI_ERR_INVALID_ARG     0x0030
#define QMI_ERR_NOT_PROVISIONED 0x0010
#define QMI_ERR_INVALID_CMD     0x0047
#define QMI_ERR_SIM_FILE_NOT_FOUND 0x0050
#define QMI_ERR_CALL_FAILED     0x000e
#define QMI_ERR_OUT_OF_CALL     0x000f
#define DOWN_MAX        256             /* packets for the host, at most */
#define CALL_MTU        1500

/* a call's rates, as WDS gives them (bit/s): LTE with NR (EN-DC), and the
 * RM520N's most in that */
#define RATE_TX         75000000u
#define RATE_RX         600000000u
#define RATE_TX_MAX     550000000u
#define RATE_RX_MAX     3400000000u

/* its SIM: a USIM, ready, its PIN disabled; on MCC 001, MNC 01 (the ITU's
 * test network) */
#define SIM_ICCID       "8900101234567890123"
#define SIM_IMSI        "001010123456789"
#define SIM_SPN         "FlipperOneEmu"

/* its network: LTE on band 3 with an NR cell beside it (EN-DC: 5G NSA) */
#define NET_MCC         1
#define NET_MNC         1
#define NET_NAME        "FlipperOneEmu"
#define NET_TAC         10783
#define NET_ECI         0x01a2b03u          /* (eNB 6699, cell 3) */
#define NET_EARFCN      1850                /* (band 3, 1800 MHz) */
#define NET_PCI         101
#define NET_NR_ARFCN    636666              /* (band n78) */
#define SIG_RSSI        (-65)               /* dBm */
#define SIG_RSRQ        (-9)                /* dB */
#define SIG_RSRP        (-78)               /* dBm */
#define SIG_SNR         18                  /* dB */
#define SIG_NR_RSRP     (-82)
#define SIG_NR_SNR      15
#define SIG_NR_RSRQ     (-10)

struct RM520NState {
    USBDevice parent_obj;

    USBEndpoint *intr;
    GQueue resp;                        /* QMUX frames for the host */
    int notify;                         /* RESPONSE_AVAILABLE owed */
    uint8_t next_cid[256];              /* each service's last client ID */
    uint8_t opmode;                     /* DMS's operating mode */
    uint32_t link_layer;                /* WDA's: 1 802.3, 2 raw IP */

    /* its data */
    NICConf conf;
    NICState *nic;
    USBEndpoint *bulk_in;
    GQueue down;                        /* IP packets for the host */
    bool connected;                     /* WDS's call up */
    uint8_t call_cid;                   /* (the client that made it) */
    uint8_t family[256];                /* each WDS client's IP family */
    uint32_t tx_pkts, rx_pkts, rx_dropped;  /* the call's */
    uint64_t tx_bytes, rx_bytes;
    bool dhcp_done;                     /* passt's addresses known */
    uint32_t dhcp_xid;
    uint8_t peer[6];                    /* passt's MAC */
    uint32_t ip, gw, mask, dns[2];      /* (network order) */
};

/* ------------------------------------------------------------ USB side */

enum { STR_MANUFACTURER = 1, STR_PRODUCT, STR_SERIAL };

static const USBDescStrings rm520n_strings = {
    [STR_MANUFACTURER]  = "Quectel",
    [STR_PRODUCT]       = "RM520N-GL",
    [STR_SERIAL]        = "f1e00001",
};

#define QMI_IFACE_DESC(bulk, ival) {                                    \
        .bInterfaceNumber = QMI_IFACE, .bNumEndpoints = 3,              \
        .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff,            \
        .bInterfaceProtocol = 0xff,                                     \
        .eps = (USBDescEndpoint[]) {                                    \
            { .bEndpointAddress = USB_DIR_IN | 1,                       \
              .bmAttributes = USB_ENDPOINT_XFER_INT,                    \
              .wMaxPacketSize = INTR_MPS, .bInterval = (ival) },        \
            { .bEndpointAddress = USB_DIR_IN | 2,                       \
              .bmAttributes = USB_ENDPOINT_XFER_BULK,                   \
              .wMaxPacketSize = (bulk) },                               \
            { .bEndpointAddress = USB_DIR_OUT | 2,                      \
              .bmAttributes = USB_ENDPOINT_XFER_BULK,                   \
              .wMaxPacketSize = (bulk) } } }

static const USBDescIface qmi_iface_fs = QMI_IFACE_DESC(64, 32);
static const USBDescIface qmi_iface_hs = QMI_IFACE_DESC(512, 9);

static const USBDescDevice rm520n_dev_fs = {
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) { {
        .bNumInterfaces = 1, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower = 250,
        .nif = 1, .ifs = &qmi_iface_fs,
    } },
};
static const USBDescDevice rm520n_dev_hs = {
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) { {
        .bNumInterfaces = 1, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_WAKEUP,
        .bMaxPower = 250,
        .nif = 1, .ifs = &qmi_iface_hs,
    } },
};

static const USBDesc rm520n_desc = {
    .id = {
        .idVendor = 0x2c7c, .idProduct = 0x0801, .bcdDevice = 0x0414,
        .iManufacturer = STR_MANUFACTURER, .iProduct = STR_PRODUCT,
        .iSerialNumber = STR_SERIAL,
    },
    .full = &rm520n_dev_fs,
    .high = &rm520n_dev_hs,
    .str = rm520n_strings,
};

/* ------------------------------------------------------------ QMI's TLVs */

typedef struct {
    uint8_t b[2048];
    int n;
} QBuf;

static void tlv(QBuf *q, uint8_t type, const void *v, int len)
{
    if (q->n + 3 + len > (int)sizeof(q->b)) {
        return;
    }
    q->b[q->n] = type;
    stw_le_p(q->b + q->n + 1, len);
    memcpy(q->b + q->n + 3, v, len);
    q->n += 3 + len;
}

static void tlv_u8(QBuf *q, uint8_t type, uint8_t v)
{
    tlv(q, type, &v, 1);
}

static void tlv_u16(QBuf *q, uint8_t type, uint16_t v)
{
    uint8_t b[2];

    stw_le_p(b, v);
    tlv(q, type, b, 2);
}

static void tlv_u32(QBuf *q, uint8_t type, uint32_t v)
{
    uint8_t b[4];

    stl_le_p(b, v);
    tlv(q, type, b, 4);
}

static void tlv_str(QBuf *q, uint8_t type, const char *s)
{
    tlv(q, type, s, strlen(s));
}

/* the result TLV (0x02): success, or failure with @err */
static void tlv_result(QBuf *q, uint16_t err)
{
    uint8_t b[4];

    stw_le_p(b, err ? 1 : 0);
    stw_le_p(b + 2, err);
    tlv(q, 0x02, b, 4);
}

/* a TLV of the request's, or NULL; its length in *len */
static const uint8_t *tlv_find(const uint8_t *t, int n, uint8_t type,
                               int *len)
{
    while (n >= 3) {
        int l = lduw_le_p(t + 1);

        if (3 + l > n) {
            break;
        }
        if (t[0] == type) {
            *len = l;
            return t + 3;
        }
        t += 3 + l;
        n -= 3 + l;
    }
    return NULL;
}

/* ------------------------------------------------- QMUX, and CDC-WDM's */

/* a QMUX frame for the host: a response (or an indication) of @svc's to
 * client @cid, transaction @tid, message @msg, with @q's TLVs */
static void qmi_send(RM520NState *s, uint8_t svc, uint8_t cid, uint16_t tid,
                     uint16_t msg, bool ind, const QBuf *q)
{
    int hdr = svc == SVC_CTL ? 6 : 7;
    GByteArray *f;
    uint8_t *b;

    if (g_queue_get_length(&s->resp) >= RESP_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "rm520n: QMI answers not taken\n");
        return;
    }
    f = g_byte_array_sized_new(6 + hdr + q->n);
    g_byte_array_set_size(f, 6 + hdr + q->n);
    b = f->data;
    b[0] = 0x01;                                /* (QMUX) */
    stw_le_p(b + 1, 5 + hdr + q->n);
    b[3] = 0x80;                                /* (from the service) */
    b[4] = svc;
    b[5] = cid;
    if (svc == SVC_CTL) {
        b[6] = ind ? 0x02 : 0x01;
        b[7] = tid;
        stw_le_p(b + 8, msg);
        stw_le_p(b + 10, q->n);
    } else {
        b[6] = ind ? 0x04 : 0x02;
        stw_le_p(b + 7, tid);
        stw_le_p(b + 9, msg);
        stw_le_p(b + 11, q->n);
    }
    memcpy(b + 6 + hdr, q->b, q->n);
    g_queue_push_tail(&s->resp, f);
    s->notify++;
    usb_wakeup(s->intr, 0);
}

/* a request's answer: its result, then @q's TLVs (a failure's too: why) */
static void qmi_answer(RM520NState *s, uint8_t svc, uint8_t cid, uint16_t tid,
                       uint16_t msg, uint16_t err, const QBuf *q)
{
    QBuf a = { .n = 0 };

    if (err == QMI_ERR_INVALID_CMD) {
        qemu_log_mask(LOG_UNIMP | LOG_GUEST_ERROR, "rm520n: QMI service "
                      "0x%02x message 0x%04x: not supported\n", svc, msg);
    }
    tlv_result(&a, err);
    if (q) {
        memcpy(a.b + a.n, q->b, q->n);
        a.n += q->n;
    }
    qmi_send(s, svc, cid, tid, msg, false, &a);
}

/* ------------------------------------------------------------- services */

/* CTL: its services' versions, client IDs, sync */
static void qmi_ctl(RM520NState *s, uint8_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    static const struct {
        uint8_t svc;
        uint16_t major, minor;
    } versions[] = {
        { SVC_CTL, 1, 5 }, { SVC_WDS, 1, 67 }, { SVC_DMS, 1, 14 },
        { SVC_NAS, 1, 25 }, { SVC_UIM, 1, 46 }, { SVC_WDA, 1, 16 },
    };
    QBuf q = { .n = 0 };
    const uint8_t *v;
    uint8_t b[64];
    int len, k;

    switch (msg) {
    case 0x0021:                                /* Get Version Info */
        b[0] = ARRAY_SIZE(versions);
        k = 1;
        for (int i = 0; i < ARRAY_SIZE(versions); i++) {
            b[k] = versions[i].svc;
            stw_le_p(b + k + 1, versions[i].major);
            stw_le_p(b + k + 3, versions[i].minor);
            k += 5;
        }
        tlv(&q, 0x01, b, k);
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_NONE, &q);
        break;
    case 0x0022:                                /* Allocate CID */
        v = tlv_find(t, n, 0x01, &len);
        if (!v || len < 1) {
            qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_INVALID_ARG, NULL);
            break;
        }
        b[0] = v[0];
        b[1] = ++s->next_cid[v[0]];
        if (!b[1]) {                            /* (0 is no client) */
            b[1] = s->next_cid[v[0]] = 1;
        }
        if (v[0] == SVC_WDS) {
            s->family[b[1]] = 0;
        }
        tlv(&q, 0x01, b, 2);
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_NONE, &q);
        break;
    case 0x0023:                                /* Release CID */
        v = tlv_find(t, n, 0x01, &len);
        if (!v || len < 2) {
            qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_INVALID_ARG, NULL);
            break;
        }
        if (v[0] == SVC_WDS && v[1] == s->call_cid) {
            s->connected = false;               /* (its call goes with it) */
        }
        tlv(&q, 0x01, v, 2);
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_NONE, &q);
        break;
    case 0x0020:                                /* Set Instance ID */
        tlv_u16(&q, 0x01, 0x0000);
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_NONE, &q);
        break;
    case 0x0026:                                /* Set Data Format */
    case 0x0027:                                /* Sync */
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_NONE, NULL);
        break;
    default:
        qmi_answer(s, SVC_CTL, 0, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        break;
    }
}

/* the call ended by the modem (@why: WDS's call end reason): WDS's Packet
 * Service Status indication, to the client that made it */
static void wds_drop(RM520NState *s, uint16_t why)
{
    QBuf q = { .n = 0 };
    uint8_t st[2] = { 1, 0 };                   /* disconnected */

    if (!s->connected) {
        return;
    }
    s->connected = false;
    tlv(&q, 0x01, st, 2);
    tlv_u16(&q, 0x10, why);
    qmi_send(s, SVC_WDS, s->call_cid, 0, 0x0022, true, &q);
}

/* DMS: the device */
static void qmi_dms(RM520NState *s, uint8_t cid, uint16_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    QBuf q = { .n = 0 };
    const uint8_t *v;
    uint8_t b[32];
    int len;

    switch (msg) {
    case 0x0020:                                /* Get Capabilities */
        stl_le_p(b, 2500000);                   /* (kbit/s: up, down) */
        stl_le_p(b + 4, 5000000);
        b[8] = 3;                               /* simultaneous CS and PS */
        b[9] = 2;                               /* a SIM supported */
        b[10] = 3;                              /* UMTS, LTE, 5G NR */
        b[11] = 5;
        b[12] = 8;
        b[13] = 10;
        tlv(&q, 0x01, b, 14);
        break;
    case 0x0021:                                /* Get Manufacturer */
        tlv_str(&q, 0x01, "Quectel");
        break;
    case 0x0022:                                /* Get Model */
        tlv_str(&q, 0x01, "RM520N-GL");
        break;
    case 0x0023:                                /* Get Revision */
        tlv_str(&q, 0x01, "RM520NGLAAR03A07M4G");
        break;
    case 0x002c:                                /* Get Hardware Revision */
        tlv_str(&q, 0x01, "10000");
        break;
    case 0x0051:                                /* Get Software Version */
        tlv_str(&q, 0x01, "RM520NGLAAR03A07M4G_01.200.01.200");
        break;
    case 0x0025:                                /* Get IDs: the IMEI */
        tlv_str(&q, 0x11, "866758040612340");
        tlv_str(&q, 0x13, "01");
        break;
    case 0x002d:                                /* Get Operating Mode */
        tlv_u8(&q, 0x01, s->opmode);
        tlv_u8(&q, 0x11, 0);                    /* (not restricted) */
        break;
    case 0x002e:                                /* Set Operating Mode */
        v = tlv_find(t, n, 0x01, &len);
        if (!v || len < 1) {
            qmi_answer(s, SVC_DMS, cid, tid, msg, QMI_ERR_INVALID_ARG, NULL);
            return;
        }
        s->opmode = v[0];
        if (s->opmode != 0) {                   /* (no service: no call) */
            wds_drop(s, 3);
        }
        break;
    case 0x0040:                                /* UIM Get CK Status */
        b[0] = 0;                               /* (no lock on the SIM) */
        b[1] = 0;
        b[2] = 0;
        tlv(&q, 0x01, b, 3);
        break;
    case 0x003c:                                /* UIM Get ICCID */
        tlv_str(&q, 0x01, SIM_ICCID);
        break;
    case 0x0043:                                /* UIM Get IMSI */
        tlv_str(&q, 0x01, SIM_IMSI);
        break;
    case 0x002b:                                /* UIM Get PIN Status */
        b[0] = 3;                               /* PIN1 disabled */
        b[1] = 3;
        b[2] = 10;
        tlv(&q, 0x11, b, 3);
        tlv(&q, 0x12, b, 3);
        break;
    case 0x0044:                                /* UIM Get State: ready */
        tlv_u8(&q, 0x01, 0);
        break;
    case 0x0024:                                /* Get MSISDN: none yet */
        qmi_answer(s, SVC_DMS, cid, tid, msg, QMI_ERR_NOT_PROVISIONED, NULL);
        return;
    default:
        qmi_answer(s, SVC_DMS, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        return;
    }
    qmi_answer(s, SVC_DMS, cid, tid, msg, QMI_ERR_NONE, &q);
}

/* the SIM's files (EF_ICCID, EF_IMSI, EF_SPN, EF_AD), as a card has them */
static const uint8_t ef_iccid[] = { 0x98, 0x00, 0x01, 0x21, 0x43, 0x65,
                                    0x87, 0x09, 0x21, 0xf3 };
static const uint8_t ef_imsi[] = { 0x08, 0x09, 0x10, 0x10, 0x10, 0x32, 0x54,
                                   0x76, 0x98 };
static const uint8_t ef_ad[] = { 0x00, 0x00, 0x00, 0x02 };

/* UIM: the SIM card, its USIM application ready, its PIN disabled */
static void qmi_uim(RM520NState *s, uint8_t cid, uint16_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    static const uint8_t aid[16] = { 0xa0, 0x00, 0x00, 0x00, 0x87, 0x10,
                                     0x02, 0xff, 0x33, 0xff, 0x01, 0x89,
                                     0x06, 0x01, 0x00, 0xff };
    QBuf q = { .n = 0 };
    const uint8_t *v;
    uint8_t b[64];
    int len, k;

    switch (msg) {
    case 0x002f:                                /* Get Card Status */
        stw_le_p(b, 0x0000);                    /* GW primary: card 0, app 0 */
        stw_le_p(b + 2, 0xffff);                /* (no 1x, no secondaries) */
        stw_le_p(b + 4, 0xffff);
        stw_le_p(b + 6, 0xffff);
        b[8] = 1;                               /* one card: present, */
        b[9] = 1;
        b[10] = 0;                              /* no UPIN */
        b[11] = 0;
        b[12] = 0;
        b[13] = 0;
        b[14] = 1;                              /* one application: */
        b[15] = 2;                              /* a USIM, ready, */
        b[16] = 7;
        b[17] = 2;                              /* (personalization ready) */
        b[18] = 0;
        b[19] = 0;
        b[20] = 0;
        b[21] = sizeof(aid);
        memcpy(b + 22, aid, sizeof(aid));
        k = 22 + sizeof(aid);
        b[k++] = 0;                             /* (UPIN not PIN1's) */
        b[k++] = 3;                             /* PIN1 disabled, */
        b[k++] = 3;                             /* its retries 3, PUK 10 */
        b[k++] = 10;
        b[k++] = 3;                             /* PIN2 disabled */
        b[k++] = 3;
        b[k++] = 10;
        tlv(&q, 0x10, b, k);
        break;
    case 0x0020:                                /* Read Transparent */
        v = tlv_find(t, n, 0x02, &len);
        if (!v || len < 2) {
            qmi_answer(s, SVC_UIM, cid, tid, msg, QMI_ERR_INVALID_ARG, NULL);
            return;
        }
        switch (lduw_le_p(v)) {
        case 0x2fe2:
            v = ef_iccid, len = sizeof(ef_iccid);
            break;
        case 0x6f07:
            v = ef_imsi, len = sizeof(ef_imsi);
            break;
        case 0x6f46:                            /* (the name, padded) */
            memset(b, 0xff, 17);
            b[0] = 0x01;
            memcpy(b + 1, SIM_SPN, strlen(SIM_SPN));
            v = b, len = 17;
            break;
        case 0x6fad:
            v = ef_ad, len = sizeof(ef_ad);
            break;
        default:
            qmi_answer(s, SVC_UIM, cid, tid, msg, QMI_ERR_SIM_FILE_NOT_FOUND,
                       NULL);
            return;
        }
        {
            uint8_t sw[2] = { 0x90, 0x00 }, r[64];

            tlv(&q, 0x10, sw, 2);
            stw_le_p(r, len);
            memcpy(r + 2, v, len);
            tlv(&q, 0x11, r, 2 + len);
        }
        break;
    case 0x002e:                                /* Register Events */
        v = tlv_find(t, n, 0x01, &len);
        tlv_u32(&q, 0x10, v && len >= 4 ? ldl_le_p(v) : 0);
        break;
    default:
        qmi_answer(s, SVC_UIM, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        return;
    }
    qmi_answer(s, SVC_UIM, cid, tid, msg, QMI_ERR_NONE, &q);
}

/* NAS: registered (while online) on its network, LTE with an NR cell */
static void qmi_nas(RM520NState *s, uint8_t cid, uint16_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    bool online = s->opmode == 0;
    QBuf q = { .n = 0 };
    uint8_t b[64];
    int k;

    switch (msg) {
    case 0x0002:                                /* Set Event Report */
    case 0x0003:                                /* Register Indications */
    case 0x0033:                                /* Set System Selection
                                                   Preference */
    case 0x0050:                                /* Config Signal Info */
    case 0x006c:                                /* ... v2 */
        break;
    case 0x0024:                                /* Get Serving System */
        b[0] = online ? 1 : 0;                  /* registered, or not */
        b[1] = online ? 1 : 2;                  /* CS, PS attached */
        b[2] = online ? 1 : 2;
        b[3] = 2;                               /* 3GPP */
        b[4] = online ? 1 : 0;                  /* LTE */
        b[5] = 8;
        tlv(&q, 0x01, b, online ? 6 : 5);
        if (!online) {
            break;
        }
        tlv_u8(&q, 0x10, 1);                    /* (roaming off) */
        b[0] = 1;
        b[1] = 0x0b;                            /* (data: LTE) */
        tlv(&q, 0x11, b, 2);
        stw_le_p(b, NET_MCC);
        stw_le_p(b + 2, NET_MNC);
        b[4] = strlen(NET_NAME);
        memcpy(b + 5, NET_NAME, b[4]);
        tlv(&q, 0x12, b, 5 + b[4]);
        tlv_u32(&q, 0x1d, NET_ECI);
        tlv_u16(&q, 0x24, NET_TAC);
        break;
    case 0x004d:                                /* Get System Info */
        b[0] = online ? 2 : 0;                  /* LTE: available */
        b[1] = b[0];
        b[2] = online;                          /* (the preferred data path) */
        tlv(&q, 0x14, b, 3);
        if (!online) {
            break;
        }
        memset(b, 0, 29);
        b[0] = 1;                               /* domain: CS and PS */
        b[1] = 3;
        b[2] = 1;                               /* (its capability, the same) */
        b[3] = 3;
        b[4] = 1;                               /* roaming off */
        b[5] = 0;
        b[6] = 1;                               /* not forbidden */
        b[7] = 0;
        b[8] = 0;                               /* (no LAC) */
        b[11] = 1;                              /* its cell */
        stl_le_p(b + 12, NET_ECI);
        b[16] = 0;                              /* (no reject) */
        b[19] = 1;                              /* its network: 001, 01 */
        memcpy(b + 20, "001", 3);
        memcpy(b + 23, "01\xff", 3);
        b[26] = 1;                              /* its TAC */
        stw_le_p(b + 27, NET_TAC);
        tlv(&q, 0x19, b, 29);
        tlv_u8(&q, 0x4e, 1);                    /* EN-DC: an NR cell */
        break;
    case 0x004f:                                /* Get Signal Info */
        if (!online) {
            break;
        }
        b[0] = (uint8_t)SIG_RSSI;               /* LTE */
        b[1] = (uint8_t)SIG_RSRQ;
        stw_le_p(b + 2, SIG_RSRP);
        stw_le_p(b + 4, SIG_SNR * 10);
        tlv(&q, 0x14, b, 6);
        stw_le_p(b, SIG_NR_RSRP);               /* NR */
        stw_le_p(b + 2, SIG_NR_SNR * 10);
        tlv(&q, 0x17, b, 4);
        tlv_u16(&q, 0x18, SIG_NR_RSRQ);
        break;
    case 0x0020:                                /* Get Signal Strength */
        if (!online) {
            qmi_answer(s, SVC_NAS, cid, tid, msg, 0x001d, NULL);  /* (no
                                                   service) */
            return;
        }
        b[0] = (uint8_t)SIG_RSSI;
        b[1] = 8;
        tlv(&q, 0x01, b, 2);
        b[0] = (uint8_t)SIG_RSRQ;
        tlv(&q, 0x16, b, 2);
        tlv_u16(&q, 0x17, SIG_SNR * 10);
        tlv_u16(&q, 0x18, SIG_RSRP);
        break;
    case 0x0043:                                /* Get Cell Location Info */
        if (!online) {
            break;
        }
        b[0] = 0;                               /* (connected) */
        b[1] = 0x00;                            /* PLMN 001 01, as BCD */
        b[2] = 0xf1;
        b[3] = 0x10;
        stw_le_p(b + 4, NET_TAC);
        stl_le_p(b + 6, NET_ECI);
        stw_le_p(b + 10, NET_EARFCN);
        stw_le_p(b + 12, NET_PCI);
        b[14] = 5;                              /* (reselection: priority, */
        b[15] = 10;                             /* thresholds) */
        b[16] = 4;
        b[17] = 30;
        b[18] = 1;                              /* the cell, measured */
        k = 19;
        stw_le_p(b + k, NET_PCI);
        stw_le_p(b + k + 2, SIG_RSRQ * 10);
        stw_le_p(b + k + 4, SIG_RSRP * 10);
        stw_le_p(b + k + 6, SIG_RSSI * 10);
        stw_le_p(b + k + 8, 40);
        tlv(&q, 0x13, b, k + 10);
        tlv_u32(&q, 0x2e, NET_NR_ARFCN);
        break;
    case 0x0025:                                /* Get Home Network */
        stw_le_p(b, NET_MCC);
        stw_le_p(b + 2, NET_MNC);
        b[4] = strlen(NET_NAME);
        memcpy(b + 5, NET_NAME, b[4]);
        tlv(&q, 0x01, b, 5 + b[4]);
        break;
    case 0x0044:                                /* Get PLMN Name: its */
        k = 0;                                  /* (in ASCII) */
        b[k++] = 1;
        b[k++] = strlen(NET_NAME);
        memcpy(b + k, NET_NAME, strlen(NET_NAME));
        k += strlen(NET_NAME);
        for (int i = 0; i < 2; i++) {           /* short, long */
            b[k++] = 1;
            b[k++] = 0;
            b[k++] = 0;
            b[k++] = strlen(NET_NAME);
            memcpy(b + k, NET_NAME, strlen(NET_NAME));
            k += strlen(NET_NAME);
        }
        tlv(&q, 0x10, b, k);
        break;
    case 0x0034:                                /* Get System Selection
                                                   Preference: UMTS, LTE,
                                                   5G NR */
        tlv_u16(&q, 0x11, 1 << 3 | 1 << 4 | 1 << 6);
        break;
    default:
        qmi_answer(s, SVC_NAS, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        return;
    }
    qmi_answer(s, SVC_NAS, cid, tid, msg, QMI_ERR_NONE, &q);
}

static void dhcp_start(RM520NState *s);

/* WDS: one call (its packet data handle 1), on the addresses passt gave;
 * IPv4 */
static void qmi_wds(RM520NState *s, uint8_t cid, uint16_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    QBuf q = { .n = 0 };
    const uint8_t *v;
    uint32_t mask = 0x3ff;
    uint8_t b[16];
    int len;

    switch (msg) {
    case 0x0001:                                /* Set Event Report */
    case 0x0003:                                /* Indication Register */
        break;
    case 0x004d:                                /* Set IP Family */
        v = tlv_find(t, n, 0x01, &len);
        if (!v || len < 1) {
            qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_INVALID_ARG, NULL);
            return;
        }
        s->family[cid] = v[0];
        break;
    case 0x0020:                                /* Start Network */
        v = tlv_find(t, n, 0x19, &len);         /* (its IP family) */
        if ((v && len >= 1 && v[0] == 6) ||
            (!(v && len >= 1) && s->family[cid] == 6)) {
            /* IPv4 only: an IPv6 call is refused, as by a network that
             * has none (ESM cause 50: "PDN type IPv4 only allowed") */
            tlv_u16(&q, 0x10, 1);
            stw_le_p(b, 6);                     /* (3GPP's) */
            stw_le_p(b + 2, 50);
            tlv(&q, 0x11, b, 4);
            qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_CALL_FAILED, &q);
            return;
        }
        if (s->opmode != 0 || !s->dhcp_done) {  /* (no service, no uplink) */
            if (!s->dhcp_done) {
                dhcp_start(s);                  /* (asked again) */
            }
            qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_CALL_FAILED, NULL);
            return;
        }
        s->connected = true;
        s->call_cid = cid;
        s->tx_pkts = s->rx_pkts = s->rx_dropped = 0;
        s->tx_bytes = s->rx_bytes = 0;
        tlv_u32(&q, 0x01, 1);
        break;
    case 0x0021:                                /* Stop Network */
        s->connected = false;
        break;
    case 0x0022:                                /* Get Packet Service Status */
        tlv_u8(&q, 0x01, s->connected ? 2 : 1);
        break;
    case 0x0023:                                /* Get Channel Rates */
    case 0x0024:                                /* Get Packet Statistics */
    case 0x002d:                                /* Get Current Settings */
        if (!s->connected) {
            qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_OUT_OF_CALL, NULL);
            return;
        }
        if (msg == 0x0023) {
            stl_le_p(b, RATE_TX);
            stl_le_p(b + 4, RATE_RX);
            stl_le_p(b + 8, RATE_TX_MAX);
            stl_le_p(b + 12, RATE_RX_MAX);
            tlv(&q, 0x01, b, 16);
        } else if (msg == 0x0024) {             /* (what the mask asks) */
            v = tlv_find(t, n, 0x01, &len);
            if (v && len >= 4) {
                mask = ldl_le_p(v);
            }
            for (int i = 0; i < 10; i++) {
                static const uint8_t type[10] = {
                    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x19, 0x1a, 0x1d, 0x1e,
                };
                uint64_t val[10] = {
                    s->tx_pkts, s->rx_pkts, 0, 0, 0, 0,
                    s->tx_bytes, s->rx_bytes, 0, s->rx_dropped,
                };

                if (!(mask & (1u << i))) {
                    continue;
                }
                if (i == 6 || i == 7) {
                    stq_le_p(b, val[i]);
                    tlv(&q, type[i], b, 8);
                } else {
                    tlv_u32(&q, type[i], val[i]);
                }
            }
        } else {
            tlv_u32(&q, 0x1e, be32_to_cpu(s->ip));
            tlv_u32(&q, 0x20, be32_to_cpu(s->gw));
            tlv_u32(&q, 0x21, be32_to_cpu(s->mask));
            tlv_u32(&q, 0x15, be32_to_cpu(s->dns[0]));
            if (s->dns[1]) {
                tlv_u32(&q, 0x16, be32_to_cpu(s->dns[1]));
            }
            tlv_u32(&q, 0x29, CALL_MTU);
            tlv_u8(&q, 0x2b, 4);
        }
        break;
    default:
        qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        return;
    }
    qmi_answer(s, SVC_WDS, cid, tid, msg, QMI_ERR_NONE, &q);
}

/* ------------------------------------------------- its data, and passt */

/* an Ethernet frame for passt: @type's payload, from its own MAC */
static void eth_out(RM520NState *s, const uint8_t *dst, uint16_t type,
                    const uint8_t *pl, int n)
{
    uint8_t f[1600];

    if (!s->nic || n > (int)sizeof(f) - 14) {
        return;
    }
    memcpy(f, dst, 6);
    memcpy(f + 6, s->conf.macaddr.a, 6);
    stw_be_p(f + 12, type);
    memcpy(f + 14, pl, n);
    qemu_send_packet(qemu_get_queue(s->nic), f, 14 + n);
}

static uint16_t ip_csum(const uint8_t *h, int n)
{
    uint32_t sum = 0;

    for (int i = 0; i < n; i += 2) {
        sum += lduw_be_p(h + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

/* DHCP to passt, for its addresses: DISCOVER, then REQUEST of what it
 * offered */
static void dhcp_send(RM520NState *s, uint8_t type, uint32_t want,
                      uint32_t server)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t b[20 + 8 + 300] = { 0 }, *u = b + 20, *d = b + 28;
    int k = 240;

    d[0] = 1;                                   /* BOOTREQUEST, Ethernet */
    d[1] = 1;
    d[2] = 6;
    stl_be_p(d + 4, s->dhcp_xid);
    stw_be_p(d + 10, 0x8000);                   /* (answer by broadcast) */
    memcpy(d + 28, s->conf.macaddr.a, 6);
    stl_be_p(d + 236, 0x63825363);              /* (the magic cookie) */
    d[k++] = 53;
    d[k++] = 1;
    d[k++] = type;
    if (type == 3) {
        d[k++] = 50;                            /* the address offered */
        d[k++] = 4;
        memcpy(d + k, &want, 4);
        k += 4;
        d[k++] = 54;                            /* by whom */
        d[k++] = 4;
        memcpy(d + k, &server, 4);
        k += 4;
    }
    d[k++] = 55;                                /* mask, router, DNS */
    d[k++] = 3;
    d[k++] = 1;
    d[k++] = 3;
    d[k++] = 6;
    d[k++] = 255;
    k = 300;
    stw_be_p(u, 68);
    stw_be_p(u + 2, 67);
    stw_be_p(u + 4, 8 + k);
    b[0] = 0x45;                                /* IPv4: 0.0.0.0 to all */
    stw_be_p(b + 2, 20 + 8 + k);
    b[8] = 64;
    b[9] = 17;
    memset(b + 16, 0xff, 4);
    stw_be_p(b + 10, ip_csum(b, 20));
    eth_out(s, bcast, ETH_P_IP, b, 20 + 8 + k);
}

static void dhcp_start(RM520NState *s)
{
    s->dhcp_done = false;
    s->dhcp_xid = g_random_int();
    if (s->nic && qemu_get_queue(s->nic)->peer) {
        dhcp_send(s, 1, 0, 0);
    }
}

/* passt's answer: an OFFER (then the REQUEST), or the ACK, its addresses */
static void dhcp_rx(RM520NState *s, const uint8_t *eth, const uint8_t *d,
                    int n)
{
    uint8_t type = 0;
    uint32_t server = 0, mask = 0, gw = 0, dns[2] = { 0, 0 };

    if (n < 240 || d[0] != 2 || ldl_be_p(d + 4) != s->dhcp_xid ||
        ldl_be_p(d + 236) != 0x63825363) {
        return;
    }
    for (int k = 240; k + 1 < n && d[k] != 255; ) {
        uint8_t o = d[k], l;

        if (o == 0) {
            k++;
            continue;
        }
        l = d[k + 1];
        if (k + 2 + l > n) {
            break;
        }
        if (o == 53 && l >= 1) {
            type = d[k + 2];
        } else if (o == 54 && l >= 4) {
            memcpy(&server, d + k + 2, 4);
        } else if (o == 1 && l >= 4) {
            memcpy(&mask, d + k + 2, 4);
        } else if (o == 3 && l >= 4) {
            memcpy(&gw, d + k + 2, 4);
        } else if (o == 6 && l >= 4) {
            memcpy(&dns[0], d + k + 2, 4);
            if (l >= 8) {
                memcpy(&dns[1], d + k + 6, 4);
            }
        }
        k += 2 + l;
    }
    if (type == 2) {                            /* OFFER */
        uint32_t yi;

        memcpy(&yi, d + 16, 4);
        dhcp_send(s, 3, yi, server);
    } else if (type == 5) {                     /* ACK */
        memcpy(&s->ip, d + 16, 4);
        s->gw = gw ? gw : server;
        s->mask = mask ? mask : cpu_to_be32(0xffffff00);
        s->dns[0] = dns[0] ? dns[0] : s->gw;
        s->dns[1] = dns[1];
        memcpy(s->peer, eth + 6, 6);
        s->dhcp_done = true;
    }
}

static bool rm520n_can_receive(NetClientState *nc)
{
    return true;
}

/* passt's frame: DHCP's answer, ARP for its address, or the call's IP */
static ssize_t rm520n_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    RM520NState *s = qemu_get_nic_opaque(nc);
    uint16_t type;

    if (size < 14) {
        return size;
    }
    type = lduw_be_p(buf + 12);
    if (type == ETH_P_ARP && size >= 42 && lduw_be_p(buf + 20) == 1 &&
        s->dhcp_done && !memcmp(buf + 38, &s->ip, 4)) {
        uint8_t a[28];                          /* (it is here) */

        memcpy(a, buf + 14, 8);
        stw_be_p(a + 6, 2);
        memcpy(a + 8, s->conf.macaddr.a, 6);
        memcpy(a + 14, &s->ip, 4);
        memcpy(a + 18, buf + 22, 10);
        eth_out(s, buf + 6, ETH_P_ARP, a, 28);
        return size;
    }
    if (type != ETH_P_IP && type != ETH_P_IPV6) {
        return size;
    }
    if (type == ETH_P_IP && size >= 14 + 20) {  /* DHCP's, to the modem */
        int ihl = (buf[14] & 0x0f) * 4;

        if (ihl >= 20 && size >= 14 + ihl + 8 + 240 && buf[14 + 9] == 17 &&
            lduw_be_p(buf + 14 + ihl + 2) == 68) {
            dhcp_rx(s, buf, buf + 14 + ihl + 8, size - 14 - ihl - 8);
            return size;
        }
    }
    if (!s->connected) {
        return size;                            /* (no call: dropped) */
    }
    if (size - 14 > CALL_MTU ||                 /* (passt's MTU is more) */
        g_queue_get_length(&s->down) >= DOWN_MAX) {
        s->rx_dropped++;
        return size;
    }
    g_queue_push_tail(&s->down, g_byte_array_append(g_byte_array_new(),
                                                    buf + 14, size - 14));
    usb_wakeup(s->bulk_in, 0);
    return size;
}

static NetClientInfo rm520n_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = rm520n_can_receive,
    .receive = rm520n_receive,
};

/* WDA: the data format, raw IP or 802.3, no aggregation */
static void qmi_wda(RM520NState *s, uint8_t cid, uint16_t tid, uint16_t msg,
                    const uint8_t *t, int n)
{
    QBuf q = { .n = 0 };
    const uint8_t *v;
    int len;

    switch (msg) {
    case 0x0020:                                /* Set Data Format */
        v = tlv_find(t, n, 0x11, &len);
        if (v && len >= 4 && (ldl_le_p(v) == 1 || ldl_le_p(v) == 2)) {
            s->link_layer = ldl_le_p(v);
        }
        /* fall through: the format as it now is */
    case 0x0021:                                /* Get Data Format */
        tlv_u8(&q, 0x10, 0);                    /* (no QoS header) */
        tlv_u32(&q, 0x11, s->link_layer);
        tlv_u32(&q, 0x12, 0);                   /* (no aggregation) */
        tlv_u32(&q, 0x13, 0);
        break;
    default:
        qmi_answer(s, SVC_WDA, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        return;
    }
    qmi_answer(s, SVC_WDA, cid, tid, msg, QMI_ERR_NONE, &q);
}

/* a QMUX frame from the host */
static void qmi_rx(RM520NState *s, const uint8_t *b, int n)
{
    uint8_t svc, cid;
    uint16_t tid, msg;
    int len, tn;

    if (n < 6 || b[0] != 0x01) {
        return;
    }
    len = MIN(lduw_le_p(b + 1) + 1, n);
    svc = b[4];
    cid = b[5];
    if (svc == SVC_CTL) {
        if (len < 12) {
            return;
        }
        tid = b[7];
        msg = lduw_le_p(b + 8);
        tn = MIN(lduw_le_p(b + 10), len - 12);
        qmi_ctl(s, tid, msg, b + 12, tn);
        return;
    }
    if (len < 13) {
        return;
    }
    tid = lduw_le_p(b + 7);
    msg = lduw_le_p(b + 9);
    tn = MIN(lduw_le_p(b + 11), len - 13);
    switch (svc) {
    case SVC_DMS:
        qmi_dms(s, cid, tid, msg, b + 13, tn);
        break;
    case SVC_WDA:
        qmi_wda(s, cid, tid, msg, b + 13, tn);
        break;
    case SVC_UIM:
        qmi_uim(s, cid, tid, msg, b + 13, tn);
        break;
    case SVC_NAS:
        qmi_nas(s, cid, tid, msg, b + 13, tn);
        break;
    case SVC_WDS:
        qmi_wds(s, cid, tid, msg, b + 13, tn);
        break;
    default:
        qmi_answer(s, svc, cid, tid, msg, QMI_ERR_INVALID_CMD, NULL);
        break;
    }
}

static void rm520n_handle_control(USBDevice *dev, USBPacket *p, int request,
                                  int value, int index, int length,
                                  uint8_t *data)
{
    RM520NState *s = RM520N(dev);
    GByteArray *f;

    if (usb_desc_handle_control(dev, p, request, value, index, length,
                                data) >= 0) {
        return;
    }
    switch (request) {
    case ClassInterfaceOutRequest | 0x00:       /* SEND_ENCAPSULATED_COMMAND */
        qmi_rx(s, data, length);
        break;
    case ClassInterfaceRequest | 0x01:          /* GET_ENCAPSULATED_RESPONSE */
        f = g_queue_pop_head(&s->resp);
        if (f) {
            p->actual_length = MIN((int)f->len, length);
            memcpy(data, f->data, p->actual_length);
            g_byte_array_unref(f);
        }
        break;
    case ClassInterfaceOutRequest | 0x22:       /* SET_CONTROL_LINE_STATE */
        break;
    default:
        p->status = USB_RET_STALL;
        break;
    }
}

static void rm520n_handle_data(USBDevice *dev, USBPacket *p)
{
    RM520NState *s = RM520N(dev);
    uint8_t note[8] = { 0xa1, 0x01, 0, 0, QMI_IFACE, 0, 0, 0 };

    if (p->pid == USB_TOKEN_IN && !p->iov.size) {
        return;                                 /* (no room: done, empty) */
    }
    if (p->pid == USB_TOKEN_IN && p->ep->nr == 1) {     /* notifications */
        if (!s->notify) {
            p->status = USB_RET_NAK;
            return;
        }
        s->notify--;
        usb_packet_copy(p, note, MIN(sizeof(note), p->iov.size));
        return;
    }
    if (p->pid == USB_TOKEN_IN) {               /* an IP packet for the host */
        GByteArray *f = g_queue_pop_head(&s->down);

        if (!f) {
            p->status = USB_RET_NAK;
            return;
        }
        usb_packet_copy(p, f->data, MIN(f->len, p->iov.size));
        s->rx_pkts++;
        s->rx_bytes += f->len;
        g_byte_array_unref(f);
        return;
    }
    /* the host's: an IP packet (raw IP; or 802.3's frame, its IP taken:
     * nothing answers ARP or DHCP there) */
    {
        uint8_t b[1600];
        int n = MIN(p->iov.size, sizeof(b)), off = 0;
        uint16_t type;

        usb_packet_copy(p, b, n);
        if (s->link_layer == 1) {
            off = 14;
        }
        if (!s->connected || n <= off || !s->dhcp_done) {
            return;
        }
        type = (b[off] >> 4) == 6 ? ETH_P_IPV6 : ETH_P_IP;
        if (off && lduw_be_p(b + 12) != type) {
            return;
        }
        s->tx_pkts++;
        s->tx_bytes += n - off;
        eth_out(s, s->peer, type, b + off, n - off);
    }
}

static void rm520n_flush(RM520NState *s)
{
    GByteArray *f;

    while ((f = g_queue_pop_head(&s->resp))) {
        g_byte_array_unref(f);
    }
    while ((f = g_queue_pop_head(&s->down))) {
        g_byte_array_unref(f);
    }
    s->notify = 0;
}

static void rm520n_handle_reset(USBDevice *dev)
{
    RM520NState *s = RM520N(dev);

    rm520n_flush(s);
    memset(s->next_cid, 0, sizeof(s->next_cid));
    s->opmode = 0;                              /* online */
    s->link_layer = 1;                          /* 802.3, until told */
    s->connected = false;
}

/* the M.2 slot's power and reset: on USB while both are released */
void rm520n_power(USBDevice *dev, bool on)
{
    Error *err = NULL;

    if (on == dev->attached) {
        return;
    }
    if (!on) {
        usb_device_detach(dev);
        rm520n_handle_reset(dev);
        return;
    }
    rm520n_handle_reset(dev);
    dhcp_start(RM520N(dev));                    /* (its addresses) */
    usb_device_attach(dev, &err);
    if (err) {
        error_report_err(err);
    }
}

static void rm520n_realize(USBDevice *dev, Error **errp)
{
    RM520NState *s = RM520N(dev);

    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 2);
    g_queue_init(&s->resp);
    g_queue_init(&s->down);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&rm520n_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)),
                          DEVICE(dev)->id, &DEVICE(dev)->mem_reentrancy_guard,
                          s);
    rm520n_handle_reset(dev);
    dev->auto_attach = false;                   /* (powered by the board) */
}

static void rm520n_unrealize(USBDevice *dev)
{
    RM520NState *s = RM520N(dev);

    rm520n_flush(s);
    if (s->nic) {
        qemu_del_nic(s->nic);
    }
}

static const Property rm520n_props[] = {
    DEFINE_NIC_PROPERTIES(RM520NState, conf),
};

static void rm520n_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    USBDeviceClass *uc = USB_DEVICE_CLASS(oc);

    uc->realize = rm520n_realize;
    uc->unrealize = rm520n_unrealize;
    uc->product_desc = "Quectel RM520N-GL";
    uc->usb_desc = &rm520n_desc;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = rm520n_handle_reset;
    uc->handle_control = rm520n_handle_control;
    uc->handle_data = rm520n_handle_data;
    device_class_set_props(dc, rm520n_props);
    dc->desc = "Quectel RM520N-GL 5G modem (QMI)";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo rm520n_types[] = {
    {
        .name = TYPE_RM520N,
        .parent = TYPE_USB_DEVICE,
        .instance_size = sizeof(RM520NState),
        .class_init = rm520n_class_init,
    },
};

DEFINE_TYPES(rm520n_types)
