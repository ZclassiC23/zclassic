/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The `make_lint_gates` test group: it runs the self-test of every lint gate.
 *
 * Problem: a lint gate is the only thing stopping a whole class of bug from
 * coming back — `check-raw-sqlite`, for one, is all that stops new raw
 * `sqlite3_step` calls from reintroducing a UTXO-wipe. If someone loosens a
 * gate's pattern ("oh, it's annoying on this PR, let me add another
 * exemption"), the gate silently stops catching violations. Every check in
 * this group prevents that, in the same shape:
 *
 *   1. Copy a known-bad fixture into the scanned tree under a unique temp name
 *      so the gate's scan actually sees it.
 *   2. Run the gate.
 *   3. Assert exit code != 0 (the gate caught the fixture).
 *   4. Remove the temp file and rerun to confirm the gate passes again.
 *
 * The checks themselves live in the sibling lint_gate_*.c files, grouped by
 * the gate family they guard; lint_gate_selftests.h is the map and the shared
 * surface. This file owns the entry table (g_lint_gate_entries — the single
 * list of every check, and whether it needs the real worktree), the internal
 * parallel runner, and the repo-root resolution the runner overrides per
 * sandbox worker.
 *
 * Gated by `ZCL_TESTING` so the shell-out + make invocation only fires when
 * the suite is built by `make test`; standalone compilations of test_zcl
 * without the macro silently turn this into a no-op pass. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_core.h"

#ifdef ZCL_TESTING

#include "lint_gate_selftests.h"

/* Per-process sandbox-root override for the internal parallel runner. When a
 * pool worker (a fork()ed child) runs a gate in its OWN hardlink sandbox, it
 * calls repo_root_set_override() with the sandbox path; from then on every
 * repo_path()/run_gate_script()/fixture-plant in that child resolves INTO the
 * sandbox (fixtures planted there, and the sandbox's own copy of the gate
 * script is exec'd, so `dirname $0/../..` roots the scan at the sandbox). This
 * is what isolates concurrent fixture planting/unlinking so the group can run
 * its ~100 checks through a bounded worker pool without racing each other's
 * scans. Set once per child, never in the parent; checked BEFORE the cache so
 * it wins over an inherited (real-root) cached value. */
static char g_repo_root_override[PATH_MAX];
static int g_repo_root_override_set = 0;

void repo_root_set_override(const char *path)
{
    if (!path) { g_repo_root_override_set = 0; return; }
    snprintf(g_repo_root_override, sizeof(g_repo_root_override), "%s", path);
    g_repo_root_override_set = 1;
}

const char *repo_root(void)
{
    static char root[PATH_MAX];
    static int cached = 0;

    if (g_repo_root_override_set)
        return g_repo_root_override[0] ? g_repo_root_override : NULL;

    if (cached) return root[0] ? root : NULL;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe) - 1) {
        cached = 1;
        root[0] = '\0';
        return NULL;
    }
    exe[n] = '\0';

    /* The binary lives at <root>/build/bin/<name>; walk UP from the exe
     * until a directory holding both the Makefile and the raw-sqlite
     * fixture appears. A single dirname() here once left root at
     * build/bin, the entry stat failed, and the WHOLE suite silently
     * no-op-SKIPped (PASS in 1s) — every source-text gate in this file
     * was dead. Bounded walk so a stray Makefile high in the tree can't
     * send the shell-outs somewhere surprising. */
    for (int depth = 0; depth < 6; depth++) {
        char *slash = strrchr(exe, '/');
        if (!slash || slash == exe) break;
        *slash = '\0';

        char probe[PATH_MAX];
        struct stat st;
        if (snprintf(probe, sizeof(probe), "%s/Makefile", exe)
                >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;
        if (snprintf(probe, sizeof(probe), "%s/%s", exe, FIXTURE_SRC_REL)
                >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;

        if (snprintf(root, sizeof(root), "%s", exe) >= (int)sizeof(root))
            break;
        cached = 1;
        return root;
    }

    cached = 1;
    root[0] = '\0';
    return NULL;
}

int repo_path(char *out, size_t outsz, const char *rel)
{
    const char *root = repo_root();
    if (!root || !out || outsz == 0 || !rel) return -1;
    return snprintf(out, outsz, "%s/%s", root, rel) >= (int)outsz ? -1 : 0;
}

/* ── Internal parallel runner for the make_lint_gates group ───────────────
 * Historically these ~100+ checks ran strictly serially, and the group ran
 * as an exclusive serial PRE-PASS (see group_requires_exclusive_repo in
 * test_parallel.c) because each check plants a fixture .c file into a shared
 * source path and then unlink()s it — a concurrent scanner in ANOTHER group
 * could readdir a transient fixture and race the unlink (grep exits 2 =>
 * FATAL). Both costs stacked: this one group was ~140s warm / ~270s cold, the
 * long pole of the whole suite (~80%).
 *
 * The runner below keeps the group exclusive but makes it INTERNALLY parallel.
 * Every check that only needs a plain file tree ("sandbox" entries) runs in a
 * bounded pool of worker processes, each pinned to its OWN hardlink copy of
 * the source tree (built with `cp -al` in ~0.1s). A worker calls
 * repo_root_set_override() so its fixture plants AND its gate-script exec
 * resolve INTO its sandbox — the sandbox's own copy of the gate script is
 * exec'd, so `dirname $0/../..` roots the scan at the sandbox. Concurrent
 * planting/unlinking therefore can never race another worker's scan, and the
 * per-worker fixture path is private. The handful of checks that depend on the
 * real .git (git grep/ls-files, .git/hooks) or run a heavier selftest stay on
 * the real worktree and run serially.
 *
 * On ANY sandbox-setup failure the runner falls back to a fully serial run in
 * the real worktree (lint_run_all_serial) — byte-identical to the historical
 * behavior — so the optimization can never trade away correctness. */

typedef int (*lint_gate_fn)(void);

struct lint_gate_entry {
    lint_gate_fn fn;
    int          real_serial; /* 1 => needs real .git / real worktree */
};

#define S_(f) { (f), 0 }
#define R_(f) { (f), 1 }
static const struct lint_gate_entry g_lint_gate_entries[] = {
    S_(t_baseline_passes),
    S_(t_fixture_trips_gate),
    S_(t_node_db_exec_fixture_trips_gate),
    S_(t_gate_recovers_after_removal),
    S_(t_coins_guard_baseline_passes),
    S_(t_coins_guard_fixture_trips_gate),
    S_(t_coins_guard_gate_recovers),
    S_(t_coins_guard_gate_fails_loud_on_no_lookup_surface),
    S_(t_observability_fixture_trips_gate),
    S_(t_observability_positive_controls_pass),
    S_(t_raw_malloc_fixture_trips_gate),
    S_(t_raw_malloc_zcl_fixture_passes),
    S_(t_raw_malloc_gate_recovers),
    S_(t_service_tip_mutation_gate),
    S_(t_legacy_candidate_source_has_no_override_scope),
    R_(t_deprecated_tools_z_is_absent),          /* git grep */
    S_(t_canonical_operator_diagnostics_contract),
    S_(t_canonical_deploy_proof_binding_contract),
    S_(t_dev_lane_deploy_contract),
    S_(t_agent_fast_ci_contract),
    S_(t_native_operator_docs_contract),
    S_(t_remote_node_update_contract),
    S_(t_native_agent_api_contract),
    S_(t_mvp_reporters_resolve_live_service_rpc_contract),
    S_(t_soak_assert_requires_known_mirror_lag),
    S_(t_boot_chain_advance_diagnostics_contract),
    S_(t_boot_core_liveness_precedes_frontend_contract),
    S_(t_boot_addrman_persistence_contract),
    S_(t_lib_runtime_gauges_are_callback_injected),
    S_(t_boot_shutdown_persistence_order_contract),
    S_(t_hodl_history_uses_runtime_db_service),
    S_(t_db_service_query_handle_is_canonical),
    S_(t_txindex_releases_node_db_between_batches),
    S_(t_peer_save_busy_reports_db_error),
    S_(t_handshake_peer_save_is_async),
    S_(t_p2p_app_persistence_is_callback_injected),
    S_(t_tx_wallet_sync_is_callback_injected),
    S_(t_p2p_block_submit_is_callback_injected),
    S_(t_flyclient_proof_builder_is_callback_injected),
    S_(t_fast_sync_uses_lib_sqlite_helpers),
    S_(t_framework_reexport_headers_stay_deleted),
    S_(t_utxo_reimport_flag_is_storage_owned),
    S_(t_net_sync_planners_are_lib_owned),
    S_(t_header_peer_votes_are_callback_injected),
    S_(t_process_block_node_db_access_is_runtime_owned),
    S_(t_process_block_split_uses_reducer_language),
    S_(t_production_comments_do_not_carry_refactor_scaffold_labels),
    S_(t_deleted_engine_names_absent_from_production_sources),
    S_(t_build_commit_macro_stays_behind_getter),
    S_(t_boot_repaired_index_persistence_contract),
    S_(t_chain_evidence_reconstruct_uses_retry_persistence),
    S_(t_boot_genesis_init_preserves_restored_authority_contract),
    S_(t_refold_from_anchor_explicit_span_gate_contract),
    S_(t_sha3_window_tool_check_contract),
    S_(t_make_ignores_ephemeral_lint_fixture_sources),
    S_(t_block_index_flat_atomic_save_contract),
    S_(t_projection_deferral_is_not_block_rejected_contract),
    S_(t_trusted_peer_stall_guard_contract),
    S_(t_gap_fill_wakes_connman_dispatch_contract),
    S_(t_msg_process_yields_to_send_phase_contract),
    S_(t_e1_file_size_ceiling),
    S_(t_e1_lib_warn_tier),
    S_(t_long_functions_enforced_ratchet),
    S_(t_long_functions_lib_warn_tier),
    S_(t_no_new_repair_rung),
    S_(t_no_new_borrowed_seed_caller),
    S_(t_no_new_coin_backfill_caller),
    S_(t_no_writer_below_sealed_frontier),
    S_(t_e9_operator_needed_sink),
    S_(t_systemd_memory_budget),
    S_(t_quality_job_guard),
    R_(t_import_copy_prove_selftest),            /* selftest under `timeout` */
    R_(t_fresh_boot_weld_prove_selftest),        /* selftest under `timeout` */
    S_(t_e14_condition_cooldown_gate),
    R_(t_markdown_links_gate),                   /* git ls-files */
    R_(t_git_hooks_gate_enforces_tracked_pre_push),  /* .git/hooks */
    R_(t_git_hooks_gate_rejects_noop_pre_push),      /* .git/hooks */
    R_(t_git_hooks_gate_rejects_noop_pre_commit),    /* .git/hooks */
    S_(t_telemetry_ontology_gate),
    S_(t_e10_framework_shape_ratchet),
    S_(t_e10_no_raw_sqlite_ratchet),
    S_(t_gate22_framework_filename_suffix),
    S_(t_gate_p2_group_purpose),
    S_(t_gate_p1_file_purpose),
    R_(t_gate_p3_orphan_placement),              /* git ls-files baseline */
    S_(t_e13_consensus_parity_fixture),
    S_(t_silent_errors_bool_fixture),
    S_(t_log_macro_return_type_gate),
    S_(t_e11_doc_accuracy),
    S_(t_model_ar_lifecycle_gate),
    S_(t_e2_one_result_type),
    S_(t_service_result_convergence_ratchet),
    S_(t_thread_supervision_ratchet),
    S_(t_supervisor_registration_widened_ratchet),
    S_(t_e3_shape_includes_header),
    S_(t_e4_projections_pure),
    S_(t_domain_purity),
    S_(t_shape_include_direction),
    S_(t_e5_stage_advances_or_blocks),
    S_(t_e6_one_write_path),
    S_(t_e7_no_authoritative_ram_state),
    S_(t_e12_honest_witness),
    S_(t_gate21_supervisor_worker_lockin),
    S_(t_hotswap_eligible_scope_gate),
    S_(t_hotswap_swappable_shape_gate),
    S_(t_hotswap_swappable_leaf_contract_gate),
    S_(t_hotswap_static_state_gate),
    S_(t_hotswap_static_state_covers_swappable),
    S_(t_privileged_transition_receipt_gate),
    S_(t_blocker_escape_registered_gate),
    R_(t_no_trust_state_ordering_gate),          /* git grep --untracked */
    S_(t_lint_gates_fail_loud_on_empty_scan),
    S_(t_no_dev_history_in_contracts),
    S_(t_no_uncited_victory),
    R_(t_no_stray_root_files),                   /* git ls-files + real root */
};
#undef S_
#undef R_
#define LINT_GATE_ENTRY_COUNT \
    (sizeof(g_lint_gate_entries) / sizeof(g_lint_gate_entries[0]))

/* Fully serial fallback: run every entry in the real worktree, in order.
 * Identical to the historical runner; used when parallel setup fails. */
static int lint_run_all_serial(void)
{
    int failures = 0;
    for (size_t i = 0; i < LINT_GATE_ENTRY_COUNT; i++)
        failures += g_lint_gate_entries[i].fn();
    return failures;
}

/* Build a hardlink copy ("sandbox") of the worktree at sb_root. Everything
 * except build/.git/.cache/test-tmp/.claude is `cp -al`'d (metadata-only, so
 * ~0.1s for the whole tree); test-tmp is created fresh so gate scratch output
 * never writes through a shared inode. Returns 0 on success. */
static int lint_sandbox_build(const char *real_root, const char *sb_root)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[lint-gate] lint_sandbox_build: fork failed: %s\n",
                strerror(errno));
        return -1;
    }
    if (pid == 0) {
        static const char *script =
            "set -e\n"
            "rm -rf \"$2\"\n"
            "mkdir -p \"$2\"\n"
            "for e in \"$1\"/* \"$1\"/.[!.]*; do\n"
            "  [ -e \"$e\" ] || continue\n"
            "  b=${e##*/}\n"
            "  case \"$b\" in build|.git|.cache|test-tmp|.claude) continue;; esac\n"
            "  cp -al \"$e\" \"$2\"/\n"
            "done\n"
            "mkdir -p \"$2\"/test-tmp\n";
        execl("/bin/sh", "sh", "-c", script, "sh",
              real_root, sb_root, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "[lint-gate] lint_sandbox_build: waitpid failed: %s\n",
                strerror(errno));
        return -1;
    }
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* Run one entry with its stdout captured to out_path and its failure count
 * written to rc_path. Any repo_root override must already be set. */
static void lint_run_one_captured(const struct lint_gate_entry *e,
                                  const char *out_path, const char *rc_path)
{
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int outfd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (outfd >= 0) {
        (void)dup2(outfd, STDOUT_FILENO);
        close(outfd);
    }
    int failures = e->fn();
    fflush(stdout);
    if (saved >= 0) {
        (void)dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    FILE *rf = fopen(rc_path, "w");
    if (rf) {
        fprintf(rf, "%d\n", failures);
        fclose(rf);
    }
}

/* Rough per-check cost hint (relative units) used to dispatch the slowest
 * checks FIRST (longest-processing-time-first). Without it the ~33s silent-
 * bool check sits near the end of the table, starts late, and pushes out the
 * makespan; started at t=0 it overlaps everything else. Values are derived
 * from measured warm gate times x the check's baseline/trip/recover run count;
 * they only need to rank, not be exact. */
static int lint_entry_weight(lint_gate_fn fn)
{
    if (fn == t_silent_errors_bool_fixture)             return 3300;
    if (fn == t_no_dev_history_in_contracts)            return 2400;
    if (fn == t_gate_p1_file_purpose)                   return 600;
    if (fn == t_lint_gates_fail_loud_on_empty_scan)     return 400;
    if (fn == t_fixture_trips_gate ||
        fn == t_node_db_exec_fixture_trips_gate ||
        fn == t_baseline_passes ||
        fn == t_gate_recovers_after_removal)            return 300;
    if (fn == t_long_functions_enforced_ratchet ||
        fn == t_long_functions_lib_warn_tier)           return 250;
    if (fn == t_thread_supervision_ratchet)             return 200;
    if (fn == t_e1_file_size_ceiling ||
        fn == t_e1_lib_warn_tier)                       return 150;
    if (fn == t_supervisor_registration_widened_ratchet) return 130;
    return 10;
}

/* Remove any sandbox bases left by a PREVIOUS run that was killed (SIGKILL/
 * SIGTERM) before its own teardown could run — the normal path rm -rf's its
 * base, but a hard kill leaks it. Best-effort: scan the worktree's parent for
 * "<worktree>.lint_sb_*" siblings and delete them. Only this group ever
 * creates that name, and only one instance runs at a time, so nothing live is
 * ever matched. */
static void lint_purge_stale_sandboxes(const char *real_root)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", real_root) >= (int)sizeof(tmp))
        return;
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    const char *parent = tmp;
    const char *base = slash + 1;

    char prefix[PATH_MAX];
    if (snprintf(prefix, sizeof(prefix), "%s.lint_sb_", base) >=
        (int)sizeof(prefix))
        return;
    size_t plen = strlen(prefix);

    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, plen) != 0) continue;
        char victim[PATH_MAX];
        if (snprintf(victim, sizeof(victim), "%s/%s", parent, e->d_name) <
            (int)sizeof(victim))
            (void)test_rm_rf_recursive(victim);
    }
    closedir(d);
}

/* Shared work dispenser handed the sandbox worker pool via MAP_SHARED. */
struct lint_dispenser {
    atomic_int next;
};

int test_make_lint_gates(void)
{
    printf("\n=== make_lint_gates tests ===\n");

    /* Resolve against the built test binary location so later tests can
     * change cwd without breaking these shell-outs. */
    struct stat st;
    char fixture_src[PATH_MAX];
    char makefile[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), FIXTURE_SRC_REL) != 0 ||
        repo_path(makefile, sizeof(makefile), "Makefile") != 0 ||
        stat(fixture_src, &st) != 0 || stat(makefile, &st) != 0) {
        printf("[lint-gate] SKIP: repo root not discoverable from test_zcl path\n");
        return 0;
    }

    const char *real_root = repo_root();
    if (!real_root)
        return lint_run_all_serial();

    /* Reap any sandbox base leaked by a hard-killed prior run before we make
     * ours (best-effort; only this group ever creates that sibling name). */
    lint_purge_stale_sandboxes(real_root);

    /* Sandbox base lives OUTSIDE the worktree (a sibling, same filesystem so
     * hardlinks work) so a real-worktree `git grep --untracked` can never see
     * a worker's planted fixture. */
    char sb_base[PATH_MAX];
    if (snprintf(sb_base, sizeof(sb_base), "%s.lint_sb_%d",
                 real_root, (int)getpid()) >= (int)sizeof(sb_base))
        return lint_run_all_serial();

    char out_dir[PATH_MAX];
    if (snprintf(out_dir, sizeof(out_dir), "%s/out", sb_base) >=
        (int)sizeof(out_dir)) {
        return lint_run_all_serial();
    }

    /* Worker count: half the cores, clamped to leave headroom (this group
     * runs as an exclusive pre-pass, so the machine is otherwise idle). Only
     * a few workers are needed to hide every check under the single slowest
     * one (~30s), but a few extra warm the shared page cache faster. */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = (ncpu > 0) ? (int)(ncpu / 4) : 4;
    if (workers < 4) workers = 4;
    if (workers > 12) workers = 12;
    const char *workers_env = getenv("ZCL_LINT_GATE_WORKERS");
    if (workers_env && *workers_env) {
        int w = atoi(workers_env);
        if (w >= 1 && w <= 64) workers = w;
    }

    /* Collect the sandbox-eligible entry indices, then reorder them
     * longest-job-first so the slowest checks start immediately. */
    int sb_idx[LINT_GATE_ENTRY_COUNT];
    int n_sb = 0;
    for (size_t i = 0; i < LINT_GATE_ENTRY_COUNT; i++)
        if (!g_lint_gate_entries[i].real_serial)
            sb_idx[n_sb++] = (int)i;
    for (int a = 1; a < n_sb; a++) {   /* stable insertion sort, weight DESC */
        int cur = sb_idx[a];
        int cw = lint_entry_weight(g_lint_gate_entries[cur].fn);
        int b = a - 1;
        while (b >= 0 &&
               lint_entry_weight(g_lint_gate_entries[sb_idx[b]].fn) < cw) {
            sb_idx[b + 1] = sb_idx[b];
            b--;
        }
        sb_idx[b + 1] = cur;
    }
    if (workers > n_sb) workers = n_sb > 0 ? n_sb : 1;

    if (mkdir(sb_base, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "[lint-gate] mkdir sandbox base failed: %s\n",
                strerror(errno));
        return lint_run_all_serial();
    }
    if (mkdir(out_dir, 0700) != 0 && errno != EEXIST) {
        (void)test_rm_rf_recursive(sb_base);
        return lint_run_all_serial();
    }

    /* Build one sandbox per worker up front (reused across the worker's
     * queue of checks). Any failure => tear down and fall back to serial. */
    char (*sb_root)[PATH_MAX] = calloc((size_t)workers, sizeof(*sb_root));
    if (!sb_root) {
        (void)test_rm_rf_recursive(sb_base);
        return lint_run_all_serial();
    }
    int setup_ok = 1;
    for (int w = 0; w < workers && setup_ok; w++) {
        if (snprintf(sb_root[w], PATH_MAX, "%s/w%d", sb_base, w) >= PATH_MAX ||
            lint_sandbox_build(real_root, sb_root[w]) != 0)
            setup_ok = 0;
    }
    if (!setup_ok) {
        free(sb_root);
        (void)test_rm_rf_recursive(sb_base);
        fprintf(stderr,
                "[lint-gate] sandbox setup failed — running serially\n");
        return lint_run_all_serial();
    }

    struct lint_dispenser *disp = mmap(NULL, sizeof(*disp),
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (disp == MAP_FAILED) {
        free(sb_root);
        (void)test_rm_rf_recursive(sb_base);
        return lint_run_all_serial();
    }
    atomic_store(&disp->next, 0);

    /* Flush the header before forking so children don't re-emit it. */
    fflush(stdout);

    /* ── Sandbox worker pool: each worker owns sandbox sb_root[w], pulls the
     * next sandbox-entry index from the shared dispenser, and runs it in its
     * private tree. Dynamic dispensing balances the ~30s slowest check
     * against the many sub-second ones. */
    for (int w = 0; w < workers; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            /* A partial pool is still correct — remaining indices are picked
             * up by the workers that did start; if none started, the parent
             * serial pass below covers nothing and rc files are missing =>
             * counted as failures. Keep going. */
            continue;
        }
        if (pid == 0) {
            /* Enter the sandbox: repo_root_set_override makes repo_path()
             * absolute-resolve into the sandbox, and chdir() makes cwd match
             * so the handful of checks that write/read RELATIVE paths (the
             * long-function keep/baseline files, the gate scratch out) land in
             * the SAME tree the gate scans. In the historical serial run cwd
             * and repo_root were both the real worktree; the sandbox must keep
             * that invariant. */
            if (chdir(sb_root[w]) != 0) {
                fprintf(stderr, "[lint-gate] worker chdir(%s) failed: %s\n",
                        sb_root[w], strerror(errno));
                _exit(0); /* leave rc files absent => counted as failures */
            }
            repo_root_set_override(sb_root[w]);
            for (;;) {
                int k = atomic_fetch_add_explicit(&disp->next, 1,
                                                  memory_order_relaxed);
                if (k >= n_sb) break;
                int gi = sb_idx[k];
                /* Defensive clean start in this sandbox (no-op if already
                 * clean); every check also cleans up after itself. */
                unlink_lint_fixtures();
                char op[PATH_MAX], rp[PATH_MAX];
                snprintf(op, sizeof(op), "%s/f%d.out", out_dir, gi);
                snprintf(rp, sizeof(rp), "%s/f%d.rc", out_dir, gi);
                lint_run_one_captured(&g_lint_gate_entries[gi], op, rp);
            }
            _exit(0);
        }
    }

    /* ── Real-worktree serial pass, run CONCURRENTLY with the sandbox pool.
     * These git/.git-dependent checks operate on the real worktree (and their
     * own /tmp scratch); the pool workers operate only on their sandboxes, so
     * the two never touch the same tree. Overlapping them hides the ~30s
     * hermetic import-copy-prove selftest under the pool's makespan instead of
     * tacking it on afterward. No repo-root override here (parent stays on the
     * real worktree); each runs one at a time (some plant into the real tree).
     * run_gate_script uses targeted waitpid(), so it never reaps a pool
     * worker; the workers are reaped by the wait() loop below. */
    for (size_t i = 0; i < LINT_GATE_ENTRY_COUNT; i++) {
        if (!g_lint_gate_entries[i].real_serial) continue;
        char op[PATH_MAX], rp[PATH_MAX];
        snprintf(op, sizeof(op), "%s/f%zu.out", out_dir, i);
        snprintf(rp, sizeof(rp), "%s/f%zu.rc", out_dir, i);
        lint_run_one_captured(&g_lint_gate_entries[i], op, rp);
    }

    for (int w = 0; w < workers; w++) {
        int st2 = 0;
        while (wait(&st2) < 0 && errno == EINTR) { /* reap all workers */ }
    }

    /* ── Collect: replay captured output in original order, sum failures. A
     * missing rc file means the worker died mid-check => count as a failure
     * so the group can never green over a crashed check. */
    int failures = 0;
    for (size_t i = 0; i < LINT_GATE_ENTRY_COUNT; i++) {
        char op[PATH_MAX], rp[PATH_MAX];
        snprintf(op, sizeof(op), "%s/f%zu.out", out_dir, i);
        snprintf(rp, sizeof(rp), "%s/f%zu.rc", out_dir, i);

        FILE *of = fopen(op, "r");
        if (of) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), of)) > 0)
                fwrite(buf, 1, n, stdout);
            fclose(of);
        }
        FILE *rf = fopen(rp, "r");
        if (rf) {
            int fc = 0;
            if (fscanf(rf, "%d", &fc) == 1)
                failures += fc;
            else
                failures += 1;
            fclose(rf);
        } else {
            printf("[lint-gate] FAIL: check #%zu produced no result "
                   "(worker crashed?)\n", i);
            failures += 1;
        }
    }
    fflush(stdout);

    (void)munmap(disp, sizeof(*disp));
    free(sb_root);
    (void)test_rm_rf_recursive(sb_base);
    return failures;
}

#else  /* !ZCL_TESTING */

int test_make_lint_gates(void)
{
    /* No-op when the lint-gate integration test is disabled. */
    return 0;
}

#endif /* ZCL_TESTING */
