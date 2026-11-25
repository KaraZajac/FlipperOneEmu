/*
 * The RK3576's VOP2 (video output processor), as Linux's rockchipdrm
 * drives it, and its video port 0 (HDMI's) on a QEMU console
 *
 * - Registers: kept as written (the driver reads several back), but:
 *   VERSION_INFO is the RK3576's (else the driver gives up a mode set);
 *   REG_CFG_DONE takes a video port's "done" (bit n, with its write enable
 *   in bit 16 + n) as pending until that port's next frame start, and reads
 *   back the global enable (bit 15) and the pending bits only; the SYS and
 *   VP interrupt registers are write-masked (EN), write-1-to-clear (CLR,
 *   both bit i and its mask 16 + i) and read RAW & EN (STATUS).
 * - Frames: a video port runs while its DSP_CTRL is out of STANDBY with
 *   its timings set, at 60 frames a second. At each frame start its
 *   pending "done" is taken and FS and FS_FIELD (vblank) are raised; a
 *   STANDBY written while it runs stops it at the next frame start, with
 *   DSP_HOLD_VALID (at once if it is not running). The tick runs only
 *   while someone waits for it (an interrupt enabled, a "done" pending,
 *   a standby due).
 * - Scanout: video port 0 (HDMI) goes to a QEMU console, composed when the
 *   console asks (a screendump, a display): its size from its active area,
 *   its background, then the windows its OVL_LAYER_SEL stacks (Cluster0/1,
 *   Esmart0/2; linear ARGB8888/XRGB8888, RGB888 and RGB565, red and blue
 *   swapped or not), read from memory through the VOP's IOMMU, scaled to
 *   their display size (nearest). No AFBC, YUV, mirroring or alpha.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "ui/console.h"
#include "rk3576.h"

#define VOP_VERSION         0x50199765
#define REG_CFG_DONE        0x000
#define CFG_DONE_GLB_EN     BIT(15)
#define VERSION_INFO        0x004
#define SYS_INT(n)          (0x080 + 0x10 * (n))    /* SYS0, SYS1 */
#define VP_INT(n)           (0x0a0 + 0x10 * (n))
#define INT_EN              0x0
#define INT_CLR             0x4
#define INT_STATUS          0x8
#define INT_RAW             0xc
#define INT_FS              BIT(0)
#define INT_FS_FIELD        BIT(5)
#define INT_DSP_HOLD_VALID  BIT(6)
#define OVL_LAYER_SEL(n)    (0x604 + 0x100 * (n))
#define VP(n)               (0xc00 + 0x100 * (n))
#define DSP_CTRL            0x00
#define DSP_STANDBY         BIT(31)
#define DSP_BG              0x2c
#define DSP_HACT_ST_END     0x4c
#define DSP_VTOTAL_VS_END   0x50
#define DSP_VACT_ST_END     0x54

#define FRAME_NS            (NANOSECONDS_PER_SECOND / 60)
#define R(off)              ((off) / 4)

/* the interrupt registers: SYS0, SYS1, then VP0-2 */
static int int_index(hwaddr off)
{
    if (off >= SYS_INT(0) && off < SYS_INT(2)) {
        return (off - SYS_INT(0)) / 0x10;
    }
    if (off >= VP_INT(0) && off < VP_INT(RK_VOP2_NVP)) {
        return 2 + (off - VP_INT(0)) / 0x10;
    }
    return -1;
}

static void vop2_irq_update(RkVop2State *s)
{
    qemu_set_irq(s->irq_sys, ((s->int_raw[0] & s->int_en[0]) |
                              (s->int_raw[1] & s->int_en[1])) != 0);
    for (int n = 0; n < RK_VOP2_NVP; n++) {
        qemu_set_irq(s->irq_vp[n],
                     (s->int_raw[2 + n] & s->int_en[2 + n]) != 0);
    }
}

/* a video port's timings are set: it can run */
static bool vp_timed(RkVop2State *s, int n)
{
    return (s->regs[R(VP(n) + DSP_VTOTAL_VS_END)] >> 16) & 0x1fff;
}

/* tick while a frame start is waited for */
static void vp_schedule(RkVop2State *s, int n)
{
    bool wanted = s->running[n] &&
                  ((s->int_en[2 + n] & (INT_FS | INT_FS_FIELD |
                                         INT_DSP_HOLD_VALID)) ||
                   (s->pending & BIT(n)) || s->standby_due[n]);

    if (wanted && !timer_pending(s->frame[n])) {
        timer_mod(s->frame[n], qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                               FRAME_NS);
    } else if (!wanted) {
        timer_del(s->frame[n]);
    }
}

static void vp_frame(void *opaque)
{
    RkVop2Port *p = opaque;
    RkVop2State *s = p->s;
    int n = p->n;

    if (!s->running[n]) {
        return;
    }
    s->pending &= ~BIT(n);
    if (s->standby_due[n]) {            /* stopped at this frame start */
        s->standby_due[n] = false;
        s->running[n] = false;
        s->int_raw[2 + n] |= INT_DSP_HOLD_VALID;
    } else {
        s->int_raw[2 + n] |= INT_FS | INT_FS_FIELD;
    }
    vop2_irq_update(s);
    vp_schedule(s, n);
}

/* DSP_CTRL or the timings written: running, stopping, stopped */
static void vp_update(RkVop2State *s, int n)
{
    bool standby = s->regs[R(VP(n) + DSP_CTRL)] & DSP_STANDBY;

    if (!standby && vp_timed(s, n)) {
        s->running[n] = true;
        s->standby_due[n] = false;
    } else if (standby && s->running[n]) {
        s->standby_due[n] = true;       /* (at the next frame start) */
    } else if (standby && !s->running[n] && !s->standby_due[n]) {
        s->int_raw[2 + n] |= INT_DSP_HOLD_VALID;
        vop2_irq_update(s);
    }
    vp_schedule(s, n);
}

static uint64_t vop2_read(void *opaque, hwaddr off, unsigned size)
{
    RkVop2State *s = opaque;
    int i = int_index(off);

    off &= ~3;
    if (i >= 0) {
        switch (off & 0xf) {
        case INT_EN:
            return s->int_en[i];
        case INT_STATUS:
            return s->int_raw[i] & s->int_en[i];
        case INT_RAW:
            return s->int_raw[i];
        default:                        /* (CLR) */
            return 0;
        }
    }
    switch (off) {
    case REG_CFG_DONE:                  /* (its write enables read 0) */
        return (s->glb_en ? CFG_DONE_GLB_EN : 0) | s->pending;
    case VERSION_INFO:
        return VOP_VERSION;
    default:
        return s->regs[R(off)];
    }
}

static void vop2_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    RkVop2State *s = opaque;
    int i = int_index(off);

    off &= ~3;
    if (i >= 0) {
        uint32_t mask = v >> 16;

        switch (off & 0xf) {
        case INT_EN:
            s->int_en[i] = (s->int_en[i] & ~mask) | (v & mask);
            break;
        case INT_CLR:
            s->int_raw[i] &= ~(v & mask);
            break;
        }
        vop2_irq_update(s);
        if (i >= 2) {
            vp_schedule(s, i - 2);
        }
        return;
    }
    if (off == REG_CFG_DONE) {
        s->glb_en = v & CFG_DONE_GLB_EN;
        for (int n = 0; n < RK_VOP2_NVP; n++) {
            if ((v & BIT(n)) && (v & BIT(16 + n))) {
                if (s->running[n]) {
                    s->pending |= BIT(n);   /* (taken at its frame start) */
                }
                if (n == 0) {
                    s->dirty = true;
                }
                vp_schedule(s, n);
            }
        }
        return;
    }
    if (off == VERSION_INFO) {
        return;
    }
    s->regs[R(off)] = v;
    for (int n = 0; n < RK_VOP2_NVP; n++) {
        if (off == VP(n) + DSP_CTRL || off == VP(n) + DSP_VTOTAL_VS_END) {
            vp_update(s, n);
        }
    }
}

static const MemoryRegionOps vop2_ops = {
    .read = vop2_read,
    .write = vop2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------ scanout */

/* @len bytes at @iova, through the IOMMU, a page at a time (what is not
 * mapped reads as 0) */
static void vop2_fetch(RkVop2State *s, uint32_t iova, uint8_t *buf, int len)
{
    while (len > 0) {
        int chunk = MIN(len, 0x1000 - (iova & 0xfff));
        hwaddr pa;

        if (s->mmu && rk_iommu_translate(s->mmu, iova, &pa)) {
            address_space_read(&address_space_memory, pa,
                               MEMTXATTRS_UNSPECIFIED, buf, chunk);
        } else if (!s->mmu) {
            address_space_read(&address_space_memory, iova,
                               MEMTXATTRS_UNSPECIFIED, buf, chunk);
        } else {
            memset(buf, 0, chunk);
        }
        iova += chunk;
        buf += chunk;
        len -= chunk;
    }
}

/* window @base (a Cluster's, or an Esmart's) of video port @n onto the
 * surface */
static void vop2_window(RkVop2State *s, hwaddr base, uint32_t *fb,
                        int fb_w, int fb_h, int stride_px)
{
    bool cluster = base < 0x1800;
    uint32_t ctrl = s->regs[R(base + (cluster ? 0x000 : 0x010))];
    uint32_t mst = s->regs[R(base + (cluster ? 0x010 : 0x014))];
    uint32_t stride = (s->regs[R(base + (cluster ? 0x018 : 0x01c))] &
                       0xffff) * 4;
    uint32_t act = s->regs[R(base + 0x020)], dsp = s->regs[R(base + 0x024)];
    uint32_t st = s->regs[R(base + 0x028)];
    int format = (ctrl >> 1) & 0x1f, bpp;
    int src_w = (act & 0xffff) + 1, src_h = (act >> 16) + 1;
    int dst_w = (dsp & 0xffff) + 1, dst_h = (dsp >> 16) + 1;
    int dst_x = st & 0xffff, dst_y = st >> 16;
    bool swap = ctrl & BIT(14);
    g_autofree uint8_t *line = NULL;

    if (!(ctrl & 1) || (cluster && (!(s->regs[R(base + 0x100)] & 1) ||
                                    (s->regs[R(base + 0x100)] & BIT(1))))) {
        return;                         /* off, or AFBC */
    }
    switch (format) {
    case 0: bpp = 4; break;             /* ARGB8888/XRGB8888 */
    case 1: bpp = 3; break;             /* RGB888 */
    case 2: bpp = 2; break;             /* RGB565 */
    default: return;                    /* (10-bit, YUV: not shown) */
    }
    line = g_malloc(src_w * bpp);
    for (int y = 0; y < dst_h && dst_y + y < fb_h; y++) {
        int sy = (int64_t)y * src_h / dst_h;
        uint32_t *out = fb + (dst_y + y) * stride_px;

        vop2_fetch(s, mst + sy * stride, line, src_w * bpp);
        for (int x = 0; x < dst_w && dst_x + x < fb_w; x++) {
            const uint8_t *p = line + ((int64_t)x * src_w / dst_w) * bpp;
            uint32_t r, g, b;

            if (bpp == 2) {
                uint16_t v = p[0] | p[1] << 8;

                r = (v >> 11) << 3;
                g = ((v >> 5) & 0x3f) << 2;
                b = (v & 0x1f) << 3;
            } else {
                b = p[0];
                g = p[1];
                r = p[2];
            }
            if (swap) {
                uint32_t t = r;

                r = b;
                b = t;
            }
            out[dst_x + x] = r << 16 | g << 8 | b;
        }
    }
}

static bool vop2_gfx_update(void *opaque)
{
    RkVop2State *s = opaque;
    static const hwaddr layers[4] = { 0x1000, 0x1200, 0x1800, 0x1c00 };
    uint32_t hact = s->regs[R(VP(0) + DSP_HACT_ST_END)];
    uint32_t vact = s->regs[R(VP(0) + DSP_VACT_ST_END)];
    int w = (int)(hact & 0xffff) - (int)(hact >> 16);
    int h = (int)(vact & 0xffff) - (int)(vact >> 16);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    DisplaySurface *surface;
    uint32_t *fb, bg, sel;
    int stride_px;

    if (!s->running[0] || w <= 0 || h <= 0) {
        w = 640;                        /* no signal: black */
        h = 480;
    }
    surface = qemu_console_surface(s->con);
    if (surface_width(surface) != w || surface_height(surface) != h) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->dirty = true;
    }
    if (!s->dirty && now - s->last_copy < NANOSECONDS_PER_SECOND) {
        return true;                    /* (nothing new since) */
    }
    s->dirty = false;
    s->last_copy = now;
    fb = surface_data(surface);
    stride_px = surface_stride(surface) / 4;
    bg = s->regs[R(VP(0) + DSP_BG)];    /* (10 bits each) */
    bg = s->running[0] ? ((bg >> 22) & 0xff) << 16 |
                         ((bg >> 12) & 0xff) << 8 | ((bg >> 2) & 0xff) : 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * stride_px + x] = bg;
        }
    }
    if (s->running[0]) {
        sel = s->regs[R(OVL_LAYER_SEL(0))];
        for (int z = 0; z < 4; z++) {       /* (bottom first) */
            int id = (sel >> (4 * z)) & 0xf;

            if (id < 4) {
                vop2_window(s, layers[id], fb, w, h, stride_px);
            }
        }
    }
    qemu_console_update_full(s->con);
    return true;
}

static const GraphicHwOps vop2_gfx_ops = {
    .gfx_update = vop2_gfx_update,
};

/* ------------------------------------------------------------------ */

static void vop2_reset(DeviceState *dev)
{
    RkVop2State *s = RK_VOP2(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->int_en, 0, sizeof(s->int_en));
    memset(s->int_raw, 0, sizeof(s->int_raw));
    s->pending = 0;
    s->glb_en = false;
    for (int n = 0; n < RK_VOP2_NVP; n++) {
        /* the ports in standby, as the driver finds them */
        s->regs[R(VP(n) + DSP_CTRL)] = DSP_STANDBY;
        s->running[n] = s->standby_due[n] = false;
        timer_del(s->frame[n]);
    }
    s->dirty = true;
    vop2_irq_update(s);
}

static void vop2_realize(DeviceState *dev, Error **errp)
{
    RkVop2State *s = RK_VOP2(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mr, OBJECT(s), &vop2_ops, s, "rk3576.vop",
                          RK_VOP2_SIZE);
    sysbus_init_mmio(sbd, &s->mr);
    sysbus_init_irq(sbd, &s->irq_sys);
    for (int n = 0; n < RK_VOP2_NVP; n++) {
        s->port[n].s = s;
        s->port[n].n = n;
        s->frame[n] = timer_new_ns(QEMU_CLOCK_VIRTUAL, vp_frame, &s->port[n]);
        sysbus_init_irq(sbd, &s->irq_vp[n]);
    }
    s->con = qemu_graphic_console_create(dev, 0, &vop2_gfx_ops, s);
    qemu_console_resize(s->con, 640, 480);
}

static void vop2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Rockchip RK3576 VOP2";
    dc->realize = vop2_realize;
    device_class_set_legacy_reset(dc, vop2_reset);
}

static const TypeInfo vop2_types[] = {
    {
        .name = TYPE_RK_VOP2,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RkVop2State),
        .class_init = vop2_class_init,
    },
};

DEFINE_TYPES(vop2_types)
