/*
 * The UART between the Flipper One's two chips: the RK3576's UART4 (the
 * device tree's "MCU interconnect", Flipper OS's console=ttyS4) and the
 * RP2350's UART0 (GPIO32 and 33, its firmware's "uart: cpu", which its
 * CLI's uart_echo opens), wired to each other inside the machine.
 *
 * Two chardevs, one each end's: what one end's UART sends goes into the
 * other's receiver, as fast as its FIFO takes it (the rest waits, up to
 * 64 KiB). The line's speed is not modelled, nor a difference between the
 * two ends' (Flipper OS's 1.5 Mbaud, uart_echo's 230400): bytes cross as
 * bytes. A program can still watch and type at the RK3576's end: what
 * UART4 sends is copied to a tap (devices/NAME/linux4.sock, the launcher's
 * `console --uart4`), and what the tap sends goes to UART4 too, beside the
 * MCU's.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "flipper_one.h"

#define TYPE_CHARDEV_FO_LINK "chardev-fo-uart-link"
#define LINK_MAX (64 * KiB)

typedef struct FoLinkChardev {
    Chardev parent;
    struct FoLinkChardev *peer;         /* the other end */
    GByteArray *in;                     /* on its way to this end's UART */
    CharFrontend *tap;                  /* a copy of what this end's UART
                                           sends (the RK3576's end) */
} FoLinkChardev;

OBJECT_DECLARE_SIMPLE_TYPE(FoLinkChardev, CHARDEV_FO_LINK)

/* as much as this end's UART takes now */
static void link_flush(FoLinkChardev *s)
{
    Chardev *chr = CHARDEV(s);

    while (s->in->len) {
        int n = MIN(qemu_chr_be_can_write(chr), (int)s->in->len);

        if (n <= 0) {
            break;
        }
        qemu_chr_be_write(chr, s->in->data, n);
        g_byte_array_remove_range(s->in, 0, n);
    }
}

/* bytes for this end's UART to receive (what does not fit is lost) */
static void link_push(FoLinkChardev *s, const uint8_t *buf, int len)
{
    len = MIN(len, LINK_MAX - (int)s->in->len);
    if (len > 0) {
        g_byte_array_append(s->in, buf, len);
    }
    link_flush(s);
}

/* this end's UART sends */
static int link_write(Chardev *chr, const uint8_t *buf, int len)
{
    FoLinkChardev *s = CHARDEV_FO_LINK(chr);

    if (s->tap && qemu_chr_fe_backend_connected(s->tap)) {
        qemu_chr_fe_write(s->tap, buf, len);    /* (a copy: may be lost) */
    }
    if (s->peer) {
        link_push(s->peer, buf, len);
    }
    return len;
}

/* its UART's FIFO has room again */
static void link_accept_input(Chardev *chr)
{
    link_flush(CHARDEV_FO_LINK(chr));
}

static void link_init(Object *obj)
{
    CHARDEV_FO_LINK(obj)->in = g_byte_array_new();
}

static void link_finalize(Object *obj)
{
    g_byte_array_free(CHARDEV_FO_LINK(obj)->in, true);
}

static void link_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_write = link_write;
    cc->chr_accept_input = link_accept_input;
}

static const TypeInfo link_info = {
    .name = TYPE_CHARDEV_FO_LINK,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(FoLinkChardev),
    .instance_init = link_init,
    .instance_finalize = link_finalize,
    .class_init = link_class_init,
};

static void link_register_types(void)
{
    type_register_static(&link_info);
}

type_init(link_register_types)

/* ------------------------------------------------------- the tap */

static int tap_can_receive(void *opaque)
{
    FoLinkChardev *s = opaque;

    return LINK_MAX - s->in->len;
}

static void tap_receive(void *opaque, const uint8_t *buf, int size)
{
    link_push(opaque, buf, size);
}

void fo_uart_link_new(Chardev **cpu, Chardev **mcu, CharFrontend *tap,
                      Chardev *tap_chr)
{
    FoLinkChardev *c = CHARDEV_FO_LINK(qemu_chardev_new(NULL,
                           TYPE_CHARDEV_FO_LINK, NULL, NULL, &error_fatal));
    FoLinkChardev *m = CHARDEV_FO_LINK(qemu_chardev_new(NULL,
                           TYPE_CHARDEV_FO_LINK, NULL, NULL, &error_fatal));

    c->peer = m;
    m->peer = c;
    if (tap_chr) {
        qemu_chr_fe_init(tap, tap_chr, &error_fatal);
        qemu_chr_fe_set_handlers(tap, tap_can_receive, tap_receive, NULL,
                                 NULL, c, NULL, true);
        c->tap = tap;
    }
    *cpu = CHARDEV(c);
    *mcu = CHARDEV(m);
}
