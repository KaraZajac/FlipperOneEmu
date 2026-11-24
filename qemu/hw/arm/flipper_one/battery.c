/*
 * The Flipper One's battery, as its gauge and charger see it
 *
 * A two-cell lithium-ion pack: each cell's open-circuit voltage from its
 * charge (a typical curve, 3.30 V empty to 4.30 V full: the firmware lets
 * the charger take the pack to 8.65 V), less what its resistance (0.15 Ω for
 * the pack, assumed) drops at the board's load.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "flipper_one.h"

/* a cell's open-circuit mV at 0, 10, ... 100 % */
static const int cell_mv[11] = {
    3300, 3620, 3680, 3730, 3770, 3810, 3860, 3930, 4010, 4120, 4300,
};

int fo_battery_mv(const FoBattery *b)
{
    int p = MIN(MAX(b->percent, 0), 100);
    int i = MIN(p / 10, 9);
    int cell = cell_mv[i] + (cell_mv[i + 1] - cell_mv[i]) * (p - i * 10) / 10;

    return 2 * cell - b->load_ma * 15 / 100;
}
