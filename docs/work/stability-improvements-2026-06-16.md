# Stability improvements — 2026-06-16

> **UPDATE 2026-06-23:** the import-time torn-UTXO failure this doc treats as the recurring live cause (Deferred hardening preamble below) is RESOLVED on the live node — now unwedged at the network tip — by commit `ab512d577` (bind a snapshot above coins-best by extending the active-chain window); a complete SHA3-verified snapshot at h=3,156,809 seeds the full UTXO set and folds forward past the block that wedged the older torn seed. Still borrowed: that snapshot is consensus-bound to the in-binary PoW header but its UTXO-set content is not yet re-derived from genesis, so the from-genesis self-mint anchor cure remains the end goal. The orthogonal hardening items below remain valid backlog.

Source: post-recovery review (5-dimension workflow, every finding grep-verified at
HEAD `fdc1f3a30`). Prioritized, safety-classified hardening work.

**Safety classes:** `safe-now` = non-consensus, hermetically testable, no live
mutation. `copy-prove` = touches boot/recovery/coins; needs a datadir-copy proof
before deploy. `owner-gated` = consensus parity or a structural/topology decision.

## Applied this pass (safe-now)

- **O1 — deploy never ships a stale binary** (`Makefile` `deploy`): the
  `$(ZCLASSIC23_BIN)` rule is a single whole-program `cc` over `$(ALL_SRCS)` with
  **no depfile tracking**, so a header-only edit left every `.c` mtime unchanged
  and `make` skipped the relink. `deploy` now `rm`s the binary and forces a
  rebuild; `deploy_verify.sh` confirms the running `build_commit`.
- **S4 — DNS-seed resolution is loud** (`lib/net/src/connman.c`): a failed
  `getaddrinfo` used to `continue` silently; now `LOG_WARN`s with `gai_strerror`.
  (`lib/net/` is outside the `check-silent-errors` lint gate — a structural blind
  spot; the warn is the guard.)
- **C3 (log-only) — cold-import seed failure is unconditional** (`block_index_
  loader_rebuild.c`): the `applied_height` set-failure log was gated on `err`
  (silent when `coins_kv` returned false with no sqlite errmsg). Now always logs;
  logic unchanged, the SHA3/refuse behavior is C-queue work.

## SAFE-NOW queue (remaining — non-consensus, hermetic)

- **S1 — `header_band` in `zcl_state`**: expose island-root / contiguous-frontier
  / remaining / ETA so an operator can tell "frozen forever" from "backfilling N
  of M". `header_band_service.c` + `diagnostics_registry.c` + the two enum lists.
- **S2 — `connman` outbound-health in `zcl_state`** + surface `healthy_outbound`
  (not `num_nodes`) in health: "N/3 healthy, dialing, K addnodes in backoff".
- **S3 — split the overloaded `block-not-finalized-by-reducer` reason**: emit a
  benign `reducer-finalize-pending` for an in-flight block (stages advancing),
  reserve the hard reason for a genuine tear; add a derived `{advancing,
  idle-at-tip, stalled, torn}` health verdict.
- **S5/S6/S7 — untested recovery discriminators**: `peer_floor` witness branches;
  the zero-outbound forgive-all-backoff remedy; the `contradiction_frozen`
  condition loop through the engine.
- **S8 — IBD-class getheaders interval while a band hole exists** (10s, request
  side only; reverts when the band closes).
- **S9 — band-aware stall condition** keyed on the *contiguous frontier* (not
  `best_header`, which sits at the high island root and hides a pinned frontier).
- **S10 — split nTx disk-recovery counters** (hash-mismatch vs unreadable).
- **S11 — postmortem capsule on operator-needed escalation** (not just on a
  fatal signal).
- **S12 — seed-reachability CI canary** (the "28 tcp failures" root: the hardcoded
  seed/addnode list rotted to mostly-dead). Probe `vSeeds` + `vFixedSeeds` and fail
  under a floor; lint the deploy env IPs against the same list.

## COPY-PROVE queue (boot/recovery/coins — datadir-copy proof first)

- **C1 — local band-fill from the already-hardlinked zclassicd bodies** (HIGH):
  the ~12k-block band currently fills over P2P at ~160 headers/round-trip even
  though the bodies are on local disk (hardlinked at import). Scan local
  `blk*.dat`, `accept_block_header` to link pprev, close the band in-process.
  Turns a ~10-min peer-gated crawl into a seconds-long local scan.
- **C2 — first-boot freeze distinguishes band-hole from genuine contradiction**:
  enter a non-terminal `band_backfilling` state (self-heals on band close)
  instead of `contradiction_frozen`, so cold import never needs a manual restart.
  Sequence after C1.
- **C4 — SHA3 snapshot-refresh rc checked** (`utxo_recovery_restore.c`): the
  `BEGIN;END;` refresh that guards the false-wipe decision discards its rc.
- **C5 — replace `system("cp …")` block-file copy with a checked in-process copy**
  (`config/src/boot.c`): a failed copy currently proceeds with a missing/truncated
  `blk*.dat`; should refuse-to-bless + page.
- **C6 — band-backfill latency sub-budget in the replay canary**.
- **C7 — end-to-end cold-import recovery test/sim** (detect→freeze→backfill→close,
  asserts un-freeze with no operator restart); wire into `make ci`.
- **C8 — explorer `blocks` projection-hole backfill** (merged from
  explorer-data-integrity, 2026-06-18; the one remaining live item — the mirror
  feeder, per-block indexer, genesis/difficulty/stats fixes all shipped). 13,383
  missing heights (3137533–3150899, the pre-restore window): the validator trips at
  `explorer_internal.h:412` (`block_rows != max_height+1`). Catchup
  (`app/services/src/node_db_catchup_service.c:303-404`) is forward-only and never
  revisits below its cursor. **Caveat:** a cold-import node lacks bodies for that band
  — backfill needs a P2P re-fetch of the range, not just a re-index. node.db-only;
  never touches the consensus `coins_kv` commit. (Other explorer defects to track,
  not blocking: the tx-input "?" placeholder — resolve prevout value at render time
  from the prevout body / `coins_kv` instead of the empty `tx_outputs`; and the Phase-B
  per-block index tables `tx_outputs`/`tx_inputs`/`view_integrity` etc. now have a
  shipped writer via `explorer_index.c` + `-reindex-explorer`.)

## OWNER-GATED / strategic

- **O2 — make self-sufficient trustless sync (FlyClient + SHA3 snapshot + delta
  P2P over the same reducer stages) the flagship; demote cold-import to opt-in.**
  Both Layer-0 failure classes (coin tear, restart fragility) are symptoms of
  cold import being a privileged write path that bypasses the validated reducer.
  Tactical bridge (copy-prove-able): a write-time import-correctness gate that
  refuses imported state until SHA3-verified AND completeness-checked at every
  height.
- **O3 — chain-aware peer-floor "healthy" definition**: count a peer toward the
  anti-eclipse floor only if it recently served a header/block at/above our tip.
  Do NOT lower the floor.
- **O4 — INJECT event vocabulary for the seed-tape simulator** (PEER_DROP /
  HEADER_ADMIT / IMPORT_TIP) so cold-import/peer-floor failures become replayable
  64-bit seeds + chaos commands.

## Deferred hardening (merged from robustness-audit-2026-06-15)

Held back deliberately (boot-path risk / multi-site lock changes / P3), not rushed.
Architecture verdict from that audit: SOUND, no refactor — the recurring failure is
torn UTXO sets INSTALLED at import with no cryptographic proof; root cause + cure are
canonical in [`never-stuck-plan.md`](./never-stuck-plan.md) +
[`tenacity-roadmap.md`](./tenacity-roadmap.md). NOTE: `stage_reconcile_clamp_tip_finalize_to_floor`
(`app/jobs/src/stage_repair.c`) is NOT dead — live caller `config/src/boot.c:3260-3274`;
ladder-deletion sequencing must re-derive the caller graph with `config/` in scope.

- **BOO-3 (P2, startup speed):** eliminate the unconditional ~3.1M-entry `block_map`
  walk on warm boot in `block_index_loader_topup.c:258-330` — fold
  disk-recovery-candidate detection into the existing `topup_row_cb` iterate, skip the
  full walk when empty (mirror the nChainTx skip-guard). Copy-prove on a livetear
  fixture; confirm identical recovery-candidate set + faster warm boot.
- **BOO-7 (P3, boot correctness window):** reorder the cold-import `coins_applied`
  raise to AFTER the seed-anchor step succeeds
  (`block_index_loader_rebuild.c:683-722`) to drop a spurious C4 tear WARN on partial
  failure. Wedge-adjacent boot code; never live.
- **BOO-2 + COI-3 (P3, diagnostic specificity):** give
  `find_lowest_prevout_unresolved_hole_unlocked` a wanted-status parameter so a lower
  transient `internal_error` row cannot mask a higher genuine `prevout_unresolved` tear
  — at BOTH call sites (`torn_gate.c:151` AND `stage_repair_coin_backfill.c:466`). Make
  `resolve_creator` (`stage_repair_coin_backfill.c:177-262`) a whole-set verdict (track
  the strongest class across all outpoints instead of breaking on the first reason).
- **CON-1 (P2):** make `db_maintenance_dump_state_json` non-blocking — trylock in the
  dump only (not `db_maintenance_status_snapshot`), emit `busy:true` + atomic
  `loop_ticks` on EBUSY so diagnostics stay responsive mid-VACUUM.
- **CON-2 (P2):** serialize `sync_monitor` `g_local_recovery` cross-thread access
  behind a dedicated static mutex, snapshotting to locals first to avoid nesting with
  `cs_nodes` (LOCK-ORDER LAW — done wrong this re-introduces the ABBA class).
- **CON-3 (P3):** static-initialize the two `tip_finalize_stage` dump mutexes
  (`PTHREAD_MUTEX_INITIALIZER`); currently zero live reachability — harmless.
- **CON-5 (P2):** brief `cs_main` snapshot in `block_index_dump_state_json` (incl.
  multi-word `nChainWork`) before formatting JSON.
- **COI-2 (P1, consensus-surface, REPORT ONLY — needs full-history replay):** a
  within-block cross-transaction double-spend is silently accepted on the live reducer
  path (`utxo_apply_delta.c:162-256` builds the spend set with no within-block
  consumed-set; the second `coins_kv_spend` is a no-op). zclassicd's ConnectBlock
  rejects these; the canonical catch `update_coins_with_undo` (`update_coins.c:57-63`)
  is dead (only `connect_block`, no production caller, reaches it). History-safe (no
  such block exists on the immutable chain), but a crafted forward block would fork.
  Fix RESTORES parity: scan recorded spent[] for a matching outpoint before recording;
  `delta_fail` with a non-repairable status (NOT `value_overflow`, which the ladder
  treats as repairable). Per the Consensus Parity Doctrine, **must be validated by a
  full-history replay** against the real chain and pinned by a `test_consensus_parity`
  golden before shipping. Do not land without the replay.

## DRY backlog (duplication audit)

Applied this pass (safe, contained, gate-verified): `lib/core/src/core_io.c`
(hoisted the duplicated `script_solver` decode out of the if/else),
`tools/mcp/controllers/ops_controller.c` (added `status_extract_json_int` +
`status_count_json_objects`, routed the ~6 inlined scrape/brace-count sites).

Tracked structural dedups (each its own reviewed change — cross-file, some
consensus-adjacent; ~900 duplicated lines total):

- **DRY1 — projection lifecycle template** (~340 lines): `open`/`close`/
  `set_event_log`/`current`/`exec_sql`/`append_*_event`/`dump_state` preambles are
  near-identical across the 8 `lib/storage/src/*_projection.c`. Extract a shared
  base; must keep the E4 *projections-pure* gate satisfied.
- **DRY2 — adapter scalar-read idiom** (~160 lines): `prepare → AR_STEP_ROW_READONLY
  → read col 0 → finalize` is copy-pasted ~20× across `adapters/outbound/persistence/*`.
  Add one checked scalar-read helper. Also: byte-identical `wal_size_bytes`
  (node_health_store + db_maintenance) and the `ctx_of/db_of/ndb_of` casts (→ one
  `PORT_SELF` macro).
- **DRY3 — reducer stage prologue** (~110 lines): `*_stage_init(ms)` prologue is
  identical across the 8 `app/jobs/src/*_stage.c`; the cross-table "read row at
  height" log readers (~90) and `*_log_ensure_schema` template (~85) repeat across
  `app/jobs/src/*_log_store.c`. Consensus-adjacent — gate + review.
- **DRY4 — controller boilerplate**: rpc-table register tail (24 controllers → macro),
  `main_state`-not-initialized guard (~12×), inline byte→hex loops bypassing
  `HexStr` (~12×), and the HTTP-200 no-cache response preamble (~13×).
- **DRY5 — small byte-identical helpers**: `load_u256`/`state_get_i32` across the 3
  `chain_evidence_*` files; the `DOMAIN_TX_FAIL`/`DOMAIN_SAP_FAIL` macros in
  `domain/consensus/src` (consensus path — move with care).
