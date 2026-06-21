# Consensus-parity audit, round 2 (2026-06-21)

> Red-team of zcl23 consensus predicates vs zclassicd C++ (`/home/rhett/github/zclassic/src`).
> 10 agents, spot-checked (read 30-50 lines around each claim), classified. Full raw
> output: workflow `parity-audit-round2` (run `wf_359bdfe4-70f`).
>
> **A LOOSENING = zcl23 accepts an input zclassicd rejects = a silent fork onto a dead
> chain.** This is the dangerous class for a daily-driver node. A TIGHTENING (zcl23
> rejects what zclassicd accepts) is also a fork but the opposite direction.
>
> **Tightening a bounded predicate is REPLAY-GATED** — full real-history replay first
> (the h=478544 / 125,811-byte-tx lesson). Lock-in tests that pin CURRENT behavior are
> safe to land now (they make the gap visible; they are NOT consensus changes).

## Confirmed loosenings (zcl23 accepts what zclassicd rejects)

| # | Predicate | zcl23 site | Class | Action |
|---|-----------|------------|-------|--------|
| L1 | **No 2 MB serialized block-size cap** — only a tx-COUNT cap (`num_vtx <= 2000000`), never `GetSerializeSize(block) > 2000000` | `domain/consensus/src/check_block.c:131-139` | real fork vector, **replay-gated** | The `[2,000,000 .. 2,097,152]` band is P2P-reachable (`MAX_PROTOCOL_MESSAGE_LENGTH` > consensus cap); disk-import/mine paths have NO upper byte bound. History-safe (real blocks <200 KB). zclassicd `main.cpp:4317`. Add byte-size clause after a full-history replay confirms 0 real blocks in the band with tx-count ≤ cap. |
| L2 | **`ContextualCheckBlockHeader` skipped for any block >1000 below tip** (`tip_h>100000 && prev<tip_h-1000` → skip) | `process_block_contextual_header.c:74-76` + `accept_block_header.c:386-392` | real fork vector, **replay-gated** | A contextually-invalid header (bad-diffbits / bad-equihash-solution-size / time-too-old / bad-version / bad-fork-at-checkpoint) below tip-1000 stays `BLOCK_VALID_TREE` and is never consensus-gated on the synchronous path. bg_validation re-checks but only sets `BG_VALIDATION_FAILED` (no `BLOCK_FAILED`, no rollback) and is opt-out. zclassicd runs it unconditionally in `AcceptBlockHeader` AND re-runs in `ConnectTip` (`main.cpp:4472`, `:3373-3388`). Removing the skip is replay-gated (`GetNextWorkRequired` on an incomplete window could spuriously trip bad-diffbits). |
| L3 | **Checkpoint fork-guard is exact-height-only** — no "reject any block below the last checkpoint height" | `domain/consensus/src/checkpoints.c:53-64` | real fork vector, **replay-gated** | zcl23 returns OK if no checkpoint exists at that exact height; zclassicd rejects ANY block with `nHeight < lastCheckpoint.nHeight` (`main.cpp:4386-4392`). Compounded by L2 (this lives inside the skipped contextual check). Tightening is replay-gated (confirm no legitimately-stored side-branch index entry below the last checkpoint). |
| L4 | **Oversize-tx grandfather allowlist** — 413 historical txs >102000 accepted in BLOCK context | `domain/consensus/src/tx_structural.c:108-133`; table `oversize_grandfather_table.inc` (413 entries, h478544..1968856, max 1.92 MB) | **reproduces LIVE zclassicd behavior** | NOT a real divergence: zclassicd `CheckBlock` uses the 2 MB `GENEROUS_BLOCK_SIZE_LIMIT` and never re-checks already-validated blocks, so running zclassicd nodes ALSO accept these 413 txs. The zcl23 predicate matches live behavior, not the lossy text. Mempool/STANDALONE path stays strict (102000). Keep; pin with a lock-in test. |
| L5 | **Mempool does not reject `tx-expiring-soon`** | `accept_to_mempool.c:84-181` (no `is_expiring_soon_tx` call) | mempool RELAY policy only, **NO fork risk**, NOT replay-gated | Worst case zcl23 relays/mines a near-expiry tx zclassicd would drop from its mempool. Block-consensus expiry parity HOLDS (`contextual_check_tx.c:69-74` `is_expired_tx` → `tx-overwinter-expired`). Closing is mempool-policy-only and parity-restoring; safe to add anytime. |
| L6 | **CHECKDATASIG sigops not counted in P2SH redeem by default** (`g_enforce_checkdatasig_sigops=false`) | `connect_block.c:126,352-361`; `script.c:115-119` | real fork vector, **replay-gated**, flag exists | zclassicd counts these toward `MAX_BLOCK_SIGOPS` by default (`main.cpp:2610`). zcl23 has the predicate behind an off-by-default atomic. **Already pinned** by `test_connect_block_checkdatasig_sigops.c` (flag-off accept / flag-on reject). Enabling by default is replay-gated. |
| L7 | **Shielded anchor-membership is a stub** — JoinSplit/Spend anchor "exists in chain history" rule absent | `lib/coins/src/coins_view.c:477-486` (`return true`), called per-tx at `connect_block.c:477` | **real fork vector, NOT replay-gated (parity-RESTORING)** | zclassicd `HaveShieldedRequirements` rejects a forged/never-existed anchor via `GetSproutAnchorAt`/`GetSaplingAnchorAt` (`coins.cpp:565-604`). zk-proof still binds the claimed root, so a valid proof against a forged root is needed — but the membership rule is entirely missing (`sapling_anchors` table is created, never read to reject). Closing is gated on **building a real anchor set** (ties into the fold-forward cure), not on a replay-permission. **Highest-priority real loosening.** |
| L8 | **Nullifier double-spend accepted below the `nullifier_kv` activation cursor** | `app/jobs/src/utxo_apply_nullifiers.c:50-75,146-187` | **bounded / honest-halt**, NOT replay-gated | A double-spend of a pre-activation note is accepted on a snapshot/cold-import datadir (the set below the cursor is empty). BUT it surfaces as the PERMANENT operator blocker `utxo_apply.nullifier_backfill_gap` (halts honestly, never serves divergent state). Closed for a from-genesis-replay node (`cursor==0`). The fold-forward cure (replay from a verified anchor) removes the gap by construction. |

## Confirmed tightenings (zcl23 rejects what zclassicd accepts — provably safe)

| # | Predicate | Why safe |
|---|-----------|----------|
| T1 | Per-tx parse-time count caps (65536 vin/vout, 4096 shielded/joinsplit) — `transaction.c:515-586` | Provably **unreachable** under the 102000-byte tx-size cap (max realizable vin ≈2488, vout ≈11333, shielded ≪4096). Memory-safety guard; no accept/reject divergence. Pin with an unreachability assertion. |
| T2 | `ContextualCheckTransaction` runs only within `CTX_TIP_WINDOW=16` of tip on the connect path — `check_block.c:444-453`, `script_validate_contextual.c:88-92` | Functionally parity-preserving: the IBD short-circuit matches zclassicd exactly (`main.cpp:951-958`); outside IBD/within window the predicates are identical and DO reject (`LOG_FAIL` → `return false`). Backstopped by bg_validation re-verify from genesis + `proof_validate_log` ok=1-prefix gating H*. The 16-block window is narrower than zclassicd's "run at contemporaneous tip" but a bad proof cannot advance H*. |

## Refuted (verified AT PARITY — good news, no action)

- CheckBlock merkle root + CVE-2012-2459 duplicate-tx (`merkle.c:229-259`) — byte-for-byte equivalent.
- CheckBlock coinbase placement (bad-cb-missing/multiple) + empty-block (`check_block.c:131-161`) — identical ordering/predicates.
- CheckBlock aggregate sigop cap (MAX_BLOCK_SIGOPS=20000, `check_block.c:168-196`) — identical counter.
- CheckBlockHeader/Contextual block version (MIN_BLOCK_VERSION=4) — at parity.
- (others in the raw output — all confirmations at parity.)

## Disposition

1. **Lock-in tests first (safe, no consensus change)** — pin CURRENT behavior for L1, L2, L3, L4, L5, L7 so each gap is visible and a future fix flips a test deliberately. L6, L8, T-cases already have (partial) pins. These freeze the accept/reject set; they are not tightenings.
2. **L7 (anchor membership) is the highest-priority real loosening** and is NOT replay-gated — it is parity-restoring once a real Sprout/Sapling anchor set is maintained. This couples to the fold-forward cure (a from-anchor replay rebuilds the note-commitment trees). Do it as part of the keystone, owner-gated + copy-proven.
3. **L1/L2/L3/L6 tightenings are replay-gated** — each needs a full real-history replay against the live chain BEFORE the reject is enabled (the h=478544 doctrine). Build the replay substrate first.
4. **L4 keep** (reproduces live zclassicd behavior). **L5 mempool-policy** (add anytime, no fork risk).
