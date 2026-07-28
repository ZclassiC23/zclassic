/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_onion_directory — the onion directory's THREE unproven contracts:
 * names are searchable, rows carry an age and expire, and the onion half of
 * a peer's /directory.json is parsed instead of discarded.
 *
 * Every assertion drives a REAL production function against a REAL SQLite
 * node.db in a fresh temp datadir. No network, no Tor thread, no live node:
 * onion_service_start() only opens the database, and the chain-scan hook
 * (onion_service_set_app_handlers) is left NULL so nothing dials.
 *
 * WHAT WOULD HAVE FAILED BEFORE
 * -----------------------------
 *  1. serve_search matched only the raw .onion hostname, so a query for a
 *     registered name returned "No results" even with the ZNAM row folded.
 *     find_by_name / directory renders name assert the join both ways.
 *  2. peer_directory rows were written once at boot and never refreshed or
 *     expired; serve_directory_json served up to 500 rows of any age with no
 *     age field at all. expiry / age_field / observe_* assert last_seen,
 *     last_success, the dial counters, the served age_secs, and that the
 *     self row survives a sweep that deletes its stale neighbours.
 *  3. try_onion_seed_fetch string-scanned only clearnet_ip; the "onion"
 *     field of every record was dropped on the floor, so the onion graph was
 *     never transitively walked. scan_* asserts the parser that feeds it,
 *     including that ONE malformed record cannot hide the honest records
 *     that follow it.
 */

#include "test/test_core.h"
#include "net/onion_discovery.h"
#include "net/onion_service.h"
#include "net/onion_ratelimit.h"
#include "platform/time_compat.h"
#include "znam/znam.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define OD_CHECK(name, expr) do {                       \
    printf("onion_directory: %s... ", (name));          \
    if ((expr)) printf("OK\n");                         \
    else { printf("FAIL\n"); failures++; }              \
} while (0)

/* Two well-formed v3 hosts (56 base32 chars + ".onion"). */
#define HOST_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion"
#define HOST_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.onion"
#define HOST_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccc.onion"
#define HOST_SELF "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz.onion"

/* ── fixture helpers ───────────────────────────────────────────── */

static sqlite3 *od_open(const char *datadir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 5000);
    return db;
}

static bool od_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("[sql: %s] ", err ? err : "?");
        sqlite3_free(err);
        return false;
    }
    return true;
}

/* Single-integer scalar query; returns `missing` when no row. */
static int64_t od_scalar(sqlite3 *db, const char *sql, int64_t missing)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s) {
        if (s) sqlite3_finalize(s);
        return missing;
    }
    int64_t v = missing;
    if (sqlite3_step(s) == SQLITE_ROW)     // raw-sql-ok: test fixture readback
        v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

/* Create the ZNAM projection table the join reads and register one name. */
static bool od_register_name(sqlite3 *db, const char *name,
                             const char *target, int height)
{
    if (!od_exec(db,
        "CREATE TABLE IF NOT EXISTS znam_names ("
        "name TEXT PRIMARY KEY,"
        "owner_address TEXT NOT NULL,"
        "target_type INTEGER NOT NULL,"
        "target_value TEXT NOT NULL,"
        "reg_txid BLOB NOT NULL,"
        "reg_height INTEGER NOT NULL,"
        "last_update_txid BLOB NOT NULL)"))
        return false;

    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO znam_names "
        "(name, owner_address, target_type, target_value, reg_txid,"
        " reg_height, last_update_txid) "
        "VALUES (?1,'t1owner',?2,?3,zeroblob(32),?4,zeroblob(32))",
        -1, &s, NULL) != SQLITE_OK || !s) {
        if (s) sqlite3_finalize(s);
        return false;
    }
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, ZNAM_TYPE_ONION);
    sqlite3_bind_text(s, 3, target, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 4, height);
    bool ok = sqlite3_step(s) == SQLITE_DONE;  // raw-sql-ok: test fixture write
    sqlite3_finalize(s);
    return ok;
}

/* ── 1. v3 hostname shape ──────────────────────────────────────── */

static int od_test_hostname_shape(void)
{
    int failures = 0;

    OD_CHECK("valid v3 host accepted",
             onion_hostname_is_valid_v3(HOST_A));
    OD_CHECK("NULL rejected", !onion_hostname_is_valid_v3(NULL));
    OD_CHECK("empty rejected", !onion_hostname_is_valid_v3(""));
    OD_CHECK("missing suffix rejected", !onion_hostname_is_valid_v3(
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    OD_CHECK("v2-length rejected", !onion_hostname_is_valid_v3(
             "abcdefghij234567.onion"));
    OD_CHECK("uppercase rejected", !onion_hostname_is_valid_v3(
             "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion"));
    /* '1' and '8' are outside the base32 alphabet [a-z2-7]. */
    OD_CHECK("out-of-alphabet digit rejected", !onion_hostname_is_valid_v3(
             "1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion"));
    OD_CHECK("trailing garbage rejected", !onion_hostname_is_valid_v3(
             HOST_A "x"));
    return failures;
}

/* ── 2. /directory.json onion-field scanner ────────────────────── */

static int od_test_scan(void)
{
    int failures = 0;
    char host[64];

    /* A realistic response: two good records around one whose onion field
     * is empty, one over-long, and clearnet fields interleaved. */
    static const char BODY[] =
        "{\"nodes\":["
        "{\"onion\":\"" HOST_A "\",\"clearnet_ip\":\"1.2.3.4\"},"
        "{\"onion\":\"\",\"clearnet_ip\":\"5.6.7.8\"},"
        "{\"onion\":\"" /* 200 chars: over-long, must be skipped not fatal */
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaa\"},"
        "{\"onion\":\"" HOST_B "\",\"clearnet_ip\":\"9.9.9.9\"}"
        "],\"count\":4}";

    const char *cur = BODY;
    bool got_a = onion_directory_scan_next_onion(&cur, host, sizeof(host));
    OD_CHECK("scan finds first onion", got_a && strcmp(host, HOST_A) == 0);

    /* The empty and the over-long records must be SKIPPED, not fatal — a
     * hostile peer cannot hide the honest records that follow its own. */
    bool got_b = onion_directory_scan_next_onion(&cur, host, sizeof(host));
    OD_CHECK("scan survives empty + over-long records",
             got_b && strcmp(host, HOST_B) == 0);

    OD_CHECK("scan terminates at end of body",
             !onion_directory_scan_next_onion(&cur, host, sizeof(host)));

    /* Unterminated field: no crash, no read past the NUL, returns false. */
    static const char TRUNC[] = "{\"nodes\":[{\"onion\":\"aaaa";
    cur = TRUNC;
    OD_CHECK("unterminated field returns false",
             !onion_directory_scan_next_onion(&cur, host, sizeof(host)));

    /* A body with no onion field at all — the pre-change clearnet-only
     * shape — must simply yield nothing. */
    static const char NOONION[] = "{\"nodes\":[{\"clearnet_ip\":\"1.2.3.4\"}]}";
    cur = NOONION;
    OD_CHECK("clearnet-only body yields no onions",
             !onion_directory_scan_next_onion(&cur, host, sizeof(host)));

    /* Defensive arguments. */
    cur = BODY;
    OD_CHECK("NULL out rejected",
             !onion_directory_scan_next_onion(&cur, NULL, sizeof(host)));
    OD_CHECK("zero-length out rejected",
             !onion_directory_scan_next_onion(&cur, host, 0));
    OD_CHECK("NULL cursor rejected",
             !onion_directory_scan_next_onion(NULL, host, sizeof(host)));
    return failures;
}

/* ── 3. observation semantics + freshness columns ──────────────── */

static int od_test_observe(const char *datadir)
{
    int failures = 0;

    /* Discovery bookkeeping never CREATES node.db — it opens READWRITE and
     * no-ops if the database is absent, so a net-layer probe can never race
     * the migration runner into existence. Assert that first, then create
     * the database the way boot does and re-observe. */
    onion_directory_observe(datadir, HOST_A, ONION_DIR_ADVERTISED, 1234);
    sqlite3 *db = od_open(datadir);   /* sqlite3_open: creates */
    if (!db) { printf("onion_directory: cannot open fixture db\n"); return 1; }
    OD_CHECK("observe before node.db exists is a silent no-op",
             od_scalar(db, "SELECT COUNT(*) FROM sqlite_master "
                           "WHERE name='peer_directory'", -1) == 0);
    sqlite3_close(db);

    /* ADVERTISED creates the row: heard about, never contacted. */
    onion_directory_observe(datadir, HOST_A, ONION_DIR_ADVERTISED, 1234);
    db = od_open(datadir);
    if (!db) { printf("onion_directory: cannot open fixture db\n"); return 1; }

    OD_CHECK("advertised creates a row",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1);
    OD_CHECK("advertised sets last_seen",
             od_scalar(db, "SELECT last_seen FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", 0) > 0);
    OD_CHECK("advertised sets first_seen",
             od_scalar(db, "SELECT first_seen FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", 0) > 0);
    OD_CHECK("advertised is NOT contact (last_success stays 0)",
             od_scalar(db, "SELECT last_success FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 0);
    OD_CHECK("advertised records the advertised height",
             od_scalar(db, "SELECT height FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1234);

    /* UNREACHABLE on an UNKNOWN host must not insert: a failed dial
     * carries no identity. */
    onion_directory_observe(datadir, HOST_C, ONION_DIR_UNREACHABLE, 0);
    OD_CHECK("failed dial on unknown host inserts nothing",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory "
                           "WHERE onion_address='" HOST_C "'", -1) == 0);

    /* UNREACHABLE on a KNOWN host bumps the failure counter only. */
    int64_t seen_before = od_scalar(db, "SELECT last_seen FROM peer_directory "
                                        "WHERE onion_address='" HOST_A "'", 0);
    onion_directory_observe(datadir, HOST_A, ONION_DIR_UNREACHABLE, 0);
    OD_CHECK("failed dial bumps dial_fail_count",
             od_scalar(db, "SELECT dial_fail_count FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1);
    OD_CHECK("failed dial does NOT refresh last_seen",
             od_scalar(db, "SELECT last_seen FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1)
                 == seen_before);

    /* REACHED is our own contact: both stamps plus the success counter. */
    onion_directory_observe(datadir, HOST_A, ONION_DIR_REACHED, 0);
    OD_CHECK("reached sets last_success",
             od_scalar(db, "SELECT last_success FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", 0) > 0);
    OD_CHECK("reached bumps dial_success_count",
             od_scalar(db, "SELECT dial_success_count FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1);
    OD_CHECK("reached never lowers a known height",
             od_scalar(db, "SELECT height FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1234);

    /* Malformed hostnames never reach the table. */
    onion_directory_observe(datadir, "not-an-onion", ONION_DIR_ADVERTISED, 1);
    onion_directory_observe(datadir, NULL, ONION_DIR_ADVERTISED, 1);
    onion_directory_observe(NULL, HOST_B, ONION_DIR_ADVERTISED, 1);
    OD_CHECK("malformed host is never stored",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory", -1) == 1);

    sqlite3_close(db);
    return failures;
}

/* ── 4. expiry ─────────────────────────────────────────────────── */

static int od_test_expiry(const char *datadir)
{
    int failures = 0;

    sqlite3 *db = od_open(datadir);
    if (!db) { printf("onion_directory: cannot open fixture db\n"); return 1; }

    int64_t now = (int64_t)platform_time_wall_time_t();
    char sql[512];

    /* A stale peer row and a stale SELF row, both older than the cutoff. */
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO peer_directory "
        "(onion_address, port, services, height, last_seen, version, self) "
        "VALUES ('" HOST_B "',8033,0,0,%lld,'test',0)",
        (long long)(now - 10 * 86400));
    OD_CHECK("stale peer row inserted", od_exec(db, sql));

    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO peer_directory "
        "(onion_address, port, services, height, last_seen, version, self) "
        "VALUES ('" HOST_SELF "',8033,0,0,%lld,'test',1)",
        (long long)(now - 10 * 86400));
    OD_CHECK("stale self row inserted", od_exec(db, sql));
    sqlite3_close(db);

    int deleted = onion_directory_expire(datadir, now,
                                         ONION_DIRECTORY_MAX_AGE_SECS);
    OD_CHECK("expire deletes exactly the stale non-self row", deleted == 1);

    db = od_open(datadir);
    if (!db) { printf("onion_directory: cannot reopen fixture db\n"); return 1; }
    OD_CHECK("stale peer row is gone",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory "
                           "WHERE onion_address='" HOST_B "'", -1) == 0);
    OD_CHECK("self row survives expiry",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory "
                           "WHERE onion_address='" HOST_SELF "'", -1) == 1);
    OD_CHECK("fresh row survives expiry",
             od_scalar(db, "SELECT COUNT(*) FROM peer_directory "
                           "WHERE onion_address='" HOST_A "'", -1) == 1);
    sqlite3_close(db);

    OD_CHECK("expire rejects a NULL datadir",
             onion_directory_expire(NULL, now, 60) == -1);
    OD_CHECK("expire rejects a negative max_age",
             onion_directory_expire(datadir, now, -1) == -1);
    return failures;
}

/* ── 5. the ZNAM join ──────────────────────────────────────────── */

static int od_test_name_join(const char *datadir)
{
    int failures = 0;
    char name[80];

    /* No registration yet → nameless, and that is not an error. */
    OD_CHECK("unregistered host resolves to no name",
             !onion_directory_name_for(datadir, HOST_A, name, sizeof(name)) &&
             name[0] == '\0');

    sqlite3 *db = od_open(datadir);
    if (!db) { printf("onion_directory: cannot open fixture db\n"); return 1; }
    /* Stored WITHOUT the ".onion" suffix — the bare-56 form. */
    OD_CHECK("register alice -> HOST_A (bare form)",
             od_register_name(db, "alice",
                              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                              "aaaaaaaaaaaaaaaa", 100));
    /* Stored WITH the suffix — the full form. */
    OD_CHECK("register bob -> HOST_SELF (full form)",
             od_register_name(db, "bob", HOST_SELF, 200));
    sqlite3_close(db);

    OD_CHECK("bare-form registration resolves",
             onion_directory_name_for(datadir, HOST_A, name, sizeof(name)) &&
             strcmp(name, "alice") == 0);
    OD_CHECK("full-form registration resolves",
             onion_directory_name_for(datadir, HOST_SELF, name, sizeof(name)) &&
             strcmp(name, "bob") == 0);
    OD_CHECK("a host with no registration stays nameless",
             !onion_directory_name_for(datadir, HOST_C, name, sizeof(name)));
    OD_CHECK("malformed host resolves to no name",
             !onion_directory_name_for(datadir, "bogus", name, sizeof(name)));
    OD_CHECK("NULL datadir resolves to no name",
             !onion_directory_name_for(NULL, HOST_A, name, sizeof(name)));
    return failures;
}

/* ── 6. end-to-end through the real request handler ────────────── */

static int od_test_served_pages(const char *datadir)
{
    int failures = 0;

    /* Drive the REAL onion request router. onion_service_start only opens
     * the database; no app handler is registered, so nothing dials. */
    onion_service_start(datadir);
    onion_ratelimit_test_reset();

    static uint8_t resp[262144];

    /* SEARCH BY NAME — the case that returned "No results" before. */
    memset(resp, 0, sizeof(resp));
    size_t n = onion_service_handle_request("GET", "/search?q=alice", NULL, 0,
                                            resp, sizeof(resp) - 1);
    resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = 0;
    const char *page = (const char *)resp;
    OD_CHECK("search by name returns a page", n > 0);
    OD_CHECK("search by name finds the registered name",
             strstr(page, "alice") != NULL);
    OD_CHECK("search by name shows the RAW address too",
             strstr(page, HOST_A) != NULL);
    OD_CHECK("search by name reports no 'No results'",
             strstr(page, "No results") == NULL);

    /* SEARCH BY ADDRESS — the pre-existing behaviour, not narrowed. */
    onion_ratelimit_test_reset();
    memset(resp, 0, sizeof(resp));
    n = onion_service_handle_request("GET", "/search?q=aaaaaaaa", NULL, 0,
                                     resp, sizeof(resp) - 1);
    resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = 0;
    OD_CHECK("search by address prefix still works",
             strstr((const char *)resp, HOST_A) != NULL);

    /* A query matching nothing must still say so. */
    onion_ratelimit_test_reset();
    memset(resp, 0, sizeof(resp));
    n = onion_service_handle_request("GET", "/search?q=zzzznomatch", NULL, 0,
                                     resp, sizeof(resp) - 1);
    resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = 0;
    OD_CHECK("non-matching query reports no results",
             strstr((const char *)resp, "No results") != NULL);

    /* DIRECTORY JSON — name beside the address, plus the age fields. */
    onion_ratelimit_test_reset();
    memset(resp, 0, sizeof(resp));
    n = onion_service_handle_request("GET", "/directory.json", NULL, 0,
                                     resp, sizeof(resp) - 1);
    resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = 0;
    const char *js = (const char *)resp;
    OD_CHECK("directory.json served", n > 0);
    OD_CHECK("directory.json carries the on-chain name",
             strstr(js, "\"name\":\"alice\"") != NULL);
    OD_CHECK("directory.json still carries the raw onion",
             strstr(js, "\"onion\":\"" HOST_A "\"") != NULL);
    OD_CHECK("directory.json carries per-row age",
             strstr(js, "\"age_secs\":") != NULL);
    OD_CHECK("directory.json carries the contact record",
             strstr(js, "\"last_success\":") != NULL &&
             strstr(js, "\"dial_fail_count\":") != NULL);
    OD_CHECK("directory.json declares its freshness policy",
             strstr(js, "\"max_age_secs\":") != NULL);

    /* DIRECTORY HTML — name as the heading, address still rendered. */
    onion_ratelimit_test_reset();
    memset(resp, 0, sizeof(resp));
    n = onion_service_handle_request("GET", "/directory", NULL, 0,
                                     resp, sizeof(resp) - 1);
    resp[n < sizeof(resp) ? n : sizeof(resp) - 1] = 0;
    const char *html = (const char *)resp;
    OD_CHECK("directory html served", n > 0);
    OD_CHECK("directory html shows the name", strstr(html, "alice") != NULL);
    OD_CHECK("directory html still shows the raw address",
             strstr(html, HOST_A) != NULL);
    OD_CHECK("directory html reports our contact record",
             strstr(html, "Reached") != NULL);

    onion_service_stop();
    return failures;
}

/* ── group entry point ─────────────────────────────────────────── */

int test_onion_directory(void)
{
    int failures = 0;
    printf("\n=== Onion Directory (names, freshness, onion-graph walk) ===\n");

    /* The address singleton is process-global; the sequential runner shares
     * it across groups. Snapshot and restore. */
    const char *prev = onion_service_get_address();
    char saved[128] = "";
    if (prev) snprintf(saved, sizeof(saved), "%s", prev);
    onion_service_set_address(NULL);

    /* Static: onion_service_start() borrows this pointer for ctx->datadir. */
    static char datadir[256];
    test_make_tmpdir(datadir, sizeof(datadir), "onion_dir", "main");

    failures += od_test_hostname_shape();
    failures += od_test_scan();
    failures += od_test_observe(datadir);
    failures += od_test_expiry(datadir);
    failures += od_test_name_join(datadir);
    failures += od_test_served_pages(datadir);

    test_cleanup_tmpdir(datadir);
    onion_service_set_address(saved[0] ? saved : NULL);

    printf("=== Onion Directory: %d failure(s) ===\n", failures);
    return failures;
}
