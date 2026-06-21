/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * znam_projection — event-log consumer for rebuildable ZNAM state.
 *
 * Consumes EV_ZNAM_REGISTER / EV_ZNAM_UPDATE / EV_ZNAM_TRANSFER /
 * EV_ZNAM_RENEW / EV_ZNAM_EXPIRE into a rebuildable SQLite projection.
 * Mirror of peers_projection.c shape.
 */

#include "storage/znam_projection.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "storage/event_log_payloads.h"
#include "storage/projection_util.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <inttypes.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZNAM_PROJECTION_SCHEMA_VERSION 1

struct znam_projection {
    sqlite3 *db;
    event_log_t *log;
    uint64_t last_consumed_offset;
    uint64_t events_consumed_total;
    uint64_t register_total;
    uint64_t update_addr_total;
    uint64_t update_text_total;
    uint64_t update_primary_total;
    uint64_t transfer_total;
    uint64_t renew_total;
    uint64_t expire_total;
    uint64_t last_catch_up_ms;
    char path[1024];
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(znam_projection_t *) g_projection = NULL;
static _Atomic uint64_t g_emit_register_total = 0;
static _Atomic uint64_t g_emit_update_addr_total = 0;
static _Atomic uint64_t g_emit_update_text_total = 0;
static _Atomic uint64_t g_emit_update_primary_total = 0;
static _Atomic uint64_t g_emit_transfer_total = 0;
static _Atomic uint64_t g_emit_renew_total = 0;
static _Atomic uint64_t g_emit_expire_total = 0;
static _Atomic uint64_t g_emit_fail_total = 0;

/* now_ms / apply_pragmas / meta_get_u64 / meta_set_u64 live in
 * storage/projection_util.h. exec_sql stays local for its
 * "[znam_projection]" log prefix. */

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:znam-projection-sql
                "[znam_projection] %s failed: %s\n",
                ctx, err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool ensure_schema(sqlite3 *db)
{
    return exec_sql(db,
        "CREATE TABLE IF NOT EXISTS znam_names ("
        " name TEXT PRIMARY KEY,"
        " owner_address TEXT NOT NULL,"
        " target_type INTEGER NOT NULL,"
        " target_value TEXT NOT NULL,"
        " reg_txid BLOB NOT NULL,"
        " reg_height INTEGER NOT NULL,"
        " registered_unix INTEGER NOT NULL DEFAULT 0,"
        " expiry_height INTEGER NOT NULL DEFAULT 0,"
        " last_update_txid BLOB NOT NULL"
        ") WITHOUT ROWID",
        "create znam_names") &&
        exec_sql(db,
        "CREATE INDEX IF NOT EXISTS idx_znam_owner "
        "ON znam_names(owner_address)",
        "create idx_znam_owner") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS znam_addr_records ("
        " name TEXT NOT NULL,"
        " coin_type INTEGER NOT NULL,"
        " address TEXT NOT NULL,"
        " PRIMARY KEY(name, coin_type)"
        ") WITHOUT ROWID",
        "create znam_addr_records") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS znam_text_records ("
        " name TEXT NOT NULL,"
        " key TEXT NOT NULL,"
        " value TEXT,"
        " PRIMARY KEY(name, key)"
        ") WITHOUT ROWID",
        "create znam_text_records") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS projection_meta ("
        " k TEXT PRIMARY KEY,"
        " v TEXT NOT NULL"
        ")",
        "create projection_meta") &&
        exec_sql(db,
        "INSERT OR IGNORE INTO projection_meta(k,v) "
        "VALUES('schema_version','1')",
        "insert schema_version") &&
        exec_sql(db,
        "INSERT OR IGNORE INTO projection_meta(k,v) "
        "VALUES('last_consumed_offset','0')",
        "insert last_consumed_offset");
}

znam_projection_t *znam_projection_open(const char *projection_path,
                                        event_log_t *log)
{
    if (!projection_path || !projection_path[0] || !log) {
        fprintf(stderr,  // obs-ok:znam-projection-open
                "[znam_projection] open: invalid args path=%p log=%p\n",
                (const void *)projection_path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(projection_path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:znam-projection-open
                "[znam_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    znam_projection_t *p = zcl_malloc(sizeof(*p), "znam_projection");
    if (!p) {
        sqlite3_close(db);
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->db = db;
    p->log = log;
    p->last_consumed_offset = meta_get_u64(db, "last_consumed_offset");
    snprintf(p->path, sizeof(p->path), "%s", projection_path);
    atomic_store_explicit(&g_projection, p, memory_order_release);
    return p;
}

void znam_projection_close(znam_projection_t *p)
{
    if (!p) return;
    znam_projection_t *cur = atomic_load_explicit(&g_projection,
                                                  memory_order_acquire);
    if (cur == p)
        atomic_store_explicit(&g_projection, NULL, memory_order_release);
    if (p->db) {
        sqlite3_exec(p->db, "PRAGMA wal_checkpoint(TRUNCATE)",
                     NULL, NULL, NULL);
        sqlite3_close(p->db);
    }
    free(p);
}

/* ── Apply helpers (used by catch_up replay) ───────────────────── */

/* Record the most recent update txid on a name's parent row. Best-effort:
   the addr/text record write is the authoritative change, so a failure to
   prepare this secondary UPDATE is non-fatal and silently ignored. */
static void bump_last_update_txid(znam_projection_t *p,
                                  const char *name,
                                  const uint8_t update_txid[32])
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "UPDATE znam_names SET last_update_txid=? WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return;
    sqlite3_bind_blob(s, 1, update_txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
}

static bool apply_register(znam_projection_t *p,
                           const struct ev_znam_register *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    char owner[EV_ZNAM_OWNER_MAX + 1];
    char target_value[EV_ZNAM_VALUE_MAX + 1];
    memset(name, 0, sizeof(name));
    memset(owner, 0, sizeof(owner));
    memset(target_value, 0, sizeof(target_value));
    memcpy(name, ev->name, ev->name_len);
    memcpy(owner, ev->owner_address, ev->owner_len);
    if (ev->target_value_len)
        memcpy(target_value, ev->target_value, ev->target_value_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO znam_names"
        "(name,owner_address,target_type,target_value,"
        " reg_txid,reg_height,registered_unix,expiry_height,"
        " last_update_txid)"
        " VALUES(?,?,?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, owner, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, ev->target_type);
    sqlite3_bind_text(s, 4, target_value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 5, ev->reg_txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 6, ev->reg_height);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)ev->registered_unix);
    sqlite3_bind_int(s, 8, ev->expiry_height);
    sqlite3_bind_blob(s, 9, ev->reg_txid, 32, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_update_addr(znam_projection_t *p,
                              const struct ev_znam_update *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    char value[EV_ZNAM_VALUE_MAX + 1];
    memset(name, 0, sizeof(name));
    memset(value, 0, sizeof(value));
    memcpy(name, ev->name, ev->name_len);
    if (ev->value_len)
        memcpy(value, ev->value, ev->value_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO znam_addr_records(name,coin_type,address)"
        " VALUES(?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->key_or_coin_type);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return false;

    bump_last_update_txid(p, name, ev->update_txid);
    return true;
}

static bool apply_update_text(znam_projection_t *p,
                              const struct ev_znam_update *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    char key[EV_ZNAM_KEY_MAX + 1];
    char value[EV_ZNAM_VALUE_MAX + 1];
    memset(name, 0, sizeof(name));
    memset(key, 0, sizeof(key));
    memset(value, 0, sizeof(value));
    memcpy(name, ev->name, ev->name_len);
    if (ev->key_len)
        memcpy(key, ev->key, ev->key_len);
    if (ev->value_len)
        memcpy(value, ev->value, ev->value_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO znam_text_records(name,key,value)"
        " VALUES(?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return false;

    bump_last_update_txid(p, name, ev->update_txid);
    return true;
}

static bool apply_update_primary(znam_projection_t *p,
                                 const struct ev_znam_update *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    char value[EV_ZNAM_VALUE_MAX + 1];
    memset(name, 0, sizeof(name));
    memset(value, 0, sizeof(value));
    memcpy(name, ev->name, ev->name_len);
    if (ev->value_len)
        memcpy(value, ev->value, ev->value_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "UPDATE znam_names SET target_type=?,target_value=?,last_update_txid=?"
        " WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_int(s, 1, ev->key_or_coin_type);
    sqlite3_bind_text(s, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 3, ev->update_txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_transfer(znam_projection_t *p,
                           const struct ev_znam_transfer *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    char new_owner[EV_ZNAM_OWNER_MAX + 1];
    memset(name, 0, sizeof(name));
    memset(new_owner, 0, sizeof(new_owner));
    memcpy(name, ev->name, ev->name_len);
    memcpy(new_owner, ev->new_owner, ev->new_owner_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "UPDATE znam_names SET owner_address=?,last_update_txid=?"
        " WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, new_owner, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(s, 2, ev->update_txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_renew(znam_projection_t *p,
                        const struct ev_znam_renew *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    memset(name, 0, sizeof(name));
    memcpy(name, ev->name, ev->name_len);

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "UPDATE znam_names SET expiry_height=?,last_update_txid=?"
        " WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_int(s, 1, ev->new_expiry_height);
    sqlite3_bind_blob(s, 2, ev->update_txid, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_expire(znam_projection_t *p,
                         const struct ev_znam_expire *ev)
{
    char name[EV_ZNAM_NAME_MAX + 1];
    memset(name, 0, sizeof(name));
    memcpy(name, ev->name, ev->name_len);

    sqlite3_stmt *s = NULL;
    int rc;
    /* Remove all related records — name lease has lapsed. */
    rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM znam_addr_records WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);

    rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM znam_text_records WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);

    rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM znam_names WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

/* ── catch_up ───────────────────────────────────────────────────── */

struct catchup_ctx {
    znam_projection_t *p;
    bool ok;
    uint64_t next_offset;
    uint64_t since_commit;
};

static bool catchup_cb(uint64_t offset, enum event_log_type type,
                       const void *payload, size_t len, void *user)
{
    struct catchup_ctx *ctx = user;
    znam_projection_t *p = ctx->p;
    uint64_t next = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;

    bool handled = false;
    if (type == EV_ZNAM_REGISTER) {
        struct ev_znam_register ev;
        if (!ev_znam_register_parse(payload, len, &ev) ||
            !apply_register(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->register_total++;
        p->events_consumed_total++;
        handled = true;
    } else if (type == EV_ZNAM_UPDATE) {
        struct ev_znam_update ev;
        if (!ev_znam_update_parse(payload, len, &ev)) {
            ctx->ok = false;
            return false;
        }
        bool applied = false;
        if (ev.action_type == EV_ZNAM_UPDATE_ACTION_ADDR) {
            applied = apply_update_addr(p, &ev);
            if (applied) p->update_addr_total++;
        } else if (ev.action_type == EV_ZNAM_UPDATE_ACTION_TEXT) {
            applied = apply_update_text(p, &ev);
            if (applied) p->update_text_total++;
        } else if (ev.action_type == EV_ZNAM_UPDATE_ACTION_PRIMARY) {
            applied = apply_update_primary(p, &ev);
            if (applied) p->update_primary_total++;
        } else {
            /* Unknown action — skip without failing. */
            applied = true;
        }
        if (!applied) {
            ctx->ok = false;
            return false;
        }
        p->events_consumed_total++;
        handled = true;
    } else if (type == EV_ZNAM_TRANSFER) {
        struct ev_znam_transfer ev;
        if (!ev_znam_transfer_parse(payload, len, &ev) ||
            !apply_transfer(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->transfer_total++;
        p->events_consumed_total++;
        handled = true;
    } else if (type == EV_ZNAM_RENEW) {
        struct ev_znam_renew ev;
        if (!ev_znam_renew_parse(payload, len, &ev) ||
            !apply_renew(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->renew_total++;
        p->events_consumed_total++;
        handled = true;
    } else if (type == EV_ZNAM_EXPIRE) {
        struct ev_znam_expire ev;
        if (!ev_znam_expire_parse(payload, len, &ev) ||
            !apply_expire(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->expire_total++;
        p->events_consumed_total++;
        handled = true;
    }

    (void)handled;
    ctx->next_offset = next;
    p->last_consumed_offset = next;
    ctx->since_commit++;
    if (ctx->since_commit >= 100) {
        if (!meta_set_u64(p->db, "last_consumed_offset", next)) {
            ctx->ok = false;
            return false;
        }
        ctx->since_commit = 0;
    }
    return true;
}

uint64_t znam_projection_catch_up(znam_projection_t *p)
{
    if (!p || !p->db || !p->log) return UINT64_MAX;
    int64_t start_ms = now_ms();
    struct catchup_ctx ctx = {
        .p = p,
        .ok = true,
        .next_offset = p->last_consumed_offset,
        .since_commit = 0,
    };

    if (!exec_sql(p->db, "BEGIN IMMEDIATE", "catch_up begin"))
        return UINT64_MAX;
    if (event_log_stream(p->log, p->last_consumed_offset,
                         catchup_cb, &ctx) < 0)
        ctx.ok = false;
    if (ctx.ok)
        ctx.ok = meta_set_u64(p->db, "last_consumed_offset",
                              ctx.next_offset);

    bool finish_ok = exec_sql(p->db, ctx.ok ? "COMMIT" : "ROLLBACK",
                              ctx.ok ? "catch_up commit" :
                                       "catch_up rollback");
    if (!ctx.ok || !finish_ok)
        return UINT64_MAX;
    p->last_consumed_offset = ctx.next_offset;
    int64_t elapsed = now_ms() - start_ms;
    p->last_catch_up_ms = elapsed > 0 ? (uint64_t)elapsed : 0;
    return p->last_consumed_offset;
}

/* ── Read accessors ─────────────────────────────────────────────── */

bool znam_projection_find(znam_projection_t *p, const char *name,
                          char *owner_out, size_t owner_cap,
                          uint8_t *target_type_out,
                          char *target_value_out, size_t target_cap,
                          int32_t *reg_height_out,
                          int32_t *expiry_height_out)
{
    if (!p || !p->db || !name) return false;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT owner_address,target_type,target_value,reg_height,expiry_height"
        " FROM znam_names WHERE name=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    bool found = false;
    if (rc == SQLITE_ROW) {
        const unsigned char *owner = sqlite3_column_text(s, 0);
        if (owner_out && owner_cap)
            snprintf(owner_out, owner_cap, "%s", owner ? (const char *)owner : "");
        if (target_type_out)
            *target_type_out = (uint8_t)sqlite3_column_int(s, 1);
        const unsigned char *tv = sqlite3_column_text(s, 2);
        if (target_value_out && target_cap)
            snprintf(target_value_out, target_cap, "%s",
                     tv ? (const char *)tv : "");
        if (reg_height_out)
            *reg_height_out = sqlite3_column_int(s, 3);
        if (expiry_height_out)
            *expiry_height_out = sqlite3_column_int(s, 4);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

bool znam_projection_addr_get(znam_projection_t *p, const char *name,
                              uint8_t coin_type,
                              char *addr_out, size_t addr_cap)
{
    if (!p || !p->db || !name) return false;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT address FROM znam_addr_records WHERE name=? AND coin_type=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, coin_type);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    bool found = false;
    if (rc == SQLITE_ROW) {
        const unsigned char *a = sqlite3_column_text(s, 0);
        if (addr_out && addr_cap)
            snprintf(addr_out, addr_cap, "%s", a ? (const char *)a : "");
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

bool znam_projection_text_get(znam_projection_t *p, const char *name,
                              const char *key,
                              char *value_out, size_t value_cap)
{
    if (!p || !p->db || !name || !key) return false;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT value FROM znam_text_records WHERE name=? AND key=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    bool found = false;
    if (rc == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(s, 0);
        if (value_out && value_cap)
            snprintf(value_out, value_cap, "%s", v ? (const char *)v : "");
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

static uint64_t count_table(znam_projection_t *p, const char *sql)
{
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    uint64_t n = 0;
    if (sqlite3_prepare_v2(p->db, sql, -1, &s, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        n = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

uint64_t znam_projection_name_count(znam_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM znam_names");
}

uint64_t znam_projection_addr_count(znam_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM znam_addr_records");
}

uint64_t znam_projection_text_count(znam_projection_t *p)
{
    return count_table(p, "SELECT COUNT(*) FROM znam_text_records");
}

/* ── Projection-emit globals ────────────────────────────────────── */

void znam_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

event_log_t *znam_projection_event_log(void)
{
    return atomic_load_explicit(&g_event_log, memory_order_acquire);
}

/* Maximum serialized event payload for any ZNAM event.
 *   REGISTER: 1+63 + 1+64 + 1 + 1+128 + 32 + 4 + 4 + 4 = 303
 *   UPDATE:   1+63 + 1+1+1+32+1+128 + 32 = 260
 * Round up generously to 512 — fits stack, no heap allocations needed.
 */
#define ZNAM_PROJECTION_PAYLOAD_MAX 512

static bool emit_register_internal(event_log_t *log,
                                   const char *name, const char *owner,
                                   uint8_t target_type,
                                   const char *target_value,
                                   const uint8_t reg_txid[32],
                                   int32_t reg_height,
                                   uint32_t registered_unix,
                                   int32_t expiry_height)
{
    struct ev_znam_register ev;
    uint8_t buf[ZNAM_PROJECTION_PAYLOAD_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    if (!name || !owner) return false;
    size_t nlen = strnlen(name, EV_ZNAM_NAME_MAX + 1);
    size_t olen = strnlen(owner, EV_ZNAM_OWNER_MAX + 1);
    if (nlen == 0 || nlen > EV_ZNAM_NAME_MAX) return false;
    if (olen == 0 || olen > EV_ZNAM_OWNER_MAX) return false;
    ev.name_len = (uint8_t)nlen;
    memcpy(ev.name, name, nlen);
    ev.owner_len = (uint8_t)olen;
    memcpy(ev.owner_address, owner, olen);
    ev.target_type = target_type;
    if (target_value) {
        size_t tlen = strnlen(target_value, EV_ZNAM_VALUE_MAX + 1);
        if (tlen > EV_ZNAM_VALUE_MAX) return false;
        ev.target_value_len = (uint8_t)tlen;
        if (tlen) memcpy(ev.target_value, target_value, tlen);
    }
    if (reg_txid) memcpy(ev.reg_txid, reg_txid, 32);
    ev.reg_height = reg_height;
    ev.registered_unix = registered_unix;
    ev.expiry_height = expiry_height;

    if (!ev_znam_register_serialize(&ev, buf, sizeof(buf), &len))
        return false;
    uint64_t off = event_log_append(log, EV_ZNAM_REGISTER, buf, len);
    return off != UINT64_MAX;
}

bool znam_projection_emit_register(const char *name, const char *owner,
                                   uint8_t target_type,
                                   const char *target_value,
                                   const uint8_t reg_txid[32],
                                   int32_t reg_height,
                                   uint32_t registered_unix,
                                   int32_t expiry_height)
{
    event_log_t *log = znam_projection_event_log();
    if (!log) return false;
    if (!emit_register_internal(log, name, owner, target_type, target_value,
                                reg_txid, reg_height, registered_unix,
                                expiry_height)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_register_total, 1,
                              memory_order_relaxed);
    return true;
}

static bool emit_update_internal(event_log_t *log,
                                 const char *name, uint8_t action_type,
                                 uint8_t key_or_coin_type, const char *key,
                                 const char *value,
                                 const uint8_t update_txid[32])
{
    struct ev_znam_update ev;
    uint8_t buf[ZNAM_PROJECTION_PAYLOAD_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    if (!name) return false;
    size_t nlen = strnlen(name, EV_ZNAM_NAME_MAX + 1);
    if (nlen == 0 || nlen > EV_ZNAM_NAME_MAX) return false;
    ev.name_len = (uint8_t)nlen;
    memcpy(ev.name, name, nlen);
    ev.action_type = action_type;
    ev.key_or_coin_type = key_or_coin_type;
    if (key && action_type == EV_ZNAM_UPDATE_ACTION_TEXT) {
        size_t klen = strnlen(key, EV_ZNAM_KEY_MAX + 1);
        if (klen > EV_ZNAM_KEY_MAX) return false;
        ev.key_len = (uint8_t)klen;
        if (klen) memcpy(ev.key, key, klen);
    }
    if (value) {
        size_t vlen = strnlen(value, EV_ZNAM_VALUE_MAX + 1);
        if (vlen > EV_ZNAM_VALUE_MAX) return false;
        ev.value_len = (uint8_t)vlen;
        if (vlen) memcpy(ev.value, value, vlen);
    }
    if (update_txid) memcpy(ev.update_txid, update_txid, 32);

    if (!ev_znam_update_serialize(&ev, buf, sizeof(buf), &len))
        return false;
    uint64_t off = event_log_append(log, EV_ZNAM_UPDATE, buf, len);
    return off != UINT64_MAX;
}

bool znam_projection_emit_update_addr(const char *name, uint8_t coin_type,
                                      const char *address,
                                      const uint8_t update_txid[32])
{
    event_log_t *log = znam_projection_event_log();
    if (!log) return false;
    if (!emit_update_internal(log, name, EV_ZNAM_UPDATE_ACTION_ADDR,
                              coin_type, NULL, address, update_txid)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_update_addr_total, 1,
                              memory_order_relaxed);
    return true;
}

bool znam_projection_emit_update_text(const char *name, const char *key,
                                      const char *value,
                                      const uint8_t update_txid[32])
{
    event_log_t *log = znam_projection_event_log();
    if (!log) return false;
    if (!emit_update_internal(log, name, EV_ZNAM_UPDATE_ACTION_TEXT,
                              0, key, value, update_txid)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_update_text_total, 1,
                              memory_order_relaxed);
    return true;
}

/* ── Diagnostics dump ───────────────────────────────────────────── */

bool znam_projection_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    json_set_object(out);
    znam_projection_t *p = atomic_load_explicit(&g_projection,
                                                memory_order_acquire);
    json_push_kv_bool(out, "open", p != NULL);
    json_push_kv_int(out, "emit_register_total",
        (int64_t)atomic_load_explicit(&g_emit_register_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_update_addr_total",
        (int64_t)atomic_load_explicit(&g_emit_update_addr_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_update_text_total",
        (int64_t)atomic_load_explicit(&g_emit_update_text_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_update_primary_total",
        (int64_t)atomic_load_explicit(&g_emit_update_primary_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_transfer_total",
        (int64_t)atomic_load_explicit(&g_emit_transfer_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_renew_total",
        (int64_t)atomic_load_explicit(&g_emit_renew_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_expire_total",
        (int64_t)atomic_load_explicit(&g_emit_expire_total,
                                      memory_order_relaxed));
    json_push_kv_int(out, "emit_fail_total",
        (int64_t)atomic_load_explicit(&g_emit_fail_total,
                                      memory_order_relaxed));
    if (!p) return true;
    json_push_kv_str(out, "path", p->path);
    json_push_kv_int(out, "last_consumed_offset",
                 (int64_t)p->last_consumed_offset);
    json_push_kv_int(out, "name_count",
                 (int64_t)znam_projection_name_count(p));
    json_push_kv_int(out, "addr_record_count",
                 (int64_t)znam_projection_addr_count(p));
    json_push_kv_int(out, "text_record_count",
                 (int64_t)znam_projection_text_count(p));
    json_push_kv_int(out, "events_consumed_total",
                 (int64_t)p->events_consumed_total);
    json_push_kv_int(out, "ev_register_total",
                 (int64_t)p->register_total);
    json_push_kv_int(out, "ev_update_addr_total",
                 (int64_t)p->update_addr_total);
    json_push_kv_int(out, "ev_update_text_total",
                 (int64_t)p->update_text_total);
    json_push_kv_int(out, "ev_update_primary_total",
                 (int64_t)p->update_primary_total);
    json_push_kv_int(out, "ev_transfer_total",
                 (int64_t)p->transfer_total);
    json_push_kv_int(out, "ev_renew_total",
                 (int64_t)p->renew_total);
    json_push_kv_int(out, "ev_expire_total",
                 (int64_t)p->expire_total);
    json_push_kv_int(out, "last_catch_up_ms", (int64_t)p->last_catch_up_ms);
    return true;
}
