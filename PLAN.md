# Plan

Goal: the Flipper One's real firmware without the device. The RP2350 runs the MCU
firmware with its own boot ROM, the RK3576 runs Flipper OS, both in one QEMU machine,
with FlipperZeroEmu's debugger-first workflow. Why one machine with both chips:
RESEARCH.md §8.

## M1: the MCU boots ☐
The RP2350B and the published MCU firmware (target `f1`, `dev` builds) to its self-check
screen and its CLI.
- ☑ QEMU 11.1.3 built for aarch64 (`qemu-system-aarch64` has the M-profile CPUs too),
      with the overlay `qemu/hw/arm/flipper_one` (2026-11-23)
- ◐ The RP2350's memory map: boot ROM (Raspberry Pi's release A4), 520 KiB of SRAM,
      16 MiB of XIP flash, the USB controller's RAM, every peripheral block (what is not
      modelled logs), and the atomic aliases (+0x1000 XOR, +0x2000 set, +0x3000 clear)
- ☑ Both Cortex-M33s (Secure, FPU, DSP, 8 MPU and 8 SAU regions) with the RP2350's
      IDAU; PSM powers core 1, which waits in its ROM (2026-11-23)
- ☑ The coprocessors: GPIO (CP0), double-precision (CP4 and CP5, the DCP, 2026-11-24),
      redundancy (CP7, the boot ROM's). QEMU patch 0001 lets a machine have them
- ◐ Boot: the boot ROM verifies the partition table and starts the firmware from
      partition A (QMI direct mode, the flash, ATRANS, SHA-256); try-before-you-buy:
      the flash-update boot of partition B and the image's buy, as an update from
      Flipper OS makes them (2026-11-25); OTP reads as blank
- ☑ The firmware's settings (its key-value store in the flash's NVM partition):
      written through the boot ROM's program and erase, kept across restarts
      (2026-11-24)
- ◐ Clocks, resets, PSM, XOSC, PLLs, ROSC, TRNG, SHA-256, POWMAN, watchdog, SIO
      (GPIO, FIFOs, spinlocks, doorbells), BOOTRAM's locks, TIMER0/1, the ADC, DWT's
      cycle counter answer; PADS keeps what is written; TICKS to come
- ◐ PIO (three blocks, the RP2350's instructions; the firmware's log UART decoded
      from GPIO41 to devices/NAME/mcu.log, 2026-11-23); the pins and IO_BANK0's
      interrupts; DMA (16 channels) and HSTX (2026-11-23); PWM's duty (2026-11-23)
- ☑ The JD9853 panel and its tearing signal: the firmware's self-check screen in the
      window and to PNG (2026-11-23)
- ☑ The PL011 UARTs: the CLI on UART1 answers (`./flipperoneemu cli`, 2026-11-23)
- ☑ DesignWare I2C: the master (I2C0, the control bus) and the slave (I2C1, the CPU's
      link at 0x69) (2026-11-24)
- ◐ The window, as FlipperZeroEmu's (Kara, 2026-11-23): a Flipper One of this project's
      own drawing around the emulated screen, its 13 buttons and the touchpad to click,
      the keyboard, its 17 LEDs where the device has them; a front-panel link for it and
      any program (docs/PANEL_LINK.md). Drawn and connected, and the screen and its
      backlight are live (2026-11-23); drawn again part by part from measurements of the
      device's renders (scripts/fo_body.py, 2026-11-24); its buttons and touchpad work
      (2026-11-24); its LEDs light as the firmware drives them (2026-11-24)
- ☑ A launcher: run, the window, stop, status, the firmware's log (RTT), its CLI, a
      picture of the screen, where the cores are; a folder per emulated Flipper One;
      gdb on it with the firmware's symbols, the MCU first and the RK3576's clusters
      as more inferiors (2026-11-25, log 83)

## M2: the MCU's board ☐
- ☑ The control bus (2026-11-24): PCAL6416 (the buttons), IQS7211E (the touchpad),
      DRV2605L (haptics: the window shakes while its motor runs), TPS62868x (the
      display's supply)
- ◐ The main bus (PIO I2C, decoded from GPIO22/23's levels, 2026-11-24): PCAL6416 (rails,
      `MASKROM_EN`; the firmware powers the Linux side and shows "CPU not responding",
      2026-11-24), BQ28Z620 (the gauge: a battery at 80 %, `battery=N`), BQ25792 (the
      charger, no USB input yet), INA219 (VSYS), FUSB302 and HD3SS3220 (USB-C, nothing
      plugged in) (2026-11-24): the self-check finds every chip; a charger to plug in
      (the window's Ctrl+U): the charger's flow, the battery charging over time, the
      USB-C chips' partner (2026-11-24); INA4230 ×5, the RK3576's rails and the control
      board's 3.3 V (`power consumption`, 2026-11-24)
- ☑ The backlight's PWM (2026-11-23); the WS2812 LEDs (17 on three chains, their
      supplies on the control expander, 2026-11-24); the headphone jack: its switch,
      and a headset's mic and buttons on the ADC (2026-11-24)
- ☑ The desktop and menus, the buttons and the touchpad from the window (2026-11-24)
- ☑ RPC: the screen stream and input, through the CLI's `rpc`: the screen streamed
      over the CLI's UART, frame for frame as it shows (scripts/fo_rpc.py, the smoke
      test; the firmware's FW-02) (2026-11-25); keys through it, at once, over the UART
      and over USB (the smoke test presses them); the UART's line no longer waits for
      its reader (QEMU patch 0016); the firmware's FW-03 to FW-06 (2026-11-26, log 84)
- ☑ USB: the RP2350's controller as a device, and a computer at the cable's end that
      enumerates it: the firmware's CDC port (`37C1:F101`) on `devices/NAME/usb.sock`
      (2026-11-24); BOOTSEL: the boot ROM's own USB boot enumerates (2026-11-24); USB/IP,
      as FlipperZeroEmu's USB is: the device on this PC's own kernel (2026-11-24); an
      update as picotool makes one: `power boot` or the button into the boot ROM's USB
      boot, PICOBOOT (`scripts/fo_picoboot.py`: read, erase, write, a UF2 loaded, a
      reboot through the watchdog) (2026-11-24)
- ☑ The charger's power switch (SDRV_CTRL): `power reboot` (its system power reset),
      ship mode and shutdown (`power ship`, `power off`, the desktop's Power Off: the
      board off until a charger or a computer is plugged in) (2026-11-24); QON (ship
      mode's own wake) waits for where the board has it
- ☑ Checks: `scripts/smoke_test.py` (2026-11-24): a throwaway device through its
      sockets: the boot and each chip, the host link and a frame shown pixel for
      pixel, the LEDs and the motor, the CLI and the firmware's unit tests, the
      buttons and the touchpad, the headset, the Flipper OS menu and its Stop, a
      charger, a computer (the CLI over USB and USB/IP), an update through PICOBOOT
      and a reboot into it, `power reboot` and ship mode, BOOTSEL

## M3: the RK3576 to a shell ◐
- ☑ The cluster: 4× Cortex-A53 and 4× Cortex-A72, GIC-400, the generic timer at 24 MHz,
      PSCI (QEMU's, over SMC) (2026-11-24)
- ◐ CRU (its registers, the PLLs locked, the clocks the timer counts at), the PMU's
      power domains, the TRNG, the Rockchip timer (the broadcast clockevent), the
      DesignWare UARTs (UART0 the console, UART4 the MCU's line) (2026-11-24); GRFs,
      pinctrl and the GPIO banks as unimplemented registers so far; the GMACs' 125 MHz
      sources at CPLL / 8, the DDR blob's word LPDDR4X (Linux's DFI), PMU0_GRF's boot
      mode (`reboot` into the MaskROM) (2026-11-26, log 98); the CRU's other reset
      values (a divider at 0 leaves a clock at its PLL's rate: refclko25m_gmac0_out at
      1188 MHz)
- ☑ The published kernel and the Flipper One device tree, started directly: Flipper
      OS's initramfs, to its shell (`scripts/fetch_os.py`, `run --linux`, `console`)
      (2026-11-24)
- ☑ The rk3x I2C (2026-11-24): the MCU's link (i2c0), the RK806 PMIC (i2c1, whose
      regulators most of the SoC waits for), the main board's chips (i2c2, the MCU's
      main bus: HD3SS3220, INA219, INA4230 ×5, the expander, the charger and gauge),
      the HYM8563 RTC (i2c8); the GPIO banks (v2, interrupts); the main bus's
      two masters take turns, as on a busy bus (2026-11-24); the MCU's link at about
      its real speed: a step the MCU held SCL on goes on when its slave lets go, not
      polled (2026-11-26, log 89)
- ☑ SCMI (TF-A's clocks): the kernel's memory after TF-A's 2 MiB, SMCCC 1.1, the base and
      clock protocols: cpufreq on both clusters (2026-11-24)
- ☑ Temperatures: the TSADC's six channels, warmed by the cores that run, its alarm and
      TSHUT, and the OTP it takes its trims from (the CPU's code and ID too); what the
      cores draw follows their clocks and the OPP tables' voltages, so cpufreq's
      cooling cools; the temperature around the device from the panel link and the
      window, the battery's too (2026-11-24)
- ◐ Storage: microSD (dw-mshc, QEMU's SD card) with the published image: Flipper OS
      boots to its login (2026-11-24), the card's data a block at a time (patch
      0014: 48 MB/s, as the UFS) (2026-11-25); the 64 GB UFS, blank (QEMU's UFS host on a PCI
      bus the kernel does not see, patches 0008-0010) (2026-11-24); U-Boot from the image
      (`run --uboot`): U-Boot proper from the card's u-boot.itb, the machine in SPL's,
      TF-A's and the DDR blob's place; its boot menu on the Flipper's screen, the keys,
      a BLS entry booted (patch 0015 for its UFS scan) (2026-11-25); the Flipper OS
      installer (`run --installer`: its Falcon FIT, the machine as SPL), to its
      interface on the screen, the update server's builds and the UFS as its
      target (2026-11-26, log 87); the UFS provisioned as its scheme has it (patch
      0017: boot LUs A and B, bBootLunEn kept), an install, and Flipper OS booted
      from the UFS, the machine as the boot ROM (UFS, then the card) (2026-11-26,
      log 88); the MCU's update within the installer's 180 s: 55 s, the I2C
      link at about its real speed (2026-11-26, log 89); a UFS reprovisioned (LUs
      carved by a written Configuration Descriptor)
- ◐ The MCU powers it: `VCC5V0_SYS_S5_EN` (the main expander's pin 12) switches it on
      and off, and Linux's PSCI SYSTEM_OFF and SYSTEM_RESET are its own (QEMU patch
      0005) (2026-11-24); `MASKROM_EN` (BOOT0): the RK3576 waits in its MaskROM when
      the rail comes up with it, the MCU's Maskrom does that; the main expander's
      enables pulled down, the machine started as a device in use (2026-11-26, log
      85); the SARADC: the boot strap (BOOT0 grounds it), BOOT1 (the MCU's PWM) and
      the board's revision, which U-Boot reads (2026-11-26, log 86); the boot ROM's
      USB download: 2207:350e, the maskrom protocol, rockusb's download-boot of the
      Flipper OS installer's loader (the machine as SPL in Falcon mode), over
      USB/IP (scripts/fo_rockusb.py) (2026-11-26, log 90); U-Boot's rockusb: Rockusb's
      Loader mode on USB0, the card read and written over USB/IP (2026-11-26, log
      95); a usbplug's Loader mode, the machine's (rkbin's loader: rkdeveloptool's
      `db`, then `wl`): the UFS's LU 0 or the card behind it (2026-11-26, log 97);
      U-Boot over USB (SPL with u-boot.itb)

## M4: the link ◐
- ◐ A program in the RK3576's place on the link: the host link (docs/HOST_LINK.md,
      `devices/NAME/host.sock`): the MCU's registers at 0x69 and CPU_INT (2026-11-24);
      the screen's frames over the display SPI, edge by edge to the MCU's PIO
      (2026-11-24), as a wave of edges (2026-11-25)
- ☑ i2c0 (rk3x) with the MCU's register slave at 0x69 and its interrupt line (GPIO2_A6):
      Flipper OS's `flipper-one-mcu` driver and the MCU firmware talk (2026-11-24)
- ☑ spi0 with the MCU's frame sampler: FlipCTL on the emulated screen, the Flipper's
      keys reaching it through the MCU (2026-11-24)
- ◐ The CPU's state, power off, reboot: Linux's states reach the MCU's Host service,
      which cuts the rail after a power-off; its CPU app powers Linux on again
      (2026-11-24); the MCU's firmware updated from Flipper OS over I2C, staged in its
      other slot and booted by the boot ROM's flash-update boot, bought (2026-11-25); the
      console UART between the two

## M5: Flipper OS's hardware ☐
- ☑ Ethernet (GMAC, dwmac 4.20a, RTL8211F): both ports and their PHYs; the first on the
      host's network through passt (`run --net`), SSH from the host (`run --ssh PORT`)
      (2026-11-24); the second on a LAN of its own (`run --lan`: a socket that a VM or
      scripts/lan_host.py joins, the link up while one is there) (2026-11-25)
- ◐ USB: both DWC3 cores (dual-role; QEMU's xHCI for the host role, patch 0011), the
      USBDP PHY, the Type-C port, the charger and battery in Linux; devices plugged in
      from the monitor (2026-11-25); USB0's device role, high speed: a computer on the
      USB-C port (the virtual PC, through USB20_SEL) enumerates Flipper OS's gadget,
      its serial port on usb.sock, all of it over USB/IP (2026-11-25); U-Boot's
      gadget (its rockusb) too, through its own DWC3 driver: XferNotReady on a bulk
      endpoint that asks for it, a transfer ended at its last TRB (2026-11-26, log
      95); the MT7921's Bluetooth (mt7921.c): on USB1 while its rfkill's GPIO
      powers it, its real firmware through MediaTek's WMT, BlueZ's controller
      (2026-11-26, log 99); its Wi-Fi; the board's hubs
- ◐ Audio: the SAIs (0, 2, 6) and the NAU8822: Flipper OS's sound card plays, out of
      QEMU's audio (`run --audio`) (2026-11-25); the internal and the headset's
      microphones, from a WAV file (`run --mic`) (2026-11-25); this PC's own
      sound (QEMU with a PipeWire backend)
- ◐ PCIe0 and the M.2 slot, HDMI: QEMU's DesignWare host with Rockchip's registers, an
      NVMe in the slot (`run --nvme`) (2026-11-25); HDMI: the VOP2, its IOMMU, the
      transmitter and a monitor (`run --hdmi`, `shot --hdmi`), Flipper OS's console
      on it (2026-11-25); DP over the USB-C port, once the MCU firmware's USB-PD
      negotiates its alternate mode (its PD service is a stub so far) (the RTC: M3; the
      main bus's chips: M2)
- ☑ Speed while FlipCTL draws: the display link's bursts go to the MCU's PIO as a
      wave, paced so that the CPUs get the BQL between them, and Linux's SPI DMA
      no longer runs whole inside its register write (QEMU patch 0012): the sound
      at speed; an exception return takes the BQL only while the PMU counts
      (patch 0013) (2026-11-25); idle measured (158 % of a host core with eight
      vCPUs, the MCU alone 15 %), the GIC's distributor writes one update each
      (patch 0018: 151 %) (2026-11-26, log 91); no BQL for an interrupt the CPU
      cannot take (patch 0019: idle 122 %, linux_check 494 s from 553)
      (2026-11-26, log 92); interrupts without an exit request, the TLB's dirty
      reset in the mmu_idx in use, the GIC's update in one pass (patches
      0020-0022: idle 67-82 %, linux_check 421 s) (2026-11-26, log 93); the
      other CPUs' TLB flushes under the BQL, MMIO's; KPTI (KASLR forces it under
      U-Boot, as on the device): QEMU flushes its whole TLB at every switch of
      ASID, a boot half again as long (log 96): an ASID-aware TLB
- ☐ Wi-Fi (the MT7921 on USB: its Bluetooth is there, log 99; its interface 3 and
      mt7921u's MCU protocol to come)

## Not planned
- The RK3576's mask ROM and DDR training blob (closed), SPL and TF-A's BL31: the machine
  starts at U-Boot proper, in their place, or at the kernel
- The NPU, the GPU (Panfrost falls back to software rendering), the video codecs
- Cycle accuracy
