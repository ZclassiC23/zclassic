# Architecture consolidation — reduce duplication, single authorities

Owner directive (2026-06-07): "make our architecture beautiful, consolidate and reduce duplication,
use workflows." Audit via workflow w87cmrha4 (33 findings, ranked). Execution via workflow wswqpjww7
(worktree-isolated parallel edits + adversarial verify). THE driver for the consolidation axis.

Beauty principle (the audit's verdict): **removing an authority class is strictly more beautiful than
sharing a helper.** Favor deleting a parallel store / subsumed subsystem / split authority over adding
a shared function.

## The biggest beauty win (GATED — durability/self-heal track, tasks #11-13)
Collapse the **3 parallel UTXO coin stores** (coins_kv / coins_view_sqlite / utxo_projection) into ONE
`coins` table in progress.kv, written inside stage_run_once's atomic txn, with **tip_finalize's
contiguous frontier as the sole truth**. DELETES two whole data stores + the cross-store catch_up fold
+ the boot-time guess-which-store-is-authoritative machinery — root of every tip-wedge for weeks (live
4-way split: coins_best_block=3134313 / cec int=3134559 / utxo_apply cursor=3134559 / utxos MAX=3132687).
Fall-out: delete cec.coins_best_block_height int + Guard A (rank 2); make BLOCK_VALID_SCRIPTS/HAVE_DATA
log-derived (rank 5). Sequence = additive → flip-reads → delete; kill-9 ×10 copy-proof on a COPY;
SHA3-commitment EXACT match vs oracle gettxoutsetinfo. (docs/work/tip-durability-collapse.md GO.)

## SAFE wave (non-consensus refactors; workflow wswqpjww7 executing the disjoint first batch)
First disjoint batch (parallel worktrees): STEP 1, 2, 4.
- **STEP 1 [rank 3] log-store extraction** — extract ensure_log_schema + log_insert across 6 stages into
  {stage}_log_store.{c,h} mirroring tip_finalize_log_store. DELETE 6+6 copy-pasted static bodies. Drops
  script_validate_stage.c 812→<800 (closes task #10, removes the baseline line). HAZARD: never drop/reorder
  a per-stage column (= torn log = wedge); each stage keeps its OWN schema.
- **STEP 2 [rank 4] SHA3 serializer single-source** — 3 hand-rolled txid||vout||value||... encoders
  (coins_kv.c:215 confesses "BYTE-IDENTICAL") → one utxo_sha3_serialize_record(). HAZARD: must be
  byte-identical to all 3 (KAT is the gate); leave the 48-byte no-script hash_utxo alone if it truly differs.
- **STEP 4 [rank 9] backfill merge** — utxo_recovery_backfill + block_index_backfill (both fill
  blocks.sapling_value/sprout_value). CONFIRM-then-merge; REFUSE if not genuine duplicates.
Then sequenced (same files, after predecessors): STEP 3 [rank 8] dump_state_json + counter-getters;
STEP 5 [rank 6] stage init/shutdown registry; STEP 6 [rank 11] service_state_advance+persist wrapper.
Each: build + lint(35) + test_parallel 0/N, commit per step.

## GATED / consensus_adjacent (copy-proof or owner-gated; NOT in the safe wave)
- 3-store collapse + cec-int delete + nStatus→log-derived (the durability/self-heal track above).
- step_xxx job-step skeleton (rank 7) — biggest line win but encodes the reducer's exact guard ordering;
  do LAST, after log-store+registry de-risk, with per-stage parity tests (over-abstraction = wedge factory).
- BLOCK_FAILED_MASK → block_failure_log; chain_integrity_decide() unifying boot+condition; unify the two
  boot reconcile paths under CEC — all defer until the store collapse settles the frontier authority.

## REJECTED as noise (honor the harvested-safe-axis lesson — do NOT execute)
coins_best_block HASH as the single source (design REFUTES it — dead-legacy, zero live forward callers);
created_outputs_index "mirror" (self-rejected, intentional semantic diff); ADD-a-consistency-checker
RPC/job (adds observability to a split the collapse DELETES — anti-beauty); trivial write_le64 loop /
coins_kv↔utxo_projection bind helpers / utxo_projection seed-loop (all evaporate when the stores collapse
— DRYing doomed code is churn).
