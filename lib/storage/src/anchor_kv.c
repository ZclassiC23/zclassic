/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * anchor_kv -- implementation.  See storage/anchor_kv.h.
 *
 * This is a progress.kv kernel primitive.  Raw sqlite steps use the canonical
 * progress-kv marker from DEFENSIVE_CODING.md; callers own transaction scope. */

#include "storage/anchor_kv.h"

#include "core/serialize.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ANCHOR_SUBSYS "anchor_kv"

static bool anchor_pool_valid(int pool)
{
    return pool == ANCHOR_POOL_SPROUT || pool == ANCHOR_POOL_SAPLING;
}

static const char *anchor_table(int pool)
{
    return pool == ANCHOR_POOL_SPROUT ? "sprout_anchors"
                                      : "sapling_anchors";
}

static void anchor_tree_init(int pool, struct incremental_merkle_tree *tree)
{
    if (pool == ANCHOR_POOL_SPROUT)
        sprout_tree_init(tree);
    else
        sapling_tree_init(tree);
}

static bool anchor_empty_tree(int pool, const struct uint256 *root,
                              struct incremental_merkle_tree *tree_out)
{
    struct incremental_merkle_tree empty;
    struct uint256 empty_root;
    anchor_tree_init(pool, &empty);
    incremental_tree_root(&empty, &empty_root);
    if (!uint256_eq(root, &empty_root))
        return false;
    if (tree_out)
        *tree_out = empty;
    return true;
}

bool anchor_kv_ensure_schema(sqlite3 *db)
{
    if (!db) {
        LOG_WARN(ANCHOR_SUBSYS, "ensure_schema: NULL db");
        return false;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS sprout_anchors ("
        "  anchor BLOB PRIMARY KEY NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  tree BLOB NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_sprout_anchors_height "
        "ON sprout_anchors(height);"
        "CREATE TABLE IF NOT EXISTS sapling_anchors ("
        "  anchor BLOB PRIMARY KEY NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  tree BLOB NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_sapling_anchors_height "
        "ON sapling_anchors(height);"
        "CREATE TABLE IF NOT EXISTS anchor_state ("
        "  pool INTEGER PRIMARY KEY NOT NULL,"
        "  activation_cursor INTEGER NOT NULL,"
        "  CHECK(pool IN (0,1)),"
        "  CHECK(activation_cursor >= 0)"
        ") WITHOUT ROWID",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "ensure_schema failed: %s",
                 err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    if (err) sqlite3_free(err);
    return true;
}

bool anchor_kv_initialize_history(sqlite3 *db, int64_t activation_cursor)
{
    if (!db || activation_cursor < 0) {
        LOG_WARN(ANCHOR_SUBSYS,
                 "initialize_history: invalid args db=%p cursor=%lld",
                 (void *)db, (long long)activation_cursor);
        return false;
    }
    /* First adoption must not leave CREATEd tables without state rows after a
     * crash: absent state means unknown coverage.  Own a transaction when the
     * caller did not already provide one; refold/reset callers participate in
     * their existing progress.kv transaction. */
    bool own_tx = sqlite3_get_autocommit(db) != 0;
    char *err = NULL;
    if (own_tx && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err)
                      != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "initialize BEGIN failed: %s",
                 err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    if (!anchor_kv_ensure_schema(db)) {
        if (own_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO anchor_state(pool,activation_cursor) "
            "VALUES(?,?)", -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "initialize prepare failed: %s",
                 sqlite3_errmsg(db));
        if (own_tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return false;
    }
    bool ok = true;
    for (int pool = ANCHOR_POOL_SPROUT; pool <= ANCHOR_POOL_SAPLING; pool++) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        if (sqlite3_bind_int(s, 1, pool) != SQLITE_OK ||
            sqlite3_bind_int64(s, 2, (sqlite3_int64)activation_cursor)
                != SQLITE_OK) {
            LOG_WARN(ANCHOR_SUBSYS, "initialize bind failed: %s",
                     sqlite3_errmsg(db));
            ok = false;
            break;
        }
        int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
        if (rc != SQLITE_DONE) {
            LOG_WARN(ANCHOR_SUBSYS, "initialize step rc=%d: %s", rc,
                     sqlite3_errmsg(db));
            ok = false;
            break;
        }
    }
    sqlite3_finalize(s);
    if (own_tx) {
        const char *finish = ok ? "COMMIT" : "ROLLBACK";
        err = NULL;
        if (sqlite3_exec(db, finish, NULL, NULL, &err) != SQLITE_OK) {
            LOG_WARN(ANCHOR_SUBSYS, "initialize %s failed: %s", finish,
                     err ? err : sqlite3_errmsg(db));
            if (err) sqlite3_free(err);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return false;
        }
        if (err) sqlite3_free(err);
    }
    return ok;
}

bool anchor_kv_activation_cursor(sqlite3 *db, int pool,
                                 int64_t *cursor_out, bool *found_out)
{
    if (cursor_out) *cursor_out = 0;
    if (found_out) *found_out = false;
    if (!db || !anchor_pool_valid(pool) || !cursor_out || !found_out) {
        LOG_WARN(ANCHOR_SUBSYS, "activation_cursor: invalid args");
        return false;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT activation_cursor FROM anchor_state WHERE pool=?",
            -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "activation_cursor prepare failed: %s",
                 sqlite3_errmsg(db));
        return false;
    }
    if (sqlite3_bind_int(s, 1, pool) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "activation_cursor bind failed: %s",
                 sqlite3_errmsg(db));
        sqlite3_finalize(s);
        return false;
    }
    bool ok = true;
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        *found_out = true;
        *cursor_out = sqlite3_column_int64(s, 0);
        if (*cursor_out < 0) {
            LOG_WARN(ANCHOR_SUBSYS,
                     "activation_cursor corrupt negative value=%lld pool=%d",
                     (long long)*cursor_out, pool);
            ok = false;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN(ANCHOR_SUBSYS, "activation_cursor step rc=%d: %s", rc,
                 sqlite3_errmsg(db));
        ok = false;
    }
    sqlite3_finalize(s);
    return ok;
}

static bool anchor_deserialize_checked(int pool, const struct uint256 *want,
                                       const void *blob, int blob_len,
                                       struct incremental_merkle_tree *out)
{
    if (!blob || blob_len <= 0 || !out)
        return false;
    anchor_tree_init(pool, out);
    struct byte_stream bs;
    stream_init_from_data(&bs, blob, (size_t)blob_len);
    if (!incremental_tree_deserialize(out, &bs) ||
        stream_remaining(&bs) != 0) {
        LOG_WARN(ANCHOR_SUBSYS,
                 "anchor tree decode failed pool=%d blob_len=%d remaining=%zu",
                 pool, blob_len, stream_remaining(&bs));
        return false;
    }
    struct uint256 got;
    incremental_tree_root(out, &got);
    if (!uint256_eq(&got, want)) {
        LOG_WARN(ANCHOR_SUBSYS,
                 "anchor tree/root mismatch pool=%d (durable row corrupt)",
                 pool);
        return false;
    }
    return true;
}

enum anchor_kv_lookup_result anchor_kv_get(
    sqlite3 *db, int pool, const struct uint256 *root,
    struct incremental_merkle_tree *tree_out, int64_t *height_out)
{
    if (height_out) *height_out = -1;
    if (!db || !anchor_pool_valid(pool) || !root) {
        LOG_WARN(ANCHOR_SUBSYS, "get: invalid args");
        return ANCHOR_KV_ERROR;
    }
    if (anchor_empty_tree(pool, root, tree_out)) {
        if (height_out) *height_out = -1;
        return ANCHOR_KV_FOUND;
    }

    char sql[128];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT height,tree FROM %s WHERE anchor=?",
                     anchor_table(pool));
    if (n <= 0 || (size_t)n >= sizeof(sql))
        return ANCHOR_KV_ERROR;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "get prepare failed pool=%d: %s", pool,
                 sqlite3_errmsg(db));
        return ANCHOR_KV_ERROR;
    }
    if (sqlite3_bind_blob(s, 1, root->data, 32, SQLITE_TRANSIENT)
            != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "get bind failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(s);
        return ANCHOR_KV_ERROR;
    }
    enum anchor_kv_lookup_result result = ANCHOR_KV_MISSING;
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(s, 1);
        int blob_len = sqlite3_column_bytes(s, 1);
        struct incremental_merkle_tree scratch;
        struct incremental_merkle_tree *dst = tree_out ? tree_out : &scratch;
        if (!anchor_deserialize_checked(pool, root, blob, blob_len, dst))
            result = ANCHOR_KV_ERROR;
        else {
            if (height_out) *height_out = sqlite3_column_int64(s, 0);
            result = ANCHOR_KV_FOUND;
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN(ANCHOR_SUBSYS, "get step rc=%d: %s", rc,
                 sqlite3_errmsg(db));
        result = ANCHOR_KV_ERROR;
    }
    sqlite3_finalize(s);
    if (result != ANCHOR_KV_MISSING)
        return result;

    int64_t activation = 0;
    bool state_found = false;
    if (!anchor_kv_activation_cursor(db, pool, &activation, &state_found))
        return ANCHOR_KV_ERROR;
    /* Missing state is not proof of completeness.  Fail closed just like an
     * explicitly nonzero adoption cursor. */
    if (!state_found || activation > 0)
        return ANCHOR_KV_HISTORY_INCOMPLETE;
    return ANCHOR_KV_MISSING;
}

enum anchor_kv_lookup_result anchor_kv_latest_tree(
    sqlite3 *db, int pool, struct incremental_merkle_tree *tree_out,
    struct uint256 *root_out, int64_t *height_out)
{
    if (height_out) *height_out = -1;
    if (!db || !anchor_pool_valid(pool) || !tree_out) {
        LOG_WARN(ANCHOR_SUBSYS, "latest_tree: invalid args");
        return ANCHOR_KV_ERROR;
    }
    char sql[160];
    int n = snprintf(sql, sizeof(sql),
                     "SELECT anchor,height,tree FROM %s "
                     "ORDER BY height DESC LIMIT 1", anchor_table(pool));
    if (n <= 0 || (size_t)n >= sizeof(sql))
        return ANCHOR_KV_ERROR;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "latest_tree prepare failed: %s",
                 sqlite3_errmsg(db));
        return ANCHOR_KV_ERROR;
    }
    enum anchor_kv_lookup_result result = ANCHOR_KV_HISTORY_INCOMPLETE;
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    if (rc == SQLITE_ROW) {
        const void *rblob = sqlite3_column_blob(s, 0);
        int rlen = sqlite3_column_bytes(s, 0);
        const void *tblob = sqlite3_column_blob(s, 2);
        int tlen = sqlite3_column_bytes(s, 2);
        struct uint256 root;
        if (!rblob || rlen != 32) {
            LOG_WARN(ANCHOR_SUBSYS, "latest_tree invalid root length=%d", rlen);
            result = ANCHOR_KV_ERROR;
        } else {
            memcpy(root.data, rblob, 32);
            if (!anchor_deserialize_checked(pool, &root, tblob, tlen,
                                            tree_out))
                result = ANCHOR_KV_ERROR;
            else {
                if (root_out) *root_out = root;
                if (height_out) *height_out = sqlite3_column_int64(s, 1);
                result = ANCHOR_KV_FOUND;
            }
        }
    } else if (rc != SQLITE_DONE) {
        LOG_WARN(ANCHOR_SUBSYS, "latest_tree step rc=%d: %s", rc,
                 sqlite3_errmsg(db));
        result = ANCHOR_KV_ERROR;
    }
    sqlite3_finalize(s);
    if (result == ANCHOR_KV_FOUND || result == ANCHOR_KV_ERROR)
        return result;

    int64_t activation = 0;
    bool state_found = false;
    if (!anchor_kv_activation_cursor(db, pool, &activation, &state_found))
        return ANCHOR_KV_ERROR;
    if (!state_found || activation > 0)
        return ANCHOR_KV_HISTORY_INCOMPLETE;
    anchor_tree_init(pool, tree_out);
    if (root_out) incremental_tree_root(tree_out, root_out);
    return ANCHOR_KV_FOUND;
}

bool anchor_kv_add_tree(sqlite3 *db, int pool,
                        const struct incremental_merkle_tree *tree,
                        int64_t height)
{
    if (!db || !anchor_pool_valid(pool) || !tree || height < 0) {
        LOG_WARN(ANCHOR_SUBSYS, "add_tree: invalid args pool=%d height=%lld",
                 pool, (long long)height);
        return false;
    }
    struct uint256 root;
    incremental_tree_root(tree, &root);
    struct incremental_merkle_tree empty;
    struct uint256 empty_root;
    anchor_tree_init(pool, &empty);
    incremental_tree_root(&empty, &empty_root);
    if (uint256_eq(&root, &empty_root))
        return true;  /* implicit in zclassicd too */

    struct byte_stream bs;
    stream_init(&bs, 2048);
    if (!incremental_tree_serialize(tree, &bs)) {
        stream_free(&bs);
        LOG_WARN(ANCHOR_SUBSYS, "add_tree serialize failed pool=%d h=%lld",
                 pool, (long long)height);
        return false;
    }
    char sql[192];
    int n = snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO %s(anchor,height,tree) VALUES(?,?,?)",
        anchor_table(pool));
    if (n <= 0 || (size_t)n >= sizeof(sql)) {
        stream_free(&bs);
        return false;
    }
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "add_tree prepare failed: %s",
                 sqlite3_errmsg(db));
        stream_free(&bs);
        return false;
    }
    bool bound =
        sqlite3_bind_blob(s, 1, root.data, 32, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(s, 2, (sqlite3_int64)height) == SQLITE_OK &&
        sqlite3_bind_blob(s, 3, bs.data, (int)bs.size, SQLITE_TRANSIENT)
            == SQLITE_OK;
    if (!bound) {
        LOG_WARN(ANCHOR_SUBSYS, "add_tree bind failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(s);
        stream_free(&bs);
        return false;
    }
    int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
    sqlite3_finalize(s);
    stream_free(&bs);
    if (rc != SQLITE_DONE) {
        LOG_WARN(ANCHOR_SUBSYS, "add_tree step rc=%d: %s", rc,
                 sqlite3_errmsg(db));
        return false;
    }
    return true;
}

bool anchor_kv_delete_range(sqlite3 *db, int64_t first_height,
                            int64_t last_height)
{
    if (!db || first_height < 0 || last_height < first_height) {
        LOG_WARN(ANCHOR_SUBSYS,
                 "delete_range: invalid args first=%lld last=%lld",
                 (long long)first_height, (long long)last_height);
        return false;
    }
    sqlite3_stmt *s = NULL;
    for (int pool = ANCHOR_POOL_SPROUT; pool <= ANCHOR_POOL_SAPLING; pool++) {
        char sql[128];
        int n = snprintf(sql, sizeof(sql),
                         "DELETE FROM %s WHERE height>=? AND height<=?",
                         anchor_table(pool));
        if (n <= 0 || (size_t)n >= sizeof(sql) ||
            sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
            LOG_WARN(ANCHOR_SUBSYS, "delete_range prepare failed: %s",
                     sqlite3_errmsg(db));
            return false;
        }
        if (sqlite3_bind_int64(s, 1, (sqlite3_int64)first_height)
                != SQLITE_OK ||
            sqlite3_bind_int64(s, 2, (sqlite3_int64)last_height)
                != SQLITE_OK) {
            LOG_WARN(ANCHOR_SUBSYS, "delete_range bind failed: %s",
                     sqlite3_errmsg(db));
            sqlite3_finalize(s);
            return false;
        }
        int rc = sqlite3_step(s);  // raw-sql-ok:progress-kv-kernel-store
        sqlite3_finalize(s);
        s = NULL;
        if (rc != SQLITE_DONE) {
            LOG_WARN(ANCHOR_SUBSYS, "delete_range step rc=%d: %s", rc,
                     sqlite3_errmsg(db));
            return false;
        }
    }
    return true;
}

bool anchor_kv_reset_in_tx(sqlite3 *db, int64_t activation_cursor)
{
    if (!db || activation_cursor < 0) {
        LOG_WARN(ANCHOR_SUBSYS, "reset: invalid args cursor=%lld",
                 (long long)activation_cursor);
        return false;
    }
    if (!anchor_kv_ensure_schema(db))
        return false;
    char *err = NULL;
    if (sqlite3_exec(db,
            "DELETE FROM sprout_anchors; DELETE FROM sapling_anchors; "
            "DELETE FROM anchor_state", NULL, NULL, &err) != SQLITE_OK) {
        LOG_WARN(ANCHOR_SUBSYS, "reset delete failed: %s",
                 err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    if (err) sqlite3_free(err);
    return anchor_kv_initialize_history(db, activation_cursor);
}
