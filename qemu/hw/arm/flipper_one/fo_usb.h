/*
 * Flipper One: a USB device controller as the computer at the other end of
 * the USB-C cable drives it (usb_host.c), a transaction at a time. The
 * MCU's USB (rp2350_usb.c) and the RK3576's USB0 (rk_dwc3.c) each answer
 * through one of these; the main board's USB mux decides which one the
 * cable's data lines reach.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HW_ARM_FLIPPER_ONE_FO_USB_H
#define HW_ARM_FLIPPER_ONE_FO_USB_H

#define FO_USB_MAX_PACKET   1024        /* the most a packet carries */

/* how the device answers a transaction (TIMEOUT: it does not: a token to
 * an address it does not have, or no device on the bus) */
typedef enum {
    FO_USB_ACK, FO_USB_NAK, FO_USB_STALL, FO_USB_TIMEOUT,
} FoUsbResult;

typedef struct FoUsbDevOps {
    int speed;                          /* as USB/IP gives it: 2 full,
                                           3 high */
    /* D+ pulled up: the device is on the bus (the device tells the host
     * when this changes, through its pull-up callback) */
    bool (*pulled_up)(void *opaque);
    /* its CPU has not yet dealt with what the last transaction did */
    bool (*irq_pending)(void *opaque);
    void (*bus_reset)(void *opaque);
    void (*detach)(void *opaque);       /* the host has gone */
    FoUsbResult (*setup)(void *opaque, uint8_t addr, const uint8_t pkt[8]);
    FoUsbResult (*out)(void *opaque, uint8_t addr, int ep,
                       const uint8_t *data, int len);
    /* @data: room for FO_USB_MAX_PACKET bytes */
    FoUsbResult (*in)(void *opaque, uint8_t addr, int ep, uint8_t *data,
                      int *len);
} FoUsbDevOps;

#endif
