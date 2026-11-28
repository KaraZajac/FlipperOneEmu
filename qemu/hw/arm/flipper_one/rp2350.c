/*
 * Raspberry Pi RP2350 (the Flipper One's MCU, an RP2350B)
 *
 * Its own bus, not QEMU's system memory: the RK3576 beside it on the
 * Flipper One has another map (DRAM where the RP2350 has its peripherals).
 *
 *   0x00000000  boot ROM, 32 KiB (Raspberry Pi's image, -bios)
 *   0x10000000  XIP flash, 16 MiB; 0x14000000 uncached, 0x1c000000
 *               untranslated (the same flash, as yet)
 *   0x13ffc000  XIP cache as SRAM, 16 KiB
 *   0x20000000  SRAM, 520 KiB (SRAM0-7 striped, SRAM8/9 = scratch X/Y)
 *   0x40000000  APB peripherals, 0x50000000 AHB, 0xd0000000 SIO
 *   0xe0000000  the Cortex-M33's PPB (QEMU's armv7m)
 *
 * What is not modelled yet is an unimplemented device: -d unimp logs it.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "hw/core/cpu.h"
#include "qemu/main-loop.h"
#include "target/arm/tcg/idau.h"
#include "hw/char/pl011.h"
#include "system/system.h"
#include "system/reset.h"
#include "rp2350.h"

/*
 * The RP2350's IDAU, fixed in the chip.  Its map of the boot ROM is in the
 * ROM's arm8_bootrom_rt0.S:
 *   0x0000-0x42ff  Exempt: text that both states run
 *   0x4300-0x7dff  Secure for instruction fetches, Exempt for data; QEMU's
 *                  IDAU knows no access type, so Exempt
 *   0x7e00-0x7fff  Secure, NonSecure-callable: the SG entry points (IDAU
 *                  region 2, which the ROM checks with TT at its start)
 * The peripherals (APB 0x40000000, AHB 0x50000000, the USB controller's RAM
 * among them) and SIO are Exempt too: on the RP2350 it is ACCESSCTRL, not
 * the SAU, that says who reaches a peripheral (the ROM's nsboot, running
 * NonSecure with SAU regions for RAM only, works the USB controller after
 * ACCESSCTRL let it); ACCESSCTRL's filter is not modelled. So is the PPB.
 * The rest is NonSecure to the IDAU: the SAU decides.
 */
static void rp2350_idau_check(IDAUInterface *ii, uint32_t address,
                              int *iregion, bool *exempt, bool *ns, bool *nsc)
{
    *iregion = IREGION_NOTVALID;
    *exempt = false;
    *ns = true;
    *nsc = false;
    if (address < 0x7e00) {
        *exempt = true;
    } else if (address < 0x8000) {
        *ns = false;
        *nsc = true;
        *iregion = 2;
    } else if ((address >= 0x40000000 && address < 0x60000000) ||
               (address & 0xf0000000) == 0xd0000000 ||
               (address & 0xfff00000) == 0xe0000000) {
        *exempt = true;
    }
}

static const struct {
    const char *name;
    hwaddr base, size;
} rp2350_unimp[] = {
    { "sysinfo",        0x40000000, 0x8000 },
    { "syscfg",         0x40008000, 0x8000 },
    { "clocks",         0x40010000, 0x8000 },
    { "psm",            0x40018000, 0x8000 },
    { "resets",         0x40020000, 0x8000 },
    { "io_bank0",       0x40028000, 0x8000 },
    { "io_qspi",        0x40030000, 0x8000 },
    { "pads_bank0",     0x40038000, 0x8000 },
    { "pads_qspi",      0x40040000, 0x8000 },
    { "xosc",           0x40048000, 0x8000 },
    { "pll_sys",        0x40050000, 0x8000 },
    { "pll_usb",        0x40058000, 0x8000 },
    { "accessctrl",     0x40060000, 0x8000 },
    { "busctrl",        0x40068000, 0x8000 },
    { "uart0",          0x40070000, 0x8000 },
    { "uart1",          0x40078000, 0x8000 },
    { "spi0",           0x40080000, 0x8000 },
    { "spi1",           0x40088000, 0x8000 },
    { "i2c0",           0x40090000, 0x8000 },
    { "i2c1",           0x40098000, 0x8000 },
    { "adc",            0x400a0000, 0x8000 },
    { "pwm",            0x400a8000, 0x8000 },
    { "timer0",         0x400b0000, 0x8000 },
    { "timer1",         0x400b8000, 0x8000 },
    { "hstx_ctrl",      0x400c0000, 0x8000 },
    { "xip_ctrl",       0x400c8000, 0x8000 },
    { "xip_qmi",        0x400d0000, 0x8000 },
    { "watchdog",       0x400d8000, 0x8000 },
    { "bootram.regs",   0x400e0400, 0x7c00 },
    { "rosc",           0x400e8000, 0x8000 },
    { "trng",           0x400f0000, 0x8000 },
    { "sha256",         0x400f8000, 0x8000 },
    { "powman",         0x40100000, 0x8000 },
    { "ticks",          0x40108000, 0x8000 },
    { "otp",            0x40120000, 0x10000 },
    { "otp_data",       0x40130000, 0x10000 },
    { "coresight",      0x40140000, 0x10000 },
    { "dft",            0x40150000, 0x8000 },
    { "glitch_detector", 0x40158000, 0x8000 },
    { "tbman",          0x40160000, 0x8000 },
    { "dma",            0x50000000, 0x100000 },
    { "usbctrl",        0x50100000, 0x100000 },
    { "pio0",           0x50200000, 0x100000 },
    { "pio1",           0x50300000, 0x100000 },
    { "pio2",           0x50400000, 0x100000 },
    { "xip_aux",        0x50500000, 0x100000 },
    { "hstx_fifo",      0x50600000, 0x100000 },
    { "coresight_trace", 0x50700000, 0x100000 },
    { "xip_maintenance", RP2350_XIP_MAINT_BASE, 0x4000000 },
    { "sio",            RP2350_SIO_BASE, 0x40000 },
};

/* an unimplemented device on the RP2350's own bus, under what is modelled */
static void rp2350_add_unimp(RP2350State *s, const char *name, hwaddr base,
                             hwaddr size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion_overlap(&s->bus, base,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), -1000);
}

/* a file's bytes into a ROM's memory: no more than @size */
static bool rp2350_load(MemoryRegion *mr, const char *path, hwaddr size,
                        const char *what, Error **errp)
{
    g_autofree char *data = NULL;
    gsize len;
    GError *err = NULL;

    if (!g_file_get_contents(path, &data, &len, &err)) {
        error_setg(errp, "rp2350: %s: %s", what, err->message);
        g_error_free(err);
        return false;
    }
    if (len > size) {
        error_setg(errp, "rp2350: %s %s is %zu bytes, more than %" HWADDR_PRIu,
                   what, path, (size_t)len, size);
        return false;
    }
    memcpy(memory_region_get_ram_ptr(mr), data, len);
    memory_region_set_dirty(mr, 0, len);
    return true;
}

/* ------------------------------------------- QEMU's devices, aliased */

static uint64_t wrap_read(void *opaque, hwaddr addr, unsigned size)
{
    RP2Wrap *w = opaque;
    uint64_t v = 0;

    if ((addr & 0xffc) != w->write_only) {
        memory_region_dispatch_read(w->target, addr & 0xfff, &v,
                                    size_memop(size) | MO_LE,
                                    MEMTXATTRS_UNSPECIFIED);
    }
    return v;
}

static void wrap_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    RP2Wrap *w = opaque;
    hwaddr reg = addr & 0xfff;
    uint64_t old = 0;
    MemOp op = size_memop(size) | MO_LE;

    if (addr >> 12) {
        if ((reg & ~3) != w->write_only) {
            memory_region_dispatch_read(w->target, reg, &old, op,
                                        MEMTXATTRS_UNSPECIFIED);
        }
        switch (addr >> 12) {
        case 1: value ^= old; break;
        case 2: value |= old; break;
        default: value = old & ~value; break;
        }
    }
    memory_region_dispatch_write(w->target, reg, value, op,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps wrap_ops = {
    .read = wrap_read,
    .write = wrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rp2350_wrap(RP2350State *s, RP2Wrap *w, const char *name,
                        hwaddr base, MemoryRegion *target, hwaddr write_only)
{
    w->target = target;
    w->write_only = write_only;
    memory_region_init_io(&w->mr, OBJECT(s), &wrap_ops, w, name, 0x4000);
    w->mr.disable_reentrancy_guard = true;         /* (the DMA, as blocks) */
    memory_region_add_subregion(&s->bus, base, &w->mr);
}

/* UART1 has the firmware's CLI: the first serial port; UART0 the second.
 * Their lines do not wait for the program on the other end (patch 0016):
 * what it has not read waits in 1 MiB, and past that is lost. Else a
 * program connected to cli.sock that stops reading stops the machine at
 * the firmware's next byte (QEMU's PL011 waits for the chardev, BQL held) */
static void rp2350_uarts_realize(RP2350State *s)
{
    for (int i = 0; i < 2; i++) {
        DeviceState *dev = qdev_new(TYPE_PL011);
        g_autofree char *name = g_strdup_printf("rp2350.uart%d", i);

        qdev_prop_set_uint32(dev, "tx-buffer", 1 * MiB);
        qdev_prop_set_chr(dev, "chardev", i ? serial_hd(0) :
                          s->uart0_chr ? s->uart0_chr : serial_hd(1));
        qdev_connect_clock_in(dev, "clk", s->sysclk);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, rp2350_irq(s, 33 + i));
        s->uart[i] = dev;
        rp2350_wrap(s, &s->uart_wrap[i], name, 0x40070000 + i * 0x8000,
                    sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0),
                    0x44 /* UARTICR */);
    }
}

static void rp2350_init(Object *obj)
{
    RP2350State *s = RP2350(obj);

    object_initialize_child(obj, "cluster", &s->cluster, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster), "cluster-id", 0);
    object_initialize_child(OBJECT(&s->cluster), "cpu0", &s->cpu[0],
                            TYPE_ARMV7M);
    object_initialize_child(OBJECT(&s->cluster), "cpu1", &s->cpu[1],
                            TYPE_ARMV7M);
    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk", NULL, NULL, 0);
}

/* a peripheral's interrupt line goes to both cores' NVICs */
static void rp2350_irq_set(void *opaque, int n, int level)
{
    RP2350State *s = opaque;

    for (int c = 0; c < ARRAY_SIZE(s->cpu); c++) {
        qemu_set_irq(s->cpu_irq[c][n], level);
    }
}

/*
 * PSM FRCE_OFF.PROC1 holds core 1 off; letting go starts it again in its
 * boot ROM. QEMU's arm_set_cpu_off() and arm_set_cpu_on_and_reset() each
 * queue work for the core, and an "on" that comes before the "off" has
 * run finds the core still on and is dropped ("CPU 1 is already on"): the
 * off runs after it and the core stays off. The SDK's
 * multicore_reset_core1(), which the firmware calls as it starts, is an
 * off and an on a few instructions apart, and then waits for core 1's
 * handshake: core 0 would wait forever. Here one piece of work does what
 * the PSM asks when it runs: off, or, if the core was held off since it
 * last started (however briefly), a reset and on.
 */
static void core1_power_work(CPUState *cs, run_on_cpu_data data)
{
    RP2350State *s = data.host_ptr;
    ARMCPU *cpu = ARM_CPU(cs);

    assert(bql_locked());
    s->core1_work = false;
    if (s->core1_held) {
        arm_set_cpu_power_state(cpu, PSCI_OFF);
        cs->halted = 1;
        cs->exception_index = EXCP_HLT;
    } else if (s->core1_cycled) {
        s->core1_cycled = false;
        cpu_reset(cs);
        cs->halted = 0;
        arm_set_cpu_power_state(cpu, PSCI_ON);
    }
}

void rp2350_core1_power(RP2350State *s, bool on)
{
    s->core1_held = !on;
    s->core1_cycled |= !on;
    if (!s->core1_work) {
        s->core1_work = true;
        async_run_on_cpu(CPU(s->cpu[1].cpu), core1_power_work,
                         RUN_ON_CPU_HOST_PTR(s));
    }
}

/*
 * CPUs are not reset with the devices: without this a reset of the chip
 * (the watchdog's, as after a PICOBOOT reboot; SYSRESETREQ) would reset
 * everything but the cores, which would run on where they were. Both
 * start again in the boot ROM, as armv7m_load_kernel() has a board's core
 * reset; without a supply (the board's, cut), they stay off.
 */
static void rp2350_cores_reset(void *opaque)
{
    RP2350State *s = opaque;

    for (int c = 0; c < ARRAY_SIZE(s->cpu); c++) {
        CPUState *cs = CPU(s->cpu[c].cpu);

        cpu_reset(cs);
        if (s->unpowered) {
            arm_set_cpu_power_state(s->cpu[c].cpu, PSCI_OFF);
            cs->halted = 1;
        }
    }
}

static void rp2350_realize(DeviceState *dev, Error **errp)
{
    RP2350State *s = RP2350(dev);
    Object *obj = OBJECT(dev);

    if (!s->bootrom) {
        error_setg(errp, "rp2350: no boot ROM (bootrom=FILE, 32 KiB)");
        return;
    }
    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "rp2350: sysclk must be connected");
        return;
    }
    memory_region_init(&s->bus, obj, "rp2350.bus", 4 * GiB);

    if (!memory_region_init_rom(&s->rom, obj, "rp2350.rom", RP2350_ROM_SIZE,
                                errp) ||
        !rp2350_load(&s->rom, s->bootrom, RP2350_ROM_SIZE, "boot ROM", errp)) {
        return;
    }
    memory_region_add_subregion(&s->bus, RP2350_ROM_BASE, &s->rom);

    /* the flash: erased, then the image; XIP through QMI's windows */
    if (!rp2350_qmi_realize(s, errp)) {
        return;
    }
    /* the OTP: its image, as the chip left the factory if it is blank */
    if (!rp2350_otp_realize(s, errp)) {
        return;
    }

    /* the boot ROM works in the USB controller's RAM too */
    if (!memory_region_init_ram(&s->xip_sram, obj, "rp2350.xip-sram",
                                RP2350_XIP_SRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->sram, obj, "rp2350.sram",
                                RP2350_SRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->bootram, obj, "rp2350.bootram",
                                RP2350_BOOTRAM_SIZE, errp) ||
        !memory_region_init_ram(&s->usb_dpram, obj, "rp2350.usb-dpram",
                                4 * KiB, errp)) {
        return;
    }
    memory_region_add_subregion(&s->bus, RP2350_XIP_SRAM_BASE, &s->xip_sram);
    memory_region_add_subregion(&s->bus, RP2350_SRAM_BASE, &s->sram);
    /* over its registers' block (rp2350_sys.c), which starts there too */
    memory_region_add_subregion_overlap(&s->bus, RP2350_BOOTRAM_BASE,
                                        &s->bootram, 1);
    memory_region_add_subregion(&s->bus, 0x50100000, &s->usb_dpram);

    for (int i = 0; i < ARRAY_SIZE(rp2350_unimp); i++) {
        rp2350_add_unimp(s, rp2350_unimp[i].name, rp2350_unimp[i].base,
                         rp2350_unimp[i].size);
    }
    rp2350_sys_realize(s);
    rp2350_sio_realize(s);

    /*
     * The two cores: Cortex-M33 with FPU, DSP and TrustZone; 4 priority
     * bits, 8 MPU regions in each security state, 8 SAU regions.  Both start
     * in the boot ROM; core 1's waits for core 0 (its salt, then a launch
     * through the SIO FIFO), and SIO tells them apart by the CPU at hand.
     */
    memory_region_init_alias(&s->bus_alias, obj, "rp2350.bus-core1", &s->bus,
                             0, 4 * GiB);
    for (int c = 0; c < ARRAY_SIZE(s->cpu); c++) {
        DeviceState *armv7m = DEVICE(&s->cpu[c]);
        MemoryRegion *bus = c ? &s->bus_alias : &s->bus;

        qdev_prop_set_uint32(armv7m, "num-irq", RP2350_NUM_IRQ);
        qdev_prop_set_uint32(armv7m, "num-prio-bits", 4);
        qdev_prop_set_uint32(armv7m, "mpu-s-regions", 8);
        qdev_prop_set_uint32(armv7m, "mpu-ns-regions", 8);
        qdev_prop_set_string(armv7m, "cpu-type",
                             ARM_CPU_TYPE_NAME("cortex-m33"));
        qdev_prop_set_uint32(armv7m, "init-svtor", RP2350_ROM_BASE);
        qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
        object_property_set_link(OBJECT(armv7m), "memory", OBJECT(bus),
                                 &error_abort);
        object_property_set_link(OBJECT(armv7m), "idau", OBJECT(s),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(armv7m), errp)) {
            return;
        }
        /* an M-profile core has no MPIDR; QEMU's PSCI finds a core by it
         * (arm_get_cpu_by_id()), and the RK3576's A53s are 0x0-0x3: these
         * two take ids that no A-profile core has */
        s->cpu[c].cpu->mp_affinity = 0xff00 | c;
        rp2350_coproc_init(s, c);
        rp2350_ppb_realize(s, c);
    }
    /* the cluster after its cores: an ARMv7M makes its CPU as it is realized,
     * and the cluster numbers the CPUs it has then (as hw/arm/armsse.c);
     * TCG has taken their index already, so only the gdb stub sees this */
    if (!qdev_realize(DEVICE(&s->cluster), NULL, errp)) {
        return;
    }
    qemu_register_reset(rp2350_cores_reset, s);
    /* each core's NVIC inputs, looked up once: a PIO block sets its lines
     * at every step it takes (qdev_get_gpio_in() searches by name) */
    for (int c = 0; c < ARRAY_SIZE(s->cpu); c++) {
        for (int n = 0; n < RP2350_NUM_IRQ; n++) {
            s->cpu_irq[c][n] = qdev_get_gpio_in(DEVICE(&s->cpu[c]), n);
        }
    }
    s->irqs = qemu_allocate_irqs(rp2350_irq_set, s, RP2350_NUM_IRQ);

    /* what has interrupts: after the cores' NVICs */
    rp2350_timer_realize(s);
    rp2350_gpio_realize(s);
    rp2350_pio_realize(s);
    rp2350_uarts_realize(s);
    rp2350_dma_realize(s);
    rp2350_hstx_realize(s);
    rp2350_pwm_realize(s);
    rp2350_i2c_realize(s);
    rp2350_usb_realize(s);
}

qemu_irq rp2350_irq(RP2350State *s, int n)
{
    return s->irqs[n];
}

static void rp2350_reset_hold(Object *obj, ResetType type)
{
    RP2350State *s = RP2350(obj);

    if (s->started) {
        s->bootsel = false;     /* held to start, let go since: a reboot
                                   (PICOBOOT's, the watchdog's) boots */
    }
    s->started = true;
    s->core1_held = s->core1_cycled = false;    /* both cores start */
    rp2350_coproc_reset(RP2350(obj));
    rp2350_sys_reset(RP2350(obj));
    rp2350_sio_reset(RP2350(obj));
    rp2350_qmi_reset(RP2350(obj));
    rp2350_timer_reset(RP2350(obj));
    rp2350_pio_reset(RP2350(obj));
    rp2350_ppb_reset(RP2350(obj));
    rp2350_dma_reset(RP2350(obj));
    rp2350_hstx_reset(RP2350(obj));
    rp2350_pwm_reset(RP2350(obj));
    rp2350_i2c_reset(RP2350(obj));
    rp2350_usb_reset(RP2350(obj));
    rp2350_gpio_reset(RP2350(obj));
}

static const Property rp2350_properties[] = {
    DEFINE_PROP_STRING("bootrom", RP2350State, bootrom),
    DEFINE_PROP_STRING("flash", RP2350State, flash),
    DEFINE_PROP_STRING("otp", RP2350State, otp_path),
    DEFINE_PROP_BOOL("bootsel", RP2350State, bootsel, false),
};

static void rp2350_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    IDAUInterfaceClass *iic = IDAU_INTERFACE_CLASS(oc);

    dc->realize = rp2350_realize;
    device_class_set_props(dc, rp2350_properties);
    rc->phases.hold = rp2350_reset_hold;
    iic->check = rp2350_idau_check;
}

static const TypeInfo rp2350_types[] = {
    {
        .name = TYPE_RP2350,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RP2350State),
        .instance_init = rp2350_init,
        .class_init = rp2350_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_IDAU_INTERFACE },
            { }
        },
    },
};
DEFINE_TYPES(rp2350_types)
