/*
 * WPA3-Personal's SAE (IEEE 802.11-2020 12.4) for the MT7921's access
 * point: group 19 (NIST P-256), the PWE by hunting and pecking; and the
 * 802.11 KDF and HMAC-SHA256 that it and its 4-way handshake use. Its
 * numbers are as on the air: 32 bytes, big-endian; an element x || y.
 *
 * Copyright (c) 2026, Kara Zajac
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef FLIPPER_ONE_SAE_H
#define FLIPPER_ONE_SAE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAE_GROUP       19

typedef struct SAE {
    uint8_t pwe[64];
    uint8_t rand[32];
    uint8_t scalar[32];
    uint8_t element[64];
    uint8_t peer_scalar[32];
    uint8_t peer_element[64];
    uint8_t kck[32];
    uint8_t pmk[32];
    uint8_t pmkid[16];
} SAE;

/* HMAC-SHA256 of @a then @b (@b may be NULL) */
void sae_hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *a,
                     size_t an, const uint8_t *b, size_t bn, uint8_t out[32]);
/* the 802.11 KDF (KDF-SHA256-@bits): @label, @ctx */
void sae_kdf_sha256(const uint8_t *key, size_t kn, const char *label,
                    const uint8_t *ctx, size_t cn, uint8_t *out, size_t bits);
/* the PWE of @password between MAC addresses @a and @b */
bool sae_pwe(SAE *s, const char *password, const uint8_t a[6],
             const uint8_t b[6]);
/* its commit from @rand and @mask (random): false if they will not do
 * (out of range: draw others); @out its scalar then its element */
bool sae_commit(SAE *s, const uint8_t rand[32], const uint8_t mask[32],
                uint8_t out[96]);
/* the peer's commit: checked, then the KCK, the PMK and the PMKID */
bool sae_peer_commit(SAE *s, const uint8_t in[96]);
/* a confirm, send-confirm @sc: of its own values first, or the peer's */
void sae_confirm(const SAE *s, uint16_t sc, bool own_first, uint8_t out[32]);

#endif
