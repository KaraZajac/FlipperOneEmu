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

## The settings (2026-11-24)

26. **The flash's program and erase.** Every setting the firmware saved failed with the
    key-value store's "invalid data detected" (the LEDs' brightness and timeout at its
    start, the haptic driver's calibration), and the NVM partition (0xF02000, 1004 KiB)
    stayed erased. The firmware programs through the Pico SDK's `flash_range_program`,
    which calls the boot ROM's; the ROM queues a page program's command and address as
    two 16-bit QMI entries with the command in the low byte, and the RP2350 sends a
    16-bit entry's low byte first. The model sent the high byte first, so the flash got
    F0h and so on as commands, ignored them, and nothing was programmed or erased.
    Booting only reads, so it never showed. Low byte first, the store works: the settings
    are saved, `nvm list` lists them, and a restart finds them ("Loaded haptic
    calibration data from NVM").

## The haptics (2026-11-24)

27. **The rest of the control bus.** A DRV2605L at 0x5A (`drv2605l.c`), its EN on
    GPIO7: 8-bit registers as the datasheet starts them, the waveform sequencer
    (library effects and waits) from GO, real-time playback, and auto-calibration and
    diagnostics that end with GO cleared and DIAG_RESULT 0. The firmware's haptic
    service calibrates at its first start (GO polled every 10 ms) and saves the result
    (entry 26); the self-check plays a double click when a check fails. The motor runs
    while EN is high, the chip out of standby and playing; how long each library effect
    lasts is not published, so clicks are 60 ms, buzzes 400 ms, the alerts as named.
    The panel link says `haptic 1` and `haptic 0`, and the window shakes its drawing
    meanwhile, as FlipperZeroEmu's does for the Zero's vibration motor. The display's
    VCI supply, a TPS62868x at 0x47 (`tps62868x.c`), takes its voltage and forced PWM.
    The self-check now says "Haptic: ok"; every chip on the control bus answers.

## The main bus (2026-11-24)

28. **I2C from a PIO program.** The main board's chips are on GPIO22 (SDA) and GPIO23
    (SCL), driven by the Pico SDK's `pio_i2c` program: open drain, its pindirs pulling
    the lines low (OE inverted in IO_BANK0), the pad pull-ups letting them up, START and
    STOP made by instructions the CPU pushes for it to EXEC. `i2c_pins.c` reads every
    change of the two pins as the master leaves them (`rp2350_pin_own()`: the chip's own
    drive and the pulls, not what the board drives) and gives it to QEMU's bit-banged I2C
    decoder, which runs the transfers on a QEMU I2C bus; what the chip there puts on SDA
    the board drives on GPIO22. The decoder gives a read bit at SCL's rise; the program
    samples in the middle of the high time, so it reads it. On that bus, the main board's
    PCAL6416 at 0x20: INT on GPIO39, RESET on GPIO38 (the main board's reset, which the
    firmware pulses to cut the Linux side's power), both open drain with the board's
    pull-ups; the control bus's expander got its RESET too (GPIO43). With the expander
    there, the firmware does what the device does after power-on: the expander's outputs
    start high, so the Linux side's rails (VCC5V0_SYS_S5, USB2 select) are on, and the
    firmware takes the host as running, stops the self-check and shows the host's screen.
    No RK3576 answers, so after 5 s it says "CPU not responding". Power opens its
    Flipper OS menu; Stop pulses the main reset, the rails go off, the desktop comes.

29. **The battery's gauge.** A BQ28Z620 at 0x55 on the main bus (`bq28z620.c`): TI's
    standard commands, 16-bit words read from the command's address on, reporting a
    battery (`battery.c`) of assumed numbers: two lithium-ion cells (the firmware's
    charge limit is 8.65 V), 3000 mAh, 25 °C, its charge the machine's `battery`
    property (80 % unless told). It does not run down; its current is what the board
    draws, which follows the Linux side's rail (the main expander's pin 12: the
    PCAL6416 model now says which pins it drives high): 40 mA off, 400 mA on, both
    assumed. The status bar shows "80%". `power show` on the CLI lists the gauge's
    every value; its average power is in 10 mW steps, as the firmware's driver found.
    The CLI's floating-point numbers all come out as their last digit ("0.001V"), the
    gauge's or not: `printf` takes doubles, and the RP2350 does double arithmetic in its
    DCP coprocessor, which is not modelled yet.

30. **The double-precision coprocessor.** Every floating-point number the firmware
    printed came out as its last digit ("0.001V"): `printf` takes doubles, and on the
    RP2350 the Pico SDK does double arithmetic and the float/double conversions in the
    DCP (CP4), which the model only logged. The datasheet (3.6.2) outlines the DCP's
    instructions but not their intermediate formats, and software uses them only in the
    SDK's canned sequences: operands written unpacked, a few steps, the result read back;
    in a multiply, divide or square root the core does the mantissas' arithmetic itself
    and writes it back for the DCP to round, and no sequence branches on what it reads
    in the middle. So `rp2350_dcp.c` keeps the operands as written and works each result
    out at the read that ends its sequence, as IEEE 754 does with the DCP's three
    differences from it (denormals are zero in and out; ties to even; the default NaN),
    including the conversions to and from integers (truncated or rounded, saturated)
    and floats. The engaged flag the SDK checks before using the DCP (is an interrupted
    sequence there to save?) is N of PCMP; saving and restoring carries the operands.
    Two DCPs a core, as the chip has: CP4 the present security state's, CP5 the
    Non-secure one. `power show` now prints 7.960 V and 25.05 °C, and the firmware's
    unit tests pass (`unit_tests`: buffers, NVM, the UI's layout).

31. **The charger and the current meter.** A BQ25792 at 0x6B (`bq2579x.c`): its byte
    map, the 16-bit registers most significant byte first, the settings as the
    datasheet starts them for two cells (the firmware resets them, then sets 8.65 V,
    3 A in and out, the ADC on, ICO), the status, flags (cleared on reading) and ADC
    read-only. No USB input yet: VBUS absent, nothing charging; the ADC reads the
    battery's side, and its TS the NTC through the board's divider as the firmware
    works it back out (25.06 °C). An INA219 at 0x45 on VSYS with a 4 mΩ shunt
    (`ina219.c`): shunt and bus voltages, and the current and power registers from
    CALIBRATION as its datasheet defines them (400 mA, 7.96 V, 3.18 W). The self-check
    now finds every chip but the USB-C pair: "Current meter: ok", "Charger: ok".

32. **The USB-C port, empty.** The FUSB302B (power delivery PHY, 0x22) and HD3SS3220
    (port controller and mux, 0x47) on the main bus (`usb_c.c`), with no cable modelled:
    register files that start as their datasheets have them and keep what is written,
    the FUSB302's SW_RES, read-clear interrupts and non-advancing FIFO address, its
    status reading nothing seen and the FIFOs empty, so the dual-role toggling the
    firmware starts never finds a partner; the HD3SS3220 reporting nothing attached.
    Their interrupts reach the MCU through the main expander and stay quiet. The
    self-check's every line now says "ok", and the start's log has no errors left but
    the blank OTP's.

33. **A DMA that keeps a PIO's FIFO full.** Writing a PIO's TX FIFO ran its state
    machines up to the present first, which is right for the CPU but not for the DMA:
    emulating a state machine's pin changes costs more host time than they take on the
    chip (a WS2812 bit is a few of them), and virtual time follows host time, so each
    word the DMA pushed was shifted out before the next arrived, the FIFO ran dry, and
    the line went quiet for hundreds of microseconds between words. Now a DMA write to a
    TX FIFO only fills it, and the block looks at it next; and when a state machine
    takes a word from a FIFO it feeds (or puts one in a FIFO it drains), the DMA answers
    at once, inside the PIO's run, as its DREQ does on the chip.

## The LEDs (2026-11-24)

34. **The WS2812s.** Seventeen LEDs on three chains (`ws2812.c`), each driven by the
    Pico SDK's ws2812 PIO program at 800 kHz from a DMA transfer: GPIO29 the network
    lights (4), GPIO30 Power, the battery's outline (2) and watts (4), GPIO31 USB
    charging, its watts (4) and the battery's centre, the panel's order. Each chain's
    supply is an output of the control expander (pins 15, 14, 13), which the firmware
    turns on before it writes and off when a chain is dark. A high pulse longer than
    0.56 µs is a 1; each LED takes 24 bits, green, red, blue, and passes the rest on;
    the line low for 50 µs starts the chain again; an LED without power is dark. They
    first decoded as pairs of LEDs with long gaps between: entry 33. Now the CLI's
    `led TYPE COLOR` (numbers: `led 4 1` is Power, green) lights the window's LEDs, at
    the half brightness the firmware sends.

## The link (2026-11-24)

35. **The I2C slave, and a host in the RK3576's place.** The MCU serves the RK3576 on
    I2C1 in slave mode at 0x69 (`i2c_intercom`: 16-bit registers, the address high byte
    first, the values low byte first; CPU_INT on GPIO35). The DesignWare model gained
    its slave side: SAR answered, START_DET and RESTART_DET, the bytes written into the
    RX FIFO with the first of each transfer flagged in DATA_CMD's bit 11 (the firmware
    frames its transfers by it, not by START and STOP, which merge while its interrupts
    are masked), a read served from the bytes the CPU writes to DATA_CMD with RD_REQ
    raised once a byte and SCL held meanwhile, RX_DONE at the master's NACK, STOP_DET.
    Raising RD_REQ again while one was pending made the firmware give a byte too many
    (an "invalid access" in its log), as it would on a chip that did that. The host
    link (`host_link.c`, docs/HOST_LINK.md, `devices/NAME/host.sock`) is the master's
    side for a program, a transfer a line, done a byte at a time as at 1 MHz and
    waiting while SCL is held. Writing CPU state 3 (online) to 0x0040 turns the screen
    from "CPU not responding" to "Waiting for CPU screen…"; the LED colour registers
    light the window's LEDs, the haptic register buzzes, and reading the system
    interrupt register lets CPU_INT go.

## The screen's frames (2026-11-24)

36. **Which 256 columns the glass shows.** The panel's window is 258 pixels a row, and
    the glass shows 256 of them. The model had shown the first 256; it is the 256 from
    the second on. The firmware's history says so: from March its GUI left its first
    column empty ("workaround for the pixel shift bug", a padding of one pixel, on the
    right too from July), and in September (#247) that padding went and the frame
    receiver took it over, putting each frame from the RK3576 one pixel on, so that
    the frame's leftmost 256 columns are the ones that show, as the kernel's binding
    (`flipper,one-display.yaml`) says. The emulated screen moved one pixel left; a
    frame from the link now shows exactly.
37. **The display link.** Flipper OS sends its screen to the MCU over SPI, a whole
    frame at a time (the kernel's `flipper-one-display` driver): "active" (its GPIO0_D1,
    the MCU's GPIO25) low, 258 × 144 bytes of grey (0 black, 255 white) in SPI mode 3
    at 33 MHz on GPIO26 (SCK) and GPIO28 (MOSI), "active" high. The MCU firmware's CPU
    app samples it with a PIO program it assembles at run time (wait for "active", then
    a bit at each rising clock edge, eight to a byte), a DMA channel into one of two
    buffers, and an interrupt on "active" going high that takes the frame if it is
    whole. The host link carries frames now (`frame HH…`, `sent` when it went out;
    `host_spi.c`): each goes out on the pins edge by edge, about 600,000 changes, in
    bursts of 1024 bits that run ahead of the virtual clock. For that the PIO model
    learned two things. `rp2350_pins_drive_at()` changes pins at a given time, which
    may be ahead of the clock, running the state machines up to that moment with the
    levels as they were before making the change, so that a machine samples what it
    would have on the chip, whatever the host's time between bursts. And a machine's
    time never goes back: one that ran ahead with a burst looks again at its own time
    when the CPU comes; a pin's change wakes only the machines that wait on a pin, so
    the LEDs' and the log's machines (PIO2) are not run again at every edge; and the
    machines' timers wait while a burst runs them. A frame takes about 0.09 s of the
    host's time, ten a second, and comes out on the panel pixel for pixel. The MCU
    asks for a frame when it shows the CPU's screen again (bit 1 of the system
    interrupt register, 0x0104); a program answers it as Flipper OS does.

## A charger (2026-11-24)

38. **An expander's latched input, read: INT again.** The firmware latches the main
    board's expander's inputs (and the buttons'), so that a short pulse on one, such as
    the charger's 256 µs INT, is held until it reads the input port; then its worker
    looks at which inputs went from high to low since the last read. The PCAL6416
    model compared a pin's later changes with the pin as it was at the read, not with
    what the read returned: after a pulse had come and gone, the read gave the latched
    low, the pin was high again, and nothing more happened, so the firmware took the
    line for still asserted and every later pulse for no change. On the chip a change
    is from what was read: the pin back high is a change again, INT goes up at the read
    and down again a moment later, and the next read sees the pulse's end. The model
    now does that, the second look at the transfer's end, so that INT's fall is an edge
    for the MCU's GPIO interrupt. A quick press and release between two reads of the
    buttons' expander is now seen whole too.
39. **A charger in the USB-C port.** The panel link's `usb 1` (the window's Ctrl+U)
    plugs in a USB-C charger: 5 V, 3 A offered by its Rp, D+ and D- shorted (a DCP); the
    firmware's PD is not written yet, so it asks for no more. The BQ25792 sees VBUS
    (and VAC1, tied to it) at once, good 30 ms later, a DCP 300 ms after that; its
    status registers follow, each change sets its flag as the datasheet has them (on
    any change, or on the rising edge), and a flag its mask lets through pulses INT,
    through the main board's expander (pin 2) to the MCU. The power flow is the
    charger's: with input, the system runs from it (90 % efficient, assumed) and the
    battery takes what is left, up to the charge cycle's current (precharge, fast
    charge at ICHG, taper as VREG holds it back, done; again on falling VRECHG below);
    with too little input, the input current is the limit (IINDPM). From 5 V at 3 A,
    that is about 1.6 A into the battery with the Linux side off, 1.3 A with it on. The
    battery's charge now follows its current over the virtual clock's time, out or in
    (`battery=` is where it starts), and the gauge reports it: the charge rising, the
    current positive, DSG off. The FUSB302 sees VBUS and the Rp's level on CC1, its
    toggling ends as a sink, its INT_N to the expander's pin 3; the HD3SS3220 attaches
    as a sink. With the Linux side on, the firmware hands the main board's expander to
    the CPU and leaves its interrupts alone; with it off (Stop), it logs each of the
    charger's interrupts as they come: VBUS_PRESENT and AC1_PRESENT, PG, then IINDPM,
    BC1.2_DONE, VBUS and CHG, DPDM_DONE; and `power show` has "USB DCP (3.25A)" and
    "Fast charge (CC mode)".

## USB (2026-11-24)

40. **The MCU's USB, and a computer at the other end.** The RP2350's USB controller as a
    device (`rp2350_usb.c`): its registers (the write-clear ones, BUFF_STATUS,
    SIE_STATUS, EP_ABORT_DONE, take the written mask by whichever atomic alias it comes,
    as TinyUSB clears them through the clear alias) and its 4 KiB DPRAM: the SETUP
    packet, each endpoint's control and buffer control (two halves, the selector
    between them put back to buffer 0 by the SEL bit TinyUSB sets at each arming, taken
    at the endpoint's next transaction since the CPU writes the DPRAM as memory), EP0's
    buffer at 0x100. A computer (`usb_host.c`, the panel link's `usb 2`) plugs in: 5 V
    and 500 mA (the charger sees a standard port), and its data lines, which reach the
    MCU while the main board's USB mux (USB20_SEL, the expander's pin 8, which the
    firmware raises with the Linux side's rail) leaves them on it. When the device pulls
    D+ up, the computer resets the bus and enumerates it as a PC would, a transaction at
    a time on the virtual clock: the firmware's TinyUSB stack answers as
    `37c1:f101 "Flipper FZCO" "Flipper One MCU Debug …"`, and its CDC ACM function goes
    to `devices/NAME/usb.sock`, DTR up while a program is connected there: the CLI's
    session starts, with its banner (`./flipperoneemu cli --usb`).
41. **BOOTSEL: the boot ROM's own USB.** Started with BOOTSEL held (`run --bootsel`) and
    a computer plugged in, the boot ROM died at once (its `varm_dead`, a BKPT in a
    HardFault). Three things, one after another. Before its USB boot, the boot ROM sets
    each of the 64 OTP pages' software locks (OTP_SW_LOCKn) and reads them back under an
    RCP check; the OTP's registers were not modelled and read 0: now the locks keep what
    is written, ORed in. Then it chose UART boot, not USB: it looks at QSPI SD1 with
    BOOTSEL holding CS low, and the model had the data lines pulled up; undriven, as
    they are when the flash has no command, they read low. Then its USB boot, which runs
    NonSecure with SAU regions for RAM only, faulted on the USB controller's registers
    (AUVIOL): on the RP2350 the peripherals are Exempt to the IDAU and ACCESSCTRL says
    who reaches them, which the boot ROM had set for the USB controller; the model's
    IDAU now exempts the APB and AHB peripherals and SIO (ACCESSCTRL's filter is not
    modelled). Then the computer's enumeration got a STALL at a status stage: it had
    sent the next SETUP 44 µs after the last status packet, before the boot ROM's
    interrupt handler had taken that packet, and the SETUP reset the transfer the late
    completion then found missing ("received unexpected packet": halt). On the chip the
    handler takes a packet within a microsecond, long before a host's next; the
    emulated CPU on its own thread can lag the virtual bus, as FlipperZeroEmu's host
    found too. The host now sends nothing while the controller's interrupt is up (10 ms
    at most). The boot ROM enumerates: `2e8a:000f "Raspberry Pi" "RP2350 Boot"`.
42. **USB/IP: the MCU's USB on this PC.** `run --usbip PORT` serves the device over
    Linux's usbip protocol (the server is FlipperZeroEmu's, on this USB's
    transactions); `sudo scripts/usbip_attach.py --port PORT` hands it to the kernel's
    vhci-hcd, and `scripts/fo_usbip.py` speaks it from userspace. The computer at the
    cable's end keeps the bus (the reset, the address); each URB the PC's kernel sends
    becomes control or bulk transactions, and the built-in CDC bridge stands aside
    while the PC has the device. The firmware's CLI answers over it; with `--bootsel`
    it is the boot ROM's drive and PICOBOOT that the PC gets. Seen on the way, in the
    firmware: its USB CLI (cli_vcp.c) only clears its "transmitting" flag when a
    transmission completes, so a reset or a SET_CONFIGURATION while output is still on
    its way (a cable pulled out in the middle of a command's answer, then plugged in
    again) leaves it mute until the MCU restarts. The smoke test reads each answer to
    its prompt before going on.

## The headphone jack (2026-11-24)

43. **A headset.** The jack's switch is GPIO42 (high with a plug in: a pull-up of the
    board's, so that it holds through the MCU's resets) and the mic line GPIO47, the
    ADC's input 7. The firmware's headphones service looks every 100 ms: a plug,
    then, a second later, a mic, when the line is above 1.5 V; then a button, from the
    resistance it works out against a 2.2 kΩ bias. A 1.5 V mic was never a mic: the
    model now biases it at 2.2 V through 2.2 kΩ, with the mic as 6.8 kΩ (assumed):
    1.66 V. And the play button's 0 Ω was never a button: the firmware wants more than
    nothing (on a device the switch and the wires give it a few ohms): 10 Ω. Each
    button now comes as the firmware has them: play A, volume up B, down C, assistant
    D (`headset play|volup|voldown|assist` on the panel link; the window's Device menu).

## An update (2026-11-24)

44. **A firmware update, as picotool makes one.** `scripts/fo_picoboot.py` speaks
    PICOBOOT, the boot ROM's vendor interface, over USB/IP: a 32-byte command on the
    bulk OUT endpoint, its data, an empty packet back when it is done; `load FILE.uf2
    --reboot` erases each sector a UF2 touches, writes its pages and reboots, as
    picotool does. What it took:
    - Reads were refused ("invalid address"): the boot ROM checks a NonSecure
      caller's buffer in USB RAM against ACCESSCTRL (its USBCTRL register's NSP bit),
      which was not modelled. It is now: the registers, their password, CFGRESET.
      Its filter on bus accesses still is not.
    - BUFF_CPU_SHOULD_HANDLE read 0. The boot ROM's USB stack keeps a double-buffered
      endpoint's two buffers in step by it (which of the two a completion was), so a
      completion in the second buffer was taken for the first, and the stack armed an
      empty packet too many after an erase: the next read's data.
    - Then, now and then, a write got no answer. The controller's model updated a
      buffer control word with a read-modify-write of both halves; the boot ROM arms
      one buffer with a 16-bit store, and the emulated CPU runs on its own thread: a
      store between the model's read and write was undone, the buffer never armed,
      the host NAKed for ever. The model writes only the half a transaction took now
      (and takes RESET out of buffer 0's half with a compare-and-swap).
    - REBOOT2 arms the watchdog: its count (LOAD, in the 1 µs ticks the boot ROM and
      the SDK run it at) and TRIGGER now reset the chip, keeping what the chip keeps:
      the watchdog's scratch registers (where the next start's kind waits: BOOTSEL, a
      flash update), REASON, POWMAN's boot registers, and the PSM's registers. The
      boot ROM reads WDSEL after a watchdog reset: one that left the oscillators
      running is unsafe for its USB boot, and it makes one more first. With WDSEL
      forgotten, every reset was one of those: a reboot loop.
    - The cores were not reset at all. QEMU resets devices, not CPUs, unless a board
      asks (armv7m_load_kernel() does it for a board's one core): the chip's reset
      set every register back while the cores ran on where they were. Both start
      again in the boot ROM now.
    - Core 1's power (PSM FRCE_OFF.PROC1): QEMU's power-off and power-on each queue
      work for the core, and an on that comes before the off has run is dropped
      ("CPU 1 is already on"); the off then leaves the core off. The firmware's start
      calls multicore_reset_core1(), an off and an on a few instructions apart, then
      waits for core 1's handshake. One piece of work now does what the PSM asks when
      it runs.
    - `--bootsel` holds the button through the first start only: a reboot boots the
      flash, as a hand let go would have it.
    - In the USB/IP client (fo_usbip.py, from FlipperZeroEmu), the set of submissions
      whose replies carry data was the class's, shared by every client in a process,
      and each client numbers its submissions from 1: a second client read replies as
      if data followed and lost its place in the stream. Each has its own now, and
      fo_picoboot.py takes only the reply to what it sent.
    The firmware's `power boot` (the SDK's reset_usb_boot()) reaches the boot ROM's USB
    boot, and the smoke test makes an update: `power boot`, the flash read back, a
    spare sector erased, written and read back, the device's own UF2 loaded (2188
    pages in about 5 s) and read back, a reboot, and the firmware starting again. Not
    yet: the firmware's `power reboot`, which is the charger's (its system power reset),
    and its ship mode and power off.
45. **Two more of the USB controller's rules.** The smoke test's update failed now and
    then: a WRITE without an answer. Caught with the client waiting for ever and gdb
    on the boot ROM's endpoints (nsboot.elf has their symbols and types): both of the
    PICOBOOT OUT endpoint's buffers FULL, BUFF_STATUS clear, and the boot ROM sure it
    had given the controller a buffer. Its USB stack takes a second packet that comes
    while the first is still being written to the flash as queued, and asks
    BUFF_CPU_SHOULD_HANDLE which buffer each was. The chip keeps a second completion
    behind the first ("clearing the buffer status bit may instantly re set it", the
    datasheet says); the model had one bit for both, so the first was lost whenever
    two came before the CPU had cleared it. That happens when the host stops waiting
    for the controller's interrupt (10 ms), which the emulated CPU, on a busy host,
    sometimes takes. The model queues them now. And an endpoint in EP_ABORT NAKs every
    transaction until it is let go, as on the chip: the boot ROM takes back an armed
    buffer that way at each transfer's start. Eight loads of the firmware in one
    session (17,500 writes) went through, where one had failed before. (Having the
    host wait for the CPU to sleep before each transaction, tried first, only hid
    it.)

## The charger's power switch (2026-11-24)

46. **`power reboot`, ship mode, power off.** The firmware does not reset the MCU for
    `power reboot`: it asks the BQ25792 for a system power reset (SDRV_CTRL = 3 in
    REG 0x11, SDRV_DLY set: at once), which opens the ship FET for 350 ms, so that
    everything on VSYS starts again. Its `power ship`, `power off` and the desktop's
    Power Off are the same register's ship mode and shutdown (after waiting for USB to
    be unplugged: the charger refuses either while there is an input, as the
    datasheet says). The board now has a supply that the charger cuts: the chips as
    reset, the RP2350's cores held off through the reset, the screen, its backlight
    and the LEDs dark, no load on the battery, and `power off` on the panel link (the
    window says so). It comes back after the 350 ms, or when a charger or a computer
    is plugged in (shutdown's only way out). Ship mode also wakes on QON pulled low:
    where the Flipper One has QON is not known yet. A machine reset in QEMU resets
    the CPUs only if the board asks, so the cores could be held off right there.

## The RK3576 (2026-11-24)

47. **Flipper OS's kernel, to a shell.** A build's Flipper One image (build 2501: 6 GiB,
    GPT: `loader`, `metadata`, `root`) keeps its kernels in a btrfs root: a `boot`
    subvolume with the boot entries (BLS), and in each profile (snapshots `@Minimal`,
    `@Desktop`, ...) the kernel, initrd and device trees an entry names.
    `scripts/fetch_os.py` takes them out without a mount (`btrfs restore` reads the
    filesystem from a file) into `firmware/os/BUILD/`. The machine now has the RK3576
    beside the RP2350: four Cortex-A53s (MPIDR 0x0-0x3) and four A72s (0x100-0x103) in
    two clusters, EL2 and no EL3, PSCI from QEMU over SMC (as TF-A's BL31 gives it), the
    GIC-400, the generic timer at 24 MHz, 4 GiB of DRAM at 0x40000000, UART0 and UART4 as
    16550s. `run --linux` starts the kernel with the device tree of the board revision
    the MCU firmware is for; its cores stay off without one. What it took:
    - QEMU's `arm_load_kernel()` walks every CPU of the machine: it would have made the
      RP2350's M33s Linux CPUs and PSCI secondaries. Patch 0004 leaves M-profile CPUs
      alone. And PSCI finds a core by its MPIDR: the M33s had QEMU's default 0 and 1,
      which are A53s 0 and 1 here; they have ids no A-profile core has now.
    - The kernel stopped where the clocks come up: `time_init()` registers the CRU's
      PLLs and polls each one's lock bit with `readl_poll_timeout()`, which needs a
      clock that runs to time out: none does yet there. The CRU is modelled: its
      registers kept (with their write masks in the top half, as Rockchip's are), each
      PLL locked. Its PLLs start in slow mode (the crystal) until the kernel sets them.
    - Then every core sat in PSCI's CPU_SUSPEND: the device tree's idle state says
      `local-timer-stop`, so the kernel wakes idle cores with a broadcast timer, the
      Rockchip timer at 0x2acc0000, which nothing modelled. It is now: a count down at
      the rate the kernel has set clk_timer0 to (CPLL / 10, 100 MHz), worked out from
      the CRU's registers as the kernel works it out.
    - The PMU's power domains (the kernel's own table for the RK3576: on/off, repair and
      chain status, idle requests acknowledged) and the TRNG (32 bytes ready on request)
      are modelled: each timed out, one after another, for minutes.
    The kernel reaches `/init` 14 s after power-on; Debian's initramfs looks for the root
    filesystem, which has nowhere to be yet, and drops to BusyBox:
    `./flipperoneemu console 'uname -a'` answers `Linux (none) 7.4.0-rc4-g08a94c1da821
    ... aarch64`, the device tree's model is "Flipper One rev. F0B0C1", 8 CPUs, 4 GB.
    The MCU firmware runs beside it as before. What fails, and comes next: the rk3x I2C
    (the MCU's link on i2c0 — `flipper-one-mcu 0-0069` times out — the RK806 PMIC on
    i2c1, which most regulators wait for, the main board's chips on i2c2, the RTC);
    SCMI (its shared memory at 0x4010f000 is RAM to the kernel without the reserved
    region U-Boot adds, and nothing answers it); storage; DWC3, PCIe, GMAC, the display.
48. **The RK3576's I2C, its PMIC and clock, and the link to the MCU.** Nine rk3x I2C
    controllers, as Linux's i2c-rk3x drives them: TX mode sends a count from the TX
    buffer, its first byte after a START the address byte; REGISTER_TX sends the
    target and register bytes, restarts and reads; RX reads on. A count is a few steps
    (a START with its address, bytes out, bytes in), each done when the target takes or
    gives it, so that a target that holds SCL is waited for. The driver first looks
    at ST (0x220) for SDA and SCL: it read 0, a bus held low, and tried to recover it
    with GPIOs that read 0 too; ST says idle now, and the GPIO banks are modelled
    (Rockchip's v2: VER_ID, each register in halves with write masks, interrupts by
    level or edge), pulled up unless the board drives a pin. On the buses:
    - I2C1: the RK806 PMIC (a register file: its ID, the regulator enables with their
      nibble of write mask, read back with both bits). Everything that waited for its
      regulators probes: the SD host (no card), the ADC, the HDMI PHY, the UFS host
      (which fails at its own registers: not modelled yet).
    - I2C8: the HYM8563 clock, QEMU's time: "setting system clock to 2026-11-24...".
    - I2C2 is the main board's bus, which the MCU's PIO I2C drives too: two masters on
      one QEMU bus. The kernel finds the HD3SS3220, the INA219, the expander, the
      charger and gauge (whose drivers then fail to register their power supplies);
      the five INA4230s are not modelled.
    - I2C0 reaches the MCU's I2C1, in slave mode at 0x69, through the RP2350 model's
      target functions, which hold SCL until the MCU firmware's interrupt handler has
      taken or given a byte; the MCU's CPU_INT (GPIO35) is the RK3576's GPIO2_A6.
    Flipper OS's `flipper-one-mcu` driver reads the MCU's system interrupt ("MCU
    restarted, restoring its settings") and registers its input devices: Flipper One
    Haptic, Buttons, Touchpad, Headset, Software Buttons. The MCU firmware gets CPU
    state 2 and its screen says "Waiting for CPU screen…". Open: the MCU logs "invalid
    access" (the kernel reads and writes registers this firmware build does not have)
    and the kernel's UCSI part finds no device tree node; in one of six starts with the
    kernel running, the MCU's PIO I2C hung in its first read of the main bus — not
    seen since, to be looked into if it comes back.
49. **The display link: Linux's frames on the MCU's screen.** Flipper OS's
    `flipper_one_display` driver (DRM, on SPI0) sends a frame for each commit: "active"
    low (the RK3576's GPIO0_D1), the frame over SPI, "active" high. SPI0 waited for its
    DMA channels: the three PL330s are QEMU's model now. The kernel's AMBA bus reads a
    PrimeCell's IDs from the last 0x20 bytes of its region, 16 KiB here, where only
    unimplemented space lay behind QEMU's 4 KiB of registers: the IDs are there too
    now. The Rockchip SPI controller is modelled as the driver uses it: words written
    to TXDR go out at once (QEMU's PL330 writes a burst to a fixed address as one
    write, so with TX DMA on each byte of an access is data), an idle controller, the
    newer kind's 64-word FIFO, a word back for each word sent when it is not transmit
    only. The board takes SPI0's bytes between "active" falling and rising (the GPIO
    banks tell it of their pins' levels) and the frame goes to the MCU as host_spi.c
    sends the host link's frames, edge by edge on its GPIO25, 26 and 28. From the
    initramfs's shell: `echo 0 > /sys/class/graphics/fb0/blank` (the commit that
    switches the display on) shows what was in the framebuffer on the MCU's screen,
    and each blank and unblank again; plain writes to /dev/fb0 commit nothing in this
    kernel. FlipCTL, which draws with DRM, needs the root filesystem.
50. **Storage: Flipper OS boots, and FlipCTL is on the screen.** The microSD slot's
    host is a DesignWare MSHC, which QEMU does not have: modelled in front of QEMU's
    SD card as Linux's dw_mmc drives it (commands to the card at once, responses,
    RINTSTS; data through the internal DMA's descriptor chain, or the FIFO for the
    driver's few bytes; DATA_OVER, the automatic CMD12; VERID 0x270a, a 32-bit FIFO).
    QEMU's SD cards are a power of 2 in size: the device's card is the build's image
    made 8 GiB (sparse), `devices/NAME/sd.img`, made by `run --linux` the first time.
    QEMU's card offers no 1.8 V signalling, so the kernel keeps it at 3.3 V high speed
    (no UHS tuning). The first try stopped at the card's first command: CPU 0 took
    interrupts and nothing else, the SD host's among those it never got to. The
    card-detect pin (GPIO0_A7, low with a card in) had BOTHEDGE set by the kernel, which
    makes a pin edge-triggered whatever INT_TYPE says; the GPIO model took it as a
    level, active low, pending for as long as the card was in. With that right: "mmc0:
    new high speed SDHC card", `mmcblk0` with its three partitions, the initramfs
    mounts the btrfs root, and systemd starts ("Welcome to Debian GNU/Linux 13
    (trixie)!"). The boot entries leave out `rw`, and Debian's initramfs mounts the root
    read-only then, where a first boot must write /etc/machine-id: `run --linux` adds
    `rw`. The first boot takes about 8 minutes (ldconfig alone 3.5), then
    `flipperone-8a726c login: user (automatic login)`. `flipctl.service` ("flipctl on the
    Flipper One panel") runs, and its interface is on the Flipper's screen in the
    window (Temperature, Power flow, Profile: Minimal, the hostname, Menu and Desktop);
    the View key opens its menu (Desktop Computer, Apps, Files, Network): the key goes
    through the MCU firmware and the I2C link to the kernel's input device, and the
    new frame back over the display SPI. `flipper-one-online.service` reports the boot
    to the MCU. Only the USB gadget's setup fails (the RK3576's USB is not modelled).
    Open: the SD driver warns "Interrupt latency" now and then (its watchdog found an
    interrupt it had not been sent yet).

## The RK3576's power (2026-11-24)

51. **The MCU switches Linux on and off.** On the device the MCU powers the RK3576:
    VCC5V0_SYS_S5, the 5 V rail for the SoC and its PMIC, is the main expander's pin 12,
    which the firmware's Host service switches on (`furi_bsp_linux_start`) and cuts by
    resetting the expander. Until now the RK3576 started with the machine. Now its
    cores wait, off, until that pin goes high, 270 ms into the MCU's start (its BSP
    init). The kernel then starts as QEMU's direct boot starts it, from its images in
    DRAM as they were loaded: saved at the first start and put back at each later one,
    since a run writes over them. Without the rail its cores are off and its devices
    as at power-on: the GIC, the UARTs, the DMA controllers, the I2C controllers and
    what is on their buses, SPI0, the SD host and its card, the CRU, PMU, GPIO and
    timer. QEMU's PSCI stood for the whole machine: SYSTEM_OFF stopped QEMU and
    SYSTEM_RESET reset the MCU too. QEMU patch 0005 lets the machine answer a PSCI call
    first, and those two are now the RK3576's alone. SYSTEM_OFF switches it off (on the
    device TF-A does, and the rail stays on), and SYSTEM_RESET starts it again. All four
    ways work with Flipper OS:
    - `systemctl poweroff`: the kernel's driver tells the MCU 7, 8, 9 (powered off), the
      RK3576 is off, and 300 ms later the MCU cuts the rail and stops its CPU app;
    - the desktop's CPU app: the rail comes on, the kernel starts again, and Flipper OS
      boots;
    - `systemctl reboot`: 6, 8, 10 (restarting), the RK3576 starts again, and the MCU
      leaves its power alone;
    - closing the CPU app while Linux runs: the rail is cut, and the RK3576 is off at
      once (its cores then use no host CPU).
    The panel link reports it (`rk3576 on`, `rk3576 off`: docs/PANEL_LINK.md), and QEMU's
    log (`devices/NAME/qemu.log`) has each change with its time.

52. **The MCU's clock, and the SD card's speed.** With Linux running, the MCU's clock fell
    behind: in a first boot its log said 204 s when 390 s had passed. The SD driver
    also warned "Interrupt latency": its 10 ms watch on a command expired with the
    interrupt raised but not yet taken. Both came from QEMU's global lock (the BQL).
    The main loop runs every device's timers, the RP2350's SysTick among them, and it
    was waiting for that lock most of the time. The firmware counts its time in
    SysTick ticks, and a tick that comes late is lost.
    QEMU's sync-profile (`sync-profile on`, then `info sync-profile`, in the monitor)
    and gdb samples of the lock's holder (`bql.lock.__data.__owner`) found the holder:
    the SD card. QEMU's card asked for every byte whether the medium was in
    (`blk_is_inserted()`, now a coroutine's work, about 300 ns), and read and wrote its
    image a block at a time, one round trip to the block layer's thread pool for each
    512 bytes. A megabyte took 300 to 580 ms, all of it with the lock held by the core
    that wrote the command register. QEMU patch 0007 keeps the medium's state from its
    last change, reads ahead a megabyte at a time, and writes consecutive blocks in one
    request. A megabyte now takes 12 to 18 ms, and over a minute of booting the MCU's
    clock kept 98 % of time, against 81 % before. The generic timer's counters also took
    the lock on every read, and Linux reads them all the time (the vDSO too). QEMU patch
    0006 reads them without it. Measured with and without it: without it the counters
    are the lock's largest wait (549,000 times in 20 s of booting, 32 s of waiting, against
    about 90,000 lock takings there with it); a loop of 200 `date`s in Linux takes 29 to
    36 s instead of 26.5 to 29; and FlipCTL's first frame comes at 198 s instead of 135
    to 169 s. The contention that remains is QEMU's own: exceptions and interrupts are
    taken under the lock. A GIC that looked only at the enabled interrupts for the best
    one (QEMU's rescans all 480 for each core at every acknowledge and EOI) made no
    difference that the benchmark could measure, and was left out.

## The main board's bus (2026-11-24)

53. **The main bus's two masters, and a state machine that ran off its program.** On the
    device the main board's bus has two masters. One is the MCU's PIO I2C. The other is
    the RK3576's I2C2, through which Linux drives the expander, the charger, the INA219
    and the USB-C chips. QEMU's I2C bus has no arbitration, and its core looks up the
    addressed chip only at a transfer's first START. So a transfer that one master
    started while the other's was under way went to the chip the other had addressed.
    Now the bus takes one transfer at a time, as a busy bus does:
    - the RK3576's controller waits while the MCU's transfer is under way (its START is
      tried again every 20 µs);
    - while the RK3576's transfer is under way, the MCU sees SCL held low, as the
      other's clock pulls it. Its PIO program waits at its next bit (`wait 1 pin, 1`,
      its clock stretching), and a START it made meanwhile reaches the bus afterwards.
    Loaded from both sides (Linux reading the INA219 in a loop while the MCU's CLI ran
    `i2c search main` again and again), Linux made 838 transfers. 139 of them waited for
    the MCU's, and 2 of the MCU's STARTs waited for Linux's. Every reading was right
    (7.944 to 7.948 V), with no error on either side.

    Under that load the MCU's PIO I2C also hung: its searches stopped, QEMU's main loop
    ran at 100 %, and Linux reported RCU stalls. There were two causes.
    - **The PIO model's, mine.** The firmware recovers from a NACK by forcing
      instructions (SMx_INSTR) while its state machine waits at `irq wait`. The model
      kept that wait's state through them, so at the next NACK the `irq wait` passed
      without raising its flag, and the firmware never knew. A forced instruction now
      ends the wait. The datasheet does not say which is right; this is the reading
      under which the recovery, which is the Pico examples' own, works.
    - **The firmware's, FW-01** (docs/FIRMWARE_BUGS.md, found with a trace of the state
      machine's last 64 instructions). The same recovery drains the TX FIFO with forced
      `out null, 32`. With autopull, the datasheet's OSR refills in the same cycle as an
      OUT that empties it, so the last word drained stays in the OSR. The program then
      reads it as a header and executes the next words as instructions, and the STOP's
      header (0x0800) is `jmp 0`, into empty instruction memory. In the emulator this
      shows only when the transfer's later words were queued before the NACK came:
      about one search in ten or twenty.
    The emulator had a share in how bad that was. A state machine looping on `jmp 0`
    ran instruction by instruction at 12.8 MHz in QEMU's main loop, which then had no
    time for anything else, and both chips stalled. A state machine that jumps to
    itself, always, now waits for the CPU or the DMA, as a stall does; on the chip such
    a loop costs nothing either. Now FW-01 costs only the bus: QEMU's main thread uses
    10 % of a core, Linux answers, and there are no RCU stalls. The hang in entry 48 (one
    start in six with the kernel running, in the MCU's first read of the main bus) may
    have been this recovery after a NACK at start-up; it has not come back. The MCU uses
    the five INA4230s, which are not modelled yet, only in `power consumption`. Its
    probes of them are NACKs, and so they can set off FW-01 too.

54. **The five power monitors.** The main bus has five TI INA4230s (0x40, 0x41, 0x43, 0x44,
    0x46), four channels each, on nineteen of the RK3576's rails and on the control
    board's 3.3 V. The MCU firmware reads them in `power consumption`, and Linux's
    `ina4230` driver binds to them (Flipper's own, from its INA3221 driver). Both drive
    the same registers, as the datasheet gives them:
    - the shunt voltage in 2.5 µV steps, or 625 nV in the 20.48 mV range;
    - the bus voltage in 1.6 mV steps;
    - the current, SHUNT × CALIBRATION / 2048;
    - the power, CURRENT × BUS / 20000;
    - the 32-bit energy, read as four bytes;
    - a conversion-ready flag at the end of each cycle, which Linux waits for after a
      reset;
    - RST and ACC_RST in CONFIG2.
    Each driver calibrates its own way and reads the same currents. The shunts are the
    board's (the device tree and the firmware agree). The voltages are the rails'
    nominal ones, while the RK3576 runs; the control board's 3.3 V is on whenever the
    board is. The currents are nominal, not measured: Flipper OS at rest, and on the
    two CPU rails so much more for each core of the cluster that is running when a
    channel is read.
    - Without Linux: the control board's 3.3 V at 60 mA, every other rail at 0.
    - With Flipper OS at rest: the A72s' rail at 60 mA and the A53s' at 40 mA, plus
      one A53 core's 120 mA when the reading comes from a shell on it.
    - With eight busy loops: 1.459 A (1.09 W) and 0.520 A.
    The MCU's probes of these addresses now get an ACK, so `power consumption` no longer
    risks FW-01. The smoke test checks the five chips and the control board's rail.

## A check of the whole device (2026-11-24)

55. **The first boot in two to three minutes, and `scripts/linux_check.py`.** With QEMU
    patches 0006 and 0007, a first boot (Flipper OS setting itself up on a new card)
    reaches its login 107 to 200 s after the start; before them, about five minutes
    (FlipCTL's first frame at 321 s). The MCU's clock keeps time through it: 239.2 s of
    its own against 246.8 s of virtual time (97 %), where it was at half speed. Now and
    then a reboot or power-off waits 90 s (one shutdown in five here): BlueZ's
    `mpris-proxy`, a user service, ignored SIGTERM and was killed at the stop timeout.
    It has no Bluetooth adapter to talk to: the Flipper One's is in its Wi-Fi chip,
    which is on USB, not modelled yet. That is Debian's behaviour and none of the MCU
    firmware's.

    `scripts/linux_check.py` checks the whole device, as `smoke_test.py` checks the MCU,
    on a throwaway device:
    - the RK3576 starting at the MCU's rail, and the panel link's `rk3576 on`;
    - the kernel on the Flipper One's device tree, and the MCU seeing CPU states 2 and 3;
    - the login, FlipCTL's frame on the Flipper's screen, and View reaching FlipCTL;
    - Linux reading VSYS (the INA219) and the five INA4230s over the bus it shares with
      the MCU, and its root on the microSD card;
    - `systemctl reboot` (the RK3576 starting again, a second login);
    - `systemctl poweroff` (PSCI off, then the MCU cutting the rail);
    - the MCU's CPU app switching it on again;
    - no oops, panic or RCU stall, and no MCU crash.
    Twenty checks, which pass. A first boot is long, so the first run keeps its card as
    it is after the checked power-off (`build/linux-check/sd.img`, for that Flipper OS
    build). Later runs start from a reflink of it. Two lessons for driving the console:
    getty and the shell throw away what was typed before they were up, so the check
    waits for a shell that answers; and a terminal escape split across two reads must
    be stripped from the text whole, not per read.

56. **SCMI: the CPU clusters' clocks, from the TF-A that the board stands in for.** Linux
    gets the A53s', the A72s' and the GPU's clocks from TF-A, over SCMI: a message in
    shared memory at 0x4010f000, then SMC 0x82000010. Three things were missing:
    - **The shared memory.** It lies in the first 2 MiB of DRAM, which TF-A keeps. The
      bootloader hands the kernel its memory after them, but QEMU's direct boot gave
      all of it from 0x40000000, so `ioremap` of that page warned and SCMI failed. The
      kernel's memory now starts at 0x40200000.
    - **SMCCC 1.1.** QEMU's PSCI does not know SMCCC_VERSION. Linux therefore took the
      firmware for SMCCC 1.0, and `arm_smccc_1_1_invoke()`, which SCMI's transport
      calls, did nothing: no SMC ever came. The board's PSCI hook (QEMU patch 0005) now
      answers as TF-A does: SMCCC_VERSION is 1.1, PSCI_FEATURES lists it, and
      SMCCC_ARCH_FEATURES has nothing. No ARCH_WORKAROUND is offered, since each would
      be an SMC at every exception entry; Linux reports Spectre-v2 as unmitigated.
    - **The server** (`rk3576_scmi.c`). Base protocol 2.0 lists the clock protocol;
      clock protocol 1.0 has 457 clocks as the device tree numbers them, of which
      SCMI_ARMCLK_L (10), SCMI_ARMCLK_B (11) and SCMI_CLK_GPU (456) are there. Each
      takes any rate from 24 MHz to 2.4 GHz in 1 MHz steps. Its names start with
      `scmi_`, since the CRU's own clocks are `armclk_l` and so on, and Linux would not
      register a second clock of the same name.
    Linux now reports "SCMI Protocol v2.0 'FlipperOneEmu:TF-A'", and cpufreq has both
    clusters (408 MHz to 2.016 and 2.208 GHz).

    That cost something, though. schedutil changed the clusters' rates dozens of times a
    second (27 rate sets a second in a benchmark), each with an SMC and, for its
    voltage, a write to the PMIC over I2C (87 transfers a second on I2C1). Flipper OS
    became a third slower (200 `date`s: 34 to 35 s against 26.5 to 29), for nothing:
    the emulated cores run as fast as the host lets them, whatever their clock. So the
    launcher boots with `cpufreq.default_governor=performance`, and the benchmark is
    back at 24 to 27 s. The clocks are still there to set: a governor can be chosen as
    on the device. The Linux check confirms both clusters' cpufreq (21 checks).

## Temperatures (2026-11-24)

57. **The temperature sensor, and the OTP it is calibrated from.** Linux had no thermal
    zones: its TSADC driver reads each sensor's trim from the OTP before it starts the
    sensor, and the OTP's controller was not modelled. The read timed out ("failed
    reading trim of sensor 0", -ETIMEDOUT) and the probe failed. FlipCTL's home screen
    has a Temperature heading. Now there are both:
    - **The OTP** (0x2a580000). The non-secure controller reads a word at a time (the
      address in AUTO_CTRL, then AUTO_EN; RD_DONE in INT_ST; DOUT0). Linux reads the
      256 bytes from byte 0x700, which hold, as the device tree's cells name them:
      - the CPU's code, 35 76;
      - a 16-byte ID, here "FlipperOneEmu". Flipper's kernel derives the Ethernet
        ports' addresses from it: the first six bytes of the SHA-256 of its hex;
      - the six sensors' trims.
      What nothing reads is 0.
    - **The TSADC** (0x2ae70000, SPI 123: `rk3576_tsadc.c`). Six channels: the SoC, the
      big cores, the little cores, DDR, the NPU and the GPU. In auto mode each channel's
      9-bit code follows its temperature by the kernel's own table (285 at 25 °C, 350 at
      85 °C, straight between). Each sensor reads a code or two off. Its trim says by
      how much: the code it read at 30 °C, against a true sensor's 290. The kernel takes
      that off again, at 923 m°C a code. A channel over its alarm code (COMP_INT) raises
      the interrupt. One over its shutdown code (COMP_SHUT) resets the chip, as its CRU
      does, if the TSHUT goes to the CRU (the board restarts the RK3576 as for PSCI's
      SYSTEM_RESET); the TSHUT pin, the other way, is logged, since the board does not
      wire it.
    - **How warm the chip is.** Nominal, not measured, as the power monitors' currents
      are:
      - 25 °C around it, the battery's temperature too;
      - the chip's own 0.4 W, plus each running core's share of the CPU rails'
        currents: 0.26 W for an A72 and 0.09 W for an A53;
      - 20 °C a watt over the ambient for the whole chip, and 8 °C more a watt of its own
        for a cluster's sensor;
      - a time constant of 10 s, with ten samples a second.
      At rest that settles near 34 °C. Eight busy loops took the SoC's sensor from 39 to
      61 °C and the A72s' from 41 to 69 °C in 35 s. Twenty seconds after they ended, the
      sensors were back at 43 to 46 °C.
    Linux now has six thermal zones, which hwmon shows as well, and cpufreq cooling for
    both clusters. FlipCTL's home screen shows the temperature under its heading: 60.1 °C
    just after a boot, which keeps the cores busy. The OTP's bytes read back through the
    nvmem device as written. The device tree names thermal zone 3 the GPU's and zone 5
    DDR's. Its sensor nodes, though, give channel 3 DDR's trim and channel 5 the GPU's,
    and the driver's comment agrees with the trims. Here the two channels read alike.
    The Linux check reads all six zones (22 checks).

58. **The temperature around it, and cores that draw less when slowed.** The panel link
    has `ambient` (and the window's Device menu a few values): the temperature around
    the device, 25 °C until set. The battery takes it at once, for its gauge and the
    charger's TS input. The charger's die is 5 °C above it, where it was a fixed 30 °C.
    The RK3576 warms from it. Raising it found four things:
    - **Slower cores drew the same.** At 85 °C, cpufreq's cooling took both clusters to
      408 MHz, and the chip went on to 116 °C anyway. Linux began its critical
      shutdown, and TSHUT reset the chip at 119.2 °C, again after each boot. Now a
      running core's draw follows its cluster's clock and the voltage the OPP table
      gives that clock: its current as (f / f_top)(V / V_top), its power as (f /
      f_top)(V / V_top)². The INA4230s' CPU rails read that voltage and that current
      too: the A72s' rail at 0.925 V and 1.459 A with four cores running at 2208 MHz.
      At 85 °C the chip now holds near 94 °C, its clusters at 408 to 600 MHz, after
      three alarms.
    - **The A72s never left 816 MHz** (a bug in entry 56's SCMI server). Their rail read
      0.70 V while Linux reported 2208 MHz. A trace of the SCMI messages (a log line in
      the server, for one boot) showed Linux sending that rate. QEMU's `ldl_le_p()`
      returns a signed int, so 2208 MHz (0x839b6800) became a 64-bit number over the
      server's maximum, and RATE_SET answered INVALID. Linux's clock framework does not
      look at `set_rate`'s result, so cpufreq recorded the change and the clock stayed
      at TF-A's 816 MHz. The A53s' top, 2016 MHz, is under 2^31 and worked. The server
      now reads a message's words unsigned. The board's other `ldl_*_p()` reads land
      in `uint32_t`s.
    - **An alarm at its own threshold.** The TSADC compared "at or over". The kernel
      sets an alarm for a trip at the trip's code, and reads that code back a little
      under the trip (84.999 °C for 85 °C, after the trim), so it set the same alarm
      again. It would then go off at every sample until the next code. TSHUT fired at
      119.2 °C, under the 120 °C it was set for. Now both compare "over", as the TRM
      words it ("higher than"): TSHUT came at 120.1 to 120.4 °C.
    - **OS-01, in Linux's driver** (docs/FIRMWARE_BUGS.md). Past a zone's last trip,
      `rockchip_thermal_set_trips()` adds the sensor's trim to INT_MAX, which then no
      longer means "no alarm". The alarm stays armed below the temperature, with
      "Failed to set trips: -34" in the log. Upstream Linux has the same lines.
    What the device does in the heat, now:
    - at 105 °C and a busy loop on each core, the zones pass 115 °C, and Linux's
      protective shutdown ends in PSCI's SYSTEM_OFF 30 s later; then the MCU cuts the
      rail;
    - at 110 °C, the chip reaches 120 °C before that shutdown is done, and TSHUT resets
      it. It boots into the same heat and is shut down again 8 s after each start.
    From about 105 °C around it, Flipper OS cannot run. The smoke test checks that the
    ambient reaches the battery and the charger.

## Storage (2026-11-24)

59. **The 64 GB UFS.** The Flipper One's own storage is 64 GB of UFS 2.2; it boots from
    microSD first, then UFS. Its host (`rockchip,rk3576-ufshc`, 0x2a2d0000) was
    unimplemented registers, which read 0. Linux sized its command descriptors from the
    capability register, the allocation failed with a WARN at every boot, and the probe
    failed (-ENOMEM). Now there is a UFS, in three parts:
    - **QEMU's UFS host** (UFSHCI 4.0, with a UFS device and its logical units behind
      it). It is a PCI device, so it sits on a PCI host of its own (QEMU's generic one)
      that is mapped nowhere: the kernel never sees a PCI bus. The host's registers are
      aliased at 0x2a2d0000, and its INTA (slot 1, which is the host's line 1) goes to
      SPI 361. It moves data in the system's memory with bus mastering, which a reset
      clears, so it is set again at each start of the RK3576. The M-PHY and the GRFs
      around it take what Rockchip's driver writes, as unimplemented registers do.
    - **Three QEMU patches**, each one a small change to its UFS:
      - 0008, DME_RESET and DME_ENABLE. Rockchip's driver resets and enables UniPro
        before it sets its PHY up, and QEMU answered both FAILURE.
      - 0009, a "lanes" property. The RK3576's link has two lanes, and Linux checks
        PA_CONNECTEDTXDATALANES against them ("Tx lane mismatch [config,reported]
        [2,1]"); QEMU reported one.
      - 0010, fWriteBoosterBufferFlushDuringHibernate settable. Rockchip's driver turns
        WriteBooster on and sets that flag, which QEMU kept read-only: three failed
        queries at every boot.
    - **The disk.** LU 0 is the device's `ufs.img`, which the launcher makes blank and
      sparse, as a UFS that nothing was installed on: 64,000,000,000 bytes, 15,625,000
      blocks of 4 KiB ("64.0 GB/59.6 GiB" to Linux). Its inquiry data are QEMU's ("QEMU
      HARDDISK", and "QEMU UFS" for the well-known LUs), not a UFS vendor's.
    Linux now attaches `sda`, with no WARN and no failed query. 32 MiB written to it
    and read back match. Reading 32 MiB takes 4.6 s, against 6.1 s from the microSD: the
    guest's own work bounds both. The Linux check looks for the 64 GB disk on the UFS
    host.

## Ethernet (2026-11-24)

60. **Both Ethernet ports, and a network for the first.** The RK3576's two GMACs and
    the board's RTL8211F PHYs were unimplemented registers. The stmmac driver got as far
    as its PHY, which read all zeros over MDIO, and phylink refused it ("validation of
    rgmii-id ... failed", "cannot attach to PHY"). A spec of what the driver needs came
    first, read from stmmac and the realtek PHY driver in Flipper's kernel. Then the
    model, `rk_gmac.c` (DWC Ethernet QoS 4.20a):
    - **The MAC.** Version 4.20. Its hardware features are as plain as the driver allows:
      one queue each way, 16 KiB FIFOs and 32-bit DMA, with no checksum offload, TSO,
      PTP, EEE, VLAN filtering, counters, wake-up frames or split header. An MDIO
      transfer and a DMA reset are done as they are asked for.
    - **The DMA**, on GMAC4's 16-byte descriptors. TX goes from the current descriptor
      to the tail pointer, frame by frame, padded to 60 bytes, and gives each descriptor
      back. RX writes each frame with its FCS (the driver strips 4 bytes) into the
      descriptors the driver owns, then raises RI at once; on the chip the RX watchdog
      raises it a little later, since the driver sets no IOC. A frame that finds too few
      descriptors waits for the driver's next tail write (RBU).
    - **The PHY**: an RTL8211F at MDIO address 1. Autonegotiation is instant, and the
      link is 1000 Mbit/s full duplex while there is one. INSR and INER drive its INTB,
      which goes low on GPIO1_C1 (port 1) or GPIO1_C2 (port 2), and phylib runs on that
      interrupt; it does not poll.
    - **The network.** Each port takes the next `-nic`, and has no link without one: no
      cable. The launcher's `run --net` puts port 1 on this PC's network through passt,
      which QEMU starts: NAT, DHCP and DNS, with no root needed. This QEMU has no slirp
      (libslirp-devel is not installed), and passt is what Fedora installs with podman.
    The results:
    - both ports attach "RTL8211F Gigabit Ethernet", and port 1 reports "end0: Link is
      Up - 1Gbps/Full";
    - DHCP gives end0 10.10.10.124/24 (passt gives the guest this PC's own address), the
      gateway answers ping, and DNS resolves;
    - 8 MiB from a server on this PC to the device took 0.98 s (68 Mbit/s), and 0.78 s
      back (86 Mbit/s), each matching its MD5;
    - the monitor's `set_link` takes the link down and up, which the PHY's interrupt
      brings to Linux;
    - `ip link set end0 down` and `up` run the driver's whole close and open again.
    The Linux check looks for both PHYs. With passt installed it runs with `--net` and
    checks port 1's link, address and gateway.

61. **SSH from this PC.** Flipper OS runs OpenSSH, socket-activated, on port 22.
    `run --ssh PORT` gives port 1 its network, as `--net` does, and passt forwards this
    PC's PORT to the device's port 22 (`-nic passt,tcp-ports=127.0.0.1/PORT:22`).
    passt bound the port on every interface until it was given the address; on
    loopback only, the device is not open to the LAN. The banner comes through,
    `SSH-2.0-OpenSSH_10.0p2 Debian-7+deb13u4`. The first connection waits for sshd to
    start, a few seconds here. How to log in is Flipper OS's business, since `user` has
    a password of its own: the console logs in by itself, and `passwd` there, or a key
    in `~/.ssh/authorized_keys`, opens SSH.

## USB (2026-11-25)

62. **The USB controllers, and the battery in Linux.** Both of the RK3576's DWC3 cores
    read 0, and dwc3 said "this is not a DesignWare USB3 DRD Core". The FUSB302's Type-C
    port waits for USB0's role switch, the charger's power supply waits for the Type-C
    port's, and the battery's waits for the charger's. So Linux had no charger and no
    battery, and FlipCTL had no power flow. A spec came first again, read from dwc3,
    xhci-plat, the PHY drivers and tcpm. Then `rk_dwc3.c`, a dual-role DWC_usb3 3.30a
    over QEMU's own xHCI:
    - **The core.** GSNPSID 0x5533330a. GHWPARAMS give a dual-role core (MODE 2): with
      MODE 1, USB0 would be forced to host and would never register the role switch.
      DCTL's soft reset is done at once; Linux resets the core through it on both
      ports. DSTS is halted while RUN_STOP is clear, commands are done at once, and
      GEVNTCOUNT reads 0, since no event is written. So a gadget binds and runs, and
      no host comes.
    - **The host role.** QEMU's xHCI sits at each controller's offset 0, with one USB 2
      and one USB 3 root port, one interrupter and 64 slots. A controller's other
      registers read 0. Two things were in the way:
      - **ERSTBA.** For DWC3, Linux writes it high dword first (its write-64-hi-lo
        quirk), and QEMU loads the event ring on the high write, so it read the ring
        from the new high half and the old low half. The first event would kill the
        controller. QEMU patch 0011 adds "erstba-hi-lo": the low write loads it, as
        on DWC3.
      - **Slots.** sysbus-xhci's defaults for intrs and slots do not survive the
        initialisation of the core it holds, which zeroes them; slots then reached
        realize as 0 and were clamped to 1. The second device got "No Slots Available"
        until both were set.
    - **The USBDP combo PHY** (USB0's USB3 PHY): its PLL lock (PMA 0x350 bits 7 and 6)
      and clock recovery (0xb84, 0x1b84) read as done. Linux polls them, and the first
      fails the controller after 100 ms.
    The result:
    - USB1's xHCI has two root hubs, and USB0 sits in its device role. With no
      role-switch-default-mode in the device tree, "none" is the device role too.
    - The FUSB302's Type-C ports register, and so do the charger, the battery
      (`bq28z620-0`, 79 %) and tcpm's source. FlipCTL's home screen fills in: the
      battery's temperature beside the SoC's, the power flow (-3.18 W), and the charge
      in its status bar.
    - Flipper OS's gadget (NCM, MTP and ACM) binds to USB0's UDC: `flipusb0` and its
      DHCP server are there, waiting for a computer. The computer on the USB-C port
      is still the MCU's.
    - A keyboard and a tablet plugged in from QEMU's monitor (`device_add usb-kbd`, on
      `usb-bus.1`, USB1's) are enumerated behind the hub QEMU adds to the one root
      port, and are input devices.
    One side effect: the USBDP PHY gives the DP controller its aux bridge, so DP now
    probes too, and its audio with it. As the audio stack starts, it probes that card
    in two bursts, about 60 errors a boot, and then stops:
    - "hdmi-audio-codec ... ASoC error (-22)": the DP codec will not prepare with no
      sink, as it would refuse on a device with nothing on DP;
    - "rockchip-sai ... Failed to idle FS": SAI6 is not modelled, and its FS-idle bit
      never comes.
    Linux now also has a seventh thermal zone, the battery's (25.1 °C). The Linux check
    now picks the TSADC's six by their type, and checks USB0, USB1 and the battery
    (27 checks). The UCSI error that remains
    ("flipper-one-typec: Failed to locate of_node") is Flipper OS's own: the MCU driver
    makes that cell from a child node of compatible "flipper,one-typec", which build
    2501's device tree does not have.

63. **A boot that never logged in (OS-02).** With USB in, one Linux check timed out
    waiting for the login, and a loop of boots stalled once in six. The kernel was
    fine:
    - the MCU had seen CPU state 3;
    - every core idled in PSCI's suspend, and TIMER0 was still re-armed;
    - UART0 had read the input sent to it;
    - a SysRq line came out at once.
    The console's getty had started, though ("Started serial-getty@ttyS0") and never
    written. Finding out why took a few tools, since the guest had no shell:
    - **SysRq.** A break from QEMU's monitor (`chardev-send-break`) and SysRq `w`
      showed nothing blocked. Flipper OS limits SysRq at run time (kernel.sysrq = 438),
      so `sysrq_enabled` was set to 1 in guest RAM through gdb, at the physical address
      that System.map and `kimage_voffset` give. A full `t` dump was then too big for
      the log buffer.
    - **Strings in RAM.** Searching the guest's RAM for agetty's own strings
      (/proc/PID/mem over QEMU's RAM block) found none, where a good boot has them:
      agetty had never been executed.
    - **The tasks' stacks.** The `(agetty)` task names led to `task_struct`s, and their
      saved fp (a pc in `__switch_to`) to stacks, read through the monitor with CPU 2's
      page tables (`x`) and named from System.map. Two waited in `flock`; one in
      `n_tty_write`, and its stack held ttyGS0's `tty_struct`.
    That is Flipper OS's (OS-02 in docs/FIRMWARE_BUGS.md). systemd holds /dev/console's
    lock while it sets up a getty's tty; the gadget's serial port has no room to write
    until a computer connects to the gadget; so its getty's setup blocks with the lock,
    and the ttyS0 and tty1 gettys wait for it. It is new here only because USB0 now has
    the device role that the gadget binds to. The Linux check masks that one getty on
    the kernel's command line, through the launcher's new `run --kernel-args`. The
    tools are a recipe for the next hang with no shell: a break with SysRq, a search of
    guest RAM, and stacks walked from `task_struct`s.

64. **A computer on Flipper OS's USB.** The MCU firmware raises USB20_SEL with the Linux
    side's rail, so on the device a computer plugged into the USB-C port while Flipper
    OS runs reaches the RK3576's USB0, not the MCU. Here it reached nothing. Now:
    - **The virtual PC drives either device.** `usb_host.c` drives a device controller
      through `fo_usb.h`: its pull-up, whether its CPU still has events to take, a bus
      reset, a detach, and SETUP, OUT and IN transactions answered with ACK, NAK, STALL
      or nothing. The MCU's USB is one; the board switches the PC between it and USB0
      as USB20_SEL changes. The PC itself:
      - takes packets of up to 1024 bytes (it had 64; high speed's bulk is 512) and
        sends OUT packets of the endpoint's size;
      - finds a CDC ACM function by its Union descriptor (Flipper OS's NCM data
        interface is class 0x0A too, and its endpoints were being taken);
      - says high speed over USB/IP for USB0.
    - **USB0's device role** (`rk_dwc3.c`), from a spec read in the 7.4-rc4 dwc3
      driver:
      - **Events.** They go into event buffer 0 as the core writes them, word by word.
        GEVNTCOUNT counts their bytes, and the driver's writes subtract. The interrupt
        is up while that is not 0 and not masked, ORed with the xHCI's.
      - **Commands.** Endpoint commands are done as they are written, and End
        Transfer's completion event comes when CMDIOC asks for it; without it, an
        endpoint is dead after its next disable.
      - **TRBs.** They are walked in memory: link and chained TRBs, a short packet
        ending a chain. A TRB done gets its size and control written back, never its
        buffer's address, since the SETUP packet is written there.
      - **EP0.** A SETUP is always ACKed, into its SETUP TRB. A data or status stage
        that finds no TRB gives one Transfer Not Ready, which is how the driver knows
        to start the status stage. SET_ADDRESS takes effect once its status stage is
        done.
      - **Speed.** DSTS says high speed. Its 4 (SuperSpeed) would have made the driver
        set EP0's packet size to 512, and the device descriptor's bMaxPacketSize0 to 9.
    - **Two bugs on the way, both the model's.** They were found with the guest's
      dwc3 trace events, enabled before the computer was plugged in, and the PC's
      state read with gdb (its last 16 transactions).
      - **Enumeration stopped at SET_CONFIGURATION's status stage.** The PC NAKed its
        status IN 1.2 million times, while Linux said "configured". The PC waits at
        most 10 ms for the driver after an event. Under TCG the driver's interrupt
        thread can take longer, so the PC sent the SETUP before the driver had given
        EP0 a SETUP TRB. The model held the SETUP, and the status IN then raised
        Transfer Not Ready ahead of the SETUP's Transfer Complete. The driver ignores
        one that comes before it expects it, and the hardware raises it once. Now a
        token to EP0 while a SETUP is held is NAKed with no event, and a held SETUP
        counts as an event still to take, so the PC waits for it.
      - **A replug in that state left EP0 stalled.** The driver had abandoned the
        transfer and stalled EP0, and the stall outlived the SETUP given after it.
        Now a SETUP given clears the stall. Also, a detach in the middle of a control
        transfer now raises the status stage's Transfer Not Ready the host would have
        raised. That way the driver's disconnect ends the transfer and gives EP0 its
        SETUP TRB again; otherwise it waits for the status stage forever.
    - **The result.** `37c1:f12f "Flipper FZCO" "Flipper One Composite Device"`, at
      high speed: NCM, MTP (FunctionFS) and ACM.
      - **Its serial port.** It is on usb.sock: Debian's getty, with its login prompt.
      - **USB/IP**, as the PC's kernel would take it (scripts/fo_usbip.py in
        userspace):
        - the device list says high speed;
        - a SET_CONFIGURATION on the configured gadget restarts its functions;
        - the ACM function gives the login prompt;
        - over NCM (alternate 1, NTB16 both ways), Flipper OS's dnsmasq offers 10.0.0.2
          (its own end is 10.0.0.1). It answers once flipusb0 has its address, about
          3 s after the link comes up, so a DHCP client's retries are needed.
      - **Unplugged and plugged in again.** Disconnect, then Reset and Connect Done,
        and it is configured again.
      The Linux check plugs a computer in, checks the enumeration, the state Linux
      sees, a line each way over ttyGS0, and the unplugging (31 checks).
    - **OS-02, confirmed.** With a computer able to reach the gadget, the bug's
      account could be checked: in 13 boots without the Linux check's mask, the 13th
      hung (its three gettys started, no login after 340 s). The panel link's `usb 2`
      then had the gadget configured, and the console logged in 2 s later, with all
      three gettys running. So the gadget's getty held the console's lock until a
      computer configured the gadget, as entry 63 read it.

65. **Sound.** Flipper OS's card "On-board Analog NAU8822" is a simple-audio-card. It
    pairs SAI2, a serial audio interface on the I2S link, with a Nuvoton NAU8822 codec
    on I2C6, and the codec is the link's master. Nothing answered at 0x1a ("Failed to
    issue reset: -6"), so there was no card, and SAI6 (HDMI's) logged "Failed to idle
    FS" at every boot. A spec came first, read from rockchip_sai, nau8822, pl330 and
    simple-card at the kernel's own revision. Then:
    - **`rk_sai.c`**, the three SAIs Flipper OS enables (0, 2, 6):
      - **Registers.** Those the driver reads from the hardware (its regmap caches
        the rest). VERSION is 0x23073576, so its idle flags are in STATUS. The flags
        are worked out from XFER; CLR's clears are done at once. FIFO interrupts are
        never raised (a gap here is the emulation's, not the program's).
      - **FIFOs.** 1024 words each way. DMA requests go to the PL330s' request
        inputs, which hold a channel at DMAWFP while high, and are driven again
        after their reset forgets them.
      - **Frame clock.** Its own as master (MCLK / (MDIV + 1) / (FW + 1)), the
        codec's as slave. While a direction runs, it is looked at every millisecond
        of virtual time: TX's frames go to the codec, and RX gets silence.
    - **`nau8822.c`**, the codec: its 9-bit registers over two-byte writes, a reset at
      any write to R0, and the defaults the driver assumes. As master it gives the
      link fs = IMCLK / 256 (MCLK, or its PLL, over MCLKSEL): 48 kHz as Linux sets
      it. Its DAC turns the frames SAI2 sends into QEMU audio at that rate, with the
      DAC's mute, power and volume and the louder of the outputs that are on.
    - **The launcher's `run --audio`.** The sound goes nowhere by default (QEMU's
      `none`, paced on the virtual clock), to a WAV file, or to a driver QEMU has.
      This QEMU has no PipeWire, PulseAudio or ALSA backend; their development
      packages were not installed when it was configured.
    The card is there, the HDMI one stops complaining, and a tone played with aplay
    comes out of the codec as it went in: 5 s of 1 kHz, no sample lost. Three things
    on the way:
    - **QEMU's PL330 runs ahead of its requests.** Its write queue holds 16 store
      instructions of a burst (8 words) each. So the channel can pass DMAWFP up to
      128 words before the stores land in the FIFO and raise its request. With a
      256-word FIFO and requests while two bursts fit, 360 words arrived in one
      gap, and the rest were lost: a 5 s tone played in 2.6 s. Now TX asks while
      256 words are free, and RX while 144 are there.
    - **A frame not yet in the FIFO is late, not silent.** The DMAC's ring always has
      frames (old ones, if the program is late), so a short FIFO means the emulation
      is behind. Those frames go when they come, up to 20 ms late, instead of as
      silence.
    - **The display link slows the sound, and the rest of the machine.** With
      FlipCTL running, the stream ran 15 to 20 % slow; without it, at speed (a period
      in 127 ms, not 125). The guest's DMA interrupts, traced in the guest, came up
      to 270 ms late, on any core, while a frame went to the MCU. That frame goes
      edge by edge through the MCU's PIO (host_spi.c): about 0.45 s of the host's
      time, with the BQL held by the main loop, every time FlipCTL draws (every 1 to
      2 s). Two small things in that path are fixed: the RP2350's interrupt routing
      looked up each core's NVIC input by name at every PIO step, and the PIO set
      its interrupt lines at every step whether they changed or not. The frame
      itself costs as much as before; making it cheaper is a task of its own.
    The SAI's ticks can be logged (`qom-set /machine/rk3576/sai2 verbose on` in QEMU's
    monitor). The Linux check plays a 2 s tone and measures it in the WAV file: 2 s
    of 1 kHz.

66. **The display link, and what held the machine up while FlipCTL drew.** Entry 65
    left the sound 15 to 20 % slow while FlipCTL drew, and blamed the display link.
    - **Where a frame's time went.** Sampled with eu-stack, the main loop spent 84 %
      of a frame running the MCU's PIO up to each of its 600,000 edges: all three
      blocks, twice an edge, a 64-bit division at every PIO step and a clock read
      at every edge. A frame cost 146 ms back to back over the host link, MCU alone.
    - **The wave** (`rp2350_pins_wave()`). The board hands the PIO a burst's lines as
      a function of time: their levels at any moment, and when one next has a
      level. A state machine reads its pins at its own time, and a WAIT for one of
      them goes on at the moment the wave brings it there, so a burst is one run of
      the PIO, not 2,000 of them. It is used only where nothing but the PIO looks at
      those pins (no listener, no interrupt, nothing on the chip driving them);
      otherwise the burst goes edge by edge as before. Also:
      - the cycle's length is worked out once per CLKDIV and clock;
      - a stalled machine looks again only when another changes pins or an IRQ
        flag, or the DMA moves a FIFO (every stalled machine stepped again after
        every step of another);
      - the interrupt lines are worked out when a FIFO or a flag moved.
      A frame now costs 71 ms instead of 146.
    - **A bug on the way, in host_spi.c.** "Active" goes down at t + 1 and the first
      clock comes 2 µs later. But a change behind the virtual clock is made at its
      present, and the bursts run behind it, so the sampler saw "active" a few
      microseconds after t + 1. It then joined the wave 3.7 µs (122 bits) into the
      frame: frames came 9 to 18 bytes short, and the firmware dropped them all
      ("Waiting for CPU screen…"). Edge by edge this never showed, because every
      edge was made at the present too. Now the first clock comes 2 µs after
      "active" really went down, and every frame shows pixel for pixel. The smoke
      test's frame caught it; a benchmark that looked only at its last frame had
      not.
    - **The sound was still behind, for two other reasons.** The SAI's tick log
      showed them:
      - **Three stalls of the main loop, about 50 ms each, in 10 s.** Each came
        just before a frame. Linux's SPI0 sends the 37 KB frame by DMA (DMAC0, a
        byte a beat), and QEMU's PL330 runs every channel until none can go on.
        So the whole transfer ran inside the guest's DMAGO register write, with
        the BQL held. QEMU patch 0012 bounds a run: at most 256 cycles per channel
        at a turn, and one such slice per channel in all. The rest runs from the
        DMAC's timer, at once.
      - **Then the DMA ran dry exactly at period boundaries.** Each dry spell
        started with whole periods of 6000 frames delivered. Linux's pl330 driver
        starts each period's program from its interrupt handler, and during a
        frame that came 13 to 115 ms late. The link's bursts held the BQL for 96 %
        of a frame's span, back to back, and the CPUs' device accesses (an
        interrupt taken among them) hardly got it between two. Now each burst
        waits as long as the last one took, so the link takes at most half of the
        main loop. On the wire, the clock pauses between bursts, as a DMA-fed one
        may.
    - **The result, with FlipCTL drawing.**
      - **The stream is at speed.** A 10 s tone's 480,000 frames went out in 9.995
        to 9.998 s of the SAI's time. Before, it ran 1.3 % slow, with 68 to 113
        ticks at the 20 ms late cap.
      - **Timed in the guest**, aplay's start and drain included, the tone took
        10.4 to 10.5 s, with a key tapped every 0.5 s too (11.4 to 11.9 s before).
      - **A frame now** costs the host about 70 ms with the MCU alone, or 140 ms
        with Flipper OS running, spread over about 300 ms.
      One play in six came out 0.3 s short at its end in the WAV; the others were
      whole. A steady tone cannot show where, since its periods line up with
      the buffer's.

67. **Exception returns without the BQL.** Sampled during entry 66's runs, the
    RK3576's cores waited for the BQL most in `helper_exception_return`. QEMU takes
    it twice at every exception return (each syscall's and interrupt's) to call the
    EL change hooks. On these cores those are the PMU's, for counters that count
    only at some ELs. With no counter enabled in PMCNTENSET they do nothing that
    matters: they set each counter's delta to the present count, and what enables a
    counter is a register write that does the PMU's bookkeeping itself. Linux
    enables none unless perf is used, and Flipper OS's kernel has no hard-lockup
    detector to use one. QEMU patch 0013 skips the hooks, and the lock, when the
    PMU's are the only ones and no counter is enabled, deciding once per return so
    that both hooks run or neither does.
    - **The lock waits.** In 80 samples of every thread during 8 × 200 `date`s at
      once, no core waited for the BQL there any more. What is left is the end of
      a run of guest code (`mttcg_cpu_thread_fn`), a few system registers, and
      device accesses.
    - **The times are loose.** Another job kept about two of this host's cores busy
      meanwhile (load 13), and the times varied widely on one build. The best of
      four rounds went from 14.8 to 6.9 s for 200 `date`s in a row, and from 5.9 to
      3.3 s for 8 × 50 at once. Login from the Linux check's kept card took 81 s
      once and 155 s another time, against 155 to 169 s before.
    The smoke test and the Linux check pass.

68. **The internal microphone.** The sound card's routing has the board's internal
    microphone on the codec's LMICP/LMICN, and the headset's on RMICP. Capture
    recorded silence: SAI2 filled its RX FIFO with zeros. Now:
    - **The SAI's receive side** takes its frames from a source, as the transmit
      side gives them to a sink. It packs them as RXCR says: 16-bit samples two to
      a word, the first in the low half.
    - **The codec's ADC** (`nau8822_adc`) is that source. Its left channel is a WAV
      file (`run --mic FILE.wav`, the codec's "mic" property: 16-bit PCM, its first
      channel, any rate, looped). It is taken as what the ADC gives at the codec's
      defaults: the input PGA at 0 dB, its +20 dB boost, the ADC at 0 dB. On that
      come the PGA's gain, the boost and the ADC's volume as Linux sets them.
      - **Silent** while that path is powered off (R2: the PGA, the boost stage,
        the ADC), not connected (R44), muted, or at volume 0.
      - **Restarted** from the file's beginning whenever the left ADC is powered
        on, so each recording starts there.
      - **The right channel**, the headset's microphone, stays silent.
    arecord in Flipper OS gets the file as it is: a 440 Hz tone at a peak of 0.04999
    (−26 dBFS) records at 439 Hz by sox's estimate and a peak of 0.049988. With
    amixer's PGA at +9 dB the peak is 0.14087, 2.818 times as much. The Linux check
    records 2 s from the file it gives `--mic`: 440 Hz at its level, and nothing on
    the right channel (34 checks).

69. **The headset's microphone, and a boot on a busy PC.**
    - **The headset's microphone.** The jack's headset is a four-pole one, so its
      microphone is there while a headset is in and no button on its cable is
      held (a button shorts or swamps it). The board now tells the codec, and the
      codec's right channel (RMICP) hears the same `--mic` file through the right
      PGA, boost and ADC volume (R46, R48, R16) and R2's right-hand power bits.
      The Linux check plugs a headset in over the panel link and records the
      file's level on the right channel, then pulls it out and records nothing
      (35 checks).
    - **Devices get 5 minutes to show up.** With this PC's other work taking
      over twelve of its sixteen threads (load 17 to 20), a reboot's second boot
      waited for the partitions' `/dev/disk/by-uuid` links. udev's coldplug of
      the emulated board was still running after systemd's 90 s device timeout,
      so `/home`, `/var/log` and `/boot` failed and Flipper OS went into
      emergency mode. The launcher now adds
      `systemd.default_device_timeout_sec=300` to the kernel's command line, as
      it adds the performance governor: a real device's partitions are there in
      seconds, so the longer wait changes nothing there.
    - **DP0's sound card fails.** In the same runs, something in Flipper OS's
      user session kept preparing DP0's card, dozens of times a second, and each
      try failed: "dw-dp 27e40000.dp: unsupported DAI 6", then "ASoC error (-22)
      at snd_soc_dai_prepare() on spdif-hifi". The device tree puts that card on
      the DP codec's S/PDIF DAI (6 is HDMI_SPDIF), which this dw-dp driver does
      not take. What retries it is still to be found.

70. **PCIe0 and the M.2 slot: an NVMe.** Flipper OS's device tree enables PCIe0
    (rockchip,rk3576-pcie: a DesignWare core, one lane) with its PHY on combo PHY 0.
    PCIe1 and both SATA ports are off. A spec came first, read from
    pcie-dw-rockchip, the DesignWare host core and the naneng combo PHY, checked
    against the target kernel's binary. QEMU's DesignWare host (i.MX7's) fits with
    a few things around it:
    - **Rockchip's registers** (PCIE_CLIENT_*, at 0x2a200000):
      - **LTSSM_STATUS** says link up, in L0 (0x00030011), once GENERAL_CON's LTSSM
        enable is set and the slot has a device. An empty slot has no link, and
        Linux then finds an empty bus.
      - **INTx.** The legacy status is INTA-D as QEMU's host raises them, and the
        legacy interrupt (SPI 280) is those not masked. The iMSI-RX goes to SPI 283.
      - **The PHY** and its GRF take writes as unimplemented registers; nothing
        is polled for PCIe.
    - **The core's version register** (0x8F8) says 5.40a. At 0, Linux sets ATU_TD
      in each window's CR1, and QEMU's host then takes its memory windows for
      config windows.
    - **The devices' DMA** reaches DRAM 1:1, as the core passes through what no
      inbound window takes. With no dma-ranges, Linux programs none, and QEMU's
      host had no path to memory at all.
    - **A bug the first boot showed.** QEMU keeps the port-logic registers
      writable, so the iATU's UPPER_LIMIT (0x924) kept the all-ones Linux probes
      it with. The region limit came out as 2^64 − 1, and the size of each window
      (the smaller of that + 1 and the range's) as 0. The MEM window failed with
      −22, and the controller with it. UPPER_LIMIT is now read-only 0: windows of
      up to 4 GiB, which all of Flipper OS's are.
    `run --nvme [FILE]` puts QEMU's NVMe in the slot (`bus=dw-pcie`). Its disk is
    a raw image, by default the device's nvme.img: a blank 256 GB, sparse, the
    first time. Linux finds the root port (16c3:abcd) and the NVMe behind it
    (1b36:0010), and `/dev/nvme0n1` is a 256 GB disk. Its queues take their
    interrupts as MSI-X through the iMSI-RX, and 4 MiB written with O_DIRECT read
    back the same. The Linux check does the same (37 checks).
    - **The Linux check on a busy PC.** With this PC's other work running, Flipper
      OS took 565 s to its login (60 to 160 s on an idle one), and the reboot's
      second login missed its fixed 600 s. The check now times the first boot
      and stretches every later wait by as much as that boot was over 200 s
      (400 s for a first boot): 2.8 times here, and only the console's 60 s
      "shell answers" was still fixed (now stretched too).

71. **The HDMI port: Flipper OS's console on a monitor.** Until now the RK3576's
    display path read as unimplemented registers. rockchipdrm still bound (card1),
    but its IOMMU failed its reset test at every resume ("Error during raw reset"),
    no monitor was ever found, and `fbcon=map:1` sent the console to an fb1 that
    did not exist. A spec came first, read from Flipper's own kernel
    (flipperdevices/flipper-linux-kernel at 08a94c1da821, which carries HDMI 2.1
    changes mainline does not). It found no risk to the panel: FlipCTL picks its
    DRM device by driver name, and rockchipdrm's card number is fixed before any
    display register is read. Now:
    - **`rk_iommu.c`**, Rockchip's IOMMU (v2), for the VOP's (two banks) and the
      video decoder's:
      - DTE_ADDR is kept as written, and a forced reset clears it (the reset
        test checks both).
      - Paging and stall follow their commands at once.
      - Its master's addresses are translated through the two-level table, with
        v2's 40-bit encoding. The decoder's "Enable paging request timed out" is
        gone too.
    - **`rk_vop2.c`**, the VOP2:
      - VERSION_INFO is the RK3576's.
      - REG_CFG_DONE holds a port's "done" until its next frame start, and reads
        back without its write enables (the driver's set-bits skips a write that
        changes nothing).
      - The interrupt registers are write-masked.
      - A video port out of STANDBY runs at 60 frames a second, raising FS and
        FS_FIELD; a STANDBY stops it, with DSP_HOLD_VALID. The tick runs only
        while something waits for it.
      - Video port 0 (HDMI's) is QEMU's first graphic console. It is composed
        only when asked (a screendump): the windows its OVL_LAYER_SEL stacks,
        read through the IOMMU.
    - **`rk_hdmi.c`**, the HDMI transmitter. Its DDC master does one byte per
      operation and gives the monitor's EDID at 0x50, NACKing anything else. The
      EDID is QEMU's, its preferred timing replaced with CTA-861's 720p60.
    - **In `rk3576.c`**:
      - the hotplug line in the IOC GRF (its level, a pending change, mask and
        clear, SPI 367);
      - the HDMI PHY's registers, kept, with its GRF status locked while CON0
        enables its PLL;
      - the VOP's interrupt and its IOMMU's ORed onto SPI 342.
    - **The board.** `run --hdmi` has a monitor in the port from the start; the
      panel link's `hdmi 1|0` plugs one in and out; `shot --hdmi FILE.png` takes
      the picture (QEMU's screendump).
    Three model bugs on the way, each found from what Linux showed:
    - **800x600, not 720p.** QEMU's EDID describes 1280x720 with a timing of its
      own (an odd pixel clock), which the HDMI PHY's rate table does not have.
      Linux left that mode out and took 800x600. The preferred timing is now
      CTA-861's 720p60 (74.25 MHz).
    - **DDC transfers that never started.** After a NACKed transfer the driver
      only soft-resets the I2C master, without clearing its operation bits. The
      model started a transfer only when those bits went from 0 to set, so the
      next one never did. They now clear themselves when a transfer is done, and
      with the soft reset.
    - **255 EDID extensions.** QEMU's generator counts extensions up from what byte
      126 already holds, and the model had filled the buffer with 0xff first. Linux
      then read 255 invalid extension blocks, 32,768 single-byte transfers per
      EDID read. The buffer is zeroed first now.
    With a monitor from the start, Linux finds HDMI-A-1 connected with 1280x720
    first, lights it, and fbcon puts tty1 there as fb1 (rockchipdrmfb). The panel
    stays fb0, and FlipCTL keeps drawing on it. `shot --hdmi` shows Debian's login
    prompt on tty1. The Linux check now has a monitor in from the start and
    checks:
    - the mode, the console's fb1 and the panel's fb0;
    - a picture that is not black;
    - Linux seeing the monitor pulled out and plugged in again.
    All 40 checks pass, the reboot and the power cycle included.

72. **DP0's sound card, corrected.** Entry 69 blamed DP0's failing card ("dw-dp
    27e40000.dp: unsupported DAI 6") on Flipper OS, saying its device tree puts the
    card on an S/PDIF DAI that the DP driver does not take. That was wrong.
    - **The driver does take S/PDIF.** First it reads the controller's CONFIG_REG1,
      whose AUDIO_SELECT (bits 2:1) says which audio interfaces the IP was built
      with. The emulator's DP controller read 0 there, which means "I2S only".
    - **What kept trying it** is WirePlumber, in the user session. As it starts, it
      probes every PCM of every card: 60 prepares of DP0, all refused. DP0 is also
      ALSA's card 0.
    - **The fix.** The DP controller is still unimplemented registers, except
      CONFIG_REG1, which now says I2S and S/PDIF (2). No more errors, neither at
      boot nor at a restart of WirePlumber.
    - **What follows from it.** WirePlumber now lists the card ("DisplayPort Digital
      Output") and makes it the default sink, ahead of the NAU8822's analog output.
      Nothing tells it that no DP monitor is there: simple-card gives the DP codec
      no jack. The device would most likely do the same; that is Flipper OS's
      policy, not the emulator's. The Linux check plays to `hw:NAU8822` directly.

73. **The HDMI picture, live.** `run --vnc PORT` gives QEMU's first graphic console,
    the HDMI port's picture, to a VNC viewer at 127.0.0.1:PORT (this PC's loopback
    only). It is composed when the viewer asks, as for a screenshot. QEMU's VNC
    server needs a keymap. The build made none: its keymaps are targets of their
    own, and only the emulator and qemu-img were built. scripts/setup_qemu.sh now
    builds en-us too, and the launcher says so if it is missing. A minimal RFB
    client gets the handshake, the console's size and a frame: 640x480 and black
    with no signal.

74. **The microSD card, at speed.** Flipper OS now boots with no failed unit. The
    kernel's remaining complaints are ones the device would print too: no GPU
    model, by plan; no UCSI in the MCU firmware, whose registers at 0x0500 are only
    reserved, so flipper-one-typec gives up with -ENODEV; supplies the device
    tree does not name. That left speed. Its root, the microSD card, was the slow
    disk: raw 1 MiB reads at 33 MB/s, against 53 for the UFS and 65 for an NVMe.
    - **Where the time went.** Sampling QEMU during the reads found the vCPUs
      mostly idle, so the cost is per request. Inside the guest, mmc's
      tracepoints time a 64 KiB request at 1.5-2.8 ms from start to done (median).
      Most of that is the guest's own path under emulation. On a loaded host it
      is 7-20 ms, and moving the interrupt to another CPU changes nothing. The
      dw_mmc driver's "Interrupt latency" warnings are the same thing: its
      watchdog finds CMD_DONE latched and the handler more than 10 ms away.
    - **What the model added.** QEMU's card gave CMD18's data one byte per call,
      and took CMD25's the same way. That is a million calls through its checks
      for each megabyte, inside the controller's register write, with the BQL
      held. QEMU patch 0014: a call moves what is left of the block. A block's
      start (its address checked, for a write its protection) and its end (the
      block read or written, the count of blocks, the state) are as they were.
    - **Now:** raw 1 MiB reads at 48 MB/s. 64 MiB written through the root file
      system with fsync goes at 46 MB/s, and reads back with the same checksum
      after the page cache is dropped. Small requests are as before: the guest's
      own path dominates them.
    - **The smoke test.** Its last check read the MCU's log while the firmware was
      starting again, and once took the CPU app's half-written "... but sel" for
      an error it does not know. It now leaves out a last line with no end yet.
      (The MCU firmware here is now dev-25112026-2e3290e4, which passes.)

75. **The boot through U-Boot.** The card image carries the device's boot chain: the
    boot ROM's ID block at sector 64 (TPL with the DDR blob, then SPL), and at sector
    16384 a FIT, u-boot.itb, with TF-A's BL31 in three parts, U-Boot 2026.10 and the
    device trees of both board revisions. `run --uboot` starts the RK3576 at U-Boot
    proper, and the machine does what comes before it, at each start:
    - **SPL's part.** It reads u-boot.itb from the card. It takes the configuration
      whose compatible names the board's revision (the machine's `rev`). It puts
      U-Boot at its load address (0x40800000), and that configuration's device tree
      right after it, where U-Boot looks for its own. TF-A's images it leaves: the
      machine is TF-A (QEMU's PSCI, the SCMI server). A U-Boot written to the card is
      the one the next start runs.
    - **The DDR blob's part.** U-Boot takes the DRAM's size from PMU1_GRF's OS_REG2/3,
      and its banks from an ATAG_DDR_MEM at the top of TF-A's 2 MiB. The PMU GRFs were
      unimplemented: zeros, which read as 256 MiB of DDR4. They now keep what is
      written (their OS_REGs whole), through a reset of the SoC but not a cut of its
      supply. The machine writes both: 4 GiB, one bank.
    - **TF-A's part.** CPU 0 starts at U-Boot's entry at EL2, as BL31 hands over to
      BL33.
    From there U-Boot does what it does on the device. It finds the RK806, the card,
    the Flipper's buttons (through the MCU) and its screen (a 256x144 framebuffer,
    through the display link and the MCU). Its bootcmd scans for boot entries (five
    profiles on the card, two kernels each) and shows `bootflow menu` on the Flipper's
    screen. The menu has no timeout: the pad's keys pick an entry, and leaving the
    menu only runs the scan again. U-Boot loads the kernel, initrd and device tree
    from the btrfs root, and starts Linux with the entry's own options and all of the
    DRAM past TF-A's 2 MiB. Flipper OS reaches its login.
    - **The TRNG.** U-Boot asks its DRNG (CTRL bit 3, the data at 0x70), where Linux
      asks its TRNG (bit 4). The model answered only Linux's, so U-Boot's read timed
      out (-110) and the kernel had no seed for KASLR. It answers both now, and
      acknowledges every request.
    - **The UFS: QEMU patch 0015.** U-Boot scans the UFS LUN by LUN. A command to a
      LUN with no logical unit failed the transfer, 126 errors on one scan, where a
      device answers it in SCSI: INQUIRY with qualifier 011b and type 1Fh, anything
      else LOGICAL UNIT NOT SUPPORTED. And the configuration descriptor, whose length
      U-Boot reads as it starts the host, was not there. Both are answered now.
      Left: "invalid UFS version 0x410", QEMU's UFSHCI 4.1, which this U-Boot does not
      list.
    - **Its keys, slowly.** U-Boot looks at the Flipper's buttons (the MCU's, over
      I2C) between redraws of its screen. A redraw sends the whole framebuffer to the
      MCU over SPI, a word at a time: 57-136 ms here ("cyclic function video_idle
      took too long"), against a few ms on the device. So a short press can fall
      between two looks and be lost, and a longer one be seen as held and repeated:
      one pick of the 7th entry got the 8th. In the window, a key held a little
      longer does it. The check reads the selection off the screen after each key.
    - **Not the emulator's:** "Net: No ethernet found". This U-Boot has no network
      driver. And four of the menu's nine entries, the 6.1.172 kernel's, cannot
      boot: their `vmlinuz` is that kernel's configuration, and their device-tree
      directory has no Flipper One tree ("Bad Linux ARM64 Image magic!"). Read from
      the build's image with `btrfs restore`, outside the emulator, it is the same:
      Flipper OS's OS-03 (docs/FIRMWARE_BUGS.md).
    - **The check.** `scripts/linux_check.py --uboot` covers the boot chain: SPL's
      part, the 4 GiB, the menu on the screen and the keys, the kernel with a KASLR
      seed and the whole DRAM, the login with the entry's own command line, and a
      reboot back through U-Boot's menu to the kernel. It passes, 15 checks
      in about 6 minutes. A computer is on the USB-C port from the start, since the
      boot entry's options mask no getty (OS-02).

76. **Ethernet port 2, on a LAN of its own.** The second port had its PHY but no
    network. QEMU's NICs went to the GMACs in the order of the `-nic` options, so the
    second port could not have one alone. The SoC now also names the ports' NIC
    models, gmac0 and gmac1, so that a NIC can say which port it is for.
    - **`run --lan`** gives port 2 a cable to a socket, devices/NAME/lan.sock: QEMU's
      stream netdev, as its server. (`-nic` cannot name a stream's address, so it is
      `-netdev` and `-net nic,netdev=lan,model=gmac1`.) Whoever connects is at the
      other end: another VM, or scripts/lan_host.py. That is a small host in Python.
      By default it is the LAN's DHCP server and gateway: 192.168.77.1, offering .10.
      With `--client` it is a DHCP client of the Flipper's, for its Router profile.
    - **The cable.** The PHY had a link whenever the port had a backend. A stream's
      backend is there before anyone connects. So the link now also follows the
      backend's own state, which QEMU's stream backend changes as a client comes and
      goes, without telling the NIC. The GMAC looks each second, as a PHY sees a link
      come and go, and its INTB tells Linux.
    - **Flipper OS** (Minimal) takes 192.168.77.10 on end1 by DHCP, with a route
      through 192.168.77.1, beside port 1's passt network. The LAN host's ping is
      answered, and so is the Flipper's ping of it. The Linux check now has port 2
      on that LAN.

77. **The MCU's firmware, updated from Flipper OS.** Flipper OS's kernel has an
    update path for the MCU, in its `flipper-one-fw-update` driver. An image (a UF2,
    or raw) written to the firmware class, `/sys/class/firmware/flipper-one-mcu/`,
    goes to the MCU over the I2C link in 256-byte blocks. Each goes to the window at
    0xf040, with a CRC, commands and a status at 0xf000. The MCU's updater writes it
    to its inactive slot. At Linux's next reboot or power-off, the MCU cuts the
    host's power and asks the boot ROM for a flash-update boot of that slot. The new
    image has to "buy" itself from the boot ROM, its try-before-you-buy, before the
    boot ROM's watchdog runs out, or the boot ROM reverts it.
    - **It works as it is.** The emulator needed no change. Flipper OS fetched the
      MCU's older build (87f6cca3, 1.1 MB) over port 1's network, from this PC's
      loopback through passt's gateway. The upload took 5 minutes, about 3.7 KB/s:
      the I2C link carries it a byte at a time, each an interrupt on either side.
      It was staged in slot B. At the reboot the MCU booted the staged image
      ("Booting the staged image at 0x00782000"), the boot ROM's flash-update boot
      started it, and it bought itself 323 ms in ("Flash-update boot: committing new
      image", "Current FW slot: B"). The MCU then powered Linux again, which read
      slot B. So the real boot ROM's partition table, flash-update boot and explicit
      buy all run on the RP2350's model.
    - **The check.** `scripts/linux_check.py --mcu-update` does it with the MCU's
      own build. It checks the updater's answers, the upload and the staging, the
      boot ROM's switch and the buy, and Flipper OS reading slot B and a
      flash-update boot after it.

78. **U-Boot's menu, lighter; and the Desktop profile.**
    - **The Desktop profile**, U-Boot's first entry, boots in the emulator. SDDM
      starts, and a Plasma Wayland session on the HDMI seat, with no failed unit. The
      login came at 7 minutes on a quiet PC and 13 on a busy one. Ten minutes in,
      its desktop was not on the monitor yet: KWin had set no framebuffer of its
      own, and the DRM state was still the console's (XR24, linear, 1280x720). With
      no GPU, Plasma draws with llvmpipe, whose code TCG then runs: slow enough that
      this waits for a long run on a quiet PC.
    - **U-Boot's keys.** QEMU sampled while U-Boot sits at its boot menu: its core
      waited on the BQL in more than half the samples, inside MMIO accesses, while
      the main loop ran the MCU's PIO capture of the display link's frames. U-Boot
      redraws its unchanging menu five to six times a second. Each frame is 37 KB,
      written a word at a time into SPI0's FIFO, and each was captured whole: read
      from the counters with gdb, 61 frames in 11 s, 4 of them dropped.
    - **The change.** The machine no longer sends a frame that is the same as the
      last one sent within a second: the MCU's screen would not change, and the
      capture is its costliest work. One a second still goes. At the idle menu that
      is one frame sent a second, the other five or six skipped, none dropped, and
      U-Boot draws faster with its core less often waiting. FlipCTL's frames under
      Linux go the same way when they repeat.
    - **And now its keys repeat.** U-Boot, quicker, saw the check's 0.25 s key held
      long enough to repeat it. The check saw the selection reach its entry, pressed
      OK, and booted the next one (one of OS-03's, which does not boot). It now
      holds a key 0.15 s, and presses OK only once the selection has stayed on its
      entry for 1.5 s. The checks pass: the smoke test, the Linux check
      (41, FlipCTL's drawing and its key among them) and the U-Boot one (15).

79. **The MCU's RPC: its screen, streamed.** The firmware's CLI has `rpc`: after a
    ready byte (0xFD) it talks the protocol of flipperdevices/
    flipperone-mcu-protobuf. Messages go each way after their length, a varint. A
    `StartVirtualDisplayRequest` gets a `Frame` for each change of the screen (258x144,
    8-bit grey, sent PLAIN). `ButtonEvent`s and `TouchEvent`s are injected as given:
    a key is PRESS, SHORT and RELEASE, sent by the computer, since the firmware derives
    nothing from them. It is what a computer's app would use, over USB.
    scripts/fo_rpc.py speaks it, with the standard library only: frames to PNGs, keys
    pressed, the session closed.
    - **The stream works.** Over the CLI's UART, at its 115200 baud, a frame takes
      about 3.5 s. Each is the firmware's own canvas, whose columns 1-256 are the
      screen's, pixel for pixel: the smoke test now checks that.
    - **The firmware's FW-02.** A frame drawn while one is still on its way is lost.
      `rpc_screen_fb_callback()` only tries the stream's mutex and returns without a
      copy or a flag, and the thread holds that mutex through a whole send. A
      screen that then stays as it is never reaches the computer. Over the UART
      that is most changes.
    - **Keys over the UART.** Injected while frames go, they came late or not at
      all: Settings, pressed through the RPC, showed its key pressed and opened its
      menu only seconds later, or not in 10 s. The firmware's UART transport moves
      received bytes into its pipe from its interrupt. So where the keys wait (the
      firmware's input or GUI threads, or the RP2350's UART under a steady transmit)
      is not found yet. The smoke test checks the stream alone.
    - **Found on the way:** a session closed while a frame is on its way still
      sends the rest of it before the CLI's prompt comes back. The smoke test waits
      for that prompt.

80. **The Desktop profile's 10-bit framebuffers.**
    - **What the 45-minute run found.** KWin had set its own framebuffers: Cluster0
      at 1280x720, and a 32x32 cursor on Esmart0. Both are AR30 (ARGB2101010):
      KWin takes 10 bits where a plane offers them, as the VOP2's cluster windows do.
      The VOP2 model drew window formats 0-2 only (ARGB8888, RGB888, RGB565), so the
      monitor showed none of it. It now draws format 3 too, the 2101010 kind (AR30
      and XR30, and with red and blue swapped AB30 and XB30), at its top 8 bits.
    - **Plasma itself does not come up here.** plasmashell never finished starting:
      its user service's start timed out each time, about 40 s (27 s of CPU), and
      systemd restarted it 53 times in 45 minutes. kded6's EGL had no GPU to open
      ("failed to get driver name for fd -1"): Mesa draws with llvmpipe, which TCG
      then runs. Under TCG the Desktop profile is past use. That is not the
      emulator's to change: a drop-in with a longer TimeoutStartSec would let it
      come up, slowly.
    - **A check of the formats.** A DRM client in Python (raw ioctls: a dumb buffer,
      ADDFB2, SETCRTC) puts four bands (red, green, blue, 50 % grey) in a given
      format on the HDMI connector's CRTC, and the host samples the picture.
      XR24, AB30 (10-bit, red and blue swapped), RG24: (255,0,0), (0,255,0),
      (0,0,255), (128,128,128). RG16: (248,0,0), (0,252,0), (0,0,248), at 565's
      precision. AR30 and XR30, red and blue in their place: the same four.
    - **Sampled wrong first.** QEMU's screendump PNG filters its rows (Up, Paeth),
      and a reader that skips only the filter byte read the bands as black. The
      Linux check's "not black" counts filtered bytes on purpose: every filter
      makes a black picture all zeros. For colours, a reader has to undo them.
    - **The Linux check** now shows AR30 bands this way, after the console's
      picture (scripts/drm_bands.py, sent in over the console), and reads the
      four colours back with the filters undone. It passes (42 checks).

81. **Fewer cores, quicker.**
    - **What idling costs.** Idle, an MCU-only device costs this PC 0.15 of a core.
      Flipper OS, a minute and a half after its login, costs about 1.2. That was
      measured from QEMU's threads' CPU time over 30 s. Its eight cores each take
      about 100 timer interrupts a second, and IPIs for the scheduler's idle
      balancing and its wakeups on other cores (the IPI tracepoints' stacks:
      `sched_balance_trigger`, `ttwu_queue_wakelist`). That is ordinary SMP Linux,
      but under TCG each interrupt is dear: the guest spends 5 % of its time in hard
      interrupts. FlipCTL redraws about every 1.5 s, each frame new (its power and
      temperatures change).
    - **Linux on four cores** (`maxcpus=4`) reaches its login in 98-104 s, against
      133-170 s with eight, and idles at 0.9 of a core against 1.2-1.3. On two cores
      it is 106 s and 0.8. The eight cost each other more under TCG (the BQL, the
      IPIs) than their parallel work gains them.
    - **`run --linux --cpus N`** gives it, as `maxcpus=N`, for the direct boot only:
      through U-Boot the kernel's options are the boot entry's. The default stays the
      device's eight, and the checks run with eight.

82. **MASKROM_EN: a question for the board.** The MCU's Settings have Maskrom.
    - **What the firmware does.** Its Host service cuts the RK3576's rail. That
      resets the main expander, whose outputs are then written as
      VCC5V0_DEV_S0_EN alone. Then it raises the rail, USB20_SEL and MASKROM_EN
      (pin 15) in one write, and lets MASKROM_EN go a second later.
    - **Modelled as a strap,** the one the boot ROM samples at its reset (the
      RK3576 waiting in its USB download mode, starting nothing), it worked: the
      menu's Maskrom left the RK3576 there, with no kernel.
    - **But the first start is the same.** At the MCU's own boot, the main
      expander's init reads the output register and writes it back with
      VCC5V0_DEV_S0_EN. A PCAL6416 resets that register to all ones, so the rail
      and MASKROM_EN came up together, and the strap model left that first boot in
      MaskROM too. Electrically the two starts are one. On the device either both
      are MaskROM, and Linux would not start after the MCU's reset until a power
      cycle, or neither is, and the menu's Maskrom would do nothing.
    - **Left open.** Which of the two it is, the board's schematic would say. The
      strap is left unmodelled meanwhile: the RK3576 boots on. This is a question
      for the hardware, not a bug recorded.

83. **gdb: the MCU first again, and `./flipperoneemu gdb`.**
    - **The stub's processes.** QEMU's gdb stub has a process for each CPU
      cluster (pid: the cluster's id plus one) and a default one for cores in
      none, and attaches the first by pid when gdb comes.
    - **What went wrong.** Since the RK3576 came, its two clusters (ids 1 and 2)
      were pids 2 and 3. The MCU's Cortex-M33s were in no cluster, so they were
      the default process, pid 4. `run --gdb` and a plain `target remote`
      therefore got the A53s, and with the MCU's ELF loaded gdb gave up with
      "Remote 'g' packet reply is too long".
    - **The fix.** The RP2350's cores are cluster 0 now, which rk3576.c's
      comment had taken for granted, so the MCU is pid 1 and is attached at once.
      The cluster is realized after the cores, as hw/arm/armsse.c does it,
      because an ARMv7M makes its CPU as it is realized. TCG took the cores'
      index then, so their translated code keeps its tag, and only the stub sees
      the change.
    - **The command.** `./flipperoneemu gdb` connects with the firmware's ELF and
      the boot ROM's symbols. `--linux` attaches the RK3576's clusters as
      inferiors 2 and 3, and `--` passes gdb more (`-- -batch -ex bt`). The stub
      now listens on 127.0.0.1 alone; before, it listened on every interface.
    - **Checked.**
      - A plain `target remote` has the two M33s, core 0 in
        vPortSuppressTicksAndSleep (furi_hal_os.c:70).
      - `gdb --linux` has three inferiors, the A53s and A72s with their threads.
      - After gdb detaches, the MCU answers its CLI.

84. **The RPC's keys; a PL011 that stopped the machine; four firmware bugs.**
    - **Keys arrive at once.** Timed against the screen itself (the panel link), not
      the stream: Power and Back by turns on the MCU's desktop, through the RPC.
      Over the UART and over USB, with the screen streamed and without, each took
      effect within its 0.15 s press, ten of ten in each, as long as the program read
      what came back.
    - **Without a reader, nothing at all.** A program that stopped reading its
      socket, as fo_rpc's `--press` does between keys, filled it. QEMU's PL011 hands
      each byte to its chardev with `qemu_chr_fe_write_all()`, which waits for the
      chardev with the BQL held. So at the firmware's next byte the whole machine
      stopped: every CPU and the main loop, until the program read. That was log
      79's "late or not at all", not the firmware.
    - **The fix: QEMU patch 0016.** A PL011 property, `tx-buffer`: what the chardev
      cannot take waits in a buffer (1 MiB for the RP2350's UARTs), and past it is
      lost, with a warning, as on a line that nobody reads. The RK3576's UARTs are
      QEMU's 16550, which waits for its chardev without the BQL: a program that
      stops reading linux.sock still stalls Linux's console, not the machine. That
      one is left open.
    - **USB needs the Linux side off.** While the Linux side has power, the board's
      mux gives the USB-C port's data to the RK3576, so a computer reaches the MCU's
      USB only with the Linux side stopped, as on the device.
    - **FW-03, the close request.** The `rpc` loop's 50 ms "wake-up" never happens:
      `pipe_receive()` returns only with its bytes or a broken pipe. So a close
      request takes effect at the next byte, which is then lost (log 79's garbled
      CLI after a close).
    - **FW-04, input before the session.** Bytes that come with DTR's rise are handled
      before the session exists and left in TinyUSB's FIFO. TinyUSB arms the OUT
      endpoint again only when a whole packet fits, so the port takes nothing more
      until the MCU restarts.
    - **FW-05, output in flight at a close.** A packet in flight when the port closes
      completes when it opens again. If its TxDone is handled before the session
      exists, `is_currently_transmitting` stays set and every later session is mute
      (the smoke test's old note about "a transmission left half-read").
    - **FW-06, a message cut off by the session's end.** It is decoded as an empty
      one and logged as an error. That turned up in the smoke test now and then:
      fo_rpc's byte after a close request came while the request still waited behind
      a stop of the stream, and so went to the RPC as a length.
    - **How they were found.** With `./flipperoneemu gdb` (log 83): TinyUSB's FIFOs
      and endpoint states, the CliVcp's fields (through the CDC driver's
      `cb_ctx[0]`), FreeRTOS's task lists and a task's stack, beside the CLI's `top`.
      For FW-06, a breakpoint at the log line, its Python dumping the bytes the
      decoder had taken, in a smoke test run with its new `--gdb PORT`.
    - **scripts/fo_rpc.py steps round them.** Over USB it waits for the session's
      banner before it writes. Its close stops a stream and reads it to its end,
      then sends the request, a byte half a second on, and reads to the prompt.
    - **The smoke test checks keys now.** In the desktop's Power tab, Down, Up and Back
      through the RPC each change the screen; the menu holds still where the
      desktop's sleeping face moves. On an error in the log, it prints the lines
      before it.

85. **MASKROM_EN after all: the main expander's pulls, its first start, its RESET.**
    - **The answer to log 82.** The firmware does not read the main expander's
      output register at its start. It reads the pins (`read_input() & 0xFF00`)
      and writes back what it finds, so across an MCU restart the expander keeps
      driving what it drove. From a cold start its pins are inputs, and what it
      reads is the board's pulls. The model had all sixteen pulled up, so every
      start found the rails, the USB mux, MASKROM_EN and the GPIO port's power
      on, and took that for a running host. The enables (8-15) must be pulled
      down on the board: otherwise every cold start would power the RK3576 into
      MaskROM. The docs say BOOT0 (MASKROM_EN) "can force the CPU into MaskROM
      mode": it pulls the RK3576's SARADC_VIN0_BOOT to ground, which its boot ROM
      reads at reset.
    - **Modelled so.** The enables are pulled down, the inputs (faults,
      interrupts, power-goods) pulled up. MASKROM_EN is a strap read when the
      RK3576's rail comes up: with it, its boot ROM waits in its USB download
      mode, which is not modelled, and nothing runs.
    - **The emulator's start.** A cold start would leave Flipper OS off until
      someone chose Start Flipper OS in the Power tab (only that menu and the
      firmware's "host is powered" start the CPU app). So the machine starts as
      the device is found in use: Flipper OS on, its MCU starting, as after an
      update. Its main expander's first reset leaves USB20_SEL,
      VCC5V0_DEV_S0_EN and the rail driven, and the firmware takes its "Host is
      powered" path. After the board's power is cut (`power reboot`, ship mode)
      it starts cold. So does a start with BOOTSEL held: a mux left on the Linux
      side would keep the USB-C port from the boot ROM.
    - **A RESET glitch, found on the way.** The firmware's open-drain setup of
      GPIO38 and GPIO43, the expanders' RESET lines, sets OUTOVER low before it
      lets the pin go, so it drives them low for a few instructions. The model
      reset the chips on it at every MCU start: the main one's rails went with it,
      where the firmware has a powered host run on across an MCU restart. The
      all-pulled-up expander had hidden it. Now a low resets them only once it has
      lasted 100 µs; the firmware's own resets hold it 50 ms.
    - **Checked.** Linux boots at the start (the RK3576 at 40 ms). Stop turns it
      off. The MCU's Maskrom leaves the RK3576 in MaskROM, with no kernel, the
      expander going 0x800, 0x9900, 0x1900 a second on. The Maskrom app's Stop,
      then Start Flipper OS, boots Linux again. The smoke test passes, three of
      three; the Linux check (42 of 42), its MCU update from Flipper OS (the MCU
      restarting under a running host) and its U-Boot path pass.
    - **BOOT1, looked into.** The MCU makes it as a 12-bit, 2 kHz PWM on GPIO4,
      eleven ladder levels, 0 V by default; it reaches the RK3576's SARADC
      channel 1. U-Boot's download-key check reads channel 1 of a device named
      `saradc*`, and Flipper's is `adc@2ae00000`, so that check never fires.
      Channel 2 is the board's revision (U-Boot: 0-208 f0b0c1, 209-616
      f0b1c2). The RK3576's SARADC is not modelled yet.

86. **The RK3576's SARADC: the boot strap, BOOT1, the board's revision.**
    - **The ladder.** The docs' "Config Table for SARADC_VIN0_BOOT" (an image in
      the docs repository, read here) has eleven codes: 0, 416, 816, 1231, 1658,
      2048, 2437, 2862, 3279, 3680, 4095. They are exactly the MCU firmware's
      levels for BOOT1 (`saradc_id_pwm`), and the halfway points between them are
      U-Boot's revision ranges (0-208 f0b0c1, 209-616 f0b1c2). It is Rockchip's
      standard resistor ladder.
    - **What the channels read.** Channel 0 is SARADC_VIN0_BOOT: the board's
      divider gives UFS, then the microSD card, then USB (config 8, 2862, the
      order the docs give), and MASKROM_EN (BOOT0) grounds it (config 1, USB,
      the MaskROM). Channel 1 is BOOT1, the MCU's PWM on GPIO4 (12 bits, 2 kHz),
      filtered: its code is the duty, the firmware's levels being the codes
      (its pin is `cpu_adc_in1_boot`). Channel 2 is the revision: 0 for f0b0c1,
      416 for f0b1c2. The rest read 0.
    - **The model.** rk3576_saradc.c is the RK3588's second version, which the
      device tree falls back to. A single conversion through CONV_CON
      (hiword-masked), 20 µs. Then DATA, END_INT_ST (a 1 written clears it) and
      SPI 124 while END_INT_EN allows it: U-Boot polls, Linux takes the interrupt.
    - **What it gives.** U-Boot reads channel 2 and now sets `fdtfile` from the
      board's revision, as on the device (`adc single adc@2ae00000 0`: 2862,
      1.258 V). Flipper OS has the three in sysfs (`in_voltage0_raw` 2862,
      `in_voltage1_raw` 0, `in_voltage2_raw` 0). The MCU's `saradc set_id 6`
      makes channel 1 read 2047, `set_id 11` 4093: the PWM model's duty rounds a
      code or two off.
    - **What it does not.** U-Boot's download-key check reads channel 1 of a
      device named `saradc*`, and Flipper's is `adc@2ae00000`, so the default
      BOOT1 (0 V) does not send it to the MaskROM. Nothing reads BOOT1 yet (the
      docs: it "selects the OS boot source").
    - **Checked.** linux_check reads the three codes, then channel 1 again after
      the MCU's `saradc set_id 6`. The smoke test and linux_check's default and
      U-Boot modes pass.

87. **The Flipper OS installer runs.**
    - **What it is.** The docs install Flipper OS this way: the RK3576 in its
      MaskROM, `rockusb download-boot installer-falcon-loader.bin` from a
      computer, and the installer, a Linux of its own, installs to the UFS or the
      microSD card over Ethernet. The loader carries a FIT for U-Boot's SPL in
      Falcon mode, also published alone as `installer-falcon.itb`. It holds
      TF-A in three parts, the kernel (zstd, at 0x50000000), its ramdisk
      (0x5e000000) and a device tree per revision (0x5c000000), picked by
      `compatible` (`flipper,one-rev-REV`). The tree's `/chosen` has the ramdisk
      and the kernel's arguments, and SPL adds `/memory`.
    - **The machine as SPL in Falcon mode.** `falcon=FILE` (the launcher's `run
      --installer FIT`) does at the RK3576's start what log 75 does for U-Boot:
      the DDR blob's part, then the configuration for the board's revision. The
      kernel is decompressed (libzstd, a dependency QEMU has already), the
      ramdisk and the tree go in place, and the tree gets `/memory` after TF-A's
      2 MiB, as the direct boot has it. The kernel starts at EL2 with the tree's
      address in x0. TF-A's and OP-TEE's parts are left out; PSCI and SCMI are
      the machine's. An installer run needs no OS build: the microSD card is the
      device's own, or blank.
    - **What it does.** The kernel and its BusyBox userland come up in about
      30 s, with DHCP on both ports; with `--net` the first reaches the
      internet through passt. The interface comes up on the Flipper's screen
      ("FlipperOS Installation", "Device type: Flipper One rev. F0B0C1
      [flipper-one]") and as a TUI on the console. It finds the update server
      ("2 target(s), 430 build(s) found", nightly #158), the target (`/dev/sda
      [UFS] 59.6 GiB`, the emulated UFS), and the MCU's firmware (0.1.0, f1),
      which it would update after the system. Its menus answer the Flipper's
      buttons through the MCU.
    - **Where it stops.** "installation failed: /dev/sda is UFS but has no boot
      LU, so the boot ROM would have nothing to boot; reprovision the device
      first". The installer carries a scheme for the UFS (in its strings):
      - LU 0 "system", the rest of the capacity;
      - LU 1 and 2, "u-boot-a" and "u-boot-b": enhanced1, 16 MiB, boot LUs A and
        B, `bBootLunEn` picking one;
      - LU 3 "recovery": enhanced1, 128 MiB;
      - `bBootEnable` on, and WriteBooster if the device offers it.

      It reads and writes the Configuration Descriptor over `/dev/bsg/ufs-bsg`
      and offers "Reprovision UFS". QEMU's UFS has one LU and no boot LU, a
      read-only `bBootLunEn`, and a Configuration Descriptor it only answers
      reads of (patch 0015's).
    - **Next: provisioning.** LUs carved from the device's image by a written
      Configuration Descriptor, their unit descriptors (`bBootLunID`,
      `bMemoryType`), enhanced1 in the Geometry Descriptor, a writable
      `bBootLunEn`, and the boot W-LUN reading the enabled boot LU. Then a boot
      from the UFS: SPL from the boot LU.

88. **Flipper OS installed on the UFS by its installer, and booted from it.**
    - **The UFS provisioned (QEMU patch 0017).**
      - An LU takes how it was provisioned as properties (`boot-lun-id`,
        `memory-type`, `data-reliability`, `provisioning-type`), into its unit
        descriptor.
      - The device takes `boot-enable`, and a drive of its own (`nvram`) for
        `bBootLunEn`, which is now writable and kept across power cycles, as
        the device keeps it.
      - The geometry offers enhanced1, at no capacity cost.
      - The configuration descriptor is unlocked and answers what the LUs are
        (UFS 3.1's layout, four indexes). A write that asks for them as they are
        is taken; any other is refused.
    - **The installer's scheme.** The machine provisions the UFS as the
      installer would on its first install (a factory-fresh one, it does):
      - LU 0, the system's: what the 64 GB leave, in whole 4 MiB allocation
        units (15218);
      - LU 1 and 2: boot LUs A and B, 16 MiB, enhanced1;
      - LU 3: recovery, 128 MiB;
      - each LU its own image; `bBootEnable` on, A first.

      The installer: "ufs provisioning: ok … matches the Flipper scheme".
    - **The install** (with `--net`):
      1. the artifacts verified (sha256), then `blkdiscard` of the system LU and
         a GPT (loader, metadata, root);
      2. U-Boot to the inactive boot LU, B (32 KiB in), then `bBootLunEn`
         switched to B;
      3. the boot menu and recovery images;
      4. `mkfs.btrfs` and a `btrfs receive` of the Minimal profile (924 MiB, at
         about 0.6 MiB/s), and its kernels;
      5. the MCU's firmware (894.5 KiB) over I2C. It was staged, but after the
         installer's 180 s: "the update is still transferring after 180 s … the
         MCU keeps running 0.1.0". The emulated link's 4 KB/s is too slow for
         it, and that is the next thing to make faster.
    - **Booting it.** The machine plays the boot ROM now, in its order
      (SARADC_VIN0_BOOT: UFS, the microSD card, USB): a medium with Rockchip's
      RKNS image 32 KiB in, the UFS read through its enabled boot LU
      (`bBootLunEn`, from the device's own state), then SPL's FIT 8 MiB in.
      - An installer run (`--installer`) starts the installer once, as a
        download into the MaskROM is; a reset boots as the boot ROM does.
      - `run --uboot` without `--linux` boots the device's own storage.
      - The installed device: the boot ROM boots the UFS's boot LU B, and its
        U-Boot (the installed one, of Nov 25) finds the four LUs and LU 0's two
        boot entries, with its menu on the Flipper's screen. Flipper OS
        7.4.0-rc4 comes up to its login with its root on the UFS (`/dev/sda3`,
        btrfs, subvolume @Minimal), and `bBootLunEn` reads 2.
    - **On the way.** The installer's `blkdiscard` left the system LU's image
      fully allocated: QEMU zeroed it, in allocated-but-unwritten extents that
      `fallocate --dig-holes` passes over. The UFS's drives take `discard=unmap`
      and `detect-zeroes=unmap` now.
    - **Checked.** linux_check's UFS check is now the scheme's LUs, by LUN: their
      sectors, their boot LU ids, and `bBootLunEn`. Its U-Boot mode boots the
      microSD card past a blank boot LU. The smoke test passes.

89. **The MCU's I2C link at about its real speed.**
    - **Where the time went.** The installer's MCU update (log 88) needed some
      220 s for its 894.5 KiB, and linux_check's took 252 s. Counters in the
      rk3x model over linux_check's update, per 500 transfers (about 17 s): the
      MCU held SCL for 10 s of them, in some 750,000 tries of a held step, one
      every 2 µs. That polling kept QEMU's main loop waking 44,000 times a
      second, taking the global lock each time. The CPUs the transfer waits on
      (the MCU draining its 16-byte RX FIFO, Linux starting the next count)
      need the same lock for each register they touch. One FIFO took some 4 ms
      to drain.
    - **The slave says when it lets go.** On the bus, a master that a slave
      holds SCL on goes on the moment SCL is let go. Now the MCU's I2C slave
      says so (`rp2350_i2c_target_on_release()`): when a byte is read from its
      RX FIFO, one is written for a read, or its FIFOs are flushed or reset.
      The rk3x model then goes on with the held step at once
      (`rk_i2c_release()`, from the main loop). The 2 µs retry is now a
      fallback, after 100 µs.
    - **Result.** Some 9,000 holds per 500 transfers (one in a FIFO's worth),
      held about 1 s in all. linux_check's MCU update: 93 s, from 252 s. The
      MCU's log puts the transfer itself (erase done to image staged) at 39 s,
      some 11 ms per 256-byte block. On the device, a block takes some 6 ms on
      the bus at 400 kHz, and the driver then polls for RUNNING every 1 ms
      (the updater reports it only once the block is written): some 29 s in
      all. The rest of linux_check's 93 s is its own: sudo, and its look at
      the upload's status every 10 s. Most of the link's time now goes on
      Linux's side, between one count and the next.
    - **Checked.** The smoke test, linux_check and its MCU-update mode (94 s) pass.
    - **And in the installer** (a fresh install of release candidate 0.1.0-rc4,
      Minimal, on a new device's UFS): its MCU step stages 0.16.0 in 55 s, from
      erase done to image staged, inside its 180 s. The installer ends with
      "installation complete".

90. **The boot ROM's USB download: rockusb's download-boot, as the docs install
    Flipper OS.**
    - **What the docs do.** "How to install linux image": the board in its
      MaskROM (on the Flipper One, the MCU's Maskrom), `rockusb list`, and
      `rockusb download-boot installer-falcon-loader.bin`; the installer's
      interface comes up on the screen. The boot ROM's USB download
      (RK3576-Boot-ROM.md): USB0 a device, 2207:350e; vendor requests (0x40,
      0x0c) bring code, 0x471 for SRAM (the DDR blob, which returns to the boot
      ROM) and 0x472 for DRAM (SPL); 4096 bytes at most each, a shorter one
      ending the payload, a CRC-16 (CCITT) at its end, two zero bytes taken
      unchecked. rockusb and rkdeveloptool send it so; rkdeveloptool calls a
      device with bcdUSB's bit 0 clear "Maskrom".
    - **The model** (rk3576_maskrom.c). While the boot ROM waits there, from
      MASKROM_EN at the rail's start or with nothing to boot on the UFS's boot
      LU and the card (`run --uboot` on a new device), the cable reaches it
      through the USB mux, and the computer on the cable enumerates it: a
      Rockusb device in its MaskROM (bcdUSB 2.00, a vendor interface ff/06/05
      with a bulk endpoint each way, no strings). A payload that fails its CRC
      is dropped. One that passes runs, the machine's to say how. 0x471 code:
      the machine does the DDR blob's work (`rk3576_ddr_blob()`), and the boot
      ROM waits for more. 0x472 code: the boot ROM is left. The machine plays
      SPL in its Falcon mode on the FIT that the code carries after SPL's own
      (the installer's loader: 'ddr', 80 KiB; 'spl', 17.4 MiB, its FIT at
      0x31700 with its images), as `--installer` does from a file. Other 0x472
      code (rkdeveloptool's usbplug, SPL with U-Boot) it cannot run, and it
      says so.
    - **The computer's side.** The virtual PC's control transfers now go to
      4096 bytes. `scripts/fo_rockusb.py` speaks rockusb's `list` and
      `download-boot` over USB/IP from userspace: a loader's entries, the 0x471
      ones with their delays, then the 0x472 ones. It waits a moment for a
      refused import: after another client lets go, the computer takes the
      device back with a reset and enumerates it again first (some 120 ms).
      `usbip attach` gives the device to this PC's kernel for the real
      rockusb.
    - **Result.** `download-boot installer-falcon-loader.bin`: 'ddr' (CRC
      good) in 0.1 s, 'spl' in 13 s (its last two bytes zero: the docs'
      self-cancelling CRC, so the boot ROM skips its slow check). SPL in Falcon
      mode starts the installer's kernel. Its interface comes up on the
      Flipper's screen 30 s later, with the UFS (59.4 GiB) as its target, as
      `--installer` has it.
    - **Checked.** `linux_check.py --maskrom [--loader FILE]`: a blank
      device's boot ROM waits in its USB download; the computer enumerates it;
      `fo_rockusb.py list` finds a Rockusb device; 0x471 code with a wrong CRC
      is dropped, and with its CRC runs; then either the loader's download-boot
      brings up the installer, or 0x472 code with no FIT in it is refused.
      Stop, then the MCU's Maskrom (MASKROM_EN), puts it in its USB download
      again (once the CPU app has settled: Stop is its Power menu's). 62 s with
      the installer's loader, 11 s without. The smoke test, linux_check, its
      U-Boot mode (the boot ROM's choice of medium, split from SPL's part) and
      its MCU-update mode pass.

91. **Where an idle Flipper OS's time goes; the GIC's distributor writes.**
    - **Measured.** With Flipper OS at its login and nothing to do, QEMU used
      158 % of a host core: the eight RK3576 vCPUs 9-21 % each, the main loop
      22 %, the MCU's core 0 11 %. The MCU alone (plain `run`) idles at 15 %.
      Inside the guest, over 30 s: FlipCTL takes 12 % of a CPU and nothing else
      much. Per second: 381 arch-timer ticks (25 on each A53, 65-74 on each
      A72), 216 IPIs (169 of them function calls), 84 interrupts of the
      RK3576's timer (the broadcast, for the idle states that stop the local
      timer), 36 of the main bus's I2C, 13 of the SD card.
    - **The vCPUs' time**, from stack samples of QEMU's threads (eu-stack, 200
      rounds): an RK3576 vCPU was busy in 14 % of them. Of that busy time, 27 %
      was waiting for the BQL, 23 % TLB fills (page walks), 12 % MMIO, 11 %
      translation and TB lookups, 8 % helpers, and 8 % running translated
      code. `info jit` shows no TLB flushes in an idle 20 s, and gdb shows the
      kernel's TLBs small (128 or 256 entries) and little used. The misses are
      the guest's own work.
    - **The GIC (QEMU patch 0018).** A vCPU was in the GIC's update in one
      sample round of eight. A distributor write is done a byte at a time, and
      each byte ended with an update that looks at every interrupt (480) for
      every CPU (8), with the BQL held. A 32-bit write, as Linux masks and
      unmasks an interrupt with, made four. Now the bytes are written, then one
      update is made for the access; the interrupt lines come out the same. In
      the update: 13 sample rounds of 200, from 25. Idle: 151 % of a core, from
      158.
    - **What is left** is TCG's price for the guest's own housekeeping, spread
      over the BQL, the TLB, MMIO and translation; no one part of it is large.
      `--cpus 4` idles lighter, as before.
    - **And a check's flake.** linux_check's U-Boot mode once missed the boot
      entry after its reboot. U-Boot warns when its screen's redraw runs past
      its 50 ms budget ("cyclic function video_idle took too long: 56485us vs
      50000us max": here each frame goes through SPI0's FIFO a word at a time),
      and the warning can come in the middle of a line of its bootflow
      listing, an entry's among them. The check now takes it out before it
      reads the listing.
    - **Checked.** The smoke test, linux_check, its U-Boot and MaskROM modes
      pass.

92. **No BQL for an interrupt the CPU cannot take (QEMU patch 0019).**
    - **Counted.** A temporary count in cpu_exec's interrupt check, with
      Flipper OS booting and then idle. All CPUs together, the check took the
      BQL 14,000 to 64,000 times a second, and delivered 1,200 to 3,000
      interrupts. 12,000 to 43,000 times a second it found only an interrupt
      the CPU could not take as it was. A level interrupt stays requested
      while the guest masks it, and until the guest unmasked it, the check
      took the BQL and let it go at each pass of the vCPU's loop.
    - **The patch.** A target may say, from the CPU's own state alone and
      without the BQL, that none of the requested interrupts can be taken;
      then the check takes no BQL. An interrupt that comes meanwhile kicks the
      CPU, so the check is made again. Arm's A profile answers as
      arm_cpu_exec_interrupt() decides (the target EL from SCR and HCR, the
      masks in PSTATE), and leaves it to the BQL's path for anything it does
      not know or for FEAT_NMI's superpriority. The M profile's NVIC is the
      BQL's, so the MCU's cores are as before. EXITTB, HALT, RESET and DEBUG,
      and record and replay, still take the BQL's path.
    - **Result.** Idle: 122 % of a host core, from 151 (158 before log 91):
      the main loop 12 % (from 18-22), the MCU's core 0 7.5 % (from 11). Boot
      to login, one boot: 118 s (129 before). While Flipper OS boots, waiting
      for the BQL stays a quarter of a busy vCPU's time. By where, a third is
      in the vCPU thread's own loop (cpu_exec's returns: halts, kicks, queued
      work, the other CPUs' TLB flushes among it, which run under the BQL).
      About a fifth each is at the interrupt check (interrupts taken), at
      MMIO, and at the generic timer's registers.
    - **Checked.** The smoke test, linux_check (494 s, from 553 with log 91's
      patch and some 650 before it), its U-Boot mode (319 s), its MCU-update
      mode (the update over I2C in 63 s, from 94: the link waits less on the
      BQL too) and its MaskROM mode pass.

93. **The vCPUs' other trips: interrupts without an exit request, the TLB's
    dirty reset, the GIC in one pass (QEMU patches 0020-0022).**
    - **Interrupts without an exit request (0020).** A count of why cpu_exec
      returned, with Flipper OS booting: for an exit request 11,000 to 35,000
      times a second, all CPUs, half of them with no work queued for the
      vCPU. An interrupt that another thread raised (a timer's, a device's,
      another CPU's SGI) kicked the vCPU as a pause does, and each return
      meant the vCPU's thread took the BQL to look at its events. Under
      MTTCG an interrupt now wakes a halted vCPU and sends a running one to
      its interrupt check, as one it raises for itself did; a pause, queued
      work and exclusive sections still exit. Alone it moved the BQL's waits
      rather than shrank them (the BQL is busy: a waiter at one place waits at
      the next).
    - **The TLB's dirty reset (0021).** Each time a page of RAM gets its first
      TB, every vCPU's TLB is searched for entries to mark not dirty, and the
      search went through every mmu_idx (sixteen on Arm). A vCPU was in it in
      one stack sample round of ten while Flipper OS booted. Now only the
      mmu_idx that tlb_c.dirty says may have entries (a guest uses two or
      three): one round of 27.
    - **The GIC in one pass (0022).** With 0018's single update for a
      distributor write, the update itself was still the GIC's most frequent
      place in the samples: for each CPU, a pass over every interrupt (8 of
      480). An interrupt's state is kept per CPU as bit masks, so one pass
      gives each interrupt's candidate CPUs at once; the lowest numbered of a
      CPU's best priority still wins.
    - **Result.** Idle with eight cores: 67 and 82 % of a host core in two
      measurements (one more, 144 %, caught the guest at work), from 122 with
      0019 and 158 at log 91's start; boot to login 73-107 s, from some 129.
      linux_check 421 s (from 494 with 0019, some 650 before log 91), its
      U-Boot mode 261 s (from 319), its MCU-update mode 343 s with the update
      over I2C in 52 s (from 435 and 63), its MaskROM mode 53 s. With
      `--cpus 4`, one boot: login in 70 s, idle 112 %; four cores no longer
      clearly gain on eight.
    - **Checked.** The smoke test, linux_check, its U-Boot, MCU-update and
      MaskROM modes pass.
