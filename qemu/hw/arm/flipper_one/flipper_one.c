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

#define TYPE_FLIPPER_ONE MACHINE_TYPE_NAME("flipper-one")
OBJECT_DECLARE_SIMPLE_TYPE(FlipperOneState, FLIPPER_ONE)

struct FlipperOneState {
    MachineState parent_obj;

    RP2350State mcu;
    RK3576State rk3576;         /* the CPU: Linux's */
    RkI2cTarget link;           /* its I2C0's target: the MCU's I2C1 */
    bool linux_boot;            /* a kernel to start on it (-kernel) */
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
    uint64_t frames_sent, frames_dropped;
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
    int headset;                /* in the jack: FO_HEADSET_... */
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

/*
 * The display link: Flipper OS sends a frame over SPI0 (CS0) between
 * "active" going low and back high, the RK3576's GPIO0_D1 (pin 25); the
 * frame then goes to the MCU as the RK3576 sends it, edge by edge on its
 * GPIO25 (active), 26 (SCK) and 28 (MOSI) (host_spi.c), after the frame
 * before it. Frames that come while two wait to go are dropped.
 */
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

    if (bank != 0 || pin != 25) {
        return;
    }
    if (!level) {                               /* active: a frame */
        s->frame_active = true;
        g_byte_array_set_size(s->frame, 0);
    } else if (s->frame_active) {
        s->frame_active = false;
        if (!s->frame->len) {
            return;
        }
        if (fo_host_spi_waiting(&s->linux_frames) >= 2) {
            s->frames_dropped++;
            return;
        }
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
 * each time after the first: a run writes over them). Without its rail,
 * or after Linux's PSCI SYSTEM_OFF, the cores are off and its devices as
 * at power-on; SYSTEM_RESET starts the kernel again. The MCU runs on
 * through all of it.
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

static void fo_rk_cpu_off(ARMCPU *cpu)
{
    arm_set_cpu_power_state(cpu, PSCI_OFF);
    CPU(cpu)->halted = 1;
}

/* in the main loop, the cores stopped: off, on, or started again */
static void fo_rk_power_bh(void *opaque)
{
    FlipperOneState *s = opaque;
    bool running = runstate_is_running();
    int want = s->rk_want;

    if (want != RK_OFF && !s->rk_rail) {
        want = RK_OFF;
    }
    if (want == RK_ON && s->rk_on && !s->rk_lost) {
        return;                                 /* (on already) */
    }
    s->rk_lost = false;
    if (running) {
        pause_all_vcpus();
    }
    rk3576_reset_devices(&s->rk3576);
    s->frame_active = false;
    if (want != RK_OFF) {
        if (!s->rk_nimages) {
            fo_rk_save_images(s);               /* the first start: as loaded */
        } else {
            fo_rk_restore_images(s);
        }
        rk_scmi_reset(&s->rk3576.scmi);         /* (TF-A's start) */
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        ARMCPU *cpu = &s->rk3576.cpu[i];
        CPUState *cs = CPU(cpu);

        cpu_reset(cs);                          /* (the others: off, for
                                                   PSCI's CPU_ON) */
        if (want != RK_OFF && i == 0) {
            arm_emulate_firmware_reset(cs, 2);  /* as QEMU's own direct boot */
            cpu_set_pc(cs, s->binfo.loader_start);
            arm_rebuild_hflags(&cpu->env);
        } else {
            fo_rk_cpu_off(cpu);
        }
    }
    s->rk_on = want != RK_OFF;
    if (running) {
        resume_all_vcpus();
    }
    qemu_log("flipper-one: %" PRId64 " ms: the RK3576 %s\n",
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
             want == RK_OFF ? "is off" : want == RK_ON ? "starts" :
             "starts again");
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

/* the RK3576's cores, with no kernel to start: reset, and off */
static void flipper_one_rk3576_reset(void *opaque)
{
    RK3576State *soc = opaque;

    for (int i = 0; i < RK3576_NCPUS; i++) {
        cpu_reset(CPU(&soc->cpu[i]));
    }
}

/*
 * The CPU, the RK3576, and its DRAM at 0x40000000 (-m, 4 GiB unless told).
 * A kernel (-kernel, -dtb, -initrd, -append) starts on it when the MCU
 * switches its rail on, as U-Boot would start it: at EL2, PSCI from QEMU
 * over SMC; without one its cores stay off.
 */
static void flipper_one_rk3576(FlipperOneState *s, MachineState *ms)
{
    object_initialize_child(OBJECT(ms), "rk3576", &s->rk3576, TYPE_RK3576);
    qdev_prop_set_bit(DEVICE(&s->rk3576), "start-on",
                      ms->kernel_filename != NULL);
    /* its I2C2 is the main board's bus, the MCU's PIO I2C's too */
    object_property_set_link(OBJECT(&s->rk3576), "i2c2-bus",
                             OBJECT(s->main_bus.bus), &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->rk3576), &error_fatal);
    memory_region_add_subregion(get_system_memory(), RK3576_DRAM_BASE,
                                ms->ram);
    /* its USB0's gadget pulls D+ up for the computer plugged in */
    s->rk3576.usb[0].pullup_fn = fo_usb_host_pullup;
    s->rk3576.usb[0].pullup_opaque = s->usb_host;
    /* the MCU on I2C0, its PMIC on I2C1, its real-time clock on I2C8 */
    s->link = (RkI2cTarget){
        .start = flipper_one_link_start, .write = flipper_one_link_write,
        .read = flipper_one_link_read, .nack = flipper_one_link_nack,
        .stop = flipper_one_link_stop, .opaque = s,
    };
    rk3576_i2c_target(&s->rk3576, 0, &s->link);
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
        }
    }
    /* the UFS's user area, its LU 0 (-drive if=none,id=ufs: the device's
     * 64 GB, as the launcher makes it) */
    {
        BlockBackend *blk = blk_by_name("ufs");

        if (blk) {
            DeviceState *lu = qdev_new("ufs-lu");

            qdev_prop_set_drive_err(lu, "drive", blk, &error_fatal);
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
     * nau8822.audiodev=ID for where its sound goes */
    {
        DeviceState *codec = qdev_new(TYPE_NAU8822);
        RkSaiState *sai = rk3576_sai(&s->rk3576, 2);

        qdev_prop_set_uint8(codec, "address", 0x1a);
        i2c_slave_realize_and_unref(I2C_SLAVE(codec),
                                    rk3576_i2c_bus(&s->rk3576, 6),
                                    &error_fatal);
        nau8822_link(codec, rk_sai_lrck, sai);
        rk_sai_set_sink(sai, nau8822_dac, codec);
    }
    if (!ms->kernel_filename) {
        qemu_register_reset(flipper_one_rk3576_reset, &s->rk3576);
        return;
    }
    /* the kernel's memory after the 2 MiB that are TF-A's (its SCMI shared
     * memory among them), as the loader gives it */
    s->binfo.ram_size = ms->ram_size - RK3576_TFA_SIZE;
    s->binfo.loader_start = RK3576_DRAM_BASE + RK3576_TFA_SIZE;
    s->binfo.board_id = -1;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
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
    s->main_inputs = 0xffff;                    /* pulled up */
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

    s->battery.design_mah = 3000;
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
