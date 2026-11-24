# Emulating the Flipper One

*Survey, 2026-11-23. Public sources only: Flipper Devices' repositories (cloned and read;
nothing in them was built or run), the developer portal (all 85 pages, from the
`flipperone-docs` repository), the published builds, Raspberry Pi's RP2350 boot ROM
release, and press coverage of the launch. Paths are relative to each repository. A
fact marked (inferred) was not read anywhere but follows from what was.*

## Short answer

- The Flipper One is two computers in one case. A **Rockchip RK3576** (4× Cortex-A72 +
  4× Cortex-A53) runs Flipper OS: Debian 13, a mainline-first kernel (7.4-rc4) with
  Flipper's own drivers. A **Raspberry Pi RP2350B** (Cortex-M33, FreeRTOS) owns the
  screen, buttons, touchpad, LEDs, haptics, battery and power, and switches the RK3576
  on and off. No NFC, RFID or Sub-GHz radio. Kickstarter 2026-11-17 to 2026-12-17,
  $399; delivery planned for July 2027.
- Everything an emulator would run is published: the MCU firmware's sources and a
  build of every push (ELF with symbols, UF2), the RP2350's boot ROM (source and
  binaries), and the Linux images (U-Boot, kernel, Debian root) with a manifest of the
  revision of each source.
- Nobody emulates it. QEMU has no RP2350 and no Rockchip machine; the images'
  maintainers call a VM "not a use case we support" (`flipperone-linux-build-scripts`
  issue 46). The nearest thing is a community container, Flipper One UI Lab, that runs
  the UI against a faked device in a browser.
- Emulated the FlipperEmu way (the real, unmodified firmware on models of the hardware),
  it is one QEMU machine with both chips, where the MCU's power switch really powers the
  RK3576. The RP2350 half is about the size of the Zero's machine; the RK3576 half is
  bigger.

## 1. The device

### The CPU side

| | |
|---|---|
| SoC | RK3576: 4× Cortex-A72 + 4× Cortex-A53 (MPIDR 0x100–0x103 and 0–3), Mali-G52 MC3, 6 TOPS NPU. Its own Cortex-M0 is unused |
| RAM | 8 GB in the press; "TBD, LPDDR4 or LPDDR5" in the docs (`general/Tech-Specs.md`) |
| Storage | 64 GB UFS 2.2 (`rockchip,rk3576-ufshc`); microSD (`rockchip,rk3576-dw-mshc`); the eMMC controller is off |
| PMIC | RK806 on I2C1 |
| Ethernet | 2× gigabit: `rockchip,rk3576-gmac` / `snps,dwmac-4.20a`, RGMII, RTL8211F PHYs; `end0`, `end1` |
| Wi-Fi, BT | MediaTek MT7921AUN module (`0e8d:7961`, `mt7921u`) on USB 3, behind a Corechips SL6341 hub (`3431:6341`) |
| USB | two DWC3: one OTG behind a FUSB302 USB-C port (USB-C1: the gadget, and the boot ROM's Maskrom), one host (hub: Wi-Fi, USB-A, USB-C2, M.2) |
| Video | HDMI 2.1 (VOP2 port 0), DisplayPort alt mode on USB-C1 (port 1), Panfrost on the Mali |
| M.2 Key B | PCIe 2.1 ×1, or SATA through an overlay; USB, UART, I2C. 5G/LTE modems, GNSS, satellite |
| Audio | NAU8822 codec on I2C6, SAI2 |
| On I2C2 | GPIO expander (TCA6416 on rev F0B0C1, PCAL6416 on F0B1C2), FUSB302, 5× INA4230, INA219, HD3SS3220, BQ28Z620 gauge, BQ25792/BQ25798 charger. The MCU's "main" bus has the same chips: one bus that the MCU hands to the CPU (inferred from `host.c` and the `i2c2-free` overlay) |
| RTC | HYM8563 on I2C8 |
| GPIO header | 3V3 and 5V, the CPU's USB 2.0, MCU GPIO40/41 (PIO, ADC, PWM), CPU GPIO4_A2–A7 and B0–B5 (I2C, UART, SPI, CAN, PWM, SAI, PDM, SPDIF) |
| Debug port | MCU SWD and reset, MCU UART, CPU reset, CPU UART0 (1,500,000 baud) |

### The MCU side

| | |
|---|---|
| MCU | RP2350B (48 GPIOs): 2× Cortex-M33 + 2× Hazard3, 150 MHz, 520 KB SRAM, 16 MB QSPI flash. The firmware runs on core 0 only, Secure, no RISC-V |
| Display | 2.39", 256×144, 64 grey levels; JD9853 controller over quad SPI, driven by the RP2350's HSTX and DMA; tearing signal at 80 Hz; backlight by PWM |
| Input | 13 buttons (Esc, View, Power, Edit, Run, 5-way pad, Back, App Switcher, PTT) on a PCAL6416 (I2C 0x20); IQS7211E touchpad (0x56); headset key by ADC |
| Output | DRV2605L haptics (0x5A, effects 0–123); 17 WS2812 LEDs on three chains, by PIO |
| Power | BQ2579x charger (0x6B), BQ28Z620 gauge (0x55), INA219 (0x45), FUSB302 (0x22; reset only, its PD code is commented out), HD3SS3220 mux (0x47), TPS62868x display supply (0x47 on the other bus), 5× INA4230; rail switches and `MASKROM_EN` on a second PCAL6416 |

Prototype revisions: F0B0C1 (MCU target `f1`) and 2.F0B1C2 (`f2`). U-Boot tells
them apart by an ADC reading (SARADC channel 2: 0–208 F0B0C1, 209–616 F0B1C2). The
docs say the power and boot sequence is "not final" and that there is no community
testing program yet.

## 2. What is published

| Repository (github.com/flipperdevices) | What it holds |
|---|---|
| `flipperone-mcu-firmware` | The RP2350 firmware: C, Pico SDK 2.3.1, FreeRTOS, furi from the Zero's firmware (about 2k ported lines of 45k). A build of every push at `update.flipperzero.one/builds/flipper-one-mcu/dev/`: `.elf` with symbols, `.bin`, `.uf2`, recovery `.uf2`, for `f1` and `f2`. MIT, some files GPL-3.0 |
| `flipperone-mcu-protobuf` | The MCU's RPC: screen frames, button and touch events, virtual display |
| `flipperone-linux-build-scripts` | debos recipes for the images: the Flipper One and six RK3576 boards. Builds at `dl-linux-images.flipp.dev`; build 2501 (2026-11-24) has a `manifest.json` with every source's revision |
| `flipper-linux-kernel` | The kernel: branch `flipper-devel`, 7.4-rc4 and about 358 commits, the Flipper One device trees and drivers |
| `u-boot` | U-Boot 2026.10 (branch `rk3576`) with Flipper display and button drivers and a BLS boot method |
| `flipctl-slint` | The UI, FlipCTL: Rust and Slint |
| `flipperone-python` | The `flipperone` pip package: the MCU's CLI over its USB port or the probe, SSH to Linux, picotool and rockusb |
| `flipperone-debug-probe` | An RP2350 probe: CMSIS-DAP SWD for the MCU; four serial ports (CPU console, MCU CLI, MCU log, the probe's CLI); CPU and MCU reset |
| `flipperone-docs` | The developer portal's sources |
| `flipperone-hardware` | A README and three pictures. The design files are on Altium 365; no schematic, BOM or pinout is public. Pins come from the firmware and the device trees |
| `flipperone-testing` | Soak tests (stress-ng, glmark2, temperatures) and `fake-flipctl`, the UI before FlipCTL |
| `flipperone-ui` | Figma links; no code |
| raspberrypi/`pico-bootrom-rp2350` | The RP2350 boot ROM's source, and releases A2 to A4 with `bootrom-combined.bin` and ELFs |

## 3. The MCU firmware

- **Runtime.** FreeRTOS (`RP2350_ARM_NTZ` port), one core, 1 kHz tick, tickless idle
  (a plain WFI). `main()` resets core 1 and waits for core 1's boot ROM to answer
  through the SIO FIFO. Clocks are the SDK's defaults: 12 MHz crystal, 150 MHz system,
  48 MHz USB. The DWT cycle counter is on.
- **The RP2350's coprocessors are used.** The SDK's `gpio_put`, `gpio_get` and
  `gpio_set_dir` are instructions of the GPIO coprocessor (CP0), and the PendSV handler
  saves the double-precision coprocessor's state (CP4, `mrrc2 p4`) at every switch
  of thread (`targets/f1/furi_hal/furi_hal_interrupt.c`). The boot ROM uses the
  redundancy coprocessor (RCP, CP7).
- **Flash, 16 MB.** Partition table at 0; firmware A at 0x2000 (7680 KiB), B at
  0x782000 linked to A, data at 0xF02000 (inferred). Normal builds are
  "try before you buy" images: started only by a flash-update boot, they must buy
  themselves before the boot ROM's 16.7 s watchdog. `recovery.uf2` (partition table and
  firmware, no TBYB flag) boots plainly.
- **Boot ROM calls:** partition table and boot info, flash erase/program/flush,
  `otp_access`, `get_sys_info` (chip ID, boot type), `explicit_buy`, reboot (by watchdog
  scratch registers, since `rom_reboot` faults on an RCP canary).
- **Peripherals:**
  - PIO, six state machines: the log's UART transmitter (1.5 Mbaud, GPIO41), an I2C
    master for the "main" bus (GPIO22/23, 400 kHz), three WS2812 chains (GPIO29–31),
    and a 7-instruction SPI-slave sampler that takes the frames from the CPU (CS 25,
    SCK 26, data 28).
  - DMA: to HSTX for the display (IRQ 3), the frame receiver (re-armed from the CS
    interrupt, reads its remaining count), the LEDs (IRQ 1).
  - HSTX on GPIO12–19 for the display.
  - I2C0, a DesignWare master: the "control" bus (GPIO20/21). I2C1, a DesignWare
    slave at 0x69 (GPIO2/3): the CPU's link. Its interrupt path runs from RAM.
  - UART1 (PL011): the CLI at 1.5 Mbaud on GPIO36/37. UART0: an echo test only.
  - USB (TinyUSB): one CDC ACM port with the CLI, `37C1:F101`, "Flipper One MCU Debug".
  - TIMER0 alarms, SysTick, watchdog (reboots only), POWMAN scratch registers, ADC
    (headset key), PWM (backlight 40 kHz; BOOT1 2 kHz on GPIO4).
  - Unused: SPI0/SPI1, RTC/AON timer, TRNG, SHA, PSRAM.
- **Display.** JD9853: commands single-line as `02 00 cmd 00` and parameters, pixels
  quad-line (`32`, address `002C00`, then 258×144 bytes). Window 86 RGB666 columns by
  144 rows: 256×144 grey at 64 levels. A frame starts at the tearing signal (GPIO5);
  without it the firmware marks the display absent and draws nothing.
- **Services:** haptic, input, input_touch, power, pd, usb_mux, headphones, led, gui
  (Clay layout), desktop, i2c_intercom and i2c_negotiator (the CPU's registers),
  host (the CPU's power), updater (A/B over I2C), usb, CLI on UART and USB. At start:
  dmesg, a self-check screen, the CLI, RPC. A crash halts.
- **CLI:** uptime, log, top, free, i2c, gpio, screen (the framebuffer as ANSI), nvm,
  device_info, otp, power, led, input, saradc, desktop, unit_tests, rpc and more.
- **RPC:** `rpc` answers 0xFD, then varint-length-prefixed nanopb messages both ways:
  screen frames (L8, 37152 bytes, plain), button and touch events, start/stop of a
  virtual display. "The same protobuf-based protocol as on Flipper Zero" (docs).
- **Tests:** `unit_tests` on hardware rigs; no host build, simulator or QEMU anywhere.

## 4. Flipper OS

- **The image:** `debian-512-<board>-<id>.img.zst` and `.bmap`: 6 GiB, GPT, 512-byte
  sectors. Partitions: `loader` (from 32 KiB; `u-boot-rockchip.bin` at sector 64),
  `metadata` (60–64 MiB, CBOR), `root` (btrfs `flipperos`, zstd). In the root, one
  subvolume per profile: `@Desktop` (KDE, the default), `@TV-Media-Box` (Kodi),
  `@Router`, `@Minimal`, `@No-Graphics`; `@home` and the logs shared. Built with debos
  and mmdebstrap in a Debian container; a Flipper apt repository has rebuilt cog, mesa,
  ffmpeg, kodi, kwin, sddm.
- **Boot chain:** boot ROM → `boost.bin` (built from U-Boot) → the closed DDR blob
  (rkbin `rk3576_ddr_lp4_2112MHz_lp5_2736MHz_v1.15.bin`) → U-Boot SPL → TF-A BL31
  (upstream, `PLAT=rk3576`) → U-Boot proper 2026.10 (draws on the Flipper's screen,
  reads its buttons over I2C; SD first, then UFS) → a BLS entry
  (`/boot/loader/entries`, a kernel per profile) → Linux. Or Falcon mode: SPL loads a
  boot-menu initramfs (FlipCTL, starts in 2.8 s) that kexecs or switches root into the
  chosen profile. The boot ROM reads its mode from `PMU1_GRF_OS_REG0`, then OTP, then
  a SARADC strap, and loads over USB in Maskrom (`2207:350e`); the docs describe it in
  detail (`hardware/RK3576-Boot-ROM.md`).
- **The kernel:** `flipper-linux-kernel`, `flipper-devel`, 7.4-rc4. Only
  `ARCH_ROCKCHIP` as a platform; virtio as modules; no PL011, generic PCI host, PL031
  or PL061 (inferred from the minimal config), so it does not boot on QEMU's `virt`
  as built. Flipper drivers: `MFD_FLIPPER_ONE` (the MCU), `TINYDRM_FLIPPER_ONE_DISPLAY`,
  `INPUT_FLIPPER_ONE`, `INPUT_FLIPPER_ONE_HAPTIC`, `LEDS_FLIPPER_ONE`,
  `UCSI_FLIPPER_ONE`, `FLIPPER_ONE_MCU_UPDATE` (UF2 over I2C). A Rockchip BSP kernel
  6.1 is built too, for the other boards only.
- **Userspace:** FlipCTL (`flipctl.service`, Rust and Slint) draws Y8 frames on the
  panel's DRM device, reads buttons and touch by evdev, drives the LEDs and haptics,
  and mirrors the screen to a browser on port 8899; apps are AppImages under a headless
  sway. systemd units and a sleep hook write the CPU's state to the MCU
  (`/sys/bus/i2c/drivers/flipper-one-mcu/*/cpustate`). USB-C1 is a gadget: NCM, MTP and
  ACM (`37C1:F12F`), `flipusb0` at 10.0.0.1/24. User `user`, password `user`; SSH.

## 5. Between the two chips

| Line | CPU (RK3576) | MCU (RP2350) | What it carries |
|---|---|---|---|
| I2C | i2c0 (0x27300000), 400 kHz, master | I2C1 slave 0x69 | Registers: 16-bit big-endian address, 16-bit little-endian values, auto-increment |
| Interrupt | GPIO2_A6, level-low | GPIO35 (`f1`; GPIO42 on `f2`), open drain | New events; the `flipper,one-mcu` node is an interrupt controller for 7 of them |
| SPI | spi0 (0x2acf0000), mode 3, 33 MHz, transmit only | PIO sampler, CS 25 / SCK 26 / data 28 | One frame per CS: 258×144 = 37152 bytes Y8 (0 black, 255 white, low 2 bits dropped). No flow control; the MCU asks for a resend by interrupt |
| UART | uart4, "MCU interconnect" (`console=ttyS4`) | a UART | The Linux console to the MCU (which pair is wired to which is not published) |
| Power | the main bus's expander | `VCC5V0_SYS_S5_EN`, `USB20_SEL` | Power on: reset the expander 50 ms, switch the rail on, hand the bus to the CPU; Linux has 30 s to report its state. CPU state 9 (off): power cut 300 ms later |
| Maskrom | — | `MASKROM_EN` | Power on with it held 1 s |
| BOOT1 | a SARADC input | PWM on GPIO4, filtered | A voltage that picks the boot source |

The MCU's registers (`applications/services/i2c_intercom/i2c_registers_map.h`; Linux:
`include/linux/mfd/flipper-one-mcu.h`):

| Address | Register |
|---|---|
| 0x0000 | status |
| 0x0040 | CPU state (0 unknown … 9 powered off, 10 restarting) |
| 0x0080 | version (1) |
| 0x0092, 0x0094 | reset mode, reset delay |
| 0x0100, 0x0180 | input interrupt, its mask |
| 0x0104, 0x0184 | system interrupt (bit 0 MCU restarted, bit 1 frame request), its mask |
| 0x0200–0x020A | buttons, touch X/Y/pressure, headset, software buttons |
| 0x0300–0x0316 | LED brightness, colours (RGB565) |
| 0x0350–0x0354 | backlight |
| 0x0400 | haptic (bit 15 play, 14:8 effect, 7:0 ms) |
| 0x0500–0x070F | USB-C (UCSI): Linux has it, the firmware does not serve it yet |
| 0xF000 | firmware update (256-byte blocks, index, CRC16) |

## 6. How it is developed

Seven sub-projects, each with a GitHub project board: MCU (8), Hardware (9), Docs (10),
Linux (11), UI (12), Testing (14), Mechanics (15). Issues for the community are labelled
"help wanted"; comments are to be contributions. CI: Buildbot (`linux-images.flipp.dev`)
for the images, GitHub Actions for the MCU. The MCU repository has rules for
AI-assisted development (`CONTRIBUTING.md`, `AGENTS.md`). Channels: Discord, X
@Flipper_RND, a weekly digest, a dev log (11 entries). Before the device ships, people
use RK3576 boards: ArmSoM Sige5 / Banana Pi BPI-M5 Pro (recommended), Radxa ROCK 4D,
FriendlyElec NanoPi M5, Luckfox Omni3576, Firefly ROC-RK3576-PC.

## 7. Prior art

- **The Flipper One:** none. Flipper One UI Lab (kalicyh/flipperone-lab, community) runs
  the public userspace in a container with a faked FlipCTL in a browser. FlipCTL itself
  has a headless renderer and the web view. The MCU's RPC streams its screen and takes
  input, for automated UI tests on hardware.
- **RP2350:** Wokwi simulates one M33 core, without the RP2350's new PIO instructions;
  picoem (Rust) is cycle-accurate and slow. No QEMU or Renode model.
- **RK3576 (or any Rockchip SoC):** no QEMU model; people use `virt` as a stand-in.

## 8. Emulating it

**One machine, both chips.** A `flipper-one` machine in `qemu-system-aarch64` (which
has the M-profile CPUs too): the RP2350's M33 cores in a cluster of their own, the
RK3576's A53/A72 cores in another, each with its own view of memory, as QEMU's ZynqMP
does with its A53s and R5s. The RK3576 stays off until the MCU firmware switches its
rail on; `MASKROM_EN`, the BOOT1 voltage, the I2C link, the frame SPI, the interrupt
line and the console UART are wired inside the machine. One clock, one snapshot, one gdb
with both chips as inferiors. Running the MCU alone is the machine with Linux off,
which the real device does too.

**What QEMU has:** Cortex-M33, Cortex-A53 and A72, the GIC-400 (`arm_gic`), the generic
timer, PL011 (the RP2350's UARTs), a 16550 (base of the DesignWare UART), PL330 DMA,
SDHCI, a UFS device model (PCI only), sysbus xHCI, PSCI by the emulator.

**What it lacks:**
- The RP2350: SIO, resets, clocks/PLLs, timers, watchdog, POWMAN, IO/pads, QMI and the
  XIP flash with its address translation, OTP, PIO, DMA, HSTX, DesignWare I2C (master
  and slave), USB, ADC, PWM; and the M-profile coprocessor instructions (CP0, CP4,
  CP7) as hooks for a machine: a patch to QEMU's ARM CPU code.
- The RK3576: CRU, PMU and power domains, the GRFs, pinctrl and GPIO, the timer,
  rk3x I2C, SPI, SARADC, OTP, dw-mshc, the UFS glue, GMAC (dwmac 4.20a), DWC3 device
  mode, PCIe, VOP2/HDMI; an SCMI answer for the kernel's clocks if TF-A is skipped.
- The boards' chips on both sides: two PCAL6416, IQS7211E, DRV2605L, BQ2579x, BQ28Z620,
  INA219, INA4230, FUSB302, HD3SS3220, TPS62868x, WS2812, JD9853; RK806, HYM8563,
  NAU8822, RTL8211F.

**An order of work:**
1. The RP2350 to its self-check screen and CLI: the real boot ROM, the flash from
   `recovery.uf2`, the coprocessors, clocks, resets, timer, watchdog, POWMAN, SIO, IO,
   PIO, DMA, HSTX and the JD9853 to a window, DesignWare I2C, PL011.
2. The MCU's chips, to the desktop: buttons, touch, LEDs, haptics, battery; the RPC's
   screen stream; USB CDC; BOOTSEL and a UF2 update through the boot ROM (a host: the
   USB/IP server written for the Zero, and `fe_msc.py`).
3. The RK3576 to a shell: the published kernel and device tree started directly
   (QEMU's PSCI), then U-Boot from the image. CRU, PMU, GRFs, pinctrl, GPIO, timers,
   UART, SD (dw-mshc) or UFS.
4. The link: the MCU's register slave on i2c0, the frame sampler on spi0, the
   interrupt, power and Maskrom: FlipCTL on the emulated screen, the CPU state, the MCU
   update over I2C.
5. Ethernet, the USB gadget (over USB/IP), M.2/PCIe, audio, HDMI.

The hard parts: the coprocessors, the boot ROM's path (partitions, TBYB, RCP), PIO,
HSTX with the panel, the DesignWare I2C slave; on the RK3576, enough of the clock and
power trees for the kernel's drivers to go on, UFS or dw-mshc, GMAC.

## 9. Open questions

- RAM size and type (docs undecided).
- Which RK3576 UART meets which MCU UART; how BOOT1's voltages map to boot sources;
  how the main I2C bus passes between the MCU and the CPU (inferred, not documented).
- Rev F0B0C1: the device tree names a TCA6416 at 0x20 where the `f1` firmware writes
  PCAL6416-only registers.
- The power and boot sequence, the boot menu and FlipCTL's architecture are still
  changing (the docs describe it once as WebKit on DRM, once as Rust and Slint).
- No schematic: pins are known only from the firmware and the device trees.
