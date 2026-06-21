/* Unit tests for coins_kv — the reducer's canonical UTXO set as a `coins` table
 * IN progress.kv. The load-bearing assertion is ATOMICITY: a coins mutation runs
 * on the progress.kv handle and therefore honors the caller's enclosing
 * transaction (rollback leaves the set unchanged). That is the entire point of
 * the move — coins commit or roll back together with the stage cursor, closing
 * the tip-wedge tear class (docs/work/tip-durability-collapse.md). */

#include "test/test_helpers.h"

#include "chain/checkpoints.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CK_CHECK(name, expr) do {                                       \
    if (expr) { printf("  coins_kv: %s... OK\n", (name)); }             \
    else { printf("  coins_kv: %s... FAIL\n", (name)); failures++; }    \
} while (0)

static struct uint256 ck_txid(uint8_t tag)
{
    struct uint256 t; uint256_set_null(&t);
    t.data[0] = tag; t.data[1] = 0xC0; t.data[31] = 0x99;
    return t;
}

/* ── anchor-coin byte-flip detected by the SHA3 UTXO commitment ──────────────
 *
 * Fault class: flip the value OR the scriptPubKey of one coin AT/BELOW the
 * anchor. This is NOT detected by reducer_frontier_compute_hstar (it reads the
 * *_log height/ok/hash columns, never coin VALUE or SCRIPT bytes, so H* is
 * unchanged). The honest detector is the SHA3 UTXO commitment compared against
 * the compiled checkpoint sha3_hash — exactly the `anchor_proven` predicate the
 * boot anchor-seed gate builds (boot_refold_from_anchor_reset,
 * config/src/boot_refold_staged.c:332-354).
 *
 * The clean coin set's commitment IS installed as the checkpoint override, so
 * the clean set passes the anchor proof by construction; the flipped set must
 * FAIL it (memcmp != 0). We assert the PREDICATE, never the _exit side effect
 * (which would kill the test process).
 *
 * Negative control (flip RED): in coins_kv_commitment() at
 * lib/storage/src/coins_kv.c, drop `value` from the absorbed record — pass a
 * constant 0 for the value field. The value flip then no longer changes the
 * commitment, so memcmp(root_flipped, cp->sha3_hash, 32) == 0 and the `!= 0`
 * assert FAILS — a corrupt set would be accepted as the proven anchor.
 * (Symmetric: pass NULL/0 for the script argument at the same call site -> the
 * script flip goes undetected and assertion C fails.) */
static int ck_case_anchor_coin_byte_flip(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "coins_kv_anchor_flip", "main");

    CK_CHECK("anchor-flip: progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    CK_CHECK("anchor-flip: db handle", db != NULL);
    CK_CHECK("anchor-flip: ensure_schema", coins_kv_ensure_schema(db));

    /* A small deterministic anchor coin set, all at/below a fixture anchor
     * height A_fix. */
    const int32_t a_fix = 100;
    struct uint256 a = ck_txid(0x11), b = ck_txid(0x22), c = ck_txid(0x33);
    uint8_t sc_a[4] = {0xA0, 0xA1, 0xA2, 0xA3};
    uint8_t sc_b[2] = {0xB0, 0xB1};
    bool seeded =
        coins_kv_add(db, a.data, 0, 5000, a_fix, true,  sc_a, sizeof(sc_a))
        && coins_kv_add(db, a.data, 1, 6000, a_fix, true,  sc_b, sizeof(sc_b))
        && coins_kv_add(db, b.data, 0, 7000, a_fix - 1, false, sc_a, sizeof(sc_a))
        && coins_kv_add(db, c.data, 0, 8000, a_fix - 2, false, NULL, 0);
    CK_CHECK("anchor-flip: clean anchor set seeded", seeded);

    /* The clean set IS the trusted anchor: its commitment + count become the
     * (overridden) compiled checkpoint. cp must outlive the override (function
     * scope). */
    uint8_t root_clean[32] = {0};
    int64_t count_clean = coins_kv_count(db);
    bool root_ok = coins_kv_commitment(db, root_clean) == 0;
    CK_CHECK("anchor-flip: clean commitment computes", root_ok && count_clean == 4);

    struct sha3_utxo_checkpoint cp;
    memset(&cp, 0, sizeof(cp));
    cp.height = a_fix;
    memcpy(cp.sha3_hash, root_clean, 32);
    cp.utxo_count = (uint64_t)count_clean;
    cp.total_supply = 5000 + 6000 + 7000 + 8000;
    checkpoints_set_sha3_override_for_test(&cp);

    const struct sha3_utxo_checkpoint *cpp = get_sha3_utxo_checkpoint();
    CK_CHECK("anchor-flip: override installed", cpp != NULL);

    /* A. CLEAN set passes the anchor proof (gate is reachable, not vacuous). */
    if (cpp) {
        uint8_t root_now[32] = {0};
        bool now_ok = coins_kv_commitment(db, root_now) == 0;
        CK_CHECK("anchor-flip: A clean == checkpoint",
                 now_ok && memcmp(root_now, cpp->sha3_hash, 32) == 0 &&
                 coins_kv_count(db) == (int64_t)cpp->utxo_count);

        /* B. VALUE byte-flip on one coin at the anchor -> anchor proof REFUSED.
         * Spend + re-add with a flipped value (same primitives the live mutation
         * uses). */
        bool flipped = coins_kv_spend(db, b.data, 0) &&
                       coins_kv_add(db, b.data, 0, 7001, a_fix - 1, false,
                                    sc_a, sizeof(sc_a));
        uint8_t root_flipped[32] = {0};
        bool flip_root_ok = coins_kv_commitment(db, root_flipped) == 0;
        bool anchor_proven =
            (memcmp(root_flipped, cpp->sha3_hash, 32) == 0) &&
            (coins_kv_count(db) == (int64_t)cpp->utxo_count);
        CK_CHECK("anchor-flip: B value flip FAILS the SHA3 gate",
                 flipped && flip_root_ok &&
                 memcmp(root_flipped, cpp->sha3_hash, 32) != 0 &&
                 !anchor_proven);

        /* Restore the clean value before the script sub-case. */
        bool restored = coins_kv_spend(db, b.data, 0) &&
                        coins_kv_add(db, b.data, 0, 7000, a_fix - 1, false,
                                     sc_a, sizeof(sc_a));
        uint8_t root_restored[32] = {0};
        CK_CHECK("anchor-flip: restore clean value",
                 restored && coins_kv_commitment(db, root_restored) == 0 &&
                 memcmp(root_restored, cpp->sha3_hash, 32) == 0);

        /* C. scriptPubKey byte-flip (same length, one byte changed) -> also
         * REFUSED. */
        uint8_t sc_a_mut[4] = {0xA0, 0xA1, 0xA2, 0xFF};  /* last byte changed */
        bool spk_flipped = coins_kv_spend(db, b.data, 0) &&
                           coins_kv_add(db, b.data, 0, 7000, a_fix - 1, false,
                                        sc_a_mut, sizeof(sc_a_mut));
        uint8_t root_spk[32] = {0};
        CK_CHECK("anchor-flip: C script flip FAILS the SHA3 gate",
                 spk_flipped && coins_kv_commitment(db, root_spk) == 0 &&
                 memcmp(root_spk, cpp->sha3_hash, 32) != 0);
    }

    /* NAMED-BLOCKER tie-in: on `!anchor_proven` the boot anchor-seed gate emits
     * EV_BOOT_VALIDATION_FAILED ("check=refold_from_anchor anchor_h=<A> reseed
     * anchor set mismatch ...") then _exit(EXIT_FAILURE)
     * (boot_refold_staged.c:338-354). The unit-level invariant is exactly the
     * refused predicate above; the _exit side is covered by the named-blocker
     * string, not re-exercised here (it would terminate the test process). The
     * forked-child _exit path is covered by test_refold_from_anchor_fatal. */

    checkpoints_reset_sha3_override_for_test();
    progress_store_close();
    return failures;
}

int test_coins_kv(void);
int test_coins_kv(void)
{
    int failures = 0;
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "coins_kv", "main");

    CK_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    CK_CHECK("db handle", db != NULL);

    /* get before schema -> graceful false (no crash). */
    {
        struct coins c; struct uint256 z = ck_txid(0x01);
        CK_CHECK("get before schema -> false", !coins_kv_get_coins(db, z.data, &c));
        coins_free(&c);
    }

    CK_CHECK("ensure_schema", coins_kv_ensure_schema(db));
    CK_CHECK("ensure_schema idempotent", coins_kv_ensure_schema(db));
    CK_CHECK("count empty == 0", coins_kv_count(db) == 0);

    /* Add a txid with 2 outputs (distinct value/script), height 100, coinbase. */
    struct uint256 t1 = ck_txid(0x11);
    unsigned char sc0[5] = {0xA0,0xA0,0xA0,0xA0,0xA0};
    unsigned char sc1[3] = {0xB1,0xB1,0xB1};
    CK_CHECK("add t1.0", coins_kv_add(db, t1.data, 0, 5000, 100, true,  sc0, sizeof(sc0)));
    CK_CHECK("add t1.1", coins_kv_add(db, t1.data, 1, 6000, 100, true,  sc1, sizeof(sc1)));
    CK_CHECK("count == 2", coins_kv_count(db) == 2);
    CK_CHECK("exists t1.0", coins_kv_exists(db, t1.data, 0));
    CK_CHECK("exists t1.1", coins_kv_exists(db, t1.data, 1));
    CK_CHECK("not exists t1.2", !coins_kv_exists(db, t1.data, 2));

    /* Round-trip via get_coins: values, scripts, height, is_coinbase exact. */
    {
        struct coins c;
        bool got = coins_kv_get_coins(db, t1.data, &c);
        bool ok = got && c.num_vout >= 2 && c.is_coinbase && c.height == 100
            && c.vout[0].value == 5000 && c.vout[1].value == 6000
            && c.vout[0].script_pub_key.size == sizeof(sc0)
            && memcmp(c.vout[0].script_pub_key.data, sc0, sizeof(sc0)) == 0
            && c.vout[1].script_pub_key.size == sizeof(sc1)
            && memcmp(c.vout[1].script_pub_key.data, sc1, sizeof(sc1)) == 0;
        CK_CHECK("get_coins round-trip", ok);
        coins_free(&c);
    }

    /* Spend one vout; the other survives; counts/reads reflect it. */
    CK_CHECK("spend t1.0", coins_kv_spend(db, t1.data, 0));
    CK_CHECK("count == 1 after spend", coins_kv_count(db) == 1);
    CK_CHECK("not exists t1.0 after spend", !coins_kv_exists(db, t1.data, 0));
    CK_CHECK("exists t1.1 after spend", coins_kv_exists(db, t1.data, 1));
    {
        struct coins c;
        bool got = coins_kv_get_coins(db, t1.data, &c);
        /* vout0 spent -> null slot; vout1 still live. */
        bool ok = got && c.num_vout >= 2
            && tx_out_is_null(&c.vout[0]) && !tx_out_is_null(&c.vout[1]);
        CK_CHECK("get_coins reflects spend", ok);
        coins_free(&c);
    }

    /* Spend the last vout -> txid has no live outputs -> get_coins false. */
    CK_CHECK("spend t1.1", coins_kv_spend(db, t1.data, 1));
    {
        struct coins c;
        CK_CHECK("get_coins all-spent -> false", !coins_kv_get_coins(db, t1.data, &c));
        coins_free(&c);
    }
    CK_CHECK("count == 0 after all spent", coins_kv_count(db) == 0);

    /* ── ATOMICITY: a coins mutation honors the enclosing txn ──────────── */
    struct uint256 t2 = ck_txid(0x22);
    /* ROLLBACK case: add inside a txn, then roll back -> set unchanged. */
    CK_CHECK("BEGIN (rollback case)",
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CK_CHECK("add t2.0 in txn", coins_kv_add(db, t2.data, 0, 7000, 200, false, sc0, sizeof(sc0)));
    CK_CHECK("exists t2.0 inside txn", coins_kv_exists(db, t2.data, 0));
    CK_CHECK("ROLLBACK", sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CK_CHECK("t2.0 GONE after rollback (atomic)", !coins_kv_exists(db, t2.data, 0));
    CK_CHECK("count still 0 after rollback", coins_kv_count(db) == 0);

    /* COMMIT case: same add inside a txn, commit -> persists. */
    CK_CHECK("BEGIN (commit case)",
             sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CK_CHECK("add t2.0 in txn (commit)", coins_kv_add(db, t2.data, 0, 7000, 200, false, sc0, sizeof(sc0)));
    CK_CHECK("COMMIT", sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CK_CHECK("t2.0 present after commit", coins_kv_exists(db, t2.data, 0));
    CK_CHECK("count == 1 after commit", coins_kv_count(db) == 1);

    progress_store_close();

    /* anchor-coin byte-flip detected by the SHA3 commitment (own store/tmpdir). */
    failures += ck_case_anchor_coin_byte_flip();

    return failures;
}
