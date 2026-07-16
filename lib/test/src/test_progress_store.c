/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the progress_store singleton (lib/storage/src/progress_store.c).
 *
 * Coverage:
 *   - open creates <datadir>/progress.kv with WAL + stage_cursor table
 *   - second open on the same datadir is idempotent (no error, same handle)
 *   - second open on a *different* datadir is rejected (one process, one store)
 *   - close releases the singleton; reopen on a fresh path succeeds
 *   - data persisted via the F-2 stage primitive survives close + reopen
 *   - dump_state_json reports open status, path, and stage_cursor row count */

#include "test/test_helpers.h"

#include "json/json.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/stage.h"

#include <dirent.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PS_CHECK(name, expr) do { \
    printf("progress_store: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* cwd-relative tmpdir to comply with the "no /tmp" project convention. */
static void ps_tmpdir(char *buf, size_t n, const char *tag)
{
    snprintf(buf, n, "./test-tmp/progress_store_%d_%s", (int)getpid(), tag);
}

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Tiny stage step that advances the cursor by one each time. */
static job_result_t step_advance_by_one(struct stage_step_ctx *c)
{
    c->cursor_out = c->cursor_in + 1;
    return JOB_ADVANCED;
}

/* Count quarantine sidecar files (progress.kv*.corrupt.*) in dir. Used to
 * assert the quick_check quarantine fired exactly when expected. */
static int ps_count_corrupt(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "progress.kv", 11) == 0 &&
            strstr(e->d_name, ".corrupt.") != NULL)
            n++;
    }
    closedir(d);
    return n;
}

int test_progress_store(void)
{
    printf("\n=== progress_store tests ===\n");
    int failures = 0;

    blocker_module_init();

    /* ── open creates file + table, idempotent on same path ────────── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "open");
        mkdir_p(dir);

        PS_CHECK("first open succeeds", progress_store_open(dir));
        PS_CHECK("handle is non-NULL", progress_store_db() != NULL);

        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/progress.kv", dir);
        struct stat st;
        PS_CHECK("progress.kv file exists",
                 stat(fpath, &st) == 0 && S_ISREG(st.st_mode));

        sqlite3 *db1 = progress_store_db();
        PS_CHECK("second open same dir is idempotent",
                 progress_store_open(dir));
        PS_CHECK("handle unchanged after idempotent open",
                 progress_store_db() == db1);

        progress_store_tx_lock();
        bool recursive_trylock = progress_store_tx_trylock();
        PS_CHECK("trylock succeeds recursively for current owner",
                 recursive_trylock);
        if (recursive_trylock)
            progress_store_tx_unlock();
        progress_store_tx_unlock();

        /* Verify the stage_cursor schema is queryable. */
        sqlite3_stmt *st_check = NULL;
        int rc = sqlite3_prepare_v2(progress_store_db(),
            "SELECT COUNT(*) FROM stage_cursor",
            -1, &st_check, NULL);
        PS_CHECK("stage_cursor table query prepares",
                 rc == SQLITE_OK);
        sqlite3_finalize(st_check);

        /* Observational status readers use their own WAL connection rather
         * than queueing behind the reducer's shared-handle transaction lock. */
        sqlite3 *reader = progress_store_open_reader();
        PS_CHECK("independent read-only connection opens",
                 reader != NULL && reader != progress_store_db());
        st_check = NULL;
        rc = reader ? sqlite3_prepare_v2(
            reader, "SELECT COUNT(*) FROM stage_cursor", -1, &st_check, NULL)
                    : SQLITE_CANTOPEN;
        PS_CHECK("independent reader sees stage_cursor", rc == SQLITE_OK);
        if (st_check)
            sqlite3_finalize(st_check);
        if (reader)
            sqlite3_close(reader);

        /* Different dir is rejected (one process, one store). */
        char dir2[256];
        ps_tmpdir(dir2, sizeof(dir2), "open_other");
        mkdir_p(dir2);
        PS_CHECK("second open with different dir is rejected",
                 !progress_store_open(dir2));

        progress_store_close();
        PS_CHECK("handle NULL after close",
                 progress_store_db() == NULL);

        test_cleanup_tmpdir(dir);
        test_cleanup_tmpdir(dir2);
    }

    /* ── cursor persistence: stage cursor survives close + reopen ──── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "persist");
        mkdir_p(dir);

        PS_CHECK("open #1 OK", progress_store_open(dir));
        stage_t *s1 = stage_create("test-advance",
                                    step_advance_by_one, NULL);
        PS_CHECK("stage create OK", s1 != NULL);

        sqlite3 *db = progress_store_db();
        for (int i = 0; i < 5; i++) {
            PS_CHECK("advance step OK",
                     stage_run_once(s1, db) == JOB_ADVANCED);
        }
        PS_CHECK("cursor == 5 after 5 advances",
                 stage_cursor(s1) == 5);

        stage_destroy(s1);
        progress_store_close();

        /* Reopen and verify the cursor is still 5. */
        PS_CHECK("open #2 OK (reopen)", progress_store_open(dir));
        stage_t *s2 = stage_create("test-advance",
                                    step_advance_by_one, NULL);
        /* stage_run_once will restore cursor from DB on first invocation. */
        PS_CHECK("first step after reopen advances from 5 to 6",
                 stage_run_once(s2, progress_store_db()) == JOB_ADVANCED);
        PS_CHECK("cursor == 6 after reopen + 1 step",
                 stage_cursor(s2) == 6);

        stage_destroy(s2);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── dump_state_json shape ─────────────────────────────────────── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "dump");
        mkdir_p(dir);

        char buf[1024];

        /* Closed state. */
        struct json_value v_closed;
        json_init(&v_closed);
        PS_CHECK("dump_state_json works when closed",
                 progress_store_dump_state_json(&v_closed, NULL));
        size_t n = json_write(&v_closed, buf, sizeof(buf));
        PS_CHECK("closed dump serializes", n > 0 && n < sizeof(buf));
        PS_CHECK("closed dump has open=false",
                 strstr(buf, "\"open\":false") != NULL);
        json_free(&v_closed);

        /* Open state. */
        PS_CHECK("open for dump", progress_store_open(dir));
        struct json_value v_open;
        json_init(&v_open);
        PS_CHECK("dump_state_json works when open",
                 progress_store_dump_state_json(&v_open, NULL));
        n = json_write(&v_open, buf, sizeof(buf));
        PS_CHECK("open dump serializes", n > 0 && n < sizeof(buf));
        PS_CHECK("open dump has open=true",
                 strstr(buf, "\"open\":true") != NULL);
        PS_CHECK("open dump reports stage_cursor_rows",
                 strstr(buf, "\"stage_cursor_rows\"") != NULL);
        PS_CHECK("open dump reports path",
                 strstr(buf, "progress.kv") != NULL);
        json_free(&v_open);

        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── input validation ──────────────────────────────────────────── */
    {
        PS_CHECK("open(NULL) rejected", !progress_store_open(NULL));
        PS_CHECK("open(\"\") rejected", !progress_store_open(""));
        PS_CHECK("reader unavailable while closed",
                 progress_store_open_reader() == NULL);
        PS_CHECK("dump(NULL) rejected",
                 !progress_store_dump_state_json(NULL, NULL));
    }

    /* ── progress_meta k/v API (S-4b) ───────────────────────────────── */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "meta");
        mkdir_p(dir);
        PS_CHECK("open for meta", progress_store_open(dir));
        sqlite3 *db = progress_store_db();

        /* Missing key returns found=false, len=0, function returns true. */
        bool found = true;
        size_t got = 99;
        uint8_t out[64] = {0};
        PS_CHECK("get missing returns true",
                 progress_meta_get(db, "no-such", out, sizeof(out),
                                   &got, &found));
        PS_CHECK("get missing reports not-found", !found && got == 0);

        /* Round-trip a blob value. */
        const char *payload = "the-quick-brown-fox";
        size_t payload_len = strlen(payload);
        PS_CHECK("set blob OK",
                 progress_meta_set(db, "k.blob", payload, payload_len));

        memset(out, 0, sizeof(out));
        got = 0; found = false;
        PS_CHECK("get blob OK",
                 progress_meta_get(db, "k.blob", out, sizeof(out),
                                   &got, &found));
        PS_CHECK("get blob reports found", found);
        PS_CHECK("get blob length matches", got == payload_len);
        PS_CHECK("get blob bytes match",
                 memcmp(out, payload, payload_len) == 0);

        memset(out, 0, sizeof(out));
        got = 0; found = false;
        PS_CHECK("exact-BLOB get accepts BLOB storage",
                 progress_meta_get_blob_exact(
                     db, "k.blob", out, sizeof(out), &got, &found) &&
                 found && got == payload_len &&
                 memcmp(out, payload, payload_len) == 0);
        PS_CHECK("exact-BLOB get reports missing without error",
                 progress_meta_get_blob_exact(
                     db, "k.exact-missing", out, sizeof(out), &got,
                     &found) && !found && got == 0);

        PS_CHECK("exact-BLOB TEXT fixture writes",
                 sqlite3_exec(db,
                     "INSERT OR REPLACE INTO progress_meta(key,value) "
                     "VALUES('k.exact-text',CAST('123x' AS TEXT))",
                     NULL, NULL, NULL) == SQLITE_OK);
        got = 99; found = false;
        PS_CHECK("exact-BLOB get rejects numeric-prefix TEXT authority",
                 !progress_meta_get_blob_exact(
                     db, "k.exact-text", out, sizeof(out), &got, &found) &&
                 found && got == 0);
        PS_CHECK("exact-BLOB REAL fixture writes",
                 sqlite3_exec(db,
                     "INSERT OR REPLACE INTO progress_meta(key,value) "
                     "VALUES('k.exact-real',1.25)",
                     NULL, NULL, NULL) == SQLITE_OK);
        got = 99; found = false;
        PS_CHECK("exact-BLOB get rejects REAL authority",
                 !progress_meta_get_blob_exact(
                     db, "k.exact-real", out, sizeof(out), &got, &found) &&
                 found && got == 0);
        PS_CHECK("exact-BLOB oversized fixture writes",
                 progress_meta_set(db, "k.exact-long", out, sizeof(out)));
        got = 99; found = false;
        PS_CHECK("exact-BLOB get never truncates authority",
                 !progress_meta_get_blob_exact(
                     db, "k.exact-long", out, 4, &got, &found) && found &&
                 got == 0);

        /* Out-of-band: get with NULL buffer reports length only. */
        got = 0;
        PS_CHECK("get blob with NULL buf reports length",
                 progress_meta_get(db, "k.blob", NULL, 0, &got, &found) &&
                 got == payload_len && found);

        /* INSERT OR REPLACE semantics — second set overwrites. */
        const char *payload2 = "OVERWRITTEN";
        PS_CHECK("set overwrite OK",
                 progress_meta_set(db, "k.blob",
                                   payload2, strlen(payload2)));
        got = 0;
        PS_CHECK("overwritten get OK",
                 progress_meta_get(db, "k.blob", out, sizeof(out),
                                   &got, &found));
        PS_CHECK("overwritten length matches",
                 got == strlen(payload2));
        PS_CHECK("overwritten bytes match",
                 memcmp(out, payload2, strlen(payload2)) == 0);

        /* int32 round-trip — typical sentinel/height storage. */
        int32_t height_in = 3120921;
        int32_t height_out = 0;
        PS_CHECK("set int32",
                 progress_meta_set(db, "k.height",
                                   &height_in, sizeof(height_in)));
        PS_CHECK("get int32",
                 progress_meta_get(db, "k.height", &height_out,
                                   sizeof(height_out), &got, &found));
        PS_CHECK("int32 round-trips",
                 found && got == sizeof(int32_t) &&
                 height_out == height_in);

        /* delete removes the row. */
        PS_CHECK("delete OK", progress_meta_delete(db, "k.blob"));
        found = true; got = 99;
        PS_CHECK("delete is observable",
                 progress_meta_get(db, "k.blob", out, sizeof(out),
                                   &got, &found) && !found && got == 0);
        /* deleting a missing key is allowed (no-op). */
        PS_CHECK("delete missing is no-op success",
                 progress_meta_delete(db, "never-existed"));

        /* in_tx variants compose with an outer BEGIN IMMEDIATE. */
        PS_CHECK("BEGIN for compose",
                 sqlite3_exec(db, "BEGIN IMMEDIATE",
                              NULL, NULL, NULL) == SQLITE_OK);
        const uint8_t one = 1;
        PS_CHECK("set_in_tx",
                 progress_meta_set_in_tx(db, "sentinel", &one, 1));
        PS_CHECK("delete_in_tx (sentinel) ok",
                 progress_meta_delete_in_tx(db, "sentinel"));
        PS_CHECK("COMMIT compose OK",
                 sqlite3_exec(db, "COMMIT",
                              NULL, NULL, NULL) == SQLITE_OK);
        PS_CHECK("sentinel not present after delete in compose",
                 progress_meta_get(db, "sentinel", out, sizeof(out),
                                   &got, &found) && !found);

        /* Bad input → false. */
        PS_CHECK("set NULL db rejected",
                 !progress_meta_set(NULL, "k", "v", 1));
        PS_CHECK("set NULL key rejected",
                 !progress_meta_set(db, NULL, "v", 1));
        PS_CHECK("set empty key rejected",
                 !progress_meta_set(db, "", "v", 1));
        PS_CHECK("get NULL db rejected",
                 !progress_meta_get(NULL, "k", out, sizeof(out),
                                    &got, &found));

        /* Persistence across close + reopen. */
        const char *persist_payload = "PERSISTED-1";
        PS_CHECK("set for persistence",
                 progress_meta_set(db, "k.persist",
                                   persist_payload,
                                   strlen(persist_payload)));
        progress_store_close();
        PS_CHECK("reopen for persistence", progress_store_open(dir));
        memset(out, 0, sizeof(out));
        got = 0; found = false;
        PS_CHECK("persisted blob survives close+reopen",
                 progress_meta_get(progress_store_db(), "k.persist",
                                   out, sizeof(out), &got, &found) &&
                 found && got == strlen(persist_payload) &&
                 memcmp(out, persist_payload, got) == 0);
        progress_store_close();
        test_cleanup_tmpdir(dir);
    }

    /* ── integrity quick_check + quarantine self-heal (competition
     *    robustness) ─────────────────────────────────────────────────────
     *
     * A corrupt progress.kv must NOT pin the node silently. On open the store
     * runs PRAGMA quick_check; a non-"ok" verdict quarantines the file aside
     * (rename → progress.kv.corrupt.<ts>...) and reopens a FRESH, empty store
     * so boot can re-seed coins_kv from the snapshot/anchor and re-fold. This
     * mirrors node.db's db_quick_check_ok path. We prove three things:
     *   (a) a HEALTHY store reopens with NO quarantine (no false positive),
     *   (b) a deliberately page-garbled store is detected → quarantine file
     *       appears → reopen succeeds with a fresh, queryable, EMPTY store,
     *   (c) it is auto-terminating (one quarantine, not a loop). */
    {
        char dir[256];
        ps_tmpdir(dir, sizeof(dir), "quarantine");
        mkdir_p(dir);
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/progress.kv", dir);

        /* Seed a healthy store with a recognizable marker, then close so the
         * WAL is checkpointed back into the main file (close TRUNCATEs WAL). */
        PS_CHECK("quarantine: seed open", progress_store_open(dir));
        const char *marker = "MARKER-PRE-CORRUPT";
        PS_CHECK("quarantine: seed marker",
                 progress_meta_set(progress_store_db(), "k.marker",
                                   marker, strlen(marker)));
        progress_store_close();

        /* (a) Reopen the HEALTHY file — must NOT quarantine, marker survives. */
        PS_CHECK("quarantine: healthy reopen OK", progress_store_open(dir));
        {
            uint8_t out[64] = {0};
            size_t got = 0; bool found = false;
            PS_CHECK("quarantine: healthy marker survives",
                     progress_meta_get(progress_store_db(), "k.marker",
                                       out, sizeof(out), &got, &found) &&
                     found && got == strlen(marker) &&
                     memcmp(out, marker, got) == 0);
        }
        PS_CHECK("quarantine: no false-positive .corrupt file",
                 ps_count_corrupt(dir) == 0);
        progress_store_close();

        /* Deliberately corrupt a middle page of the main DB file. SQLite's
         * default page size is 4096; page 1 is the header (offset 0). We
         * scribble garbage well inside the file (offset 4096 onward) so the
         * header still parses far enough for quick_check to walk the b-tree
         * and report malformation rather than a bare "file is not a
         * database". */
        {
            struct stat st;
            PS_CHECK("quarantine: file exists pre-corrupt",
                     stat(fpath, &st) == 0 && st.st_size > 4096);
            FILE *f = fopen(fpath, "r+b");
            PS_CHECK("quarantine: open file for corruption", f != NULL);
            if (f) {
                /* Overwrite from offset 4096 with 0xEE garbage across the
                 * second page region so a b-tree page is clobbered. */
                long start = 4096;
                long span = st.st_size - start;
                if (span > 4096) span = 4096;  /* one page is plenty */
                if (span < 0) span = 0;
                int seek_ok = (fseek(f, start, SEEK_SET) == 0);
                PS_CHECK("quarantine: seek into file body", seek_ok);
                uint8_t garbage[4096];
                memset(garbage, 0xEE, sizeof(garbage));
                size_t to_write = (size_t)span;
                size_t wrote = fwrite(garbage, 1,
                                      to_write < sizeof(garbage)
                                          ? to_write : sizeof(garbage), f);
                PS_CHECK("quarantine: wrote garbage page", wrote > 0);
                fclose(f);
            }
        }

        /* (b) Reopen the CORRUPT file — quick_check must fire the quarantine
         *     and reopen a fresh store. open() returns true (self-healed). */
        PS_CHECK("quarantine: corrupt reopen self-heals (returns true)",
                 progress_store_open(dir));
        PS_CHECK("quarantine: handle non-NULL after self-heal",
                 progress_store_db() != NULL);
        PS_CHECK("quarantine: .corrupt sidecar was created",
                 ps_count_corrupt(dir) >= 1);
        /* Fresh store is queryable (schema re-created) ... */
        {
            sqlite3_stmt *q = NULL;
            int rc = sqlite3_prepare_v2(progress_store_db(),
                "SELECT COUNT(*) FROM stage_cursor", -1, &q, NULL);
            PS_CHECK("quarantine: fresh store stage_cursor queryable",
                     rc == SQLITE_OK);
            sqlite3_finalize(q);
        }
        /* ... and EMPTY: the pre-corrupt marker is GONE (state was derived,
         * the snapshot/anchor is the real source of truth). */
        {
            uint8_t out[64] = {0};
            size_t got = 99; bool found = true;
            PS_CHECK("quarantine: fresh store dropped stale marker",
                     progress_meta_get(progress_store_db(), "k.marker",
                                       out, sizeof(out), &got, &found) &&
                     !found && got == 0);
        }
        progress_store_close();

        /* (c) A second reopen of the now-healthy fresh store does NOT create a
         *     further .corrupt file (auto-terminating; one quarantine). */
        {
            int before = ps_count_corrupt(dir);
            PS_CHECK("quarantine: post-heal reopen OK",
                     progress_store_open(dir));
            PS_CHECK("quarantine: no second quarantine (idempotent)",
                     ps_count_corrupt(dir) == before);
            progress_store_close();
        }

        test_cleanup_tmpdir(dir);
    }

    printf("progress_store: %d failures\n", failures);
    return failures;
}
