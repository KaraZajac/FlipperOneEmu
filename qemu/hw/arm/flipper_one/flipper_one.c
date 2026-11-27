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
#include "qemu/units.h"
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
#include "system/runstate.h"
#include "system/reset.h"
#include "system/blockdev.h"
#include "system/block-backend.h"
#include "hw/core/qdev-properties-system.h"
#include "flipper_one.h"
#include "rp2350.h"
#include "rk3576.h"
#include "hw/arm/boot.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "target/arm/internals.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/multiprocessing.h"
#include <libfdt.h>
#include "qemu/guest-random.h"
#include "hw/usb/usb.h"
#include <zstd.h>

#define TYPE_FLIPPER_ONE MACHINE_TYPE_NAME("flipper-one")
OBJECT_DECLARE_SIMPLE_TYPE(FlipperOneState, FLIPPER_ONE)

struct FlipperOneState {
    MachineState parent_obj;

    RP2350State mcu;
    RK3576State rk3576;         /* the CPU: Linux's */
    RkI2cTarget link;           /* its I2C0's target: the MCU's I2C1 */
    bool linux_boot;            /* a kernel to start on it (-kernel), or
                                   U-Boot: */
    bool uboot;                 /* ... U-Boot proper from its microSD card,
                                   the machine as SPL and TF-A (property) */
    bool deep_idle;             /* the kernel's tree keeps its CPUs' power-
                                   down idle states (property) */
    char *rev;                  /* the board's revision (property) */
    char *falcon;               /* ... or a FIT for SPL's Falcon mode, as
                                   the Flipper OS installer is (property) */
    uint8_t *falcon_fit;        /* ... its bytes, read once */
    bool falcon_done;           /* ... started once: as rockusb's download
                                   into the MaskROM, which a reset ends */
    gsize falcon_len;
    hwaddr entry_x0;            /* x0 for what the RK3576 starts at */
    BlockBackend *sd_blk;       /* the microSD card's image */
    BlockBackend *boot_blk;     /* ... or the UFS's boot LU: what the boot
                                   ROM boots this time (fo_spl) */
    bool rk_rail;               /* VCC5V0_SYS_S5: its supply, the MCU's */
    bool rk_on;                 /* ... it runs */
    int rk_want;                /* RK_...: to do, in rk_power_bh */
    bool rk_lost;               /* ... and its power went, meanwhile */
    QEMUBH *rk_power_bh;
    struct {
        hwaddr addr;
        size_t len;
        uint8_t *data;
    } rk_images[4];             /* the boot stub, kernel, initrd, DTB, as
                                   loaded (saved at its first start) */
    int rk_nimages;
    FoHostSpi linux_frames;     /* its frames, as they go to the MCU */
    GByteArray *frame;          /* ... one coming in over its SPI0 */
    bool frame_active;          /* ... between "active" falling and rising */
    uint64_t frames_sent, frames_dropped, frames_same;
    GByteArray *last_frame;     /* ... the last one sent, and when */
    int64_t last_frame_ms;
    struct arm_boot_info binfo; /* ... a kernel started on it directly */
    Clock *mcu_clk;
    FlipperOnePanelState *panel;
    FoJd9853 display;
    I2CSlave *buttons;          /* the control bus's PCAL6416 */
    I2CSlave *touchpad;         /* ... IQS7211E */
    I2CSlave *haptic;           /* ... DRV2605L */
    FoI2cPins main_bus;         /* the main bus: a PIO program's, GPIO22/23 */
    I2CSlave *main_expander;    /* ... its PCAL6416 */
    uint16_t main_inputs;       /* ... the levels at its inputs */
    I2CSlave *charger;          /* ... the BQ25792 */
    I2CSlave *usb_pd;           /* ... the USB-C port's FUSB302 */
    I2CSlave *usb_mux;          /* ... and its HD3SS3220 */
    int usb;                    /* in the port: 0 nothing, 1 a charger, 2 a
                                   computer */
    bool usb20_sel;             /* the USB mux: the RK3576's, not the MCU's */
    QEMUTimer *expander_reset[2];   /* RESET low long enough: the control
                                       bus's expander, the main one's */
    bool maskrom_strap;         /* MASKROM_EN (BOOT0, the main expander's pin
                                   15): SARADC_VIN0_BOOT pulled to ground */
    bool in_maskrom;            /* ... the RK3576 started with it, or had
                                   nothing to boot: its boot ROM waits for
                                   a USB download (rk3576_maskrom.c) */
    bool dram_up;               /* ... which has run the DDR blob */
    int headset;                /* in the jack: FO_HEADSET_... */
    DeviceState *codec;         /* ... its mic on the NAU8822's RMICP */
    struct USBDevice *mt7921;   /* Wi-Fi and Bluetooth, on USB1 */
    bool hdmi;                  /* a monitor in the HDMI port */
    bool hdmi_at_start;         /* ... from the start (property) */
    int ambient_dc;             /* around the device: 0.1 °C */
    bool off;                   /* the board's supply cut (the charger's
                                   ship FET) */
    QEMUTimer *power_timer;     /* ... back, after a system power reset */
    DeviceState *usb_host;      /* the computer, for the MCU's USB */
    FoBattery battery;
    FoLeds leds;                /* the WS2812s, GPIO29-31 */
    FoHost *host;               /* a program in the RK3576's place */
    CharFrontend log;
    char *flash;
    char *log_id;
    bool bootsel;
};

#define FO_REV_DEFAULT  "f0b0c1"

/*
 * The front panel's buttons, on the expander's pins (pressed: low), and its
 * finger, on the touchpad
 */
static void flipper_one_usb(FlipperOneState *s, int what);
static void flipper_one_headset(FlipperOneState *s, int what);
static void flipper_one_ambient(FlipperOneState *s, int dc);

static void flipper_one_input(void *opaque)
{
    FlipperOneState *s = opaque;
    FlipperOnePanelState *p = s->panel;

    pcal6416_set_pins(s->buttons, ~p->keys & 0xffff);
    iqs7211e_touch(s->touchpad, p->touching, p->touch_x, p->touch_y,
                   p->touch_p);
    if (p->usb != s->usb) {
        flipper_one_usb(s, p->usb);
    }
    if (p->headset != s->headset) {
        flipper_one_headset(s, p->headset);
    }
    if (p->hdmi != s->hdmi) {
        s->hdmi = p->hdmi;
        rk3576_hdmi_plug(&s->rk3576, s->hdmi);
    }
    if (p->ambient_dc != s->ambient_dc) {
        flipper_one_ambient(s, p->ambient_dc);
    }
}

/*
 * The temperature around the device (the panel's `ambient`, 25 °C unless
 * told): the battery takes it at once (its gauge reads it, and the
 * charger's TS input), and the RK3576 warms or cools toward it, its own
 * warmth on top (rk3576_tsadc.c)
 */
static void flipper_one_ambient(FlipperOneState *s, int dc)
{
    s->ambient_dc = dc;
    s->battery.temp_dk = dc + 2732;
    s->rk3576.tsadc.ambient = dc / 10.0;
}

/*
 * The 3.5 mm jack: its switch on GPIO42 (pulled high with a plug in), the
 * mic line on GPIO47 (ADC input 7). A four-pole headset's mic is biased at
 * 2.2 V through 2.2 kΩ (the firmware's figures) and loads it as 6.8 kΩ
 * (assumed): 1.66 V, above the 1.5 V the firmware takes for a mic present.
 * A button on its cable puts its resistor across the mic, as the standard
 * has them: play 0 Ω (10, its switch and the cable's wires: the firmware
 * takes exactly nothing for no button), assistant 135 Ω, volume up 240 Ω,
 * down 470 Ω.
 */
static void flipper_one_headset(FlipperOneState *s, int what)
{
    static const int ohms[FO_HEADSET_NUM] = {
        [FO_HEADSET_IN] = 6800,             /* the mic alone */
        [FO_HEADSET_PLAY] = 10,             /* "0 Ω": the switch, the cable */
        [FO_HEADSET_ASSIST] = 135,
        [FO_HEADSET_VOLUP] = 240, [FO_HEADSET_VOLDOWN] = 470,
    };
    int r = what == FO_HEADSET_IN ? 6800 :     /* ... a button across it */
            ohms[what] * 6800 / (ohms[what] + 6800);

    s->headset = what;
    s->mcu.adc_mv[7] = what == FO_HEADSET_NONE ? 0 : 2200 * r / (r + 2200);
    /* the mic to the codec too, which a button held shorts or swamps */
    if (s->codec) {
        nau8822_headset_mic(s->codec, what == FO_HEADSET_IN);
    }
    /* the switch pulls the line up, through the MCU's resets */
    s->mcu.ext_pullup = deposit64(s->mcu.ext_pullup, 42, 1,
                                  what != FO_HEADSET_NONE);
    rp2350_pins_update(&s->mcu, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/* what a computer plugged in reaches: the USB mux (USB20_SEL, the main
 * expander's pin 8, which the MCU firmware raises with the Linux side's
 * rail) puts its USB 2 lines on the MCU's USB while low, on the RK3576's
 * USB0 (the Type-C port's controller) while high */
static void flipper_one_usb_data(FlipperOneState *s)
{
    if (s->usb != 2) {
        fo_usb_host_connect(s->usb_host, NULL, NULL);
    } else if (!s->usb20_sel) {
        fo_usb_host_connect(s->usb_host, &rp2350_usb_ops, &s->mcu);
    } else if (s->rk3576.maskrom.on) {          /* (its boot ROM's) */
        fo_usb_host_connect(s->usb_host, &rk_maskrom_usb_ops,
                            &s->rk3576.maskrom);
    } else {
        fo_usb_host_connect(s->usb_host, &rk_dwc3_usb_ops,
                            &s->rk3576.usb[0]);
    }
}

/*
 * The USB-C port: nothing, a charger, or a computer. The charger is a USB-C
 * one of 5 V that offers 3 A (its Rp) and shorts D+ to D- (a dedicated
 * charging port); the computer's port gives 5 V and 500 mA (the default
 * Rp, a standard port), and its data lines. The firmware negotiates no more
 * (its PD is not written yet), so 5 V is what there is.
 */
static void flipper_one_power(FlipperOneState *s, bool on);

static void flipper_one_usb(FlipperOneState *s, int what)
{
    int rp_ma = what == 1 ? 3000 : what == 2 ? 900 : 0;
    bool plugged = what && !s->usb;

    s->usb = what;
    bq2579x_input(s->charger, what ? 5000 : 0, what == 1 ? 3000 : 500,
                  what == 1);
    fusb302_attach(s->usb_pd, rp_ma);
    hd3ss3220_attach(s->usb_mux, rp_ma);
    rp2350_usb_vbus(&s->mcu, what != 0);
    flipper_one_usb_data(s);
    if (plugged && s->off) {
        flipper_one_power(s, true);     /* a source wakes the charger */
    }
}

/*
 * The board's supply, VSYS, through the charger's ship FET (SDRV_CTRL):
 * cut, the chips on it are as reset, the RP2350's cores held off, the
 * screen, its backlight and the LEDs dark, and the board draws nothing
 * from the battery; back, everything starts as at power-on. A system
 * power reset cuts it for 350 ms; shutdown and ship mode until a source
 * is plugged in (ship mode also wakes on QON pulled low: where the board
 * has QON is not known yet).
 */
static void flipper_one_power(FlipperOneState *s, bool on)
{
    if (on != s->off) {
        return;                         /* as it is */
    }
    s->off = !on;
    s->mcu.unpowered = !on;
    timer_del(s->power_timer);
    fo_battery_update(&s->battery);
    s->battery.load_ma = on ? 40 : 0;
    fo_battery_update(&s->battery);
    if (!on) {
        fo_jd9853_unpowered(&s->display);
        fo_panel_backlight(s->panel, 0);
        fo_panel_haptic(s->panel, false);
        for (int c = 0; c < FO_LED_CHAINS; c++) {
            fo_leds_power(&s->leds, c, false);
        }
    }
    fo_panel_power(s->panel, on);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void flipper_one_supply(void *opaque, int what)
{
    FlipperOneState *s = opaque;

    flipper_one_power(s, false);
    if (what == FO_SUPPLY_RESET) {
        timer_mod(s->power_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 350 * SCALE_MS);
    }
}

static void flipper_one_power_back(void *opaque)
{
    flipper_one_power(opaque, true);
}

/* MASKROM_EN (the docs' BOOT0): it pulls the RK3576's SARADC_VIN0_BOOT to
 * ground, which its boot ROM reads at reset: then it skips every boot
 * medium and waits in its USB download mode. The MCU's Maskrom cuts the
 * rail and raises it with this, which it lets go a second later */
static void flipper_one_maskrom(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    s->maskrom_strap = level;
}

/*
 * The device as the emulator starts it: in use, Flipper OS on, its MCU just
 * starting (as after a firmware update, or a watchdog's reset). An MCU
 * restart leaves the main expander as it was: it still drives the Linux
 * side's rails and the USB mux to it, and the firmware's start keeps them
 * (its "Host is powered" path). After the board's power is cut, it starts
 * cold instead: the enables pulled down, the Linux side off until it is
 * started (the Power tab's Start Flipper OS). So does a start with BOOTSEL
 * held, a power-on with the button down: the boot ROM's USB boot has the
 * USB-C port, which a mux left on the Linux side would keep from it
 */
#define MAIN_INPUTS     0x00ff      /* the main expander's pins 0-7 */
#define MAIN_HOST_ON    ((1 << 8) | (1 << 11) | (1 << 12))  /* USB20_SEL,
                                       VCC5V0_DEV_S0_EN, VCC5V0_SYS_S5_EN */

/*
 * What the RK3576's SARADC reads, on Rockchip's ladder of eleven codes
 * (0, 416, 816, 1231, 1658, 2048, 2437, 2862, 3279, 3680, 4095). Channel 0
 * is SARADC_VIN0_BOOT, which its boot ROM reads for its boot media's order:
 * the board's divider has UFS, then the microSD card, then USB (Rockchip's
 * config 8, 2862, the order the docs give), and MASKROM_EN (BOOT0) pulls it
 * to ground (config 1: USB, the MaskROM). Channel 1 is BOOT1, the MCU's PWM
 * on GPIO4 (12 bits, 2 kHz) through a filter: the firmware's levels are the
 * ladder's codes, so the code is its duty. Channel 2 is the board's
 * revision, a divider U-Boot reads (0-208 f0b0c1, 209-616 f0b1c2, the
 * halfway points between codes): 0 and 416. The others: 0.
 */
#define SARADC_BOOT_ORDER   2862

static int flipper_one_saradc(void *opaque, int ch)
{
    FlipperOneState *s = opaque;
    int duty;

    switch (ch) {
    case 0:
        return s->maskrom_strap ? 0 : SARADC_BOOT_ORDER;
    case 1:
        duty = rp2350_pwm_duty(&s->mcu, 4);
        if (duty < 0) {                         /* (a plain pin) */
            duty = (s->mcu.pin_level >> 4) & 1 ? 65535 : 0;
        }
        return (duty * 4095 + 32767) / 65535;
    case 2:
        return strcmp(s->rev ?: FO_REV_DEFAULT, "f0b1c2") ? 0 : 416;
    }
    return 0;
}

static void flipper_one_usb20_sel(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    s->usb20_sel = level;
    flipper_one_usb_data(s);
}

/* an input of the main board's expander: the charger's INT (pin 2) and the
 * FUSB302's (pin 3), open drain, low when asserted */
static void flipper_one_main_input(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    s->main_inputs = deposit32(s->main_inputs, n, 1, level);
    pcal6416_set_pins(s->main_expander, s->main_inputs);
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

/* CPU_INT (GPIO35, open drain, the board's pull-up): the RK3576's GPIO2_A6,
 * and the host link */
static void flipper_one_cpu_int(void *opaque, int pin, bool level,
                                int64_t when_ns)
{
    FlipperOneState *s = opaque;

    rk3576_gpio_in(&s->rk3576, 2, 6, level);
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
static void fo_rk_power(FlipperOneState *s, int want);
enum { RK_OFF, RK_ON, RK_RESTART };

static void flipper_one_host_power(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    fo_battery_update(&s->battery);             /* up to now, as it was */
    s->battery.load_ma = s->off ? 0 : level ? 400 : 40;
    fo_battery_update(&s->battery);
    if (s->linux_boot && level != s->rk_rail) {
        s->rk_rail = level;
        fo_rk_power(s, level ? RK_ON : RK_OFF);
    }
    s->rk_rail = level;
}

/*
 * The expanders' RESET: GPIO43 (the control bus's), GPIO38 (the main
 * board's), open drain with the board's pull-ups; a low resets a chip only
 * when it lasts. The firmware's open-drain setup of the pins drives them
 * low for a few instructions (OUTOVER low before it lets the pin go): the
 * chips would be reset at every MCU start, and the main one's rails with
 * them, where the firmware has a powered host run on across an MCU
 * restart. On the board that glitch does not reset them. Here a low resets
 * them once it has lasted 20 ms: the virtual clock follows the host's, and
 * the glitch's few instructions are two writes to IO_BANK0, each needing
 * the BQL, which the RK3576's eight cores keep busy while Linux boots (a
 * 100 us threshold once let the glitch reset the main expander, and cut
 * the Linux side's rail as it started). The firmware's own resets hold it
 * 50 ms.
 */
#define EXPANDER_RESET_NS   (20 * SCALE_MS)

static void flipper_one_buttons_reset(void *opaque)
{
    FlipperOneState *s = opaque;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->buttons), 0), 0);
}

static void flipper_one_main_reset(void *opaque)
{
    FlipperOneState *s = opaque;

    qemu_set_irq(qdev_get_gpio_in(DEVICE(s->main_expander), 0), 0);
}

static void flipper_one_expander_reset(void *opaque, int pin, bool level,
                                       int64_t when_ns)
{
    FlipperOneState *s = opaque;
    int i = pin == 43 ? 0 : 1;
    I2CSlave *dev = i ? s->main_expander : s->buttons;

    if (!level) {
        timer_mod(s->expander_reset[i], when_ns + EXPANDER_RESET_NS);
    } else {
        timer_del(s->expander_reset[i]);
        qemu_set_irq(qdev_get_gpio_in(DEVICE(dev), 0), 1);
    }
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

/*
 * The rails the five INA4230s on the main bus measure, four each, in their
 * order (0x40, 0x41, 0x43, 0x44, 0x46; as the device tree and the MCU
 * firmware name them), with their shunts. A rail is at its voltage while
 * the RK3576 runs, the control board's 3.3 V while the board has its
 * supply. The currents are nominal, not measured: Flipper OS at rest, and
 * on the two CPU rails so much more for each core of the cluster that is
 * running (not waiting for an interrupt) as a channel is read, at the
 * cluster's top clock; at a lower one less, and the rail at the voltage
 * the OPP table gives that clock (rk3576_cluster_*).
 */
static const struct {
    const char *name;
    uint32_t shunt_uohm;
    uint16_t mv, ma;
    int8_t cluster;                     /* a CPU rail's: 0 the A53s, 1 the
                                           A72s; -1 */
    uint16_t ma_per_core;
} fo_rails[20] = {
    { "VDD_0V75_S3",         50000,  750,  40, -1,   0 },
    { "VCC3V3_CONTROL",      10000, 3300,  60, -1,   0 },
    { "VDD0V85_DDR_S0",      20000,  850, 120, -1,   0 },
    { "VCC_3V3_S3",          10000, 3300,  90, -1,   0 },
    { "VDDQ0V51_DDR_S0",     20000,  510,  90, -1,   0 },
    { "VDD0V75_NPU_S0",      10000,  750,   5, -1,   0 },
    { "VDD0V75_GPU_S0",      20000,  750,  40, -1,   0 },
    { "VDD0V75_LOGIC_S0",    20000,  750, 280, -1,   0 },
    { "VDD0V75_CPU_BIG_S0",  10000,  750,  60,  1, 350 },
    { "VDDA_1V2_S0",         50000, 1200,   8, -1,   0 },
    { "VCCA_1V8_S0",         20000, 1800,  25, -1,   0 },
    { "VDD0V75_CPU_LIT_S0",  10000,  750,  40,  0, 120 },
    { "VDDA_0V75_S0",        50000,  750,  15, -1,   0 },
    { "VDDA_0V85_S0",        20000,  850,  25, -1,   0 },
    { "VDDA0V75_HDMI_S0",    20000,  750,   4, -1,   0 },
    { "VDDA0V85_DDR_PLL_S0", 50000,  850,   3, -1,   0 },
    { "VCCA_3V3_S0",         20000, 3300,  15, -1,   0 },
    { "VCCIO_3V3_SD_S0",     50000, 3300,  20, -1,   0 },
    { "VDD2_1V05_DDR_S3",    20000, 1050, 180, -1,   0 },
    { "VCC_1V8_S3",          20000, 1800,  70, -1,   0 },
};
#define FO_RAIL_CONTROL 1                       /* VCC3V3_CONTROL */

static void flipper_one_rail(void *opaque, int rail, int32_t *mv,
                             int32_t *ua)
{
    FlipperOneState *s = opaque;
    int ma = fo_rails[rail].ma;

    int c = fo_rails[rail].cluster;
    double core_ma = 0;

    *mv = *ua = 0;
    if (s->off || (rail != FO_RAIL_CONTROL && !s->rk_on)) {
        return;
    }
    *mv = fo_rails[rail].mv;
    if (c >= 0) {
        for (int i = 0; i < 4; i++) {
            if (!qatomic_read(&CPU(&s->rk3576.cpu[c * 4 + i])->halted)) {
                core_ma += fo_rails[rail].ma_per_core;
            }
        }
        core_ma *= rk3576_cluster_current(&s->rk3576, c);
        *mv = rk3576_cluster_mv(&s->rk3576, c);
    }
    *ua = ma * 1000 + (int32_t)(core_ma * 1000 + 0.5);
}

/* the main board's bus: the MCU's PIO I2C, and the RK3576's I2C2 */
static bool flipper_one_main_bus_claim(void *opaque, bool claim)
{
    FlipperOneState *s = opaque;

    return fo_i2c_pins_claim(&s->main_bus, claim);
}

/*
 * The link: the RK3576's I2C0 reaches the MCU's I2C1, which its firmware
 * serves in slave mode at 0x69 (its intercom). The MCU's controller takes
 * each byte when its FIFOs let it, holding SCL until then.
 */
static int flipper_one_link_start(void *opaque, uint8_t addr, bool read)
{
    FlipperOneState *s = opaque;

    return rp2350_i2c_target_start(&s->mcu, 1, addr, read) ? 1 : -1;
}

static int flipper_one_link_write(void *opaque, uint8_t byte)
{
    FlipperOneState *s = opaque;

    return rp2350_i2c_target_write(&s->mcu, 1, byte);
}

static int flipper_one_link_read(void *opaque, uint8_t *byte)
{
    FlipperOneState *s = opaque;

    return rp2350_i2c_target_read(&s->mcu, 1, byte);
}

static void flipper_one_link_nack(void *opaque)
{
    FlipperOneState *s = opaque;

    rp2350_i2c_target_nack(&s->mcu, 1);
}

static void flipper_one_link_stop(void *opaque)
{
    FlipperOneState *s = opaque;

    rp2350_i2c_target_stop(&s->mcu, 1);
}

static void flipper_one_link_release(void *opaque)
{
    FlipperOneState *s = opaque;

    rk_i2c_release(&s->rk3576.i2c[0]);
}

/*
 * The display link: Flipper OS sends a frame over SPI0 (CS0) between
 * "active" going low and back high, the RK3576's GPIO0_D1 (pin 25); the
 * frame then goes to the MCU as the RK3576 sends it, edge by edge on its
 * GPIO25 (active), 26 (SCK) and 28 (MOSI) (host_spi.c), after the frame
 * before it. Frames that come while two wait to go are dropped. And a
 * frame the same as the last one sent, within a second of it, is not sent
 * again: the MCU's screen would not change, and its PIO's capture of a
 * frame is the machine's dearest work. (U-Boot redraws its unchanging boot
 * menu five times a second, each frame a word at a time into SPI0's FIFO:
 * captured, they kept its core waiting on the BQL, and its keys unread.)
 */
#define FRAME_SAME_MS   1000
#define FRAME_MAX       65536

static void flipper_one_spi0_out(void *opaque, int cs, const uint8_t *data,
                                 int len)
{
    FlipperOneState *s = opaque;

    if (s->frame_active && (cs & 1) && s->frame->len + len <= FRAME_MAX) {
        g_byte_array_append(s->frame, data, len);
    }
}

static void flipper_one_rk_gpio(void *opaque, int bank, int pin, bool level)
{
    FlipperOneState *s = opaque;

    if (bank == 1 && pin == 29) {               /* GPIO1_D5: the MT7921's
                                                   PMU_EN (rfkill's) */
        if (s->mt7921) {
            mt7921_power(s->mt7921, level);
        }
        return;
    }
    if (bank != 0 || pin != 25) {
        return;
    }
    if (!level) {                               /* active: a frame */
        s->frame_active = true;
        g_byte_array_set_size(s->frame, 0);
    } else if (s->frame_active) {
        s->frame_active = false;
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (!s->frame->len) {
            return;
        }
        if (s->last_frame && s->last_frame->len == s->frame->len &&
            now - s->last_frame_ms < FRAME_SAME_MS &&
            !memcmp(s->last_frame->data, s->frame->data, s->frame->len)) {
            s->frames_same++;
            return;
        }
        if (fo_host_spi_waiting(&s->linux_frames) >= 2) {
            s->frames_dropped++;
            return;
        }
        if (!s->last_frame) {
            s->last_frame = g_byte_array_sized_new(FO_LCD_H * (FO_LCD_W + 2));
        }
        g_byte_array_set_size(s->last_frame, 0);
        g_byte_array_append(s->last_frame, s->frame->data, s->frame->len);
        s->last_frame_ms = now;
        fo_host_spi_send(&s->linux_frames, s->frame);   /* (its now) */
        s->frame = g_byte_array_sized_new(FO_LCD_H * (FO_LCD_W + 2));
    }
}

static void flipper_one_frame_sent(void *opaque)
{
    FlipperOneState *s = opaque;

    s->frames_sent++;
}

/*
 * The RK3576 has its supply when the MCU switches VCC5V0_SYS_S5 on (the
 * main expander's pin 12), as on the device. At the machine's start its
 * cores are off; the kernel (-kernel, as QEMU loads it) starts when the
 * rail comes on, from its images in DRAM as they were loaded (put back
 * each time after the first: a run writes over them). Or U-Boot does
 * (uboot=on), as on the device: at each start the machine leaves what the
 * DDR blob leaves, loads U-Boot from the microSD card as SPL does (fo_spl)
 * and starts it at EL2, as TF-A starts BL33. Without its rail, or after
 * Linux's PSCI SYSTEM_OFF, the cores are off and its devices as at
 * power-on, and the PMU's always-on registers lost; SYSTEM_RESET starts it
 * again. The MCU runs on through all of it.
 */
static void fo_rk_save_images(FlipperOneState *s)
{
    AddressSpace *as = &address_space_memory;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    uint64_t ksize = 0;
    uint32_t dtsize = 0;
    struct {
        hwaddr addr;
        size_t len;
    } r[4];

    /* the kernel's Image header says its size, the DTB's its own */
    address_space_read(as, s->binfo.entry + 16, attrs, &ksize, 8);
    address_space_read(as, s->binfo.dtb_start + 4, attrs, &dtsize, 4);
    r[0].addr = s->binfo.loader_start;
    r[0].len = 0x1000;
    r[1].addr = s->binfo.entry;
    r[1].len = MIN(le64_to_cpu(ksize) ?: 64 * MiB, 256 * MiB);
    r[2].addr = s->binfo.initrd_start;
    r[2].len = s->binfo.initrd_size;
    r[3].addr = s->binfo.dtb_start;
    r[3].len = MIN(be32_to_cpu(dtsize), 4 * MiB);
    for (int i = 0; i < 4; i++) {
        if (!r[i].len) {
            continue;
        }
        s->rk_images[s->rk_nimages].addr = r[i].addr;
        s->rk_images[s->rk_nimages].len = r[i].len;
        s->rk_images[s->rk_nimages].data = g_malloc(r[i].len);
        address_space_read(as, r[i].addr, attrs,
                           s->rk_images[s->rk_nimages].data, r[i].len);
        s->rk_nimages++;
    }
}

static void fo_rk_restore_images(FlipperOneState *s)
{
    for (int i = 0; i < s->rk_nimages; i++) {
        address_space_write(&address_space_memory, s->rk_images[i].addr,
                            MEMTXATTRS_UNSPECIFIED, s->rk_images[i].data,
                            s->rk_images[i].len);
    }
}

/*
 * U-Boot proper, as SPL loads it on the device: from the microSD card's
 * u-boot.itb (a FIT at sector 16384, its images' data after its tree or in
 * it), the configuration for the board's revision (compatible
 * "flipper,one-rev-REV"; the FIT's default if none is), U-Boot at its load
 * address and that configuration's device tree right after it, where U-Boot
 * looks for its own. TF-A's images are left: the machine is TF-A (QEMU's
 * PSCI, the SCMI server). Its entry; 0 if there is nothing SPL would start
 * (the boot ROM would go on to its USB mode).
 */
#define FO_SPL_FIT      (16384 * 512)
#define FO_SPL_FIT_MAX  (64 * KiB)

static bool fo_fit_addr(const void *fit, int node, const char *name,
                        hwaddr *addr)
{
    int len;
    const fdt32_t *p = fdt_getprop(fit, node, name, &len);

    if (!p || (len != 4 && len != 8)) {
        return false;
    }
    *addr = len == 8 ? (hwaddr)fdt32_to_cpu(p[0]) << 32 | fdt32_to_cpu(p[1])
                     : fdt32_to_cpu(p[0]);
    return true;
}

/* an image of the FIT into DRAM at *@addr, or at its own load address if
 * that is 0: its length, 0 if it cannot be */
static uint32_t fo_spl_image(FlipperOneState *s, const void *fit, int node,
                             int64_t data_at, hwaddr *addr)
{
    const char *comp = fdt_getprop(fit, node, "compression", NULL);
    g_autofree uint8_t *buf = NULL;
    const void *data;
    hwaddr pos, size;
    int len;

    if ((comp && strcmp(comp, "none")) ||
        (!*addr && !fo_fit_addr(fit, node, "load", addr))) {
        return 0;
    }
    data = fdt_getprop(fit, node, "data", &len);
    if (!data) {
        if (!fo_fit_addr(fit, node, "data-size", &size) || !size ||
            size > 64 * MiB) {
            return 0;
        }
        if (fo_fit_addr(fit, node, "data-position", &pos)) {
            pos += FO_SPL_FIT;
        } else if (fo_fit_addr(fit, node, "data-offset", &pos)) {
            pos += data_at;
        } else {
            return 0;
        }
        len = size;
        buf = g_malloc(len);
        if (blk_pread(s->boot_blk, pos, len, buf, 0) < 0) {
            return 0;
        }
        data = buf;
    }
    address_space_write(&address_space_memory, *addr, MEMTXATTRS_UNSPECIFIED,
                        data, len);
    return len;
}

static hwaddr fo_spl_fail(const char *why)
{
    qemu_log("flipper-one: SPL: %s\n", why);
    return 0;
}

/*
 * What the boot ROM boots, in the order SARADC_VIN0_BOOT gives it (the
 * board's config 8: UFS, then the microSD card, then USB): a medium with
 * Rockchip's RKNS image (DDR init and SPL) 32 KiB in, the UFS read through
 * its enabled boot LU. Its u-boot-rockchip.bin there has the FIT for SPL at
 * 8 MiB on both.
 */
#define FO_RKNS_AT      0x8000

static bool fo_bootable(BlockBackend *blk)
{
    uint8_t magic[4];

    return blk && blk_is_inserted(blk) &&
           blk_pread(blk, FO_RKNS_AT, sizeof(magic), magic, 0) >= 0 &&
           !memcmp(magic, "RKNS", 4);
}

/* the UFS's enabled boot LU: bBootLunEn, as the device's own state keeps
 * it (patch 0017's nvram: A until something switches it) */
static BlockBackend *fo_ufs_boot_lu(void)
{
    BlockBackend *nv = blk_by_name("ufsnv");
    uint8_t buf[16];
    int lun = 1;

    if (nv && blk_pread(nv, 0, sizeof(buf), buf, 0) >= 0 &&
        !memcmp(buf, "QUFSNV01", 8)) {
        lun = buf[8];
    }
    return lun == 1 ? blk_by_name("ufs1") : lun == 2 ? blk_by_name("ufs2")
                                                     : NULL;
}

/* the medium the boot ROM boots (boot_blk): false if none has an image,
 * and it falls back to its USB download */
static bool fo_boot_rom(FlipperOneState *s)
{
    s->boot_blk = fo_ufs_boot_lu();
    if (!fo_bootable(s->boot_blk)) {
        s->boot_blk = s->sd_blk;
    }
    if (!fo_bootable(s->boot_blk)) {
        s->boot_blk = NULL;
        qemu_log("flipper-one: the boot ROM finds nothing to boot on the "
                 "UFS's boot LU or the microSD card: it waits in its MaskROM, "
                 "for a USB download\n");
        return false;
    }
    qemu_log("flipper-one: the boot ROM boots %s\n",
             s->boot_blk == s->sd_blk ? "the microSD card"
                                      : "the UFS's boot LU");
    return true;
}

/* SPL, from the medium the boot ROM chose: U-Boot proper from its FIT */
static hwaddr fo_spl(FlipperOneState *s)
{
    g_autofree char *compat = g_strdup_printf("flipper,one-rev-%s",
                                              s->rev ?: FO_REV_DEFAULT);
    g_autofree uint8_t *fit = NULL;
    uint8_t head[sizeof(struct fdt_header)];
    int images, confs, conf = -1, node, uboot = -1;
    const char *name, *os;
    hwaddr addr = 0, entry, fdt_at;
    uint32_t len, size;

    if (blk_pread(s->boot_blk, FO_SPL_FIT, sizeof(head), head, 0) < 0 ||
        fdt_magic(head) != FDT_MAGIC) {
        return fo_spl_fail("no u-boot.itb 8 MiB in");
    }
    size = fdt_totalsize(head);
    if (size < sizeof(head) || size > FO_SPL_FIT_MAX) {
        return fo_spl_fail("u-boot.itb's tree is not one SPL takes");
    }
    fit = g_malloc(size);
    if (blk_pread(s->boot_blk, FO_SPL_FIT, size, fit, 0) < 0 ||
        fdt_check_header(fit)) {
        return fo_spl_fail("u-boot.itb's tree is not one SPL takes");
    }
    images = fdt_path_offset(fit, "/images");
    confs = fdt_path_offset(fit, "/configurations");
    if (images < 0 || confs < 0) {
        return fo_spl_fail("u-boot.itb has no images or configurations");
    }
    fdt_for_each_subnode(node, fit, confs) {
        const char *c = fdt_getprop(fit, node, "compatible", NULL);

        if (c && !strcmp(c, compat)) {
            conf = node;
            break;
        }
    }
    if (conf < 0) {
        name = fdt_getprop(fit, confs, "default", NULL);
        conf = name ? fdt_subnode_offset(fit, confs, name) : -1;
        qemu_log("flipper-one: SPL: no configuration for %s: the default\n",
                 compat);
        if (conf < 0) {
            return fo_spl_fail("u-boot.itb has no default configuration");
        }
    }
    /* U-Boot: of the configuration's images, the one whose OS is U-Boot */
    for (int i = 0; uboot < 0; i++) {
        name = fdt_stringlist_get(fit, conf, "loadables", i, NULL);
        if (!name && !i) {
            name = fdt_getprop(fit, conf, "firmware", NULL);
        }
        if (!name) {
            break;
        }
        node = fdt_subnode_offset(fit, images, name);
        os = node >= 0 ? fdt_getprop(fit, node, "os", NULL) : NULL;
        if (os && !strcmp(os, "u-boot")) {
            uboot = node;
        }
    }
    if (uboot < 0) {
        name = fdt_getprop(fit, conf, "firmware", NULL);
        node = name ? fdt_subnode_offset(fit, images, name) : -1;
        os = node >= 0 ? fdt_getprop(fit, node, "os", NULL) : NULL;
        if (os && !strcmp(os, "u-boot")) {
            uboot = node;
        }
    }
    if (uboot < 0 || !(len = fo_spl_image(s, fit, uboot, FO_SPL_FIT +
                                          ROUND_UP(size, 4), &addr))) {
        return fo_spl_fail("no U-Boot in u-boot.itb that SPL could load");
    }
    entry = addr;
    fo_fit_addr(fit, uboot, "entry", &entry);
    name = fdt_getprop(fit, conf, "fdt", NULL);
    node = name ? fdt_subnode_offset(fit, images, name) : -1;
    fdt_at = ROUND_UP(addr + len, 8);
    if (node < 0 || !fo_spl_image(s, fit, node, FO_SPL_FIT + ROUND_UP(size, 4),
                                  &fdt_at)) {
        return fo_spl_fail("no device tree for U-Boot in u-boot.itb");
    }
    qemu_log("flipper-one: SPL: U-Boot at 0x%" HWADDR_PRIx ", %s after it\n",
             addr, (const char *)fdt_getprop(fit, conf, "description", NULL)
                   ?: name);
    return entry;
}

/*
 * The Flipper OS installer, as rockusb's download-boot starts it on the
 * device: a FIT for U-Boot's SPL in its Falcon mode, whose configuration for
 * the board's revision has the kernel (zstd), its ramdisk and its device
 * tree, besides TF-A's BL31 (falcon=FILE). The machine is SPL and TF-A here,
 * as for U-Boot (fo_spl): the kernel decompressed to its load address, the
 * ramdisk and the tree to theirs, the tree given the memory (after TF-A's 2
 * MiB, as the direct boot has it: SPL adds the node; the tree's /chosen
 * already has the ramdisk and the kernel's arguments), and the kernel
 * started at EL2 with the tree's address in x0, PSCI and SCMI the machine's
 * own. Its entry, 0 if it cannot be.
 */
#define FO_FALCON_KERNEL_MAX    (128 * MiB)

static const uint8_t *fo_falcon_data(FlipperOneState *s, int node,
                                     size_t *len)
{
    const void *fit = s->falcon_fit;
    hwaddr off, size;
    int l;
    const void *data = fdt_getprop(fit, node, "data", &l);

    if (data) {
        *len = l;
        return data;
    }
    if (!fo_fit_addr(fit, node, "data-size", &size)) {
        return NULL;
    }
    if (fo_fit_addr(fit, node, "data-offset", &off)) {
        off += ROUND_UP(fdt_totalsize(fit), 4);
    } else if (!fo_fit_addr(fit, node, "data-position", &off)) {
        return NULL;
    }
    if (off > s->falcon_len || size > s->falcon_len - off) {
        return NULL;
    }
    *len = size;
    return s->falcon_fit + off;
}

/*
 * The kernel's tree, as the machine gives it (a direct boot, or SPL's
 * Falcon mode): its CPUs' idle states that power a core down (the RK3576's
 * cpu-sleep: PSCI's power-down, its local timer stopped) unavailable, as
 * firmware does with a state it does not provide. QEMU's PSCI does a
 * CPU_SUSPEND as a WFI and returns, which Linux takes for a failed entry,
 * after its whole way into and out of a power-down state for nothing: the
 * CPU PM notifiers (the GIC's per-CPU state saved and restored), the tick
 * handed to the broadcast timer and back. Idle, QEMU took 2.6 times the
 * host CPU, and an IPI to an idle core four times as long (log 106).
 * Linux idles with WFI then, as with no idle states. Not with deep-idle=on:
 * the tree as it is, the device's.
 */
static int fo_fdt_idle_states(FlipperOneState *s, void *fdt)
{
    int node = -1, err;

    if (s->deep_idle) {
        return 0;
    }
    while ((node = fdt_node_offset_by_compatible(fdt, node,
                                                 "arm,idle-state")) >= 0) {
        err = fdt_setprop_string(fdt, node, "status", "disabled");
        if (err) {
            return err;
        }
    }
    return 0;
}

/* an image of the configuration to its load address: its end, 0 if not */
static hwaddr fo_falcon_image(FlipperOneState *s, int node, bool tree)
{
    const void *fit = s->falcon_fit;
    const char *comp = fdt_getprop(fit, node, "compression", NULL);
    g_autofree uint8_t *buf = NULL;
    const uint8_t *data;
    size_t len;
    hwaddr load;

    if (!fo_fit_addr(fit, node, "load", &load) ||
        !(data = fo_falcon_data(s, node, &len))) {
        return 0;
    }
    if (comp && !strcmp(comp, "zstd")) {
        unsigned long long n = ZSTD_getFrameContentSize(data, len);

        if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN ||
            n > FO_FALCON_KERNEL_MAX) {
            return 0;
        }
        buf = g_malloc(n);
        if (ZSTD_isError(ZSTD_decompress(buf, n, data, len))) {
            return 0;
        }
        data = buf;
        len = n;
    } else if (comp && strcmp(comp, "none")) {
        return 0;
    }
    if (tree) {                         /* the memory, as SPL adds it */
        hwaddr base = RK3576_DRAM_BASE + RK3576_TFA_SIZE;
        uint64_t size = MACHINE(s)->ram_size - RK3576_TFA_SIZE;
        fdt32_t reg[4] = {
            cpu_to_fdt32(base >> 32), cpu_to_fdt32(base),
            cpu_to_fdt32(size >> 32), cpu_to_fdt32(size),
        };
        uint8_t *t = g_malloc(len + 1024);
        int m;

        if (fdt_open_into(data, t, len + 1024) ||
            (m = fdt_add_subnode(t, 0, "memory@40200000")) < 0 ||
            fdt_setprop_string(t, m, "device_type", "memory") ||
            fdt_setprop(t, m, "reg", reg, sizeof(reg)) ||
            fo_fdt_idle_states(s, t) || fdt_pack(t)) {
            g_free(t);
            return 0;
        }
        g_free(buf);
        buf = t;
        data = t;
        len = fdt_totalsize(t);
    }
    address_space_write(&address_space_memory, load, MEMTXATTRS_UNSPECIFIED,
                        data, len);
    return load + len;
}

static hwaddr fo_falcon_boot(FlipperOneState *s)
{
    g_autofree char *compat = g_strdup_printf("flipper,one-rev-%s",
                                              s->rev ?: FO_REV_DEFAULT);
    const void *fit = s->falcon_fit;
    const char *name, *type;
    int images, confs, conf = -1, node;
    hwaddr entry = 0, fdt_at = 0;

    if (s->falcon_len < sizeof(struct fdt_header) || fdt_check_header(fit) ||
        fdt_totalsize(fit) > s->falcon_len) {
        return fo_spl_fail("the installer is not a FIT");
    }
    images = fdt_path_offset(fit, "/images");
    confs = fdt_path_offset(fit, "/configurations");
    if (images < 0 || confs < 0) {
        return fo_spl_fail("the installer's FIT has no images or "
                           "configurations");
    }
    fdt_for_each_subnode(node, fit, confs) {
        if (!fdt_stringlist_search(fit, node, "compatible", compat)) {
            conf = node;
            break;
        }
    }
    if (conf < 0) {
        return fo_spl_fail("the installer has no configuration for this "
                           "board's revision");
    }
    /* its loadables (TF-A's left out: the machine is it) and its tree */
    for (int i = 0; (name = fdt_stringlist_get(fit, conf, "loadables", i,
                                                NULL)); i++) {
        node = fdt_subnode_offset(fit, images, name);
        type = node >= 0 ? fdt_getprop(fit, node, "type", NULL) : NULL;
        if (!type || !strcmp(type, "firmware")) {
            continue;
        }
        if (!fo_falcon_image(s, node, false)) {
            return fo_spl_fail("an image of the installer could not be "
                               "loaded");
        }
        if (!strcmp(type, "kernel") &&
            !fo_fit_addr(fit, node, "entry", &entry)) {
            fo_fit_addr(fit, node, "load", &entry);
        }
    }
    name = fdt_getprop(fit, conf, "fdt", NULL);
    node = name ? fdt_subnode_offset(fit, images, name) : -1;
    if (node < 0 || !fo_fit_addr(fit, node, "load", &fdt_at) ||
        !fo_falcon_image(s, node, true)) {
        return fo_spl_fail("no device tree in the installer");
    }
    if (!entry) {
        return fo_spl_fail("no kernel in the installer");
    }
    s->entry_x0 = fdt_at;
    qemu_log("flipper-one: SPL (Falcon): the kernel at 0x%" HWADDR_PRIx
             ", %s at 0x%" HWADDR_PRIx "\n", entry,
             (const char *)fdt_getprop(fit, conf, "description", NULL) ?: name,
             fdt_at);
    return entry;
}

/* ... the installer's file (falcon=FILE), read once */
static hwaddr fo_falcon(FlipperOneState *s)
{
    g_autoptr(GError) err = NULL;

    if (!s->falcon_fit &&
        !g_file_get_contents(s->falcon, (char **)&s->falcon_fit,
                             &s->falcon_len, &err)) {
        return fo_spl_fail(err->message);
    }
    return fo_falcon_boot(s);
}

/* the first FIT (a tree with images and configurations) in @data, with
 * what follows it (its images' data, if they are outside it) */
static const uint8_t *fo_find_fit(const uint8_t *data, size_t len,
                                  size_t *fit_len)
{
    for (size_t at = 0; at + sizeof(struct fdt_header) <= len; at += 4) {
        const void *t = data + at;

        if (fdt_magic(t) == FDT_MAGIC && fdt_totalsize(t) <= len - at &&
            !fdt_check_header(t) && fdt_path_offset(t, "/images") >= 0 &&
            fdt_path_offset(t, "/configurations") >= 0) {
            *fit_len = len - at;
            return t;
        }
    }
    return NULL;
}

static void fo_rk_cpu_off(ARMCPU *cpu)
{
    arm_set_cpu_power_state(cpu, PSCI_OFF);
    CPU(cpu)->halted = 1;
}

/* in the main loop, the cores stopped: off, on, or started again */
/* the RK3576's CPUs as TF-A leaves them: CPU 0 at @entry (x0: entry_x0),
 * the others off, for PSCI's CPU_ON; all of them off with no @entry. The
 * vCPUs paused. */
static void fo_rk_start(FlipperOneState *s, hwaddr entry)
{
    if (entry) {
        rk_scmi_reset(&s->rk3576.scmi);         /* (TF-A's start) */
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        ARMCPU *cpu = &s->rk3576.cpu[i];
        CPUState *cs = CPU(cpu);

        cpu_reset(cs);
        if (entry && i == 0) {
            arm_emulate_firmware_reset(cs, 2);  /* as QEMU's own direct boot,
                                                   or TF-A to BL33 */
            cpu_set_pc(cs, entry);
            cpu->env.xregs[0] = s->entry_x0;
            arm_rebuild_hflags(&cpu->env);
        } else {
            fo_rk_cpu_off(cpu);
        }
    }
    s->rk_on = entry != 0;
}

/*
 * What the boot ROM's USB download runs (rk3576_maskrom.c). 0x471 code, for
 * SRAM, is the DDR blob (or its like): the machine does its work
 * (rk3576_ddr_blob()), and the boot ROM waits for the next. 0x472 code, for
 * DRAM, is SPL: the machine plays U-Boot's SPL in its Falcon mode on the
 * FIT that the payload has after SPL's code, as rockusb's download-boot of
 * the Flipper OS installer's loader has it (fo_falcon_boot()). The boot ROM
 * is left then, whatever comes of it. 0x472 code with no FIT is a usbplug,
 * as a Rockchip loader for rkdeveloptool has one: the machine plays that
 * too, Rockusb's Loader mode on USB0 (rk_maskrom_loader()), the UFS's LU 0
 * behind it (or the card, if Rockusb switches to it). Its reset restarts
 * the RK3576.
 */
static void fo_maskrom_run(void *opaque, uint16_t code, const uint8_t *data,
                           size_t len)
{
    FlipperOneState *s = opaque;
    bool running = runstate_is_running();
    const uint8_t *fit = NULL;
    size_t fit_len = 0;
    hwaddr entry = 0;

    if (code == 0x471) {
        rk3576_ddr_blob(&s->rk3576, MACHINE(s)->ram_size);
        s->dram_up = true;
        qemu_log("flipper-one: %" PRId64 " ms: the boot ROM's 0x471 code "
                 "ran: the DRAM is up (the machine's DDR blob)\n",
                 qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        return;
    }
    if (s->dram_up && !(fit = fo_find_fit(data, len, &fit_len))) {
        qemu_log("flipper-one: %" PRId64 " ms: the boot ROM's 0x472 code has "
                 "no FIT: a usbplug, which the machine plays: Rockusb's Loader "
                 "mode on USB0, %s behind it\n",
                 qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                 blk_by_name("ufs") ? "the UFS's LU 0" : "the microSD card");
        rk_maskrom_loader(&s->rk3576.maskrom, blk_by_name("ufs"), s->sd_blk);
        return;
    }
    s->in_maskrom = false;                      /* SPL's from here */
    rk_maskrom_set(&s->rk3576.maskrom, false);
    flipper_one_usb_data(s);
    s->entry_x0 = 0;
    if (!s->dram_up) {
        fo_spl_fail("0x472 code with the DRAM not up (no 0x471 code before "
                    "it): it cannot run");
    } else {
        g_free(s->falcon_fit);
        s->falcon_fit = g_memdup2(fit, fit_len);
        s->falcon_len = fit_len;
        s->falcon_done = true;
        entry = fo_falcon_boot(s);
    }
    if (running) {
        pause_all_vcpus();
    }
    fo_rk_start(s, entry);
    if (running) {
        resume_all_vcpus();
    }
    qemu_log("flipper-one: %" PRId64 " ms: the RK3576 %s\n",
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
             entry ? "starts what the boot ROM's USB download gave it"
                   : "has nothing to start");
    fo_panel_rk3576(s->panel, s->rk_on);
}

static void fo_rk_power_bh(void *opaque)
{
    FlipperOneState *s = opaque;
    bool running = runstate_is_running();
    int want = s->rk_want;
    hwaddr entry = 0;

    s->entry_x0 = 0;
    if (want != RK_OFF && !s->rk_rail) {
        want = RK_OFF;
    }
    if (want == RK_ON && s->rk_on && !s->rk_lost) {
        return;                                 /* (on already) */
    }
    s->rk_lost = false;
    rk_maskrom_set(&s->rk3576.maskrom, false);  /* (its USB0 goes, to come
                                                   back if it is still due) */
    if (running) {
        pause_all_vcpus();
    }
    rk3576_reset_devices(&s->rk3576);
    s->frame_active = false;
    if (s->last_frame) {                        /* (a new start's first frame
                                                   goes, whatever it is) */
        g_byte_array_set_size(s->last_frame, 0);
    }
    /* the boot ROM's USB download: BOOT0 low at the reset, or the boot mode
     * that asks for it (Linux's `reboot maskrom`) */
    s->in_maskrom = want != RK_OFF &&
                    (rk3576_brom_download(&s->rk3576) || s->maskrom_strap);
    s->dram_up = false;
    if (want == RK_OFF) {
        rk3576_power_lost(&s->rk3576);
    } else if (s->in_maskrom) {
        /* its boot ROM in its USB download mode, the strap read at the
         * reset: nothing starts until a computer sends something */
    } else if (s->falcon && !s->falcon_done) {  /* ... SPL in Falcon mode */
        s->falcon_done = true;                  /* (the download's: once) */
        rk3576_ddr_blob(&s->rk3576, MACHINE(s)->ram_size);
        entry = fo_falcon(s);
    } else if (s->uboot || s->falcon) {         /* the DDR blob, then SPL */
        s->in_maskrom = !fo_boot_rom(s);
        if (!s->in_maskrom) {
            rk3576_ddr_blob(&s->rk3576, MACHINE(s)->ram_size);
            entry = fo_spl(s);
        }
    } else {
        if (!s->rk_nimages) {
            fo_rk_save_images(s);               /* the first start: as loaded */
        } else {
            fo_rk_restore_images(s);
        }
        /* (the DDR blob's words, which the device has at any boot and
         * Linux's DFI reads; the blob's ATAG is U-Boot's alone) */
        rk3576_ddr_words(&s->rk3576, MACHINE(s)->ram_size);
        entry = s->binfo.loader_start;
    }
    fo_rk_start(s, entry);
    if (running) {
        resume_all_vcpus();
    }
    rk_maskrom_set(&s->rk3576.maskrom, s->in_maskrom);
    flipper_one_usb_data(s);
    qemu_log("flipper-one: %" PRId64 " ms: the RK3576 %s\n",
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
             want == RK_OFF ? "is off" :
             s->in_maskrom ? "waits in its MaskROM: its USB download, USB0 "
                             "a device for the computer on the cable" :
             !entry ? "has nothing to start" :
             want == RK_ON ? "starts" : "starts again");
    fo_panel_rk3576(s->panel, s->rk_on);
}

static void fo_rk_power(FlipperOneState *s, int want)
{
    if (want == RK_OFF) {
        s->rk_lost = true;              /* (on again before the BH: a restart) */
    }
    s->rk_want = want;
    qemu_bh_schedule(s->rk_power_bh);
}

/* a machine reset: QEMU's direct boot has started the kernel's core; it
 * waits for its rail (the expanders are as reset: the rail off) */
static void flipper_one_rk3576_gate(void *opaque)
{
    FlipperOneState *s = opaque;

    s->rk_rail = s->rk_on = s->rk_lost = false;
    rk3576_power_lost(&s->rk3576);
    for (int i = 0; i < RK3576_NCPUS; i++) {
        fo_rk_cpu_off(&s->rk3576.cpu[i]);
    }
    fo_panel_rk3576(s->panel, false);
}

/*
 * Linux's PSCI calls that are the RK3576's alone (QEMU patch 0005): on the
 * device TF-A turns the SoC off or resets it, and the MCU's rail stays on;
 * here the board does, and the core that called stops, as PSCI has it. And
 * the SMC that TF-A answers besides PSCI: SCMI (rk3576_scmi.c).
 */
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u
#define PSCI_SYSTEM_RESET2      0x84000012u
#define PSCI_64BIT              0x40000000u
#define PSCI_FEATURES           0x8400000au
#define SMCCC_VERSION           0x80000000u
#define SMCCC_ARCH_FEATURES     0x80000001u

static bool flipper_one_psci(ARMCPU *cpu, void *opaque)
{
    FlipperOneState *s = opaque;
    uint32_t fn;

    if (!is_a64(&cpu->env)) {
        return false;
    }
    fn = cpu->env.xregs[0];
    /* SMCCC 1.1, as TF-A has it (QEMU's PSCI does not): Linux calls its
     * firmware's other services through it, SCMI among them; no
     * ARCH_WORKAROUND (each would be an SMC at every exception entry) */
    if (fn == PSCI_FEATURES && cpu->env.xregs[1] == SMCCC_VERSION) {
        cpu->env.xregs[0] = 0;
        return true;
    }
    if (fn == SMCCC_VERSION) {
        cpu->env.xregs[0] = 0x10001;
        return true;
    }
    if (fn == SMCCC_ARCH_FEATURES) {
        cpu->env.xregs[0] = (uint64_t)-1;       /* NOT_SUPPORTED */
        return true;
    }
    if (fn == RK3576_SCMI_SMC) {                /* TF-A's SCMI server */
        rk_scmi_call(&s->rk3576.scmi);
        cpu->env.xregs[0] = 0;                  /* SMCCC: done */
        return true;
    }
    switch (fn & ~PSCI_64BIT) {
    case PSCI_SYSTEM_OFF:
        fo_rk_power(s, RK_OFF);
        break;
    case PSCI_SYSTEM_RESET:
    case PSCI_SYSTEM_RESET2:
        fo_rk_power(s, RK_RESTART);
        break;
    default:
        return false;
    }
    arm_set_cpu_off(arm_cpu_mp_affinity(cpu));  /* it stops now, as in PSCI */
    return true;
}

/* the Ethernet PHYs' INTB, low while asserted: GPIO1_C1 and GPIO1_C2 */
static void flipper_one_phy_int(void *opaque, int n, int level)
{
    FlipperOneState *s = opaque;

    rk3576_gpio_in(&s->rk3576, 1, 17 + n, !level);
}

/* the RK3576's CRU resets it (its TSADC's TSHUT): as TF-A's SYSTEM_RESET */
static void flipper_one_rk_cru_reset(void *opaque)
{
    fo_rk_power(opaque, RK_RESTART);
}

/* Rockusb's reset, as a usbplug does it (rk3576_maskrom.c): @op 3 into the
 * MaskROM (the boot mode, as Linux's `reboot maskrom` writes it), 2 off (as
 * TF-A's SYSTEM_OFF), the others a reset */
static void flipper_one_rockusb_reset(void *opaque, int op)
{
    FlipperOneState *s = opaque;

    if (op == 3) {
        rk3576_brom_download_ask(&s->rk3576);
    }
    fo_rk_power(s, op == 2 ? RK_OFF : RK_RESTART);
}

/* the RK3576's cores, with no kernel to start: reset, and off */
static void flipper_one_rk3576_reset(void *opaque)
{
    RK3576State *soc = opaque;

    for (int i = 0; i < RK3576_NCPUS; i++) {
        cpu_reset(CPU(&soc->cpu[i]));
    }
}

/*
 * The kernel's tree, booted directly: its /chosen gets a seed for the RNG
 * (from this PC, or -seed's), as U-Boot gives one on the device, so that
 * its entropy is there from the start. Not U-Boot's seed for KASLR: on
 * these cores (no E0PD) KASLR forces KPTI, which still costs a boot some
 * time, if far less since its flips keep QEMU's TLB (patch 0024; `run
 * --uboot` has it, as the device does). Its CPUs' power-down idle states
 * go (fo_fdt_idle_states). The images are kept as loaded, so each start
 * of this run has the same seed.
 */
static void flipper_one_modify_dtb(const struct arm_boot_info *info,
                                   void *fdt)
{
    FlipperOneState *s = container_of(info, FlipperOneState, binfo);
    uint8_t rng[32];
    int chosen;

    fo_fdt_idle_states(s, fdt);
    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0 || qemu_guest_getrandom(rng, sizeof(rng), NULL)) {
        return;
    }
    fdt_setprop(fdt, chosen, "rng-seed", rng, sizeof(rng));
}

/*
 * The CPU, the RK3576, and its DRAM at 0x40000000 (-m, 4 GiB unless told).
 * A kernel (-kernel, -dtb, -initrd, -append) starts on it when the MCU
 * switches its rail on, as U-Boot would start it: at EL2, PSCI from QEMU
 * over SMC; or U-Boot from its microSD card (uboot=on); without either its
 * cores stay off.
 */
static void flipper_one_rk3576(FlipperOneState *s, MachineState *ms)
{
    object_initialize_child(OBJECT(ms), "rk3576", &s->rk3576, TYPE_RK3576);
    /* its UFS's own state (patch 0017): -drive if=none,id=ufsnv */
    s->rk3576.ufs_nvram = blk_by_name("ufsnv");
    s->rk3576.ufs_boot_enable = true;
    s->rk3576.saradc_in = flipper_one_saradc;
    s->rk3576.saradc_in_opaque = s;
    qdev_prop_set_bit(DEVICE(&s->rk3576), "start-on",
                      ms->kernel_filename != NULL || s->uboot ||
                      s->falcon);
    /* its I2C2 is the main board's bus, the MCU's PIO I2C's too */
    object_property_set_link(OBJECT(&s->rk3576), "i2c2-bus",
                             OBJECT(s->main_bus.bus), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->rk3576), &error_fatal);
    s->hdmi = s->panel->hdmi;                   /* (a monitor from the start) */
    rk3576_hdmi_plug(&s->rk3576, s->hdmi);
    memory_region_add_subregion(get_system_memory(), RK3576_DRAM_BASE,
                                ms->ram);
    /* its USB0's gadget pulls D+ up for the computer plugged in */
    s->rk3576.usb[0].pullup_fn = fo_usb_host_pullup;
    s->rk3576.usb[0].pullup_opaque = s->usb_host;
    s->rk3576.maskrom.pullup_fn = fo_usb_host_pullup;
    s->rk3576.maskrom.pullup_opaque = s->usb_host;
    s->rk3576.maskrom.run = fo_maskrom_run;
    s->rk3576.maskrom.run_opaque = s;
    s->rk3576.maskrom.reset = flipper_one_rockusb_reset;
    s->rk3576.maskrom.reset_opaque = s;
    /* the MCU on I2C0, its PMIC on I2C1, its real-time clock on I2C8 */
    s->link = (RkI2cTarget){
        .start = flipper_one_link_start, .write = flipper_one_link_write,
        .read = flipper_one_link_read, .nack = flipper_one_link_nack,
        .stop = flipper_one_link_stop, .opaque = s,
    };
    rk3576_i2c_target(&s->rk3576, 0, &s->link);
    rp2350_i2c_target_on_release(&s->mcu, 1, flipper_one_link_release, s);
    rk3576_i2c_arbiter(&s->rk3576, 2, flipper_one_main_bus_claim, s);
    /* a microSD card (-drive if=sd): its detect switch, GPIO0_A7, low */
    {
        DriveInfo *di = drive_get(IF_SD, 0, 0);

        if (di) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                                    &error_fatal);
            qdev_realize_and_unref(card,
                                   BUS(dw_mshc_bus(&s->rk3576.sdmmc)),
                                   &error_fatal);
            rk3576_gpio_in(&s->rk3576, 0, 7, false);
            s->sd_blk = blk_by_legacy_dinfo(di);
        }
    }
    /*
     * The UFS, provisioned as the Flipper OS installer's scheme has it (as if
     * it had done it once: a factory-fresh one it would provision first),
     * each LU its own drive (-drive if=none,id=ufsN, the launcher's):
     * LU 0, the system's, what the others leave; LU 1 and 2, boot LUs A and
     * B (enhanced1, 16 MiB), which the boot ROM loads from, bBootLunEn
     * picking one; LU 3, recovery (enhanced1, 128 MiB). Each with its data
     * protected against a power failure and thin provisioning, as the
     * scheme asks
     */
    {
        static const struct { const char *id; uint8_t boot, mem; } lus[] = {
            { "ufs", 0, 0 }, { "ufs1", 1, 3 }, { "ufs2", 2, 3 },
            { "ufs3", 0, 3 },
        };

        for (int i = 0; i < ARRAY_SIZE(lus); i++) {
            BlockBackend *blk = blk_by_name(lus[i].id);
            DeviceState *lu;

            if (!blk) {
                continue;
            }
            lu = qdev_new("ufs-lu");
            qdev_prop_set_drive_err(lu, "drive", blk, &error_fatal);
            qdev_prop_set_uint8(lu, "lun", i);
            qdev_prop_set_uint8(lu, "boot-lun-id", lus[i].boot);
            qdev_prop_set_uint8(lu, "memory-type", lus[i].mem);
            qdev_prop_set_uint8(lu, "data-reliability", 1);
            qdev_prop_set_uint8(lu, "provisioning-type", 2);    /* thin */
            qdev_realize_and_unref(lu, rk3576_ufs_bus(&s->rk3576),
                                   &error_fatal);
        }
    }
    /* the display link: SPI0's frames to the MCU */
    s->frame = g_byte_array_sized_new(FO_LCD_H * (FO_LCD_W + 2));
    fo_host_spi_init(&s->linux_frames, &s->mcu, flipper_one_frame_sent, s);
    rk_spi_set_out(&s->rk3576.spi0, flipper_one_spi0_out, s);
    s->rk3576.gpio_out = flipper_one_rk_gpio;
    s->rk3576.gpio_out_opaque = s;
    /*
     * The MT7921 (its Bluetooth), on USB1's second USB 2 port (on the
     * device behind the board's hub). Its PMU_EN is GPIO1_D5, which the
     * device tree's rfkill-gpio "Onboard WiFi+BT" drives (blocked by
     * default): pulled down, the chip off until Linux unblocks it
     */
    {
        DeviceState *bt = qdev_new(TYPE_MT7921);

        qdev_prop_set_string(bt, "port", "2");
        /* (its access point's uplink: a -nic of model mt7921, if any) */
        qemu_configure_nic_device(bt, false, "mt7921");
        qdev_realize_and_unref(bt, BUS(&s->rk3576.usb[1].xhci.xhci.bus),
                               &error_fatal);
        s->mt7921 = USB_DEVICE(bt);
        rk3576_gpio_in(&s->rk3576, 1, 29, false);
    }
    s->rk3576.cru_reset = flipper_one_rk_cru_reset;
    s->rk3576.cru_reset_opaque = s;
    for (int i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->rk3576.gmac[i]), "phy-int", 0,
                                    qemu_allocate_irq(flipper_one_phy_int,
                                                      s, i));
    }
    i2c_slave_create_simple(rk3576_i2c_bus(&s->rk3576, 1), "rk806", 0x23);
    i2c_slave_create_simple(rk3576_i2c_bus(&s->rk3576, 8), "hym8563", 0x51);
    /* the audio codec on I2C6, on SAI2's I2S link: -global
     * nau8822.audiodev=ID for where its sound goes, -global
     * nau8822.mic=FILE.wav for what its internal microphone hears */
    {
        DeviceState *codec = qdev_new(TYPE_NAU8822);
        RkSaiState *sai = rk3576_sai(&s->rk3576, 2);

        s->codec = codec;

        qdev_prop_set_uint8(codec, "address", 0x1a);
        i2c_slave_realize_and_unref(I2C_SLAVE(codec),
                                    rk3576_i2c_bus(&s->rk3576, 6),
                                    &error_fatal);
        nau8822_link(codec, rk_sai_lrck, sai);
        rk_sai_set_sink(sai, nau8822_dac, codec);
        rk_sai_set_source(sai, nau8822_adc, codec);
        nau8822_headset_mic(codec, s->headset == FO_HEADSET_IN);
    }
    if ((s->uboot != 0) + (ms->kernel_filename != NULL) +
        (s->falcon != NULL) > 1) {
        error_report("flipper-one: one of uboot=on, falcon=FILE, -kernel");
        exit(1);
    }
    if (!ms->kernel_filename && !s->uboot && !s->falcon) {
        qemu_register_reset(flipper_one_rk3576_reset, &s->rk3576);
        return;
    }
    /* the kernel's memory after the 2 MiB that are TF-A's (its SCMI shared
     * memory among them), as the loader gives it; with U-Boot, QEMU loads
     * nothing (SPL's part is fo_spl()) and gives its PSCI */
    s->binfo.ram_size = ms->ram_size - RK3576_TFA_SIZE;
    s->binfo.loader_start = RK3576_DRAM_BASE + RK3576_TFA_SIZE;
    s->binfo.board_id = -1;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    s->binfo.firmware_loaded = s->uboot || s->falcon;
    s->binfo.modify_dtb = flipper_one_modify_dtb;
    arm_load_kernel(&s->rk3576.cpu[0], ms, &s->binfo);
    /* (after arm_load_kernel's: reset handlers run in that order) */
    s->linux_boot = true;
    s->rk_power_bh = qemu_bh_new(fo_rk_power_bh, s);
    qemu_register_reset(flipper_one_rk3576_gate, s);
    for (int i = 0; i < RK3576_NCPUS; i++) {
        s->rk3576.cpu[i].psci_hook = flipper_one_psci;
        s->rk3576.cpu[i].psci_hook_opaque = s;
    }
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
    s->panel->hdmi = s->hdmi_at_start;

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
    s->mcu.adc_mv[7] = 0;                       /* the jack: empty */
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
    qdev_connect_gpio_out_named(DEVICE(s->main_expander), "pins", 15,
                                qemu_allocate_irq(flipper_one_maskrom, s, 0));
    /* pins 0-7 are inputs, the board's faults, interrupts and power-goods,
     * open drain: pulled up. 8-15 are enables (the rails, the USB mux,
     * MASKROM_EN): pulled down, so that they are off while the expander's
     * pins are inputs. The firmware's init reads them back and keeps what
     * it finds: across an MCU restart the levels the expander still drives
     * (Linux runs on); from a cold start, nothing on */
    s->main_inputs = MAIN_INPUTS;
    if (!s->bootsel) {
        pcal6416_preset(s->main_expander, MAIN_INPUTS, MAIN_HOST_ON);
    }
    s->battery.load_ma = 40;
    bq28z620_attach(i2c_slave_create_simple(s->main_bus.bus, "bq28z620",
                                            0x55), &s->battery);
    s->charger = i2c_slave_create_simple(s->main_bus.bus, "bq2579x", 0x6b);
    bq2579x_attach(s->charger, &s->battery);
    bq2579x_on_supply(s->charger, flipper_one_supply, s);
    s->power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, flipper_one_power_back,
                                  s);
    qdev_connect_gpio_out(DEVICE(s->charger), 0,
                          qemu_allocate_irq(flipper_one_main_input, s, 2));
    ina219_attach(i2c_slave_create_simple(s->main_bus.bus, "ina219", 0x45),
                  &s->battery);
    for (int i = 0; i < 5; i++) {
        static const uint8_t addr[5] = { 0x40, 0x41, 0x43, 0x44, 0x46 };
        int rail[4];
        uint32_t shunt[4];

        for (int c = 0; c < 4; c++) {
            rail[c] = 4 * i + c;
            shunt[c] = fo_rails[4 * i + c].shunt_uohm;
        }
        ina4230_attach(i2c_slave_create_simple(s->main_bus.bus, "ina4230",
                                               addr[i]),
                       flipper_one_rail, s, rail, shunt);
    }
    s->usb_pd = i2c_slave_create_simple(s->main_bus.bus, "fusb302", 0x22);
    qdev_connect_gpio_out(DEVICE(s->usb_pd), 0,
                          qemu_allocate_irq(flipper_one_main_input, s, 3));
    s->usb_mux = i2c_slave_create_simple(s->main_bus.bus, "hd3ss3220", 0x47);
    fo_battery_start(&s->battery);

    /* the USB-C port's USB 2: the mux (the main expander's pin 8), and the
     * computer at the cable's end: -global flipper-one-usb-host.cdc=CHARDEV
     * for the CDC serial port it finds */
    qdev_connect_gpio_out_named(DEVICE(s->main_expander), "pins", 8,
                                qemu_allocate_irq(flipper_one_usb20_sel, s,
                                                  0));
    s->usb_host = qdev_new(TYPE_FO_USB_HOST);
    object_property_add_child(OBJECT(ms), "usb-host", OBJECT(s->usb_host));
    qdev_realize_and_unref(s->usb_host, NULL, &error_fatal);
    s->mcu.usb.pullup_fn = fo_usb_host_pullup;
    s->mcu.usb.pullup_opaque = s->usb_host;
    s->expander_reset[0] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        flipper_one_buttons_reset, s);
    s->expander_reset[1] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        flipper_one_main_reset, s);
    rp2350_pin_listen(&s->mcu, 43, flipper_one_expander_reset, s);
    rp2350_pin_listen(&s->mcu, 38, flipper_one_expander_reset, s);

    flipper_one_rk3576(s, ms);
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
    int32_t value = fo_battery_percent(&FLIPPER_ONE(obj)->battery);

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
    fo_battery_set_percent(&FLIPPER_ONE(obj)->battery, value);
}

static void flipper_one_instance_init(Object *obj)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    s->battery.design_mah = 3100;               /* (the device tree's) */
    s->ambient_dc = 250;                        /* 25 °C */
    s->battery.temp_dk = 2982;
    fo_battery_set_percent(&s->battery, 80);
}

static bool flipper_one_get_bootsel(Object *obj, Error **errp)
{
    return FLIPPER_ONE(obj)->bootsel;
}

static void flipper_one_set_bootsel(Object *obj, bool value, Error **errp)
{
    FLIPPER_ONE(obj)->bootsel = value;
}

static bool flipper_one_get_hdmi(Object *obj, Error **errp)
{
    return FLIPPER_ONE(obj)->hdmi_at_start;
}

static void flipper_one_set_hdmi(Object *obj, bool value, Error **errp)
{
    FLIPPER_ONE(obj)->hdmi_at_start = value;
}

static bool flipper_one_get_deep_idle(Object *obj, Error **errp)
{
    return FLIPPER_ONE(obj)->deep_idle;
}

static void flipper_one_set_deep_idle(Object *obj, bool value, Error **errp)
{
    FLIPPER_ONE(obj)->deep_idle = value;
}

static bool flipper_one_get_uboot(Object *obj, Error **errp)
{
    return FLIPPER_ONE(obj)->uboot;
}

static void flipper_one_set_uboot(Object *obj, bool value, Error **errp)
{
    FLIPPER_ONE(obj)->uboot = value;
}

static char *flipper_one_get_falcon(Object *obj, Error **errp)
{
    return g_strdup(FLIPPER_ONE(obj)->falcon);
}

static void flipper_one_set_falcon(Object *obj, const char *value,
                                   Error **errp)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    g_free(s->falcon);
    s->falcon = *value ? g_strdup(value) : NULL;
}

static char *flipper_one_get_rev(Object *obj, Error **errp)
{
    return g_strdup(FLIPPER_ONE(obj)->rev ?: FO_REV_DEFAULT);
}

static void flipper_one_set_rev(Object *obj, const char *value, Error **errp)
{
    FlipperOneState *s = FLIPPER_ONE(obj);

    if (strcmp(value, "f0b0c1") && strcmp(value, "f0b1c2")) {
        error_setg(errp, "rev: f0b0c1 or f0b1c2");
        return;
    }
    g_free(s->rev);
    s->rev = g_strdup(value);
}

static void flipper_one_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Flipper One: its MCU (a Raspberry Pi RP2350B) and its CPU "
               "(a Rockchip RK3576)";
    mc->init = flipper_one_init;
    mc->default_ram_size = 4 * GiB;             /* the RK3576's DRAM */
    mc->default_ram_id = "flipper-one.dram";
    /* the RP2350's two cores and the RK3576's eight (TCG gives a thread to
     * as many as this says) */
    mc->default_cpus = 2 + RK3576_NCPUS;
    mc->min_cpus = 2 + RK3576_NCPUS;
    mc->max_cpus = 2 + RK3576_NCPUS;
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
        "Start with the RP2350's BOOTSEL held (and let it go after): its "
        "boot ROM's USB mode");
    object_class_property_add(oc, "battery", "int", flipper_one_get_battery,
                              flipper_one_set_battery, NULL, NULL);
    object_class_property_set_description(oc, "battery",
        "The battery's charge at the start, 0 to 100 (%); 80 unless told");
    object_class_property_add_bool(oc, "uboot", flipper_one_get_uboot,
                                   flipper_one_set_uboot);
    object_class_property_set_description(oc, "uboot",
        "Start the RK3576 at U-Boot proper from its microSD card, as SPL "
        "loads it from u-boot.itb (sector 16384), the machine in TF-A's "
        "place and the DDR blob's (in -kernel's stead)");
    object_class_property_add_str(oc, "falcon", flipper_one_get_falcon,
                                  flipper_one_set_falcon);
    object_class_property_set_description(oc, "falcon",
        "Start the RK3576 at a FIT for SPL's Falcon mode, as the Flipper OS "
        "installer is: its kernel, ramdisk and device tree, as SPL and TF-A "
        "would");
    object_class_property_add_str(oc, "rev", flipper_one_get_rev,
                                  flipper_one_set_rev);
    object_class_property_set_description(oc, "rev",
        "The board's revision, as its device trees name it (f0b0c1 unless "
        "told, or f0b1c2): the configuration of u-boot.itb that SPL takes");
    object_class_property_add_bool(oc, "deep-idle", flipper_one_get_deep_idle,
                                   flipper_one_set_deep_idle);
    object_class_property_set_description(oc, "deep-idle",
        "Keep the CPUs' power-down idle states (cpu-sleep) in the kernel's "
        "tree, in a direct boot or SPL's Falcon mode: the device's tree as "
        "it is. QEMU's PSCI runs them as a WFI; without them Linux idles "
        "with WFI, and spares itself their way in and out");
    object_class_property_add_bool(oc, "hdmi", flipper_one_get_hdmi,
                                   flipper_one_set_hdmi);
    object_class_property_set_description(oc, "hdmi",
        "A monitor in the HDMI port from the start (the panel link's "
        "\"hdmi 1|0\" plugs one in and out); its picture, the VOP's video "
        "port 0, is QEMU's first graphic console");
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
