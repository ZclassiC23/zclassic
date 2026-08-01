/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Hermetic coverage for the bounded ZPAY v1 Sapling memo envelope. */

#include "test/test_core.h"

#include "zid/zpay.h"

#include <string.h>

static void zp_fill(uint8_t *out, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)(seed + i);
}

static void zp_fixture(struct zpay_envelope *e)
{
    memset(e, 0, sizeof(*e));
    e->network = ZPAY_NETWORK_MAINNET;
    e->message_type = ZPAY_MESSAGE_INVOICE;
    e->created_at = 1700000000;
    e->expires_at = 1700000600;
    zp_fill(e->nonce, sizeof(e->nonce), 1);
    zp_fill(e->request_id, sizeof(e->request_id), 31);
    zp_fill(e->invoice_digest, sizeof(e->invoice_digest), 61);
    memcpy(e->asset, "ZCL", 4);
    zp_fill(e->amount_commitment, sizeof(e->amount_commitment), 101);
}

int test_zpay(void)
{
    int failures = 0;

    TEST("zpay anonymous envelope round-trips and stays explicitly anonymous") {
        struct zpay_envelope in;
        zp_fixture(&in);
        uint8_t memo[ZPAY_MEMO_LEN];
        struct zpay_envelope out;
        ASSERT(zpay_memo_encode(memo, &in, NULL));
        ASSERT(zpay_memo_decode(memo, &out));
        ASSERT_EQ(out.network, ZPAY_NETWORK_MAINNET);
        ASSERT_EQ(out.message_type, ZPAY_MESSAGE_INVOICE);
        ASSERT(strcmp(out.asset, "ZCL") == 0);
        ASSERT(memcmp(out.request_id, in.request_id, 16) == 0);
        ASSERT_EQ(zpay_memo_authenticate(memo, 1700000100, &out),
                  ZPAY_SENDER_ANONYMOUS);
        ASSERT(!out.has_identity_doc);
        PASS();
    }

    TEST("zpay signed envelope authenticates the exact canonical bytes") {
        struct zpay_envelope in;
        zp_fixture(&in);
        in.message_type = ZPAY_MESSAGE_PAYMENT;
        in.has_reply = true;
        zp_fill(in.reply_ref, sizeof(in.reply_ref), 211);
        uint8_t seed[32];
        zp_fill(seed, sizeof(seed), 7);
        uint8_t memo[ZPAY_MEMO_LEN];
        struct zpay_envelope out;
        ASSERT(zpay_memo_encode(memo, &in, seed));
        ASSERT_EQ(zpay_memo_authenticate(memo, 1700000100, &out),
                  ZPAY_SENDER_ZID_VERIFIED);
        ASSERT(out.has_identity_doc);
        ASSERT(out.has_reply);
        ASSERT(memcmp(out.reply_ref, in.reply_ref, 32) == 0);

        memo[58] ^= 1; /* invoice digest: parseable, but no longer signed */
        ASSERT_EQ(zpay_memo_authenticate(memo, 1700000100, &out),
                  ZPAY_SENDER_ZID_INVALID);
        PASS();
    }

    TEST("zpay rejects wrong-network, expired, and not-yet-current requests") {
        struct zpay_envelope in;
        zp_fixture(&in);
        uint8_t memo[ZPAY_MEMO_LEN];
        struct zpay_envelope out;
        ASSERT(zpay_memo_encode(memo, &in, NULL));
        ASSERT(zpay_memo_decode(memo, &out));
        ASSERT(zpay_envelope_is_current(&out, ZPAY_NETWORK_MAINNET,
                                        1700000100));
        ASSERT(!zpay_envelope_is_current(&out, ZPAY_NETWORK_TESTNET,
                                         1700000100));
        ASSERT(!zpay_envelope_is_current(&out, ZPAY_NETWORK_MAINNET,
                                         1699999999));
        ASSERT(!zpay_envelope_is_current(&out, ZPAY_NETWORK_MAINNET,
                                         1700000600));
        PASS();
    }

    TEST("zpay expired identity document never authenticates") {
        struct zpay_envelope in;
        zp_fixture(&in);
        uint8_t seed[32] = {9};
        uint8_t memo[ZPAY_MEMO_LEN];
        ASSERT(zpay_memo_encode(memo, &in, seed));
        ASSERT_EQ(zpay_memo_authenticate(memo, in.expires_at, NULL),
                  ZPAY_SENDER_ZID_INVALID);
        PASS();
    }

    TEST("zpay strict decoder rejects reserved flags and noncanonical padding") {
        struct zpay_envelope in;
        zp_fixture(&in);
        uint8_t memo[ZPAY_MEMO_LEN];
        struct zpay_envelope out;
        ASSERT(zpay_memo_encode(memo, &in, NULL));
        uint8_t bad[ZPAY_MEMO_LEN];
        memcpy(bad, memo, sizeof(bad));
        bad[7] |= 0x80;
        ASSERT(!zpay_memo_decode(bad, &out));
        memcpy(bad, memo, sizeof(bad));
        bad[ZPAY_MEMO_LEN - 1] = 0;
        ASSERT(!zpay_memo_decode(bad, &out));
        PASS();
    }

    TEST("zpay refuses ambiguous asset spelling and leaves output untouched") {
        struct zpay_envelope in;
        zp_fixture(&in);
        memcpy(in.asset, "zcl", 4);
        uint8_t memo[ZPAY_MEMO_LEN];
        memset(memo, 0xA5, sizeof(memo));
        ASSERT(!zpay_memo_encode(memo, &in, NULL));
        bool untouched = true;
        for (size_t i = 0; i < sizeof(memo); i++)
            if (memo[i] != 0xA5) { untouched = false; break; }
        ASSERT(untouched);
        PASS();
    }

_test_next:;
    if (failures == 0)
        printf("test_zpay: all passed\n");
    else
        printf("test_zpay: %d FAILED\n", failures);
    return failures;
}
