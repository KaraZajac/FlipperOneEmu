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

    /* the board's input parts are told of a change */
    void (*input)(void *opaque);
    void *input_opaque;
};

/* the parts that draw and light: what changed is sent on the next tick */
void fo_panel_backlight(FlipperOnePanelState *s, uint8_t level);
void fo_panel_led(FlipperOnePanelState *s, int index, uint32_t rgb);
void fo_panel_lcd_on(FlipperOnePanelState *s, bool on);
void fo_panel_haptic(FlipperOnePanelState *s, bool on);

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

/* ------------------------------------------------- the control bus */

/* pcal6416.c: the levels outside drives its pins to (1: high); its RESET is
 * GPIO input 0 (low: held in reset), its INT GPIO output 0 */
struct I2CSlave;
void pcal6416_set_pins(struct I2CSlave *dev, uint16_t levels);

/* iqs7211e.c: the finger on the touchpad (0 ... 1000 across and down) and
 * its pressure (0 ... 255), or none */
void iqs7211e_touch(struct I2CSlave *dev, bool touching, int x, int y, int p);

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
