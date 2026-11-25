# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-25): the whole Flipper One runs: its MCU firmware and Flipper OS,
with FlipCTL on the screen.** The RP2350's own boot ROM verifies
the flash's partition table and starts the MCU firmware on the emulated chip. The
firmware starts FreeRTOS, logs through its PIO UART, draws on the screen in the window
(over HSTX and DMA to the emulated JD9853 panel), lights the backlight, answers on its
CLI, and takes the window's buttons (through the control bus's I2C expander) and
touchpad (through its IQS7211E): its desktop, menus and touchpad test work. Its haptic
driver buzzes (the window shakes), its 17 LEDs light in the window as it drives them,
and its settings are kept in the flash across restarts. Its main bus (I2C from a PIO
program) reaches the main board's expander, which powers the Linux side, the battery's
gauge (the status bar shows its charge), its charger, the system rail's current meter,
the five power monitors on the Linux side's rails (`power consumption`; the CPU rails
follow how many cores run) and the USB-C port's chips: the self-check finds every chip. A charger can be plugged
into its USB-C port, and the battery charges; a computer plugged in gets its USB serial
port, also as a real USB device on this PC (USB/IP), and the firmware can be updated as
picotool updates it, through the RP2350's boot ROM. Its `power reboot` power-cycles the
board through the charger, and its power off (ship mode) turns it off until a charger
is plugged in. The RK3576 (four Cortex-A53s and four A72s) boots a published
Flipper OS build from its image on an emulated microSD card, to its login: its own
kernel and device tree, Debian 13 under systemd, and FlipCTL, which draws Flipper OS's
interface on the Flipper's screen (its frames go over the display SPI to the MCU
firmware, which shows them) and answers its keys (which the MCU firmware passes on over
their I2C link). Its CPU clusters' clocks come from TF-A's SCMI (cpufreq), its
temperature sensors, calibrated from its OTP, read the chip warming as its cores run,
and its 64 GB UFS (blank, as one that nothing was installed on) is a disk to Linux.
Its two Ethernet ports have their PHYs, and with `--net` the first is on this PC's
network: DHCP, DNS, and the internet through NAT. Its USB controllers come up (USB
devices can be plugged into the second from QEMU's monitor), and with them its Type-C
port, charger and battery: FlipCTL shows the charge, the power flow and both
temperatures. A computer plugged into the USB-C port while Flipper OS runs reaches
the RK3576, as on the device (the MCU switches the port's USB to it with its rail),
and enumerates Flipper OS's USB gadget at high speed: its serial port's login on
`devices/NAME/usb.sock`, and over USB/IP the whole gadget on this PC: its network
(NCM, with a DHCP server on Flipper OS's side), MTP and the serial port. Its sound card
(the NAU8822 codec on SAI2) plays, at 48 kHz: to a WAV file, or nowhere (`run --audio`);
its microphones (the internal one, and a headset's in the jack) hear a WAV file
(`run --mic`). An NVMe SSD can be put in its M.2 slot (`run --nvme`), on PCIe0, and
a monitor in its HDMI port (`run --hdmi`): Linux lights it at 1280x720 with its
console on it, and `shot --hdmi` takes its picture.
It reaches its login in two to three minutes, the first boot included
(TCG emulates eight cores). The MCU switches the RK3576 on and off as on the device:
Flipper OS's power-off and reboot work, and so does the MCU's own Stop (now and then a
power-off or reboot waits 90 s for BlueZ's `mpris-proxy`, which has no Bluetooth
adapter: the Wi-Fi and Bluetooth chip is on USB, not modelled yet). The RK3576's
DisplayPort (over the USB-C port) and more come next. [RESEARCH.md](RESEARCH.md) is the survey
this starts from:
the device, what Flipper Devices publishes, and what emulating it takes.
[PLAN.md](PLAN.md) is the order of work; the MCU comes first.
[docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md) is what was found on the way.

## Trying it

```sh
scripts/setup_qemu.sh        # QEMU 11.1.3, its patches and this machine (about 6 minutes)
scripts/fetch_firmware.py    # the MCU firmware's newest build, and the RP2350 boot ROM
./flipperoneemu gui          # the window: it starts a device, devices/one
scripts/fetch_os.py          # a Flipper OS build (2 GB), for ...
./flipperoneemu gui --linux  # ... the whole device: Flipper OS on its RK3576 too
```

The window draws a Flipper One around the emulated screen, part by part where the device
has them: this project's own vector drawing, measured from the device's published renders
(it is not Flipper Devices' artwork, and the device's name stands where their wordmark
is), with its LEDs where the device has them. Click its buttons, or use the keyboard:
the arrows and Enter are the pad, Backspace or Esc is Back, F1 to F5 are the soft keys
(Esc, View, Power, Edit, Run), Tab is App Switcher, Space is PTT; drag on the touchpad;
Ctrl+U plugs a charger into the USB-C port, Ctrl+Shift+U a computer; the Device menu
also plugs a monitor into the HDMI port, and sets the temperature around it (the battery's, and what the RK3576 warms from: at
85 °C Linux slows its cores, at 110 °C it cannot run). It talks to the
emulator over a front-panel link, [docs/PANEL_LINK.md](docs/PANEL_LINK.md), which any program can use.
Another,
[docs/HOST_LINK.md](docs/HOST_LINK.md) (`devices/NAME/host.sock`), lets a program take
the RK3576's place on its link to the MCU: the I2C registers (CPU state, LEDs, haptics,
input), the interrupt line, and the screen: frames it sends go over the display's SPI
as Flipper OS sends them, and the MCU shows them.

At its start the MCU firmware powers the Linux side, as the device does, and shows its
screen. Without `--linux` nothing runs on the RK3576, so after a few seconds that screen
says "CPU not responding". Power opens the Flipper OS menu, and Stop there turns the
Linux side off and brings the MCU's own desktop. With `--linux` the RK3576 starts when
the MCU switches its power on, and Flipper OS's interface replaces that screen.

```sh
./flipperoneemu run [-n NAME] [--detach] [--gdb PORT]   # a device without the window
./flipperoneemu run --bootsel ...    # ... started with BOOTSEL held: the boot ROM's USB boot
./flipperoneemu run --usbip 3240 ... # ... its USB given to this PC: sudo scripts/usbip_attach.py
                                     # (with Flipper OS running, its USB gadget's)
scripts/fetch_os.py                  # a Flipper OS build: its kernel, initrd and device trees
./flipperoneemu run --linux --net ... # ... with its first Ethernet port on this PC's
                                     # network (NAT, through passt: installed with podman)
./flipperoneemu run --linux --audio FILE.wav ...  # ... the sound Flipper OS plays, into
                                     # a WAV file (or a driver this QEMU has: this PC's
                                     # sound needs pipewire-devel when QEMU is configured)
./flipperoneemu run --linux --mic FILE.wav ...  # ... and what its internal microphone
                                     # hears: a WAV file (16-bit PCM), looped
./flipperoneemu run --linux --hdmi ...  # ... a monitor in the HDMI port (Linux lights it,
                                     # 1280x720, its console on it; the panel link's
                                     # `hdmi 1|0` plugs it in and out)
./flipperoneemu run --linux --hdmi --vnc 5901 ...  # ... and its picture, live, to a VNC
                                     # viewer at 127.0.0.1:5901
./flipperoneemu run --linux --nvme [FILE.img] ...  # ... an NVMe SSD in the M.2 slot,
                                     # its disk a raw image (devices/NAME/nvme.img: a
                                     # blank 256 GB, sparse, unless told)
./flipperoneemu run --linux --kernel-args 'ARGS' ...  # ... with more on the kernel's
                                     # command line (systemd.mask=serial-getty@flipserial0.service
                                     # avoids Flipper OS's OS-02, docs/FIRMWARE_BUGS.md)
./flipperoneemu run --linux --ssh 2222 ...  # ... and this PC's port 2222 to its SSH:
                                     # ssh -p 2222 user@127.0.0.1 (set a password with
                                     # passwd on its console first, which logs in itself)
./flipperoneemu run --linux ...      # ... Flipper OS on the RK3576, from the build's
                                     # image on the device's microSD (devices/NAME/sd.img);
                                     # its UFS, devices/NAME/ufs.img, is blank
./flipperoneemu console [-n NAME] [CMD]  # Linux's console (the RK3576's UART0); Ctrl-] leaves
./flipperoneemu console --usb ...    # ... Flipper OS's serial port on its USB gadget
                                     # (a computer plugged in: Ctrl+Shift+U)
socat - UNIX-CONNECT:devices/NAME/monitor.sock  # QEMU's monitor: device_add usb-kbd,bus=usb-bus.1
                                     # plugs a keyboard into the RK3576's USB1
scripts/fo_picoboot.py --port 3240 load FIRMWARE.uf2 --reboot
                                     # an update, as picotool makes one: after the
                                     # CLI's `power boot` (or --bootsel), a computer
                                     # plugged in (Ctrl+Shift+U)
./flipperoneemu log [-n NAME]        # the firmware's log, from its RTT buffer
./flipperoneemu cli [-n NAME] [CMD]  # the firmware's CLI (UART1); Ctrl-] leaves
./flipperoneemu cli --usb ...         # ... on its USB serial port (a computer plugged in)
./flipperoneemu where [-n NAME]      # where each core is: its PC and function
./flipperoneemu shot FILE.png        # the screen as a picture
./flipperoneemu shot --hdmi FILE.png # ... or what the HDMI port shows (run --hdmi)
./flipperoneemu status | stop [-n NAME]
scripts/smoke_test.py                # a throwaway device, checked: PASS or FAIL
scripts/linux_check.py               # ... with Flipper OS too: boot, screen, keys, sensors,
                                     # USB, reboot, power-off (keeps a booted card in build/)
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
| 0005 | target/arm: let a machine answer the calls QEMU's PSCI takes (power off and reset of one chip) |
| 0006 | target/arm: read the generic timer's counters without the BQL |
| 0007 | hw/sd/sd: move the card's data without a request per block or a check per byte |
| 0008 | hw/ufs: answer DME_RESET and DME_ENABLE (Rockchip's UFS driver sends them) |
| 0009 | hw/ufs: the link's lanes as a property (the RK3576's link has two) |
| 0010 | hw/ufs: let WriteBooster's flush during hibernation be set |
| 0011 | hw/usb/hcd-xhci: a property for an ERSTBA written high then low (as DWC3's) |
| 0012 | hw/dma/pl330: let the machine go on during a long transfer |
| 0013 | target/arm: an exception return without the BQL when the PMU is idle |
| 0014 | hw/sd/sd: a multi-block transfer's bytes a block at a time |

## Layout

| Path | What |
|---|---|
| `qemu/hw/arm/flipper_one/` | The machine and its devices, linked into QEMU's source tree |
| `qemu/patches/` | Changes to QEMU itself |
| `flipperoneemu` | The launcher: run, the window, stop, status, log, cli, shot, where |
| `scripts/` | Setting up QEMU, fetching the firmware, the window (`fo_gui.py`, `fo_view.py`, `fo_link.py`), the launcher's parts, checks |
| `docs/BRINGUP_LOG.md` | What was found on the way, in order |
| `docs/FIRMWARE_BUGS.md` | Bugs in the MCU firmware and Flipper OS that the emulator turned up |

## License

BSD-3-Clause ([LICENSE](LICENSE)). The firmware the emulator runs is not part of this
repository: the MCU firmware (MIT, some files GPL-3.0) and Flipper OS come from Flipper
Devices' builds, the RP2350 boot ROM from Raspberry Pi's releases.
