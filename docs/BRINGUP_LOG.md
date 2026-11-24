# Bring-up log

What was found on the way, in order, with how and what was done. Read these before
modelling anything new: most traps repeat. Numbers go on; an entry is not rewritten.

Tools: `qemu-system-aarch64 -d unimp,guest_errors -D FILE` (every access to a block
that is not modelled yet, every coprocessor instruction the model does not know), the
boot ROM's ELF with symbols (`firmware/bootrom/A4/arm-bootrom.elf`, `objdump -d`) and
its source (raspberrypi/pico-bootrom-rp2350, in `third_party/`), the Pico SDK 2.3.1's
register headers.

## The RP2350 and its boot ROM (2026-11-23)

1. **Coprocessors from the first instructions.** The boot ROM's reset handler reads
   SIO's CPUID, gives itself coprocessor 7 in CPACR and NSACR, and checks every step
   with it: CP7 is the RP2350's redundancy coprocessor (RCP: salted canaries, checked
   booleans, a sequence counter, a panic). The ROM has about 540 such instructions;
   the MCU firmware uses CP0 (GPIO, 45) and CP4 (double-precision arithmetic, 42; its
   PendSV saves CP4's state). QEMU takes every coprocessor instruction but CP10/11 for
   a NOCP UsageFault, and CPACR keeps only CP10/11's bits. QEMU patch 0001: a machine
   gives a CPU a function per coprocessor 0..7 (`arm_m_set_coproc()`); the RCP is
   modelled from the SDK's `hardware/rcp.h` (`rp2350_coproc.c`), CP0 and CP4 log.

2. **TT must say IDAU region 2.** After setting SAU region 7 the ROM runs `tt` on
   0x7fe1 and wants 0x02ce0700: Secure, SAU region 7, IDAU region 2 valid. The
   RP2350 has a fixed IDAU; the ROM's `arm8_bootrom_rt0.S` gives its map of the ROM:
   0x0000-0x42ff Exempt, 0x4300-0x7dff Secure for fetches and Exempt for data,
   0x7e00-0x7fff Secure and NonSecure-callable (the SG entry points). QEMU's IDAU
   interface does not see the access type, so 0x4300-0x7dff is Exempt for both. The
   USB controller's RAM is Exempt too (the ROM's nsboot), and the PPB; the rest is
   NonSecure to the IDAU, for the SAU to decide.

3. **MPU registers read back as the Cortex-M33 keeps them.** The ROM writes MPU_RNR
   with 0x02ce0700 for region 0 and 0xf0-0xf3 for regions 0-3 ("RNR is only 3
   bits"), and MPU_RLAR with bit 4 set, then reads all back. QEMU ignored a region
   number past the last and kept RLAR's bit 4 (RES0 before v8.1M), and gave the M33 16
   regions where the RP2350 has 8 in each security state. QEMU patch 0002; the SoC
   sets 8.

4. **The HardFault handler begins with BKPT.** With no debugger that escalates inside
   HardFault: a Lockup, and QEMU ended with "Lockup: can't escalate 3 to HardFault".
   QEMU patch 0003 is FlipperZeroEmu's 0015: the core stops, the machine runs on.

5. **The aliases.** The ROM pulses three blocks' resets through RESETS' set and clear
   aliases (+0x2000, +0x3000) and checks RESET_DONE with the RCP. Every peripheral's
   registers are a block of 4 KiB followed by its XOR, set and clear aliases, which the
   peripheral applies itself; `rp2_block_init()` gives a model that, and narrow writes
   copied across the word as the APB bridge does. RESET_DONE is ~RESET at once.

6. **Salts from TRNG and SHA-256.** The ROM hashes the TRNG's entropy registers with
   the SHA-256 accelerator for the RCP's salts. The TRNG model always has fresh words
   (QEMU's guest random source, `-seed` repeats them); SHA-256 is computed for real, a
   block at a time as the words come, with the accelerator's byte swap.

7. **The USB controller's RAM is the ROM's working memory.** As an unimplemented
   device it lost what the ROM put there; it is RAM now (4 KiB at 0x50100000).

8. **BOOTSEL is QSPI's CS read low.** The ROM reads the flash's CS line through SIO's
   GPIO_HI_IN: low is the BOOTSEL button, and the ROM went to its USB boot and waited
   for the crystal oscillator. SIO's model reads CS (and the data lines) high, the
   flash's pull-ups; `-M flipper-one,bootsel=on` holds CS low. With XOSC, the PLLs,
   CLOCKS and PSM answering, the ROM takes the flash path: it sets up the QSPI pads and
   QMI, and talks to the flash in QMI's direct mode (next).

9. **QMI's direct mode queues before it is enabled.** The ROM writes a command into
   DIRECT_TX and only then sets EN and ASSERT_CS0N ("Anything queued in TX FIFO will
   start now"); a model that dropped TX while disabled lost the write enable. TX is a
   FIFO that goes out once EN is set, with CS asserted or not; AUTO_CS0N holds CS for
   as long as the host keeps writing (until it next reads the CSR or RX). The flash
   behind it answers byte by byte as a W25Q128JV, its dummy phases counted in clocks
   (8 / lanes a byte), and writes programs and erases back to the image file. XIP is
   CS0's four 4 MiB windows through ATRANS0-3 (BASE, SIZE in 4 KiB sectors): the ROM
   maps the partition it boots at 0x10000000 that way.

10. **SHA-256's SUM registers are not swapped.** The ROM checks the partition table's
    hash (HASH_DEF: its block's first 25 words) and, with a wrong digest, took the
    table for unverified and went to its USB boot. Found with gdb on the ROM's ELF,
    breakpoints on its boot path (`s_varm_crit_ram_trash_perform_flash_scan_and_maybe_run_image`
    and what it calls). The model's SHA-256 was right (against `sha256sum`), but it
    applied CSR.BSWAP to the SUM registers too; the ROM reverses them itself (`rev`
    after reading), as the SDK's `sha256_get_result()` does. BSWAP is for what is
    written. The ROM also writes WDATA a byte at a time (`strb`, the padding) and ends
    with words: bytes are packed into a word in the order they come.

11. **The MCU firmware runs.** The SDK's runtime takes boot lock 7: BOOTRAM's
    BOOTLOCKn are claimed by a read (non-zero if it was free) and let go by a write,
    as SIO's spinlocks; BOOTLOCK_STAT has 1 for unclaimed. TIMER0/1 count the virtual
    clock's microseconds, with alarms and their interrupts; the GPIO coprocessor
    reaches SIO's OUT and OE (the SDK's `hardware/gpio_coproc.h` gives every
    encoding).

12. **Core 1.** `main()` calls `multicore_reset_core1()`: PSM FRCE_OFF.PROC1 set and
    cleared, then a wait for core 1's ROM to push 0 through the SIO FIFO. The SoC has
    two Cortex-M33s now, both started in the ROM; core 1 waits for its salt and a
    launch. QEMU needs core 1's bus as an alias (a region has one container) and the
    machine's CPU count at 2 (TCG makes as many thread contexts); PSM powers core 1
    with `arm_set_cpu_off()` and `arm_set_cpu_on_and_reset()`. A peripheral's
    interrupt goes to both NVICs.

13. **Its log, before its UART works.** The firmware logs to SEGGER RTT too, a ring in
    its RAM: `scripts/rtt.py MONITOR.sock FIRMWARE.elf` finds `_SEGGER_RTT` and reads
    it with the monitor's `xp`. FreeRTOS starts ("Logs initialized", "NVM partition:
    0x00f02000 1004k", the key-value store, "FuriHalOs Init OK"; the DCP's state is
    saved at every switch of thread), then `[CRASH][SerialControlDriver] furi_check
    failed`: `uart_pio_init()` gets no PIO state machine for GPIO41, the log UART,
    since PIO's GPIOBASE reads 0. PIO is next.

## The MCU firmware's start (2026-11-23)

14. **PIO.** The firmware's log UART is a PIO program on GPIO41 (`uart_pio_init()`), and
    the SDK claims a state machine with the pin range it needs: with GPIOBASE (0 or
    16 on the RP2350B, which PIO pins 0-31 start at) reading 0, the claim failed and
    the firmware crashed. `rp2350_pio.c` interprets the PIO's instructions, the
    RP2350's too, each state machine on the virtual clock in picoseconds from its
    clock divider: a block runs its machines up to the present when its timer comes,
    and whenever the CPU touches it or a pin changes; a stalled machine costs nothing
    until then. Forced instructions (SMn_INSTR) run even with the machine disabled:
    the SDK sets a program's start and its pins' directions that way.

15. **The pins.** A pin is driven by the function its GPIOn_CTRL selects (SIO, a PIO
    block), through the overrides and its pad (OD and ISO let go); else by the board,
    else by its pull. Every change goes to the pin's listeners with the virtual time
    of the instruction that made it, and to IO_BANK0's interrupts (INTRn's edges and
    levels, PROCn_INTE/INTF/INTS, IO_IRQ_BANK0 to each core). SIO's GPIO_IN reads the
    pins. A UART decoder on GPIO41 takes the bits from those times: `-M
    flipper-one,log=CHARDEV`; the launcher writes `devices/NAME/mcu.log`. The first
    line through the emulated PIO: "Hardware log output initialized".

16. **Then the ADC.** `./flipperoneemu where` (each core's PC and function, from the
    firmware's and the boot ROM's ELF) finds core 0 in the SDK's `adc_init()`, which
    waits for the ADC's READY.

17. **The ADC.** READY follows EN, and a START_ONCE conversion is done at once: RESULT
    is the board's voltage on that input, from the machine's table (3.3 V on GPIO40-47,
    0.706 V, 27 °C, on the temperature sensor).

18. **Microseconds from the cycle counter.** `furi_delay_us()` counts the DWT's CYCCNT,
    which QEMU's Cortex-M33 does not have. `rp2350_ppb.c` overlays each core's system
    space with the DWT (CYCCNT counts the system clock from the virtual clock while
    DEMCR.TRCENA and CYCCNTENA are set; four comparators, kept), ACTLR, DEMCR and the
    RP2350's EPPB.

19. **The UARTs.** `furi_hal_serial_init()` drains the UART's FIFO and waits for BUSY to
    clear. UART0 and UART1 are QEMU's PL011, which is what the RP2350 has, behind a
    wrapper that gives them the RP2350's XOR, set and clear aliases. UART1 is the CLI's:
    QEMU's first serial port, the launcher's `devices/NAME/cli.sock`, and
    `./flipperoneemu cli [COMMAND]`.

20. **The end of its start.** The firmware's start then runs to its end: the self-check
    starts, and "Startup complete" at 4.4 s. Every chip on its I2C buses is missing
    (nothing is behind the DesignWare I2C, or the main bus's PIO I2C, yet), so each
    service logs that its chip was not found, and the firmware goes on. The CLI answers
    once its service is up: `help`, `device_info` (firmware `dev-23112026-87f6cca3`),
    `uptime`, `free` (203 KiB of 475 KiB free).

21. **The screen.** The JD9853 is wired to HSTX: IO0-3 on GPIO19, 16, 15 and 14, SCK on
    GPIO18, CS on GPIO17, reset on GPIO12, TE on GPIO5. `rp2350_hstx.c` shifts each word
    written to the FIFO out at once, N_SHIFTS cycles of the eight pins' BITn selections.
    `jd9853.c` reads those cycles as QSPI transactions: the instruction (02h, or 32h
    for data on four lines), three address bytes with the command in the middle, then
    the data. RAMWR's bytes fill the panel's 258 × 144 window a byte a pixel, and the
    leftmost 256 of each row are what the glass shows. A frame is a DMA transfer
    (`rp2350_dma.c`: 16 channels, DREQ pacing, the trigger aliases, chaining, IRQs
    DMA_IRQ_0-3) that the firmware starts on TE's rising edge (80 Hz while TEON). QEMU
    first refused the DMA's writes into the HSTX FIFO ("Blocked re-entrant IO"): all
    of the SoC's blocks are one QEMU device, and a transfer started by a write to one
    block reached another. The blocks now let that happen, as the chip's bus does. The
    self-check screen is in the window: "CPU: Dual Cortex-M33 @ 150MHz", "Memory free:
    203K", each chip "NOT FOUND", and its duck.

22. **The backlight.** GPIO6's PWM (`rp2350_pwm.c`: a pin's duty from its slice's TOP,
    CC and polarity) goes to the window. The firmware's default is 10 % (26 of 255),
    for 10 s after the last input (`LED_BACKLIGHT_TIME_DEFAULT`), then off. The window
    draws it by lightness (CIE L\*): at 10 % duty an LED looks about 38 % as bright as at
    full. The buttons come through the control bus's PCAL6416, so after those 10 s the
    screen stays dark until the I2C models are in.

## The buttons (2026-11-24)

23. **The control bus.** The touchpad, the haptics, the display's supply and the
    buttons' expander are on I2C0 (GPIO20/21, 400 kHz), the RP2350's Synopsys
    DW_apb_i2c, which the firmware drives through the Pico SDK's polling calls: a
    command to DATA_CMD (RESTART and STOP flags with each byte), TX_EMPTY waited for,
    TX_ABRT_SOURCE read, STOP_DET waited for. `rp2350_i2c.c` does each command at
    once on a QEMU I2C bus, so TX_EMPTY holds while enabled; an address or byte that
    nothing ACKs aborts as the controller does (TX_ABRT, its source, a STOP, the
    commands after it flushed until CLR_TX_ABRT is read). The SDK disables the
    controller, sets TAR and enables it again between a register's address (written
    without a STOP) and the read after it: the transfer stays open, and the read's
    RESTART is a repeated start. With chips that answer or NACK at once, the
    firmware's start now takes 0.5 s instead of 4.4 s of timeouts.

24. **The buttons.** A PCAL6416A at 0x20 on that bus (`pcal6416.c`): its registers in
    pairs, a port each, the command byte toggling between them; the input port
    through the polarity inversion; an unmasked input that changes pulls INT low and
    sets its interrupt status; a latched input keeps the level it changed to until
    the port is read, which lets INT go. The 13 buttons are on its pins, pressed low,
    in the firmware's `InputKey` order (the panel's key order was chosen to match);
    INT is GPIO46, where the input service waits for a falling edge. The self-check
    now says "Expander control: ok", and the window's buttons reach the firmware: OK
    leaves the self-check for the desktop, the Settings soft key opens its menu, the
    pad moves through it, Back returns, and a key wakes the backlight.

## The touchpad (2026-11-24)

25. **The touchpad.** An Azoteq IQS7211E at 0x56 on the control bus (`iqs7211e.c`),
    its RDY on GPIO9. Its registers are 16-bit words, little-endian, the address going
    up a word every two bytes. The chip senses a cycle at a time and then opens a
    communication window, RDY low until a STOP (or its I2C timeout); in stream mode
    every cycle, in event mode only when the finger came, went or moved. The firmware's
    driver takes one step of its start per window (the product number 0x0458, Show
    Reset, its settings, Ack Reset, a re-ATI, the data, event mode) and from then reads
    the data block, GESTURES to FINGER_2_AREA, in each: a finger seen, it publishes a
    touch's Start, Move and End. The window's touchpad (0 … 1000 across and down) is
    scaled to the chip's X and Y resolution (1000 × 800 as the firmware sets it),
    flipped as its settings ask (the f2 board's are), and a missing finger reads
    0xFFFF. The self-check says "Touchpad: ok"; `input dump` on the CLI shows the
    window's drags; the Touchpad Test app (`desktop start_app touchpad_test`) draws
    them; a touch wakes the backlight. RDY and the expander's INT are open drain with
    the board's pull-ups, which the RP2350's reset does not undo (it forgets what the
    board drives, and the chips' resets come before it).
