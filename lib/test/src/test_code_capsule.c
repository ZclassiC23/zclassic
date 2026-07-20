/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Code capsule / navigator contract (codeindex/codeindex.h,
 * config/command_handler_index.h). Step-0 contract test: the new call-graph
 * surface exists, links, and fails closed on bad args; the ci_ref record
 * carries the `enclosing` column; the handler index is non-NULL. WF4 lanes
 * 4A/4D land the golden capsule + real callers/callees/dispatch joins. */

#include "test/test_helpers.h"
#include "codeindex/codeindex.h"
#include "config/command_handler_index.h"
#include <stddef.h>
#include <string.h>

static int test_code_capsule_ci_ref_has_enclosing(void)
{
    int failures = 0;
    TEST("code_capsule: ci_ref carries an enclosing column") {
        struct ci_ref r;
        memset(&r, 0, sizeof(r));
        /* The field exists and is a fixed buffer we can write. */
        ASSERT(sizeof(r.enclosing) == 128);
        strncpy(r.enclosing, "sovereignty_guard_allow", sizeof(r.enclosing) - 1);
        ASSERT(strcmp(r.enclosing, "sovereignty_guard_allow") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int test_code_capsule_queries_fail_closed(void)
{
    int failures = 0;
    TEST("code_capsule: call-graph stubs fail closed on bad args") {
        struct ci_ref out[4];
        char buf[64];
        /* NULL index → documented error sentinels, never a crash. */
        ASSERT(codeindex_callers(NULL, "x", out, 4) == -1);
        ASSERT(codeindex_callees(NULL, "x", out, 4) == -1);
        ASSERT(codeindex_symbol_id(NULL, "x", buf, sizeof(buf)) == -1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_code_capsule_handler_index_present(void)
{
    int failures = 0;
    TEST("code_capsule: command handler index accessor is non-NULL") {
        const struct zcl_command_handler_index *ix = zcl_command_handler_index();
        ASSERT(ix != NULL);
        /* Step-0: table is empty until lane 4C's stringizing expansion lands. */
        ASSERT(ix->count == 0);
        ASSERT(ix->entries != NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_code_capsule(void)
{
    int failures = 0;
    failures += test_code_capsule_ci_ref_has_enclosing();
    failures += test_code_capsule_queries_fail_closed();
    failures += test_code_capsule_handler_index_present();
    return failures;
}
