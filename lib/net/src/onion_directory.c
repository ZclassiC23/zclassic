/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * onion_directory — the peer_directory ledger behind the onion front door.
 *
 * Contract, observation semantics, and every bound: net/onion_discovery.h.
 * Split out of onion_service.c, which serves the /directory pages; this
 * file owns the DATA (the table, its freshness columns, the expiry sweep,
 * the v3 hostname predicate, the /directory.json onion-field parser, and
 * the read-only join onto the ZNAM name projection).
 *
 * A lib/ module talking raw SQLite rather than the AR_* model macros: those
 * live under app/models and would invert the lib/ -> app/ dependency
 * direction check-lib-layering enforces. Same principled exception as
 * lib/net/src/rom_seed_ledger.c and lib/storage/src/peers_projection.c;
 * every step goes through the AR_STEP_* wrappers.
 *
 * Names are READ here and never written: znam_names has exactly one writer
 * (the on-chain ZNAM projection). A second writable copy of a name inside
 * the directory would be the cloned-ledger bug the architecture forbids. */

#include "platform/time_compat.h"
#include "net/onion_discovery.h"
#include "znam/znam.h"
#include "util/ar_step_readonly.h"
#include "util/log_json.h"
#include "util/log_macros.h"
#include "util/path_check.h"

#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* A RENDER guard, not a re-implementation of ZNAM validity.
 *
 * Registry validity is znam_validate_name()'s job and has exactly one
 * enforcement point: the on-chain ZNAM fold that writes znam_names. lib/net
 * ranks BELOW lib/znam in the module graph (check-lib-module-order), so
 * calling into it from here would invert the dependency — and re-deciding
 * "is this a legal name" in a second place is how two answers start to
 * drift. What the directory actually needs is narrower and local: is this
 * string safe to put in an HTML page and a JSON document as a label. Kept
 * deliberately at or tighter than the registry rule, so it can only ever
 * withhold a label, never invent one. */
bool onion_directory_label_is_renderable(const char *name)
{
    if (!name || !name[0]) return false;
    size_t n = strlen(name);
    if (n > ZNAM_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

/* On-chain hostnames are attacker-controlled. Only the exact Tor v3
 * shape (56 base32 [a-z2-7] chars + ".onion" = 62) may reach HTML,
 * JSON, or the peer_directory table. Contract in net/onion_discovery.h;
 * exported because connman's seed walker filters peer-served directory
 * hostnames through the SAME predicate. */
bool onion_hostname_is_valid_v3(const char *h)
{
    if (!h) return false;
    if (strlen(h) != 62 || strcmp(h + 56, ".onion") != 0) return false;
    for (size_t i = 0; i < 56; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')))
            return false;
    }
    return true;
}

/* Contract in net/onion_discovery.h: skip malformed fields, never abort
 * the scan on one — a hostile peer must not be able to hide the honest
 * records that follow its own by emitting one broken field. */
bool onion_directory_scan_next_onion(const char **cursor,
                                     char *out, size_t out_len)
{
    if (!cursor || !*cursor || !out || out_len == 0)
        return false;
    out[0] = '\0';

    static const char KEY[] = "\"onion\":\"";
    const char *p = *cursor;

    for (;;) {
        const char *hit = strstr(p, KEY);
        if (!hit) {
            *cursor = p + strlen(p);
            return false;
        }
        const char *val = hit + (sizeof(KEY) - 1);
        const char *end = strchr(val, '"');
        if (!end) {
            /* Unterminated: nothing parseable remains. */
            *cursor = val + strlen(val);
            return false;
        }
        size_t len = (size_t)(end - val);
        p = end + 1;
        if (len == 0 || len >= out_len)
            continue;   /* empty or over-long: skip, keep scanning */
        memcpy(out, val, len);
        out[len] = '\0';
        *cursor = p;
        return true;
    }
}

/* ── Peer directory table ─────────────────────────────────── */

void onion_directory_ensure_table(sqlite3 *db)
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
    /* Freshness columns (see net/onion_discovery.h). Idempotent ALTERs,
     * same pattern as the clearnet pair above: a row written by an older
     * binary reads first_seen/last_success = 0, which the age/expiry
     * arithmetic treats as "never reached", not as "reached at epoch". */
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN first_seen INTEGER NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN last_success INTEGER NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN dial_success_count INTEGER NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "ALTER TABLE peer_directory ADD COLUMN dial_fail_count INTEGER NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
}

/* ── ZNAM name join ───────────────────────────────────────────
 *
 * The peer_directory has no name column and never will: names are an
 * on-chain projection with its own writer (znam_names), and a second
 * writable copy of a name would be exactly the cloned-ledger bug the
 * architecture forbids. Discovery READS the projection instead.
 *
 * `db` is any open handle on node.db. Returns false (out = "") when the
 * projection table does not exist yet — a node that has not folded a
 * ZNAM registration is nameless, not broken. */
bool onion_directory_name_for_db(sqlite3 *db, const char *onion,
                                 char *out, size_t out_len)
{
    if (!db || !out || out_len == 0) return false;
    out[0] = '\0';
    if (!onion_hostname_is_valid_v3(onion)) return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT name FROM znam_names WHERE target_type=?1 "
        "AND (target_value=?2 OR target_value=?3) "
        "ORDER BY reg_height ASC, name ASC LIMIT 1",
        -1, &s, NULL) != SQLITE_OK || !s) {
        if (s) sqlite3_finalize(s);
        return false;   /* projection absent: nameless, not an error */
    }

    /* Registrations in the wild carry the target with or without the
     * ".onion" suffix; match both rather than silently missing half. */
    char bare[64];
    snprintf(bare, sizeof(bare), "%.56s", onion);

    sqlite3_bind_int(s, 1, ZNAM_TYPE_ONION);
    sqlite3_bind_text(s, 2, onion, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, bare, -1, SQLITE_STATIC);

    bool got = false;
    if (AR_STEP_ROW_READONLY(s) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(s, 0);
        if (name && name[0] && onion_directory_label_is_renderable(name)) {
            snprintf(out, out_len, "%s", name);
            got = true;
        }
    }
    sqlite3_finalize(s);
    return got;
}

bool onion_directory_name_for(const char *datadir, const char *onion,
                              char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!datadir) return false;

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 5000);
    bool got = onion_directory_name_for_db(db, onion, out, out_len);
    sqlite3_close(db);
    return got;
}

/* ── Directory freshness ledger ───────────────────────────────
 *
 * Contract + observation semantics: net/onion_discovery.h. */

static _Atomic int64_t g_last_expire_sweep = 0;

int onion_directory_expire(const char *datadir, int64_t now_unix,
                           int64_t max_age_secs)
{
    if (!datadir)
        LOG_ERR("net", "onion_directory_expire: null datadir");
    if (max_age_secs < 0)
        LOG_ERR("net", "onion_directory_expire: negative max_age %lld",
                (long long)max_age_secs);

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        LOG_ERR("net", "onion_directory_expire: cannot open %s", db_path);
    }
    sqlite3_busy_timeout(db, 5000);

    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(db,
        "DELETE FROM peer_directory WHERE self=0 AND last_seen < ?",
        -1, &del, NULL) != SQLITE_OK || !del) {
        if (del) sqlite3_finalize(del);
        const char *why = sqlite3_errmsg(db);
        LOG_WARN("net", "onion_directory_expire: prepare failed: %s", why);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_int64(del, 1, now_unix - max_age_secs);
    (void)AR_STEP_WRITE(del);
    sqlite3_finalize(del);

    int deleted = sqlite3_changes(db);
    sqlite3_close(db);
    if (deleted > 0)
        log_jsonf(LOG_JSON_INFO, "onion_directory_expired",
                  "\"rows\":%d,\"max_age_secs\":%lld",
                  deleted, (long long)max_age_secs);
    return deleted;
}

void onion_directory_observe(const char *datadir, const char *onion,
                             enum onion_directory_observation obs,
                             int height)
{
    if (!datadir || !onion_hostname_is_valid_v3(onion))
        return;

    char db_path[1024];
    zcl_node_db_path(db_path, sizeof(db_path), datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        /* Bookkeeping is best-effort: a dial path never fails because the
         * directory could not be updated. */
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 5000);
    onion_directory_ensure_table(db);

    int64_t now = (int64_t)platform_time_wall_time_t();

    /* UNREACHABLE never inserts: a failed dial carries no identity. */
    if (obs != ONION_DIR_UNREACHABLE) {
        sqlite3_stmt *ins = NULL;
        if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO peer_directory "
            "(onion_address, height, last_seen, version, first_seen) "
            "VALUES (?1, ?2, ?3, 'directory', ?3)",
            -1, &ins, NULL) == SQLITE_OK && ins) {
            sqlite3_bind_text(ins, 1, onion, -1, SQLITE_STATIC);
            sqlite3_bind_int(ins, 2, height > 0 ? height : 0);
            sqlite3_bind_int64(ins, 3, now);
            (void)AR_STEP_WRITE(ins);
            sqlite3_finalize(ins);
        } else {
            if (ins) sqlite3_finalize(ins);
            LOG_WARN("net", "onion_directory_observe: insert prepare failed: %s",
                     sqlite3_errmsg(db));
        }
    }

    const char *sql = NULL;
    switch (obs) {
    case ONION_DIR_REACHED:
        sql = "UPDATE peer_directory SET last_seen=?2, last_success=?2,"
              " dial_success_count=dial_success_count+1,"
              " height=MAX(height, ?3),"
              " first_seen=CASE WHEN first_seen=0 THEN ?2 ELSE first_seen END"
              " WHERE onion_address=?1";
        break;
    case ONION_DIR_ADVERTISED:
        sql = "UPDATE peer_directory SET last_seen=?2,"
              " height=MAX(height, ?3),"
              " first_seen=CASE WHEN first_seen=0 THEN ?2 ELSE first_seen END"
              " WHERE onion_address=?1";
        break;
    case ONION_DIR_UNREACHABLE:
        sql = "UPDATE peer_directory SET dial_fail_count=dial_fail_count+1"
              " WHERE onion_address=?1";
        break;
    }
    if (!sql) {
        sqlite3_close(db);
        LOG_WARN("net", "onion_directory_observe: unknown observation %d",
                 (int)obs);
        return;
    }

    sqlite3_stmt *upd = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &upd, NULL) == SQLITE_OK && upd) {
        sqlite3_bind_text(upd, 1, onion, -1, SQLITE_STATIC);
        if (obs != ONION_DIR_UNREACHABLE) {
            sqlite3_bind_int64(upd, 2, now);
            sqlite3_bind_int(upd, 3, height > 0 ? height : 0);
        }
        (void)AR_STEP_WRITE(upd);
        sqlite3_finalize(upd);
    } else {
        if (upd) sqlite3_finalize(upd);
        LOG_WARN("net", "onion_directory_observe: update prepare failed: %s",
                 sqlite3_errmsg(db));
    }
    sqlite3_close(db);

    /* Opportunistic expiry, throttled: the probe cadence drives the sweep,
     * so a long-running node ages rows out without a second thread. */
    int64_t prev = atomic_load(&g_last_expire_sweep);
    if (now - prev >= ONION_DIRECTORY_EXPIRE_INTERVAL_SECS &&
        atomic_compare_exchange_strong(&g_last_expire_sweep, &prev, now))
        (void)onion_directory_expire(datadir, now,
                                     ONION_DIRECTORY_MAX_AGE_SECS);
}
