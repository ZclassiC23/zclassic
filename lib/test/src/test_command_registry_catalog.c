/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Golden contract tests for the native command registry catalog
 * (docs/NATIVE_COMMAND_INTERFACE.md §20). Proves the composition-root catalog
 * is well-formed, shallow, budgeted, fail-closed for planned leaves, and that
 * every READY leaf has a live binding — without contacting a node.
 */

#include "test/test_helpers.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "json/json.h"

#include <string.h>

static const struct zcl_command_spec *find_spec(
    const struct zcl_command_registry *reg, const char *path)
{
    for (size_t i = 0; i < reg->count; i++)
        if (strcmp(reg->commands[i].path, path) == 0)
            return &reg->commands[i];
    return NULL;
}

static bool exec_leaf(const struct zcl_command_registry *reg,
                      const struct zcl_command_spec *spec,
                      char *out, size_t out_size,
                      enum zcl_command_exit *exit_code)
{
    struct zcl_command_context ctx = {
        .registry = reg,
        .granted_capabilities = ~(uint64_t)0,
    };
    struct json_value input;
    json_init(&input);
    json_set_object(&input);
    size_t n = zcl_command_registry_execute_json(reg, spec, &ctx, &input,
                                                 false, spec->path, "normal", 0,
                                                 0, NULL,
                                                 out, out_size, exit_code);
    json_free(&input);
    return n > 0;
}

static int test_catalog_wellformed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("catalog validates and is non-trivial") {
        char why[128] = { 0 };
        ASSERT(reg != NULL);
        ASSERT(reg->count > 40);
        ASSERT(zcl_command_registry_validate(reg, why, sizeof(why)));
        PASS();
    } _test_next:;
    return failures;
}

static int test_six_roots(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("root exposes exactly six choices") {
        size_t roots = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const char *p = reg->commands[i].parent;
            if (!p || !p[0])
                roots++;
        }
        ASSERT_EQ(roots, (size_t)6);
        ASSERT(find_spec(reg, "status") != NULL);
        ASSERT(find_spec(reg, "core") != NULL);
        ASSERT(find_spec(reg, "app") != NULL);
        ASSERT(find_spec(reg, "dev") != NULL);
        ASSERT(find_spec(reg, "ops") != NULL);
        ASSERT(find_spec(reg, "discover") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_root_menu_budget(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("root menu is within its byte budget") {
        size_t n = zcl_command_registry_menu_json(reg, "root", out,
                                                  sizeof(out));
        ASSERT(n > 0);
        ASSERT(n <= ZCL_COMMAND_ROOT_BUDGET);
        PASS();
    } _test_next:;
    return failures;
}

static int test_branch_menus_shallow(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("branch menus stay in budget and list only immediate children") {
        const char *branches[] = { "core", "core.chain", "core.wallet",
                                   "ops", "discover" };
        for (size_t b = 0; b < sizeof(branches) / sizeof(branches[0]); b++) {
            size_t n = zcl_command_registry_menu_json(reg, branches[b], out,
                                                      sizeof(out));
            ASSERT(n > 0);
            ASSERT(n <= ZCL_COMMAND_BRANCH_BUDGET);
            struct json_value doc;
            ASSERT(json_read(&doc, out, n) && doc.type == JSON_OBJ);
            const struct json_value *children = json_get(&doc, "children");
            ASSERT(children && children->type == JSON_ARR);
            for (size_t i = 0; i < children->num_children; i++) {
                const char *cpath =
                    json_get_str(json_get(&children->children[i], "path"));
                const struct zcl_command_spec *cs = find_spec(reg, cpath);
                ASSERT(cs != NULL);
                ASSERT_STR_EQ(cs->parent, branches[b]);
            }
            json_free(&doc);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_search_bounded(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("search returns at most five ranked matches") {
        const char *queries[] = { "block", "wallet", "sync", "peer", "a" };
        for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
            size_t n = zcl_command_registry_search_json(reg, queries[q], out,
                                                        sizeof(out));
            ASSERT(n > 0);
            struct json_value doc;
            ASSERT(json_read(&doc, out, n) && doc.type == JSON_OBJ);
            const struct json_value *matches = json_get(&doc, "matches");
            ASSERT(matches && matches->type == JSON_ARR);
            ASSERT(matches->num_children <= ZCL_COMMAND_SEARCH_LIMIT);
            json_free(&doc);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_ready_leaves_bound(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every READY leaf has a non-NULL handler and bridge binding") {
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->availability != ZCL_COMMAND_READY)
                continue;
            ASSERT(s->handler != NULL);
            if (s->handler == zcl_native_bridge_command)
                ASSERT(zcl_native_bridge_tool_for_path(s->path) != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_bridge_bindings_reverse(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every sampled bridge binding names a READY core/ops leaf") {
        const char *sample[] = {
            "core.status", "core.chain.tip", "ops.health", "ops.metrics",
            "core.storage.query", "core.chain.block.get",
        };
        for (size_t i = 0; i < sizeof(sample) / sizeof(sample[0]); i++) {
            const char *tool = zcl_native_bridge_tool_for_path(sample[i]);
            ASSERT(tool != NULL && tool[0] != 0);
            const struct zcl_command_spec *s = find_spec(reg, sample[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT(s->handler == zcl_native_bridge_command);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_planned_fail_closed(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("every planned leaf blocks with exit 3 and no handler") {
        int checked = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *s = &reg->commands[i];
            if (s->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;
            if (s->availability != ZCL_COMMAND_PLANNED)
                continue;
            ASSERT(s->handler == NULL);
            ASSERT(s->availability_reason && s->availability_reason[0]);
            enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_BLOCKED);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "COMMAND_PLANNED") != NULL);
            checked++;
        }
        ASSERT(checked > 5);
        PASS();
    } _test_next:;
    return failures;
}

static int test_envelope_vectors(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("a local discovery leaf returns a passing common envelope") {
        const struct zcl_command_spec *s = find_spec(reg, "discover.describe");
        ASSERT(s != NULL);
        struct zcl_command_context ctx = {
            .registry = reg, .granted_capabilities = ~(uint64_t)0,
        };
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "path", "core.status");
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        size_t n = zcl_command_registry_execute_json(
            reg, s, &ctx, &input, false, "discover.describe", "normal", 0, 0,
            NULL, out, sizeof(out), &code);
        json_free(&input);
        ASSERT(n > 0);
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"schema\":\"zcl.result.v1\"") != NULL);
        ASSERT(strstr(out, "\"ok\":true") != NULL);
        ASSERT(strstr(out, "core.status") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_typo_stays_branch(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("a typo under a canonical branch resolves to the branch, not a leaf") {
        /* `core chain bogus`: longest path is the core.chain BRANCH with the
         * unknown word left over. The adapter turns this into the structured
         * unknown-command error; the registry never invents a leaf for it, so
         * it can never fall through to an arbitrary RPC method. */
        const char *words[] = { "core", "chain", "bogus" };
        size_t consumed = 0;
        bool alias = false;
        char invoked[ZCL_COMMAND_MAX_PATH];
        const struct zcl_command_spec *s = zcl_command_registry_resolve_words(
            reg, words, 3, &consumed, &alias, invoked, sizeof(invoked));
        ASSERT(s != NULL);
        ASSERT_STR_EQ(s->path, "core.chain");
        ASSERT_EQ(s->mode, ZCL_COMMAND_MODE_BRANCH);
        ASSERT_EQ(consumed, (size_t)2);
        ASSERT(find_spec(reg, "core.chain.bogus") == NULL);
        ASSERT(zcl_command_registry_find(reg, "core.chain.bogus", NULL) ==
               NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_dev_branch_leaves(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("dev branch carries the expected ready/planned leaf availability") {
        const struct zcl_command_spec *dev = find_spec(reg, "dev");
        ASSERT(dev != NULL);
        ASSERT_EQ(dev->mode, ZCL_COMMAND_MODE_BRANCH);

        const char *ready[] = {
            "dev.status", "dev.core.boundary", "dev.app.describe",
            "dev.app.plan", "dev.app.simulate", "dev.change.plan",
            "dev.app.list", "dev.test.plan",
        };
        for (size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, ready[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT(s->handler != NULL);
        }
        /* Unfinished dev operations are explicitly planned + handlerless, so
         * discovery can never advertise a dev command that cannot dispatch. */
        const char *planned[] = {
            "dev.core.proof", "dev.app.inspect", "dev.test.replay",
            "dev.generation.rollback", "dev.loop.events",
        };
        for (size_t i = 0; i < sizeof(planned) / sizeof(planned[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, planned[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_PLANNED);
            ASSERT(s->handler == NULL);
        }
        /* Dev executors are real handlers only in ZCL_DEV_BUILD.  This test
         * binary is a release-shaped catalog, so those leaves must remain
         * explicit COMPAT entries rather than falsely READY. */
        const char *compat[] = {
            "dev.change.apply", "dev.loop.ensure", "dev.loop.status",
            "dev.loop.wait", "dev.loop.stop", "dev.test.run",
            "dev.test.sim", "dev.generation.current",
            "dev.generation.history", "dev.diagnose.latest",
            "dev.vcs.revert", "dev.vcs.seal.grant",
        };
        for (size_t i = 0; i < sizeof(compat) / sizeof(compat[0]); i++) {
            const struct zcl_command_spec *s = find_spec(reg, compat[i]);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_COMPAT);
            ASSERT(s->handler == NULL);
            ASSERT(s->compat_target != NULL && s->compat_target[0]);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* Build a body larger than the ordinary-result budget: several long scalar
 * fields plus one nested container, so projection must drop or page. */
static void make_large_body(struct json_value *body)
{
    char big[420];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    json_init(body);
    json_set_object(body);
    for (int i = 0; i < 8; i++) {
        char key[16];
        (void)snprintf(key, sizeof(key), "s%d", i);
        (void)json_push_kv_str(body, key, big);
    }
    struct json_value nested;
    json_init(&nested);
    json_set_object(&nested);
    (void)json_push_kv_str(&nested, "a", big);
    (void)json_push_kv_str(&nested, "b", big);
    (void)json_push_kv(body, "nested", &nested);
    json_free(&nested);
}

static int test_response_budget_views(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    const struct zcl_command_spec *s = find_spec(reg, "core.status");
    char scratch[ZCL_COMMAND_LIST_BUDGET + 1];

    TEST("bridge projection: summary/normal/full page a too-large body") {
        ASSERT(s != NULL);

        /* summary: drop containers, fit the ordinary-result budget. */
        struct json_value body;
        make_large_body(&body);
        struct zcl_command_request req = { .spec = s, .view = "summary" };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        size_t n = json_write(&reply.data, scratch, sizeof(scratch));
        ASSERT(n > 0 && n <= ZCL_COMMAND_RESULT_BUDGET);
        ASSERT(json_get(&reply.data, "nested") == NULL);
        const struct json_value *page = json_get(&reply.data, "_page");
        ASSERT(page != NULL && page->type == JSON_OBJ);
        ASSERT_STR_EQ(json_get_str(json_get(page, "view")), "summary");
        ASSERT(json_get(page, "truncated") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&body);

        /* normal: truncate with an explicit retrieval next command. */
        make_large_body(&body);
        req = (struct zcl_command_request){ .spec = s, .view = "normal" };
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        n = json_write(&reply.data, scratch, sizeof(scratch));
        ASSERT(n > 0 && n <= ZCL_COMMAND_RESULT_BUDGET);
        page = json_get(&reply.data, "_page");
        ASSERT(page != NULL);
        const struct json_value *trunc = json_get(page, "truncated");
        ASSERT(trunc != NULL && trunc->type == JSON_BOOL && trunc->val.b);
        ASSERT(json_get(page, "next_cursor") != NULL);
        ASSERT(reply.next_count >= 1);
        ASSERT(strstr(reply.next[0].input_json, "full") != NULL);
        zcl_command_reply_free(&reply);
        json_free(&body);

        /* full: honor --max-items and page via an advancing cursor. */
        make_large_body(&body);
        req = (struct zcl_command_request){
            .spec = s, .view = "full", .max_items = 3, .cursor = "0",
        };
        zcl_command_reply_init(&reply, s->output_schema);
        zcl_native_bridge_project(&req, &body, &reply);
        page = json_get(&reply.data, "_page");
        ASSERT(page != NULL);
        ASSERT_EQ(json_get_int(json_get(page, "included")), (int64_t)3);
        const struct json_value *nc = json_get(page, "next_cursor");
        ASSERT(nc != NULL);
        ASSERT_EQ(json_get_int(nc), (int64_t)3);
        zcl_command_reply_free(&reply);
        json_free(&body);
        PASS();
    } _test_next:;

    return failures;
}

/* dev.vcs.revert IS a golden catalog row now (config/commands/dev.def via
 * ZCL_COMMAND_DEV_COMMAND, asserted COMPAT above in test_dev_branch_leaves).
 * What test_dev_branch_leaves does NOT reach is the handler body itself: a
 * release/testing build (this test binary is built WITHOUT ZCL_DEV_BUILD,
 * see Makefile TEST_FAST_CFLAGS) must link the `#ifndef ZCL_DEV_BUILD` stub
 * body of zcl_native_handle_dev_vcs_revert — never the real
 * vcs_revert()+shell-fallback path — and that stub must fail closed
 * (BLOCKED, not a silent no-op) instead of mutating anything. */
static int test_dev_vcs_revert_release_stub(void)
{
    int failures = 0;
    TEST("dev.vcs.revert fails closed (BLOCKED) outside a dev build") {
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "to",
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

        struct zcl_command_request request = {
            .spec = NULL,
            .context = NULL,
            .input = &input,
            .view = "normal",
            .budget_bytes = 0,
            .invoked_by_alias = false,
            .invoked_name = "dev.vcs.revert",
        };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.dev_vcs_revert.v1");
        zcl_native_handle_dev_vcs_revert(&request, &reply);

        ASSERT_EQ((int)reply.status, (int)ZCL_COMMAND_STATUS_BLOCKED);
        ASSERT_EQ((int)reply.exit_code, (int)ZCL_COMMAND_EXIT_BLOCKED);
        ASSERT_STR_EQ(reply.error.code, "DEV_BUILD_REQUIRED");
        zcl_command_reply_free(&reply);
        json_free(&input);
        PASS();
    } _test_next:;
    return failures;
}

/* dev.vcs.seal.grant IS a golden catalog row now (config/commands/dev.def
 * via ZCL_COMMAND_DEV_COMMAND, asserted COMPAT above in
 * test_dev_branch_leaves). Same shape as test_dev_vcs_revert_release_stub:
 * this release/testing build (no ZCL_DEV_BUILD) links the `#ifndef
 * ZCL_DEV_BUILD` stub body of zcl_native_handle_dev_vcs_seal_grant — never
 * the real vcs_seal_grant_unseal() path — so the mandatory-confirm gate
 * inside ZCL_DEV_BUILD is not reachable from this binary. What IS provable
 * here is that the stub fails closed (BLOCKED, never a silent mutation)
 * regardless of whether the caller supplied a well-formed, owner-confirmed
 * request or an unconfirmed one — granting a ZVCS unseal token is simply
 * unavailable outside a dev build. */
static int test_dev_vcs_seal_grant_release_stub(void)
{
    int failures = 0;
    TEST("dev.vcs.seal.grant fails closed (BLOCKED) outside a dev build, "
         "confirmed or not") {
        const bool confirms[] = { true, false };
        for (size_t i = 0; i < sizeof(confirms) / sizeof(confirms[0]); i++) {
            struct json_value input;
            json_init(&input);
            json_set_object(&input);
            (void)json_push_kv_str(&input, "reason", "post-baseline review");
            (void)json_push_kv_bool(&input, "confirm", confirms[i]);

            struct zcl_command_request request = {
                .spec = NULL,
                .context = NULL,
                .input = &input,
                .view = "normal",
                .budget_bytes = 0,
                .invoked_by_alias = false,
                .invoked_name = "dev.vcs.seal.grant",
            };
            struct zcl_command_reply reply;
            zcl_command_reply_init(&reply, "zcl.dev_vcs_seal_grant.v1");
            zcl_native_handle_dev_vcs_seal_grant(&request, &reply);

            ASSERT_EQ((int)reply.status, (int)ZCL_COMMAND_STATUS_BLOCKED);
            ASSERT_EQ((int)reply.exit_code, (int)ZCL_COMMAND_EXIT_BLOCKED);
            ASSERT_STR_EQ(reply.error.code, "DEV_BUILD_REQUIRED");
            zcl_command_reply_free(&reply);
            json_free(&input);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* W0: ops.selftest is the native, node-free successor of the MCP
 * `zcl_self_test mode=registry`. It sweeps the catalog for the static
 * well-formedness the registry guarantees. Because test_catalog_wellformed
 * already proves the whole catalog validates, ops.selftest MUST report
 * fail == 0 with a passing envelope, so the dev-lane deploy verify can gate
 * on it without a running node. */
static int test_ops_selftest_registry(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("ops.selftest sweeps the registry and reports fail:0") {
        const struct zcl_command_spec *s = find_spec(reg, "ops.selftest");
        ASSERT(s != NULL);
        ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
        ASSERT(s->handler != NULL);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_OK);
        ASSERT(strstr(out, "\"ok\":true") != NULL);
        ASSERT(strstr(out, "\"mode\":\"registry\"") != NULL);
        ASSERT(strstr(out, "\"fail\":0") != NULL);
        /* At least the READY read/discovery leaves pass. */
        ASSERT(strstr(out, "\"pass\":0") == NULL);
        PASS();
    } _test_next:;
    return failures;
}

/* W0: ops.state is the native successor of the MCP `zcl_state` primitive.
 * Its node-contacting path (dumpstate RPC) needs a live node, but its input
 * guard is node-free: a missing `subsystem` must fail INVALID before any RPC,
 * naming MISSING_SUBSYSTEM and offering an executable next command. */
static int test_ops_state_requires_subsystem(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("ops.state fails INVALID without a subsystem, before any node call") {
        const struct zcl_command_spec *s = find_spec(reg, "ops.state");
        ASSERT(s != NULL);
        ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
        ASSERT(s->handler != NULL);
        ASSERT_STR_EQ(s->input_keys, "subsystem,key");
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
        ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
        ASSERT_EQ(code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT(strstr(out, "\"ok\":false") != NULL);
        ASSERT(strstr(out, "MISSING_SUBSYSTEM") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

static int test_is_root_ownership(void)
{
    int failures = 0;
    TEST("is_root owns core/app/dev/ops/discover but not status") {
        ASSERT(zcl_native_command_is_root("core"));
        ASSERT(zcl_native_command_is_root("app"));
        ASSERT(zcl_native_command_is_root("ops"));
        ASSERT(zcl_native_command_is_root("discover"));
        ASSERT(zcl_native_command_is_root("help"));
        ASSERT(zcl_native_command_is_root("search"));
        ASSERT(!zcl_native_command_is_root("status"));
        ASSERT(zcl_native_command_is_root("dev"));
        ASSERT(!zcl_native_command_is_root("getblockcount"));
        PASS();
    } _test_next:;
    return failures;
}

int test_command_registry_catalog(void)
{
    int failures = 0;
    failures += test_catalog_wellformed();
    failures += test_six_roots();
    failures += test_root_menu_budget();
    failures += test_branch_menus_shallow();
    failures += test_search_bounded();
    failures += test_ready_leaves_bound();
    failures += test_bridge_bindings_reverse();
    failures += test_planned_fail_closed();
    failures += test_envelope_vectors();
    failures += test_dev_branch_leaves();
    failures += test_response_budget_views();
    failures += test_typo_stays_branch();
    failures += test_ops_selftest_registry();
    failures += test_ops_state_requires_subsystem();
    failures += test_dev_vcs_revert_release_stub();
    failures += test_dev_vcs_seal_grant_release_stub();
    failures += test_is_root_ownership();
    printf("=== command_registry_catalog: %d failures ===\n", failures);
    return failures;
}
