# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-23): just started; nothing runs yet.** [RESEARCH.md](RESEARCH.md) is the
survey this starts from: the device, what Flipper Devices publishes, and what emulating
it takes. [PLAN.md](PLAN.md) is the order of work; the MCU comes first.

## Layout

| Path | What |
|---|---|
| `qemu/hw/arm/flipper_one/` | The machine and its devices, linked into QEMU's source tree |
| `qemu/patches/` | Changes to QEMU itself |
| `scripts/` | Setting up QEMU, fetching the firmware, the launcher's parts, checks |
| `docs/BRINGUP_LOG.md` | What was found on the way, in order |

## License

BSD-3-Clause ([LICENSE](LICENSE)). The firmware the emulator runs is not part of this
repository: the MCU firmware (MIT, some files GPL-3.0) and Flipper OS come from Flipper
Devices' builds, the RP2350 boot ROM from Raspberry Pi's releases.
