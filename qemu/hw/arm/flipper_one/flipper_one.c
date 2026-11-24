/*
 * Flipper One machine
 *
 *   qemu-system-aarch64 -M flipper-one,flash=FLASH.bin -bios BOOTROM.bin
 *
 * So far its MCU, a Raspberry Pi RP2350B, with Raspberry Pi's boot ROM
 * (-bios: bootrom-combined.bin, 32 KiB) and the image of its 16 MiB QSPI
 * flash (flash=: a raw image, as scripts/uf2.py writes it from a
 * recovery .uf2: partition table and firmware).  The RK3576 comes later.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/arm/machines-qom.h"
#include "rp2350.h"

#define TYPE_FLIPPER_ONE MACHINE_TYPE_NAME("flipper-one")
OBJECT_DECLARE_SIMPLE_TYPE(FlipperOneState, FLIPPER_ONE)

struct FlipperOneState {
    MachineState parent_obj;

    RP2350State mcu;
    Clock *mcu_clk;
    char *flash;
    bool bootsel;
};

static void flipper_one_init(MachineState *ms)
{
    FlipperOneState *s = FLIPPER_ONE(ms);
    DeviceState *mcu;

    if (!ms->firmware) {
        error_report("flipper-one: -bios FILE is the RP2350's boot ROM "
                     "(bootrom-combined.bin, 32 KiB)");
        exit(1);
    }
    /* the MCU's system clock: 150 MHz, as its firmware sets it */
    s->mcu_clk = clock_new(OBJECT(ms), "mcu-clk");
    clock_set_hz(s->mcu_clk, 150 * 1000 * 1000);

    object_initialize_child(OBJECT(ms), "mcu", &s->mcu, TYPE_RP2350);
    mcu = DEVICE(&s->mcu);
    qdev_prop_set_string(mcu, "bootrom", ms->firmware);
    if (s->flash) {
        qdev_prop_set_string(mcu, "flash", s->flash);
    }
    qdev_prop_set_bit(mcu, "bootsel", s->bootsel);
    qdev_connect_clock_in(mcu, "sysclk", s->mcu_clk);
    sysbus_realize(SYS_BUS_DEVICE(mcu), &error_fatal);
}

static char *flipper_one_get_flash(Object *obj, Error **errp)
{
    return g_strdup(FLIPPER_ONE(obj)->flash);
}

static void flipper_one_set_flash(Object *obj, const char *value, Error **errp)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    g_free(s->flash);
    s->flash = g_strdup(value);
}

static bool flipper_one_get_bootsel(Object *obj, Error **errp)
{
    return FLIPPER_ONE(obj)->bootsel;
}

static void flipper_one_set_bootsel(Object *obj, bool value, Error **errp)
{
    FLIPPER_ONE(obj)->bootsel = value;
}

static void flipper_one_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Flipper One (so far its MCU, a Raspberry Pi RP2350B)";
    mc->init = flipper_one_init;
    mc->default_ram_size = 0;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    object_class_property_add_str(oc, "flash", flipper_one_get_flash,
                                  flipper_one_set_flash);
    object_class_property_set_description(oc, "flash",
        "The image of the MCU's 16 MiB QSPI flash (raw; scripts/uf2.py "
        "writes one from a recovery .uf2)");
    object_class_property_add_bool(oc, "bootsel", flipper_one_get_bootsel,
                                   flipper_one_set_bootsel);
    object_class_property_set_description(oc, "bootsel",
        "Start with the RP2350's BOOTSEL held: its boot ROM's USB mode");
}

static const TypeInfo flipper_one_types[] = {
    {
        .name = TYPE_FLIPPER_ONE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(FlipperOneState),
        .class_init = flipper_one_class_init,
        .interfaces = aarch64_machine_interfaces,
    },
};
DEFINE_TYPES(flipper_one_types)
