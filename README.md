# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-24): the MCU and its board run, and the CPU boots Flipper OS's kernel
to a shell.** The RP2350's own boot ROM verifies
the flash's partition table and starts the MCU firmware on the emulated chip. The
firmware starts FreeRTOS, logs through its PIO UART, draws on the screen in the window
(over HSTX and DMA to the emulated JD9853 panel), lights the backlight, answers on its
CLI, and takes the window's buttons (through the control bus's I2C expander) and
touchpad (through its IQS7211E): its desktop, menus and touchpad test work. Its haptic
driver buzzes (the window shakes), its 17 LEDs light in the window as it drives them,
and its settings are kept in the flash across restarts. Its main bus (I2C from a PIO
program) reaches the main board's expander, which powers the Linux side, the battery's
gauge (the status bar shows its charge), its charger, the system rail's current meter
and the USB-C port's chips: the self-check finds every chip. A charger can be plugged
into its USB-C port, and the battery charges; a computer plugged in gets its USB serial
port, also as a real USB device on this PC (USB/IP), and the firmware can be updated as
picotool updates it, through the RP2350's boot ROM. Its `power reboot` power-cycles the
board through the charger, and its power off (ship mode) turns it off until a charger
is plugged in. The RK3576 (four Cortex-A53s and four A72s) starts Flipper OS's own
kernel and device tree from a published build, which boots to its initramfs's shell on
the debug port's UART, and whose own driver for the MCU talks to the MCU firmware over
their I2C link: the kernel gets the Flipper's buttons, touchpad and headset as input
devices, the MCU learns the CPU's state, and the frames of Linux's display driver reach
the MCU's screen over their SPI link. Storage (for the whole of Flipper OS) and more of
the RK3576's devices come next. [RESEARCH.md](RESEARCH.md) is the survey this
starts from:
the device, what Flipper Devices publishes, and what emulating it takes.
[PLAN.md](PLAN.md) is the order of work; the MCU comes first.
[docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md) is what was found on the way.

## Trying it

```sh
scripts/setup_qemu.sh        # QEMU 11.1.3, its patches and this machine (about 6 minutes)
scripts/fetch_firmware.py    # the MCU firmware's newest build, and the RP2350 boot ROM
./flipperoneemu gui          # the window: it starts a device, devices/one
```

The window draws a Flipper One around the emulated screen, part by part where the device
has them: this project's own vector drawing, measured from the device's published renders
(it is not Flipper Devices' artwork, and the device's name stands where their wordmark
is), with its LEDs where the device has them. Click its buttons, or use the keyboard:
the arrows and Enter are the pad, Backspace or Esc is Back, F1 to F5 are the soft keys
(Esc, View, Power, Edit, Run), Tab is App Switcher, Space is PTT; drag on the touchpad;
Ctrl+U plugs a charger into the USB-C port, Ctrl+Shift+U a computer. It talks to the
emulator over a front-panel link, [docs/PANEL_LINK.md](docs/PANEL_LINK.md), which any program can use.
Another,
[docs/HOST_LINK.md](docs/HOST_LINK.md) (`devices/NAME/host.sock`), lets a program take
the RK3576's place on its link to the MCU: the I2C registers (CPU state, LEDs, haptics,
input), the interrupt line, and the screen: frames it sends go over the display's SPI
as Flipper OS sends them, and the MCU shows them.

At its start the MCU firmware powers the Linux side, as the device does, and shows its
screen; the RK3576 is not emulated yet, so after a few seconds that screen says "CPU not
responding". Power opens the Flipper OS menu, and Stop there turns the Linux side off
and brings the MCU's own desktop.

```sh
./flipperoneemu run [-n NAME] [--detach] [--gdb PORT]   # a device without the window
./flipperoneemu run --bootsel ...    # ... started with BOOTSEL held: the boot ROM's USB boot
./flipperoneemu run --usbip 3240 ... # ... its USB given to this PC: sudo scripts/usbip_attach.py
scripts/fetch_os.py                  # a Flipper OS build: its kernel, initrd and device trees
./flipperoneemu run --linux ...      # ... started on the RK3576, beside the MCU
./flipperoneemu console [-n NAME] [CMD]  # Linux's console (the RK3576's UART0); Ctrl-] leaves
scripts/fo_picoboot.py --port 3240 load FIRMWARE.uf2 --reboot
                                     # an update, as picotool makes one: after the
                                     # CLI's `power boot` (or --bootsel), a computer
                                     # plugged in (Ctrl+Shift+U)
./flipperoneemu log [-n NAME]        # the firmware's log, from its RTT buffer
./flipperoneemu cli [-n NAME] [CMD]  # the firmware's CLI (UART1); Ctrl-] leaves
./flipperoneemu cli --usb ...         # ... on its USB serial port (a computer plugged in)
./flipperoneemu where [-n NAME]      # where each core is: its PC and function
./flipperoneemu shot FILE.png        # the screen as a picture
./flipperoneemu status | stop [-n NAME]
scripts/smoke_test.py                # a throwaway device, checked: PASS or FAIL
```

A device is a folder, `devices/NAME`: the MCU's flash (made from the newest recovery
`.uf2` the first time, and written to as the firmware writes its flash), its sockets,
QEMU's log, and `mcu.log`: what the firmware writes to its log UART (a PIO program on
GPIO41). By hand: `-M flipper-one,flash=FILE -bios bootrom-combined.bin`;
`bootsel=on` starts with the BOOTSEL button held; `battery=N` sets the battery's charge
(80 % unless told); `-d unimp,guest_errors` logs every access to what is not modelled
yet.

## QEMU patches

| Patch | What |
|---|---|
| 0001 | target/arm: an M-profile chip's own coprocessors (the RP2350's GPIO, DCP and RCP) |
| 0002 | armv7m_nvic: MPU_RNR and MPU_RLAR keep what the Cortex-M33 keeps |
| 0003 | armv7m_nvic: model Lockup (FlipperZeroEmu's 0015) |
| 0004 | hw/arm/boot: leave M-profile CPUs alone (the kernel loader, in a machine with both) |

## Layout

| Path | What |
|---|---|
| `qemu/hw/arm/flipper_one/` | The machine and its devices, linked into QEMU's source tree |
| `qemu/patches/` | Changes to QEMU itself |
| `flipperoneemu` | The launcher: run, the window, stop, status, log, cli, shot, where |
| `scripts/` | Setting up QEMU, fetching the firmware, the window (`fo_gui.py`, `fo_view.py`, `fo_link.py`), the launcher's parts, checks |
| `docs/BRINGUP_LOG.md` | What was found on the way, in order |

## License

BSD-3-Clause ([LICENSE](LICENSE)). The firmware the emulator runs is not part of this
repository: the MCU firmware (MIT, some files GPL-3.0) and Flipper OS come from Flipper
Devices' builds, the RP2350 boot ROM from Raspberry Pi's releases.
