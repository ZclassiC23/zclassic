# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.

State at handoff: main worktree. Verify HEAD with `git status --short --branch`.

---

## 2026-06-12 (latest, eve) — wave 4 MERGED; live COIN TEAR found; soak/deploy split; redeploy = wipe + cold import

**Wave 4 (3 parallel worktree implementers → adversarial review → merged
`33838e421`, union gate 0/422 + lint all-pass, pushed):** tenacity-roadmap
items 3, 5, and 4-M1.
- **Item 3 — reindex epilogue** (`reindex_epilogue.c` + boot_index wiring):
  after a clean from-genesis replay, ALL durable post-reindex state is
  derived in one ordered commit (coins_kv reseed, SHA3 commitment recompute,
  reducer cursors + coins_applied_height + tip_finalize anchor clamped to
  the replayed tip). Without it the recovery path itself manufactured the
  coins_applied > hstar coin-tear wedge. Epilogue failure PAGES and leaves
  the reindex sentinel pending (retries do NOT advance the attempt budget —
  the page is the backstop).
- **Item 5 — replay canary** (`tools/scripts/replay_canary.sh` + Makefile
  `replay-canary-anchor`/`-genesis` + systemd units): scheduled band replay
  on a scratch datadir (`~/.zclassic-c23-canary-scratch`, NOT /tmp) with an
  atomic PASS/FAIL sentinel — closes the "set parity had no continuous
  gate" hole the coin tear exposed. The i5 reviewer caught a vaporware
  staleness guard → made real + elapsed-time band in `f89a3b00c`. First
  runs + the seeded known-bad RED proof must pass BEFORE green nights
  count; reindex smoke is KNOWN BLOCKED on regtest durability (documented
  SKIP). Opt-in node-spawning harnesses landed in `82ade88f2`.
- **Item 4-M1 — seal** (`seal_kv` ring + candidate hook in
  utxo_apply_stage in-stage-txn + ratifier in rolling_anchor_service +
  row-8 page): every 1000-height grid point gets a candidate seal (height,
  block_hash, coins_sha3, utxo_count, supply); ratified one anchor-window
  later. Prune lands DARK (`SEAL_PRUNE_ENABLED 0`). nullifier_sha3 all-zero
  in M1 (activation gap). `zcl_state subsystem=seal`. First live seal
  expected when coins_applied crosses grid 3,146,000.
- Repair-ladder census: 15,227 LOC (the M4 deletion target). WIP branch
  `fix/crashonly-verb-selection` DROPPED after review (ideas mined; archived
  tag).

**LIVE COIN TEAR (found ~22:00 UTC by the wave-4 boot-smoke; PREDATES
wave 4; root cause OPEN):** the live node wedged at 3,145,366 — UTXO set
has 13 extra / ≥1 missing coins vs zclassicd, surfacing as
`prevout_unresolved` on canonical 3,145,367 → `block_failed_mask_at_tip`
paging. The wedged datadir is preserved at
`~/.zclassic-c23-cointear-fixture-20260612` (KEEP — it is the root-cause
fixture). A restart cannot fix a torn set; the remedy is a WIPE +
fresh two-step cold import on the merged binary. Set parity had NO
continuous gate before this — the wave-4 canary (item 5) is that gate.

**SOAK/DEPLOY SPLIT (owner-approved):** rapid deploys are the norm on the
main node; MVP-C6 soak time accrues on a dedicated `zclassic23-soak`
linger service — pinned binary `~/.local/bin/zclassic23-soak` (= merged
`33838e421`, never touched by `make deploy`), fresh datadir
`~/.zclassic-c23-soak`, P2P 8043 / RPC 18242, no -tor, addnodes
127.0.0.1:8033 + :8023. Its bootstrap (started 22:43 UTC: step-1 imported
3,138,616 headers in 88 s, then normal boot) IS the run-4 cold-import
real prove — and both cold-import lattice fixes (`41de86064` seed
utxo_apply row + `7a28501e5`) are in the merged binary's ancestry, so
this bootstrap exercises them.

**RESOLVED (2026-06-13 ~00:20 UTC): both gates passed; the live node is
REDEPLOYED and CURED.**
- **Soak gate (run-4 cold-import prove)**: converged, 11/11 hash probes
  MATCH, same-bestblock `gettxoutsetinfo` parity EXACT (txouts 1,344,643;
  supply 10,395,252.99498115). The wave-3 lattice fixes held end-to-end.
- **Live redeploy**: wipe + two-step recipe on the merged binary →
  healthy:true at tip 3,145,475, gap 0, hash probes 6/6 MATCH, set parity
  EXACT, operator_needed clear, 5 healthy peers / 5 groups, zero blockers.
  The block_failed_mask_at_tip paging is GONE.
- **Peer-floor gotcha (cost ~1 h, now durable)**: a localhost-only addnode
  set can NEVER converge a cold import — announce-stream holes at the seed
  boundary need body_fetch getdata, which the anti-eclipse floor (≥3
  healthy peers) correctly gates. Both units now carry external addnodes;
  do NOT lower the floor. The 20-min tip-stall watchdog recycle is what
  re-dials the unit addnode list — it cleared both nodes' final stretch.
- **Tor identity note**: "identity outside datadir" was WRONG — dynhost
  mints a fresh ephemeral onion every boot; a wipe loses nothing. The
  stale-address defect that hid this is fixed (`2e5b9cd23`, live-proven:
  reported == tor.log newest).
- **Canary**: hermetic verdict proofs 6/6 (pass + 5 typed FAILs on the
  real script); live RED proven (fresh atomic FAIL sentinel from a real
  spawn); RPC-ready window fixed (`3cde4fb52`); first live green
  from-anchor run in flight. Timers (deploy/examples) install after PASS.
- **Next**: seal watch at grid 3,146,000 (~12-22 h; check `zcl_state
  subsystem=seal` for candidate+ratify; prune stays DARK).

---

## 2026-06-12 — wave 3 shipped + the copy-prove caught a permanent cold-import wedge BEFORE it reached live

**Wave 3 (5 parallel worktree agents → adversarial refuters → merged
`08cde64ca..128ac402f` + fix-forward `6ae0a837a`/`4aaf24c3d`):**
- **#32** block_index.bin is now ONE atomic file (BIIE v2 embedded SHA3
  header, one rename + dir fsync; legacy two-file still loads).
- **#31** tip_finalize +1-convention unification (see below — this is the
  one the copy-prove kept finding more of).
- **#34** step_finalize read-decide-move window closed under
  progress_store_tx_lock; stall log names the blocked class. long_op
  deletion ABORTED correctly — live caller in sync_state_stuck.c.
- **#36** coarse block-hash parity now fires AT TIP; a mismatch LATCHES
  `parity_bh_drift_detected` (operator-cleared only) — separate key from
  the SHA3 path so a later confirmation can never silently un-page it
  (refuter-found missed-page defect, fixed + production-config test).
- soak_attestation service: 60s JSON evidence log (MVP criterion 7),
  live at `~/.zclassic-c23/soak_attestation.jsonl`, `zcl_state
  subsystem=soak`.

**The copy-prove story (read memory
`project_l1_bump_anchor_collapse_hold_wedge_2026-06-12`):** the #31
fixture prove caught TWO more lattice members that would have pinned any
future cold import forever:
1. `c796673c0` — the L1 reconcile's old `floor = hstar+1` bumped the
   correctly-seeded tip_finalize cursor by one → fresh seed anchor
   REJECTED by `reducer_anchor_candidate_ok` → trusted anchor collapsed
   to the compiled checkpoint → I4.3 sweep latched the chain-linkage
   HOLD over the log-less import region → step_finalize FATAL loop.
   Fix: own-frame clamp band [hstar, hstar+1] capped at coins
   applied-through; T9 + 3 sibling pins shifted (the pins ENCODED the
   bug — proven, which is the bar for changing them).
2. `41de86064` — step_finalize at cursor C consumes the utxo_apply row
   AT C; on a cold import nothing ever writes row H (the import IS block
   H's apply authority), so the stage idled forever on uv_row_missing.
   Fix: the seed stamps its own ok=1 status='anchor' utxo_apply row at H
   (INSERT OR IGNORE, after the upstream stamps — FIX-3's empty-log
   prong reads utxo_apply_log emptiness).

**Deployed live:** `c796673c0` at 19:18 UTC — healthy:true at tip within
96s, soak attestation accumulating, no deadlock through the canary
window. The seed-row fix (`41de86064`) only affects future cold imports;
deploy it with the next regular deploy after the run-3 fixture prove
converges. Diagnosis chain for any future "tip pinned at seed": HOLD
refusals → `chain_linkage_check` refuse_from → which check set the hold
→ if window_sweep I4.3, check anchor acceptance vs tip_finalize cursor
frame; if TF_BLOCKED_UV_ROW_MISSING, check the utxo_apply row at the
cursor height.

**Rescued WIP (review before landing):** local branch
`fix/crashonly-verb-selection` (`2bf708100`) holds ~390 lines of
uncommitted work found in a stale /tmp worktree during cleanup — the
crash-only auto-reindex backstop learning cold-import seed provenance
(reconcile, not -reindex-chainstate, for imported-authority coin sets)
+ a new test_boot_crashonly.c. Ungated and 44 commits behind main;
review against the wave-3/#31 changes before any merge.

**Perf note (backlog):** cold-import backfill routes 64-block getdata
batches to slow REMOTE peers while the co-located zclassicd firehose
idles (~2-4 blk/s observed); request routing should prefer localhost.
Also: `contradiction_frozen` pages once per cold-import boot
(`active_tip_ancestry_unlinkable (h=seed)`) — pre-existing transient,
clears on the first commit; candidate for the RUNBOOK benign list after
one more observation.

---

## 2026-06-12 — stability wave 2: a green node finally REPORTS healthy; parity oracle armed; one self-inflicted deadlock root-caused and fixed same-session

**Outcome:** task #33 closed — the persisted chain-evidence active-tip
now follows the live tip, so an at-tip node reports `healthy:true` and
`active_tip_hash_mismatch:false` (verified live across 5 block publishes,
67/67 RPC probes, 0 failures). The MVP-C8 parity oracle is ACTIVE against
the co-located zclassicd (`zcl_state subsystem=utxo_parity`:
`active=true`, source `zclassicd-coarse`, structurally unable to
false-page; checks fire only at reorg-safe heights — task #36 tracks
making them fire at tip). RUNBOOK gained the "Benign log patterns at
tip" section (now incl. the honest `tip_stale` flap on slow blocks).

**The hard lesson (read `feedback_reducer_drive_lock_order_law` in
memory + the header of `app/services/src/chain_evidence_live_advance.c`):**
the first #33 deploy (`873ba9955`) ran the evidence follow on the
reducer drive, which holds the coins_kv authority mutex; the health
path takes csr->lock THEN coins_kv — the inverted edge ABBA-deadlocked
the live node within two blocks (RPC dead, P2P alive). Fix
(`b33898178`): the drive only stamps a leaf-mutex pending slot
(`chain_evidence_note_finalized_tip`); `node_health_collect` drains it
(init → drain → snapshot) with the established lock order. NOTHING on
the drive may ever take csr->lock or construct a
chain_evidence_controller.

---

## 2026-06-12 (later) — task #30 ROOT-CAUSED + FIXED: the served tip no longer trails the network by one block

**Outcome:** the "single-block header lag" (task #30) was never a
header-fetch problem — headers, bodies, scripts, proofs and coins were
all current within seconds of each block's arrival. The served tip
trailed because **two +1 re-anchor paths skipped the pending tip→tip+1
finalize transition after every publish**, so block N could only be
published when N+1 arrived (one full inter-block interval late, every
block; live forensic: 3144858 arrived 11:05:34, published 11:17:00).

**THE INVARIANT (the whole fix):** the tip_finalize cursor floor equals
the served tip's OWN height T — the T→T+1 transition is always the
stage's to finalize. Nothing may stamp T+1.

**LANDED on main:**
- `67062bbf6` — authority anchor target = height (was height+1; fired
  via `chain_set_active_tip` → `set_authoritative_tip` on EVERY tip
  commit). Plus the latent splice-class kill: new
  `tip_finalize_stage_resolve_durable_tip` (convention-aware, the
  returned height always owns the returned hash) replaces the naive
  `tip = cursor−1` + raw `finalized_tip_at` read in `rebuild_seed_tip`,
  `seed_tip_from_finalized` AND the chain_state_validator Case-3b
  AGREE (which previously matched anchor rows only and fell through to
  the Case-4 reset on finalized-row shutdowns); ingest read-back via
  `block_hash_at`; `ensure_authority_anchor_row` never downgrades a
  finalized row. Regression block "noskip" (15 asserts) in
  test_tip_finalize_stage.c.
- `967437452` — the per-ingest runtime re-seed (reducer_ingest_service)
  gated on `cursor < tip` (was `< tip+1`): it was dormant under the old
  anchor and re-introduced the skip on every at-tip ingest once the
  anchor was fixed (re-proven live: 3144895 arrived 11:50:30, published
  11:53:40 with 3144896).
- `aa91b0454` — tooling: Makefile BUILD_COMMIT appends `-dirty` on
  uncommitted tracked changes; `build/bin/sqlq` (read-only
  vendored-sqlite CLI — no python, no sqlite3 CLI on the host; it read
  the live tip_finalize_log and exposed the alternating finalized/anchor
  lattice that was the smoking gun).

**Gates:** test_parallel 0/415 (twice), `make lint` all-pass. Binary
`967437452` deployed 12:06:56 UTC, boot clean at the pre-restart parity
height. **LIVE-VALIDATED 12:14–12:15 UTC:** blocks 3144908 and 3144909
each published at the SAME 5 s watcher tick they arrived at zclassicd
(previously one full inter-block interval late, every block); 6/6
hash-identity probes MATCH; tip_finalize_log now shows consecutive
finalized rows (the alternating finalized/anchor lattice is gone).

**Open (task #31):** two cosmetic +1s remain, each costing one late
block per EVENT (not per block): the boot clamp floors the cursor at
coins_best+1 (one late block per restart), and
`tip_finalize_stage_seed_anchor` stamps height+1 (one per cold-import —
changing it must conform `reducer_anchor_candidate_ok`, the H*
trusted-base scan, and be copy-proven on a cold-import fixture). The
resolver tolerates both conventions, so #31 can land incrementally.

---

## 2026-06-12 — defect #10 FIXED + LIVE-PROVEN: restarts keep the connected extent (projection top-up); cold-import drop-in REMOVED; sidecar crash window killed

**Outcome:** task #29 (defect #10) closed. A restart no longer drops the
active chain to the stale flat-file floor and re-chases the window —
copy-proven on a live-datadir fixture (boot after kill-9 came back at
the FULL connected extent h=3,144,836 in 58 s, 6/6 hash probes vs
zclassicd MATCH) and live-proven twice by the deploy restarts (boot
anchored at the pre-deploy tip, gap 0 within minutes, 8/8 probes
MATCH). The `-cold-import` firstboot drop-in is REMOVED from the unit
(standing rule satisfied); the unit is clean.

**Root cause (defect #10):** connect-time block-index state (HAVE_DATA,
nFile/nDataPos, nTx) was mutated in memory only; the durable record is
the EV_BLOCK_HEADER event log → block_index_projection, and **no
normal-boot path ever read the projection back**. Compounding it, no
live path ever set `pindex->nTx`, so even the projection rows carried
n_tx=0 and nChainTx propagation died at the window floor.

**LANDED on main:**
- `c8a99618e` — THE FIX: `block_index_projection_topup()` (new
  `app/services/src/block_index_loader_topup.c`) — unconditional
  raise-only fold of the projection over the loaded map before boot's
  nChainTx propagation (applies HAVE_DATA/positions/nTx/valid-level,
  inserts missing entries with pprev+chainwork, hash-bound disk nTx
  recovery for legacy n_tx=0 rows, refuses height conflicts loudly).
  Plus: nTx stamped at body persist (reducer ingest + body_persist
  stage); auto_reindex_request sentinel cleared on both verb-refusal
  paths AND on a verified integrity-clean boot (it lingered forever,
  costing a consume→refuse cycle — or a full reindex — every boot).
  New regression group `test_block_index_topup` (22 assertions).
- `2962efc7c` — top-up re-emits recovered nTx so projection rows
  self-correct (the ~7.4k disk preads are paid once, ever).
- `48e96784b` — NEW DEFECT found by the first deploy restart, fixed:
  `save_block_index_flat` renamed the fresh 541 MB body then died
  inside the multi-second post-rename rehash → next boot quarantined a
  GOOD file (self-healed, one boot cycle). Now the SHA3 streams over
  the bytes as written and a new `ssio_write_sidecar_raw()` stamps the
  sidecar ~1 ms after the rename. Lint-gate contract updated to pin
  the stronger shape.
- `test_rebuild_recent` updated to mirror the shipped cap (10000).

**Operational state:** node healthy at tip (gap 0, hash-identical
probes incl. the whole former band), binary `48e96784b` live, unit has
only latency-guard + legacy-sync drop-ins. Guardian monitor armed.
Copy-prove fixture preserved at `~/.zclassic-c23-defect10-copyprove`
(13 GB — delete when comfortable). Casual restarts are now SAFE.

**Open:** wave-3 repair-ladder deletion (window_rebuild + ~1,450-1,550
LOC) and the legacy coins.db best-block view demotion remain future
work (wave-2's list). Gates: build clean, lint all-pass, test_parallel
0/415.

---

## 2026-06-11 — live node AT TIP on the all-fixes binary; 5th defect (header band hole) fix IN FLIGHT; wave-2 derived-coins_best MERGED

**Outcome:** the 6h outage is over and the live node tracks the
zclassicd tip on binary `600efd53b` (hash-identity at every probed
height incl. shift probes, gap ≤1, 5h+ uptime, NRestarts=0). FIVE
defects root-caused this session: systemd's start-limit giving up
mid-crash-loop, the boot FATAL-instead-of-recover path, the FR-3
oversize consensus regression, the header-splice authority bug
(MERGED `600efd53b` and deployed), and — found by the fail-loud stack
on the final deploy — the **cold-import header band hole** (fix IN
FLIGHT, branch `fix/header-band-backfill`). Wave 2 of the canonical
refactor also MERGED (`5fc5d22cc`): coins_best_block is DERIVED from
the reducer's durable logs, the utxos mirror demoted to a rebuildable
cache (fixture proof: restores 5,798 blocks higher than the installed
anchor; poisoned cache ignored with zero FATALs).

**LANDED on main:**
- `706a7c00a` — boot crash-only auto-recovery: integrity FATAL
  crash-loop → bounded auto-reindex sentinel (max 3/anchor, then
  page). PROVEN on the real wedge fixture (boot1 writes sentinel +
  exits clean; boot2 consumes + replays).
- `0b45e93a5` — never-give-up unit: `StartLimitIntervalSec=0`,
  `RestartSec=5` / `RestartSteps=8` / `RestartMaxDelaySec=600` (the 6h
  outage was systemd's start-limit giving up mid-crash-loop).
  INSTALLED live.
- `b1a335638` — docs: TENACITY.md + CLAUDE.md guardrails +
  `docs/work/tenacity-roadmap.md` (information-theory audit: ~3
  progress-facts encoded ~16 ways across 6+ stores; ~16,254 LOC repair
  ladder; install-vs-derive dominates the bug history; 0 gates sampled
  the live failure distribution).
- `b0c0b4f9a` (merge of `fix/consensus-oversize-grandfather`,
  `ccc7fbbfa`) — THE FR-3 OVERSIZE CONSENSUS REGRESSION FIX.
  `f8592c386` had copied zclassicd's TEXT
  (MAX_TX_SIZE_AFTER_SAPLING=102000), but the canonical chain holds
  **413 oversize txs** (heights 478544..1968856, max 1,922,197 B at
  h=685036 — found by a COMPLETE empirical scan: every height
  0..3143532 frame-walked + oracle-compared). The old binary FATALed
  any full validation at 478544 and also stalled the forward reducer.
  Fix: txid-keyed grandfather table (recomputed txid, block-context
  only; mempool + fresh blocks stay strict 102000 = zclassicd live
  behavior). VALIDATED by a full genesis replay through the whole
  grandfather range, zero rejects.
- `a2da7e107` (merge of `fix/invariant-a-restore-clamp`,
  `21d177bf9`+`447fa757b`) — INVARIANT A: restored tips must be
  DERIVED-backable. Trust-rooted pprev descent to genesis/SHA3-anchor
  required at the single commit_tip choke point; evidence-based
  finalized-floor settle (363 floor_rewind rows on the fixture);
  detached-island promotions refused (3 boot.c bypasses closed). The
  morning's crash-loop fixture now boots post-restore-integrity CLEAN
  (was 1267 holes + 631 mismatches UNRECOVERABLE), zero reindex
  requests, serving. Residual on that fixture: the known
  L1-refuse/L2 coin-tear class (coins_applied=3143198 > hstar) —
  tracked, separate.

**LIVE NODE (datadir `~/.zclassic-c23`, service `zclassic23` under the
new unit):**
- AT TIP on binary `600efd53b` since 10:21 UTC (final deploy after the
  splice merge): two-step recipe (`--importblockindex` 3,138,616
  headers/66s, then ONE in-service `-cold-import` boot via the
  firstboot drop-in), seed binding oracle-verified at birth, climbed
  to gap≤1 and held; `-nobgvalidation` REMOVED from the env (oversize
  fix is in the binary).
- ⚠️ DEFECT #5 — header band hole (the reason "solid" is still NO):
  the cold-import installed its anchor at h=3143301 while the
  genesis-rooted header frontier was 3140573; msg_headers' old
  "restarting getheaders from tip" policy discarded every
  frontier-extending batch, so the band (3140574..3143300] was never
  requested. Live symptoms: `getblockhash` "out of range" for the
  whole band, chain_evidence `contradiction_frozen`
  (`active_tip_ancestry_unlinkable h=3143301`), `operator_needed`
  paging the entire boot. Forward progress unaffected — and the
  fail-loud stack did exactly its job (froze the health claim, paged,
  kept advancing on bounded advisory).
- FIX MERGED to main (`0f9ffadae`, branch deleted): header band
  backfill from the contiguous frontier + below-tip batches count as
  progress + non-destructive in-memory closure + startup reconcile
  lifts a stale freeze. COPY-PROVEN on a fresh copy of the live
  datadir (fixture `~/.zclassic-c23-bandhole-fixture-20260611`,
  preserved): boot rewound to the true frontier 3137373, freeze
  lifted, band re-walked with ZERO restart-from-tip kills, 6,712
  bodies backfilled in ~10 min, at-tip with best-block hash identical
  to zclassicd — while the live node (control) still showed the hole.
- ⚠️ TEMPORARY: the firstboot `-cold-import` drop-in
  (`~/.config/systemd/user/zclassic23.service.d/firstboot-coldimport.conf`)
  is STILL INSTALLED — it MUST be removed (+ daemon-reload) BEFORE the
  remediation restart so the node boots normally on the existing
  datadir (the copy-prove rehearsed exactly that boot).

**NEXT — live remediation (owner approved "deploy when proven"):**
1. Copy-prove the MERGED binary (wave-2 + band fix, `a1fc79c80`) on a
   fresh live-datadir copy — IN FLIGHT (`~/.zclassic-c23-merged-proof-
   20260611`, ports 18024/28233).
2. When proven: remove the firstboot drop-in, `make deploy` (WAL
   checkpoint + unit install + daemon-reload + restart).
3. Expect the rehearsed recovery (~13 min): rewind to true frontier →
   band backfill → body catch-up → at-tip.
4. Verify: band probes (3140574/3142000/3143300) byte-equal to
   zclassicd, healthcheck unfrozen + operator_needed clear, shift
   probes clean at tip. Known cosmetic residual: persisted cec
   evidence is not re-stamped after bulk catchup
   (`active_tip_hash_mismatch` until the next restart) — follow-up
   fix queued.

**Ops notes:** the two-step recipe + unit guardrails are documented in
CLAUDE.md (`b1a335638`). Evidence dirs preserved on disk:
`~/.zclassic-c23-offbyone-evidence-20260611`,
`~/.zclassic-c23-splice-evidence-20260611`,
`~/.zclassic-c23-bandhole-fixture-20260611` (band-fix copy-prove,
healed), `~/.zclassic-c23-wave2-proof` (wave-2 fixture proof),
`~/.zclassic-c23-merged-proof-20260611` (merged-binary rehearsal),
plus the old replay/canary fixtures. The wedge fixture's `blocks/` has pre-existing frame
corruption at h=3115015 (replay stops gracefully there — by-design
page-operator case). Branch hygiene done: origin has exactly ONE
branch (`main`); all stale local/remote branches deleted; `pr` remote
removed; mirror branch cleanup done.

---

## 2026-06-10 — mainnet sync wedge cleared END TO END (branch `feature/sync-fixes`)

**VERIFIED LIVE:** trackb (cold import) went from a wiped datadir to the
LIVE TIP in ONE boot — h=3142304 vs zclassicd 3142305, hash-identical at
3142118/3142200/3142300, straight through the old prevout_unresolved
wedge. tracka (trustless genesis sync) unwedged from its h=6756 freeze
and advances steadily with all eight stages ticking. Six root causes,
each diagnosed empirically (supervisor dumpstate ages, /proc per-thread
CPU+wchan, gdb-as-parent repro, raw leveldb probes, SQL over the
imported set):

1. **Download queue O(n²) under dm->cs** — per-item linear dedup +
   sorted-insert memmove against a queue pinned at its 65536 cap;
   bulk enqueues held the lock for minutes, the supervisor thread
   starved, and the staged pipeline stopped ticking. Now an O(1)
   membership set + one sort-merge per bulk call, evict-highest at
   cap. Also: stall recovery collapsed from 12 full block_map scans
   per invocation to one.
2. **Header sync crawled its own known span** — periodic getheaders
   re-anchored at the ACTIVE TIP and the continuation never skipped
   known headers (tracka: +640 headers/30min at 100% CPU). An
   all-known batch now continues from pindex_best_header; tip-class
   anchors prefer best_header (both gated on heights_repaired).
3. **UTXO import silently dropped the txid-keyspace TAIL** — the
   reader's end-of-range check jumped past the publish of the chunk
   being filled: every txid above FF3779C7... (~1,561 records,
   ~4,400 coins) missing on every import with identical
   plausible-looking row counts. THE original prevout_unresolved
   cause. Proven with a raw leveldb probe (501,273 'c' records on
   disk vs 499,712 imported distinct txids).
4. **gap_fill windowed from the active tip, not the reducer
   frontier** — post-hard-kill boots restore the tip above the staged
   cursors and nothing re-downloads below tip+1: the classic
   tip_finalize>utxo_apply inversion wedge. The window bottom now
   clamps to body_fetch's cursor.
5. **coins_kv empty on the FIRST boot after a cold import** — the
   projection-based boot rebuild runs pre-import against an empty
   projection, so the script_validate prevout resolver had no
   pre-anchor coins: first post-anchor spend fails ok=0, the seeded
   anchor is refused, H* falls back to the compiled checkpoint, tip
   pins at the anchor. The import now seeds coins_kv directly from
   node.db utxos (1,344,557 rows, one ATTACH-copy).
6. **Live-chainstate copy hardened** — cp -a of a live LevelDB can
   tear; the copy is now signature-verified point-in-time (retry
   until no source file changed mid-copy, refuse instead of tear).

Plus: validate_headers window report treats a missing log table as
"no data" instead of LOG_ERR spam. (A seventh fix — full block_map
scans per successor hop in msg_headers — was independently fixed on
this main by 416fddb85 and is not re-applied here.)

Ops notes: track units are transient systemd-run (no auto-restart —
the tip watchdog's self-shutdown leaves them dead; use real units with
Restart=on-failure for soaks). zcl-rpc needs ZCL_DATADIR+ZCL_RPCPORT;
state via `zcl-rpc dumpstate '"supervisor"'`, SQL via
`zcl-rpc dbquery '"SELECT ..."'`. ptrace is yama-blocked: run the node
as a gdb CHILD on a copy datadir, or read /proc/PID/task/*/stat+wchan.

---

## ★★★ 2026-06-09 SESSION CHECKPOINT — security-audit remediation landed (read FIRST)

The Round-1 security audit's chain-fatal findings are FIXED on main, gated by
build + `make lint` (all gates) + `test_parallel` **0/380**. Full disposition
(including two findings refuted with citations: the C-4 retarget half is
FALSE — `bad-diffbits` runs live via `accept_block_header`; M-2's proposed
SIGHASH_SINGLE sentinel would CREATE a fork) is in
**`docs/work/security-audit-response-2026-06-09.md`**.

Landed in this checkpoint:
- **C-1** script push stack-overflow fixed at the source (`script_get_op`
  capacity guard) + sigop-walk parity companion + `-fstack-protector-strong`.
- **C-2** coinbase subsidy ceiling live (`bad_cb_amount` status, genesis
  exempt, zclassicd `bad-cb-amount` parity).
- **C-3** consensus nullifier set live (`nullifier_kv`, atomic with the coins
  commit, rewind-safe). Activation-forward on existing datadirs — typed
  blocker `utxo_apply.nullifier_backfill_gap` records the remaining window.
- **#26 / H-1** per-tx contextual rules wired into `script_validate`
  (`script_validate_contextual.c`) per the verified spec + the `tip_h >= 0`
  correction; transient infra failures persist as resurrectable
  `internal_error`, never permanent rejects.
- **C-5/H-3** encrypted wallet backups (`WALLET_BACKUP_PASSWORD`,
  `--decrypt-wallet-backup`) + operator-private `/api` routes 403'd on the
  clearnet listener.
- `consensus/zclassicd-parity-2026-06-08` (7 parity fixes + the #26 spec)
  merged to main.

⚠️ Deployment remains owner-gated: prove on a datadir COPY first (replay
zero-contextual-reject, nullifier false-positive-free advance) per the
response doc. The prevout_unresolved frontier hole (below) is still the live
sync blocker for the wedged datadir.

---

## ★★ 2026-06-08 SESSION CHECKPOINT — consensus-parity axis (supersedes the wedge framing below for forward planning)

Two agents ran in parallel: **consensus parity + MVP-CI** (this branch) and **Codex** (the forward-sync stall — the live blocker, still in flight).

**MERGED to main (2026-06-09):** branch `consensus/zclassicd-parity-2026-06-08` — 7 zclassicd-consensus-parity commits, each proven history-safe — FR-1 tx-expiry strict `>` (`cbee16138`), FR-2 finality=header-time (`e5b44f25b`), FR-3 tx-cap 102000 (`f8592c386`), miner cap 200KB (`8b0a2af1f`), **#1** CHECKDATASIG block-sigops (`e1d125e42`), **#5** reject coinbase→transparent (`08e48a69e`), audit+#26-spec docs (`a56640ef3`).

**VERIFIED-READY (not landed):** `git merge --no-edit mvp/autonomous-honesty-2026-06-08` — proven conflict-free + 378/378 green on `7e18d9c70` (MVP #5 real file delivery + #4/#7 honesty; node.db schema 19→20). Land at a clean Codex checkpoint.

**#26 (root cause of #2/#4) — designed+fork-safety-verified:** wire per-tx contextual rules into the live reducer at the head of `script_validate_stage.c step_validate`, IBD-gated only on the per-tx call + tip-proximity guard. Spec: `docs/work/contextual-check-wiring-spec-2026-06-08.md`.

**Handed to Codex:** #3 JoinSplit Ed25519 sig (`proof_validate_stage.c`), FR-5 `HaveShieldedRequirements` stub (`coins_view.c:480`), FR-4 sapling-root reject — see `docs/work/consensus-parity-supplemental-audit-2026-06-08.md`. Codex's in-flight WIP backed up to origin `wip/codex-forward-sync-checkpoint-2026-06-08`.

**MVP v1: MRS 0/8** (all 8 = green hermetic slice/proxy; none full). 5 doc honesty discrepancies (all UNDER-claim) incl. **D1**: the `mvp-spawn` CI job gates #1/#6/#7 full-binary proxies on EVERY push/PR (MVP.md wrongly says "opt-in"). Full punch-list + next-dev critical path in memory `project_session_handoff_2026-06-08`.

**Next-dev path:** Codex lands forward-sync → merge honesty branch → implement #26 → fix D1-D5 → then live-gated #3/#6/#8.

---

## 2026-06-08 checkpoint — reducer L1/stale-replay work pushed; live wedge still open

Latest checkpoint from main worktree is the reducer self-heal/stale-replay
repair stack. Local validation before commit:

- `make -j$(nproc)` — clean / already up to date.
- `make test_parallel` — clean / already up to date.
- `build/bin/test_parallel` — **0/378 groups failed**.
- `make lint` — **all 35 gates passed**.
- `git diff --check` — clean.

What is in this checkpoint:

- `script_validate` no longer depends on `-txindex` for normal prevout
  lookup. It resolves in-window prevouts through `created_outputs` and older
  live coins through `coins_kv`, bounded by the physical coin frontier.
- Reducer L1 refill now clamps stale `validate_headers`, `body_fetch`, and
  `body_persist` cursors after clearing holes / hash-split rows, and caps
  `tip_finalize` at the physical coin frontier.
- Stale script replay repair is wired through the reducer repair path and can
  rewind replay cursors back to the coin frontier, backfill `created_outputs`,
  and replay under the fixed binary.
- Owner-gated value-overflow one-shot repair is present in
  `utxo_apply_delta_repair.c`; **`utxo_apply_delta.c` was not touched**.
- Copy-proof isolation avoids seed discovery in connect-only mode so copy runs
  stay isolated.
- `reducer_frontier_reconcile_light` witness now accepts durable reducer cursor
  movement as observable progress, so zero-peer copy repairs do not exhaust as
  `unwitnessed`.

Copy-proof status:

- Last copy run:
  `/home/rhett/.zclassic-c23-COPY-20260608-120105-l1-validate-refill-proof5`.
- It successfully fired the value-overflow repair at **3,132,747**, stale
  script replay repair at **3,132,720**, validate hash-split repair, and body
  refetch clamp at **3,134,302**.
- It then wedged at **3,135,517** with `script_validate_log ok=0
  status='prevout_unresolved'`.
- The missing prevout is
  `bf8ae6840fd8c30fdcb968f60afa85b34bc51fc8e518d87d545a553316a9f6ff:0`.
  Raw block scan found it created at **3,073,765** with no prior spend before
  the failing block; it is absent from this copy's `coins_kv` and
  `created_outputs`.

Next developer:

1. Do **not** cut over. The copy proof has not held through tip.
2. Continue on a datadir COPY only. Do not touch the live service, live
   datadirs, or live ports.
3. The next real fix is a guarded repair for the old missing coin frontier
   hole, most likely a targeted `utxo_apply` rewind/reapply from before the
   creator height **3,073,765** using the existing inverse-delta/reorg
   machinery. Do not make `script_validate` silently ignore this; `utxo_apply`
   also needs the coin physically present.
4. `tools/scripts/soak_assert.sh` now resolves `build/bin/zcl-rpc` from the
   repository root, so the soak assertion script works when launched outside
   the repo root.

---

## ★ LIVE WEDGE — STILL OPEN; P2 (self-heal prerequisite) LANDED; next = L0/L1 H* self-heal (2026-06-07) [PRIOR-SESSION REFERENCE; owner reframed onto zclassicd consensus parity 2026-06-08 — see the checkpoint above]

**The live wedge is NOT resolved.** (The 2026-06-06 `validate_headers` recheck
fix below was real and shipped, but it did **not** clear the live node — the
datadir is **multi-epoch torn**, a deeper class than recheck starvation.)

**Live truth (this handoff):** node serves at **3,134,951**, oracle (`zclassicd`,
RPC 8232) at **3,139,290** → **~4,339 behind and not climbing** (`tip_advance_age`
== uptime). The 8-stage reducer's upstream cursors have raced to **3,138,981**
while `tip_finalize` is stuck at **~3,134,954**. Per-height logs, the 8 stage
cursors, and the legacy `block_index` flags drifted to **different heights across
crash/rewind/replay epochs**, and **nothing reconciles them as a window**.
Crucially: **the coins are consistent** (inverse-delta-complete) — this is a
*flag/cursor view* tear, not a coin-money tear. Self-heal is `operator_needed`
(5 attempts exhausted). ⚠️ **Deploy drift:** the *running* binary predates repo
HEAD; a clean redeploy is owner-gated and won't by itself fix a torn datadir.

**The fix (active driver: `docs/work/self-healing-reducer-plan.md`, 2026-06-07):**
compute a provably-consistent frontier H* from durable state, then sweep-heal the
drifted flags/cursors forward over the (H*..served_floor] window — never rewinding
the consistent coins, never deleting a `tip_finalize_log` row.

- **P2 (DONE, on `main` `c81e69ae0..4b60c1149`, fully proven):** `coins_applied_height`
  is co-committed inside the `utxo_apply` txn on **all** write paths (forward,
  reorg-unwind, poison_rewind) → a contiguous applied-coins frontier that can't
  hide an interior hole. Proven by 2 independent workflow impls (agree) + 3
  adversarial verifiers + `test_parallel` **0/375** + `lint` 35-gate + **chaos 9/9**
  + a kill-9 copy-proof (`tools/copyproof_p2_frontier.sh`) holding the invariant on
  the raw crash image. Offline checker: `make p2_invariant_check` -> `build/bin/p2_invariant_check <datadir>`.
- **L0 (task #11) `reducer_frontier_compute_hstar` — DONE** (on `main` `50dfd1753`):
  pure SELECT-only authority in `app/jobs/src/reducer_frontier.c` returning
  {hstar, served_floor}; anchored at the SHA3 checkpoint **3,056,758** (cold-import
  logs are sparse → contiguity-from-genesis is wrong); C1–C6 of the plan, NULL-hash
  = no-evidence, C4 coin-tear = WARN-only. PURE read-only (no mutation — heal is L1).
  Adversarially reviewed (PASS: read-only purity, checkpoint floor, tear-correctness,
  mutation-sensitivity via 2 revert-experiments); `test_reducer_frontier` 5 topologies;
  build green, `test_parallel` **0/376**, `lint` 35-gate. Follow-up: convert the C2/C3
  per-height query loop to set-based SQL before wiring into a hot/boot path.
- **NEXT — L1 (task #12) `reducer_frontier_reconcile_light`:** a Condition that
  sweep-heals `block_index` flags + clears HAVE_DATA holes (so `body_fetch`
  re-requests) + clamps ONLY the `tip_finalize` cursor — never touches coins.
- **L2/L3 (task #13):** forward-immunity for a *future genuine coin tear* +
  subtraction of the now-subsumed legacy repair code. NOT needed for today's wedge.

**Method discipline (mandatory):** diagnose on a datadir **COPY** only (isolated
ports 18299/18933, `setsid`); never touch the live datadir/oracle/ports; H* ≥
checkpoint 3,056,758; never delete a `tip_finalize_log` row; never lower the
public tip below `coins_best`. See `docs/work/fast-path.md` and memory
`project_tipfinalize_precondition_desync_fix_2026-06-07`.

---

## ACTIVE AXIS (2026-06-05): convergence — "everything the zclassic23 way"

Owner directive this run: *"do EVERYTHING the zclassic23 way — dig into every
file, DRY, good API, document everything, use multiple workflows, commit + push
as you go, make the server amazing."* This is the **architecture/quality** axis
(distinct from the v1/live-wedge mission below — both are real; the owner chose
this one). Full state + gotchas: memory
`project_convergence_axis_2026-06-05.md`; ranked work: `docs/work/convergence-backlog.md`.

**Where it stands — the autonomous-safe convergence drive is COMPLETE** (rounds
2–15, all green build0/lint35/test_parallel 0/371, all pushed; latest run
`88349b39d`→`ea7542cda` fixed ~52 real bugs + ~40 docs):
- 8/8 framework shapes real; `framework_shape_allowlist`=0; lint ratchets at 0.
  File-size debt board: `boot.c` (3618, FROZEN — **never touch**) + `boot_services.c`.
- **The whole non-consensus surface is harvested** (lib/net feature transports,
  every app/ controller/view/model, lib/wallet, lib/rpc, lib/storage projections,
  supervisors, metrics). Real bugs killed this session: a wallet keypool concurrent
  OOB + data-race snapshots, 2 key-material `memory_cleanse` leaks, a 1-byte stack
  overflow, a 4-projection event-skip class bug, a post-broadcast send-failure bug,
  a proven send-path tx/entry leak, 11 P2P-transport NULL-derefs/unchecked writes,
  3 shielded signing-path `signature_hash` checks, 5 UTXO script leaks, 2 realloc-OOM
  NULL-derefs, a double-free, a snapshot-anchor UAF race, a malformed-txid uninit read.
- **Consensus-adjacent layer handled the careful way** (rounds 12–15): strict-bar
  audit → read-only prove-or-refute (skeptical analysts + adjudicator) BEFORE any
  edit. Fixed: bg-validation honesty (no false "verified" when script checks skipped
  for missing undo; surfaces `verification_incomplete`), quorum symmetric tally.
  Refuted with evidence: `header_sync:575` (sanity gate; connect_block is the arbiter),
  coins-commitment no-rollback (derivative), `block_index_db` nFile cast (fails closed).
- **CORRECTION recorded**: `zcl_mutex` is RECURSIVE — two earlier "deadlock" calls
  were overclaims (code stands; framing corrected in git + backlog).

**The proven method (repeat it):** read-only audit Workflow → ranked backlog →
parallel **edit-only** adversarially-reviewed Workflows on **DISJOINT** file sets
→ ONE union gate I run myself (`make -j$(nproc) zclassic23` + `make lint` [35
gates] + `build/bin/test_parallel` [expect `0/371`] + boot-smoke if the boot path changed)
→ commit per logical group → push. Redirect `make` output to a file and grep
`error:|warning:` to save context. Mutating workflows on disjoint files may run
concurrently; verify the union together.

**What remains is owner-gated / repro-on-copy ONLY** (the autonomous-safe drive is
done — do NOT re-audit the swept surface, it's harvested; see
`docs/work/convergence-backlog.md` §12, §12-EXT, Round 8–15):
1. **`block_index_loader.c:376` nChainTx DRY** — real but latent (safe error
   direction; `connect_block` re-validates). The clean fix (call
   `block_index_forward_pass` instead of 3 hand-rolled recomputes) touches the
   **never-touch** `config/src/boot.c:2138` — needs an owner call on how to fix
   without editing boot.c. Boot-smoke on a COPY before deploy.
2. **`boot_services.c` shutdown TU** + **flyclient/MMB block** — boot decomposition
   tail; HIGH RISK (coins.db COMMIT-before-`block_index` fsync), needs a real
   SIGTERM stop+restart on a datadir COPY. Do ALONE.
3. **peer-scoring enum extension** (OWNER-GATED — changes DoS-ban policy). Extend
   the enum first; do NOT do a naive `peer_misbehaving`→`peer_scoring_record` swap.
4. **§12 consensus-DECISION items** (coins_view rollback nit, nFile>INT_MAX reject,
   plus the originals) — each repro-on-copy; most already triaged/refuted.
5. More audits ONLY if a NEW subsystem appears — **STOP fanning over swept code.**

**Gotchas that cost time this run (don't relearn):**
- **Boot-smoke light-copy floor is NOT a regression.** `repro_on_copy.sh`
  (default `--light`, no `blocks/`) rewinds tip 3134303→**3132299** given ≥~90s
  (DEGRADED_SERVING, "Not fatal"). PROVE innocence by running HEAD's binary on the
  same window — it floors identically. A crash or a LOWER floor = real regression.
- `test_make_lint_gates.c` BANS ~90 refactor-scaffold substrings in production
  comments ("byte-for-byte", "verbatim", "extracted from", "code motion", …).
  Name the PURPOSE, not the refactor. Renaming a symbol's home also needs the
  gauges/owner-file asserts in that test repointed.
- `check-observability-pairing` is a compiled tool: a raw `fprintf(stderr,…)`
  must be paired (−3/+6 lines) by `event_emit`/terminal `return`/`// obs-ok:`,
  ELSE route it through `LogPrintf` (not flagged). Editing a file can line-shift a
  PRE-EXISTING fprintf out of its window → newly fails.
- Gate #15/#21 (supervisor) now scope `config/src/`; a moved boot worker that
  spawns a thread must keep its `supervisor_register_in_domain`/contract or carry
  `// supervisor-ok:<tag>`.
- `config/src/*.c` is globbed (`CONFIG_SRCS = $(wildcard …)`) — new boot units
  build with NO Makefile edit.
- Adversarial review EARNS its keep (caught the `coins_alloc` 4th caller, a silent
  peer-ban weight change, an over-broad blake2b doc). Always run it; respect FAILs.

---

## The mission is v1 (not the refactor)

The v1 bar is **[`docs/MVP.md`](./MVP.md)** — 8 operator acceptance criteria;
v1 = MRS 8/8. **THE plan is [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md)**
(MVP-anchored, with the live wedge as priority #1 and the autonomous /
owner-gated / operational critical path).

Honest status: **~2/8 met by hand, 0/8 CI-enforced** (every criterion test
gates on `ZCL_STRESS_TESTS=1`, which `make ci` never sets). Do not trust
`make test_parallel` green as a v1 proof — it runs zero MVP criteria.

The framework/architecture refactor is **~90% done and OFF the v1 path.**
`docs/FRAMEWORK.md` (architecture) and `docs/REFACTOR_STATUS.md` (debt board)
are reference. **Do not jump the queue into refactor work** while v1 buckets
are open.

---

## ⛔ #1 priority — the live wedge

**ROOT CAUSE (verified live 2026-06-05, multi-agent dx + direct probing) — it is
NOT a consensus code bug; the code is behaving CORRECTLY.** The node holds at
tip 3,134,303 because block body **3,134,304 is missing** (`legacy_mirror`
`stuck_reason:"missing-have-data"`) and there is **no eligible body source**:
- **Native P2P is correctly gated out by the anti-eclipse floor.** The node's
  ONLY reachable peer is one local MagicBean on `127.0.0.1:8033` that **flaps**
  (`disconnected=748`, `blocks_received=0`, stuck `syncing_headers`); external
  mainnet peers are TCP-unreachable from this box (`addnode_tcp_failures=1765`).
  A single localhost peer can never satisfy `p2p_minimum_viable`
  (`block_source_policy_runtime.c:161` — needs ≥2 healthy on ≥2 distinct IPv4
  groups, or ≥3). This gate is a **safety invariant — do NOT lower it.** A
  diagnosis workflow proposed lowering the floor to 2; that is **WRONG** (the
  effective floor is already 2 + eclipse checks, and the node has only 1 peer)
  and would weaken eclipse defense. Rejected.
- **The co-located zclassicd is healthy and readable but advisory-only.**
  zclassicd (PID 2273227, up 2 days) serves RPC on `127.0.0.1:8232`
  (`getblockcount`→3,136,562 verified) and `legacy_mirror` reads it fine
  (`reachable:true`). But the mirror is `bounded_advisory_fallback` trust and is
  in **"observing" mode** (`last_error:"local sync primary; mirror observing"`)
  — by design it will NOT authoritatively supply consensus blocks. The
  `rpc-unreachable` blocker label is **stale** (mirror actually reads zclassicd).

So `reorg_detected_total` climbing / `finalized_total=0` / the
"no-header-solution-backfill-required" rejections are all **downstream
symptoms** of the body-source starvation, not independent bugs.

**The two real resolutions are OPERATIONAL / OWNER-GATED (no consensus edit):**
1. **Native peer diversity** — get ≥2 honest, reachable, distinct-IP-group
   native peers so P2P clears the eclipse floor and bodies flow. Blocked here by
   the environment (only one flapping localhost peer is reachable). This is the
   trust-preserving fix.
2. **(Owner-gated policy)** Decide whether a fully-validated, co-located
   zclassicd may *authoritatively* supply bodies when native P2P is
   eclipse-starved — the long-standing advisory→authoritative mirror trust
   decision (see the cutover/single-engine history). Do NOT flip unilaterally.

Housekeeping: the `zclassicd-rhett` **systemd unit is crash-looping**
(`NRestarts=3990`, "Cannot obtain a lock on data directory" — it is a duplicate
fighting PID 2273227 for the datadir lock). Harmless to the oracle but pure
wasted CPU; mask/stop it. It is NOT the running oracle — do not confuse them.

Older notes (still valid as background): diagnose on a datadir **COPY**, never
live (`tools/diagnose_gap.sh`, `docs/work/fast-path.md`); the prior have-data
window-extender wiring was reverted (`481c520b9`) for churning `tip_finalize`;
recovery FSM design in `docs/work/service-state-machine.md`.

---

## Do Not

1. Do not weaken a lint gate or grow a baseline.
2. Do not delete `tip_finalize_log` rows or hand-edit stage cursors.
3. Do not ship a consensus-adjacent fix without a datadir-copy proof
   (`tools/repro_on_copy.sh`). The boot self-heal heals only on a `utxo_sha3`
   commitment match; otherwise it preserves FATAL — never weaken that.
4. Do not stop `zclassicd-rhett`; manage long-running services through
   `systemctl --user`.
5. Do not restore deleted cutover/projection-diff/public shadow tooling.
6. Do not move the local `zclassic23` P2P listener back to `8033`; the active
   dev node is on `8023` to avoid a `zcashd` port conflict.

---

## First 5 Minutes

```bash
git status --short --branch
make lint
touch lib/test/src/test_parallel.c && make test_parallel && build/bin/test_parallel
build/bin/zcl-rpc getblockcount        # live tip — is it advancing?
```

If the node is not running, or the tip is not advancing, record that
explicitly before claiming any live proof. Forward progress on the running
node is the real bar.

---

## Where the detail lives

| Need | Doc |
|------|-----|
| The v1 contract (8 criteria) | [`docs/MVP.md`](./MVP.md) |
| **THE plan** (critical path) | [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) |
| How to execute consensus-critical work safely | [`docs/work/fast-path.md`](./work/fast-path.md) |
| Engineering quality board (41 items) | [`docs/work/FINISH_CHECKLIST.md`](./work/FINISH_CHECKLIST.md) |
| Architecture (canonical) | [`docs/FRAMEWORK.md`](./FRAMEWORK.md) |
| Architecture debt board (off v1 path) | [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md) |
| Directory / file-purpose map | [`docs/PROJECT_OVERVIEW.md`](./PROJECT_OVERVIEW.md) |

Default to subtraction. Prove on a copy before touching the live chain.
