/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * REAL-crypto equivalence test for the parallel verification engine
 * (lib/validation/src/thread_pool.c + verify_queue.c).
 *
 * The companion test_verify_engine.c pins the engine CONTRACT with synthetic
 * (index%7) verifiers — it never runs a real signature check and never proves
 * any crypto executed. This test is complementary: it drives REAL ECDSA
 * script verification (privkey -> pubkey -> P2PK scriptPubKey -> real
 * signature_hash -> privkey_sign -> scriptSig -> verify_script) through the
 * SAME engine, serially (-par=1) and fanned across N workers, and proves:
 *
 *   1. Each SCRIPT_CHECK job replicates the bg_validation per-input verifier
 *      (bg_validation_scripts.c:28-41 verify_script_item) EXACTLY: build a
 *      tx_sig_checker over the item, then verify_script(scriptSig, scriptPubKey,
 *      flags, checker, branch_id). The struct script_check_item shape is
 *      sibling-private to app/services, so its 7 fields are replicated here as
 *      a test-local `struct sci`; the verifier body is byte-for-byte the same.
 *   2. The batch verdict AND every per-item verdict are IDENTICAL whether run
 *      serially (NULL pool, the -par=1 path) or across 4 worker threads.
 *   3. The ENGINE has no built-in "crypto ran" counter (verify_queue keeps no
 *      shared counter — results land in per-job slots only). So this test owns
 *      a file-scope atomic counter, bumped INSIDE the job fn right before each
 *      verify_script call. It is reset before each run and asserted == the item
 *      count in BOTH modes and EQUAL across modes — proving real ECDSA actually
 *      executed n times on each path (not stubbed) and that parallelism dropped
 *      or duplicated NO work. (Mirrors how the stage tests assert
 *      verified_total in test_script_validate_stage.c.)
 *   4. A planted bad-signature item is REJECTED in both modes, so the AND-reduce
 *      batch verdict is false on both paths.
 *
 * Determinism: privkey_sign uses RFC6979 deterministic nonces and every item's
 * verdict is a pure function of its self-contained payload (no rand, no clock),
 * so the result is independent of which worker picks up which item. The keys
 * are drawn fresh per run but the PASS/FAIL classification is fixed by
 * construction (only the planted item is corrupted), which is what the test
 * asserts — not the specific key bytes.
 *
 * NOT wired into the fold / staged reducer / any consensus path. Additive test
 * only.
 */

#include "test/test_helpers.h"

#include "validation/verify_queue.h"
#include "validation/thread_pool.h"
#include "validation/tx_verifier.h"
#include "validation/sighash.h"

#include "keys/key.h"
#include "keys/pubkey.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "script/script_flags.h"
#include "script/standard.h"
#include "primitives/transaction.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define VEE_CHECK(name, expr) do {                      \
    printf("verify_engine_equiv: %s... ", (name));      \
    if (expr) printf("OK\n");                            \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

/* The Sapling network upgrade branch id used by the existing signing tests
 * (test_multisig.c, test_sighash_edge.c). */
#define VEE_BRANCH_ID 0x76b809bb

/* Replica of the sibling-private struct script_check_item
 * (app/services/src/bg_validation_internal.h:21-29). Same 7 fields, same
 * types — so the verify_script_item body below is byte-for-byte identical. */
struct sci {
    const struct transaction *tx;
    unsigned int input_index;
    int64_t amount;
    uint32_t branch_id;
    struct precomputed_tx_data txdata;
    struct script script_pub_key;
    uint32_t flags;
};

/* "Crypto executed" counter the ENGINE deliberately does not provide. Bumped
 * inside the job fn immediately before each real verify_script call. */
static _Atomic uint64_t g_sci_verified = 0;

/* The pure verifier the engine runs for each SCRIPT_CHECK job. Body replicates
 * bg_validation_scripts.c:28-41 verify_script_item EXACTLY (cast arg -> item,
 * init a tx_sig_checker over the item, build the sig_checker, verify_script),
 * with one added line: count that real ECDSA verification ran. Pure function of
 * its arg (the only shared touch is an atomic increment, which cannot change
 * any per-job verdict). */
static bool vee_verify_script_item(void *item)
{
    struct sci *sc = item;

    atomic_fetch_add_explicit(&g_sci_verified, 1, memory_order_relaxed);

    struct tx_sig_checker tsc;
    tx_sig_checker_init(&tsc, sc->tx, sc->input_index,
                        sc->amount, sc->branch_id, &sc->txdata);
    struct sig_checker checker = tx_make_sig_checker(&tsc);

    ScriptError serror = SCRIPT_ERR_OK;
    return verify_script(&sc->tx->vin[sc->input_index].script_sig,
                         &sc->script_pub_key,
                         sc->flags, &checker,
                         sc->branch_id, &serror);
}

/* Build ONE real P2PK spend. `make_bad` plants a signature over the WRONG hash
 * so verify_script returns false (a known-FAIL item). On success the tx owns
 * the scriptSig; the item points its scriptPubKey + txdata at the built spend.
 * Returns true on a clean construction (crypto build succeeded). The amount and
 * a stable per-item flag set match the bg_validation verifier's inputs. */
static bool vee_build_item(struct sci *out,
                           struct transaction *tx,    /* caller-owned storage  */
                           struct tx_in *vin,         /* caller-owned, 1 entry */
                           struct tx_out *vout,       /* caller-owned, 1 entry */
                           struct privkey *key,       /* caller-owned          */
                           int64_t amount,
                           bool make_bad)
{
    privkey_make_new(key, true);
    struct pubkey pub;
    if (!privkey_get_pubkey(key, &pub))
        return false;

    /* P2PK prevout scriptPubKey: <pubkey> OP_CHECKSIG. */
    struct script spk;
    script_init(&spk);
    if (!script_push_data(&spk, pub.vch, pub.size))
        return false;
    if (!script_push_op(&spk, OP_CHECKSIG))
        return false;

    /* Minimal Sapling v4 tx, 1 vin / 1 vout (spending the P2PK output). */
    memset(tx, 0, sizeof(*tx));
    tx->overwintered = true;
    tx->version = SAPLING_TX_VERSION;
    tx->version_group_id = SAPLING_VERSION_GROUP_ID;

    memset(vin, 0, sizeof(*vin));
    tx_in_init(vin);
    vin->sequence = 0xffffffff;
    tx->vin = vin;
    tx->num_vin = 1;

    memset(vout, 0, sizeof(*vout));
    vout->value = amount;
    struct key_id dummy_kid;
    memset(&dummy_kid, 0, sizeof(dummy_kid));
    script_for_p2pkh(&vout->script_pub_key, &dummy_kid);
    tx->vout = vout;
    tx->num_vout = 1;

    /* Real sighash over the P2PK scriptPubKey (the subscript), SIGHASH_ALL. */
    struct sighash_type ht;
    ht.raw = SIGHASH_ALL;
    struct precomputed_tx_data txdata;
    precompute_tx_data(tx, &txdata);

    struct uint256 sighash;
    if (!signature_hash(&spk, tx, 0, ht, amount,
                        VEE_BRANCH_ID, &txdata, &sighash))
        return false;

    /* For the planted-bad item, sign a DIFFERENT hash so the ECDSA check
     * genuinely runs and genuinely FAILS (real crypto, false verdict). */
    struct uint256 to_sign = sighash;
    if (make_bad)
        to_sign.data[0] ^= 0x01;

    unsigned char sig[80];
    size_t siglen = sizeof(sig);
    if (!privkey_sign(key, &to_sign, sig, &siglen) || siglen > 72)
        return false;
    sig[siglen++] = SIGHASH_ALL;

    /* scriptSig: <DER sig || hashtype>. */
    struct script *ss = &tx->vin[0].script_sig;
    script_init(ss);
    if (!script_push_data(ss, sig, siglen))
        return false;

    out->tx = tx;
    out->input_index = 0;
    out->amount = amount;
    out->branch_id = VEE_BRANCH_ID;
    out->txdata = txdata;
    out->script_pub_key = spk;
    out->flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    return true;
}

#define VEE_N 24                /* > 4 so the pool genuinely fans out */
#define VEE_BAD_IDX 17          /* the one planted-bad item               */

int test_verify_engine_equiv(void)
{
    printf("\n=== parallel verify engine: REAL-crypto equivalence ===\n");
    int failures = 0;

    /* ── Build VEE_N real P2PK spends; index VEE_BAD_IDX is a known FAIL. ── */
    static struct transaction txs[VEE_N];
    static struct tx_in       vins[VEE_N];
    static struct tx_out      vouts[VEE_N];
    static struct privkey     keys[VEE_N];
    static struct sci         items[VEE_N];

    bool built_all = true;
    for (int i = 0; i < VEE_N; i++) {
        bool bad = (i == VEE_BAD_IDX);
        int64_t amount = 100000000 + (int64_t)i; /* deterministic, distinct */
        if (!vee_build_item(&items[i], &txs[i], &vins[i], &vouts[i],
                            &keys[i], amount, bad))
            built_all = false;
    }
    VEE_CHECK("built VEE_N real P2PK spends (incl. one planted-bad)", built_all);
    if (!built_all)
        return failures; /* cannot meaningfully proceed */

    /* Wrap each item as a SCRIPT_CHECK verify_queue job. */
    struct verify_job sj[VEE_N], pj[VEE_N];
    for (int i = 0; i < VEE_N; i++) {
        sj[i].kind = VERIFY_JOB_SCRIPT_CHECK;
        sj[i].fn   = vee_verify_script_item;
        sj[i].arg  = &items[i];
        sj[i].result = false;
        pj[i] = sj[i];
    }

    /* ── Serial run (-par=1 path: NULL pool forces inline execution). ────── */
    atomic_store(&g_sci_verified, 0);
    bool serial_batch = verify_queue_submit_batch(NULL, sj, VEE_N);
    uint64_t serial_count = atomic_load(&g_sci_verified);

    /* The batch must be FALSE (the planted-bad item fails), and exactly one
     * item (VEE_BAD_IDX) must have a false per-job verdict. */
    VEE_CHECK("serial batch verdict is false (planted-bad item rejected)",
              serial_batch == false);

    bool serial_only_bad_failed = true;
    for (int i = 0; i < VEE_N; i++) {
        bool expect = (i != VEE_BAD_IDX);
        if (sj[i].result != expect) serial_only_bad_failed = false;
    }
    VEE_CHECK("serial: every good item passed, only planted-bad failed",
              serial_only_bad_failed);
    VEE_CHECK("serial: planted-bad item per-job verdict is false",
              sj[VEE_BAD_IDX].result == false);

    /* Real ECDSA actually executed once per item on the serial path. */
    VEE_CHECK("serial: crypto ran exactly VEE_N times (real ECDSA, not stubbed)",
              serial_count == (uint64_t)VEE_N);

    /* ── Parallel run: 4 worker threads (force >1 fan-out). ──────────────── */
    struct thread_pool pool;
    bool started = thread_pool_start(&pool, 4);
    VEE_CHECK("pool start (4 workers)",
              started && thread_pool_worker_count(&pool) == 4);

    atomic_store(&g_sci_verified, 0);
    bool par_batch = verify_queue_submit_batch(&pool, pj, VEE_N);
    uint64_t par_count = atomic_load(&g_sci_verified);

    /* Batch verdict identical to serial. */
    VEE_CHECK("parallel batch verdict == serial batch verdict",
              par_batch == serial_batch);

    /* Every per-item verdict identical to serial. */
    bool perjob_match = true;
    for (int i = 0; i < VEE_N; i++)
        if (pj[i].result != sj[i].result) perjob_match = false;
    VEE_CHECK("parallel per-item verdicts == serial per-item verdicts",
              perjob_match);

    /* The planted-bad item is rejected on the parallel path too. */
    VEE_CHECK("parallel: planted-bad item per-job verdict is false",
              pj[VEE_BAD_IDX].result == false);

    /* Real ECDSA executed once per item on the parallel path — no work
     * dropped, none duplicated. */
    VEE_CHECK("parallel: crypto ran exactly VEE_N times (no drop/dup)",
              par_count == (uint64_t)VEE_N);

    /* And the count is EQUAL across modes. */
    VEE_CHECK("crypto-run count equal serial vs parallel",
              par_count == serial_count);

    thread_pool_stop(&pool);

    /* ── Re-run with the bad item PATCHED to good: batch flips to true ───── */
    /* Rebuild item VEE_BAD_IDX as a valid spend (sign the REAL sighash). */
    {
        bool rebuilt = vee_build_item(&items[VEE_BAD_IDX], &txs[VEE_BAD_IDX],
                                      &vins[VEE_BAD_IDX], &vouts[VEE_BAD_IDX],
                                      &keys[VEE_BAD_IDX],
                                      100000000 + VEE_BAD_IDX, false /*good*/);
        VEE_CHECK("rebuilt planted item as a valid spend", rebuilt);

        for (int i = 0; i < VEE_N; i++) {
            sj[i].arg = &items[i]; sj[i].result = false;
            pj[i].arg = &items[i]; pj[i].result = false;
        }

        atomic_store(&g_sci_verified, 0);
        bool all_good_serial = verify_queue_submit_batch(NULL, sj, VEE_N);
        uint64_t c_serial = atomic_load(&g_sci_verified);

        struct thread_pool pool2;
        thread_pool_start(&pool2, 4);
        atomic_store(&g_sci_verified, 0);
        bool all_good_par = verify_queue_submit_batch(&pool2, pj, VEE_N);
        uint64_t c_par = atomic_load(&g_sci_verified);
        thread_pool_stop(&pool2);

        VEE_CHECK("all-good batch reduces to true (serial)",
                  all_good_serial == true);
        VEE_CHECK("all-good batch verdict == across modes",
                  all_good_par == all_good_serial);
        bool perjob_match2 = true;
        for (int i = 0; i < VEE_N; i++)
            if (pj[i].result != sj[i].result || pj[i].result != true)
                perjob_match2 = false;
        VEE_CHECK("all-good per-item verdicts all true + equal across modes",
                  perjob_match2);
        VEE_CHECK("all-good: crypto ran VEE_N times equal across modes",
                  c_serial == (uint64_t)VEE_N && c_par == c_serial);
    }

    return failures;
}
