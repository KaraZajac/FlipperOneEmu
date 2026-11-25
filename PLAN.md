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
      partition A (QMI direct mode, the flash, ATRANS, SHA-256); OTP reads as blank,
      try-before-you-buy images and the watchdog's reboots to come
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
- ◐ A launcher: run, the window, stop, status, the firmware's log (RTT), its CLI, a
      picture of the screen, where the cores are; a folder per emulated Flipper One.
      gdb's symbols to come

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
- ☐ RPC: the screen stream and input, through the CLI's `rpc`
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
      pinctrl and the GPIO banks as unimplemented registers so far
- ☑ The published kernel and the Flipper One device tree, started directly: Flipper
      OS's initramfs, to its shell (`scripts/fetch_os.py`, `run --linux`, `console`)
      (2026-11-24)
- ☑ The rk3x I2C (2026-11-24): the MCU's link (i2c0), the RK806 PMIC (i2c1, whose
      regulators most of the SoC waits for), the main board's chips (i2c2, the MCU's
      main bus: HD3SS3220, INA219, INA4230 ×5, the expander, the charger and gauge),
      the HYM8563 RTC (i2c8); the GPIO banks (v2, interrupts); the main bus's
      two masters take turns, as on a busy bus (2026-11-24)
- ☑ SCMI (TF-A's clocks): the kernel's memory after TF-A's 2 MiB, SMCCC 1.1, the base and
      clock protocols: cpufreq on both clusters (2026-11-24)
- ☑ Temperatures: the TSADC's six channels, warmed by the cores that run, its alarm and
      TSHUT, and the OTP it takes its trims from (the CPU's code and ID too); what the
      cores draw follows their clocks and the OPP tables' voltages, so cpufreq's
      cooling cools; the temperature around the device from the panel link and the
      window, the battery's too (2026-11-24)
- ◐ Storage: microSD (dw-mshc, QEMU's SD card) with the published image: Flipper OS
      boots to its login (2026-11-24); the 64 GB UFS, blank (QEMU's UFS host on a PCI
      bus the kernel does not see, patches 0008-0010) (2026-11-24); then Flipper OS on
      the UFS, and U-Boot from the image (SPL, TF-A BL31, U-Boot proper, a BLS entry)
- ◐ The MCU powers it: `VCC5V0_SYS_S5_EN` (the main expander's pin 12) switches it on
      and off, and Linux's PSCI SYSTEM_OFF and SYSTEM_RESET are its own (QEMU patch
      0005) (2026-11-24); `MASKROM_EN`; BOOT1's voltage on a SARADC input

## M4: the link ◐
- ◐ A program in the RK3576's place on the link: the host link (docs/HOST_LINK.md,
      `devices/NAME/host.sock`): the MCU's registers at 0x69 and CPU_INT (2026-11-24);
      the screen's frames over the display SPI, edge by edge to the MCU's PIO
      (2026-11-24)
- ☑ i2c0 (rk3x) with the MCU's register slave at 0x69 and its interrupt line (GPIO2_A6):
      Flipper OS's `flipper-one-mcu` driver and the MCU firmware talk (2026-11-24)
- ☑ spi0 with the MCU's frame sampler: FlipCTL on the emulated screen, the Flipper's
      keys reaching it through the MCU (2026-11-24)
- ◐ The CPU's state, power off, reboot: Linux's states reach the MCU's Host service,
      which cuts the rail after a power-off; its CPU app powers Linux on again
      (2026-11-24); the console UART between the two; the MCU update over I2C

## M5: Flipper OS's hardware ☐
- ◐ Ethernet (GMAC, dwmac 4.20a, RTL8211F): both ports and their PHYs; the first on the
      host's network through passt (`run --net`), SSH from the host (`run --ssh PORT`)
      (2026-11-24); a network for the second port
- ◐ USB: both DWC3 cores (dual-role; QEMU's xHCI for the host role, patch 0011), the
      USBDP PHY, the Type-C port, the charger and battery in Linux; devices plugged in
      from the monitor (2026-11-25); USB0's device role, high speed: a computer on the
      USB-C port (the virtual PC, through USB20_SEL) enumerates Flipper OS's gadget,
      its serial port on usb.sock, all of it over USB/IP (2026-11-25); the board's
      hubs and the MT7921
- ☐ M.2 and PCIe, audio (NAU8822), HDMI (the RTC: M3; the main bus's chips: M2)
- ☐ Wi-Fi (the MT7921 on USB)

## Not planned
- The RK3576's mask ROM and DDR training blob (closed): the machine starts at SPL or at
  the kernel
- The NPU, the GPU (Panfrost falls back to software rendering), the video codecs
- Cycle accuracy
