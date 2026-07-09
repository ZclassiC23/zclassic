# S2 — utxo_mirror delta-apply (landed) + the persistent node.db lock holder

**Date:** 2026-07-09. **Status:** S2 delta-apply is CODE-COMPLETE, unit-tested,
and `make lint` green. It is **NOT yet copy-proven on a live datadir copy and
NOT deployed to canonical** (canonical is operator-gated). The second half of
the P0 — the persistent node.db write-lock holder — is now DIAGNOSED but NOT yet
fixed. Read this before touching the wallet-persistence P0.

## The P0 (recap)
Wallet key persistence is broken on canonical: `getnewaddress` fails,
`getwalletinfo.persistence` shows `healthy:false, mismatch:true`. Root symptom:
node.db (WAL, busy_timeout=10s) has a writer holding the write lock >10s, so the
wallet-key flush and the utxo_mirror both get `SQLITE_BUSY` ("database is
locked"). Two independent causes, BOTH must be fixed:

### Cause 1 — the mirror's O(1.3M)-per-block rebuild  → **FIXED here (S2)**
`utxo_mirror_sync_service.c` detected drift every 5s and ran
`mirror_rebuild_from_coins_kv`: `DELETE FROM utxos` + re-INSERT of the entire
~1.3M-row UTXO set (plus a full `addresses` GROUP BY over all rows) in one
node.db transaction — a multi-second write-lock hold on essentially every
accepted block.

**What landed:** an incremental delta path.
- New module `app/services/src/utxo_mirror_delta.{c,h}`
  (`utxo_mirror_delta_apply`): applies only the coins changed in
  `(cursor, frontier]` — upserts the live coins_kv rows created in that height
  range, deletes the outpoints in `utxo_apply_delta.spent_blob` for those
  heights, and re-derives the `addresses`/`wallet_utxos` caches for ONLY the
  touched addresses (`db_utxo_refresh_caches_for_address`, new in
  `app/models/src/utxo.c`). Per-pass height cap (2000) bounds the write-lock
  hold; a large post-outage gap catches up over a few passes.
- The whole apply is idempotent and provably converges on coins_kv regardless of
  chunk boundaries (a coin created-and-spent in-range is never live so never
  inserted; its spent_blob delete is a harmless no-op).
- `utxo_mirror_sync_service.c` now tries the delta first and reserves the
  wholesale rebuild for the cases delta can't handle: fresh/empty mirror, a torn
  mirror (cursor==frontier but row counts differ), a reorg (frontier<cursor), or
  a missing `utxo_apply_delta` row (returns FALLBACK). Those self-heal via the
  existing full rebuild.
- Test: `lib/test/src/test_utxo_mirror_sync.c` (now a real `test_parallel` group,
  `make t ONLY=utxo_mirror_sync`) proves the delta path adds+deletes the right
  coins, maintains the `addresses` aggregate incrementally, returns
  rows_CHANGED (not a full row count — the discriminator that no full wipe ran),
  advances the cursor durably, and falls back on a missing delta row.

Gates run: `make t ONLY=utxo_mirror_sync` (all OK), `make lint` (all checks
passed). Full `make t` / `make ci` is the pre-push gate.

### Cause 2 — a second node.db connection holds a long write txn → **NOT fixed**
A read-only investigation (see below) found node.db writes are supposed to
funnel through the single serialized `db_service` write lane, but several
services open their OWN node.db connection and bypass it. In WAL, a second
connection holding a write transaction produces `SQLITE_BUSY` for every other
writer once it holds past busy_timeout.

**Prime suspect (confirmed by live logs):** the node.db catch-up job
(`app/services/src/node_db_catchup_service.c`, launched via
`sync_controller_catchup_jobs.c:55` `node_db_sync_open_private_db_like` →
`sync_controller.c:217-233`) opens a private READWRITE connection and holds ONE
transaction across its scan (commits only every `batch_size=100000` blocks,
`node_db_catchup_service.c:504`), entering turbo (`wal_autocheckpoint=0`). It is
re-armed every ~5s by the projection-backfill thread
(`boot_background_workers.c:475-488`) whenever `projection_tip < chain_tip`.
Live logs at tip show `SQLite catchup: 1 blocks ... complete in 0s` per block
AND `chain_advance_coordinator.* ... database is locked` — i.e. the private
connection contends with the lane.

Secondary suspect: `boot_address_backfill.c` (another private READWRITE
connection, 10k-row batches over ~1.3M addresses; boot one-shot, multi-minute).
Others that bypass the lane: `store_controller.c` (30s timer), `zslp_service.c`.

**Recommended fix (next session):** route the catch-up writes through the shared
`db_service` lane instead of `node_db_sync_open_private_db_like` (so ALL node.db
writes serialize and cross-connection BUSY becomes impossible), OR — smaller —
shorten the catch-up commit cadence so it never holds past busy_timeout. Prefer
the lane route; it generalizes to the whole "multiple node.db writers" class.

## Remaining steps to close the P0 (in order)
1. **Copy-prove S2** on a copy of the live datadir: run the built binary against
   `tools/repro_on_copy.sh`-style copy and confirm (a) the mirror ADVANCES to
   the frontier (no more `rebuild: aborted after 0 rows`), (b) "database is
   locked" frequency drops, (c) `getnewaddress` persists. If the lock errors
   persist after S2, Cause 2 is confirmed load-bearing → do the catch-up-lane
   fix and re-prove.
2. **Fix Cause 2** (catch-up private connection → lane). Copy-prove.
3. **Deploy to canonical** — operator-gated (`ZCL_DEPLOY_ALLOW_CANONICAL` /
   owner confirmation). Do NOT auto-deploy; canonical protects the public node +
   real funds. Deploy to the dev lane first to observe.

## Note on the Codex executor tier
The `codex:codex-rescue` executor was unavailable this session — the Codex CLI's
local command sandbox failed to initialize on this host (loopback/namespace
error), so it could not read files, edit, or run gates. S2 was implemented
directly. See memory `reference_codex_sandbox_blocked_2026-07-09`.
