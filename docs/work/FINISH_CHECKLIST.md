# ZClassic23 — Finish Checklist

> Generated 2026-06-02 by a 9-dimension multi-agent audit (11 agents, 56 verified
> findings → 41 deduped items). This is the authoritative "what is left to make the
> software finished/perfect" board. Rubric = the owner doctrine: errors fail loudly,
> the architecture solves problems by construction, subtract before adding, baselines
> ratchet to zero, 800-line ceiling. Every item names concrete files + verified evidence.
> Check items off as landed; keep severity/effort tags. Companion to `docs/REFACTOR_STATUS.md`.

## Progress log

- **2026-06-02 · Wave 1 (commit `287a5a478`) — loud failures + consensus loudness.** 9 items
  landed green (build rc=0, test_parallel 0/284, lint rc=0, zero validation-semantics change):
  §1.1, §1.2, §1.3 (verified already-fixed by returning macros — closed), §1.6, §2.2, §2.4
  (script/proof/utxo dump_state readers), §2.5, §2.6, §2.7. Every reducer FATAL is now loud +
  latched; the drain emits EV_OPERATOR_NEEDED on a FATAL-masquerading-as-idle pass; every stage
  failure now carries height/txid/vin/reason to node.log + zcl_state.
  **Remaining in §1:** §1.4, §1.5, §1.7, §1.8 (gate work). **Remaining in §2:** §2.1 (P0 prevout
  resolver — the live root cause), §2.3 (ScriptErrorString mapper).

- **2026-06-02 · §2.1 prevout resolver (commit `2e1db80e2`) — the P0 root cause.** Built design (B):
  a `body_persist`-maintained forward creation index `(txid,vout)→{value,scriptPubKey,height}`,
  queried by a layered resolver (index → seeded `utxo_projection` fallback), wired as the production
  default in `script_validate_stage_init`. Correct-by-construction (body_persist.cursor >
  script_validate.cursor), fail-loud on miss, `verify_script` untouched. New `test_created_outputs_index`
  (17 checks) + registered in all 3 harness sites → build/lint/`test_parallel` **0/285**. NOTE: live
  diagnosis shows the *current* live blocker is `tip_finalize` precondition_failed (249×), i.e. §3 —
  §2.1 fixed a latent defect (the resolver was unwired; any transparent-spend block would wedge it).
  Live forward-progress proof pairs with §3.1.
- **Also:** CLAUDE.md accuracy (static→dynamic, tool-count) committed separately.
- **2026-06-02 · §8.1 (commit `e89ffc228`):** registered 2 silently-skipped condition tests in
  test_parallel (0/285→0/287); recorded the 15-test parallel-skip finding for a triage pass.
- **2026-06-02 · Wave 2 (commit `1cef5fe01`) — subtraction + hardening, −968 LOC net.** §4.2
  (delete coins_view_stage_backing), §4.3 (delete 4 orphaned self-heal recovery sources;
  prove-before-delete KEPT process_block_self_heal.c — it is load-bearing), §6.1, §6.2, §9.2
  (unsigned release now hard-fails), §9.5 (BUILDINFO records flags). build/lint/test_parallel 0/287.
  **§4 remaining:** §4.1 (UTXO author dead branch — touches connect_block consensus), §4.4/§4.5
  (process_block.h surface, g_live_check), §4.3-tail (self_heal.c stays, by design).
- **2026-06-02 · Wave 3 (commit `9ee2b3ad4`) — lint breadth + loudness surfacing + tests, all green
  (build rc=0, lint 33 gates, test_parallel 0/290).** §2.3 (ScriptErrorString mapper + serror in
  event / new column / dump_state — bad-sig now distinct from bad-pubkey), §1.5 (jobs+conditions
  silent-error gates), §1.7 (alloc gates scan `*.h`), §1.8 (silent-error gate accepts only
  error-level logs), §7.1 (obs-pairing gate scans working tree), §9.1 (reproducible release profile),
  §9.6 (CI workflow), §4.6/§4.7/§4.8 (STAGE-is-default comments + island record + single-engine note),
  §9.3 (tool-count measured 98; non-drifting `~100`), §8.2/§8.4/§6.3/§8.5 (3 new tests: stage_repair
  poison-rewind guards, oracle_policy threshold/flip, stage_anchor atomic/forward-only). **Integration:**
  the broadened gates surfaced 8 pre-existing silent-error sites → fixed faithfully (LOG_WARN→LOG_ERR on
  4 infra-failure DB sites per §2.7; `// raw-return-ok` markers on 3 condition not-ready guards +
  disk_monitor + the activerecord marker format). 3 tests registered in all 3 harness sites (287→290).
  **32/51 checklist lines done.** Deferred residue: §4.8 central label-scrub (B3/B7/B8 comments in 10
  non-owned files; exact hit-list captured in the Wave 3 agent build_note) — cosmetic, do later.
- **2026-06-02 · Wave 4 (commit `0a6604424`) — single-responsibility splits + prove-first subtraction,
  all green (build rc=0, lint 33 gates, test_parallel 0/290).** §5.2 (split chain_activation_controller.c
  800→514 + new reducer_ingest_service.c 315; one cross-TU static shared via a private header, every
  public symbol unchanged), §5.4 (split stage_repair.c 754→100 + 3 focused TUs; the destructive
  poison-rewind guards preserved byte-for-byte), §8.3 (parity_diff false-green now runs the real parity
  gate), §4.4 (narrowed process_block.h — removed the 1 proven-orphan export process_block_get_node_db;
  full classification table in the agent build_note), §4.5 (deleted the dormant delta_diverged/live-checker
  cutover scaffold from utxo_apply_stage.c — no prod setter; consensus apply path byte-identical).
  **Integration:** §4.5's deletion required dropping 4 references in test_utxo_apply_stage.c; the new
  Service file tripped the E2 one-result-type gate → added the `one-result-type-ok:reducer-drive-counts`
  marker (reducer entry points return advance-counts/bools; failures surface via the FATAL latch +
  EV_OPERATOR_NEEDED). **37/51 checklist lines done.**
- **2026-06-02 · Wave 5 (commit `9b04b4438`) — supervisor liveness + dead-author purge, all green
  (build rc=0, lint 33 gates, test_parallel 0/290).** §1.4/§10.1 (registered disk_monitor /
  mempool_limits / db_maintenance in the supervisor `op` domain with liveness contracts + atomic
  progress markers, wired inside each service _start so boot.c is untouched; broadened the
  registration gate to all app/services/src/*.c with a principled long-running detector that exempts
  the short-burst fork-join bg_validation_scripts.c by rule), §7.2 (both ratcheting baselines
  confirmed at zero), §4.1 (deleted the genuinely-dead update_coins emit-helper dual-write branch +
  counters + 4 no-op call sites; consensus apply path byte-identical). **Prove-first caught a wrong
  audit premise:** the UTXO_AUTHOR enum + get_author() are LIVE (utxo_apply_stage/_delta read
  get_author()==STAGE to pick DRIVER vs FOLLOWER reorg-unwind), so the enum/getter were RETAINED —
  only the truly-dead emit branch went. Dropped the now-dead single-writer gate from
  test_utxo_apply_authorship.c. **41/51 checklist lines done.** Whether the DRIVER/FOLLOWER duality
  is itself residue (author is always STAGE) is a deeper consensus-path question logged for the §3
  live-cluster pass, not done here.
- **2026-06-02 · Wave 6 (commit `4fe2964e3`) — principled raw-sql hatch + legacy_import Service
  extraction, all green (build rc=0, lint 33 gates, test_parallel 0/290).** §5.1 (investigated:
  AR is for node.db domain models, the reducer Jobs write to the progress.kv KERNEL store below the
  AR layer → routing them through AR is a category error; resolved by making the hatch PRINCIPLED:
  documented the kernel-store exception in DEFENSIVE_CODING.md, normalized all 48 markers to one
  canonical `raw-sql-ok:progress-kv-kernel-store` tag, gate reports the bounded count — comment-only,
  no logic change), §5.5 (extracted the 619-line cold-import orchestration into a new Service behind
  legacy_import_service_run(); legacy_import.c is now a 31-line shim keeping the public symbol, so all
  callers are untouched; integration used core/amount.h COIN to avoid a Service→View dep + an E2
  marker for the preserved int contract), §9.4 (CLAUDE.md already self-contained — verified, ticked).
  **44/51 checklist lines done.**
- **DEFER decisions (documented, doctrine "less is more"):** the two remaining non-§3 lines are
  consciously deferred, not forgotten:
  • **§5.3 pre-emptive splits** — the file-size E1 baseline is already ZERO (no file is over the
    ceiling). Splitting under-ceiling files purely for future headroom is churn against "subtract /
    less is more"; the genuinely mixed-concern files (chain_activation_controller, stage_repair) were
    already split in Wave 4. Re-open only when a specific file actually needs to grow past 800.
  • **§5.6 rename *_controller/*_repository → *_service** — pure cosmetics; the framework-shape lint
    gate already PASSES with the current names. Large include-churn blast radius;
    chain_evidence_controller.c is a live §3.4 target and chain_state_repository.c raises a deeper
    Model/Storage-Adapter boundary question — better revisited with the §3 cluster than churned now.
  **Remaining real work = the 5 §3 live-tip items** (boot finalized-cursor seed + contiguity window),
  the solo carve-out: implement → validate on a datadir COPY (tip_finalize health) → owner-gated deploy.
- **2026-06-03 · §3 analysis + §3.1 building block (commit `43a4f031f`).** Ran a 3-agent read-only
  analysis of the §3 cluster (verified plans; §3.1 confidence high, §3.2/3.3/3.5 medium with a real
  perf hazard — a 3M-entry map scan per drain — and §3.5 "delete the generic" is WRONG: header_admit/
  validate_headers still need the best_header extender, so the end-state is two extenders by ROLE).
  Implemented §3.1 as a forward-only, contiguity-guarded, bounded (≤50000-gap) seed function
  `block_index_loader_seed_tip_from_finalized` + validated it on a **--light repro-on-copy** of the live
  stuck datadir. RESULT: **no collapse** (tip never dropped — the regression detector stayed green), but
  the seed correctly **no-opped**, revealing the placement assumption was wrong: at the boot recovery
  section the in-memory active tip is still **genesis (h=0)** — the coins/UTXO authority sets the
  persisted CSR tip but not the in-memory active_chain there (`csr: tip committed from=3132741 to=0
  reason=genesis_init`). So §3.1's wiring is **BLOCKED** on identifying the boot point where the
  in-memory active tip is established to the coins frontier; the function is committed as a tested-safe
  building block (UNWIRED) and the unverified wiring reverted. NEXT (§3.1 wiring follow-up): trace the
  post-recovery activation/restore that establishes the in-memory active tip to 3132687, place the seed
  call immediately after it, re-run repro-on-copy expecting tip 3132687→3132741 with reorg_detected
  flat. §3 stays the owner-gated carve-out.
- **2026-06-03 · §3.1 WIRED + DONE + pivotal finding (commit `8b0228643`).** A 3-agent investigation
  pinned the mechanism (high confidence): `active_chain_height()` (== getblockcount) is a 3-tier
  resolver; at runtime tip_finalize registers itself as the height authority (`is_authoritative()`
  returns true unconditionally) so getblockcount == `g_last_advance_height`. The authority is
  registered by `tip_finalize_stage_init` via `staged_sync_supervisor_register` (boot_services.c:3684),
  which runs INSIDE `app_init_services` (boot.c:3577) — AFTER the recovery section, which is why the
  earlier 2790 wiring read genesis. Re-wired the seed at boot_services.c right after
  staged_sync_supervisor_register (the race-free window: authority live, runtime services + first
  supervisor tick not yet started) and re-validated on a --light repro-on-copy. **§3.1 is now wired +
  validated (no collapse, correct forward-only no-op) → DONE.**
  **PIVOTAL FINDING — the live wedge is NOT §3.1:** a fresh boot of the live datadir reconciles the
  active tip to coins_best+1 on its own (reducer-reconcile clamps the cursor to coins_best+1=3132742,
  coins_best=3132741), so the tip reaches **3132741** at boot and the seed sees cur_h already at the
  frontier → no-op. The real blocker surfaced instead: post-restore integrity reports
  **tip_window_holes=10001 (first_hole_h=3122741) → DEGRADED_SERVING**. So the live runtime 3132687
  (vs the 3132741 a fresh boot restores) is a **runtime tip-rewind**: boot reaches 3132741, then
  tip_finalize's reorg detector false-fires on the 10001 window holes and rewinds — the **§3.2/§3.3
  window-contiguity / reorg-churn** root cause, NOT §3.1. Also surfaced a **boot-perf bug**:
  `svc.rpc_mmb_register` took **328s** (why RPC missed the 360s repro window).
  **REDIRECT for §3.2/§3.3:** the lever is the contiguous have-data window (so H+1 is never a hole)
  + making `rewind_cursor_if_active_chain_reorged` not false-fire on window holes — validated by a
  repro-on-copy watching `reorg_detected` stay flat while the tip holds at 3132741. Separately file the
  mmb-register 328s boot-perf regression. §3.1's seed stays as a correct guard for the genuine
  dropped-frontier (coins < frontier) case.
- **2026-06-03 · Architecture axis: Wave A collapse + Rank 2 prove-first (commits `41174e498`,
  `b08df1219`).** Opened the deeper "purpose-per-file" board in REFACTOR_STATUS.md (Ranks 1-6) with a
  full-tree audit: 4 layers, domain/ depends on nothing (0 includes of app/lib), app/ depends inward
  18×, all 11 lint baselines at 0. **Wave A (done):** base58 + bech32 `lib/encoding` wrappers were pure
  forwarders → migrated all callers to `domain_encoding_*`, deleted the 4 wrapper files, dropped the
  now-moot wrapper-parity sub-tests; build + test_parallel 0/290 + lint green. `upgrades.c` investigated
  and **correctly KEPT** (not a duplicate — lib owns the consensus data tables, domain owns the
  arithmetic). **Rank 2 / ports (prove-first):** `check_raw_sqlite.sh` is GATE-CLEAN with an empty
  allowlist; the 49 raw-sqlite app/ sites are correct-by-design (Models=AR storage internals,
  Jobs=progress-kv kernel store, Views=read-only introspection) → NOT a migration; closed.
- **2026-06-03 · §5.3 / §5.6 reaffirmed keep-by-design (strengthened).** Re-analyzed with the new
  evidence: §5.6 rename is high-churn cosmetic over a passing framework-shape gate — `chain_evidence_
  controller` carries **197 symbol sites across 14 files** + diagnostics-key coupling; `chain_activation_
  controller` has `test_make_lint_gates` assertion coupling; `chain_state_repository` is a deliberate,
  consensus-critical **Repository pattern** (the single-writer guarding the 1.3M-UTXO loss). The prior
  "Model/Storage-Adapter boundary question" on `chain_state_repository` is **resolved** by the Rank-2
  prove-first (its raw sqlite is correct-by-design AR access, no violation). §5.3 stays churn (no file is
  over the 800 ceiling). Both remain deferred-by-design; the real structural win is Wave D (boot
  decomposition), tracked in REFACTOR_STATUS.

## 1. Loud failures & silent-halt elimination

- [x] **P0** (S) Stage runner swallows JOB_FATAL with no LOG_ — `change` — files: `lib/util/src/stage.c` — terminal FATAL only bumps an atomic count and returns; a wedged stage never reaches node.log, only `zcl_state` JSON.
- [x] **P0** (M) Drain/driver cannot distinguish JOB_FATAL from JOB_IDLE — `change` — files: `app/jobs/include/jobs/job.h`, `app/services/src/chain_activation_controller.c` — STAGE_DRAIN_IMPL breaks on any non-ADVANCED int count and `reducer_drain_to_convergence` breaks on adv==0, so a FATAL-looping reducer looks healthy-idle; surface FATAL distinctly or emit EV_OPERATOR_NEEDED.
- [x] **P1** (S) snapshot_fetch.c "LOG then fall through" → NULL-deref / double-unlock — `change` — files: `app/services/src/snapshot_fetch.c` — four guards log loudly then continue: null chunk_data memcpy crash, double-unlock, deref of zero-init store; add `return <err>;` after each log.
- [x] **P1** (M) Supervisor-registration gate globs `*_service.c` only — 3 live unsupervised loops evade it — `change` — files: `tools/scripts/check_supervisor_registration.sh`, `app/services/src/disk_monitor.c`, `app/services/src/db_maintenance.c`, `app/services/src/mempool_limits.c`, `config/src/boot.c` — broaden glob to `*.c` under app/services/src and add a liveness_contract + `supervisor_register_in_domain` to each background thread (started at boot.c:1003/1090, boot_services.c:259) so a hung loop is visible to the liveness tree.
- [x] **P1** (M) Add silent-error lint gates for app/jobs/src and app/conditions/src (newest shapes) — `add` — files: `Makefile`, `app/jobs/src/script_validate_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/conditions/src/stale_validate_headers_repair.c` — mirror check-silent-errors-services/-controllers for jobs+conditions, wire into `lint:`, bump doc-accuracy gate count; the authoritative reducer lives in app/jobs and is currently unguarded.
- [x] **P2** (S) scan_util.h: two unchecked `realloc()` leak + NULL-deref on OOM — `change` — files: `app/controllers/include/controllers/scan_util.h`, `app/controllers/src/wallet_scan.c`, `app/controllers/src/legacy_import.c` — `scan_uset_add`/`scan_wl_add` overwrite the pointer with NULL then deref; route through `zcl_realloc` and fail loudly (used by production wallet scan / legacy import).
- [x] **P2** (S) Alloc lint gates (check-malloc, check-raw-malloc) scan only `*.c` — header inline allocs evade them — `change` — files: `Makefile`, `tools/scripts/check_raw_malloc.sh`, `app/controllers/include/controllers/scan_util.h` — add `--include='*.h'` (the sqlite gate already does), which is precisely how the scan_util.h realloc bug stayed green.
- [x] **P3** (S) Silent-error gate accepts any prev-line `printf`, not only error-logging — `change` — files: `Makefile` — tighten allowed-prefix regex to LOG_ERR/LOG_FAIL/LOG_RETURN/log_json(level=error); drop bare `printf` so proximity to any stdout write no longer satisfies the gate.

## 2. Consensus validation loudness/correctness

- [x] **P0** (L) ROOT CAUSE: script_validate prevout resolver is never wired → universal `internal_error` — `investigate` — files: `app/jobs/src/script_validate_stage.c`, `app/supervisors/src/staged_sync_supervisor.c`, `config/src/boot.c` — `script_validate_stage_set_prevout_resolver()` has zero callers, so `default_prevout` returns false whenever `!fTxIndex` (default), failing every non-coinbase input above the first spend-bearing block; design a projection/spent-coin-index-backed resolver (ordering vs utxo_apply needs decision). This is the live finalization blocker.
- [x] **P0** (M) script_validate `internal_error` is reasonless — conflates null-block, prevout-unresolved, real script-invalid (DEFECT) — `change` — files: `app/jobs/src/script_validate_stage.c` — split into typed reasons (`block_decode_failed` vs `prevout_unresolved tx=<txid> vin=<n>` → BLOCKED/retry), put cause+txid+vin in EV_BLOCK_REJECTED and persist+expose them. (Merges 3 overlapping findings on this file.)
- [x] **P1** (M) verify_script ScriptError captured but discarded; no `ScriptErrorString` mapper exists — `change` — files: `app/jobs/src/script_validate_stage.c`, `lib/script/include/script/script_error.h` — add a ScriptError→string mapper in lib/script and carry code+string+txid into the script_invalid event so a bad-sig block is distinguishable from bad-pubkey/non-canonical-DER.
- [x] **P1** (M) Per-height failure reason columns (`first_failure_*`) are write-only — no dump_state/controller/MCP reader — `add` — files: `app/jobs/src/script_validate_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/jobs/src/utxo_apply_stage.c` — each `*_dump_state_json` should SELECT the lowest ok=0 height and emit {blocking_height, status, reason, txid, vin/proof_type/kind} so `zcl_state` answers "why is the pipeline stuck".
- [x] **P1** (S) tip_finalize `precondition_failed` does not say WHICH precondition (masks the script stall) — `change` — files: `app/jobs/src/tip_finalize_stage.c` — `preconditions_ok()` collapses 4 checks + a chainwork test into one bool; emit a reason token (`have_data_missing|not_script_valid|not_header_valid|chainwork_not_greater`).
- [x] **P2** (S) proof_validate failure carries proof-type but not txid/reason to any operator surface — `change` — files: `app/jobs/src/proof_validate_stage.c` — `first_failure_txid` is write-only and sapling.c LOG_FAIL reasons are unleveled/uncorrelated; include txid in the event and surface status+proof_type+txid in dump_state_json.
- [x] **P2** (S) utxo_apply lookup-callback failure recorded as generic `internal_error`/`lookup` with no log — `change` — files: `app/jobs/src/utxo_apply_delta.c`, `app/jobs/src/utxo_apply_stage.c` — LOG_WARN the lookup-provider failure with height+txid+vout and carry a distinguishing kind so the loudest (infra) error isn't the quietest.

## 3. Active-chain window & CSR-consistency architecture

- [x] **P0** (L) Boot does not seed active tip from the durable finalized cursor → finalized work dropped on reboot — `change` — files: `config/src/boot.c`, `app/services/src/block_index_loader_rebuild.c`, `main.c` — make `rebuild_seed_tip` (read MAX finalized height from tip_finalize_log, `chain_set_active_tip`) UNCONDITIONAL after any loader, not gated behind `-rebuildfromlog`; establishes csr_consistent=true by construction. Prerequisite for the items below.
- [ ] **P1** (M) Replace generic best-header window extender with the contiguous have-data/contiguity frontier — `change` — files: `app/jobs/include/jobs/stage_helpers.h`, `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h` — route all 8 `reducer_extend_window_to_candidate` stage call sites through `active_chain_extend_window_have_data`, passing utxo_apply's cursor as max_height, so H+1 can never be a fork/header-only successor.
- [ ] **P1** (M) tip_finalize one-block lookahead reads a mutable H+1 slot a generic extender re-fills — `change` — files: `app/jobs/src/tip_finalize_stage.c`, `app/jobs/include/jobs/stage_helpers.h` — change the lookahead window source to the contiguous finalized frontier so `finalized_row_active_match` can't false-fire and `rewind_cursor_if_active_chain_reorged` becomes assert-only (kills the live reorg_detected churn). Depends on the contiguity extender above.
- [ ] **P2** (M) Tighten `chain_evidence_controller_reconcile_startup` from "defer to next commit" to fail-loud — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_restore_repair.c` — after the boot tip is seeded from the finalized cursor (#1), a residual coins/active divergence is genuine corruption → LOG_ERR/EV_OPERATOR_NEEDED, not silent INFO defer.
- [ ] **P2** (S/L) Resolve `active_chain_extend_window_have_data` to exactly ONE window authority (wire it, delete the generic) — `investigate` — files: `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h`, `lib/test/src/test_active_chain_extend.c` — confirm bounded scan (`MAX_GAP=8192`) cost on a 3M-entry map under supervisor tick and that anchored extend still exposes legitimate higher-work reorgs; then delete `active_chain_extend_window` + the best_header path.

## 4. Legacy engine deletion (single-engine purge)

- [x] **P1** (M) Delete dead dual-write UTXO authorship branch (`utxo_projection_set_author` has zero prod callers) — `remove` — files: `lib/storage/src/utxo_projection.c`, `lib/storage/include/storage/utxo_projection.h`, `lib/validation/src/update_coins.c`, `lib/validation/include/validation/update_coins.h` — author is permanently STAGE; the `_emit_utxo_add/_spend_projection` helpers early-return always; delete helpers + 4 call sites (connect_block.c:752/799, update_coins.c:145/187), collapse the enum, remove `UTXO_AUTHOR_LEGACY`.
- [x] **P1** (M) Delete `coins_view_stage_backing.c` — entirely test-only dual-read/dual-write residue — `remove` — files: `lib/storage/src/coins_view_stage_backing.c`, `lib/storage/include/storage/coins_view_stage_backing.h` — no production caller; removing it also unblocks the `UTXO_AUTHOR_LEGACY` deletion; rewrite/delete the two coupled tests.
- [x] **P2** (M) Delete orphaned missing-UTXO self-heal recovery island (471 LOC, dispatcher removed in c0fc39749) — `remove` — files: `lib/validation/src/process_block_self_heal_chain_scan.c`, `lib/validation/src/process_block_self_heal_sqlite_tx_index.c`, `lib/validation/src/process_block_self_heal_legacy_rpc.c`, `lib/validation/src/process_block_self_heal_inject.c`, `lib/validation/src/process_block_self_heal.c` — the 3 recovery sources + injector + note/is-failure trackers have zero non-test callers; strip matching content asserts in `lib/test/src/test_make_lint_gates.c`. KEEP `process_block_self_heal_hot_loop.c` and `_scan_state.c` (live).
- [x] **P2** (M) Audit/narrow process_block.h legacy-shaped surface (`g_body_pull_active` etc.) — `investigate` — files: `lib/validation/include/validation/process_block.h`, `lib/validation/src/process_block.c`, `app/jobs/src/tip_finalize_post_step.c` — header half-states "engine deleted" yet still exports a 3229-LOC `process_block_*` surface; trace each export's live caller to confirm reducer-reached vs orphaned, then narrow.
- [x] **P2** (M) Decide fate of `g_live_check` projection-vs-coins.db parity hook (cutover scaffold or permanent guard) — `investigate` — files: `app/jobs/src/utxo_apply_stage.c` — find the production setter (if any) of `utxo_apply_stage_set_live_checker`; keep+rename off "live_check" if a permanent invariant, else delete the `delta_diverged` scaffold.
- [x] **P3** (S) Fix stale comments claiming `UTXO_AUTHOR_LEGACY` is "the default" — `change` — files: `app/jobs/src/utxo_apply_delta.c`, `lib/storage/include/storage/utxo_projection.h` — real default is STAGE; fix together with the dead-branch removal.
- [x] **P3** (S) Document `process_block_self_heal_legacy_rpc.c` keep-status (zclassicd-interop) OR fold into the island delete — `change` — files: `lib/validation/src/process_block_self_heal_legacy_rpc.c` — reconcile with the island-deletion item (this file is in that delete set); whichever wins, record it in LEGACY_LIFECYCLE.md so a future "delete legacy" grep is unambiguous.
- [x] **P3** (S) Restore a single-engine completion note OR scrub dangling B3/B8/DRIVER-FOLLOWER labels — `change` — files: `docs/work/single-engine-newcode-plan.md`, `docs/work/single-engine-newcode-design.json` — plan/design docs were deleted while in-code comments + `one_write_path_baseline.txt` header still cite their step/wave labels; record completion in REFACTOR_STATUS.md or scrub the labels.

## 5. Eight-shape conformance & file splits

- [x] **P1** (L) Migrate the Job reducer pipeline's raw Model-shape persistence off the non-ratcheting `raw-sql-ok` hatch — `investigate` — files: `app/jobs/src/stage_repair.c`, `app/jobs/src/utxo_apply_delta.c`, `app/jobs/src/validate_headers_report.c`, `app/jobs/src/tip_finalize_log_store.c`, `app/jobs/src/utxo_apply_stage.c`, `app/jobs/src/validate_headers_stage.c`, `app/jobs/src/header_admit_stage.c`, `app/jobs/src/body_persist_stage.c`, `app/jobs/src/body_fetch_stage.c`, `app/jobs/src/proof_validate_stage.c`, `app/jobs/src/script_validate_stage.c` — 38 inline `raw-sql-ok` comments hide raw sqlite3_step/exec from the gate; decide the durable stage-cursor store shape (Model / Storage Adapter / AR-wrapped stage_log) and route writes through the AR lifecycle so the hatch ratchets to zero.
- [x] **P2** (M) Split `chain_activation_controller.c` (mixed activation state-machine + reducer ingest, exactly 800 lines) — `change` — files: `app/services/src/chain_activation_controller.c` — extract the reducer-ingest half (`reducer_is_authoritative/_kick/_ingest_block`, lines 560-782) into `reducer_ingest_service.c`; also resolves the 800-line ceiling and the name/shape mismatch.
- [ ] **P2** (M) Pre-emptively split files at/near the E1 800-line ceiling (empty baseline, zero headroom) — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c`, `app/jobs/src/utxo_apply_delta.c`, `app/models/src/wallet_tx.c`, `app/models/src/database_migrate.c`, `app/views/src/explorer_pages_view.c`, `app/controllers/src/sync_controller_import.c`, `app/controllers/src/sync_controller_catchup.c`, `app/services/src/bg_validation_service.c`, `app/jobs/src/stage_repair.c` — any single added line trips CI with no inline override; split toward single-responsibility before an emergency baseline.
- [x] **P2** (M) Split new `stage_repair.c` (754 lines, 9 raw-sql-ok hatches, ≥3 repair concerns) before it lands permanent — `change` — files: `app/jobs/src/stage_repair.c` — separate header-solution backfill / body-fetch candidacy / poison rewind / tip-finalize clamp; route persistence through a Model.
- [x] **P2** (L) Extract orchestration out of large "Controllers" (import/sync) into Service/Job shape — `investigate` — files: `app/controllers/src/sync_controller_import.c`, `app/controllers/src/sync_controller_catchup.c`, `app/controllers/src/legacy_import.c` — REFACTOR_STATUS.md:182 flags this; `legacy_import.c` (604 LOC) is a multi-pass mmap scanner+Sapling-decrypt walker (Service/Job-grade) in a Controller.
- [ ] **P3** (S) Rename Service files carrying contradicting shape names (`*_controller`, `*_repository` → `*_service`) — `change` — files: `app/services/src/chain_activation_controller.c`, `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c` — names mislead the "folder shape == file shape" PR check; `chain_state_repository.c` also does direct sqlite3 (deeper Model/Storage-Adapter boundary question).

## 6. Dead code & unwired seams

- [x] **P3** (S) Fix stale process_block.h doc claiming a dead self-heal producer chain is live — `change` — files: `lib/validation/include/validation/process_block.h` — line 253 asserts `note_utxo_failure()->maybe_trigger_hot_loop_exit()` sets the pause, but neither is called in process_block.c; rewrite to the actual (hot_loop) producer.
- [x] **P3** (S) Correct stale note: generic `lib/util/projection.c` API is wired, not an "unwired WIP seam" — `change` — files: `lib/util/src/projection.c`, `lib/util/include/util/projection.h` — `projection_open/close/query_int64` have ~12 prod callers; only `query_text/double`/`is_open` are test-only; update the doctrine note so agents don't hunt a phantom seam.
- [x] **P3** (S) Audit `stage_anchor_upstream_cursors_to` test coverage (recent churn in 392f7256d) — `investigate` — files: `app/jobs/src/stage_anchor.c`, `app/jobs/src/tip_finalize_stage.c` — zero test files name it; verify test_tip_finalize_stage drives multi-cursor state, else add a focused atomic-advance/never-rewind test.

## 7. Lint baselines & defensive gates → zero

- [x] **P2** (S) observability-pairing gate scans committed HEAD-vs-merge-base only — working tree unscanned — `change` — files: `tools/check_observability_pairing.c` — add `git diff` (unstaged) + `--cached` to the scan set so in-flight modified app/lib .c files are checked before commit, not skipped.
- [x] **P2** (M) Drive the file-size E1 ceiling and supervisor-gate glob baselines to zero — `change` — files: `tools/scripts/check_file_size_ceiling.sh`, `tools/scripts/check_supervisor_registration.sh` — via the splits (§5) and registrations (§1/§10).

## 8. Test coverage

- [x] **P1** (S) Register two condition tests in test_parallel — DONE (commit adds X() for `body_fetch_missing_have_data_condition` + `stale_validate_headers_repair_condition`; test_parallel 0/285→0/287). **Expanded finding (own pass):** a full test.c-vs-test_parallel diff shows **15** `failures += test_X()` in test.c with NO X() in test_parallel (silently skipped by the runner we use): `chain_advance_atomicity, chain_tip, checkpoint, cold_start_sync, kill9_recovery, ldb_snapshot, onion_bootstrap, pprev_walk, sapling_lazy_init, sha3_windows, shielded_payment_gate, soak_harness, store_e2e_gate, thread_registry, utxo_snapshot_loader`. Some are legitimately serial-only (soak/integration/port-binding); needs per-test triage + a registry-sync gate WITH a documented exclusion allowlist (NOT a blind add — a stateful test in the fork-parallel runner can flake). The 30 "parallel-only" are mostly SPEC_LIST UI tests run via a different test.c path (not actually skipped).
- [x] **P1** (M) Add test_stage_repair.c covering the destructive poison-rewind safety guards — `add` — files: `app/jobs/src/stage_repair.c`, `lib/test/src/test_stale_validate_headers_repair_condition.c` — assert reject-when-ok=1-row-at/above-frontier (the ~47279 regression guard), non-frontier reject, POISON_NONE no-op, and header save/load hash-mismatch + oversized-solution rejection (currently all untaken).
- [x] **P2** (S) `ZCL_TEST_ONLY=parity_diff` subset is a no-op returning a false green — `change` — files: `lib/test/src/test.c` — wire it to a real parity gate (test_reorg_parity / _projection_parity / _projection_replay_invariant) or delete the dead branch.
- [x] **P2** (M) Add test_oracle_policy.c (gates live chain extension, zero test references) — `add` — files: `app/services/src/oracle_policy.c`, `app/services/include/services/oracle_policy.h` — use `oracle_policy_reset_for_test`: disagreement crosses threshold → state transitions → `chain_extension_allowed()` flips → clear() restores; assert dump_state_json fields.
- [x] **P3** (S) Add stage_anchor focused test — `investigate` — files: `app/jobs/src/stage_anchor.c`, `lib/test/src/test_tip_finalize_stage.c` — assert cursors advance atomically to target and never rewind below current.

## 9. Docs / build / release

- [x] **P1** (M) Add a reproducible release build profile (the "reproducible signed releases" claim is currently unachievable) — `change` — files: `tools/release.sh`, `Makefile` — drop `-march=native` (pin e.g. `-march=x86-64-v3`), add `-Wl,--build-id=none`, set `SOURCE_DATE_EPOCH`, and use deterministic `tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner` so the `.sha3` is a stable attestation.
- [x] **P2** (S) Make unsigned release a hard failure (not a silent GPG skip) — `change` — files: `tools/release.sh` — lines 172-179 skip signing when no key is present yet still produce a "complete" release; require a key or explicit `--unsigned`.
- [x] **P2** (S) Fix CLAUDE.md MCP tool-count self-contradiction (100+ vs 98; real=98) — `change` — files: `CLAUDE.md` — pick 98 to match routing tables; add a build/test assertion so the count can't drift.
- [x] **P2** (S) Fix CLAUDE.md "statically-linked" claim — binary is dynamically linked — `change` — files: `CLAUDE.md` — `file`/`ldd` show libstdc++/libm/libc/libgcc_s + dynamic -lsqlite3/-lssl; change to "self-contained ~15 MB C23 binary".
- [x] **P3** (S) BUILDINFO comment claims it records "flags" but it does not — `change` — files: `tools/release.sh` — record `$(CFLAGS)/$(LDFLAGS)` into BUILDINFO or fix the comment.
- [x] **P3** (M) Add a CI workflow running `make ci` + a reproducible-build SHA3 cross-runner diff — `add` — files: `Makefile`, new `.github/workflows/` — none exists, so lint/tests/reproducible-build run only locally.

## 10. Other (critic gaps)

- [x] **P2** (M) Register disk_monitor / db_maintenance / mempool_limits in the supervisor liveness tree — `investigate` — files: `config/src/boot.c`, `app/services/src/disk_monitor.c`, `app/services/src/db_maintenance.c`, `app/services/src/mempool_limits.c` — these spawn infinite loops via `thread_registry_spawn_ex` (boot.c:1003/1090) with proper stop specs but zero `supervisor_register`, so a hung loop is invisible to `zcl_state subsystem=supervisor`; add a liveness_contract + progress marker each. (Lands with §1's gate-glob fix.)

## Counts

Total items: 41 — By severity: **P0×7, P1×15, P2×14, P3×5** — By action: remove×3, change×27, add×6, investigate×5 (action = dominant verb per merged item).

## Critical path (do in this order)

1. **§2 #1 (P0)** wire the script_validate prevout resolver — the actual root cause of the live finalization stall AND a "finished" defect (a seam exists, was never connected).
2. **§3 #1 (P0)** seed the boot active tip from the finalized cursor — makes csr_consistent=true by construction; unblocks §3.
3. **§1 #1–2 + §2 #2,#5 (P0)** make every reducer failure loud and specific — so the next blocker names itself.
4. Then §3 #2–3 (contiguity window), §4 deletions (subtract), §5 splits, §7 baselines→0, §8 tests, §9 release.
