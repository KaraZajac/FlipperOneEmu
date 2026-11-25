/*
 * Rockchip's IOMMU (v2, "rockchip,rk3568-iommu"), as Linux's rockchip-iommu
 * drives it: one or more banks of the same registers (programmed alike) in
 * front of a master, which has its addresses translated through it
 * (rk_iommu_translate()).
 *
 * - DTE_ADDR: the directory's address, as v2 encodes a 40-bit one (bits
 *   31:12, then 35:32 in 11:8 and 39:36 in 7:4); kept as written, and
 *   cleared by a forced reset (Linux checks both).
 * - STATUS: paging and stall as the commands left them; idle, its replay
 *   buffer empty, no page fault.
 * - COMMAND: enable or disable paging, enable or disable a stall, zap the
 *   cache, a page fault done, a forced reset: done at once.
 * - The interrupt registers: no fault is raised (a mapping missing makes
 *   the master read zeroes), so the line stays low. AUTO_GATING is kept.
 * Translation: the directory (1024 32-bit entries, IOVA bits 31:22) gives
 * a page table (IOVA bits 21:12), whose entry gives the 4 KiB page; an
 * entry is valid with bit 0, its address encoded as DTE_ADDR's is.
 * Paging off, an address is its own.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "rk3576.h"

#define MMU_DTE_ADDR        0x00
#define MMU_STATUS          0x04
#define STATUS_PAGING       BIT(0)
#define STATUS_STALL        BIT(2)
#define STATUS_IDLE         BIT(3)
#define STATUS_REPLAY_EMPTY BIT(4)
#define STATUS_STALL_NOT    BIT(31)
#define MMU_COMMAND         0x08
#define CMD_ENABLE_PAGING   0
#define CMD_DISABLE_PAGING  1
#define CMD_ENABLE_STALL    2
#define CMD_DISABLE_STALL   3
#define CMD_ZAP_CACHE       4
#define CMD_PAGE_FAULT_DONE 5
#define CMD_FORCE_RESET     6
#define MMU_PAGE_FAULT_ADDR 0x0c
#define MMU_ZAP_ONE_LINE    0x10
#define MMU_INT_RAWSTAT     0x14
#define MMU_INT_CLEAR       0x18
#define MMU_INT_MASK        0x1c
#define MMU_INT_STATUS      0x20
#define MMU_AUTO_GATING     0x24
#define MMU_BANK_SIZE       0x40

/* v2's encoding of a 40-bit address in 32 bits, undone */
static hwaddr dec(uint32_t x)
{
    return (x & 0xfffff000u) | (uint64_t)(x & 0xf00) << 24 |
           (uint64_t)(x & 0xf0) << 32;
}

bool rk_iommu_translate(RkIommuState *s, uint32_t iova, hwaddr *pa)
{
    RkIommuBank *b = &s->bank[0];
    uint32_t dte, pte;

    if (!b->paging) {
        *pa = iova;
        return true;
    }
    dte = address_space_ldl_le(&address_space_memory,
                               dec(b->dte) + 4 * (iova >> 22),
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(dte & 1)) {
        return false;
    }
    pte = address_space_ldl_le(&address_space_memory,
                               dec(dte) + 4 * ((iova >> 12) & 0x3ff),
                               MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(pte & 1)) {
        return false;
    }
    *pa = dec(pte) + (iova & 0xfff);
    return true;
}

static void rk_iommu_irq_update(RkIommuState *s)
{
    bool level = false;

    for (int i = 0; i < s->nbanks; i++) {
        level |= (s->bank[i].int_raw & s->bank[i].int_mask) != 0;
    }
    qemu_set_irq(s->irq, level);
}

static uint64_t rk_iommu_read(void *opaque, hwaddr off, unsigned size)
{
    RkIommuBank *b = opaque;

    switch (off) {
    case MMU_DTE_ADDR:
        return b->dte;
    case MMU_STATUS:
        return (b->paging ? STATUS_PAGING : 0) |
               (b->stall ? STATUS_STALL : STATUS_STALL_NOT) |
               STATUS_IDLE | STATUS_REPLAY_EMPTY;
    case MMU_INT_RAWSTAT:
        return b->int_raw;
    case MMU_INT_MASK:
        return b->int_mask;
    case MMU_INT_STATUS:
        return b->int_raw & b->int_mask;
    case MMU_AUTO_GATING:
        return b->auto_gating;
    default:                            /* (PAGE_FAULT_ADDR: none) */
        return 0;
    }
}

static void rk_iommu_write(void *opaque, hwaddr off, uint64_t v,
                           unsigned size)
{
    RkIommuBank *b = opaque;

    switch (off) {
    case MMU_DTE_ADDR:
        b->dte = v;
        break;
    case MMU_COMMAND:
        switch (v) {
        case CMD_ENABLE_PAGING:
            b->paging = true;
            break;
        case CMD_DISABLE_PAGING:
            b->paging = false;
            break;
        case CMD_ENABLE_STALL:
            b->stall = true;
            break;
        case CMD_DISABLE_STALL:
            b->stall = false;
            break;
        case CMD_FORCE_RESET:
            b->dte = 0;
            b->paging = b->stall = false;
            b->int_raw = 0;
            break;
        default:                        /* (no cache to zap, no fault) */
            break;
        }
        break;
    case MMU_INT_CLEAR:
        b->int_raw &= ~v;
        break;
    case MMU_INT_MASK:
        b->int_mask = v & 3;
        break;
    case MMU_AUTO_GATING:
        b->auto_gating = v;
        break;
    }
    rk_iommu_irq_update(b->s);
}

static const MemoryRegionOps rk_iommu_ops = {
    .read = rk_iommu_read,
    .write = rk_iommu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk_iommu_reset(DeviceState *dev)
{
    RkIommuState *s = RK_IOMMU(dev);

    for (int i = 0; i < RK_IOMMU_MAX_BANKS; i++) {
        s->bank[i].dte = 0;
        s->bank[i].paging = s->bank[i].stall = false;
        s->bank[i].int_raw = s->bank[i].int_mask = 0;
        s->bank[i].auto_gating = BIT(31);
    }
    qemu_set_irq(s->irq, 0);
}

static void rk_iommu_realize(DeviceState *dev, Error **errp)
{
    RkIommuState *s = RK_IOMMU(dev);

    if (!s->nbanks || s->nbanks > RK_IOMMU_MAX_BANKS) {
        error_setg(errp, "rk-iommu: 1 to %d banks", RK_IOMMU_MAX_BANKS);
        return;
    }
    for (int i = 0; i < s->nbanks; i++) {
        s->bank[i].s = s;
        memory_region_init_io(&s->mr[i], OBJECT(s), &rk_iommu_ops,
                              &s->bank[i], "rk-iommu", MMU_BANK_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mr[i]);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static const Property rk_iommu_props[] = {
    DEFINE_PROP_UINT32("banks", RkIommuState, nbanks, 1),
};

static void rk_iommu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Rockchip IOMMU (v2)";
    dc->realize = rk_iommu_realize;
    device_class_set_legacy_reset(dc, rk_iommu_reset);
    device_class_set_props(dc, rk_iommu_props);
}

static const TypeInfo rk_iommu_types[] = {
    {
        .name = TYPE_RK_IOMMU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkIommuState),
        .class_init = rk_iommu_class_init,
    },
};

DEFINE_TYPES(rk_iommu_types)
