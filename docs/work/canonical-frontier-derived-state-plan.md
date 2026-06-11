# Canonical frontier-derived state — two gates, delete the heal ladder

**Date:** 2026-06-11 · **Status:** in implementation · **Net LOC:** ≈ −2,800 to −3,100

The owner mandate: *"the node must always strongly stay synced to tip; multiple
layers of redundancy; do it the strongest way; simulator + real data, no guessing;
**don't add more repair code — make it canonical and DRY**; commit + push."*

This plan is the output of the `canonical-sync-architecture` workflow (6 mappers →
diagnose → 3 designs → judge panel → synthesis). It is **net-subtractive**: two small
gates added, nine repair modules deleted.

## The one defect, two faces

Both live wedge classes are the **same** architectural defect: **state INSTALLED
instead of DERIVED from the log**, surfacing at two chokepoints.

### Wedge A — restore installs a tip above the header frontier (boot crash-loop)
`utxo_recovery_restore_chain_tip` commits a tip via the single chokepoint
`csr_validate_locked` (`chain_state_service.c:236-363`), which checks the tip against
block_map / SQLite / UTXO-count but **never against the durable header frontier**
(`validate_headers_log` contiguous ok=1 prefix). It then **slams**
`pindex_best_header` forward to the installed tip (`csr_commit_tip:723-729`). So a tip
lands at 3143175 while headers were validated only to 3141533 → a 1267-block hole
window that `chain_integrity_check_post_restore` then FATALs on. *A checker diagnosing
corruption the install step manufactured.* (Live: NRestarts=11, systemd FAILED.)

### Wedge B — false coin-tear dead-ending at the unimplemented L2
`utxo_apply` (stage 6) gates on `proof_validate`; `tip_finalize` (stage 7) gates on
`utxo_apply` **plus** an extra `have_data` precondition. When bodies are missing-for-
finalize but proofs passed (the post-reimport state), `utxo_apply` leads, `coins_applied`
reaches `hstar+2`, and `read_frontier_snapshot` (`stage_repair_reducer_frontier.c:277`)
flags `coins_applied > hstar+1` as `refused_coin_tear` — terminal, because L2
(`reducer_frontier_reconcile_deep`) was **never implemented**. The "tear" is legitimate
pipeline depth misread as corruption.

## Canonical invariants (enforced by construction at one chokepoint each)

- **INVARIANT A (frontier-bounded tip).** A tip is committable at height `h` only if
  `validate_headers_log` has a contiguous ok=1 prefix reaching `h`, OR the commit carries
  a typed `rollback_auth` (SNAPSHOT/RESTORE/VALIDATION/REINDEX/BOOT_REPAIR), OR it is the
  existing `forward_from_active_tip` carve-out. `pindex_best_header` is a **projection** of
  that prefix, never slammed forward. Chokepoint: `csr_validate_locked`.
- **INVARIANT B (coins bounded by finality).** `utxo_apply` may not advance
  `coins_applied_height` past `tip_finalize`'s finalized frontier by more than
  `PIPELINE_DEPTH_UTXO_OVER_FINALIZE = 1`. Chokepoint: `utxo_apply` `step_apply`
  precondition. The tear test compares `coins_applied` against `utxo_apply_log`'s **own**
  contiguous prefix + depth, not the `tip_finalize`-pinned global MIN H*.
- **Derived-state corollary.** One shared frontier reader
  (`reducer_frontier_header_frontier`, wrapping the existing static
  `log_contiguous_prefix`); `coins_best_block` derived from `coins_applied_height`; the
  node.db `utxos` table loses all read authority (SHA3 commitment + fast-sync serve
  re-pointed to `coins_kv` via the **same** encoder → byte-identical commitment).
- **Consensus parity (unchanged).** Neither gate touches Equihash params, activation
  heights, or block/tx validity. They constrain only the **sequencing** of when derived
  state may be installed. Accept/reject set identical to zclassicd.

## Ordered steps (each independently buildable + gated build+lint+test_parallel green)

1. **Export the frontier reader** (DRY primitive, no behavior change) —
   `reducer_frontier_header_frontier()` over `log_contiguous_prefix`; name
   `PIPELINE_DEPTH_UTXO_OVER_FINALIZE`.
2. **Invariant B** (no consensus surface, ships first to de-risk) — utxo_apply
   self-clamp to finality (floored at trusted anchor); depth-aware tear test.
3. **Invariant A** — frontier gate in `csr_validate_locked` (after the rollback-auth +
   forward-from-active-tip carve-outs) + replace the slam at 723-729 with derive-to-
   `min(new_tip, frontier)`. Pass `header_frontier_hint` via the commit struct; fail-open.
4. **Collapse restore tip-selection** to derive-from-frontier (clamp candidate before commit).
5. **Re-point SHA3 / fast-sync serve readers to coins_kv** (parity-gated, utxos still present).
6. **Derive `coins_best_block` from `coins_applied_height`** (kills the two-name drift).
   *Status: implemented on `refactor/derive-coins-best-demote-mirror` —
   `reducer_frontier_derive_coins_best` + `coins_kv_is_proven_authority`; every
   decision-path reader derives, every legacy repair rung is gated on !derived,
   caches labeled. Serve-side (step 5) still pending.*
7. **Delete the dead heal ladder** (grep-proven zero callers): chain_restore_integrity,
   chain_restore rebuild ladder, stage_repair_reducer_frontier_{tipfin,refill,purge} +
   tear branch, reducer_frontier_reconcile_light, utxo_recovery_torn_anchor (M2),
   coins_view_sqlite rewind/reconcile/case-b gate.
8. **Simulator tests + real-data copy-prove + parity gate.**

## Redundancy ladder (reuses existing primitives — no new heal code)

1. **reducer / every tick** — Invariant B backpressure: a body-starved tip_finalize holds
   the whole pipeline at one height; `body_fetch` refills; resumes. No tear, no L2.
2. **boot / process start** — Invariant A + frontier-clamped restore: always commits the
   highest validated tip; no post-restore FATAL to crash-loop on.
3. **service-restart / supervisor** — `chain_tip_watchdog` (300/600/1200s, bounded 3) —
   now fires only on genuine peer/network stalls.
4. **external / systemd + MCP** — after the bounded budget, emit `EV_OPERATOR_NEEDED`,
   leave node up degraded; `zcl_state subsystem=chain_tip_watchdog` surfaces it. No silent halt.

## Real-data proof (copies only, never live)

Fixture: `~/.zclassic-c23-postrestore-wedge-20260611` (frozen failing datadir: tip 3143175
ahead of header frontier 3141533, 1267 holes, 631 mismatches). zclassicd oracle at RPC
8232, tip 3,143,314. (1) Repro the crash-loop on a COPY with the current binary. (2) Patched
binary boots the COPY degraded-but-live at 3141533 (no FATAL) and the reducer advances
forward. (3) `coins_kv_serve_utxo_root` over `coins` == prior commitment over `utxos`
byte-for-byte, and == zclassicd `gettxoutsetinfo` at the same finalized height. Deploy is a
separate owner-gated action only after the copy reaches tip with byte-identical parity.

## Open questions to close during implementation

- Enumerate all `update_header_tip=true` tip-commit callers; assert each supplies a
  `header_frontier_hint` or a `rollback_auth` (no above-frontier tip slips the gate).
- Prove `block_index_loader` warm-boot tip == frontier on a clean datadir (no over-reject).
- `data_integrity_compute` / `utxo_projection` shadow-seed: confirm non-consensus or repoint.
- `reconcile_light`: shrink-vs-delete after the step-7 grep shows registration sites.
