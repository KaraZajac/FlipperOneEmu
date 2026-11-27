# FlipperOneEmu

Run the [Flipper One](https://flipper.net/pages/flipper-one)'s real firmware on your PC.
The Flipper One is two computers in one case: a Rockchip RK3576 that runs Flipper OS
(Debian), and a Raspberry Pi RP2350 that runs the screen, buttons, LEDs, battery and
power, and switches the RK3576 on. This is one QEMU machine with both chips. It runs their
unmodified firmware: the MCU firmware's `.uf2`/`.elf` as Flipper Devices builds it, the
RP2350's own boot ROM, and the Flipper OS image.

It is the sibling of [FlipperZeroEmu](https://github.com/KaraZajac/FlipperZeroEmu), which
does the same for the Flipper Zero.

**Status (2026-11-26): the whole Flipper One runs: its MCU firmware and Flipper OS,
with FlipCTL on the screen, and Flipper OS installs onto it as the docs do it.** The
RP2350's own boot ROM verifies the flash's partition table and starts the MCU firmware
on the emulated chip. The firmware starts FreeRTOS, logs through its PIO UART, draws on
the screen in the window (over HSTX and DMA to the emulated JD9853 panel), lights the
backlight, answers on its CLI, and takes the window's buttons (through the control
bus's I2C expander) and touchpad (through its IQS7211E): its desktop, menus and
touchpad test work. Its haptic driver buzzes (the window shakes), its 17 LEDs light in
the window as it drives them, and its settings are kept in the flash across restarts.
Its main bus (I2C from a PIO program) reaches the main board's expander, which powers
the Linux side, the battery's gauge (the status bar shows its charge), its charger, the
system rail's current meter, the five power monitors on the Linux side's rails
(`power consumption`; the CPU rails follow how many cores run) and the USB-C port's
chips: the self-check finds every chip. A charger can be plugged into its USB-C port,
and the battery charges; a computer plugged in gets its USB serial port, also as a real
USB device on this PC (USB/IP), and the firmware can be updated as picotool updates it,
through the RP2350's boot ROM. Its `power reboot` power-cycles the board through the
charger, and its power off (ship mode) turns it off until a charger is plugged in;
after either, as on the device, Flipper OS stays off until the Power tab's Start
Flipper OS (the emulator itself starts as a device in use, Flipper OS on). The MCU's
Maskrom puts the RK3576 in its MaskROM (BOOT0), where its boot ROM takes a USB
download, as rockusb's download-boot sends one. The RK3576 (four Cortex-A53s and four
A72s) boots a published Flipper OS build from its image on an emulated microSD card, to
its login: its own kernel and device tree, Debian 13 under systemd, and FlipCTL, which
draws Flipper OS's interface on the Flipper's screen (its frames go over the display
SPI to the MCU firmware, which shows them) and answers its keys (which the MCU firmware
passes on over their I2C link). Its CPU clusters' clocks come from TF-A's SCMI
(cpufreq), its temperature sensors, calibrated from its OTP, read the chip warming as
its cores run, its SARADC reads the boot-order strap, the MCU's BOOT1 and the board's
revision (which U-Boot tells the board by), and its 64 GB UFS (UFS 2.2 on an HS-G3
link, new), provisioned as the Flipper OS installer has it (a system LU, boot LUs A and
B, recovery), is blank until something is installed on it. Its two Ethernet ports have
their PHYs, and with `--net` the first is on this PC's network: DHCP, DNS, and the
internet through NAT. Its USB controllers come up, the second with the board's USB hub
on it (a CoreChips SL6341: its USB 2.0 and SuperSpeed hubs, the MT7921 at SuperSpeed on
its port 1; USB devices can be plugged into its USB-A and USB-C2 ports from QEMU's
monitor), and with them its Type-C port, charger and battery: FlipCTL shows the charge,
the power flow and both temperatures. A computer plugged into the USB-C port while
Flipper OS runs reaches the RK3576, as on the device (the MCU switches the port's USB
to it with its rail), and enumerates Flipper OS's USB gadget at high speed: its serial
port's login on `devices/NAME/usb.sock`, and over USB/IP the whole gadget on this PC:
its network (NCM, with a DHCP server on Flipper OS's side), MTP and the serial port.
Its sound card (the NAU8822 codec on SAI2) plays, at 48 kHz: to a WAV file, or nowhere
(`run --audio`); its microphones (the internal one, and a headset's in the jack) hear a
WAV file (`run --mic`). An NVMe SSD can be put in its M.2 slot (`run --nvme`), on
PCIe0, and a monitor in its HDMI port (`run --hdmi`): Linux lights it at 1280x720 with
its console on it, and `shot --hdmi` takes its picture. It reaches its login in about a
minute and a half to two (TCG emulates eight cores). The MCU switches the RK3576 on and
off as on the device: Flipper OS's power-off and reboot work, and so does the MCU's own
Stop. Its Wi-Fi and Bluetooth are the MT7921's, on USB1 while Linux unblocks its
rfkill: the chip takes its real firmware from mt7921u and btusb; NetworkManager has its
Wi-Fi (FlipCTL's Wi-Fi menu with it, the Wi-Fi LED lit once it joins) and BlueZ its
controller, whose scans find an LE beacon and a sensor near it, the sensor one that
BlueZ connects to (its GATT services: its battery, its heart rate). With `--wifi` an
802.11n access point is near it (144.4 Mbit/s), on this PC's network (through passt):
an open network and a WPA2 one, which the Wi-Fi finds and joins, from FlipCTL too;
nothing else is ever found (no radio). It also boots as the device does, through U-Boot
from the card (its boot menu on the Flipper's screen; with KASLR, and so KPTI, about
two and a half minutes here), and Flipper OS installs onto it as the docs do it: in its
MaskROM, rockusb's download-boot of the installer's loader (`scripts/fo_rockusb.py`,
over USB/IP); the installer provisions the UFS, installs Flipper OS and updates the
MCU's firmware over I2C, and the boot ROM then boots the UFS. Flipper OS updates the
MCU's firmware as on the device, and Rockusb's Loader mode (a Rockchip loader's
usbplug, or U-Boot's rockusb) reads and writes the UFS or the card from the PC. The
RK3576's DisplayPort (over the USB-C port) and more come next.
[RESEARCH.md](RESEARCH.md) is the survey this starts from: the device, what Flipper
Devices publishes, and what emulating it takes. [PLAN.md](PLAN.md) is the order of
work; the MCU comes first. [docs/BRINGUP_LOG.md](docs/BRINGUP_LOG.md) is what was found
on the way.

## Trying it

```sh
scripts/setup_qemu.sh        # QEMU 11.1.3, its patches and this machine (about 6 minutes)
scripts/fetch_firmware.py    # the MCU firmware's newest build, and the RP2350 boot ROM
./flipperoneemu gui          # the window: it starts a device, devices/one
scripts/fetch_os.py          # a Flipper OS build (2 GB), for ...
./flipperoneemu gui --linux  # ... the whole device: Flipper OS on its RK3576 too
./flipperoneemu gui --linux --cpus 4  # ... on four of its eight cores
./flipperoneemu gui --linux --net --wifi  # ... on this PC's network: its Ethernet, and
                             # an access point near it (passt): FlipCTL's Network >
                             # Wi-Fi joins FlipperOneEmu, or FlipperOneEmu-WPA2
                             # (passphrase flipperone)
./flipperoneemu gui --uboot  # ... through U-Boot, as the device boots: its boot menu on
                             # the screen, the pad to pick a profile
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
./flipperoneemu run --linux --wifi ... # ... and an access point near it, on this PC's
                                     # network (through passt): an open network,
                                     # FlipperOneEmu, and a WPA2 one, FlipperOneEmu-WPA2
                                     # (passphrase flipperone, or --wifi-psk's), which its
                                     # Wi-Fi (the MT7921) finds and joins: FlipCTL's Network >
                                     # Wi-Fi, or nmcli dev wifi connect
./flipperoneemu run --linux --lan ... # ... and a cable in its second port, to a LAN at
                                     # devices/NAME/lan.sock (QEMU's stream netdev): a VM
                                     # joins it with -netdev stream,addr.type=unix,
                                     # addr.path=...; scripts/lan_host.py is a host there
                                     # (the LAN's DHCP server and gateway, or with
                                     # --client a DHCP client of the Flipper's)
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
                                     # its UFS (devices/NAME/ufs*.img: LU 0, the boot LUs,
                                     # recovery) blank until the installer fills it
./flipperoneemu run --linux --cpus 4 ...  # ... on four of its eight cores (the device has
                                     # eight; here both boot to the login in some 70-110 s,
                                     # since QEMU patches 0019-0022)
./flipperoneemu run --linux --deep-idle ...  # ... with the cores' power-down idle
                                     # state (cpu-sleep) that the device's tree has; the
                                     # machine drops it otherwise: QEMU's PSCI runs it as a
                                     # WFI, and without it QEMU idles at a third of the host
                                     # CPU, an IPI to an idle core a quarter as slow
./flipperoneemu run --linux --uboot ...  # ... through U-Boot from that card, as on the
                                     # device: its boot menu on the Flipper's screen (the
                                     # pad's up, down and OK pick a profile and kernel; hold
                                     # a key a moment, U-Boot is slow here), the entry's own
                                     # options (KASLR too, as on the device, and with it
                                     # KPTI, whose flips at each entry to the kernel keep
                                     # QEMU's TLB since patch 0024);
                                     # `s` on the console at power-on stops it at
                                     # U-Boot's shell, where `rockusb 0 mmc 0` makes USB0 a
                                     # Rockusb device, the card behind it (with --usbip 3240:
scripts/fo_rockusb.py --port 3240 read 0 8 FILE | write START FILE  # Rockusb's Loader
                                     # mode, as rockusb's rl and wl, over USB/IP)
./flipperoneemu run --installer FIT --net  # the Flipper OS installer, as rockusb's
                                     # download-boot starts it from the MaskROM: its
                                     # installer-falcon.itb (dl-linux-images.flipp.dev/
                                     # falcon-installer/, BUILD/flipper-one/), the machine
                                     # as SPL in Falcon mode. Its interface on the screen
                                     # (the pad, OK, Back): it installs Flipper OS on the
                                     # UFS, and the MCU's firmware
./flipperoneemu run --uboot          # ... a device's own storage, as its boot ROM boots
                                     # it: the UFS's boot LU (once installed), or else the
                                     # microSD card; with neither (a new device), or with
                                     # the MCU's Maskrom, it waits in its MaskROM, USB0 a
                                     # device (2207:350e) for a USB download (so it does
                                     # after Flipper OS's `sudo systemctl reboot
                                     # --reboot-argument=maskrom`, as on the device):
scripts/fo_rockusb.py --port 3240 download-boot installer-falcon-loader.bin  # rockusb's,
                                     # over USB/IP (`run --uboot --usbip 3240 --net`, a
                                     # computer plugged in: Ctrl+Shift+U): the installer
                                     # starts, the docs' way (`list` finds the device;
                                     # sudo scripts/usbip_attach.py: the real rockusb)
scripts/fo_rockusb.py --port 3240 download-boot rk3576_loader.bin  # ... or Rockchip's
                                     # own (rkbin's, its usbplug): the machine plays the
                                     # usbplug, Rockusb's Loader mode on USB0, the UFS's
                                     # LU 0 behind it (`switch-storage sd`: the card);
                                     # then read, write, flash-info, reset [maskrom]
                                     # (rkdeveloptool's rl, wl, rfi, rd [3]), and
                                     # write-bmap: Flipper OS's image (.img.zst) as its
                                     # bmap maps it, onto the card (switch-storage sd),
                                     # which then boots
./flipperoneemu console [-n NAME] [CMD]  # Linux's console (the RK3576's UART0); Ctrl-] leaves
./flipperoneemu console --usb ...    # ... Flipper OS's serial port on its USB gadget
                                     # (a computer plugged in: Ctrl+Shift+U)
                                     # The MCU's firmware from Flipper OS, as on the
                                     # device: a .uf2 written to /sys/class/firmware/
                                     # flipper-one-mcu/data (loading 1, then 0) goes to the
                                     # MCU over I2C; Linux's next reboot boots it
socat - UNIX-CONNECT:devices/NAME/monitor.sock  # QEMU's monitor: device_add usb-kbd,bus=usb-bus.1
                                     # plugs a keyboard into the USB-A port (the board's
                                     # hub's port 1.2; 1.3 is USB-C2's, and 2.2 and 2.3
                                     # theirs at SuperSpeed, for a USB 3 device)
scripts/fo_picoboot.py --port 3240 load FIRMWARE.uf2 --reboot
                                     # an update, as picotool makes one: after the
                                     # CLI's `power boot` (or --bootsel), a computer
                                     # plugged in (Ctrl+Shift+U)
./flipperoneemu log [-n NAME]        # the firmware's log, from its RTT buffer
scripts/fo_rpc.py [-n NAME] [--frames N] [--press KEY ...]  # the firmware's RPC, as a
                                     # computer's app speaks it: its screen streamed
                                     # to PNGs, its keys pressed (--usb: over USB)
./flipperoneemu cli [-n NAME] [CMD]  # the firmware's CLI (UART1); Ctrl-] leaves
./flipperoneemu cli --usb ...         # ... on its USB serial port (a computer plugged in)
./flipperoneemu where [-n NAME]      # where each core is: its PC and function
./flipperoneemu gdb [-n NAME]        # gdb on a device run with --gdb PORT: the MCU's
                                     # two cores, its firmware's symbols and the boot
                                     # ROM's; --linux adds the RK3576's clusters as
                                     # inferiors 2 and 3; -- passes gdb more
./flipperoneemu shot FILE.png        # the screen as a picture
./flipperoneemu shot --hdmi FILE.png # ... or what the HDMI port shows (run --hdmi)
./flipperoneemu status | stop [-n NAME]
scripts/smoke_test.py                # a throwaway device, checked: PASS or FAIL
scripts/linux_check.py               # ... with Flipper OS too: boot, screen, keys, sensors,
                                     # USB, reboot, power-off (keeps a booted card in build/)
scripts/linux_check.py --maskrom [--loader FILE]  # ... or a new device's boot ROM in
                                     # its USB download: the maskrom protocol, a loader's
                                     # download-boot (the installer's) or a usbplug's
                                     # Loader mode (the UFS and the card written), the
                                     # MCU's Maskrom
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
| 0015 | hw/ufs: answer a LUN with no logical unit, and the configuration descriptor (U-Boot's scan) |
| 0016 | hw/char/pl011: a transmit that does not wait for the chardev (a program that stops reading cli.sock stopped the machine) |
| 0017 | hw/ufs: a device provisioned: boot LUs, a writable bBootLunEn kept in the device's own state, enhanced1, the configuration descriptor (the Flipper OS installer's scheme) |
| 0018 | hw/intc/arm_gic: one update for a distributor write (a 32-bit write, as Linux masks an interrupt with, looked at every interrupt for every CPU four times) |
| 0019 | accel/tcg, target/arm: no BQL for an interrupt the CPU cannot take (a masked one took and let go of the BQL at each pass of the vCPU's loop) |
| 0020 | accel/tcg/mttcg: an interrupt without an exit request (each one another thread raised made cpu_exec return, and the vCPU take the BQL) |
| 0021 | accel/tcg/cputlb: reset dirty only in the mmu_idx in use (every vCPU's sixteen, each time a page of RAM got its first TB) |
| 0022 | hw/intc/arm_gic: one pass over the interrupts for every CPU (a GIC update looked at every interrupt once per CPU) |
| 0023 | hw/ufs: a reference clock gating wait time (bRefClkGatingWaitTime read 0, which Linux complained of) |
| 0024 | target/arm: Linux's KPTI flips of TTBR1_EL1 keep the TLB (under U-Boot's KASLR each system call and interrupt from EL0 flushed it twice) |
| 0025 | hw/ufs: the device's version, its link's highest gear and its health as properties (QEMU's was always UFS 4.1 on HS-G4, its health not given) |
| 0026 | hw/usb/hcd-xhci: a DWC3's ports: USB 2 first, each xHCI port a USB port of its own (QEMU puts USB 3 first and pairs them, so a USB 3 hub's two hubs could not both be on a connection) |
| 0027 | target/arm: the generic timer at its frequency exactly (QEMU counts a tick each whole ns: at 24 MHz each 41 ns, 1.6 % fast; NTP stepped Flipper OS's clock back at each poll), and WFIT/WFET/WFE's timer armed in ns, not ticks |

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
