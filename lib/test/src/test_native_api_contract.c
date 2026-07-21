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

#include "test/test_helpers.h"

#include "config/command_catalog.h"
#include "dev_failure_store.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "controllers/rpc_client.h"
#include "controllers/status_native_handlers.h"
#include "json/json.h"
#include "platform/time_compat.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        ASSERT(apps && apps->type == JSON_ARR && apps->num_children == 2);
        ASSERT_STR_EQ(json_get_str(&apps->children[0]), "blog");
        ASSERT_STR_EQ(json_get_str(&apps->children[1]), "social");
        ASSERT_EQ(json_get_int(json_get(&reply.data, "count")), 2);
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
 *     strictly (an older node's v1, a newer node's v3) degrades to a
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

int test_native_api_contract(void)
{
    int failures = 0;
    failures += test_every_branch_menu_lists_only_own_children();
    failures += test_every_leaf_dot_path_resolves_from_cli_words();
    failures += test_root_and_discover_aliases_resolve();
    failures += test_missing_required_input_fails_closed_structured();
    failures += test_dev_failure_native_api();
    failures += test_native_app_catalog_uses_strict_builtin_source();
    failures += test_status_brief_body_schema_skew_tolerance();
    failures += test_status_brief_body_front_door_deadline();
    printf("=== native_api_contract: %d failures ===\n", failures);
    return failures;
}
