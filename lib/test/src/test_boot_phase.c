/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the boot-stage state machine (lib/util/src/boot_phase.c).
 *
 * Cannot test misorder paths — boot_stage_advance_to() calls abort() on
 * backward moves or out-of-range targets, which would kill the test
 * runner. Coverage focuses on the legal transitions, idempotent
 * re-advance, name lookup, and the read-only predicates.
 *
 * Uses boot_stage_reset_for_testing() (only available under -DZCL_TESTING)
 * to restore the global stage between sub-tests and at function exit,
 * so the sequential test driver (test.c) can run this alongside other
 * tests without polluting their view of the boot stage. */

#include "test/test_helpers.h"
#include "config/boot.h"
#include "util/boot_phase.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define BP_CHECK(name, expr) do { \
    printf("boot_phase: %s... ", (name)); \
    if ((expr)) printf("OK\n");           \
    else { printf("FAIL\n"); failures++; } \
} while (0)

int test_boot_phase(void)
{
    printf("\n=== boot_phase tests ===\n");
    int failures = 0;

    boot_stage_reset_for_testing();

    /* ── name lookup ────────────────────────────────────────────── */
    BP_CHECK("name(INIT) is \"init\"",
        strcmp(boot_stage_name(BOOT_STAGE_INIT), "init") == 0);
    BP_CHECK("name(DB_OPEN) is \"db_open\"",
        strcmp(boot_stage_name(BOOT_STAGE_DB_OPEN), "db_open") == 0);
    BP_CHECK("name(READY) is \"ready\"",
        strcmp(boot_stage_name(BOOT_STAGE_READY), "ready") == 0);
    BP_CHECK("name(SHUTDOWN_COMPLETE) is \"shutdown_complete\"",
        strcmp(boot_stage_name(BOOT_STAGE_SHUTDOWN_COMPLETE),
               "shutdown_complete") == 0);

    /* Negative and >= MAX targets return "(invalid)". */
    BP_CHECK("name(-1) is \"(invalid)\"",
        strcmp(boot_stage_name((enum boot_stage)-1), "(invalid)") == 0);
    BP_CHECK("name(__MAX) is \"(invalid)\"",
        strcmp(boot_stage_name(BOOT_STAGE__MAX), "(invalid)") == 0);

    /* Every legal enum has a non-empty name (no gaps in the table). */
    {
        bool all_named = true;
        for (int s = 0; s < (int)BOOT_STAGE__MAX; s++) {
            const char *n = boot_stage_name((enum boot_stage)s);
            if (!n || !*n || strcmp(n, "(invalid)") == 0) {
                all_named = false;
                break;
            }
        }
        BP_CHECK("every legal stage has a non-empty name", all_named);
    }

    /* ── current + predicate at INIT ────────────────────────────── */
    BP_CHECK("current() is INIT after reset",
        boot_stage_current() == BOOT_STAGE_INIT);
    BP_CHECK("is(INIT) true at INIT",
        boot_stage_is(BOOT_STAGE_INIT));
    BP_CHECK("is(DB_OPEN) false at INIT",
        !boot_stage_is(BOOT_STAGE_DB_OPEN));

    /* ── idempotent re-advance ──────────────────────────────────── */
    boot_stage_advance_to(BOOT_STAGE_INIT);  /* same stage — no-op */
    BP_CHECK("idempotent re-advance keeps stage at INIT",
        boot_stage_current() == BOOT_STAGE_INIT);

    /* ── forward step ───────────────────────────────────────────── */
    boot_stage_advance_to(BOOT_STAGE_DATADIR_LOCKED);
    BP_CHECK("advance to DATADIR_LOCKED",
        boot_stage_current() == BOOT_STAGE_DATADIR_LOCKED);
    BP_CHECK("is(DATADIR_LOCKED) true after advance",
        boot_stage_is(BOOT_STAGE_DATADIR_LOCKED));
    BP_CHECK("is(INIT) false after advance",
        !boot_stage_is(BOOT_STAGE_INIT));

    boot_stage_advance_to(BOOT_STAGE_CRYPTO_READY);
    boot_stage_advance_to(BOOT_STAGE_DB_OPEN);
    BP_CHECK("step through CRYPTO_READY -> DB_OPEN",
        boot_stage_current() == BOOT_STAGE_DB_OPEN);

    /* ZK params are consensus-critical on mainnet. If the background loader
     * thread cannot even start, boot must name params_missing and park before
     * CRYPTO_READY rather than silently continuing. */
    BP_CHECK("mainnet params thread failure is fatal",
        boot_test_params_thread_failure_is_fatal(true, true, false));
    BP_CHECK("missing params dir has no thread failure",
        !boot_test_params_thread_failure_is_fatal(false, true, false));
    BP_CHECK("mint-anchor-fast keeps params thread failure nonfatal",
        !boot_test_params_thread_failure_is_fatal(true, true, true));
    BP_CHECK("non-mainnet params thread failure stays warning-only",
        !boot_test_params_thread_failure_is_fatal(true, false, false));

    /* ── boot_need_legacy_header_pull (fresh-datadir need_zcd fix) ──
     * MEMORY/bug: on a genuinely fresh/empty datadir both
     * active_chain_height() and db_block_max_height() are 0, so the
     * ratio-only test `local < chain_h*9/10` degenerates to `0 < 0` and
     * never fires — the fresh node silently falls back to a slow P2P
     * header crawl instead of the ~60s legacy import. The fix adds an
     * explicit empty-datadir trigger (local_index_size == 0) gated on a
     * legacy source actually being present. */
    BP_CHECK("empty datadir + legacy present fires the pull (the fix)",
        boot_need_legacy_header_pull(0, 0, true));
    BP_CHECK("empty datadir + no legacy source does NOT fire",
        !boot_need_legacy_header_pull(0, 0, false));
    BP_CHECK("empty datadir + no legacy source, nonzero chain_h estimate",
        !boot_need_legacy_header_pull(0, 3000000, false));
    BP_CHECK("local at 90pct of chain height does not fire (at threshold)",
        !boot_need_legacy_header_pull(2700000, 3000000, true));
    BP_CHECK("local just below 90pct of chain height fires (ratio trigger)",
        boot_need_legacy_header_pull(2699999, 3000000, true));
    BP_CHECK("local far below chain height fires even with tiny local>0",
        boot_need_legacy_header_pull(3, 3000000, true));
    BP_CHECK("local at chain height does not fire",
        !boot_need_legacy_header_pull(3000000, 3000000, true));
    BP_CHECK("ratio trigger requires legacy source present",
        !boot_need_legacy_header_pull(3, 3000000, false));

    /* ── boot_need_blocks_table_hydrate (importblockindex determinism) ──
     * MEMORY/bug: the `blocks`-table bulk-hydrate rung (config/src/boot.c,
     * the sink --importblockindex CLI-bulk-loads header rows into) used to
     * be gated on `!loaded && map_size<=1`. An EARLIER loader rung (flat
     * file / block_index_cache) can succeed ("loaded=true") with a small
     * STALE map left over from a partial P2P header sync that predates the
     * CLI import — the `!loaded` guard then PERMANENTLY skips the bulk
     * rung even though the blocks table holds millions of complete rows,
     * and the node falls back to a P2P/getheaders header crawl to re-fetch
     * headers it already has on disk (~90 min instead of ~74s). The fix
     * keys the rung on the blocks-table row count vs the CURRENTLY loaded
     * map size instead of the `loaded` flag — a stale small map never
     * blocks it. */
    BP_CHECK("empty map + blocks table populated fires (fresh datadir "
             "chooses bulk)",
        boot_need_blocks_table_hydrate(0, 3100000));
    BP_CHECK("genesis-only map + blocks table populated fires",
        boot_need_blocks_table_hydrate(1, 3100000));
    BP_CHECK("stale small map (200) far below blocks table fires — the "
             "exact `loaded=true` stale-map defect this closes",
        boot_need_blocks_table_hydrate(200, 3100000));
    BP_CHECK("blocks table empty never fires (nothing to hydrate)",
        !boot_need_blocks_table_hydrate(0, 0));
    BP_CHECK("map at 90pct of blocks table rows does not fire (at threshold)",
        !boot_need_blocks_table_hydrate(2790000, 3100000));
    BP_CHECK("map just below 90pct of blocks table rows fires (ratio "
             "trigger)",
        boot_need_blocks_table_hydrate(2789999, 3100000));
    BP_CHECK("fully-loaded map (warm restart) does not re-fire — preserves "
             "existing warm-datadir behavior",
        !boot_need_blocks_table_hydrate(3100000, 3100000));

    /* ── forward-jump (legal, emits WARN) ──────────────────────── */
    boot_stage_reset_for_testing();
    boot_stage_advance_to(BOOT_STAGE_READY);
    BP_CHECK("forward-jump from INIT to READY (skipped intermediate)",
        boot_stage_current() == BOOT_STAGE_READY);

    /* ── shutdown entry from mid-boot ──────────────────────────── */
    boot_stage_reset_for_testing();
    boot_stage_advance_to(BOOT_STAGE_DB_OPEN);
    boot_stage_advance_to(BOOT_STAGE_SHUTDOWN_REQUESTED);
    BP_CHECK("shutdown can be entered from mid-boot (DB_OPEN)",
        boot_stage_current() == BOOT_STAGE_SHUTDOWN_REQUESTED);

    /* Within the shutdown range, advance to SHUTDOWN_COMPLETE is a normal
     * forward step. */
    boot_stage_advance_to(BOOT_STAGE_SHUTDOWN_COMPLETE);
    BP_CHECK("advance SHUTDOWN_REQUESTED -> SHUTDOWN_COMPLETE",
        boot_stage_current() == BOOT_STAGE_SHUTDOWN_COMPLETE);

    /* ── illegal transitions abort() (fork-isolated) ─────────────────
     * boot_stage_advance_to() calls abort() on a BACKWARD move and on an
     * OUT-OF-RANGE target (lib/util/src/boot_phase.c:112-118 and
     * :140-147). abort() raises SIGABRT with no handler installed in the
     * test process, so it would kill the runner. We fork a child, have it
     * perform the illegal advance, and assert the child is *terminated by
     * SIGABRT* (WIFSIGNALED && WTERMSIG==SIGABRT). The child redirects its
     * own stderr to /dev/null so the abort's diagnostic fprintf does not
     * pollute the test log, and falls through to a distinct _exit() code
     * if abort() did NOT fire — a real regression then surfaces as a clean
     * exit instead of a signal. Mirrors the fork+SIGABRT idiom in
     * lib/test/src/test_postmortem.c:104-121. */

    /* (a) BACKWARD move: DB_OPEN -> INIT must abort. */
    fflush(stdout);
    fflush(stderr);
    {
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
            boot_stage_reset_for_testing();           /* -> INIT */
            boot_stage_advance_to(BOOT_STAGE_DB_OPEN); /* legal forward jump */
            boot_stage_advance_to(BOOT_STAGE_INIT);    /* illegal: backward */
            _exit(99); /* reached only if the abort() did NOT fire */
        }
        BP_CHECK("fork backward-move child", pid > 0);
        if (pid > 0) {
            int status = 0;
            pid_t got = waitpid(pid, &status, 0);
            BP_CHECK("wait backward-move child", got == pid);
            BP_CHECK("backward move (DB_OPEN -> INIT) aborts via SIGABRT",
                     got == pid && WIFSIGNALED(status) &&
                     WTERMSIG(status) == SIGABRT);
        }
    }

    /* (b) OUT-OF-RANGE target: BOOT_STAGE__MAX must abort. */
    fflush(stdout);
    fflush(stderr);
    {
        pid_t pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
            boot_stage_reset_for_testing();              /* -> INIT */
            boot_stage_advance_to(BOOT_STAGE__MAX);       /* illegal: >= MAX */
            _exit(99); /* reached only if the abort() did NOT fire */
        }
        BP_CHECK("fork out-of-range child", pid > 0);
        if (pid > 0) {
            int status = 0;
            pid_t got = waitpid(pid, &status, 0);
            BP_CHECK("wait out-of-range child", got == pid);
            BP_CHECK("out-of-range target (__MAX) aborts via SIGABRT",
                     got == pid && WIFSIGNALED(status) &&
                     WTERMSIG(status) == SIGABRT);
        }
    }

    /* Restore for any subsequent tests in this process. */
    boot_stage_reset_for_testing();
    return failures;
}
