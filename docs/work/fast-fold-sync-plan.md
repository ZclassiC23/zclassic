# Fast-Fold Sync Plan — get the node fully synced (2026-07-14)

> **2026-07-15 correction — historical plan, failed terminal parity gate.**
> `zclassic23-mint-receipt` reached h=3,056,758 but produced 1,354,769 UTXOs
> instead of 1,354,771 and the wrong SHA3 root. It correctly emitted no trusted
> cure. Its deterministic crash loop was stopped after 187 retries with the
> failed datadir preserved. Treat all `[running]`, `EXPORT-READY`, rate, and ETA claims below as
> historical. Preserve the failed datadir, diagnose on a copy, and start a new
> v2 receipt-owning producer only after the parity defect is understood. Live
> node truth and current next actions are in `docs/HANDOFF.md`. Production
> bundle activation is also contained until an independently replay-bound
> receipt outside the self-describing artifact is implemented.

**Owner directive (2026-07-14 night):** "keep going until the node is fully synced,
don't wait on me... use workflows of subagents, get it done the right way, long term,
in parallel... make sure what you need to run is running in a linger service so you
can't lose it... document everything clearly for future Claude." 7-hour autonomous
window granted. This file is the plan of record; update it as state changes.

## The problem (measured, not assumed)

The live daily-driver node (`~/.zclassic-c23`:18232) is **wedged below tip** on
`utxo_apply.anchor_backfill_gap` (verify the live H\* via `zclassic23 status` /
`dumpstate reducer_frontier`; `docs/HANDOFF.md` holds current state):
`sapling_anchors`/nullifiers are empty below the reducer
cursor because the node was seeded from a **coins-only** snapshot that skipped the
from-genesis shielded build. To cure it, some producer must **fold from genesis** building
the complete Sapling/Sprout note-commitment trees + nullifiers, reach the anchor/tip, and
then the live node cuts over to that complete state.

**The fold is the bottleneck.** Measured 2026-07-14 across all producers:

| producer | datadir | height | rate | binary | note |
|---|---|---|---|---|---|
| anchor-mint | `~/.zclassic-c23-anchor-mint` | 1.39M | ~3 blk/s | old | `-mint-anchor-fast`, 5 days, IO-bound old binary |
| mint-fast | `~/.zclassic-c23-mint-fast` | 768k | 5.9 blk/s | old | `-mint-anchor-fast`, old binary |
| **mint-receipt** | `~/.zclassic-c23-mint-receipt` | 364k | **25.9 blk/s** | **new** | FULL validation, fastest, exportable — the durable floor |

**Key finding:** the crypto-*skipping* old producers are *slower* than the crypto-*doing*
new one → **crypto is NOT the ceiling; a structural cost is** (IO/fsync, pprev/ancestor
walk, or tree-append). Confirms the stale note `reference_refold_bottleneck_measured_2026-06-24`
(~50 blk/s structural ceiling). So parallelizing/skipping crypto buys ~2x at most, NOT the
~10x needed for "a few hours." Full sync at 25.9 blk/s ≈ 29h.

## The plan — make the ONE-TIME fold apply-bound, then never pay it again

Information-theory framing (owner's): the baked **SHA3 checkpoint (~h3.1M) is a compact
commitment to the entire fold output**. The production fold asserts terminal
`SHA3==checkpoint`, so that one hash certifies the whole 3M-block fold. Therefore we can
**build state fast and skip redundant re-verification below the checkpoint** — the hash
catches any divergence at the end.

Stacked levers (each gated on a real measurement + the terminal SHA3 gate):

1. **Skip redundant verification below the checkpoint.** `-mint-anchor-fast` already skips
   Groth16(`pv`)+ECDSA(`sv`) and defaults `ZCL_FOLD_INRAM=1`. Missing piece: also skip
   **Equihash header validation (`vh`)** below the checkpoint (mirror the `mint_skip_crypto`
   gate). Low risk; lifts rate toward the structural ceiling.
2. **Break the structural ceiling (THE CRUX).** The ~50 blk/s wall is IO/pprev/tree-append,
   not crypto. Fixing it (index the pprev walk / batch IO / tune in-RAM flush) is what takes
   the fold from ~50 to 150+ blk/s → **~4h fold**. This is the piece worth real engineering.
3. **Cut the live node over** to the complete state once the fold reaches tip.
4. **Snapshot the complete state** so every future sync is the ~60s design target — the
   one-time cost, paid once.

## Execution (workflow of subagents — "break it into smaller problems")

**Workflow `fast-fold-ceiling`** (run `wf_1c1df811-ee0`, launched 2026-07-14): 3 self-contained
Opus agents, each in its own git worktree, attacking a distinct sub-problem —
`perf/fold-pprev-index` (ancestor-walk), `perf/fold-io-inram-batch` (IO/fsync),
`perf/fold-vh-skip-hoist` (skip Equihash below checkpoint + hoist per-block recompute).
Each profiles the real bottleneck fresh, implements its fix, builds (full LTO), measures
before/after blk/s on an isolated `-mint-anchor-fast` fold, and light-parity-checks
(coins/nullifier counts at a low height + HOW-not-WHAT argument). A synthesis agent picks
the fastest parity-plausible binary + integration/stacking plan + GO/NO-GO.

Prior lane already built: `eval/stacked-fold` (A1 proof pool + header-pool widening +
cross-height proof lookahead) — parity-sensitive, needs the SHA3 gate before use.

## ⚠️ IO IS THE CEILING, NOT CPU (2026-07-14 05:3x — measured)

When multiple FULL folds run, the box is **disk-IO-bound, not CPU-bound**: CPU idle ~77% while
processes sit in `folio_wait_bit_common` (page-fault waits on the sqlite/coins mmap). The floor
drops from ~25 to ~16 blk/s under IO contention. Implications:
- **Biggest lever = `ZCL_FOLD_INRAM=1`** on the FULL `-mint-anchor` fold. FULL defaults it OFF
  (disk-per-batch); `-fast` defaults it ON. Setting it explicitly keeps FULL validation
  (installable bundle) but folds coins in RAM → minimal disk IO → should break the IO ceiling.
  `ZCL_FOLD_INRAM_FLUSH_EVERY=100000` bounds RAM + keeps resumability. Applied to
  `zclassic23-fastfold.service`.
- **Don't run many concurrent folds** — they thrash the disk and slow each other. Consolidate
  onto the single fastest fold once measured; the workflow's 7 scratch measurement folds also
  drain IO.
- The `perf/fold-io-inram-batch` workflow lane targets exactly this — its fix code is the most
  relevant to harvest.

## Durability (linger services — "so you can't lose it")

- **Floor:** `zclassic23-mint-receipt.service` (systemd --user linger) keeps folding the
  full-validation cure no matter what — survives session drop + reboot. NEVER kill it
  without a faster replacement ready.
- **Fast fold:** when the workflow yields a parity-clean fast binary, install it as a NEW
  linger service (e.g. `zclassic23-fastfold.service`) on a FRESH datadir
  (`--importblockindex $HOME/.zclassic <dd>/node.db` then the mint unit). Do NOT reuse the
  receipt datadir. NEVER touch `~/.zclassic-c23-anchor-mint` / `-mint-fast` datadirs.
- **Hard parity gate:** the production fold's terminal `SHA3==checkpoint` assert is the real
  guarantee. If it FATALs on mismatch → the fast binary broke parity → revert to receipt.

## Autonomy bar

Owner granted "keep going until fully synced, don't wait on me." Proceed without pause;
the cutover is authorized once copy-proof green + revert exists. Surface ONLY a genuine
**borrowed-trust** fork (skipping verification of the ACTIVE chain, or trusting external
state). Self-checkpoint trust below the baked h3.1M checkpoint is within the sovereign-cure
design and needs no approval.

## Current state / next actions (keep updated)

- [SOLVED] **Cutover mechanism already built** — see `## Cutover procedure` below. A producer
  that folds to the anchor (h=3,056,758) emits `consensus-state-bundle-3056758.sqlite`;
  `-install-consensus-bundle=PATH` installs it (shielded activation cursor → 0, clears the
  wedge). Self-checkpoint trust, no owner approval needed. **Anchor-only is sufficient** (the
  install resets the reducer to the anchor with complete state, then normal boot re-folds to
  tip from local bodies). So the SOLE critical path is: get a producer to the anchor fast.
- [PRECONDITION found — legacy protected-v1 producer] Export only fires from a
  **COMMITTED build with an active source-receipt session** (the protected v1
  binary requires its exact Git commit), and the binary must
  NOT be swapped between fold-start and anchor. ⇒ the fast-fold winner must be COMMITTED (merged
  to a branch/main) and the fold run on that committed build, not a raw worktree binary.
- [LINCHPIN RESOLVED — decisive] agent `abe76e5b`: **a `-mint-anchor-FAST` bundle CANNOT cure
  the live node.** (1) fast-mint skips the exporter (`boot_mint_anchor.c:771` gates export on
  `!mint_skip_crypto_get()`) → no bundle. (2) The `-install-consensus-bundle` flag always runs
  the publication CAS (`boot_install_consensus_bundle.c:193/211`), which REFUSES any
  `validation_profile != FULL` (`consensus_state_publication_cas.c:341-348`). Completeness gates
  DO accept CHECKPOINT_FOLD, but the profile gate does not.
  ⇒ **STRATEGY CORRECTION: the cure fold MUST be FULL `-mint-anchor` (crypto ON). Speed it by
  PARALLELIZING/PIPELINING crypto (A1 proof pool + `eval/stacked-fold` header pool + cross-height
  lookahead — all preserve FULL profile) and fixing the structural ceiling (pprev/IO). Do NOT use
  `-mint-anchor-fast` or the vh-SKIP lane for the production fold — skipping crypto stamps
  CHECKPOINT_FOLD and forecloses BOTH export and install.**
  ⇒ **The protected v1 binary MUST be its exact committed build** (legacy
  40-hex Git claim; current v2 writers instead require the baked SHA-256 source
  identity),
  run end-to-end so the receipt opens at fold-start (`boot_mint_anchor.c:505`) and finalizes at
  the anchor (`:743`). No binary swap mid-fold.
- [running] Workflow `wf_1c1df811-ee0` — fast-fold binary (3 lanes + synth). Notifies on completion.
- [running, EXPORT-READY ✓] `zclassic23-mint-receipt.service` — durable floor FULL-validation fold.
  Journal confirms "[mint-anchor] durable source receipt session opened (profile=full)" at fold
  start ⇒ committed build + FULL receipt session ⇒ it WILL export an installable bundle at the
  anchor. This is the guaranteed (slow) cure. ~h384k+; rate dips to ~15 blk/s under workflow
  contention (temporary; won't finish in 7h either way). The winning fast binary must likewise be
  a committed FULL build that opens a receipt session (verify the same journal line on it).
## ⚠️⚠️ HONEST OUTCOME (2026-07-14 06:0x) — full sync will NOT complete in the 7h window

Every speed lever was tried and hit a hard wall:
- **`-mint-anchor-fast` (in-RAM, fast)** → CHECKPOINT_FOLD → **can't export/install** (dead end).
- **FULL `-mint-anchor` + `ZCL_FOLD_INRAM=1`** → was **FATAL by design** at the time of writing
  (script_validate resolved prevouts from durable coins_kv only, so an active overlay wedged the
  fold). **CURED (in-RAM serial-pipeline fold):** the offline `-mint-anchor` drive is single-
  threaded, so it now brackets the whole drive with `coins_ram_mint_drive_enter/exit` and
  `coins_kv_overlay_safe()` admits overlay READS on that drive thread — script_validate resolves
  recent-coin prevouts straight from the un-flushed overlay. FULL `-mint-anchor` now DEFAULTS to
  the in-RAM overlay (opt out `ZCL_FOLD_INRAM=0`); the terminal SHA3/count hard-assert is unchanged.
  This is the write-behind/IO-volume lever the "one real long-term lever" note below called for.
- **Parallel crypto (stacked-fold)** → helps little because the ceiling is **disk IO**, not crypto.
- **Workflow `fast-fold-ceiling`** → busted (~1.5h, no committed fixes; killed; it + its 7 scratch
  folds were thrashing IO and starving the real fold).

**Bottom line:** the cure MUST be a FULL fold; a FULL fold is **IO-bound at ~15–25 blk/s and cannot
use in-RAM** → **~28h+ to the anchor.** No producer is near the anchor (best is the floor at h518k).
So the node **cannot be fully synced in the remaining window** — that's physics, not a missing trick.

**What IS true and good:** the floor (`mint-receipt`, FULL, receipt session open) is the guaranteed
cure and folds toward the anchor; the cutover is proven-ready + documented (minutes once the bundle
lands). **The one real long-term lever** (future work, genuine engineering — NOT a flag): an
**IO-batching fix for the FULL fold** — defer/batch the per-batch disk COMMITS (fewer fdatasync,
larger write batches) while KEEPING on-disk prevout reads for script_validate. That preserves FULL
profile + installability but cuts the disk-IO ceiling. The `perf/fold-io-inram-batch` idea was
right; it just needs to batch WRITES, not move coins fully to RAM. Do this properly in a focused
session, copy-prove parity via the terminal SHA3 checkpoint.

- [DISABLED] `zclassic23-fastfold.service` — FULL+in-RAM is invalid (see above). Unit left in place
  but disabled; datadir `~/.zclassic-c23-fastfold` can be reused for the io-batch experiment.
- [then] Whichever FULL producer reaches the anchor first exports
  `consensus-state-bundle-3056758.sqlite` → copy-prove install on a live-copy (H* climbs past
  3,176,325) → cutover live node → snapshot for future ~60s sync.
- [fallback] If workflow NO_GO: receipt producer grinds ~29h to anchor, then cutover.
- [DONE 2026-07-14 — owner authorized] Stopped all 4 dead-end folds: `anchor-mint` +
  `mint-fast-v2` (both `-mint-anchor-FAST` → CHECKPOINT_FOLD → can't export) via
  `systemctl --user stop`, and orphan scratch folds `mint-A` (h122k) + `mint-B` (h140k) (FULL
  but behind the floor + NO receipt session → can't export) via kill. Load 36→18, CPU idle
  ~77%. Datadirs preserved for anchor-mint/mint-fast (reversible). The floor + workflow now
  have the full box.
- [STRATEGY] Real production binary = FULL `-mint-anchor` (crypto ON, so it exports) +
  PARALLEL/PIPELINED crypto (`eval/stacked-fold`: A1 proof pool + header pool + cross-height
  lookahead — keeps FULL profile) + structural fixes (`perf/fold-pprev-index`,
  `perf/fold-io-inram-batch`), all merged to a COMMITTED branch + built + run end-to-end. The
  `perf/fold-vh-skip-hoist` lane is NOT usable (skipping crypto → CHECKPOINT_FOLD). Physics:
  crypto (~74ms/blk) fully pipelined across 16 cores → ~4.6ms/blk → the serial apply path
  (~2-5ms if pprev/IO fixed) governs → target ~150-200 blk/s → ~4-6h fold. The production fold's
  live telemetry IS the measurement (crypto-heavy region starts ~h276k).

Cross-refs: memory `project_session_2026-07-14_cleanup_and_sync_state`,
`project_autonomy_grant_synced_2026-07-14`, `reference_refold_bottleneck_measured_2026-06-24`,
`docs/HANDOFF.md`.

---

## Cutover procedure

> **Purpose.** Once a producer datadir (e.g. `~/.zclassic-c23-mint-receipt`) has
> folded **complete** transparent+shielded state to the anchor, make the live
> daily-driver `~/.zclassic-c23` adopt that state and resume syncing to tip —
> without re-introducing a coins-only / incomplete-shielded gap. **The full
> mechanism is already built and wired.** This section is the operator runbook.

### The mechanism (already in the tree — do NOT rebuild it)

The cutover is a producer **export** + a consumer **install-activate**, both
purpose-built for exactly this cure. Nothing here is new engineering; the drive
only runs the two verbs and copy-proves.

- **Producer export — `boot_mint_anchor_export_bundle`**
  (`config/src/boot_mint_anchor.c:360`). When a `-mint-anchor`/`-mint-anchor-fast`
  producer folds genesis → the baked SHA3 checkpoint height (the **anchor**,
  `h=3,056,758`, `lib/chain/src/checkpoints.c:86`), it quiesces the in-RAM
  overlay and emits **`consensus-state-bundle-3056758.sqlite`** into its own
  datadir. The bundle is an immutable SQLite file holding `bundle_meta`, `coins`,
  `anchors`, `nullifiers`, the 8 reducer stage cursors, and a **producer source
  receipt** binding `running_binary_digest == SHA3(/proc/self/exe)` — so only the
  exact folding binary can produce it (`config/src/consensus_state_snapshot_export.c:645`,
  contract `config/include/config/consensus_state_snapshot_export.h`). The bundle
  is complete-history: `history_complete=1`, `activation_boundary=0`, all source
  cursors `0`, `source_fold_cursor = height+1`.

- **Consumer install — `-install-consensus-bundle=PATH`**
  (`config/src/boot_install_consensus_bundle.c:110`, argv `src/main.c:3403`,
  contract `config/include/config/boot.h:355`). TERMINAL (`_exit`s). Steps, all
  fail-closed: (1) **containment** — refuses the canonical `~/.zclassic-c23`
  unless `ZCL_DEPLOY_ALLOW_CANONICAL=1` is set (dev/copy datadirs proceed
  freely); (2) **admit + strictly validate** the immutable bundle (recomputes the
  UTXO root/count, verifies every anchor tree→root and the nullifier digest);
  (3) **publication CAS** — the bundle's `height/hash` must sit on *this* node's
  own validated header chain (selected-chain binding), and the artifact +
  producer-source receipts must all be present and mutually binding, else REFUSE;
  (4) on ADMIT, atomically install via `consensus_state_snapshot_install_activate`
  (`config/src/consensus_state_snapshot_install_activate.c:318`).

- **The atomic install** refuses any non-complete bundle up front
  (`:357` — `history_complete`, `activation_boundary==0`, all source cursors `0`,
  `source_fold_cursor==height+1`; a coins-only or mixed-provenance bundle can
  never install and reinstate the gap). It captures a **physically restorable
  prior generation** via capability-relative `VACUUM INTO` from the already-
  open singleton under the process transaction lock (path returned in
  `result->prior_generation_path`). It immediately takes `BEGIN IMMEDIATE` and
  requires an unchanged SQLite data-version, total-change counter, and file
  identity before any cutover write; then it independently reopens/quick-checks
  the sidecar-free image and fsyncs the file and directory. It then
  installs coins + anchors + nullifiers + the 8 stage cursors in **ONE
  `BEGIN IMMEDIATE` transaction**, rechecks the durable ADMIT's exact H*/hash,
  clears stale replay/backfill/refold generation markers, re-verifies every
  destination commitment, seeds `tip_finalize`, and requires exact
  `H*=served=height` plus `coins_applied_height=height+1` before `COMMIT`.
  **Shielded activation cursor = 0.** A commit error with an unprovable outcome
  is named `COMMIT_OUTCOME_UNKNOWN`; it never falsely claims rollback.

### Why this clears the wedge (the gap-close, cited)

The blocker `utxo_apply.anchor_backfill_gap` is set/cleared by
`utxo_apply_anchor_gap_blocker_refresh` (`app/jobs/src/utxo_apply_anchors.c:264`):
it **clears** iff, for *both* Sprout and Sapling pools, the activation cursor is
found **and `== 0`** (`:280-288`). The install writes complete genesis→anchor
anchor tables with **activation cursor 0**, so the blocker clears. As the reducer
then folds `3,056,759` forward, `fold_sapling`/`fold_sprout`
(`app/jobs/src/utxo_apply_anchors.c:117,149`) find a real latest frontier via
`anchor_kv_latest_tree` (no more `HISTORY_INCOMPLETE`), append the block's
commitments, verify `hashFinalSaplingRoot` (`:177`), and advance — so the fold
sails past the old wedge at `3,176,326` instead of failing closed on an unknown
root. The live coins-only defect (empty `sapling_anchors` below the cursor +
positive activation cursor) is *baked into the on-disk state* from a one-time
seed; the current live unit passes only `-datadir` (no `-load-snapshot-at-own-height`),
so replacing the on-disk state with the complete bundle is a permanent fix, not
something re-applied each boot.

### Anchor vs tip — the decision (Q4)

**The producer only needs to reach the ANCHOR (`h=3,056,758`), not the live tip.**
The bundle is anchor-scoped *by construction* (the exporter's height is the
compiled checkpoint height, and the receipt proof is scoped to that checkpoint —
there is no "export at tip" verb). Install **resets the reducer DOWN to the
anchor with complete shielded state**, and the subsequent normal boot **re-folds
forward** from `H*=3,056,758` → old wedge (`3,176,325`) → network tip using the
local block bodies already on disk. Re-folding the ~120k anchor→wedge blocks
(plus wedge→tip) at the node's fold rate is the price; it is *forward progress
with complete state*, and it is both **faster and safer** than waiting for a
producer to fold all the way to tip (which the export path does not even
support). After the node reaches tip with complete state, snapshot it (plan
step 4) so future syncs pay the ~60 s design cost, not the re-fold.

### COPY-PROVE FIRST (non-negotiable — prove H\* climbs on a COPY)

Do this on a datadir **copy** and confirm the fix FIRES before touching live.

```bash
# 0. Preconditions: the bundle exists in the producer datadir.
PROD=~/.zclassic-c23-mint-receipt
BUNDLE=$PROD/consensus-state-bundle-3056758.sqlite
ls -l "$BUNDLE"    # must exist; if absent the producer has not reached the anchor yet (see BLOCKER)

# 1. Stop the live node and make a COPY of its datadir (never touch the original).
systemctl --user stop zclassic23
CPY=~/.zclassic-c23-cutover-copy
rm -rf "$CPY"; cp -a ~/.zclassic-c23 "$CPY"     # cp -a preserves the blk body files
# (vendor libs are already inside build/; no extra copy needed for the same binary)

# 2. Install the complete bundle into the COPY. Non-canonical datadir => no env gate.
#    TERMINAL: this prints "INSTALLED ... reboot normally" and exits, or a typed REFUSED.
build/bin/zclassic23 -datadir="$CPY" -install-consensus-bundle="$BUNDLE"
#    Expect: "INSTALLED: ... reboot normally; the reducer folds forward from H*=3056758 to tip."

# 3. Boot the COPY normally on an isolated port and let it re-fold forward.
build/bin/zclassic23 -datadir="$CPY" -port=8133 -rpcport=18133 &

# 4. SUCCESS ASSERTION — H* must CLIMB PAST 3,176,325 on the copy (gate on CLIMB,
#    not "booted w/o FATAL"). Sample reducer_frontier twice, minutes apart:
build/bin/zclassic23 -datadir="$CPY" -rpcport=18133 dumpstate reducer_frontier
#    PASS iff: (a) blocker utxo_apply.anchor_backfill_gap is CLEARED, AND
#              (b) H* > 3,176,325 and strictly increasing between two samples, AND
#              (c) coins_applied_height == H* + 1 (continuous coverage, no stamped span).
#    This is the G-SOV climb gate (docs/work/self-verified-tip-plan.md).
```

If any of (a)/(b)/(c) fails, STOP — the bundle or the install is wrong; do not
touch live. Tear down the copy (`rm -rf "$CPY"`) and re-diagnose.

### Live cutover (only after the copy proof is GREEN)

```bash
# The old live datadir is preserved as the belt-and-suspenders revert point.
mv ~/.zclassic-c23 ~/.zclassic-c23.pre-cutover        # keep the exact prior bytes
cp -a ~/.zclassic-c23.pre-cutover ~/.zclassic-c23     # install into a working copy

# Canonical datadir => the containment latch must be set explicitly (operational
# safety, not a trust gate — see classification below).
ZCL_DEPLOY_ALLOW_CANONICAL=1 \
  build/bin/zclassic23 -datadir=~/.zclassic-c23 -install-consensus-bundle="$BUNDLE"
#    Expect the "INSTALLED ... reboot normally" terminal. Note the printed
#    prior_generation_path (a VACUUM INTO image fenced to the immediately
#    following cutover transaction).

systemctl --user start zclassic23     # normal boot; reducer re-folds anchor -> tip
# Watch H* climb past 3,176,325 to network tip, gap blocker cleared, over the
# next few hours of forward fold.
```

### Revert (three layers, strongest first)

1. **In-transaction rollback (automatic).** The whole install is ONE
   `BEGIN IMMEDIATE`; any validation/insert/commit failure rolls back and leaves
   the live store byte-for-state identical (`consensus_state_snapshot_install_activate.c:420`).
   Nothing half-installs.
2. **Prior-generation swap (post-commit, surgical).** The install captured a
   `VACUUM INTO` image of the progress store, fenced by SQLite data-version and
   total-change checks to the immediately following transaction; swap it back:
   ```bash
   systemctl --user stop zclassic23
   cp -f <result.prior_generation_path> ~/.zclassic-c23/progress.kv   # prior logical generation
   systemctl --user start zclassic23
   ```
3. **Whole-datadir revert (belt-and-suspenders).** Restore the untouched
   pre-cutover datadir:
   ```bash
   systemctl --user stop zclassic23
   rm -rf ~/.zclassic-c23 && mv ~/.zclassic-c23.pre-cutover ~/.zclassic-c23
   systemctl --user start zclassic23     # back to the (wedged) prior state, intact
   ```

### Trust-fork classification — SELF-checkpoint trust, NOT borrowed

This cutover is **within the sovereign-cure design; it does NOT need owner
approval** (beyond the one-time `ZCL_DEPLOY_ALLOW_CANONICAL=1` operational latch).
The producer's state is **self-derived** (the node's own code folds it from
genesis) and terminally attested by `SHA3 == the baked checkpoint`; the install's
publication CAS additionally binds the bundle's `height/hash` to *this live
node's own validated header chain* and binds the producer receipt to the exact
folding binary — no peer-provided, network-borrowed, or `zclassicd`-minted state
is trusted anywhere in the path. This is precisely the "self-checkpoint trust
below the baked h3.1M checkpoint" the autonomy bar (this doc, "Autonomy bar";
`project_autonomy_grant_synced_2026-07-14`) authorizes *without* pause once
copy-proof is green + a revert exists — both provided above. Surface to the owner
ONLY if the CAS ever refuses on **selected-chain binding** (would mean the
bundle's anchor block is not on the live node's header chain — a genuine fork),
never for the routine install.

### BLOCKER (as of 2026-07-14)

**The bundle does not exist yet.** The mechanism is fully built and this runbook
is ready, but `consensus-state-bundle-3056758.sqlite` is only emitted when a
producer **reaches the anchor `h=3,056,758`**. The floor producer
(`zclassic23-mint-receipt`, `~/.zclassic-c23-mint-receipt`) was at `~h364k`
(2026-07-14) — far short of the anchor (ETA ~22 h+ at ~25 blk/s, per the fold
table above). **The cutover cannot run until the fold reaches the anchor and the
export succeeds.** Two secondary preconditions to verify before the export can
even fire: (i) the producer must have an **active source-receipt session**
(`consensus_state_producer_receipt_begin` runs at fold start; this protected
legacy-v1 producer requires its exact 40-hex commit — a current v2 producer
instead requires its baked SHA-256 source identity); (ii) the export runs in-process at
finalize by the *exact* folding binary, so the producer unit must not be swapped
to a different binary between fold and anchor. Accelerating the fold to the
anchor (the `fast-fold-ceiling` workflow above) is therefore the gating task; the
cutover itself is a ~minutes operation once the bundle lands.

## S1.3 — mint-fold progress.kv sync-off + candidate race (2026-07-14)

The FULL `-mint-anchor` fold never engaged the repo's own IBD durability mode:
`progress_store_set_sync_mode(true)` (`PRAGMA synchronous=OFF`) was only called
from the staged-sync supervisor tick, which never runs during the mint (stages
boot offline, no liveness contracts) — so the whole genesis→anchor fold paid an
fsync barrier on every stage-batch COMMIT. Landed fix (branch
`perf/mint-progress-sync-off`):

- `boot_mint_anchor_run` now flips progress.kv to `synchronous=OFF` for the
  fold's duration (default-on; opt out with `ZCL_MINT_PROGRESS_SYNC_OFF=0`) and
  restores `NORMAL` **then** `wal_checkpoint(TRUNCATE)` before the snapshot /
  receipt-finalize / bundle export (and on the frontier-walled path), so every
  artifact derives from a durable db. Crash-ordering survives: bodies are
  fdatasync'd BEFORE each COMMIT by the pre-commit veto hook, so a lost COMMIT
  only leaves the cursor behind the bodies — resume re-folds the window; the
  terminal SHA3==checkpoint assert makes a wrong artifact impossible.
- `mint-progress.log` gains a `cm:<ewma>us` token (outer batch-COMMIT wall time,
  `stage_batch_commit_us_ewma()`): watching it collapse from ~ms to tens of µs
  is the direct proof the barrier is gone.

**Candidate race:** the floor producer (`zclassic23-mint-receipt`) is NEVER
touched — its receipt binds its exact binary. A candidate producer
(`zclassic23-iobatch.service`, datadir `~/.zclassic-c23-iobatch`, ports
39080-39083, `ZCL_REFOLD_DRAIN_BATCH=10000`, Nice=10 + best-effort ionice so the
floor wins IO arbitration) folds fresh from genesis on a clean committed build of
main (A1 parallel validation + this patch). Decision rule ~1h in: KEEP iff
candidate ≥1.5× the floor's own logged rate at the SAME height band AND the
floor's current rate stays within ~20% of its pre-start baseline; otherwise kill
the candidate (stop unit, rm datadir — fully disposable). First producer to emit
`consensus-state-bundle-3056758.sqlite` feeds the cutover runbook above.

### S1.3 race VERDICT (2026-07-14 16:0x UTC) — candidate KILLED, floor is the cure

Measured at the SAME height (h=19,999, both from genesis): floor 33.6 blk/s
(548s elapsed) vs candidate 26.0 blk/s (709s) = **0.77×** — nowhere near the
≥1.5× keep bar — while the floor degraded 34→16.5 blk/s (>50%, past the 20%
kill threshold). Killed per the pre-registered rule (unit disabled+removed,
datadir purged); floor recovered as the sole folder.

**What the cm: telemetry proved:** batch COMMITs cost 1.9–2.6 s even with
`synchronous=OFF` — the fsync barrier was NOT the dominant cost; **write
volume is** (WAL growth + checkpoint page-copying + page-cache writeback for
~10k-block batches). A1 parallel crypto also showed no pv win at equal
heights in the mint drive. Conclusions for future fold-speed work:
1. The S1.3 patch (sync-off + cm:) stays — harmless, and cm: is the
   diagnostic that settled this in minutes instead of hours.
2. The real lever is REDUCING WRITE VOLUME per fold (coin upsert batching /
   fewer rewrites), not removing fsyncs and not crypto parallelism.
3. Two concurrent FULL folds on one NVMe starve each other — never race
   producers on this box; A/B on copies at DIFFERENT times instead.
4. Boot-to-fold for a fresh producer datadir took ~36 min (2× quick_check +
   index load + legacy-seed copy-then-wipe) — a standing boots-fast defect.
