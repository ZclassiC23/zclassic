# Tenacity Roadmap — sync super fast, fail almost never

**Date:** 2026-06-11 · **Status:** active · **Builds on:** [`canonical-frontier-derived-state-plan.md`](./canonical-frontier-derived-state-plan.md) (referenced below as "the canonical plan" — not duplicated here)

## The lens (anchor every item to it)

1. **State duplication = guaranteed divergence.** Redundant encodings of the same fact
   without one authoritative source + derivation discipline WILL diverge under partial
   writes; repair code that guesses between copies converts crashes into wedges. Cure is
   architectural: one source (the log), derived views, divergence unrepresentable.
2. **Verification must sample the real distribution.** The chain is the only ground truth
   for consensus (reference source text is a lossy proxy); live failure modes (boot
   restore, recovery, crash, import) need gates that exercise them, or green tests measure
   the wrong distribution. `test_parallel 0/409` stayed green through a 6-hour outage.
3. **Sync speed has an information floor:** headers + UTXO snapshot (trusted) or FlyClient
   sampling (trustless). We already measure at the floor (`--importblockindex` 3.14M
   headers in 60–74 s, then `-cold-import` → hash-identical tip in ~25 min). The remaining
   work is making the floor the *only* path and making "fast" never mean "silently
   incomplete".

**Standing rule (write into doctrine, enforce on every PR):** a new wedge gets a new
*write-time invariant at a chokepoint*, never a new repair module.

---

## Ordered items

### 1. Land Invariant A — frontier-bounded tip *(LANDED — `21d177bf9` + `447fa757b`, merged `a2da7e107`)*

- **Goal (met):** make Wedge A (restore installs a tip above the validated header
  frontier → post-restore integrity FATAL → crash-loop) *unwritable*. This is the boot
  crash-loop class that took the node down 6+ hours on 2026-06-11.
- **Mechanism as landed:** canonical plan steps 3–4 in two halves. Log half
  (`21d177bf9`): every restore-family install funnels through the single
  `utxo_recovery_commit_tip` chokepoint; evidence-based floor rewind replaces the
  anti-rewind refusal that turned a consistent rollback target (3137373) into a wedge.
  Index half (`447fa757b`, taught by the FIRST build FAILING on the fixture): the log
  authority alone is fabricatable — installs must also be trust-rooted (pprev descent
  to genesis/SHA3-anchor), settle-looped, and the boot promotion bypasses gated.
- **Acceptance (proven):** the COPY of the frozen fixture
  `~/.zclassic-c23-postrestore-wedge-20260611` (tip 3143175 > frontier 3141533, 1267
  holes, previously UNRECOVERABLE) boots post-restore-integrity CLEAN — no FATAL, zero
  reindex requests, serving (363 floor_rewind rows); build + lint + test_parallel 0/409
  green. Residue on that fixture: the coins_applied>hstar coin-tear class — separate,
  see item 3 / §4d row 1. Deploy stays owner-gated.

### 2. Land the oversize grandfather fix + re-prove full genesis replay *(LANDED — `ccc7fbbfa`, merged `b0c0b4f9a`; live deploy pending, see item 9)*

- **Goal (met):** the canonical chain must replay from genesis without a single
  consensus reject. FR-3's text-copied `MAX_TX_SIZE_AFTER_SAPLING=102000` false-rejected
  real history (mined when the rule was 2 MB; Sapling active 476969; zclassicd tightened
  later without grandfathering — the reference cannot resync its own chain). This
  FATALed auto-reindex and stalled the forward reducer.
- **Mechanism as landed:** a COMPLETE empirical scan (every height 0..3143532
  frame-walked + oracle-compared) found **413** oversize txs (h=478544..1968856, max
  1,922,197 B at h=685036); each is excused via a txid-keyed grandfather allowlist in
  `domain/consensus/src/tx_structural.c`, BLOCK context only — mempool and fresh blocks
  stay strict 102000 (= zclassicd live behavior). Chain-conformance, not parity
  divergence — the accept/reject set vs the *chain* is what parity means.
- **Acceptance (proven):** full genesis replay through the whole grandfather range,
  zero rejects. Still open: making that replay a *standing* nightly institution with
  commitment == zclassicd `gettxoutsetinfo` asserted every run — that is canary #5.

### 3. Reindex epilogue derivation — recovery must not manufacture the next wedge

- **Goal:** `-reindex-chainstate` (now the crash-only auto-recovery path, `706a7c00a`)
  currently ends in a torn state: with the never-give-up unit (`0b45e93a5`) this can
  degrade into an infinite reindex loop. Close it.
- **Mechanism:** code-read confirmed gaps in `config/src/boot_index.c:160–297` and
  `config/src/boot.c:3321–3344`:
  - `boot_index_clear_coins_state` **deletes** `coins_best_block` / `utxo_commitment` /
    `utxo_sha3` but the epilogue never **recomputes** them;
  - `coins_kv` (progress.kv) is never reseeded from the replayed set;
  - the `utxo_apply` / `coins_applied` cursors keep stale pre-reindex values — the
    recovery path itself manufactures the `coins_applied > hstar` wedge shape.
  Fix = the epilogue derives, in one ordered commit discipline: reseed coins_kv from the
  replayed set, recompute (not delete) the SHA3 commitment, clamp the reducer cursors to
  the replayed tip. Same lens as everything else: derived, never installed-stale.
- **Acceptance proof:** (a) regtest reindex smoke (seed ~50 blocks, `-reindex-chainstate`,
  assert tip + row-count parity + recomputed==served commitment + SERVING) — gates every
  push, ~3 min; (b) mainnet copy reindex ends with cursors == replayed tip and no
  `coins_applied > hstar`; (c) kill -9 mid-reindex then reboot converges (feeds item 7).
- **Size:** ~250 LOC + smoke harness; 1 worker session.

### 4. Seal + Window — `window_rebuild`, the one recovery verb *(DESIGN — upgrades the old plain ladder-deletion item; deletion stays the OUTCOME, window_rebuild is the MECHANISM that makes every deletion safe)*

**Status: PLANNED unless a line is marked IS.** Survey basis: main @`706a7c00a`,
2026-06-11. Consensus parity untouched throughout — `ZCL_FINALITY_DEPTH`, checkpoints,
and validity rules are *read*, never written. Doctrine home: `docs/TENACITY.md`.

**The doctrine (THE ZCLASSIC23 WAY).** The chain is the only immutable truth. Every byte
of derived state lives in exactly one of two domains. The **SEALED DOMAIN** sits at or
below the seal height S: state pinned by a checkpointed SHA3 UTXO commitment,
recomputable from genesis, continuously proven so by the standing replay institution
(item 5). The **WORKING WINDOW** is everything above S — cursors, stage logs, coin
deltas, verdicts, mirrors — explicitly **disposable**. One recovery verb replaces the
entire repair ladder: **`window_rebuild`** — discard the window, reset to the seal,
replay forward, recompute the commitment, verify, resume. **Recompute, never repair.**
Our own 2026-06-11 record is the evidence: every recovery that worked was a recompute
(cold-import, reindex, auto-reindex, reorg unwind); every recovery that failed or
re-wedged was a repair (anchor heals, backfills, reconcile guesses, oscillating poison
rewinds).

#### 4a. The seal

Dual boundary, sized from our own data:

| Boundary | Height | Status | Mechanism |
|---|---|---|---|
| **Soft margin F** (reorg floor) | tip − `ZCL_FINALITY_DEPTH` (10) | **IS** | bounded inverse-delta unwind, `utxo_apply_delta_reorg.c` (finality clamp ~:381–:421, range delete :218) |
| **Hard seal S** (state checkpoint) | latest *ratified* grid seal, nominal lag 1,000–2,100 blocks | **PLANNED** | new `seal` ring in progress.kv, hosted in rolling_anchor |

- Consensus already makes >10-deep reorgs unrepresentable
  (`main_constants.h:33-34,67`; `sync_evidence_policy.c:11-15`). Observed reorgs:
  8 unwinds today, all depth=3; kill-9 WAL rewinds 1–6 blocks. Seal lag = one
  1,000-block grid window + up-to-one of accumulation + 10-block ratification — two
  orders of magnitude of margin, replay bounded at ~0.5–7 s (measured 300–1,700 blk/s
  on the connect/reindex path; the ~1–3 blk/s projection catch-up path is forbidden
  for window replay).
- Seals land on the existing 1,000-block anchor grid (`SHA3_WINDOW_SIZE=1000`,
  `sha3_windows.h:22`), so input-bytes and state are pinned at the same heights:
  **rolling_anchor seals replay INPUT** (block-bytes SHA3 per window,
  `ra_compute_window_hash`, `rolling_anchor_service.c:312-360` — IS), **the seal ring
  seals replay OUTPUT** (state — PLANNED).
- **Floor:** with no runtime seal, S degenerates to the compiled checkpoint
  h=3,056,758 (`checkpoints.c:86-105`, mirrored by `REDUCER_FRONTIER_TRUSTED_ANCHOR`)
  and window_rebuild degenerates to today's auto-reindex — the design strictly
  generalizes what already works.
- **Seal record (PLANNED):** `seal(height, block_hash, coins_sha3, nullifier_sha3,
  utxo_count, supply, anchor_window_sha3, ratified, sealed_at, self_sha3)` in
  progress.kv (the co-commit theorem of `coins_kv.h:1-23` extends to it), kept as a
  ring of the last 4 so a corrupt latest seal steps back one window instead of paging.
- **Advance protocol (PLANNED):** *candidate at the frontier, ratify at depth.*
  (1) When `coins_applied_height` crosses a grid point G in steady-state tip-following
  (never during IBD/bulk replay), run the ~1 s coins_kv + nullifiers SHA3 scan at the
  flush boundary (`process_block_flush_policy.c:183`, the seam the
  coins-commitment-persist plan designed) and insert `ratified=0`, co-committed in the
  same progress.kv epoch. (2) The existing supervisor child `chain.rolling_anchor`
  (60 s tick, `rolling_anchor_service.c:52,:549-557`) ratifies iff
  G ≤ `zcl_immutable_height(tip)`, block-bytes prefix end ≥ G (export the
  `rolling_anchor_effective_prefix_end` getter, :482-488), and the active chain still
  contains `block_hash` at G. (3) On ratification, prune `utxo_apply_delta` ≤ G and
  stage-log rows ≤ G−RETENTION — seal advance IS the retention policy. A crash leaves
  no candidate or an unratified one; nothing to repair.

#### 4b. The window contract

**The rule (absolute):** no window state may ever be consulted as evidence for a repair
decision. Window state is OUTPUT, never INPUT. Admissible evidence = the seal ring,
sealed block bytes (compiled `sha3_windows` + runtime evidence file + compiled
checkpoint), the header tree at/below S, and consensus rules. Every rung that reads two
window artifacts and *guesses which is right* (coins vs cursors, log vs mirror, anchor
vs frontier) is structurally forbidden — a disagreement between two window artifacts is
resolved by discarding both.

Window inventory (everything above S; all IS, anchors survey-verified): the 8 stage
cursors + 8 stage logs in progress.kv; `coins`/`nullifiers` rows above S +
`utxo_apply_delta` inverses (the one window artifact the verb reads, and only to invert
it); `coins_applied_height`; `created_outputs_index`; misc `progress_meta` markers; the
node.db `utxos` mirror + `node_state` keys (`coins_best_block`, `utxo_commitment`,
`utxo_sha3`, tip pair, sapling_tree); the tip_finalize last-advance pair; block_map
verdict bits above S (the header *tree* below S is sealed content; *verdicts* above S
are window); `pindex_best_header` projection; dirty coins cache; projection-db residue;
the rolling-anchor runtime evidence file. NOT window: the rebuild/reindex sentinels,
the seal ring, blocks/ files, the read-only LevelDB import source.

#### 4c. The primitive — `window_rebuild`

- **Home:** `app/jobs/src/window_rebuild.c` (+ header) — a Job: the reducer resetting
  its own state via the stages' own storage contracts. Sentinel:
  `lib/storage/src/window_rebuild_sentinel.c`, byte-for-byte the `boot_auto_reindex.h`
  pattern (request/pending/clear, fsync-durable, `WINDOW_REBUILD_MAX=3`).
- **Crash-only execution:** a runtime trigger only writes the sentinel (typed reason +
  anchor + attempt) and requests a restart — the verb itself runs ONLY at boot, after
  block_index load and before any stage thread starts. Quiesce is free; a crash during
  rebuild looks like a fresh trigger (the sentinel clears only after post-rebuild
  verify); the attempt budget burns honestly across crashes.
- **Steps:** (1) log `window_rebuild begin reason=… attempt=N`; (2) select S = newest
  self-hash-valid *ratified* seal, else the compiled checkpoint; (3) discard the window
  in ONE progress.kv `BEGIN IMMEDIATE`: unwind coins/nullifiers C→S+1 via the existing
  `utxo_apply_delta_reorg.c` machinery with the fork-point generalized to S, delete the
  8 stage logs above S, reset all 8 cursors to S (one anchor row in tip_finalize_log,
  the trusted-seed stamp pattern), `coins_applied_height := S`; (4) recompute coins +
  nullifier SHA3 and verify vs the seal (+count/supply) — mismatch steps the ring back
  one seal and repeats; (5) reset derived mirrors FROM the just-verified seal values
  (closes item 3's delete-but-never-recompute gap; once canonical steps 5–6 land this
  collapses to "mark the demoted cache cold"), clear verdict bits above S; (6) replay
  forward S+1→target on the reindex connect path (`boot_index.c:183-305` grown a
  start-height parameter); a missing body above S just stops the replay — the rest is
  ordinary sync re-fetched by the stages after resume; (7) reseal at the highest grid
  point ≤ frontier, run the boot integrity gate, clear the sentinel, resume; (8)
  budget: 120 s wall (17× headroom over the 0.5–7 s expectation), 3 attempts ⇒ fall
  back exactly one rung to full auto-reindex from the compiled anchor (kept verbatim)
  ⇒ if that also exhausts, **page** (`EV_OPERATOR_NEEDED` + typed trail). The verb
  never reads or re-fetches below S — sealed-domain corruption pages, always.
- **Entry points:** runtime `window_rebuild_trigger(reason, anchor)` from every §4d
  site; boot direct-call from `boot_crashonly.c` classification and the coins-integrity
  gate; operator: one MCP tool `zcl_window_rebuild` (replaces the whole
  repair_controller surface) + `zcl_state subsystem=seal`.

#### 4d. The trigger table — every detector stays, every remedy becomes the verb

| # | Wedge class (detector — IS) | Today's remedy (dies) | New remedy |
|---|---|---|---|
| 1 | coin tear (Invariant-B detector, `c8018a388`) | frontier tear branch + `_coin.c` re-mint + reconcile_light | `window_rebuild(WR_COIN_TEAR)` |
| 2 | height splice / cursor oscillation / stale tip_finalize pair | poison_rewind + header_solution repair | splice branch (4f step 1) prevents at write time; residue ⇒ `WR_HEIGHT_SPLICE` |
| 3 | stale/holed verdict at frontier (the live h=3142977 class) | refill/purge/tipfin rungs, stale_validate_headers_repair | `WR_STALE_VERDICT` |
| 4 | torn coins anchor at boot (`boot.c:1707-1712` — the deletion canary) | L1 torn-anchor heal, then FATAL | `WR_TORN_ANCHOR` (pre-stage; direct call) |
| 5 | reorg residue / unwind failure / value_overflow hole | delta_repair one-shot + backfill scanners | inner ≤10-deep unwind kept; any failure ⇒ `WR_REORG_RESIDUE` |
| 6 | boot integrity mismatch (4 boot.c reconcile branches) | reconcile-and-guess | `WR_BOOT_INTEGRITY` |
| 7 | blocker escalation / tip_not_advancing exhausted | restart ladder → operator | one `WR_BLOCKER_ESCALATION` before paging |
| 8 | corrupt block frame at/below seal (the h=3115015 fixture stall — read failure breaks the extend loop and the 60 s tick retries forever, `rolling_anchor_service.c:429-436`; on supervisor stall it only warns, `LOG_WARN` + `EV_CHAIN_ADVANCE_DECISION` at `:490-502`, never pages) | warn-but-retry-forever | **PAGES** (fix rolling_anchor to raise `EV_OPERATOR_NEEDED` after N read_failures); above prefix_end = window territory, normal re-fetch |
| 9 | header reorg deeper than 10 | refused | unchanged: refuse + **page** (consensus-unrepresentable) |
| 10 | seal ring exhausted / reindex fallback exhausted / mismatch vs *compiled* checkpoint | n/a | **PAGES** (recompute already failed; repair is forbidden; a human looks) |
| 11 | progress.kv itself SQLITE_CORRUPT | FATAL | **PAGES** (the verb's own substrate is damaged) |

Rule of thumb the table encodes: **anomaly above S ⇒ rebuild; anomaly at/below S ⇒
page.** Nothing in between exists anymore.

#### 4e. The deletion list (LOC measured by `wc -l`; every delete gated on grep-proven zero callers; `boot.c:1709` is the canary and falls first)

**Wave A — the canonical-plan step-7 slate, retargeted at window_rebuild (lands only
after 4f M1–M3):** `utxo_recovery_torn_anchor.c` 213;
`stage_repair_reducer_frontier_{tipfin,refill,purge,coin}.c` + the tear branch
649+742+337+786+~400; `reducer_frontier_reconcile_light.c` 643; the chain_restore
ladder (repair 692, disk_repair 490, executor 230, planner 74, boot_snapshot 176,
boot_activation 50, integrity 148) 1,860; coins_view_sqlite rewind/reconcile/case-b
~300–500. **Subtotal ≈ 5,900–6,150** (the listed figures sum to 5,630 + 300–500).

**Wave B — beyond the old plan, subsumed by "any window anomaly ⇒ rebuild":** coin
re-mint scanners (`stage_repair_coin_backfill{,_scan,_util}.c`) 1,720; point rungs
(`stage_repair_rewind.c` 379, `utxo_apply_delta_repair.c` 456,
`stage_repair_header_solution.c` 201, `stage_repair_body_fetch.c` 196,
`stage_repair.c` 138, `stale_validate_headers_repair.c` 231) 1,601; the operator
repair surface → one `zcl_window_rebuild` tool (`repair_controller{,_rebuild,_utxo}.c`
+ `blockchain_controller_recovery.c`) 1,678; guess-between-copies orchestration
(`recovery_policy.c`, `utxo_recovery_{backfill,frontier_gate}.c`, majority of
`utxo_recovery_{restore,service}.c` — **carve-out:** the external zclassicd/LevelDB
import seam survives) net ~1,500–2,000; legacy self-heal hot path 401; boot reconcile
heuristics + repair tails of the block_index loaders ~1,000–1,500.

**Explicit KEEPS (honesty):** `boot_auto_reindex.c` + `boot_crashonly.c` (the fallback
rung the verb generalizes); `utxo_recovery_ldb_copy.c` (verified-install half);
`utxo_apply_delta_reorg.c` (the inner fast path AND the verb's step-3 engine);
`rolling_anchor_service.c` (extended into the seal ratifier + stall-pages fix).

**Additions:** seal writer + ratify ≈ 450–600; verb + sentinel + boot wiring ≈
500–700; MCP tool + state dump ≈ 150; fixtures/tests ≈ 800–1,200 (test ledger).
**The arithmetic, honestly:** Waves A+B as itemized sum to ≈13,500–15,000 gross — a
CEILING that assumes every "majority of" and "~" estimate deletes in full. The
committed slate is the conservative **−9,000 to −10,500 gross production LOC** of the
measured 13,760-LOC repair-family census (~16.2k with embedded branches) — the gap is
keeps-inside-listed-files, the import-seam carve-out, and estimate risk — against
~+1,300–1,600 added. This SUBSUMES and more than doubles the old item's −4,500 gross
slate.

#### 4f. Migration sequencing (hard ordering; does not contradict either in-flight branch)

1. ~~Land `fix/header-splice-derive-heights`~~ DONE (`c572def48`, merged `600efd53b`)
   — trustworthy window *content* at write time (heights from parent links, hash-bound
   verdicts); window_rebuild handles window *state* disposal. Complementary, zero
   overlap.
2. Land canonical-frontier steps 1–6: Invariant A DONE (`a2da7e107`, merged) + the
   derive-`coins_best` / demote-`utxos`-mirror **semantics** (plan steps 5–6 — IN
   FLIGHT as local worktree branch `refactor/derive-coins-best-demote-mirror`:
   derivation function + unit tests built, boot-gate and count repoints in progress;
   depend on the semantics, not the branch name).
3. **M1 — the seal (additive only):** seal table + candidate hook + ratify tick +
   prefix-end getter + `zcl_state subsystem=seal` + the rolling-anchor
   page-on-read-failure fix. Ships independently; proves itself by accumulating
   ratified seals live with zero behavior change.
4. **M2 — the verb:** `window_rebuild_run` + sentinel + `boot_crashonly` rewire
   (auto-reindex demoted to fallback rung). Fixture-proven before any trigger uses it.
5. **M3 — trigger rewire:** §4d rows 1–7 flipped one at a time, each with its fixture;
   old rung stays compiled-but-uncalled one step, then grep-zero-callers, then delete.
6. **M4 — deletion Waves A then B** + pinned tests + a lint ratchet in the
   `framework_shape_allowlist` style: no symbol outside
   `window_rebuild.c`/`utxo_apply_delta_reorg.c` may write `coins`/
   `coins_applied_height` on a recovery path ("recompute, never repair", enforced).

#### 4g. Acceptance proofs (all on datadir COPIES, never live)

- **The real wedge fixture:** a copy of frozen
  `~/.zclassic-c23-postrestore-wedge-20260611` boots with M2+M3, emits one
  `window_rebuild ok` event, then `tip_finalize` advances and the commitment equals the
  reseal value — the exact state that defeated the ladder must fall to the verb.
- **Synthetic splice fixture:** fabricated spliced-height/stale-pair state self-heals
  via `WR_HEIGHT_SPLICE` end-to-end in **<60 s** (expected 0.5–7 s replay + boot).
- **Synthetic coin tear:** deleted coins rows / bumped `coins_applied_height` converge
  via `WR_COIN_TEAR`, recomputed SHA3 byte-equal to the seal.
- **kill-9 mid-rebuild:** ≥50 randomized kill points (post-sentinel, mid-discard-txn,
  post-discard pre-verify, mid-replay, pre-clear); invariants: sentinel cleared ⇔
  verify passed; final commitment byte-equal to an untouched control replay; attempt
  budget honestly consumed.
- **Seal/sealed-bytes corruption:** corrupt newest seal ⇒ ring step-back; all 4 ⇒
  compiled-anchor reindex fallback; exhausted ⇒ page, node holds. The h=3115015-style
  corrupt frame below prefix_end ⇒ **pages** instead of retrying forever.
- **Ladder removal proof:** grep-zero-callers per module before each delete; build +
  lint (incl. the new ratchet + E13) + **test_parallel 0/409 green** after every wave;
  canaries #5/#7 green 7 consecutive days with the ladder gone.
- **Size:** M1–M2 ≈ 1 worker session each; M3–M4 ≈ 2 sessions, mechanical after the
  fixtures exist.

### 5. Standing full-history replay canary (the single highest-leverage gate) *(HARNESS + GATE LANDED — wave4/i5-replay-canary; 7-green-nights + live timer-install owner-gated)*

- **Goal:** a recurring institution that samples the REAL distribution: would have caught
  3 of this session's 4 live failures. The gates ARE the product — this lands before/with
  the deletions in item 4 because it is the only honest verification of them.
- **Mechanism as landed:** the harness `tools/scripts/replay_canary.sh` (pure bash + the
  binary's own tools, no python) + `tools/scripts/isolated_mainnet_env.sh` (mainnet sibling
  of the regtest isolation chokepoint). It spawns an isolated /tmp mainnet node on 3905x
  ports, headers via `--importblockindex` (read-only against `~/.zclassic`), seeds the UTXO
  snapshot to anchor 3,056,758, replays anchor->tip through the reducer, and asserts:
  (a) bg_validation reaches COMPLETE with zero header-admit rejects (`getsyncdiag`); (b) the
  compiled anchor checkpoint passed without an integrity FATAL + the local commitment is a
  real 64-hex recompute (`getutxocommitment`); (c) coarse UTXO stats == co-located zclassicd
  `gettxoutsetinfo` (RPC 8232, read-only). Weekly variant replays genesis->tip (~6 h,
  bg-validation ON, dials zclassicd P2P 8033 for bodies). **The AUTHORITATIVE verdict is a
  sentinel FILE** (`~/.local/state/zclassic23-canary/replay_canary_<from>.json`) written
  atomically (tmp + sync + rename) ONLY after every assertion passes; the shell exit code is
  advisory and drives systemd `OnFailure=` (the page channel: `journalctl -t replay-canary`).
  Never exit-0-as-proof — a killed/OOM run leaves no fresh PASS sentinel.
- **Deviations from the spec (recorded honestly):** (1) there is NO `getconsensusreport`
  RPC at HEAD — `consensus_reject_index_total` is MCP-only, unreachable from the zcl-rpc
  harness; the zcl-rpc-reachable "zero rejects" signal is `bg_validation.state==COMPLETE`
  (a consensus reject during replay → FAILED) + `getsyncdiag` `headers.total_rejected==0`.
  (2) From-anchor the commitment is computed at the TIP (not the anchor), so the byte-equal
  checkpoint assert is exercised by the boot integrity gate at h=3,056,758 (boot refuses
  otherwise), and the harness additionally enforces a non-error 64-hex commitment; a
  commitment taken exactly at the anchor height must equal the compiled checkpoint.
  (3) The mvp-spawn workflow merge (`c086c5136`) is OUT of scope (tracked separately).
- **Acceptance proof (landed):** the hermetic gate `lib/test/src/test_replay_canary_verdict.c`
  drives `replay_canary.sh --self-test=<mode>` against fixture RPC outputs and red-fails on a
  seeded known-bad reducer — `fail-rejects` (consensus_rejects), `fail-sha3` (sha3_mismatch),
  `fail-crossnode`, `fail-timeout` (budget_exceeded), plus the SIGKILL-leaves-no-fresh-PASS
  case — and green-passes the `pass` fixture. Runs in `make ci` via `test_zcl`/`test_parallel`.
  **Still owner-gated:** install the systemd timers (`deploy/examples/zclassic23-replay-canary-*`),
  accumulate 7 consecutive green nights, then flip the `docs/MVP.md` rows from ◐.
- **Size:** ~200 LOC shell/C harness + 2 timer/service unit pairs; 1 session. ~45 min/night
  + ~6 h/wk on the 32-core box.

### 6. Chain-derived golden extremals for every bounded consensus predicate

- **Goal:** permanently close the text-vs-chain class. FR-3 was one of 12 recorded
  text-derived rule findings resting on the same history-safe-by-text argument
  (FR-1–FR-5 + the miner cap + the 6 confirmed in
  [`consensus-parity-supplemental-audit-2026-06-08.md`](./consensus-parity-supplemental-audit-2026-06-08.md),
  whose summary explicitly invokes "exactly the FR-3 argument" — the argument the
  h=478544 tx refuted). "History-safe" must be a machine invariant, not a header comment
  (`consensus.h:23–29` was prose, never checked, and was wrong).
- **Mechanism:** one-time ~20-min C scan of the real chain producing a pinned per-era
  extremals table: max tx size, max block size, max sigops, version/locktime/expiry
  ranges, per activation era. New test group: every bounded consensus predicate must
  ACCEPT every pinned real extremal. Amend `docs/CONSENSUS_PARITY_DOCTRINE.md` with the
  precedence rule — **chain > deployed zclassicd behavior > zclassicd source text** — and
  require any bounded-predicate change to cite the table.
- **Acceptance proof:** the test group red-fails when `MAX_TX_SIZE_AFTER_SAPLING` is set
  to 102000 *without* the grandfather (reproducing FR-3), green with it; doctrine amended.
- **Size:** ~300 LOC scan tool + table + test group; 1 session. Scan cost is one-time.

### 7. Crash-boot soak gate — sample the off-diagonal torn-state space

- **Goal:** hand-enumerated unit tears cannot cover real multi-store crash states; the
  restore/recovery path must be exercised the way it fails. 0 of this session's 4
  failures were catchable by any gate as configured — this and #5 fix the measurement
  channel.
- **Mechanism:** nightly (~40 min, ~10 cycles): reflink-copy the frozen wedge fixture +
  a fresh live snapshot; boot HEAD; `kill -9` at randomized phases (boot, mid-advance,
  mid-reindex); reboot; assert SERVING, tip ≥ floor, no FATAL, no restart loop, within
  time budget. This is also the standing watch on the never-give-up unit: an infinite
  reindex loop is a red soak, not a mystery.
- **Acceptance proof:** soak red on the pre-`706a7c00a` binary against the wedge fixture
  (gate fires on the known-bad past), then 7 consecutive green nights at HEAD.
- **Size:** ~250 LOC harness + timer unit; 1 session.

### 8. One-command tenacious bootstrap — make the proven recipe the only path

- **Goal:** kill the cold-sync footgun. `-cold-import` alone leaves a 3.1M-header hole
  (headers=960) and pins forever; the proven recipe is `--importblockindex` FIRST
  (3.14M headers, 60–74 s) then `-cold-import` boot → hash-identical tip in ~25 min
  (multi-height verified vs zclassicd, warm-reboot-proven). A recipe that lives only in
  docs will rot (the docs already had it wrong once, plus the wrong zclassicd P2P port —
  it is 8033).
- **Mechanism:** wrap the two steps in the binary. Preferred shape: `-cold-import` boot
  **auto-detects** the header hole (header count ≪ source index extent) and runs the
  block-index import itself before applying the snapshot — auto-compose, don't document
  around. Optionally alias the whole thing as `-tenacious-sync=<sourcedir>`. Post-import
  completeness invariant always-on: contiguity genesis..tip, zero nBits==0, per-keyspace
  counts vs source LevelDB (the 1,561-record silent-tail-drop precedent is the reason
  row-count plausibility is banned) — seconds of cost, refuses to mark the sync complete
  otherwise.
- **Acceptance proof:** single command from an EMPTY datadir → SERVING at a
  hash-identical tip vs zclassicd in ~25 min; weekly cold-sync canary (~30 min) executes
  exactly this command forever, keeping the recipe honest by running it; the old
  partial-recipe path either composes or refuses loudly (no silent pin).
- **Size:** ~200 LOC boot wiring + invariant + canary unit; 1 session.

### 9. Remove the temporary `-nobgvalidation` once item 2 lands

- **Goal:** restore full background re-verification on the live node. The flag is a
  TEMPORARY mitigation injected 2026-06-11 via `~/.config/zclassic23/env`
  (word-split into `ZCL_ADDNODE_FLAGS`) because `bg_validation_service.c:291`
  `check_block` would false-flag canonical block 478544 → a `BLOCK_FAILED_VALID`
  tip-wedge vector (one stale failed-bit wedges the tip).
- **Mechanism:** after item 2 deploys, delete the flag from the env file, restart the
  service, let bg validation walk past h=478544.
- **Acceptance proof:** live `zcl_validationstatus` shows progress monotonically past
  478544 with zero false flags; no `BLOCK_FAILED_VALID` entries appear; RSS stays within
  the known bg-validation envelope (watch the stair-step gotcha).
- **Size:** config-only + one live observation window. Owner-visible (live deploy).

---

## Sequencing at a glance

```
DONE (merged to main):      1 (Invariant A)   2 (grandfather + replay proof)
NOW (parallel, in flight):  4f-step-2 (derive coins_best / demote mirror, worktree)
                            4-M1 (the seal — purely additive, may land in parallel)
THEN:                       3 (reindex epilogue)   5 (replay canary — lands WITH 3, gates 4)
THEN:                       4-M2/M3 (window_rebuild verb, then trigger rewire)
THEN:                       4-M4 (delete the ladder, Waves A→B)  6 (extremals)  7 (crash soak)
THEN:                       8 (one-command bootstrap + weekly canary)
LAST (live, owner-gated):   9 (drop -nobgvalidation)
```

Everything here is either an invariant at a chokepoint, a deletion, or a gate that
samples reality. Nothing adds a repair module — item 4 adds exactly one recovery *verb*
and deletes the ladder it replaces. Net production LOC across the roadmap is
**negative** (~−9,000..−10,500 of gross repair-family deletion per item 4e against
~+1,300–1,600 of seal/verb/invariants/harnesses ≈ **−7,500..−9,000 net** — subsuming
and more than doubling the canonical plan's −2,800..−3,100 slate), and the standing
compute cost is ~1–1.5 h/night + ~30 min/week on hardware we already own.
