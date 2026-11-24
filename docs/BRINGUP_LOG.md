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
