/*
 * The RP2350's coprocessors, on each Cortex-M33's coprocessor port
 * (QEMU patch 0001: arm_m_set_coproc()):
 *
 *   CP0  GPIO: the SIO's GPIO registers, fast       (not yet: logged)
 *   CP4  DCP, double-precision arithmetic           (not yet: logged)
 *   CP7  RCP, redundancy: salted stack canaries, checked booleans and
 *        integers, a sequence counter, random delays, a panic
 *
 * The RCP's instructions are those of the Pico SDK's hardware/rcp.h; their
 * values are its own: a boolean is 0xa500a500 (true) or 0x00c300c3 (false),
 * an integer with its parity word gives 0x96009600.  A canary is a function
 * of the core's salt and the tag; any function does that gives the same
 * value to the same salt and tag, so this one is not the chip's.  Core 0's
 * boot ROM loads both cores' salts (MCRR p7, #8 with CRm the core); before
 * its salt, a core may only ask whether it has one.  A check that fails is
 * a UsageFault here (the chip faults; the boot ROM's fault handler stops on
 * the RCP), and the RCP's panic stops the core where it is.  MCR and MCR2
 * differ in the chip's random delay only.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "target/arm/cpu.h"
#include "rp2350.h"

#define RCP_TRUE    0xa500a500u
#define RCP_FALSE   0x00c300c3u
#define RCP_PARITY  0x96009600u

static bool rcp_valid(uint32_t b)
{
    return b == RCP_TRUE || b == RCP_FALSE;
}

static uint64_t rcp_mix(uint64_t x)
{
    x ^= x >> 31;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 29;
    x *= 0x94d049bb133111ebull;
    return x ^ (x >> 32);
}

static uint32_t rcp_canary(RP2350RCP *r, int core, int tag)
{
    return rcp_mix(r->salt[core] ^ (0x9e3779b97f4a7c15ull * (tag + 1)));
}

static const char *const rcp_kind[] = {
    [ARM_MCOPROC_MCR] = "mcr", [ARM_MCOPROC_MRC] = "mrc",
    [ARM_MCOPROC_MCRR] = "mcrr", [ARM_MCOPROC_MRRC] = "mrrc",
    [ARM_MCOPROC_CDP] = "cdp",
};

static void coproc_log(int mask, ARMCPU *cpu, ARMMCoprocInsn *in,
                       const char *what)
{
    qemu_log_mask(mask, "rp2350: core %d at 0x%08x: %s%s p%d, #%d, "
                  "c%d, c%d, #%d (0x%016" PRIx64 "): %s\n",
                  CPU(cpu)->cpu_index, cpu->env.regs[15], rcp_kind[in->kind],
                  in->two ? "2" : "", in->cp, in->opc1, in->crn, in->crm,
                  in->opc2, in->value, what);
}

static void rp2350_rcp(void *opaque, ARMCPU *cpu, ARMMCoprocInsn *in)
{
    RP2350Core *c = opaque;
    RP2350RCP *r = &c->soc->rcp;
    int core = c->index;
    uint32_t a = in->value, b = in->value >> 32;
    int tag = in->crn << 4 | in->crm;
    bool ok = true;

    /* the salt (either core's), and whether this core has its own */
    if (in->kind == ARM_MCOPROC_MCRR && in->opc1 == 8) {
        int which = in->crm & 1;

        r->salt[which] = in->value;
        r->salted[which] = true;
        r->prng[which] = rcp_mix(in->value) | 1;
        return;
    }
    if (in->kind == ARM_MCOPROC_MRC && in->opc1 == 1) {
        in->value = r->salted[core] ? RCP_TRUE : RCP_FALSE;
        return;
    }
    if (in->kind == ARM_MCOPROC_CDP && in->opc1 == 0 && in->opc2 == 1) {
        coproc_log(LOG_GUEST_ERROR, cpu, in, "RCP panic: the core stops");
        in->fault = ARM_MCOPROC_HALT;
        return;
    }
    if (!r->salted[core]) {
        coproc_log(LOG_GUEST_ERROR, cpu, in, "RCP used before its salt");
        in->fault = ARM_MCOPROC_UNDEF;
        return;
    }

    switch (in->kind) {
    case ARM_MCOPROC_MRC:
        if (in->opc1 == 0 && in->opc2 == 1) {               /* canary get */
            in->value = rcp_canary(r, core, tag);
            return;
        }
        if (in->opc1 == 2) {                                /* random byte */
            r->prng[core] = rcp_mix(r->prng[core]);
            in->value = r->prng[core] >> 56;
            return;
        }
        break;
    case ARM_MCOPROC_MCR:
        switch (in->opc1) {
        case 0:                                             /* canary check */
            if (in->opc2 == 1) {
                ok = a == rcp_canary(r, core, tag);
                goto done;
            }
            break;
        case 1:                                             /* bvalid */
            ok = rcp_valid(a);
            goto done;
        case 2:                                             /* btrue */
            ok = a == RCP_TRUE;
            goto done;
        case 3:                                             /* bfalse */
            ok = a == RCP_FALSE;
            goto done;
        case 4:                                             /* count set */
            r->count[core] = tag;
            return;
        case 5:                                             /* count check */
            ok = r->count[core] == tag;
            r->count[core]++;
            goto done;
        }
        break;
    case ARM_MCOPROC_MCRR:
        switch (in->opc1) {
        case 0:                                             /* b2valid */
            ok = rcp_valid(a) && rcp_valid(b);
            goto done;
        case 1:                                             /* b2and */
            ok = a == RCP_TRUE && b == RCP_TRUE;
            goto done;
        case 2:                                             /* b2or */
            ok = rcp_valid(a) && rcp_valid(b) &&
                 (a == RCP_TRUE || b == RCP_TRUE);
            goto done;
        case 3:                                             /* bxorvalid */
            ok = rcp_valid(a ^ b);
            goto done;
        case 4:                                             /* bxortrue */
            ok = (a ^ b) == RCP_TRUE;
            goto done;
        case 5:                                             /* bxorfalse */
            ok = (a ^ b) == RCP_FALSE;
            goto done;
        case 6:                                             /* ivalid */
            ok = (a ^ b) == RCP_PARITY;
            goto done;
        case 7:                                             /* iequal */
            ok = a == b;
            goto done;
        }
        break;
    default:
        break;
    }
    coproc_log(LOG_UNIMP, cpu, in, "an RCP instruction this model lacks");
    in->fault = ARM_MCOPROC_UNDEF;
    return;
done:
    if (!ok) {
        coproc_log(LOG_GUEST_ERROR, cpu, in, "RCP check failed");
        in->fault = ARM_MCOPROC_UNDEF;
    }
}

/* the GPIO coprocessor and the DCP: logged, until they are modelled */
static void rp2350_coproc_unimp(void *opaque, ARMCPU *cpu, ARMMCoprocInsn *in)
{
    coproc_log(LOG_UNIMP, cpu, in, "not modelled yet");
    in->value = 0;
}

void rp2350_coproc_init(RP2350State *s, int core)
{
    ARMCPU *cpu = s->cpu[core].cpu;

    s->core[core].soc = s;
    s->core[core].index = core;
    arm_m_set_coproc(cpu, 0, rp2350_coproc_unimp, &s->core[core], true);
    arm_m_set_coproc(cpu, 4, rp2350_coproc_unimp, &s->core[core], false);
    arm_m_set_coproc(cpu, 7, rp2350_rcp, &s->core[core], false);
}

void rp2350_coproc_reset(RP2350State *s)
{
    memset(&s->rcp, 0, sizeof(s->rcp));
}
