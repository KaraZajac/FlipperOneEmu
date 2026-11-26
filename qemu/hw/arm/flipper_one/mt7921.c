/*
 * The MediaTek MT7921 (MT7961) on the Flipper One's USB1, Wi-Fi and
 * Bluetooth in one chip, as Linux's btusb and btmtk (its Bluetooth) and
 * mt7921u (its Wi-Fi) drive it.
 *
 * The chip is on USB while its PMU_EN is high (GPIO1_D5, the device tree's
 * rfkill-gpio "Onboard WiFi+BT", blocked by default: the board,
 * mt7921_power()), and each power-up is a cold chip: its patch firmware
 * gone. As MediaTek's USB devices have it:
 * - its descriptors: 0e8d:7961, an interface association of three
 *   Bluetooth interfaces (e0/01/01): 0, HCI (events on interrupt 0x81, ACL
 *   on bulk 0x82 and 0x02); 1, SCO (isochronous 0x83 and 0x03, the usual
 *   seven alternate settings); 2, ISO over interrupt (0x8a, 0x0a, in its
 *   alternate setting 1);
 * - its registers, read with vendor request 0x63 (the chip's ID at
 *   0x70010200, its firmware's version and flavour), and the "uhw" ones
 *   (requests 1 and 2, types 0xde and 0x5e: the endpoint reset option, the
 *   subsystem's reset done);
 * - WMT, MediaTek's commands to the chip's own firmware: an HCI command
 *   (0xfc6f) that no Command Complete answers, its event (0xe4) fetched by
 *   the host with vendor request 1 (value 0x30) until one is there. The
 *   patch firmware comes in sections (a query each, which "not there yet"
 *   answers, then its data, 250 bytes a command); "function on" enables
 *   Bluetooth. The patch stays while the chip is powered: the next open's
 *   queries find it there.
 * - then a Bluetooth 5.2 controller (HCI and LMP 0x0b, MediaTek's company
 *   ID 70), BR/EDR and LE, with what it claims in its features and its
 *   supported commands answered: the HCI core's initialisation, BlueZ's
 *   settings, inquiry (complete after its length, no device found), LE
 *   scanning and advertising (no reports), connections (pages time out).
 *   No radio: nothing is ever found, nothing connects. The values not
 *   fixed by the protocol (its subversion, the TX powers, the lists'
 *   sizes) are a typical controller's, not read from a device.
 *
 * Its Wi-Fi is interface 3 (ff/ff/ff): bulk IN 0x84 (received frames and,
 * once WFDMA's HOST_CONFIG bit 6 says so, the MCU's events) and 0x85, bulk
 * OUT 0x08 (MCU commands), 0x04 (the firmware's data while FW_DL_EN, then
 * best-effort frames), 0x05-0x07, 0x09 (the other queues), in that order:
 * mt76 takes their roles from it. Registers through vendor requests 0x63
 * and 0x66 (type 0xdf, 0x5f) and the "uhw" ones (one space with the
 * Bluetooth's): the chip's ID and revision, CONN_ON_MISC's state (power
 * on, the ROM's patch, the RAM firmware running), the WFSYS reset (a pulse
 * on WF_SUBSYS_RST: the MCU back in its ROM), the busy bits never set. Its
 * MCU: commands as mt76 builds them (a 4-byte length, the TXD, the MCU's
 * header: legacy, CE, EXT or UNI), events back as the firmware sends them
 * (RXD0 type 7 and the exact length, the command's seq echoed). The
 * firmware is the guest's own, its patch and RAM code taken as they come
 * and dropped; then the NIC's capabilities (its MAC address, two streams,
 * 2.4 and 5 GHz), and what bringing the interface up and scanning send. A
 * scan ends after a while with no network found: no radio. Commands it
 * does not know get an answer as if done, and are logged.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "flipper_one.h"

OBJECT_DECLARE_SIMPLE_TYPE(MT7921State, MT7921)

#define EVQ_MAX         8192            /* HCI events waiting, in bytes */
#define INTR_MPS        16              /* the event endpoint's packets */
#define WMT_MAX         64              /* a WMT event at most */
#define HCI_EV_WMT      0xe4

/* WMT's operations (btmtk.h) */
enum {
    WMT_PATCH_DWNLD = 0x1, WMT_TEST = 0x2, WMT_WAKEUP = 0x3, WMT_HIF = 0x4,
    WMT_FUNC_CTRL = 0x6, WMT_RST = 0x7, WMT_REGISTER = 0x8,
    WMT_SEMAPHORE = 0x17,
};

struct MT7921State {
    USBDevice parent_obj;
    char *bdaddr_prop;                  /* "bdaddr": xx:xx:xx:xx:xx:xx
                                           (MediaTek's OUI, "FO", 1 unless
                                           told) */
    uint8_t bdaddr[6];                  /* (HCI's order: least first) */
    USBEndpoint *intr;
    GByteArray *evq;                    /* HCI events, back to back */
    uint32_t ev_left;                   /* ... of the first, not yet sent */
    uint8_t wmt[WMT_MAX];               /* the WMT event to be fetched */
    int wmt_len;
    /* the chip: its patch firmware, Bluetooth on */
    bool patched, bt_on;
    uint32_t dl_sections;               /* sections whose data came */
    /* the controller */
    uint8_t name[248];
    uint8_t cod[3];
    uint8_t scan_enable, inquiry_mode, page_scan_type, inquiry_scan_type;
    uint8_t ssp_mode, le_host, sc_host, auth_enable, err_reporting;
    uint16_t voice_setting, page_timeout;
    uint8_t eir_fec, eir[240];
    QEMUTimer *inquiry;                 /* its end */
    QEMUTimer *page;                    /* a page's or a name request's */
    uint8_t page_op;                    /* ... which (0: none) */
    uint8_t page_addr[6];
    bool le_connecting;
    /* the chip's registers: std and uhw, the Bluetooth's and Wi-Fi's */
    GHashTable *regs;                   /* address → value (else 0) */
    /* its Wi-Fi */
    char *wifi_mac_prop;                /* "wifi-mac" (MediaTek's OUI, "FO",
                                           2 unless told) */
    uint8_t wifi_mac[6];
    uint32_t conn_on_misc;              /* bits 2:0: the MCU's state */
    bool wifi_patched;                  /* the ROM's patch is in */
    GQueue wevq;                        /* MCU events, a transfer each */
    QEMUTimer *scan;                    /* a scan's end */
};

/* ------------------------------------------------------------ USB side */

enum { STR_MANUFACTURER = 1, STR_PRODUCT, STR_SERIAL };

static const USBDescStrings mt7921_strings = {
    [STR_MANUFACTURER]  = "MediaTek Inc.",
    [STR_PRODUCT]       = "Wireless_Device",
    [STR_SERIAL]        = "000000000",
};

#define BT_IFACE(n, alt, neps, epsv) {                          \
        .bInterfaceNumber = (n), .bAlternateSetting = (alt),    \
        .bNumEndpoints = (neps), .bInterfaceClass = 0xe0,       \
        .bInterfaceSubClass = 0x01, .bInterfaceProtocol = 0x01, \
        .eps = (epsv) }

/* the endpoints at full speed (fs) and high speed (hs): bulk packets and
 * the interrupt endpoints' interval differ */
#define HCI_EPS(bulk, ival) (USBDescEndpoint[]) {                       \
        { .bEndpointAddress = USB_DIR_IN | 1,                           \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = INTR_MPS, .bInterval = (ival) },            \
        { .bEndpointAddress = USB_DIR_IN | 2,                           \
          .bmAttributes = USB_ENDPOINT_XFER_BULK,                       \
          .wMaxPacketSize = (bulk) },                                   \
        { .bEndpointAddress = USB_DIR_OUT | 2,                          \
          .bmAttributes = USB_ENDPOINT_XFER_BULK,                       \
          .wMaxPacketSize = (bulk) } }
#define SCO_EPS(mps, ival) (USBDescEndpoint[]) {                        \
        { .bEndpointAddress = USB_DIR_IN | 3,                           \
          .bmAttributes = USB_ENDPOINT_XFER_ISOC,                       \
          .wMaxPacketSize = (mps), .bInterval = (ival) },               \
        { .bEndpointAddress = USB_DIR_OUT | 3,                          \
          .bmAttributes = USB_ENDPOINT_XFER_ISOC,                       \
          .wMaxPacketSize = (mps), .bInterval = (ival) } }
#define ISO_EPS(ival) (USBDescEndpoint[]) {                             \
        { .bEndpointAddress = USB_DIR_IN | 10,                          \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = 64, .bInterval = (ival) },                  \
        { .bEndpointAddress = USB_DIR_OUT | 10,                         \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = 64, .bInterval = (ival) } }

#define BT_IFACES(bulk, ival) {                                         \
        BT_IFACE(0, 0, 3, HCI_EPS(bulk, ival)),                         \
        BT_IFACE(1, 0, 2, SCO_EPS(0, ival)),                            \
        BT_IFACE(1, 1, 2, SCO_EPS(9, ival)),                            \
        BT_IFACE(1, 2, 2, SCO_EPS(17, ival)),                           \
        BT_IFACE(1, 3, 2, SCO_EPS(25, ival)),                           \
        BT_IFACE(1, 4, 2, SCO_EPS(33, ival)),                           \
        BT_IFACE(1, 5, 2, SCO_EPS(49, ival)),                           \
        BT_IFACE(1, 6, 2, SCO_EPS(63, ival)),                           \
        BT_IFACE(2, 0, 0, NULL),                                        \
        BT_IFACE(2, 1, 2, ISO_EPS(ival)),                               \
    }
#define N_BT_IFACES     10

/* the Wi-Fi's interface: its endpoints in the order mt76 takes their roles
 * from (IN: received frames and events, then the MCU's queue; OUT: MCU
 * commands, then best effort, background, video, voice, management) */
#define WIFI_EP(addr, mps) { .bEndpointAddress = (addr),                  \
        .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = (mps) }
#define WIFI_IFACE(mps) {                                               \
        .bInterfaceNumber = 3, .bNumEndpoints = 8,                      \
        .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff,            \
        .bInterfaceProtocol = 0xff,                                     \
        .eps = (USBDescEndpoint[]) {                                    \
            WIFI_EP(USB_DIR_IN | 4, mps), WIFI_EP(USB_DIR_IN | 5, mps), \
            WIFI_EP(USB_DIR_OUT | 8, mps), WIFI_EP(USB_DIR_OUT | 4, mps), \
            WIFI_EP(USB_DIR_OUT | 5, mps), WIFI_EP(USB_DIR_OUT | 6, mps), \
            WIFI_EP(USB_DIR_OUT | 7, mps), WIFI_EP(USB_DIR_OUT | 9, mps), \
        } }

static const USBDescIface wifi_iface_fs = WIFI_IFACE(64);
static const USBDescIface wifi_iface_hs = WIFI_IFACE(512);

static const USBDescIface bt_ifaces_fs[N_BT_IFACES] = BT_IFACES(64, 1);
static const USBDescIface bt_ifaces_hs[N_BT_IFACES] = BT_IFACES(512, 4);

static const USBDescIfaceAssoc bt_group_fs = {
    .bFirstInterface = 0, .bInterfaceCount = 3,
    .bFunctionClass = 0xe0, .bFunctionSubClass = 0x01,
    .bFunctionProtocol = 0x01,
    .nif = N_BT_IFACES, .ifs = bt_ifaces_fs,
};
static const USBDescIfaceAssoc bt_group_hs = {
    .bFirstInterface = 0, .bInterfaceCount = 3,
    .bFunctionClass = 0xe0, .bFunctionSubClass = 0x01,
    .bFunctionProtocol = 0x01,
    .nif = N_BT_IFACES, .ifs = bt_ifaces_hs,
};

static const USBDescDevice mt7921_dev_fs = {
    .bcdUSB = 0x0210, .bDeviceClass = 0xef, .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) { {
        .bNumInterfaces = 4, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |
                        USB_CFG_ATT_WAKEUP,
        .bMaxPower = 50,
        .nif_groups = 1, .if_groups = &bt_group_fs,
        .nif = 1, .ifs = &wifi_iface_fs,
    } },
};
static const USBDescDevice mt7921_dev_hs = {
    .bcdUSB = 0x0210, .bDeviceClass = 0xef, .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) { {
        .bNumInterfaces = 4, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |
                        USB_CFG_ATT_WAKEUP,
        .bMaxPower = 50,
        .nif_groups = 1, .if_groups = &bt_group_hs,
        .nif = 1, .ifs = &wifi_iface_hs,
    } },
};

static const USBDesc mt7921_desc = {
    .id = {
        .idVendor = 0x0e8d, .idProduct = 0x7961, .bcdDevice = 0x0100,
        .iManufacturer = STR_MANUFACTURER, .iProduct = STR_PRODUCT,
        .iSerialNumber = STR_SERIAL,
    },
    .full = &mt7921_dev_fs,
    .high = &mt7921_dev_hs,
    .str = mt7921_strings,
};

/* ------------------------------------------------------- HCI's events */

static void ev_put(MT7921State *s, uint8_t code, const uint8_t *p, int n)
{
    uint8_t h[2] = { code, n };

    if (s->evq->len + 2 + n > EVQ_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "mt7921: events not taken: 0x%02x "
                      "dropped\n", code);
        return;
    }
    g_byte_array_append(s->evq, h, 2);
    g_byte_array_append(s->evq, p, n);
    usb_wakeup(s->intr, 0);
}

/* Command Complete: @p the return parameters, status first */
static void ev_cc(MT7921State *s, uint16_t op, const uint8_t *p, int n)
{
    uint8_t e[255];

    e[0] = 1;                                   /* Num_HCI_Command_Packets */
    stw_le_p(e + 1, op);
    memcpy(e + 3, p, n);
    ev_put(s, 0x0e, e, 3 + n);
}

static void ev_cc_status(MT7921State *s, uint16_t op, uint8_t status)
{
    ev_cc(s, op, &status, 1);
}

static void ev_cs(MT7921State *s, uint16_t op, uint8_t status)
{
    uint8_t e[4] = { status, 1, op, op >> 8 };

    ev_put(s, 0x0f, e, 4);
}

/* ---------------------------------------------------- the controller */

/* LMP features, page 0: a dual-mode controller's (3- and 5-slot, EDR, AFH,
 * sniff subrating, EIR, SSP, LE and BR/EDR at once, ...; extended
 * features) */
static const uint8_t lmp_features[8] = {
    0xbf, 0xfe, 0x0f, 0xfe, 0xdb, 0xff, 0x7b, 0x87,
};
/* LE features: encryption, the connection parameters request, extended
 * reject, peripheral-initiated features exchange, ping, data length
 * extension, LL privacy, extended scanner filter policies; the 2M PHY,
 * channel selection #2. (No extended advertising: legacy advertising
 * and scanning) */
static const uint8_t le_features[8] = { 0xff, 0x41, 0, 0, 0, 0, 0, 0 };
/* LE supported states: all of them */
static const uint8_t le_states[8] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0, 0,
};

/* the supported commands this controller answers: octet, bit (the core
 * specification's table, Vol 4 Part E 6.27) */
static const uint8_t supported_cmds[][2] = {
    { 0, 0 }, { 0, 1 }, { 0, 4 }, { 0, 5 }, { 0, 7 },   /* inquiry, conn */
    { 2, 3 }, { 2, 4 }, { 2, 7 },               /* name request, version */
    { 5, 3 }, { 5, 4 }, { 5, 6 }, { 5, 7 },     /* link policy, mask, reset */
    { 6, 0 }, { 6, 5 }, { 6, 7 },               /* event filter, link keys */
    { 7, 0 }, { 7, 1 }, { 7, 3 }, { 7, 4 }, { 7, 5 }, { 7, 6 }, { 7, 7 },
    { 8, 0 }, { 8, 1 }, { 8, 2 }, { 8, 3 }, { 8, 4 }, { 8, 5 },
    { 9, 0 }, { 9, 1 }, { 9, 2 }, { 9, 3 },     /* class, voice */
    { 10, 4 }, { 10, 5 }, { 10, 6 },            /* flow control */
    { 11, 2 }, { 11, 3 }, { 11, 4 },            /* IAC */
    { 12, 4 }, { 12, 5 }, { 12, 6 }, { 12, 7 }, /* inquiry scan, mode */
    { 13, 0 }, { 13, 1 },                       /* page scan type */
    { 14, 3 }, { 14, 5 }, { 14, 6 }, { 14, 7 }, /* version, features, ... */
    { 15, 1 },                                  /* BD_ADDR */
    { 17, 0 }, { 17, 1 }, { 17, 5 }, { 17, 6 }, /* EIR, SSP */
    { 18, 0 }, { 18, 2 }, { 18, 3 },            /* inquiry TX, err data */
    { 22, 2 },                                  /* event mask page 2 */
    { 24, 5 }, { 24, 6 },                       /* LE host support */
    { 25, 0 }, { 25, 1 }, { 25, 2 }, { 25, 4 }, { 25, 5 }, { 25, 6 },
    { 25, 7 },
    { 26, 0 }, { 26, 1 }, { 26, 2 }, { 26, 3 }, { 26, 4 }, { 26, 5 },
    { 26, 6 }, { 26, 7 },
    { 27, 0 }, { 27, 1 }, { 27, 2 }, { 27, 5 }, { 27, 7 },
    { 28, 3 },                                  /* LE supported states */
    { 29, 5 },                                  /* local codecs (v1) */
    { 33, 7 }, { 34, 0 },                       /* LE data length */
    { 34, 3 }, { 34, 4 }, { 34, 5 }, { 34, 6 }, /* LE resolving list */
    { 35, 1 }, { 35, 2 }, { 35, 3 }, { 35, 5 }, /* ... RPA, PHY */
    { 38, 7 },                                  /* LE read TX power */
};

static void controller_reset(MT7921State *s)
{
    timer_del(s->inquiry);
    timer_del(s->page);
    s->page_op = 0;
    s->le_connecting = false;
    s->scan_enable = 0;
    s->inquiry_mode = 0;
    s->page_scan_type = 0;
    s->inquiry_scan_type = 0;
    s->ssp_mode = 0;
    s->le_host = 0;
    s->sc_host = 0;
    s->auth_enable = 0;
    s->err_reporting = 0;
    s->voice_setting = 0x0060;
    s->page_timeout = 0x2000;                   /* 5.12 s */
    s->eir_fec = 0;
    memset(s->eir, 0, sizeof(s->eir));
    memset(s->cod, 0, sizeof(s->cod));
    memset(s->name, 0, sizeof(s->name));
}

static void inquiry_done(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t status = 0;

    ev_put(s, 0x01, &status, 1);                /* Inquiry Complete */
}

/* a page (Create Connection) or a name request: its timeout, as with no
 * device there */
static void page_done(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t e[255] = { 0x04 };                  /* Page Timeout */

    if (s->page_op == 0x05) {                   /* Connection Complete */
        memcpy(e + 3, s->page_addr, 6);
        e[9] = 0x01;                            /* ACL */
        ev_put(s, 0x03, e, 11);
    } else if (s->page_op == 0x19) {            /* Remote Name Request
                                                   Complete */
        memcpy(e + 1, s->page_addr, 6);
        ev_put(s, 0x07, e, 255);
    }
    s->page_op = 0;
}

static void page_start(MT7921State *s, uint8_t op, const uint8_t *addr)
{
    s->page_op = op;
    memcpy(s->page_addr, addr, 6);
    timer_mod(s->page, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (int64_t)s->page_timeout * 625000);       /* 0.625 ms each */
}

/* an HCI command: what it asks, answered */
static void hci_command(MT7921State *s, const uint8_t *pkt, int len)
{
    uint16_t op;
    const uint8_t *p;
    uint8_t r[255] = { 0 };                     /* status 0, then ... */
    int plen;

    if (len < 3) {
        return;
    }
    op = lduw_le_p(pkt);
    plen = MIN(pkt[2], len - 3);
    p = pkt + 3;

    switch (op) {
    /* ---- link control */
    case 0x0401:                                /* Inquiry */
        if (plen < 5) {
            ev_cs(s, op, 0x12);
            break;
        }
        ev_cs(s, op, 0);
        timer_mod(s->inquiry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)MAX(p[3], 1) * 1280 * SCALE_MS);
        break;
    case 0x0402:                                /* Inquiry Cancel */
        timer_del(s->inquiry);
        ev_cc_status(s, op, 0);
        break;
    case 0x0405:                                /* Create Connection */
    case 0x0419:                                /* Remote Name Request */
        if (plen < 6 || s->page_op) {
            ev_cs(s, op, s->page_op ? 0x0c : 0x12);
            break;
        }
        ev_cs(s, op, 0);
        page_start(s, op & 0xff, p);
        break;
    case 0x0408:                                /* Create Connection Cancel */
    case 0x041a:                                /* Remote Name Request Cancel */
        if (plen < 6 || s->page_op != ((op & 0xff) == 0x08 ? 0x05 : 0x19) ||
            memcmp(p, s->page_addr, 6)) {
            r[0] = 0x02;                        /* (nothing of that) */
            memcpy(r + 1, p, MIN(plen, 6));
            ev_cc(s, op, r, 7);
            break;
        }
        memcpy(r + 1, p, 6);
        ev_cc(s, op, r, 7);
        timer_del(s->page);
        {
            uint8_t e[255] = { 0x02 };          /* Unknown Connection ID */

            if (s->page_op == 0x05) {
                memcpy(e + 3, s->page_addr, 6);
                e[9] = 0x01;
                ev_put(s, 0x03, e, 11);
            } else {
                memcpy(e + 1, s->page_addr, 6);
                ev_put(s, 0x07, e, 255);
            }
        }
        s->page_op = 0;
        break;
    case 0x0406:                                /* Disconnect */
    case 0x041d:                                /* Read Remote Version */
    case 0x041b:                                /* Read Remote Features */
    case 0x041c:                                /* ... Extended Features */
    case 0x2013:                                /* LE Connection Update */
    case 0x2016:                                /* LE Read Remote Features */
        ev_cs(s, op, 0x02);                     /* (no connection) */
        break;

    /* ---- link policy */
    case 0x080e:                                /* Read Default Link Policy */
        ev_cc(s, op, r, 3);
        break;
    case 0x080f:                                /* Write Default Link Policy */
        ev_cc_status(s, op, 0);
        break;

    /* ---- controller and baseband */
    case 0x0c03:                                /* Reset */
        controller_reset(s);
        ev_cc_status(s, op, 0);
        break;
    case 0x0c0d:                                /* Read Stored Link Key */
        stw_le_p(r + 1, 8);                     /* (room for 8, none) */
        ev_cc(s, op, r, 5);
        break;
    case 0x0c12:                                /* Delete Stored Link Key */
        ev_cc(s, op, r, 3);
        break;
    case 0x0c13:                                /* Write Local Name */
        memset(s->name, 0, sizeof(s->name));
        memcpy(s->name, p, MIN(plen, 248));
        ev_cc_status(s, op, 0);
        break;
    case 0x0c14:                                /* Read Local Name */
        memcpy(r + 1, s->name, 248);
        ev_cc(s, op, r, 249);
        break;
    case 0x0c15:                                /* Read Connection Accept
                                                   Timeout */
        stw_le_p(r + 1, 0x1f40);
        ev_cc(s, op, r, 3);
        break;
    case 0x0c17:                                /* Read Page Timeout */
        stw_le_p(r + 1, s->page_timeout);
        ev_cc(s, op, r, 3);
        break;
    case 0x0c18:                                /* Write Page Timeout */
        if (plen >= 2) {
            s->page_timeout = MAX(lduw_le_p(p), 1);
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x0c19:                                /* Read Scan Enable */
        r[1] = s->scan_enable;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c1a:                                /* Write Scan Enable */
        s->scan_enable = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c1b:                                /* Read Page Scan Activity */
        stw_le_p(r + 1, 0x0800);
        stw_le_p(r + 3, 0x0012);
        ev_cc(s, op, r, 5);
        break;
    case 0x0c1d:                                /* Read Inquiry Scan Activity */
        stw_le_p(r + 1, 0x1000);
        stw_le_p(r + 3, 0x0012);
        ev_cc(s, op, r, 5);
        break;
    case 0x0c1f:                                /* Read Authentication Enable */
        r[1] = s->auth_enable;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c20:                                /* Write Authentication
                                                   Enable */
        s->auth_enable = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c23:                                /* Read Class of Device */
        memcpy(r + 1, s->cod, 3);
        ev_cc(s, op, r, 4);
        break;
    case 0x0c24:                                /* Write Class of Device */
        memcpy(s->cod, p, MIN(plen, 3));
        ev_cc_status(s, op, 0);
        break;
    case 0x0c25:                                /* Read Voice Setting */
        stw_le_p(r + 1, s->voice_setting);
        ev_cc(s, op, r, 3);
        break;
    case 0x0c26:                                /* Write Voice Setting */
        if (plen >= 2) {
            s->voice_setting = lduw_le_p(p);
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x0c38:                                /* Read Number of Supported
                                                   IAC */
        r[1] = 1;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c39:                                /* Read Current IAC LAP */
        r[1] = 1;
        r[2] = 0x33, r[3] = 0x8b, r[4] = 0x9e;  /* the GIAC */
        ev_cc(s, op, r, 5);
        break;
    case 0x0c42:                                /* Read Inquiry Scan Type */
        r[1] = s->inquiry_scan_type;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c43:                                /* Write Inquiry Scan Type */
        s->inquiry_scan_type = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c44:                                /* Read Inquiry Mode */
        r[1] = s->inquiry_mode;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c45:                                /* Write Inquiry Mode */
        s->inquiry_mode = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c46:                                /* Read Page Scan Type */
        r[1] = s->page_scan_type;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c47:                                /* Write Page Scan Type */
        s->page_scan_type = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c51:                                /* Read Extended Inquiry
                                                   Response */
        r[1] = s->eir_fec;
        memcpy(r + 2, s->eir, 240);
        ev_cc(s, op, r, 242);
        break;
    case 0x0c52:                                /* Write Extended Inquiry
                                                   Response */
        if (plen >= 1) {
            s->eir_fec = p[0];
            memset(s->eir, 0, sizeof(s->eir));
            memcpy(s->eir, p + 1, MIN(plen - 1, 240));
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x0c55:                                /* Read Simple Pairing Mode */
        r[1] = s->ssp_mode;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c56:                                /* Write Simple Pairing Mode */
        s->ssp_mode = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c57:                                /* Read Local OOB Data */
    case 0x0c7d:                                /* ... Extended Data */
        qemu_guest_getrandom_nofail(r + 1, op == 0x0c57 ? 32 : 64);
        ev_cc(s, op, r, op == 0x0c57 ? 33 : 65);
        break;
    case 0x0c58:                                /* Read Inquiry Response TX
                                                   Power Level */
        r[1] = 0;                               /* 0 dBm */
        ev_cc(s, op, r, 2);
        break;
    case 0x0c5a:                                /* Read Default Erroneous
                                                   Data Reporting */
        r[1] = s->err_reporting;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c5b:                                /* Write ... */
        s->err_reporting = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c6c:                                /* Read LE Host Support */
        r[1] = s->le_host;
        ev_cc(s, op, r, 3);
        break;
    case 0x0c6d:                                /* Write LE Host Support */
        s->le_host = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c79:                                /* Read Secure Connections
                                                   Host Support */
        r[1] = s->sc_host;
        ev_cc(s, op, r, 2);
        break;
    case 0x0c7a:                                /* Write ... */
        s->sc_host = plen ? p[0] : 0;
        ev_cc_status(s, op, 0);
        break;
    case 0x0c01:                                /* Set Event Mask */
    case 0x0c05:                                /* Set Event Filter */
    case 0x0c16:                                /* Write Connection Accept
                                                   Timeout */
    case 0x0c1c:                                /* Write Page Scan Activity */
    case 0x0c1e:                                /* Write Inquiry Scan
                                                   Activity */
    case 0x0c2f:                                /* Write Synchronous Flow
                                                   Control Enable */
    case 0x0c31:                                /* Set Controller To Host
                                                   Flow Control */
    case 0x0c33:                                /* Host Buffer Size */
    case 0x0c3a:                                /* Write Current IAC LAP */
    case 0x0c63:                                /* Set Event Mask Page 2 */
        ev_cc_status(s, op, 0);
        break;

    /* ---- informational */
    case 0x1001:                                /* Read Local Version */
        r[1] = 0x0b;                            /* HCI 5.2 */
        stw_le_p(r + 2, 0x0000);
        r[4] = 0x0b;                            /* LMP 5.2 */
        stw_le_p(r + 5, 70);                    /* MediaTek */
        stw_le_p(r + 7, 0x2209);
        ev_cc(s, op, r, 9);
        break;
    case 0x1002:                                /* Read Local Supported
                                                   Commands */
        for (int i = 0; i < ARRAY_SIZE(supported_cmds); i++) {
            r[1 + supported_cmds[i][0]] |= 1 << supported_cmds[i][1];
        }
        ev_cc(s, op, r, 65);
        break;
    case 0x1003:                                /* Read Local Supported
                                                   Features */
        memcpy(r + 1, lmp_features, 8);
        ev_cc(s, op, r, 9);
        break;
    case 0x1004:                                /* Read Local Extended
                                                   Features */
        r[1] = plen ? p[0] : 0;
        r[2] = 2;                               /* (pages 0-2) */
        if (r[1] == 0) {
            memcpy(r + 3, lmp_features, 8);
        } else if (r[1] == 1) {                 /* the host's */
            r[3] = (s->ssp_mode ? 0x01 : 0) | (s->le_host ? 0x02 : 0) |
                   (s->sc_host ? 0x08 : 0);
        } else if (r[1] == 2) {
            r[4] = 0x03;                        /* SC, ping */
        }
        ev_cc(s, op, r, 11);
        break;
    case 0x1005:                                /* Read Buffer Size */
        stw_le_p(r + 1, 1021);                  /* ACL */
        r[3] = 255;                             /* SCO */
        stw_le_p(r + 4, 8);
        stw_le_p(r + 6, 4);
        ev_cc(s, op, r, 8);
        break;
    case 0x1009:                                /* Read BD_ADDR */
        memcpy(r + 1, s->bdaddr, 6);
        ev_cc(s, op, r, 7);
        break;
    case 0x100b:                                /* Read Local Supported
                                                   Codecs */
        r[1] = 3;                               /* CVSD, transparent, mSBC */
        r[2] = 0x02, r[3] = 0x03, r[4] = 0x05;
        r[5] = 0;                               /* (no vendor's) */
        ev_cc(s, op, r, 6);
        break;

    /* ---- LE */
    case 0x2002:                                /* LE Read Buffer Size */
        stw_le_p(r + 1, 251);
        r[3] = 8;
        ev_cc(s, op, r, 4);
        break;
    case 0x2003:                                /* LE Read Local Supported
                                                   Features */
        memcpy(r + 1, le_features, 8);
        ev_cc(s, op, r, 9);
        break;
    case 0x2007:                                /* LE Read Advertising
                                                   Channel TX Power */
        r[1] = 0;
        ev_cc(s, op, r, 2);
        break;
    case 0x200d:                                /* LE Create Connection */
        if (s->le_connecting) {
            ev_cs(s, op, 0x0c);
            break;
        }
        s->le_connecting = true;                /* (until cancelled) */
        ev_cs(s, op, 0);
        break;
    case 0x200e:                                /* LE Create Connection
                                                   Cancel */
        if (!s->le_connecting) {
            ev_cc_status(s, op, 0x0c);
            break;
        }
        s->le_connecting = false;
        ev_cc_status(s, op, 0);
        {
            uint8_t e[19] = { 0x01, 0x02 };     /* LE Connection Complete:
                                                   Unknown Connection ID */
            ev_put(s, 0x3e, e, sizeof(e));
        }
        break;
    case 0x200f:                                /* LE Read Filter Accept List
                                                   Size */
        r[1] = 16;
        ev_cc(s, op, r, 2);
        break;
    case 0x2018:                                /* LE Rand */
        qemu_guest_getrandom_nofail(r + 1, 8);
        ev_cc(s, op, r, 9);
        break;
    case 0x201c:                                /* LE Read Supported States */
        memcpy(r + 1, le_states, 8);
        ev_cc(s, op, r, 9);
        break;
    case 0x2023:                                /* LE Read Suggested Default
                                                   Data Length */
        stw_le_p(r + 1, 27);
        stw_le_p(r + 3, 328);
        ev_cc(s, op, r, 5);
        break;
    case 0x202a:                                /* LE Read Resolving List
                                                   Size */
        r[1] = 16;
        ev_cc(s, op, r, 2);
        break;
    case 0x202f:                                /* LE Read Maximum Data
                                                   Length */
        stw_le_p(r + 1, 251);
        stw_le_p(r + 3, 2120);
        stw_le_p(r + 5, 251);
        stw_le_p(r + 7, 2120);
        ev_cc(s, op, r, 9);
        break;
    case 0x204b:                                /* LE Read Transmit Power */
        r[1] = (uint8_t)-20;
        r[2] = 10;
        ev_cc(s, op, r, 3);
        break;
    case 0x2001:                                /* LE Set Event Mask */
    case 0x2005:                                /* LE Set Random Address */
    case 0x2006:                                /* LE Set Advertising
                                                   Parameters */
    case 0x2008:                                /* LE Set Advertising Data */
    case 0x2009:                                /* LE Set Scan Response Data */
    case 0x200a:                                /* LE Set Advertising Enable */
    case 0x200b:                                /* LE Set Scan Parameters */
    case 0x200c:                                /* LE Set Scan Enable */
    case 0x2010:                                /* LE Clear Filter Accept
                                                   List */
    case 0x2011:                                /* LE Add Device To ... */
    case 0x2012:                                /* LE Remove Device From ... */
    case 0x2024:                                /* LE Write Suggested Default
                                                   Data Length */
    case 0x2027:                                /* LE Add Device To
                                                   Resolving List */
    case 0x2028:                                /* LE Remove Device From ... */
    case 0x2029:                                /* LE Clear Resolving List */
    case 0x202d:                                /* LE Set Address Resolution
                                                   Enable */
    case 0x202e:                                /* LE Set RPA Timeout */
    case 0x2031:                                /* LE Set Default PHY */
    case 0x204e:                                /* LE Set Privacy Mode */
        ev_cc_status(s, op, 0);
        break;

    /* ---- MediaTek's */
    case 0xfc1a:                                /* its BD_ADDR, set */
        if (plen >= 6) {
            memcpy(s->bdaddr, p, 6);
        }
        ev_cc_status(s, op, 0);
        break;
    case 0xfd98:                                /* ISO over interface 2's
                                                   interrupt endpoints, set */
        ev_cc_status(s, op, 0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP | LOG_GUEST_ERROR, "mt7921: HCI command "
                      "0x%04x (%d bytes) not answered but as unknown\n",
                      op, plen);
        ev_cc_status(s, op, 0x01);              /* Unknown HCI Command */
        break;
    }
}

/* ------------------------------------------------------------ WMT */

static void wmt_reply(MT7921State *s, uint8_t op, uint8_t flag,
                      const uint8_t *data, int n)
{
    uint8_t *e = s->wmt;

    e[0] = HCI_EV_WMT;
    e[1] = 5 + n;
    e[2] = 2;                                   /* (the chip's to the host) */
    e[3] = op;
    stw_le_p(e + 4, 1 + n);
    e[6] = flag;
    memcpy(e + 7, data, n);
    s->wmt_len = 7 + n;
}

/* a WMT command (HCI command 0xfc6f's parameters): dir, op, dlen, flag,
 * its data */
static void wmt_command(MT7921State *s, const uint8_t *c, int len)
{
    uint8_t op, flag, st[2];

    if (len < 5) {
        return;
    }
    op = c[1];
    flag = c[4];
    switch (op) {
    case WMT_PATCH_DWNLD:
        if (!flag) {                            /* a section's query */
            wmt_reply(s, op, s->patched ? 2 : 0, NULL, 0);
        } else {                                /* its data: start,
                                                   continue, end */
            if (flag == 3) {
                s->dl_sections++;
            }
            wmt_reply(s, op, 0, NULL, 0);
        }
        break;
    case WMT_FUNC_CTRL:
        if (flag == 4) {                        /* a query */
            stw_be_p(st, s->bt_on ? 0x404 : 0x0);
            wmt_reply(s, op, 0, st, 2);
            break;
        }
        s->bt_on = len > 5 && c[5];
        if (s->bt_on && s->dl_sections) {
            s->patched = true;                  /* (its sections are in) */
        }
        wmt_reply(s, op, 0, NULL, 0);
        break;
    case WMT_SEMAPHORE:
        wmt_reply(s, op, s->patched ? 1 : 2, NULL, 0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP | LOG_GUEST_ERROR, "mt7921: WMT op 0x%02x: "
                      "an empty answer\n", op);
        wmt_reply(s, op, 0, NULL, 0);
        break;
    }
}

/* ------------------------------------------------- the chip's registers */

static void wifi_reset(MT7921State *s);

static uint32_t reg_get(MT7921State *s, uint32_t a)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                                GUINT_TO_POINTER(a)));
}

/* what a register reads: what was written, but for these */
static uint32_t mt7921_reg(MT7921State *s, uint32_t a)
{
    switch (a) {
    case 0x70010200:                            /* its ID */
        return 0x7961;
    case 0x70010204:                            /* its revision */
        return 0x10;
    case 0x80021004:                            /* (Bluetooth's) its
                                                   firmware's version: _1_2 */
        return 0x00000001;
    case 0x70002510:                            /* MTK_BT_MISC: reset done */
        return 0x100;
    case 0x7c0600f0:                            /* CONN_ON_MISC */
        return s->conn_on_misc;
    case 0x7c024208:                            /* WFDMA's GLO_CFG: its DMA */
        return reg_get(s, a) & ~(BIT(1) | BIT(3));      /* never busy */
    case 0x74000018:                            /* UDMA's WLCFG_0: ... */
        return reg_get(s, a) & ~(BIT(30) | BIT(31));
    case 0x74000a20:                            /* WFSYS: its init done */
        return reg_get(s, a) | BIT(22);
    case 0x820d4230:                            /* WTBL update: never busy */
        return reg_get(s, a) & ~BIT(31);
    default:
        return reg_get(s, a);
    }
}

static void mt7921_reg_write(MT7921State *s, uint32_t a, uint32_t v)
{
    uint32_t old = reg_get(s, a);

    g_hash_table_insert(s->regs, GUINT_TO_POINTER(a), GUINT_TO_POINTER(v));
    if (a == 0x70002600 && (old & 1) && !(v & 1)) {    /* WF_SUBSYS_RST, */
        wifi_reset(s);                                  /* pulsed */
    }
}

/* --------------------------------------------------- its Wi-Fi's MCU */

#define WIFI_EVQ_MAX    64                  /* events waiting, at most */
#define SCAN_NS         (1500 * SCALE_MS)   /* a scan's length */

/* the IN endpoint the MCU's events go to: 0x84 once WFDMA's HOST_CONFIG
 * says so (USB_RXEVT_EP4_EN), else 0x85 */
static int wifi_event_ep(MT7921State *s)
{
    return reg_get(s, 0x7c027030) & BIT(6) ? 4 : 5;
}

/* an event: its RXD (type 7, the exact length) and header (@28 eid, @29
 * the command's seq, @32 ext_eid, or a status), its payload at @36 */
static void wifi_event(MT7921State *s, uint8_t eid, uint8_t seq, uint8_t b32,
                       const void *pl, int n)
{
    GByteArray *e;
    uint8_t h[36] = { 0 };

    if (g_queue_get_length(&s->wevq) >= WIFI_EVQ_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "mt7921: Wi-Fi events not taken: "
                      "0x%02x dropped\n", eid);
        return;
    }
    stl_le_p(h, 0x38000000 | (36 + n));
    stw_le_p(h + 24, 36 + n - 24);
    h[28] = eid;
    h[29] = seq;
    h[32] = b32;
    e = g_byte_array_sized_new(36 + n);
    g_byte_array_append(e, h, 36);
    if (n) {
        g_byte_array_append(e, pl, n);
    }
    g_queue_push_tail(&s->wevq, e);
    usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN,
                          wifi_event_ep(s)), 0);
}

static void wifi_flush(MT7921State *s)
{
    GByteArray *e;

    while ((e = g_queue_pop_head(&s->wevq))) {
        g_byte_array_free(e, true);
    }
}

/* the WFSYS reset (a pulse on WF_SUBSYS_RST): its MCU back in its ROM */
static void wifi_reset(MT7921State *s)
{
    s->wifi_patched = false;
    s->conn_on_misc = 1;                        /* (powered, nothing on) */
    wifi_flush(s);
    timer_del(s->scan);
}

static void scan_done(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t done[476] = { 0 };                  /* (no network found) */

    wifi_event(s, 0x0d, 0, 0, done, sizeof(done));      /* SCAN_DONE */
}

/* its NIC's capabilities: its MAC address; two streams, HT, VHT and HE, 80
 * MHz, LDPC and STBC, 2.4 and 5 GHz */
static void wifi_nic_capab(MT7921State *s, uint8_t seq)
{
    uint8_t pl[4 + 8 + 8 + 8 + 12] = { 2 };     /* two elements */
    static const uint8_t phy[12] = { 1, 1, 1, 2, 2, 0, 1, 1, 1, 1, 3, 1 };

    stl_le_p(pl + 4, 0x07);                     /* MAC_ADDR */
    stl_le_p(pl + 8, 8);
    memcpy(pl + 12, s->wifi_mac, 6);
    stl_le_p(pl + 20, 0x08);                    /* PHY */
    stl_le_p(pl + 24, 12);
    memcpy(pl + 28, phy, 12);
    wifi_event(s, 0x01, seq, 0, pl, sizeof(pl));
}

static void wifi_unknown(const char *kind, unsigned id, int n)
{
    qemu_log_mask(LOG_UNIMP | LOG_GUEST_ERROR, "mt7921: Wi-Fi %s command "
                  "0x%02x (%d bytes): answered as done\n", kind, id, n);
}

/* a legacy (the ROM's) command: the patch, the RAM firmware, its start */
static void wifi_legacy(MT7921State *s, uint8_t cid, uint8_t seq,
                        const uint8_t *pl, int n)
{
    switch (cid) {
    case 0x04:                                  /* NIC_POWER_CTRL */
        s->conn_on_misc = (s->conn_on_misc & ~7) | 1;
        break;
    case 0x10:                                  /* PATCH_SEM_CONTROL: get, */
        wifi_event(s, 0x04, seq, n >= 4 && ldl_le_p(pl) ?   /* release */
                   (s->wifi_patched ? 1 : 2) : 3, NULL, 0);
        break;
    case 0x01:                                  /* TARGET_ADDRESS_LEN_REQ */
    case 0x05:                                  /* PATCH_START_REQ */
        wifi_event(s, 0x01, seq, 0, NULL, 0);   /* (its data: FW_SCATTER) */
        break;
    case 0x07:                                  /* PATCH_FINISH_REQ */
        s->wifi_patched = true;
        wifi_event(s, 0x01, seq, 0, NULL, 0);   /* (eid 0x01: 0x07 is an
                                                   event's own) */
        break;
    case 0x02:                                  /* FW_START_REQ: running */
        wifi_event(s, 0x01, seq, 0, NULL, 0);
        s->conn_on_misc = (s->conn_on_misc & ~3) | 3;
        break;
    default:
        wifi_unknown("legacy", cid, n);
        wifi_event(s, 0x01, seq, 0, NULL, 0);
        break;
    }
}

/* a CE command (the RAM firmware's) */
static void wifi_ce(MT7921State *s, uint8_t cid, bool set, uint8_t seq,
                    const uint8_t *pl, int n)
{
    uint8_t r[8] = { 0 };

    switch (cid) {
    case 0x8a:                                  /* GET_NIC_CAPAB */
        wifi_nic_capab(s, seq);
        break;
    case 0xc0:                                  /* REG_READ, REG_WRITE */
        if (!set) {
            memcpy(r, pl, MIN(n, 4));           /* {reg, val 0} */
            wifi_event(s, 0x01, seq, 0, r, 8);
        }
        break;
    case 0x03:                                  /* START_HW_SCAN */
        timer_mod(s->scan, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCAN_NS);
        break;
    case 0x1b:                                  /* CANCEL_HW_SCAN (the
                                                   driver completes it) */
        timer_del(s->scan);
        break;
    case 0x0a:                                  /* SET_RX_FILTER */
    case 0x0f:                                  /* SET_CHAN_DOMAIN */
    case 0x1d:                                  /* SET_EDCA_PARMS */
    case 0x5d:                                  /* SET_RATE_TX_POWER */
    case 0x61:                                  /* SCHED_SCAN_ENABLE */
    case 0x62:                                  /* SCHED_SCAN_REQ */
    case 0xc5:                                  /* FWLOG_2_HOST */
    case 0xca:                                  /* CHIP_CONFIG */
        break;                                  /* (none waits) */
    default:
        wifi_unknown("CE", cid, n);
        wifi_event(s, 0x01, seq, 0, NULL, 0);
        break;
    }
}

/* an EXT command (cid 0xed) */
static void wifi_ext(MT7921State *s, uint8_t ext, uint8_t seq,
                     const uint8_t *pl, int n)
{
    uint8_t r[8] = { 0 };

    switch (ext) {
    case 0x2c:                                  /* THERMAL_CTRL: °C @40 */
        stl_le_p(r + 4, 45);
        wifi_event(s, 0xed, seq, ext, r, 8);
        break;
    case 0xbd:                                  /* WF_RF_PIN_CTRL: */
        if (n >= 1 && pl[0] == 1) {             /* not blocked */
            r[0] = 1;
            wifi_event(s, 0xed, seq, ext, r, 4);
        }
        break;
    case 0x08:                                  /* CHANNEL_SWITCH */
    case 0x21:                                  /* EFUSE_BUFFER_MODE */
    case 0x3e:                                  /* PROTECT_CTRL */
    case 0x46:                                  /* MAC_INIT_CTRL */
    case 0x4e:                                  /* SET_RX_PATH */
        wifi_event(s, 0xed, seq, ext, NULL, 0);
        break;
    default:
        wifi_unknown("EXT", ext, n);
        wifi_event(s, 0xed, seq, ext, NULL, 0);
        break;
    }
}

/* a UNI command: its answer names it, status 0 */
static void wifi_uni(MT7921State *s, uint16_t cid, uint8_t seq, int n)
{
    uint8_t r[8] = { cid };

    switch (cid) {
    case 0x01:                                  /* DEV_INFO_UPDATE */
    case 0x02:                                  /* BSS_INFO_UPDATE */
    case 0x03:                                  /* STA_REC_UPDATE */
    case 0x05:                                  /* SUSPEND */
    case 0x06:                                  /* OFFLOAD */
    case 0x07:                                  /* HIF_CTRL */
        wifi_event(s, 0x01, seq, 0, r, 8);
        break;
    case 0x24:                                  /* SNIFFER */
        wifi_event(s, 0x01, seq, 0, NULL, 0);
        break;
    default:
        wifi_unknown("UNI", cid, n);
        wifi_event(s, 0x01, seq, 0, r, 8);
        break;
    }
}

/* a command on 0x08: a 4-byte length, the TXD (8 words), the MCU's header
 * (UNI if its option has bit 1: its cid a word at @34; else the cid @36,
 * set_query @38 (3: legacy), ext_cid @41), seq @39, the payload */
static void wifi_command(MT7921State *s, const uint8_t *b, int len)
{
    const uint8_t *t = b + 4;
    uint32_t l;
    uint8_t seq;

    if (len < 4 + 48) {
        return;
    }
    l = MIN(ldl_le_p(b) & 0xffff, (uint32_t)len - 4);
    seq = t[39];
    if (t[43] & 2) {                            /* UNI */
        wifi_uni(s, lduw_le_p(t + 34), seq, l >= 48 ? l - 48 : 0);
    } else if (l >= 64) {
        if (t[36] == 0xed) {
            wifi_ext(s, t[41], seq, t + 64, l - 64);
        } else if (t[38] == 3) {
            wifi_legacy(s, t[36], seq, t + 64, l - 64);
        } else {
            wifi_ce(s, t[36], t[38] == 1, seq, t + 64, l - 64);
        }
    }
}

/* -------------------------------------------------- as USB drives it */

static void mt7921_handle_control(USBDevice *dev, USBPacket *p, int request,
                                  int value, int index, int length,
                                  uint8_t *data)
{
    MT7921State *s = MT7921(dev);

    if (usb_desc_handle_control(dev, p, request, value, index, length,
                                data) >= 0) {
        return;
    }
    switch (request) {
    case 0x2000:                                /* an HCI command (class, */
    case 0x2100:                                /* to the device or its
                                                   interface) */
        if (length >= 3 && lduw_le_p(data) == 0xfc6f) {
            wmt_command(s, data + 3, MIN(data[2], length - 3));
        } else {
            hci_command(s, data, length);
        }
        break;
    case VendorDeviceRequest | 0x63:            /* a register (Bluetooth's */
    case 0xdf63:                                /* and Wi-Fi's: READ_EXT), */
    case 0xde01:                                /* a uhw one */
        stl_le_p(data, mt7921_reg(s, (uint32_t)value << 16 | index));
        p->actual_length = MIN(4, length);
        break;
    case 0x5f66:                                /* ... written (WRITE_EXT) */
    case 0x5e02:
        if (length >= 4) {
            mt7921_reg_write(s, (uint32_t)value << 16 | index,
                             ldl_le_p(data));
        }
        break;
    case 0x5f04:                                /* the Wi-Fi's MCU: power */
        s->conn_on_misc |= 1;
        break;
    case VendorDeviceRequest | 0x01:            /* a WMT event, if one is */
        p->actual_length = MIN(s->wmt_len, length);
        memcpy(data, s->wmt, p->actual_length);
        s->wmt_len = 0;
        break;
    default:                                    /* (a stall makes mt7921u
                                                   think its bus hung) */
        qemu_log_mask(LOG_UNIMP | LOG_GUEST_ERROR, "mt7921: control request "
                      "0x%04x (0x%04x, 0x%04x): nothing done\n", request,
                      value, index);
        if (request & (USB_DIR_IN << 8)) {
            memset(data, 0, length);
            p->actual_length = length;
        }
        break;
    }
}

static void mt7921_handle_data(USBDevice *dev, USBPacket *p)
{
    MT7921State *s = MT7921(dev);
    uint32_t n;

    if (p->pid == USB_TOKEN_IN && p->ep->nr == 1) {     /* HCI events */
        if (!s->evq->len) {
            p->status = USB_RET_NAK;
            return;
        }
        if (!s->ev_left) {
            s->ev_left = 2 + s->evq->data[1];
        }
        n = MIN(MIN(s->ev_left, INTR_MPS), p->iov.size);
        usb_packet_copy(p, s->evq->data, n);
        g_byte_array_remove_range(s->evq, 0, n);
        s->ev_left -= n;
        return;
    }
    if (p->pid == USB_TOKEN_IN && (p->ep->nr == 4 || p->ep->nr == 5)) {
        GByteArray *e;                          /* the Wi-Fi's MCU events */

        if (p->ep->nr != wifi_event_ep(s) ||
            !(e = g_queue_pop_head(&s->wevq))) {
            p->status = USB_RET_NAK;            /* (no frame is received) */
            return;
        }
        usb_packet_copy(p, e->data, MIN(e->len, p->iov.size));
        g_byte_array_free(e, true);
        return;
    }
    if (p->pid == USB_TOKEN_OUT && p->ep->nr == 8) {    /* an MCU command */
        g_autofree uint8_t *b = g_malloc(p->iov.size);

        usb_packet_copy(p, b, p->iov.size);
        wifi_command(s, b, p->iov.size);
        return;
    }
    if (p->pid == USB_TOKEN_OUT) {              /* ACL, ISO, the Wi-Fi's
                                                   firmware data (0x04 while
                                                   FW_DL_EN) and frames: no
                                                   link takes them */
        p->actual_length = p->iov.size;
        return;
    }
    p->status = USB_RET_NAK;                    /* (no ACL, SCO or ISO
                                                   comes) */
}

static void mt7921_handle_reset(USBDevice *dev)
{
    MT7921State *s = MT7921(dev);

    g_byte_array_set_size(s->evq, 0);
    s->ev_left = 0;
    s->wmt_len = 0;
    wifi_flush(s);
}

void mt7921_power(USBDevice *dev, bool on)
{
    MT7921State *s = MT7921(dev);
    Error *err = NULL;

    if (on == dev->attached) {
        return;
    }
    if (!on) {
        usb_device_detach(dev);
        controller_reset(s);
        timer_del(s->scan);
        return;
    }
    /* a cold chip: no patch, Bluetooth off, the Wi-Fi's MCU unpowered */
    s->patched = s->bt_on = false;
    s->dl_sections = 0;
    controller_reset(s);
    g_hash_table_remove_all(s->regs);
    s->conn_on_misc = 0;
    s->wifi_patched = false;
    timer_del(s->scan);
    mt7921_handle_reset(dev);
    usb_device_attach(dev, &err);
    if (err) {
        error_report_err(err);
    }
}

static void mt7921_realize(USBDevice *dev, Error **errp)
{
    MT7921State *s = MT7921(dev);
    unsigned int a[6];

    if (sscanf(s->bdaddr_prop ?: "00:0c:e7:46:4f:01", "%x:%x:%x:%x:%x:%x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        error_setg(errp, "mt7921: bdaddr: xx:xx:xx:xx:xx:xx");
        return;
    }
    for (int i = 0; i < 6; i++) {
        s->bdaddr[i] = a[5 - i];
    }
    if (sscanf(s->wifi_mac_prop ?: "00:0c:e7:46:4f:02", "%x:%x:%x:%x:%x:%x",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        error_setg(errp, "mt7921: wifi-mac: xx:xx:xx:xx:xx:xx");
        return;
    }
    for (int i = 0; i < 6; i++) {
        s->wifi_mac[i] = a[i];
    }
    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->evq = g_byte_array_new();
    s->inquiry = timer_new_ns(QEMU_CLOCK_VIRTUAL, inquiry_done, s);
    s->page = timer_new_ns(QEMU_CLOCK_VIRTUAL, page_done, s);
    s->scan = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan_done, s);
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_queue_init(&s->wevq);
    controller_reset(s);
    dev->auto_attach = false;                   /* (powered by the board) */
}

static const Property mt7921_props[] = {
    DEFINE_PROP_STRING("bdaddr", MT7921State, bdaddr_prop),
    DEFINE_PROP_STRING("wifi-mac", MT7921State, wifi_mac_prop),
};

static void mt7921_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    USBDeviceClass *uc = USB_DEVICE_CLASS(oc);

    uc->realize = mt7921_realize;
    uc->product_desc = "MediaTek MT7921";
    uc->usb_desc = &mt7921_desc;
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = mt7921_handle_reset;
    uc->handle_control = mt7921_handle_control;
    uc->handle_data = mt7921_handle_data;
    device_class_set_props(dc, mt7921_props);
    dc->desc = "MediaTek MT7921 Wi-Fi and Bluetooth";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo mt7921_types[] = {
    {
        .name = TYPE_MT7921,
        .parent = TYPE_USB_DEVICE,
        .instance_size = sizeof(MT7921State),
        .class_init = mt7921_class_init,
    },
};

DEFINE_TYPES(mt7921_types)
