/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit test for utxo_mirror_sync — the node.db `utxos` mirror feeder that
 * keeps the explorer read model synced to the authoritative coins_kv set.
 *
 * The load-bearing assertions:
 *   - a frozen mirror (cold-import seed) below the coins_kv applied frontier
 *     is DETECTED as drift and rebuilt to match coins_kv exactly;
 *   - the rebuilt rows carry the DERIVED explorer columns (script_type +
 *     address_hash) computed from each scriptPubKey, so address balances are
 *     correct after the rebuild;
 *   - the durable cursor (node.db state key) advances to the frontier and is
 *     read back on the next pass so a no-drift mirror does NO work;
 *   - a spent coin (absent from coins_kv) is removed from the mirror;
 *   - the feeder NEVER touches coins_kv (consensus authority untouched).
 */

#include "test/test_helpers.h"

#include "config/db_service.h"
#include "config/runtime.h"
#include "models/database.h"
#include "models/utxo.h"
#include "script/standard.h"
#include "services/utxo_mirror_sync_service.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define UMS_CHECK(name, expr) do {                                          \
    if (expr) { printf("  utxo_mirror_sync: %s... OK\n", (name)); }          \
    else { printf("  utxo_mirror_sync: %s... FAIL\n", (name)); failures++; } \
} while (0)

/* A 25-byte P2PKH script with a recognisable 20-byte hash derived from tag. */
static void ums_p2pkh(uint8_t script[25], uint8_t hash20[20], uint8_t tag)
{
    memset(hash20, 0, 20);
    hash20[0] = tag; hash20[19] = 0xA5;
    script[0] = 0x76; /* OP_DUP */
    script[1] = 0xa9; /* OP_HASH160 */
    script[2] = 0x14; /* push 20 */
    memcpy(script + 3, hash20, 20);
    script[23] = 0x88; /* OP_EQUALVERIFY */
    script[24] = 0xac; /* OP_CHECKSIG */
}

static void ums_txid(uint8_t out[32], uint8_t tag)
{
    memset(out, 0, 32);
    out[0] = tag; out[31] = 0x9c;
}

/* Set the coins_kv applied frontier inside its own txn (a writer's job; the
 * feeder only reads it). */
static bool ums_set_frontier(sqlite3 *db, int32_t h)
{
    char *err = NULL;
    bool ok = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) == SQLITE_OK
              && coins_kv_set_applied_height_in_tx(db, h)
              && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    if (!ok) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return ok;
}

/* Seed one frozen row directly into the node.db mirror (the cold-import seed
 * the feeder must bring forward). */
static bool ums_seed_mirror_row(struct node_db *ndb, uint8_t tag, int32_t h)
{
    uint8_t txid[32]; ums_txid(txid, tag);
    uint8_t script[25], hash20[20]; ums_p2pkh(script, hash20, tag);
    struct db_utxo u; memset(&u, 0, sizeof(u));
    memcpy(u.txid, txid, 32);
    u.vout = 0; u.value = 1000; u.height = h; u.is_coinbase = true;
    u.script = script; u.script_len = sizeof(script);
    u.script_type = utxo_classify_script(u.script, u.script_len,
                                         u.address_hash, &u.has_address);
    return db_utxo_save(ndb, &u);
}

/* Add one coin to the authoritative coins_kv set. */
static bool ums_add_coin(sqlite3 *pdb, uint8_t tag, int64_t value, int32_t h)
{
    uint8_t txid[32]; ums_txid(txid, tag);
    uint8_t script[25], hash20[20]; ums_p2pkh(script, hash20, tag);
    return coins_kv_add(pdb, txid, 0, value, h, true, script, sizeof(script));
}

int test_utxo_mirror_sync(void);
int test_utxo_mirror_sync(void)
{
    printf("\n=== utxo_mirror_sync tests ===\n");
    int failures = 0;

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "utxo_mirror_sync", "main");

    char ndb_path[600];
    snprintf(ndb_path, sizeof(ndb_path), "%s/node.db", dir);

    struct node_db ndb;
    bool db_ok = node_db_open(&ndb, ndb_path);
    UMS_CHECK("node.db opens", db_ok);
    if (!db_ok) { test_cleanup_tmpdir(dir); return failures; }

    UMS_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *pdb = progress_store_db();
    UMS_CHECK("coins_kv schema", coins_kv_ensure_schema(pdb));

    /* ── Fixture: a FROZEN mirror (2 rows at h=100) while coins_kv holds the
     * live set (4 coins through h=104) — the exact cold-import-freeze shape. */
    UMS_CHECK("seed frozen mirror tag=0x11", ums_seed_mirror_row(&ndb, 0x11, 100));
    UMS_CHECK("seed frozen mirror tag=0x22", ums_seed_mirror_row(&ndb, 0x22, 100));
    UMS_CHECK("mirror count == 2 (frozen)", db_utxo_count(&ndb) == 2);

    /* coins_kv: the same two coins PLUS two created after the freeze. */
    UMS_CHECK("coin 0x11", ums_add_coin(pdb, 0x11, 1000, 100));
    UMS_CHECK("coin 0x22", ums_add_coin(pdb, 0x22, 1000, 100));
    UMS_CHECK("coin 0x33 (post-freeze)", ums_add_coin(pdb, 0x33, 2222, 103));
    UMS_CHECK("coin 0x44 (post-freeze)", ums_add_coin(pdb, 0x44, 3333, 104));
    UMS_CHECK("coins_kv count == 4", coins_kv_count(pdb) == 4);
    UMS_CHECK("set frontier 105", ums_set_frontier(pdb, 105));

    /* ── The feeder. */
    struct db_service dbsvc;
    struct app_runtime_context runtime;
    struct utxo_mirror_sync_service svc;
    memset(&runtime, 0, sizeof(runtime));
    db_service_init(&dbsvc);
    UMS_CHECK("db service attaches", db_service_attach(&dbsvc, &ndb));
    UMS_CHECK("db service starts", db_service_start(&dbsvc));
    runtime.db_service = &dbsvc;
    app_runtime_set_current(&runtime);
    utxo_mirror_sync_init(&svc, &ndb);

    /* NULL/closed-db guard. */
    UMS_CHECK("run_once(NULL) -> -1", utxo_mirror_sync_run_once(NULL) == -1);

    UMS_CHECK("open sync batch before mirror pass", db_service_begin_write(&dbsvc));
    ndb.sync_in_batch = true;

    /* Pass 1: drift detected, mirror rebuilt to match coins_kv (4 rows). */
    int64_t written = utxo_mirror_sync_run_once(&svc);
    UMS_CHECK("pass1 wrote 4 rows", written == 4);
    UMS_CHECK("mirror pass flushed open sync batch",
              !ndb.sync_in_batch && !ndb.tx_open);
    UMS_CHECK("mirror count == 4 after rebuild", db_utxo_count(&ndb) == 4);
    UMS_CHECK("mirror max height == 104", db_utxo_max_height(&ndb) == 104);

    /* The post-freeze coin's address balance is now present (the whole point —
     * a frozen mirror would report 0 for tag=0x44). */
    {
        uint8_t script[25], hash20[20]; ums_p2pkh(script, hash20, 0x44);
        int64_t bal = db_utxo_balance_for_address(&ndb, hash20);
        UMS_CHECK("post-freeze address balance == 3333", bal == 3333);
        UMS_CHECK("post-freeze script_type == P2PKH",
                  utxo_classify_script(script, sizeof(script), hash20, &(bool){0})
                  == SCRIPT_P2PKH);
    }

    /* Durable cursor advanced to the frontier. */
    {
        int64_t cur = -1;
        bool got = node_db_state_get_int(&ndb, UTXO_MIRROR_SYNC_CURSOR_KEY, &cur);
        UMS_CHECK("cursor persisted", got);
        UMS_CHECK("cursor == frontier (105)", cur == 105);
    }

    /* Pass 2: no drift (cursor == frontier, counts equal) → no work. */
    int64_t written2 = utxo_mirror_sync_run_once(&svc);
    UMS_CHECK("pass2 no-op (0 rows)", written2 == 0);
    UMS_CHECK("mirror count still 4", db_utxo_count(&ndb) == 4);

    /* ── Spend one coin in coins_kv + advance the frontier. The next pass must
     * REMOVE it from the mirror (count back to 3) — proves spends propagate. */
    {
        uint8_t txid[32]; ums_txid(txid, 0x33);
        UMS_CHECK("spend coin 0x33", coins_kv_spend(pdb, txid, 0));
        UMS_CHECK("coins_kv count == 3", coins_kv_count(pdb) == 3);
        UMS_CHECK("advance frontier 106", ums_set_frontier(pdb, 106));
    }
    int64_t written3 = utxo_mirror_sync_run_once(&svc);
    UMS_CHECK("pass3 rebuilt 3 rows", written3 == 3);
    UMS_CHECK("mirror count == 3 after spend", db_utxo_count(&ndb) == 3);
    UMS_CHECK("spent coin gone from mirror",
              !db_utxo_exists(&ndb,
                  (const uint8_t[32]){[0]=0x33,[31]=0x9c}, 0));

    /* The feeder never wrote coins_kv: the authority count is unchanged by the
     * mirror passes (consensus path untouched). */
    UMS_CHECK("coins_kv count untouched by feeder (== 3)",
              coins_kv_count(pdb) == 3);

    app_runtime_set_current(NULL);
    db_service_stop(&dbsvc);
    progress_store_close();
    node_db_close(&ndb);
    test_cleanup_tmpdir(dir);
    return failures;
}
