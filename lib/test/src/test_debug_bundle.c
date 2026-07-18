/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the debug bundle (app/controllers/src/
 * diagnostics_debug_bundle.c): the one-shot diagnostic capture behind the
 * `debugbundle` RPC / ops.debug.bundle native command and the
 * supervisor-stall auto-capture.
 *
 * Coverage:
 *   (a) debugbundle RPC end-to-end: a tmp datadir wired through
 *       diagnostics_controller_set_state, rpc_table_execute("debugbundle")
 *       returns { path, bytes, subsystems_captured, subsystems_failed,
 *       trigger=manual }; the file at the returned path exists, is exactly
 *       `bytes` long, parses as JSON, and carries the top-level contract
 *       (format, trigger, build, supervisor_stalls, subsystems with the
 *       registered dumpers keyed by name).
 *   (b) supervisor-stall trigger metadata: a direct debug_bundle_write
 *       with trigger "supervisor_stall" + child/reason lands trigger_child
 *       and trigger_stall_reason in the document.
 *   (c) no datadir: debug_bundle_write fails cleanly (false, empty path)
 *       rather than writing somewhere surprising.
 *
 * The auto-capture thread hand-off (debug_bundle_on_stall) is deliberately
 * NOT driven here: it is a detached best-effort worker whose timing would
 * make a unit test flaky. Its observer seam is covered deterministically
 * in test_supervisor.c ("process-wide stall observer"), and its rate
 * limit is one static timestamp + one in-flight flag reviewed by reading.
 * Dumpers run under the same minimal fixture test_health_rollup already
 * uses — every registered dumper must tolerate an unbooted process — so
 * no extra per-subsystem setup is paid here. */

#include "test/test_helpers.h"
#include "controllers/diagnostics_controller.h"
#include "controllers/diagnostics_internal.h"
#include "json/json.h"
#include "rpc/server.h"
#include "util/safe_alloc.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DBB_CHECK(name, expr) do { \
    printf("debug_bundle: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Read a whole file into a fresh NUL-terminated buffer (zcl_malloc'd;
 * caller frees). NULL on any failure. */
static char *dbb_read_file(const char *path, long *size_out)
{
    if (size_out) *size_out = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = zcl_malloc((size_t)sz + 1, "debug_bundle_test_read");
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    if (size_out) *size_out = sz;
    return buf;
}

/* Parse the bundle file at `path` into `doc` (caller json_free's). */
static bool dbb_parse_bundle(const char *path, struct json_value *doc,
                             long *size_out)
{
    char *raw = dbb_read_file(path, size_out);
    if (!raw) return false;
    bool ok = json_read(doc, raw, strlen(raw)) && doc->type == JSON_OBJ;
    free(raw);
    return ok;
}

int test_debug_bundle(void)
{
    printf("\n=== debug_bundle tests ===\n");
    int failures = 0;

    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "debug_bundle", "1");
    mkdir("./test-tmp", 0755);
    mkdir(dir, 0755);
    diagnostics_controller_set_state(NULL, dir);

    /* ── (a) debugbundle RPC end-to-end ──────────────────────────── */
    char bundle_path[1200] = {0};
    {
        struct json_value params;
        json_init(&params);
        json_set_array(&params);
        struct json_value result;
        json_init(&result);

        struct rpc_table tbl;
        rpc_table_init(&tbl);
        register_diagnostics_rpc_commands(&tbl);
        bool ok = rpc_table_execute(&tbl, "debugbundle", &params, &result);

        DBB_CHECK("rpc: execute returns true", ok);
        const char *path = json_get_str(json_get(&result, "path"));
        DBB_CHECK("rpc: path names a debug-bundle file",
                  path && strstr(path, "debug-bundle-") != NULL);
        DBB_CHECK("rpc: trigger is manual",
                  strcmp(json_get_str(json_get(&result, "trigger")),
                         "manual") == 0);
        size_t count = diagnostics_dumper_count();
        int64_t captured =
            json_get_int(json_get(&result, "subsystems_captured"));
        int64_t failed =
            json_get_int(json_get(&result, "subsystems_failed"));
        DBB_CHECK("rpc: captured + failed covers every registered dumper",
                  captured >= 1 && failed >= 0 &&
                  captured + failed == (int64_t)count);
        int64_t bytes = json_get_int(json_get(&result, "bytes"));
        DBB_CHECK("rpc: bytes > 0", bytes > 0);
        if (path) snprintf(bundle_path, sizeof(bundle_path), "%s", path);

        struct json_value doc;
        json_init(&doc);
        long fsize = 0;
        ok = dbb_parse_bundle(bundle_path, &doc, &fsize);
        DBB_CHECK("file: exists and parses as a JSON object", ok);
        if (ok) {
            DBB_CHECK("file: size matches the reported bytes",
                      fsize == (long)bytes);
            DBB_CHECK("file: format tag",
                      strcmp(json_get_str(json_get(&doc, "format")),
                             "zcl.debug_bundle.v1") == 0);
            DBB_CHECK("file: captured_at_utc present",
                      json_get(&doc, "captured_at_utc") != NULL);
            DBB_CHECK("file: trigger is manual",
                      strcmp(json_get_str(json_get(&doc, "trigger")),
                             "manual") == 0);
            const struct json_value *build = json_get(&doc, "build");
            DBB_CHECK("file: build block carries build_commit",
                      build && build->type == JSON_OBJ &&
                      json_get(build, "build_commit") != NULL);
            const struct json_value *stalls =
                json_get(&doc, "supervisor_stalls");
            DBB_CHECK("file: supervisor_stalls summary present",
                      stalls && stalls->type == JSON_OBJ &&
                      json_get(stalls, "children") != NULL);
            const struct json_value *subs = json_get(&doc, "subsystems");
            DBB_CHECK("file: subsystems object present",
                      subs && subs->type == JSON_OBJ);
            DBB_CHECK("file: supervisor dumper captured by name",
                      subs && json_get(subs, "supervisor") != NULL);
            json_free(&doc);
        }
        json_free(&params);
        json_free(&result);
    }

    /* ── (b) supervisor-stall trigger metadata ───────────────────── */
    char stall_path[1200] = {0};
    {
        struct debug_bundle_result res;
        bool ok = debug_bundle_write("supervisor_stall", "test.child",
                                     (int)SUPERVISOR_STALL_TIME_DEADLINE,
                                     &res);
        DBB_CHECK("stall write: returns true", ok);
        snprintf(stall_path, sizeof(stall_path), "%s", res.path);

        struct json_value doc;
        json_init(&doc);
        ok = dbb_parse_bundle(stall_path, &doc, NULL);
        DBB_CHECK("stall write: file parses", ok);
        if (ok) {
            DBB_CHECK("stall write: trigger is supervisor_stall",
                      strcmp(json_get_str(json_get(&doc, "trigger")),
                             "supervisor_stall") == 0);
            DBB_CHECK("stall write: trigger_child recorded",
                      strcmp(json_get_str(json_get(&doc, "trigger_child")),
                             "test.child") == 0);
            DBB_CHECK("stall write: trigger_stall_reason recorded",
                      strcmp(json_get_str(
                                 json_get(&doc, "trigger_stall_reason")),
                             "time_deadline") == 0);
            json_free(&doc);
        }
    }

    /* ── (c) no datadir: clean failure, no surprise write ────────── */
    {
        diagnostics_controller_set_state(NULL, "");
        struct debug_bundle_result res;
        bool ok = debug_bundle_write("manual", NULL,
                                     (int)SUPERVISOR_STALL_NONE, &res);
        DBB_CHECK("no datadir: write fails cleanly", !ok);
        DBB_CHECK("no datadir: no path returned", res.path[0] == '\0');
    }

    if (bundle_path[0]) unlink(bundle_path);
    if (stall_path[0]) unlink(stall_path);
    rmdir(dir);
    return failures;
}
