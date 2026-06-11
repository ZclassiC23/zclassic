# TENACITY — Standing Engineering Doctrine for a Tenacious Binary

*Adopted 2026-06-11 from the information-theory diagnosis (HEAD `0b45e93a5`).
Companion docs: `docs/work/canonical-frontier-derived-state-plan.md` (the what),
`docs/CONSENSUS_PARITY_DOCTRINE.md` (the parity bar),
`docs/work/fast-path.md` (diagnose on a COPY, never live).*

**Honesty rule for this document:** every claim is marked **[LANDED commit]**,
**[IN FLIGHT branch]**, **[DESIGNED spec]** (written design, not yet code), or
**[PROPOSED]**. Status lines must be updated when reality changes; a stale
LANDED is a doctrine violation.

---

## The zclassic23 way

Six principles, stated natively. Everything else in this document is an
instance of one of them. The plain model first: **blocks are the input rows;
everything else is a formula.** Every table, cursor, mirror, and commitment in
the datadir is a formula over the chain. Bugs happen when someone types a
number into a formula cell — the cell now disagrees with its own formula, and
no amount of comparing cells tells you which one is right. You never fix it by
editing the cell.

**1. The chain is the only truth — everything else is derived.** The block
chain is the one immutable input; cursors, coins, verdicts, and commitments
are recomputable views of it. 2026-06-11's boot crash-loop was a tip
*installed* (3143175) above what the validated evidence supported (frontier
3141533) — a typed-in number, not a derived one. Both live wedge classes that
day were the same defect: state installed instead of derived.

**2. One writer per fact; everything else is a formula.** Each fact has
exactly one authoritative writer inside one transaction; every other encoding
is read-only derivation. node.db `utxos` has 4 independent writers today (§2)
— that is why coin tears were representable at all. Invariant B made the tear
check a formula over utxo_apply's OWN co-committed log instead of a second
copy **[LANDED `c8018a388`]**; demoting the mirror itself is
**[IN FLIGHT `refactor/derive-coins-best-demote-mirror` — implementation
complete on the branch: `reducer_frontier_derive_coins_best` (gated on
`coins_kv_is_proven_authority`) is the decision-path authority; the boot
gate, restore anchor, CSR counts, Guard A clamp, and orphan-mirror detector
all derive; node_state `coins_best_block` + the `utxos` mirror are labeled
caches; serve-side stays plan step 5]** (canonical plan steps 5–6).

**3. The sealed domain and the working window.** Derived state splits at a
finality boundary: at or below it, state pinned by checkpointed SHA3 UTXO
commitments, recomputable from genesis and continuously re-proven; above it, a
working window of cursors, stage logs, and coin deltas that is explicitly
disposable. The compiled checkpoint h=3,056,758 (sha3 `00e95dbd…`) is the
degenerate seal today; the rolling seal ring above it is **[DESIGNED —
roadmap item 4 in `docs/work/tenacity-roadmap.md`]**.

**4. One recovery verb: rebuild the window, never repair it.** Any window
anomaly — coin tear, height splice, stale verdict, integrity mismatch — gets
one remedy: discard the window, reset to the last seal, replay forward
(a ≤~2,100-block span, seconds at measured replay throughput), recompute the
commitment, resume. The 2026-06-11 record is the proof: every recovery that
worked was a recompute (crash-only auto-reindex `706a7c00a`, cold-import);
every one that failed was a repair (the anti-rewind floor refusing a
consistent rollback target → 6 h crash-loop; depth-3 poison-rewinds
oscillating 8×). `window_rebuild` is **[DESIGNED — roadmap item 4, upgrading
the old ladder-deletion item: the verb is the mechanism that makes each
deletion safe]**; auto-reindex-from-anchor is its landed degenerate form.

**5. Recompute fixes bugs — fix the code, re-derive the state.** When a wrong
formula has written wrong cells, the fix is the formula plus a recompute of
the column, never hand-editing cells. The height-splice fix derives heights
from parent links and hash-binds verdicts, so spliced rows are re-derived
rather than patched **[LANDED `c572def48`, merged `600efd53b`]**.

**6. Verify against the chain, not against text or mirrors.** Ground truth is
the canonical chain's own bytes; reference source text and sibling copies are
lossy proxies. Text said 102,000 bytes was the tx maximum; the chain holds a
125,811-byte tx at h=478544, and trusting the text FATALed genesis replay
(I4, F6). Trusting a lagging mirror manufactured the false coin-tear the same
day — both cured by checking the derivation, not the copy.

---

## 1. The information-theoretic invariants

These are the laws. Every wedge, crash-loop, and silent hole in project history
traces to violating one of them.

**I1 — Single source of truth.** Each fact has exactly one authoritative
encoding. For chain progress that is the stage cursor + logs + coins inside
progress.kv, committed as one `BEGIN IMMEDIATE`
(`lib/storage/include/storage/coins_kv.h:6-13` states the theorem: two WAL
databases have no atomic cross-commit; a crash drifts copies apart and no
forward path realigns them).

**I2 — Derive views, never install state.** Everything that is not the source
is a projection, recomputed from the source, never written independently. H*
is the model: pure-derived, read-only
(`app/jobs/include/jobs/reducer_frontier.h:56`). Invariant B applied this to
the coin-tear — derived from utxo_apply's OWN co-committed log, not the
lagging frontier **[LANDED `c8018a388`]**.

**I3 — Divergence unrepresentable.** When copies CAN diverge, repair code that
guesses between them becomes the bug (this session: the anti-rewind floor
refused a consistent rollback target → 6h systemd-FAILED crash-loop). The cure
is a write-time invariant at a chokepoint, never a new repair module.
**Standing rule: new wedge = new write-time invariant, never a new repair
rung.** A PR adding `*repair*|*recover*|*heal*|*reconcile*` code must cite the
invariant that makes the divergence unwritable, or be declined
**[PROPOSED — not yet a lint gate in `docs/DEFENSIVE_CODING.md`]**.

**I4 — Verify against the chain, not text.** The canonical chain is the only
ground truth for consensus; reference source code is a lossy proxy.
Precedence: **chain > deployed zclassicd behavior > zclassicd source text**.
Proof it matters: `lib/consensus/include/consensus/consensus.h:23-29` copied
`MAX_TX_SIZE_AFTER_SAPLING=102000` from zclassicd text with the safety claim
as a prose comment; the real chain has a 125,811-byte tx at h=478544 — the
first of 413 oversize txs through h=1968856, max 1,922,197 B (mined under the
2 MB rule; zclassicd tightened without grandfathering and cannot resync its
own chain). Result: genesis replay FATAL + forward reducer stall. Fix:
empirical {txid,size} allowlist, block context only, mempool stays strict
**[LANDED `ccc7fbbfa`, merged `b0c0b4f9a`; proven by full genesis replay
through the grandfather range, zero rejects]**. Doctrine amendment to
CONSENSUS_PARITY_DOCTRINE.md **[PROPOSED]**.

**I5 — Crash-only.** Every derived state is rebuildable from the source; every
recovery is bounded, automatic, and terminates in SERVING. The process may die
at any instruction; the boot path must converge without an operator. Landed
pieces: crash-only auto-reindex instead of FATAL crash-loop
**[LANDED `706a7c00a`]**; never-give-up unit (`StartLimitIntervalSec=0`,
stepped backoff) **[LANDED `0b45e93a5`]**. NOT yet crash-only: the reindex
epilogue itself (§4, F3).

**I6 — Measure the real distribution.** test_parallel green is a regression
floor, not a liveness proof. 0 of this session's 4 live failures were
catchable by any gate as configured (`0/409 green` coexisted with `node down
6+ hours`). Gates must sample real crash/import/repair histories and real
chain content (§5).

---

## 2. The divergence surface (audit of progress-truth encodings)

~3 facts, ~16 named encodings across 6+ separate stores (4 SQLite WAL
databases — progress.kv, node.db, utxo_projection.db,
block_index_projection.db — plus LevelDB and the block_index.bin flat
file), no atomic cross-commit between stores. Verdicts are from
`docs/work/canonical-frontier-derived-state-plan.md` Phase 1/2.

### Fact: the UTXO set (5 encodings)

| Encoding | Writer(s) | Authority verdict |
|---|---|---|
| `coins` table in progress.kv (coins_kv) | reducer, inside the stage txn (`lib/storage/src/coins_view_sqlite.c` via coins_kv) | **CANONICAL** |
| node.db `utxos` | **4 independent modules**: `lib/storage/src/coins_view_sqlite.c:681`, `lib/net/src/fast_sync.c:723`, `app/services/src/snapshot_apply.c:117`, `app/models/src/database.c:143` | duplicate — demote to read-only derived view or delete [IN FLIGHT `refactor/derive-coins-best-demote-mirror`] |
| utxo_projection.db | event_log fold | reduce to one-shot boot seed, then delete [PROPOSED Phase 2] |
| LevelDB `chainstate/` | legacy import/copy paths | demote to read-only import source [PROPOSED Phase 2] |
| created_outputs index | reducer (co-committed) | derived, in-txn — OK |

### Fact: the applied frontier / best block (6 encodings)

| Encoding | Writer(s) | Authority verdict |
|---|---|---|
| utxo_apply stage cursor (progress.kv) | reducer txn | **CANONICAL** |
| `coins_applied_height` (node_state) | co-commit path | derived; goes stale across reindex (§4 F3) |
| `coins_best_block` (node_state) | coins view + recovery paths | duplicate anchor — derive, then retire with node.db utxos [IN FLIGHT `refactor/derive-coins-best-demote-mirror`] |
| LevelDB `'B'` best-block key | import/copy paths | read-only import source [PROPOSED] |
| `tip_height`/`tip_hash` (node_state) | tip promote | derived view |
| cached SHA3 utxo commitment | commitment service | derived; deleted-not-recomputed by reindex today (§4 F3) |

### Fact: the block index (5 encodings)

| Encoding | Writer(s) | Authority verdict |
|---|---|---|
| block_index.bin + SHA3 sidecar | flat-file writer | load-bearing today |
| node.db header cache | header sync | 4→2 replicas target [PROPOSED Phase 2] |
| LevelDB index tree | import paths | read-only import source [PROPOSED] |
| block_index_projection.db headers | event fold | retire [PROPOSED] |
| event_log `EV_BLOCK_HEADER` | reducer log | log-of-record |

**Carrying cost of letting these diverge:** ~16,254 LOC of production repair
ladder + ~11,005 LOC of tests pinning repair behavior (~27,300 LOC total).
Step-7 of the canonical-frontier plan deletes ~4,533 production LOC; the
verified-install invariant (§4 F5) makes another ~11,700 LOC dead. Ordering
constraint: zero-callers is the state AFTER plan steps 1–6 —
`utxo_recovery_heal_torn_legacy_coins_anchor` is still live-called from
`config/src/boot.c:1708`; deletions cannot run out of order.

---

## 3. The sync-speed floor

Trusted bootstrap's information floor is *headers + UTXO snapshot*. We measure
at the floor today; remaining work is recipe-safety, not throughput.

**Measured (2026-06-11, verified hash-identical vs zclassicd at multiple
heights, warm-reboot-proven):**

| Step | Measured |
|---|---|
| `--importblockindex` (headers from RUNNING zclassicd) | 3.14M headers in **60–74 s** |
| then `-cold-import` boot (UTXO set + delta) | hash-identical tip in **~25 min** |
| Trustless alternative: FlyClient sampling | ~60 s, ≥150-bit vs minority adversary |

**The proven recipe (the ONLY recipe):** `--importblockindex` FIRST, then
`-cold-import` boot. **`-cold-import` alone is a footgun**: it leaves a 3.1M
header hole (headers=960) and pins forever. The footgun must be removed or
auto-composed in code, not documented around **[PROPOSED]**.

Ports (memory previously had these wrong; live-verified 2026-06-11):
zclassicd RPC **8232**, P2P **8033**; zclassic23 RPC **18232**, live P2P
**8023** via the unit's `-port=8023` (the binary's compiled default is 8033,
which the sibling zclassicd owns on this box — never run both on it).

**Targets:** (a) one-command recipe, hole unrepresentable; (b) post-import
completeness invariant (§5 G4) so a fast sync can never be a silently
incomplete one — precedent: a UTXO import once silently dropped the txid
keyspace tail (1,561 records) with plausible row counts and shipped green its
whole life.

---

## 4. The failure-rate budget

Defense in depth: each failure mode is absorbed by the innermost layer that
can; outer layers are backstops; the residue pages the operator via
EV_OPERATOR_NEEDED / Conditions / supervisor — never a silent halt.

| # | Failure mode | Absorbing layer | Status | Residual risk |
|---|---|---|---|---|
| F1 | Coins/cursor tear (false-tear, L2 wedge) | Invariant B: tear derived from utxo_apply's own co-committed log | **[LANDED `c8018a388`]** | Low — class unrepresentable |
| F2 | Boot restore installs tip above validated header frontier (this session: tip 3143175 > frontier 3141533 → FATAL crash-loop) | Invariant A: trust-rooted installs at the single commit_tip chokepoint; `pindex_best_header` a projection; evidence-based floor rewind | **[LANDED `21d177bf9`+`447fa757b`, merged `a2da7e107` — the crash-loop fixture now boots post-restore-integrity CLEAN, zero reindex requests]** | Fixture residue: the coins_applied>hstar coin-tear class — that is F3's territory, tracked separately |
| F3 | Reindex epilogue tear: `-reindex-chainstate` replay never reseeds coins_kv, deletes-not-recomputes the SHA3 commitment, keeps stale utxo_apply/coins_applied cursors (`config/src/boot_index.c:159-297`, `config/src/boot.c:3321-3346`) | Epilogue closure inside the replay's own txn discipline | **[PROPOSED Phase 2 item 6]** | **Medium — recovery currently manufactures the coins_applied>hstar wedge shape** |
| F4 | Unrecoverable boot integrity (any class) | Crash-only auto-reindex instead of FATAL | **[LANDED `706a7c00a`]** | Backstop only; with F3 open this can become an infinite reindex loop — soak gate (§5 G3) must watch it |
| F5 | Silent import hole (1,561-record precedent) | Verified-install: no imported state (LevelDB copy, snapshot, cold-import) becomes authoritative until SHA3-verified + completeness-checked (contiguity, per-keyspace counts vs source); point-in-time half exists in `utxo_recovery_ldb_copy.c` | **[PROPOSED Phase 2 item 5]** | Until built: every import path can ship a hole |
| F6 | Consensus text-vs-chain false-reject (h=478544 oversize tx) | Grandfather scanned real-chain violations; enforce 102000 above | **[LANDED `ccc7fbbfa`, merged `b0c0b4f9a`; live binary predates it — deploy pending]** | Any future predicate tightening re-opens the class — only the extremals gate (§5 G2) closes it permanently |
| F7 | Process death mid-write | progress.kv single `BEGIN IMMEDIATE` (cursor+log+coins+frontier) | **[LANDED]** (coins_kv design) | Low inside the boundary; nonzero across engine boundaries until §2 deletions land |
| F8 | Service crash-loop exhausts systemd | `StartLimitIntervalSec=0` + stepped restart backoff | **[LANDED `0b45e93a5`]** | None for "stays down"; converts to retry-forever — needs F4/F3 to converge |
| F9 | Stalled-not-crashed (silent halt) | supervisor liveness tree (deadlines, stall_fires) + Conditions + watchdog | **[LANDED]** (`lib/util/include/util/supervisor.h`) | Detection only — remediation is the Condition's job |
| F10 | Concurrency recurrence (realloc-array UAF fixed 3×, cached-stmt race 2×) | Lint gate banning lock-free reads of growable arrays + one locked primitive each | **[PROPOSED]** | Point-fixed today; a 4th point fix is not acceptable |
| F11 | Quadratic path under a lock (operationally identical to a wedge; 6 fixed) | Soak (§5 G3) + supervisor deadlines | partial | New O(n²) remains possible |

**Page-the-operator residue** (everything the layers do NOT absorb): consensus
divergence vs zclassicd at identical height (parity break — never auto-heal),
exhausted auto-reindex (reindex loop count threshold), disk-full/IO error,
and any supervisor child in stall_fires escalation. These must surface as
EV_OPERATOR_NEEDED + Condition, with the node held SERVING-degraded where
safe — never a silent halt, never a FATAL loop.

---

## 5. Standing verification institutions

The gates ARE the product, and they are the only honest verification of the
refactor — **they land FIRST**. Budget: ~1–1.5 h/night + ~30 min/week on the
existing 32-core box. Each must emit a verdict sentinel (never exit-0 alone —
`a91770f88` passed build+lint+tests with its new seed path never executing;
only repro-on-copy proved it fires).

| # | Gate | What it samples | Cost | Status |
|---|---|---|---|---|
| G1 | **Full-history replay canary** — HEAD binary, scratch datadir, bootstrap to anchor 3,056,758, replay all bodies through the reducer; assert zero consensus rejects, zero internal_errors, final commitment == zclassicd `gettxoutsetinfo` | Real chain content vs every consensus predicate; the reducer end-to-end | nightly ~45 min anchor-bounded; weekly ~6 h full-genesis | **[PROPOSED]** — would have caught 3 of this session's 4 failures |
| G2 | **Chain-derived golden extremals** — one-time C scan of the real chain for per-era maxima (tx size, block size, sigops, version/locktime/expiry); every bounded consensus predicate must ACCEPT every pinned extremal | Text-vs-chain class (F6), permanently, for all future tightenings | one-time ~20 min, then free | **[PROPOSED]** |
| G3 | **Crash-boot soak** — reflink-copy the frozen wedge fixture (`~/.zclassic-c23-postrestore-wedge-20260611`, 12 GB) + a live snapshot; boot HEAD; kill -9 at randomized phases (boot, mid-advance, mid-reindex); reboot; assert SERVING, tip ≥ floor, no FATAL, no loop, time-budgeted; ~10 cycles | Off-diagonal torn multi-store states from real histories (the space hand-enumerated unit tears cannot cover) | nightly ~40 min | **[PROPOSED]** |
| G4 | **Recipe smokes** — (a) regtest reindex smoke: seed 50 blocks, `-reindex-chainstate`, assert tip + rowcount parity + recomputed==served commitment + SERVING (~3 min, every push); (b) always-on post-import completeness invariant: contiguity genesis..tip, zero nBits==0, per-keyspace counts vs source LevelDB (seconds); (c) weekly cold-sync canary: full §3 recipe to hash-identical tip (~30 min) | The recovery and bootstrap paths as actually exercised | per-push / always-on / weekly | **[PROPOSED]** |
| G5 | **Merge the mvp-spawn workflow** (`c086c5136`, written and verified green, never merged) so something executes code on push; carry verdict-sentinel discipline into every canary | Push-time execution (today's GitHub CI = 2 diff-scan workflows that never run code) | — | **[NOT MERGED]** |

**What exists today (honest):** 36 lint gates (source-text greps), 409
hermetic test_parallel groups (synthetic regtest 48,5 fixtures), `make ci`
(policy-forbidden from starting a node), test-crash (guaranteed-SKIP —
`ZCL_CRASH_DATADIR` unset). All valuable as a regression floor; none sample
the live failure distribution. That is the gap G1–G5 close.
