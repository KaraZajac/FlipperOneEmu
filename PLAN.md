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
- ◐ The coprocessors: GPIO (CP0), double-precision (CP4), redundancy (CP7, the boot
      ROM's). QEMU patch 0001 lets a machine have them; RCP and GPIO are modelled, the
      DCP logs
- ◐ Boot: the boot ROM verifies the partition table and starts the firmware from
      partition A (QMI direct mode, the flash, ATRANS, SHA-256); OTP reads as blank,
      try-before-you-buy images and the watchdog's reboots to come
- ◐ Clocks, resets, PSM, XOSC, PLLs, ROSC, TRNG, SHA-256, POWMAN, watchdog, SIO
      (GPIO, FIFOs, spinlocks, doorbells), BOOTRAM's locks, TIMER0/1, the ADC, DWT's
      cycle counter answer; PADS keeps what is written; TICKS to come
- ◐ PIO (three blocks, the RP2350's instructions; the firmware's log UART decoded
      from GPIO41 to devices/NAME/mcu.log, 2026-11-23); the pins and IO_BANK0's
      interrupts; DMA (16 channels) and HSTX (2026-11-23); PWM's duty (2026-11-23)
- ☑ The JD9853 panel and its tearing signal: the firmware's self-check screen in the
      window and to PNG (2026-11-23)
- ☑ The PL011 UARTs: the CLI on UART1 answers (`./flipperoneemu cli`, 2026-11-23)
- ☐ DesignWare I2C (master and slave)
- ◐ The window, as FlipperZeroEmu's (Kara, 2026-11-23): a Flipper One of this project's
      own drawing around the emulated screen, its 13 buttons and the touchpad to click,
      the keyboard, its 17 LEDs where the device has them; a front-panel link for it and
      any program (docs/PANEL_LINK.md). Drawn and connected, and the screen and its
      backlight are live (2026-11-23); drawn again part by part from measurements of the
      device's renders (scripts/fo_body.py, 2026-11-24); its LEDs and buttons come alive with the models
      behind them (the WS2812s, the control bus's expander, the IQS7211E)
- ◐ A launcher: run, the window, stop, status, the firmware's log (RTT), its CLI, a
      picture of the screen, where the cores are; a folder per emulated Flipper One.
      gdb's symbols to come

## M2: the MCU's board ☐
- ☐ The control bus: PCAL6416 (buttons), IQS7211E (touchpad), DRV2605L (haptics),
      TPS62868x (the display's supply)
- ☐ The main bus (PIO I2C): PCAL6416 (rails, `MASKROM_EN`), BQ2579x, BQ28Z620, INA219,
      INA4230 ×5, FUSB302, HD3SS3220
- ◐ The backlight's PWM (2026-11-23); the WS2812 LEDs (17 on three chains), the
      headset key (ADC)
- ☐ The desktop and menus; buttons and touch from the window and the launcher
- ☐ RPC: the screen stream and input, through the CLI's `rpc`
- ☐ USB: the CLI's CDC port (`37C1:F101`), and BOOTSEL: a UF2 copied onto the boot ROM's
      drive, over USB/IP as FlipperZeroEmu's USB is
- ☐ Checks: boot, CLI, screen, input, update

## M3: the RK3576 to a shell ☐
- ☐ The cluster: 4× Cortex-A53 and 4× Cortex-A72, GIC-400, the generic timer at 24 MHz,
      PSCI
- ☐ CRU, PMU and power domains, GRFs, pinctrl, GPIO banks, the Rockchip timer, the
      DesignWare UARTs (the console at 1,500,000 baud)
- ☐ The published kernel and the Flipper One device tree, started directly
- ☐ Storage: microSD (dw-mshc) or UFS with the published image; then U-Boot from the
      image (SPL, TF-A BL31, U-Boot proper, a BLS entry)
- ☐ The MCU powers it: `VCC5V0_SYS_S5_EN` switches the cluster on; `MASKROM_EN`; BOOT1's
      voltage on a SARADC input

## M4: the link ☐
- ☐ i2c0 (rk3x) with the MCU's register slave at 0x69 and its interrupt line (GPIO2_A6)
- ☐ spi0 with the MCU's frame sampler: FlipCTL on the emulated screen
- ☐ The console UART between the two; the CPU's state, power off, reboot; the MCU
      update over I2C

## M5: Flipper OS's hardware ☐
- ☐ Ethernet (GMAC, dwmac 4.20a, RTL8211F) to the host's network
- ☐ USB: the gadget on USB-C1 (NCM, MTP, ACM) over USB/IP; the host controller and its hub
- ☐ M.2 and PCIe, audio (NAU8822), HDMI, the RTC, the power chips on I2C2
- ☐ Wi-Fi (the MT7921 on USB)

## Not planned
- The RK3576's mask ROM and DDR training blob (closed): the machine starts at SPL or at
  the kernel
- The NPU, the GPU (Panfrost falls back to software rendering), the video codecs
- Cycle accuracy
