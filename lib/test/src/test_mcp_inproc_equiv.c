/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Equivalence test for the in-process MCP transport.
 *
 * The MCP server has two RPC backends behind a single mcp_node_rpc()
 * entry point (tools/mcp/rpc_client.c):
 *
 *   - mcp_node_rpc_http   — the default: socket + JSON-RPC POST to the node.
 *   - mcp_node_rpc_inproc — the -mcp-inprocess path: rpc_table_execute()
 *                           on the live table in the SAME process.
 *
 * Both MUST hand every controller the IDENTICAL malloc'd JSON shape — the
 * bare "result" value on success, the "error" object on failure — so that
 * controllers and mcp_return_rpc_body are transport-agnostic. This test
 * registers a tiny rpc_table, wires it the same way the HTTP server does
 * (rpc_http_start sets the active table), and asserts the in-process
 * projection equals the exact JSON the wire path produces for success,
 * handler-error, and method-not-found.
 *
 * The expected string is computed by running the WHOLE wire sequence
 * independently (build envelope -> serialize -> re-parse -> extract), so
 * the test proves the projection is correct, not merely self-consistent,
 * and captures duplicate-key / ordering quirks identically. */

#include "test/test_helpers.h"
#include "mcp/rpc_client.h"
#include "rpc/server.h"
#include "rpc/httpserver.h"
#include "rpc/protocol.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

/* ── Test handlers registered into the table ───────────────────────── */

/* Success: returns an object {"n":42}. The in-process path must hand back
 * exactly that object (the "result" value), no envelope. */
static bool h_success(const struct json_value *params, bool help,
                      struct json_value *result)
{
    (void)params; (void)help;
    json_set_object(result);
    json_push_kv_int(result, "n", 42);
    return true;
}

/* Failure: returns false with a structured error in *result, exactly the
 * discipline rpc_table_execute documents. */
static bool h_fail(const struct json_value *params, bool help,
                   struct json_value *result)
{
    (void)params; (void)help;
    json_rpc_error_full(result, RPC_MISC_ERROR, "boom", NULL);
    return false;
}

/* Reproduce the EXACT projection the out-of-process HTTP path performs, so
 * the test compares in-process against the wire path's real bytes (not a
 * re-call of the in-process helper). The HTTP path: rpc_table_execute fills
 * *result, the server builds the {result,error,id} envelope, serializes it,
 * ships it over the socket, then mcp_node_rpc_http re-parses the wire bytes
 * and json_write's either the "error" object (if non-null) or "result".
 * Running that whole sequence here — including the serialize+re-parse round
 * trip — captures duplicate-key and ordering quirks identically. */
static char *http_projection(const struct rpc_table *t,
                             const char *method, const char *params_json)
{
    struct json_value params;
    json_init(&params);
    if (params_json && params_json[0])
        json_read(&params, params_json, strlen(params_json));
    else
        json_set_array(&params);

    struct json_value result;
    json_init(&result);
    bool ok = rpc_table_execute(t, method, &params, &result);

    struct json_value id;
    json_init(&id);
    json_set_int(&id, 1);

    struct json_value envelope;
    json_init(&envelope);
    rpc_http_test_build_response_envelope(ok, method, &result, &id, &envelope);

    char *wire = NULL;
    size_t wire_len = 0;
    char *out = NULL;
    if (rpc_http_test_serialize_response(&envelope, &wire, &wire_len)) {
        struct json_value reparsed;
        json_init(&reparsed);
        if (json_read(&reparsed, wire, wire_len)) {
            const struct json_value *err = json_get(&reparsed, "error");
            const struct json_value *res = json_get(&reparsed, "result");
            const struct json_value *pick =
                (err && err->type != JSON_NULL) ? err : res;
            if (pick) {
                size_t need = json_write(pick, NULL, 0) + 1;
                out = malloc(need);
                if (out) json_write(pick, out, need);
            }
        }
        json_free(&reparsed);
        free(wire);
    }
    json_free(&envelope);
    json_free(&id);
    json_free(&result);
    json_free(&params);
    return out;
}

/* ── One TEST block per static function (house style — each needs its own
 *     _test_next: label) ────────────────────────────────────────────── */

static int t_active_table(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: active table is the one we started") {
        ASSERT(rpc_http_active_table() == tbl);
        PASS();
    } _test_next:;
    return failures;
}

static int t_success(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: success matches the wire projection") {
        char *got = mcp_node_rpc_inproc("ok_method", "[]");
        char *exp = http_projection(tbl, "ok_method", "[]");
        ASSERT(got != NULL);
        ASSERT(exp != NULL);
        ASSERT_STR_EQ(got, exp);
        free(got); free(exp);
        PASS();
    } _test_next:;
    return failures;
}

static int t_handler_error(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: handler error matches the wire projection") {
        char *got = mcp_node_rpc_inproc("fail_method", "[]");
        char *exp = http_projection(tbl, "fail_method", "[]");
        ASSERT(got != NULL);
        ASSERT(exp != NULL);
        ASSERT_STR_EQ(got, exp);
        free(got); free(exp);
        PASS();
    } _test_next:;
    return failures;
}

static int t_not_found(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: unknown method matches the wire projection") {
        char *got = mcp_node_rpc_inproc("no_such_method", "[]");
        char *exp = http_projection(tbl, "no_such_method", "[]");
        ASSERT(got != NULL);
        ASSERT(exp != NULL);
        ASSERT_STR_EQ(got, exp);
        free(got); free(exp);
        PASS();
    } _test_next:;
    return failures;
}

static int t_null_params(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: NULL params behaves like [] and matches wire") {
        char *got = mcp_node_rpc_inproc("ok_method", NULL);
        char *exp = http_projection(tbl, "ok_method", "[]");
        ASSERT(got != NULL);
        ASSERT(exp != NULL);
        ASSERT_STR_EQ(got, exp);
        free(got); free(exp);
        PASS();
    } _test_next:;
    return failures;
}

static int t_selector_routes(const struct rpc_table *tbl)
{
    int failures = 0;
    TEST("inproc: selector routes mcp_node_rpc to the in-process backend") {
        mcp_rpc_client_use_inprocess();
        char *got = mcp_node_rpc("ok_method", "[]");
        char *exp = http_projection(tbl, "ok_method", "[]");
        ASSERT(got != NULL);
        ASSERT(exp != NULL);
        ASSERT_STR_EQ(got, exp);
        free(got); free(exp);
        PASS();
    } _test_next:;
    return failures;
}

/* Must run while the server is still in its initial warmup state (before
 * set_rpc_warmup_finished()); there is no public way to re-enter warmup. */
static int t_warmup(void)
{
    int failures = 0;
    TEST("inproc: warmup surfaces the in-warmup error") {
        char *got = mcp_node_rpc_inproc("ok_method", "[]");
        ASSERT(got != NULL);
        ASSERT(strstr(got, "\"code\":") != NULL);
        ASSERT(strstr(got, "\"method\":\"ok_method\"") != NULL);
        free(got);
        PASS();
    } _test_next:;
    return failures;
}

static int t_no_table(void)
{
    int failures = 0;
    /* With no active table (server stopped), the in-process path must still
     * hand back a proper error body, never NULL. */
    TEST("inproc: no live table yields an error body, not NULL") {
        char *got = mcp_node_rpc_inproc("ok_method", "[]");
        ASSERT(got != NULL);
        ASSERT(strstr(got, "\"error\"") != NULL);
        free(got);
        PASS();
    } _test_next:;
    return failures;
}

int test_mcp_inproc_equiv(void)
{
    int failures = 0;

    static struct rpc_table tbl;
    rpc_table_init(&tbl);
    struct rpc_command c_ok   = { "test", "ok_method",   h_success, true };
    struct rpc_command c_fail = { "test", "fail_method", h_fail,    true };
    rpc_table_append(&tbl, &c_ok);
    rpc_table_append(&tbl, &c_fail);

    char dir[] = "/tmp/zcl_mcp_inproc_XXXXXX";
    if (!mkdtemp(dir)) {
        printf("test_mcp_inproc_equiv: mkdtemp failed\n");
        return 1;
    }

    bool started = rpc_http_start(&tbl, 0, NULL, NULL, dir);
    if (!started) {
        printf("(SKIP: rpc_http_start failed — cannot wire active table)\n");
        return 0;
    }

    /* The server starts in warmup; exercise the in-warmup projection FIRST
     * (there is no public way to re-enter warmup once finished). */
    failures += t_active_table(&tbl);
    failures += t_warmup();

    set_rpc_warmup_finished();

    failures += t_success(&tbl);
    failures += t_handler_error(&tbl);
    failures += t_not_found(&tbl);
    failures += t_null_params(&tbl);
    failures += t_selector_routes(&tbl);

    rpc_http_stop();

    /* After stop, the active table is NULL — the no-table guard runs here. */
    failures += t_no_table();

    return failures;
}
