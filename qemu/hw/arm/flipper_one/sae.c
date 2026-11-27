/*
 * WPA3-Personal's SAE for the MT7921's access point (see sae.h): P-256 in
 * Montgomery form on four 64-bit limbs, its points Jacobian. Nothing here
 * is in constant time: the peer is the guest, and the password its own.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifdef SAE_STANDALONE                   /* (its test, on the host) */
#include <glib.h>
#include <string.h>
#else
#include "qemu/osdep.h"
#endif
#include "sae.h"

typedef struct {
    uint64_t v[4];                      /* (little-endian limbs) */
} Fe;

typedef struct {
    Fe x, y, z;                         /* (Jacobian, Montgomery; z 0: none) */
} Pt;

/* p, the field's prime; n, the group's order; b; and a = -3 */
static const Fe P = { { 0xffffffffffffffffull, 0x00000000ffffffffull,
                        0x0000000000000000ull, 0xffffffff00000001ull } };
static const Fe N = { { 0xf3b9cac2fc632551ull, 0xbce6faada7179e84ull,
                        0xffffffffffffffffull, 0xffffffff00000000ull } };
static const Fe B = { { 0x3bce3c3e27d2604bull, 0x651d06b0cc53b0f6ull,
                        0xb3ebbd55769886bcull, 0x5ac635d8aa3a93e7ull } };

static Fe R2;                           /* 2^512 mod p */
static Fe ONE;                          /* 1 in Montgomery form */
static Fe BM;                           /* b in Montgomery form */
static Fe EXP_INV, EXP_SQRT, EXP_LEG;   /* p - 2, (p + 1) / 4, (p - 1) / 2 */
static bool ready;

static int fe_cmp(const Fe *a, const Fe *b)
{
    for (int i = 3; i >= 0; i--) {
        if (a->v[i] != b->v[i]) {
            return a->v[i] < b->v[i] ? -1 : 1;
        }
    }
    return 0;
}

static bool fe_zero(const Fe *a)
{
    return !(a->v[0] | a->v[1] | a->v[2] | a->v[3]);
}

static uint64_t fe_add_raw(Fe *r, const Fe *a, const Fe *b)
{
    unsigned __int128 c = 0;

    for (int i = 0; i < 4; i++) {
        c += (unsigned __int128)a->v[i] + b->v[i];
        r->v[i] = (uint64_t)c;
        c >>= 64;
    }
    return (uint64_t)c;
}

static uint64_t fe_sub_raw(Fe *r, const Fe *a, const Fe *b)
{
    uint64_t borrow = 0;

    for (int i = 0; i < 4; i++) {
        unsigned __int128 d = (unsigned __int128)a->v[i] - b->v[i] - borrow;

        r->v[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) & 1;
    }
    return borrow;
}

/* r = a + b mod m (a, b < m) */
static void mod_add(Fe *r, const Fe *a, const Fe *b, const Fe *m)
{
    if (fe_add_raw(r, a, b) || fe_cmp(r, m) >= 0) {
        fe_sub_raw(r, r, m);
    }
}

static void fp_sub(Fe *r, const Fe *a, const Fe *b)
{
    if (fe_sub_raw(r, a, b)) {
        fe_add_raw(r, r, &P);
    }
}

/* Montgomery's product mod p (CIOS; -1/p mod 2^64 is 1) */
static void fp_mul(Fe *r, const Fe *a, const Fe *b)
{
    uint64_t t[6] = { 0 };
    Fe x;

    for (int i = 0; i < 4; i++) {
        unsigned __int128 c = 0;
        uint64_t u;

        for (int j = 0; j < 4; j++) {
            c += (unsigned __int128)a->v[j] * b->v[i] + t[j];
            t[j] = (uint64_t)c;
            c >>= 64;
        }
        c += t[4];
        t[4] = (uint64_t)c;
        t[5] = (uint64_t)(c >> 64);
        u = t[0];
        c = (unsigned __int128)u * P.v[0] + t[0];
        c >>= 64;
        for (int j = 1; j < 4; j++) {
            c += (unsigned __int128)u * P.v[j] + t[j];
            t[j - 1] = (uint64_t)c;
            c >>= 64;
        }
        c += t[4];
        t[3] = (uint64_t)c;
        t[4] = t[5] + (uint64_t)(c >> 64);
    }
    x.v[0] = t[0];
    x.v[1] = t[1];
    x.v[2] = t[2];
    x.v[3] = t[3];
    if (t[4] || fe_cmp(&x, &P) >= 0) {
        fe_sub_raw(&x, &x, &P);
    }
    *r = x;
}

static void fp_sqr(Fe *r, const Fe *a)
{
    fp_mul(r, a, a);
}

/* a^e (a in Montgomery form, e plain) */
static void fp_pow(Fe *r, const Fe *a, const Fe *e)
{
    Fe x = ONE;

    for (int i = 255; i >= 0; i--) {
        fp_sqr(&x, &x);
        if (e->v[i / 64] >> (i % 64) & 1) {
            fp_mul(&x, &x, a);
        }
    }
    *r = x;
}

static void to_mont(Fe *r, const Fe *a)
{
    fp_mul(r, a, &R2);
}

static void from_mont(Fe *r, const Fe *a)
{
    Fe one = { { 1, 0, 0, 0 } };

    fp_mul(r, a, &one);
}

static void shr(Fe *r, const Fe *a, int k)
{
    for (int i = 0; i < 4; i++) {
        r->v[i] = a->v[i] >> k | (i < 3 ? a->v[i + 1] << (64 - k) : 0);
    }
}

static void init(void)
{
    Fe one = { { 1, 0, 0, 0 } }, two = { { 2, 0, 0, 0 } }, x;

    if (ready) {
        return;
    }
    R2 = one;                           /* (1, doubled 512 times mod p) */
    for (int i = 0; i < 512; i++) {
        mod_add(&R2, &R2, &R2, &P);
    }
    to_mont(&ONE, &one);
    to_mont(&BM, &B);
    fe_sub_raw(&EXP_INV, &P, &two);
    fe_add_raw(&x, &P, &one);           /* (p + 1 fits: p's top limb) */
    shr(&EXP_SQRT, &x, 2);
    fe_sub_raw(&x, &P, &one);
    shr(&EXP_LEG, &x, 1);
    ready = true;
}

static void fe_from_bytes(Fe *r, const uint8_t b[32])
{
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;

        for (int j = 0; j < 8; j++) {
            v = v << 8 | b[(3 - i) * 8 + j];
        }
        r->v[i] = v;
    }
}

static void fe_to_bytes(uint8_t b[32], const Fe *a)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            b[(3 - i) * 8 + j] = a->v[i] >> (56 - 8 * j);
        }
    }
}

/* y^2 of x on the curve (Montgomery form): x^3 - 3x + b */
static void curve_rhs(Fe *r, const Fe *x)
{
    Fe t, x3;

    fp_sqr(&t, x);
    fp_mul(&x3, &t, x);
    mod_add(&t, x, x, &P);
    mod_add(&t, &t, x, &P);
    fp_sub(&x3, &x3, &t);
    mod_add(r, &x3, &BM, &P);
}

/* ------------------------------------------------------------ points */

static void pt_dbl(Pt *r, const Pt *a)
{
    Fe delta, gamma, beta, alpha, t, u, x3, y3, z3;

    if (fe_zero(&a->z) || fe_zero(&a->y)) {
        memset(r, 0, sizeof(*r));
        return;
    }
    fp_sqr(&delta, &a->z);
    fp_sqr(&gamma, &a->y);
    fp_mul(&beta, &a->x, &gamma);
    fp_sub(&t, &a->x, &delta);          /* alpha = 3 (x - delta)(x + delta) */
    mod_add(&u, &a->x, &delta, &P);
    fp_mul(&alpha, &t, &u);
    mod_add(&t, &alpha, &alpha, &P);
    mod_add(&alpha, &t, &alpha, &P);
    fp_sqr(&x3, &alpha);                /* x3 = alpha^2 - 8 beta */
    mod_add(&t, &beta, &beta, &P);
    mod_add(&t, &t, &t, &P);            /* (4 beta) */
    mod_add(&u, &t, &t, &P);            /* (8 beta) */
    fp_sub(&x3, &x3, &u);
    mod_add(&z3, &a->y, &a->z, &P);     /* z3 = (y + z)^2 - gamma - delta */
    fp_sqr(&z3, &z3);
    fp_sub(&z3, &z3, &gamma);
    fp_sub(&z3, &z3, &delta);
    fp_sub(&u, &t, &x3);                /* y3 = alpha (4 beta - x3) - 8 g^2 */
    fp_mul(&y3, &alpha, &u);
    fp_sqr(&t, &gamma);
    mod_add(&t, &t, &t, &P);
    mod_add(&t, &t, &t, &P);
    mod_add(&t, &t, &t, &P);
    fp_sub(&y3, &y3, &t);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

static void pt_add(Pt *r, const Pt *a, const Pt *b)
{
    Fe z1z1, z2z2, u1, u2, s1, s2, h, rr, hh, hhh, v, t, x3, y3, z3;

    if (fe_zero(&a->z)) {
        *r = *b;
        return;
    }
    if (fe_zero(&b->z)) {
        *r = *a;
        return;
    }
    fp_sqr(&z1z1, &a->z);
    fp_sqr(&z2z2, &b->z);
    fp_mul(&u1, &a->x, &z2z2);
    fp_mul(&u2, &b->x, &z1z1);
    fp_mul(&t, &b->z, &z2z2);
    fp_mul(&s1, &a->y, &t);
    fp_mul(&t, &a->z, &z1z1);
    fp_mul(&s2, &b->y, &t);
    fp_sub(&h, &u2, &u1);
    fp_sub(&rr, &s2, &s1);
    if (fe_zero(&h)) {
        if (fe_zero(&rr)) {
            pt_dbl(r, a);
        } else {
            memset(r, 0, sizeof(*r));   /* (a = -b) */
        }
        return;
    }
    fp_sqr(&hh, &h);
    fp_mul(&hhh, &h, &hh);
    fp_mul(&v, &u1, &hh);
    fp_sqr(&x3, &rr);                   /* x3 = r^2 - h^3 - 2 v */
    fp_sub(&x3, &x3, &hhh);
    fp_sub(&x3, &x3, &v);
    fp_sub(&x3, &x3, &v);
    fp_sub(&t, &v, &x3);                /* y3 = r (v - x3) - s1 h^3 */
    fp_mul(&y3, &rr, &t);
    fp_mul(&t, &s1, &hhh);
    fp_sub(&y3, &y3, &t);
    fp_mul(&t, &a->z, &b->z);           /* z3 = z1 z2 h */
    fp_mul(&z3, &t, &h);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

/* k a (k plain) */
static void pt_mul(Pt *r, const Fe *k, const Pt *a)
{
    Pt x;

    memset(&x, 0, sizeof(x));
    for (int i = 255; i >= 0; i--) {
        pt_dbl(&x, &x);
        if (k->v[i / 64] >> (i % 64) & 1) {
            pt_add(&x, &x, a);
        }
    }
    *r = x;
}

/* a point from x || y (bytes), checked on the curve */
static bool pt_from_bytes(Pt *r, const uint8_t b[64])
{
    Fe x, y, lhs, rhs;

    fe_from_bytes(&x, b);
    fe_from_bytes(&y, b + 32);
    if (fe_cmp(&x, &P) >= 0 || fe_cmp(&y, &P) >= 0) {
        return false;
    }
    to_mont(&r->x, &x);
    to_mont(&r->y, &y);
    r->z = ONE;
    fp_sqr(&lhs, &r->y);
    curve_rhs(&rhs, &r->x);
    return fe_cmp(&lhs, &rhs) == 0;
}

/* x || y of a point (not the point at infinity) */
static void pt_to_bytes(uint8_t b[64], const Pt *a)
{
    Fe zi, zi2, x, y;

    fp_pow(&zi, &a->z, &EXP_INV);
    fp_sqr(&zi2, &zi);
    fp_mul(&x, &a->x, &zi2);
    fp_mul(&zi2, &zi2, &zi);
    fp_mul(&y, &a->y, &zi2);
    from_mont(&x, &x);
    from_mont(&y, &y);
    fe_to_bytes(b, &x);
    fe_to_bytes(b + 32, &y);
}

/* ------------------------------------------------- HMAC, KDF; and SAE */

void sae_hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *a,
                     size_t an, const uint8_t *b, size_t bn, uint8_t out[32])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, kn);
    gsize n = 32;

    g_hmac_update(h, a, an);
    if (b) {
        g_hmac_update(h, b, bn);
    }
    g_hmac_get_digest(h, out, &n);
    g_hmac_unref(h);
}

void sae_kdf_sha256(const uint8_t *key, size_t kn, const char *label,
                    const uint8_t *ctx, size_t cn, uint8_t *out, size_t bits)
{
    size_t n = (bits + 7) / 8, done = 0;

    for (uint16_t i = 1; done < n; i++) {
        uint8_t le[2] = { i, i >> 8 }, len[2] = { bits, bits >> 8 }, d[32];
        GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, kn);
        gsize dn = 32;

        g_hmac_update(h, le, 2);
        g_hmac_update(h, (const uint8_t *)label, strlen(label));
        g_hmac_update(h, ctx, cn);
        g_hmac_update(h, len, 2);
        g_hmac_get_digest(h, d, &dn);
        g_hmac_unref(h);
        memcpy(out + done, d, MIN(32, n - done));
        done += 32;
    }
}

bool sae_pwe(SAE *s, const char *password, const uint8_t a[6],
             const uint8_t b[6])
{
    const uint8_t *hi = memcmp(a, b, 6) > 0 ? a : b, *lo = hi == a ? b : a;
    uint8_t key[12], seed[32], val[32], prime[32];

    init();
    memcpy(key, hi, 6);
    memcpy(key + 6, lo, 6);
    fe_to_bytes(prime, &P);
    /* (the first found; past 40 tries, as many as it takes) */
    for (int counter = 1; counter < 256; counter++) {
        uint8_t c = counter;
        Fe x, xm, y2, leg, y;

        sae_hmac_sha256(key, 12, (const uint8_t *)password, strlen(password),
                        &c, 1, seed);
        sae_kdf_sha256(seed, 32, "SAE Hunting and Pecking", prime, 32,
                       val, 256);
        fe_from_bytes(&x, val);
        if (fe_cmp(&x, &P) >= 0) {
            continue;
        }
        to_mont(&xm, &x);
        curve_rhs(&y2, &xm);
        fp_pow(&leg, &y2, &EXP_LEG);
        if (fe_cmp(&leg, &ONE) != 0) {
            continue;                   /* (no square root: no point) */
        }
        fp_pow(&y, &y2, &EXP_SQRT);
        from_mont(&y, &y);
        if ((y.v[0] & 1) != (seed[31] & 1)) {
            fe_sub_raw(&y, &P, &y);
        }
        fe_to_bytes(s->pwe, &x);
        fe_to_bytes(s->pwe + 32, &y);
        return true;
    }
    return false;
}

/* a scalar in (1, n) */
static bool scalar_ok(const Fe *k)
{
    Fe one = { { 1, 0, 0, 0 } };

    return fe_cmp(k, &one) > 0 && fe_cmp(k, &N) < 0;
}

bool sae_commit(SAE *s, const uint8_t rand[32], const uint8_t mask[32],
                uint8_t out[96])
{
    const Fe zero = { { 0 } };
    Fe r, m, sc;
    Pt pwe, e;

    init();
    fe_from_bytes(&r, rand);
    fe_from_bytes(&m, mask);
    if (!scalar_ok(&r) || !scalar_ok(&m)) {
        return false;
    }
    mod_add(&sc, &r, &m, &N);
    if (!scalar_ok(&sc) || !pt_from_bytes(&pwe, s->pwe)) {
        return false;
    }
    pt_mul(&e, &m, &pwe);               /* its element: -(mask PWE) */
    if (fe_zero(&e.z)) {
        return false;
    }
    fp_sub(&e.y, &zero, &e.y);
    memcpy(s->rand, rand, 32);
    fe_to_bytes(s->scalar, &sc);
    pt_to_bytes(s->element, &e);
    memcpy(out, s->scalar, 32);
    memcpy(out + 32, s->element, 64);
    return true;
}

bool sae_peer_commit(SAE *s, const uint8_t in[96])
{
    static const uint8_t zero[32];
    uint8_t k[64], keyseed[32], value[32], kp[64];
    Fe ps, r, sc, sum;
    Pt pwe, pe, t;

    init();
    fe_from_bytes(&ps, in);
    if (!scalar_ok(&ps) || !pt_from_bytes(&pe, in + 32) ||
        !pt_from_bytes(&pwe, s->pwe)) {
        return false;
    }
    if (!memcmp(in, s->scalar, 32) && !memcmp(in + 32, s->element, 64)) {
        return false;                   /* (its own, reflected) */
    }
    fe_from_bytes(&r, s->rand);
    pt_mul(&t, &ps, &pwe);              /* K = rand (ps PWE + PE) */
    pt_add(&t, &t, &pe);
    pt_mul(&t, &r, &t);
    if (fe_zero(&t.z)) {
        return false;
    }
    pt_to_bytes(k, &t);
    sae_hmac_sha256(zero, 32, k, 32, NULL, 0, keyseed);
    fe_from_bytes(&sc, s->scalar);
    mod_add(&sum, &sc, &ps, &N);
    fe_to_bytes(value, &sum);
    sae_kdf_sha256(keyseed, 32, "SAE KCK and PMK", value, 32, kp, 512);
    memcpy(s->kck, kp, 32);
    memcpy(s->pmk, kp + 32, 32);
    memcpy(s->pmkid, value, 16);
    memcpy(s->peer_scalar, in, 32);
    memcpy(s->peer_element, in + 32, 64);
    return true;
}

void sae_confirm(const SAE *s, uint16_t sc, bool own_first, uint8_t out[32])
{
    uint8_t b[2 + 96 + 96];

    b[0] = sc;
    b[1] = sc >> 8;
    memcpy(b + 2, own_first ? s->scalar : s->peer_scalar, 32);
    memcpy(b + 34, own_first ? s->element : s->peer_element, 64);
    memcpy(b + 98, own_first ? s->peer_scalar : s->scalar, 32);
    memcpy(b + 130, own_first ? s->peer_element : s->element, 64);
    sae_hmac_sha256(s->kck, 32, b, sizeof(b), NULL, 0, out);
}
