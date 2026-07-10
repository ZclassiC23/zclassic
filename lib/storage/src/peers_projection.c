/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * peers_projection — event-log consumer for rebuildable peer state.
 *
 * Consumes EV_PEER_OBSERVED / EV_PEER_DROPPED into a rebuildable SQLite
 * projection used by diagnostics and replay checks.
 */

#include "storage/peers_projection.h"

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

#define PEERS_PROJECTION_SCHEMA_VERSION 1

struct peers_projection {
    sqlite3 *db;
    event_log_t *log;
    uint64_t last_consumed_offset;
    uint64_t events_consumed_total;
    uint64_t peer_observed_total;
    uint64_t peer_dropped_total;
    uint64_t replace_collisions_total;
    uint64_t last_catch_up_ms;
    char path[1024];
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(peers_projection_t *) g_projection = NULL;
static _Atomic uint64_t g_emit_observed_total = 0;
static _Atomic uint64_t g_emit_dropped_total = 0;
static _Atomic uint64_t g_emit_fail_total = 0;

/* now_ms / apply_pragmas / meta_get_u64 / meta_set_u64 live in
 * storage/projection_util.h. exec_sql stays local for its
 * "[peers_projection]" log prefix. */

static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:peers-projection-sql
                "[peers_projection] %s failed: %s\n",
                ctx, err ? err : sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

static bool ensure_schema(sqlite3 *db)
{
    return exec_sql(db,
        "CREATE TABLE IF NOT EXISTS peers ("
        " ip BLOB NOT NULL,"
        " port INTEGER NOT NULL,"
        " services INTEGER NOT NULL,"
        " last_seen INTEGER NOT NULL,"
        " height_hint INTEGER NOT NULL,"
        " is_onion INTEGER NOT NULL DEFAULT 0,"
        " onion TEXT,"
        " PRIMARY KEY(ip, port)"
        ") WITHOUT ROWID",
        "create peers") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS addresses ("
        " ip BLOB NOT NULL,"
        " port INTEGER NOT NULL,"
        " is_onion INTEGER NOT NULL DEFAULT 0,"
        " onion TEXT,"
        " last_seen INTEGER NOT NULL,"
        " services INTEGER NOT NULL,"
        " height_hint INTEGER NOT NULL,"
        " PRIMARY KEY(ip, port)"
        ") WITHOUT ROWID",
        "create addresses") &&
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

peers_projection_t *peers_projection_open(const char *projection_path,
                                          event_log_t *log)
{
    if (!projection_path || !projection_path[0] || !log) {
        fprintf(stderr,  // obs-ok:peers-projection-open
                "[peers_projection] open: invalid args path=%p log=%p\n",
                (const void *)projection_path, (void *)log);
        return NULL;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(projection_path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr,  // obs-ok:peers-projection-open
                "[peers_projection] sqlite open failed: %s\n",
                db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (!apply_pragmas(db) || !ensure_schema(db)) {
        sqlite3_close(db);
        return NULL;
    }

    peers_projection_t *p = zcl_malloc(sizeof(*p), "peers_projection");
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

void peers_projection_close(peers_projection_t *p)
{
    if (!p) return;
    peers_projection_t *cur = atomic_load_explicit(&g_projection,
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

static bool peer_exists(sqlite3 *db, const uint8_t ip[16], uint16_t port)
{
    sqlite3_stmt *s = NULL;
    bool found = false;
    if (sqlite3_prepare_v2(db,
        "SELECT 1 FROM peers WHERE ip=? AND port=?",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, port);
    found = sqlite3_step(s) == SQLITE_ROW;  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return found;
}

static bool apply_peer_observed(peers_projection_t *p,
                                const struct ev_peer_observed *ev)
{
    if (peer_exists(p->db, ev->ip_v4_or_v6, ev->port))
        p->replace_collisions_total++;

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO peers"
        "(ip,port,services,last_seen,height_hint,is_onion,onion)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)ev->services_bitmap);
    sqlite3_bind_int64(s, 4, ev->observed_unix);
    sqlite3_bind_int(s, 5, ev->height_hint);
    sqlite3_bind_int(s, 6, ev->is_onion ? 1 : 0);
    if (ev->onion_len)
        sqlite3_bind_text(s, 7, ev->onion, ev->onion_len,
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 7);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return false;

    rc = sqlite3_prepare_v2(p->db,
        "INSERT OR REPLACE INTO addresses"
        "(ip,port,is_onion,onion,last_seen,services,height_hint)"
        " VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    sqlite3_bind_int(s, 3, ev->is_onion ? 1 : 0);
    if (ev->onion_len)
        sqlite3_bind_text(s, 4, ev->onion, ev->onion_len,
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 4);
    sqlite3_bind_int64(s, 5, ev->observed_unix);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)ev->services_bitmap);
    sqlite3_bind_int(s, 7, ev->height_hint);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_peer_dropped(peers_projection_t *p,
                               const struct ev_peer_dropped *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "DELETE FROM peers WHERE ip=? AND port=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

struct catchup_ctx {
    peers_projection_t *p;
    bool ok;
    uint64_t next_offset;
    uint64_t since_commit;
};

static bool catchup_cb(uint64_t offset, enum event_log_type type,
                       const void *payload, size_t len, void *user)
{
    struct catchup_ctx *ctx = user;
    peers_projection_t *p = ctx->p;
    uint64_t next = offset + EVENT_LOG_FRAME_OVERHEAD + (uint64_t)len;

    if (type == EV_PEER_OBSERVED) {
        struct ev_peer_observed ev;
        if (!ev_peer_observed_parse(payload, len, &ev) ||
            !apply_peer_observed(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->peer_observed_total++;
        p->events_consumed_total++;
    } else if (type == EV_PEER_DROPPED) {
        struct ev_peer_dropped ev;
        if (!ev_peer_dropped_parse(payload, len, &ev) ||
            !apply_peer_dropped(p, &ev)) {
            ctx->ok = false;
            return false;
        }
        p->peer_dropped_total++;
        p->events_consumed_total++;
    }

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

uint64_t peers_projection_catch_up(peers_projection_t *p)
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
    if (!ctx.ok || !finish_ok) {
        /* Rolled back — restore the cached offset from persisted meta;
           SQLite discarded the in-flight writes on ROLLBACK. Without this,
           the catchup_cb's in-flight advance leaks and the next catch_up
           skips events. */
        p->last_consumed_offset = meta_get_u64(p->db, "last_consumed_offset");
        return UINT64_MAX;
    }
    p->last_consumed_offset = ctx.next_offset;
    int64_t elapsed = now_ms() - start_ms;
    p->last_catch_up_ms = elapsed > 0 ? (uint64_t)elapsed : 0;
    return p->last_consumed_offset;
}

bool peers_projection_get(peers_projection_t *p,
                          const uint8_t ip[16], uint16_t port,
                          uint64_t *services_out,
                          int64_t *last_seen_out,
                          int32_t *height_hint_out)
{
    if (!p || !p->db || !ip) return false;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(p->db,
        "SELECT services,last_seen,height_hint FROM peers"
        " WHERE ip=? AND port=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, port);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    if (rc == SQLITE_ROW) {
        if (services_out)
            *services_out = (uint64_t)sqlite3_column_int64(s, 0);
        if (last_seen_out)
            *last_seen_out = sqlite3_column_int64(s, 1);
        if (height_hint_out)
            *height_hint_out = sqlite3_column_int(s, 2);
        sqlite3_finalize(s);
        return true;
    }
    sqlite3_finalize(s);
    return false;
}

uint64_t peers_projection_count(peers_projection_t *p)
{
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    uint64_t n = 0;
    if (sqlite3_prepare_v2(p->db, "SELECT COUNT(*) FROM peers",
                           -1, &s, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        n = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

void peers_projection_set_event_log(event_log_t *log)
{
    atomic_store_explicit(&g_event_log, log, memory_order_release);
}

event_log_t *peers_projection_event_log(void)
{
    return atomic_load_explicit(&g_event_log, memory_order_acquire);
}

bool peers_projection_emit_observed(const uint8_t ip[16], uint16_t port,
                                    uint64_t services, int64_t observed_unix,
                                    int32_t height_hint)
{
    event_log_t *log = peers_projection_event_log();
    if (!log || !ip) return false;
    struct ev_peer_observed ev;
    uint8_t buf[EV_PEER_OBSERVED_FIXED_LEN + EV_PEER_ONION_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.services_bitmap = services;
    ev.observed_unix = observed_unix > 0 ? (uint32_t)observed_unix : 0;
    ev.height_hint = height_hint;
    if (!ev_peer_observed_serialize(&ev, buf, sizeof(buf), &len)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_PEER_OBSERVED, buf, len);
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_observed_total, 1,
                              memory_order_relaxed);
    return true;
}

bool peers_projection_emit_dropped(const uint8_t ip[16], uint16_t port,
                                   uint8_t reason)
{
    event_log_t *log = peers_projection_event_log();
    if (!log || !ip) return false;
    struct ev_peer_dropped ev;
    uint8_t buf[EV_PEER_DROPPED_LEN];
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.reason = reason;
    if (!ev_peer_dropped_serialize(&ev, buf)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_PEER_DROPPED, buf, sizeof(buf));
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1,
                                  memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_dropped_total, 1,
                              memory_order_relaxed);
    return true;
}

bool peers_projection_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    json_set_object(out);
    peers_projection_t *p = atomic_load_explicit(&g_projection,
                                                 memory_order_acquire);
    json_push_kv_bool(out, "open", p != NULL);
    json_push_kv_int(out, "emit_observed_total",
                 (int64_t)atomic_load_explicit(&g_emit_observed_total,
                                               memory_order_relaxed));
    json_push_kv_int(out, "emit_dropped_total",
                 (int64_t)atomic_load_explicit(&g_emit_dropped_total,
                                               memory_order_relaxed));
    json_push_kv_int(out, "emit_fail_total",
                 (int64_t)atomic_load_explicit(&g_emit_fail_total,
                                               memory_order_relaxed));

    /* Reserved `_health` key (see docs/work "Adding state introspection" +
     * app/controllers/src/diagnostics_health_rollup.c): { ok, reason }.
     * Maps the already-computed open + emit_fail_total fields above — no
     * new health logic. */
    {
        uint64_t fails = atomic_load_explicit(&g_emit_fail_total,
                                              memory_order_relaxed);
        bool ok = p != NULL && fails == 0;
        struct json_value health = {0};
        json_set_object(&health);
        json_push_kv_bool(&health, "ok", ok);
        char reason_buf[128] = "";
        if (!p)
            snprintf(reason_buf, sizeof(reason_buf),
                     "peers_projection not open");
        else if (fails > 0)
            snprintf(reason_buf, sizeof(reason_buf),
                     "peers_projection emit_fail_total=%llu",
                     (unsigned long long)fails);
        json_push_kv_str(&health, "reason", reason_buf);
        json_push_kv(out, "_health", &health);
        json_free(&health);
    }
    if (!p) return true;
    json_push_kv_str(out, "path", p->path);
    json_push_kv_int(out, "last_consumed_offset",
                 (int64_t)p->last_consumed_offset);
    json_push_kv_int(out, "peer_count",
                 (int64_t)peers_projection_count(p));
    json_push_kv_int(out, "events_consumed_total",
                 (int64_t)p->events_consumed_total);
    json_push_kv_int(out, "ev_peer_observed_total",
                 (int64_t)p->peer_observed_total);
    json_push_kv_int(out, "ev_peer_dropped_total",
                 (int64_t)p->peer_dropped_total);
    json_push_kv_int(out, "replace_collisions_total",
                 (int64_t)p->replace_collisions_total);
    json_push_kv_int(out, "last_catch_up_ms", (int64_t)p->last_catch_up_ms);
    return true;
}
