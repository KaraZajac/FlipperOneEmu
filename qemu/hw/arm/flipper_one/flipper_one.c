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
#include "qapi/visitor.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/machines-qom.h"
#include "chardev/char.h"
#include "flipper_one.h"
#include "rp2350.h"

#define TYPE_FLIPPER_ONE MACHINE_TYPE_NAME("flipper-one")
OBJECT_DECLARE_SIMPLE_TYPE(FlipperOneState, FLIPPER_ONE)

struct FlipperOneState {
    MachineState parent_obj;

    RP2350State mcu;
    Clock *mcu_clk;
    FlipperOnePanelState *panel;
    FoJd9853 display;
    I2CSlave *buttons;          /* the control bus's PCAL6416 */
    I2CSlave *touchpad;         /* ... IQS7211E */
    I2CSlave *haptic;           /* ... DRV2605L */
    FoI2cPins main_bus;         /* the main bus: a PIO program's, GPIO22/23 */
    I2CSlave *main_expander;    /* ... its PCAL6416 */
    FoBattery battery;
    FoLeds leds;                /* the WS2812s, GPIO29-31 */
    FoHost *host;               /* a program in the RK3576's place */
    CharFrontend log;
    char *flash;
    char *log_id;
    bool bootsel;
};

/*
 * The front panel's buttons, on the expander's pins (pressed: low), and its
 * finger, on the touchpad
 */
static void flipper_one_input(void *opaque)
{
    FlipperOneState *s = opaque;
    FlipperOnePanelState *p = s->panel;

    pcal6416_set_pins(s->buttons, ~p->keys & 0xffff);
    iqs7211e_touch(s->touchpad, p->touching, p->touch_x, p->touch_y,
                   p->touch_p);
}

/* the expanders' INT and the touchpad's RDY: open drain, the board's
 * pull-ups on GPIO46, GPIO39 and GPIO9 */
static void flipper_one_buttons_int(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    rp2350_pin_drive(&s->mcu, 46, level ? -1 : 0);
}

static void flipper_one_main_int(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    rp2350_pin_drive(&s->mcu, 39, level ? -1 : 0);
}

/* CPU_INT (GPIO35, open drain, the board's pull-up): to the host link */
static void flipper_one_cpu_int(void *opaque, int pin, bool level,
                                int64_t when_ns)
{
    FlipperOneState *s = opaque;

    fo_host_int(s->host, level);
}

/* the LED chains' supplies: the control expander's pins 15, 14, 13 */
static void flipper_one_led_power(void *opaque, int chain, int level)
{
    FlipperOneState *s = opaque;

    fo_leds_power(&s->leds, chain, level);
}

/*
 * The Linux side's rail (VCC5V0_SYS_S5, the main expander's pin 12): what
 * the board draws from the battery, assumed: 40 mA with it off (the MCU, the
 * screen), 400 mA with it on (about 3 W)
 */
static void flipper_one_host_power(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    s->battery.load_ma = level ? 400 : 40;
}

/* the expanders' RESET: GPIO43 (the control bus's), GPIO38 (the main
 * board's), open drain with the board's pull-ups */
static void flipper_one_expander_reset(void *opaque, int pin, bool level,
                                       int64_t when_ns)
{
    FlipperOneState *s = opaque;
    I2CSlave *dev = pin == 43 ? s->buttons : s->main_expander;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(dev), 0), level);
}

static void flipper_one_touch_rdy(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    rp2350_pin_drive(&s->mcu, 9, level ? -1 : 0);
}

/* the haptic driver: its EN is GPIO7; its motor, the window's to show */
static void flipper_one_haptic_en(void *opaque, int pin, bool level,
                                  int64_t when_ns)
{
    FlipperOneState *s = opaque;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->haptic), 0), level);
}

static void flipper_one_haptic(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    fo_panel_haptic(s->panel, level);
}

/* the screen's backlight: GPIO6's PWM (or its level, as a plain pin) */
static void flipper_one_backlight(void *opaque)
{
    FlipperOneState *s = opaque;
    int duty = rp2350_pwm_duty(&s->mcu, 6);

    if (duty < 0) {
        duty = (s->mcu.pin_level >> 6) & 1 ? 65535 : 0;
    }
    fo_panel_backlight(s->panel, (duty * 255 + 32767) / 65535);
}

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

    /* the firmware's log UART: a PIO program's, on GPIO41 at 1.5 Mbaud */
    if (s->log_id) {
        Chardev *chr = qemu_chr_find(s->log_id);

        if (!chr) {
            error_report("flipper-one: log: no chardev '%s'", s->log_id);
            exit(1);
        }
        qemu_chr_fe_init(&s->log, chr, &error_fatal);
        rp2350_pin_uart(&s->mcu, 41, 1500000, &s->log);
    }

    /* the front panel: -global flipper-one-panel.link=CHARDEV for a window */
    s->panel = FLIPPER_ONE_PANEL(qdev_new(TYPE_FLIPPER_ONE_PANEL));
    object_property_add_child(OBJECT(ms), "panel", OBJECT(s->panel));
    qdev_realize_and_unref(DEVICE(s->panel), NULL, &error_fatal);

    /* the RK3576's side of its link to the MCU, for a program until it is
     * emulated: -global flipper-one-host.link=CHARDEV (I2C1 at 0x69, CPU_INT
     * on GPIO35) */
    s->host = FLIPPER_ONE_HOST(qdev_new(TYPE_FLIPPER_ONE_HOST));
    s->host->soc = &s->mcu;
    s->host->i2c = 1;
    s->host->addr = 0x69;
    object_property_add_child(OBJECT(ms), "host", OBJECT(s->host));
    qdev_realize_and_unref(DEVICE(s->host), NULL, &error_fatal);
    s->mcu.ext_pullup |= 1ull << 35;
    rp2350_pin_listen(&s->mcu, 35, flipper_one_cpu_int, s);

    /* the screen: its controller on HSTX, its backlight on GPIO6's PWM */
    fo_jd9853_init(&s->display, &s->mcu, s->panel);
    s->mcu.pwm_fn = flipper_one_backlight;

    /* the control bus (I2C0, GPIO20/21): the buttons' expander at 0x20,
     * the screen's supply at 0x47, the touchpad's controller at 0x56, the
     * haptic driver at 0x5A */
    s->mcu.ext_pullup |= (1ull << 46) | (1ull << 9);
    i2c_slave_create_simple(rp2350_i2c_bus(&s->mcu, 0), "tps62868x", 0x47);
    s->haptic = i2c_slave_create_simple(rp2350_i2c_bus(&s->mcu, 0),
                                        "drv2605l", 0x5a);
    qdev_connect_gpio_out(DEVICE(s->haptic), 0,
                          qemu_allocate_irq(flipper_one_haptic, s, 0));
    rp2350_pin_listen(&s->mcu, 7, flipper_one_haptic_en, s);
    s->buttons = i2c_slave_create_simple(rp2350_i2c_bus(&s->mcu, 0),
                                         "pcal6416", 0x20);
    qdev_connect_gpio_out(DEVICE(s->buttons), 0,
                          qemu_allocate_irq(flipper_one_buttons_int, s, 0));
    fo_leds_init(&s->leds, &s->mcu, s->panel);
    for (int chain = 0; chain < FO_LED_CHAINS; chain++) {
        qdev_connect_gpio_out_named(DEVICE(s->buttons), "pins", 15 - chain,
            qemu_allocate_irq(flipper_one_led_power, s, chain));
    }
    s->touchpad = i2c_slave_create_simple(rp2350_i2c_bus(&s->mcu, 0),
                                          "iqs7211e", 0x56);
    qdev_connect_gpio_out(DEVICE(s->touchpad), 0,
                          qemu_allocate_irq(flipper_one_touch_rdy, s, 0));
    s->panel->input = flipper_one_input;
    s->panel->input_opaque = s;
    s->mcu.pwm_opaque = s;

    /* the main bus (a PIO program's I2C on GPIO22/23): the main board's
     * expander at 0x20 (its INT on GPIO39; its pin 12 the Linux side's
     * rail), the USB-C port's PD PHY at 0x22 and port controller at 0x47,
     * the system rail's monitor at 0x45, the battery's gauge at 0x55, its
     * charger at 0x6B */
    fo_i2c_pins_init(&s->main_bus, &s->mcu, 22, 23, "main-i2c");
    s->mcu.ext_pullup |= (1ull << 39) | (1ull << 38) | (1ull << 43);
    s->main_expander = i2c_slave_create_simple(s->main_bus.bus, "pcal6416",
                                               0x20);
    qdev_connect_gpio_out(DEVICE(s->main_expander), 0,
                          qemu_allocate_irq(flipper_one_main_int, s, 0));
    qdev_connect_gpio_out_named(DEVICE(s->main_expander), "pins", 12,
                                qemu_allocate_irq(flipper_one_host_power, s,
                                                  0));
    s->battery.load_ma = 40;
    bq28z620_attach(i2c_slave_create_simple(s->main_bus.bus, "bq28z620",
                                            0x55), &s->battery);
    bq2579x_attach(i2c_slave_create_simple(s->main_bus.bus, "bq2579x", 0x6b),
                   &s->battery);
    ina219_attach(i2c_slave_create_simple(s->main_bus.bus, "ina219", 0x45),
                  &s->battery);
    i2c_slave_create_simple(s->main_bus.bus, "fusb302", 0x22);
    i2c_slave_create_simple(s->main_bus.bus, "hd3ss3220", 0x47);
    rp2350_pin_listen(&s->mcu, 43, flipper_one_expander_reset, s);
    rp2350_pin_listen(&s->mcu, 38, flipper_one_expander_reset, s);
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

static char *flipper_one_get_log(Object *obj, Error **errp)
{
    return g_strdup(FLIPPER_ONE(obj)->log_id);
}

static void flipper_one_set_log(Object *obj, const char *value, Error **errp)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    g_free(s->log_id);
    s->log_id = g_strdup(value);
}

static void flipper_one_get_battery(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    int32_t value = FLIPPER_ONE(obj)->battery.percent;

    visit_type_int32(v, name, &value, errp);
}

static void flipper_one_set_battery(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < 0 || value > 100) {
        error_setg(errp, "battery: a charge from 0 to 100 (%%)");
        return;
    }
    FLIPPER_ONE(obj)->battery.percent = value;
}

static void flipper_one_instance_init(Object *obj)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    s->battery.percent = 80;
    s->battery.design_mah = 3000;
    s->battery.temp_dk = 2982;                  /* 25 °C */
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
    /* the RP2350's two cores (TCG gives a thread to as many as this says) */
    mc->default_cpus = 2;
    mc->min_cpus = 2;
    mc->max_cpus = 2;
    object_class_property_add_str(oc, "flash", flipper_one_get_flash,
                                  flipper_one_set_flash);
    object_class_property_set_description(oc, "flash",
        "The image of the MCU's 16 MiB QSPI flash (raw; scripts/uf2.py "
        "writes one from a recovery .uf2)");
    object_class_property_add_str(oc, "log", flipper_one_get_log,
                                  flipper_one_set_log);
    object_class_property_set_description(oc, "log",
        "Chardev id that gets the MCU firmware's log (its log UART, a PIO "
        "program on GPIO41 at 1.5 Mbaud)");
    object_class_property_add_bool(oc, "bootsel", flipper_one_get_bootsel,
                                   flipper_one_set_bootsel);
    object_class_property_set_description(oc, "bootsel",
        "Start with the RP2350's BOOTSEL held: its boot ROM's USB mode");
    object_class_property_add(oc, "battery", "int", flipper_one_get_battery,
                              flipper_one_set_battery, NULL, NULL);
    object_class_property_set_description(oc, "battery",
        "The battery's charge, 0 to 100 (%); 80 unless told");
}

static const TypeInfo flipper_one_types[] = {
    {
        .name = TYPE_FLIPPER_ONE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(FlipperOneState),
        .instance_init = flipper_one_instance_init,
        .class_init = flipper_one_class_init,
        .interfaces = aarch64_machine_interfaces,
    },
};
DEFINE_TYPES(flipper_one_types)
