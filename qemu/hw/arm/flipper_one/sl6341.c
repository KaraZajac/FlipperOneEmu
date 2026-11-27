/*
 * The Flipper One's USB hub, on its USB1: a CoreChips SL6341, USB 3.0,
 * four ports. As every USB 3 hub, it is two hubs to its host, on the two
 * ports of USB1's one connection (rk_dwc3.c: the DWC3's ports, QEMU patch
 * 0026):
 * - its USB 2.0 hub, 3431:6241: high speed, one transaction translator,
 *   on USB1's USB 2 port, xHCI port 1;
 * - its SuperSpeed hub, 3431:6341, on USB1's USB 3 port, xHCI port 2.
 * The device tree has them as hub@1 and hub@2 (Linux's onboard hub driver
 * powers them), with the same four ports: 1 the MT7921, on SuperSpeed;
 * 2 the USB-A connector; 3 USB-C2, its SuperSpeed lanes through the
 * HD3SS3220; 4 not connected. A port wired to nothing takes no device
 * here: QEMU's ports are "1.2" and "1.3" on the USB 2.0 hub (USB-A,
 * USB-C2; a device plugged in with no port given goes to the first free,
 * USB-A's), and "2.1" (the MT7921), "2.2" and "2.3" on the SuperSpeed hub.
 *
 * Its ports do what Linux's hub driver asks of them: powered, a
 * connection, a reset that enables, suspend; on the SuperSpeed hub its
 * link states (Rx.Detect with nothing there, U0, U3 suspended), warm
 * reset, the U1 and U2 timeouts, the hub's depth. Its identity past its
 * IDs is not the chip's, which is not published: no strings, its
 * bcdDevice 1.00, its delays.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "hw/usb/desc.h"
#include "flipper_one.h"

#define NPORTS          4
#define PORT_WIRED(n)   ((n) == 1 || (n) == 2 || (n) == 3)

/* class requests: bmRequestType << 8 | bRequest */
#define ClearHubFeature  (0x2000 | USB_REQ_CLEAR_FEATURE)
#define ClearPortFeature (0x2300 | USB_REQ_CLEAR_FEATURE)
#define GetHubDescriptor (0xa000 | USB_REQ_GET_DESCRIPTOR)
#define GetHubStatus     (0xa000 | USB_REQ_GET_STATUS)
#define GetPortStatus    (0xa300 | USB_REQ_GET_STATUS)
#define SetHubFeature    (0x2000 | USB_REQ_SET_FEATURE)
#define SetPortFeature   (0x2300 | USB_REQ_SET_FEATURE)
#define ClearTTBuffer    0x2308
#define ResetTT          0x2309
#define StopTT           0x230b
#define SetHubDepth      0x200c
#define GetPortErrCount  0xa30d

/* wPortStatus: both hubs */
#define ST_CONNECTION   0x0001
#define ST_ENABLE       0x0002
#define ST_SUSPEND      0x0004          /* (USB 2.0) */
#define ST_RESET        0x0010
/* ... the USB 2.0 hub's */
#define ST_POWER        0x0100
#define ST_LOW_SPEED    0x0200
#define ST_HIGH_SPEED   0x0400
#define ST_SPEED        (ST_LOW_SPEED | ST_HIGH_SPEED)
/* ... the SuperSpeed hub's */
#define ST_SS_LINK      0x01e0          /* its link state, << 5 */
#define ST_SS_POWER     0x0200
#define LS_U0           0x0
#define LS_U3           0x3
#define LS_RX_DETECT    0x5

/* wPortChange */
#define CH_CONNECTION   0x0001
#define CH_ENABLE       0x0002          /* (USB 2.0) */
#define CH_SUSPEND      0x0004          /* (USB 2.0) */
#define CH_RESET        0x0010
#define CH_BH_RESET     0x0020          /* (SuperSpeed) */
#define CH_LINK_STATE   0x0040          /* (SuperSpeed) */
#define CH_CONFIG_ERROR 0x0080          /* (SuperSpeed) */

/* features */
#define F_PORT_ENABLE           1
#define F_PORT_SUSPEND          2
#define F_PORT_RESET            4
#define F_PORT_LINK_STATE       5
#define F_PORT_POWER            8
#define F_C_PORT_CONNECTION     16
#define F_C_PORT_ENABLE         17
#define F_C_PORT_SUSPEND        18
#define F_C_PORT_OVER_CURRENT   19
#define F_C_PORT_RESET          20
#define F_PORT_TEST             21
#define F_PORT_INDICATOR        22
#define F_PORT_U1_TIMEOUT       23
#define F_PORT_U2_TIMEOUT       24
#define F_C_PORT_LINK_STATE     25
#define F_C_PORT_CONFIG_ERROR   26
#define F_PORT_REMOTE_WAKE_MASK 27
#define F_BH_PORT_RESET         28
#define F_C_BH_PORT_RESET       29
#define F_FORCE_LINKPM_ACCEPT   30

typedef struct Sl6341Port {
    USBPort port;
    uint16_t status;
    uint16_t change;
} Sl6341Port;

typedef struct Sl6341State {
    USBDevice dev;
    bool super;                 /* the SuperSpeed hub, else the USB 2.0 one */
    USBEndpoint *intr;
    Sl6341Port ports[NPORTS];
} Sl6341State;

OBJECT_DECLARE_SIMPLE_TYPE(Sl6341State, SL6341)

/* ------------------------------------------------------- descriptors */

static const USBDescIface iface_fs = {
    .bInterfaceNumber = 0, .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_HUB,
    .eps = (USBDescEndpoint[]) { {
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 1, .bInterval = 0xff,
    } },
};

/* high speed: 2^(12-1) microframes, 256 ms */
static const USBDescIface iface_hs = {
    .bInterfaceNumber = 0, .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_HUB,
    .eps = (USBDescEndpoint[]) { {
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 1, .bInterval = 12,
    } },
};

static const USBDescIface iface_ss = {
    .bInterfaceNumber = 0, .bNumEndpoints = 1,
    .bInterfaceClass = USB_CLASS_HUB,
    .eps = (USBDescEndpoint[]) { {
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes = USB_ENDPOINT_XFER_INT,
        .wMaxPacketSize = 2, .bInterval = 12, .wBytesPerInterval = 2,
    } },
};

#define HUB_CONF(ifp) (USBDescConfig[]) { {                             \
        .bNumInterfaces = 1, .bConfigurationValue = 1,                  \
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER |       \
                        USB_CFG_ATT_WAKEUP,                             \
        .nif = 1, .ifs = (ifp) } }

static const USBDescDevice dev_fs = {
    .bcdUSB = 0x0210, .bDeviceClass = USB_CLASS_HUB,
    .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = HUB_CONF(&iface_fs),
};

/* one transaction translator */
static const USBDescDevice dev_hs = {
    .bcdUSB = 0x0210, .bDeviceClass = USB_CLASS_HUB,
    .bDeviceProtocol = 1,
    .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = HUB_CONF(&iface_hs),
};

static const USBDescDevice dev_ss = {
    .bcdUSB = 0x0320, .bDeviceClass = USB_CLASS_HUB,
    .bDeviceProtocol = 3,
    .bMaxPacketSize0 = 9, .bNumConfigurations = 1,
    .confs = HUB_CONF(&iface_ss),
};

static const USBDesc desc_usb2 = {
    .id = { .idVendor = 0x3431, .idProduct = 0x6241, .bcdDevice = 0x0100 },
    .full = &dev_fs,
    .high = &dev_hs,
};

static const USBDesc desc_usb3 = {
    .id = { .idVendor = 0x3431, .idProduct = 0x6341, .bcdDevice = 0x0100 },
    .super = &dev_ss,
};

/* ------------------------------------------------------- ports */

static uint16_t power_bit(Sl6341State *s)
{
    return s->super ? ST_SS_POWER : ST_POWER;
}

static void port_link(Sl6341Port *p, int state)
{
    p->status = (p->status & ~ST_SS_LINK) | state << 5;
}

static bool port_present(Sl6341Port *p)
{
    return p->port.dev && p->port.dev->attached;
}

/* the port as it stands: its device there, or not */
static void port_update(Sl6341State *s, Sl6341Port *p)
{
    bool was = p->status & ST_CONNECTION;
    bool now = port_present(p) && (p->status & power_bit(s));

    if (now) {
        p->status |= ST_CONNECTION;
        if (s->super && !was) {
            /* (a SuperSpeed link trains to U0 and enables its port) */
            p->status |= ST_ENABLE;
            port_link(p, LS_U0);
        }
        if (!s->super) {
            p->status &= ~ST_SPEED;
            p->status |= p->port.dev->speed == USB_SPEED_HIGH ? ST_HIGH_SPEED :
                         p->port.dev->speed == USB_SPEED_LOW ? ST_LOW_SPEED :
                         0;
        }
    } else {
        p->status &= ~(ST_CONNECTION | ST_ENABLE | ST_SUSPEND);
        if (!s->super) {
            p->status &= ~ST_SPEED;
        }
    }
    if (s->super && (p->status & ST_SS_POWER) && !(p->status & ST_ENABLE)) {
        port_link(p, LS_RX_DETECT);
    }
    if (was != now) {
        p->change |= CH_CONNECTION;
    }
}

static void notify(Sl6341State *s)
{
    usb_wakeup(s->intr, 0);
}

static void sl6341_attach(USBPort *port)
{
    Sl6341State *s = port->opaque;

    port_update(s, &s->ports[port->index]);
    notify(s);
}

static void sl6341_detach(USBPort *port)
{
    Sl6341State *s = port->opaque;
    Sl6341Port *p = &s->ports[port->index];

    /* let the host know the device is gone */
    s->dev.port->ops->child_detach(s->dev.port, port->dev);
    if (p->status & ST_CONNECTION) {
        p->change |= CH_CONNECTION;
    }
    p->status &= ~(ST_CONNECTION | ST_ENABLE | ST_SUSPEND);
    if (!s->super) {
        p->status &= ~ST_SPEED;
    } else if (p->status & ST_SS_POWER) {
        port_link(p, LS_RX_DETECT);
    }
    notify(s);
}

static void sl6341_child_detach(USBPort *port, USBDevice *child)
{
    Sl6341State *s = port->opaque;

    s->dev.port->ops->child_detach(s->dev.port, child);
}

/* a device's remote wakeup: its port resumes */
static void sl6341_wakeup(USBPort *port)
{
    Sl6341State *s = port->opaque;
    Sl6341Port *p = &s->ports[port->index];

    if (s->super) {
        if (((p->status & ST_SS_LINK) >> 5) == LS_U3) {
            port_link(p, LS_U0);
            p->change |= CH_LINK_STATE;
            notify(s);
        }
    } else if (p->status & ST_SUSPEND) {
        p->status &= ~ST_SUSPEND;
        p->change |= CH_SUSPEND;
        notify(s);
    }
}

/* (a packet behind the hub completes upstream, as QEMU's own hub has it) */
static void sl6341_complete(USBPort *port, USBPacket *packet)
{
    Sl6341State *s = port->opaque;

    s->dev.port->ops->complete(s->dev.port, packet);
}

static USBPortOps sl6341_port_ops = {
    .attach = sl6341_attach,
    .detach = sl6341_detach,
    .child_detach = sl6341_child_detach,
    .wakeup = sl6341_wakeup,
    .complete = sl6341_complete,
};

/* a port reset: the device's, then the port enabled (in U0) */
static void port_reset(Sl6341State *s, Sl6341Port *p, bool warm)
{
    if (port_present(p) && (p->status & power_bit(s))) {
        usb_device_reset(p->port.dev);
        p->status |= ST_CONNECTION | ST_ENABLE;
        p->status &= ~ST_SUSPEND;
        if (s->super) {
            port_link(p, LS_U0);
        }
    } else {
        p->status &= ~ST_ENABLE;
    }
    p->status &= ~ST_RESET;
    p->change |= CH_RESET | (s->super && warm ? CH_BH_RESET : 0);
    notify(s);
}

static USBDevice *sl6341_find_device(USBDevice *dev, uint8_t addr)
{
    Sl6341State *s = SL6341(dev);

    for (int i = 0; i < NPORTS; i++) {
        USBDevice *d;

        if (!(s->ports[i].status & ST_ENABLE)) {
            continue;
        }
        d = usb_find_device(&s->ports[i].port, addr);
        if (d) {
            return d;
        }
    }
    return NULL;
}

/* a reset of the hub: its ports powered (its power switching is
 * per port, but Linux powers them all at once), their devices there */
static void sl6341_handle_reset(USBDevice *dev)
{
    Sl6341State *s = SL6341(dev);

    for (int i = 0; i < NPORTS; i++) {
        Sl6341Port *p = &s->ports[i];

        p->status = power_bit(s);
        p->change = 0;
        if (s->super) {
            port_link(p, LS_RX_DETECT);
        }
        port_update(s, p);
    }
}

/* the hub descriptor: USB 2.0's (0x29) or SuperSpeed's (0x2a) */
static int hub_descriptor(Sl6341State *s, uint8_t *d)
{
    /* per-port power switching and over-current; port 1 not removable */
    if (s->super) {
        static const uint8_t ss[12] = {
            12, 0x2a, NPORTS, 0x09, 0x00, 10, 0, 0, 0x00, 0x00, 0x02, 0x00,
        };

        memcpy(d, ss, sizeof(ss));
        return sizeof(ss);
    } else {
        static const uint8_t hs[9] = {
            9, 0x29, NPORTS, 0x09, 0x00, 10, 0, 0x02, 0xff,
        };

        memcpy(d, hs, sizeof(hs));
        return sizeof(hs);
    }
}

static void sl6341_handle_control(USBDevice *dev, USBPacket *pk,
                                  int request, int value, int index,
                                  int length, uint8_t *data)
{
    Sl6341State *s = SL6341(dev);
    unsigned n = (index & 0xff) - 1;
    Sl6341Port *p = n < NPORTS ? &s->ports[n] : NULL;

    if (usb_desc_handle_control(dev, pk, request, value, index, length,
                                data) >= 0) {
        return;
    }

    switch (request) {
    case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
        if (value != 0 || index != 0x81) {     /* (the endpoint's halt) */
            goto stall;
        }
        break;
    case GetHubDescriptor:
        pk->actual_length = MIN(hub_descriptor(s, data), length);
        break;
    case GetHubStatus:
        memset(data, 0, 4);
        pk->actual_length = 4;
        break;
    case SetHubFeature:
    case ClearHubFeature:
        if (value != 0 && value != 1) {
            goto stall;
        }
        break;
    case SetHubDepth:
    case ClearTTBuffer:
    case ResetTT:
    case StopTT:
        break;
    case GetPortErrCount:
        if (!p || !s->super) {
            goto stall;
        }
        data[0] = data[1] = 0;
        pk->actual_length = 2;
        break;
    case GetPortStatus:
        if (!p) {
            goto stall;
        }
        stw_le_p(data, p->status);
        stw_le_p(data + 2, p->change);
        pk->actual_length = 4;
        break;
    case SetPortFeature:
        if (!p) {
            goto stall;
        }
        switch (value) {
        case F_PORT_RESET:
            port_reset(s, p, false);
            break;
        case F_BH_PORT_RESET:
            if (!s->super) {
                goto stall;
            }
            port_reset(s, p, true);
            break;
        case F_PORT_POWER:
            p->status |= power_bit(s);
            port_update(s, p);
            notify(s);
            break;
        case F_PORT_SUSPEND:
            if (s->super) {
                goto stall;
            }
            p->status |= ST_SUSPEND;
            break;
        case F_PORT_LINK_STATE: {
            int state = index >> 8;

            if (!s->super) {
                goto stall;
            }
            if (state == LS_U0 || state == LS_U3) {
                port_link(p, state);
            } else if (state == LS_RX_DETECT) {
                p->status &= ~ST_ENABLE;
                port_link(p, LS_RX_DETECT);
            }
            break;
        }
        case F_PORT_U1_TIMEOUT:
        case F_PORT_U2_TIMEOUT:
        case F_PORT_REMOTE_WAKE_MASK:
        case F_FORCE_LINKPM_ACCEPT:
        case F_PORT_TEST:
        case F_PORT_INDICATOR:
            break;
        default:
            goto stall;
        }
        break;
    case ClearPortFeature:
        if (!p) {
            goto stall;
        }
        switch (value) {
        case F_PORT_ENABLE:
            p->status &= ~ST_ENABLE;
            break;
        case F_PORT_SUSPEND:
            if (p->status & ST_SUSPEND) {
                p->status &= ~ST_SUSPEND;
                p->change |= CH_SUSPEND;
                notify(s);
            }
            break;
        case F_PORT_POWER:
            p->status &= ~(power_bit(s) | ST_CONNECTION | ST_ENABLE |
                           ST_SUSPEND);
            if (s->super) {
                port_link(p, 0x4);              /* SS.Disabled */
            }
            p->change = 0;
            break;
        case F_C_PORT_CONNECTION:
            p->change &= ~CH_CONNECTION;
            break;
        case F_C_PORT_ENABLE:
            p->change &= ~CH_ENABLE;
            break;
        case F_C_PORT_SUSPEND:
            p->change &= ~CH_SUSPEND;
            break;
        case F_C_PORT_OVER_CURRENT:
            break;
        case F_C_PORT_RESET:
            p->change &= ~CH_RESET;
            break;
        case F_C_BH_PORT_RESET:
            p->change &= ~CH_BH_RESET;
            break;
        case F_C_PORT_LINK_STATE:
            p->change &= ~CH_LINK_STATE;
            break;
        case F_C_PORT_CONFIG_ERROR:
            p->change &= ~CH_CONFIG_ERROR;
            break;
        case F_PORT_INDICATOR:
        case F_PORT_REMOTE_WAKE_MASK:
            break;
        default:
            goto stall;
        }
        break;
    default:
    stall:
        pk->status = USB_RET_STALL;
        break;
    }
}

/* the status change endpoint: a bit for each port with a change */
static void sl6341_handle_data(USBDevice *dev, USBPacket *pk)
{
    Sl6341State *s = SL6341(dev);
    uint8_t bits = 0;

    if (pk->pid != USB_TOKEN_IN || pk->ep->nr != 1) {
        pk->status = USB_RET_STALL;
        return;
    }
    for (int i = 0; i < NPORTS; i++) {
        if (s->ports[i].change) {
            bits |= 1 << (i + 1);
        }
    }
    if (!bits) {
        pk->status = USB_RET_NAK;
        return;
    }
    usb_packet_copy(pk, &bits, MIN(pk->iov.size, 1));
}

static void sl6341_realize(USBDevice *dev, Error **errp)
{
    Sl6341State *s = SL6341(dev);

    dev->usb_desc = s->super ? &desc_usb3 : &desc_usb2;
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, 1);
    for (int i = 0; i < NPORTS; i++) {
        if (!PORT_WIRED(i + 1) || (!s->super && i + 1 == 1)) {
            continue;           /* (nothing can be plugged in there) */
        }
        usb_register_port(usb_bus_from_device(dev), &s->ports[i].port, s, i,
                          &sl6341_port_ops,
                          s->super ? USB_SPEED_MASK_SUPER :
                          USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL |
                          USB_SPEED_MASK_HIGH);
        usb_port_location(&s->ports[i].port, dev->port, i + 1);
    }
    sl6341_handle_reset(dev);
}

static void sl6341_unrealize(USBDevice *dev)
{
    Sl6341State *s = SL6341(dev);

    for (int i = 0; i < NPORTS; i++) {
        if (s->ports[i].port.opaque) {
            usb_unregister_port(usb_bus_from_device(dev),
                                &s->ports[i].port);
        }
    }
}

static const Property sl6341_properties[] = {
    DEFINE_PROP_BOOL("super", Sl6341State, super, false),
};

static void sl6341_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize = sl6341_realize;
    uc->unrealize = sl6341_unrealize;
    uc->product_desc = "CoreChips SL6341 USB 3.0 hub";
    uc->usb_desc = &desc_usb2;
    uc->find_device = sl6341_find_device;
    uc->handle_attach = usb_desc_attach;        /* (its speed's descriptors) */
    uc->handle_reset = sl6341_handle_reset;
    uc->handle_control = sl6341_handle_control;
    uc->handle_data = sl6341_handle_data;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->desc = "the Flipper One's USB hub (one of its two halves)";
    device_class_set_props(dc, sl6341_properties);
}

static const TypeInfo sl6341_info = {
    .name = TYPE_SL6341,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(Sl6341State),
    .class_init = sl6341_class_init,
};

static void sl6341_register_types(void)
{
    type_register_static(&sl6341_info);
}

type_init(sl6341_register_types)
