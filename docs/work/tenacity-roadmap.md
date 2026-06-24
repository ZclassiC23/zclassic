# Tenacity Roadmap — sync super fast, fail almost never

**Date:** 2026-06-11 (rev 2026-06-16) · **Status:** active · **Builds on:** [`canonical-frontier-derived-state-plan.md`](./canonical-frontier-derived-state-plan.md) ("the canonical plan" — not duplicated here).

> This roadmap is forward engineering, not an active-incident log — the historical failures below are the *origin* of each item, not current blockers. Near-term hardening sequencing lives in [`stability-improvements-2026-06-16.md`](./stability-improvements-2026-06-16.md); the merged rock-solid recovery-program L1/L2 items are in this doc (see "Rock-solid recovery program" below).

## The lens

The three principles every item anchors to (state-duplication → divergence; verification must sample the real distribution not reference text; sync has an information floor) are TENACITY doctrine — see [`docs/TENACITY.md`](../TENACITY.md). Floor is already measured: `--importblockindex` 3.14M headers in 60–74 s, then a normal boot (auto-reads ~/.zclassic) → hash-identical tip in ~25 min; remaining work is making the floor the *only* path and never letting "fast" mean "silently incomplete".

**Standing rule (doctrine, enforced on every PR):** a new wedge gets a new *write-time invariant at a chokepoint*, never a new repair module.

---

## Ordered items

### 1. Invariant A — frontier-bounded tip *(LANDED — `21d177bf9`+`447fa757b`, merged `a2da7e107`)*
Makes Wedge A (restore installs a tip above the validated header frontier → post-restore integrity FATAL → boot crash-loop) unwritable. Log half: every restore install funnels through the single `utxo_recovery_commit_tip` chokepoint with evidence-based floor rewind. Index half: installs must also be trust-rooted (pprev descent to genesis/SHA3-anchor), settle-looped, boot promotion bypass gated. Proven on the frozen fixture `~/.zclassic-c23-postrestore-wedge-20260611` (boots CLEAN, zero reindex, serving); build+lint+test_parallel 0/409. Residue on that fixture = the coins_applied>hstar coin-tear class (item 3 / §4d row 1).

### 2. Oversize grandfather fix + full genesis replay *(LANDED — `ccc7fbbfa`, merged `b0c0b4f9a`)*
The canonical chain must replay from genesis with zero consensus rejects. FR-3's text-copied `MAX_TX_SIZE_AFTER_SAPLING=102000` false-rejected real history (mined when the rule was 2 MB). A complete scan (every height 0..3143532, oracle-compared) found **413** oversize txs (h=478544..1968856, max 1,922,197 B at h=685036); each is excused via a txid-keyed grandfather allowlist in `domain/consensus/src/tx_structural.c`, **BLOCK context only** — mempool and fresh blocks stay strict 102000 (= zclassicd live behavior). The standing-nightly-replay institution (commitment == zclassicd `gettxoutsetinfo` every run) is item 5.

### 3. Reindex epilogue derivation — recovery must not manufacture the next wedge *(LANDED — reindex_epilogue.c wired at config/src/boot_index.c:402; test_reindex_epilogue.c)*
- **Problem:** `-reindex-chainstate` (the crash-only auto-recovery path, `706a7c00a`) ends torn; with the never-give-up unit (`0b45e93a5`) it can degrade into an infinite reindex loop. Confirmed gaps at `config/src/boot_index.c:160–297` and `config/src/boot.c:3321–3344`: `boot_index_clear_coins_state` **deletes** `coins_best_block`/`utxo_commitment`/`utxo_sha3` but the epilogue never **recomputes** them; `coins_kv` is never reseeded; the `utxo_apply`/`coins_applied` cursors keep stale pre-reindex values — recovery manufactures the `coins_applied > hstar` wedge shape.
- **Fix:** the epilogue derives in one ordered commit: reseed coins_kv from the replayed set, recompute (not delete) the SHA3 commitment, clamp the reducer cursors to the replayed tip. Derived, never installed-stale.
- **Acceptance:** (a) regtest reindex smoke (~50 blocks, `-reindex-chainstate`, assert tip + row-count parity + recomputed==served commitment + SERVING) — gates every push, ~3 min; (b) mainnet copy reindex ends with cursors == replayed tip, no `coins_applied > hstar`; (c) kill -9 mid-reindex then reboot converges (feeds item 7).
- **Size:** ~250 LOC + smoke harness; 1 session.

### 4. Seal + Window — `window_rebuild`, the one recovery verb *(DESIGN)*
**Status: PLANNED unless a line is marked IS.** Survey basis: main @`706a7c00a`. Consensus parity untouched throughout — `ZCL_FINALITY_DEPTH`, checkpoints, validity rules are *read*, never written. Doctrine home: `docs/TENACITY.md`.

**The doctrine (THE ZCLASSIC23 WAY).** The chain is the only immutable truth. Derived state lives in exactly two domains. The **SEALED DOMAIN** (≤ seal height S): pinned by a checkpointed SHA3 UTXO commitment, recomputable from genesis, continuously proven by the standing replay institution (item 5). The **WORKING WINDOW** (> S): cursors, stage logs, coin deltas, verdicts, mirrors — explicitly **disposable**. One recovery verb replaces the entire ladder: **`window_rebuild`** — discard the window, reset to the seal, replay forward, recompute the commitment, verify, resume. **Recompute, never repair.** Our 2026-06-11 record is the evidence: every recovery that worked was a recompute (cold-import, reindex, auto-reindex, reorg unwind); every one that failed/re-wedged was a repair (anchor heals, backfills, reconcile guesses, oscillating poison rewinds).

#### 4a. The seal — dual boundary

| Boundary | Height | Status | Mechanism |
|---|---|---|---|
| **Soft margin F** (reorg floor) | tip − `ZCL_FINALITY_DEPTH` (10) | **IS** | bounded inverse-delta unwind, `utxo_apply_delta_reorg.c` (finality clamp ~:381–:421, range delete :218) |
| **Hard seal S** (state checkpoint) | latest *ratified* grid seal, nominal lag 1,000–2,100 blocks | **PLANNED** | new `seal` ring in progress.kv, hosted in rolling_anchor |

- Consensus already makes >10-deep reorgs unrepresentable (`main_constants.h:33-34,67`; `sync_evidence_policy.c:11-15`). Observed: 8 unwinds, all depth=3; kill-9 WAL rewinds 1–6 blocks. Seal lag = one 1,000-block grid window + ≤1 of accumulation + 10-block ratification — two orders of magnitude of margin; replay bounded ~0.5–7 s (300–1,700 blk/s on the connect/reindex path; the ~1–3 blk/s projection catch-up path is forbidden for window replay).
- Seals land on the existing 1,000-block anchor grid (`SHA3_WINDOW_SIZE=1000`, `sha3_windows.h:22`): **rolling_anchor seals replay INPUT** (block-bytes SHA3 per window, `ra_compute_window_hash`, `rolling_anchor_service.c:312-360` — IS); **the seal ring seals replay OUTPUT** (state — PLANNED).
- **Floor:** with no runtime seal, S degenerates to the compiled checkpoint h=3,056,758 (`checkpoints.c:86-105`, mirrored by `REDUCER_FRONTIER_TRUSTED_ANCHOR`) and window_rebuild degenerates to today's auto-reindex — the design strictly generalizes what works.
- **Seal record (PLANNED):** `seal(height, block_hash, coins_sha3, nullifier_sha3, utxo_count, supply, anchor_window_sha3, ratified, sealed_at, self_sha3)` in progress.kv (co-commit theorem of `coins_kv.h:1-23` extends to it), kept as a ring of the last 4 (a corrupt latest seal steps back one window).
- **Advance protocol (PLANNED): candidate at the frontier, ratify at depth.** (1) When `coins_applied_height` crosses a grid point G in steady-state tip-following (never during IBD/bulk replay), run the ~1 s coins_kv + nullifiers SHA3 scan at the flush boundary (`process_block_flush_policy.c:183`) and insert `ratified=0`, co-committed in the same progress.kv epoch. (2) The supervisor child `chain.rolling_anchor` (60 s tick, `rolling_anchor_service.c:52,:549-557`) ratifies iff G ≤ `zcl_immutable_height(tip)`, block-bytes prefix end ≥ G (export `rolling_anchor_effective_prefix_end`, :482-488), and the active chain still contains `block_hash` at G. (3) On ratification, prune `utxo_apply_delta` ≤ G and stage-log rows ≤ G−RETENTION — seal advance IS the retention policy. A crash leaves no candidate or an unratified one; nothing to repair.

#### 4b. The window contract
**The rule (absolute):** no window state may ever be consulted as evidence for a repair decision. Window state is OUTPUT, never INPUT. Admissible evidence = the seal ring, sealed block bytes (compiled `sha3_windows` + runtime evidence file + compiled checkpoint), the header tree at/below S, and consensus rules. Every rung that reads two window artifacts and *guesses which is right* (coins vs cursors, log vs mirror, anchor vs frontier) is structurally forbidden — a disagreement between two window artifacts is resolved by discarding both.

Window inventory (everything > S; all IS, survey-verified): the 8 stage cursors + 8 stage logs in progress.kv; `coins`/`nullifiers` rows above S + `utxo_apply_delta` inverses (the one window artifact the verb reads, only to invert it); `coins_applied_height`; `created_outputs_index`; misc `progress_meta` markers; the node.db `utxos` mirror + `node_state` keys (`coins_best_block`, `utxo_commitment`, `utxo_sha3`, tip pair, sapling_tree); the tip_finalize last-advance pair; block_map verdict bits above S (the header *tree* below S is sealed; *verdicts* above S are window); `pindex_best_header` projection; dirty coins cache; projection-db residue; the rolling-anchor runtime evidence file. NOT window: the rebuild/reindex sentinels, the seal ring, blocks/ files, the read-only LevelDB import source.

#### 4c. The primitive — `window_rebuild`
- **Home:** `app/jobs/src/window_rebuild.c` (+ header) — a Job: the reducer resetting its own state via the stages' storage contracts. Sentinel: `lib/storage/src/window_rebuild_sentinel.c`, byte-for-byte the `boot_auto_reindex.h` pattern (request/pending/clear, fsync-durable, `WINDOW_REBUILD_MAX=3`).
- **Crash-only execution:** a runtime trigger only writes the sentinel (typed reason + anchor + attempt) and requests a restart; the verb runs ONLY at boot, after block_index load and before any stage thread starts. A crash during rebuild looks like a fresh trigger (sentinel clears only after post-rebuild verify); the attempt budget burns honestly across crashes.
- **Steps:** (1) log `window_rebuild begin reason=… attempt=N`; (2) select S = newest self-hash-valid *ratified* seal, else the compiled checkpoint; (3) discard the window in ONE progress.kv `BEGIN IMMEDIATE`: unwind coins/nullifiers C→S+1 via `utxo_apply_delta_reorg.c` (fork-point generalized to S), delete the 8 stage logs above S, reset all 8 cursors to S (one anchor row in tip_finalize_log, the trusted-seed stamp pattern), `coins_applied_height := S`; (4) recompute coins + nullifier SHA3 and verify vs the seal (+count/supply) — mismatch steps the ring back one seal and repeats; (5) reset derived mirrors FROM the just-verified seal values (closes item 3's delete-but-never-recompute gap), clear verdict bits above S; (6) replay forward S+1→target on the reindex connect path (`boot_index.c:183-305` grown a start-height parameter); a missing body above S just stops the replay — the rest is re-fetched by the stages after resume; (7) reseal at the highest grid point ≤ frontier, run the boot integrity gate, clear the sentinel, resume; (8) budget: 120 s wall (17× headroom), 3 attempts ⇒ fall back one rung to full auto-reindex from the compiled anchor (kept verbatim) ⇒ if that also exhausts, **page** (`EV_OPERATOR_NEEDED` + typed trail). The verb never reads/re-fetches below S — sealed-domain corruption pages, always.
- **Entry points:** runtime `window_rebuild_trigger(reason, anchor)` from every §4d site; boot direct-call from `boot_crashonly.c` classification and the coins-integrity gate; operator: one MCP tool `zcl_window_rebuild` (replaces the whole repair_controller surface) + `zcl_state subsystem=seal`.

#### 4d. The trigger table — every detector stays, every remedy becomes the verb

| # | Wedge class (detector — IS) | Today's remedy (dies) | New remedy |
|---|---|---|---|
| 1 | coin tear (Invariant-B detector, `c8018a388`) | frontier tear branch + `_coin.c` re-mint + reconcile_light | `window_rebuild(WR_COIN_TEAR)` |
| 2 | height splice / cursor oscillation / stale tip_finalize pair | poison_rewind + header_solution repair | splice branch (4f step 1) prevents at write time; residue ⇒ `WR_HEIGHT_SPLICE` |
| 3 | stale/holed verdict at frontier (the h=3142977 class) | refill/purge/tipfin rungs, stale_validate_headers_repair | `WR_STALE_VERDICT` |
| 4 | torn coins anchor at boot (`boot.c:1707-1712` — the deletion canary) | L1 torn-anchor heal, then FATAL | `WR_TORN_ANCHOR` (pre-stage; direct call) |
| 5 | reorg residue / unwind failure / value_overflow hole | delta_repair one-shot + backfill scanners | inner ≤10-deep unwind kept; any failure ⇒ `WR_REORG_RESIDUE` |
| 6 | boot integrity mismatch (4 boot.c reconcile branches) | reconcile-and-guess | `WR_BOOT_INTEGRITY` |
| 7 | blocker escalation / tip_not_advancing exhausted | restart ladder → operator | one `WR_BLOCKER_ESCALATION` before paging |
| 8 | corrupt block frame at/below seal (h=3115015 fixture stall: read failure breaks the extend loop, 60 s tick retries forever, `rolling_anchor_service.c:429-436`; on supervisor stall only warns, `LOG_WARN`+`EV_CHAIN_ADVANCE_DECISION` :490-502, never pages) | warn-but-retry-forever | **PAGES** (fix rolling_anchor to raise `EV_OPERATOR_NEEDED` after N read_failures); above prefix_end = window territory, normal re-fetch |
| 9 | header reorg deeper than 10 | refused | unchanged: refuse + **page** (consensus-unrepresentable) |
| 10 | seal ring exhausted / reindex fallback exhausted / mismatch vs *compiled* checkpoint | n/a | **PAGES** (recompute already failed; repair is forbidden) |
| 11 | progress.kv itself SQLITE_CORRUPT | FATAL | **PAGES** (the verb's own substrate is damaged) |

Rule of thumb: **anomaly above S ⇒ rebuild; anomaly at/below S ⇒ page.** Nothing in between exists anymore.

#### 4e. The deletion list (LOC by `wc -l`; every delete gated on grep-proven zero callers; `boot.c:1709` is the canary, falls first)
- **Wave A (canonical-plan step-7 slate, retargeted at window_rebuild; lands only after 4f M1–M3):** `utxo_recovery_torn_anchor.c` 213; `stage_repair_reducer_frontier_{tipfin,refill,purge,coin}.c` + the tear branch 649+742+337+786+~400; `reducer_frontier_reconcile_light.c` 643; the chain_restore ladder (repair 692, disk_repair 490, executor 230, planner 74, boot_snapshot 176, boot_activation 50, integrity 148) 1,860; coins_view_sqlite rewind/reconcile/case-b ~300–500. **Subtotal ≈ 5,900–6,150.**
- **Wave B (subsumed by "any window anomaly ⇒ rebuild"):** coin re-mint scanners (`stage_repair_coin_backfill{,_scan,_util}.c`) 1,720; point rungs (`stage_repair_rewind.c` 379, `utxo_apply_delta_repair.c` 456, `stage_repair_header_solution.c` 201, `stage_repair_body_fetch.c` 196, `stage_repair.c` 138, `stale_validate_headers_repair.c` 231) 1,601; the operator repair surface → one `zcl_window_rebuild` (`repair_controller{,_rebuild,_utxo}.c` + `blockchain_controller_recovery.c`) 1,678; guess-between-copies orchestration (`recovery_policy.c`, `utxo_recovery_{backfill,frontier_gate}.c`, majority of `utxo_recovery_{restore,service}.c` — **carve-out:** the external zclassicd/LevelDB import seam survives) net ~1,500–2,000; boot reconcile heuristics + block_index loader repair tails ~1,000–1,500; legacy self-heal hot path 401.
- **Explicit KEEPS:** `boot_auto_reindex.c` + `boot_crashonly.c` (the fallback rung); `utxo_recovery_ldb_copy.c` (verified-install half); `utxo_apply_delta_reorg.c` (inner fast path AND the verb's step-3 engine); `rolling_anchor_service.c` (extended into seal ratifier + stall-pages fix).
- **Additions:** seal writer + ratify ≈ 450–600; verb + sentinel + boot wiring ≈ 500–700; MCP tool + state dump ≈ 150; fixtures/tests ≈ 800–1,200.
- **The arithmetic:** Waves A+B itemized sum to ≈13,500–15,000 gross (a CEILING). Committed slate is the conservative **−9,000 to −10,500 gross production LOC** of the measured 13,760-LOC repair-family census (~16.2k with embedded branches), against ~+1,300–1,600 added — subsumes and more than doubles the old item's −4,500 gross slate.

#### 4f. Migration sequencing (hard ordering)
1. ~~Land `fix/header-splice-derive-heights`~~ DONE (`c572def48`, merged `600efd53b`) — trustworthy window *content* at write time (heights from parent links, hash-bound verdicts). Complementary, zero overlap.
2. Canonical-frontier steps 1–6: Invariant A DONE (`a2da7e107`) + derive-`coins_best` / demote-`utxos`-mirror **semantics** (plan steps 5–6, IN FLIGHT, worktree branch `refactor/derive-coins-best-demote-mirror`: derivation fn + unit tests built, boot-gate + count repoints in progress — depend on the semantics, not the branch name).
3. **M1 — the seal (additive only):** seal table + candidate hook + ratify tick + prefix-end getter + `zcl_state subsystem=seal` + the rolling-anchor page-on-read-failure fix. Ships independently; proves itself by accumulating ratified seals live, zero behavior change.
4. **M2 — the verb:** `window_rebuild_run` + sentinel + `boot_crashonly` rewire (auto-reindex demoted to fallback). Fixture-proven before any trigger uses it.
5. **M3 — trigger rewire:** §4d rows 1–7 flipped one at a time, each with its fixture; old rung stays compiled-but-uncalled one step, then grep-zero-callers, then delete.
6. **M4 — deletion Waves A then B** + pinned tests + a lint ratchet (`framework_shape_allowlist` style): no symbol outside `window_rebuild.c`/`utxo_apply_delta_reorg.c` may write `coins`/`coins_applied_height` on a recovery path ("recompute, never repair", enforced).

#### 4g. Acceptance proofs (all on datadir COPIES, never live)
- **Real wedge fixture:** a copy of `~/.zclassic-c23-postrestore-wedge-20260611` boots with M2+M3, emits one `window_rebuild ok` event, `tip_finalize` advances, commitment == reseal value.
- **Synthetic splice fixture:** fabricated spliced-height/stale-pair state self-heals via `WR_HEIGHT_SPLICE` end-to-end in **<60 s** (expected 0.5–7 s).
- **Synthetic coin tear:** deleted coins rows / bumped `coins_applied_height` converge via `WR_COIN_TEAR`, recomputed SHA3 byte-equal to the seal.
- **kill-9 mid-rebuild:** ≥50 randomized kill points (post-sentinel, mid-discard-txn, post-discard pre-verify, mid-replay, pre-clear); invariants: sentinel cleared ⇔ verify passed; final commitment byte-equal to an untouched control replay; budget honestly consumed.
- **Seal/sealed-bytes corruption:** corrupt newest seal ⇒ ring step-back; all 4 ⇒ compiled-anchor reindex fallback; exhausted ⇒ page, node holds. The h=3115015-style corrupt frame below prefix_end ⇒ **pages** instead of retrying forever.
- **Ladder removal proof:** grep-zero-callers per module before each delete; build + lint (incl. the new ratchet + E13) + **test_parallel 0/409 green** after every wave; canaries #5/#7 green 7 consecutive days with the ladder gone.
- **Size:** M1–M2 ≈ 1 session each; M3–M4 ≈ 2 sessions, mechanical after fixtures exist.

### 5. Standing full-history replay canary *(HARNESS + GATE LANDED — wave4/i5-replay-canary; 7-green-nights + live timer-install owner-gated)*
- **Goal:** a recurring institution that samples the REAL distribution (would have caught 3 of this session's 4 live failures). The gates ARE the product — lands before/with item 4's deletions, the only honest verification of them.
- **Landed:** harness `tools/scripts/replay_canary.sh` (pure bash + the binary's own tools) + `tools/scripts/isolated_mainnet_env.sh` (mainnet sibling of the regtest isolation chokepoint). Spawns an isolated /tmp mainnet node on 3905x ports, headers via `--importblockindex` (read-only vs `~/.zclassic`), seeds the UTXO snapshot to anchor 3,056,758, replays anchor→tip, asserts: (a) bg_validation COMPLETE with zero header-admit rejects (`getsyncdiag`); (b) the compiled anchor checkpoint passed without integrity FATAL + local commitment is a real 64-hex recompute (`getutxocommitment`); (c) coarse UTXO stats == co-located zclassicd `gettxoutsetinfo` (RPC 8232, read-only). Weekly variant replays genesis→tip (~6 h, bg-validation ON, zclassicd P2P 8033 for bodies). **Authoritative verdict = a sentinel FILE** (`~/.local/state/zclassic23-canary/replay_canary_<from>.json`, atomic tmp+sync+rename) written ONLY after every assertion passes; shell exit is advisory and drives systemd `OnFailure=` (page channel `journalctl -t replay-canary`). Never exit-0-as-proof — a killed/OOM run leaves no fresh PASS sentinel.
- **Deviations from spec (honest):** (1) NO `getconsensusreport` RPC at HEAD — `consensus_reject_index_total` is MCP-only; the zcl-rpc-reachable "zero rejects" signal is `bg_validation.state==COMPLETE` + `getsyncdiag headers.total_rejected==0`. (2) From-anchor the commitment is computed at the TIP, so the byte-equal checkpoint assert is exercised by the boot integrity gate at h=3,056,758 (boot refuses otherwise) + a non-error 64-hex check. (3) The mvp-spawn workflow merge (`c086c5136`) is OUT of scope.
- **Acceptance (landed):** hermetic gate `lib/test/src/test_replay_canary_verdict.c` drives `replay_canary.sh --self-test=<mode>` against fixture RPC outputs, red-fails on a seeded known-bad reducer (`fail-rejects`, `fail-sha3`, `fail-crossnode`, `fail-timeout`, plus SIGKILL-leaves-no-fresh-PASS) and green-passes `pass`. Runs in `make ci`. **OPEN (owner-gated):** install the systemd timers (`deploy/examples/zclassic23-replay-canary-*`), accumulate 7 consecutive green nights, then flip the `docs/MVP.md` rows from ◐.
- **Size:** ~200 LOC harness + 2 timer/service unit pairs; 1 session. ~45 min/night + ~6 h/wk.

### 6. Chain-derived golden extremals for every bounded consensus predicate *(OPEN)*
- **Goal:** permanently close the text-vs-chain class. FR-3 was one of 12 text-derived rule findings on the same "history-safe-by-text" argument (FR-1–FR-5 + the miner cap + the 6 confirmed in [`consensus-parity-supplemental-audit-2026-06-08.md`](./consensus-parity-supplemental-audit-2026-06-08.md), which explicitly invokes "exactly the FR-3 argument" — the one h=478544 refuted). "History-safe" must be a machine invariant, not a header comment (`consensus.h:23–29` was prose, never checked, and was wrong).
- **Mechanism:** one-time ~20-min C scan of the real chain → a pinned per-era extremals table (max tx size, max block size, max sigops, version/locktime/expiry ranges, per activation era). New test group: every bounded consensus predicate must ACCEPT every pinned real extremal. Amend `docs/CONSENSUS_PARITY_DOCTRINE.md` with the precedence rule — **chain > deployed zclassicd behavior > zclassicd source text** — and require any bounded-predicate change to cite the table.
- **Acceptance:** the test group red-fails when `MAX_TX_SIZE_AFTER_SAPLING` is set to 102000 *without* the grandfather (reproduces FR-3), green with it; doctrine amended.
- **Size:** ~300 LOC scan tool + table + test group; 1 session (scan is one-time).

### 7. Crash-boot soak gate — sample the off-diagonal torn-state space *(OPEN)*
- **Goal:** hand-enumerated unit tears cannot cover real multi-store crash states; exercise restore/recovery the way it fails. 0 of this session's 4 failures were catchable by any gate as configured — this and #5 fix the measurement channel.
- **Mechanism:** nightly (~40 min, ~10 cycles): reflink-copy the frozen wedge fixture + a fresh live snapshot; boot HEAD; `kill -9` at randomized phases (boot, mid-advance, mid-reindex); reboot; assert SERVING, tip ≥ floor, no FATAL, no restart loop, within budget. Also the standing watch on the never-give-up unit: an infinite reindex loop is a red soak, not a mystery.
- **Acceptance:** soak red on the pre-`706a7c00a` binary against the wedge fixture, then 7 consecutive green nights at HEAD.
- **Size:** ~250 LOC harness + timer unit; 1 session.

### 8. One-command tenacious bootstrap — make the proven recipe the only path *(OPEN)*
- **Goal:** kill the cold-sync footgun. The two-step recipe (`--importblockindex` then a normal boot; zclassicd P2P=8033) is in CLAUDE.md "Tenacity & recovery"; `-cold-import` alone leaves a 3.1M-header hole (headers=960) and pins forever. A docs-only recipe rots — fold it into the binary.
- **Mechanism:** wrap the two steps in the binary. Preferred: `-cold-import` boot **auto-detects** the header hole (header count ≪ source index extent) and runs the block-index import itself before applying the snapshot. Optionally alias as `-tenacious-sync=<sourcedir>`. Post-import completeness invariant always-on: contiguity genesis..tip, zero nBits==0, per-keyspace counts vs source LevelDB (the 1,561-record silent-tail-drop precedent bans row-count plausibility) — refuses to mark the sync complete otherwise.
- **Acceptance:** single command from an EMPTY datadir → SERVING at a hash-identical tip vs zclassicd in ~25 min; weekly cold-sync canary (~30 min) runs exactly this command forever; the old partial-recipe path either composes or refuses loudly (no silent pin).
- **Size:** ~200 LOC boot wiring + invariant + canary unit; 1 session.

### 9. Remove the temporary `-nobgvalidation` once item 2 deploys *(OPEN, owner-gated/live)*
- **Goal:** restore full background re-verification on the live node. The flag is a TEMPORARY mitigation (2026-06-11, via `~/.config/zclassic23/env` word-split into `ZCL_ADDNODE_FLAGS`) because `bg_validation_service.c:291` `check_block` would false-flag canonical block 478544 → a `BLOCK_FAILED_VALID` tip-wedge vector (one stale failed-bit wedges the tip).
- **Mechanism:** after item 2 deploys, delete the flag from the env file, restart the service, let bg validation walk past h=478544.
- **Acceptance:** live `zcl_validationstatus` shows progress monotonically past 478544 with zero false flags; no `BLOCK_FAILED_VALID` entries; RSS stays within the known bg-validation envelope (watch the stair-step gotcha).
- **Size:** config-only + one live observation window.

---

## Rock-solid recovery program — open L1/L2 items (merged from rock-solid-program-2026-06-16)

The 2026-06-16 recovery program's Layer-0 (restore the live node) is DONE; these are
its still-open autonomous (copy-prove-gated, no live mutation) and continuous-proof
items. Both Layer-0 wedge classes (the coin tear AND restart fragility) share one
root: cold import is a non-self-sufficient "borrow from zclassicd" bootstrap (skips
SHA3 verify at non-checkpoint heights, depends on a co-located zclassicd, commits
state derived from zclassicd's index). The structural cure is self-sufficient sync
(FlyClient + SHA3 snapshot + delta P2P) — see [`never-stuck-plan.md`](./never-stuck-plan.md);
these harden the bridge.

### L1 — autonomous durability (copy-prove-gated, no live mutation)

- **(b) Cold-import restart-fragility — OPTION 1 *(keystone)*.** Stop the destructive
  `zclassicd_import_best` backward commit (and the clamped flat re-save) when the
  node's own derived frontier is **strictly above** zclassicd's index tip. Derive the
  frontier via `boot_derive_coins_best(&ndcb)` (`config/src/boot.c:820`); suppress
  `boot_promote_tip_via_csr(best, "zclassicd_import_best", false)` (`:2110-2113`) ONLY
  when `have_ndcb && ndcb.height > zcd_best_h` (**strictly** `>`); emit a loud WARN +
  `EV_RECOVERY_ACTION zcd_import_tip_suppressed`. Keep the index import + blk-file
  linking unconditional. Skip `save_block_index_flat` (`:2122`) when promotion was
  suppressed. Key strictly `>`, never `>=` (a node at exactly zcd's tip or trailing
  MUST still promote). Do NOT touch the detached-island guard, do NOT raise
  `REBUILD_RECENT_MAX_RANGE`, do NOT force-link the pprev=NULL placeholder. Root cause
  of the live `contradiction_frozen` latch. Copy-prove matrix: baseline reproduces the
  backward commit; patched suppresses it; negative (real torn set / derived ≤ zcd)
  still promotes/refuses as today. **CAVEAT (verify first):** the captured fixture's
  failure may be stale-flat-rejection → detached-root, which OPTION-1 does NOT cover;
  the true fix may be forward-extent connectivity preservation (raise-only projection
  topup so coins-best is never a detached root).
- **(c) `chain_restore_finalize` SEGV — serialize the recovery cascade.** Make the
  rebuild cascade hold `ms->cs_main` for the block_index/active_chain mutation. Today
  `chain_restore_rebuild_active_chain{,_from_block_files}` mutate shared block_index
  node fields + the `active_chain` slot array under `c->write_lock` ONLY (never
  `cs_main`, `chain_restore_repair.c:50,52,231,242`) while bg-hash-verify + bg-validation
  read those same fields under `cs_main`/lock-free → heap corruption (same class as the
  documented `phashBlock`/`block_map_grow` UAF). Defense-in-depth: quiesce bg-validation
  + bg-hash around the rebuild, or only run it at boot before workers spawn. Gate:
  ASan/UBSan + TSan binary on an ISOLATED copy of an anchor-replay datadir exhibiting
  `tip_window_holes`; confirm both sanitizers clean + the canary survives. Review the
  lock change against the LOCK-ORDER LAW (do NOT take `cs_main` while holding a leaf
  lock the drive path holds).
- **(d1) Scope the replay-canary FAIL sentinel *(LANDED 2026-06-24)*.**
  `canary_sentinel_watch.c` now filters both stale dimensions in the shared
  `~/.local/state/zclassic23-canary/` verdict dir: cross-build FAILs do not page
  (already present before this pass), and FAILs whose `started_ts` predates the
  watcher process start are recorded as `stale_run` but do not latch
  `replay_canary_failed`. Both stale drops log once per mtime, and
  `test_canary_sentinel_watch` proves stale-run suppression plus fresh same-build
  FAIL paging.
- **(d2) Make `contradiction_frozen` self-clearing once reconciled.** The freeze is
  raised on a transient `active_tip_hash != csr_tip_hash` at boot
  (`chain_evidence_reconstruct.c:178-184`) but the remedy returns
  `COND_REMEDY_FAILED` and never clears (`contradiction_frozen.c:38`) even after the
  chain reconciles forward. The coins-best cursor mismatch is already correctly
  treated as recoverable lag in the same function (`:185-201`) — the self-clearing
  precedent is right there. Fix: once `active_tip_hash == csr_tip_hash` again, clear
  the `cec.*` keys and let the witness mark the condition resolved. Root trigger is
  (b); (d2) is belt-and-suspenders for any datadir already carrying the latch.
  Secondary: eliminate the `database is locked` write of `cec.contradiction_reason`
  at boot.

### L2 — continuous proof (all LOCAL, no GitHub Actions)

- **(4.1) Local pre-push enforcement.** Add a tracked `.githooks/pre-push` running
  `make lint` (incl. E13) + a fast `test_parallel` subset; `git config core.hooksPath
  .githooks` (per-clone; document for wt2/wt3). A push failing lint or the
  consensus-parity golden group is blocked locally. Keep it fast enough to not tempt
  `--no-verify`.
- **(4.2) systemd --user timer for the heavy gate + soak accrual.** Nightly `make ci`
  against HEAD, dated verdict = local CI. Pair with the dev lane staying up to accrue
  soak time toward MVP-C (7-day soak).
- **(4.3) Get the replay canary GREEN and scheduled.** Masking fix already landed
  (poll loop `kill -0` + `node_crashed_signal_N`). Remaining: land (c) so anchor-replay
  no longer SEGVs, land (d1) so a stale cross-binary sentinel can't latch a false FAIL,
  then schedule on the §4.2 timer and require a PASS sentinel for the running
  build_commit before declaring the node soak-healthy.
- **(4.4) MVP-C8 parity oracle — keep it continuous.** Make the tip-hash-vs-zclassicd
  probe a continuous timer on live + soak lanes, alerting on `match:false`, so a future
  tear is caught the moment it diverges, not 2.85 days later. The live node had NO
  continuous set-parity gate when the tear formed.
- **(4.5) Rolling-commitment seal *(owner-gated, designed not built)*.** A rolling
  per-window SHA3 UTXO commitment, persisted, would let a node detect AND repair a coin
  tear from its own recent history instead of a full re-import. Consensus-adjacent; must
  be designed against the parity doctrine first. Overlaps item 4 (the seal) above.

---

## Sequencing at a glance

```
DONE (merged):              1 (Invariant A)   2 (grandfather + replay proof)
                            5 (replay canary HARNESS+GATE; 7-green-nights + timer install owner-gated)
                            4f-step-1 (header-splice-derive-heights)
NOW (parallel, in flight):  4f-step-2 (derive coins_best / demote mirror, worktree)
                            4-M1 (the seal — purely additive)
THEN:                       3 (reindex epilogue)   5 timers (gates 4)
THEN:                       4-M2/M3 (window_rebuild verb, then trigger rewire)
THEN:                       4-M4 (delete the ladder, Waves A→B)  6 (extremals)  7 (crash soak)
THEN:                       8 (one-command bootstrap + weekly canary)
LAST (live, owner-gated):   9 (drop -nobgvalidation)
```

Everything here is an invariant at a chokepoint, a deletion, or a gate that samples reality. Nothing adds a repair module — item 4 adds exactly one recovery *verb* and deletes the ladder it replaces. Net production LOC is **negative**: item 4e's −9,000..−10,500 gross deletion against ~+1,300–1,600 added ≈ **−7,500..−9,000 net** (more than doubles the canonical plan's −2,800..−3,100 slate). Standing compute cost ~1–1.5 h/night + ~30 min/week on hardware we own.
