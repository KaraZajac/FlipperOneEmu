/*
 * The RK3576's HDMI transmitter (Synopsys DesignWare HDMI QP), as Linux's
 * dw-hdmi-qp drives it, and the monitor at the cable's end
 *
 * - Registers: kept as written (the driver reads several back and writes
 *   them modified).
 * - Its DDC master (I2CM): a write to INTERFACE_CONTROL0 that sets one of
 *   its operation bits does one single-byte transfer to the slave address
 *   in its bits 11:5, at the register in its bits 19:12 (with EXT_READ,
 *   plus 256 times the segment pointer in INTERFACE_CONTROL1). The
 *   monitor answers at 0x50 with its EDID (a write there only sets the
 *   offset); anything else, or no monitor, is a NACK. The operation bits
 *   clear themselves when the transfer is done, and with the master's soft
 *   reset (CONTROL0 bit 0): the driver clears them after a byte that went
 *   through, but after a NACK it only resets the master.
 * - MAINUNIT_1's interrupt: OP_DONE and NACK_RCVD, masked by its MASK_N,
 *   cleared by its CLEAR, on the main line.
 * The monitor (rk_hdmi_plug()): its EDID is QEMU's, a 128-byte DVI one
 * that prefers 1280x720 at 60 Hz (an RGB TMDS link: no SCDC, FRL, YUV or
 * audio). Its hotplug line is the IOC GRF's, which the SoC has.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/display/edid.h"
#include "rk3576.h"

#define I2CM_CONTROL0       0xec
#define I2CM_INTERFACE_CONTROL0 0xf4
#define I2CM_REG(v)         (((v) >> 12) & 0xff)
#define I2CM_SLAVE(v)       (((v) >> 5) & 0x7f)
#define I2CM_EXT_READ       BIT(4)
#define I2CM_SHORT_READ     BIT(3)
#define I2CM_FM_READ        BIT(2)
#define I2CM_FM_WRITE       BIT(1)
#define I2CM_OPS            0x1e
#define I2CM_INTERFACE_CONTROL1 0xf8
#define I2CM_SEG_PTR(v)     (((v) >> 7) & 0xff)
#define I2CM_INTERFACE_RDDATA_0_3 0x10c
#define MAINUNIT_1_INT_STATUS 0x3020
#define MAINUNIT_1_INT_MASK_N 0x3024
#define MAINUNIT_1_INT_CLEAR 0x3028
#define I2CM_OP_DONE        BIT(0)
#define I2CM_NACK_RCVD      BIT(2)

#define DDC_ADDR            0x50
#define R(off)              ((off) / 4)

static void hdmi_irq_update(RkHdmiState *s)
{
    qemu_set_irq(s->irq_main, (s->int1 &
                               s->regs[R(MAINUNIT_1_INT_MASK_N)]) != 0);
}

/* one DDC transfer, as INTERFACE_CONTROL0's operation bits ask */
static void hdmi_ddc(RkHdmiState *s, uint32_t ctl)
{
    bool monitor = s->connected && I2CM_SLAVE(ctl) == DDC_ADDR;

    if (monitor && (ctl & (I2CM_FM_READ | I2CM_EXT_READ | I2CM_SHORT_READ))) {
        unsigned off = I2CM_REG(ctl);

        if (ctl & I2CM_EXT_READ) {
            off += 256 * I2CM_SEG_PTR(s->regs[R(I2CM_INTERFACE_CONTROL1)]);
        }
        s->regs[R(I2CM_INTERFACE_RDDATA_0_3)] =
            off < sizeof(s->edid) ? s->edid[off] : 0xff;
        s->int1 |= I2CM_OP_DONE;
    } else if (monitor) {               /* (FM_WRITE: an offset, no more) */
        s->int1 |= I2CM_OP_DONE;
    } else {
        s->int1 |= I2CM_NACK_RCVD;
    }
    hdmi_irq_update(s);
}

static uint64_t hdmi_read(void *opaque, hwaddr off, unsigned size)
{
    RkHdmiState *s = opaque;

    off &= ~3;
    if (off == MAINUNIT_1_INT_STATUS) {
        return s->int1;
    }
    return s->regs[R(off)];
}

static void hdmi_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkHdmiState *s = opaque;

    off &= ~3;
    switch (off) {
    case MAINUNIT_1_INT_STATUS:
        return;
    case MAINUNIT_1_INT_CLEAR:
        s->int1 &= ~v;
        hdmi_irq_update(s);
        return;
    }
    s->regs[R(off)] = v;
    if (off == I2CM_INTERFACE_CONTROL0 && (v & I2CM_OPS)) {
        hdmi_ddc(s, v);
        s->regs[R(off)] &= ~I2CM_OPS;   /* (done: they clear themselves) */
    } else if (off == I2CM_CONTROL0 && (v & 1)) {
        s->regs[R(I2CM_INTERFACE_CONTROL0)] &= ~I2CM_OPS;  /* soft reset */
    } else if (off == MAINUNIT_1_INT_MASK_N) {
        hdmi_irq_update(s);
    }
}

static const MemoryRegionOps hdmi_ops = {
    .read = hdmi_read,
    .write = hdmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void rk_hdmi_plug(RkHdmiState *s, bool connected)
{
    s->connected = connected;
}

static void hdmi_reset(DeviceState *dev)
{
    RkHdmiState *s = RK_HDMI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->int1 = 0;
    hdmi_irq_update(s);
}

static void hdmi_realize(DeviceState *dev, Error **errp)
{
    /* the preferred timing CTA-861's 720p60 (74.25 MHz, 1650 x 750), not
     * the one QEMU works out: the HDMI PHY makes the rates of its table,
     * and Linux leaves out the modes it cannot */
    static const uint8_t dtd_720p[12] = {
        0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e, 0x28, 0x55, 0x00,
    };
    RkHdmiState *s = RK_HDMI(dev);
    uint8_t sum = 0;
    qemu_edid_info info = {
        .name = "FlipperOneEmu",
        .serial = "1",
        .prefx = 1280,
        .prefy = 720,
        .refresh_rate = 60000,
    };

    memory_region_init_io(&s->mr, OBJECT(s), &hdmi_ops, s, "rk3576.hdmi",
                          RK_HDMI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_main);
    /* (QEMU's generator counts extensions up from what byte 126 holds:
     * zeroes first. Past the block, 0xff, as from a bus nothing drives) */
    memset(s->edid, 0, 128);
    memset(s->edid + 128, 0xff, sizeof(s->edid) - 128);
    qemu_edid_generate(s->edid, 128, &info);
    memcpy(s->edid + 54, dtd_720p, sizeof(dtd_720p));
    s->edid[54 + 15] = s->edid[54 + 16] = 0;    /* (no border) */
    s->edid[54 + 17] = 0x1e;            /* digital separate sync, +h +v */
    for (int i = 0; i < 127; i++) {
        sum += s->edid[i];
    }
    s->edid[127] = -sum;
}

static void hdmi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Rockchip RK3576 HDMI transmitter (DW HDMI QP)";
    dc->realize = hdmi_realize;
    device_class_set_legacy_reset(dc, hdmi_reset);
}

static const TypeInfo hdmi_types[] = {
    {
        .name = TYPE_RK_HDMI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkHdmiState),
        .class_init = hdmi_class_init,
    },
};

DEFINE_TYPES(hdmi_types)
