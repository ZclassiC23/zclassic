/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the command interaction ledger — the agent flight recorder
 * (Phase D, util/command_ledger). Proves: well-formed sub-PIPE_BUF NDJSON
 * appends; kernel-atomic concurrent (forked) appends; cap rotation with a
 * single retained generation + retention_gap; the content-free PRIVACY CANARY
 * (a secret input never lands in the ledger); summary/tail statistics + caps;
 * durable-ledger describe integration; and NULL-sink dispatch safety.
 */

#define _GNU_SOURCE
#include "test/test_helpers.h"

#include "config/command_catalog.h"
#include "kernel/command_registry.h"
#include "util/command_ledger.h"
#include "util/safe_alloc.h"
#include "json/json.h"
#include "platform/clock.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── helpers ────────────────────────────────────────────────────────────── */
static char *make_tmpdir(void)
{
    char tmpl[] = "/tmp/zcl_cmd_ledger_XXXXXX";
    char *d = mkdtemp(tmpl);
    return d ? strdup(d) : NULL;
}

static char *slurp(const char *path, size_t *len)
{
    if (len)
        *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return NULL;
    }
    size_t n = (size_t)st.st_size;
    char *b = zcl_malloc(n + 1, "test ledger slurp");
    if (!b) {
        close(fd);
        return NULL;
    }
    ssize_t g = pread(fd, b, n, 0);
    close(fd);
    if (g < 0) {
        free(b);
        return NULL;
    }
    b[g] = '\0';
    if (len)
        *len = (size_t)g;
    return b;
}

static const struct zcl_command_spec *leaf(const char *path)
{
    return zcl_command_registry_find(zcl_command_catalog(), path, NULL);
}

/* Dispatch a leaf with a JSON input document through the real kernel path. */
static size_t exec_leaf_input(const struct zcl_command_spec *spec,
                              const char *input_json, char *out,
                              size_t out_size)
{
    const struct zcl_command_registry *reg = zcl_command_catalog();
    struct zcl_command_context ctx = {
        .registry = reg,
        .operator_lane = "dev",
        .granted_capabilities = ~(uint64_t)0,
        .authority_ceiling = ZCL_COMMAND_AUTH_OWNER,
    };
    struct json_value input;
    json_init(&input);
    if (!input_json || !input_json[0])
        json_set_object(&input);
    else if (!json_read(&input, input_json, strlen(input_json)))
        json_set_object(&input);
    enum zcl_command_exit code = ZCL_COMMAND_EXIT_INTERNAL;
    size_t n = zcl_command_registry_execute_json(
        reg, spec, &ctx, &input, false, spec->path, "normal", 0, 0, NULL, out,
        out_size, &code);
    json_free(&input);
    return n;
}

/* Build one record and hand it straight to the sink (fixture path). */
static void sink_record(const char *lf, uint64_t seq, int64_t elapsed_us,
                        bool ok, bool budget_exceeded, int64_t output_bytes)
{
    struct zcl_command_ledger_record r;
    memset(&r, 0, sizeof(r));
    r.ts_unix_ms = clock_now_wall_ms();
    r.seq = seq;
    r.leaf = lf;
    r.transport = ZCL_CMD_TRANSPORT_NATIVE;
    r.input_bytes = 2;
    r.output_bytes = output_bytes;
    r.budget_bytes = 4096;
    r.budget_exceeded = budget_exceeded;
    r.elapsed_us = elapsed_us;
    r.budget_ms = 250;
    r.latency_class = ZCL_COMMAND_LATENCY_FAST;
    r.ok = ok;
    (void)snprintf(r.request_id, sizeof(r.request_id), "local-%016llx",
                   (unsigned long long)seq);
    command_ledger_sink(&r);
}

/* Count and validate NDJSON lines; returns line count, sets *all_ok. */
static int validate_lines(const char *buf, size_t len, bool *all_ok)
{
    int lines = 0;
    *all_ok = true;
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && buf[i] != '\n')
            i++;
        size_t ll = i - s;
        if (i < len)
            i++;
        if (ll == 0)
            continue;
        lines++;
        if (ll >= 4096) {
            *all_ok = false;
            continue;
        }
        struct json_value rec;
        json_init(&rec);
        if (!json_read(&rec, buf + s, ll) || rec.type != JSON_OBJ)
            *all_ok = false;
        else if (strcmp(json_get_str(json_get(&rec, "schema")),
                        "zcl.cmd_ledger.v1") != 0)
            *all_ok = false;
        json_free(&rec);
    }
    return lines;
}

/* ── (a) N appends -> N well-formed lines each < 4096B ──────────────────── */
static int test_ledger_append_wellformed(void)
{
    int failures = 0;
    TEST("N sink appends produce N well-formed lines each < 4096B") {
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir));
        command_ledger_test_set_cap(0);
        const struct zcl_command_spec *help = leaf("discover.help");
        ASSERT(help != NULL);
        char out[ZCL_COMMAND_RESULT_BUDGET + 1];
        const int N = 25;
        for (int i = 0; i < N; i++)
            ASSERT(exec_leaf_input(help, "{}", out, sizeof(out)) > 0);
        char path[700];
        (void)snprintf(path, sizeof(path),
                       "%s/telemetry/command_ledger.ndjson", dir);
        size_t len = 0;
        char *buf = slurp(path, &len);
        ASSERT(buf != NULL);
        bool all_ok = false;
        int lines = validate_lines(buf, len, &all_ok);
        ASSERT_EQ(lines, N);
        ASSERT(all_ok);
        free(buf);
        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (b) concurrent (forked) writers -> exact, un-torn line count ───────── */
static int test_ledger_concurrent_writers(void)
{
    int failures = 0;
    TEST("concurrent fresh-process writers append exact, un-torn lines") {
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir));
        command_ledger_test_set_cap(0);
        const int K = 4, M = 64;
        for (int c = 0; c < K; c++) {
            pid_t pid = fork();
            ASSERT(pid >= 0);
            if (pid == 0) {
                for (int i = 0; i < M; i++)
                    sink_record("concurrent.leaf", (uint64_t)(c * 1000 + i),
                                123, true, false, 80);
                _exit(0);
            }
        }
        for (int c = 0; c < K; c++) {
            int st = 0;
            (void)wait(&st);
        }
        char path[700];
        (void)snprintf(path, sizeof(path),
                       "%s/telemetry/command_ledger.ndjson", dir);
        size_t len = 0;
        char *buf = slurp(path, &len);
        ASSERT(buf != NULL);
        bool all_ok = false;
        int lines = validate_lines(buf, len, &all_ok);
        ASSERT_EQ(lines, K * M);
        ASSERT(all_ok);
        free(buf);
        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (c) rotation at cap; one generation; retention_gap on 2nd rotation ─── */
static int test_ledger_rotation(void)
{
    int failures = 0;
    TEST("rotation caps the file, keeps one generation, flags retention_gap") {
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir));
        command_ledger_test_set_cap(4096);
        for (int i = 0; i < 120; i++)
            sink_record("rotate.leaf", (uint64_t)i, 111, true, false, 64);
        char cur[700], one[720];
        (void)snprintf(cur, sizeof(cur),
                       "%s/telemetry/command_ledger.ndjson", dir);
        (void)snprintf(one, sizeof(one), "%s.1", cur);
        struct stat st;
        ASSERT(stat(cur, &st) == 0);              /* current reopened */
        ASSERT(st.st_size <= 4096 + 1024);        /* bounded near the cap */
        ASSERT(stat(one, &st) == 0);              /* one rotated generation */

        struct json_value sum;
        json_init(&sum);
        json_set_object(&sum);
        ASSERT(command_ledger_summary(0, NULL, 10, &sum));
        ASSERT(json_get_bool(json_get(&sum, "retention_gap")) == true);
        json_free(&sum);

        struct json_value ds;
        json_init(&ds);
        json_set_object(&ds);
        ASSERT(command_ledger_dump_state_json(&ds, NULL));
        ASSERT(json_get_int(json_get(&ds, "rotations")) >= (int64_t)2);
        json_free(&ds);

        command_ledger_test_set_cap(0);
        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (d) PRIVACY CANARY: a secret input never lands in the ledger ───────── */
static int test_ledger_privacy_canary(void)
{
    int failures = 0;
    TEST("ledger is content-free: a secret input is absent from bytes+tail") {
        static const char *SECRET = "zzcanaryneedle7391deadbeef";
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir));
        command_ledger_test_set_cap(0);
        const struct zcl_command_spec *search = leaf("discover.search");
        ASSERT(search != NULL);
        char in[128];
        (void)snprintf(in, sizeof(in), "{\"query\":\"%s\"}", SECRET);
        char out[ZCL_COMMAND_LIST_BUDGET + 1];
        ASSERT(exec_leaf_input(search, in, out, sizeof(out)) > 0);
        /* The dispatch's OWN output echoes the secret (proving it really flowed
         * through the command); the ledger must NOT retain it. */
        ASSERT(strstr(out, SECRET) != NULL);

        char path[700];
        (void)snprintf(path, sizeof(path),
                       "%s/telemetry/command_ledger.ndjson", dir);
        size_t len = 0;
        char *buf = slurp(path, &len);
        ASSERT(buf != NULL);
        ASSERT(strstr(buf, SECRET) == NULL);            /* canary ABSENT */
        ASSERT(strstr(buf, "discover.search") != NULL); /* call WAS recorded */
        free(buf);

        struct json_value tl;
        json_init(&tl);
        json_set_object(&tl);
        ASSERT(command_ledger_tail(50, NULL, &tl));
        char *tbuf = zcl_malloc(ZCL_COMMAND_LIST_BUDGET * 2, "test tail buf");
        ASSERT(tbuf != NULL);
        size_t tn = json_write(&tl, tbuf, ZCL_COMMAND_LIST_BUDGET * 2);
        ASSERT(tn > 0);
        ASSERT(strstr(tbuf, SECRET) == NULL);
        free(tbuf);
        json_free(&tl);

        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (e) summary/tail statistics + caps + budget ────────────────────────── */
static int test_ledger_summary_tail(void)
{
    int failures = 0;
    TEST("summary/tail compute correct stats within budget") {
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir));
        command_ledger_test_set_cap(0);
        /* alpha: 10 calls, elapsed 100..1000; ok when k>=2 (so 2 errors),
         * budget_exceeded when k<3 (so 3 overruns), output 100. */
        for (int k = 0; k < 10; k++)
            sink_record("alpha", (uint64_t)(k + 1), (k + 1) * 100, k >= 2,
                        k < 3, 100);
        /* beta: 5 calls, elapsed 50, output 200. */
        for (int k = 0; k < 5; k++)
            sink_record("beta", (uint64_t)(11 + k), 50, true, false, 200);

        struct json_value sum;
        json_init(&sum);
        json_set_object(&sum);
        ASSERT(command_ledger_summary(0, NULL, 50, &sum));
        char sbuf[9000];
        size_t sn = json_write(&sum, sbuf, sizeof(sbuf));
        ASSERT(sn > 0 && sn <= 8192); /* output within the leaf budget */
        ASSERT(json_get_bool(json_get(&sum, "retention_gap")) == false);
        const struct json_value *leaves = json_get(&sum, "leaves");
        ASSERT(leaves != NULL && leaves->type == JSON_ARR);
        ASSERT_EQ((int)leaves->num_children, 2);
        const struct json_value *a = json_at(leaves, 0); /* alpha ranks first */
        ASSERT_STR_EQ(json_get_str(json_get(a, "leaf")), "alpha");
        ASSERT_EQ(json_get_int(json_get(a, "calls")), (int64_t)10);
        ASSERT_EQ(json_get_int(json_get(a, "errors")), (int64_t)2);
        double er = json_get_real(json_get(a, "error_rate"));
        double d = er - 0.2;
        if (d < 0)
            d = -d;
        ASSERT(d < 1e-9);
        ASSERT_EQ(json_get_int(json_get(a, "p50_us")), (int64_t)500);
        ASSERT_EQ(json_get_int(json_get(a, "p99_us")), (int64_t)900);
        ASSERT_EQ(json_get_int(json_get(a, "avg_output_bytes")), (int64_t)100);
        ASSERT_EQ(json_get_int(json_get(a, "budget_exceeded_count")),
                  (int64_t)3);
        json_free(&sum);

        struct json_value tl;
        json_init(&tl);
        json_set_object(&tl);
        ASSERT(command_ledger_tail(3, NULL, &tl));
        const struct json_value *recs = json_get(&tl, "records");
        ASSERT(recs != NULL && recs->type == JSON_ARR);
        ASSERT_EQ((int)recs->num_children, 3);
        ASSERT_EQ(json_get_int(json_get(json_at(recs, 0), "seq")), (int64_t)15);
        ASSERT_EQ(json_get_int(json_get(json_at(recs, 1), "seq")), (int64_t)14);
        ASSERT_EQ(json_get_int(json_get(json_at(recs, 2), "seq")), (int64_t)13);
        ASSERT_STR_EQ(json_get_str(json_get(json_at(recs, 0), "leaf")), "beta");
        json_free(&tl);

        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (f) describe prefers the durable ledger source ─────────────────────── */
static int test_ledger_describe_source(void)
{
    int failures = 0;
    TEST("describe surfaces observed_source=durable_ledger with >=10 samples") {
        char *dir = make_tmpdir();
        ASSERT(dir != NULL);
        ASSERT(command_ledger_install(dir)); /* registers the latency source */
        command_ledger_test_set_cap(0);
        for (int k = 0; k < 12; k++)
            sink_record("core.status", (uint64_t)(k + 1), (k + 1) * 10, true,
                        false, 50);
        char dbuf[ZCL_COMMAND_SPEC_BUDGET + 1];
        size_t n = zcl_command_registry_describe_json(
            zcl_command_catalog(), "core.status", dbuf, sizeof(dbuf));
        ASSERT(n > 0);
        struct json_value root;
        ASSERT(json_read(&root, dbuf, n));
        const struct json_value *policy = json_get(&root, "policy");
        ASSERT(policy != NULL);
        ASSERT_STR_EQ(json_get_str(json_get(policy, "observed_source")),
                      "durable_ledger");
        ASSERT(json_get_int(json_get(policy, "observed_samples")) >=
               (int64_t)10);
        json_free(&root);
        command_ledger_uninstall();
        free(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── (g) NULL-sink dispatch safety ──────────────────────────────────────── */
static int test_ledger_null_sink_safe(void)
{
    int failures = 0;
    TEST("dispatch with no ledger sink installed succeeds and does not crash") {
        command_ledger_uninstall(); /* clear any registered sink/source */
        const struct zcl_command_spec *help = leaf("discover.help");
        ASSERT(help != NULL);
        char out[ZCL_COMMAND_RESULT_BUDGET + 1];
        ASSERT(exec_leaf_input(help, "{}", out, sizeof(out)) > 0);
        /* describe falls back to the in-process ring when no source is set. */
        char dbuf[ZCL_COMMAND_SPEC_BUDGET + 1];
        size_t n = zcl_command_registry_describe_json(
            zcl_command_catalog(), "discover.help", dbuf, sizeof(dbuf));
        ASSERT(n > 0);
        ASSERT(strstr(dbuf, "\"observed_source\":\"process_ring\"") != NULL);
        PASS();
    } _test_next:;
    return failures;
}

int test_command_ledger(void)
{
    int failures = 0;
    failures += test_ledger_append_wellformed();
    failures += test_ledger_concurrent_writers();
    failures += test_ledger_rotation();
    failures += test_ledger_privacy_canary();
    failures += test_ledger_summary_tail();
    failures += test_ledger_describe_source();
    failures += test_ledger_null_sink_safe();
    /* Ensure the global hooks are cleared before any other test group runs in
     * the shared serial-runner process. */
    command_ledger_uninstall();
    command_ledger_test_set_cap(0);
    printf("=== command_ledger: %d failures ===\n", failures);
    return failures;
}
