/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * peers_projection — event-log consumer for rebuildable peer state.
 *
 * Folds EV_PEER_OBSERVED / EV_PEER_DROPPED into a rebuildable SQLite
 * projection. The cursor/transaction/projection_meta plumbing lives in
 * storage/projection_consumer.c — this file owns only the peer domain
 * schema and per-event apply logic.
 */

#include "storage/peers_projection.h"

#include "json/json.h"
#include "storage/event_log_payloads.h"
#include "storage/projection_consumer.h"
#include "storage/projection_util.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define PEERS_PROJECTION_SCHEMA_VERSION 1

struct peers_projection {
    projection_consumer_t *pc;
    sqlite3 *db;  /* == projection_consumer_db(pc); cached for read accessors */
    uint64_t peer_observed_total;
    uint64_t peer_dropped_total;
    uint64_t replace_collisions_total;
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(peers_projection_t *) g_projection = NULL;
static _Atomic uint64_t g_emit_observed_total = 0;
static _Atomic uint64_t g_emit_dropped_total = 0;
static _Atomic uint64_t g_emit_fail_total = 0;

static bool apply_event(sqlite3 *db, enum event_log_type type,
                        const void *payload, size_t len, void *ctx,
                        bool *out_handled);

/* Shared exec-and-log body; also satisfies projection_util.h's exec_sql decl. */
static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    return projection_consumer_exec_sql(db, "peers_projection", sql, ctx);
}

static bool ensure_schema(sqlite3 *db, void *ctx)
{
    (void)ctx;
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
        "create addresses");
}

peers_projection_t *peers_projection_open(const char *projection_path,
                                          event_log_t *log)
{
    if (!projection_path || !projection_path[0] || !log)
        return NULL;  /* projection_consumer_open() logs the details */

    peers_projection_t *p = zcl_malloc(sizeof(*p), "peers_projection");
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));

    struct projection_consumer_spec spec = {
        .schema_version = PEERS_PROJECTION_SCHEMA_VERSION,
        .ensure_schema = ensure_schema,
        .apply_event = apply_event,
        .ctx = p,
        .commit_batch = 0,
    };
    p->pc = projection_consumer_open(projection_path, log, &spec);
    if (!p->pc) {
        free(p);
        return NULL;
    }
    p->db = projection_consumer_db(p->pc);
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
    projection_consumer_close(p->pc);
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

static bool apply_peer_observed(sqlite3 *db, peers_projection_t *p,
                                const struct ev_peer_observed *ev)
{
    if (peer_exists(db, ev->ip_v4_or_v6, ev->port))
        p->replace_collisions_total++;

    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
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

    rc = sqlite3_prepare_v2(db,
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

static bool apply_peer_dropped(sqlite3 *db, const struct ev_peer_dropped *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM peers WHERE ip=? AND port=?",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_event(sqlite3 *db, enum event_log_type type,
                        const void *payload, size_t len, void *ctx,
                        bool *out_handled)
{
    peers_projection_t *p = ctx;
    *out_handled = false;

    if (type == EV_PEER_OBSERVED) {
        struct ev_peer_observed ev;
        if (!ev_peer_observed_parse(payload, len, &ev) ||
            !apply_peer_observed(db, p, &ev))
            return false;
        p->peer_observed_total++;
        *out_handled = true;
        return true;
    }
    if (type == EV_PEER_DROPPED) {
        struct ev_peer_dropped ev;
        if (!ev_peer_dropped_parse(payload, len, &ev) ||
            !apply_peer_dropped(db, &ev))
            return false;
        p->peer_dropped_total++;
        *out_handled = true;
        return true;
    }
    return true;  /* unrecognized event type: skip past it */
}

uint64_t peers_projection_catch_up(peers_projection_t *p)
{
    if (!p) return UINT64_MAX;
    return projection_consumer_catch_up(p->pc);
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

    /* Reserved `_health` key: { ok, reason } from open + emit_fail_total. */
    {
        uint64_t fails = atomic_load_explicit(&g_emit_fail_total,
                                              memory_order_relaxed);
        projection_push_health(out, "peers_projection", p, fails);
    }
    if (!p) return true;
    projection_consumer_dump_common(out, p->pc);
    json_push_kv_int(out, "peer_count",
                 (int64_t)peers_projection_count(p));
    json_push_kv_int(out, "ev_peer_observed_total",
                 (int64_t)p->peer_observed_total);
    json_push_kv_int(out, "ev_peer_dropped_total",
                 (int64_t)p->peer_dropped_total);
    json_push_kv_int(out, "replace_collisions_total",
                 (int64_t)p->replace_collisions_total);
    return true;
}
