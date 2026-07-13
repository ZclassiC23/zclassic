/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the ZCL Names HTML site slice
 * (app/controllers/src/name_site_controller.c + app/views/src/name_view.c):
 *   - name→site resolution precedence (onion > url > profile fallback)
 *   - browse index / profile show render
 *   - register POST refusals (missing CSRF, missing PoW)
 *   - the name-bound proof-of-work gate (verify + single-use replay refusal)
 *   - the name_records relationship RPC
 *
 * Model validation, projection fold (register→update→transfer→renew→expire),
 * and rebuild-from-scratch idempotence are covered by test_znam.c /
 * test_znam_projection.c; this file exercises the request-path surface built
 * on top of them. */

#include "test/test_helpers.h"
#include "controllers/name_site_controller.h"
#include "controllers/name_controller.h"
#include "models/znam.h"
#include "models/database.h"
#include "rpc/server.h"
#include "json/json.h"
#include "crypto/sha3.h"
#include "net/fast_sync.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TS_CHECK(label, cond)                                             \
    do {                                                                  \
        if (!(cond)) { printf("  FAIL: %s\n", (label)); failures++; }     \
    } while (0)

static bool open_site_db(sqlite3 **db_out, struct node_db *ndb_out)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return false;
    sqlite3_exec(db,
        "CREATE TABLE znam_names("
        "name TEXT PRIMARY KEY, owner_address TEXT,"
        "target_type INTEGER, target_value TEXT,"
        "reg_txid BLOB, reg_height INTEGER,"
        "last_update_txid BLOB,"
        "expiry_height INTEGER NOT NULL DEFAULT 0)", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE znam_text_records("
        "name TEXT, key TEXT, value TEXT,"
        "PRIMARY KEY(name,key))", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TABLE znam_addr_records("
        "name TEXT, coin_type INTEGER, address TEXT,"
        "PRIMARY KEY(name,coin_type))", NULL, NULL, NULL);
    *db_out = db;
    memset(ndb_out, 0, sizeof(*ndb_out));
    ndb_out->db = db;
    ndb_out->open = true;
    return true;
}

static bool seed_name(struct node_db *ndb, const char *name,
                      uint8_t type, const char *value)
{
    struct znam_entry e;
    memset(&e, 0, sizeof(e));
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.owner_address, sizeof(e.owner_address), "t1Owner%s", name);
    e.target_type = type;
    snprintf(e.target_value, sizeof(e.target_value), "%s", value);
    memset(e.reg_txid, 0xAB, 32);          /* non-zero — validator requires it */
    memset(e.last_update_txid, 0xAB, 32);
    e.reg_height = 100;
    e.expiry_height = 210340;
    return db_znam_save(ndb, &e);
}

/* Solve the name-bound register puzzle and format ts/nonce as decimal
 * strings, exactly as the browser/curl client would submit them. */
static void solve_name_pow(const char *name, char ts_out[32], char nonce_out[32])
{
    char ctx[128];
    uint8_t peer[32];
    int n = snprintf(ctx, sizeof(ctx), "znam:register:pow:%s", name);
    sha3_256((const unsigned char *)ctx, (size_t)n, peer);
    struct fast_sync_pow pow;
    memset(&pow, 0, sizeof(pow));
    fast_sync_solve_pow(peer, &pow);
    snprintf(ts_out, 32, "%lld", (long long)pow.timestamp);
    snprintf(nonce_out, 32, "%llu", (unsigned long long)pow.nonce);
}

static int t_resolution_precedence(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!open_site_db(&db, &ndb)) return 1;
    rpc_name_set_state(&ndb);

    /* onion via primary target, plus a competing url text record → onion wins */
    TS_CHECK("seed onionpri", seed_name(&ndb, "onionpri", ZNAM_TYPE_ONION,
                                        "aaaa.onion"));
    db_znam_text_save(&ndb, "onionpri", "url", "http://example.com");

    /* onion via explicit text record (primary is a t-addr) → onion still wins */
    TS_CHECK("seed oniontxt", seed_name(&ndb, "oniontxt", ZNAM_TYPE_TADDR,
                                        "t1Addr"));
    db_znam_text_save(&ndb, "oniontxt", "onion", "bbbb.onion");
    db_znam_text_save(&ndb, "oniontxt", "url", "http://nope.com");

    /* url only → redirect to url */
    TS_CHECK("seed urlsite", seed_name(&ndb, "urlsite", ZNAM_TYPE_TADDR,
                                       "t1Addr"));
    db_znam_text_save(&ndb, "urlsite", "url", "https://site.example");

    /* nothing routable → profile page fallback */
    TS_CHECK("seed profonly", seed_name(&ndb, "profonly", ZNAM_TYPE_TADDR,
                                        "t1AddrProfile"));

    uint8_t resp[65536];
    size_t nb;

    nb = name_site_handle_request("GET", "/n/onionpri", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("onion primary -> 302", strstr((char *)resp, "302 Found") != NULL);
    TS_CHECK("onion primary Location",
             strstr((char *)resp, "Location: http://aaaa.onion/") != NULL);

    nb = name_site_handle_request("GET", "/n/oniontxt", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("onion text beats url",
             strstr((char *)resp, "Location: http://bbbb.onion/") != NULL);

    nb = name_site_handle_request("GET", "/n/urlsite", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("url -> 302", strstr((char *)resp, "302 Found") != NULL);
    TS_CHECK("url Location",
             strstr((char *)resp, "Location: https://site.example") != NULL);

    nb = name_site_handle_request("GET", "/n/profonly", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("profile fallback 200", strstr((char *)resp, "200 OK") != NULL);
    TS_CHECK("profile shows name", strstr((char *)resp, "profonly") != NULL);
    TS_CHECK("profile shows owner",
             strstr((char *)resp, "t1Ownerprofonly") != NULL);

    /* unknown name → 404 */
    nb = name_site_handle_request("GET", "/n/ghost", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("unknown -> 404", strstr((char *)resp, "404 Not Found") != NULL);

    /* invalid name (uppercase) → 404, never a resolution */
    nb = name_site_handle_request("GET", "/n/BadName", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("invalid name -> 404", strstr((char *)resp, "404 Not Found") != NULL);

    rpc_name_set_state(NULL);
    sqlite3_close(db);
    return failures;
}

static int t_index_and_show(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!open_site_db(&db, &ndb)) return 1;
    rpc_name_set_state(&ndb);

    seed_name(&ndb, "alpha", ZNAM_TYPE_ONION, "alpha.onion");
    seed_name(&ndb, "beta", ZNAM_TYPE_TADDR, "t1Beta");
    db_znam_text_save(&ndb, "beta", "email", "beta@example.com");

    uint8_t resp[65536];
    size_t nb;

    nb = name_site_handle_request("GET", "/names", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("index 200", strstr((char *)resp, "200 OK") != NULL);
    TS_CHECK("index lists alpha", strstr((char *)resp, "alpha") != NULL);
    TS_CHECK("index lists beta", strstr((char *)resp, "beta") != NULL);

    nb = name_site_handle_request("GET", "/names/beta", NULL, 0, resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("show beta 200", strstr((char *)resp, "200 OK") != NULL);
    TS_CHECK("show beta record",
             strstr((char *)resp, "beta@example.com") != NULL);

    /* register form renders with the CSRF token + embeds the PoW solver */
    nb = name_site_handle_request("GET", "/names/register", NULL, 0,
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("register form 200", strstr((char *)resp, "200 OK") != NULL);
    TS_CHECK("register form has csrf",
             strstr((char *)resp, "csrf_token") != NULL);
    TS_CHECK("register form has pow solver",
             strstr((char *)resp, "namePowSolveChunked") != NULL);

    rpc_name_set_state(NULL);
    sqlite3_close(db);
    return failures;
}

static int t_register_refusals(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!open_site_db(&db, &ndb)) return 1;
    rpc_name_set_state(&ndb);

    uint8_t resp[65536];
    size_t nb;

    /* Default-off: without the operator opt-in, POST is refused before
     * CSRF/PoW even run (public registration spends the node wallet). */
    unsetenv("ZCL_NAMES_PUBLIC_REGISTER");
    const char *any = "name=alice&type=onion&value=alice.onion";
    nb = name_site_handle_request("POST", "/names/register",
                                  (const uint8_t *)any, strlen(any),
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("public-register default-off refused",
             strstr((char *)resp, "disabled") != NULL);

    /* Opt in for the remaining gate-order checks. */
    setenv("ZCL_NAMES_PUBLIC_REGISTER", "1", 1);

    /* No CSRF token → refused before anything else. */
    const char *no_csrf = "name=alice&type=onion&value=alice.onion";
    nb = name_site_handle_request("POST", "/names/register",
                                  (const uint8_t *)no_csrf, strlen(no_csrf),
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("no-csrf refused", strstr((char *)resp, "CSRF") != NULL);

    /* Valid CSRF but no PoW → refused at the PoW gate (proves the gate fires
     * after CSRF passes). */
    char csrf[33];
    name_site_csrf_token(csrf);
    char body[256];
    snprintf(body, sizeof(body),
             "name=alice&type=onion&value=alice.onion&csrf_token=%s", csrf);
    nb = name_site_handle_request("POST", "/names/register",
                                  (const uint8_t *)body, strlen(body),
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("no-pow refused", strstr((char *)resp, "Proof-of-work") != NULL);

    /* Valid CSRF + valid PoW but a bad name → refused at validation. */
    char ts[32], nonce[32];
    solve_name_pow("BADNAME", ts, nonce); /* bind matches the submitted name */
    snprintf(body, sizeof(body),
             "name=BADNAME&type=onion&value=x.onion&csrf_token=%s"
             "&pow_ts=%s&pow_nonce=%s", csrf, ts, nonce);
    nb = name_site_handle_request("POST", "/names/register",
                                  (const uint8_t *)body, strlen(body),
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("bad-name refused", strstr((char *)resp, "Invalid name") != NULL);

    /* Full valid CSRF + PoW for a good name → reaches compose, which refuses
     * because no wallet is wired in this unit fixture. Proves the whole gate
     * chain passes end to end. */
    solve_name_pow("postname", ts, nonce);
    snprintf(body, sizeof(body),
             "name=postname&type=onion&value=post.onion&csrf_token=%s"
             "&pow_ts=%s&pow_nonce=%s", csrf, ts, nonce);
    nb = name_site_handle_request("POST", "/names/register",
                                  (const uint8_t *)body, strlen(body),
                                  resp, sizeof(resp));
    resp[nb < sizeof(resp) ? nb : sizeof(resp) - 1] = '\0';
    TS_CHECK("valid gate reaches compose (wallet not loaded)",
             strstr((char *)resp, "wallet not loaded") != NULL);

    rpc_name_set_state(NULL);
    sqlite3_close(db);
    return failures;
}

static int t_pow_gate_single_use(void)
{
    int failures = 0;
    char ts[32], nonce[32];
    solve_name_pow("powname", ts, nonce);

    /* First presentation verifies + claims. */
    TS_CHECK("pow accepted once",
             name_pow_verify_and_claim("powname", ts, nonce));
    /* Replay of the same solution is refused (single-use ring). */
    TS_CHECK("pow replay refused",
             !name_pow_verify_and_claim("powname", ts, nonce));
    /* Same solution bound to a DIFFERENT name never verifies (name-bound). */
    TS_CHECK("pow not portable across names",
             !name_pow_verify_and_claim("othername", ts, nonce));
    /* Malformed fields refused, not crashed. */
    TS_CHECK("pow empty refused",
             !name_pow_verify_and_claim("powname", "", ""));
    TS_CHECK("pow garbage refused",
             !name_pow_verify_and_claim("powname", "notanumber", "x"));

    return failures;
}

static int t_name_records_rpc(void)
{
    int failures = 0;
    sqlite3 *db = NULL;
    struct node_db ndb;
    if (!open_site_db(&db, &ndb)) return 1;
    rpc_name_set_state(&ndb);

    struct rpc_table t;
    rpc_table_init(&t);
    register_name_rpc_commands(&t);

    seed_name(&ndb, "recname", ZNAM_TYPE_TADDR, "t1Rec");
    db_znam_text_save(&ndb, "recname", "url", "https://rec.example");
    db_znam_addr_save(&ndb, "recname", ZNAM_TYPE_BTC, "bc1qrec");

    const struct rpc_command *cmd = rpc_table_find(&t, "name_records");
    TS_CHECK("name_records registered", cmd != NULL);

    if (cmd) {
        struct json_value params = {0}, arg = {0}, result = {0};
        json_set_array(&params);
        json_set_str(&arg, "recname");
        json_push_back(&params, &arg);
        json_free(&arg);
        bool ok = cmd->actor(&params, false, &result);
        TS_CHECK("name_records ok", ok);
        TS_CHECK("name_records found",
                 json_get_bool(json_get(&result, "found")));
        const struct json_value *texts = json_get(&result, "text_records");
        const struct json_value *addrs = json_get(&result, "address_records");
        TS_CHECK("name_records has text", texts && json_size(texts) == 1);
        TS_CHECK("name_records has addr", addrs && json_size(addrs) == 1);
        json_free(&params);
        json_free(&result);
    }

    rpc_name_set_state(NULL);
    sqlite3_close(db);
    return failures;
}

int test_znam_site(void)
{
    int failures = 0;
    printf("\n=== znam site (resolution + register) tests ===\n");
    failures += t_resolution_precedence();
    failures += t_index_and_show();
    failures += t_register_refusals();
    failures += t_pow_gate_single_use();
    failures += t_name_records_rpc();
    printf("znam_site: %d failures\n", failures);
    return failures;
}
