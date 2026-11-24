/*
 * Raspberry Pi RP2350 (the Flipper One's MCU, an RP2350B)
 *
 * Its own bus, not QEMU's system memory: the RK3576 beside it on the
 * Flipper One has another map (DRAM where the RP2350 has its peripherals).
 *
 *   0x00000000  boot ROM, 32 KiB (Raspberry Pi's image, -bios)
 *   0x10000000  XIP flash, 16 MiB; 0x14000000 uncached, 0x1c000000
 *               untranslated (the same flash, as yet)
 *   0x13ffc000  XIP cache as SRAM, 16 KiB
 *   0x20000000  SRAM, 520 KiB (SRAM0-7 striped, SRAM8/9 = scratch X/Y)
 *   0x40000000  APB peripherals, 0x50000000 AHB, 0xd0000000 SIO
 *   0xe0000000  the Cortex-M33's PPB (QEMU's armv7m)
 *
 * What is not modelled yet is an unimplemented device: -d unimp logs it.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/tcg/idau.h"
#include "rp2350.h"

/*
 * The RP2350's IDAU, fixed in the chip.  Its map of the boot ROM is in the
 * ROM's arm8_bootrom_rt0.S:
 *   0x0000-0x42ff  Exempt: text that both states run
 *   0x4300-0x7dff  Secure for instruction fetches, Exempt for data; QEMU's
 *                  IDAU knows no access type, so Exempt
 *   0x7e00-0x7fff  Secure, NonSecure-callable: the SG entry points (IDAU
 *                  region 2, which the ROM checks with TT at its start)
 * The USB controller's RAM is Exempt too (the ROM's nsboot), and so is the
 * PPB.  The rest is NonSecure to the IDAU: the SAU decides.
 */
static void rp2350_idau_check(IDAUInterface *ii, uint32_t address,
                              int *iregion, bool *exempt, bool *ns, bool *nsc)
{
    *iregion = IREGION_NOTVALID;
    *exempt = false;
    *ns = true;
    *nsc = false;
    if (address < 0x7e00) {
        *exempt = true;
    } else if (address < 0x8000) {
        *ns = false;
        *nsc = true;
        *iregion = 2;
    } else if ((address & 0xfffff000) == 0x50100000 ||
               (address & 0xfff00000) == 0xe0000000) {
        *exempt = true;
    }
}

static const struct {
    const char *name;
    hwaddr base, size;
} rp2350_unimp[] = {
    { "sysinfo",        0x40000000, 0x8000 },
    { "syscfg",         0x40008000, 0x8000 },
    { "clocks",         0x40010000, 0x8000 },
    { "psm",            0x40018000, 0x8000 },
    { "resets",         0x40020000, 0x8000 },
    { "io_bank0",       0x40028000, 0x8000 },
    { "io_qspi",        0x40030000, 0x8000 },
    { "pads_bank0",     0x40038000, 0x8000 },
    { "pads_qspi",      0x40040000, 0x8000 },
    { "xosc",           0x40048000, 0x8000 },
    { "pll_sys",        0x40050000, 0x8000 },
    { "pll_usb",        0x40058000, 0x8000 },
    { "accessctrl",     0x40060000, 0x8000 },
    { "busctrl",        0x40068000, 0x8000 },
    { "uart0",          0x40070000, 0x8000 },
    { "uart1",          0x40078000, 0x8000 },
    { "spi0",           0x40080000, 0x8000 },
    { "spi1",           0x40088000, 0x8000 },
    { "i2c0",           0x40090000, 0x8000 },
    { "i2c1",           0x40098000, 0x8000 },
    { "adc",            0x400a0000, 0x8000 },
    { "pwm",            0x400a8000, 0x8000 },
    { "timer0",         0x400b0000, 0x8000 },
    { "timer1",         0x400b8000, 0x8000 },
    { "hstx_ctrl",      0x400c0000, 0x8000 },
    { "xip_ctrl",       0x400c8000, 0x8000 },
    { "xip_qmi",        0x400d0000, 0x8000 },
    { "watchdog",       0x400d8000, 0x8000 },
    { "bootram.regs",   0x400e0400, 0x7c00 },
    { "rosc",           0x400e8000, 0x8000 },
    { "trng",           0x400f0000, 0x8000 },
    { "sha256",         0x400f8000, 0x8000 },
    { "powman",         0x40100000, 0x8000 },
    { "ticks",          0x40108000, 0x8000 },
    { "otp",            0x40120000, 0x10000 },
    { "otp_data",       0x40130000, 0x10000 },
    { "coresight",      0x40140000, 0x10000 },
    { "dft",            0x40150000, 0x8000 },
    { "glitch_detector", 0x40158000, 0x8000 },
    { "tbman",          0x40160000, 0x8000 },
    { "dma",            0x50000000, 0x100000 },
    { "usbctrl",        0x50100000, 0x100000 },
    { "pio0",           0x50200000, 0x100000 },
    { "pio1",           0x50300000, 0x100000 },
    { "pio2",           0x50400000, 0x100000 },
    { "xip_aux",        0x50500000, 0x100000 },
    { "hstx_fifo",      0x50600000, 0x100000 },
    { "coresight_trace", 0x50700000, 0x100000 },
    { "xip_maintenance", RP2350_XIP_MAINT_BASE, 0x4000000 },
    { "sio",            RP2350_SIO_BASE, 0x40000 },
};

/* an unimplemented device on the RP2350's own bus, under what is modelled */
static void rp2350_add_unimp(RP2350State *s, const char *name, hwaddr base,
                             hwaddr size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion_overlap(&s->bus, base,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), -1000);
}

/* a file's bytes into a ROM's memory: no more than @size */
static bool rp2350_load(MemoryRegion *mr, const char *path, hwaddr size,
                        const char *what, Error **errp)
{
    g_autofree char *data = NULL;
    gsize len;
    GError *err = NULL;

    if (!g_file_get_contents(path, &data, &len, &err)) {
        error_setg(errp, "rp2350: %s: %s", what, err->message);
        g_error_free(err);
        return false;
    }
    if (len > size) {
        error_setg(errp, "rp2350: %s %s is %zu bytes, more than %" HWADDR_PRIu,
                   what, path, (size_t)len, size);
        return false;
    }
    memcpy(memory_region_get_ram_ptr(mr), data, len);
    memory_region_set_dirty(mr, 0, len);
    return true;
}

static void rp2350_init(Object *obj)
{
    RP2350State *s = RP2350(obj);

    object_initialize_child(obj, "cpu0", &s->cpu[0], TYPE_ARMV7M);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk", NULL, NULL, 0);
}

static void rp2350_realize(DeviceState *dev, Error **errp)
{
    RP2350State *s = RP2350(dev);
    Object *obj = OBJECT(dev);
    DeviceState *armv7m = DEVICE(&s->cpu[0]);

    if (!s->bootrom) {
        error_setg(errp, "rp2350: no boot ROM (bootrom=FILE, 32 KiB)");
        return;
    }
    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "rp2350: sysclk must be connected");
        return;
    }
    memory_region_init(&s->bus, obj, "rp2350.bus", 4 * GiB);

    if (!memory_region_init_rom(&s->rom, obj, "rp2350.rom", RP2350_ROM_SIZE,
                                errp) ||
        !rp2350_load(&s->rom, s->bootrom, RP2350_ROM_SIZE, "boot ROM", errp)) {
        return;
    }
    memory_region_add_subregion(&s->bus, RP2350_ROM_BASE, &s->rom);

    /* the flash: erased, then the image (its first bytes) */
    if (!memory_region_init_rom(&s->xip, obj, "rp2350.flash",
                                RP2350_FLASH_SIZE, errp)) {
        return;
    }
    memset(memory_region_get_ram_ptr(&s->xip), 0xff, RP2350_FLASH_SIZE);
    if (s->flash &&
        !rp2350_load(&s->xip, s->flash, RP2350_FLASH_SIZE, "flash", errp)) {
        return;
    }
    memory_region_add_subregion(&s->bus, RP2350_XIP_BASE, &s->xip);
    memory_region_init_alias(&s->xip_nocache, obj, "rp2350.xip-nocache",
                             &s->xip, 0, RP2350_FLASH_SIZE);
    memory_region_add_subregion(&s->bus, RP2350_XIP_NOCACHE_BASE,
                                &s->xip_nocache);
    memory_region_init_alias(&s->xip_notrans, obj, "rp2350.xip-notranslate",
                             &s->xip, 0, RP2350_FLASH_SIZE);
    memory_region_add_subregion(&s->bus, RP2350_XIP_NOTRANS_BASE,
                                &s->xip_notrans);

    /* the boot ROM works in the USB controller's RAM too */
    if (!memory_region_init_ram(&s->xip_sram, obj, "rp2350.xip-sram",
                                RP2350_XIP_SRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->sram, obj, "rp2350.sram",
                                RP2350_SRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->bootram, obj, "rp2350.bootram",
                                RP2350_BOOTRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->usb_dpram, obj, "rp2350.usb-dpram",
                                4 * KiB, errp)) {
        return;
    }
    memory_region_add_subregion(&s->bus, RP2350_XIP_SRAM_BASE, &s->xip_sram);
    memory_region_add_subregion(&s->bus, RP2350_SRAM_BASE, &s->sram);
    memory_region_add_subregion(&s->bus, RP2350_BOOTRAM_BASE, &s->bootram);
    memory_region_add_subregion(&s->bus, 0x50100000, &s->usb_dpram);

    for (int i = 0; i < ARRAY_SIZE(rp2350_unimp); i++) {
        rp2350_add_unimp(s, rp2350_unimp[i].name, rp2350_unimp[i].base,
                         rp2350_unimp[i].size);
    }
    rp2350_sys_realize(s);
    rp2350_sio_realize(s);

    /*
     * core 0: Cortex-M33 with FPU, DSP and TrustZone; 4 priority bits, 8 MPU
     * regions in each security state, 8 SAU regions
     */
    qdev_prop_set_uint32(armv7m, "num-irq", RP2350_NUM_IRQ);
    qdev_prop_set_uint32(armv7m, "num-prio-bits", 4);
    qdev_prop_set_uint32(armv7m, "mpu-s-regions", 8);
    qdev_prop_set_uint32(armv7m, "mpu-ns-regions", 8);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(armv7m, "init-svtor", RP2350_ROM_BASE);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(armv7m), "memory", OBJECT(&s->bus),
                             &error_abort);
    object_property_set_link(OBJECT(armv7m), "idau", OBJECT(s), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(armv7m), errp)) {
        return;
    }
    rp2350_coproc_init(s, 0);
}

static void rp2350_reset_hold(Object *obj, ResetType type)
{
    rp2350_coproc_reset(RP2350(obj));
    rp2350_sys_reset(RP2350(obj));
    rp2350_sio_reset(RP2350(obj));
}

static const Property rp2350_properties[] = {
    DEFINE_PROP_STRING("bootrom", RP2350State, bootrom),
    DEFINE_PROP_STRING("flash", RP2350State, flash),
    DEFINE_PROP_BOOL("bootsel", RP2350State, bootsel, false),
};

static void rp2350_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    IDAUInterfaceClass *iic = IDAU_INTERFACE_CLASS(oc);

    dc->realize = rp2350_realize;
    device_class_set_props(dc, rp2350_properties);
    rc->phases.hold = rp2350_reset_hold;
    iic->check = rp2350_idau_check;
}

static const TypeInfo rp2350_types[] = {
    {
        .name = TYPE_RP2350,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350State),
        .instance_init = rp2350_init,
        .class_init = rp2350_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_IDAU_INTERFACE },
            { }
        },
    },
};
DEFINE_TYPES(rp2350_types)
