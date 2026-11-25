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
| OS-02 | With no computer on the USB gadget, the getty on its serial port (flipserial0) blocks while holding the console's lock, and now and then the debug UART's and tty1's gettys never start: no login | Flipper OS 2501 | no console login | no |
| OS-03 | The image's boot entries for the 6.1.172 kernel (one per profile) cannot boot: their `vmlinuz` is the kernel's configuration, and their device-tree directory has no Flipper One tree. U-Boot lists them in its boot menu, and picking one fails ("Bad Linux ARM64 Image magic!") | Flipper OS 2501 | four of nine boot entries do not boot | no |

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

### OS-02: the gadget's getty holds the console's lock

- **What happens:** now and then the boot reaches user space but no login ever comes
  on the debug UART (ttyS0), nor on tty1. The kernel runs on, and so do the services:
  the MCU sees CPU state 3, and FlipCTL can draw. But the two gettys never start
  agetty, and nothing answers on the console.
- **Why:** Flipper OS's USB gadget (`usb-ncm-mtp-acm-gadget.service`) has an ACM
  function, whose tty `ttyGS0` is named `flipserial0`. A getty is started on it,
  `serial-getty@flipserial0.service`, beside those for ttyS0 and tty1. Before
  systemd starts agetty for a getty service it sets the service's tty up, and holds
  an exclusive `flock` on /dev/console while it does. The gadget's serial driver
  (`u_serial`) gives a tty no room to write while no host is connected to the gadget
  (`gs_write_room` is 0 until a host configures it). So the flipserial0 getty's setup
  blocks in its first write to ttyGS0, which is a blocking write, and keeps the lock.
  The ttyS0 and tty1 gettys wait for the lock with no timeout. If their setup took
  the lock first, they start, and only the flipserial0 getty hangs, unseen. So the
  order of three processes decides: here 2 boots of 19.
- **When:** any boot with no computer on the USB-C port while the RK3576 has it.
  Until a computer configures the gadget nothing frees the lock. A computer does:
  in a boot that had hung for 340 s, plugging one in (the panel link's `usb 2`)
  brought the console's login 2 s later, as the gadget was configured, and all
  three gettys ran.
- **To see it:** boot Flipper OS in the emulator (`run --linux`) a few times. In a
  boot that hangs, all three gettys' processes are still named `(agetty)` (forked,
  never executed). Their kernel stacks, read from QEMU:
  - two in `__do_sys_flock` → `locks_lock_inode_wait`, holding no tty;
  - one in `ksys_write` → `tty_write` → `n_tty_write` → `wait_woken`, on ttyGS0's
    `tty_struct`.
  Agetty's own strings are nowhere in the guest's memory, as they are in a boot that
  logs in.
- **Seen on:** Flipper OS build 2501 (systemd from Debian 13), in the emulator, since
  the emulator gave USB0 the device role a gadget binds to. Not tried on a real
  Flipper One, where a boot with nothing on the USB-C port should be the same.
- **A fix, for Flipper OS:** start the gadget's getty only once a host has configured
  the gadget, for example from a udev rule on the ACM function's connection rather
  than on the tty's appearance. Or give that service `TTYReset=no` and
  `TTYVHangup=no`, which might be enough, but that is not verified here. The
  emulator's Linux check masks the service on the kernel's command line
  (`systemd.mask=serial-getty@flipserial0.service`, `run --kernel-args`).
- **Log:** entries 63 and 64.

### OS-03: the 6.1.172 kernel's boot entries hold no kernel

- **What happens:** the image's root holds, beside each profile's 7.4.0-rc4 kernel, a
  6.1.172 one with a boot entry of its own (Boot Loader Specification type 1), so
  U-Boot's boot menu has nine entries, four of them 6.1.172. Picking one of those
  fails in U-Boot:
  `Skipping fdtdir /@Minimal/usr/lib/linux-image-6.1.172 for failure retrieving dts`,
  then `Bad Linux ARM64 Image magic!`, `Boot failed (err=-22)`, and U-Boot's shell.
- **Why:** the entry (`/loader/entries/600-flipperos-Minimal-6.1.172+3-0.conf` in the
  `boot` subvolume, and likewise for Desktop, TV Media Box and Router) names
  `linux /@Minimal/usr/lib/modules/6.1.172/vmlinuz`. That file, 218,160 bytes, is not
  a kernel but the kernel's configuration: it begins
  `# Automatically generated file; DO NOT EDIT.` /
  `# Linux/arm64 6.1.172 Kernel Configuration`. It is so in every profile's subvolume
  (Minimal, Desktop and Router read). And the entry's `devicetreedir`,
  `/@Minimal/usr/lib/linux-image-6.1.172`, has 489 device trees of other boards
  (Rockchip's evaluation boards, among them RK3576 ones) and none of the Flipper
  One's, so U-Boot finds no `rockchip/rk3576-flipper-one-rev-f0b0c1.dtb` there. The
  entry's options also name `console=ttyFIQ0`, the console of Rockchip's vendor
  kernels. Its packaging looks to have put the configuration where the image goes.
- **What of the emulator is in its path:** U-Boot reads the files from the microSD
  card's btrfs root through the emulated SD host. The 7.4.0-rc4 entries' files read
  the same way are right: their kernel, a gzip, boots. And the 6.1.172 `vmlinuz` above
  was read straight from the build's image with `btrfs restore` (as
  `scripts/fetch_os.py` reads it), not through the emulator, with the same content.
- **Seen on:** Flipper OS build 2501, in the emulator's boot through U-Boot
  (`run --uboot`), and in the build's image itself. Not tried on a real Flipper One,
  whose U-Boot is the image's own and should fail the same way.
- **A fix, for Flipper OS:** install the 6.1.172 kernel's image as `vmlinuz` (and the
  Flipper One's device trees for it), or leave its boot entries out of the image
  until there is one.
- **Log:** entry 75.

