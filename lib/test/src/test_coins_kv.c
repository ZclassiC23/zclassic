/* Unit tests for coins_kv — the reducer's canonical UTXO set as a `coins` table
 * IN progress.kv. The load-bearing assertion is ATOMICITY: a coins mutation runs
 * on the progress.kv handle and therefore honors the caller's enclosing
 * transaction (rollback leaves the set unchanged). That is the entire point of
 * the move — coins commit or roll back together with the stage cursor, closing
 * the tip-wedge tear class (docs/work/tip-durability-collapse.md). */

#include "test/test_helpers.h"

#include "coins/coins_view.h"
#include "core/uint256.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
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
    return failures;
}
