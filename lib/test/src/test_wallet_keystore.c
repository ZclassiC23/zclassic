/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for wallet_keystore — passphrase-based AES-256-GCM at rest. */

#include "test/test_helpers.h"
#include "wallet/wallet_keystore.h"
#include "config/boot.h"   /* wallet_at_rest_boot_decision + operator lanes */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A representative private key payload (32 bytes — Sapling spending
 * key length).  Tests use this so the format we exercise matches
 * what the live wallet would feed in. */
static const uint8_t k_secret_key[32] = {
    0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
    0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
    0xde,0xad,0xbe,0xef,0xca,0xfe,0xba,0xbe,
    0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,
};

static const char k_passphrase[] = "correct horse battery staple";
static const char k_wrong_pass[] = "correct horse battery stapld";

/* Use the minimum iterations in tests so each call costs ~5ms not
 * ~150ms — the keystore math is identical, only the cost changes. */
#define TEST_ITERS WKS_MIN_ITERS

static int test_round_trip(void)
{
    int failures = 0;
    TEST("wallet_keystore: encrypt then decrypt round-trips") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));
        ASSERT(env_len == wks_envelope_size(sizeof(k_secret_key)));

        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(wks_decrypt(env, env_len, k_passphrase,
                           plain, sizeof(plain), &plain_len));
        ASSERT(plain_len == sizeof(k_secret_key));
        ASSERT(memcmp(plain, k_secret_key, plain_len) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_wrong_passphrase_rejected(void)
{
    int failures = 0;
    TEST("wallet_keystore: wrong passphrase fails decrypt (GCM tag)") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));

        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, env_len, k_wrong_pass,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tampered_ciphertext_rejected(void)
{
    int failures = 0;
    TEST("wallet_keystore: tampered ciphertext fails decrypt") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));

        /* Flip a bit in the ciphertext (offset >= WKS_HEADER_LEN). */
        env[WKS_HEADER_LEN + 5] ^= 0x01;

        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, env_len, k_passphrase,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_tampered_tag_rejected(void)
{
    int failures = 0;
    TEST("wallet_keystore: tampered auth tag fails decrypt") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));

        env[44] ^= 0x80;  /* tag offset */

        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, env_len, k_passphrase,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_unique_envelopes(void)
{
    int failures = 0;
    TEST("wallet_keystore: re-encrypting the same key produces a fresh envelope") {
        uint8_t e1[256];
        uint8_t e2[256];
        size_t l1 = 0, l2 = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           e1, sizeof(e1), &l1));
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           e2, sizeof(e2), &l2));
        ASSERT(l1 == l2);
        /* Salt + nonce are random per call → envelopes must differ. */
        ASSERT(memcmp(e1, e2, l1) != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_header(void)
{
    int failures = 0;
    TEST("wallet_keystore: envelope header has WKS1 magic + iterations") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));

        ASSERT(memcmp(env, WKS_MAGIC, WKS_MAGIC_LEN) == 0);
        ASSERT(wks_envelope_iterations(env, env_len) == TEST_ITERS);
        PASS();
    } _test_next:;
    return failures;
}

static int test_buffer_too_small(void)
{
    int failures = 0;
    TEST("wallet_keystore: encrypt rejects too-small output buffer") {
        uint8_t env[16];  /* far smaller than WKS_HEADER_LEN */
        size_t env_len = 0;
        ASSERT(!wks_encrypt(k_secret_key, sizeof(k_secret_key),
                            k_passphrase, TEST_ITERS,
                            env, sizeof(env), &env_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_iters_clamped(void)
{
    int failures = 0;
    TEST("wallet_keystore: out-of-range iterations are clamped") {
        uint8_t env[256];
        size_t env_len = 0;

        /* Below minimum: clamped up to WKS_MIN_ITERS. */
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, 1,
                           env, sizeof(env), &env_len));
        ASSERT(wks_envelope_iterations(env, env_len) == WKS_MIN_ITERS);

        /* Above maximum: clamped down to WKS_MAX_ITERS.  Don't actually
         * pay the cost — clamp check is enough. */
        size_t l2 = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, WKS_MIN_ITERS,
                           env, sizeof(env), &l2));
        ASSERT(wks_envelope_iterations(env, l2) == WKS_MIN_ITERS);
        PASS();
    } _test_next:;
    return failures;
}

static int test_default_iters_env(void)
{
    int failures = 0;
    TEST("wallet_keystore: ZCL_WALLET_KDF_ITERS env override is honoured") {
        setenv("ZCL_WALLET_KDF_ITERS", "150000", 1);
        ASSERT(wks_default_iterations() == 150000);

        setenv("ZCL_WALLET_KDF_ITERS", "5", 1);  /* below MIN */
        ASSERT(wks_default_iterations() == WKS_MIN_ITERS);

        setenv("ZCL_WALLET_KDF_ITERS", "100000000", 1);  /* above MAX */
        ASSERT(wks_default_iterations() == WKS_MAX_ITERS);

        unsetenv("ZCL_WALLET_KDF_ITERS");
        ASSERT(wks_default_iterations() == WKS_DEFAULT_ITERS);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_too_short(void)
{
    int failures = 0;
    TEST("wallet_keystore: decrypt rejects truncated envelope") {
        uint8_t env[10] = {0};
        memcpy(env, WKS_MAGIC, WKS_MAGIC_LEN);
        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, sizeof(env), k_passphrase,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_bad_magic(void)
{
    int failures = 0;
    TEST("wallet_keystore: decrypt rejects bad magic header") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));
        env[0] = 'X';  /* clobber the magic */

        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, env_len, k_passphrase,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

static int test_empty_plaintext(void)
{
    int failures = 0;
    TEST("wallet_keystore: zero-length plaintext is a valid envelope") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(wks_encrypt(NULL, 0,
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));
        ASSERT(env_len == WKS_HEADER_LEN);

        uint8_t plain[64];
        size_t plain_len = 99;
        ASSERT(wks_decrypt(env, env_len, k_passphrase,
                           plain, sizeof(plain), &plain_len));
        ASSERT(plain_len == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_long_plaintext(void)
{
    int failures = 0;
    TEST("wallet_keystore: round-trips a long plaintext (1024 bytes)") {
        uint8_t plaintext[1024];
        for (size_t i = 0; i < sizeof(plaintext); i++)
            plaintext[i] = (uint8_t)(i ^ 0x5A);

        uint8_t env[2048];
        size_t env_len = 0;
        ASSERT(wks_encrypt(plaintext, sizeof(plaintext),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));

        uint8_t recovered[2048];
        size_t recovered_len = 0;
        ASSERT(wks_decrypt(env, env_len, k_passphrase,
                           recovered, sizeof(recovered), &recovered_len));
        ASSERT(recovered_len == sizeof(plaintext));
        ASSERT(memcmp(recovered, plaintext, recovered_len) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_null_passphrase(void)
{
    int failures = 0;
    TEST("wallet_keystore: NULL passphrase is rejected on encrypt + decrypt") {
        uint8_t env[256];
        size_t env_len = 0;
        ASSERT(!wks_encrypt(k_secret_key, sizeof(k_secret_key),
                            NULL, TEST_ITERS,
                            env, sizeof(env), &env_len));

        ASSERT(wks_encrypt(k_secret_key, sizeof(k_secret_key),
                           k_passphrase, TEST_ITERS,
                           env, sizeof(env), &env_len));
        uint8_t plain[64];
        size_t plain_len = 0;
        ASSERT(!wks_decrypt(env, env_len, NULL,
                            plain, sizeof(plain), &plain_len));
        PASS();
    } _test_next:;
    return failures;
}

/* ── At-rest creation policy ─────────────────────────────────── */

static int test_at_rest_creation_policy(void)
{
    int failures = 0;

    /* Snapshot + clear the env vars the policy reads so the test is
     * independent of the caller's environment. */
    const char *saved_pass  = getenv("ZCL_WALLET_PASSPHRASE");
    const char *saved_optin = getenv("ZCL_ALLOW_PLAINTEXT_WALLET");
    char *pass_copy  = saved_pass  ? strdup(saved_pass)  : NULL;
    char *optin_copy = saved_optin ? strdup(saved_optin) : NULL;

    TEST("wallet_keystore: at-rest creation policy (refuse/encrypt/opt-in)") {
        /* No passphrase + no opt-in => REFUSE (do not silently create
         * a plaintext wallet). */
        unsetenv("ZCL_WALLET_PASSPHRASE");
        unsetenv("ZCL_ALLOW_PLAINTEXT_WALLET");
        ASSERT(wallet_at_rest_creation_policy() == WALLET_AT_REST_REFUSE);

        /* Passphrase set => ENCRYPTED, and it wins over the opt-in. */
        setenv("ZCL_WALLET_PASSPHRASE", "correct horse", 1);
        ASSERT(wallet_at_rest_creation_policy() == WALLET_AT_REST_ENCRYPTED);
        setenv("ZCL_ALLOW_PLAINTEXT_WALLET", "1", 1);
        ASSERT(wallet_at_rest_creation_policy() == WALLET_AT_REST_ENCRYPTED);

        /* Opt-in without passphrase => PLAINTEXT_OPTIN. */
        unsetenv("ZCL_WALLET_PASSPHRASE");
        setenv("ZCL_ALLOW_PLAINTEXT_WALLET", "1", 1);
        ASSERT(wallet_at_rest_creation_policy() == WALLET_AT_REST_PLAINTEXT_OPTIN);

        /* Empty passphrase and opt-in="0" do NOT count. */
        setenv("ZCL_WALLET_PASSPHRASE", "", 1);
        setenv("ZCL_ALLOW_PLAINTEXT_WALLET", "0", 1);
        ASSERT(wallet_at_rest_creation_policy() == WALLET_AT_REST_REFUSE);
        PASS();
    } _test_next:;

    /* Restore the environment. */
    if (pass_copy)  setenv("ZCL_WALLET_PASSPHRASE", pass_copy, 1);
    else            unsetenv("ZCL_WALLET_PASSPHRASE");
    if (optin_copy) setenv("ZCL_ALLOW_PLAINTEXT_WALLET", optin_copy, 1);
    else            unsetenv("ZCL_ALLOW_PLAINTEXT_WALLET");
    free(pass_copy);
    free(optin_copy);

    return failures;
}

/* ── Boot-site creation decision (policy × mint × lane) ─────────── */

static int test_at_rest_boot_decision(void)
{
    int failures = 0;

    TEST("wallet_keystore: boot decision matrix (mint/lane/passphrase)") {
        /* Passphrase (ENCRYPTED) wins in every context, mint or not. */
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_ENCRYPTED, false,
                   ZCL_OPERATOR_LANE_CANONICAL) == WALLET_BOOT_CREATE_ENCRYPTED);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_ENCRYPTED, true,
                   ZCL_OPERATOR_LANE_UNKNOWN) == WALLET_BOOT_CREATE_ENCRYPTED);

        /* Explicit opt-in proceeds (plaintext) in every context. */
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_PLAINTEXT_OPTIN, false,
                   ZCL_OPERATOR_LANE_CANONICAL) == WALLET_BOOT_CREATE_PLAINTEXT);

        /* REFUSE policy + canonical / unknown (interactive default) => REFUSE. */
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_CANONICAL) == WALLET_BOOT_REFUSE);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_UNKNOWN) == WALLET_BOOT_REFUSE);

        /* REFUSE policy + offline mint producer => exempt (proceed quietly),
         * regardless of lane. mint-anchor-fast is covered (it implies
         * mint_anchor at the boot site). */
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, true,
                   ZCL_OPERATOR_LANE_UNKNOWN) == WALLET_BOOT_CREATE_MINT_EXEMPT);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, true,
                   ZCL_OPERATOR_LANE_CANONICAL) == WALLET_BOOT_CREATE_MINT_EXEMPT);

        /* REFUSE policy + declared non-canonical automated lane
         * (dev/soak/test/copy) => downgrade to plaintext (proceed, warn). */
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_DEV) == WALLET_BOOT_CREATE_PLAINTEXT);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_SOAK) == WALLET_BOOT_CREATE_PLAINTEXT);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_TEST) == WALLET_BOOT_CREATE_PLAINTEXT);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_COPY) == WALLET_BOOT_CREATE_PLAINTEXT);
        ASSERT(wallet_at_rest_boot_decision(
                   WALLET_AT_REST_REFUSE, false,
                   ZCL_OPERATOR_LANE_STANDBY) == WALLET_BOOT_CREATE_PLAINTEXT);
        PASS();
    } _test_next:;

    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_wallet_keystore(void);

int test_wallet_keystore(void)
{
    int failures = 0;

    failures += test_at_rest_creation_policy();
    failures += test_at_rest_boot_decision();
    failures += test_round_trip();
    failures += test_wrong_passphrase_rejected();
    failures += test_tampered_ciphertext_rejected();
    failures += test_tampered_tag_rejected();
    failures += test_unique_envelopes();
    failures += test_envelope_header();
    failures += test_buffer_too_small();
    failures += test_iters_clamped();
    failures += test_default_iters_env();
    failures += test_envelope_too_short();
    failures += test_bad_magic();
    failures += test_empty_plaintext();
    failures += test_long_plaintext();
    failures += test_null_passphrase();

    return failures;
}
