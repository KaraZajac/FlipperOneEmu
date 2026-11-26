/*
 * Rockchip RK3576: the Flipper One's application processor
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HW_ARM_FLIPPER_ONE_RK3576_H
#define HW_ARM_FLIPPER_ONE_RK3576_H

#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gic.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "net/net.h"
#include "hw/usb/hcd-xhci-sysbus.h"
#include "target/arm/cpu.h"

#define TYPE_RK3576 "rk3576"
OBJECT_DECLARE_SIMPLE_TYPE(RK3576State, RK3576)

/*
 * A block of Rockchip registers: what is written is kept, and most take
 * the bits they change as a mask in their top half (HIWORD_UPDATE: the
 * CRU's, the GRFs'); a block's own hooks add what its registers do.
 */
typedef struct RkBlock RkBlock;
struct RkBlock {
    MemoryRegion mr;
    struct RK3576State *soc;
    uint32_t *regs;
    bool hiword;
    uint32_t (*read)(RkBlock *b, hwaddr off);   /* NULL: the register */
    /* after a write: the register was @old, @v was written */
    void (*wrote)(RkBlock *b, hwaddr off, uint32_t old, uint32_t v);
};

/* a Rockchip timer (rk3288's kind): a 64-bit count down from LOAD */
typedef struct RkTimer {
    MemoryRegion mr;
    struct RK3576State *soc;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t load[2], ctrl, status;
    bool done;                          /* a one-shot count ran out */
    int64_t start_ns;                   /* its count began then */
    uint64_t hz;                        /* ... at this rate */
} RkTimer;

/* rk3x_i2c.c: a Rockchip I2C controller */
#define TYPE_RK_I2C "rk3x-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(RkI2cState, RK_I2C)

/*
 * What a controller's bus reaches, a step at a time, if not QEMU I2C
 * devices: each step 1 done, 0 not yet (the target holds SCL: the step is
 * tried again), -1 not acknowledged
 */
typedef struct RkI2cTarget {
    int (*start)(void *opaque, uint8_t addr, bool read);
    int (*write)(void *opaque, uint8_t byte);
    int (*read)(void *opaque, uint8_t *byte);
    void (*nack)(void *opaque);
    void (*stop)(void *opaque);
    void *opaque;
} RkI2cTarget;

enum { RKI2C_START, RKI2C_WRITE, RKI2C_READ };

typedef struct RkI2cStep {
    uint8_t op, byte;
    bool last;                          /* a read: the last, not ACKed */
} RkI2cStep;

struct RkI2cState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    I2CBus *bus;                        /* its own, or ... */
    I2CBus *shared_bus;                 /* ... this one, given */
    uint32_t con, clkdiv, mrxaddr, mrxraddr, mtxcnt, mrxcnt, ien, ipd, fcnt;
    uint32_t txbuf[8], rxbuf[8];
    bool active;                        /* a transfer is under way */
    bool need_addr;                     /* ... a START waits for its target */
    const RkI2cTarget *target;          /* NULL: QEMU's I2C devices */
    bool (*arb)(void *opaque, bool claim);  /* another master on the bus */
    void *arb_opaque;
    bool claimed;                       /* ... this one has it */
    bool busy;                          /* ... it has it: try again then */
    RkI2cStep steps[40];                /* the count under way */
    int nsteps, step, nread;
    bool reading;
    QEMUTimer *timer;                   /* ... a step the target held */
};

/* @t is what I2C@n's bus reaches */
void rk3576_i2c_target(RK3576State *s, int n, const RkI2cTarget *t);
/* ... and it has let go of the SCL it held: the count goes on */
void rk_i2c_release(RkI2cState *s);
/* I2C@n's bus has another master: @claim takes the bus before a START
 * (false: the other has a transfer under way) and lets it go after the
 * STOP */
void rk3576_i2c_arbiter(RK3576State *s, int n,
                        bool (*claim)(void *opaque, bool claim),
                        void *opaque);

/* a GPIO bank (Rockchip's v2: 32 pins, each register split in halves) */
typedef struct RkGpio {
    MemoryRegion mr;
    struct RK3576State *soc;
    int bank;
    qemu_irq irq;
    uint32_t dr, ddr;                   /* output levels, outputs */
    uint32_t en, mask, type, pol, both; /* interrupts: on, masked, edge,
                                           high or rising, either edge */
    uint32_t raw;                       /* ... pending (levels as they are,
                                           edges until EOI) */
    uint32_t in;                        /* the levels from outside */
    uint32_t level;                     /* the pins' levels, as last seen */
    uint32_t debounce, dbclk_en, dbclk_con;
} RkGpio;

/* rk_spi.c: a Rockchip SPI controller, as a host */
#define TYPE_RK_SPI "rockchip-spi"
OBJECT_DECLARE_SIMPLE_TYPE(RkSpiState, RK_SPI)
#define RKSPI_FIFO          64

struct RkSpiState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    uint32_t ctrlr0, ctrlr1, ssienr, ser, baudr, txftlr, rxftlr, imr;
    uint32_t dmacr, dmatdlr, dmardlr;
    uint16_t rx[RKSPI_FIFO];            /* what came in (nothing drives
                                           MISO: 0s) */
    int rx_n, rx_head;
    /* the board gets what goes out, with SER's chip selects */
    void (*out)(void *opaque, int cs, const uint8_t *data, int len);
    void *out_opaque;
};

void rk_spi_set_out(RkSpiState *s,
                    void (*out)(void *opaque, int cs, const uint8_t *data,
                                int len), void *opaque);

/* dw_mshc.c: a DesignWare SD host, with an SD bus for a card */
#define TYPE_DW_MSHC "dw-mshc"
OBJECT_DECLARE_SIMPLE_TYPE(DwMshcState, DW_MSHC)

struct DwMshcState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    SDBus sdbus;
    uint32_t ctrl, pwren, clkdiv, clksrc, clkena, tmout, ctype, blksiz;
    uint32_t bytcnt, intmask, cmdarg, cmd, resp[4], rintsts, fifoth, usrid;
    uint32_t uhs, bmod, dbaddr, idsts, idinten, dscaddr, bufaddr;
    uint32_t tcbcnt, tbbcnt;
    uint32_t other[128];                /* the rest, kept (timing, phases) */
    uint8_t fifo[4096];                 /* the driver's few bytes */
    uint32_t fifo_len, fifo_pos;
    uint32_t writing;                   /* ... a write's, to come */
};

SDBus *dw_mshc_bus(DwMshcState *s);

/* rk_dwc3.c: a USB controller (DWC3, dual-role), with QEMU's xHCI for its
 * host role; for its device role, the computer at the cable's end drives
 * it through rk_dwc3_usb_ops (fo_usb.h; its opaque: the RkDwc3State), and
 * pullup_fn tells that computer when D+ is pulled up or let go */
#define TYPE_RK_DWC3 "rk-dwc3"
OBJECT_DECLARE_SIMPLE_TYPE(RkDwc3State, RK_DWC3)

/* a physical endpoint of its device role (n: 2 * number + IN) */
typedef struct RkDwc3Ep {
    uint64_t start;                     /* where its transfer started */
    uint64_t cur;                       /* the TRB the transfer is at, */
    uint32_t done;                      /* ... and the bytes of it moved */
    uint32_t events;                    /* the events it gives (DEPCFG) */
    uint16_t mps;                       /* its packets' size */
    bool started, stalled;
    bool nrdy;                          /* a XferNotReady given, until the
                                           transfer it asked for starts */
} RkDwc3Ep;

struct RkDwc3State {
    SysBusDevice parent_obj;
    MemoryRegion mr;                    /* its 4 MiB */
    MemoryRegion quiet;                 /* ... where nothing is */
    MemoryRegion regs_mr;               /* ... the DWC3's own registers */
    XHCISysbusState xhci;               /* ... and its xHCI's, at 0 */
    uint32_t regs[0xb00 / 4];           /* 0xc100-0xcbff */
    qemu_irq irq;                       /* the xHCI's, ORed with: */
    bool xhci_irq;
    /* the device role */
    RkDwc3Ep ep[32];
    uint32_t ev_off, ev_count;          /* the event buffer: where the next
                                           goes; how much is in it */
    uint32_t ev_wait[64];               /* events it has no room for yet */
    int ev_nwait;
    bool pulled;                        /* D+ pulled up, as last told */
    bool connected;                     /* a host has reset it */
    uint8_t addr;                       /* the address it answers */
    bool ctl;                           /* a control transfer under way: */
    bool ctl_in, ctl_set_address, ctl_data_done;
    uint16_t ctl_len;
    uint8_t setup[8];
    bool setup_held;                    /* ... its SETUP, not yet given */
    bool ep0_stalled;
    void (*pullup_fn)(void *opaque, bool on);
    void *pullup_opaque;
};

struct FoUsbDevOps;
extern const struct FoUsbDevOps rk_dwc3_usb_ops;

/* rk_sai.c: a SAI (serial audio interface): FIFOs between its DMAC and an
 * I2S link. As the link's master its frame clock is its own; as a slave
 * (SAI2, the codec's) the codec gives it (rk_sai_lrck). The frames it sends
 * go to its sink, the codec's DAC; those it receives come from its source,
 * the codec's ADC (silence without one). */
#define TYPE_RK_SAI "rk-sai"
OBJECT_DECLARE_SIMPLE_TYPE(RkSaiState, RK_SAI)
#define RK_SAI_FIFO     1024            /* words, each way */

/* what takes the frames a SAI sends: @frames of them, S16 stereo */
typedef void RkSaiSink(void *opaque, const int16_t *lr, int frames);
/* what gives the frames a SAI receives: @frames of them into @lr */
typedef void RkSaiSource(void *opaque, int16_t *lr, int frames);

struct RkSaiState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;
    qemu_irq dma_busy[2];               /* to its DMAC, TX's and RX's:
                                           QEMU's PL330 waits while high */
    QEMUTimer *timer;                   /* its frame clock, while it runs */
    uint32_t regs[0x80 / 4];
    uint32_t tx[RK_SAI_FIFO], rx[RK_SAI_FIFO];
    int tx_head, tx_n, rx_head, rx_n;
    bool tx_half, rx_half;              /* (16-bit samples: half a word) */
    int64_t last_ns;                    /* the frame clock, last looked at */
    int64_t owed;                       /* ... the frames since, × 1e9 */
    int64_t tx_late;                    /* frames due that TX did not have */
    uint32_t mclk;                      /* its MCLK, as master (property) */
    int lrck_hz;                        /* a slave's frame clock (0: none) */
    RkSaiSink *sink;
    void *sink_opaque;
    RkSaiSource *source;
    void *source_opaque;
    uint16_t rx_low;                    /* (a word's first half, as it came) */
    bool verbose;                       /* (property, any time: its ticks
                                           and controls in QEMU's log) */
    int tx_in;                          /* (... words come since a tick) */
};

/* @sai's frame clock, from the link's master: @hz frames a second (0:
 * none) */
void rk_sai_lrck(void *sai, int hz);
void rk_sai_set_sink(RkSaiState *s, RkSaiSink *sink, void *opaque);
void rk_sai_set_source(RkSaiState *s, RkSaiSource *source, void *opaque);

/* rk_iommu.c: a Rockchip IOMMU (v2): its banks' registers, and the
 * translation of its master's addresses */
#define TYPE_RK_IOMMU "rk-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RkIommuState, RK_IOMMU)
#define RK_IOMMU_MAX_BANKS 2

typedef struct RkIommuBank {
    struct RkIommuState *s;
    uint32_t dte;                       /* DTE_ADDR, as written */
    bool paging, stall;
    uint32_t int_raw, int_mask, auto_gating;
} RkIommuBank;

struct RkIommuState {
    SysBusDevice parent_obj;
    MemoryRegion mr[RK_IOMMU_MAX_BANKS];
    qemu_irq irq;
    uint32_t nbanks;                    /* (property) */
    RkIommuBank bank[RK_IOMMU_MAX_BANKS];
};

/* @iova through @s's tables (its first bank's: the banks are programmed
 * alike): the physical address; false if nothing is mapped there */
bool rk_iommu_translate(RkIommuState *s, uint32_t iova, hwaddr *pa);

/* rk_vop2.c: the VOP2 (display controller): its registers, frames and
 * interrupts; video port 0 (HDMI's) on a QEMU console */
#define TYPE_RK_VOP2 "rk-vop2"
OBJECT_DECLARE_SIMPLE_TYPE(RkVop2State, RK_VOP2)
#define RK_VOP2_NVP     3               /* video ports */
#define RK_VOP2_SIZE    0x3000

typedef struct RkVop2Port {
    struct RkVop2State *s;
    int n;
} RkVop2Port;

struct RkVop2State {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t regs[RK_VOP2_SIZE / 4];
    uint32_t int_en[2 + RK_VOP2_NVP];   /* SYS0, SYS1, then VP0-2 */
    uint32_t int_raw[2 + RK_VOP2_NVP];
    uint8_t pending;                    /* REG_CFG_DONE's, per port */
    bool glb_en;
    bool running[RK_VOP2_NVP];
    bool standby_due[RK_VOP2_NVP];      /* (at the next frame start) */
    QEMUTimer *frame[RK_VOP2_NVP];
    RkVop2Port port[RK_VOP2_NVP];
    qemu_irq irq_sys, irq_vp[RK_VOP2_NVP];
    RkIommuState *mmu;                  /* its IOMMU (the SoC's to set) */
    QemuConsole *con;                   /* video port 0's picture */
    bool dirty;                         /* ... a "done" since its copy */
    int64_t last_copy;
};

/* rk_hdmi.c: the HDMI transmitter (DW HDMI QP), its DDC master answering
 * with the monitor's EDID */
#define TYPE_RK_HDMI "rk-hdmi"
OBJECT_DECLARE_SIMPLE_TYPE(RkHdmiState, RK_HDMI)
#define RK_HDMI_SIZE    0x20000

struct RkHdmiState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    uint32_t regs[RK_HDMI_SIZE / 4];
    uint32_t int1;                      /* MAINUNIT_1's interrupts */
    qemu_irq irq_main;
    bool connected;                     /* a monitor at the cable's end */
    uint8_t edid[256];
};

/* a monitor plugged in, or not: what the DDC master finds */
void rk_hdmi_plug(RkHdmiState *s, bool connected);

/* rk_gmac.c: an Ethernet MAC (DWC Ethernet QoS 4.20a), with the board's
 * RTL8211F PHY on its MDIO bus */
#define TYPE_RK_GMAC "rk-gmac"
OBJECT_DECLARE_SIMPLE_TYPE(RkGmacState, RK_GMAC)
#define RKGMAC_FRAME        16384       /* a frame, as its FIFOs take it */

struct RkGmacState {
    SysBusDevice parent_obj;
    MemoryRegion mr;
    qemu_irq irq;                       /* macirq */
    qemu_irq phy_int;                   /* the PHY's INTB: 1, asserted */
    NICConf conf;
    NICState *nic;
    uint32_t regs[0x1200 / 4];          /* the MAC's, MTL's and DMA's */
    uint32_t status;                    /* channel 0's (write 1 to clear) */
    uint64_t tx_cur, rx_cur;            /* the DMA's current descriptors */
    bool rbu;                           /* ... none for RX: said */
    uint32_t tx_len;                    /* a frame being gathered, ... */
    bool tx_drop;                       /* ... too long: dropped */
    uint8_t txbuf[RKGMAC_FRAME];
    uint8_t rxbuf[RKGMAC_FRAME + 4];    /* (with its FCS) */
    bool link;                          /* the PHY's link */
    QEMUTimer *cable;                   /* ... looked at each second: the
                                           backend's end of the cable */
    struct {
        uint16_t bmcr, anar, gbcr, page, insr;
        uint16_t vnd[6][16];            /* 0x10-0x1f, by page */
    } phy;
};

/*
 * rk3576_scmi.c: the SCMI server that TF-A is (the CPU and GPU clocks), on
 * SMC 0x82000010, its shared memory in DRAM's first 2 MiB, which are
 * TF-A's: the kernel's memory starts after them
 */
#define RK3576_SCMI_SMC         0x82000010u
#define RK3576_SCMI_SHMEM       0x4010f000
#define RK3576_TFA_SIZE         0x200000
typedef struct RkScmi {
    uint64_t clk_hz[3];
    bool clk_on[3];
} RkScmi;
/* at the RK3576's start: the channel free, the clocks as TF-A leaves them */
void rk_scmi_reset(RkScmi *s);
/* SMC 0x82000010: the message in the shared memory, answered there */
void rk_scmi_call(RkScmi *s);
/*
 * Cluster @c (0: the A53s, 1: the A72s) at its clock now: the voltage the
 * kernel gives its rail with that clock (the OPP table's, in mV), and what
 * a running core draws against what it draws at the cluster's top clock:
 * its current, (f / f_top)(V / V_top), and its power, (f / f_top)(V /
 * V_top)^2, as for any CMOS logic
 */
int rk3576_cluster_mv(struct RK3576State *s, int c);
double rk3576_cluster_current(struct RK3576State *s, int c);
double rk3576_cluster_power(struct RK3576State *s, int c);

/*
 * rk3576_tsadc.c: the temperature sensor (six channels: the SoC, the big
 * cores, the little cores, DDR, the NPU, the GPU), and how warm the chip
 * is, from the cores that run
 */
#define RK3576_TSADC_CHANNELS   6
typedef struct RkTsadc {
    MemoryRegion mr;
    struct RK3576State *soc;
    qemu_irq irq;
    QEMUTimer *timer;                   /* the samples, the chip's warmth */
    uint32_t regs[0x400 / 4];
    double ambient;                     /* °C, around it */
    double temp[RK3576_TSADC_CHANNELS]; /* °C, at each sensor */
    int64_t last_ns;                    /* ... as of then */
} RkTsadc;
void rk_tsadc_init(struct RK3576State *s, RkTsadc *t, hwaddr base,
                   qemu_irq irq);
/* its registers as at power-on (the chip's warmth stays) */
void rk_tsadc_reset(RkTsadc *t);
/* the sensors' trims, into the OTP's non-secure part */
void rk_tsadc_otp_trims(uint8_t *otp);

/* rk3576_saradc.c: the SARADC, eight channels of 12 bits over 1.8 V; what
 * they read, the board's (RK3576State's saradc_in) */
#define RK3576_SARADC_CHANNELS  8
typedef struct RkSaradc {
    MemoryRegion mr;
    struct RK3576State *soc;
    qemu_irq irq;
    QEMUTimer *timer;                   /* a conversion's end */
    uint32_t regs[0x200 / 4];
} RkSaradc;
void rk_saradc_init(struct RK3576State *s, RkSaradc *a, hwaddr base,
                    qemu_irq irq);
void rk_saradc_reset(RkSaradc *a);

#define RK3576_NCPUS        8
#define RK3576_NI2C         9           /* I2C0 ... I2C8 */
#define RK3576_NGPIO        5           /* GPIO0 ... GPIO4 */
#define RK3576_NSAI         3           /* those Flipper OS enables */           /* 4 Cortex-A53, then 4 Cortex-A72 */
#define RK3576_DRAM_BASE    0x40000000ULL
#define RK3576_GIC_SPIS     448         /* the GIC-400's, as the SoC has it */

struct RK3576State {
    SysBusDevice parent_obj;

    CPUClusterState cluster[2];         /* little (A53s), big (A72s) */
    ARMCPU cpu[RK3576_NCPUS];
    GICState gic;
    RkBlock cru;                        /* clocks and resets */
    RkBlock pmu;                        /* power domains */
    RkBlock pmu0_grf, pmu1_grf;         /* the PMU's GRFs: always on, kept
                                           through a reset of the SoC (their
                                           OS_REGs: the reboot mode, the DDR
                                           blob's word on the DRAM) */
    RkBlock rng;                        /* the TRNG */
    RkTimer timer0;                     /* the broadcast clockevent */
    RkI2cState i2c[RK3576_NI2C];
    RkGpio gpio[RK3576_NGPIO];
    RkSpiState spi0;                    /* the display link */
    DwMshcState sdmmc;                  /* the microSD slot */
    RkGmacState gmac[2];                /* the Ethernet ports */
    RkDwc3State usb[2];                 /* USB0 (the USB-C port's, OTG),
                                           USB1 (host: the hub's) */
    RkBlock usbdp;                      /* USB0's USB3/DP combo PHY */
    RkScmi scmi;                        /* TF-A's SCMI server */
    RkBlock otp;                        /* the OTP's controller */
    uint8_t otp_ns[0x100];              /* ... the part it reads */
    RkTsadc tsadc;                      /* the temperature sensor */
    RkSaradc saradc;                    /* the ADC */
    DeviceState *ufs_pcie;              /* a PCI host of its own, for ... */
    PCIDevice *ufs;                     /* ... the UFS host (QEMU's) */
    MemoryRegion ufs_hci;               /* its registers, where the RK3576
                                           has its host's */
    DeviceState *uart[2];               /* UART0, UART4 */
    DeviceState *dmac[3];
    MemoryRegion dmac_id[3];            /* the PL330s' IDs, where AMBA
                                           looks: their regions' ends */
    RkSaiState sai[RK3576_NSAI];        /* SAI0, SAI2 (the codec's), SAI6
                                           (HDMI's): rk3576_sai() */
    RkVop2State vop;                    /* the display controller */
    RkHdmiState hdmi;                   /* ... its HDMI transmitter, */
    RkBlock hpd;                        /* ... the hotplug line (IOC GRF), */
    bool hpd_level, hpd_pending;
    RkBlock hdptx, hdptx_grf;           /* ... the HDMI PHY and its GRF */
    RkBlock dp0;                        /* the DP controller (its config) */
    DeviceState *vop_sys_or;            /* (the VOP's and its IOMMU's line) */
    RkIommuState vop_mmu;               /* the VOP's IOMMU (two banks) */
    RkIommuState vdec_mmu;              /* the video decoder's (two) */
    DeviceState *pcie;                  /* PCIe0, the M.2 slot's: QEMU's
                                           DesignWare host, ... */
    RkBlock pcie_apb;                   /* ... Rockchip's registers, */
    uint8_t pcie_intx;                  /* ... INTA-D as the host has them */
    MemoryRegion pcie_dma;              /* ... its devices' DMA: DRAM */
    /* the board is told of a GPIO pin's new level */
    void (*gpio_out)(void *opaque, int bank, int pin, bool level);
    void *gpio_out_opaque;
    /* the CRU resets the chip (the TSADC's TSHUT): the board's to do, as
     * PSCI's SYSTEM_RESET is */
    void (*cru_reset)(void *opaque);
    void *cru_reset_opaque;
    /* the code (0-4095, 1.8 V full scale) at SARADC channel @ch's pin */
    int (*saradc_in)(void *opaque, int ch);
    void *saradc_in_opaque;
    I2CBus *i2c2_bus;                   /* I2C2's bus, if it is shared */
    /* the UFS device as the board's was provisioned: bBootEnable, and a
     * drive for its own state (bBootLunEn), both the board's to give */
    bool ufs_boot_enable;
    BlockBackend *ufs_nvram;
    bool start_on;                      /* CPU 0 runs from the start (a
                                           kernel given), else all are off */
};

/* an SPI of the GIC, as the device tree numbers them */
qemu_irq rk3576_spi(RK3576State *s, int spi);
/* a monitor plugged into the HDMI port, or pulled out */
void rk3576_hdmi_plug(RK3576State *s, bool on);
/* its devices as at power-on (not the cores, which are the board's to
 * start) */
void rk3576_reset_devices(RK3576State *s);
/* its supply gone: what is always on loses what it kept */
void rk3576_power_lost(RK3576State *s);
/* what Rockchip's DDR blob leaves for the stages after it, for DRAM of
 * @size bytes at RK3576_DRAM_BASE */
void rk3576_ddr_blob(RK3576State *s, uint64_t size);
/* the bus of I2C@n, for what the board has on it */
I2CBus *rk3576_i2c_bus(RK3576State *s, int n);
/* SAI @n (0, 2 or 6) */
RkSaiState *rk3576_sai(RK3576State *s, int n);
/* the UFS host's bus, for the device's logical units (ufs-lu) */
BusState *rk3576_ufs_bus(RK3576State *s);
/* a level from outside on GPIO@bank's @pin (0 ... 31: A0 ... D7) */
void rk3576_gpio_in(RK3576State *s, int bank, int pin, bool level);

#endif
