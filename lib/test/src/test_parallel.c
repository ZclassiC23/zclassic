/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fork-parallel driver for the zclassic23 test suite.
 *
 * The sequential runner (`build/bin/test_zcl`, `main()` in test.c) executes
 * ~170 test groups back-to-back on a single CPU. On a 32-core box we
 * barely use 3% of available compute and the suite takes 8-15 minutes.
 *
 * This binary runs the same groups concurrently. Every group gets its
 * own child process via fork(), with output captured to a per-child
 * temp file. The parent waits for all children and then replays their
 * output in group order so a human sees a deterministic transcript.
 *
 * The in-process call pattern of the sequential runner — one
 * ecc_start() for everything — doesn't translate directly to a
 * parallel runner. Each child does its own `chain_params_select` +
 * `ecc_start` + `ecc_verify_init` before running its group, then
 * `ecc_verify_destroy` + `ecc_stop` before exit. The extra setup cost
 * is paid once per group and is dwarfed by the per-group test time.
 *
 * Maintenance note: the registry below must stay in sync with the
 * dispatch list in lib/test/src/test.c. Adding a test to test.c but
 * not here means the parallel runner silently skips it. Adding it
 * here but not test.c means the sequential path skips it. A
 * lightweight source-level diff would catch drift, but the canonical
 * registry is test_parallel.c — test.c is the legacy shape. */

#define _POSIX_C_SOURCE 200809L

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "event/event.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Required by process_block.c (normally in main.c) */
volatile sig_atomic_t g_shutdown_requested = 0;

/* ── Test registry ─────────────────────────────────────────────────
 *
 * TEST_LIST and SPEC_LIST are X-macros expanded three times below:
 *   DECL  — produces extern int test_<name>(void);
 *   ENTRY — produces {"<name>", test_<name>} table rows
 *
 * Keep in sync with the dispatch list in lib/test/src/test.c.
 */

#define TEST_LIST(X) \
    X(game) X(crypto) X(crypto_registry) X(encoding) X(chain) \
    X(pprev_walk) X(chain_tip) X(checkpoint) X(keys) \
    X(script) X(net) X(transaction) X(mempool) X(accept_to_mempool) X(rpc) X(sqlite) \
    X(activerecord) X(validation) X(sapling_lazy_init) X(sapling) X(sapling_crypto) \
    X(bn254) X(merkle_tree) X(slp) X(models) X(core) X(znam) X(htlc) \
    X(file_market) X(strong_params) X(json) X(robustness) X(wallet) \
    X(primitives) X(bloom) X(coins) X(store) X(blog) X(api) \
    X(explorer) X(explorer_index) X(mining) X(utxo_commitment) X(mmr) X(mmb) X(sha3_windows) \
    X(keystone_utxo_binding) \
    X(flyclient) X(flyclient_chainwork_floor) X(scan_util) X(tor) \
    X(onion_bootstrap) X(cold_start_sync) X(kill9_recovery) \
    X(shielded_payment_gate) X(store_e2e_gate) X(store_e2e_shielded) X(soak_harness) \
    X(event) X(download) X(consensus) X(consensus_parity) \
    X(policy) X(wallet_view) X(fast_sync) X(block_scan) \
    X(node_health_service) X(chain_state_repo) X(recovery_policy) \
    X(chain_evidence_controller) \
    X(chain_evidence_live_advance) \
    X(long_op) \
    X(db_txn) X(sync_service) X(snapshot_sync_service) \
    X(file_controller) X(file_ops) X(integrity) X(rolling_anchor_service) \
    X(protocols) \
    X(chain_restore_planner) X(chain_restore_service) \
    X(chain_activation_controller) \
    X(mcp_router) X(mcp_controllers) X(mcp_middleware) X(mcp_metrics) \
    X(mcp_inproc_equiv) \
    X(mcp_e2e) X(mcp_notify) X(db_validators) X(peer_scoring) X(peer_bandwidth) \
    X(secrets_hygiene) X(block_index_integrity) \
    X(block_map_grow_phashblock) \
    X(block_successor) \
    X(key_hostile_wif) \
    X(block_locator_bounds) X(block_map_grow_collision) \
    X(connect_node_locked) X(stream_read_no_overflow) \
    X(fast_sync_serve_chunk_db_clamps) X(connman_node_count_locked) \
    X(fees_oom) X(fees_oom_inject) X(multisig_consensus_branches) \
    X(parse_script_oversize_hex) X(script_num_minimal_encoding) \
    X(domain_consensus_pow_seal_matrix)    X(domain_consensus_pow_seal_powlimit_floor) \
    X(domain_consensus_pow_seal_malformed_paths)    X(domain_consensus_pow_seal_deterministic) \
    X(checkpoints_progress_boundary_crossover)    X(checkpoints_progress_zero_defenses) \
    X(checkpoints_progress_sigcheck_factor)    X(checkpoints_progress_regression_seal) \
    X(regtest_generate) \
    X(equihash_null_guards)    X(equihash_solution_size_demux) \
    X(equihash_blake2b_state_seal)    X(equihash_serialization_matches_independent_rebuild) \
    X(equihash_legacy_wrapper_regression_seal)    X(coins_amount_codec_roundtrip) \
    X(coins_amount_codec_boundary_exponents)    X(coins_amount_codec_digit_preservation) \
    X(coins_amount_codec_regression_seal)    X(hmac_sha512_kat_rfc4231_jefe) \
    X(hmac_sha512_kat_oversized_key)    X(hmac_sha512_empty_message) \
    X(hmac_sha512_multiblock_stateful_write)    X(hmac_sha512_key_len_128_boundary) \
    X(pbkdf2_sha512_rfc_vector)    X(pbkdf2_sha512_multiblock) \
    X(pbkdf2_sha512_high_iterations)    X(pbkdf2_sha512_empty_inputs) \
    X(pbkdf2_sha512_one_byte_output) \
    X(sapling_address_hash_fields)    X(sprout_address_hash_fields) \
    X(sapling_sprout_hash_idempotence_and_distinct)    X(sprout_spending_key_viewing_key) \
    X(note_encryption_kdf_domain_separation)    X(note_encryption_prf_ock_known_answer) \
    X(note_encryption_sapling_kdf_arg_order_distinct)    X(note_encryption_sapling_kdf_avalanche) \
    X(note_encryption_sapling_kdf_known_answer)    X(note_encryption_sprout_kdf_avalanche) \
    X(note_encryption_sprout_kdf_known_answer)    X(note_encryption_sprout_kdf_nonce_sweep) \
    X(zip32_default_diversifier_deterministic)    X(zip32_default_diversifier_is_ff1_of_settled_index) \
    X(zip32_diversifier_advances_index_in_place)    X(zip32_diversifier_distinct_keys_distinct_output) \
    X(zip32_diversifier_index_boundaries)    X(zip32_diversifier_skips_invalid_indices) \
    X(zip32_ff1_radix2_deterministic)    X(script_interp_altstack_conditional) \
    X(script_interp_op2rot_order)    X(script_interp_oppick_bounds) \
    X(script_interp_oproll_semantics)    X(script_interp_optuck_insert) \
    X(script_interp_overflow_boundary) \
    X(wallet_backup) \
    X(wallet_canary) X(wallet_persistence_cycle) \
    X(wallet_flush_rollback) X(log_json) X(http_middleware) \
    X(rpc_timeout) X(wallet_keystore) X(wallet_sqlite_enc) \
    X(zcl_result) X(wallet_sqlite_open_errors) X(watch_only) \
    X(coin_selection) X(disk_monitor) X(db_maintenance) \
    X(mempool_limits) X(addrman_integrity) X(ibd_throttle) \
    X(consensus_reject_events) X(consensus_reject_index) \
    X(chain_rollback) X(alerts) X(ws_events) X(trace) X(phgr13_fix) \
    X(snark_kat) \
    X(no_hardcoded_home) X(cookie_rotation) X(reorg_safety) X(reorg_parity) \
    X(reorg_projection_parity) X(stage_reorg_unwind_parity) \
    X(coins_applied_frontier) \
    X(utxo_apply_value_balance) X(utxo_apply_unspendable) \
    X(utxo_apply_coinbase_maturity) \
    X(connect_block_self_write) X(connect_block_sapling_root) \
    X(connect_block_checkdatasig_sigops) X(invalidateblock) \
    X(key_scrub) X(block_index_loader) X(chain_state_validator) \
    X(utxo_recovery_service) X(utxo_reimport_flag) \
    X(self_heal_scan_fallback) \
    X(rpc_error_envelope) X(tx_property) \
    X(workpool) X(bip113_bip65) X(mempool_orphan) X(fee_estimation) \
    X(header_sync) X(header_sync_stall) X(hd_keychain) X(mnemonic) \
    X(bip44) X(compact_blocks) X(dandelion) X(addrman_rebalance) \
    X(block_pruning) X(schema_migration) X(db_migration_idempotent) \
    X(coins_view_atomicity) X(coins_anchor_reconcile_all) \
    X(coins_best_derivation) \
    X(boot_coins_anchor_dual_store_recovery) X(make_lint_gates) X(multisig) \
    X(mcp_fuzz) X(rpc_auth_hardening) \
    X(disk_block_io) X(msg_handlers) X(process_headers_adversarial) \
    X(chain_advance_coordinator) \
    X(chain_advance_atomicity) \
    X(lag_slo) X(boot_phase) X(path_check) X(supervisor) \
    X(supervisor_domains) X(supervisor_production_tree) \
    X(condition_engine) X(utxo_activation_paused) \
    X(sync_watchdog_conditions) X(sticky_conditions) X(peer_snapshot_conditions) \
    X(snapshot_receive_stalled_condition) \
    X(snapshot_negotiation_stalled_condition) X(snapshot_failed_reset_condition) \
    X(snapshot_complete_resume_condition) X(chain_integrity_failed_condition) \
    X(orphan_utxo_above_tip) \
    X(tip_fork_stale) \
    X(tip_stall_oracle_rebuild_condition) \
    X(body_fetch_missing_have_data_condition) \
    X(stale_validate_headers_repair_condition) \
    X(active_chain_extend) \
    X(rebuild_recent) \
    X(torn_index_blocks_tip) \
    X(have_data_unreadable) \
    X(chain_tip_watchdog_bounded_restart) X(blocker) X(service_state) \
    X(service_state_driver) \
    X(clock) X(rng) X(seed_tape) X(postmortem) X(util_signal_handler) X(chaos_harness) X(stage) X(stage_anchor) X(mailbox) X(mailbox_adoption) \
    X(projection) X(projection_adoption) X(progress_store) X(event_log) \
    X(mempool_projection) X(peers_projection) X(znam_projection) \
    X(wallet_projection) X(small_projections) \
    X(utxo_projection) X(utxo_apply_authorship) X(coins_view_projection) \
    X(coins_view_kv) \
    X(block_index_projection) X(block_index_rebuild) \
    X(block_index_topup) \
    X(projection_replay_invariant) \
    X(header_admit_stage) X(header_probe_poll) \
    X(validate_headers_stage) X(body_fetch_stage) \
    X(body_persist_stage) X(created_outputs_index) X(coins_kv) X(coins_ram) \
    X(seal_kv) X(seal_ratify) \
    X(nullifier_kv) \
    X(script_validate_stage) X(script_validate_contextual_gate) \
    X(proof_validate_stage) X(mint_skip_crypto) \
    X(utxo_apply_stage) X(utxo_apply_crash_replay) \
    X(tip_finalize_stage) X(tip_finalize_post_step) X(reducer_frontier) \
    X(waitforheight_provable) \
    X(refold_progress_floor) X(refold_premature_clear) \
    X(refold_from_anchor_fatal) X(refold_auto_arm) X(anchor_selfmint) \
    X(loader_owns_seed_gate) X(boot_refold_window_extend) \
    X(refold_retro_validate) X(refold_body_span_contiguous) \
    X(contaminated_coin_above_anchor) X(boot_reindex_terminates) \
    X(chain_linkage_check) X(invariant_sentinel) \
    X(seed_integrity_gate) X(mirror_divergence_locator) \
    X(log_throttle) \
    X(reducer_frontier_reconcile_light) \
    X(reducer_stage_fuzz) \
    X(mint_proof_harness) \
    X(reducer_ingest_e2e) X(stage_reducer_unwedge) X(stage_repair) \
    X(stage_repair_coin_backfill) \
    X(stage_anchor_frontier_cap) X(stage_repair_script_refill) \
    X(stage_repair_tipfin_backfill) X(reorg_residue_tipfin_replace) \
    X(utxo_apply_upstream_hole) \
    X(reducer_reconcile_witness) \
    X(reducer_step_drain_harness) \
    X(reducer_ondemand_genesis_seed) \
    X(domain_consensus_verify) X(domain_consensus_subsidy) \
    X(domain_consensus_pow) X(domain_consensus_sigops) \
    X(domain_consensus_script_standard) \
    X(domain_consensus_tx_structural) X(domain_consensus_sapling_structural) \
    X(domain_consensus_sighash) \
    X(domain_consensus_check_block) X(domain_consensus_equihash) \
    X(domain_consensus_script_interp) X(domain_consensus_coins_math) \
    X(domain_consensus_checkpoints) X(domain_consensus_locktime) \
    X(domain_consensus_upgrades) X(domain_consensus_coinbase) \
    X(domain_consensus_header_accept) \
    X(domain_wallet_key_derivation) X(domain_wallet_mnemonic) \
    X(domain_encoding_base58) X(domain_encoding_bech32) \
    X(block_log_file) X(block_log_legacy) X(replay_verify) \
    X(utxo_snapshot_inmem) X(hodl_history_port) X(node_health_store_port) \
    X(db_maintenance_port) X(wallet_backup_port) \
    X(snapshot_store_port) \
    X(block_index_sidecar_port) \
    X(wallet_view_port) \
    X(bg_hash_verify_store_port) \
    X(bg_validation_store_port) \
    X(zslp_store_port) \
    X(sapling_tree) X(heartbeat) X(syncdiag_rpc) X(peer_lifecycle) \
    X(chainstate_legacy_reader) X(ldb_snapshot) X(utxo_snapshot_loader) \
    X(load_verify_boot) \
    X(chain_stall_repro) \
    X(connect_tip_hot_loop_exit) X(connman_addnode_fallback) \
    X(failed_child_cap) X(header_probe) \
    X(power_node_contract_spec) X(process_block_revalidate) \
    X(rpc_safety) X(service_kernel) X(thread_registry) X(sync_state_fsm) \
    X(unclean_shutdown_advance) X(utxo_audit) X(utxo_parity_service) \
    X(soak_attestation) \
    X(zclassicd_oracle) X(oracle_policy) \
    X(script_interp_edge) X(sighash_edge) X(sigops_edge) \
    X(check_tx_edge) X(check_block_edge) X(amount_subsidy_edge) \
    X(locktime_edge) \
    X(pow_diffadj_precedence) X(bip34_coinbase_height_parity) \
    X(reducer_block_ingest_gate) \
    X(onion_bootstrap_slice) X(shielded_receive_slice) X(shielded_receive_persist) \
    X(reducer_forward_progress_gate) X(parity_slice) \
    X(parity_lockin_anchor_membership) X(parity_lockin_contextual_header) \
    X(coins_kv_reset_for_reseed) X(reindex_epilogue) \
    X(replay_canary_verdict) \
    X(canary_sentinel_watch) X(seed_torn_import_gate) \
    X(shielded_spend_slice) X(atomic_commit_ordering) \
    X(coldimport_restart_fragility) \
    X(block_parse_cache)

#define SPEC_LIST(X) \
    X(wallet_dashboard) X(wallet_send) X(wallet_receive) \
    X(wallet_shield) X(wallet_node) X(wallet_history) \
    X(wallet_coins) X(wallet_pulse) X(wallet_tx_detail) \
    X(wallet_navigation) X(wallet_errors) X(wallet_privacy) \
    X(wallet_sovereignty) X(wallet_celebration) \
    X(wallet_empowerment) X(wallet_flow) X(wallet_accessibility) \
    X(data_hooks) X(event_observers) X(state_machine) \
    X(ux_sierra) X(html_quality) X(user_journeys) X(e2e_wallet) \
    X(render_audit) X(smoke) X(100_stories) X(consensus_compat)

/* Forward declarations */
#define DECL_TEST(name) extern int test_##name(void);
TEST_LIST(DECL_TEST)
#undef DECL_TEST

#define DECL_SPEC(name) extern int spec_##name(void);
SPEC_LIST(DECL_SPEC)
#undef DECL_SPEC

struct test_group {
    const char *name;
    int (*fn)(void);
};

static const struct test_group g_groups[] = {
#define ROW_TEST(name) {"test_" #name, test_##name},
    TEST_LIST(ROW_TEST)
#undef ROW_TEST
#define ROW_SPEC(name) {"spec_" #name, spec_##name},
    SPEC_LIST(ROW_SPEC)
#undef ROW_SPEC
};

static const size_t g_num_groups =
    sizeof(g_groups) / sizeof(g_groups[0]);

/* ── Parent-side worker pool ───────────────────────────────────────*/

struct child_slot {
    pid_t pid;           /* 0 if slot is free */
    size_t group_idx;    /* index into g_groups for the running group */
    char out_path[128];  /* tempfile path for this child's stdout+stderr */
};

struct group_result {
    int status;          /* -1 if not yet run, else wait-status from waitpid */
    int signaled;        /* 1 if killed by a signal */
    int exit_code;       /* only valid if signaled == 0 */
    double wall_seconds; /* 0 until measured */
    time_t start;
    char out_path[128];  /* owned by the slot; copied here on reap */
    int skipped;         /* 1 if excluded by --only=SUBSTR (not run) */
    int skip_markers;    /* "SKIP (" sentinel lines in captured output */
};

static int get_nproc(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) return 1;
    if (n > 1024) return 1024;
    return (int)n;
}

static void child_run(size_t idx, const char *out_path)
{
    /* Redirect stdout + stderr to our tempfile. */
    int fd = open(out_path,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        /* Can't redirect — run anyway; parent will see empty output. */
        perror("test_parallel: open tempfile");
    } else {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();
    event_log_init();

    int failures = g_groups[idx].fn();

    ecc_verify_destroy();
    ecc_stop();

    fflush(stdout);
    fflush(stderr);
    _exit(failures ? 1 : 0);
}

static int find_slot_by_pid(struct child_slot *slots, int jobs, pid_t pid)
{
    for (int i = 0; i < jobs; i++)
        if (slots[i].pid == pid) return i;
    return -1;
}

static int find_free_slot(struct child_slot *slots, int jobs)
{
    for (int i = 0; i < jobs; i++)
        if (slots[i].pid == 0) return i;
    return -1;
}

static void make_tempfile_path(char *buf, size_t sz, size_t idx, pid_t parent)
{
    snprintf(buf, sz, "./test-tmp/test_parallel_%lld_%zu.log",
             (long long)parent, idx);
}

static void ensure_tmp_dir(void)
{
    struct stat st;
    if (stat("./test-tmp", &st) == 0) return;
    if (mkdir("./test-tmp", 0755) != 0 && errno != EEXIST)
        perror("test_parallel: mkdir ./test-tmp");
}

static void print_captured(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("  (captured output missing at %s)\n", path);
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
    }
    fclose(fp);
}

/* Count lines carrying the suite's skip sentinel ("SKIP ("). Gated
 * groups (the five ZCL_STRESS_TESTS MVP acceptance gates, the stress
 * harnesses) and environment-starved subtests print it and still exit
 * 0, so a green run can hide unexecuted coverage. The summary counts
 * the markers so "ALL TESTS PASSED" can never silently absorb a skip;
 * the gates themselves stay opt-in (they are gated for runtime
 * reasons — visibility, not force-enabling, is the contract). */
static int count_skip_markers(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[4096];
    int n = 0;
    while (fgets(line, sizeof(line), fp))
        if (strstr(line, "SKIP ("))
            n++;
    fclose(fp);
    return n;
}

/* test_make_lint_gates plants real *.c fixture files into the live source
 * tree at fixed paths (app/jobs/src/, domain/consensus/src/, ...) and then
 * unlink()s them. Two of those paths sit INSIDE other groups' lint scan
 * surfaces (the E5 fixture under app/jobs, the domain-purity fixture under
 * domain/consensus), so if any other group's gate script greps/finds the
 * tree concurrently it can readdir a fixture and then have open() race the
 * unlink — grep exits 2, the gate treats >=2 as FATAL. The cure is to run
 * this group ALONE (run_group_exclusive, a synchronous pre-pass) before the
 * parallel pool dispatches anything, so no concurrent scanner can ever
 * observe a transient fixture.
 *
 * The group's stored name carries the ROW_TEST "test_" prefix
 * ("test_make_lint_gates"), so compare on the SUFFIX after that prefix —
 * a bare strcmp against "make_lint_gates" silently never matched and left
 * the guard dead (the group ran in the 32-worker pool and flaked the
 * suite). Stripping the prefix here keeps the predicate matching whether
 * the caller passes the stored name or the bare group name. */
static bool group_requires_exclusive_repo(const char *name)
{
    if (!name) return false;
    if (strncmp(name, "test_", 5) == 0) name += 5;
    return strcmp(name, "make_lint_gates") == 0;
}

static void run_group_exclusive(size_t idx, pid_t parent_pid,
                                struct group_result *results)
{
    char out_path[128];
    make_tempfile_path(out_path, sizeof(out_path), idx, parent_pid);
    results[idx].start = platform_time_wall_time_t();

    pid_t pid = fork();
    if (pid < 0) {
        perror("test_parallel: exclusive fork");
        results[idx].status = 1;
        results[idx].signaled = 0;
        results[idx].exit_code = 2;
        return;
    }
    if (pid == 0) {
        child_run(idx, out_path);
        _exit(2); /* unreachable */
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("test_parallel: exclusive waitpid");
        status = 1;
        break;
    }

    results[idx].status = status;
    if (WIFSIGNALED(status)) {
        results[idx].signaled = 1;
        results[idx].exit_code = WTERMSIG(status);
    } else {
        results[idx].signaled = 0;
        results[idx].exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    memcpy(results[idx].out_path, out_path, sizeof(results[idx].out_path));
    time_t now = platform_time_wall_time_t();
    results[idx].wall_seconds = (double)(now - results[idx].start);
}

int main(int argc, char **argv)
{
    int jobs = get_nproc();
    int timeout_secs = 300; /* per-group; generous so slow groups like
                             * test_merkle_tree (~110s standalone) don't
                             * get cut off the first time a machine is
                             * loaded. */
    bool verbose = false;
    bool list_only = false;
    const char *only = NULL; /* --only=SUBSTR: run just matching groups */

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--jobs=", 7) == 0) {
            jobs = atoi(argv[i] + 7);
            if (jobs < 1) jobs = 1;
        } else if (strncmp(argv[i], "-j", 2) == 0 && argv[i][2]) {
            jobs = atoi(argv[i] + 2);
            if (jobs < 1) jobs = 1;
        } else if (strncmp(argv[i], "--timeout=", 10) == 0) {
            timeout_secs = atoi(argv[i] + 10);
            if (timeout_secs < 1) timeout_secs = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else if (strncmp(argv[i], "--only=", 7) == 0) {
            only = argv[i] + 7;
        } else {
            fprintf(stderr,
                    "Usage: %s [--jobs=N] [--timeout=SECS] [--verbose] "
                    "[--list] [--only=SUBSTR]\n",
                    argv[0]);
            return 2;
        }
    }

    if (list_only) {
        for (size_t i = 0; i < g_num_groups; i++)
            printf("%s\n", g_groups[i].name);
        return 0;
    }

    ensure_tmp_dir();

    setbuf(stdout, NULL);
    printf("test_parallel: %zu groups, %d workers, %ds per-group timeout\n",
           g_num_groups, jobs, timeout_secs);

    struct group_result *results =
        calloc(g_num_groups, sizeof(*results));
    if (!results) {
        fprintf(stderr, "test_parallel: calloc failed\n");
        return 1;
    }
    for (size_t i = 0; i < g_num_groups; i++) {
        results[i].status = -1;
    }

    /* --only=SUBSTR: pre-mark non-matching groups as already-reaped so
     * they are neither dispatched nor counted. Lets a dev iterate on one
     * group in ~seconds instead of waiting on the slowest group (the long
     * pole is test_merkle_tree, ~110s). */
    size_t pre_skipped = 0;
    if (only) {
        for (size_t i = 0; i < g_num_groups; i++) {
            if (!strstr(g_groups[i].name, only)) {
                results[i].status = 0; /* excludes from dispatch loop */
                results[i].skipped = 1;
                pre_skipped++;
            }
        }
        if (pre_skipped == g_num_groups) {
            fprintf(stderr,
                    "test_parallel: --only=%s matched no groups\n", only);
            free(results);
            return 2;
        }
    }

    struct child_slot *slots =
        calloc((size_t)jobs, sizeof(*slots));
    if (!slots) {
        fprintf(stderr, "test_parallel: slot calloc failed\n");
        free(results);
        return 1;
    }

    struct timespec t_start;
    platform_time_monotonic_timespec(&t_start);

    pid_t parent_pid = getpid();
    size_t reaped = pre_skipped;

    for (size_t i = 0; i < g_num_groups; i++) {
        if (results[i].skipped)
            continue;
        if (!group_requires_exclusive_repo(g_groups[i].name))
            continue;
        if (verbose)
            printf("[exclusive] [%zu/%zu] %s\n",
                   i + 1, g_num_groups, g_groups[i].name);
        run_group_exclusive(i, parent_pid, results);
        reaped++;
    }

    size_t next_idx = 0;

    while (reaped < g_num_groups) {
        /* Dispatch as many children as we have free slots and
         * remaining groups. */
        while (next_idx < g_num_groups) {
            if (results[next_idx].status != -1) {
                next_idx++;
                continue;
            }
            int slot = find_free_slot(slots, jobs);
            if (slot < 0) break;

            make_tempfile_path(slots[slot].out_path,
                               sizeof(slots[slot].out_path),
                               next_idx, parent_pid);
            results[next_idx].start = platform_time_wall_time_t();

            pid_t pid = fork();
            if (pid < 0) {
                perror("test_parallel: fork");
                break;
            }
            if (pid == 0) {
                child_run(next_idx, slots[slot].out_path);
                _exit(2); /* unreachable */
            }
            slots[slot].pid = pid;
            slots[slot].group_idx = next_idx;
            if (verbose) {
                printf("[dispatch] [%zu/%zu] pid=%d %s\n",
                       next_idx + 1, g_num_groups, pid,
                       g_groups[next_idx].name);
            }
            next_idx++;
        }

        /* Enforce the per-group timeout by SIGKILLing any slot whose
         * child has been running longer than `timeout_secs`. The kill
         * flows into the normal reap path below. */
        time_t now_tick = platform_time_wall_time_t();
        for (int i = 0; i < jobs; i++) {
            if (slots[i].pid == 0) continue;
            if (now_tick - results[slots[i].group_idx].start > timeout_secs) {
                if (verbose) {
                    printf("[timeout ] [%zu] %s (after %ds)\n",
                           slots[i].group_idx,
                           g_groups[slots[i].group_idx].name,
                           timeout_secs);
                }
                kill(slots[i].pid, SIGKILL);
            }
        }

        /* Wait for any child to finish, with a 1-second poll ceiling
         * so the timeout sweep above runs on a regular cadence even
         * when children are long-running. */
        int status = 0;
        pid_t done = waitpid(-1, &status, WNOHANG);
        if (done == 0) {
            struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
            nanosleep(&ts, NULL);
            continue;
        }
        if (done < 0) {
            if (errno == EINTR) continue;
            perror("test_parallel: waitpid");
            break;
        }
        int slot = find_slot_by_pid(slots, jobs, done);
        if (slot < 0) {
            /* Unknown pid — ignore. */
            continue;
        }
        size_t idx = slots[slot].group_idx;
        results[idx].status = status;
        if (WIFSIGNALED(status)) {
            results[idx].signaled = 1;
            results[idx].exit_code = WTERMSIG(status);
        } else {
            results[idx].signaled = 0;
            results[idx].exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        memcpy(results[idx].out_path, slots[slot].out_path,
               sizeof(results[idx].out_path));
        time_t now = platform_time_wall_time_t();
        results[idx].wall_seconds = (double)(now - results[idx].start);

        if (verbose) {
            printf("[done    ] [%zu] %s (%s, %.0fs)\n",
                   idx, g_groups[idx].name,
                   results[idx].signaled ? "SIGNALED" :
                   (results[idx].exit_code == 0 ? "PASS" : "FAIL"),
                   results[idx].wall_seconds);
        }

        slots[slot].pid = 0;
        reaped++;
    }

    /* All children reaped — replay their output in group order. */
    struct timespec t_end;
    platform_time_monotonic_timespec(&t_end);
    double wall =
        (double)(t_end.tv_sec - t_start.tv_sec) +
        (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    int failed_groups = 0;
    int skip_groups = 0;
    for (size_t i = 0; i < g_num_groups; i++) {
        if (results[i].skipped) continue;
        bool pass =
            !results[i].signaled && results[i].exit_code == 0;
        results[i].skip_markers = results[i].out_path[0]
            ? count_skip_markers(results[i].out_path) : 0;
        if (results[i].skip_markers > 0) skip_groups++;
        char skip_note[32] = "";
        if (results[i].skip_markers > 0)
            snprintf(skip_note, sizeof(skip_note), ", %d SKIP",
                     results[i].skip_markers);
        printf("\n==================== %s (%s%s, %.0fs) ====================\n",
               g_groups[i].name,
               results[i].signaled ? "SIGNALED" : (pass ? "PASS" : "FAIL"),
               skip_note, results[i].wall_seconds);
        if (!pass) failed_groups++;
        print_captured(results[i].out_path);
        if (pass && results[i].out_path[0]) unlink(results[i].out_path);
    }

    printf("\n%s — %d/%zu groups failed, %d skipped (%.1fs wall, %d workers)%s\n",
           failed_groups == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED",
           failed_groups, g_num_groups - pre_skipped, skip_groups, wall, jobs,
           only ? " [--only filtered]" : "");
    if (skip_groups > 0) {
        printf("Skipped coverage (self-skipped groups — most need "
               "ZCL_STRESS_TESTS=1 + an isolated run):\n");
        for (size_t i = 0; i < g_num_groups; i++) {
            if (results[i].skipped || results[i].skip_markers == 0) continue;
            printf("  - %s: %d skip marker(s)\n",
                   g_groups[i].name, results[i].skip_markers);
        }
    }
    if (failed_groups > 0) {
        printf("Failed groups:\n");
        for (size_t i = 0; i < g_num_groups; i++) {
            if (results[i].skipped) continue;
            bool pass =
                !results[i].signaled && results[i].exit_code == 0;
            if (pass) continue;
            printf("  - %s: %s",
                   g_groups[i].name,
                   results[i].signaled ? "signaled" : "exit");
            if (results[i].signaled)
                printf(" signal=%d", results[i].exit_code);
            else
                printf(" code=%d", results[i].exit_code);
            if (results[i].out_path[0])
                printf(" log=%s", results[i].out_path);
            printf("\n");
        }
    }

    free(slots);
    free(results);
    return failed_groups == 0 ? 0 : 1;
}
