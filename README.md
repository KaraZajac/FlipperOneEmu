# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-23): just started.** The RP2350's own boot ROM runs on the emulated
chip through its security and salt set-up to its first look at the flash; the MCU
firmware does not run yet. [RESEARCH.md](RESEARCH.md) is the survey this starts from:
the device, what Flipper Devices publishes, and what emulating it takes.
[PLAN.md](PLAN.md) is the order of work; the MCU comes first.
[docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md) is what was found on the way.

## Trying it

```sh
scripts/setup_qemu.sh        # QEMU 11.1.3, its patches and this machine (about 6 minutes)
scripts/fetch_firmware.py    # the MCU firmware's newest build, and the RP2350 boot ROM
scripts/uf2.py build/flash.bin firmware/mcu/*/flipper-one-mcu-f1-recovery-*.uf2
build/qemu/qemu-system-aarch64 -M flipper-one,flash=build/flash.bin \
    -bios firmware/bootrom/A4/bootrom-combined.bin -display none \
    -d unimp,guest_errors        # every access to what is not modelled yet
```

`-M flipper-one,bootsel=on` starts with the BOOTSEL button held.

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
| `scripts/` | Setting up QEMU, fetching the firmware, the launcher's parts, checks |
| `docs/BRINGUP_LOG.md` | What was found on the way, in order |

## License

BSD-3-Clause ([LICENSE](LICENSE)). The firmware the emulator runs is not part of this
repository: the MCU firmware (MIT, some files GPL-3.0) and Flipper OS come from Flipper
Devices' builds, the RP2350 boot ROM from Raspberry Pi's releases.
