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
