/*
 * The Flipper One's battery, as its gauge and charger see it
 *
 * A two-cell lithium-ion pack: each cell's open-circuit voltage from its
 * charge (a typical curve, 3.30 V empty to 4.30 V full: the firmware lets
 * the charger take the pack to 8.65 V), and at its terminals that, plus
 * what its resistance (0.15 Ω for the pack, assumed) makes of its current.
 * The charge follows the current over the virtual clock's time, the
 * current staying as it was last worked out in between: it is worked out
 * again whenever something reads the battery or changes what it depends on,
 * and every ten seconds besides, so that a charge's phases come when they
 * would.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "flipper_one.h"

#define R_MOHM          150                     /* the pack's resistance */
#define TICK_NS         (10 * NANOSECONDS_PER_SECOND)
#define NS_PER_HOUR     3600e9

/* a cell's open-circuit mV at 0, 10, ... 100 % */
static const int cell_mv[11] = {
    3300, 3620, 3680, 3730, 3770, 3810, 3860, 3930, 4010, 4120, 4300,
};

int fo_battery_percent(const FoBattery *b)
{
    return lround(b->mah * 100 / MAX(b->design_mah, 1));
}

void fo_battery_set_percent(FoBattery *b, int percent)
{
    b->mah = (double)b->design_mah * MIN(MAX(percent, 0), 100) / 100;
}

int fo_battery_ocv_mv(const FoBattery *b)
{
    double p = MIN(MAX(b->mah * 100 / MAX(b->design_mah, 1), 0), 100);
    int i = MIN((int)(p / 10), 9);

    return 2 * lround(cell_mv[i] + (cell_mv[i + 1] - cell_mv[i]) *
                      (p - i * 10) / 10);
}

int fo_battery_mv(const FoBattery *b)
{
    return fo_battery_ocv_mv(b) + b->ibat_ma * R_MOHM / 1000;
}

void fo_battery_update(FoBattery *b)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (b->at_ns && now > b->at_ns) {
        b->mah += b->ibat_ma * (now - b->at_ns) / NS_PER_HOUR;
        b->mah = MIN(MAX(b->mah, 0), b->design_mah);
    }
    b->at_ns = now;
    b->ibat_ma = b->flow ? b->flow(b->flow_opaque, b) : -b->load_ma;
}

static void fo_battery_tick(void *opaque)
{
    FoBattery *b = opaque;

    fo_battery_update(b);
    timer_mod(b->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + TICK_NS);
}

void fo_battery_start(FoBattery *b)
{
    b->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, fo_battery_tick, b);
    fo_battery_tick(b);
}
