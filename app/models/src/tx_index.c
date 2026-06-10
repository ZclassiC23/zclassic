/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ActiveRecord model: Transaction (tx_index)
 *
 * validates :txid, :block_hash, presence: true
 * validates :block_height, :tx_index, :file_num, :file_pos, numericality: { >= 0 }
 *
 * belongs_to :block */

#include "models/tx_index.h"
#include "models/block.h"
#include "util/log_macros.h"
#include <string.h>

/* ── Callbacks ─────────────────────────────────────────────────── */

DEFINE_MODEL_CALLBACKS(tx)

/* before_save: validate txid non-null, height/file_number non-negative */
static bool tx_index_before_save(void *record, void *ctx)
{
    (void)ctx;
    const struct db_tx_index *t = record;
    static const uint8_t zero[32] = {0};
    if (memcmp(t->txid, zero, 32) == 0) {
        LOG_FAIL("tx_index", "before_save REJECTED: null txid");
    }
    if (t->block_height < 0) {
        LOG_FAIL("tx_index", "before_save REJECTED: negative height %d",
                 t->block_height);
    }
    if (t->file_num < 0) {
        LOG_FAIL("tx_index", "before_save REJECTED: negative file_num %d",
                 t->file_num);
    }
    return true;
}

static void tx_index_init_hooks(void)
{
    static bool done = false;
    if (done) return;
    struct ar_callbacks *cbs = db_tx_callbacks();
    ar_register_before_save(cbs, tx_index_before_save);
    done = true;
}

/* ── Validation ────────────────────────────────────────────────── */

bool db_tx_validate(const struct db_tx_index *t, struct ar_errors *errors)
{
    ar_errors_clear(errors);
    validates_presence_of(errors, t, txid);
    validates_presence_of(errors, t, block_hash);
    validates_non_negative(errors, t, block_height);
    validates_non_negative(errors, t, tx_index);
    validates_non_negative(errors, t, file_num);
    validates_non_negative(errors, t, file_pos);
    return !ar_errors_any(errors);
}

/* ── Save ──────────────────────────────────────────────────────── */

bool db_tx_save(struct node_db *ndb, const struct db_tx_index *t)
{
    if (!ndb->open) return false;

    tx_index_init_hooks();
    struct ar_callbacks *cbs = db_tx_callbacks();
    AR_BEGIN_SAVE(cbs, "tx_index", t, db_tx_validate);

    sqlite3_stmt *s = ndb->stmt_tx_insert;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, t->txid, 32);
    AR_BIND_BLOB(s, 2, t->block_hash, 32);
    AR_BIND_INT(s, 3, t->block_height);
    AR_BIND_INT(s, 4, t->tx_index);
    AR_BIND_INT(s, 5, t->file_num);
    AR_BIND_INT(s, 6, t->file_pos);
    AR_BIND_INT(s, 7, t->is_coinbase ? 1 : 0);

    bool ok = AR_STEP_DONE(s);
    AR_FINISH_SAVE(cbs, t, ok);
}

/* ── Find ──────────────────────────────────────────────────────── */

bool db_tx_find(struct node_db *ndb, const uint8_t txid[32],
                struct db_tx_index *out)
{
    if (!ndb->open) return false;
    sqlite3_stmt *s = ndb->stmt_tx_find;
    sqlite3_reset(s);
    AR_BIND_BLOB(s, 1, txid, 32);
    AR_FIND_ONE_CACHED(s, out,
        memcpy(out->txid, txid, 32);
        AR_READ_BLOB(s, 0, out->block_hash, 32);
        out->block_height = (int)AR_COL_INT(s, 1);
        out->tx_index = (int)AR_COL_INT(s, 2);
        out->file_num = (int)AR_COL_INT(s, 3);
        out->file_pos = (int)AR_COL_INT(s, 4);
        out->is_coinbase = AR_COL_INT(s, 5) != 0);
}

bool db_tx_find_native_or_reversed(struct node_db *ndb,
                                   const uint8_t txid[32],
                                   struct db_tx_index *out,
                                   bool *used_reversed)
{
    if (used_reversed)
        *used_reversed = false;
    if (!ndb || !txid || !out)
        return false;

    if (db_tx_find(ndb, txid, out))
        return true;

    uint8_t reversed[32];
    for (size_t i = 0; i < sizeof(reversed); i++)
        reversed[i] = txid[sizeof(reversed) - 1 - i];

    if (!db_tx_find(ndb, reversed, out))
        return false;

    if (used_reversed)
        *used_reversed = true;
    return true;
}

/* ── Delete ────────────────────────────────────────────────────── */

bool db_tx_delete(struct node_db *ndb, const uint8_t txid[32])
{
    if (!ndb->open) return false;

    struct ar_callbacks *cbs = db_tx_callbacks();
    struct db_tx_index t;
    memcpy(t.txid, txid, 32);
    sqlite3_stmt *s = NULL;
    AR_ADHOC_DESTROY(ndb, s, "DELETE FROM transactions WHERE txid=?",
        cbs, &t, AR_BIND_BLOB(s, 1, txid, 32));
}

/* ── Queries ───────────────────────────────────────────────────── */

int db_tx_count(struct node_db *ndb)
{
    if (!ndb->open) return 0;
    AR_QUERY_COUNT_SQL(ndb, "SELECT COUNT(*) FROM transactions");
}

bool db_tx_delete_all(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return false;
    return node_db_exec(ndb, "DELETE FROM transactions");
}

bool db_tx_prepare_bulk_load(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return false;
    sqlite3_exec(ndb->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA cache_size=-524288", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=0", NULL, NULL, NULL);
    sqlite3_busy_timeout(ndb->db, 30000);
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_tx_block",
                 NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "DROP INDEX IF EXISTS idx_tx_height",
                 NULL, NULL, NULL);
    return db_tx_delete_all(ndb);
}

bool db_tx_finalize_bulk_load(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        return false;
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_tx_block ON transactions(block_hash)",
        NULL, NULL, NULL);
    sqlite3_exec(ndb->db,
        "CREATE INDEX IF NOT EXISTS idx_tx_height ON transactions(block_height)",
        NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_exec(ndb->db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);
    sqlite3_wal_checkpoint_v2(ndb->db, NULL,
        SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    return true;
}

bool db_tx_configure_additive_build(struct node_db *ndb)
{
    if (!ndb || !ndb->open)
        LOG_FAIL("tx_index", "additive build requested without an open database");

    sqlite3_busy_timeout(ndb->db, 30000);
    if (!node_db_exec(ndb, "PRAGMA synchronous=NORMAL"))
        LOG_FAIL("tx_index", "failed to set synchronous=NORMAL for additive build");
    if (!node_db_exec(ndb, "PRAGMA wal_autocheckpoint=0"))
        LOG_FAIL("tx_index", "failed to disable WAL autocheckpoint for additive build");
    return true;
}

bool db_tx_save_batch(struct node_db *ndb, const struct db_tx_index *txs,
                      size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!db_tx_save(ndb, &txs[i]))
            return false;
    }
    return true;
}

int db_tx_find_by_block(struct node_db *ndb, const uint8_t block_hash[32],
                        struct db_tx_index *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(ndb->db,
        "SELECT txid,block_height,tx_index,file_num,file_pos,is_coinbase"
        " FROM transactions WHERE block_hash=? ORDER BY tx_index",
        -1, &s, NULL);
    AR_BIND_BLOB(s, 1, block_hash, 32);
    int count = 0;
    while (AR_STEP_ROW(s) && (size_t)count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        AR_READ_BLOB(s, 0, out[count].txid, 32);
        memcpy(out[count].block_hash, block_hash, 32);
        out[count].block_height = (int)AR_COL_INT(s, 1);
        out[count].tx_index = (int)AR_COL_INT(s, 2);
        out[count].file_num = (int)AR_COL_INT(s, 3);
        out[count].file_pos = (int)AR_COL_INT(s, 4);
        out[count].is_coinbase = AR_COL_INT(s, 5) != 0;
        count++;
    }
    AR_FINALIZE(s);
    return count;
}

bool db_tx_output_value(struct node_db *ndb, const uint8_t txid[32],
                        uint32_t vout, int64_t *out_value)
{
    if (!ndb || !ndb->open || !txid || !out_value)
        return false;
    sqlite3_stmt *s = NULL;
    AR_QUERY_ONE_BOOL(ndb, s,
        "SELECT value FROM tx_outputs WHERE txid=? AND vout=?",
        (AR_BIND_BLOB(s, 1, txid, 32), AR_BIND_INT(s, 2, (int)vout)),
        (*out_value = AR_COL_INT(s, 0)));
}

/* ── Relationships ─────────────────────────────────────────────── */

/* belongs_to :block */
bool db_tx_block(struct node_db *ndb, const struct db_tx_index *t,
                 struct db_block *out)
{
    return db_block_find_by_hash(ndb, t->block_hash, out);
}
