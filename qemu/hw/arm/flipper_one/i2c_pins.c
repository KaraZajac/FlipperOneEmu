/*
 * An I2C bus on two of the RP2350's pins, as a master bit-bangs it
 *
 * The Flipper One's main bus is a PIO program's (the Pico SDK's pio_i2c, on
 * GPIO22 SDA and GPIO23 SCL): open drain, its pindirs pulling the lines low,
 * the pull-ups letting them up. Each change of either pin is read as the
 * master leaves the line (rp2350_pin_own(): its own drive and the pulls,
 * not the chips') and given to QEMU's bit-banged I2C decoder, which starts,
 * writes, reads and ends transfers on a QEMU I2C bus; what the chip there
 * puts on SDA (an ACK, a bit read) the board drives on the pin. The decoder
 * gives a read bit at SCL's rising edge and lets SDA go at its falling one;
 * the master samples in the middle of the high time, so it reads it.
 *
 * The bus has a second master, the RK3576's I2C2 (rk3x_i2c.c), with the
 * same chips: one transfer at a time, as a busy bus has it. The other
 * master takes the bus only between the MCU's transfers
 * (fo_i2c_pins_claim()), and for as long as it has it the MCU sees SCL
 * low, as the other's clock pulls it: the PIO program waits at its next
 * bit ("wait 1 pin", its clock stretching), and a START it makes meanwhile
 * comes to the bus when the other master lets it go.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "system/reset.h"
#include "flipper_one.h"
#include "rp2350.h"

static void i2c_pins_changed(void *opaque, int pin, bool level, int64_t when)
{
    FoI2cPins *b = opaque;
    int sda = rp2350_pin_own(b->soc, b->sda);
    int own_scl = rp2350_pin_own(b->soc, b->scl);
    int scl = own_scl && !b->held;
    bool low;

    /* while the other master has the bus: SDA changing with the MCU's SCL
     * let go is its START (falling) or STOP (rising), for later */
    if (b->held && own_scl && sda != b->own_sda) {
        b->start_pending = !sda;
    }
    b->own_sda = sda;

    /* SDA first: the master changes it while SCL is low, but a chip holding
     * it low may have hidden that until now */
    if (sda != b->bb.last_data) {
        bitbang_i2c_set(&b->bb, BITBANG_I2C_SDA, sda);
    }
    if (scl != b->bb.last_clock) {
        bitbang_i2c_set(&b->bb, BITBANG_I2C_SCL, scl);
    }
    low = !b->bb.device_out;
    if (low != b->sda_low) {
        b->sda_low = low;
        rp2350_pin_drive(b->soc, b->sda, low ? 0 : -1);
    }
}

static void i2c_pins_reset(void *opaque)
{
    FoI2cPins *b = opaque;

    if (b->bb.state != STOPPED && b->bb.current_addr >= 0) {
        i2c_end_transfer(b->bus);
    }
    bitbang_i2c_init(&b->bb, b->bus);
    b->bb.state = STOPPED;
    b->bb.current_addr = -1;
    b->sda_low = false;
    b->held = b->start_pending = false;
    b->own_sda = 1;
}

bool fo_i2c_pins_claim(FoI2cPins *b, bool claim)
{
    if (claim) {
        if (b->bb.state != STOPPED) {
            return false;                       /* the MCU's transfer */
        }
        b->held = true;
        b->start_pending = false;
        rp2350_pin_drive(b->soc, b->scl, 0);
    } else if (b->held) {
        b->held = false;
        if (b->start_pending) {                 /* the MCU's START, now */
            b->start_pending = false;
            b->bb.state = SENDING_BIT7;
            b->bb.current_addr = -1;
        }
        rp2350_pin_drive(b->soc, b->scl, -1);
    }
    i2c_pins_changed(b, b->scl, false, 0);
    return true;
}

I2CBus *fo_i2c_pins_init(FoI2cPins *b, struct RP2350State *soc, int sda,
                         int scl, const char *name)
{
    b->soc = soc;
    b->sda = sda;
    b->scl = scl;
    b->bus = i2c_init_bus(DEVICE(soc), name);
    i2c_pins_reset(b);
    soc->ext_pullup |= (1ull << sda) | (1ull << scl);    /* the board's */
    rp2350_pin_listen(soc, sda, i2c_pins_changed, b);
    rp2350_pin_listen(soc, scl, i2c_pins_changed, b);
    qemu_register_reset(i2c_pins_reset, b);
    return b->bus;
}
