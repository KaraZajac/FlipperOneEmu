/*
 * The MediaTek MT7921 (MT7961) on the Flipper One's USB1, Wi-Fi and
 * Bluetooth in one chip, as Linux's btusb and btmtk (its Bluetooth) and
 * mt7921u (its Wi-Fi) drive it.
 *
 * The chip is on USB while its PMU_EN is high (GPIO1_D5, the device tree's
 * rfkill-gpio "Onboard WiFi+BT", blocked by default: the board,
 * mt7921_power()), and each power-up is a cold chip: its patch firmware
 * gone. As MediaTek's USB devices have it:
 * - its descriptors: 0e8d:7961, USB 3.0 as the module is (on the board,
 *   the hub's SuperSpeed port 1: sl6341.c), an interface association of three
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
 *   settings, inquiry (complete after its length), LE scanning and
 *   advertising, BR/EDR connections (pages of anything else time out).
 *   No radio, but three devices near the Flipper, which its scans hear:
 *   - over LE, a beacon (non-connectable, "FlipperOneEmu Beacon") and a
 *     sensor ("FlipperOneEmu Sensor"), which a connection reaches: its
 *     ACL data over bulk 0x02 and 0x82, L2CAP, and an ATT server with its
 *     GATT database (Generic Access, Battery, Heart Rate with its
 *     notifications once the host subscribes, Device Information);
 *     pairing it refuses;
 *   - over BR/EDR, a speaker ("FlipperOneEmu Speaker", an inquiry's
 *     extended result), which pairs with SSP (Just Works), answers SDP and
 *     plays: A2DP's sink, SBC, what it receives to the "speaker" file.
 *   The values not fixed by the protocol (its subversion, the TX powers,
 *   the lists' sizes) are a typical controller's, not read from a device.
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
 * scan ends after a while. Commands it does not know get an answer as if
 * done, and are logged.
 *
 * With an uplink (its "netdev", a -nic of model mt7921) an access point is
 * near it: an open 802.11n network ("ap-ssid", FlipperOneEmu, on
 * "ap-channel" 6), whose beacon each scan of its channel finds. It is HT
 * (two streams, 20 MHz, short guard interval: MCS 0-15) with WMM, which
 * mac80211 wants for HT: its data to the station comes at MCS 15 (144.4
 * Mbit/s), as do the TX statuses of the station's data; management frames
 * at 6 Mbit/s, as an AP sends them. A Block Ack session the station asks
 * for is accepted (its ADDBA Response). Frames go as the chip
 * receives them (an RXD of type 2, the channel, RCPI for -42 dBm, the
 * 802.11 frame) on 0x84; those the host sends come as a TXD and the frame
 * (802.11, or 802.3 once the station is authorized), and each that asks
 * for its TX status gets it, acked, before the AP answers (mac80211
 * weighs them in that order): the AP's authentication and association
 * responses, 30 ms on. The remain-on-channel the firmware's CNM asks for
 * is granted at once. Data is bridged: the station's to the uplink as
 * Ethernet frames, the uplink's to the station as 802.11 ones.
 *
 * With a passphrase ("ap-psk") a second network is beside it, WPA2-PSK
 * (CCMP), its SSID the first's with "-WPA2", its BSSID the next one: its
 * beacon has an RSN element, and after the association its AP runs the
 * 4-way handshake (the PMK from the passphrase, PBKDF2; the PTK, the
 * PRF's; EAPOL-Key messages 1 and 3, their MICs HMAC-SHA1's, the GTK
 * wrapped with AES; a wrong MIC, a wrong passphrase: deauthenticated).
 * The keys go to the chip, which ciphers on the air: on USB the frames
 * stay clear, the AP's marked as the chip decrypted them (RXD1's
 * SEC_MODE CCMP, the Protected bit).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "crypto/aes.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "net/net.h"
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
    QEMUTimer *beacon_ble;              /* an LE scan's: the beacon's
                                           advertisement, now and then */
    bool le_scan_dup;                   /* ... once a scan, the host
                                           filtering duplicates */
    bool le_scan_active;                /* ... an active one: the sensor's
                                           scan response too */
    uint8_t le_evmask[8];               /* the host's LE event mask */
    /* the sensor near it: a connectable LE peripheral, its GATT server */
    bool le_accept_sensor;              /* in the host's accept list */
    QEMUTimer *le_conn_timer;           /* a connection to it, being made */
    uint16_t le_conn_params[3];         /* ... its interval, latency and
                                           supervision timeout */
    bool le_conn;                       /* connected (LE_HANDLE) */
    uint16_t att_mtu;
    uint8_t cccd[3];                    /* its CCCDs: Service Changed,
                                           Battery Level, Heart Rate */
    QEMUTimer *hr_timer;                /* its heart rate notifications */
    uint8_t hr_beat;
    /* the speaker near it: a BR/EDR device, SSP's pairing, its SDP */
    QEMUTimer *inq_result;              /* an inquiry finding it */
    QEMUTimer *br_page;                 /* a page of it, answered */
    uint8_t br_page_op;                 /* ... a connection's (0x05) or a
                                           name request's (0x19) */
    bool br_conn;                       /* connected (BR_HANDLE) */
    uint8_t br_l2rx[2048];              /* an L2CAP PDU on it, put together */
    int br_l2rx_len;
    uint16_t sdp_cid;                   /* the host's end of the SDP channel
                                           (0: none) */
    uint16_t avdtp_cid[2];              /* ... of AVDTP's: signalling, the
                                           media (0: none) */
    uint8_t avdtp_conf[16];             /* its stream's configuration, as set */
    int avdtp_conf_len;
    char *speaker_path;                 /* "speaker": where what it plays
                                           goes (SBC) */
    int speaker_fd;
    GQueue aclq;                        /* ACL packets to the host (0x82) */
    USBEndpoint *acl_in;
    uint8_t l2rx[512];                  /* an L2CAP PDU from the host, put
                                           together */
    int l2rx_len;
    /* the chip's registers: std and uhw, the Bluetooth's and Wi-Fi's */
    GHashTable *regs;                   /* address → value (else 0) */
    /* its Wi-Fi */
    char *wifi_mac_prop;                /* "wifi-mac" (MediaTek's OUI, "FO",
                                           2 unless told) */
    uint8_t wifi_mac[6];
    uint32_t conn_on_misc;              /* bits 2:0: the MCU's state */
    bool wifi_patched;                  /* the ROM's patch is in */
    GQueue wq[2];                       /* transfers for IN 0x84 (frames,
                                           TX status, events) and 0x85 */
    QEMUTimer *scan;                    /* a scan's end */
    /* the access point near it, with an uplink (its NIC's netdev) */
    NICConf conf;
    NICState *nic;
    char *ap_ssid;                      /* "ap-ssid" */
    uint8_t ap_chan;                    /* "ap-channel" */
    uint8_t bssid[6];
    uint8_t sta[6];                     /* the station, as its frames say */
    int sta_state;                      /* AP_NONE, AP_AUTH, AP_ASSOC */
    int sta_bss;                        /* ... on which network: 0 open,
                                           1 WPA2 */
    /* the WPA2 network: its passphrase ("ap-psk"), the handshake's state */
    char *ap_psk;
    char *ssid2;
    uint8_t bssid2[6];
    uint8_t pmk[32], anonce[32], kck[16], kek[16], gtk[16];
    uint64_t replay;
    bool sta_secure;                    /* the handshake done */
    QEMUTimer *eapol;                   /* its message 1, after the
                                           association's answer */
    uint16_t ap_seq;
    QEMUTimer *beacon;                  /* a beacon, in a scan */
    QEMUTimer *ap_reply;                /* the AP's answer, then */
    GByteArray *ap_pending;
};

enum { AP_NONE, AP_AUTH, AP_ASSOC };

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

/* the endpoints at full speed (fs), high speed (hs) and SuperSpeed (ss):
 * bulk packets and the interrupt endpoints' interval differ; at
 * SuperSpeed, each periodic endpoint's companion has its bytes an
 * interval (what QEMU's descriptors take at the other speeds too, and
 * leave out) */
#define HCI_EPS(bulk, ival) (USBDescEndpoint[]) {                       \
        { .bEndpointAddress = USB_DIR_IN | 1,                           \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = INTR_MPS, .bInterval = (ival),              \
          .wBytesPerInterval = INTR_MPS },                              \
        { .bEndpointAddress = USB_DIR_IN | 2,                           \
          .bmAttributes = USB_ENDPOINT_XFER_BULK,                       \
          .wMaxPacketSize = (bulk) },                                   \
        { .bEndpointAddress = USB_DIR_OUT | 2,                          \
          .bmAttributes = USB_ENDPOINT_XFER_BULK,                       \
          .wMaxPacketSize = (bulk) } }
#define SCO_EPS(mps, ival) (USBDescEndpoint[]) {                        \
        { .bEndpointAddress = USB_DIR_IN | 3,                           \
          .bmAttributes = USB_ENDPOINT_XFER_ISOC,                       \
          .wMaxPacketSize = (mps), .bInterval = (ival),                 \
          .wBytesPerInterval = (mps) },                                 \
        { .bEndpointAddress = USB_DIR_OUT | 3,                          \
          .bmAttributes = USB_ENDPOINT_XFER_ISOC,                       \
          .wMaxPacketSize = (mps), .bInterval = (ival),                 \
          .wBytesPerInterval = (mps) } }
#define ISO_EPS(ival) (USBDescEndpoint[]) {                             \
        { .bEndpointAddress = USB_DIR_IN | 10,                          \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = 64, .bInterval = (ival),                    \
          .wBytesPerInterval = 64 },                                    \
        { .bEndpointAddress = USB_DIR_OUT | 10,                         \
          .bmAttributes = USB_ENDPOINT_XFER_INT,                        \
          .wMaxPacketSize = 64, .bInterval = (ival),                    \
          .wBytesPerInterval = 64 } }

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
static const USBDescIface wifi_iface_ss = WIFI_IFACE(1024);

static const USBDescIface bt_ifaces_fs[N_BT_IFACES] = BT_IFACES(64, 1);
static const USBDescIface bt_ifaces_hs[N_BT_IFACES] = BT_IFACES(512, 4);
static const USBDescIface bt_ifaces_ss[N_BT_IFACES] = BT_IFACES(1024, 4);

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
static const USBDescIfaceAssoc bt_group_ss = {
    .bFirstInterface = 0, .bInterfaceCount = 3,
    .bFunctionClass = 0xe0, .bFunctionSubClass = 0x01,
    .bFunctionProtocol = 0x01,
    .nif = N_BT_IFACES, .ifs = bt_ifaces_ss,
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
/* (USB 3.0: on the board, the hub's SuperSpeed port 1) */
static const USBDescDevice mt7921_dev_ss = {
    .bcdUSB = 0x0320, .bDeviceClass = 0xef, .bDeviceSubClass = 0x02,
    .bDeviceProtocol = 0x01, .bMaxPacketSize0 = 9, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) { {
        .bNumInterfaces = 4, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |
                        USB_CFG_ATT_WAKEUP,
        .bMaxPower = 50,
        .nif_groups = 1, .if_groups = &bt_group_ss,
        .nif = 1, .ifs = &wifi_iface_ss,
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
    .super = &mt7921_dev_ss,
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

#define LE_HANDLE   0x0040              /* the sensor's connection */

/* the sensor's connection gone (or never made): its timers, its packets */
static void le_conn_drop(MT7921State *s)
{
    GByteArray *a;

    s->le_conn = false;
    timer_del(s->le_conn_timer);
    timer_del(s->hr_timer);
    while ((a = g_queue_pop_head(&s->aclq))) {
        g_byte_array_free(a, true);
    }
}

static void controller_reset(MT7921State *s)
{
    timer_del(s->inquiry);
    timer_del(s->inq_result);
    timer_del(s->br_page);
    s->br_page_op = 0;
    s->br_conn = false;                         /* (the speaker's link) */
    s->br_l2rx_len = 0;
    s->sdp_cid = 0;
    s->avdtp_cid[0] = s->avdtp_cid[1] = 0;
    timer_del(s->page);
    s->page_op = 0;
    s->le_connecting = false;
    timer_del(s->beacon_ble);
    le_conn_drop(s);
    s->le_accept_sensor = false;
    s->le_scan_active = false;
    memset(s->le_evmask, 0, sizeof(s->le_evmask));
    s->le_evmask[0] = 0x1f;                     /* (its default) */
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

/* ------------------------------------------ the LE devices near it */

/* (their random static addresses: C0:FE:46:4F:42:01 and :02) */
static const uint8_t beacon_addr[6] = { 0x01, 0x42, 0x4f, 0x46, 0xfe, 0xc0 };
static const uint8_t sensor_addr[6] = { 0x02, 0x42, 0x4f, 0x46, 0xfe, 0xc0 };

/* an LE Advertising Report: one, of @type (ADV_IND 0, ADV_NONCONN_IND 3,
 * SCAN_RSP 4), from a random address */
static void adv_report(MT7921State *s, uint8_t type, const uint8_t *addr,
                       const uint8_t *data, int n, int8_t rssi)
{
    uint8_t e[48] = { 0x02, 1, type, 0x01 };

    memcpy(e + 4, addr, 6);
    e[10] = n;
    memcpy(e + 11, data, n);
    e[11 + n] = rssi;
    ev_put(s, 0x3e, e, 12 + n);
}

/* advertising data: its flags (general discoverable, no BR/EDR), its
 * services (16-bit UUIDs), its complete name */
static int adv_data(uint8_t *d, const uint8_t *uuids, int nu,
                    const char *name)
{
    int n = 0, l = strlen(name);

    d[n++] = 2, d[n++] = 0x01, d[n++] = 0x06;
    if (nu) {
        d[n++] = 1 + nu, d[n++] = 0x03;
        memcpy(d + n, uuids, nu);
        n += nu;
    }
    d[n++] = 1 + l, d[n++] = 0x09;
    memcpy(d + n, name, l);
    return n + l;
}

/*
 * What an LE scan hears, every second, or once a scan when the host filters
 * duplicates (BlueZ's passive scan, always on, then hears them once):
 * - the beacon: non-connectable, its flags and name, at -60 dBm;
 * - the sensor while it is not connected: connectable, its flags, its
 *   services (Heart Rate, Battery) and name, at -55 dBm, and to an active
 *   scan its scan response (its TX power).
 */
static void beacon_ble(void *opaque)
{
    MT7921State *s = opaque;
    static const uint8_t svcs[] = { 0x0d, 0x18, 0x0f, 0x18 };
    static const uint8_t txp[] = { 2, 0x0a, 0 };
    uint8_t d[31];

    adv_report(s, 0x03, beacon_addr, d,
               adv_data(d, NULL, 0, "FlipperOneEmu Beacon"), -60);
    if (!s->le_conn && !timer_pending(s->le_conn_timer)) {
        adv_report(s, 0x00, sensor_addr, d,
                   adv_data(d, svcs, sizeof(svcs), "FlipperOneEmu Sensor"),
                   -55);
        if (s->le_scan_active) {
            adv_report(s, 0x04, sensor_addr, txp, sizeof(txp), -55);
        }
    }
    if (!s->le_scan_dup) {
        timer_mod(s->beacon_ble, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND);
    }
}

/* an L2CAP PDU to the host on the sensor's connection: one ACL packet
 * (bulk 0x82), as the LE buffers take it (251 bytes) */
static void l2cap_send(MT7921State *s, uint16_t cid, const uint8_t *q, int n)
{
    GByteArray *a = g_byte_array_sized_new(8 + n);
    uint8_t h[8];

    stw_le_p(h, LE_HANDLE | 0x2000);            /* (PB: a start, flushable) */
    stw_le_p(h + 2, 4 + n);
    stw_le_p(h + 4, n);
    stw_le_p(h + 6, cid);
    g_byte_array_append(a, h, 8);
    g_byte_array_append(a, q, n);
    g_queue_push_tail(&s->aclq, a);
    usb_wakeup(s->acl_in, 0);
}

/* the sensor's GATT database, handles from 1 (16-bit UUIDs only) */
enum { GATT_R = 1, GATT_CCCD = 2, GATT_DYN = 4 };
typedef struct GattAttr {
    uint16_t type;
    uint8_t flags, len;
    const char *val;
} GattAttr;

static const GattAttr gatt_db[] = {
    { 0x2800, GATT_R, 2, "\x00\x18" },          /* 1: Generic Access */
    { 0x2803, GATT_R, 5, "\x02\x03\x00\x00\x2a" },  /* Device Name: read */
    { 0x2a00, GATT_R, 20, "FlipperOneEmu Sensor" },
    { 0x2803, GATT_R, 5, "\x02\x05\x00\x01\x2a" },  /* Appearance: read */
    { 0x2a01, GATT_R, 2, "\x41\x03" },          /* (a heart rate belt) */
    { 0x2800, GATT_R, 2, "\x01\x18" },          /* 6: Generic Attribute */
    { 0x2803, GATT_R, 5, "\x20\x08\x00\x05\x2a" },  /* Service Changed:
                                                       indicate */
    { 0x2a05, 0, 4, "\x01\x00\xff\xff" },
    { 0x2902, GATT_R | GATT_CCCD, 2, NULL },
    { 0x2800, GATT_R, 2, "\x0f\x18" },          /* 10: Battery */
    { 0x2803, GATT_R, 5, "\x12\x0c\x00\x19\x2a" },  /* Battery Level: read,
                                                       notify */
    { 0x2a19, GATT_R | GATT_DYN, 1, NULL },
    { 0x2902, GATT_R | GATT_CCCD, 2, NULL },
    { 0x2800, GATT_R, 2, "\x0d\x18" },          /* 14: Heart Rate */
    { 0x2803, GATT_R, 5, "\x10\x10\x00\x37\x2a" },  /* Heart Rate
                                                       Measurement: notify */
    { 0x2a37, GATT_DYN, 2, NULL },
    { 0x2902, GATT_R | GATT_CCCD, 2, NULL },
    { 0x2803, GATT_R, 5, "\x02\x13\x00\x38\x2a" },  /* Body Sensor
                                                       Location: read */
    { 0x2a38, GATT_R, 1, "\x01" },              /* (the chest) */
    { 0x2800, GATT_R, 2, "\x0a\x18" },          /* 20: Device Information */
    { 0x2803, GATT_R, 5, "\x02\x16\x00\x29\x2a" },  /* Manufacturer Name:
                                                       read */
    { 0x2a29, GATT_R, 13, "FlipperOneEmu" },
    { 0x2803, GATT_R, 5, "\x02\x18\x00\x24\x2a" },  /* Model Number: read */
    { 0x2a24, GATT_R, 6, "Sensor" },
};
#define GATT_N ((int)ARRAY_SIZE(gatt_db))

/* a CCCD's index in the state: Service Changed's, Battery Level's, Heart
 * Rate Measurement's */
static int gatt_cccd(int h)
{
    return h == 9 ? 0 : h == 13 ? 1 : h == 17 ? 2 : -1;
}

/* attribute @h's value into @v: its length */
static int gatt_value(MT7921State *s, int h, uint8_t *v)
{
    const GattAttr *a = &gatt_db[h - 1];

    if (a->flags & GATT_CCCD) {
        v[0] = s->cccd[gatt_cccd(h)];
        v[1] = 0;
        return 2;
    }
    if (h == 12) {
        v[0] = 87;                              /* (its battery: 87 %) */
        return 1;
    }
    if (h == 16) {
        v[0] = 0;                               /* (flags: 8-bit rate) */
        v[1] = 68 + s->hr_beat % 9;             /* (beats a minute) */
        return 2;
    }
    memcpy(v, a->val, a->len);
    return a->len;
}

/* the last handle of the service at @h: before the next service's */
static int gatt_group_end(int h)
{
    while (h < GATT_N && gatt_db[h].type != 0x2800) {
        h++;
    }
    return h;
}

static void att_error(MT7921State *s, uint8_t req, uint16_t h, uint8_t err)
{
    uint8_t r[5] = { 0x01, req };

    stw_le_p(r + 2, h);
    r[4] = err;
    l2cap_send(s, 4, r, sizeof(r));
}

/* the sensor's ATT server: the host's GATT client's requests (Core 5.2,
 * Vol 3 Part F): its MTU, the discoveries, reads, its CCCDs written */
static void att_rx(MT7921State *s, const uint8_t *q, int n)
{
    static const uint8_t base[12] = {       /* (16-bit UUIDs' base, the */
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, /* first twelve bytes) */
        0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
    };
    uint8_t r[256], v[32];
    int mtu = s->att_mtu, m, l = 0, h, vl;
    uint16_t a = n >= 3 ? lduw_le_p(q + 1) : 0;
    uint16_t b = n >= 5 ? lduw_le_p(q + 3) : 0;
    uint16_t type = n >= 7 ? lduw_le_p(q + 5) : 0;

    if (n < 1) {
        return;
    }
    switch (q[0]) {
    case 0x02:                                  /* Exchange MTU */
        if (n < 3) {
            break;
        }
        s->att_mtu = MAX(23, MIN(a, 247));
        r[0] = 0x03;
        stw_le_p(r + 1, 247);
        l2cap_send(s, 4, r, 3);
        return;
    case 0x04:                                  /* Find Information */
        if (n < 5 || !a || a > b) {
            att_error(s, q[0], a, 0x01);        /* Invalid Handle */
            return;
        }
        r[0] = 0x05;
        r[1] = 1;                               /* (16-bit UUIDs) */
        for (h = a, m = 2; h <= MIN(b, GATT_N) && m + 4 <= mtu;
             h++, m += 4) {
            stw_le_p(r + m, h);
            stw_le_p(r + m + 2, gatt_db[h - 1].type);
        }
        if (m == 2) {
            att_error(s, q[0], a, 0x0a);        /* Attribute Not Found */
            return;
        }
        l2cap_send(s, 4, r, m);
        return;
    case 0x06:                                  /* Find By Type Value */
        if (n < 7 || !a || a > b) {
            att_error(s, q[0], a, 0x01);
            return;
        }
        r[0] = 0x07;
        for (h = a, m = 1; h <= MIN(b, GATT_N) && m + 4 <= mtu; h++) {
            if (type == 0x2800 && gatt_db[h - 1].type == 0x2800 &&
                n == 9 && !memcmp(q + 7, gatt_db[h - 1].val, 2)) {
                stw_le_p(r + m, h);
                stw_le_p(r + m + 2, gatt_group_end(h));
                m += 4;
            }
        }
        if (m == 1) {
            att_error(s, q[0], a, 0x0a);
            return;
        }
        l2cap_send(s, 4, r, m);
        return;
    case 0x08:                                  /* Read By Type */
    case 0x10:                                  /* Read By Group Type */
        if ((n != 7 && n != 21) || !a || a > b) {
            att_error(s, q[0], a, 0x01);
            return;
        }
        if (n == 21) {                          /* (128 bits: ours, or none) */
            type = memcmp(q + 5, base, 12) || lduw_le_p(q + 19) ? 0 :
                   lduw_le_p(q + 17);
        }
        if (q[0] == 0x10 && type != 0x2800 && type != 0x2801) {
            att_error(s, q[0], a, 0x10);        /* Unsupported Group Type */
            return;
        }
        r[0] = q[0] + 1;
        for (h = a, m = 2; h <= MIN(b, GATT_N); h++) {
            int il;

            if (gatt_db[h - 1].type != type) {
                continue;
            }
            if (!(gatt_db[h - 1].flags & GATT_R)) {
                if (!l) {
                    att_error(s, q[0], h, 0x02);    /* Read Not Permitted */
                    return;
                }
                break;
            }
            vl = gatt_value(s, h, v);
            il = q[0] == 0x10 ? 4 + vl : 2 + MIN(vl, mtu - 4);
            if ((l && il != l) || m + il > mtu) {
                break;
            }
            l = il;
            stw_le_p(r + m, h);
            if (q[0] == 0x10) {
                stw_le_p(r + m + 2, gatt_group_end(h));
                memcpy(r + m + 4, v, vl);
            } else {
                memcpy(r + m + 2, v, il - 2);
            }
            m += il;
        }
        if (!l) {
            att_error(s, q[0], a, 0x0a);
            return;
        }
        r[1] = l;
        l2cap_send(s, 4, r, m);
        return;
    case 0x0a:                                  /* Read */
    case 0x0c:                                  /* Read Blob */
        if (n < (q[0] == 0x0a ? 3 : 5) || !a || a > GATT_N) {
            att_error(s, q[0], a, 0x01);
            return;
        }
        if (!(gatt_db[a - 1].flags & GATT_R)) {
            att_error(s, q[0], a, 0x02);
            return;
        }
        vl = gatt_value(s, a, v);
        m = q[0] == 0x0c ? b : 0;               /* (the offset) */
        if (m > vl) {
            att_error(s, q[0], a, 0x07);        /* Invalid Offset */
            return;
        }
        r[0] = q[0] + 1;
        l = MIN(vl - m, mtu - 1);
        memcpy(r + 1, v + m, l);
        l2cap_send(s, 4, r, 1 + l);
        return;
    case 0x12:                                  /* Write Request */
    case 0x52:                                  /* Write Command */
        if (n == 5 && a && a <= GATT_N && gatt_cccd(a) >= 0) {
            s->cccd[gatt_cccd(a)] = q[3];
            if (a == 17 && (q[3] & 1)) {        /* heart rate, each second */
                timer_mod(s->hr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          NANOSECONDS_PER_SECOND);
            } else if (a == 17) {
                timer_del(s->hr_timer);
            }
            if (q[0] == 0x12) {
                r[0] = 0x13;
                l2cap_send(s, 4, r, 1);
            }
        } else if (q[0] == 0x12) {
            att_error(s, q[0], a, a && a <= GATT_N ? 0x03 : 0x01);
        }                                       /* (Write Not Permitted) */
        return;
    case 0x1e:                                  /* Handle Value
                                                   Confirmation */
        return;
    }
    if (!(q[0] & 0x40)) {                       /* (a request: commands are
                                                   dropped) */
        att_error(s, q[0], 0, 0x06);            /* Request Not Supported */
    }
}

/* the sensor's heart rate, notified each second while subscribed */
static void hr_tick(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t r[5] = { 0x1b, 16, 0 };             /* Handle Value Notification */

    if (!s->le_conn || !(s->cccd[2] & 1)) {
        return;
    }
    s->hr_beat++;
    gatt_value(s, 16, r + 3);
    l2cap_send(s, 4, r, sizeof(r));
    timer_mod(s->hr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND);
}

/* an L2CAP PDU from the host on the sensor's connection: ATT's; SMP's,
 * whose pairing it refuses; the LE signalling channel's, whose requests
 * it rejects (it has no credit-based channels) */
static void l2cap_rx(MT7921State *s, uint16_t cid, const uint8_t *q, int n)
{
    uint8_t r[6];

    if (cid == 4) {
        att_rx(s, q, n);
    } else if (cid == 6 && n >= 1 && q[0] == 0x01) {    /* Pairing Request */
        r[0] = 0x05;                            /* Pairing Failed: */
        r[1] = 0x05;                            /* Pairing Not Supported */
        l2cap_send(s, 6, r, 2);
    } else if (cid == 5 && n >= 4 &&
               (q[0] == 0x06 || q[0] == 0x0a || q[0] == 0x12 ||
                q[0] == 0x14 || q[0] == 0x17)) {
        r[0] = 0x01;                            /* Command Reject: */
        r[1] = q[1];
        stw_le_p(r + 2, 2);
        stw_le_p(r + 4, 0);                     /* not understood */
        l2cap_send(s, 5, r, 6);
    }
}

static void br_acl_rx(MT7921State *s, uint16_t hdr, const uint8_t *q,
                      int len);
#define BR_HANDLE       0x0041          /* the speaker's connection */

/* an ACL packet from the host (bulk 0x02): its buffer back at once (Number
 * Of Completed Packets), and on the sensor's connection its data an L2CAP
 * PDU's, put together (on the speaker's: br_acl_rx) */
static void acl_rx(MT7921State *s, const uint8_t *q, int n)
{
    uint8_t e[5] = { 1 };
    uint16_t hdr, len;

    if (n < 4) {
        return;
    }
    hdr = lduw_le_p(q);
    len = MIN(lduw_le_p(q + 2), n - 4);
    stw_le_p(e + 1, hdr & 0xfff);
    stw_le_p(e + 3, 1);
    ev_put(s, 0x13, e, sizeof(e));
    if ((hdr & 0xfff) == BR_HANDLE && s->br_conn) {
        br_acl_rx(s, hdr, q + 4, len);
        return;
    }
    if ((hdr & 0xfff) != LE_HANDLE || !s->le_conn) {
        return;
    }
    if ((hdr >> 12 & 3) != 1) {                 /* (not a continuation) */
        s->l2rx_len = 0;
    }
    if (s->l2rx_len + len > sizeof(s->l2rx)) {
        s->l2rx_len = 0;
        return;
    }
    memcpy(s->l2rx + s->l2rx_len, q + 4, len);
    s->l2rx_len += len;
    if (s->l2rx_len >= 4 && s->l2rx_len >= 4 + lduw_le_p(s->l2rx)) {
        l2cap_rx(s, lduw_le_p(s->l2rx + 2), s->l2rx + 4,
                 lduw_le_p(s->l2rx));
        s->l2rx_len = 0;
    }
}

/*
 * LE Connection Complete, or the Enhanced one when the host's LE event mask
 * asks for it (bit 9: Linux does when the controller has LL privacy, and
 * waits for that one alone): @status, the sensor's connection (the
 * controller central, the parameters asked for), or a failed attempt's.
 */
static void le_conn_complete(MT7921State *s, uint8_t status)
{
    bool enh = s->le_evmask[1] & 0x02;
    uint8_t e[31] = { enh ? 0x0a : 0x01, status };
    int o = enh ? 12 : 0;                       /* (two RPAs, none) */

    if (!status) {
        stw_le_p(e + 2, LE_HANDLE);
        e[4] = 0x00;                            /* (central) */
        e[5] = 0x01;                            /* (a random address) */
        memcpy(e + 6, sensor_addr, 6);
        stw_le_p(e + 12 + o, s->le_conn_params[0]);
        stw_le_p(e + 14 + o, s->le_conn_params[1]);
        stw_le_p(e + 16 + o, s->le_conn_params[2]);
    }
    ev_put(s, 0x3e, e, 19 + o);
}

/* the sensor's connection made */
static void le_conn_done(void *opaque)
{
    MT7921State *s = opaque;

    s->le_connecting = false;
    s->le_conn = true;
    s->att_mtu = 23;
    s->l2rx_len = 0;
    memset(s->cccd, 0, sizeof(s->cccd));
    le_conn_complete(s, 0);
}

/* a command on the sensor's connection, its Command Status given: the
 * event that ends it */
static void le_conn_cmd(MT7921State *s, uint16_t op, const uint8_t *p,
                        int plen)
{
    uint8_t e[12] = { 0 };

    stw_le_p(e + 1, LE_HANDLE);
    switch (op) {
    case 0x0406:                                /* Disconnection Complete */
        e[3] = 0x16;                            /* (by the local host) */
        le_conn_drop(s);
        ev_put(s, 0x05, e, 4);
        break;
    case 0x041d:                                /* Read Remote Version
                                                   Information Complete */
        e[3] = 0x0b;                            /* (5.2) */
        stw_le_p(e + 4, 0xffff);                /* (no company's) */
        stw_le_p(e + 6, 0x0001);
        ev_put(s, 0x0c, e, 8);
        break;
    case 0x2013:                                /* LE Connection Update
                                                   Complete */
        if (plen >= 10) {
            s->le_conn_params[0] = lduw_le_p(p + 4);
            s->le_conn_params[1] = lduw_le_p(p + 6);
            s->le_conn_params[2] = lduw_le_p(p + 8);
        }
        e[0] = 0x03;
        e[1] = 0;
        stw_le_p(e + 2, LE_HANDLE);
        stw_le_p(e + 4, s->le_conn_params[0]);
        stw_le_p(e + 6, s->le_conn_params[1]);
        stw_le_p(e + 8, s->le_conn_params[2]);
        ev_put(s, 0x3e, e, 10);
        break;
    case 0x2016:                                /* LE Read Remote Features
                                                   Complete: none (no
                                                   encryption, no pairing) */
        e[0] = 0x04;
        e[1] = 0;
        stw_le_p(e + 2, LE_HANDLE);
        ev_put(s, 0x3e, e, 12);
        break;
    }
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

/* ------------------------------------- the speaker near it (BR/EDR) */

/*
 * "FlipperOneEmu Speaker", C0:FE:46:4F:42:03: a BR/EDR device an inquiry
 * finds (its name and services in its extended inquiry response), which
 * answers a page. Its connection pairs with SSP, Just Works (it has no
 * input or output, and no Secure Connections): a link key the host keeps,
 * which it takes back later as one it remembers. Its L2CAP has the
 * signalling channel, SDP (one record, an A2DP sink's) and AVDTP's two
 * channels: A2DP's sink, SBC (avdtp_sig_rx). What it plays goes to the
 * "speaker" file as raw SBC, which ffmpeg plays (-f sbc); AVRCP and other
 * profiles' channels it refuses.
 */
#define SDP_CID         0x0040          /* its end of an SDP channel */
#define AVDTP_SIG_CID   0x0041          /* ... of AVDTP's signalling */
#define AVDTP_MEDIA_CID 0x0042          /* ... of its media channel */
#define SPEAKER_NAME    "FlipperOneEmu Speaker"

static const uint8_t speaker_addr[6] = { 0x03, 0x42, 0x4f, 0x46, 0xfe, 0xc0 };
/* its class: audio/video, a loudspeaker; the rendering and audio services */
static const uint8_t speaker_cod[3] = { 0x14, 0x04, 0x24 };

static bool is_speaker(const uint8_t *addr)
{
    return !memcmp(addr, speaker_addr, 6);
}

/* an inquiry finding it, 0.3 s in: its result as the host's inquiry mode
 * asks (2: extended, its name and its services' UUIDs: A2DP's sink, AVRCP) */
static void inq_result(void *opaque)
{
    static const uint8_t eir_tail[] = { 5, 0x03, 0x0b, 0x11, 0x0e, 0x11,
                                        2, 0x0a, 0x04 };
    MT7921State *s = opaque;
    uint8_t e[255] = { 1 };                     /* one response */
    int n = sizeof(SPEAKER_NAME) - 1;

    if (!timer_pending(s->inquiry)) {
        return;
    }
    memcpy(e + 1, speaker_addr, 6);
    e[7] = 0x01;                                /* page scan: R1 */
    if (s->inquiry_mode == 0) {                 /* Inquiry Result */
        memcpy(e + 10, speaker_cod, 3);
        stw_le_p(e + 13, 0x1234);               /* (its clock offset) */
        ev_put(s, 0x02, e, 15);
        return;
    }
    memcpy(e + 9, speaker_cod, 3);
    stw_le_p(e + 12, 0x1234);
    e[14] = (uint8_t)-50;                       /* RSSI, dBm */
    if (s->inquiry_mode == 1) {                 /* ... with RSSI */
        ev_put(s, 0x22, e, 15);
        return;
    }
    e[15] = n + 1;                              /* Extended Inquiry Result */
    e[16] = 0x09;                               /* (its name, complete) */
    memcpy(e + 17, SPEAKER_NAME, n);
    memcpy(e + 17 + n, eir_tail, sizeof(eir_tail));
    ev_put(s, 0x2f, e, 255);
}

/* a page of it answered: its connection, or its name */
static void br_page_done(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t e[255] = { 0 };

    if (s->br_page_op == 0x05) {                /* Connection Complete */
        stw_le_p(e + 1, BR_HANDLE);
        memcpy(e + 3, speaker_addr, 6);
        e[9] = 0x01;                            /* ACL, not encrypted */
        s->br_conn = true;
        s->br_l2rx_len = 0;
        s->sdp_cid = 0;
        s->avdtp_cid[0] = s->avdtp_cid[1] = 0;
        ev_put(s, 0x03, e, 11);
    } else if (s->br_page_op == 0x19) {         /* Remote Name Request
                                                   Complete */
        memcpy(e + 1, speaker_addr, 6);
        memcpy(e + 7, SPEAKER_NAME, sizeof(SPEAKER_NAME) - 1);
        ev_put(s, 0x07, e, 255);
    }
    s->br_page_op = 0;
}

static void br_conn_drop(MT7921State *s)
{
    s->br_conn = false;
    s->br_l2rx_len = 0;
    s->sdp_cid = 0;
    s->avdtp_cid[0] = s->avdtp_cid[1] = 0;
}

/* an L2CAP PDU to the host on the speaker's connection: one ACL packet */
static void br_l2cap_send(MT7921State *s, uint16_t cid, const uint8_t *q,
                          int n)
{
    GByteArray *a = g_byte_array_sized_new(8 + n);
    uint8_t h[8];

    stw_le_p(h, BR_HANDLE | 0x2000);            /* (PB: a start, flushable) */
    stw_le_p(h + 2, 4 + n);
    stw_le_p(h + 4, n);
    stw_le_p(h + 6, cid);
    g_byte_array_append(a, h, 8);
    g_byte_array_append(a, q, n);
    g_queue_push_tail(&s->aclq, a);
    usb_wakeup(s->acl_in, 0);
}

/* a signalling command to the host: @code, @ident, its data */
static void br_sig(MT7921State *s, uint8_t code, uint8_t ident,
                   const uint8_t *d, int n)
{
    uint8_t c[64];

    c[0] = code;
    c[1] = ident;
    stw_le_p(c + 2, n);
    if (n) {
        memcpy(c + 4, d, n);
    }
    br_l2cap_send(s, 1, c, 4 + n);
}

/*
 * Its SDP record, an A2DP sink's, as SDP's data elements: its handle; its
 * class (Audio Sink); its protocols (L2CAP, PSM 0x19; AVDTP 1.3); the
 * public browse group; its profile (A2DP 1.3); its name; its features (a
 * speaker)
 */
static const uint8_t sdp_record[] = {
    0x35, 0x4f,
    0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x01,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0b,
    0x09, 0x00, 0x04, 0x35, 0x10,
        0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
        0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03,
    0x09, 0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02,
    0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d,
        0x09, 0x01, 0x03,
    0x09, 0x01, 0x00, 0x25, 0x0a, 'A', 'u', 'd', 'i', 'o', ' ',
        'S', 'i', 'n', 'k',
    0x09, 0x03, 0x11, 0x09, 0x00, 0x02,
};
/* the UUIDs a search finds it by */
static const uint16_t sdp_uuids[] = {
    0x1002, 0x0100, 0x0019, 0x110b, 0x110d, 0x110a,
};

/* does an SDP search pattern (a sequence of UUIDs, at @p) find the record? */
static bool sdp_matches(const uint8_t *p, int n)
{
    int i = p[0] == 0x36 ? 3 : 2;

    if (n < 3 || (p[0] != 0x35 && p[0] != 0x36)) {
        return false;
    }
    n = MIN(n, p[0] == 0x35 ? 2 + p[1] : 3 + lduw_be_p(p + 1));
    while (i + 3 <= n) {
        if (p[i] == 0x19) {                     /* a 16-bit UUID */
            uint16_t u = lduw_be_p(p + i + 1);

            for (int k = 0; k < ARRAY_SIZE(sdp_uuids); k++) {
                if (u == sdp_uuids[k]) {
                    return true;
                }
            }
            i += 3;
        } else if (p[i] == 0x1a) {              /* a 32-bit one */
            i += 5;
        } else if (p[i] == 0x1c) {              /* a 128-bit one */
            i += 17;
        } else {
            break;
        }
    }
    return false;
}

/* the length of the data element at @p (its header too), or 0 */
static int sdp_de_len(const uint8_t *p, int n)
{
    int sz = p[0] & 7;

    if (n < 1) {
        return 0;
    }
    if (sz < 5) {
        return p[0] ? 1 + (1 << sz) : 1;
    }
    if (sz == 5 && n >= 2) {
        return 2 + p[1];
    }
    if (sz == 6 && n >= 3) {
        return 3 + lduw_be_p(p + 1);
    }
    return 0;
}

/* an SDP PDU from the host: the search and attribute requests, answered
 * whole (no continuation: the record is small) */
static void sdp_rx(MT7921State *s, const uint8_t *q, int n)
{
    uint8_t r[160];
    int pat, m = 0;
    bool found;

    if (n < 5) {
        return;
    }
    pat = sdp_de_len(q + 5, n - 5);
    found = pat && sdp_matches(q + 5, pat);
    r[0] = q[0] + 1;                            /* (its response) */
    r[1] = q[1];                                /* (its transaction) */
    r[2] = q[2];
    switch (q[0]) {
    case 0x02:                                  /* Service Search */
        stw_be_p(r + 5, found);                 /* (total, current) */
        stw_be_p(r + 7, found);
        m = 9;
        if (found) {
            stl_be_p(r + 9, 0x00010001);
            m += 4;
        }
        r[m++] = 0;                             /* (no continuation) */
        break;
    case 0x04:                                  /* Service Attribute */
        if (n < 9 || ldl_be_p(q + 5) != 0x00010001) {
            r[0] = 0x01;                        /* Error: */
            stw_be_p(r + 5, 0x0002);            /* invalid record handle */
            m = 7;
            break;
        }
        stw_be_p(r + 5, sizeof(sdp_record));
        memcpy(r + 7, sdp_record, sizeof(sdp_record));
        m = 7 + sizeof(sdp_record);
        r[m++] = 0;
        break;
    case 0x06:                                  /* Service Search Attribute */
        if (found) {
            stw_be_p(r + 5, 2 + sizeof(sdp_record));
            r[7] = 0x35;                        /* (the one record's list) */
            r[8] = sizeof(sdp_record);
            memcpy(r + 9, sdp_record, sizeof(sdp_record));
            m = 9 + sizeof(sdp_record);
        } else {
            stw_be_p(r + 5, 2);
            r[7] = 0x35;                        /* (an empty list) */
            r[8] = 0;
            m = 9;
        }
        r[m++] = 0;
        break;
    default:
        r[0] = 0x01;                            /* Error: invalid syntax */
        stw_be_p(r + 5, 0x0003);
        m = 7;
        break;
    }
    stw_be_p(r + 3, m - 5);
    br_l2cap_send(s, s->sdp_cid, r, m);
}

/* a signalling command from the host on the speaker's connection */
static void br_sig_rx(MT7921State *s, const uint8_t *q, int n)
{
    uint8_t d[16] = { 0 };
    uint8_t code, id;
    uint16_t ours;

    if (n < 4) {
        return;
    }
    code = q[0];
    id = q[1];
    q += 4;
    n -= 4;
    switch (code) {
    case 0x02:                                  /* Connection Request */
        if (n < 4) {
            return;
        }
        stw_le_p(d + 2, lduw_le_p(q + 2));      /* (its source CID) */
        if (lduw_le_p(q) == 0x0001 && !s->sdp_cid) {        /* SDP */
            s->sdp_cid = lduw_le_p(q + 2);
            ours = SDP_CID;
        } else if (lduw_le_p(q) == 0x0019 &&
                   (!s->avdtp_cid[0] || !s->avdtp_cid[1])) {
            int k = s->avdtp_cid[0] ? 1 : 0;    /* AVDTP: its signalling,
                                                   then its media */
            s->avdtp_cid[k] = lduw_le_p(q + 2);
            ours = k ? AVDTP_MEDIA_CID : AVDTP_SIG_CID;
        } else {
            stw_le_p(d + 4, lduw_le_p(q) == 0x0001 || lduw_le_p(q) == 0x0019 ?
                     0x0004 : 0x0002);          /* no resources; PSM not
                                                   supported */
            br_sig(s, 0x03, id, d, 8);
            return;
        }
        stw_le_p(d, ours);
        br_sig(s, 0x03, id, d, 8);              /* success */
        /* its own configuration: its MTU, 1000 (an SBC packet's room) */
        stw_le_p(d, lduw_le_p(q + 2));
        stw_le_p(d + 2, 0);
        d[4] = 0x01;
        d[5] = 2;
        stw_le_p(d + 6, 1000);
        br_sig(s, 0x04, id + 0x40, d, 8);
        break;
    case 0x04:                                  /* Configuration Request */
        if (n < 4) {
            return;
        }
        ours = lduw_le_p(q);                    /* (its channel: ours) */
        stw_le_p(d, ours == AVDTP_SIG_CID ? s->avdtp_cid[0] :
                    ours == AVDTP_MEDIA_CID ? s->avdtp_cid[1] : s->sdp_cid);
        br_sig(s, 0x05, id, d, 6);              /* Configuration Response:
                                                   accepted */
        break;
    case 0x06:                                  /* Disconnection Request */
        if (n < 4) {
            return;
        }
        memcpy(d, q, 4);
        if (lduw_le_p(q) == SDP_CID) {
            s->sdp_cid = 0;
        } else if (lduw_le_p(q) == AVDTP_SIG_CID) {
            s->avdtp_cid[0] = 0;
        } else if (lduw_le_p(q) == AVDTP_MEDIA_CID) {
            s->avdtp_cid[1] = 0;
        }
        br_sig(s, 0x07, id, d, 4);
        break;
    case 0x08:                                  /* Echo Request */
        br_sig(s, 0x09, id, NULL, 0);
        break;
    case 0x0a:                                  /* Information Request */
        if (n < 2) {
            return;
        }
        stw_le_p(d, lduw_le_p(q));
        if (lduw_le_p(q) == 0x0002) {           /* extended features: */
            stl_le_p(d + 4, 0x80);              /* fixed channels */
            br_sig(s, 0x0b, id, d, 8);
        } else if (lduw_le_p(q) == 0x0003) {    /* fixed channels: */
            d[4] = 0x02;                        /* signalling */
            br_sig(s, 0x0b, id, d, 12);
        } else {
            stw_le_p(d + 2, 0x0001);            /* not supported */
            br_sig(s, 0x0b, id, d, 4);
        }
        break;
    case 0x03:                                  /* (Connection Response,
    case 0x05:                                     Configuration Response,
    case 0x07:                                     Disconnection Response:
    case 0x0b:                                     to its own) */
        break;
    default:
        stw_le_p(d, 0);                         /* Command Reject: not
                                                   understood */
        br_sig(s, 0x01, id, d, 2);
        break;
    }
}

/*
 * AVDTP's signalling, the speaker's side (AVDTP 1.3, A2DP's sink): one
 * stream end point, SEID 1, an audio sink taking SBC at any of its rates,
 * modes, blocks, subbands and allocations, bitpools 2 to 53. Each command
 * is accepted: discovery, its capabilities, the configuration the host
 * sets (kept, for GET_CONFIGURATION), open, start, suspend, close, abort,
 * delay reports; any other is rejected (general reject).
 */
static void avdtp_send(MT7921State *s, uint8_t hdr, uint8_t sig,
                       const uint8_t *d, int n)
{
    uint8_t b[32];

    b[0] = hdr;
    b[1] = sig;
    if (n) {
        memcpy(b + 2, d, n);
    }
    br_l2cap_send(s, s->avdtp_cid[0], b, 2 + n);
}

static void avdtp_sig_rx(MT7921State *s, const uint8_t *q, int n)
{
    /* its capabilities: media transport; SBC, everything, bitpool 2-53 */
    static const uint8_t caps[] = { 0x01, 0x00,
                                    0x07, 0x06, 0x00, 0x00,
                                    0xff, 0xff, 2, 53 };
    static const uint8_t sep[] = { 1 << 2, 0x08 };  /* SEID 1, free; audio,
                                                       a sink */
    uint8_t accept, sig;

    if (n < 2 || (q[0] & 0x0f) != 0) {          /* (a command, whole) */
        return;
    }
    accept = (q[0] & 0xf0) | 0x02;              /* (its transaction's) */
    sig = q[1] & 0x3f;
    switch (sig) {
    case 0x01:                                  /* DISCOVER */
        avdtp_send(s, accept, sig, sep, sizeof(sep));
        break;
    case 0x02:                                  /* GET_CAPABILITIES */
    case 0x0c:                                  /* GET_ALL_CAPABILITIES */
        avdtp_send(s, accept, sig, caps, sizeof(caps));
        break;
    case 0x03:                                  /* SET_CONFIGURATION */
        s->avdtp_conf_len = MIN(MAX(n - 4, 0), (int)sizeof(s->avdtp_conf));
        memcpy(s->avdtp_conf, q + 4, s->avdtp_conf_len);
        avdtp_send(s, accept, sig, NULL, 0);
        break;
    case 0x04:                                  /* GET_CONFIGURATION */
        avdtp_send(s, accept, sig, s->avdtp_conf, s->avdtp_conf_len);
        break;
    case 0x05:                                  /* RECONFIGURE */
    case 0x06:                                  /* OPEN */
    case 0x07:                                  /* START */
    case 0x08:                                  /* CLOSE */
    case 0x09:                                  /* SUSPEND */
    case 0x0a:                                  /* ABORT */
    case 0x0d:                                  /* DELAYREPORT */
        avdtp_send(s, accept, sig, NULL, 0);
        break;
    default:
        avdtp_send(s, (q[0] & 0xf0) | 0x01, sig, NULL, 0);  /* (general
                                                               reject) */
        break;
    }
}

/* a media packet: RTP (its CSRCs and extension skipped), SBC's one-byte
 * header, then SBC frames, which go to the "speaker" file as they come */
static void avdtp_media_rx(MT7921State *s, const uint8_t *q, int n)
{
    int h;

    if (n < 13 || q[0] >> 6 != 2) {             /* (RTP, version 2) */
        return;
    }
    h = 12 + 4 * (q[0] & 0x0f);
    if (q[0] & 0x10) {                          /* (an extension) */
        if (n < h + 4) {
            return;
        }
        h += 4 + 4 * lduw_be_p(q + h + 2);
    }
    if (n <= h + 1) {
        return;
    }
    if (s->speaker_fd >= 0 &&
        qemu_write_full(s->speaker_fd, q + h + 1, n - h - 1) != n - h - 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "mt7921: speaker: cannot write\n");
    }
}

/* an ACL packet's data on the speaker's connection: an L2CAP PDU's */
static void br_acl_rx(MT7921State *s, uint16_t hdr, const uint8_t *q,
                      int len)
{
    uint16_t cid;

    if ((hdr >> 12 & 3) != 1) {                 /* (not a continuation) */
        s->br_l2rx_len = 0;
    }
    if (s->br_l2rx_len + len > sizeof(s->br_l2rx)) {
        s->br_l2rx_len = 0;
        return;
    }
    memcpy(s->br_l2rx + s->br_l2rx_len, q, len);
    s->br_l2rx_len += len;
    if (s->br_l2rx_len < 4 || s->br_l2rx_len < 4 + lduw_le_p(s->br_l2rx)) {
        return;
    }
    cid = lduw_le_p(s->br_l2rx + 2);
    if (cid == 1) {
        br_sig_rx(s, s->br_l2rx + 4, lduw_le_p(s->br_l2rx));
    } else if (cid == SDP_CID && s->sdp_cid) {
        sdp_rx(s, s->br_l2rx + 4, lduw_le_p(s->br_l2rx));
    } else if (cid == AVDTP_SIG_CID && s->avdtp_cid[0]) {
        avdtp_sig_rx(s, s->br_l2rx + 4, lduw_le_p(s->br_l2rx));
    } else if (cid == AVDTP_MEDIA_CID && s->avdtp_cid[1]) {
        avdtp_media_rx(s, s->br_l2rx + 4, lduw_le_p(s->br_l2rx));
    }
    s->br_l2rx_len = 0;
}

/*
 * A command on the speaker's connection, or about it: answered, and the
 * events that end it. False: not one of these.
 */
static bool br_command(MT7921State *s, uint16_t op, const uint8_t *p,
                       int plen)
{
    uint8_t e[24] = { 0 };
    bool on_conn = plen >= 2 && (lduw_le_p(p) & 0xfff) == BR_HANDLE &&
                   s->br_conn;
    bool of_it = plen >= 6 && is_speaker(p);

    switch (op) {
    case 0x0405:                                /* Create Connection */
    case 0x0419:                                /* Remote Name Request */
        if (!of_it) {
            return false;
        }
        if (s->br_page_op || (op == 0x0405 && s->br_conn)) {
            ev_cs(s, op, op == 0x0405 && s->br_conn ? 0x0b : 0x0c);
            return true;
        }
        ev_cs(s, op, 0);
        s->br_page_op = op & 0xff;
        timer_mod(s->br_page, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (op == 0x0405 ? 120 : 40) * SCALE_MS);
        return true;
    case 0x0408:                                /* Create Connection Cancel */
    case 0x041a:                                /* Remote Name Request
                                                   Cancel */
        if (!of_it) {
            return false;
        }
        e[0] = s->br_page_op ? 0 : 0x02;
        memcpy(e + 1, speaker_addr, 6);
        ev_cc(s, op, e, 7);
        if (s->br_page_op) {
            timer_del(s->br_page);
            memset(e, 0, sizeof(e));
            e[0] = 0x02;                        /* Unknown Connection ID */
            memcpy(e + (s->br_page_op == 0x05 ? 3 : 1), speaker_addr, 6);
            if (s->br_page_op == 0x05) {
                e[9] = 0x01;
                ev_put(s, 0x03, e, 11);
            } else {
                uint8_t r[255] = { 0x02 };

                memcpy(r + 1, speaker_addr, 6);
                ev_put(s, 0x07, r, 255);
            }
            s->br_page_op = 0;
        }
        return true;
    }
    if (!on_conn) {
        return false;
    }
    stw_le_p(e + 1, BR_HANDLE);
    switch (op) {
    case 0x0406:                                /* Disconnect */
        ev_cs(s, op, 0);
        e[3] = 0x16;                            /* (by the local host) */
        br_conn_drop(s);
        ev_put(s, 0x05, e, 4);                  /* Disconnection Complete */
        break;
    case 0x041b:                                /* Read Remote Features */
        ev_cs(s, op, 0);
        memcpy(e + 3, lmp_features, 8);
        ev_put(s, 0x0b, e, 11);
        break;
    case 0x041c:                                /* ... Extended Features:
                                                   page 1, the host's SSP */
        ev_cs(s, op, 0);
        e[3] = plen > 2 ? p[2] : 0;
        e[4] = 1;
        if (e[3] == 0) {
            memcpy(e + 5, lmp_features, 8);
        } else if (e[3] == 1) {
            e[5] = 0x01;
        }
        ev_put(s, 0x23, e, 13);
        break;
    case 0x041d:                                /* Read Remote Version */
        ev_cs(s, op, 0);
        e[3] = 0x09;                            /* (5.0) */
        stw_le_p(e + 4, 0xffff);
        stw_le_p(e + 6, 0x0001);
        ev_put(s, 0x0c, e, 8);
        break;
    case 0x041f:                                /* Read Clock Offset */
        ev_cs(s, op, 0);
        stw_le_p(e + 3, 0x1234);
        ev_put(s, 0x1c, e, 5);
        break;
    case 0x0411:                                /* Authentication Requested:
                                                   the host's link key? */
        ev_cs(s, op, 0);
        ev_put(s, 0x17, speaker_addr, 6);       /* Link Key Request */
        break;
    case 0x0413:                                /* Set Connection Encryption */
        ev_cs(s, op, 0);
        e[3] = plen > 2 ? p[2] : 1;
        ev_put(s, 0x08, e, 4);                  /* Encryption Change */
        break;
    case 0x0803:                                /* Sniff Mode */
        ev_cs(s, op, 0);
        e[3] = 2;
        stw_le_p(e + 4, plen >= 4 ? lduw_le_p(p + 2) : 0x0800);
        ev_put(s, 0x14, e, 6);                  /* Mode Change */
        break;
    case 0x0804:                                /* Exit Sniff Mode */
        ev_cs(s, op, 0);
        ev_put(s, 0x14, e, 6);
        break;
    case 0x0809:                                /* Role Discovery: central */
        ev_cc(s, op, e, 4);
        break;
    case 0x040f:                                /* Change Connection Packet
                                                   Type: as asked */
        ev_cs(s, op, 0);
        stw_le_p(e + 3, plen >= 4 ? lduw_le_p(p + 2) : 0xcc18);
        ev_put(s, 0x1d, e, 5);
        break;
    case 0x080d:                                /* Write Link Policy */
    case 0x0c37:                                /* Write Link Supervision
                                                   Timeout */
        ev_cc(s, op, e, 3);
        break;
    case 0x1405:                                /* Read RSSI */
        e[3] = (uint8_t)-50;
        ev_cc(s, op, e, 4);
        break;
    case 0x1408:                                /* Read Encryption Key Size */
        e[3] = 16;
        ev_cc(s, op, e, 4);
        break;
    default:
        return false;
    }
    return true;
}

/* SSP's commands, about the speaker: a link key, its pairing */
static bool br_pairing(MT7921State *s, uint16_t op, const uint8_t *p,
                       int plen)
{
    uint8_t e[24] = { 0 };

    if (plen < 6 || !is_speaker(p) || !s->br_conn) {
        return false;
    }
    memcpy(e + 1, speaker_addr, 6);
    switch (op) {
    case 0x040b:                                /* Link Key Request Reply:
                                                   one it remembers */
        ev_cc(s, op, e, 7);
        stw_le_p(e + 1, BR_HANDLE);
        ev_put(s, 0x06, e, 3);                  /* Authentication Complete */
        break;
    case 0x040c:                                /* ... Negative: pairing */
        ev_cc(s, op, e, 7);
        ev_put(s, 0x31, speaker_addr, 6);       /* IO Capability Request */
        break;
    case 0x042b:                                /* IO Capability Request
                                                   Reply */
        ev_cc(s, op, e, 7);
        memcpy(e, speaker_addr, 6);
        e[6] = 0x03;                            /* NoInputNoOutput */
        e[7] = 0;                               /* (no OOB data) */
        e[8] = 0x04;                            /* general bonding, no MITM */
        ev_put(s, 0x32, e, 9);                  /* IO Capability Response */
        stl_le_p(e + 6, 413131);                /* (its number, Just Works:
                                                   never shown) */
        ev_put(s, 0x33, e, 10);                 /* User Confirmation
                                                   Request */
        break;
    case 0x042c:                                /* User Confirmation
                                                   Request Reply */
        ev_cc(s, op, e, 7);
        ev_put(s, 0x36, e, 7);                  /* Simple Pairing Complete */
        memcpy(e, speaker_addr, 6);
        qemu_guest_getrandom_nofail(e + 6, 16);
        e[22] = 0x04;                           /* unauthenticated, P-192 */
        ev_put(s, 0x18, e, 23);                 /* Link Key Notification */
        memset(e, 0, sizeof(e));
        stw_le_p(e + 1, BR_HANDLE);
        ev_put(s, 0x06, e, 3);                  /* Authentication Complete */
        break;
    case 0x042d:                                /* ... Negative */
    case 0x0434:                                /* IO Capability Request
                                                   Negative Reply */
        ev_cc(s, op, e, 7);
        e[0] = 0x05;                            /* Authentication Failure */
        ev_put(s, 0x36, e, 7);
        e[0] = 0x05;
        stw_le_p(e + 1, BR_HANDLE);
        ev_put(s, 0x06, e, 3);
        break;
    default:
        return false;
    }
    return true;
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

    if (br_command(s, op, p, plen) || br_pairing(s, op, p, plen)) {
        return;                                 /* (the speaker's) */
    }
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
        timer_mod(s->inq_result, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  300 * SCALE_MS);              /* (the speaker found) */
        break;
    case 0x0402:                                /* Inquiry Cancel */
        timer_del(s->inquiry);
        timer_del(s->inq_result);
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
        if (plen < 2 || (lduw_le_p(p) & 0xfff) != LE_HANDLE ||
            !s->le_conn || op == 0x041b || op == 0x041c) {
            ev_cs(s, op, 0x02);                 /* (no connection: but the
                                                   sensor's, LE) */
            break;
        }
        ev_cs(s, op, 0);
        le_conn_cmd(s, op, p, plen);
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
        s->le_connecting = true;                /* (until made, or
                                                   cancelled) */
        ev_cs(s, op, 0);
        /* the sensor: asked for, or in the accept list asked to use */
        if (plen >= 21 && !s->le_conn &&
            (p[4] ? s->le_accept_sensor : !memcmp(p + 6, sensor_addr, 6))) {
            s->le_conn_params[0] = lduw_le_p(p + 15);   /* (the longest) */
            s->le_conn_params[1] = lduw_le_p(p + 17);
            s->le_conn_params[2] = lduw_le_p(p + 19);
            timer_mod(s->le_conn_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 30 * SCALE_MS);
        }
        break;
    case 0x200e:                                /* LE Create Connection
                                                   Cancel */
        if (!s->le_connecting) {
            ev_cc_status(s, op, 0x0c);
            break;
        }
        s->le_connecting = false;
        timer_del(s->le_conn_timer);
        ev_cc_status(s, op, 0);
        le_conn_complete(s, 0x02);              /* (Unknown Connection ID) */
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
    case 0x200c:                                /* LE Set Scan Enable: the
                                                   beacon in it */
        if (plen && p[0]) {
            s->le_scan_dup = plen > 1 && p[1];
            timer_mod(s->beacon_ble, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      200 * SCALE_MS);
        } else {
            timer_del(s->beacon_ble);
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x200b:                                /* LE Set Scan Parameters */
        s->le_scan_active = plen && p[0] == 0x01;
        ev_cc_status(s, op, 0);
        break;
    case 0x2010:                                /* LE Clear Filter Accept
                                                   List */
        s->le_accept_sensor = false;
        ev_cc_status(s, op, 0);
        break;
    case 0x2011:                                /* LE Add Device To ... */
    case 0x2012:                                /* LE Remove Device From ... */
        if (plen >= 7 && !memcmp(p + 1, sensor_addr, 6)) {
            s->le_accept_sensor = op == 0x2011;
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x2001:                                /* LE Set Event Mask */
        if (plen >= 8) {
            memcpy(s->le_evmask, p, 8);
        }
        ev_cc_status(s, op, 0);
        break;
    case 0x2005:                                /* LE Set Random Address */
    case 0x2006:                                /* LE Set Advertising
                                                   Parameters */
    case 0x2008:                                /* LE Set Advertising Data */
    case 0x2009:                                /* LE Set Scan Response Data */
    case 0x200a:                                /* LE Set Advertising Enable */
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
        if (a >= 0x820d8000 && a < 0x820d8000 + 20 * 0x100 &&
            (a & 0xff) == 0x78) {               /* a WTBL's DW30: the ack's */
            return 0x8282;                      /* RCPI (-42 dBm) */
        }
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

#define WIFI_Q_MAX      256                 /* transfers waiting, at most */
#define SCAN_NS         (1500 * SCALE_MS)   /* a scan's length */

/* the IN endpoint the MCU's events go to: 0x84 once WFDMA's HOST_CONFIG
 * says so (USB_RXEVT_EP4_EN), else 0x85 */
static int wifi_event_ep(MT7921State *s)
{
    return reg_get(s, 0x7c027030) & BIT(6) ? 4 : 5;
}

/* a transfer for IN 0x84 (@q 0) or 0x85 (1): a header, a body */
static void wifi_in(MT7921State *s, int q, const void *h, int hn,
                    const void *b, int bn)
{
    GByteArray *e;

    if (g_queue_get_length(&s->wq[q]) >= WIFI_Q_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "mt7921: Wi-Fi transfers not taken: "
                      "one dropped\n");
        return;
    }
    e = g_byte_array_sized_new(hn + bn);
    g_byte_array_append(e, h, hn);
    if (bn) {
        g_byte_array_append(e, b, bn);
    }
    g_queue_push_tail(&s->wq[q], e);
    usb_wakeup(usb_ep_get(USB_DEVICE(s), USB_TOKEN_IN, q ? 5 : 4), 0);
}

/* an event: its RXD (type 7, the exact length) and header (@28 eid, @29
 * the command's seq, @30 its option, @32 ext_eid, or a status), its
 * payload at @36 */
static void wifi_event_opt(MT7921State *s, uint8_t eid, uint8_t seq,
                           uint8_t option, uint8_t b32, const void *pl, int n)
{
    uint8_t h[36] = { 0 };

    stl_le_p(h, 0x38000000 | (36 + n));
    stw_le_p(h + 24, 36 + n - 24);
    h[28] = eid;
    h[29] = seq;
    h[30] = option;
    h[32] = b32;
    wifi_in(s, wifi_event_ep(s) == 4 ? 0 : 1, h, 36, pl, n);
}

static void wifi_event(MT7921State *s, uint8_t eid, uint8_t seq, uint8_t b32,
                       const void *pl, int n)
{
    wifi_event_opt(s, eid, seq, 0, b32, pl, n);
}

static void wifi_flush(MT7921State *s)
{
    GByteArray *e;

    for (int q = 0; q < 2; q++) {
        while ((e = g_queue_pop_head(&s->wq[q]))) {
            g_byte_array_free(e, true);
        }
    }
}

/* the WFSYS reset (a pulse on WF_SUBSYS_RST): its MCU back in its ROM */
static void wifi_reset(MT7921State *s)
{
    s->wifi_patched = false;
    s->conn_on_misc = 1;                        /* (powered, nothing on) */
    wifi_flush(s);
    timer_del(s->scan);
    timer_del(s->beacon);
    timer_del(s->ap_reply);
    timer_del(s->eapol);
    s->sta_state = AP_NONE;
    s->sta_secure = false;
}

static void scan_done(void *opaque)
{
    MT7921State *s = opaque;
    uint8_t done[476] = { 0 };                  /* (no network found) */

    wifi_event(s, 0x0d, 0, 0, done, sizeof(done));      /* SCAN_DONE */
}

/* ------------------------------------------------- the access point */

static bool ap_on(MT7921State *s)
{
    return s->nic && qemu_get_queue(s->nic)->peer;
}

/* its networks: 0, open; 1, WPA2 (with a passphrase) */
static int ap_nets(MT7921State *s)
{
    return s->ap_psk && *s->ap_psk ? 2 : 1;
}

static const uint8_t *ap_bssid(MT7921State *s, int i)
{
    return i ? s->bssid2 : s->bssid;
}

/* the network a frame is addressed to (addr1), or -1 */
static int ap_net_of(MT7921State *s, const uint8_t *a)
{
    for (int i = 0; i < ap_nets(s); i++) {
        if (!memcmp(a, ap_bssid(s, i), 6)) {
            return i;
        }
    }
    return -1;
}

/* the station's frames may pass to the uplink, and back: associated, and
 * on the WPA2 network its handshake done */
static bool ap_port_open(MT7921State *s)
{
    return s->sta_state == AP_ASSOC && (s->sta_bss == 0 || s->sta_secure);
}

/* ------------------------------------------------- WPA2's cryptography */

static void hmac_sha1(const uint8_t *key, int kn, const uint8_t *a, int an,
                      const uint8_t *b, int bn, uint8_t out[20])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA1, key, kn);
    gsize l = 20;

    g_hmac_update(h, a, an);
    if (bn) {
        g_hmac_update(h, b, bn);
    }
    g_hmac_get_digest(h, out, &l);
    g_hmac_unref(h);
}

/* PBKDF2 with HMAC-SHA1: WPA2's PMK, from the passphrase and the SSID */
static void pbkdf2_sha1(const char *pass, const char *salt, int iters,
                        uint8_t *out, int on)
{
    int pn = strlen(pass), sn = strlen(salt);
    uint8_t u[20], x[20], blk[4];

    for (int b = 1, pos = 0; pos < on; b++, pos += 20) {
        stl_be_p(blk, b);
        hmac_sha1((const uint8_t *)pass, pn, (const uint8_t *)salt, sn,
                  blk, 4, u);
        memcpy(x, u, 20);
        for (int i = 1; i < iters; i++) {
            hmac_sha1((const uint8_t *)pass, pn, u, 20, NULL, 0, u);
            for (int j = 0; j < 20; j++) {
                x[j] ^= u[j];
            }
        }
        memcpy(out + pos, x, MIN(20, on - pos));
    }
}

/* the PRF (IEEE 802.11's, HMAC-SHA1): @label, 0, @data, a counter */
static void wpa_prf(const uint8_t *key, const char *label,
                    const uint8_t *data, int dn, uint8_t *out, int on)
{
    uint8_t m[128], d[20];
    int ln = strlen(label) + 1;                 /* (its 0 too) */

    memcpy(m, label, ln);
    memcpy(m + ln, data, dn);
    for (int i = 0, pos = 0; pos < on; i++, pos += 20) {
        m[ln + dn] = i;
        hmac_sha1(key, 32, m, ln + dn + 1, NULL, 0, d);
        memcpy(out + pos, d, MIN(20, on - pos));
    }
}

/* AES key wrap (RFC 3394) of @n bytes (a multiple of 8) with @kek */
static void aes_wrap(const uint8_t *kek, const uint8_t *in, int n,
                     uint8_t *out)
{
    AES_KEY k;
    uint8_t a[8], b[16], c[16];
    int blocks = n / 8;

    AES_set_encrypt_key(kek, 128, &k);
    memset(a, 0xa6, 8);
    memcpy(out + 8, in, n);
    for (int j = 0; j < 6; j++) {
        for (int i = 1; i <= blocks; i++) {
            uint64_t tt = (uint64_t)blocks * j + i;

            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            AES_encrypt(b, c, &k);
            for (int x = 0; x < 8; x++) {
                a[x] = c[x] ^ (tt >> (8 * (7 - x)));
            }
            memcpy(out + 8 * i, c + 8, 8);
        }
    }
    memcpy(out, a, 8);
}

/* the rates, as P-RXV word 0 and TX status word 0 have them: OFDM 6
 * Mbit/s (mode 1, index 0xb); HT MCS 15 (mode 2: two streams, NSTS 1,
 * 20 MHz) with the short guard interval (RXV only; mt76 keeps its TX
 * rate's from what it set) */
#define RXV_OFDM6       0x0100000b
#define RXV_HT_MCS15    (0x02000000 | 1 << 15 | 1 << 7 | 15)
#define TXS_OFDM6       0x004b
#define TXS_HT_MCS15    (1 << 10 | 2 << 6 | 15)

/* a frame as the chip receives it, on 0x84: its RXD (type 2, group 3: the
 * rate, and RCPI 0x82 on two chains: -42 dBm), the AP's channel,
 * @addr_type (1: unicast to the station, 2: multicast, 3: broadcast). Data
 * to the station comes at HT MCS 15, the rest at 6 Mbit/s */
static void ap_rx_sec(MT7921State *s, const uint8_t *f, int n, int addr_type,
                      bool decrypted)
{
    uint8_t h[32] = { 0 };
    bool data = n >= 2 && (f[0] & 0x0c) == 0x08;

    stl_le_p(h, 0x10000000 | (32 + n));
    stl_le_p(h + 4, BIT(13) | (decrypted ? 4 << 16 : 0));  /* (CCMP) */
    stl_le_p(h + 12, s->ap_chan << 8 | addr_type << 16);
    stl_le_p(h + 24, data && addr_type == 1 ? RXV_HT_MCS15 : RXV_OFDM6);
    stl_le_p(h + 28, 0x8282);
    wifi_in(s, 0, h, 32, f, n);
}

static void ap_rx(MT7921State *s, const uint8_t *f, int n, int addr_type)
{
    ap_rx_sec(s, f, n, addr_type, false);
}

static int ap_hdr(MT7921State *s, uint8_t *f, uint16_t fc, const uint8_t *a1,
                  const uint8_t *a2, const uint8_t *a3)
{
    stw_le_p(f, fc);
    stw_le_p(f + 2, 0);
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, a2, 6);
    memcpy(f + 16, a3, 6);
    stw_le_p(f + 22, (s->ap_seq++ & 0xfff) << 4);
    return 24;
}

/* its rates: 1, 2, 5.5, 11 (basic), 6, 9, 12, 18; then 24, 36, 48, 54 */
static const uint8_t ap_rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96,
                                    0x0c, 0x12, 0x18, 0x24 };
static const uint8_t ap_xrates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6c };
static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* WPA2's RSN element: CCMP for groups and pairs, PSK */
static const uint8_t rsn_ie[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4,
                                  1, 0, 0x00, 0x0f, 0xac, 4,
                                  1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };

/* HT capabilities: LDPC, SM power save off, short GI at 20 MHz, TX STBC
 * and one stream of RX STBC, 20 MHz only; A-MPDUs to 64 KiB; MCS 0-15
 * both ways */
static const uint8_t ht_cap_ie[] = { 45, 26, 0xad, 0x01, 0x03,
                                     0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0x01, 0, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0 };

/* WMM's parameters (the usual: best effort, background, video, voice) */
static const uint8_t wmm_ie[] = { 221, 24, 0x00, 0x50, 0xf2, 2, 1, 1, 0, 0,
                                  0x03, 0xa4, 0x00, 0x00,
                                  0x27, 0xa4, 0x00, 0x00,
                                  0x42, 0x43, 0x5e, 0x00,
                                  0x62, 0x32, 0x2f, 0x00 };

/* HT capabilities, HT operation (the AP's channel, 20 MHz, no
 * protection) and WMM: what an 802.11n AP adds, at @f */
static int ap_ht_ies(MT7921State *s, uint8_t *f)
{
    int n = 0;

    memcpy(f, ht_cap_ie, sizeof(ht_cap_ie));
    n += sizeof(ht_cap_ie);
    f[n++] = 61;
    f[n++] = 22;
    f[n++] = s->ap_chan;
    memset(f + n, 0, 21);
    n += 21;
    memcpy(f + n, wmm_ie, sizeof(wmm_ie));
    return n + sizeof(wmm_ie);
}

static void ap_beacon_of(MT7921State *s, int net)
{
    const char *ssid = net ? s->ssid2 : s->ap_ssid;
    uint8_t f[256];
    int n, l = MIN(strlen(ssid), 32);

    n = ap_hdr(s, f, 0x0080, bcast, ap_bssid(s, net), ap_bssid(s, net));
    stq_le_p(f + n, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    stw_le_p(f + n + 8, 100);                   /* every 100 TU */
    stw_le_p(f + n + 10, net ? 0x0431 : 0x0421);    /* ESS, (privacy,) short
                                                       preamble and slot */
    n += 12;
    f[n++] = 0;                                 /* its SSID */
    f[n++] = l;
    memcpy(f + n, ssid, l);
    n += l;
    memcpy(f + n, ap_rates, sizeof(ap_rates));
    n += sizeof(ap_rates);
    f[n++] = 3;                                 /* its channel */
    f[n++] = 1;
    f[n++] = s->ap_chan;
    f[n++] = 5;                                 /* TIM: none buffered */
    f[n++] = 4;
    f[n++] = 0, f[n++] = 1, f[n++] = 0, f[n++] = 0;
    f[n++] = 42;                                /* ERP: nothing to protect */
    f[n++] = 1;
    f[n++] = 0;
    if (net) {
        memcpy(f + n, rsn_ie, sizeof(rsn_ie));
        n += sizeof(rsn_ie);
    }
    memcpy(f + n, ap_xrates, sizeof(ap_xrates));
    n += sizeof(ap_xrates);
    n += ap_ht_ies(s, f + n);
    ap_rx(s, f, n, 3);
}

static void ap_beacon(void *opaque)
{
    MT7921State *s = opaque;

    for (int i = 0; i < ap_nets(s); i++) {
        ap_beacon_of(s, i);
    }
}

/* an EAPOL-Key message to the station (RSN, version 2: HMAC-SHA1 MICs,
 * AES-wrapped key data): @info, the replay counter, the ANonce, @kd */
static void eapol_send(MT7921State *s, uint16_t info, const uint8_t *kd,
                       int kn, bool mic)
{
    static const uint8_t snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
    uint8_t f[400], d[20], *e, *k;
    int n;

    n = ap_hdr(s, f, 0x0208, s->sta, s->bssid2, s->bssid2);
    memcpy(f + n, snap, 8);
    e = f + n + 8;
    memset(e, 0, 4 + 95 + kn);
    e[0] = 2;                                   /* 802.1X-2004, Key */
    e[1] = 3;
    stw_be_p(e + 2, 95 + kn);
    k = e + 4;
    k[0] = 2;                                   /* RSN */
    stw_be_p(k + 1, info);
    stw_be_p(k + 3, 16);                        /* (CCMP's key) */
    stq_be_p(k + 5, ++s->replay);
    memcpy(k + 13, s->anonce, 32);
    stw_be_p(k + 93, kn);
    memcpy(k + 95, kd, kn);
    if (mic) {
        hmac_sha1(s->kck, 16, e, 4 + 95 + kn, NULL, 0, d);
        memcpy(k + 77, d, 16);
    }
    ap_rx(s, f, n + 8 + 4 + 95 + kn, 1);
}

static void eapol_msg1(void *opaque)
{
    MT7921State *s = opaque;

    if (s->sta_state != AP_ASSOC || s->sta_bss != 1) {
        return;
    }
    qemu_guest_getrandom_nofail(s->anonce, 32);
    eapol_send(s, 0x008a, NULL, 0, false);      /* pairwise, ack */
}

/* the station's message: 2 (its SNonce: the PTK; its MIC, the passphrase
 * right: message 3), or 4 (done) */
static void eapol_rx(MT7921State *s, const uint8_t *e, int n)
{
    uint8_t ptk[48], b[76], m[400], d[20], kd[48], wrapped[56];
    const uint8_t *k = e + 4, *aa = s->bssid2, *spa = s->sta;
    uint16_t info;
    int len;

    if (s->sta_bss != 1 || s->sta_state != AP_ASSOC || n < 4 + 95 ||
        e[1] != 3 || k[0] != 2) {
        return;
    }
    info = lduw_be_p(k + 1);
    len = MIN(4 + lduw_be_p(e + 2), MIN(n, (int)sizeof(m)));
    if (!(info & 0x0100) || (info & 0x0080)) {  /* a MIC, no ack: ours */
        return;
    }
    if (!(info & 0x0200)) {                     /* message 2: the PTK */
        bool lo = memcmp(aa, spa, 6) < 0;
        bool nlo = memcmp(s->anonce, k + 13, 32) < 0;

        memcpy(b, lo ? aa : spa, 6);
        memcpy(b + 6, lo ? spa : aa, 6);
        memcpy(b + 12, nlo ? s->anonce : k + 13, 32);
        memcpy(b + 44, nlo ? k + 13 : s->anonce, 32);
        wpa_prf(s->pmk, "Pairwise key expansion", b, 76, ptk, 48);
        memcpy(s->kck, ptk, 16);
        memcpy(s->kek, ptk + 16, 16);
    }
    memcpy(m, e, len);                          /* its MIC, checked */
    memset(m + 4 + 77, 0, 16);
    hmac_sha1(s->kck, 16, m, len, NULL, 0, d);
    if (memcmp(d, k + 77, 16)) {                /* the passphrase wrong */
        uint8_t f[26];
        int h = ap_hdr(s, f, 0x00c0, s->sta, s->bssid2, s->bssid2);

        stw_le_p(f + h, 15);                    /* 4-way handshake timeout */
        ap_rx(s, f, h + 2, 1);
        s->sta_state = AP_NONE;
        return;
    }
    if (info & 0x0200) {                        /* message 4: done */
        s->sta_secure = true;
        return;
    }
    /* message 3: its RSN element, the GTK (key 1), padded to 8, wrapped */
    memcpy(kd, rsn_ie, sizeof(rsn_ie));
    memcpy(kd + 22, (uint8_t[]){ 0xdd, 22, 0x00, 0x0f, 0xac, 1, 1, 0 }, 8);
    memcpy(kd + 30, s->gtk, 16);
    kd[46] = 0xdd;
    kd[47] = 0;
    aes_wrap(s->kek, kd, 48, wrapped);
    eapol_send(s, 0x13ca, wrapped, 56, true);   /* install, ack, MIC,
                                                   secure, encrypted */
}

/* the AP's answer, a moment after the TX status of what it answers */
static void ap_reply_now(void *opaque)
{
    MT7921State *s = opaque;

    if (s->ap_pending && s->ap_pending->len) {
        ap_rx(s, s->ap_pending->data, s->ap_pending->len, 1);
        g_byte_array_set_size(s->ap_pending, 0);
    }
}

static void ap_reply(MT7921State *s, const uint8_t *f, int n)
{
    g_byte_array_set_size(s->ap_pending, 0);
    g_byte_array_append(s->ap_pending, f, n);
    timer_mod(s->ap_reply, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              30 * SCALE_MS);
}

/* an Ethernet frame of the station's, to the uplink */
static void ap_uplink(MT7921State *s, const uint8_t *eth, int n)
{
    if (n >= 14 && lduw_be_p(eth + 12) == 0x888e) {
        eapol_rx(s, eth + 14, n - 14);          /* (EAPOL: the AP's) */
    } else if (ap_port_open(s) && n >= 14 && ap_on(s)) {
        qemu_send_packet(qemu_get_queue(s->nic), eth, n);
    }
}

/* an 802.11 frame the station sent: to the AP, or through it */
static void ap_frame(MT7921State *s, const uint8_t *f, int n)
{
    static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
    uint8_t r[192], eth[2400];
    uint16_t fc;
    int h, m, net;

    if (n < 24 || !ap_on(s) || (net = ap_net_of(s, f + 4)) < 0) {
        return;                                 /* (addr1: not an AP) */
    }
    fc = lduw_le_p(f);
    switch (fc & 0xfc) {
    case 0xb0:                                  /* authentication: open */
        if (n < 30 || lduw_le_p(f + 24) != 0 || lduw_le_p(f + 26) != 1) {
            break;
        }
        memcpy(s->sta, f + 10, 6);
        s->sta_state = AP_AUTH;
        s->sta_bss = net;
        s->sta_secure = false;
        m = ap_hdr(s, r, 0x00b0, s->sta, ap_bssid(s, net), ap_bssid(s, net));
        stw_le_p(r + m, 0);                     /* open, 2, success */
        stw_le_p(r + m + 2, 2);
        stw_le_p(r + m + 4, 0);
        ap_reply(s, r, m + 6);
        break;
    case 0x00:                                  /* (re)association */
    case 0x20:
        if (s->sta_state == AP_NONE || net != s->sta_bss ||
            memcmp(f + 10, s->sta, 6)) {
            break;
        }
        s->sta_state = AP_ASSOC;
        s->sta_secure = false;
        m = ap_hdr(s, r, (fc & 0xfc) + 0x10, s->sta, ap_bssid(s, net),
                   ap_bssid(s, net));
        stw_le_p(r + m, net ? 0x0431 : 0x0421); /* its capability */
        stw_le_p(r + m + 2, 0);                 /* success */
        stw_le_p(r + m + 4, 0xc001);            /* AID 1 */
        m += 6;
        memcpy(r + m, ap_rates, sizeof(ap_rates));
        m += sizeof(ap_rates);
        memcpy(r + m, ap_xrates, sizeof(ap_xrates));
        m += sizeof(ap_xrates);
        m += ap_ht_ies(s, r + m);
        ap_reply(s, r, m);
        if (net) {                              /* then the handshake */
            timer_mod(s->eapol, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      80 * SCALE_MS);
        }
        break;
    case 0xd0:                                  /* action: an ADDBA Request */
        if (n < 33 || f[24] != 3 || f[25] != 0 ||
            s->sta_state != AP_ASSOC || memcmp(f + 10, s->sta, 6)) {
            break;
        }
        {
            uint16_t ba = lduw_le_p(f + 27), size = ba >> 6;

            /* accepted: its TID, immediate, its buffer up to 64 */
            ba = (ba & 0x3c) | 0x02 | (size && size < 64 ? size : 64) << 6;
            m = ap_hdr(s, r, 0x00d0, s->sta, ap_bssid(s, net),
                       ap_bssid(s, net));
            r[m++] = 3;                         /* Block Ack: ADDBA Response */
            r[m++] = 1;
            r[m++] = f[26];                     /* (its dialog token) */
            stw_le_p(r + m, 0);                 /* success */
            stw_le_p(r + m + 2, ba);
            stw_le_p(r + m + 4, lduw_le_p(f + 29));    /* (its timeout) */
            ap_reply(s, r, m + 6);
        }
        break;
    case 0xa0:                                  /* disassociation */
    case 0xc0:                                  /* deauthentication */
        if (!memcmp(f + 10, s->sta, 6)) {
            s->sta_state = AP_NONE;
            s->sta_secure = false;
        }
        break;
    case 0x08:                                  /* data (ToDS), QoS too */
    case 0x88:
        h = 24 + ((fc & 0xfc) == 0x88 ? 2 : 0) +
            ((fc & 0x88) == 0x88 && (fc & 0x8000) ? 4 : 0);
        if ((fc & 0x4000) && n >= h + 16 && memcmp(f + h, snap, 6) &&
            !memcmp(f + h + 8, snap, 6)) {
            h += 8;                             /* (the room for CCMP's
                                                   header, the chip's) */
        }
        if (!(fc & 0x0100) || n < h + 8 || memcmp(f + h, snap, 6) ||
            n - h - 6 + 12 > (int)sizeof(eth)) {
            break;
        }
        if (lduw_be_p(f + h + 6) == 0x888e) {   /* EAPOL: the AP's */
            eapol_rx(s, f + h + 8, n - h - 8);
            break;
        }
        memcpy(eth, f + 16, 6);                 /* DA: addr3 */
        memcpy(eth + 6, f + 10, 6);             /* SA: addr2 */
        memcpy(eth + 12, f + h + 6, n - h - 6); /* EtherType, payload */
        ap_uplink(s, eth, n - h - 6 + 12);
        break;
    default:                                    /* (null data, actions) */
        break;
    }
}

/* the uplink's frame: to the station, if it is associated (unicast to it,
 * multicast, broadcast) */
static bool ap_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t ap_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    MT7921State *s = qemu_get_nic_opaque(nc);
    static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
    uint8_t f[2400];
    bool group = buf[0] & 1;
    int n;

    if (!ap_port_open(s) || size < 14 || size - 12 + 30 > sizeof(f) ||
        (!group && memcmp(buf, s->sta, 6))) {
        return size;
    }
    n = ap_hdr(s, f, s->sta_bss ? 0x4208 : 0x0208, buf,     /* data, FromDS, */
               ap_bssid(s, s->sta_bss), buf + 6);   /* (protected) */
    memcpy(f + n, snap, 6);
    memcpy(f + n + 6, buf + 12, size - 12);
    ap_rx_sec(s, f, n + 6 + size - 12,
              !group ? 1 : memcmp(buf, bcast, 6) ? 2 : 3, s->sta_bss);
    return size;
}

static NetClientInfo ap_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ap_can_receive,
    .receive = ap_receive,
};

/* a TX status, acked at @rate (TXS_*), for @wcid's frame @pid: a packet of
 * type 0 on 0x84, one record */
static void wifi_txs(MT7921State *s, uint16_t wcid, uint8_t pid,
                     uint32_t rate)
{
    uint8_t r[40] = { 0 };

    stl_le_p(r, 40);
    stl_le_p(r + 8, rate | BIT(15));
    stl_le_p(r + 16, (uint32_t)(wcid & 0x3ff) << 16);
    stl_le_p(r + 20, (uint32_t)pid << 24);
    stl_le_p(r + 24, (uint32_t)qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    wifi_in(s, 0, r, 40, NULL, 0);
}

/* a frame the host sends: a 4-byte length, the TXD (16 words: DW0 its
 * length + 64, DW1 the WLAN index and the header's format, 0 802.3 or 2
 * 802.11; DW3 NO_ACK; DW5 the PID and TX_STATUS_HOST), the frame at 68 */
static void wifi_tx(MT7921State *s, const uint8_t *b, int len)
{
    uint32_t txd0, txd1, txd5;
    int n;

    if (len < 68 + 14) {
        return;
    }
    txd0 = ldl_le_p(b + 4);
    txd1 = ldl_le_p(b + 8);
    txd5 = ldl_le_p(b + 24);
    n = MIN((int)(txd0 & 0xffff) - 64, len - 68);
    if (n < 14) {
        return;
    }
    if ((txd5 & BIT(10)) && (txd5 & 0xff) >= 3) {
        /* (data, 802.3 or 802.11, at HT MCS 15; management at 6 Mbit/s) */
        bool data = ((txd1 >> 16) & 3) == 0 || (b[68] & 0x0c) == 0x08;

        wifi_txs(s, txd1 & 0x3ff, txd5 & 0xff,
                 data ? TXS_HT_MCS15 : TXS_OFDM6);
    }
    if (((txd1 >> 16) & 3) == 0) {             /* 802.3: as it is */
        ap_uplink(s, b + 68, n);
    } else {
        ap_frame(s, b + 68, n);
    }
}

/* does the scan (START_HW_SCAN's request) look on the AP's channel? (@158
 * channel_type 0: all of them; else @159 channels_num, @160 {band,
 * channel}, and past 32 @826 ext_channels_num, @830 the rest) */
static bool scan_has_chan(MT7921State *s, const uint8_t *pl, int n)
{
    int num, ext;

    if (n < 160 || pl[158] == 0) {
        return true;
    }
    num = MIN(pl[159], 32);
    for (int i = 0; i < num && 161 + 2 * i < n; i++) {
        if (pl[160 + 2 * i] == 1 && pl[161 + 2 * i] == s->ap_chan) {
            return true;
        }
    }
    ext = n > 826 ? pl[826] : 0;
    for (int i = 0; i < ext && 831 + 2 * i < n; i++) {
        if (pl[830 + 2 * i] == 1 && pl[831 + 2 * i] == s->ap_chan) {
            return true;
        }
    }
    return false;
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
    case 0x03:                                  /* START_HW_SCAN: the AP's */
        if (ap_on(s) && scan_has_chan(s, pl, n)) {      /* beacon in it */
            timer_mod(s->beacon, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SCAN_NS / 4);
        }
        timer_mod(s->scan, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SCAN_NS);
        break;
    case 0x1b:                                  /* CANCEL_HW_SCAN (the
                                                   driver completes it) */
        timer_del(s->scan);
        break;
    case 0x0a:                                  /* SET_RX_FILTER */
    case 0x0f:                                  /* SET_CHAN_DOMAIN */
    case 0x17:                                  /* SET_BSS_ABORT */
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
static void wifi_uni(MT7921State *s, uint16_t cid, uint8_t seq,
                     const uint8_t *pl, int n)
{
    uint8_t r[8] = { cid }, g[24] = { 0 };

    switch (cid) {
    case 0x27:                                  /* ROC: an acquire (its */
        if (n >= 28 && lduw_le_p(pl + 4) == 0) {        /* tag 0) granted */
            stw_le_p(g + 4, 0);                 /* (UNI_EVENT_ROC_GRANT) */
            stw_le_p(g + 6, 20);
            g[8] = pl[8];                       /* bss_idx, tokenid */
            g[9] = pl[9];
            g[11] = pl[10];                     /* the channel, sco, band, */
            g[12] = pl[11];                     /* width, centres */
            g[13] = pl[12];
            g[14] = pl[13];
            g[15] = pl[14];
            g[16] = pl[15];
            g[17] = pl[19];                     /* reqtype */
            g[18] = pl[24];                     /* dbdcband */
            stl_le_p(g + 20, ldl_le_p(pl + 20));        /* max_interval */
            wifi_event_opt(s, 0x27, 0, 0x04, 0, g, sizeof(g));
        }
        break;
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
        wifi_uni(s, lduw_le_p(t + 34), seq, t + 48, l >= 48 ? l - 48 : 0);
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

    /*
     * An IN transfer with no room: done, empty, nothing taken from a queue.
     * The host's xHCI driver turns a cancelled transfer's TRBs into No-Ops,
     * and QEMU's xHCI hands such a transfer on as an empty packet: NAKed, it
     * would stay first on its endpoint, and take the next packet queued
     * there into its no room (an ATT response, a received frame, lost).
     */
    if (p->pid == USB_TOKEN_IN && !p->iov.size) {
        return;
    }
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
    if (p->pid == USB_TOKEN_IN && p->ep->nr == 2) {     /* ACL to the host */
        GByteArray *a = g_queue_pop_head(&s->aclq);

        if (!a) {
            p->status = USB_RET_NAK;
            return;
        }
        n = MIN(a->len, p->iov.size);
        usb_packet_copy(p, a->data, n);
        if (n < a->len) {                       /* (the rest next time:
                                                   btusb puts it together) */
            g_byte_array_remove_range(a, 0, n);
            g_queue_push_head(&s->aclq, a);
        } else {
            g_byte_array_free(a, true);
        }
        return;
    }
    if (p->pid == USB_TOKEN_OUT && p->ep->nr == 2) {    /* ACL from it */
        g_autofree uint8_t *b = g_malloc(p->iov.size);

        usb_packet_copy(p, b, p->iov.size);
        acl_rx(s, b, p->iov.size);
        return;
    }
    if (p->pid == USB_TOKEN_IN && (p->ep->nr == 4 || p->ep->nr == 5)) {
        GByteArray *e;                          /* the Wi-Fi's MCU events */

        if (!(e = g_queue_pop_head(&s->wq[p->ep->nr == 4 ? 0 : 1]))) {
            p->status = USB_RET_NAK;            /* (nothing to take) */
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
    if (p->pid == USB_TOKEN_OUT && p->ep->nr >= 4 && p->ep->nr <= 9 &&
        !(p->ep->nr == 4 && (reg_get(s, 0x74000008) & BIT(3)))) {
        g_autofree uint8_t *b = g_malloc(p->iov.size);  /* a frame (not */
                                                        /* the firmware) */
        usb_packet_copy(p, b, p->iov.size);
        wifi_tx(s, b, p->iov.size);
        return;
    }
    if (p->pid == USB_TOKEN_OUT) {              /* ISO, the Wi-Fi's
                                                   firmware data (0x04 while
                                                   FW_DL_EN): taken */
        p->actual_length = p->iov.size;
        return;
    }
    p->status = USB_RET_NAK;                    /* (no SCO or ISO
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
        timer_del(s->beacon);
        timer_del(s->ap_reply);
        timer_del(s->eapol);
        s->sta_state = AP_NONE;
        s->sta_secure = false;
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

    s->speaker_fd = -1;
    if (s->speaker_path) {                      /* (what the speaker plays) */
        s->speaker_fd = qemu_create(s->speaker_path,
                                    O_WRONLY | O_TRUNC | O_BINARY, 0644, errp);
        if (s->speaker_fd < 0) {
            return;
        }
    }

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
    s->acl_in = usb_ep_get(dev, USB_TOKEN_IN, 2);
    s->evq = g_byte_array_new();
    s->inquiry = timer_new_ns(QEMU_CLOCK_VIRTUAL, inquiry_done, s);
    s->inq_result = timer_new_ns(QEMU_CLOCK_VIRTUAL, inq_result, s);
    s->br_page = timer_new_ns(QEMU_CLOCK_VIRTUAL, br_page_done, s);
    s->page = timer_new_ns(QEMU_CLOCK_VIRTUAL, page_done, s);
    s->beacon_ble = timer_new_ns(QEMU_CLOCK_VIRTUAL, beacon_ble, s);
    s->le_conn_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, le_conn_done, s);
    s->hr_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hr_tick, s);
    g_queue_init(&s->aclq);
    s->scan = timer_new_ns(QEMU_CLOCK_VIRTUAL, scan_done, s);
    s->beacon = timer_new_ns(QEMU_CLOCK_VIRTUAL, ap_beacon, s);
    s->ap_reply = timer_new_ns(QEMU_CLOCK_VIRTUAL, ap_reply_now, s);
    s->ap_pending = g_byte_array_new();
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_queue_init(&s->wq[0]);
    g_queue_init(&s->wq[1]);
    /* the AP's BSSID: a locally administered one of MediaTek's OUI */
    memcpy(s->bssid, (uint8_t[]){ 0x02, 0x0c, 0xe7, 0x46, 0x4f, 0xa0 }, 6);
    memcpy(s->conf.macaddr.a, s->bssid, 6);
    s->nic = qemu_new_nic(&ap_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), DEVICE(dev)->id,
                          &DEVICE(dev)->mem_reentrancy_guard, s);
    if (!s->ap_ssid) {
        s->ap_ssid = g_strdup("FlipperOneEmu");
    }
    /* the WPA2 network beside it: its SSID, BSSID, PMK, GTK */
    s->ssid2 = g_strdup_printf("%s-WPA2", s->ap_ssid);
    memcpy(s->bssid2, s->bssid, 6);
    s->bssid2[5]++;
    s->eapol = timer_new_ns(QEMU_CLOCK_VIRTUAL, eapol_msg1, s);
    if (ap_nets(s) > 1) {
        pbkdf2_sha1(s->ap_psk, s->ssid2, 4096, s->pmk, 32);
        qemu_guest_getrandom_nofail(s->gtk, 16);
    }
    controller_reset(s);
    dev->auto_attach = false;                   /* (powered by the board) */
}

static const Property mt7921_props[] = {
    DEFINE_PROP_STRING("bdaddr", MT7921State, bdaddr_prop),
    DEFINE_PROP_STRING("wifi-mac", MT7921State, wifi_mac_prop),
    DEFINE_PROP_STRING("ap-ssid", MT7921State, ap_ssid),
    DEFINE_PROP_UINT8("ap-channel", MT7921State, ap_chan, 6),
    DEFINE_PROP_STRING("ap-psk", MT7921State, ap_psk),
    /* the speaker's stream, raw SBC as it comes (ffmpeg -f sbc -i FILE) */
    DEFINE_PROP_STRING("speaker", MT7921State, speaker_path),
    DEFINE_NIC_PROPERTIES(MT7921State, conf),
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
