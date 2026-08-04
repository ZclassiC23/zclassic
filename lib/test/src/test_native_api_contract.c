/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native-command API contract tests (docs/NATIVE_COMMAND_INTERFACE.md,
 * docs/API_REFERENCE.md).
 *
 * test_command_registry_catalog.c already proves the catalog is well-formed,
 * that a SAMPLE of branch menus stay shallow, that search is bounded to
 * five, and that one leaf (ops.state) fails closed on a missing required
 * key. This file sweeps invariants that sample did not cover for the WHOLE
 * catalog, without contacting a live node:
 *
 *   1. every BRANCH leaf's menu (zcl.command_menu.v1) lists exactly its own
 *      immediate children, in the fixed 5-field child summary shape;
 *   2. every non-branch leaf's dotted machine id resolves back to itself
 *      through the space-separated CLI grammar (contract §3);
 *   3. the declared root/discovery aliases resolve through that same
 *      grammar to their canonical leaf;
 *   4. a second, disjoint set of leaves with a required input key
 *      (discover.describe, discover.schema, dev.app.describe,
 *      dev.app.plan — none of them ops.state) reject an empty input with a
 *      structured zcl.result.v1 error envelope: ok=false, a non-empty
 *      error.code, and exit code INVALID.
 */

#include "test/test_core.h"

#include "base/hex.h"
#include "core/core_io.h"
#include "config/command_catalog.h"
#include "controllers/transaction_controller.h"
#include "dev_failure_store.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "controllers/rpc_client.h"
#include "controllers/status_native_handlers.h"
#include "json/json.h"
#include "keys/key_io.h"
#include "platform/time_compat.h"
#include "rpc/server.h"
#include "sim/simnet.h"
#include "support/cleanse.h"
#include "util/safe_alloc.h"
#include "wallet/keystore.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
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

/* 1. Every branch menu is the fixed zcl.command_menu.v1 shape and lists only
 * its own immediate children — never grandchildren, never a leaf's argument
 * schema, aliases, example, or transport metadata (contract §8). */
static int test_every_branch_menu_lists_only_own_children(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_LIST_BUDGET + 1];
    TEST("every branch menu is zcl.command_menu.v1 with only its own children") {
        int branches_checked = 0;
        for (size_t b = 0; b < reg->count; b++) {
            const struct zcl_command_spec *branch = &reg->commands[b];
            if (branch->mode != ZCL_COMMAND_MODE_BRANCH)
                continue;
            branches_checked++;

            size_t n = zcl_command_registry_menu_json(reg, branch->path, out,
                                                       sizeof(out));
            ASSERT(n > 0);
            ASSERT(n <= ZCL_COMMAND_BRANCH_BUDGET);

            struct json_value doc;
            ASSERT(json_read(&doc, out, n) && doc.type == JSON_OBJ);
            ASSERT_STR_EQ(json_get_str(json_get(&doc, "schema")),
                         "zcl.command_menu.v1");
            ASSERT_STR_EQ(json_get_str(json_get(&doc, "path")), branch->path);

            const struct json_value *children = json_get(&doc, "children");
            ASSERT(children && children->type == JSON_ARR);

            size_t expected = 0;
            for (size_t i = 0; i < reg->count; i++) {
                const char *parent = reg->commands[i].parent;
                if (parent && strcmp(parent, branch->path) == 0)
                    expected++;
            }
            ASSERT_EQ(children->num_children, expected);

            for (size_t i = 0; i < children->num_children; i++) {
                const struct json_value *child = &children->children[i];
                /* Fixed child summary shape: path, summary, risk, latency,
                 * availability — nothing else leaks into a branch menu. */
                ASSERT_EQ(child->num_children, (size_t)5);
                const char *cpath = json_get_str(json_get(child, "path"));
                ASSERT(cpath != NULL && cpath[0]);
                const struct zcl_command_spec *cs = find_spec(reg, cpath);
                ASSERT(cs != NULL);
                ASSERT_STR_EQ(cs->parent, branch->path);
                ASSERT_STR_EQ(json_get_str(json_get(child, "availability")),
                             zcl_command_availability_name(cs->availability));
            }
            json_free(&doc);
        }
        /* root.def + core.def + apps.def + ops.def + dev.def declare ~40
         * branches today; this floor catches an accidental catalog thin-out
         * without pinning an exact count that would rot on every new leaf. */
        ASSERT(branches_checked > 20);
        PASS();
    } _test_next:;
    return failures;
}

/* Split "a.b.c" into up to max_words NUL-terminated segments. */
static size_t split_path_words(const char *path,
                               char words_buf[][ZCL_COMMAND_MAX_PATH],
                               size_t max_words)
{
    size_t count = 0;
    size_t start = 0;
    size_t len = strlen(path);
    for (size_t i = 0; i <= len && count < max_words; i++) {
        if (path[i] == '.' || path[i] == '\0') {
            size_t seg_len = i - start;
            if (seg_len > 0 && seg_len < ZCL_COMMAND_MAX_PATH) {
                memcpy(words_buf[count], path + start, seg_len);
                words_buf[count][seg_len] = 0;
                count++;
            }
            start = i + 1;
        }
    }
    return count;
}

/* 2. Every non-branch leaf's dotted machine id ("core.chain.block.get") is
 * exactly what its space-separated CLI words ("core chain block get")
 * resolve to (contract §3: "Stable machine IDs use dots ... CLI paths use
 * spaces"). This sweeps the WHOLE catalog, not a sample. */
static int test_every_leaf_dot_path_resolves_from_cli_words(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("every leaf's dotted path resolves via its CLI words, 1:1, no alias") {
        int checked = 0;
        for (size_t i = 0; i < reg->count; i++) {
            const struct zcl_command_spec *spec = &reg->commands[i];
            if (spec->mode == ZCL_COMMAND_MODE_BRANCH)
                continue;

            char words_storage[8][ZCL_COMMAND_MAX_PATH];
            size_t n = split_path_words(spec->path, words_storage, 8);
            ASSERT(n > 0 && n <= 8);
            const char *words[8];
            for (size_t w = 0; w < n; w++)
                words[w] = words_storage[w];

            size_t consumed = 0;
            bool was_alias = true;
            char invoked[ZCL_COMMAND_MAX_PATH];
            const struct zcl_command_spec *resolved =
                zcl_command_registry_resolve_words(reg, words, n, &consumed,
                                                   &was_alias, invoked,
                                                   sizeof(invoked));
            ASSERT(resolved != NULL);
            ASSERT_STR_EQ(resolved->path, spec->path);
            ASSERT_EQ(consumed, n);
            ASSERT(!was_alias);
            checked++;
        }
        ASSERT(checked > 80);
        PASS();
    } _test_next:;
    return failures;
}

/* 3. Declared root/discovery aliases resolve through the same word-by-word
 * grammar to their canonical leaf, with was_alias=true (contract §16:
 * "Existing native commands ... become aliases pointing to registry command
 * IDs"). */
static int test_root_and_discover_aliases_resolve(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    TEST("declared aliases resolve to their canonical leaf via CLI words") {
        struct { const char *word; const char *canonical; } cases[] = {
            { "help", "discover.help" },
            { "search", "discover.search" },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            const char *words[1] = { cases[c].word };
            size_t consumed = 0;
            bool was_alias = false;
            char invoked[ZCL_COMMAND_MAX_PATH];
            const struct zcl_command_spec *resolved =
                zcl_command_registry_resolve_words(reg, words, 1, &consumed,
                                                   &was_alias, invoked,
                                                   sizeof(invoked));
            ASSERT(resolved != NULL);
            ASSERT_STR_EQ(resolved->path, cases[c].canonical);
            ASSERT_EQ(consumed, (size_t)1);
            ASSERT(was_alias);
        }
        PASS();
    } _test_next:;
    return failures;
}

/* 4. A second, disjoint set of READY leaves (none of them ops.state, already
 * covered by test_command_registry_catalog.c) reject an empty input with the
 * structured zcl.result.v1 error envelope: ok=false, status=failed, a named
 * error.code, and exit code INVALID — before any node contact. */
static int test_missing_required_input_fails_closed_structured(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char out[ZCL_COMMAND_RESULT_BUDGET + 1];
    TEST("required-input leaves fail closed with a structured error, not a silent pass") {
        struct { const char *path; const char *expected_code; } cases[] = {
            { "discover.describe", "UNKNOWN_PATH" },
            { "discover.schema", "UNKNOWN_PATH" },
            { "dev.app.describe", "MISSING_APP_ID" },
            { "dev.app.plan", "MISSING_ARGS" },
        };
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            const struct zcl_command_spec *s = find_spec(reg, cases[c].path);
            ASSERT(s != NULL);
            ASSERT_EQ(s->availability, ZCL_COMMAND_READY);
            ASSERT(s->handler != NULL);

            enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
            ASSERT(exec_leaf(reg, s, out, sizeof(out), &code));
            ASSERT_EQ(code, ZCL_COMMAND_EXIT_INVALID);
            ASSERT(strstr(out, "\"schema\":\"zcl.result.v1\"") != NULL);
            ASSERT(strstr(out, "\"ok\":false") != NULL);
            ASSERT(strstr(out, "\"status\":\"failed\"") != NULL);

            char code_needle[64];
            (void)snprintf(code_needle, sizeof(code_needle), "\"code\":\"%s\"",
                           cases[c].expected_code);
            ASSERT(strstr(out, code_needle) != NULL);
        }
        PASS();
    } _test_next:;
    return failures;
}

static size_t exec_dev_handler(
    const char *path, zcl_command_handler_fn handler, const char *source_root,
    const struct json_value *input, const char *view,
    char *out, size_t out_size, enum zcl_command_exit *exit_code)
{
    const struct zcl_command_registry *catalog = zcl_command_catalog();
    const struct zcl_command_spec *declared = find_spec(catalog, path);
    if (!declared)
        return 0;
    struct zcl_command_spec executable = *declared;
    executable.availability = ZCL_COMMAND_READY;
    executable.availability_reason = "";
    executable.compat_target = "";
    executable.handler = handler;
    struct zcl_command_registry local = {
        .commands = &executable,
        .count = 1,
    };
    struct zcl_command_context context = {
        .registry = catalog,
        .source_root = source_root,
        .operator_lane = "dev",
        .granted_capabilities = ~(uint64_t)0,
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
        .dev_build = true,
    };
    return zcl_command_registry_execute_json(
        &local, &executable, &context, input, false, path,
        view ? view : "normal", 0, 0, NULL, out, out_size, exit_code);
}

static bool validate_emitted_next(const struct json_value *root,
                                  size_t index, const char *expected_command)
{
    const struct json_value *next = json_get(root, "next");
    if (!next || next->type != JSON_ARR || index >= next->num_children)
        return false;
    const struct json_value *item = &next->children[index];
    const struct json_value *command = json_get(item, "command");
    const struct json_value *input = json_get(item, "input");
    if (!command || command->type != JSON_STR ||
        strcmp(json_get_str(command), expected_command) != 0 ||
        !input || input->type != JSON_OBJ)
        return false;
    const struct zcl_command_spec *next_spec =
        find_spec(zcl_command_catalog(), expected_command);
    char why[160] = {0};
    return next_spec &&
           zcl_command_registry_input_validate(next_spec, input, why,
                                               sizeof(why));
}

static bool run_dev_failure_api_fixture(void)
{
    bool ok = false;
    char home[PATH_MAX], repo[PATH_MAX];
    char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    if (getenv("HOME") && !saved_home)
        return false;
    test_make_tmpdir(home, sizeof(home), "native_api", "dev_failure");
    if (snprintf(repo, sizeof(repo), "%s/repo", home) <= 0 ||
        mkdir(repo, 0700) != 0 || setenv("HOME", home, 1) != 0)
        goto cleanup;

#define API_REQUIRE(expr)                                                    \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "dev-failure API fixture failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #expr);                              \
            goto cleanup;                                                    \
        }                                                                    \
    } while (0)

    struct json_value empty;
    json_init(&empty);
    json_set_object(&empty);
    char out[16384];
    enum zcl_command_exit exit_code = ZCL_COMMAND_EXIT_INTERNAL;
    size_t len = exec_dev_handler(
        "dev.diagnose.latest", zcl_native_handle_dev_diagnose_latest, repo,
        &empty, "normal", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && len <= 2048 && exit_code == ZCL_COMMAND_EXIT_OK);
    struct json_value root;
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    const struct json_value *data = json_get(&root, "data");
    API_REQUIRE(data && data->type == JSON_OBJ);
    API_REQUIRE(strcmp(json_get_str(json_get(data, "schema")),
                       "zcl.dev_failure_latest_result.v1") == 0);
    API_REQUIRE(json_get(data, "found") &&
                !json_get(data, "found")->val.b);
    API_REQUIRE(json_get(&root, "next")->num_children == 0);
    json_free(&root);

    char source[65], mutation[65], execution[65];
    memset(source, 'a', 64);
    memset(mutation, 'b', 64);
    memset(execution, 'c', 64);
    source[64] = mutation[64] = execution[64] = 0;
    char first_error[511], capsule[1023];
    first_error[0] = 'x';
    for (size_t i = 1; i < sizeof(first_error) - 1; i++)
        first_error[i] = i % 2 ? '"' : '\\';
    first_error[sizeof(first_error) - 1] = 0;
    for (size_t i = 0; i < sizeof(capsule) - 1; i++)
        capsule[i] = i % 2 ? '"' : '\\';
    capsule[sizeof(capsule) - 1] = 0;
    struct zcl_dev_failure_record record;
    char why[192] = {0};
    API_REQUIRE(zcl_dev_failure_record_failure(
        repo, source, mutation, execution, "verify.compile", first_error,
        capsule, "dev.ff", &record, why, sizeof(why)));

    len = exec_dev_handler(
        "dev.diagnose.latest", zcl_native_handle_dev_diagnose_latest, repo,
        &empty, "normal", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && len <= 2048 && exit_code == ZCL_COMMAND_EXIT_OK);
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    data = json_get(&root, "data");
    API_REQUIRE(data && json_get(data, "found")->val.b);
    API_REQUIRE(strcmp(json_get_str(json_get(data, "failure_id")),
                       record.failure_id) == 0);
    API_REQUIRE(validate_emitted_next(&root, 0, "dev.diagnose.show"));
    const struct json_value *next = json_get(&root, "next");
    const struct json_value *next_input =
        json_get(&next->children[0], "input");
    API_REQUIRE(strcmp(json_get_str(json_get(next_input, "failure_id")),
                       record.failure_id) == 0);
    json_free(&root);

    struct json_value ref;
    json_init(&ref);
    json_set_object(&ref);
    API_REQUIRE(json_push_kv_str(&ref, "failure_id", record.failure_id));
    len = exec_dev_handler(
        "dev.diagnose.show", zcl_native_handle_dev_diagnose_show, repo,
        &ref, "summary", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && len <= 2048 && exit_code == ZCL_COMMAND_EXIT_OK);
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    data = json_get(&root, "data");
    API_REQUIRE(data && strcmp(json_get_str(json_get(data, "schema")),
                               "zcl.dev_failure_show.v1") == 0);
    API_REQUIRE(!json_get(data, "record_sha3"));
    API_REQUIRE(!json_get(data, "failure_capsule"));
    API_REQUIRE(validate_emitted_next(&root, 0, "dev.ff"));
    json_free(&root);

    len = exec_dev_handler(
        "dev.diagnose.show", zcl_native_handle_dev_diagnose_show, repo,
        &ref, "normal", out, sizeof(out), &exit_code);
    /* Bound raised 2048 -> 2144 to absorb OS-B2's per-command latency contract
     * (budget_ms/elapsed_ms/budget_exceeded, ~55 bytes) now in every envelope. */
    API_REQUIRE(len > 0 && len <= 2144 && exit_code == ZCL_COMMAND_EXIT_OK);
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    data = json_get(&root, "data");
    API_REQUIRE(json_get(data, "record_sha3") != NULL);
    API_REQUIRE(json_get(data, "first_source_mutation_sha256") != NULL);
    API_REQUIRE(json_get(data, "first_execution_id_sha3") != NULL);
    API_REQUIRE(json_get(data, "capsule_available")->val.b);
    API_REQUIRE(!json_get(data, "failure_capsule"));
    json_free(&root);

    len = exec_dev_handler(
        "dev.diagnose.show", zcl_native_handle_dev_diagnose_show, repo,
        &ref, "full", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && len <= 6144 && exit_code == ZCL_COMMAND_EXIT_OK);
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    data = json_get(&root, "data");
    API_REQUIRE(strcmp(json_get_str(json_get(data, "failure_capsule")),
                       capsule) == 0);
    API_REQUIRE(strcmp(json_get_str(json_get(data, "retry_command")),
                       "dev.ff") == 0);
    json_free(&root);

    char uppercase[65];
    memset(uppercase, 'A', 64);
    uppercase[64] = 0;
    json_free(&ref);
    json_init(&ref);
    json_set_object(&ref);
    API_REQUIRE(json_push_kv_str(&ref, "failure_id", uppercase));
    len = exec_dev_handler(
        "dev.diagnose.show", zcl_native_handle_dev_diagnose_show, repo,
        &ref, "normal", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && exit_code == ZCL_COMMAND_EXIT_INVALID);
    API_REQUIRE(strstr(out, "\"code\":\"INVALID_FAILURE_ID\"") != NULL);

    char missing[65];
    memset(missing, 'f', 64);
    missing[64] = 0;
    json_free(&ref);
    json_init(&ref);
    json_set_object(&ref);
    API_REQUIRE(json_push_kv_str(&ref, "failure_id", missing));
    len = exec_dev_handler(
        "dev.diagnose.show", zcl_native_handle_dev_diagnose_show, repo,
        &ref, "normal", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && exit_code == ZCL_COMMAND_EXIT_FAILED);
    API_REQUIRE(strstr(out, "\"code\":\"FAILURE_NOT_FOUND\"") != NULL);
    json_init(&root);
    API_REQUIRE(json_read(&root, out, len));
    API_REQUIRE(validate_emitted_next(&root, 0, "dev.diagnose.latest"));
    json_free(&root);

    char latest_path[PATH_MAX];
    API_REQUIRE(snprintf(
        latest_path, sizeof(latest_path),
        "%s/.local/state/zclassic23-dev/workspaces/%s/latest-failure.json",
        home, record.workspace_id) > 0);
    int fd = open(latest_path, O_WRONLY | O_CLOEXEC);
    API_REQUIRE(fd >= 0 && pwrite(fd, "X", 1, 0) == 1 && close(fd) == 0);
    len = exec_dev_handler(
        "dev.diagnose.latest", zcl_native_handle_dev_diagnose_latest, repo,
        &empty, "normal", out, sizeof(out), &exit_code);
    API_REQUIRE(len > 0 && exit_code == ZCL_COMMAND_EXIT_INTERNAL);
    API_REQUIRE(strstr(out, "\"code\":\"FAILURE_STORE_INVALID\"") != NULL);

    json_free(&ref);
    json_free(&empty);
    ok = true;

cleanup:
    if (saved_home) {
        (void)setenv("HOME", saved_home, 1);
        free(saved_home);
    } else
        (void)unsetenv("HOME");
    test_rm_rf_recursive(home);
#undef API_REQUIRE
    return ok;
}

static int test_dev_failure_native_api(void)
{
    int failures = 0;
    TEST("native API: compiler failure projections are bounded, typed, and fail closed") {
        ASSERT(run_dev_failure_api_fixture());
        PASS();
    } _test_next:;
    return failures;
}

static int test_native_app_catalog_uses_strict_builtin_source(void)
{
    int failures = 0;
    TEST("native app list and inspect share the strict built-in catalog") {
        struct json_value input;
        json_init(&input);
        json_set_object(&input);
        struct zcl_command_request request = {
            .input = &input,
            .view = "normal",
            .invoked_name = "app.list",
        };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, "zcl.app_index.v1");
        zcl_native_handle_app_list(&request, &reply);
        const struct json_value *apps = json_get(&reply.data, "apps");
        ASSERT(apps && apps->type == JSON_ARR && apps->num_children == 3);
        ASSERT_STR_EQ(json_get_str(&apps->children[0]), "blog");
        ASSERT_STR_EQ(json_get_str(&apps->children[1]), "social");
        ASSERT_STR_EQ(json_get_str(&apps->children[2]), "yardsale");
        ASSERT_EQ(json_get_int(json_get(&reply.data, "count")), 3);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "catalog")),
                      "built-in-strict-v1");
        zcl_command_reply_free(&reply);

        (void)json_push_kv_str(&input, "app_id", "blog");
        request.invoked_name = "app.inspect";
        zcl_command_reply_init(&reply, "zcl.app_inspect.v1");
        zcl_native_handle_app_inspect(&request, &reply);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "app_id")), "blog");
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "manifest")),
                      "apps/blog/app.def");
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "authority")),
                      "definition-only");
        zcl_command_reply_free(&reply);

        json_free(&input);
        json_init(&input);
        json_set_object(&input);
        (void)json_push_kv_str(&input, "app_id", "missing");
        request.input = &input;
        zcl_command_reply_init(&reply, "zcl.app_inspect.v1");
        zcl_native_handle_app_inspect(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_BLOCKED);
        ASSERT_STR_EQ(reply.error.code, "UNKNOWN_APP");
        zcl_command_reply_free(&reply);
        json_free(&input);
        PASS();
    } _test_next:;
    return failures;
}

/* ── wf/status-front-door ─────────────────────────────────────────
 *
 * The flagless `zclassic23 status` front door (core.status.brief,
 * status_brief_native_handler.c) must always answer truthfully and fast.
 * Two contracts, tested directly against zcl_native_status_brief_body
 * (below the command-registry envelope, which test_command_registry_
 * catalog.c's test_status_brief_* already covers):
 *
 *   - schema-skew tolerance: a PRESENT schema in the known
 *     zcl.public_status.* family that isn't the exact version validated
 *     strictly (an older node's v1, a future v4) degrades to a
 *     best-effort brief instead of the old one-size-fits-all "invalid
 *     zcl.public_status.v2" error; an ABSENT schema, or a genuinely
 *     malformed field on a MATCHING v2 document, still fails closed.
 *   - the ~250ms front-door deadline: a peer that accepts the TCP
 *     connection but never answers must not be able to hold the call for
 *     the generic 10s RPC ceiling. */

static const char *g_status_body_rpc_fixture;

static char *status_body_mock_rpc(const char *method, const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "agent") == 0 && g_status_body_rpc_fixture)
        return strdup(g_status_body_rpc_fixture);
    return strdup("null");
}

static char *status_frontdoor_mock_rpc(const char *method,
                                       const char *params_json)
{
    (void)params_json;
    if (strcmp(method, "dumpstate") == 0 && g_status_body_rpc_fixture)
        return strdup(g_status_body_rpc_fixture);
    return strdup("null");
}

static int test_native_bridge_resident_binding(void)
{
    int failures = 0;
    TEST("native bridge: resident RPC binding survives the lazy ensure seam") {
        zcl_native_bridge_bind_rpc("/tmp/zcl-resident-dev", 18252);
        zcl_native_bridge_ensure_rpc();
        ASSERT_STR_EQ(node_rpc_client_datadir(), "/tmp/zcl-resident-dev");
        zcl_native_bridge_bind_rpc("", 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_status_frontdoor_preserves_rpc_error(void)
{
    int failures = 0;
    TEST("zcl_native_status_body: JSON-RPC failures keep the transport/auth "
         "diagnosis instead of masquerading as a candidate schema failure") {
        node_rpc_client_set_test_hook(status_frontdoor_mock_rpc);
        static const char cookie_error[] =
            "{\"error\":{\"code\":-32603,\"message\":"
            "\"cannot read RPC auth cookie at /tmp/dev/.cookie\"}}";
        g_status_body_rpc_fixture = cookie_error;
        struct zcl_native_body_err err = {0};
        char *body = zcl_native_status_body(NULL, &err);
        ASSERT(body == NULL);
        ASSERT_EQ((int)err.status, (int)ZCL_NATIVE_BODY_UNAVAILABLE);
        ASSERT(strstr(err.message, "RPC failed") != NULL);
        ASSERT(strstr(err.message, "cannot read RPC auth cookie") != NULL);
        ASSERT(strstr(err.message, "missing state object") == NULL);

        static const char healthy[] =
            "{\"state\":{\"schema\":\"zcl.status_frontdoor.v1\","
            "\"serving\":true}}";
        g_status_body_rpc_fixture = healthy;
        err = (struct zcl_native_body_err){0};
        body = zcl_native_status_body(NULL, &err);
        ASSERT(body != NULL);
        struct json_value data;
        json_init(&data);
        ASSERT(json_read(&data, body, strlen(body)) && data.type == JSON_OBJ);
        ASSERT(json_get_bool(json_get(&data, "serving")));
        ASSERT_STR_EQ(json_get_str(json_get(&data, "status_source")),
                      "status_frontdoor");
        json_free(&data);
        free(body);
        PASS();
    } _test_next:;
    g_status_body_rpc_fixture = NULL;
    node_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_status_brief_body_schema_skew_tolerance(void)
{
    int failures = 0;
    TEST("zcl_native_status_brief_body: present-but-older schema degrades "
        "gracefully; absent schema and matching-v2-malformed still fail "
        "closed") {
        node_rpc_client_set_test_hook(status_body_mock_rpc);

        /* (a) An older node (v1) still carries the fields this CLI knows
         * under the same names -- those must surface, ok:true, not a hard
         * schema error. */
        static const char older[] =
            "{\"schema\":\"zcl.public_status.v1\","
            "\"served_height\":42,\"served_height_known\":true,"
            "\"serving\":true,\"healthy\":true,"
            "\"primary_blocker\":\"none\"}";
        g_status_body_rpc_fixture = older;
        struct zcl_native_body_err err = {0};
        char *body = zcl_native_status_brief_body(NULL, &err);
        ASSERT(body != NULL);
        struct json_value data;
        ASSERT(json_read(&data, body, strlen(body)) &&
              data.type == JSON_OBJ);
        ASSERT_EQ(json_get_int(json_get(&data, "hstar")), (int64_t)42);
        ASSERT(json_get_bool(json_get(&data, "serving")));
        ASSERT(json_get_bool(json_get(&data, "healthy")));
        ASSERT_STR_EQ(json_get_str(json_get(&data, "primary_blocker")),
                      "none");
        ASSERT(json_get_bool(json_get(&data, "partial_result")));
        ASSERT_STR_EQ(json_get_str(json_get(&data, "schema_skew")),
                      "zcl.public_status.v1");
        json_free(&data);
        free(body);

        /* (b) An entirely ABSENT schema key is unaffected by the new
         * tolerance (there is no schema value to match against the known
         * family) -- still the pre-existing version-skew hard failure. */
        static const char no_schema[] = "{\"served_height\":42}";
        g_status_body_rpc_fixture = no_schema;
        err = (struct zcl_native_body_err){0};
        body = zcl_native_status_brief_body(NULL, &err);
        ASSERT(body == NULL);
        ASSERT_EQ((int)err.status, (int)ZCL_NATIVE_BODY_UNAVAILABLE);
        ASSERT(strstr(err.message, "predates the CLI contract") != NULL);

        /* (c) A MATCHING v2 schema with a genuinely malformed field must
         * still fail closed -- schema-skew tolerance never weakens strict
         * validation of the exact contract version this build targets. */
        static const char malformed_v2[] =
            "{\"schema\":\"zcl.public_status.v2\","
            "\"served_height\":\"not-an-int\","
            "\"served_height_known\":true}";
        g_status_body_rpc_fixture = malformed_v2;
        err = (struct zcl_native_body_err){0};
        body = zcl_native_status_brief_body(NULL, &err);
        ASSERT(body == NULL);
        ASSERT_EQ((int)err.status, (int)ZCL_NATIVE_BODY_UNAVAILABLE);
        ASSERT(strstr(err.message, "invalid zcl.public_status.v2") != NULL);
        ASSERT(strstr(err.message, "predates the CLI contract") == NULL);

        PASS();
    } _test_next:;
    g_status_body_rpc_fixture = NULL;
    node_rpc_client_set_test_hook(NULL);
    return failures;
}

static int test_status_brief_body_front_door_deadline(void)
{
    int failures = 0;
    char *dir = NULL;
    int blackhole = -1;
    TEST("zcl_native_status_brief_body: a peer that accepts the connection "
        "but never answers is bounded by the ~250ms front-door deadline, "
        "not the generic 10s RPC ceiling") {
        /* Force the REAL out-of-process HTTP path -- no test hook -- so
         * this proves the actual socket-level deadline plumbing, not a
         * mock. */
        node_rpc_client_set_test_hook(NULL);

        /* A bound+listening socket completes the client's connect() via the
         * kernel accept queue with nobody ever calling accept() -- exactly
         * "TCP up, nobody home to answer" (see rpc_client.c's "node
         * accepted the connection but did not answer" branch, and the same
         * pattern in test_cli_auth_robust.c). */
        blackhole = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT(blackhole >= 0);
        struct sockaddr_in addr = { 0 };
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        int reuse = 1;
        setsockopt(blackhole, SOL_SOCKET, SO_REUSEADDR, &reuse,
                  sizeof(reuse));
        ASSERT(bind(blackhole, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        socklen_t alen = sizeof(addr);
        ASSERT(getsockname(blackhole, (struct sockaddr *)&addr, &alen) == 0);
        uint16_t port = ntohs(addr.sin_port);
        ASSERT(listen(blackhole, 1) == 0);

        char dir_template[] = "/tmp/zcl-status-frontdoor-XXXXXX";
        dir = strdup(mkdtemp(dir_template));
        ASSERT(dir != NULL);
        char cookie_path[320];
        (void)snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", dir);
        FILE *cf = fopen(cookie_path, "w");
        ASSERT(cf != NULL);
        (void)fprintf(cf, "dummyuser:dummypass\n");
        (void)fclose(cf);

        node_rpc_client_init(dir, (int)port);
        ASSERT(setenv("ZCL_STATUS_DEADLINE_MS", "200", 1) == 0);

        int64_t t0 = platform_time_monotonic_ms();
        struct zcl_native_body_err err = {0};
        char *body = zcl_native_status_brief_body(NULL, &err);
        int64_t elapsed_ms = platform_time_monotonic_ms() - t0;

        (void)unsetenv("ZCL_STATUS_DEADLINE_MS");

        /* Well under the generic 10s (ZCL_RPC_DEADLINE_MS default) ceiling
         * -- proves the ~200ms front-door budget actually bounds the call
         * rather than falling back to the env-wide default. Generous
         * margin against CI scheduling jitter. */
        ASSERT(elapsed_ms < 3000);
        ASSERT(body == NULL);
        ASSERT_EQ((int)err.status, (int)ZCL_NATIVE_BODY_UNAVAILABLE);
        ASSERT(strstr(err.message, "did not answer within the deadline") !=
              NULL);
        PASS();
    } _test_next:;
    if (blackhole >= 0)
        close(blackhole);
    if (dir) {
        char rmcmd[512];
        (void)snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", dir);
        (void)system(rmcmd);
        free(dir);
    }
    node_rpc_client_set_test_hook(NULL);
    node_rpc_client_init("", 0);
    return failures;
}

/* ── mutating core.wallet.* leaves: E2E over a stubbed wallet RPC ──────────
 * These leaves are dedicated handlers that reach the node over node_rpc_call.
 * With the ZCL_TESTING test hook installed we drive the real registry handlers
 * through their plan/commit contract with NO live node: address.new persists
 * an address, and transaction.send only broadcasts on the confirmed call. */
static int g_wallet_send_calls;
static int g_wallet_raw_broadcast_calls;

static char *wallet_stub_rpc(const char *method, const char *params_json)
{
    (void)params_json;
    if (method && strcmp(method, "getnewaddress") == 0)
        return strdup("\"t1StubTransparentAddress00000000000\"");
    if (method && strcmp(method, "sendtoaddress") == 0) {
        g_wallet_send_calls++;
        return strdup(
            "\"aa11bb22cc33dd44ee55ff66aa77bb88"
            "cc99dd00ee11ff22aa33bb44cc55dd66\"");
    }
    if (method && strcmp(method, "createrawtransaction") == 0)
        return strdup("\"0400008085202f89000000000000000000000000\"");
    if (method && strcmp(method, "signrawtransaction") == 0)
        return strdup("{\"hex\":\"0400008085202f89000000000000000000000000\","
                      "\"complete\":true}");
    if (method && strcmp(method, "sendrawtransaction") == 0) {
        g_wallet_raw_broadcast_calls++;
        return strdup(
            "\"bb11bb22cc33dd44ee55ff66aa77bb88"
            "cc99dd00ee11ff22aa33bb44cc55dd77\"");
    }
    return strdup("null");
}

static int test_wallet_mutating_native_e2e(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();

    TEST("core.wallet mutating leaves execute plan/commit over a stubbed RPC") {
        g_wallet_send_calls = 0;
        g_wallet_raw_broadcast_calls = 0;
        node_rpc_client_set_test_hook(wallet_stub_rpc);

        /* 1. address.new persists and returns a fresh transparent address. */
        const struct zcl_command_spec *new_spec =
            find_spec(reg, "core.wallet.address.new");
        ASSERT(new_spec != NULL);
        ASSERT(new_spec->availability == ZCL_COMMAND_READY);
        ASSERT(new_spec->handler != NULL);
        struct json_value empty;
        json_init(&empty);
        json_set_object(&empty);
        struct zcl_command_request req_new = {
            .spec = new_spec, .input = &empty, .view = "normal",
        };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, new_spec->output_schema);
        zcl_native_handle_wallet_address_new(&req_new, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "address")),
                      "t1StubTransparentAddress00000000000");
        ASSERT(json_get_bool(json_get(&reply.data, "created")));
        ASSERT(reply.error.mutated);
        zcl_command_reply_free(&reply);
        json_free(&empty);

        /* 2. transaction.send WITHOUT confirm returns a plan and DOES NOT
         *    broadcast (g_wallet_send_calls stays 0). */
        const struct zcl_command_spec *send_spec =
            find_spec(reg, "core.wallet.transaction.send");
        ASSERT(send_spec != NULL);
        ASSERT(send_spec->availability == ZCL_COMMAND_READY);
        ASSERT(send_spec->confirmation == ZCL_COMMAND_CONFIRM_PLAN_COMMIT);
        struct json_value plan_in;
        json_init(&plan_in);
        json_set_object(&plan_in);
        (void)json_push_kv_str(&plan_in, "address",
                               "t1Dest0000000000000000000000000000");
        (void)json_push_kv_real(&plan_in, "amount", 1.25);
        struct zcl_command_request req_plan = {
            .spec = send_spec, .input = &plan_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_wallet_transaction_send(&req_plan, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT(!json_get_bool(json_get(&reply.data, "committed")));
        ASSERT(!reply.error.mutated);
        /* No next-action, and specifically not one naming this same leaf.
         * push_next_array() rejects a next whose path equals the running
         * command, so emitting one failed the WHOLE envelope and this leaf
         * answered RESPONSE_BUDGET_EXCEEDED instead of a plan. The previous
         * version of this test asserted next_count >= 1 and passed throughout,
         * because it only ever inspected the in-memory reply and never asked
         * whether that reply could be serialized. */
        ASSERT_EQ(reply.next_count, 0);
        ASSERT_EQ(g_wallet_send_calls, 0);
        /* The committing input travels as data, and must validate against
         * this leaf — re-running it with that input is what commits. */
        const char *commit_input =
            json_get_str(json_get(&reply.data, "commit_input"));
        ASSERT(commit_input && commit_input[0]);
        struct json_value commit_next;
        json_init(&commit_next);
        ASSERT(json_read(&commit_next, commit_input, strlen(commit_input)));
        char why[160] = {0};
        ASSERT(zcl_command_registry_input_validate(send_spec, &commit_next,
                                                   why, sizeof(why)));
        ASSERT(json_get_bool(json_get(&commit_next, "confirm")));
        json_free(&commit_next);
        /* THE check the old assertion was missing: the plan reply must
         * actually serialize. A reply that cannot be rendered is not a plan,
         * whatever its fields say. */
        {
            char rendered[8192];
            enum zcl_command_exit rc = ZCL_COMMAND_EXIT_OK;
            struct zcl_command_context sctx = {
                .registry = reg,
                .granted_capabilities = ~(uint64_t)0,
                .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
            };
            size_t n = zcl_command_registry_execute_json(
                reg, send_spec, &sctx, &plan_in, false, send_spec->path,
                "normal", 0, 0, NULL, rendered, sizeof(rendered), &rc);
            ASSERT(n > 0);
            ASSERT_EQ(rc, ZCL_COMMAND_EXIT_OK);
            ASSERT(strstr(rendered, "\"stage\":\"plan\"") != NULL);
        }
        zcl_command_reply_free(&reply);
        json_free(&plan_in);

        /* 3. transaction.send WITH confirm:true broadcasts and returns a txid;
         *    exactly one sendtoaddress call fired. */
        struct json_value commit_in;
        json_init(&commit_in);
        json_set_object(&commit_in);
        (void)json_push_kv_str(&commit_in, "address",
                               "t1Dest0000000000000000000000000000");
        (void)json_push_kv_real(&commit_in, "amount", 1.25);
        (void)json_push_kv_bool(&commit_in, "confirm", true);
        struct zcl_command_request req_commit = {
            .spec = send_spec, .input = &commit_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_wallet_transaction_send(&req_commit, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "committed");
        ASSERT(json_get_bool(json_get(&reply.data, "committed")));
        ASSERT(reply.error.mutated);
        ASSERT(json_get_str(json_get(&reply.data, "txid")) != NULL);
        ASSERT_EQ(g_wallet_send_calls, 1);
        zcl_command_reply_free(&reply);
        json_free(&commit_in);

        /* 4. raw create/sign do not broadcast; raw broadcast itself obeys
         *    the plan/commit boundary. */
        const struct zcl_command_spec *raw_create =
            find_spec(reg, "core.wallet.transaction.raw.create");
        const struct zcl_command_spec *raw_sign =
            find_spec(reg, "core.wallet.transaction.raw.sign");
        const struct zcl_command_spec *raw_broadcast =
            find_spec(reg, "core.wallet.transaction.raw.broadcast");
        ASSERT(raw_create && raw_sign && raw_broadcast);
        struct json_value raw_in, inputs, outputs;
        json_init(&raw_in); json_set_object(&raw_in);
        json_init(&inputs); json_set_array(&inputs);
        json_init(&outputs); json_set_object(&outputs);
        (void)json_push_kv(&raw_in, "inputs", &inputs);
        (void)json_push_kv(&raw_in, "outputs", &outputs);
        json_free(&inputs); json_free(&outputs);
        struct zcl_command_request raw_req = {
            .spec = raw_create, .input = &raw_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, raw_create->output_schema);
        zcl_native_handle_wallet_raw_create(&raw_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const char *raw_hex = json_get_str(json_get(&reply.data, "raw_hex"));
        ASSERT(raw_hex && raw_hex[0]);
        char raw_copy[128];
        (void)snprintf(raw_copy, sizeof(raw_copy), "%s", raw_hex);
        zcl_command_reply_free(&reply);
        json_free(&raw_in);

        json_init(&raw_in); json_set_object(&raw_in);
        (void)json_push_kv_str(&raw_in, "raw_hex", raw_copy);
        raw_req.spec = raw_sign; raw_req.input = &raw_in;
        zcl_command_reply_init(&reply, raw_sign->output_schema);
        zcl_native_handle_wallet_raw_sign(&raw_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_get_bool(json_get(&reply.data, "complete")));
        ASSERT_EQ(g_wallet_raw_broadcast_calls, 0);
        zcl_command_reply_free(&reply);

        raw_req.spec = raw_broadcast;
        zcl_command_reply_init(&reply, raw_broadcast->output_schema);
        zcl_native_handle_wallet_raw_broadcast(&raw_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT_EQ(g_wallet_raw_broadcast_calls, 0);
        zcl_command_reply_free(&reply);
        (void)json_push_kv_bool(&raw_in, "confirm", true);
        zcl_command_reply_init(&reply, raw_broadcast->output_schema);
        zcl_native_handle_wallet_raw_broadcast(&raw_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "committed");
        ASSERT_EQ(g_wallet_raw_broadcast_calls, 1);
        zcl_command_reply_free(&reply);
        json_free(&raw_in);

        /* 5. export-key without confirm must NOT reveal a key. */
        const struct zcl_command_spec *xk_spec =
            find_spec(reg, "core.wallet.address.export-key");
        ASSERT(xk_spec != NULL);
        ASSERT(xk_spec->availability == ZCL_COMMAND_READY);
        struct json_value xk_in;
        json_init(&xk_in);
        json_set_object(&xk_in);
        (void)json_push_kv_str(&xk_in, "address",
                               "t1Dest0000000000000000000000000000");
        struct zcl_command_request req_xk = {
            .spec = xk_spec, .input = &xk_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, xk_spec->output_schema);
        zcl_native_handle_wallet_address_export_key(&req_xk, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT(json_get(&reply.data, "privkey") == NULL);
        zcl_command_reply_free(&reply);
        json_free(&xk_in);

        /* 6. a missing required key fails closed with a typed error body. */
        struct json_value bad_in;
        json_init(&bad_in);
        json_set_object(&bad_in);
        struct zcl_command_request req_bad = {
            .spec = send_spec, .input = &bad_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_wallet_transaction_send(&req_bad, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT_STR_EQ(reply.error.code, "MISSING_ADDRESS");
        ASSERT(reply.error.message[0] != '\0');
        ASSERT(!reply.error.mutated);
        ASSERT_EQ(g_wallet_send_calls, 1);
        zcl_command_reply_free(&reply);
        json_free(&bad_in);

        node_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    /* Never leave a stub installed for the next group in this process. */
    node_rpc_client_set_test_hook(NULL);
    return failures;
}

/* The ordinary wallet contract above deliberately uses canned RPC bodies so
 * it can pin plan/commit behavior without allocating consensus state. This
 * second bridge is the transaction-lab proof: the public typed handlers call
 * the REAL raw-transaction RPC actors, and the exact signed bytes returned by
 * those actors are decoded and mined through simnet/connect_block. */
struct raw_simnet_bridge {
    struct rpc_table *table;
    struct simnet *sim;
    int broadcast_calls;
    bool mined;
    struct uint256 txid;
};

static struct raw_simnet_bridge *g_raw_simnet_bridge;

static char *raw_simnet_rpc(const char *method, const char *params_json)
{
    struct raw_simnet_bridge *bridge = g_raw_simnet_bridge;
    if (!bridge || !bridge->table || !bridge->sim || !method ||
        !params_json)
        return NULL;

    struct json_value params;
    json_init(&params);
    if (!json_read(&params, params_json, strlen(params_json))) {
        json_free(&params);
        return strdup("{\"code\":-1,\"message\":\"invalid lab RPC params\"}");
    }

    struct json_value result;
    json_init(&result);
    bool ok = rpc_table_execute(bridge->table, method, &params, &result);

    if (ok && strcmp(method, "sendrawtransaction") == 0) {
        bridge->broadcast_calls++;
        const char *raw_hex = json_get_str(json_at(&params, 0));
        struct transaction tx;
        transaction_init(&tx);
        if (!raw_hex || !decode_hex_tx(&tx, raw_hex)) {
            transaction_free(&tx);
            ok = false;
        } else {
            bridge->txid = tx.hash;
            /* simnet_mint_txs takes ownership after a valid decoded request,
             * whether admission succeeds or not. */
            ok = simnet_mint_txs(bridge->sim, &tx, 1);
            bridge->mined = ok;
        }
        if (!ok) {
            json_free(&result);
            json_init(&result);
            json_set_object(&result);
            (void)json_push_kv_int(&result, "code", -1);
            (void)json_push_kv_str(&result, "message",
                                   "simnet rejected signed raw transaction");
        }
    }

    size_t need = json_write(&result, NULL, 0);
    char *out = zcl_malloc(need + 1, "raw simnet RPC result");
    if (out) {
        (void)json_write(&result, out, need + 1);
        out[need] = '\0';
    }
    json_free(&result);
    json_free(&params);
    return out;
}

static int test_raw_native_pipeline_mines_exact_signed_bytes(void)
{
    int failures = 0;
    struct simnet sim;
    memset(&sim, 0, sizeof(sim));
    bool sim_ready = false;
    struct basic_keystore *ks = NULL;
    bool ks_ready = false;
    struct privkey funding_key = {0};
    struct privkey recipient_key = {0};

    TEST("raw typed create/sign/broadcast mines exact signed bytes in simnet") {
        ASSERT(simnet_init(&sim));
        sim_ready = true;

        ks = zcl_malloc(sizeof(*ks), "raw simnet keystore");
        ASSERT(ks != NULL);
        keystore_init(ks);
        ks_ready = true;

        struct pubkey funding_pub;
        privkey_make_new(&funding_key, true);
        ASSERT(privkey_get_pubkey(&funding_key, &funding_pub));
        struct key_id funding_id = pubkey_get_id(&funding_pub);
        ASSERT(keystore_add_key(ks, &funding_key));

        struct script funding_script;
        script_for_p2pkh(&funding_script, &funding_id);
        struct uint256 funding_txid;
        int funding_height = simnet_tip_height(&sim) + 1;
        ASSERT(simnet_mint_coinbase_to(&sim, &funding_script, 1000000,
                                      &funding_txid));
        ASSERT(simnet_mint_to_height(
            &sim, funding_height + COINBASE_MATURITY));
        ASSERT(simnet_coin_exists(&sim, &funding_txid));

        struct pubkey recipient_pub;
        privkey_make_new(&recipient_key, true);
        ASSERT(privkey_get_pubkey(&recipient_key, &recipient_pub));
        struct tx_destination recipient = {0};
        recipient.type = DEST_KEY_ID;
        recipient.id.key = pubkey_get_id(&recipient_pub);
        const struct chain_params *cp = chain_params_get();
        size_t pk_len = 0, script_len = 0;
        const unsigned char *pk_prefix = chain_params_base58_prefix(
            cp, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *script_prefix = chain_params_base58_prefix(
            cp, B58_SCRIPT_ADDRESS, &script_len);
        char recipient_address[128];
        ASSERT(encode_destination(&recipient, pk_prefix, pk_len,
                                  script_prefix, script_len,
                                  recipient_address,
                                  sizeof(recipient_address)));

        struct rpc_table raw_table;
        rpc_table_init(&raw_table);
        register_rawtransaction_rpc_commands(&raw_table);
        char warmup_status[64];
        if (rpc_is_in_warmup(warmup_status, sizeof(warmup_status)))
            set_rpc_warmup_finished();
        rpc_rawtx_set_state(NULL, NULL, NULL, NULL);
        rpc_rawtx_set_keystore(ks);

        struct raw_simnet_bridge bridge = {
            .table = &raw_table,
            .sim = &sim,
        };
        uint256_set_null(&bridge.txid);
        g_raw_simnet_bridge = &bridge;
        node_rpc_client_set_test_hook(raw_simnet_rpc);

        const struct zcl_command_registry *reg = zcl_command_catalog();
        const struct zcl_command_spec *create_spec = find_spec(
            reg, "core.wallet.transaction.raw.create");
        const struct zcl_command_spec *sign_spec = find_spec(
            reg, "core.wallet.transaction.raw.sign");
        const struct zcl_command_spec *broadcast_spec = find_spec(
            reg, "core.wallet.transaction.raw.broadcast");
        ASSERT(create_spec && sign_spec && broadcast_spec);

        char funding_hex[65];
        uint256_get_hex(&funding_txid, funding_hex);
        struct json_value create_in, inputs, input, outputs;
        json_init(&create_in); json_set_object(&create_in);
        json_init(&inputs); json_set_array(&inputs);
        json_init(&input); json_set_object(&input);
        (void)json_push_kv_str(&input, "txid", funding_hex);
        (void)json_push_kv_int(&input, "vout", 0);
        (void)json_push_back(&inputs, &input);
        json_free(&input);
        json_init(&outputs); json_set_object(&outputs);
        (void)json_push_kv_real(&outputs, recipient_address, 0.008);
        (void)json_push_kv(&create_in, "inputs", &inputs);
        (void)json_push_kv(&create_in, "outputs", &outputs);
        json_free(&inputs); json_free(&outputs);

        struct zcl_command_request request = {
            .spec = create_spec, .input = &create_in, .view = "normal",
        };
        struct zcl_command_reply reply;
        zcl_command_reply_init(&reply, create_spec->output_schema);
        zcl_native_handle_wallet_raw_create(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        const char *created = json_get_str(json_get(&reply.data, "raw_hex"));
        ASSERT(created && strlen(created) < 8192);
        char created_hex[8192];
        (void)snprintf(created_hex, sizeof(created_hex), "%s", created);
        zcl_command_reply_free(&reply);
        json_free(&create_in);

        ASSERT(funding_script.size <= 64);
        char funding_script_hex[129];
        zcl_hex_encode(funding_script.data, funding_script.size,
                       funding_script_hex);
        struct json_value sign_in, prevtxs, prevtx;
        json_init(&sign_in); json_set_object(&sign_in);
        (void)json_push_kv_str(&sign_in, "raw_hex", created_hex);
        json_init(&prevtxs); json_set_array(&prevtxs);
        json_init(&prevtx); json_set_object(&prevtx);
        (void)json_push_kv_str(&prevtx, "txid", funding_hex);
        (void)json_push_kv_int(&prevtx, "vout", 0);
        (void)json_push_kv_str(&prevtx, "scriptPubKey",
                               funding_script_hex);
        (void)json_push_kv_real(&prevtx, "amount", 0.01);
        (void)json_push_back(&prevtxs, &prevtx);
        json_free(&prevtx);
        (void)json_push_kv(&sign_in, "prevtxs", &prevtxs);
        json_free(&prevtxs);

        request.spec = sign_spec;
        request.input = &sign_in;
        zcl_command_reply_init(&reply, sign_spec->output_schema);
        zcl_native_handle_wallet_raw_sign(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(json_get_bool(json_get(&reply.data, "complete")));
        const char *signed_raw = json_get_str(
            json_get(&reply.data, "raw_hex"));
        ASSERT(signed_raw && strlen(signed_raw) < 8192);
        char signed_hex[8192];
        (void)snprintf(signed_hex, sizeof(signed_hex), "%s", signed_raw);
        zcl_command_reply_free(&reply);
        json_free(&sign_in);

        struct json_value broadcast_in;
        json_init(&broadcast_in); json_set_object(&broadcast_in);
        (void)json_push_kv_str(&broadcast_in, "raw_hex", signed_hex);
        request.spec = broadcast_spec;
        request.input = &broadcast_in;
        zcl_command_reply_init(&reply, broadcast_spec->output_schema);
        zcl_native_handle_wallet_raw_broadcast(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT_EQ(bridge.broadcast_calls, 0);
        ASSERT(simnet_coin_exists(&sim, &funding_txid));
        zcl_command_reply_free(&reply);

        (void)json_push_kv_bool(&broadcast_in, "confirm", true);
        zcl_command_reply_init(&reply, broadcast_spec->output_schema);
        zcl_native_handle_wallet_raw_broadcast(&request, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "committed");
        ASSERT_EQ(bridge.broadcast_calls, 1);
        ASSERT(bridge.mined);
        ASSERT(!simnet_coin_exists(&sim, &funding_txid));
        int64_t recipient_value = 0;
        ASSERT(simnet_coin_value(&sim, &bridge.txid, 0, &recipient_value));
        ASSERT_EQ(recipient_value, 800000);
        char mined_hex[65];
        uint256_get_hex(&bridge.txid, mined_hex);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "txid")),
                      mined_hex);
        zcl_command_reply_free(&reply);
        json_free(&broadcast_in);
        PASS();
    } _test_next:;

    node_rpc_client_set_test_hook(NULL);
    g_raw_simnet_bridge = NULL;
    rpc_rawtx_set_keystore(NULL);
    rpc_rawtx_set_state(NULL, NULL, NULL, NULL);
    if (ks_ready) keystore_free(ks);
    free(ks);
    memory_cleanse(funding_key.vch, sizeof(funding_key.vch));
    memory_cleanse(recipient_key.vch, sizeof(recipient_key.vch));
    if (sim_ready) simnet_free(&sim);
    return failures;
}

/* ── mutating app.* feature leaves: E2E over a stubbed app RPC ─────────────
 * The promoted write leaves (config/commands/app_features.def) are
 * dedicated handlers in app/controllers/src/app_write_native_handlers.c. Driven
 * here with no live node through the ZCL_TESTING RPC hook, proving three things
 * the catalog test cannot: the plan leg never reaches the RPC, the confirmed
 * leg does exactly once, and a backing RPC that succeeds WITHOUT doing the job
 * (a ZNAM write answering status="ready" because the node carries no wallet)
 * is reported BLOCKED with mutated=false rather than PASSED. */
static int g_app_name_register_calls;
static int g_app_msg_send_calls;
static int g_app_token_send_calls;
static int g_app_market_content_calls;
static int g_app_market_purchase_plan_calls;
static int g_app_market_purchase_commit_calls;
static int g_app_market_purchase_status_calls;
static int g_app_market_purchase_retrieve_calls;
static bool g_app_name_degraded;

static char *app_write_stub_rpc(const char *method, const char *params_json)
{
    (void)params_json;
    if (method && strcmp(method, "name_register") == 0) {
        g_app_name_register_calls++;
        if (g_app_name_degraded)
            return strdup("{\"name\":\"alice\",\"status\":\"ready\","
                          "\"op_return_hex\":\"6a\"}");
        return strdup("{\"name\":\"alice\",\"type\":\"z-address\","
                      "\"txid\":\"aa11bb22cc33dd44ee55ff66aa77bb88"
                      "cc99dd00ee11ff22aa33bb44cc55dd66\","
                      "\"fee\":1000,\"status\":\"broadcast\"}");
    }
    if (method && strcmp(method, "msg_send") == 0) {
        g_app_msg_send_calls++;
        return strdup("{\"msg_id\":\"00112233445566778899aabbccddeeff"
                      "00112233445566778899aabbccddeeff\","
                      "\"peer_id\":7,\"status\":\"sent\"}");
    }
    if (method && strcmp(method, "zslp_send_tx") == 0) {
        g_app_token_send_calls++;
        return strdup("{\"status\":\"broadcast\","
                      "\"txid\":\"11111111111111111111111111111111"
                      "11111111111111111111111111111111\","
                      "\"token_id\":\"22222222222222222222222222222222"
                      "22222222222222222222222222222222\","
                      "\"to\":\"t1stub\",\"amount\":25,\"fee\":10000}");
    }
    if (method && strcmp(method, "zmarket_content_register") == 0) {
        g_app_market_content_calls++;
        return strdup("{\"schema\":\"zcl.market_content.v1\","
                      "\"status\":\"registered\","
                      "\"offer_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
                      "\"root_hash\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
                      "\"size_bytes\":8193,\"num_chunks\":1,"
                      "\"registered_at\":1700000000}");
    }
    if (method && strcmp(method, "zmarket_purchase_plan") == 0) {
        g_app_market_purchase_plan_calls++;
        return strdup("{\"ok\":true,\"schema\":\"zcl.market_purchase.v1\","
                      "\"plan_id\":\"11111111111111111111111111111111"
                      "11111111111111111111111111111111\","
                      "\"offer_id\":\"22222222222222222222222222222222"
                      "22222222222222222222222222222222\","
                      "\"buyer_pubkey\":\"33333333333333333333333333333333"
                      "33333333333333333333333333333333\","
                      "\"wallet_scope\":\"dev\",\"state\":\"planned\","
                      "\"chunk_start\":0,\"chunks_paid\":1,"
                      "\"amount_zat\":60000,\"maximum_fee_zat\":10000,"
                      "\"reserved_zat\":70000,\"expires_at\":1700000600,"
                      "\"idempotent_replay\":false,"
                      "\"payment_notification_queued\":false}");
    }
    if (method && strcmp(method, "zmarket_purchase_commit") == 0) {
        g_app_market_purchase_commit_calls++;
        return strdup("{\"ok\":true,\"schema\":\"zcl.market_purchase.v1\","
                      "\"plan_id\":\"11111111111111111111111111111111"
                      "11111111111111111111111111111111\","
                      "\"offer_id\":\"22222222222222222222222222222222"
                      "22222222222222222222222222222222\","
                      "\"buyer_pubkey\":\"33333333333333333333333333333333"
                      "33333333333333333333333333333333\","
                      "\"wallet_scope\":\"dev\","
                      "\"state\":\"mempool_accepted\","
                      "\"chunk_start\":0,\"chunks_paid\":1,"
                      "\"amount_zat\":60000,\"maximum_fee_zat\":10000,"
                      "\"reserved_zat\":70000,\"expires_at\":1700000600,"
                      "\"idempotent_replay\":false,"
                      "\"txid\":\"44444444444444444444444444444444"
                      "44444444444444444444444444444444\","
                      "\"claim_id\":\"55555555555555555555555555555555"
                      "55555555555555555555555555555555\","
                      "\"payment_notification_queued\":true}");
    }
    if (method && strcmp(method, "zmarket_purchase_status") == 0) {
        g_app_market_purchase_status_calls++;
        return strdup("{\"ok\":true,\"schema\":\"zcl.market_purchase.v1\","
                      "\"plan_id\":\"11111111111111111111111111111111"
                      "11111111111111111111111111111111\","
                      "\"offer_id\":\"22222222222222222222222222222222"
                      "22222222222222222222222222222222\","
                      "\"buyer_pubkey\":\"33333333333333333333333333333333"
                      "33333333333333333333333333333333\","
                      "\"wallet_scope\":\"dev\","
                      "\"state\":\"mempool_accepted\","
                      "\"chunk_start\":0,\"chunks_paid\":1,"
                      "\"amount_zat\":60000,\"maximum_fee_zat\":10000,"
                      "\"reserved_zat\":70000,\"expires_at\":1700000600,"
                      "\"idempotent_replay\":true,"
                      "\"txid\":\"44444444444444444444444444444444"
                      "44444444444444444444444444444444\","
                      "\"claim_id\":\"55555555555555555555555555555555"
                      "55555555555555555555555555555555\","
                      "\"payment_notification_queued\":false}");
    }
    if (method && strcmp(method, "zmarket_purchase_retrieve") == 0) {
        g_app_market_purchase_retrieve_calls++;
        return strdup("{\"ok\":true,\"schema\":\"zcl.market_purchase.v1\","
                      "\"plan_id\":\"11111111111111111111111111111111"
                      "11111111111111111111111111111111\","
                      "\"offer_id\":\"22222222222222222222222222222222"
                      "22222222222222222222222222222222\","
                      "\"buyer_pubkey\":\"33333333333333333333333333333333"
                      "33333333333333333333333333333333\","
                      "\"wallet_scope\":\"dev\",\"state\":\"confirmed\","
                      "\"chunk_start\":0,\"chunks_paid\":1,"
                      "\"amount_zat\":60000,\"maximum_fee_zat\":10000,"
                      "\"reserved_zat\":70000,\"expires_at\":1700000600,"
                      "\"idempotent_replay\":false,"
                      "\"download_state\":\"complete\","
                      "\"chunks_received\":1,\"num_chunks\":1,"
                      "\"bytes_received\":8193,\"size_bytes\":8193,"
                      "\"destination_published\":true}");
    }
    if (method && strcmp(method, "swap_initiate") == 0)
        return strdup("{\"swap_id\":\"stub\",\"role\":\"initiator\","
                      "\"state\":\"pending\",\"p2sh_address\":\"t3Stub\"}");
    return strdup("null");
}

static int test_app_write_native_e2e(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();

    TEST("app.* write leaves execute plan/commit over a stubbed RPC") {
        g_app_name_register_calls = 0;
        g_app_msg_send_calls = 0;
        g_app_token_send_calls = 0;
        g_app_market_content_calls = 0;
        g_app_market_purchase_plan_calls = 0;
        g_app_market_purchase_commit_calls = 0;
        g_app_market_purchase_status_calls = 0;
        g_app_market_purchase_retrieve_calls = 0;
        g_app_name_degraded = false;
        node_rpc_client_set_test_hook(app_write_stub_rpc);

        const struct zcl_command_spec *reg_spec =
            find_spec(reg, "app.names.register");
        ASSERT(reg_spec != NULL);
        ASSERT(reg_spec->availability == ZCL_COMMAND_READY);
        ASSERT(reg_spec->confirmation == ZCL_COMMAND_CONFIRM_PLAN_COMMIT);

        struct zcl_command_reply reply;

        /* 1. plan leg: no RPC fires, nothing claims to have mutated, and the
         *    emitted commit_input validates against this same leaf. */
        struct json_value plan_in;
        json_init(&plan_in);
        json_set_object(&plan_in);
        (void)json_push_kv_str(&plan_in, "name", "alice");
        (void)json_push_kv_str(&plan_in, "type", "zaddr");
        (void)json_push_kv_str(&plan_in, "value", "zs1stub");
        struct zcl_command_request req_plan = {
            .spec = reg_spec, .input = &plan_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, reg_spec->output_schema);
        zcl_native_handle_name_register(&req_plan, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT(!reply.error.mutated);
        ASSERT_EQ(g_app_name_register_calls, 0);
        const char *commit_input =
            json_get_str(json_get(&reply.data, "commit_input"));
        ASSERT(commit_input && commit_input[0]);
        struct json_value commit_next;
        json_init(&commit_next);
        ASSERT(json_read(&commit_next, commit_input, strlen(commit_input)));
        char why[160] = {0};
        ASSERT(zcl_command_registry_input_validate(reg_spec, &commit_next, why,
                                                   sizeof(why)));
        ASSERT(json_get_bool(json_get(&commit_next, "confirm")));
        /* The plan must actually serialize through the real envelope. */
        {
            char rendered[8192];
            enum zcl_command_exit rc = ZCL_COMMAND_EXIT_OK;
            struct zcl_command_context sctx = {
                .registry = reg,
                .granted_capabilities = ~(uint64_t)0,
                .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
            };
            size_t n = zcl_command_registry_execute_json(
                reg, reg_spec, &sctx, &plan_in, false, reg_spec->path,
                "normal", 0, 0, NULL, rendered, sizeof(rendered), &rc);
            ASSERT(n > 0);
            ASSERT_EQ(rc, ZCL_COMMAND_EXIT_OK);
            ASSERT(strstr(rendered, "\"stage\":\"plan\"") != NULL);
        }
        zcl_command_reply_free(&reply);
        json_free(&plan_in);

        /* 2. commit leg: exactly one RPC, the broadcast txid surfaces. */
        struct zcl_command_request req_commit = {
            .spec = reg_spec, .input = &commit_next, .view = "normal",
        };
        zcl_command_reply_init(&reply, reg_spec->output_schema);
        zcl_native_handle_name_register(&req_commit, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "committed");
        ASSERT(reply.error.mutated);
        ASSERT(json_get_str(json_get(&reply.data, "txid")) != NULL);
        ASSERT_EQ(g_app_name_register_calls, 1);
        zcl_command_reply_free(&reply);

        /* 3. the DEGRADED success: name_register answered without a wallet, so
         *    the OP_RETURN was built but never broadcast. A PASSED reply here
         *    would be the exact lie this leaf population was cleaned of. */
        g_app_name_degraded = true;
        zcl_command_reply_init(&reply, reg_spec->output_schema);
        zcl_native_handle_name_register(&req_commit, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_BLOCKED);
        ASSERT_STR_EQ(reply.error.code, "NAME_WRITE_NOT_BROADCAST");
        ASSERT(!reply.error.mutated);
        ASSERT(strstr(reply.error.message, "ready") != NULL);
        ASSERT_EQ(g_app_name_register_calls, 2);
        zcl_command_reply_free(&reply);
        g_app_name_degraded = false;
        json_free(&commit_next);

        /* 4. a missing required key fails closed before any RPC. */
        struct json_value bad_in;
        json_init(&bad_in);
        json_set_object(&bad_in);
        (void)json_push_kv_str(&bad_in, "name", "alice");
        (void)json_push_kv_bool(&bad_in, "confirm", true);
        struct zcl_command_request req_bad = {
            .spec = reg_spec, .input = &bad_in, .view = "normal",
        };
        zcl_command_reply_init(&reply, reg_spec->output_schema);
        zcl_native_handle_name_register(&req_bad, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT_STR_EQ(reply.error.code, "MISSING_INPUT");
        ASSERT(!reply.error.mutated);
        ASSERT_EQ(g_app_name_register_calls, 2);
        zcl_command_reply_free(&reply);
        json_free(&bad_in);

        /* 5. token.send has the same two-leg transaction contract: plan is
         * non-mutating; commit calls the receipt-bearing RPC exactly once. */
        const struct zcl_command_spec *token_spec =
            find_spec(reg, "app.tokens.send");
        ASSERT(token_spec != NULL);
        ASSERT(token_spec->availability == ZCL_COMMAND_READY);
        struct json_value token_plan;
        json_init(&token_plan);
        json_set_object(&token_plan);
        (void)json_push_kv_str(
            &token_plan, "token_id",
            "2222222222222222222222222222222222222222222222222222222222222222");
        (void)json_push_kv_str(&token_plan, "to", "t1stub");
        (void)json_push_kv_str(&token_plan, "units", "25");
        struct zcl_command_request token_plan_req = {
            .spec = token_spec, .input = &token_plan, .view = "normal",
        };
        zcl_command_reply_init(&reply, token_spec->output_schema);
        zcl_native_handle_token_send(&token_plan_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT(!reply.error.mutated);
        ASSERT_EQ(g_app_token_send_calls, 0);
        zcl_command_reply_free(&reply);
        (void)json_push_kv_bool(&token_plan, "confirm", true);
        zcl_command_reply_init(&reply, token_spec->output_schema);
        zcl_native_handle_token_send(&token_plan_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_token_send_calls, 1);
        ASSERT(json_get_str(json_get(&reply.data, "txid")) != NULL);
        zcl_command_reply_free(&reply);
        json_free(&token_plan);

        /* 6. Private seller content is an idempotent no-funds app write. It
         * executes once without a plan round trip and never echoes the path. */
        const struct zcl_command_spec *content_spec =
            find_spec(reg, "app.market.content.register");
        ASSERT(content_spec != NULL);
        ASSERT_EQ(content_spec->availability, ZCL_COMMAND_READY);
        ASSERT_EQ(content_spec->confirmation, ZCL_COMMAND_CONFIRM_NONE);
        struct json_value content_input;
        json_init(&content_input);
        json_set_object(&content_input);
        (void)json_push_kv_str(
            &content_input, "offer_id",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        (void)json_push_kv_str(&content_input, "content_path",
                               "/owner/private/paid-content.bin");
        struct zcl_command_request content_req = {
            .spec = content_spec, .input = &content_input, .view = "normal",
        };
        zcl_command_reply_init(&reply, content_spec->output_schema);
        zcl_native_handle_market_content_register(&content_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_market_content_calls, 1);
        char content_rendered[4096];
        size_t content_len = json_write(&reply.data, content_rendered,
                                        sizeof(content_rendered));
        ASSERT(content_len > 0);
        ASSERT(strstr(content_rendered, "registered") != NULL);
        ASSERT(strstr(content_rendered, "/owner/private") == NULL);
        ASSERT(strstr(content_rendered, "content_path") == NULL);
        zcl_command_reply_free(&reply);
        json_free(&content_input);

        /* 7. Buyer payment is three explicit typed operations. Plan reserves
         * value+fee and emits a path/address-free commit input; commit calls
         * the value-moving RPC once; status is read-only. */
        const struct zcl_command_spec *purchase_plan_spec =
            find_spec(reg, "app.market.purchase.plan");
        const struct zcl_command_spec *purchase_commit_spec =
            find_spec(reg, "app.market.purchase.commit");
        const struct zcl_command_spec *purchase_status_spec =
            find_spec(reg, "app.market.purchase.status");
        ASSERT(purchase_plan_spec && purchase_commit_spec &&
               purchase_status_spec);
        struct json_value purchase_input;
        json_init(&purchase_input);
        json_set_object(&purchase_input);
        (void)json_push_kv_str(&purchase_input, "wallet_scope", "dev");
        (void)json_push_kv_str(
            &purchase_input, "offer_id",
            "2222222222222222222222222222222222222222222222222222222222222222");
        (void)json_push_kv_str(&purchase_input, "source_address",
                               "zs1owner-private-source");
        (void)json_push_kv_int(&purchase_input, "chunk_start", 0);
        (void)json_push_kv_int(&purchase_input, "chunks_paid", 1);
        (void)json_push_kv_str(&purchase_input, "idempotency_key",
                               "native-contract-1");
        struct zcl_command_request purchase_plan_req = {
            .spec = purchase_plan_spec, .input = &purchase_input,
            .view = "normal",
        };
        zcl_command_reply_init(&reply, purchase_plan_spec->output_schema);
        zcl_native_handle_market_purchase_plan(&purchase_plan_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_market_purchase_plan_calls, 1);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        const char *purchase_commit_input =
            json_get_str(json_get(&reply.data, "commit_input"));
        ASSERT(purchase_commit_input && purchase_commit_input[0]);
        ASSERT(strstr(purchase_commit_input, "source_address") == NULL);
        ASSERT(strstr(purchase_commit_input, "private-source") == NULL);
        char purchase_rendered[4096];
        size_t purchase_len = json_write(&reply.data, purchase_rendered,
                                         sizeof(purchase_rendered));
        ASSERT(purchase_len > 0);
        ASSERT(strstr(purchase_rendered, "source_address") == NULL);
        ASSERT(strstr(purchase_rendered, "seller") == NULL);
        ASSERT(strstr(purchase_rendered, "memo") == NULL);
        struct json_value purchase_commit_input_json;
        json_init(&purchase_commit_input_json);
        ASSERT(json_read(&purchase_commit_input_json, purchase_commit_input,
                         strlen(purchase_commit_input)));
        char purchase_why[160] = {0};
        ASSERT(zcl_command_registry_input_validate(
            purchase_commit_spec, &purchase_commit_input_json, purchase_why,
            sizeof(purchase_why)));
        zcl_command_reply_free(&reply);
        json_free(&purchase_input);

        struct zcl_command_request purchase_commit_req = {
            .spec = purchase_commit_spec,
            .input = &purchase_commit_input_json, .view = "normal",
        };
        zcl_command_reply_init(&reply, purchase_commit_spec->output_schema);
        zcl_native_handle_market_purchase_commit(&purchase_commit_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_market_purchase_commit_calls, 1);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "committed");
        ASSERT(json_get_str(json_get(&reply.data, "txid")) != NULL);
        ASSERT(json_get_str(json_get(&reply.data, "claim_id")) != NULL);
        zcl_command_reply_free(&reply);

        struct json_value purchase_status_input;
        json_init(&purchase_status_input);
        json_set_object(&purchase_status_input);
        (void)json_push_kv_str(
            &purchase_status_input, "plan_id",
            json_get_str(json_get(&purchase_commit_input_json, "plan_id")));
        struct zcl_command_request purchase_status_req = {
            .spec = purchase_status_spec, .input = &purchase_status_input,
            .view = "normal",
        };
        zcl_command_reply_init(&reply, purchase_status_spec->output_schema);
        zcl_native_handle_market_purchase_status(&purchase_status_req, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(!reply.error.mutated);
        ASSERT_EQ(g_app_market_purchase_status_calls, 1);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "status");
        zcl_command_reply_free(&reply);
        json_free(&purchase_status_input);

        const struct zcl_command_spec *purchase_retrieve_spec =
            find_spec(reg, "app.market.purchase.retrieve");
        ASSERT(purchase_retrieve_spec != NULL);
        struct json_value purchase_retrieve_input;
        json_init(&purchase_retrieve_input);
        json_set_object(&purchase_retrieve_input);
        (void)json_push_kv_str(
            &purchase_retrieve_input, "plan_id",
            json_get_str(json_get(&purchase_commit_input_json, "plan_id")));
        (void)json_push_kv_str(&purchase_retrieve_input, "destination_path",
                               "/owner/private/purchased.bin");
        struct zcl_command_request purchase_retrieve_req = {
            .spec = purchase_retrieve_spec,
            .input = &purchase_retrieve_input, .view = "normal",
        };
        zcl_command_reply_init(&reply, purchase_retrieve_spec->output_schema);
        zcl_native_handle_market_purchase_retrieve(&purchase_retrieve_req,
                                                   &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_market_purchase_retrieve_calls, 1);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")),
                      "retrieved");
        char retrieved_rendered[4096];
        size_t retrieved_len = json_write(&reply.data, retrieved_rendered,
                                           sizeof(retrieved_rendered));
        ASSERT(retrieved_len > 0);
        ASSERT(strstr(retrieved_rendered, "destination_path") == NULL);
        ASSERT(strstr(retrieved_rendered, "/owner/private") == NULL);
        ASSERT(strstr(retrieved_rendered, "endpoint") == NULL);
        ASSERT(strstr(retrieved_rendered, "buyer_seed") == NULL);
        zcl_command_reply_free(&reply);
        json_free(&purchase_retrieve_input);
        json_free(&purchase_commit_input_json);

        /* 8. messaging.send picks the recipient key its channel names: the p2p
         *    channel demands peer_id and refuses before touching the node. */
        const struct zcl_command_spec *send_spec =
            find_spec(reg, "app.messaging.send");
        ASSERT(send_spec != NULL);
        ASSERT(send_spec->availability == ZCL_COMMAND_READY);
        struct json_value nopeer;
        json_init(&nopeer);
        json_set_object(&nopeer);
        (void)json_push_kv_str(&nopeer, "message", "hi");
        (void)json_push_kv_bool(&nopeer, "confirm", true);
        struct zcl_command_request req_nopeer = {
            .spec = send_spec, .input = &nopeer, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_message_send(&req_nopeer, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_INVALID);
        ASSERT_STR_EQ(reply.error.code, "MISSING_INPUT");
        ASSERT_EQ(g_app_msg_send_calls, 0);
        zcl_command_reply_free(&reply);
        json_free(&nopeer);

        struct json_value p2p;
        json_init(&p2p);
        json_set_object(&p2p);
        (void)json_push_kv_str(&p2p, "message", "hi");
        (void)json_push_kv_int(&p2p, "peer_id", 7);
        (void)json_push_kv_bool(&p2p, "confirm", true);
        struct zcl_command_request req_p2p = {
            .spec = send_spec, .input = &p2p, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_message_send(&req_p2p, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT(reply.error.mutated);
        ASSERT_EQ(g_app_msg_send_calls, 1);
        ASSERT(json_get_str(json_get(&reply.data, "msg_id")) != NULL);
        zcl_command_reply_free(&reply);
        /* The same input WITHOUT confirm must plan, not send again. */
        struct json_value p2p_plan;
        json_init(&p2p_plan);
        json_copy(&p2p_plan, &p2p);
        json_free(&p2p);
        json_init(&p2p);
        json_set_object(&p2p);
        (void)json_push_kv_str(&p2p, "message", "hi");
        (void)json_push_kv_int(&p2p, "peer_id", 7);
        struct zcl_command_request req_p2p_plan = {
            .spec = send_spec, .input = &p2p, .view = "normal",
        };
        zcl_command_reply_init(&reply, send_spec->output_schema);
        zcl_native_handle_message_send(&req_p2p_plan, &reply);
        ASSERT_EQ(reply.exit_code, ZCL_COMMAND_EXIT_OK);
        ASSERT_STR_EQ(json_get_str(json_get(&reply.data, "stage")), "plan");
        ASSERT(!json_get_bool(json_get(&reply.data, "spends_funds")));
        ASSERT_EQ(g_app_msg_send_calls, 1);
        zcl_command_reply_free(&reply);
        json_free(&p2p);
        json_free(&p2p_plan);

        node_rpc_client_set_test_hook(NULL);
        PASS();
    } _test_next:;
    node_rpc_client_set_test_hook(NULL);
    return failures;
}

/* ── a node that answers only PART of a reply must read as a slow node ─────
 * The node writes HTTP response headers before its handler blocks, so a
 * deadline that fires while the handler waits on the node.db write lock left
 * node_rpc_call returning the header fragment. Every caller parses that
 * return value, so `core.wallet.utxo.list` reported TOOL_ERROR "RPC
 * listunspent returned an unparseable body" — a body-shape complaint for what
 * is really a busy node. rpc_client.c now names the truncation instead.
 *
 * Driven through zcl_command_registry_execute_json against a REAL socket (no
 * test hook) and asserted on the RENDERED BYTES the caller receives, because
 * that is the only layer where the wrong error text is visible. */
struct partial_reply_server {
    int listen_fd;
    int accepted_fd;
};

static void *partial_reply_serve(void *arg)
{
    struct partial_reply_server *s = arg;
    /* Send the response HEADERS and nothing else, then hold the connection
     * open — exactly the shape a node produces when its handler blocks after
     * the HTTP preamble is already on the wire. */
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0)
        return NULL;
    s->accepted_fd = fd;
    static const char hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: 64\r\nConnection: close\r\n\r\n";
    (void)!write(fd, hdr, sizeof(hdr) - 1);
    /* Park until the parent closes us out; never write the body. */
    for (int i = 0; i < 200; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        (void)nanosleep(&ts, NULL);
        if (s->listen_fd < 0)
            break;
    }
    return NULL;
}

static int test_partial_rpc_reply_names_the_timeout(void)
{
    int failures = 0;
    const struct zcl_command_registry *reg = zcl_command_catalog();
    char *dir = NULL;
    pthread_t th = 0;
    bool joined = false;
    struct partial_reply_server srv = { .listen_fd = -1, .accepted_fd = -1 };
    char *out = malloc(ZCL_COMMAND_LIST_BUDGET + 1);

    TEST("core.wallet.utxo.list: a node that sends only the HTTP headers "
        "before the deadline renders a named transport timeout, never "
        "'unparseable body'") {
        ASSERT(out != NULL);
        node_rpc_client_set_test_hook(NULL);

        srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT(srv.listen_fd >= 0);
        struct sockaddr_in addr = { 0 };
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        int reuse = 1;
        setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse));
        ASSERT(bind(srv.listen_fd, (struct sockaddr *)&addr,
                    sizeof(addr)) == 0);
        socklen_t alen = sizeof(addr);
        ASSERT(getsockname(srv.listen_fd, (struct sockaddr *)&addr,
                           &alen) == 0);
        uint16_t port = ntohs(addr.sin_port);
        ASSERT(listen(srv.listen_fd, 1) == 0);
        ASSERT(pthread_create(&th, NULL, partial_reply_serve, &srv) == 0);

        char dir_template[] = "/tmp/zcl-partial-reply-XXXXXX";
        dir = strdup(mkdtemp(dir_template));
        ASSERT(dir != NULL);
        char cookie_path[320];
        (void)snprintf(cookie_path, sizeof(cookie_path), "%s/.cookie", dir);
        FILE *cf = fopen(cookie_path, "w");
        ASSERT(cf != NULL);
        (void)fprintf(cf, "dummyuser:dummypass\n");
        (void)fclose(cf);
        node_rpc_client_init(dir, (int)port);
        ASSERT(setenv("ZCL_RPC_DEADLINE_MS", "400", 1) == 0);

        const struct zcl_command_spec *s =
            find_spec(reg, "core.wallet.utxo.list");
        ASSERT(s != NULL);
        enum zcl_command_exit code = ZCL_COMMAND_EXIT_OK;
        bool dispatched = exec_leaf(reg, s, out,
                                    ZCL_COMMAND_LIST_BUDGET + 1, &code);
        (void)unsetenv("ZCL_RPC_DEADLINE_MS");
        ASSERT(dispatched);

        /* The rendered bytes are the contract: a busy node is named as one. */
        ASSERT(strstr(out, "unparseable body") == NULL);
        ASSERT(strstr(out, "truncated reply") != NULL);
        ASSERT(strstr(out, "\"ok\":false") != NULL);
        PASS();
    } _test_next:;
    if (srv.listen_fd >= 0) {
        int lf = srv.listen_fd;
        srv.listen_fd = -1;
        if (th) {
            (void)pthread_join(th, NULL);
            joined = true;
        }
        if (srv.accepted_fd >= 0)
            close(srv.accepted_fd);
        close(lf);
    }
    if (th && !joined)
        (void)pthread_join(th, NULL);
    if (dir) {
        char rmcmd[512];
        (void)snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", dir);
        (void)system(rmcmd);
        free(dir);
    }
    free(out);
    (void)unsetenv("ZCL_RPC_DEADLINE_MS");
    node_rpc_client_init("", 0);
    node_rpc_client_set_test_hook(NULL);
    return failures;
}

int test_native_api_contract(void)
{
    int failures = 0;
    failures += test_every_branch_menu_lists_only_own_children();
    failures += test_every_leaf_dot_path_resolves_from_cli_words();
    failures += test_root_and_discover_aliases_resolve();
    failures += test_missing_required_input_fails_closed_structured();
    failures += test_dev_failure_native_api();
    failures += test_native_app_catalog_uses_strict_builtin_source();
    failures += test_wallet_mutating_native_e2e();
    failures += test_raw_native_pipeline_mines_exact_signed_bytes();
    failures += test_app_write_native_e2e();
    failures += test_native_bridge_resident_binding();
    failures += test_status_frontdoor_preserves_rpc_error();
    failures += test_status_brief_body_schema_skew_tolerance();
    failures += test_status_brief_body_front_door_deadline();
    failures += test_partial_rpc_reply_names_the_timeout();
    printf("=== native_api_contract: %d failures ===\n", failures);
    return failures;
}
