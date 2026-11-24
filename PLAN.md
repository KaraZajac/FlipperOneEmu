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
- ◐ Cortex-M33 core 0 (Secure, FPU, DSP, 8 MPU and 8 SAU regions) with the RP2350's
      IDAU; core 1 as far as the boot ROM's launch handshake through the SIO FIFO
- ◐ The coprocessors: GPIO (CP0), double-precision (CP4), redundancy (CP7, the boot
      ROM's). QEMU patch 0001 lets a machine have them; the RCP is modelled
- ☐ Boot: OTP, QMI and the XIP translation of partitions, the boot ROM's flash
      functions, watchdog and POWMAN scratch, try-before-you-buy images
- ◐ Clocks, resets, PSM, XOSC, PLLs, ROSC, TRNG, SHA-256, POWMAN, watchdog, SIO
      (GPIO, FIFOs, spinlocks, doorbells) answer as the boot ROM needs; TICKS,
      TIMER0/1, IO_BANK0, PADS to come
- ☐ PIO (three blocks, the RP2350's instructions), DMA, HSTX
- ☐ The JD9853 panel and its tearing signal: the screen to a window and to PNG
- ☐ DesignWare I2C (master and slave); the PL011 UARTs (the CLI on UART1)
- ☐ A launcher: run, stop, status, screen, CLI, gdb; a folder per emulated Flipper One

## M2: the MCU's board ☐
- ☐ The control bus: PCAL6416 (buttons), IQS7211E (touchpad), DRV2605L (haptics),
      TPS62868x (the display's supply)
- ☐ The main bus (PIO I2C): PCAL6416 (rails, `MASKROM_EN`), BQ2579x, BQ28Z620, INA219,
      INA4230 ×5, FUSB302, HD3SS3220
- ☐ The WS2812 LEDs (17 on three chains), the backlight's PWM, the headset key (ADC)
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
