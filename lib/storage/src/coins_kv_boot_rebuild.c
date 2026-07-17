/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * coins_kv_boot_rebuild — one-shot idempotent migration that populates the
 * progress.kv coins set from the (already caught-up / snapshot-seeded) UTXO
 * projection on existing datadirs, so the read-flip onto coins_kv has data
 * (docs/work/tip-durability-collapse.md). Fresh-sync nodes build coins_kv
 * forward via utxo_apply and never need this.
 *
 * Raw sqlite carries // raw-sql-ok:progress-kv-kernel-store (kernel store).
 */
#include "storage/coins_kv.h"

#include "storage/progress_store.h"
#include "storage/utxo_projection.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>
#include <string.h>

#define COINS_KV_MIGRATION_KEY COINS_KV_MIGRATION_COMPLETE_KEY  /* single source: coins_kv.h */

static bool migration_done(sqlite3 *db)
{
    uint8_t v = 0; size_t n = 0; bool found = false;
    if (!progress_meta_get(db, COINS_KV_MIGRATION_KEY, &v, sizeof(v), &n, &found))
        return false;
    return found && n >= 1 && v == 1;
}

bool coins_kv_boot_rebuild_if_needed(sqlite3 *progress_db,
                                     struct utxo_projection *proj)
{
    /* Safe to call before progress_store is open (early boot read-view path) or
     * before the projection exists — no-op, retried later. */
    if (!progress_db || !proj)
        return true;
    if (!coins_kv_ensure_schema(progress_db) ||
        !progress_meta_table_ensure(progress_db))
        return false;

    if (migration_done(progress_db))
        return true;

    /* Already populated forward (fresh sync) — stamp done, never re-scan. */
    if (coins_kv_count(progress_db) > 0) {
        uint8_t one = 1;
        (void)progress_meta_set(progress_db, COINS_KV_MIGRATION_KEY, &one, 1);
        return true;
    }

    /* Projection not yet populated (e.g. pre-snapshot-seed). No-op WITHOUT
     * stamping so a later call (post-seed, same boot) does the copy. */
    if (utxo_projection_count(proj) == 0)
        return true;

    const char *ppath = utxo_projection_path(proj);
    if (!ppath || !ppath[0]) {
        LOG_WARN("coins_kv", "[coins_kv] boot rebuild: no projection path");
        return false;
    }

    /* ATTACH the projection and bulk-copy its live set into coins_kv. ATTACH /
     * DETACH run OUTSIDE a txn; the INSERT...SELECT + done-flag run inside ONE
     * BEGIN IMMEDIATE so a crash mid-copy rolls back cleanly (next boot retries
     * — coins_kv stays empty, flag unset). */
    sqlite3_stmt *att = NULL;
    if (sqlite3_prepare_v2(progress_db, "ATTACH DATABASE ? AS coinssrc",
                           -1, &att, NULL) != SQLITE_OK) {
        LOG_WARN("coins_kv", "[coins_kv] boot rebuild ATTACH prepare failed: %s",
                 sqlite3_errmsg(progress_db));
        return false;
    }
    sqlite3_bind_text(att, 1, ppath, -1, SQLITE_TRANSIENT);
    int arc = sqlite3_step(att);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(att);
    if (arc != SQLITE_DONE) {
        LOG_WARN("coins_kv", "[coins_kv] boot rebuild ATTACH failed rc=%d", arc);
        return false;
    }

    bool ok = true;
    char *err = NULL;
    if (sqlite3_exec(progress_db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (ok && sqlite3_exec(progress_db,
            "INSERT OR REPLACE INTO coins"
            "(txid,vout,value,height,is_coinbase,script) "
            "SELECT txid,vout,value,height,is_coinbase,script FROM coinssrc.utxo",
            NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (ok) {
        uint8_t one = 1;
        if (!progress_meta_set_in_tx(progress_db, COINS_KV_MIGRATION_KEY, &one, 1))
            ok = false;
    }
    if (ok && sqlite3_exec(progress_db, "COMMIT", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (!ok) {
        LOG_WARN("coins_kv", "[coins_kv] boot rebuild copy failed: %s",
                 err ? err : "(no message)");
        sqlite3_exec(progress_db, "ROLLBACK", NULL, NULL, NULL);
    }
    if (err) sqlite3_free(err);
    sqlite3_exec(progress_db, "DETACH DATABASE coinssrc", NULL, NULL, NULL);

    if (ok)
        LOG_INFO("coins_kv",
                 "[coins_kv] boot rebuild migrated %lld UTXOs from projection",
                 (long long)coins_kv_count(progress_db));
    return ok;
}

/* Seed coins_kv directly from node.db's `utxos` table after a cold
 * LevelDB import. The generic boot rebuild above copies from the utxo
 * PROJECTION, which is still EMPTY on the import boot (it catches up
 * from the event log later) — leaving coins_kv empty, which strands
 * script_validate's prevout resolver for every pre-anchor coin.
 * The symptom is prevout_unresolved at the first post-anchor block
 * with a spend: the anchor candidate is rejected and the tip pins
 * at the import anchor. Same ATTACH-copy + done-stamp shape;
 * idempotent via the same migration key.
 *
 * DELIBERATELY HEIGHT-AGNOSTIC — it copies whatever UTXO set the source node.db
 * holds and does NOT re-check the compiled SHA3 UTXO checkpoint here. It is
 * called at the checkpoint height (boot_refold_from_anchor_reset, the anchor
 * reseed) AND at the TIP (reindex_epilogue after a genesis..tip replay,
 * utxo_recovery_restore after a cold import), where the checkpoint's cp->height
 * commitment legitimately does not apply. The checkpoint content re-check lives
 * at the checkpoint-positioned install sites: the -refold-from-anchor hard-assert
 * (which calls coins_kv_verify_against_checkpoint on this seed's output) and the
 * bundle installer's activate_verify_destination. Operators can re-derive the
 * commitment against the baked checkpoint on demand with the -verify-rom verb. */
bool coins_kv_seed_from_node_db(sqlite3 *progress_db,
                                const char *node_db_path)
{
    if (!progress_db || !node_db_path || !node_db_path[0])
        return false;
    if (!coins_kv_ensure_schema(progress_db) ||
        !progress_meta_table_ensure(progress_db))
        return false;
    if (migration_done(progress_db))
        return true;
    if (coins_kv_count(progress_db) > 0) {
        uint8_t one = 1;
        (void)progress_meta_set(progress_db, COINS_KV_MIGRATION_KEY, &one, 1);
        return true;
    }

    sqlite3_stmt *att = NULL;
    if (sqlite3_prepare_v2(progress_db, "ATTACH DATABASE ? AS coinssrc",
                           -1, &att, NULL) != SQLITE_OK) {
        LOG_WARN("coins_kv", "[coins_kv] import seed ATTACH prepare failed: %s",
                 sqlite3_errmsg(progress_db));
        return false;
    }
    sqlite3_bind_text(att, 1, node_db_path, -1, SQLITE_TRANSIENT);
    int arc = sqlite3_step(att);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(att);
    if (arc != SQLITE_DONE) {
        LOG_WARN("coins_kv", "[coins_kv] import seed ATTACH failed rc=%d", arc);
        return false;
    }

    bool ok = true;
    char *err = NULL;
    if (sqlite3_exec(progress_db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (ok && sqlite3_exec(progress_db,
            "INSERT OR REPLACE INTO coins"
            "(txid,vout,value,height,is_coinbase,script) "
            "SELECT txid,vout,value,height,is_coinbase,script "
            "FROM coinssrc.utxos",
            NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (ok) {
        uint8_t one = 1;
        if (!progress_meta_set_in_tx(progress_db, COINS_KV_MIGRATION_KEY, &one, 1))
            ok = false;
    }
    if (ok && sqlite3_exec(progress_db, "COMMIT", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (!ok) {
        LOG_WARN("coins_kv", "[coins_kv] import seed copy failed: %s",
                 err ? err : "(no message)");
        sqlite3_exec(progress_db, "ROLLBACK", NULL, NULL, NULL);
    }
    if (err) sqlite3_free(err);
    sqlite3_exec(progress_db, "DETACH DATABASE coinssrc", NULL, NULL, NULL);

    if (ok)
        LOG_INFO("coins_kv",
                 "[coins_kv] import seed migrated %lld UTXOs from node.db",
                 (long long)coins_kv_count(progress_db));
    return ok;
}
