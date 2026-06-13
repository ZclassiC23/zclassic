/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_canary_sentinel_watch — hermetic tests for the in-node replay-canary
 * sentinel watcher + its replay_canary_failed Condition.
 *
 * Every block uses a private tmp dir exported via ZCL_CANARY_VERDICT_DIR
 * (the same env the canary harness reads), so the operator's real verdict
 * dir, the live node, and $HOME are never touched. Asserts the five
 * contracts from the build spec:
 *   (a) FAIL sentinel  → condition raised (and pages),
 *   (b) PASS overwrite → condition cleared,
 *   (c) corrupt JSON   → no raise, no crash, logged once per mtime,
 *   (d) absent dir     → quiet no-op,
 *   (e) idempotency    → two ticks on the same FAIL = ONE raise/remedy.
 * Plus the documented absence policy: a FAIL stays latched when its
 * sentinel disappears (a re-running canary must not un-page the node). */

#include "test/test_helpers.h"

#include "framework/condition.h"
#include "json/json.h"
#include "services/canary_sentinel_watch.h"
#include "conditions/replay_canary_failed.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CSW_CHECK(name, expr) do { \
    printf("canary_sentinel_watch: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static bool write_file(const char *dir, const char *name, const char *body)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
        (int)sizeof(path))
        return false;
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    bool ok = fputs(body, f) >= 0;
    fclose(f);
    return ok;
}

/* Mirror the harness's sentinel shape (extra fields prove tolerance). */
static bool write_sentinel(const char *dir, const char *kind,
                           const char *verdict, const char *reason,
                           long long ts)
{
    char name[128];
    char body[512];
    snprintf(name, sizeof(name), "replay_canary_%s.json", kind);
    snprintf(body, sizeof(body),
             "{\"verdict\":\"%s\",\"from\":\"%s\",\"ts\":%lld,"
             "\"started_ts\":%lld,\"build_commit\":\"deadbee\","
             "\"tip\":3145000,\"verified_height\":3145000,"
             "\"bg_state\":\"complete\",\"consensus_rejects\":0,"
             "\"reason\":\"%s\",\"elapsed_sec\":1234}\n",
             verdict, kind, ts, ts - 1200, reason);
    return write_file(dir, name, body);
}

static void rm_in_dir(const char *dir, const char *name)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) <
        (int)sizeof(path))
        unlink(path);
}

static void watch_test_setup(const char *dir)
{
    condition_engine_reset_for_testing();
    canary_sentinel_watch_test_reset();
    replay_canary_failed_test_reset();
    register_replay_canary_failed();
    setenv("ZCL_CANARY_VERDICT_DIR", dir, 1);
}

static void watch_test_teardown(const char *dir)
{
    rm_in_dir(dir, "replay_canary_anchor.json");
    rm_in_dir(dir, "replay_canary_genesis.json");
    rm_in_dir(dir, "replay_canary_anchor.json.tmp.999");
    unsetenv("ZCL_CANARY_VERDICT_DIR");
    canary_sentinel_watch_test_reset();
    replay_canary_failed_test_reset();
    condition_engine_reset_for_testing();
}

static bool cond_snapshot(struct condition_runtime_snapshot *out)
{
    return condition_engine_get_registered_snapshot("replay_canary_failed",
                                                    out);
}

static int64_t dump_int(const char *field)
{
    struct json_value v;
    json_init(&v);
    int64_t val = -1;
    if (canary_watch_dump_state_json(&v, NULL))
        val = json_get_int(json_get(&v, field));
    json_free(&v);
    return val;
}

static bool dump_bool(const char *field)
{
    struct json_value v;
    json_init(&v);
    bool val = false;
    if (canary_watch_dump_state_json(&v, NULL))
        val = json_get_bool(json_get(&v, field));
    json_free(&v);
    return val;
}

int test_canary_sentinel_watch(void)
{
    printf("\n=== canary_sentinel_watch tests ===\n");
    int failures = 0;

    char tmpl[] = "/tmp/zcl_canary_watch_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        printf("canary_sentinel_watch: mkdtemp FAILED — cannot run\n");
        return 1;
    }

    /* (d) absent dir → quiet no-op, nothing raised, no crash. */
    {
        char absent[PATH_MAX];
        snprintf(absent, sizeof(absent), "%s/never_created", dir);
        watch_test_setup(absent);
        canary_sentinel_watch_tick_once();
        canary_sentinel_watch_tick_once();
        condition_engine_tick();
        bool ok = !canary_sentinel_watch_fail_active();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && replay_canary_failed_test_remedy_calls() == 0;
        ok = ok && dump_int("files_seen_last") == 0;
        CSW_CHECK("absent dir is a quiet no-op", ok);
        watch_test_teardown(absent);
    }

    /* (a) FAIL sentinel → condition raised, remedy logged, page emitted. */
    {
        watch_test_setup(dir);
        bool ok = write_sentinel(dir, "anchor", "FAIL", "sha3_mismatch",
                                 1718000000LL);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();

        char detail[256];
        int fails = canary_sentinel_watch_fail_detail(detail, sizeof(detail));
        ok = ok && fails == 1;
        ok = ok && strstr(detail, "kind=anchor") != NULL;
        ok = ok && strstr(detail, "reason=sha3_mismatch") != NULL;
        ok = ok && strstr(detail, "ts=1718000000") != NULL;

        condition_engine_tick();
        struct condition_runtime_snapshot snap;
        ok = ok && cond_snapshot(&snap);
        ok = ok && snap.currently_active;
        ok = ok && snap.attempts == 1;
        ok = ok && snap.operator_needed_emitted;
        ok = ok && replay_canary_failed_test_remedy_calls() == 1;
        ok = ok && dump_bool("condition_active");
        CSW_CHECK("FAIL sentinel raises replay_canary_failed and pages", ok);
        watch_test_teardown(dir);
    }

    /* (e) idempotency: re-scanning the SAME FAIL must not re-raise — one
     * detect edge, one remedy, no event spam. */
    {
        watch_test_setup(dir);
        bool ok = write_sentinel(dir, "anchor", "FAIL", "crossnode_height",
                                 1718000001LL);
        canary_sentinel_watch_tick_once();
        condition_engine_tick();
        canary_sentinel_watch_tick_once();   /* tick 2: same FAIL file */
        condition_engine_tick();
        struct condition_runtime_snapshot snap;
        ok = ok && cond_snapshot(&snap);
        ok = ok && snap.currently_active;
        ok = ok && snap.attempts == 1;       /* still ONE remedy attempt */
        ok = ok && snap.cleared_count == 0;
        ok = ok && replay_canary_failed_test_remedy_calls() == 1;
        CSW_CHECK("two ticks on the same FAIL = one raise", ok);
        watch_test_teardown(dir);
    }

    /* (b) PASS overwrite (the harness's atomic replace) → cleared. */
    {
        watch_test_setup(dir);
        bool ok = write_sentinel(dir, "anchor", "FAIL", "budget_exceeded",
                                 1718000002LL);
        canary_sentinel_watch_tick_once();
        condition_engine_tick();
        struct condition_runtime_snapshot snap;
        ok = ok && cond_snapshot(&snap) && snap.currently_active;

        ok = ok && write_sentinel(dir, "anchor", "PASS", "", 1718000060LL);
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        condition_engine_tick();
        ok = ok && cond_snapshot(&snap);
        ok = ok && !snap.currently_active;
        ok = ok && snap.cleared_count == 1;
        ok = ok && condition_engine_get_active_count() == 0;
        CSW_CHECK("PASS overwrite clears the condition", ok);
        watch_test_teardown(dir);
    }

    /* (c) corrupt JSON → no raise, no crash; logged once per mtime; an
     * in-flight .tmp. file is never read as a verdict. */
    {
        watch_test_setup(dir);
        bool ok = write_file(dir, "replay_canary_anchor.json",
                             "{\"verdict\":\"FA");  /* torn write */
        ok = ok && write_file(dir, "replay_canary_anchor.json.tmp.999",
                              "{\"verdict\":\"FAIL\",\"from\":\"anchor\","
                              "\"ts\":1,\"reason\":\"x\"}\n");
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        ok = ok && dump_int("parse_failures_logged") == 1;
        canary_sentinel_watch_tick_once();   /* same mtime: no second log */
        ok = ok && dump_int("parse_failures_logged") == 1;
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && replay_canary_failed_test_remedy_calls() == 0;
        CSW_CHECK("corrupt JSON never raises and logs once per mtime", ok);
        watch_test_teardown(dir);
    }

    /* Documented absence policy: a latched FAIL survives its sentinel
     * disappearing (canary re-run deletes it first) — only PASS clears. */
    {
        watch_test_setup(dir);
        bool ok = write_sentinel(dir, "genesis", "FAIL", "rpc_never_ready",
                                 1718000003LL);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();
        rm_in_dir(dir, "replay_canary_genesis.json");
        canary_sentinel_watch_tick_once();   /* file gone: still latched */
        ok = ok && canary_sentinel_watch_fail_active();
        ok = ok && write_sentinel(dir, "genesis", "PASS", "",
                                  1718000400LL);
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        CSW_CHECK("FAIL stays latched across sentinel absence until PASS",
                  ok);
        watch_test_teardown(dir);
    }

    rmdir(dir);
    return failures;
}
