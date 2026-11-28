# Firmware bugs found with the emulator

Bugs in the Flipper One's firmware that turned up while FlipperOneEmu was built and
checked: in its MCU firmware (flipperdevices/flipperone-mcu-firmware), numbered `FW-`,
and in Flipper OS (its kernel, flipperdevices/flipper-linux-kernel, and its interface,
FlipCTL, flipperdevices/flipctl-slint), numbered `OS-`.
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
| FW-02 | The RPC's screen stream loses a frame drawn while it is still sending the one before; when the screen then stays as it is, the computer's view of it stays stale until the next change | dev 2e3290e4 | stale screen over RPC | no |
| FW-03 | An RPC session over the CLI does not end on its close request: the CLI's prompt does not come back until another byte comes, and that byte is lost | dev 2e3290e4 | RPC session left open | no |
| FW-04 | Bytes that reach the USB serial port before its CLI session is up are never read, and the port takes no more input until the MCU restarts | dev 2e3290e4 | USB CLI deaf | no |
| FW-05 | The USB serial port closed while the firmware is still sending leaves every later session there mute (no banner, no answers) until the MCU restarts | dev 2e3290e4 | USB CLI mute | no |
| FW-06 | An RPC session that ends while a message is half received logs "Received empty message (tag=0)": the cut-off message is decoded as a whole, empty one | dev 2e3290e4 | false error in the log | no |
| OS-01 | Past a thermal zone's last trip, Linux cannot set the TSADC's alarm ("Failed to set trips: -34") and leaves it armed below the temperature, so it goes off at every sample until the chip is off | Flipper OS 2501 | log errors, alarm left on | no |
| OS-02 | With no computer on the USB gadget, the getty on its serial port (flipserial0) blocks while holding the console's lock, and now and then the debug UART's and tty1's gettys never start: no login | Flipper OS 2501 | no console login | no |
| OS-03 | The image's boot entries for the 6.1.172 kernel (one per profile) cannot boot: their `vmlinuz` is the kernel's configuration, and their device-tree directory has no Flipper One tree. U-Boot lists them in its boot menu, and picking one fails ("Bad Linux ARM64 Image magic!") | Flipper OS 2501 | four of nine boot entries do not boot | no |
| OS-04 | FlipCTL's Settings row "USB-C1 power out" opens nothing: FlipCTL reads Type-C port 0, which on the Flipper One's tree is USB-C2's (the HD3SS3220's), not USB-C1's (the FUSB302's, port 1) | Flipper OS 2501 | a setting that does nothing | no |
| OS-05 | The kernel's command line has `console=ttyS4` for the line to the MCU, but the kernel registers four 8250 ports at most (`CONFIG_SERIAL_8250_RUNTIME_UARTS=4`), so there is no ttyS4: UART4 (the tree's `serial4`) is ttyS1, and Linux's console never reaches the MCU | Flipper OS 2501 | the MCU's console line has no console | no |
| OS-06 | FlipCTL's status bar never names a modem's technology (5G, LTE ...): it reads `mmcli --output-keyvalue`'s `modem.generic.access-technologies`, which mmcli prints as an array (`.length`, `.value[1]` ...), so the key is never there and the label stays `--` | Flipper OS 2501 | a status that never shows | no |
| OS-07 | FlipCTL's 5G Modem page never shows the cell (TAC, cell ID, PLMN, PCI, channel, band, RSRP ...): it runs qmicli through qmi-proxy as the desktop's user, and qmi-proxy takes no client but root | Flipper OS 2501 | a page's details always empty | no |
| OS-08 | FlipCTL's 5G Modem page never shows the link's rate (`Link`): it reads `/sys/class/net/IFACE/speed` for the modem's interface, which qmi_wwan, the QMI modem's driver, never has (EINVAL) | Flipper OS 2501 | a row always empty | no |

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

### FW-02: the RPC's screen stream loses a frame drawn while it sends

- **What happens:** a computer that streams the MCU's screen over the RPC (the CLI's
  `rpc`, a `StartVirtualDisplayRequest`) gets a frame for each change of the screen,
  except one that comes while a frame is still on its way. Then it gets none: when
  the screen then stays as it is, as a menu does once it is drawn, the computer's
  view stays stale until the next change. Here, over the CLI's UART: Settings
  pressed through the RPC on the desktop; the frame of its key shown pressed came,
  and then none for 18 s, while the Settings menu came to the screen.
- **Why:** `rpc_screen_fb_callback()` (applications/services/rpc/rpc_screen_stream.c),
  which the GUI calls with each new frame, takes the stream's mutex with no wait,
  `furi_mutex_acquire(stream->mutex, 0)`, and returns if it cannot: the frame is
  neither copied nor flagged. The stream's thread holds that mutex for the whole of
  `rpc_screen_send_frame()`, the frame's sending. So a frame drawn during a send is
  lost, and the GUI does not draw an unchanging screen again.
- **When:** whenever the screen changes while a frame goes. Over the CLI's UART at
  115200 baud a frame (37 KB) takes about 3.5 s, so it is most changes. Over USB it
  takes milliseconds, so it is the last frame of a quick run of them (an animation's
  end) now and then.
- **What of the emulator is in its path:** the UART carries the frames at its baud
  rate, as on the device. The callback and the thread are the firmware's.
- **Seen on:** MCU firmware dev-25112026-2e3290e4, in the emulator. Not tried on a
  real Flipper One.
- **A fix, for the firmware:** when the copy cannot be made, set a flag that the
  frame was lost (or keep a second buffer), and have the thread, after a send, take
  the GUI's current frame if one was lost (`gui_update()` draws it again). A
  computer can ask for a fresh frame meanwhile: a stop and a start of the stream.
- **Log:** entry 79.

### FW-03: an RPC session ends on its close request only when another byte comes

- **What happens:** a computer ends its RPC session (the CLI's `rpc`) with an
  `RpcSessionCloseRequest`. The firmware stops the screen stream, but the session
  goes on: the CLI's prompt does not come back. Over the UART it lasts until another
  byte comes, and the session drops that byte, so the first character of the next
  command is lost. Over USB it lasts until the port is closed.
- **Why:** `rpc_cli.c` reads with `pipe_receive(pipe, buffer, 1)`, after
  `pipe_set_state_check_period(pipe, 50)`, under the comment "Wake up every 50 ms to
  check connection and close_request". But `pipe_receive()` (lib/containers/pipe.c)
  loops until all the bytes asked for have come or the pipe is broken: the period is
  only how often it looks at the pipe's state, and it does not return. So
  `session_close_request` (set by `rpc_session_close_handler()` through the session's
  close callback) is looked at only after one more byte. The loop then breaks before
  feeding it to the session, and the byte is dropped.
- **When:** every close request.
- **What of the emulator is in its path:** nothing. The UART and the USB serial port
  carry the bytes as they come.
- **Seen on:** dev-25112026-2e3290e4, in the emulator. Not tried on a real Flipper One.
- **A fix, for the firmware:** a receive that gives up after the period (the pipe's
  stream buffer read with that timeout), so that the loop looks at the flag every
  50 ms as meant. A computer can meanwhile send one byte after the request, half a
  second on: scripts/fo_rpc.py does, and reads to the prompt.
- **Log:** entry 84.

### FW-04: the USB serial port loses input that comes before its session

- **What happens:** a program opens the MCU's USB serial port and writes at once.
  It gets the CLI's banner, but no answer, then or later: the port takes no more input
  until the MCU restarts. Its output still works.
- **Why:** opening the port raises DTR. TinyUSB's line-state callback signals
  `Connected` to `cli_vcp.c`, and the data's arrival signals `Rx`, both on one event
  flag. `cli_vcp_event_happened()` takes the flags together and handles `Rx` before
  `Connected`. With no session yet (`own_pipe` NULL), `cli_vcp_maybe_receive_data()`
  returns without reading, and the bytes stay in TinyUSB's receive FIFO (64 bytes).
  TinyUSB arms the OUT endpoint again only when a whole 64-byte packet fits in that
  FIFO (`_prep_out_transaction()`), and with one byte in it only 63 do. So the
  endpoint stays unarmed, the computer's next bytes are NAKed, and no `Rx` comes
  again. Nothing else reads the FIFO: the shell's space-freed callback needs data
  in the pipe first.
- **Seen through gdb** (`./flipperoneemu gdb`): the CDC's receive FIFO at write 9,
  read 8, the one byte, and EP2 OUT neither busy nor claimed. In a session that works
  it is busy.
- **When:** the bytes come in one batch with `Connected`, before the CLI's thread has
  handled it. The USB service's thread and the CLI's have the same priority (16), so
  the CLI's runs only once the USB task is through with what it has queued.
- **What of the emulator is in its path:** the computer here (usb_host.c) sends its
  first bulk OUT 20 µs after DTR's control transfer. A PC's program writes only after
  its `open()` returns, so on a real PC the window should be narrower: this would be
  rare there, not usual. Not tried on a real Flipper One.
- **A fix, for the firmware:** once the session's pipe is up in the `Connected`
  handler, read what is waiting (`cli_vcp_maybe_receive_data()`); or handle
  `Connected` before `Rx`. The emulator's tools wait for the banner before they write
  (scripts/fo_rpc.py, the smoke test).
- **Seen on:** dev-25112026-2e3290e4, in the emulator.
- **Log:** entry 84.

### FW-05: the USB serial port closed while output is on its way

- **What happens:** the USB serial port is closed while the firmware is still sending
  (a long `help`, a frame of the RPC's stream, the prompt). Every later session on
  the port is mute: no banner, no answers, until the MCU restarts.
- **Why:** `cli_vcp.c` sets `is_currently_transmitting` when it hands a packet to
  TinyUSB. The next `cli_vcp_maybe_send_data()` that finds the pipe empty clears it,
  after the packet's TxDone. With the port closed the computer stops reading, and the
  packet stays in flight. The `Disconnected` handler frees the pipe and leaves the
  flag. When the port is opened again, the computer reads and the packet completes.
  If its TxDone is handled while there is no session (before `Connected`),
  `cli_vcp_maybe_send_data()` finds no pipe and returns, and the flag stays set. The
  new session's banner then waits in the pipe for a TxDone that never comes, since
  nothing is in flight: `cli_vcp_data_from_shell()` signals a send only when the flag
  is clear.
- **Seen through gdb:** the CliVcp (through the CDC driver's `cb_ctx[0]`) with
  `is_currently_transmitting` true, `is_connected` false and `own_pipe` NULL;
  TinyUSB's transmit FIFO empty and EP2 IN not busy; the CLI's thread waiting in its
  event loop.
- **When:** in the emulator, the computer raises DTR and then reads (usb_host.c), so
  the TxDone and `Connected` race: within a few closes in the middle of `help`. On a
  Linux PC it should be every time. Its cdc-acm submits its reads in its port's
  `activate()`, and `tty_port_open()` raises DTR only after that, in
  `tty_port_block_til_ready()`, so the packet's TxDone comes before `Connected`. Not
  tried on a real Flipper One.
- **A fix, for the firmware:** clear the flag in the `Disconnected` handler, and
  where `cli_vcp_maybe_send_data()` finds no pipe.
- **Seen on:** dev-25112026-2e3290e4, in the emulator.
- **Log:** entry 84.

### FW-06: a message cut off by the session's end, decoded as an empty one

- **What happens:** an RPC session ends (its close request, or the port closed)
  while the firmware has a message's length and not yet its bytes. The log then has
  `[E][RpcSrv] Received empty message (tag=0)`, though nothing empty was sent.
- **Why:** the session's worker decodes with nanopb from `rpc_pb_stream_read()`
  (applications/services/rpc/rpc.c). When the session ends there, that function sets
  `istream->bytes_left = 0` and returns false. nanopb takes a failed read with no
  bytes left for the message's end: the decode succeeds, with no field. So
  `which_content` is 0 and the worker logs the error (it is ending anyway).
- **When:** a byte goes to the session after its last whole message, and the
  session ends before more come. Here: a byte sent after a close request (FW-03's
  way out) while the request waited behind a stop of the screen stream. The stop
  waits for the frame on its way, seconds over the UART. Caught in the emulator with
  gdb at the log line: the decoder had taken the stop (`02 2a 00`), the close request
  (`02 32 00`) and then that byte (`0d`) as the next message's length.
- **What of the emulator is in its path:** nothing. The bytes came in whole and in
  order.
- **Seen on:** dev-25112026-2e3290e4, in the emulator. Not tried on a real Flipper One.
- **A fix, for the firmware:** on the session's end, leave `bytes_left` as it is (the
  read fails, and so does the decode, whose error the worker already leaves unlogged
  while it ends). Or log the empty message only when the session is not ending.
  scripts/fo_rpc.py reads a stream to its end before it sends a close request.
- **Log:** entry 84.

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


### OS-04: "USB-C1 power out" reads USB-C2's port

- **What happens:** Settings, "USB-C1 power out": Ok opens nothing, and the row
  shows no value. The popup it should open offers charging the Flipper or powering
  what is plugged in with one of the tree's three sets (5 V, 18 W, 27 W).
- **Why:** FlipCTL (`crates/flipper-ui/src/usb_pd.rs`, at the build's revision
  6c8c74e and still on its `dev` branch) reads the port at
  `/sys/class/typec/port0`, and gives up when that port has no
  `select_usb_power_delivery`. On the Flipper One's tree, port 0 is the HD3SS3220's
  (i2c2 0x47, `usb-mux@47`): USB-C2, a host port, with no capability sets. USB-C1,
  the FUSB302's (i2c2 0x22, `typec-portc@22`), with the sets `[pd0] pd1 pd2`, is
  port 1:
  - The typec class numbers ports in the order they register. Both drivers are
    built in (`CONFIG_TYPEC_FUSB302=y`, `CONFIG_TYPEC_HD3SS3220=y`).
  - The FUSB302 comes first in the tree, but its probe needs the charger's VBUS
    regulator (`vbus-supply`, `charger@6b/regulators/vbus`). It defers until the
    BQ25792 has probed, and the BQ25792 comes after both in the tree.
  - The HD3SS3220 needs only two fixed regulators, its interrupt and its pins, so
    it registers first: `hd3ss3220 2-0047: probed revision=0x2`.

  In the emulator, `/sys/class/typec/port0` is `.../2-0047/typec/port0` (power role
  `[source]`) and `port1` is `.../2-0022/typec/port1`
  (`select_usb_power_delivery`: `[pd0] pd1 pd2`).
- **What of the emulator is in its path:** the order comes from the kernel's own
  probing of the board's tree. The emulator's part is that both chips answer on
  the bus. A real HD3SS3220 stays silent for some tens of milliseconds after it is
  powered, which its driver waits out inside its probe; the FUSB302 still waits for
  the charger after it. A real Flipper One should number the ports the same way.
- **Seen on:** Flipper OS build 2501 (FlipCTL 6c8c74e), in the emulator. Not tried on
  a real Flipper One.
- **A fix, for FlipCTL:** look for the port rather than assume its number. For
  example, take the `/sys/class/typec/port*` that has `select_usb_power_delivery`,
  or the one whose device is bound to `fusb302`.
- **Log:** entry 108.

### OS-05: `console=ttyS4` names no port

- **What happens:** every boot entry's options (and the emulator's direct boot, which
  takes them from the build) have `console=ttyS4,1500000n8`, for UART4, the device
  tree's "MCU interconnect" and its alias `serial4`. No ttyS4 exists: `/proc/consoles`
  lists ttyS0 and tty1 only, so the kernel's messages never go to the MCU, and
  systemd's getty generator, which follows the active consoles, starts no getty there.
  UART4 is ttyS1 (`2ad70000.serial: ttyS1 MMIO32:0x000000002ad70000`).
- **Why:** the kernel has `CONFIG_SERIAL_8250_NR_UARTS=8` but
  `CONFIG_SERIAL_8250_RUNTIME_UARTS=4`, so the 8250 core makes ports 0 to 3 at boot
  (`/sys/module/8250/parameters/nr_uarts` reads 4). A port whose alias names a line
  past those, 4 here, gets the first one free instead: 1, after UART0's 0.
- **What of the emulator is in its path:** none. The numbering comes from the kernel's
  configuration and the device tree's aliases. The emulator's UART4 is a 16550 (the
  DesignWare UART's own registers past it read 0), which 8250_dw registers as it
  would the chip.
- **Seen on:** Flipper OS build 2501, in the emulator. Not tried on a real Flipper One,
  where the same kernel and tree should number it the same.
- **A fix, for Flipper OS:** `CONFIG_SERIAL_8250_RUNTIME_UARTS` of 5 or more (or
  `8250.nr_uarts=12` on the command line), so the alias's line exists; or name the
  console `ttyS1`, which holds only while no UART before it in the tree is enabled.
- **Log:** entry 113.

### OS-06: the status bar's modem technology is always `--`

- **What happens:** with a modem registered (in the emulator, `run --modem`: LTE with an
  NR cell, ModemManager's access technologies `lte, 5gnr`), the status bar shows the
  signal's bars but `--` where the technology should be.
- **Why:** FlipCTL (`crates/flipper-ui/src/status.rs`, `mod modem`, at 6c8c74e) runs
  `mmcli -m N --output-keyvalue` and takes the line whose key is
  `modem.generic.access-technologies`. ModemManager 1.24's mmcli prints that list as an
  array, one line for its length and one for each value:

  ```
  modem.generic.access-technologies.length    : 2
  modem.generic.access-technologies.value[1]  : lte
  modem.generic.access-technologies.value[2]  : 5gnr
  ```

  No line has the key itself, so `label()` gets nothing and returns `--`.
  - Its comment has mmcli name the technologies "newest first", and `label()` shows
    the first it knows. mmcli lists them in the order of ModemManager's flags, oldest
    first. Read as an array, a non-standalone 5G connection (`lte, 5gnr`) would still
    show `LTE`.
- **What of the emulator is in its path:** the modem's registration (QMI's system
  info). The key's form is mmcli's own. The 5G Modem page reads `mmcli -J`, where the
  list is a JSON array, and shows `LTE/5GNR` from it.
- **Seen on:** Flipper OS build 2501 (FlipCTL 6c8c74e, ModemManager 1.24.0), in the
  emulator. Not tried on a real Flipper One, where the same mmcli should print the
  same.
- **A fix, for FlipCTL:** read `.value[1]`, `.value[2]` ... (or `mmcli -J`), and pick the
  newest technology it knows, not the first.
- **Log:** entry 121.

### OS-07: the 5G Modem page's cell is always `--`

- **What happens:** Network > 5G Modem shows the operator, the technologies and the
  signal (from ModemManager), but `--` for TAC, Cell ID, PLMN, PCI and the rest below.
- **Why:** FlipCTL (`crates/flipper-ui/src/sysinfo.rs`, `modem()`, at 6c8c74e) gets
  them by running `qmicli -d DEV --device-open-proxy --nas-get-serving-system`,
  `--nas-get-cell-location-info` and `--nas-get-signal-strength`. FlipCTL runs as the
  desktop's user (`user`). qmi-proxy, which ModemManager has the device through, takes
  no client but root: as `user`, qmicli fails ("Cannot read from istream: connection
  broken", "couldn't open the QmiDevice: ... Broken pipe"). The device itself
  (`/dev/cdc-wdm0`, `crw------- root root`) is root's too.
  - As root, the same command prints what the page wants (`3GPP cell ID: '1714947'`,
    `LTE tracking area code: '10783'`, `MCC: '1'`, `MNC: '1'`).
- **What of the emulator is in its path:** the modem's answers to those three QMI
  messages, which root's qmicli shows are there.
- **Seen on:** Flipper OS build 2501 (FlipCTL 6c8c74e, libqmi 1.36.0), in the emulator.
  Not tried on a real Flipper One.
- **A fix, for Flipper OS or FlipCTL:** let FlipCTL's user reach the modem (qmi-proxy
  run for that group, or a udev rule and qmicli without the proxy while ModemManager
  is not using it), or take the cell from ModemManager (`mmcli --location-get`, with
  location's 3GPP source enabled, and `--signal-get`).
- **Log:** entry 121.

### OS-08: the 5G Modem page's Link is always `--`

- **What happens:** Network > 5G Modem, connected (an address on its interface, shown
  as `IP`), shows `--` for `Link`.
- **Why:** FlipCTL (`crates/flipper-ui/src/sysinfo.rs`, `modem()` and `link_mbps()`, at
  6c8c74e) takes the bearer's interface from ModemManager (`wwu1u4`) and reads
  `/sys/class/net/wwu1u4/speed` for the downlink's and uplink's rate. That interface
  is qmi_wwan's, and qmi_wwan keeps usbnet's ethtool operations
  (`drivers/net/usb/usbnet.c`, `usbnet_ethtool_ops`), whose link settings come only
  from an MII (`usbnet_get_link_ksettings_mii`: `-EOPNOTSUPP` without one). A QMI
  modem has none, so reading `speed` fails ("Invalid argument") whatever the modem
  does.
- **What of the emulator is in its path:** none: the kernel's driver answers.
- **Seen on:** Flipper OS build 2501 (FlipCTL 6c8c74e, its kernel 7.4.0-rc4), in the
  emulator. Not tried on a real Flipper One.
- **A fix, for FlipCTL:** ask the modem: QMI's WDS Get Current Channel Rate
  (`qmicli --wds-get-channel-rates`, which needs OS-07's fix too) gives the call's
  rates; or leave the row out for a WWAN interface.
- **Log:** entry 122.
