/*
 * The RK3576's USB controllers: Synopsys DWC3 cores (DWC_usb3 3.30a),
 * dual-role, as Linux's dwc3 drives them, with QEMU's xHCI for the host
 * role
 *
 * A controller's 4 MiB:
 *   0x0000  QEMU's xHCI (16 KiB: capability, operational, port, runtime
 *           and doorbell registers): one USB 2 and one USB 3 root port,
 *           one interrupter; ERSTBA taking effect on its low dword, as on
 *           DWC3, where Linux writes it high dword first (QEMU patch 0011)
 *   0xc100  the DWC3's global registers: GSNPSID 0x5533330a; GHWPARAMS of
 *           a dual-role core (MODE 2) with 64-bit addresses, a UTMI PHY,
 *           16 endpoints and FIFOs for a gadget; GEVNTCOUNT reads 0 (no
 *           event is ever written); the rest kept as written
 *   0xc700  its device registers: DCTL's soft reset done at once, DSTS
 *           halted while RUN_STOP is clear, endpoint and generic commands
 *           done at once; the rest kept. A gadget bound to it runs, and no
 *           host ever comes: the computer on the USB-C port is the MCU's
 *           so far
 *   the rest reads 0
 * Its interrupt is the xHCI's.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "rk3576.h"

#define REGS_BASE       0xc100
#define R(a)            (((a) - REGS_BASE) / 4)

#define GSNPSID         0xc120
#define GHWPARAMS0      0xc140          /* ... GHWPARAMS7 at 0xc15c */
#define GUSB2PHYCFG     0xc200
#define GUSB3PIPECTL    0xc2c0
#define GTXFIFOSIZ(n)   (0xc300 + 4 * (n))
#define GRXFIFOSIZ0     0xc380
#define GEVNTCOUNT      0xc40c
#define GHWPARAMS8      0xc600
#define DCTL            0xc704
#define DCTL_RUN_STOP   BIT(31)
#define DCTL_CSFTRST    BIT(30)
#define DSTS            0xc70c
#define DSTS_DEVCTRLHLT BIT(22)
#define DGCMD           0xc714
#define DEPCMD0         0xc80c          /* ... every 0x10, 32 of them */
#define CMD_DONE        (BIT(10) | 0xf000 | 0x7f0000)   /* CMDACT, status,
                                                           resource index */

static const uint32_t ghwparams[8] = {
    0x4020400a,     /* DRD, AXI, 64-bit addresses */
    0x01008000,     /* EN_PWROPT 1, one interrupt */
    0,
    0x08210045,     /* 16 endpoints (8 IN), UTMI 16-bit, SuperSpeed Gen1 */
    0, 0,
    0,              /* (not an FPGA) */
    0x02850800,
};

static uint64_t dwc3_read(void *opaque, hwaddr off, unsigned size)
{
    RkDwc3State *s = opaque;
    hwaddr a = off + REGS_BASE;

    switch (a) {
    case GSNPSID:
        return 0x5533330a;
    case GHWPARAMS8:
    case GEVNTCOUNT:
        return 0;
    case DCTL:
        return s->regs[R(a)] & ~DCTL_CSFTRST;  /* (reset at once) */
    case DSTS:
        return 0x00920004 |
               (s->regs[R(DCTL)] & DCTL_RUN_STOP ? 0 : DSTS_DEVCTRLHLT);
    case DGCMD:
        return s->regs[R(a)] & ~CMD_DONE;
    }
    if (a >= GHWPARAMS0 && a < GHWPARAMS0 + sizeof(ghwparams)) {
        return ghwparams[(a - GHWPARAMS0) / 4];
    }
    if (a >= DEPCMD0 && a < DEPCMD0 + 32 * 0x10 && (a & 0xf) == 0xc) {
        return s->regs[R(a)] & ~CMD_DONE;
    }
    return s->regs[R(a)];
}

static void dwc3_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkDwc3State *s = opaque;
    hwaddr a = off + REGS_BASE;

    if (a == GSNPSID || a == GEVNTCOUNT || a == GHWPARAMS8 ||
        (a >= GHWPARAMS0 && a < GHWPARAMS0 + sizeof(ghwparams))) {
        return;                                 /* (read-only) */
    }
    s->regs[R(a)] = v;
}

static const MemoryRegionOps dwc3_ops = {
    .read = dwc3_read,
    .write = dwc3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* what nothing is at: reads 0, takes writes */
static uint64_t quiet_read(void *opaque, hwaddr off, unsigned size)
{
    return 0;
}

static void quiet_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
}

static const MemoryRegionOps quiet_ops = {
    .read = quiet_read,
    .write = quiet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
};

static void dwc3_reset(DeviceState *dev)
{
    RkDwc3State *s = RK_DWC3(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(GUSB2PHYCFG)] = 0x40102400;
    s->regs[R(GUSB3PIPECTL)] = 0x010c0002;
    s->regs[R(GTXFIFOSIZ(0))] = 0x0042;
    for (int n = 1; n < 8; n++) {
        s->regs[R(GTXFIFOSIZ(n))] = 0x0184;     /* (max packet 1024) */
    }
    s->regs[R(GRXFIFOSIZ0)] = 0x0285;
}

static void dwc3_init(Object *obj)
{
    RkDwc3State *s = RK_DWC3(obj);

    object_initialize_child(obj, "xhci", &s->xhci, TYPE_XHCI_SYSBUS);
}

static void dwc3_realize(DeviceState *dev, Error **errp)
{
    RkDwc3State *s = RK_DWC3(dev);
    SysBusDevice *xhci = SYS_BUS_DEVICE(&s->xhci);

    /* (sysbus-xhci's defaults for intrs and slots do not survive its
     * core's initialisation, which zeroes them: both are set here) */
    qdev_prop_set_uint32(DEVICE(xhci), "p2", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "p3", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "intrs", 1);
    qdev_prop_set_uint32(DEVICE(xhci), "slots", 64);
    qdev_prop_set_bit(DEVICE(xhci), "erstba-hi-lo", true);    /* (0011) */
    if (!sysbus_realize(xhci, errp)) {
        return;
    }
    memory_region_init(&s->mr, OBJECT(s), "rk-dwc3", 4 * MiB);
    memory_region_init_io(&s->quiet, OBJECT(s), &quiet_ops, s,
                          "rk-dwc3.none", 4 * MiB);
    memory_region_add_subregion_overlap(&s->mr, 0, &s->quiet, -1);
    memory_region_add_subregion(&s->mr, 0, sysbus_mmio_get_region(xhci, 0));
    memory_region_init_io(&s->regs_mr, OBJECT(s), &dwc3_ops, s,
                          "rk-dwc3.regs", sizeof(s->regs));
    memory_region_add_subregion(&s->mr, REGS_BASE, &s->regs_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    sysbus_pass_irq(SYS_BUS_DEVICE(dev), xhci);
}

static void dwc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = dwc3_realize;
    device_class_set_legacy_reset(dc, dwc3_reset);
}

static const TypeInfo dwc3_types[] = {
    {
        .name = TYPE_RK_DWC3,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkDwc3State),
        .instance_init = dwc3_init,
        .class_init = dwc3_class_init,
    },
};

DEFINE_TYPES(dwc3_types)
