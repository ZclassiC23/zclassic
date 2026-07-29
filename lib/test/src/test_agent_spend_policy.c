/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Agent spend policy tests (docs/work/agent-spend-policy-design.md,
 * "Enforcement" lane).
 *
 * ── The seam this file drives, and why ────────────────────────────────────
 * The policy gate runs in the CLI process, which has no node.db, so it reaches
 * the grant store over the node's `agentsession` RPC. These tests install a
 * node_rpc_call test hook that dispatches into the REAL rpc table
 * (register_agent_session_rpc_commands -> controller -> service -> model)
 * against a tmp node_db. Nothing about the store is mocked; only the socket
 * is. That matters: the defect this layer had was a hollow boundary — the
 * earlier tests hand-wired app_runtime and so proved a path the shipped binary
 * cannot take, because app_runtime is only ever populated by the node's boot.
 *
 * Proves:
 *  1. A NULL/empty session id is the explicit local-operator exemption.
 *  2. DEFAULT DENY on the money surface: the key-material and unbounded wallet
 *     leaves (address.export-key, address.import, backup.now, rescan) are
 *     REFUSED for a bounded session even though none carries an `amount` — the
 *     exact bypass that made every cap decorative.
 *  3. The grant surface (vault.session.*) is refused for a bounded session: a
 *     grant that can mint itself a wider grant is not a bound.
 *  4. Understood wallet reads pass and debit nothing.
 *  5. Understood spends: under-limit debits, per-tx and window caps refuse,
 *     revoked/missing fail closed, and a PLAN-stage invocation enforces the
 *     caps while debiting nothing.
 *  6. The allowlist is checked against each leaf's OWN recipient key.
 *  7. amount INT / REAL / decimal string parse identically; junk, negative,
 *     absurd and MISSING all fail closed.
 *  8. A grant store that cannot be reached refuses (fail closed).
 *  9. Kernel hook on RENDERED BYTES: the POLICY_* code, no plan body, the
 *     REDACTED grant id (never the bearer token), and the per-invocation
 *     authority block.
 * 10. `vault send` through execute_json debits EXACTLY ONCE.
 * 11. A handler that fails after an allowed debit gets the window back. */

#include "test/test_core.h"

#include "config/command_catalog.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "controllers/agent_session_controller.h"
#include "controllers/rpc_client.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "models/agent_session.h"
#include "models/database.h"
#include "models/principal.h"
#include "platform/time_compat.h"
#include "rpc/server.h"
#include "services/agent_spend_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_account = "t1AgentSpendPolicyTestAccount00000000";
static const char *const k_sid_a = "0123456789abcdef0123456789abcdef";
static const char *const k_sid_missing = "dddddddddddddddddddddddddddddddd";
static const char *const k_recipient = "t1Recipient00000000000";
static const char *const k_txid =
    "\"aa11bb22cc33dd44ee55ff6600112233445566778899aabbccddeeff00112233\"";

/* ── fixture ─────────────────────────────────────────────────────────────── */

struct asp_fixture {
    struct node_db ndb;
    struct db_service dbsvc;
    struct app_runtime_context runtime;
    struct rpc_table tbl;
    char dir[256];
};

static struct asp_fixture *g_fixture;
static const char *g_sendtoaddress_result;  /* NULL -> the send RPC fails */
static int g_commit_calls;                  /* the double-debit detector */
static int g_release_calls;
static bool g_node_down;

static void seed_principal(struct node_db *ndb, const char *address)
{
    struct db_principal p;
    memset(&p, 0, sizeof(p));
    snprintf(p.address, sizeof(p.address), "%s", address);
    snprintf(p.pubkey_hex, sizeof(p.pubkey_hex), "02%064x", 0xabcdU);
    p.key_kind = PRINCIPAL_KEY_SECP256K1;
    p.role = PRINCIPAL_ROLE_OPERATOR;
    p.status = PRINCIPAL_STATUS_ACTIVE;
    p.sybil_proof_height = -1;
    (void)db_principal_save(ndb, &p);
}

/* Stands in for the loopback socket: runs the real RPC handler in-process. */
static char *asp_rpc_hook(const char *method, const char *params_json)
{
    if (strcmp(method, "sendtoaddress") == 0) {
        if (!g_sendtoaddress_result)
            return strdup("{\"code\":-6,\"message\":\"Insufficient funds\"}");
        return strdup(g_sendtoaddress_result);
    }
    if (strcmp(method, "agentsession") != 0)
        return strdup("null");
    if (g_node_down || !g_fixture)
        return NULL;

    struct json_value params;
    if (!json_read(&params, params_json, strlen(params_json))) {
        json_free(&params);
        return NULL;
    }
    /* Observe what the policy actually ASKED for, so "debited once" is
     * measured at the wire rather than inferred from the reply. */
    const char *action = json_get_str(json_at(&params, 0));
    const struct json_value *body = json_at(&params, 1);
    if (action && strcmp(action, "authorize") == 0 && body) {
        const struct json_value *c = json_get(body, "commit");
        if (c && json_get_bool(c))
            g_commit_calls++;
    }
    if (action && strcmp(action, "release") == 0)
        g_release_calls++;

    struct json_value result;
    json_init(&result);
    bool ok = rpc_table_execute(&g_fixture->tbl, "agentsession", &params,
                                &result);
    json_free(&params);
    char *out = malloc(16384);
    if (!out) {
        json_free(&result);
        return NULL;
    }
    if (!ok) {
        char msg[256] = "rpc failure";
        if (result.type == JSON_STR)
            (void)snprintf(msg, sizeof(msg), "%s", json_get_str(&result));
        (void)snprintf(out, 16384, "{\"code\":-32603,\"message\":\"%s\"}", msg);
    } else if (json_write(&result, out, 16384) == 0) {
        free(out);
        json_free(&result);
        return NULL;
    }
    json_free(&result);
    return out;
}

static int asp_open(struct asp_fixture *f, const char *tag)
{
    char dbpath[320];
    test_make_tmpdir(f->dir, sizeof(f->dir), "agent_spend_policy", tag);
    snprintf(dbpath, sizeof(dbpath), "%s/node.db", f->dir);
    memset(&f->ndb, 0, sizeof(f->ndb));
    memset(&f->dbsvc, 0, sizeof(f->dbsvc));
    memset(&f->runtime, 0, sizeof(f->runtime));
    if (!node_db_open(&f->ndb, dbpath)) {
        printf("agent_spend_policy: node_db_open... FAIL\n");
        test_rm_rf(f->dir);
        return 0;
    }
    seed_principal(&f->ndb, k_account);
    db_service_init(&f->dbsvc);
    if (!db_service_attach(&f->dbsvc, &f->ndb) ||
        !db_service_start(&f->dbsvc)) {
        printf("agent_spend_policy: db_service start... FAIL\n");
        node_db_close(&f->ndb);
        test_rm_rf(f->dir);
        return 0;
    }
    f->runtime.db_service = &f->dbsvc;
    /* app_runtime is what the NODE side of the RPC uses; the policy side never
     * touches it. Both halves live in this one process, which is exactly what
     * lets the hook stand in for the socket without faking the store. */
    app_runtime_set_current(&f->runtime);
    rpc_table_init(&f->tbl);
    register_agent_session_rpc_commands(&f->tbl);
    /* rpc_table_execute refuses every method while the server reports
     * warmup, so this fixture has to declare the server ready. */
    set_rpc_warmup_finished();
    g_fixture = f;
    g_sendtoaddress_result = NULL;
    g_commit_calls = 0;
    g_release_calls = 0;
    g_node_down = false;
    node_rpc_client_set_test_hook(asp_rpc_hook);
    return 1;
}

static void asp_close(struct asp_fixture *f)
{
    node_rpc_client_set_test_hook(NULL);
    g_fixture = NULL;
    app_runtime_set_current(NULL);
    db_service_stop(&f->dbsvc);
    node_db_close(&f->ndb);
    test_rm_rf(f->dir);
}

static void mk_session(struct db_agent_session *s, const char *sid,
                       int64_t max_per_tx_zat, int64_t max_per_window_zat)
{
    int64_t now = (int64_t)platform_time_wall_time_t();
    memset(s, 0, sizeof(*s));
    snprintf(s->session_id, sizeof(s->session_id), "%s", sid);
    snprintf(s->account, sizeof(s->account), "%s", k_account);
    s->max_per_tx_zat = max_per_tx_zat;
    s->max_per_window_zat = max_per_window_zat;
    s->window_seconds = 3600;
    s->window_start_epoch = now;
    s->spent_in_window_zat = 0;
    s->created_at = now;
    s->expires_at = 0;
    s->revoked = 0;
}

static const struct zcl_command_spec *spec_for(const char *path)
{
    const struct zcl_command_registry *reg = zcl_command_catalog();
    return reg ? zcl_command_registry_find(reg, path, NULL) : NULL;
}

/* {<recipient_key>: recipient, amount: <amount>, confirm: <confirm>} */
static void asp_input(struct json_value *in, const char *recipient_key,
                      const char *recipient, const double *amount,
                      bool confirm)
{
    json_init(in);
    json_set_object(in);
    if (recipient_key && recipient)
        (void)json_push_kv_str(in, recipient_key, recipient);
    if (amount)
        (void)json_push_kv_real(in, "amount", *amount);
    if (confirm)
        (void)json_push_kv_bool(in, "confirm", true);
}

static int64_t spent_now(struct asp_fixture *f, const char *sid)
{
    struct db_agent_session got;
    if (!agent_session_find(&f->ndb, sid, &got))
        return -1;
    return got.spent_in_window_zat;
}

/* ── 1. local-operator exemption ─────────────────────────────────────────── */

static int test_operator_exemption(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("a NULL/empty session id is the explicit local-operator exemption") {
        ASSERT(asp_open(&f, "exempt"));
        struct agent_spend_policy_decision d;
        double amt = 5.0;
        struct json_value in;
        asp_input(&in, "address", k_recipient, &amt, true);
        const struct zcl_command_spec *send =
            spec_for("core.wallet.transaction.send");
        ASSERT(send != NULL);
        agent_spend_policy_evaluate(NULL, send, &in, true, &d);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 0);
        agent_spend_policy_evaluate("", send, &in, true, &d);
        ASSERT(d.allowed);
        /* Even the leaves a bounded session may never touch stay open to the
         * un-sessioned operator: the exemption is total, and explicit. */
        agent_spend_policy_evaluate(
            NULL, spec_for("core.wallet.address.export-key"), &in, true, &d);
        ASSERT(d.allowed);
        agent_spend_policy_evaluate(
            NULL, spec_for("vault.session.create"), &in, true, &d);
        ASSERT(d.allowed);
        json_free(&in);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 2 + 3. default deny: key material, unbounded wallet verbs, grants ───── */

static int test_default_deny_surface(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("a bounded session is refused every wallet leaf the policy cannot "
        "bound, and the entire grant surface") {
        ASSERT(asp_open(&f, "deny"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000000, 100000000);   /* 1 ZCL caps */
        ASSERT(agent_session_save(&f.ndb, &s));

        /* None of these carries an `amount`, which is precisely why keying the
         * gate on the input's shape let every one of them through. */
        static const char *const denied[] = {
            "core.wallet.address.export-key",
            "core.wallet.address.import",
            "core.wallet.backup.now",
            "core.wallet.rescan",
        };
        for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
            const struct zcl_command_spec *sp = spec_for(denied[i]);
            ASSERT(sp != NULL);
            struct json_value in;
            json_init(&in);
            json_set_object(&in);
            (void)json_push_kv_str(&in, "address", k_recipient);
            (void)json_push_kv_bool(&in, "confirm", true);
            struct agent_spend_policy_decision d;
            agent_spend_policy_evaluate(k_sid_a, sp, &in, true, &d);
            json_free(&in);
            if (d.allowed)
                printf("\n  leaf %s was NOT refused\n", denied[i]);
            ASSERT(!d.allowed);
            ASSERT_STR_EQ(d.code, "POLICY_NOT_UNDERSTOOD");
        }

        static const char *const grants[] = {
            "vault.session.create",
            "vault.session.list",
            "vault.session.revoke",
        };
        for (size_t i = 0; i < sizeof(grants) / sizeof(grants[0]); i++) {
            const struct zcl_command_spec *sp = spec_for(grants[i]);
            ASSERT(sp != NULL);
            struct json_value in;
            json_init(&in);
            json_set_object(&in);
            (void)json_push_kv_bool(&in, "confirm", true);
            struct agent_spend_policy_decision d;
            agent_spend_policy_evaluate(k_sid_a, sp, &in, true, &d);
            json_free(&in);
            if (d.allowed)
                printf("\n  grant leaf %s was NOT refused\n", grants[i]);
            ASSERT(!d.allowed);
            ASSERT_STR_EQ(d.code, "POLICY_NO_GRANT_MINT");
        }

        /* A dispatch that names no command cannot be classified -> refuse. */
        struct json_value empty;
        json_init(&empty);
        json_set_object(&empty);
        struct agent_spend_policy_decision d;
        agent_spend_policy_evaluate(k_sid_a, NULL, &empty, true, &d);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_UNKNOWN_COMMAND");
        json_free(&empty);

        ASSERT_EQ(spent_now(&f, k_sid_a), 0);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3b. a read whose REACH is spend authority is refused ────────────────── */

/* `core.storage.query` is declared READ + CAP_CHAIN_READ, so the default-deny
 * branch classified it as a plain read and let a bounded session run arbitrary
 * SELECT over node.db. node.db holds material whose possession authorizes a
 * spend: `agent_sessions.session_id` is a bearer grant, so a 0.01-ZCL session
 * could read the 100-ZCL session's token and present it; `zswp_contracts.secret`
 * is an HTLC preimage. A bound the bounded party can raise for itself is not a
 * bound. Both the policy row here and the SQL-surface denylist
 * (app/controllers/src/dbquery_controller.c) must hold — this test owns the
 * policy half, lib/test/src/test_dbquery_secret_denylist.c owns the other. */
static int test_arbitrary_sql_refused(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("a bounded session may not run arbitrary SQL over node.db") {
        ASSERT(asp_open(&f, "sql"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000000, 100000000);
        ASSERT(agent_session_save(&f.ndb, &s));

        static const char *const sql_leaves[] = {
            "core.storage.query",
            "core.storage.query.offline",
        };
        for (size_t i = 0; i < sizeof(sql_leaves) / sizeof(sql_leaves[0]);
             i++) {
            const struct zcl_command_spec *sp = spec_for(sql_leaves[i]);
            ASSERT(sp != NULL);
            struct json_value in;
            json_init(&in);
            json_set_object(&in);
            (void)json_push_kv_str(&in, "sql",
                                   "SELECT session_id FROM agent_sessions");
            struct agent_spend_policy_decision d;
            agent_spend_policy_evaluate(k_sid_a, sp, &in, false, &d);
            json_free(&in);
            if (d.allowed)
                printf("\n  sql leaf %s was NOT refused\n", sql_leaves[i]);
            ASSERT(!d.allowed);
            ASSERT_STR_EQ(d.code, "POLICY_UNBOUNDABLE");
            /* The refusal names the grant that said no, redacted — never the
             * bearer token the caller presented. */
            ASSERT(strstr(d.evidence, "…") != NULL);
            ASSERT(strcmp(d.evidence, k_sid_a) != 0);
        }

        /* The un-sessioned local operator keeps the same command: this is a
         * narrowing of what a GRANT reaches, not a removal of a surface. */
        struct json_value in;
        json_init(&in);
        json_set_object(&in);
        (void)json_push_kv_str(&in, "sql", "SELECT 1");
        struct agent_spend_policy_decision d;
        agent_spend_policy_evaluate(NULL, spec_for("core.storage.query"), &in,
                                    false, &d);
        ASSERT(d.allowed);
        json_free(&in);

        ASSERT_EQ(spent_now(&f, k_sid_a), 0);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 4. understood wallet reads pass, free ───────────────────────────────── */

static int test_wallet_reads_pass(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("understood wallet reads pass for a bounded session and debit "
        "nothing") {
        ASSERT(asp_open(&f, "reads"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 1, 1);   /* caps so tight every spend fails */
        ASSERT(agent_session_save(&f.ndb, &s));
        static const char *const reads[] = {
            "core.wallet.balance", "core.wallet.utxo.list",
            "core.wallet.transaction.list", "core.wallet.shielded.balance",
            "vault.list", "vault.show",
        };
        for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); i++) {
            const struct zcl_command_spec *sp = spec_for(reads[i]);
            ASSERT(sp != NULL);
            struct json_value in;
            json_init(&in);
            json_set_object(&in);
            struct agent_spend_policy_decision d;
            agent_spend_policy_evaluate(k_sid_a, sp, &in, false, &d);
            json_free(&in);
            if (!d.allowed)
                printf("\n  read leaf %s was refused (%s)\n", reads[i], d.code);
            ASSERT(d.allowed);
            ASSERT_EQ(d.debited_zat, 0);
        }
        ASSERT_EQ(spent_now(&f, k_sid_a), 0);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 5. the caps on an understood spend ──────────────────────────────────── */

static int test_spend_caps(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("understood spends: plan enforces without debiting, commit debits, "
        "per-tx and window caps refuse, revoked/missing fail closed") {
        ASSERT(asp_open(&f, "caps"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 1000000, 600000);  /* per-tx .01, window .006 */
        ASSERT(agent_session_save(&f.ndb, &s));
        const struct zcl_command_spec *send =
            spec_for("core.wallet.transaction.send");
        ASSERT(send != NULL);
        struct agent_spend_policy_decision d;
        struct json_value in;

        /* Plan stage: caps enforced, nothing spent. */
        double big = 0.02;
        asp_input(&in, "address", k_recipient, &big, false);
        agent_spend_policy_evaluate(k_sid_a, send, &in, false, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_TX_LIMIT");

        double small = 0.005;
        asp_input(&in, "address", k_recipient, &small, false);
        agent_spend_policy_evaluate(k_sid_a, send, &in, false, &d);
        json_free(&in);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 0);
        ASSERT_EQ(spent_now(&f, k_sid_a), 0);

        /* Committing: the same spend debits. */
        asp_input(&in, "address", k_recipient, &small, true);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 500000);
        ASSERT_EQ(d.window_remaining_zat, 100000);
        ASSERT_EQ(spent_now(&f, k_sid_a), 500000);

        /* 500000 + 200000 > 600000: the window cap bites, nothing added. */
        double next = 0.002;
        asp_input(&in, "address", k_recipient, &next, true);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_WINDOW_LIMIT");
        ASSERT_EQ(spent_now(&f, k_sid_a), 500000);

        /* Revoked and missing both fail closed. */
        ASSERT(agent_session_revoke(&f.ndb, k_sid_a));
        asp_input(&in, "address", k_recipient, &small, true);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "SESSION_INVALID");
        agent_spend_policy_evaluate(k_sid_missing, send, &in, true, &d);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "SESSION_INVALID");
        json_free(&in);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 6. the allowlist, per leaf's own recipient key ──────────────────────── */

static int test_allowlist_both_keys(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("the allowlist is checked against each leaf's OWN recipient key "
        "(address for transparent, to for shielded)") {
        ASSERT(asp_open(&f, "allow"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000000, 100000000);
        snprintf(s.recipient_allowlist, sizeof(s.recipient_allowlist),
                 "%s,zs1Allowed", k_recipient);
        ASSERT(agent_session_save(&f.ndb, &s));
        struct agent_spend_policy_decision d;
        double amt = 0.001;
        struct json_value in;

        asp_input(&in, "address", k_recipient, &amt, true);
        agent_spend_policy_evaluate(k_sid_a,
            spec_for("core.wallet.transaction.send"), &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);

        asp_input(&in, "address", "t1NotListed", &amt, true);
        agent_spend_policy_evaluate(k_sid_a,
            spec_for("core.wallet.transaction.send"), &in, true, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_RECIPIENT");

        /* The shielded leaf's recipient is `to`. A gate that only looked for
         * `address` would find nothing and — depending on which way it
         * guessed — either refuse a legitimate send or wave an unlisted one
         * through. */
        asp_input(&in, "to", "zs1Allowed", &amt, true);
        agent_spend_policy_evaluate(k_sid_a,
            spec_for("core.wallet.shielded.send"), &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);

        asp_input(&in, "to", "zs1NotListed", &amt, true);
        agent_spend_policy_evaluate(k_sid_a,
            spec_for("core.wallet.shielded.send"), &in, true, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_RECIPIENT");
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 7. amount shapes ────────────────────────────────────────────────────── */

static int test_amount_shapes(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("amount as INT / REAL / decimal string parse identically; junk, "
        "negative, absurd and MISSING all fail closed") {
        ASSERT(asp_open(&f, "amount"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 200000000, 2000000000);  /* 2 / 20 ZCL */
        ASSERT(agent_session_save(&f.ndb, &s));
        const struct zcl_command_spec *send =
            spec_for("core.wallet.transaction.send");
        struct agent_spend_policy_decision d;
        struct json_value in;

        json_init(&in);
        json_set_object(&in);
        (void)json_push_kv_str(&in, "address", k_recipient);
        (void)json_push_kv_int(&in, "amount", 1);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 100000000);

        json_init(&in);
        json_set_object(&in);
        (void)json_push_kv_str(&in, "address", k_recipient);
        (void)json_push_kv_str(&in, "amount", "1.0");
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 100000000);

        double r = 1.0;
        asp_input(&in, "address", k_recipient, &r, true);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(d.allowed);
        ASSERT_EQ(d.debited_zat, 100000000);
        ASSERT_EQ(spent_now(&f, k_sid_a), 300000000);

        static const char *const junk[] = { "abc", "1.0x", "-1", "", "1e999" };
        for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
            json_init(&in);
            json_set_object(&in);
            (void)json_push_kv_str(&in, "address", k_recipient);
            (void)json_push_kv_str(&in, "amount", junk[i]);
            agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
            json_free(&in);
            if (d.allowed)
                printf("\n  amount \"%s\" was NOT refused\n", junk[i]);
            ASSERT(!d.allowed);
            /* "1e999" parses as +inf and saturates past every possible cap, so
             * it refuses as a limit rather than as a parse failure. Refused
             * either way, which is the property that matters. */
            ASSERT(strcmp(d.code, "POLICY_AMOUNT") == 0 ||
                   strcmp(d.code, "POLICY_TX_LIMIT") == 0);
        }

        /* A spend leaf dispatched with NO amount is refused, not waved
         * through: an unbounded spend is not a bounded one. */
        json_init(&in);
        json_set_object(&in);
        (void)json_push_kv_str(&in, "address", k_recipient);
        agent_spend_policy_evaluate(k_sid_a, send, &in, true, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "POLICY_AMOUNT");
        ASSERT_EQ(spent_now(&f, k_sid_a), 300000000);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 8. node down = fail closed ──────────────────────────────────────────── */

static int test_node_down_fails_closed(void)
{
    int failures = 0;
    struct asp_fixture f;
    TEST("a grant store that cannot be reached refuses the spend") {
        ASSERT(asp_open(&f, "down"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000000, 100000000);
        ASSERT(agent_session_save(&f.ndb, &s));
        g_node_down = true;
        double amt = 0.001;
        struct json_value in;
        asp_input(&in, "address", k_recipient, &amt, true);
        struct agent_spend_policy_decision d;
        agent_spend_policy_evaluate(
            k_sid_a, spec_for("core.wallet.transaction.send"), &in, true, &d);
        json_free(&in);
        ASSERT(!d.allowed);
        ASSERT_STR_EQ(d.code, "NODE_UNREACHABLE");
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 9 + 10 + 11. through the kernel, on rendered bytes ──────────────────── */

static bool exec_leaf(const char *path, const char *session_id,
                      const struct json_value *input, char *out,
                      size_t out_size, enum zcl_command_exit *code)
{
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *spec = spec_for(path);
    if (!reg || !spec)
        return false;
    struct zcl_command_context ctx = {
        .registry = reg,
        .granted_capabilities = ~(uint64_t)0,
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        .agent_session = session_id,
    };
    return zcl_command_registry_execute_json(reg, spec, &ctx, input, false,
                                             path, "normal", 0, 0, NULL, out,
                                             out_size, code) > 0;
}

static int test_kernel_hook_rendered_bytes(void)
{
    int failures = 0;
    struct asp_fixture f;
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("execute_json refuses an over-limit spend in the RENDERED bytes, "
        "with the redacted grant id, no plan body, and an authority block") {
        ASSERT(asp_open(&f, "kernel"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000, 100000);   /* 0.001 ZCL */
        ASSERT(agent_session_save(&f.ndb, &s));

        double amt = 5.0;
        struct json_value in;
        asp_input(&in, "address", k_recipient, &amt, true);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
        ASSERT(exec_leaf("core.wallet.transaction.send", k_sid_a, &in, out,
                         sizeof(out), &code));
        json_free(&in);
        ASSERT_EQ((int)code, (int)ZCL_COMMAND_EXIT_DENIED);
        ASSERT(strstr(out, "POLICY_TX_LIMIT") != NULL);
        /* The handler must not have run. */
        ASSERT(strstr(out, "plan_token") == NULL);
        ASSERT(strstr(out, "\"stage\"") == NULL);
        /* The bearer token must NEVER appear; the redacted form must. */
        ASSERT(strstr(out, k_sid_a) == NULL);
        ASSERT(strstr(out, "01234567\xe2\x80\xa6") != NULL);
        ASSERT(strstr(out, "\"policy\":\"bounded\"") != NULL);

        /* An un-sessioned dispatch renders the exemption explicitly rather
         * than by absence, and still reaches the handler's plan. */
        asp_input(&in, "address", k_recipient, &amt, false);
        ASSERT(exec_leaf("core.wallet.transaction.send", NULL, &in, out,
                         sizeof(out), &code));
        json_free(&in);
        ASSERT(strstr(out, "\"policy\":\"exempt\"") != NULL);
        ASSERT(strstr(out, "none (local operator)") != NULL);
        ASSERT(strstr(out, "POLICY_") == NULL);
        ASSERT(strstr(out, "plan_token") != NULL);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* THE double-debit regression. `vault send` is a registry leaf whose handler
 * dispatches onward to core.wallet.transaction.send IN-PROCESS. Both the
 * kernel gate and the vault's own gate used to charge the window, so a session
 * whose window cap equalled its per-tx cap could never complete one send: the
 * first debit consumed the whole window and the second check refused what the
 * caller was entitled to. */
static int test_vault_send_debits_once(void)
{
    int failures = 0;
    struct asp_fixture f;
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("vault send through execute_json debits the window exactly once") {
        ASSERT(asp_open(&f, "once"));
        struct db_agent_session s;
        /* per-tx == per-window == 1 ZCL: a double debit cannot hide here. */
        mk_session(&s, k_sid_a, 100000000, 100000000);
        ASSERT(agent_session_save(&f.ndb, &s));
        g_sendtoaddress_result = k_txid;

        double amt = 1.0;
        struct json_value in;
        asp_input(&in, "address", k_recipient, &amt, true);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf("vault.send", k_sid_a, &in, out, sizeof(out), &code));
        json_free(&in);
        if (code != ZCL_COMMAND_EXIT_OK)
            printf("\n  vault.send reply: %s\n", out);
        ASSERT_EQ((int)code, (int)ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "POLICY_") == NULL);
        ASSERT_EQ((int64_t)g_commit_calls, (int64_t)1);
        ASSERT_EQ(spent_now(&f, k_sid_a), 100000000);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

/* A debit pays for a spend. When the handler then fails, the window gets it
 * back — otherwise a node hiccup permanently costs an agent its budget with no
 * txid to show for it. */
static int test_failed_handler_releases_debit(void)
{
    int failures = 0;
    struct asp_fixture f;
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("a handler that fails after an allowed debit gets the window "
        "credited back, and the budget is usable again") {
        ASSERT(asp_open(&f, "release"));
        struct db_agent_session s;
        mk_session(&s, k_sid_a, 100000000, 100000000);
        ASSERT(agent_session_save(&f.ndb, &s));
        g_sendtoaddress_result = NULL;   /* the send RPC fails */

        double amt = 1.0;
        struct json_value in;
        asp_input(&in, "address", k_recipient, &amt, true);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
        ASSERT(exec_leaf("core.wallet.transaction.send", k_sid_a, &in, out,
                         sizeof(out), &code));
        json_free(&in);
        ASSERT(code != ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ((int64_t)g_release_calls, (int64_t)1);
        ASSERT_EQ(spent_now(&f, k_sid_a), 0);

        g_sendtoaddress_result = k_txid;
        asp_input(&in, "address", k_recipient, &amt, true);
        code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf("core.wallet.transaction.send", k_sid_a, &in, out,
                         sizeof(out), &code));
        json_free(&in);
        ASSERT_EQ((int)code, (int)ZCL_COMMAND_EXIT_OK);
        ASSERT_EQ(spent_now(&f, k_sid_a), 100000000);
        asp_close(&f);
        PASS();
    } _test_next:;
    return failures;
}

int test_agent_spend_policy(void);
int test_agent_spend_policy(void)
{
    printf("\n=== agent spend policy tests ===\n");
    int failures = 0;
    failures += test_operator_exemption();
    failures += test_default_deny_surface();
    failures += test_arbitrary_sql_refused();
    failures += test_wallet_reads_pass();
    failures += test_spend_caps();
    failures += test_allowlist_both_keys();
    failures += test_amount_shapes();
    failures += test_node_down_fails_closed();
    failures += test_kernel_hook_rendered_bytes();
    failures += test_vault_send_debits_once();
    failures += test_failed_handler_releases_debit();
    printf("agent_spend_policy: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}
