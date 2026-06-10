/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * Unit tests for domain/consensus/coinbase.{c,h}.
 *
 * These tests pin the pure coinbase-transaction-shaping primitives.
 * They are independent of the mempool/chain orchestration that lives
 * in lib/mining/src/miner.c::create_new_block. The regression seal
 * directly compares the shape produced by the domain function with the
 * byte-identical legacy construction copied into this file as a
 * reference implementation — if either side drifts the test shouts.
 */

#include "test/test_helpers.h"

#include "domain/consensus/coinbase.h"

#include "chain/chainparams.h"
#include "chain/subsidy.h"
#include "consensus/params.h"
#include "consensus/upgrades.h"
#include "core/amount.h"
#include "primitives/transaction.h"
#include "script/script.h"

#include <stdio.h>
#include <string.h>

#define DCC_CHECK(name, expr) do { \
    printf("domain_consensus_coinbase: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* --- legacy reference: byte-identical to what miner.c used to write
 * inline. Kept here as the regression seal. -------------------------- */
static void legacy_script_sig_placeholder(int height, struct script *out)
{
    out->data[0] = 0x03;
    out->data[1] = (uint8_t)(height & 0xff);
    out->data[2] = (uint8_t)((height >> 8) & 0xff);
    out->data[3] = (uint8_t)((height >> 16) & 0xff);
    out->data[4] = (uint8_t)OP_0;
    out->size = 5;
}

static void legacy_script_sig_with_extra_nonce(int height,
                                               uint32_t extra_nonce,
                                               struct script *out)
{
    out->data[0] = 0x03;
    out->data[1] = (uint8_t)(height & 0xff);
    out->data[2] = (uint8_t)((height >> 8) & 0xff);
    out->data[3] = (uint8_t)((height >> 16) & 0xff);

    uint8_t en[4];
    en[0] = (uint8_t)(extra_nonce & 0xff);
    en[1] = (uint8_t)((extra_nonce >> 8) & 0xff);
    en[2] = (uint8_t)((extra_nonce >> 16) & 0xff);
    en[3] = (uint8_t)((extra_nonce >> 24) & 0xff);

    int en_len = 4;
    while (en_len > 1 && en[en_len - 1] == 0)
        en_len--;

    out->data[4] = (uint8_t)en_len;
    memcpy(out->data + 5, en, (size_t)en_len);
    out->size = (uint16_t)(5 + en_len);
}

static bool scripts_equal(const struct script *a, const struct script *b)
{
    return a->size == b->size && memcmp(a->data, b->data, a->size) == 0;
}

int test_domain_consensus_coinbase(void)
{
    int failures = 0;
    const struct chain_params *cp = chain_params_get();
    const struct consensus_params *params = &cp->consensus;

    /* --- error-path / contract tests --- */

    /* placeholder: null out. */
    {
        struct zcl_result r = domain_consensus_coinbase_script_sig_placeholder(
                100, NULL);
        DCC_CHECK("placeholder null out -> ERR_NULL_OUT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT);
    }
    /* placeholder: negative height. */
    {
        struct script s; script_init(&s);
        struct zcl_result r = domain_consensus_coinbase_script_sig_placeholder(
                -1, &s);
        DCC_CHECK("placeholder negative height -> ERR_NEG_HEIGHT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT);
    }
    /* placeholder: 2^24 overflow. */
    {
        struct script s; script_init(&s);
        struct zcl_result r = domain_consensus_coinbase_script_sig_placeholder(
                0x01000000, &s);
        DCC_CHECK("placeholder h=2^24 -> ERR_HEIGHT_RANGE",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE);
    }

    /* extra-nonce: null out. */
    {
        struct zcl_result r =
            domain_consensus_coinbase_script_sig_with_extra_nonce(100, 0, NULL);
        DCC_CHECK("extra_nonce null out -> ERR_NULL_OUT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT);
    }
    /* extra-nonce: negative height. */
    {
        struct script s; script_init(&s);
        struct zcl_result r =
            domain_consensus_coinbase_script_sig_with_extra_nonce(-5, 7, &s);
        DCC_CHECK("extra_nonce negative height -> ERR_NEG_HEIGHT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT);
    }
    /* extra-nonce: height overflow. */
    {
        struct script s; script_init(&s);
        struct zcl_result r =
            domain_consensus_coinbase_script_sig_with_extra_nonce(
                    0x01000000, 7, &s);
        DCC_CHECK("extra_nonce h=2^24 -> ERR_HEIGHT_RANGE",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE);
    }

    /* build: null out_tx. */
    {
        struct script m; script_init(&m);
        struct domain_consensus_coinbase_inputs in = {
            .n_height = 1, .subsidy = 0, .total_fees = 0,
            .miner_script = &m, .params = params,
        };
        struct zcl_result r = domain_consensus_coinbase_build(&in, NULL);
        DCC_CHECK("build null out_tx -> ERR_NULL_OUT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NULL_OUT);
    }
    /* build: null inputs struct. */
    {
        struct transaction tx; transaction_init(&tx);
        struct zcl_result r = domain_consensus_coinbase_build(NULL, &tx);
        DCC_CHECK("build null in -> ERR_NULL_PARAMS",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NULL_PARAMS);
        transaction_free(&tx);
    }
    /* build: null miner_script. */
    {
        struct transaction tx; transaction_init(&tx);
        (void)transaction_alloc(&tx, 1, 1);
        struct domain_consensus_coinbase_inputs in = {
            .n_height = 1, .subsidy = 0, .total_fees = 0,
            .miner_script = NULL, .params = params,
        };
        struct zcl_result r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build null script -> ERR_NULL_SCRIPT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NULL_SCRIPT);
        transaction_free(&tx);
    }
    /* build: negative fees / subsidy / height / out of range. */
    {
        struct script m; script_init(&m);
        struct transaction tx; transaction_init(&tx);
        (void)transaction_alloc(&tx, 1, 1);

        struct domain_consensus_coinbase_inputs in = {
            .n_height = 1, .subsidy = 0, .total_fees = -1,
            .miner_script = &m, .params = params,
        };
        struct zcl_result r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build neg fees -> ERR_NEG_FEES",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NEG_FEES);

        in.total_fees = 0;
        in.subsidy = -1;
        r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build neg subsidy -> ERR_NEG_SUBSIDY",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NEG_SUBSIDY);

        in.subsidy = 0;
        in.n_height = -1;
        r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build neg height -> ERR_NEG_HEIGHT",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NEG_HEIGHT);

        in.n_height = 0x01000000;
        r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build h=2^24 -> ERR_HEIGHT_RANGE",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_HEIGHT_RANGE);
        transaction_free(&tx);
    }
    /* build: out_tx not preallocated. */
    {
        struct script m; script_init(&m);
        struct transaction tx; transaction_init(&tx);   /* num_vin=0 */
        struct domain_consensus_coinbase_inputs in = {
            .n_height = 1, .subsidy = 0, .total_fees = 0,
            .miner_script = &m, .params = params,
        };
        struct zcl_result r = domain_consensus_coinbase_build(&in, &tx);
        DCC_CHECK("build no prealloc -> ERR_NOT_PREALLOC",
                  !r.ok && r.code == DOMAIN_CONSENSUS_COINBASE_ERR_NOT_PREALLOC);
        transaction_free(&tx);
    }

    /* --- script-sig shape regression seal (placeholder vs legacy) --- */
    {
        int heights[] = { 0, 1, 100, 0xff, 0x100, 0xffff, 0x10000,
                          706559, 706560, 707000, 2387000, 0x00FFFFFF };
        int n = (int)(sizeof(heights) / sizeof(heights[0]));
        bool all_match = true;
        for (int i = 0; i < n; i++) {
            struct script dom, leg;
            script_init(&dom); script_init(&leg);
            struct zcl_result r =
                domain_consensus_coinbase_script_sig_placeholder(heights[i], &dom);
            legacy_script_sig_placeholder(heights[i], &leg);
            if (!r.ok || !scripts_equal(&dom, &leg)) {
                printf("\n  PLACEHOLDER MISMATCH h=%d ok=%d dom.size=%u "
                       "leg.size=%u\n", heights[i], (int)r.ok,
                       (unsigned)dom.size, (unsigned)leg.size);
                all_match = false;
            }
        }
        DCC_CHECK("placeholder script_sig matches legacy across heights",
                  all_match);
    }

    /* --- script-sig shape regression seal (extra-nonce vs legacy) --- */
    {
        int heights[] = { 0, 1, 706559, 706560, 707000, 2387001, 0x00FFFFFF };
        uint32_t nonces[] = { 0, 1, 0xff, 0x100, 0xffff, 0x10000, 0xffffffffU };
        int nh = (int)(sizeof(heights) / sizeof(heights[0]));
        int nn = (int)(sizeof(nonces)  / sizeof(nonces[0]));
        bool all_match = true;
        for (int i = 0; i < nh; i++) for (int j = 0; j < nn; j++) {
            struct script dom, leg;
            script_init(&dom); script_init(&leg);
            struct zcl_result r =
                domain_consensus_coinbase_script_sig_with_extra_nonce(
                    heights[i], nonces[j], &dom);
            legacy_script_sig_with_extra_nonce(heights[i], nonces[j], &leg);
            if (!r.ok || !scripts_equal(&dom, &leg)) {
                printf("\n  EXTRA_NONCE MISMATCH h=%d en=%u dom.size=%u "
                       "leg.size=%u\n", heights[i], nonces[j],
                       (unsigned)dom.size, (unsigned)leg.size);
                all_match = false;
            }
        }
        DCC_CHECK("extra_nonce script_sig matches legacy across heights x nonces",
                  all_match);
    }

    /* zero-nonce keeps exactly one zero byte (legacy semantics). */
    {
        struct script s; script_init(&s);
        struct zcl_result r =
            domain_consensus_coinbase_script_sig_with_extra_nonce(1, 0, &s);
        DCC_CHECK("extra_nonce=0 -> single zero byte (size=6, last bytes 01 00)",
                  r.ok && s.size == 6 && s.data[4] == 1 && s.data[5] == 0);
    }

    /* --- coinbase_build full-tx regression seal across epochs. We
     * exercise heights spanning the pre-Overwinter / Overwinter /
     * Sapling / pre- and post-Buttercup subsidy boundary so the version
     * selection logic and the value computation are both pinned. ----- */
    {
        struct script miner; script_init(&miner);
        unsigned char p2pkh[20] = {0};
        miner.data[0] = OP_DUP;
        miner.data[1] = OP_HASH160;
        miner.data[2] = 0x14;
        memcpy(miner.data + 3, p2pkh, 20);
        miner.data[23] = OP_EQUALVERIFY;
        miner.data[24] = OP_CHECKSIG;
        miner.size = 25;

        int heights[] = {
            1,         /* slow-start, pre-Overwinter (likely v1) */
            20000,     /* past slow-start, pre-Overwinter */
            500000,    /* mainnet: Overwinter/Sapling active */
            706559,    /* one before Buttercup activation */
            706560,    /* at Buttercup activation */
            707001,    /* post-Buttercup, reduced subsidy */
            1500000,   /* deep post-Buttercup */
        };
        int64_t fee_cases[] = { 0, 1, 1000000 };
        int nh = (int)(sizeof(heights) / sizeof(heights[0]));
        int nf = (int)(sizeof(fee_cases) / sizeof(fee_cases[0]));
        bool all_match = true;
        for (int i = 0; i < nh; i++) for (int j = 0; j < nf; j++) {
            int h = heights[i];
            int64_t fees = fee_cases[j];
            int64_t subsidy = get_block_subsidy(h, params);

            struct transaction tx; transaction_init(&tx);
            if (!transaction_alloc(&tx, 1, 1)) { all_match = false; continue; }
            struct domain_consensus_coinbase_inputs in = {
                .n_height = h, .subsidy = subsidy, .total_fees = fees,
                .miner_script = &miner, .params = params,
            };
            struct zcl_result r = domain_consensus_coinbase_build(&in, &tx);
            if (!r.ok) { all_match = false; transaction_free(&tx); continue; }

            /* Structural invariants the legacy code held. */
            bool ok = transaction_is_coinbase(&tx)
                   && tx.num_vin == 1
                   && tx.num_vout == 1
                   && tx.vout[0].value == subsidy + fees
                   && tx.vout[0].script_pub_key.size == miner.size
                   && memcmp(tx.vout[0].script_pub_key.data,
                             miner.data, miner.size) == 0
                   && tx.expiry_height == 0
                   /* placeholder shape: [03, h0, h1, h2, OP_0] */
                   && tx.vin[0].script_sig.size == 5
                   && tx.vin[0].script_sig.data[0] == 0x03
                   && tx.vin[0].script_sig.data[4] == (uint8_t)OP_0;

            /* Version per epoch matches legacy `create_new_block`. */
            if (consensus_network_upgrade_active(params, h, UPGRADE_SAPLING)) {
                ok = ok && tx.version == SAPLING_TX_VERSION
                        && tx.version_group_id == SAPLING_VERSION_GROUP_ID;
            } else if (consensus_network_upgrade_active(params, h,
                                                        UPGRADE_OVERWINTER)) {
                ok = ok && tx.version == OVERWINTER_TX_VERSION
                        && tx.version_group_id == OVERWINTER_VERSION_GROUP_ID;
            }
            /* pre-Overwinter: legacy left version untouched (v1) — same
             * here, transaction_init() sets version=1. */
            else {
                ok = ok && tx.version == 1 && tx.version_group_id == 0;
            }

            if (!ok) {
                printf("\n  BUILD MISMATCH h=%d fees=%lld subsidy=%lld "
                       "version=%d gid=0x%x scriptSig.size=%u\n",
                       h, (long long)fees, (long long)subsidy,
                       tx.version, tx.version_group_id,
                       (unsigned)tx.vin[0].script_sig.size);
                all_match = false;
            }

            /* Determinism: rebuild and compare hashes. */
            struct transaction tx2; transaction_init(&tx2);
            if (transaction_alloc(&tx2, 1, 1)) {
                struct zcl_result r2 = domain_consensus_coinbase_build(&in, &tx2);
                if (!r2.ok ||
                    memcmp(&tx.hash, &tx2.hash, sizeof(tx.hash)) != 0) {
                    printf("\n  BUILD NON-DETERMINISTIC h=%d\n", h);
                    all_match = false;
                }
                transaction_free(&tx2);
            }
            transaction_free(&tx);
        }
        DCC_CHECK("coinbase_build invariants + version selection across "
                  "pre/post-Sapling and pre/post-Buttercup", all_match);
    }

    return failures;
}
