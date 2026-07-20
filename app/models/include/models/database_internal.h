/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Internal helper contract shared between the node_db connection-handle
 * source files (database.c, database_migrate.c, database_migrate_features.c,
 * database_modes.c). NOT part of the public model API — callers must
 * include models/database.h.
 *
 * This header keeps shared connection-handle helpers visible only to the
 * database model siblings that own migrations, runtime modes, and health
 * activity stamping. */

#ifndef ZCL_DB_MODEL_DATABASE_INTERNAL_H
#define ZCL_DB_MODEL_DATABASE_INTERNAL_H

#include "models/database.h"
#include "util/log_macros.h"
#include <sqlite3.h>

/* Persist the schema_version counter; halt migration on failure so
 * we don't silently re-apply the same migration on next boot. Used by
 * every versioned migration block in database_migrate.c and
 * database_migrate_features.c. */
#define DB_MIGRATE_PERSIST_VERSION(ndb, ver) do { \
    int32_t _v = (int32_t)(ver); \
    if (!node_db_state_set((ndb), "schema_version", &_v, sizeof(_v))) { \
        LOG_ERR("db", "migrate: failed to persist " \
                "schema_version=%d; aborting migration to prevent " \
                "loop on next boot", (int)_v); \
    } \
} while (0)

/* Apply the app-feature migration blocks (schema v14+): store products
 * and orders, ZCL Market file offers, ZNAM name registry, ZMSG
 * messaging, ZSWP atomic-swap contracts, HODL wave history, and the
 * content-addressed blob store. `*version` is the current schema
 * version on entry and the post-migration version on return; returns
 * the number of migration blocks applied. (Defined in
 * database_migrate_features.c; called only by node_db_migrate().) */
int node_db_migrate_features(struct node_db *ndb, int *version);

/* Continuation of node_db_migrate_features() for schema v30+ (E1 file-size
 * split — database_migrate_features2.c). Same contract; called only by
 * node_db_migrate_features() at the v30 handoff. */
int node_db_migrate_features2(struct node_db *ndb, int *version);

/* Execute `sql`, logging any error with `where` context. Returns the
 * sqlite3 rc so callers can make tolerance decisions. (Defined in
 * database.c; used by migrations and performance-mode helpers.) */
int db_exec_checked(sqlite3 *db, const char *sql, const char *where);

/* Apply the idempotent baseline schema DDL (SCHEMA[]). Defined in
 * database_schema.c; called once from node_db_open(). Returns false on a
 * real schema regression (boot must halt). */
bool create_schema(struct node_db *ndb);

/* Like db_exec_checked but tolerates a known-benign error substring
 * (e.g. "duplicate column name" when re-applying idempotent ALTERs).
 * (Defined in database.c; used by the migration runner.) */
int db_exec_tolerant(sqlite3 *db, const char *sql, const char *where,
                     const char *tolerable_substr);

/* Record turbo-mode transition in the runtime health snapshot.
 * (Defined in database.c; used by performance-mode helpers.) */
void node_db_note_turbo_mode(struct node_db *ndb, bool turbo_mode,
                             const char *op, int rc);

/* Stamp last-activity time + sqlite rc + op label into the runtime
 * health snapshot. (Defined in database.c; used by the KV state store
 * in database_migrate.c.) */
void node_db_note_activity(struct node_db *ndb, const char *op, int rc);

/* ── Long-running maintenance-op machinery (database_long_op.c) ─────────
 *
 * The progress-handler wrappers and the lock-free busy-op registry that backs
 * the public node_db_long_op_active() live in database_long_op.c so database.c
 * stays focused on the connection open/migration lifecycle. db_quick_check_ok()
 * and node_db_open() (in database.c) call db_long_op_start/finish and
 * db_exec_checked_progress;
 * the progress callback raises the deadline blocker via note_deadline. See
 * database.h for the public reader and database_long_op.c for the rationale. */
#define ZCL_DB_LONG_OP_DEADLINE_MS  (15 * 60 * 1000)

struct db_long_op_progress {
    const char *op;
    const char *path;
    int64_t start_ms;
    int64_t last_log_ms;
    uint64_t callbacks;
    bool log_enabled;
};

/* Install/remove the SQLite progress handler and publish/clear the busy op. */
void db_long_op_start(sqlite3 *db, struct db_long_op_progress *progress,
                      const char *op, const char *path);
void db_long_op_finish(sqlite3 *db, struct db_long_op_progress *progress,
                       bool ok, int rc);
/* db_exec_checked wrapped in the long-op progress lifecycle. */
int db_exec_checked_progress(sqlite3 *db, const char *sql,
                             const char *where, const char *path);

/* Registry hooks shared with the progress callback. */
void db_long_op_publish(const char *op, int64_t start_ms);
void db_long_op_unpublish(void);
void db_long_op_note_deadline(const char *op, int64_t elapsed_ms);

#endif
