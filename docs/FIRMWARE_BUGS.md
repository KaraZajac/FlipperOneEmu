# Firmware bugs found with the emulator

Bugs in the Flipper One's firmware that turned up while FlipperOneEmu was built and
checked: in its MCU firmware (flipperdevices/flipperone-mcu-firmware), numbered `FW-`,
and in Flipper OS (its kernel, flipperdevices/flipper-linux-kernel), numbered `OS-`.
They are the firmware's, not the emulator's: each one is read in the firmware's
source, and the entry says what of the emulator, if anything, is in its path.

- **None has been tried on a real Flipper One.** Where the cause is in the source, a
  real one should do the same; where it rests on how the chip behaves, the entry says
  what the datasheet gives and that it was not tried.
- **None has been reported upstream.** Whether to report is Kara's decision; the
  "Reported" column is there for it.
- Each bug has a number that stays its own (`FW-01`, `OS-01`), the firmware builds it
  was seen on, and the entry of `BRINGUP_LOG.md` that has the whole story.

A new bug gets the next number and a row in the table. A bug that is fixed upstream
keeps its row, with the build that fixed it.

## The list

| No. | What happens | Builds | Kind | Reported |
| --- | --- | --- | --- | --- |
| FW-01 | After a NACK on the main board's bus, that bus's state machine runs off its program, and every later transfer on the bus times out until the MCU restarts | dev 87f6cca3 | bus lost | no |
| OS-01 | Past a thermal zone's last trip, Linux cannot set the TSADC's alarm ("Failed to set trips: -34") and leaves it armed below the temperature, so it goes off at every sample until the chip is off | Flipper OS 2501 | log errors, alarm left on | no |

### FW-01: the main bus after a NACK, with words still queued

- **What happens:** a transfer on the main board's bus that gets a NACK where none is
  expected (an address no chip answers) leaves the bus's PIO state machine looping in
  empty instruction memory. From then on every transfer on that bus times out: the
  charger, the gauge, the main expander, the power sensors, the USB-C chips. It lasts
  until the MCU restarts.
- **Why:** the bus is a PIO program (`lib/drivers/i2c_master_pio`, from the Pico
  examples' `pio_i2c`) with autopull at 16 bits. After a NACK the state machine stops
  at `irq wait 0 rel`, and the driver calls `pio_i2c_resume_after_error()`. That
  empties the TX FIFO with forced `out null, 32` instructions until the FIFO is empty
  (`pio_sm_drain_tx_fifo()`). It then forces a jump to the program's wrap target,
  `entry_point`, and clears the flag. Then the driver queues a STOP.
  With autopull, the RP2350 refills the OSR from the FIFO in the same cycle as the OUT
  that empties it (datasheet 11.5.4.2). So the drain ends with the FIFO empty but the
  last word it took still in the OSR. `entry_point` reads that word as a record's
  header (`out x, 6`, `out y, 1`). A START or STOP instruction word (0xFF80, for one)
  gives an instruction count of 63, so the words that come next are executed as
  instructions. The first is the header of the STOP queued after the recovery,
  0x0800, which is `jmp 0`. The program sits at 7 to 24, instruction memory 0 is
  empty, and an empty word is `jmp 0` too: the state machine stays there.
- **When:** at the NACK, words of the transfer are still queued behind the one being
  sent. A probe by `furi_hal_i2c_device_ready()` is the address, one data byte and a
  four-word STOP. On the chip the CPU queues them long before the address byte has
  gone out at 400 kHz, so this should be the usual case. In the emulator the CPU's
  register accesses are slow, and often the NACK comes first, with nothing queued;
  then the recovery works. So here it shows now and then: about one run of the CLI's
  `i2c search main` (106 empty addresses) in ten or twenty.
- **To see it:** `./flipperoneemu cli 'i2c search main'` a number of times; once it has
  happened, a search finds nothing, and the MCU's log fills with errors from the
  charger, the gauge and the expander.
- **Seen on:** dev 87f6cca3 (2026-11-23), in the emulator, with a trace of the state
  machine's last 64 instructions. Not tried on a real Flipper One. It rests on the
  OSR's refill as the datasheet gives it: a chip that refilled only when an OUT
  needed data would leave the OSR empty, and the recovery would work. The Pico
  examples' own I2C program recovers the same way.
- **A fix, for the firmware:** one more forced `out null, 32` after the drain, which
  empties the OSR with nothing left to refill it from.
- **Log:** entry 53.

### OS-01: the TSADC's alarm, past the last trip, with a sensor's trim

- **What happens:** when a thermal zone's temperature passes its last trip point, the
  kernel logs two lines and leaves the zone's alarm where it was. On the RK3576 every
  zone's last trip is the critical one, 115 °C. The lines:

  ```
  rk_tsadcv2_temp_to_code: invalid temperature, temp=2147482724 error=511
  thermal thermal_zone1: Failed to set trips: -34
  ```

  The alarm stays armed at the trip just passed, and the temperature is over it, so
  the TSADC raises it again at every sample until the chip is off.
- **Why:** with no trip above, the thermal core calls `set_trips` with `high` as
  INT_MAX. The RK3576's alarm function (`rk_tsadcv3_alarm_temp()`) takes INT_MAX as "no
  alarm" and disables the channel's interrupt. But `rockchip_thermal_set_trips()` adds
  the sensor's trim to `high` first (`high + sensor->trim_temp`). For a sensor whose
  trim is not exactly the base code, the sum is no longer INT_MAX:
  - a trim one code low gives INT_MAX − 923;
  - a trim above overflows, signed, to about −INT_MAX.
  Either goes to `rk_tsadcv2_temp_to_code()`, off its table. That returns an error,
  `set_trips` returns -ERANGE, and nothing is written. Upstream Linux (torvalds/linux
  master, 2026-11-24) has the same lines.
- **When:** a zone above its last trip. On this device that is only past 115 °C, while
  Linux is already shutting down for it ("HARDWARE PROTECTION shutdown"), so what it
  costs is the log lines and the alarm's interrupts in the shutdown's last seconds.
- **To see it:** in the emulator, send the panel link's `ambient 105`, then a busy loop
  on each core. The zones pass 115 °C, and the log has the two lines for every zone
  but one. The TSADC's registers, read from QEMU during the shutdown:
  - HT_INT_EN is 0x37;
  - each armed channel's COMP_INT (its critical code, 381 to 385) is under its DATA
    (382 to 386);
  - INT_PD is pending.
  The zone whose sensor's trim is exactly the base code (channel 3, DDR's trim, as
  the emulator's OTP has it) is disarmed as it should be: the control.
- **Seen on:** Flipper OS build 2501 (kernel 7.4.0-rc4-g08a94c1da821), in the
  emulator, with the trims the emulator's OTP gives: each sensor 0 to 2 codes off.
  - Not tried on a real Flipper One. A real RK3576's trims are its own, and any
    sensor whose trim is not exactly the base code does the same.
  - The repeated alarm follows from the registers; how often it goes off was not
    measured. Here a sample is taken every 100 ms; the chip takes one every 2.5 ms.
- **A fix, for the kernel:** in `rockchip_thermal_set_trips()`, pass INT_MAX on as it
  is: `high == INT_MAX ? INT_MAX : high + sensor->trim_temp`.
- **Log:** entry 58.
