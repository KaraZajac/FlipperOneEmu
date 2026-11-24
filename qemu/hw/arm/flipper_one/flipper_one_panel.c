/*
 * The Flipper One's front panel, for a program that shows it
 * (docs/PANEL_LINK.md): out go the screen, its backlight, the LEDs, the
 * vibration motor and whether the machine runs; in come the buttons, the
 * touchpad, and a charger plugged into the USB-C port or pulled out.
 *
 * The screen is the panel's 256x144 picture, 0 black to 255 white, which the
 * display controller's model draws into; what changed since the program was
 * last told goes out every 16 ms of the host's time, as runs of rows.  The
 * buttons are held as the program holds them; the board's input parts (the
 * control bus's expander, the touchpad's controller) are told of a change.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/runstate.h"
#include "flipper_one.h"

#define TICK_MS         16
#define ROWS_PER_LINE   16              /* 4 KiB of picture to a line at most */

static const char *const key_names[FO_KEY_NUM] = {
    [FO_KEY_VIEW] = "view",     [FO_KEY_ESC] = "esc",
    [FO_KEY_POWER] = "power",   [FO_KEY_EDIT] = "edit",
    [FO_KEY_RUN] = "run",       [FO_KEY_SWITCH] = "switch",
    [FO_KEY_BACK] = "back",     [FO_KEY_DOWN] = "down",
    [FO_KEY_RIGHT] = "right",   [FO_KEY_OK] = "ok",
    [FO_KEY_LEFT] = "left",     [FO_KEY_UP] = "up",
    [FO_KEY_PTT] = "ptt",
};

static void panel_input(FlipperOnePanelState *s)
{
    if (s->input) {
        s->input(s->input_opaque);
    }
}

static void panel_running(FlipperOnePanelState *s, bool all)
{
    int on = runstate_is_running();

    if (!all && on == s->sent_running) {
        return;
    }
    s->sent_running = on;
    if (on) {
        flipper_link_send(&s->lk, "running 1\n");
    } else {
        flipper_link_send(&s->lk, "running 0 %s\n",
                          runstate_check(RUN_STATE_DEBUG) ? "debug" :
                          runstate_check(RUN_STATE_PAUSED) ? "paused" :
                          RunState_str(runstate_get()));
    }
}

/* the rows that changed, a run of them to a line */
static void panel_rows(FlipperOnePanelState *s, bool all)
{
    int y = 0;

    while (y < FO_LCD_H) {
        int n = 0;
        g_autofree char *b64 = NULL;

        if (!all && !memcmp(s->fb[y], s->sent[y], FO_LCD_W)) {
            y++;
            continue;
        }
        while (y + n < FO_LCD_H && n < ROWS_PER_LINE &&
               (all || memcmp(s->fb[y + n], s->sent[y + n], FO_LCD_W))) {
            n++;
        }
        memcpy(s->sent[y], s->fb[y], n * FO_LCD_W);
        b64 = g_base64_encode(s->sent[y], n * FO_LCD_W);
        flipper_link_send(&s->lk, "rows %d %d %s\n", y, n, b64);
        y += n;
    }
}

static void panel_lights(FlipperOnePanelState *s, bool all)
{
    if (all || s->lcd_on != s->sent_lcd_on) {
        s->sent_lcd_on = s->lcd_on;
        flipper_link_send(&s->lk, "lcd %s\n", s->lcd_on ? "on" : "off");
    }
    if (all || s->backlight != s->sent_backlight) {
        s->sent_backlight = s->backlight;
        flipper_link_send(&s->lk, "backlight %d\n", s->backlight);
    }
    if (all || memcmp(s->leds, s->sent_leds, sizeof(s->leds))) {
        g_autoptr(GString) line = g_string_new("leds");

        memcpy(s->sent_leds, s->leds, sizeof(s->leds));
        for (int i = 0; i < FO_LEDS; i++) {
            g_string_append_printf(line, " %06x", s->leds[i] & 0xffffff);
        }
        flipper_link_send(&s->lk, "%s\n", line->str);
    }
    /* a buzz shorter than a tick is still told, for a tick */
    if (all || (s->haptic || s->haptic_blip) != s->sent_haptic) {
        s->sent_haptic = s->haptic || s->haptic_blip;
        flipper_link_send(&s->lk, "haptic %d\n", s->sent_haptic);
    }
    s->haptic_blip = false;
}

static void panel_everything(FlipperOnePanelState *s)
{
    panel_running(s, true);
    panel_lights(s, true);
    panel_rows(s, true);
    flipper_link_send(&s->lk, "usb %d\n", s->usb);
}

static void panel_tick(void *opaque)
{
    FlipperOnePanelState *s = opaque;

    if (!s->lk.open) {
        return;
    }
    panel_running(s, false);
    panel_lights(s, false);
    panel_rows(s, false);
    timer_mod(s->tick, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TICK_MS);
}

static void panel_vm_change(void *opaque, bool running, RunState state)
{
    FlipperOnePanelState *s = opaque;

    if (s->lk.open) {
        panel_running(s, false);
    }
}

static int panel_key(const char *name)
{
    for (int i = 0; i < FO_KEY_NUM; i++) {
        if (!strcmp(name, key_names[i])) {
            return i;
        }
    }
    return -1;
}

static void panel_line(void *opaque, char *line)
{
    FlipperOnePanelState *s = opaque;
    char *arg = strchr(line, ' ');
    int k, x, y, p;

    if (arg) {
        *arg++ = 0;
        arg = g_strstrip(arg);
    }
    if (!strcmp(line, "refresh")) {
        panel_everything(s);
    } else if ((!strcmp(line, "press") || !strcmp(line, "release")) && arg &&
               (k = panel_key(arg)) >= 0) {
        uint16_t keys = deposit32(s->keys, k, 1, line[0] == 'p');

        if (keys != s->keys) {
            s->keys = keys;
            panel_input(s);
        }
    } else if (!strcmp(line, "touch") && arg) {
        if (!strcmp(arg, "end")) {
            s->touching = false;
        } else if (sscanf(arg, "%d %d %d", &x, &y, &p) == 3) {
            s->touching = true;
            s->touch_x = MIN(MAX(x, 0), 1000);
            s->touch_y = MIN(MAX(y, 0), 1000);
            s->touch_p = MIN(MAX(p, 0), 255);
        }
        panel_input(s);
    } else if (!strcmp(line, "usb") && arg && (!strcmp(arg, "0") ||
                                               !strcmp(arg, "1"))) {
        s->usb = arg[0] == '1';
        flipper_link_send(&s->lk, "usb %d\n", s->usb);
        panel_input(s);
    }
}

static void panel_opened(void *opaque, bool open)
{
    FlipperOnePanelState *s = opaque;

    if (open) {
        flipper_link_send(&s->lk, "hello flipperoneemu panel 1\n");
        panel_everything(s);
        timer_mod(s->tick, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TICK_MS);
    } else {
        timer_del(s->tick);
        if (s->keys || s->touching) {           /* what it held, let go */
            s->keys = 0;
            s->touching = false;
            panel_input(s);
        }
    }
}

void fo_panel_backlight(FlipperOnePanelState *s, uint8_t level)
{
    s->backlight = level;
}

void fo_panel_led(FlipperOnePanelState *s, int index, uint32_t rgb)
{
    if (index >= 0 && index < FO_LEDS) {
        s->leds[index] = rgb & 0xffffff;
    }
}

void fo_panel_lcd_on(FlipperOnePanelState *s, bool on)
{
    s->lcd_on = on;
}

void fo_panel_haptic(FlipperOnePanelState *s, bool on)
{
    s->haptic = on;
    s->haptic_blip |= on && s->lk.open;     /* not one from before it came */
}

static void panel_realize(DeviceState *dev, Error **errp)
{
    FlipperOnePanelState *s = FLIPPER_ONE_PANEL(dev);

    s->sent_running = -1;
    s->tick = timer_new_ms(QEMU_CLOCK_REALTIME, panel_tick, s);
    qemu_add_vm_change_state_handler(panel_vm_change, s);
    flipper_link_init(&s->lk, &s->link_chr, "panel", panel_line,
                      panel_opened, s);
}

static const Property panel_properties[] = {
    DEFINE_PROP_CHR("link", FlipperOnePanelState, link_chr),
};

static void panel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = panel_realize;
    dc->desc = "Flipper One front panel: its screen, lights, buttons and "
               "touchpad for a program (docs/PANEL_LINK.md)";
    device_class_set_props(dc, panel_properties);
}

static const TypeInfo panel_types[] = {
    {
        .name = TYPE_FLIPPER_ONE_PANEL,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(FlipperOnePanelState),
        .class_init = panel_class_init,
    },
};
DEFINE_TYPES(panel_types)
