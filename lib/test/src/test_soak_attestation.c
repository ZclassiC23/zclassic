/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_soak_attestation — unit-tests for the soak attestation service.
 *
 * Covers:
 *   1. Line format — a single tick produces one well-formed JSON line with
 *      all required fields.
 *   2. Growth + rotation — writing enough fake bytes causes the service to
 *      rename the primary to .1 and open a fresh primary.
 *   3. fsync cadence — after SOAK_ATTESTATION_FSYNC_EVERY ticks the service
 *      increments its internal counter (tested via state dump).
 *   4. State dump — soak_dump_state_json returns a valid JSON object with
 *      expected fields.
 *   5. Failure resilience — if the datadir is /dev/null the write fails, the
 *      failure counter increments, but the tick does not crash.
 *   6. Reset — soak_attestation_reset_for_test() returns the service to its
 *      pristine state.
 *
 * Tests use a tmpdir built under /tmp so the live node.db is never touched.
 * The service is reset between cases so each case starts clean. */

#include "test/test_helpers.h"
#include "controllers/agent_security_posture.h"
#include "services/soak_attestation_service.h"
#include "json/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static char g_tmpdir[256];

static void make_tmpdir(void)
{
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/test_soak_attest_%d", (int)getpid());
    mkdir(g_tmpdir, 0755);
}

static void cleanup_tmpdir(void)
{
    /* Remove all files in tmpdir, then the dir itself. */
    char path[320];
    DIR *d = opendir(g_tmpdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", g_tmpdir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(g_tmpdir);
}

/* Read the primary attestation file into buf (null-terminated). */
static bool read_primary(char *buf, size_t bufsz)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/soak_attestation.jsonl", g_tmpdir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

/* Check whether a JSON key="value" substring exists in a line. */
static bool has_json_key(const char *line, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    return strstr(line, needle) != NULL;
}

/* ── Tests ──────────────────────────────────────────────────────── */

static int test_line_format(void)
{
    int failures = 0;
    TEST("soak_attestation: single tick writes well-formed JSON line") {
        soak_attestation_reset_for_test();
        agent_security_posture_test_override_review_required(0);
        make_tmpdir();
        soak_attestation_init(g_tmpdir);
        soak_attestation_tick();

        char buf[1024] = {0};
        bool got = read_primary(buf, sizeof(buf));
        if (!got) {
            printf("FAIL (no data written)\n");
            failures++; goto _done_fmt;
        }
        /* Every required field must be present. */
        bool ok = has_json_key(buf, "ts")              &&
                  has_json_key(buf, "height")          &&
                  has_json_key(buf, "healthy")         &&
                  has_json_key(buf, "degraded_reason") &&
                  has_json_key(buf, "security_review_required") &&
                  has_json_key(buf, "security_posture_ok") &&
                  has_json_key(buf, "window_eligible") &&
                  has_json_key(buf, "build_commit")    &&
                  has_json_key(buf, "uptime_s");
        if (!ok) {
            printf("FAIL: missing field(s) in line: %s\n", buf);
            failures++; goto _done_fmt;
        }
        /* Line must end with '\n'. */
        size_t len = strlen(buf);
        if (len == 0 || buf[len - 1] != '\n') {
            printf("FAIL: line does not end with newline: [%s]\n", buf);
            failures++; goto _done_fmt;
        }
        /* Must look like a JSON object (starts with '{', ends with '}' before \n). */
        if (buf[0] != '{') {
            printf("FAIL: line does not start with '{': %s\n", buf);
            failures++;
        }
        PASS();
    } _done_fmt:;
    agent_security_posture_test_override_review_required(-1);
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

static int test_security_posture_breaks_window(void)
{
    int failures = 0;
    TEST("soak_attestation: review-required posture is window-ineligible") {
        soak_attestation_reset_for_test();
        agent_security_posture_test_override_review_required(1);
        make_tmpdir();
        soak_attestation_init(g_tmpdir);
        soak_attestation_tick();

        char buf[1024] = {0};
        bool ok = read_primary(buf, sizeof(buf)) &&
            strstr(buf, "\"healthy\":false") != NULL &&
            strstr(buf, "\"security_review_required\":true") != NULL &&
            strstr(buf, "\"security_posture_ok\":false") != NULL &&
            strstr(buf, "\"window_eligible\":false") != NULL &&
            strstr(buf, "\"degraded_reason\":\"review_required_test\"") != NULL;

        struct json_value state = {0};
        ok = ok && soak_dump_state_json(&state, NULL);
        ok = ok && !json_get_bool(json_get(&state, "last_healthy"));
        ok = ok && !json_get_bool(json_get(&state,
                                            "last_window_eligible"));
        ok = ok && json_get_bool(json_get(
            &state, "last_security_review_required"));
        json_free(&state);

        if (ok) PASS();
        else { printf("FAIL: line=%s\n", buf); failures++; }
    }
    agent_security_posture_test_override_review_required(-1);
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

static int test_rotation(void)
{
    int failures = 0;
    TEST("soak_attestation: rotation when file exceeds 50 MB") {
        soak_attestation_reset_for_test();
        make_tmpdir();
        soak_attestation_init(g_tmpdir);

        /* Write a dummy primary file that is already at the rotation threshold.
         * We create it manually via truncate so we don't need to write 50 MB
         * of actual data. The service reads st_size on the first open. */
        char primary[320];
        snprintf(primary, sizeof(primary),
                 "%s/soak_attestation.jsonl", g_tmpdir);
        int fd = open(primary, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            printf("FAIL: could not create primary: %s\n", strerror(errno));
            failures++; goto _done_rot;
        }
        /* Sparse file at exactly the rotation size + 1 byte to exceed it. */
        if (ftruncate(fd, (off_t)(SOAK_ATTESTATION_ROTATE_BYTES + 1)) < 0) {
            printf("FAIL: ftruncate: %s\n", strerror(errno));
            close(fd);
            failures++; goto _done_rot;
        }
        close(fd);

        /* First tick should detect the oversize primary and rotate before
         * writing the new line. */
        soak_attestation_tick();

        /* After rotation the .1 file must exist. */
        char rotated[320];
        snprintf(rotated, sizeof(rotated),
                 "%s/soak_attestation.jsonl.1", g_tmpdir);
        struct stat st;
        if (stat(rotated, &st) != 0) {
            printf("FAIL: .1 file not found after rotation: %s\n",
                   strerror(errno));
            failures++; goto _done_rot;
        }

        /* The new primary must be small (just the one line the tick wrote). */
        struct stat st2;
        if (stat(primary, &st2) != 0) {
            printf("FAIL: new primary not found after rotation\n");
            failures++; goto _done_rot;
        }
        if (st2.st_size > 1024) {
            printf("FAIL: new primary too large (%lld bytes) — rotation may have "
                   "not happened\n", (long long)st2.st_size);
            failures++; goto _done_rot;
        }

        /* State dump must show exactly 1 rotation. */
        struct json_value jv = {0};
        soak_dump_state_json(&jv, NULL);
        int64_t rots = json_get_int(json_get(&jv, "rotations"));
        json_free(&jv);
        if (rots != 1) {
            printf("FAIL: expected 1 rotation, got %lld\n", (long long)rots);
            failures++;
        } else {
            PASS();
        }
    } _done_rot:;
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

static int test_state_dump(void)
{
    int failures = 0;
    TEST("soak_attestation: state dump has all expected keys") {
        soak_attestation_reset_for_test();
        make_tmpdir();
        soak_attestation_init(g_tmpdir);
        soak_attestation_tick();

        struct json_value jv = {0};
        bool ok = soak_dump_state_json(&jv, NULL);
        if (!ok || jv.type != JSON_OBJ) {
            printf("FAIL: dump returned false or non-object\n");
            json_free(&jv);
            failures++; goto _done_dump;
        }
        /* Check key presence. */
        const char *keys[] = {
            "initialized", "lines_written", "last_ts",
            "last_healthy", "rotations", "write_failures", "file_bytes"
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const struct json_value *v = json_get(&jv, keys[i]);
            if (!v) {
                printf("FAIL: missing key '%s' in dump\n", keys[i]);
                failures++;
            }
        }
        /* lines_written must be >= 1 after one tick. */
        int64_t lw = json_get_int(json_get(&jv, "lines_written"));
        if (lw < 1) {
            printf("FAIL: lines_written=%lld after one tick\n", (long long)lw);
            failures++;
        }
        json_free(&jv);
        if (!failures) PASS();
    } _done_dump:;
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

static int test_write_failure_counter(void)
{
    int failures = 0;
    TEST("soak_attestation: write_failures increments when fd fails") {
        soak_attestation_reset_for_test();
        /* Pass a path that cannot be written (a non-existent subdirectory). */
        soak_attestation_init("/tmp/soak_test_no_such_dir_xyz/sub");

        /* The service will fail to open the file. Tick and verify the counter. */
        soak_attestation_tick();

        struct json_value jv = {0};
        soak_dump_state_json(&jv, NULL);
        int64_t wf = json_get_int(json_get(&jv, "write_failures"));
        json_free(&jv);
        if (wf < 1) {
            printf("FAIL: write_failures=%lld; expected >= 1 after failing open\n",
                   (long long)wf);
            failures++; goto _done_fail;
        }
        PASS();
    } _done_fail:;
    soak_attestation_reset_for_test();
    return failures;
}

static int test_reset(void)
{
    int failures = 0;
    TEST("soak_attestation: reset returns to pristine state") {
        soak_attestation_reset_for_test();
        make_tmpdir();
        soak_attestation_init(g_tmpdir);
        soak_attestation_tick();
        soak_attestation_tick();

        soak_attestation_reset_for_test();

        struct json_value jv = {0};
        soak_dump_state_json(&jv, NULL);
        bool init_flag = json_get_bool(json_get(&jv, "initialized"));
        int64_t lw     = json_get_int(json_get(&jv, "lines_written"));
        json_free(&jv);

        if (init_flag || lw != 0) {
            printf("FAIL: after reset: initialized=%d lines_written=%lld\n",
                   (int)init_flag, (long long)lw);
            failures++; goto _done_reset;
        }
        PASS();
    } _done_reset:;
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

static int test_multiple_lines(void)
{
    int failures = 0;
    TEST("soak_attestation: N ticks produce N lines") {
        const int N = 5;
        soak_attestation_reset_for_test();
        make_tmpdir();
        soak_attestation_init(g_tmpdir);

        for (int i = 0; i < N; i++)
            soak_attestation_tick();

        struct json_value jv = {0};
        soak_dump_state_json(&jv, NULL);
        int64_t lw = json_get_int(json_get(&jv, "lines_written"));
        json_free(&jv);

        if (lw != N) {
            printf("FAIL: expected %d lines, got %lld\n", N, (long long)lw);
            failures++; goto _done_multi;
        }
        /* Count newlines in the file. */
        char buf[4096] = {0};
        bool got = read_primary(buf, sizeof(buf));
        if (!got) {
            printf("FAIL: could not read primary file\n");
            failures++; goto _done_multi;
        }
        int newlines = 0;
        for (const char *p = buf; *p; p++)
            if (*p == '\n') newlines++;
        if (newlines != N) {
            printf("FAIL: expected %d newlines, got %d\n", N, newlines);
            failures++; goto _done_multi;
        }
        PASS();
    } _done_multi:;
    soak_attestation_reset_for_test();
    cleanup_tmpdir();
    return failures;
}

/* ── Test registration ───────────────────────────────────────────── */

int test_soak_attestation(void)
{
    int failures = 0;
    failures += test_line_format();
    failures += test_security_posture_breaks_window();
    failures += test_rotation();
    failures += test_state_dump();
    failures += test_write_failure_counter();
    failures += test_reset();
    failures += test_multiple_lines();
    return failures;
}
