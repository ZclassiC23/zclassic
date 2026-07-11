/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_vcs_devloop — the dev-loop <-> ZVCS auto-anchor glue
 * (lib/vcs/src/vcs_devloop.c), the function tools/dev/devloop_cycle.c:
 * finish_cycle() calls on every "passed" verdict.
 *
 * Coverage:
 *   1. hex32_decode: valid decode + NULL/short-length/non-hex rejection.
 *   2. a finish_cycle-shaped anchor call (verdict struct + generation hex +
 *      repo root) lands a commit whose verdict fields and generation
 *      binding round-trip exactly, including the "generation unknown"
 *      (e.g. a docs-only "check" cycle) all-zero case.
 *   3. fail-open: a repo whose .zvcs/ cannot be created (a regular file
 *      occupies that path) returns VCS_DEVLOOP_ANCHOR_ERROR with a message,
 *      never crashes.
 *   4. sealed-path refusal: editing a sealed file surfaces
 *      VCS_DEVLOOP_ANCHOR_REFUSED without crashing, and does not advance
 *      HEAD (nothing commits for that cycle).
 *
 * All work happens under ./test-tmp/ (project no-/tmp convention). */

#include "test/test_helpers.h"

#include "vcs/vcs.h"
#include "vcs/vcs_devloop.h"
#include "vcs/vcs_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VD_CHECK(name, expr) do {                                       \
    if (expr) { printf("  vcs_devloop: %s... OK\n", (name)); }          \
    else { printf("  vcs_devloop: %s... FAIL\n", (name)); failures++; } \
} while (0)

/* Write content to <dir>/<rel>, creating parent dirs. */
static bool vd_write(const char *dir, const char *rel, const char *content)
{
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir, rel);
    for (char *p = full + strlen(dir) + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(full, 0755);
            *p = '/';
        }
    }
    FILE *f = fopen(full, "wb");
    if (!f) return false;
    size_t n = content ? strlen(content) : 0;
    if (n) fwrite(content, 1, n, f);
    fclose(f);
    return true;
}

/* Deterministic 64-hex-char string derived from `seed` (avoids hand-counted
 * hex literals). */
static void make_hex64(char out[65], uint8_t seed)
{
    for (int i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", (uint8_t)(seed + i));
    out[64] = 0;
}

struct captured_commit {
    struct vcs_commit c;
    uint8_t            id[32];
    bool                got;
};

static bool capture_first_commit(const struct vcs_commit *c,
                                 const uint8_t commit_id[32], void *user)
{
    struct captured_commit *cap = user;
    cap->c = *c;
    memcpy(cap->id, commit_id, 32);
    cap->got = true;
    return false; /* newest-first log: stop after the first one */
}

/* ── test: hex32_decode ─────────────────────────────────────────── */
static int t_hex32(void)
{
    int failures = 0;

    char hex[65];
    make_hex64(hex, 0x10);
    uint8_t out[32];
    VD_CHECK("hex32: valid decode", vcs_devloop_hex32_decode(hex, out));
    VD_CHECK("hex32: first byte", out[0] == 0x10);
    VD_CHECK("hex32: last byte", out[31] == (uint8_t)(0x10 + 31));

    uint8_t scratch[32];
    VD_CHECK("hex32: NULL hex rejected",
             !vcs_devloop_hex32_decode(NULL, scratch));
    VD_CHECK("hex32: NULL out rejected",
             !vcs_devloop_hex32_decode(hex, NULL));
    VD_CHECK("hex32: short length rejected",
             !vcs_devloop_hex32_decode("abcd", scratch));

    char bad[65];
    make_hex64(bad, 0);
    bad[10] = 'z';
    VD_CHECK("hex32: non-hex char rejected",
             !vcs_devloop_hex32_decode(bad, scratch));

    return failures;
}

/* ── test: a green cycle lands a correctly-bound commit ───────────── */
static int t_anchor_ok(const char *dir)
{
    int failures = 0;

    vd_write(dir, "readme.txt", "hello\n");
    vd_write(dir, "src/main.c", "int main(void){return 0;}\n");

    char hex[65];
    make_hex64(hex, 0x42);

    struct vcs_devloop_verdict v = {0};
    v.verdict_status = 0;
    v.phase = "resident_commit";
    v.elapsed_ms = 1234;
    v.generation_hex = hex;
    v.agent_id = "agent-fable";
    v.session_id = "sess-abc";
    v.task_ref = "task/zvcs-2.3";

    struct vcs_devloop_anchor_result ar = {0};
    vcs_devloop_anchor_cycle(dir, &v, &ar);
    VD_CHECK("anchor: status OK", ar.status == VCS_DEVLOOP_ANCHOR_OK);

    struct vcs_repo *r = vcs_open(dir);
    VD_CHECK("anchor: reopen", r != NULL);
    if (!r) return failures + 1;

    struct captured_commit cap = {0};
    VD_CHECK("anchor: vcs_log ok",
             vcs_log(r, 1, capture_first_commit, &cap) == VCS_OK);
    VD_CHECK("anchor: commit captured", cap.got);
    VD_CHECK("anchor: commit id matches result",
             cap.got && memcmp(cap.id, ar.commit_id, 32) == 0);
    VD_CHECK("anchor: verdict_status bound",
             cap.got && cap.c.verdict_status == 0);
    VD_CHECK("anchor: phase bound",
             cap.got && strcmp(cap.c.phase, "resident_commit") == 0);
    VD_CHECK("anchor: elapsed_ms bound",
             cap.got && cap.c.elapsed_ms == 1234);
    uint8_t want_gen[32];
    vcs_devloop_hex32_decode(hex, want_gen);
    VD_CHECK("anchor: generation_sha256 bound",
             cap.got && memcmp(cap.c.generation_sha256, want_gen, 32) == 0);
    VD_CHECK("anchor: agent_id bound",
             cap.got && strcmp(cap.c.agent_id, "agent-fable") == 0);
    VD_CHECK("anchor: session_id bound",
             cap.got && strcmp(cap.c.session_id, "sess-abc") == 0);
    VD_CHECK("anchor: task_ref bound",
             cap.got && strcmp(cap.c.task_ref, "task/zvcs-2.3") == 0);
    vcs_close(r);

    /* A cycle with no known generation (e.g. a docs-only "check" cycle, or
     * a reload whose deploy-state file did not parse) still anchors — with
     * an all-zero generation_sha256, never a failure. */
    vd_write(dir, "readme.txt", "hello again\n");
    struct vcs_devloop_verdict v2 = {0};
    v2.verdict_status = 0;
    v2.phase = "check";
    v2.elapsed_ms = 5;
    struct vcs_devloop_anchor_result ar2 = {0};
    vcs_devloop_anchor_cycle(dir, &v2, &ar2);
    VD_CHECK("anchor: generation-absent cycle still OK",
             ar2.status == VCS_DEVLOOP_ANCHOR_OK);

    r = vcs_open(dir);
    struct captured_commit cap2 = {0};
    vcs_log(r, 1, capture_first_commit, &cap2);
    uint8_t zero32[32] = {0};
    VD_CHECK("anchor: absent generation binds all-zero",
             cap2.got && memcmp(cap2.c.generation_sha256, zero32, 32) == 0);
    vcs_close(r);

    return failures;
}

/* lib/vcs never spawns a process to run the initial baseline (ZVCS
 * sovereignty — check-vcs-no-git); the detach mechanics live only in the
 * dev-only tools/dev/devloop_baseline.c, which this test suite does not
 * link. So this test drives both halves in-process and hermetically: (1)
 * the anchor path reports the baseline as REQUIRED without spawning
 * anything, then (2) vcs_devloop_run_initial_baseline() is called directly
 * to run it synchronously, and the next cycle anchors normally. No child
 * process, no polling loop. */
static int t_initial_anchor_deferred(const char *dir)
{
    int failures = 0;
    vd_write(dir, "src/main.c", "int main(void){return 0;}\n");

    struct vcs_devloop_verdict v = {0};
    v.phase = "resident_commit";
    v.defer_initial_snapshot = true;
    struct vcs_devloop_anchor_result first = {0};
    vcs_devloop_anchor_cycle(dir, &v, &first);
    VD_CHECK("deferred: first cycle leaves foreground",
             first.status == VCS_DEVLOOP_ANCHOR_DEFERRED);
    VD_CHECK("deferred: result names unanchored cycle",
             strstr(first.error, "unanchored") != NULL);
    VD_CHECK("deferred: caller told a baseline is required",
             first.baseline_needed);

    /* A second concurrent cycle before anyone has run the baseline still
     * reports baseline_needed — nothing has claimed it yet (no lock is
     * held, since lib/vcs never launches anything on its own). */
    struct vcs_devloop_anchor_result concurrent = {0};
    vcs_devloop_anchor_cycle(dir, &v, &concurrent);
    VD_CHECK("deferred: second cycle also unanchored",
             concurrent.status == VCS_DEVLOOP_ANCHOR_DEFERRED);
    VD_CHECK("deferred: second cycle also told a baseline is required",
             concurrent.baseline_needed);

    /* Run the baseline synchronously, in-process — this is exactly what
     * tools/dev/devloop_baseline.c's grandchild worker calls. */
    struct vcs_devloop_anchor_result baseline = {0};
    vcs_devloop_run_initial_baseline(dir, &baseline);
    VD_CHECK("deferred: synchronous baseline completes",
             baseline.status == VCS_DEVLOOP_ANCHOR_OK);

    /* Once history exists, a defer_initial_snapshot cycle anchors normally
     * rather than deferring again. */
    struct vcs_devloop_anchor_result next = {0};
    vcs_devloop_anchor_cycle(dir, &v, &next);
    VD_CHECK("deferred: next warm cycle anchors synchronously",
             next.status == VCS_DEVLOOP_ANCHOR_OK);

    /* Once durable history exists, re-running the baseline directly is a
     * clean no-op path (lock acquires immediately, snapshot lands — no
     * "already running" collision since nothing else holds the lock). */
    struct vcs_devloop_anchor_result rerun = {0};
    vcs_devloop_run_initial_baseline(dir, &rerun);
    VD_CHECK("deferred: re-running baseline after history exists still OK",
             rerun.status == VCS_DEVLOOP_ANCHOR_OK);

    return failures;
}

/* ── test: fail-open when .zvcs/ cannot be created ─────────────────── */
static int t_fail_open(const char *dir)
{
    int failures = 0;

    vd_write(dir, "readme.txt", "hi\n");

    /* Block .zvcs/ from ever being created: put a regular file where the
     * directory needs to go, so vcs_object_store_init's mkdir() fails with
     * ENOTDIR (not EEXIST) and vcs_open() returns NULL. */
    char blocker[4096];
    snprintf(blocker, sizeof(blocker), "%s/.zvcs", dir);
    FILE *f = fopen(blocker, "wb");
    VD_CHECK("fail-open: blocker file created", f != NULL);
    if (f) fclose(f);

    struct vcs_devloop_verdict v = {0};
    v.verdict_status = 0;
    v.phase = "resident_commit";
    struct vcs_devloop_anchor_result ar = {0};
    vcs_devloop_anchor_cycle(dir, &v, &ar);
    VD_CHECK("fail-open: status ERROR (not a crash)",
             ar.status == VCS_DEVLOOP_ANCHOR_ERROR);
    VD_CHECK("fail-open: error message set", ar.error[0] != '\0');

    unlink(blocker);
    return failures;
}

/* ── test: sealed-path change is refused, and does not advance HEAD ── */
static int t_sealed_refusal(const char *dir)
{
    int failures = 0;

    vd_write(dir, ".zvcs/sealed_paths", "sealed/\n");
    vd_write(dir, "sealed/consensus.txt", "RULE=1\n");
    vd_write(dir, "readme.txt", "hi\n");

    struct vcs_devloop_verdict v = {0};
    v.verdict_status = 0;
    v.phase = "resident_commit";

    struct vcs_devloop_anchor_result ar1 = {0};
    vcs_devloop_anchor_cycle(dir, &v, &ar1);
    VD_CHECK("sealed: initial anchor pins OK",
             ar1.status == VCS_DEVLOOP_ANCHOR_OK);

    /* Edit the sealed file -> the next anchor is refused, not crashed. */
    vd_write(dir, "sealed/consensus.txt", "RULE=2\n");
    struct vcs_devloop_anchor_result ar2 = {0};
    vcs_devloop_anchor_cycle(dir, &v, &ar2);
    VD_CHECK("sealed: refusal surfaced",
             ar2.status == VCS_DEVLOOP_ANCHOR_REFUSED);
    VD_CHECK("sealed: refusal error set", ar2.error[0] != '\0');

    /* HEAD did not move: the refused cycle recorded nothing. */
    struct vcs_repo *r = vcs_open(dir);
    VD_CHECK("sealed: reopen", r != NULL);
    if (r) {
        uint8_t head[32];
        bool found = false;
        bool got_head = vcs_index_ref_get(vcs_repo_index(r), "HEAD", head,
                                          &found);
        VD_CHECK("sealed: HEAD readable", got_head && found);
        VD_CHECK("sealed: HEAD unchanged (refusal did not commit)",
                 got_head && found &&
                 memcmp(head, ar1.commit_id, 32) == 0);
        vcs_close(r);
    }

    return failures;
}

int test_vcs_devloop(void)
{
    printf("\n=== vcs_devloop: dev-loop auto-anchor glue ===\n");
    int failures = 0;

    failures += t_hex32();

    char dir[512];

    test_make_tmpdir(dir, sizeof(dir), "vcs_devloop", "ok");
    failures += t_anchor_ok(dir);
    test_rm_rf_recursive(dir);

    test_make_tmpdir(dir, sizeof(dir), "vcs_devloop", "deferred");
    failures += t_initial_anchor_deferred(dir);
    test_rm_rf_recursive(dir);

    test_make_tmpdir(dir, sizeof(dir), "vcs_devloop", "failopen");
    failures += t_fail_open(dir);
    test_rm_rf_recursive(dir);

    test_make_tmpdir(dir, sizeof(dir), "vcs_devloop", "sealed");
    failures += t_sealed_refusal(dir);
    test_rm_rf_recursive(dir);

    printf("=== vcs_devloop complete: %d failure(s) ===\n", failures);
    return failures;
}
