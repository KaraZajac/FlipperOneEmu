/*
 * The RK3576's SCMI server, as its TF-A serves Linux the CPU and GPU clocks
 *
 * The transport is SMC: Linux puts a message in the shared memory at
 * 0x4010f000 (in the first 2 MiB of DRAM, which TF-A keeps: the kernel's
 * memory starts after them) and calls SMC 0x82000010; the answer is there
 * when the SMC returns (x0 0). The shared memory as SCMI has it: the
 * channel's status at 0x04 (bit 0 free), the length at 0x14 (the header's
 * 4 bytes and the payload's), the message header at 0x18 (message 7:0,
 * type 9:8, protocol 17:10, token 27:18), the payload at 0x1c, an answer's
 * status first. Two protocols:
 *   base (0x10), version 2.0: one protocol besides itself, no other agent
 *   clock (0x14), version 1.0: 457 clocks, as the device tree numbers them
 *       (rockchip,rk3576-cru.h), of which three are there: SCMI_ARMCLK_L
 *       (10, the A53s), SCMI_ARMCLK_B (11, the A72s), SCMI_CLK_GPU (456);
 *       each takes any rate from 24 MHz to 2.4 GHz in 1 MHz steps, and
 *       keeps it; the others are NOT_FOUND
 * Its vendor is "FlipperOneEmu", its sub-vendor "TF-A": it stands in for
 * Rockchip's firmware, whose own names are not known here. A rate changes
 * nothing else: the emulated cores run as fast as they run.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "rk3576.h"

#define SHM_STATUS      0x04
#define SHM_LENGTH      0x14
#define SHM_HEADER      0x18
#define SHM_PAYLOAD     0x1c
#define SHM_SIZE        0x100

#define CHANNEL_FREE    1

enum { SCMI_SUCCESS = 0, SCMI_NOT_SUPPORTED = -1, SCMI_INVALID = -2,
       SCMI_NOT_FOUND = -4 };

#define PROT_BASE       0x10
#define PROT_CLOCK      0x14

#define N_CLOCKS        457
static const struct {
    uint32_t id;
    const char *name;
    uint64_t boot_hz;
} scmi_clocks[] = {
    { 10, "scmi_armclk_l", 816000000 },     /* (the CRU's own clocks are */
    { 11, "scmi_armclk_b", 816000000 },     /* armclk_l and so on: a name */
    { 456, "scmi_clk_gpu", 198000000 },     /* is a clock's in Linux) */
};
#define RATE_MIN        24000000ull
#define RATE_MAX        2400000000ull
#define RATE_STEP       1000000ull

static int scmi_clock(uint32_t id)
{
    for (int i = 0; i < ARRAY_SIZE(scmi_clocks); i++) {
        if (scmi_clocks[i].id == id) {
            return i;
        }
    }
    return -1;
}

static void put32(uint8_t *p, int *n, uint32_t v)
{
    stl_le_p(p + *n, v);
    *n += 4;
}

static void put_name(uint8_t *p, int *n, const char *name)
{
    memset(p + *n, 0, 16);
    memcpy(p + *n, name, MIN(strlen(name), 15));
    *n += 16;
}

/* a command's answer, after its status, in @out (@n bytes); its status */
static int32_t scmi_base(RkScmi *s, int msg, const uint8_t *in, int in_n,
                         uint8_t *out, int *n)
{
    switch (msg) {
    case 0x0:                                   /* PROTOCOL_VERSION */
        put32(out, n, 0x20000);
        return SCMI_SUCCESS;
    case 0x1:                                   /* PROTOCOL_ATTRIBUTES */
        put32(out, n, 1);                       /* 1 protocol, 0 agents */
        return SCMI_SUCCESS;
    case 0x2:                                   /* PROTOCOL_MESSAGE_ATTRIB */
        if (in_n < 4 || ldl_le_p(in) > 0x6) {
            return SCMI_NOT_FOUND;
        }
        put32(out, n, 0);
        return SCMI_SUCCESS;
    case 0x3:                                   /* DISCOVER_VENDOR */
        put_name(out, n, "FlipperOneEmu");
        return SCMI_SUCCESS;
    case 0x4:                                   /* DISCOVER_SUB_VENDOR */
        put_name(out, n, "TF-A");
        return SCMI_SUCCESS;
    case 0x5:                                   /* DISCOVER_IMPLEMENTATION */
        put32(out, n, 1);
        return SCMI_SUCCESS;
    case 0x6:                                   /* DISCOVER_LIST_PROTOCOLS */
        if (in_n >= 4 && ldl_le_p(in) == 0) {
            put32(out, n, 1);
            put32(out, n, PROT_CLOCK);          /* (one byte, padded) */
        } else {
            put32(out, n, 0);
        }
        return SCMI_SUCCESS;
    }
    return SCMI_NOT_SUPPORTED;
}

static int32_t scmi_clock_cmd(RkScmi *s, int msg, const uint8_t *in,
                              int in_n, uint8_t *out, int *n)
{
    int c = in_n >= 4 ? scmi_clock(ldl_le_p(in)) : -1;
    uint64_t hz;

    switch (msg) {
    case 0x0:                                   /* PROTOCOL_VERSION */
        put32(out, n, 0x10000);
        return SCMI_SUCCESS;
    case 0x1:                                   /* PROTOCOL_ATTRIBUTES */
        put32(out, n, N_CLOCKS);                /* no asynchronous ones */
        return SCMI_SUCCESS;
    case 0x2:                                   /* PROTOCOL_MESSAGE_ATTRIB */
        if (in_n < 4 || ldl_le_p(in) > 0x7) {
            return SCMI_NOT_FOUND;
        }
        put32(out, n, 0);
        return SCMI_SUCCESS;
    case 0x3:                                   /* CLOCK_ATTRIBUTES */
        if (c < 0) {
            return SCMI_NOT_FOUND;
        }
        put32(out, n, s->clk_on[c]);
        put_name(out, n, scmi_clocks[c].name);
        return SCMI_SUCCESS;
    case 0x4:                                   /* CLOCK_DESCRIBE_RATES */
        if (c < 0) {
            return SCMI_NOT_FOUND;
        }
        put32(out, n, 3 | 1 << 12);             /* a range: min, max, step */
        put32(out, n, (uint32_t)RATE_MIN);
        put32(out, n, RATE_MIN >> 32);
        put32(out, n, (uint32_t)RATE_MAX);
        put32(out, n, RATE_MAX >> 32);
        put32(out, n, (uint32_t)RATE_STEP);
        put32(out, n, RATE_STEP >> 32);
        return SCMI_SUCCESS;
    case 0x5:                                   /* CLOCK_RATE_SET */
        if (in_n < 16) {
            return SCMI_INVALID;
        }
        c = scmi_clock(ldl_le_p(in + 4));
        if (c < 0) {
            return SCMI_NOT_FOUND;
        }
        hz = ldl_le_p(in + 8) | (uint64_t)ldl_le_p(in + 12) << 32;
        if (hz < RATE_MIN || hz > RATE_MAX) {
            return SCMI_INVALID;
        }
        s->clk_hz[c] = hz / RATE_STEP * RATE_STEP;
        return SCMI_SUCCESS;
    case 0x6:                                   /* CLOCK_RATE_GET */
        if (c < 0) {
            return SCMI_NOT_FOUND;
        }
        put32(out, n, (uint32_t)s->clk_hz[c]);
        put32(out, n, s->clk_hz[c] >> 32);
        return SCMI_SUCCESS;
    case 0x7:                                   /* CLOCK_CONFIG_SET */
        if (c < 0) {
            return SCMI_NOT_FOUND;
        }
        if (in_n < 8) {
            return SCMI_INVALID;
        }
        s->clk_on[c] = ldl_le_p(in + 4) & 1;
        return SCMI_SUCCESS;
    }
    return SCMI_NOT_SUPPORTED;
}

void rk_scmi_reset(RkScmi *s)
{
    uint32_t free = cpu_to_le32(CHANNEL_FREE);

    for (int i = 0; i < ARRAY_SIZE(scmi_clocks); i++) {
        s->clk_hz[i] = scmi_clocks[i].boot_hz;
        s->clk_on[i] = true;
    }
    address_space_write(&address_space_memory, RK3576_SCMI_SHMEM + SHM_STATUS,
                        MEMTXATTRS_UNSPECIFIED, &free, 4);
}

void rk_scmi_call(RkScmi *s)
{
    uint8_t shm[SHM_SIZE], out[SHM_SIZE - SHM_PAYLOAD - 4];
    uint32_t header, len;
    int32_t status;
    int n = 0, in_n;

    address_space_read(&address_space_memory, RK3576_SCMI_SHMEM,
                       MEMTXATTRS_UNSPECIFIED, shm, sizeof(shm));
    header = ldl_le_p(shm + SHM_HEADER);
    len = ldl_le_p(shm + SHM_LENGTH);
    in_n = MIN(MAX((int)len - 4, 0), SHM_SIZE - SHM_PAYLOAD);
    switch ((header >> 10) & 0xff) {
    case PROT_BASE:
        status = scmi_base(s, header & 0xff, shm + SHM_PAYLOAD, in_n, out,
                           &n);
        break;
    case PROT_CLOCK:
        status = scmi_clock_cmd(s, header & 0xff, shm + SHM_PAYLOAD, in_n,
                                out, &n);
        break;
    default:
        status = SCMI_NOT_SUPPORTED;
    }
    if (status != SCMI_SUCCESS) {
        n = 0;
    }
    stl_le_p(shm + SHM_PAYLOAD, status);
    memcpy(shm + SHM_PAYLOAD + 4, out, n);
    stl_le_p(shm + SHM_LENGTH, 8 + n);
    stl_le_p(shm + SHM_STATUS, CHANNEL_FREE);
    address_space_write(&address_space_memory, RK3576_SCMI_SHMEM,
                        MEMTXATTRS_UNSPECIFIED, shm, SHM_PAYLOAD + 4 + n);
}
