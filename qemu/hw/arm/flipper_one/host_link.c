/*
 * The host link: a program in the RK3576's place on its link to the MCU
 * (docs/HOST_LINK.md)
 *
 * Until the RK3576 is emulated, a program can be its side of the I2C link:
 * the master of I2C1, which the MCU firmware serves in slave mode at 0x69
 * (i2c_intercom: 16-bit registers, the address first, high byte first,
 * then the values, low byte first),
 * and the reader of CPU_INT (GPIO35, open drain, low: something to read).
 * It sends a line a transfer: "write AAAA VVVV ..." (registers from AAAA),
 * "read AAAA N" (N registers), or the bytes themselves ("send HH ...",
 * "recv N"). Each is done as the RK3576's controller would at 1 MHz: a
 * START to 0x69, the bytes a step at a time (waiting while the MCU holds
 * SCL), a repeated START to read, the last byte NACKed, a STOP; then "ok",
 * "regs AAAA VVVV ...", "got HH ..." or "nack" (nothing answered at 0x69),
 * "timeout" (SCL held 200 ms). CPU_INT's level goes out as "int 0" or
 * "int 1" when it changes, and on connecting.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "flipper_one.h"
#include "rp2350.h"

#define BYTE_NS         9000            /* a byte and its ACK at 1 MHz */
#define HELD_NS         2000            /* SCL held: look again */
#define TIMEOUT_NS      200000000

typedef enum { OP_WRITE, OP_READ, OP_SEND, OP_RECV } HostOpKind;

typedef struct HostOp {
    HostOpKind kind;
    uint16_t reg;
    GByteArray *out;                    /* bytes to write */
    int nread;                          /* ... then bytes to read */
    GByteArray *in;
} HostOp;

typedef enum { ST_IDLE, ST_START_W, ST_WRITE, ST_START_R, ST_READ,
               ST_STOP } HostState;

/* a register's address goes high byte first, its value low byte first */
static void put16(GByteArray *a, unsigned v, bool high_first)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

    if (!high_first) {
        b[0] = v;
        b[1] = v >> 8;
    }
    g_byte_array_append(a, b, 2);
}

static void host_op_free(HostOp *op)
{
    g_byte_array_unref(op->out);
    g_byte_array_unref(op->in);
    g_free(op);
}

static void host_finish(FoHost *h, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void host_finish(FoHost *h, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *line = NULL;

    rp2350_i2c_target_stop(h->soc, h->i2c);
    va_start(ap, fmt);
    line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (h->lk.open) {
        flipper_link_send(&h->lk, "%s\n", line);
    }
    host_op_free(h->op);
    h->op = NULL;
    h->state = ST_IDLE;
}

static void host_reply(FoHost *h)
{
    HostOp *op = h->op;
    g_autoptr(GString) s = g_string_new(NULL);

    switch (op->kind) {
    case OP_WRITE: case OP_SEND:
        g_string_append(s, "ok");
        break;
    case OP_READ:
        g_string_append_printf(s, "regs %04x", op->reg);
        for (guint i = 0; i + 1 < op->in->len; i += 2) {
            g_string_append_printf(s, " %04x",
                                   op->in->data[i] | op->in->data[i + 1] << 8);
        }
        break;
    case OP_RECV:
        g_string_append(s, "got");
        for (guint i = 0; i < op->in->len; i++) {
            g_string_append_printf(s, " %02x", op->in->data[i]);
        }
        break;
    }
    host_finish(h, "%s", s->str);
}

static void host_after(FoHost *h, int64_t ns)
{
    timer_mod(h->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

/* the next transfer, if there is one */
static void host_next(FoHost *h)
{
    if (h->op || g_queue_is_empty(h->ops)) {
        return;
    }
    h->op = g_queue_pop_head(h->ops);
    h->pos = 0;
    h->started = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    h->state = h->op->out->len ? ST_START_W : ST_START_R;
    host_after(h, BYTE_NS);
}

/* a step of the transfer: a START, a byte, the STOP */
static void host_step(void *opaque)
{
    FoHost *h = opaque;
    HostOp *op = h->op;
    uint8_t byte;
    int r = 1;

    if (!op) {
        host_next(h);
        return;
    }
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - h->started > TIMEOUT_NS) {
        host_finish(h, "timeout");
        host_next(h);
        return;
    }
    switch (h->state) {
    case ST_START_W:
    case ST_START_R:
        if (!rp2350_i2c_target_start(h->soc, h->i2c, h->addr,
                                     h->state == ST_START_R)) {
            host_finish(h, "nack");
            host_next(h);
            return;
        }
        h->pos = 0;
        h->state = h->state == ST_START_R ? ST_READ : ST_WRITE;
        break;
    case ST_WRITE:
        r = rp2350_i2c_target_write(h->soc, h->i2c, op->out->data[h->pos]);
        if (r && ++h->pos == op->out->len) {
            h->state = op->nread ? ST_START_R : ST_STOP;
        }
        break;
    case ST_READ:
        r = rp2350_i2c_target_read(h->soc, h->i2c, &byte);
        if (r) {
            g_byte_array_append(op->in, &byte, 1);
            if (++h->pos == op->nread) {
                rp2350_i2c_target_nack(h->soc, h->i2c);
                h->state = ST_STOP;
            }
        }
        break;
    case ST_STOP:
        host_reply(h);
        host_next(h);
        return;
    case ST_IDLE:
        return;
    }
    host_after(h, r ? BYTE_NS : HELD_NS);
}

/* "write AAAA VVVV ..." "read AAAA N" "send HH ..." "recv N" */
static void host_line(void *opaque, char *line)
{
    FoHost *h = opaque;
    g_auto(GStrv) w = g_strsplit_set(g_strstrip(line), " \t", -1);
    HostOp *op;
    int n = g_strv_length(w);
    char *end;

    if (!n || !*w[0]) {
        return;
    }
    op = g_new0(HostOp, 1);
    op->out = g_byte_array_new();
    op->in = g_byte_array_new();
    if (!strcmp(w[0], "write") && n >= 2) {
        op->kind = OP_WRITE;
        op->reg = strtoul(w[1], NULL, 16);
        put16(op->out, op->reg, true);
        for (int i = 2; i < n; i++) {
            put16(op->out, strtoul(w[i], NULL, 16), false);
        }
    } else if (!strcmp(w[0], "read") && n == 3) {
        op->kind = OP_READ;
        op->reg = strtoul(w[1], NULL, 16);
        put16(op->out, op->reg, true);
        op->nread = 2 * strtoul(w[2], &end, 0);
    } else if (!strcmp(w[0], "send") && n >= 2) {
        op->kind = OP_SEND;
        for (int i = 1; i < n; i++) {
            uint8_t b = strtoul(w[i], NULL, 16);

            g_byte_array_append(op->out, &b, 1);
        }
    } else if (!strcmp(w[0], "recv") && n == 2) {
        op->kind = OP_RECV;
        op->nread = strtoul(w[1], &end, 0);
    } else {
        host_op_free(op);
        flipper_link_send(&h->lk, "error what is \"%s\"?\n", w[0]);
        return;
    }
    if (op->nread > 256 || (!op->out->len && !op->nread)) {
        host_op_free(op);
        flipper_link_send(&h->lk, "error too long, or nothing\n");
        return;
    }
    g_queue_push_tail(h->ops, op);
    host_next(h);
}

static void host_opened(void *opaque, bool open)
{
    FoHost *h = opaque;

    if (open) {
        flipper_link_send(&h->lk, "hello flipperoneemu host 1\n");
        flipper_link_send(&h->lk, "int %d\n", h->int_level);
    }
}

void fo_host_int(FoHost *h, bool level)
{
    if (level != h->int_level) {
        h->int_level = level;
        if (h->lk.open) {
            flipper_link_send(&h->lk, "int %d\n", level);
        }
    }
}

static void host_realize(DeviceState *dev, Error **errp)
{
    FoHost *h = FLIPPER_ONE_HOST(dev);

    h->ops = g_queue_new();
    h->int_level = 1;
    h->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, host_step, h);
    flipper_link_init(&h->lk, &h->link_chr, "host", host_line, host_opened, h);
}

static const Property host_properties[] = {
    DEFINE_PROP_CHR("link", FoHost, link_chr),
};

static void host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = host_realize;
    dc->desc = "Flipper One host link: a program as the RK3576's side of its "
               "I2C link to the MCU (docs/HOST_LINK.md)";
    device_class_set_props(dc, host_properties);
}

static const TypeInfo host_types[] = {
    {
        .name = TYPE_FLIPPER_ONE_HOST,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(FoHost),
        .class_init = host_class_init,
    },
};
DEFINE_TYPES(host_types)
