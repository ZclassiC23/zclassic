# Plan: persist the `utxo_sha3` commitment so the §3 self-heal auto-fires

> ## ✅ DURABILITY KEYSTONE — Option A landed 2026-06-06 (`801832692`)
> A 3-lens consensus-safety panel (`wf_8ea5ab58-84d`, GO_WITH_FIXES) found the
> minimal safe fix for reducer-mined-block durability is **NOT** a coins.db write
> (Options B & C were REJECTED — the reducer writing coins.db would corrupt the
> mainnet boot integrity gate and hit the wrong-table commitment hole). The
> reducer's durable state (`tip_finalize_log.tip_hash` + cursor, `utxo_projection`)
> is already crash-safe and boot already runs the forward-only seed
> `block_index_loader_seed_tip_from_finalized`. The only gap: the block_index map
> was never persisted on a small chain (`size > 1000` gate). **Fix: lower the
> shutdown persist gate to `> 1`** (boot_services.c). VERIFIED: regtest
> `generate 5` → clean restart → `getblockcount=5`. Mainnet byte-identical
> (always >>1000). coins.db stays the untouched stale anchor (boot.c:1743
> unchanged). REMAINING: under SIGKILL the reducer tip is rewound toward the
> stale coins.db anchor — that is the single-engine cutover proper (reducer tip
> vs coins.db), still owner-gated. The `utxo_sha3` commitment-persist below
> remains the OPTIONAL hardening for the §3 self-heal (must compute over the
> PROJECTION, not the stale coins.db `utxos`).
>
> ---

## Context

The shipped boot self-heal `coins_reconcile_stale_anchor`
(`lib/storage/src/coins_view_sqlite.c`, commit `11f87b8b6`) heals a stale
`coins_best_block` anchor only when a stored height-stamped SHA3 commitment
(`utxo_sha3`) recompute-matches the live `utxos` table. But `utxo_sha3` is
written only at snapshot-import + one RPC path, so a long-running node never
has a fresh one — the self-heal can't fire on a *recurring* §3 wedge. This
closes that gap by persisting the commitment at a consistent boundary.
(`-reindex-chainstate` reachability — commit `50b33285d` — is the operator
fallback; this makes the *automatic* path real.) An original paper plan named
the wrong persist boundary and was rejected (DO_NOT_APPLY, 3-lens panel
2026-06-03, `wf_c80cf28e-371`); the corrected design below is the live spec.

## CORRECTED DESIGN (adversarial review 2026-06-03) — read this first

The paper plan named the wrong boundary. Against the real tree:

1. **Wrong persist site (fatal).** `flush_coins_if_needed` /
   `process_block_flush_coins` have **zero production callers** — dead on the
   live path. The authoritative advance is the staged reducer
   (`app/jobs/src/utxo_apply_stage.c` → `utxo_projection`); live coins-SQLite
   persistence happens only at **boot-reindex** (`boot_index.c`) and
   **shutdown** (`shutdown_flush_coins_to_sqlite`). A per-block cadence gate
   here would never fire → the anchored record would never be written. So the
   whole IBD/cadence machinery is misplaced.
2. **Persist must be ATOMIC with the coins flush (HIGH).** Write the 76-byte
   anchored `utxo_sha3` record **inside `coins_view_sqlite_batch_write_ex`'s
   transaction** (next to the rows + `coins_best_block` + consensus-commitment
   writes, before the COMMIT) — not in a trailing separate txn (crash window
   otherwise: on-disk record wouldn't match the durable set).
3. **Derive height/identity from the table, in the same txn (HIGH).** Don't
   trust caller-supplied height/hash. Use `SELECT MAX(height), COUNT(*) FROM
   utxos`, resolve the anchor as the `status>=3` block AT that height, and
   **refuse to stamp** unless `coins_best_block` matches it. Only stamp when
   the cache is empty post-flush (no pending higher rows).
4. **Anchored heal must re-validate the stored hash (HIGH).** In
   `coins_reconcile_stale_anchor`, after `_load_anchor` succeeds, run
   `SELECT height FROM blocks WHERE hash=? AND status>=3` and require
   `height == commit_h`; on any miss, fall back to the existing height→hash
   path (never re-point blindly).
5. **Don't reuse `EV_DB_ERROR` for a success seal** (pollutes the node-health
   error ring) — add `EV_COINS_COMMITMENT_SEALED` or reuse `EV_COINS_FLUSH`.
6. **Don't churn the dead `process_block` flush API** signatures.

Net: the keystone is achievable, but it belongs in `batch_write_ex` (the real
coins-commit boundary), table-derived + atomic, with a validated anchored
heal — a materially different change from the original below. The 76-byte
record format + back-compat `_load`/`_load_anchor` from the spec are sound and
reusable. Implement carefully on a datadir COPY; deploy stays owner-gated.

## Test (`lib/test/src/test_coins_commitment_persist.c`, new)

Driving the real `flush_coins_if_needed` needs a full chain, so instead: build a
min node.db, apply a set, invoke the `ZCL_TESTING` hook
`process_block_test_persist_commitment_once(cvs, flushed_hash)` (forces cadence,
runs the real helper), tear the anchor, and assert `coins_view_sqlite_open()`
heals against the just-persisted commitment. Register in the usual 3 places.
The full drafted code is in the workflow result
(`finish-s3-unhaltable`, run `wf_77539f37-539`).

## Out of scope (D audit, all P3 — not worth the churn)

DRY-extract the anchor-resolve SQL (3 sites); route `EV_OPERATOR_NEEDED` from
the boot coins `_exit` (blocked on `alerts_init` ordering); redundant ROLLBACK
after a failed COMMIT in `coins_rewind_above_tip`. Surfaces audited clean
otherwise.
