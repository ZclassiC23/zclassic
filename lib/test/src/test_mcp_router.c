/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Tests for the MCP router: parameter validation, dispatch, error
 * envelopes, and schema JSON output.
 *
 * These tests register fake routes with stub handlers, so they run
 * without a live node. */

#include "test/test_helpers.h"
#include "mcp/router.h"
#include "event/event.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "util/safe_alloc.h"

/* ── Stub handlers ──────────────────────────────────────────── */

static int g_echo_calls = 0;

static int h_echo(const struct mcp_request *req, struct mcp_response *res)
{
    g_echo_calls++;
    char *buf = zcl_malloc(2048, "mcp_echo_buf");
    if (!buf) return -1;
    snprintf(buf, 2048, "{\"tool\":\"%s\",\"args_present\":%s}",
             req->tool, req->args ? "true" : "false");
    res->body = buf;
    return 0;
}

static int h_fail(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    res->error = MCP_ERR_HANDLER_FAILED;
    snprintf(res->error_message, sizeof(res->error_message),
             "simulated handler failure");
    snprintf(res->error_param, sizeof(res->error_param), "simulated");
    return -1;
}

static int h_null(const struct mcp_request *req, struct mcp_response *res)
{
    (void)req;
    res->body = NULL;
    return 0;
}

/* ── Route fixtures ─────────────────────────────────────────── */

static const struct mcp_param_spec p_required_str[] = {
    { "name", MCP_PARAM_STR, true, "A name",
      0, 0, 1, 32, NULL, NULL },
};

static const struct mcp_param_spec p_int_range[] = {
    { "count", MCP_PARAM_INT, true, "An int in [1,10]",
      1, 10, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_enum_str[] = {
    { "action", MCP_PARAM_STR, true, "Must be add/remove/onetry",
      0, 0, 0, 0, "add,remove,onetry", NULL },
};

static const struct mcp_param_spec p_mixed[] = {
    { "name",   MCP_PARAM_STR,  true,  "name",        0, 0, 1, 32, NULL, NULL },
    { "count",  MCP_PARAM_INT,  false, "count",       0, 100, 0, 0, NULL, "10" },
    { "active", MCP_PARAM_BOOL, false, "active flag", 0, 0, 0, 0, NULL, "false" },
    { "ratio",  MCP_PARAM_REAL, false, "ratio",       0, 0, 0, 0, NULL, NULL },
};

static const struct mcp_param_spec p_strlen[] = {
    { "tag", MCP_PARAM_STR, true, "tag", 0, 0, 3, 8, NULL, NULL },
};

static const struct mcp_tool_route r_echo = {
    "t.echo", "test", "Echo handler", NULL, 0, h_echo, 0, NULL
};
static const struct mcp_tool_route r_required = {
    "t.required", "test", "Requires name", p_required_str,
    sizeof(p_required_str) / sizeof(p_required_str[0]), h_echo, 0, NULL
};
static const struct mcp_tool_route r_int = {
    "t.int", "test", "Int in range", p_int_range,
    sizeof(p_int_range) / sizeof(p_int_range[0]), h_echo, 0, NULL
};
static const struct mcp_tool_route r_enum = {
    "t.enum", "test", "Enum", p_enum_str,
    sizeof(p_enum_str) / sizeof(p_enum_str[0]), h_echo, 0, NULL
};
static const struct mcp_tool_route r_mixed = {
    "t.mixed", "test", "Mixed params", p_mixed,
    sizeof(p_mixed) / sizeof(p_mixed[0]), h_echo, 0, NULL
};
static const struct mcp_tool_route r_strlen = {
    "t.strlen", "test", "Strlen 3-8", p_strlen,
    sizeof(p_strlen) / sizeof(p_strlen[0]), h_echo, 0, NULL
};
static const struct mcp_tool_route r_fail = {
    "t.fail", "test", "Always fails", NULL, 0, h_fail, 0, NULL
};
static const struct mcp_tool_route r_null = {
    "t.null", "test", "Returns null", NULL, 0, h_null, 0, NULL
};

/* ── Helpers ────────────────────────────────────────────────── */

static void setup_routes(void)
{
    mcp_router_reset();
    mcp_router_register(&r_echo);
    mcp_router_register(&r_required);
    mcp_router_register(&r_int);
    mcp_router_register(&r_enum);
    mcp_router_register(&r_mixed);
    mcp_router_register(&r_strlen);
    mcp_router_register(&r_fail);
    mcp_router_register(&r_null);
}

static bool parse_args(struct json_value *v, const char *s)
{
    return json_read(v, s, strlen(s));
}

static bool contains(const char *buf, const char *needle)
{
    return strstr(buf, needle) != NULL;
}

/* ── Tests ──────────────────────────────────────────────────── */

static int test_router_register_and_count(void)
{
    int failures = 0;
    TEST("router registers and counts routes") {
        ASSERT(mcp_router_count() == 8);
        ASSERT(mcp_router_find("t.echo") != NULL);
        ASSERT(mcp_router_find("t.required") != NULL);
        ASSERT(mcp_router_find("t.nonexistent") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_duplicate_rejected(void)
{
    int failures = 0;
    TEST("duplicate route registration is rejected") {
        bool ok = mcp_router_register(&r_echo);
        ASSERT(!ok);
        ASSERT(mcp_router_count() == 8);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_unknown_tool(void)
{
    int failures = 0;
    TEST("unknown tool returns UNKNOWN_TOOL envelope") {
        char *r = mcp_router_dispatch("t.does_not_exist", NULL);
        ASSERT(r != NULL);
        ASSERT(contains(r, "\"error\""));
        ASSERT(contains(r, "UNKNOWN_TOOL"));
        ASSERT(contains(r, "\"tool\":\"t.does_not_exist\""));
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_missing_required(void)
{
    int failures = 0;
    TEST("missing required parameter rejected") {
        struct json_value v;
        parse_args(&v, "{}");
        char *r = mcp_router_dispatch("t.required", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "MISSING_PARAM"));
        ASSERT(contains(r, "\"param\":\"name\""));
        ASSERT(contains(r, "\"tool\":\"t.required\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_null_args(void)
{
    int failures = 0;
    TEST("null args treated as missing required param") {
        char *r = mcp_router_dispatch("t.required", NULL);
        ASSERT(r != NULL);
        ASSERT(contains(r, "MISSING_PARAM"));
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_wrong_type(void)
{
    int failures = 0;
    TEST("wrong-type param rejected") {
        struct json_value v;
        parse_args(&v, "{\"name\": 42}");
        char *r = mcp_router_dispatch("t.required", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "INVALID_TYPE"));
        ASSERT(contains(r, "\"param\":\"name\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_int_below(void)
{
    int failures = 0;
    TEST("int below range rejected") {
        struct json_value v;
        parse_args(&v, "{\"count\": 0}");
        char *r = mcp_router_dispatch("t.int", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "OUT_OF_RANGE"));
        ASSERT(contains(r, "\"param\":\"count\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_int_above(void)
{
    int failures = 0;
    TEST("int above range rejected") {
        struct json_value v;
        parse_args(&v, "{\"count\": 11}");
        char *r = mcp_router_dispatch("t.int", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "OUT_OF_RANGE"));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_int_inside(void)
{
    int failures = 0;
    TEST("int inside range accepted") {
        struct json_value v;
        parse_args(&v, "{\"count\": 5}");
        char *r = mcp_router_dispatch("t.int", &v);
        ASSERT(r != NULL);
        ASSERT(!contains(r, "\"error\""));
        ASSERT(contains(r, "\"tool\":\"t.int\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_str_too_short(void)
{
    int failures = 0;
    TEST("string shorter than min_len rejected") {
        struct json_value v;
        parse_args(&v, "{\"tag\": \"ab\"}");
        char *r = mcp_router_dispatch("t.strlen", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "STRING_TOO_SHORT"));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_str_too_long(void)
{
    int failures = 0;
    TEST("string longer than max_len rejected") {
        struct json_value v;
        parse_args(&v, "{\"tag\": \"abcdefghij\"}");
        char *r = mcp_router_dispatch("t.strlen", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "STRING_TOO_LONG"));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_enum_mismatch(void)
{
    int failures = 0;
    TEST("enum mismatch rejected") {
        struct json_value v;
        parse_args(&v, "{\"action\": \"bogus\"}");
        char *r = mcp_router_dispatch("t.enum", &v);
        ASSERT(r != NULL);
        ASSERT(contains(r, "ENUM_MISMATCH"));
        ASSERT(contains(r, "\"param\":\"action\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_enum_match(void)
{
    int failures = 0;
    TEST("enum match accepted") {
        struct json_value v;
        parse_args(&v, "{\"action\": \"onetry\"}");
        char *r = mcp_router_dispatch("t.enum", &v);
        ASSERT(r != NULL);
        ASSERT(!contains(r, "\"error\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_optional_omitted(void)
{
    int failures = 0;
    TEST("optional param omitted is OK") {
        struct json_value v;
        parse_args(&v, "{\"name\": \"alice\"}");
        char *r = mcp_router_dispatch("t.mixed", &v);
        ASSERT(r != NULL);
        ASSERT(!contains(r, "\"error\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_handler_failure(void)
{
    int failures = 0;
    TEST("handler failure produces HANDLER_FAILED envelope") {
        char *r = mcp_router_dispatch("t.fail", NULL);
        ASSERT(r != NULL);
        ASSERT(contains(r, "HANDLER_FAILED"));
        ASSERT(contains(r, "simulated handler failure"));
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_handler_null_body(void)
{
    int failures = 0;
    TEST("handler with null body produces error envelope") {
        char *r = mcp_router_dispatch("t.null", NULL);
        ASSERT(r != NULL);
        ASSERT(contains(r, "\"error\""));
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_handler_counter(void)
{
    int failures = 0;
    TEST("successful dispatch invokes the handler") {
        int before = g_echo_calls;
        char *r = mcp_router_dispatch("t.echo", NULL);
        ASSERT(r != NULL);
        ASSERT(g_echo_calls == before + 1);
        free(r);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_schema_required(void)
{
    int failures = 0;
    TEST("input schema JSON exposes type, description, required") {
        char buf[2048];
        size_t n = mcp_router_input_schema_json(&r_required, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "\"type\":\"object\""));
        ASSERT(contains(buf, "\"name\":{\"type\":\"string\""));
        ASSERT(contains(buf, "\"required\":[\"name\"]"));
        ASSERT(contains(buf, "\"minLength\":1"));
        ASSERT(contains(buf, "\"maxLength\":32"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_schema_int(void)
{
    int failures = 0;
    TEST("input schema JSON includes integer min/max") {
        char buf[2048];
        size_t n = mcp_router_input_schema_json(&r_int, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "\"minimum\":1"));
        ASSERT(contains(buf, "\"maximum\":10"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_schema_enum(void)
{
    int failures = 0;
    TEST("input schema JSON emits enum array") {
        char buf[2048];
        size_t n = mcp_router_input_schema_json(&r_enum, buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(contains(buf, "\"enum\":[\"add\",\"remove\",\"onetry\"]"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_tools_list(void)
{
    int failures = 0;
    TEST("tools/list JSON array contains all registered tools") {
        char buf[16384];
        size_t n = mcp_router_tools_list_json(buf, sizeof(buf));
        ASSERT(n > 0);
        ASSERT(buf[0] == '[');
        ASSERT(buf[n-1] == ']');
        ASSERT(contains(buf, "\"name\":\"t.echo\""));
        ASSERT(contains(buf, "\"name\":\"t.enum\""));
        ASSERT(contains(buf, "\"domain\":\"test\""));
        ASSERT(contains(buf, "\"inputSchema\":"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_envelope_escape(void)
{
    int failures = 0;
    TEST("error envelope JSON escapes quotes in message") {
        char buf[1024];
        size_t n = mcp_router_error_envelope(
            buf, sizeof(buf), MCP_ERR_INTERNAL, "t.escaped", NULL,
            "message with \"quotes\" and \\backslash");
        ASSERT(n > 0);
        ASSERT(contains(buf, "\\\"quotes\\\""));
        ASSERT(contains(buf, "\\\\backslash"));
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_error_names(void)
{
    int failures = 0;
    TEST("error code names round-trip") {
        ASSERT_STR_EQ(mcp_error_code_name(MCP_OK), "OK");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_UNKNOWN_TOOL), "UNKNOWN_TOOL");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_MISSING_PARAM), "MISSING_PARAM");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_INVALID_TYPE), "INVALID_TYPE");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_OUT_OF_RANGE), "OUT_OF_RANGE");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_ENUM_MISMATCH), "ENUM_MISMATCH");
        ASSERT_STR_EQ(mcp_error_code_name(MCP_ERR_HANDLER_FAILED), "HANDLER_FAILED");
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_validate_direct(void)
{
    int failures = 0;
    TEST("mcp_router_validate reports param name on failure") {
        char param[64] = {0}, msg[256] = {0};
        struct json_value v;
        parse_args(&v, "{}");
        enum mcp_error_code c = mcp_router_validate(&r_required, &v,
                                                    param, sizeof(param),
                                                    msg, sizeof(msg));
        ASSERT(c == MCP_ERR_MISSING_PARAM);
        ASSERT_STR_EQ(param, "name");
        ASSERT(msg[0] != 0);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_at_order(void)
{
    int failures = 0;
    TEST("mcp_router_at returns routes in registration order") {
        const struct mcp_tool_route *r0 = mcp_router_at(0);
        ASSERT(r0 != NULL);
        ASSERT_STR_EQ(r0->name, "t.echo");
        const struct mcp_tool_route *r1 = mcp_router_at(1);
        ASSERT(r1 != NULL);
        ASSERT_STR_EQ(r1->name, "t.required");
        ASSERT(mcp_router_at(9999) == NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_mixed_ok(void)
{
    int failures = 0;
    TEST("mixed params with optional count accepts valid values") {
        struct json_value v;
        parse_args(&v, "{\"name\":\"x\",\"count\":50}");
        char *r = mcp_router_dispatch("t.mixed", &v);
        ASSERT(r != NULL);
        ASSERT(!contains(r, "\"error\""));
        free(r);
        json_free(&v);
        PASS();
    } _test_next:;
    return failures;
}

static int test_router_reset_clears(void)
{
    int failures = 0;
    TEST("mcp_router_reset clears the registry") {
        mcp_router_reset();
        ASSERT(mcp_router_count() == 0);
        ASSERT(mcp_router_find("t.echo") == NULL);
        setup_routes();
        ASSERT(mcp_router_count() == 8);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ────────────────────────────────────────────── */

int test_mcp_router(void);

int test_mcp_router(void)
{
    int failures = 0;
    event_log_init();
    setup_routes();

    failures += test_router_register_and_count();
    failures += test_router_duplicate_rejected();
    failures += test_router_unknown_tool();
    failures += test_router_missing_required();
    failures += test_router_null_args();
    failures += test_router_wrong_type();
    failures += test_router_int_below();
    failures += test_router_int_above();
    failures += test_router_int_inside();
    failures += test_router_str_too_short();
    failures += test_router_str_too_long();
    failures += test_router_enum_mismatch();
    failures += test_router_enum_match();
    failures += test_router_optional_omitted();
    failures += test_router_handler_failure();
    failures += test_router_handler_null_body();
    failures += test_router_handler_counter();
    failures += test_router_schema_required();
    failures += test_router_schema_int();
    failures += test_router_schema_enum();
    failures += test_router_tools_list();
    failures += test_router_envelope_escape();
    failures += test_router_error_names();
    failures += test_router_validate_direct();
    failures += test_router_at_order();
    failures += test_router_mixed_ok();
    failures += test_router_reset_clears();

    return failures;
}
