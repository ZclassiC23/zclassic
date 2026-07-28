/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for ZID (sovereign identity layer Phase 1) — ed25519 keypair/sign
 * against the RFC 8032 §7.1 vectors, and the zid document codec:
 * encode/decode round-trip, tamper rejection, expiry, blinded keys, and
 * the monotonic-seq supersede rule. */

#include "test/test_core.h"
#include "zid/zid.h"
#include "crypto/ed25519.h"
#include <string.h>

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_max)
{
    size_t n = strlen(hex) / 2;
    if (n > out_max) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return 0;
        out[i] = (uint8_t)v;
    }
    return n;
}

/* RFC 8032 §7.1 TEST 1 / TEST 2: derive the keypair from the seed, sign
 * the message, and require byte-exact equality with the published pk and
 * sig, plus acceptance by ed25519_verify. */
static int test_rfc8032_vector(const char *label, const char *seed_hex,
                               const char *pk_hex, const uint8_t *msg,
                               size_t msg_len, const char *sig_hex)
{
    int failures = 0;
    uint8_t seed[32], want_pk[32], want_sig[64];
    hex_to_bytes(seed_hex, seed, sizeof(seed));
    hex_to_bytes(pk_hex, want_pk, sizeof(want_pk));
    hex_to_bytes(sig_hex, want_sig, sizeof(want_sig));

    uint8_t pk[32], sk[32];
    ed25519_keypair(pk, sk, seed);

    printf("ed25519 %s: keypair pk matches RFC 8032... ", label);
    if (memcmp(pk, want_pk, 32) == 0 && memcmp(sk, seed, 32) == 0)
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    uint8_t sig[64];
    ed25519_sign(sig, msg, msg_len, sk, pk);

    printf("ed25519 %s: sign matches RFC 8032... ", label);
    if (memcmp(sig, want_sig, 64) == 0)
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("ed25519 %s: published vector verifies... ", label);
    if (ed25519_verify(want_sig, msg, msg_len, want_pk))
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("ed25519 %s: own signature verifies... ", label);
    if (ed25519_verify(sig, msg, msg_len, pk))
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    return failures;
}

int test_zid(void)
{
    int failures = 0;

    printf("\n=== ZID Tests ===\n");

    /* ── RFC 8032 §7.1 known-answer vectors ─────────────────────── */

    failures += test_rfc8032_vector(
        "TEST 1 (empty msg)",
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        NULL, 0,
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");

    const uint8_t msg_r = 0x72;
    failures += test_rfc8032_vector(
        "TEST 2 (1-byte 0x72)",
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        &msg_r, 1,
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");

    /* ── Document sign → encode → decode → verify round-trip ─────── */

    uint8_t seed[32];
    hex_to_bytes("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                 seed, sizeof(seed));
    const uint8_t body[] = "hello sovereign identity";
    const uint64_t now = 1700000000;

    struct zid_doc doc;
    printf("zid_doc_sign: fills and signs... ");
    if (zid_doc_sign(&doc, body, sizeof(body) - 1, 1, now + 3600, seed))
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    uint8_t wire[ZID_DOC_MAX];
    size_t wire_len = zid_doc_encode(wire, sizeof(wire), &doc);
    printf("zid_doc_encode: exact wire size... ");
    if (wire_len == 51 + sizeof(body) - 1 + 64)
        printf("OK\n");
    else { printf("FAIL (len=%zu)\n", wire_len); failures++; }

    struct zid_doc back;
    printf("zid sign→encode→decode→verify round-trip... ");
    if (zid_doc_decode(&back, wire, wire_len) &&
        back.body_len == sizeof(body) - 1 &&
        memcmp(back.body, body, sizeof(body) - 1) == 0 &&
        back.seq == 1 && back.expiry == now + 3600 &&
        memcmp(back.master_pubkey, doc.master_pubkey, 32) == 0 &&
        zid_doc_verify(&back, now))
        printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    /* ── Tamper rejection ────────────────────────────────────────── */

    printf("zid_doc_verify: flipped body byte rejected... ");
    {
        struct zid_doc t = doc;
        t.body[0] ^= 0x01;
        if (!zid_doc_verify(&t, now)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_verify: wrong master pubkey rejected... ");
    {
        struct zid_doc t = doc;
        t.master_pubkey[0] ^= 0x01;
        if (!zid_doc_verify(&t, now)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_verify: expired doc rejected (expiry <= now)... ");
    if (!zid_doc_verify(&doc, now + 3600)) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("zid_doc_decode: truncated buffer rejected... ");
    {
        struct zid_doc t;
        if (!zid_doc_decode(&t, wire, wire_len - 1)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_decode: bad version byte rejected... ");
    {
        uint8_t bad[ZID_DOC_MAX];
        memcpy(bad, wire, wire_len);
        bad[0] = 2;
        struct zid_doc t;
        if (!zid_doc_decode(&t, bad, wire_len)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_encode: undersized output buffer rejected... ");
    if (zid_doc_encode(wire, wire_len - 1, &doc) == 0) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("zid_doc_sign: oversize body rejected... ");
    {
        struct zid_doc t;
        uint8_t big[ZID_BODY_MAX + 1];
        memset(big, 0xAB, sizeof(big));
        if (!zid_doc_sign(&t, big, ZID_BODY_MAX + 1, 1, now + 1, seed))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Blinded keys ────────────────────────────────────────────── */

    printf("zid_blinded_key: deterministic... ");
    {
        uint8_t k1[32], k2[32];
        zid_blinded_key(k1, doc.master_pubkey, 100);
        zid_blinded_key(k2, doc.master_pubkey, 100);
        if (memcmp(k1, k2, 32) == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_blinded_key: differs across periods... ");
    {
        uint8_t k1[32], k2[32];
        zid_blinded_key(k1, doc.master_pubkey, 100);
        zid_blinded_key(k2, doc.master_pubkey, 101);
        if (memcmp(k1, k2, 32) != 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_blinded_key: differs across pubkeys... ");
    {
        uint8_t other_pk[32];
        memcpy(other_pk, doc.master_pubkey, 32);
        other_pk[0] ^= 0x01;
        uint8_t k1[32], k2[32];
        zid_blinded_key(k1, doc.master_pubkey, 100);
        zid_blinded_key(k2, other_pk, 100);
        if (memcmp(k1, k2, 32) != 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_blinded_key: self-consistency vector (computed twice)... ");
    {
        uint8_t k1[32], k2[32];
        zid_blinded_key(k1, doc.master_pubkey, 42);
        zid_blinded_key(k2, doc.master_pubkey, 42);
        printf("(vector ");
        for (int i = 0; i < 32; i++) printf("%02x", k1[i]);
        printf(") ");
        if (memcmp(k1, k2, 32) == 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Monotonic-seq supersede rule ────────────────────────────── */

    printf("zid_doc_supersedes: higher seq supersedes... ");
    {
        struct zid_doc newer = doc;
        newer.seq = doc.seq + 1;
        if (zid_doc_supersedes(&newer, &doc)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_supersedes: equal seq does not... ");
    if (!zid_doc_supersedes(&doc, &doc)) printf("OK\n");
    else { printf("FAIL\n"); failures++; }

    printf("zid_doc_supersedes: lower seq does not... ");
    {
        struct zid_doc older = doc;
        older.seq = doc.seq + 1;
        if (!zid_doc_supersedes(&doc, &older)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_doc_supersedes: different pubkey does not... ");
    {
        struct zid_doc alien = doc;
        alien.seq = doc.seq + 100;
        alien.master_pubkey[0] ^= 0x01;
        if (!zid_doc_supersedes(&alien, &doc)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("=== ZID: %d failure(s) ===\n", failures);
    return failures;
}
