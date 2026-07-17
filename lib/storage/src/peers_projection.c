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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* v2 adds the durable reputation columns on `addresses` + the append-only
 * peer_sessions / fork_events ledgers (NET-2 "bank everything, never
 * relearn"). Additive/idempotent — a v1 DB is migrated in place by
 * ensure_schema; the projection is rebuildable from the event log regardless. */
#define PEERS_PROJECTION_SCHEMA_VERSION 2

/* Append-only ledgers are retention-capped (delete-oldest past the cap). */
#define PEER_SESSIONS_RETENTION_CAP 50000
#define FORK_EVENTS_RETENTION_CAP   50000

/* Stringize the numeric caps into their retention SQL literals. */
#define PP_STR_(x) #x
#define PP_STR(x)  PP_STR_(x)

struct peers_projection {
    projection_consumer_t *pc;
    sqlite3 *db;  /* == projection_consumer_db(pc); cached for read accessors */
    uint64_t peer_observed_total;
    uint64_t peer_dropped_total;
    uint64_t replace_collisions_total;
    uint64_t session_closed_total;
    uint64_t fork_observed_total;
};

static _Atomic(event_log_t *) g_event_log = NULL;
static _Atomic(peers_projection_t *) g_projection = NULL;
static _Atomic uint64_t g_emit_observed_total = 0;
static _Atomic uint64_t g_emit_dropped_total = 0;
static _Atomic uint64_t g_emit_session_closed_total = 0;
static _Atomic uint64_t g_emit_fork_observed_total = 0;
static _Atomic uint64_t g_emit_fail_total = 0;

/* Runtime ledger retention caps (default to the compile-time constants; a
 * ZCL_TESTING hook lowers them so the delete-oldest path is provable without
 * inserting 50k rows). */
static _Atomic uint64_t g_peer_sessions_cap = PEER_SESSIONS_RETENTION_CAP;
static _Atomic uint64_t g_fork_events_cap = FORK_EVENTS_RETENTION_CAP;

static bool apply_event(sqlite3 *db, enum event_log_type type,
                        const void *payload, size_t len, void *ctx,
                        bool *out_handled);

/* Shared exec-and-log body; also satisfies projection_util.h's exec_sql decl. */
static bool exec_sql(sqlite3 *db, const char *sql, const char *ctx)
{
    return projection_consumer_exec_sql(db, "peers_projection", sql, ctx);
}

/* Idempotent ADD COLUMN: SQLite has no "ADD COLUMN IF NOT EXISTS", so probe
 * pragma table_info first. Handles both a fresh v2 DB (column already present
 * from a prior ensure_column in this same open) and a v1 DB being migrated. */
static bool ensure_column(sqlite3 *db, const char *table, const char *col,
                          const char *decl)
{
    sqlite3_stmt *s = NULL;
    char pragma[128];
    snprintf(pragma, sizeof(pragma), "PRAGMA table_info(%s)", table);
    if (sqlite3_prepare_v2(db, pragma, -1, &s, NULL) != SQLITE_OK)
        return false;
    bool present = false;
    while (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:projection-primitive
        const unsigned char *name = sqlite3_column_text(s, 1);
        if (name && strcmp((const char *)name, col) == 0) {
            present = true;
            break;
        }
    }
    sqlite3_finalize(s);
    if (present)
        return true;
    char alter[192];
    snprintf(alter, sizeof(alter), "ALTER TABLE %s ADD COLUMN %s %s",
             table, col, decl);
    return exec_sql(db, alter, "add reputation column");
}

static bool ensure_schema(sqlite3 *db, void *ctx)
{
    (void)ctx;
    if (!exec_sql(db,
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
        "create peers") ||
        !exec_sql(db,
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
        "create addresses"))
        return false;

    /* v2 additive reputation columns on the durable `addresses` book. */
    if (!ensure_column(db, "addresses", "bandwidth_score",
                       "INTEGER NOT NULL DEFAULT 0") ||
        !ensure_column(db, "addresses", "avg_latency_us",
                       "INTEGER NOT NULL DEFAULT 0") ||
        !ensure_column(db, "addresses", "sessions_count",
                       "INTEGER NOT NULL DEFAULT 0") ||
        !ensure_column(db, "addresses", "last_useful_time",
                       "INTEGER NOT NULL DEFAULT 0") ||
        !ensure_column(db, "addresses", "headers_delivered",
                       "INTEGER NOT NULL DEFAULT 0") ||
        !ensure_column(db, "addresses", "blocks_delivered",
                       "INTEGER NOT NULL DEFAULT 0"))
        return false;

    /* Append-only durable session + fork ledgers (retention-capped). */
    return exec_sql(db,
        "CREATE TABLE IF NOT EXISTS peer_sessions ("
        " seq INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ip BLOB NOT NULL,"
        " port INTEGER NOT NULL,"
        " closed_unix INTEGER NOT NULL,"
        " duration_secs INTEGER NOT NULL,"
        " bytes_in INTEGER NOT NULL,"
        " bytes_out INTEGER NOT NULL,"
        " headers_delivered INTEGER NOT NULL,"
        " blocks_delivered INTEGER NOT NULL,"
        " reason INTEGER NOT NULL,"
        " bandwidth_score INTEGER NOT NULL,"
        " avg_latency_us INTEGER NOT NULL"
        ")",
        "create peer_sessions") &&
        exec_sql(db,
        "CREATE TABLE IF NOT EXISTS fork_events ("
        " seq INTEGER PRIMARY KEY AUTOINCREMENT,"
        " observed_unix INTEGER NOT NULL,"
        " height INTEGER NOT NULL,"
        " num_clusters INTEGER NOT NULL,"
        " count_a INTEGER NOT NULL,"
        " count_b INTEGER NOT NULL,"
        " tip_hash_a TEXT,"
        " tip_hash_b TEXT"
        ")",
        "create fork_events");
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

    /* UPSERT (not INSERT OR REPLACE) so the durable reputation columns folded
     * from EV_PEER_SESSION_CLOSED survive a re-observation of the same peer. */
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO addresses"
        "(ip,port,is_onion,onion,last_seen,services,height_hint)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(ip,port) DO UPDATE SET"
        "  is_onion=excluded.is_onion, onion=excluded.onion,"
        "  last_seen=excluded.last_seen, services=excluded.services,"
        "  height_hint=excluded.height_hint",
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

/* Fold a closed session: append the durable ledger row (retention-capped) and
 * accumulate the peer's durable reputation on `addresses`. */
static bool apply_session_closed(sqlite3 *db,
                                 const struct ev_peer_session_closed *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO peer_sessions"
        "(ip,port,closed_unix,duration_secs,bytes_in,bytes_out,"
        " headers_delivered,blocks_delivered,reason,bandwidth_score,"
        " avg_latency_us)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    sqlite3_bind_int64(s, 3, ev->last_useful_time);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)ev->duration_secs);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)ev->bytes_in);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)ev->bytes_out);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)ev->headers_delivered);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)ev->blocks_delivered);
    sqlite3_bind_int(s, 9, ev->reason);
    sqlite3_bind_int64(s, 10, (sqlite3_int64)ev->bandwidth_score);
    sqlite3_bind_int64(s, 11, ev->avg_latency_us);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return false;

    /* Retention: keep the newest N rows (delete-oldest past the cap). On an
     * empty table MAX(seq) is NULL, so `seq <= NULL` matches nothing — safe. */
    {
        char prune[128];
        snprintf(prune, sizeof(prune),
            "DELETE FROM peer_sessions WHERE seq <= "
            "(SELECT MAX(seq) FROM peer_sessions) - %llu",
            (unsigned long long)atomic_load_explicit(&g_peer_sessions_cap,
                                                     memory_order_relaxed));
        if (!exec_sql(db, prune, "prune peer_sessions"))
            return false;
    }

    /* Durable reputation on the address book (accumulate delivery totals,
     * bump session count, track the freshest useful time). */
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO addresses"
        "(ip,port,is_onion,onion,last_seen,services,height_hint,"
        " bandwidth_score,avg_latency_us,sessions_count,last_useful_time,"
        " headers_delivered,blocks_delivered)"
        " VALUES(?,?,0,NULL,?,0,-1,?,?,1,?,?,?)"
        " ON CONFLICT(ip,port) DO UPDATE SET"
        "  bandwidth_score=excluded.bandwidth_score,"
        "  avg_latency_us=excluded.avg_latency_us,"
        "  sessions_count=addresses.sessions_count+1,"
        "  last_useful_time=MAX(addresses.last_useful_time,"
        "                       excluded.last_useful_time),"
        "  headers_delivered=addresses.headers_delivered+"
        "                    excluded.headers_delivered,"
        "  blocks_delivered=addresses.blocks_delivered+"
        "                   excluded.blocks_delivered",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_blob(s, 1, ev->ip_v4_or_v6, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ev->port);
    sqlite3_bind_int64(s, 3, ev->last_useful_time);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)ev->bandwidth_score);
    sqlite3_bind_int64(s, 5, ev->avg_latency_us);
    sqlite3_bind_int64(s, 6, ev->last_useful_time);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)ev->headers_delivered);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)ev->blocks_delivered);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}

static bool apply_fork_observed(sqlite3 *db,
                                const struct ev_net_fork_observed *ev)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO fork_events"
        "(observed_unix,height,num_clusters,count_a,count_b,"
        " tip_hash_a,tip_hash_b) VALUES(?,?,?,?,?,?,?)",
        -1, &s, NULL);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_int64(s, 1, ev->observed_unix);
    sqlite3_bind_int64(s, 2, ev->height);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)ev->num_clusters);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)ev->count_a);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)ev->count_b);
    if (ev->hash_a_len)
        sqlite3_bind_text(s, 6, ev->tip_hash_a, ev->hash_a_len,
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 6);
    if (ev->hash_b_len)
        sqlite3_bind_text(s, 7, ev->tip_hash_b, ev->hash_b_len,
                          SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, 7);
    rc = sqlite3_step(s);  // raw-sql-ok:projection-primitive
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return false;

    {
        char prune[128];
        snprintf(prune, sizeof(prune),
            "DELETE FROM fork_events WHERE seq <= "
            "(SELECT MAX(seq) FROM fork_events) - %llu",
            (unsigned long long)atomic_load_explicit(&g_fork_events_cap,
                                                     memory_order_relaxed));
        return exec_sql(db, prune, "prune fork_events");
    }
}

static bool apply_event(sqlite3 *db, enum event_log_type type,
                        const void *payload, size_t len, void *ctx,
                        bool *out_handled)
{
    peers_projection_t *p = ctx;
    *out_handled = false;

    if (type == EV_PEER_SESSION_CLOSED) {
        struct ev_peer_session_closed ev;
        if (!ev_peer_session_closed_parse(payload, len, &ev) ||
            !apply_session_closed(db, &ev))
            return false;
        p->session_closed_total++;
        *out_handled = true;
        return true;
    }
    if (type == EV_NET_FORK_OBSERVED) {
        struct ev_net_fork_observed ev;
        if (!ev_net_fork_observed_parse(payload, len, &ev) ||
            !apply_fork_observed(db, &ev))
            return false;
        p->fork_observed_total++;
        *out_handled = true;
        return true;
    }
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

bool peers_projection_emit_session_closed(const uint8_t ip[16], uint16_t port,
                                          uint8_t reason,
                                          uint32_t duration_secs,
                                          uint64_t bytes_in, uint64_t bytes_out,
                                          uint64_t headers_delivered,
                                          uint64_t blocks_delivered,
                                          uint32_t bandwidth_score,
                                          int64_t avg_latency_us,
                                          int64_t last_useful_time)
{
    event_log_t *log = peers_projection_event_log();
    if (!log || !ip) return false;
    struct ev_peer_session_closed ev;
    uint8_t buf[EV_PEER_SESSION_CLOSED_LEN];
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.reason = reason;
    ev.duration_secs = duration_secs;
    ev.bytes_in = bytes_in;
    ev.bytes_out = bytes_out;
    ev.headers_delivered = headers_delivered;
    ev.blocks_delivered = blocks_delivered;
    ev.bandwidth_score = bandwidth_score;
    ev.avg_latency_us = avg_latency_us;
    ev.last_useful_time = last_useful_time;
    if (!ev_peer_session_closed_serialize(&ev, buf)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1, memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_PEER_SESSION_CLOSED, buf,
                                    sizeof(buf));
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1, memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_session_closed_total, 1,
                              memory_order_relaxed);
    return true;
}

bool peers_projection_emit_fork_observed(int64_t height, int64_t observed_unix,
                                         uint32_t num_clusters,
                                         uint32_t count_a, uint32_t count_b,
                                         const char *tip_hash_a,
                                         const char *tip_hash_b)
{
    event_log_t *log = peers_projection_event_log();
    if (!log) return false;
    struct ev_net_fork_observed ev;
    uint8_t buf[EV_NET_FORK_OBSERVED_FIXED_LEN + 2 * EV_NET_FORK_TIP_HEX_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    ev.height = height;
    ev.observed_unix = observed_unix;
    ev.num_clusters = num_clusters;
    ev.count_a = count_a;
    ev.count_b = count_b;
    if (tip_hash_a && tip_hash_a[0]) {
        size_t n = strlen(tip_hash_a);
        if (n > EV_NET_FORK_TIP_HEX_MAX) n = EV_NET_FORK_TIP_HEX_MAX;
        memcpy(ev.tip_hash_a, tip_hash_a, n);
        ev.hash_a_len = (uint8_t)n;
    }
    if (tip_hash_b && tip_hash_b[0]) {
        size_t n = strlen(tip_hash_b);
        if (n > EV_NET_FORK_TIP_HEX_MAX) n = EV_NET_FORK_TIP_HEX_MAX;
        memcpy(ev.tip_hash_b, tip_hash_b, n);
        ev.hash_b_len = (uint8_t)n;
    }
    if (!ev_net_fork_observed_serialize(&ev, buf, sizeof(buf), &len)) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1, memory_order_relaxed);
        return false;
    }
    uint64_t off = event_log_append(log, EV_NET_FORK_OBSERVED, buf, len);
    if (off == UINT64_MAX) {
        atomic_fetch_add_explicit(&g_emit_fail_total, 1, memory_order_relaxed);
        return false;
    }
    atomic_fetch_add_explicit(&g_emit_fork_observed_total, 1,
                              memory_order_relaxed);
    return true;
}

/* Read the durable reputation columns for one address. Fail-open: a missing
 * row / unopened projection / bad args leaves *out zeroed and returns false. */
static bool read_reputation_row(sqlite3 *db, const uint8_t ip[16],
                                uint16_t port, struct peer_reputation *out)
{
    memset(out, 0, sizeof(*out));
    if (!db || !ip) return false;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT bandwidth_score,avg_latency_us,sessions_count,"
        " last_useful_time,headers_delivered,blocks_delivered"
        " FROM addresses WHERE ip=? AND port=?",
        -1, &s, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(s, 1, ip, 16, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, port);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:projection-primitive
        out->bandwidth_score = (uint32_t)sqlite3_column_int64(s, 0);
        out->avg_latency_us = sqlite3_column_int64(s, 1);
        out->sessions_count = sqlite3_column_int64(s, 2);
        out->last_useful_time = sqlite3_column_int64(s, 3);
        out->headers_delivered = sqlite3_column_int64(s, 4);
        out->blocks_delivered = sqlite3_column_int64(s, 5);
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

bool peers_projection_get_reputation(peers_projection_t *p,
                                     const uint8_t ip[16], uint16_t port,
                                     struct peer_reputation *out)
{
    if (!out) return false;
    if (!p || !p->db) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return read_reputation_row(p->db, ip, port, out);
}

bool peers_projection_get_reputation_global(const uint8_t ip[16], uint16_t port,
                                            struct peer_reputation *out)
{
    if (!out) return false;
    peers_projection_t *p = atomic_load_explicit(&g_projection,
                                                 memory_order_acquire);
    return peers_projection_get_reputation(p, ip, port, out);
}

size_t peers_projection_for_each_reputation_global(size_t max,
                                                   peers_reputation_cb cb,
                                                   void *ctx)
{
    if (!cb || max == 0) return 0;
    peers_projection_t *p = atomic_load_explicit(&g_projection,
                                                 memory_order_acquire);
    if (!p || !p->db) return 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(p->db,
        "SELECT ip,port,bandwidth_score,avg_latency_us,sessions_count,"
        " last_useful_time,headers_delivered,blocks_delivered"
        " FROM addresses WHERE sessions_count>0"
        " ORDER BY last_useful_time DESC LIMIT ?",
        -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)(max > INT64_MAX ? INT64_MAX
                                                             : (int64_t)max));
    size_t visited = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {  // raw-sql-ok:projection-primitive
        const void *ipblob = sqlite3_column_blob(s, 0);
        int iplen = sqlite3_column_bytes(s, 0);
        if (!ipblob || iplen != 16)
            continue;
        uint8_t ip[16];
        memcpy(ip, ipblob, 16);
        uint16_t port = (uint16_t)sqlite3_column_int(s, 1);
        struct peer_reputation rep;
        memset(&rep, 0, sizeof(rep));
        rep.bandwidth_score = (uint32_t)sqlite3_column_int64(s, 2);
        rep.avg_latency_us = sqlite3_column_int64(s, 3);
        rep.sessions_count = sqlite3_column_int64(s, 4);
        rep.last_useful_time = sqlite3_column_int64(s, 5);
        rep.headers_delivered = sqlite3_column_int64(s, 6);
        rep.blocks_delivered = sqlite3_column_int64(s, 7);
        cb(ip, port, &rep, ctx);
        visited++;
    }
    sqlite3_finalize(s);
    return visited;
}

/* Count rows in a ledger table (introspection only). */
static int64_t ledger_count(sqlite3 *db, const char *table)
{
    if (!db) return 0;
    sqlite3_stmt *s = NULL;
    char sql[64];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return 0;
    int64_t n = 0;
    if (sqlite3_step(s) == SQLITE_ROW)  // raw-sql-ok:projection-primitive
        n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
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
    json_push_kv_int(out, "emit_session_closed_total",
                 (int64_t)atomic_load_explicit(&g_emit_session_closed_total,
                                               memory_order_relaxed));
    json_push_kv_int(out, "emit_fork_observed_total",
                 (int64_t)atomic_load_explicit(&g_emit_fork_observed_total,
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
    json_push_kv_int(out, "ev_session_closed_total",
                 (int64_t)p->session_closed_total);
    json_push_kv_int(out, "ev_fork_observed_total",
                 (int64_t)p->fork_observed_total);
    json_push_kv_int(out, "peer_sessions_rows",
                 ledger_count(p->db, "peer_sessions"));
    json_push_kv_int(out, "fork_events_rows",
                 ledger_count(p->db, "fork_events"));
    json_push_kv_int(out, "replace_collisions_total",
                 (int64_t)p->replace_collisions_total);
    return true;
}

#ifdef ZCL_TESTING
void peers_projection_test_set_retention_caps(uint64_t sessions_cap,
                                              uint64_t forks_cap)
{
    atomic_store_explicit(&g_peer_sessions_cap, sessions_cap,
                          memory_order_relaxed);
    atomic_store_explicit(&g_fork_events_cap, forks_cap, memory_order_relaxed);
}

void peers_projection_test_reset_retention_caps(void)
{
    atomic_store_explicit(&g_peer_sessions_cap, PEER_SESSIONS_RETENTION_CAP,
                          memory_order_relaxed);
    atomic_store_explicit(&g_fork_events_cap, FORK_EVENTS_RETENTION_CAP,
                          memory_order_relaxed);
}

int64_t peers_projection_test_ledger_count(peers_projection_t *p,
                                           const char *table)
{
    if (!p || !table) return -1;
    return ledger_count(p->db, table);
}
#endif
