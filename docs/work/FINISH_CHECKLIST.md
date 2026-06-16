# ZClassic23 — Finish Checklist

> Generated 2026-06-02 by a 9-dimension multi-agent audit (11 agents, 56 verified
> findings → 41 deduped items). This is the authoritative "what is left to make the
> software finished/perfect" board. Rubric = the owner doctrine: errors fail loudly,
> the architecture solves problems by construction, subtract before adding, baselines
> ratchet to zero, 800-line ceiling. Every item names concrete files + verified evidence.
> Check items off as landed; keep severity/effort tags. Companion to `docs/REFACTOR_STATUS.md`.

## Status

The 45 [x] items below are landed (durable record = the [x] + commit history +
`docs/REFACTOR_STATUS.md`). **Remaining real work = the 6 open [ ] items** (the §3
live-tip contiguity window + the two defer-by-design §5 items): implement → validate
on a datadir COPY → owner-gated deploy.

**§3 PIVOTAL FINDING — the live wedge is NOT §3.1.** §3.1 (boot seed-tip-from-finalized
cursor) is DONE; a fresh boot reconciles the active tip to coins_best+1 on its own, so
the seed correctly no-ops. The real blocker is a runtime tip-rewind: tip_finalize's
reorg detector false-fires on `tip_window_holes` → §3.2/§3.3 window-contiguity. (The
live wedge has since been re-root-caused further — see MEMORY / `FORWARD_PLAN.md`.)

## 1. Loud failures & silent-halt elimination

- [x] **P0** (S) Stage runner swallows JOB_FATAL with no LOG_ — `change` — files: `lib/util/src/stage.c`.
- [x] **P0** (M) Drain/driver cannot distinguish JOB_FATAL from JOB_IDLE — `change` — files: `app/jobs/include/jobs/job.h`, `app/services/src/chain_activation_controller.c` — surface FATAL distinctly or emit EV_OPERATOR_NEEDED.
- [x] **P1** (S) snapshot_fetch.c "LOG then fall through" → NULL-deref / double-unlock — `change` — files: `app/services/src/snapshot_fetch.c`.
- [x] **P1** (M) Supervisor-registration gate globs `*_service.c` only — 3 live unsupervised loops evade it — `change` — files: `tools/scripts/check_supervisor_registration.sh`, `app/services/src/disk_monitor.c`, `app/services/src/db_maintenance.c`, `app/services/src/mempool_limits.c`, `config/src/boot.c` — broaden glob to `*.c` under app/services/src + add a liveness_contract + `supervisor_register_in_domain` to each background thread (boot.c:1003/1090, boot_services.c:259).
- [x] **P1** (M) Add silent-error lint gates for app/jobs/src and app/conditions/src — `add` — files: `Makefile`, `app/jobs/src/script_validate_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/conditions/src/stale_validate_headers_repair.c` — mirror check-silent-errors-services/-controllers for jobs+conditions.
- [x] **P2** (S) scan_util.h: two unchecked `realloc()` leak + NULL-deref on OOM — `change` — files: `app/controllers/include/controllers/scan_util.h`, `app/controllers/src/wallet_scan.c`, `app/controllers/src/legacy_import.c` — route through `zcl_realloc`.
- [x] **P2** (S) Alloc lint gates (check-malloc, check-raw-malloc) scan only `*.c` — header inline allocs evade them — `change` — files: `Makefile`, `tools/scripts/check_raw_malloc.sh`, `app/controllers/include/controllers/scan_util.h` — add `--include='*.h'`.
- [x] **P3** (S) Silent-error gate accepts any prev-line `printf`, not only error-logging — `change` — files: `Makefile` — tighten allowed-prefix regex to LOG_ERR/LOG_FAIL/LOG_RETURN/log_json(level=error).

## 2. Consensus validation loudness/correctness

- [x] **P0** (L) ROOT CAUSE: script_validate prevout resolver is never wired → universal `internal_error` — `investigate` — files: `app/jobs/src/script_validate_stage.c`, `app/supervisors/src/staged_sync_supervisor.c`, `config/src/boot.c` — wire `script_validate_stage_set_prevout_resolver()` (had zero callers) to a `body_persist`-maintained forward `created_outputs_index` `(txid,vout)→{value,scriptPubKey,height}`, correct-by-construction (body_persist.cursor > script_validate.cursor), fail-loud on miss.
- [x] **P0** (M) script_validate `internal_error` is reasonless — conflates null-block, prevout-unresolved, real script-invalid — `change` — files: `app/jobs/src/script_validate_stage.c` — split into typed reasons (`block_decode_failed` vs `prevout_unresolved tx=<txid> vin=<n>`), put cause+txid+vin in EV_BLOCK_REJECTED.
- [x] **P1** (M) verify_script ScriptError captured but discarded; no `ScriptErrorString` mapper exists — `change` — files: `app/jobs/src/script_validate_stage.c`, `lib/script/include/script/script_error.h`.
- [x] **P1** (M) Per-height failure reason columns (`first_failure_*`) are write-only — no dump_state/controller/MCP reader — `add` — files: `app/jobs/src/script_validate_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/jobs/src/utxo_apply_stage.c`.
- [x] **P1** (S) tip_finalize `precondition_failed` does not say WHICH precondition — `change` — files: `app/jobs/src/tip_finalize_stage.c` — emit a reason token (`have_data_missing|not_script_valid|not_header_valid|chainwork_not_greater`).
- [x] **P2** (S) proof_validate failure carries proof-type but not txid/reason to any operator surface — `change` — files: `app/jobs/src/proof_validate_stage.c`.
- [x] **P2** (S) utxo_apply lookup-callback failure recorded as generic `internal_error`/`lookup` with no log — `change` — files: `app/jobs/src/utxo_apply_delta.c`, `app/jobs/src/utxo_apply_stage.c`.

## 3. Active-chain window & CSR-consistency architecture

- [x] **P0** (L) Boot does not seed active tip from the durable finalized cursor → finalized work dropped on reboot — `change` — files: `config/src/boot.c`, `app/services/src/block_index_loader_rebuild.c`, `main.c` — `block_index_loader_seed_tip_from_finalized` (read MAX finalized height from tip_finalize_log, `chain_set_active_tip`), establishes csr_consistent=true by construction. Prerequisite for the items below.
- [ ] **P1** (M) Replace generic best-header window extender with the contiguous have-data/contiguity frontier — `change` — files: `app/jobs/include/jobs/stage_helpers.h`, `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h` — route all 8 `reducer_extend_window_to_candidate` stage call sites through `active_chain_extend_window_have_data`, passing utxo_apply's cursor as max_height, so H+1 can never be a fork/header-only successor.
- [ ] **P1** (M) tip_finalize one-block lookahead reads a mutable H+1 slot a generic extender re-fills — `change` — files: `app/jobs/src/tip_finalize_stage.c`, `app/jobs/include/jobs/stage_helpers.h` — change the lookahead window source to the contiguous finalized frontier so `finalized_row_active_match` can't false-fire and `rewind_cursor_if_active_chain_reorged` becomes assert-only (kills the live reorg_detected churn). Depends on the contiguity extender above.
- [ ] **P2** (M) Tighten `chain_evidence_controller_reconcile_startup` from "defer to next commit" to fail-loud — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_restore_repair.c` — after the boot tip is seeded from the finalized cursor (#1), a residual coins/active divergence is genuine corruption → LOG_ERR/EV_OPERATOR_NEEDED, not silent INFO defer.
- [ ] **P2** (S/L) Resolve `active_chain_extend_window_have_data` to exactly ONE window authority (wire it, delete the generic) — `investigate` — files: `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h`, `lib/test/src/test_active_chain_extend.c` — confirm bounded scan (`MAX_GAP=8192`) cost on a 3M-entry map under supervisor tick and that anchored extend still exposes legitimate higher-work reorgs; then delete `active_chain_extend_window` + the best_header path.

## 4. Legacy engine deletion (single-engine purge)

- [x] **P1** (M) Delete dead dual-write UTXO authorship branch (`utxo_projection_set_author` has zero prod callers) — `remove` — files: `lib/storage/src/utxo_projection.c`, `lib/storage/include/storage/utxo_projection.h`, `lib/validation/src/update_coins.c`, `lib/validation/include/validation/update_coins.h` — deleted only the dead emit branch + 4 call sites; the UTXO_AUTHOR enum + get_author() are LIVE (utxo_apply reads `get_author()==STAGE` for DRIVER vs FOLLOWER reorg-unwind) → RETAINED.
- [x] **P1** (M) Delete `coins_view_stage_backing.c` — entirely test-only dual-read/dual-write residue — `remove` — files: `lib/storage/src/coins_view_stage_backing.c`, `lib/storage/include/storage/coins_view_stage_backing.h`.
- [x] **P2** (M) Delete orphaned missing-UTXO self-heal recovery island (471 LOC, dispatcher removed in c0fc39749) — `remove` — files: `lib/validation/src/process_block_self_heal_chain_scan.c`, `lib/validation/src/process_block_self_heal_sqlite_tx_index.c`, `lib/validation/src/process_block_self_heal_legacy_rpc.c`, `lib/validation/src/process_block_self_heal_inject.c`, `lib/validation/src/process_block_self_heal.c` — strip matching content asserts in `lib/test/src/test_make_lint_gates.c`. KEEP `process_block_self_heal_hot_loop.c` and `_scan_state.c` (live).
- [x] **P2** (M) Audit/narrow process_block.h legacy-shaped surface (`g_body_pull_active` etc.) — `investigate` — files: `lib/validation/include/validation/process_block.h`, `lib/validation/src/process_block.c`, `app/jobs/src/tip_finalize_post_step.c`.
- [x] **P2** (M) Decide fate of `g_live_check` projection-vs-coins.db parity hook — `investigate` — files: `app/jobs/src/utxo_apply_stage.c`.
- [x] **P3** (S) Fix stale comments claiming `UTXO_AUTHOR_LEGACY` is "the default" (real default is STAGE) — `change` — files: `app/jobs/src/utxo_apply_delta.c`, `lib/storage/include/storage/utxo_projection.h`.
- [x] **P3** (S) Document `process_block_self_heal_legacy_rpc.c` keep-status OR fold into the island delete — `change` — files: `lib/validation/src/process_block_self_heal_legacy_rpc.c` — record the outcome in LEGACY_LIFECYCLE.md.
- [x] **P3** (S) Restore a single-engine completion note OR scrub dangling B3/B8/DRIVER-FOLLOWER labels — `change` — files: `docs/work/single-engine-newcode-plan.md`, `docs/work/single-engine-newcode-design.json`.

## 5. Eight-shape conformance & file splits

- [x] **P1** (L) Migrate the Job reducer pipeline's raw Model-shape persistence off the non-ratcheting `raw-sql-ok` hatch — `investigate` — files: `app/jobs/src/stage_repair.c`, `app/jobs/src/utxo_apply_delta.c`, `app/jobs/src/validate_headers_report.c`, `app/jobs/src/tip_finalize_log_store.c`, `app/jobs/src/utxo_apply_stage.c`, `app/jobs/src/validate_headers_stage.c`, `app/jobs/src/header_admit_stage.c`, `app/jobs/src/body_persist_stage.c`, `app/jobs/src/body_fetch_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/jobs/src/script_validate_stage.c` — the reducer Jobs write to the progress.kv KERNEL store below the AR layer (routing through AR is a category error); resolved by a principled hatch documented in DEFENSIVE_CODING.md + one canonical `raw-sql-ok:progress-kv-kernel-store` tag.
- [x] **P2** (M) Split `chain_activation_controller.c` (mixed activation state-machine + reducer ingest, exactly 800 lines) — `change` — files: `app/services/src/chain_activation_controller.c` — extract the reducer-ingest half (lines 560-782) into `reducer_ingest_service.c`.
- [ ] **P2** (M) Pre-emptively split files at/near the E1 800-line ceiling (empty baseline, zero headroom) — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c`, `app/jobs/src/utxo_apply_delta.c`, `app/models/src/wallet_tx.c`, `app/models/src/database_migrate.c`, `app/views/src/explorer_pages_view.c`, `app/controllers/src/sync_controller_import.c`, `app/controllers/src/sync_controller_catchup.c`, `app/services/src/bg_validation_service.c`, `app/jobs/src/stage_repair.c` — any single added line trips CI with no inline override; split toward single-responsibility before an emergency baseline. **Deferred by design:** the E1 baseline is already ZERO (no file is over the ceiling); splitting under-ceiling files purely for headroom is churn against "subtract / less is more" — re-open only when a specific file actually needs to grow past 800.
- [x] **P2** (M) Split new `stage_repair.c` (754 lines, 9 raw-sql-ok hatches, ≥3 repair concerns) — `change` — files: `app/jobs/src/stage_repair.c` — split 754→100 + 3 focused TUs; destructive poison-rewind guards preserved byte-for-byte.
- [x] **P2** (L) Extract orchestration out of large "Controllers" (import/sync) into Service/Job shape — `investigate` — files: `app/controllers/src/sync_controller_import.c`, `app/controllers/src/sync_controller_catchup.c`, `app/controllers/src/legacy_import.c` — extracted the 619-line cold-import orchestration behind `legacy_import_service_run()`; legacy_import.c is a 31-line shim keeping the public symbol.
- [ ] **P3** (S) Rename Service files carrying contradicting shape names (`*_controller`, `*_repository` → `*_service`) — `change` — files: `app/services/src/chain_activation_controller.c`, `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c` — names mislead the "folder shape == file shape" PR check; `chain_state_repository.c` also does direct sqlite3 (deeper Model/Storage-Adapter boundary question). **Deferred by design:** pure cosmetics over a passing framework-shape gate with a high-churn blast radius (`chain_evidence_controller` = 197 symbol sites across 14 files + diagnostics-key coupling); `chain_state_repository.c` is a deliberate consensus-critical Repository pattern (the single-writer guarding the 1.3M-UTXO loss), its raw sqlite resolved correct-by-design.

## 6. Dead code & unwired seams

- [x] **P3** (S) Fix stale process_block.h doc claiming a dead self-heal producer chain is live — `change` — files: `lib/validation/include/validation/process_block.h`.
- [x] **P3** (S) Correct stale note: generic `lib/util/projection.c` API is wired, not an "unwired WIP seam" — `change` — files: `lib/util/src/projection.c`, `lib/util/include/util/projection.h`.
- [x] **P3** (S) Audit `stage_anchor_upstream_cursors_to` test coverage — `investigate` — files: `app/jobs/src/stage_anchor.c`, `app/jobs/src/tip_finalize_stage.c`.

## 7. Lint baselines & defensive gates → zero

- [x] **P2** (S) observability-pairing gate scans committed HEAD-vs-merge-base only — working tree unscanned — `change` — files: `tools/check_observability_pairing.c` — add `git diff` (unstaged) + `--cached` to the scan set.
- [x] **P2** (M) Drive the file-size E1 ceiling and supervisor-gate glob baselines to zero — `change` — files: `tools/scripts/check_file_size_ceiling.sh`, `tools/scripts/check_supervisor_registration.sh`.

## 8. Test coverage

- [x] **P1** (S) Register two condition tests in test_parallel — `add` — for `body_fetch_missing_have_data_condition` + `stale_validate_headers_repair_condition`. A full test.c-vs-test_parallel diff found **15** `failures += test_X()` with NO X() in test_parallel: `chain_advance_atomicity, chain_tip, checkpoint, cold_start_sync, kill9_recovery, ldb_snapshot, onion_bootstrap, pprev_walk, sapling_lazy_init, sha3_windows, shielded_payment_gate, soak_harness, store_e2e_gate, thread_registry, utxo_snapshot_loader` — needs per-test triage + a registry-sync gate WITH a documented exclusion allowlist (a stateful test in the fork-parallel runner can flake).
- [x] **P1** (M) Add test_stage_repair.c covering the destructive poison-rewind safety guards — `add` — files: `app/jobs/src/stage_repair.c`, `lib/test/src/test_stale_validate_headers_repair_condition.c` — assert reject-when-ok=1-row-at/above-frontier (the ~47279 regression guard), non-frontier reject, POISON_NONE no-op, and header save/load hash-mismatch + oversized-solution rejection.
- [x] **P2** (S) `ZCL_TEST_ONLY=parity_diff` subset is a no-op returning a false green — `change` — files: `lib/test/src/test.c` — wire it to a real parity gate.
- [x] **P2** (M) Add test_oracle_policy.c (gates live chain extension, zero test references) — `add` — files: `app/services/src/oracle_policy.c`, `app/services/include/services/oracle_policy.h`.
- [x] **P3** (S) Add stage_anchor focused test — `investigate` — files: `app/jobs/src/stage_anchor.c`, `lib/test/src/test_tip_finalize_stage.c` — assert cursors advance atomically to target and never rewind below current.

## 9. Docs / build / release

- [x] **P1** (M) Add a reproducible release build profile — `change` — files: `tools/release.sh`, `Makefile` — drop `-march=native` (pin `-march=x86-64-v3`), add `-Wl,--build-id=none`, set `SOURCE_DATE_EPOCH`, deterministic `tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner`.
- [x] **P2** (S) Make unsigned release a hard failure (not a silent GPG skip) — `change` — files: `tools/release.sh` — require a key or explicit `--unsigned`.
- [x] **P2** (S) Fix CLAUDE.md MCP tool-count self-contradiction (100+ vs 98; real=98) — `change` — files: `CLAUDE.md` — add a build/test assertion so the count can't drift.
- [x] **P2** (S) Fix CLAUDE.md "statically-linked" claim — binary is dynamically linked — `change` — files: `CLAUDE.md` — change to "self-contained ~15 MB C23 binary".
- [x] **P3** (S) BUILDINFO comment claims it records "flags" but it does not — `change` — files: `tools/release.sh`.
- [x] **P3** (M) Add a CI workflow running `make ci` + a reproducible-build SHA3 cross-runner diff — `add` — files: `Makefile`, new `.github/workflows/`.

## 10. Other (critic gaps)

- [x] **P2** (M) Register disk_monitor / db_maintenance / mempool_limits in the supervisor liveness tree — `investigate` — files: `config/src/boot.c`, `app/services/src/disk_monitor.c`, `app/services/src/db_maintenance.c`, `app/services/src/mempool_limits.c` — these spawn infinite loops via `thread_registry_spawn_ex` (boot.c:1003/1090) with zero `supervisor_register`; add a liveness_contract + progress marker each. (Lands with §1's gate-glob fix.)

## Counts

Total items: 41 — By severity: **P0×7, P1×15, P2×14, P3×5** — By action: remove×3, change×27, add×6, investigate×5 (action = dominant verb per merged item).

## Critical path (do in this order)

1. **§2 #1 (P0)** wire the script_validate prevout resolver — the actual root cause of the live finalization stall AND a "finished" defect (a seam exists, was never connected).
2. **§3 #1 (P0)** seed the boot active tip from the finalized cursor — makes csr_consistent=true by construction; unblocks §3.
3. **§1 #1–2 + §2 #2,#5 (P0)** make every reducer failure loud and specific — so the next blocker names itself.
4. Then §3 #2–3 (contiguity window), §4 deletions (subtract), §5 splits, §7 baselines→0, §8 tests, §9 release.
