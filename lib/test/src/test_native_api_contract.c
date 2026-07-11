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
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "json/json.h"

#include <stdio.h>
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
            { "agent", "status" },
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

int test_native_api_contract(void)
{
    int failures = 0;
    failures += test_every_branch_menu_lists_only_own_children();
    failures += test_every_leaf_dot_path_resolves_from_cli_words();
    failures += test_root_and_discover_aliases_resolve();
    failures += test_missing_required_input_fails_closed_structured();
    printf("=== native_api_contract: %d failures ===\n", failures);
    return failures;
}
