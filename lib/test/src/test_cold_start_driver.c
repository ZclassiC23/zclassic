/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * -cold-start staged, resumable driver (config/boot_cold_start.h).
 *
 * Proves the properties that make the one-command cold start RESUMABLE and
 * correctly ORDERED, without spawning a real node:
 *
 *   (a) plan configuration + stage naming/params.
 *   (b) receipt round-trip: write is durable (temp file gone, receipt present,
 *       fields correct) and read matches only on the SAME bound parameter.
 *   (c) resume decision: cold_start_plan_next() returns the first configured
 *       prep stage whose receipt is absent; a present receipt is skipped; a
 *       parameter-changed receipt is NOT skipped.
 *   (d) drive loop with an injected recording runner: a fresh datadir runs
 *       [headers, seed] then reaches SERVE; after a simulated kill following
 *       stage 1 (only the headers receipt on disk), a re-drive runs ONLY [seed]
 *       — completed stages are skipped via their receipts.
 *   (e) drive halts on a stage-runner failure WITHOUT writing that stage's
 *       receipt, so the failed stage is retried on the next run.
 *
 * Hermetic: temp datadirs only, no child processes, no node boot. */

#include "test/test_helpers.h"
#include "config/boot_cold_start.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* True iff the file at `path` contains `needle` (small receipt files). */
static bool file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* ── Injected recording runner ──────────────────────────────────────── */

struct fake_runner {
    enum cold_start_stage ran[8];
    int n;
    enum cold_start_stage fail_on; /* run this stage returns non-zero */
    bool use_fail;
};

static int fake_run_stage(const struct cold_start_plan *plan,
                          enum cold_start_stage stage, void *user)
{
    (void)plan;
    struct fake_runner *f = (struct fake_runner *)user;
    if (f->n < (int)(sizeof(f->ran) / sizeof(f->ran[0])))
        f->ran[f->n++] = stage;
    if (f->use_fail && stage == f->fail_on)
        return 7; /* simulate a stage failure */
    return 0;     /* success => driver writes the receipt */
}

static bool made_tmp(char *dir /* size >= 32 */)
{
    strcpy(dir, "/tmp/zcl_coldstart_XXXXXX");
    return mkdtemp(dir) != NULL;
}

/* rm -rf on a small, known coldstart tree (receipts + dir). */
static void cleanup_tree(const char *datadir)
{
    char p[512];
    const enum cold_start_stage prep[] = { COLD_START_STAGE_HEADERS,
                                           COLD_START_STAGE_SEED,
                                           COLD_START_STAGE_BUNDLE };
    for (size_t i = 0; i < sizeof(prep) / sizeof(prep[0]); i++) {
        if (cold_start_receipt_path(datadir, prep[i], p, sizeof(p)) > 0)
            unlink(p);
    }
    snprintf(p, sizeof(p), "%s/coldstart", datadir);
    rmdir(p);
    rmdir(datadir);
}

int test_cold_start_driver(void)
{
    int failures = 0;

    /* (a) plan configuration + stage naming/params. */
    printf("cold_start plan configuration + stage names ... ");
    {
        struct cold_start_plan plan = {
            .datadir = "/tmp/x", .header_source = "/src",
            .seed_snapshot = "/seed.snap", .install_bundle = NULL,
        };
        bool ok =
            strcmp(cold_start_stage_name(COLD_START_STAGE_HEADERS), "headers") == 0 &&
            strcmp(cold_start_stage_name(COLD_START_STAGE_SEED), "seed") == 0 &&
            strcmp(cold_start_stage_name(COLD_START_STAGE_BUNDLE), "bundle") == 0 &&
            strcmp(cold_start_stage_name(COLD_START_STAGE_SERVE), "serve") == 0 &&
            cold_start_stage_configured(&plan, COLD_START_STAGE_HEADERS) &&
            cold_start_stage_configured(&plan, COLD_START_STAGE_SEED) &&
            !cold_start_stage_configured(&plan, COLD_START_STAGE_BUNDLE) &&
            cold_start_stage_configured(&plan, COLD_START_STAGE_SERVE) &&
            strcmp(cold_start_stage_param(&plan, COLD_START_STAGE_HEADERS), "/src") == 0 &&
            cold_start_stage_param(&plan, COLD_START_STAGE_BUNDLE) == NULL &&
            cold_start_stage_param(&plan, COLD_START_STAGE_SERVE) == NULL;
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* (b) receipt round-trip: durable write, correct fields, parameter match. */
    printf("cold_start receipt write is durable + parameter-bound ... ");
    {
        char dir[64];
        bool ok = made_tmp(dir);
        if (ok) {
            ok &= cold_start_receipt_write(dir, COLD_START_STAGE_HEADERS,
                                           "/home/.zclassic");
            char rpath[512], tmp[600];
            ok &= cold_start_receipt_path(dir, COLD_START_STAGE_HEADERS, rpath,
                                          sizeof(rpath)) > 0;
            /* The receipt exists; the temp file does NOT (atomic rename). */
            ok &= access(rpath, F_OK) == 0;
            snprintf(tmp, sizeof(tmp), "%s.tmp", rpath);
            ok &= access(tmp, F_OK) != 0;
            ok &= file_contains(rpath, "magic=ZCLCOLDSTART\n");
            ok &= file_contains(rpath, "stage=headers\n");
            ok &= file_contains(rpath, "has_param=1\n");
            ok &= file_contains(rpath, "param=/home/.zclassic\n");
            /* Matches only on the same bound parameter. */
            ok &= cold_start_receipt_matches(dir, COLD_START_STAGE_HEADERS,
                                             "/home/.zclassic");
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_HEADERS,
                                              "/home/.other");
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_HEADERS,
                                              NULL);
            /* A never-written stage never matches. */
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_SEED,
                                              "/seed.snap");
            /* A parameter-less stage receipt matches NULL only. */
            ok &= cold_start_receipt_write(dir, COLD_START_STAGE_BUNDLE, NULL);
            ok &= cold_start_receipt_matches(dir, COLD_START_STAGE_BUNDLE, NULL);
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_BUNDLE,
                                              "/b");
            cleanup_tree(dir);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* (c) resume decision via receipts. */
    printf("cold_start plan_next resume decision ... ");
    {
        char dir[64];
        bool ok = made_tmp(dir);
        if (ok) {
            struct cold_start_plan plan = {
                .datadir = dir, .header_source = "/src",
                .seed_snapshot = "/seed.snap", .install_bundle = NULL,
            };
            /* Fresh: no receipts => first configured stage is HEADERS. */
            ok &= cold_start_plan_next(&plan) == COLD_START_STAGE_HEADERS;
            /* After headers receipt: skip to SEED. */
            ok &= cold_start_receipt_write(dir, COLD_START_STAGE_HEADERS, "/src");
            ok &= cold_start_plan_next(&plan) == COLD_START_STAGE_SEED;
            /* After seed receipt: all configured prep done => SERVE (bundle is
             * unconfigured, so it is not required). */
            ok &= cold_start_receipt_write(dir, COLD_START_STAGE_SEED,
                                           "/seed.snap");
            ok &= cold_start_plan_next(&plan) == COLD_START_STAGE_SERVE;
            /* Changing the source parameter re-arms the HEADERS stage — a stale
             * receipt for a different source must NOT be skipped. */
            plan.header_source = "/src2";
            ok &= cold_start_plan_next(&plan) == COLD_START_STAGE_HEADERS;
            cleanup_tree(dir);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* (d) drive loop + resume-after-kill (the core acceptance property). */
    printf("cold_start drive runs stages in order + resumes after a kill ... ");
    {
        char dir[64];
        bool ok = made_tmp(dir);
        if (ok) {
            struct cold_start_plan plan = {
                .datadir = dir, .header_source = "/src",
                .seed_snapshot = "/seed.snap", .install_bundle = NULL,
            };

            /* Full drive on a fresh datadir: runs [headers, seed], reaches
             * SERVE, and leaves both receipts. */
            struct fake_runner f1 = {0};
            enum cold_start_stage reached = COLD_START_STAGE_HEADERS;
            ok &= cold_start_drive(&plan, fake_run_stage, &f1, &reached) == 0;
            ok &= reached == COLD_START_STAGE_SERVE;
            ok &= f1.n == 2 &&
                  f1.ran[0] == COLD_START_STAGE_HEADERS &&
                  f1.ran[1] == COLD_START_STAGE_SEED;

            /* Simulate a KILL after stage 1: keep only the HEADERS receipt,
             * remove the SEED receipt as if the process died mid-seed. */
            char seedr[512];
            ok &= cold_start_receipt_path(dir, COLD_START_STAGE_SEED, seedr,
                                          sizeof(seedr)) > 0;
            unlink(seedr);
            ok &= cold_start_receipt_matches(dir, COLD_START_STAGE_HEADERS,
                                             "/src");
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_SEED,
                                              "/seed.snap");

            /* Re-drive: resume MUST skip the completed headers stage and run
             * ONLY the seed stage, then reach SERVE. */
            struct fake_runner f2 = {0};
            reached = COLD_START_STAGE_HEADERS;
            ok &= cold_start_drive(&plan, fake_run_stage, &f2, &reached) == 0;
            ok &= reached == COLD_START_STAGE_SERVE;
            ok &= f2.n == 1 && f2.ran[0] == COLD_START_STAGE_SEED;

            /* A third drive with everything receipted runs NOTHING. */
            struct fake_runner f3 = {0};
            ok &= cold_start_drive(&plan, fake_run_stage, &f3, &reached) == 0;
            ok &= reached == COLD_START_STAGE_SERVE && f3.n == 0;
            cleanup_tree(dir);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    /* (e) a stage failure halts WITHOUT a receipt (retryable). */
    printf("cold_start drive halts on stage failure without a receipt ... ");
    {
        char dir[64];
        bool ok = made_tmp(dir);
        if (ok) {
            struct cold_start_plan plan = {
                .datadir = dir, .header_source = "/src",
                .seed_snapshot = "/seed.snap",
                .install_bundle = "/bundle.db",
            };
            /* Fail on SEED: headers should complete + receipt; seed runs, fails,
             * no seed receipt; bundle never runs. */
            struct fake_runner f = { .use_fail = true,
                                     .fail_on = COLD_START_STAGE_SEED };
            enum cold_start_stage reached = COLD_START_STAGE_HEADERS;
            int rc = cold_start_drive(&plan, fake_run_stage, &f, &reached);
            ok &= rc == 7;
            ok &= reached == COLD_START_STAGE_SEED;
            ok &= f.n == 2 &&
                  f.ran[0] == COLD_START_STAGE_HEADERS &&
                  f.ran[1] == COLD_START_STAGE_SEED;
            ok &= cold_start_receipt_matches(dir, COLD_START_STAGE_HEADERS,
                                             "/src");
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_SEED,
                                              "/seed.snap");
            ok &= !cold_start_receipt_matches(dir, COLD_START_STAGE_BUNDLE,
                                              "/bundle.db");

            /* Next run (no failure) resumes at SEED and completes to SERVE. */
            struct fake_runner f2 = {0};
            reached = COLD_START_STAGE_HEADERS;
            ok &= cold_start_drive(&plan, fake_run_stage, &f2, &reached) == 0;
            ok &= reached == COLD_START_STAGE_SERVE;
            ok &= f2.n == 2 &&
                  f2.ran[0] == COLD_START_STAGE_SEED &&
                  f2.ran[1] == COLD_START_STAGE_BUNDLE;
            cleanup_tree(dir);
        }
        if (ok) printf("OK\n"); else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
