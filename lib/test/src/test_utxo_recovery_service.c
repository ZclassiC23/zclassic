/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for utxo_recovery_service — boot-time UTXO wipe, import,
 * restore, and integrity operations.
 */

#include "test/test_helpers.h"
#include "services/utxo_recovery_service.h"
#include "services/recovery_policy.h"
#include "services/chain_state_service.h"
#include "storage/utxo_reimport_flag.h"
#include "storage/progress_store.h"
#include "validation/main_state.h"
#include "chain/chainparams.h"
#include "models/database.h"
#include "storage/coins_view_sqlite.h"
#include "util/ar_step_readonly.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#define URS_HEX32(byte) \
    #byte #byte #byte #byte #byte #byte #byte #byte \
    #byte #byte #byte #byte #byte #byte #byte #byte \
    #byte #byte #byte #byte #byte #byte #byte #byte \
    #byte #byte #byte #byte #byte #byte #byte #byte

#define URS_CHECK(name, expr) do {              \
    printf("%s... ", (name));                   \
    if ((expr)) printf("OK\n");                 \
    else { printf("FAIL\n"); failures++; }      \
} while (0)

/* Build a minimal chain in main_state */
static void urs_hash_for_height(int h, struct uint256 *out)
{
    memset(out, 0, sizeof(*out));
    out->data[0] = (uint8_t)(h & 0xFF);
    out->data[1] = (uint8_t)((h >> 8) & 0xFF);
    out->data[3] = 0xCC;  /* distinct from CSV tests */
}

static void urs_build_chain(struct main_state *ms, int n)
{
    struct uint256 hashes[256];
    int limit = n < 256 ? n : 256;

    for (int h = 0; h < limit; h++) {
        urs_hash_for_height(h, &hashes[h]);

        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hashes[h]);
        if (!pi) continue;

        pi->nHeight = h;
        pi->nBits = 0x1f07ffff;
        pi->nTime = 1000000 + (uint32_t)h * 150;
        pi->nVersion = 4;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nChainTx = (uint32_t)(h + 1);
        arith_uint256_set_u64(&pi->nChainWork, (uint64_t)(h + 1));

        if (h > 0) {
            struct block_index *prev = block_map_find(
                &ms->map_block_index, &hashes[h - 1]);
            if (prev) pi->pprev = prev;
        }
    }
    if (limit > 0) {
        struct block_index *tip = block_map_find(
            &ms->map_block_index, &hashes[limit - 1]);
        if (tip) active_chain_move_window_tip(&ms->chain_active, tip);
    }
}

static int64_t urs_count_sql(struct node_db *ndb, const char *sql)
{
    int64_t count = -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (AR_STEP_ROW_READONLY(st) == SQLITE_ROW)
            count = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return count;
}

static bool urs_exec_progress(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("progress SQL failed: %s\n", err ? err : "(no message)");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool urs_seed_finalized_floor(int height, const struct uint256 *hash)
{
    sqlite3 *db = progress_store_db();
    if (!db || !hash)
        return false;
    if (!urs_exec_progress(db,
            "CREATE TABLE IF NOT EXISTS tip_finalize_log ("
            "height INTEGER PRIMARY KEY, status TEXT, ok INTEGER NOT NULL,"
            "tip_hash BLOB)"))
        return false;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO tip_finalize_log"
            "(height,status,ok,tip_hash) "
            "VALUES(?,'finalized',1,?)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, height);
    sqlite3_bind_blob(st, 2, hash->data, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

int test_utxo_recovery_service(void)
{
    printf("\n=== utxo recovery service tests ===\n");
    int failures = 0;

    /* ── 1. Policy-gated wipe: allowed (small UTXO set) ── */

    {
        /* Create temp SQLite DB with a few UTXOs */
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_wipe.db", getpid());
        mkdir("./test-tmp", 0755);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Insert 5 fake UTXOs */
            for (int i = 0; i < 5; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, i + 1);
                node_db_exec(&ndb, sql);
            }

            int64_t before = node_db_utxo_count(&ndb);

            /* A prior import's durable seed anchor — the wipe destroys the
             * coins, so it MUST also clear the anchor that attests to them. */
            uint8_t seed_hash[32];
            memset(seed_hash, 0xCD, sizeof(seed_hash));
            node_db_state_set_int(&ndb, "cold_import_seed_anchor_height",
                                  3100000);
            node_db_state_set(&ndb, "cold_import_seed_anchor_hash",
                              seed_hash, sizeof(seed_hash));
            node_db_state_set_int(&ndb, "cold_import_seed_anchor_utxo_count",
                                  before);

            /* Set env to allow wipe of 10 rows */
            setenv("ZCL_MAX_UTXO_WIPE_ROWS", "10", 1);
            bool ok = utxo_recovery_wipe(&ndb, "test.small_wipe").ok;
            int64_t after = node_db_utxo_count(&ndb);
            unsetenv("ZCL_MAX_UTXO_WIPE_ROWS");

            int64_t seed_h = -1;
            bool have_h = node_db_state_get_int(
                &ndb, "cold_import_seed_anchor_height", &seed_h);
            int64_t seed_c = -1;
            bool have_c = node_db_state_get_int(
                &ndb, "cold_import_seed_anchor_utxo_count", &seed_c);

            URS_CHECK("urs: policy allows small wipe",
                      ok && before == 5 && after == 0);
            URS_CHECK("urs: utxo wipe clears cold-import seed anchor "
                      "(key cannot outlive the coins it attests to)",
                      !have_h && !have_c);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: policy allows small wipe (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 2. Policy-gated wipe: refused (too many rows) ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_refuse.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Insert 50 fake UTXOs */
            node_db_begin(&ndb);
            for (int i = 0; i < 50; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, i + 1);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            /* Set env to allow only 10 rows — should refuse 50 */
            setenv("ZCL_MAX_UTXO_WIPE_ROWS", "10", 1);
            bool ok = utxo_recovery_wipe(&ndb, "test.large_wipe").ok;
            int64_t after = node_db_utxo_count(&ndb);
            unsetenv("ZCL_MAX_UTXO_WIPE_ROWS");

            URS_CHECK("urs: policy refuses large wipe",
                      !ok && after == 50);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: policy refuses large wipe (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 3. Reimport flag detection ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_reimport", getpid());
        mkdir(tmpdir, 0755);

        /* Write needs_reimport flag */
        char flag_path[512];
        snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", tmpdir);
        FILE *f = fopen(flag_path, "w");
        if (f) { fputs("1", f); fclose(f); }

        bool found = utxo_reimport_flag_check_and_clear(tmpdir);
        /* File should be removed after check */
        struct stat st;
        bool file_gone = (stat(flag_path, &st) != 0);

        URS_CHECK("urs: reimport flag detected and removed",
                  found && file_gone);

        rmdir(tmpdir);
    }

    /* ── 4. Reimport flag absent → returns false ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_no_reimport", getpid());
        mkdir(tmpdir, 0755);

        bool found = utxo_reimport_flag_check_and_clear(tmpdir);
        URS_CHECK("urs: no reimport flag → false", !found);

        rmdir(tmpdir);
    }

    /* ── 5. Prepare reimport: wipe + clear migration flag ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_prep.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Set migration flag */
            uint8_t one = 1;
            node_db_state_set(&ndb, "leveldb_utxo_migrated", &one, 1);

            /* Set the durable cold-import seed anchor (height/hash/count) as a
             * prior accepted import would. prepare_reimport MUST clear all
             * three so a later partial reimport cannot read a stale key and
             * trust-stamp a finalized frontier above the real coin frontier. */
            uint8_t seed_hash[32];
            memset(seed_hash, 0xAB, sizeof(seed_hash));
            node_db_state_set_int(&ndb, "cold_import_seed_anchor_height",
                                  3100000);
            node_db_state_set(&ndb, "cold_import_seed_anchor_hash",
                              seed_hash, sizeof(seed_hash));
            node_db_state_set_int(&ndb, "cold_import_seed_anchor_utxo_count",
                                  1300000);

            /* Insert 3 UTXOs */
            for (int i = 0; i < 3; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, 1, "
                    "100000, X'00')", i, i);
                node_db_exec(&ndb, sql);
            }

            bool ok = utxo_recovery_prepare_reimport(&ndb).ok;

            /* prepare_reimport no longer wipes UTXOs (the wipe happens
             * at the start of import_ldb instead).  It only clears the
             * migration flag so import_ldb will re-run. */
            int64_t utxos = node_db_utxo_count(&ndb);
            uint8_t buf[8];
            size_t len = 0;
            bool flag = node_db_state_get(&ndb, "leveldb_utxo_migrated",
                                           buf, sizeof(buf), &len);

            /* The three durable seed keys must ALL be gone. */
            int64_t seed_h = -1;
            bool have_h = node_db_state_get_int(
                &ndb, "cold_import_seed_anchor_height", &seed_h);
            uint8_t hbuf[32];
            size_t hlen = 0;
            bool have_hash = node_db_state_get(
                &ndb, "cold_import_seed_anchor_hash",
                hbuf, sizeof(hbuf), &hlen);
            int64_t seed_c = -1;
            bool have_c = node_db_state_get_int(
                &ndb, "cold_import_seed_anchor_utxo_count", &seed_c);

            URS_CHECK("urs: prepare reimport: clear flag, keep UTXOs",
                      ok && utxos == 3 && !flag);
            URS_CHECK("urs: prepare reimport clears cold-import seed anchor "
                      "(no stale key strands across a reimport)",
                      !have_h && !have_hash && !have_c);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: prepare reimport (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 6. Clean above tip: removes only stragglers ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_above.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 50);  /* tip at h=49 */

        if (node_db_open(&ndb, db_path)) {
            /* Insert UTXOs: 10 below tip, 5 at tip+1 */
            node_db_begin(&ndb);
            for (int i = 0; i < 15; i++) {
                int h = (i < 10) ? (i + 1) : 50;
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, h);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            int64_t before = node_db_utxo_count(&ndb);
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t after = node_db_utxo_count(&ndb);

            URS_CHECK("urs: clean above tip removes 5 tip+1 stragglers",
                      before == 15 && cleaned == 5 && after == 10);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 7. Clean above tip: refuses when >1000 ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_refuse_above.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 10);  /* tip at h=9 */

        if (node_db_open(&ndb, db_path)) {
            /* Insert 1500 UTXOs all above tip (h=10..1509) */
            node_db_begin(&ndb);
            for (int i = 0; i < 1500; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, "
                    "script) VALUES(X'%032d', %d, %d, "
                    "100000, X'00')", i, i, 10 + i);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            int64_t before = node_db_utxo_count(&ndb);
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t after = node_db_utxo_count(&ndb);

            URS_CHECK("urs: clean above tip refuses >1000",
                      before == 1500 && cleaned == 0 && after == 1500);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip refuses (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 8. Clean above tip: stale coinbase at tip+1 is rewound ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_stale_coinbase.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 50);  /* tip at h=49 */

        if (node_db_open(&ndb, db_path)) {
            node_db_begin(&ndb);
            node_db_exec(&ndb,
                "INSERT INTO utxos(txid, vout, height, value, script, "
                "is_coinbase) VALUES(X'" URS_HEX32(AB) "', 0, 50, "
                "100000, X'51', 1)");
            node_db_exec(&ndb,
                "INSERT OR REPLACE INTO node_state(key,value) "
                "VALUES('utxo_commitment', X'" URS_HEX32(CD) "')");
            node_db_commit(&ndb);

            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t stale = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM utxos WHERE txid=X'" URS_HEX32(AB) "'");
            int64_t commitment = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM node_state WHERE key='utxo_commitment'");

            URS_CHECK("urs: stale coinbase at tip+1 is rewound on boot",
                      cleaned == 1 && stale == 0 && commitment == 0);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: stale coinbase at tip+1 (db open failed)",
                      false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 9. Clean above tip: refuses >32 single-block rows ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_refuse_33.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        urs_build_chain(&ms, 50);  /* tip at h=49 */

        if (node_db_open(&ndb, db_path)) {
            node_db_begin(&ndb);
            for (int i = 0; i < 33; i++) {
                char sql[256];
                snprintf(sql, sizeof(sql),
                    "INSERT INTO utxos(txid, vout, height, value, script, "
                    "is_coinbase) VALUES(randomblob(32), %d, 50, "
                    "100000, X'51', 1)", i);
                node_db_exec(&ndb, sql);
            }
            node_db_commit(&ndb);

            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            int64_t above = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM utxos WHERE height > 49");

            URS_CHECK("urs: clean above tip refuses >32 rows",
                      cleaned == 0 && above == 33);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip refuses >32 (db open failed)",
                      false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 10. Reimport flag with "0" value → no reimport ── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_urs_reimport0", getpid());
        mkdir(tmpdir, 0755);

        char flag_path[512];
        snprintf(flag_path, sizeof(flag_path), "%s/needs_reimport", tmpdir);
        FILE *f = fopen(flag_path, "w");
        if (f) { fputs("0", f); fclose(f); }

        bool found = utxo_reimport_flag_check_and_clear(tmpdir);
        URS_CHECK("urs: reimport flag '0' → no reimport", !found);

        /* File should still be removed */
        unlink(flag_path);
        rmdir(tmpdir);
    }

    /* ── 11. LDB import: already migrated → no-op ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_ldb_noop.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            /* Mark as already migrated */
            uint8_t one = 1;
            node_db_state_set(&ndb, "leveldb_utxo_migrated", &one, 1);

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);

            struct coins_view_cache cache;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&cache, &nv);

            struct utxo_recovery_ctx uctx = {
                .state = &ms,
                .coins_sqlite = NULL,
                .coins_tip = &cache,
                .ndb = &ndb,
                .datadir = "/nonexistent",
                .params = NULL,
                .activation_ctl = NULL,
                .db_service = NULL,
            };

            struct utxo_import_result ir = utxo_recovery_import_ldb(&uctx);

            URS_CHECK("urs: LDB import skips when already migrated",
                      ir.status.ok && !ir.imported && !ir.skip_activate);

            coins_view_cache_free(&cache);
            block_map_free(&ms.map_block_index);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: LDB import skip (db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 12. Restore with no UTXOs publishes genesis through CSR ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_restore_genesis.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            chain_params_select(CHAIN_MAIN);
            const struct chain_params *params = chain_params_get();

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);

            struct block_index *genesis = chainstate_insert_block_index(
                (struct chainstate *)&ms,
                &params->consensus.hashGenesisBlock);
            if (genesis) {
                genesis->nHeight = 0;
                genesis->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
                genesis->nTx = 1;
                genesis->nChainTx = 1;
            }

            struct coins_view_cache cache;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&cache, &nv);

            struct uint256 unknown_best;
            memset(&unknown_best, 0x42, sizeof(unknown_best));
            coins_view_cache_set_best_block(&cache, &unknown_best);

            struct chain_state_repository *csr = csr_instance();
            csr_init(csr, &ms.map_block_index, &ms.chain_active,
                     &ms.pindex_best_header, &cache, &ndb, NULL);

            struct utxo_recovery_ctx uctx = {
                .state = &ms,
                .coins_sqlite = NULL,
                .coins_tip = &cache,
                .ndb = &ndb,
                .datadir = "/nonexistent",
                .params = params,
                .activation_ctl = NULL,
                .db_service = NULL,
            };

            struct chain_restore_result rr =
                utxo_recovery_restore_chain_tip(&uctx, NULL);

            struct uint256 got_best;
            memset(&got_best, 0, sizeof(got_best));
            coins_view_cache_get_best_block(&cache, &got_best);

            uint8_t persisted[32];
            size_t persisted_len = 0;
            bool got_persisted = node_db_state_get(
                &ndb, "coins_best_block", persisted, sizeof(persisted),
                &persisted_len);

            URS_CHECK("urs: no-UTXO restore commits genesis through CSR",
                      rr.status.ok && rr.restored &&
                      active_chain_tip(&ms.chain_active) == genesis &&
                      uint256_eq(&got_best,
                                  &params->consensus.hashGenesisBlock) &&
                      got_persisted && persisted_len == 32 &&
                      memcmp(persisted,
                             params->consensus.hashGenesisBlock.data,
                             32) == 0);

            csr_free(csr);
            coins_view_cache_free(&cache);
            active_chain_free(&ms.chain_active);
            block_map_free(&ms.map_block_index);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: no-UTXO restore commits genesis through CSR "
                      "(db open failed)", false);
        }
        unlink(db_path);
    }

    /* ── 12b. scan_fallback cannot roll below finalized reducer floor ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_scan_floor.db", getpid());
        char progress_dir[256];
        test_make_tmpdir(progress_dir, sizeof(progress_dir),
                         "urs_scan_floor", "progress");

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool pdb_open = progress_store_open(progress_dir);
        if (node_db_open(&ndb, db_path) && pdb_open) {
            chain_params_select(CHAIN_MAIN);
            const struct chain_params *params = chain_params_get();

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);
            urs_build_chain(&ms, 121);  /* active tip h=120 */

            struct uint256 scan_hash;
            urs_hash_for_height(80, &scan_hash);
            struct block_index *scan_fallback =
                block_map_find(&ms.map_block_index, &scan_hash);

            struct coins_view_cache cache;
            struct coins_view nv;
            memset(&nv, 0, sizeof(nv));
            coins_view_cache_init(&cache, &nv);

            struct chain_state_repository *csr = csr_instance();
            csr_init(csr, &ms.map_block_index, &ms.chain_active,
                     &ms.pindex_best_header, &cache, &ndb, NULL);

            struct utxo_recovery_ctx uctx = {
                .state = &ms,
                .coins_sqlite = NULL,
                .coins_tip = &cache,
                .ndb = &ndb,
                .datadir = "/nonexistent",
                .params = params,
                .activation_ctl = NULL,
                .db_service = NULL,
            };

            struct uint256 floor_hash;
            urs_hash_for_height(120, &floor_hash);
            bool seeded = urs_seed_finalized_floor(120, &floor_hash);
            struct chain_restore_result rr =
                utxo_recovery_restore_chain_tip(&uctx, scan_fallback);

            struct uint256 got_best;
            coins_view_cache_get_best_block(&cache, &got_best);

            URS_CHECK("urs: scan_fallback refuses finalized-floor rollback",
                      seeded && rr.status.ok && rr.restored &&
                      rr.restored_height == 120 &&
                      uint256_eq(&rr.restored_hash, &floor_hash) &&
                      rr.skip_activate &&
                      active_chain_height(&ms.chain_active) == 120 &&
                      uint256_is_null(&got_best));

            csr_free(csr);
            coins_view_cache_free(&cache);
            active_chain_free(&ms.chain_active);
            block_map_free(&ms.map_block_index);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: scan_fallback finalized-floor fixture", false);
            if (ndb.open)
                node_db_close(&ndb);
        }
        if (pdb_open)
            progress_store_close();
        test_cleanup_tmpdir(progress_dir);
        unlink(db_path);
    }

    /* ── 12c. Recovery entry points return rich status on invalid context ── */

    {
        struct utxo_import_result ir = utxo_recovery_import_ldb(NULL);
        URS_CHECK("urs: import null ctx returns zcl_result error",
                  !ir.status.ok && ir.status.code == -1);

        struct chain_restore_result rr =
            utxo_recovery_restore_chain_tip(NULL, NULL);
        URS_CHECK("urs: restore null ctx returns zcl_result error",
                  !rr.status.ok && rr.status.code == -20 &&
                  rr.restored_height == -1);

        struct boot_validation_result vr;
        memset(&vr, 0, sizeof(vr));
        vr.action = BOOT_OK;
        struct recovery_exec_result er = utxo_recovery_execute(NULL, &vr);
        URS_CHECK("urs: execute null ctx returns zcl_result error",
                  !er.status.ok && er.status.code == -30 &&
                  !er.recovered && !er.skip_activate);
    }

    /* ── 13. Clean above tip: no-op when tip=0 ── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        /* no chain built — tip is NULL */

        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_notip.db", getpid());
        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            int cleaned = utxo_recovery_clean_above_tip(&ndb, &ms);
            URS_CHECK("urs: clean above tip no-op when tip=0",
                      cleaned == 0);
            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: clean above tip no-op (db open failed)", false);
        }
        unlink(db_path);
        block_map_free(&ms.map_block_index);
    }

    /* ── 14. Stale coins cursor can advance to sync projection tip ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_stale_cursor.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            node_db_begin(&ndb);
            node_db_exec(&ndb,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "file_num,data_pos,num_tx) VALUES("
                "X'" URS_HEX32(11) "',100,X'" URS_HEX32(00) "',4,"
                "X'" URS_HEX32(22) "',1000,0,X'',X'',X'',13,0,1,1)");
            node_db_exec(&ndb,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "file_num,data_pos,num_tx) VALUES("
                "X'" URS_HEX32(33) "',105,X'" URS_HEX32(11) "',4,"
                "X'" URS_HEX32(44) "',2000,0,X'',X'',X'',13,0,2,1)");
            node_db_exec(&ndb,
                "INSERT INTO utxos(txid,vout,height,value,script,"
                "is_coinbase) VALUES(X'" URS_HEX32(AA) "',0,105,"
                "100000,X'51',1)");
            node_db_exec(&ndb,
                "INSERT INTO utxos(txid,vout,height,value,script,"
                "is_coinbase) VALUES(X'" URS_HEX32(BB) "',0,106,"
                "100000,X'51',1)");
            node_db_exec(&ndb,
                "INSERT INTO transactions(txid,block_hash,block_height,"
                "tx_index,file_num,file_pos,is_coinbase) VALUES("
                "X'" URS_HEX32(BB) "',X'" URS_HEX32(55) "',106,0,0,0,1)");
            node_db_exec(&ndb,
                "INSERT OR REPLACE INTO node_state(key,value) "
                "VALUES('utxo_commitment', X'" URS_HEX32(CC) "')");
            node_db_commit(&ndb);

            uint8_t coins_hash[32];
            memset(coins_hash, 0x11, sizeof(coins_hash));
            uint8_t sync_hash[32];
            memset(sync_hash, 0x33, sizeof(sync_hash));
            node_db_state_set(&ndb, "coins_best_block",
                              coins_hash, sizeof(coins_hash));
            node_db_state_set(&ndb, "sync_projection_tip_hash",
                              sync_hash, sizeof(sync_hash));
            node_db_state_set_int(&ndb, "sync_projection_tip_height", 105);

            bool repaired =
                utxo_recovery_repair_stale_cursor_from_sync_projection(&ndb);

            uint8_t got[32];
            size_t got_len = 0;
            bool got_best = node_db_state_get(&ndb, "coins_best_block",
                                              got, sizeof(got), &got_len);
            int64_t above = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM utxos WHERE height > 105");
            int64_t stale_tx = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM transactions WHERE block_height > 105");
            int64_t commitment = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM node_state WHERE key='utxo_commitment'");

            struct coins_view_sqlite cvs;
            bool opens = coins_view_sqlite_open(&cvs, ndb.db);
            if (opens)
                coins_view_sqlite_close(&cvs);

            URS_CHECK("urs: stale coins cursor repairs from sync projection",
                      repaired && got_best && got_len == 32 &&
                      memcmp(got, sync_hash, sizeof(sync_hash)) == 0 &&
                      above == 0 && stale_tx == 0 && commitment == 0 &&
                      opens);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: stale coins cursor repair (db open failed)",
                      false);
        }
        unlink(db_path);
    }

    /* ── 15. Stale cursor repair tolerates live UTXO height lag ── */

    {
        char db_path[256];
        snprintf(db_path, sizeof(db_path),
                 "./test-tmp/%d_urs_stale_cursor_lag.db", getpid());

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        if (node_db_open(&ndb, db_path)) {
            node_db_begin(&ndb);
            node_db_exec(&ndb,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "file_num,data_pos,num_tx) VALUES("
                "X'" URS_HEX32(21) "',100,X'" URS_HEX32(00) "',4,"
                "X'" URS_HEX32(22) "',1000,0,X'',X'',X'',13,0,1,1)");
            node_db_exec(&ndb,
                "INSERT INTO blocks(hash,height,prev_hash,version,"
                "merkle_root,time,bits,nonce,solution,chain_work,status,"
                "file_num,data_pos,num_tx) VALUES("
                "X'" URS_HEX32(43) "',105,X'" URS_HEX32(21) "',4,"
                "X'" URS_HEX32(44) "',2000,0,X'',X'',X'',13,0,2,1)");
            node_db_exec(&ndb,
                "INSERT INTO utxos(txid,vout,height,value,script,"
                "is_coinbase) VALUES(X'" URS_HEX32(DD) "',0,102,"
                "100000,X'51',1)");
            node_db_exec(&ndb,
                "INSERT OR REPLACE INTO node_state(key,value) "
                "VALUES('utxo_commitment', X'" URS_HEX32(EE) "')");
            node_db_commit(&ndb);

            uint8_t coins_hash[32];
            memset(coins_hash, 0x21, sizeof(coins_hash));
            uint8_t sync_hash[32];
            memset(sync_hash, 0x43, sizeof(sync_hash));
            node_db_state_set(&ndb, "coins_best_block",
                              coins_hash, sizeof(coins_hash));
            node_db_state_set(&ndb, "sync_projection_tip_hash",
                              sync_hash, sizeof(sync_hash));
            node_db_state_set_int(&ndb, "sync_projection_tip_height", 105);

            bool repaired =
                utxo_recovery_repair_stale_cursor_from_sync_projection(&ndb);
            uint8_t got[32];
            size_t got_len = 0;
            bool got_best = node_db_state_get(&ndb, "coins_best_block",
                                              got, sizeof(got), &got_len);
            int64_t commitment = urs_count_sql(&ndb,
                "SELECT COUNT(*) FROM node_state WHERE key='utxo_commitment'");

            struct coins_view_sqlite cvs;
            bool opens = coins_view_sqlite_open(&cvs, ndb.db);
            if (opens)
                coins_view_sqlite_close(&cvs);

            URS_CHECK("urs: stale cursor repair allows UTXO height lag",
                      repaired && got_best && got_len == 32 &&
                      memcmp(got, sync_hash, sizeof(sync_hash)) == 0 &&
                      commitment == 0 && opens);

            node_db_close(&ndb);
        } else {
            URS_CHECK("urs: stale cursor lag repair (db open failed)",
                      false);
        }
        unlink(db_path);
    }

    printf("--- utxo_recovery_service: %d failure(s) ---\n", failures);
    return failures;
}
