/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the native dbquery secret-material denylist
 * (app/controllers/src/dbquery_controller.c: dbq_secret_hit()).
 *
 * Prior to this fix, `dbquery` was SELECT-only and
 * DDL/DML-blocked but had no notion of *which* SELECTs were safe:
 * `SELECT privkey FROM wallet_keys` was legal SQL that dumped the
 * plaintext keystore through any read-only query surface.
 *
 * These tests exercise diag_rpc_dbquery() directly against a real
 * node.db (":memory:") opened through the normal node_db_open() /
 * db_service_* / app_runtime_set_current() path — the exact seam the
 * live handler reads via app_runtime_node_db() — so we're proving the
 * real code path, not a mock. */

#include "test/test_helpers.h"
#include "controllers/diagnostics_internal.h"
#include "config/db_service.h"
#include "config/runtime.h"

#define DDT_RUN(name, expr) do { \
    printf("%s... ", (name));    \
    bool _ok = (expr);           \
    if (_ok) printf("OK\n");     \
    else { printf("FAIL\n"); failures++; } \
} while (0)

struct ddt_fixture {
    struct node_db ndb;
    struct db_service dbsvc;
    struct app_runtime_context runtime;
};

static bool ddt_fixture_init(struct ddt_fixture *f)
{
    memset(f, 0, sizeof(*f));
    if (!node_db_open(&f->ndb, ":memory:"))
        return false;
    db_service_init(&f->dbsvc);
    if (!db_service_attach(&f->dbsvc, &f->ndb)) return false;
    if (!db_service_start(&f->dbsvc)) return false;
    f->runtime.db_service = &f->dbsvc;
    app_runtime_set_current(&f->runtime);
    return true;
}

static void ddt_fixture_tear_down(struct ddt_fixture *f)
{
    app_runtime_set_current(NULL);
    db_service_stop(&f->dbsvc);
    node_db_close(&f->ndb);
}

/* Runs `sql` through the real dbquery RPC handler. Returns the
 * handler's own bool result (true = executed, false = rejected at
 * validation) and hands back the JSON body it produced. */
static bool ddt_query(const char *sql, struct json_value *result)
{
    struct json_value params;
    struct json_value sql_v;
    json_init(&params);
    json_init(&sql_v);
    json_set_array(&params);
    json_set_str(&sql_v, sql);
    json_push_back(&params, &sql_v);
    json_free(&sql_v);

    json_init(result);
    bool rc = diag_rpc_dbquery(&params, /*help=*/false, result);
    json_free(&params);
    return rc;
}

/* 1. The three secret tables are denied wholesale, in the plainest
 *    form the security eval reported: `SELECT * FROM <table>`. */
static int t_secret_table_denied(void)
{
    int failures = 0;
    static const char *tables[] = {
        "wallet_keys", "wallet_sapling_keys", "wallet_seed",
    };
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "SELECT * FROM %s LIMIT 1", tables[i]);
        struct json_value result;
        bool rc = ddt_query(sql, &result);
        char label[160];
        snprintf(label, sizeof(label), "dbquery: SELECT * FROM %s denied",
                 tables[i]);
        DDT_RUN(label, !rc && result.type == JSON_STR &&
                strstr(json_get_str(&result), "secret") != NULL);
        json_free(&result);
    }
    return failures;
}

/* 2. Named secret columns are denied even when the query is written
 *    against the exact reported repro string. */
static int t_secret_column_denied(void)
{
    int failures = 0;

    struct json_value result;
    bool rc = ddt_query("SELECT privkey FROM wallet_keys", &result);
    DDT_RUN("dbquery: SELECT privkey FROM wallet_keys denied (repro)",
            !rc && result.type == JSON_STR);
    json_free(&result);

    rc = ddt_query("SELECT xsk FROM wallet_sapling_keys", &result);
    DDT_RUN("dbquery: SELECT xsk (spending key) denied", !rc);
    json_free(&result);

    rc = ddt_query("SELECT seed FROM wallet_seed", &result);
    DDT_RUN("dbquery: SELECT seed (HD seed) denied", !rc);
    json_free(&result);

    /* Column keyword catches a secret-named column even projected
     * alongside non-secret columns from the same table. */
    rc = ddt_query("SELECT pubkey_hash, privkey FROM wallet_keys", &result);
    DDT_RUN("dbquery: privkey denied even mixed with public columns", !rc);
    json_free(&result);

    return failures;
}

/* 3. Ordinary, non-secret queries still work — the denylist must not
 *    collapse into "reject everything". Includes the exact
 *    sqlite_master probe the agent-contract test suite depends on
 *    (test_syncdiag_rpc.c), which must NOT collide with the "master"
 *    keyword because of the underscore word-boundary. */
static int t_normal_query_allowed(void)
{
    int failures = 0;

    struct json_value result;
    bool rc = ddt_query("SELECT * FROM blocks LIMIT 1", &result);
    DDT_RUN("dbquery: SELECT * FROM blocks LIMIT 1 still works",
            rc && result.type == JSON_OBJ &&
            json_get(&result, "columns") != NULL);
    json_free(&result);

    rc = ddt_query("SELECT txid FROM wallet_transactions LIMIT 1", &result);
    DDT_RUN("dbquery: non-secret wallet table (wallet_transactions) allowed",
            rc && result.type == JSON_OBJ);
    json_free(&result);

    rc = ddt_query(
        "SELECT name FROM sqlite_master WHERE type='table' LIMIT 5",
        &result);
    DDT_RUN("dbquery: sqlite_master probe not blocked by 'master' keyword",
            rc && result.type == JSON_OBJ);
    json_free(&result);

    return failures;
}

/* 4. Obfuscated references — whitespace, case, aliasing, bracket/quote
 *    identifiers, nested subqueries — are still caught (or otherwise
 *    safely rejected, e.g. by the pre-existing "must start with
 *    SELECT" gate on a WITH-CTE form). Every case here must come back
 *    denied; none may leak a row. */
static int t_obfuscation_denied(void)
{
    int failures = 0;
    static const char *sqls[] = {
        /* extra / irregular whitespace */
        "SELECT   *    FROM     wallet_keys",
        "SELECT *\nFROM\twallet_keys",
        /* case variation */
        "select PrivKey from Wallet_Keys",
        /* table alias + qualified column reference */
        "SELECT w.privkey FROM wallet_keys AS w",
        /* nested subquery wrapping the secret table */
        "SELECT * FROM (SELECT privkey FROM wallet_keys) t",
        /* bracket-quoted identifier */
        "SELECT * FROM [wallet_keys]",
        /* double-quoted identifier */
        "SELECT * FROM \"wallet_keys\"",
        /* CTE form: rejected upstream by the "must start with SELECT"
         * gate, not the secret check -- still must not leak. */
        "WITH x AS (SELECT privkey FROM wallet_keys) SELECT * FROM x",
    };
    for (size_t i = 0; i < sizeof(sqls) / sizeof(sqls[0]); i++) {
        struct json_value result;
        bool rc = ddt_query(sqls[i], &result);
        char label[192];
        snprintf(label, sizeof(label), "dbquery: obfuscation case %zu denied",
                 i);
        DDT_RUN(label, !rc && result.type != JSON_OBJ);
        json_free(&result);
    }
    return failures;
}

int test_dbquery_secret_denylist(void)
{
    printf("\n=== dbquery secret denylist tests ===\n");
    int failures = 0;

    struct ddt_fixture f;
    if (!ddt_fixture_init(&f)) {
        printf("dbquery_secret_denylist: fixture init failed\n");
        return 1;
    }

    failures += t_secret_table_denied();
    failures += t_secret_column_denied();
    failures += t_normal_query_allowed();
    failures += t_obfuscation_denied();

    ddt_fixture_tear_down(&f);
    return failures;
}
