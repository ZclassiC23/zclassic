/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_canary_sentinel_watch — hermetic tests for the in-node replay-canary
 * sentinel watcher + its replay_canary_failed Condition.
 *
 * Every block uses a private tmp dir exported via ZCL_CANARY_VERDICT_DIR
 * (the same env the canary harness reads), so the operator's real verdict
 * dir, the live node, and $HOME are never touched. Asserts the six
 * contracts from the build spec:
 *   (a) FAIL sentinel  → condition raised (and pages),
 *   (b) PASS overwrite → condition cleared,
 *   (c) corrupt JSON   → no raise, no crash, logged once per mtime,
 *   (d) absent dir     → quiet no-op,
 *   (e) idempotency    → two ticks on the same FAIL = ONE raise/remedy,
 *   (f) mixed kinds    → anchor=FAIL + genesis=PASS pages naming only the
 *                        failing kind; all-green clears.
 *   (g) cross-build    → a FAIL written by a DIFFERENT build than the one
 *                        running is ignored (shared dir + fresh deploy); a
 *                        same-build FAIL still pages.
 *   (h) -dirty norm    → a bare-hash FAIL from the same SOURCE still pages even
 *                        when the binary is a dirty build (dev-lane default).
 *   (i) pre-start run  → a FAIL from a run started before this process is
 *                        ignored; a fresh same-build FAIL still pages.
 * Plus the documented absence policy: a FAIL stays latched when its
 * sentinel disappears (a re-running canary must not un-page the node). */

#include "test/test_helpers.h"

#include "framework/condition.h"
#include "json/json.h"
#include "services/canary_sentinel_watch.h"
#include "conditions/replay_canary_failed.h"
#include "util/clientversion.h"

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

/* Mirror the harness's sentinel shape (extra fields prove tolerance), with an
 * explicit build_commit so a test can exercise the cross-build staleness drop
 * (a FAIL written by a DIFFERENT build than the one running must not page). */
static bool write_sentinel_commit(const char *dir, const char *kind,
                                  const char *verdict, const char *reason,
                                  long long ts, const char *build_commit)
{
    char name[128];
    char body[512];
    snprintf(name, sizeof(name), "replay_canary_%s.json", kind);
    snprintf(body, sizeof(body),
             "{\"verdict\":\"%s\",\"from\":\"%s\",\"ts\":%lld,"
             "\"started_ts\":%lld,\"build_commit\":\"%s\","
             "\"tip\":3145000,\"verified_height\":3145000,"
             "\"bg_state\":\"complete\",\"consensus_rejects\":0,"
             "\"reason\":\"%s\",\"elapsed_sec\":1234}\n",
             verdict, kind, ts, ts - 1200,
             build_commit ? build_commit : "", reason);
    return write_file(dir, name, body);
}

/* The common case: a sentinel written by the RUNNING build, so a FAIL latches
 * (same-build verdicts are genuine evidence about this binary). */
static bool write_sentinel(const char *dir, const char *kind,
                           const char *verdict, const char *reason,
                           long long ts)
{
    return write_sentinel_commit(dir, kind, verdict, reason, ts,
                                 zcl_build_commit());
}

/* Strip a trailing "-dirty" — the canary harness writes the bare short hash
 * while a dirty-built binary's zcl_build_commit() carries the suffix. */
static void strip_dirty(const char *in, char *out, size_t cap)
{
    snprintf(out, cap, "%s", in ? in : "");
    char *d = strstr(out, "-dirty");
    if (d)
        *d = '\0';
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
    canary_sentinel_watch_test_set_process_start(1717000000LL);
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

    /* (f) mixed verdicts across kinds: anchor=FAIL + genesis=PASS must keep
     * the condition raised with detail naming ONLY the failing kind; a later
     * anchor PASS (all kinds green) clears it. */
    {
        watch_test_setup(dir);
        bool ok = write_sentinel(dir, "anchor", "FAIL", "sha3_mismatch",
                                 1718000100LL);
        ok = ok && write_sentinel(dir, "genesis", "PASS", "", 1718000101LL);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();

        char detail[256];
        int fails = canary_sentinel_watch_fail_detail(detail, sizeof(detail));
        ok = ok && fails == 1;
        ok = ok && strstr(detail, "kind=anchor") != NULL;
        ok = ok && strstr(detail, "kind=genesis") == NULL;

        condition_engine_tick();
        struct condition_runtime_snapshot snap;
        ok = ok && cond_snapshot(&snap) && snap.currently_active;

        ok = ok && write_sentinel(dir, "anchor", "PASS", "", 1718000160LL);
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        condition_engine_tick();
        ok = ok && cond_snapshot(&snap) && !snap.currently_active;
        CSW_CHECK("mixed FAIL+PASS kinds page on the failing kind only", ok);
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

    /* (g) Cross-build staleness: a FAIL written by a DIFFERENT binary than the
     * one running is not evidence about THIS build (shared verdict dir + a
     * freshly-deployed binary) — it must NOT latch the pager. A SAME-build
     * FAIL alongside it still pages, proving we did not disable FAIL paging
     * wholesale; the stale one is excluded from the page detail. */
    {
        watch_test_setup(dir);
        /* Stale cross-build FAIL alone → recorded but never raises. */
        bool ok = write_sentinel_commit(dir, "anchor", "FAIL",
                                        "rpc_unreachable", 1718000500LL,
                                        "deadbee_oldbuild");
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        ok = ok && dump_bool("fail_latched") == false;
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && replay_canary_failed_test_remedy_calls() == 0;

        /* A same-build FAIL on another kind DOES page. */
        ok = ok && write_sentinel(dir, "genesis", "FAIL", "sha3_mismatch",
                                  1718000501LL);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();
        char detail[256];
        int fails = canary_sentinel_watch_fail_detail(detail, sizeof(detail));
        ok = ok && fails == 1;                            /* only the genuine one */
        ok = ok && strstr(detail, "kind=genesis") != NULL;
        ok = ok && strstr(detail, "kind=anchor") == NULL; /* stale one excluded */
        CSW_CHECK("cross-build FAIL ignored; same-build FAIL still pages", ok);
        watch_test_teardown(dir);
    }

    /* (h) -dirty normalization: the canary harness writes the BARE short hash
     * while a dirty-built binary's zcl_build_commit() carries "-dirty". A
     * same-SOURCE FAIL must STILL page — never be demoted as cross-build (the
     * dev-lane default deploy is a dirty build). */
    {
        watch_test_setup(dir);
        char bare[64];
        strip_dirty(zcl_build_commit(), bare, sizeof(bare));
        bool ok = write_sentinel_commit(dir, "anchor", "FAIL", "sha3_mismatch",
                                        1718000600LL, bare);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();
        CSW_CHECK("same-source FAIL pages even when binary is -dirty "
                  "(bare-hash sentinel)", ok);
        watch_test_teardown(dir);
    }

    /* (i) Run-start staleness: the shared verdict dir can contain a FAIL from
     * this same build, but from a canary run that started before this node
     * process. It is not evidence about the running process, so it must not
     * page. A fresh same-build FAIL still pages. */
    {
        watch_test_setup(dir);
        canary_sentinel_watch_test_set_process_start(1718000000LL);
        bool ok = write_sentinel(dir, "anchor", "FAIL", "old_process_fail",
                                 1718000500LL);
        canary_sentinel_watch_tick_once();
        ok = ok && !canary_sentinel_watch_fail_active();
        ok = ok && dump_bool("fail_latched") == false;
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && replay_canary_failed_test_remedy_calls() == 0;

        ok = ok && write_sentinel(dir, "genesis", "FAIL", "fresh_fail",
                                  1718002000LL);
        canary_sentinel_watch_tick_once();
        ok = ok && canary_sentinel_watch_fail_active();
        char detail[256];
        int fails = canary_sentinel_watch_fail_detail(detail, sizeof(detail));
        ok = ok && fails == 1;
        ok = ok && strstr(detail, "kind=genesis") != NULL;
        ok = ok && strstr(detail, "kind=anchor") == NULL;
        CSW_CHECK("pre-start FAIL ignored; fresh same-build FAIL still pages",
                  ok);
        watch_test_teardown(dir);
    }

    rmdir(dir);
    return failures;
}
