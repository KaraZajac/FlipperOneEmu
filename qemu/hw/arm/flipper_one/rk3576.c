/*
 * Rockchip RK3576: the Flipper One's application processor, so far as its
 * kernel needs it to start and to speak (the first step of its bring-up)
 *
 *   CPUs      4 Cortex-A53 (MPIDR 0x000-0x003) and 4 Cortex-A72 (0x100-
 *             0x103), each four a cluster of their own; EL2 and no EL3:
 *             PSCI is QEMU's, over SMC, as TF-A's BL31 gives it on the
 *             device (an SMC that PSCI does not know, as SCMI's, answers
 *             NOT_SUPPORTED); the generic timer at 24 MHz, as the SoC's
 *   GIC-400   GICD 0x2a701000, GICC 0x2a702000, GICH 0x2a704000, GICV
 *             0x2a706000; 448 SPIs; the timers on PPIs 13, 14, 11 and 10,
 *             the maintenance interrupt on 9 (the device tree's)
 *   CRU       0x27200000: what is written is kept (HIWORD_UPDATE); each
 *             PLL says it is locked (PLLCON6 bit 15). At reset every PLL
 *             is in slow mode: the clocks are the 24 MHz crystal's until
 *             the kernel sets them (U-Boot and TF-A have, on a device)
 *   TIMER0    0x2acc0000, SPI 45: the kernel's broadcast clockevent (the
 *             cores' own timers stop in their deep idle: local-timer-stop):
 *             a count down from LOAD at clk_timer0's rate, worked out from
 *             the CRU as the kernel works it out (CPLL or GPLL / n, or the
 *             crystal); one-shot or reloading; INT_STATUS, write 1 to clear
 *   PMU       0x27380000: the power domains (the kernel's table for the
 *             RK3576): PWR_CON (0x210, 0x214) asks each on or off, and the
 *             repair and chain status say it is (0x570, 0x248; memory
 *             0x250; the NPU's 0x230); an idle request (0x110, 0x114) is
 *             acknowledged and idle at once (0x120, 0x128)
 *   PMU GRFs  0x26024000, 0x26026000: what is written is kept, through a
 *             reset of the SoC but not a cut of its supply; their OS_REGs
 *             (0x200-0x22c) whole, and PMU0's boot mode (0x40, the device
 *             tree's syscon-reboot-mode: Linux's `reboot maskrom` writes
 *             BOOT_BROM_DOWNLOAD there, which the boot ROM takes at the
 *             reset, rk3576_brom_download()); in PMU1's OS_REG2-3 what the
 *             DDR blob says of the DRAM (rk3576_ddr_blob)
 *   RNG       0x2a410000: the TRNG: a request (CTRL bit 4: Linux's) has 32
 *             bytes ready at once (TRNG_DATA, 0x50; STATE bit 4, write 1 to
 *             clear), and so has one of the DRNG's (bit 3, DRNG_DATA at 0x70:
 *             U-Boot's, the kernel's KASLR seed), from QEMU's guest random
 *             source (-seed makes them the same each run); the other
 *             requests (instantiate, reseed, test) are acknowledged at once
 *   OTP       0x2a580000: the non-secure controller's reads, a word at a
 *             time (AUTO_CTRL's address, AUTO_EN; RD_DONE in INT_ST, write
 *             1 to clear; DOUT0), of the 256 bytes from byte 0x700 that
 *             Linux reads (the secure part below reads 0): the CPU's code
 *             (3576), an ID of 16 bytes ("FlipperOneEmu": Linux derives
 *             the Ethernet ports' addresses from it), the temperature
 *             sensors' trims; what nothing reads is 0
 *   TSADC     rk3576_tsadc.c, 0x2ae70000, SPI 123: six temperatures
 *   SARADC    rk3576_saradc.c, 0x2ae00000, SPI 124: eight channels, the
 *             board's inputs
 *   GPIO0-4   0x27320000, 0x2ae10000-0x2ae40000; SPIs 153, 157, 161, 165,
 *             169: Rockchip's v2 banks (VER_ID 0x010219c8; each register in
 *             two halves with write masks): outputs, inputs from the board
 *             (pulled up unless it says otherwise), interrupts by level or
 *             edge (both edges with BOTHEDGE, whatever INT_TYPE says: a
 *             level there was an interrupt for ever from a card-detect pin
 *             held low), masked, cleared by EOI
 *   DMAC0-2   0x2ab90000, 0x2abb0000, 0x2abd0000: ARM PL330s (QEMU's), their
 *             event and abort interrupts on the first of their two SPIs
 *             (32, 34, 36); only the SAIs hold them back with their
 *             requests, the other peripherals never do. AMBA reads a
 *             PrimeCell's IDs at the end of its region, 16 KiB here: QEMU's
 *             4 KiB of registers are there, and their IDs again at 0x3fe0
 *   SPI0      rk_spi.c, 0x2acf0000, SPI 116: the display link (the board
 *             takes what it sends)
 *   SDMMC     dw_mshc.c, 0x2a310000, SPI 251: the microSD slot's host
 *   USB0-1    rk_dwc3.c, 0x23000000 and 0x23400000, SPIs 261 and 260: the
 *             USB controllers (DWC3: USB0 the USB-C port's, dual-role;
 *             USB1 the host of the board's hub), QEMU's xHCI in each
 *   USBDP     0x2b010000: USB0's USB3/DP combo PHY, its registers kept,
 *             its PLL locked and clock recovered whenever Linux looks
 *   GMAC0-1   rk_gmac.c, 0x2a220000 and 0x2a230000, SPIs 293 and 301: the
 *             Ethernet ports, each with the board's PHY; each takes the next
 *             -nic (its network), or the one whose model is gmac0 or gmac1,
 *             and has no link without one
 *   SAI0/2/6  rk_sai.c, 0x2a600000, 0x2a620000, 0x27d50000, SPIs 187, 189,
 *             193: the serial audio interfaces Flipper OS enables (SAI2:
 *             the codec's I2S link, SAI6: HDMI's); their DMA requests on
 *             DMAC0's 0/1, DMAC1's 0/1, DMAC2's 4/5
 *   VOP2      rk_vop2.c, 0x27d00000, SPIs 342 (with its IOMMU's), 379-381:
 *             the display controller; video port 0 (HDMI's) on a QEMU
 *             console, read through its IOMMU
 *   DP0       0x27e40000: the DP controller as unimplemented registers, but
 *             its CONFIG_REG1 (audio from I2S and S/PDIF)
 *   HDMI      rk_hdmi.c, 0x27da0000, SPI 341: the transmitter, its DDC
 *             master reading the monitor's EDID; the hotplug line in the IOC
 *             GRF (0x2604a400, SPI 367); the HDPTX PHY (0x2b000000) as kept
 *             registers, its GRF (0x26032000) locked while its PLL is on
 *   IOMMUs    rk_iommu.c: the VOP's (0x27d07e00, 0x27d07f00; SPI 342) and
 *             the video decoder's (0x27b00800, 0x27b00900; SPI 309)
 *   PCIe0     0x22000000 (DBI), 0x2a200000 (Rockchip's registers): the M.2
 *             slot's root complex, QEMU's DesignWare host (a viewport iATU,
 *             the iMSI-RX on SPI 283) with Rockchip's link state and INTx
 *             (SPI 280) around it; its devices' DMA reaches DRAM 1:1. A
 *             device in the slot goes on its root port's bus, dw-pcie
 *             (the launcher's --nvme: -device nvme,bus=dw-pcie)
 *   UFS       0x2a2d0000, SPI 361: QEMU's UFS host (UFSHCI 4.0, no MCQ;
 *             QEMU patch 0008 for the UIC commands Rockchip's driver
 *             sends), on a PCI host that the kernel does not see; the
 *             board gives it its logical units
 *   I2C0-8    rk3x_i2c.c: I2C0 at 0x27300000, I2C1-8 from 0x2ac40000 a
 *             0x10000 apart; SPIs 88-96
 *   UART0, UART4  DesignWare APB UARTs, 16550s with 32-bit registers
 *             (reg-shift 2) on a 24 MHz clock: UART0 is the debug port's
 *             console (serial0, the kernel's stdout), UART4 the line to the
 *             MCU (ttyS4); the machine gives them their chardevs
 *   the rest  0x20000000-0x3fffffff: unimplemented, logged (-d unimp),
 *             read as 0
 *
 * DRAM is the machine's, at 0x40000000. With no kernel given the cores
 * stay off (the board's rail for them is the MCU's to switch: not yet).
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/guest-random.h"
#include "hw/core/or-irq.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/gpex.h"
#include "hw/pci-host/designware.h"
#include "hw/pci/pci_bridge.h"
#include "hw/core/boards.h"
#include "rk3576.h"

#define PERIPH_BASE     0x20000000
#define PERIPH_SIZE     0x20000000
#define GIC_BASE        0x2a700000      /* CBAR: the GIC-400's block */
#define GIC_DIST        0x2a701000
#define GIC_CPU         0x2a702000
#define GIC_VIFACE      0x2a704000
#define GIC_VCPU        0x2a706000
#define CRU_BASE        0x27200000
#define CRU_CLKSEL(n)   (0x300 + 4 * (n))   /* CLKSEL_CON(n) */
#define CRU_SIZE        0x50000
#define PMU_BASE        0x27380000
#define PMU_SIZE        0x800
#define PMU0_GRF_BASE   0x26024000
#define PMU1_GRF_BASE   0x26026000
#define PMU_GRF_SIZE    0x1000
#define PMU_GRF_OS_REG  0x200           /* OS_REG0-11: plain 32-bit words */
#define PMU_GRF_OS_END  0x230
#define PMU0_GRF_BOOT_MODE  0x40        /* the reboot mode's word */
#define BOOT_BROM_DOWNLOAD  0xef08a53c  /* ... the boot ROM's USB download */
#define RNG_BASE        0x2a410000
#define RNG_SIZE        0x200
#define OTP_BASE        0x2a580000
#define OTP_SIZE        0x400
#define OTP_NS_WORD     0x1c0           /* the non-secure part's first word */
#define TSADC_BASE      0x2ae70000
#define TSADC_SPI       123
#define SARADC_BASE     0x2ae00000
#define SARADC_SPI      124
#define UFS_BASE        0x2a2d0000
#define UFS_SPI         361
#define GMAC_BASE       0x2a220000      /* GMAC0; GMAC1 0x10000 on */
#define GMAC_SPI        293             /* GMAC0's macirq; GMAC1's 8 on */
#define USB_BASE        0x23000000      /* USB0; USB1 0x400000 on */
#define USB0_SPI        261
#define USB1_SPI        260
#define USBDP_BASE      0x2b010000
#define USBDP_SIZE      0x10000
#define PCIE0_DBI       0x22000000
#define PCIE0_APB       0x2a200000
#define PCIE0_APB_SIZE  0x10000
#define PCIE0_SPI_LEGACY 280
#define PCIE0_SPI_MSI   283
#define VOP_BASE        0x27d00000
#define VOP_SPI_SYS     342
#define VOP_SPI_VP0     379
#define HDMI_BASE       0x27da0000
#define HDMI_SPI_MAIN   341
#define HDMI_SPI_HPD    367
#define HPD_BASE        0x2604a400              /* (in the IOC GRF) */
#define HDPTX_BASE      0x2b000000
#define HDPTX_SIZE      0x2000
#define HDPTX_GRF_BASE  0x26032000
#define DP0_BASE        0x27e40000
#define TIMER0_BASE     0x2acc0000
#define TIMER0_SPI      45
#define UART0_BASE      0x2ad40000
#define UART0_SPI       76
#define UART4_BASE      0x2ad70000
#define UART4_SPI       80
#define UART_BAUDBASE   (24000000 / 16)
#define CNTFRQ_HZ       24000000

/* interrupt IDs, as the GIC numbers its private ones */
#define PPI_MAINTENANCE 25
#define PPI_HYP_TIMER   26
#define PPI_VIRT_TIMER  27
#define PPI_SEC_TIMER   29
#define PPI_PHYS_TIMER  30

static const uint64_t rk3576_mpidr[RK3576_NCPUS] = {
    0x000, 0x001, 0x002, 0x003,         /* the A53s */
    0x100, 0x101, 0x102, 0x103,         /* the A72s */
};

/* ------------------------------------------------- register blocks */

static uint64_t rk_read(void *opaque, hwaddr off, unsigned size)
{
    RkBlock *b = opaque;

    off &= ~3;
    return b->read ? b->read(b, off) : b->regs[off / 4];
}

static void rk_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkBlock *b = opaque;
    uint32_t *r = &b->regs[(off & ~3) / 4];
    uint32_t old = *r;

    if (b->hiword) {
        uint32_t mask = v >> 16;

        *r = (*r & ~mask) | (v & mask);
    } else {
        *r = v;
    }
    if (b->wrote) {
        b->wrote(b, off & ~3, old, v);
    }
}

static const MemoryRegionOps rk_ops = {
    .read = rk_read,
    .write = rk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk_block_init(RK3576State *s, RkBlock *b, const char *name,
                          hwaddr base, uint32_t size, bool hiword)
{
    b->soc = s;
    b->hiword = hiword;
    b->regs = g_new0(uint32_t, size / 4);
    memory_region_init_io(&b->mr, OBJECT(s), &rk_ops, b, name, size);
    memory_region_add_subregion(get_system_memory(), base, &b->mr);
}

/* ------------------------------------------------------ PMU's GRFs */

/* their OS_REGs take the word written whole, and so does PMU0's boot
 * mode; the rest a GRF's halves */
static void pmu_grf_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    if ((off >= PMU_GRF_OS_REG && off < PMU_GRF_OS_END) ||
        (b == &b->soc->pmu0_grf && off == PMU0_GRF_BOOT_MODE)) {
        b->regs[off / 4] = v;
    }
}

bool rk3576_brom_download(RK3576State *s)
{
    uint32_t *mode = &s->pmu0_grf.regs[PMU0_GRF_BOOT_MODE / 4];

    if (*mode != BOOT_BROM_DOWNLOAD) {
        return false;
    }
    *mode = 0;                                  /* (taken: once) */
    return true;
}

void rk3576_brom_download_ask(RK3576State *s)
{
    s->pmu0_grf.regs[PMU0_GRF_BOOT_MODE / 4] = BOOT_BROM_DOWNLOAD;
}

void rk3576_power_lost(RK3576State *s)
{
    memset(s->pmu0_grf.regs, 0, PMU_GRF_SIZE);
    memset(s->pmu1_grf.regs, 0, PMU_GRF_SIZE);
}

/* (U-Boot's js_hash(), over an ATAG but its last word) */
static uint32_t atag_hash(const uint8_t *p, size_t len)
{
    uint32_t h = 0x47c6a7e6;

    for (size_t i = 0; i < len; i++) {
        h ^= (h << 5) + p[i] + (h >> 2);
    }
    return h;
}

/*
 * What Rockchip's DDR blob leaves when it has trained the DRAM:
 *   PMU1_GRF's OS_REG2 and OS_REG3: the DRAM's kind and geometry, as
 *     U-Boot's rockchip_sdram_size() and Linux's DFI read them: LPDDR4X
 *     (type 8: its low bits in OS_REG2, its high one in OS_REG3, whose
 *     words are of the blob's version 3; the docs leave the Flipper One's
 *     kind open, LPDDR4 or 5, and the blob handles both), one channel of
 *     one rank, up to 4 GiB (U-Boot takes 3 GiB at most from it, what is
 *     below 4 GiB: it places itself there);
 *   an ATAG_DDR_MEM at the top of TF-A's 2 MiB (0x401fe000), the DRAM as
 *     one bank: U-Boot gives the kernel all of it from there.
 */
#define ATAG_DDR_MEM    0x54410052
#define ATAG_ADDR       (RK3576_DRAM_BASE + 2 * MiB - 8 * KiB)
#define ATAG_WORDS      48              /* its header, and tag_ddr_mem's 46 */
#define DDR_LPDDR4X     8               /* (Rockchip's types: DDR4 is 0) */
#define DDR_REG_VERSION 3               /* OS_REG3's: the type's high bits */

void rk3576_ddr_words(RK3576State *s, uint64_t size)
{
    uint64_t mb = MIN(size, 4 * GiB) >> 20;
    int n = mb ? 63 - clz64(mb) : 8;            /* log2 of it, rounded down */
    int col = MAX(9, MIN(12, n + 15 - 16));     /* row + col = n + 15 */
    int row = MAX(13, MIN(16, n + 15 - col));   /* (+ bk 3, bw 2; bank
                                                   groups are DDR4's) */

    s->pmu1_grf.regs[(PMU_GRF_OS_REG + 8) / 4] = (DDR_LPDDR4X & 7) << 13 |
                                                  (row - 13) << 6 |
                                                  (col - 9) << 9;
    s->pmu1_grf.regs[(PMU_GRF_OS_REG + 12) / 4] = DDR_REG_VERSION << 28 |
                                                   (DDR_LPDDR4X >> 3) << 12;
}

void rk3576_ddr_blob(RK3576State *s, uint64_t size)
{
    uint32_t tag[ATAG_WORDS] = { 0 };

    rk3576_ddr_words(s, size);
    tag[0] = cpu_to_le32(ATAG_WORDS);
    tag[1] = cpu_to_le32(ATAG_DDR_MEM);
    tag[2] = cpu_to_le32(1);                    /* one bank: ... */
    tag[4] = cpu_to_le32((uint32_t)RK3576_DRAM_BASE);   /* bank[0], its start */
    tag[6] = cpu_to_le32((uint32_t)size);       /* bank[1], its size */
    tag[7] = cpu_to_le32(size >> 32);
    tag[ATAG_WORDS - 1] = cpu_to_le32(atag_hash((uint8_t *)tag,
                                                (ATAG_WORDS - 1) * 4));
    address_space_write(&address_space_memory, ATAG_ADDR,
                        MEMTXATTRS_UNSPECIFIED, tag, sizeof(tag));
    address_space_write(&address_space_memory, ATAG_ADDR + sizeof(tag),
                        MEMTXATTRS_UNSPECIFIED, &(uint32_t){ 0 }, 4);
}

/* -------------------------------------------------------------- CRU */

/* the PLLs' CON0 (RK3588-type: CON6 bit 15 is LOCK_STATUS): BPLL as the
 * kernel names it and as the big cores' CRU has it, LPLL (the little
 * cores', in the CCI's CRU), VPLL, AUPLL, CPLL, GPLL, PPLL */
static const uint32_t cru_plls[] = {
    0x00000, 0x38000, 0x48040, 0x00160, 0x00180, 0x001a0, 0x001c0, 0x08200,
};

static uint32_t cru_read(RkBlock *b, hwaddr off)
{
    uint32_t v = b->regs[off / 4];

    for (int i = 0; i < ARRAY_SIZE(cru_plls); i++) {
        if (off == cru_plls[i] + 0x18) {
            v |= 0x8000;                        /* locked */
        }
    }
    return v;
}

/* a PLL's output (RK3588-type: (24 MHz * (m + k / 65536) / p) >> s in
 * normal mode, the crystal in slow mode); @mode_shift: its MODE_CON0 bits */
static uint64_t cru_pll_hz(RK3576State *s, uint32_t con0, int mode_shift)
{
    uint32_t *r = &s->cru.regs[con0 / 4];
    int mode = (s->cru.regs[0x280 / 4] >> mode_shift) & 3;
    uint64_t m = r[0] & 0x3ff, p = r[1] & 0x3f, k = r[2] & 0xffff;

    if (mode != 1) {
        return mode ? 32768 : 24000000;     /* deep slow, slow */
    }
    if (!p) {
        return 0;
    }
    return ((24000000ULL * m + (24000000ULL * k >> 16)) / p) >>
           ((r[1] >> 6) & 7);
}

/* clk_timer0_root: clk_cpll_div10 (CLKSEL_CON(0): GPLL or CPLL / n) or the
 * crystal (CLKSEL_CON(71) bit 14) */
static uint64_t cru_timer0_hz(RK3576State *s)
{
    uint32_t sel0 = s->cru.regs[0x300 / 4];
    uint64_t pll;

    if (s->cru.regs[0x41c / 4] & (1 << 14)) {
        return 24000000;
    }
    pll = sel0 & (1 << 11) ? cru_pll_hz(s, 0x1a0, 8) : cru_pll_hz(s, 0x1c0, 2);
    return pll / (((sel0 >> 6) & 0x1f) + 1);
}

/* -------------------------------------------------------------- PMU */

/* the power domains: PWR_CON's offset and bit (1: off), and the bit that
 * says it is on in the repair and chain status (the NPU: STATUS, 1: off) */
static const struct {
    uint8_t pwr_off;
    uint32_t pwr, status, on;
} pmu_domains[] = {
    { 0x0, BIT(0),  BIT(0), 0 },            /* npu */
    { 0x0, BIT(6),  0, BIT(6) },            /* nvm */
    { 0x0, BIT(7),  0, BIT(7) },            /* sdgmac */
    { 0x0, BIT(8),  0, BIT(8) },            /* audio */
    { 0x0, BIT(9),  0, BIT(9) },            /* php */
    { 0x0, BIT(10), 0, BIT(10) },           /* subphp */
    { 0x0, BIT(11), 0, BIT(11) },           /* vop */
    { 0x0, BIT(14), 0, BIT(14) },           /* vo1 */
    { 0x0, BIT(15), 0, BIT(15) },           /* vo0 */
    { 0x4, BIT(0),  0, BIT(16) },           /* usb */
    { 0x4, BIT(1),  0, BIT(17) },           /* vi */
    { 0x4, BIT(2),  0, BIT(18) },           /* vepu0 */
    { 0x4, BIT(3),  0, BIT(19) },           /* vepu1 */
    { 0x4, BIT(4),  0, BIT(20) },           /* vdec */
    { 0x4, BIT(5),  0, BIT(21) },           /* vpu */
    { 0x4, BIT(6),  0, BIT(22) },           /* nputop */
    { 0x4, BIT(7),  0, BIT(23) },           /* npu0 */
    { 0x4, BIT(8),  0, BIT(24) },           /* npu1 */
    { 0x4, BIT(9),  0, BIT(25) },           /* gpu */
};

static uint32_t pmu_read(RkBlock *b, hwaddr off)
{
    uint32_t on = 0, off_status = 0;

    for (int i = 0; i < ARRAY_SIZE(pmu_domains); i++) {
        bool is_off = b->regs[(0x210 + pmu_domains[i].pwr_off) / 4] &
                      pmu_domains[i].pwr;

        if (is_off) {
            off_status |= pmu_domains[i].status;
        } else {
            on |= pmu_domains[i].on;
        }
    }
    switch (off) {
    case 0x230: return off_status;                  /* STATUS: 1, off */
    case 0x248: case 0x570: return on;              /* chain, repair */
    case 0x250: return ~on & 0x3ffffc0;             /* memory: 1, off */
    case 0x120: case 0x128:                         /* ACK, IDLE */
        return (b->regs[0x110 / 4] & 0xffff) |
               (b->regs[0x114 / 4] & 0xffff) << 16;
    }
    return b->regs[off / 4];
}

/* -------------------------------------------------------------- RNG */

static void rng_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    if (off == 0x10) {                              /* CTRL (hiword) */
        uint32_t mask = v >> 16;

        b->regs[off / 4] = (old & ~mask) | (v & mask);
        if (v & mask & BIT(4)) {                    /* SW_TRNG_REQ: Linux's */
            qemu_guest_getrandom_nofail(&b->regs[0x50 / 4], 32);
        }
        if (v & mask & BIT(3)) {                    /* SW_DRNG_REQ: U-Boot's */
            qemu_guest_getrandom_nofail(&b->regs[0x70 / 4], 32);
        }
        b->regs[0x14 / 4] |= v & mask & 0x1f;       /* each one's ACK (an
                                                       instantiation, reseed
                                                       or test too) */
    } else if (off == 0x14) {                       /* STATE: W1C */
        b->regs[off / 4] = old & ~v;
    }
}

/* -------------------------------------------------------------- OTP */

#define OTP_AUTO_CTRL   0x04            /* the word's address in 31:16 */
#define OTP_AUTO_EN     0x08
#define OTP_DOUT0       0x20
#define OTP_INT_ST      0x84
#define OTP_RD_DONE     BIT(1)

static void otp_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    RK3576State *s = b->soc;

    if (off == OTP_AUTO_EN && v & 1) {              /* a read: done at once */
        uint32_t word = (b->regs[OTP_AUTO_CTRL / 4] >> 16) - OTP_NS_WORD;

        b->regs[OTP_DOUT0 / 4] = word < sizeof(s->otp_ns) / 4 ?
                                 ldl_le_p(s->otp_ns + word * 4) : 0;
        b->regs[OTP_AUTO_EN / 4] = 0;
        b->regs[OTP_INT_ST / 4] |= OTP_RD_DONE;
    } else if (off == OTP_INT_ST) {
        b->regs[off / 4] = old & ~v;
    }
}

/* what the OTP's non-secure part holds, as the device tree's cells name
 * it (rk3576.dtsi) */
static void otp_init(RK3576State *s)
{
    static const char id[16] = "FlipperOneEmu";

    s->otp_ns[0x02] = 0x35;                         /* cpu-code: 3576 */
    s->otp_ns[0x03] = 0x76;
    memcpy(s->otp_ns + 0x0a, id, sizeof(id));       /* id */
    rk_tsadc_otp_trims(s->otp_ns);
}

/* ------------------------------------------------------------ USBDP */

/* the USB3/DP combo PHY's PMA (at 0x8000): its PLL locked and its clock
 * recovered as soon as Linux looks (it polls them at each init) */
static uint32_t usbdp_read(RkBlock *b, hwaddr off)
{
    uint32_t v = b->regs[off / 4];

    switch (off) {
    case 0x8350:
        return v | 0xc0;                        /* LCPLL_LOCK_DONE, AFC_DONE */
    case 0x8b84:
    case 0x9b84:
        return v | 1;                           /* CDR locked (lane 0, 1) */
    }
    return v;
}

/* -------------------------------------------------------------- UFS */

/* the UFS host moves its data itself: bus mastering on its PCI side (a
 * reset clears it, as any PCI device's) */
static void ufs_master(RK3576State *s)
{
    pci_default_write_config(s->ufs, PCI_COMMAND, PCI_COMMAND_MASTER, 2);
}

BusState *rk3576_ufs_bus(RK3576State *s)
{
    return QLIST_FIRST(&DEVICE(s->ufs)->child_bus);
}

/* ------------------------------------------------------------ PCIe0 */

/* whatever is in the M.2 slot, on the root port's bus */
static bool pcie_has_device(RK3576State *s)
{
    DesignwarePCIEHost *h = DESIGNWARE_PCIE_HOST(s->pcie);
    PCIBus *sec = pci_bridge_get_sec_bus(PCI_BRIDGE(&h->root));

    return !QTAILQ_EMPTY(&BUS(sec)->children);
}

/* the legacy interrupt: INTA-D as the host raises them, less those masked
 * (PCIE_CLIENT_INTR_MASK_LEGACY) */
static void pcie_legacy_update(RK3576State *s)
{
    qemu_set_irq(rk3576_spi(s, PCIE0_SPI_LEGACY),
                 (s->pcie_intx & ~s->pcie_apb.regs[0x01c / 4] & 0xf) != 0);
}

static void pcie_intx(void *opaque, int n, int level)
{
    RK3576State *s = opaque;

    s->pcie_intx = deposit32(s->pcie_intx, n, 1, level);
    pcie_legacy_update(s);
}

static uint32_t pcie_apb_read(RkBlock *b, hwaddr off)
{
    RK3576State *s = b->soc;

    switch (off) {
    case 0x008:                         /* INTR_STATUS_LEGACY */
        return s->pcie_intx;
    case 0x010:                         /* INTR_STATUS_MISC: nothing */
    case 0x154:                         /* (RASDES: no L1 substates) */
        return 0;
    case 0x300:                         /* LTSSM_STATUS */
        /* the link up and in L0 once the LTSSM runs (GENERAL_CON's bit 2),
         * if the slot has a device: an empty slot has no link */
        return (b->regs[0] & BIT(2)) && pcie_has_device(s) ? 0x00030011 : 0;
    default:
        return b->regs[off / 4];
    }
}

static void pcie_apb_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    if (off == 0x01c) {
        pcie_legacy_update(b->soc);
    }
}

/* --------------------------------------------------------- the display */

/* the HDMI's hotplug line, in the IOC GRF (MISC_CON0 at +0x00: bit 1
 * clears its pending change, bit 2 masks it; HDMI_HPD_STATUS at +0x40:
 * bit 3 the level, bit 4 a change pending) */
static void hpd_irq_update(RK3576State *s)
{
    qemu_set_irq(rk3576_spi(s, HDMI_SPI_HPD),
                 s->hpd_pending && !(s->hpd.regs[0] & BIT(2)));
}

static uint32_t hpd_read(RkBlock *b, hwaddr off)
{
    RK3576State *s = b->soc;

    if (off == 0x40) {
        return (s->hpd_level ? BIT(3) : 0) | (s->hpd_pending ? BIT(4) : 0);
    }
    return b->regs[off / 4];
}

static void hpd_wrote(RkBlock *b, hwaddr off, uint32_t old, uint32_t v)
{
    RK3576State *s = b->soc;

    if (off == 0 && (v & BIT(1)) && (v & BIT(17))) {
        s->hpd_pending = false;         /* (HPD_INT_CLR clears itself) */
        b->regs[0] &= ~BIT(1);
    }
    hpd_irq_update(s);
}

void rk3576_hdmi_plug(RK3576State *s, bool on)
{
    rk_hdmi_plug(&s->hdmi, on);
    if (on != s->hpd_level) {
        s->hpd_level = on;
        s->hpd_pending = true;
        hpd_irq_update(s);
    }
}

/* the HDMI PHY's GRF: its STATUS (+0x80) has the PHY ready, its clock
 * ready and its PLL locked while CON0 enables the PLL (bit 7) */
static uint32_t hdptx_grf_read(RkBlock *b, hwaddr off)
{
    if (off == 0x80) {
        return b->regs[0] & BIT(7) ? 0xf : 0;
    }
    return b->regs[off / 4];
}

/* the DP controller (DW DP), as unimplemented registers but one: its
 * CONFIG_REG1 says what it was built with, audio from I2S and S/PDIF
 * (AUDIO_SELECT, bits 2:1, = 2); Linux checks that before it takes the
 * DP0 sound card's S/PDIF */
static uint32_t dp_read(RkBlock *b, hwaddr off)
{
    return off == 0x100 ? 2 << 1 : 0;
}

/* ------------------------------------------------------------ TIMER0 */

#define TMR_ENABLE      0x1
#define TMR_ONESHOT     0x2                     /* USER_DEFINED_COUNT */
#define TMR_INT_UNMASK  0x4

static uint64_t tmr_load(RkTimer *t)
{
    return (uint64_t)t->load[1] << 32 | t->load[0];
}

static int64_t tmr_period_ns(RkTimer *t)
{
    return muldiv64(tmr_load(t) + 1, NANOSECONDS_PER_SECOND, t->hz);
}

static void tmr_irq(RkTimer *t)
{
    qemu_set_irq(t->irq, (t->status & 1) && (t->ctrl & TMR_INT_UNMASK));
}

static void tmr_expired(void *opaque)
{
    RkTimer *t = opaque;

    t->status |= 1;
    tmr_irq(t);
    if (t->ctrl & TMR_ONESHOT) {
        t->done = true;
    } else {
        t->start_ns += tmr_period_ns(t);        /* reloaded */
        timer_mod(t->timer, t->start_ns + tmr_period_ns(t));
    }
}

/* what is left of the count */
static uint64_t tmr_value(RkTimer *t)
{
    uint64_t gone;

    if (!(t->ctrl & TMR_ENABLE)) {
        return tmr_load(t);
    }
    if (t->done) {
        return 0;
    }
    gone = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->start_ns,
                    t->hz, NANOSECONDS_PER_SECOND);
    return gone > tmr_load(t) ? 0 : tmr_load(t) - gone;
}

static uint64_t tmr_read(void *opaque, hwaddr off, unsigned size)
{
    RkTimer *t = opaque;

    switch (off) {
    case 0x00: case 0x04: return t->load[off / 4];
    case 0x08: return tmr_value(t);
    case 0x0c: return tmr_value(t) >> 32;
    case 0x10: return t->ctrl;
    case 0x18: return t->status;
    }
    return 0;
}

static void tmr_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkTimer *t = opaque;

    switch (off) {
    case 0x00: case 0x04:
        t->load[off / 4] = v;
        break;
    case 0x10:
        t->ctrl = v & 7;
        timer_del(t->timer);
        t->done = false;
        if (t->ctrl & TMR_ENABLE) {
            t->hz = cru_timer0_hz(t->soc) ?: 24000000;
            t->start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(t->timer, t->start_ns + tmr_period_ns(t));
        }
        tmr_irq(t);
        break;
    case 0x18:
        t->status &= ~v;
        tmr_irq(t);
        break;
    }
}

static const MemoryRegionOps tmr_ops = {
    .read = tmr_read,
    .write = tmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------ GPIO */

static const hwaddr gpio_base[RK3576_NGPIO] = {
    0x27320000, 0x2ae10000, 0x2ae20000, 0x2ae30000, 0x2ae40000,
};
static const int gpio_spi[RK3576_NGPIO] = { 153, 157, 161, 165, 169 };

/* the pins' levels now: the latched edges, the levels as they are, the
 * interrupt */
static void gpio_update(RkGpio *g)
{
    uint32_t level = (g->dr & g->ddr) | (g->in & ~g->ddr);
    uint32_t changed = level ^ g->level;
    uint32_t rose = level & ~g->level, fell = ~level & g->level;
    uint32_t edges = (rose & (g->pol | g->both)) | (fell & (~g->pol | g->both));
    uint32_t levels = ~(level ^ g->pol);        /* at its active level */
    uint32_t edge = g->type | g->both;          /* BOTHEDGE: edges, whatever
                                                   INT_TYPE says */

    g->level = level;
    g->raw = (g->raw & edge) | (edges & edge) | (levels & ~edge);
    g->raw &= g->en;
    qemu_set_irq(g->irq, (g->raw & ~g->mask) != 0);
    for (int pin = 0; changed && g->soc && g->soc->gpio_out; pin++) {
        if (changed & BIT(pin)) {
            changed &= ~BIT(pin);
            g->soc->gpio_out(g->soc->gpio_out_opaque, g->bank, pin,
                             level & BIT(pin));
        }
    }
}

static uint32_t *gpio_reg(RkGpio *g, hwaddr off)
{
    switch (off & ~7) {
    case 0x00: return &g->dr;
    case 0x08: return &g->ddr;
    case 0x10: return &g->en;
    case 0x18: return &g->mask;
    case 0x20: return &g->type;
    case 0x28: return &g->pol;
    case 0x30: return &g->both;
    case 0x38: return &g->debounce;
    case 0x40: return &g->dbclk_en;
    }
    return NULL;
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    RkGpio *g = opaque;
    uint32_t *r = gpio_reg(g, off);

    if (r) {                                    /* a half */
        return off & 4 ? *r >> 16 : *r & 0xffff;
    }
    switch (off) {
    case 0x48: return g->dbclk_con;
    case 0x50: return g->raw & ~g->mask;        /* INT_STATUS */
    case 0x58: return g->raw;                   /* INT_RAWSTATUS */
    case 0x70: return g->level;                 /* EXT_PORT */
    case 0x78: return 0x010219c8;               /* VER_ID: v2 */
    }
    return 0;
}

static void gpio_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkGpio *g = opaque;
    uint32_t *r = gpio_reg(g, off);
    int shift = off & 4 ? 16 : 0;
    uint32_t mask = (v >> 16) << shift;

    if (r) {
        *r = (*r & ~mask) | ((v & 0xffff) << shift & mask);
    } else if ((off & ~4) == 0x60) {            /* PORT_EOI: edges done */
        g->raw &= ~((v & 0xffff) << shift & mask);
    } else if (off == 0x48) {
        g->dbclk_con = v;
    }
    gpio_update(g);
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void rk3576_gpio_in(RK3576State *s, int bank, int pin, bool level)
{
    RkGpio *g = &s->gpio[bank];

    g->in = deposit32(g->in, pin, 1, level);
    gpio_update(g);
}

/* ------------------------------------------------------------ the SoC */

void rk3576_reset_devices(RK3576State *s)
{
    device_cold_reset(DEVICE(&s->gic));
    for (int i = 0; i < 2; i++) {
        device_cold_reset(s->uart[i]);
    }
    for (int d = 0; d < 3; d++) {
        device_cold_reset(s->dmac[d]);
    }
    for (int i = 0; i < RK3576_NSAI; i++) {    /* (after the DMACs, whose
                                                   reset forgets their
                                                   requests) */
        device_cold_reset(DEVICE(&s->sai[i]));
    }
    /* (I2C2's bus is the main board's: what is on it keeps its power) */
    for (int n = 0; n < RK3576_NI2C; n++) {
        device_cold_reset(DEVICE(&s->i2c[n]));
    }
    device_cold_reset(DEVICE(&s->spi0));
    device_cold_reset(DEVICE(&s->sdmmc));       /* and its card */
    for (int n = 0; n < 2; n++) {
        device_cold_reset(DEVICE(&s->gmac[n]));     /* (and its PHY) */
        device_cold_reset(DEVICE(&s->usb[n]));
        device_cold_reset(DEVICE(&s->usb[n].xhci));
    }
    memset(s->usbdp.regs, 0, USBDP_SIZE);
    device_cold_reset(s->pcie);                 /* (and what is in its slot) */
    device_cold_reset(DEVICE(&s->vop_mmu));
    device_cold_reset(DEVICE(&s->vop));
    device_cold_reset(DEVICE(&s->hdmi));        /* (the monitor stays) */
    memset(s->hpd.regs, 0, 0x100);
    s->hpd_pending = false;
    hpd_irq_update(s);
    memset(s->hdptx.regs, 0, HDPTX_SIZE);
    memset(s->hdptx_grf.regs, 0, 0x100);
    device_cold_reset(DEVICE(&s->vdec_mmu));
    memset(s->pcie_apb.regs, 0, PCIE0_APB_SIZE);
    pcie_legacy_update(s);
    device_cold_reset(s->ufs_pcie);             /* (the UFS host's PCI side:
                                                   the kernel resets the
                                                   host as it starts it) */
    ufs_master(s);
    memset(s->cru.regs, 0, CRU_SIZE);
    /* the GMACs' 125 MHz sources (stmmac's CSR clock) as the device has
     * them: CPLL / 8 (CLKSEL_CON(30) bits 14:10, CLKSEL_CON(31) bits 4:0),
     * once the kernel's tree has CPLL at 1 GHz; nothing in Linux sets
     * them, and at 0 they were CPLL whole, past what stmmac's MDC
     * dividers take ("clk_csr value out of range") */
    s->cru.regs[CRU_CLKSEL(30) / 4] = 7 << 10;
    s->cru.regs[CRU_CLKSEL(31) / 4] = 7;
    memset(s->pmu.regs, 0, PMU_SIZE);
    memset(s->rng.regs, 0, RNG_SIZE);
    memset(s->otp.regs, 0, OTP_SIZE);
    rk_tsadc_reset(&s->tsadc);
    rk_saradc_reset(&s->saradc);
    for (int b = 0; b < RK3576_NGPIO; b++) {
        RkGpio *g = &s->gpio[b];

        g->dr = g->ddr = g->en = g->mask = g->type = g->pol = g->both = 0;
        g->raw = g->debounce = g->dbclk_en = g->dbclk_con = 0;
        gpio_update(g);                         /* (the board's inputs stay) */
    }
    timer_del(s->timer0.timer);
    s->timer0.load[0] = s->timer0.load[1] = 0;
    s->timer0.ctrl = s->timer0.status = 0;
    s->timer0.done = false;
    tmr_irq(&s->timer0);
}

I2CBus *rk3576_i2c_bus(RK3576State *s, int n)
{
    return s->i2c[n].bus;
}

/* the SAIs Flipper OS enables: where, their SPI, their DMAC and its
 * requests for TX and RX */
static const struct {
    int n;
    hwaddr base;
    int spi, dmac, tx, rx;
} rk3576_sais[RK3576_NSAI] = {
    { 0, 0x2a600000, 187, 0, 0, 1 },
    { 2, 0x2a620000, 189, 1, 0, 1 },    /* the codec's */
    { 6, 0x27d50000, 193, 2, 4, 5 },    /* HDMI's */
};

RkSaiState *rk3576_sai(RK3576State *s, int n)
{
    for (int i = 0; i < RK3576_NSAI; i++) {
        if (rk3576_sais[i].n == n) {
            return &s->sai[i];
        }
    }
    g_assert_not_reached();
}

void rk3576_i2c_target(RK3576State *s, int n, const RkI2cTarget *t)
{
    s->i2c[n].target = t;
}

void rk3576_i2c_arbiter(RK3576State *s, int n,
                        bool (*claim)(void *opaque, bool claim),
                        void *opaque)
{
    s->i2c[n].arb = claim;
    s->i2c[n].arb_opaque = opaque;
}

qemu_irq rk3576_spi(RK3576State *s, int spi)
{
    return qdev_get_gpio_in(DEVICE(&s->gic), spi);
}

static qemu_irq rk3576_ppi(RK3576State *s, int cpu, int id)
{
    return qdev_get_gpio_in(DEVICE(&s->gic),
                            RK3576_GIC_SPIS + cpu * GIC_INTERNAL + id);
}

static void rk3576_init(Object *obj)
{
    RK3576State *s = RK3576(obj);

    for (int c = 0; c < 2; c++) {
        object_initialize_child(obj, c ? "cluster-big" : "cluster-little",
                                &s->cluster[c], TYPE_CPU_CLUSTER);
        /* the RP2350's cores are cluster 0 */
        qdev_prop_set_uint32(DEVICE(&s->cluster[c]), "cluster-id", 1 + c);
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        int c = i / 4;

        object_initialize_child(OBJECT(&s->cluster[c]), "cpu[*]", &s->cpu[i],
                                c ? ARM_CPU_TYPE_NAME("cortex-a72")
                                  : ARM_CPU_TYPE_NAME("cortex-a53"));
    }
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);
    object_initialize_child(obj, "spi0", &s->spi0, TYPE_RK_SPI);
    object_initialize_child(obj, "sdmmc", &s->sdmmc, TYPE_DW_MSHC);
    for (int n = 0; n < 2; n++) {
        g_autofree char *name = g_strdup_printf("gmac%d", n);
        g_autofree char *usb = g_strdup_printf("usb%d", n);

        object_initialize_child(obj, name, &s->gmac[n], TYPE_RK_GMAC);
        object_initialize_child(obj, usb, &s->usb[n], TYPE_RK_DWC3);
    }
    for (int n = 0; n < RK3576_NI2C; n++) {
        g_autofree char *name = g_strdup_printf("i2c%d", n);

        object_initialize_child(obj, name, &s->i2c[n], TYPE_RK_I2C);
    }
    for (int i = 0; i < RK3576_NSAI; i++) {
        g_autofree char *name = g_strdup_printf("sai%d", rk3576_sais[i].n);

        object_initialize_child(obj, name, &s->sai[i], TYPE_RK_SAI);
    }
    qdev_init_gpio_in_named(DEVICE(obj), pcie_intx, "pcie0-intx", 4);
    object_initialize_child(obj, "vop", &s->vop, TYPE_RK_VOP2);
    object_initialize_child(obj, "hdmi", &s->hdmi, TYPE_RK_HDMI);
    object_initialize_child(obj, "vop-mmu", &s->vop_mmu, TYPE_RK_IOMMU);
    object_initialize_child(obj, "vdec-mmu", &s->vdec_mmu, TYPE_RK_IOMMU);
}

static void rk3576_realize(DeviceState *dev, Error **errp)
{
    RK3576State *s = RK3576(dev);
    SysBusDevice *gic = SYS_BUS_DEVICE(&s->gic);
    MemoryRegion *sysmem = get_system_memory();

    for (int c = 0; c < 2; c++) {
        if (!qdev_realize(DEVICE(&s->cluster[c]), NULL, errp)) {
            return;
        }
    }
    for (int i = 0; i < RK3576_NCPUS; i++) {
        Object *cpu = OBJECT(&s->cpu[i]);

        object_property_set_int(cpu, "mp-affinity", rk3576_mpidr[i],
                                &error_abort);
        object_property_set_bool(cpu, "has_el3", false, &error_abort);
        object_property_set_bool(cpu, "has_el2", true, &error_abort);
        object_property_set_int(cpu, "reset-cbar", GIC_BASE, &error_abort);
        object_property_set_int(cpu, "core-count", 4, &error_abort);
        object_property_set_uint(cpu, "cntfrq", CNTFRQ_HZ, &error_abort);
        /* KPTI's flips of TTBR1 keep the TLB (patch 0024): Linux's, under
           U-Boot's KASLR as on the device */
        object_property_set_bool(cpu, "x-kpti-asid-pairs", true,
                                 &error_abort);
        object_property_set_bool(cpu, "start-powered-off",
                                 i || !s->start_on, &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    /* the GIC-400: its CPU interfaces are these eight, from the first */
    qdev_prop_set_uint32(DEVICE(gic), "revision", 2);
    qdev_prop_set_uint32(DEVICE(gic), "num-cpu", RK3576_NCPUS);
    qdev_prop_set_uint32(DEVICE(gic), "first-cpu-index",
                         CPU(&s->cpu[0])->cpu_index);
    qdev_prop_set_uint32(DEVICE(gic), "num-irq",
                         RK3576_GIC_SPIS + GIC_INTERNAL);
    qdev_prop_set_bit(DEVICE(gic), "has-security-extensions", false);
    qdev_prop_set_bit(DEVICE(gic), "has-virtualization-extensions", true);
    if (!sysbus_realize(gic, errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, GIC_DIST,
                                sysbus_mmio_get_region(gic, 0));
    memory_region_add_subregion(sysmem, GIC_CPU,
                                sysbus_mmio_get_region(gic, 1));
    memory_region_add_subregion(sysmem, GIC_VIFACE,
                                sysbus_mmio_get_region(gic, 2));
    memory_region_add_subregion(sysmem, GIC_VCPU,
                                sysbus_mmio_get_region(gic, 3));
    for (int i = 0; i < RK3576_NCPUS; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);

        sysbus_connect_irq(gic, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gic, i + RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gic, i + 2 * RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gic, i + 3 * RK3576_NCPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
        sysbus_connect_irq(gic, i + 4 * RK3576_NCPUS,
                           rk3576_ppi(s, i, PPI_MAINTENANCE));
        qdev_connect_gpio_out(cpu, GTIMER_PHYS,
                              rk3576_ppi(s, i, PPI_PHYS_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_VIRT,
                              rk3576_ppi(s, i, PPI_VIRT_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_HYP,
                              rk3576_ppi(s, i, PPI_HYP_TIMER));
        qdev_connect_gpio_out(cpu, GTIMER_SEC,
                              rk3576_ppi(s, i, PPI_SEC_TIMER));
    }

    /* the console (serial port 2) and the MCU's line (serial port 3) */
    s->uart[0] = DEVICE(serial_mm_init(sysmem, UART0_BASE, 2,
                                       rk3576_spi(s, UART0_SPI),
                                       UART_BAUDBASE, serial_hd(2),
                                       DEVICE_LITTLE_ENDIAN));
    s->uart[1] = DEVICE(serial_mm_init(sysmem, UART4_BASE, 2,
                                       rk3576_spi(s, UART4_SPI),
                                       UART_BAUDBASE, serial_hd(3),
                                       DEVICE_LITTLE_ENDIAN));

    rk_block_init(s, &s->cru, "rk3576.cru", CRU_BASE, CRU_SIZE, true);
    s->cru.read = cru_read;

    rk_block_init(s, &s->pmu, "rk3576.pmu", PMU_BASE, PMU_SIZE, true);
    s->pmu.read = pmu_read;
    rk_block_init(s, &s->pmu0_grf, "rk3576.pmu0-grf", PMU0_GRF_BASE,
                  PMU_GRF_SIZE, true);
    s->pmu0_grf.wrote = pmu_grf_wrote;
    rk_block_init(s, &s->pmu1_grf, "rk3576.pmu1-grf", PMU1_GRF_BASE,
                  PMU_GRF_SIZE, true);
    s->pmu1_grf.wrote = pmu_grf_wrote;
    rk_block_init(s, &s->rng, "rk3576.rng", RNG_BASE, RNG_SIZE, false);
    s->rng.wrote = rng_wrote;
    rk_block_init(s, &s->otp, "rk3576.otp", OTP_BASE, OTP_SIZE, false);
    s->otp.wrote = otp_wrote;
    otp_init(s);
    rk_tsadc_init(s, &s->tsadc, TSADC_BASE, rk3576_spi(s, TSADC_SPI));
    rk_saradc_init(s, &s->saradc, SARADC_BASE, rk3576_spi(s, SARADC_SPI));
    rk_maskrom_init(&s->maskrom);

    s->timer0.soc = s;
    s->timer0.irq = rk3576_spi(s, TIMER0_SPI);
    s->timer0.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tmr_expired,
                                   &s->timer0);
    memory_region_init_io(&s->timer0.mr, OBJECT(s), &tmr_ops, &s->timer0,
                          "rk3576.timer0", 0x20);
    memory_region_add_subregion(sysmem, TIMER0_BASE, &s->timer0.mr);

    for (int b = 0; b < RK3576_NGPIO; b++) {
        RkGpio *g = &s->gpio[b];
        g_autofree char *name = g_strdup_printf("rk3576.gpio%d", b);

        g->soc = s;
        g->bank = b;
        g->irq = rk3576_spi(s, gpio_spi[b]);
        g->in = g->level = 0xffffffff;          /* pulled up */
        memory_region_init_io(&g->mr, OBJECT(s), &gpio_ops, g, name, 0x200);
        memory_region_add_subregion(sysmem, gpio_base[b], &g->mr);
    }

    for (int n = 0; n < RK3576_NI2C; n++) {
        SysBusDevice *i2c = SYS_BUS_DEVICE(&s->i2c[n]);

        if (n == 2 && s->i2c2_bus) {
            object_property_set_link(OBJECT(i2c), "bus",
                                     OBJECT(s->i2c2_bus), &error_abort);
        }
        if (!sysbus_realize(i2c, errp)) {
            return;
        }
        memory_region_add_subregion(sysmem, n ? 0x2ac40000 + (n - 1) * 0x10000
                                              : 0x27300000,
                                    sysbus_mmio_get_region(i2c, 0));
        sysbus_connect_irq(i2c, 0, rk3576_spi(s, 88 + n));
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi0), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, 0x2acf0000,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->spi0),
                                                       0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi0), 0, rk3576_spi(s, 116));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdmmc), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, 0x2a310000,
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->sdmmc), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdmmc), 0, rk3576_spi(s, 251));

    /* the USB controllers, and USB0's combo PHY. USB1's xHCI has a second
     * USB 2 port, for the MT7921 (the board's: behind its hub on the
     * device), the first one free */
    for (int n = 0; n < 2; n++) {
        SysBusDevice *usb = SYS_BUS_DEVICE(&s->usb[n]);

        qdev_prop_set_uint32(DEVICE(usb), "usb2-ports", n ? 2 : 1);
        if (!sysbus_realize(usb, errp)) {
            return;
        }
        memory_region_add_subregion(sysmem, USB_BASE + n * 0x400000,
                                    sysbus_mmio_get_region(usb, 0));
        sysbus_connect_irq(usb, 0, rk3576_spi(s, n ? USB1_SPI : USB0_SPI));
    }
    rk_block_init(s, &s->usbdp, "rk3576.usbdp", USBDP_BASE, USBDP_SIZE,
                  false);
    s->usbdp.read = usbdp_read;

    /* the Ethernet ports: each takes the next -nic for it, or the one of its
     * model, gmac0 or gmac1 (none: no cable) */
    for (int n = 0; n < 2; n++) {
        SysBusDevice *gmac = SYS_BUS_DEVICE(&s->gmac[n]);

        qemu_configure_nic_device(DEVICE(gmac), true, n ? "gmac1" : "gmac0");
        if (!sysbus_realize(gmac, errp)) {
            return;
        }
        memory_region_add_subregion(sysmem, GMAC_BASE + n * 0x10000,
                                    sysbus_mmio_get_region(gmac, 0));
        sysbus_connect_irq(gmac, 0, rk3576_spi(s, GMAC_SPI + n * 8));
    }

    /*
     * The UFS host: QEMU's, which is a PCI device, on a PCI host of its own
     * that the kernel does not see (it is mapped nowhere): its registers
     * where the RK3576 has its host's, its INTA on the host's SPI, its DMA
     * to the system's memory. The M-PHY and the GRFs around it take what
     * Rockchip's driver writes as unimplemented registers do.
     */
    s->ufs_pcie = qdev_new(TYPE_GPEX_HOST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->ufs_pcie), &error_fatal);
    /* (slot 0 is the host's own: in slot 1 its INTA is the host's line 1) */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->ufs_pcie), 1,
                       rk3576_spi(s, UFS_SPI));
    s->ufs = pci_new(PCI_DEVFN(1, 0), "ufs");
    qdev_prop_set_string(DEVICE(s->ufs), "serial", "FlipperOneEmu");
    qdev_prop_set_uint8(DEVICE(s->ufs), "lanes", 2);    /* (patch 0009) */
    /*
     * (patch 0025) the Flipper One's: UFS 2.2 (its specification), so
     * HS-G3 at most; new, so its health descriptor says normal, 0-10%
     * of its life used
     */
    qdev_prop_set_uint16(DEVICE(s->ufs), "spec-version", 0x0220);
    qdev_prop_set_uint8(DEVICE(s->ufs), "hs-gear", 3);
    qdev_prop_set_uint8(DEVICE(s->ufs), "pre-eol-info", 1);
    qdev_prop_set_uint8(DEVICE(s->ufs), "life-time-est-a", 1);
    qdev_prop_set_uint8(DEVICE(s->ufs), "life-time-est-b", 1);
    /* (patch 0017) provisioned with its boot feature on, boot LU A first */
    qdev_prop_set_bit(DEVICE(s->ufs), "boot-enable", s->ufs_boot_enable);
    qdev_prop_set_uint8(DEVICE(s->ufs), "boot-lun-en",
                        s->ufs_boot_enable ? 1 : 0);
    if (s->ufs_nvram) {
        qdev_prop_set_drive_err(DEVICE(s->ufs), "nvram", s->ufs_nvram,
                                &error_fatal);
    }
    pci_realize_and_unref(s->ufs, PCI_HOST_BRIDGE(s->ufs_pcie)->bus,
                          &error_fatal);
    memory_region_init_alias(&s->ufs_hci, OBJECT(s), "rk3576.ufs",
                             s->ufs->io_regions[0].memory, 0,
                             memory_region_size(s->ufs->io_regions[0].memory));
    memory_region_add_subregion(sysmem, UFS_BASE, &s->ufs_hci);
    ufs_master(s);

    for (int d = 0; d < 3; d++) {
        DeviceState *dma = qdev_new("pl330");
        DeviceState *any = qdev_new(TYPE_OR_IRQ);

        s->dmac[d] = dma;

        object_property_set_link(OBJECT(dma), "memory", OBJECT(sysmem),
                                 &error_abort);
        qdev_prop_set_uint32(dma, "num_chnls", 8);
        qdev_prop_set_uint8(dma, "num_periph_req", 32);
        qdev_prop_set_uint8(dma, "num_events", 16);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        memory_region_add_subregion(sysmem, 0x2ab90000 + d * 0x20000,
                                    sysbus_mmio_get_region(
                                        SYS_BUS_DEVICE(dma), 0));
        memory_region_init_alias(&s->dmac_id[d], OBJECT(s), "rk3576.dmac-id",
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(dma),
                                                        0), 0xfe0, 0x20);
        memory_region_add_subregion(sysmem, 0x2ab90000 + d * 0x20000 + 0x3fe0,
                                    &s->dmac_id[d]);
        qdev_prop_set_uint16(any, "num-lines", 17);
        qdev_realize_and_unref(any, NULL, &error_fatal);
        for (int i = 0; i < 17; i++) {          /* abort, then events */
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), i,
                               qdev_get_gpio_in(any, i));
        }
        qdev_connect_gpio_out(any, 0, rk3576_spi(s, 32 + 2 * d));
    }

    /* the SAIs, their requests to their DMACs */
    for (int i = 0; i < RK3576_NSAI; i++) {
        SysBusDevice *sai = SYS_BUS_DEVICE(&s->sai[i]);
        DeviceState *dma = s->dmac[rk3576_sais[i].dmac];

        if (!sysbus_realize(sai, errp)) {
            return;
        }
        memory_region_add_subregion(sysmem, rk3576_sais[i].base,
                                    sysbus_mmio_get_region(sai, 0));
        sysbus_connect_irq(sai, 0, rk3576_spi(s, rk3576_sais[i].spi));
        qdev_connect_gpio_out_named(DEVICE(sai), "dma", 0,
                                    qdev_get_gpio_in(dma, rk3576_sais[i].tx));
        qdev_connect_gpio_out_named(DEVICE(sai), "dma", 1,
                                    qdev_get_gpio_in(dma, rk3576_sais[i].rx));
    }

    /*
     * PCIe0, the M.2 slot's (rockchip,rk3576-pcie, one lane): QEMU's
     * DesignWare host, whose DBI (4 KiB: the root port's config space and
     * its port logic, a viewport iATU, the iMSI-RX) is at the DBI's base,
     * and whose outbound windows go where Linux programs them; Rockchip's
     * registers around it (PCIE_CLIENT_*); the combo PHY and its GRF take
     * what Linux writes as unimplemented registers do (it polls nothing).
     */
    s->pcie = qdev_new(TYPE_DESIGNWARE_PCIE_HOST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->pcie), &error_fatal);
    memory_region_add_subregion(sysmem, PCIE0_DBI, sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(s->pcie), 0));
    {
        DesignwarePCIEHost *h = DESIGNWARE_PCIE_HOST(s->pcie);

        /* the core's version, 5.40a: with none (0) Linux sets ATU_TD in
         * each window's CR1, and QEMU's host then takes its memory windows
         * for config ones */
        pci_set_long(PCI_DEVICE(&h->root)->config + 0x8f8, 0x3534302a);
        /* an iATU region's upper limit (0x924), read-only 0: regions of up
         * to 4 GiB, as on a core built so. (QEMU keeps its port logic
         * writable: the all-ones Linux probes it with stayed, the region
         * limit came out 2^64 - 1, and each window's size, the smaller of
         * that + 1 and the range's, 0) */
        pci_set_long(PCI_DEVICE(&h->root)->config + 0x924, 0);
        pci_set_long(PCI_DEVICE(&h->root)->wmask + 0x924, 0);
        /* its devices' DMA: DRAM, 1:1, as the core passes through what no
         * inbound window takes (Linux programs none: no dma-ranges) */
        memory_region_init_alias(&s->pcie_dma, OBJECT(s), "rk3576.pcie0-dma",
                                 sysmem, RK3576_DRAM_BASE,
                                 current_machine->ram_size);
        memory_region_add_subregion_overlap(&h->pci.address_space_root,
                                            RK3576_DRAM_BASE, &s->pcie_dma,
                                            -2);
    }
    for (int i = 0; i < 4; i++) {               /* INTA-D, then MSI */
        sysbus_connect_irq(SYS_BUS_DEVICE(s->pcie), i,
                           qdev_get_gpio_in_named(dev, "pcie0-intx", i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(s->pcie), 4,
                       rk3576_spi(s, PCIE0_SPI_MSI));
    rk_block_init(s, &s->pcie_apb, "rk3576.pcie0-client", PCIE0_APB,
                  PCIE0_APB_SIZE, true);
    s->pcie_apb.read = pcie_apb_read;
    s->pcie_apb.wrote = pcie_apb_wrote;

    /*
     * The display: the VOP2, its IOMMU (below), its HDMI transmitter with
     * the hotplug line in the IOC GRF, the HDMI PHY (its registers kept,
     * its GRF's status locked while its PLL is on). The VOP's own
     * interrupt and its IOMMU's share SPI 342.
     */
    s->vop_sys_or = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint16(s->vop_sys_or, "num-lines", 2);
    qdev_realize_and_unref(s->vop_sys_or, NULL, &error_fatal);
    qdev_connect_gpio_out(s->vop_sys_or, 0, rk3576_spi(s, VOP_SPI_SYS));
    s->vop.mmu = &s->vop_mmu;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vop), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, VOP_BASE, sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->vop), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vop), 0,
                       qdev_get_gpio_in(s->vop_sys_or, 0));
    for (int n = 0; n < RK_VOP2_NVP; n++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->vop), 1 + n,
                           rk3576_spi(s, VOP_SPI_VP0 + n));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hdmi), errp)) {
        return;
    }
    memory_region_add_subregion(sysmem, HDMI_BASE, sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->hdmi), 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->hdmi), 0,
                       rk3576_spi(s, HDMI_SPI_MAIN));
    rk_block_init(s, &s->hpd, "rk3576.ioc-grf-hpd", HPD_BASE, 0x100, true);
    s->hpd.read = hpd_read;
    s->hpd.wrote = hpd_wrote;
    rk_block_init(s, &s->hdptx, "rk3576.hdptx", HDPTX_BASE, HDPTX_SIZE,
                  false);
    rk_block_init(s, &s->hdptx_grf, "rk3576.hdptx-grf", HDPTX_GRF_BASE,
                  0x100, true);
    s->hdptx_grf.read = hdptx_grf_read;
    rk_block_init(s, &s->dp0, "rk3576.dp0", DP0_BASE, 0x1000, false);
    s->dp0.read = dp_read;

    /* the IOMMUs Flipper OS enables: the VOP's, and the video decoder's,
     * two banks each */
    {
        static const struct {
            hwaddr bank[2];
            int spi;
        } mmus[2] = {
            { { 0x27d07e00, 0x27d07f00 }, 342 },    /* (shared with the VOP's
                                                       own interrupt) */
            { { 0x27b00800, 0x27b00900 }, 309 },
        };
        RkIommuState *mmu[2] = { &s->vop_mmu, &s->vdec_mmu };

        for (int i = 0; i < 2; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(mmu[i]);

            qdev_prop_set_uint32(DEVICE(mmu[i]), "banks", 2);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            for (int b = 0; b < 2; b++) {
                memory_region_add_subregion(sysmem, mmus[i].bank[b],
                                            sysbus_mmio_get_region(sbd, b));
            }
            sysbus_connect_irq(sbd, 0, i ? rk3576_spi(s, mmus[i].spi) :
                                  qdev_get_gpio_in(s->vop_sys_or, 1));
        }
    }

    create_unimplemented_device("rk3576", PERIPH_BASE, PERIPH_SIZE);
}

static const Property rk3576_props[] = {
    DEFINE_PROP_BOOL("start-on", RK3576State, start_on, false),
    DEFINE_PROP_LINK("i2c2-bus", RK3576State, i2c2_bus, TYPE_I2C_BUS,
                     I2CBus *),
};

static void rk3576_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3576_realize;
    device_class_set_props(dc, rk3576_props);
    dc->user_creatable = false;
}

static const TypeInfo rk3576_types[] = {
    {
        .name = TYPE_RK3576,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RK3576State),
        .instance_init = rk3576_init,
        .class_init = rk3576_class_init,
    },
};

DEFINE_TYPES(rk3576_types)
