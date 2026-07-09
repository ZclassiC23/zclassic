/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZMSG on-chain channel — in-sim end-to-end proof.
 * =====================================================================
 *
 * WHAT THIS PROVES
 * ----------------
 *   Params-free (runs unconditionally):
 *     1. zmsg_memo_encode → zmsg_memo_decode round-trips a UTF-8 message
 *        (with and without a reply-to), and the strict decoder rejects a
 *        non-ZMSG (all-0xF6) memo.
 *     2. zmsg_ingest_onchain_note() lands a ZMSG memo in the message store as
 *        an inbound channel=onchain message whose body is the payload, and is
 *        idempotent (a second ingest of the same note dedups by deterministic
 *        msg_id = SHA3(txid‖memo)). A non-ZMSG memo is NOT ingested.
 *
 *   Params-gated (skips cleanly when ~/.zcash-params is absent):
 *     3. A REAL t→z shielded output built with the production Sapling output
 *        prover (sapling_build_output_description) carrying a ZMSG memo, MINED
 *        into the deterministic sim (tree append + hashFinalSaplingRoot stamp +
 *        connect_block), then DECRYPTED by the recipient (ka_agree/kdf/
 *        note_decrypt), PARSED (zmsg_memo_decode), INGESTED, and surfaced in
 *        the store with the original body — the full on-chain ZMSG transport.
 *
 * Consensus is untouched: the memo is opaque free-form bytes. The prover<->
 * verifier gap (see test_simnet_sapling_shielded_send.c) does NOT affect this
 * test — memo decryption authenticates via the AEAD tag, independent of the
 * Groth16 proof, and the mint runs with the contextual verifier OFF (proofs
 * deferred), exactly as the shielded-send harness mints.
 *
 * PARAMS-GATED: the ~50 MB Sapling proving keys are not in-repo; the real leg
 * skips when absent (test_snark_kat pattern). This group is excluded from the
 * fast default pool (group_is_params_heavy) and opted in via ZCL_PARAMS_TESTS=1.
 */

#include "test/test_helpers.h"

#include "sim/simnet.h"
#include "sim/simnet_sapling.h"

#include "sapling/sapling.h"
#include "sapling/params_init.h"
#include "sapling/zip32.h"
#include "sapling/fr.h"
#include "sapling/note_encryption.h"

#include "net/zmsg.h"
#include "models/zmsg.h"

#include "primitives/transaction.h"
#include "validation/sighash.h"
#include "consensus/upgrades.h"
#include "script/script.h"
#include "script/sighashtype.h"
#include "core/uint256.h"
#include "util/safe_alloc.h"
#include "support/cleanse.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ZM_CHECK(name, expr) do {                    \
    printf("  %s... ", (name));                      \
    if (expr) printf("OK\n");                         \
    else { printf("FAIL\n"); failures++; }            \
} while (0)

/* Derive a fixed Sapling spending key + default address from a 32-byte seed
 * (pure ZIP-32, params-free). */
struct zm_sapling_id {
    uint8_t ask[32], nsk[32], ovk[32];
    uint8_t ak[32], nk[32], ivk[32];
    uint8_t d[11], pk_d[32];
};
static bool zm_derive_id(struct zm_sapling_id *id, const uint8_t seed[32])
{
    struct zip32_xsk xsk;
    zip32_xsk_master(&xsk, seed, 32);
    struct zip32_xfvk xfvk;
    zip32_xsk_to_xfvk(&xfvk, &xsk);
    if (!zip32_xfvk_address(&xfvk, id->d, id->pk_d))
        return false;
    memcpy(id->ask, xsk.expsk.ask, 32);
    memcpy(id->nsk, xsk.expsk.nsk, 32);
    memcpy(id->ovk, xsk.expsk.ovk, 32);
    memcpy(id->ak, xfvk.fvk.ak, 32);
    memcpy(id->nk, xfvk.fvk.nk, 32);
    sapling_crh_ivk(id->ak, id->nk, id->ivk);
    return true;
}

/* Binding signature over one output's rcv (bsk = -rcv for a lone output),
 * via the production sapling_create_binding_sig(). */
static bool zm_binding_sig(const uint8_t out_rcv[32], const uint8_t sighash[32],
                           uint8_t binding_sig_out[64])
{
    struct fs term, neg, bsk;
    fs_zero(&bsk);
    if (!fs_from_bytes(&term, out_rcv))
        return false;
    fs_neg(&neg, &term);          /* outputs subtract from bsk */
    fs_add(&bsk, &bsk, &neg);
    uint8_t bsk_bytes[32];
    fs_to_bytes(bsk_bytes, &bsk);
    bool ok = sapling_create_binding_sig(bsk_bytes, sighash, binding_sig_out);
    memory_cleanse(bsk_bytes, sizeof(bsk_bytes));
    memory_cleanse(&bsk, sizeof(bsk));
    return ok;
}

/* Count messages currently in the store matching (channel=onchain, body). */
static int zm_store_has_onchain_body(const char *body)
{
    struct zmsg_message msgs[64];
    int n = zmsg_store_list(msgs, 64, false);
    int hits = 0;
    for (int i = 0; i < n; i++)
        if (msgs[i].channel == ZMSG_CHANNEL_ONCHAIN &&
            strcmp(msgs[i].body, body) == 0)
            hits++;
    return hits;
}

int test_simnet_zmsg_onchain(void);
int test_simnet_zmsg_onchain(void)
{
    printf("\n=== ZMSG on-chain channel: in-sim end-to-end ===\n");
    int failures = 0;

    /* ── Params-free: codec + ingest into the store ── */
    {
        const char *body = "zmsg-onchain params-free ingest probe";
        uint8_t txid[32];
        for (int i = 0; i < 32; i++) txid[i] = (uint8_t)(0x10 + i);

        uint8_t memo[ZMSG_MEMO_LEN];
        ZM_CHECK("encode ZMSG memo",
                 zmsg_memo_encode(memo, (const uint8_t *)body, strlen(body), NULL));

        struct zmsg_memo dec;
        ZM_CHECK("decode ZMSG memo", zmsg_memo_decode(memo, &dec));
        ZM_CHECK("decoded payload matches",
                 dec.payload_len == strlen(body) &&
                 memcmp(dec.payload, body, strlen(body)) == 0);

        int before = zm_store_has_onchain_body(body);
        ZM_CHECK("ingest lands ZMSG in store",
                 zmsg_ingest_onchain_note(NULL, memo, txid));
        ZM_CHECK("store now shows the onchain message",
                 zm_store_has_onchain_body(body) == before + 1);

        /* Idempotent: re-ingest the same note dedups (deterministic msg_id). */
        zmsg_ingest_onchain_note(NULL, memo, txid);
        ZM_CHECK("re-ingest is deduped (no duplicate)",
                 zm_store_has_onchain_body(body) == before + 1);

        /* A non-ZMSG memo is NOT ingested. */
        uint8_t plain[ZMSG_MEMO_LEN];
        memset(plain, ZMSG_MEMO_PAD_BYTE, sizeof(plain));
        ZM_CHECK("non-ZMSG memo is not ingested",
                 !zmsg_ingest_onchain_note(NULL, plain, txid));
    }

    /* ── Params-gated: real prover + mint + decrypt + parse + ingest ── */
    const char *home = getenv("HOME");
    char params_dir[512];
    snprintf(params_dir, sizeof(params_dir), "%s/.zcash-params",
             (home && *home) ? home : ".");
    if (!sapling_init_params(params_dir)) {
        printf("  ~/.zcash-params absent — SKIPPING real-prover leg "
               "(codec + ingest above ran)\n");
        printf("ZMSG on-chain: %s (%d failures, prover leg skipped)\n",
               failures == 0 ? "OK" : "FAIL", failures);
        return failures;
    }
    printf("  ~/.zcash-params present — running REAL prover t->z memo leg\n");

    const int64_t FUND_VALUE     = 100000000;   /* 1 ZCL coinbase to shield */
    const int64_t FEE            = 10000;
    const int64_t SHIELDED_VALUE = FUND_VALUE - FEE;
    const int     SAPLING_H       = 100;

    uint8_t seed32[32];
    memset(seed32, 0x2D, sizeof(seed32));    /* fixed recipient identity */
    struct zm_sapling_id id;
    ZM_CHECK("derive recipient Sapling key + address", zm_derive_id(&id, seed32));

    struct simnet s;
    ZM_CHECK("simnet_init", simnet_init(&s));
    simnet_activate_sapling_at(&s, SAPLING_H);
    ZM_CHECK("enable in-sim Sapling tree", simnet_enable_sapling_tree(&s));
    simnet_enable_contextual_check(&s, false);   /* mint w/o proof verify */

    struct script fund_script;
    script_init(&fund_script);
    { uint8_t pk[3] = {0x76, 0xa9, 0x14}; script_set(&fund_script, pk, sizeof(pk)); }
    struct uint256 cb_txid;
    ZM_CHECK("mint funding coinbase (h=100)",
             simnet_mint_coinbase_to(&s, &fund_script, FUND_VALUE, &cb_txid));
    ZM_CHECK("mature coinbase to h=200",
             simnet_mint_to_height(&s, SAPLING_H + COINBASE_MATURITY));

    /* Build the on-chain ZMSG memo. */
    const char *body = "on-chain ZMSG: agent commerce, greetings.";
    uint8_t memo[ZMSG_MEMO_LEN];
    ZM_CHECK("encode t->z ZMSG memo",
             zmsg_memo_encode(memo, (const uint8_t *)body, strlen(body), NULL));

    struct transaction tz;
    transaction_init(&tz);
    tz.overwintered     = true;
    tz.version          = SAPLING_TX_VERSION;
    tz.version_group_id = SAPLING_VERSION_GROUP_ID;
    tz.lock_time        = 0;
    tz.expiry_height    = 0;
    ZM_CHECK("alloc t->z transparent input", transaction_alloc(&tz, 1, 0));
    tz.vin[0].prevout.hash = cb_txid;
    tz.vin[0].prevout.n    = 0;
    tz.vin[0].sequence     = 0xFFFFFFFF;
    { uint8_t ss[2] = {0x00, 0x00}; script_set(&tz.vin[0].script_sig, ss, sizeof(ss)); }
    tz.value_balance = -SHIELDED_VALUE;

    tz.v_shielded_output = zcl_calloc(1, sizeof(struct output_description), "zmsg_tz_out");
    ZM_CHECK("alloc t->z shielded output", tz.v_shielded_output != NULL);
    tz.num_shielded_output = 1;

    int tz_height = SAPLING_H + COINBASE_MATURITY + 1;
    bool built = false;
    if (tz.v_shielded_output) {
        struct output_description *od = &tz.v_shielded_output[0];
        uint8_t tz_out_rcv[32];
        built = sapling_build_output_description(
                    id.ovk, id.d, id.pk_d, (uint64_t)SHIELDED_VALUE,
                    memo, od->cv.data, od->cm.data, od->ephemeral_key.data,
                    od->enc_ciphertext, od->out_ciphertext, od->zkproof,
                    tz_out_rcv);
        ZM_CHECK("build t->z output carrying ZMSG memo (real prover)", built);

        transaction_compute_hash(&tz);
        uint32_t branch = consensus_current_epoch_branch_id(tz_height,
                                                            &s.params.consensus);
        struct sighash_type ht; ht.raw = 1;
        struct precomputed_tx_data txd; precompute_tx_data(&tz, &txd);
        struct script empty; empty.size = 0;
        struct uint256 sighash;
        ZM_CHECK("t->z binding sighash",
                 signature_hash(&empty, &tz, NOT_AN_INPUT, ht, 0, branch,
                                &txd, &sighash));
        ZM_CHECK("t->z binding sig",
                 built && zm_binding_sig(tz_out_rcv, sighash.data, tz.binding_sig));
    }

    transaction_compute_hash(&tz);
    struct uint256 tz_txid = tz.hash;

    /* Recipient-side receive: decrypt the shielded output, parse the ZMSG memo,
     * and ingest it. Done BEFORE the mint because simnet_mint_txs moves the tx
     * into the block and transaction_init()s the caller's copy (zeroing it);
     * the note bytes are identical whether or not mined. */
    if (built && tz.v_shielded_output) {
        struct output_description *od = &tz.v_shielded_output[0];
        uint8_t dhsecret[32], enckey[32], pt[564];
        bool dec = sapling_ka_agree(od->ephemeral_key.data, id.ivk, dhsecret) &&
                   sapling_kdf(enckey, dhsecret, od->ephemeral_key.data) &&
                   sapling_note_decrypt(enckey, od->enc_ciphertext,
                                        sizeof(od->enc_ciphertext), pt);
        ZM_CHECK("recipient decrypts note (AEAD authenticates)", dec);

        if (dec) {
            /* Memo occupies the trailing 512 bytes of the decrypted plaintext. */
            struct zmsg_memo parsed;
            ZM_CHECK("decrypted memo parses as ZMSG",
                     zmsg_memo_decode(pt + 52, &parsed));
            ZM_CHECK("parsed payload matches original body",
                     parsed.payload_len == strlen(body) &&
                     memcmp(parsed.payload, body, strlen(body)) == 0);

            int before = zm_store_has_onchain_body(body);
            ZM_CHECK("ingest decrypted ZMSG into store",
                     zmsg_ingest_onchain_note(NULL, pt + 52, tz_txid.data));
            ZM_CHECK("store surfaces the on-chain message",
                     zm_store_has_onchain_body(body) == before + 1);
        }
    }

    /* Mine the t->z into the sim (tree append + root stamp + connect_block).
     * simnet_mint_txs zeroes `tz`, so keep the shielded-output pointer to free
     * afterwards (the harness owns only vin/vout). */
    struct output_description *shielded_owned = tz.v_shielded_output;
    ZM_CHECK("mint t->z (mined in-sim)", simnet_mint_txs(&s, &tz, 1));
    ZM_CHECK("tree has 1 note after t->z", simnet_sapling_tree_size(&s) == 1);
    free(shielded_owned);
    simnet_free(&s);

    printf("ZMSG on-chain: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
