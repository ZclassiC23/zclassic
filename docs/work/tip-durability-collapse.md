# Chain-Tip-Durability Collapse — the wedge-class kill

Status: **GO, mechanism corrected** (adversarially verified across two 7–9 agent
workflows, 2026-06-07). The earlier "ATTACH utxo_projection.db into progress.kv"
mechanism was **refuted** (WAL — see below); the corrected mechanism is to move
the coins set INTO progress.kv as a table. Supersedes the prior task#2
"convergent" and "layered" designs and the rejected "collapse-onto-utxo_apply-
cursor" variant.

## The disease (every tip-wedge for weeks)

The served chain tip / "coins applied through N" is recorded **redundantly** in
multiple stores with **no atomic cross-commit**, so any crash/recovery drifts
them apart and boot *guesses* which is right. Live 4-way split observed:
`coins_best_block HASH`=3134313 / `cec.coins_best_block_height` INT=3134559 /
`utxo_apply` cursor=3134559 / `utxos MAX`=3132687. The guessing is the bug factory.

### Root cause, code-confirmed (the single tear window)

The reducer's 8 stage logs + their cursors all commit atomically inside
`stage_run_once`'s `BEGIN IMMEDIATE` on **progress.kv** (`stage.c:316-356`) — they
never tear. **The coins set is the one exception.** `utxo_apply` step:
1. `emit_delta()` (`utxo_apply_stage.c:244`) appends `EV_UTXO_ADD/SPEND` to the
   **event_log** (a separate file) via `event_log_append()` — **two fsyncs
   OUTSIDE** the stage txn (`utxo_projection.c:430,450`).
2. The inverse-delta + `utxo_apply_log` row + the stage cursor commit **later**,
   inside `stage_run_once`'s txn on **progress.kv**.
3. `utxo_projection_catch_up()` then folds the event_log into the coins table in
   **utxo_projection.db** — a **third** file, its **own** txn, advancing
   `last_consumed_offset` (`utxo_projection.c:362-396`).

So the coins (event_log + utxo_projection.db) and the cursor (progress.kv) live
in different WAL databases with **no atomic cross-commit**. A kill-9 anywhere in
that window leaves the cursor ahead of/behind the coins it claims to have
applied. Worse, `last_consumed_offset` is **height-blind** and is **never
rewound** when a reorg rewinds the stage cursor — so `catch_up` resumes from a
stale offset and **no forward path can rebuild a torn projection**
(`script_validate` then logs `prevout_unresolved` on perfectly valid blocks; the
PROOF below hit exactly this at height 3134571). The in-code comment at
`utxo_apply_stage.c:447` ("crash-safe … independent of the stage cursor txn") is
the bug, stated as a feature.

### PROOF (2026-06-07)

A full copy of the live torn datadir, booted on the Step-2 binary (`e9e7a1f6c`),
**could not climb past the wedge**. Boot took the "restoring chain tip" path; the
Step-2 reconcile clamp never fired (reorg detection had already rewound
tip_finalize *below* the floor, disarming the `cur>floor` clamp precondition,
`stage_repair.c:56`). Preserved log summary showed the disease end-to-end:
`565×` cec anti-rewind "refusing to rewrite persisted high tip 3134559 down to
3134313", `246` block-body holes from 3134314 (`have_data_missing`),
`not_script_valid` at 3134570. The oracle (zclassicd, height 3138774) confirms
3134571 is a **valid** main-chain block — so this is torn LOCAL state, not a
consensus bug. A torn datadir is **unhealable by forward self-heal**.

## The single source of truth (verified)

**`tip_finalize`'s contiguous finalized frontier** — the row + the stage cursor
commit in ONE `stage_run_once` txn. It is the consensus gate (reorg detect →
chainwork-greater → UTXO conservation → MMB/FlyClient + wallet/Sapling), not a
redundant durability stage. Once the coins set commits in the **same** txn, the
finalized frontier and the coins are atomic by construction.

Rejected authorities (both NO-GO, code-confirmed): `coins_best_block` HASH is
**dead-legacy** (`process_block` has zero live callers) and not atomic; the
`utxo_apply` cursor advances PAST failed blocks (`utxo_apply_stage.c:213-216`).

## The structural rip-out (corrected mechanism)

**Move the coins/UTXO set INTO progress.kv as a `coins` table, written inside
`stage_run_once`'s `BEGIN IMMEDIATE`** — exactly the proven-correct pattern of
`created_outputs_index` (a progress.kv table written in the body_persist txn,
which never tears). Then every effect of block N — coin add/delete, inverse-delta,
`utxo_apply_log` row, stage cursor — commits or rolls back as **one atomic unit**
in **one database / one WAL**. By construction there is nothing left to drift and
nothing for boot to guess between.

**Why NOT `ATTACH`:** SQLite gives atomic multi-database commit (master journal)
ONLY in rollback-journal mode. **progress.kv and utxo_projection.db are both WAL**
(`progress_store.c:59`, `utxo_projection.c` checkpoints) — in WAL there is no
master journal, so a cross-attached-db transaction is **NOT atomic**. ATTACH
would reintroduce the exact tear. (This refuted the first design's step 3.)

**Why NOT collapse onto a separate-db `applied_height`:** the inverse-delta (for
reorg unwind) lives in progress.kv and MUST be atomic with the coin mutation;
keeping coins in a separate db forces a second non-atomic boundary. One db is the
only clean answer.

## Ordered steps (each independently gateable + copy-provable)

The established safe pattern in this repo for consensus-store moves is
**additive → flip reads → delete old** (cf. the single-engine `EV_BLOCK_HEADER`
work). Apply it here so the tree stays green per commit:

1. **Additive coins table in progress.kv** + an in-txn apply primitive
   `coins_kv_apply_delta(db, height, hash, adds[], spends[])` (INSERT adds /
   DELETE spends + inverse-delta) callable from inside `step_apply` so it lands
   in `stage_run_once`'s txn. Schema mirrors the projection's `utxo` table.
   Gate: build + lint + test_parallel 0; unit test that apply+rollback is atomic.
2. **Dual-write** (authoring): `step_apply` writes the coins delta into the
   progress.kv `coins` table in-txn AND (temporarily) keeps the event_log emit,
   so existing readers are unaffected. Gate: parity test — progress.kv `coins`
   == utxo_projection `utxo` after a drain (reuse `test_utxo_apply_authorship_parity`).
3. **Flip reads**: repoint `projection_live_lookup`, `script_validate`'s prevout
   resolver, `coins_view_projection`, snapshot_apply, reorg-unwind, and the
   commitment/explorer readers to the progress.kv `coins` table. Gate: build +
   lint + full test_parallel 0 + **COPY-PROOF** (clean-pipeline climb + SHA3
   UTXO-commitment EXACT match vs oracle `gettxoutsetinfo`).
4. **Delete the old path** on the live forward path: drop the event_log emit +
   post-step `catch_up` (keep `catch_up` only for boot back-compat replay of
   pre-migration datadirs). Delete `cec.coins_best_block_height` + Guard A
   (`chain_evidence_authority_service.c:635/527`, `chain_evidence_reconstruct.c:200`,
   `utxo_recovery_service.c:192`; repoint readers `event_controller.c:53`,
   `diagnostics_registry.c:346`, `chain_evidence_snapshot.c:96` + fix
   `test_syncdiag_rpc.c`, `test_node_health_service.c`). Pure subtraction.
5. **Boot invariant assert** (in a TU outside the frozen `boot.c`): served tip ==
   tip_finalize contiguous ok=1 frontier == coins MAX height; FAIL LOUD on
   divergence (silent-halt doctrine). Demote `coins_best_block` HASH writes to
   the cold-import/recovery path only.

**MANDATORY acceptance gate for steps 3–4:** kill-9 mid-climb ≥10×, restart each
time — coins MAX height == `utxo_apply` cursor-1 == tip_finalize frontier on
EVERY restart (no drift). ADVANCE_DEADLINE ≥300s. This is the proof the class is
dead.

No must-never-fork gate is weakened: reorg detect, chainwork-greater, UTXO
conservation, PoW/Equihash, MMB/FlyClient, script/proof validation, crypto,
sapling all stay exactly where they are on the tip_finalize publish path. The
change only moves the *durability boundary* of the coins fold (now atomic),
which can only make consensus state more consistent. No `tip_finalize_log` row is
ever deleted (anti-rewind preserved); the public tip can never drop below the
contiguous ok=1 frontier.

## Live-node recovery (the torn datadir is unhealable — rebuild clean)

The live `~/.zclassic-c23` is shredded across body_fetch holes, projection,
coins, and the cec counter. It must be **rebuilt clean**, owner-gated.

**`cold-import` is NOT safe against the always-running oracle** (refuted,
2026-06-07): the auto-trigger pending-anchor metadata is read but not written by
present code, and `ldb_snapshot_make` hardlinks the oracle's LevelDB SSTs — a
race with the oracle's compaction can ENOENT mid-copy and fall back to unlinking
the oracle's LOCK (`ldb_snapshot.c:99-109`, no SST-deletion retry). **Do not run
it against the live oracle.**

**Safe recovery = clean P2P resync** of a wiped `~/.zclassic-c23` from the oracle
peer (127.0.0.1:8033) + network — reads **nothing** from the oracle's files, so
zero oracle risk; produces coins == projection == frontier by construction (and
exercises MVP #1 cold-sync). Prove it on a fresh isolated datadir first; the live
wipe+resync is the one OWNER-GATED action. The structural fix must land first so
the rebuilt datadir cannot re-tear.

## Deferred (off the critical path, owner-gated)

- Eventual deletion of the dead `process_block` / `coins_view_sqlite` legacy
  connect engine once recovery is re-rooted off it.
- Migration of existing on-disk datadirs (boot replay of pre-migration event_log
  into the new progress.kv `coins` table — back-compat path in step 4).
