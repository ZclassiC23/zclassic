# Canonical Service State Machine — design + verified root cause

Status: DESIGN + live-verified diagnosis (2026-06-01). Supersedes the verbose
agent output kept at the workflow transcript. Implementation-ready.

Mandate (owner): finish zclassic23 as a **supervised self-healing service** —
one binary that always boots into an **observable** state, reconciles
chain/storage/sync authority deterministically, turns every inconsistency into
a **named repair Condition**, starts the supervisor early enough to expose
progress, catches up to tip, and **never silently loops or dies unless the data
is truly unrecoverable**.

Doctrine honored: fix ROOT causes not band-aids; never weaken a safety/consensus
gate; subtract before adding; one source of truth per fact; the reducer/staged
Job pipeline is the authoritative chain-advance engine.

---

## 0. The live failure (the acceptance test)

The node was dead in a systemd crash-loop (restart counter → 10, systemd gave
up). Verified live (stopgap run with `-allow-degraded` + `zcl_state`/`zcl_sql`):

**Layered wedge — four stacked defects, one canonical truth missing:**

1. **HEAD: `BLOCK_HAVE_DATA` flag desync (in-memory vs disk).** Block 3130702's
   in-memory block_index entry has `data=0` (`STALL: entries_at_3130702=1
   (data=0 fail=0)`), yet its body **is persisted on disk** (`reducer
   persisted ingested block body h=3130702 file=50`) and node.db `blocks.status`
   = `0x0d` (HAVE_DATA|VALID_SCRIPTS). Activation sees no body → can't connect
   3130702 → gap-fills it forever → tip frozen at 3130701. Root: the reducer
   persist/cursor path advances without **durably** setting the canonical
   in-memory/`block_index.bin` `HAVE_DATA` flag (write-ordering invariant
   violated; cf. memory `at_tip_kill9_ordering_invariant`).
2. **Stage-cursor / durable-marker desync.** progress.kv cursors
   (body_persist/script_validate/utxo_apply/tip_finalize = **3132857**) and
   node_state durable tips (`cec.active_tip_height`,
   `sync_projection_tip_height` = **3132856**) sit AHEAD of the applied tip
   (`cec.coins_best_block_height` = **3130701**, == in-memory active tip; hashes
   match). So utxo_apply/tip_finalize **idle** (cursor says "done";
   `advanced_count=0, idle`) and never re-apply 3130702..3132856.
3. **Boot integrity FATAL.** `chain_restore_finalize()` →
   `chain_integrity_check_post_restore` reports `zero_nbits=0, mismatches=0,
   tip_window_holes=2155` (holes from 3130702). `config/src/boot.c:3390` does
   `return false` → "Initialization failed." → restart loop. This is **normal
   IBD-style lag, not corruption** — but it's handled as fatal **before the
   repair layer runs**.
4. **Repairs can't witness.** `block_failed_mask_at_tip` (active, 5 attempts,
   `result=height_not_found` — wrong remedy; nothing is failure-masked),
   `body_fetch_missing_have_data` (queues frontier 3132857 not 3130702 — wrong
   height), `legacy_mirror_stuck` — all `unwitnessed`, escalate to
   `operator_needed`. Auto-clear historically needs a peer quorum oracle and
   there are no healthy zcl23 peers (only zclassicd, wrong protocol). But a
   **trusted local oracle exists**: zclassicd @ 3132869 on RPC 8232.

Authoritative coherent floor = `coins_best_block_height` = **3130701**
(active_tip_hash == coins_best_hash; node serves there). `utxo_max_height=3130517`
is a lagging unspent-output counter, not the apply anchor.

---

## 1. Service State Machine

Two **orthogonal** dimensions, kept separate:

- `enum boot_stage` (`lib/util/include/util/boot_phase.h`) — ordered one-time
  init checkpoints, monotonic, abort-on-backward. Unchanged except one inserted
  boundary `BOOT_STAGE_CHAIN_RESTORE`.
- `enum service_state` (NEW, `lib/util/.../service_state.h`) — runtime
  operational mode, **bidirectional**: `BOOT → RESTORE → RECONCILE →
  DEGRADED_SERVING → SYNCING → HEALTHY → REPAIRING`. Illegal transitions are
  logged + dropped (never abort — operational mode must not crash the node).
  In-memory `_Atomic`; mirrored to progress.kv; exposed via
  `zcl_state subsystem=service_state` + `zcl_status`.

```
BOOT → RESTORE → RECONCILE ─┬─ ok+caught-up ──────────────► HEALTHY
                            ├─ ok+behind ───────► SYNCING ─► HEALTHY
                            └─ reconcilable ─► DEGRADED_SERVING ─(repair witnessed)─► SYNCING
                                                  ▲                                   │
                                                  └──────── REPAIRING ◄──── HEALTHY (post-boot condition)
RECONCILE ─ unrecoverable(mismatch/zero_nbits) ─► FATAL-LOUD + EV_OPERATOR_NEEDED (unless -allow-degraded)
```

Owners: boot steps drive BOOT/RESTORE/RECONCILE; the finalize gate picks
RECONCILE→{SYNCING,HEALTHY,DEGRADED_SERVING,fatal}; the condition engine +
sync_monitor own DEGRADED↔SYNCING↔HEALTHY↔REPAIRING after boot.

## 2. Authority model (single source of truth per fact)

| Fact | Authority | Invariant |
|---|---|---|
| applied/validated/active tip | in-memory active_chain tip == `cec.coins_best_block_height` | `tip_finalize ≤ utxo_apply ≤ … ≤ HAVE_DATA-contiguous ≤ header_admit ≤ header_tip` |
| header tip / sync target | `pindex_best_header` / header_admit cursor | may lead the active tip (normal) |
| durable tip markers | `cec.active_tip_height`, `sync_projection_tip_height` | MUST equal the applied tip; never lead it |

**Reconcile rule (the live class):** when durable markers/cursors lead the
applied tip with `HAVE_DATA` present for the gap → the divergence is
**coins-application lag**, not body absence. Re-apply, don't re-fetch.

## 3. The reconcile (corrected remedy — verified)

NOT body-fetch (bodies present). The repair, atomic under one
`progress_store_tx_lock()` + DB txn:

1. **Re-hydrate `HAVE_DATA`**: for every gap block whose body is readable on
   disk / projection marks HAVE_DATA but in-memory lacks it, set
   `nStatus |= BLOCK_HAVE_DATA` on the canonical index (fixes the HEAD).
2. **Rewind desynced cursors** body_persist/script_validate/utxo_apply/
   tip_finalize 3132857 → coherent floor (coins_best 3130701) so stages
   re-process 3130702.. .
3. **Roll durable markers down** `cec.active_tip_height/hash`,
   `sync_projection_tip_height/hash` → 3130701 (so serving never advertises
   3132856; future restart won't re-diverge).
4. **Oracle-witnessed failed-mask clear** (if any true `BLOCK_FAILED_VALID`):
   only clear when trusted oracle zclassicd agrees the same hash at that height
   is canonical. Never blind-clear.
5. Stages re-apply 3130702..header; coins_best climbs; active tip climbs;
   DEGRADED→SYNCING→HEALTHY.

**Witness = `cec.coins_best_block_height` strictly increases** past the rollback
point (proves real UTXO application), NOT mere hole-closure (which re-publishing
the tip could fake).

## 4. De-fatal plan (boot.c finalize gate — the keystone)

Replace `config/src/boot.c:3377-3406`. Read the structured
`chain_integrity_result` (cached via `chain_restore_get_boot_snapshot`, the
fields the bool discards), classify:

- `reconcilable = !finalize_ok && zero_nbits_count==0 &&
  active_chain_mismatches==0 && tip_window_holes>0` → `service_state_advance(
  DEGRADED_SERVING, …)`, LOUD log, `EV_OPERATOR_NEEDED` advisory, ensure the
  reconcile Condition is active, **continue** (never `return false`).
- `unrecoverable = zero_nbits_count>0 || active_chain_mismatches>0` → keep fatal
  (LOUD + `EV_OPERATOR_NEEDED` + observable) unless `-allow-degraded`.

`-allow-degraded` is demoted to a narrow override for the **fatal-LOUD** class
only, not the primary path.

**Consensus gates explicitly NOT touched:** `connect_block.c:161`
(coins_view↔prevhash), all `CSR_REJECTED_*`, `find_most_work_chain`
validity/HAVE_DATA filters, PoW/sig/proof verification. The integrity check is
NOT a consensus gate (it counts NULL slots in the in-memory active_chain[]
window) — de-fatalizing its *reaction* weakens nothing. (Adversarial critique
confirmed: weakens_safety_gate=false, consensus_risk=false.)

## 5. Early supervisor

Split `app_init_services` into `_early` (supervisor + condition engine +
`condition_engine_set_main_state` + alerts/EV_OPERATOR_NEEDED routing + register
conditions) called BEFORE the finalize gate, and `_full` (P2P/Tor/frontend/
runtime kernels — bind no sockets early) at the normal point. The gate may run
one synchronous `condition_engine_tick()` for an immediate first remedy; boot
always continues on the reconcilable class.

## 6. Increments

- **I1 (keystone):** `service_state` primitive + `BOOT_STAGE_CHAIN_RESTORE` +
  de-fatal finalize gate (classification) + diagnostics exposure + tests.
  Permanently kills the crash-loop; node boots observable into DEGRADED_SERVING;
  preserves fatal-LOUD for true corruption.
- **I2 (reconcile):** §3 repair (HAVE_DATA re-hydration + cursor/marker rewind +
  oracle-witnessed clear) as a named Condition with coins-advance witness; fix
  `body_fetch_missing_have_data` to target validated_tip+1; early supervisor.
- **I3 (prevention):** enforce the write-ordering invariant so cursors/markers/
  `block_index.bin` never advance ahead of the committed coins tip — makes the
  desync structurally impossible; the reconcile becomes the safety net.
- **I4:** de-fatal the remaining boot exits (wallet/coins_view/projection/
  kernel) → DEGRADED + named Conditions, per the full plan.

## 7. Live proof (the real gate)

On a COPY of the wedged datadir (preserve the torn datadir first): old binary
crash-loops; new binary → `service_state=DEGRADED_SERVING` then `SYNCING`,
process never exits, `zcl_getblockcount` returns the coins tip (never 3132856),
`coins_best` climbs 3130701→3132869, `zcl_utxocommitment` byte-matches zclassicd
at convergence (no consensus drift), kill-9 mid-climb re-converges. Only then
deploy to the live linger service.
