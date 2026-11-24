/*
 * The RP2350's double-precision coprocessor (DCP): CP4 on each core
 *
 * Each core has two, the Secure one and the Non-secure one: CP4 is the one
 * of the core's present security state, CP5 the Non-secure one (Secure code
 * only). Software uses them only in the "canned" instruction sequences of
 * the Pico SDK (hardware/dcp_canned.inc.S): operands written unpacked
 * (WXUP, WYUP; WXYU two singles; WXIC, WXUC, WXDC, WXFC for conversions),
 * a few steps, the result read back packed (RDDA ... RDDG, RDFA ... RDFG,
 * RDIC, RDUC, RCMP). In a multiply, divide or square root the core does
 * the mantissas' arithmetic itself: it reads them (RXMS, RYMS, RXYH, and
 * RYMR's and RXMQ's first approximations) and writes its product, quotient
 * or root back (WXMS, WXMO, WXDD, WXDQ, WXFM, WXFD, WXFQ) for the DCP to
 * normalise and round.
 *
 * The datasheet (3.6.2) gives the instructions in outline only, not the
 * bits of the intermediate formats. So this model keeps the operands as
 * they were written and works out each result at the read that ends its
 * sequence, as IEEE 754 does it with what the datasheet says the DCP does
 * differently: denormals are zero on input and output; rounding is to
 * nearest, ties to even; a NaN result is the default NaN. What the core
 * reads in the middle of a sequence is the operands' mantissas (Q62 with
 * the leading 1, shifted as asked) and plain approximations; what it writes
 * back is not needed, as no canned sequence branches on any of it.
 *
 * The status the SDK reads before using the DCP (PCMP: is another sequence
 * in progress, which an interrupt would have to save?) has the engaged flag
 * in N: set by every instruction of a sequence but its final read, which
 * clears it (a P "peek" read leaves it). After ADD0, RCMP's Z, C and V are
 * the comparison: equal Z and C, greater C, less none, unordered C and V.
 * RXVD gives the X operand's sign, zero, infinity and NaN in N, Z, C, V
 * (an assumption: nothing the firmware uses reads it). Saving and
 * restoring the state (RXMD, RYMD, REFD; WXMD, WYMD, WEFD) carries these
 * operands and flags, which is all an interrupt handler does with them.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "target/arm/cpu.h"
#include "rp2350.h"

#define D_SIGN      0x8000000000000000ull
#define D_EXP       0x7ff0000000000000ull
#define D_NAN       0x7ff8000000000000ull       /* the default NaN */
#define F_NAN       0x7fc00000u

#define ST_Z        0x40000000u
#define ST_C        0x20000000u
#define ST_V        0x10000000u

static double d_of(uint64_t v)
{
    double d;

    memcpy(&d, &v, sizeof(d));
    return d;
}

static uint64_t bits_of(double d)
{
    uint64_t v;

    memcpy(&v, &d, sizeof(v));
    return v;
}

static float f_of(uint32_t v)
{
    float f;

    memcpy(&f, &v, sizeof(f));
    return f;
}

static uint32_t fbits_of(float f)
{
    uint32_t v;

    memcpy(&v, &f, sizeof(v));
    return v;
}

/* a denormal is zero, keeping its sign */
static uint64_t dcp_ftz(uint64_t v)
{
    return v & D_EXP ? v : v & D_SIGN;
}

static double dcp_single(uint32_t v)
{
    return v & 0x7f800000u ? (double)f_of(v) : (v & 0x80000000u ? -0.0 : 0.0);
}

static uint64_t dcp_double_result(double d)
{
    return isnan(d) ? D_NAN : dcp_ftz(bits_of(d));
}

static uint32_t dcp_single_result(float f)
{
    uint32_t v = fbits_of(f);

    if (isnan(f)) {
        return F_NAN;
    }
    return v & 0x7f800000u ? v : v & 0x80000000u;
}

/* ADD0: X against Y */
static uint32_t dcp_compare(double a, double b)
{
    if (isnan(a) || isnan(b)) {
        return ST_C | ST_V;
    }
    if (a == b) {
        return ST_Z | ST_C;
    }
    return a > b ? ST_C : 0;
}

/* a mantissa, Q62 with its leading 1 (0 for a zero), shifted right */
static uint64_t dcp_mantissa(uint64_t v, int shift)
{
    uint64_t m;

    if (!(v & D_EXP)) {
        return 0;
    }
    m = ((v & ~(D_SIGN | D_EXP)) | (1ull << 52)) << 10;
    return shift < 64 ? m >> shift : 0;
}

/* RDIC, RDUC: truncated (NTDC) or rounded (NRDC), saturated; NaN is 0 */
static uint32_t dcp_integer(double v, bool nearest, bool is_unsigned)
{
    if (isnan(v)) {
        return 0;
    }
    v = nearest ? nearbyint(v) : trunc(v);
    if (is_unsigned) {
        return v <= 0 ? 0 : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
    }
    if (v <= -2147483648.0) {
        return (uint32_t)INT32_MIN;
    }
    return v >= 2147483647.0 ? INT32_MAX : (uint32_t)(int32_t)v;
}

static void dcp_write(RP2Dcp *d, ARMMCoprocInsn *in)
{
    uint32_t lo = in->value, hi = in->value >> 32;

    switch (in->opc1 << 4 | in->crm) {
    case 0x00: d->x = in->value; break;                         /* WXMD */
    case 0x01: d->y = in->value; break;                         /* WYMD */
    case 0x02:                                                  /* WEFD */
        d->status = lo & (ST_Z | ST_C | ST_V);
        d->nearest = lo & 1;
        d->engaged = hi & 1;
        return;
    case 0x10: d->x = dcp_ftz(in->value); break;                /* WXUP */
    case 0x11: d->y = dcp_ftz(in->value); break;                /* WYUP */
    case 0x12:                                                  /* WXYU */
        d->x = bits_of(dcp_single(lo));
        d->y = bits_of(dcp_single(hi));
        break;
    case 0x60:                                                  /* WXUC */
        d->x = bits_of((double)lo);
        d->y = 0;
        break;
    case 0x70:                                                  /* WXIC */
        d->x = bits_of((double)(int32_t)lo);
        d->y = 0;
        break;
    case 0x80:                                                  /* WXDC */
        d->x = dcp_ftz(in->value);
        d->y = 0;
        break;
    case 0x92:                                                  /* WXFC */
        d->x = bits_of(dcp_single(lo));
        d->y = 0;
        break;
    case 0x20: case 0x30: case 0x40: case 0x50:     /* WXMS WXMO WXDD WXDQ */
    case 0xa0: case 0xb0: case 0xc0:                /* WXFM WXFD WXFQ */
        break;                  /* the core's mantissas: the result is ours */
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rp2350: DCP: MCRR #%d, c%d is not "
                      "an instruction\n", in->opc1, in->crm);
    }
    d->engaged = true;
}

static void dcp_cdp(RP2Dcp *d, ARMMCoprocInsn *in)
{
    switch (in->opc1 << 8 | in->crm << 4 | in->opc2) {
    case 0x000:                                                 /* INIT */
        d->x = d->y = 0;
        d->status = 0;
        d->nearest = false;
        break;
    case 0x010:                                                 /* ADD0 */
        d->status = dcp_compare(d_of(d->x), d_of(d->y));
        break;
    case 0x802:                                                 /* NTDC */
        d->nearest = false;
        break;
    case 0x803:                                                 /* NRDC */
        d->nearest = true;
        break;
    default:            /* ADD1 SUB1 SQR0 NORM NRDF NRDD: at the read */
        break;
    }
    d->engaged = true;
}

/* MRC: a word (with Rt the PC, its top four bits are N, Z, C and V) */
static bool dcp_read32(RP2Dcp *d, ARMMCoprocInsn *in, uint32_t *out)
{
    double x = d_of(d->x), y = d_of(d->y);
    float fx = x, fy = y;

    switch (in->crm << 4 | in->opc2) {
    case 0x00:                                                  /* RXVD */
        *out = (d->x & D_SIGN ? 0x80000000u : 0) |
               (x == 0 ? ST_Z : 0) | (isinf(x) ? ST_C : 0) |
               (isnan(x) ? ST_V : 0) | 1;
        return true;
    case 0x01:                                                  /* RCMP */
        *out = (d->engaged ? 0x80000000u : 0) | d->status;
        return true;
    case 0x20: *out = dcp_single_result(fx + fy); return true;  /* RDFA */
    case 0x21: *out = dcp_single_result(fx - fy); return true;  /* RDFS */
    case 0x22: *out = dcp_single_result(fx * fy); return true;  /* RDFM */
    case 0x23: *out = dcp_single_result(fx / fy); return true;  /* RDFD */
    case 0x24: *out = dcp_single_result(sqrtf(fx)); return true; /* RDFQ */
    case 0x25: *out = dcp_single_result((float)x); return true; /* RDFG */
    case 0x30: *out = dcp_integer(x, d->nearest, false); return true;
    case 0x31: *out = dcp_integer(x, d->nearest, true); return true;
    }
    return false;
}

/* what a read does to the engaged flag (its P form leaves it) */
typedef enum { DCP_BAD, DCP_ENDS, DCP_GOES_ON, DCP_KEEPS } DcpRead;

/* MRRC: two words, Rt the low one */
static DcpRead dcp_read64(RP2Dcp *d, ARMMCoprocInsn *in, uint64_t *out)
{
    double x = d_of(d->x), y = d_of(d->y);

    switch (in->opc1 << 4 | in->crm) {
    case 0x10: *out = dcp_double_result(x + y); return DCP_ENDS;    /* RDDA */
    case 0x30: *out = dcp_double_result(x - y); return DCP_ENDS;    /* RDDS */
    case 0x50: *out = dcp_double_result(x * y); return DCP_ENDS;    /* RDDM */
    case 0x70: *out = dcp_double_result(x / y); return DCP_ENDS;    /* RDDD */
    case 0x90: *out = dcp_double_result(sqrt(x)); return DCP_ENDS;  /* RDDQ */
    case 0xb0: *out = dcp_double_result(x); return DCP_ENDS;        /* RDDG */
    case 0x08: *out = d->x; return DCP_KEEPS;                       /* RXMD */
    case 0x09: *out = d->y; return DCP_ENDS;        /* RYMD: engaged = 0 */
    case 0x0a:                                                      /* REFD */
        *out = d->status | d->nearest | (uint64_t)d->engaged << 32;
        return DCP_KEEPS;
    case 0x11:                                  /* RXYH: Y's top, X's top */
        *out = dcp_mantissa(d->y, 0) >> 32 |
               (dcp_mantissa(d->x, 0) >> 32) << 32;
        return DCP_GOES_ON;
    case 0x21:                  /* RYMR: a reciprocal's first guess, Y's top */
    case 0x41:                  /* RXMQ: a root's first guess, X's top */
        *out = 0x80000000u |
               (dcp_mantissa(in->opc1 == 2 ? d->y : d->x, 0) >> 32) << 32;
        return DCP_GOES_ON;
    }
    switch (in->crm) {
    case 4: *out = dcp_mantissa(d->x, in->opc1); return DCP_GOES_ON; /* RXMS */
    case 5: *out = dcp_mantissa(d->y, in->opc1); return DCP_GOES_ON; /* RYMS */
    }
    return DCP_BAD;
}

void rp2350_dcp(void *opaque, ARMCPU *cpu, ARMMCoprocInsn *in)
{
    RP2350Core *c = opaque;
    /* CP4: the present security state's; CP5: the Non-secure one */
    RP2Dcp *d = &c->dcp[in->cp == 5 || !cpu->env.v7m.secure];
    DcpRead r = DCP_BAD;

    switch (in->kind) {
    case ARM_MCOPROC_MCRR:
        dcp_write(d, in);
        return;
    case ARM_MCOPROC_CDP:
        dcp_cdp(d, in);
        return;
    case ARM_MCOPROC_MRC: {
        uint32_t v = 0;

        if (dcp_read32(d, in, &v)) {
            r = DCP_ENDS;
        }
        in->value = v;
        break;
    }
    case ARM_MCOPROC_MRRC:
        r = dcp_read64(d, in, &in->value);
        break;
    default:
        break;
    }
    if (r == DCP_BAD) {
        qemu_log_mask(LOG_GUEST_ERROR, "rp2350: DCP: not an instruction "
                      "(kind %d, #%d, c%d, c%d, #%d)\n", in->kind, in->opc1,
                      in->crn, in->crm, in->opc2);
        in->value = 0;
    } else if (!in->two && r != DCP_KEEPS) {
        d->engaged = r == DCP_GOES_ON;
    }
}

void rp2350_dcp_reset(RP2350State *s)
{
    for (int i = 0; i < 2; i++) {
        memset(s->core[i].dcp, 0, sizeof(s->core[i].dcp));
    }
}
