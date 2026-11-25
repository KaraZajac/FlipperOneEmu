/*
 * The RK3576's Ethernet MACs: Synopsys DWC Ethernet QoS 4.20a ("GMAC4"), as
 * Linux's stmmac drives them, each with the board's RTL8211F PHY at address
 * 1 on its MDIO bus
 *
 * The MAC, so far as the driver uses it (the other registers keep what is
 * written):
 *   0x000 Configuration (RE, TE), 0x0a0 RxQ_Ctrl0 (RXQ0EN): receiving needs
 *         all three on, and the channel's SR; sending TE and ST
 *   0x110 Version 0x42 (4.20); 0x11c-0x128 the hardware features: one queue
 *         and one DMA channel each way, 16 KiB FIFOs, 32-bit DMA, GMII and
 *         RGMII; no checksum offload, TSO, PTP, EEE, VLAN filtering, MMC
 *         counters, wake-up frames or split header, so the driver takes its
 *         plainest path
 *   0x200 and 0x204, MDIO: clause 22; a transfer is done (GB clear) as it
 *         is asked for
 *   the interrupt status of the MAC (0x0b0) and the MTL (0xc20, 0xd2c): 0
 *   0x1000 DMA Mode: SWR resets the MAC (the PHY stays), at once
 *   channel 0 at 0x1100: TX and RX control (ST; SR and RBSZ, the size of
 *         each of a descriptor's two buffers), the rings' bases, lengths and
 *         tail pointers, interrupt enable, status (write 1 to clear; NIS
 *         and AIS sum up its normal and abnormal bits), the current
 *         descriptors
 *
 * Its DMA, on GMAC4's 16-byte descriptors (32-bit addresses):
 *   TX  from the current descriptor up to the tail pointer, each frame from
 *       FD to LD (buffer 1 of each), padded to 60 bytes, sent; each
 *       descriptor given back (OWN clear, FD and LD kept, no error); TI if
 *       the frame's last had IOC. It waits at a descriptor the driver does
 *       not own the frame of yet, until its next tail write
 *   RX  a frame with its FCS (64 bytes at least, as on the wire), into the
 *       descriptors the driver owns, buffer 1 then buffer 2 of each, each
 *       full but the last, whose write-back has LD and the frame's length;
 *       RI at once (the driver sets no IOC: on the chip the RX watchdog
 *       raises it a few hundred microseconds later). A frame that the
 *       owned descriptors cannot hold waits (RBU once) for the driver's
 *       next tail write. Nothing is filtered: what the network gives the
 *       port is the port's
 * macirq is the channel's enabled status bits: (normal & NIE) | (abnormal
 * & AIE).
 *
 * The PHY, as Linux's realtek driver drives an RTL8211F: the standard
 * registers (BMCR's reset and autonegotiation restart done at once:
 * autonegotiation is instant), 1000 Mbit/s full duplex while there is a
 * link, MMD registers reading 0 (no EEE), the vendor registers by page
 * (pages 0xa42, 0xa43 and the default one being one file), PHYSR, and INSR
 * (cleared by its reading) with INER, which drive its INTB ("phy-int", 1
 * asserted). There is a link while the port's network backend is there and
 * its link up (the monitor's set_link takes it down, as a cable pulled
 * out) and the PHY is not powered down.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "rk3576.h"

#define MAC_CONFIG          0x000
#define MAC_RE              BIT(0)
#define MAC_TE              BIT(1)
#define MAC_RXQ_CTRL0       0x0a0
#define MAC_INT_STATUS      0x0b0
#define MAC_VERSION         0x110
#define MAC_HWF0            0x11c
#define MAC_HWF1            0x120
#define MAC_HWF2            0x124
#define MAC_HWF3            0x128
#define MAC_MDIO_ADDR       0x200
#define MDIO_GB             BIT(0)
#define MDIO_C45E           BIT(1)
#define MAC_MDIO_DATA       0x204
#define MAC_ADDR0_HI        0x300
#define MAC_ADDR0_LO        0x304
#define MTL_INT_STATUS      0xc20
#define MTL_Q0_INT_CTRL     0xd2c
#define DMA_MODE            0x1000
#define DMA_SWR             BIT(0)
#define DMA_INT_STATUS      0x1008
#define CH_TX_CTRL          0x1104
#define CH_ST               BIT(0)
#define CH_RX_CTRL          0x1108
#define CH_SR               BIT(0)
#define CH_TXDESC_HI        0x1110
#define CH_TXDESC_LO        0x1114
#define CH_RXDESC_HI        0x1118
#define CH_RXDESC_LO        0x111c
#define CH_TX_TAIL          0x1120
#define CH_RX_TAIL          0x1128
#define CH_TX_RING_LEN      0x112c
#define CH_RX_RING_LEN      0x1130
#define CH_INT_EN           0x1134
#define CH_CUR_TX_DESC      0x1144
#define CH_CUR_RX_DESC      0x114c
#define CH_STATUS           0x1160

#define VERSION             0x00000042
#define HWF0                0x10000023      /* MII, GMII, SMA; RGMII */
#define HWF1                0x000001c7      /* FIFOs 16 KiB each way */

/* the channel's status and interrupt enable bits */
#define ST_TI               BIT(0)
#define ST_TPS              BIT(1)
#define ST_TBU              BIT(2)
#define ST_RI               BIT(6)
#define ST_RBU              BIT(7)
#define ST_RPS              BIT(8)
#define ST_RWT              BIT(9)
#define ST_ETI              BIT(10)
#define ST_ERI              BIT(11)
#define ST_FBE              BIT(12)
#define ST_CDE              BIT(13)
#define ST_AIS              BIT(14)
#define ST_NIS              BIT(15)
#define ST_NORMAL           (ST_TI | ST_TBU | ST_RI | ST_ERI)
#define ST_ABNORMAL         (ST_TPS | ST_RBU | ST_RPS | ST_RWT | ST_ETI | \
                             ST_FBE | ST_CDE)

/* descriptors */
#define DES3_OWN            BIT(31)
#define DES3_CTXT           BIT(30)
#define DES3_FD             BIT(29)
#define DES3_LD             BIT(28)
#define TDES2_IOC           BIT(31)
#define TDES2_B1L           0x3fff
#define RDES3_BUF2V         BIT(25)

#define ETH_MIN             60              /* a frame without its FCS */
#define FCS_LEN             4

/* the PHY */
#define PHY_ADDR            1
#define BMCR_RESET          BIT(15)
#define BMCR_PDOWN          BIT(11)
#define BMCR_ANRESTART      BIT(9)
#define INSR_LINK           BIT(4)
#define VND_INER            0x12            /* page 0xa42 */
#define VND_PHYSR           0x1a
#define VND_INSR            0x1d

static uint64_t gmac_tx_base(RkGmacState *s)
{
    return (uint64_t)s->regs[CH_TXDESC_HI / 4] << 32 |
           s->regs[CH_TXDESC_LO / 4];
}

static uint64_t gmac_rx_base(RkGmacState *s)
{
    return (uint64_t)s->regs[CH_RXDESC_HI / 4] << 32 |
           s->regs[CH_RXDESC_LO / 4];
}

/* the descriptor after @cur in a ring of @len + 1 from @base */
static uint64_t gmac_next(uint64_t cur, uint64_t base, uint32_t len)
{
    return (cur - base) / 16 >= len ? base : cur + 16;
}

static void gmac_desc_read(uint64_t addr, uint32_t d[4])
{
    dma_memory_read(&address_space_memory, addr, d, 16,
                    MEMTXATTRS_UNSPECIFIED);
    for (int i = 0; i < 4; i++) {
        d[i] = le32_to_cpu(d[i]);
    }
}

static void gmac_desc_write32(uint64_t addr, uint32_t v)
{
    stl_le_dma(&address_space_memory, addr, v, MEMTXATTRS_UNSPECIFIED);
}

/* ------------------------------------------------------- interrupts */

static uint32_t gmac_status(RkGmacState *s)
{
    uint32_t st = s->status;

    if (st & ST_NORMAL) {
        st |= ST_NIS;
    }
    if (st & ST_ABNORMAL) {
        st |= ST_AIS;
    }
    return st;
}

static void gmac_irq(RkGmacState *s)
{
    uint32_t pending = s->status & s->regs[CH_INT_EN / 4];
    bool normal = (pending & ST_NORMAL) && (s->regs[CH_INT_EN / 4] & ST_NIS);
    bool abnormal = (pending & ST_ABNORMAL) &&
                    (s->regs[CH_INT_EN / 4] & ST_AIS);

    qemu_set_irq(s->irq, normal || abnormal);
}

/* -------------------------------------------------------------- PHY */

static void phy_int(RkGmacState *s)
{
    qemu_set_irq(s->phy_int, (s->phy.insr & s->phy.vnd[0][VND_INER - 0x10])
                             != 0);
}

/* the link, as it is now: a change is latched in INSR */
static void gmac_link(RkGmacState *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);
    bool link = nc->peer && !nc->link_down &&
                !(s->phy.bmcr & BMCR_PDOWN);

    if (link != s->link) {
        s->link = link;
        s->phy.insr |= INSR_LINK;
        phy_int(s);
        if (link) {
            qemu_flush_queued_packets(nc);
        }
    }
}

/* a page's file of vendor registers (0x10-0x1e) */
static int phy_page(uint16_t page)
{
    switch (page) {
    case 0: case 0xa42: case 0xa43:
        return 0;                               /* (one file) */
    case 0xd08: return 1;                       /* RGMII delays */
    case 0xd8a: return 2;                       /* wake-on-LAN */
    case 0xd8c: return 3;
    case 0xd04: return 4;                       /* LEDs */
    }
    return 5;                                   /* (the rest, together) */
}

static void phy_reset(RkGmacState *s)
{
    memset(&s->phy, 0, sizeof(s->phy));
    s->phy.bmcr = 0x1140;                       /* autonegotiation, 1000 */
    s->phy.anar = 0x01e1;
    s->phy.gbcr = 0x0200;
}

static uint16_t phy_read(RkGmacState *s, int reg)
{
    int file = phy_page(s->phy.page);
    uint16_t v;

    switch (reg) {
    case 0x00: return s->phy.bmcr;
    case 0x01: return s->link ? 0x796d : 0x7949;     /* BMSR */
    case 0x02: return 0x001c;                       /* PHYID: RTL8211F */
    case 0x03: return 0xc916;
    case 0x04: return s->phy.anar;
    case 0x05: return s->link ? 0x41e1 : 0;         /* the partner's */
    case 0x06: return s->link ? 0x0001 : 0;         /* ANER */
    case 0x09: return s->phy.gbcr;
    case 0x0a: return s->link ? 0x3800 : 0;         /* GBSR: 1000 FD */
    case 0x0d: case 0x0e: return 0;                 /* MMD: no EEE */
    case 0x0f: return 0x3000;                       /* GBESR */
    case 0x1f: return s->phy.page;
    }
    if (reg < 0x10) {
        return 0;
    }
    if (file == 0 && reg == VND_PHYSR) {
        return s->link ? 0x002c : 0;                /* 1000, FD, link */
    }
    if (file == 0 && reg == VND_INSR) {
        v = s->phy.insr;
        s->phy.insr = 0;                            /* (read, cleared) */
        phy_int(s);
        return v;
    }
    return s->phy.vnd[file][reg - 0x10];
}

static void phy_write(RkGmacState *s, int reg, uint16_t v)
{
    int file = phy_page(s->phy.page);

    switch (reg) {
    case 0x00:
        if (v & BMCR_RESET) {
            phy_reset(s);
        } else {
            s->phy.bmcr = v & ~BMCR_ANRESTART;      /* (negotiated at once) */
        }
        gmac_link(s);
        return;
    case 0x04:
        s->phy.anar = v;
        return;
    case 0x09:
        s->phy.gbcr = v;
        return;
    case 0x1f:
        s->phy.page = v;
        return;
    }
    if (reg < 0x10 || (file == 0 && (reg == VND_PHYSR || reg == VND_INSR))) {
        return;
    }
    s->phy.vnd[file][reg - 0x10] = v;
    if (file == 0 && reg == VND_INER) {
        phy_int(s);
    }
}

/* a transfer on the MDIO bus, as MAC_MDIO_Address asks for it (clause 45
 * ones read 0xffff: no clause 45 device is there) */
static void gmac_mdio(RkGmacState *s, uint32_t a)
{
    int pa = (a >> 21) & 0x1f, reg = (a >> 16) & 0x1f, goc = (a >> 2) & 3;
    uint32_t *data = &s->regs[MAC_MDIO_DATA / 4];

    if (goc == 1 && !(a & MDIO_C45E)) {
        if (pa == PHY_ADDR) {
            phy_write(s, reg, *data);
        }
    } else if (goc == 3) {
        *data = (*data & 0xffff0000) |
                (pa == PHY_ADDR && !(a & MDIO_C45E) ? phy_read(s, reg)
                                                    : 0xffff);
    }
}

/* --------------------------------------------------------------- TX */

static void gmac_tx(RkGmacState *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);
    uint32_t d[4], len;

    while ((s->regs[MAC_CONFIG / 4] & MAC_TE) &&
           (s->regs[CH_TX_CTRL / 4] & CH_ST) &&
           (uint32_t)s->tx_cur != s->regs[CH_TX_TAIL / 4]) {
        gmac_desc_read(s->tx_cur, d);
        if (!(d[3] & DES3_OWN)) {
            break;                              /* (not the DMA's yet) */
        }
        if (!(d[3] & DES3_CTXT)) {
            if (d[3] & DES3_FD) {
                s->tx_len = 0;
                s->tx_drop = false;
            }
            len = d[2] & TDES2_B1L;
            if (s->tx_len + len <= sizeof(s->txbuf)) {
                dma_memory_read(&address_space_memory, d[0],
                                s->txbuf + s->tx_len, len,
                                MEMTXATTRS_UNSPECIFIED);
                s->tx_len += len;
            } else {
                s->tx_drop = true;              /* (more than the FIFO) */
            }
        }
        /* given back, with no error */
        gmac_desc_write32(s->tx_cur + 12, d[3] & (DES3_FD | DES3_LD));
        s->tx_cur = gmac_next(s->tx_cur, gmac_tx_base(s),
                              s->regs[CH_TX_RING_LEN / 4]);
        if ((d[3] & (DES3_CTXT | DES3_LD)) == DES3_LD) {
            if (!s->tx_drop) {
                if (s->tx_len < ETH_MIN) {      /* padded, as the MAC does */
                    memset(s->txbuf + s->tx_len, 0, ETH_MIN - s->tx_len);
                    s->tx_len = ETH_MIN;
                }
                qemu_send_packet(nc, s->txbuf, s->tx_len);
            }
            s->tx_len = 0;
            if (d[2] & TDES2_IOC) {
                s->status |= ST_TI;
            }
        }
    }
    gmac_irq(s);
}

/* --------------------------------------------------------------- RX */

static bool gmac_rx_on(RkGmacState *s)
{
    return (s->regs[MAC_CONFIG / 4] & MAC_RE) &&
           (s->regs[MAC_RXQ_CTRL0 / 4] & 3) &&
           (s->regs[CH_RX_CTRL / 4] & CH_SR) && s->link;
}

/* no descriptor for what comes: said once, until the driver gives more */
static void gmac_rbu(RkGmacState *s)
{
    if (!s->rbu) {
        s->rbu = true;
        s->status |= ST_RBU;
        gmac_irq(s);
    }
}

static void gmac_rx_kick(RkGmacState *s)
{
    if (gmac_rx_on(s)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static bool gmac_can_receive(NetClientState *nc)
{
    RkGmacState *s = qemu_get_nic_opaque(nc);
    uint32_t d[4];

    if (!gmac_rx_on(s)) {
        return true;                    /* (taken, and dropped: no link, or
                                           the MAC not receiving) */
    }
    gmac_desc_read(s->rx_cur, d);
    if (!(d[3] & DES3_OWN)) {
        gmac_rbu(s);
        return false;
    }
    return true;
}

static ssize_t gmac_receive(NetClientState *nc, const uint8_t *buf,
                            size_t size)
{
    RkGmacState *s = qemu_get_nic_opaque(nc);
    uint32_t bsz = (s->regs[CH_RX_CTRL / 4] >> 1) & 0x3fff;
    uint32_t ring = s->regs[CH_RX_RING_LEN / 4], d[4], crc, n;
    uint64_t base = gmac_rx_base(s), cur;
    size_t len = size, total, room = 0, done = 0;
    bool first = true;

    if (!gmac_rx_on(s) || !bsz || size > sizeof(s->rxbuf) - FCS_LEN) {
        return size;                            /* (dropped) */
    }
    memcpy(s->rxbuf, buf, size);
    if (len < ETH_MIN) {                        /* as it came on the wire */
        memset(s->rxbuf + len, 0, ETH_MIN - len);
        len = ETH_MIN;
    }
    crc = cpu_to_le32(net_crc32_le(s->rxbuf, len));
    memcpy(s->rxbuf + len, &crc, FCS_LEN);
    total = len + FCS_LEN;

    /* the descriptors it takes, all the driver's */
    cur = s->rx_cur;
    for (uint32_t i = 0; room < total; i++) {
        if (i > ring) {
            return size;                        /* (more than the ring) */
        }
        gmac_desc_read(cur, d);
        if (!(d[3] & DES3_OWN)) {
            gmac_rbu(s);
            return 0;                           /* (it waits) */
        }
        room += d[3] & RDES3_BUF2V ? 2 * bsz : bsz;
        cur = gmac_next(cur, base, ring);
    }

    while (done < total) {
        gmac_desc_read(s->rx_cur, d);
        n = MIN(bsz, total - done);
        dma_memory_write(&address_space_memory, d[0], s->rxbuf + done, n,
                         MEMTXATTRS_UNSPECIFIED);
        done += n;
        if (done < total && (d[3] & RDES3_BUF2V)) {
            n = MIN(bsz, total - done);
            dma_memory_write(&address_space_memory, d[2], s->rxbuf + done, n,
                             MEMTXATTRS_UNSPECIFIED);
            done += n;
        }
        for (int w = 0; w < 3; w++) {
            gmac_desc_write32(s->rx_cur + 4 * w, 0);
        }
        gmac_desc_write32(s->rx_cur + 12, (first ? DES3_FD : 0) |
                          (done == total ? DES3_LD | total : 0));
        first = false;
        s->rx_cur = gmac_next(s->rx_cur, base, ring);
    }
    s->rbu = false;
    s->status |= ST_RI;
    gmac_irq(s);
    return size;
}

static void gmac_set_link(NetClientState *nc)
{
    gmac_link(qemu_get_nic_opaque(nc));
}

static NetClientInfo gmac_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = gmac_can_receive,
    .receive = gmac_receive,
    .link_status_changed = gmac_set_link,
};

/* -------------------------------------------------------- registers */

/* the MAC, its MTL and DMA as at reset (not the PHY) */
static void gmac_reset_mac(RkGmacState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MAC_ADDR0_HI / 4] = 0x8000ffff;
    s->regs[MAC_ADDR0_LO / 4] = 0xffffffff;
    s->status = 0;
    s->tx_cur = s->rx_cur = 0;
    s->tx_len = 0;
    s->tx_drop = s->rbu = false;
    gmac_irq(s);
}

static uint64_t gmac_read(void *opaque, hwaddr off, unsigned size)
{
    RkGmacState *s = opaque;

    switch (off) {
    case MAC_VERSION: return VERSION;
    case MAC_HWF0: return HWF0;
    case MAC_HWF1: return HWF1;
    case MAC_HWF2: case MAC_HWF3: return 0;
    case MAC_INT_STATUS: case MTL_INT_STATUS: case MTL_Q0_INT_CTRL:
        return 0;
    case DMA_INT_STATUS:                        /* DC0IS */
        return (gmac_status(s) & s->regs[CH_INT_EN / 4]) != 0;
    case CH_CUR_TX_DESC: return (uint32_t)s->tx_cur;
    case CH_CUR_RX_DESC: return (uint32_t)s->rx_cur;
    case CH_STATUS: return gmac_status(s);
    }
    return off < sizeof(s->regs) ? s->regs[off / 4] : 0;
}

static void gmac_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkGmacState *s = opaque;
    uint32_t old;

    if (off >= sizeof(s->regs)) {
        return;
    }
    switch (off) {
    case MAC_VERSION: case MAC_HWF0: case MAC_HWF1: case MAC_HWF2:
    case MAC_HWF3:
        return;
    case DMA_MODE:
        if (v & DMA_SWR) {
            gmac_reset_mac(s);                  /* (done at once) */
            return;
        }
        break;
    case CH_STATUS:
        s->status &= ~v;                        /* write 1 to clear */
        gmac_irq(s);
        return;
    }
    old = s->regs[off / 4];
    s->regs[off / 4] = v;
    switch (off) {
    case MAC_CONFIG:
        if (v & ~old & MAC_TE) {
            gmac_tx(s);
        }
        if (v & ~old & MAC_RE) {
            gmac_rx_kick(s);
        }
        break;
    case MAC_RXQ_CTRL0:
        gmac_rx_kick(s);
        break;
    case MAC_MDIO_ADDR:
        if (v & MDIO_GB) {
            gmac_mdio(s, v);
            s->regs[off / 4] &= ~MDIO_GB;       /* done */
        }
        break;
    case CH_TX_CTRL:
        if (v & ~old & CH_ST) {
            gmac_tx(s);
        }
        break;
    case CH_RX_CTRL:
        if (v & ~old & CH_SR) {
            gmac_rx_kick(s);
        }
        break;
    case CH_TXDESC_LO:
        if (!(s->regs[CH_TX_CTRL / 4] & CH_ST)) {
            s->tx_cur = gmac_tx_base(s);
        }
        break;
    case CH_RXDESC_LO:
        if (!(s->regs[CH_RX_CTRL / 4] & CH_SR)) {
            s->rx_cur = gmac_rx_base(s);
        }
        break;
    case CH_TX_TAIL:
        gmac_tx(s);
        break;
    case CH_RX_TAIL:
        s->rbu = false;
        gmac_rx_kick(s);
        break;
    case CH_INT_EN:
        gmac_irq(s);
        break;
    }
}

static const MemoryRegionOps gmac_ops = {
    .read = gmac_read,
    .write = gmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void gmac_reset(DeviceState *dev)
{
    RkGmacState *s = RK_GMAC(dev);

    gmac_reset_mac(s);
    phy_reset(s);
    s->link = false;
    gmac_link(s);
    s->phy.insr = 0;                            /* (no change to tell of) */
    phy_int(s);
}

static void gmac_realize(DeviceState *dev, Error **errp)
{
    RkGmacState *s = RK_GMAC(dev);

    memory_region_init_io(&s->mr, OBJECT(s), &gmac_ops, s, "rk-gmac",
                          0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mr);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->phy_int, "phy-int", 1);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&gmac_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const Property gmac_props[] = {
    DEFINE_NIC_PROPERTIES(RkGmacState, conf),
};

static void gmac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = gmac_realize;
    device_class_set_legacy_reset(dc, gmac_reset);
    device_class_set_props(dc, gmac_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo gmac_types[] = {
    {
        .name = TYPE_RK_GMAC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkGmacState),
        .class_init = gmac_class_init,
    },
};

DEFINE_TYPES(gmac_types)
