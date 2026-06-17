# ZClassic23 — Finish Checklist

> Generated 2026-06-02 by a 9-dimension multi-agent audit (41 items). Rubric =
> owner doctrine: errors fail loudly, architecture solves by construction, subtract
> before adding, baselines ratchet to zero, 800-line ceiling. Companion to
> `docs/REFACTOR_STATUS.md`.

**Completed items (45 of 51): see git history + `docs/REFACTOR_STATUS.md`** — the
durable record. Below are the 6 that remain.

## Status

Remaining real work = the §3 live-tip contiguity window + two defer-by-design items
(§5). Each: implement → validate on a datadir COPY → owner-gated deploy.

**§3 PIVOTAL FINDING — the live wedge is NOT §3.1.** §3.1 (boot seed-tip-from-finalized
cursor) is DONE; a fresh boot reconciles the active tip to coins_best+1 on its own, so
the seed correctly no-ops. The real blocker is a runtime tip-rewind: tip_finalize's
reorg detector false-fires on `tip_window_holes` → §3.2/§3.3 window-contiguity. (Since
re-root-caused further — see MEMORY / `FORWARD_PLAN.md`.)

## 3. Active-chain window & CSR-consistency architecture

- [ ] **P1** (M) Replace generic best-header window extender with the contiguous have-data/contiguity frontier — `change` — files: `app/jobs/include/jobs/stage_helpers.h`, `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h` — route all 8 `reducer_extend_window_to_candidate` stage call sites through `active_chain_extend_window_have_data`, passing utxo_apply's cursor as max_height, so H+1 can never be a fork/header-only successor.
- [ ] **P1** (M) tip_finalize one-block lookahead reads a mutable H+1 slot a generic extender re-fills — `change` — files: `app/jobs/src/tip_finalize_stage.c`, `app/jobs/include/jobs/stage_helpers.h` — change the lookahead window source to the contiguous finalized frontier so `finalized_row_active_match` can't false-fire and `rewind_cursor_if_active_chain_reorged` becomes assert-only (kills the live reorg_detected churn). Depends on the contiguity extender above.
- [ ] **P2** (M) Tighten `chain_evidence_controller_reconcile_startup` from "defer to next commit" to fail-loud — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_restore_repair.c` — after the boot tip is seeded from the finalized cursor, a residual coins/active divergence is genuine corruption → LOG_ERR/EV_OPERATOR_NEEDED, not silent INFO defer.
- [ ] **P2** (S/L) Resolve `active_chain_extend_window_have_data` to exactly ONE window authority (wire it, delete the generic) — `investigate` — files: `lib/validation/src/chainstate.c`, `lib/validation/include/validation/chainstate.h`, `lib/test/src/test_active_chain_extend.c` — confirm bounded scan (`MAX_GAP=8192`) cost on a 3M-entry map under supervisor tick and that anchored extend still exposes legitimate higher-work reorgs; then delete `active_chain_extend_window` + the best_header path.

## 5. Eight-shape conformance & file splits

- [ ] **P2** (M) Pre-emptively split files at/near the E1 800-line ceiling — `change` — files: `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c`, `app/jobs/src/utxo_apply_delta.c`, `app/models/src/wallet_tx.c`, `app/models/src/database_migrate.c`, `app/views/src/explorer_pages_view.c`, `app/controllers/src/sync_controller_import.c`, `app/controllers/src/sync_controller_catchup.c`, `app/services/src/bg_validation_service.c`, `app/jobs/src/stage_repair.c`. **Deferred by design:** the E1 baseline is already ZERO (no file is over the ceiling); splitting under-ceiling files purely for headroom is churn against "subtract / less is more" — re-open only when a specific file actually needs to grow past 800.
- [ ] **P3** (S) Rename Service files carrying contradicting shape names (`*_controller`, `*_repository` → `*_service`) — `change` — files: `app/services/src/chain_activation_controller.c`, `app/services/src/chain_evidence_controller.c`, `app/services/src/chain_state_repository.c`. **Deferred by design:** pure cosmetics over a passing framework-shape gate with a high-churn blast radius (`chain_evidence_controller` = 197 symbol sites across 14 files + diagnostics-key coupling); `chain_state_repository.c` is a deliberate consensus-critical Repository pattern (the single-writer guarding the 1.3M-UTXO loss), its raw sqlite resolved correct-by-design.

## Critical path (do in this order)

1. **§3 contiguity window** (the two P1 items above) — the active live blocker.
2. Then the §3 P2 fail-loud + window-authority cleanup.
3. The §5 items are deferred by design — re-open only on the stated triggers.
