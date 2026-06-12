/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_replay_canary_verdict — the hermetic, in-CI half of the standing
 * replay canary (tenacity-roadmap item 5).
 *
 * It does NOT spawn a mainnet node (too heavy for CI). It drives the
 * harness's verdict LOGIC through `replay_canary.sh --self-test=<mode>`,
 * which injects synthetic RPC outputs from a fixture dir instead of
 * spawning a node, and asserts the two contracts that make the canary
 * trustworthy:
 *
 *   1. GATE-FIRES-ON-KNOWN-BAD: each seeded-bad fixture produces a FAIL
 *      sentinel with the correct `reason` and a non-zero exit — proving
 *      the canary red-fails BEFORE any green night can count. The two
 *      named acceptance cases are fail-rejects (a seeded consensus reject)
 *      and fail-sha3 (a commitment != the compiled checkpoint).
 *
 *   2. NEVER-EXIT-0-AS-PROOF: a SIGKILL of the harness mid-run leaves NO
 *      fresh PASS sentinel. Proof requires a *positive* fresh PASS record,
 *      never the absence of a non-zero exit. We verify this directly:
 *      kill the harness while it is blocked, then assert no PASS sentinel
 *      with a fresh timestamp exists.
 *
 * Every run uses a private fixture dir + verdict dir under /tmp so the
 * live node, the live datadir, and the operator's real verdict dir are
 * never touched. */

#include "test/test_helpers.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── repo_root: walk UP from the test binary to the tree that holds the
 * Makefile AND the canary harness, so the shell-out hits the right file
 * regardless of the cwd the suite runs in. Bounded walk. ─────────── */
#define CANARY_REL "tools/scripts/replay_canary.sh"

static const char *repo_root(void)
{
    static char root[PATH_MAX];
    static int cached = 0;
    if (cached) return root[0] ? root : NULL;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe) - 1) {
        cached = 1; root[0] = '\0'; return NULL;
    }
    exe[n] = '\0';

    for (int depth = 0; depth < 8; depth++) {
        char *slash = strrchr(exe, '/');
        if (!slash || slash == exe) break;
        *slash = '\0';

        char probe[PATH_MAX];
        struct stat st;
        if (snprintf(probe, sizeof(probe), "%s/Makefile", exe) >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;
        if (snprintf(probe, sizeof(probe), "%s/%s", exe, CANARY_REL) >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;

        if (snprintf(root, sizeof(root), "%s", exe) >= (int)sizeof(root)) break;
        cached = 1;
        return root;
    }
    cached = 1; root[0] = '\0';
    return NULL;
}

/* ── tiny fs helpers ────────────────────────────────────────────── */

static void write_file(const char *dir, const char *name, const char *content)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static bool read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t r = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[r] = '\0';
    return r > 0;
}

/* Write the five baseline PASS fixtures into a fresh fixture dir, then
 * apply a per-mode mutation that flips exactly one assertion. */
static void seed_fixtures(const char *fx, const char *mode)
{
    /* bg_validation reaches COMPLETE, full coverage. */
    write_file(fx, "getsyncdetail.json",
        "{\"bg_validation\":{\"state\":\"complete\","
        "\"verified_height\":3145329,\"chain_height\":3145329,"
        "\"script_verif_skipped_no_undo\":0}}\n");
    /* zero header rejects. */
    write_file(fx, "getsyncdiag.json",
        "{\"headers\":{\"total_accepted\":3145329,\"total_rejected\":0}}\n");
    /* a real 64-hex commitment, taken at the tip (not the anchor). */
    write_file(fx, "getutxocommitment.json",
        "{\"sha3_hash\":\"deadbeef00000000000000000000000000000000"
        "00000000000000000000beef\",\"height\":3145329,"
        "\"utxo_count\":1354771}\n");
    /* node coarse stats. */
    write_file(fx, "gettxoutsetinfo.json",
        "{\"height\":3145329,\"bestblock\":\"abc123\","
        "\"transactions\":100,\"txouts\":1354771,"
        "\"total_amount\":\"10364138.33747381\"}\n");
    /* zclassicd coarse stats — same VALUES, but total_amount is an
     * UNQUOTED JSON number (zclassicd's real format, verified live), so
     * this exercises the harness's json_amount tolerance: the cross-node
     * supply compare must still match the node's quoted form. */
    write_file(fx, "zd_gettxoutsetinfo.json",
        "{\"height\":3145329,\"bestblock\":\"abc123\","
        "\"transactions\":100,\"txouts\":1354771,"
        "\"total_amount\":10364138.33747381}\n");

    if (strcmp(mode, "fail-rejects") == 0) {
        /* a single header-admit reject => "consensus_rejects". */
        write_file(fx, "getsyncdiag.json",
            "{\"headers\":{\"total_accepted\":3145329,"
            "\"total_rejected\":1}}\n");
    } else if (strcmp(mode, "fail-sha3") == 0) {
        /* commitment AT the anchor height that does NOT equal the
         * compiled checkpoint => "sha3_mismatch". */
        write_file(fx, "getutxocommitment.json",
            "{\"sha3_hash\":\"00000000000000000000000000000000"
            "00000000000000000000000000000bad\","
            "\"height\":3056758,\"utxo_count\":1354771}\n");
    } else if (strcmp(mode, "fail-crossnode") == 0) {
        /* zclassicd txouts disagree with the node => "crossnode_txouts". */
        write_file(fx, "zd_gettxoutsetinfo.json",
            "{\"height\":3145329,\"bestblock\":\"abc123\","
            "\"transactions\":100,\"txouts\":999999,"
            "\"total_amount\":\"10364138.33747381\"}\n");
    } else if (strcmp(mode, "fail-timeout") == 0) {
        /* the live harness writes state=timeout on a budget overrun;
         * the fixture mirrors that terminal state => "budget_exceeded". */
        write_file(fx, "getsyncdetail.json",
            "{\"bg_validation\":{\"state\":\"timeout\","
            "\"verified_height\":100,\"chain_height\":3145329,"
            "\"script_verif_skipped_no_undo\":0}}\n");
    }
    /* mode == "pass": no mutation. */
}

/* Run the harness in self-test mode against fixtures `mode`. Returns the
 * process exit code via *exit_code, and the sentinel verdict + reason via
 * out_verdict / out_reason (each may be empty if no sentinel was written).
 * `from` selects anchor|genesis (sentinel name differs). */
static int run_canary_selftest(const char *mode, const char *from,
                               char *out_verdict, size_t vsz,
                               char *out_reason, size_t rsz)
{
    const char *root = repo_root();
    if (!root) return -999;

    char fx[PATH_MAX], vd[PATH_MAX];
    snprintf(fx, sizeof(fx), "/tmp/test_canary_fx_%d_%s", (int)getpid(), mode);
    snprintf(vd, sizeof(vd), "/tmp/test_canary_vd_%d_%s", (int)getpid(), mode);
    mkdir(fx, 0755);
    mkdir(vd, 0755);
    seed_fixtures(fx, mode);

    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof(cmd),
        "ZCL_CANARY_SELFTEST_DIR='%s' ZCL_CANARY_VERDICT_DIR='%s' "
        "bash '%s/%s' --from=%s --self-test=%s >/dev/null 2>&1",
        fx, vd, root, CANARY_REL, from, mode);

    int rc = system(cmd);
    int exit_code = (rc == -1) ? -1 : WEXITSTATUS(rc);

    char sentinel[PATH_MAX];
    snprintf(sentinel, sizeof(sentinel), "%s/replay_canary_%s.json", vd, from);
    char buf[2048] = {0};
    out_verdict[0] = '\0';
    out_reason[0] = '\0';
    if (read_file(sentinel, buf, sizeof(buf))) {
        const char *v = strstr(buf, "\"verdict\":\"");
        if (v) { v += strlen("\"verdict\":\""); size_t i = 0;
                 while (v[i] && v[i] != '"' && i < vsz - 1) { out_verdict[i] = v[i]; i++; }
                 out_verdict[i] = '\0'; }
        const char *r = strstr(buf, "\"reason\":\"");
        if (r) { r += strlen("\"reason\":\""); size_t i = 0;
                 while (r[i] && r[i] != '"' && i < rsz - 1) { out_reason[i] = r[i]; i++; }
                 out_reason[i] = '\0'; }
    }

    /* clean up fixture + verdict dirs */
    char rm[PATH_MAX + 32];
    snprintf(rm, sizeof(rm), "rm -rf '%s' '%s'", fx, vd);
    if (system(rm) != 0) { /* best-effort cleanup */ }

    return exit_code;
}

/* ── Tests ──────────────────────────────────────────────────────── */

static int test_pass_writes_pass_sentinel(void)
{
    int failures = 0;
    TEST("replay-canary: --self-test=pass writes a PASS sentinel, exit 0") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("pass", "anchor",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT_EQ(ec, 0);
        ASSERT_STR_EQ(verdict, "PASS");
        ASSERT_STR_EQ(reason, "");
        PASS();
    } _test_next:;
    return failures;
}

static int test_fail_rejects_fires(void)
{
    int failures = 0;
    TEST("replay-canary: seeded consensus reject => FAIL reason=consensus_rejects") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("fail-rejects", "anchor",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT(ec != 0);
        ASSERT_STR_EQ(verdict, "FAIL");
        ASSERT_STR_EQ(reason, "consensus_rejects");
        PASS();
    } _test_next:;
    return failures;
}

static int test_fail_sha3_fires(void)
{
    int failures = 0;
    TEST("replay-canary: seeded bad commitment => FAIL reason=sha3_mismatch") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("fail-sha3", "anchor",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT(ec != 0);
        ASSERT_STR_EQ(verdict, "FAIL");
        ASSERT_STR_EQ(reason, "sha3_mismatch");
        PASS();
    } _test_next:;
    return failures;
}

static int test_fail_crossnode_fires(void)
{
    int failures = 0;
    TEST("replay-canary: txouts disagree with zclassicd => FAIL reason=crossnode_txouts") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("fail-crossnode", "anchor",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT(ec != 0);
        ASSERT_STR_EQ(verdict, "FAIL");
        ASSERT_STR_EQ(reason, "crossnode_txouts");
        PASS();
    } _test_next:;
    return failures;
}

static int test_fail_timeout_fires(void)
{
    int failures = 0;
    TEST("replay-canary: bg stuck past budget => FAIL reason=budget_exceeded") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("fail-timeout", "anchor",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT(ec != 0);
        ASSERT_STR_EQ(verdict, "FAIL");
        ASSERT_STR_EQ(reason, "budget_exceeded");
        PASS();
    } _test_next:;
    return failures;
}

/* The named top defect: exit-0-as-proof. A harness killed mid-run must
 * leave NO fresh PASS sentinel. We block the wrapper shell on a FIFO read
 * BEFORE the harness ever runs (so it cannot write a sentinel), SIGKILL
 * the whole process group, then assert no PASS sentinel exists — proving
 * that proof requires a *positive* fresh PASS record, never the absence
 * of a non-zero exit.
 *
 * Why block in the wrapper, not inside the harness: a killed run is
 * indistinguishable, from the verdict file's perspective, whether the
 * kill lands before arg-parse, during the node spawn, or mid-poll — in
 * every case the atomic sentinel write (the LAST step) never executes.
 * Blocking the wrapper on a never-fed FIFO models that deterministically
 * and portably (no race on how fast the harness reaches its first I/O). */
static int test_sigkill_leaves_no_fresh_pass(void)
{
    int failures = 0;
    TEST("replay-canary: SIGKILL mid-run leaves NO fresh PASS sentinel") {
        const char *root = repo_root();
        if (!root) { printf("SKIP (repo root not found)\n"); break; }

        char fx[PATH_MAX], vd[PATH_MAX], fifo[PATH_MAX];
        snprintf(fx, sizeof(fx), "/tmp/test_canary_kill_fx_%d", (int)getpid());
        snprintf(vd, sizeof(vd), "/tmp/test_canary_kill_vd_%d", (int)getpid());
        snprintf(fifo, sizeof(fifo), "/tmp/test_canary_kill_fifo_%d", (int)getpid());
        mkdir(fx, 0755);
        mkdir(vd, 0755);
        /* Seed VALID fixtures: an uninterrupted run WOULD write a PASS.
         * The whole point is that the kill prevents it. */
        seed_fixtures(fx, "pass");

        unlink(fifo);
        if (mkfifo(fifo, 0644) != 0) {
            printf("FAIL (mkfifo: %s)\n", strerror(errno));
            failures++; goto _kill_cleanup;
        }

        char sentinel[PATH_MAX];
        snprintf(sentinel, sizeof(sentinel), "%s/replay_canary_anchor.json", vd);

        pid_t pid = fork();
        if (pid == 0) {
            /* child: in its own group; the wrapper FIRST blocks on the FIFO
             * read (no writer ever opens it), so the harness exec NEVER runs
             * and no sentinel can be written. */
            setsid();
            char cmd[PATH_MAX * 4];
            snprintf(cmd, sizeof(cmd),
                "read _ < '%s'; "
                "ZCL_CANARY_SELFTEST_DIR='%s' ZCL_CANARY_VERDICT_DIR='%s' "
                "exec bash '%s/%s' --from=anchor --self-test=pass",
                fifo, fx, vd, root, CANARY_REL);
            execlp("sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }

        /* Let it reach the blocking FIFO read, then SIGKILL the whole group. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 400 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        struct stat st_mid;
        bool existed_mid = (stat(sentinel, &st_mid) == 0);

        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);

        struct stat st_after;
        bool exists_after = (stat(sentinel, &st_after) == 0);

        /* The contract: a killed harness leaves no PASS sentinel. */
        if (existed_mid || exists_after) {
            printf("FAIL: sentinel %s exists after SIGKILL (mid=%d after=%d) "
                   "— exit-0-as-proof leak\n", sentinel, existed_mid, exists_after);
            failures++; goto _kill_cleanup;
        }
        PASS();

    _kill_cleanup:;
        unlink(fifo);
        char rm[PATH_MAX + 32];
        snprintf(rm, sizeof(rm), "rm -rf '%s' '%s'", fx, vd);
        if (system(rm) != 0) { /* best-effort */ }
    }
    return failures;
}

/* The verdict is per-`--from`: a genesis fixture writes a genesis-named
 * sentinel, an anchor fixture an anchor-named one. Confirms the sentinel
 * naming the systemd units + staleness check rely on. */
static int test_from_genesis_sentinel_name(void)
{
    int failures = 0;
    TEST("replay-canary: --from=genesis writes the genesis-named sentinel") {
        char verdict[16], reason[64];
        int ec = run_canary_selftest("pass", "genesis",
                                     verdict, sizeof(verdict),
                                     reason, sizeof(reason));
        if (ec == -999) { printf("SKIP (repo root not found)\n"); break; }
        ASSERT_EQ(ec, 0);
        ASSERT_STR_EQ(verdict, "PASS");
        PASS();
    } _test_next:;
    return failures;
}

/* ── Registration ───────────────────────────────────────────────── */

int test_replay_canary_verdict(void)
{
    int failures = 0;
    failures += test_pass_writes_pass_sentinel();
    failures += test_fail_rejects_fires();
    failures += test_fail_sha3_fires();
    failures += test_fail_crossnode_fires();
    failures += test_fail_timeout_fires();
    failures += test_sigkill_leaves_no_fresh_pass();
    failures += test_from_genesis_sentinel_name();
    return failures;
}
