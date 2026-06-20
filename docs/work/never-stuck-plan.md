# THE BIG PLAN — prove the foundation, fold forward, delete the lie-cover

Status: PLAN (owner-review). Verified 2026-06-18 by a 12-agent pass (4 deep gating
questions + 7 lighter-model LOC census). Supersedes earlier framings.

## 0. 2026-06-19 LIVE RE-VERIFICATION (4 workflows, cross-checked by hand)

Ran against the RUNNING wedged node (`wk80speni` root-cause, `wyrtafl1h` never-stuck
sweep / 24 agents, `wzfdxe34f` repair-vs-reality, + a breadth agent), every
load-bearing claim re-queried by hand. Net: the thesis HOLDS; three refinements.

- **CONFIRMED — two facets, the engine is honest.** Reducer H\* = **3,056,759**
  (node.log `L1 refused: coins_applied_height=3151412 > hstar_cursor=3056759`,
  1,186+ reps): it refuses the coin-tear at the rowless gap. The SERVED tip_finalize
  cursor = 3,151,411 is the stamp. `utxo_apply_log` real ok=1 rows exist ONLY in
  [3,150,900..3,151,411]; the gap [3,056,759..3,150,899] (~94k) is ROWLESS. So the
  surface stall (tip_finalize JOB_IDLE, 32,614 idle ticks, finalized_total=0) sits
  ATOP the deep lie.
- **REFINED — B7 is NOT an independent second root; it is the borrowed foundation at
  the working face.** The 4 false-rejects (3151412/621/965/3152130, all
  `prevout_unresolved`) are SYMPTOMS, not a genuine forward false-reject. Two sub-
  flavors, both traced: (A) **wrong-fork import** — at h=3,151,306 the committed
  block D7F76F30's coinbase 7E7894BF (in node.db tx_outputs, NEVER spent, what 3151412
  spends) is ABSENT from coins/utxos/snapshot, which hold the stale-fork sibling
  E1BF3809's coinbase 02663FF1. The breadth agent bounded this at **EXACTLY 1
  wrong-fork coin** in the whole UTXO set (two methods). (B) **above-tip** —
  3151965/3152130 spend coins born 3151826/3152013, absent everywhere because the
  fold is stuck behind (A). The repair family ENGAGES (refused_coin_tear=TRUE; coin-
  backfill works the hole) but TERMINALLY REFUSES (`coin_backfill.refused.3151412…=
  txindex_miss`) because node.db txindex tops at 3,151,411 — so it pages, never
  remedies. ⇒ B7's "build a sound ok=0 re-derive path" is real, but the cure for the
  LIVE wedge is still B2 (fold the gap), which dissolves all four.
- **ADDED — never-stuck hardening map (8 confirmed + 1 critic-found), the durable
  artifact for future agents.** P0: (1) `script_validate_stage.c:519-526` advance-
  past `prevout_unresolved` ok=0 [LIVE root]; (2) `tip_finalize_stage.c:425-431`
  deadlock on a not_script_valid successor [LIVE]; (3) `tip_finalize_anchor.c:214-221`
  stamps `reducer_trusted_base_height` over the 94k logless region, and the guard
  `reducer_anchor_candidate_ok` (reducer_frontier.c:242-269) only checks the SINGLE
  row at H+1 — the guard itself is too shallow; (4) **NEW/most-reachable**
  `proof_validate_stage.c:397-423` has the SAME advance-past-failure bug but its
  trigger `params_not_loaded` is a guaranteed async-boot transient → can manufacture a
  permanent false-reject on ANY cold/fast boot, not just this datadir. P1: (5)
  `block_index_loader_rebuild.c:500-532,723-729` stamps coins_applied_height=H+1 on a
  borrowed count-check. P2: (6) `chain_tip_watchdog.c:270-276` burns its full 3-restart
  budget on a deterministic stall (cause-blind, age-only escalation) — already
  saturated live, now inert/paging-only, NO destructive path; (7) **`validation_pack`
  window_sweep HOLD** (`chain_linkage_check.c:262-279`) latched at refuse_from=3,056,759
  — a SECOND tip-deadlock layer that **survives any cure of the script reject**; the
  cure MUST also fill the fold-gap to release it; (8) `mirror_divergence` hold
  (halt_on_recoverable) — not armed today (zclassicd unreachable), the B8 risk; (9)
  `utxo_apply_delta_reorg.c:382-429` JOB_FATAL spin on a >FINALITY_DEPTH reorg.
- **SAFE vs GATED.** The ONLY autonomous-safe item is (6) — give the watchdog a cause
  probe so it skips restarts on a deterministic stage disagreement (pure operational
  resilience, no consensus surface). Everything else — making transient misses
  retryable (1/4), tightening the anchor guard (3), and the fold-the-gap verb (B2) —
  touches a validation verdict's TIMING or the import authorities, so per the h=478544
  doctrine it is consensus-ADJACENT: owner go + copy-prove on the frozen wedge fixture
  + full-history replay before live. A 1-coin targeted backfill (the bounded wrong-fork
  coin) is a viable LIVENESS band-aid but leaves the deep lie (H\*=3,056,759) intact —
  not the cure.

Full evidence + every file:line is in the north-star memory
`project_frontier_wedge_real_rootcause_2026-06-18` (re-corrected 2026-06-19) and the
carve-manifest's live-verified correction header.

## 0b. CURE RESOLVED 2026-06-19 (workflow `wl5j9piiu`, adversarially verified — `partial`)

The one open question — *are the coins at the checkpoint or stamped forward?* — is
RESOLVED by on-disk evidence: **`coins_verdict = STAMPED_FORWARD_3151412`.** The
coins table is at the contaminated TIP, not the checkpoint, so **subtraction-only is
INSUFFICIENT.** The cure is a one-shot **forward bodies-only REFOLD**:

1. Re-materialize the checkpoint UTXO set at H=3,056,758 by folding LOCAL block
   bodies (no cached 1,354,771-row table exists — it must be COMPUTED) and HARD-ASSERT
   `utxo_count==1,354,771` + SHA3 root vs `checkpoints.c:86-104`.
2. Clear coins/created_outputs/nullifiers + forward `*_log` rows above the checkpoint.
3. Fold the 94,653 committed bodies 3,056,759..tip with real script+proof validation,
   re-deriving every coin from the committed block. Fixes fork-divergence AND above-tip
   by construction. (Rewind-from-tip is UNSAFE — the tip set is already contaminated,
   can't reproduce the SHA3 root.)

**TWO load-bearing gates the skeptic caught (do not skip):**
- **Read BLOCK BODIES, never the mirror.** The refold MUST source coins from committed
  block bodies (`blocks.file_num/data_pos`), NEVER node.db `utxos`/`tx_outputs` — that
  mirror is ITSELF wrong-fork (02663FF1 present / 7E7894BF absent at h=3,151,306,
  verified). The existing `-reindex-chainstate` epilogue (`reindex_epilogue.c:95-101`)
  reseeds coins from that mirror → a stock reindex would **reproduce** the divergence.
- **Guard the seed stamp, don't delete it.** The seed_exempt stamp
  (`stage_anchor.c:153-154` + `block_index_loader_rebuild.c:728/743`) lies ONLY for the
  borrowed-snapshot import; `reindex_epilogue.c:120-148` LEGITIMATELY uses the same
  stamp to finalize a TRUSTWORTHY replay (delete globally → reindex finalizer breaks,
  H\* never climbs). Gate it on provenance.

**Fold cost — measured-but-thin:** forward leg ~5–25 min on the Ryzen 9 7950X3D
(sparse: ~103k tx, ~109k ECDSA, ~11k Groth16, ~89k coinbase-only) but extrapolated
from a single 1-second 28-blk/s burst → confirm empirically. SEPARATE + UNCOSTED: the
genesis→checkpoint re-materialization (~3.06M-block fold, ~32× the leg) if no
known-good earlier UTXO state seeds it; if the recompute ≠ 1,354,771/SHA3 root the
premise is WRONG → fall back to a fresh trustless cold-import.

**Copy-prove procedure (all on a COPY; never live, never touch `~/.zclassic`):**
0. `cp -a /home/rhett/.zclassic-c23 …-refold-fixture-COPY` (ext fs has no reflink; ~6.8G).
1. Pre-state fingerprint FROM THE COPY (it is a moving target while live runs): coins
   count, MAX(height), coins_applied_height, and the wrong-fork witnesses (02663FF1
   present / 7E7894BF absent at h=3,151,306).
2. BUILD the refold binary (does not exist yet): guarded seed + the one-shot refold
   driver above.
3. RUN against the COPY datadir.
4. **DECISIVE GATE:** tip advances 3,151,411→3,151,412 with a REAL `utxo_apply_log` ok=1
   row at 3,151,412, and `reducer_frontier_compute_hstar >= 3,151,412` (no rowless gap).
5. **CORRECTNESS GATE:** coins now contains 7E7894BF and NOT 02663FF1 at h=3,151,306;
   re-assert the SHA3 checkpoint.
6. **PARITY GATE:** zero ok=0 across the band; tip hash-identical to zclassicd at the
   same heights — *currently UNRUNNABLE* (zclassicd reindexing, RPC "Loading block
   index", mirror rpc-unreachable). Wait for it before authorizing.
7. Present go/no-go to the owner. **Owner decides the LIVE deploy** (rewrites consensus
   state, irreversible) — copy-prove is autonomous; the live cutover is not.

**go/no-go RIGHT NOW = NO** for an autonomous run: the refold binary does not exist,
and the decisive parity gate can't run while zclassicd reindexes. Autonomous-safe NOW =
read-only COPY + pre-state fingerprint only. Residual risks: checkpoint
re-materialization cost/correctness; node.db is the trust root for "committed" bodies
(cross-checked only by zclassicd parity, currently down); the seed-guard change touches
the in-tree reindex finalizer and needs a fresh-cold-import regression test.

## 0c. 2026-06-20 GROUNDED RE-VERIFICATION (workflow `wf_b3a254cd-696`, 4 lenses + adversarial verify)

Re-ran the cure against the tree after a host OOM KILLed the `user@1000` systemd manager
(2026-06-19 17:02 — node + zclassicd both died; rebuilt `build/bin/zclassic23` @ ec06586a1,
restored under linger; the node booted back to the SAME pre-existing wedge at 3,151,411,
honest `operator_needed`). The thesis HOLDS; record these NEW verified corrections so no
future agent re-derives them or trusts the stale roadmap text:

- **The cure does NOT need LB-1 (the parallel verify engine).** VERIFIED: `verify_queue`/
  `thread_pool` have ZERO non-test callers; the from-anchor refold drives the EXISTING serial
  pipeline (`reducer_drain_to_convergence`, `reducer_ingest_service.c:79`). The pool is a
  throughput nicety (~94k bodies in minutes vs ~8h) and a genuine prerequisite ONLY for the
  full-genesis self-mint (LB-2). So B2 (fold the gap) is INDEPENDENT of parallelism — never
  block it on LB-1.

- **`refold_driver_main` is VAPOR.** `architecture-execution-plan.md` item 1.5 cites
  `refold_driver_main` at `reducer_ingest_service.c:142` as if present; grep is EMPTY across
  the committed tree (it lives only in an uncommitted stash). The real drain entry is
  `reducer_drain_to_convergence` (`reducer_ingest_service.c:79`, called from
  `chain_activation_service.c:447`). Any step that "promotes `refold_driver_main`" references
  nothing — the from-anchor driver must be BUILT (this IS B2/B5). *Fix the execution-plan line.*

- **FIX-1 "wire H\* to served APIs" is DONE** (shipped this session, commit e75b5c62c).
  `getblockcount` serves `reducer_frontier_provable_tip_cached` (`blockchain_controller_blocks.c:45`)
  AND P2P `start_height` serves H\* (`msg_version.c:155`, with the comment that only the OUTWARD
  claim switches to H\* while internal window readers stay on `active_chain_height`). The
  roadmap's "last unwired surface" is closed — scope FIX-1 to zero. *Fix the roadmap/plan text.*

- **The `coins_kv` slice CANNOT be salvaged by `DELETE WHERE height > anchor`** — this sharpens
  §0b with the mechanism proof. VERIFIED: `coins_kv` stores `height` = coin CREATION height
  (`coins_kv.c:63-77`) and a spend is a row DELETE (`coins_kv.c:93-98`); `apply_coins_kv` spends
  prevouts by txid/vout regardless of creation height. So the contaminated above-anchor
  application already DELETED below-anchor coin rows that above-anchor blocks spent — a
  height-bounded DELETE removes above-anchor CREATIONS but cannot RESTORE those wrongly-spent
  below-anchor coins. Post-DELETE the slice is missing coins → its SHA3 ≠ the checkpoint root →
  any "re-use the slice after a hard assert" shortcut FATALs on the live wedge. The cure MUST
  fully reset (`coins_kv_reset_for_reseed`, `coins_kv.c:466`) + re-materialize the anchor set
  from a trusted source bounded to `height<=anchor` + hard-assert `SHA3==checkpoints.c root` &
  `count==1,354,771`. (Exactly §0b's "re-materialize / it must be COMPUTED", now with the proof.)

- **Ordering knot (acknowledge it):** re-materializing the anchor has NO in-tree trustless
  source today — `utxo_snapshot_loader.c` has zero production callers; the snapshot WRITER
  (`--gen-utxo-snapshot`) is not in a production main. So the only rebuild source RIGHT NOW is a
  re-copy of zclassicd's chainstate BOUNDED to `height<=anchor` — the very D3/D4 copy path the
  plan deletes. Clean sequence: bound the cold-import copy to `<=anchor` (keeps a trustworthy
  anchor source) → fold bodies above (B2) → LATER replace even the anchor copy with a
  self-minted / snapshot-loaded anchor (LB-2). Do NOT delete the anchor-copy source before the
  self-mint or snapshot-loader source exists.

- **The duplicate mirror does NOT fight the refold today** (corrects Residual #4 framing):
  `utxo_mirror_sync_service.c:319-320` ALREADY guards `if (refold_in_progress()) return 0;`,
  skipping its 5s full DELETE+reinsert during a refold — provided the from-anchor verb sets
  `refold_in_progress`. Deleting the mirror is still a valid North-Star simplification (D2), but
  it is NOT a refold-time correctness hazard; the KEEP-pending-proof stance in Residual #4 stands.

- **Liveness vs structural-impossibility are DISTINCT** (label honestly): the from-anchor refold
  verb (B2) delivers LIVENESS — bounded auto-repair instead of the permanent `operator_needed`
  latch. It does NOT by itself make the tear class impossible; only deleting the unbounded
  above-anchor `INSERT...SELECT FROM coinssrc.utxos` (`coins_kv_boot_rebuild.c:158-163`, B3)
  makes it structurally impossible. Both are needed; B2 gates B3.

Net: no change to the build order (B1..B8) or the deletions table — this section corrects the
execution-plan's two stale claims (`refold_driver_main`, FIX-1), records the `coins_kv`-reset
mechanism, and names the anchor-source ordering knot so the B2/B3 implementer never ships the
unsound DELETE-and-reuse shortcut.

## 0d. 2026-06-20 COPY-PROVE RESULT — B2 hard-assert PROVEN; the anchor SOURCE is the gap

Built `-refold-from-anchor` (branch `wf/b2-from-anchor-refold`) and ran it against a 12 G working
copy of the frozen fixture (isolated ports, `-connect=127.0.0.1:1 -nolegacyimport -nobgvalidation`).
Decisive result:

```
FATAL: -refold-from-anchor: re-seeded anchor set FAILED the SHA3/count check
(count=1344574 want=1354771, commitment_match=0, reseed_ok=1) — refusing to fold from an unproven anchor
```

Two proven conclusions:
1. **The B2 hard-assert WORKS.** It computed `coins_kv_commitment` over the re-seeded set, compared to
   the compiled checkpoint (count 1,354,771 + SHA3 root), found a mismatch, and FATAL'd — never folded
   from an unproven base. The "fail loud, name the exact gap" property holds.
2. **Re-seeding from node.db `utxos` CANNOT reconstruct the anchor set.** It yields 1,344,574 coins
   (the contaminated TIP state), not the anchor's 1,354,771. Above-anchor spends already row-DELETEd
   below-anchor coins, so the mirror is the tip, not the anchor. **There is NO in-tree trustless source
   that re-materializes the anchor UTXO set on a contaminated datadir** (the Plan agent's central
   unknown, now proven).

**Corollary (corrects §0c + the roadmap): B2 is NOT fully independent of the mint.** The fold-FORWARD
leg (anchor+1..tip, ~94k blocks) is serial-OK / LB-1-independent — but the anchor-SET SOURCE must be a
VERIFIED anchor UTXO set, which only two things produce: (a) a stored SHA3-committed snapshot AT the
anchor (a minted artifact; `utxo_snapshot_loader.c` exists but has no production writer/source), or
(b) a fold from GENESIS to the anchor (LB-2's core mint, ~3.06M blocks — where LB-1's parallel verify
actually pays off). Corrected build order: **mint the anchor set once (fold genesis→anchor, full
validation, store a SHA3-committed snapshot) → the from-anchor refold loads + asserts + folds-forward
from THAT.** Replace B2's `coins_kv_seed_from_node_db` source with the snapshot loader; the node.db
re-seed is a dead end on a contaminated datadir.

**Live-node recovery TODAY: unchanged** — only the two-step cold-import crutch produces an internally
consistent set (zclassicd's full chainstate at ITS tip). B2 cannot self-heal until the anchor mint exists.

## 1. Thesis

Wire the self-sufficient proof (`snapshot_verify`) into the cold-import door, kill
the row-count heuristic and the cursor-stamp lie so every stage cursor is *derived*
from a folded log instead of *installed*, then delete ~13,500–15,500 LOC of
repair / trust-brain / mirror accretion that exists only to cover for an unproven
foundation. Fast + never-lies + never-stuck become the *same* property — the node
only ever records facts it computed.

## 1b. The principle — fold the frozen part ONCE, then verify/locate/repair cheap

The mental model future agents must internalize (it makes the cure both fast AND
correct, and it is *why* replaying the chain is the lazy solve):

The chain is **immutable below `tip − ZCL_FINALITY_DEPTH` (=10)** —
`zcl_immutable_height()` (`sync_evidence_policy.c:11`); reorgs deeper than 10 are
refused `below_finality_depth` (IBD relaxes to 1000). Information theory on that: a
fact over the frozen part **never changes**, so compute its fingerprint **once** and
thereafter **verify by O(1) comparison, locate any divergence by O(log n) bisect,
repair only the bounded mutable window.** Work is proportional to the ~10-block tip
window + log(n) hash checks — **never the chain.** Replaying genesis→tip (or even
checkpoint→tip) to *check* or *fix* state is recomputing frozen facts: the slow solve.

**Keystone diagnosis (verified 2026-06-19, workflow `wf_b01bc4b0-504`) — the node
fingerprints the WRONG thing per height.** It commits to AND checks a per-height
**block hash** — `validate_headers_log.hash`, `script_validate_log.block_hash`, and
`reducer_frontier.c:422` (`apply_hash_agreement`) already bisects the first block-hash
split. But **no folded log stores a per-height UTXO-set fingerprint** — `utxo_apply_log`
holds only `spent/added_count` + `total_value_delta`; `tip_finalize_log` holds
`utxo_size_after` (a COUNT, not a hash). So a divergence that is **height-correct (block
hash agrees) but state-wrong (a wrong-fork coin)** is **invisible to every per-height
check** — which is *exactly* the live wedge (one wrong-fork coinbase at h=3,151,306,
block hash correct). You cannot bisect or bounded-repair a fault you never fingerprint.

**The pieces already exist — compose them, don't rebuild:**
- Finality window: `ZCL_FINALITY_DEPTH 10` (`main_constants.h:33`), `zcl_immutable_height` (`sync_evidence_policy.c:11`).
- ONE canonical (script-inclusive, zclassicd-verified) SHA3 UTXO fingerprint, single rung: `checkpoints.c:86` (h=3,056,758); the runtime stamp is a single `node_state 'utxo_sha3'` (INSERT OR REPLACE — at most one stored).
- The correctly-shaped per-100-block ladder is **DEFINED but DEAD**: the Commitment MMR, leaf `{height, block_hash, utxo_root, data_root}` (`mmr.h:120,122-126`), appended by `rpc_blockchain_maybe_commit` (`blockchain_controller.c:243`) — which has **zero callers on the connect path** (`tip_finalize_post_step.c:184-197` appends only the block-hash MMR + the MMB header leaf), so the ladder is never populated. Its `utxo_root` is the **XOR accumulator** (`blockchain_controller.c:266`), which omits scriptPubKey → **not byte-comparable** to the canonical SHA3.
- MMB/FlyClient is a **header/PoW ladder only** — leaf binds `{block_hash,height,nBits,sapling_root,chain_work}`, **no utxo_root** (`mmb.h:41-48`). This is B1's keystone gap.
- A working **binary-search locator** already exists (`mirror_divergence_locator.c:168-193`) — but it bisects against the **external zclassicd** over block hashes, not a self-committed ladder, not UTXO state.
- Bounded-window **repair** already clamps to `tip−10` (`utxo_apply_delta_reorg.c`).

**Composition (this is B1+B2+B5 stated as one loop):** on the connect path, fold each
finalized block once and record a per-height **canonical** UTXO fingerprint (revive the
Commitment MMR with the SHA3 root, bound to PoW per B1); point the bisect at the
**self-committed** ladder (not zclassicd); trigger the existing bounded-window repair.
Then verify/locate/repair are **self-contained and O(log n + window)** — fast,
never-lies, never-stuck, the same property.

## 2. Invariants (true by construction)

- **I1** No coin enters state except by `utxo_apply` folding a verified block, OR a
  snapshot whose UTXO root is bound to a PoW-proven chain. No row-count acceptance.
  *Lint* `check-no-rowcount-install`; *test* `test_cold_import_requires_proof`.
- **I2** No stage cursor stamped forward without a real per-stage log row. Delete the
  `seed_exempt` bypass. *Lint* `check-no-seed-exempt-stamp`; assert H*==MIN(real cursors).
- **I3** H* is derived, never installed — `reducer_frontier.c` stays the only authority,
  refuses + names the missing height. (Already true — protect it.)
- **I4** Logs append-only, keyed `(height, block_hash)`, readers pick the active-hash row.
  *Lint* forbid `INSERT OR REPLACE` on the 7 stage logs; `test_log_append_only_reorg`.
- **I5** Consensus parity inviolable — a canonical block is NEVER marked invalid; every
  predicate tightening replayed against real history first (h=478544 lesson). E13 + goldens.
- **I6** Node stands alone — zero runtime dependence on zclassicd. *Lint* `check-no-zclassicd-runtime`.

## 3. Step 0 — immediate live recovery (no new code, copy-prove first)

Live: tip pinned 3,151,411, `degraded_reason: utxo_apply log hole, first hole h=3056759`,
P2P healthy. The honest engine refusing the borrowed foundation. Recover with the proven
two-step on a COPY first (`cp -a`, `--importblockindex` then normal boot), verify
tip==network + healthy + hash-match vs zclassicd at ≥2 heights, then deploy to live.
`rm -f build/bin/zclassic23` + verify build_commit. Band-aid to unblock soak, NOT the cure.

## 4. The build (ordered, each copy-proven + replayed before live)

- **B1 — Bind the UTXO set to PoW (keystone, senior).** Q2 found the **fatal gap**
  (`mmb.h:10`): the MMB leaf is `block_hash||height||timestamp||nBits||sapling_root||
  chain_work` — **no UTXO commitment**, and `chain_work`/`nBits` are attacker-chosen.
  SHA3 only proves bytes hash to the root the *peer claimed*. Add a UTXO commitment to the
  proof + reconcile sampled work against the offered chain_work. Pure-additive (no validity
  change) but consensus-sensitive + HIGH risk → own ADR + adversarial negative test (forged
  set with matching self-root must REJECT). **Gates everything.**
- **B2 — Wire `snapsync_verify_flyclient` into the cold-import door** (today reachable only
  from `msgprocessor_snapshot.c:1093`). Precondition: B1 landed.
- **B3 — Delete the row-count heuristic (`utxo_recovery_restore.c:323`) + the `seed_exempt`
  stamp (`stage_anchor.c:257`).** The wound itself. After B2 the only seed is a proven snapshot.
- **B4 — Logs truly append-only / reorg-correct** — re-key the 7 stage logs to
  `(height, block_hash)`, readers select active-hash. Schema change touching consensus reads
  → replay real reorg history first. MEDIUM-HIGH risk.
- **B5 — Confirm fold is cheap** — still UNMEASURED, and **cannot be measured with any
  existing verb** (verified 2026-06-18, workflow `wz32ptvlr`). `rebuild_recent 3056758` is
  invalid on two grounds: it caps at 10,000 blocks (`REBUILD_RECENT_MAX_RANGE`) and fetches
  every block from zclassicd RPC. `-reindex-chainstate` replays from GENESIS and self-refuses
  on this cold-import datadir (no genesis-side bodies). `poison_rewind` is frontier-only +
  ok=1-floor-guarded, so it can't target the checkpoint. The forward fold engine over on-disk
  bodies EXISTS (`body_fetch`→`utxo_apply`, network-free) but there is **no operator verb to
  rewind utxo_apply to a chosen height and re-drain forward**. So B5 *is* B2: the measurement
  and the fix are the same work — once the snapshot-seed-then-fold trigger lands, it measures
  itself. Don't assume "coinbase-only / <1 min": `script_validate_log` + `proof_validate_log`
  have ZERO gap rows, so the fold re-runs full script+proof validation for ~94k real blocks
  (e.g. block 3,151,412 carries a tx with vin=17). Parallelize `script_validate` only if the
  first real measurement demands.
- **B6 — Honest cold-sync = verify snapshot (B1+B2) → apply at proven height → fold to tip (B5).**
  No zclassicd anywhere. Must beat MVP C3 (<10 min).
- **B7 — Honest recovery from a FORWARD false-reject (the SECOND wedge root).** Verified
  2026-06-18 (workflow `w78oa8h3o`): the wedge is NOT single-rooted. One structural pin (H*
  honestly refusing a non-contiguous log) has TWO independent causes — the borrowed-import
  hole (B1–B3) AND a genuine forward false-reject: `script_validate`/`proof_validate` write an
  `ok=0` row and STILL advance their cursor, pinning H* with **no in-stage auto-recheck** (grep
  for recheck/quorum in those stages = empty). The ONLY things that clear an `ok=0` today are
  the operator `reconsiderblock` path and the very `stage_repair` ladder slated for deletion.
  Build a sound, non-lie-cover way for an `ok=0` to be re-derived and (on genuine validity)
  cleared — e.g. re-run the predicate at a parity-checked height with a peer-quorum witness.
  **Gates the D5 repair-ladder deletion** (deleting it without B7 removes the only escape from
  a forward false-reject). Pairs with the two known latent parity holes (the all-zeros-only
  `hashFinalSaplingRoot` reject `connect_block.c:638`; the h=478544 tx-size lesson).
- **B8 — Demote the `oracle_policy` halt path to evidence-only (a never-stuck risk).**
  Traced to ground 2026-06-18: it IS production-wired, not just telemetry. Feeders
  `oracle_policy_record_disagreement()` fire from the four zclassicd comparators
  (`quorum_oracle_service.c:334`, `mirror_divergence_locator.c:282`,
  `legacy_mirror_sync_service.c:274/303`, `zclassicd_oracle_service.c:203`); enough distinct
  disagreement heights → `OP_HALTED` (`oracle_policy.c:168`) → `oracle_policy_chain_extension_
  allowed()` returns false (`:205`) → consumed at `rolling_anchor_service.c:403,433`. So a
  zclassicd that is merely WRONG/behind can stop the rolling-anchor extension — a comparator
  gating liveness, against the "stands alone / cannot get stuck" property. (One open trace: the
  exact blast radius — whole-chain advance vs only the rolling-anchor service — confirm before
  the cut.) Fix = record divergence as evidence, never gate extension on it. Low risk, high
  principle. NOTE: this episode is itself the doctrine — the deep-read synth asserted this as
  "verified", a first skeptical grep made it look like dead telemetry, and only tracing the
  `_allowed()` wrapper to its `rolling_anchor` callers settled it. Trust neither the story nor
  the first grep; trace to ground.

## 5. The deletions (LAST, each gated on its replacement)

Net **~13,500–15,500 LOC** removed, zero features lost. Bottom-up:

| Order | Cluster | LOC | Gated on | Preserve |
|------|---------|-----|----------|----------|
| D1 | coin_backfill | ~4,400 | B3 | torn-import verdict folded into a transition check |
| D2 | legacy_mirror + zclassicd_oracle + quorum/policy/divergence | ~4,600 | I6/B6 | `chain_linkage` fork-HOLD KEPT; oracle reads stubbed |
| D3 | utxo_parity vs zclassicd + parity_poll | ~1,300 | I6 | C8 parity canary re-sourced to peer quorum |
| D4 | cold-import LDB cluster | ~3,500 | B2+B3 | KEEP frontier_gate, projection_topup, importblockindex header read, snapshot_apply, reindex_epilogue |
| D5 | reducer_frontier reconcile ladder | ~3,700 | B3+B4 | KEEP rewind (reorg), body_fetch, header_solution, boot clamp |
| D6 | stage_anchor seed_exempt + condition siblings | ~800 | B3,D2 | all reorg/disk/peer conditions preserved |
| D7 | purge verb | ~340 | **B4 ONLY** | reorg-residue now structural |

**Ordering rule:** D7 is LAST and strictly gated on B4 — it is currently the only thing
keeping the height-keyed logs reorg-correct.

## 6. Scale / many-eyes

Lighter-model fan-out: per-file deletions + include/registry/boot-spec edits, condition
de-registration, `INSERT OR REPLACE`→`INSERT` sweeps, the new grep lint gates.
Senior care: B1 (PoW binding), B4 (active-hash reader), B2/B3 (the flip), every
real-history replay, D2's fork-HOLD stub edits.

## 7. Honest residuals (measure before irreversible cuts)

1. **Fold wall-clock is estimated, not measured — AND not measurable today** (verified
   2026-06-18, workflow `wz32ptvlr`, cross-checked against `progress.kv`). No existing verb
   re-folds utxo_apply from the checkpoint over on-disk bodies (`rebuild_recent` caps at 10k +
   needs zclassicd; `-reindex-chainstate` is genesis-wide + self-refuses here; `poison_rewind`
   is frontier-only). The fold rate can only be measured *after* the B2 from-checkpoint fold
   trigger exists — so resolve this residual *inside* B2/B5, not before D1. Sharper truth: the
   gap 3,056,759..3,150,899 has ZERO rows in EVERY stage log (header_admit, validate_headers,
   body_fetch, body_persist, script_validate, proof_validate, utxo_apply, tip_finalize) — no
   stage folded it; cold import stamped cursors/`coins_applied_height` to 3,151,412. The
   concrete blocker is a real spend at h=3,151,412 (tx 4e9565…, vin=17) of a coin born in the
   unfolded gap → absent from the snapshot-seeded `coins_kv` → `coin_backfill` latched
   `REFUSED_SPENT` (durable, survives reboot). On-disk UNDO is also missing for the top ~14k
   blocks (present only checkpoint..~3,137,000), so a forward re-fold (bodies-only) is the
   viable path; rewind-from-undo is not.
2. **B1 is design work, not wiring** — where the UTXO commitment binds is unresolved +
   consensus-sensitive → ADR + adversarial review before code.
3. **Restart-durability after cold re-import** is a separate root cause; confirm whether B6
   inherently fixes it.
4. **`utxo_mirror_sync` (node.db utxos)** — KEEP pending proof it's a pure rebuildable cache.
5. **`state_window_inconsistent`** — decide at D6, may be reusable for commitment-audit.
6. **B4 migration cost** on live 110 MB progress.kv / 8.8 GB node.db — measure on a copy.

## 8. First concrete step

(a) `cp -a ~/.zclassic-c23 ~/.zclassic-c23-step0-copy-…` and run the proven recovery to
unblock the live node (still valid as the band-aid). (b) ~~`rebuild_recent 3056758` to time
the fold~~ — **invalid; deleted** (verified 2026-06-18). There is no offline from-checkpoint
fold verb, so the fold can't be timed yet (see Residual #1). The real first build step is
therefore **B2**: wire a snapshot-seed-then-fold trigger that rewinds the reducer to the
checkpoint and re-drains forward over the on-disk bodies — that trigger *is* the missing
measurement instrument and the cure at once. Safe isolated copy-node boot recipe (verified,
ready for when B2 lands): `build/bin/zclassic23 -datadir=<COPY> -port=8073 -rpcport=18272
-fsport=18074 -httpsport=8473 -connect=127.0.0.1:1 -nolegacyimport -nobgvalidation`, plus
`rm -rf <COPY>/onion-keys <COPY>/ssl` (no flag disables Tor/clearnet; deleting the keys does).
