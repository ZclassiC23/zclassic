/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for ZID (sovereign identity layer Phase 1) — ed25519 keypair/sign
 * against the RFC 8032 §7.1 vectors, and the zid document codec:
 * encode/decode round-trip, tamper rejection, expiry, blinded keys, and
 * the monotonic-seq supersede rule. */

#include "test/test_core.h"
#include "zid/zid.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
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

    printf("zid_blinded_key: self-consistency vector (ZIDB tag, period 42)... ");
    {
        uint8_t k1[32], k2[32], want[32];
        zid_blinded_key(k1, doc.master_pubkey, 42);
        zid_blinded_key(k2, doc.master_pubkey, 42);
        hex_to_bytes("38d696445a7024f2e004b7a4fa425aa111b9131e79bd126f4274de6a2fd58adf",
                     want, sizeof(want));
        if (memcmp(k1, k2, 32) == 0 && memcmp(k1, want, 32) == 0)
            printf("OK\n");
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

    /* ── Anchor-domain tree (MMR) ────────────────────────────────── */

    printf("\n=== ZID anchor-domain tree ===\n");

    /* Deterministic record digests: SHA3-256("zid-test-leaf" ‖ seed ‖ i). */
    #define ZID_TEST_MAX_LEAVES 32
    uint8_t leaves[ZID_TEST_MAX_LEAVES][32];
    uint8_t leaves_b[ZID_TEST_MAX_LEAVES][32];
    for (uint64_t i = 0; i < ZID_TEST_MAX_LEAVES; i++) {
        uint8_t buf[13 + 1 + 8];
        memcpy(buf, "zid-test-leaf", 13);
        buf[13] = 0xA0;
        for (int b = 0; b < 8; b++) buf[14 + b] = (uint8_t)(i >> (8 * b));
        sha3_256(buf, sizeof(buf), leaves[i]);
        buf[13] = 0xB0;
        sha3_256(buf, sizeof(buf), leaves_b[i]);
    }

    /* Empty tree: zero root, prove/verify refuse. */
    printf("zid_tree: empty tree root is zero, prove refuses... ");
    {
        struct zid_tree t;
        zid_tree_init(&t);
        uint8_t root[32], proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        uint32_t plen = 0;
        zid_tree_root(&t, root);
        uint8_t zero[32] = {0};
        bool empty_ok = memcmp(root, zero, 32) == 0 &&
            !zid_tree_prove_from_leaves(leaves, 0, 0, proof, &plen, pr) &&
            !zid_tree_verify(root, leaves[0], 0, 0, proof, 0);
        if (empty_ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* For each size: prove EVERY leaf from the leaf list, verify true,
     * and cross-check the rebuilt root against the incremental
     * zid_tree_append + zid_tree_root root. Peaks == popcount(n). */
    const uint64_t sizes[] = {1, 2, 3, 5, 8, 17};
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        uint64_t n = sizes[si];
        printf("zid_tree: %llu leaves — prove+verify every leaf, roots agree... ",
               (unsigned long long)n);
        struct zid_tree t;
        zid_tree_init(&t);
        for (uint64_t i = 0; i < n; i++)
            zid_tree_append(&t, leaves[i]);
        uint8_t inc_root[32];
        zid_tree_root(&t, inc_root);

        bool ok = (t.num_peaks == (uint32_t)__builtin_popcountll(n));
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        for (uint64_t i = 0; ok && i < n; i++) {
            uint32_t plen = 0;
            if (!zid_tree_prove_from_leaves(leaves, n, i, proof, &plen, pr)) {
                printf("\n  prove leaf %llu failed\n", (unsigned long long)i);
                ok = false;
                break;
            }
            if (memcmp(pr, inc_root, 32) != 0) {
                printf("\n  rebuilt root != incremental root at leaf %llu\n",
                       (unsigned long long)i);
                ok = false;
                break;
            }
            if (!zid_tree_verify(inc_root, leaves[i], i, n, proof, plen)) {
                printf("\n  verify leaf %llu failed\n", (unsigned long long)i);
                ok = false;
                break;
            }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Tamper battery on a 17-leaf tree, leaf 7. */
    printf("zid_tree_verify: wrong root rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        root[0] ^= 0x01;
        if (!zid_tree_verify(root, leaves[7], 7, 17, proof, plen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree_verify: wrong index rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        if (!zid_tree_verify(root, leaves[8], 8, 17, proof, plen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree_verify: wrong num_leaves rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        if (!zid_tree_verify(root, leaves[7], 7, 16, proof, plen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree_verify: flipped sibling byte rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        proof[0][0] ^= 0x01;
        if (!zid_tree_verify(root, leaves[7], 7, 17, proof, plen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree_verify: digest from a different tree rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        if (!zid_tree_verify(root, leaves_b[7], 7, 17, proof, plen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree_verify: truncated proof rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);
        if (plen > 0 &&
            !zid_tree_verify(root, leaves[7], 7, 17, proof, plen - 1))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* Cross-tag isolation: fold the SAME record digest as a chain-mmr
     * leaf (SHA3(0x00 ‖ digest), no "ZIDL") along the zid proof; the
     * result must differ from the zid tree root. */
    printf("zid_tree: chain-mmr-tagged leaf does not replay against zid root... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], root[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, root);

        /* Manual chain-style fold: leaf = SHA3(0x00 ‖ digest), internals
         * = SHA3(0x01 ‖ l ‖ r), same geometry (17 leaves → peaks 16+1,
         * index 7 sits in the 16-leaf peak at position 7). */
        uint8_t buf[65];
        buf[0] = 0x00;
        memcpy(buf + 1, leaves[7], 32);
        uint8_t h[32];
        sha3_256(buf, 33, h);
        uint64_t pos = 7;
        for (uint32_t i = 0; i < 4; i++) { /* 16-leaf peak: path length 4 */
            uint8_t parent[32];
            buf[0] = 0x01;
            if ((pos & 1) == 0) {
                memcpy(buf + 1, h, 32);
                memcpy(buf + 33, proof[i], 32);
            } else {
                memcpy(buf + 1, proof[i], 32);
                memcpy(buf + 33, h, 32);
            }
            sha3_256(buf, 65, parent);
            memcpy(h, parent, 32);
            pos >>= 1;
        }
        /* Bag chain-folded peak with the remaining zid proof peak. */
        struct sha3_256_ctx ctx;
        sha3_256_init(&ctx);
        uint8_t tag = 0x02;
        sha3_256_write(&ctx, &tag, 1);
        sha3_256_write(&ctx, h, 32);
        sha3_256_write(&ctx, proof[4], 32);
        uint8_t chain_root[32];
        sha3_256_finalize(&ctx, chain_root);

        if (memcmp(chain_root, root, 32) != 0) printf("OK\n");
        else { printf("FAIL (cross-tag replay succeeded)\n"); failures++; }
    }

    printf("zid_tree: root changes on append... ");
    {
        struct zid_tree t;
        zid_tree_init(&t);
        uint8_t r1[32], r2[32];
        zid_tree_append(&t, leaves[0]);
        zid_tree_root(&t, r1);
        zid_tree_append(&t, leaves[1]);
        zid_tree_root(&t, r2);
        if (memcmp(r1, r2, 32) != 0) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_tree: peak count == popcount(num_leaves)... ");
    {
        bool ok = true;
        for (uint64_t n = 1; n <= 23 && ok; n++) {
            struct zid_tree t;
            zid_tree_init(&t);
            for (uint64_t i = 0; i < n; i++)
                zid_tree_append(&t, leaves[i % ZID_TEST_MAX_LEAVES]);
            if (t.num_peaks != (uint32_t)__builtin_popcountll(n))
                ok = false;
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Canonical proof wire format ─────────────────────────────── */

    printf("\n=== ZID proof wire format ===\n");

    /* Round-trip: prove → encode → decode → fields match → verify. */
    const uint64_t wire_sizes[] = {1, 3, 8, 17};
    for (size_t si = 0; si < sizeof(wire_sizes) / sizeof(wire_sizes[0]); si++) {
        uint64_t n = wire_sizes[si];
        printf("zid_proof: %llu leaves — encode→decode→verify round-trip... ",
               (unsigned long long)n);
        struct zid_tree t;
        zid_tree_init(&t);
        for (uint64_t i = 0; i < n; i++)
            zid_tree_append(&t, leaves[i]);
        uint8_t root[32];
        zid_tree_root(&t, root);

        bool ok = true;
        for (uint64_t i = 0; ok && i < n; i++) {
            uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
            uint32_t plen = 0;
            if (!zid_tree_prove_from_leaves(leaves, n, i, proof, &plen, pr)) {
                ok = false;
                break;
            }
            uint8_t wire[ZID_PROOF_WIRE_MAX];
            size_t wire_len = zid_proof_encode(wire, sizeof(wire),
                                               i, n, proof, plen);
            if (wire_len != 19 + (size_t)plen * 32) { ok = false; break; }

            uint64_t d_index = 0, d_n = 0;
            uint8_t d_proof[ZID_TREE_MAX_PEAKS][32];
            uint32_t d_plen = 0;
            if (!zid_proof_decode(&d_index, &d_n, d_proof, &d_plen,
                                  wire, wire_len)) { ok = false; break; }
            if (d_index != i || d_n != n || d_plen != plen ||
                memcmp(d_proof, proof, (size_t)plen * 32) != 0) {
                ok = false;
                break;
            }
            if (!zid_tree_verify(root, leaves[i], d_index, d_n,
                                 d_proof, d_plen)) { ok = false; break; }
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_proof_decode: truncated buffer rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, pr);
        uint8_t wire[ZID_PROOF_WIRE_MAX];
        size_t wire_len = zid_proof_encode(wire, sizeof(wire), 7, 17, proof, plen);
        uint64_t d_index, d_n;
        uint32_t d_plen;
        uint8_t d_proof[ZID_TREE_MAX_PEAKS][32];
        if (!zid_proof_decode(&d_index, &d_n, d_proof, &d_plen,
                              wire, wire_len - 1)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_proof_decode: wrong version rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, pr);
        uint8_t wire[ZID_PROOF_WIRE_MAX];
        size_t wire_len = zid_proof_encode(wire, sizeof(wire), 7, 17, proof, plen);
        wire[0] = 2;
        uint64_t d_index, d_n;
        uint32_t d_plen;
        uint8_t d_proof[ZID_TREE_MAX_PEAKS][32];
        if (!zid_proof_decode(&d_index, &d_n, d_proof, &d_plen,
                              wire, wire_len)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_proof_decode: proof_len field vs actual length mismatch rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, pr);
        uint8_t wire[ZID_PROOF_WIRE_MAX];
        size_t wire_len = zid_proof_encode(wire, sizeof(wire), 7, 17, proof, plen);
        wire[17] ^= 0x01; /* proof_len LE low byte at offset 17 */
        uint64_t d_index, d_n;
        uint32_t d_plen;
        uint8_t d_proof[ZID_TREE_MAX_PEAKS][32];
        if (!zid_proof_decode(&d_index, &d_n, d_proof, &d_plen,
                              wire, wire_len)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_proof_decode: oversize proof_len field rejected... ");
    {
        uint8_t wire[ZID_PROOF_WIRE_MAX];
        memset(wire, 0, sizeof(wire));
        wire[0] = ZID_PROOF_VERSION;
        wire[17] = (uint8_t)(ZID_TREE_MAX_PEAKS + 1); /* 65 > 64 */
        uint64_t d_index, d_n;
        uint32_t d_plen;
        uint8_t d_proof[ZID_TREE_MAX_PEAKS][32];
        if (!zid_proof_decode(&d_index, &d_n, d_proof, &d_plen,
                              wire, ZID_PROOF_WIRE_MAX)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_proof_encode: undersized output buffer rejected... ");
    {
        uint8_t proof[ZID_TREE_MAX_PEAKS][32], pr[32];
        uint32_t plen = 0;
        zid_tree_prove_from_leaves(leaves, 17, 7, proof, &plen, pr);
        uint8_t wire[ZID_PROOF_WIRE_MAX];
        size_t wire_len = zid_proof_encode(wire, sizeof(wire), 7, 17, proof, plen);
        if (zid_proof_encode(wire, wire_len - 1, 7, 17, proof, plen) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* ── Release record codec ────────────────────────────────────── */

    printf("\n=== ZID release record ===\n");

    struct zid_release rel;
    memset(&rel, 0, sizeof(rel));
    snprintf(rel.name, sizeof(rel.name), "zclassic23");
    snprintf(rel.version, sizeof(rel.version), "1.0.0");
    memset(rel.manifest_root, 0x5C, 32);

    printf("zid_release: body encode→decode round-trip... ");
    {
        uint8_t body[ZID_RELEASE_BODY_MAX];
        size_t blen = zid_release_encode_body(body, sizeof(body), &rel);
        struct zid_release back;
        if (blen == 4 + 1 + 10 + 1 + 5 + 32 &&
            zid_release_decode_body(&back, body, (uint16_t)blen) &&
            strcmp(back.name, "zclassic23") == 0 &&
            strcmp(back.version, "1.0.0") == 0 &&
            memcmp(back.manifest_root, rel.manifest_root, 32) == 0)
            printf("OK\n");
        else { printf("FAIL (blen=%zu)\n", blen); failures++; }
    }

    printf("zid_release: sign→verify round-trip... ");
    {
        struct zid_doc rdoc;
        struct zid_release out;
        if (zid_release_sign(&rdoc, &rel, 7, now + 3600, seed) &&
            zid_release_verify(&rdoc, &out, now) &&
            strcmp(out.name, "zclassic23") == 0 &&
            memcmp(out.manifest_root, rel.manifest_root, 32) == 0)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_decode: bad name_len rejected... ");
    {
        uint8_t body[ZID_RELEASE_BODY_MAX];
        size_t blen = zid_release_encode_body(body, sizeof(body), &rel);
        body[4] = 200; /* name_len beyond ZID_RELEASE_NAME_MAX */
        struct zid_release out;
        if (!zid_release_decode_body(&out, body, (uint16_t)blen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_decode: truncated body rejected... ");
    {
        uint8_t body[ZID_RELEASE_BODY_MAX];
        size_t blen = zid_release_encode_body(body, sizeof(body), &rel);
        struct zid_release out;
        if (!zid_release_decode_body(&out, body, (uint16_t)(blen - 1)))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_decode: wrong tag rejected... ");
    {
        uint8_t body[ZID_RELEASE_BODY_MAX];
        size_t blen = zid_release_encode_body(body, sizeof(body), &rel);
        body[0] = 'X';
        struct zid_release out;
        if (!zid_release_decode_body(&out, body, (uint16_t)blen)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_decode: non-printable name rejected... ");
    {
        struct zid_release bad = rel;
        bad.name[2] = '\x01';
        uint8_t body[ZID_RELEASE_BODY_MAX];
        struct zid_release out;
        if (zid_release_encode_body(body, sizeof(body), &bad) == 0 &&
            !zid_release_decode_body(&out, (const uint8_t *)"ZIDR\x03" "a\x01" "b"
                                     "\x01" "v" "00000000000000000000000000000000", 42))
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_verify: signature over wrong body rejected... ");
    {
        /* A validly-signed doc whose body is NOT this release record. */
        struct zid_doc rdoc;
        struct zid_release out;
        const uint8_t other_body[] = "not a release record at all";
        zid_doc_sign(&rdoc, other_body, sizeof(other_body) - 1, 1,
                     now + 3600, seed);
        if (!zid_release_verify(&rdoc, &out, now)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_verify: tampered signed body rejected... ");
    {
        struct zid_doc rdoc;
        zid_release_sign(&rdoc, &rel, 7, now + 3600, seed);
        rdoc.body[rdoc.body_len - 1] ^= 0x01; /* flip a manifest_root byte */
        struct zid_release out;
        if (!zid_release_verify(&rdoc, &out, now)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_verify: expired doc rejected... ");
    {
        struct zid_doc rdoc;
        zid_release_sign(&rdoc, &rel, 7, now + 100, seed);
        struct zid_release out;
        if (!zid_release_verify(&rdoc, &out, now + 100)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("zid_release_verify: NULL rel_out still verifies... ");
    {
        struct zid_doc rdoc;
        zid_release_sign(&rdoc, &rel, 7, now + 3600, seed);
        if (zid_release_verify(&rdoc, NULL, now)) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("=== ZID: %d failure(s) ===\n", failures);
    return failures;
}
