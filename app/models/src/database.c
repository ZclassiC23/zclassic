/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * ar-validate-skip:connection-handle-not-a-row
 *   struct node_db wraps the SQLite connection plus a registry of
 *   cached prepared statements. It is not a row record, so the
 *   validates_* / AR_BEGIN_SAVE lifecycle does not apply. Row-level
 *   validation lives on the models that *use* this handle.
 *
 *   models/database_validators.{c,h} holds the shared field validators
 *   (range checks, address syntax, …) that row models invoke from
 *   their own validates_* paths. */

#include "platform/time_compat.h"
#include "util/log_macros.h"
#include "models/database.h"
#include "models/database_internal.h"
#include "models/database_validators.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t db_now_seconds(void)
{
    return (int64_t)platform_time_wall_time_t();
}

static int64_t db_now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void node_db_state_init(struct node_db *ndb)
{
    if (!ndb)
        return;
    zcl_mutex_init(&ndb->state_mutex);
    ndb->state_mutex_init = true;
    ndb->tx_open = false;
    ndb->turbo_mode = false;
    ndb->last_activity_time = db_now_seconds();
    ndb->last_sqlite_rc = SQLITE_OK;
    snprintf(ndb->last_op, sizeof(ndb->last_op), "%s", "open");
}

static void node_db_state_destroy(struct node_db *ndb)
{
    if (!ndb || !ndb->state_mutex_init)
        return;
    zcl_mutex_destroy(&ndb->state_mutex);
    ndb->state_mutex_init = false;
}

/* Execute `sql`, logging any error with `where` context.  Returns the
 * sqlite3 rc so callers can make tolerance decisions.  Wrapping
 * sqlite3_exec ensures the rc is never silently discarded — a dropped
 * rc hides schema-migration and turbo-mode failures. */
int db_exec_checked(sqlite3 *db, const char *sql, const char *where)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_WARN("db", "[db] %s failed: %s (sql=%s)", where, err ? err : "(no errmsg)", sql);
    }
    sqlite3_free(err);
    return rc;
}

/* Like db_exec_checked but tolerates a known-benign error substring
 * (e.g. "duplicate column name" when re-applying idempotent ALTERs).
 * Returns SQLITE_OK on tolerated failure so callers can treat it as
 * success; returns the original rc otherwise. */
int db_exec_tolerant(sqlite3 *db, const char *sql, const char *where,
                     const char *tolerable_substr)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (tolerable_substr && err && strstr(err, tolerable_substr)) {
            sqlite3_free(err);
            return SQLITE_OK;
        }
        LOG_WARN("db", "[db] %s failed: %s (sql=%s)", where, err ? err : "(no errmsg)", sql);
    }
    sqlite3_free(err);
    return rc;
}

/*
 * Stamp the activity/rc/op snapshot fields under state_mutex. When
 * flag_field is non-NULL it is assigned flag_value inside the same lock
 * acquisition so the mode change and the activity stamp stay atomic.
 */
static void node_db_stamp_activity(struct node_db *ndb, bool *flag_field,
                                   bool flag_value, const char *op, int rc)
{
    if (!ndb || !ndb->state_mutex_init)
        return;
    zcl_mutex_lock(&ndb->state_mutex);
    if (flag_field)
        *flag_field = flag_value;
    ndb->last_activity_time = db_now_seconds();
    ndb->last_sqlite_rc = rc;
    if (op && op[0]) {
        snprintf(ndb->last_op, sizeof(ndb->last_op), "%s", op);
    }
    zcl_mutex_unlock(&ndb->state_mutex);
}

void node_db_note_activity(struct node_db *ndb, const char *op, int rc)
{
    node_db_stamp_activity(ndb, NULL, false, op, rc);
}

static void node_db_note_tx_state(struct node_db *ndb, bool tx_open,
                                  const char *op, int rc)
{
    node_db_stamp_activity(ndb, ndb ? &ndb->tx_open : NULL, tx_open, op, rc);
}

void node_db_note_turbo_mode(struct node_db *ndb, bool turbo_mode,
                             const char *op, int rc)
{
    node_db_stamp_activity(ndb, ndb ? &ndb->turbo_mode : NULL, turbo_mode,
                           op, rc);
}


static bool prepare_statements(struct node_db *ndb)
{
    sqlite3 *db = ndb->db;
    int rc;

#define PREP(field, sql) do { \
    rc = sqlite3_prepare_v2(db, sql, -1, &ndb->field, NULL); \
    if (rc != SQLITE_OK) { \
        LOG_FAIL("db", "db: prepare %s: %s", #field, \
                sqlite3_errmsg(db)); \
    } \
} while (0)

    PREP(stmt_utxo_insert,
         "INSERT OR REPLACE INTO utxos"
         "(txid,vout,value,script,script_type,"
         "address_hash,height,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?,?)");

    PREP(stmt_snapshot_staging_insert,
         "INSERT OR REPLACE INTO snapshot_staging_utxos"
         "(txid,vout,value,script,script_type,"
         "address_hash,height,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?,?)");

    PREP(stmt_utxo_delete,
         "DELETE FROM utxos WHERE txid=? AND vout=?");

    PREP(stmt_utxo_find,
         "SELECT value,script,script_type,"
         "address_hash,height,is_coinbase"
         " FROM utxos WHERE txid=? AND vout=?");

    PREP(stmt_block_insert,
         "INSERT OR REPLACE INTO blocks"
         "(hash,height,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value)"
         " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    PREP(stmt_block_by_hash,
         "SELECT height,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value"
         " FROM blocks WHERE hash=?");

    PREP(stmt_block_by_height,
         "SELECT hash,prev_hash,version,merkle_root,"
         "time,bits,nonce,solution,chain_work,status,"
         "file_num,data_pos,undo_pos,num_tx,"
         "sapling_root,sprout_root,"
         "sapling_value,sprout_value"
         " FROM blocks WHERE height=?"
         " AND status>=3 LIMIT 1");

    PREP(stmt_tx_insert,
         "INSERT OR REPLACE INTO transactions"
         "(txid,block_hash,block_height,"
         "tx_index,file_num,file_pos,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?)");

    PREP(stmt_tx_find,
         "SELECT block_hash,block_height,tx_index,"
         "file_num,file_pos,is_coinbase"
         " FROM transactions WHERE txid=?");

    PREP(stmt_wallet_utxo_insert,
         "INSERT OR REPLACE INTO wallet_utxos"
         "(txid,vout,value,address_hash,"
         "script,height,is_coinbase)"
         " VALUES(?,?,?,?,?,?,?)");

    PREP(stmt_wallet_utxo_spend,
         "UPDATE wallet_utxos"
         " SET spent_txid=?,spent_vin=?"
         " WHERE txid=? AND vout=?");

    PREP(stmt_wallet_balance,
         "SELECT COALESCE(SUM(value),0)"
         " FROM wallet_utxos"
         " WHERE spent_txid IS NULL");

    PREP(stmt_nullifier_insert,
         "INSERT OR IGNORE INTO"
         " sapling_nullifiers(nullifier)"
         " VALUES(?)");

    PREP(stmt_nullifier_exists,
         "SELECT 1 FROM sapling_nullifiers"
         " WHERE nullifier=?");

    PREP(stmt_state_set,
         "INSERT OR REPLACE INTO"
         " node_state(key,value) VALUES(?,?)");

    PREP(stmt_state_get,
         "SELECT value FROM node_state"
         " WHERE key=?");

    /* Peer model */
    PREP(stmt_peer_save,
         "INSERT OR REPLACE INTO peers"
         "(ip,port,services,last_seen,last_try,attempts,source,"
         "bandwidth_score,is_zcl23)"
         " VALUES(?,?,?,?,?,?,?,?,?)");

    PREP(stmt_peer_find,
         "SELECT id,ip,port,services,last_seen,last_try,attempts,"
         "source,bandwidth_score,is_zcl23"
         " FROM peers WHERE ip=? AND port=?");

    PREP(stmt_peer_delete,
         "DELETE FROM peers WHERE ip=? AND port=?");

    PREP(stmt_peer_count,
         "SELECT COUNT(*) FROM peers");

    /* File service model */
    PREP(stmt_file_service_save,
         "INSERT OR REPLACE INTO file_services"
         " (ip, port, p2p_port, last_seen, is_zcl23)"
         " VALUES(?, ?, ?, ?, ?)");

    PREP(stmt_file_service_find,
         "SELECT ip, port, p2p_port, last_seen, is_zcl23"
         " FROM file_services WHERE ip=? AND port=?");

    /* Explorer projection inserts (full per-block indexer). All
     * INSERT OR REPLACE keyed on their natural PK so a reindex / restart
     * re-walk overwrites the row, never double-inserts. node.db only —
     * none of these touch coins_kv / progress.kv / consensus. */
    PREP(stmt_txo_insert,
         "INSERT OR REPLACE INTO tx_outputs"
         "(txid,vout,value,script_type,address_hash,block_height)"
         " VALUES(?,?,?,?,?,?)");

    PREP(stmt_txi_insert,
         "INSERT OR REPLACE INTO tx_inputs"
         "(txid,vin_index,prev_txid,prev_vout,block_height)"
         " VALUES(?,?,?,?,?)");

    PREP(stmt_opret_insert,
         "INSERT OR REPLACE INTO op_returns"
         "(txid,block_height,script,is_slp) VALUES(?,?,?,?)");

    PREP(stmt_sspend_insert,
         "INSERT OR REPLACE INTO sapling_spends"
         "(txid,spend_index,cv,anchor,nullifier,rk,block_height)"
         " VALUES(?,?,?,?,?,?,?)");

    PREP(stmt_soutput_insert,
         "INSERT OR REPLACE INTO sapling_outputs"
         "(txid,output_index,cv,cm,ephemeral_key,block_height)"
         " VALUES(?,?,?,?,?,?)");

    PREP(stmt_js_insert,
         "INSERT OR REPLACE INTO joinsplits"
         "(txid,js_index,vpub_old,vpub_new,anchor,block_height)"
         " VALUES(?,?,?,?,?,?)");

    PREP(stmt_spnf_insert,
         "INSERT OR REPLACE INTO sprout_nullifiers"
         "(nullifier,txid,block_height) VALUES(?,?,?)");

    PREP(stmt_vint_insert,
         "INSERT OR REPLACE INTO view_integrity"
         "(height,sha3_hash) VALUES(?,?)");

#undef PREP
    return true;
}

/* the cache_size and mmap_size values below are *the* tuning
 * knobs for node.db in normal (non-IBD-turbo) mode.  Pinned by a
 * regression test (test_db_pragma_tuning in test_sqlite.c).  Do not
 * bump mmap_size beyond 256 MB without
 * rereading the landmine comment at config/src/boot_index.c:306 —
 * secondary RW connections to the same file concurrent with the
 * main connection's WAL writes can dereference invalidated mmap
 * pages and SIGSEGV.  256 MB is safe for the *single* main handle
 * because all access goes through the serializing node_db mutex;
 * secondary opens elsewhere must stay at mmap_size=0. */
#define ZCL_NODE_DB_CACHE_SIZE_KIB  65536   /* -65536 → ~64 MiB page cache */
#define ZCL_NODE_DB_MMAP_BYTES      (256LL * 1024 * 1024)  /* 256 MiB */
#define ZCL_NODE_DB_BUSY_TIMEOUT_MS 10000
#define ZCL_DB_LONG_OP_PROGRESS_OPS 50000
#define ZCL_DB_LONG_OP_LOG_MS       15000
#define ZCL_DB_LONG_OP_LOG_MIN_BYTES (64LL * 1024LL * 1024LL)

static void db_set_pragmas(sqlite3 *db)
{
    /* One exec call to keep the PRAGMA batch atomic with respect to
     * other threads that might latch onto the connection immediately
     * after open_raw returns. */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA cache_size=-%d;"        /* negative → KiB units */
        "PRAGMA mmap_size=%lld;"
        "PRAGMA temp_store=MEMORY;"
        "PRAGMA foreign_keys=ON",
        ZCL_NODE_DB_CACHE_SIZE_KIB,
        (long long)ZCL_NODE_DB_MMAP_BYTES);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_busy_timeout(db, ZCL_NODE_DB_BUSY_TIMEOUT_MS);
}

struct db_long_op_progress {
    const char *op;
    const char *path;
    int64_t start_ms;
    int64_t last_log_ms;
    uint64_t callbacks;
    bool log_enabled;
};

static bool db_long_op_log_enabled(const char *path)
{
    if (!path || !path[0] || strcmp(path, ":memory:") == 0)
        return false;

    const char *force = getenv("ZCL_DB_LONG_OP_LOG_ALL");
    if (force && force[0] && strcmp(force, "0") != 0)
        return true;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return st.st_size >= ZCL_DB_LONG_OP_LOG_MIN_BYTES;
}

static int db_long_op_progress_cb(void *arg)
{
    struct db_long_op_progress *p = arg;
    if (!p)
        return 0;

    p->callbacks++;
    if (!p->log_enabled)
        return 0;

    int64_t now_ms = db_now_ms();
    if (now_ms - p->last_log_ms < ZCL_DB_LONG_OP_LOG_MS)
        return 0;

    LOG_INFO("db",
             "db: %s still running path=%s elapsed_ms=%lld callbacks=%llu",
             p->op ? p->op : "long_op",
             p->path ? p->path : "(unknown)",
             (long long)(now_ms - p->start_ms),
             (unsigned long long)p->callbacks);
    p->last_log_ms = now_ms;
    return 0;
}

static void db_long_op_start(sqlite3 *db, struct db_long_op_progress *progress,
                             const char *op, const char *path)
{
    if (!progress)
        return;

    progress->op = op;
    progress->path = path ? path : (db ? sqlite3_db_filename(db, "main") : NULL);
    progress->start_ms = db_now_ms();
    progress->last_log_ms = progress->start_ms;
    progress->callbacks = 0;
    progress->log_enabled = db_long_op_log_enabled(progress->path);

    if (!db || !progress->log_enabled)
        return;

    LOG_INFO("db", "db: %s start path=%s",
             progress->op ? progress->op : "long_op",
             progress->path ? progress->path : "(unknown)");
    sqlite3_progress_handler(db, ZCL_DB_LONG_OP_PROGRESS_OPS,
                             db_long_op_progress_cb, progress);
}

static void db_long_op_finish(sqlite3 *db, struct db_long_op_progress *progress,
                              bool ok, int rc)
{
    if (db)
        sqlite3_progress_handler(db, 0, NULL, NULL);
    if (!progress || !progress->log_enabled)
        return;

    int64_t elapsed_ms = db_now_ms() - progress->start_ms;
    LOG_INFO("db",
             "db: %s done path=%s ok=%d rc=%d elapsed_ms=%lld callbacks=%llu",
             progress->op ? progress->op : "long_op",
             progress->path ? progress->path : "(unknown)", ok ? 1 : 0, rc,
             (long long)elapsed_ms, (unsigned long long)progress->callbacks);
}

static int db_exec_checked_progress(sqlite3 *db, const char *sql,
                                    const char *where, const char *path)
{
    struct db_long_op_progress progress;
    db_long_op_start(db, &progress, where, path);
    int rc = db_exec_checked(db, sql, where);
    db_long_op_finish(db, &progress, rc == SQLITE_OK, rc);
    return rc;
}

static bool db_quick_check_ok(sqlite3 *db, const char *path)
{
    sqlite3_stmt *stmt = NULL;
    bool ok = false;
    struct db_long_op_progress progress;
    db_long_op_start(db, &progress, "quick_check", path);

    int rc = sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_long_op_finish(db, &progress, false, rc);
        LOG_FAIL("db", "db: quick_check prepare failed: %s",
                sqlite3_errmsg(db));
    }
    rc = sqlite3_step(stmt);  // raw-sql-ok:read-only-introspection
    if (rc == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        ok = txt && strcmp((const char *)txt, "ok") == 0;
        if (!ok && txt) {
            LOG_WARN("db", "db: quick_check failed: %s", txt);
        }
    } else {
        LOG_WARN("db", "db: quick_check step failed: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    db_long_op_finish(db, &progress, ok, rc);
    return ok;
}

static void db_quarantine_one(const char *path, const char *suffix)
{
    char dst[1400];
    if (access(path, F_OK) != 0) return;
    snprintf(dst, sizeof(dst), "%s.%s", path, suffix);
    if (rename(path, dst) == 0) {
        LOG_INFO("db", "db: quarantined %s -> %s", path, dst);
    } else {
        LOG_WARN("db", "db: failed to quarantine %s: %s", path, strerror(errno));
    }
}

static void db_quarantine_files(const char *path)
{
    char wal[1200];
    char shm[1200];
    char suffix[64];
    time_t now = platform_time_wall_time_t();
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(suffix, sizeof(suffix), "corrupt-%Y%m%dT%H%M%SZ", &tmv);

    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);

    db_quarantine_one(path, suffix);
    db_quarantine_one(wal, suffix);
    db_quarantine_one(shm, suffix);
}

static bool db_open_raw(sqlite3 **db_out, const char *path)
{
    int rc = sqlite3_open_v2(path, db_out,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        LOG_WARN("db", "db: cannot open %s: %s", path, sqlite3_errmsg(*db_out));
        sqlite3_close(*db_out);
        *db_out = NULL;
        return false;
    }
    db_set_pragmas(*db_out);
    return true;
}

static void finalize_statements(struct node_db *ndb)
{
    sqlite3_finalize(ndb->stmt_utxo_insert);
    sqlite3_finalize(ndb->stmt_snapshot_staging_insert);
    sqlite3_finalize(ndb->stmt_utxo_delete);
    sqlite3_finalize(ndb->stmt_utxo_find);
    sqlite3_finalize(ndb->stmt_block_insert);
    sqlite3_finalize(ndb->stmt_block_by_hash);
    sqlite3_finalize(ndb->stmt_block_by_height);
    sqlite3_finalize(ndb->stmt_tx_insert);
    sqlite3_finalize(ndb->stmt_tx_find);
    sqlite3_finalize(ndb->stmt_wallet_utxo_insert);
    sqlite3_finalize(ndb->stmt_wallet_utxo_spend);
    sqlite3_finalize(ndb->stmt_wallet_balance);
    sqlite3_finalize(ndb->stmt_nullifier_insert);
    sqlite3_finalize(ndb->stmt_nullifier_exists);
    sqlite3_finalize(ndb->stmt_state_set);
    sqlite3_finalize(ndb->stmt_state_get);
    sqlite3_finalize(ndb->stmt_peer_save);
    sqlite3_finalize(ndb->stmt_peer_find);
    sqlite3_finalize(ndb->stmt_peer_delete);
    sqlite3_finalize(ndb->stmt_peer_count);
    sqlite3_finalize(ndb->stmt_file_service_save);
    sqlite3_finalize(ndb->stmt_file_service_find);
    sqlite3_finalize(ndb->stmt_txo_insert);
    sqlite3_finalize(ndb->stmt_txi_insert);
    sqlite3_finalize(ndb->stmt_opret_insert);
    sqlite3_finalize(ndb->stmt_sspend_insert);
    sqlite3_finalize(ndb->stmt_soutput_insert);
    sqlite3_finalize(ndb->stmt_js_insert);
    sqlite3_finalize(ndb->stmt_spnf_insert);
    sqlite3_finalize(ndb->stmt_vint_insert);
}

bool node_db_open(struct node_db *ndb, const char *path)
{
    memset(ndb, 0, sizeof(*ndb));
    if (path)
        snprintf(ndb->path, sizeof(ndb->path), "%s", path);
    node_db_state_init(ndb);
    if (!db_open_raw(&ndb->db, path)) {
        node_db_state_destroy(ndb);
        return false;
    }

    if (!db_quick_check_ok(ndb->db, path)) {
        LOG_INFO("db", "db: %s is malformed; rebuilding fresh SQLite state", path);
        sqlite3_close(ndb->db);
        ndb->db = NULL;
        db_quarantine_files(path);
        if (!db_open_raw(&ndb->db, path)) {
            node_db_state_destroy(ndb);
            return false;
        }
    }

    if (!create_schema(ndb)) {
        sqlite3_close(ndb->db);
        ndb->db = NULL;
        node_db_state_destroy(ndb);
        return false;
    }

    ndb->open = true; /* node_db_migrate uses node_db_state_* helpers. */
    int migrated = node_db_migrate(ndb, NULL);
    ndb->open = false;
    if (migrated < 0) {
        sqlite3_close(ndb->db);
        ndb->db = NULL;
        node_db_state_destroy(ndb);
        return false;
    }

    /* Crash recovery: staged snapshot rows are never authoritative across
     * process lifetimes. If the node died mid-receive or mid-verify, discard
     * the incomplete staging namespace before normal boot continues. */
    if (db_exec_checked_progress(ndb->db,
            "DELETE FROM snapshot_staging_utxos",
            "snapshot_staging_boot_cleanup", path) != SQLITE_OK ||
        db_exec_checked_progress(ndb->db,
            "DELETE FROM node_state WHERE key LIKE 'snapshot_staging_%'",
            "snapshot_staging_state_boot_cleanup", path) != SQLITE_OK) {
        sqlite3_close(ndb->db);
        ndb->db = NULL;
        node_db_state_destroy(ndb);
        return false;
    }

    if (!prepare_statements(ndb)) {
        sqlite3_close(ndb->db);
        ndb->db = NULL;
        node_db_state_destroy(ndb);
        return false;
    }

    ndb->open = true;
    /* Register model validators once per process.  Idempotent. */
    db_register_all_validators();
    node_db_note_activity(ndb, "open", SQLITE_OK);
    return true;
}

void node_db_close(struct node_db *ndb)
{
    if (!ndb)
        return;
    if (!ndb->open) {
        node_db_state_destroy(ndb);
        return;
    }
    node_db_note_activity(ndb, "close", SQLITE_OK);
    finalize_statements(ndb);
    sqlite3_close(ndb->db);
    ndb->open = false;
    node_db_state_destroy(ndb);
}

bool node_db_exec(struct node_db *ndb, const char *sql)
{
    if (!ndb->open) return false;
    char *err = NULL;
    int rc = sqlite3_exec(ndb->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_WARN("db", "db: exec failed: %s", err);
        node_db_note_activity(ndb, sql ? sql : "exec", rc);
        sqlite3_free(err);
        return false;
    }
    node_db_note_activity(ndb, sql ? sql : "exec", rc);
    return true;
}

bool node_db_prepare_readonly_query(struct node_db *ndb, const char *sql,
                                    sqlite3_stmt **stmt_out)
{
    if (!ndb || !ndb->open || !sql || !stmt_out)
        LOG_FAIL("db", "prepare_readonly_query called with invalid arguments");

    *stmt_out = NULL;
    int rc = sqlite3_prepare_v2(ndb->db, sql, -1, stmt_out, NULL);
    node_db_note_activity(ndb, "prepare_readonly_query", rc);
    if (rc != SQLITE_OK || !*stmt_out) {
        LOG_FAIL("db", "prepare_readonly_query failed: rc=%d msg=%s sql=%s",
                 rc, sqlite3_errmsg(ndb->db), sql);
    }
    if (!sqlite3_stmt_readonly(*stmt_out)) {
        sqlite3_finalize(*stmt_out);
        *stmt_out = NULL;
        LOG_FAIL("db", "prepare_readonly_query rejected writable statement: %s",
                 sql);
    }
    return true;
}

bool node_db_begin(struct node_db *ndb)
{
    bool ok = node_db_exec(ndb, "BEGIN TRANSACTION");
    node_db_note_tx_state(ndb, ok, "BEGIN TRANSACTION",
                          ok ? SQLITE_OK : SQLITE_ERROR);
    return ok;
}

bool node_db_commit(struct node_db *ndb)
{
    bool ok = node_db_exec(ndb, "COMMIT");
    node_db_note_tx_state(ndb, false, "COMMIT",
                          ok ? SQLITE_OK : SQLITE_ERROR);
    return ok;
}

bool node_db_rollback(struct node_db *ndb)
{
    bool ok = node_db_exec(ndb, "ROLLBACK");
    node_db_note_tx_state(ndb, false, "ROLLBACK",
                          ok ? SQLITE_OK : SQLITE_ERROR);
    return ok;
}

void node_db_set_sync_batch_size(struct node_db *ndb, int batch_size)
{
    if (!ndb) return;
    ndb->sync_batch_size = batch_size > 0 ? batch_size : 1;
    node_db_note_activity(ndb, "set_sync_batch_size", SQLITE_OK);
}

bool node_db_sync_flush(struct node_db *ndb)
{
    if (!ndb || !ndb->open) return false;
    if (ndb->sync_in_batch) {
        bool ok = node_db_commit(ndb);
        ndb->sync_in_batch = false;
        ndb->sync_pending_blocks = 0;
        return ok;
    }
    return true;
}

void node_db_get_status(struct node_db *ndb, struct node_db_status *out)
{
    struct node_db_status empty = {0};

    if (!out)
        return;
    *out = empty;
    if (!ndb)
        return;

    if (ndb->state_mutex_init)
        zcl_mutex_lock(&ndb->state_mutex);
    out->open = ndb->open;
    out->tx_open = ndb->tx_open;
    out->turbo_mode = ndb->turbo_mode;
    out->sync_batch_size = ndb->sync_batch_size;
    out->sync_pending_blocks = ndb->sync_pending_blocks;
    out->last_activity_time = ndb->last_activity_time;
    out->last_sqlite_rc = ndb->last_sqlite_rc;
    snprintf(out->last_op, sizeof(out->last_op), "%s", ndb->last_op);
    if (ndb->state_mutex_init)
        zcl_mutex_unlock(&ndb->state_mutex);
}

/* node_db_state_*, node_db_schema_version, and node_db_migrate live
 * in database_migrate.c (the KV store + migration runner). */
