# Self-healing reducer — epoch-aware rewind-to-clean-frontier + replay

Owner directive (2026-06-07): "build self-healing first" — make a torn datadir self-heal to
tip with NO manual rebuild, on top of the coins_kv inverse-delta. Status: **GO_WITH_GUARDS**.
Forensics ground truth lives in
`memory/project_tipfinalize_precondition_desync_fix_2026-06-07.md`.

## The disease (tear-mode taxonomy T1–T16; the live ones)
The reducer's per-height logs + cursors + the legacy block_index nStatus drift apart across
crash/rewind/replay **epochs**. The live tear is NOT a coin tear:
- **T2/T5** (the live blocker): reducer log truth is AHEAD of the served active-chain — 246/711
  sub-cursor window holes (missing BLOCK_HAVE_DATA) at 3134314.. ; healed by NOTHING today
  (forward-only cursors never revisit; chain_restore only sets HAVE_DATA where bytes exist;
  rebuild_recent loops because cursors are past the gap).
- **T3** block_index VALID_SCRIPTS drifted CLEAR while script_validate_log.ok=1 (Fix A heals this
  lazily one-block at the live frontier, never at boot, never sweeps a window).
- **T9** tip_finalize_log mixed epochs: [3134954]=ok1 but [3134955]=ok0; cursor rewound below its
  own log. **T10** anchor rows stamped above an ok=0 hole. **T8** cec int poisoned above the hash.
- Coins are CONSISTENT (forensics: 0 ok=1-heights missing an inverse-delta; complete+exact). So
  the live recovery is L1's domain (flag/cursor), NOT L2 (coins).

## The provably-consistent frontier H* (computed from durable state ONLY)
H* = the deepest height where ALL hold, computed from progress.kv + the coins store, and
NEVER from g_last_advance_height / cec int / served-tip / active_chain[] (Clause C7 — those are
the drifted authorities, checked AGAINST H*, never inputs to it):
- C1 cursors cover H*: every stage_cursor.cursor >= H*+1.
- C2 contiguous ok=1 prefix: every success log has a row, ok=1, for all [0..H*] (no hole, no ok=0).
- C3 single-epoch / hash-agree: **2-way at-height** check
  validate_headers_log[H].hash == script_validate_log[H].block_hash; the tip_finalize hash check is
  the LOOKAHEAD form folded into C4 (row H stores hash(H+1)); **exclude is_anchor rows** (they store
  hash(H)); **NULL hash = "no evidence", does NOT lower H*** (cold-import prefix is healable).
- C4 coins applied exactly to H*: coins_frontier = MIN(height(coins_best_block HASH), MAX(coins.height));
  tip_finalize_log[coins_frontier].tip_hash == coins_best_block (lookahead form).
- C5 block_index flags log-consistent over [0..H*]; a flag-only drift healable from the hash-bound
  log does NOT lower H*; a byteless hole (body_persist ok=1 but bytes absent, not under a trusted
  snapshot-anchor prefix) DOES.
- C6 H* = MIN(all caps) — a PREFIX boundary, never a high-water island above a hole.

### SPARSE LOGS / trusted anchor
The per-height stage logs are **SPARSE** on any cold-import/fast-sync node — the import populates
coins + block_index DIRECTLY and bypasses the stage logs (live: header_admit_log has rows at 0 + a
sparse middle + dense recent ~7000; huge gaps below). So "contiguous ok=1 prefix **from 0**" computes
H* ≈ genesis on every fast-synced node — fatally wrong. Correction:
- **trusted_anchor** = the commitment-verified base below which coins+block_index are trusted WITHOUT
  stage-log rows = the SHA3 UTXO checkpoint height (get_sha3_utxo_checkpoint()->height = 3,056,758),
  or a higher durably-stored verified coins commitment (the L2-prereq rolling commitment).
- C2 is computed as the contiguous ok=1 run **UPWARD from trusted_anchor**, NOT from 0. Heights at/below
  trusted_anchor are consistent-by-import (no log row required). A missing log row BELOW trusted_anchor
  is NOT a hole; a missing/ok=0 row ABOVE it caps H*.
- This makes trusted_anchor the FLOOR of H* (H* >= trusted_anchor always), unifying the "H* >=
  checkpoint" guard with the sparse-log base. The C3 hash walk + C2 contiguity only run on
  (trusted_anchor..upper]. L0 must take trusted_anchor as an input.
- **served_floor = MAX(height) FROM tip_finalize_log WHERE ok=1** — a SEPARATE companion.
  served_floor can be > H* (T9/T10). The public tip authority IS served_floor (chainstate.c:535).
- Hard guard: H* >= SHA3 UTXO checkpoint (3,056,758) — never rewind across irreversible finality.

## Layered plan (smallest-first, each independently provable)

**L0 — compute authority (no mutation, ships first).** `reducer_frontier_compute_hstar(progress_db,
coins) -> {hstar, served_floor}` in a new TU `app/jobs/src/reducer_frontier.c`, ONE SELECT-only read
txn under progress_store_tx_lock, applying the C3 predicate. Mandated regression test
`test_reducer_frontier_hstar`: over a multi-million-row CONSISTENT log set (lookahead finalize rows +
at-height vh/sv rows + leading NULL-hash cold-import prefix + trusted anchor) assert **H*==tip** (not
H*<<tip); plus torn T9/T10 fixtures assert
H*==prefix-end AND served_floor==MAX(ok=1)>H*; plus H* >= checkpoint guard.

**L1 — light self-heal (clears the LIVE wedge class; flag/cursor only; NEVER coins, NEVER deletes
tip_finalize_log).** Condition `reducer_frontier_reconcile_light`. detect() ~5s poll, fires iff a
flag/cursor sits above H* over (H*..served_floor] AND coins-applied <= H* (no coin tear) AND no
tip_finalize_log ok=1 row sits above the contiguous prefix; gate detect on tip_age AND peer_count
(benign 0-peer wait must not page). remedy() ONE BEGIN IMMEDIATE under progress_store_tx_lock.

Do NOT reset the utxo_apply cursor when coins are fine — the live case. The coin set is applied +
correct to the coins frontier; resetting the utxo_apply cursor below it would force a coin
RE-APPLICATION (transiently resurrecting spent coins / needless work) — violating L1's "coins
untouched" contract. CORRECT L1 for a flag/log-epoch drift with coins intact:
  (a) clamp ONLY the tip_finalize cursor down to H*+1 (the existing stage_reconcile_clamp does exactly
      this) so tip_finalize re-finalizes forward over the window;
  (b) SWEEP-heal block_index nStatus over (H*..served_floor/tip] from the hash-bound logs — the missing
      piece (Fix A heals ONE block lazily at the frontier; L1 sweeps the whole window at boot/condition
      time): set VALID_SCRIPTS where script_validate_log[h].ok=1 & block_hash matches, set HAVE_DATA
      where body_persist_log[h].ok=1 & bytes-on-disk, CLEAR HAVE_DATA where bytes absent (→ body_fetch
      re-requests), clear FAILED_MASK above H*;
  (c) the upstream validation cursors (header_admit..proof_validate) are reset ONLY if their logs have a
      genuine ok=0/hole above H* — a pure flag drift leaves them intact (their logs are ok=1).
  utxo_apply cursor + the coin set are NOT touched. Resetting utxo_apply (and the consequent coin
  rewind) belongs ONLY to L2 (a genuine coins-applied > H* tear). This is why L0 must distinguish a
  LOG/HASH defect (ok=0 row, hash-split — caps H*, may need upstream re-run) from a FLAG drift
  (block_index nStatus vs ok=1 log — healed by the sweep, does NOT need a coin rewind).
**NEVER deletes a tip_finalize_log row, NEVER touches coins.** Keep the
table_has_success_at_or_above(tip_finalize_log) refusal. witness() = durable tip (MAX(ok=1)) advanced
past the pre-heal H* (a quantity the heal can't move) → monotone, loop-free.

**L2 — deep coins rewind (ONLY a genuine coin tear; commitment-gated or refuse).** Condition
`reducer_frontier_reconcile_deep`, fires iff coins-applied > H*. remedy() FIRST verifies EVERY
utxo_apply_delta row in (H*,applied] present+non-malformed (per-height, closes GAP-B silent no-op);
if any missing → REFUSE + EV_OPERATOR_NEEDED (re-derive from clean source) — **NEVER** DELETE FROM
coins (can't restore spent coins). Else emit_inverse_delta applied..H*+1 in ONE BEGIN IMMEDIATE with
cursor reset; then recompute coins_kv_commitment over the post-rewind set and require exact hash+count
match vs a stored trusted commitment at H* before certifying; absent one → refuse. Does NOT route
through reorg_is_allowed (same-branch re-derivation, not a competing fork; finality-depth gate
untouched). Ships LAST, only after L2 prerequisite: stamp a stored trusted coins commitment at
finalized heights (utxo_commitment_sha3_save at tip_finalize).

**L3 — subtraction + unification.** Delete stage_reconcile_clamp_tip_finalize_to_floor + its
boot.c:3344 call (subsumed by L0+L1). Delete dead chain_restore_clear_failed_above_tip. Stop
chain_restore_finalize returning ZCL_ERR(-2) on RECONCILABLE (ends the chain_integrity_failed→
EV_OPERATOR_NEEDED churn). Make utxo_recovery cec-int write defer to the unified H* authority. ONE
frontier authority, boot + runtime.

## REJECTED (do not ship — the FATAL flaws)
- bulk `DELETE FROM coins WHERE height > H*` (removes created, never restores spent → double-spend).
- tip_finalize_log "prefix-preserving relaxation" deleting rows above H* (collapses served_floor T9/T10).
- raw 3-way hash_cap `!=` (lookahead convention → H* near genesis → rewinds healthy nodes).
- certifying forward progress on a rewound coin set without a commitment recompute-match.

## Durable-truth basis (the facts the resolution depends on)
- A stale ok=0 at height h WHERE coins for h exist = the LOG is wrong, not the chain — reconcile the
  log, don't rewind coins.
- tip_finalize SKIPS (marks upstream_failed + advances) on a stale ok=0 utxo_apply row (stage.c:343)
  — so a stale ok=0 makes a GOOD block permanently "failed", holing the finalized chain.
- Coins are the durable truth; logs/flags are drifted views. Above the last stored verified commitment
  (SHA3 checkpoint 3056758) coins are trusted-but-unverified (delta-internally-consistent, not
  reindex-proven).

## RESOLUTION (synthesis REJECTED as unsafe)
The "synthesize utxo_apply ok=1 from existing coins presence" idea is **FATALLY unsafe** and is REJECTED:
presence ≠ value-conservation. added_blob stores only txid|vout (no value → cannot recompute
total_value_delta/conservation); "prevout absent" cannot distinguish this-block-spent from
later-block-spent → a window holding a double-spendable coin finalizes before the scalar
utxo_count_diverged gate trips (the codebase itself: "a double-spendable set looks fine by row counts",
coins_view_sqlite.c:271). Coin trust REQUIRES a content-bearing SHA3 commitment recompute-match
(coins_reconcile_stale_anchor discipline), never a presence check. STEP A (re-validate script/proof from
the present body) is necessary but NOT sufficient (valid scripts can still double-spend).
Also the H* rule UNDER-rewinds: coins_frontier=MIN(coins_best_block HASH, MAX(coins.height)) is wrong —
MAX(coins.height) is the most-recent SURVIVING coin's creation height, NOT a contiguous applied frontier;
a missing-fsync interior drop is invisible to it. And progress.kv is WAL+synchronous=NORMAL (no per-commit
fsync); and the projection/event_log dual-write is STILL LIVE (Fix A flipped READS not WRITES).

### CORRECTED ORDER — the consolidation IS the self-heal's safety prerequisite:
**P1 (biggest beauty win + safety prereq, GATED) — DELETE the projection dual coin-write** (durability
step 4): utxo_apply stops emit_delta→event_log + utxo_projection catch_up (utxo_apply_stage.c:267,491-495);
coins_kv becomes the SOLE coin store. Collapses 3 stores→1. Copy-proof: SHA3 coins commitment EXACT match
vs oracle gettxoutsetinfo + kill-9×10.
**P2 — co-committed `coins_applied_height` cursor** written INSIDE the coins stage txn (single contiguous
frontier; replaces MAX(coins.height) everywhere). A contiguous cursor cannot hide an interior hole.
**P3 — rolling stored verified commitment** at finalized heights (utxo_commitment_sha3_save at tip_finalize)
— gives every future self-heal a recent self-trusted frontier to recompute-match against.
**P4 — set synchronous=FULL on the coins/progress.kv path** OR gate post-power-loss boot on an L2 recompute.
**Then** the self-heal: L1 may RECONCILE logs/flags TO coins + HOLD the served tip, but may NOT advance the
tip above the highest stored commitment without an L2 commitment recompute-match. L2 = deep coins rewind,
commitment-gated. The naive verdict-synthesis is gone; coin trust = commitment-match only.

## Copy-proof (on a COPY of the torn live datadir, never live)
(1) durable tip climbs past pre-heal H* to network tip; window holes fill via body_fetch re-request.
(2) served_floor NEVER drops (assert active_chain_height >= pre-heal served_floor every poll; no
tip_finalize_log row below served_floor deleted). (3) coins == clean reindex (byte-identical
commitment; for L1 the coin set is byte-identical throughout). (4) kill-9 mid-replay durable; same H*
recomputes deterministically. (5) H* >= 3,056,758 always. (6) healthy fast-synced node = NO-OP
(H*==tip). (7) 0-peer: heal does flag/cursor reset then HOLDS in REPAIRING without paging, resumes on
peer connect.
