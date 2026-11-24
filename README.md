# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-23): just started.** The RP2350's own boot ROM verifies the flash's
partition table and starts the MCU firmware on the emulated chip; the firmware starts
FreeRTOS on core 0 (core 1 waits in its ROM), logs through its PIO UART, and waits for
the ADC, the next model on the way to its screen. [RESEARCH.md](RESEARCH.md) is the survey this starts from:
the device, what Flipper Devices publishes, and what emulating it takes.
[PLAN.md](PLAN.md) is the order of work; the MCU comes first.
[docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md) is what was found on the way.

## Trying it

```sh
scripts/setup_qemu.sh        # QEMU 11.1.3, its patches and this machine (about 6 minutes)
scripts/fetch_firmware.py    # the MCU firmware's newest build, and the RP2350 boot ROM
./flipperoneemu gui          # the window: it starts a device, devices/one
```

The window draws a Flipper One of this project's own around the emulated screen (it is
not Flipper Devices' artwork), with its LEDs where the device has them. Click its
buttons, or use the keyboard: the arrows and Enter are the pad, Backspace or Esc is
Back, F1 to F5 are the soft keys (Esc, View, Power, Edit, Run), Tab is App Switcher,
Space is PTT; drag on the touchpad. It talks to the emulator over a front-panel link,
[docs/PANEL_LINK.md](docs/PANEL_LINK.md), which any program can use.

```sh
./flipperoneemu run [-n NAME] [--detach] [--gdb PORT]   # a device without the window
./flipperoneemu log [-n NAME]        # the firmware's log, from its RTT buffer
./flipperoneemu where [-n NAME]      # where each core is: its PC and function
./flipperoneemu shot FILE.png        # the screen as a picture
./flipperoneemu status | stop [-n NAME]
```

A device is a folder, `devices/NAME`: the MCU's flash (made from the newest recovery
`.uf2` the first time, and written to as the firmware writes its flash), its sockets,
QEMU's log, and `mcu.log`: what the firmware writes to its log UART (a PIO program on
GPIO41). By hand: `-M flipper-one,flash=FILE -bios bootrom-combined.bin`;
`bootsel=on` starts with the BOOTSEL button held; `-d unimp,guest_errors` logs every
access to what is not modelled yet.

## QEMU patches

| Patch | What |
|---|---|
| 0001 | target/arm: an M-profile chip's own coprocessors (the RP2350's GPIO, DCP and RCP) |
| 0002 | armv7m_nvic: MPU_RNR and MPU_RLAR keep what the Cortex-M33 keeps |
| 0003 | armv7m_nvic: model Lockup (FlipperZeroEmu's 0015) |

## Layout

| Path | What |
|---|---|
| `qemu/hw/arm/flipper_one/` | The machine and its devices, linked into QEMU's source tree |
| `qemu/patches/` | Changes to QEMU itself |
| `flipperoneemu` | The launcher: run, the window, stop, status, log, shot |
| `scripts/` | Setting up QEMU, fetching the firmware, the window (`fo_gui.py`, `fo_view.py`, `fo_link.py`), the launcher's parts, checks |
| `docs/BRINGUP_LOG.md` | What was found on the way, in order |

## License

BSD-3-Clause ([LICENSE](LICENSE)). The firmware the emulator runs is not part of this
repository: the MCU firmware (MIT, some files GPL-3.0) and Flipper OS come from Flipper
Devices' builds, the RP2350 boot ROM from Raspberry Pi's releases.
