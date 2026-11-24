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
