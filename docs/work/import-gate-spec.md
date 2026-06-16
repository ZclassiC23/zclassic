# Import-Correctness Gate — design spec

**Status:** implemented (2026-06-13, commit `7a1f87471`) — bless-time
forward-evidence torn-import gate in `block_index_loader_torn_gate.c` (Part A)
+ de-vacuumed SHA3 checkpoint stamp in `utxo_recovery_restore.c` (Part B1).
Residuals (unspent-forever torn coin; P2P-snapshot seed path) tracked below.
**Motivates:** the recurring I4.3 anchor-collapse wedge (live main node held at
h=3,145,594). Root cause memory: `project_recurring_anchor_collapse_wedge_2026-06-13`.
**Blocks (when fixed):** MVP C3 (fast cold-sync), C6 (zero operator
intervention), C8 (consensus parity at tip) — all need sustained live forward
progress, which the wedge denies.

---

## TL;DR — the reframe

A *write-time* gate that refuses a coin-incomplete import is **infeasible**: at
cold-import write time the block **bodies are stripped** and a missing *mature*
coin is indistinguishable from a legitimately-spent one. The sound fix is a
**bless-time forward-evidence gate** plus a **de-vacuumed commitment stamp**. It
does not promise write-time completeness it cannot deliver; it guarantees that **a
coin set forward-validation has already proven torn is never blessed as the
trusted base**, and that the operator gets a **typed, actionable** verdict instead
of an opaque HOLD. It is a no-op on a faithful import (zero false-reject), never
floats H* over a real `ok=0`, and adds no repair rung (TENACITY I3 preserved).

---

## Problem statement (code-grounded)

The trusted-base coin set is **missing** the canonical coinbase
`60fc6f43a630b5b7:0` at h≈3,145,486. Canonical block 3,145,595 (~109 below the
tip) spends it. The honest machinery then does exactly what it should:

1. `script_validate` records `ok=0 prevout_unresolved` (live:
   `script_validate.blocking_height=3145595`, `blocking_status=prevout_unresolved`,
   txid `31a1019d…` vin 9).
2. `coin_backfill` refuses to fabricate the coin (live: `refused_unprovable`,
   `refuse_reason=txindex_miss tx=60fc6f43a630b5b7`).
3. `reducer_anchor_candidate_ok` (`app/jobs/src/reducer_frontier.c:209-236`)
   rejects the durable trusted base because the first row above it is not `ok=1`.
4. H* collapses to the compiled checkpoint 3,056,758; the MIN-fold reads the
   log-less import region as an 88k hole.
5. `invariant_sentinel.c:237-251` latches the I4.3 HOLD.

**The HOLD is honest and correct.** The defect is *upstream*: nothing prevents a
coin-incomplete set from being **stamped/blessed as the trusted base** in the
first place, and the existing commitment check is **vacuous** —
`utxo_recovery_restore.c:378-380` stamps the SHA3 of *whatever was imported*, and
`seed_integrity_gate.c` `gate_commitment_check` (:180-210) recomputes over the
*same* table and returns true. It certifies the tear.

**Provenance (probe-verified):** a fresh two-step cold import on HEAD from the
running zclassicd connects **cleanly** through 3,145,595 to 3,145,830. The import
*copy* path is faithful; the live tear is a **one-off** that entered during the
node's original P2P sync (a transient reorg captured a wrong coinbase, recorded
into the coin set, never reconciled). So the gate is **preventive**: stop a
transient artifact from silently becoming "our" chain across reboots.

Why no cheaper detector exists:

- **block_map_find is a pure hash lookup** (`lib/validation/src/chainstate.c:53-72`)
  — the tear is invisible to any header/hash probe (block_index is canonical,
  hash-matches zclassicd); it lives **only in the coin set**.
- **Depth 109 > COINBASE_MATURITY=100** means the coinbase was already mature and
  legitimately spendable, which is *why* 3,145,595 spends it. So per-height
  coinbase presence cannot distinguish torn from spent. (155 distinct coinbase
  heights exist in the region — a coverage-count gate never fires.)
- **The failure is per-OUTPOINT, not per-height.** `created_index_prevout`
  (`app/jobs/src/script_validate_stage.c:118-189`) resolves `prevout->hash` +
  `prevout->n` via `coins_kv_get`; a same-height *different-txid* orphan coinbase
  never resolves the missing canonical outpoint.

There is only **one** compiled SHA3 checkpoint, at 3,056,758, ~88k blocks below
the import (`lib/chain/src/checkpoints.c:86-104`); and at cold-import write time
`--importblockindex` clears `BLOCK_HAVE_DATA|BLOCK_HAVE_UNDO` and zeroes file
positions (`app/controllers/src/snapshot_controller_import.c:174-193`), so bodies
are fetched lazily over P2P *after* import — there is nothing to ask "does the
active chain spend a coin I am missing" against without the full replay fast-sync
exists to avoid (~25 min, the C3 budget).

---

## The design

Two parts. **Part A** is the primary mechanism; **Part B1** removes a false trust
signal; **Part B2** is owner-gated belt-and-suspenders.

### Part A — bless-time forward-evidence gate (primary)

**Where:** `app/services/src/block_index_loader_rebuild.c`,
`block_index_loader_seed_stages_from_cold_import` — immediately **after** the
existing (2d) coin-count cross-check (ends ~`:613`) and **before** the (3) H*
trigger / (5) `tip_finalize_stage_seed_anchor` (~`:671`). This function already
has `progress_db` in scope and runs **every boot**.

**Algorithm:** under `progress_store_tx_lock()`, call the existing helper
`find_lowest_prevout_unresolved_hole_unlocked(progress_db, /*cursor=*/H+1,
&hole_h, status, &hole_hash, &found)`
(`app/jobs/src/stage_repair_coin_backfill_util.c:133-180` — it `SELECT`s the
lowest `script_validate_log` row `WHERE ok=0 AND status IN
('internal_error','prevout_unresolved','block_decode_failed') AND height<cursor`).
If `found` **and** `hole_h` is within `(compiled_anchor, H]`, the durable
forward-validation evidence already proves the seed's own coin set is torn below
H → **refuse to bless**.

**Why this is sound and not circular:** it is preventive, not a post-hoc relabel.
On the **first boot where forward validation has recorded the hole**, the seed is
refused and **never re-blessed across reboots** — so a transient artifact cannot
silently become the trusted base. On a faithful import there is **no** `ok=0`
hole below H (the probe-clean fixture connects through 3,145,830), so the gate is
a **no-op** — zero false-reject.

### Part B1 — de-vacuum the commitment stamp (supporting)

**Where:** `app/services/src/utxo_recovery_restore.c:308-315`. Today the
"verify-later" branch (`imported_count > 100000`) leads to a `utxo_sha3` stamp
(`:378-380`) at *any* height; `gate_commitment_check` then recomputes the same set
and passes vacuously. **Change:** only stamp `utxo_sha3` when the imported set
matches the **compiled checkpoint** (the `:309-311` condition — `cp->utxo_count
&& memcmp(root)==0`). At non-checkpoint heights, do **not** stamp. This removes
the **false claim** of cryptographic verification, leaving Part A as the sole,
honest trust authority above the checkpoint. It changes nothing for faithful
imports (the gate was already vacuously-true).

### Part B2 — owner-gated higher checkpoints (deferred, belt-and-suspenders)

Optionally ship additional compiled SHA3 checkpoints at higher heights via owner
ceremony (`lib/chain/src/checkpoints.c:86-104` currently has exactly one). An
import landing **exactly** on a shipped checkpoint then gets a genuine O(n) SHA3
cross-check, which is the only thing that catches an **unspent-forever** torn coin
(see scope limits). **Not the primary mechanism** — requires a same-binary
cross-node SHA3-equality proof first, else it false-rejects legitimate imports.

---

## Failure behavior (exact)

On a detected hole within `(compiled_anchor, H]`:

1. `block_index_loader_seed_stages_from_cold_import` returns **0** (its existing
   no-bless return) → **no** tip_finalize anchor, **no** `coins_applied_height`
   stamp, **no** upstream cursor advance. The seed is never blessed; H* stays
   pinned at the compiled checkpoint 3,056,758 (the reducer keeps refusing the
   candidate via `reducer_anchor_candidate_ok`, exactly as today).
2. A **typed PERMANENT blocker** is raised: `blocker_init(&rec, "seed.torn_import",
   "validation_pack", BLOCKER_PERMANENT, reason)` + `blocker_set(&rec)`, then
   `event_emitf(EV_OPERATOR_NEEDED, 0, "check=seed.torn_import seed_h=%d
   first_hole_h=%d %s", H, hole_h, reason)` — same mechanism as
   `seed_integrity_gate.c:64-68` and `invariant_sentinel.c:115`.
3. The operator message names the **action**: *"torn cold-import coin set: forward
   validation found an unresolved prevout at h=&lt;hole_h&gt; below seed H=&lt;H&gt;;
   the imported trusted base is coin-incomplete. ACTION: wipe ~/.zclassic-c23 and
   re-import with the two-step cold-sync recipe (`--importblockindex` then normal
   boot). See TENACITY cold-sync recipe."*
4. **No** seed/cursor stamp, **no** float of H* over the `ok=0` row, **no**
   downstream repair rung (TENACITY I3 preserved).
5. Part B1: at non-checkpoint heights `utxo_sha3` is not stamped, so the
   commitment check stays honestly unarmed.

The I4.3 HOLD remains the honest steady state; this gate makes the **refusal
happen at bless time** so the torn set is never trusted in the first place,
rather than being re-discovered by forward validation each boot.

---

## What this does NOT cover (honest scope)

1. **True write-time completeness for buried mature coins** — not provided, and
   infeasible (bodies stripped; full replay forbidden). The guarantee is HONESTY +
   a CLEAR action, not silent self-healing.
2. **An unspent-forever torn coin** — if the active chain never spends the missing
   coin, `script_validate` never records `ok=0`, so the forward-evidence gate has
   nothing to fire on. Only a full-set commitment with an external reference
   (owner-gated Part B2) catches that. Out of scope for the live wedge, which **is**
   spent (at 3,145,595).
3. **Window latency** — on a fresh boot of a torn datadir the gate fires only
   *after* `script_validate` has advanced past the spending block and recorded the
   `ok=0` row. On the live node that evidence is already durable (persists across
   reboots), so the gate fires at the next bless attempt. A torn set whose spending
   block has not yet been validated would bless once, then refuse on the subsequent
   boot. It never floats H* over the `ok=0` row, so this is **refusal latency, not a
   safety hole**.
4. **Part B1 removes a false guarantee; it does not add a new one.** Above the
   single compiled checkpoint, the only honest trust is the count cross-check + the
   forward-evidence gate.

---

## Where it lives

- **Part A** is `block_index_loader_torn_gate.c`, called from
  `block_index_loader_seed_stages_from_cold_import` after the (2d) coin-count
  cross-check (~`:613`) and before the (3) H* gate (~`:615`) /
  `tip_finalize_stage_seed_anchor` (`:671`). It reuses
  `find_lowest_prevout_unresolved_hole_unlocked`
  (`app/jobs/src/stage_repair_coin_backfill_util.c:133-180`) as-is, holding
  `progress_store_tx_lock`. The typed blocker id `seed.torn_import` and entry point
  live in `app/services/include/services/seed_integrity_gate.h` (helper in
  `app/services/src/utxo_recovery_frontier_gate.c`), reusing
  `blocker_init(BLOCKER_PERMANENT)` + `event_emitf(EV_OPERATOR_NEEDED)` exactly as
  `seed_integrity_gate.c:61-69` / `invariant_sentinel.c:115`.
- **Part B1** is `utxo_recovery_restore.c:308-315`: gate the
  `seed_integrity_stamp_utxo_sha3` call (`:378-380`) on the compiled-checkpoint
  match only.
- The `seed.torn_import` blocker is **cleared** on the clean-reimport path in
  `app/services/src/utxo_recovery_seed_provenance.c:68-78`, next to
  `utxo_recovery_clear_cold_import_seed`, so a clean re-import is not permanently
  blocked.

Tested by `lib/test/src/test_seed_torn_import_gate.c`: torn fixture →
`block_index_loader_seed_stages_from_cold_import` returns 0, blocker
`seed.torn_import` present, `EV_OPERATOR_NEEDED` emitted naming `first_hole_h`, no
tip_finalize anchor row, `coins_applied_height` not advanced to H+1,
`reducer_frontier_compute_hstar` still returns the compiled anchor (H* not
floated); clean control (no `ok=0` row) → no-op (seeding proceeds, H* reaches H,
no blocker, no event).

---

## Sequencing — copy-prove before any live deploy

lint: E13 consensus-parity unaffected — this touches no consensus predicate, only
the bless decision and the stamp condition.

- **Copy-prove FIRES:** copy `~/.zclassic-c23-cointear-fixture-20260612` to a
  scratch datadir, boot HEAD against the **copy** (never live, per the recovery
  doctrine), confirm the gate fires at bless time (typed `seed.torn_import` blocker
  + `EV_OPERATOR_NEEDED`, no anchor stamp, H* pinned at the checkpoint). Capture the
  operator message.
- **Copy-prove NO false-reject:** on a fresh probe-clean two-step cold import (the
  path that connects through 3,145,830), confirm the gate is a **no-op** and the
  seed blesses to H*. Both copy-proves must hold before any owner-gated live deploy.
- **(Owner-gated, deferred) Part B2:** only if the owner wants to close the
  unspent-forever gap — design the higher-checkpoint shipping ceremony with a
  same-binary cross-node SHA3-equality proof first.

---

## Residual / open

1. **Part B1 interaction.** Demoting the stamp leaves `gate_commitment_check`
   vacuously-true (returns true when no stamp) at non-checkpoint heights — grep
   `utxo_commitment_sha3_load` callers to confirm no path treats *presence* of a
   stamp as a hard trust signal that would now be absent.
2. **Seed-path coverage.** Should Part A also run on the snapshot/FlyClient seed
   path, not just the cold-import LDB path? The buried-tear class can arise from any
   trusted base seeded over a log-less region — confirm whether `snapshot_apply.c`
   routes through the same `block_index_loader` bless consumer.
3. **Part B2 cadence (owner-gated).** If shipping higher checkpoints: how often must
   `(height, sha3, count)` rows be burned/signed, and is the cross-binary SHA3 over
   `(txid,vout,value,script,height,is_coinbase)` byte-stable across a fresh importer
   vs the owner's reference node? Prove with a same-binary cross-node SHA3-equality
   test before any exact-match gate.

---

## Rejected alternatives (and why)

- **Recent-window coinbase-presence gate** — misses the live tear (depth 109 > maturity
  100 sits in the band it downgrades to non-paging) and false-rejects legitimately-spent
  coinbases if widened. Per-height presence is too coarse; the failure is per-outpoint.
  155 coinbase heights exist live, so a count gate never fires.
- **Bounded-window spend-resolution reading source bodies** — self-contradictory: the
  in-window body re-creates the missing coinbase in the created-set and masks the tear;
  no fixed K brackets all depths; duplicates the existing `coin_backfill` body-reader;
  has no bodies at fresh-import time (the failure mode it claims to gate).
- **Incremental rolling commitment from the checkpoint** — there is no per-block connect
  to amortize onto (the fast path seeds cursors over a log-less region), so it reduces to
  a full replay; the XOR accumulator is order-blind and false-passes a same-value swap;
  reindex already sets `g_utxo_commitment_skip` and recomputes O(n), proving the project
  judged the carry not worth maintaining.
- **Tip-only checks** (`active_chain_contains`, "tip coinbase resolves") — pass on the
  live tear because the tip is canonical while the buried coin 109 below is missing.

The honest framing salvaged from the last alternative — *write-time completeness for
buried coins is infeasible; the guarantee must be HONESTY + a clear action* — is the
foundation of the recommended design.
