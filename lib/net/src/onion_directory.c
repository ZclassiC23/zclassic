/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * onion_directory — the peer directory this node publishes at
 * /directory.json, and the onion graph it learns from other nodes'.
 *
 * Split out of onion_service.c, which owns the onion FRONT DOOR (request
 * routing and page chrome). This file owns the DATA behind two of those
 * routes: the peer_directory table, its freshness rules, the supervised
 * refresh round that keeps it current, and the parser + bounds for the
 * transitive discovery half. The two serve_directory_* renderers stay
 * with the chrome they share; they read the freshness rule from here.
 *
 * The whole contract, including what a directory record IS and is not,
 * is in net/onion_service.h. The one-line version: a record is a hint
 * about where to look, never proof of who is there, so nothing in this
 * file may ever REMOVE a peer from any other source's reach. */

#include "platform/time_compat.h"
#include "net/onion_service.h"
#include "net/onion_peer_merge.h"
#include "util/log_json.h"
#include "util/log_macros.h"
#include "util/path_check.h"
#include "util/supervisor.h"
#include "util/ar_step_readonly.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#define ODIR_LOG "net.onion_directory"

static void ensure_directory_table(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS peer_directory ("
        "onion_address TEXT PRIMARY KEY,"
        "port INTEGER NOT NULL DEFAULT 8033,"
        "services INTEGER NOT NULL DEFAULT 0,"
        "height INTEGER NOT NULL DEFAULT 0,"
        "last_seen INTEGER NOT NULL,"
        "version TEXT,"
        "self INTEGER NOT NULL DEFAULT 0,"
        "clearnet_ip TEXT DEFAULT '',"
        "clearnet_port INTEGER DEFAULT 0"
        ")", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "onion_service: failed to create directory table: %s\n",  // obs-ok:pre-existing-diagnostic
                err ? err : "unknown");
        sqlite3_free(err);
    }
    /* Add clearnet columns to existing databases */
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN clearnet_ip TEXT DEFAULT ''",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN clearnet_port INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
    /* Freshness columns. first_seen/last_probe/probe_ok/fail_count make a
     * row's history readable; `source` says whether we measured it
     * ourselves or another node told us about it. Adding a column that is
     * already present is an expected no-op here, exactly like the two
     * clearnet ALTERs above. */
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN first_seen INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN last_probe INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN probe_ok INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN fail_count INTEGER DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN source TEXT DEFAULT ''",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_peer_directory_last_seen "
                     "ON peer_directory(last_seen)", NULL, NULL, NULL);
}

/* ── Freshness: one pure rule, no database ─────────────────── */

int64_t onion_directory_age_secs(int64_t last_seen, int64_t now)
{
    int64_t age = now - last_seen;
    return age < 0 ? 0 : age;
}

enum onion_dir_freshness onion_directory_freshness(int64_t last_seen,
                                                   int64_t now, bool self)
{
    /* Our own presence is not hearsay — we are the node. */
    if (self)
        return ONION_DIR_FRESH;
    /* No stamp at all is no provenance at all. */
    if (last_seen <= 0)
        return ONION_DIR_EXPIRED;
    int64_t age = onion_directory_age_secs(last_seen, now);
    if (age < ONION_DIR_STALE_SECS)
        return ONION_DIR_FRESH;
    if (age < ONION_DIR_EXPIRE_SECS)
        return ONION_DIR_STALE;
    return ONION_DIR_EXPIRED;
}

/* Populate directory from the discovery sources (signed descriptors +
 * the on-chain/wallet scrape, merged by onion_peers_collect).
 *
 * This is an UPSERT, not the old INSERT OR IGNORE: a peer the sources
 * still announce is a peer we still have evidence for, so its last_seen
 * moves forward. Without that, every row froze at its first sighting and
 * the whole table aged out or, worse, was served forever as if current.
 * Returns the number of rows inserted or refreshed. */
static int populate_directory_from_chain(sqlite3 *db)
{
    if (!onion_service_datadir()) return 0;

    struct onion_peer peers[256];
    int found = onion_service_discover_peers(peers, 256);

    if (found <= 0) return 0;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO peer_directory "
        "(onion_address, height, first_seen, last_seen, version, source) "
        "VALUES (?, ?, ?, ?, 'chain', 'discovery') "
        "ON CONFLICT(onion_address) DO UPDATE SET "
        "  last_seen = excluded.last_seen,"
        "  height = MAX(peer_directory.height, excluded.height),"
        "  fail_count = 0",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_WARN(ODIR_LOG, "failed to prepare peer upsert: %s", sqlite3_errmsg(db));
        return 0;
    }

    int64_t now = (int64_t)platform_time_wall_time_t();
    int touched = 0;
    for (int i = 0; i < found; i++) {
        if (!peers[i].hostname[0]) continue;
        sqlite3_reset(ins);
        sqlite3_bind_text(ins, 1, peers[i].hostname, -1, SQLITE_STATIC);
        sqlite3_bind_int(ins, 2, peers[i].height);
        sqlite3_bind_int64(ins, 3, now);
        sqlite3_bind_int64(ins, 4, now);
        if (AR_STEP_WRITE(ins) == SQLITE_DONE)
            touched++;
    }
    sqlite3_finalize(ins);

    log_jsonf(LOG_JSON_INFO, "onion_directory_loaded",
              "\"peers_loaded\":%d,\"rows_touched\":%d", found, touched);
    return touched;
}

/* Drop rows nothing has confirmed for ONION_DIR_EXPIRE_SECS. Our own row
 * is never expired; a row with no stamp at all (last_seen <= 0) is, since
 * it has no provenance to age. Returns the number deleted. */
static int expire_directory_rows(sqlite3 *db, int64_t now)
{
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM peer_directory WHERE self = 0 AND "
        "(last_seen <= 0 OR last_seen < ?)",
        -1, &del, NULL) != SQLITE_OK || !del) {
        LOG_WARN(ODIR_LOG, "failed to prepare directory expiry: %s",
                 sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_int64(del, 1, now - ONION_DIR_EXPIRE_SECS);
    int deleted = 0;
    if (AR_STEP_WRITE(del) == SQLITE_DONE)
        deleted = sqlite3_changes(db);
    sqlite3_finalize(del);
    return deleted;
}

static int count_directory_rows(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM peer_directory",
                           -1, &s, NULL) != SQLITE_OK || !s)
        return -1;
    int n = -1;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW)
        n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

/* Open the directory database read-write. NULL when there is no datadir
 * (the onion service was never started) or the file will not open — both
 * of which the callers must treat as "not wired", never as "nothing to
 * do". */
static sqlite3 *directory_open_rw(void)
{
    const char *datadir = onion_service_datadir();
    if (!datadir)
        return NULL;
    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    /* Short, deliberate: the refresh round runs on the shared supervisor
     * tick runner, which must never park behind a long reducer commit. */
    sqlite3_busy_timeout(db, 2000);
    ensure_directory_table(db);
    return db;
}

/* Register our own .onion address with clearnet IP if known. Returns true
 * when a row was written — the refresh round counts that as real work,
 * because it is what keeps THIS node's served row current. */
static bool register_self(sqlite3 *db)
{
    const char *self_addr = onion_service_get_address();
    if (!self_addr || !self_addr[0]) return false;

    /* Discover our public IP */
    extern void peer_strategy_discover_self(void *profile, uint16_t port);
    struct { bool has_public_ip; bool nat; bool upnp; bool tor;
             uint8_t public_ip[4]; uint16_t public_port;
             char onion_address[68]; } profile = {0};
    peer_strategy_discover_self(&profile, 8033);

    char ip_str[64] = "";
    if (profile.has_public_ip) {
        snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                 profile.public_ip[0], profile.public_ip[1],
                 profile.public_ip[2], profile.public_ip[3]);
    }

    /* UPSERT rather than INSERT OR REPLACE: replacing the row would reset
     * first_seen, losing how long this node has been announcing itself. */
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO peer_directory "
        "(onion_address, port, services, height, first_seen, last_seen,"
        " last_probe, probe_ok, fail_count, version, self,"
        " clearnet_ip, clearnet_port, source) "
        "VALUES (?, 8033, 1029, 0, ?, ?, ?, 1, 0, '0.1.0', 1, ?, ?, 'self') "
        "ON CONFLICT(onion_address) DO UPDATE SET "
        "  last_seen = excluded.last_seen,"
        "  last_probe = excluded.last_probe,"
        "  probe_ok = 1, fail_count = 0, self = 1, source = 'self',"
        "  clearnet_ip = excluded.clearnet_ip,"
        "  clearnet_port = excluded.clearnet_port",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        fprintf(stderr, "onion_service: failed to prepare self-register: %s\n",
                sqlite3_errmsg(db));
        return false;
    }
    int64_t now = (int64_t)platform_time_wall_time_t();
    sqlite3_bind_text(ins, 1, self_addr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 2, now);
    sqlite3_bind_int64(ins, 3, now);
    sqlite3_bind_int64(ins, 4, now);
    sqlite3_bind_text(ins, 5, ip_str[0] ? ip_str : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 6, ip_str[0] ? 8033 : 0);
    bool ok = (AR_STEP_WRITE(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);
    return ok;
}

/* ── The census bridge ────────────────────────────────────── */

int onion_service_directory_observe(const struct onion_directory_observation *obs,
                                    size_t n,
                                    struct onion_directory_refresh_stats *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
    if (n == 0)
        return 0;                    /* a census with nothing to say is fine */
    if (!obs)
        LOG_ERR(ODIR_LOG, "observe: NULL observations for n=%zu", n);

    sqlite3 *db = directory_open_rw();
    if (!db)
        LOG_ERR(ODIR_LOG, "observe: directory not open (no datadir)");

    /* Reachable: last_seen only ever moves FORWARD (MAX), so a probe with
     * a stale clock cannot age a row backwards. Unreachable: last_seen is
     * deliberately untouched — a failed dial is not evidence of absence,
     * it just stops being evidence of presence, and the row ages out on
     * its own. */
    sqlite3_stmt *up_ok = NULL, *up_fail = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE peer_directory SET last_seen = MAX(last_seen, ?),"
            " last_probe = ?, probe_ok = 1, fail_count = 0,"
            " height = MAX(height, ?) WHERE onion_address = ?",
            -1, &up_ok, NULL) != SQLITE_OK || !up_ok ||
        sqlite3_prepare_v2(db,
            "UPDATE peer_directory SET last_probe = ?, probe_ok = 0,"
            " fail_count = fail_count + 1 WHERE onion_address = ?",
            -1, &up_fail, NULL) != SQLITE_OK || !up_fail) {
        LOG_WARN(ODIR_LOG, "observe: prepare failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(up_ok);
        sqlite3_finalize(up_fail);
        sqlite3_close(db);
        return -1;
    }

    struct onion_directory_refresh_stats st = {0};
    int64_t now = (int64_t)platform_time_wall_time_t();
    for (size_t i = 0; i < n; i++) {
        const struct onion_directory_observation *o = &obs[i];
        if (!onion_hostname_valid(o->hostname)) {
            st.unknown++;
            continue;
        }
        int64_t probe = (o->observed_unix > 0 && o->observed_unix <= now)
                            ? o->observed_unix : now;
        if (o->reachable) {
            sqlite3_reset(up_ok);
            sqlite3_bind_int64(up_ok, 1, probe);
            sqlite3_bind_int64(up_ok, 2, probe);
            sqlite3_bind_int64(up_ok, 3,
                               o->best_height > 0 ? o->best_height : 0);
            sqlite3_bind_text(up_ok, 4, o->hostname, -1, SQLITE_STATIC);
            if (AR_STEP_WRITE(up_ok) == SQLITE_DONE && sqlite3_changes(db) > 0) {
                st.observed++;
                st.refreshed++;
            } else {
                st.unknown++;
            }
        } else {
            sqlite3_reset(up_fail);
            sqlite3_bind_int64(up_fail, 1, probe);
            sqlite3_bind_text(up_fail, 2, o->hostname, -1, SQLITE_STATIC);
            if (AR_STEP_WRITE(up_fail) == SQLITE_DONE && sqlite3_changes(db) > 0) {
                st.observed++;
                st.failed++;
            } else {
                st.unknown++;
            }
        }
    }
    sqlite3_finalize(up_ok);
    sqlite3_finalize(up_fail);
    st.rows_after = count_directory_rows(db);
    sqlite3_close(db);

    if (out)
        *out = st;
    return st.observed;
}

bool onion_service_directory_refresh(struct onion_directory_refresh_stats *out)
{
    struct onion_directory_refresh_stats st = {0};
    if (out)
        *out = st;

    sqlite3 *db = directory_open_rw();
    if (!db)
        LOG_FAIL(ODIR_LOG, "refresh: directory not open (no datadir)");

    int64_t now = (int64_t)platform_time_wall_time_t();
    st.discovered = populate_directory_from_chain(db);
    if (register_self(db))
        st.refreshed++;
    st.expired = expire_directory_rows(db, now);
    st.rows_after = count_directory_rows(db);
    sqlite3_close(db);

    if (out)
        *out = st;
    if (st.discovered || st.expired)
        log_jsonf(LOG_JSON_INFO, "onion_directory_refreshed",
                  "\"discovered\":%d,\"expired\":%d,\"rows\":%d",
                  st.discovered, st.expired, st.rows_after);
    return true;
}

/* ── The onion graph: parse + ADD-only persistence ────────── */

/* Read `key`'s integer value out of a single directory object's text.
 * `seg` is already bounded to one object by the caller. */
static int64_t odir_json_int(const char *seg, const char *key, int64_t dflt)
{
    const char *k = strstr(seg, key);
    if (!k)
        return dflt;
    k += strlen(key);
    while (*k == ' ' || *k == ':')
        k++;
    if (*k != '-' && (*k < '0' || *k > '9'))
        return dflt;
    return (int64_t)strtoll(k, NULL, 10);
}

int onion_directory_parse_relay_hints(const char *body, const char *self_host,
                                      struct onion_relay_hint *out, size_t max)
{
    if (!body || !out || max == 0)
        return 0;

    static const char NEEDLE[] = "\"onion\":\"";
    const size_t NEEDLE_LEN = sizeof(NEEDLE) - 1;

    int kept = 0;
    const char *p = body;
    while ((size_t)kept < max && (p = strstr(p, NEEDLE)) != NULL) {
        p += NEEDLE_LEN;
        const char *end = strchr(p, '"');
        if (!end || end == p) {
            p++;
            continue;
        }
        size_t hlen = (size_t)(end - p);
        char host[64];
        if (hlen >= sizeof(host)) {
            p = end + 1;
            continue;
        }
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        p = end + 1;

        /* Every hostname, from every source, through the one v3 rule. */
        if (!onion_hostname_valid(host))
            continue;
        if (self_host && self_host[0] && strcmp(host, self_host) == 0)
            continue;
        bool dup = false;
        for (int i = 0; i < kept && !dup; i++)
            dup = (strcmp(out[i].hostname, host) == 0);
        if (dup)
            continue;

        /* Only the rest of THIS object, so a later node's port/height can
         * never be read as this one's. */
        char seg[512];
        const char *obj_end = strchr(p, '}');
        size_t seglen = obj_end ? (size_t)(obj_end - p) : strlen(p);
        if (seglen >= sizeof(seg))
            seglen = sizeof(seg) - 1;
        memcpy(seg, p, seglen);
        seg[seglen] = '\0';

        memset(&out[kept], 0, sizeof(out[kept]));
        snprintf(out[kept].hostname, sizeof(out[kept].hostname), "%s", host);
        int64_t port = odir_json_int(seg, "\"port\"", 0);
        out[kept].port = (port > 0 && port <= 65535) ? (int)port : 0;
        int64_t height = odir_json_int(seg, "\"height\"", 0);
        out[kept].height = (height > 0 && height <= 0x7fffffff) ? (int)height : 0;
        int64_t seen = odir_json_int(seg, "\"last_seen\"", 0);
        out[kept].last_seen = seen > 0 ? seen : 0;
        kept++;
    }
    return kept;
}

/* ── Follow budget for the one transitive hop ─────────────── */

static pthread_mutex_t g_relay_mutex = PTHREAD_MUTEX_INITIALIZER;
static char    g_relay_seen[ONION_RELAY_VISIT_CAP][64];
static int     g_relay_seen_n = 0;
static int     g_relay_follows = 0;
static int64_t g_relay_window_start = 0;

bool onion_directory_claim_relay_follow(const char *hostname, int64_t now)
{
    if (!onion_hostname_valid(hostname))
        LOG_FAIL(ODIR_LOG, "claim_relay_follow: hostname fails the v3 rule");

    bool claimed = false;
    pthread_mutex_lock(&g_relay_mutex);
    if (g_relay_window_start == 0 ||
        now - g_relay_window_start >= ONION_RELAY_WINDOW_SECS ||
        now < g_relay_window_start) {
        g_relay_window_start = now;
        g_relay_follows = 0;
        g_relay_seen_n = 0;
    }
    bool seen = false;
    for (int i = 0; i < g_relay_seen_n && !seen; i++)
        seen = (strcmp(g_relay_seen[i], hostname) == 0);
    if (!seen && g_relay_follows < ONION_RELAY_FOLLOW_BUDGET &&
        g_relay_seen_n < ONION_RELAY_VISIT_CAP) {
        snprintf(g_relay_seen[g_relay_seen_n], sizeof(g_relay_seen[0]),
                 "%s", hostname);
        g_relay_seen_n++;
        g_relay_follows++;
        claimed = true;
    }
    pthread_mutex_unlock(&g_relay_mutex);
    return claimed;
}

void onion_directory_reset_relay_follow(void)
{
    pthread_mutex_lock(&g_relay_mutex);
    g_relay_seen_n = 0;
    g_relay_follows = 0;
    g_relay_window_start = 0;
    pthread_mutex_unlock(&g_relay_mutex);
}

bool onion_service_directory_learn(const char *hostname, int port, int height,
                                   int64_t peer_last_seen)
{
    if (!onion_hostname_valid(hostname))
        LOG_FAIL(ODIR_LOG, "learn: hostname fails the v3 rule");

    int64_t now = (int64_t)platform_time_wall_time_t();
    /* Hearsay may age a row, never freshen it past our own clock. */
    int64_t stamp = (peer_last_seen > 0 && peer_last_seen < now)
                        ? peer_last_seen : now;
    if (now - stamp >= ONION_DIR_EXPIRE_SECS)
        LOG_FAIL(ODIR_LOG,
                 "learn: relayed stamp is already past expiry (age %llds)",
                 (long long)(now - stamp));

    sqlite3 *db = directory_open_rw();
    if (!db)
        LOG_FAIL(ODIR_LOG, "learn: directory not open (no datadir)");

    /* INSERT OR IGNORE, by construction: a hostname another node told us
     * about may only ADD a place to look. It never overwrites a row we
     * measured ourselves and it never deletes anything. */
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO peer_directory "
            "(onion_address, port, services, height, first_seen, last_seen,"
            " last_probe, probe_ok, fail_count, version, self,"
            " clearnet_ip, clearnet_port, source) "
            "VALUES (?, ?, 0, ?, ?, ?, 0, 0, 0, 'relay', 0, '', 0, 'relay')",
            -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_WARN(ODIR_LOG, "learn: prepare failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(ins);
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(ins, 1, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 2, (port > 0 && port <= 65535) ? port : 8033);
    sqlite3_bind_int(ins, 3, height > 0 ? height : 0);
    sqlite3_bind_int64(ins, 4, now);
    sqlite3_bind_int64(ins, 5, stamp);
    bool ok = (AR_STEP_WRITE(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);
    sqlite3_close(db);
    if (!ok)
        LOG_FAIL(ODIR_LOG, "learn: insert failed for a valid hostname");
    return true;
}

/* ── Supervised refresh (no dedicated thread) ─────────────── */

/* Four missed rounds. Long enough that a transient busy database or a
 * boot-time race never fires it, short enough that a directory which has
 * genuinely stopped refreshing is named within an hour instead of being
 * served stale for a week. */
#define ONION_DIR_MAX_QUIET_US \
    ((int64_t)ONION_DIR_REFRESH_SECS * 4 * 1000 * 1000)

static struct liveness_contract g_dir_contract;
static _Atomic supervisor_child_id g_dir_child = SUPERVISOR_INVALID_ID;
static _Atomic int64_t  g_dir_marker = 0;   /* cumulative rows written */
static _Atomic uint64_t g_dir_rounds = 0;

static void onion_directory_tick(struct liveness_contract *c)
{
    (void)c;
    supervisor_child_id id = atomic_load(&g_dir_child);
    struct onion_directory_refresh_stats st;
    bool ok = onion_service_directory_refresh(&st);
    atomic_fetch_add(&g_dir_rounds, 1);

    if (!ok) {
        /* Directory unopenable. Report NEITHER progress nor idle: this is
         * precisely the state the no-progress detector exists to surface,
         * and calling it idle would hide it forever. */
        supervisor_tick(id);
        return;
    }

    int touched = st.discovered + st.refreshed + st.expired;
    if (touched > 0) {
        int64_t marker = atomic_fetch_add(&g_dir_marker, touched) + touched;
        supervisor_progress(id, marker);
    } else {
        /* The round completed against a writable directory and there was
         * nothing to write: no source announced a peer, nothing aged out,
         * and Tor has not handed us an address to publish yet. Positively
         * established no-work, not a failure and not a skipped round. */
        supervisor_progress_idle(id);
    }
    supervisor_tick(id);
}

void onion_service_directory_register_refresh(void)
{
    if (atomic_load(&g_dir_child) != SUPERVISOR_INVALID_ID)
        return;
    liveness_contract_init(&g_dir_contract, "net.onion_directory");
    atomic_store(&g_dir_contract.period_secs, (int64_t)ONION_DIR_REFRESH_SECS);
    atomic_store(&g_dir_contract.deadline_secs, (int64_t)0);
    g_dir_contract.on_tick = onion_directory_tick;
    g_dir_contract.on_stall = NULL;
    /* A ROOT child, not supervisor_register_in_domain(net, ...): lib/net
     * cannot include the app-side supervisors/domains.h without a layering
     * violation — same reason the four connman threads are root children. */
    supervisor_child_id id = supervisor_register(&g_dir_contract);  // supervisor-root-ok:lib-net-cannot-include-app-domains
    atomic_store(&g_dir_child, id);
    if (id == SUPERVISOR_INVALID_ID) {
        LOG_WARN(ODIR_LOG, "supervisor register failed (registry full)");
        return;
    }
    /* Count RESULTS, not activity: rounds that write nothing and report
     * nothing idle accumulate into a NO_PROGRESS stall. */
    supervisor_set_progress_max_quiet(id, ONION_DIR_MAX_QUIET_US);
}

void onion_service_directory_unregister_refresh(void)
{
    supervisor_child_id id = atomic_exchange(&g_dir_child, SUPERVISOR_INVALID_ID);
    if (id != SUPERVISOR_INVALID_ID)
        supervisor_unregister(id);
}


/* ── Boot and address-change entry points ─────────────────── */

void onion_directory_boot_round(void)
{
    sqlite3 *db = directory_open_rw();
    if (!db) {
        LOG_WARN(ODIR_LOG, "boot round: directory not open (no datadir)");
        return;
    }
    (void)populate_directory_from_chain(db);
    (void)register_self(db);
    /* Expire on the way in as well as on the tick: a node that has been
     * down for a week must not serve its pre-shutdown list as current
     * during the first refresh interval after boot. */
    (void)expire_directory_rows(db, (int64_t)platform_time_wall_time_t());
    sqlite3_close(db);
}

void onion_directory_register_self(void)
{
    sqlite3 *db = directory_open_rw();
    if (!db) {
        LOG_WARN(ODIR_LOG, "self-register: directory not open (no datadir)");
        return;
    }
    bool wrote = register_self(db);
    sqlite3_close(db);
    if (!wrote)
        return;
    const char *addr = onion_service_get_address();
    char addr_safe[96];
    log_json_escape(addr_safe, sizeof(addr_safe), addr ? addr : "");
    log_jsonf(LOG_JSON_INFO, "onion_self_registered",
              "\"address\":\"%s\"", addr_safe);
}
