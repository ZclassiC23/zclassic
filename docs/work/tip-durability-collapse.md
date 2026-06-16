# Chain-Tip-Durability Collapse — the wedge-class kill

The corrected mechanism (move the coins set INTO progress.kv as a `coins` table)
has **landed** (`lib/storage/src/coins_kv.c`). This doc is the rationale of
record for the `coins_kv` subsystem — the WHY/invariant anchor cited by the
source.

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
(`script_validate` then logs `prevout_unresolved` on perfectly valid blocks; this
hit exactly height 3134571, which the oracle confirms is a **valid** main-chain
block — so torn LOCAL state, not a consensus bug). The in-code comment at
`utxo_apply_stage.c:447` ("crash-safe … independent of the stage cursor txn") is
the bug, stated as a feature.

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
would reintroduce the exact tear.

**Why NOT collapse onto a separate-db `applied_height`:** the inverse-delta (for
reorg unwind) lives in progress.kv and MUST be atomic with the coin mutation;
keeping coins in a separate db forces a second non-atomic boundary. One db is the
only clean answer.

## Boot invariant + standing regression contract

**Boot invariant:** served tip == tip_finalize contiguous ok=1 frontier == coins
MAX height; FAIL LOUD on divergence (silent-halt doctrine).

**MANDATORY acceptance gate:** kill-9 mid-climb ≥10×, restart each time — coins
MAX height == `utxo_apply` cursor-1 == tip_finalize frontier on EVERY restart (no
drift). ADVANCE_DEADLINE ≥300s. This is the proof the class is dead.

No must-never-fork gate is weakened: reorg detect, chainwork-greater, UTXO
conservation, PoW/Equihash, MMB/FlyClient, script/proof validation, crypto,
sapling all stay exactly where they are on the tip_finalize publish path. The
change only moves the *durability boundary* of the coins fold (now atomic),
which can only make consensus state more consistent. No `tip_finalize_log` row is
ever deleted (anti-rewind preserved); the public tip can never drop below the
contiguous ok=1 frontier. (Boot keeps a back-compat replay of pre-migration
event_log into the new progress.kv `coins` table.)

## Live-node recovery (the torn datadir is unhealable — rebuild clean)

A torn datadir is **unhealable by forward self-heal**. The live `~/.zclassic-c23`
is shredded across body_fetch holes, projection, coins, and the cec counter. It
must be **rebuilt clean**, owner-gated.

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
