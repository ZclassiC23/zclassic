/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Directory freshness and the onion graph (lib/net/src/onion_service.c,
 * plus the transitive half of try_onion_seed_fetch in connman.c).
 *
 * Two defects are pinned here.
 *
 *  1. The peer directory used to be written once at boot and never again:
 *     no refresh, no last_seen maintenance, no expiry, and
 *     /directory.json handed out up to 500 rows with nothing on them a
 *     reader could use to tell a minute-old row from a week-old one.
 *     Covered below: the pure freshness rule, expiry on a refresh round,
 *     census observations moving (or deliberately NOT moving) last_seen,
 *     and the age/policy fields on every served row.
 *
 *  2. try_onion_seed_fetch string-scanned a fetched /directory.json for
 *     clearnet_ip and threw the "onion" field away, so an onion peer
 *     could never teach this node about another onion peer. Covered
 *     below: the parser for that field (validation, dedupe, self-skip,
 *     per-object field binding, the per-response cap) and the follow
 *     budget that stops one response from dominating the pool.
 *
 * The load-bearing property throughout: a directory record is a HINT
 * ABOUT WHERE TO LOOK, never proof of who is there. So every path here
 * may only ever ADD a place to try. The asserts that matter most are the
 * negative ones — hearsay never overwrites a first-hand row, a failed
 * probe never moves last_seen, an observation for an unknown host never
 * inserts, and nothing in this file can remove a peer from any other
 * source's reach.
 */

#include "test/test_core.h"

#include "platform/time_compat.h"
#include "net/onion_service.h"
#include "net/onion_peer_merge.h"
#include "util/path_check.h"

#include <sqlite3.h>

/* Two well-formed v3 names (56 chars from [a-z2-7] + ".onion") and one
 * that fails the rule in the least obvious way — a '1', which is not in
 * the base32 alphabet. */
#define OD_HOST_A \
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaad.onion"
#define OD_HOST_B \
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbe.onion"
#define OD_HOST_C \
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccf.onion"
#define OD_HOST_BAD \
    "1111111111111111111111111111111111111111111111111111111a.onion"

#define OD_CHECK(label, cond) do { \
    printf("onion_directory: %s... ", (label)); \
    if (cond) { printf("OK\n"); } \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* ── 1. The pure freshness rule ───────────────────────────────────── */

static int od_freshness_rule(void)
{
    int failures = 0;
    const int64_t now = 1800000000;

    OD_CHECK("a row confirmed a minute ago is FRESH",
             onion_directory_freshness(now - 60, now, false) == ONION_DIR_FRESH);
    OD_CHECK("a row one second inside the stale window is still FRESH",
             onion_directory_freshness(now - (ONION_DIR_STALE_SECS - 1), now,
                                       false) == ONION_DIR_FRESH);
    OD_CHECK("a row exactly at the stale threshold is STALE",
             onion_directory_freshness(now - ONION_DIR_STALE_SECS, now,
                                       false) == ONION_DIR_STALE);
    OD_CHECK("a day-old row is STALE, not dropped",
             onion_directory_freshness(now - 86400, now, false) ==
                 ONION_DIR_STALE);
    OD_CHECK("a row past the expiry window is EXPIRED",
             onion_directory_freshness(now - ONION_DIR_EXPIRE_SECS, now,
                                       false) == ONION_DIR_EXPIRED);
    OD_CHECK("a week-old row — the exact bug — is EXPIRED",
             onion_directory_freshness(now - 7 * 86400 - 1, now, false) ==
                 ONION_DIR_EXPIRED);
    OD_CHECK("a row with no stamp at all has no provenance and is EXPIRED",
             onion_directory_freshness(0, now, false) == ONION_DIR_EXPIRED);
    OD_CHECK("a negative stamp is EXPIRED",
             onion_directory_freshness(-5, now, false) == ONION_DIR_EXPIRED);
    /* Our own presence is not hearsay: it is never aged out, whatever the
     * stamp says, because dropping it would stop us advertising ourselves. */
    OD_CHECK("our own row is FRESH even with an ancient stamp",
             onion_directory_freshness(1, now, true) == ONION_DIR_FRESH);
    OD_CHECK("our own row is FRESH even with no stamp",
             onion_directory_freshness(0, now, true) == ONION_DIR_FRESH);
    /* Peer clock skew must never buy a row extra life beyond "now". */
    OD_CHECK("a future stamp is clamped to age 0, never negative",
             onion_directory_age_secs(now + 9999, now) == 0);
    OD_CHECK("a future stamp reads FRESH, not as a wrapped-around expiry",
             onion_directory_freshness(now + 9999, now, false) ==
                 ONION_DIR_FRESH);
    OD_CHECK("age is the plain difference otherwise",
             onion_directory_age_secs(now - 4242, now) == 4242);
    return failures;
}

/* ── 2. Parsing the onion half of a /directory.json ───────────────── */

static int od_parse_relay_hints(void)
{
    int failures = 0;
    struct onion_relay_hint hints[16];

    /* A response shaped exactly like the one this node serves. */
    static const char BODY[] =
        "{\"nodes\":["
        "{\"onion\":\"" OD_HOST_A "\",\"port\":8033,\"services\":1029,"
        "\"height\":3196556,\"last_seen\":1799999000,\"version\":\"0.1.0\","
        "\"self\":true,\"clearnet_ip\":\"1.2.3.4\",\"clearnet_port\":8033},"
        "{\"onion\":\"" OD_HOST_B "\",\"port\":9033,\"services\":0,"
        "\"height\":42,\"last_seen\":1799998000,\"version\":\"0.1.0\","
        "\"self\":false,\"clearnet_ip\":\"\",\"clearnet_port\":0}"
        "],\"count\":2}";

    memset(hints, 0, sizeof(hints));
    int n = onion_directory_parse_relay_hints(BODY, NULL, hints, 16);
    OD_CHECK("both advertised onions are parsed (the field used to be dropped)",
             n == 2);
    OD_CHECK("first hostname is carried verbatim",
             n == 2 && strcmp(hints[0].hostname, OD_HOST_A) == 0);
    OD_CHECK("second hostname is carried verbatim",
             n == 2 && strcmp(hints[1].hostname, OD_HOST_B) == 0);
    OD_CHECK("each entry keeps its OWN port, not the next object's",
             n == 2 && hints[0].port == 8033 && hints[1].port == 9033);
    OD_CHECK("each entry keeps its OWN height",
             n == 2 && hints[0].height == 3196556 && hints[1].height == 42);
    OD_CHECK("each entry keeps its OWN last_seen",
             n == 2 && hints[0].last_seen == 1799999000 &&
             hints[1].last_seen == 1799998000);
    OD_CHECK("clearnet_port is not mistaken for port",
             n == 2 && hints[1].port == 9033);

    /* Asking about ourselves must not learn ourselves. */
    memset(hints, 0, sizeof(hints));
    n = onion_directory_parse_relay_hints(BODY, OD_HOST_A, hints, 16);
    OD_CHECK("our own hostname is skipped when the peer advertises it",
             n == 1 && strcmp(hints[0].hostname, OD_HOST_B) == 0);

    /* Every hostname goes through the one v3 rule regardless of source. */
    static const char HOSTILE[] =
        "{\"nodes\":["
        "{\"onion\":\"" OD_HOST_BAD "\",\"port\":8033},"
        "{\"onion\":\"short.onion\",\"port\":8033},"
        "{\"onion\":\"\",\"port\":8033},"
        "{\"onion\":\"" OD_HOST_A "\",\"port\":8033}"
        "]}";
    memset(hints, 0, sizeof(hints));
    n = onion_directory_parse_relay_hints(HOSTILE, NULL, hints, 16);
    OD_CHECK("malformed hostnames are dropped, the valid one survives",
             n == 1 && strcmp(hints[0].hostname, OD_HOST_A) == 0);

    /* A peer repeating one hostname must not consume the whole budget. */
    static const char REPEATED[] =
        "{\"nodes\":["
        "{\"onion\":\"" OD_HOST_A "\",\"port\":1},"
        "{\"onion\":\"" OD_HOST_A "\",\"port\":2},"
        "{\"onion\":\"" OD_HOST_A "\",\"port\":3},"
        "{\"onion\":\"" OD_HOST_B "\",\"port\":4}"
        "]}";
    memset(hints, 0, sizeof(hints));
    n = onion_directory_parse_relay_hints(REPEATED, NULL, hints, 16);
    OD_CHECK("a repeated hostname is deduped within one response", n == 2);

    /* The per-response cap: one node's answer cannot dominate the pool. */
    memset(hints, 0, sizeof(hints));
    n = onion_directory_parse_relay_hints(BODY, NULL, hints, 1);
    OD_CHECK("the caller's cap bounds one response's contribution", n == 1);

    /* Degenerate inputs are a clean zero, never a read past the buffer. */
    memset(hints, 0, sizeof(hints));
    OD_CHECK("a body with no onion field yields nothing",
             onion_directory_parse_relay_hints("{\"nodes\":[]}", NULL,
                                               hints, 16) == 0);
    OD_CHECK("a truncated onion value yields nothing",
             onion_directory_parse_relay_hints("{\"onion\":\"aaaa", NULL,
                                               hints, 16) == 0);
    OD_CHECK("an over-long onion value is rejected, not truncated into a host",
             onion_directory_parse_relay_hints(
                 "{\"onion\":\"" OD_HOST_A OD_HOST_A OD_HOST_A "\"}",
                 NULL, hints, 16) == 0);
    OD_CHECK("NULL body is a clean zero",
             onion_directory_parse_relay_hints(NULL, NULL, hints, 16) == 0);
    OD_CHECK("zero capacity is a clean zero",
             onion_directory_parse_relay_hints(BODY, NULL, hints, 0) == 0);
    return failures;
}

/* ── 3. The follow budget ─────────────────────────────────────────── */

static int od_follow_budget(void)
{
    int failures = 0;
    const int64_t t0 = 1800000000;

    onion_directory_reset_relay_follow();

    OD_CHECK("a malformed hostname can never claim a fetch",
             !onion_directory_claim_relay_follow(OD_HOST_BAD, t0));
    OD_CHECK("NULL hostname can never claim a fetch",
             !onion_directory_claim_relay_follow(NULL, t0));

    OD_CHECK("the first claim on a fresh window succeeds",
             onion_directory_claim_relay_follow(OD_HOST_A, t0));
    OD_CHECK("the same hostname cannot be followed twice in one window",
             !onion_directory_claim_relay_follow(OD_HOST_A, t0));
    OD_CHECK("a different hostname still gets through",
             onion_directory_claim_relay_follow(OD_HOST_B, t0));

    /* Exhaust the rest of the budget with distinct hostnames, then prove
     * the (budget+1)th is refused — this is the cap that stops a
     * directory response from turning into an unbounded crawl. */
    onion_directory_reset_relay_follow();
    int granted = 0;
    for (int i = 0; i < ONION_RELAY_FOLLOW_BUDGET + 8; i++) {
        char host[64];
        snprintf(host, sizeof(host), "%s", OD_HOST_A);
        /* Vary two base32 chars so each name is distinct and still valid. */
        host[0] = (char)('a' + (i % 26));
        host[1] = (char)('a' + ((i / 26) % 26));
        if (!onion_hostname_valid(host)) continue;
        if (onion_directory_claim_relay_follow(host, t0)) granted++;
    }
    OD_CHECK("the follow budget is a hard cap per window",
             granted == ONION_RELAY_FOLLOW_BUDGET);

    /* Rolling the window refills the budget AND clears the dedupe ring,
     * so a host is never locked out for the life of the process. */
    OD_CHECK("an exhausted budget refuses a new host inside the window",
             !onion_directory_claim_relay_follow(OD_HOST_C, t0 + 1));
    OD_CHECK("rolling the window refills the budget",
             onion_directory_claim_relay_follow(
                 OD_HOST_C, t0 + ONION_RELAY_WINDOW_SECS));
    OD_CHECK("a previously-followed host is reachable again next window",
             onion_directory_claim_relay_follow(
                 OD_HOST_A, t0 + ONION_RELAY_WINDOW_SECS));

    onion_directory_reset_relay_follow();
    return failures;
}

/* ── 4. The durable directory: refresh, observe, learn, serve ─────── */

static sqlite3 *od_open_db(const char *datadir)
{
    char path[1024];
    zcl_node_db_path(path, sizeof(path), datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 2000);
    return db;
}

/* Read one integer column for one host. -1 when the row is gone. */
static int64_t od_row_int(sqlite3 *db, const char *host, const char *col)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT %s FROM peer_directory WHERE onion_address = ?", col);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK || !s)
        return -1;
    sqlite3_bind_text(s, 1, host, -1, SQLITE_STATIC);
    int64_t v = -1;
    if (sqlite3_step(s) == SQLITE_ROW)
        v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static bool od_row_source(sqlite3 *db, const char *host, char *out, size_t n)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(source,'') FROM peer_directory "
            "WHERE onion_address = ?", -1, &s, NULL) != SQLITE_OK || !s)
        return false;
    sqlite3_bind_text(s, 1, host, -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        snprintf(out, n, "%s", v ? v : "");
        found = true;
    }
    sqlite3_finalize(s);
    return found;
}

static void od_insert_row(sqlite3 *db, const char *host, int64_t last_seen,
                          const char *source)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO peer_directory "
            "(onion_address, port, services, height, first_seen, last_seen,"
            " last_probe, probe_ok, fail_count, version, self,"
            " clearnet_ip, clearnet_port, source) "
            "VALUES (?, 8033, 0, 0, ?, ?, 0, 0, 0, 'test', 0, '', 0, ?)",
            -1, &s, NULL) != SQLITE_OK || !s)
        return;
    sqlite3_bind_text(s, 1, host, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, last_seen);
    sqlite3_bind_int64(s, 3, last_seen);
    sqlite3_bind_text(s, 4, source, -1, SQLITE_STATIC);
    (void)sqlite3_step(s);
    sqlite3_finalize(s);
}

static int od_durable_directory(void)
{
    int failures = 0;

    /* onion_service_start keeps the datadir POINTER, so it must outlive
     * every call below — static, not a stack buffer. */
    static char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "onion_directory", "db");
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);

    onion_service_start(dir);

    sqlite3 *db = od_open_db(dir);
    if (!db) {
        printf("onion_directory: could not open the test node db... FAIL\n");
        onion_service_stop();
        test_cleanup_tmpdir(dir);
        return 1;
    }

    const int64_t now = (int64_t)platform_time_wall_time_t();

    /* ── learn(): hearsay may only ADD ───────────────────────────── */
    OD_CHECK("a malformed relayed hostname is refused",
             !onion_service_directory_learn(OD_HOST_BAD, 8033, 1, now - 60));
    OD_CHECK("a valid relayed hostname is recorded",
             onion_service_directory_learn(OD_HOST_A, 8033, 99, now - 60));
    { char src[32] = "";
      OD_CHECK("a relayed row is marked as hearsay, not as our own measurement",
               od_row_source(db, OD_HOST_A, src, sizeof(src)) &&
               strcmp(src, "relay") == 0); }
    OD_CHECK("a relayed stamp already past expiry is refused outright",
             !onion_service_directory_learn(OD_HOST_C, 8033, 1,
                                            now - ONION_DIR_EXPIRE_SECS - 1));
    OD_CHECK("the refused stale hearsay left no row behind",
             od_row_int(db, OD_HOST_C, "last_seen") == -1);

    /* A first-hand row must survive a peer telling us about it. */
    od_insert_row(db, OD_HOST_B, now - 120, "discovery");
    OD_CHECK("learn() on a host we already know reports success",
             onion_service_directory_learn(OD_HOST_B, 1234, 7, now - 3600));
    { char src[32] = "";
      OD_CHECK("hearsay never overwrites a first-hand row",
               od_row_source(db, OD_HOST_B, src, sizeof(src)) &&
               strcmp(src, "discovery") == 0); }
    OD_CHECK("hearsay never rewrites a first-hand last_seen",
             od_row_int(db, OD_HOST_B, "last_seen") == now - 120);

    /* ── observe(): the census bridge ────────────────────────────── */
    struct onion_directory_observation obs[3];
    struct onion_directory_refresh_stats st;

    memset(obs, 0, sizeof(obs));
    snprintf(obs[0].hostname, sizeof(obs[0].hostname), "%s", OD_HOST_A);
    obs[0].reachable = true;
    obs[0].observed_unix = now - 5;
    obs[0].best_height = 4242;
    OD_CHECK("a reachable observation is applied",
             onion_service_directory_observe(obs, 1, &st) == 1 &&
             st.refreshed == 1);
    OD_CHECK("a reachable observation moves last_seen forward",
             od_row_int(db, OD_HOST_A, "last_seen") == now - 5);
    OD_CHECK("a reachable observation records probe success",
             od_row_int(db, OD_HOST_A, "probe_ok") == 1);
    OD_CHECK("a reachable observation adopts the higher height",
             od_row_int(db, OD_HOST_A, "height") == 4242);

    /* A failed dial is not evidence of absence — it must NOT move
     * last_seen, only stop being evidence of presence. */
    int64_t before = od_row_int(db, OD_HOST_A, "last_seen");
    memset(obs, 0, sizeof(obs));
    snprintf(obs[0].hostname, sizeof(obs[0].hostname), "%s", OD_HOST_A);
    obs[0].reachable = false;
    obs[0].observed_unix = now;
    obs[0].best_height = -1;
    OD_CHECK("an unreachable observation is applied",
             onion_service_directory_observe(obs, 1, &st) == 1 &&
             st.failed == 1);
    OD_CHECK("an unreachable observation does NOT move last_seen",
             od_row_int(db, OD_HOST_A, "last_seen") == before);
    OD_CHECK("an unreachable observation bumps fail_count",
             od_row_int(db, OD_HOST_A, "fail_count") == 1);
    OD_CHECK("an unreachable observation clears probe_ok",
             od_row_int(db, OD_HOST_A, "probe_ok") == 0);
    OD_CHECK("an unreachable observation never lowers the height",
             od_row_int(db, OD_HOST_A, "height") == 4242);

    /* An observation naming a host we do not have may not INSERT one:
     * a lying census must not be able to add a peer. */
    memset(obs, 0, sizeof(obs));
    snprintf(obs[0].hostname, sizeof(obs[0].hostname), "%s", OD_HOST_C);
    obs[0].reachable = true;
    obs[0].observed_unix = now;
    OD_CHECK("an observation for an unknown host is counted, not applied",
             onion_service_directory_observe(obs, 1, &st) == 0 &&
             st.unknown == 1);
    OD_CHECK("an observation for an unknown host inserts nothing",
             od_row_int(db, OD_HOST_C, "last_seen") == -1);

    memset(obs, 0, sizeof(obs));
    snprintf(obs[0].hostname, sizeof(obs[0].hostname), "%s", OD_HOST_BAD);
    obs[0].reachable = true;
    OD_CHECK("an observation with a malformed hostname is rejected",
             onion_service_directory_observe(obs, 1, &st) == 0 &&
             st.unknown == 1);
    OD_CHECK("an empty observation batch is not an error",
             onion_service_directory_observe(obs, 0, &st) == 0);

    /* ── refresh(): the round that was never running ─────────────── */
    od_insert_row(db, OD_HOST_C, now - ONION_DIR_EXPIRE_SECS - 60, "discovery");
    OD_CHECK("the week-old row exists before the refresh round",
             od_row_int(db, OD_HOST_C, "last_seen") > 0);
    OD_CHECK("a refresh round completes against a writable directory",
             onion_service_directory_refresh(&st));
    OD_CHECK("the refresh round expired exactly the aged-out row",
             st.expired == 1);
    OD_CHECK("the aged-out row is gone",
             od_row_int(db, OD_HOST_C, "last_seen") == -1);
    OD_CHECK("rows still inside the window survive the refresh",
             od_row_int(db, OD_HOST_A, "last_seen") > 0 &&
             od_row_int(db, OD_HOST_B, "last_seen") > 0);

    /* ── serve: every row carries its own age ────────────────────── */
    od_insert_row(db, OD_HOST_C, now - ONION_DIR_EXPIRE_SECS - 60, "discovery");
    static uint8_t resp[262144];
    memset(resp, 0, sizeof(resp));
    size_t rn = onion_service_handle_request("GET", "/directory.json", NULL, 0,
                                             resp, sizeof(resp));
    const char *json = (const char *)resp;
    OD_CHECK("/directory.json still answers", rn > 0 && strstr(json, "200 OK"));
    OD_CHECK("every served row carries its own age",
             strstr(json, "\"age_secs\":") != NULL);
    OD_CHECK("every served row carries a stale flag",
             strstr(json, "\"stale\":") != NULL);
    OD_CHECK("every served row says where it came from",
             strstr(json, "\"source\":") != NULL);
    OD_CHECK("the response states when it was generated",
             strstr(json, "\"generated_at\":") != NULL);
    OD_CHECK("the response states the freshness policy it applied",
             strstr(json, "\"stale_after_secs\":") != NULL &&
             strstr(json, "\"expire_after_secs\":") != NULL);
    OD_CHECK("a row inside the window is served",
             strstr(json, OD_HOST_A) != NULL);
    OD_CHECK("an aged-out row is NOT served as if it were current",
             strstr(json, OD_HOST_C) == NULL);
    OD_CHECK("the response admits how many rows it withheld",
             strstr(json, "\"skipped_expired\":1") != NULL);

    sqlite3_close(db);
    onion_service_stop();
    test_cleanup_tmpdir(dir);
    return failures;
}

/* ── Entry point ──────────────────────────────────────────────────── */

int test_onion_directory(void)
{
    int failures = 0;
    printf("\n=== Onion Directory Freshness + Onion Graph Tests ===\n");
    failures += od_freshness_rule();
    failures += od_parse_relay_hints();
    failures += od_follow_budget();
    failures += od_durable_directory();
    return failures;
}
