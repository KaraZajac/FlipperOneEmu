/*
 * Flipper One: the board's parts around its chips
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HW_ARM_FLIPPER_ONE_FLIPPER_ONE_H
#define HW_ARM_FLIPPER_ONE_FLIPPER_ONE_H

#include "chardev/char-fe.h"
#include "hw/core/qdev.h"
#include "hw/i2c/bitbang_i2c.h"
#include "qemu/timer.h"
#include "qom/object.h"

/* ------------------------------------------------------- a line link */

typedef struct FlipperLink {
    CharFrontend *chr;
    const char *name;            /* for the log: "panel" */
    GString *in, *out;           /* a partial line in; bytes not yet sent */
    guint watch;
    bool open;                   /* a program is connected */
    void (*on_line)(void *opaque, char *line);
    void (*on_open)(void *opaque, bool open);
    void *opaque;
    /* optional: false = the device can take no line now; reading stops until
     * it calls flipper_link_resume() */
    bool (*ready)(void *opaque);
    bool held, busy;
} FlipperLink;
void flipper_link_resume(FlipperLink *l);
void flipper_link_init(FlipperLink *l, CharFrontend *chr, const char *name,
                       void (*on_line)(void *opaque, char *line),
                       void (*on_open)(void *opaque, bool open), void *opaque);
void flipper_link_send(FlipperLink *l, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ------------------------------------------------------ the front panel */

#define FO_LCD_W        256
#define FO_LCD_H        144
#define FO_LEDS         17

/*
 * The buttons, in the order of the control bus's PCAL6416 inputs (the
 * firmware's InputKey bits): the soft keys under the screen are Key1 (Esc),
 * Key2 (View), Power, Key4 (Edit) and Key5 (Run).
 */
enum {
    FO_KEY_VIEW, FO_KEY_ESC, FO_KEY_POWER, FO_KEY_EDIT, FO_KEY_RUN,
    FO_KEY_SWITCH, FO_KEY_BACK, FO_KEY_DOWN, FO_KEY_RIGHT, FO_KEY_OK,
    FO_KEY_LEFT, FO_KEY_UP, FO_KEY_PTT,
    FO_KEY_NUM
};

#define TYPE_FLIPPER_ONE_PANEL "flipper-one-panel"
OBJECT_DECLARE_SIMPLE_TYPE(FlipperOnePanelState, FLIPPER_ONE_PANEL)

struct FlipperOnePanelState {
    DeviceState parent_obj;

    CharFrontend link_chr;
    FlipperLink lk;
    QEMUTimer *tick;

    /* what the panel shows: 0 black ... 255 white, as the LCD has it */
    uint8_t fb[FO_LCD_H][FO_LCD_W];
    bool lcd_on;                        /* the controller's display is on */
    uint8_t backlight;                  /* 0 ... 255 */
    uint32_t leds[FO_LEDS];             /* 0xRRGGBB, in the WS2812s' order */
    bool haptic;                        /* the vibration motor runs */
    bool haptic_blip;                   /* ... it ran since the last tick */
    /* ... and what the program was last told of it */
    uint8_t sent[FO_LCD_H][FO_LCD_W];
    bool sent_lcd_on;
    int sent_backlight;
    uint32_t sent_leds[FO_LEDS];
    bool sent_haptic;
    int sent_running;                   /* -1: nothing yet */

    /* what the program holds down, and where it touches the touchpad */
    uint16_t keys;
    bool touching;
    uint16_t touch_x, touch_y;          /* 0 ... 1000 across the pad */
    uint8_t touch_p;
    int usb;                            /* in the USB-C port: 0 nothing, 1 a
                                           charger, 2 a computer; it stays
                                           when the program goes */
    int headset;                        /* in the jack: FO_HEADSET_... */
    bool powered;                       /* the board has its supply */
    int rk3576;                         /* the RK3576 runs: 1, 0, or -1 (no
                                           kernel for it: not told) */

    /* the board's input parts are told of a change */
    void (*input)(void *opaque);
    void *input_opaque;
};

/* a headset in the jack, and the button on its cable held */
enum {
    FO_HEADSET_NONE, FO_HEADSET_IN, FO_HEADSET_PLAY, FO_HEADSET_VOLUP,
    FO_HEADSET_VOLDOWN, FO_HEADSET_ASSIST, FO_HEADSET_NUM
};

/* the parts that draw and light: what changed is sent on the next tick */
void fo_panel_backlight(FlipperOnePanelState *s, uint8_t level);
void fo_panel_led(FlipperOnePanelState *s, int index, uint32_t rgb);
void fo_panel_lcd_on(FlipperOnePanelState *s, bool on);
void fo_panel_haptic(FlipperOnePanelState *s, bool on);
/* the board's supply came or went: told at once */
void fo_panel_power(FlipperOnePanelState *s, bool on);
void fo_panel_rk3576(FlipperOnePanelState *s, bool on);

/* ------------------------------------------- the display controller */

struct RP2350State;

typedef struct FoJd9853 {
    struct RP2350State *soc;
    FlipperOnePanelState *panel;
    bool in_reset, selected;
    int phase, bits, naddr;
    uint8_t cur, instr, cmd;
    uint8_t params[64];
    int nparams;
    uint32_t pos;                       /* RAMWR's place in the 258 x 144 */
    bool te_on, sleep, disp_on, te_level;
    QEMUTimer *te;
} FoJd9853;

void fo_jd9853_init(FoJd9853 *d, struct RP2350State *soc,
                    FlipperOnePanelState *panel);
/* its supply went: reset, nothing shown, until its RESET pin lets go */
void fo_jd9853_unpowered(FoJd9853 *d);

/* ------------------------------------------------- the host link */

#define TYPE_FLIPPER_ONE_HOST "flipper-one-host"
OBJECT_DECLARE_SIMPLE_TYPE(FoHost, FLIPPER_ONE_HOST)

struct HostOp;

/*
 * host_spi.c: the RK3576's display link, a frame at a time as Flipper OS
 * sends it: "active" low on GPIO25, SPI mode 3 at 33 MHz on GPIO26 (SCK) and
 * GPIO28 (MOSI), "active" up
 */
typedef struct FoHostSpi {
    struct RP2350State *soc;
    QEMUTimer *timer;
    GQueue *frames;                     /* GByteArray: frames to send */
    GByteArray *cur;                    /* the one going out */
    size_t bit;                         /* ... its next bit */
    int phase;
    int64_t t;                          /* the next change's time (ns) */
    bool driving;                       /* the lines are taken */
    bool closing;                       /* ... until this frame is out */
    void (*sent)(void *opaque);         /* a frame went out */
    void *opaque;
} FoHostSpi;

void fo_host_spi_init(FoHostSpi *f, struct RP2350State *soc,
                      void (*sent)(void *opaque), void *opaque);
void fo_host_spi_send(FoHostSpi *f, GByteArray *frame);
/* frames not started yet */
int fo_host_spi_waiting(FoHostSpi *f);
/* the program went: no more frames, the lines let go after this one */
void fo_host_spi_close(FoHostSpi *f);

struct FoHost {
    DeviceState parent_obj;

    CharFrontend link_chr;
    FlipperLink lk;
    QEMUTimer *timer;
    struct RP2350State *soc;            /* the machine's: the MCU, */
    int i2c;                            /* ... its controller (1) */
    uint8_t addr;                       /* ... at this address (0x69) */
    GQueue *ops;                        /* transfers to do */
    struct HostOp *op;                  /* the one being done */
    int state, pos;
    int64_t started;
    bool int_level;                     /* CPU_INT, as last told */
    FoHostSpi spi;                      /* the display link */
};

/* host_link.c: CPU_INT's level (0: asserted) */
void fo_host_int(FoHost *h, bool level);

/* ------------------------------------------------------- the LEDs */

#define FO_LED_CHAINS   3

typedef struct FoLedChain {
    bool powered;
    int64_t rise_ns, fall_ns;
    int bits;                           /* since the chain started again */
    uint32_t shift;
} FoLedChain;

typedef struct FoLeds {
    FlipperOnePanelState *panel;
    FoLedChain chain[FO_LED_CHAINS];
} FoLeds;

/* ws2812.c: the chains on GPIO29-31; a chain's supply, on or off */
void fo_leds_init(FoLeds *l, struct RP2350State *soc,
                  FlipperOnePanelState *panel);
void fo_leds_power(FoLeds *l, int chain, bool on);

/* ------------------------------------------------- the control bus */

/* pcal6416.c: the levels outside drives its pins to (1: high); its RESET is
 * GPIO input 0 (low: held in reset), its INT GPIO output 0 */
struct I2CSlave;
void pcal6416_set_pins(struct I2CSlave *dev, uint16_t levels);

/* usb_host.c: the computer at the other end of the MCU's USB: whether
 * its data lines reach the MCU's */
#define TYPE_FO_USB_HOST "flipper-one-usb-host"
void fo_usb_host_attach(DeviceState *dev, struct RP2350State *soc);
void fo_usb_host_cable(DeviceState *dev, bool data);

/* iqs7211e.c: the finger on the touchpad (0 ... 1000 across and down) and
 * its pressure (0 ... 255), or none */
void iqs7211e_touch(struct I2CSlave *dev, bool touching, int x, int y, int p);

/* ------------------------------------------- the battery, as it is */

/*
 * The battery as the power chips see it (battery.c): a two-cell lithium-ion
 * pack (the firmware's charge limit is 8.65 V). Its numbers are assumed: 3000
 * mAh, 25 °C, its charge at the start the machine's "battery" property (80 %
 * unless told). Its charge follows its current over the virtual clock's
 * time: out of it, what the board draws; into it, what the charger gives it
 * (bq2579x.c works that out, its "flow").
 */
typedef struct FoBattery {
    double mah;                         /* the charge in it */
    int design_mah;
    int temp_dk;                        /* 0.1 K */
    int load_ma;                        /* what the board draws (VSYS) */
    int ibat_ma;                        /* its current: + into it */
    int64_t at_ns;                      /* the charge's time */
    QEMUTimer *tick;
    /* its current, from the charger, as things are now */
    int (*flow)(void *opaque, struct FoBattery *b);
    void *flow_opaque;
} FoBattery;

/* the clock starts for it */
void fo_battery_start(FoBattery *b);
/* its charge up to now, then its current again: before anything that
 * depends on them is read, and after anything they depend on changed */
void fo_battery_update(FoBattery *b);
int fo_battery_percent(const FoBattery *b);
void fo_battery_set_percent(FoBattery *b, int percent);
/* its voltage at rest; at its terminals, at its current (mV) */
int fo_battery_ocv_mv(const FoBattery *b);
int fo_battery_mv(const FoBattery *b);

/* bq28z620.c, bq2579x.c, ina219.c: the gauge, the charger and the system
 * rail's monitor measure @bat */
void bq28z620_attach(struct I2CSlave *dev, FoBattery *bat);
void bq2579x_attach(struct I2CSlave *dev, FoBattery *bat);
void ina219_attach(struct I2CSlave *dev, FoBattery *bat);

/* bq2579x.c: a source on VBUS (@mv, giving up to @ma; @dcp: a dedicated
 * charger, else a computer's port), or none (0) */
void bq2579x_input(struct I2CSlave *dev, int mv, int ma, bool dcp);
/* bq2579x.c: the ship FET's control (SDRV_CTRL) acting on the board's
 * supply: @fn gets what it does, FO_SUPPLY_... */
enum { FO_SUPPLY_SHUTDOWN = 1, FO_SUPPLY_SHIP = 2, FO_SUPPLY_RESET = 3 };
void bq2579x_on_supply(struct I2CSlave *dev,
                       void (*fn)(void *opaque, int what), void *opaque);
/* usb_c.c: a source on the USB-C port, offering @rp_ma (its Rp), or none */
void fusb302_attach(struct I2CSlave *dev, int rp_ma);
void hd3ss3220_attach(struct I2CSlave *dev, int rp_ma);

/* ---------------------------------------------- the main bus, on pins */

typedef struct FoI2cPins {
    struct RP2350State *soc;
    int sda, scl;
    I2CBus *bus;
    bitbang_i2c_interface bb;
    bool sda_low;                       /* a chip pulls SDA low */
} FoI2cPins;

/* i2c_pins.c: a QEMU I2C bus behind a master that bit-bangs @sda, @scl */
I2CBus *fo_i2c_pins_init(FoI2cPins *b, struct RP2350State *soc, int sda,
                         int scl, const char *name);

#endif
